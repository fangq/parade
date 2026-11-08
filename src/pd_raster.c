/*
 * Parade glyph outlines and rasterizer
 *
 * Outlines come from the TrueType glyf table (simple and composite
 * glyphs) or CFF charstrings (pd_cff.c). The rasterizer fills them with
 * exact-area antialiasing in integer arithmetic only (the cell/cover
 * scheme of FreeType's smooth rasterizer), so the same glyph gives the
 * same pixels on every platform, like the rest of Parade.
 *
 * Coordinates: outlines are delivered in font units x 64 (26.6), y up.
 * Inside the rasterizer positions are in 1/256 pixel ("subpixels").
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "parade_layout.h"

#define ONE 256                 /* subpixels per pixel */
/* bound for accumulated glyf deltas: real outlines stay within int16, so 26.6 values fit int32 */
#define CLAMP_COORD(v) ((v) > (1 << 24) ? (1 << 24) : (v) < -(1 << 24) ? -(1 << 24) : (v))
#define MAX_COMPOSITE_DEPTH 8

static uint32_t RU16(const pd_font* f, uint64_t off) {
    return off + 2 <= f->len ? ((uint32_t)f->data[off] << 8) | f->data[off + 1] : 0;
}

static int32_t RS16(const pd_font* f, uint64_t off) {
    return (int16_t)RU16(f, off);
}

static uint32_t RU32(const pd_font* f, uint64_t off) {
    return off + 4 <= f->len ? ((uint32_t)f->data[off] << 24) | ((uint32_t)f->data[off + 1] << 16) |
           ((uint32_t)f->data[off + 2] << 8) | f->data[off + 3] : 0;
}

/* ------------------------------------------------------------------ */
/* outline collection                                                 */
/* ------------------------------------------------------------------ */

enum {
    OP_MOVE = 0,
    OP_LINE = 1,
    OP_QUAD = 2,
    OP_CUBIC = 3
};

typedef struct {
    int32_t op;
    int32_t x[3], y[3];         /* 26.6 font units; the end point is the last used entry */
} seg;

typedef struct {
    seg* s;
    int32_t n, cap;
    int err;
} path_t;

static void path_add(path_t* p, int32_t op, const int32_t* xs, const int32_t* ys, int npts) {
    int k;

    if (pd_grow((void**)&p->s, &p->cap, (int64_t)p->n + 1, sizeof(seg))) {
        p->err = 1;
        return;
    }

    memset(&p->s[p->n], 0, sizeof(seg));
    p->s[p->n].op = op;

    for (k = 0; k < npts; k++) {
        p->s[p->n].x[k] = xs[k];
        p->s[p->n].y[k] = ys[k];
    }

    p->n++;
}

/* one TrueType contour (on/off points with implied on-points) as segments */
static void tt_contour(path_t* p, const int32_t* px, const int32_t* py, const uint8_t* on, int32_t n) {
    int32_t i, start = 0;
    int32_t sx, sy;

    if (n <= 0) {
        return;
    }

    /* start on an on-curve point, or the midpoint of two off-curve points */
    if (on[0]) {
        sx = px[0];
        sy = py[0];
        start = 1;
    } else if (on[n - 1]) {
        sx = px[n - 1];
        sy = py[n - 1];
        start = 0;
        n--;
    } else {
        sx = (px[0] + px[n - 1]) / 2;
        sy = (py[0] + py[n - 1]) / 2;
        start = 0;
    }

    path_add(p, OP_MOVE, &sx, &sy, 1);

    {
        int32_t cx = 0, cy = 0, have_ctrl = 0;

        for (i = start; i <= n - 1 + start; i++) {
            int32_t k = i % (n ? n : 1), x = px[k], y = py[k];

            if (i == n - 1 + start + 1) {
                break;
            }

            if (on[k]) {
                if (have_ctrl) {
                    int32_t xs[2] = { cx, x }, ys[2] = { cy, y };
                    path_add(p, OP_QUAD, xs, ys, 2);
                    have_ctrl = 0;
                } else {
                    path_add(p, OP_LINE, &x, &y, 1);
                }
            } else {
                if (have_ctrl) {    /* implied on-point between two controls */
                    int32_t mx = (cx + x) / 2, my = (cy + y) / 2;
                    int32_t xs[2] = { cx, mx }, ys[2] = { cy, my };
                    path_add(p, OP_QUAD, xs, ys, 2);
                }

                cx = x;
                cy = y;
                have_ctrl = 1;
            }
        }

        /* close back to the start */
        if (have_ctrl) {
            int32_t xs[2] = { cx, sx }, ys[2] = { cy, sy };
            path_add(p, OP_QUAD, xs, ys, 2);
        } else {
            path_add(p, OP_LINE, &sx, &sy, 1);
        }
    }
}

