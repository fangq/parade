/*
 * Parade SVG: a page's display list, or one drawing's, as SVG 1.1 --
 * paths with their curves, dashes, ends and corners; gradients and
 * patterns as SVG's own; glyphs as paths from Parade's outlines (each
 * glyph defined once, used where it is set); pictures as data: URIs, cut
 * to their frames and shapes; what only pixels can draw (soft shadows,
 * glows, soft edges, inner shadows, reflections) as PNG pictures from
 * Parade's rasterizer. User units are points, y down, as on the page.
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_doc_internal.h"
#include "pd_conv.h"
#include "parade_layout.h"
#include "pd_paint.h"

typedef struct {
    pd_buf* o;
    const pd_doc* d;
    int32_t ids;                /* defs made so far, for their names */
    struct {
        const pd_font* f;
        uint32_t g;
    } *glyphs;                  /* glyphs defined, in order: glyph k is #g<k> */
    int32_t nglyphs, capglyphs;
} svgw;

/* a length on the page (sp) in points, to a thousandth */
static void sv_num(pd_buf* o, double sp) {
    double v = sp / 65536.0;
    long long q = llround(v * 1000);

    if (q % 1000 == 0) {
        pb_printf(o, "%lld", q / 1000);
    } else {
        pb_printf(o, "%s%lld.%03lld", q < 0 ? "-" : "", llabs(q) / 1000, llabs(q) % 1000);

        while (o->n > 0 && o->p[o->n - 1] == '0') {     /* no trailing zeros */
            o->n--;
        }
    }
}

/* attr="#rrggbb" and attr-opacity when see-through (alpha 0: a colour given without one, opaque) */
static void sv_color(pd_buf* o, const char* attr, uint32_t c) {
    uint32_t a = c >> 24;

    pb_printf(o, " %s=\"#%06x\"", attr, (unsigned)(c & 0xFFFFFF));

    if (a && a < 255) {
        pb_printf(o, " %s-opacity=\"%.3f\"", attr, a / 255.0);
    }
}

/* a path item's outline as path data: its curve where it has one */
static void sv_d(pd_buf* o, const pd_sp* pts, int32_t n, int closed) {
    int32_t q, first = 1;

    for (q = 0; q < n; q++) {
        const pd_sp* p = &pts[2 * q];

        if (p[0] == PD_PATH_BREAK) {
            if (closed && !first) {
                pb_puts(o, "Z");
            }

            first = 1;
        } else if (p[0] == PD_PATH_CUBIC && !first && q + 3 < n) {
            int k;

            pb_putc(o, 'C');

            for (k = 1; k <= 3; k++) {
                if (k > 1) {
                    pb_putc(o, ' ');
                }

                sv_num(o, p[2 * k]);
                pb_putc(o, ' ');
                sv_num(o, p[2 * k + 1]);
            }

            q += 3;
        } else if (p[0] != PD_PATH_CUBIC) {
            pb_putc(o, first ? 'M' : 'L');
            sv_num(o, p[0]);
            pb_putc(o, ' ');
            sv_num(o, p[1]);
            first = 0;
        }
    }

    if (closed && !first) {
        pb_puts(o, "Z");
    }
}

static void sv_path_d(pd_buf* o, const pd_draw* a, int closed) {
    pb_puts(o, " d=\"");

    if (a->curve && a->ncurve >= 2) {
        sv_d(o, a->curve, a->ncurve, closed);
    } else {
        sv_d(o, a->points, a->npoints, closed);
    }

    pb_putc(o, '"');
}

/* a gradient's stops */
static void sv_stops(pd_buf* o, const pd_gradient* g) {
    int i;

    for (i = 0; i < g->n; i++) {
        pb_printf(o, "<stop offset=\"%.4f\" stop-color=\"#%06x\"", g->pos[i] / 65536.0, (unsigned)(g->color[i] & 0xFFFFFF));

        if ((g->color[i] >> 24) < 255) {
            pb_printf(o, " stop-opacity=\"%.3f\"", (g->color[i] >> 24) / 255.0);
        }

        pb_puts(o, "/>");
    }
}

