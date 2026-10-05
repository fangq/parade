/*
 * Parade math: TeX-style formula layout with an OpenType MATH font
 *
 * A LaTeX subset is parsed into atoms (ord, op, bin, rel, open, close,
 * punct, inner) and laid out following TeX's Appendix G, with the
 * constants, italic corrections, accent attachments, size variants and
 * glyph assemblies of the font's MATH table (fonts without one get
 * Latin Modern's values scaled to the em). Everything is integer sp, so
 * a formula looks the same on every platform.
 *
 * Coordinates inside boxes: x right, y down from the baseline; a box has
 * a width, a height above and a depth below its baseline.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

/* ------------------------------------------------------------------ */
/* the font's MATH table                                              */
/* ------------------------------------------------------------------ */

enum {
    MC_MATH_LEADING, MC_AXIS, MC_ACCENT_BASE, MC_FLAT_ACCENT_BASE, MC_SUB_DOWN, MC_SUB_TOP_MAX, MC_SUB_DROP_MIN,
    MC_SUP_UP, MC_SUP_UP_CRAMPED, MC_SUP_BOTTOM_MIN, MC_SUP_DROP_MAX, MC_SUBSUP_GAP, MC_SUP_BOTTOM_MAX_WITH_SUB,
    MC_SPACE_AFTER_SCRIPT, MC_UPPER_LIMIT_GAP, MC_UPPER_LIMIT_RISE, MC_LOWER_LIMIT_GAP, MC_LOWER_LIMIT_DROP,
    MC_STACK_TOP_UP, MC_STACK_TOP_UP_D, MC_STACK_BOTTOM_DOWN, MC_STACK_BOTTOM_DOWN_D, MC_STACK_GAP, MC_STACK_GAP_D,
    MC_STRETCH_TOP_UP, MC_STRETCH_BOTTOM_DOWN, MC_STRETCH_GAP_ABOVE, MC_STRETCH_GAP_BELOW, MC_FRAC_NUM_UP,
    MC_FRAC_NUM_UP_D, MC_FRAC_DEN_DOWN, MC_FRAC_DEN_DOWN_D, MC_FRAC_NUM_GAP, MC_FRAC_NUM_GAP_D, MC_FRAC_RULE,
    MC_FRAC_DEN_GAP, MC_FRAC_DEN_GAP_D, MC_SKEW_HGAP, MC_SKEW_VGAP, MC_OVERBAR_GAP, MC_OVERBAR_RULE,
    MC_OVERBAR_EXTRA, MC_UNDERBAR_GAP, MC_UNDERBAR_RULE, MC_UNDERBAR_EXTRA, MC_RADICAL_GAP, MC_RADICAL_GAP_D,
    MC_RADICAL_RULE, MC_RADICAL_EXTRA, MC_RADICAL_KERN_BEFORE, MC_RADICAL_KERN_AFTER, MC_COUNT
};

/* Latin Modern Math's constants per 1000 units, for fonts without a MATH table */
static const int16_t lm_constants[MC_COUNT] = {
    154, 250, 450, 664, 247, 344, 200, 363, 289, 108, 250, 160, 344, 56, 200, 111, 167, 600, 444, 677, 345, 686,
    120, 280, 111, 111, 200, 167, 394, 677, 345, 686, 40, 120, 40, 40, 120, 350, 96, 120, 40, 40, 120, 40, 40, 50,
    148, 40, 40, 278, -556
};

static uint32_t U16(const pd_font* f, uint64_t off) {
    return off + 2 <= f->len ? ((uint32_t)f->data[off] << 8) | f->data[off + 1] : 0;
}

static int32_t S16(const pd_font* f, uint64_t off) {
    return (int16_t)U16(f, off);
}

static int32_t coverage(const pd_font* f, uint32_t cov, uint32_t g) {
    uint32_t fmt = U16(f, cov), lo = 0, hi = U16(f, cov + 2);

    if (!cov) {
        return -1;
    }

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;

        if (fmt == 1) {
            uint32_t v = U16(f, (uint64_t)cov + 4 + 2 * mid);

            if (v < g) {
                lo = mid + 1;
            } else if (v > g) {
                hi = mid;
            } else {
                return (int32_t)mid;
            }
        } else if (fmt == 2) {
            uint64_t rr = (uint64_t)cov + 4 + 6 * mid;

            if (U16(f, rr + 2) < g) {
                lo = mid + 1;
            } else if (U16(f, rr) > g) {
                hi = mid;
            } else {
                return (int32_t)(U16(f, rr + 4) + g - U16(f, rr));
            }
        } else {
            return -1;
        }
    }

    return -1;
}

int32_t pd_font_has_math(const pd_font* f) {
    return f && f->math != 0 && f->math_len >= 10;
}

/* ------------------------------------------------------------------ */
/* boxes                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_math_item* v;
    int32_t n, cap;
    pd_sp w, h, d;
    pd_sp ic;                   /* italic correction */
    pd_sp accent_x;             /* top accent attachment, -1 = the middle */
    int is_char;                /* a single glyph (scripts attach without baseline drop) */
    uint32_t glyph;
    int err;
} mbox;

enum {
    ORD = 0, OP = 1, BIN = 2, REL = 3, OPEN = 4, CLOSE = 5, PUNCT = 6, INNER = 7, NOATOM = -1
};

typedef struct {
    int cls;
    mbox b;
    int limits;                 /* ops: 1 limits, 0 nolimits, -1 by style */
    int big;                    /* ops that grow in display style */
    int ital;                   /* integral-like: subscript tucked under by the italic correction */
} matom;

typedef struct {
    const pd_font* f;
    uint32_t C, GI, V;          /* MATH constants, glyph info, variants (absolute), 0 = absent */
    int16_t fallback[MC_COUNT];
    int32_t upem;
    pd_sp size;                 /* the formula's text size */
    const char* s;
    size_t n, pos;
    int depth;
    int32_t items;              /* total items made: a bound against pathological input */
} mctx;

static void box_free(mbox* b) {
    free(b->v);
    memset(b, 0, sizeof(*b));
}

static void box_init(mbox* b) {
    memset(b, 0, sizeof(*b));
    b->accent_x = -1;
}

static void box_push(mctx* M, mbox* b, const pd_math_item* it) {
    if (b->err || M->items > 200000) {
        b->err = 1;
        return;
    }

    if (pd_grow((void**)&b->v, &b->cap, (int64_t)b->n + 1, sizeof(pd_math_item))) {
        b->err = 1;
        return;
    }

    b->v[b->n++] = *it;
    M->items++;
}

/* put src into dst at x, moved down by shift; dst's width covers it */
static void box_place(mctx* M, mbox* dst, const mbox* src, pd_sp x, pd_sp shift) {
    int32_t i;

    for (i = 0; i < src->n; i++) {
        pd_math_item it = src->v[i];

        it.x += x;
        it.y += shift;
        box_push(M, dst, &it);
    }

    dst->err |= src->err;
    dst->h = src->h - shift > dst->h ? src->h - shift : dst->h;
    dst->d = src->d + shift > dst->d ? src->d + shift : dst->d;
    dst->w = x + src->w > dst->w ? x + src->w : dst->w;
}

static void box_rule(mctx* M, mbox* b, pd_sp x, pd_sp top, pd_sp w, pd_sp h) {
    pd_math_item it;

    memset(&it, 0, sizeof(it));
    it.kind = 1;
    it.x = x;
    it.y = top;
    it.w = w;
    it.h = h;
    box_push(M, b, &it);
    b->h = -top > b->h ? -top : b->h;
    b->d = top + h > b->d ? top + h : b->d;
    b->w = x + w > b->w ? x + w : b->w;
}

/* ------------------------------------------------------------------ */
/* font data                                                          */
/* ------------------------------------------------------------------ */

static pd_sp SC(const mctx* M, int32_t units, pd_sp size) {
    return pd_scale(units, size, M->upem);
}

static int32_t mconst(const mctx* M, int k) {
    return M->C ? S16(M->f, (uint64_t)M->C + 8 + 4 * k) : M->fallback[k];
}

static pd_sp K(const mctx* M, int k, pd_sp size) {
    return SC(M, mconst(M, k), size);
}

static int32_t script_pct(const mctx* M, int ss) {
    int32_t v = M->C ? S16(M->f, (uint64_t)M->C + (ss ? 2 : 0)) : (ss ? 50 : 70);
    return v > 10 && v <= 100 ? v : (ss ? 50 : 70);
}

/* style 0 display, 1 text, 2 script, 3 scriptscript */
static pd_sp style_size(const mctx* M, int style) {
    return style <= 1 ? M->size : (pd_sp)((int64_t)M->size * script_pct(M, style == 3) / 100);
}

typedef struct {
    int32_t x0, y0, x1, y1;
    int any;
} bbox_t;

static void bb_pt(void* u, int32_t x, int32_t y) {
    bbox_t* b = (bbox_t*)u;

    if (!b->any) {
        b->x0 = b->x1 = x;
        b->y0 = b->y1 = y;
        b->any = 1;
    }

    b->x0 = x < b->x0 ? x : b->x0;
    b->x1 = x > b->x1 ? x : b->x1;
    b->y0 = y < b->y0 ? y : b->y0;
    b->y1 = y > b->y1 ? y : b->y1;
}

