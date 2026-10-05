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

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

#define ONE 256                 /* subpixels per pixel */
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

static void apply(const xform* t, int32_t x, int32_t y, int32_t* ox, int32_t* oy) {
    *ox = (int32_t)(((int64_t)t->a * x + (int64_t)t->c * y) >> 14) + t->dx;
    *oy = (int32_t)(((int64_t)t->b * x + (int64_t)t->d * y) >> 14) + t->dy;
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
            m.a = (int32_t)(((int64_t)t->a * c.a + (int64_t)t->c * c.b) >> 14);
            m.b = (int32_t)(((int64_t)t->b * c.a + (int64_t)t->d * c.b) >> 14);
            m.c = (int32_t)(((int64_t)t->a * c.c + (int64_t)t->c * c.d) >> 14);
            m.d = (int32_t)(((int64_t)t->b * c.c + (int64_t)t->d * c.d) >> 14);
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

#define SX(v) ((int32_t)(((int64_t)(v) * scale) >> 32) + subpixel)
#define SY(v) (-(int32_t)(((int64_t)(v) * scale) >> 32))

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
    info->left = ox;
    info->top = -oy;
    info->width = (maxx >> 8) - ox + 1;
    info->height = (maxy >> 8) - oy + 1;
    need = info->width * info->height;

    if (info->width > 4096 || info->height > 4096) {
        free(p.s);
        return PD_ERR_RANGE;
    }

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
