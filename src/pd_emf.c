/*
 * pd_emf.c - Windows metafiles (EMF, WMF) as Parade drawings
 *
 * Word keeps pasted charts and diagrams as metafiles: records of GDI
 * calls. They are played here into the drawing description the layout
 * draws (application/vnd.parade.drawing+json): polygons and polylines
 * (rectangles, ellipses, Bezier curves flattened, GDI paths with their
 * holes), single lines of text at their reference points, and bitmaps,
 * each a PNG resource of the document. Coordinates go through the
 * metafile's window and viewport mapping and its world transform to the
 * picture's frame. EMF+ records (in comments) are skipped: Office writes
 * the plain EMF records beside them. Clipping to rectangles (a clip
 * rectangle, a region's bounds, a clip path's bounds) is followed: fills
 * cut to it, outlines cut segment by segment, text outside it left out;
 * raster operations other than copying are not modelled.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"
#include "pd_internal.h"
#include "pd_preset.h"

#define MAX_OBJ 1024

typedef struct {
    int kind;                   /* 0 none, 1 pen, 2 brush, 3 font, 4 other (a slot taken) */
    uint32_t color;             /* 0xAARRGGBB; pens and brushes */
    int null;                   /* a null pen or brush */
    double width;               /* pens: logical units */
    uint32_t style;             /* pens: PS_ dash, end cap and join bits */
    int hatch;                  /* brushes: HS_ + 1 when hatched, else 0 */
    double height;              /* fonts: logical units; < 0 character height */
    int weight, italic;
    int esc;                    /* fonts: the escapement, tenths of a degree anticlockwise */
    char face[64];
} gobj;

typedef struct {
    double wox, woy, wex, wey;  /* window */
    double vox, voy, vex, vey;  /* viewport */
    int map;                    /* 1 MM_TEXT, 7 isotropic, 8 anisotropic */
    double m[6];                /* world transform: x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5 */
    gobj pen, brush, font;
    uint32_t text_color;
    uint32_t bk_color;          /* behind a hatch, when bk_mode is OPAQUE (2) */
    int bk_mode;
    int text_align;
    double cx, cy;              /* current position, logical */
    int poly_fill;
    int clip;                   /* drawing clipped to (clip_x0, clip_y0)-(clip_x1, clip_y1), drawing units */
    double clip_x0, clip_y0, clip_x1, clip_y1;
} gstate;

typedef struct {
    pd_doc* d;
    pd_buf* o;
    int first;
    gstate g, stack[16];
    int depth;
    gobj obj[MAX_OBJ];
    int nobj;
    /* device (EMF) or logical (WMF) units to the drawing's */
    double fx0, fy0, fsx, fsy;
    int wmf;
    /* a GDI path being built */
    int in_path;
    double* pp;                 /* x, y pairs in drawing units; NAN pairs between figures */
    int npp, cappp;
    int fig_open;
    /* EMF+ (GDI+ records in comments): played instead of the plain records beside them */
    struct pctx_s* plus;        /* its state, while it is played; NULL when the plain records are */
    int mute;                   /* plain records not drawn: the EMF+ ones beside them are (but after a GetDC) */
    double lw_sp;               /* > 0: the line width (drawing units) for the next path, not the pen's */
    const char* dash_sp;        /* non-NULL: its dashes, in line widths (100000ths), not the pen's style */
    const char* grad_sp;        /* non-NULL: the fill's gradient, JSON members to add to the next filled path */
    int depth_mf;               /* metafiles drawn inside metafiles, this deep */
    int getdc;                  /* an EMF+ GetDC last: the plain records after it are drawn */
    /* curves kept as curves beside the points they are flattened to ("cv"): drawing units; (INFINITY, 0) before
       each cubic's three points, (NAN, NAN) between rings */
    double* pcv;                /* the GDI path being built's */
    int npcv, cappcv, pcv_cubic;
    const double* cur_cv;       /* the figure being written's, when it has a cubic */
    int cur_ncv;
} mf;

static uint32_t u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int32_t s32(const unsigned char* p) {
    return (int32_t)u32(p);
}

static uint16_t u16(const unsigned char* p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static int16_t s16(const unsigned char* p) {
    return (int16_t)u16(p);
}

/* a COLORREF (0x00BBGGRR) as 0xFFRRGGBB */
static uint32_t colorref(uint32_t c) {
    return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

static double fin(double v) {
    return isfinite(v) ? v : 0;
}

/* logical to drawing coordinates */
static void map_pt(const mf* M, double x, double y, double* ox, double* oy) {
    const gstate* g = &M->g;
    double wx = g->m[0] * x + g->m[2] * y + g->m[4], wy = g->m[1] * x + g->m[3] * y + g->m[5];
    double dx = wx, dy = wy;

    if (g->map == 7 || g->map == 8) {
        double sx = g->wex != 0 ? g->vex / g->wex : 1, sy = g->wey != 0 ? g->vey / g->wey : 1;

        if (g->map == 7) {      /* isotropic: one scale, the smaller */
            double s = fabs(sx) < fabs(sy) ? fabs(sx) : fabs(sy);

            sx = sx < 0 ? -s : s;
            sy = sy < 0 ? -s : s;
        }

        dx = (wx - g->wox) * sx + g->vox;
        dy = (wy - g->woy) * sy + g->voy;
    } else {
        dx = wx - g->wox + g->vox;
        dy = wy - g->woy + g->voy;
    }

    *ox = fin((dx - M->fx0) * M->fsx);
    *oy = fin((dy - M->fy0) * M->fsy);
}

/* a length in logical units, in drawing units (the transforms' mean scale) */
static double map_len(const mf* M, double v) {
    double ax, ay, bx, by, cx, cy;

    map_pt(M, 0, 0, &ax, &ay);
    map_pt(M, 1, 0, &bx, &by);
    map_pt(M, 0, 1, &cx, &cy);
    return fabs(v) * sqrt(fabs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)));
}

static void sep(mf* M) {
    if (!M->first) {
        pb_putc(M->o, ',');
    }

    M->first = 0;
}

/* a polygon or polyline (drawing units), with the pen and brush in use; NAN pairs part rings */
static void out_path_raw(mf* M, const double* xy, int n, int closed, int fill, int stroke) {
    uint32_t fc = fill && !M->g.brush.null && M->g.brush.kind == 2 ? M->g.brush.color : 0;
    uint32_t lc = stroke && !M->g.pen.null && M->g.pen.kind == 1 ? M->g.pen.color : 0;
    double lw = M->lw_sp > 0 ? M->lw_sp : M->g.pen.width > 0 ? map_len(M, M->g.pen.width) : 0.5 * 65536;    /* 0: a hairline */
    int i;

    if (n < 2 || (!fc && !lc) || M->mute) {
        return;
    }

    sep(M);
    pb_puts(M->o, "{\"path\":[");

    for (i = 0; i < n; i++) {
        if (isnan(xy[2 * i])) {
            pb_printf(M->o, "%s%d,%d", i ? "," : "", (int)INT32_MIN, (int)INT32_MIN);
        } else {
            pb_printf(M->o, "%s%d,%d", i ? "," : "", (int)xy[2 * i], (int)xy[2 * i + 1]);
        }
    }

    pb_printf(M->o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d", closed, (unsigned)fc, (unsigned)lc,
              (int)(lw < 1 ? 1 : lw));

    if (fc && M->g.brush.hatch) {   /* a hatched brush: the pattern Office has for it, over the background */
        static const char* hs[6] = { "horz", "vert", "dnDiag", "upDiag", "cross", "diagCross" };
        unsigned char b[8];

        if (pd_pattern_bits(hs[M->g.brush.hatch - 1], b)) {
            pb_printf(M->o, ",\"patt\":[%d,%d,%d,%d,%d,%d,%d,%d],\"pfg\":%u,\"pbg\":%u", b[0], b[1], b[2], b[3], b[4],
                      b[5], b[6], b[7], (unsigned)fc, (unsigned)(M->g.bk_mode == 2 ? M->g.bk_color : 0));
        }
    }

    if (fc && M->grad_sp) {
        pb_puts(M->o, M->grad_sp);
    }

    if (M->cur_cv && M->cur_ncv >= 2) {     /* its curves as curves, for output that draws them */
        pb_puts(M->o, ",\"cv\":[");

        for (i = 0; i < M->cur_ncv; i++) {
            const double* c = M->cur_cv + 2 * i;

            if (isnan(c[0])) {
                pb_printf(M->o, "%s%d,%d", i ? "," : "", (int)INT32_MIN, (int)INT32_MIN);
            } else if (isinf(c[0])) {
                pb_printf(M->o, "%s%d,0", i ? "," : "", (int)INT32_MIN + 1);
            } else {
                pb_printf(M->o, "%s%d,%d", i ? "," : "", (int)c[0], (int)c[1]);
            }
        }

        pb_putc(M->o, ']');
    }

    if (lc) {   /* the pen's dashes (as GDI's, in line widths), ends and corners */
        static const char* dash[5] = { "", "300000,100000", "100000,100000", "300000,100000,100000,100000",
                                       "300000,100000,100000,100000,100000,100000"
                                     };
        uint32_t ps = M->g.pen.style, cap = ps & 0xF00, join = ps & 0xF000;

        if (M->dash_sp) {
            pb_printf(M->o, ",\"dash\":[%s]", M->dash_sp);
        } else if ((ps & 15) >= 1 && (ps & 15) <= 4) {
            pb_printf(M->o, ",\"dash\":[%s]", dash[ps & 15]);
        }

        if ((ps & 0x10000) && cap != 0x200) {   /* a geometric pen's ends are round unless flat */
            pb_printf(M->o, ",\"cap\":%d", cap == 0x100 ? 2 : 1);
        }

        if (join) {
            pb_printf(M->o, ",\"join\":%d", join == 0x1000 ? 1 : 2);
        }
    }

    pb_putc(M->o, '}');
}

/* a ring (n points) cut to the clip rectangle (Sutherland-Hodgman), into out (room for 2 n + 8 points); its
   points */
static int clip_ring(const gstate* g, const double* in, int n, double* out) {
    double* a = (double*)malloc((size_t)(2 * n + 8) * 2 * sizeof(double)), *b, *t;
    int na = n, nb, e, i;

    if (!a) {
        return 0;
    }

    b = (double*)malloc((size_t)(2 * n + 8) * 2 * sizeof(double));

    if (!b) {
        free(a);
        return 0;
    }

    memcpy(a, in, (size_t)n * 2 * sizeof(double));

    for (e = 0; e < 4 && na > 0; e++) {     /* left, right, top, bottom */
        nb = 0;

        for (i = 0; i < na; i++) {
            const double* p = a + 2 * i, *q = a + 2 * ((i + 1) % na);
            double vp = e == 0 ? p[0] - g->clip_x0 : e == 1 ? g->clip_x1 - p[0] : e == 2 ? p[1] - g->clip_y0 :
                        g->clip_y1 - p[1];
            double vq = e == 0 ? q[0] - g->clip_x0 : e == 1 ? g->clip_x1 - q[0] : e == 2 ? q[1] - g->clip_y0 :
                        g->clip_y1 - q[1];

            if (vp >= 0) {
                b[2 * nb] = p[0];
                b[2 * nb + 1] = p[1];
                nb++;
            }

            if ((vp >= 0) != (vq >= 0) && nb < 2 * n + 8) {
                double tt = vp / (vp - vq);

                b[2 * nb] = p[0] + (q[0] - p[0]) * tt;
                b[2 * nb + 1] = p[1] + (q[1] - p[1]) * tt;
                nb++;
            }
        }

        t = a;
        a = b;
        b = t;
        na = nb;
    }

    memcpy(out, a, (size_t)na * 2 * sizeof(double));
    free(a);
    free(b);
    return na;
}

/* a segment cut to the clip rectangle (Liang-Barsky): 0 when none of it is inside */
static int clip_seg(const gstate* g, double* x0, double* y0, double* x1, double* y1) {
    double t0 = 0, t1 = 1, dx = *x1 - *x0, dy = *y1 - *y0;
    double p[4] = { -dx, dx, -dy, dy }, q[4] = { *x0 - g->clip_x0, g->clip_x1 - *x0, *y0 - g->clip_y0, g->clip_y1 - *y0 };
    int i;

    for (i = 0; i < 4; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) {
                return 0;
            }
        } else {
            double r = q[i] / p[i];

            if (p[i] < 0) {
                t0 = r > t0 ? r : t0;
            } else {
                t1 = r < t1 ? r : t1;
            }
        }
    }

    if (t0 > t1) {
        return 0;
    }

    *x1 = *x0 + dx * t1;
    *y1 = *y0 + dy * t1;
    *x0 = *x0 + dx * t0;
    *y0 = *y0 + dy * t0;
    return 1;
}

/* a polygon or polyline as out_path_raw draws it, cut to the clip rectangle when there is one: the fill ring by
   ring, the outline segment by segment (the clip's edges are not drawn) */
static void out_path(mf* M, const double* xy, int n, int closed, int fill, int stroke) {
    const gstate* g = &M->g;
    double* out;
    int i, start, no = 0;

    if (!g->clip || n < 2) {
        out_path_raw(M, xy, n, closed, fill, stroke);
        return;
    }

    {   /* all of it inside the clip: as it is, curves and all */
        int inside = 1;

        for (i = 0; i < n && inside; i++) {
            inside = isnan(xy[2 * i]) || (xy[2 * i] >= g->clip_x0 && xy[2 * i] <= g->clip_x1 &&
                                          xy[2 * i + 1] >= g->clip_y0 && xy[2 * i + 1] <= g->clip_y1);
        }

        if (inside) {
            out_path_raw(M, xy, n, closed, fill, stroke);
            return;
        }
    }

    M->cur_cv = NULL;   /* cut: its flattened points alone */

    if (!(out = (double*)malloc((size_t)(3 * n + 16) * 2 * sizeof(double)))) {
        return;
    }

    if (fill && closed) {   /* each ring cut, NAN pairs between them again */
        for (start = 0; start < n;) {
            int end = start, k;

            while (end < n && !isnan(xy[2 * end])) {
                end++;
            }

            if (end - start >= 3) {
                double* ring = (double*)malloc((size_t)(2 * (end - start) + 8) * 2 * sizeof(double));

                if (ring) {
                    k = clip_ring(g, xy + 2 * start, end - start, ring);

                    if (k >= 3) {
                        if (no > 0) {
                            out[2 * no] = out[2 * no + 1] = NAN;
                            no++;
                        }

                        memcpy(out + 2 * no, ring, (size_t)k * 2 * sizeof(double));
                        no += k;
                    }

                    free(ring);
                }
            }

            start = end + 1;
        }

        if (no >= 3) {
            out_path_raw(M, out, no, 1, 1, 0);
        }
    }

    if (stroke) {   /* the outline: its runs of segments inside, each a polyline */
        for (start = 0; start < n;) {
            int end = start;

            while (end < n && !isnan(xy[2 * end])) {
                end++;
            }

            no = 0;

            for (i = start; i < end - 1 + (closed && end - start > 2); i++) {
                const double* p = xy + 2 * i, *q = xy + 2 * (i + 1 < end ? i + 1 : start);
                double x0 = p[0], y0 = p[1], x1 = q[0], y1 = q[1];

                if (!clip_seg(g, &x0, &y0, &x1, &y1)) {
                    if (no >= 2) {
                        out_path_raw(M, out, no, 0, 0, 1);
                    }

                    no = 0;
                    continue;
                }

                if (no > 0 && (fabs(out[2 * no - 2] - x0) > 0.5 || fabs(out[2 * no - 1] - y0) > 0.5)) {
                    if (no >= 2) {  /* re-entered elsewhere: a new run */
                        out_path_raw(M, out, no, 0, 0, 1);
                    }

                    no = 0;
                }

                if (no == 0) {
                    out[0] = x0;
                    out[1] = y0;
                    no = 1;
                }

                if (no < 3 * n + 15) {
                    out[2 * no] = x1;
                    out[2 * no + 1] = y1;
                    no++;
                }
            }

            if (no >= 2) {
                out_path_raw(M, out, no, 0, 0, 1);
            }

            start = end + 1;
        }
    }

    free(out);
}

/* the clip rectangle narrowed to (x0, y0)-(x1, y1), drawing units (Copy: replaced) */
static void clip_to(mf* M, double x0, double y0, double x1, double y1, int copy) {
    gstate* g = &M->g;
    double t;

    if (x1 < x0) {
        t = x0;
        x0 = x1;
        x1 = t;
    }

    if (y1 < y0) {
        t = y0;
        y0 = y1;
        y1 = t;
    }

    if (g->clip && !copy) {
        x0 = x0 > g->clip_x0 ? x0 : g->clip_x0;
        y0 = y0 > g->clip_y0 ? y0 : g->clip_y0;
        x1 = x1 < g->clip_x1 ? x1 : g->clip_x1;
        y1 = y1 < g->clip_y1 ? y1 : g->clip_y1;
    }

    g->clip = 1;
    g->clip_x0 = x0;
    g->clip_y0 = y0;
    g->clip_x1 = x1 > x0 ? x1 : x0;
    g->clip_y1 = y1 > y0 ? y1 : y0;
}