static void bb_quad(void* u, int32_t cx, int32_t cy, int32_t x, int32_t y) {
    bb_pt(u, cx, cy);
    bb_pt(u, x, y);
}

static void bb_cubic(void* u, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t x, int32_t y) {
    bb_pt(u, ax, ay);
    bb_pt(u, bx, by);
    bb_pt(u, x, y);
}

/* glyph ink extents in font units (control points included: a little generous on curves) */
static void glyph_bbox_x(const pd_font* f, uint32_t g, int32_t* xmin, int32_t* xmax, int32_t* ymin, int32_t* ymax) {
    pd_outline_sink sk;
    bbox_t b;

    memset(&sk, 0, sizeof(sk));
    memset(&b, 0, sizeof(b));
    sk.move_to = bb_pt;
    sk.line_to = bb_pt;
    sk.quad_to = bb_quad;
    sk.cubic_to = bb_cubic;
    pd_font_glyph_outline(f, g, &sk, &b);
    *ymin = b.any ? b.y0 / 64 : 0;
    *ymax = b.any ? b.y1 / 64 : 0;
    *xmin = b.any ? b.x0 / 64 : 0;
    *xmax = b.any ? b.x1 / 64 : 0;
}

static void glyph_bbox(const pd_font* f, uint32_t g, int32_t* ymin, int32_t* ymax) {
    int32_t x0, x1;

    glyph_bbox_x(f, g, &x0, &x1, ymin, ymax);
}

static int32_t italic_correction(const mctx* M, uint32_t g) {
    uint32_t ic;
    int32_t i;

    if (!M->GI || (ic = U16(M->f, M->GI)) == 0) {
        return 0;
    }

    ic += M->GI;
    i = coverage(M->f, ic + U16(M->f, ic), g);
    return i >= 0 && (uint32_t)i < U16(M->f, ic + 2) ? S16(M->f, (uint64_t)ic + 4 + 4 * i) : 0;
}

static int32_t top_accent(const mctx* M, uint32_t g) {
    uint32_t ta;
    int32_t i;

    if (!M->GI || (ta = U16(M->f, M->GI + 2)) == 0) {
        return -1;
    }

    ta += M->GI;
    i = coverage(M->f, ta + U16(M->f, ta), g);
    return i >= 0 && (uint32_t)i < U16(M->f, ta + 2) ? S16(M->f, (uint64_t)ta + 4 + 4 * i) : -1;
}

/* a box holding one glyph */
static mbox box_glyph(mctx* M, uint32_t g, pd_sp size) {
    mbox b;
    pd_math_item it;
    int32_t y0, y1, acc;

    box_init(&b);
    memset(&it, 0, sizeof(it));
    it.glyph = g;
    it.size = size;
    box_push(M, &b, &it);
    glyph_bbox(M->f, g, &y0, &y1);
    b.w = SC(M, pd_font_glyph_advance(M->f, g), size);
    b.h = y1 > 0 ? SC(M, y1, size) : 0;
    b.d = y0 < 0 ? SC(M, -y0, size) : 0;
    b.ic = SC(M, italic_correction(M, g), size);
    acc = top_accent(M, g);
    b.accent_x = acc >= 0 ? SC(M, acc, size) : -1;
    b.is_char = 1;
    b.glyph = g;
    return b;
}

/* a code point as a box; missing glyphs fall back to the plain character, then to nothing */
static mbox box_char(mctx* M, uint32_t cp, pd_sp size, uint32_t fallback) {
    uint32_t g = pd_font_glyph_index(M->f, cp);

    if (!g && fallback) {
        g = pd_font_glyph_index(M->f, fallback);
    }

    if (!g) {
        mbox b;

        box_init(&b);
        b.w = size / 3;
        return b;
    }

    return box_glyph(M, g, size);
}

/* the vertical (or horizontal) construction of a glyph in MathVariants: absolute offset, 0 = none */
static uint32_t construction(const mctx* M, uint32_t g, int horiz) {
    uint32_t V = M->V, cov;
    int32_t i, nv, nh;

    if (!V) {
        return 0;
    }

    nv = (int32_t)U16(M->f, V + 6);
    nh = (int32_t)U16(M->f, V + 8);
    cov = U16(M->f, V + (horiz ? 4 : 2));

    if (!cov || (i = coverage(M->f, V + cov, g)) < 0 || i >= (horiz ? nh : nv)) {
        return 0;
    }

    return V + U16(M->f, (uint64_t)V + 10 + 2 * (uint64_t)(horiz ? nv + i : i));
}

/*
 * A delimiter (or radical, or big operator) of at least target size
 * (height + depth): the first size variant that is large enough, else
 * the glyph assembly, else the largest variant.
 */
static mbox stretchy(mctx* M, uint32_t g, pd_sp target, pd_sp size) {
    uint32_t c = construction(M, g, 0), asm_off;
    int32_t nvar, i;
    uint32_t last = g;

    if (!c) {
        return box_glyph(M, g, size);
    }

    nvar = (int32_t)U16(M->f, c + 2);

    for (i = 0; i < nvar; i++) {
        uint32_t vg = U16(M->f, (uint64_t)c + 4 + 4 * i);
        pd_sp adv = SC(M, (int32_t)U16(M->f, (uint64_t)c + 6 + 4 * i), size);

        last = vg;

        if (adv >= target) {
            return box_glyph(M, vg, size);
        }
    }

    asm_off = U16(M->f, c);

    if (asm_off) {      /* the assembly: parts bottom to top, extenders repeated */
        uint32_t A = c + asm_off;
        int32_t np = (int32_t)U16(M->f, A + 4), reps, k, r;
        pd_sp omin = SC(M, (int32_t)U16(M->f, M->V), size);

        if (np > 0 && np < 64) {
            for (reps = 0; reps < 64; reps++) {
                pd_sp full = 0, ovl_max = 0x7FFFFFFF, total;
                int32_t joints = -1;

                for (k = 0; k < np; k++) {
                    uint64_t P = (uint64_t)A + 6 + 10 * k;
                    int ext = U16(M->f, P + 8) & 1;
                    pd_sp adv = SC(M, (int32_t)U16(M->f, P + 6), size);
                    pd_sp sc = SC(M, (int32_t)U16(M->f, P + 2), size), ec = SC(M, (int32_t)U16(M->f, P + 4), size);
                    int32_t times = ext ? reps : 1;

                    full += adv * times;
                    joints += times;
                    ovl_max = sc < ovl_max && times ? sc : ovl_max;
                    ovl_max = ec < ovl_max && times ? ec : ovl_max;
                }

                total = full - (joints > 0 ? joints : 0) * omin;

                if (total >= target || reps == 63) {
                    pd_sp ovl = joints > 0 ? (full - target) / joints : 0, y;
                    mbox b;

                    ovl = ovl < omin ? omin : ovl > ovl_max ? ovl_max : ovl;
                    total = full - (joints > 0 ? joints : 0) * ovl;
                    box_init(&b);
                    y = 0;  /* the bottom of the next part, up from the box bottom */

                    for (k = 0; k < np; k++) {
                        uint64_t P = (uint64_t)A + 6 + 10 * k;
                        int ext = U16(M->f, P + 8) & 1;
                        uint32_t pg = U16(M->f, P);
                        pd_sp adv = SC(M, (int32_t)U16(M->f, P + 6), size);

                        for (r = 0; r < (ext ? reps : 1); r++) {
                            mbox part = box_glyph(M, pg, size);

                            /* the part's ink bottom sits at y above the box's baseline (its bottom) */
                            box_place(M, &b, &part, 0, -(y + part.d));
                            box_free(&part);
                            y += adv - ovl;
                        }
                    }

                    b.h = b.h > total ? b.h : total;    /* the stack, its baseline at the bottom */
                    b.is_char = 0;
                    return b;
                }
            }
        }
    }

    return box_glyph(M, last, size);
}

/* a delimiter of a size, centered on the math axis */
static mbox delimiter(mctx* M, uint32_t cp, pd_sp target, pd_sp size) {
    pd_sp axis = K(M, MC_AXIS, size);
    mbox g, b;
    pd_sp shift;

    if (cp == '.') {    /* the null delimiter */
        box_init(&b);
        b.w = size * 12 / 100;
        return b;
    }

    g = stretchy(M, pd_font_glyph_index(M->f, cp) ? pd_font_glyph_index(M->f, cp) : pd_font_glyph_index(M->f, '|'),
                 target, size);
    /* center: (h - d)/2 at the axis */
    shift = (g.h - g.d) / 2 - axis;
    box_init(&b);
    box_place(M, &b, &g, 0, shift);
    b.ic = 0;
    box_free(&g);
    return b;
}

/* ------------------------------------------------------------------ */
/* the parser                                                         */
/* ------------------------------------------------------------------ */

