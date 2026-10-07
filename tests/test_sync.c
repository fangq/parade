/*
 * Parade collaboration tests (make SYNC=yrs): replicas of one document,
 * edited at the same time, their updates read late and read again through
 * a relay, must end up the same -- and each replica's document must be its
 * shared state after every edit and every update.
 *
 * Debugging: SYNC_SEED=n another random run, SYNC_EVERY=n compare every n
 * edits (and stop at the first divergence, naming the edits since), SYNC_STEP
 * deliver and compare after every edit, SYNC_REC=file record what replica 0
 * receives (a base state, then the updates), SYNC_FAST skip the per-step
 * self-check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_doc.h"
#include "parade_sync.h"

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t rnd_state = 2024;
static void seed_from_env(void) { if (getenv("SYNC_SEED")) rnd_state = (uint32_t)strtoul(getenv("SYNC_SEED"), NULL, 10); }

static uint32_t rnd(uint32_t n) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return n ? (rnd_state >> 8) % n : 0;
}

/* ------------------------------------------------------------------ */
/* a relay: everyone's updates in one log, read by each at its own pace */
/* ------------------------------------------------------------------ */

/*
 * As with a server (y-websocket, a Postgres log with LISTEN/NOTIFY, a
 * CouchDB changes feed): every update goes into one log in the order it
 * came in, and each replica reads the log in that order, whenever it gets
 * to it -- some lagging far behind -- and now and then reads a stretch
 * again, as after a reconnect. What one replica made after seeing
 * another's update is in the log after that update, so nobody ever gets an
 * update before one it depends on. (yrs 0.28 loses updates that arrive
 * before ones they depend on, which a peer-to-peer mesh could do.)
 */

#define NREP 3

typedef struct {
    unsigned char* p;
    size_t n;
    int from;
} msg;

typedef struct {
    pd_doc* d;
    pd_sync* s;
    int self;
} replica;

static replica R[NREP];
static msg* LOG;
static int nlog_, caplog;
static int cursor[NREP];        /* how far each replica has read */
static size_t sent_bytes;
static int sent_msgs;
static FILE* rec;               /* SYNC_REC: what replica 0 receives, for replaying with yrs alone */

static void record(int to, const void* p, size_t n) {
    uint32_t len = (uint32_t)n;

    if (rec && to == 0) {
        fwrite(&len, 4, 1, rec);
        fwrite(p, 1, n, rec);
        fflush(rec);
    }
}

static void on_send(void* user, const void* update, size_t len) {
    replica* r = (replica*)user;

    sent_bytes += len;
    sent_msgs++;

    if (nlog_ == caplog) {
        caplog = caplog ? caplog * 2 : 256;
        LOG = (msg*)realloc(LOG, (size_t)caplog * sizeof(msg));
    }

    LOG[nlog_].p = (unsigned char*)malloc(len ? len : 1);
    memcpy(LOG[nlog_].p, update, len);
    LOG[nlog_].n = len;
    LOG[nlog_].from = r->self;
    nlog_++;
}

/* a replica's document is its shared state, after every edit and every update (debugging) */
static void self_check(int k, const char* what) {
    char* a, *b;

    if (getenv("SYNC_FAST")) {
        return;
    }

    a = pd_sync_dump(R[k].s, 0);
    b = pd_sync_dump(R[k].s, 1);

    if (strcmp(a, b) != 0) {
        fprintf(stderr, "replica %d: document is not its shared state after %s\n--- doc\n%s\n--- shared\n%s\n", k, what, a, b);
        exit(1);
    }

    pd_sync_free_data(a);
    pd_sync_free_data(b);
}

/* the next update in the log for a replica (its own are skipped); 0 when it has read everything */
static int deliver_one(int to) {
    while (cursor[to] < nlog_) {
        msg* m = &LOG[cursor[to]++];

        if (m->from != to) {
            record(to, m->p, m->n);
            CHECK(pd_sync_receive(R[to].s, m->p, m->n) == PD_OK);
            self_check(to, "an update");
            return 1;
        }
    }

    return 0;
}