/* a path's fill as a paint server in defs (a gradient, a pattern); its id, 0 for a plain colour */
static int32_t sv_paint(svgw* S, const pd_draw* a) {
    pd_buf* o = S->o;
    pd_gradient g;
    int32_t id;

    if (pd_draw_gradient(a, &g)) {
        id = ++S->ids;

        if (g.kind == PD_GRAD_LINEAR) {
            pb_printf(o, "<defs><linearGradient id=\"p%d\" gradientUnits=\"userSpaceOnUse\" x1=\"", (int)id);
            sv_num(o, g.x0);
            pb_puts(o, "\" y1=\"");
            sv_num(o, g.y0);
            pb_puts(o, "\" x2=\"");
            sv_num(o, g.x1);
            pb_puts(o, "\" y2=\"");
            sv_num(o, g.y1);
            pb_puts(o, "\">");
            sv_stops(o, &g);
            pb_puts(o, "</linearGradient></defs>\n");
        } else {    /* a circle; a rectangular one as the circle through its box's corners */
            double cx = g.kind == PD_GRAD_RECT ? (g.x0 + g.x1) / 2 : g.x0, cy = g.kind == PD_GRAD_RECT ? (g.y0 + g.y1) / 2 : g.y0;
            double r = g.kind == PD_GRAD_RECT ? sqrt((g.bx1 - g.bx0) * (g.bx1 - g.bx0) + (g.by1 - g.by0) * (g.by1 - g.by0)) / 2 :
                       sqrt((g.x1 - g.x0) * (g.x1 - g.x0) + (g.y1 - g.y0) * (g.y1 - g.y0));

            pb_printf(o, "<defs><radialGradient id=\"p%d\" gradientUnits=\"userSpaceOnUse\" cx=\"", (int)id);
            sv_num(o, cx);
            pb_puts(o, "\" cy=\"");
            sv_num(o, cy);
            pb_puts(o, "\" r=\"");
            sv_num(o, r);
            pb_puts(o, "\">");
            sv_stops(o, &g);
            pb_puts(o, "</radialGradient></defs>\n");
        }

        return id;
    }

    if (a->style && a->style->pattern) {    /* 8 by 8 cells of 3/4 pt from the page's corner */
        const pd_path_style* st = a->style;
        int r, x;

        id = ++S->ids;
        pb_printf(o, "<defs><pattern id=\"p%d\" patternUnits=\"userSpaceOnUse\" width=\"6\" height=\"6\">", (int)id);

        if (st->pattern_bg) {
            pb_puts(o, "<rect width=\"6\" height=\"6\"");
            sv_color(o, "fill", st->pattern_bg);
            pb_puts(o, "/>");
        }

        pb_puts(o, "<path d=\"");

        for (r = 0; r < 8; r++) {
            for (x = 0; x < 8; x++) {
                if ((st->pattern_bits[r] >> (7 - x)) & 1) {
                    pb_printf(o, "M%.2f %.2fh.75v.75h-.75Z", x * 0.75, r * 0.75);
                }
            }
        }

        pb_putc(o, '"');
        sv_color(o, "fill", st->pattern_fg);
        pb_puts(o, "/></pattern></defs>\n");
        return id;
    }

    return 0;
}

/* an item's layers drawn by the rasterizer as a PNG picture over its extent, at 144 dpi */
static void sv_fx(svgw* S, const pd_draw* a, int layers) {
    pd_sp x0, y0, x1, y1;
    int32_t scale = 2 << 16, W, H;
    uint8_t* px;
    unsigned char* png = NULL;
    size_t pn = 0;

    pd_draw_bounds(a, &x0, &y0, &x1, &y1);
    x0 -= PD_PT(2);
    y0 -= PD_PT(2);
    x1 += PD_PT(2);
    y1 += PD_PT(2);

    while ((int64_t)(x1 - x0) / 65536 * (y1 - y0) / 65536 * scale / 65536 * scale / 65536 > 4000000 && scale > 8192) {
        scale /= 2;
    }

    W = (int32_t)(((int64_t)x1 - x0) * scale / 65536 / 65536) + 1;
    H = (int32_t)(((int64_t)y1 - y0) * scale / 65536 / 65536) + 1;

    if (W <= 0 || H <= 0 || !(px = (uint8_t*)calloc((size_t)W * H, 4))) {
        return;
    }

    if (pd_draw_render_layers(a, layers, x0, y0, scale, px, W, H, W * 4) == PD_OK &&
            pd_png_encode(px, W, H, W * 4, &png, &pn) == 0) {
        pb_puts(S->o, "<image x=\"");
        sv_num(S->o, x0);
        pb_puts(S->o, "\" y=\"");
        sv_num(S->o, y0);
        pb_puts(S->o, "\" width=\"");
        sv_num(S->o, (double)W * 65536 * 65536 / scale);
        pb_puts(S->o, "\" height=\"");
        sv_num(S->o, (double)H * 65536 * 65536 / scale);
        pb_puts(S->o, "\" preserveAspectRatio=\"none\" xlink:href=\"data:image/png;base64,");
        pb_base64(S->o, png, pn);
        pb_puts(S->o, "\"/>\n");
    }

    free(png);
    free(px);
}

