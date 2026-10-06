/*
 * Parade real-time collaboration: the document bound to a yrs CRDT replica
 *
 * The shared state (a yrs document):
 *   "blocks": map  key -> block map {"k": kind, "p": properties (JSON text),
 *                                    "t": text (paragraphs) | "kids": array of keys (containers)}
 *   "styles": map  style name -> definition (JSON text)
 *   "lists":  map  key -> list levels (JSON text)
 *   "res":    map  hash -> picture bytes, "m:" hash -> its media type
 * A block's key is made by the replica that made the block ("<client>.<n>"),
 * so keys never clash; each replica maps them to its own block ids. The
 * main tree is "root" and the stories (headers, footnotes) "stories".
 *
 * Text keeps Parade's byte offsets (the shared text counts UTF-8 bytes).
 * Character formatting is one attribute per property, so concurrent
 * changes to different properties of the same text both stay; an inline
 * object is its U+FFFC character with an "obj" attribute describing it.
 *
 * Local edits: after each operation the touched blocks are compared with
 * a shadow of what was last shared and the difference is written -- text
 * as one replaced stretch plus formatting changes, children as one
 * replaced stretch of keys, properties whole. Remote edits: the blocks an
 * update touched are compared with the document and the document is edited
 * to match, through the ordinary operations, so the caret, markers and
 * layout follow as for any edit. A block in two parents' lists (two people
 * moved it at once) is shown under the parent with the smaller key; a key
 * that would make a cycle is left out.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pd_doc_internal.h"
#include "pd_json.h"
#include "pd_conv.h"
#include "parade_sync.h"
#include "libyrs.h"

#define KEYLEN 40
#define MAXATTR 32

typedef struct {
    char k[KEYLEN];
} skey;

typedef struct {
    int valid;
    char* props;                /* properties as last shared */
    char* text;                 /* paragraphs: text and each byte's signature, as last shared */
    uint32_t len;
    int32_t* sig;
    skey* kids;                 /* containers: the shared list of children's keys */
    int32_t nkids;
} shadow;

typedef struct {
    char k[KEYLEN];
    pd_block_id id;
} hent;

/* a paragraph's text with a formatting signature for every byte */
typedef struct {
    char* t;
    uint32_t n;
    int32_t* sig;
} pstate;

/* one formatting attribute */
typedef struct {
    char name[8];
    int str;
    int64_t i;
    const char* s;
    size_t sl;
} attr;

typedef struct {
    attr a[MAXATTR];
    int n;
} attrs;

struct pd_sync {
    pd_doc* d;
    YDoc* y;
    Branch* blocks, *styles, *lists, *res;
    uint64_t client;
    uint32_t counter;
    skey* key_of;               /* by block id */
    uint32_t nkey_of;
    hent* ht;                   /* key -> block id */
    uint32_t htcap, htn;
    shadow* sh;                 /* by block id */
    uint32_t nsh;
    /* formatting signatures: interned attribute lists */
    char** sigs;
    unsigned char* sigobj;      /* has an "obj" attribute */
    pd_format_id* sfmt;         /* its format + 1, when known */
    uint32_t nsigs, capsigs, capsigs_par;
    int32_t* sht;               /* hash -> signature + 1 */
    uint32_t shtcap;
    int32_t* fsig;              /* format id -> signature + 1 */
    uint32_t nfsig;
    skey* list_key;             /* list id -> key */
    uint32_t nlist_key;
    skey* res_key;              /* resource id -> hash */
    uint32_t nres_key;
    /* work after a local operation */
    pd_block_id* dk, *dp, *dt;  /* children, properties, text to share */
    uint32_t ndk, ndp, ndt, capdk, capdp, capdt;
    pd_style_id* ds;
    uint32_t nds, capds;
    /* work after a remote update */
    skey* chg;
    uint32_t nchg, capchg;
    int styles_changed;
    int applying, remote;
    pd_sync_send_fn send;
    void* send_user;
};

static void shadows_check(pd_sync* s, const char* where);
static int list_has(const skey* v, int32_t n, const char* k);

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

/* grow a zero-filled array to hold need elements */
static int zgrow(void** p, uint32_t* cap, uint32_t need, size_t el) {
    uint32_t nc;
    void* q;

    if (need <= *cap) {
        return 0;
    }

    for (nc = *cap ? *cap : 16; nc < need; nc *= 2) {
    }

    if ((q = realloc(*p, (size_t)nc * el)) == NULL) {
        return -1;
    }

    memset((char*)q + (size_t)*cap * el, 0, (size_t)(nc - *cap) * el);
    *p = q;
    *cap = nc;
    return 0;
}

static uint32_t fnv(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    size_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ (unsigned char)s[i]) * 16777619u;
    }

    return h;
}

/* the length of a string in a fixed field, which need not be terminated */
static size_t slen(const char* s, size_t cap) {
    const char* z = (const char*)memchr(s, 0, cap);

    return z ? (size_t)(z - s) : cap;
}

static char* dupn(const char* s, size_t n) {
    char* r = (char*)malloc(n + 1);

    if (r) {
        memcpy(r, s, n);
        r[n] = '\0';
    }

    return r;
}

static int to_pb(void* user, const void* data, size_t len) {
    pb_put((pd_buf*)user, data, len);
    return 0;
}

/* the buffer as a C string, handed over */
static char* pb_take(pd_buf* b) {
    char* r;

    pb_put(b, "", 1);
    r = b->p;

    if (b->err) {
        free(r);
        return NULL;
    }

    return r;
}

/* ------------------------------------------------------------------ */
/* keys                                                               */
/* ------------------------------------------------------------------ */

static const char* key_of(const pd_sync* s, pd_block_id id) {
    return id < s->nkey_of && s->key_of[id].k[0] ? s->key_of[id].k : NULL;
}

static hent* ht_find(const pd_sync* s, const char* k) {
    uint32_t i;

    if (!s->htcap) {
        return NULL;
    }

    for (i = fnv(k, strlen(k)) & (s->htcap - 1); s->ht[i].k[0]; i = (i + 1) & (s->htcap - 1)) {
        if (!strcmp(s->ht[i].k, k)) {
            return &s->ht[i];
        }
    }

    return &s->ht[i];
}

static pd_block_id id_of(const pd_sync* s, const char* k) {
    hent* e = ht_find(s, k);

    return e && e->k[0] ? e->id : 0;
}

static int ht_rehash(pd_sync* s) {
    hent* old = s->ht;
    uint32_t oc = s->htcap, i;

    s->htcap = oc ? oc * 2 : 1024;

    if ((s->ht = (hent*)calloc(s->htcap, sizeof(hent))) == NULL) {
        s->ht = old;
        s->htcap = oc;
        return -1;
    }

    for (i = 0; i < oc; i++) {
        if (old[i].k[0]) {
            *ht_find(s, old[i].k) = old[i];
        }
    }

    free(old);
    return 0;
}

static shadow* sh_of(pd_sync* s, pd_block_id id) {
    if (zgrow((void**)&s->sh, &s->nsh, id + 1, sizeof(shadow))) {
        return NULL;
    }

    return &s->sh[id];
}

static void sh_clear(shadow* h) {
    free(h->props);
    free(h->text);
    free(h->sig);
    free(h->kids);
    memset(h, 0, sizeof(*h));
}

/* key k is block id now (a block made again for it leaves the old one keyless) */
static int map_set(pd_sync* s, const char* k, pd_block_id id) {
    hent* e;

    if ((s->htn + 1) * 2 > s->htcap && ht_rehash(s)) {
        return -1;
    }

    if (zgrow((void**)&s->key_of, &s->nkey_of, id + 1, sizeof(skey)) || !sh_of(s, id)) {
        return -1;
    }

    e = ht_find(s, k);

    if (e->k[0] && e->id < s->nkey_of && e->id != id) {
        s->key_of[e->id].k[0] = '\0';
        sh_clear(&s->sh[e->id]);
    } else if (!e->k[0]) {
        s->htn++;
    }

    snprintf(e->k, KEYLEN, "%s", k);
    e->id = id;
    snprintf(s->key_of[id].k, KEYLEN, "%s", k);
    return 0;
}

static void key_new(pd_sync* s, char* out) {
    snprintf(out, KEYLEN, "%llx.%x", (unsigned long long)s->client, ++s->counter);
}

/* ------------------------------------------------------------------ */
/* the shared state                                                   */
/* ------------------------------------------------------------------ */

static Branch* map_branch(const Branch* m, const YTransaction* t, const char* key, int8_t want) {
    YOutput* o = m ? ymap_get(m, t, key) : NULL;
    Branch* b = NULL;

    if (o) {
        b = o->tag == want ? (want == Y_MAP ? youtput_read_ymap(o) : want == Y_TEXT ? youtput_read_ytext(o) :
                              youtput_read_yarray(o)) : NULL;
        youtput_destroy(o);
    }

    return b;
}

static Branch* block_map(pd_sync* s, const YTransaction* t, const char* key) {
    return map_branch(s->blocks, t, key, Y_MAP);
}

/* a string entry of a map, copied (NULL if none) */
static char* map_str(const Branch* m, const YTransaction* t, const char* key) {
    YOutput* o = m ? ymap_get(m, t, key) : NULL;
    char* r = NULL, *v;

    if (o) {
        if ((v = youtput_read_string(o)) != NULL) {
            r = dupn(v, strlen(v));
        }

        youtput_destroy(o);
    }

    return r;
}

static int64_t map_int(const Branch* m, const YTransaction* t, const char* key, int64_t def) {
    YOutput* o = m ? ymap_get(m, t, key) : NULL;
    const int64_t* v;
    int64_t r = def;

    if (o) {
        if ((v = youtput_read_long(o)) != NULL) {
            r = *v;
        } else if (o->tag == Y_JSON_NUM) {
            r = (int64_t)o->value.num;
        }

        youtput_destroy(o);
    }

    return r;
}

/* a block's shared list of children's keys */
static skey* kids_read(const Branch* kb, const YTransaction* t, int32_t* n) {
    uint32_t len = kb ? yarray_len(kb) : 0, i;
    skey* r = (skey*)calloc((size_t)len + 1, sizeof(skey));

    *n = 0;

    for (i = 0; r && i < len; i++) {
        YOutput* o = yarray_get(kb, t, i);
        char* v = o ? youtput_read_string(o) : NULL;

        if (v) {
            snprintf(r[(*n)++].k, KEYLEN, "%s", v);
        }

        if (o) {
            youtput_destroy(o);
        }
    }

    return r;
}

/* ------------------------------------------------------------------ */
/* formatting signatures                                              */
/* ------------------------------------------------------------------ */

static void at_int(attrs* a, const char* name, int64_t v) {
    if (a->n < MAXATTR) {
        snprintf(a->a[a->n].name, sizeof(a->a[0].name), "%s", name);
        a->a[a->n].str = 0;
        a->a[a->n].i = v;
        a->n++;
    }
}

static void at_str(attrs* a, const char* name, const char* v, size_t len) {
    if (a->n < MAXATTR) {
        snprintf(a->a[a->n].name, sizeof(a->a[0].name), "%s", name);
        a->a[a->n].str = 1;
        a->a[a->n].s = v;
        a->a[a->n].sl = len;
        a->n++;
    }
}

static int at_cmp(const void* x, const void* y) {
    return strcmp(((const attr*)x)->name, ((const attr*)y)->name);
}

