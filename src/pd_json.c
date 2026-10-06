/*
 * Parade internal JData/BJData codec, ported from mimamo's
 * mmm_json.hpp / mmm_bjdata.hpp / mmm_base64.hpp conventions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "pd_json.h"

/* ------------------------------------------------------------------ */
/* base64                                                             */
/* ------------------------------------------------------------------ */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* pj_base64_encode(const void* data, size_t n, size_t* outlen) {
    const unsigned char* in = (const unsigned char*)data;
    char* out = (char*)malloc((n + 2) / 3 * 4 + 1);
    size_t i = 0, o = 0;

    if (!out) {
        return NULL;
    }

    for (; i + 3 <= n; i += 3) {
        out[o++] = b64[in[i] >> 2];
        out[o++] = b64[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
        out[o++] = b64[((in[i + 1] & 0x0f) << 2) | (in[i + 2] >> 6)];
        out[o++] = b64[in[i + 2] & 0x3f];
    }

    if (i < n) {
        out[o++] = b64[in[i] >> 2];

        if (n - i == 1) {
            out[o++] = b64[(in[i] & 0x03) << 4];
            out[o++] = '=';
        } else {
            out[o++] = b64[((in[i] & 0x03) << 4) | (in[i + 1] >> 4)];
            out[o++] = b64[(in[i + 1] & 0x0f) << 2];
        }

        out[o++] = '=';
    }

    out[o] = '\0';
    *outlen = o;
    return out;
}

/* whitespace and stray bytes are skipped; a count not divisible by 4 fails */
int pj_base64_decode(const char* text, size_t len, unsigned char** out, size_t* outlen) {
    unsigned char table[256], block[4];
    size_t i, count = 0, o = 0;
    int have = 0, pad = 0;

    memset(table, 0x80, sizeof(table));

    for (i = 0; i < 64; i++) {
        table[(unsigned char)b64[i]] = (unsigned char)i;
    }

    table['='] = 0;

    for (i = 0; i < len; i++) {
        count += table[(unsigned char)text[i]] != 0x80;
    }

    *out = (unsigned char*)malloc(count / 4 * 3 + 1);
    *outlen = 0;

    if (!*out) {
        return -1;
    }

    if (count % 4) {
        free(*out);
        *out = NULL;
        return -1;
    }

    for (i = 0; i < len; i++) {
        unsigned char v = table[(unsigned char)text[i]];

        if (v == 0x80) {
            continue;
        }

        pad += text[i] == '=';
        block[have++] = v;

        if (have < 4) {
            continue;
        }

        (*out)[o++] = (unsigned char)((block[0] << 2) | (block[1] >> 4));
        (*out)[o++] = (unsigned char)((block[1] << 4) | (block[2] >> 2));
        (*out)[o++] = (unsigned char)((block[2] << 6) | block[3]);
        have = 0;

        if (pad) {
            if (pad > 2) {
                free(*out);
                *out = NULL;
                return -1;
            }

            o -= (size_t)pad;
            break;
        }
    }

    *outlen = o;
    return 0;
}

/* ------------------------------------------------------------------ */
/* writer                                                             */
/* ------------------------------------------------------------------ */

static void flush(pj_writer* w) {
    if (w->nbuf && !w->err && w->fn(w->user, w->buf, w->nbuf)) {
        w->err = 1;
    }

    w->nbuf = 0;
}

static void emit(pj_writer* w, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;

    if (w->err) {
        return;
    }

    if (n > sizeof(w->buf)) {
        flush(w);

        if (!w->err && w->fn(w->user, p, n)) {
            w->err = 1;
        }

        return;
    }

    if (w->nbuf + n > sizeof(w->buf)) {
        flush(w);
    }

    memcpy(w->buf + w->nbuf, p, n);
    w->nbuf += n;
}

static void emit_c(pj_writer* w, char c) {
    emit(w, &c, 1);
}

static void emit_s(pj_writer* w, const char* s) {
    emit(w, s, strlen(s));
}

/* little-endian, byte by byte: the file is the same on every host */
static void put_le(pj_writer* w, uint64_t v, int bytes) {
    unsigned char b[8];
    int k;

    for (k = 0; k < bytes; k++) {
        b[k] = (unsigned char)(v >> (8 * k));
    }

    emit(w, b, (size_t)bytes);
}

/* a length in the smallest of i U I l L, as mmm_bjdata's put_size */
static void put_size(pj_writer* w, uint64_t n) {
    if (n <= 127u) {
        emit_c(w, 'i');
        put_le(w, n, 1);
    } else if (n <= 255u) {
        emit_c(w, 'U');
        put_le(w, n, 1);
    } else if (n <= 32767u) {
        emit_c(w, 'I');
        put_le(w, n, 2);
    } else if (n <= 2147483647u) {
        emit_c(w, 'l');
        put_le(w, n, 4);
    } else {
        emit_c(w, 'L');
        put_le(w, n, 8);
    }
}

/* the smallest integer marker covering [lo, hi] and its byte width */
static char int_marker(int64_t lo, int64_t hi, int* bytes) {
    if (lo >= -128 && hi <= 127) {
        *bytes = 1;
        return 'i';
    }

    if (lo >= 0 && hi <= 255) {
        *bytes = 1;
        return 'U';
    }

    if (lo >= -32768 && hi <= 32767) {
        *bytes = 2;
        return 'I';
    }

    if (lo >= 0 && hi <= 65535) {
        *bytes = 2;
        return 'u';
    }

    if (lo >= INT32_MIN && hi <= INT32_MAX) {
        *bytes = 4;
        return 'l';
    }

    if (lo >= 0 && hi <= (int64_t)UINT32_MAX) {
        *bytes = 4;
        return 'm';
    }

    *bytes = 8;
    return 'L';
}

static void newline(pj_writer* w, int depth) {
    int k;

    if (w->compact) {
        return;
    }

    emit_c(w, '\n');

    for (k = 0; k < depth; k++) {
        emit_c(w, ' ');
    }
}

/* separator and indentation before a value or a key */
static void prefix(pj_writer* w) {
    if (w->after_key) {
        w->after_key = 0;
        return;
    }

    if (w->depth > 0) {
        if (!w->binary) {
            if (w->count[w->depth] > 0) {
                emit_c(w, ',');
            }

            newline(w, w->depth);
        }

        w->count[w->depth]++;
    }
}

void pj_init(pj_writer* w, int binary, pd_writer fn, void* user) {
    memset(w, 0, sizeof(*w));
    w->binary = binary;
    w->fn = fn;
    w->user = user;
}

static void open_c(pj_writer* w, char c) {
    prefix(w);
    emit_c(w, c);

    if (w->depth + 1 >= PJ_MAX_DEPTH) {
        w->err = 1;
        return;
    }

    w->depth++;
    w->count[w->depth] = 0;
}

static void close_c(pj_writer* w, char c) {
    if (w->depth <= 0) {
        w->err = 1;
        return;
    }

    if (!w->binary && w->count[w->depth] > 0) {
        newline(w, w->depth - 1);
    }

    w->depth--;
    emit_c(w, c);
}

void pj_obj_begin(pj_writer* w) {
    open_c(w, '{');
}

void pj_obj_end(pj_writer* w) {
    close_c(w, '}');
}

void pj_arr_begin(pj_writer* w) {
    open_c(w, '[');
}

void pj_arr_end(pj_writer* w) {
    close_c(w, ']');
}

static void json_string(pj_writer* w, const char* s, size_t n) {
    size_t i;

    emit_c(w, '"');

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (c) {
            case '"':
                emit_s(w, "\\\"");
                break;

            case '\\':
                emit_s(w, "\\\\");
                break;

            case '\b':
                emit_s(w, "\\b");
                break;

            case '\f':
                emit_s(w, "\\f");
                break;

            case '\n':
                emit_s(w, "\\n");
                break;

            case '\r':
                emit_s(w, "\\r");
                break;

            case '\t':
                emit_s(w, "\\t");
                break;

            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    emit_s(w, buf);
                } else {
                    emit_c(w, (char)c);     /* UTF-8 passes through */
                }
        }
    }

    emit_c(w, '"');
}

