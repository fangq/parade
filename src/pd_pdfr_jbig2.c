/*
 * Parade PDF reader: a JBIG2 decoder for JBIG2Decode (ITU T.88)
 *
 * What PDFs use: generic regions (arithmetic, with typical prediction, or
 * MMR), symbol dictionaries and text regions (arithmetic, refinement
 * included), the page put together from them by its regions' combination
 * operators. Halftone regions, pattern dictionaries and Huffman-coded
 * dictionaries and text regions are not decoded (their region is left as
 * the page's default). The output is what the filter gives a PDF: rows of
 * 1-bit samples, 0 black.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr.h"

/* ------------------------------------------------------------------ */
/* the MQ arithmetic decoder (Annex E)                                */
/* ------------------------------------------------------------------ */

static const struct {
    uint16_t qe;
    uint8_t nmps, nlps, sw;
} qe_tab[47] = {
    {0x5601, 1, 1, 1}, {0x3401, 2, 6, 0}, {0x1801, 3, 9, 0}, {0x0ac1, 4, 12, 0}, {0x0521, 5, 29, 0},
    {0x0221, 38, 33, 0}, {0x5601, 7, 6, 1}, {0x5401, 8, 14, 0}, {0x4801, 9, 14, 0}, {0x3801, 10, 14, 0},
    {0x3001, 11, 17, 0}, {0x2401, 12, 18, 0}, {0x1c01, 13, 20, 0}, {0x1601, 29, 21, 0}, {0x5601, 15, 14, 1},
    {0x5401, 16, 14, 0}, {0x5101, 17, 15, 0}, {0x4801, 18, 16, 0}, {0x3801, 19, 17, 0}, {0x3401, 20, 18, 0},
    {0x3001, 21, 19, 0}, {0x2801, 22, 19, 0}, {0x2401, 23, 20, 0}, {0x2201, 24, 21, 0}, {0x1c01, 25, 22, 0},
    {0x1801, 26, 23, 0}, {0x1601, 27, 24, 0}, {0x1401, 28, 25, 0}, {0x1201, 29, 26, 0}, {0x1101, 30, 27, 0},
    {0x0ac1, 31, 28, 0}, {0x09c1, 32, 29, 0}, {0x08a1, 33, 30, 0}, {0x0521, 34, 31, 0}, {0x0441, 35, 32, 0},
    {0x02a1, 36, 33, 0}, {0x0221, 37, 34, 0}, {0x0141, 38, 35, 0}, {0x0111, 39, 36, 0}, {0x0085, 40, 37, 0},
    {0x0049, 41, 38, 0}, {0x0025, 42, 39, 0}, {0x0015, 43, 40, 0}, {0x0009, 44, 41, 0}, {0x0005, 45, 42, 0},
    {0x0001, 45, 43, 0}, {0x5601, 46, 46, 0}
};

typedef struct {
    const uint8_t* p;
    size_t n, bp;
    uint32_t chigh, clow, a;
    int ct;
} mq;

static uint32_t mq_byte(const mq* m, size_t i) {
    return i < m->n ? m->p[i] : 0xFF;
}

static void mq_bytein(mq* m) {
    if (mq_byte(m, m->bp) == 0xFF) {
        if (mq_byte(m, m->bp + 1) > 0x8F) {
            m->clow += 0xFF00;
            m->ct = 8;
        } else {
            m->bp++;
            m->clow += mq_byte(m, m->bp) << 9;
            m->ct = 7;
        }
    } else {
        m->bp++;
        m->clow += m->bp < m->n ? (uint32_t)m->p[m->bp] << 8 : 0xFF00;
        m->ct = 8;
    }

    if (m->clow > 0xFFFF) {
        m->chigh += m->clow >> 16;
        m->clow &= 0xFFFF;
    }
}

static void mq_init(mq* m, const uint8_t* p, size_t n) {
    m->p = p;
    m->n = n;
    m->bp = 0;
    m->chigh = mq_byte(m, 0);
    m->clow = 0;
    mq_bytein(m);
    m->chigh = ((m->chigh << 7) & 0xFFFF) | ((m->clow >> 9) & 0x7F);
    m->clow = (m->clow << 7) & 0xFFFF;
    m->ct -= 7;
    m->a = 0x8000;
}