static void sv_path(svgw* S, const pd_draw* a) {
    pd_buf* o = S->o;
    const pd_path_style* st = a->style;
    int fx = pd_draw_effects(a), fill = a->fill != 0, stroke = a->line_width > 0 && a->color != 0;
    int32_t paint;

    if (fx & PD_LAYER_UNDER) {
        sv_fx(S, a, PD_LAYER_UNDER);
    }

    if (fx & PD_LAYER_BODY) {
        sv_fx(S, a, PD_LAYER_BODY | PD_LAYER_INNER);
        fill = 0;
    }

    paint = fill ? sv_paint(S, a) : 0;

    if (fill || stroke) {
        pb_puts(o, "<path");
        sv_path_d(o, a, (a->path_flags & PD_PATH_CLOSED) != 0 || (fill && !stroke));

        if (paint) {
            pb_printf(o, " fill=\"url(#p%d)\"", (int)paint);
        } else if (fill) {
            sv_color(o, "fill", a->fill);
        } else {
            pb_puts(o, " fill=\"none\"");
        }

        if (stroke) {
            int k;

            sv_color(o, "stroke", a->color);
            pb_puts(o, " stroke-width=\"");
            sv_num(o, a->line_width);
            pb_printf(o, "\" stroke-linejoin=\"%s\"", st && st->join == PD_JOIN_MITER ? "miter\" stroke-miterlimit=\"8" :
                      st && st->join == PD_JOIN_BEVEL ? "bevel" : "round");

            if (st && st->cap) {
                pb_printf(o, " stroke-linecap=\"%s\"", st->cap == PD_CAP_ROUND ? "round" : "square");
            }

            if (st && st->dash_count >= 2) {
                pb_puts(o, " stroke-dasharray=\"");

                for (k = 0; k < st->dash_count && k < 8; k++) {
                    if (k) {
                        pb_putc(o, ' ');
                    }

                    sv_num(o, st->dash[k]);
                }

                pb_putc(o, '"');
            }
        }

        pb_puts(o, "/>\n");
    }

    if ((fx & PD_LAYER_INNER) && !(fx & PD_LAYER_BODY)) {
        sv_fx(S, a, PD_LAYER_INNER);
    }

    if (fx & PD_LAYER_REFL) {
        sv_fx(S, a, PD_LAYER_REFL);
    }
}

/* glyph outlines as path data, font units (26.6 divided out), y up */
static void gs_move(void* u, int32_t x, int32_t y) {
    pb_printf((pd_buf*)u, "M%g %g", x / 64.0, y / 64.0);
}

static void gs_line(void* u, int32_t x, int32_t y) {
    pb_printf((pd_buf*)u, "L%g %g", x / 64.0, y / 64.0);
}

static void gs_quad(void* u, int32_t ax, int32_t ay, int32_t x, int32_t y) {
    pb_printf((pd_buf*)u, "Q%g %g %g %g", ax / 64.0, ay / 64.0, x / 64.0, y / 64.0);
}

static void gs_cubic(void* u, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t x, int32_t y) {
    pb_printf((pd_buf*)u, "C%g %g %g %g %g %g", ax / 64.0, ay / 64.0, bx / 64.0, by / 64.0, x / 64.0, y / 64.0);
}

static void gs_close(void* u) {
    pb_putc((pd_buf*)u, 'Z');
}

static void sv_glyph(svgw* S, const pd_draw* a) {
    pd_buf* o = S->o;
    int32_t k;
    double em;

    for (k = 0; k < S->nglyphs && (S->glyphs[k].f != a->font || S->glyphs[k].g != a->glyph); k++) {
    }

    if (k == S->nglyphs) {  /* the first time: its outline defined */
        pd_outline_sink sk;

        if (pd_grow((void**)&S->glyphs, &S->capglyphs, (int64_t)S->nglyphs + 1, sizeof(S->glyphs[0]))) {
            o->err = 1;
            return;
        }

        S->glyphs[k].f = a->font;
        S->glyphs[k].g = a->glyph;
        S->nglyphs++;
        sk.move_to = gs_move;
        sk.line_to = gs_line;
        sk.quad_to = gs_quad;
        sk.cubic_to = gs_cubic;
        sk.close = gs_close;
        pb_printf(o, "<defs><path id=\"g%d\" d=\"", (int)k);
        pd_font_glyph_outline(a->font, a->glyph, &sk, o);
        pb_puts(o, "\"/></defs>\n");
    }

    em = (double)a->size / 65536.0 / a->font->m.units_per_em;
    pb_printf(o, "<use xlink:href=\"#g%d\" transform=\"translate(", (int)k);
    sv_num(o, a->x);
    pb_putc(o, ' ');
    sv_num(o, a->y);
    pb_putc(o, ')');

    if (a->rotation) {
        pb_printf(o, " rotate(%.4f)", a->rotation / 60000.0);
    }

    pb_printf(o, " scale(%.6g %.6g)\"", em * (a->scale ? a->scale : 65536) / 65536.0, -em);
    sv_color(o, "fill", a->color ? a->color : 0xFF000000u);
    pb_puts(o, "/>\n");
}