/* the signature of an attribute list: interned, so equal lists are one number */
static int32_t sig_intern(pd_sync* s, attrs* a) {
    pd_buf b;
    uint32_t h, i;
    int k;

    memset(&b, 0, sizeof(b));
    qsort(a->a, (size_t)a->n, sizeof(attr), at_cmp);

    for (k = 0; k < a->n; k++) {
        size_t j;

        pb_puts(&b, a->a[k].name);
        pb_put(&b, "\x01", 1);

        if (a->a[k].str) {
            pb_put(&b, "s", 1);

            for (j = 0; j < a->a[k].sl; j++) {
                char c = a->a[k].s[j];

                pb_put(&b, c == '\x01' || c == '\x02' || c == '\0' ? " " : &c, 1);
            }
        } else {
            pb_printf(&b, "i%lld", (long long)a->a[k].i);
        }

        pb_put(&b, "\x02", 1);
    }

    pb_put(&b, "", 1);

    if (b.err) {
        pb_free(&b);
        return 0;
    }

    if (s->nsigs * 2 >= s->shtcap) {    /* the hash table at most half full */
        uint32_t nc = s->shtcap ? s->shtcap * 2 : 1024;
        int32_t* t = (int32_t*)calloc(nc, sizeof(int32_t));

        if (!t) {
            pb_free(&b);
            return 0;
        }

        free(s->sht);
        s->sht = t;
        s->shtcap = nc;

        for (i = 0; i < s->nsigs; i++) {
            for (h = fnv(s->sigs[i], strlen(s->sigs[i])) & (nc - 1); s->sht[h]; h = (h + 1) & (nc - 1)) {
            }

            s->sht[h] = (int32_t)i + 1;
        }
    }

    for (h = fnv(b.p, b.n - 1) & (s->shtcap - 1); s->sht[h]; h = (h + 1) & (s->shtcap - 1)) {
        if (!strcmp(s->sigs[s->sht[h] - 1], b.p)) {
            pb_free(&b);
            return s->sht[h] - 1;
        }
    }

    if (zgrow((void**)&s->sigs, &s->capsigs, s->nsigs + 1, sizeof(char*))) {
        pb_free(&b);
        return 0;
    }

    {   /* the parallel arrays: the same capacity */
        uint32_t c1 = s->nsigs ? s->nsigs : 0, c2 = c1;

        c1 = c2 = s->sigobj ? s->capsigs_par : 0;

        if (zgrow((void**)&s->sigobj, &c1, s->capsigs, 1) || zgrow((void**)&s->sfmt, &c2, s->capsigs,
                sizeof(pd_format_id))) {
            pb_free(&b);
            return 0;
        }

        s->capsigs_par = s->capsigs;
    }

    s->sigs[s->nsigs] = b.p;
    s->sigobj[s->nsigs] = strstr(b.p, "obj\x01") == b.p || strstr(b.p, "\x02obj\x01") != NULL;
    s->sfmt[s->nsigs] = 0;
    s->sht[h] = (int32_t)s->nsigs + 1;
    return (int32_t)s->nsigs++;
}

/* a signature back into its attributes (pointing into the interned string) */
static void sig_attrs(const pd_sync* s, int32_t sig, attrs* a) {
    const char* p = s->sigs[sig];

    a->n = 0;

    while (*p && a->n < MAXATTR) {
        const char* e = strchr(p, '\x01'), *v, *z;
        attr* x = &a->a[a->n];

        if (!e) {
            break;
        }

        v = e + 1;
        z = strchr(v, '\x02');

        if (!z) {
            break;
        }

        snprintf(x->name, sizeof(x->name), "%.*s", (int)(e - p), p);

        if (*v == 's') {
            x->str = 1;
            x->s = v + 1;
            x->sl = (size_t)(z - v - 1);
        } else {
            x->str = 0;
            x->i = strtoll(v + 1, NULL, 10);
        }

        a->n++;
        p = z + 1;
    }
}

static const attr* at_get(const attrs* a, const char* name) {
    int k;

    for (k = 0; k < a->n; k++) {
        if (!strcmp(a->a[k].name, name)) {
            return &a->a[k];
        }
    }

    return NULL;
}

/* the attributes of a format */
static void fmt_attrs(pd_sync* s, pd_format_id f, attrs* a, int* has_link) {
    pd_doc* d = s->d;
    pd_style_id cs = 0;
    pd_char_props cp;
    uint32_t m;
    static char rev[160];

    a->n = 0;
    *has_link = 0;

    if (pd_doc_format_info(d, f, &cs, &cp) != PD_OK) {
        return;
    }

    m = cp.mask;

    if (cs && pd_doc_style_name(d, cs)) {
        at_str(a, "cs", pd_doc_style_name(d, cs), strlen(pd_doc_style_name(d, cs)));
    }

    if (m & PD_CP_FAMILY) {
        at_str(a, "f", cp.family, slen(cp.family, sizeof(cp.family)));
    }

#define AI(bit, name, field) if (m & (bit)) { at_int(a, name, (int64_t)cp.field); }
    AI(PD_CP_SIZE, "sz", size)
    AI(PD_CP_WEIGHT, "w", weight)
    AI(PD_CP_ITALIC, "i", italic)
    AI(PD_CP_COLOR, "c", color)
    AI(PD_CP_BACKGROUND, "bg", background)
    AI(PD_CP_UNDERLINE, "u", underline)
    AI(PD_CP_STRIKE, "s", strike)
    AI(PD_CP_SHIFT, "sh", shift)
    AI(PD_CP_LETTERSPACE, "ls", letter_space)
    AI(PD_CP_KERNING, "k", kerning)
    AI(PD_CP_SMALLCAPS, "sc", small_caps)
    AI(PD_CP_CAPS, "caps", caps)
    AI(PD_CP_HIDDEN, "hid", hidden)
    AI(PD_CP_POSITION, "pos", position)
#undef AI

    if (m & PD_CP_LANG) {
        at_str(a, "lang", cp.lang, slen(cp.lang, sizeof(cp.lang)));
    }

    if ((m & PD_CP_LINK) && cp.link_target) {
        const char* k = key_of(s, cp.link_target);

        *has_link = 1;

        if (k) {
            at_str(a, "link", k, strlen(k));
        }
    }

    if ((m & PD_CP_REVISION) && cp.revision) {
        pd_revision rv;

        if (pd_doc_revision_get(d, cp.revision, &rv) == PD_OK) {
            snprintf(rev, sizeof(rev), "%d|%s|%s", (int)rv.kind, rv.author, rv.date);
            at_str(a, "rev", rev, strlen(rev));
        }
    }
}

static int32_t fmt_sig(pd_sync* s, pd_format_id f) {
    attrs a;
    int link;
    int32_t g;

    if (f < s->nfsig && s->fsig[f]) {
        return s->fsig[f] - 1;
    }

    fmt_attrs(s, f, &a, &link);
    g = sig_intern(s, &a);

    if (!link && !zgrow((void**)&s->fsig, &s->nfsig, f + 1, sizeof(int32_t))) {
        s->fsig[f] = g + 1;     /* a link's key can change from unknown to known: not kept */
    }

    return g;
}

/* the signature with an object attribute added */
static int32_t sig_with_obj(pd_sync* s, int32_t sig, const char* obj) {
    attrs a;

    sig_attrs(s, sig, &a);
    at_str(&a, "obj", obj, strlen(obj));
    return sig_intern(s, &a);
}

/* the format a signature describes in this document */
static pd_format_id sig_fmt(pd_sync* s, int32_t sig) {
    pd_doc* d = s->d;
    attrs a;
    const attr* x;
    pd_char_props cp;
    pd_style_id cs = 0;
    int link = 0;
    pd_format_id f;
    char buf[200];

    if (s->sfmt[sig]) {
        return s->sfmt[sig] - 1;
    }

    sig_attrs(s, sig, &a);
    memset(&cp, 0, sizeof(cp));

    if ((x = at_get(&a, "cs")) != NULL) {
        snprintf(buf, sizeof(buf), "%.*s", (int)x->sl, x->s);
        cs = pd_doc_style_find(d, buf);
    }

    if ((x = at_get(&a, "f")) != NULL) {
        cp.mask |= PD_CP_FAMILY;
        snprintf(cp.family, sizeof(cp.family), "%.*s", (int)x->sl, x->s);
    }

#define AG(bit, name, field, type) if ((x = at_get(&a, name)) != NULL) { cp.mask |= (bit); cp.field = (type)x->i; }
    AG(PD_CP_SIZE, "sz", size, pd_sp)
    AG(PD_CP_WEIGHT, "w", weight, int32_t)
    AG(PD_CP_ITALIC, "i", italic, int32_t)
    AG(PD_CP_COLOR, "c", color, uint32_t)
    AG(PD_CP_BACKGROUND, "bg", background, uint32_t)
    AG(PD_CP_UNDERLINE, "u", underline, int32_t)
    AG(PD_CP_STRIKE, "s", strike, int32_t)
    AG(PD_CP_SHIFT, "sh", shift, int32_t)
    AG(PD_CP_LETTERSPACE, "ls", letter_space, pd_sp)
    AG(PD_CP_KERNING, "k", kerning, int32_t)
    AG(PD_CP_SMALLCAPS, "sc", small_caps, int32_t)
    AG(PD_CP_CAPS, "caps", caps, int32_t)
    AG(PD_CP_HIDDEN, "hid", hidden, int32_t)
    AG(PD_CP_POSITION, "pos", position, pd_sp)
#undef AG

    if ((x = at_get(&a, "lang")) != NULL) {
        cp.mask |= PD_CP_LANG;
        snprintf(cp.lang, sizeof(cp.lang), "%.*s", (int)x->sl, x->s);
    }

    if ((x = at_get(&a, "link")) != NULL) {
        snprintf(buf, sizeof(buf), "%.*s", (int)x->sl, x->s);
        link = 1;

        if ((cp.link_target = id_of(s, buf)) != 0) {
            cp.mask |= PD_CP_LINK;
        }
    }

    if ((x = at_get(&a, "rev")) != NULL) {
        pd_revision rv;
        char* p, *q;

        memset(&rv, 0, sizeof(rv));
        snprintf(buf, sizeof(buf), "%.*s", (int)x->sl, x->s);
        rv.kind = atoi(buf);

        if ((p = strchr(buf, '|')) != NULL && (q = strchr(p + 1, '|')) != NULL) {
            *q = '\0';
            snprintf(rv.author, sizeof(rv.author), "%s", p + 1);
            snprintf(rv.date, sizeof(rv.date), "%s", q + 1);

            if (pd_doc_revision_add(d, &rv, &cp.revision) == PD_OK) {
                cp.mask |= PD_CP_REVISION;
            }
        }
    }

    f = pd_doc_format(d, cs, &cp);

    if (!link) {
        s->sfmt[sig] = f + 1;
    }

    return f;
}

/* attributes as a yrs map input; with nulls for the names in `minus` it lacks (they are cleared) */
typedef struct {
    char* keys[MAXATTR * 2];
    YInput vals[MAXATTR * 2];
    char* strs[MAXATTR * 2];
    uint32_t n;
} ymapin;

static void ymapin_make(pd_sync* s, int32_t sig, int32_t minus, ymapin* m) {
    attrs a, o;
    int k, j;

    m->n = 0;
    sig_attrs(s, sig, &a);

    for (k = 0; k < a.n; k++) {
        if (!strcmp(a.a[k].name, "obj")) {
            continue;   /* an object is an embed of its own, not formatting */
        }

        m->keys[m->n] = dupn(a.a[k].name, strlen(a.a[k].name));
        m->strs[m->n] = a.a[k].str ? dupn(a.a[k].s, a.a[k].sl) : NULL;
        m->vals[m->n] = a.a[k].str ? yinput_string(m->strs[m->n]) : yinput_long(a.a[k].i);
        m->n++;
    }

    if (minus >= 0) {
        sig_attrs(s, minus, &o);

        for (j = 0; j < o.n; j++) {
            if (!at_get(&a, o.a[j].name) && strcmp(o.a[j].name, "obj") != 0) {
                m->keys[m->n] = dupn(o.a[j].name, strlen(o.a[j].name));
                m->strs[m->n] = NULL;
                m->vals[m->n] = yinput_null();
                m->n++;
            }
        }
    }
}

