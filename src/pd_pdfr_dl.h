/*
 * Parade PDF reader: the display list a page's content becomes
 * (pd_pdfr_draw.c makes it, pd_pdfr_raster.c draws it)
 *
 * Coordinates are in the list's own space: for a page, points on the page
 * as shown (top left origin, y down); for a tiling pattern's cell, its
 * pattern space. Paths are doubles: 0 x y (move), 1 x y (line),
 * 2 x1 y1 x2 y2 x y (cubic), 3 (close).
 */

#ifndef PD_PDFR_DL_H
#define PD_PDFR_DL_H

#include "pd_pdfr.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { DI_FILL = 0, DI_STROKE, DI_GLYPH, DI_IMAGE, DI_SHADE };
enum { DP_SOLID = 0, DP_SHADE, DP_TILE };
enum { BL_NORMAL = 0, BL_MULTIPLY, BL_SCREEN, BL_DARKEN, BL_LIGHTEN, BL_OVERLAY, BL_DIFFERENCE };

typedef struct pfunc pfunc;

/* a colour space, made once a page */
typedef struct pcspace pcspace;
struct pcspace {
    int kind;                   /* CS_* */
    int n;                      /* components */
    pcspace* base;              /* Indexed, Separation, DeviceN: the space the colour ends in */
    unsigned char* lookup;      /* Indexed */
    int hival;
    pfunc* tint;                /* Separation, DeviceN */
    double white[3], range[4];  /* Lab */
    int none;                   /* a Separation /None: paints nothing */
};

enum { CS_GRAY = 0, CS_RGB, CS_CMYK, CS_LAB, CS_INDEXED, CS_SEP, CS_PATTERN };

struct pfunc {
    int type;
    int m, n;                   /* inputs, outputs */
    double domain[8], range[32];
    /* 0: sampled */
    int size[4], bps;
    double encode[8], decode[32];
    double* samples;
    /* 2 */
    double c0[16], c1[16], e;
    /* 3 */
    pfunc** fns;
    int k;
    double bounds[16], enc3[32];
    /* 4 */
    double* prog;               /* opcodes and numbers */
    int nprog;
};

int pfn_eval(const pfunc* f, const double* in, double* out);
void pcs_to_rgb(const pcspace* cs, const double* c, double* rgb);

/* a shading, its colours to hand */
typedef struct {
    int type;
    pcspace* cs;
    double coords[6];
    double domain[2];
    int extend[2];
    double bbox[4];
    int has_bbox;
    uint32_t lut[256];          /* 1..3 (t), and 4..7 when functions colour the mesh: t to RGB */
    int has_lut;
    pfunc* fn[8];               /* type 1 (or a function per output) */
    int nfn;
    double matrix[6];           /* type 1's domain to shading space */
    double fdomain[4];
    float* tri;                 /* meshes: triangles, 3 vertices of x y r g b */
    int32_t ntri;
    int has_background;
    uint32_t background;
} pshade;

/* a picture, decoded */
typedef struct pimage {
    int32_t w, h;
    int stencil;                /* a mask: one byte a pixel, 255 painted */
    int interpolate;
    unsigned char* px;          /* RGBA (not premultiplied), or the mask */
    struct pimage* half;        /* the same at half the size, made when drawn small */
    size_t bytes;
} pimage;

typedef struct {
    pv stream;                  /* an image XObject (decoded through the document's cache) */
    pv res;
    pimage* own;                /* or an inline image, decoded already */
    uint32_t mask_rgb;          /* a stencil's colour is the item's paint */
} dimgref;

typedef struct {
    uint8_t kind;               /* DP_* */
    uint8_t blend;
    float alpha;
    uint32_t rgb;               /* 0xRRGGBB */
    int32_t index;              /* shading or tile */
    double m[6];                /* shading space / pattern space to the list's */
} dpaint;

typedef struct {
    uint8_t type;               /* DI_* */
    uint8_t evenodd;
    uint8_t cap, join;
    int32_t clip;               /* clip node, -1 none */
    int32_t smask;              /* soft mask, -1 none */
    dpaint paint;
    int32_t path, npath;        /* doubles in the path store */
    float lw, miter, dash_phase;
    int32_t dash, ndash;        /* floats in the dash store */
    pfont* font;
    uint32_t code;
    double m[6];                /* glyph space or the image's unit square to the list's */
    int32_t image;              /* the picture (an index into the list's images) */
} ditem;

typedef struct {
    int32_t parent;
    int32_t path, npath;        /* npath 0: an empty clip (nothing shown) */
    uint8_t evenodd;
} dclip;