static void deliver_all(void) {
    int k, any = 1;

    while (any) {
        any = 0;

        for (k = 0; k < NREP; k++) {
            while (deliver_one(k)) {
                any = 1;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* editing                                                            */
/* ------------------------------------------------------------------ */

static pd_pos at(pd_block_id b, uint32_t off) {
    pd_pos p;

    p.block = b;
    p.offset = off;
    return p;
}

/* the paragraphs in reading order */
static int paras(pd_doc* d, pd_block_id* out, int cap) {
    int n = 0;
    pd_block_id p;

    for (p = pd_doc_next_paragraph(d, 0); p && n < cap; p = pd_doc_next_paragraph(d, p)) {
        out[n++] = p;
    }

    return n;
}

static uint32_t text_len(pd_doc* d, pd_block_id p) {
    const char* t;
    uint32_t n = 0;

    pd_doc_para_text(d, p, &t, &n);
    return n;
}

/* a random offset on a character boundary, never inside an object */
static uint32_t boundary(pd_doc* d, pd_block_id p, uint32_t off) {
    const char* t;
    uint32_t n = 0;

    pd_doc_para_text(d, p, &t, &n);

    if (off > n) {
        off = n;
    }

    while (off > 0 && off < n && ((unsigned char)t[off] & 0xC0) == 0x80) {
        off--;
    }

    return off;
}

static const char* const words[] = { "alpha ", "beta ", "gamma ", "\xC3\xA9t\xC3\xA9 ", "\xE5\xA4\xA9 ", "delta",
                                      "x", "Hello world. ", "\xF0\x9F\x98\x80"
                                    };

static int random_edit(int rep, pd_res_id res) {
    pd_doc* d = R[rep].d;
    pd_block_id P[512];
    int n = paras(d, P, 512), k = (int)rnd(30);
    pd_block_id p = n ? P[rnd((uint32_t)n)] : 0;
    uint32_t len = p ? text_len(d, p) : 0, a, b;
    pd_range r;
    pd_char_props cp;
    pd_block_info bi;

    if (!p) {
        return -1;
    }

    a = boundary(d, p, rnd(len + 1));
    b = boundary(d, p, a + rnd(12));

    if (b < a) {
        b = a;
    }

    r.start = at(p, a);
    r.end = at(p, b);

    switch (k) {
        case 0: case 1: case 2: case 3: {   /* typing */
            const char* w = words[rnd(sizeof(words) / sizeof(words[0]))];

            pd_doc_insert_text(d, at(p, a), w, strlen(w), PD_FORMAT_INHERIT, NULL);
            break;
        }

        case 4: case 5:
            pd_doc_delete(d, r, NULL);
            break;

        case 6:
            pd_doc_split(d, at(p, a), NULL);
            break;

        case 7: {   /* join with the next paragraph, when they share a parent */
            pd_block_id q = pd_doc_next_paragraph(d, p);
            pd_block_info qi;

            if (q && pd_doc_block_info(d, p, &bi) == PD_OK && pd_doc_block_info(d, q, &qi) == PD_OK &&
                    bi.parent == qi.parent) {
                r.start = at(p, len);
                r.end = at(q, 0);
                pd_doc_delete(d, r, NULL);
            }

            break;
        }

        case 8: case 9: case 10:
            memset(&cp, 0, sizeof(cp));

            if (k == 8) {
                cp.mask = PD_CP_WEIGHT;
                cp.weight = rnd(2) ? 700 : 400;
            } else if (k == 9) {
                cp.mask = PD_CP_ITALIC;
                cp.italic = (int32_t)rnd(2);
            } else {
                cp.mask = PD_CP_COLOR;
                cp.color = 0xFF000000u | rnd(0xFFFFFF);
            }

            pd_doc_set_char_props(d, r, &cp);
            break;

        case 11:
            pd_doc_set_para_style(d, p, pd_doc_style_find(d, rnd(2) ? "Heading 1" : "Normal"));
            break;

        case 12: {  /* a new paragraph after this one */
            pd_block_id np;

            if (pd_doc_block_info(d, p, &bi) == PD_OK &&
                    pd_doc_insert_block(d, bi.parent, bi.index + 1, PD_BLOCK_PARAGRAPH, &np) == PD_OK) {
                pd_doc_insert_text(d, at(np, 0), "New paragraph.", 14, PD_FORMAT_INHERIT, NULL);
            }

            break;
        }

        case 13:    /* a paragraph goes, when it is not alone */
            if (pd_doc_block_info(d, p, &bi) == PD_OK) {
                pd_block_info pi;

                if (pd_doc_block_info(d, bi.parent, &pi) == PD_OK && pi.child_count > 1) {
                    pd_doc_remove_block(d, p);
                }
            }

            break;

        case 14:    /* moved to the front of its parent */
            if (pd_doc_block_info(d, p, &bi) == PD_OK && bi.index > 0) {
                pd_doc_move_block(d, p, bi.parent, 0);
            }

            break;

        case 15: {  /* a picture */
            pd_inline o;

            memset(&o, 0, sizeof(o));
            o.kind = PD_INLINE_IMAGE;
            o.resource = res;
            o.width = 36 * 65536;
            o.height = 24 * 65536;
            pd_doc_insert_inline(d, at(p, a), &o, NULL);
            break;
        }

        case 16:    /* tracked typing */
            pd_doc_set_tracking(d, "Tracker");
            pd_doc_insert_text(d, at(p, a), "tracked ", 8, PD_FORMAT_INHERIT, NULL);
            pd_doc_delete(d, r, NULL);
            pd_doc_set_tracking(d, NULL);
            break;

        case 17:
            pd_doc_undo(d);
            break;

        case 18:
            pd_doc_set_role(d, p, rnd(2) ? PD_ROLE_HEADING : PD_ROLE_BODY, 1);
            break;

        case 19: {  /* into a list (a new one, now and then) */
            pd_list_level lv;
            pd_list_id l = (pd_list_id)pd_doc_list_count(d);

            if (!l || rnd(4) == 0) {
                memset(&lv, 0, sizeof(lv));
                lv.format = rnd(2) ? PD_NUM_DECIMAL : PD_NUM_BULLET;
                lv.start = 1;
                strcpy(lv.text, "%1.");
                lv.indent = 18 * 65536;
                pd_doc_list_define(d, 1, &lv, &l);
            }

            pd_doc_set_list(d, p, rnd(3) ? l : 0, 0);
            break;
        }

        case 20: {  /* a style redefined */
            pd_para_props pp;
            pd_char_props sc;

            memset(&pp, 0, sizeof(pp));
            memset(&sc, 0, sizeof(sc));
            pp.mask = PD_PP_SPACE_BEFORE;
            pp.space_before = (pd_sp)(rnd(24) * 65536);
            sc.mask = PD_CP_SIZE | PD_CP_WEIGHT;
            sc.size = (pd_sp)((12 + rnd(12)) * 65536);
            sc.weight = 700;
            pd_doc_style_define(d, "Heading 1", PD_STYLE_PARAGRAPH, pd_doc_style_find(d, "Normal"), &pp, &sc, NULL);
            break;
        }

        case 21: {  /* a table after this paragraph, with text in its cell */
            pd_block_id tb, row, cell, cp0;

            if (pd_doc_block_info(d, p, &bi) == PD_OK &&
                    pd_doc_insert_block(d, bi.parent, bi.index + 1, PD_BLOCK_TABLE, &tb) == PD_OK &&
                    (row = pd_doc_child(d, tb, 0)) != 0 && (cell = pd_doc_child(d, row, 0)) != 0 &&
                    (cp0 = pd_doc_child(d, cell, 0)) != 0) {
                pd_doc_insert_text(d, at(cp0, 0), "cell", 4, PD_FORMAT_INHERIT, NULL);
            }

            break;
        }

        case 22: {  /* the section's margins */
            pd_block_id sec = pd_doc_child(d, pd_doc_root(d), 0);
            pd_section_props sp;

            if (pd_doc_section_props(d, sec, &sp) == PD_OK) {
                sp.margin_left = (pd_sp)((36 + rnd(72)) * 65536);
                pd_doc_set_section_props(d, sec, &sp);
            }

            break;
        }

        case 24: case 25:   /* this replica's own last edit undone, or redone */
            if (k == 24) {
                pd_sync_undo(R[rep].s);
            } else {
                pd_sync_redo(R[rep].s);
            }

            break;

        case 26: {  /* a comment, or a reply to one */
            pd_comment c;
            pd_comment_id id;
            int32_t nc = pd_doc_comment_count(d);

            memset(&c, 0, sizeof(c));
            snprintf(c.author, sizeof(c.author), "Reviewer %d", rep);
            c.text = rnd(2) ? "Please check." : "Agreed \xE2\x9C\x93";
            c.text_len = (uint32_t)strlen(c.text);
            c.range = r;
            c.parent = nc > 0 && rnd(2) ? (pd_comment_id)(1 + rnd((uint32_t)nc)) : 0;

            if (pd_doc_comment_add(d, &c, &id) != PD_OK && c.parent) {
                c.parent = 0;
                pd_doc_comment_add(d, &c, &id);
            }

            break;
        }

        case 27: {  /* a comment resolved, reopened or edited */
            int32_t nc = pd_doc_comment_count(d);
            pd_comment c;
            pd_comment_id id = nc > 0 ? (pd_comment_id)(1 + rnd((uint32_t)nc)) : 0;

            if (id && pd_doc_comment_get(d, id, &c) == PD_OK) {
                if (rnd(2)) {
                    c.resolved = !c.resolved;
                } else {
                    c.text = "Edited.";
                    c.text_len = 7;
                }

                pd_doc_comment_set(d, id, &c);
            }

            break;
        }

        case 28: {  /* a comment removed (its replies with it) */
            int32_t nc = pd_doc_comment_count(d);

            if (nc > 0) {
                pd_doc_comment_remove(d, (pd_comment_id)(1 + rnd((uint32_t)nc)));
            }

            break;
        }

        default: {  /* direct paragraph properties */
            pd_para_props pp;

            memset(&pp, 0, sizeof(pp));
            pp.mask = PD_PP_ALIGN | PD_PP_SPACE_AFTER;
            pp.align = (int32_t)rnd(4);
            pp.space_after = (pd_sp)(rnd(12) * 65536);
            pd_doc_set_para_props(d, p, &pp);
            break;
        }
    }

    return k;
}

/* ------------------------------------------------------------------ */
/* tests                                                              */
/* ------------------------------------------------------------------ */

static int same_everywhere(const char* what, int show) {
    char* d0 = pd_sync_dump(R[0].s, 0), *s0 = pd_sync_dump(R[0].s, 1);
    int k, ok = d0 && s0;

    for (k = 1; k < NREP && ok; k++) {
        char* dk = pd_sync_dump(R[k].s, 0), *sk = pd_sync_dump(R[k].s, 1);

        if (!dk || strcmp(d0, dk) != 0 || !sk || strcmp(s0, sk) != 0) {
            ok = 0;

            if (show) {
                void* v0, *vk;
                size_t n0, nk;

                pd_sync_state_vector(R[0].s, &v0, &n0);
                pd_sync_state_vector(R[k].s, &vk, &nk);
                fprintf(stderr, "state vectors %s\n", n0 == nk && !memcmp(v0, vk, n0) ? "equal" : "DIFFER");
                {
                    void* df;
                    size_t dn;

                    pd_sync_diff(R[0].s, vk, nk, &df, &dn);
                    fprintf(stderr, "replica 0 has %zu bytes replica %d lacks; ", dn, k);
                    pd_sync_free_data(df);
                    pd_sync_diff(R[k].s, v0, n0, &df, &dn);
                    fprintf(stderr, "replica %d has %zu bytes replica 0 lacks\n", k, dn);
                    pd_sync_free_data(df);
                }
                pd_sync_free_data(v0);
                pd_sync_free_data(vk);
                fprintf(stderr, "%s: replica %d differs\n--- 0 doc\n%s\n--- %d doc\n%s\n--- 0 shared\n%s\n--- %d shared\n%s\n",
                        what, k, d0, k, dk ? dk : "", s0, k, sk ? sk : "");
            }
        }

        pd_sync_free_data(dk);
        pd_sync_free_data(sk);
    }

    if (ok && strcmp(d0, s0) != 0) {
        ok = 0;

        if (show) {
            fprintf(stderr, "%s: the document is not the shared state\n--- doc\n%s\n--- shared\n%s\n", what, d0, s0);
        }
    }

    pd_sync_free_data(d0);
    pd_sync_free_data(s0);
    return ok;
}

static void setup(void) {
    int k;
    pd_res_id res;
    pd_pos c;
    pd_char_props cp;
    pd_block_id p;
    void* full;
    size_t n;


    for (k = 0; k < NREP; k++) {
        R[k].self = k;
        CHECK(pd_doc_new(&R[k].d) == PD_OK);
        CHECK(pd_sync_new(R[k].d, (uint64_t)(k + 1) * 1000, &R[k].s) == PD_OK);
        pd_sync_set_sender(R[k].s, on_send, &R[k]);
    }

    /* the first editor's document, made the shared one */
    p = pd_doc_next_paragraph(R[0].d, 0);
    pd_doc_insert_text(R[0].d, at(p, 0), "The quick brown fox.", 20, PD_FORMAT_INHERIT, &c);
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_WEIGHT;
    cp.weight = 700;
    pd_doc_set_char_props(R[0].d, (pd_range) { at(p, 4), at(p, 9) }, &cp);
    pd_doc_split(R[0].d, c, &c);
    pd_doc_insert_text(R[0].d, c, "Second paragraph.", 17, PD_FORMAT_INHERIT, NULL);
    pd_doc_add_resource(R[0].d, "image/png", "\x89PNG\r\n\x1a\n-fake-", 14, &res);
    CHECK(pd_sync_publish(R[0].s) == PD_OK);

    /* the others join: the whole shared state */
    CHECK(pd_sync_diff(R[0].s, NULL, 0, &full, &n) == PD_OK);

    for (k = 1; k < NREP; k++) {
        CHECK(pd_sync_receive(R[k].s, full, n) == PD_OK);
    }

    pd_sync_free_data(full);

    for (k = 0; k < NREP; k++) {    /* what publishing sent, everyone has already */
        cursor[k] = nlog_;
    }
}

static void test_join_and_merge(void) {
    pd_block_id p0, p1;
    pd_char_props cp;

    CHECK(same_everywhere("joined", 1));

    /* at once: one types at the start, one replaces a word, one makes a word italic */
    p0 = pd_doc_next_paragraph(R[0].d, 0);
    pd_doc_insert_text(R[0].d, at(p0, 0), "Look: ", 6, PD_FORMAT_INHERIT, NULL);
    p1 = pd_doc_next_paragraph(R[1].d, 0);
    pd_doc_delete(R[1].d, (pd_range) { at(p1, 10), at(p1, 15) }, NULL);
    pd_doc_insert_text(R[1].d, at(p1, 10), "red", 3, PD_FORMAT_INHERIT, NULL);
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_ITALIC;
    cp.italic = 1;
    pd_doc_set_char_props(R[2].d, (pd_range) { at(pd_doc_next_paragraph(R[2].d, 0), 16), at(pd_doc_next_paragraph(R[2].d, 0), 19) },
                          &cp);
    deliver_all();
    CHECK(same_everywhere("concurrent edits", 1));

    {
        const char* t;
        uint32_t n;

        pd_doc_para_text(R[2].d, pd_doc_next_paragraph(R[2].d, 0), &t, &n);
        CHECK(n == 24 && !memcmp(t, "Look: The quick red fox.", 24));
    }
}

static const char* first_text(pd_doc* d, uint32_t* n) {
    const char* t = "";

    *n = 0;
    pd_doc_para_text(d, pd_doc_next_paragraph(d, 0), &t, n);
    return t;
}

static void test_own_undo(void) {
    uint32_t n;
    const char* t;
    pd_comment c;
    pd_comment_id id;
    int k;

    /* Ann types, Bob types; Ann's undo takes away Ann's words only, on everyone's screen */
    pd_doc_insert_text(R[0].d, at(pd_doc_next_paragraph(R[0].d, 0), 0), "Ann ", 4, PD_FORMAT_INHERIT, NULL);
    deliver_all();
    pd_doc_insert_text(R[1].d, at(pd_doc_next_paragraph(R[1].d, 0), 0), "Bob ", 4, PD_FORMAT_INHERIT, NULL);
    deliver_all();
    t = first_text(R[2].d, &n);
    CHECK(n >= 8 && !memcmp(t, "Bob Ann ", 8));
    CHECK(pd_sync_can_undo(R[0].s) > 0);
    CHECK(pd_sync_undo(R[0].s) == PD_OK);
    deliver_all();

    for (k = 0; k < NREP; k++) {
        t = first_text(R[k].d, &n);
        CHECK(n >= 4 && !memcmp(t, "Bob ", 4) && (n < 8 || memcmp(t + 4, "Ann ", 4) != 0));
    }

    CHECK(pd_sync_redo(R[0].s) == PD_OK);
    deliver_all();
    t = first_text(R[1].d, &n);
    CHECK(n >= 8 && !memcmp(t, "Bob Ann ", 8));
    CHECK(same_everywhere("own undo", 1));

    /* a comment made on one replica shows on the others, on the same words, and goes when undone */
    memset(&c, 0, sizeof(c));
    strcpy(c.author, "Cy");
    c.text = "Who?";
    c.text_len = 4;
    c.range.start = at(pd_doc_next_paragraph(R[2].d, 0), 0);
    c.range.end = at(pd_doc_next_paragraph(R[2].d, 0), 3);
    CHECK(pd_doc_comment_add(R[2].d, &c, &id) == PD_OK);
    deliver_all();
    CHECK(pd_doc_comment_count(R[0].d) >= 1 && pd_doc_comment_get(R[0].d, (pd_comment_id)pd_doc_comment_count(R[0].d), &c) == PD_OK &&
          c.text_len == 4 && c.range.end.offset - c.range.start.offset == 3);
    CHECK(same_everywhere("shared comment", 1));
    CHECK(pd_sync_undo(R[2].s) == PD_OK);
    deliver_all();
    CHECK(pd_doc_comment_get(R[0].d, (pd_comment_id)pd_doc_comment_count(R[0].d), &c) != PD_OK);
    CHECK(same_everywhere("comment undone", 1));
}

static void test_random(void) {
    int i, k, diverged = 0;

    int step = getenv("SYNC_STEP") != NULL, every = getenv("SYNC_EVERY") ? atoi(getenv("SYNC_EVERY")) : 250;
    int logk[64], logop[64], nlog = 0;

    for (i = 0; i < 3000; i++) {
        int op;

        k = (int)rnd(NREP);
        op = random_edit(k, 1);

        {
            char what[64];

            snprintf(what, sizeof(what), "step %d, edit %d", i, op);
            self_check(k, what);
        }

        if (step) {     /* debugging: everything delivered and compared after every edit */
            deliver_all();

            if (!same_everywhere("step", 1)) {
                fprintf(stderr, "  first divergence: step %d, replica %d, edit %d\n", i, k, op);
                exit(1);
            }

            continue;
        }

        /* each step some replica reads on; now and then one reads a stretch again (a reconnect) */
        if (rnd(3) == 0) {
            deliver_one((int)rnd(NREP));
        }

        if (rnd(53) == 0) {
            int t = (int)rnd(NREP);

            cursor[t] -= (int)rnd(8);
            cursor[t] = cursor[t] < 0 ? 0 : cursor[t];
        }

        /* now and then a replica reconnects: says what it has, gets what it lacks from another */
        if (rnd(97) == 0) {
            int f = (int)rnd(NREP), t = (int)rnd(NREP);
            void* sv, *df;
            size_t svn, dn;

            if (f != t && pd_sync_state_vector(R[t].s, &sv, &svn) == PD_OK) {
                if (pd_sync_diff(R[f].s, sv, svn, &df, &dn) == PD_OK) {
                    CHECK(pd_sync_receive(R[t].s, df, dn) == PD_OK);
                    pd_sync_free_data(df);
                }

                pd_sync_free_data(sv);
            }
        }

        if (nlog < 64) {
            logk[nlog] = k;
            logop[nlog++] = op;
        }

        if (i % every == every - 1) {
            deliver_all();

            if (!same_everywhere("random editing", !diverged)) {
                if (!diverged) {
                    int j;

                    fprintf(stderr, "  diverged after step %d; since the last check:", i);

                    for (j = 0; j < nlog; j++) {
                        fprintf(stderr, " r%d:e%d", logk[j], logop[j]);
                    }

                    fprintf(stderr, "\n");
                }

                diverged++;

                if (getenv("SYNC_EVERY")) {
                    exit(1);
                }
            }

            nlog = 0;

            if (getenv("SYNC_REC") && !diverged) {    /* a fresh recording from this good state */
                void* base;
                size_t bn;
                FILE* f = fopen(getenv("SYNC_REC"), "wb");
                uint32_t len;

                if (rec) {
                    fclose(rec);
                }

                pd_sync_diff(R[0].s, NULL, 0, &base, &bn);
                len = (uint32_t)bn;
                fwrite(&len, 4, 1, f);
                fwrite(base, 1, bn, f);
                pd_sync_free_data(base);
                rec = f;
            }
        }
    }

    deliver_all();
    CHECK(diverged == 0 && same_everywhere("random editing, at the end", diverged == 0));
    printf("  %d edits on %d replicas, read late and re-read through a relay: %d updates, %.0f bytes each on average\n",
           i, NREP, sent_msgs, sent_msgs ? (double)sent_bytes / sent_msgs : 0.0);
}

static void test_catch_up(void) {
    pd_doc* d;
    pd_sync* s;
    void* sv, *diff;
    size_t svn, dn;
    char* a, *b;

    /* a replica that was away: says what it has, gets what it lacks */
    CHECK(pd_doc_new(&d) == PD_OK);
    CHECK(pd_sync_new(d, 99000, &s) == PD_OK);
    CHECK(pd_sync_state_vector(s, &sv, &svn) == PD_OK);
    CHECK(pd_sync_diff(R[1].s, sv, svn, &diff, &dn) == PD_OK);
    CHECK(pd_sync_receive(s, diff, dn) == PD_OK);
    a = pd_sync_dump(s, 0);
    b = pd_sync_dump(R[1].s, 0);
    CHECK(a && b && !strcmp(a, b));
    printf("  a late replica catches up from a %zu-byte state vector with a %zu-byte update\n", svn, dn);
    pd_sync_free_data(a);
    pd_sync_free_data(b);
    pd_sync_free_data(sv);
    pd_sync_free_data(diff);
    /* garbage is refused, and changes nothing */
    CHECK(pd_sync_receive(s, "\xff\xfe junk", 7) != PD_OK);
    pd_sync_free(s);
    pd_doc_free(d);
}

/* the relay's compaction: the log merged in two steps (the first merge, then it and the rest) gives
   a newcomer the same document as reading the whole log */
static void test_merge(void) {
    const void** u = (const void**)malloc((size_t)(nlog_ + 1) * sizeof(void*));
    size_t* n = (size_t*)malloc((size_t)(nlog_ + 1) * sizeof(size_t));
    void* m1, *m2, *bad;
    size_t n1, n2, nb, total = 0;
    int k, half = nlog_ / 2;
    pd_doc* d;
    pd_sync* s;
    char* a, *b;

    for (k = 0; k < half; k++) {
        u[k] = LOG[k].p;
        n[k] = LOG[k].n;
    }

    CHECK(pd_sync_merge(u, n, (size_t)half, &m1, &n1) == PD_OK);
    u[0] = m1;
    n[0] = n1;

    for (k = half; k < nlog_; k++) {
        u[k - half + 1] = LOG[k].p;
        n[k - half + 1] = LOG[k].n;
        total += LOG[k].n;
    }

    for (k = 0; k < half; k++) {
        total += LOG[k].n;
    }

    CHECK(pd_sync_merge(u, n, (size_t)(nlog_ - half + 1), &m2, &n2) == PD_OK);
    CHECK(pd_doc_new(&d) == PD_OK);
    CHECK(pd_sync_new(d, 99001, &s) == PD_OK);
    CHECK(pd_sync_receive(s, m2, n2) == PD_OK);
    a = pd_sync_dump(s, 0);
    b = pd_sync_dump(R[1].s, 0);
    CHECK(a && b && !strcmp(a, b));
    printf("  %d updates (%zu bytes) merged into one of %zu bytes\n", nlog_, total, n2);
    pd_sync_free_data(a);
    pd_sync_free_data(b);
    /* the last update without what it builds on, or garbage: refused */
    u[0] = LOG[nlog_ - 1].p;
    n[0] = LOG[nlog_ - 1].n;
    CHECK(pd_sync_merge(u, n, 1, &bad, &nb) == PD_ERR_FORMAT && !bad);
    u[0] = "\xff\xfe junk";
    n[0] = 7;
    CHECK(pd_sync_merge(u, n, 1, &bad, &nb) == PD_ERR_FORMAT && !bad);
    CHECK(pd_sync_merge(NULL, NULL, 0, &bad, &nb) == PD_OK);
    pd_sync_free_data(bad);
    pd_sync_free_data(m1);
    pd_sync_free_data(m2);
    pd_sync_free(s);
    pd_doc_free(d);
    free(u);
    free(n);
}

int main(void) {
    int k;

    seed_from_env();
    printf("join, concurrent edits\n");
    setup();
    test_join_and_merge();
    printf("undo of one's own edits, comments\n");
    test_own_undo();
    printf("random edits on three replicas\n");
    test_random();
    printf("catching up\n");
    test_catch_up();
    printf("merging the log\n");
    test_merge();

    for (k = 0; k < NREP; k++) {
        pd_sync_free(R[k].s);
        pd_doc_free(R[k].d);
    }

    for (k = 0; k < nlog_; k++) {
        free(LOG[k].p);
    }

    free(LOG);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