static int mq_bit(mq* m, uint8_t* cx) {
    int idx = *cx >> 1, mps = *cx & 1, d;
    uint32_t qe = qe_tab[idx].qe, a = m->a - qe;

    if (m->chigh < qe) {
        if (a < qe) {
            a = qe;
            d = mps;
            idx = qe_tab[idx].nmps;
        } else {
            a = qe;
            d = 1 ^ mps;

            if (qe_tab[idx].sw) {
                mps = d;
            }

            idx = qe_tab[idx].nlps;
        }
    } else {
        m->chigh -= qe;

        if (a & 0x8000) {
            m->a = a;
            return mps;
        }

        if (a < qe) {
            d = 1 ^ mps;

            if (qe_tab[idx].sw) {
                mps = d;
            }

            idx = qe_tab[idx].nlps;
        } else {
            d = mps;
            idx = qe_tab[idx].nmps;
        }
    }

    do {
        if (m->ct == 0) {
            mq_bytein(m);
        }

        a <<= 1;
        m->chigh = ((m->chigh << 1) & 0xFFFF) | ((m->clow >> 15) & 1);
        m->clow = (m->clow << 1) & 0xFFFF;
        m->ct--;
    } while (!(a & 0x8000));

    m->a = a;
    *cx = (uint8_t)((idx << 1) | mps);
    return d;
}

/* ---- integers (Annex A.2) and symbol ids (A.3) ---- */

#define OOB INT32_MIN

typedef struct {
    uint8_t cx[512];
} iactx;

static uint32_t ia_bits(mq* m, iactx* c, uint32_t* prev, int len) {
    uint32_t v = 0;
    int i;

    for (i = 0; i < len; i++) {
        int b = mq_bit(m, &c->cx[*prev]);

        *prev = *prev < 256 ? (*prev << 1) | (uint32_t)b : ((((*prev << 1) | (uint32_t)b) & 511) | 256);
        v = (v << 1) | (uint32_t)b;
    }

    return v;
}

static int32_t ia_int(mq* m, iactx* c) {
    uint32_t prev = 1, v;
    int sign = (int)ia_bits(m, c, &prev, 1);

    if (!ia_bits(m, c, &prev, 1)) {
        v = ia_bits(m, c, &prev, 2);
    } else if (!ia_bits(m, c, &prev, 1)) {
        v = ia_bits(m, c, &prev, 4) + 4;
    } else if (!ia_bits(m, c, &prev, 1)) {
        v = ia_bits(m, c, &prev, 6) + 20;
    } else if (!ia_bits(m, c, &prev, 1)) {
        v = ia_bits(m, c, &prev, 8) + 84;
    } else if (!ia_bits(m, c, &prev, 1)) {
        v = ia_bits(m, c, &prev, 12) + 340;
    } else {
        v = ia_bits(m, c, &prev, 32) + 4436;
    }

    if (v > 0x7FFFFFFF) {
        v = 0x7FFFFFFF;
    }

    if (sign) {
        return v ? -(int32_t)v : OOB;
    }

    return (int32_t)v;
}

static uint32_t ia_id(mq* m, uint8_t* cx, int len) {
    uint32_t prev = 1;
    int i;

    for (i = 0; i < len; i++) {
        prev = (prev << 1) | (uint32_t)mq_bit(m, &cx[prev]);
    }

    return len < 31 ? prev & ((1u << len) - 1) : prev & 0x7FFFFFFF;
}

/* ------------------------------------------------------------------ */
/* bitmaps: a byte a pixel, 1 black                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t w, h;
    uint8_t* px;
} jbm;

static jbm* bm_new(int32_t w, int32_t h) {
    jbm* b;

    if (w < 0 || h < 0 || (int64_t)w * h > ((int64_t)1 << 26)) {    /* past any scan's page */
        return NULL;
    }

    b = (jbm*)calloc(1, sizeof(jbm));

    if (!b) {
        return NULL;
    }

    b->w = w;
    b->h = h;
    b->px = (uint8_t*)calloc((size_t)w * (size_t)h + 1, 1);

    if (!b->px) {
        free(b);
        return NULL;
    }

    return b;
}

static void bm_free(jbm* b) {
    if (b) {
        free(b->px);
        free(b);
    }
}

static int px_at(const jbm* b, int32_t x, int32_t y) {
    return x >= 0 && y >= 0 && x < b->w && y < b->h ? b->px[(size_t)y * (size_t)b->w + (size_t)x] : 0;
}