/* a curve's pairs (logical units, the cv encoding) mapped to the drawing's: markers as they are */
static void map_cv(const mf* M, const double* l, int n, double* out) {
    int i;

    for (i = 0; i < n; i++) {
        if (isnan(l[2 * i]) || isinf(l[2 * i])) {
            out[2 * i] = l[2 * i];
            out[2 * i + 1] = l[2 * i + 1];
        } else {
            map_pt(M, l[2 * i], l[2 * i + 1], &out[2 * i], &out[2 * i + 1]);
        }
    }
}

/* the path being built, or straight out: points in logical units; lcv, when given, the same figure with its curves
   as curves (ncv pairs, the cv encoding, logical units) -- else the points are its curve */
static void figure_cv(mf* M, const double* lxy, int n, int closed, int fill, int stroke, const double* lcv, int ncv) {
    double* xy;
    int i;

    if (n < 1) {
        return;
    }

    if (!lcv) {
        lcv = lxy;
        ncv = n;
    }

    if (M->in_path) {
        int cont = M->npp > 0 && M->fig_open;

        if (pd_grow((void**)&M->pp, &M->cappp, (int64_t)M->npp + 2 * n + 2, sizeof(double)) ||
                pd_grow((void**)&M->pcv, &M->cappcv, (int64_t)M->npcv + 2 * ncv + 2, sizeof(double))) {
            return;
        }

        if (M->npp > 0 && !M->fig_open) {
            M->pp[M->npp++] = NAN;
            M->pp[M->npp++] = NAN;
        }

        if (M->npcv > 0 && !cont) {
            M->pcv[M->npcv++] = NAN;
            M->pcv[M->npcv++] = NAN;
        }

        for (i = 0; i < n; i++) {
            map_pt(M, lxy[2 * i], lxy[2 * i + 1], &M->pp[M->npp], &M->pp[M->npp + 1]);
            M->npp += 2;
        }

        map_cv(M, lcv, ncv, M->pcv + M->npcv);
        M->npcv += 2 * ncv;

        for (i = 0; i < ncv; i++) {
            M->pcv_cubic |= isinf(lcv[2 * i]) != 0;
        }

        M->fig_open = !closed;
        return;
    }

    if ((xy = (double*)malloc(((size_t)n + (size_t)ncv) * 2 * sizeof(double))) == NULL) {
        return;
    }

    for (i = 0; i < n; i++) {
        map_pt(M, lxy[2 * i], lxy[2 * i + 1], &xy[2 * i], &xy[2 * i + 1]);
    }

    if (lcv != lxy) {
        map_cv(M, lcv, ncv, xy + 2 * n);
        M->cur_cv = xy + 2 * n;
        M->cur_ncv = ncv;
    }

    out_path(M, xy, n, closed, fill, stroke);
    M->cur_cv = NULL;
    free(xy);
}

static void figure(mf* M, const double* lxy, int n, int closed, int fill, int stroke) {
    figure_cv(M, lxy, n, closed, fill, stroke, NULL, 0);
}

/* a cubic Bezier from p0 through p1, p2 to p3 as 16 straight pieces (p0 not included) */
static int bezier(double* out, const double* p) {
    int k;

    for (k = 1; k <= 16; k++) {
        double t = k / 16.0, u = 1 - t;

        out[2 * (k - 1)] = u * u * u * p[0] + 3 * u * u * t * p[2] + 3 * u * t * t * p[4] + t * t * t * p[6];
        out[2 * (k - 1) + 1] = u * u * u * p[1] + 3 * u * u * t * p[3] + 3 * u * t * t * p[5] + t * t * t * p[7];
    }

    return 16;
}

/* an ellipse in the box, logical units, as 64 points */
/* an elliptic arc (centre, radii; from angle a0 to a1, either way) as cubics, the cv encoding: its start (when
   start), then (INFINITY, 0) and three points for each piece of a quarter turn or less; the pairs written */
static int arc_cv(double cx, double cy, double rx, double ry, double a0, double a1, int start, double* out) {
    int pieces = (int)ceil(fabs(a1 - a0) / 1.5707963267948966 - 1e-9), k, n = 0;
    double step;

    pieces = pieces < 1 ? 1 : pieces > 4 ? 4 : pieces;
    step = (a1 - a0) / pieces;

    if (start) {
        out[n++] = cx + rx * cos(a0);
        out[n++] = cy + ry * sin(a0);
    }

    for (k = 0; k < pieces; k++) {
        double b0 = a0 + step * k, b1 = b0 + step, kk = 4.0 / 3 * tan((b1 - b0) / 4);

        out[n++] = INFINITY;
        out[n++] = 0;
        out[n++] = cx + rx * (cos(b0) - kk * sin(b0));
        out[n++] = cy + ry * (sin(b0) + kk * cos(b0));
        out[n++] = cx + rx * (cos(b1) + kk * sin(b1));
        out[n++] = cy + ry * (sin(b1) - kk * cos(b1));
        out[n++] = cx + rx * cos(b1);
        out[n++] = cy + ry * sin(b1);
    }

    return n / 2;
}

static void ellipse(mf* M, double l, double t, double r, double b) {
    double xy[128];
    int k;

    double cv[2 * 17];
    int nc;

    for (k = 0; k < 64; k++) {
        double a = k * 6.283185307179586 / 64;

        xy[2 * k] = (l + r) / 2 + (r - l) / 2 * cos(a);
        xy[2 * k + 1] = (t + b) / 2 + (b - t) / 2 * sin(a);
    }

    nc = arc_cv((l + r) / 2, (t + b) / 2, (r - l) / 2, (b - t) / 2, 0, 6.283185307179586, 1, cv);
    figure_cv(M, xy, 64, 1, 1, 1, cv, nc);
}

/* GDI's arcs: on the ellipse in (l, t, r, b), anticlockwise on the device from the radial through (xs, ys) to the
   one through (xe, ye). how 0 an arc (open), 1 a chord (closed), 2 a pie (closed through the centre), 3 an arc
   joined from the current position (ArcTo), which then moves to its end */
static void arc(mf* M, double l, double t, double r, double b, double xs, double ys, double xe, double ye, int how) {
    double xy[2 * 68], cx = (l + r) / 2, cy = (t + b) / 2, rx = fabs(r - l) / 2, ry = fabs(b - t) / 2;
    double a0 = atan2((ys - cy) * (rx > 0 ? rx : 1), (xs - cx) * (ry > 0 ? ry : 1));
    double a1 = atan2((ye - cy) * (rx > 0 ? rx : 1), (xe - cx) * (ry > 0 ? ry : 1));
    int k, n = 0;

    if (a1 >= a0) {     /* anticlockwise with y down: the angle falls */
        a1 -= 6.283185307179586;
    }

    if (how == 3) {
        xy[n++] = M->g.cx;
        xy[n++] = M->g.cy;
    }

    for (k = 0; k <= 64; k++) {
        double a = a0 + (a1 - a0) * k / 64;

        xy[n++] = cx + rx * cos(a);
        xy[n++] = cy + ry * sin(a);
    }

    if (how == 2) {
        xy[n++] = cx;
        xy[n++] = cy;
    }

    {   /* and as curves: the line to it (ArcTo), the arc, the line through the centre (a pie) */
        double cv[2 * 24];
        int nc = 0;

        if (how == 3) {
            cv[nc++] = M->g.cx;
            cv[nc++] = M->g.cy;
        }

        nc += 2 * arc_cv(cx, cy, rx, ry, a0, a1, 1, cv + nc);

        if (how == 2) {
            cv[nc++] = cx;
            cv[nc++] = cy;
        }

        if (how == 3 && M->in_path && M->fig_open) {    /* goes on from where the figure is */
            figure_cv(M, xy + 2, n / 2 - 1, 0, 0, 1, cv + 2, nc / 2 - 1);
        } else {
            figure_cv(M, xy, n / 2, how == 1 || how == 2, how == 1 || how == 2, 1, cv, nc / 2);
        }
    }

    if (how == 3) {
        M->g.cx = xy[n - 2];
        M->g.cy = xy[n - 1];
    }
}

/* a GRADIENTFILL rectangle (logical units): from colour c0 at its left (top, when down) to c1 at its right */
static void grad_rect(mf* M, double l, double t, double r, double b, uint32_t c0, uint32_t c1, int down) {
    double x[4] = { l, r, r, l }, y[4] = { t, t, b, b }, q[8];
    int k;

    if (M->in_path || M->mute) {
        return;
    }

    for (k = 0; k < 4; k++) {
        map_pt(M, x[k], y[k], &q[2 * k], &q[2 * k + 1]);
    }

    sep(M);
    pb_printf(M->o, "{\"path\":[%d,%d,%d,%d,%d,%d,%d,%d],\"closed\":1,\"fill\":%u,\"line\":0,\"lw\":0,\"gk\":1,"
              "\"f2\":%u,\"ga\":%d,\"gs\":[0,%u,100000,%u]}", (int)q[0], (int)q[1], (int)q[2], (int)q[3], (int)q[4], (int)q[5],
              (int)q[6], (int)q[7], (unsigned)c0, (unsigned)c1, down ? 5400000 : 0, (unsigned)c0, (unsigned)c1);
}

static void rect(mf* M, double l, double t, double r, double b) {
    double xy[8] = { l, t, r, t, r, b, l, b };

    figure(M, xy, 4, 1, 1, 1);
}

/* ------------------------------------------------------------------ */
/* bitmaps                                                            */
/* ------------------------------------------------------------------ */

static void be32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void png_chunk(pd_buf* b, const char* type, const unsigned char* data, size_t n) {
    unsigned char h[8];
    unsigned char* tmp = (unsigned char*)malloc(n + 4);

    if (!tmp) {
        b->err = 1;
        return;
    }

    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    pb_put(b, h, 8);
    memcpy(tmp, type, 4);

    if (n) {    /* IEND has no data at all */
        memcpy(tmp + 4, data, n);
        pb_put(b, data, n);
    }
    be32(h, pd_crc32(tmp, n + 4));
    pb_put(b, h, 4);
    free(tmp);
}

