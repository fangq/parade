/*
 * Parade PDF writer
 *
 * Writes the pages of a layout as PDF 1.7, from the same display list the
 * screen uses, so the PDF shows exactly the computed layout:
 *  - TrueType fonts are embedded as subsets (FontFile2, CIDFontType2,
 *    Identity-H): glyphs not used are emptied but keep their numbers, so
 *    the glyph id is the character id and the map stays the identity;
 *  - CFF fonts become Type 3 fonts drawn from Parade's own outlines, so a
 *    16 MB CJK font costs only the glyphs a document uses;
 *  - ToUnicode maps come from the document text at each glyph's cluster,
 *    so text copied from the PDF is the document's text;
 *  - JPEG passes through; PNG (gray/RGB/palette/alpha, interlaced or not) and
 *    a GIF's first frame are decoded and re-encoded with a soft mask where
 *    needed;
 *  - headings become the outline (bookmarks);
 *  - output is deterministic: integer coordinates printed exactly, no
 *    timestamps, the file id derived from the content.
 */

#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_doc_internal.h"
#include "parade_layout.h"
#include "pd_paint.h"

/* ------------------------------------------------------------------ */
/* buffered output with offsets                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_writer fn;
    void* user;
    unsigned char buf[16384];
    size_t nbuf;
    uint64_t pos;
    int err;
    uint64_t* offs;             /* object offsets, by object number */
    int32_t nobj, capobj;
    uint64_t hash;              /* for the file id */
} pdfw;

static void w_flush(pdfw* w) {
    if (w->nbuf && !w->err && w->fn(w->user, w->buf, w->nbuf)) {
        w->err = 1;
    }

    w->nbuf = 0;
}

static void w_raw(pdfw* w, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;

    for (i = 0; i < n; i++) {
        w->hash = (w->hash ^ p[i]) * 1099511628211ULL;
    }

    w->pos += n;

    if (n > sizeof(w->buf)) {
        w_flush(w);

        if (!w->err && w->fn(w->user, p, n)) {
            w->err = 1;
        }

        return;
    }

    if (w->nbuf + n > sizeof(w->buf)) {
        w_flush(w);
    }

    memcpy(w->buf + w->nbuf, p, n);
    w->nbuf += n;
}

static void w_str(pdfw* w, const char* s) {
    w_raw(w, s, strlen(s));
}

static void w_fmt(pdfw* w, const char* fmt, ...) {
    char tmp[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) {
        w_raw(w, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
    }
}

static int32_t new_obj(pdfw* w) {
    if (pd_grow((void**)&w->offs, &w->capobj, (int64_t)w->nobj + 2, sizeof(uint64_t))) {
        w->err = 1;
        return 0;
    }

    w->nobj++;
    w->offs[w->nobj] = 0;
    return w->nobj;
}

static void begin_obj(pdfw* w, int32_t id) {
    w->offs[id] = w->pos;
    w_fmt(w, "%d 0 obj\n", (int)id);
}

static void end_obj(pdfw* w) {
    w_str(w, "\nendobj\n");
}

/* a stream object, deflated when it helps */
static void stream_obj(pdfw* w, int32_t id, const char* dict, const void* data, size_t n, int compress) {
    unsigned char* z = NULL;
    size_t zn = 0;

    begin_obj(w, id);

    if (compress && n > 64 && pd_deflate(data, n, 1, &z, &zn) == 0 && zn < n) {
        w_fmt(w, "<< %s /Length %lu /Filter /FlateDecode >>\nstream\n", dict, (unsigned long)zn);
        w_raw(w, z, zn);
    } else {
        w_fmt(w, "<< %s /Length %lu >>\nstream\n", dict, (unsigned long)n);
        w_raw(w, data, n);
    }

    free(z);
    w_str(w, "\nendstream");
    end_obj(w);
}

/* a growable byte buffer */
typedef struct {
    char* p;
    size_t n, cap;
    int err;
} sbuf;

static void sb_raw(sbuf* b, const void* d, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        char* q;

        while (nc < b->n + n + 1) {
            nc *= 2;
        }

        q = (char*)realloc(b->p, nc);

        if (!q) {
            b->err = 1;
            return;
        }

        b->p = q;
        b->cap = nc;
    }

    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void sb_fmt(sbuf* b, const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) {
        sb_raw(b, tmp, (size_t)(n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1));
    }
}

/* sp as points with up to 4 decimals, exactly, independent of locale */
static void num(char* out, size_t cap, int64_t sp) {
    int64_t v = sp * 10000, q;
    int neg = v < 0;
    char frac[8];
    int k;

    v = neg ? -v : v;
    q = (v + 32768) / 65536;    /* 1/10000 pt, rounded */
    snprintf(frac, sizeof(frac), "%04d", (int)(q % 10000));

    for (k = 3; k >= 0 && frac[k] == '0'; k--) {
        frac[k] = '\0';
    }

    snprintf(out, cap, "%s%lld%s%s", neg && q ? "-" : "", (long long)(q / 10000), frac[0] ? "." : "", frac);
}

static void sb_num(sbuf* b, int64_t sp) {
    char t[32];

    num(t, sizeof(t), sp);
    sb_raw(b, t, strlen(t));
    sb_raw(b, " ", 1);
}

/* a colour 0xAARRGGBB that is see-through: its graphics state /GA0../GA16 (sixteenths), -1 when opaque (alpha 255,
   or 0: a colour given without one) */
static int see_through(uint32_t c) {
    uint32_t a = c >> 24;

    return a == 0 || a == 255 ? -1 : (int)((a * 16 + 127) / 255);
}

/* color 0xAARRGGBB as three components with 3 decimals */
static void sb_rgb(sbuf* b, uint32_t c, const char* op) {
    int k;

    for (k = 2; k >= 0; k--) {
        int v = (int)((c >> (8 * k)) & 0xFF) * 1000 / 255;
        sb_fmt(b, "%d.%03d ", v / 1000, v % 1000);
    }

    sb_fmt(b, "%s\n", op);
}

/* ------------------------------------------------------------------ */
/* fonts                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const pd_font* font;
    int truetype;               /* 1: FontFile2 subset, 0: Type 3 from outlines */
    uint8_t* used;              /* by glyph id */
    uint32_t* uni;              /* glyph id -> code point (first seen) */
    int32_t nglyphs;
    /* Type 3: glyphs are assigned single-byte codes in chunks of 256 */
    uint32_t* t3code;           /* glyph id -> (chunk << 8 | code) + 1, 0 = unassigned */
    int32_t t3count;
    int32_t obj;                /* Type0 (TrueType) or first Type 3 chunk object */
    int32_t* t3obj;
    int32_t nchunks;
    char name[64];
    int32_t index;
} pfont;

static uint32_t RU16(const pd_font* f, uint64_t o) {
    return o + 2 <= f->len ? ((uint32_t)f->data[o] << 8) | f->data[o + 1] : 0;
}

static uint32_t RU32(const pd_font* f, uint64_t o) {
    return o + 4 <= f->len ? ((uint32_t)f->data[o] << 24) | ((uint32_t)f->data[o + 1] << 16) |
           ((uint32_t)f->data[o + 2] << 8) | f->data[o + 3] : 0;
}

static uint32_t find_tab(const pd_font* f, uint32_t tag, uint32_t* len) {
    uint32_t n = RU16(f, f->base + 4), i;

    for (i = 0; i < n; i++) {
        uint64_t r = (uint64_t)f->base + 12 + 16 * i;

        if (RU32(f, r) == tag) {
            uint32_t off = RU32(f, r + 8), l = RU32(f, r + 12);

            if ((uint64_t)off + l > f->len) {
                return 0;
            }

            if (len) {
                *len = l;
            }

            return off;
        }
    }

    return 0;
}

#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* PostScript name (name id 6), sanitized; empty if missing */
static void ps_name(const pd_font* f, char* out, size_t cap) {
    uint32_t nm = find_tab(f, TAG('n', 'a', 'm', 'e'), NULL), n, i, so;
    size_t o = 0;

    out[0] = '\0';

    if (!nm) {
        return;
    }

    n = RU16(f, nm + 2);
    so = nm + RU16(f, nm + 4);

    for (i = 0; i < n && !o; i++) {
        uint64_t r = (uint64_t)nm + 6 + 12 * i;
        uint32_t plat = RU16(f, r), id = RU16(f, r + 6), len = RU16(f, r + 8), off = RU16(f, r + 10), k;

        if (id != 6) {
            continue;
        }

        for (k = 0; k < len && o + 1 < cap; k += (plat == 3 || plat == 0) ? 2 : 1) {
            uint32_t c = (plat == 3 || plat == 0) ? RU16(f, (uint64_t)so + off + k) :
                         (k < f->len ? f->data[so + off + k] : 0);

            if (c > 32 && c < 127 && !strchr("[](){}<>/%#", (int)c)) {
                out[o++] = (char)c;
            }
        }
    }

    out[o] = '\0';
}

static int glyf_range(const pd_font* f, uint32_t g, uint32_t* off, uint32_t* len) {
    uint32_t a, b;

    if ((int32_t)g >= f->m.num_glyphs) {
        return -1;
    }

    a = f->loca_long ? RU32(f, (uint64_t)f->loca + 4 * g) : 2 * RU16(f, (uint64_t)f->loca + 2 * g);
    b = f->loca_long ? RU32(f, (uint64_t)f->loca + 4 * g + 4) : 2 * RU16(f, (uint64_t)f->loca + 2 * g + 2);

    if (b < a || b > f->glyf_len) {
        return -1;
    }

    *off = f->glyf + a;
    *len = b - a;
    return 0;
}

/* mark the components of composite glyphs as used too */
static void close_composites(pfont* pf) {
    const pd_font* f = pf->font;
    int changed = 1, guard = 0;

    while (changed && guard++ < 8) {
        int32_t g;

        changed = 0;

        for (g = 0; g < pf->nglyphs; g++) {
            uint32_t off, len;
            uint64_t q;
            uint32_t flags;

            if (!pf->used[g] || glyf_range(f, (uint32_t)g, &off, &len) || len < 10 || (int16_t)RU16(f, off) >= 0) {
                continue;
            }

            q = (uint64_t)off + 10;

            do {
                uint32_t comp;

                flags = RU16(f, q);
                comp = RU16(f, q + 2);
                q += 4 + ((flags & 1) ? 4 : 2) + ((flags & 8) ? 2 : (flags & 0x40) ? 4 : (flags & 0x80) ? 8 : 0);

                if ((int32_t)comp < pf->nglyphs && !pf->used[comp]) {
                    pf->used[comp] = 1;
                    changed = 1;
                }
            } while ((flags & 0x20) && q < (uint64_t)off + len);
        }
    }
}