static void sv_image(svgw* S, const pd_draw* a) {
    pd_buf* o = S->o;
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    int32_t clip = 0;

    if (!S->d || pd_doc_resource(S->d, a->resource, &mime, &data, &len) != PD_OK || !mime || strncmp(mime, "image/", 6)) {
        return;     /* (a drawing inside is drawn as its items) */
    }

    if (a->clip_w > 0 || (a->clip_points && a->clip_npoints >= 3)) {    /* its frame and its shape */
        clip = ++S->ids;
        pb_printf(o, "<defs><clipPath id=\"c%d\">", (int)clip);

        if (a->clip_points && a->clip_npoints >= 3) {
            pb_puts(o, "<path");
            pb_puts(o, " d=\"");
            sv_d(o, a->clip_points, a->clip_npoints, 1);
            pb_puts(o, "\"/>");
        }

        if (a->clip_w > 0) {
            pb_puts(o, "<rect x=\"");
            sv_num(o, a->clip_x);
            pb_puts(o, "\" y=\"");
            sv_num(o, a->clip_y);
            pb_puts(o, "\" width=\"");
            sv_num(o, a->clip_w);
            pb_puts(o, "\" height=\"");
            sv_num(o, a->clip_h);
            pb_puts(o, "\"/>");
        }

        pb_puts(o, "</clipPath></defs>\n");
    }

    pb_puts(o, "<g");

    if (clip) {
        pb_printf(o, " clip-path=\"url(#c%d)\"", (int)clip);
    }

    pb_puts(o, "><image");

    if (a->rotation || a->flip) {   /* turned and flipped about its frame's middle */
        double cx = (a->clip_w > 0 ? a->clip_x + a->clip_w / 2.0 : a->x + a->w / 2.0) / 65536.0;
        double cy = (a->clip_w > 0 ? a->clip_y + a->clip_h / 2.0 : a->y + a->h / 2.0) / 65536.0;

        pb_printf(o, " transform=\"translate(%.3f %.3f) rotate(%.4f) scale(%d %d) translate(%.3f %.3f)\"", cx, cy,
                  a->rotation / 60000.0, a->flip & PD_FLIP_H ? -1 : 1, a->flip & PD_FLIP_V ? -1 : 1, -cx, -cy);
    }

    pb_puts(o, " x=\"");
    sv_num(o, a->x);
    pb_puts(o, "\" y=\"");
    sv_num(o, a->y);
    pb_puts(o, "\" width=\"");
    sv_num(o, a->w);
    pb_puts(o, "\" height=\"");
    sv_num(o, a->h);
    pb_puts(o, "\" preserveAspectRatio=\"none\" xlink:href=\"data:");
    pb_puts(o, mime);
    pb_puts(o, ";base64,");
    pb_base64(o, (const unsigned char*)data, len);
    pb_puts(o, "\"/></g>\n");
}

/* items as an SVG document w by h points, into o */
static void sv_doc(const pd_doc* d, const pd_draw* it, int32_t n, pd_sp x0, pd_sp y0, pd_sp w, pd_sp h, pd_buf* o,
                   int standalone) {
    svgw S;
    int32_t i;

    memset(&S, 0, sizeof(S));
    S.o = o;
    S.d = d;

    if (standalone) {
        pb_puts(o, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    }

    pb_puts(o, "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" version=\"1.1\" "
            "width=\"");
    sv_num(o, w);
    pb_puts(o, "pt\" height=\"");
    sv_num(o, h);
    pb_puts(o, "pt\" viewBox=\"");
    sv_num(o, x0);
    pb_putc(o, ' ');
    sv_num(o, y0);
    pb_putc(o, ' ');
    sv_num(o, w);
    pb_putc(o, ' ');
    sv_num(o, h);
    pb_puts(o, "\">\n");

    for (i = 0; i < n; i++) {
        const pd_draw* a = &it[i];

        if (a->kind == PD_DRAW_PATH && a->points && a->npoints >= 2) {
            sv_path(&S, a);
        } else if (a->kind == PD_DRAW_RULE && a->w > 0 && a->h > 0) {
            pb_puts(o, "<rect x=\"");
            sv_num(o, a->x);
            pb_puts(o, "\" y=\"");
            sv_num(o, a->y);
            pb_puts(o, "\" width=\"");
            sv_num(o, a->w);
            pb_puts(o, "\" height=\"");
            sv_num(o, a->h);
            pb_putc(o, '"');
            sv_color(o, "fill", a->color);
            pb_puts(o, "/>\n");
        } else if (a->kind == PD_DRAW_GLYPH && a->font) {
            sv_glyph(&S, a);
        } else if (a->kind == PD_DRAW_IMAGE) {
            sv_image(&S, a);
        }
    }

    pb_puts(o, "</svg>\n");
    free(S.glyphs);
}

pd_status pd_layout_write_svg(const pd_layout* layout, int32_t page, pd_writer fn, void* user) {
    pd_page_info pi;
    pd_draw* it;
    int32_t n = 0;
    pd_buf o;
    pd_status st;

    if (!layout || !fn || pd_layout_page_info(layout, page, &pi) != PD_OK ||
            pd_layout_page_items(layout, page, NULL, 0, &n) != PD_OK) {
        return PD_ERR_ARG;
    }

    if (!(it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw)))) {
        return PD_ERR_NOMEM;
    }

    pd_layout_page_items(layout, page, it, n, &n);
    memset(&o, 0, sizeof(o));
    sv_doc(pd_layout_doc(layout), it, n, 0, 0, pi.width, pi.height, &o, 1);
    free(it);
    st = o.err ? PD_ERR_NOMEM : pb_flush(&o, fn, user);
    pb_free(&o);
    return st;
}