/* RGBA rows, top down, as a PNG resource of the document */
static pd_res_id png_resource(pd_doc* d, const unsigned char* rgba, int w, int h) {
    size_t row = (size_t)w * 4 + 1, k;
    unsigned char* raw = (unsigned char*)malloc(row * (size_t)h), *z = NULL, ihdr[13];
    size_t zn = 0;
    pd_buf b;
    pd_res_id res = 0;
    int y;

    if (!raw) {
        return 0;
    }

    for (y = 0; y < h; y++) {
        raw[row * (size_t)y] = 0;
        memcpy(raw + row * (size_t)y + 1, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
    }

    memset(&b, 0, sizeof(b));

    if (pd_deflate(raw, row * (size_t)h, 1, &z, &zn) == 0) {
        pb_put(&b, "\x89PNG\r\n\x1a\n", 8);
        be32(ihdr, (uint32_t)w);
        be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;
        ihdr[9] = 6;    /* RGBA */
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        png_chunk(&b, "IHDR", ihdr, 13);
        png_chunk(&b, "IDAT", z, zn);
        png_chunk(&b, "IEND", NULL, 0);

        if (!b.err && pd_doc_add_resource(d, "image/png", b.p, b.n, &res) != PD_OK) {
            res = 0;
        }
    }

    (void)k;
    free(z);
    free(raw);
    pb_free(&b);
    return res;
}

/* a DIB (BITMAPINFO and bits) as a picture resource: decoded to PNG, or a PNG or JPEG it holds */
static pd_res_id dib_resource(pd_doc* d, const unsigned char* bmi, size_t nbmi, const unsigned char* bits,
                              size_t nbits) {
    int32_t w, h, top = 0, y, x;
    int bpp, comp, ncol, stride;
    const unsigned char* pal;
    unsigned char* rgba;
    pd_res_id res = 0;
    int any_alpha = 0;

    if (nbmi < 40 || u32(bmi) < 40) {
        return 0;
    }

    w = s32(bmi + 4);
    h = s32(bmi + 8);
    bpp = u16(bmi + 14);
    comp = (int)u32(bmi + 16);
    ncol = (int)u32(bmi + 32);

    if (comp == 4 || comp == 5) {   /* the bits are a JPEG or a PNG */
        return pd_doc_add_resource(d, comp == 4 ? "image/jpeg" : "image/png", bits, nbits, &res) == PD_OK ? res : 0;
    }

    if (h < 0) {
        h = -h;
        top = 1;
    }

    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || (int64_t)w * h > 64 * 1024 * 1024 ||
            !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32) || (comp != 0 && comp != 3)) {
        return 0;
    }

    if (ncol == 0 && bpp <= 8) {
        ncol = 1 << bpp;
    }

    pal = bmi + u32(bmi);

    if (bpp <= 8 && (size_t)(pal - bmi) + (size_t)ncol * 4 > nbmi) {
        return 0;
    }

    stride = ((w * bpp + 31) / 32) * 4;

    if ((size_t)stride * (size_t)h > nbits || (rgba = (unsigned char*)malloc((size_t)w * h * 4)) == NULL) {
        return 0;
    }

    for (y = 0; y < h; y++) {
        const unsigned char* r = bits + (size_t)stride * (size_t)(top ? y : h - 1 - y);
        unsigned char* o = rgba + (size_t)y * w * 4;

        for (x = 0; x < w; x++, o += 4) {
            uint32_t v;

            switch (bpp) {
                case 1:
                case 4:
                case 8: {
                    int i = bpp == 8 ? r[x] : bpp == 4 ? (r[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (r[x / 8] >> (7 - (x & 7))) & 1;

                    i = i < ncol ? i : 0;
                    o[0] = pal[4 * i + 2];
                    o[1] = pal[4 * i + 1];
                    o[2] = pal[4 * i];
                    o[3] = 255;
                    break;
                }

                case 16:
                    v = u16(r + 2 * x);
                    o[0] = (unsigned char)(((v >> 10) & 31) * 255 / 31);
                    o[1] = (unsigned char)(((v >> 5) & 31) * 255 / 31);
                    o[2] = (unsigned char)((v & 31) * 255 / 31);
                    o[3] = 255;
                    break;

                case 24:
                    o[0] = r[3 * x + 2];
                    o[1] = r[3 * x + 1];
                    o[2] = r[3 * x];
                    o[3] = 255;
                    break;

                default:
                    o[0] = r[4 * x + 2];
                    o[1] = r[4 * x + 1];
                    o[2] = r[4 * x];
                    o[3] = r[4 * x + 3];
                    any_alpha |= o[3] != 0;
            }
        }
    }

    if (bpp == 32 && !any_alpha) {  /* no alpha at all: opaque */
        for (y = 0; y < w * h; y++) {
            rgba[4 * y + 3] = 255;
        }
    }

    res = png_resource(d, rgba, w, h);
    free(rgba);
    return res;
}

/* Raster operations: which put a bitmap down as it is. GDI's transparency
   is a 1-bit mask ORed in (SRCPAINT) and the picture ANDed over it
   (SRCAND): on a white page the picture alone is what shows, so the mask
   is left out and the AND drawn. Other effects are left out too. */
static int rop_draws(uint32_t rop, const unsigned char* bmi, size_t nbmi) {
    int bpp = nbmi >= 16 ? u16(bmi + 14) : 0;

    rop >>= 16;     /* the operation's index */
    return rop == 0xCC || rop == 0x88 || rop == 0x66 || rop == 0xC0 || (rop == 0xEE && bpp > 1);
}

/* a bitmap drawn into a rectangle given in logical units */
static void out_bitmap(mf* M, pd_res_id res, double x, double y, double w, double h) {
    double ax, ay, bx, by;

    if (!res || M->mute) {
        return;
    }

    map_pt(M, x, y, &ax, &ay);
    map_pt(M, x + w, y + h, &bx, &by);
    sep(M);
    pb_printf(M->o, "{\"img\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", (int)res, (int)(ax < bx ? ax : bx),
              (int)(ay < by ? ay : by), (int)fabs(bx - ax), (int)fabs(by - ay));
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

/* UTF-16LE to UTF-8 into o, escaped for JSON */
static void json_utf16(pd_buf* o, const unsigned char* s, size_t nchars) {
    size_t i;

    pb_putc(o, '"');

    for (i = 0; i < nchars; i++) {
        uint32_t c = u16(s + 2 * i);

        if (c >= 0xD800 && c < 0xDC00 && i + 1 < nchars) {
            uint32_t c2 = u16(s + 2 * i + 2);

            if (c2 >= 0xDC00 && c2 < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i++;
            }
        }

        if (c == '"' || c == '\\') {
            pb_putc(o, '\\');
            pb_putc(o, (char)c);
        } else if (c < 32) {
            pb_putc(o, ' ');
        } else if (c < 0x80) {
            pb_putc(o, (char)c);
        } else if (c < 0x800) {
            pb_putc(o, (char)(0xC0 | (c >> 6)));
            pb_putc(o, (char)(0x80 | (c & 63)));
        } else if (c < 0x10000) {
            pb_putc(o, (char)(0xE0 | (c >> 12)));
            pb_putc(o, (char)(0x80 | ((c >> 6) & 63)));
            pb_putc(o, (char)(0x80 | (c & 63)));
        } else {
            pb_putc(o, (char)(0xF0 | (c >> 18)));
            pb_putc(o, (char)(0x80 | ((c >> 12) & 63)));
            pb_putc(o, (char)(0x80 | ((c >> 6) & 63)));
            pb_putc(o, (char)(0x80 | (c & 63)));
        }
    }

    pb_putc(o, '"');
}

/* a line of text at a reference point (logical), in the font and colour in use */
static void out_text(mf* M, double x, double y, const unsigned char* s, size_t nchars, int utf16) {
    const gobj* f = &M->g.font;
    double em = f->kind == 3 ? (f->height < 0 ? -f->height : f->height * 0.85) : 12, ox, oy, sz;
    int al = M->g.text_align;
    size_t i;
    int blank = 1;

    for (i = 0; i < nchars; i++) {
        uint32_t c = utf16 ? u16(s + 2 * i) : s[i];

        blank &= c == ' ' || c == 0;
    }

    if (nchars == 0 || blank || M->mute) {
        return;
    }

    /* the baseline from the reference point: top, bottom or the baseline itself -- down the text, which
       may be turned (its escapement) */
    {
        double sh = (al & 24) == 24 ? 0 : (al & 8) ? -em * 0.21 : em * 0.9;
        double th = f->kind == 3 ? f->esc / 10.0 * 3.14159265358979323846 / 180 : 0;

        x += sh * sin(th);
        y += sh * cos(th);
    }

    map_pt(M, x, y, &ox, &oy);
    sz = map_len(M, em);

    if (M->g.clip && (ox < M->g.clip_x0 - 1 || ox > M->g.clip_x1 + 1 || oy < M->g.clip_y0 - 1 ||
                      oy > M->g.clip_y1 + sz)) {
        return;     /* its reference point outside the clip: cut off */
    }

    sep(M);
    pb_puts(M->o, "{\"label\":");

    if (utf16) {
        json_utf16(M->o, s, nchars);
    } else {
        unsigned char* w = (unsigned char*)malloc(nchars * 2);

        if (w) {    /* Windows-1252, near enough as Latin-1 */
            for (i = 0; i < nchars; i++) {
                w[2 * i] = s[i];
                w[2 * i + 1] = 0;
            }

            json_utf16(M->o, w, nchars);
            free(w);
        } else {
            pb_puts(M->o, "\"\"");
        }
    }

    pb_printf(M->o, ",\"x\":%d,\"y\":%d,\"sz\":%d,\"w\":%d,\"i\":%d,\"c\":%u,\"ha\":%d", (int)ox, (int)oy,
              (int)(sz > 65536 ? sz : 65536), f->kind == 3 && f->weight > 0 ? f->weight : 400, f->kind == 3 && f->italic,
              (unsigned)M->g.text_color, (al & 6) == 6 ? 1 : (al & 6) == 2 ? 2 : 0);

    {   /* turned: the baseline's way (the escapement's, anticlockwise) through the transforms, clockwise here */
        double th = f->kind == 3 ? f->esc / 10.0 * 3.14159265358979323846 / 180 : 0, bx, by, deg;

        map_pt(M, x + 100 * cos(th), y - 100 * sin(th), &bx, &by);
        deg = atan2(by - oy, bx - ox) * 180 / 3.14159265358979323846;

        if (fabs(deg) > 0.5) {
            pb_printf(M->o, ",\"rot\":%d", (int)lround((deg < 0 ? deg + 360 : deg) * 60000));
        }
    }

    if (f->kind == 3 && f->face[0]) {
        size_t k;

        pb_puts(M->o, ",\"f\":\"");

        for (k = 0; f->face[k]; k++) {
            if (f->face[k] != '"' && f->face[k] != '\\') {
                pb_putc(M->o, f->face[k]);
            }
        }

        pb_putc(M->o, '"');
    }

    pb_putc(M->o, '}');
}

/* a face name in UTF-16, to ASCII-ish UTF-8 */
static void face_name(char* out, size_t cap, const unsigned char* s, size_t maxchars) {
    size_t i, k = 0;

    for (i = 0; i < maxchars && k + 1 < cap; i++) {
        uint32_t c = u16(s + 2 * i);

        if (!c) {
            break;
        }

        out[k++] = c < 0x80 ? (char)c : '?';
    }

    out[k] = '\0';
}

/* ------------------------------------------------------------------ */
/* objects and state                                                  */
/* ------------------------------------------------------------------ */

static void state_init(gstate* g) {
    memset(g, 0, sizeof(*g));
    g->wex = g->wey = g->vex = g->vey = 1;
    g->map = 1;
    g->m[0] = g->m[3] = 1;
    g->pen.kind = 1;
    g->pen.color = 0xFF000000u;
    g->brush.kind = 2;
    g->brush.color = 0xFFFFFFFFu;
    g->text_color = 0xFF000000u;
    g->bk_color = 0xFFFFFFFFu;
    g->bk_mode = 2;
}

static void select_stock(mf* M, uint32_t ih) {
    uint32_t n = ih & 0x7FFFFFFF;

    if (n <= 5) {   /* white, light grey, grey, dark grey, black, null brushes */
        static const uint32_t c[] = { 0xFFFFFFFF, 0xFFC0C0C0, 0xFF808080, 0xFF404040, 0xFF000000, 0 };

        memset(&M->g.brush, 0, sizeof(gobj));
        M->g.brush.kind = 2;
        M->g.brush.color = c[n];
        M->g.brush.null = n == 5;
    } else if (n <= 8) {    /* white, black, null pens */
        memset(&M->g.pen, 0, sizeof(gobj));
        M->g.pen.kind = 1;
        M->g.pen.color = n == 6 ? 0xFFFFFFFFu : 0xFF000000u;
        M->g.pen.null = n == 8;
    }
}

static void select_obj(mf* M, uint32_t ih) {
    if (ih & 0x80000000u) {
        select_stock(M, ih);
    } else if (ih < MAX_OBJ) {
        const gobj* o = &M->obj[ih];

        if (o->kind == 1) {
            M->g.pen = *o;
        } else if (o->kind == 2) {
            M->g.brush = *o;
        } else if (o->kind == 3) {
            M->g.font = *o;
        }
    }
}

/* the path built so far, drawn */
static void path_out(mf* M, int fill, int stroke) {
    if (M->pcv_cubic && M->npcv >= 4) {
        M->cur_cv = M->pcv;
        M->cur_ncv = M->npcv / 2;
    }

    out_path(M, M->pp, M->npp / 2, 1, fill, stroke);
    M->cur_cv = NULL;
    M->npp = 0;
    M->npcv = 0;
    M->pcv_cubic = 0;
    M->fig_open = 0;
}

/* ------------------------------------------------------------------ */
/* EMF                                                                */
/* ------------------------------------------------------------------ */

/* points of a poly record: 32-bit or 16-bit pairs after a count */
static double* read_pts(const unsigned char* p, size_t avail, uint32_t n, int small) {
    double* xy;
    uint32_t i;

    if (n == 0 || n > 1000000 || (size_t)n * (small ? 4 : 8) > avail ||
            (xy = (double*)malloc((size_t)n * 2 * sizeof(double))) == NULL) {
        return NULL;
    }

    for (i = 0; i < n; i++) {
        xy[2 * i] = small ? s16(p + 4 * i) : s32(p + 8 * i);
        xy[2 * i + 1] = small ? s16(p + 4 * i + 2) : s32(p + 8 * i + 4);
    }

    return xy;
}

/* poly-Bezier points (the first being the start unless "to") as a flattened polyline */
static double* flatten(const double* pts, uint32_t n, double sx, double sy, int to, int* out_n) {
    uint32_t segs = to ? n / 3 : (n - 1) / 3, i;
    double* xy = (double*)malloc(((size_t)segs * 16 + 1) * 2 * sizeof(double));
    double cur[2];
    int k = 0;

    if (!xy) {
        return NULL;
    }

    cur[0] = to ? sx : pts[0];
    cur[1] = to ? sy : pts[1];
    xy[k++] = cur[0];
    xy[k++] = cur[1];

    for (i = 0; i < segs; i++) {
        const double* q = pts + (to ? 6 * i : 2 + 6 * i);
        double p[8] = { cur[0], cur[1], q[0], q[1], q[2], q[3], q[4], q[5] };

        k += 2 * bezier(xy + k, p);
        cur[0] = q[4];
        cur[1] = q[5];
    }

    *out_n = k / 2;
    return xy;
}

/* ------------------------------------------------------------------ */
/* EMF+                                                               */
/* ------------------------------------------------------------------ */

/* A metafile GDI+ wrote keeps its drawing as EMF+ records in comments. In
   a dual one the plain records beside them say much the same -- but not
   what GDI cannot: a picture or a metafile GDI+ drew inside it, which is
   left out of them -- and the plain records after a GetDC are drawn by
   GDI itself; an EMF+ only one has nothing else. As Office does, the EMF+
   records are played when there are any and all of them are ones played
   here, the plain ones then only after a GetDC; otherwise the plain
   records alone, as before. Points go through the world transform, the
   page transform (a unit and a scale) and then Base: the metafile's device
   units to the drawing's -- for a metafile drawn inside another, on to
   where it is drawn in that one. */

#define PLUS_OBJ 64

typedef struct {
    int kind;                   /* 0 none, 1 brush, 2 pen, 3 path, 4 region, 5 image, 6 font, 7 string format */
    uint32_t color;             /* brushes and pens: 0xAARRGGBB */
    double width;               /* pens */
    int unit;                   /* pens: their width's; fonts: their size's */
    int dash, cap, join;        /* pens: DashStyle (5 custom), LineCap, LineJoin */
    double dashes[16];
    int ndash;
    double* pts;                /* paths: x, y pairs; regions: their bounds (4) */
    unsigned char* types;
    int npts;
    double em;                  /* fonts */
    int style;
    char face[64];
    int align, lalign;          /* string formats: StringAlignment across and down */
    int empty, infinite;        /* regions */
    int grad;                   /* brushes: 1 a linear gradient, its stops (position in 100000ths, colour) */
    int nstop;
    int32_t stop_pos[16];
    uint32_t stop_col[16];
    double bt[6];               /* ... the brush's transform, and its rectangle's diagonal: the gradient's line */
    double gw, gh;
    pd_res_id res;              /* images: a picture */
    unsigned char* data;        /* images: a metafile (owned) */
    size_t ndata;
} pobj;

typedef struct pctx_s {
    double base[6];             /* device units to the drawing's */
    double world[6];
    int page_unit;
    double page_scale, dpix, dpiy;
    int clip0;                  /* the clip when the metafile began (one inside another: where it is drawn) */
    double c0[4];
    struct {
        uint32_t id;
        double world[6];
        int page_unit;
        double page_scale;
        int clip;
        double c[4];
    } stack[32];
    int nstack;
    pobj obj[PLUS_OBJ];
    unsigned char* cont;        /* an object coming in pieces */
    size_t ncont, totcont;
    int contid;
    const pobj* fillb;          /* the brush object of the record being played, when it is a gradient */
} pctx;

static void emf_play(mf* M, const unsigned char* p, size_t n);
static double* bez_cv(const double* pts, uint32_t n, double sx, double sy, int to);

static double f32(const unsigned char* p) {
    float f;

    memcpy(&f, p, 4);
    return isfinite(f) ? f : 0;
}

/* a after b: x -> a(b(x)) */
static void aff_mul(double* out, const double* a, const double* b) {
    double t[6];

    t[0] = a[0] * b[0] + a[2] * b[1];
    t[1] = a[1] * b[0] + a[3] * b[1];
    t[2] = a[0] * b[2] + a[2] * b[3];
    t[3] = a[1] * b[2] + a[3] * b[3];
    t[4] = a[0] * b[4] + a[2] * b[5] + a[4];
    t[5] = a[1] * b[4] + a[3] * b[5] + a[5];
    memcpy(out, t, sizeof(t));
}

static void aff_set(double* m, double a, double b, double c, double d, double e, double f) {
    m[0] = a;
    m[1] = b;
    m[2] = c;
    m[3] = d;
    m[4] = e;
    m[5] = f;
}

static void aff_pt(const double* m, double x, double y, double* ox, double* oy) {
    *ox = fin(m[0] * x + m[2] * y + m[4]);
    *oy = fin(m[1] * x + m[3] * y + m[5]);
}

static double aff_scale(const double* m) {
    return sqrt(fabs(m[0] * m[3] - m[1] * m[2]));
}

/* a unit's device pixels */
static double unit_px(const pctx* P, int unit, int vertical) {
    double dpi = vertical ? P->dpiy : P->dpix;

    switch (unit) {
        case 3:
            return dpi / 72;    /* point */

        case 4:
            return dpi;         /* inch */

        case 5:
            return dpi / 300;   /* document */

        case 6:
            return dpi / 25.4;  /* millimetre */

        default:
            return 1;           /* world, display, pixel */
    }
}

/* world to drawing units */
static void plus_mat(const pctx* P, double* m) {
    double pg[6];

    aff_set(pg, P->page_scale * unit_px(P, P->page_unit, 0), 0, 0, P->page_scale * unit_px(P, P->page_unit, 1), 0, 0);
    aff_mul(m, pg, P->world);
    aff_mul(m, P->base, m);
}

/* n points of a drawing record or a path: floats, 16-bit integers (C) or 7- and 15-bit steps from the last (R);
   *used, the bytes they took. NULL when they run past the end */
static double* plus_points(const unsigned char* p, size_t avail, uint32_t n, uint16_t flags, size_t* used) {
    double* xy;
    size_t at = 0;
    uint32_t i;
    int k;

    if (n == 0 || n > 4000000 || (xy = (double*)malloc((size_t)n * 2 * sizeof(double))) == NULL) {
        return NULL;
    }

    for (i = 0; i < n; i++) {
        if (flags & 0x0800) {   /* relative */
            for (k = 0; k < 2; k++) {
                int v;

                if (at >= avail || ((p[at] & 0x80) && at + 1 >= avail)) {
                    free(xy);
                    return NULL;
                }

                if (p[at] & 0x80) {
                    v = ((p[at] & 0x7F) << 8) | p[at + 1];
                    v = v & 0x4000 ? v - 0x8000 : v;
                    at += 2;
                } else {
                    v = p[at] & 0x7F;
                    v = v & 0x40 ? v - 0x80 : v;
                    at++;
                }

                xy[2 * i + k] = (i ? xy[2 * (i - 1) + k] : 0) + v;
            }
        } else if (flags & 0x4000) {
            if (at + 4 > avail) {
                free(xy);
                return NULL;
            }

            xy[2 * i] = s16(p + at);
            xy[2 * i + 1] = s16(p + at + 2);
            at += 4;
        } else {
            if (at + 8 > avail) {
                free(xy);
                return NULL;
            }

            xy[2 * i] = f32(p + at);
            xy[2 * i + 1] = f32(p + at + 4);
            at += 8;
        }
    }

    *used = at;
    return xy;
}

/* an EmfPlusBrush into o: its colour -- a hatch's fore colour, a path gradient's centre, a texture grey -- and a
   linear gradient's stops and way */
static void read_brush_obj(pobj* o, const unsigned char* p, size_t n) {
    o->kind = 1;
    o->color = 0;

    if (n < 12) {
        return;
    }

    switch (u32(p + 4)) {
        case 0:
            o->color = u32(p + 8);
            break;

        case 1:
            o->color = n >= 16 ? u32(p + 12) : 0;
            break;

        case 3:     /* a path gradient: from its centre colour out to the one around (or its colours between) */
            o->color = n >= 20 ? u32(p + 16) : 0;

            if (n >= 36) {
                uint32_t fl = u32(p + 8), ns = u32(p + 28), cnt;
                size_t at = 32 + 4 * (size_t)ns;
                uint32_t around = ns > 0 ? u32(p + 32) : o->color;
                int k;

                if (ns > 1000 || at + 4 > n) {
                    break;
                }

                at += (fl & 0x01) ? 4 + (size_t)u32(p + at) : 4 + 8 * (size_t)u32(p + at);     /* its boundary */

                if ((fl & 0x02) && at + 24 <= n) {
                    at += 24;
                }

                o->grad = 2;
                o->stop_pos[0] = 0;
                o->stop_col[0] = o->color;
                o->stop_pos[1] = 100000;
                o->stop_col[1] = around;
                o->nstop = 2;

                if ((fl & 0x04) && at + 4 <= n && (cnt = u32(p + at)) >= 2 && cnt <= 16 && at + 4 + 8 * (size_t)cnt <= n) {
                    for (k = 0; k < (int)cnt; k++) {    /* positions from the boundary in: turned round, from the centre */
                        o->stop_pos[cnt - 1 - k] = 100000 - (int32_t)lround(f32(p + at + 4 + 4 * k) * 100000);
                        o->stop_col[cnt - 1 - k] = u32(p + at + 4 + 4 * cnt + 4 * k);
                    }

                    o->nstop = (int)cnt;
                }
            }

            break;

        case 4:
            if (n >= 48) {
                uint32_t fl = u32(p + 8), c0 = u32(p + 32), c1 = u32(p + 36);
                size_t at = 48;
                int k;

                o->color = c0;
                o->grad = 1;
                o->gw = f32(p + 24);    /* from the rectangle's top left to its bottom right */
                o->gh = f32(p + 28);
                aff_set(o->bt, 1, 0, 0, 1, 0, 0);

                if ((fl & 0x02) && at + 24 <= n) {
                    aff_set(o->bt, f32(p + at), f32(p + at + 4), f32(p + at + 8), f32(p + at + 12), 0, 0);
                    at += 24;
                }

                if ((fl & 0x04) && at + 4 <= n) {   /* colours at positions */
                    uint32_t cnt = u32(p + at);

                    if (cnt >= 2 && cnt <= 16 && at + 4 + 8 * (size_t)cnt <= n) {
                        for (k = 0; k < (int)cnt; k++) {
                            o->stop_pos[k] = (int32_t)lround(f32(p + at + 4 + 4 * k) * 100000);
                            o->stop_col[k] = u32(p + at + 4 + 4 * cnt + 4 * k);
                        }

                        o->nstop = (int)cnt;
                    }
                } else if ((fl & 0x08) && at + 4 <= n) {    /* blend factors: how far to the end colour, where */
                    uint32_t cnt = u32(p + at);

                    if (cnt >= 2 && cnt <= 16 && at + 4 + 8 * (size_t)cnt <= n) {
                        for (k = 0; k < (int)cnt; k++) {
                            double f = f32(p + at + 4 + 4 * cnt + 4 * k);
                            uint32_t c = 0;
                            int sh;

                            f = f < 0 ? 0 : f > 1 ? 1 : f;

                            for (sh = 0; sh < 32; sh += 8) {
                                double a = (c0 >> sh) & 0xFF, b = (c1 >> sh) & 0xFF;

                                c |= (uint32_t)lround(a + (b - a) * f) << sh;
                            }

                            o->stop_pos[k] = (int32_t)lround(f32(p + at + 4 + 4 * k) * 100000);
                            o->stop_col[k] = c;
                        }

                        o->nstop = (int)cnt;
                    }
                }

                if (o->nstop == 0) {
                    o->stop_pos[0] = 0;
                    o->stop_col[0] = c0;
                    o->stop_pos[1] = 100000;
                    o->stop_col[1] = c1;
                    o->nstop = 2;
                }
            }

            break;

        default:
            o->color = 0xFF808080u;
            break;
    }
}

static uint32_t read_brush(const unsigned char* p, size_t n) {
    pobj o;

    memset(&o, 0, sizeof(o));
    read_brush_obj(&o, p, n);
    return o.color;
}

/* a drawing record's brush: a colour of its own (S) or a brush object's */
static uint32_t brush_color(pctx* P, uint32_t v, int inline_color) {
    if (inline_color) {
        return v;
    }

    if (v < PLUS_OBJ && P->obj[v].kind == 1 && P->obj[v].grad) {
        P->fillb = &P->obj[v];
    }

    return v < PLUS_OBJ && P->obj[v].kind == 1 ? P->obj[v].color : 0;
}

static void pobj_clear(pobj* o) {
    free(o->pts);
    free(o->types);
    free(o->data);
    memset(o, 0, sizeof(*o));
}

/* an EmfPlusPath into o: its points (x, y) and their types; 0 when it is cut short */
static int read_path(pobj* o, const unsigned char* p, size_t n) {
    uint32_t cnt, i;
    uint16_t fl;
    size_t used = 0, at;

    if (n < 12) {
        return 0;
    }

    cnt = u32(p + 4);
    fl = (uint16_t)u32(p + 8);
    o->kind = 3;

    if (cnt == 0) {
        return 1;
    }

    if ((o->pts = plus_points(p + 12, n - 12, cnt, fl, &used)) == NULL ||
            (o->types = (unsigned char*)malloc(cnt)) == NULL) {
        return 0;
    }

    at = 12 + used;

    for (i = 0; i < cnt;) {     /* the types: run-length encoded (0x1000), or a byte each */
        if (fl & 0x1000) {
            unsigned run;

            if (at + 2 > n) {
                return 0;
            }

            run = p[at] & 0x3F;

            while (run-- > 0 && i < cnt) {
                o->types[i++] = p[at + 1];
            }

            at += 2;
        } else {
            if (at >= n) {
                return 0;
            }

            o->types[i++] = p[at++];
        }
    }

    o->npts = (int)cnt;
    return 1;
}

static void bounds_of(const double* xy, int n, double* b) {
    int i;

    b[0] = b[2] = xy[0];
    b[1] = b[3] = xy[1];

    for (i = 1; i < n; i++) {
        b[0] = xy[2 * i] < b[0] ? xy[2 * i] : b[0];
        b[1] = xy[2 * i + 1] < b[1] ? xy[2 * i + 1] : b[1];
        b[2] = xy[2 * i] > b[2] ? xy[2 * i] : b[2];
        b[3] = xy[2 * i + 1] > b[3] ? xy[2 * i + 1] : b[3];
    }
}

/* a region's node at *at: its bounds (world units) into b, or *empty, or *infinite */
static void region_node(const unsigned char* p, size_t n, size_t* at, double* b, int* empty, int* infinite, int depth) {
    uint32_t t;

    *empty = *infinite = 0;
    b[0] = b[1] = b[2] = b[3] = 0;

    if (*at + 4 > n || depth > 32) {
        *empty = 1;
        return;
    }

    t = u32(p + *at);
    *at += 4;

    if (t == 0x10000000u) {     /* a rectangle */
        if (*at + 16 <= n) {
            b[0] = f32(p + *at);
            b[1] = f32(p + *at + 4);
            b[2] = b[0] + f32(p + *at + 8);
            b[3] = b[1] + f32(p + *at + 12);
        }

        *at += 16;
    } else if (t == 0x10000001u) {  /* a path: its points' bounds */
        uint32_t sz = *at + 4 <= n ? u32(p + *at) : 0;
        pobj o;

        memset(&o, 0, sizeof(o));

        if (*at + 4 <= n && sz <= n - *at - 4 && read_path(&o, p + *at + 4, sz) && o.npts > 0) {
            bounds_of(o.pts, o.npts, b);
        } else {
            *empty = 1;
        }

        pobj_clear(&o);
        *at += 4 + (size_t)sz;
    } else if (t == 0x10000002u) {
        *empty = 1;
    } else if (t == 0x10000003u) {
        *infinite = 1;
    } else if (t >= 1 && t <= 5) {  /* two combined: and, or; the others as the first */
        double l[4], r[4];
        int le, li, re, ri;

        region_node(p, n, at, l, &le, &li, depth + 1);
        region_node(p, n, at, r, &re, &ri, depth + 1);
        memcpy(b, l, sizeof(l));
        *empty = le;
        *infinite = li;

        if (t == 1) {
            if (le || re) {
                *empty = 1;
            } else if (li) {
                memcpy(b, r, sizeof(r));
                *infinite = ri;
            } else if (!ri) {
                b[0] = r[0] > b[0] ? r[0] : b[0];
                b[1] = r[1] > b[1] ? r[1] : b[1];
                b[2] = r[2] < b[2] ? r[2] : b[2];
                b[3] = r[3] < b[3] ? r[3] : b[3];
                *empty = b[2] <= b[0] || b[3] <= b[1];
            }
        } else if (t == 2) {
            if (li || ri) {
                *infinite = 1;
                *empty = 0;
            } else if (le) {
                memcpy(b, r, sizeof(r));
                *empty = re;
            } else if (!re) {
                b[0] = r[0] < b[0] ? r[0] : b[0];
                b[1] = r[1] < b[1] ? r[1] : b[1];
                b[2] = r[2] > b[2] ? r[2] : b[2];
                b[3] = r[3] > b[3] ? r[3] : b[3];
            }
        }
    } else {
        *empty = 1;
    }
}

/* an EmfPlusPen into o: width and unit, ends, corners, dashes, and its brush's colour */
static void read_pen(pobj* o, const unsigned char* p, size_t n) {
    uint32_t fl;
    size_t at = 20;

    o->kind = 2;
    o->color = 0xFF000000u;

    if (n < 20) {
        return;
    }

    fl = u32(p + 8);
    o->unit = (int)u32(p + 12);
    o->width = f32(p + 16);

    if (fl & 0x1) {     /* a transform */
        at += 24;
    }

    if ((fl & 0x2) && at + 4 <= n) {
        o->cap = s32(p + at);
        at += 4;
    }

    if ((fl & 0x4) && at + 4 <= n) {
        o->cap = s32(p + at);
        at += 4;
    }

    if ((fl & 0x8) && at + 4 <= n) {
        o->join = s32(p + at);
        at += 4;
    }

    if (fl & 0x10) {    /* miter limit */
        at += 4;
    }

    o->dash = (fl & 0x100) ? 5 : 0;     /* dashes given and no style: those dashes */

    if ((fl & 0x20) && at + 4 <= n) {
        o->dash = s32(p + at);
        at += 4;
    }

    if (fl & 0x40) {    /* dashes' ends */
        at += 4;
    }

    if (fl & 0x80) {    /* dashes' offset */
        at += 4;
    }

    if ((fl & 0x100) && at + 4 <= n) {
        uint32_t k, nd = u32(p + at);

        at += 4;

        for (k = 0; k < nd && at + 4 <= n; k++, at += 4) {
            if (o->ndash < 16) {
                o->dashes[o->ndash++] = f32(p + at);
            }
        }
    }

    if (fl & 0x200) {   /* alignment */
        at += 4;
    }

    if ((fl & 0x400) && at + 4 <= n) {  /* compound line */
        at += 4 + 4 * (size_t)u32(p + at);
    }

    if ((fl & 0x800) && at + 4 <= n) {  /* custom start cap */
        at += 4 + (size_t)u32(p + at);
    }

    if ((fl & 0x1000) && at + 4 <= n) { /* custom end cap */
        at += 4 + (size_t)u32(p + at);
    }

    if (at < n) {
        o->color = read_brush(p + at, n - at);
    }
}

/* an image object: a picture (a PNG or JPEG as it is, pixels made a PNG), or a metafile kept to play */
static void read_image(mf* M, pobj* o, const unsigned char* p, size_t n) {
    o->kind = 5;

    if (n >= 28 && u32(p + 4) == 1) {
        int32_t w = s32(p + 8), h = s32(p + 12), stride = s32(p + 16);
        uint32_t fmt = u32(p + 20), type = u32(p + 24);
        const unsigned char* b = p + 28;
        size_t nb = n - 28;
        int bp = fmt == 0x21808 ? 3 : 4;

        if (type == 1) {
            const char* mime = nb >= 8 && !memcmp(b, "\x89PNG", 4) ? "image/png" :
                               nb >= 3 && b[0] == 0xFF && b[1] == 0xD8 ? "image/jpeg" : NULL;

            if (mime && pd_doc_add_resource(M->d, mime, b, nb, &o->res) != PD_OK) {
                o->res = 0;
            }
        } else if (w > 0 && h > 0 && w < 20000 && h < 20000 && (fmt == 0x26200A || fmt == 0x22009 ||
                   fmt == 0xE200B || fmt == 0x21808) && stride >= w * bp && (size_t)stride * (size_t)h <= nb) {
            unsigned char* rgba = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
            int x, y;

            if (rgba) {
                for (y = 0; y < h; y++) {
                    for (x = 0; x < w; x++) {
                        const unsigned char* s = b + (size_t)y * (size_t)stride + (size_t)x * (size_t)bp;
                        unsigned char* d = rgba + ((size_t)y * (size_t)w + (size_t)x) * 4;

                        d[0] = s[2];
                        d[1] = s[1];
                        d[2] = s[0];
                        d[3] = bp == 4 && fmt != 0x22009 ? s[3] : 255;
                    }
                }

                o->res = png_resource(M->d, rgba, w, h);
                free(rgba);
            }
        }
    } else if (n >= 16 && u32(p + 4) == 2) {
        uint32_t sz = u32(p + 12);

        if (sz <= n - 16 && (o->data = (unsigned char*)malloc(sz ? sz : 1)) != NULL) {
            memcpy(o->data, p + 16, sz);
            o->ndata = sz;
        }
    }
}

static void read_object(mf* M, pctx* P, int id, int type, const unsigned char* p, size_t n) {
    pobj* o = &P->obj[id];

    pobj_clear(o);

    switch (type) {
        case 1:
            read_brush_obj(o, p, n);
            break;

        case 2:
            read_pen(o, p, n);
            break;

        case 3:
            if (!read_path(o, p, n)) {
                pobj_clear(o);
            }

            break;

        case 4:
            if (n >= 8 && (o->pts = (double*)malloc(4 * sizeof(double))) != NULL) {
                size_t at = 8;

                region_node(p, n, &at, o->pts, &o->empty, &o->infinite, 0);
                o->kind = 4;
            }

            break;

        case 5:
            read_image(M, o, p, n);
            break;

        case 6:
            if (n >= 24) {
                size_t nch = u32(p + 20) < (n - 24) / 2 ? u32(p + 20) : (n - 24) / 2;

                o->kind = 6;
                o->em = f32(p + 4);
                o->unit = (int)u32(p + 8);
                o->style = s32(p + 12);
                face_name(o->face, sizeof(o->face), p + 24, nch);
            }

            break;

        case 7:
            if (n >= 20) {
                o->kind = 7;
                o->align = (int)u32(p + 12);
                o->lalign = (int)u32(p + 16);
            }

            break;

        default:
            break;
    }
}

static void clip_back(mf* M, const pctx* P) {
    M->g.clip = P->clip0;
    M->g.clip_x0 = P->c0[0];
    M->g.clip_y0 = P->c0[1];
    M->g.clip_x1 = P->c0[2];
    M->g.clip_y1 = P->c0[3];
}

/* the clip set to a rectangle of world units (its bounds, as all clipping here) by a combine mode: replace
   (within the metafile's own), intersect, union; the others leave it */
static void plus_clip(mf* M, pctx* P, double x0, double y0, double x1, double y1, int mode) {
    double m[6], xy[8], b[4];

    plus_mat(P, m);
    aff_pt(m, x0, y0, &xy[0], &xy[1]);
    aff_pt(m, x1, y0, &xy[2], &xy[3]);
    aff_pt(m, x1, y1, &xy[4], &xy[5]);
    aff_pt(m, x0, y1, &xy[6], &xy[7]);
    bounds_of(xy, 4, b);

    if (mode == 0) {
        clip_back(M, P);
        clip_to(M, b[0], b[1], b[2], b[3], 0);
    } else if (mode == 1) {
        clip_to(M, b[0], b[1], b[2], b[3], 0);
    } else if (mode == 2 && M->g.clip) {
        clip_to(M, b[0] < M->g.clip_x0 ? b[0] : M->g.clip_x0, b[1] < M->g.clip_y0 ? b[1] : M->g.clip_y0,
                b[2] > M->g.clip_x1 ? b[2] : M->g.clip_x1, b[3] > M->g.clip_y1 ? b[3] : M->g.clip_y1, 1);
    }
}

/* a figure (world points; NAN pairs between rings) filled with a colour and/or outlined with a pen */
static void plus_figure_cv(mf* M, pctx* P, const double* wxy, int n, int closed, uint32_t fill, const pobj* pen,
                           const double* wcv, int ncv) {
    double m[6], *xy;
    gobj kb = M->g.brush, kp = M->g.pen;
    char dash[200], grad[600];
    int i;

    if (n < 2 || (!fill && (!pen || !(pen->color >> 24)))) {
        return;
    }

    if ((xy = (double*)malloc(((size_t)n + (size_t)(wcv ? ncv : 0)) * 2 * sizeof(double))) == NULL) {
        return;
    }

    plus_mat(P, m);

    for (i = 0; i < n; i++) {
        if (isnan(wxy[2 * i])) {
            xy[2 * i] = xy[2 * i + 1] = NAN;
        } else {
            aff_pt(m, wxy[2 * i], wxy[2 * i + 1], &xy[2 * i], &xy[2 * i + 1]);
        }
    }

    if (wcv && ncv >= 2) {  /* its curves as curves, through the same transforms */
        double* c = xy + 2 * n;

        for (i = 0; i < ncv; i++) {
            if (isnan(wcv[2 * i]) || isinf(wcv[2 * i])) {
                c[2 * i] = wcv[2 * i];
                c[2 * i + 1] = wcv[2 * i + 1];
            } else {
                aff_pt(m, wcv[2 * i], wcv[2 * i + 1], &c[2 * i], &c[2 * i + 1]);
            }
        }

        M->cur_cv = c;
        M->cur_ncv = ncv;
    }

    memset(&M->g.brush, 0, sizeof(gobj));
    memset(&M->g.pen, 0, sizeof(gobj));
    M->g.brush.kind = 2;
    M->g.brush.color = fill;
    M->g.brush.null = !fill;
    M->g.pen.kind = 1;
    M->g.pen.null = !pen;

    if (pen) {
        double w = pen->unit == 0 ? pen->width * aff_scale(m) : pen->width * unit_px(P, pen->unit, 0) *
                   aff_scale(P->base);
        int k, at = 0;

        M->g.pen.color = pen->color;
        /* GDI's pen style bits for it: geometric; round, square or flat ends; bevelled, round or mitred corners */
        M->g.pen.style = 0x10000 | (pen->cap == 2 ? 0 : pen->cap == 1 ? 0x100 : 0x200) |
                         (pen->join == 1 ? 0x1000 : pen->join == 2 ? 0 : 0x2000) |
                         (pen->dash >= 1 && pen->dash <= 4 ? (uint32_t)pen->dash : 0);
        M->lw_sp = w > 0 ? w : 0.5 * 65536;    /* 0: a hairline */

        if (pen->ndash > 0 && pen->dash != 0) {    /* its own dashes */
            for (k = 0; k < pen->ndash && at < (int)sizeof(dash) - 16; k++) {
                at += snprintf(dash + at, sizeof(dash) - (size_t)at, "%s%d", k ? "," : "",
                               (int)(pen->dashes[k] * 100000));
            }

            M->dash_sp = dash;
        }
    }

    if (fill && P->fillb && P->fillb->grad) {  /* a linear gradient: its stops, its way through the transforms */
        const pobj* b = P->fillb;
        double v[6], deg;
        int k, at;

        aff_mul(v, m, b->bt);
        deg = atan2(v[1] * b->gw + v[3] * b->gh, v[0] * b->gw + v[2] * b->gh) * 180 / 3.14159265358979323846;

        if (b->grad == 2) {     /* out from the middle of the shape to its edges */
            at = snprintf(grad, sizeof(grad), ",\"gk\":2,\"gp\":2,\"f2\":%u,\"gs\":[", (unsigned)b->stop_col[b->nstop - 1]);
        } else {
            at = snprintf(grad, sizeof(grad), ",\"gk\":1,\"f2\":%u,\"ga\":%d,\"gs\":[",
                          (unsigned)b->stop_col[b->nstop - 1], (int)lround((deg < 0 ? deg + 360 : deg) * 60000));
        }

        for (k = 0; k < b->nstop && at < (int)sizeof(grad) - 32; k++) {
            at += snprintf(grad + at, sizeof(grad) - (size_t)at, "%s%d,%u", k ? "," : "", (int)b->stop_pos[k],
                           (unsigned)b->stop_col[k]);
        }

        snprintf(grad + at, sizeof(grad) - (size_t)at, "]");
        M->g.brush.color = b->stop_col[0];
        M->grad_sp = grad;
    }

    M->mute = 0;
    out_path(M, xy, n, closed, fill != 0, pen != NULL);
    M->cur_cv = NULL;
    M->grad_sp = NULL;
    M->mute = 1;
    M->g.brush = kb;
    M->g.pen = kp;
    M->lw_sp = 0;
    M->dash_sp = NULL;
    free(xy);
}

static void plus_figure(mf* M, pctx* P, const double* wxy, int n, int closed, uint32_t fill, const pobj* pen) {
    plus_figure_cv(M, P, wxy, n, closed, fill, pen, NULL, 0);
}

/* a path object drawn: filled (its figures together, each closed) and/or outlined (each as it is) */
static void plus_path(mf* M, pctx* P, const pobj* path, uint32_t fill, const pobj* pen) {
    double* xy, *cv, cur[2] = { 0, 0 };
    int i, k = 0, start = 0, closed = 0, c = 0, cstart = 0, cubic = 0;

    if (!path || path->kind != 3 || path->npts <= 0) {
        return;
    }

    xy = (double*)malloc(((size_t)path->npts * 17 + 2) * 2 * sizeof(double));
    cv = (double*)malloc(((size_t)path->npts * 2 + 2) * 2 * sizeof(double));

    if (!xy || !cv) {
        free(xy);
        free(cv);
        return;
    }

    for (i = 0; i < path->npts; i++) {
        int t = path->types[i] & 7;
        const double* q = path->pts + 2 * i;

        if (t == 0 && i > 0) {  /* a new figure */
            if (pen) {
                plus_figure_cv(M, P, xy + 2 * start, k - start, closed, 0, pen, cubic ? cv + 2 * cstart : NULL,
                               c - cstart);
            }

            if (fill) {
                xy[2 * k] = xy[2 * k + 1] = NAN;
                k++;
                cv[2 * c] = cv[2 * c + 1] = NAN;
                c++;
            } else {
                k = 0;
                c = 0;
            }

            start = k;
            cstart = c;
            closed = 0;
        }

        if (t == 3 && i + 2 < path->npts) {     /* a Bezier: this point and the next two */
            double b[8] = { cur[0], cur[1], q[0], q[1], q[2], q[3], q[4], q[5] };

            k += bezier(xy + 2 * k, b);
            cv[2 * c] = INFINITY;
            cv[2 * c + 1] = 0;
            memcpy(cv + 2 * c + 2, q, 6 * sizeof(double));
            c += 4;
            cubic = 1;
            cur[0] = q[4];
            cur[1] = q[5];
            closed |= (path->types[i + 2] & 0x80) != 0;
            i += 2;
        } else {
            xy[2 * k] = q[0];
            xy[2 * k + 1] = q[1];
            k++;
            cv[2 * c] = q[0];
            cv[2 * c + 1] = q[1];
            c++;
            cur[0] = q[0];
            cur[1] = q[1];
            closed |= (path->types[i] & 0x80) != 0;
        }
    }

    if (pen) {
        plus_figure_cv(M, P, xy + 2 * start, k - start, closed, 0, pen, cubic ? cv + 2 * cstart : NULL, c - cstart);
    }

    if (fill) {
        plus_figure_cv(M, P, xy, k, 1, fill, NULL, cubic ? cv : NULL, c);
    }

    free(xy);
    free(cv);
}

static void plus_rect(mf* M, pctx* P, double x, double y, double w, double h, uint32_t fill, const pobj* pen) {
    double xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };

    plus_figure(M, P, xy, 4, 1, fill, pen);
}

static void plus_ellipse(mf* M, pctx* P, double x, double y, double w, double h, uint32_t fill, const pobj* pen) {
    double xy[128];
    int k;

    double cv[2 * 17];
    int nc;

    for (k = 0; k < 64; k++) {
        double a = k * 2 * 3.14159265358979323846 / 64;

        xy[2 * k] = x + w / 2 + w / 2 * cos(a);
        xy[2 * k + 1] = y + h / 2 + h / 2 * sin(a);
    }

    nc = arc_cv(x + w / 2, y + h / 2, w / 2, h / 2, 0, 2 * 3.14159265358979323846, 1, cv);
    plus_figure_cv(M, P, xy, 64, 1, fill, pen, cv, nc);
}

/* a string in a layout rectangle (world units), in a font and colour, placed as its format says; turned as the
   transform turns it */
static void plus_string(mf* M, pctx* P, const pobj* font, const pobj* fmt, uint32_t color, double rx, double ry,
                        double rw, double rh, const unsigned char* s, size_t nch) {
    double m[6], em, x, y, ox, oy, sz, rot;
    int al = fmt && fmt->kind == 7 ? fmt->align : 0, la = fmt && fmt->kind == 7 ? fmt->lalign : 0;
    size_t i;
    int blank = 1;

    for (i = 0; i < nch; i++) {
        blank &= u16(s + 2 * i) == ' ' || u16(s + 2 * i) == 0;
    }

    if (!font || font->kind != 6 || nch == 0 || blank || !(color >> 24)) {
        return;
    }

    plus_mat(P, m);
    /* the font's size in world units: as it is, or through its unit and the page transform back */
    em = font->unit == 0 || font->unit == 2 ? font->em : font->em * unit_px(P, font->unit, 1) /
         (P->page_scale * unit_px(P, P->page_unit, 1));
    x = al == 1 ? rx + rw / 2 : al == 2 ? rx + rw : rx;
    y = la == 1 && rh > 0 ? ry + rh / 2 + em * 0.33 : la == 2 && rh > 0 ? ry + rh - em * 0.25 : ry + em * 0.9;
    aff_pt(m, x, y, &ox, &oy);
    sz = em * aff_scale(m);
    rot = atan2(m[1], m[0]) * 180 / 3.14159265358979323846;

    if (M->g.clip && (ox < M->g.clip_x0 - sz || ox > M->g.clip_x1 + sz || oy < M->g.clip_y0 - sz ||
                      oy > M->g.clip_y1 + sz)) {
        return;     /* outside the clip: cut off */
    }

    sep(M);
    pb_puts(M->o, "{\"label\":");
    json_utf16(M->o, s, nch);
    pb_printf(M->o, ",\"x\":%d,\"y\":%d,\"sz\":%d,\"w\":%d,\"i\":%d,\"c\":%u,\"ha\":%d", (int)ox, (int)oy,
              (int)(sz > 65536 ? sz : 65536), font->style & 1 ? 700 : 400, (font->style & 2) != 0, (unsigned)color,
              al == 1 ? 1 : al == 2 ? 2 : 0);

    if (fabs(rot) > 0.5) {  /* turned (clockwise, the y axis running down) about its reference point */
        pb_printf(M->o, ",\"rot\":%d", (int)lround((rot < 0 ? rot + 360 : rot) * 60000));
    }

    if (font->face[0]) {
        size_t k;

        pb_puts(M->o, ",\"f\":\"");

        for (k = 0; font->face[k]; k++) {
            if (font->face[k] != '"' && font->face[k] != '\\') {
                pb_putc(M->o, font->face[k]);
            }
        }

        pb_putc(M->o, '"');
    }

    pb_putc(M->o, '}');
}

static void plus_play(mf* M, pctx* P, const unsigned char* p, size_t n);

/* an image drawn into a parallelogram (world points: where its top left, top right and bottom left go) from a
   source rectangle of it: a picture as the parallelogram's bounds, a metafile played inside it */
static void plus_image(mf* M, pctx* P, const pobj* img, double sx, double sy, double sw, double sh, const double* q) {
    double m[6], d[6], t[6];
    int k;

    if (!img || img->kind != 5 || sw == 0 || sh == 0) {
        return;
    }

    plus_mat(P, m);

    if (img->res) {
        double xy[8], b[4];

        for (k = 0; k < 3; k++) {
            aff_pt(m, q[2 * k], q[2 * k + 1], &xy[2 * k], &xy[2 * k + 1]);
        }

        xy[6] = xy[2] + xy[4] - xy[0];
        xy[7] = xy[3] + xy[5] - xy[1];
        bounds_of(xy, 4, b);
        M->mute = 0;
        sep(M);
        pb_printf(M->o, "{\"img\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", (int)img->res, (int)b[0], (int)b[1],
                  (int)(b[2] - b[0]), (int)(b[3] - b[1]));
        M->mute = 1;
        return;
    }

    if (img->data && pd_metafile_kind(img->data, img->ndata) == 1 && M->depth_mf < 4) {
        /* the metafile's device units from its frame's corner, as the source rectangle's; on to the
           parallelogram, then through this one's transforms */
        const unsigned char* h = img->data;
        double devx = s32(h + 72), devy = s32(h + 76), mmx = s32(h + 80), mmy = s32(h + 84);
        double pxx = mmx > 0 ? devx / mmx : 3.78, pxy = mmy > 0 ? devy / mmy : 3.78;
        mf* C;
        pctx* Q;

        aff_set(d, (q[2] - q[0]) / sw, (q[3] - q[1]) / sw, (q[4] - q[0]) / sh, (q[5] - q[1]) / sh, 0, 0);
        d[4] = q[0] - d[0] * sx - d[2] * sy;
        d[5] = q[1] - d[1] * sx - d[3] * sy;
        aff_set(t, 1, 0, 0, 1, -s32(h + 24) / 100.0 * pxx, -s32(h + 28) / 100.0 * pxy);
        aff_mul(d, d, t);

        if ((C = (mf*)calloc(1, sizeof(mf))) == NULL) {
            return;
        }

        if ((Q = (pctx*)calloc(1, sizeof(pctx))) == NULL) {
            free(C);
            return;
        }

        aff_mul(Q->base, m, d);
        C->d = M->d;
        C->o = M->o;
        C->first = M->first;
        state_init(&C->g);
        C->g.clip = M->g.clip;
        C->g.clip_x0 = M->g.clip_x0;
        C->g.clip_y0 = M->g.clip_y0;
        C->g.clip_x1 = M->g.clip_x1;
        C->g.clip_y1 = M->g.clip_y1;
        C->depth_mf = M->depth_mf + 1;
        /* its plain records, should it have no EMF+ ones: the same mapping, but for any turn */
        C->fsx = Q->base[0] ? Q->base[0] : 1;
        C->fsy = Q->base[3] ? Q->base[3] : 1;
        C->fx0 = -Q->base[4] / C->fsx;
        C->fy0 = -Q->base[5] / C->fsy;
        plus_play(C, Q, img->data, img->ndata);
        M->first = C->first;
        free(C->pp);
        free(C->pcv);
        free(C);
        free(Q);
    }
}

/* the EMF+ records of one comment */
static void plus_records(mf* M, pctx* P, const unsigned char* p, size_t n) {
    size_t at = 0;

    while (at + 12 <= n) {
        uint16_t type = u16(p + at), fl = u16(p + at + 2);
        uint32_t size = u32(p + at + 4), dsize = u32(p + at + 8);
        const unsigned char* r = p + at + 12;
        const pobj* pen = (fl & 0xFF) < PLUS_OBJ && P->obj[fl & 0xFF].kind == 2 ? &P->obj[fl & 0xFF] : NULL;

        if (size < 12 || size > n - at || dsize > size - 12) {
            break;
        }

        M->getdc = type == 0x4004;     /* the plain records that follow drawn, till the next EMF+ one */
        P->fillb = NULL;

        switch (type) {
            case 0x4001:    /* header: the resolution of its units */
                if (dsize >= 16) {
                    P->dpix = u32(r + 8) ? u32(r + 8) : 96;
                    P->dpiy = u32(r + 12) ? u32(r + 12) : 96;
                }

                break;

            case 0x4008: {  /* an object, perhaps in pieces */
                int id = fl & 0xFF, ot = (fl >> 8) & 0x7F;

                if (id >= PLUS_OBJ) {
                    break;
                }

                if (fl & 0x8000) {
                    uint32_t tot = dsize >= 4 ? u32(r) : 0;

                    if (!P->cont || P->contid != id) {
                        free(P->cont);
                        P->cont = tot > 0 && tot < 0x10000000u ? (unsigned char*)malloc(tot) : NULL;
                        P->ncont = 0;
                        P->totcont = tot;
                        P->contid = id;
                    }

                    if (P->cont && dsize >= 4) {
                        size_t k = dsize - 4 < P->totcont - P->ncont ? dsize - 4 : P->totcont - P->ncont;

                        memcpy(P->cont + P->ncont, r + 4, k);
                        P->ncont += k;

                        if (P->ncont >= P->totcont) {
                            read_object(M, P, id, ot, P->cont, P->ncont);
                            free(P->cont);
                            P->cont = NULL;
                        }
                    }
                } else {
                    read_object(M, P, id, ot, r, dsize);
                }

                break;
            }

            case 0x400A:    /* FillRects */
            case 0x400B: {  /* DrawRects */
                size_t hd = type == 0x400A ? 8 : 4, rs = (fl & 0x4000) ? 8 : 16;
                uint32_t cnt = dsize >= hd ? u32(r + hd - 4) : 0, k;
                uint32_t c = type == 0x400A ? brush_color(P, u32(r), (fl & 0x8000) != 0) : 0;

                if (type == 0x400B && !pen) {
                    break;
                }

                for (k = 0; k < cnt && hd + rs * ((size_t)k + 1) <= dsize; k++) {
                    const unsigned char* q = r + hd + rs * k;

                    if (fl & 0x4000) {
                        plus_rect(M, P, s16(q), s16(q + 2), s16(q + 4), s16(q + 6), c, type == 0x400B ? pen : NULL);
                    } else {
                        plus_rect(M, P, f32(q), f32(q + 4), f32(q + 8), f32(q + 12), c, type == 0x400B ? pen : NULL);
                    }
                }

                break;
            }

            case 0x400C:    /* FillPolygon */
            case 0x400D:    /* DrawLines */
            case 0x4019: {  /* DrawBeziers */
                size_t used = 0, hd = type == 0x400C ? 8 : 4;
                uint32_t cnt = dsize >= hd ? u32(r + hd - 4) : 0;
                double* xy = cnt ? plus_points(r + hd, dsize - hd, cnt, fl, &used) : NULL;

                if (xy) {
                    if (type == 0x400C) {
                        plus_figure(M, P, xy, (int)cnt, 1, brush_color(P, u32(r), (fl & 0x8000) != 0), NULL);
                    } else if (pen && type == 0x4019) {
                        int nf = 0;
                        double* f = flatten(xy, cnt, 0, 0, 0, &nf);

                        double* bc = bez_cv(xy, cnt, 0, 0, 0);

                        if (f) {
                            plus_figure_cv(M, P, f, nf, 0, 0, pen, bc, 1 + 4 * (int)((cnt - 1) / 3));
                            free(f);
                        }

                        free(bc);
                    } else if (pen) {
                        plus_figure(M, P, xy, (int)cnt, (fl & 0x2000) != 0, 0, pen);
                    }

                    free(xy);
                }

                break;
            }

            case 0x400E:    /* FillEllipse */
            case 0x400F: {  /* DrawEllipse */
                size_t hd = type == 0x400E ? 4 : 0;
                double x, y, w, h;

                if (dsize < hd + ((fl & 0x4000) ? 8 : 16)) {
                    break;
                }

                if (fl & 0x4000) {
                    x = s16(r + hd);
                    y = s16(r + hd + 2);
                    w = s16(r + hd + 4);
                    h = s16(r + hd + 6);
                } else {
                    x = f32(r + hd);
                    y = f32(r + hd + 4);
                    w = f32(r + hd + 8);
                    h = f32(r + hd + 12);
                }

                if (type == 0x400E) {
                    plus_ellipse(M, P, x, y, w, h, brush_color(P, u32(r), (fl & 0x8000) != 0), NULL);
                } else if (pen) {
                    plus_ellipse(M, P, x, y, w, h, 0, pen);
                }

                break;
            }

            case 0x4014:    /* FillPath */
                if (dsize >= 4 && (fl & 0xFF) < PLUS_OBJ) {
                    uint32_t c = brush_color(P, u32(r), (fl & 0x8000) != 0);

                    if (c) {
                        plus_path(M, P, &P->obj[fl & 0xFF], c, NULL);
                    }
                }

                break;

            case 0x4015:    /* DrawPath: the pen is named in the data, the path in the flags */
                if (dsize >= 4 && (fl & 0xFF) < PLUS_OBJ && u32(r) < PLUS_OBJ && P->obj[u32(r)].kind == 2) {
                    plus_path(M, P, &P->obj[fl & 0xFF], 0, &P->obj[u32(r)]);
                }

                break;

            case 0x401A:    /* DrawImage: into a rectangle */
                if (dsize >= 24 + ((fl & 0x4000) ? 8u : 16u) && (fl & 0xFF) < PLUS_OBJ) {
                    double x, y, w, h, q[6];

                    if (fl & 0x4000) {
                        x = s16(r + 24);
                        y = s16(r + 26);
                        w = s16(r + 28);
                        h = s16(r + 30);
                    } else {
                        x = f32(r + 24);
                        y = f32(r + 28);
                        w = f32(r + 32);
                        h = f32(r + 36);
                    }

                    q[0] = x;
                    q[1] = y;
                    q[2] = x + w;
                    q[3] = y;
                    q[4] = x;
                    q[5] = y + h;
                    plus_image(M, P, &P->obj[fl & 0xFF], f32(r + 8), f32(r + 12), f32(r + 16), f32(r + 20), q);
                }

                break;

            case 0x401B:    /* DrawImagePoints: into a parallelogram */
                if (dsize >= 28 && (fl & 0xFF) < PLUS_OBJ && u32(r + 24) == 3) {
                    size_t used = 0;
                    double* q = plus_points(r + 28, dsize - 28, 3, fl, &used);

                    if (q) {
                        plus_image(M, P, &P->obj[fl & 0xFF], f32(r + 8), f32(r + 12), f32(r + 16), f32(r + 20), q);
                        free(q);
                    }
                }

                break;

            case 0x401C:    /* DrawString */
                if (dsize >= 28 && (fl & 0xFF) < PLUS_OBJ) {
                    uint32_t fid = u32(r + 4), nch = u32(r + 8);

                    if ((size_t)nch * 2 <= dsize - 28) {
                        plus_string(M, P, &P->obj[fl & 0xFF], fid < PLUS_OBJ ? &P->obj[fid] : NULL,
                                    brush_color(P, u32(r), (fl & 0x8000) != 0), f32(r + 12), f32(r + 16), f32(r + 20),
                                    f32(r + 24), r + 28, nch);
                    }
                }

                break;

            case 0x4025:    /* Save */
            case 0x4027:    /* BeginContainer */
            case 0x4028:    /* BeginContainerNoParams */
                if (P->nstack < 32 && dsize >= (type == 0x4027 ? 36u : 4u)) {
                    int k = P->nstack++;

                    P->stack[k].id = u32(r + (type == 0x4027 ? 32 : 0));
                    memcpy(P->stack[k].world, P->world, sizeof(P->world));
                    P->stack[k].page_unit = P->page_unit;
                    P->stack[k].page_scale = P->page_scale;
                    P->stack[k].clip = M->g.clip;
                    P->stack[k].c[0] = M->g.clip_x0;
                    P->stack[k].c[1] = M->g.clip_y0;
                    P->stack[k].c[2] = M->g.clip_x1;
                    P->stack[k].c[3] = M->g.clip_y1;

                    if (type == 0x4027) {   /* the source rectangle onto the destination */
                        double sw = f32(r + 24), sh = f32(r + 28), t[6];

                        if (sw != 0 && sh != 0) {
                            aff_set(t, f32(r + 8) / sw, 0, 0, f32(r + 12) / sh, 0, 0);
                            t[4] = f32(r) - t[0] * f32(r + 16);
                            t[5] = f32(r + 4) - t[3] * f32(r + 20);
                            aff_mul(P->world, P->world, t);
                        }
                    }
                }

                break;

            case 0x4026:    /* Restore */
            case 0x4029:    /* EndContainer */
                if (dsize >= 4) {
                    int k;

                    for (k = P->nstack - 1; k >= 0 && P->stack[k].id != u32(r); k--) {
                    }

                    if (k >= 0) {
                        memcpy(P->world, P->stack[k].world, sizeof(P->world));
                        P->page_unit = P->stack[k].page_unit;
                        P->page_scale = P->stack[k].page_scale;
                        M->g.clip = P->stack[k].clip;
                        M->g.clip_x0 = P->stack[k].c[0];
                        M->g.clip_y0 = P->stack[k].c[1];
                        M->g.clip_x1 = P->stack[k].c[2];
                        M->g.clip_y1 = P->stack[k].c[3];
                        P->nstack = k;
                    }
                }

                break;

            case 0x402A:    /* SetWorldTransform */
                if (dsize >= 24) {
                    aff_set(P->world, f32(r), f32(r + 4), f32(r + 8), f32(r + 12), f32(r + 16), f32(r + 20));
                }

                break;

            case 0x402B:    /* ResetWorldTransform */
                aff_set(P->world, 1, 0, 0, 1, 0, 0);
                break;

            case 0x402C:    /* MultiplyWorldTransform */
            case 0x402D:    /* TranslateWorldTransform */
            case 0x402E:    /* ScaleWorldTransform */
            case 0x402F: {  /* RotateWorldTransform */
                double t[6];

                if (type == 0x402C && dsize >= 24) {
                    aff_set(t, f32(r), f32(r + 4), f32(r + 8), f32(r + 12), f32(r + 16), f32(r + 20));
                } else if (type == 0x402D && dsize >= 8) {
                    aff_set(t, 1, 0, 0, 1, f32(r), f32(r + 4));
                } else if (type == 0x402E && dsize >= 8) {
                    aff_set(t, f32(r), 0, 0, f32(r + 4), 0, 0);
                } else if (type == 0x402F && dsize >= 4) {
                    double a = f32(r) * 3.14159265358979323846 / 180;

                    aff_set(t, cos(a), sin(a), -sin(a), cos(a), 0, 0);
                } else {
                    break;
                }

                if (fl & 0x2000) {  /* appended: after the transform there is */
                    aff_mul(P->world, t, P->world);
                } else {            /* prepended: before it */
                    aff_mul(P->world, P->world, t);
                }

                break;
            }

            case 0x4030:    /* SetPageTransform */
                if (dsize >= 4) {
                    P->page_unit = fl & 0xFF;
                    P->page_scale = f32(r) != 0 ? f32(r) : 1;
                }

                break;

            case 0x4031:    /* ResetClip */
                clip_back(M, P);
                break;

            case 0x4032:    /* SetClipRect */
                if (dsize >= 16) {
                    plus_clip(M, P, f32(r), f32(r + 4), f32(r) + f32(r + 8), f32(r + 4) + f32(r + 12), (fl >> 8) & 15);
                }

                break;

            case 0x4033:    /* SetClipPath: to its bounds */
            case 0x4034: {  /* SetClipRegion: to its bounds */
                const pobj* o = (fl & 0xFF) < PLUS_OBJ ? &P->obj[fl & 0xFF] : NULL;
                int mode = (fl >> 8) & 15;
                double b[4];

                if (o && o->kind == 3 && o->npts > 0) {
                    bounds_of(o->pts, o->npts, b);
                    plus_clip(M, P, b[0], b[1], b[2], b[3], mode);
                } else if (o && o->kind == 4 && o->empty) {
                    plus_clip(M, P, 0, 0, 0, 0, mode);
                } else if (o && o->kind == 4 && !o->infinite) {
                    plus_clip(M, P, o->pts[0], o->pts[1], o->pts[2], o->pts[3], mode);
                } else if (o && o->kind == 4 && mode == 0) {
                    clip_back(M, P);
                }

                break;
            }

            case 0x4035:    /* OffsetClip */
                if (dsize >= 8 && M->g.clip) {
                    double m[6], dx = f32(r), dy = f32(r + 4);

                    plus_mat(P, m);
                    M->g.clip_x0 += m[0] * dx + m[2] * dy;
                    M->g.clip_x1 += m[0] * dx + m[2] * dy;
                    M->g.clip_y0 += m[1] * dx + m[3] * dy;
                    M->g.clip_y1 += m[1] * dx + m[3] * dy;
                }

                break;

            default:
                break;
        }

        at += size;
    }
}

/* the EMF+ record types played above, and those that draw nothing (settings, comments, terminal server's) */
static int plus_known(uint16_t t) {
    return (t >= 0x4001 && t <= 0x4004) || (t >= 0x4008 && t <= 0x400F) || t == 0x4014 || t == 0x4015 ||
           (t >= 0x4019 && t <= 0x4035) || (t >= 0x4038 && t <= 0x403A);
}

/* 1 when a metafile has EMF+ records and all are known: they are played then, not the plain ones */
static int plus_usable(const unsigned char* p, size_t n) {
    size_t at = 0;
    int any = 0;

    while (at + 8 <= n) {
        uint32_t type = u32(p + at), size = u32(p + at + 4);

        if (size < 8 || size > n - at) {
            break;
        }

        if (type == 70 && size >= 16 && u32(p + at + 12) == 0x2B464D45u) {     /* a comment, "EMF+" */
            size_t q = 16, e = 12 + (size_t)u32(p + at + 8) < size ? 12 + (size_t)u32(p + at + 8) : size;

            while (q + 12 <= e) {
                uint32_t rs = u32(p + at + q + 4);

                if (rs < 12 || rs > e - q) {
                    break;
                }

                if (!plus_known(u16(p + at + q))) {
                    return 0;
                }

                any |= u16(p + at + q) == 0x4001;
                q += rs;
            }
        }

        if (type == 14) {
            break;
        }

        at += size;
    }

    return any;
}

/* a metafile played: its EMF+ records when they can be, else its plain ones */
static void plus_play(mf* M, pctx* P, const unsigned char* p, size_t n) {
    int k;

    P->clip0 = M->g.clip;
    P->c0[0] = M->g.clip_x0;
    P->c0[1] = M->g.clip_y0;
    P->c0[2] = M->g.clip_x1;
    P->c0[3] = M->g.clip_y1;
    aff_set(P->world, 1, 0, 0, 1, 0, 0);
    P->page_unit = 2;
    P->page_scale = 1;
    P->dpix = P->dpiy = 96;
    M->plus = plus_usable(p, n) ? P : NULL;
    M->getdc = 0;
    emf_play(M, p, n);
    M->plus = NULL;
    M->mute = 0;

    for (k = 0; k < PLUS_OBJ; k++) {
        pobj_clear(&P->obj[k]);
    }

    free(P->cont);
    P->cont = NULL;
}

/* poly-Bezier points as the cv encoding: the start, then (INFINITY, 0) and three points a cubic */
static double* bez_cv(const double* pts, uint32_t n, double sx, double sy, int to) {
    uint32_t segs = to ? n / 3 : (n - 1) / 3, i;
    double* cv = (double*)malloc(((size_t)segs * 4 + 1) * 2 * sizeof(double));
    int k = 0;

    if (!cv) {
        return NULL;
    }

    cv[k++] = to ? sx : pts[0];
    cv[k++] = to ? sy : pts[1];

    for (i = 0; i < segs; i++) {
        const double* q = pts + (to ? 6 * i : 2 + 6 * i);

        cv[k++] = INFINITY;
        cv[k++] = 0;
        memcpy(cv + k, q, 6 * sizeof(double));
        k += 6;
    }

    return cv;
}

static void emf_play(mf* M, const unsigned char* p, size_t n) {
    size_t at = 0;

    while (at + 8 <= n) {
        uint32_t type = u32(p + at), size = u32(p + at + 4);
        const unsigned char* r = p + at;

        if (size < 8 || size > n - at || (size & 3)) {
            break;
        }

        if (M->plus && type == 70 && size >= 16 && u32(r + 8) >= 4 && u32(r + 12) == 0x2B464D45u) {   /* EMF+ */
            plus_records(M, M->plus, r + 16, (u32(r + 8) < size - 12 ? u32(r + 8) : size - 12) - 4);
            at += size;
            continue;
        }

        M->mute = M->plus && !M->getdc;    /* the plain records beside EMF+ ones: only what follows a GetDC drawn */

        switch (type) {
            case 9:     /* SETWINDOWEXTEX */
                if (size >= 16) { M->g.wex = s32(r + 8); M->g.wey = s32(r + 12); }
                break;

            case 10:    /* SETWINDOWORGEX */
                if (size >= 16) { M->g.wox = s32(r + 8); M->g.woy = s32(r + 12); }
                break;

            case 11:    /* SETVIEWPORTEXTEX */
                if (size >= 16) { M->g.vex = s32(r + 8); M->g.vey = s32(r + 12); }
                break;

            case 12:    /* SETVIEWPORTORGEX */
                if (size >= 16) { M->g.vox = s32(r + 8); M->g.voy = s32(r + 12); }
                break;

            case 17:    /* SETMAPMODE */
                if (size >= 12) { M->g.map = (int)u32(r + 8); }
                break;

            case 22:    /* SETTEXTALIGN */
                if (size >= 12) { M->g.text_align = (int)u32(r + 8); }
                break;

            case 24:    /* SETTEXTCOLOR */
                if (size >= 12) { M->g.text_color = colorref(u32(r + 8)); }
                break;

            case 25:    /* SETBKCOLOR */
                if (size >= 12) { M->g.bk_color = colorref(u32(r + 8)); }
                break;

            case 18:    /* SETBKMODE */
                if (size >= 12) { M->g.bk_mode = (int)u32(r + 8); }
                break;

            case 19:    /* SETPOLYFILLMODE */
                if (size >= 12) { M->g.poly_fill = (int)u32(r + 8); }
                break;

            case 27:    /* MOVETOEX */
                if (size >= 16) {
                    M->g.cx = s32(r + 8);
                    M->g.cy = s32(r + 12);

                    if (M->in_path) {
                        M->fig_open = 0;
                    }
                }

                break;

            case 54:    /* LINETO */
                if (size >= 16) {
                    double xy[4] = { M->g.cx, M->g.cy, s32(r + 8), s32(r + 12) };

                    if (M->in_path && M->fig_open) {    /* goes on from where the figure is */
                        figure(M, xy + 2, 1, 0, 0, 1);
                    } else {
                        figure(M, xy, 2, 0, 0, 1);
                    }

                    M->g.cx = xy[2];
                    M->g.cy = xy[3];
                }

                break;

            case 33:    /* SAVEDC */
                if (M->depth < 16) {
                    M->stack[M->depth++] = M->g;
                }

                break;

            case 34:    /* RESTOREDC: relative (negative) or absolute */
                if (size >= 12) {
                    int32_t k = s32(r + 8);
                    int to = k < 0 ? M->depth + k : k - 1;

                    if (to >= 0 && to < M->depth) {
                        M->g = M->stack[to];
                        M->depth = to;
                    }
                }

                break;

            case 35:    /* SETWORLDTRANSFORM */
                if (size >= 32) {
                    float f[6];
                    int k;

                    memcpy(f, r + 8, 24);

                    for (k = 0; k < 6; k++) {
                        M->g.m[k] = fin(f[k]);
                    }
                }

                break;

            case 36:    /* MODIFYWORLDTRANSFORM */
                if (size >= 36) {
                    float f[6];
                    double a[6], *m = M->g.m, t[6];
                    int k, mode = (int)u32(r + 32);

                    memcpy(f, r + 8, 24);

                    for (k = 0; k < 6; k++) {
                        a[k] = fin(f[k]);
                    }

                    if (mode == 1) {    /* identity */
                        m[0] = m[3] = 1;
                        m[1] = m[2] = m[4] = m[5] = 0;
                    } else if (mode == 2 || mode == 3) {    /* a first (2: left multiply) or after (3) */
                        const double* x = mode == 2 ? a : m, *y = mode == 2 ? m : a;

                        t[0] = x[0] * y[0] + x[1] * y[2];
                        t[1] = x[0] * y[1] + x[1] * y[3];
                        t[2] = x[2] * y[0] + x[3] * y[2];
                        t[3] = x[2] * y[1] + x[3] * y[3];
                        t[4] = x[4] * y[0] + x[5] * y[2] + y[4];
                        t[5] = x[4] * y[1] + x[5] * y[3] + y[5];
                        memcpy(m, t, sizeof(t));
                    } else if (mode == 4) {
                        memcpy(m, a, sizeof(a));
                    }
                }

                break;

            case 37:    /* SELECTOBJECT */
                if (size >= 12) {
                    select_obj(M, u32(r + 8));
                }

                break;

            case 38:    /* CREATEPEN */
                if (size >= 28 && u32(r + 8) < MAX_OBJ) {
                    gobj* o = &M->obj[u32(r + 8)];

                    memset(o, 0, sizeof(*o));
                    o->kind = 1;
                    o->null = (u32(r + 12) & 15) == 5;
                    o->width = s32(r + 16);
                    o->style = u32(r + 12) | (o->width > 1 ? 0x10000 : 0);   /* wider than a pixel: geometric */
                    o->color = colorref(u32(r + 24));
                }

                break;

            case 95:    /* EXTCREATEPEN */
                if (size >= 44 && u32(r + 8) < MAX_OBJ) {
                    gobj* o = &M->obj[u32(r + 8)];

                    memset(o, 0, sizeof(*o));
                    o->kind = 1;
                    o->null = (u32(r + 28) & 15) == 5 || u32(r + 36) == 1;    /* PS_NULL, or a null brush */
                    o->style = u32(r + 28);
                    o->width = u32(r + 32);
                    o->color = colorref(u32(r + 40));
                }

                break;

            case 39:    /* CREATEBRUSHINDIRECT */
                if (size >= 24 && u32(r + 8) < MAX_OBJ) {
                    gobj* o = &M->obj[u32(r + 8)];

                    memset(o, 0, sizeof(*o));
                    o->kind = 2;
                    o->null = u32(r + 12) == 1;
                    o->color = colorref(u32(r + 16));
                    o->hatch = u32(r + 12) == 2 && size >= 24 && u32(r + 20) <= 5 ? (int)u32(r + 20) + 1 : 0;
                }

                break;

            case 93:    /* CREATEDIBPATTERNBRUSHPT: a pattern, taken as grey */
            case 94:
                if (size >= 12 && u32(r + 8) < MAX_OBJ) {
                    gobj* o = &M->obj[u32(r + 8)];

                    memset(o, 0, sizeof(*o));
                    o->kind = 2;
                    o->color = 0xFF808080u;
                }

                break;

            case 82:    /* EXTCREATEFONTINDIRECTW */
                if (size >= 12 + 92 && u32(r + 8) < MAX_OBJ) {
                    gobj* o = &M->obj[u32(r + 8)];
                    const unsigned char* lf = r + 12;

                    memset(o, 0, sizeof(*o));
                    o->kind = 3;
                    o->height = s32(lf);
                    o->weight = s32(lf + 16);
                    o->esc = s32(lf + 8);
                    o->italic = lf[20] != 0;
                    face_name(o->face, sizeof(o->face), lf + 28, 32);
                }

                break;

            case 40:    /* DELETEOBJECT */
                if (size >= 12 && u32(r + 8) < MAX_OBJ) {
                    M->obj[u32(r + 8)].kind = 0;
                }

                break;

            case 43:    /* RECTANGLE */
                if (size >= 24) {
                    rect(M, s32(r + 8), s32(r + 12), s32(r + 16), s32(r + 20));
                }

                break;

            case 44:    /* ROUNDRECT: square corners */
                if (size >= 32) {
                    rect(M, s32(r + 8), s32(r + 12), s32(r + 16), s32(r + 20));
                }

                break;

            case 42:    /* ELLIPSE */
                if (size >= 24) {
                    ellipse(M, s32(r + 8), s32(r + 12), s32(r + 16), s32(r + 20));
                }

                break;

            case 118:   /* GRADIENTFILL bounds, vertices, meshes, mode; TRIVERTEX x, y, r, g, b, a (16 bits each) */
                if (size >= 36) {
                    uint32_t nv = u32(r + 24), nm = u32(r + 28), mode = u32(r + 32), i;

                    for (i = 0; mode <= 1 && i < nm && 36 + 16 * (uint64_t)nv + 8 * (uint64_t)(i + 1) <= size; i++) {
                        uint32_t a = u32(r + 36 + 16 * nv + 8 * i), b = u32(r + 40 + 16 * nv + 8 * i);
                        const unsigned char* va = r + 36 + 16 * a, *vb = r + 36 + 16 * b;
                        uint32_t ca, cb;

                        if (a >= nv || b >= nv) {
                            continue;
                        }

                        ca = 0xFF000000u | (uint32_t)va[9] << 16 | (uint32_t)va[11] << 8 | va[13];  /* each 16-bit's high byte */
                        cb = 0xFF000000u | (uint32_t)vb[9] << 16 | (uint32_t)vb[11] << 8 | vb[13];
                        grad_rect(M, s32(va), s32(va + 4), s32(vb), s32(vb + 4), ca, cb, mode == 1);
                    }
                }

                break;

            case 45:    /* ARC box, start, end */
            case 46:    /* CHORD */
            case 47:    /* PIE */
            case 55:    /* ARCTO */
                if (size >= 40) {
                    arc(M, s32(r + 8), s32(r + 12), s32(r + 16), s32(r + 20), s32(r + 24), s32(r + 28), s32(r + 32),
                        s32(r + 36), type == 45 ? 0 : type == 46 ? 1 : type == 47 ? 2 : 3);
                }

                break;

            case 41:    /* ANGLEARC centre, radius, start and sweep angles (degrees, anticlockwise) */
                if (size >= 28) {
                    double cx = s32(r + 8), cy = s32(r + 12), rad = u32(r + 16), st, sw;
                    float f[2];

                    memcpy(f, r + 20, 8);
                    st = f[0] * 3.14159265358979 / 180;
                    sw = f[1] * 3.14159265358979 / 180;
                    /* (a sweep of 0 or a full turn: the start and end radials the same) */
                    arc(M, cx - rad, cy - rad, cx + rad, cy + rad, cx + cos(st) * rad, cy - sin(st) * rad,
                        cx + cos(st + (fabs(sw) >= 6.2831 ? sw * 0.9999 : sw)) * rad,
                        cy - sin(st + (fabs(sw) >= 6.2831 ? sw * 0.9999 : sw)) * rad, 3);
                }

                break;

            case 3:     /* POLYGON */
            case 4:     /* POLYLINE */
            case 86:    /* POLYGON16 */
            case 87:    /* POLYLINE16 */
            case 6:     /* POLYLINETO */
            case 89:    /* POLYLINETO16 */
                if (size >= 28) {
                    uint32_t cnt = u32(r + 24);
                    int small = type >= 86, to = type == 6 || type == 89, poly = type == 3 || type == 86;
                    double* xy = read_pts(r + 28, size - 28, cnt, small);

                    if (xy) {
                        if (to) {
                            double* xy2 = (double*)malloc(((size_t)cnt + 1) * 2 * sizeof(double));

                            if (xy2) {
                                xy2[0] = M->g.cx;
                                xy2[1] = M->g.cy;
                                memcpy(xy2 + 2, xy, (size_t)cnt * 2 * sizeof(double));

                                if (M->in_path && M->fig_open) {
                                    figure(M, xy2 + 2, (int)cnt, 0, 0, 1);
                                } else {
                                    figure(M, xy2, (int)cnt + 1, 0, 0, 1);
                                }

                                free(xy2);
                            }

                            M->g.cx = xy[2 * cnt - 2];
                            M->g.cy = xy[2 * cnt - 1];
                        } else {
                            figure(M, xy, (int)cnt, poly, poly, 1);
                        }

                        free(xy);
                    }
                }

                break;

            case 2:     /* POLYBEZIER */
            case 85:    /* POLYBEZIER16 */
            case 5:     /* POLYBEZIERTO */
            case 88:    /* POLYBEZIERTO16 */
                if (size >= 28) {
                    uint32_t cnt = u32(r + 24);
                    int small = type >= 85, to = type == 5 || type == 88, k;
                    double* pts = read_pts(r + 28, size - 28, cnt, small), *xy;

                    if (pts && (xy = flatten(pts, cnt, M->g.cx, M->g.cy, to, &k)) != NULL) {
                        double* cv = bez_cv(pts, cnt, M->g.cx, M->g.cy, to);
                        int nc = to ? 1 + 4 * (int)(cnt / 3) : 1 + 4 * (int)((cnt - 1) / 3);

                        if (to && M->in_path && M->fig_open) {
                            figure_cv(M, xy + 2, k - 1, 0, 0, 1, cv ? cv + 2 : NULL, nc - 1);
                        } else {
                            figure_cv(M, xy, k, 0, 0, 1, cv, nc);
                        }

                        free(cv);

                        M->g.cx = xy[2 * k - 2];
                        M->g.cy = xy[2 * k - 1];
                        free(xy);
                    }

                    free(pts);
                }

                break;

            case 7:     /* POLYPOLYLINE */
            case 8:     /* POLYPOLYGON */
            case 90:    /* POLYPOLYLINE16 */
            case 91:    /* POLYPOLYGON16 */
                if (size >= 32) {
                    uint32_t np = u32(r + 24), cnt = u32(r + 28), i, done = 0;
                    int small = type >= 90, poly = type == 8 || type == 91;

                    if (np < 100000 && 32 + (size_t)np * 4 <= size) {
                        double* xy = read_pts(r + 32 + np * 4, size - 32 - np * 4, cnt, small);
                        double* all = NULL;
                        int nall = 0;

                        if (xy && poly && (all = (double*)malloc(((size_t)cnt + np) * 2 * sizeof(double))) != NULL) {
                            /* a polygon's rings drawn together: its holes stay holes */
                            for (i = 0; i < np && done <= cnt; i++) {
                                uint32_t c = u32(r + 32 + 4 * i), k;

                                if (done + c > cnt) {
                                    break;
                                }

                                if (i) {
                                    all[2 * nall] = NAN;
                                    all[2 * nall + 1] = NAN;
                                    nall++;
                                }

                                for (k = 0; k < c; k++) {
                                    map_pt(M, xy[2 * (done + k)], xy[2 * (done + k) + 1], &all[2 * nall], &all[2 * nall + 1]);
                                    nall++;
                                }

                                done += c;
                            }

                            if (M->in_path) {   /* into the path, ring by ring */
                                if (!pd_grow((void**)&M->pp, &M->cappp, (int64_t)M->npp + 2 * nall + 2, sizeof(double))) {
                                    if (M->npp > 0) {
                                        M->pp[M->npp++] = NAN;
                                        M->pp[M->npp++] = NAN;
                                    }

                                    memcpy(M->pp + M->npp, all, (size_t)nall * 2 * sizeof(double));
                                    M->npp += 2 * nall;
                                }
                            } else {
                                out_path(M, all, nall, 1, 1, 1);
                            }
                        } else if (xy) {
                            for (i = 0; i < np && done <= cnt; i++) {
                                uint32_t c = u32(r + 32 + 4 * i);

                                if (done + c > cnt) {
                                    break;
                                }

                                figure(M, xy + 2 * done, (int)c, 0, 0, 1);
                                done += c;
                            }
                        }

                        free(all);
                        free(xy);
                    }
                }

                break;

            case 30:    /* INTERSECTCLIPRECT: a rectangle, logical units */
                if (size >= 24) {
                    double ax, ay, bx, by;

                    map_pt(M, s32(r + 8), s32(r + 12), &ax, &ay);
                    map_pt(M, s32(r + 16), s32(r + 20), &bx, &by);
                    clip_to(M, ax, ay, bx, by, 0);
                }

                break;

            case 75:    /* EXTSELECTCLIPRGN: a region (its bounds, device units), or none */
                if (size >= 16) {
                    uint32_t cb = u32(r + 8), mode = u32(r + 12);

                    if (cb == 0 || size < 16 + 32) {
                        if (mode == 5) {    /* RGN_COPY of nothing: unclipped */
                            M->g.clip = 0;
                        }
                    } else {
                        const unsigned char* rh = r + 16;   /* RGNDATAHEADER: size, type, count, size, bounds */
                        double ax = (s32(rh + 16) - M->fx0) * M->fsx, ay = (s32(rh + 20) - M->fy0) * M->fsy;
                        double bx = (s32(rh + 24) - M->fx0) * M->fsx, by = (s32(rh + 28) - M->fy0) * M->fsy;

                        if (mode == 5 || mode == 1) {   /* RGN_COPY, RGN_AND */
                            clip_to(M, ax, ay, bx, by, mode == 5);
                        }
                    }
                }

                break;

            case 67:    /* SELECTCLIPPATH: the path's bounds */
                if (M->npp > 0) {
                    double ax = 1e300, ay = 1e300, bx = -1e300, by = -1e300;
                    int q;

                    for (q = 0; q < M->npp; q++) {
                        if (!isnan(M->pp[2 * q])) {
                            ax = M->pp[2 * q] < ax ? M->pp[2 * q] : ax;
                            ay = M->pp[2 * q + 1] < ay ? M->pp[2 * q + 1] : ay;
                            bx = M->pp[2 * q] > bx ? M->pp[2 * q] : bx;
                            by = M->pp[2 * q + 1] > by ? M->pp[2 * q + 1] : by;
                        }
                    }

                    if (bx >= ax) {
                        clip_to(M, ax, ay, bx, by, size >= 12 && u32(r + 8) == 5);
                    }

                    M->npp = 0;
                }

                break;

            case 59:    /* BEGINPATH */
                M->in_path = 1;
                M->npp = 0;
                M->fig_open = 0;
                break;

            case 60:    /* ENDPATH */
                M->in_path = 0;
                break;

            case 61:    /* CLOSEFIGURE */
                M->fig_open = 0;
                break;

            case 62:    /* FILLPATH */
                path_out(M, 1, 0);
                break;

            case 63:    /* STROKEANDFILLPATH */
                path_out(M, 1, 1);
                break;

            case 64:    /* STROKEPATH */
                path_out(M, 0, 1);
                break;

            case 68:    /* ABORTPATH */
                M->in_path = 0;
                M->npp = 0;
                break;

            case 84:    /* EXTTEXTOUTW */
            case 83:    /* EXTTEXTOUTA */
                if (size >= 76) {
                    double x = s32(r + 36), y = s32(r + 40);
                    uint32_t nch = u32(r + 44), off = u32(r + 48);
                    size_t bytes = (size_t)nch * (type == 84 ? 2 : 1);

                    if (nch > 0 && nch < 100000 && off < size && bytes <= size - off) {
                        out_text(M, x, y, r + off, nch, type == 84);
                    }
                }

                break;

            case 81:    /* STRETCHDIBITS */
                if (size >= 80) {
                    uint32_t ob = u32(r + 48), cb = u32(r + 52), obits = u32(r + 56), cbits = u32(r + 60);

                    if (!M->mute && ob <= size && cb <= size - ob && obits <= size && cbits <= size - obits && cb &&
                            rop_draws(u32(r + 68), r + ob, cb)) {   /* (muted: not made a picture nothing shows) */
                        out_bitmap(M, dib_resource(M->d, r + ob, cb, r + obits, cbits), s32(r + 24), s32(r + 28),
                                   s32(r + 72), s32(r + 76));
                    }
                }

                break;

            case 80:    /* SETDIBITSTODEVICE */
                if (size >= 76) {
                    uint32_t ob = u32(r + 48), cb = u32(r + 52), obits = u32(r + 56), cbits = u32(r + 60);

                    if (!M->mute && ob <= size && cb <= size - ob && obits <= size && cbits <= size - obits && cb) {
                        out_bitmap(M, dib_resource(M->d, r + ob, cb, r + obits, cbits), s32(r + 24), s32(r + 28),
                                   s32(r + 40), s32(r + 44));
                    }
                }

                break;

            case 76:    /* BITBLT */
            case 77:    /* STRETCHBLT */
                if (size >= 100) {
                    uint32_t ob = u32(r + 84), cb = u32(r + 88), obits = u32(r + 92), cbits = u32(r + 96);
                    double x = s32(r + 24), y = s32(r + 28), w = s32(r + 32), h = s32(r + 36);
                    uint32_t rop = u32(r + 40) >> 16;

                    if (cb == 0) {  /* no source: the brush (PATCOPY), white or black over the rectangle -- or nothing */
                        double xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
                        gobj keep = M->g.brush;

                        if (rop == 0xFF || rop == 0x00) {
                            M->g.brush.kind = 2;
                            M->g.brush.null = 0;
                            M->g.brush.color = rop == 0xFF ? 0xFFFFFFFFu : 0xFF000000u;
                        }

                        if (rop == 0xF0 || rop == 0xFF || rop == 0x00) {
                            figure(M, xy, 4, 1, 1, 0);
                        }

                        M->g.brush = keep;
                    } else if (!M->mute && ob <= size && cb <= size - ob && obits <= size && cbits <= size - obits &&
                               rop_draws(u32(r + 40), r + ob, cb)) {
                        out_bitmap(M, dib_resource(M->d, r + ob, cb, r + obits, cbits), x, y, w, h);
                    }
                }

                break;

            case 14:    /* EOF */
                return;

            default:
                break;
        }

        at += size;
    }
}

/* ------------------------------------------------------------------ */
/* WMF                                                                */
/* ------------------------------------------------------------------ */

static int wmf_new_slot(mf* M) {
    int i;

    for (i = 0; i < MAX_OBJ; i++) {
        if (M->obj[i].kind == 0) {
            return i;
        }
    }

    return -1;
}

static void wmf_play(mf* M, const unsigned char* p, size_t n) {
    size_t at = 18;     /* past the META header */

    while (at + 6 <= n) {
        uint32_t words = u32(p + at);
        size_t size = (size_t)words * 2;
        uint16_t fn = u16(p + at + 4);
        const unsigned char* a = p + at + 6;     /* the parameters */
        size_t na = size >= 6 ? size - 6 : 0;

        if (size < 6 || size > n - at) {
            break;
        }

#define P16(i) ((size_t)(i) * 2 + 2 <= na ? s16(a + 2 * (i)) : 0)
        switch (fn) {
            case 0x0000:    /* EOF */
                return;

            case 0x0201:    /* SETBKCOLOR */
                M->g.bk_color = colorref((uint32_t)(uint16_t)P16(0) | ((uint32_t)(uint16_t)P16(1) << 16));
                break;

            case 0x0102:    /* SETBKMODE */
                M->g.bk_mode = P16(0);
                break;

            case 0x020B:    /* SETWINDOWORG y, x */
                M->g.woy = P16(0);
                M->g.wox = P16(1);
                break;

            case 0x020C:    /* SETWINDOWEXT y, x */
                M->g.wey = P16(0);
                M->g.wex = P16(1);
                M->g.map = 8;
                M->g.vex = M->g.wex;    /* the window is the picture: logical units map to it one to one */
                M->g.vey = M->g.wey;
                M->g.vox = M->g.wox;
                M->g.voy = M->g.woy;
                break;

            case 0x0209:    /* SETTEXTCOLOR */
                M->g.text_color = colorref((uint32_t)(uint16_t)P16(0) | ((uint32_t)(uint16_t)P16(1) << 16));
                break;

            case 0x012E:    /* SETTEXTALIGN */
                M->g.text_align = (uint16_t)P16(0);
                break;

            case 0x001E:    /* SAVEDC */
                if (M->depth < 16) {
                    M->stack[M->depth++] = M->g;
                }

                break;

            case 0x0127:    /* RESTOREDC */
                if (M->depth > 0) {
                    M->g = M->stack[--M->depth];
                }

                break;

            case 0x0214:    /* MOVETO y, x */
                M->g.cy = P16(0);
                M->g.cx = P16(1);
                break;

            case 0x0213: {  /* LINETO y, x */
                double xy[4] = { M->g.cx, M->g.cy, P16(1), P16(0) };

                figure(M, xy, 2, 0, 0, 1);
                M->g.cx = xy[2];
                M->g.cy = xy[3];
                break;
            }

            case 0x0324:    /* POLYGON */
            case 0x0325: {  /* POLYLINE */
                int cnt = P16(0), i;
                double* xy;

                if (cnt > 0 && (size_t)(1 + 2 * cnt) * 2 <= na && (xy = (double*)malloc((size_t)cnt * 2 * sizeof(double)))) {
                    for (i = 0; i < 2 * cnt; i++) {
                        xy[i] = s16(a + 2 + 2 * i);
                    }

                    figure(M, xy, cnt, fn == 0x0324, fn == 0x0324, 1);
                    free(xy);
                }

                break;
            }

            case 0x0538: {  /* POLYPOLYGON: counts, then points */
                int np = P16(0), i, k, done = 0, total = 0, nall = 0;
                double* all;

                for (i = 0; i < np && (size_t)(1 + i) * 2 + 2 <= na; i++) {
                    total += (uint16_t)P16(1 + i);
                }

                if (np > 0 && total > 0 && (size_t)(1 + np + 2 * total) * 2 <= na &&
                        (all = (double*)malloc((size_t)(total + np) * 2 * sizeof(double)))) {
                    for (i = 0; i < np; i++) {
                        int c = (uint16_t)P16(1 + i);

                        if (i) {
                            all[2 * nall] = NAN;
                            all[2 * nall + 1] = NAN;
                            nall++;
                        }

                        for (k = 0; k < c; k++) {
                            const unsigned char* q = a + 2 * (1 + np) + 4 * (size_t)(done + k);

                            map_pt(M, s16(q), s16(q + 2), &all[2 * nall], &all[2 * nall + 1]);
                            nall++;
                        }

                        done += c;
                    }

                    out_path(M, all, nall, 1, 1, 1);
                    free(all);
                }

                break;
            }

            case 0x0416: {  /* INTERSECTCLIPRECT bottom, right, top, left */
                double ax, ay, bx, by;

                map_pt(M, P16(3), P16(2), &ax, &ay);
                map_pt(M, P16(1), P16(0), &bx, &by);
                clip_to(M, ax, ay, bx, by, 0);
                break;
            }

            case 0x041B:    /* RECTANGLE bottom, right, top, left */
                rect(M, P16(3), P16(2), P16(1), P16(0));
                break;

            case 0x061C:    /* ROUNDRECT h, w, bottom, right, top, left */
                rect(M, P16(5), P16(4), P16(3), P16(2));
                break;

            case 0x0418:    /* ELLIPSE bottom, right, top, left */
                ellipse(M, P16(3), P16(2), P16(1), P16(0));
                break;

            case 0x0817:    /* ARC yend, xend, ystart, xstart, bottom, right, top, left */
            case 0x0830:    /* CHORD */
            case 0x081A:    /* PIE */
                arc(M, P16(7), P16(6), P16(5), P16(4), P16(3), P16(2), P16(1), P16(0),
                    fn == 0x0817 ? 0 : fn == 0x0830 ? 1 : 2);
                break;

            case 0x02FA: {  /* CREATEPENINDIRECT style, width x, width y, colour */
                int s = wmf_new_slot(M);

                if (s >= 0) {
                    gobj* o = &M->obj[s];

                    memset(o, 0, sizeof(*o));
                    o->kind = 1;
                    o->null = (P16(0) & 15) == 5;
                    o->width = P16(1);
                    o->style = (uint32_t)(uint16_t)P16(0) | (o->width > 1 ? 0x10000 : 0);
                    o->color = colorref((uint32_t)(uint16_t)P16(3) | ((uint32_t)(uint16_t)P16(4) << 16));
                }

                break;
            }

            case 0x02FC: {  /* CREATEBRUSHINDIRECT style, colour, hatch */
                int s = wmf_new_slot(M);

                if (s >= 0) {
                    gobj* o = &M->obj[s];

                    memset(o, 0, sizeof(*o));
                    o->kind = 2;
                    o->null = P16(0) == 1;
                    o->color = colorref((uint32_t)(uint16_t)P16(1) | ((uint32_t)(uint16_t)P16(2) << 16));
                    o->hatch = P16(0) == 2 && (uint16_t)P16(3) <= 5 ? P16(3) + 1 : 0;
                }

                break;
            }

            case 0x02FB: {  /* CREATEFONTINDIRECT */
                int s = wmf_new_slot(M);

                if (s >= 0) {
                    gobj* o = &M->obj[s];
                    size_t k;

                    memset(o, 0, sizeof(*o));
                    o->kind = 3;
                    o->height = P16(0);
                    o->weight = P16(4);
                    o->italic = na > 10 && a[10] != 0;

                    for (k = 0; k + 18 < na && k + 1 < sizeof(o->face) && a[18 + k]; k++) {
                        o->face[k] = a[18 + k] < 0x80 ? (char)a[18 + k] : '?';
                    }
                }

                break;
            }

            case 0x00F7:    /* CREATEPALETTE, and the other objects that take a slot */
            case 0x06FF:    /* CREATEREGION */
            case 0x01F9:    /* CREATEPATTERNBRUSH */
            case 0x0142: {  /* DIBCREATEPATTERNBRUSH */
                int s = wmf_new_slot(M);

                if (s >= 0) {
                    memset(&M->obj[s], 0, sizeof(gobj));
                    M->obj[s].kind = fn == 0x0142 || fn == 0x01F9 ? 2 : 4;
                    M->obj[s].color = 0xFF808080u;
                }

                break;
            }

            case 0x012D:    /* SELECTOBJECT */
                select_obj(M, (uint16_t)P16(0));
                break;

            case 0x01F0:    /* DELETEOBJECT */
                if ((uint16_t)P16(0) < MAX_OBJ) {
                    M->obj[(uint16_t)P16(0)].kind = 0;
                }

                break;

            case 0x0521: {  /* TEXTOUT count, string, y, x */
                int cnt = P16(0), pad = (cnt + 1) & ~1;

                if (cnt > 0 && (size_t)(2 + pad + 4) <= na) {
                    out_text(M, s16(a + 2 + pad + 2), s16(a + 2 + pad), a + 2, (size_t)cnt, 0);
                }

                break;
            }

            case 0x0A32: {  /* EXTTEXTOUT y, x, count, options, [rect], string */
                int cnt = P16(2), opt = (uint16_t)P16(3);
                size_t off = 8 + ((opt & 6) ? 8 : 0);

                if (cnt > 0 && off + (size_t)cnt <= na) {
                    out_text(M, P16(1), P16(0), a + off, (size_t)cnt, 0);
                }

                break;
            }

            case 0x0F43:    /* STRETCHDIB rop(2), usage, srcH, srcW, ySrc, xSrc, destH, destW, yDst, xDst, DIB */
                if (na > 22 + 40) {
                    const unsigned char* bmi = a + 22;
                    uint32_t hs = u32(bmi), bpp = u16(bmi + 14), nc = u32(bmi + 32);
                    size_t pal = bpp <= 8 ? (size_t)(nc ? nc : (1u << bpp)) * 4 : (u32(bmi + 16) == 3 ? 12 : 0);

                    if (hs >= 40 && 22 + hs + pal <= na) {
                        out_bitmap(M, dib_resource(M->d, bmi, hs + pal, bmi + hs + pal, na - 22 - hs - pal), P16(10), P16(9),
                                   P16(8), P16(7));
                    }
                }

                break;

            case 0x0B41:    /* DIBSTRETCHBLT rop(2), srcH, srcW, ySrc, xSrc, destH, destW, yDst, xDst, DIB */
            case 0x0940:    /* DIBBITBLT rop(2), ySrc, xSrc, height, width, yDst, xDst, DIB */
                {
                    size_t hd = fn == 0x0B41 ? 20 : 16;

                    if (na > hd + 40) {
                        const unsigned char* bmi = a + hd;
                        uint32_t hs = u32(bmi), bpp = u16(bmi + 14), nc = u32(bmi + 32);
                        size_t pal = bpp <= 8 ? (size_t)(nc ? nc : (1u << bpp)) * 4 : (u32(bmi + 16) == 3 ? 12 : 0);

                        if (hs >= 40 && hd + hs + pal <= na) {
                            pd_res_id res = dib_resource(M->d, bmi, hs + pal, bmi + hs + pal, na - hd - hs - pal);

                            if (fn == 0x0B41) {
                                out_bitmap(M, res, P16(9), P16(8), P16(7), P16(6));
                            } else {
                                out_bitmap(M, res, P16(7), P16(6), P16(5), P16(4));
                            }
                        }
                    }
                }

                break;

            default:
                break;
        }
#undef P16

        at += size;
    }
}

/* ------------------------------------------------------------------ */

int pd_metafile_kind(const unsigned char* p, size_t n) {
    if (n >= 88 && u32(p) == 1 && u32(p + 40) == 0x464D4520u) {
        return 1;   /* EMF: a header record and " EMF" */
    }

    if (n >= 40 && u32(p) == 0x9AC6CDD7u) {
        return 2;   /* a placeable WMF */
    }

    if (n >= 18 && (u16(p) == 1 || u16(p) == 2) && u16(p + 2) == 9 && (u16(p + 4) == 0x0300 || u16(p + 4) == 0x0100)) {
        return 2;   /* a WMF without the placeable header */
    }

    return 0;
}

/* a metafile's drawing description into o (its pictures added to the document as they come); 1 when it draws
   something */
static int metafile_json(pd_doc* d, const unsigned char* p, size_t n, pd_res_id src, pd_buf* out) {
    mf* M;
    pd_buf o;
    int ok = 0;
    int kind = pd_metafile_kind(p, n);
    double wsp, hsp;

    if (!kind || (M = (mf*)calloc(1, sizeof(mf))) == NULL) {
        return 0;
    }

    memset(&o, 0, sizeof(o));
    M->d = d;
    M->o = &o;
    M->first = 1;
    state_init(&M->g);

    if (kind == 1) {
        /* the frame (.01 mm) and the device's pixels per mm: device units to points */
        int32_t fl = s32(p + 24), ft = s32(p + 28), fr = s32(p + 32), fb = s32(p + 36);
        double devx = s32(p + 72), devy = s32(p + 76), mmx = s32(p + 80), mmy = s32(p + 84);
        double pxmm_x = mmx > 0 ? devx / mmx : 3.78, pxmm_y = mmy > 0 ? devy / mmy : 3.78;

        if (fr <= fl || fb <= ft) {     /* no frame: the bounds, in device units */
            fl = (int32_t)(s32(p + 8) / pxmm_x * 100);
            ft = (int32_t)(s32(p + 12) / pxmm_y * 100);
            fr = (int32_t)(s32(p + 16) / pxmm_x * 100);
            fb = (int32_t)(s32(p + 20) / pxmm_y * 100);
        }

        wsp = (fr - fl) / 100.0 * 72 / 25.4 * 65536;
        hsp = (fb - ft) / 100.0 * 72 / 25.4 * 65536;
        M->fx0 = fl / 100.0 * pxmm_x;
        M->fy0 = ft / 100.0 * pxmm_y;
        M->fsx = fr > fl ? wsp / ((fr - fl) / 100.0 * pxmm_x) : 1;
        M->fsy = fb > ft ? hsp / ((fb - ft) / 100.0 * pxmm_y) : 1;
    } else {
        /* the placeable header's box and units per inch; else the window the records set */
        const unsigned char* q = p;
        double l = 0, t = 0, r = 1000, b = 1000, inch = 1440;

        if (u32(p) == 0x9AC6CDD7u) {
            l = s16(p + 6);
            t = s16(p + 8);
            r = s16(p + 10);
            b = s16(p + 12);
            inch = u16(p + 14) ? u16(p + 14) : 1440;
            q = p + 22;
            n -= 22;
        }

        if (r <= l || b <= t) {
            r = l + 1000;
            b = t + 1000;
        }

        wsp = (r - l) / inch * 72 * 65536;
        hsp = (b - t) / inch * 72 * 65536;
        M->fx0 = l;
        M->fy0 = t;
        M->fsx = wsp / (r - l);
        M->fsy = hsp / (b - t);
        M->wmf = 1;
        p = q;
    }

    if (wsp > 0 && hsp > 0 && wsp < 2e9 && hsp < 2e9) {
        pb_printf(&o, "{\"w\":%d,\"h\":%d,\"src\":%d,\"items\":[", (int)wsp, (int)hsp, (int)src);

        if (kind == 1) {
            pctx* P = (pctx*)calloc(1, sizeof(pctx));

            if (P) {    /* device units to the drawing's, for the EMF+ records */
                aff_set(P->base, M->fsx, 0, 0, M->fsy, -M->fx0 * M->fsx, -M->fy0 * M->fsy);
                plus_play(M, P, p, n);
                free(P);
            }
        } else {
            wmf_play(M, p, n);
        }

        pb_puts(&o, "]}");
        ok = !o.err && !M->first;
    }

    free(M->pp);
    free(M->pcv);
    free(M);

    if (ok) {
        *out = o;
    } else {
        pb_free(&o);
    }

    return ok;
}

#define DRAWING_MIME "application/vnd.parade.drawing+json"

pd_res_id pd_metafile_drawing(pd_doc* d, const unsigned char* p, size_t n, pd_res_id src) {
    pd_buf o;
    pd_res_id res = 0;
    int32_t n0 = pd_doc_res_count(d);

    memset(&o, 0, sizeof(o));

    if (metafile_json(d, p, n, src, &o) && pd_doc_add_resource(d, DRAWING_MIME, o.p, o.n, &res) == PD_OK) {
        /* saved as where it comes from: made again from the metafile on load */
        pd_doc_res_derived(d, res, src, (pd_res_id)n0 + 1, res - 1);
    } else {
        res = 0;
    }

    pb_free(&o);
    return res;
}

int pd_metafile_remake(pd_doc* d, pd_res_id r) {
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    pd_res_id src, lo, hi;
    pd_buf o;
    int rc = -1;

    if (!d || r < 1 || (int32_t)r > pd_doc_res_count(d)) {
        return -1;
    }

    if (!pd_doc_res_origin(d, r, &src, &lo, &hi) || pd_doc_resource(d, src, &mime, &data, &len) != PD_OK) {
        return -1;
    }

    memset(&o, 0, sizeof(o));
    pd_doc_res_replay(d, lo, hi);

    if (metafile_json(d, (const unsigned char*)data, len, src, &o)) {
        rc = pd_doc_res_fill(d, r, o.p, o.n);
    }

    pd_doc_res_replay(d, 0, 0);
    pb_free(&o);
    return rc;
}