static void put32(sbuf* b, uint32_t v) {
    unsigned char t[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8),
                           (unsigned char)v
                         };
    sb_raw(b, t, 4);
}

static void put16(sbuf* b, uint32_t v) {
    unsigned char t[2] = { (unsigned char)(v >> 8), (unsigned char)v };
    sb_raw(b, t, 2);
}

static uint32_t checksum(const unsigned char* p, size_t n) {
    uint32_t s = 0;
    size_t i;

    for (i = 0; i < n; i += 4) {
        uint32_t v = 0;
        size_t k;

        for (k = 0; k < 4; k++) {
            v = (v << 8) | (i + k < n ? p[i + k] : 0);
        }

        s += v;
    }

    return s;
}

/* a TrueType font file holding only the used glyphs (others empty, numbering kept) */
static int subset_truetype(const pfont* pf, sbuf* out) {
    const pd_font* f = pf->font;
    static const uint32_t keep[] = { TAG('c', 'v', 't', ' '), TAG('f', 'p', 'g', 'm'), TAG('g', 'l', 'y', 'f'),
                                     TAG('h', 'e', 'a', 'd'), TAG('h', 'h', 'e', 'a'), TAG('h', 'm', 't', 'x'),
                                     TAG('l', 'o', 'c', 'a'), TAG('m', 'a', 'x', 'p'), TAG('p', 'r', 'e', 'p')
                                   };
    sbuf tab[9];
    uint32_t present[9];
    int nt = 0, i, k, headi = -1;
    uint32_t dir_len, offset, total;
    int32_t g;

    memset(tab, 0, sizeof(tab));

    for (i = 0; i < 9; i++) {
        uint32_t len = 0, off;

        if (keep[i] == TAG('g', 'l', 'y', 'f') || keep[i] == TAG('l', 'o', 'c', 'a')) {
            continue;
        }

        off = find_tab(f, keep[i], &len);

        if (off) {
            sb_raw(&tab[i], f->data + off, len);
        }
    }

    /* glyf and a long loca */
    for (g = 0; g < pf->nglyphs; g++) {
        uint32_t off, len;

        put32(&tab[6], (uint32_t)tab[2].n);

        if (pf->used[g] && glyf_range(f, (uint32_t)g, &off, &len) == 0 && len) {
            sb_raw(&tab[2], f->data + off, len);

            while (tab[2].n % 4) {
                sb_raw(&tab[2], "", 1);
            }
        }
    }

    put32(&tab[6], (uint32_t)tab[2].n);

    if (tab[3].n >= 54) {   /* head: long loca, checksum adjustment cleared */
        tab[3].p[50] = 0;
        tab[3].p[51] = 1;
        memset(tab[3].p + 8, 0, 4);
    }

    for (i = 0; i < 9; i++) {
        if (tab[i].n || keep[i] == TAG('g', 'l', 'y', 'f')) {
            present[nt++] = (uint32_t)i;
        }

        if (keep[i] == TAG('h', 'e', 'a', 'd')) {
            headi = i;
        }
    }

    /* offset table */
    {
        int es = 0, sr = 1;

        while (sr * 2 <= nt) {
            sr *= 2;
            es++;
        }

        put32(out, 0x00010000);
        put16(out, (uint32_t)nt);
        put16(out, (uint32_t)sr * 16);
        put16(out, (uint32_t)es);
        put16(out, (uint32_t)(nt * 16 - sr * 16));
    }

    dir_len = 12 + 16 * (uint32_t)nt;
    offset = dir_len;

    for (k = 0; k < nt; k++) {
        sbuf* t = &tab[present[k]];

        put32(out, keep[present[k]]);
        put32(out, checksum((const unsigned char*)t->p, t->n));
        put32(out, offset);
        put32(out, (uint32_t)t->n);
        offset += (uint32_t)((t->n + 3) & ~(size_t)3);
    }

    for (k = 0; k < nt; k++) {
        sbuf* t = &tab[present[k]];

        if (t->n) {
            sb_raw(out, t->p, t->n);
        }

        while (out->n % 4) {
            sb_raw(out, "", 1);
        }
    }

    /* checkSumAdjustment = 0xB1B0AFBA - checksum of the whole font */
    total = checksum((const unsigned char*)out->p, out->n);

    for (k = 0; k < nt; k++) {
        if ((int)present[k] == headi) {
            uint32_t at = 0, adj = 0xB1B0AFBAu - total;
            int j;

            for (j = 0; j < k; j++) {
                at += (uint32_t)((tab[present[j]].n + 3) & ~(size_t)3);
            }

            at += dir_len + 8;

            if (at + 4 <= out->n) {
                out->p[at] = (char)(adj >> 24);
                out->p[at + 1] = (char)(adj >> 16);
                out->p[at + 2] = (char)(adj >> 8);
                out->p[at + 3] = (char)adj;
            }
        }
    }

    for (i = 0; i < 9; i++) {
        free(tab[i].p);
    }

    return out->err ? -1 : 0;
}

