/*
 * Parade PDF reader: drawing a display list into pixels
 *
 * Paths are flattened in device space and filled by area accumulation
 * (each edge adds its signed area to the cells it crosses; a running sum
 * along a row is the winding coverage, non-zero or even-odd by how it is
 * folded). Strokes become polygons -- a band a segment, joins, caps,
 * dashes -- filled non-zero. Clips are masks of the same coverage, an
 * axis-aligned rectangle kept as a rectangle. Glyphs are filled outlines,
 * their bitmaps kept by font, glyph, size and quarter-pixel offset.
 * Pictures are sampled bilinearly from a level no more than twice the size
 * they cover. Shadings are evaluated a pixel at a time; meshes are drawn
 * as Gouraud triangles; tiling patterns are drawn once into a tile and
 * repeated.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr_dl.h"

pimage* pimage_level(pdoc* d, pimage* im, double px_w, double px_h);
uint32_t psh_fn_color(const pshade* s, double x, double y);

/* ------------------------------------------------------------------ */
/* the drawing context                                                */
/* ------------------------------------------------------------------ */

typedef struct mask {
    int ref;
    int32_t x0, y0, x1, y1;     /* where anything shows (device pixels, x1 y1 exclusive) */
    uint8_t* m;                 /* coverage over the target (w x h), NULL: all of the rectangle */
} mask;

typedef struct {
    int32_t node;
    mask* mk;
} mslot;

typedef struct {
    pdoc* d;
    dlist* L;
    double M[6];
    rtarget* t;
    float* acc;                 /* the accumulation buffer, reused */
    size_t acc_cap;
    uint8_t* cov;               /* a fill's coverage, reused */
    size_t cov_cap;
    uint8_t* pbuf;              /* a paint's colours over a fill's box, reused */
    size_t pbuf_cap;
    mslot slots[8];
    int nslots;
    double* pts;                /* flattened rings: x y pairs; ring starts in rings */
    int32_t npts, cappts;
    int32_t* rings;             /* each ring's first point, and whether it is closed (1 << 30) */
    int32_t nrings, caprings;
    int ri, gi, bi;             /* channel order */
    uint8_t** smasks;           /* each soft mask over the target, made when first used */
    int32_t last_clip, last_smask;  /* the clip and soft mask last put together, and the result */
    mask* both;
} rctx;