/* B into A at (x, y) by operator op: 0 OR, 1 AND, 2 XOR, 3 XNOR, 4 REPLACE */
static void bm_compose(jbm* a, const jbm* b, int32_t x, int32_t y, int op) {
    int32_t i, j;

    for (j = 0; j < b->h; j++) {
        int32_t yy = y + j;

        if (yy < 0 || yy >= a->h) {
            continue;
        }

        for (i = 0; i < b->w; i++) {
            int32_t xx = x + i;
            uint8_t* d, s;

            if (xx < 0 || xx >= a->w) {
                continue;
            }

            d = &a->px[(size_t)yy * (size_t)a->w + (size_t)xx];
            s = b->px[(size_t)j * (size_t)b->w + (size_t)i];

            switch (op) {
                case 0: *d |= s; break;
                case 1: *d &= s; break;
                case 2: *d ^= s; break;
                case 3: *d = (uint8_t)(!(*d ^ s)); break;
                default: *d = s;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* generic region decoding (6.2)                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    int8_t dx, dy;
} off;

/* each template's pixels, the context's highest bit first; AT pixels (index into at[]) as dx = 127 + k */
static const off tmpl0[16] = { {127, 3}, {-1, -2}, {0, -2}, {1, -2}, {127, 2}, {127, 1}, {-2, -1}, {-1, -1},
                               {0, -1}, {1, -1}, {2, -1}, {127, 0}, {-4, 0}, {-3, 0}, {-2, 0}, {-1, 0} };
static const off tmpl1[13] = { {-1, -2}, {0, -2}, {1, -2}, {2, -2}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {2, -1},
                               {127, 0}, {-3, 0}, {-2, 0}, {-1, 0} };
static const off tmpl2[10] = { {-1, -2}, {0, -2}, {1, -2}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {127, 0},
                               {-2, 0}, {-1, 0} };
static const off tmpl3[10] = { {-3, -1}, {-2, -1}, {-1, -1}, {0, -1}, {1, -1}, {127, 0}, {-4, 0}, {-3, 0},
                               {-2, 0}, {-1, 0} };

static jbm* generic_arith(mq* m, uint8_t* cx, int32_t w, int32_t h, int tmpl, int tpgdon, const int8_t* at) {
    static const uint32_t sltp_cx[4] = { 0x9B25, 0x0795, 0x00E5, 0x0195 };
    const off* t = tmpl == 0 ? tmpl0 : tmpl == 1 ? tmpl1 : tmpl == 2 ? tmpl2 : tmpl3;
    int n = tmpl == 0 ? 16 : tmpl == 1 ? 13 : 10, k, ltp = 0;
    int8_t dx[16], dy[16];
    jbm* b = bm_new(w, h);
    int32_t x, y;

    if (!b) {
        return NULL;
    }

    for (k = 0; k < n; k++) {
        if (t[k].dx == 127) {
            dx[k] = at[2 * t[k].dy];
            dy[k] = at[2 * t[k].dy + 1];
        } else {
            dx[k] = t[k].dx;
            dy[k] = t[k].dy;
        }
    }

    for (y = 0; y < h; y++) {
        uint8_t* row = b->px + (size_t)y * (size_t)w;

        if (tpgdon) {
            ltp ^= mq_bit(m, &cx[sltp_cx[tmpl]]);

            if (ltp) {  /* the same as the row above */
                if (y > 0) {
                    memcpy(row, row - w, (size_t)w);
                }

                continue;
            }
        }

        for (x = 0; x < w; x++) {
            uint32_t c = 0;

            for (k = 0; k < n; k++) {
                c = (c << 1) | (uint32_t)px_at(b, x + dx[k], y + dy[k]);
            }

            row[x] = (uint8_t)mq_bit(m, &cx[c]);
        }
    }

    return b;
}

/* MMR: T.6, the same as CCITT G4 (whose decoder gives 1 black when BlackIs1) */
static jbm* generic_mmr(const uint8_t* p, size_t n, int32_t w, int32_t h) {
    size_t len;
    unsigned char* bits = pd_ccitt_decode(p, n, -1, w, h, 1, 0, &len);
    jbm* b = bits ? bm_new(w, h) : NULL;
    int32_t x, y;
    size_t row = (size_t)(w + 7) / 8;

    if (b) {
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t at = (size_t)y * row + (size_t)x / 8;

                b->px[(size_t)y * (size_t)w + (size_t)x] = at < len ? (bits[at] >> (7 - x % 8)) & 1 : 0;
            }
        }
    }

    free(bits);
    return b;
}

/* ---- generic refinement (6.3) ---- */

static jbm* refine(mq* m, uint8_t* cx, int32_t w, int32_t h, int tmpl, const jbm* ref, int32_t rdx, int32_t rdy,
                   const int8_t* rat) {
    jbm* b = bm_new(w, h);
    int32_t x, y;

    if (!b) {
        return NULL;
    }

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t c;
            int32_t rx = x - rdx, ry = y - rdy;

            if (tmpl == 0) {
                c = (uint32_t)px_at(b, x + rat[0], y + rat[1]);
                c = (c << 1) | (uint32_t)px_at(b, x - 1, y - 1);
                c = (c << 1) | (uint32_t)px_at(b, x, y - 1);
                c = (c << 1) | (uint32_t)px_at(b, x + 1, y - 1);
                c = (c << 1) | (uint32_t)px_at(b, x - 1, y);
                c = (c << 1) | (uint32_t)px_at(ref, rx + rat[2], ry + rat[3]);
                c = (c << 1) | (uint32_t)px_at(ref, rx - 1, ry - 1);
                c = (c << 1) | (uint32_t)px_at(ref, rx, ry - 1);
                c = (c << 1) | (uint32_t)px_at(ref, rx + 1, ry - 1);
                c = (c << 1) | (uint32_t)px_at(ref, rx - 1, ry);
                c = (c << 1) | (uint32_t)px_at(ref, rx, ry);
                c = (c << 1) | (uint32_t)px_at(ref, rx + 1, ry);
                c = (c << 1) | (uint32_t)px_at(ref, rx - 1, ry + 1);
                c = (c << 1) | (uint32_t)px_at(ref, rx, ry + 1);
                c = (c << 1) | (uint32_t)px_at(ref, rx + 1, ry + 1);
            } else {
                c = (uint32_t)px_at(b, x - 1, y - 1);
                c = (c << 1) | (uint32_t)px_at(b, x, y - 1);
                c = (c << 1) | (uint32_t)px_at(b, x + 1, y - 1);
                c = (c << 1) | (uint32_t)px_at(b, x - 1, y);
                c = (c << 1) | (uint32_t)px_at(ref, rx, ry - 1);
                c = (c << 1) | (uint32_t)px_at(ref, rx - 1, ry);
                c = (c << 1) | (uint32_t)px_at(ref, rx, ry);
                c = (c << 1) | (uint32_t)px_at(ref, rx + 1, ry);
                c = (c << 1) | (uint32_t)px_at(ref, rx, ry + 1);
                c = (c << 1) | (uint32_t)px_at(ref, rx + 1, ry + 1);
            }

            b->px[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)mq_bit(m, &cx[c]);
        }
    }

    return b;
}

