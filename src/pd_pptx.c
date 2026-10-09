/*
 * PowerPoint (.pptx) read as pages: each slide a page as big as the slide, the page a canvas (a drawing canvas
 * in front of the text, from the page's corner) holding the slide's shapes -- those of its master and layout
 * behind them. The slides are made into a .docx of that, in memory, and read as any .docx is: a slide's
 * shapes are DrawingML as a Word canvas's are, its text boxes WordprocessingML made from its text bodies, with
 * what placeholders take from the layout and the master (where they are, their text's size, colour, bullets).
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_conv.h"

/* ---- a small DOM over the markup tokenizer ---- */

typedef struct {
    char name[48];              /* local name */
    size_t a, b;                /* the element: its '<' to after its end */
    size_t ia, ib;              /* its inside */
    const char* attrs;
    size_t alen;
    int parent, kid, next;
} xnode;

typedef struct {
    char* s;                    /* owned */
    size_t n;
    xnode* v;
    int nv, cap;
} xdoc;

static void xd_free(xdoc* d) {
    free(d->s);
    free(d->v);
    memset(d, 0, sizeof(*d));
}

/* parsed from owned text (taken); 0 when it cannot be */
static int xd_parse(xdoc* d, char* s, size_t n) {
    pd_markup m;
    int stack[256], depth = 0, last[256];
    size_t at;

    memset(d, 0, sizeof(*d));
    d->s = s;
    d->n = n;

    if (!s) {
        return 0;
    }

    mu_init(&m, s, n, 0);

    for (;;) {
        at = m.pos;

        if (mu_next(&m) == MT_END) {
            break;
        }

        if (m.type == MT_OPEN || m.type == MT_EMPTY) {
            xnode* x;
            int k;

            if (d->nv >= d->cap) {
                int nc = d->cap ? d->cap * 2 : 256;
                xnode* t = (xnode*)realloc(d->v, sizeof(xnode) * (size_t)nc);

                if (!t) {
                    return 0;
                }

                d->v = t;
                d->cap = nc;
            }

            k = d->nv++;
            x = &d->v[k];
            memset(x, 0, sizeof(*x));
            snprintf(x->name, sizeof(x->name), "%s", mu_local(m.name));
            x->a = at;
            x->ia = x->ib = x->b = m.pos;
            x->attrs = m.attrs;
            x->alen = m.alen;
            x->parent = depth ? stack[depth - 1] : -1;
            x->kid = x->next = -1;

            if (depth) {    /* the last child of its parent: this one after it */
                int p = stack[depth - 1];

                if (last[depth - 1] < 0) {
                    d->v[p].kid = k;
                } else {
                    d->v[last[depth - 1]].next = k;
                }

                last[depth - 1] = k;
            }

            if (m.type == MT_OPEN && depth < 255) {
                stack[depth] = k;
                last[depth] = -1;
                depth++;
            }
        } else if (m.type == MT_CLOSE && depth) {
            xnode* x = &d->v[stack[--depth]];

            x->ib = at;
            x->b = m.pos;
        }
    }

    return d->nv > 0;
}

static int xd_kid(const xdoc* d, int n, const char* name) {
    int k;

    if (n < 0) {
        return -1;
    }

    for (k = d->v[n].kid; k >= 0; k = d->v[k].next) {
        if (!strcmp(d->v[k].name, name)) {
            return k;
        }
    }

    return -1;
}

/* a descendant by a path of local names ("spPr/xfrm") */
static int xd_path(const xdoc* d, int n, const char* path) {
    char part[48];
    const char* p = path;

    while (n >= 0 && *p) {
        size_t k = 0;

        while (*p && *p != '/' && k < sizeof(part) - 1) {
            part[k++] = *p++;
        }

        part[k] = '\0';

        if (*p == '/') {
            p++;
        }

        n = xd_kid(d, n, part);
    }

    return n;
}

/* the first element of a name anywhere in the document */
static int xd_find(const xdoc* d, const char* name) {
    int k;

    for (k = 0; k < d->nv; k++) {
        if (!strcmp(d->v[k].name, name)) {
            return k;
        }
    }

    return -1;
}

static int xd_attr(const xdoc* d, int n, const char* name, char* buf, size_t cap) {
    pd_markup m;

    buf[0] = '\0';

    if (n < 0) {
        return 0;
    }

    memset(&m, 0, sizeof(m));
    m.attrs = d->v[n].attrs;
    m.alen = d->v[n].alen;
    return mu_attr(&m, name, buf, cap);
}

static long long xd_int(const xdoc* d, int n, const char* name, long long def) {
    char v[40];

    return xd_attr(d, n, name, v, sizeof(v)) && v[0] ? atoll(v) : def;
}

/* an element's whole text, or its inside */
static void xd_raw(pd_buf* o, const xdoc* d, int n) {
    pb_put(o, d->s + d->v[n].a, d->v[n].b - d->v[n].a);
}

static void xd_inner(pd_buf* o, const xdoc* d, int n) {
    pb_put(o, d->s + d->v[n].ia, d->v[n].ib - d->v[n].ia);
}

/* ---- the package ---- */

typedef struct {
    char id[64], target[256];   /* the target as a path in the package */
} prel;

typedef struct {
    xdoc x;
    char path[256];
    prel* rels;
    int nrels;
} ppart;

typedef struct {
    const unsigned char* zip;
    size_t zn;
    pd_buf doc, rels;           /* the .docx made: its document.xml and its relationships */
    void* zw;
    int nmedia;
    char media[256][128];       /* the pictures put in it so far: their paths in the package, */
    char media_id[256][16];     /* and their ids in the .docx */
    uint32_t theme[12];         /* dk1 lt1 dk2 lt2 accent1-6 hlink folHlink */
    char major[64], minor[64];  /* the theme's fonts: headings, body */
    int docpr;
    long long sw, sh;           /* the slide size (EMU) */
} pptx;

/* a path relative to a part's folder ("../media/a.png" from "ppt/slides/slide1.xml") */
static void resolve(const char* base, const char* target, char* out, size_t cap) {
    char buf[512];
    char* parts[64];
    int n = 0, i;
    char* p;
    size_t k;

    if (target[0] == '/') {
        snprintf(buf, sizeof(buf), "%s", target + 1);
    } else {
        const char* slash = strrchr(base, '/');

        snprintf(buf, sizeof(buf), "%.*s%s", slash ? (int)(slash - base + 1) : 0, base, target);
    }

    for (p = strtok(buf, "/"); p && n < 64; p = strtok(NULL, "/")) {
        if (!strcmp(p, "..")) {
            n = n > 0 ? n - 1 : 0;
        } else if (strcmp(p, ".")) {
            parts[n++] = p;
        }
    }

    out[0] = '\0';
    k = 0;

    for (i = 0; i < n && k < cap; i++) {
        k += (size_t)snprintf(out + k, cap - k, "%s%s", i ? "/" : "", parts[i]);
    }
}

static int part_load(pptx* P, ppart* pt, const char* path) {
    size_t len = 0;
    char* s;
    char rpath[300];
    const char* slash;
    xdoc r;
    int k;

    memset(pt, 0, sizeof(*pt));
    snprintf(pt->path, sizeof(pt->path), "%s", path);
    s = (char*)pd_zip_get(P->zip, P->zn, path, &len);

    if (!s || !xd_parse(&pt->x, s, len)) {
        if (!s) {
            free(s);
        }

        return 0;
    }

    slash = strrchr(path, '/');
    snprintf(rpath, sizeof(rpath), "%.*s_rels/%s.rels", slash ? (int)(slash - path + 1) : 0, path,
             slash ? slash + 1 : path);
    s = (char*)pd_zip_get(P->zip, P->zn, rpath, &len);

    if (s && xd_parse(&r, s, len)) {
        pt->rels = (prel*)calloc((size_t)r.nv, sizeof(prel));

        for (k = 0; pt->rels && k < r.nv; k++) {
            char tgt[256];

            if (strcmp(r.v[k].name, "Relationship")) {
                continue;
            }

            xd_attr(&r, k, "Id", pt->rels[pt->nrels].id, sizeof(pt->rels[0].id));
            xd_attr(&r, k, "Target", tgt, sizeof(tgt));
            resolve(path, tgt, pt->rels[pt->nrels].target, sizeof(pt->rels[0].target));
            pt->nrels++;
        }

        xd_free(&r);
    } else if (s) {
        free(s);
    }

    return 1;
}