/* glyph data range in glyf, 0 length = empty glyph */
static int glyf_range(const pd_font* f, uint32_t g, uint32_t* off, uint32_t* len) {
    uint32_t a, b;

    if ((int32_t)g >= f->m.num_glyphs) {
        return -1;
    }

    if (f->loca_long) {
        a = RU32(f, (uint64_t)f->loca + 4 * g);
        b = RU32(f, (uint64_t)f->loca + 4 * g + 4);
    } else {
        a = 2 * RU16(f, (uint64_t)f->loca + 2 * g);
        b = 2 * RU16(f, (uint64_t)f->loca + 2 * g + 2);
    }

    if (b < a || (uint64_t)b > f->glyf_len) {
        return -1;
    }

    *off = f->glyf + a;
    *len = b - a;
    return 0;
}

/* xx, yx, xy, yy in 2.14; dx, dy in 26.6 */
typedef struct {
    int32_t a, b, c, d, dx, dy;
} xform;

/* composite transforms of a malformed font can grow without bound: saturate */
static int32_t sat(int64_t v) {
    return v > (1 << 29) ? (1 << 29) : v < -(1 << 29) ? -(1 << 29) : (int32_t)v;     /* midpoints stay in int32 */
}

static void apply(const xform* t, int32_t x, int32_t y, int32_t* ox, int32_t* oy) {
    *ox = sat((((int64_t)t->a * x + (int64_t)t->c * y) >> 14) + t->dx);
    *oy = sat((((int64_t)t->b * x + (int64_t)t->d * y) >> 14) + t->dy);
}

static int tt_glyph(const pd_font* f, uint32_t g, const xform* t, path_t* p, int depth) {
    uint32_t off, len;
    int32_t ncont;

    if (depth > MAX_COMPOSITE_DEPTH || glyf_range(f, g, &off, &len)) {
        return -1;
    }

    if (len < 10) {
        return 0;   /* empty glyph (space) */
    }

    ncont = RS16(f, off);

    if (ncont >= 0) {
        uint64_t q = (uint64_t)off + 10, end = (uint64_t)off + len, xq, yq;
        int32_t npts, i, k, *px, *py, x = 0, y = 0, c0 = 0;
        uint8_t* fl;
        uint32_t ninst;

        if (ncont == 0) {
            return 0;
        }

        npts = (int32_t)RU16(f, q + 2 * (uint64_t)(ncont - 1)) + 1;

        if (npts <= 0 || (uint64_t)npts > len * 2 + 4) {
            return -1;
        }

        ninst = RU16(f, q + 2 * (uint64_t)ncont);
        q += 2 * (uint64_t)ncont + 2 + ninst;
        fl = (uint8_t*)malloc((size_t)npts);
        px = (int32_t*)malloc((size_t)npts * sizeof(int32_t));
        py = (int32_t*)malloc((size_t)npts * sizeof(int32_t));

        if (!fl || !px || !py) {
            free(fl);
            free(px);
            free(py);
            return -1;
        }

        for (i = 0; i < npts && q < end;) {     /* flags with repeats */
            uint8_t v = f->data[q++];
            int rep = 0;

            if ((v & 8) && q < end) {
                rep = f->data[q++];
            }

            for (k = 0; k <= rep && i < npts; k++) {
                fl[i++] = v;
            }
        }

        if (i < npts) {
            free(fl);
            free(px);
            free(py);
            return -1;
        }

        xq = q;

        for (i = 0; i < npts; i++) {    /* x coordinates */
            if (fl[i] & 2) {
                int32_t dv = xq < end ? f->data[xq++] : 0;
                x += (fl[i] & 16) ? dv : -dv;
            } else if (!(fl[i] & 16)) {
                x += RS16(f, xq);
                xq += 2;
            }

            x = CLAMP_COORD(x);
            px[i] = x;
        }

        yq = xq;

        for (i = 0; i < npts; i++) {    /* y coordinates */
            if (fl[i] & 4) {
                int32_t dv = yq < end ? f->data[yq++] : 0;
                y += (fl[i] & 32) ? dv : -dv;
            } else if (!(fl[i] & 32)) {
                y += RS16(f, yq);
                yq += 2;
            }

            y = CLAMP_COORD(y);
            py[i] = y;
        }

        for (i = 0; i < npts; i++) {    /* to 26.6, transformed */
            apply(t, px[i] * 64, py[i] * 64, &px[i], &py[i]);
            fl[i] &= 1;
        }

        for (k = 0; k < ncont; k++) {
            int32_t e = (int32_t)RU16(f, (uint64_t)off + 10 + 2 * (uint64_t)k);

            if (e < c0 || e >= npts) {
                break;
            }

            tt_contour(p, px + c0, py + c0, fl + c0, e - c0 + 1);
            c0 = e + 1;
        }

        free(fl);
        free(px);
        free(py);
        return p->err ? -1 : 0;
    }

    /* composite: components with offsets and optional scale */
    {
        uint64_t q = (uint64_t)off + 10, end = (uint64_t)off + len;
        uint32_t flags;

        do {
            uint32_t comp;
            int32_t a1, a2;
            xform c, m;

            if (q + 4 > end) {
                return -1;
            }

            flags = RU16(f, q);
            comp = RU16(f, q + 2);
            q += 4;

            if (flags & 1) {    /* ARG_1_AND_2_ARE_WORDS */
                a1 = RS16(f, q);
                a2 = RS16(f, q + 2);
                q += 4;
            } else {
                a1 = (int8_t)f->data[q];
                a2 = (int8_t)f->data[q + 1];
                q += 2;
            }

            c.a = c.d = 1 << 14;
            c.b = c.c = 0;

            if (flags & 8) {            /* WE_HAVE_A_SCALE */
                c.a = c.d = RS16(f, q);
                q += 2;
            } else if (flags & 0x40) {  /* X_AND_Y_SCALE */
                c.a = RS16(f, q);
                c.d = RS16(f, q + 2);
                q += 4;
            } else if (flags & 0x80) {  /* TWO_BY_TWO */
                c.a = RS16(f, q);
                c.b = RS16(f, q + 2);
                c.c = RS16(f, q + 4);
                c.d = RS16(f, q + 6);
                q += 8;
            }

            /* ARGS_ARE_XY_VALUES: an offset; point matching is not supported (offset 0) */
            c.dx = (flags & 2) ? a1 * 64 : 0;
            c.dy = (flags & 2) ? a2 * 64 : 0;

            /* compose: parent t after component c */
            m.a = sat(((int64_t)t->a * c.a + (int64_t)t->c * c.b) >> 14);
            m.b = sat(((int64_t)t->b * c.a + (int64_t)t->d * c.b) >> 14);
            m.c = sat(((int64_t)t->a * c.c + (int64_t)t->c * c.d) >> 14);
            m.d = sat(((int64_t)t->b * c.c + (int64_t)t->d * c.d) >> 14);
            apply(t, c.dx, c.dy, &m.dx, &m.dy);

            if (tt_glyph(f, comp, &m, p, depth + 1)) {
                return -1;
            }
        } while (flags & 0x20);     /* MORE_COMPONENTS */
    }

    return 0;
}