pd_status pd_layout_write_svg_part(const pd_layout* layout, int32_t page, pd_sp x, pd_sp y, pd_sp w, pd_sp h,
                                   const int32_t* shapes, int32_t nshapes, pd_writer fn, void* user) {
    pd_draw* it;
    int32_t n = 0, i, k, m = 0;
    pd_buf o;
    pd_status st;

    int fit = w <= 0 || h <= 0;
    pd_sp fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;

    if (!layout || !fn || (fit && nshapes <= 0) || (nshapes > 0 && !shapes) ||
            pd_layout_page_items(layout, page, NULL, 0, &n) != PD_OK) {
        return PD_ERR_ARG;
    }

    if (!(it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw)))) {
        return PD_ERR_NOMEM;
    }

    pd_layout_page_items(layout, page, it, n, &n);

    for (i = 0; i < n; i++) {   /* what is drawn for those shapes, in the box */
        pd_sp bx0, by0, bx1, by1;
        int want = nshapes <= 0;

        for (k = 0; k < nshapes && !want; k++) {
            want = it[i].shape == shapes[k];
        }

        pd_draw_bounds(&it[i], &bx0, &by0, &bx1, &by1);

        if (want && (fit || (bx1 >= x && by1 >= y && bx0 <= x + w && by0 <= y + h)) && it[i].kind != PD_DRAW_LINK) {
            if (fit && it[i].kind == PD_DRAW_PATH && it[i].points && it[i].npoints > 0) {
                /* a path's own points, its line round them (not the room a redraw leaves for its corners) */
                int32_t q;
                pd_sp pad = it[i].line_width;

                bx0 = by0 = INT32_MAX;
                bx1 = by1 = INT32_MIN;

                for (q = 0; q < it[i].npoints; q++) {
                    pd_sp px = it[i].points[2 * q], py = it[i].points[2 * q + 1];

                    if (px == PD_PATH_BREAK || px == PD_PATH_CUBIC) {
                        continue;
                    }

                    bx0 = px < bx0 ? px : bx0;
                    by0 = py < by0 ? py : by0;
                    bx1 = px > bx1 ? px : bx1;
                    by1 = py > by1 ? py : by1;
                }

                bx0 -= pad / 2;
                by0 -= pad / 2;
                bx1 += pad / 2;
                by1 += pad / 2;
            }

            if (fit) {  /* the box: round all of it */
                fx0 = m == 0 || bx0 < fx0 ? bx0 : fx0;
                fy0 = m == 0 || by0 < fy0 ? by0 : fy0;
                fx1 = m == 0 || bx1 > fx1 ? bx1 : fx1;
                fy1 = m == 0 || by1 > fy1 ? by1 : fy1;
            }

            it[m++] = it[i];
        }
    }

    if (fit) {
        x = fx0;
        y = fy0;
        w = fx1 - fx0 > 0 ? fx1 - fx0 : 1;
        h = fy1 - fy0 > 0 ? fy1 - fy0 : 1;
    }

    memset(&o, 0, sizeof(o));
    sv_doc(pd_layout_doc(layout), it, m, x, y, w, h, &o, 1);
    free(it);
    st = o.err ? PD_ERR_NOMEM : m == 0 ? PD_ERR_RANGE : pb_flush(&o, fn, user);
    pb_free(&o);
    return st;
}

/* ---- a text box's text as shapes: its letters' outlines, DrawingML freeforms ---- */

typedef struct {
    char k;                     /* 'M', 'L', 'Q' (quadBezTo: control, end), 'C' (two controls, end), 'Z' */
    double x[3], y[3];          /* EMU */
} tseg;

typedef struct {
    uint32_t color;
    long line;                  /* the line it is on (its baseline, turned back, in half points); rules: LONG_MIN */
    tseg* s;
    int n, cap;
} tpath;