static const struct {
    const char* name;
    uint32_t cp;
    int cls;
} symbols[] = {
    /* Greek */
    { "alpha", 0x1D6FC, ORD }, { "beta", 0x1D6FD, ORD }, { "gamma", 0x1D6FE, ORD }, { "delta", 0x1D6FF, ORD },
    { "epsilon", 0x1D716, ORD }, { "varepsilon", 0x1D700, ORD }, { "zeta", 0x1D701, ORD }, { "eta", 0x1D702, ORD },
    { "theta", 0x1D703, ORD }, { "vartheta", 0x1D717, ORD }, { "iota", 0x1D704, ORD }, { "kappa", 0x1D705, ORD },
    { "lambda", 0x1D706, ORD }, { "mu", 0x1D707, ORD }, { "nu", 0x1D708, ORD }, { "xi", 0x1D709, ORD },
    { "pi", 0x1D70B, ORD }, { "varpi", 0x1D71B, ORD }, { "rho", 0x1D70C, ORD }, { "varrho", 0x1D71A, ORD },
    { "sigma", 0x1D70E, ORD }, { "varsigma", 0x1D70D, ORD }, { "tau", 0x1D70F, ORD }, { "upsilon", 0x1D710, ORD },
    { "phi", 0x1D719, ORD }, { "varphi", 0x1D711, ORD }, { "chi", 0x1D712, ORD }, { "psi", 0x1D713, ORD },
    { "omega", 0x1D714, ORD }, { "Gamma", 0x393, ORD }, { "Delta", 0x394, ORD }, { "Theta", 0x398, ORD },
    { "Lambda", 0x39B, ORD }, { "Xi", 0x39E, ORD }, { "Pi", 0x3A0, ORD }, { "Sigma", 0x3A3, ORD },
    { "Upsilon", 0x3A5, ORD }, { "Phi", 0x3A6, ORD }, { "Psi", 0x3A8, ORD }, { "Omega", 0x3A9, ORD },
    /* ordinary symbols */
    { "infty", 0x221E, ORD }, { "partial", 0x1D715, ORD }, { "nabla", 0x2207, ORD }, { "forall", 0x2200, ORD },
    { "exists", 0x2203, ORD }, { "nexists", 0x2204, ORD }, { "emptyset", 0x2205, ORD }, { "varnothing", 0x2205, ORD },
    { "neg", 0xAC, ORD }, { "lnot", 0xAC, ORD }, { "hbar", 0x210F, ORD }, { "ell", 0x2113, ORD }, { "Re", 0x211C, ORD },
    { "Im", 0x2111, ORD }, { "aleph", 0x2135, ORD }, { "wp", 0x2118, ORD }, { "prime", 0x2032, ORD },
    { "angle", 0x2220, ORD }, { "triangle", 0x25B3, ORD }, { "top", 0x22A4, ORD }, { "bot", 0x22A5, ORD },
    { "ldots", 0x2026, INNER }, { "dots", 0x2026, INNER }, { "cdots", 0x22EF, INNER }, { "vdots", 0x22EE, ORD },
    { "ddots", 0x22F1, INNER }, { "dagger", 0x2020, BIN }, { "ddagger", 0x2021, BIN }, { "S", 0xA7, ORD },
    { "P", 0xB6, ORD }, { "degree", 0xB0, ORD }, { "Box", 0x25A1, ORD }, { "square", 0x25A1, ORD },
    { "clubsuit", 0x2663, ORD }, { "heartsuit", 0x2661, ORD }, { "imath", 0x1D6A4, ORD }, { "jmath", 0x1D6A5, ORD },
    /* binary operators */
    { "pm", 0xB1, BIN }, { "mp", 0x2213, BIN }, { "times", 0xD7, BIN }, { "div", 0xF7, BIN }, { "cdot", 0x22C5, BIN },
    { "ast", 0x2217, BIN }, { "star", 0x22C6, BIN }, { "circ", 0x2218, BIN }, { "bullet", 0x2219, BIN },
    { "cap", 0x2229, BIN }, { "cup", 0x222A, BIN }, { "wedge", 0x2227, BIN }, { "land", 0x2227, BIN },
    { "vee", 0x2228, BIN }, { "lor", 0x2228, BIN }, { "setminus", 0x2216, BIN }, { "oplus", 0x2295, BIN },
    { "ominus", 0x2296, BIN }, { "otimes", 0x2297, BIN }, { "oslash", 0x2298, BIN }, { "odot", 0x2299, BIN },
    { "sqcup", 0x2294, BIN }, { "sqcap", 0x2293, BIN }, { "uplus", 0x228E, BIN }, { "amalg", 0x2A3F, BIN },
    { "wr", 0x2240, BIN }, { "diamond", 0x22C4, BIN }, { "bigtriangleup", 0x25B3, BIN },
    /* relations */
    { "leq", 0x2264, REL }, { "le", 0x2264, REL }, { "geq", 0x2265, REL }, { "ge", 0x2265, REL }, { "neq", 0x2260, REL },
    { "ne", 0x2260, REL }, { "equiv", 0x2261, REL }, { "approx", 0x2248, REL }, { "sim", 0x223C, REL },
    { "simeq", 0x2243, REL }, { "cong", 0x2245, REL }, { "propto", 0x221D, REL }, { "ll", 0x226A, REL },
    { "gg", 0x226B, REL }, { "in", 0x2208, REL }, { "notin", 0x2209, REL }, { "ni", 0x220B, REL },
    { "subset", 0x2282, REL }, { "supset", 0x2283, REL }, { "subseteq", 0x2286, REL }, { "supseteq", 0x2287, REL },
    { "perp", 0x27C2, REL }, { "parallel", 0x2225, REL }, { "mid", 0x2223, REL }, { "vdash", 0x22A2, REL },
    { "dashv", 0x22A3, REL }, { "models", 0x22A8, REL }, { "prec", 0x227A, REL }, { "succ", 0x227B, REL },
    { "preceq", 0x2AAF, REL }, { "succeq", 0x2AB0, REL }, { "doteq", 0x2250, REL }, { "asymp", 0x224D, REL },
    { "to", 0x2192, REL }, { "rightarrow", 0x2192, REL }, { "leftarrow", 0x2190, REL }, { "gets", 0x2190, REL },
    { "leftrightarrow", 0x2194, REL }, { "Rightarrow", 0x21D2, REL }, { "Leftarrow", 0x21D0, REL },
    { "Leftrightarrow", 0x21D4, REL }, { "iff", 0x27FA, REL }, { "implies", 0x27F9, REL }, { "mapsto", 0x21A6, REL },
    { "longrightarrow", 0x27F6, REL }, { "longleftarrow", 0x27F5, REL }, { "uparrow", 0x2191, REL },
    { "downarrow", 0x2193, REL }, { "hookrightarrow", 0x21AA, REL }, { "nearrow", 0x2197, REL },
    { "searrow", 0x2198, REL }, { "colon", ':', PUNCT },
    /* delimiters */
    { "langle", 0x27E8, OPEN }, { "rangle", 0x27E9, CLOSE }, { "lfloor", 0x230A, OPEN }, { "rfloor", 0x230B, CLOSE },
    { "lceil", 0x2308, OPEN }, { "rceil", 0x2309, CLOSE }, { "lbrace", '{', OPEN }, { "rbrace", '}', CLOSE },
    { "lvert", '|', OPEN }, { "rvert", '|', CLOSE }, { "lVert", 0x2016, OPEN }, { "rVert", 0x2016, CLOSE },
    { "vert", '|', ORD }, { "Vert", 0x2016, ORD }, { "backslash", '\\', ORD }
};

static const struct {
    const char* name;
    uint32_t cp;
    int limits;                 /* 1 in display style */
    int integral;
} bigops[] = {
    { "sum", 0x2211, 1, 0 }, { "prod", 0x220F, 1, 0 }, { "coprod", 0x2210, 1, 0 }, { "int", 0x222B, 0, 1 },
    { "iint", 0x222C, 0, 1 }, { "iiint", 0x222D, 0, 1 }, { "oint", 0x222E, 0, 1 }, { "bigcup", 0x22C3, 1, 0 },
    { "bigcap", 0x22C2, 1, 0 }, { "bigoplus", 0x2A01, 1, 0 }, { "bigotimes", 0x2A02, 1, 0 }, { "bigodot", 0x2A00, 1, 0 },
    { "bigvee", 0x22C1, 1, 0 }, { "bigwedge", 0x22C0, 1, 0 }, { "bigsqcup", 0x2A06, 1, 0 }, { "biguplus", 0x2A04, 1, 0 }
};

static const struct {
    const char* name;
    int limits;
} named_ops[] = {
    { "sin", 0 }, { "cos", 0 }, { "tan", 0 }, { "cot", 0 }, { "sec", 0 }, { "csc", 0 }, { "arcsin", 0 }, { "arccos", 0 },
    { "arctan", 0 }, { "sinh", 0 }, { "cosh", 0 }, { "tanh", 0 }, { "coth", 0 }, { "log", 0 }, { "ln", 0 }, { "lg", 0 },
    { "exp", 0 }, { "dim", 0 }, { "ker", 0 }, { "deg", 0 }, { "arg", 0 }, { "hom", 0 }, { "lim", 1 }, { "liminf", 1 },
    { "limsup", 1 }, { "max", 1 }, { "min", 1 }, { "sup", 1 }, { "inf", 1 }, { "det", 1 }, { "gcd", 1 }, { "Pr", 1 },
    { "argmax", 1 }, { "argmin", 1 }
};