int pd_cff_glyph(const pd_font* f, uint32_t g, void (*emit)(void*, int, const int32_t*, const int32_t*, int),
                 void* user);

static void cff_emit(void* user, int op, const int32_t* xs, const int32_t* ys, int n) {
    path_add((path_t*)user, op, xs, ys, n);
}

static int glyph_path(const pd_font* f, uint32_t g, path_t* p) {
    xform id;

    memset(p, 0, sizeof(*p));

    if (f->glyf && f->loca) {
        id.a = id.d = 1 << 14;
        id.b = id.c = id.dx = id.dy = 0;
        return tt_glyph(f, g, &id, p, 0);
    }

    if (f->cff) {
        return pd_cff_glyph(f, g, cff_emit, p);
    }

    return -1;
}

pd_status pd_font_glyph_outline(const pd_font* f, uint32_t g, const pd_outline_sink* sink, void* user) {
    path_t p;
    int32_t i;
    int open = 0;

    if (!f || !sink) {
        return PD_ERR_ARG;
    }

    if (glyph_path(f, g, &p)) {
        free(p.s);
        return PD_ERR_FONT;
    }

    for (i = 0; i < p.n; i++) {
        const seg* s = &p.s[i];

        switch (s->op) {
            case OP_MOVE:
                if (open && sink->close) {
                    sink->close(user);
                }

                if (sink->move_to) {
                    sink->move_to(user, s->x[0], s->y[0]);
                }

                open = 1;
                break;

            case OP_LINE:
                if (sink->line_to) {
                    sink->line_to(user, s->x[0], s->y[0]);
                }

                break;

            case OP_QUAD:
                if (sink->quad_to) {
                    sink->quad_to(user, s->x[0], s->y[0], s->x[1], s->y[1]);
                }

                break;

            case OP_CUBIC:
                if (sink->cubic_to) {
                    sink->cubic_to(user, s->x[0], s->y[0], s->x[1], s->y[1], s->x[2], s->y[2]);
                }

                break;
        }
    }

    if (open && sink->close) {
        sink->close(user);
    }

    free(p.s);
    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* rasterizer: cells with cover and area, integer only                */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t w, h;
    int32_t* cover;             /* w x h */
    int32_t* area;
} canvas;

/* one piece inside one cell: fractional x positions fa, fb in [0, ONE], height dy */
static void cell_put(canvas* c, int32_t row, int32_t cx, int32_t fa, int32_t fb, int32_t dy) {
    if (cx < 0) {   /* left of the bitmap: only its cover reaches the pixels */
        if (c->w > 0) {
            c->cover[row * c->w] += dy;
        }

        return;
    }

    if (cx >= c->w) {
        return;
    }

    c->cover[row * c->w + cx] += dy;
    c->area[row * c->w + cx] += dy * (fa + fb);
}