static void part_free(ppart* pt) {
    xd_free(&pt->x);
    free(pt->rels);
    memset(pt, 0, sizeof(*pt));
}

static const char* rel_of(const ppart* pt, const char* id) {
    int k;

    for (k = 0; k < pt->nrels; k++) {
        if (!strcmp(pt->rels[k].id, id)) {
            return pt->rels[k].target;
        }
    }

    return NULL;
}

/* the target of the first relationship whose target has a part of its path ("slideLayouts/") */
static const char* rel_like(const ppart* pt, const char* kind) {
    int k;

    for (k = 0; k < pt->nrels; k++) {
        if (strstr(pt->rels[k].target, kind)) {
            return pt->rels[k].target;
        }
    }

    return NULL;
}

/* a picture of a part in the .docx: its id there (the file copied in the first time) */
static const char* media_id(pptx* P, const ppart* pt, const char* rid) {
    const char* path = rel_of(pt, rid), *name;
    unsigned char* data;
    size_t len = 0;
    char dst[200];
    int k;

    if (!path) {
        return NULL;
    }

    for (k = 0; k < P->nmedia; k++) {
        if (!strcmp(P->media[k], path)) {
            return P->media_id[k];
        }
    }

    if (P->nmedia >= 256 || (data = pd_zip_get(P->zip, P->zn, path, &len)) == NULL) {
        return NULL;
    }

    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    snprintf(dst, sizeof(dst), "word/media/p%d_%.100s", P->nmedia, name);
    pd_zipw_add(P->zw, dst, data, len);
    free(data);
    snprintf(P->media[P->nmedia], sizeof(P->media[0]), "%.127s", path);
    snprintf(P->media_id[P->nmedia], sizeof(P->media_id[0]), "rIdPm%d", P->nmedia);
    pb_printf(&P->rels, "<Relationship Id=\"%s\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
              "relationships/image\" Target=\"media/p%d_%s\"/>", P->media_id[P->nmedia], P->nmedia, name);
    return P->media_id[P->nmedia++];
}

/* ---- colours ---- */

static uint32_t hexrgb(const char* v) {
    return (uint32_t)strtoul(v, NULL, 16) & 0xFFFFFFu;
}

static int theme_slot(const char* v) {
    static const char* names[] = { "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3", "accent4", "accent5",
                                   "accent6", "hlink", "folHlink"
                                 };
    int k;

    if (!strcmp(v, "tx1")) {
        return 0;
    }

    if (!strcmp(v, "bg1")) {
        return 1;
    }

    if (!strcmp(v, "tx2")) {
        return 2;
    }

    if (!strcmp(v, "bg2")) {
        return 3;
    }

    for (k = 0; k < 12; k++) {
        if (!strcmp(v, names[k])) {
            return k;
        }
    }

    return -1;
}