static const struct {
    const char* name;
    uint32_t cp;
} accents[] = {
    { "hat", 0x302 }, { "widehat", 0x302 }, { "bar", 0x304 }, { "tilde", 0x303 }, { "widetilde", 0x303 },
    { "vec", 0x20D7 }, { "dot", 0x307 }, { "ddot", 0x308 }, { "acute", 0x301 }, { "grave", 0x300 },
    { "check", 0x30C }, { "breve", 0x306 }, { "mathring", 0x30A }
};

/* letters in a math alphabet */
enum {
    AL_ITALIC = 0, AL_ROMAN, AL_BOLD, AL_BB, AL_CAL, AL_BOLDITALIC, AL_SANS, AL_TT, AL_FRAK
};

static uint32_t alpha(uint32_t c, int al) {
    int upper = c >= 'A' && c <= 'Z', lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
    uint32_t i = upper ? c - 'A' : lower ? c - 'a' : 0;

    if (!upper && !lower && !(digit && (al == AL_BOLD || al == AL_BB || al == AL_SANS || al == AL_TT))) {
        return c;
    }

    switch (al) {
        case AL_ITALIC:
            if (c == 'h') {
                return 0x210E;
            }

            return upper ? 0x1D434 + i : 0x1D44E + i;

        case AL_BOLD:
            return digit ? 0x1D7CE + (c - '0') : upper ? 0x1D400 + i : 0x1D41A + i;

        case AL_BOLDITALIC:
            return upper ? 0x1D468 + i : 0x1D482 + i;

        case AL_SANS:
            return digit ? 0x1D7E2 + (c - '0') : upper ? 0x1D5A0 + i : 0x1D5BA + i;

        case AL_TT:
            return digit ? 0x1D7F6 + (c - '0') : upper ? 0x1D670 + i : 0x1D68A + i;

        case AL_BB:
            if (digit) {
                return 0x1D7D8 + (c - '0');
            }

            switch (c) {
                case 'C':
                    return 0x2102;

                case 'H':
                    return 0x210D;

                case 'N':
                    return 0x2115;

                case 'P':
                    return 0x2119;

                case 'Q':
                    return 0x211A;

                case 'R':
                    return 0x211D;

                case 'Z':
                    return 0x2124;
            }

            return upper ? 0x1D538 + i : 0x1D552 + i;

        case AL_CAL:
            switch (c) {
                case 'B':
                    return 0x212C;

                case 'E':
                    return 0x2130;

                case 'F':
                    return 0x2131;

                case 'H':
                    return 0x210B;

                case 'I':
                    return 0x2110;

                case 'L':
                    return 0x2112;

                case 'M':
                    return 0x2133;

                case 'R':
                    return 0x211B;

                case 'e':
                    return 0x212F;

                case 'g':
                    return 0x210A;

                case 'o':
                    return 0x2134;
            }

            return upper ? 0x1D49C + i : 0x1D4B6 + i;

        case AL_FRAK:
            switch (c) {
                case 'C':
                    return 0x212D;

                case 'H':
                    return 0x210C;

                case 'I':
                    return 0x2111;

                case 'R':
                    return 0x211C;

                case 'Z':
                    return 0x2128;
            }

            return upper ? 0x1D504 + i : 0x1D51E + i;
    }

    return c;
}

static void skip_space(mctx* M) {
    while (M->pos < M->n && (M->s[M->pos] == ' ' || M->s[M->pos] == '\t' || M->s[M->pos] == '\n' ||
                             M->s[M->pos] == '\r')) {
        M->pos++;
    }
}

/* \name or \x; name into buf */
static int read_command(mctx* M, char* buf, size_t cap) {
    size_t k = 0;

    if (M->pos >= M->n || M->s[M->pos] != '\\') {
        return 0;
    }

    M->pos++;

    if (M->pos < M->n && ((M->s[M->pos] >= 'a' && M->s[M->pos] <= 'z') || (M->s[M->pos] >= 'A' &&
                          M->s[M->pos] <= 'Z'))) {
        while (M->pos < M->n && ((M->s[M->pos] >= 'a' && M->s[M->pos] <= 'z') || (M->s[M->pos] >= 'A' &&
                                 M->s[M->pos] <= 'Z')) && k + 1 < cap) {
            buf[k++] = M->s[M->pos++];
        }
    } else if (M->pos < M->n) {
        buf[k++] = M->s[M->pos++];
    }

    buf[k] = '\0';
    return 1;
}

/* one UTF-8 code point */
static uint32_t read_cp(mctx* M) {
    const unsigned char* s = (const unsigned char*)M->s;
    uint32_t c = s[M->pos++];
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, k;

    c &= n ? 0x3F >> n : 0x7F;

    for (k = 0; k < n && M->pos < M->n; k++) {
        c = (c << 6) | (s[M->pos++] & 0x3F);
    }

    return c;
}

typedef struct {
    int style;                  /* 0 D, 1 T, 2 S, 3 SS */
    int cramped;
    int alphabet;
} mstyle;

static mstyle sup_style(mstyle s) {
    s.style = s.style < 2 ? 2 : 3;
    return s;
}

static mstyle sub_style(mstyle s) {
    s = sup_style(s);
    s.cramped = 1;
    return s;
}

static mstyle frac_num_style(mstyle s) {
    s.style = s.style == 0 ? 1 : s.style == 1 ? 2 : 3;
    return s;
}

static mstyle frac_den_style(mstyle s) {
    s = frac_num_style(s);
    s.cramped = 1;
    return s;
}

enum {
    END_EOF = 0, END_BRACE, END_AMP, END_ROW, END_RIGHT, END_END
};

static mbox parse_list(mctx* M, mstyle st, int* ended);

/* an argument: {group} or a single token */
static mbox parse_arg(mctx* M, mstyle st) {
    int ended;
    mbox b;

    skip_space(M);

    if (M->pos < M->n && M->s[M->pos] == '{') {
        M->pos++;
        return parse_list(M, st, &ended);
    }

    if (M->pos >= M->n) {
        box_init(&b);
        return b;
    }

    /* a single token: parse a list of exactly one atom by isolating it */
    {
        size_t start = M->pos, end;
        const char* saved_s = M->s;
        size_t saved_n = M->n;

        if (M->s[M->pos] == '\\') {
            char name[64];

            read_command(M, name, sizeof(name));
        } else {
            read_cp(M);
        }

        end = M->pos;
        M->s = saved_s + start;
        M->n = end - start;
        M->pos = 0;
        b = parse_list(M, st, &ended);
        M->s = saved_s;
        M->n = saved_n;
        M->pos = end;
        return b;
    }
}

/* text in a group, upright, spaces kept */
static mbox parse_text(mctx* M, mstyle st, int alphabet) {
    pd_sp size = style_size(M, st.style);
    mbox b;
    int depth = 0;

    box_init(&b);
    skip_space(M);

    if (M->pos >= M->n || M->s[M->pos] != '{') {
        return b;
    }

    M->pos++;

    while (M->pos < M->n) {
        uint32_t c;
        mbox g;

        if (M->s[M->pos] == '}' && depth == 0) {
            M->pos++;
            break;
        }

        if (M->s[M->pos] == '{') {
            depth++;
            M->pos++;
            continue;
        }

        if (M->s[M->pos] == '}') {
            depth--;
            M->pos++;
            continue;
        }

        if (M->s[M->pos] == '\\' && M->pos + 1 < M->n) {
            M->pos++;
        }

        c = read_cp(M);

        if (c == ' ') {
            uint32_t sg = pd_font_glyph_index(M->f, ' ');

            b.w += sg ? SC(M, pd_font_glyph_advance(M->f, sg), size) : size / 4;
            continue;
        }

        g = box_char(M, alpha(c, alphabet), size, c);
        box_place(M, &b, &g, b.w, 0);
        box_free(&g);
    }

    b.is_char = 0;
    return b;
}

/* TeX's inter-atom spacing in mu (1/18 em): <0 only in display and text styles */
static const int8_t spacing[8][8] = {
    /*         Ord Op Bin Rel Open Close Punct Inner */
    /* Ord */ { 0, 3, -4, -5, 0, 0, 0, -3 },
    /* Op  */ { 3, 3, 0, -5, 0, 0, 0, -3 },
    /* Bin */ { -4, -4, 0, 0, -4, 0, 0, -4 },
    /* Rel */ { -5, -5, 0, 0, -5, 0, 0, -5 },
    /* Open */ { 0, 0, 0, 0, 0, 0, 0, 0 },
    /* Close */ { 0, 3, -4, -5, 0, 0, 0, -3 },
    /* Punct */ { -3, -3, 0, -3, -3, -3, -3, -3 },
    /* Inner */ { -3, 3, -4, -5, -3, 0, -3, -3 }
};