typedef struct {
    tpath* p;
    int n, cap;
    int err;
    /* the glyph being taken: font units x 64, y up, to EMU */
    double px, py, sc, hs, c, sn, ox, oy, kx, ky;
    tpath* cur;
} tshapes;

static void ts_add(tshapes* T, char k, const double* xy, int npt) {
    tpath* P = T->cur;
    int i;

    if (!P || T->err) {
        return;
    }

    if (P->n >= P->cap) {
        int cap = P->cap ? P->cap * 2 : 64;
        tseg* g = (tseg*)realloc(P->s, (size_t)cap * sizeof(tseg));

        if (!g) {
            T->err = 1;
            return;
        }

        P->s = g;
        P->cap = cap;
    }

    P->s[P->n].k = k;

    for (i = 0; i < npt; i++) {
        P->s[P->n].x[i] = xy[2 * i];
        P->s[P->n].y[i] = xy[2 * i + 1];
    }

    P->n++;
}

/* a point of the glyph (font units x 64, y up) into EMU: scaled, turned about the pen, put where the pen is */
static void ts_map(const tshapes* T, int32_t fx, int32_t fy, double* out) {
    double gx = fx * T->sc * T->hs, gy = -fy * T->sc;
    double x = T->px + gx * T->c - gy * T->sn, y = T->py + gx * T->sn + gy * T->c;

    out[0] = (x - T->ox) * T->kx;
    out[1] = (y - T->oy) * T->ky;
}

static void ts_move(void* u, int32_t x, int32_t y) {
    double p[2];

    ts_map((tshapes*)u, x, y, p);
    ts_add((tshapes*)u, 'M', p, 1);
}

static void ts_line(void* u, int32_t x, int32_t y) {
    double p[2];

    ts_map((tshapes*)u, x, y, p);
    ts_add((tshapes*)u, 'L', p, 1);
}

static void ts_quad(void* u, int32_t cx, int32_t cy, int32_t x, int32_t y) {
    double p[4];

    ts_map((tshapes*)u, cx, cy, p);
    ts_map((tshapes*)u, x, y, p + 2);
    ts_add((tshapes*)u, 'Q', p, 2);
}

static void ts_cubic(void* u, int32_t c1x, int32_t c1y, int32_t c2x, int32_t c2y, int32_t x, int32_t y) {
    double p[6];

    ts_map((tshapes*)u, c1x, c1y, p);
    ts_map((tshapes*)u, c2x, c2y, p + 2);
    ts_map((tshapes*)u, x, y, p + 4);
    ts_add((tshapes*)u, 'C', p, 3);
}

static void ts_close(void* u) {
    ts_add((tshapes*)u, 'Z', NULL, 0);
}

/* the freeform of that colour on that line, made the first time */
static tpath* ts_path(tshapes* T, uint32_t color, long line) {
    int i;

    for (i = 0; i < T->n; i++) {
        if (T->p[i].color == color && T->p[i].line == line) {
            return &T->p[i];
        }
    }

    if (T->n >= T->cap) {
        int cap = T->cap ? T->cap * 2 : 16;
        tpath* g = (tpath*)realloc(T->p, (size_t)cap * sizeof(tpath));

        if (!g) {
            T->err = 1;
            return NULL;
        }

        T->p = g;
        T->cap = cap;
    }

    memset(&T->p[T->n], 0, sizeof(tpath));
    T->p[T->n].color = color;
    T->p[T->n].line = line;
    return &T->p[T->n++];
}

/* the story a block is in (the top of its parents) */
static pd_block_id ts_story(const pd_doc* d, pd_block_id b) {
    pd_block_info bi;
    int guard = 0;

    while (b && pd_doc_block_info(d, b, &bi) == PD_OK && bi.parent && guard++ < 64) {
        b = bi.parent;
    }

    return b;
}

static void ts_colour(pd_buf* o, uint32_t c) {
    unsigned a = c >> 24;

    pb_printf(o, "<a:srgbClr val=\"%06X\"", (unsigned)(c & 0xFFFFFFu));

    if (a && a < 255) {
        pb_printf(o, "><a:alpha val=\"%u\"/></a:srgbClr>", a * 100000u / 255);
    } else {
        pb_puts(o, "/>");
    }
}

static long long ts_emu(double v) {
    return v > 2e12 ? (long long)2e12 : v < -2e12 ? (long long)-2e12 : (long long)floor(v + 0.5);
}