void pj_key(pj_writer* w, const char* key) {
    size_t n = strlen(key);

    prefix(w);

    if (w->binary) {
        put_size(w, n);     /* a key is a length and its bytes, no 'S' */
        emit(w, key, n);
    } else {
        json_string(w, key, n);
        emit_s(w, ": ");
    }

    w->after_key = 1;
}

void pj_null(pj_writer* w) {
    prefix(w);
    emit_s(w, w->binary ? "Z" : "null");
}

void pj_bool(pj_writer* w, int v) {
    prefix(w);
    emit_s(w, w->binary ? (v ? "T" : "F") : (v ? "true" : "false"));
}

void pj_int(pj_writer* w, int64_t v) {
    prefix(w);

    if (w->binary) {
        int bytes;
        char m = int_marker(v, v, &bytes);
        emit_c(w, m);
        put_le(w, (uint64_t)v, bytes);
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)v);
        emit_s(w, buf);
    }
}

void pj_str(pj_writer* w, const char* s, size_t n) {
    prefix(w);

    if (w->binary) {
        emit_c(w, 'S');
        put_size(w, n);
        emit(w, s, n);
    } else {
        json_string(w, s, n);
    }
}

void pj_cstr(pj_writer* w, const char* s) {
    pj_str(w, s, strlen(s));
}