/* scripts and limits on a nucleus */
static void attach_scripts(mctx* M, matom* a, mbox* sup, mbox* sub, mbox* primes, mstyle st) {
    pd_sp size = style_size(M, st.style);
    mbox* N = &a->b, r;

    box_init(&r);

    if (a->cls == OP && (a->limits == 1 || (a->limits == -1 && st.style == 0))) {
        /* limits above and below, centered */
        pd_sp W = N->w, up = 0, down = 0;

        W = sup && sup->w > W ? sup->w : W;
        W = sub && sub->w > W ? sub->w : W;
        box_place(M, &r, N, (W - N->w) / 2, 0);

        if (sup) {
            up = K(M, MC_UPPER_LIMIT_RISE, size);
            up = K(M, MC_UPPER_LIMIT_GAP, size) + sup->d > up ? K(M, MC_UPPER_LIMIT_GAP, size) + sup->d : up;
            box_place(M, &r, sup, (W - sup->w) / 2 + N->ic / 2, -(N->h + up));
        }

        if (sub) {
            down = K(M, MC_LOWER_LIMIT_DROP, size);
            down = K(M, MC_LOWER_LIMIT_GAP, size) + sub->h > down ? K(M, MC_LOWER_LIMIT_GAP, size) + sub->h : down;
            box_place(M, &r, sub, (W - sub->w) / 2 - N->ic / 2, N->d + down);
        }

        r.w = W;
    } else {
        pd_sp sup_shift = 0, sub_shift = 0, x = N->w;
        int ch = N->is_char;

        box_place(M, &r, N, 0, 0);

        if (sup) {
            sup_shift = K(M, st.cramped ? MC_SUP_UP_CRAMPED : MC_SUP_UP, size);

            if (!ch && N->h - K(M, MC_SUP_DROP_MAX, size) > sup_shift) {
                sup_shift = N->h - K(M, MC_SUP_DROP_MAX, size);
            }

            if (sup->d + K(M, MC_SUP_BOTTOM_MIN, size) > sup_shift) {
                sup_shift = sup->d + K(M, MC_SUP_BOTTOM_MIN, size);
            }
        }

        if (sub) {
            sub_shift = K(M, MC_SUB_DOWN, size);

            if (!ch && N->d + K(M, MC_SUB_DROP_MIN, size) > sub_shift) {
                sub_shift = N->d + K(M, MC_SUB_DROP_MIN, size);
            }

            if (sub->h - K(M, MC_SUB_TOP_MAX, size) > sub_shift) {
                sub_shift = sub->h - K(M, MC_SUB_TOP_MAX, size);
            }
        }

        if (sup && sub) {   /* keep a gap between them */
            pd_sp gap = (sup_shift - sup->d) - (sub->h - sub_shift), need = K(M, MC_SUBSUP_GAP, size);

            if (gap < need) {
                pd_sp lift = K(M, MC_SUP_BOTTOM_MAX_WITH_SUB, size) - (sup_shift - sup->d);

                if (lift > 0) {
                    lift = lift < need - gap ? lift : need - gap;
                    sup_shift += lift;
                    gap += lift;
                }

                sub_shift += need - gap > 0 ? need - gap : 0;
            }
        }

        /* a letter's superscript clears its slant; an integral's subscript tucks under it instead */
        pd_sp sup_x = x + (a->cls == OP ? 0 : N->ic);

        if (primes && primes->n) {  /* primes are drawn raised already: full size, on the baseline */
            box_place(M, &r, primes, sup_x, 0);
        }

        if (sup) {
            box_place(M, &r, sup, sup_x + (primes ? primes->w : 0), -sup_shift);
        }

        if (sub) {
            box_place(M, &r, sub, x - (a->ital ? N->ic : 0), sub_shift);
        }

        r.w += K(M, MC_SPACE_AFTER_SCRIPT, size);
    }

    r.is_char = 0;
    r.ic = 0;
    box_free(N);
    *N = r;
}

static mbox make_frac(mctx* M, mbox* num, mbox* den, mstyle st, int rule, uint32_t ldelim, uint32_t rdelim) {
    pd_sp size = style_size(M, st.style), axis = K(M, MC_AXIS, size), t = rule ? K(M, MC_FRAC_RULE, size) : 0;
    int disp = st.style == 0;
    pd_sp up, down, W, pad = size * 12 / 100;
    mbox r;

    box_init(&r);

    if (rule) {
        up = K(M, disp ? MC_FRAC_NUM_UP_D : MC_FRAC_NUM_UP, size);
        down = K(M, disp ? MC_FRAC_DEN_DOWN_D : MC_FRAC_DEN_DOWN, size);

        if (up - num->d < axis + t / 2 + K(M, disp ? MC_FRAC_NUM_GAP_D : MC_FRAC_NUM_GAP, size)) {
            up = axis + t / 2 + K(M, disp ? MC_FRAC_NUM_GAP_D : MC_FRAC_NUM_GAP, size) + num->d;
        }

        if ((axis - t / 2) - (den->h - down) < K(M, disp ? MC_FRAC_DEN_GAP_D : MC_FRAC_DEN_GAP, size)) {
            down = K(M, disp ? MC_FRAC_DEN_GAP_D : MC_FRAC_DEN_GAP, size) + den->h - axis + t / 2;
        }
    } else {    /* a stack (\binom): no rule */
        pd_sp gap, need = K(M, disp ? MC_STACK_GAP_D : MC_STACK_GAP, size);

        up = K(M, disp ? MC_STACK_TOP_UP_D : MC_STACK_TOP_UP, size);
        down = K(M, disp ? MC_STACK_BOTTOM_DOWN_D : MC_STACK_BOTTOM_DOWN, size);
        gap = (up - num->d) - (den->h - down);

        if (gap < need) {
            up += (need - gap) / 2;
            down += need - gap - (need - gap) / 2;
        }
    }

    W = num->w > den->w ? num->w : den->w;
    box_place(M, &r, num, pad + (W - num->w) / 2, -up);
    box_place(M, &r, den, pad + (W - den->w) / 2, down);

    if (rule) {
        box_rule(M, &r, pad, -(axis + t / 2), W, t);
    }

    r.w = W + 2 * pad;

    if (ldelim || rdelim) {     /* \binom: parentheses around */
        pd_sp delta = r.h - axis > r.d + axis ? r.h - axis : r.d + axis, target = 2 * delta;
        mbox L = delimiter(M, ldelim, target, size), R = delimiter(M, rdelim, target, size), all;

        box_init(&all);
        box_place(M, &all, &L, 0, 0);
        box_place(M, &all, &r, L.w, 0);
        box_place(M, &all, &R, L.w + r.w, 0);
        box_free(&L);
        box_free(&R);
        box_free(&r);
        r = all;
    }

    return r;
}

static mbox make_sqrt(mctx* M, mbox* body, mbox* degree, mstyle st) {
    pd_sp size = style_size(M, st.style), t = K(M, MC_RADICAL_RULE, size);
    pd_sp gap = K(M, st.style == 0 ? MC_RADICAL_GAP_D : MC_RADICAL_GAP, size), need, top;
    uint32_t g = pd_font_glyph_index(M->f, 0x221A);
    mbox rad, r;
    pd_sp x = 0;

    box_init(&r);
    need = body->h + body->d + gap + t;
    rad = g ? stretchy(M, g, need, size) : box_char(M, 0x221A, size, 'V');

    if (rad.h + rad.d > need) {     /* the extra room goes half into the gap */
        gap += (rad.h + rad.d - need) / 2;
    }

    top = body->h + gap + t;    /* the radical's top meets the rule's top */

    if (degree && degree->n) {      /* the root's degree, raised, kerned into the sign */
        int32_t raise_pct = M->C ? S16(M->f, (uint64_t)M->C + 8 + 4 * MC_COUNT) : 60;
        pd_sp kb = K(M, MC_RADICAL_KERN_BEFORE, size), ka = K(M, MC_RADICAL_KERN_AFTER, size);
        pd_sp raise = (pd_sp)((int64_t)(rad.h + rad.d) * raise_pct / 100) - (rad.h + rad.d - top);

        box_place(M, &r, degree, kb, -(raise + degree->d));
        x = kb + degree->w + ka;
        x = x < 0 ? 0 : x;
    }

    box_place(M, &r, &rad, x, -(top - rad.h));
    box_rule(M, &r, x + rad.w, -top, body->w, t);
    box_place(M, &r, body, x + rad.w, 0);
    r.h = top + K(M, MC_RADICAL_EXTRA, size) > r.h ? top + K(M, MC_RADICAL_EXTRA, size) : r.h;
    box_free(&rad);
    return r;
}

static mbox make_accent(mctx* M, mbox* base, uint32_t acc_cp, mstyle st, int wide) {
    pd_sp size = style_size(M, st.style), raise, ax, bx;
    uint32_t g = pd_font_glyph_index(M->f, acc_cp), c;
    mbox acc, r;

    box_init(&r);

    if (!g) {
        return *base;
    }

    /* a wider variant for wide bases (\widehat) */
    if (wide && (c = construction(M, g, 1)) != 0) {
        int32_t i, nvar = (int32_t)U16(M->f, c + 2);

        for (i = 0; i < nvar; i++) {
            uint32_t vg = U16(M->f, (uint64_t)c + 4 + 4 * i);
            pd_sp adv = SC(M, (int32_t)U16(M->f, (uint64_t)c + 6 + 4 * i), size);

            g = vg;

            if (adv >= base->w) {
                break;
            }
        }
    }

    acc = box_glyph(M, g, size);
    raise = base->h - K(M, MC_ACCENT_BASE, size);
    raise = raise > 0 ? raise : 0;
    bx = base->accent_x >= 0 && base->is_char ? base->accent_x : base->w / 2;

    if (acc.accent_x >= 0) {
        ax = acc.accent_x;
    } else {    /* the middle of its ink (combining accents are drawn left of their origin) */
        int32_t x0, x1, y0, y1;

        glyph_bbox_x(M->f, g, &x0, &x1, &y0, &y1);
        ax = SC(M, (x0 + x1) / 2, size);
    }

    box_place(M, &r, base, 0, 0);
    box_place(M, &r, &acc, bx - ax, -raise);
    r.w = base->w;
    r.ic = base->ic;
    box_free(&acc);
    return r;
}

