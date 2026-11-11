/*
 * Parade PDF reader: the content-stream interpreter
 *
 * A page's content streams (and the forms, patterns, Type 3 glyphs and
 * annotation appearances they draw) are run once into a display list in
 * page space; drawing it at any scale is pd_pdfr_raster.c's. The text shown
 * is recorded as it goes: each character's code point, its box on the page
 * and where in the content stream it came from, for selection, search and
 * editing.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr_dl.h"

#define MAX_FORM_DEPTH 16
#define MAX_GSTATE 128
#define LIST_KEEP 24            /* pages whose lists are kept */

pcspace* pcs_parse(pdoc* d, parena** A, pv v, pv res, int depth);
pshade* psh_parse(pdoc* d, parena** A, pv v, pv res);

typedef struct {
    double ctm[6];
    int32_t clip;
    pcspace* fcs, *scs;
    double fc[32], sc[32];
    pv fpat, spat;
    double lw, miter;
    int cap, join;
    float dash[16];
    int ndash;
    float dphase;
    double fa, sa;
    int blend;
    int32_t smask;
    pfont* font;
    double fs, tc, tw, th, tl, trise;
    int tmode;
} gstate;

typedef struct interp {
    pdoc* d;
    dlist* L;
    gstate* gs;
    int ngs, capgs;
    pv res;
    pbuf path;                  /* doubles, in list space */
    int path_open;
    double cx, cy, sx, sy;      /* the current point and the subpath's start, user space */
    int pending_clip, pending_eo;
    double tm[6], tlm[6];
    pbuf textclip;
    int textclip_used;
    int depth;
    int stream;
    int hidden;
    int marked[64];             /* each open marked-content sequence: 1 if it hid */
    int nmarked;
    int uncolored;              /* a Type 3 glyph with d1: its colour the text's */
    int text;                   /* record the text shown */
    int32_t run;
    const unsigned char* src;   /* the stream being run (offsets in it are recorded) */
    size_t op_start, op_end;
    double base_inv[6];         /* the page's base matrix undone */
    size_t str_start[64], str_end[64];
    int nstr;
    int type3_depth;
    size_t* bounds;             /* the page's content streams run as one: where each starts in it */
    int nbounds;
} interp;

static void run_stream(interp* I, const unsigned char* data, size_t len, pv res);

/* ------------------------------------------------------------------ */
/* the list                                                           */
/* ------------------------------------------------------------------ */

