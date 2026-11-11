/*
 * Parade PDF reader: objects, cross-reference, streams, filters, security
 *
 * The file is held in memory. Objects are parsed when first asked for and
 * kept, in an arena that lives as long as the document. The cross-reference
 * comes from the file's tables and streams (an update's sections first, so
 * the newest wins); a file whose tables are wrong or missing is read again
 * by scanning it for "n g obj". Strings and streams of an encrypted file are
 * decrypted as they are read (RC4, AES-128, AES-256: the standard security
 * handler, revisions 2 to 6).
 */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr.h"

#define MAX_DEPTH 64

/* ------------------------------------------------------------------ */
/* arena and buffers                                                  */
/* ------------------------------------------------------------------ */

void* pa_alloc(parena** a, size_t n) {
    parena* c = *a;

    n = (n + 7) & ~(size_t)7;

    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 ? n : 65536;

        c = (parena*)malloc(sizeof(parena) + cap);

        if (!c) {
            return NULL;
        }

        c->next = *a;
        c->used = 0;
        c->cap = cap;
        *a = c;
    }

    c->used += n;
    memset(c->mem + c->used - n, 0, n);
    return c->mem + c->used - n;
}

char* pa_strdup(parena** a, const char* s, size_t n) {
    char* r = (char*)pa_alloc(a, n + 1);

    if (r) {
        memcpy(r, s, n);
        r[n] = 0;
    }

    return r;
}

void pa_free(parena* a) {
    while (a) {
        parena* n = a->next;

        free(a);
        a = n;
    }
}

void qb_put(pbuf* b, const void* d, size_t n) {
    if (b->err) {
        return;
    }

    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        unsigned char* q;

        while (cap < b->n + n + 1) {
            cap *= 2;
        }

        q = (unsigned char*)realloc(b->p, cap);

        if (!q) {
            b->err = 1;
            return;
        }

        b->p = q;
        b->cap = cap;
    }

    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}

void qb_puts(pbuf* b, const char* s) {
    qb_put(b, s, strlen(s));
}

void qb_printf(pbuf* b, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) {
        qb_put(b, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

void qb_num(pbuf* b, double v) {
    char tmp[64];
    int n;

    if (!(v == v) || fabs(v) > 1e12) {
        v = 0;
    }

    n = snprintf(tmp, sizeof(tmp), "%.4f", v);

    while (n > 0 && tmp[n - 1] == '0') {
        n--;
    }

    if (n > 0 && tmp[n - 1] == '.') {
        n--;
    }

    tmp[n] = 0;

    if (strcmp(tmp, "-0") == 0) {
        strcpy(tmp, "0");
    }

    qb_puts(b, tmp);
}

void qb_free(pbuf* b) {
    free(b->p);
    memset(b, 0, sizeof(*b));
}

/* ------------------------------------------------------------------ */
/* values                                                             */
/* ------------------------------------------------------------------ */

pv pv_null(void) {
    pv v;

    memset(&v, 0, sizeof(v));
    return v;
}

pv pv_int(int64_t i) {
    pv v = pv_null();

    v.t = PV_INT;
    v.u.i = i;
    return v;
}

pv pv_real(double r) {
    pv v = pv_null();

    v.t = PV_REAL;
    v.u.r = r;
    return v;
}

pv pv_bool(int b) {
    pv v = pv_null();

    v.t = PV_BOOL;
    v.u.i = b != 0;
    return v;
}

pv pv_name(pdoc* d, const char* s) {
    pv v = pv_null();

    v.t = PV_NAME;
    v.n = (int32_t)strlen(s);
    v.u.s = pa_strdup(&d->arena, s, (size_t)v.n);
    return v;
}

pv pv_string(pdoc* d, const void* s, size_t n) {
    pv v = pv_null();

    v.t = PV_STRING;
    v.n = (int32_t)n;
    v.u.s = pa_strdup(&d->arena, (const char*)s, n);
    return v;
}

pv pv_ref(int32_t num, int32_t gen) {
    pv v = pv_null();

    v.t = PV_REF;
    v.u.i = num;
    v.n = gen;
    return v;
}

pv pv_array(pdoc* d, int32_t n) {
    pv v = pv_null();

    v.t = PV_ARRAY;
    v.n = n;
    v.u.a = n ? (pv*)pa_alloc(&d->arena, sizeof(pv) * (size_t)n) : NULL;
    return v;
}

pv pv_dict(pdoc* d, int32_t n) {
    pv v = pv_null();

    v.t = PV_DICT;
    v.n = 0;
    v.u.d = (pkv*)pa_alloc(&d->arena, sizeof(pkv) * (size_t)(n > 0 ? n : 1));
    return v;
}

int pv_is_name(pv v, const char* s) {
    return v.t == PV_NAME && strcmp(v.u.s, s) == 0;
}

pkv* pv_entries(pv v, int32_t* n) {
    if (v.t == PV_DICT) {
        *n = v.n;
        return v.u.d;
    }

    if (v.t == PV_STREAM && v.u.st) {
        *n = v.u.st->nd;
        return v.u.st->d;
    }

    *n = 0;
    return NULL;
}

pv pd_get_raw(pv dict, const char* key) {
    int32_t n, i;
    pkv* e = pv_entries(dict, &n);

    for (i = 0; i < n; i++) {
        if (strcmp(e[i].key, key) == 0) {
            return e[i].val;
        }
    }

    return pv_null();
}

pv pd_resolve(pdoc* d, pv v) {
    int k;

    for (k = 0; k < 8 && v.t == PV_REF; k++) {
        v = pd_object(d, (int32_t)v.u.i);
    }

    return v.t == PV_REF ? pv_null() : v;
}

pv pd_get(pdoc* d, pv dict, const char* key) {
    return pd_resolve(d, pd_get_raw(pd_resolve(d, dict), key));
}

pv pd_index(pdoc* d, pv arr, int32_t i) {
    arr = pd_resolve(d, arr);

    if (arr.t != PV_ARRAY || i < 0 || i >= arr.n) {
        return pv_null();
    }

    return pd_resolve(d, arr.u.a[i]);
}

int pd_num(pdoc* d, pv v, double* out) {
    v = pd_resolve(d, v);

    if (v.t == PV_INT) {
        *out = (double)v.u.i;
        return 1;
    }

    if (v.t == PV_REAL) {
        *out = v.u.r;
        return 1;
    }

    return 0;
}

double pd_getnum(pdoc* d, pv dict, const char* key, double dflt) {
    double r;

    return pd_num(d, pd_get(d, dict, key), &r) ? r : dflt;
}

int64_t pd_getint(pdoc* d, pv dict, const char* key, int64_t dflt) {
    pv v = pd_get(d, dict, key);

    if (v.t == PV_INT) {
        return v.u.i;
    }

    if (v.t == PV_REAL) {
        return (int64_t)v.u.r;
    }

    if (v.t == PV_BOOL) {
        return v.u.i;
    }

    return dflt;
}

const char* pd_getname(pdoc* d, pv dict, const char* key) {
    pv v = pd_get(d, dict, key);

    return v.t == PV_NAME ? v.u.s : NULL;
}

int pd_rect(pdoc* d, pv v, double* r) {
    int i;

    v = pd_resolve(d, v);

    if (v.t != PV_ARRAY || v.n < 4) {
        return 0;
    }

    for (i = 0; i < 4; i++) {
        if (!pd_num(d, v.u.a[i], &r[i])) {
            return 0;
        }
    }

    if (r[0] > r[2]) {
        double t = r[0];

        r[0] = r[2];
        r[2] = t;
    }

    if (r[1] > r[3]) {
        double t = r[1];

        r[1] = r[3];
        r[3] = t;
    }

    return 1;
}

int pd_matrix(pdoc* d, pv v, double* m) {
    int i;

    v = pd_resolve(d, v);

    if (v.t != PV_ARRAY || v.n < 6) {
        return 0;
    }

    for (i = 0; i < 6; i++) {
        if (!pd_num(d, v.u.a[i], &m[i])) {
            return 0;
        }
    }

    return 1;
}

void pd_dict_set(pdoc* d, pv* dict, const char* key, pv val) {
    int32_t n, i;
    pkv* e = pv_entries(*dict, &n), *ne;

    for (i = 0; i < n; i++) {
        if (strcmp(e[i].key, key) == 0) {
            e[i].val = val;
            return;
        }
    }

    ne = (pkv*)pa_alloc(&d->arena, sizeof(pkv) * (size_t)(n + 1));

    if (!ne) {
        return;
    }

    if (n) {
        memcpy(ne, e, sizeof(pkv) * (size_t)n);
    }

    ne[n].key = pa_strdup(&d->arena, key, strlen(key));
    ne[n].val = val;

    if (dict->t == PV_DICT) {
        dict->u.d = ne;
        dict->n = n + 1;
    } else if (dict->t == PV_STREAM) {
        dict->u.st->d = ne;
        dict->u.st->nd = n + 1;
    }
}

void pd_dict_del(pv* dict, const char* key) {
    int32_t n, i;
    pkv* e = pv_entries(*dict, &n);

    for (i = 0; i < n; i++) {
        if (strcmp(e[i].key, key) == 0) {
            memmove(&e[i], &e[i + 1], sizeof(pkv) * (size_t)(n - i - 1));

            if (dict->t == PV_DICT) {
                dict->n--;
            } else {
                dict->u.st->nd--;
            }

            return;
        }
    }
}

pv pd_array_add(pdoc* d, pv arr, pv item) {
    pv r = pv_array(d, (arr.t == PV_ARRAY ? arr.n : 0) + 1);

    if (arr.t == PV_ARRAY && arr.n) {
        memcpy(r.u.a, arr.u.a, sizeof(pv) * (size_t)arr.n);
    }

    r.u.a[r.n - 1] = item;
    return r;
}

/* ------------------------------------------------------------------ */
/* lexer                                                              */
/* ------------------------------------------------------------------ */

static int is_ws(int c) {
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static int is_delim(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}

void pl_skip_ws(plex* L) {
    while (L->i < L->n) {
        int c = L->p[L->i];

        if (is_ws(c)) {
            L->i++;
        } else if (c == '%') {
            while (L->i < L->n && L->p[L->i] != 10 && L->p[L->i] != 13) {
                L->i++;
            }
        } else {
            break;
        }
    }
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static pv err_val(void) {
    pv v = pv_null();

    v.t = PV_ERR;
    return v;
}

static pv lex_string(plex* L) {
    pbuf b;
    int depth = 1;
    pv v;

    memset(&b, 0, sizeof(b));
    L->i++;

    while (L->i < L->n) {
        int c = L->p[L->i++];
        unsigned char ch;

        if (c == '(') {
            depth++;
        } else if (c == ')') {
            if (--depth == 0) {
                break;
            }
        } else if (c == '\\') {
            if (L->i >= L->n) {
                break;
            }

            c = L->p[L->i++];

            switch (c) {
                case 'n': c = 10; break;
                case 'r': c = 13; break;
                case 't': c = 9; break;
                case 'b': c = 8; break;
                case 'f': c = 12; break;
                case 13:
                    if (L->i < L->n && L->p[L->i] == 10) {
                        L->i++;
                    }

                    continue;

                case 10:
                    continue;

                default:
                    if (c >= '0' && c <= '7') {
                        int k, o = c - '0';

                        for (k = 0; k < 2 && L->i < L->n && L->p[L->i] >= '0' && L->p[L->i] <= '7'; k++) {
                            o = o * 8 + (L->p[L->i++] - '0');
                        }

                        c = o & 255;
                    }
            }
        } else if (c == 13) {   /* an end of line in a string is a line feed */
            if (L->i < L->n && L->p[L->i] == 10) {
                L->i++;
            }

            c = 10;
        }

        ch = (unsigned char)c;
        qb_put(&b, &ch, 1);
    }

    v = pv_string(L->d, b.p ? (const char*)b.p : "", b.n);
    qb_free(&b);
    return v;
}

static pv lex_hex(plex* L) {
    pbuf b;
    int hi = -1;
    pv v;

    memset(&b, 0, sizeof(b));
    L->i++;

    while (L->i < L->n) {
        int c = L->p[L->i++], h;

        if (c == '>') {
            break;
        }

        h = hexval(c);

        if (h < 0) {
            continue;
        }

        if (hi < 0) {
            hi = h;
        } else {
            unsigned char ch = (unsigned char)(hi * 16 + h);

            qb_put(&b, &ch, 1);
            hi = -1;
        }
    }

    if (hi >= 0) {
        unsigned char ch = (unsigned char)(hi * 16);

        qb_put(&b, &ch, 1);
    }

    v = pv_string(L->d, b.p ? (const char*)b.p : "", b.n);
    v.hex = 1;
    qb_free(&b);
    return v;
}

static pv lex_name(plex* L) {
    char tmp[256];
    size_t k = 0;

    L->i++;

    while (L->i < L->n) {
        int c = L->p[L->i];

        if (is_ws(c) || is_delim(c)) {
            break;
        }

        L->i++;

        if (c == '#' && L->i + 1 < L->n && hexval(L->p[L->i]) >= 0 && hexval(L->p[L->i + 1]) >= 0) {
            c = hexval(L->p[L->i]) * 16 + hexval(L->p[L->i + 1]);
            L->i += 2;
        }

        if (k < sizeof(tmp) - 1) {
            tmp[k++] = (char)c;
        }
    }

    tmp[k] = 0;
    return pv_name(L->d, tmp);
}

static pv read_value(plex* L, int depth);

/* a word: a number, true/false/null, or a keyword */
static pv lex_word(plex* L) {
    size_t s = L->i;
    char tmp[64];
    size_t k;
    int c = L->p[L->i];

    if (c == '+' || c == '-' || c == '.' || (c >= '0' && c <= '9')) {
        int dot = 0, digits = 0;
        int64_t iv = 0;
        int neg = 0;
        double frac = 0, scale = 1;

        while (L->i < L->n && (L->p[L->i] == '+' || L->p[L->i] == '-')) {
            neg ^= L->p[L->i] == '-';
            L->i++;
        }

        while (L->i < L->n) {
            c = L->p[L->i];

            if (c >= '0' && c <= '9') {
                digits++;

                if (dot) {
                    scale *= 10;
                    frac = frac * 10 + (c - '0');
                } else if (iv < ((int64_t)1 << 56)) {
                    iv = iv * 10 + (c - '0');
                }
            } else if (c == '.' && !dot) {
                dot = 1;
            } else {
                break;
            }

            L->i++;
        }

        if (L->i < L->n && !is_ws(L->p[L->i]) && !is_delim(L->p[L->i])) {
            /* "1e5", "12abc": not a number after all; take the rest as a word below */
            while (L->i < L->n && !is_ws(L->p[L->i]) && !is_delim(L->p[L->i])) {
                L->i++;
            }

            if (digits) {
                return pv_real(atof((const char*)L->p + s));
            }
        } else if (digits) {
            if (dot) {
                double r = (double)iv + frac / scale;

                return pv_real(neg ? -r : r);
            }

            return pv_int(neg ? -iv : iv);
        } else {
            return pv_int(0);
        }
    }

    while (L->i < L->n && !is_ws(L->p[L->i]) && !is_delim(L->p[L->i])) {
        L->i++;
    }

    k = L->i - s;

    if (k == 0) {   /* a stray delimiter: skip it */
        L->i++;
        return pl_read(L);
    }

    if (k > sizeof(tmp) - 1) {
        k = sizeof(tmp) - 1;
    }

    memcpy(tmp, L->p + s, k);
    tmp[k] = 0;

    if (strcmp(tmp, "true") == 0) {
        return pv_bool(1);
    }

    if (strcmp(tmp, "false") == 0) {
        return pv_bool(0);
    }

    if (strcmp(tmp, "null") == 0) {
        return pv_null();
    }

    {
        pv v = pv_null();

        v.t = PV_OP;
        v.n = (int32_t)k;
        v.u.s = pa_strdup(&L->d->arena, tmp, k);
        return v;
    }
}

pv pl_read(plex* L) {
    return read_value(L, 0);
}

static pv read_value(plex* L, int depth) {
    int c;

    pl_skip_ws(L);

    if (L->i >= L->n) {
        return err_val();
    }

    c = L->p[L->i];

    if (c == '(') {
        return lex_string(L);
    }

    if (c == '/') {
        return lex_name(L);
    }

    if (c == '<') {
        if (L->i + 1 < L->n && L->p[L->i + 1] == '<') {    /* a dictionary */
            pbuf kv;
            pv dict;
            int32_t cnt = 0;

            memset(&kv, 0, sizeof(kv));
            L->i += 2;

            for (;;) {
                pv k, v;
                pkv e;

                pl_skip_ws(L);

                if (L->i >= L->n) {
                    break;
                }

                if (L->p[L->i] == '>') {
                    L->i += L->i + 1 < L->n && L->p[L->i + 1] == '>' ? 2 : 1;
                    break;
                }

                k = depth < MAX_DEPTH ? read_value(L, depth + 1) : err_val();

                if (k.t == PV_ERR) {
                    break;
                }

                if (k.t != PV_NAME) {
                    if (k.t == PV_OP && (strcmp(k.u.s, "endobj") == 0 || strcmp(k.u.s, "stream") == 0)) {
                        L->i -= (size_t)k.n;    /* a dictionary never closed */
                        break;
                    }

                    continue;
                }

                pl_skip_ws(L);

                if (L->i < L->n && L->p[L->i] == '>') {
                    v = pv_null();
                } else {
                    v = read_value(L, depth + 1);
                }

                if (v.t == PV_ERR) {
                    break;
                }

                if (v.t == PV_OP) {     /* /Key endobj: a value missing */
                    L->i -= (size_t)v.n;
                    v = pv_null();
                }

                e.key = k.u.s;
                e.val = v;
                qb_put(&kv, &e, sizeof(e));
                cnt++;
            }

            dict = pv_dict(L->d, cnt);

            if (cnt && kv.p) {
                memcpy(dict.u.d, kv.p, sizeof(pkv) * (size_t)cnt);
            }

            dict.n = cnt;
            qb_free(&kv);
            return dict;
        }

        return lex_hex(L);
    }

    if (c == '[') {
        pbuf items;
        pv arr;
        int32_t cnt = 0;

        memset(&items, 0, sizeof(items));
        L->i++;

        for (;;) {
            pv v;

            pl_skip_ws(L);

            if (L->i >= L->n) {
                break;
            }

            if (L->p[L->i] == ']') {
                L->i++;
                break;
            }

            v = depth < MAX_DEPTH ? read_value(L, depth + 1) : err_val();

            if (v.t == PV_ERR) {
                break;
            }

            if (v.t == PV_OP && !L->content) {
                if (strcmp(v.u.s, "R") == 0 && cnt >= 2) {
                    pv* a = (pv*)items.p;

                    if (a[cnt - 2].t == PV_INT && a[cnt - 1].t == PV_INT) {
                        a[cnt - 2] = pv_ref((int32_t)a[cnt - 2].u.i, (int32_t)a[cnt - 1].u.i);
                        items.n -= sizeof(pv);
                        cnt--;
                    }

                    continue;
                }

                if (strcmp(v.u.s, "endobj") == 0) {
                    L->i -= (size_t)v.n;
                    break;
                }

                continue;
            }

            qb_put(&items, &v, sizeof(v));
            cnt++;
        }

        arr = pv_array(L->d, cnt);

        if (cnt && items.p) {
            memcpy(arr.u.a, items.p, sizeof(pv) * (size_t)cnt);
        }

        qb_free(&items);
        return arr;
    }

    if (c == ']' || c == '>' || c == ')') {
        pv v = pv_null();

        L->i++;
        v.t = PV_OP;
        v.n = 1;
        v.u.s = c == ']' ? "]" : c == '>' ? ">" : ")";
        return v;
    }

    if (c == '{' || c == '}') {
        pv v = pv_null();

        L->i++;
        v.t = PV_OP;
        v.n = 1;
        v.u.s = c == '{' ? "{" : "}";
        return v;
    }

    {
        pv v = lex_word(L);

        /* n g R outside an array (a dictionary's value, an object's) */
        if (v.t == PV_INT && !L->content && v.u.i >= 0) {
            size_t save = L->i;
            plex M = *L;
            pv g, r;

            pl_skip_ws(&M);

            if (M.i < M.n && M.p[M.i] >= '0' && M.p[M.i] <= '9') {
                g = lex_word(&M);
                pl_skip_ws(&M);

                if (g.t == PV_INT && M.i < M.n && M.p[M.i] == 'R' &&
                    (M.i + 1 >= M.n || is_ws(M.p[M.i + 1]) || is_delim(M.p[M.i + 1]))) {
                    L->i = M.i + 1;
                    r = pv_ref((int32_t)v.u.i, (int32_t)g.u.i);
                    return r;
                }
            }

            L->i = save;
        }

        return v;
    }
}

/* ------------------------------------------------------------------ */
/* text strings                                                       */
/* ------------------------------------------------------------------ */

static const uint16_t pdfdoc_hi[32] = {     /* 0x80..0x9F */
    0x2022, 0x2020, 0x2021, 0x2026, 0x2014, 0x2013, 0x0192, 0x2044, 0x2039, 0x203A, 0x2212, 0x2030, 0x201E,
    0x201C, 0x201D, 0x2018, 0x2019, 0x201A, 0x2122, 0xFB01, 0xFB02, 0x0141, 0x0152, 0x0160, 0x0178, 0x017D,
    0x0131, 0x0142, 0x0153, 0x0161, 0x017E, 0xFFFD
};

static const uint16_t pdfdoc_lo[8] = { 0x02D8, 0x02C7, 0x02C6, 0x02D9, 0x02DD, 0x02DB, 0x02DA, 0x02DC };  /* 0x18.. */

static size_t put_utf8(char* o, uint32_t c) {
    if (c < 0x80) {
        o[0] = (char)c;
        return 1;
    }

    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 63));
        return 2;
    }

    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12));
        o[1] = (char)(0x80 | ((c >> 6) & 63));
        o[2] = (char)(0x80 | (c & 63));
        return 3;
    }

    o[0] = (char)(0xF0 | (c >> 18));
    o[1] = (char)(0x80 | ((c >> 12) & 63));
    o[2] = (char)(0x80 | ((c >> 6) & 63));
    o[3] = (char)(0x80 | (c & 63));
    return 4;
}