static mbox make_bar(mctx* M, mbox* base, int over, mstyle st) {
    pd_sp size = style_size(M, st.style);
    pd_sp t = K(M, over ? MC_OVERBAR_RULE : MC_UNDERBAR_RULE, size);
    pd_sp gap = K(M, over ? MC_OVERBAR_GAP : MC_UNDERBAR_GAP, size);
    mbox r;

    box_init(&r);
    box_place(M, &r, base, 0, 0);

    if (over) {
        box_rule(M, &r, 0, -(base->h + gap + t), base->w, t);
        r.h += K(M, MC_OVERBAR_EXTRA, size);
    } else {
        box_rule(M, &r, 0, base->d + gap, base->w, t);
        r.d += K(M, MC_UNDERBAR_EXTRA, size);
    }

    return r;
}

/* \begin{matrix} ... \end{matrix} and friends */
static mbox make_matrix(mctx* M, mstyle st, const char* env) {
    pd_sp size = style_size(M, st.style), axis = K(M, MC_AXIS, size);
    mbox cells[64][16];
    int nrows = 0, ncols[64], r, c, ended = END_EOF, maxc = 0, cases = strcmp(env, "cases") == 0, env_done = 0;
    pd_sp colw[16], rowh[64], rowd[64], y, total_h, sep = cases ? size : size * 8 / 10;
    mbox body;
    mstyle cs = st;

    cs.style = st.style == 0 ? 1 : st.style;
    memset(ncols, 0, sizeof(ncols));
    memset(colw, 0, sizeof(colw));
    box_init(&body);

    while (nrows < 64) {
        c = 0;

        for (;;) {
            mbox cell = parse_list(M, cs, &ended);

            if (c < 16) {
                cells[nrows][c++] = cell;
            } else {
                box_free(&cell);
            }

            if (ended != END_AMP) {
                break;
            }
        }

        ncols[nrows++] = c;
        maxc = c > maxc ? c : maxc;

        if (ended != END_ROW) {
            break;
        }

        skip_space(M);

        if (M->pos + 4 <= M->n && memcmp(M->s + M->pos, "\\end", 4) == 0) {
            char name[16];

            mbox tmp;

            read_command(M, name, sizeof(name));
            tmp = parse_text(M, st, AL_ROMAN);    /* {env} */
            box_free(&tmp);
            ended = END_END;
            env_done = 1;
            break;
        }
    }

    if (ended == END_END && !env_done) {
        /* \end{...} consumed by parse_list: its argument follows */
        skip_space(M);

        if (M->pos < M->n && M->s[M->pos] == '{') {
            mbox tmp = parse_text(M, st, AL_ROMAN);

            box_free(&tmp);
        }
    }

    for (r = 0; r < nrows; r++) {
        rowh[r] = size * 7 / 10;    /* a strut */
        rowd[r] = size * 3 / 10;

        for (c = 0; c < ncols[r]; c++) {
            colw[c] = cells[r][c].w > colw[c] ? cells[r][c].w : colw[c];
            rowh[r] = cells[r][c].h > rowh[r] ? cells[r][c].h : rowh[r];
            rowd[r] = cells[r][c].d > rowd[r] ? cells[r][c].d : rowd[r];
        }
    }

    for (r = 0, y = 0; r < nrows; r++) {
        pd_sp x = 0;

        y += rowh[r];

        for (c = 0; c < maxc; c++) {
            if (c < ncols[r]) {
                pd_sp off = cases ? 0 : (colw[c] - cells[r][c].w) / 2;

                box_place(M, &body, &cells[r][c], x + off, y);
                box_free(&cells[r][c]);
            }

            x += colw[c] + (c + 1 < maxc ? sep : 0);
        }

        body.w = x > body.w ? x : body.w;
        y += rowd[r] + (r + 1 < nrows ? size * 2 / 10 : 0);
    }

    /* the whole array centered on the axis */
    total_h = y;
    {
        mbox centered;

        box_init(&centered);
        body.h = 0;
        body.d = total_h;
        box_place(M, &centered, &body, 0, -(total_h / 2 + axis));
        box_free(&body);
        body = centered;
    }

    {
        uint32_t l = 0, rr = 0;

        if (!strcmp(env, "pmatrix")) {
            l = '(';
            rr = ')';
        } else if (!strcmp(env, "bmatrix")) {
            l = '[';
            rr = ']';
        } else if (!strcmp(env, "Bmatrix") || cases) {
            l = '{';
            rr = cases ? '.' : '}';
        } else if (!strcmp(env, "vmatrix")) {
            l = rr = '|';
        } else if (!strcmp(env, "Vmatrix")) {
            l = rr = 0x2016;
        }

        if (l) {
            pd_sp delta = body.h - axis > body.d + axis ? body.h - axis : body.d + axis;
            mbox L = delimiter(M, l, 2 * delta, size), R = delimiter(M, rr, 2 * delta, size), all;
            pd_sp pad = size / 10;

            box_init(&all);
            box_place(M, &all, &L, 0, 0);
            box_place(M, &all, &body, L.w + pad, 0);
            box_place(M, &all, &R, L.w + body.w + 2 * pad, 0);
            box_free(&L);
            box_free(&R);
            box_free(&body);
            body = all;
        }
    }

    body.is_char = 0;
    return body;
}

/* the delimiter named after \left, \right, \big..: '.' is none */
static uint32_t read_delim(mctx* M) {
    char name[64];
    int i;

    skip_space(M);

    if (M->pos >= M->n) {
        return '.';
    }

    if (M->s[M->pos] == '\\') {
        read_command(M, name, sizeof(name));

        if (!strcmp(name, "{")) {
            return '{';
        }

        if (!strcmp(name, "}")) {
            return '}';
        }

        if (!strcmp(name, "|")) {
            return 0x2016;
        }

        for (i = 0; i < (int)(sizeof(symbols) / sizeof(symbols[0])); i++) {
            if (!strcmp(symbols[i].name, name)) {
                return symbols[i].cp;
            }
        }

        return '.';
    }

    return read_cp(M);
}

