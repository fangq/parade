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
 * the plain EMF records beside them. Clipping, raster operations other
 * than copying, and hatched brushes (filled solid) are not modelled.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"
#include "pd_internal.h"

#define MAX_OBJ 1024

typedef struct {
    int kind;                   /* 0 none, 1 pen, 2 brush, 3 font, 4 other (a slot taken) */
    uint32_t color;             /* 0xAARRGGBB; pens and brushes */
    int null;                   /* a null pen or brush */
    double width;               /* pens: logical units */
    double height;              /* fonts: logical units; < 0 character height */
    int weight, italic;
    char face[64];
} gobj;

typedef struct {
    double wox, woy, wex, wey;  /* window */
    double vox, voy, vex, vey;  /* viewport */
    int map;                    /* 1 MM_TEXT, 7 isotropic, 8 anisotropic */
    double m[6];                /* world transform: x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5 */
    gobj pen, brush, font;
    uint32_t text_color;
    int text_align;
    double cx, cy;              /* current position, logical */
    int poly_fill;
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
static void out_path(mf* M, const double* xy, int n, int closed, int fill, int stroke) {
    uint32_t fc = fill && !M->g.brush.null && M->g.brush.kind == 2 ? M->g.brush.color : 0;
    uint32_t lc = stroke && !M->g.pen.null && M->g.pen.kind == 1 ? M->g.pen.color : 0;
    double lw = M->g.pen.width > 0 ? map_len(M, M->g.pen.width) : 0.5 * 65536;    /* 0: a hairline */
    int i;

    if (n < 2 || (!fc && !lc)) {
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

    pb_printf(M->o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d}", closed, (unsigned)fc, (unsigned)lc,
              (int)(lw < 1 ? 1 : lw));
}

/* the path being built, or straight out: points in logical units */
static void figure(mf* M, const double* lxy, int n, int closed, int fill, int stroke) {
    double* xy;
    int i;

    if (n < 1) {
        return;
    }

    if (M->in_path) {
        if (pd_grow((void**)&M->pp, &M->cappp, (int64_t)M->npp + 2 * n + 2, sizeof(double))) {
            return;
        }

        if (M->npp > 0 && !M->fig_open) {
            M->pp[M->npp++] = NAN;
            M->pp[M->npp++] = NAN;
        }

        for (i = 0; i < n; i++) {
            map_pt(M, lxy[2 * i], lxy[2 * i + 1], &M->pp[M->npp], &M->pp[M->npp + 1]);
            M->npp += 2;
        }

        M->fig_open = !closed;
        return;
    }

    if ((xy = (double*)malloc((size_t)n * 2 * sizeof(double))) == NULL) {
        return;
    }

    for (i = 0; i < n; i++) {
        map_pt(M, lxy[2 * i], lxy[2 * i + 1], &xy[2 * i], &xy[2 * i + 1]);
    }

    out_path(M, xy, n, closed, fill, stroke);
    free(xy);
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
static void ellipse(mf* M, double l, double t, double r, double b) {
    double xy[128];
    int k;

    for (k = 0; k < 64; k++) {
        double a = k * 6.283185307179586 / 64;

        xy[2 * k] = (l + r) / 2 + (r - l) / 2 * cos(a);
        xy[2 * k + 1] = (t + b) / 2 + (b - t) / 2 * sin(a);
    }

    figure(M, xy, 64, 1, 1, 1);
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

    if (!res) {
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

    if (nchars == 0 || blank) {
        return;
    }

    /* the baseline from the reference point: top, bottom or the baseline itself */
    if ((al & 24) == 24) {
        /* baseline */
    } else if (al & 8) {
        y -= em * 0.21;
    } else {
        y += em * 0.9;
    }

    map_pt(M, x, y, &ox, &oy);
    sz = map_len(M, em);
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
    out_path(M, M->pp, M->npp / 2, 1, fill, stroke);
    M->npp = 0;
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

static void emf_play(mf* M, const unsigned char* p, size_t n) {
    size_t at = 0;

    while (at + 8 <= n) {
        uint32_t type = u32(p + at), size = u32(p + at + 4);
        const unsigned char* r = p + at;

        if (size < 8 || size > n - at || (size & 3)) {
            break;
        }

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
                    o->color = colorref(u32(r + 24));
                }

                break;

            case 95:    /* EXTCREATEPEN */
                if (size >= 44 && u32(r + 8) < MAX_OBJ) {
                    gobj* o = &M->obj[u32(r + 8)];

                    memset(o, 0, sizeof(*o));
                    o->kind = 1;
                    o->null = (u32(r + 28) & 15) == 5 || u32(r + 36) == 1;    /* PS_NULL, or a null brush */
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
                        if (to && M->in_path && M->fig_open) {
                            figure(M, xy + 2, k - 1, 0, 0, 1);
                        } else {
                            figure(M, xy, k, 0, 0, 1);
                        }

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

                    if (ob <= size && cb <= size - ob && obits <= size && cbits <= size - obits && cb && rop_draws(u32(r + 68), r + ob, cb)) {
                        out_bitmap(M, dib_resource(M->d, r + ob, cb, r + obits, cbits), s32(r + 24), s32(r + 28),
                                   s32(r + 72), s32(r + 76));
                    }
                }

                break;

            case 80:    /* SETDIBITSTODEVICE */
                if (size >= 76) {
                    uint32_t ob = u32(r + 48), cb = u32(r + 52), obits = u32(r + 56), cbits = u32(r + 60);

                    if (ob <= size && cb <= size - ob && obits <= size && cbits <= size - obits && cb) {
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
                    } else if (ob <= size && cb <= size - ob && obits <= size && cbits <= size - obits &&
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

            case 0x041B:    /* RECTANGLE bottom, right, top, left */
                rect(M, P16(3), P16(2), P16(1), P16(0));
                break;

            case 0x061C:    /* ROUNDRECT h, w, bottom, right, top, left */
                rect(M, P16(5), P16(4), P16(3), P16(2));
                break;

            case 0x0418:    /* ELLIPSE bottom, right, top, left */
                ellipse(M, P16(3), P16(2), P16(1), P16(0));
                break;

            case 0x02FA: {  /* CREATEPENINDIRECT style, width x, width y, colour */
                int s = wmf_new_slot(M);

                if (s >= 0) {
                    gobj* o = &M->obj[s];

                    memset(o, 0, sizeof(*o));
                    o->kind = 1;
                    o->null = (P16(0) & 15) == 5;
                    o->width = P16(1);
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

pd_res_id pd_metafile_drawing(pd_doc* d, const unsigned char* p, size_t n, pd_res_id src) {
    mf* M;
    pd_buf o;
    pd_res_id res = 0;
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
            emf_play(M, p, n);
        } else {
            wmf_play(M, p, n);
        }

        pb_puts(&o, "]}");

        if (!o.err && !M->first && pd_doc_add_resource(d, "application/vnd.parade.drawing+json", o.p, o.n, &res) != PD_OK) {
            res = 0;
        }
    }

    pb_free(&o);
    free(M->pp);
    free(M);
    return res;
}