pd_status pd_layout_text_shapes(const pd_layout* layout, int32_t page, pd_block_id story, pd_sp ox, pd_sp oy,
                                double kx, double ky, pd_writer fn, void* user) {
    const pd_doc* d;
    pd_draw* it;
    int32_t n = 0, i, k;
    tshapes T;
    pd_buf o;
    pd_status st;
    double gx0 = 1e300, gy0 = 1e300, gx1 = -1e300, gy1 = -1e300;
    int id = 0;

    if (!layout || !fn || !story || kx <= 0 || ky <= 0 || pd_layout_page_items(layout, page, NULL, 0, &n) != PD_OK) {
        return PD_ERR_ARG;
    }

    if (!(it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw)))) {
        return PD_ERR_NOMEM;
    }

    pd_layout_page_items(layout, page, it, n, &n);
    d = pd_layout_doc(layout);
    memset(&T, 0, sizeof(T));
    T.ox = ox;
    T.oy = oy;
    T.kx = kx;
    T.ky = ky;

    for (i = 0; i < n && !T.err; i++) {
        const pd_draw* a = &it[i];

        if (!a->block || ts_story(d, a->block) != story) {
            continue;
        }

        if (a->kind == PD_DRAW_GLYPH && a->font && a->size > 0 && (a->color >> 24)) {
            pd_font_metrics fm;
            pd_outline_sink sink = { ts_move, ts_line, ts_quad, ts_cubic, ts_close };
            double r = a->rotation / 60000.0 * 3.14159265358979323846 / 180;
            long line;

            if (pd_font_get_metrics(a->font, &fm) != PD_OK || fm.units_per_em <= 0) {
                continue;
            }

            T.c = cos(r);
            T.sn = sin(r);
            T.px = a->x;
            T.py = a->y;
            T.sc = (double)a->size / fm.units_per_em / 64;
            T.hs = a->scale > 0 ? a->scale / 65536.0 : 1;
            line = (long)floor((-a->x * T.sn + a->y * T.c) / 32768.0 + 0.5);     /* (half points) */
            T.cur = ts_path(&T, a->color, line);
            pd_font_glyph_outline(a->font, a->glyph, &sink, &T);
        } else if (a->kind == PD_DRAW_RULE && a->w > 0 && a->h > 0 && (a->color >> 24)) {
            double q[8];    /* an underline, a strike, a highlight: its rectangle */

            q[0] = (a->x - ox) * kx;
            q[1] = (a->y - oy) * ky;
            q[2] = (a->x + a->w - ox) * kx;
            q[3] = q[1];
            q[4] = q[2];
            q[5] = (a->y + a->h - oy) * ky;
            q[6] = q[0];
            q[7] = q[5];
            T.cur = ts_path(&T, a->color, LONG_MIN);
            ts_add(&T, 'M', q, 1);
            ts_add(&T, 'L', q + 2, 1);
            ts_add(&T, 'L', q + 4, 1);
            ts_add(&T, 'L', q + 6, 1);
            ts_add(&T, 'Z', NULL, 0);
        } else if (a->kind == PD_DRAW_PATH && a->points && a->npoints >= 3 && (a->fill >> 24)) {
            int32_t q;  /* a rule drawn turned (with turned text): its polygon */

            T.cur = ts_path(&T, a->fill, LONG_MIN);

            for (q = 0; q < a->npoints; q++) {
                double p[2];

                if (a->points[2 * q] == PD_PATH_BREAK || a->points[2 * q] == PD_PATH_CUBIC) {
                    continue;
                }

                p[0] = (a->points[2 * q] - ox) * kx;
                p[1] = (a->points[2 * q + 1] - oy) * ky;
                ts_add(&T, q == 0 ? 'M' : 'L', p, 1);
            }

            ts_add(&T, 'Z', NULL, 0);
        }
    }

    free(it);
    memset(&o, 0, sizeof(o));

    for (i = 0; i < T.n; i++) {     /* the group: round all of them */
        for (k = 0; k < T.p[i].n; k++) {
            int np = T.p[i].s[k].k == 'C' ? 3 : T.p[i].s[k].k == 'Q' ? 2 : T.p[i].s[k].k == 'Z' ? 0 : 1, q;

            for (q = 0; q < np; q++) {
                gx0 = T.p[i].s[k].x[q] < gx0 ? T.p[i].s[k].x[q] : gx0;
                gy0 = T.p[i].s[k].y[q] < gy0 ? T.p[i].s[k].y[q] : gy0;
                gx1 = T.p[i].s[k].x[q] > gx1 ? T.p[i].s[k].x[q] : gx1;
                gy1 = T.p[i].s[k].y[q] > gy1 ? T.p[i].s[k].y[q] : gy1;
            }
        }
    }

    if (!T.err && gx1 > gx0) {
        long long ext_x = ts_emu(gx1 - gx0) > 0 ? ts_emu(gx1 - gx0) : 1, ext_y = ts_emu(gy1 - gy0) > 0 ? ts_emu(gy1 - gy0) : 1;

        pb_printf(&o, "<wpg:wgp><wpg:cNvGrpSpPr/><wpg:grpSpPr><a:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" "
                  "cy=\"%lld\"/><a:chOff x=\"%lld\" y=\"%lld\"/><a:chExt cx=\"%lld\" cy=\"%lld\"/></a:xfrm></wpg:grpSpPr>",
                  ts_emu(gx0), ts_emu(gy0), ext_x, ext_y, ts_emu(gx0), ts_emu(gy0), ext_x, ext_y);

        for (i = 0; i < T.n; i++) {
            const tpath* P = &T.p[i];
            double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
            long long cx, cy;

            for (k = 0; k < P->n; k++) {
                int np = P->s[k].k == 'C' ? 3 : P->s[k].k == 'Q' ? 2 : P->s[k].k == 'Z' ? 0 : 1, q;

                for (q = 0; q < np; q++) {
                    x0 = P->s[k].x[q] < x0 ? P->s[k].x[q] : x0;
                    y0 = P->s[k].y[q] < y0 ? P->s[k].y[q] : y0;
                    x1 = P->s[k].x[q] > x1 ? P->s[k].x[q] : x1;
                    y1 = P->s[k].y[q] > y1 ? P->s[k].y[q] : y1;
                }
            }

            if (x1 < x0) {
                continue;
            }

            cx = ts_emu(x1 - x0) > 0 ? ts_emu(x1 - x0) : 1;
            cy = ts_emu(y1 - y0) > 0 ? ts_emu(y1 - y0) : 1;
            pb_printf(&o, "<wps:wsp><wps:cNvPr id=\"%%ID%d%%\" name=\"Text as shape %%ID%d%%\"/><wps:cNvSpPr/><wps:spPr>"
                      "<a:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm><a:custGeom><a:avLst/>"
                      "<a:gdLst/><a:ahLst/><a:cxnLst/><a:rect l=\"l\" t=\"t\" r=\"r\" b=\"b\"/><a:pathLst><a:path w=\"%lld\" "
                      "h=\"%lld\" stroke=\"0\">", id, id, ts_emu(x0), ts_emu(y0), cx, cy, cx, cy);
            id++;

            for (k = 0; k < P->n; k++) {
                const tseg* g = &P->s[k];
                int np = g->k == 'C' ? 3 : g->k == 'Q' ? 2 : g->k == 'Z' ? 0 : 1, q;
                const char* el = g->k == 'M' ? "moveTo" : g->k == 'L' ? "lnTo" : g->k == 'Q' ? "quadBezTo" :
                                 g->k == 'C' ? "cubicBezTo" : NULL;

                if (!el) {
                    pb_puts(&o, "<a:close/>");
                    continue;
                }

                pb_printf(&o, "<a:%s>", el);

                for (q = 0; q < np; q++) {
                    pb_printf(&o, "<a:pt x=\"%lld\" y=\"%lld\"/>", ts_emu(g->x[q] - x0), ts_emu(g->y[q] - y0));
                }

                pb_printf(&o, "</a:%s>", el);
            }

            pb_puts(&o, "</a:path></a:pathLst></a:custGeom><a:solidFill>");
            ts_colour(&o, P->color);
            pb_puts(&o, "</a:solidFill><a:ln><a:noFill/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>");
        }

        pb_puts(&o, "</wpg:wgp>");
    }

    for (i = 0; i < T.n; i++) {
        free(T.p[i].s);
    }

    free(T.p);
    st = T.err || o.err ? PD_ERR_NOMEM : id == 0 ? PD_ERR_RANGE : pb_flush(&o, fn, user);
    pb_free(&o);
    return st;
}

/* a drawing as SVG into a buffer (for HTML, which puts it inline): 0 when it is no drawing */
int pd_drawing_svg_buf(const pd_doc* doc, pd_res_id res, pd_sp w, pd_sp h, pd_buf* o) {
    pd_drawlist l;

    if (pd_drawing_list(doc, res, w, h, &l) != PD_OK) {
        return 0;
    }

    sv_doc(doc, l.d, l.n, 0, 0, w, h, o, 0);
    pd_drawlist_free(&l);
    return 1;
}

pd_status pd_drawing_to_svg(const pd_doc* doc, pd_res_id res, pd_sp width, pd_sp height, pd_writer fn, void* user) {
    pd_drawlist l;
    pd_buf o;
    pd_status st;

    if (!doc || !fn || width <= 0 || height <= 0) {
        return PD_ERR_ARG;
    }

    if ((st = pd_drawing_list(doc, res, width, height, &l)) != PD_OK) {
        return st;
    }

    memset(&o, 0, sizeof(o));
    sv_doc(doc, l.d, l.n, 0, 0, width, height, &o, 1);
    pd_drawlist_free(&l);
    st = o.err ? PD_ERR_NOMEM : pb_flush(&o, fn, user);
    pb_free(&o);
    return st;
}