void pj_bytes(pj_writer* w, const void* data, size_t n) {
    if (w->binary) {
        prefix(w);
        emit_c(w, 'H');     /* JData: a byte stream is an H payload, not base64 */
        put_size(w, n);
        emit(w, data, n);
    } else {
        size_t len;
        char* t = pj_base64_encode(data, n, &len);

        if (!t) {
            w->err = 1;
            return;
        }

        pj_str(w, t, len);
        free(t);
    }
}

void pj_int_matrix(pj_writer* w, const int64_t* data, int64_t rows, int32_t cols) {
    int64_t k, total = rows * cols, lo = 0, hi = 0;

    if (!w->binary) {
        int64_t r;
        int32_t c;
        char buf[32];

        pj_arr_begin(w);

        for (r = 0; r < rows; r++) {    /* one compact row per line */
            prefix(w);
            emit_c(w, '[');

            for (c = 0; c < cols; c++) {
                snprintf(buf, sizeof(buf), c ? ",%lld" : "%lld", (long long)data[r * cols + c]);
                emit_s(w, buf);
            }

            emit_c(w, ']');
        }

        pj_arr_end(w);
        return;
    }

    for (k = 0; k < total; k++) {
        lo = data[k] < lo ? data[k] : lo;
        hi = data[k] > hi ? data[k] : hi;
    }

    {
        int bytes;
        char m = int_marker(lo, hi, &bytes);

        /* [$m#[$l#i2 rows cols then the payload: self-terminating, no ']' */
        prefix(w);
        emit_c(w, '[');
        emit_c(w, '$');
        emit_c(w, m);
        emit_c(w, '#');
        emit_c(w, '[');
        emit_c(w, '$');
        emit_c(w, 'l');
        emit_c(w, '#');
        put_size(w, 2);
        put_le(w, (uint64_t)rows, 4);
        put_le(w, (uint64_t)cols, 4);

        for (k = 0; k < total; k++) {
            put_le(w, (uint64_t)data[k], bytes);
        }
    }
}

int pj_finish(pj_writer* w) {
    if (!w->binary && !w->compact) {
        emit_c(w, '\n');
    }

    flush(w);
    return w->err || w->depth != 0;
}

/* ------------------------------------------------------------------ */
/* arena                                                              */
/* ------------------------------------------------------------------ */

typedef struct chunk {
    struct chunk* prev;
    size_t used, cap;
} chunk;

struct pj_doc {
    chunk* top;
    pj_node* root;
};

static void* arena(pj_doc* d, size_t n) {
    chunk* c = d->top;
    void* p;

    n = (n + 7) & ~(size_t)7;

    if (!c || c->used + n > c->cap) {
        size_t cap = n > 65536 ? n : 65536;
        chunk* nc = (chunk*)malloc(sizeof(chunk) + cap);

        if (!nc) {
            return NULL;
        }

        nc->prev = c;
        nc->used = 0;
        nc->cap = cap;
        d->top = c = nc;
    }

    p = (char*)(c + 1) + c->used;
    c->used += n;
    return p;
}

void pj_free(pj_doc* d) {
    if (!d) {
        return;
    }

    while (d->top) {
        chunk* p = d->top->prev;
        free(d->top);
        d->top = p;
    }

    free(d);
}

const pj_node* pj_root(const pj_doc* d) {
    return d ? d->root : NULL;
}

typedef struct {
    pj_doc* d;
    const unsigned char* p;
    size_t n, i;
    const char* err;
    int depth;
} parser;

static pj_node* node(parser* ps, int type) {
    pj_node* x = (pj_node*)arena(ps->d, sizeof(pj_node));

    if (!x) {
        ps->err = "out of memory";
        return NULL;
    }

    memset(x, 0, sizeof(*x));
    x->type = type;
    return x;
}

static void append(pj_node* parent, pj_node** tail, pj_node* kid) {
    if (*tail) {
        (*tail)->next = kid;
    } else {
        parent->child = kid;
    }

    *tail = kid;
    parent->n++;
}

static char* copy_bytes(parser* ps, const void* s, size_t n) {
    char* t = (char*)arena(ps->d, n + 1);

    if (!t) {
        ps->err = "out of memory";
        return NULL;
    }

    memcpy(t, s, n);
    t[n] = '\0';
    return t;
}