/* ToUnicode CMap: codes of nbytes width mapped to code points */
static void to_unicode(sbuf* b, const uint32_t* code, const uint32_t* uni, int32_t n, int nbytes) {
    int32_t i, cnt = 0;

    sb_fmt(b, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
           "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
           "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n1 begincodespacerange\n%s\nendcodespacerange\n",
           nbytes == 2 ? "<0000> <FFFF>" : "<00> <FF>");

    for (i = 0; i < n; i++) {
        cnt += uni[i] != 0;
    }

    while (cnt > 0) {   /* at most 100 entries per block */
        int32_t nb = cnt > 100 ? 100 : cnt, done = 0;

        sb_fmt(b, "%d beginbfchar\n", (int)nb);

        for (i = 0; i < n && done < nb; i++) {
            uint32_t u = uni[i];

            if (!u) {
                continue;
            }

            sb_fmt(b, nbytes == 2 ? "<%04X> <" : "<%02X> <", code[i]);

            if (u >= 0x10000) {     /* UTF-16 surrogate pair */
                u -= 0x10000;
                sb_fmt(b, "%04X%04X", 0xD800 + (u >> 10), 0xDC00 + (u & 0x3FF));
            } else {
                sb_fmt(b, "%04X", u);
            }

            sb_fmt(b, ">\n");
            done++;
        }

        sb_fmt(b, "endbfchar\n");
        cnt -= nb;

        /* entries written are skipped next round by clearing them */
        for (i = 0; i < n && nb > 0; i++) {
            if (uni[i]) {
                ((uint32_t*)uni)[i] = 0;
                nb--;
            }
        }
    }

    sb_fmt(b, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
}

/* Type 3 glyph procedure from the outline, in 1000-unit glyph space */
typedef struct {
    sbuf* b;
    int32_t upem;
} t3ctx;

static int64_t t3v(const t3ctx* c, int32_t v) {
    return (int64_t)v * 1000 / c->upem / 64;
}

static void t3_move(void* u, int32_t x, int32_t y) {
    t3ctx* c = (t3ctx*)u;
    sb_fmt(c->b, "%lld %lld m\n", (long long)t3v(c, x), (long long)t3v(c, y));
}

static void t3_line(void* u, int32_t x, int32_t y) {
    t3ctx* c = (t3ctx*)u;
    sb_fmt(c->b, "%lld %lld l\n", (long long)t3v(c, x), (long long)t3v(c, y));
}

static void t3_cubic(void* u, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t x, int32_t y) {
    t3ctx* c = (t3ctx*)u;
    sb_fmt(c->b, "%lld %lld %lld %lld %lld %lld c\n", (long long)t3v(c, ax), (long long)t3v(c, ay),
           (long long)t3v(c, bx), (long long)t3v(c, by), (long long)t3v(c, x), (long long)t3v(c, y));
}

static void t3_close(void* u) {
    sb_fmt(((t3ctx*)u)->b, "h\n");
}

/* ------------------------------------------------------------------ */
/* images                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_res_id res;
    int32_t obj, smask;
    int ok;
} pimage;

/* a path's rings (m, l, h) in PDF's coordinates, the page H high */
static void sb_rings(sbuf* c, const pd_sp* pts, int32_t n, int closed, int64_t H) {
    int32_t q, first;

    for (q = 0, first = 1; q < n; q++) {    /* first: the next point starts a ring */
        if (pts[2 * q] == PD_PATH_BREAK) {
            if (closed && !first) {
                sb_fmt(c, "h ");
            }

            first = 1;
            continue;
        }

        sb_num(c, pts[2 * q]);
        sb_num(c, H - pts[2 * q + 1]);
        sb_fmt(c, first ? "m " : "l ");
        first = 0;
    }

    if (closed && !first) {
        sb_fmt(c, "h ");
    }
}

/* a path item's rings: from its curve where it has one (m, l, c), else its points */
static void sb_path(sbuf* c, const pd_draw* a, int closed, int64_t H) {
    int32_t q, first;

    if (!a->curve || a->ncurve < 2) {
        sb_rings(c, a->points, a->npoints, closed, H);
        return;
    }

    for (q = 0, first = 1; q < a->ncurve; q++) {
        const pd_sp* p = &a->curve[2 * q];

        if (p[0] == PD_PATH_BREAK) {
            if (closed && !first) {
                sb_fmt(c, "h ");
            }

            first = 1;
        } else if (p[0] == PD_PATH_CUBIC && !first && q + 3 < a->ncurve) {
            int k;

            for (k = 1; k <= 3; k++) {
                sb_num(c, p[2 * k]);
                sb_num(c, H - p[2 * k + 1]);
            }

            sb_fmt(c, "c ");
            q += 3;
        } else if (p[0] != PD_PATH_CUBIC) {
            sb_num(c, p[0]);
            sb_num(c, H - p[1]);
            sb_fmt(c, first ? "m " : "l ");
            first = 0;
        }
    }

    if (closed && !first) {
        sb_fmt(c, "h ");
    }
}

/* resources made while the pages are written (shadings, soft masks, patterns, pictures), named by object number:
   /Sh12, /SG13, /Pt14, /Xo15 */
typedef struct {
    char kind;                  /* S shading, G graphics state, P pattern, X XObject */
    int32_t obj;
} pres;

typedef struct {
    pres* r;
    int32_t n, cap;
} preslist;

static void res_add(preslist* L, char kind, int32_t obj) {
    if (obj && !pd_grow((void**)&L->r, &L->cap, (int64_t)L->n + 1, sizeof(pres))) {
        L->r[L->n].kind = kind;
        L->r[L->n++].obj = obj;
    }
}

/* the resources of a kind as dictionary entries: " /Sh12 12 0 R ..." */
static void w_res(pdfw* w, const preslist* L, char kind, const char* prefix) {
    int32_t i;

    for (i = 0; i < L->n; i++) {
        if (L->r[i].kind == kind) {
            w_fmt(w, " /%s%d %d 0 R", prefix, (int)L->r[i].obj, (int)L->r[i].obj);
        }
    }
}

/* a colour of a gradient's stop as a function's output: r g b, or its alpha as a gray */
static void fn_color(char* out, size_t cap, uint32_t c, int alpha) {
    if (alpha) {
        snprintf(out, cap, "%.4f", ((c >> 24) & 255) / 255.0);
    } else {
        snprintf(out, cap, "%.4f %.4f %.4f", ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0);
    }
}

/* a gradient's colours (or alphas) from 0 to 1: a Type 2 function between two stops, a Type 3 stitching of them for
   more; its object */
static int32_t pdf_grad_fn(pdfw* w, const pd_gradient* g, int alpha) {
    char c0[64], c1[64];
    int32_t parts[PD_MAX_STOPS + 2], obj;
    int i;

    if (g->n == 2) {
        obj = new_obj(w);
        fn_color(c0, sizeof(c0), g->color[0], alpha);
        fn_color(c1, sizeof(c1), g->color[1], alpha);
        begin_obj(w, obj);
        w_fmt(w, "<< /FunctionType 2 /Domain [0 1] /C0 [%s] /C1 [%s] /N 1 >>", c0, c1);
        end_obj(w);
        return obj;
    }

    for (i = 0; i + 1 < g->n; i++) {
        parts[i] = new_obj(w);
        fn_color(c0, sizeof(c0), g->color[i], alpha);
        fn_color(c1, sizeof(c1), g->color[i + 1], alpha);
        begin_obj(w, parts[i]);
        w_fmt(w, "<< /FunctionType 2 /Domain [0 1] /C0 [%s] /C1 [%s] /N 1 >>", c0, c1);
        end_obj(w);
    }

    obj = new_obj(w);
    begin_obj(w, obj);
    w_str(w, "<< /FunctionType 3 /Domain [0 1] /Functions [");

    for (i = 0; i + 1 < g->n; i++) {
        w_fmt(w, "%d 0 R ", (int)parts[i]);
    }

    w_str(w, "] /Bounds [");

    for (i = 1; i + 1 < g->n; i++) {
        w_fmt(w, "%.5f ", g->pos[i] / 65536.0);
    }

    w_str(w, "] /Encode [");

    for (i = 0; i + 1 < g->n; i++) {
        w_str(w, "0 1 ");
    }

    w_str(w, "] >>");
    end_obj(w);
    return obj;
}

/* a shading object: axial along (x0, y0)-(x1, y1), or radial about (x0, y0) out to r; page sp, PDF's y up */
static int32_t pdf_shading(pdfw* w, int32_t fn, int alpha, int radial, double x0, double y0, double x1, double y1,
                           double r, int64_t H) {
    int32_t obj = new_obj(w);
    char a[32], b[32], c[32], d[32], e[32];

    num(a, sizeof(a), (int64_t)x0);
    num(b, sizeof(b), H - (int64_t)y0);
    num(c, sizeof(c), (int64_t)x1);
    num(d, sizeof(d), H - (int64_t)y1);
    num(e, sizeof(e), (int64_t)r);
    begin_obj(w, obj);

    if (radial) {
        w_fmt(w, "<< /ShadingType 3 /ColorSpace /Device%s /Coords [%s %s 0 %s %s %s] /Function %d 0 R /Extend [true true] >>",
              alpha ? "Gray" : "RGB", a, b, a, b, e, (int)fn);
    } else {
        w_fmt(w, "<< /ShadingType 2 /ColorSpace /Device%s /Coords [%s %s %s %s] /Function %d 0 R /Extend [true true] >>",
              alpha ? "Gray" : "RGB", a, b, c, d, (int)fn);
    }

    end_obj(w);
    return obj;
}

static void sb_poly4(sbuf* c, const double* q, int64_t H) {
    int k;

    for (k = 0; k < 4; k++) {
        sb_num(c, (int64_t)q[2 * k]);
        sb_num(c, H - (int64_t)q[2 * k + 1]);
        sb_fmt(c, k ? "l " : "m ");
    }

    sb_fmt(c, "h W n ");
}

/* A path filled with its gradient (or the gradient's alphas, as grays, for its soft mask): the path the clip, then
   the shading; a rectangular one as four axial shadings, one between each side of its inner rectangle and the
   outer, and the inner one's colour. Shadings made are added to R. */
static void pdf_grad_content(pdfw* w, sbuf* c, preslist* R, const pd_draw* a, const pd_gradient* g, int alpha,
                             int64_t H) {
    int32_t fn = pdf_grad_fn(w, g, alpha), sh;

    sb_fmt(c, "q ");
    sb_path(c, a, 1, H);
    sb_fmt(c, "W n ");

    if (g->kind == PD_GRAD_RECT) {
        double side[4][8] = {   /* top, right, bottom, left: outer corners, then inner */
            { g->bx0, g->by0, g->bx1, g->by0, g->x1, g->y0, g->x0, g->y0 },
            { g->bx1, g->by0, g->bx1, g->by1, g->x1, g->y1, g->x1, g->y0 },
            { g->bx1, g->by1, g->bx0, g->by1, g->x0, g->y1, g->x1, g->y1 },
            { g->bx0, g->by1, g->bx0, g->by0, g->x0, g->y0, g->x0, g->y1 }
        };
        double from[4][4] = {   /* each side's axis: from the inner edge to the outer */
            { g->x0, g->y0, g->x0, g->by0 }, { g->x1, g->y0, g->bx1, g->y0 }, { g->x0, g->y1, g->x0, g->by1 },
            { g->x0, g->y0, g->bx0, g->y0 }
        };
        char col[64];
        int k;

        fn_color(col, sizeof(col), g->color[0], alpha);
        sb_fmt(c, "q %s %s ", col, alpha ? "g" : "rg");
        sb_num(c, (int64_t)g->x0);
        sb_num(c, H - (int64_t)g->y1);
        sb_num(c, (int64_t)(g->x1 - g->x0));
        sb_num(c, (int64_t)(g->y1 - g->y0));
        sb_fmt(c, "re f Q\n");

        for (k = 0; k < 4; k++) {
            if (from[k][0] == from[k][2] && from[k][1] == from[k][3]) {
                continue;   /* the inner rectangle reaches this side */
            }

            sh = pdf_shading(w, fn, alpha, 0, from[k][0], from[k][1], from[k][2], from[k][3], 0, H);
            res_add(R, 'S', sh);
            sb_fmt(c, "q ");
            sb_poly4(c, side[k], H);
            sb_fmt(c, "/Sh%d sh Q\n", (int)sh);
        }
    } else {
        double r = sqrt((g->x1 - g->x0) * (g->x1 - g->x0) + (g->y1 - g->y0) * (g->y1 - g->y0));

        sh = pdf_shading(w, fn, alpha, g->kind == PD_GRAD_CIRCLE, g->x0, g->y0, g->x1, g->y1, r, H);
        res_add(R, 'S', sh);
        sb_fmt(c, "/Sh%d sh ", (int)sh);
    }

    sb_fmt(c, "Q\n");
}

/* A pattern fill as a tiling pattern: its 8 by 8 cells (3/4 pt each) in the foreground over the background, the
   tiles counted from the page's top left corner as the rasterizer counts them; its object */
static int32_t pdf_pattern(pdfw* w, const pd_path_style* st, int64_t H, int compress) {
    sbuf m;
    char dict[256], hs[32];
    int32_t obj = new_obj(w);
    int r, x;

    memset(&m, 0, sizeof(m));

    if (st->pattern_bg) {
        sb_rgb(&m, st->pattern_bg, "rg");
        sb_fmt(&m, "0 0 6 6 re f ");
    }

    sb_rgb(&m, st->pattern_fg, "rg");

    for (r = 0; r < 8; r++) {   /* each row's runs of set cells, row 0 at the top */
        for (x = 0; x < 8; x++) {
            int e = x;

            if (!((st->pattern_bits[r] >> (7 - x)) & 1)) {
                continue;
            }

            while (e + 1 < 8 && ((st->pattern_bits[r] >> (7 - e - 1)) & 1)) {
                e++;
            }

            sb_fmt(&m, "%.2f %.2f %.2f 0.75 re ", x * 0.75, 6 - (r + 1) * 0.75, (e - x + 1) * 0.75);
            x = e;
        }
    }

    sb_fmt(&m, "f");
    num(hs, sizeof(hs), H);
    snprintf(dict, sizeof(dict), "/Type /Pattern /PatternType 1 /PaintType 1 /TilingType 1 /BBox [0 0 6 6] /XStep 6 "
             "/YStep 6 /Matrix [1 0 0 1 0 %s] /Resources << >>", hs);
    stream_obj(w, obj, dict, m.p ? m.p : "", m.n, compress);
    free(m.p);
    return obj;
}

/* A path's gradient fill: as it is, or, with see-through stops, under a soft mask of their alphas (a form drawing
   the same gradient in grays, its luminosity the mask) */
static void pdf_gradient(pdfw* w, sbuf* c, preslist* R, const pd_draw* a, const pd_gradient* g, int64_t W, int64_t H,
                         int compress) {
    if (g->alpha) {
        sbuf m;
        preslist F;
        char dict[512], ws[32], hs[32];
        int32_t form = new_obj(w), gs, i;
        size_t dl;

        memset(&m, 0, sizeof(m));
        memset(&F, 0, sizeof(F));
        pdf_grad_content(w, &m, &F, a, g, 1, H);
        num(ws, sizeof(ws), W);
        num(hs, sizeof(hs), H);
        dl = (size_t)snprintf(dict, sizeof(dict), "/Type /XObject /Subtype /Form /BBox [0 0 %s %s] /Group << /S "
                              "/Transparency /CS /DeviceGray >> /Resources << /Shading <<", ws, hs);

        for (i = 0; i < F.n && dl + 32 < sizeof(dict); i++) {
            dl += (size_t)snprintf(dict + dl, sizeof(dict) - dl, " /Sh%d %d 0 R", (int)F.r[i].obj, (int)F.r[i].obj);
        }

        snprintf(dict + dl, sizeof(dict) - dl, " >> >>");
        stream_obj(w, form, dict, m.p ? m.p : "", m.n, compress);
        gs = new_obj(w);
        begin_obj(w, gs);
        w_fmt(w, "<< /SMask << /Type /Mask /S /Luminosity /G %d 0 R >> >>", (int)form);
        end_obj(w);
        res_add(R, 'G', gs);
        sb_fmt(c, "q /SG%d gs\n", (int)gs);
        pdf_grad_content(w, c, R, a, g, 0, H);
        sb_fmt(c, "Q\n");
        free(m.p);
        free(F.r);
    } else {
        pdf_grad_content(w, c, R, a, g, 0, H);
    }
}

/* rows of a PNG (each a filter byte, then stride bytes) unfiltered in place */
static void png_unfilter(unsigned char* raw, uint32_t rows, uint32_t stride, uint32_t bpp) {
    uint32_t y, x;

    for (y = 0; y < rows; y++) {
        unsigned char* row = raw + (size_t)y * (stride + 1) + 1, *up = y ? row - (stride + 1) : NULL;
        unsigned ft = row[-1];

        row[-1] = 0;

        for (x = 0; x < stride; x++) {
            int a = x >= bpp ? row[x - bpp] : 0, b = up ? up[x] : 0, c = (up && x >= bpp) ? up[x - bpp] : 0;
            int pr, pa, pb, pc;

            switch (ft) {
                case 1:
                    row[x] = (unsigned char)(row[x] + a);
                    break;

                case 2:
                    row[x] = (unsigned char)(row[x] + b);
                    break;

                case 3:
                    row[x] = (unsigned char)(row[x] + ((a + b) >> 1));
                    break;

                case 4:
                    pr = a + b - c;
                    pa = abs(pr - a);
                    pb = abs(pr - b);
                    pc = abs(pr - c);
                    row[x] = (unsigned char)(row[x] + ((pa <= pb && pa <= pc) ? a : pb <= pc ? b : c));
                    break;

                default:
                    break;
            }
        }
    }
}

/* a GIF's first frame on its logical screen, transparent where the frame leaves it or marks it so */
static int gif_decode(const unsigned char* p, size_t n, int32_t* w, int32_t* h, unsigned char** rgb,
                      unsigned char** alpha) {
    size_t i = 13, npix;
    const unsigned char* gct = NULL, *ct;
    int W, H, ngct = 0, trans = -1;

    if (n < 13 || (memcmp(p, "GIF87a", 6) && memcmp(p, "GIF89a", 6))) {
        return -1;
    }

    W = p[6] | (p[7] << 8);
    H = p[8] | (p[9] << 8);

    if (W <= 0 || H <= 0 || (int64_t)W * H > 64 * 1024 * 1024) {
        return -1;
    }

    if (p[10] & 0x80) {
        ngct = 2 << (p[10] & 7);
        gct = p + 13;
        i += (size_t)ngct * 3;
    }

    while (i < n) {
        if (p[i] == 0x21 && i + 1 < n) {    /* an extension: a graphic control one says which index is clear */
            size_t k = i + 2;

            if (p[i + 1] == 0xF9 && k + 4 < n && p[k] >= 4 && (p[k + 1] & 1)) {
                trans = p[k + 4];
            }

            while (k < n && p[k]) {
                k += (size_t)p[k] + 1;
            }

            i = k + 1;
        } else if (p[i] == 0x2C && i + 10 < n) {
            int fx = p[i + 1] | (p[i + 2] << 8), fy = p[i + 3] | (p[i + 4] << 8);
            int fw = p[i + 5] | (p[i + 6] << 8), fh = p[i + 7] | (p[i + 8] << 8), fl = p[i + 9];
            int nct = ngct, minc, clear, codesz, avail, old = -1, first = 0, pass = 0, row = 0, col = 0;
            static const int pstart[4] = { 0, 4, 2, 1 }, pstep[4] = { 8, 8, 4, 2 };
            uint16_t* prefix;
            unsigned char* suffix, *stack, *idx;
            uint32_t bits = 0;
            int nbits = 0, sub = 0;
            size_t k, done = 0, fpix = (size_t)fw * fh;

            i += 10;
            ct = gct;

            if (fl & 0x80) {
                nct = 2 << (fl & 7);
                ct = p + i;
                i += (size_t)nct * 3;
            }

            if (!ct || i >= n || (minc = p[i++]) < 2 || minc > 11) {
                return -1;
            }

            npix = (size_t)W * H;
            prefix = (uint16_t*)malloc(4096 * sizeof(uint16_t));
            suffix = (unsigned char*)malloc(4096);
            stack = (unsigned char*)malloc(4097);
            idx = (unsigned char*)malloc(fpix ? fpix : 1);
            *rgb = (unsigned char*)calloc(npix, 3);
            *alpha = (unsigned char*)calloc(npix, 1);

            if (!prefix || !suffix || !stack || !idx || !*rgb || !*alpha) {
                free(prefix);
                free(suffix);
                free(stack);
                free(idx);
                free(*rgb);
                free(*alpha);
                *rgb = *alpha = NULL;
                return -1;
            }

            clear = 1 << minc;
            codesz = minc + 1;
            avail = clear + 2;

            for (k = 0; k < (size_t)clear; k++) {
                prefix[k] = 0xFFFF;
                suffix[k] = (unsigned char)k;
            }

            /* LZW codes, least significant bit first, packed in sub-blocks */
            while (done < fpix) {
                int code, in, sp = 0;

                while (nbits < codesz) {
                    if (!sub) {
                        if (i >= n || !p[i]) {
                            goto out;
                        }

                        sub = p[i++];
                    }

                    if (i >= n) {
                        goto out;
                    }

                    bits |= (uint32_t)p[i++] << nbits;
                    nbits += 8;
                    sub--;
                }

                code = (int)(bits & ((1u << codesz) - 1));
                bits >>= codesz;
                nbits -= codesz;

                if (code == clear) {
                    codesz = minc + 1;
                    avail = clear + 2;
                    old = -1;
                    continue;
                }

                if (code == clear + 1) {
                    break;
                }

                if (old < 0) {
                    if (code >= clear) {
                        break;
                    }

                    idx[done++] = (unsigned char)code;
                    old = first = code;
                    continue;
                }

                in = code;

                if (code > avail || code >= 4096) {
                    break;
                }

                if (code == avail) {    /* the code being defined: the previous string and its first byte */
                    stack[sp++] = (unsigned char)first;
                    code = old;
                }

                while (code >= clear && sp < 4096) {
                    stack[sp++] = suffix[code];
                    code = prefix[code];
                }

                first = suffix[code];
                stack[sp++] = (unsigned char)first;

                while (sp > 0 && done < fpix) {
                    idx[done++] = stack[--sp];
                }

                if (avail < 4096) {
                    prefix[avail] = (uint16_t)old;
                    suffix[avail] = (unsigned char)first;
                    avail++;

                    if (avail == (1 << codesz) && codesz < 12) {
                        codesz++;
                    }
                }

                old = in;
            }

out:
            /* the indices onto the screen, row by row (interlaced: in its four passes) */
            for (k = 0; k < done; k++) {
                int x = fx + col, y = fy + row, c = idx[k];

                if (x < W && y < H && c != trans && c < nct) {
                    size_t o = (size_t)y * W + x;

                    memcpy(*rgb + o * 3, ct + c * 3, 3);
                    (*alpha)[o] = 255;
                }

                if (++col == fw) {
                    col = 0;

                    if (fl & 0x40) {
                        row += pstep[pass];

                        while (row >= fh && pass < 3) {
                            row = pstart[++pass];
                        }
                    } else {
                        row++;
                    }
                }
            }

            free(prefix);
            free(suffix);
            free(stack);
            free(idx);

            for (k = 0; k < npix && (*alpha)[k] == 255; k++) {
            }

            if (k == npix) {    /* every pixel opaque: no mask */
                free(*alpha);
                *alpha = NULL;
            }

            *w = W;
            *h = H;
            return 0;
        } else {
            break;
        }
    }

    return -1;
}

static int png_decode(const unsigned char* p, size_t n, int32_t* w, int32_t* h, unsigned char** rgb,
                      unsigned char** alpha) {
    size_t i = 8, idn = 0;
    unsigned char* idat = NULL, *raw = NULL;
    uint32_t W = 0, H = 0, depth = 0, ctype = 0, inter = 0, bpp, stride, y, x;
    unsigned char pal[768];
    uint32_t npal = 0;
    unsigned char trns[256];
    uint32_t ntrns = 0;
    size_t rawn;

    if (n < 8 || memcmp(p, "\x89PNG\r\n\x1a\n", 8)) {
        return -1;
    }

    memset(trns, 255, sizeof(trns));

    while (i + 8 <= n) {
        uint32_t len = ((uint32_t)p[i] << 24) | ((uint32_t)p[i + 1] << 16) | ((uint32_t)p[i + 2] << 8) | p[i + 3];
        const unsigned char* t = p + i + 4, *d = p + i + 8;

        if (i + 12 + (size_t)len > n) {
            break;
        }

        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            W = ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
            H = ((uint32_t)d[4] << 24) | ((uint32_t)d[5] << 16) | ((uint32_t)d[6] << 8) | d[7];
            depth = d[8];
            ctype = d[9];
            inter = d[12];
        } else if (!memcmp(t, "PLTE", 4) && len <= 768) {
            memcpy(pal, d, len);
            npal = len / 3;
        } else if (!memcmp(t, "tRNS", 4) && len <= 256) {
            memcpy(trns, d, len);
            ntrns = len;
        } else if (!memcmp(t, "IDAT", 4)) {
            unsigned char* q = (unsigned char*)realloc(idat, idn + len + 1);

            if (!q) {
                free(idat);
                return -1;
            }

            idat = q;
            memcpy(idat + idn, d, len);
            idn += len;
        }

        i += 12 + (size_t)len;
    }

    (void)ntrns;

    if (!W || !H || W > 20000 || H > 20000 || depth != 8 || inter > 1 || !idat ||
            !(ctype == 0 || ctype == 2 || ctype == 3 || ctype == 4 || ctype == 6) || (ctype == 3 && !npal)) {
        free(idat);
        return -1;
    }

    bpp = ctype == 2 ? 3 : ctype == 4 ? 2 : ctype == 6 ? 4 : 1;
    stride = W * bpp;

    if (inter) {    /* Adam7: seven passes, each a smaller image, put together into rows as if not interlaced */
        static const uint32_t x0[7] = { 0, 4, 0, 2, 0, 1, 0 }, y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
        static const uint32_t dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
        size_t need = 0, at = 0;
        unsigned char* pass;
        int k;

        for (k = 0; k < 7; k++) {
            uint32_t pw = W > x0[k] ? (W - x0[k] + dx[k] - 1) / dx[k] : 0, ph = H > y0[k] ? (H - y0[k] + dy[k] - 1) / dy[k] : 0;

            need += pw && ph ? (size_t)(pw * bpp + 1) * ph : 0;
        }

        if (pd_inflate(idat, idn, 1, need, &pass, &rawn) || rawn != need ||
                (raw = (unsigned char*)calloc((size_t)(stride + 1) * H, 1)) == NULL) {
            free(idat);
            free(pass);
            return -1;
        }

        for (k = 0; k < 7; k++) {
            uint32_t pw = W > x0[k] ? (W - x0[k] + dx[k] - 1) / dx[k] : 0, ph = H > y0[k] ? (H - y0[k] + dy[k] - 1) / dy[k] : 0;
            uint32_t px, py;

            if (!pw || !ph) {
                continue;
            }

            png_unfilter(pass + at, ph, pw * bpp, bpp);

            for (py = 0; py < ph; py++) {
                for (px = 0; px < pw; px++) {
                    memcpy(raw + (size_t)(y0[k] + py * dy[k]) * (stride + 1) + 1 + (size_t)(x0[k] + px * dx[k]) * bpp,
                           pass + at + (size_t)py * (pw * bpp + 1) + 1 + (size_t)px * bpp, bpp);
                }
            }

            at += (size_t)(pw * bpp + 1) * ph;
        }

        free(pass);
        free(idat);
    } else {
        if (pd_inflate(idat, idn, 1, (size_t)(stride + 1) * H, &raw, &rawn) || rawn != (size_t)(stride + 1) * H) {
            free(idat);
            free(raw);
            return -1;
        }

        free(idat);
        png_unfilter(raw, H, stride, bpp);
    }

    *rgb = (unsigned char*)malloc((size_t)W * H * 3);
    *alpha = (ctype == 4 || ctype == 6 || (ctype == 3 && ntrns)) ? (unsigned char*)malloc((size_t)W * H) : NULL;

    if (!*rgb) {
        free(raw);
        free(*alpha);
        return -1;
    }

    for (y = 0; y < H; y++) {
        const unsigned char* row = raw + (size_t)y * (stride + 1) + 1;

        for (x = 0; x < W; x++) {
            unsigned char* o = *rgb + ((size_t)y * W + x) * 3;
            const unsigned char* s = row + x * bpp;
            unsigned a = 255;

            if (ctype == 0 || ctype == 4) {
                o[0] = o[1] = o[2] = s[0];
                a = ctype == 4 ? s[1] : 255;
            } else if (ctype == 3) {
                unsigned k = s[0] < npal ? s[0] : 0;

                o[0] = pal[3 * k];
                o[1] = pal[3 * k + 1];
                o[2] = pal[3 * k + 2];
                a = trns[k];
            } else {
                o[0] = s[0];
                o[1] = s[1];
                o[2] = s[2];
                a = ctype == 6 ? s[3] : 255;
            }

            if (*alpha) {
                (*alpha)[(size_t)y * W + x] = (unsigned char)a;
            }
        }
    }

    free(raw);
    *w = (int32_t)W;
    *h = (int32_t)H;
    return 0;
}