char* pd_text_utf8(pv s, size_t* n) {
    const unsigned char* p = (const unsigned char*)s.u.s;
    size_t len = s.t == PV_STRING || s.t == PV_NAME ? (size_t)s.n : 0, i, k = 0;
    char* o = (char*)malloc(len * 3 + 4);

    if (!o) {
        return NULL;
    }

    if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        for (i = 2; i + 1 < len; i += 2) {
            uint32_t c = ((uint32_t)p[i] << 8) | p[i + 1];

            if (c >= 0xD800 && c < 0xDC00 && i + 3 < len) {
                uint32_t c2 = ((uint32_t)p[i + 2] << 8) | p[i + 3];

                if (c2 >= 0xDC00 && c2 < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                    i += 2;
                }
            }

            if (c == 0x1B) {    /* a language escape: skip to its end */
                while (i + 1 < len && !(p[i] == 0 && p[i + 1] == 0x1B)) {
                    i += 2;
                }

                continue;
            }

            k += put_utf8(o + k, c);
        }
    } else if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        memcpy(o, p + 3, len - 3);
        k = len - 3;
    } else {
        for (i = 0; i < len; i++) {
            uint32_t c = p[i];

            if (c >= 0x80 && c < 0xA0) {
                c = pdfdoc_hi[c - 0x80];
            } else if (c >= 0x18 && c < 0x20) {
                c = pdfdoc_lo[c - 0x18];
            } else if (c == 0xAD) {
                c = 0x2D;
            }

            k += put_utf8(o + k, c);
        }
    }

    o[k] = 0;

    if (n) {
        *n = k;
    }

    return o;
}

pv pd_utf8_text(pdoc* d, const char* utf8) {
    const unsigned char* p = (const unsigned char*)utf8;
    size_t n = strlen(utf8), i, k = 2;
    int ascii = 1;
    unsigned char* o;
    pv v;

    for (i = 0; i < n; i++) {
        if (p[i] >= 0x80) {
            ascii = 0;
        }
    }

    if (ascii) {
        return pv_string(d, utf8, n);
    }

    o = (unsigned char*)malloc(n * 4 + 4);

    if (!o) {
        return pv_string(d, "", 0);
    }

    o[0] = 0xFE;
    o[1] = 0xFF;

    for (i = 0; i < n;) {
        uint32_t c = p[i];
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, j;

        c &= extra == 3 ? 7 : extra == 2 ? 15 : extra == 1 ? 31 : 127;

        for (j = 0, i++; j < extra && i < n; j++, i++) {
            c = (c << 6) | (p[i] & 63);
        }

        if (c >= 0x10000) {
            uint32_t h = 0xD800 + ((c - 0x10000) >> 10), l = 0xDC00 + ((c - 0x10000) & 1023);

            o[k++] = (unsigned char)(h >> 8);
            o[k++] = (unsigned char)h;
            o[k++] = (unsigned char)(l >> 8);
            o[k++] = (unsigned char)l;
        } else {
            o[k++] = (unsigned char)(c >> 8);
            o[k++] = (unsigned char)c;
        }
    }

    v = pv_string(d, o, k);
    free(o);
    return v;
}

/* ------------------------------------------------------------------ */
/* crypto: MD5, RC4, AES, SHA-2                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t s[4];
    uint64_t n;
    unsigned char b[64];
} md5_t;

#define ROTL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static void md5_block(md5_t* m, const unsigned char* p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };
    static const int R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9,
        14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10,
        15, 21, 6, 10, 15, 21
    };
    uint32_t w[16], a = m->s[0], b = m->s[1], c = m->s[2], dd = m->s[3];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) | ((uint32_t)p[4 * i + 2] << 16) |
               ((uint32_t)p[4 * i + 3] << 24);
    }

    for (i = 0; i < 64; i++) {
        uint32_t f, g, t;

        if (i < 16) {
            f = (b & c) | (~b & dd);
            g = (uint32_t)i;
        } else if (i < 32) {
            f = (dd & b) | (~dd & c);
            g = (uint32_t)(5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ dd;
            g = (uint32_t)(3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~dd);
            g = (uint32_t)(7 * i) & 15;
        }

        t = dd;
        dd = c;
        c = b;
        b = b + ROTL(a + f + K[i] + w[g], R[i]);
        a = t;
    }

    m->s[0] += a;
    m->s[1] += b;
    m->s[2] += c;
    m->s[3] += dd;
}

static void md5_init(md5_t* m) {
    m->s[0] = 0x67452301;
    m->s[1] = 0xefcdab89;
    m->s[2] = 0x98badcfe;
    m->s[3] = 0x10325476;
    m->n = 0;
}

static void md5_add(md5_t* m, const unsigned char* p, size_t n) {
    while (n) {
        size_t have = (size_t)(m->n & 63), take = 64 - have < n ? 64 - have : n;

        memcpy(m->b + have, p, take);
        m->n += take;
        p += take;
        n -= take;

        if ((m->n & 63) == 0) {
            md5_block(m, m->b);
        }
    }
}

static void md5_end(md5_t* m, unsigned char out[16]) {
    uint64_t bits = m->n * 8;
    unsigned char pad = 0x80, z = 0, len[8];
    int i;

    md5_add(m, &pad, 1);

    while ((m->n & 63) != 56) {
        md5_add(m, &z, 1);
    }

    for (i = 0; i < 8; i++) {
        len[i] = (unsigned char)(bits >> (8 * i));
    }

    md5_add(m, len, 8);

    for (i = 0; i < 16; i++) {
        out[i] = (unsigned char)(m->s[i / 4] >> (8 * (i % 4)));
    }
}

void pd_md5(const unsigned char* data, size_t n, unsigned char out[16]) {
    md5_t m;

    md5_init(&m);
    md5_add(&m, data, n);
    md5_end(&m, out);
}

void pd_rc4(const unsigned char* key, int keylen, unsigned char* data, size_t n) {
    unsigned char S[256];
    int i, j = 0;
    size_t k;

    for (i = 0; i < 256; i++) {
        S[i] = (unsigned char)i;
    }

    for (i = 0; i < 256; i++) {
        unsigned char t;

        j = (j + S[i] + key[i % keylen]) & 255;
        t = S[i];
        S[i] = S[j];
        S[j] = t;
    }

    i = j = 0;

    for (k = 0; k < n; k++) {
        unsigned char t;

        i = (i + 1) & 255;
        j = (j + S[i]) & 255;
        t = S[i];
        S[i] = S[j];
        S[j] = t;
        data[k] ^= S[(S[i] + S[j]) & 255];
    }
}

/* AES */
static unsigned char aes_sbox[256], aes_inv[256];

