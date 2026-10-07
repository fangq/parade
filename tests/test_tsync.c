/* pd_tsync: plain text shared between replicas through a relay-ordered log. Each replica has a host text
   (UTF-8) it edits and the listener keeps in step; after every step a host's text is its shared text, and
   once every update is delivered all of them are the same. SYNC_SEED, SYNC_STEPS: the random part. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_tsync.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("CHECK failed at %d: %s\n", __LINE__, #c); } } while (0)

#define NREP 3

typedef struct {
    pd_tsync* t;
    char* text;         /* the host's */
    size_t len;
} rep;

typedef struct {
    unsigned char* p;
    size_t n;
    int from;
} msg;

static rep R[NREP];
static msg* LOG;
static int nlog, caplog, cursor[NREP];
static int sending = -1;    /* the replica whose sender is speaking */

/* the byte offset of a position in code units */
static int misaligned;
static size_t byte_at(const char* s, size_t n, uint32_t units) {
    size_t i = 0;
    uint32_t u = 0;

    while (i < n && u < units) {
        unsigned char c = (unsigned char)s[i];
        size_t k = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;

        u += c >= 0xF0 ? 2 : 1;
        i += k;
    }
    if (u != units && i < n) misaligned++;
    if (u > units && getenv("SYNC_TRACE")) printf("    position %u is inside a character (lands on %u)\n", units, u);

    return i;
}

static void on_send(void* user, const void* update, size_t len) {
    if (nlog == caplog) {
        caplog = caplog ? caplog * 2 : 256;
        LOG = (msg*)realloc(LOG, (size_t)caplog * sizeof(msg));
    }

    LOG[nlog].p = (unsigned char*)malloc(len ? len : 1);
    memcpy(LOG[nlog].p, update, len);
    LOG[nlog].n = len;
    LOG[nlog].from = (int)(intptr_t)user;
    nlog++;
}

static void on_change(void* user, uint32_t at, uint32_t removed, const char* ins, size_t inslen) {
    rep* r = &R[(intptr_t)user];
    if (getenv("SYNC_TRACE"))
        printf("  change %d: at %u remove %u insert %zu \"%.*s\" (strlen %zu) host %zu/%u units\n", (int)(intptr_t)user, at,
               removed, inslen, (int)inslen, ins, strlen(ins), r->len, pd_tsync_units(r->text, r->len));
    size_t a = byte_at(r->text, r->len, at), b = byte_at(r->text, r->len, at + removed);

    char* n = (char*)malloc(r->len - (b - a) + inslen + 1);   /* a new buffer: shrinking first would lose the tail */

    memcpy(n, r->text, a);
    memcpy(n + a, ins, inslen);
    memcpy(n + a + inslen, r->text + b, r->len - b + 1);
    free(r->text);
    r->text = n;
    r->len = r->len - (b - a) + inslen;
}

/* what a host does when the user types: its own text, then pd_tsync told */
static void host_insert(int k, uint32_t at, const char* s) {
    on_change((void*)(intptr_t)k, at, 0, s, strlen(s));
    CHECK(pd_tsync_insert(R[k].t, at, s, strlen(s)) == PD_OK);
}

static void host_delete(int k, uint32_t at, uint32_t n) {
    on_change((void*)(intptr_t)k, at, n, "", 0);
    CHECK(pd_tsync_delete(R[k].t, at, n) == PD_OK);
}

static const char* last_op = "";
static void in_step(int k) {
    char* s = pd_tsync_text(R[k].t);

    if (strcmp(s, R[k].text) != 0 || pd_tsync_units(R[k].text, R[k].len) != pd_tsync_length(R[k].t)) {
        failures++;
        printf("replica %d after %s: host \"%s\" (%u) but shared \"%s\" (%u)\n", k, last_op, R[k].text, pd_tsync_units(R[k].text, R[k].len), s, pd_tsync_length(R[k].t));
        if (getenv("SYNC_STOP")) exit(1);
    }
    checks++;
    pd_tsync_free_data(s);
}

static int deliver_one(int to) {
    while (cursor[to] < nlog) {
        msg* m = &LOG[cursor[to]++];

        if (m->from != to) {
            CHECK(pd_tsync_receive(R[to].t, m->p, m->n) == PD_OK);
            in_step(to);
            return 1;
        }
    }

    return 0;
}

static void deliver_all(void) {
    int k;

    for (k = 0; k < NREP; k++) {
        while (deliver_one(k)) {
        }
    }
}

static void setup(void) {
    int k;

    for (k = 0; k < NREP; k++) {
        CHECK(pd_tsync_new(1000 + k, &R[k].t) == PD_OK);
        pd_tsync_set_sender(R[k].t, on_send, (void*)(intptr_t)k);
        pd_tsync_set_listener(R[k].t, on_change, (void*)(intptr_t)k);
        R[k].text = (char*)calloc(1, 1);
        R[k].len = 0;
    }
}