static int grow(void** p, int32_t* cap, int32_t need, size_t elem) {
    if (need <= *cap) {
        return 1;
    }

    {
        int32_t nc = *cap ? *cap : 256;
        void* q;

        while (nc < need) {
            nc *= 2;
        }

        q = realloc(*p, (size_t)nc * elem);

        if (!q) {
            return 0;
        }

        *p = q;
        *cap = nc;
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* flattening                                                         */
/* ------------------------------------------------------------------ */

static void ring_start(rctx* R, int closed) {
    if (grow((void**)&R->rings, &R->caprings, R->nrings + 1, sizeof(int32_t))) {
        R->rings[R->nrings++] = R->npts | (closed ? 1 << 30 : 0);
    }
}

static void ring_pt(rctx* R, double x, double y) {
    if (grow((void**)&R->pts, &R->cappts, 2 * (R->npts + 1), sizeof(double))) {
        R->pts[2 * R->npts] = x;
        R->pts[2 * R->npts + 1] = y;
        R->npts++;
    }
}

static int32_t ring_first(const rctx* R, int32_t i) {
    return R->rings[i] & ((1 << 30) - 1);
}

static int32_t ring_end(const rctx* R, int32_t i) {
    return i + 1 < R->nrings ? ring_first(R, i + 1) : R->npts;
}

static void cubic_flat(rctx* R, double x0, double y0, double x1, double y1, double x2, double y2, double x3,
                       double y3) {
    double ddx = fabs(x0 - 2 * x1 + x2), ddy = fabs(y0 - 2 * y1 + y2);
    double ddx2 = fabs(x1 - 2 * x2 + x3), ddy2 = fabs(y1 - 2 * y2 + y3);
    double dd = sqrt((ddx > ddx2 ? ddx : ddx2) * (ddx > ddx2 ? ddx : ddx2) + (ddy > ddy2 ? ddy : ddy2) *
                     (ddy > ddy2 ? ddy : ddy2));
    int n = (int)ceil(sqrt(dd * 0.75 / 0.2)), k;

    n = n < 1 ? 1 : n > 200 ? 200 : n;

    for (k = 1; k <= n; k++) {
        double t = (double)k / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;

        ring_pt(R, a * x0 + b * x1 + c * x2 + e * x3, a * y0 + b * y1 + c * y2 + e * y3);
    }
}

/* a list path through M into rings */
static void flatten(rctx* R, const double* v, int32_t n, const double* M) {
    int32_t i = 0;
    double cx = 0, cy = 0, sx = 0, sy = 0;
    int open = 0;

    R->npts = 0;
    R->nrings = 0;

    while (i < n) {
        int op = (int)v[i];

        if (op == 0 && i + 2 < n) {
            mat_pt(M, v[i + 1], v[i + 2], &cx, &cy);
            sx = cx;
            sy = cy;
            ring_start(R, 0);
            ring_pt(R, cx, cy);
            open = 1;
            i += 3;
        } else if (op == 1 && i + 2 < n) {
            if (!open) {
                ring_start(R, 0);
                ring_pt(R, cx, cy);
                open = 1;
            }

            mat_pt(M, v[i + 1], v[i + 2], &cx, &cy);
            ring_pt(R, cx, cy);
            i += 3;
        } else if (op == 2 && i + 6 < n) {
            double x1, y1, x2, y2, x3, y3;

            if (!open) {
                ring_start(R, 0);
                ring_pt(R, cx, cy);
                open = 1;
            }

            mat_pt(M, v[i + 1], v[i + 2], &x1, &y1);
            mat_pt(M, v[i + 3], v[i + 4], &x2, &y2);
            mat_pt(M, v[i + 5], v[i + 6], &x3, &y3);
            cubic_flat(R, cx, cy, x1, y1, x2, y2, x3, y3);
            cx = x3;
            cy = y3;
            i += 7;
        } else if (op == 3) {
            if (open && R->nrings) {
                R->rings[R->nrings - 1] |= 1 << 30;
            }

            /* after a close the pen is back at the start: a later line starts a new ring from there */
            cx = sx;
            cy = sy;
            open = 0;
            i += 1;
        } else {
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* accumulation                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t x0, y0, w, h;       /* the box (device pixels) */
    float* a;                   /* (w + 2) x h */
} acc_t;

static void acc_line(acc_t* A, double px0, double py0, double px1, double py1) {
    double dir, dxdy, x;
    int32_t y, ystart, yend;
    double w = A->w;

    px0 -= A->x0;
    px1 -= A->x0;
    py0 -= A->y0;
    py1 -= A->y0;

    if (py0 == py1) {
        return;
    }

    if (py0 < py1) {
        dir = 1;
    } else {
        double t;

        dir = -1;
        t = px0;
        px0 = px1;
        px1 = t;
        t = py0;
        py0 = py1;
        py1 = t;
    }

    if (py1 <= 0 || py0 >= A->h) {
        return;
    }

    dxdy = (px1 - px0) / (py1 - py0);
    x = px0;

    if (py0 < 0) {
        x -= py0 * dxdy;
        py0 = 0;
    }

    if (py1 > A->h) {
        py1 = A->h;
    }

    ystart = (int32_t)py0;
    yend = (int32_t)ceil(py1);

    for (y = ystart; y < yend && y < A->h; y++) {
        float* row = A->a + (size_t)y * (size_t)(A->w + 2);
        double dy = ((y + 1) < py1 ? (y + 1) : py1) - (y > py0 ? y : py0);
        double xnext = x + dxdy * dy, d = dy * dir, xa, xb;
        double x0c = x < 0 ? 0 : x > w ? w : x, x1c = xnext < 0 ? 0 : xnext > w ? w : xnext;

        if (x0c < x1c) {
            xa = x0c;
            xb = x1c;
        } else {
            xa = x1c;
            xb = x0c;
        }

        {
            double xaf = floor(xa);
            int32_t xai = (int32_t)xaf, xbi = (int32_t)ceil(xb);

            if (xbi <= xai + 1) {
                double xmf = 0.5 * (x0c + x1c) - xaf;

                row[xai] += (float)(d - d * xmf);
                row[xai + 1] += (float)(d * xmf);
            } else {
                double s = 1 / (xb - xa), x0f = xa - xaf, a0 = 0.5 * s * (1 - x0f) * (1 - x0f);
                double x1f = xb - xbi + 1, am = 0.5 * s * x1f * x1f;
                int32_t xi;

                row[xai] += (float)(d * a0);

                if (xbi == xai + 2) {
                    row[xai + 1] += (float)(d * (1 - a0 - am));
                } else {
                    double a1 = s * (1.5 - x0f), a2;

                    row[xai + 1] += (float)(d * (a1 - a0));

                    for (xi = xai + 2; xi < xbi - 1; xi++) {
                        row[xi] += (float)(d * s);
                    }

                    a2 = a1 + (xbi - xai - 3) * s;
                    row[xbi - 1] += (float)(d * (1 - a2 - am));
                }

                row[xbi] += (float)(d * am);
            }
        }

        x = xnext;
    }
}

static int acc_begin(rctx* R, acc_t* A, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    size_t need;

    A->x0 = x0;
    A->y0 = y0;
    A->w = x1 - x0;
    A->h = y1 - y0;

    if (A->w <= 0 || A->h <= 0) {
        return 0;
    }

    need = (size_t)(A->w + 2) * (size_t)A->h;

    if (need > R->acc_cap) {
        float* q = (float*)realloc(R->acc, need * sizeof(float));

        if (!q) {
            return 0;
        }

        R->acc = q;
        R->acc_cap = need;
    }

    A->a = R->acc;
    memset(A->a, 0, need * sizeof(float));
    return 1;
}

/* the coverage of the accumulated rows into R->cov (w x h bytes) */
static uint8_t* acc_cover(rctx* R, const acc_t* A, int evenodd) {
    size_t need = (size_t)A->w * (size_t)A->h;
    int32_t x, y;

    if (need > R->cov_cap) {
        uint8_t* q = (uint8_t*)realloc(R->cov, need);

        if (!q) {
            return NULL;
        }

        R->cov = q;
        R->cov_cap = need;
    }

    for (y = 0; y < A->h; y++) {
        const float* row = A->a + (size_t)y * (size_t)(A->w + 2);
        uint8_t* o = R->cov + (size_t)y * (size_t)A->w;
        double s = 0;

        for (x = 0; x < A->w; x++) {
            double c;

            s += row[x];
            c = fabs(s);

            if (evenodd) {
                c = fmod(c, 2.0);
                c = c > 1 ? 2 - c : c;
            } else if (c > 1) {
                c = 1;
            }

            o[x] = (uint8_t)(c * 255 + 0.5);
        }
    }

    return R->cov;
}

/* the rings into the accumulator, each closed */
static void acc_rings(rctx* R, acc_t* A) {
    int32_t r;

    for (r = 0; r < R->nrings; r++) {
        int32_t a = ring_first(R, r), b = ring_end(R, r), i;

        if (b - a < 2) {
            continue;
        }

        for (i = a; i < b; i++) {
            int32_t j = i + 1 < b ? i + 1 : a;

            acc_line(A, R->pts[2 * i], R->pts[2 * i + 1], R->pts[2 * j], R->pts[2 * j + 1]);
        }
    }
}

/* a polygon added wound positively, so overlapping pieces of a stroke add up */
static void acc_poly(acc_t* A, const double* xy, int n) {
    double area = 0;
    int i;

    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;

        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }

    for (i = 0; i < n; i++) {
        int p = area >= 0 ? i : n - 1 - i, q = area >= 0 ? (i + 1) % n : (2 * n - 2 - i) % n;

        acc_line(A, xy[2 * p], xy[2 * p + 1], xy[2 * q], xy[2 * q + 1]);
    }
}

static void acc_disc(acc_t* A, double x, double y, double r) {
    double xy[2 * 48];
    int k, n = 8 + (int)(r * 2);

    n = n > 48 ? 48 : n;

    for (k = 0; k < n; k++) {
        double t = k * 2 * M_PI / n;

        xy[2 * k] = x + r * cos(t);
        xy[2 * k + 1] = y + r * sin(t);
    }

    acc_poly(A, xy, n);
}

static void acc_quad(acc_t* A, double ax, double ay, double bx, double by, double cx, double cy, double dx, double dy) {
    double xy[8] = { ax, ay, bx, by, cx, cy, dx, dy };

    acc_poly(A, xy, 4);
}

/* ------------------------------------------------------------------ */
/* strokes                                                            */
/* ------------------------------------------------------------------ */

static void stroke_line(acc_t* A, const double* xy, int32_t n, int closed, double hw, int cap, int join,
                        double miter) {
    int32_t i, nseg;

    while (closed && n > 1 && xy[2 * n - 2] == xy[0] && xy[2 * n - 1] == xy[1]) {
        n--;
    }

    if (n == 1 || (n == 2 && xy[0] == xy[2] && xy[1] == xy[3])) {
        if (cap == 1) {
            acc_disc(A, xy[0], xy[1], hw);
        } else if (cap == 2) {
            acc_quad(A, xy[0] - hw, xy[1] - hw, xy[0] + hw, xy[1] - hw, xy[0] + hw, xy[1] + hw, xy[0] - hw, xy[1] + hw);
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

        if (!closed && cap == 2) {
            if (i == 0) {
                sx = -dx / len * hw;
                sy = -dy / len * hw;
            }

            if (i == nseg - 1) {
                ex = dx / len * hw;
                ey = dy / len * hw;
            }
        }

        acc_quad(A, a[0] + sx + nx, a[1] + sy + ny, b[0] + ex + nx, b[1] + ey + ny, b[0] + ex - nx, b[1] + ey - ny,
                 a[0] + sx - nx, a[1] + sy - ny);
    }

    for (i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) {
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
        cs = d0x * d1x + d0y * d1y;

        if (cs > 0.99999) {
            continue;
        }

        if (join == 1) {
            acc_disc(A, v[0], v[1], hw);
            continue;
        }

        n0x = -d0y;
        n0y = d0x;
        n1x = -d1y;
        n1y = d1x;
        s = n0x * d1x + n0y * d1y < 0 ? hw : -hw;

        /* a miter while the point is within the limit: 1 / sin(half the angle) <= miter */
        if (join == 0 && miter * miter * (1 - cs) >= 2 - 1e-9) {
            double mx = n0x + n1x, my = n0y + n1y, k = 2 / (mx * mx + my * my);

            acc_quad(A, v[0], v[1], v[0] + s * n0x, v[1] + s * n0y, v[0] + s * mx * k, v[1] + s * my * k,
                     v[0] + s * n1x, v[1] + s * n1y);
        } else {
            double t[6] = { v[0], v[1], v[0] + s * n0x, v[1] + s * n0y, v[0] + s * n1x, v[1] + s * n1y };

            acc_poly(A, t, 3);
        }
    }

    if (!closed && cap == 1) {
        acc_disc(A, xy[0], xy[1], hw);
        acc_disc(A, xy[2 * n - 2], xy[2 * n - 1], hw);
    }
}

static void stroke_dashed(acc_t* A, const double* xy, int32_t n, int closed, double hw, const float* dash, int nd,
                          double phase, int cap, int join, double miter) {
    double* run = (double*)malloc(((size_t)n + 4) * 2 * sizeof(double)), left;
    int32_t i, nr, nseg = closed ? n : n - 1, di = 0, on = 1, guard = 0;
    double total = 0;

    if (!run) {
        return;
    }

    for (i = 0; i < nd; i++) {
        total += dash[i];
    }

    if (total <= 0) {
        free(run);
        stroke_line(A, xy, n, closed, hw, cap, join, miter);
        return;
    }

    phase = fmod(phase, total);

    if (phase < 0) {
        phase += total;
    }

    left = dash[0];

    while (phase > 0 && guard++ < 1000) {     /* the phase: into the pattern */
        if (phase >= left) {
            phase -= left;
            di = (di + 1) % nd;
            on = !on;
            left = dash[di];
        } else {
            left -= phase;
            phase = 0;
        }
    }

    run[0] = xy[0];
    run[1] = xy[1];
    nr = 1;

    for (i = 0; i < nseg; i++) {
        const double* a = &xy[2 * i], *b = &xy[2 * ((i + 1) % n)];
        double dx = b[0] - a[0], dy = b[1] - a[1], len = sqrt(dx * dx + dy * dy), t = 0;
        int steps = 0;

        while (len - t > left && steps++ < 100000) {
            double px, py;

            t += left;
            px = a[0] + dx * t / len;
            py = a[1] + dy * t / len;

            if (on) {
                run[2 * nr] = px;
                run[2 * nr + 1] = py;
                stroke_line(A, run, nr + 1, 0, hw, cap, join, miter);
            }

            run[0] = px;
            run[1] = py;
            nr = 1;
            on = !on;
            di = (di + 1) % nd;
            left = dash[di] > 0 ? dash[di] : 1e-3;
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
        stroke_line(A, run, nr, 0, hw, cap, join, miter);
    }

    free(run);
}

/* ------------------------------------------------------------------ */
/* clips                                                              */
/* ------------------------------------------------------------------ */

static void mask_drop(mask* m) {
    if (m && --m->ref == 0) {
        free(m->m);
        free(m);
    }
}

/* the box of the rings, rounded out to pixels */
static void rings_box(const rctx* R, double pad, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    double a = 1e30, b = 1e30, c = -1e30, e = -1e30;
    int32_t i;

    for (i = 0; i < R->npts; i++) {
        double x = R->pts[2 * i], y = R->pts[2 * i + 1];

        a = x < a ? x : a;
        b = y < b ? y : b;
        c = x > c ? x : c;
        e = y > e ? y : e;
    }

    a -= pad;
    b -= pad;
    c += pad;
    e += pad;
    a = a < -1e8 ? -1e8 : a;
    b = b < -1e8 ? -1e8 : b;
    c = c > 1e8 ? 1e8 : c;
    e = e > 1e8 ? 1e8 : e;
    *x0 = (int32_t)floor(a);
    *y0 = (int32_t)floor(b);
    *x1 = (int32_t)ceil(c) + 1;
    *y1 = (int32_t)ceil(e) + 1;
}

/* an axis-aligned rectangle (one ring of four corners): its edges */
static int rings_rect(const rctx* R, double* r) {
    int32_t a, b, i, k = 0;
    double xs[5], ys[5];

    if (R->nrings != 1) {
        return 0;
    }

    a = ring_first(R, 0);
    b = ring_end(R, 0);

    if (b - a == 5 && R->pts[2 * a] == R->pts[2 * (b - 1)] && R->pts[2 * a + 1] == R->pts[2 * (b - 1) + 1]) {
        b--;
    }

    if (b - a != 4) {
        return 0;
    }

    for (i = a; i < b; i++) {
        xs[k] = R->pts[2 * i];
        ys[k] = R->pts[2 * i + 1];
        k++;
    }

    for (i = 0; i < 4; i++) {
        int j = (i + 1) % 4;

        if (fabs(xs[i] - xs[j]) > 1e-6 && fabs(ys[i] - ys[j]) > 1e-6) {
            return 0;
        }
    }

    r[0] = r[2] = xs[0];
    r[1] = r[3] = ys[0];

    for (i = 1; i < 4; i++) {
        r[0] = xs[i] < r[0] ? xs[i] : r[0];
        r[1] = ys[i] < r[1] ? ys[i] : r[1];
        r[2] = xs[i] > r[2] ? xs[i] : r[2];
        r[3] = ys[i] > r[3] ? ys[i] : r[3];
    }

    return 1;
}

static mask* clip_mask(rctx* R, int32_t node);

static mask* make_mask(rctx* R, int32_t node) {
    dlist* L = R->L;
    const dclip* c = &L->clips[node];
    mask* parent = c->parent >= 0 ? clip_mask(R, c->parent) : NULL;
    mask* m = (mask*)calloc(1, sizeof(mask));
    double r[4];

    if (!m) {
        return NULL;
    }

    m->ref = 1;

    if (parent) {
        m->x0 = parent->x0;
        m->y0 = parent->y0;
        m->x1 = parent->x1;
        m->y1 = parent->y1;
    } else {
        m->x0 = 0;
        m->y0 = 0;
        m->x1 = R->t->w;
        m->y1 = R->t->h;
    }

    if (c->npath == 0) {    /* nothing shows */
        m->x1 = m->x0;
        return m;
    }

    flatten(R, L->pts + c->path, c->npath, R->M);

    if (rings_rect(R, r) && (fabs(r[0] - floor(r[0] + 0.5)) < 0.02 || 1) && !c->evenodd) {
        /* a rectangle: its pixels, a partly covered one counted in when half covered */
        int32_t a = (int32_t)floor(r[0] + 0.5), b = (int32_t)floor(r[1] + 0.5);
        int32_t e = (int32_t)floor(r[2] + 0.5), f = (int32_t)floor(r[3] + 0.5);

        if (e == a && r[2] > r[0]) {
            e = a + 1;
        }

        if (f == b && r[3] > r[1]) {
            f = b + 1;
        }

        m->x0 = a > m->x0 ? a : m->x0;
        m->y0 = b > m->y0 ? b : m->y0;
        m->x1 = e < m->x1 ? e : m->x1;
        m->y1 = f < m->y1 ? f : m->y1;

        if (parent && parent->m) {
            m->m = NULL;
            /* share the parent's coverage: copy it, the rectangle on top */
            m->m = (uint8_t*)malloc((size_t)R->t->w * (size_t)R->t->h);

            if (m->m) {
                memcpy(m->m, parent->m, (size_t)R->t->w * (size_t)R->t->h);
            }
        }

        return m;
    }

    {
        acc_t A;
        int32_t x0, y0, x1, y1, x, y;
        uint8_t* cov;

        rings_box(R, 1, &x0, &y0, &x1, &y1);
        x0 = x0 > m->x0 ? x0 : m->x0;
        y0 = y0 > m->y0 ? y0 : m->y0;
        x1 = x1 < m->x1 ? x1 : m->x1;
        y1 = y1 < m->y1 ? y1 : m->y1;

        m->m = (uint8_t*)calloc((size_t)R->t->w * (size_t)R->t->h, 1);

        if (!m->m || x1 <= x0 || y1 <= y0 || !acc_begin(R, &A, x0, y0, x1, y1)) {
            m->x1 = m->x0;
            return m;
        }

        acc_rings(R, &A);
        cov = acc_cover(R, &A, c->evenodd);

        if (!cov) {
            m->x1 = m->x0;
            return m;
        }

        for (y = 0; y < A.h; y++) {
            uint8_t* o = m->m + (size_t)(y + y0) * (size_t)R->t->w + x0;
            const uint8_t* s = cov + (size_t)y * (size_t)A.w;
            const uint8_t* pm = parent && parent->m ? parent->m + (size_t)(y + y0) * (size_t)R->t->w + x0 : NULL;

            for (x = 0; x < A.w; x++) {
                o[x] = pm ? (uint8_t)((s[x] * pm[x] + 127) / 255) : s[x];
            }
        }

        m->x0 = x0;
        m->y0 = y0;
        m->x1 = x1;
        m->y1 = y1;
        return m;
    }
}

static mask* clip_mask(rctx* R, int32_t node) {
    int i;
    mask* m;

    if (node < 0) {
        return NULL;
    }

    for (i = 0; i < R->nslots; i++) {
        if (R->slots[i].node == node) {
            mslot s = R->slots[i];

            memmove(&R->slots[i], &R->slots[i + 1], sizeof(mslot) * (size_t)(R->nslots - i - 1));
            R->slots[R->nslots - 1] = s;
            return s.mk;
        }
    }

    m = make_mask(R, node);

    if (!m) {
        return NULL;
    }

    if (R->nslots == 8) {
        mask_drop(R->slots[0].mk);
        memmove(&R->slots[0], &R->slots[1], sizeof(mslot) * 7);
        R->nslots--;
    }

    R->slots[R->nslots].node = node;
    R->slots[R->nslots].mk = m;
    R->nslots++;
    return m;
}

/* ------------------------------------------------------------------ */
/* painting                                                           */
/* ------------------------------------------------------------------ */

static uint8_t blend1(int mode, int s, int dd) {
    switch (mode) {
        case BL_MULTIPLY: return (uint8_t)(s * dd / 255);
        case BL_SCREEN: return (uint8_t)(s + dd - s * dd / 255);
        case BL_DARKEN: return (uint8_t)(s < dd ? s : dd);
        case BL_LIGHTEN: return (uint8_t)(s > dd ? s : dd);
        case BL_DIFFERENCE: return (uint8_t)(s > dd ? s - dd : dd - s);
        case BL_OVERLAY: return (uint8_t)(dd < 128 ? 2 * s * dd / 255 : 255 - 2 * (255 - s) * (255 - dd) / 255);
        default: return (uint8_t)s;
    }
}

/* one pixel: colour r g b (not premultiplied) at alpha a (0..255) over the target's pixel */
static inline void put_px(const rctx* R, uint8_t* p, int r, int g, int b, int a, int mode) {
    int ia;

    if (a <= 0) {
        return;
    }

    if (mode != BL_NORMAL) {
        r = blend1(mode, r, p[R->ri]);
        g = blend1(mode, g, p[R->gi]);
        b = blend1(mode, b, p[R->bi]);
    }

    if (a >= 255) {
        p[R->ri] = (uint8_t)r;
        p[R->gi] = (uint8_t)g;
        p[R->bi] = (uint8_t)b;
        p[3] = 255;
        return;
    }

    ia = 255 - a;
    p[R->ri] = (uint8_t)((r * a + p[R->ri] * ia + 127) / 255);
    p[R->gi] = (uint8_t)((g * a + p[R->gi] * ia + 127) / 255);
    p[R->bi] = (uint8_t)((b * a + p[R->bi] * ia + 127) / 255);
    p[3] = (uint8_t)(a + p[3] * ia / 255);
}

static void draw_tile_into(rctx* R, const dtile* t, const double* P, uint8_t* buf, int32_t x0, int32_t y0,
                           int32_t w, int32_t h);

/* a shading's or pattern's colours over a box, RGBA (alpha 0 where it paints nothing) */
static uint8_t* paint_box(rctx* R, const dpaint* p, int32_t x0, int32_t y0, int32_t w, int32_t h) {
    size_t need = (size_t)w * (size_t)h * 4;
    double P[6], inv[6];
    int32_t x, y;

    if (need > R->pbuf_cap) {
        uint8_t* q = (uint8_t*)realloc(R->pbuf, need);

        if (!q) {
            return NULL;
        }

        R->pbuf = q;
        R->pbuf_cap = need;
    }

    memset(R->pbuf, 0, need);
    mat_mul(p->m, R->M, P);

    if (p->kind == DP_TILE) {
        if (p->index >= 0 && p->index < R->L->ntiles) {
            draw_tile_into(R, &R->L->tiles[p->index], P, R->pbuf, x0, y0, w, h);
        }

        return R->pbuf;
    }

    if (p->index < 0 || p->index >= R->L->nshades || !mat_inv(P, inv)) {
        return R->pbuf;
    }

    {
        const pshade* s = R->L->shades[p->index];

        if (s->type >= 4 && s->type <= 7) {
            /* Gouraud triangles, each pixel centre in one coloured from its corners */
            int32_t i;

            for (i = 0; i < s->ntri; i++) {
                const float* v = s->tri + (size_t)i * 15;
                double ax, ay, bx, by, cx, cy, det;
                int32_t tx0, ty0, tx1, ty1;

                mat_pt(P, v[0], v[1], &ax, &ay);
                mat_pt(P, v[5], v[6], &bx, &by);
                mat_pt(P, v[10], v[11], &cx, &cy);
                ax -= x0;
                bx -= x0;
                cx -= x0;
                ay -= y0;
                by -= y0;
                cy -= y0;
                det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);

                if (fabs(det) < 1e-12) {
                    continue;
                }

                tx0 = (int32_t)floor(fmin(ax, fmin(bx, cx)));
                ty0 = (int32_t)floor(fmin(ay, fmin(by, cy)));
                tx1 = (int32_t)ceil(fmax(ax, fmax(bx, cx)));
                ty1 = (int32_t)ceil(fmax(ay, fmax(by, cy)));
                tx0 = tx0 < 0 ? 0 : tx0;
                ty0 = ty0 < 0 ? 0 : ty0;
                tx1 = tx1 > w ? w : tx1;
                ty1 = ty1 > h ? h : ty1;

                for (y = ty0; y < ty1; y++) {
                    for (x = tx0; x < tx1; x++) {
                        double px = x + 0.5, py = y + 0.5;
                        double l1 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / det;
                        double l2 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / det, l3 = 1 - l1 - l2;

                        /* inside, with a hair's give so shared edges leave no gap */
                        if (l1 >= -1e-4 && l2 >= -1e-4 && l3 >= -1e-4) {
                            uint8_t* o = R->pbuf + ((size_t)y * (size_t)w + (size_t)x) * 4;
                            double r = l1 * v[2] + l2 * v[7] + l3 * v[12], g = l1 * v[3] + l2 * v[8] + l3 * v[13];
                            double b = l1 * v[4] + l2 * v[9] + l3 * v[14];

                            o[0] = (uint8_t)(fmax(0, fmin(1, r)) * 255 + 0.5);
                            o[1] = (uint8_t)(fmax(0, fmin(1, g)) * 255 + 0.5);
                            o[2] = (uint8_t)(fmax(0, fmin(1, b)) * 255 + 0.5);
                            o[3] = 255;
                        }
                    }
                }
            }

            return R->pbuf;
        }

        for (y = 0; y < h; y++) {
            uint8_t* o = R->pbuf + (size_t)y * (size_t)w * 4;

            for (x = 0; x < w; x++) {
                double sx, sy, t = -1;
                uint32_t c = 0;
                int ok = 0;

                mat_pt(inv, x0 + x + 0.5, y0 + y + 0.5, &sx, &sy);

                if (s->has_bbox && (sx < s->bbox[0] || sx > s->bbox[2] || sy < s->bbox[1] || sy > s->bbox[3])) {
                    continue;
                }

                if (s->type == 2) {
                    double dx = s->coords[2] - s->coords[0], dy = s->coords[3] - s->coords[1], l2 = dx * dx + dy * dy;

                    t = l2 > 0 ? ((sx - s->coords[0]) * dx + (sy - s->coords[1]) * dy) / l2 : 0;

                    if (t < 0) {
                        ok = s->extend[0];
                        t = 0;
                    } else if (t > 1) {
                        ok = s->extend[1];
                        t = 1;
                    } else {
                        ok = 1;
                    }
                } else if (s->type == 3) {
                    double x0c = s->coords[0], y0c = s->coords[1], r0 = s->coords[2];
                    double cdx = s->coords[3] - x0c, cdy = s->coords[4] - y0c, dr = s->coords[5] - r0;
                    double pdx = sx - x0c, pdy = sy - y0c;
                    double A = cdx * cdx + cdy * cdy - dr * dr, B = pdx * cdx + pdy * cdy + r0 * dr;
                    double C = pdx * pdx + pdy * pdy - r0 * r0;
                    double sol[2];
                    int ns = 0, k;

                    if (fabs(A) < 1e-12) {
                        if (fabs(B) > 1e-12) {
                            sol[ns++] = C / (2 * B);
                        }
                    } else {
                        double disc = B * B - A * C;

                        if (disc >= 0) {
                            double sq = sqrt(disc);

                            sol[ns++] = (B + sq) / A;
                            sol[ns++] = (B - sq) / A;

                            if (sol[1] > sol[0]) {
                                double tt = sol[0];

                                sol[0] = sol[1];
                                sol[1] = tt;
                            }
                        }
                    }

                    for (k = 0; k < ns && !ok; k++) {
                        double sv = sol[k];

                        if (r0 + sv * dr < 0) {
                            continue;
                        }

                        if (sv >= 0 && sv <= 1) {
                            t = sv;
                            ok = 1;
                        } else if (sv > 1 && s->extend[1]) {
                            t = 1;
                            ok = 1;
                        } else if (sv < 0 && s->extend[0]) {
                            t = 0;
                            ok = 1;
                        }
                    }
                } else if (s->type == 1) {
                    double fi[6], fx, fy;

                    if (mat_inv(s->matrix, fi)) {
                        mat_pt(fi, sx, sy, &fx, &fy);

                        if (fx >= s->fdomain[0] && fx <= s->fdomain[1] && fy >= s->fdomain[2] && fy <= s->fdomain[3]) {
                            c = psh_fn_color(s, fx, fy);
                            ok = 2;
                        }
                    }
                }

                if (ok == 1 && s->has_lut) {
                    int idx = (int)(t * 255 + 0.5);

                    c = s->lut[idx < 0 ? 0 : idx > 255 ? 255 : idx];
                }

                if (ok) {
                    o[4 * x] = (uint8_t)(c >> 16);
                    o[4 * x + 1] = (uint8_t)(c >> 8);
                    o[4 * x + 2] = (uint8_t)c;
                    o[4 * x + 3] = 255;
                }
            }
        }
    }

    return R->pbuf;
}

/* a coverage box (w x h at x0, y0 on the target) painted, through the clip */
static void paint_cov(rctx* R, const uint8_t* cov, int32_t x0, int32_t y0, int32_t w, int32_t h, const dpaint* p,
                      mask* m) {
    int32_t x, y, cx0 = x0, cy0 = y0, cx1 = x0 + w, cy1 = y0 + h;
    int alpha = (int)(p->alpha * 255 + 0.5);
    const uint8_t* pb = NULL;
    int r = (int)(p->rgb >> 16) & 255, g = (int)(p->rgb >> 8) & 255, b = (int)p->rgb & 255;

    if (alpha <= 0) {
        return;
    }

    cx0 = cx0 > 0 ? cx0 : 0;
    cy0 = cy0 > 0 ? cy0 : 0;
    cx1 = cx1 < R->t->w ? cx1 : R->t->w;
    cy1 = cy1 < R->t->h ? cy1 : R->t->h;

    if (m) {
        cx0 = cx0 > m->x0 ? cx0 : m->x0;
        cy0 = cy0 > m->y0 ? cy0 : m->y0;
        cx1 = cx1 < m->x1 ? cx1 : m->x1;
        cy1 = cy1 < m->y1 ? cy1 : m->y1;
    }

    if (cx1 <= cx0 || cy1 <= cy0) {
        return;
    }

    if (p->kind != DP_SOLID) {
        pb = paint_box(R, p, cx0, cy0, cx1 - cx0, cy1 - cy0);

        if (!pb) {
            return;
        }
    }

    for (y = cy0; y < cy1; y++) {
        uint8_t* row = R->t->buf + (size_t)y * (size_t)R->t->stride;
        const uint8_t* c = cov ? cov + (size_t)(y - y0) * (size_t)w - x0 : NULL;
        const uint8_t* mm = m && m->m ? m->m + (size_t)y * (size_t)R->t->w : NULL;
        const uint8_t* pp = pb ? pb + (size_t)(y - cy0) * (size_t)(cx1 - cx0) * 4 - (size_t)cx0 * 4 : NULL;

        for (x = cx0; x < cx1; x++) {
            int a = c ? c[x] : 255;

            if (!a) {
                continue;
            }

            if (mm) {
                a = a * mm[x] / 255;
            }

            if (pp) {
                const uint8_t* q = pp + (size_t)x * 4;

                a = a * q[3] / 255 * alpha / 255;
                put_px(R, row + (size_t)x * 4, q[0], q[1], q[2], a, p->blend);
            } else {
                put_px(R, row + (size_t)x * 4, r, g, b, alpha == 255 ? a : a * alpha / 255, p->blend);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* items                                                              */
/* ------------------------------------------------------------------ */

static void fill_rings(rctx* R, const ditem* it, mask* m, int evenodd, double pad) {
    acc_t A;
    int32_t x0, y0, x1, y1;
    uint8_t* cov;
    double r[4];

    if (R->npts == 0) {
        return;
    }

    rings_box(R, pad, &x0, &y0, &x1, &y1);
    x0 = x0 > 0 ? x0 : 0;
    y0 = y0 > 0 ? y0 : 0;
    x1 = x1 < R->t->w ? x1 : R->t->w;
    y1 = y1 < R->t->h ? y1 : R->t->h;

    if (m) {
        x0 = x0 > m->x0 ? x0 : m->x0;
        y0 = y0 > m->y0 ? y0 : m->y0;
        x1 = x1 < m->x1 ? x1 : m->x1;
        y1 = y1 < m->y1 ? y1 : m->y1;
    }

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    /* a rectangle: its coverage at once, the edges' fractions included */
    if (pad == 0 && rings_rect(R, r)) {
        int32_t w = x1 - x0, h = y1 - y0, x, y;
        size_t need = (size_t)w * (size_t)h;

        if (need > R->cov_cap) {
            uint8_t* q = (uint8_t*)realloc(R->cov, need);

            if (!q) {
                return;
            }

            R->cov = q;
            R->cov_cap = need;
        }

        for (y = 0; y < h; y++) {
            double py0 = y0 + y, cy = fmin(py0 + 1, r[3]) - fmax(py0, r[1]);

            cy = cy < 0 ? 0 : cy;

            for (x = 0; x < w; x++) {
                double px0 = x0 + x, cx = fmin(px0 + 1, r[2]) - fmax(px0, r[0]);

                cx = cx < 0 ? 0 : cx;
                R->cov[(size_t)y * (size_t)w + (size_t)x] = (uint8_t)(cx * cy * 255 + 0.5);
            }
        }

        paint_cov(R, R->cov, x0, y0, w, h, &it->paint, m);
        return;
    }

    if (!acc_begin(R, &A, x0, y0, x1, y1)) {
        return;
    }

    acc_rings(R, &A);
    cov = acc_cover(R, &A, evenodd);

    if (cov) {
        paint_cov(R, cov, x0, y0, A.w, A.h, &it->paint, m);
    }
}

static void draw_fill(rctx* R, const ditem* it, mask* m) {
    flatten(R, R->L->pts + it->path, it->npath, R->M);
    fill_rings(R, it, m, it->evenodd, 0);
}

static void draw_stroke(rctx* R, const ditem* it, mask* m) {
    double sc = sqrt(fabs(R->M[0] * R->M[3] - R->M[1] * R->M[2])), hw = it->lw * sc / 2, pad;
    acc_t A;
    int32_t x0, y0, x1, y1, r;
    uint8_t* cov;

    if (hw < 0.4) {     /* a hairline: a pixel or so wide */
        hw = 0.4;
    }

    flatten(R, R->L->pts + it->path, it->npath, R->M);

    if (R->npts == 0) {
        return;
    }

    pad = hw * (it->join == 0 ? (it->miter > 1 ? it->miter : 1) : 1) + 2;

    if (pad > hw * 10 + 2) {
        pad = hw * 10 + 2;
    }

    rings_box(R, pad, &x0, &y0, &x1, &y1);
    x0 = x0 > 0 ? x0 : 0;
    y0 = y0 > 0 ? y0 : 0;
    x1 = x1 < R->t->w ? x1 : R->t->w;
    y1 = y1 < R->t->h ? y1 : R->t->h;

    if (m) {
        x0 = x0 > m->x0 ? x0 : m->x0;
        y0 = y0 > m->y0 ? y0 : m->y0;
        x1 = x1 < m->x1 ? x1 : m->x1;
        y1 = y1 < m->y1 ? y1 : m->y1;
    }

    if (x1 <= x0 || y1 <= y0 || !acc_begin(R, &A, x0, y0, x1, y1)) {
        return;
    }

    for (r = 0; r < R->nrings; r++) {
        int32_t a = ring_first(R, r), b = ring_end(R, r);
        int closed = (R->rings[r] >> 30) & 1;

        if (it->ndash > 0 && it->dash + it->ndash <= R->L->ndashes) {
            float dash[16];
            int k;

            for (k = 0; k < it->ndash && k < 16; k++) {
                dash[k] = (float)(R->L->dashes[it->dash + k] * sc);
            }

            stroke_dashed(&A, R->pts + 2 * a, b - a, closed, hw, dash, it->ndash < 16 ? it->ndash : 16,
                          it->dash_phase * sc, it->cap, it->join, it->miter);
        } else {
            stroke_line(&A, R->pts + 2 * a, b - a, closed, hw, it->cap, it->join, it->miter);
        }
    }

    cov = acc_cover(R, &A, 0);

    if (cov) {
        paint_cov(R, cov, x0, y0, A.w, A.h, &it->paint, m);
    }
}

/* ---- glyphs ---- */

typedef struct gent {
    struct gent* next;
    const void* font;
    uint32_t key;
    int32_t qa, qb, qc, qd;     /* the matrix, in 1/64 of a pixel an em */
    uint8_t fx, fy;             /* the quarter-pixel offset */
    int32_t ox, oy, w, h;       /* the bitmap's top left relative to the pen's pixel */
    uint8_t* bits;
} gent;

#define GBUCKETS 4096
#define GBUDGET ((size_t)48 << 20)

typedef struct {
    gent* b[GBUCKETS];
} gcache;

static void gcache_clear(pdoc* d) {
    gcache* c = (gcache*)d->gcache;
    int i;

    if (!c) {
        return;
    }

    for (i = 0; i < GBUCKETS; i++) {
        gent* e = c->b[i];

        while (e) {
            gent* n = e->next;

            free(e->bits);
            free(e);
            e = n;
        }

        c->b[i] = NULL;
    }

    d->gbytes = 0;
}

void pd_glyph_cache_free(pdoc* d);

void pd_glyph_cache_free(pdoc* d) {
    gcache_clear(d);
    free(d->gcache);
    d->gcache = NULL;
}

typedef struct {
    rctx* R;
    double m[6];
    double cx, cy, sx, sy;
    int open;
} osink;

static void os_move(void* u, double x, double y) {
    osink* s = (osink*)u;
    double px, py;

    mat_pt(s->m, x, y, &px, &py);
    ring_start(s->R, 1);
    ring_pt(s->R, px, py);
    s->cx = s->sx = px;
    s->cy = s->sy = py;
    s->open = 1;
}

static void os_line(void* u, double x, double y) {
    osink* s = (osink*)u;
    double px, py;

    mat_pt(s->m, x, y, &px, &py);
    ring_pt(s->R, px, py);
    s->cx = px;
    s->cy = py;
}

static void os_cubic(void* u, double a, double b, double c, double dd, double x, double y) {
    osink* s = (osink*)u;
    double x1, y1, x2, y2, x3, y3;

    mat_pt(s->m, a, b, &x1, &y1);
    mat_pt(s->m, c, dd, &x2, &y2);
    mat_pt(s->m, x, y, &x3, &y3);
    cubic_flat(s->R, s->cx, s->cy, x1, y1, x2, y2, x3, y3);
    s->cx = x3;
    s->cy = y3;
}

static void os_close(void* u) {
    (void)u;
}

/* the outline through m into the rings */
static int glyph_rings(rctx* R, pfont* f, uint32_t code, const double* m) {
    osink s;
    pf_sink k;

    memset(&s, 0, sizeof(s));
    s.R = R;
    memcpy(s.m, m, sizeof(s.m));
    k.move = os_move;
    k.line = os_line;
    k.cubic = os_cubic;
    k.close = os_close;
    R->npts = 0;
    R->nrings = 0;
    return pf_outline(f, code, &k, &s);
}

static void draw_glyph(rctx* R, const ditem* it, mask* m) {
    double G[6];
    int32_t pxi, pyi, qa, qb, qc, qd;
    double px, py, em;
    uint8_t fx, fy;
    gcache* c;
    gent* e;
    uint32_t key = pf_glyph_key(it->font, it->code), h;

    mat_mul(it->m, R->M, G);
    px = G[4];
    py = G[5];
    em = sqrt(fabs(G[0] * G[3] - G[1] * G[2])) * 1000;

    if (key == 0xFFFFFFFFu || px < -em * 2 - 100 || py < -em * 2 - 100 || px > R->t->w + em * 2 + 100 ||
        py > R->t->h + em * 2 + 100) {
        return;
    }

    if (em > 300 || em < 0.5) {     /* large or tiny: filled as a path */
        if (glyph_rings(R, it->font, it->code, G) == 0) {
            fill_rings(R, it, m, 0, 1);
        }

        return;
    }

    pxi = (int32_t)floor(px);
    pyi = (int32_t)floor(py);
    fx = (uint8_t)((px - pxi) * 4);
    fy = (uint8_t)((py - pyi) * 4);
    qa = (int32_t)lround(G[0] * 64000);
    qb = (int32_t)lround(G[1] * 64000);
    qc = (int32_t)lround(G[2] * 64000);
    qd = (int32_t)lround(G[3] * 64000);

    if (!R->d->gcache) {
        R->d->gcache = calloc(1, sizeof(gcache));
    }

    c = (gcache*)R->d->gcache;

    if (!c) {
        return;
    }

    h = (uint32_t)((uintptr_t)it->font >> 4) * 2654435761u ^ key * 40503u ^ (uint32_t)qa * 31u ^ (uint32_t)qd * 17u ^
        (uint32_t)qb * 7u ^ (uint32_t)qc * 3u ^ fx * 101u ^ fy * 211u;
    h %= GBUCKETS;

    for (e = c->b[h]; e; e = e->next) {
        if (e->font == it->font && e->key == key && e->qa == qa && e->qb == qb && e->qc == qc && e->qd == qd &&
            e->fx == fx && e->fy == fy) {
            break;
        }
    }

    if (!e) {
        double Mq[6];
        acc_t A;
        int32_t x0, y0, x1, y1;
        uint8_t* cov;

        Mq[0] = qa / 64000.0;
        Mq[1] = qb / 64000.0;
        Mq[2] = qc / 64000.0;
        Mq[3] = qd / 64000.0;
        Mq[4] = fx / 4.0 + 0.125;
        Mq[5] = fy / 4.0 + 0.125;
        e = (gent*)calloc(1, sizeof(gent));

        if (!e) {
            return;
        }

        e->font = it->font;
        e->key = key;
        e->qa = qa;
        e->qb = qb;
        e->qc = qc;
        e->qd = qd;
        e->fx = fx;
        e->fy = fy;

        if (glyph_rings(R, it->font, it->code, Mq) == 0 && R->npts > 0) {
            rings_box(R, 0, &x0, &y0, &x1, &y1);

            if (x1 - x0 < 1024 && y1 - y0 < 1024 && acc_begin(R, &A, x0, y0, x1, y1)) {
                acc_rings(R, &A);
                cov = acc_cover(R, &A, 0);

                if (cov) {
                    e->bits = (uint8_t*)malloc((size_t)A.w * (size_t)A.h);

                    if (e->bits) {
                        memcpy(e->bits, cov, (size_t)A.w * (size_t)A.h);
                        e->ox = x0;
                        e->oy = y0;
                        e->w = A.w;
                        e->h = A.h;
                    }
                }
            }
        }

        if (R->d->gbytes > GBUDGET) {
            gcache_clear(R->d);
        }

        R->d->gbytes += sizeof(gent) + (size_t)e->w * (size_t)e->h;
        e->next = c->b[h];
        c->b[h] = e;
    }

    if (e->bits) {
        paint_cov(R, e->bits, pxi + e->ox, pyi + e->oy, e->w, e->h, &it->paint, m);
    }
}

/* ---- pictures ---- */

static void draw_image(rctx* R, const ditem* it, mask* m) {
    double D[6], inv[6], xs[4], ys[4], bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30, pw, ph;
    pimage* im0, *im;
    int32_t x0, y0, x1, y1, x, y, k;
    int alpha = (int)(it->paint.alpha * 255 + 0.5);
    int stencil;

    if (it->image < 0 || it->image >= R->L->nimages) {
        return;
    }

    mat_mul(it->m, R->M, D);

    if (!mat_inv(D, inv)) {
        return;
    }

    mat_pt(D, 0, 0, &xs[0], &ys[0]);
    mat_pt(D, 1, 0, &xs[1], &ys[1]);
    mat_pt(D, 1, 1, &xs[2], &ys[2]);
    mat_pt(D, 0, 1, &xs[3], &ys[3]);

    for (k = 0; k < 4; k++) {
        bx0 = xs[k] < bx0 ? xs[k] : bx0;
        by0 = ys[k] < by0 ? ys[k] : by0;
        bx1 = xs[k] > bx1 ? xs[k] : bx1;
        by1 = ys[k] > by1 ? ys[k] : by1;
    }

    x0 = (int32_t)floor(bx0);
    y0 = (int32_t)floor(by0);
    x1 = (int32_t)ceil(bx1);
    y1 = (int32_t)ceil(by1);
    x0 = x0 > 0 ? x0 : 0;
    y0 = y0 > 0 ? y0 : 0;
    x1 = x1 < R->t->w ? x1 : R->t->w;
    y1 = y1 < R->t->h ? y1 : R->t->h;

    if (m) {
        x0 = x0 > m->x0 ? x0 : m->x0;
        y0 = y0 > m->y0 ? y0 : m->y0;
        x1 = x1 < m->x1 ? x1 : m->x1;
        y1 = y1 < m->y1 ? y1 : m->y1;
    }

    if (x1 <= x0 || y1 <= y0) {
        return;
    }

    im0 = pdr_image_fetch(R->d, &R->L->images[it->image]);

    if (!im0) {
        return;
    }

    /* the pixels the picture covers each way: its sides' lengths on the device */
    pw = sqrt(D[0] * D[0] + D[1] * D[1]);
    ph = sqrt(D[2] * D[2] + D[3] * D[3]);
    im = pimage_level(R->d, im0, pw, ph);
    stencil = im->stencil;

    {
        int r = (int)(it->paint.rgb >> 16) & 255, g = (int)(it->paint.rgb >> 8) & 255, b = (int)it->paint.rgb & 255;
        int32_t W = im->w, H = im->h;
        int smooth = !(pw > W * 3 && ph > H * 3 && !im->interpolate && W * H < 4096) || stencil;

        for (y = y0; y < y1; y++) {
            uint8_t* row = R->t->buf + (size_t)y * (size_t)R->t->stride;
            const uint8_t* mm = m && m->m ? m->m + (size_t)y * (size_t)R->t->w : NULL;
            double u = inv[0] * (x0 + 0.5) + inv[2] * (y + 0.5) + inv[4];
            double v = inv[1] * (x0 + 0.5) + inv[3] * (y + 0.5) + inv[5];

            for (x = x0; x < x1; x++, u += inv[0], v += inv[1]) {
                double sx, sy;
                int a, cr, cg, cb, ca;

                if (u < 0 || u >= 1 || v < 0 || v >= 1) {
                    continue;
                }

                if (mm && !mm[x]) {
                    continue;
                }

                sx = u * W - 0.5;
                sy = v * H - 0.5;

                if (smooth) {
                    int32_t ix = (int32_t)floor(sx), iy = (int32_t)floor(sy), ix1 = ix + 1, iy1 = iy + 1;
                    double fxx = sx - ix, fyy = sy - iy;

                    ix = ix < 0 ? 0 : ix >= W ? W - 1 : ix;
                    iy = iy < 0 ? 0 : iy >= H ? H - 1 : iy;
                    ix1 = ix1 < 0 ? 0 : ix1 >= W ? W - 1 : ix1;
                    iy1 = iy1 < 0 ? 0 : iy1 >= H ? H - 1 : iy1;

                    if (stencil) {
                        double s0 = im->px[(size_t)iy * W + ix] * (1 - fxx) + im->px[(size_t)iy * W + ix1] * fxx;
                        double s1 = im->px[(size_t)iy1 * W + ix] * (1 - fxx) + im->px[(size_t)iy1 * W + ix1] * fxx;

                        a = (int)(s0 * (1 - fyy) + s1 * fyy + 0.5);
                        cr = r;
                        cg = g;
                        cb = b;
                    } else {
                        const uint8_t* p00 = im->px + ((size_t)iy * W + ix) * 4, *p01 = im->px + ((size_t)iy * W + ix1) * 4;
                        const uint8_t* p10 = im->px + ((size_t)iy1 * W + ix) * 4, *p11 = im->px + ((size_t)iy1 * W + ix1) * 4;
                        double w00 = (1 - fxx) * (1 - fyy) * p00[3], w01 = fxx * (1 - fyy) * p01[3];
                        double w10 = (1 - fxx) * fyy * p10[3], w11 = fxx * fyy * p11[3], ws = w00 + w01 + w10 + w11;

                        if (ws <= 0) {
                            continue;
                        }

                        cr = (int)((p00[0] * w00 + p01[0] * w01 + p10[0] * w10 + p11[0] * w11) / ws + 0.5);
                        cg = (int)((p00[1] * w00 + p01[1] * w01 + p10[1] * w10 + p11[1] * w11) / ws + 0.5);
                        cb = (int)((p00[2] * w00 + p01[2] * w01 + p10[2] * w10 + p11[2] * w11) / ws + 0.5);
                        a = (int)(ws + 0.5);
                    }
                } else {
                    int32_t ix = (int32_t)(u * W), iy = (int32_t)(v * H);
                    const uint8_t* q;

                    ix = ix >= W ? W - 1 : ix;
                    iy = iy >= H ? H - 1 : iy;
                    q = im->px + ((size_t)iy * W + ix) * 4;
                    cr = q[0];
                    cg = q[1];
                    cb = q[2];
                    a = q[3];
                }

                ca = a * alpha / 255;

                if (mm) {
                    ca = ca * mm[x] / 255;
                }

                put_px(R, row + (size_t)x * 4, cr, cg, cb, ca, it->paint.blend);
            }
        }
    }
}

static void draw_shade(rctx* R, const ditem* it, mask* m) {
    int32_t x0 = 0, y0 = 0, x1 = R->t->w, y1 = R->t->h;
    const pshade* s;

    if (it->paint.index < 0 || it->paint.index >= R->L->nshades) {
        return;
    }

    s = R->L->shades[it->paint.index];

    if (m) {
        x0 = m->x0;
        y0 = m->y0;
        x1 = m->x1;
        y1 = m->y1;
    }

    if (s->has_bbox) {  /* no further than its box */
        double P[6], xs[4], ys[4], a = 1e30, b = 1e30, c = -1e30, e = -1e30;
        int k;

        mat_mul(it->paint.m, R->M, P);
        mat_pt(P, s->bbox[0], s->bbox[1], &xs[0], &ys[0]);
        mat_pt(P, s->bbox[2], s->bbox[1], &xs[1], &ys[1]);
        mat_pt(P, s->bbox[2], s->bbox[3], &xs[2], &ys[2]);
        mat_pt(P, s->bbox[0], s->bbox[3], &xs[3], &ys[3]);

        for (k = 0; k < 4; k++) {
            a = xs[k] < a ? xs[k] : a;
            b = ys[k] < b ? ys[k] : b;
            c = xs[k] > c ? xs[k] : c;
            e = ys[k] > e ? ys[k] : e;
        }

        x0 = (int32_t)floor(a) > x0 ? (int32_t)floor(a) : x0;
        y0 = (int32_t)floor(b) > y0 ? (int32_t)floor(b) : y0;
        x1 = (int32_t)ceil(c) < x1 ? (int32_t)ceil(c) : x1;
        y1 = (int32_t)ceil(e) < y1 ? (int32_t)ceil(e) : y1;
    }

    if (x1 > x0 && y1 > y0) {
        paint_cov(R, NULL, x0, y0, x1 - x0, y1 - y0, &it->paint, m);
    }
}

static void draw_items(rctx* R, int32_t from, int32_t to);
static void rctx_free(rctx* R);

/* a soft mask's coverage over the target: its group drawn, its luminosity or alpha */
static uint8_t* soft_mask(rctx* R, int32_t id) {
    const dsmask* s;
    rtarget tt;
    rctx S;
    uint8_t* buf, *out;
    int32_t n, i;

    if (id < 0 || id >= R->L->nsmasks) {
        return NULL;
    }

    if (!R->smasks) {
        R->smasks = (uint8_t**)calloc((size_t)R->L->nsmasks, sizeof(uint8_t*));

        if (!R->smasks) {
            return NULL;
        }
    }

    if (R->smasks[id]) {
        return R->smasks[id];
    }

    s = &R->L->smasks[id];
    n = R->t->w * R->t->h;
    buf = (uint8_t*)malloc((size_t)n * 4);
    out = (uint8_t*)malloc((size_t)n);

    if (!buf || !out) {
        free(buf);
        free(out);
        return NULL;
    }

    for (i = 0; i < n; i++) {
        if (s->luminosity) {
            buf[4 * i] = (uint8_t)(s->backdrop >> 16);
            buf[4 * i + 1] = (uint8_t)(s->backdrop >> 8);
            buf[4 * i + 2] = (uint8_t)s->backdrop;
            buf[4 * i + 3] = 255;
        } else {
            buf[4 * i] = buf[4 * i + 1] = buf[4 * i + 2] = buf[4 * i + 3] = 0;
        }
    }

    tt.buf = buf;
    tt.w = R->t->w;
    tt.h = R->t->h;
    tt.stride = R->t->w * 4;
    tt.bgra = 0;
    memset(&S, 0, sizeof(S));
    S.d = R->d;
    S.L = s->group;
    memcpy(S.M, R->M, sizeof(S.M));
    S.t = &tt;
    S.ri = 0;
    S.gi = 1;
    S.bi = 2;
    S.last_clip = S.last_smask = -2;
    draw_items(&S, 0, s->group->n);
    rctx_free(&S);

    for (i = 0; i < n; i++) {
        out[i] = s->luminosity ? (uint8_t)((buf[4 * i] * 77 + buf[4 * i + 1] * 150 + buf[4 * i + 2] * 29) >> 8) :
                 buf[4 * i + 3];
    }

    free(buf);
    R->smasks[id] = out;
    return out;
}

/* the clip with a soft mask on it */
static mask* with_smask(rctx* R, mask* clip, int32_t clip_id, int32_t sm_id) {
    uint8_t* sm;
    mask* m;
    int32_t n, i;

    if (R->both && R->last_clip == clip_id && R->last_smask == sm_id) {
        return R->both;
    }

    sm = soft_mask(R, sm_id);

    if (!sm) {
        return clip;
    }

    m = (mask*)calloc(1, sizeof(mask));

    if (!m) {
        return clip;
    }

    n = R->t->w * R->t->h;
    m->ref = 1;
    m->x0 = clip ? clip->x0 : 0;
    m->y0 = clip ? clip->y0 : 0;
    m->x1 = clip ? clip->x1 : R->t->w;
    m->y1 = clip ? clip->y1 : R->t->h;
    m->m = (uint8_t*)malloc((size_t)n);

    if (!m->m) {
        free(m);
        return clip;
    }

    for (i = 0; i < n; i++) {
        m->m[i] = clip && clip->m ? (uint8_t)(clip->m[i] * sm[i] / 255) : sm[i];
    }

    mask_drop(R->both);
    R->both = m;
    R->last_clip = clip_id;
    R->last_smask = sm_id;
    return m;
}

static void draw_items(rctx* R, int32_t from, int32_t to) {
    int32_t i;

    for (i = from; i < to; i++) {
        const ditem* it = &R->L->items[i];
        mask* m = it->clip >= 0 ? clip_mask(R, it->clip) : NULL;

        if (it->smask >= 0) {
            m = with_smask(R, m, it->clip, it->smask);
        }

        if (m && (m->x1 <= m->x0 || m->y1 <= m->y0)) {
            continue;
        }

        if (m) {
            m->ref++;   /* held while drawn: a mask made below may push it out of the slots */
        }

        switch (it->type) {
            case DI_FILL: draw_fill(R, it, m); break;
            case DI_STROKE: draw_stroke(R, it, m); break;
            case DI_GLYPH: draw_glyph(R, it, m); break;
            case DI_IMAGE: draw_image(R, it, m); break;
            case DI_SHADE: draw_shade(R, it, m); break;
            default: break;
        }

        mask_drop(m);
    }
}

static void rctx_free(rctx* R) {
    int i;

    for (i = 0; i < R->nslots; i++) {
        mask_drop(R->slots[i].mk);
    }

    if (R->smasks) {
        for (i = 0; i < R->L->nsmasks; i++) {
            free(R->smasks[i]);
        }

        free(R->smasks);
    }

    mask_drop(R->both);

    free(R->acc);
    free(R->cov);
    free(R->pbuf);
    free(R->pts);
    free(R->rings);
}

/* ---- tiling patterns ---- */

/* the pattern's tile repeated over a box of w x h pixels at x0, y0 on the target (P: pattern space to the
   target's pixels) */
static void draw_tile_into(rctx* R, const dtile* t, const double* P, uint8_t* buf, int32_t x0, int32_t y0,
                           int32_t w, int32_t h) {
    double res = sqrt(fabs(P[0] * P[3] - P[1] * P[2])), inv[6];
    int32_t tw, th, x, y;
    uint8_t* tile;
    rtarget tt;
    rctx S;
    int i, j;

    if (res <= 0 || !mat_inv(P, inv)) {
        return;
    }

    tw = (int32_t)ceil(fabs(t->xstep) * res);
    th = (int32_t)ceil(fabs(t->ystep) * res);

    if (tw < 1 || th < 1) {
        return;
    }

    if (tw > 2048 || th > 2048) {   /* a huge cell drawn at less resolution */
        double k = 2048.0 / (tw > th ? tw : th);

        res *= k;
        tw = (int32_t)ceil(fabs(t->xstep) * res);
        th = (int32_t)ceil(fabs(t->ystep) * res);
    }

    tile = (uint8_t*)calloc((size_t)tw * (size_t)th, 4);

    if (!tile) {
        return;
    }

    tt.buf = tile;
    tt.w = tw;
    tt.h = th;
    tt.stride = tw * 4;
    tt.bgra = 0;

    /* the cell drawn at its neighbours' offsets too, so a cell's overhang shows in the next */
    for (j = -1; j <= 1; j++) {
        for (i = -1; i <= 1; i++) {
            double TM[6] = { res, 0, 0, res, (i * fabs(t->xstep)) * res, (j * fabs(t->ystep)) * res };

            memset(&S, 0, sizeof(S));
            S.d = R->d;
            S.L = t->cell;
            memcpy(S.M, TM, sizeof(TM));
            S.t = &tt;
            S.ri = 0;
            S.gi = 1;
            S.bi = 2;
            S.last_clip = S.last_smask = -2;
            draw_items(&S, 0, t->cell->n);
            rctx_free(&S);
        }
    }

    /* each pixel of the box: back to pattern space, into the cell, its tile pixel */
    for (y = 0; y < h; y++) {
        uint8_t* o = buf + (size_t)y * (size_t)w * 4;

        for (x = 0; x < w; x++) {
            double u, v;
            int32_t tx, ty;
            const uint8_t* q;

            mat_pt(inv, x0 + x + 0.5, y0 + y + 0.5, &u, &v);
            u = fmod(u, fabs(t->xstep));
            v = fmod(v, fabs(t->ystep));
            u = u < 0 ? u + fabs(t->xstep) : u;
            v = v < 0 ? v + fabs(t->ystep) : v;
            tx = (int32_t)(u * res);
            ty = (int32_t)(v * res);
            tx = tx >= tw ? tw - 1 : tx;
            ty = ty >= th ? th - 1 : ty;
            q = tile + ((size_t)ty * (size_t)tw + (size_t)tx) * 4;
            o[4 * x] = q[0];
            o[4 * x + 1] = q[1];
            o[4 * x + 2] = q[2];
            o[4 * x + 3] = q[3];
        }
    }

    free(tile);
}

void pr_draw(pdoc* d, dlist* L, const double* M, rtarget* t, int32_t from, int32_t to) {
    rctx R;

    memset(&R, 0, sizeof(R));
    R.d = d;
    R.L = L;
    memcpy(R.M, M, sizeof(R.M));
    R.t = t;
    R.ri = t->bgra ? 2 : 0;
    R.gi = 1;
    R.bi = t->bgra ? 0 : 2;
    R.last_clip = R.last_smask = -2;
    draw_items(&R, from, to);
    rctx_free(&R);
}

pd_status pd_pdfr_render(pd_pdfr* d, int32_t page, double scale, int32_t ox, int32_t oy, uint8_t* buf, int32_t width,
                         int32_t height, int32_t stride, int32_t flags) {
    dlist* L;
    rtarget t;
    double M[6];
    int32_t y;

    if (!d || !buf || width <= 0 || height <= 0 || stride < width * 4 || !(scale > 0)) {
        return PD_ERR_ARG;
    }

    L = pd_page_list(d, page);

    if (!L) {
        return PD_ERR_RANGE;
    }

    if (!(flags & PD_PDFR_NO_WHITE)) {
        for (y = 0; y < height; y++) {
            memset(buf + (size_t)y * (size_t)stride, 255, (size_t)width * 4);
        }
    }

    t.buf = buf;
    t.w = width;
    t.h = height;
    t.stride = stride;
    t.bgra = (flags & PD_PDFR_BGRA) != 0;
    M[0] = scale;
    M[1] = 0;
    M[2] = 0;
    M[3] = scale;
    M[4] = -ox;
    M[5] = -oy;

    pr_draw(d, L, M, &t, 0, (flags & PD_PDFR_NO_ANNOTS) ? L->annots_from : L->n);
    return PD_OK;
}