static int grow(void** p, int32_t* cap, int32_t need, size_t elem) {
    if (need <= *cap) {
        return 1;
    }

    {
        int32_t nc = *cap ? *cap : 64;
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

static int32_t add_path(dlist* L, const double* v, int32_t n) {
    int32_t at = L->npts;

    if (!grow((void**)&L->pts, &L->cappts, L->npts + n, sizeof(double))) {
        return -1;
    }

    memcpy(L->pts + L->npts, v, sizeof(double) * (size_t)n);
    L->npts += n;
    return at;
}

static ditem* add_item(interp* I, int type) {
    dlist* L = I->L;
    ditem* it;

    if (I->hidden) {
        return NULL;
    }

    if (!grow((void**)&L->items, &L->cap, L->n + 1, sizeof(ditem))) {
        return NULL;
    }

    it = &L->items[L->n++];
    memset(it, 0, sizeof(*it));
    it->type = (uint8_t)type;
    it->clip = I->gs[I->ngs - 1].clip;
    it->smask = I->gs[I->ngs - 1].smask;
    it->image = -1;
    return it;
}

void dl_free(dlist* L) {
    int32_t i;

    if (!L) {
        return;
    }

    for (i = 0; i < L->ntiles; i++) {
        dl_free(L->tiles[i].cell);
    }

    for (i = 0; i < L->nsmasks; i++) {
        dl_free(L->smasks[i].group);
    }

    free(L->smasks);

    for (i = 0; i < L->nshades; i++) {
        if (L->shades[i]) {
            free(L->shades[i]->tri);
        }
    }

    for (i = 0; i < L->nimages; i++) {
        pimage_free(L->images[i].own);
    }

    free(L->items);
    free(L->pts);
    free(L->dashes);
    free(L->clips);
    free(L->shades);
    free(L->tiles);
    free(L->images);
    free(L->chars);
    pa_free(L->arena);
    free(L);
}

/* ------------------------------------------------------------------ */
/* graphics state                                                     */
/* ------------------------------------------------------------------ */

static gstate* gs(interp* I) {
    return &I->gs[I->ngs - 1];
}

static void gs_save(interp* I) {
    if (I->ngs >= MAX_GSTATE) {
        I->ngs++;   /* counted, not kept: the restore that matches drops it */
        return;
    }

    if (!grow((void**)&I->gs, &I->capgs, I->ngs + 1, sizeof(gstate))) {
        return;
    }

    I->gs[I->ngs] = I->gs[I->ngs - 1];
    I->ngs++;
}

static void gs_restore(interp* I, int floor) {
    if (I->ngs > floor) {
        I->ngs--;

        if (I->ngs > I->capgs) {
            I->ngs = I->capgs;
        }
    }
}

static void gs_init(gstate* g, const double* ctm, parena** A) {
    memset(g, 0, sizeof(*g));
    memcpy(g->ctm, ctm, sizeof(g->ctm));
    g->clip = -1;
    g->smask = -1;
    g->fcs = (pcspace*)pa_alloc(A, sizeof(pcspace));
    g->fcs->kind = CS_GRAY;
    g->fcs->n = 1;
    g->scs = g->fcs;
    g->lw = 1;
    g->miter = 10;
    g->fa = g->sa = 1;
    g->th = 1;
}

/* the fill or stroke paint */
static void make_paint(interp* I, int stroke, dpaint* p) {
    gstate* g = gs(I);
    pcspace* cs = stroke ? g->scs : g->fcs;
    const double* c = stroke ? g->sc : g->fc;
    double rgb[3];

    memset(p, 0, sizeof(*p));
    p->alpha = (float)(stroke ? g->sa : g->fa);
    p->blend = (uint8_t)g->blend;

    if (cs && cs->kind == CS_PATTERN) {
        pv pat = stroke ? g->spat : g->fpat;
        int64_t type = pd_getint(I->d, pat, "PatternType", 0);
        double pm[6] = { 1, 0, 0, 1, 0, 0 };
        dlist* L = I->L;

        pd_matrix(I->d, pd_get(I->d, pat, "Matrix"), pm);

        /* a pattern's space is the default space of the page (or the form it is in): its matrix then the
           base matrix the list started with */
        mat_mul(pm, I->gs[0].ctm, p->m);

        if (type == 2) {
            pshade* s = psh_parse(I->d, &L->arena, pd_get(I->d, pat, "Shading"), I->res);

            if (s) {
                pshade** q = (pshade**)realloc(L->shades, sizeof(pshade*) * (size_t)(L->nshades + 1));

                if (q) {
                    L->shades = q;
                    L->shades[L->nshades] = s;
                    p->kind = DP_SHADE;
                    p->index = L->nshades++;
                    return;
                }
            }

            p->rgb = 0x808080;
            return;
        }

        if (type == 1 && pat.t == PV_STREAM) {
            dtile t;
            dtile* q;
            int64_t paint = pd_getint(I->d, pat, "PaintType", 1);

            memset(&t, 0, sizeof(t));
            t.colored = paint != 2;
            t.xstep = pd_getnum(I->d, pat, "XStep", 0);
            t.ystep = pd_getnum(I->d, pat, "YStep", 0);
            pd_rect(I->d, pd_get(I->d, pat, "BBox"), t.bbox);

            if (t.xstep == 0 || t.ystep == 0 || fabs(t.xstep) > 1e6 || fabs(t.ystep) > 1e6 || I->depth > 6) {
                p->rgb = 0x808080;
                return;
            }

            /* the cell's content, in pattern space */
            {
                interp J;
                size_t len;
                unsigned char* data = pd_stream_data(I->d, pat, &len, NULL, NULL);
                double id[6] = { 1, 0, 0, 1, 0, 0 };
                pv pres = pd_get(I->d, pat, "Resources");

                t.cell = (dlist*)calloc(1, sizeof(dlist));

                if (!t.cell || !data) {
                    free(t.cell);
                    free(data);
                    p->rgb = 0x808080;
                    return;
                }

                memset(&J, 0, sizeof(J));
                J.d = I->d;
                J.L = t.cell;
                J.depth = I->depth + 1;
                J.stream = -1;
                J.capgs = 0;
                J.gs = NULL;
                grow((void**)&J.gs, &J.capgs, 4, sizeof(gstate));
                J.ngs = 1;
                gs_init(&J.gs[0], id, &t.cell->arena);

                if (!t.colored) {   /* the cell painted in the colour given with the pattern */
                    pcspace* base = cs->base;
                    double crgb[3];

                    pcs_to_rgb(base, c, crgb);
                    J.uncolored = 1;
                    J.gs[0].fcs = J.gs[0].scs = (pcspace*)pa_alloc(&t.cell->arena, sizeof(pcspace));
                    J.gs[0].fcs->kind = CS_RGB;
                    J.gs[0].fcs->n = 3;
                    memcpy(J.gs[0].fc, crgb, sizeof(crgb));
                    memcpy(J.gs[0].sc, crgb, sizeof(crgb));
                }

                run_stream(&J, data, len, pres.t == PV_DICT ? pres : I->res);
                free(data);
                free(J.gs);
                qb_free(&J.path);
                qb_free(&J.textclip);
            }

            q = (dtile*)realloc(L->tiles, sizeof(dtile) * (size_t)(L->ntiles + 1));

            if (!q) {
                dl_free(t.cell);
                p->rgb = 0x808080;
                return;
            }

            L->tiles = q;
            L->tiles[L->ntiles] = t;
            p->kind = DP_TILE;
            p->index = L->ntiles++;
            return;
        }

        p->rgb = 0x808080;
        return;
    }

    if (cs && cs->kind == CS_SEP && cs->none) {
        p->alpha = 0;
    }

    pcs_to_rgb(cs, c, rgb);
    p->rgb = ((uint32_t)(rgb[0] * 255 + 0.5) << 16) | ((uint32_t)(rgb[1] * 255 + 0.5) << 8) |
             (uint32_t)(rgb[2] * 255 + 0.5);
}

/* ------------------------------------------------------------------ */
/* paths                                                              */
/* ------------------------------------------------------------------ */

static void path_pt(interp* I, int op, const double* xy, int n) {
    double v[7];
    int k;

    v[0] = op;

    for (k = 0; k < n; k++) {
        mat_pt(gs(I)->ctm, xy[2 * k], xy[2 * k + 1], &v[1 + 2 * k], &v[2 + 2 * k]);
    }

    qb_put(&I->path, v, sizeof(double) * (size_t)(1 + 2 * n));
}

static void path_close(interp* I) {
    double v = 3;

    qb_put(&I->path, &v, sizeof(v));
    I->cx = I->sx;
    I->cy = I->sy;
}

static double ctm_scale(const double* m) {
    return sqrt(fabs(m[0] * m[3] - m[1] * m[2]));
}

static void path_paint(interp* I, int fill, int stroke, int eo) {
    int32_t n = (int32_t)(I->path.n / sizeof(double)), at;
    gstate* g = gs(I);

    if (n > 0 && (fill || stroke)) {
        at = add_path(I->L, (const double*)I->path.p, n);

        if (at >= 0 && fill) {
            ditem* it = add_item(I, DI_FILL);

            if (it) {
                it->path = at;
                it->npath = n;
                it->evenodd = (uint8_t)eo;
                make_paint(I, 0, &it->paint);
            }
        }

        if (at >= 0 && stroke) {
            ditem* it = add_item(I, DI_STROKE);

            if (it) {
                double sc = ctm_scale(g->ctm);
                int k;

                it->path = at;
                it->npath = n;
                it->lw = (float)(g->lw * sc);
                it->cap = (uint8_t)g->cap;
                it->join = (uint8_t)g->join;
                it->miter = (float)g->miter;
                make_paint(I, 1, &it->paint);

                if (g->ndash > 0 && grow((void**)&I->L->dashes, &I->L->capdashes, I->L->ndashes + g->ndash,
                                         sizeof(float))) {
                    float total = 0;

                    for (k = 0; k < g->ndash; k++) {
                        total += g->dash[k];
                    }

                    if (total > 0) {
                        it->dash = I->L->ndashes;
                        it->ndash = g->ndash;

                        for (k = 0; k < g->ndash; k++) {
                            I->L->dashes[I->L->ndashes++] = (float)(g->dash[k] * sc);
                        }

                        it->dash_phase = (float)(g->dphase * sc);
                    }
                }
            }
        }
    }

    if (I->pending_clip && n >= 0) {
        dlist* L = I->L;

        if (grow((void**)&L->clips, &L->capclips, L->nclips + 1, sizeof(dclip))) {
            dclip* c = &L->clips[L->nclips];

            c->parent = g->clip;
            c->path = n > 0 ? add_path(L, (const double*)I->path.p, n) : 0;
            c->npath = n > 0 ? n : 0;
            c->evenodd = (uint8_t)I->pending_eo;
            g->clip = L->nclips++;
        }
    }

    I->pending_clip = 0;
    I->path.n = 0;
    I->path_open = 0;
}

static void rect_path(interp* I, double x, double y, double w, double h) {
    double p[2];

    p[0] = x;
    p[1] = y;
    path_pt(I, 0, p, 1);
    p[0] = x + w;
    path_pt(I, 1, p, 1);
    p[1] = y + h;
    path_pt(I, 1, p, 1);
    p[0] = x;
    path_pt(I, 1, p, 1);
    I->sx = x;
    I->sy = y;
    path_close(I);
}

/* a clip of a rectangle in user space, made now */
static void clip_rect(interp* I, const double* r) {
    I->path.n = 0;
    rect_path(I, r[0], r[1], r[2] - r[0], r[3] - r[1]);
    I->pending_clip = 1;
    I->pending_eo = 0;
    path_paint(I, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* operands                                                           */
/* ------------------------------------------------------------------ */

#define MAX_OPS 64

typedef struct {
    pv v[MAX_OPS];
    int n;
} ops_t;

static double num(const ops_t* o, int i) {
    pv v;

    if (i < 0 || i >= o->n) {
        return 0;
    }

    v = o->v[i];
    return v.t == PV_INT ? (double)v.u.i : v.t == PV_REAL ? v.u.r : 0;
}

/* the last k operands (operators take theirs from the end, extra ones before them ignored) */
static double arg(const ops_t* o, int k, int i) {
    return num(o, o->n - k + i);
}

/* ------------------------------------------------------------------ */
/* colour                                                             */
/* ------------------------------------------------------------------ */

static pcspace* named_cs(interp* I, pv name) {
    return pcs_parse(I->d, &I->L->arena, name, I->res, 0);
}

static void set_default_color(pcspace* cs, double* c) {
    int k;

    for (k = 0; k < 32; k++) {
        c[k] = 0;
    }

    if (cs && cs->kind == CS_CMYK) {
        c[3] = 1;
    } else if (cs && cs->kind == CS_SEP) {
        for (k = 0; k < cs->n; k++) {
            c[k] = 1;
        }
    } else if (cs && cs->kind == CS_LAB) {
        c[0] = 0;
    }
}

static void set_color(interp* I, const ops_t* o, int stroke) {
    gstate* g = gs(I);
    pcspace* cs = stroke ? g->scs : g->fcs;
    double* c = stroke ? g->sc : g->fc;
    int k, n = 0;

    if (I->uncolored) {
        return;
    }

    for (k = 0; k < o->n && n < 32; k++) {
        if (o->v[k].t == PV_INT || o->v[k].t == PV_REAL) {
            c[n++] = num(o, k);
        } else if (o->v[k].t == PV_NAME && cs && cs->kind == CS_PATTERN) {
            pv pat = pd_get(I->d, pd_get(I->d, I->res, "Pattern"), o->v[k].u.s);

            if (stroke) {
                g->spat = pat;
            } else {
                g->fpat = pat;
            }
        }
    }
}

static void set_device(interp* I, int stroke, int kind, int n, const ops_t* o) {
    gstate* g = gs(I);
    pcspace* cs;
    double* c = stroke ? g->sc : g->fc;
    int k;

    if (I->uncolored) {
        return;
    }

    cs = (pcspace*)pa_alloc(&I->L->arena, sizeof(pcspace));

    if (!cs) {
        return;
    }

    cs->kind = kind;
    cs->n = n;

    if (stroke) {
        g->scs = cs;
    } else {
        g->fcs = cs;
    }

    for (k = 0; k < n; k++) {
        c[k] = arg(o, n, k);
    }
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    interp* I;
    double m[6];
    int first;
} gsink;

static void gk_move(void* u, double x, double y) {
    gsink* s = (gsink*)u;
    double v[3] = { 0, 0, 0 };

    mat_pt(s->m, x, y, &v[1], &v[2]);
    qb_put(&s->I->path, v, sizeof(v));
}

static void gk_line(void* u, double x, double y) {
    gsink* s = (gsink*)u;
    double v[3] = { 1, 0, 0 };

    mat_pt(s->m, x, y, &v[1], &v[2]);
    qb_put(&s->I->path, v, sizeof(v));
}

static void gk_cubic(void* u, double a, double b, double c, double dd, double x, double y) {
    gsink* s = (gsink*)u;
    double v[7] = { 2, 0, 0, 0, 0, 0, 0 };

    mat_pt(s->m, a, b, &v[1], &v[2]);
    mat_pt(s->m, c, dd, &v[3], &v[4]);
    mat_pt(s->m, x, y, &v[5], &v[6]);
    qb_put(&s->I->path, v, sizeof(v));
}

static void gk_close(void* u) {
    double v = 3;

    qb_put(&((gsink*)u)->I->path, &v, sizeof(v));
}

static void record_char(interp* I, pfont* f, uint32_t code, const double* trm, double w0, size_t boff, int blen) {
    uint32_t u[4];
    int n = pf_unicode(f, code, u, 4), k;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30, asc = 0.9, desc = -0.25;
    double corners[4][2] = { {0, desc}, {w0, desc}, {w0, asc}, {0, asc} };
    dlist* L = I->L;
    gstate* g = gs(I);

    if (n == 0) {
        u[0] = 0xFFFD;
        n = 1;
    }

    if (w0 <= 0) {
        corners[1][0] = corners[2][0] = 0.5;
    }

    for (k = 0; k < 4; k++) {
        double x, y;

        mat_pt(trm, corners[k][0], corners[k][1], &x, &y);
        x0 = x < x0 ? x : x0;
        y0 = y < y0 ? y : y0;
        x1 = x > x1 ? x : x1;
        y1 = y > y1 ? y : y1;
    }

    for (k = 0; k < n; k++) {
        tchar* t;

        if (!grow((void**)&L->chars, &L->capchars, L->nchars + 1, sizeof(tchar))) {
            return;
        }

        t = &L->chars[L->nchars++];
        memset(t, 0, sizeof(*t));
        t->pub.cp = u[k];
        /* a ligature's characters share its box, a part each */
        t->pub.x0 = (float)(x0 + (x1 - x0) * k / n);
        t->pub.x1 = (float)(x0 + (x1 - x0) * (k + 1) / n);
        t->pub.y0 = (float)y0;
        t->pub.y1 = (float)y1;
        t->pub.size = (float)(g->fs * sqrt(fabs(trm[0] * trm[3] - trm[1] * trm[2])) / (g->fs ? g->fs : 1));
        t->pub.run = I->run;
        t->stream = I->stream;
        t->op_start = (uint32_t)I->op_start;
        t->op_end = (uint32_t)I->op_end;

        if (I->stream >= 0 && I->bounds) {     /* which stream, and where in it */
            int b = 0;

            while (b + 1 < I->nbounds && I->bounds[b + 1] <= I->op_start) {
                b++;
            }

            t->stream = b;
            t->op_start = (uint32_t)(I->op_start - I->bounds[b]);
            t->op_end = (uint32_t)(I->op_end - I->bounds[b]);

            if (I->op_end > I->bounds[b + 1]) {     /* an operation across two streams: not edited in place */
                t->stream = -1;
            }
        }

        if (g->fcs && g->fcs->kind != CS_PATTERN) {
            double rgb[3];

            pcs_to_rgb(g->fcs, g->fc, rgb);
            t->rgb = ((uint32_t)(rgb[0] * 255 + 0.5) << 16) | ((uint32_t)(rgb[1] * 255 + 0.5) << 8) |
                     (uint32_t)(rgb[2] * 255 + 0.5);
        }

        mat_mul(trm, I->base_inv, t->um);
        t->byte_off = (uint32_t)boff;
        t->byte_len = (uint8_t)blen;
        t->font = f;
        t->fs = g->fs;
        memcpy(t->tm, trm, sizeof(t->tm));
    }
}

static void show_glyph(interp* I, pfont* f, uint32_t code, const double* trm) {
    gstate* g = gs(I);
    int mode = g->tmode;

    if (pf_is_type3(f)) {
        /* the glyph's procedure, run with the font's matrix to text space */
        double fm[6], m[6];
        pv res, proc = pf_type3_proc(f, code, fm, &res);
        size_t len;
        unsigned char* data;

        if (proc.t != PV_STREAM || I->type3_depth > 4 || mode == 3 || mode == 7) {
            return;
        }

        data = pd_stream_data(I->d, proc, &len, NULL, NULL);

        if (!data) {
            return;
        }

        mat_mul(fm, trm, m);

        {
            pv saved_res = I->res;
            int saved_unc = I->uncolored, saved_stream = I->stream, saved_text = I->text, floor;
            size_t saved_path = I->path.n;
            double tm[6], tlm[6];
            const unsigned char* saved_src = I->src;

            memcpy(tm, I->tm, sizeof(tm));
            memcpy(tlm, I->tlm, sizeof(tlm));
            gs_save(I);
            floor = I->ngs;
            memcpy(gs(I)->ctm, m, sizeof(m));
            I->type3_depth++;
            I->stream = -1;
            I->text = 0;
            run_stream(I, data, len, res.t == PV_DICT ? res : I->res);
            I->type3_depth--;

            while (I->ngs > floor) {
                gs_restore(I, 1);
            }

            gs_restore(I, 1);
            I->res = saved_res;
            I->uncolored = saved_unc;
            I->stream = saved_stream;
            I->text = saved_text;
            I->path.n = saved_path;
            I->src = saved_src;
            memcpy(I->tm, tm, sizeof(tm));
            memcpy(I->tlm, tlm, sizeof(tlm));
        }

        free(data);
        return;
    }

    if (mode == 0 || mode == 4) {   /* filled: an item the rasterizer fills from the outline */
        ditem* it = add_item(I, DI_GLYPH);

        if (it) {
            double s[6] = { 0.001, 0, 0, 0.001, 0, 0 };

            it->font = f;
            it->code = code;
            mat_mul(s, trm, it->m);
            make_paint(I, 0, &it->paint);
        }
    }

    if (mode == 1 || mode == 2 || mode == 5 || mode == 6 || mode >= 4) {
        /* the outline as a path: stroked, filled and stroked, or clipped to */
        gsink s;
        pf_sink sk;
        size_t start = I->path.n;
        double sc[6] = { 0.001, 0, 0, 0.001, 0, 0 };

        s.I = I;
        mat_mul(sc, trm, s.m);
        sk.move = gk_move;
        sk.line = gk_line;
        sk.cubic = gk_cubic;
        sk.close = gk_close;
        pf_outline(f, code, &sk, &s);

        if (mode == 1 || mode == 2 || mode == 5 || mode == 6) {
            int32_t n = (int32_t)((I->path.n - start) / sizeof(double));

            if (n > 0) {
                int32_t at = add_path(I->L, (const double*)(I->path.p + start), n);

                if (at >= 0 && mode == 2) {
                    ditem* it = add_item(I, DI_FILL);

                    if (it) {
                        it->path = at;
                        it->npath = n;
                        make_paint(I, 0, &it->paint);
                    }
                }

                if (at >= 0) {
                    ditem* it = add_item(I, DI_STROKE);

                    if (it) {
                        it->path = at;
                        it->npath = n;
                        it->lw = (float)(g->lw * ctm_scale(g->ctm));
                        it->cap = (uint8_t)g->cap;
                        it->join = (uint8_t)g->join;
                        it->miter = (float)g->miter;
                        make_paint(I, 1, &it->paint);
                    }
                }
            }
        }

        if (mode >= 4) {    /* kept for the clip at ET */
            qb_put(&I->textclip, I->path.p + start, I->path.n - start);
            I->textclip_used = 1;
        }

        I->path.n = start;
    }
}

static void show_string(interp* I, pv s) {
    gstate* g = gs(I);
    pfont* f = g->font;
    const unsigned char* p = (const unsigned char*)s.u.s;
    size_t n = (size_t)s.n, i = 0;

    if (!f || s.t != PV_STRING) {
        return;
    }

    while (i < n) {
        uint32_t code;
        int k = pf_next(f, p + i, n - i, &code);
        double w0, trm[6], tsm[6], tx, ty;

        if (k <= 0) {
            break;
        }

        w0 = pf_width(f, code);
        tsm[0] = g->fs * g->th;
        tsm[1] = 0;
        tsm[2] = 0;
        tsm[3] = g->fs;
        tsm[4] = 0;
        tsm[5] = g->trise;

        if (pf_vertical(f)) {   /* the glyph hangs from its origin, centred on it */
            tsm[4] = -w0 * g->fs / 2;
            tsm[5] = -0.88 * g->fs + g->trise;
        }

        mat_mul(tsm, I->tm, trm);
        mat_mul(trm, g->ctm, trm);
        show_glyph(I, f, code, trm);

        if (I->text) {
            double box[6];

            /* the box in text space units of an em (the character's advance across) */
            mat_mul(tsm, I->tm, box);
            mat_mul(box, g->ctm, box);
            record_char(I, f, code, box, pf_vertical(f) ? 1 : w0, i, k);
        }

        if (pf_vertical(f)) {
            tx = 0;
            ty = -(g->fs + g->tc + (k == 1 && code == 32 ? g->tw : 0));
        } else {
            tx = (w0 * g->fs + g->tc + (k == 1 && code == 32 ? g->tw : 0)) * g->th;
            ty = 0;
        }

        {
            double t[6] = { 1, 0, 0, 1, tx, ty };

            mat_mul(t, I->tm, I->tm);
        }

        i += (size_t)k;
    }
}

static void end_text(interp* I) {
    if (I->textclip_used) {
        /* the glyphs of clipping modes are the clip, together */
        size_t save = I->path.n;

        I->path.n = 0;
        qb_put(&I->path, I->textclip.p, I->textclip.n);
        I->pending_clip = 1;
        I->pending_eo = 0;
        path_paint(I, 0, 0, 0);
        I->path.n = save > 0 ? 0 : 0;
        I->textclip.n = 0;
        I->textclip_used = 0;
    }
}

/* ------------------------------------------------------------------ */
/* XObjects, shadings, inline images                                  */
/* ------------------------------------------------------------------ */

static int32_t add_image_ref(dlist* L, pv stream, pv res, pimage* own) {
    dimgref* r;

    if (!grow((void**)&L->images, &L->capimages, L->nimages + 1, sizeof(dimgref))) {
        pimage_free(own);
        return -1;
    }

    r = &L->images[L->nimages];
    memset(r, 0, sizeof(*r));
    r->stream = stream;
    r->res = res;
    r->own = own;
    return L->nimages++;
}

static void draw_image(interp* I, int32_t ref, int stencil) {
    ditem* it = add_item(I, DI_IMAGE);
    gstate* g = gs(I);

    if (!it) {
        return;
    }

    /* the unit square, top row first: the image's (0,0) is its top left */
    {
        double flip[6] = { 1, 0, 0, -1, 0, 1 };

        mat_mul(flip, g->ctm, it->m);
    }

    it->image = ref;

    if (stencil) {
        make_paint(I, 0, &it->paint);
        it->evenodd = 1;    /* a stencil: painted in its paint */
    } else {
        it->paint.alpha = (float)g->fa;
        it->paint.blend = (uint8_t)g->blend;
    }
}

static int is_stencil(pdoc* d, pv dict) {
    pv v = pd_get(d, dict, "ImageMask");

    if (v.t == PV_NULL) {
        v = pd_get(d, dict, "IM");
    }

    return v.t == PV_BOOL && v.u.i;
}

static int oc_hidden(interp* I, pv oc);

static void do_form(interp* I, pv form) {
    size_t len;
    unsigned char* data;
    double m[6] = { 1, 0, 0, 1, 0, 0 }, bbox[4];
    pv res;
    int floor;

    if (I->depth >= MAX_FORM_DEPTH) {
        return;
    }

    data = pd_stream_data(I->d, form, &len, NULL, NULL);

    if (!data) {
        return;
    }

    pd_matrix(I->d, pd_get(I->d, form, "Matrix"), m);
    res = pd_get(I->d, form, "Resources");
    gs_save(I);
    floor = I->ngs;
    mat_mul(m, gs(I)->ctm, gs(I)->ctm);

    if (pd_rect(I->d, pd_get(I->d, form, "BBox"), bbox)) {
        clip_rect(I, bbox);
    }

    {
        pv saved = I->res;
        int saved_stream = I->stream;
        const unsigned char* saved_src = I->src;
        size_t saved_op = I->op_start;
        double tm[6], tlm[6];

        memcpy(tm, I->tm, sizeof(tm));
        memcpy(tlm, I->tlm, sizeof(tlm));
        I->depth++;
        I->stream = -1;
        run_stream(I, data, len, res.t == PV_DICT ? res : I->res);
        I->depth--;
        I->res = saved;
        I->stream = saved_stream;
        I->src = saved_src;
        I->op_start = saved_op;
        memcpy(I->tm, tm, sizeof(tm));
        memcpy(I->tlm, tlm, sizeof(tlm));
    }

    while (I->ngs > floor) {
        gs_restore(I, 1);
    }

    gs_restore(I, 1);
    free(data);
}

static void do_xobject(interp* I, const char* name) {
    pv xo = pd_get_raw(pd_get(I->d, I->res, "XObject"), name), x;
    const char* sub;

    x = pd_resolve(I->d, xo);

    if (x.t != PV_STREAM) {
        return;
    }

    if (oc_hidden(I, pd_get(I->d, x, "OC"))) {
        return;
    }

    sub = pd_getname(I->d, x, "Subtype");

    if (sub && strcmp(sub, "Image") == 0) {
        int32_t ref = add_image_ref(I->L, x, I->res, NULL);

        if (ref >= 0 && !I->hidden) {
            draw_image(I, ref, is_stencil(I->d, x));
        }
    } else if (sub && strcmp(sub, "Form") == 0) {
        do_form(I, x);
    }
}

static void do_shading(interp* I, const char* name) {
    pv sh = pd_get(I->d, pd_get(I->d, I->res, "Shading"), name);
    pshade* s;
    dlist* L = I->L;
    pshade** q;
    ditem* it;

    if (I->hidden) {
        return;
    }

    s = psh_parse(I->d, &L->arena, sh, I->res);

    if (!s) {
        return;
    }

    q = (pshade**)realloc(L->shades, sizeof(pshade*) * (size_t)(L->nshades + 1));

    if (!q) {
        return;
    }

    L->shades = q;
    L->shades[L->nshades] = s;
    it = add_item(I, DI_SHADE);

    if (it) {
        it->paint.kind = DP_SHADE;
        it->paint.index = L->nshades;
        it->paint.alpha = (float)gs(I)->fa;
        it->paint.blend = (uint8_t)gs(I)->blend;
        memcpy(it->paint.m, gs(I)->ctm, sizeof(it->paint.m));
    }

    L->nshades++;
}

/* BI ... ID data EI: the dictionary read, the data up to EI */
static void inline_image(interp* I, plex* L) {
    pv dict = pv_dict(I->d, 16);
    size_t start, end;
    int32_t w, h, bpc, nc = 1;
    pv f, cs;

    for (;;) {
        pv k = pl_read(L), v;

        if (k.t == PV_ERR) {
            return;
        }

        if (k.t == PV_OP && strcmp(k.u.s, "ID") == 0) {
            break;
        }

        if (k.t != PV_NAME) {
            continue;
        }

        v = pl_read(L);

        if (v.t == PV_ERR) {
            return;
        }

        pd_dict_set(I->d, &dict, k.u.s, v);
    }

    start = L->i;

    if (start < L->n && (L->p[start] == ' ' || L->p[start] == '\n' || L->p[start] == '\r' || L->p[start] == '\t')) {
        start++;
    }

    f = pd_get_raw(dict, "F");

    if (f.t == PV_NULL) {
        f = pd_get_raw(dict, "Filter");
    }

    w = (int32_t)pd_getint(I->d, dict, "W", pd_getint(I->d, dict, "Width", 0));
    h = (int32_t)pd_getint(I->d, dict, "H", pd_getint(I->d, dict, "Height", 0));
    bpc = (int32_t)pd_getint(I->d, dict, "BPC", pd_getint(I->d, dict, "BitsPerComponent", 1));
    cs = pd_get_raw(dict, "CS");

    if (cs.t == PV_NULL) {
        cs = pd_get_raw(dict, "ColorSpace");
    }

    if (!is_stencil(I->d, dict)) {
        pcspace* c = cs.t != PV_NULL ? named_cs(I, cs) : NULL;

        nc = c ? c->n : 1;
    }

    end = (size_t)-1;

    if (f.t == PV_NULL && w > 0 && h > 0 && bpc > 0) {
        size_t need = ((size_t)w * (size_t)nc * (size_t)bpc + 7) / 8 * (size_t)h;

        if (start + need <= L->n) {
            end = start + need;
        }
    }

    if (end == (size_t)-1) {
        size_t i;

        for (i = start; i + 2 <= L->n; i++) {
            if (L->p[i] == 'E' && L->p[i + 1] == 'I' && i > start &&
                (L->p[i - 1] == ' ' || L->p[i - 1] == '\n' || L->p[i - 1] == '\r' || L->p[i - 1] == '\t' ||
                 L->p[i - 1] == '>') &&
                (i + 2 == L->n || L->p[i + 2] == ' ' || L->p[i + 2] == '\n' || L->p[i + 2] == '\r' ||
                 L->p[i + 2] == '\t')) {
                end = i - 1;
                break;
            }
        }

        if (end == (size_t)-1) {
            end = L->n;
        }
    }

    {
        pimage* im = pdr_image_decode(I->d, dict, L->p + start, end - start, I->res);

        if (im) {
            int32_t ref = add_image_ref(I->L, pv_null(), I->res, im);

            if (ref >= 0 && !I->hidden) {
                draw_image(I, ref, im->stencil);
            }
        }
    }

    /* past EI */
    L->i = end;

    {
        size_t i;

        for (i = end; i + 2 <= L->n; i++) {
            if (L->p[i] == 'E' && L->p[i + 1] == 'I') {
                L->i = i + 2;
                break;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* optional content                                                   */
/* ------------------------------------------------------------------ */

static int ocg_off(pdoc* d, pv ocg) {
    pv props = pd_get(d, pd_get(d, d->trailer, "Root"), "OCProperties"), dflt = pd_get(d, props, "D");
    pv off = pd_get(d, dflt, "OFF");
    int32_t i;

    if (ocg.t != PV_REF) {
        return 0;
    }

    for (i = 0; off.t == PV_ARRAY && i < off.n; i++) {
        if (off.u.a[i].t == PV_REF && off.u.a[i].u.i == ocg.u.i) {
            return 1;
        }
    }

    return 0;
}

static int oc_hidden(interp* I, pv ocref) {
    pdoc* d = I->d;
    pv oc = pd_resolve(d, ocref);
    const char* type;

    if (oc.t != PV_DICT) {
        return 0;
    }

    type = pd_getname(d, oc, "Type");

    if (type && strcmp(type, "OCMD") == 0) {
        pv ocgs = pd_get_raw(oc, "OCGs");
        const char* p = pd_getname(d, oc, "P");
        int32_t i, on = 0, off = 0, n = 0;

        if (ocgs.t == PV_REF) {
            pv r = pd_resolve(d, ocgs);

            if (r.t == PV_DICT) {
                return ocg_off(d, ocgs);
            }

            ocgs = r;
        }

        for (i = 0; ocgs.t == PV_ARRAY && i < ocgs.n; i++) {
            if (ocg_off(d, ocgs.u.a[i])) {
                off++;
            } else {
                on++;
            }

            n++;
        }

        if (!n) {
            return 0;
        }

        if (!p || strcmp(p, "AnyOn") == 0) {
            return on == 0;
        }

        if (strcmp(p, "AllOn") == 0) {
            return off > 0;
        }

        if (strcmp(p, "AnyOff") == 0) {
            return off == 0;
        }

        if (strcmp(p, "AllOff") == 0) {
            return on > 0;
        }

        return 0;
    }

    return ocg_off(d, ocref);
}

/* ------------------------------------------------------------------ */
/* operators                                                          */
/* ------------------------------------------------------------------ */

/* a soft mask: its group run into a list of its own, in the space the state was set in */
static void set_smask(interp* I, pv sm) {
    pdoc* d = I->d;
    pv form = pd_get(d, sm, "G");
    const char* type = pd_getname(d, sm, "S");
    dlist* L = I->L, *G;
    interp J;
    size_t len;
    unsigned char* data;
    dsmask* q;
    double m[6] = { 1, 0, 0, 1, 0, 0 }, bbox[4], ctm[6];
    pv res;

    if (form.t != PV_STREAM || I->depth > 8) {
        gs(I)->smask = -1;
        return;
    }

    data = pd_stream_data(d, form, &len, NULL, NULL);
    G = (dlist*)calloc(1, sizeof(dlist));

    if (!data || !G) {
        free(data);
        free(G);
        return;
    }

    pd_matrix(d, pd_get(d, form, "Matrix"), m);
    mat_mul(m, gs(I)->ctm, ctm);
    memset(&J, 0, sizeof(J));
    J.d = d;
    J.L = G;
    J.depth = I->depth + 1;
    J.stream = -1;
    grow((void**)&J.gs, &J.capgs, 4, sizeof(gstate));
    J.ngs = 1;
    gs_init(&J.gs[0], ctm, &G->arena);

    if (pd_rect(d, pd_get(d, form, "BBox"), bbox)) {
        clip_rect(&J, bbox);
    }

    res = pd_get(d, form, "Resources");
    run_stream(&J, data, len, res.t == PV_DICT ? res : I->res);
    free(data);
    free(J.gs);
    qb_free(&J.path);
    qb_free(&J.textclip);
    q = (dsmask*)realloc(L->smasks, sizeof(dsmask) * (size_t)(L->nsmasks + 1));

    if (!q) {
        dl_free(G);
        return;
    }

    L->smasks = q;
    q = &L->smasks[L->nsmasks];
    memset(q, 0, sizeof(*q));
    q->group = G;
    q->luminosity = !type || strcmp(type, "Alpha") != 0;

    {
        double bc[32] = { 0 }, rgb[3];
        pv bcv = pd_get(d, sm, "BC");
        int32_t k;
        pcspace* cs = pcs_parse(d, &L->arena, pd_get(d, pd_get(d, form, "Group"), "CS"), I->res, 0);

        for (k = 0; bcv.t == PV_ARRAY && k < bcv.n && k < 32; k++) {
            pd_num(d, bcv.u.a[k], &bc[k]);
        }

        if (bcv.t == PV_ARRAY) {
            pcs_to_rgb(cs, bc, rgb);
            q->backdrop = ((uint32_t)(rgb[0] * 255 + 0.5) << 16) | ((uint32_t)(rgb[1] * 255 + 0.5) << 8) |
                          (uint32_t)(rgb[2] * 255 + 0.5);
        }
    }

    gs(I)->smask = L->nsmasks++;
}

static void ext_gstate(interp* I, const char* name) {
    pdoc* d = I->d;
    pv e = pd_get(d, pd_get(d, I->res, "ExtGState"), name);
    gstate* g = gs(I);
    pv v;

    if (e.t != PV_DICT) {
        return;
    }

    if ((v = pd_get(d, e, "LW")).t != PV_NULL) {
        pd_num(d, v, &g->lw);
    }

    if ((v = pd_get(d, e, "LC")).t == PV_INT) {
        g->cap = (int)v.u.i;
    }

    if ((v = pd_get(d, e, "LJ")).t == PV_INT) {
        g->join = (int)v.u.i;
    }

    if ((v = pd_get(d, e, "ML")).t != PV_NULL) {
        pd_num(d, v, &g->miter);
    }

    if ((v = pd_get(d, e, "CA")).t != PV_NULL) {
        pd_num(d, v, &g->sa);
    }

    if ((v = pd_get(d, e, "ca")).t != PV_NULL) {
        pd_num(d, v, &g->fa);
    }

    if ((v = pd_get(d, e, "D")).t == PV_ARRAY && v.n >= 2) {
        pv a = pd_index(d, v, 0);
        int32_t k;
        double ph = 0;

        g->ndash = 0;

        for (k = 0; a.t == PV_ARRAY && k < a.n && k < 16; k++) {
            double x = 0;

            pd_num(d, a.u.a[k], &x);
            g->dash[g->ndash++] = (float)x;
        }

        pd_num(d, pd_index(d, v, 1), &ph);
        g->dphase = (float)ph;
    }

    if ((v = pd_get(d, e, "Font")).t == PV_ARRAY && v.n >= 2) {
        g->font = pf_load(d, v.u.a[0]);
        pd_num(d, pd_index(d, v, 1), &g->fs);
    }

    v = pd_get(d, e, "SMask");

    if (v.t == PV_NAME) {
        g->smask = -1;
    } else if (v.t == PV_DICT) {
        set_smask(I, v);
    }

    v = pd_get(d, e, "BM");

    if (v.t == PV_ARRAY && v.n > 0) {
        v = pd_index(d, v, 0);
    }

    if (v.t == PV_NAME) {
        const char* s = v.u.s;

        g->blend = strcmp(s, "Multiply") == 0 ? BL_MULTIPLY : strcmp(s, "Screen") == 0 ? BL_SCREEN :
                   strcmp(s, "Darken") == 0 ? BL_DARKEN : strcmp(s, "Lighten") == 0 ? BL_LIGHTEN :
                   strcmp(s, "Overlay") == 0 ? BL_OVERLAY : strcmp(s, "Difference") == 0 ? BL_DIFFERENCE : BL_NORMAL;
    }
}

static void op(interp* I, const char* o, ops_t* a, plex* L) {
    gstate* g = gs(I);
    pdoc* d = I->d;
    double p[6];

#define IS(s) (strcmp(o, s) == 0)

    switch (o[0]) {
        case 'q':
            if (IS("q")) {
                gs_save(I);
            }

            return;

        case 'Q':
            if (IS("Q")) {
                gs_restore(I, 1);
            }

            return;

        case 'c':
            if (IS("cm")) {
                double m[6];
                int k;

                for (k = 0; k < 6; k++) {
                    m[k] = arg(a, 6, k);
                }

                mat_mul(m, g->ctm, g->ctm);
            } else if (IS("c")) {
                int k;

                for (k = 0; k < 6; k++) {
                    p[k] = arg(a, 6, k);
                }

                path_pt(I, 2, p, 3);
                I->cx = p[4];
                I->cy = p[5];
            } else if (IS("cs")) {
                if (a->n >= 1 && !I->uncolored) {
                    g->fcs = named_cs(I, a->v[a->n - 1]);
                    set_default_color(g->fcs, g->fc);
                }
            } else if (IS("cs")) {
            }

            return;

        case 'C':
            if (IS("CS")) {
                if (a->n >= 1 && !I->uncolored) {
                    g->scs = named_cs(I, a->v[a->n - 1]);
                    set_default_color(g->scs, g->sc);
                }
            }

            return;

        case 'm':
            if (IS("m")) {
                p[0] = arg(a, 2, 0);
                p[1] = arg(a, 2, 1);
                path_pt(I, 0, p, 1);
                I->cx = I->sx = p[0];
                I->cy = I->sy = p[1];
                I->path_open = 1;
            }

            return;

        case 'l':
            if (IS("l")) {
                p[0] = arg(a, 2, 0);
                p[1] = arg(a, 2, 1);

                if (!I->path_open) {    /* a line with no move first: from where the pen is */
                    double q[2] = { I->cx, I->cy };

                    path_pt(I, 0, q, 1);
                    I->path_open = 1;
                }

                path_pt(I, 1, p, 1);
                I->cx = p[0];
                I->cy = p[1];
            }

            return;

        case 'v':
            if (IS("v")) {
                p[0] = I->cx;
                p[1] = I->cy;
                p[2] = arg(a, 4, 0);
                p[3] = arg(a, 4, 1);
                p[4] = arg(a, 4, 2);
                p[5] = arg(a, 4, 3);
                path_pt(I, 2, p, 3);
                I->cx = p[4];
                I->cy = p[5];
            }

            return;

        case 'y':
            if (IS("y")) {
                p[0] = arg(a, 4, 0);
                p[1] = arg(a, 4, 1);
                p[2] = p[4] = arg(a, 4, 2);
                p[3] = p[5] = arg(a, 4, 3);
                path_pt(I, 2, p, 3);
                I->cx = p[4];
                I->cy = p[5];
            }

            return;

        case 'h':
            if (IS("h")) {
                path_close(I);
            }

            return;

        case 'r':
            if (IS("re")) {
                rect_path(I, arg(a, 4, 0), arg(a, 4, 1), arg(a, 4, 2), arg(a, 4, 3));
                I->path_open = 1;
            } else if (IS("rg") && !I->uncolored) {
                set_device(I, 0, CS_RGB, 3, a);
            } else if (IS("ri")) {
            }

            return;

        case 'R':
            if (IS("RG") && !I->uncolored) {
                set_device(I, 1, CS_RGB, 3, a);
            }

            return;

        case 'g':
            if (IS("g") && !I->uncolored) {
                set_device(I, 0, CS_GRAY, 1, a);
            } else if (IS("gs") && a->n >= 1 && a->v[a->n - 1].t == PV_NAME) {
                ext_gstate(I, a->v[a->n - 1].u.s);
            }

            return;

        case 'G':
            if (IS("G") && !I->uncolored) {
                set_device(I, 1, CS_GRAY, 1, a);
            }

            return;

        case 'k':
            if (IS("k") && !I->uncolored) {
                set_device(I, 0, CS_CMYK, 4, a);
            }

            return;

        case 'K':
            if (IS("K") && !I->uncolored) {
                set_device(I, 1, CS_CMYK, 4, a);
            }

            return;

        case 's':
            if (IS("sc") || IS("scn")) {
                set_color(I, a, 0);
            } else if (IS("s")) {
                path_close(I);
                path_paint(I, 0, 1, 0);
            } else if (IS("sh") && a->n >= 1 && a->v[a->n - 1].t == PV_NAME) {
                do_shading(I, a->v[a->n - 1].u.s);
            }

            return;

        case 'S':
            if (IS("SC") || IS("SCN")) {
                set_color(I, a, 1);
            } else if (IS("S")) {
                path_paint(I, 0, 1, 0);
            }

            return;

        case 'f':
            if (IS("f") || IS("F")) {
                path_paint(I, 1, 0, 0);
            } else if (IS("f*")) {
                path_paint(I, 1, 0, 1);
            }

            return;

        case 'F':
            if (IS("F")) {
                path_paint(I, 1, 0, 0);
            }

            return;

        case 'B':
            if (IS("B")) {
                path_paint(I, 1, 1, 0);
            } else if (IS("B*")) {
                path_paint(I, 1, 1, 1);
            } else if (IS("BT")) {
                I->tm[0] = I->tm[3] = I->tlm[0] = I->tlm[3] = 1;
                I->tm[1] = I->tm[2] = I->tm[4] = I->tm[5] = 0;
                I->tlm[1] = I->tlm[2] = I->tlm[4] = I->tlm[5] = 0;
                I->textclip.n = 0;
                I->textclip_used = 0;
            } else if (IS("BI")) {
                inline_image(I, L);
            } else if (IS("BDC") || IS("BMC")) {
                int hide = 0;

                if (IS("BDC") && a->n >= 2 && pv_is_name(a->v[a->n - 2], "OC")) {
                    pv props = a->v[a->n - 1];

                    if (props.t == PV_NAME) {
                        props = pd_get_raw(pd_get(d, I->res, "Properties"), props.u.s);
                    }

                    hide = oc_hidden(I, props);
                }

                if (I->nmarked < 64) {
                    I->marked[I->nmarked] = hide;
                }

                I->nmarked++;
                I->hidden += hide;
            } else if (IS("BX")) {
            }

            return;

        case 'b':
            if (IS("b")) {
                path_close(I);
                path_paint(I, 1, 1, 0);
            } else if (IS("b*")) {
                path_close(I);
                path_paint(I, 1, 1, 1);
            }

            return;

        case 'n':
            if (IS("n")) {
                path_paint(I, 0, 0, 0);
            }

            return;

        case 'W':
            if (IS("W")) {
                I->pending_clip = 1;
                I->pending_eo = 0;
            } else if (IS("W*")) {
                I->pending_clip = 1;
                I->pending_eo = 1;
            }

            return;

        case 'w':
            if (IS("w")) {
                g->lw = arg(a, 1, 0);
            }

            return;

        case 'J':
            if (IS("J")) {
                g->cap = (int)arg(a, 1, 0);
            }

            return;

        case 'j':
            if (IS("j")) {
                g->join = (int)arg(a, 1, 0);
            }

            return;

        case 'M':
            if (IS("M")) {
                g->miter = arg(a, 1, 0);
            } else if (IS("MP")) {
            }

            return;

        case 'd':
            if (IS("d") && a->n >= 2 && a->v[a->n - 2].t == PV_ARRAY) {
                pv arr = a->v[a->n - 2];
                int32_t k;

                g->ndash = 0;

                for (k = 0; k < arr.n && k < 16; k++) {
                    double x = 0;

                    pd_num(d, arr.u.a[k], &x);
                    g->dash[g->ndash++] = (float)x;
                }

                g->dphase = (float)arg(a, 1, 0);
            } else if (IS("d0")) {
            } else if (IS("d1")) {
                I->uncolored = 1;
            }

            return;

        case 'i':
            return;

        case 'E':
            if (IS("ET")) {
                end_text(I);
            } else if (IS("EMC")) {
                if (I->nmarked > 0) {
                    I->nmarked--;

                    if (I->nmarked < 64) {
                        I->hidden -= I->marked[I->nmarked];
                    }
                }
            } else if (IS("EX")) {
            }

            return;

        case 'D':
            if (IS("Do") && a->n >= 1 && a->v[a->n - 1].t == PV_NAME) {
                do_xobject(I, a->v[a->n - 1].u.s);
            } else if (IS("DP")) {
            }

            return;

        case 'T':
            switch (o[1]) {
                case 'c': g->tc = arg(a, 1, 0); return;
                case 'w': g->tw = arg(a, 1, 0); return;
                case 'z': g->th = arg(a, 1, 0) / 100; return;
                case 'L': g->tl = arg(a, 1, 0); return;
                case 's': g->trise = arg(a, 1, 0); return;
                case 'r': g->tmode = (int)arg(a, 1, 0); return;

                case 'f':
                    if (a->n >= 2 && a->v[a->n - 2].t == PV_NAME) {
                        g->font = pf_load(d, pd_get_raw(pd_get(d, I->res, "Font"), a->v[a->n - 2].u.s));
                    }

                    g->fs = arg(a, 1, 0);
                    return;

                case 'd': case 'D': {
                    double t[6] = { 1, 0, 0, 1, arg(a, 2, 0), arg(a, 2, 1) };

                    if (o[1] == 'D') {
                        g->tl = -t[5];
                    }

                    mat_mul(t, I->tlm, I->tlm);
                    memcpy(I->tm, I->tlm, sizeof(I->tm));
                    return;
                }

                case 'm': {
                    int k;

                    for (k = 0; k < 6; k++) {
                        I->tlm[k] = arg(a, 6, k);
                    }

                    memcpy(I->tm, I->tlm, sizeof(I->tm));
                    return;
                }

                case '*': {
                    double t[6] = { 1, 0, 0, 1, 0, -g->tl };

                    mat_mul(t, I->tlm, I->tlm);
                    memcpy(I->tm, I->tlm, sizeof(I->tm));
                    return;
                }

                case 'j':
                    if (a->n >= 1) {
                        I->nstr = 1;
                        show_string(I, a->v[a->n - 1]);
                        I->run++;
                    }

                    return;

                case 'J':
                    if (a->n >= 1 && a->v[a->n - 1].t == PV_ARRAY) {
                        pv arr = a->v[a->n - 1];
                        int32_t k;

                        for (k = 0; k < arr.n; k++) {
                            pv e = arr.u.a[k];

                            if (e.t == PV_STRING) {
                                show_string(I, e);
                            } else if (e.t == PV_INT || e.t == PV_REAL) {
                                double v = e.t == PV_INT ? (double)e.u.i : e.u.r;
                                double t[6] = { 1, 0, 0, 1, 0, 0 };

                                if (g->font && pf_vertical(g->font)) {
                                    t[5] = -v / 1000 * g->fs;
                                } else {
                                    t[4] = -v / 1000 * g->fs * g->th;
                                }

                                mat_mul(t, I->tm, I->tm);
                            }
                        }

                        I->run++;
                    }

                    return;

                default:
                    return;
            }

        case '\'':
            if (o[1] == 0) {
                double t[6] = { 1, 0, 0, 1, 0, -g->tl };

                mat_mul(t, I->tlm, I->tlm);
                memcpy(I->tm, I->tlm, sizeof(I->tm));

                if (a->n >= 1) {
                    show_string(I, a->v[a->n - 1]);
                }

                I->run++;
            }

            return;

        case '"':
            if (o[1] == 0 && a->n >= 3) {
                double t[6] = { 1, 0, 0, 1, 0, -g->tl };

                g->tw = num(a, a->n - 3);
                g->tc = num(a, a->n - 2);
                mat_mul(t, I->tlm, I->tlm);
                memcpy(I->tm, I->tlm, sizeof(I->tm));
                show_string(I, a->v[a->n - 1]);
                I->run++;
            }

            return;

        default:
            return;
    }
}

static void run_stream(interp* I, const unsigned char* data, size_t len, pv res) {
    plex L;
    ops_t a;
    pv saved = I->res;
    const unsigned char* saved_src = I->src;
    int base_ngs = I->ngs;

    memset(&L, 0, sizeof(L));
    L.p = data;
    L.n = len;
    L.d = I->d;
    L.content = 1;
    a.n = 0;
    I->res = res;
    I->src = data;

    for (;;) {
        pv v;
        size_t at;

        pl_skip_ws(&L);
        at = L.i;
        v = pl_read(&L);

        if (v.t == PV_ERR) {
            break;
        }

        if (a.n == 0) {
            I->op_start = at;
        }

        if (v.t == PV_OP) {
            I->op_end = L.i;

            if (v.u.s[0] != '{' && v.u.s[0] != '}' && v.u.s[0] != ']' && v.u.s[0] != '>' && v.u.s[0] != ')') {
                op(I, v.u.s, &a, &L);
            }

            a.n = 0;
            continue;
        }

        if (a.n < MAX_OPS) {
            a.v[a.n++] = v;
        }
    }

    /* an unbalanced q in this stream does not carry out of it into what drew it */
    while (I->ngs > base_ngs && base_ngs > 0 && I->depth > 0) {
        gs_restore(I, base_ngs);
        break;
    }

    I->res = saved;
    I->src = saved_src;
}

/* ------------------------------------------------------------------ */
/* annotations                                                        */
/* ------------------------------------------------------------------ */

void pd_annot_appearance(pdoc* d, pv annot, pv* form);   /* pd_pdfr_edit.c: one made if it has none */

static void draw_annot(interp* I, pv annot, const double* base) {
    pdoc* d = I->d;
    int64_t flags = pd_getint(d, annot, "F", 0);
    const char* sub = pd_getname(d, annot, "Subtype");
    pv ap, form;
    double rect[4], bbox[4], fm[6] = { 1, 0, 0, 1, 0, 0 };

    if ((flags & 2) || (flags & 32) || !sub || strcmp(sub, "Popup") == 0 || strcmp(sub, "Link") == 0) {
        return;     /* hidden, no view, a popup, or a link (shown by its hover) */
    }

    if (!pd_rect(d, pd_get(d, annot, "Rect"), rect)) {
        return;
    }

    if (oc_hidden(I, pd_get(d, annot, "OC"))) {
        return;
    }

    ap = pd_get(d, pd_get(d, annot, "AP"), "N");
    form = ap;

    if (ap.t == PV_DICT) {      /* states: the one AS names */
        const char* as = pd_getname(d, annot, "AS");

        form = as ? pd_get(d, ap, as) : pv_null();

        if (form.t != PV_STREAM && !as) {
            form = pd_get(d, ap, "Off");
        }
    }

    if (form.t != PV_STREAM) {
        pd_annot_appearance(d, annot, &form);
    }

    if (form.t != PV_STREAM) {
        return;
    }

    if (!pd_rect(d, pd_get(d, form, "BBox"), bbox)) {
        memcpy(bbox, rect, sizeof(bbox));
    }

    pd_matrix(d, pd_get(d, form, "Matrix"), fm);

    {
        /* the form's box through its matrix, mapped onto the annotation's rectangle (12.5.5) */
        double xs[4], ys[4], bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30, A[6], M[6];
        int k;

        mat_pt(fm, bbox[0], bbox[1], &xs[0], &ys[0]);
        mat_pt(fm, bbox[2], bbox[1], &xs[1], &ys[1]);
        mat_pt(fm, bbox[2], bbox[3], &xs[2], &ys[2]);
        mat_pt(fm, bbox[0], bbox[3], &xs[3], &ys[3]);

        for (k = 0; k < 4; k++) {
            bx0 = xs[k] < bx0 ? xs[k] : bx0;
            by0 = ys[k] < by0 ? ys[k] : by0;
            bx1 = xs[k] > bx1 ? xs[k] : bx1;
            by1 = ys[k] > by1 ? ys[k] : by1;
        }

        A[0] = bx1 > bx0 ? (rect[2] - rect[0]) / (bx1 - bx0) : 1;
        A[3] = by1 > by0 ? (rect[3] - rect[1]) / (by1 - by0) : 1;
        A[1] = A[2] = 0;
        A[4] = rect[0] - bx0 * A[0];
        A[5] = rect[1] - by0 * A[3];

        if (flags & 16) {   /* NoRotate is not followed: drawn as placed */
        }

        mat_mul(A, base, M);

        {
            int floor;

            gs_save(I);
            floor = I->ngs;
            memcpy(gs(I)->ctm, M, sizeof(M));
            gs(I)->clip = -1;
            gs(I)->fa = gs(I)->sa = 1;

            {
                double ca;
                pv cav = pd_get(d, annot, "CA");

                if (pd_num(d, cav, &ca) && strcmp(sub, "Widget") != 0) {
                    gs(I)->fa = gs(I)->sa = ca;
                }
            }

            /* the form drawn with its own matrix inside A */
            do_form(I, form);

            while (I->ngs > floor) {
                gs_restore(I, 1);
            }

            gs_restore(I, 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* pages                                                              */
/* ------------------------------------------------------------------ */

void pd_page_base(pdoc* d, int32_t page, double* m) {
    const ppage* p = &d->pages[page];
    double w = p->crop[2] - p->crop[0], h = p->crop[3] - p->crop[1];
    double uu = pd_getnum(d, p->dict, "UserUnit", 1);
    double t[6];

    if (!(uu > 0)) {
        uu = 1;
    }

    /* user space to the crop box's corner, y down */
    t[0] = uu;
    t[1] = 0;
    t[2] = 0;
    t[3] = -uu;
    t[4] = -p->crop[0] * uu;
    t[5] = p->crop[3] * uu;
    w *= uu;
    h *= uu;

    /* then turned clockwise by Rotate */
    switch (p->rotate) {
        case 90: {
            double r[6] = { 0, 1, -1, 0, h, 0 };

            mat_mul(t, r, m);
            return;
        }

        case 180: {
            double r[6] = { -1, 0, 0, -1, w, h };

            mat_mul(t, r, m);
            return;
        }

        case 270: {
            double r[6] = { 0, -1, 1, 0, 0, w };

            mat_mul(t, r, m);
            return;
        }

        default:
            memcpy(m, t, sizeof(t));
    }
}

static void page_contents(interp* I, pv page) {
    pdoc* d = I->d;
    pv c = pd_get(d, page, "Contents");
    int32_t i, n = c.t == PV_ARRAY ? c.n : 1;
    pv res = pd_get(d, page, "Resources");

    for (i = 0; i < n; i++) {
        pv st = c.t == PV_ARRAY ? pd_index(d, c, i) : c;
        size_t len;
        unsigned char* data;

        if (st.t != PV_STREAM) {
            continue;
        }

        data = pd_stream_data(d, st, &len, NULL, NULL);

        if (!data) {
            continue;
        }

        I->stream = i;
        run_stream(I, data, len, res);
        free(data);
    }
}

void pd_page_cache_drop(pdoc* d, int32_t page) {
    int32_t i;

    if (!d->cache || page < 0 || page >= d->npages) {
        return;
    }

    dl_free(d->cache[page]);
    d->cache[page] = NULL;

    for (i = 0; i < d->nlru; i++) {
        if (d->page_lru[i] == page) {
            memmove(&d->page_lru[i], &d->page_lru[i + 1], sizeof(int32_t) * (size_t)(d->nlru - i - 1));
            d->nlru--;
            break;
        }
    }
}

void pd_pages_cache_free(pdoc* d) {
    int32_t i;

    for (i = 0; d->cache && i < d->npages; i++) {
        dl_free(d->cache[i]);
        d->cache[i] = NULL;
    }

    free(d->page_lru);
    d->page_lru = NULL;
    d->nlru = 0;
    pd_image_cache_free(d);
}

dlist* pd_page_list(pdoc* d, int32_t page) {
    dlist* L;
    interp I;
    double base[6];
    ppage* p;
    pv res;

    if (!d->cache || page < 0 || page >= d->npages) {
        return NULL;
    }

    if (d->cache[page] && d->cache[page]->changes != d->changes) {
        pd_page_cache_drop(d, page);
    }

    if (d->cache[page]) {
        int32_t i;

        for (i = 0; i < d->nlru; i++) {     /* the most recent to the end */
            if (d->page_lru[i] == page) {
                memmove(&d->page_lru[i], &d->page_lru[i + 1], sizeof(int32_t) * (size_t)(d->nlru - i - 1));
                d->page_lru[d->nlru - 1] = page;
                break;
            }
        }

        return d->cache[page];
    }

    while (d->nlru >= LIST_KEEP) {
        pd_page_cache_drop(d, d->page_lru[0]);
    }

    L = (dlist*)calloc(1, sizeof(dlist));

    if (!L) {
        return NULL;
    }

    p = &d->pages[page];
    pd_pdfr_page_size(d, page, &L->width, &L->height);
    pd_page_base(d, page, base);
    memset(&I, 0, sizeof(I));
    I.d = d;
    I.L = L;
    I.text = 1;
    mat_inv(base, I.base_inv);
    grow((void**)&I.gs, &I.capgs, 8, sizeof(gstate));
    I.ngs = 1;
    gs_init(&I.gs[0], base, &L->arena);
    res = p->resources;

    {
        /* the page's own resources, else the inherited ones */
        pv own = pd_get(d, p->dict, "Resources");

        if (own.t == PV_DICT) {
            res = own;
        }
    }

    {
        double crop[4];
        int k;

        /* clipped to the crop box */
        for (k = 0; k < 4; k++) {
            crop[k] = p->crop[k];
        }

        clip_rect(&I, crop);
    }

    {
        /* the content streams run as one: a token or an operation may run on from one into the next */
        pv c = pd_get(d, p->dict, "Contents");
        int32_t i, n = c.t == PV_ARRAY ? c.n : 1;
        pbuf all;

        memset(&all, 0, sizeof(all));
        I.bounds = (size_t*)calloc((size_t)n + 1, sizeof(size_t));

        for (i = 0; i < n && I.bounds; i++) {
            pv st = c.t == PV_ARRAY ? pd_index(d, c, i) : c;
            size_t len;
            unsigned char* data = st.t == PV_STREAM ? pd_stream_data(d, st, &len, NULL, NULL) : NULL;

            I.bounds[i] = all.n;

            if (data) {
                qb_put(&all, data, len);
                free(data);
            }

            qb_put(&all, "\n", 1);
        }

        if (I.bounds) {
            I.bounds[n] = all.n;
            I.nbounds = n;
            I.stream = 0;

            if (all.p) {
                run_stream(&I, all.p, all.n, res);
            }
        }

        qb_free(&all);
        free(I.bounds);
        I.bounds = NULL;
        I.nbounds = 0;
    }

    (void)page_contents;

    /* annotations, each from a fresh state */
    L->annots_from = L->n;
    I.text = 0;
    I.ngs = 1;
    I.hidden = 0;
    gs_init(&I.gs[0], base, &L->arena);

    {
        pv annots = pd_get(d, p->dict, "Annots");
        int32_t i;

        for (i = 0; annots.t == PV_ARRAY && i < annots.n; i++) {
            pv an = pd_index(d, annots, i);

            if (an.t == PV_DICT) {
                I.ngs = 1;
                draw_annot(&I, an, base);
            }
        }
    }

    free(I.gs);
    qb_free(&I.path);
    qb_free(&I.textclip);
    L->changes = d->changes;
    d->cache[page] = L;

    if (!d->page_lru) {
        d->page_lru = (int32_t*)malloc(sizeof(int32_t) * (LIST_KEEP + 1));
    }

    if (d->page_lru) {
        d->page_lru[d->nlru++] = page;
    }

    return L;
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

/* the page's text as pd_pdfr_page_text gives it; map[i] the recorded character of each (-1 for one put in) */
int32_t pd_text_build(dlist* L, pd_pdfr_char* buf, int32_t cap, int32_t* map);

int32_t pd_text_build(dlist* L, pd_pdfr_char* buf, int32_t cap, int32_t* map) {
    int32_t i, n = 0;
    const tchar* prev = NULL;

    /* the characters, with a space or a line break put in where the file leaves only a gap */
    for (i = 0; i < L->nchars; i++) {
        const tchar* c = &L->chars[i];
        int flags = 0;

        if (prev && prev->pub.cp != ' ' && c->pub.cp != ' ') {
            double h = c->pub.y1 - c->pub.y0, ph = prev->pub.y1 - prev->pub.y0;
            double mid = (c->pub.y0 + c->pub.y1) / 2, pmid = (prev->pub.y0 + prev->pub.y1) / 2;
            double size = h > ph ? h : ph;

            if (fabs(mid - pmid) > size * 0.6 || c->pub.x1 < prev->pub.x0 - size * 0.5) {
                flags = PD_PDFR_CHAR_NEWLINE;
            } else if (c->pub.x0 - prev->pub.x1 > size * 0.18) {
                flags = PD_PDFR_CHAR_SPACE;
            }
        } else if (prev && prev->pub.cp == ' ') {
            double mid = (c->pub.y0 + c->pub.y1) / 2, pmid = (prev->pub.y0 + prev->pub.y1) / 2;
            double size = c->pub.y1 - c->pub.y0;

            if (fabs(mid - pmid) > size * 0.6 && c->pub.cp != ' ') {
                flags = PD_PDFR_CHAR_NEWLINE;
            }
        }

        if (flags) {
            if (buf && n < cap) {
                pd_pdfr_char* o = &buf[n];

                memset(o, 0, sizeof(*o));
                o->cp = flags == PD_PDFR_CHAR_NEWLINE ? '\n' : ' ';
                o->flags = flags;
                o->x0 = o->x1 = prev->pub.x1;
                o->y0 = prev->pub.y0;
                o->y1 = prev->pub.y1;
                o->size = prev->pub.size;
                o->run = prev->pub.run;
            }

            if (map && n < cap) {
                map[n] = -1;
            }

            n++;
        }

        if (buf && n < cap) {
            buf[n] = c->pub;
        }

        if (map && n < cap) {
            map[n] = i;
        }

        n++;
        prev = c;
    }

    return n;
}

pd_status pd_pdfr_page_text(pd_pdfr* d, int32_t page, pd_pdfr_char* buf, int32_t cap, int32_t* count) {
    dlist* L;
    int32_t n;

    if (!d || !count) {
        return PD_ERR_ARG;
    }

    L = pd_page_list(d, page);

    if (!L) {
        return PD_ERR_RANGE;
    }

    n = pd_text_build(L, buf, buf ? cap : 0, NULL);
    *count = n;
    return buf && n > cap ? PD_ERR_RANGE : PD_OK;
}