static int same_all(void) {
    int k;

    for (k = 1; k < NREP; k++) {
        if (strcmp(R[0].text, R[k].text) != 0) {
            return 0;
        }
    }

    return 1;
}

static unsigned rnd_state = 1;
static unsigned rnd(void) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 16) & 0x7fff;
}

int main(void) {
    int k, i, steps;
    const char* seed = getenv("SYNC_SEED");
    const char* words[] = {"a", "bc", "def ", "\n", "x\ny", "\xc3\xa9", "\xe4\xb8\xad\xe6\x96\x87",
                           "\xf0\x9f\x98\x80", "line\n", "  "};

    setup();

    /* a sharer's text; a joiner gets it as edits */
    on_change((void*)0, 0, 0, "Hello world", 11);
    CHECK(pd_tsync_publish(R[0].t, "Hello world", 11) == PD_OK);
    CHECK(pd_tsync_can_undo(R[0].t) == 0);
    deliver_all();
    CHECK(same_all() && strcmp(R[1].text, "Hello world") == 0);

    /* at once: one inserts where the other deletes before it */
    host_insert(0, 6, "big ");
    host_delete(1, 0, 6);
    deliver_all();
    CHECK(same_all());
    CHECK(strcmp(R[2].text, "big world") == 0);

    /* characters of several bytes, and past the BMP */
    host_insert(2, 0, "\xc3\xa9\xf0\x9f\x98\x80 ");     /* e-acute, a smiley, a space: 4 units */
    deliver_all();
    CHECK(pd_tsync_length(R[0].t) == 13 && pd_tsync_units(R[0].text, R[0].len) == 13);
    host_delete(0, 1, 2);      /* the smiley: two units */
    deliver_all();
    CHECK(same_all() && strcmp(R[1].text, "\xc3\xa9 big world") == 0);

    /* undo takes back one's own typing only, and tells the host */
    pd_tsync_seal(R[1].t);      /* a new step: the caret moved since the last edit */
    host_insert(1, 0, "Bob: ");
    host_insert(0, pd_tsync_length(R[0].t), "!");
    deliver_all();
    CHECK(pd_tsync_undo(R[1].t) == PD_OK);
    in_step(1);
    deliver_all();
    CHECK(same_all() && strcmp(R[2].text, "\xc3\xa9 big world!") == 0);
    CHECK(pd_tsync_redo(R[1].t) == PD_OK);
    deliver_all();
    CHECK(same_all() && strncmp(R[0].text, "Bob: ", 5) == 0);
    CHECK(pd_tsync_insert(R[0].t, 999, "x", 1) == PD_ERR_ARG && pd_tsync_delete(R[0].t, 0, 9999) == PD_ERR_ARG);
    CHECK(pd_tsync_receive(R[0].t, "\xff\xfe junk", 7) != PD_OK);

    /* random: edits, undo and redo on all three, updates delivered in the log's order at random times */
    rnd_state = seed ? (unsigned)atoi(seed) : 7;
    steps = getenv("SYNC_STEPS") ? atoi(getenv("SYNC_STEPS")) : 3000;

    for (i = 0; i < steps; i++) {
        int r = rnd() % 100;
        uint32_t n;

        k = rnd() % NREP;
        if (getenv("SYNC_SOLO")) { k = 0; if (r >= 73) r = 10; }
        n = pd_tsync_length(R[k].t);

        if (r < 45) {
            uint32_t at;
            size_t b;

            /* at a character boundary of the host's text */
            b = byte_at(R[k].text, R[k].len, n ? rnd() % (n + 1) : 0);
            at = pd_tsync_units(R[k].text, b);
            last_op = "insert";
            { int w = rnd() % 10; if (getenv("SYNC_NOWIDE") && w == 7) w = 0; host_insert(k, at, words[w]); }
        } else if (r < 65 && n > 0) {
            size_t a = byte_at(R[k].text, R[k].len, rnd() % n), b;
            uint32_t ua = pd_tsync_units(R[k].text, a), ub;

            b = byte_at(R[k].text, R[k].len, ua + 1 + rnd() % 6);
            ub = pd_tsync_units(R[k].text, b);
            last_op = "delete";
            host_delete(k, ua, ub - ua);
        } else if (r < 70 && !getenv("SYNC_NOUNDO")) {
            last_op = "undo";
            pd_tsync_undo(R[k].t);
        } else if (r < 73 && !getenv("SYNC_NOUNDO")) {
            last_op = "redo";
            pd_tsync_redo(R[k].t);
        } else {
            last_op = "receive";
            deliver_one(rnd() % NREP);
        }

        in_step(k);
    }

    deliver_all();
    CHECK(same_all());
    printf("  %d random steps; the texts agree (%u units, %d updates)\n", steps, pd_tsync_length(R[0].t), nlog);

    for (k = 0; k < NREP; k++) {
        pd_tsync_free(R[k].t);
        free(R[k].text);
    }

    for (i = 0; i < nlog; i++) {
        free(LOG[i].p);
    }

    free(LOG);
    (void)sending;
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