typedef struct ppage_cache dlist;

typedef struct {
    dlist* cell;                /* the cell's content in pattern space */
    double xstep, ystep, bbox[4];
    int colored;
} dtile;

typedef struct {
    dlist* group;               /* the mask's group, in the list's space */
    int luminosity;             /* else its alpha */
    uint32_t backdrop;          /* 0xRRGGBB, luminosity only */
} dsmask;

typedef struct {
    pd_pdfr_char pub;
    int32_t stream;             /* which of the page's content streams, -1 a form's */
    uint32_t op_start, op_end;  /* the text-showing operation's bytes in it */
    uint32_t str_start, str_end;    /* the string's bytes (the one this character is in) */
    uint32_t byte_off;          /* the code's place in the decoded string */
    uint8_t byte_len;
    pfont* font;
    double fs;
    double tm[6];               /* the text rendering matrix at the character (text space x 1 to the page) */
    double um[6];               /* the same to user space (the page's base matrix undone) */
    uint32_t rgb;               /* its fill colour */
    double w0;                  /* the glyph's advance, glyph space / 1000 */
    double tc, tw;              /* the character and word spacing it was shown with */
    uint8_t vertical;           /* in a vertical font */
    unsigned char code[4];      /* the code's bytes in the string (byte_len of them, 4 at most) */
} tchar;

struct ppage_cache {
    ditem* items;
    int32_t n, cap;
    double* pts;
    int32_t npts, cappts;
    float* dashes;
    int32_t ndashes, capdashes;
    dclip* clips;
    int32_t nclips, capclips;
    pshade** shades;
    int32_t nshades;
    dtile* tiles;
    int32_t ntiles;
    dsmask* smasks;
    int32_t nsmasks;
    dimgref* images;            /* the pictures drawn, decoded when drawn (inline ones decoded and owned) */
    int32_t nimages, capimages;
    tchar* chars;
    int32_t nchars, capchars;
    int32_t annots_from;        /* the items of annotations start here */
    int32_t changes;
    double width, height;
    parena* arena;              /* colour spaces, functions, shadings */
};

void dl_free(dlist* L);
/* the page's list, made if it is not (annotations included) */
dlist* pd_page_list(pdoc* d, int32_t page);
/* draw a list: M its space to the device's pixels; clip rectangle cx0..cx1 in device pixels */
typedef struct {
    uint8_t* buf;
    int32_t w, h, stride;
    int bgra;
} rtarget;
void pr_draw(pdoc* d, dlist* L, const double* M, rtarget* t, int32_t from, int32_t to);

/* matrices: r = a then b (a point goes through a first) */
static inline void mat_mul(const double* a, const double* b, double* r) {
    double t[6];

    t[0] = a[0] * b[0] + a[1] * b[2];
    t[1] = a[0] * b[1] + a[1] * b[3];
    t[2] = a[2] * b[0] + a[3] * b[2];
    t[3] = a[2] * b[1] + a[3] * b[3];
    t[4] = a[4] * b[0] + a[5] * b[2] + b[4];
    t[5] = a[4] * b[1] + a[5] * b[3] + b[5];
    r[0] = t[0];
    r[1] = t[1];
    r[2] = t[2];
    r[3] = t[3];
    r[4] = t[4];
    r[5] = t[5];
}

static inline int mat_inv(const double* m, double* r) {
    double det = m[0] * m[3] - m[1] * m[2], t[6];

    if (det == 0 || !(det == det)) {
        return 0;
    }

    t[0] = m[3] / det;
    t[1] = -m[1] / det;
    t[2] = -m[2] / det;
    t[3] = m[0] / det;
    t[4] = -(m[4] * t[0] + m[5] * t[2]);
    t[5] = -(m[4] * t[1] + m[5] * t[3]);
    r[0] = t[0];
    r[1] = t[1];
    r[2] = t[2];
    r[3] = t[3];
    r[4] = t[4];
    r[5] = t[5];
    return 1;
}

static inline void mat_pt(const double* m, double x, double y, double* ox, double* oy) {
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}

/* the page's base matrix: PDF user space to the page as shown */
void pd_page_base(pdoc* d, int32_t page, double* m);

/* pictures (pd_pdfr_draw.c): decoded from a stream (an XObject, or an inline image's dictionary and data), and
   through the document's cache, which keeps the most recently drawn within a budget */
pimage* pdr_image_decode(pdoc* d, pv dict, const unsigned char* data, size_t len, pv res);
pimage* pdr_image_fetch(pdoc* d, dimgref* r);
void pimage_free(pimage* im);
void pd_image_cache_free(pdoc* d);

#endif