static unsigned char xtime(unsigned char x) {
    return (unsigned char)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static unsigned char gmul(unsigned char a, unsigned char b) {
    unsigned char p = 0;

    while (b) {
        if (b & 1) {
            p ^= a;
        }

        a = xtime(a);
        b >>= 1;
    }

    return p;
}

static void aes_tables(void) {
    int i;

    if (aes_sbox[0]) {
        return;
    }

    for (i = 0; i < 256; i++) {
        unsigned char inv = 0, s, x;
        int k;

        if (i) {    /* the multiplicative inverse, by search (once) */
            for (k = 1; k < 256; k++) {
                if (gmul((unsigned char)i, (unsigned char)k) == 1) {
                    inv = (unsigned char)k;
                    break;
                }
            }
        }

        s = x = inv;

        for (k = 0; k < 4; k++) {
            x = (unsigned char)((x << 1) | (x >> 7));
            s ^= x;
        }

        s ^= 0x63;
        aes_sbox[i] = s;
        aes_inv[s] = (unsigned char)i;
    }
}

typedef struct {
    unsigned char rk[240];
    int nr;
} aes_t;

static void aes_key(aes_t* a, const unsigned char* key, int bits) {
    int nk = bits / 32, i, total;
    unsigned char rcon = 1;

    aes_tables();
    a->nr = nk + 6;
    total = 4 * (a->nr + 1);
    memcpy(a->rk, key, (size_t)nk * 4);

    for (i = nk; i < total; i++) {
        unsigned char t[4];

        memcpy(t, a->rk + 4 * (i - 1), 4);

        if (i % nk == 0) {
            unsigned char u = t[0];

            t[0] = (unsigned char)(aes_sbox[t[1]] ^ rcon);
            t[1] = aes_sbox[t[2]];
            t[2] = aes_sbox[t[3]];
            t[3] = aes_sbox[u];
            rcon = xtime(rcon);
        } else if (nk > 6 && i % nk == 4) {
            int k;

            for (k = 0; k < 4; k++) {
                t[k] = aes_sbox[t[k]];
            }
        }

        {
            int k;

            for (k = 0; k < 4; k++) {
                a->rk[4 * i + k] = a->rk[4 * (i - nk) + k] ^ t[k];
            }
        }
    }
}

static void aes_encrypt_block(const aes_t* a, unsigned char* s) {
    int r, i;

    for (i = 0; i < 16; i++) {
        s[i] ^= a->rk[i];
    }

    for (r = 1; r <= a->nr; r++) {
        unsigned char t[16];

        for (i = 0; i < 16; i++) {  /* sub bytes and shift rows */
            int c = i / 4, row = i % 4;

            t[i] = aes_sbox[s[((c + row) % 4) * 4 + row]];
        }

        if (r != a->nr) {
            for (i = 0; i < 4; i++) {
                unsigned char* c = t + 4 * i, a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];

                c[0] = (unsigned char)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
                c[1] = (unsigned char)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
                c[2] = (unsigned char)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
                c[3] = (unsigned char)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
            }
        }

        for (i = 0; i < 16; i++) {
            s[i] = t[i] ^ a->rk[16 * r + i];
        }
    }
}

static void aes_decrypt_block(const aes_t* a, unsigned char* s) {
    int r, i;

    for (i = 0; i < 16; i++) {
        s[i] ^= a->rk[16 * a->nr + i];
    }

    for (r = a->nr - 1; r >= 0; r--) {
        unsigned char t[16];

        for (i = 0; i < 16; i++) {  /* inverse shift rows and sub bytes */
            int c = i / 4, row = i % 4;

            t[((c + row) % 4) * 4 + row] = aes_inv[s[i]];
        }

        for (i = 0; i < 16; i++) {
            t[i] ^= a->rk[16 * r + i];
        }

        if (r) {
            for (i = 0; i < 4; i++) {
                unsigned char* c = t + 4 * i, a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];

                c[0] = (unsigned char)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
                c[1] = (unsigned char)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
                c[2] = (unsigned char)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
                c[3] = (unsigned char)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
            }
        }

        memcpy(s, t, 16);
    }
}

/* CBC: the first 16 bytes of data are the IV; padding removed (PKCS#5) when pad says */
static unsigned char* aes_cbc_decrypt(const unsigned char* key, int bits, const unsigned char* data, size_t n,
                                      int pad, size_t* outlen) {
    aes_t a;
    unsigned char* o, iv[16];
    size_t i, m;

    if (n < 16) {
        *outlen = 0;
        return (unsigned char*)calloc(1, 1);
    }

    aes_key(&a, key, bits);
    memcpy(iv, data, 16);
    m = (n - 16) / 16 * 16;
    o = (unsigned char*)malloc(m + 1);

    if (!o) {
        return NULL;
    }

    for (i = 0; i < m; i += 16) {
        int k;

        memcpy(o + i, data + 16 + i, 16);
        aes_decrypt_block(&a, o + i);

        for (k = 0; k < 16; k++) {
            o[i + k] ^= iv[k];
        }

        memcpy(iv, data + 16 + i, 16);
    }

    if (pad && m >= 16 && o[m - 1] >= 1 && o[m - 1] <= 16) {
        m -= o[m - 1];
    }

    *outlen = m;
    return o;
}

static unsigned char* aes_cbc_encrypt(const unsigned char* key, int bits, const unsigned char* iv0,
                                      const unsigned char* data, size_t n, int pad, size_t* outlen) {
    aes_t a;
    size_t m = pad ? (n / 16 + 1) * 16 : (n + 15) / 16 * 16, i;
    unsigned char* o = (unsigned char*)malloc(m + 16), *prev;

    if (!o) {
        return NULL;
    }

    aes_key(&a, key, bits);
    memcpy(o, iv0, 16);
    memcpy(o + 16, data, n);
    memset(o + 16 + n, pad ? (int)(m - n) : 0, m - n);
    prev = o;

    for (i = 0; i < m; i += 16) {
        int k;

        for (k = 0; k < 16; k++) {
            o[16 + i + k] ^= prev[k];
        }

        aes_encrypt_block(&a, o + 16 + i);
        prev = o + 16 + i;
    }

    *outlen = m + 16;
    return o;
}