static void rgb_hsl(uint32_t c, double* h, double* s, double* l) {
    double r = ((c >> 16) & 255) / 255.0, g = ((c >> 8) & 255) / 255.0, b = (c & 255) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;

    *l = (mx + mn) / 2;
    *s = d == 0 ? 0 : d / (1 - fabs(2 * *l - 1));
    *h = d == 0 ? 0 : mx == r ? fmod((g - b) / d + 6, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
}

static uint32_t hsl_rgb(double h, double s, double l) {
    double c = (1 - fabs(2 * l - 1)) * s, x = c * (1 - fabs(fmod(h, 2) - 1)), m = l - c / 2, r, g, b;

    r = h < 1 ? c : h < 2 ? x : h < 4 ? 0 : h < 5 ? x : c;
    g = h < 1 ? x : h < 3 ? c : h < 4 ? x : 0;
    b = h < 2 ? 0 : h < 3 ? x : h < 5 ? c : x;
    return ((uint32_t)((r + m) * 255 + 0.5) << 16) | ((uint32_t)((g + m) * 255 + 0.5) << 8) |
           (uint32_t)((b + m) * 255 + 0.5);
}

/* a colour element (srgbClr, schemeClr, sysClr, prstClr) with its changes (lumMod, lumOff, tint, shade):
   RGB, or -1 */
static long colour_of(const pptx* P, const xdoc* d, int c) {
    char v[40];
    uint32_t rgb;
    int k;
    double h, s, l;

    if (c < 0) {
        return -1;
    }

    if (!strcmp(d->v[c].name, "srgbClr") && xd_attr(d, c, "val", v, sizeof(v))) {
        rgb = hexrgb(v);
    } else if (!strcmp(d->v[c].name, "schemeClr") && xd_attr(d, c, "val", v, sizeof(v)) && theme_slot(v) >= 0) {
        rgb = P->theme[theme_slot(v)];
    } else if (!strcmp(d->v[c].name, "sysClr") && xd_attr(d, c, "lastClr", v, sizeof(v))) {
        rgb = hexrgb(v);
    } else if (!strcmp(d->v[c].name, "prstClr") && xd_attr(d, c, "val", v, sizeof(v))) {
        rgb = !strcmp(v, "white") ? 0xFFFFFF : !strcmp(v, "red") ? 0xFF0000 : !strcmp(v, "blue") ? 0x0000FF : 0;
    } else {
        return -1;
    }

    rgb_hsl(rgb, &h, &s, &l);

    for (k = d->v[c].kid; k >= 0; k = d->v[k].next) {
        double val = xd_int(d, k, "val", 100000) / 100000.0;

        if (!strcmp(d->v[k].name, "lumMod")) {
            l *= val;
        } else if (!strcmp(d->v[k].name, "lumOff")) {
            l += val;
        } else if (!strcmp(d->v[k].name, "tint")) {
            l = l + (1 - l) * (1 - val);
        } else if (!strcmp(d->v[k].name, "shade")) {
            l *= val;
        }
    }

    l = l < 0 ? 0 : l > 1 ? 1 : l;
    return (long)hsl_rgb(h, s, l);
}

/* the colour of a solidFill under element n */
static long fill_colour(const pptx* P, const xdoc* d, int n) {
    int f = xd_kid(d, n, "solidFill");

    return f >= 0 && d->v[f].kid >= 0 ? colour_of(P, d, d->v[f].kid) : -1;
}

static void theme_load(pptx* P, const char* path) {
    ppart t;
    static const char* names[12] = { "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3", "accent4",
                                     "accent5", "accent6", "hlink", "folHlink"
                                   };
    static const uint32_t defaults[12] = { 0x000000, 0xFFFFFF, 0x44546A, 0xE7E6E6, 0x4472C4, 0xED7D31, 0xA5A5A5,
                                           0xFFC000, 0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72
                                         };
    int k, s, f;

    memcpy(P->theme, defaults, sizeof(defaults));
    snprintf(P->major, sizeof(P->major), "Calibri Light");
    snprintf(P->minor, sizeof(P->minor), "Calibri");

    if (!path || !part_load(P, &t, path)) {
        return;
    }

    s = xd_find(&t.x, "clrScheme");

    for (k = 0; s >= 0 && k < 12; k++) {
        int e = xd_kid(&t.x, s, names[k]);
        long c = e >= 0 ? colour_of(P, &t.x, t.x.v[e].kid) : -1;

        if (c >= 0) {
            P->theme[k] = (uint32_t)c;
        }
    }

    f = xd_path(&t.x, xd_find(&t.x, "majorFont"), "latin");
    xd_attr(&t.x, f, "typeface", P->major, sizeof(P->major));
    f = xd_path(&t.x, xd_find(&t.x, "minorFont"), "latin");
    xd_attr(&t.x, f, "typeface", P->minor, sizeof(P->minor));
    part_free(&t);
}

/* ---- placeholders: what a slide's take from its layout and master ---- */

typedef struct {
    const xdoc* d;
    int n;
} xref;

/* a placeholder's kind: 0 none, 1 a title, 2 body text, 3 others (date, footer, slide number) */
static int ph_kind(const char* type) {
    return !strcmp(type, "title") || !strcmp(type, "ctrTitle") ? 1 : !type[0] || !strcmp(type, "body") ||
           !strcmp(type, "obj") || !strcmp(type, "subTitle") || !strcmp(type, "tbl") || !strcmp(type, "chart") ||
           !strcmp(type, "pic") ? 2 : 3;
}

/* a shape's placeholder: its type and index; 0 when it is none */
static int ph_of(const xdoc* d, int sp, char* type, size_t tcap, long long* idx) {
    int nv = d->v[sp].kid, ph = -1;

    type[0] = '\0';
    *idx = -1;

    for (; nv >= 0 && ph < 0; nv = d->v[nv].next) {
        if (strstr(d->v[nv].name, "nvSpPr") || strstr(d->v[nv].name, "nvPicPr") ||
                strstr(d->v[nv].name, "nvGraphicFramePr")) {
            ph = xd_path(d, nv, "nvPr/ph");
        }
    }

    if (ph < 0) {
        return 0;
    }

    xd_attr(d, ph, "type", type, tcap);
    *idx = xd_int(d, ph, "idx", -1);
    return 1;
}

/* the placeholder of a layout or a master matching one: by index, else by type (a master: by its kind) */
static int ph_match(const xdoc* d, const char* type, long long idx, int master) {
    int k, by_type = -1;

    for (k = 0; k < d->nv; k++) {
        char t[32];
        long long i;

        if (strcmp(d->v[k].name, "sp") || !ph_of(d, k, t, sizeof(t), &i)) {
            continue;
        }

        if (!master && idx >= 0 && i == idx) {
            return k;
        }

        if (by_type < 0 && (!strcmp(t, type) || (ph_kind(t) == ph_kind(type) && ph_kind(type) != 3) ||
                            (!t[0] && !type[0]))) {
            by_type = k;
        }
    }

    return by_type;
}

/* ---- text: a text body as WordprocessingML ---- */

typedef struct {
    pptx* P;
    xref lvl[8];                /* where a level's properties may be, nearest first */
    int nlvl;
    int title;                  /* a title: the theme's heading font */
    long font_colour;           /* the shape's style's text colour (fontRef), -1 none */
    double scale, spacing;      /* autofit: the font's scale, the line spacing's */
    int slide_no;               /* for a slide-number field */
} tctx;

/* the levels' properties for level L: the paragraph's own, the text body's list styles, the placeholders', the
   master's text styles */
static void lvl_chain(tctx* T, const xdoc* const ds[], const int ns[], int nn, int lvl) {
    char name[24];
    int k;

    snprintf(name, sizeof(name), "lvl%dpPr", lvl < 0 ? 1 : lvl > 8 ? 9 : lvl + 1);
    T->nlvl = 0;

    for (k = 0; k < nn && T->nlvl < 8; k++) {
        int e = ns[k] >= 0 ? xd_kid(ds[k], ns[k], name) : -1;

        if (e >= 0) {
            T->lvl[T->nlvl].d = ds[k];
            T->lvl[T->nlvl++].n = e;
        }
    }
}

/* a paragraph property (an attribute of its pPr, or the levels'); 0 when none has it */
static int ppr_attr(const tctx* T, const xdoc* d, int ppr, const char* name, char* buf, size_t cap) {
    int k;

    if (ppr >= 0 && xd_attr(d, ppr, name, buf, cap)) {
        return 1;
    }

    for (k = 0; k < T->nlvl; k++) {
        if (xd_attr(T->lvl[k].d, T->lvl[k].n, name, buf, cap)) {
            return 1;
        }
    }

    return 0;
}

/* a paragraph property element (lnSpc, spcBef, buChar, ...): the nearest that has one */
static xref ppr_kid(const tctx* T, const xdoc* d, int ppr, const char* name) {
    xref r = { NULL, -1 };
    int k;

    if (ppr >= 0 && (r.n = xd_kid(d, ppr, name)) >= 0) {
        r.d = d;
        return r;
    }

    for (k = 0; k < T->nlvl; k++) {
        if ((r.n = xd_kid(T->lvl[k].d, T->lvl[k].n, name)) >= 0) {
            r.d = T->lvl[k].d;
            return r;
        }
    }

    r.n = -1;
    return r;
}

/* a run property: the run's rPr, else the levels' defRPr */
static int rpr_attr(const tctx* T, const xdoc* d, int rpr, const char* name, char* buf, size_t cap) {
    int k;

    if (rpr >= 0 && xd_attr(d, rpr, name, buf, cap)) {
        return 1;
    }

    for (k = 0; k < T->nlvl; k++) {
        if (xd_attr(T->lvl[k].d, xd_kid(T->lvl[k].d, T->lvl[k].n, "defRPr"), name, buf, cap)) {
            return 1;
        }
    }

    return 0;
}

static long rpr_colour(const tctx* T, const xdoc* d, int rpr) {
    long c = rpr >= 0 ? fill_colour(T->P, d, rpr) : -1;
    int k;

    for (k = 0; c < 0 && k < T->nlvl; k++) {
        int dr = xd_kid(T->lvl[k].d, T->lvl[k].n, "defRPr");

        c = dr >= 0 ? fill_colour(T->P, T->lvl[k].d, dr) : -1;
    }

    return c >= 0 ? c : T->font_colour >= 0 ? T->font_colour : (long)T->P->theme[0];
}

static void rpr_font(const tctx* T, const xdoc* d, int rpr, char* face, size_t cap) {
    int k, l = rpr >= 0 ? xd_kid(d, rpr, "latin") : -1;

    face[0] = '\0';

    if (l >= 0) {
        xd_attr(d, l, "typeface", face, cap);
    }

    for (k = 0; !face[0] && k < T->nlvl; k++) {
        xd_attr(T->lvl[k].d, xd_path(T->lvl[k].d, T->lvl[k].n, "defRPr/latin"), "typeface", face, cap);
    }

    if (!face[0] || !strcmp(face, "+mn-lt")) {
        snprintf(face, cap, "%s", T->title ? T->P->major : T->P->minor);
    } else if (!strcmp(face, "+mj-lt")) {
        snprintf(face, cap, "%s", T->P->major);
    }
}

static void put_esc(pd_buf* o, const char* s) {
    for (; *s; s++) {
        if (*s == '&') {
            pb_puts(o, "&amp;");
        } else if (*s == '<') {
            pb_puts(o, "&lt;");
        } else if (*s == '>') {
            pb_puts(o, "&gt;");
        } else if (*s == '"') {
            pb_puts(o, "&quot;");
        } else {
            pb_putc(o, *s);
        }
    }
}

/* a face named with its weight ("Raleway ExtraBold"): the family, and whether the weight is a bold one */
static int font_weight(char* face) {
    static const struct {
        const char* w;
        int bold;
    } ws[] = { { " ExtraBold", 1 }, { " Extra Bold", 1 }, { " SemiBold", 1 }, { " Semibold", 1 }, { " Black", 1 },
        { " Heavy", 1 }, { " Bold", 1 }, { " ExtraLight", 0 }, { " Light", 0 }, { " Thin", 0 }, { " Medium", 0 }
    };
    size_t n = strlen(face), k;

    for (k = 0; k < sizeof(ws) / sizeof(ws[0]); k++) {
        size_t m = strlen(ws[k].w);

        if (n > m && !strcmp(face + n - m, ws[k].w)) {
            face[n - m] = '\0';
            return ws[k].bold;
        }
    }

    return 0;
}

/* a run's properties, as WordprocessingML */
static void put_rpr(pd_buf* o, const tctx* T, const xdoc* d, int rpr) {
    char v[80];
    long sz = 1800, c;
    int heavy;

    pb_puts(o, "<w:rPr>");
    rpr_font(T, d, rpr, v, sizeof(v));
    heavy = font_weight(v);
    pb_puts(o, "<w:rFonts w:ascii=\"");
    put_esc(o, v);
    pb_puts(o, "\" w:hAnsi=\"");
    put_esc(o, v);
    pb_puts(o, "\" w:cs=\"");
    put_esc(o, v);
    pb_puts(o, "\"/>");

    if (heavy || (rpr_attr(T, d, rpr, "b", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true")))) {
        pb_puts(o, "<w:b/>");
    }

    if (rpr_attr(T, d, rpr, "i", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"))) {
        pb_puts(o, "<w:i/>");
    }

    if (rpr_attr(T, d, rpr, "u", v, sizeof(v)) && strcmp(v, "none")) {
        pb_puts(o, "<w:u w:val=\"single\"/>");
    }

    if (rpr_attr(T, d, rpr, "strike", v, sizeof(v)) && strcmp(v, "noStrike")) {
        pb_puts(o, "<w:strike/>");
    }

    if (rpr_attr(T, d, rpr, "baseline", v, sizeof(v)) && atoi(v) != 0) {
        pb_printf(o, "<w:vertAlign w:val=\"%s\"/>", atoi(v) > 0 ? "superscript" : "subscript");
    }

    c = rpr_colour(T, d, rpr);
    pb_printf(o, "<w:color w:val=\"%06lX\"/>", (unsigned long)c & 0xFFFFFFul);

    if (rpr_attr(T, d, rpr, "sz", v, sizeof(v))) {
        sz = atol(v);
    }

    sz = (long)(sz * T->scale + 0.5);
    pb_printf(o, "<w:sz w:val=\"%ld\"/><w:szCs w:val=\"%ld\"/></w:rPr>", (sz + 25) / 50, (sz + 25) / 50);
}

/* spacing (spcPts in hundredths of a point; spcPct): twips, or (line, Pct) the line's 240ths */
static int spacing_of(const xref r, int* pct) {
    int k;

    *pct = 0;

    if (r.n < 0) {
        return -1;
    }

    if ((k = xd_kid(r.d, r.n, "spcPts")) >= 0) {
        return (int)(xd_int(r.d, k, "val", 0) / 5);
    }

    if ((k = xd_kid(r.d, r.n, "spcPct")) >= 0) {
        *pct = 1;
        return (int)(xd_int(r.d, k, "val", 100000) * 240 / 100000);
    }

    return -1;
}

static void put_paragraph(pd_buf* o, tctx* T, const xdoc* d, int p, int* autonum) {
    int ppr = xd_kid(d, p, "pPr"), k, first_rpr = -1, pct, sp, any = 0;
    char v[64], bu[16] = "";
    xref r;
    long long marl, ind;

    pb_puts(o, "<w:p><w:pPr>");
    /* line spacing and the space around */
    pb_puts(o, "<w:spacing");
    sp = spacing_of(ppr_kid(T, d, ppr, "spcBef"), &pct);

    if (sp >= 0 && !pct) {
        pb_printf(o, " w:before=\"%d\"", sp);
    }

    sp = spacing_of(ppr_kid(T, d, ppr, "spcAft"), &pct);
    pb_printf(o, " w:after=\"%d\"", sp >= 0 && !pct ? sp : 0);
    sp = spacing_of(ppr_kid(T, d, ppr, "lnSpc"), &pct);

    if (sp >= 0 && pct) {
        pb_printf(o, " w:line=\"%d\" w:lineRule=\"auto\"", (int)(sp * T->spacing + 0.5));
    } else if (sp >= 0) {
        pb_printf(o, " w:line=\"%d\" w:lineRule=\"exact\"", sp);
    } else {
        pb_printf(o, " w:line=\"%d\" w:lineRule=\"auto\"", (int)(240 * T->spacing + 0.5));
    }

    pb_puts(o, "/>");
    marl = ppr_attr(T, d, ppr, "marL", v, sizeof(v)) ? atoll(v) : 0;
    ind = ppr_attr(T, d, ppr, "indent", v, sizeof(v)) ? atoll(v) : 0;

    if (marl || ind) {
        pb_printf(o, "<w:ind w:left=\"%lld\"%s%lld\"/>", marl / 635, ind < 0 ? " w:hanging=\"" : " w:firstLine=\"",
                  (ind < 0 ? -ind : ind) / 635);
    }

    if (ppr_attr(T, d, ppr, "algn", v, sizeof(v))) {
        const char* jc = !strcmp(v, "ctr") ? "center" : !strcmp(v, "r") ? "right" : !strcmp(v, "just") ||
                         !strcmp(v, "dist") ? "both" : "left";

        pb_printf(o, "<w:jc w:val=\"%s\"/>", jc);
    }

    /* the paragraph mark's size: an empty line as tall as its text would be */
    for (k = d->v[p].kid; k >= 0; k = d->v[k].next) {
        if (!strcmp(d->v[k].name, "r") && first_rpr < 0) {
            first_rpr = xd_kid(d, k, "rPr");
            any = 1;
        }
    }

    if (!any) {
        first_rpr = xd_kid(d, p, "endParaRPr");
    }

    put_rpr(o, T, d, first_rpr);
    pb_puts(o, "</w:pPr>");

    /* its bullet: as text before it */
    r = ppr_kid(T, d, ppr, "buNone");

    if (any && r.n < 0) {
        xref c = ppr_kid(T, d, ppr, "buChar"), a = ppr_kid(T, d, ppr, "buAutoNum");

        if (a.n >= 0) {
            snprintf(bu, sizeof(bu), "%d.", ++*autonum);
        } else if (c.n >= 0) {
            xd_attr(c.d, c.n, "char", v, sizeof(v));
            snprintf(bu, sizeof(bu), "%s", strcmp(v, "") ? v : "\xE2\x80\xA2");

            if ((unsigned char)bu[0] < 0x80 && strchr("\xA7lnqvu\xD8\xFC", bu[0])) {
                snprintf(bu, sizeof(bu), "\xE2\x80\xA2");   /* a Wingdings or Symbol one: a plain bullet */
            }
        }
    }

    if (!bu[0] || !isdigit((unsigned char)bu[0])) {
        *autonum = 0;   /* a numbered list ends where a paragraph is not numbered */
    }

    if (bu[0]) {
        pb_puts(o, "<w:r>");
        put_rpr(o, T, d, first_rpr);
        pb_puts(o, "<w:t xml:space=\"preserve\">");
        put_esc(o, bu);
        pb_puts(o, "</w:t></w:r><w:r>");
        put_rpr(o, T, d, first_rpr);
        pb_puts(o, "<w:tab/></w:r>");
    }

    for (k = d->v[p].kid; k >= 0; k = d->v[k].next) {
        const char* n = d->v[k].name;

        if (!strcmp(n, "r") || !strcmp(n, "fld")) {
            int t = xd_kid(d, k, "t");
            char ft[32];

            pb_puts(o, "<w:r>");
            put_rpr(o, T, d, xd_kid(d, k, "rPr"));
            pb_puts(o, "<w:t xml:space=\"preserve\">");

            if (!strcmp(n, "fld") && xd_attr(d, k, "type", ft, sizeof(ft)) && !strcmp(ft, "slidenum")) {
                pb_printf(o, "%d", T->slide_no);    /* the slide's number, whatever it said when saved */
            } else if (t >= 0) {
                xd_inner(o, d, t);  /* already escaped */
            }

            pb_puts(o, "</w:t></w:r>");
        } else if (!strcmp(n, "br")) {
            pb_puts(o, "<w:r>");
            put_rpr(o, T, d, xd_kid(d, k, "rPr"));
            pb_puts(o, "<w:br/></w:r>");
        }
    }

    pb_puts(o, "</w:p>");
}

/* ---- shapes ---- */

typedef struct {
    pptx* P;
    int slide_no;
    ppart* slide, *layout, *master;
    const xdoc* pres;           /* presentation.xml: its default text style */
    int pres_style;
    int id_base;                /* the part's shape ids made apart from the other parts' */
    pd_buf* o;
} conv;

/* an element under another name: its attributes and inside kept */
static void put_renamed(pd_buf* o, const xdoc* d, int n, const char* name) {
    pb_printf(o, "<%s", name);

    if (d->v[n].alen) {
        pb_putc(o, ' ');
        pb_put(o, d->v[n].attrs, d->v[n].alen);
    }

    if (d->v[n].ib > d->v[n].ia) {
        pb_putc(o, '>');
        xd_inner(o, d, n);
        pb_printf(o, "</%s>", name);
    } else {
        pb_puts(o, "/>");
    }
}

/* an inside with its pictures' ids made the .docx's */
static void put_with_media(pd_buf* o, const conv* C, const ppart* pt, const char* s, size_t n) {
    static const char key[] = "r:embed=\"";
    size_t i = 0;

    while (i < n) {
        const char* k = NULL;
        size_t j;

        for (j = i; j + sizeof(key) - 1 <= n; j++) {
            if (!memcmp(s + j, key, sizeof(key) - 1)) {
                k = s + j;
                break;
            }
        }

        if (!k) {
            pb_put(o, s + i, n - i);
            break;
        }

        pb_put(o, s + i, (size_t)(k - s) - i + sizeof(key) - 1);
        i = (size_t)(k - s) + sizeof(key) - 1;

        {
            char rid[64];
            size_t q = 0;
            const char* m;

            while (i < n && s[i] != '"' && q < sizeof(rid) - 1) {
                rid[q++] = s[i++];
            }

            rid[q] = '\0';
            m = media_id(C->P, pt, rid);
            pb_puts(o, m ? m : "");
        }
    }
}

static void put_tree(conv* C, ppart* pt, int tree, int skip_ph);

/* a text body as a text box's content: its paragraphs, each with what its level takes from the list styles */
static void put_txbody(conv* C, ppart* pt, int sp, int tx, int kind, int lsp, int msp) {
    const xdoc* d = &pt->x;
    const xdoc* ds[6];
    int ns[6], nn = 0, p, autonum = 0, ts;
    tctx T;
    int bp = xd_kid(d, tx, "bodyPr"), na = bp >= 0 ? xd_kid(d, bp, "normAutofit") : -1, fr;

    memset(&T, 0, sizeof(T));
    T.P = C->P;
    T.slide_no = C->slide_no;
    T.title = kind == 1;
    T.scale = na >= 0 ? xd_int(d, na, "fontScale", 100000) / 100000.0 : 1;
    T.spacing = na >= 0 ? 1 - xd_int(d, na, "lnSpcReduction", 0) / 100000.0 : 1;
    fr = xd_path(d, sp, "style/fontRef");
    T.font_colour = fr >= 0 && d->v[fr].kid >= 0 ? colour_of(C->P, d, d->v[fr].kid) : -1;
    /* the list styles, nearest first: its own, its layout placeholder's, its master's, the master's styles for
       its kind, the presentation's */
    ds[nn] = d;
    ns[nn++] = xd_kid(d, tx, "lstStyle");

    if (lsp >= 0) {
        ds[nn] = &C->layout->x;
        ns[nn++] = xd_path(&C->layout->x, lsp, "txBody/lstStyle");
    }

    if (msp >= 0) {
        ds[nn] = &C->master->x;
        ns[nn++] = xd_path(&C->master->x, msp, "txBody/lstStyle");
    }

    ts = C->master ? xd_find(&C->master->x, "txStyles") : -1;

    if (ts >= 0) {
        ds[nn] = &C->master->x;
        ns[nn++] = xd_kid(&C->master->x, ts, kind == 1 ? "titleStyle" : kind == 2 ? "bodyStyle" : "otherStyle");
    }

    if (C->pres && C->pres_style >= 0) {
        ds[nn] = C->pres;
        ns[nn++] = C->pres_style;
    }

    pb_puts(C->o, "<wps:txbx><w:txbxContent>");

    for (p = d->v[tx].kid; p >= 0; p = d->v[p].next) {
        if (!strcmp(d->v[p].name, "p")) {
            int ppr = xd_kid(d, p, "pPr");

            lvl_chain(&T, ds, ns, nn, ppr >= 0 ? (int)xd_int(d, ppr, "lvl", 0) : 0);
            put_paragraph(C->o, &T, d, p, &autonum);
        }
    }

    pb_puts(C->o, "</w:txbxContent></wps:txbx>");
}

/* a text body's bodyPr: its own attributes, else its placeholders' */
static void put_bodypr(conv* C, const xdoc* d, int tx, int lsp, int msp) {
    static const char* names[] = { "lIns", "tIns", "rIns", "bIns", "anchor", "wrap", "vert", "anchorCtr" };
    int k, bp = tx >= 0 ? xd_kid(d, tx, "bodyPr") : -1;
    int lb = lsp >= 0 ? xd_path(&C->layout->x, lsp, "txBody/bodyPr") : -1;
    int mb = msp >= 0 ? xd_path(&C->master->x, msp, "txBody/bodyPr") : -1;

    pb_puts(C->o, "<wps:bodyPr");

    for (k = 0; k < (int)(sizeof(names) / sizeof(names[0])); k++) {
        char v[40];

        if (xd_attr(d, bp, names[k], v, sizeof(v)) || (lb >= 0 && xd_attr(&C->layout->x, lb, names[k], v, sizeof(v))) ||
                (mb >= 0 && xd_attr(&C->master->x, mb, names[k], v, sizeof(v)))) {
            pb_printf(C->o, " %s=\"%s\"", names[k], v);
        }
    }

    pb_puts(C->o, "/>");
}

static int has_text(const xdoc* d, int tx) {
    int p, r;

    for (p = tx >= 0 ? d->v[tx].kid : -1; p >= 0; p = d->v[p].next) {
        for (r = d->v[p].kid; r >= 0; r = d->v[r].next) {
            if (!strcmp(d->v[r].name, "r") || !strcmp(d->v[r].name, "fld")) {
                return 1;
            }
        }
    }

    return 0;
}

/* a shape's spPr: its own, with where it is and its geometry from its placeholders when it has not its own */
static int put_sppr(conv* C, ppart* pt, int sp, int lsp, int msp, const char* el) {
    const xdoc* d = &pt->x;
    int spr = xd_kid(d, sp, "spPr"), xf = xd_kid(d, spr, "xfrm"), k, geom = 0;
    xref pxf = { d, xf };

    if (xf < 0 && lsp >= 0 && (k = xd_path(&C->layout->x, lsp, "spPr/xfrm")) >= 0) {
        pxf.d = &C->layout->x;
        pxf.n = k;
    } else if (xf < 0 && msp >= 0 && (k = xd_path(&C->master->x, msp, "spPr/xfrm")) >= 0) {
        pxf.d = &C->master->x;
        pxf.n = k;
    }

    if (pxf.n < 0) {
        return 0;   /* nowhere */
    }

    pb_printf(C->o, "<%s>", el);
    xd_raw(C->o, pxf.d, pxf.n);

    for (k = spr >= 0 ? d->v[spr].kid : -1; k >= 0; k = d->v[k].next) {
        if (!strcmp(d->v[k].name, "xfrm")) {
            continue;
        }

        geom |= !strcmp(d->v[k].name, "prstGeom") || !strcmp(d->v[k].name, "custGeom");

        if (!geom && (!strcmp(d->v[k].name, "solidFill") || !strcmp(d->v[k].name, "noFill") ||
                      !strcmp(d->v[k].name, "gradFill") || !strcmp(d->v[k].name, "blipFill") ||
                      !strcmp(d->v[k].name, "pattFill") || !strcmp(d->v[k].name, "ln"))) {
            pb_puts(C->o, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>");     /* the geometry goes first */
            geom = 1;
        }

        if (!strcmp(d->v[k].name, "blipFill")) {
            pb_puts(C->o, "<a:blipFill>");
            put_with_media(C->o, C, pt, d->s + d->v[k].ia, d->v[k].ib - d->v[k].ia);
            pb_puts(C->o, "</a:blipFill>");
        } else {
            xd_raw(C->o, d, k);
        }
    }

    if (!geom) {
        pb_puts(C->o, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>");
    }

    pb_printf(C->o, "</%s>", el);
    return 1;
}

static void put_nvpr(conv* C, const xdoc* d, int sp, const char* nv, const char* el) {
    char name[160];
    int c = xd_path(d, sp, nv);

    xd_attr(d, c, "name", name, sizeof(name));
    pb_printf(C->o, "<%s id=\"%lld\" name=\"", el, xd_int(d, c, "id", 0) + C->id_base);
    put_esc(C->o, name);
    pb_puts(C->o, "\"/>");
}

/* a table (a:tbl) as a text box holding a Word table, drawn with a plain style: a header row in the theme's
   first accent, banded rows */
static void put_table(conv* C, ppart* pt, int gf, int tbl) {
    const xdoc* d = &pt->x;
    int grid = xd_kid(d, tbl, "tblGrid"), tp = xd_kid(d, tbl, "tblPr"), k, r, row = 0;
    int first = (int)xd_int(d, tp, "firstRow", 0), band = (int)xd_int(d, tp, "bandRow", 0);
    int xf = xd_kid(d, gf, "xfrm");
    long long total = 0;
    uint32_t acc = C->P->theme[4];

    if (xf < 0) {
        return;
    }

    pb_puts(C->o, "<wps:wsp>");
    put_nvpr(C, d, gf, "nvGraphicFramePr/cNvPr", "wps:cNvPr");
    pb_puts(C->o, "<wps:cNvSpPr txBox=\"1\"/><wps:spPr><a:xfrm>");
    xd_inner(C->o, d, xf);
    pb_puts(C->o, "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></wps:spPr>"
            "<wps:txbx><w:txbxContent><w:tbl><w:tblPr><w:tblLayout w:type=\"fixed\"/><w:tblBorders>"
            "<w:top w:val=\"single\" w:sz=\"4\" w:color=\"FFFFFF\"/><w:left w:val=\"single\" w:sz=\"4\" w:color=\"FFFFFF\"/>"
            "<w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"FFFFFF\"/><w:right w:val=\"single\" w:sz=\"4\" w:color=\"FFFFFF\"/>"
            "<w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"FFFFFF\"/><w:insideV w:val=\"single\" w:sz=\"4\" "
            "w:color=\"FFFFFF\"/></w:tblBorders><w:tblCellMar><w:left w:w=\"144\" w:type=\"dxa\"/><w:right w:w=\"144\" "
            "w:type=\"dxa\"/></w:tblCellMar></w:tblPr><w:tblGrid>");

    for (k = grid >= 0 ? d->v[grid].kid : -1; k >= 0; k = d->v[k].next) {
        pb_printf(C->o, "<w:gridCol w:w=\"%lld\"/>", xd_int(d, k, "w", 0) / 635);
        total += xd_int(d, k, "w", 0);
    }

    pb_puts(C->o, "</w:tblGrid>");

    for (r = d->v[tbl].kid; r >= 0; r = d->v[r].next) {
        int c;

        if (strcmp(d->v[r].name, "tr")) {
            continue;
        }

        pb_printf(C->o, "<w:tr><w:trPr><w:trHeight w:val=\"%lld\"/></w:trPr>", xd_int(d, r, "h", 0) / 635);

        for (c = d->v[r].kid; c >= 0; c = d->v[c].next) {
            int tc = xd_kid(d, c, "tcPr"), tx = xd_kid(d, c, "txBody"), p, autonum = 0;
            long fill = tc >= 0 ? fill_colour(C->P, d, tc) : -1;
            long long span = xd_int(d, c, "gridSpan", 1);
            tctx T;

            if (strcmp(d->v[c].name, "tc") || xd_int(d, c, "hMerge", 0)) {
                continue;
            }

            if (fill < 0 && first && row == 0) {
                fill = (long)acc;
            } else if (fill < 0 && band) {
                double h, s, l;

                rgb_hsl(acc, &h, &s, &l);
                fill = (long)hsl_rgb(h, s, (row - first) % 2 ? 0.92 : 0.84);
            }

            pb_puts(C->o, "<w:tc><w:tcPr>");

            if (span > 1) {
                pb_printf(C->o, "<w:gridSpan w:val=\"%lld\"/>", span);
            }

            if (xd_int(d, c, "vMerge", 0)) {
                pb_puts(C->o, "<w:vMerge/>");
            } else if (xd_int(d, c, "rowSpan", 1) > 1) {
                pb_puts(C->o, "<w:vMerge w:val=\"restart\"/>");
            }

            if (fill >= 0) {
                pb_printf(C->o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06lX\"/>", (unsigned long)fill);
            }

            pb_puts(C->o, "</w:tcPr>");
            memset(&T, 0, sizeof(T));
            T.P = C->P;
            T.scale = T.spacing = 1;
            T.font_colour = first && row == 0 ? 0xFFFFFF : -1;

            for (p = tx >= 0 ? d->v[tx].kid : -1; p >= 0; p = d->v[p].next) {
                if (!strcmp(d->v[p].name, "p")) {
                    const xdoc* ds[2] = { d, C->master ? &C->master->x : d };
                    int ns[2] = { xd_kid(d, tx, "lstStyle"), C->master ? xd_path(&C->master->x,
                                  xd_find(&C->master->x, "txStyles"), "otherStyle") : -1
                                };

                    lvl_chain(&T, ds, ns, 2, 0);
                    put_paragraph(C->o, &T, d, p, &autonum);
                }
            }

            if (tx < 0 || !has_text(d, tx)) {
                pb_puts(C->o, "<w:p/>");
            }

            pb_puts(C->o, "</w:tc>");
        }

        pb_puts(C->o, "</w:tr>");
        row++;
    }

    (void)total;
    pb_puts(C->o, "</w:tbl><w:p/></w:txbxContent></wps:txbx><wps:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\"/>"
            "</wps:wsp>");
}

/* SmartArt: the drawing PowerPoint keeps of it (its shapes, as it last laid them out), in a group at the frame */
static void put_smartart(conv* C, ppart* pt, int gf) {
    const xdoc* d = &pt->x;
    int ri = xd_find(d, "relIds"), xf = xd_kid(d, gf, "xfrm"), off, ext, k;
    char dm[64];
    const char* dmpath, *drawpath = NULL;
    ppart dmp, dr;

    if (ri < 0 || xf < 0 || !xd_attr(d, ri, "r:dm", dm, sizeof(dm)) || !(dmpath = rel_of(pt, dm)) ||
            !part_load(C->P, &dmp, dmpath)) {
        return;
    }

    k = xd_find(&dmp.x, "dataModelExt");

    if (k >= 0) {
        char rid[64];

        if (xd_attr(&dmp.x, k, "relId", rid, sizeof(rid))) {
            drawpath = rel_of(pt, rid);
        }
    }

    if (drawpath && part_load(C->P, &dr, drawpath)) {
        int tree = xd_find(&dr.x, "spTree");

        off = xd_kid(d, xf, "off");
        ext = xd_kid(d, xf, "ext");

        if (tree >= 0 && off >= 0 && ext >= 0) {
            pb_printf(C->o, "<wpg:grpSp><wpg:cNvGrpSpPr/><wpg:grpSpPr><a:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext "
                      "cx=\"%lld\" cy=\"%lld\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
                      "</wpg:grpSpPr>", xd_int(d, off, "x", 0), xd_int(d, off, "y", 0), xd_int(d, ext, "cx", 0),
                      xd_int(d, ext, "cy", 0), xd_int(d, ext, "cx", 0), xd_int(d, ext, "cy", 0));
            put_tree(C, &dr, tree, 0);
            pb_puts(C->o, "</wpg:grpSp>");
        }

        part_free(&dr);
    }

    part_free(&dmp);
}

/* a shape filled with a picture as that picture: its fill's stretch (insets past its box: the picture cropped
   by the box) made a crop */
static void put_blip_shape(conv* C, ppart* pt, int sp, int lsp, int msp) {
    const xdoc* d = &pt->x;
    int bf = xd_path(d, sp, "spPr/blipFill"), bl = xd_kid(d, bf, "blip"), fr = xd_path(d, bf, "stretch/fillRect");
    int src = xd_kid(d, bf, "srcRect"), spr = xd_kid(d, sp, "spPr"), k;
    char rid[64];
    const char* m;
    double in[4] = { 0, 0, 0, 0 };
    static const char* sides[4] = { "l", "t", "r", "b" };
    xref xf = { d, xd_kid(d, spr, "xfrm") };

    if (!xd_attr(d, bl, "r:embed", rid, sizeof(rid)) || (m = media_id(C->P, pt, rid)) == NULL) {
        return;
    }

    if (xf.n < 0 && lsp >= 0) {
        xf.d = &C->layout->x;
        xf.n = xd_path(xf.d, lsp, "spPr/xfrm");
    }

    if (xf.n < 0 && msp >= 0) {
        xf.d = &C->master->x;
        xf.n = xd_path(xf.d, msp, "spPr/xfrm");
    }

    if (xf.n < 0) {
        return;
    }

    pb_puts(C->o, "<pic:pic><pic:nvPicPr>");
    put_nvpr(C, d, sp, "nvSpPr/cNvPr", "pic:cNvPr");
    pb_printf(C->o, "<pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"%s\"/>", m);

    if (src >= 0) {
        xd_raw(C->o, d, src);
    } else if (fr >= 0) {
        for (k = 0; k < 4; k++) {
            in[k] = -xd_int(d, fr, sides[k], 0) / 100000.0;     /* past the box: positive */
        }

        if (in[0] > 0 || in[1] > 0 || in[2] > 0 || in[3] > 0) {
            double w = 1 + in[0] + in[2], h = 1 + in[1] + in[3];

            pb_printf(C->o, "<a:srcRect l=\"%d\" t=\"%d\" r=\"%d\" b=\"%d\"/>", (int)(in[0] > 0 ? in[0] / w * 100000 : 0),
                      (int)(in[1] > 0 ? in[1] / h * 100000 : 0), (int)(in[2] > 0 ? in[2] / w * 100000 : 0),
                      (int)(in[3] > 0 ? in[3] / h * 100000 : 0));
        }
    }

    pb_puts(C->o, "<a:stretch><a:fillRect/></a:stretch></pic:blipFill><pic:spPr>");
    xd_raw(C->o, xf.d, xf.n);
    pb_puts(C->o, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>");
}

static void put_shape(conv* C, ppart* pt, int n, int skip_ph) {
    const xdoc* d = &pt->x;
    const char* el = d->v[n].name;
    char type[32], v[16];
    long long idx;
    int isph = 0, lsp = -1, msp = -1, kind = 0;

    if (!strcmp(el, "sp") || !strcmp(el, "pic") || !strcmp(el, "graphicFrame")) {
        isph = ph_of(d, n, type, sizeof(type), &idx);

        if (isph && skip_ph) {
            return;     /* a layout's or a master's placeholder: the slide's own, if it has it */
        }

        if (isph) {
            kind = ph_kind(type);
            lsp = C->layout ? ph_match(&C->layout->x, type, idx, 0) : -1;
            msp = C->master ? ph_match(&C->master->x, type, idx, 1) : -1;
        }
    }

    if (!strcmp(el, "sp") && xd_path(d, n, "spPr/blipFill/blip") >= 0) {
        put_blip_shape(C, pt, n, lsp, msp);     /* a shape filled with a picture: the picture */
    } else if (!strcmp(el, "sp")) {
        int tx = xd_kid(d, n, "txBody"), st = xd_kid(d, n, "style"), cn = xd_path(d, n, "nvSpPr/cNvSpPr");
        size_t mark = C->o->n;

        pb_puts(C->o, "<wps:wsp>");
        put_nvpr(C, d, n, "nvSpPr/cNvPr", "wps:cNvPr");
        pb_puts(C->o, xd_attr(d, cn, "txBox", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true")) ?
                "<wps:cNvSpPr txBox=\"1\"/>" : "<wps:cNvSpPr/>");

        if (!put_sppr(C, pt, n, lsp, msp, "wps:spPr")) {
            C->o->n = mark;     /* nowhere to put it */
            return;
        }

        if (st >= 0) {
            put_renamed(C->o, d, st, "wps:style");
        }

        if (tx >= 0 && has_text(d, tx)) {
            put_txbody(C, pt, n, tx, isph ? kind : 0, lsp, msp);
        }

        put_bodypr(C, d, tx, lsp, msp);
        pb_puts(C->o, "</wps:wsp>");
    } else if (!strcmp(el, "cxnSp")) {
        int cnv = xd_path(d, n, "nvCxnSpPr/cNvCxnSpPr"), st = xd_kid(d, n, "style"), k;
        size_t mark = C->o->n;

        pb_puts(C->o, "<wps:wsp>");
        put_nvpr(C, d, n, "nvCxnSpPr/cNvPr", "wps:cNvPr");
        pb_puts(C->o, "<wps:cNvCnPr>");

        for (k = cnv >= 0 ? d->v[cnv].kid : -1; k >= 0; k = d->v[k].next) {
            if (!strcmp(d->v[k].name, "stCxn") || !strcmp(d->v[k].name, "endCxn")) {
                pb_printf(C->o, "<a:%s id=\"%lld\" idx=\"%lld\"/>", d->v[k].name, xd_int(d, k, "id", 0) + C->id_base,
                          xd_int(d, k, "idx", 0));
            }
        }

        pb_puts(C->o, "</wps:cNvCnPr>");

        if (!put_sppr(C, pt, n, -1, -1, "wps:spPr")) {
            C->o->n = mark;
            return;
        }

        if (st >= 0) {
            put_renamed(C->o, d, st, "wps:style");
        }

        pb_puts(C->o, "<wps:bodyPr/></wps:wsp>");
    } else if (!strcmp(el, "pic")) {
        int bf = xd_kid(d, n, "blipFill");
        size_t mark = C->o->n;

        if (bf < 0) {
            return;
        }

        pb_puts(C->o, "<pic:pic><pic:nvPicPr>");
        put_nvpr(C, d, n, "nvPicPr/cNvPr", "pic:cNvPr");
        pb_puts(C->o, "<pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>");
        put_with_media(C->o, C, pt, d->s + d->v[bf].ia, d->v[bf].ib - d->v[bf].ia);
        pb_puts(C->o, "</pic:blipFill>");

        if (!put_sppr(C, pt, n, lsp, msp, "pic:spPr")) {
            C->o->n = mark;
            return;
        }

        pb_puts(C->o, "</pic:pic>");
    } else if (!strcmp(el, "grpSp")) {
        int gp = xd_kid(d, n, "grpSpPr");

        pb_puts(C->o, "<wpg:grpSp><wpg:cNvGrpSpPr/>");

        if (gp >= 0) {
            put_renamed(C->o, d, gp, "wpg:grpSpPr");
        } else {
            pb_puts(C->o, "<wpg:grpSpPr/>");
        }

        put_tree(C, pt, n, skip_ph);
        pb_puts(C->o, "</wpg:grpSp>");
    } else if (!strcmp(el, "graphicFrame")) {
        int gd = xd_path(d, n, "graphic/graphicData"), tbl = gd >= 0 ? xd_kid(d, gd, "tbl") : -1;

        if (tbl >= 0) {
            put_table(C, pt, n, tbl);
        } else if (gd >= 0 && xd_kid(d, gd, "relIds") >= 0) {
            put_smartart(C, pt, n);
        }
    } else if (!strcmp(el, "AlternateContent")) {   /* what a newer PowerPoint has: its fallback */
        int fb = xd_kid(d, n, "Fallback"), k;

        for (k = fb >= 0 ? d->v[fb].kid : -1; k >= 0; k = d->v[k].next) {
            put_shape(C, pt, k, skip_ph);
        }
    }
}

static void put_tree(conv* C, ppart* pt, int tree, int skip_ph) {
    int k;

    for (k = pt->x.v[tree].kid; k >= 0; k = pt->x.v[k].next) {
        put_shape(C, pt, k, skip_ph);
    }
}

/* ---- slides ---- */

/* a part's background, as the canvas's (-1: none of its own) */
static long background_of(const pptx* P, const ppart* pt) {
    const xdoc* d = &pt->x;
    int bg = xd_find(d, "bg"), k;

    if (bg < 0) {
        return -1;
    }

    if ((k = xd_kid(d, bg, "bgPr")) >= 0) {
        int g = xd_kid(d, k, "gradFill");
        long c = fill_colour(P, d, k);

        if (c < 0 && g >= 0) {  /* a gradient: its first stop */
            int gs = xd_path(d, g, "gsLst/gs");

            c = gs >= 0 && d->v[gs].kid >= 0 ? colour_of(P, d, d->v[gs].kid) : -1;
        }

        return c;
    }

    if ((k = xd_kid(d, bg, "bgRef")) >= 0 && d->v[k].kid >= 0) {
        return colour_of(P, d, d->v[k].kid);
    }

    return -1;
}

static void put_slide(pptx* P, const xdoc* pres, int pres_style, const char* path, int no, int last) {
    ppart slide, layout, master;
    int have_l = 0, have_m = 0, tree, show = 1;
    const char* lp, *mp;
    long bg;
    conv C;
    pd_buf cv;
    char v[16];

    memset(&cv, 0, sizeof(cv));

    if (!part_load(P, &slide, path)) {
        return;
    }

    if ((lp = rel_like(&slide, "slideLayouts/")) != NULL && part_load(P, &layout, lp)) {
        have_l = 1;

        if ((mp = rel_like(&layout, "slideMasters/")) != NULL && part_load(P, &master, mp)) {
            have_m = 1;
        }
    }

    memset(&C, 0, sizeof(C));
    C.P = P;
    C.slide_no = no;
    C.slide = &slide;
    C.layout = have_l ? &layout : NULL;
    C.master = have_m ? &master : NULL;
    C.pres = pres;
    C.pres_style = pres_style;
    C.o = &cv;

    /* the background: the slide's, else its layout's, else its master's; white */
    bg = background_of(P, &slide);

    if (bg < 0 && have_l) {
        bg = background_of(P, &layout);
    }

    if (bg < 0 && have_m) {
        bg = background_of(P, &master);
    }

    pb_printf(&cv, "<wpc:bg><a:solidFill><a:srgbClr val=\"%06lX\"/></a:solidFill></wpc:bg><wpc:whole/>",
              (unsigned long)(bg >= 0 ? bg : 0xFFFFFF));

    /* what the master and the layout draw on every slide (unless the slide or the layout says not), behind it */
    if (xd_attr(&slide.x, 0, "showMasterSp", v, sizeof(v)) && (!strcmp(v, "0") || !strcmp(v, "false"))) {
        show = 0;
    }

    if (show && have_m && !(have_l && xd_attr(&layout.x, 0, "showMasterSp", v, sizeof(v)) &&
                            (!strcmp(v, "0") || !strcmp(v, "false"))) && (tree = xd_find(&master.x, "spTree")) >= 0) {
        C.id_base = 2000000;
        put_tree(&C, &master, tree, 1);
    }

    if (show && have_l && (tree = xd_find(&layout.x, "spTree")) >= 0) {
        C.id_base = 1000000;
        put_tree(&C, &layout, tree, 1);
    }

    if ((tree = xd_find(&slide.x, "spTree")) >= 0) {
        C.id_base = 0;
        put_tree(&C, &slide, tree, 0);
    }

    /* the slide: a page as big as it, its canvas in front of an empty paragraph, from the page's corner */
    P->docpr++;
    pb_printf(&P->doc, "<w:p><w:pPr><w:spacing w:before=\"0\" w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/>");

    if (!last) {
        pb_printf(&P->doc, "<w:sectPr><w:pgSz w:w=\"%lld\" w:h=\"%lld\"%s/><w:pgMar w:top=\"0\" w:right=\"0\" "
                  "w:bottom=\"0\" w:left=\"0\" w:header=\"0\" w:footer=\"0\" w:gutter=\"0\"/></w:sectPr>",
                  P->sw / 635, P->sh / 635, P->sw > P->sh ? " w:orient=\"landscape\"" : "");
    }

    pb_printf(&P->doc, "<w:rPr><w:sz w:val=\"2\"/></w:rPr></w:pPr><w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" "
              "distL=\"0\" distR=\"0\" simplePos=\"0\" relativeHeight=\"%d\" behindDoc=\"0\" locked=\"0\" "
              "layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"page\">"
              "<wp:posOffset>0</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"page\"><wp:posOffset>0"
              "</wp:posOffset></wp:positionV><wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:effectExtent l=\"0\" t=\"0\" "
              "r=\"0\" b=\"0\"/><wp:wrapNone/><wp:docPr id=\"%d\" name=\"Slide %d\"/><wp:cNvGraphicFramePr/>"
              "<a:graphic><a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas\">"
              "<wpc:wpc>", P->docpr, P->sw, P->sh, P->docpr, P->docpr);
    pb_put(&P->doc, cv.p, cv.n);
    pb_puts(&P->doc, "</wpc:wpc></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r></w:p>");
    pb_free(&cv);

    if (have_m) {
        part_free(&master);
    }

    if (have_l) {
        part_free(&layout);
    }

    part_free(&slide);
}

pd_status pd_pptx_import(pd_doc* d, const unsigned char* s, size_t n) {
    pptx P;
    ppart pres;
    pd_buf zip;
    int lst, k, nslides = 0, idx = 0, ds;
    const char* paths[1024];
    char tp[300];
    pd_status st;

    memset(&P, 0, sizeof(P));
    P.zip = s;
    P.zn = n;

    if (!part_load(&P, &pres, "ppt/presentation.xml")) {
        return PD_ERR_FORMAT;
    }

    k = xd_find(&pres.x, "sldSz");
    P.sw = xd_int(&pres.x, k, "cx", 9144000);
    P.sh = xd_int(&pres.x, k, "cy", 6858000);
    ds = xd_find(&pres.x, "defaultTextStyle");

    /* the theme: the first master's */
    {
        int mid = xd_path(&pres.x, xd_find(&pres.x, "sldMasterIdLst"), "sldMasterId");
        const char* mp = NULL;
        ppart m;

        if (mid >= 0 && xd_attr(&pres.x, mid, "r:id", tp, sizeof(tp))) {
            mp = rel_of(&pres, tp);
        }

        if (mp && part_load(&P, &m, mp)) {
            theme_load(&P, rel_like(&m, "theme/"));
            part_free(&m);
        } else {
            theme_load(&P, NULL);
        }
    }

    lst = xd_find(&pres.x, "sldIdLst");

    for (k = lst >= 0 ? pres.x.v[lst].kid : -1; k >= 0 && nslides < 1024; k = pres.x.v[k].next) {
        if (xd_attr(&pres.x, k, "r:id", tp, sizeof(tp)) && rel_of(&pres, tp)) {
            paths[nslides++] = rel_of(&pres, tp);
        }
    }

    memset(&zip, 0, sizeof(zip));
    P.zw = pd_zipw_new(&zip);

    if (!P.zw) {
        part_free(&pres);
        return PD_ERR_NOMEM;
    }

    pb_puts(&P.doc, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
            "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
            "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\" "
            "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\" "
            "xmlns:wpg=\"http://schemas.microsoft.com/office/word/2010/wordprocessingGroup\" "
            "xmlns:wpc=\"http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas\"><w:body>");
    pb_puts(&P.rels, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships "
            "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rIdPt\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "theme\" Target=\"theme/theme1.xml\"/>");

    for (idx = 0; idx < nslides; idx++) {
        put_slide(&P, &pres.x, ds, paths[idx], idx + 1, idx == nslides - 1);
    }

    if (nslides == 0) {
        pb_puts(&P.doc, "<w:p/>");
    }

    pb_printf(&P.doc, "<w:sectPr><w:pgSz w:w=\"%lld\" w:h=\"%lld\"%s/><w:pgMar w:top=\"0\" w:right=\"0\" w:bottom=\"0\" "
              "w:left=\"0\" w:header=\"0\" w:footer=\"0\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>",
              P.sw / 635, P.sh / 635, P.sw > P.sh ? " w:orient=\"landscape\"" : "");
    pb_puts(&P.rels, "</Relationships>");
    {
        static const char types[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Types "
                                    "xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>";

        pd_zipw_add(P.zw, "[Content_Types].xml", types, sizeof(types) - 1);
    }
    pd_zipw_add(P.zw, "word/document.xml", P.doc.p, P.doc.n);
    pd_zipw_add(P.zw, "word/_rels/document.xml.rels", P.rels.p, P.rels.n);

    {   /* the theme, for the colours shapes name by their place in it */
        const char* mp = NULL;
        int mid = xd_path(&pres.x, xd_find(&pres.x, "sldMasterIdLst"), "sldMasterId");
        ppart m;

        if (mid >= 0 && xd_attr(&pres.x, mid, "r:id", tp, sizeof(tp))) {
            mp = rel_of(&pres, tp);
        }

        if (mp && part_load(&P, &m, mp)) {
            const char* th = rel_like(&m, "theme/");
            size_t tl = 0;
            unsigned char* t = th ? pd_zip_get(s, n, th, &tl) : NULL;

            if (t) {
                pd_zipw_add(P.zw, "word/theme/theme1.xml", t, tl);
                free(t);
            }

            part_free(&m);
        }
    }

    pd_zipw_finish(P.zw);
    pb_free(&P.doc);
    pb_free(&P.rels);
    part_free(&pres);
    if (getenv("PD_PPTX_DOCX")) {   /* for a look at what the slides were made into */
        FILE* f = fopen(getenv("PD_PPTX_DOCX"), "wb");

        if (f) {
            fwrite(zip.p, 1, zip.n, f);
            fclose(f);
        }
    }

    st = pd_docx_import(d, (const unsigned char*)zip.p, zip.n);
    pb_free(&zip);
    return st;
}