static int jpeg_info(const unsigned char* p, size_t n, int32_t* w, int32_t* h, int* comps) {
    size_t i = 2;

    if (n < 4 || p[0] != 0xFF || p[1] != 0xD8) {
        return -1;
    }

    while (i + 4 <= n) {
        unsigned m;
        size_t len;

        if (p[i] != 0xFF) {
            return -1;
        }

        m = p[i + 1];
        len = ((size_t)p[i + 2] << 8) | p[i + 3];

        if ((m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB)) {
            if (i + 9 >= n) {
                return -1;
            }

            *h = (int32_t)(((unsigned)p[i + 5] << 8) | p[i + 6]);
            *w = (int32_t)(((unsigned)p[i + 7] << 8) | p[i + 8]);
            *comps = p[i + 9];
            return (*w > 0 && *h > 0 && (*comps == 1 || *comps == 3 || *comps == 4)) ? 0 : -1;
        }

        i += 2 + len;
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* the document                                                       */
/* ------------------------------------------------------------------ */

void pd_pdf_options_init(pd_pdf_options* o) {
    if (o) {
        memset(o, 0, sizeof(*o));
        o->compress = 1;
        o->outlines = 1;
    }
}

static pfont* font_of(pfont** fonts, int32_t* nfonts, const pd_font* f) {
    int32_t i;
    pfont* pf;

    for (i = 0; i < *nfonts; i++) {
        if ((*fonts)[i].font == f) {
            return &(*fonts)[i];
        }
    }

    pf = (pfont*)realloc(*fonts, ((size_t) * nfonts + 1) * sizeof(pfont));

    if (!pf) {
        return NULL;
    }

    *fonts = pf;
    pf = &(*fonts)[(*nfonts)++];
    memset(pf, 0, sizeof(*pf));
    pf->font = f;
    pf->truetype = f->glyf && f->loca;
    pf->nglyphs = f->m.num_glyphs > 0 ? f->m.num_glyphs : 1;
    pf->used = (uint8_t*)calloc((size_t)pf->nglyphs, 1);
    pf->uni = (uint32_t*)calloc((size_t)pf->nglyphs, sizeof(uint32_t));
    pf->t3code = (uint32_t*)calloc((size_t)pf->nglyphs, sizeof(uint32_t));
    pf->index = *nfonts;
    ps_name(f, pf->name, sizeof(pf->name));

    if (!pf->name[0]) {
        snprintf(pf->name, sizeof(pf->name), "ParadeFont%d", (int)pf->index);
    }

    return (pf->used && pf->uni && pf->t3code) ? pf : NULL;
}

typedef struct {
    pd_block_id block;
    int32_t level, page;
    pd_sp y;
    int32_t obj;
    char title[256];
} outline;

pd_status pd_layout_write_pdf(const pd_layout* L, const pd_pdf_options* opt, pd_writer fn, void* user) {
    pd_pdf_options defaults;
    const pd_doc* d;
    pdfw w;
    int32_t npages = pd_layout_page_count(L), pg, i, nfonts = 0, nimages = 0, nout = 0, k;
    pfont* fonts = NULL;
    pimage* images = NULL;
    outline* outs = NULL;
    preslist R;
    int32_t catalog, pages_obj, res_obj, info_obj, outlines_obj = 0, *page_obj, *content_obj;
    pd_status st = PD_OK;

    if (!L || !fn || npages <= 0) {
        return PD_ERR_ARG;
    }

    if (!opt) {
        pd_pdf_options_init(&defaults);
        opt = &defaults;
    }

    d = pd_layout_doc(L);
    memset(&w, 0, sizeof(w));
    memset(&R, 0, sizeof(R));
    w.fn = fn;
    w.user = user;
    w.hash = 1469598103934665603ULL;

    /* pass 1: fonts, glyphs, images */
    for (pg = 0; pg < npages; pg++) {
        int32_t n = 0;
        pd_draw* it;

        pd_layout_page_items(L, pg, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));

        if (!it) {
            st = PD_ERR_NOMEM;
            goto done;
        }

        pd_layout_page_items(L, pg, it, n, &n);

        for (i = 0; i < n; i++) {
            if (it[i].kind == PD_DRAW_GLYPH && it[i].font) {
                pfont* pf = font_of(&fonts, &nfonts, it[i].font);

                if (!pf) {
                    free(it);
                    st = PD_ERR_NOMEM;
                    goto done;
                }

                if ((int32_t)it[i].glyph < pf->nglyphs) {
                    pf->used[it[i].glyph] = 1;

                    if (!pf->uni[it[i].glyph]) {
                        pf->uni[it[i].glyph] = it[i].text;
                    }
                }
            } else if (it[i].kind == PD_DRAW_IMAGE && it[i].resource) {
                for (k = 0; k < nimages && images[k].res != it[i].resource; k++) {
                }

                if (k == nimages) {
                    pimage* q = (pimage*)realloc(images, ((size_t)nimages + 1) * sizeof(pimage));

                    if (!q) {
                        free(it);
                        st = PD_ERR_NOMEM;
                        goto done;
                    }

                    images = q;
                    memset(&images[nimages], 0, sizeof(pimage));
                    images[nimages++].res = it[i].resource;
                }
            }
        }

        free(it);
    }

    /* Type 3 codes: used glyphs in id order, 256 per chunk */
    for (i = 0; i < nfonts; i++) {
        pfont* pf = &fonts[i];
        int32_t g;

        pf->used[0] = 1;

        if (pf->truetype) {
            close_composites(pf);
            continue;
        }

        for (g = 0; g < pf->nglyphs; g++) {
            if (pf->used[g]) {
                pf->t3code[g] = (uint32_t)pf->t3count + 1;
                pf->t3count++;
            }
        }

        pf->nchunks = (pf->t3count + 255) / 256;
        pf->t3obj = (int32_t*)calloc((size_t)pf->nchunks + 1, sizeof(int32_t));
    }

    /* object numbers */
    catalog = new_obj(&w);
    pages_obj = new_obj(&w);
    res_obj = new_obj(&w);
    info_obj = new_obj(&w);
    page_obj = (int32_t*)malloc((size_t)npages * sizeof(int32_t));
    content_obj = (int32_t*)malloc((size_t)npages * sizeof(int32_t));

    if (!page_obj || !content_obj) {
        free(page_obj);
        free(content_obj);
        st = PD_ERR_NOMEM;
        goto done;
    }

    for (pg = 0; pg < npages; pg++) {
        page_obj[pg] = new_obj(&w);
        content_obj[pg] = new_obj(&w);
    }

    for (i = 0; i < nfonts; i++) {
        if (fonts[i].truetype) {
            fonts[i].obj = new_obj(&w);
        } else {
            for (k = 0; k < fonts[i].nchunks; k++) {
                fonts[i].t3obj[k] = new_obj(&w);
            }
        }
    }

    for (i = 0; i < nimages; i++) {
        images[i].obj = new_obj(&w);
    }

    /* outline entries from headings and titles, in reading order */
    if (opt->outlines) {
        pd_block_id b = PD_ROOT_ID;
        blk* bx;

        while ((bx = pd_doc_blk(d, b)) && bx->kind != PD_BLOCK_PARAGRAPH && bx->nkids) {
            b = bx->kids[0];
        }

        for (b = (bx && bx->kind == PD_BLOCK_PARAGRAPH) ? b : 0; b; b = pd_doc_next_paragraph(d, b)) {
            blk* p = pd_doc_blk(d, b);
            int32_t page;
            pd_sp x, base, asc, desc;
            outline* q;
            size_t o = 0, j;
            pd_pos at;

            at.block = b;
            at.offset = 0;

            if (!p || (p->st.role != PD_ROLE_HEADING && p->st.role != PD_ROLE_TITLE) ||
                    pd_layout_caret(L, at, &page, &x, &base, &asc, &desc) != PD_OK) {
                continue;
            }

            q = (outline*)realloc(outs, ((size_t)nout + 1) * sizeof(outline));

            if (!q) {
                break;
            }

            outs = q;
            memset(&outs[nout], 0, sizeof(outline));
            outs[nout].block = b;
            outs[nout].level = p->st.role == PD_ROLE_TITLE ? 0 : p->st.level;
            outs[nout].page = page;
            outs[nout].y = base - asc;

            for (j = 0; j < p->st.len && o + 1 < sizeof(outs[nout].title); j++) {
                if (j + 2 < p->st.len && !memcmp(p->st.text + j, "\xEF\xBF\xBC", 3)) {
                    j += 2;     /* objects are not part of a title */
                    continue;
                }

                outs[nout].title[o++] = p->st.text[j];
            }

            outs[nout].obj = new_obj(&w);
            nout++;
        }

        if (nout) {
            outlines_obj = new_obj(&w);
        }
    }

    w_raw(&w, "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n", 15);

    /* catalog, page tree, shared resources */
    begin_obj(&w, catalog);
    w_fmt(&w, "<< /Type /Catalog /Pages %d 0 R", (int)pages_obj);

    if (outlines_obj) {
        w_fmt(&w, " /Outlines %d 0 R /PageMode /UseOutlines", (int)outlines_obj);
    }

    w_str(&w, " >>");
    end_obj(&w);
    begin_obj(&w, pages_obj);
    w_str(&w, "<< /Type /Pages /Kids [");

    for (pg = 0; pg < npages; pg++) {
        w_fmt(&w, "%d 0 R ", (int)page_obj[pg]);
    }

    w_fmt(&w, "] /Count %d >>", (int)npages);
    end_obj(&w);
    /* pages and their content */
    for (pg = 0; pg < npages && !w.err; pg++) {
        pd_page_info pi;
        pd_draw* it;
        int32_t n = 0;
        sbuf c;
        char W[32], H[32];
        const pfont* cur_font = NULL;
        int32_t cur_chunk = -1;
        pd_sp cur_size = 0, run_y = 0, pen = 0;
        uint32_t cur_color = 0;
        int have_color = 0;         /* the text colour is set (white is one: no colour value can say "unset") */
        int32_t cur_scale = 65536;  /* Tz, per page content stream */
        int in_text = 0, in_tj = 0, run_rot = 0;  /* (run_rot: the run is of a turned glyph) */

        memset(&c, 0, sizeof(c));
        pd_layout_page_info(L, pg, &pi);
        num(W, sizeof(W), pi.width);
        num(H, sizeof(H), pi.height);
        begin_obj(&w, page_obj[pg]);
        w_fmt(&w, "<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s] /Resources %d 0 R /Contents %d 0 R >>",
              (int)pages_obj, W, H, (int)res_obj, (int)content_obj[pg]);
        end_obj(&w);

        pd_layout_page_items(L, pg, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));

        if (!it) {
            st = PD_ERR_NOMEM;
            break;
        }

        pd_layout_page_items(L, pg, it, n, &n);

        for (i = 0; i < n; i++) {
            const pd_draw* a = &it[i];

            if (a->kind != PD_DRAW_GLYPH || !a->font) {
                if (in_tj) {
                    sb_fmt(&c, "] TJ\n");
                    in_tj = 0;
                }

                if (in_text) {
                    sb_fmt(&c, "ET\n");
                    in_text = 0;
                    cur_font = NULL;
                    have_color = 0;
                }

                if (a->kind == PD_DRAW_RULE) {
                    int ga = see_through(a->color);

                    if (ga >= 0) {
                        sb_fmt(&c, "q /GA%d gs ", ga);
                    }

                    sb_rgb(&c, a->color, "rg");
                    sb_num(&c, a->x);
                    sb_num(&c, (int64_t)pi.height - a->y - a->h);
                    sb_num(&c, a->w);
                    sb_num(&c, a->h);
                    sb_fmt(&c, ga >= 0 ? "re f Q\n" : "re f\n");
                } else if (a->kind == PD_DRAW_PATH && a->points && a->npoints >= 2) {
                    int32_t q, first;
                    int stroke = a->line_width > 0 && a->color, fill = a->fill != 0;
                    pd_gradient grd;
                    int hasg = fill && pd_draw_gradient(a, &grd);
                    int ga = see_through(fill && !hasg ? a->fill : a->color);

                    if (ga >= 0) {
                        sb_fmt(&c, "q /GA%d gs ", ga);
                    }

                    if (fill && !hasg && a->style && a->style->pattern) {   /* a pattern: filled on its own */
                        int32_t pt = pdf_pattern(&w, a->style, (int64_t)pi.height, opt->compress);

                        res_add(&R, 'P', pt);
                        sb_fmt(&c, "q /Pattern cs /Pt%d scn ", (int)pt);
                        sb_path(&c, a, 1, (int64_t)pi.height);
                        sb_fmt(&c, "f Q\n");
                        fill = 0;

                        if (!stroke) {
                            if (ga >= 0) {
                                sb_fmt(&c, "Q\n");
                            }

                            continue;
                        }
                    }

                    if (hasg) {     /* a gradient: drawn on its own, then the outline */
                        pdf_gradient(&w, &c, &R, a, &grd, pi.width, (int64_t)pi.height, opt->compress);
                        fill = 0;

                        if (!stroke) {
                            if (ga >= 0) {
                                sb_fmt(&c, "Q\n");
                            }

                            continue;
                        }
                    }

                    if (fill) {
                        sb_rgb(&c, a->fill, "rg");
                    }

                    if (stroke && a->style) {   /* dashes, ends and corners: for this path only */
                        const pd_path_style* ps = a->style;

                        sb_fmt(&c, "q ");
                        sb_rgb(&c, a->color, "RG");
                        sb_num(&c, a->line_width);
                        sb_fmt(&c, "w %d J %d j 8 M [", ps->cap == PD_CAP_ROUND ? 1 : ps->cap == PD_CAP_SQUARE ? 2 : 0,
                               ps->join == PD_JOIN_MITER ? 0 : ps->join == PD_JOIN_BEVEL ? 2 : 1);

                        for (q = 0; q < ps->dash_count && q < 8; q++) {
                            sb_num(&c, ps->dash[q]);
                        }

                        sb_fmt(&c, "] 0 d ");
                    } else if (stroke) {
                        sb_rgb(&c, a->color, "RG");
                        sb_num(&c, a->line_width);
                        sb_fmt(&c, "w 1 j ");
                    }

                    (void)first;
                    sb_path(&c, a, (a->path_flags & PD_PATH_CLOSED) != 0, (int64_t)pi.height);
                    sb_fmt(&c, fill && stroke ? "B" : fill ? "f" : "S");
                    sb_fmt(&c, stroke && a->style ? " Q\n" : "\n");

                    if (ga >= 0) {
                        sb_fmt(&c, "Q\n");
                    }
                } else if (a->kind == PD_DRAW_IMAGE || a->kind == PD_DRAW_BOX) {
                    for (k = 0; k < nimages && (a->kind != PD_DRAW_IMAGE || images[k].res != a->resource); k++) {
                    }

                    if (a->kind == PD_DRAW_IMAGE && k < nimages) {
                        sb_fmt(&c, "q ");

                        if (a->clip_points && a->clip_npoints >= 3) {   /* cut to its shape */
                            sb_rings(&c, a->clip_points, a->clip_npoints, 1, (int64_t)pi.height);
                            sb_fmt(&c, "W n ");
                        }

                        if (a->rotation || a->flip) {   /* turned and flipped about its middle */
                            double cx = (a->clip_w > 0 ? a->clip_x + a->clip_w / 2.0 : a->x + a->w / 2.0) / 65536.0;
                            double cy = ((double)pi.height - (a->clip_w > 0 ? a->clip_y + a->clip_h / 2.0 :
                                                              a->y + a->h / 2.0)) / 65536.0;
                            double th = a->rotation / 60000.0 * 3.14159265358979 / 180;

                            sb_fmt(&c, "1 0 0 1 %.4f %.4f cm %.6f %.6f %.6f %.6f 0 0 cm %d 0 0 %d 0 0 cm 1 0 0 1 %.4f %.4f cm ",
                                   cx, cy, cos(th), -sin(th), sin(th), cos(th), a->flip & PD_FLIP_H ? -1 : 1,
                                   a->flip & PD_FLIP_V ? -1 : 1, -cx, -cy);
                        }

                        if (a->clip_w > 0 && a->clip_h > 0) {   /* cropped: shown only in its frame */
                            sb_num(&c, a->clip_x);
                            sb_num(&c, (int64_t)pi.height - a->clip_y - a->clip_h);
                            sb_num(&c, a->clip_w);
                            sb_num(&c, a->clip_h);
                            sb_fmt(&c, "re W n ");
                        }

                        sb_num(&c, a->w);
                        sb_fmt(&c, "0 0 ");
                        sb_num(&c, a->h);
                        sb_num(&c, a->x);
                        sb_num(&c, (int64_t)pi.height - a->y - a->h);
                        sb_fmt(&c, "cm /Im%d Do Q\n", (int)k);
                    }
                }

                continue;
            }

            /* glyphs: one TJ run while font, size, color and baseline stay the same */
            {
                const pfont* pf = NULL;
                int32_t chunk = 0, code;
                int32_t adv1000;

                for (k = 0; k < nfonts; k++) {
                    if (fonts[k].font == a->font) {
                        pf = &fonts[k];
                    }
                }

                if (!pf || (int32_t)a->glyph >= pf->nglyphs) {
                    continue;
                }

                if (pf->truetype) {
                    code = (int32_t)a->glyph;
                } else {
                    code = (int32_t)pf->t3code[a->glyph] - 1;
                    chunk = code / 256;
                    code %= 256;
                }

                adv1000 = (int32_t)((int64_t)pd_font_glyph_advance(a->font, a->glyph) * 1000 /
                                    a->font->m.units_per_em);

                if (in_tj && (pf != cur_font || chunk != cur_chunk || a->size != cur_size || a->y != run_y ||
                              !have_color || a->color != cur_color || (a->scale ? a->scale : 65536) != cur_scale ||
                              a->rotation || run_rot)) {
                    sb_fmt(&c, "] TJ\n");
                    in_tj = 0;
                }

                if (!in_text) {
                    sb_fmt(&c, "BT\n");
                    in_text = 1;
                }

                if (!have_color || a->color != cur_color) {
                    sb_rgb(&c, a->color, "rg");
                    cur_color = a->color;
                    have_color = 1;
                }

                if (pf != cur_font || chunk != cur_chunk || a->size != cur_size) {
                    if (pf->truetype) {
                        sb_fmt(&c, "/F%d ", (int)pf->index - 1);
                    } else {
                        sb_fmt(&c, "/F%dc%d ", (int)pf->index - 1, (int)chunk);
                    }

                    sb_num(&c, a->size);
                    sb_fmt(&c, "Tf\n");
                    cur_font = pf;
                    cur_chunk = chunk;
                    cur_size = a->size;
                }

                if ((a->scale ? a->scale : 65536) != cur_scale) {    /* font expansion: horizontal scaling */
                    cur_scale = a->scale ? a->scale : 65536;
                    sb_fmt(&c, "%lld.%03lld Tz\n", (long long)((int64_t)cur_scale * 100 / 65536),
                           (long long)((int64_t)cur_scale * 100000 / 65536 % 1000));
                }

                if (!in_tj && a->rotation) {   /* turned (clockwise on the page): a run of its own */
                    double th = a->rotation / 60000.0 * 3.14159265358979323846 / 180;

                    sb_fmt(&c, "%.6f %.6f %.6f %.6f ", cos(th), -sin(th), sin(th), cos(th));
                    sb_num(&c, a->x);
                    sb_num(&c, (int64_t)pi.height - a->y);
                    sb_fmt(&c, "Tm [");
                    in_tj = 1;
                    run_rot = 1;
                    run_y = a->y;
                    pen = a->x;
                } else if (!in_tj) {
                    sb_fmt(&c, "1 0 0 1 ");
                    sb_num(&c, a->x);
                    sb_num(&c, (int64_t)pi.height - a->y);
                    sb_fmt(&c, "Tm [");
                    in_tj = 1;
                    run_rot = 0;
                    run_y = a->y;
                    pen = a->x;
                } else if (a->x != pen) {
                    /* TJ adjustment in thousandths of an em (scaled by Tz): positive moves left */
                    int64_t adj = -((int64_t)(a->x - pen) * 1000 * 65536) / ((int64_t)a->size * cur_scale);

                    if (adj) {
                        sb_fmt(&c, "%lld", (long long)adj);
                    }

                    pen += (pd_sp)(-adj * a->size / 1000 * cur_scale / 65536);
                }

                sb_fmt(&c, pf->truetype ? "<%04X>" : "<%02X>", (unsigned)code);
                pen += (pd_sp)((int64_t)adv1000 * a->size / 1000 * cur_scale / 65536);
            }
        }

        if (in_tj) {
            sb_fmt(&c, "] TJ\n");
        }

        if (in_text) {
            sb_fmt(&c, "ET\n");
        }

        free(it);
        stream_obj(&w, content_obj[pg], "", c.p ? c.p : "", c.n, opt->compress);
        free(c.p);
    }

    /* the resources, the shadings and the rest the pages made among them */
    begin_obj(&w, res_obj);
    w_str(&w, "<< /ProcSet [/PDF /Text /ImageC /ImageB] /Font <<");

    for (i = 0; i < nfonts; i++) {
        if (fonts[i].truetype) {
            w_fmt(&w, " /F%d %d 0 R", (int)i, (int)fonts[i].obj);
        } else {
            for (k = 0; k < fonts[i].nchunks; k++) {
                w_fmt(&w, " /F%dc%d %d 0 R", (int)i, (int)k, (int)fonts[i].t3obj[k]);
            }
        }
    }

    w_str(&w, " >> /ExtGState <<");     /* see-through fills and strokes, in sixteenths */

    for (i = 0; i <= 16; i++) {
        w_fmt(&w, " /GA%d << /ca %d.%04d /CA %d.%04d >>", (int)i, (int)(i / 16), (int)(i % 16 * 625),
              (int)(i / 16), (int)(i % 16 * 625));
    }

    w_res(&w, &R, 'G', "SG");
    w_str(&w, " >> /XObject <<");

    for (i = 0; i < nimages; i++) {
        w_fmt(&w, " /Im%d %d 0 R", (int)i, (int)images[i].obj);
    }

    w_res(&w, &R, 'X', "Xo");
    w_str(&w, " >> /Shading <<");
    w_res(&w, &R, 'S', "Sh");
    w_str(&w, " >> /Pattern <<");
    w_res(&w, &R, 'P', "Pt");
    w_str(&w, " >> >>");
    end_obj(&w);


    /* fonts */
    for (i = 0; i < nfonts && !w.err; i++) {
        pfont* pf = &fonts[i];
        const pd_font* f = pf->font;
        int32_t upem = f->m.units_per_em, g;
        uint32_t head = find_tab(f, TAG('h', 'e', 'a', 'd'), NULL);
        int32_t bb[4];
        char tag[8];

        bb[0] = (int16_t)RU16(f, head + 36) * 1000 / upem;
        bb[1] = (int16_t)RU16(f, head + 38) * 1000 / upem;
        bb[2] = (int16_t)RU16(f, head + 40) * 1000 / upem;
        bb[3] = (int16_t)RU16(f, head + 42) * 1000 / upem;

        /* subset tag: six letters from a hash of the used glyphs */
        {
            uint64_t h = 1469598103934665603ULL;

            for (g = 0; g < pf->nglyphs; g++) {
                h = (h ^ pf->used[g]) * 1099511628211ULL;
            }

            for (k = 0; k < 6; k++) {
                tag[k] = (char)('A' + h % 26);
                h /= 26;
            }

            tag[6] = '\0';
        }

        if (pf->truetype) {
            int32_t cid = new_obj(&w), desc = new_obj(&w), file = new_obj(&w), tu = new_obj(&w);
            sbuf ff, cm;
            uint32_t* codes = (uint32_t*)malloc((size_t)pf->nglyphs * sizeof(uint32_t));

            memset(&ff, 0, sizeof(ff));
            memset(&cm, 0, sizeof(cm));

            begin_obj(&w, pf->obj);
            w_fmt(&w, "<< /Type /Font /Subtype /Type0 /BaseFont /%s+%s /Encoding /Identity-H /DescendantFonts [%d 0 R] "
                  "/ToUnicode %d 0 R >>", tag, pf->name, (int)cid, (int)tu);
            end_obj(&w);

            begin_obj(&w, cid);
            w_fmt(&w, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s+%s /CIDSystemInfo << /Registry (Adobe) "
                  "/Ordering (Identity) /Supplement 0 >> /FontDescriptor %d 0 R /CIDToGIDMap /Identity /W [",
                  tag, pf->name, (int)desc);

            for (g = 0; g < pf->nglyphs; g++) {
                if (pf->used[g]) {
                    w_fmt(&w, "%d [%d] ", (int)g, (int)((int64_t)pd_font_glyph_advance(f, (uint32_t)g) * 1000 / upem));
                }
            }

            w_str(&w, "] >>");
            end_obj(&w);

            begin_obj(&w, desc);
            w_fmt(&w, "<< /Type /FontDescriptor /FontName /%s+%s /Flags 32 /FontBBox [%d %d %d %d] /ItalicAngle 0 "
                  "/Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 %d 0 R >>", tag, pf->name, (int)bb[0],
                  (int)bb[1], (int)bb[2], (int)bb[3], (int)(f->m.ascender * 1000 / upem),
                  (int)(f->m.descender * 1000 / upem),
                  (int)((f->m.cap_height ? f->m.cap_height : f->m.ascender) * 1000 / upem), (int)file);
            end_obj(&w);

            if (subset_truetype(pf, &ff) == 0) {
                char dict[64];

                snprintf(dict, sizeof(dict), "/Length1 %lu", (unsigned long)ff.n);
                stream_obj(&w, file, dict, ff.p, ff.n, opt->compress);
            } else {
                stream_obj(&w, file, "", "", 0, 0);
            }

            for (g = 0; codes && g < pf->nglyphs; g++) {
                codes[g] = (uint32_t)g;
            }

            if (codes) {
                to_unicode(&cm, codes, pf->uni, pf->nglyphs, 2);
            }

            stream_obj(&w, tu, "", cm.p ? cm.p : "", cm.n, opt->compress);
            free(codes);
            free(ff.p);
            free(cm.p);
        } else {
            /* Type 3: one font per 256 glyphs, glyph procedures from the outlines */
            for (k = 0; k < pf->nchunks && !w.err; k++) {
                int32_t first = -1, last = -1, cp_obj = new_obj(&w), tu = new_obj(&w), nproc = 0, j;
                int32_t* procs = (int32_t*)calloc(256, sizeof(int32_t));
                int32_t* gid_of = (int32_t*)malloc(256 * sizeof(int32_t));
                uint32_t codes[256], unis[256];
                sbuf cm;

                if (!procs || !gid_of) {
                    free(procs);
                    free(gid_of);
                    st = PD_ERR_NOMEM;
                    break;
                }

                memset(&cm, 0, sizeof(cm));

                for (j = 0; j < 256; j++) {
                    gid_of[j] = -1;
                    codes[j] = (uint32_t)j;
                    unis[j] = 0;
                }

                for (g = 0; g < pf->nglyphs; g++) {
                    int32_t cc = (int32_t)pf->t3code[g] - 1;

                    if (cc >= k * 256 && cc < (k + 1) * 256) {
                        gid_of[cc - k * 256] = g;
                        unis[cc - k * 256] = pf->uni[g];
                        first = first < 0 ? cc - k * 256 : first;
                        last = cc - k * 256;
                        procs[cc - k * 256] = new_obj(&w);
                        nproc++;
                    }
                }

                begin_obj(&w, pf->t3obj[k]);
                w_fmt(&w, "<< /Type /Font /Subtype /Type3 /Name /F%dc%d /FontBBox [%d %d %d %d] "
                      "/FontMatrix [0.001 0 0 0.001 0 0] /CharProcs %d 0 R /Encoding << /Type /Encoding /Differences [%d",
                      (int)i, (int)k, (int)bb[0], (int)bb[1], (int)bb[2], (int)bb[3], (int)cp_obj, (int)first);

                for (j = first; j <= last && j >= 0; j++) {
                    w_fmt(&w, " /g%d", (int)j);
                }

                w_fmt(&w, "] >> /FirstChar %d /LastChar %d /Widths [", (int)first, (int)last);

                for (j = first; j <= last && j >= 0; j++) {
                    w_fmt(&w, "%d ", gid_of[j] >= 0 ? (int)((int64_t)pd_font_glyph_advance(f, (uint32_t)gid_of[j]) *
                                                            1000 / upem) : 0);
                }

                w_fmt(&w, "] /Resources << >> /ToUnicode %d 0 R >>", (int)tu);
                end_obj(&w);

                begin_obj(&w, cp_obj);
                w_str(&w, "<<");

                for (j = 0; j < 256; j++) {
                    if (procs[j]) {
                        w_fmt(&w, " /g%d %d 0 R", (int)j, (int)procs[j]);
                    }
                }

                w_str(&w, " >>");
                end_obj(&w);

                for (j = 0; j < 256 && !w.err; j++) {
                    sbuf gp;
                    t3ctx tc;
                    pd_outline_sink sink;

                    if (!procs[j]) {
                        continue;
                    }

                    memset(&gp, 0, sizeof(gp));
                    tc.b = &gp;
                    tc.upem = upem;
                    memset(&sink, 0, sizeof(sink));
                    sink.move_to = t3_move;
                    sink.line_to = t3_line;
                    sink.cubic_to = t3_cubic;
                    sink.close = t3_close;
                    sb_fmt(&gp, "%d 0 d0\n", (int)((int64_t)pd_font_glyph_advance(f, (uint32_t)gid_of[j]) * 1000 / upem));

                    if (gid_of[j] > 0 && pd_font_glyph_outline(f, (uint32_t)gid_of[j], &sink, &tc) == PD_OK) {
                        sb_fmt(&gp, "f\n");
                    }

                    stream_obj(&w, procs[j], "", gp.p ? gp.p : "", gp.n, opt->compress);
                    free(gp.p);
                }

                to_unicode(&cm, codes, unis, 256, 1);
                stream_obj(&w, tu, "", cm.p ? cm.p : "", cm.n, opt->compress);
                free(cm.p);
                free(procs);
                free(gid_of);
                (void)nproc;
            }

        }
    }

    /* images */
    for (i = 0; i < nimages && !w.err; i++) {
        const char* mime;
        const void* data;
        size_t len;
        int32_t iw = 0, ih = 0;
        int comps;

        if (pd_doc_resource(d, images[i].res, &mime, &data, &len) != PD_OK) {
            len = 0;
            data = "";
            mime = "";
        }

        if (jpeg_info((const unsigned char*)data, len, &iw, &ih, &comps) == 0) {
            char dict[160];

            snprintf(dict, sizeof(dict), "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /%s "
                     "/BitsPerComponent 8 /Filter /DCTDecode", (int)iw, (int)ih,
                     comps == 1 ? "DeviceGray" : comps == 4 ? "DeviceCMYK" : "DeviceRGB");
            begin_obj(&w, images[i].obj);
            w_fmt(&w, "<< %s /Length %lu >>\nstream\n", dict, (unsigned long)len);
            w_raw(&w, data, len);
            w_str(&w, "\nendstream");
            end_obj(&w);
        } else {
            unsigned char* rgb = NULL, *alpha = NULL;
            char dict[200];

            if (png_decode((const unsigned char*)data, len, &iw, &ih, &rgb, &alpha) == 0 ||
                    gif_decode((const unsigned char*)data, len, &iw, &ih, &rgb, &alpha) == 0) {
                int32_t sm = alpha ? new_obj(&w) : 0;

                snprintf(dict, sizeof(dict), "/Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB "
                         "/BitsPerComponent 8%s", (int)iw, (int)ih, sm ? "" : "");

                if (sm) {
                    size_t dl = strlen(dict);

                    snprintf(dict + dl, sizeof(dict) - dl, " /SMask %d 0 R", (int)sm);
                }

                stream_obj(&w, images[i].obj, dict, rgb, (size_t)iw * ih * 3, 1);

                if (sm) {
                    snprintf(dict, sizeof(dict), "/Type /XObject /Subtype /Image /Width %d /Height %d "
                             "/ColorSpace /DeviceGray /BitsPerComponent 8", (int)iw, (int)ih);
                    stream_obj(&w, sm, dict, alpha, (size_t)iw * ih, 1);
                }
            } else {    /* unknown or unsupported data: a light gray placeholder pixel */
                stream_obj(&w, images[i].obj, "/Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace "
                           "/DeviceGray /BitsPerComponent 8", "\xDD", 1, 0);
            }

            free(rgb);
            free(alpha);
        }
    }

    /* outlines: a flat list nested by level */
    if (outlines_obj && !w.err) {
        int32_t* parent = (int32_t*)malloc((size_t)nout * sizeof(int32_t));

        if (parent) {
            for (i = 0; i < nout; i++) {    /* parent: the nearest earlier entry with a lower level */
                parent[i] = -1;

                for (k = i - 1; k >= 0; k--) {
                    if (outs[k].level < outs[i].level) {
                        parent[i] = k;
                        break;
                    }
                }
            }

            for (i = 0; i < nout && !w.err; i++) {
                int32_t prev = -1, next = -1, first = -1, last = -1, count = 0;
                char ybuf[32];

                for (k = i - 1; k >= 0; k--) {
                    if (parent[k] == parent[i]) {
                        prev = k;
                        break;
                    }
                }

                for (k = i + 1; k < nout; k++) {
                    if (parent[k] == parent[i]) {
                        next = k;
                        break;
                    }
                }

                for (k = i + 1; k < nout; k++) {
                    if (parent[k] == i) {
                        first = first < 0 ? k : first;
                        last = k;
                        count++;
                    }
                }

                {
                    pd_page_info opi;

                    pd_layout_page_info(L, outs[i].page, &opi);
                    num(ybuf, sizeof(ybuf), (int64_t)opi.height - outs[i].y);
                }

                begin_obj(&w, outs[i].obj);
                w_str(&w, "<< /Title <FEFF");

                {
                    /* UTF-16BE title */
                    const unsigned char* t = (const unsigned char*)outs[i].title;

                    while (*t) {
                        uint32_t cp = *t++;
                        int n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0, j;

                        cp &= n ? 0x3F >> n : 0x7F;

                        for (j = 0; j < n && *t; j++) {
                            cp = (cp << 6) | (*t++ & 0x3F);
                        }

                        if (cp >= 0x10000) {
                            cp -= 0x10000;
                            w_fmt(&w, "%04X%04X", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
                        } else {
                            w_fmt(&w, "%04X", cp);
                        }
                    }
                }

                w_fmt(&w, "> /Parent %d 0 R /Dest [%d 0 R /XYZ 0 %s 0]", (int)(parent[i] >= 0 ? outs[parent[i]].obj :
                        outlines_obj), (int)page_obj[outs[i].page], ybuf);

                if (prev >= 0) {
                    w_fmt(&w, " /Prev %d 0 R", (int)outs[prev].obj);
                }

                if (next >= 0) {
                    w_fmt(&w, " /Next %d 0 R", (int)outs[next].obj);
                }

                if (first >= 0) {
                    w_fmt(&w, " /First %d 0 R /Last %d 0 R /Count %d", (int)outs[first].obj, (int)outs[last].obj,
                          (int)count);
                }

                w_str(&w, " >>");
                end_obj(&w);
            }

            begin_obj(&w, outlines_obj);
            {
                int32_t first = -1, last = -1, count = 0;

                for (i = 0; i < nout; i++) {
                    if (parent[i] < 0) {
                        first = first < 0 ? i : first;
                        last = i;
                        count++;
                    }
                }

                w_fmt(&w, "<< /Type /Outlines /First %d 0 R /Last %d 0 R /Count %d >>", (int)outs[first].obj,
                      (int)outs[last].obj, (int)count);
            }
            end_obj(&w);
            free(parent);
        }
    }

    /* document information (no dates: the file depends on the content only) */
    begin_obj(&w, info_obj);
    w_str(&w, "<< /Producer (Parade 0.1.0)");

    if (opt->title[0] || (nout && outs[0].level == 0)) {
        const char* t = opt->title[0] ? opt->title : outs[0].title;

        w_str(&w, " /Title <FEFF");

        while (*t) {
            uint32_t cp = (unsigned char) * t++;
            int n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0, j;

            cp &= n ? 0x3F >> n : 0x7F;

            for (j = 0; j < n && *t; j++) {
                cp = (cp << 6) | ((unsigned char) * t++ & 0x3F);
            }

            if (cp < 0x10000) {
                w_fmt(&w, "%04X", cp);
            }
        }

        w_str(&w, ">");
    }

    if (opt->author[0]) {
        size_t j;

        w_str(&w, " /Author (");

        for (j = 0; opt->author[j] && j < sizeof(opt->author); j++) {
            char ch = opt->author[j];

            if (ch == '(' || ch == ')' || ch == '\\') {
                w_raw(&w, "\\", 1);
            }

            w_raw(&w, &ch, 1);
        }

        w_str(&w, ")");
    }

    w_str(&w, " >>");
    end_obj(&w);

    /* cross-reference table and trailer */
    {
        uint64_t xref = w.pos, id = w.hash;

        w_fmt(&w, "xref\n0 %d\n0000000000 65535 f \n", (int)w.nobj + 1);

        for (i = 1; i <= w.nobj; i++) {
            w_fmt(&w, "%010llu 00000 n \n", (unsigned long long)w.offs[i]);
        }

        w_fmt(&w, "trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R /ID [<%016llX%016llX> <%016llX%016llX>] >>\n"
              "startxref\n%llu\n%%%%EOF\n", (int)w.nobj + 1, (int)catalog, (int)info_obj, (unsigned long long)id,
              (unsigned long long)(id * 31), (unsigned long long)id, (unsigned long long)(id * 31),
              (unsigned long long)xref);
    }

    w_flush(&w);
    free(page_obj);
    free(content_obj);

    if (w.err && st == PD_OK) {
        st = PD_ERR_IO;
    }

done:

    for (i = 0; i < nfonts; i++) {
        free(fonts[i].used);
        free(fonts[i].uni);
        free(fonts[i].t3code);
        free(fonts[i].t3obj);
    }

    free(fonts);
    free(images);
    free(outs);
    free(R.r);
    free(w.offs);
    return st;
}