static void ymapin_free(ymapin* m) {
    uint32_t k;

    for (k = 0; k < m->n; k++) {
        free(m->keys[k]);
        free(m->strs[k]);
    }

    m->n = 0;
}

/* ------------------------------------------------------------------ */
/* inline objects and pictures                                        */
/* ------------------------------------------------------------------ */

static const char* const obj_kind[] = { "image", "equation", "field", "footnote", "link", "bookmark", "tab", "user",
                                        "raw"
                                      };

/* the hash a picture is shared under; stored in the shared state when t can write */
static const char* res_hash(pd_sync* s, pd_res_id r, YTransaction* t) {
    const char* mime;
    const void* data;
    size_t len;

    if (!r || pd_doc_resource(s->d, r, &mime, &data, &len) != PD_OK) {
        return NULL;
    }

    if (r >= s->nres_key || !s->res_key[r].k[0]) {
        uint64_t h = 14695981039346656037ull;
        size_t i;

        for (i = 0; i < len; i++) {
            h = (h ^ ((const unsigned char*)data)[i]) * 1099511628211ull;
        }

        if (zgrow((void**)&s->res_key, &s->nres_key, r + 1, sizeof(skey))) {
            return NULL;
        }

        snprintf(s->res_key[r].k, KEYLEN, "%016llx%x", (unsigned long long)h, (unsigned)(len & 0xFFFFF));
    }

    if (t && ytransaction_writeable(t)) {
        YOutput* o = ymap_get(s->res, t, s->res_key[r].k);

        if (o) {
            youtput_destroy(o);
        } else {
            char mk[KEYLEN + 4];
            YInput v = yinput_binary((const char*)data, (uint32_t)len), m = yinput_string(mime);

            snprintf(mk, sizeof(mk), "m:%s", s->res_key[r].k);
            ymap_insert(s->res, t, s->res_key[r].k, &v);
            ymap_insert(s->res, t, mk, &m);
        }
    }

    return s->res_key[r].k;
}

/* the picture shared under a hash, in this document */
static pd_res_id res_local(pd_sync* s, const YTransaction* t, const char* hash) {
    uint32_t r;
    YOutput* o;
    pd_res_id id = 0;

    for (r = 1; r < s->nres_key; r++) {
        if (!strcmp(s->res_key[r].k, hash)) {
            return r;
        }
    }

    if ((o = ymap_get(s->res, t, hash)) != NULL) {
        const char* data = youtput_read_binary(o);
        char mk[KEYLEN + 4], *mime;

        snprintf(mk, sizeof(mk), "m:%s", hash);
        mime = map_str(s->res, t, mk);

        if (data && pd_doc_add_resource(s->d, mime ? mime : "application/octet-stream", data, o->len, &id) == PD_OK &&
                !zgrow((void**)&s->res_key, &s->nres_key, id + 1, sizeof(skey))) {
            snprintf(s->res_key[id].k, KEYLEN, "%s", hash);
        }

        free(mime);
        youtput_destroy(o);
    }

    return id;
}

static char* obj_json(pd_sync* s, const pd_inline* o, YTransaction* t) {
    pd_buf b;
    pj_writer w;
    const char* k;

    memset(&b, 0, sizeof(b));
    pj_init(&w, 0, to_pb, &b);
    w.compact = 1;
    pj_obj_begin(&w);
    pj_key(&w, "Kind");
    pj_cstr(&w, o->kind >= 0 && o->kind < (int32_t)(sizeof(obj_kind) / sizeof(obj_kind[0])) ? obj_kind[o->kind] : "user");
#define OI(name, v) if (v) { pj_key(&w, name); pj_int(&w, (int64_t)(v)); }
    OI("W", o->width)
    OI("H", o->height)
    OI("D", o->depth)
    OI("Field", o->field)
    OI("Level", o->level)
    OI("User", o->user)
#undef OI

    if (o->target && (k = key_of(s, o->target)) != NULL) {
        pj_key(&w, "Target");
        pj_cstr(&w, k);
    }

    if (o->kind == PD_INLINE_IMAGE && (k = res_hash(s, o->resource, t)) != NULL) {
        pj_key(&w, "Res");
        pj_cstr(&w, k);
    }

    if (o->name[0]) {
        pj_key(&w, "Name");
        pj_cstr(&w, o->name);
    }

    if (o->source_len > 0) {
        pj_key(&w, "Src");
        pj_str(&w, o->source, (size_t)o->source_len);
    }

    if (o->title_len > 0) {
        pj_key(&w, "Title");
        pj_str(&w, o->title, (size_t)o->title_len);
    }

    if (o->alt_len > 0) {
        pj_key(&w, "Alt");
        pj_str(&w, o->alt, (size_t)o->alt_len);
    }

    pj_obj_end(&w);
    pj_finish(&w);
    return pb_take(&b);
}