/* ------------------------------------------------------------------ */
/* segments                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t num;
    int type;
    uint32_t refs[64];
    int nrefs;
    const uint8_t* data;
    size_t len;
    jbm** syms;                 /* a symbol dictionary's exported symbols */
    int32_t nsyms;
} jseg;

typedef struct {
    jseg* s;
    int n, cap;
    jbm* page;
    int page_op, page_def;
    int32_t page_h_known;
} jdoc;

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t be16(const uint8_t* p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

/* the segment headers of an embedded stream (no file header), each with its data */
static void read_segments(jdoc* J, const uint8_t* p, size_t n) {
    size_t i = 0;

    while (i + 11 <= n) {
        jseg s;
        uint32_t num = be32(p + i), len;
        int flags = p[i + 4], rc = p[i + 5] >> 5, nref = rc, k, refsize;
        size_t j;

        memset(&s, 0, sizeof(s));
        s.num = num;
        s.type = flags & 63;

        if (rc == 7) {
            if (i + 9 > n) {
                break;
            }

            nref = (int)(be32(p + i + 5) & 0x1FFFFFFF);
            j = i + 9 + (size_t)(nref + 8) / 8;
        } else {
            j = i + 6;
        }

        refsize = num <= 256 ? 1 : num <= 65536 ? 2 : 4;

        for (k = 0; k < nref; k++, j += (size_t)refsize) {
            uint32_t r;

            if (j + (size_t)refsize > n) {
                return;
            }

            r = refsize == 1 ? p[j] : refsize == 2 ? be16(p + j) : be32(p + j);

            if (s.nrefs < 64) {
                s.refs[s.nrefs++] = r;
            }
        }

        j += (flags & 64) ? 4 : 1;     /* the page it belongs to */

        if (j + 4 > n) {
            break;
        }

        len = be32(p + j);
        j += 4;

        if (len == 0xFFFFFFFFu) {   /* an immediate generic region of unknown length: to the end */
            len = (uint32_t)(n - j);
        }

        if (j + len > n) {
            len = (uint32_t)(n - j);
        }

        s.data = p + j;
        s.len = len;

        if (J->n >= J->cap) {
            int nc = J->cap ? J->cap * 2 : 32;
            jseg* q = (jseg*)realloc(J->s, sizeof(jseg) * (size_t)nc);

            if (!q) {
                return;
            }

            J->s = q;
            J->cap = nc;
        }

        J->s[J->n++] = s;
        i = j + len;

        if (s.type == 51) {     /* end of file */
            break;
        }
    }
}

static jseg* find_seg(jdoc* J, uint32_t num) {
    int i;

    for (i = 0; i < J->n; i++) {
        if (J->s[i].num == num) {
            return &J->s[i];
        }
    }

    return NULL;
}

/* the symbols of the dictionaries a segment refers to, in order */
static jbm** referred_symbols(jdoc* J, const jseg* s, int32_t* n) {
    int32_t total = 0, k = 0;
    int i;
    jbm** out;

    for (i = 0; i < s->nrefs; i++) {
        jseg* r = find_seg(J, s->refs[i]);

        if (r && r->type == 0) {
            total += r->nsyms;
        }
    }

    out = (jbm**)calloc((size_t)total + 1, sizeof(jbm*));

    if (!out) {
        *n = 0;
        return NULL;
    }

    for (i = 0; i < s->nrefs; i++) {
        jseg* r = find_seg(J, s->refs[i]);
        int32_t m;

        if (r && r->type == 0) {
            for (m = 0; m < r->nsyms; m++) {
                out[k++] = r->syms[m];
            }
        }
    }

    *n = k;
    return out;
}

static int code_len(int32_t n) {
    int len = 0;

    while (len < 31 && ((int64_t)1 << len) < n) {
        len++;
    }

    return len;
}

/* ---- text regions (6.4), arithmetic ---- */

typedef struct {
    iactx dt, fs, ds, it, ri, rdw, rdh, rdx, rdy, ai, ex, dh, dw;
    uint8_t* id;
    uint8_t gb[65536];
    uint8_t gr[8192];
} jctx;

static jbm* text_region(mq* m, jctx* C, int32_t w, int32_t h, int32_t ninst, jbm** syms, int32_t nsyms, int flags,
                        const int8_t* rat) {
    int refine_on = (flags >> 1) & 1, logstrips = (flags >> 2) & 3, refcorner = (flags >> 4) & 3;
    int transposed = (flags >> 6) & 1, op = (flags >> 7) & 3, defpx = (flags >> 9) & 1, rtmpl = (flags >> 15) & 1;
    int32_t dsoff = (flags >> 10) & 31, strips = 1 << logstrips, stript, firsts = 0, i = 0;
    int idlen = code_len(nsyms);
    jbm* b = bm_new(w, h);

    if (!b) {
        return NULL;
    }

    if (dsoff > 15) {
        dsoff -= 32;
    }

    if (defpx) {
        memset(b->px, 1, (size_t)w * (size_t)h);
    }

    stript = ia_int(m, &C->dt);

    if (stript == OOB) {
        return b;
    }

    stript = -stript * strips;

    while (i < ninst) {
        int32_t dt = ia_int(m, &C->dt), dfs, curs;
        int guard = 0;

        if (dt == OOB) {
            break;
        }

        stript += dt * strips;
        dfs = ia_int(m, &C->fs);

        if (dfs == OOB) {
            break;
        }

        firsts += dfs;
        curs = firsts;

        for (;;) {
            int32_t curt = strips > 1 ? ia_int(m, &C->it) : 0;
            int32_t t = stript + curt, id, sw, sh, ot, os, ds;
            jbm* sym, *made = NULL;

            id = (int32_t)ia_id(m, C->id, idlen);

            if (id < 0 || id >= nsyms || !syms[id]) {
                return b;   /* a damaged stream: what was placed is kept */
            }

            sym = syms[id];

            if (refine_on && ia_int(m, &C->ri)) {
                int32_t rdw = ia_int(m, &C->rdw), rdh = ia_int(m, &C->rdh), rdx = ia_int(m, &C->rdx);
                int32_t rdy = ia_int(m, &C->rdy);

                if (rdw == OOB || rdh == OOB || rdx == OOB || rdy == OOB) {
                    return b;
                }

                made = refine(m, C->gr, sym->w + rdw, sym->h + rdh, rtmpl, sym, (rdw >> 1) + rdx, (rdh >> 1) + rdy,
                              rat);

                if (!made) {
                    return b;
                }

                sym = made;
            }

            sw = sym->w;
            sh = sym->h;

            if (!transposed) {
                if (refcorner & 2) {    /* right: the instance ends where S is */
                    curs += sw - 1;
                }

                ot = (refcorner & 1) ? t : t - sh + 1;     /* top or bottom */
                os = (refcorner & 2) ? curs - sw + 1 : curs;
                bm_compose(b, sym, os, ot, op);

                if (!(refcorner & 2)) {
                    curs += sw - 1;
                }
            } else {
                /* transposed: S runs down the page, T across; the corner's top or bottom is S's, its left or
                   right T's (6.4.5 3c) */
                if (!(refcorner & 1)) {
                    curs += sh - 1;
                }

                ot = (refcorner & 2) ? t - sw + 1 : t;
                os = (refcorner & 1) ? curs : curs - sh + 1;
                bm_compose(b, sym, ot, os, op);

                if (refcorner & 1) {
                    curs += sh - 1;
                }
            }

            bm_free(made);
            i++;

            if (i >= ninst || ++guard > 1000000) {
                break;
            }

            ds = ia_int(m, &C->ds);

            if (ds == OOB) {
                break;
            }

            curs += ds + dsoff;
        }
    }

    return b;
}

/* ---- symbol dictionaries (6.5), arithmetic ---- */

static void symbol_dict(jdoc* J, jseg* s, jctx* C) {
    const uint8_t* p = s->data;
    size_t n = s->len, at = 2;
    int flags, huff, refagg, tmpl, rtmpl;
    int8_t gat[8] = { 3, -1, -3, -1, 2, -2, -2, -2 }, rat[4] = { -1, -1, -1, -1 };
    uint32_t nex, nnew;
    jbm** in, **all;
    int32_t nin, k, ndec = 0, hc = 0;
    mq m;

    if (n < 2) {
        return;
    }

    flags = (int)be16(p);
    huff = flags & 1;
    refagg = (flags >> 1) & 1;
    tmpl = (flags >> 10) & 3;
    rtmpl = (flags >> 12) & 1;

    if (huff) {
        return;     /* Huffman: not done */
    }

    if (tmpl == 0) {
        if (at + 8 > n) {
            return;
        }

        memcpy(gat, p + at, 8);
        at += 8;
    } else {
        if (at + 2 > n) {
            return;
        }

        memcpy(gat, p + at, 2);
        at += 2;
    }

    if (refagg && !rtmpl) {
        if (at + 4 > n) {
            return;
        }

        memcpy(rat, p + at, 4);
        at += 4;
    }

    if (at + 8 > n) {
        return;
    }

    nex = be32(p + at);
    nnew = be32(p + at + 4);
    at += 8;

    if (nnew > 1000000 || nex > 2000000) {
        return;
    }

    in = referred_symbols(J, s, &nin);
    all = (jbm**)calloc((size_t)nin + nnew + 1, sizeof(jbm*));

    if (!all) {
        free(in);
        return;
    }

    for (k = 0; k < nin; k++) {
        all[k] = in[k];
    }

    memset(C, 0, sizeof(*C) - sizeof(C->gb) - sizeof(C->gr));
    memset(C->gb, 0, sizeof(C->gb));
    memset(C->gr, 0, sizeof(C->gr));
    mq_init(&m, p + at, n - at);

    if (refagg) {
        int len = code_len(nin + (int32_t)nnew);

        C->id = (uint8_t*)calloc((size_t)1 << (len + 1), 1);
    }

    while (ndec < (int32_t)nnew) {
        int32_t dh = ia_int(&m, &C->dh), sw = 0;

        if (dh == OOB) {
            break;
        }

        hc += dh;

        if (hc < 0 || hc > 8192) {  /* a symbol taller than any glyph of a scan: a damaged stream */
            break;
        }

        for (;;) {
            int32_t dw = ia_int(&m, &C->dw);
            jbm* sym = NULL;

            if (dw == OOB || ndec >= (int32_t)nnew) {
                break;
            }

            sw += dw;

            if (sw < 0 || sw > 8192) {
                ndec = (int32_t)nnew;
                break;
            }

            if (!refagg) {
                sym = generic_arith(&m, C->gb, sw, hc, tmpl, 0, gat);
            } else {
                int32_t ninst = ia_int(&m, &C->ai);

                if (ninst == 1) {
                    int len = code_len(nin + (int32_t)nnew);
                    int32_t id = (int32_t)ia_id(&m, C->id, len), rdx = ia_int(&m, &C->rdx), rdy = ia_int(&m, &C->rdy);

                    if (id >= 0 && id < nin + ndec && all[id] && rdx != OOB && rdy != OOB) {
                        sym = refine(&m, C->gr, sw, hc, rtmpl, all[id], rdx, rdy, rat);
                    }
                } else if (ninst > 1) {     /* an aggregate: a text region of the symbols so far */
                    jctx* T = (jctx*)calloc(1, sizeof(jctx));

                    if (T) {
                        int len = code_len(nin + (int32_t)nnew);

                        T->id = (uint8_t*)calloc((size_t)1 << (len + 1), 1);
                        memcpy(T->gr, C->gr, sizeof(T->gr));
                        sym = text_region(&m, T, sw, hc, ninst, all, nin + ndec, 2 | (rtmpl << 15), rat);
                        free(T->id);
                        free(T);
                    }
                }
            }

            if (!sym) {
                sym = bm_new(sw > 0 ? sw : 0, hc);
            }

            all[nin + ndec] = sym;
            ndec++;
        }
    }

    /* the symbols exported: runs of IAEX, off then on */
    {
        int32_t total = nin + ndec, i = 0, cur = 0, nout = 0;
        jbm** out = (jbm**)calloc((size_t)total + 1, sizeof(jbm*));
        uint8_t* kept = (uint8_t*)calloc((size_t)total + 1, 1);

        while (out && kept && i < total) {
            int32_t run = ia_int(&m, &C->ex), r;

            if (run == OOB || run < 0) {
                break;
            }

            for (r = 0; r < run && i < total; r++, i++) {
                if (cur) {
                    out[nout++] = all[i];
                    kept[i] = 1;
                }
            }

            cur = !cur;
        }

        /* the new symbols not exported are not needed */
        for (i = nin; i < total; i++) {
            if (!kept || !kept[i]) {
                bm_free(all[i]);
            }
        }

        s->syms = out;
        s->nsyms = nout;
        free(kept);
    }

    free(C->id);
    C->id = NULL;
    free(all);
    free(in);
}

/* ------------------------------------------------------------------ */
/* the page                                                           */
/* ------------------------------------------------------------------ */

static void region(jdoc* J, jseg* s, jctx* C) {
    const uint8_t* p = s->data;
    size_t n = s->len, at = 17;
    int32_t w, h, x, y, op;
    jbm* b = NULL;

    if (n < 18 || !J->page) {
        return;
    }

    w = (int32_t)be32(p);
    h = (int32_t)be32(p + 4);
    x = (int32_t)be32(p + 8);
    y = (int32_t)be32(p + 12);
    op = p[16] & 7;

    if (w <= 0 || w > 1 << 20 || h == 0) {
        return;
    }

    if (h < 0 || h > 1 << 20) {     /* unknown: the page's height below it */
        h = J->page->h - y;
    }

    if (s->type == 6 || s->type == 7 || s->type == 4) {     /* text region */
        int flags = (int)be16(p + at);
        int8_t rat[4] = { -1, -1, -1, -1 };
        int32_t ninst, nsyms;
        jbm** syms;
        mq m;

        at += 2;

        if (flags & 1) {
            return;     /* Huffman: not done */
        }

        if ((flags & 2) && !((flags >> 15) & 1)) {
            if (at + 4 > n) {
                return;
            }

            memcpy(rat, p + at, 4);
            at += 4;
        }

        if (at + 4 > n) {
            return;
        }

        ninst = (int32_t)be32(p + at);
        at += 4;

        if (ninst < 0 || ninst > 4000000) {     /* more glyphs than a page holds: a damaged stream */
            return;
        }
        syms = referred_symbols(J, s, &nsyms);

        if (!syms) {
            return;
        }

        memset(C, 0, sizeof(*C) - sizeof(C->gb) - sizeof(C->gr));
        memset(C->gr, 0, sizeof(C->gr));
        C->id = (uint8_t*)calloc((size_t)1 << (code_len(nsyms) + 1), 1);
        mq_init(&m, p + at, n - at);

        if (C->id) {
            b = text_region(&m, C, w, h, ninst, syms, nsyms, flags, rat);
        }

        free(C->id);
        C->id = NULL;
        free(syms);
    } else if (s->type == 36 || s->type == 38 || s->type == 39) {   /* generic region */
        int gflags = p[at], mmr = gflags & 1, tmpl = (gflags >> 1) & 3, tpgdon = (gflags >> 3) & 1;
        int8_t gat[8] = { 3, -1, -3, -1, 2, -2, -2, -2 };

        at++;

        if (!mmr) {
            size_t na = tmpl == 0 ? 8 : 2;

            if (at + na > n) {
                return;
            }

            memcpy(gat, p + at, na);
            at += na;
        }

        if (mmr) {
            b = generic_mmr(p + at, n - at, w, h);
        } else {
            mq m;

            memset(C->gb, 0, sizeof(C->gb));
            mq_init(&m, p + at, n - at);
            b = generic_arith(&m, C->gb, w, h, tmpl, tpgdon, gat);
        }
    } else if (s->type == 40 || s->type == 42 || s->type == 43) {   /* generic refinement of the page */
        /* not done: the page as it is */
        return;
    } else {
        return;     /* halftone: not done */
    }

    if (b) {
        bm_compose(J->page, b, x, y, op);
        bm_free(b);
    }
}

unsigned char* pd_jbig2_decode(const unsigned char* data, size_t n, const unsigned char* globals, size_t gn,
                               int32_t w, int32_t h, size_t* outlen);

unsigned char* pd_jbig2_decode(const unsigned char* data, size_t n, const unsigned char* globals, size_t gn,
                               int32_t w, int32_t h, size_t* outlen) {
    jdoc J;
    jctx* C = (jctx*)calloc(1, sizeof(jctx));
    int i;
    unsigned char* out = NULL;

    memset(&J, 0, sizeof(J));

    if (!C) {
        return NULL;
    }

    if (globals) {
        read_segments(&J, globals, gn);
    }

    read_segments(&J, data, n);

    for (i = 0; i < J.n; i++) {
        jseg* s = &J.s[i];

        switch (s->type) {
            case 0:
                symbol_dict(&J, s, C);
                break;

            case 48:    /* page information */
                if (s->len >= 19 && !J.page) {
                    int32_t pw = (int32_t)be32(s->data), ph = (int32_t)be32(s->data + 4);
                    int pf = s->data[16];

                    if (ph < 0 || ph > 1 << 20) {
                        ph = h > 0 ? h : 1;     /* striped, its height known at the end: the image's */
                    }

                    J.page = bm_new(pw > 0 ? pw : w, ph);
                    J.page_def = (pf >> 2) & 1;

                    if (J.page && J.page_def) {
                        memset(J.page->px, 1, (size_t)J.page->w * (size_t)J.page->h);
                    }
                }

                break;

            case 4: case 6: case 7: case 36: case 38: case 39: case 40: case 42: case 43:
                region(&J, s, C);
                break;

            default:
                break;
        }
    }

    if (J.page) {
        size_t row = (size_t)(w + 7) / 8, len = row * (size_t)(h > 0 ? h : 0);
        int32_t x, y;

        out = (unsigned char*)malloc(len + 1);

        if (out) {
            memset(out, 0xFF, len);

            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    if (px_at(J.page, x, y)) {  /* black is 0 in what the filter gives */
                        out[(size_t)y * row + (size_t)x / 8] &= (unsigned char)~(0x80 >> (x % 8));
                    }
                }
            }

            *outlen = len;
        }
    }

    for (i = 0; i < J.n; i++) {
        int32_t k;

        for (k = 0; k < J.s[i].nsyms; k++) {
            /* a symbol is freed once: the dictionary that made it exports it, others only refer to it */
            int owner = 1, q;

            for (q = 0; q < i && owner; q++) {
                int32_t r;

                for (r = 0; r < J.s[q].nsyms; r++) {
                    if (J.s[q].syms[r] == J.s[i].syms[k]) {
                        owner = 0;
                        break;
                    }
                }
            }

            if (owner) {
                bm_free(J.s[i].syms[k]);
            }
        }

        free(J.s[i].syms);
    }

    bm_free(J.page);
    free(J.s);
    free(C);
    return out;
}