/* the atoms of a list until its end, then spacing and Bin fixes */
static mbox parse_list(mctx* M, mstyle st, int* ended) {
    matom* atoms = NULL;
    int32_t na = 0, capa = 0, i;
    pd_sp size = style_size(M, st.style);
    mbox out;

    *ended = END_EOF;
    box_init(&out);

    if (++M->depth > 48) {
        M->depth--;
        M->pos = M->n;
        return out;
    }

    while (M->pos < M->n) {
        matom a;
        char name[64];
        uint32_t c;

        memset(&a, 0, sizeof(a));
        box_init(&a.b);
        a.cls = NOATOM;
        a.limits = -1;
        skip_space(M);

        if (M->pos >= M->n) {
            break;
        }

        c = (unsigned char)M->s[M->pos];

        if (c == '}') {
            M->pos++;
            *ended = END_BRACE;
            break;
        }

        if (c == '&') {
            M->pos++;
            *ended = END_AMP;
            break;
        }

        if (c == '^' || c == '_' || c == '\'') {     /* scripts and primes on the previous atom (or on nothing) */
            mbox s1, *sup = NULL, *sub = NULL, s2, pr;

            if (na == 0 || atoms[na - 1].cls == NOATOM) {
                matom e;

                memset(&e, 0, sizeof(e));
                box_init(&e.b);
                e.cls = ORD;
                e.limits = -1;

                if (!pd_grow((void**)&atoms, &capa, (int64_t)na + 1, sizeof(matom))) {
                    atoms[na++] = e;
                }
            }

            if (na == 0) {
                break;
            }

            box_init(&s1);
            box_init(&s2);
            box_init(&pr);

            /* both scripts of the atom, in either order, and its primes */
            while (M->pos < M->n && (M->s[M->pos] == '^' || M->s[M->pos] == '_' || M->s[M->pos] == '\'')) {
                int is_sup = M->s[M->pos] == '^';

                if (M->s[M->pos] == '\'') {
                    mbox g = box_char(M, 0x2032, size, '\'');

                    if (pr.n < 4) {
                        box_place(M, &pr, &g, pr.w, 0);
                    }

                    box_free(&g);
                    M->pos++;
                    skip_space(M);
                    continue;
                }

                M->pos++;

                if (is_sup && !sup) {
                    s1 = parse_arg(M, sup_style(st));
                    sup = &s1;
                } else if (!is_sup && !sub) {
                    s2 = parse_arg(M, sub_style(st));
                    sub = &s2;
                } else {
                    mbox junk = parse_arg(M, sub_style(st));

                    box_free(&junk);
                }

                skip_space(M);

                if (M->pos + 1 < M->n && M->s[M->pos] == '\\' && (strncmp(M->s + M->pos, "\\limits", 7) == 0 ||
                        strncmp(M->s + M->pos, "\\nolimits", 9) == 0)) {
                    break;
                }
            }

            attach_scripts(M, &atoms[na - 1], sup, sub, &pr, st);

            if (sup) {
                box_free(&s1);
            }

            if (sub) {
                box_free(&s2);
            }

            box_free(&pr);
            continue;
        }

        if (c == '{') {
            int e2;

            M->pos++;
            a.b = parse_list(M, st, &e2);
            a.cls = ORD;
        } else if (c == '\\') {
            size_t save = M->pos;

            read_command(M, name, sizeof(name));

            if (!strcmp(name, "\\")) {
                *ended = END_ROW;
                break;
            }

            if (!strcmp(name, "right")) {
                M->pos = save;
                *ended = END_RIGHT;
                break;
            }

            if (!strcmp(name, "end")) {
                *ended = END_END;
                break;
            }

            if (!strcmp(name, "frac") || !strcmp(name, "dfrac") || !strcmp(name, "tfrac") || !strcmp(name, "binom") ||
                    !strcmp(name, "cfrac")) {
                mstyle fs = st;
                mbox num, den;

                if (name[0] == 'd' || name[0] == 'c') {
                    fs.style = 0;
                } else if (name[0] == 't') {
                    fs.style = fs.style < 1 ? 1 : fs.style;
                }

                num = parse_arg(M, frac_num_style(fs));
                den = parse_arg(M, frac_den_style(fs));
                a.b = make_frac(M, &num, &den, fs, name[0] != 'b', name[0] == 'b' ? '(' : 0, name[0] == 'b' ? ')' : 0);
                a.cls = name[0] == 'b' ? ORD : INNER;
                box_free(&num);
                box_free(&den);
            } else if (!strcmp(name, "sqrt")) {
                mbox deg, body;
                mstyle cs = st;

                box_init(&deg);
                skip_space(M);

                if (M->pos < M->n && M->s[M->pos] == '[') {     /* [degree] */
                    size_t e = M->pos + 1;
                    int dep = 0;
                    const char* ss = M->s;
                    size_t sn = M->n;

                    while (e < M->n && !(M->s[e] == ']' && dep == 0)) {
                        dep += M->s[e] == '{' ? 1 : M->s[e] == '}' ? -1 : 0;
                        e++;
                    }

                    {
                        int e2;
                        mstyle ds = st;

                        ds.style = 3;
                        M->s = ss + M->pos + 1;
                        M->n = e - M->pos - 1;
                        M->pos = 0;
                        deg = parse_list(M, ds, &e2);
                        M->s = ss;
                        M->n = sn;
                        M->pos = e < sn ? e + 1 : sn;
                    }
                }

                cs.cramped = 1;
                body = parse_arg(M, cs);
                a.b = make_sqrt(M, &body, &deg, st);
                a.cls = ORD;
                box_free(&body);
                box_free(&deg);
            } else if (!strcmp(name, "left")) {
                uint32_t l = read_delim(M), r = '.';
                int e2;
                mbox body = parse_list(M, st, &e2);
                pd_sp axis = K(M, MC_AXIS, size), delta, target;
                mbox L, R;

                if (e2 == END_RIGHT) {
                    read_command(M, name, sizeof(name));
                    r = read_delim(M);
                }

                delta = body.h - axis > body.d + axis ? body.h - axis : body.d + axis;
                target = (pd_sp)((int64_t)2 * delta * 901 / 1000);
                target = target < 2 * delta - size / 2 ? 2 * delta - size / 2 : target;
                L = delimiter(M, l, target, size);
                R = delimiter(M, r, target, size);
                box_place(M, &a.b, &L, 0, 0);
                box_place(M, &a.b, &body, L.w, 0);
                box_place(M, &a.b, &R, L.w + body.w, 0);
                a.cls = INNER;
                box_free(&L);
                box_free(&R);
                box_free(&body);
            } else if (!strncmp(name, "big", 3) || !strncmp(name, "Big", 3)) {
                static const int pct[4] = { 120, 180, 240, 300 };
                int k = (name[0] == 'B' ? 1 : 0) + (!strncmp(name + 3, "g", 1) ? 2 : 0);
                char last = name[strlen(name) - 1];
                uint32_t d = read_delim(M);

                a.b = delimiter(M, d, (pd_sp)((int64_t)size * pct[k] / 100), size);
                a.cls = last == 'l' ? OPEN : last == 'r' ? CLOSE : last == 'm' ? REL : ORD;
            } else if (!strcmp(name, "text") || !strcmp(name, "textrm") || !strcmp(name, "mbox") ||
                       !strcmp(name, "textit") || !strcmp(name, "textbf")) {
                a.b = parse_text(M, st, !strcmp(name, "textbf") ? AL_BOLD : !strcmp(name, "textit") ? AL_ITALIC : AL_ROMAN);
                a.cls = ORD;
            } else if (!strcmp(name, "mathrm") || !strcmp(name, "mathbf") || !strcmp(name, "mathbb") ||
                       !strcmp(name, "mathcal") || !strcmp(name, "mathit") || !strcmp(name, "mathsf") ||
                       !strcmp(name, "mathtt") || !strcmp(name, "mathfrak") || !strcmp(name, "boldsymbol") ||
                       !strcmp(name, "operatorname")) {
                mstyle as = st;
                int e2;

                as.alphabet = !strcmp(name, "mathbf") ? AL_BOLD : !strcmp(name, "mathbb") ? AL_BB :
                              !strcmp(name, "mathcal") ? AL_CAL : !strcmp(name, "mathit") ? AL_ITALIC :
                              !strcmp(name, "mathsf") ? AL_SANS : !strcmp(name, "mathtt") ? AL_TT :
                              !strcmp(name, "mathfrak") ? AL_FRAK : !strcmp(name, "boldsymbol") ? AL_BOLDITALIC : AL_ROMAN;
                skip_space(M);

                if (M->pos < M->n && M->s[M->pos] == '{') {
                    M->pos++;
                    as.alphabet += 100;     /* (marks an explicit alphabet for letters) */
                    a.b = parse_list(M, as, &e2);
                } else {
                    as.alphabet += 100;
                    a.b = parse_arg(M, as);
                }

                a.cls = !strcmp(name, "operatorname") ? OP : ORD;
                a.limits = 0;
            } else if (!strcmp(name, "begin")) {
                mbox envname = parse_text(M, st, AL_ROMAN);
                char env[16] = "matrix";
                size_t k = 0, p = M->pos;

                (void)p;
                box_free(&envname);
                /* the name: re-read the raw text between the braces we just passed */
                {
                    size_t e = M->pos, s0;

                    while (e > 0 && M->s[e - 1] != '{') {
                        e--;
                    }

                    s0 = e;

                    while (s0 < M->n && M->s[s0] != '}' && k + 1 < sizeof(env)) {
                        env[k++] = M->s[s0++];
                    }

                    env[k] = '\0';
                }

                a.b = make_matrix(M, st, env);
                a.cls = !strcmp(env, "cases") ? INNER : ORD;
            } else if (!strcmp(name, "overline") || !strcmp(name, "underline")) {
                mstyle bs = st;
                mbox base;

                bs.cramped = name[0] == 'o' ? 1 : bs.cramped;
                base = parse_arg(M, bs);
                a.b = make_bar(M, &base, name[0] == 'o', st);
                a.cls = ORD;
                box_free(&base);
            } else if (!strcmp(name, "limits") || !strcmp(name, "nolimits")) {
                if (na > 0 && atoms[na - 1].cls == OP) {
                    atoms[na - 1].limits = name[0] == 'l';
                }

                continue;
            } else if (!strcmp(name, ",") || !strcmp(name, ":") || !strcmp(name, ">") || !strcmp(name, ";") ||
                       !strcmp(name, "!") || !strcmp(name, "quad") || !strcmp(name, "qquad") || !strcmp(name, " ") ||
                       !strcmp(name, "enspace") || !strcmp(name, "thinspace")) {
                pd_sp mu = size / 18;
                pd_sp w = name[0] == ',' || !strcmp(name, "thinspace") ? 3 * mu : name[0] == ':' || name[0] == '>' ? 4 * mu :
                          name[0] == ';' ? 5 * mu : name[0] == '!' ? -3 * mu : !strcmp(name, "quad") ? size :
                          !strcmp(name, "qquad") ? 2 * size : !strcmp(name, "enspace") ? size / 2 : size / 4;

                a.b.w = w;
                a.cls = NOATOM;     /* glue: no spacing of its own */
            } else if (!strcmp(name, "{") || !strcmp(name, "}") || !strcmp(name, "|") || !strcmp(name, "#") ||
                       !strcmp(name, "%") || !strcmp(name, "&") || !strcmp(name, "$") || !strcmp(name, "_")) {
                uint32_t cp = name[0] == '|' ? 0x2016 : (uint32_t)name[0];

                a.b = box_char(M, cp, size, 0);
                a.cls = name[0] == '{' ? OPEN : name[0] == '}' ? CLOSE : ORD;
            } else {
                int k, found = 0;

                for (k = 0; k < (int)(sizeof(bigops) / sizeof(bigops[0])) && !found; k++) {
                    if (!strcmp(bigops[k].name, name)) {
                        uint32_t g = pd_font_glyph_index(M->f, bigops[k].cp);
                        pd_sp axis = K(M, MC_AXIS, size);
                        mbox op;

                        if (st.style == 0 && g) {   /* display: a larger variant */
                            pd_sp min = SC(M, M->C ? (int32_t)U16(M->f, M->C + 6) : 1300, size);
                            op = stretchy(M, g, min, size);
                        } else {
                            op = box_char(M, bigops[k].cp, size, 0);
                        }

                        /* centered on the axis */
                        box_place(M, &a.b, &op, 0, (op.h - op.d) / 2 - axis);
                        a.b.ic = op.ic;
                        a.b.is_char = 0;
                        box_free(&op);
                        a.cls = OP;
                        a.limits = bigops[k].limits ? -1 : 0;
                        a.ital = bigops[k].integral;
                        found = 1;
                    }
                }

                for (k = 0; k < (int)(sizeof(named_ops) / sizeof(named_ops[0])) && !found; k++) {
                    if (!strcmp(named_ops[k].name, name)) {
                        const char* t = name;

                        while (*t) {
                            mbox g = box_char(M, (uint32_t)(unsigned char) * t++, size, 0);

                            box_place(M, &a.b, &g, a.b.w, 0);
                            box_free(&g);
                        }

                        a.b.is_char = 0;
                        a.cls = OP;
                        a.limits = named_ops[k].limits ? -1 : 0;
                        a.big = 2;
                        found = 1;
                    }
                }

                for (k = 0; k < (int)(sizeof(accents) / sizeof(accents[0])) && !found; k++) {
                    if (!strcmp(accents[k].name, name)) {
                        mstyle bs = st;
                        mbox base;

                        bs.cramped = 1;
                        base = parse_arg(M, bs);
                        a.b = make_accent(M, &base, accents[k].cp, st, !strncmp(name, "wide", 4));

                        if (a.b.v != base.v) {
                            box_free(&base);
                        }

                        a.cls = ORD;
                        found = 1;
                    }
                }

                for (k = 0; k < (int)(sizeof(symbols) / sizeof(symbols[0])) && !found; k++) {
                    if (!strcmp(symbols[k].name, name)) {
                        uint32_t cp = symbols[k].cp;

                        if (cp >= 0x1D6FC && cp <= 0x1D71B && st.alphabet >= 100 && st.alphabet != 100 + AL_ITALIC) {
                            /* upright Greek inside \mathrm etc. */
                            static const uint16_t up[] = { 0x3B1, 0x3B2, 0x3B3, 0x3B4, 0x3B5, 0x3B6, 0x3B7, 0x3B8, 0x3B9,
                                                           0x3BA, 0x3BB, 0x3BC, 0x3BD, 0x3BE, 0x3BF, 0x3C0, 0x3C1, 0x3C2,
                                                           0x3C3, 0x3C4, 0x3C5, 0x3C6, 0x3C7, 0x3C8, 0x3C9
                                                         };

                            if (cp - 0x1D6FC < sizeof(up) / sizeof(up[0])) {
                                cp = up[cp - 0x1D6FC];
                            }
                        }

                        a.b = box_char(M, cp, size, 0);
                        a.cls = symbols[k].cls;
                        found = 1;
                    }
                }

                if (!found) {   /* unknown: its name, upright */
                    const char* t = name;

                    while (*t) {
                        mbox g = box_char(M, (uint32_t)(unsigned char) * t++, size, 0);

                        box_place(M, &a.b, &g, a.b.w, 0);
                        box_free(&g);
                    }

                    a.cls = ORD;
                }
            }
        } else {
            int al = st.alphabet >= 100 ? st.alphabet - 100 : AL_ITALIC;

            c = read_cp(M);

            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                a.b = box_char(M, alpha(c, al), size, c);
                a.cls = ORD;
            } else if (c >= '0' && c <= '9') {
                a.b = box_char(M, al == AL_BOLD || al == AL_BB || al == AL_SANS || al == AL_TT ? alpha(c, al) : c, size, c);
                a.cls = ORD;
            } else {
                uint32_t cp = c;

                a.cls = ORD;

                switch (c) {
                    case '+':
                        a.cls = BIN;
                        break;

                    case '-':
                        cp = 0x2212;
                        a.cls = BIN;
                        break;

                    case '*':
                        cp = 0x2217;
                        a.cls = BIN;
                        break;

                    case '=':
                    case '<':
                    case '>':
                    case ':':
                        a.cls = REL;
                        break;

                    case ',':
                    case ';':
                        a.cls = PUNCT;
                        break;

                    case '(':
                    case '[':
                        a.cls = OPEN;
                        break;

                    case ')':
                    case ']':
                    case '!':
                    case '?':
                        a.cls = CLOSE;
                        break;

                    case '~':
                        a.b.w = size / 4;
                        a.cls = NOATOM;
                        break;
                }

                if (a.cls != NOATOM || c != '~') {
                    a.b = box_char(M, cp, size, c);
                }
            }
        }

        if (pd_grow((void**)&atoms, &capa, (int64_t)na + 1, sizeof(matom))) {
            box_free(&a.b);
            break;
        }

        atoms[na++] = a;
    }

    /* Bin becomes Ord where it cannot be binary (TeX rules 5 and 6) */
    for (i = 0; i < na; i++) {
        if (atoms[i].cls == BIN) {
            int prev = NOATOM;
            int32_t k;

            for (k = i - 1; k >= 0 && atoms[k].cls == NOATOM; k--) {
            }

            prev = k >= 0 ? atoms[k].cls : NOATOM;

            if (prev == NOATOM || prev == BIN || prev == OP || prev == REL || prev == OPEN || prev == PUNCT) {
                atoms[i].cls = ORD;
            }
        }

        if (atoms[i].cls == REL || atoms[i].cls == CLOSE || atoms[i].cls == PUNCT) {
            int32_t k;

            for (k = i - 1; k >= 0 && atoms[k].cls == NOATOM; k--) {
            }

            if (k >= 0 && atoms[k].cls == BIN) {
                atoms[k].cls = ORD;
            }
        }
    }

    if (na > 0 && atoms[na - 1].cls == BIN) {
        atoms[na - 1].cls = ORD;
    }

    /* the list with TeX's spacing between atoms */
    {
        int prev = NOATOM;
        pd_sp mu = size / 18;

        for (i = 0; i < na; i++) {
            matom* a = &atoms[i];

            if (a->cls != NOATOM && prev != NOATOM) {
                int sp = spacing[prev][a->cls];

                if (sp < 0) {
                    sp = st.style <= 1 ? -sp : 0;
                }

                out.w += sp * mu;
            }

            if (a->cls == NOATOM) {
                out.w += a->b.w;    /* explicit space */
                box_free(&a->b);
                continue;
            }

            box_place(M, &out, &a->b, out.w, 0);
            out.ic = a->b.ic;
            out.accent_x = na == 1 ? a->b.accent_x : -1;
            out.is_char = na == 1 && a->b.is_char;
            out.glyph = a->b.glyph;
            prev = a->cls;
            box_free(&a->b);
        }
    }

    free(atoms);
    M->depth--;
    return out;
}