/* an object from its description; the strings point into the parsed tree */
static int obj_parse(pd_sync* s, const YTransaction* t, const pj_node* r, pd_inline* o) {
    const pj_node* x;
    int32_t k;

    memset(o, 0, sizeof(*o));

    if (!r || r->type != PJ_OBJ || !(x = pj_get(r, "Kind")) || x->type != PJ_STR) {
        return 0;
    }

    for (k = 0; k < (int32_t)(sizeof(obj_kind) / sizeof(obj_kind[0])); k++) {
        if (!strcmp(x->s, obj_kind[k])) {
            o->kind = k;
        }
    }

    o->width = (pd_sp)pj_int_or(pj_get(r, "W"), 0);
    o->height = (pd_sp)pj_int_or(pj_get(r, "H"), 0);
    o->depth = (pd_sp)pj_int_or(pj_get(r, "D"), 0);
    o->field = (int32_t)pj_int_or(pj_get(r, "Field"), 0);
    o->level = (int32_t)pj_int_or(pj_get(r, "Level"), 0);
    o->user = (int32_t)pj_int_or(pj_get(r, "User"), 0);

    if ((x = pj_get(r, "Target")) != NULL && x->type == PJ_STR) {
        o->target = id_of(s, x->s);
    }

    if ((x = pj_get(r, "Res")) != NULL && x->type == PJ_STR) {
        o->resource = res_local(s, t, x->s);
    }

    if ((x = pj_get(r, "Name")) != NULL && x->type == PJ_STR) {
        snprintf(o->name, sizeof(o->name), "%s", x->s);
    }

    if ((x = pj_get(r, "Src")) != NULL && x->type == PJ_STR) {
        o->source = x->s;
        o->source_len = (int32_t)x->len;
    }

    if ((x = pj_get(r, "Title")) != NULL && x->type == PJ_STR) {
        o->title = x->s;
        o->title_len = (int32_t)x->len;
    }

    if ((x = pj_get(r, "Alt")) != NULL && x->type == PJ_STR) {
        o->alt = x->s;
        o->alt_len = (int32_t)x->len;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* paragraph text with signatures                                     */
/* ------------------------------------------------------------------ */

static void ps_free(pstate* p) {
    free(p->t);
    free(p->sig);
    memset(p, 0, sizeof(*p));
}

/* a paragraph of the document; objects' descriptions are written with t (pictures stored when it writes) */
static int ps_doc(pd_sync* s, const blk* b, pstate* p, YTransaction* t) {
    const bstate* st = &b->st;
    uint32_t i;
    int32_t r;

    memset(p, 0, sizeof(*p));
    p->n = st->len;
    p->t = dupn(st->text ? st->text : "", st->len);
    p->sig = (int32_t*)malloc(((size_t)st->len + 1) * sizeof(int32_t));

    if (!p->t || !p->sig) {
        ps_free(p);
        return -1;
    }

    for (i = 0; i < st->len; i++) {
        p->sig[i] = fmt_sig(s, 0);
    }

    for (r = 0; r < st->nruns; r++) {
        int32_t g = fmt_sig(s, st->runs[r].format);

        for (i = st->runs[r].start; i < st->runs[r].end && i < st->len; i++) {
            p->sig[i] = g;
        }
    }

    for (r = 0; r < st->ninl; r++) {
        uint32_t off = st->inl[r].offset;
        char* j;

        if (off + 3 > st->len || (j = obj_json(s, &st->inl[r].obj, t)) == NULL) {
            continue;
        }

        p->sig[off] = p->sig[off + 1] = p->sig[off + 2] = sig_with_obj(s, p->sig[off], j);
        free(j);
    }

    return 0;
}

/* a paragraph of the shared state */
static int ps_crdt(pd_sync* s, const Branch* txt, const YTransaction* t, pstate* p) {
    uint32_t nc = 0, c;
    YChunk* ch = txt ? ytext_chunks(txt, t, &nc) : NULL;
    pd_buf tb;
    int32_t* sig = NULL;
    uint32_t cap = 0;

    memset(p, 0, sizeof(*p));
    memset(&tb, 0, sizeof(tb));

    for (c = 0; c < nc; c++) {
        char* v = youtput_read_string(&ch[c].data);
        const char* obj = NULL;
        attrs a;
        uint32_t k, n0 = (uint32_t)tb.n, n;
        int32_t g;

        if (!v && ch[c].data.tag == Y_JSON_MAP) {   /* an embed: an inline object, U+FFFC in the text */
            for (k = 0; k < ch[c].data.len; k++) {
                const YMapEntry* e = &ch[c].data.value.map[k];

                if (e->key && !strcmp(e->key, "o") && e->value && e->value->tag == Y_JSON_STR) {
                    obj = youtput_read_string(e->value);
                }
            }

            if (!obj) {
                continue;
            }

            v = (char*)"\xEF\xBF\xBC";
        } else if (!v) {
            continue;
        }

        a.n = 0;

        for (k = 0; k < ch[c].fmt_len; k++) {
            const YOutput* x = ch[c].fmt[k].value;
            const char* name = ch[c].fmt[k].key;
            char* sv;

            if (!x || !name) {
                continue;
            }

            if (x->tag == Y_JSON_STR && (sv = youtput_read_string(x)) != NULL) {
                at_str(&a, name, sv, strlen(sv));
            } else if (x->tag == Y_JSON_INT) {
                at_int(&a, name, *youtput_read_long(x));
            } else if (x->tag == Y_JSON_NUM) {
                at_int(&a, name, (int64_t)x->value.num);
            } else if (x->tag == Y_JSON_BOOL) {
                at_int(&a, name, x->value.flag ? 1 : 0);
            }
        }

        if (obj) {
            at_str(&a, "obj", obj, strlen(obj));
        }

        g = sig_intern(s, &a);     /* the attribute strings are copied in */
        n = (uint32_t)strlen(v);
        pb_put(&tb, v, n);

        if (zgrow((void**)&sig, &cap, (uint32_t)tb.n + 1, sizeof(int32_t))) {
            break;
        }

        for (k = n0; k < n0 + n; k++) {
            sig[k] = g;
        }
    }

    if (ch) {
        ychunks_destroy(ch, nc);
    }

    p->n = (uint32_t)tb.n;
    p->t = pb_take(&tb);
    p->sig = sig ? sig : (int32_t*)calloc(1, sizeof(int32_t));

    if (!p->t || !p->sig) {
        ps_free(p);
        return -1;
    }

    return 0;
}

/* the stretch where two texts differ: same in [0, *pre) and in the last *suf bytes; an object
   counts as the same only with the same description */
static void ps_diff(const pd_sync* s, const pstate* a, const pstate* b, uint32_t* pre, uint32_t* suf) {
    uint32_t p = 0, q = 0, m = a->n < b->n ? a->n : b->n;

#define SAME(i, j) (a->t[i] == b->t[j] && ((!s->sigobj[a->sig[i]] && !s->sigobj[b->sig[j]]) || a->sig[i] == b->sig[j]))

    while (p < m && SAME(p, p)) {
        p++;
    }

    while (p > 0 && p < m && ((unsigned char)a->t[p] & 0xC0) == 0x80) {     /* not inside a character */
        p--;
    }

    while (q < m - p && SAME(a->n - 1 - q, b->n - 1 - q)) {
        q++;
    }

    while (q > 0 && q < a->n && ((unsigned char)a->t[a->n - q] & 0xC0) == 0x80) {
        q--;
    }

#undef SAME
    *pre = p;
    *suf = q;
}

/* the shared text's index for a byte offset: an object is one there, three bytes here */
static uint32_t yidx(const pd_sync* s, const pstate* p, uint32_t off) {
    uint32_t i, n = 0;

    for (i = 0; i < off && i < p->n; i++) {
        if ((unsigned char)p->t[i] == 0xEF && s->sigobj[p->sig[i]]) {
            n++;
        }
    }

    return off - 2 * n;
}

/* ------------------------------------------------------------------ */
/* block properties                                                   */
/* ------------------------------------------------------------------ */

static const char* list_key(pd_sync* s, pd_list_id l, YTransaction* t) {
    if (!l) {
        return NULL;
    }

    if ((l >= s->nlist_key || !s->list_key[l].k[0]) && t && ytransaction_writeable(t)) {
        pd_list_level lv[9];
        int32_t n = 9, k;
        pd_buf b;
        pj_writer w;
        char* js;
        YInput v;

        if (pd_doc_list_info(s->d, l, &n, lv) != PD_OK || zgrow((void**)&s->list_key, &s->nlist_key, l + 1,
                sizeof(skey))) {
            return NULL;
        }

        memset(&b, 0, sizeof(b));
        pj_init(&w, 0, to_pb, &b);
        w.compact = 1;
        pj_arr_begin(&w);

        for (k = 0; k < n; k++) {
            pj_arr_begin(&w);
            pj_int(&w, lv[k].format);
            pj_int(&w, lv[k].start);
            pj_cstr(&w, lv[k].text);
            pj_int(&w, lv[k].indent);
            pj_int(&w, lv[k].hanging);
            pj_int(&w, lv[k].restart_after);
            pj_cstr(&w, lv[k].label_family);
            pj_int(&w, lv[k].label_size);
            pj_int(&w, lv[k].label_weight);
            pj_int(&w, lv[k].label_italic);
            pj_int(&w, (int64_t)lv[k].label_color);
            pj_arr_end(&w);
        }

        pj_arr_end(&w);
        pj_finish(&w);

        if ((js = pb_take(&b)) == NULL) {
            return NULL;
        }

        key_new(s, s->list_key[l].k);
        v = yinput_string(js);
        ymap_insert(s->lists, t, s->list_key[l].k, &v);
        free(js);
    }

    return l < s->nlist_key && s->list_key[l].k[0] ? s->list_key[l].k : NULL;
}

/* the list shared under a key, in this document (defined from the shared state when new) */
static pd_list_id list_local(pd_sync* s, const YTransaction* t, const char* key) {
    uint32_t l;
    char* js;
    pj_doc* j;
    pd_list_id id = 0;

    for (l = 1; l < s->nlist_key; l++) {
        if (!strcmp(s->list_key[l].k, key)) {
            return l;
        }
    }

    if ((js = map_str(s->lists, t, key)) == NULL) {
        return 0;
    }

    if ((j = pj_parse(js, strlen(js), 0, NULL)) != NULL) {
        const pj_node* r = pj_root(j), *c;
        pd_list_level lv[9];
        int32_t n = 0;

        memset(lv, 0, sizeof(lv));

        for (c = r && r->type == PJ_ARR ? r->child : NULL; c && n < 9; c = c->next, n++) {
            const pj_node* e = c->type == PJ_ARR ? c->child : NULL;

#define NX(dst, type) if (e) { dst = (type)pj_int_or(e, 0); e = e->next; }
#define NS(dst) if (e) { if (e->type == PJ_STR) snprintf(dst, sizeof(dst), "%s", e->s); e = e->next; }
            NX(lv[n].format, int32_t)
            NX(lv[n].start, int32_t)
            NS(lv[n].text)
            NX(lv[n].indent, pd_sp)
            NX(lv[n].hanging, pd_sp)
            NX(lv[n].restart_after, int32_t)
            NS(lv[n].label_family)
            NX(lv[n].label_size, pd_sp)
            NX(lv[n].label_weight, int32_t)
            NX(lv[n].label_italic, int32_t)
            NX(lv[n].label_color, uint32_t)
#undef NX
#undef NS
        }

        if (n > 0 && pd_doc_list_define(s->d, n, lv, &id) == PD_OK &&
                !zgrow((void**)&s->list_key, &s->nlist_key, id + 1, sizeof(skey))) {
            snprintf(s->list_key[id].k, KEYLEN, "%s", key);
        } else {
            id = 0;
        }

        pj_free(j);
    }

    free(js);
    return id;
}

/* a block's properties as shared: ids made into names and keys */
static char* props_json(pd_sync* s, const blk* b, YTransaction* t) {
    pd_doc* d = s->d;
    pd_buf buf;
    pj_writer w;

    memset(&buf, 0, sizeof(buf));
    pj_init(&w, 0, to_pb, &buf);
    w.compact = 1;
    pj_obj_begin(&w);

    if (b->kind == PD_BLOCK_PARAGRAPH) {
        const bstate* st = &b->st;
        const char* k;
        pd_para_props pp = st->pp;

        pj_key(&w, "Role");
        pj_int(&w, st->role);
        pj_key(&w, "Level");
        pj_int(&w, st->level);

        if (st->style && pd_doc_style_name(d, st->style)) {
            pj_key(&w, "Style");
            pj_cstr(&w, pd_doc_style_name(d, st->style));
        }

        if (st->list && (k = list_key(s, st->list, t)) != NULL) {
            pj_key(&w, "List");
            pj_cstr(&w, k);
            pj_key(&w, "ListLevel");
            pj_int(&w, st->list_level);
        }

        if ((pp.mask & PD_PP_NEXT_STYLE) && pp.next_style && pd_doc_style_name(d, pp.next_style)) {
            pj_key(&w, "Next");
            pj_cstr(&w, pd_doc_style_name(d, pp.next_style));
        }

        pp.mask &= ~PD_PP_NEXT_STYLE;
        pp.next_style = 0;

        if (pp.mask) {
            pj_key(&w, "Para");
            pd_jd_put_pp(&w, &pp);
        }

        if (st->at.quote_depth || st->at.task || st->at.loose || st->at.lang[0] || st->at.cont || st->at.div_class[0]) {
            pj_key(&w, "Attrs");
            pj_arr_begin(&w);
            pj_int(&w, st->at.quote_depth);
            pj_int(&w, st->at.task);
            pj_int(&w, st->at.loose);
            pj_cstr(&w, st->at.lang);
            pj_int(&w, st->at.cont);
            pj_cstr(&w, st->at.div_class);
            pj_arr_end(&w);
        }
    } else if (b->kind != PD_BLOCK_ROOT && b->kind != PD_BLOCK_STORY && b->kind != PD_BLOCK_ROW) {
        blk c = *b;     /* the stories a section names go as keys, by themselves */

        if (b->kind == PD_BLOCK_SECTION) {
            pd_block_id* f[6];
            int i;

            f[0] = &c.st.sp.header;
            f[1] = &c.st.sp.header_first;
            f[2] = &c.st.sp.header_even;
            f[3] = &c.st.sp.footer;
            f[4] = &c.st.sp.footer_first;
            f[5] = &c.st.sp.footer_even;
            pj_key(&w, "St");
            pj_arr_begin(&w);

            for (i = 0; i < 6; i++) {
                const char* k = *f[i] ? key_of(s, *f[i]) : NULL;

                pj_cstr(&w, k ? k : "");
                *f[i] = 0;
            }

            pj_arr_end(&w);
        }

        pj_key(&w, "B");
        pd_jd_put_block(&w, d, &c, 0);
    }

    pj_obj_end(&w);
    pj_finish(&w);
    return pb_take(&buf);
}

/* make a block's properties what the shared state says */
static void props_apply(pd_sync* s, const YTransaction* t, blk* b, const char* json) {
    pd_doc* d = s->d;
    pj_doc* j = pj_parse(json, strlen(json), 0, NULL);
    const pj_node* r = j ? pj_root(j) : NULL, *x;
    pd_block_id id = b->id;

    if (!r || r->type != PJ_OBJ) {
        pj_free(j);
        return;
    }

    if (b->kind == PD_BLOCK_PARAGRAPH) {
        pd_style_id st = 0;
        pd_list_id l = 0;
        int32_t role = (int32_t)pj_int_or(pj_get(r, "Role"), 0), level = (int32_t)pj_int_or(pj_get(r, "Level"), 0);
        int32_t ll = (int32_t)pj_int_or(pj_get(r, "ListLevel"), 0);
        pd_para_props pp;
        pd_para_attrs at;

        if ((x = pj_get(r, "Style")) != NULL && x->type == PJ_STR) {
            st = pd_doc_style_find(d, x->s);
        }

        if (st != b->st.style) {
            pd_doc_set_para_style(d, id, st);
        }

        if (role != b->st.role || level != b->st.level) {
            pd_doc_set_role(d, id, (pd_role)role, level);
        }

        if ((x = pj_get(r, "List")) != NULL && x->type == PJ_STR) {
            l = list_local(s, t, x->s);
        }

        if ((b = pd_doc_blk(d, id)) != NULL && (l != b->st.list || (l && ll != b->st.list_level))) {
            pd_doc_set_list(d, id, l, l ? ll : 0);
        }

        memset(&pp, 0, sizeof(pp));

        if ((x = pj_get(r, "Para")) != NULL) {
            pd_jd_get_pp(d, x, &pp);
        }

        if ((x = pj_get(r, "Next")) != NULL && x->type == PJ_STR && (pp.next_style = pd_doc_style_find(d, x->s)) != 0) {
            pp.mask |= PD_PP_NEXT_STYLE;
        }

        pd_doc_pp_normalize(&pp);

        if ((b = pd_doc_blk(d, id)) != NULL && memcmp(&pp, &b->st.pp, sizeof(pp)) != 0) {
            pd_doc_set_para_props(d, id, &pp);
        }

        memset(&at, 0, sizeof(at));

        if ((x = pj_get(r, "Attrs")) != NULL && x->type == PJ_ARR) {
            const pj_node* e = x->child;

            if (e) {
                at.quote_depth = (int32_t)pj_int_or(e, 0);
                e = e->next;
            }

            if (e) {
                at.task = (int32_t)pj_int_or(e, 0);
                e = e->next;
            }

            if (e) {
                at.loose = (int32_t)pj_int_or(e, 0);
                e = e->next;
            }

            if (e) {
                if (e->type == PJ_STR) {
                    snprintf(at.lang, sizeof(at.lang), "%s", e->s);
                }

                e = e->next;
            }

            if (e) {
                at.cont = (int32_t)pj_int_or(e, 0);
                e = e->next;
            }

            if (e && e->type == PJ_STR) {
                snprintf(at.div_class, sizeof(at.div_class), "%s", e->s);
            }
        }

        if ((b = pd_doc_blk(d, id)) != NULL && memcmp(&at, &b->st.at, sizeof(at)) != 0) {
            pd_doc_set_para_attrs(d, id, &at);
        }
    } else if ((x = pj_get(r, "B")) != NULL) {
        bstate st = b->st;

        if (pd_jd_get_block(d, x, b->kind, &st)) {
            if (b->kind == PD_BLOCK_SECTION) {
                pd_block_id* f[6];
                const pj_node* e = pj_get(r, "St");
                int i;

                f[0] = &st.sp.header;
                f[1] = &st.sp.header_first;
                f[2] = &st.sp.header_even;
                f[3] = &st.sp.footer;
                f[4] = &st.sp.footer_first;
                f[5] = &st.sp.footer_even;
                e = e && e->type == PJ_ARR ? e->child : NULL;

                for (i = 0; i < 6; i++, e = e ? e->next : NULL) {
                    *f[i] = e && e->type == PJ_STR && e->s[0] ? id_of(s, e->s) : 0;
                }

                if (memcmp(&st.sp, &b->st.sp, sizeof(st.sp)) != 0) {
                    pd_doc_set_section_props(d, id, &st.sp);
                }
            } else if (b->kind == PD_BLOCK_FLOAT && memcmp(&st.fp, &b->st.fp, sizeof(st.fp)) != 0) {
                pd_doc_set_float_props(d, id, &st.fp);
            } else if (b->kind == PD_BLOCK_TABLE && memcmp(&st.tp, &b->st.tp, sizeof(st.tp)) != 0) {
                pd_doc_set_table_props(d, id, &st.tp);
            } else if (b->kind == PD_BLOCK_CELL && memcmp(&st.cell, &b->st.cell, sizeof(st.cell)) != 0) {
                pd_doc_set_cell_props(d, id, &st.cell);
            } else if (b->kind == PD_BLOCK_BREAK && st.break_kind != b->st.break_kind) {
                pd_doc_set_break(d, id, (pd_break_kind)st.break_kind);
            }
        }
    }

    pj_free(j);
}

/* ------------------------------------------------------------------ */
/* styles                                                             */
/* ------------------------------------------------------------------ */

static char* style_json(pd_sync* s, pd_style_id id) {
    pd_doc* d = s->d;
    int32_t kind = 0;
    pd_style_id parent = 0;
    pd_para_props pp;
    pd_char_props cp;
    pd_buf b;
    pj_writer w;

    if (pd_doc_style_info(d, id, &kind, &parent, &pp, &cp) != PD_OK) {
        return NULL;
    }

    memset(&b, 0, sizeof(b));
    pj_init(&w, 0, to_pb, &b);
    w.compact = 1;
    pj_obj_begin(&w);
    pj_key(&w, "Kind");
    pj_int(&w, kind);

    if (parent && pd_doc_style_name(d, parent)) {
        pj_key(&w, "Parent");
        pj_cstr(&w, pd_doc_style_name(d, parent));
    }

    if ((pp.mask & PD_PP_NEXT_STYLE) && pp.next_style && pd_doc_style_name(d, pp.next_style)) {
        pj_key(&w, "Next");
        pj_cstr(&w, pd_doc_style_name(d, pp.next_style));
    }

    pp.mask &= ~PD_PP_NEXT_STYLE;
    pp.next_style = 0;
    pj_key(&w, "Para");
    pd_jd_put_pp(&w, &pp);
    pj_key(&w, "Char");
    pd_jd_put_cp(&w, &cp);
    pj_obj_end(&w);
    pj_finish(&w);
    return pb_take(&b);
}

static void style_share(pd_sync* s, YTransaction* t, pd_style_id id) {
    const char* name = pd_doc_style_name(s->d, id);
    char* js = name ? style_json(s, id) : NULL, *old;

    if (js && ((old = map_str(s->styles, t, name)) == NULL || strcmp(old, js) != 0)) {
        YInput v = yinput_string(js);

        ymap_insert(s->styles, t, name, &v);
        free(old);
    } else if (js) {
        free(old);
    }

    free(js);
}

/* every shared style this document defines differently, defined as shared */
static void styles_pull(pd_sync* s, const YTransaction* t) {
    YMapIter* it = ymap_iter(s->styles, t);
    YMapEntry* e;
    int pass;

    /* twice: a style's parent may come after it */
    for (pass = 0; pass < 2 && it; pass++) {
        while ((e = ymap_iter_next(it)) != NULL) {
            char* v = e->value ? youtput_read_string(e->value) : NULL;
            pd_style_id id = pd_doc_style_find(s->d, e->key);
            char* cur = id ? style_json(s, id) : NULL;

            if (v && (!cur || strcmp(cur, v) != 0)) {
                pj_doc* j = pj_parse(v, strlen(v), 0, NULL);
                const pj_node* r = j ? pj_root(j) : NULL, *x;

                if (r && r->type == PJ_OBJ) {
                    pd_para_props pp;
                    pd_char_props cp;
                    pd_style_id parent = 0;

                    memset(&pp, 0, sizeof(pp));
                    memset(&cp, 0, sizeof(cp));

                    if ((x = pj_get(r, "Para")) != NULL) {
                        pd_jd_get_pp(s->d, x, &pp);
                    }

                    if ((x = pj_get(r, "Char")) != NULL) {
                        pd_jd_get_cp(s->d, x, &cp);
                    }

                    if ((x = pj_get(r, "Next")) != NULL && x->type == PJ_STR &&
                            (pp.next_style = pd_doc_style_find(s->d, x->s)) != 0) {
                        pp.mask |= PD_PP_NEXT_STYLE;
                    }

                    if ((x = pj_get(r, "Parent")) != NULL && x->type == PJ_STR) {
                        parent = pd_doc_style_find(s->d, x->s);
                    }

                    pd_doc_style_define(s->d, e->key, (pd_style_kind)pj_int_or(pj_get(r, "Kind"), 0), parent, &pp, &cp,
                                        NULL);
                }

                pj_free(j);
            }

            free(cur);
            ymap_entry_destroy(e);
        }

        ymap_iter_destroy(it);
        it = pass == 0 ? ymap_iter(s->styles, t) : NULL;
    }

    /* formats name styles: what they resolve to may have changed */
    if (s->sfmt) {
        memset(s->sfmt, 0, (size_t)s->capsigs_par * sizeof(pd_format_id));
    }

    if (s->fsig) {
        memset(s->fsig, 0, (size_t)s->nfsig * sizeof(int32_t));
    }
}

/* ------------------------------------------------------------------ */
/* local edits into the shared state                                  */
/* ------------------------------------------------------------------ */

static void push(pd_block_id** v, uint32_t* n, uint32_t* cap, pd_block_id id) {
    uint32_t i;

    for (i = 0; i < *n; i++) {
        if ((*v)[i] == id) {
            return;
        }
    }

    if (!zgrow((void**)v, cap, *n + 1, sizeof(pd_block_id))) {
        (*v)[(*n)++] = id;
    }
}

static int is_container(int32_t kind) {
    return kind != PD_BLOCK_PARAGRAPH && kind != PD_BLOCK_BREAK;
}

/* a block new to the shared state: its map, with its content to follow */
static const char* share_new(pd_sync* s, YTransaction* t, blk* b) {
    char key[KEYLEN], *props;
    char* keys[3];
    YInput vals[3], m;
    static char empty[1];
    YInput none[1];
    shadow* h;

    key_new(s, key);

    if (map_set(s, key, b->id) || (props = props_json(s, b, t)) == NULL) {
        return NULL;
    }

    keys[0] = "k";
    keys[1] = "p";
    vals[0] = yinput_long(b->kind);
    vals[1] = yinput_string(props);

    if (is_container(b->kind)) {
        keys[2] = "kids";
        vals[2] = yinput_yarray(none, 0);
    } else {
        keys[2] = "t";
        vals[2] = yinput_ytext(empty);
    }

    m = yinput_ymap(keys, vals, b->kind == PD_BLOCK_BREAK ? 2 : 3);
    ymap_insert(s->blocks, t, key, &m);
    h = sh_of(s, b->id);
    sh_clear(h);
    h->valid = 1;
    h->props = props;

    if (is_container(b->kind)) {
        push(&s->dk, &s->ndk, &s->capdk, b->id);
    } else if (b->kind == PD_BLOCK_PARAGRAPH) {
        push(&s->dt, &s->ndt, &s->capdt, b->id);
    }

    return key_of(s, b->id);
}

/* Turn the shared list's stretch from old into new with as few changes as can be found: the keys
   of new that are in old in the same order (a longest increasing run of their old places) stay,
   everything else goes and comes in where new has it -- so a move moves one key, and two people
   moving different blocks at once do not each write the whole list. at: where the stretch starts. */
static void list_edit(Branch* kb, YTransaction* t, const skey* old, int32_t no, const skey* nw, int32_t nn,
                      uint32_t at) {
    int32_t* pos = (int32_t*)malloc(((size_t)nn + 1) * sizeof(int32_t));    /* each new key's old place */
    int32_t* tail = (int32_t*)malloc(((size_t)nn + 1) * sizeof(int32_t));   /* LIS: smallest end of each length */
    int32_t* prev = (int32_t*)malloc(((size_t)nn + 1) * sizeof(int32_t));
    unsigned char* keep_old = (unsigned char*)calloc((size_t)no + 1, 1), *keep_new = (unsigned char*)calloc((size_t)nn + 1,
                              1);
    int32_t i, j, len = 0, idx;

    if (!pos || !tail || !prev || !keep_old || !keep_new) {
        goto done;
    }

    for (i = 0; i < nn; i++) {
        pos[i] = -1;

        for (j = 0; j < no && pos[i] < 0; j++) {
            if (!keep_old[j] && !strcmp(nw[i].k, old[j].k)) {
                pos[i] = j;
                keep_old[j] = 2;    /* claimed (a key twice in old: the first copy) */
            }
        }
    }

    for (i = 0; i < nn; i++) {  /* patience: tail[l] = the new index ending the best run of length l + 1 */
        int32_t lo = 0, hi = len;

        if (pos[i] < 0) {
            continue;
        }

        while (lo < hi) {
            int32_t mid = (lo + hi) / 2;

            if (pos[tail[mid]] < pos[i]) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        prev[i] = lo > 0 ? tail[lo - 1] : -1;
        tail[lo] = i;

        if (lo == len) {
            len++;
        }
    }

    memset(keep_old, 0, (size_t)no + 1);

    for (idx = len ? tail[len - 1] : -1; idx >= 0; idx = prev[idx]) {
        keep_new[idx] = 1;
        keep_old[pos[idx]] = 1;
    }

    for (j = no - 1; j >= 0; j--) {     /* what does not stay goes, from the end so places hold */
        if (!keep_old[j]) {
            yarray_remove_range(kb, t, at + (uint32_t)j, 1);
        }
    }

    for (i = 0, idx = 0; i < nn; i++, idx++) {  /* the rest comes in where it belongs */
        if (!keep_new[i]) {
            YInput v = yinput_string(nw[i].k);

            yarray_insert_range(kb, t, at + (uint32_t)idx, &v, 1);
        }
    }

done:
    free(pos);
    free(tail);
    free(prev);
    free(keep_old);
    free(keep_new);
}

/* a block back in a list (undo of its deletion): its content is compared again, all the way down,
   since it may be older than what is shared for it */
static void push_subtree(pd_sync* s, pd_block_id id, int depth) {
    blk* b = pd_doc_blk(s->d, id);
    int32_t i;

    if (!b || depth > 64) {
        return;
    }

    push(&s->dp, &s->ndp, &s->capdp, id);

    if (is_container(b->kind)) {
        push(&s->dk, &s->ndk, &s->capdk, id);
    }

    for (i = 0; i < b->nkids; i++) {
        push_subtree(s, b->kids[i], depth + 1);
    }
}

static void kids_share(pd_sync* s, YTransaction* t, blk* b) {
    const char* k = key_of(s, b->id);
    Branch* bm = k ? block_map(s, t, k) : NULL, *kb = bm ? map_branch(bm, t, "kids", Y_ARRAY) : NULL;
    shadow* h = sh_of(s, b->id);
    skey* cur;
    int32_t n = 0, i, p = 0, q = 0, m;

    if (!kb || !h || (cur = (skey*)calloc((size_t)b->nkids + 1, sizeof(skey))) == NULL) {
        return;
    }

    for (i = 0; i < b->nkids; i++) {
        blk* c = pd_doc_blk(s->d, b->kids[i]);
        const char* ck = c ? key_of(s, c->id) : NULL;

        if (c && !ck) {
            ck = share_new(s, t, c);
        }

        if (ck) {
            snprintf(cur[n++].k, KEYLEN, "%s", ck);
        }
    }

    if ((h = sh_of(s, b->id)) == NULL) {     /* sharing children grows the shadows: found again */
        free(cur);
        return;
    }

    for (i = 0; i < b->nkids; i++) {    /* keyed, but not in the list as shared: back from a deletion */
        const char* ck = key_of(s, b->kids[i]);

        if (ck && !list_has(h->kids, h->nkids, ck)) {
            push_subtree(s, b->kids[i], 0);
        }
    }

    m = n < h->nkids ? n : h->nkids;

    while (p < m && !strcmp(cur[p].k, h->kids[p].k)) {
        p++;
    }

    while (q < m - p && !strcmp(cur[n - 1 - q].k, h->kids[h->nkids - 1 - q].k)) {
        q++;
    }

    list_edit(kb, t, h->kids + p, h->nkids - p - q, cur + p, n - p - q, (uint32_t)p);
    free(h->kids);
    h->kids = cur;
    h->nkids = n;
}

static void props_share(pd_sync* s, YTransaction* t, blk* b) {
    const char* k = key_of(s, b->id);
    Branch* bm = k ? block_map(s, t, k) : NULL;
    shadow* h = sh_of(s, b->id);
    char* js;

    if (!bm || !h || (js = props_json(s, b, t)) == NULL) {
        return;
    }

    if (!h->props || strcmp(h->props, js) != 0) {
        YInput v = yinput_string(js);

        ymap_insert(bm, t, "p", &v);
        free(h->props);
        h->props = js;
    } else {
        free(js);
    }
}

static void text_share(pd_sync* s, YTransaction* t, blk* b) {
    const char* k = key_of(s, b->id);
    Branch* bm = k ? block_map(s, t, k) : NULL, *tb = bm ? map_branch(bm, t, "t", Y_TEXT) : NULL;
    shadow* h = sh_of(s, b->id);
    pstate cur, old;
    uint32_t p, q, i, j, dl, il, ypos;
    int32_t* after;

    if (!tb || !h || ps_doc(s, b, &cur, t)) {
        return;
    }

    old.t = h->text ? h->text : (char*)"";
    old.n = h->len;
    old.sig = h->sig;

    if (!old.sig) {
        old.n = 0;
    }

    ps_diff(s, &old, &cur, &p, &q);
    dl = old.n - q - p;
    il = cur.n - q - p;

    if (dl > 0) {
        uint32_t y0 = yidx(s, &old, p);

        ytext_remove_range(tb, t, y0, yidx(s, &old, p + dl) - y0);
    }

    for (i = p, ypos = yidx(s, &cur, p); i < p + il; i = j) {   /* the new stretch, a run at a time */
        ymapin m;
        YInput in;

        if (s->sigobj[cur.sig[i]]) {    /* an object: an embed carrying its description */
            attrs a;
            const attr* x;

            j = i + 3 <= p + il ? i + 3 : p + il;
            sig_attrs(s, cur.sig[i], &a);

            if ((x = at_get(&a, "obj")) != NULL) {
                char* js = dupn(x->s, x->sl), *ok = "o";
                YInput val = yinput_string(js), content = yinput_json_map(&ok, &val, 1);

                ymapin_make(s, cur.sig[i], -1, &m);
                in = yinput_json_map(m.keys, m.vals, m.n);
                ytext_insert_embed(tb, t, ypos, &content, &in);
                ymapin_free(&m);
                free(js);
                ypos++;
            }

            continue;
        }

        for (j = i + 1; j < p + il && cur.sig[j] == cur.sig[i]; j++) {
        }

        {
            char* piece = dupn(cur.t + i, j - i);

            if (!piece) {
                break;
            }

            ymapin_make(s, cur.sig[i], -1, &m);
            in = yinput_json_map(m.keys, m.vals, m.n);
            ytext_insert(tb, t, ypos, piece, &in);
            ymapin_free(&m);
            free(piece);
            ypos += j - i;
        }
    }

    /* formatting changed where the text did not */
    after = (int32_t*)malloc(((size_t)cur.n + 1) * sizeof(int32_t));

    if (after) {
        for (i = 0; i < p; i++) {
            after[i] = old.sig[i];
        }

        for (i = p; i < p + il; i++) {
            after[i] = cur.sig[i];
        }

        for (i = 0; i < q; i++) {
            after[cur.n - q + i] = old.sig[old.n - q + i];
        }

        for (i = 0; i < cur.n; i = j) {
            ymapin m;
            YInput in;

            for (j = i + 1; j < cur.n && cur.sig[j] == cur.sig[i] && after[j] == after[i]; j++) {
            }

            if (cur.sig[i] != after[i]) {
                uint32_t y0 = yidx(s, &cur, i);

                ymapin_make(s, cur.sig[i], after[i], &m);
                in = yinput_json_map(m.keys, m.vals, m.n);

                if (m.n) {
                    ytext_format(tb, t, y0, yidx(s, &cur, j) - y0, &in);
                }

                ymapin_free(&m);
            }
        }

        free(after);
    }

    free(h->text);
    free(h->sig);
    h->text = cur.t;
    h->len = cur.n;
    h->sig = cur.sig;
}

/* the touched blocks of the operation just finished, into the shared state */
static void share(pd_sync* s) {
    YTransaction* t = ydoc_write_transaction(s->y, 0, NULL);
    int guard = 0;

    while ((s->ndk || s->ndp || s->ndt || s->nds) && guard++ < 1000000) {
        blk* b;
        pd_block_id id;

        if (s->nds) {
            style_share(s, t, s->ds[--s->nds]);
            continue;
        }

        if (s->ndk) {
            id = s->dk[--s->ndk];
        } else if (s->ndp) {
            id = s->dp[--s->ndp];
        } else {
            id = s->dt[--s->ndt];
        }

        if ((b = pd_doc_blk(s->d, id)) == NULL) {
            continue;
        }

        if (!key_of(s, id)) {   /* new: its parent shares it, with its content */
            if (b->parent && pd_doc_blk(s->d, b->parent)) {
                push(&s->dk, &s->ndk, &s->capdk, b->parent);
            }

            continue;
        }

        if (is_container(b->kind)) {
            kids_share(s, t, b);
        }

        props_share(s, t, b);

        if (b->kind == PD_BLOCK_PARAGRAPH) {
            text_share(s, t, b);
        }
    }

    ytransaction_commit(t);
    shadows_check(s, "share");
}

static void on_doc_change(void* user) {
    pd_sync* s = (pd_sync*)user;
    int32_t i, n;

    if (s->applying) {
        return;
    }

    n = pd_doc_touched_count(s->d);

    for (i = 0; i < n; i++) {
        int32_t kind;
        pd_block_id b;
        pd_style_id st;

        pd_doc_touched(s->d, i, &kind, &b, &st);

        if (kind == PD_CHANGE_STRUCTURE && b) {
            push(&s->dk, &s->ndk, &s->capdk, b);
        } else if (kind == PD_CHANGE_STYLE && st) {
            push(&s->ds, &s->nds, &s->capds, st);
        } else if (b) {
            push(&s->dp, &s->ndp, &s->capdp, b);
        }
    }

    if (s->ndk || s->ndp || s->nds) {
        share(s);
    }
}

/* ------------------------------------------------------------------ */
/* the shared state into the document                                 */
/* ------------------------------------------------------------------ */

static void reconcile_kids(pd_sync* s, const YTransaction* t, pd_block_id pid, const char* pkey, int depth);

static void reconcile_text(pd_sync* s, const YTransaction* t, pd_block_id id, const char* key) {
    pd_doc* d = s->d;
    Branch* bm = block_map(s, t, key), *tb = bm ? map_branch(bm, t, "t", Y_TEXT) : NULL;
    blk* b = pd_doc_blk(d, id);
    shadow* h = sh_of(s, id);
    pstate want, cur;
    uint32_t p, q, i, j, dl;
    pd_pos at;

    if (!tb || !b || !h || ps_crdt(s, tb, t, &want)) {
        return;
    }

    if (ps_doc(s, b, &cur, NULL)) {
        ps_free(&want);
        return;
    }

    ps_diff(s, &cur, &want, &p, &q);
    dl = cur.n - q - p;
    at.block = id;

    if (dl > 0) {
        pd_range r;

        r.start.block = r.end.block = id;
        r.start.offset = p;
        r.end.offset = p + dl;
        pd_doc_delete(d, r, NULL);
    }

    for (i = p; i < want.n - q; i = j) {
        pd_format_id f = sig_fmt(s, want.sig[i]);

        at.offset = i;

        if (s->sigobj[want.sig[i]]) {   /* an object: its own three bytes */
            attrs a;
            const attr* x;
            pd_inline o;
            pj_doc* jd = NULL;

            j = i + 3 <= want.n ? i + 3 : want.n;
            sig_attrs(s, want.sig[i], &a);

            if ((x = at_get(&a, "obj")) != NULL && (jd = pj_parse(x->s, x->sl, 0, NULL)) != NULL &&
                    obj_parse(s, t, pj_root(jd), &o) && pd_doc_insert_inline(d, at, &o, NULL) == PD_OK) {
                pd_range r;

                r.start = at;
                r.end = at;
                r.end.offset += 3;
                pd_doc_set_format(d, r, f);
            }

            pj_free(jd);
            continue;
        }

        for (j = i + 1; j < want.n - q && want.sig[j] == want.sig[i]; j++) {
        }

        pd_doc_insert_text(d, at, want.t + i, j - i, f, NULL);
    }

    /* formatting changed where the text did not */
    ps_free(&cur);

    if ((b = pd_doc_blk(d, id)) != NULL && ps_doc(s, b, &cur, NULL) == 0) {
        for (i = 0; i < cur.n && i < want.n; i = j) {
            for (j = i + 1; j < cur.n && j < want.n && cur.sig[j] == cur.sig[i] && want.sig[j] == want.sig[i]; j++) {
            }

            if (cur.sig[i] != want.sig[i]) {
                pd_range r;

                r.start.block = r.end.block = id;
                r.start.offset = i;
                r.end.offset = j;
                pd_doc_set_format(d, r, sig_fmt(s, want.sig[i]));
            }
        }

        ps_free(&cur);
    }

    free(h->text);
    free(h->sig);
    h->text = want.t;
    h->len = want.n;
    h->sig = want.sig;
}

static void reconcile_props(pd_sync* s, const YTransaction* t, pd_block_id id, const char* key) {
    Branch* bm = block_map(s, t, key);
    blk* b = pd_doc_blk(s->d, id);
    shadow* h = sh_of(s, id);
    char* js = bm ? map_str(bm, t, "p") : NULL;

    if (!js || !b || !h) {
        free(js);
        return;
    }

    if (!h->props || strcmp(h->props, js) != 0) {
        props_apply(s, t, b, js);
    }

    free(h->props);
    h->props = js;
}

/* a block of the shared state, made in the document at a place */
static pd_block_id materialize(pd_sync* s, const YTransaction* t, pd_block_id parent, int32_t index, const char* key,
                               int32_t kind, int depth) {
    pd_block_id id = 0;
    shadow* h;

    if (pd_doc_insert_block(s->d, parent == PD_STORYROOT_ID && kind == PD_BLOCK_STORY ? 0 : parent, index,
                            (pd_block_kind)kind, &id) != PD_OK || map_set(s, key, id) || (h = sh_of(s, id)) == NULL) {
        return 0;
    }

    sh_clear(h);
    h->valid = 1;
    reconcile_props(s, t, id, key);

    if (kind == PD_BLOCK_PARAGRAPH) {
        reconcile_text(s, t, id, key);
    } else if (is_container(kind)) {
        reconcile_kids(s, t, id, key, depth + 1);
    }

    return id;
}

static int ancestor_or_self(const pd_doc* d, pd_block_id a, pd_block_id id) {
    int n = 0;
    blk* b;

    for (; id && n < 4096; n++) {
        if (id == a) {
            return 1;
        }

        b = pd_doc_blk(d, id);
        id = b ? b->parent : 0;
    }

    return 0;
}

static int list_has(const skey* v, int32_t n, const char* k) {
    int32_t i;

    for (i = 0; i < n; i++) {
        if (!strcmp(v[i].k, k)) {
            return 1;
        }
    }

    return 0;
}

static int allowed(int32_t parent_kind, pd_block_id parent, int32_t kind) {
    return parent == PD_STORYROOT_ID ? kind == PD_BLOCK_STORY : pd_doc_child_allowed(parent_kind, kind);
}

/* make a container's children what its shared list says */
static void reconcile_kids(pd_sync* s, const YTransaction* t, pd_block_id pid, const char* pkey, int depth) {
    pd_doc* d = s->d;
    Branch* bm = block_map(s, t, pkey), *kb = bm ? map_branch(bm, t, "kids", Y_ARRAY) : NULL;
    blk* p = pd_doc_blk(d, pid);
    shadow* h = sh_of(s, pid);
    skey* raw;
    int32_t n = 0, i, placed = 0;

    if (!kb || !p || !h || depth > 64 || (raw = kids_read(kb, t, &n)) == NULL) {
        return;
    }

    free(h->kids);
    h->kids = raw;
    h->nkids = n;
    h->valid = 1;

    for (i = 0; i < n; i++) {
        const char* k = raw[i].k;
        Branch* cm;
        int32_t kind;
        pd_block_id id;
        blk* c;

        if (list_has(raw, i, k) || (cm = block_map(s, t, k)) == NULL) {
            continue;   /* twice in the list, or not a block */
        }

        kind = (int32_t)map_int(cm, t, "k", -1);
        p = pd_doc_blk(d, pid);

        if (!p || !allowed(p->kind, pid, kind)) {
            continue;
        }

        id = id_of(s, k);
        c = id ? pd_doc_blk(d, id) : NULL;

        if (c && c->kind != kind) {
            c = NULL;
        }

        if (c) {
            if (ancestor_or_self(d, c->id, pid)) {
                continue;   /* would make a cycle */
            }

            if (c->parent != pid) {     /* in two lists at once: the parent with the smaller key keeps it */
                const char* qk = key_of(s, c->parent);
                shadow* qh = c->parent < s->nsh ? &s->sh[c->parent] : NULL;

                if (qk && qh && qh->valid && list_has(qh->kids, qh->nkids, k) && strcmp(qk, pkey) < 0) {
                    continue;
                }
            }

            if (placed >= p->nkids || p->kids[placed] != c->id) {
                if (pd_doc_move_block(d, c->id, pid, placed) != PD_OK) {
                    continue;
                }
            }
        } else if (!materialize(s, t, pid, placed, k, kind, depth)) {
            continue;
        }

        placed++;
    }

    /* nothing listed (two people removed its last children at once): the container, which cannot be
       empty here, gets an empty placeholder of its own -- the same on every replica, and shared only
       once someone writes in it */
    if (placed == 0 && pid != PD_STORYROOT_ID && (p = pd_doc_blk(d, pid)) != NULL) {
        int32_t pk = p->kind == PD_BLOCK_ROOT ? PD_BLOCK_SECTION : p->kind == PD_BLOCK_TABLE ? PD_BLOCK_ROW :
                     p->kind == PD_BLOCK_ROW ? PD_BLOCK_CELL : PD_BLOCK_PARAGRAPH;

        if (!(p->nkids == 1 && !key_of(s, p->kids[0]) && pd_doc_blk(d, p->kids[0]) &&
                pd_doc_blk(d, p->kids[0])->kind == pk) && pd_doc_insert_block(d, pid, 0, (pd_block_kind)pk, NULL) == PD_OK) {
            placed = 1;
        } else if (p->nkids >= 1) {
            placed = 1;
        }
    }

    /* whatever else is there goes */
    while ((p = pd_doc_blk(d, pid)) != NULL && p->nkids > placed && placed > 0) {
        pd_block_id x = p->kids[p->nkids - 1];

        if (pd_doc_remove_block(d, x) != PD_OK) {   /* its key stays mapped: it may be listed again */
            break;
        }
    }
}

/* a block the document has set aside (deleted, kept for undo): its shadow follows the shared state, so
   that if undo brings the block back, what it brings is compared with what is really shared */
static void shadow_pull(pd_sync* s, const YTransaction* t, pd_block_id id, const char* key) {
    Branch* bm = block_map(s, t, key), *x;
    shadow* h = sh_of(s, id);
    char* js;

    if (!bm || !h) {
        return;
    }

    if ((js = map_str(bm, t, "p")) != NULL) {
        free(h->props);
        h->props = js;
    }

    if ((x = map_branch(bm, t, "t", Y_TEXT)) != NULL) {
        pstate p;

        if (ps_crdt(s, x, t, &p) == 0) {
            free(h->text);
            free(h->sig);
            h->text = p.t;
            h->len = p.n;
            h->sig = p.sig;
        }
    }

    if ((x = map_branch(bm, t, "kids", Y_ARRAY)) != NULL) {
        int32_t n = 0;
        skey* kids = kids_read(x, t, &n);

        if (kids) {
            free(h->kids);
            h->kids = kids;
            h->nkids = n;
        }
    }
}

static void reconcile(pd_sync* s) {
    pd_doc* d = s->d;
    YTransaction* t = ydoc_read_transaction(s->y);
    char track[sizeof(d->track)];
    uint32_t i;
    int pass;

    memcpy(track, d->track, sizeof(track));
    memset(d->track, 0, sizeof(d->track));     /* remote edits are not this author's tracked changes */
    s->applying = 1;
    pd_doc_begin_group(d, "Remote edits");

    if (s->styles_changed) {
        styles_pull(s, t);
        s->styles_changed = 0;
    }

    /* the trees first, the stories before what names them; then properties and text */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < s->nchg; i++) {
            const char* k = s->chg[i].k;
            pd_block_id id = id_of(s, k);
            blk* b = id ? pd_doc_blk(d, id) : NULL;

            if (b && is_container(b->kind) && (pass == 0) == (id == PD_STORYROOT_ID)) {
                reconcile_kids(s, t, id, k, 0);
            }
        }
    }

    for (i = 0; i < s->nchg; i++) {
        const char* k = s->chg[i].k;
        pd_block_id id = id_of(s, k);
        blk* b = id ? pd_doc_blk(d, id) : NULL;

        if (b) {
            reconcile_props(s, t, id, k);

            if (b->kind == PD_BLOCK_PARAGRAPH) {
                reconcile_text(s, t, id, k);
            }
        } else if (id && id < d->captab && d->tab[id]) {
            shadow_pull(s, t, id, k);
        }
    }

    pd_doc_end_group(d);
    s->applying = 0;
    memcpy(d->track, track, sizeof(track));
    ytransaction_commit(t);
    s->nchg = 0;
}

/* ------------------------------------------------------------------ */
/* observers                                                          */
/* ------------------------------------------------------------------ */

static void chg_add(pd_sync* s, const char* k) {
    uint32_t i;

    for (i = 0; i < s->nchg; i++) {
        if (!strcmp(s->chg[i].k, k)) {
            return;
        }
    }

    if (strlen(k) < KEYLEN && !zgrow((void**)&s->chg, &s->capchg, s->nchg + 1, sizeof(skey))) {
        snprintf(s->chg[s->nchg++].k, KEYLEN, "%s", k);
    }
}

static void on_blocks(void* user, uint32_t n, const YEvent* ev) {
    pd_sync* s = (pd_sync*)user;
    uint32_t i;

    if (!s->remote) {
        return;
    }

    for (i = 0; i < n; i++) {
        YPathSegment* path = NULL;
        uint32_t len = 0;

        if (ev[i].tag == Y_MAP) {
            path = ymap_event_path(&ev[i].content.map, &len);
        } else if (ev[i].tag == Y_TEXT) {
            path = ytext_event_path(&ev[i].content.text, &len);
        } else if (ev[i].tag == Y_ARRAY) {
            path = yarray_event_path(&ev[i].content.array, &len);
        }

        if (len > 0 && path[0].tag == Y_EVENT_PATH_KEY) {
            chg_add(s, path[0].value.key);
        } else if (len == 0 && ev[i].tag == Y_MAP) {     /* blocks added to the map itself */
            uint32_t nk = 0, k;
            YEventKeyChange* kc = ymap_event_keys(&ev[i].content.map, &nk);

            for (k = 0; k < nk; k++) {
                chg_add(s, kc[k].key);
            }

            if (kc) {
                yevent_keys_destroy(kc, nk);
            }
        }

        if (path) {
            ypath_destroy(path, len);
        }
    }
}

static void on_styles(void* user, uint32_t n, const YEvent* ev) {
    pd_sync* s = (pd_sync*)user;

    (void)n;
    (void)ev;

    if (s->remote) {
        s->styles_changed = 1;
    }
}

static void on_update(void* user, uint32_t len, const char* data) {
    pd_sync* s = (pd_sync*)user;

    if (!s->remote && s->send) {
        s->send(s->send_user, data, len);
    }
}

/* debugging (built with -DPD_SYNC_SELFCHECK): every shadow is the shared state */
static void shadows_check(pd_sync* s, const char* where) {
    YTransaction* t;
    uint32_t id;

#ifndef PD_SYNC_SELFCHECK
    (void)s;
    (void)t;
    (void)id;
    (void)where;
    return;
#else
    t = ydoc_read_transaction(s->y);

    for (id = 1; id < s->nkey_of && id < s->nsh; id++) {
        const char* k = key_of(s, id);
        Branch* bm = k ? block_map(s, t, k) : NULL, *x;
        shadow* h = &s->sh[id];
        char* js;

        if (!bm || !h->valid || id >= s->d->captab || !s->d->tab[id]) {
            continue;   /* freed blocks never come back */
        }

        if ((js = map_str(bm, t, "p")) != NULL) {
            if (h->props && strcmp(js, h->props) != 0) {
                fprintf(stderr, "CHECK %llx after %s: %s (%u, %s) props shadow differs\n", (unsigned long long)s->client,
                        where, k, id, pd_doc_blk(s->d, id) ? "alive" : "detached");
            }

            free(js);
        }

        if ((x = map_branch(bm, t, "t", Y_TEXT)) != NULL && h->sig) {
            pstate p;

            if (ps_crdt(s, x, t, &p) == 0) {
                if (p.n != h->len || memcmp(p.t, h->text, p.n) != 0 || memcmp(p.sig, h->sig, p.n * sizeof(int32_t)) != 0) {
                    fprintf(stderr, "CHECK %llx after %s: %s (%u, %s) text shadow differs\n", (unsigned long long)s->client,
                            where, k, id, pd_doc_blk(s->d, id) ? "alive" : "detached");
                }

                ps_free(&p);
            }
        }
    }

    ytransaction_commit(t);
#endif
}

/* ------------------------------------------------------------------ */
/* public                                                             */
/* ------------------------------------------------------------------ */

pd_status pd_sync_new(pd_doc* d, uint64_t client, pd_sync** out) {
    pd_sync* s;
    YOptions o;
    YTransaction* t;

    if (!d || !out || d->sync_fn) {
        return d && d->sync_fn ? PD_ERR_STATE : PD_ERR_ARG;
    }

    if ((s = (pd_sync*)calloc(1, sizeof(pd_sync))) == NULL) {
        return PD_ERR_NOMEM;
    }

    if (!client) {      /* 53 bits, as yrs wants */
        client = ((uint64_t)time(NULL) * 2654435761u ^ (uint64_t)(uintptr_t)s * 40503u ^ (uint64_t)clock() << 20) &
                 ((1ull << 53) - 1);
    }

    o = yoptions();
    o.id = client & ((1ull << 53) - 1);
    o.flags = Y_OFFSET_BYTES;
    s->y = ydoc_new_with_options(o);
    s->d = d;
    s->client = o.id;

    if (!s->y) {
        free(s);
        return PD_ERR_NOMEM;
    }

    s->blocks = ymap(s->y, "blocks");     /* root types first: asking for one inside a transaction deadlocks */
    s->styles = ymap(s->y, "styles");
    s->lists = ymap(s->y, "lists");
    s->res = ymap(s->y, "res");
    yobserve_deep(s->blocks, 1, "b", s, on_blocks);
    yobserve_deep(s->styles, 1, "s", s, on_styles);
    t = ydoc_write_transaction(s->y, 0, NULL);     /* subscribing needs one that writes */
    ytransaction_observe_updates_v1(t, 1, "u", s, on_update);
    ytransaction_commit(t);

    if (map_set(s, "root", PD_ROOT_ID) || map_set(s, "stories", PD_STORYROOT_ID)) {
        pd_sync_free(s);
        return PD_ERR_NOMEM;
    }

    d->sync_fn = on_doc_change;
    d->sync_user = s;
    *out = s;
    return PD_OK;
}

void pd_sync_free(pd_sync* s) {
    uint32_t i;

    if (!s) {
        return;
    }

    if (s->d && s->d->sync_user == s) {
        s->d->sync_fn = NULL;
        s->d->sync_user = NULL;
    }

    if (s->y) {
        yunobserve_deep(s->blocks, 1, "b");
        yunobserve_deep(s->styles, 1, "s");
        ydoc_destroy(s->y);
    }

    for (i = 0; i < s->nsh; i++) {
        sh_clear(&s->sh[i]);
    }

    for (i = 0; i < s->nsigs; i++) {
        free(s->sigs[i]);
    }

    free(s->sh);
    free(s->key_of);
    free(s->ht);
    free(s->sigs);
    free(s->sigobj);
    free(s->sfmt);
    free(s->sht);
    free(s->fsig);
    free(s->list_key);
    free(s->res_key);
    free(s->dk);
    free(s->dp);
    free(s->dt);
    free(s->ds);
    free(s->chg);
    free(s);
}

pd_status pd_sync_publish(pd_sync* s) {
    YTransaction* t;
    pd_style_id st;
    const char* roots[2] = { "root", "stories" };
    int i;

    if (!s) {
        return PD_ERR_ARG;
    }

    t = ydoc_write_transaction(s->y, 0, NULL);

    for (i = 0; i < 2; i++) {   /* the two roots, then everything under them */
        char* keys[3] = { "k", "p", "kids" };
        YInput vals[3], m, none[1];

        vals[0] = yinput_long(PD_BLOCK_ROOT);
        vals[1] = yinput_string("{}");
        vals[2] = yinput_yarray(none, 0);
        m = yinput_ymap(keys, vals, 3);
        ymap_insert(s->blocks, t, roots[i], &m);
        sh_clear(&s->sh[i ? PD_STORYROOT_ID : PD_ROOT_ID]);
        s->sh[i ? PD_STORYROOT_ID : PD_ROOT_ID].valid = 1;
        s->sh[i ? PD_STORYROOT_ID : PD_ROOT_ID].props = dupn("{}", 2);
    }

    ytransaction_commit(t);

    for (st = 1; (int32_t)st <= pd_doc_style_count(s->d) + 1; st++) {
        if (pd_doc_style_name(s->d, st)) {
            push(&s->ds, &s->nds, &s->capds, st);
        }
    }

    push(&s->dk, &s->ndk, &s->capdk, PD_STORYROOT_ID);
    push(&s->dk, &s->ndk, &s->capdk, PD_ROOT_ID);
    share(s);
    return PD_OK;
}

void pd_sync_set_sender(pd_sync* s, pd_sync_send_fn fn, void* user) {
    if (s) {
        s->send = fn;
        s->send_user = user;
    }
}

pd_status pd_sync_receive(pd_sync* s, const void* data, size_t len) {
    YTransaction* t;
    uint8_t rc;

    if (!s || (!data && len) || len > UINT32_MAX || s->d->in_op || s->d->group_depth > 0) {
        return PD_ERR_ARG;
    }

    s->remote = 1;
    t = ydoc_write_transaction(s->y, 0, NULL);
    rc = ytransaction_apply(t, (const char*)data, (uint32_t)len);
    ytransaction_commit(t);
    s->remote = 0;

    if (rc) {
        s->nchg = 0;
        return PD_ERR_FORMAT;
    }

    if (s->nchg || s->styles_changed) {
        reconcile(s);
    }

    shadows_check(s, "receive");
    return PD_OK;
}

static pd_status copy_out(char* p, uint32_t n, void** out, size_t* len) {
    void* r = malloc(n ? n : 1);

    if (!r) {
        ybinary_destroy(p, n);
        return PD_ERR_NOMEM;
    }

    memcpy(r, p, n);
    ybinary_destroy(p, n);
    *out = r;
    *len = n;
    return PD_OK;
}

pd_status pd_sync_state_vector(pd_sync* s, void** out, size_t* len) {
    YTransaction* t;
    uint32_t n = 0;
    char* p;

    if (!s || !out || !len) {
        return PD_ERR_ARG;
    }

    t = ydoc_read_transaction(s->y);
    p = ytransaction_state_vector_v1(t, &n);
    ytransaction_commit(t);
    return p ? copy_out(p, n, out, len) : PD_ERR_NOMEM;
}

pd_status pd_sync_diff(pd_sync* s, const void* sv, size_t sv_len, void** out, size_t* len) {
    YTransaction* t;
    uint32_t n = 0;
    char* p;

    if (!s || !out || !len || sv_len > UINT32_MAX) {
        return PD_ERR_ARG;
    }

    t = ydoc_read_transaction(s->y);
    p = ytransaction_state_diff_v1(t, (const char*)sv, sv ? (uint32_t)sv_len : 0, &n);
    ytransaction_commit(t);
    return p ? copy_out(p, n, out, len) : PD_ERR_FORMAT;
}

void pd_sync_free_data(void* data) {
    free(data);
}

/* ------------------------------------------------------------------ */
/* dump                                                               */
/* ------------------------------------------------------------------ */

static void dump_text(pd_sync* s, pd_buf* o, const pstate* p, int depth) {
    uint32_t i, j;

    for (i = 0; i < p->n; i = j) {
        const char* g;

        for (j = i + 1; j < p->n && p->sig[j] == p->sig[i]; j++) {
        }

        pb_printf(o, "%*s  |%.*s| ", depth * 2, "", (int)(j - i), p->t + i);

        for (g = s->sigs[p->sig[i]]; *g; g++) {
            pb_put(o, *g == '\x01' ? "=" : *g == '\x02' ? ";" : g, 1);
        }

        pb_puts(o, "\n");
    }
}

/* a block not shared and holding nothing: the placeholder of an emptied container */
static int placeholder(pd_sync* s, const blk* b, int depth) {
    int32_t i;

    if (key_of(s, b->id) || depth > 64 || (b->kind == PD_BLOCK_PARAGRAPH && b->st.len > 0)) {
        return 0;
    }

    for (i = 0; i < b->nkids; i++) {
        const blk* c = pd_doc_blk(s->d, b->kids[i]);

        if (c && !placeholder(s, c, depth + 1)) {
            return 0;
        }
    }

    return 1;
}

static void dump_doc(pd_sync* s, pd_buf* o, pd_block_id id, int depth) {
    blk* b = pd_doc_blk(s->d, id);
    const char* k = key_of(s, id);
    char* pj;
    int32_t i;

    if (!b || depth > 64 || placeholder(s, b, 0)) {
        return;
    }

    pj = props_json(s, b, NULL);
    pb_printf(o, "%*s%s %d %s\n", depth * 2, "", k ? k : "?", (int)b->kind, pj ? pj : "");
    free(pj);

    if (b->kind == PD_BLOCK_PARAGRAPH) {
        pstate p;

        if (ps_doc(s, b, &p, NULL) == 0) {
            dump_text(s, o, &p, depth);
            ps_free(&p);
        }
    }

    for (i = 0; i < b->nkids; i++) {
        dump_doc(s, o, b->kids[i], depth + 1);
    }
}

static void dump_crdt(pd_sync* s, const YTransaction* t, pd_buf* o, const char* k, int depth) {
    Branch* bm = block_map(s, t, k), *x;
    char* pj;

    if (!bm || depth > 64) {
        return;
    }

    pj = map_str(bm, t, "p");
    pb_printf(o, "%*s%s %d %s\n", depth * 2, "", k, (int)map_int(bm, t, "k", -1), pj ? pj : "");
    free(pj);

    if ((x = map_branch(bm, t, "t", Y_TEXT)) != NULL) {
        pstate p;

        if (ps_crdt(s, x, t, &p) == 0) {
            dump_text(s, o, &p, depth);
            ps_free(&p);
        }
    }

    if ((x = map_branch(bm, t, "kids", Y_ARRAY)) != NULL) {
        int32_t n = 0, i;
        skey* kids = kids_read(x, t, &n);

        for (i = 0; kids && i < n; i++) {
            if (!list_has(kids, i, kids[i].k)) {     /* as the document shows it: a key once */
                dump_crdt(s, t, o, kids[i].k, depth + 1);
            }
        }

        free(kids);
    }
}

char* pd_sync_dump(pd_sync* s, int from_shared) {
    pd_buf o;

    if (!s) {
        return NULL;
    }

    memset(&o, 0, sizeof(o));

    if (from_shared) {
        YTransaction* t = ydoc_read_transaction(s->y);

        dump_crdt(s, t, &o, "stories", 0);
        dump_crdt(s, t, &o, "root", 0);
        ytransaction_commit(t);
    } else {
        dump_doc(s, &o, PD_STORYROOT_ID, 0);
        dump_doc(s, &o, PD_ROOT_ID, 0);
    }

    return pb_take(&o);
}