/* the arithmetic decoder against T.88's test sequence (H.2): 0 when it decodes as the standard says */
int pd_jbig2_mq_check(void);

int pd_jbig2_mq_check(void) {
    static const uint8_t in[30] = {
        0x84, 0xC7, 0x3B, 0xFC, 0xE1, 0xA1, 0x43, 0x04, 0x02, 0x20, 0x00, 0x00, 0x41, 0x0D, 0xBB, 0x86, 0xF4, 0x31,
        0x7F, 0xFF, 0x88, 0xFF, 0x37, 0x47, 0x1A, 0xDB, 0x6A, 0xDF, 0xFF, 0xAC
    };
    static const uint8_t want[32] = {
        0x00, 0x02, 0x00, 0x51, 0x00, 0x00, 0x00, 0xC0, 0x03, 0x52, 0x87, 0x2A, 0xAA, 0xAA, 0xAA, 0xAA, 0x82, 0xC0,
        0x20, 0x00, 0xFC, 0xD7, 0x9E, 0xF6, 0xBF, 0x7F, 0xED, 0x90, 0x4F, 0x46, 0xA3, 0xBF
    };
    mq m;
    uint8_t cx = 0;
    int i, k;

    mq_init(&m, in, sizeof(in));

    for (i = 0; i < 32; i++) {
        int byte = 0;

        for (k = 0; k < 8; k++) {
            byte = (byte << 1) | mq_bit(&m, &cx);
        }

        if (byte != want[i]) {
            return i + 1;
        }
    }

    return 0;
}