/* a segment that stays inside one row (y in subpixels within [0, ONE]), split at cell edges */
static void cell_add(canvas* c, int32_t row, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t cell = x0 >> 8, last = x1 >> 8, dir, xa = x0, ya = y0;
    int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;

    if (row < 0 || row >= c->h || y0 == y1) {
        return;
    }

    if (cell == last) {
        cell_put(c, row, cell, x0 - cell * ONE, x1 - cell * ONE, y1 - y0);
        return;
    }

    dir = x1 > x0 ? 1 : -1;

    for (;;) {
        int32_t edge, yb;

        if (cell == last) {
            cell_put(c, row, cell, xa - cell * ONE, x1 - cell * ONE, y1 - ya);
            return;
        }

        edge = dir > 0 ? (cell + 1) * ONE : cell * ONE;
        yb = (int32_t)(y0 + ((int64_t)(edge - x0) * dy) / dx);
        cell_put(c, row, cell, xa - cell * ONE, edge - cell * ONE, yb - ya);
        xa = edge;
        ya = yb;
        cell += dir;
    }
}

static void line_add(canvas* c, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    int32_t r0, r1, dir, row;
    int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
    int32_t xa = x0, ya = y0;

    if (y0 == y1) {
        return;
    }

    r0 = y0 >> 8;
    r1 = y1 >> 8;
    dir = y1 > y0 ? 1 : -1;

    if (r0 == r1) {
        cell_add(c, r0, x0, y0 - r0 * ONE, x1, y1 - r0 * ONE);
        return;
    }

    for (row = r0; ; row += dir) {
        int32_t by = dir > 0 ? (row + 1) * ONE : row * ONE, xb;

        if (row == r1) {
            cell_add(c, row, xa, ya - row * ONE, x1, y1 - row * ONE);
            return;
        }

        xb = (int32_t)(x0 + ((int64_t)(by - y0) * dx) / dy);
        cell_add(c, row, xa, ya - row * ONE, xb, by - row * ONE);
        xa = xb;
        ya = by;
    }
}

static int32_t isqrt(int64_t v) {
    int64_t r = 0, b = (int64_t)1 << 40;

    while (b > v) {
        b >>= 2;
    }

    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }

        b >>= 2;
    }

    return (int32_t)r;
}

/* subdivisions so the flattening error stays near 1/8 pixel */
static int32_t steps_for(int64_t devx, int64_t devy) {
    int64_t dev = (devx < 0 ? -devx : devx) + (devy < 0 ? -devy : devy);
    int32_t n = 1 + isqrt(dev / 32);

    return n > 64 ? 64 : n;
}

/* a 26.6 coordinate in subpixels, clamped so offsets and negation cannot overflow */
static int32_t scale_coord(int32_t v, int64_t scale) {
    int64_t r;

    v = v > (1 << 22) ? (1 << 22) : v < -(1 << 22) ? -(1 << 22) : v;     /* scale < 2^41: no int64 overflow */
    r = ((int64_t)v * scale) >> 32;

    return r > (1 << 30) ? (1 << 30) : r < -(1 << 30) ? -(1 << 30) : (int32_t)r;
}