/* SHA-256 and SHA-384/512 */
static void sha256(const unsigned char* data, size_t n, unsigned char out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab,
                      0x5be0cd19 };
    size_t total = ((n + 9 + 63) / 64) * 64, off, i;
    unsigned char* m = (unsigned char*)calloc(total, 1);

    if (!m) {
        memset(out, 0, 32);
        return;
    }

    memcpy(m, data, n);
    m[n] = 0x80;

    for (i = 0; i < 8; i++) {
        m[total - 1 - i] = (unsigned char)(((uint64_t)n * 8) >> (8 * i));
    }

    for (off = 0; off < total; off += 64) {
        uint32_t w[64], a, b, c, d, e, f, g, hh;

        for (i = 0; i < 16; i++) {
            w[i] = ((uint32_t)m[off + 4 * i] << 24) | ((uint32_t)m[off + 4 * i + 1] << 16) |
                   ((uint32_t)m[off + 4 * i + 2] << 8) | m[off + 4 * i + 3];
        }

        for (i = 16; i < 64; i++) {
            uint32_t s0 = ((w[i - 15] >> 7) | (w[i - 15] << 25)) ^ ((w[i - 15] >> 18) | (w[i - 15] << 14)) ^
                          (w[i - 15] >> 3);
            uint32_t s1 = ((w[i - 2] >> 17) | (w[i - 2] << 15)) ^ ((w[i - 2] >> 19) | (w[i - 2] << 13)) ^
                          (w[i - 2] >> 10);

            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        a = h[0];
        b = h[1];
        c = h[2];
        d = h[3];
        e = h[4];
        f = h[5];
        g = h[6];
        hh = h[7];

        for (i = 0; i < 64; i++) {
            uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
            uint32_t ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;

            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    for (i = 0; i < 32; i++) {
        out[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
    }

    free(m);
}

static void sha512(const unsigned char* data, size_t n, unsigned char* out, int bits384) {
    static const uint64_t K[80] = {
        0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
        0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
        0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
        0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
        0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
        0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
        0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
        0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
        0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
        0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
        0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
        0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
        0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
        0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
        0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
        0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
        0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
        0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
        0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
        0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
    };
    uint64_t h[8];
    size_t total = ((n + 17 + 127) / 128) * 128, off, i;
    unsigned char* m = (unsigned char*)calloc(total, 1);

    if (bits384) {
        uint64_t h0[8] = { 0xcbbb9d5dc1059ed8ULL, 0x629a292a367cd507ULL, 0x9159015a3070dd17ULL,
                           0x152fecd8f70e5939ULL, 0x67332667ffc00b31ULL, 0x8eb44a8768581511ULL,
                           0xdb0c2e0d64f98fa7ULL, 0x47b5481dbefa4fa4ULL };

        memcpy(h, h0, sizeof(h));
    } else {
        uint64_t h0[8] = { 0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                           0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                           0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };

        memcpy(h, h0, sizeof(h));
    }

    if (!m) {
        memset(out, 0, bits384 ? 48 : 64);
        return;
    }

    memcpy(m, data, n);
    m[n] = 0x80;

    for (i = 0; i < 8; i++) {
        m[total - 1 - i] = (unsigned char)(((uint64_t)n * 8) >> (8 * i));
    }

#define R64(x, c) (((x) >> (c)) | ((x) << (64 - (c))))

    for (off = 0; off < total; off += 128) {
        uint64_t w[80], a, b, c, d, e, f, g, hh;

        for (i = 0; i < 16; i++) {
            int k;

            w[i] = 0;

            for (k = 0; k < 8; k++) {
                w[i] = (w[i] << 8) | m[off + 8 * i + (size_t)k];
            }
        }

        for (i = 16; i < 80; i++) {
            uint64_t s0 = R64(w[i - 15], 1) ^ R64(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = R64(w[i - 2], 19) ^ R64(w[i - 2], 61) ^ (w[i - 2] >> 6);

            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        a = h[0];
        b = h[1];
        c = h[2];
        d = h[3];
        e = h[4];
        f = h[5];
        g = h[6];
        hh = h[7];

        for (i = 0; i < 80; i++) {
            uint64_t S1 = R64(e, 14) ^ R64(e, 18) ^ R64(e, 41), ch = (e & f) ^ (~e & g);
            uint64_t t1 = hh + S1 + ch + K[i] + w[i], S0 = R64(a, 28) ^ R64(a, 34) ^ R64(a, 39);
            uint64_t mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;

            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    for (i = 0; i < (size_t)(bits384 ? 48 : 64); i++) {
        out[i] = (unsigned char)(h[i / 8] >> (56 - 8 * (i % 8)));
    }

    free(m);
}

/* ------------------------------------------------------------------ */
/* the standard security handler                                      */
/* ------------------------------------------------------------------ */

static const unsigned char pw_pad[32] = {
    0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E, 0x56, 0xFF, 0xFA, 0x01, 0x08,
    0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68, 0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A
};

static void pad_password(const char* pw, unsigned char out[32]) {
    size_t n = pw ? strlen(pw) : 0;

    if (n > 32) {
        n = 32;
    }

    memcpy(out, pw, n);
    memcpy(out + n, pw_pad, 32 - n);
}

static pv str_of(pdoc* d, pv dict, const char* key) {
    pv v = pd_get(d, dict, key);

    return v.t == PV_STRING ? v : pv_null();
}

/* algorithm 2: the file key from a (user) password, revisions 2 to 4 */
static void key_r4(pdoc* d, pv enc, const unsigned char* padded, unsigned char* key, int len) {
    md5_t m;
    unsigned char h[16];
    pv o = str_of(d, enc, "O"), id = pd_index(d, pd_get(d, d->trailer, "ID"), 0);
    int32_t p = (int32_t)pd_getint(d, enc, "P", 0);
    unsigned char pb[4];
    int i;

    md5_init(&m);
    md5_add(&m, padded, 32);

    if (o.t == PV_STRING) {
        md5_add(&m, (const unsigned char*)o.u.s, (size_t)(o.n < 32 ? o.n : 32));
    }

    for (i = 0; i < 4; i++) {
        pb[i] = (unsigned char)((uint32_t)p >> (8 * i));
    }

    md5_add(&m, pb, 4);

    if (id.t == PV_STRING) {
        md5_add(&m, (const unsigned char*)id.u.s, (size_t)id.n);
    }

    if (d->crypt_r >= 4 && !d->crypt_meta) {
        static const unsigned char ff[4] = { 255, 255, 255, 255 };

        md5_add(&m, ff, 4);
    }

    md5_end(&m, h);

    if (d->crypt_r >= 3) {
        for (i = 0; i < 50; i++) {
            pd_md5(h, (size_t)len, h);
        }
    }

    memcpy(key, h, (size_t)len);
}

/* algorithms 4 and 5: does this key make /U */
static int check_user_r4(pdoc* d, pv enc, const unsigned char* key, int len) {
    pv u = str_of(d, enc, "U");
    unsigned char buf[32];

    if (u.t != PV_STRING || u.n < 16) {
        return 0;
    }

    if (d->crypt_r == 2) {
        memcpy(buf, pw_pad, 32);
        pd_rc4(key, len, buf, 32);
        return memcmp(buf, u.u.s, 32 < u.n ? 32 : (size_t)u.n) == 0;
    }

    {
        md5_t m;
        pv id = pd_index(d, pd_get(d, d->trailer, "ID"), 0);
        unsigned char k2[32];
        int i, j;

        md5_init(&m);
        md5_add(&m, pw_pad, 32);

        if (id.t == PV_STRING) {
            md5_add(&m, (const unsigned char*)id.u.s, (size_t)id.n);
        }

        md5_end(&m, buf);
        pd_rc4(key, len, buf, 16);

        for (i = 1; i <= 19; i++) {
            for (j = 0; j < len; j++) {
                k2[j] = (unsigned char)(key[j] ^ i);
            }

            pd_rc4(k2, len, buf, 16);
        }

        return memcmp(buf, u.u.s, 16) == 0;
    }
}

/* algorithm 2.B (revision 6), or SHA-256 alone (revision 5) */
static void hash_r6(pdoc* d, const unsigned char* pw, size_t pwn, const unsigned char* salt, const unsigned char* udata,
                    size_t un, unsigned char out[32]) {
    unsigned char k[64], *buf;
    size_t kn = 32, cap;
    int round;

    {
        unsigned char* t = (unsigned char*)malloc(pwn + 8 + un + 1);

        if (!t) {
            memset(out, 0, 32);
            return;
        }

        memcpy(t, pw, pwn);
        memcpy(t + pwn, salt, 8);
        memcpy(t + pwn + 8, udata, un);
        sha256(t, pwn + 8 + un, k);
        free(t);
    }

    if (d->crypt_r < 6) {
        memcpy(out, k, 32);
        return;
    }

    cap = 64 * (pwn + 64 + un);
    buf = (unsigned char*)malloc(cap);

    if (!buf) {
        memcpy(out, k, 32);
        return;
    }

    for (round = 0;; round++) {
        size_t one = pwn + kn + un, i, en;
        unsigned char* e;
        int sum = 0;

        for (i = 0; i < 64; i++) {
            memcpy(buf + i * one, pw, pwn);
            memcpy(buf + i * one + pwn, k, kn);
            memcpy(buf + i * one + pwn + kn, udata, un);
        }

        e = aes_cbc_encrypt(k, 128, k + 16, buf, one * 64, 0, &en);

        if (!e) {
            break;
        }

        for (i = 0; i < 16; i++) {
            sum += e[16 + i];
        }

        switch (sum % 3) {
            case 0: sha256(e + 16, one * 64, k); kn = 32; break;
            case 1: sha512(e + 16, one * 64, k, 1); kn = 48; break;
            default: sha512(e + 16, one * 64, k, 0); kn = 64; break;
        }

        {
            int last = e[16 + one * 64 - 1];

            free(e);

            if (round >= 63 && last <= round - 32) {
                break;
            }
        }
    }

    free(buf);
    memcpy(out, k, 32);
}

static int setup_r6(pdoc* d, pv enc, const char* password) {
    pv u = str_of(d, enc, "U"), o = str_of(d, enc, "O"), ue = str_of(d, enc, "UE"), oe = str_of(d, enc, "OE");
    const unsigned char* pw = (const unsigned char*)(password ? password : "");
    size_t pwn = strlen((const char*)pw);
    unsigned char h[32], iv[16];
    unsigned char* k;
    size_t kn;

    if (pwn > 127) {
        pwn = 127;
    }

    if (u.t != PV_STRING || o.t != PV_STRING || u.n < 48 || o.n < 48) {
        return 0;
    }

    memset(iv, 0, 16);
    hash_r6(d, pw, pwn, (const unsigned char*)u.u.s + 32, NULL, 0, h);

    if (memcmp(h, u.u.s, 32) == 0 && ue.t == PV_STRING && ue.n >= 32) {
        unsigned char tmp[48];

        hash_r6(d, pw, pwn, (const unsigned char*)u.u.s + 40, NULL, 0, h);
        memcpy(tmp, iv, 16);
        memcpy(tmp + 16, ue.u.s, 32);
        k = aes_cbc_decrypt(h, 256, tmp, 48, 0, &kn);

        if (k && kn >= 32) {
            memcpy(d->key, k, 32);
            d->keylen = 32;
            free(k);
            return 1;
        }

        free(k);
    }

    hash_r6(d, pw, pwn, (const unsigned char*)o.u.s + 32, (const unsigned char*)u.u.s, 48, h);

    if (memcmp(h, o.u.s, 32) == 0 && oe.t == PV_STRING && oe.n >= 32) {
        unsigned char tmp[48];

        hash_r6(d, pw, pwn, (const unsigned char*)o.u.s + 40, (const unsigned char*)u.u.s, 48, h);
        memcpy(tmp, iv, 16);
        memcpy(tmp + 16, oe.u.s, 32);
        k = aes_cbc_decrypt(h, 256, tmp, 48, 0, &kn);

        if (k && kn >= 32) {
            memcpy(d->key, k, 32);
            d->keylen = 32;
            free(k);
            return 1;
        }

        free(k);
    }

    return 0;
}

static int cfm_method(pdoc* d, pv enc, const char* name) {
    pv cf, filt;
    const char* cfm;

    if (!name || strcmp(name, "Identity") == 0) {
        return 0;
    }

    cf = pd_get(d, enc, "CF");
    filt = pd_get(d, cf, name);
    cfm = pd_getname(d, filt, "CFM");

    if (!cfm || strcmp(cfm, "V2") == 0) {
        return 1;
    }

    if (strcmp(cfm, "AESV2") == 0) {
        return 2;
    }

    if (strcmp(cfm, "AESV3") == 0) {
        return 3;
    }

    return cfm && strcmp(cfm, "None") == 0 ? 0 : 1;
}

/* 0 ready (or not encrypted), 1 a password needed, -1 not a handler known */
static int setup_crypt(pdoc* d, const char* password) {
    pv encref = pd_get_raw(d->trailer, "Encrypt"), enc;
    const char* filter;
    unsigned char padded[32];
    int len;

    if (encref.t == PV_NULL) {
        return 0;
    }

    d->encrypt_ref = encref;
    enc = pd_resolve(d, encref);

    if (enc.t != PV_DICT) {
        return 0;
    }

    filter = pd_getname(d, enc, "Filter");

    if (!filter || strcmp(filter, "Standard") != 0) {
        snprintf(d->error, sizeof(d->error), "the file is encrypted by a security handler not known (%s)",
                 filter ? filter : "none");
        return -1;
    }

    d->crypt_v = (int)pd_getint(d, enc, "V", 0);
    d->crypt_r = (int)pd_getint(d, enc, "R", 2);
    d->crypt_meta = pd_get(d, enc, "EncryptMetadata").t == PV_BOOL ? (int)pd_get(d, enc, "EncryptMetadata").u.i : 1;
    len = (int)(pd_getint(d, enc, "Length", 40) / 8);

    if (d->crypt_v >= 4) {
        d->crypt_streams = cfm_method(d, enc, pd_getname(d, enc, "StmF"));
        d->crypt_strings = cfm_method(d, enc, pd_getname(d, enc, "StrF"));

        if (d->crypt_v == 4) {
            pv cf = pd_get(d, pd_get(d, enc, "CF"), pd_getname(d, enc, "StmF") ? pd_getname(d, enc, "StmF") : "");
            int64_t l = pd_getint(d, cf, "Length", 16);

            len = (int)(l > 32 ? l / 8 : l);   /* bytes here, though some write bits */
        }
    } else {
        d->crypt_streams = d->crypt_strings = 1;
    }

    if (len < 5 || len > 16) {
        len = d->crypt_v == 1 ? 5 : 16;
    }

    d->crypt = d->crypt_streams ? d->crypt_streams : d->crypt_strings ? d->crypt_strings : 1;

    if (d->crypt_r >= 5) {
        if (!setup_r6(d, enc, password) && !(password && *password == 0 ? 0 : setup_r6(d, enc, ""))) {
            return 1;
        }

        return 0;
    }

    /* the user password, then the given one as the owner's */
    pad_password(password ? password : "", padded);
    key_r4(d, enc, padded, d->key, len);

    if (check_user_r4(d, enc, d->key, len)) {
        d->keylen = len;
        return 0;
    }

    {
        unsigned char ok[16], user[32];
        pv o = str_of(d, enc, "O");
        int i, j;

        pd_md5(padded, 32, ok);

        if (d->crypt_r >= 3) {
            for (i = 0; i < 50; i++) {
                pd_md5(ok, 16, ok);
            }
        }

        if (o.t == PV_STRING && o.n >= 32) {
            memcpy(user, o.u.s, 32);

            if (d->crypt_r == 2) {
                pd_rc4(ok, len, user, 32);
            } else {
                for (i = 19; i >= 0; i--) {
                    unsigned char k2[16];

                    for (j = 0; j < len; j++) {
                        k2[j] = (unsigned char)(ok[j] ^ i);
                    }

                    pd_rc4(k2, len, user, 32);
                }
            }

            key_r4(d, enc, user, d->key, len);

            if (check_user_r4(d, enc, d->key, len)) {
                d->keylen = len;
                return 0;
            }
        }
    }

    return 1;
}

static int obj_key(pdoc* d, int32_t num, int32_t gen, int method, unsigned char* key) {
    unsigned char t[32 + 9];
    int n;

    if (method == 3) {
        memcpy(key, d->key, 32);
        return 32;
    }

    memcpy(t, d->key, (size_t)d->keylen);
    t[d->keylen] = (unsigned char)num;
    t[d->keylen + 1] = (unsigned char)(num >> 8);
    t[d->keylen + 2] = (unsigned char)(num >> 16);
    t[d->keylen + 3] = (unsigned char)gen;
    t[d->keylen + 4] = (unsigned char)(gen >> 8);
    n = d->keylen + 5;

    if (method == 2) {
        memcpy(t + n, "sAlT", 4);
        n += 4;
    }

    pd_md5(t, (size_t)n, key);
    return d->keylen + 5 < 16 ? d->keylen + 5 : 16;
}

unsigned char* pd_decrypt(pdoc* d, int32_t num, int32_t gen, int method, const unsigned char* data, size_t n,
                          size_t* outlen) {
    unsigned char key[32];
    int kl;

    if (!method || !d->keylen) {
        unsigned char* o = (unsigned char*)malloc(n + 1);

        if (o) {
            memcpy(o, data, n);
        }

        *outlen = n;
        return o;
    }

    kl = obj_key(d, num, gen, method, key);

    if (method == 1) {
        unsigned char* o = (unsigned char*)malloc(n + 1);

        if (o) {
            memcpy(o, data, n);
            pd_rc4(key, kl, o, n);
        }

        *outlen = n;
        return o;
    }

    return aes_cbc_decrypt(key, method == 3 ? 256 : 128, data, n, 1, outlen);
}

unsigned char* pd_encrypt(pdoc* d, int32_t num, int32_t gen, int method, const unsigned char* data, size_t n,
                          size_t* outlen) {
    unsigned char key[32], iv[16];
    int kl, i;

    if (!method || !d->keylen) {
        return pd_decrypt(d, num, gen, 0, data, n, outlen);
    }

    kl = obj_key(d, num, gen, method, key);

    if (method == 1) {
        return pd_decrypt(d, num, gen, 1, data, n, outlen);     /* RC4 is its own inverse */
    }

    {
        unsigned char seed[24];

        memcpy(seed, data, n < 8 ? n : 8);
        seed[8] = (unsigned char)num;
        seed[9] = (unsigned char)(num >> 8);
        seed[10] = (unsigned char)(n);
        seed[11] = (unsigned char)(n >> 8);

        for (i = 12; i < 24; i++) {
            seed[i] = (unsigned char)(i * 37 + d->changes);
        }

        pd_md5(seed, 24, iv);
    }

    (void)kl;
    return aes_cbc_encrypt(key, method == 3 ? 256 : 128, iv, data, n, 1, outlen);
}

/* strings of an object decrypted, the tree walked */
static void decrypt_strings(pdoc* d, pv* v, int32_t num, int32_t gen, int depth) {
    int32_t i, n;
    pkv* e;

    if (depth > MAX_DEPTH) {
        return;
    }

    if (v->t == PV_STRING && d->crypt_strings) {
        size_t on;
        unsigned char* o = pd_decrypt(d, num, gen, d->crypt_strings, (const unsigned char*)v->u.s, (size_t)v->n, &on);

        if (o) {
            int hex = v->hex;

            *v = pv_string(d, o, on);
            v->hex = (uint8_t)hex;
            free(o);
        }
    } else if (v->t == PV_ARRAY) {
        for (i = 0; i < v->n; i++) {
            decrypt_strings(d, &v->u.a[i], num, gen, depth + 1);
        }
    } else if (v->t == PV_DICT || v->t == PV_STREAM) {
        e = pv_entries(*v, &n);

        for (i = 0; i < n; i++) {
            decrypt_strings(d, &e[i].val, num, gen, depth + 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* objects                                                            */
/* ------------------------------------------------------------------ */

static int64_t find_bytes(const unsigned char* p, size_t n, size_t from, const char* s) {
    size_t k = strlen(s), i;

    for (i = from; i + k <= n; i++) {
        if (p[i] == (unsigned char)s[0] && memcmp(p + i, s, k) == 0) {
            return (int64_t)i;
        }
    }

    return -1;
}

static int grow_xref(pdoc* d, int32_t n) {
    if (n > d->nxref) {
        pxref* x;

        if (n > 50000000) {
            return 0;
        }

        x = (pxref*)realloc(d->xref, sizeof(pxref) * (size_t)n);

        if (!x) {
            return 0;
        }

        memset(x + d->nxref, 0, sizeof(pxref) * (size_t)(n - d->nxref));
        d->xref = x;
        d->nxref = n;
    }

    return 1;
}

/* the object at a file offset: "num gen obj value [stream] endobj"; *num gets its number */
static pv parse_at(pdoc* d, int64_t off, int32_t* num, int32_t* gen) {
    plex L;
    pv a, b, c, v;

    memset(&L, 0, sizeof(L));
    L.p = d->data;
    L.n = d->len;
    L.i = (size_t)off;
    L.d = d;

    if (off < 0 || (size_t)off >= d->len) {
        return err_val();
    }

    a = lex_word(&L);
    pl_skip_ws(&L);
    b = L.i < L.n ? lex_word(&L) : err_val();
    pl_skip_ws(&L);
    c = L.i < L.n ? lex_word(&L) : err_val();

    if (a.t != PV_INT || b.t != PV_INT || c.t != PV_OP || strcmp(c.u.s, "obj") != 0) {
        return err_val();
    }

    *num = (int32_t)a.u.i;
    *gen = (int32_t)b.u.i;
    v = read_value(&L, 0);

    if (v.t == PV_OP && strcmp(v.u.s, "endobj") == 0) {
        return pv_null();
    }

    pl_skip_ws(&L);

    if (v.t == PV_DICT && L.i + 6 <= L.n && memcmp(L.p + L.i, "stream", 6) == 0) {
        pstream* st = (pstream*)pa_alloc(&d->arena, sizeof(pstream));
        size_t s = L.i + 6;
        double len = -1;
        pv lv = pd_get_raw(v, "Length");

        if (s < L.n && L.p[s] == 13) {
            s++;
        }

        if (s < L.n && L.p[s] == 10) {
            s++;
        }

        if (lv.t == PV_INT) {
            len = (double)lv.u.i;
        } else if (lv.t == PV_REF && lv.u.i != *num) {
            pd_num(d, lv, &len);
        }

        /* the length checked against an endstream where it says; else the endstream looked for */
        if (len >= 0 && s + (size_t)len <= L.n) {
            size_t e = s + (size_t)len;

            while (e < L.n && is_ws(L.p[e])) {
                e++;
            }

            if (!(e + 9 <= L.n && memcmp(L.p + e, "endstream", 9) == 0)) {
                len = -1;
            }
        } else {
            len = -1;
        }

        if (len < 0) {
            int64_t e = find_bytes(L.p, L.n, s, "endstream");

            if (e < 0) {
                e = (int64_t)L.n;
            }

            len = (double)(e - (int64_t)s);

            while (len > 0 && (L.p[s + (size_t)len - 1] == 10 || L.p[s + (size_t)len - 1] == 13)) {
                len--;
            }
        }

        st->d = v.u.d;
        st->nd = v.n;
        st->off = (int64_t)s;
        st->len = (int64_t)len;
        st->num = *num;
        st->gen = *gen;
        v.t = PV_STREAM;
        v.u.st = st;
        v.n = 0;
    }

    return v;
}

typedef struct {
    int32_t num;
    int32_t n;
    int32_t* nums;
    int64_t* offs;
    unsigned char* data;
    size_t len;
} pobjstm;

static void load_objstm(pdoc* d, int32_t stmnum) {
    pv st = pd_object(d, stmnum);
    size_t len;
    unsigned char* data;
    int64_t n, first, i;
    plex L;

    if (st.t != PV_STREAM) {
        return;
    }

    data = pd_stream_data(d, st, &len, NULL, NULL);

    if (!data) {
        return;
    }

    n = pd_getint(d, st, "N", 0);
    first = pd_getint(d, st, "First", 0);

    if (n < 0 || n > 1000000 || first < 0 || (size_t)first > len) {
        free(data);
        return;
    }

    memset(&L, 0, sizeof(L));
    L.p = data;
    L.n = len;
    L.d = d;

    {
        int32_t* nums = (int32_t*)malloc(sizeof(int32_t) * (size_t)(n + 1));
        int64_t* offs = (int64_t*)malloc(sizeof(int64_t) * (size_t)(n + 1));

        if (!nums || !offs) {
            free(nums);
            free(offs);
            free(data);
            return;
        }

        for (i = 0; i < n; i++) {
            pv a, b;

            pl_skip_ws(&L);
            a = L.i < L.n ? lex_word(&L) : err_val();
            pl_skip_ws(&L);
            b = L.i < L.n ? lex_word(&L) : err_val();

            if (a.t != PV_INT || b.t != PV_INT) {
                n = i;
                break;
            }

            nums[i] = (int32_t)a.u.i;
            offs[i] = b.u.i;
        }

        for (i = 0; i < n; i++) {
            int32_t on = nums[i];

            if (on > 0 && on < d->nxref && d->xref[on].type == 2 && d->xref[on].off == stmnum && !d->xref[on].obj) {
                pv v;

                L.content = 0;
                L.i = (size_t)(first + offs[i]);

                if (L.i >= L.n) {
                    continue;
                }

                v = read_value(&L, 0);

                if (v.t == PV_ERR || v.t == PV_OP) {
                    v = pv_null();
                }

                d->xref[on].obj = (pv*)pa_alloc(&d->arena, sizeof(pv));

                if (d->xref[on].obj) {
                    *d->xref[on].obj = v;
                }
            }
        }

        free(nums);
        free(offs);
    }

    free(data);
}

static int repair(pdoc* d);
void pd_glyph_cache_free(pdoc* d);

pv pd_object(pdoc* d, int32_t num) {
    pxref* x;
    pv v;
    int32_t on = 0, og = 0;

    if (num <= 0 || num >= d->nxref) {
        return pv_null();
    }

    x = &d->xref[num];

    if (x->obj) {
        return *x->obj;
    }

    if (x->loading || x->type == 0) {
        return pv_null();
    }

    x->loading = 1;

    if (x->type == 2) {
        load_objstm(d, (int32_t)x->off);
        x = &d->xref[num];
        x->loading = 0;
        return x->obj ? *x->obj : pv_null();
    }

    v = parse_at(d, x->off, &on, &og);

    if ((v.t == PV_ERR || on != num) && !d->repaired) {
        x->loading = 0;

        if (repair(d) == 0 && num < d->nxref) {
            return pd_object(d, num);
        }

        return pv_null();
    }

    x = &d->xref[num];
    x->loading = 0;

    if (v.t == PV_ERR) {
        v = pv_null();
    }

    if (d->keylen && !(d->encrypt_ref.t == PV_REF && d->encrypt_ref.u.i == num)) {
        decrypt_strings(d, &v, num, og, 0);
    }

    x->obj = (pv*)pa_alloc(&d->arena, sizeof(pv));

    if (x->obj) {
        *x->obj = v;
    }

    return v;
}

/* ------------------------------------------------------------------ */
/* filters                                                            */
/* ------------------------------------------------------------------ */

static unsigned char* predict(unsigned char* data, size_t* len, pdoc* d, pv parms) {
    int64_t pred = pd_getint(d, parms, "Predictor", 1), colors = pd_getint(d, parms, "Colors", 1);
    int64_t bpc = pd_getint(d, parms, "BitsPerComponent", 8), cols = pd_getint(d, parms, "Columns", 1);
    size_t bpp, row, i, nrows, k;
    unsigned char* out, *prev;

    if (pred < 2 || colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || cols < 1 || cols > 1 << 24) {
        return data;
    }

    bpp = (size_t)((colors * bpc + 7) / 8);
    row = (size_t)((colors * bpc * cols + 7) / 8);

    if (pred == 2) {    /* TIFF: 8 bits a component only, the usual case */
        if (bpc == 8) {
            for (i = 0; i + row <= *len; i += row) {
                for (k = bpp; k < row; k++) {
                    data[i + k] = (unsigned char)(data[i + k] + data[i + k - bpp]);
                }
            }
        }

        return data;
    }

    nrows = *len / (row + 1);
    out = (unsigned char*)calloc(nrows * row + row + 1, 1);

    if (!out) {
        return data;
    }

    prev = (unsigned char*)calloc(row + 1, 1);

    for (i = 0; i < nrows && prev; i++) {
        const unsigned char* src = data + i * (row + 1) + 1;
        unsigned char* dst = out + i * row;
        int f = data[i * (row + 1)];

        for (k = 0; k < row; k++) {
            int a = k >= bpp ? dst[k - bpp] : 0, b = prev[k], c = k >= bpp ? prev[k - bpp] : 0, x = src[k];

            switch (f) {
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                case 4: {
                    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);

                    x += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                    break;
                }
                default: break;
            }

            dst[k] = (unsigned char)x;
        }

        memcpy(prev, dst, row);
    }

    free(prev);
    free(data);
    *len = nrows * row;
    return out;
}

static unsigned char* flate(const unsigned char* p, size_t n, size_t* outlen) {
    unsigned char* o = NULL;
    size_t on = 0;
    size_t skip = 0;

    if (n >= 2 && (p[0] & 0x0F) == 8 && ((p[0] << 8) | p[1]) % 31 == 0) {
        skip = 2;
    }

    if (pd_inflate_partial(p + skip, n - skip, 0, &o, &on) < 0) {
        if (skip == 0 || pd_inflate_partial(p, n, 0, &o, &on) < 0) {
            *outlen = 0;
            return (unsigned char*)calloc(1, 1);
        }
    }

    *outlen = on;
    return o;
}

static unsigned char* lzw(const unsigned char* p, size_t n, int early, size_t* outlen) {
    pbuf o;
    uint16_t prefix[4096];
    unsigned char suffix[4096], first[4096];
    uint16_t length[4096];
    int next = 258, bits = 9, old = -1;
    uint32_t acc = 0;
    int nacc = 0;
    size_t i = 0;
    unsigned char stack[4097];

    memset(&o, 0, sizeof(o));

    for (;;) {
        int code, k, len;

        while (nacc < bits) {
            if (i >= n) {
                goto done;
            }

            acc = (acc << 8) | p[i++];
            nacc += 8;
        }

        code = (int)((acc >> (nacc - bits)) & ((1u << bits) - 1));
        nacc -= bits;

        if (code == 256) {
            next = 258;
            bits = 9;
            old = -1;
            continue;
        }

        if (code == 257) {
            break;
        }

        if (old < 0) {
            unsigned char c = (unsigned char)code;

            if (code > 255) {
                break;
            }

            qb_put(&o, &c, 1);
            old = code;
            continue;
        }

        if (code < next || code < 256) {
            int c = code;

            len = 0;

            while (c > 255 && len < 4096) {
                stack[len++] = suffix[c];
                c = prefix[c];
            }

            stack[len++] = (unsigned char)c;

            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = (unsigned char)c;
                first[next] = old > 255 ? first[old] : (unsigned char)old;
                length[next] = (uint16_t)((old > 255 ? length[old] : 1) + 1);
                next++;
            }
        } else if (code == next) {
            int c = old;

            len = 0;
            stack[len++] = 0;   /* the first byte of old, set below */

            while (c > 255 && len < 4096) {
                stack[len++] = suffix[c];
                c = prefix[c];
            }

            stack[len++] = (unsigned char)c;
            stack[0] = (unsigned char)c;

            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = (unsigned char)c;
                next++;
            }
        } else {
            break;
        }

        for (k = len - 1; k >= 0; k--) {
            qb_put(&o, &stack[k], 1);
        }

        old = code;

        if (next + early >= (1 << bits) && bits < 12) {
            bits++;
        }
    }

done:
    (void)first;
    (void)length;
    *outlen = o.n;
    return o.p ? o.p : (unsigned char*)calloc(1, 1);
}

static unsigned char* ahx(const unsigned char* p, size_t n, size_t* outlen) {
    unsigned char* o = (unsigned char*)malloc(n / 2 + 2);
    size_t k = 0, i;
    int hi = -1;

    if (!o) {
        return NULL;
    }

    for (i = 0; i < n && p[i] != '>'; i++) {
        int h = hexval(p[i]);

        if (h < 0) {
            continue;
        }

        if (hi < 0) {
            hi = h;
        } else {
            o[k++] = (unsigned char)(hi * 16 + h);
            hi = -1;
        }
    }

    if (hi >= 0) {
        o[k++] = (unsigned char)(hi * 16);
    }

    *outlen = k;
    return o;
}

static unsigned char* a85(const unsigned char* p, size_t n, size_t* outlen) {
    unsigned char* o = (unsigned char*)malloc(n / 5 * 4 + n + 8);
    size_t k = 0, i = 0;
    uint32_t acc = 0;
    int cnt = 0;

    if (!o) {
        return NULL;
    }

    if (n >= 2 && p[0] == '<' && p[1] == '~') {
        i = 2;
    }

    for (; i < n; i++) {
        int c = p[i];

        if (c == '~') {
            break;
        }

        if (is_ws(c)) {
            continue;
        }

        if (c == 'z' && cnt == 0) {
            memset(o + k, 0, 4);
            k += 4;
            continue;
        }

        if (c < '!' || c > 'u') {
            continue;
        }

        acc = acc * 85 + (uint32_t)(c - '!');

        if (++cnt == 5) {
            o[k++] = (unsigned char)(acc >> 24);
            o[k++] = (unsigned char)(acc >> 16);
            o[k++] = (unsigned char)(acc >> 8);
            o[k++] = (unsigned char)acc;
            acc = 0;
            cnt = 0;
        }
    }

    if (cnt > 1) {
        int j;

        for (j = cnt; j < 5; j++) {
            acc = acc * 85 + 84;
        }

        for (j = 0; j < cnt - 1; j++) {
            o[k++] = (unsigned char)(acc >> (24 - 8 * j));
        }
    }

    *outlen = k;
    return o;
}

static unsigned char* rle(const unsigned char* p, size_t n, size_t* outlen) {
    pbuf o;
    size_t i = 0;

    memset(&o, 0, sizeof(o));

    while (i < n) {
        int len = p[i++];

        if (len == 128) {
            break;
        }

        if (len < 128) {
            size_t take = (size_t)len + 1 <= n - i ? (size_t)len + 1 : n - i;

            qb_put(&o, p + i, take);
            i += take;
        } else if (i < n) {
            unsigned char run[128];

            memset(run, p[i++], sizeof(run));
            qb_put(&o, run, (size_t)(257 - len));
        }
    }

    *outlen = o.n;
    return o.p ? o.p : (unsigned char*)calloc(1, 1);
}

unsigned char* pd_decode(pdoc* d, const unsigned char* data, size_t len, pv filter, pv parms, size_t* outlen,
                         int* image_filter, pv* image_parms) {
    unsigned char* cur = (unsigned char*)malloc(len + 1);
    size_t n = len;
    int32_t i, nf;

    if (image_filter) {
        *image_filter = PF_NONE;
    }

    if (!cur) {
        return NULL;
    }

    memcpy(cur, data, len);
    cur[len] = 0;
    filter = pd_resolve(d, filter);
    parms = pd_resolve(d, parms);
    nf = filter.t == PV_ARRAY ? filter.n : filter.t == PV_NAME ? 1 : 0;

    for (i = 0; i < nf; i++) {
        pv f = filter.t == PV_ARRAY ? pd_index(d, filter, i) : filter;
        pv pm = parms.t == PV_ARRAY ? pd_index(d, parms, i) : parms;
        const char* name = f.t == PV_NAME ? f.u.s : "";
        unsigned char* next = NULL;
        size_t nn = 0;

        if (strcmp(name, "FlateDecode") == 0 || strcmp(name, "Fl") == 0) {
            next = flate(cur, n, &nn);

            if (next) {
                next = predict(next, &nn, d, pm);
            }
        } else if (strcmp(name, "LZWDecode") == 0 || strcmp(name, "LZW") == 0) {
            next = lzw(cur, n, (int)pd_getint(d, pm, "EarlyChange", 1), &nn);

            if (next) {
                next = predict(next, &nn, d, pm);
            }
        } else if (strcmp(name, "ASCIIHexDecode") == 0 || strcmp(name, "AHx") == 0) {
            next = ahx(cur, n, &nn);
        } else if (strcmp(name, "ASCII85Decode") == 0 || strcmp(name, "A85") == 0) {
            next = a85(cur, n, &nn);
        } else if (strcmp(name, "RunLengthDecode") == 0 || strcmp(name, "RL") == 0) {
            next = rle(cur, n, &nn);
        } else if (strcmp(name, "Crypt") == 0) {
            continue;
        } else {
            int kind = strcmp(name, "DCTDecode") == 0 || strcmp(name, "DCT") == 0 ? PF_DCT :
                       strcmp(name, "JPXDecode") == 0 ? PF_JPX : strcmp(name, "JBIG2Decode") == 0 ? PF_JBIG2 :
                       strcmp(name, "CCITTFaxDecode") == 0 || strcmp(name, "CCF") == 0 ? PF_CCITT : -1;

            if (kind > 0 && image_filter) {
                *image_filter = kind;

                if (image_parms) {
                    *image_parms = pm;
                }
            }

            break;  /* an image codec, or a filter not known: the data as far as it got */
        }

        free(cur);
        cur = next;
        n = nn;

        if (!cur) {
            return NULL;
        }
    }

    *outlen = n;
    return cur;
}

unsigned char* pd_stream_raw(pdoc* d, pv stream, size_t* len) {
    pstream* st;
    const unsigned char* src;
    size_t n;

    stream = pd_resolve(d, stream);

    if (stream.t != PV_STREAM) {
        return NULL;
    }

    st = stream.u.st;

    if (st->data) {
        src = st->data;
        n = (size_t)st->len;
    } else {
        if (st->off < 0 || (size_t)st->off > d->len) {
            return NULL;
        }

        src = d->data + st->off;
        n = (size_t)st->len;

        if ((size_t)st->off + n > d->len) {
            n = d->len - (size_t)st->off;
        }
    }

    if (d->keylen && st->num && !st->data) {
        const char* type = pd_getname(d, stream, "Type");
        pv f = pd_get(d, stream, "Filter");
        int method = d->crypt_streams;

        if (type && (strcmp(type, "XRef") == 0 || (strcmp(type, "Metadata") == 0 && !d->crypt_meta))) {
            method = 0;
        }

        if ((f.t == PV_NAME && strcmp(f.u.s, "Crypt") == 0) ||
            (f.t == PV_ARRAY && f.n > 0 && pv_is_name(pd_index(d, f, 0), "Crypt"))) {
            pv dp = pd_get(d, stream, "DecodeParms");
            const char* nm;

            if (dp.t == PV_ARRAY) {
                dp = pd_index(d, dp, 0);
            }

            nm = pd_getname(d, dp, "Name");
            method = cfm_method(d, pd_resolve(d, d->encrypt_ref), nm ? nm : "Identity");
        }

        if (method) {
            return pd_decrypt(d, st->num, st->gen, method, src, n, len);
        }
    }

    {
        unsigned char* o = (unsigned char*)malloc(n + 1);

        if (o) {
            memcpy(o, src, n);
            o[n] = 0;
        }

        *len = n;
        return o;
    }
}

unsigned char* pd_stream_data(pdoc* d, pv stream, size_t* len, int* image_filter, pv* image_parms) {
    size_t n;
    unsigned char* raw = pd_stream_raw(d, stream, &n), *o;
    pv f, p;

    if (!raw) {
        return NULL;
    }

    f = pd_get(d, stream, "Filter");
    p = pd_get(d, stream, "DecodeParms");

    if (f.t == PV_NULL) {
        f = pd_get(d, stream, "F");    /* inline images' abbreviations */
        p = pd_get(d, stream, "DP");
    }

    if (f.t == PV_NULL) {
        *len = n;

        if (image_filter) {
            *image_filter = PF_NONE;
        }

        return raw;
    }

    o = pd_decode(d, raw, n, f, p, len, image_filter, image_parms);
    free(raw);
    return o;
}

/* ------------------------------------------------------------------ */
/* cross-reference                                                    */
/* ------------------------------------------------------------------ */

static int read_xref_stream(pdoc* d, pv st, int32_t declared_size) {
    int64_t w[3] = { 1, 2, 1 };
    pv wa = pd_get(d, st, "W"), idx = pd_get(d, st, "Index");
    size_t len, pos = 0;
    unsigned char* data;
    int32_t i, nidx;
    int64_t rowlen;

    for (i = 0; i < 3; i++) {
        pv e = pd_index(d, wa, i);

        w[i] = e.t == PV_INT ? e.u.i : 0;

        if (w[i] < 0 || w[i] > 8) {
            return 0;
        }
    }

    rowlen = w[0] + w[1] + w[2];

    if (rowlen <= 0) {
        return 0;
    }

    data = pd_stream_data(d, st, &len, NULL, NULL);

    if (!data) {
        return 0;
    }

    nidx = idx.t == PV_ARRAY ? idx.n / 2 : 1;

    for (i = 0; i < nidx; i++) {
        int64_t start = idx.t == PV_ARRAY ? pd_getint(d, pv_null(), "", 0) : 0, count;
        int64_t k;

        if (idx.t == PV_ARRAY) {
            pv a = pd_index(d, idx, 2 * i), b = pd_index(d, idx, 2 * i + 1);

            start = a.t == PV_INT ? a.u.i : 0;
            count = b.t == PV_INT ? b.u.i : 0;
        } else {
            count = declared_size;
        }

        if (start < 0 || count < 0 || start + count > 50000000) {
            break;
        }

        if (!grow_xref(d, (int32_t)(start + count))) {
            break;
        }

        for (k = 0; k < count && pos + (size_t)rowlen <= len; k++) {
            int64_t f[3];
            int j, b;
            pxref* x = &d->xref[start + k];

            for (j = 0; j < 3; j++) {
                f[j] = 0;

                for (b = 0; b < w[j]; b++) {
                    f[j] = (f[j] << 8) | data[pos++];
                }
            }

            if (w[0] == 0) {
                f[0] = 1;
            }

            if (x->type || x->gen < 0) {
                continue;   /* a newer section set it */
            }

            if (f[0] == 0) {
                x->gen = -1;    /* freed in a newer section: older ones do not bring it back */
            } else if (f[0] == 1) {
                x->type = 1;
                x->off = f[1];
                x->gen = (int32_t)f[2];
            } else if (f[0] == 2) {
                x->type = 2;
                x->off = f[1];
                x->gen = (int32_t)f[2];
            }
        }
    }

    free(data);
    return 1;
}

static void merge_trailer(pdoc* d, pv t) {
    int32_t n, i;
    pkv* e = pv_entries(t, &n);

    if (d->trailer.t != PV_DICT) {
        d->trailer = pv_dict(d, n);
    }

    for (i = 0; i < n; i++) {
        if (strcmp(e[i].key, "Prev") && strcmp(e[i].key, "XRefStm") && pd_get_raw(d->trailer, e[i].key).t == PV_NULL &&
            strcmp(e[i].key, "Length") && strcmp(e[i].key, "Filter") && strcmp(e[i].key, "DecodeParms") &&
            strcmp(e[i].key, "W") && strcmp(e[i].key, "Index") && strcmp(e[i].key, "Type")) {
            pd_dict_set(d, &d->trailer, e[i].key, e[i].val);
        }
    }
}

static int read_xref_at(pdoc* d, int64_t off, int depth) {
    plex L;

    if (depth > 64 || off < 0 || (size_t)off >= d->len) {
        return 0;
    }

    memset(&L, 0, sizeof(L));
    L.p = d->data;
    L.n = d->len;
    L.i = (size_t)off;
    L.d = d;
    pl_skip_ws(&L);

    if (L.i + 4 <= L.n && memcmp(L.p + L.i, "xref", 4) == 0) {
        pv trailer;

        L.i += 4;

        for (;;) {
            pv a, b;
            int64_t k;

            pl_skip_ws(&L);

            if (L.i + 7 <= L.n && memcmp(L.p + L.i, "trailer", 7) == 0) {
                L.i += 7;
                break;
            }

            a = lex_word(&L);
            pl_skip_ws(&L);
            b = lex_word(&L);

            if (a.t != PV_INT || b.t != PV_INT || a.u.i < 0 || b.u.i < 0 || a.u.i + b.u.i > 50000000) {
                return 0;
            }

            if (!grow_xref(d, (int32_t)(a.u.i + b.u.i))) {
                return 0;
            }

            for (k = 0; k < b.u.i; k++) {
                pv o, g, t;
                pxref* x;

                pl_skip_ws(&L);
                o = lex_word(&L);
                pl_skip_ws(&L);
                g = lex_word(&L);
                pl_skip_ws(&L);
                t = lex_word(&L);

                if (o.t != PV_INT || g.t != PV_INT || t.t != PV_OP) {
                    return 0;
                }

                x = &d->xref[a.u.i + k];

                if (x->type || x->gen < 0) {
                    continue;
                }

                if (t.u.s[0] == 'n' && o.u.i > 0) {
                    x->type = 1;
                    x->off = o.u.i;
                    x->gen = (int32_t)g.u.i;
                } else if (a.u.i + k > 0) {
                    x->gen = -1;
                }
            }
        }

        trailer = read_value(&L, 0);

        if (trailer.t != PV_DICT) {
            return 0;
        }

        {
            pv xs = pd_get_raw(trailer, "XRefStm");

            if (xs.t == PV_INT) {   /* a hybrid file: the stream's entries count as this section's */
                int32_t num, gen;
                pv st = parse_at(d, xs.u.i, &num, &gen);

                if (st.t == PV_STREAM) {
                    read_xref_stream(d, st, (int32_t)pd_getint(d, trailer, "Size", 0));
                }
            }
        }

        merge_trailer(d, trailer);

        {
            pv prev = pd_get_raw(trailer, "Prev");

            if (prev.t == PV_INT && prev.u.i != off) {
                return read_xref_at(d, prev.u.i, depth + 1) || 1;
            }
        }

        return 1;
    }

    {
        int32_t num, gen;
        pv st = parse_at(d, off, &num, &gen);

        if (st.t != PV_STREAM || !pv_is_name(pd_get_raw(st, "Type"), "XRef")) {
            return 0;
        }

        if (depth == 0) {
            d->xref_stream = 1;
        }

        if (!read_xref_stream(d, st, (int32_t)pd_getint(d, st, "Size", 0))) {
            return 0;
        }

        {
            pv t = pv_null();

            t.t = PV_DICT;
            t.u.d = st.u.st->d;
            t.n = st.u.st->nd;
            merge_trailer(d, t);

            {
                pv prev = pd_get_raw(t, "Prev");

                if (prev.t == PV_INT && prev.u.i != off) {
                    read_xref_at(d, prev.u.i, depth + 1);
                }
            }
        }

        return 1;
    }
}

/* the file scanned for its objects: the last "n g obj" of each number wins */
static int repair(pdoc* d) {
    size_t i;
    int32_t catalog = 0;

    d->repaired = 1;

    for (i = 0; i < (size_t)d->nxref; i++) {
        d->xref[i].type = 0;
        d->xref[i].gen = 0;
        d->xref[i].obj = NULL;
    }

    for (i = 0; i + 4 < d->len; i++) {
        if (d->data[i] == 'o' && d->data[i + 1] == 'b' && d->data[i + 2] == 'j' &&
            (i + 3 >= d->len || is_ws(d->data[i + 3]) || is_delim(d->data[i + 3]))) {
            /* back over "gen" and "num" */
            size_t j = i, e1, s1, e2, s2;
            int64_t num = 0, gen = 0;

            while (j > 0 && is_ws(d->data[j - 1])) {
                j--;
            }

            e2 = j;

            while (j > 0 && isdigit(d->data[j - 1])) {
                j--;
            }

            s2 = j;

            while (j > 0 && is_ws(d->data[j - 1])) {
                j--;
            }

            e1 = j;

            while (j > 0 && isdigit(d->data[j - 1])) {
                j--;
            }

            s1 = j;

            if (s2 == e2 || s1 == e1 || e1 == s2 || e2 - s2 > 5 || e1 - s1 > 9) {
                continue;
            }

            for (j = s1; j < e1; j++) {
                num = num * 10 + (d->data[j] - '0');
            }

            for (j = s2; j < e2; j++) {
                gen = gen * 10 + (d->data[j] - '0');
            }

            if (num <= 0 || num > 10000000 || !grow_xref(d, (int32_t)num + 1)) {
                continue;
            }

            d->xref[num].type = 1;
            d->xref[num].off = (int64_t)s1;
            d->xref[num].gen = (int32_t)gen;
            d->xref[num].obj = NULL;
        }
    }

    /* the trailers in it, the last first; then object streams and xref streams */
    {
        int64_t at = 0;

        while ((at = find_bytes(d->data, d->len, (size_t)at, "trailer")) >= 0) {
            plex L;
            pv t;

            memset(&L, 0, sizeof(L));
            L.p = d->data;
            L.n = d->len;
            L.i = (size_t)at + 7;
            L.d = d;
            t = read_value(&L, 0);

            if (t.t == PV_DICT && pd_get_raw(t, "Root").t == PV_REF) {
                pd_dict_set(d, &d->trailer, "Root", pd_get_raw(t, "Root"));

                if (pd_get_raw(t, "Info").t == PV_REF) {
                    pd_dict_set(d, &d->trailer, "Info", pd_get_raw(t, "Info"));
                }

                if (pd_get_raw(t, "Encrypt").t != PV_NULL) {
                    pd_dict_set(d, &d->trailer, "Encrypt", pd_get_raw(t, "Encrypt"));
                }

                if (pd_get_raw(t, "ID").t != PV_NULL) {
                    pd_dict_set(d, &d->trailer, "ID", pd_get_raw(t, "ID"));
                }
            }

            at += 7;
        }
    }

    for (i = 1; i < (size_t)d->nxref; i++) {
        if (d->xref[i].type == 1) {
            pv v = pd_object(d, (int32_t)i);

            if (v.t == PV_STREAM) {
                const char* type = pd_getname(d, v, "Type");

                if (type && strcmp(type, "ObjStm") == 0) {
                    size_t len;
                    unsigned char* data = pd_stream_data(d, v, &len, NULL, NULL);
                    int64_t n = pd_getint(d, v, "N", 0), k;
                    plex L;

                    if (!data) {
                        continue;
                    }

                    memset(&L, 0, sizeof(L));
                    L.p = data;
                    L.n = len;
                    L.d = d;

                    for (k = 0; k < n && k < 1000000; k++) {
                        pv a, b;

                        pl_skip_ws(&L);
                        a = L.i < L.n ? lex_word(&L) : err_val();
                        pl_skip_ws(&L);
                        b = L.i < L.n ? lex_word(&L) : err_val();

                        if (a.t != PV_INT || b.t != PV_INT || a.u.i <= 0 || a.u.i > 10000000) {
                            break;
                        }

                        if (grow_xref(d, (int32_t)a.u.i + 1) && d->xref[a.u.i].type != 1) {
                            d->xref[a.u.i].type = 2;
                            d->xref[a.u.i].off = (int64_t)i;
                            d->xref[a.u.i].gen = (int32_t)k;
                        }
                    }

                    free(data);
                } else if (type && strcmp(type, "XRef") == 0 && pd_get_raw(d->trailer, "Root").t == PV_NULL &&
                           pd_get_raw(v, "Root").t == PV_REF) {
                    pd_dict_set(d, &d->trailer, "Root", pd_get_raw(v, "Root"));

                    if (pd_get_raw(v, "Encrypt").t != PV_NULL) {
                        pd_dict_set(d, &d->trailer, "Encrypt", pd_get_raw(v, "Encrypt"));
                    }

                    if (pd_get_raw(v, "ID").t != PV_NULL) {
                        pd_dict_set(d, &d->trailer, "ID", pd_get_raw(v, "ID"));
                    }
                }
            }
        }
    }

    if (pd_get(d, d->trailer, "Root").t != PV_DICT) {
        for (i = 1; i < (size_t)d->nxref && !catalog; i++) {
            if (d->xref[i].type) {
                pv v = pd_object(d, (int32_t)i);

                if (v.t == PV_DICT && pv_is_name(pd_get_raw(v, "Type"), "Catalog")) {
                    catalog = (int32_t)i;
                }
            }
        }

        if (catalog) {
            pd_dict_set(d, &d->trailer, "Root", pv_ref(catalog, 0));
        }
    }

    return pd_get(d, d->trailer, "Root").t == PV_DICT ? 0 : -1;
}

static int read_xref(pdoc* d) {
    size_t from = d->len > 2048 ? d->len - 2048 : 0;
    int64_t at = -1, s = (int64_t)from;

    while ((s = find_bytes(d->data, d->len, (size_t)s, "startxref")) >= 0) {
        at = s;
        s += 9;
    }

    if (at >= 0) {
        plex L;
        pv off;

        memset(&L, 0, sizeof(L));
        L.p = d->data;
        L.n = d->len;
        L.i = (size_t)at + 9;
        L.d = d;
        pl_skip_ws(&L);
        off = lex_word(&L);

        if (off.t == PV_INT && read_xref_at(d, off.u.i, 0)) {
            d->last_xref = off.u.i;

            if (pd_get(d, d->trailer, "Root").t == PV_DICT) {
                return 0;
            }
        }
    }

    d->trailer = pv_null();
    d->xref_stream = 0;
    d->last_xref = 0;
    return repair(d);
}

/* ------------------------------------------------------------------ */
/* pages                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    pv resources;
    double media[4], crop[4];
    int has_media, has_crop;
    int32_t rotate;
} inherit;

static void add_page(pdoc* d, pv ref, pv node, const inherit* in, int32_t* cap) {
    ppage* p;

    if (d->npages >= *cap) {
        int32_t nc = *cap ? *cap * 2 : 16;
        ppage* q = (ppage*)realloc(d->pages, sizeof(ppage) * (size_t)nc);

        if (!q) {
            return;
        }

        d->pages = q;
        *cap = nc;
    }

    p = &d->pages[d->npages++];
    memset(p, 0, sizeof(*p));
    p->ref = ref;
    p->dict = node;
    p->resources = in->resources;

    if (in->has_media) {
        memcpy(p->media, in->media, sizeof(p->media));
    } else {
        p->media[2] = 612;
        p->media[3] = 792;
    }

    if (in->has_crop) {
        memcpy(p->crop, in->crop, sizeof(p->crop));

        /* the crop box within the media box */
        p->crop[0] = p->crop[0] > p->media[0] ? p->crop[0] : p->media[0];
        p->crop[1] = p->crop[1] > p->media[1] ? p->crop[1] : p->media[1];
        p->crop[2] = p->crop[2] < p->media[2] ? p->crop[2] : p->media[2];
        p->crop[3] = p->crop[3] < p->media[3] ? p->crop[3] : p->media[3];

        if (p->crop[2] - p->crop[0] < 1 || p->crop[3] - p->crop[1] < 1) {
            memcpy(p->crop, p->media, sizeof(p->crop));
        }
    } else {
        memcpy(p->crop, p->media, sizeof(p->crop));
    }

    p->rotate = ((in->rotate % 360) + 360) % 360 / 90 * 90;
}

static void walk_pages(pdoc* d, pv ref, inherit in, int depth, int32_t* cap, unsigned char* seen) {
    pv node = pd_resolve(d, ref), kids, v;
    const char* type;
    int32_t i;

    if (depth > 64 || node.t != PV_DICT) {
        return;
    }

    if (ref.t == PV_REF && ref.u.i > 0 && ref.u.i < d->nxref) {
        if (seen[ref.u.i]) {
            return;
        }

        seen[ref.u.i] = 1;
    }

    v = pd_get(d, node, "Resources");

    if (v.t == PV_DICT) {
        in.resources = v;
    }

    if (pd_rect(d, pd_get(d, node, "MediaBox"), in.media)) {
        in.has_media = 1;
    }

    if (pd_rect(d, pd_get(d, node, "CropBox"), in.crop)) {
        in.has_crop = 1;
    }

    v = pd_get(d, node, "Rotate");

    if (v.t == PV_INT) {
        in.rotate = (int32_t)v.u.i;
    }

    type = pd_getname(d, node, "Type");
    kids = pd_get(d, node, "Kids");

    if (kids.t == PV_ARRAY && !(type && strcmp(type, "Page") == 0)) {
        for (i = 0; i < kids.n; i++) {
            walk_pages(d, kids.u.a[i], in, depth + 1, cap, seen);
        }

        return;
    }

    if ((type && strcmp(type, "Page") == 0) || pd_get_raw(node, "Contents").t != PV_NULL) {
        add_page(d, ref, node, &in, cap);
    }
}

int pd_pages_load(pdoc* d) {
    pv root = pd_get(d, d->trailer, "Root");
    inherit in;
    int32_t cap = 0;
    unsigned char* seen = (unsigned char*)calloc((size_t)d->nxref + 1, 1);

    if (!seen) {
        return -1;
    }

    memset(&in, 0, sizeof(in));
    free(d->pages);
    d->pages = NULL;
    d->npages = 0;
    walk_pages(d, pd_get_raw(root, "Pages"), in, 0, &cap, seen);

    if (d->npages == 0) {   /* a broken tree: every page object in the file, in order */
        int32_t i;

        memset(seen, 0, (size_t)d->nxref + 1);

        for (i = 1; i < d->nxref; i++) {
            pv v = d->xref[i].type ? pd_object(d, i) : pv_null();

            if (v.t == PV_DICT && pv_is_name(pd_get_raw(v, "Type"), "Page")) {
                walk_pages(d, pv_ref(i, d->xref[i].gen), in, 0, &cap, seen);
            }
        }
    }

    free(seen);
    d->acroform = pd_get(d, root, "AcroForm");
    return d->npages > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* the public face                                                    */
/* ------------------------------------------------------------------ */

static pd_status open_doc(pdoc* d, const char* password) {
    int c;

    if (d->len < 8) {
        snprintf(d->error, sizeof(d->error), "not a PDF file (too short)");
        return PD_ERR_FORMAT;
    }

    if (find_bytes(d->data, d->len < 1024 ? d->len : 1024, 0, "%PDF") < 0 && find_bytes(d->data, d->len, 0, " obj") < 0) {
        snprintf(d->error, sizeof(d->error), "not a PDF file");
        return PD_ERR_FORMAT;
    }

    if (read_xref(d) != 0) {
        snprintf(d->error, sizeof(d->error), "no document catalog found");
        return PD_ERR_FORMAT;
    }

    c = setup_crypt(d, password);

    if (c < 0) {
        return PD_ERR_FORMAT;
    }

    if (c > 0) {
        snprintf(d->error, sizeof(d->error), "the file needs a password");
        return PD_ERR_STATE;
    }

    if (pd_pages_load(d) != 0) {
        snprintf(d->error, sizeof(d->error), "no pages found");
        return PD_ERR_FORMAT;
    }

    d->cache = (ppage_cache**)calloc((size_t)d->npages, sizeof(ppage_cache*));
    return d->cache ? PD_OK : PD_ERR_NOMEM;
}

pd_status pd_pdfr_open_memory(const void* data, size_t len, const char* password, pd_pdfr** out) {
    pdoc* d;
    pd_status st;

    if (!out || (!data && len)) {
        return PD_ERR_ARG;
    }

    *out = NULL;
    d = (pdoc*)calloc(1, sizeof(pdoc));

    if (!d) {
        return PD_ERR_NOMEM;
    }

    d->data = (unsigned char*)malloc(len + 1);

    if (!d->data) {
        free(d);
        return PD_ERR_NOMEM;
    }

    memcpy(d->data, data, len);
    d->data[len] = 0;
    d->len = len;
    st = open_doc(d, password);
    *out = d;   /* even when it failed: the error and whether it needs a password are asked of it */
    return st;
}

pd_status pd_pdfr_open_file(const char* path, const char* password, pd_pdfr** out) {
    FILE* fp;
    long n;
    unsigned char* data;
    pd_status st;

    if (!path || !out) {
        return PD_ERR_ARG;
    }

    *out = NULL;
    fp = fopen(path, "rb");

    if (!fp) {
        return PD_ERR_IO;
    }

    if (fseek(fp, 0, SEEK_END) || (n = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return PD_ERR_IO;
    }

    data = (unsigned char*)malloc((size_t)n + 1);

    if (!data || fread(data, 1, (size_t)n, fp) != (size_t)n) {
        free(data);
        fclose(fp);
        return PD_ERR_IO;
    }

    fclose(fp);
    st = pd_pdfr_open_memory(data, (size_t)n, password, out);
    free(data);
    return st;
}

void pd_pdfr_free(pd_pdfr* d) {
    if (!d) {
        return;
    }

    pd_pages_cache_free(d);
    pd_glyph_cache_free(d);
    pf_free_all(d);
    free(d->cache);
    free(d->pages);
    free(d->xref);
    free(d->data);
    pa_free(d->arena);
    free(d);
}

const char* pd_pdfr_error(const pd_pdfr* d) {
    return d ? d->error : "no document";
}

int32_t pd_pdfr_encrypted(const pd_pdfr* d) {
    return d && d->crypt && !d->keylen ? 1 : 0;
}

void pd_pdfr_add_font_dir(pd_pdfr* d, const char* dir) {
    if (d && dir && d->nfont_dirs < 4) {
        snprintf(d->font_dirs[d->nfont_dirs++], sizeof(d->font_dirs[0]), "%s", dir);
    }
}

int32_t pd_pdfr_page_count(const pd_pdfr* d) {
    return d && d->cache ? d->npages : 0;
}

pd_status pd_pdfr_page_size(pd_pdfr* d, int32_t page, double* w, double* h) {
    const ppage* p;
    double pw, ph;

    if (!d || !d->cache || page < 0 || page >= d->npages) {
        return PD_ERR_RANGE;
    }

    p = &d->pages[page];
    pw = p->crop[2] - p->crop[0];
    ph = p->crop[3] - p->crop[1];

    {
        double uu = pd_getnum(d, p->dict, "UserUnit", 1);

        if (uu > 0 && uu != 1) {
            pw *= uu;
            ph *= uu;
        }
    }

    if (p->rotate == 90 || p->rotate == 270) {
        double t = pw;

        pw = ph;
        ph = t;
    }

    if (w) {
        *w = pw;
    }

    if (h) {
        *h = ph;
    }

    return PD_OK;
}

pd_status pd_pdfr_info(pd_pdfr* d, const char* key, char* buf, size_t cap) {
    pv v;
    char* s;
    size_t n;

    if (!d || !key || !buf || !cap) {
        return PD_ERR_ARG;
    }

    buf[0] = 0;
    v = pd_get(d, pd_get(d, d->trailer, "Info"), key);

    if (v.t != PV_STRING) {
        return PD_ERR_RANGE;
    }

    s = pd_text_utf8(v, &n);

    if (!s) {
        return PD_ERR_NOMEM;
    }

    snprintf(buf, cap, "%s", s);
    free(s);
    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* CCITT fax (Group 3 and 4)                                          */
/* ------------------------------------------------------------------ */

/* run-length codes: {bits, code, run} for white and black, terminating and make-up codes */
typedef struct {
    uint8_t bits;
    uint16_t code;
    int16_t run;
} fcode;

static const fcode fax_white[] = {
    {8, 0x35, 0}, {6, 0x7, 1}, {4, 0x7, 2}, {4, 0x8, 3}, {4, 0xB, 4}, {4, 0xC, 5}, {4, 0xE, 6}, {4, 0xF, 7},
    {5, 0x13, 8}, {5, 0x14, 9}, {5, 0x7, 10}, {5, 0x8, 11}, {6, 0x8, 12}, {6, 0x3, 13}, {6, 0x34, 14},
    {6, 0x35, 15}, {6, 0x2A, 16}, {6, 0x2B, 17}, {7, 0x27, 18}, {7, 0xC, 19}, {7, 0x8, 20}, {7, 0x17, 21},
    {7, 0x3, 22}, {7, 0x4, 23}, {7, 0x28, 24}, {7, 0x2B, 25}, {7, 0x13, 26}, {7, 0x24, 27}, {7, 0x18, 28},
    {8, 0x2, 29}, {8, 0x3, 30}, {8, 0x1A, 31}, {8, 0x1B, 32}, {8, 0x12, 33}, {8, 0x13, 34}, {8, 0x14, 35},
    {8, 0x15, 36}, {8, 0x16, 37}, {8, 0x17, 38}, {8, 0x28, 39}, {8, 0x29, 40}, {8, 0x2A, 41}, {8, 0x2B, 42},
    {8, 0x2C, 43}, {8, 0x2D, 44}, {8, 0x4, 45}, {8, 0x5, 46}, {8, 0xA, 47}, {8, 0xB, 48}, {8, 0x52, 49},
    {8, 0x53, 50}, {8, 0x54, 51}, {8, 0x55, 52}, {8, 0x24, 53}, {8, 0x25, 54}, {8, 0x58, 55}, {8, 0x59, 56},
    {8, 0x5A, 57}, {8, 0x5B, 58}, {8, 0x4A, 59}, {8, 0x4B, 60}, {8, 0x32, 61}, {8, 0x33, 62}, {8, 0x34, 63},
    {5, 0x1B, 64}, {5, 0x12, 128}, {6, 0x17, 192}, {7, 0x37, 256}, {8, 0x36, 320}, {8, 0x37, 384},
    {8, 0x64, 448}, {8, 0x65, 512}, {8, 0x68, 576}, {8, 0x67, 640}, {9, 0xCC, 704}, {9, 0xCD, 768},
    {9, 0xD2, 832}, {9, 0xD3, 896}, {9, 0xD4, 960}, {9, 0xD5, 1024}, {9, 0xD6, 1088}, {9, 0xD7, 1152},
    {9, 0xD8, 1216}, {9, 0xD9, 1280}, {9, 0xDA, 1344}, {9, 0xDB, 1408}, {9, 0x98, 1472}, {9, 0x99, 1536},
    {9, 0x9A, 1600}, {6, 0x18, 1664}, {9, 0x9B, 1728},
    {11, 0x8, 1792}, {11, 0xC, 1856}, {11, 0xD, 1920}, {12, 0x12, 1984}, {12, 0x13, 2048}, {12, 0x14, 2112},
    {12, 0x15, 2176}, {12, 0x16, 2240}, {12, 0x17, 2304}, {12, 0x1C, 2368}, {12, 0x1D, 2432}, {12, 0x1E, 2496},
    {12, 0x1F, 2560}
};

static const fcode fax_black[] = {
    {10, 0x37, 0}, {3, 0x2, 1}, {2, 0x3, 2}, {2, 0x2, 3}, {3, 0x3, 4}, {4, 0x3, 5}, {4, 0x2, 6}, {5, 0x3, 7},
    {6, 0x5, 8}, {6, 0x4, 9}, {7, 0x4, 10}, {7, 0x5, 11}, {7, 0x7, 12}, {8, 0x4, 13}, {8, 0x7, 14},
    {9, 0x18, 15}, {10, 0x17, 16}, {10, 0x18, 17}, {10, 0x8, 18}, {11, 0x67, 19}, {11, 0x68, 20},
    {11, 0x6C, 21}, {11, 0x37, 22}, {11, 0x28, 23}, {11, 0x17, 24}, {11, 0x18, 25}, {12, 0xCA, 26},
    {12, 0xCB, 27}, {12, 0xCC, 28}, {12, 0xCD, 29}, {12, 0x68, 30}, {12, 0x69, 31}, {12, 0x6A, 32},
    {12, 0x6B, 33}, {12, 0xD2, 34}, {12, 0xD3, 35}, {12, 0xD4, 36}, {12, 0xD5, 37}, {12, 0xD6, 38},
    {12, 0xD7, 39}, {12, 0x6C, 40}, {12, 0x6D, 41}, {12, 0xDA, 42}, {12, 0xDB, 43}, {12, 0x54, 44},
    {12, 0x55, 45}, {12, 0x56, 46}, {12, 0x57, 47}, {12, 0x64, 48}, {12, 0x65, 49}, {12, 0x52, 50},
    {12, 0x53, 51}, {12, 0x24, 52}, {12, 0x37, 53}, {12, 0x38, 54}, {12, 0x27, 55}, {12, 0x28, 56},
    {12, 0x58, 57}, {12, 0x59, 58}, {12, 0x2B, 59}, {12, 0x2C, 60}, {12, 0x5A, 61}, {12, 0x66, 62},
    {12, 0x67, 63}, {10, 0xF, 64}, {12, 0xC8, 128}, {12, 0xC9, 192}, {12, 0x5B, 256}, {12, 0x33, 320},
    {12, 0x34, 384}, {12, 0x35, 448}, {13, 0x6C, 512}, {13, 0x6D, 576}, {13, 0x4A, 640}, {13, 0x4B, 704},
    {13, 0x4C, 768}, {13, 0x4D, 832}, {13, 0x72, 896}, {13, 0x73, 960}, {13, 0x74, 1024}, {13, 0x75, 1088},
    {13, 0x76, 1152}, {13, 0x77, 1216}, {13, 0x52, 1280}, {13, 0x53, 1344}, {13, 0x54, 1408},
    {13, 0x55, 1472}, {13, 0x5A, 1536}, {13, 0x5B, 1600}, {13, 0x64, 1664}, {13, 0x65, 1728},
    {11, 0x8, 1792}, {11, 0xC, 1856}, {11, 0xD, 1920}, {12, 0x12, 1984}, {12, 0x13, 2048}, {12, 0x14, 2112},
    {12, 0x15, 2176}, {12, 0x16, 2240}, {12, 0x17, 2304}, {12, 0x1C, 2368}, {12, 0x1D, 2432}, {12, 0x1E, 2496},
    {12, 0x1F, 2560}
};

typedef struct {
    const unsigned char* p;
    size_t n, bit;
} fbits;

static int fpeek(fbits* b, int k) {
    int v = 0, i;

    for (i = 0; i < k; i++) {
        size_t bit = b->bit + (size_t)i;
        int x = bit / 8 < b->n ? (b->p[bit / 8] >> (7 - bit % 8)) & 1 : 0;

        v = (v << 1) | x;
    }

    return v;
}

/* one run (make-up codes added up); -1 at an error */
static int frun(fbits* b, int black) {
    const fcode* t = black ? fax_black : fax_white;
    size_t nt = black ? sizeof(fax_black) / sizeof(fax_black[0]) : sizeof(fax_white) / sizeof(fax_white[0]);
    int total = 0;

    for (;;) {
        size_t i;
        int found = -1;

        if (b->bit / 8 >= b->n) {
            return -1;
        }

        for (i = 0; i < nt; i++) {
            if (fpeek(b, t[i].bits) == t[i].code) {
                found = (int)i;
                break;
            }
        }

        if (found < 0) {
            return -1;
        }

        b->bit += t[found].bits;
        total += t[found].run;

        if (t[found].run < 64) {
            return total;
        }
    }
}

unsigned char* pd_ccitt_decode(const unsigned char* data, size_t n, int32_t k, int32_t columns, int32_t rows,
                               int black1, int byte_align, size_t* outlen) {
    fbits b;
    int32_t* ref, *cur, nref, ncur, row;
    size_t rowbytes = (size_t)(columns + 7) / 8;
    pbuf o;

    if (columns <= 0 || columns > 1 << 20) {
        return NULL;
    }

    b.p = data;
    b.n = n;
    b.bit = 0;
    memset(&o, 0, sizeof(o));
    ref = (int32_t*)malloc(sizeof(int32_t) * (size_t)(columns + 4));
    cur = (int32_t*)malloc(sizeof(int32_t) * (size_t)(columns + 4));

    if (!ref || !cur) {
        free(ref);
        free(cur);
        return NULL;
    }

    ref[0] = columns;
    ref[1] = columns;
    nref = 1;

    for (row = 0; rows <= 0 || row < rows; row++) {
        int two_d = k < 0, color = 0;
        int32_t a0 = -1, i;
        unsigned char* line;

        if (b.bit / 8 >= n) {
            break;
        }

        if (byte_align && k >= 0) {
            b.bit = (b.bit + 7) / 8 * 8;
        }

        while (fpeek(&b, 12) == 1) {    /* EOL */
            b.bit += 12;

            if (k > 0) {
                two_d = !fpeek(&b, 1);
                b.bit++;
            }
        }

        if (fpeek(&b, 24) == 0x001001 && k < 0) {   /* EOFB */
            break;
        }

        ncur = 0;

        if (two_d) {
            while (a0 < columns) {
                int32_t b1, b2, bi;

                /* b1: the first changing element on the reference line right of a0 that changes to the other
                   colour (even ones change to black) */
                for (bi = 0; bi < nref && !(ref[bi] > a0 && (bi & 1) == color); bi++) {
                }

                b1 = bi < nref ? ref[bi] : columns;
                b2 = bi + 1 < nref ? ref[bi + 1] : columns;

                if (fpeek(&b, 4) == 1) {    /* pass */
                    b.bit += 4;
                    a0 = b2;
                } else if (fpeek(&b, 3) == 1) {     /* horizontal */
                    int r1, r2;
                    int32_t start = a0 < 0 ? 0 : a0;

                    b.bit += 3;
                    r1 = frun(&b, color);
                    r2 = frun(&b, !color);

                    if (r1 < 0 || r2 < 0) {
                        goto done;
                    }

                    cur[ncur++] = start + r1 < columns ? start + r1 : columns;
                    cur[ncur++] = start + r1 + r2 < columns ? start + r1 + r2 : columns;
                    a0 = start + r1 + r2;
                } else {
                    int dlt;

                    if (fpeek(&b, 1) == 1) {
                        b.bit += 1;
                        dlt = 0;
                    } else if (fpeek(&b, 3) == 3) {
                        b.bit += 3;
                        dlt = 1;
                    } else if (fpeek(&b, 3) == 2) {
                        b.bit += 3;
                        dlt = -1;
                    } else if (fpeek(&b, 6) == 3) {
                        b.bit += 6;
                        dlt = 2;
                    } else if (fpeek(&b, 6) == 2) {
                        b.bit += 6;
                        dlt = -2;
                    } else if (fpeek(&b, 7) == 3) {
                        b.bit += 7;
                        dlt = 3;
                    } else if (fpeek(&b, 7) == 2) {
                        b.bit += 7;
                        dlt = -3;
                    } else {
                        goto done;
                    }

                    a0 = b1 + dlt;

                    if (a0 < 0) {
                        a0 = 0;
                    }

                    if (a0 > columns) {
                        a0 = columns;
                    }

                    cur[ncur++] = a0;
                    color = !color;
                }

                if (ncur >= columns + 2) {
                    break;
                }
            }
        } else {
            int32_t x = 0;

            while (x < columns) {
                int r = frun(&b, color);

                if (r < 0) {
                    goto done;
                }

                x += r;
                cur[ncur++] = x < columns ? x : columns;
                color = !color;
            }
        }

        /* the line from its changing elements: white first */
        line = (unsigned char*)calloc(rowbytes, 1);

        if (!line) {
            break;
        }

        {
            int32_t x = 0, c = 0;

            for (i = 0; i <= ncur; i++) {
                int32_t e = i < ncur ? cur[i] : columns, xx;

                for (xx = x; xx < e && xx < columns; xx++) {
                    if (c) {
                        line[xx / 8] |= (unsigned char)(0x80 >> (xx % 8));
                    }
                }

                x = e > x ? e : x;
                c = !c;
            }
        }

        if (!black1) {
            size_t q;

            for (q = 0; q < rowbytes; q++) {
                line[q] = (unsigned char)~line[q];
            }
        }

        qb_put(&o, line, rowbytes);
        free(line);

        memcpy(ref, cur, sizeof(int32_t) * (size_t)ncur);
        nref = ncur;
        ref[nref] = columns;
        ref[nref + 1] = columns;
    }

done:
    free(ref);
    free(cur);

    if (rows > 0 && o.n < rowbytes * (size_t)rows) {    /* the rest of the image white */
        size_t need = rowbytes * (size_t)rows - o.n;
        unsigned char* w = (unsigned char*)malloc(need);

        if (w) {
            memset(w, black1 ? 0 : 0xFF, need);
            qb_put(&o, w, need);
            free(w);
        }
    }

    *outlen = o.n;
    return o.p;
}