/* ------------------------------------------------------------------ */
/* JSON text reader                                                   */
/* ------------------------------------------------------------------ */

static void ws(parser* ps) {
    while (ps->i < ps->n && (ps->p[ps->i] == ' ' || ps->p[ps->i] == '\t' || ps->p[ps->i] == '\n' ||
                             ps->p[ps->i] == '\r')) {
        ps->i++;
    }
}

static int hex4(parser* ps, uint32_t* v) {
    int k;

    *v = 0;

    if (ps->i + 4 > ps->n) {
        return -1;
    }

    for (k = 0; k < 4; k++) {
        unsigned char c = ps->p[ps->i++];
        *v <<= 4;

        if (c >= '0' && c <= '9') {
            *v |= c - '0';
        } else if (c >= 'a' && c <= 'f') {
            *v |= c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            *v |= c - 'A' + 10;
        } else {
            return -1;
        }
    }

    return 0;
}

static size_t put_utf8(char* o, uint32_t cp) {
    if (cp < 0x80) {
        o[0] = (char)cp;
        return 1;
    }

    if (cp < 0x800) {
        o[0] = (char)(0xC0 | (cp >> 6));
        o[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }

    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }

    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* a quoted string, unescaped; the decoded text is never longer than the source */
static const char* json_str(parser* ps, size_t* len) {
    size_t start = ++ps->i, end = start, o = 0;
    char* out;

    while (end < ps->n && ps->p[end] != '"') {
        end += ps->p[end] == '\\' ? 2 : 1;
    }

    if (end >= ps->n) {
        ps->err = "unterminated string";
        return NULL;
    }

    out = (char*)arena(ps->d, end - start + 1);

    if (!out) {
        ps->err = "out of memory";
        return NULL;
    }

    while (ps->i < end) {
        unsigned char c = ps->p[ps->i++];

        if (c < 0x20) {
            ps->err = "control character in string";
            return NULL;
        }

        if (c != '\\') {
            out[o++] = (char)c;
            continue;
        }

        c = ps->p[ps->i++];

        switch (c) {
            case '"':
            case '\\':
            case '/':
                out[o++] = (char)c;
                break;

            case 'b':
                out[o++] = '\b';
                break;

            case 'f':
                out[o++] = '\f';
                break;

            case 'n':
                out[o++] = '\n';
                break;

            case 'r':
                out[o++] = '\r';
                break;

            case 't':
                out[o++] = '\t';
                break;

            case 'u': {
                uint32_t cp, lo;

                if (hex4(ps, &cp)) {
                    ps->err = "bad \\u escape";
                    return NULL;
                }

                /* a surrogate pair is one code point (mimamo decodes them separately) */
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->i + 6 <= end && ps->p[ps->i] == '\\' &&
                        ps->p[ps->i + 1] == 'u') {
                    size_t save = ps->i;

                    ps->i += 2;

                    if (!hex4(ps, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        ps->i = save;
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;    /* lone surrogate */
                }

                o += put_utf8(out + o, cp);
                break;
            }

            default:
                ps->err = "unknown escape";
                return NULL;
        }
    }

    ps->i = end + 1;
    out[o] = '\0';
    *len = o;
    return out;
}

static pj_node* json_value(parser* ps);

static pj_node* json_number(parser* ps) {
    size_t s = ps->i;
    int neg = 0, frac = 0;
    uint64_t mag = 0;
    int overflow = 0;
    pj_node* x;

    if (ps->p[ps->i] == '-') {
        neg = 1;
        ps->i++;
    }

    if (ps->i >= ps->n || ps->p[ps->i] < '0' || ps->p[ps->i] > '9') {
        ps->err = "bad number";
        return NULL;
    }

    while (ps->i < ps->n && ps->p[ps->i] >= '0' && ps->p[ps->i] <= '9') {
        unsigned d = ps->p[ps->i++] - '0';

        if (mag > (UINT64_MAX - d) / 10) {
            overflow = 1;
        } else {
            mag = mag * 10 + d;
        }
    }

    if (ps->i < ps->n && ps->p[ps->i] == '.') {
        frac = 1;
        ps->i++;

        while (ps->i < ps->n && ps->p[ps->i] >= '0' && ps->p[ps->i] <= '9') {
            ps->i++;
        }
    }

    if (ps->i < ps->n && (ps->p[ps->i] == 'e' || ps->p[ps->i] == 'E')) {
        frac = 1;
        ps->i++;

        if (ps->i < ps->n && (ps->p[ps->i] == '+' || ps->p[ps->i] == '-')) {
            ps->i++;
        }

        while (ps->i < ps->n && ps->p[ps->i] >= '0' && ps->p[ps->i] <= '9') {
            ps->i++;
        }
    }

    if (!frac && !overflow && (neg ? mag <= (uint64_t)INT64_MAX + 1 : mag <= (uint64_t)INT64_MAX)) {
        x = node(ps, PJ_INT);

        if (x) {
            x->i = neg ? (int64_t)(0 - mag) : (int64_t)mag;
        }

        return x;
    }

    if (!frac && !overflow && !neg) {
        x = node(ps, PJ_UINT);

        if (x) {
            x->u = mag;
        }

        return x;
    }

    {
        /* floats are not used by documents; parsed for completeness */
        char buf[64];
        size_t k = ps->i - s < sizeof(buf) - 1 ? ps->i - s : sizeof(buf) - 1;

        memcpy(buf, ps->p + s, k);
        buf[k] = '\0';
        x = node(ps, PJ_FLOAT);

        if (x) {
            x->f = strtod(buf, NULL);
        }

        return x;
    }
}

static int lit(parser* ps, const char* word) {
    size_t k = strlen(word);

    if (ps->i + k > ps->n || memcmp(ps->p + ps->i, word, k)) {
        ps->err = "bad literal";
        return -1;
    }

    ps->i += k;
    return 0;
}

static pj_node* json_value(parser* ps) {
    pj_node* x, *tail = NULL;

    ws(ps);

    if (ps->i >= ps->n) {
        ps->err = "unexpected end";
        return NULL;
    }

    if (++ps->depth > PJ_MAX_DEPTH) {
        ps->err = "nesting too deep";
        return NULL;
    }

    switch (ps->p[ps->i]) {
        case '{':
            x = node(ps, PJ_OBJ);
            ps->i++;
            ws(ps);

            if (x && ps->i < ps->n && ps->p[ps->i] == '}') {
                ps->i++;
                break;
            }

            while (x) {
                const char* k;
                size_t kl;
                pj_node* v;

                ws(ps);

                if (ps->i >= ps->n || ps->p[ps->i] != '"') {
                    ps->err = "expected a key";
                    return NULL;
                }

                k = json_str(ps, &kl);

                if (!k) {
                    return NULL;
                }

                ws(ps);

                if (ps->i >= ps->n || ps->p[ps->i] != ':') {
                    ps->err = "expected ':'";
                    return NULL;
                }

                ps->i++;
                v = json_value(ps);

                if (!v) {
                    return NULL;
                }

                v->key = k;
                v->keylen = kl;
                append(x, &tail, v);
                ws(ps);

                if (ps->i < ps->n && ps->p[ps->i] == ',') {
                    ps->i++;
                    continue;
                }

                if (ps->i < ps->n && ps->p[ps->i] == '}') {
                    ps->i++;
                    break;
                }

                ps->err = "expected ',' or '}'";
                return NULL;
            }

            break;

        case '[':
            x = node(ps, PJ_ARR);
            ps->i++;
            ws(ps);

            if (x && ps->i < ps->n && ps->p[ps->i] == ']') {
                ps->i++;
                break;
            }

            while (x) {
                pj_node* v = json_value(ps);

                if (!v) {
                    return NULL;
                }

                append(x, &tail, v);
                ws(ps);

                if (ps->i < ps->n && ps->p[ps->i] == ',') {
                    ps->i++;
                    continue;
                }

                if (ps->i < ps->n && ps->p[ps->i] == ']') {
                    ps->i++;
                    break;
                }

                ps->err = "expected ',' or ']'";
                return NULL;
            }

            break;

        case '"':
            x = node(ps, PJ_STR);

            if (x) {
                x->s = json_str(ps, &x->len);

                if (!x->s) {
                    return NULL;
                }
            }

            break;

        case 't':
        case 'f':
            x = node(ps, PJ_BOOL);

            if (x) {
                x->i = ps->p[ps->i] == 't';

                if (lit(ps, x->i ? "true" : "false")) {
                    return NULL;
                }
            }

            break;

        case 'n':
            x = node(ps, PJ_NULL);

            if (x && lit(ps, "null")) {
                return NULL;
            }

            break;

        default:
            x = json_number(ps);
    }

    ps->depth--;
    return x;
}

/* ------------------------------------------------------------------ */
/* BJData reader                                                      */
/* ------------------------------------------------------------------ */

static int get_le(parser* ps, int bytes, uint64_t* v) {
    int k;

    if (ps->i + (size_t)bytes > ps->n) {
        ps->err = "payload ends early";
        return -1;
    }

    *v = 0;

    for (k = 0; k < bytes; k++) {
        *v |= (uint64_t)ps->p[ps->i++] << (8 * k);
    }

    return 0;
}

/* byte width of a fixed-size marker; 0 if the marker is not one */
static int fixed_size(char m) {
    switch (m) {
        case 'i':
        case 'U':
        case 'C':
        case 'B':
            return 1;

        case 'I':
        case 'u':
        case 'h':
            return 2;

        case 'l':
        case 'm':
        case 'd':
            return 4;

        case 'L':
        case 'M':
        case 'D':
            return 8;
    }

    return 0;
}

static double half_to_double(uint16_t h) {
    int e = (h >> 10) & 0x1F;
    double m = h & 0x3FF, v;


    if (e == 0) {
        v = m / 1024.0 / 16384.0;
    } else if (e == 31) {
        v = m ? NAN : INFINITY;
    } else {
        v = (1.0 + m / 1024.0);

        for (; e > 15; e--) {
            v *= 2;
        }

        for (; e < 15; e++) {
            v /= 2;
        }
    }

    return (h & 0x8000) ? -v : v;
}

/* a fixed-size scalar of marker m into node x */
static int scalar(parser* ps, char m, pj_node* x) {
    uint64_t v;
    int bytes = fixed_size(m);

    if (!bytes || get_le(ps, bytes, &v)) {
        if (!ps->err) {
            ps->err = "not a fixed-size marker";
        }

        return -1;
    }

    switch (m) {
        case 'i':
            x->type = PJ_INT;
            x->i = (int8_t)v;
            break;

        case 'I':
            x->type = PJ_INT;
            x->i = (int16_t)v;
            break;

        case 'l':
            x->type = PJ_INT;
            x->i = (int32_t)v;
            break;

        case 'L':
            x->type = PJ_INT;
            x->i = (int64_t)v;
            break;

        case 'M':
            if (v > (uint64_t)INT64_MAX) {
                x->type = PJ_UINT;
                x->u = v;
            } else {
                x->type = PJ_INT;
                x->i = (int64_t)v;
            }

            break;

        case 'C':
            x->type = PJ_STR;
            x->s = copy_bytes(ps, &v, 1);
            x->len = 1;
            break;

        case 'h':
            x->type = PJ_FLOAT;
            x->f = half_to_double((uint16_t)v);
            break;

        case 'd': {
            uint32_t b = (uint32_t)v;
            float f;
            memcpy(&f, &b, 4);
            x->type = PJ_FLOAT;
            x->f = f;
            break;
        }

        case 'D':
            x->type = PJ_FLOAT;
            memcpy(&x->f, &v, 8);
            break;

        default:    /* U u m B */
            x->type = PJ_INT;
            x->i = (int64_t)v;
    }

    return 0;
}

static int bj_size(parser* ps, uint64_t* n) {
    pj_node t;
    char m;

    if (ps->i >= ps->n) {
        ps->err = "a size was expected";
        return -1;
    }

    m = (char)ps->p[ps->i++];

    if (strchr("iUIulmLM", m) == NULL || m == '\0') {
        ps->err = "not a size marker";
        return -1;
    }

    memset(&t, 0, sizeof(t));

    if (scalar(ps, m, &t)) {
        return -1;
    }

    if (t.type != PJ_INT || t.i < 0) {
        ps->err = "negative or huge size";
        return -1;
    }

    *n = (uint64_t)t.i;
    return 0;
}

static pj_node* bj_value(parser* ps);

/* elements of a typed N-D payload, rebuilt as nested arrays in row-major order */
static pj_node* bj_nd(parser* ps, char m, const uint64_t* dims, int nd, int colmajor, size_t base, int axis,
                      uint64_t offset, const uint64_t* rstride, const uint64_t* cstride) {
    pj_node* x = node(ps, PJ_ARR), *tail = NULL;
    uint64_t k;
    int bytes = fixed_size(m);

    if (!x) {
        return NULL;
    }

    for (k = 0; k < dims[axis]; k++) {
        pj_node* kid;

        if (axis + 1 < nd) {
            kid = bj_nd(ps, m, dims, nd, colmajor, base, axis + 1, offset + k * (colmajor ? cstride[axis] :
                        rstride[axis]), rstride, cstride);
        } else {
            uint64_t idx = offset + k * (colmajor ? cstride[axis] : rstride[axis]);
            size_t save = ps->i;

            kid = node(ps, PJ_NULL);
            ps->i = base + (size_t)(idx * (uint64_t)bytes);

            if (kid && scalar(ps, m, kid)) {
                return NULL;
            }

            ps->i = save;
        }

        if (!kid) {
            return NULL;
        }

        append(x, &tail, kid);
    }

    return x;
}

/* the body of an array or object after '[' or '{' */
static pj_node* bj_container(parser* ps, int is_obj) {
    pj_node* x = node(ps, is_obj ? PJ_OBJ : PJ_ARR), *tail = NULL;
    char type = 0;
    uint64_t count = 0, dims[32], k;
    int nd = 0, counted = 0, colmajor = 0;

    if (!x) {
        return NULL;
    }

    if (ps->i < ps->n && ps->p[ps->i] == '$') {
        ps->i++;

        if (ps->i >= ps->n || !fixed_size((char)ps->p[ps->i])) {
            ps->err = "optimized type must be a fixed-size marker";
            return NULL;
        }

        type = (char)ps->p[ps->i++];

        if (ps->i >= ps->n || ps->p[ps->i] != '#') {
            ps->err = "an optimized type needs a count";
            return NULL;
        }
    }

    if (ps->i < ps->n && ps->p[ps->i] == '#') {
        ps->i++;
        counted = 1;

        if (!is_obj && ps->i < ps->n && ps->p[ps->i] == '[') {     /* N-D dimension vector */
            pj_node* dv;
            const pj_node* d;

            ps->i++;

            if (ps->i < ps->n && ps->p[ps->i] == '[') {     /* one level deeper: column-major */
                colmajor = 1;
                ps->i++;
            }

            ps->depth++;
            dv = bj_container(ps, 0);
            ps->depth--;

            if (!dv) {
                return NULL;
            }

            if (colmajor) {
                if (ps->i >= ps->n || ps->p[ps->i] != ']') {
                    ps->err = "column-major wrapper not closed";
                    return NULL;
                }

                ps->i++;
            }

            count = 1;

            for (d = dv->child; d; d = d->next) {
                if (d->type != PJ_INT || d->i < 0 || nd >= 32) {
                    ps->err = "bad dimension vector";
                    return NULL;
                }

                dims[nd++] = (uint64_t)d->i;

                if (d->i && count > ((uint64_t)ps->n) / (uint64_t)d->i) {
                    ps->err = "dimensions exceed the payload";
                    return NULL;
                }

                count *= (uint64_t)d->i;
            }
        } else if (bj_size(ps, &count)) {
            return NULL;
        }

        /* every element takes at least one byte (keys too): a count larger
           than what is left cannot be honest */
        if (count > ps->n - ps->i) {
            ps->err = "count exceeds the payload";
            return NULL;
        }
    }

    if (type && !is_obj) {
        size_t base = ps->i, bytes = (size_t)fixed_size(type);

        if (count * bytes > ps->n - ps->i) {
            ps->err = "typed array exceeds the payload";
            return NULL;
        }

        if (nd >= 2) {
            uint64_t rstride[32], cstride[32];
            int a;
            pj_node* nested;

            rstride[nd - 1] = 1;

            for (a = nd - 2; a >= 0; a--) {
                rstride[a] = rstride[a + 1] * dims[a + 1];
            }

            cstride[0] = 1;

            for (a = 1; a < nd; a++) {
                cstride[a] = cstride[a - 1] * dims[a - 1];
            }

            nested = bj_nd(ps, type, dims, nd, colmajor, base, 0, 0, rstride, cstride);
            ps->i = base + (size_t)(count * bytes);
            return nested;
        }

        for (k = 0; k < count; k++) {
            pj_node* v = node(ps, PJ_NULL);

            if (!v || scalar(ps, type, v)) {
                return NULL;
            }

            append(x, &tail, v);
        }

        return x;
    }

    for (k = 0; !counted || k < count; k++) {
        pj_node* v;

        while (ps->i < ps->n && ps->p[ps->i] == 'N') {     /* no-op */
            ps->i++;
        }

        if (!counted && ps->i < ps->n && ps->p[ps->i] == (is_obj ? '}' : ']')) {
            ps->i++;
            return x;
        }

        if (is_obj) {
            uint64_t kl;
            const char* key;

            if (bj_size(ps, &kl)) {
                return NULL;
            }

            if (kl > ps->n - ps->i) {
                ps->err = "key runs past the end";
                return NULL;
            }

            key = copy_bytes(ps, ps->p + ps->i, (size_t)kl);
            ps->i += (size_t)kl;

            if (!key) {
                return NULL;
            }

            if (type) {
                v = node(ps, PJ_NULL);

                if (!v || scalar(ps, type, v)) {
                    return NULL;
                }
            } else {
                v = bj_value(ps);
            }

            if (!v) {
                return NULL;
            }

            v->key = key;
            v->keylen = (size_t)kl;
        } else {
            v = bj_value(ps);

            if (!v) {
                return NULL;
            }
        }

        append(x, &tail, v);
    }

    return x;
}

static pj_node* bj_value(parser* ps) {
    pj_node* x;
    char m;

    while (ps->i < ps->n && ps->p[ps->i] == 'N') {
        ps->i++;
    }

    if (ps->i >= ps->n) {
        ps->err = "a value was expected";
        return NULL;
    }

    if (++ps->depth > PJ_MAX_DEPTH) {
        ps->err = "nesting too deep";
        return NULL;
    }

    m = (char)ps->p[ps->i++];

    switch (m) {
        case 'Z':
            x = node(ps, PJ_NULL);
            break;

        case 'T':
        case 'F':
            x = node(ps, PJ_BOOL);

            if (x) {
                x->i = m == 'T';
            }

            break;

        case 'S':
        case 'H': {
            uint64_t n;

            if (bj_size(ps, &n)) {
                return NULL;
            }

            if (n > ps->n - ps->i) {
                ps->err = "string runs past the end";
                return NULL;
            }

            x = node(ps, m == 'S' ? PJ_STR : PJ_BYTES);

            if (x) {
                x->s = copy_bytes(ps, ps->p + ps->i, (size_t)n);
                x->len = (size_t)n;
                ps->i += (size_t)n;

                if (!x->s) {
                    return NULL;
                }
            }

            break;
        }

        case '[':
            x = bj_container(ps, 0);
            break;

        case '{':
            x = bj_container(ps, 1);
            break;

        default:
            x = node(ps, PJ_NULL);

            if (x && scalar(ps, m, x)) {
                if (!ps->err || !fixed_size(m)) {
                    ps->err = "unknown marker";
                }

                return NULL;
            }
    }

    ps->depth--;
    return x;
}

pj_doc* pj_parse(const void* data, size_t len, int binary, const char** error) {
    parser ps;
    pj_doc* d = (pj_doc*)calloc(1, sizeof(pj_doc));

    if (error) {
        *error = NULL;
    }

    if (!d) {
        return NULL;
    }

    memset(&ps, 0, sizeof(ps));
    ps.d = d;
    ps.p = (const unsigned char*)data;
    ps.n = data ? len : 0;

    if (!binary) {
        if (ps.n >= 3 && ps.p[0] == 0xEF && ps.p[1] == 0xBB && ps.p[2] == 0xBF) {
            ps.i = 3;   /* UTF-8 byte order mark */
        }

        d->root = json_value(&ps);

        if (d->root) {
            ws(&ps);

            if (ps.i != ps.n) {
                ps.err = "trailing data";
                d->root = NULL;
            }
        }
    } else {
        d->root = bj_value(&ps);
    }

    if (!d->root) {
        if (error) {
            *error = ps.err ? ps.err : "malformed input";
        }

        pj_free(d);
        return NULL;
    }

    return d;
}

/* ------------------------------------------------------------------ */
/* lookups                                                            */
/* ------------------------------------------------------------------ */

const pj_node* pj_get(const pj_node* obj, const char* key) {
    const pj_node* c;
    size_t n = strlen(key);

    if (!obj || obj->type != PJ_OBJ) {
        return NULL;
    }

    for (c = obj->child; c; c = c->next) {
        if (c->keylen == n && memcmp(c->key, key, n) == 0) {
            return c;
        }
    }

    return NULL;
}

const pj_node* pj_at(const pj_node* arr, int32_t index) {
    const pj_node* c;

    if (!arr || (arr->type != PJ_ARR && arr->type != PJ_OBJ) || index < 0) {
        return NULL;
    }

    for (c = arr->child; c && index > 0; c = c->next) {
        index--;
    }

    return c;
}

int pj_is_int(const pj_node* n) {
    return n && n->type == PJ_INT;
}

int64_t pj_int_or(const pj_node* n, int64_t def) {
    return pj_is_int(n) ? n->i : (n && n->type == PJ_BOOL ? n->i : def);
}

int pj_stream_bytes(const pj_node* n, unsigned char** out, size_t* len) {
    *out = NULL;
    *len = 0;

    if (!n) {
        return -1;
    }

    if (n->type == PJ_OBJ) {    /* {"_DataInfo_": {...}, "Data": ...} */
        return pj_stream_bytes(pj_get(n, "Data"), out, len);
    }

    if (n->type == PJ_BYTES) {
        *out = (unsigned char*)malloc(n->len ? n->len : 1);

        if (!*out) {
            return -1;
        }

        memcpy(*out, n->s, n->len);
        *len = n->len;
        return 0;
    }

    if (n->type == PJ_STR) {
        return pj_base64_decode(n->s, n->len, out, len);
    }

    return -1;
}