pd_status pd_math_layout(const pd_font* f, pd_sp size, const char* tex, size_t len, int32_t display,
                         pd_math_item* items, int32_t cap, int32_t* count, pd_math_metrics* m) {
    mctx M;
    mstyle st;
    mbox b;
    int ended, k;
    pd_status rc = PD_OK;

    if (!f || !count || (!tex && len) || size <= 0 || cap < 0) {
        return PD_ERR_ARG;
    }

    memset(&M, 0, sizeof(M));
    M.f = f;
    M.upem = f->m.units_per_em > 0 ? f->m.units_per_em : 1000;
    M.size = size;
    M.s = tex ? tex : "";
    M.n = len;

    for (k = 0; k < MC_COUNT; k++) {
        M.fallback[k] = (int16_t)((int32_t)lm_constants[k] * M.upem / 1000);
    }

    if (pd_font_has_math(f)) {
        uint32_t base = f->math;

        M.C = U16(f, base + 4) ? base + U16(f, base + 4) : 0;
        M.GI = U16(f, base + 6) ? base + U16(f, base + 6) : 0;
        M.V = U16(f, base + 8) ? base + U16(f, base + 8) : 0;
    }

    memset(&st, 0, sizeof(st));
    st.style = display ? 0 : 1;
    b = parse_list(&M, st, &ended);

    /* a stray closer at the top level just ends the list: go on with the rest */
    while (M.pos < M.n && ended != END_EOF) {
        mbox more;

        if (ended == END_RIGHT) {   /* a \right without its \left: dropped */
            char name[16];

            read_command(&M, name, sizeof(name));
            read_delim(&M);
        }

        more = parse_list(&M, st, &ended);

        box_place(&M, &b, &more, b.w, 0);
        box_free(&more);
    }

    if (b.err) {
        box_free(&b);
        return PD_ERR_NOMEM;
    }

    *count = b.n;

    if (m) {
        m->width = b.w;
        m->height = b.h;
        m->depth = b.d;
    }

    if (items) {
        if (cap < b.n) {
            rc = PD_ERR_RANGE;
        } else if (b.n) {
            memcpy(items, b.v, (size_t)b.n * sizeof(pd_math_item));
        }
    }

    box_free(&b);
    return rc;
}