pd_status pd_font_glyph_render(const pd_font* f, uint32_t g, pd_sp px_per_em, int32_t subpixel, uint8_t* buf,
                               int32_t cap, pd_glyph_image* info) {
    path_t p;
    int64_t scale;              /* 26.6 font units -> subpixels, as a 32.32 factor */
    int32_t i, k, minx = INT32_MAX, miny = INT32_MAX, maxx = INT32_MIN, maxy = INT32_MIN, need;
    int32_t ox, oy, px0 = 0, py0 = 0, cx = 0, cy = 0;
    canvas c;

    if (!f || !info || px_per_em <= 0 || px_per_em > PD_PT(2000) || subpixel < 0 || subpixel >= ONE || cap < 0) {
        return PD_ERR_ARG;
    }

    memset(info, 0, sizeof(*info));

    if (glyph_path(f, g, &p)) {
        free(p.s);
        return PD_ERR_FONT;
    }

    /* subpixels per 26.6 unit = px_per_em/65536 * ONE / (upem * 64) */
    scale = ((int64_t)px_per_em << 16) * ONE / ((int64_t)f->m.units_per_em * 64) ;

#define SX(v) (scale_coord(v, scale) + subpixel)
#define SY(v) (-scale_coord(v, scale))

    for (i = 0; i < p.n; i++) {
        int npts = p.s[i].op == OP_CUBIC ? 3 : p.s[i].op == OP_QUAD ? 2 : 1;

        for (k = 0; k < npts; k++) {
            int32_t x = SX(p.s[i].x[k]), y = SY(p.s[i].y[k]);

            minx = x < minx ? x : minx;
            maxx = x > maxx ? x : maxx;
            miny = y < miny ? y : miny;
            maxy = y > maxy ? y : maxy;
        }
    }

    if (p.n == 0) {     /* nothing to draw */
        free(p.s);
        return PD_OK;
    }

    ox = (minx >> 8);
    oy = (miny >> 8);

    /* bounds of a malformed outline may span the whole int32 range */
    if ((int64_t)(maxx >> 8) - ox + 1 > 4096 || (int64_t)(maxy >> 8) - oy + 1 > 4096) {
        free(p.s);
        return PD_ERR_RANGE;
    }

    info->left = ox;
    info->top = -oy;
    info->width = (maxx >> 8) - ox + 1;
    info->height = (maxy >> 8) - oy + 1;
    need = info->width * info->height;

    if (!buf) {
        free(p.s);
        return PD_OK;
    }

    if (cap < need) {
        free(p.s);
        return PD_ERR_RANGE;
    }

    c.w = info->width;
    c.h = info->height;
    c.cover = (int32_t*)calloc((size_t)need, sizeof(int32_t));
    c.area = (int32_t*)calloc((size_t)need, sizeof(int32_t));

    if (!c.cover || !c.area) {
        free(c.cover);
        free(c.area);
        free(p.s);
        return PD_ERR_NOMEM;
    }

    ox *= ONE;
    oy *= ONE;

    for (i = 0; i < p.n; i++) {
        const seg* s = &p.s[i];

        if (s->op == OP_MOVE) {
            if (i > 0 && (cx != px0 || cy != py0)) {
                line_add(&c, cx, cy, px0, py0);     /* close the previous contour */
            }

            cx = px0 = SX(s->x[0]) - ox;
            cy = py0 = SY(s->y[0]) - oy;
        } else if (s->op == OP_LINE) {
            int32_t x = SX(s->x[0]) - ox, y = SY(s->y[0]) - oy;

            line_add(&c, cx, cy, x, y);
            cx = x;
            cy = y;
        } else {
            int32_t x1 = SX(s->x[0]) - ox, y1 = SY(s->y[0]) - oy, x2 = SX(s->x[1]) - ox, y2 = SY(s->y[1]) - oy;
            int32_t x3 = 0, y3 = 0, n, j, lx = cx, ly = cy;

            if (s->op == OP_QUAD) {
                n = steps_for((int64_t)cx - 2 * x1 + x2, (int64_t)cy - 2 * y1 + y2);
            } else {
                x3 = SX(s->x[2]) - ox;
                y3 = SY(s->y[2]) - oy;
                n = steps_for(((int64_t)cx - 2 * x1 + x2) + ((int64_t)x1 - 2 * x2 + x3),
                              ((int64_t)cy - 2 * y1 + y2) + ((int64_t)y1 - 2 * y2 + y3));
            }

            for (j = 1; j <= n; j++) {
                int64_t t = j, u = n - j, nn = n;
                int32_t x, y;

                if (s->op == OP_QUAD) {
                    int64_t d = nn * nn;
                    x = (int32_t)((u * u * cx + 2 * u * t * x1 + t * t * x2) / d);
                    y = (int32_t)((u * u * cy + 2 * u * t * y1 + t * t * y2) / d);
                } else {
                    int64_t d = nn * nn * nn;
                    x = (int32_t)((u * u * u * cx + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3) / d);
                    y = (int32_t)((u * u * u * cy + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3) / d);
                }

                line_add(&c, lx, ly, x, y);
                lx = x;
                ly = y;
            }

            cx = s->op == OP_QUAD ? x2 : x3;
            cy = s->op == OP_QUAD ? y2 : y3;
        }
    }

    if (cx != px0 || cy != py0) {
        line_add(&c, cx, cy, px0, py0);
    }

    /* accumulate: coverage = covers to the left + the partial area of this cell */
    for (i = 0; i < c.h; i++) {
        int32_t acc = 0;

        for (k = 0; k < c.w; k++) {
            int32_t v = acc * 2 * ONE + c.cover[i * c.w + k] * 2 * ONE - c.area[i * c.w + k];
            int32_t a;

            acc += c.cover[i * c.w + k];
            v = v < 0 ? -v : v;
            a = (int32_t)(((int64_t)v * 255 + ONE * ONE) / (2 * ONE * ONE));   /* rounded */
            buf[i * c.w + k] = (uint8_t)(a > 255 ? 255 : a);
        }
    }

    free(c.cover);
    free(c.area);
    free(p.s);
    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* display-list paths: filled and stroked into an RGBA image           */
/* ------------------------------------------------------------------ */

/* Geometry (a stroke's outline, its dashes) is worked out in doubles in
   subpixels; what reaches the pixels goes through the integer cells above,
   each piece of a stroke wound the same way so overlapping pieces add up
   under the non-zero rule instead of cancelling. */

#define RC_LIMIT (1 << 26)      /* subpixel coordinates kept within this, so no row loop runs away */

static int32_t rc_clamp(double v) {
    return v > RC_LIMIT ? RC_LIMIT : v < -RC_LIMIT ? -RC_LIMIT : (int32_t)lround(v);
}

/* a closed polygon, wound positively (reversed if not) */
static void rc_poly(canvas* c, const double* xy, int32_t n) {
    double a = 0;
    int32_t i;

    if (n < 3) {
        return;
    }

    for (i = 0; i < n; i++) {
        int32_t j = (i + 1) % n;

        a += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }

    for (i = 0; i < n; i++) {
        int32_t p = a >= 0 ? i : n - 1 - i, q = a >= 0 ? (i + 1) % n : (2 * n - 2 - i) % n;

        line_add(c, rc_clamp(xy[2 * p]), rc_clamp(xy[2 * p + 1]), rc_clamp(xy[2 * q]), rc_clamp(xy[2 * q + 1]));
    }
}

static void rc_disc(canvas* c, double x, double y, double r) {
    double xy[2 * 64];
    int32_t k, n = 8 + (int32_t)(r / 48);

    n = n > 64 ? 64 : n;

    for (k = 0; k < n; k++) {
        double t = k * 6.283185307179586 / n;

        xy[2 * k] = x + r * cos(t);
        xy[2 * k + 1] = y + r * sin(t);
    }

    rc_poly(c, xy, n);
}

static void rc_quad(canvas* c, double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) {
    double xy[8] = { ax, ay, bx, by, cx, cy, dx, dy };

    rc_poly(c, xy, 4);
}

/* a polyline's stroke (half width hw): a band along each segment, the corners joined and the ends capped as
   the style says */
static void rc_stroke_line(canvas* c, const double* xy, int32_t n, int closed, double hw, int cap, int join) {
    int32_t i, nseg;

    while (closed && n > 1 && xy[2 * n - 2] == xy[0] && xy[2 * n - 1] == xy[1]) {
        n--;    /* the closing point given twice */
    }

    if (n == 1 || (n == 2 && xy[0] == xy[2] && xy[1] == xy[3])) {   /* a dot */
        if (cap == PD_CAP_ROUND) {
            rc_disc(c, xy[0], xy[1], hw);
        } else if (cap == PD_CAP_SQUARE) {
            rc_quad(c, xy[0] - hw, xy[1] - hw, xy[0] + hw, xy[1] - hw, xy[0] + hw, xy[1] + hw, xy[0] - hw, xy[1] + hw);
        }

        return;
    }

    nseg = closed ? n : n - 1;

    for (i = 0; i < nseg; i++) {
        const double* a = &xy[2 * i], *b = &xy[2 * ((i + 1) % n)];
        double dx = b[0] - a[0], dy = b[1] - a[1], len = sqrt(dx * dx + dy * dy), nx, ny, ex = 0, ey = 0, sx = 0, sy = 0;

        if (len <= 0) {
            continue;
        }

        nx = -dy / len * hw;
        ny = dx / len * hw;

        if (!closed && cap == PD_CAP_SQUARE) {  /* the ends run on half the width */
            if (i == 0) {
                sx = -dx / len * hw;
                sy = -dy / len * hw;
            }

            if (i == nseg - 1) {
                ex = dx / len * hw;
                ey = dy / len * hw;
            }
        }

        rc_quad(c, a[0] + sx + nx, a[1] + sy + ny, b[0] + ex + nx, b[1] + ey + ny, b[0] + ex - nx, b[1] + ey - ny,
                a[0] + sx - nx, a[1] + sy - ny);
    }

    for (i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {   /* the corners */
        const double* v = &xy[2 * i], *p = &xy[2 * ((i + n - 1) % n)], *q = &xy[2 * ((i + 1) % n)];
        double d0x = v[0] - p[0], d0y = v[1] - p[1], d1x = q[0] - v[0], d1y = q[1] - v[1];
        double l0 = sqrt(d0x * d0x + d0y * d0y), l1 = sqrt(d1x * d1x + d1y * d1y), n0x, n0y, n1x, n1y, s, cs;

        if (l0 <= 0 || l1 <= 0) {
            continue;
        }

        d0x /= l0;
        d0y /= l0;
        d1x /= l1;
        d1y /= l1;

        if (join == PD_JOIN_ROUND) {
            rc_disc(c, v[0], v[1], hw);
            continue;
        }

        n0x = -d0y;
        n0y = d0x;
        n1x = -d1y;
        n1y = d1x;
        s = n0x * d1x + n0y * d1y < 0 ? hw : -hw;    /* the outer side of the turn */
        cs = d0x * d1x + d0y * d1y;     /* cos of the turn */

        if (cs > 0.9999) {
            continue;   /* straight on */
        }

        if (join == PD_JOIN_MITER && 1 + cs >= 1.0 / 32) {     /* the point no further than 8 widths out */
            double mx = n0x + n1x, my = n0y + n1y, k = 2 / (mx * mx + my * my);

            rc_quad(c, v[0], v[1], v[0] + s * n0x, v[1] + s * n0y, v[0] + s * mx * k, v[1] + s * my * k,
                    v[0] + s * n1x, v[1] + s * n1y);
        } else {
            double xy3[6] = { v[0], v[1], v[0] + s * n0x, v[1] + s * n0y, v[0] + s * n1x, v[1] + s * n1y };

            rc_poly(c, xy3, 3);
        }
    }

    if (!closed && cap == PD_CAP_ROUND) {
        rc_disc(c, xy[0], xy[1], hw);
        rc_disc(c, xy[2 * n - 2], xy[2 * n - 1], hw);
    }
}

/* a ring's dashes: each stretch of it that is on stroked as an open line, the pattern starting again with the
   ring */
static void rc_dashed(canvas* c, const double* xy, int32_t n, int closed, double hw, const double* dash, int nd,
                      int cap, int join) {
    double* run = (double*)malloc(((size_t)n + 2) * 2 * sizeof(double)), left = dash[0];
    int32_t i, nr = 0, di = 0, on = 1, nseg = closed ? n : n - 1;

    if (!run) {
        return;
    }

    run[0] = xy[0];
    run[1] = xy[1];
    nr = 1;

    for (i = 0; i < nseg; i++) {
        const double* a = &xy[2 * i], *b = &xy[2 * ((i + 1) % n)];
        double dx = b[0] - a[0], dy = b[1] - a[1], len = sqrt(dx * dx + dy * dy), t = 0;

        while (len - t > left) {    /* the pattern changes within this segment */
            double px, py;

            t += left;
            px = a[0] + dx * t / len;
            py = a[1] + dy * t / len;

            if (on) {
                run[2 * nr] = px;
                run[2 * nr + 1] = py;
                rc_stroke_line(c, run, nr + 1, 0, hw, cap, join);
            }

            run[0] = px;    /* a gap ends or a dash ends: either way the next run starts here */
            run[1] = py;
            nr = 1;
            on = !on;
            di = (di + 1) % nd;
            left = dash[di];
        }

        left -= len - t;

        if (on) {
            run[2 * nr] = b[0];
            run[2 * nr + 1] = b[1];
            nr++;
        } else {
            run[0] = b[0];
            run[1] = b[1];
            nr = 1;
        }
    }

    if (on && nr >= 2) {
        rc_stroke_line(c, run, nr, 0, hw, cap, join);
    }

    free(run);
}

/* what a pixel is painted with */
typedef struct {
    uint32_t color;             /* 0xAARRGGBB */
} rpaint;

static uint32_t rp_color(const rpaint* p, int32_t x, int32_t y) {
    (void)x;
    (void)y;
    return p->color;
}

/* the canvas's coverage painted over the image (premultiplied), the canvas's top-left at pixel (px, py) */
static void rc_composite(const canvas* c, int32_t px, int32_t py, const rpaint* paint, uint8_t* rgba, int32_t stride) {
    int32_t i, k;

    for (i = 0; i < c->h; i++) {
        int32_t acc = 0;
        uint8_t* row = rgba + (size_t)(py + i) * (size_t)stride + (size_t)px * 4;

        for (k = 0; k < c->w; k++) {
            int32_t v = acc * 2 * ONE + c->cover[i * c->w + k] * 2 * ONE - c->area[i * c->w + k], cov;

            acc += c->cover[i * c->w + k];
            v = v < 0 ? -v : v;
            cov = (int32_t)(((int64_t)v * 255 + ONE * ONE) / (2 * ONE * ONE));
            cov = cov > 255 ? 255 : cov;

            if (cov) {
                uint32_t col = rp_color(paint, px + k, py + i);
                int32_t a = (int32_t)((col >> 24) * (uint32_t)cov + 127) / 255, q;
                uint8_t* d = row + 4 * k;

                for (q = 0; q < 3; q++) {
                    int32_t s = (int32_t)((col >> (16 - 8 * q)) & 255) * a / 255;

                    d[q] = (uint8_t)(s + d[q] * (255 - a) / 255);
                }

                d[3] = (uint8_t)(a + d[3] * (255 - a) / 255);
            }
        }
    }
}

/* a page coordinate in subpixels of the image */
static double rc_sub(pd_sp v, pd_sp o, int32_t scale) {
    return (double)(((int64_t)v - o) * scale / (1 << 24));
}

/* a path's rings in subpixels relative to (ox, oy) subpixels: each ring given to fn */
static void rc_rings(const pd_draw* a, pd_sp x0, pd_sp y0, int32_t scale, double ox, double oy,
                     void (*fn)(void*, const double*, int32_t), void* user) {
    double* xy = (double*)malloc(((size_t)a->npoints + 1) * 2 * sizeof(double));
    int32_t i, n = 0;

    if (!xy) {
        return;
    }

    for (i = 0; i <= a->npoints; i++) {
        if (i == a->npoints || a->points[2 * i] == PD_PATH_BREAK) {
            if (n) {
                fn(user, xy, n);
            }

            n = 0;
            continue;
        }

        xy[2 * n] = rc_sub(a->points[2 * i], x0, scale) - ox;
        xy[2 * n + 1] = rc_sub(a->points[2 * i + 1], y0, scale) - oy;
        n++;
    }

    free(xy);
}

typedef struct {
    canvas* c;
    double hw;                  /* stroke: half its width; fill: 0 */
    int closed, cap, join, nd;
    double dash[8];
} rc_job;

static void rc_ring(void* user, const double* xy, int32_t n) {
    rc_job* J = (rc_job*)user;
    int32_t i;

    if (J->hw <= 0) {   /* filled: the ring closed */
        for (i = 0; i < n; i++) {
            int32_t j = (i + 1) % n;

            line_add(J->c, rc_clamp(xy[2 * i]), rc_clamp(xy[2 * i + 1]), rc_clamp(xy[2 * j]), rc_clamp(xy[2 * j + 1]));
        }
    } else if (J->nd) {
        rc_dashed(J->c, xy, n, J->closed, J->hw, J->dash, J->nd, J->cap, J->join);
    } else {
        rc_stroke_line(J->c, xy, n, J->closed, J->hw, J->cap, J->join);
    }
}

static void rc_clear(canvas* c) {
    memset(c->cover, 0, (size_t)c->w * (size_t)c->h * sizeof(int32_t));
    memset(c->area, 0, (size_t)c->w * (size_t)c->h * sizeof(int32_t));
}

pd_status pd_draw_render(const pd_draw* items, int32_t n, pd_sp x0, pd_sp y0, int32_t scale, uint8_t* rgba,
                         int32_t width, int32_t height, int32_t stride) {
    int32_t i;

    if ((!items && n) || n < 0 || !rgba || width <= 0 || height <= 0 || stride < 4 * width || scale <= 0 ||
            scale > (256 << 16)) {
        return PD_ERR_ARG;
    }

    for (i = 0; i < n; i++) {
        const pd_draw* a = &items[i];
        const pd_path_style* st = a->kind == PD_DRAW_PATH ? a->style : NULL;
        double hw = 0, grow, bx0, by0, bx1, by1;
        int32_t px0, py0, px1, py1;
        canvas c;
        rpaint paint;
        rc_job J;

        if (a->kind == PD_DRAW_PATH && (!a->points || a->npoints < 1)) {
            continue;
        }

        if (a->kind != PD_DRAW_PATH && a->kind != PD_DRAW_RULE) {
            continue;
        }

        if (a->kind == PD_DRAW_PATH && a->line_width > 0 && a->color) {
            hw = rc_sub(a->line_width, 0, scale) / 2;
            hw = hw < ONE / 4 ? ONE / 4 : hw;   /* no thinner than a quarter pixel */
        }

        grow = hw * (st && st->join == PD_JOIN_MITER ? 8 : 1.5) + 2 * ONE;
        bx0 = rc_sub(a->x, x0, scale) - grow;
        by0 = rc_sub(a->y, y0, scale) - grow;
        bx1 = rc_sub(a->x + a->w, x0, scale) + grow;
        by1 = rc_sub(a->y + a->h, y0, scale) + grow;
        px0 = bx0 < 0 ? 0 : bx0 > (double)width * ONE ? width : (int32_t)(bx0 / ONE);
        py0 = by0 < 0 ? 0 : by0 > (double)height * ONE ? height : (int32_t)(by0 / ONE);
        px1 = bx1 < 0 ? 0 : bx1 >= (double)width * ONE ? width : (int32_t)(bx1 / ONE) + 1;
        py1 = by1 < 0 ? 0 : by1 >= (double)height * ONE ? height : (int32_t)(by1 / ONE) + 1;

        if (px1 <= px0 || py1 <= py0) {
            continue;
        }

        c.w = px1 - px0;
        c.h = py1 - py0;
        c.cover = (int32_t*)calloc((size_t)c.w * (size_t)c.h, sizeof(int32_t));
        c.area = (int32_t*)calloc((size_t)c.w * (size_t)c.h, sizeof(int32_t));

        if (!c.cover || !c.area) {
            free(c.cover);
            free(c.area);
            return PD_ERR_NOMEM;
        }

        memset(&J, 0, sizeof(J));
        J.c = &c;

        if (a->kind == PD_DRAW_RULE) {
            double q[8];

            q[0] = q[6] = rc_sub(a->x, x0, scale) - (double)px0 * ONE;
            q[1] = q[3] = rc_sub(a->y, y0, scale) - (double)py0 * ONE;
            q[2] = q[4] = rc_sub(a->x + a->w, x0, scale) - (double)px0 * ONE;
            q[5] = q[7] = rc_sub(a->y + a->h, y0, scale) - (double)py0 * ONE;
            rc_ring(&J, q, 4);
            paint.color = a->color;
            rc_composite(&c, px0, py0, &paint, rgba, stride);
        } else {
            if (a->fill) {
                rc_rings(a, x0, y0, scale, (double)px0 * ONE, (double)py0 * ONE, rc_ring, &J);
                paint.color = a->fill;
                rc_composite(&c, px0, py0, &paint, rgba, stride);
            }

            if (hw > 0) {
                int k;

                rc_clear(&c);
                J.hw = hw;
                J.closed = (a->path_flags & PD_PATH_CLOSED) != 0;
                J.cap = st ? st->cap : PD_CAP_FLAT;
                J.join = st ? st->join : PD_JOIN_ROUND;

                for (k = 0; st && k < st->dash_count && k < 8; k++) {
                    J.dash[k] = rc_sub(st->dash[k], 0, scale);
                    J.dash[k] = J.dash[k] < ONE / 8 ? ONE / 8 : J.dash[k];
                }

                J.nd = st && st->dash_count >= 2 ? (st->dash_count > 8 ? 8 : st->dash_count) & ~1 : 0;
                rc_rings(a, x0, y0, scale, (double)px0 * ONE, (double)py0 * ONE, rc_ring, &J);
                paint.color = a->color;
                rc_composite(&c, px0, py0, &paint, rgba, stride);
            }
        }

        free(c.cover);
        free(c.area);
    }

    return PD_OK;
}
