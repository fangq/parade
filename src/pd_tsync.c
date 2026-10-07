/*
 * pd_tsync: a plain text shared through yrs (see include/parade_tsync.h).
 *
 * One root text, "text", indexed in UTF-16 code units. What the host does
 * is written in transactions with the origin "local", the one the undo
 * manager tracks, so undo takes back this replica's own edits only; the
 * update observer sends every change but a received one.
 *
 * Every other change -- a received update, an undo, a redo -- reaches the
 * host as one replacement: the text kept here as the host has it, compared
 * with the shared one, the differing middle between whole characters. Not
 * yrs's own delta: that one can put a position between the two halves of a
 * character beyond the BMP (an emoji), which a UTF-8 host cannot have.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "parade_tsync.h"
#include "libyrs.h"

struct pd_tsync {
    YDoc* y;
    Branch* text;
    YUndoManager* um;
    pd_tsync_send_fn send;
    void* send_user;
    pd_tsync_change_fn change;
    void* change_user;
    int remote;         /* applying a received update: not to be sent back */
    char* host;         /* the text as the host has it */
    size_t hostlen;
};

static const char LOCAL[] = "local";

uint32_t pd_tsync_units(const char* s, size_t n) {
    uint32_t u = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if ((c & 0xC0) != 0x80) {       /* a sequence's first byte: one unit, two past the BMP */
            u += c >= 0xF0 ? 2 : 1;
        }
    }

    return u;
}

static void on_update(void* user, uint32_t len, const char* data) {
    pd_tsync* t = (pd_tsync*)user;

    if (!t->remote && t->send) {
        t->send(t->send_user, data, len);
    }
}

/* the shared text now (a malloc'd copy) */
static char* shared(pd_tsync* t, size_t* len) {
    YTransaction* r = ydoc_read_transaction(t->y);
    char* s = ytext_string(t->text, r), *c;

    ytransaction_commit(r);
    *len = s ? strlen(s) : 0;
    c = (char*)malloc(*len + 1);

    if (c) {
        memcpy(c, s ? s : "", *len);
        c[*len] = 0;
    }

    if (s) {
        ystring_destroy(s);
    }

    return c;
}

static int cont(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

/* after a change the host did not make: the host told what differs, as one replacement between characters */
static void tell_host(pd_tsync* t) {
    size_t n, p = 0, q = 0, lo = t->hostlen;
    char* now = shared(t, &n);

    if (!now) {
        return;
    }

    while (p < lo && p < n && t->host[p] == now[p]) {
        p++;
    }

    while (p > 0 && ((p < lo && cont((unsigned char)t->host[p])) || (p < n && cont((unsigned char)now[p])))) {
        p--;    /* the same character on both sides up to here: back to its start */
    }

    while (q < lo - p && q < n - p && t->host[lo - 1 - q] == now[n - 1 - q]) {
        q++;
    }

    while (q > 0 && (cont((unsigned char)t->host[lo - q]) || cont((unsigned char)now[n - q]))) {
        q--;
    }

    if ((p < lo - q || p < n - q) && t->change) {
        t->change(t->change_user, pd_tsync_units(t->host, p), pd_tsync_units(t->host + p, lo - q - p), now + p,
                  n - q - p);
    }

    free(t->host);
    t->host = now;
    t->hostlen = n;
}

/* after the host's own edit: what it has is the shared text */
static void host_has(pd_tsync* t) {
    size_t n;
    char* now = shared(t, &n);

    if (now) {
        free(t->host);
        t->host = now;
        t->hostlen = n;
    }
}

pd_status pd_tsync_new(uint64_t client, pd_tsync** out) {
    pd_tsync* t;
    YOptions o;
    YUndoManagerOptions uo;
    YTransaction* w;

    if (!out) {
        return PD_ERR_ARG;
    }

    *out = NULL;

    if ((t = (pd_tsync*)calloc(1, sizeof(pd_tsync))) == NULL) {
        return PD_ERR_NOMEM;
    }

    if (!client) {      /* 53 bits, as yrs wants */
        client = ((uint64_t)time(NULL) * 2654435761u ^ (uint64_t)(uintptr_t)t * 40503u ^ (uint64_t)clock() << 20) &
                 ((1ull << 53) - 1);
    }

    o = yoptions();
    o.id = client & ((1ull << 53) - 1);
    o.flags = Y_OFFSET_UTF16 | Y_SKIP_GC;    /* undo brings back what was deleted: kept */

    if ((t->y = ydoc_new_with_options(o)) == NULL) {
        free(t);
        return PD_ERR_NOMEM;
    }

    t->text = ytext(t->y, "text");          /* a root type before any transaction: inside one it deadlocks */
    t->host = (char*)calloc(1, 1);
    uo.capture_timeout_millis = 600;         /* a run of typing is one step to undo */

    if ((t->um = yundo_manager(&uo)) != NULL) {
        yundo_manager_add_scope(t->um, t->y, t->text);
        yundo_manager_add_origin(t->um, (uint32_t)strlen(LOCAL), LOCAL);
    }

    w = ydoc_write_transaction(t->y, 0, NULL);     /* subscribing needs one that writes */
    ytransaction_observe_updates_v1(w, 1, "u", t, on_update);
    ytransaction_commit(w);
    *out = t;
    return PD_OK;
}

void pd_tsync_free(pd_tsync* t) {
    if (!t) {
        return;
    }

    if (t->um) {
        yundo_manager_destroy(t->um);
    }

    ydoc_destroy(t->y);
    free(t->host);
    free(t);
}

void pd_tsync_set_sender(pd_tsync* t, pd_tsync_send_fn fn, void* user) {
    if (t) {
        t->send = fn;
        t->send_user = user;
    }
}

void pd_tsync_set_listener(pd_tsync* t, pd_tsync_change_fn fn, void* user) {
    if (t) {
        t->change = fn;
        t->change_user = user;
    }
}

/* a NUL-terminated copy: yrs takes C strings */
static char* cstr(const char* s, size_t n) {
    char* c = (char*)malloc(n + 1);

    if (c) {
        memcpy(c, s, n);
        c[n] = 0;
    }

    return c;
}

pd_status pd_tsync_insert(pd_tsync* t, uint32_t at, const char* utf8, size_t len) {
    YTransaction* w;
    char* c;

    if (!t || (len && !utf8) || at > pd_tsync_length(t)) {
        return PD_ERR_ARG;
    }

    if (len == 0) {
        return PD_OK;
    }

    if ((c = cstr(utf8, len)) == NULL) {
        return PD_ERR_NOMEM;
    }

    w = ydoc_write_transaction(t->y, (uint32_t)strlen(LOCAL), LOCAL);
    ytext_insert(t->text, w, at, c, NULL);
    ytransaction_commit(w);
    free(c);
    host_has(t);
    return PD_OK;
}

pd_status pd_tsync_delete(pd_tsync* t, uint32_t at, uint32_t count) {
    YTransaction* w;
    uint32_t n;

    if (!t) {
        return PD_ERR_ARG;
    }

    n = pd_tsync_length(t);

    if (at > n || count > n - at) {
        return PD_ERR_ARG;
    }

    if (count == 0) {
        return PD_OK;
    }

    w = ydoc_write_transaction(t->y, (uint32_t)strlen(LOCAL), LOCAL);
    ytext_remove_range(t->text, w, at, count);
    ytransaction_commit(w);
    host_has(t);
    return PD_OK;
}

pd_status pd_tsync_publish(pd_tsync* t, const char* utf8, size_t len) {
    pd_status st;

    if (!t) {
        return PD_ERR_ARG;
    }

    st = pd_tsync_insert(t, 0, utf8, len);
    pd_tsync_clear_undo(t);     /* the starting text is not an edit to undo */
    return st;
}

pd_status pd_tsync_receive(pd_tsync* t, const void* update, size_t len) {
    YTransaction* w;
    uint8_t err;

    if (!t || (len && !update) || len > UINT32_MAX) {
        return PD_ERR_ARG;
    }

    t->remote = 1;
    w = ydoc_write_transaction(t->y, 0, NULL);
    err = ytransaction_apply(w, (const char*)update, (uint32_t)len);
    ytransaction_commit(w);
    t->remote = 0;
    tell_host(t);
    return err ? PD_ERR_FORMAT : PD_OK;
}

static pd_status undo_redo(pd_tsync* t, int redo) {
    uint8_t ok;

    if (!t || !t->um) {
        return PD_ERR_ARG;
    }

    yundo_manager_stop(t->um);      /* the step under way ends here */
    ok = redo ? yundo_manager_redo(t->um) : yundo_manager_undo(t->um);
    tell_host(t);
    return ok ? PD_OK : PD_ERR_STATE;
}

pd_status pd_tsync_undo(pd_tsync* t) {
    return undo_redo(t, 0);
}

pd_status pd_tsync_redo(pd_tsync* t) {
    return undo_redo(t, 1);
}

int32_t pd_tsync_can_undo(pd_tsync* t) {
    return t && t->um ? (int32_t)yundo_manager_undo_stack_len(t->um) : 0;
}

int32_t pd_tsync_can_redo(pd_tsync* t) {
    return t && t->um ? (int32_t)yundo_manager_redo_stack_len(t->um) : 0;
}

void pd_tsync_seal(pd_tsync* t) {
    if (t && t->um) {
        yundo_manager_stop(t->um);
    }
}

void pd_tsync_clear_undo(pd_tsync* t) {
    if (t && t->um) {
        yundo_manager_clear(t->um);
    }
}

char* pd_tsync_text(pd_tsync* t) {
    YTransaction* r;
    char* s, *c;

    if (!t) {
        return NULL;
    }

    r = ydoc_read_transaction(t->y);
    s = ytext_string(t->text, r);
    ytransaction_commit(r);
    c = cstr(s ? s : "", s ? strlen(s) : 0);

    if (s) {
        ystring_destroy(s);
    }

    return c;
}

uint32_t pd_tsync_length(pd_tsync* t) {
    YTransaction* r;
    uint32_t n;

    if (!t) {
        return 0;
    }

    r = ydoc_read_transaction(t->y);
    n = ytext_len(t->text, r);
    ytransaction_commit(r);
    return n;
}

void pd_tsync_free_data(void* data) {
    free(data);
}
