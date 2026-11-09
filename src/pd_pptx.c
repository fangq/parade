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
#include "pd_dgm.h"
#include "pd_doc_internal.h"
#include "pd_xdoc.h"

/* ---- the package ---- */

typedef struct {
    char id[64], target[256];   /* the target as a path in the package (an external one as it is: a URL) */
    int external;
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
    char major_cs[64], minor_cs[64];    /* and for complex scripts (cs, else its Arabic or Hebrew font), "" none */
    char major_ea[64], minor_ea[64];    /* and for East Asian text, "" none */
    double line_w[3];           /* the theme's line widths (EMU) */
    char theme_path[256];       /* the theme's part, "" none */
    char doc_theme[256];        /* the theme the .docx made carries (the first master's): colours of the slides with
                                   it are linked to it */
    uint32_t last_ref;          /* the theme colour the last colour_of came to, 0 none (pd_theme_color) */
    long ph;                    /* what a theme style's phClr is while it is copied, -1 none */
    int docpr;
    long long sw, sh;           /* the slide size (EMU) */
    int alias[4];               /* bg1, tx1, bg2, tx2: the theme's colours they are (the master's clrMap) */
    char fonts[96][64];         /* the faces the text names, for the font table: what each is like */
    int nfonts;
    const xdoc* tstyles;        /* ppt/tableStyles.xml, NULL if none */
    int cells;                  /* tables' cell fills made shapes: their ids */
    int docx;                   /* a .docx's SmartArt: pictures named by the package's own paths (in rels) */
    int nlinks;                 /* the links made relationships of the .docx */
    int ncharts;
    char charts[64][128];       /* the charts put in it so far: their parts' paths in the package */
    struct pslide {             /* each slide made a page: its layout, its own background, its colour, whether it */
        char layout[256];       /* shows its master's shapes -- for a writer to put it back on its layout */
        int own_bg, no_master;
        long bg;
        char clr[256];          /* its own colour map (a:overrideClrMapping's attributes), "" the master's */
        char notes[256];        /* its notes slide's part ("" none) */
        char* tail;             /* what follows its colour map as it was (its transition, timing, extensions), after
                                   a line of the namespaces and mc:Ignorable of its root they need; malloc'ed, or NULL */
    } *rec;
    int nrec, caprec;
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

/* a part's relationships, their targets as paths in the package */
static void part_rels(pptx* P, ppart* pt) {
    size_t len = 0;
    char* s;
    char rpath[300];
    const char* slash = strrchr(pt->path, '/');
    xdoc r;
    int k;

    snprintf(rpath, sizeof(rpath), "%.*s_rels/%s.rels", slash ? (int)(slash - pt->path + 1) : 0, pt->path,
             slash ? slash + 1 : pt->path);
    s = (char*)pd_zip_get(P->zip, P->zn, rpath, &len);

    if (s && xd_parse(&r, s, len)) {
        pt->rels = (prel*)calloc((size_t)r.nv, sizeof(prel));

        for (k = 0; pt->rels && k < r.nv; k++) {
            char tgt[256], mode[16];

            if (strcmp(r.v[k].name, "Relationship")) {
                continue;
            }

            xd_attr(&r, k, "Id", pt->rels[pt->nrels].id, sizeof(pt->rels[0].id));
            xd_attr(&r, k, "Target", tgt, sizeof(tgt));
            pt->rels[pt->nrels].external = xd_attr(&r, k, "TargetMode", mode, sizeof(mode)) && !strcmp(mode, "External");

            if (pt->rels[pt->nrels].external) {
                snprintf(pt->rels[pt->nrels].target, sizeof(pt->rels[0].target), "%s", tgt);
            } else {
                resolve(pt->path, tgt, pt->rels[pt->nrels].target, sizeof(pt->rels[0].target));
            }

            pt->nrels++;
        }

        xd_free(&r);
    } else if (s) {
        free(s);
    }
}

static int part_load(pptx* P, ppart* pt, const char* path) {
    size_t len = 0;
    char* s;

    memset(pt, 0, sizeof(*pt));
    snprintf(pt->path, sizeof(pt->path), "%s", path);
    s = (char*)pd_zip_get(P->zip, P->zn, path, &len);

    if (!s || !xd_parse(&pt->x, s, len)) {
        xd_free(&pt->x);
        return 0;
    }

    part_rels(P, pt);
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
static void put_esc(pd_buf* o, const char* s);

static const char* media_id(pptx* P, const ppart* pt, const char* rid) {
    const char* path = rel_of(pt, rid), *name;
    unsigned char* data;
    size_t len = 0;
    char dst[200];
    int k;

    if (!path || (!P->zw && !P->docx)) {
        return NULL;
    }

    for (k = 0; k < P->nmedia; k++) {
        if (!strcmp(P->media[k], path)) {
            return P->media_id[k];
        }
    }

    if (P->docx) {  /* a .docx's SmartArt: the picture where it is in the package, by an id of its own */
        if (P->nmedia >= 256) {
            return NULL;
        }

        snprintf(P->media[P->nmedia], sizeof(P->media[0]), "%.127s", path);
        snprintf(P->media_id[P->nmedia], sizeof(P->media_id[0]), "rIdPdS%d", P->nmedia);
        pb_printf(&P->rels, "<Relationship Id=\"%s\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                  "relationships/image\" Target=\"/", P->media_id[P->nmedia]);
        put_esc(&P->rels, path);
        pb_puts(&P->rels, "\"/>");
        return P->media_id[P->nmedia++];
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

static const char* const SLOTS[12] = { "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3", "accent4", "accent5",
                                       "accent6", "hlink", "folHlink"
                                     };

/* a scheme colour's place in the theme: through the slide's colour map for the background and text ones */
static int theme_slot_of(const pptx* P, const char* v) {
    const char** names = (const char**)SLOTS;
    int k;

    if (!strcmp(v, "bg1")) {
        return P->alias[0];
    }

    if (!strcmp(v, "tx1")) {
        return P->alias[1];
    }

    if (!strcmp(v, "bg2")) {
        return P->alias[2];
    }

    if (!strcmp(v, "tx2")) {
        return P->alias[3];
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

    ((pptx*)P)->last_ref = 0;

    if (c < 0) {
        return -1;
    }

    if (!strcmp(d->v[c].name, "srgbClr") && xd_attr(d, c, "val", v, sizeof(v))) {
        rgb = hexrgb(v);
    } else if (!strcmp(d->v[c].name, "schemeClr") && xd_attr(d, c, "val", v, sizeof(v)) && theme_slot_of(P, v) >= 0) {
        rgb = P->theme[theme_slot_of(P, v)];
    } else if (!strcmp(d->v[c].name, "sysClr") && xd_attr(d, c, "lastClr", v, sizeof(v))) {
        rgb = hexrgb(v);
    } else if (!strcmp(d->v[c].name, "scrgbClr")) {   /* percentages, linear */
        rgb = ((uint32_t)(xd_int(d, c, "r", 0) * 255 / 100000) << 16) | ((uint32_t)(xd_int(d, c, "g", 0) * 255 / 100000) << 8) |
              (uint32_t)(xd_int(d, c, "b", 0) * 255 / 100000);
    } else if (!strcmp(d->v[c].name, "prstClr") && xd_attr(d, c, "val", v, sizeof(v))) {
        rgb = !strcmp(v, "white") ? 0xFFFFFF : !strcmp(v, "red") ? 0xFF0000 : !strcmp(v, "blue") ? 0x0000FF : 0;
    } else {
        return -1;
    }

    rgb_hsl(rgb, &h, &s, &l);

    {
        double mul = 1, add = 0;    /* what the changes do to the lightness: l * mul + add */
        int plain = 1;

        for (k = d->v[c].kid; k >= 0; k = d->v[k].next) {
            double val = xd_int(d, k, "val", 100000) / 100000.0;

            if (!strcmp(d->v[k].name, "lumMod") || !strcmp(d->v[k].name, "shade")) {
                l *= val;
                mul *= val;
                add *= val;
            } else if (!strcmp(d->v[k].name, "lumOff")) {
                l += val;
                add += val;
            } else if (!strcmp(d->v[k].name, "tint")) {
                l = l + (1 - l) * (1 - val);
                mul *= val;
                add = add * val + (1 - val);
            } else {
                plain = 0;  /* alpha, satMod...: a colour of its own */
            }
        }

        /* a theme colour of the document's own theme: linked to it */
        if (plain && !strcmp(d->v[c].name, "schemeClr") && P->theme_path[0] && !strcmp(P->theme_path, P->doc_theme)) {
            xd_attr(d, c, "val", v, sizeof(v));
            ((pptx*)P)->last_ref = pd_theme_color(theme_slot_of(P, v), (int32_t)(mul * 100000 + 0.5),
                                                  (int32_t)(add * 100000 + (add < 0 ? -0.5 : 0.5)));
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
    P->line_w[0] = 6350;
    P->line_w[1] = 12700;
    P->line_w[2] = 19050;
    snprintf(P->major, sizeof(P->major), "Calibri Light");
    snprintf(P->minor, sizeof(P->minor), "Calibri");
    P->major_cs[0] = P->minor_cs[0] = P->major_ea[0] = P->minor_ea[0] = '\0';
    P->theme_path[0] = '\0';

    if (!path || !part_load(P, &t, path)) {
        return;
    }

    snprintf(P->theme_path, sizeof(P->theme_path), "%s", path);

    s = xd_find(&t.x, "clrScheme");

    for (k = 0; s >= 0 && k < 12; k++) {
        int e = xd_kid(&t.x, s, names[k]);
        long c = e >= 0 ? colour_of(P, &t.x, t.x.v[e].kid) : -1;

        if (c >= 0) {
            P->theme[k] = (uint32_t)c;
        }
    }

    s = xd_find(&t.x, "lnStyleLst");

    for (k = 0, f = s >= 0 ? t.x.v[s].kid : -1; f >= 0 && k < 3; f = t.x.v[f].next) {
        if (!strcmp(t.x.v[f].name, "ln")) {
            P->line_w[k] = (double)xd_int(&t.x, f, "w", (long long)P->line_w[k]);
            k++;
        }
    }

    f = xd_path(&t.x, xd_find(&t.x, "majorFont"), "latin");
    xd_attr(&t.x, f, "typeface", P->major, sizeof(P->major));
    f = xd_path(&t.x, xd_find(&t.x, "minorFont"), "latin");
    xd_attr(&t.x, f, "typeface", P->minor, sizeof(P->minor));

    xd_attr(&t.x, xd_path(&t.x, xd_find(&t.x, "majorFont"), "ea"), "typeface", P->major_ea, sizeof(P->major_ea));
    xd_attr(&t.x, xd_path(&t.x, xd_find(&t.x, "minorFont"), "ea"), "typeface", P->minor_ea, sizeof(P->minor_ea));

    for (k = 0; k < 2; k++) {   /* the complex scripts' faces: cs, else the Arabic script's, else the Hebrew */
        int ft = xd_find(&t.x, k ? "minorFont" : "majorFont"), e;
        char* out = k ? P->minor_cs : P->major_cs;

        xd_attr(&t.x, xd_kid(&t.x, ft, "cs"), "typeface", out, 64);

        for (e = ft >= 0 ? t.x.v[ft].kid : -1; e >= 0 && !out[0]; e = t.x.v[e].next) {
            char sc[16];

            if (!strcmp(t.x.v[e].name, "font") && xd_attr(&t.x, e, "script", sc, sizeof(sc)) && !strcmp(sc, "Arab")) {
                xd_attr(&t.x, e, "typeface", out, 64);
            }
        }

        for (e = ft >= 0 ? t.x.v[ft].kid : -1; e >= 0 && !out[0]; e = t.x.v[e].next) {
            char sc[16];

            if (!strcmp(t.x.v[e].name, "font") && xd_attr(&t.x, e, "script", sc, sizeof(sc)) && !strcmp(sc, "Hebr")) {
                xd_attr(&t.x, e, "typeface", out, 64);
            }
        }
    }
    part_free(&t);
}

/* markup copied with its scheme colours made the RGB the slide's theme and colour map give them -- the .docx made
   has one theme, and a presentation a theme for each master and a colour map for each slide */
static void put_mapped(const pptx* P, pd_buf* o, const char* s, size_t n) {
    static const char open_[] = "<a:schemeClr val=\"", close_[] = "</a:schemeClr>";
    char conv[64];
    int depth = 0;
    size_t i = 0;

    while (i < n) {
        if (s[i] == '<' && n - i >= sizeof(open_) - 1 && !memcmp(s + i, open_, sizeof(open_) - 1)) {
            size_t j = i + sizeof(open_) - 1, k = 0;
            char v[40];
            int slot, self;

            while (j < n && s[j] != '"' && k < sizeof(v) - 1) {
                v[k++] = s[j++];
            }

            v[k] = '\0';
            slot = theme_slot_of(P, v);

            if (slot < 0 && P->ph >= 0 && !strcmp(v, "phClr")) {   /* a theme style's placeholder: its colour */
                for (k = j; k < n && s[k] != '>'; k++) {
                }

                self = k > 0 && k < n && s[k - 1] == '/';
                pb_printf(o, "<a:srgbClr val=\"%06lX\"", (unsigned long)(P->ph & 0xFFFFFF));
                i = j + 1;

                if (!self && depth < 64) {
                    conv[depth++] = 1;
                }

                continue;
            }

            for (k = j; k < n && s[k] != '>'; k++) {
            }

            self = k > 0 && k < n && s[k - 1] == '/';

            if (slot >= 0) {
                pb_printf(o, "<a:srgbClr val=\"%06X\"", (unsigned)(P->theme[slot] & 0xFFFFFFu));
                i = j + 1;  /* after its value's closing quote: its other attributes and its end as they are */

                if (!self && depth < 64) {
                    conv[depth++] = 1;
                }

                continue;
            }

            if (!self && depth < 64) {
                conv[depth++] = 0;
            }
        } else if (s[i] == '<' && n - i >= sizeof(close_) - 1 && !memcmp(s + i, close_, sizeof(close_) - 1)) {
            int was = depth > 0 ? conv[--depth] : 0;

            pb_puts(o, was ? "</a:srgbClr>" : close_);
            i += sizeof(close_) - 1;
            continue;
        }

        pb_putc(o, s[i++]);
    }
}

static void xd_raw_mapped(const pptx* P, pd_buf* o, const xdoc* d, int n) {
    put_mapped(P, o, d->s + d->v[n].a, d->v[n].b - d->v[n].a);
}

/* the colour map a part gives (p:clrMap, or p:clrMapOvr's a:overrideClrMapping): bg1, tx1, bg2, tx2 */
static void clrmap_load(pptx* P, const xdoc* d) {
    static const char* names[4] = { "bg1", "tx1", "bg2", "tx2" };
    int m = xd_find(d, "clrMap"), k, q;

    if (m < 0) {
        m = xd_find(d, "overrideClrMapping");
    }

    for (k = 0; m >= 0 && k < 4; k++) {
        char v[32];

        if (xd_attr(d, m, names[k], v, sizeof(v))) {
            for (q = 0; q < 4; q++) {
                if (!strcmp(v, SLOTS[q])) {
                    P->alias[k] = q;
                }
            }
        }
    }
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
    int force_bold;             /* a table style's bold text */
    int bullet;                 /* while its bullet is put: */
    long bu_colour;             /* the bullet's colour (buClr), -1 the text's */
    double bu_scale;            /* and its size (buSzPct), 0 the text's */
    long bu_sz;                 /* its size in hundredths of a point (buSzPts), 0 none */
    const char* bu_face;        /* its face (buFont), NULL the text's */
    const ppart* parts[3];      /* the parts the levels' properties are in: for a picture bullet's picture */
    long def_sz;                /* the size of runs that give none (hundredths of a point), 0: 18 points */
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

/* of paragraph property elements that stand for one another (buChar, buAutoNum, buNone...), the nearest: *which its
   index in NAMES, -1 none */
static xref ppr_first(const tctx* T, const xdoc* d, int ppr, const char* const* names, int n, int* which) {
    xref r = { NULL, -1 };
    int k, q;

    *which = -1;

    for (k = -1; k < T->nlvl; k++) {
        const xdoc* x = k < 0 ? d : T->lvl[k].d;
        int e = k < 0 ? ppr : T->lvl[k].n;

        for (q = 0; e >= 0 && q < n; q++) {
            if ((r.n = xd_kid(x, e, names[q])) >= 0) {
                r.d = x;
                *which = q;
                return r;
            }
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

    if (c < 0 && T->font_colour >= 0) {
        return T->font_colour;  /* the shape's style's (or the table style's) over the master's defaults */
    }

    for (k = 0; c < 0 && k < T->nlvl; k++) {
        int dr = xd_kid(T->lvl[k].d, T->lvl[k].n, "defRPr");

        c = dr >= 0 ? fill_colour(T->P, T->lvl[k].d, dr) : -1;
    }

    return c >= 0 ? c : T->font_colour >= 0 ? T->font_colour : (long)T->P->theme[0];
}

/* a run's face; *th the theme's font it is (PD_FONT_THEME_MAJOR, _MINOR), 0 a face of its own */
static void rpr_font(const tctx* T, const xdoc* d, int rpr, char* face, size_t cap, int* th) {
    int k, l = rpr >= 0 ? xd_kid(d, rpr, "latin") : -1;

    face[0] = '\0';

    if (l >= 0) {
        xd_attr(d, l, "typeface", face, cap);
    }

    for (k = 0; !face[0] && k < T->nlvl; k++) {
        xd_attr(T->lvl[k].d, xd_path(T->lvl[k].d, T->lvl[k].n, "defRPr/latin"), "typeface", face, cap);
    }

    *th = 0;

    if (!face[0] || !strcmp(face, "+mn-lt")) {
        *th = T->title && !face[0] ? PD_FONT_THEME_MAJOR : PD_FONT_THEME_MINOR;
        snprintf(face, cap, "%s", *th == PD_FONT_THEME_MAJOR ? T->P->major : T->P->minor);
    } else if (!strcmp(face, "+mj-lt")) {
        *th = PD_FONT_THEME_MAJOR;
        snprintf(face, cap, "%s", T->P->major);
    }

    if (!T->P->theme_path[0] || strcmp(T->P->theme_path, T->P->doc_theme)) {
        *th = 0;    /* another master's theme: not the document's */
    }
}

/* a run's face for complex scripts (Arabic, Hebrew): its own cs, its list styles', the theme's; "" none */
static void rpr_cs_font(const tctx* T, const xdoc* d, int rpr, char* face, size_t cap) {
    int k;

    face[0] = '\0';
    xd_attr(d, rpr >= 0 ? xd_kid(d, rpr, "cs") : -1, "typeface", face, cap);

    for (k = 0; !face[0] && k < T->nlvl; k++) {
        xd_attr(T->lvl[k].d, xd_path(T->lvl[k].d, T->lvl[k].n, "defRPr/cs"), "typeface", face, cap);
    }

    if (!face[0] || !strcmp(face, "+mn-cs")) {
        snprintf(face, cap, "%s", T->title ? T->P->major_cs : T->P->minor_cs);
    } else if (!strcmp(face, "+mj-cs")) {
        snprintf(face, cap, "%s", T->P->major_cs);
    }
}

/* a run's face for East Asian text: its own ea, its list styles', the theme's when it names it; "" none */
static void rpr_ea_font(const tctx* T, const xdoc* d, int rpr, char* face, size_t cap) {
    int k;

    face[0] = '\0';
    xd_attr(d, rpr >= 0 ? xd_kid(d, rpr, "ea") : -1, "typeface", face, cap);

    for (k = 0; !face[0] && k < T->nlvl; k++) {
        xd_attr(T->lvl[k].d, xd_path(T->lvl[k].d, T->lvl[k].n, "defRPr/ea"), "typeface", face, cap);
    }

    if (!strcmp(face, "+mn-ea") || !strcmp(face, "+mj-ea")) {
        snprintf(face, cap, "%s", face[2] == 'j' ? T->P->major_ea : T->P->minor_ea);
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
    int heavy, link, th;
    uint32_t ref;

    pb_puts(o, "<w:rPr>");
    rpr_font(T, d, rpr, v, sizeof(v), &th);

    if (T->bullet && T->bu_face) {  /* the bullet's own face */
        th = !strcmp(T->bu_face, "+mj-lt") ? PD_FONT_THEME_MAJOR : !strcmp(T->bu_face, "+mn-lt") ?
             PD_FONT_THEME_MINOR : 0;
        snprintf(v, sizeof(v), "%s", th == PD_FONT_THEME_MAJOR ? T->P->major : th ? T->P->minor : T->bu_face);

        if (!T->P->theme_path[0] || strcmp(T->P->theme_path, T->P->doc_theme)) {
            th = 0;
        }
    }

    heavy = font_weight(v);
    {   /* the face, for the font table */
        int q;

        for (q = 0; q < T->P->nfonts && strcmp(T->P->fonts[q], v); q++) {
        }

        if (q == T->P->nfonts && q < 96) {
            snprintf(T->P->fonts[T->P->nfonts++], sizeof(T->P->fonts[0]), "%s", v);
        }
    }
    pb_puts(o, "<w:rFonts w:ascii=\"");
    put_esc(o, v);
    pb_puts(o, "\" w:hAnsi=\"");
    put_esc(o, v);
    pb_puts(o, "\" w:cs=\"");
    {
        char cs[64];

        rpr_cs_font(T, d, rpr, cs, sizeof(cs));
        put_esc(o, cs[0] ? cs : v);
    }
    pb_putc(o, '"');
    {   /* East Asian text's face: its own ea, its list styles', the theme's ("" none: the text's) */
        char ea[64];

        rpr_ea_font(T, d, rpr, ea, sizeof(ea));

        if (ea[0]) {
            pb_puts(o, " w:eastAsia=\"");
            put_esc(o, ea);
            pb_putc(o, '"');
        }
    }

    if (th && !heavy) {
        pb_printf(o, " w:asciiTheme=\"%sHAnsi\" w:hAnsiTheme=\"%sHAnsi\"", th == PD_FONT_THEME_MAJOR ? "major" : "minor",
                  th == PD_FONT_THEME_MAJOR ? "major" : "minor");
    }

    pb_puts(o, "/>");

    if (heavy || T->force_bold || (rpr_attr(T, d, rpr, "b", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true")))) {
        pb_puts(o, "<w:b/>");
    }

    if (rpr_attr(T, d, rpr, "i", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"))) {
        pb_puts(o, "<w:i/>");
    }

    link = !T->bullet && rpr >= 0 && xd_kid(d, rpr, "hlinkClick") >= 0;

    if (link || (rpr_attr(T, d, rpr, "u", v, sizeof(v)) && strcmp(v, "none"))) {
        pb_puts(o, "<w:u w:val=\"single\"/>");
    }

    if (rpr_attr(T, d, rpr, "strike", v, sizeof(v)) && strcmp(v, "noStrike")) {
        pb_puts(o, "<w:strike/>");
    }

    if (rpr_attr(T, d, rpr, "baseline", v, sizeof(v)) && atoi(v) != 0) {
        pb_printf(o, "<w:vertAlign w:val=\"%s\"/>", atoi(v) > 0 ? "superscript" : "subscript");
    }

    if (rpr_attr(T, d, rpr, "cap", v, sizeof(v)) && strcmp(v, "none")) {     /* all, small */
        pb_puts(o, !strcmp(v, "small") ? "<w:smallCaps/>" : "<w:caps/>");
    }

    if (rpr_attr(T, d, rpr, "spc", v, sizeof(v)) && atol(v) != 0) {    /* hundredths of a point: twips */
        pb_printf(o, "<w:spacing w:val=\"%ld\"/>", (long)floor(atol(v) * T->scale / 5 + 0.5));
    }

    if (!T->bullet && rpr >= 0 && xd_kid(d, rpr, "highlight") >= 0) {     /* a colour behind the text */
        int h = xd_kid(d, rpr, "highlight");
        long hc = d->v[h].kid >= 0 ? colour_of(T->P, d, d->v[h].kid) : -1;

        if (hc >= 0) {
            pb_printf(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06lX\"/>", (unsigned long)hc & 0xFFFFFFul);
        }
    }

    /* a link in the theme's link colour */
    T->P->last_ref = 0;
    c = T->bullet && T->bu_colour >= 0 ? T->bu_colour : link ? (long)T->P->theme[10] : rpr_colour(T, d, rpr);
    ref = T->bullet && T->bu_colour >= 0 ? 0 : link ? (T->P->theme_path[0] && !strcmp(T->P->theme_path, T->P->doc_theme) ?
            pd_theme_color(PD_THEME_HLINK, 100000, 0) : 0) : T->P->last_ref;
    pb_printf(o, "<w:color w:val=\"%06lX\"", (unsigned long)c & 0xFFFFFFul);
    pd_conv_theme_attr(o, ref, 0);
    pb_puts(o, "/>");

    if (rpr_attr(T, d, rpr, "sz", v, sizeof(v))) {
        sz = atol(v);
    } else if (T->def_sz > 0) {
        sz = T->def_sz;
    }

    sz = T->bullet && T->bu_sz > 0 ? (long)(T->bu_sz * T->scale + 0.5) :
         (long)(sz * T->scale * (T->bullet && T->bu_scale > 0 ? T->bu_scale : 1) + 0.5);
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

/* a bullet character as Unicode: a symbol font's (Wingdings, Symbol; its code as is or at U+F0xx) as the
   character it shows, a plain bullet when not known */
static const char* symbol_bullet(const char* ch, const char* face) {
    static const struct {
        const char* face;
        unsigned code;
        const char* u;
    } map[] = {
        { "Wingdings", 0xA7, "\xE2\x96\xAA" }, { "Wingdings", 0x6C, "\xE2\x97\x8F" }, { "Wingdings", 0x6E, "\xE2\x96\xA0" },
        { "Wingdings", 0x6F, "\xE2\x96\xA1" }, { "Wingdings", 0x71, "\xE2\x9D\x91" }, { "Wingdings", 0x75, "\xE2\x97\x86" },
        { "Wingdings", 0x76, "\xE2\x9D\x96" }, { "Wingdings", 0x9F, "\xE2\x80\xA2" }, { "Wingdings", 0xA1, "\xE2\x97\x8B" },
        { "Wingdings", 0xD8, "\xE2\x9E\xA2" }, { "Wingdings", 0xE0, "\xE2\x86\x92" }, { "Wingdings", 0xE8, "\xE2\x9E\x94" },
        { "Wingdings", 0xFC, "\xE2\x9C\x93" }, { "Wingdings", 0xFB, "\xE2\x9C\x97" },
        { "Wingdings 2", 0x97, "\xE2\x96\xAA" }, { "Wingdings 2", 0x98, "\xE2\x96\xAA" }, { "Wingdings 2", 0x9E, "\xE2\x97\x8F" },
        { "Wingdings 2", 0xA2, "\xE2\x96\xA0" }, { "Wingdings 2", 0x50, "\xE2\x9C\x93" },
        { "Wingdings 3", 0x7D, "\xE2\x96\xB8" }, { "Wingdings 3", 0x75, "\xE2\x96\xB6" }, { "Wingdings 3", 0xC6, "\xE2\x9E\x9C" },
        { "Symbol", 0xB7, "\xE2\x80\xA2" }, { "Symbol", 0x2D, "\xE2\x80\x93" }, { "Symbol", 0xAE, "\xE2\x86\x92" }
    };
    const unsigned char* u = (const unsigned char*)ch;
    unsigned code;
    size_t k;
    int sym = !strncmp(face, "Wingdings", 9) || !strcmp(face, "Symbol") || !strcmp(face, "Webdings") ||
              !strcmp(face, "Marlett");

    if (!u[0]) {
        return "\xE2\x80\xA2";
    }

    code = u[0] < 0x80 ? u[0] : (u[0] & 0xE0) == 0xC0 && u[1] ? ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3F) :
           (u[0] & 0xF0) == 0xE0 && u[1] && u[2] ? ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3F) : 0;

    if (code >= 0xF000 && code <= 0xF0FF) {
        code -= 0xF000;
        sym = 1;
    }

    if (!sym) {
        return ch;
    }

    for (k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
        if (map[k].code == code && (!strcmp(map[k].face, face) || (!face[0] && !strcmp(map[k].face, "Wingdings")))) {
            return map[k].u;
        }
    }

    return "\xE2\x80\xA2";
}

/* where a text body's numbered paragraphs are: each level's number so far (0 none) and its scheme */
typedef struct {
    int n[9];
    char scheme[9][32];
} autonum;

/* an autonumbered paragraph's label: number V in scheme S (ST_TextAutonumberScheme) */
static void autonum_label(const char* s, int v, char* out, size_t cap) {
    static const struct {
        const char* name;
        int fmt;
    } kinds[] = {
        { "alphaLc", PD_NUM_LOWER_ALPHA }, { "alphaUc", PD_NUM_UPPER_ALPHA }, { "romanLc", PD_NUM_LOWER_ROMAN },
        { "romanUc", PD_NUM_UPPER_ROMAN }, { "arabicDb", PD_NUM_FULLWIDTH }, { "circleNumDb", PD_NUM_CIRCLED },
        { "circleNumWdWhite", PD_NUM_CIRCLED }, { "ea1", PD_NUM_CJK_COUNTING }, { "arabic1", -1 }, { "arabic2", -2 },
        { "hebrew2", -3 }, { "thaiAlpha", -4 }, { "hindiAlpha1", -5 }, { "hindiAlpha", -6 }, { "thaiNum", -7 },
        { "hindiNum", -8 }, { "circleNumWdBlack", -9 }, { "arabic", PD_NUM_DECIMAL }
    };
    /* the alphabets: the Arabic one, the Arabic in its abjad order, Hebrew, Thai, Hindi's consonants and vowels */
    static const char* const abc[6] = {
        "\xD8\xA3\xD8\xA8\xD8\xAA\xD8\xAB\xD8\xAC\xD8\xAD\xD8\xAE\xD8\xAF\xD8\xB0\xD8\xB1\xD8\xB2\xD8\xB3\xD8\xB4\xD8\xB5"
        "\xD8\xB6\xD8\xB7\xD8\xB8\xD8\xB9\xD8\xBA\xD9\x81\xD9\x82\xD9\x83\xD9\x84\xD9\x85\xD9\x86\xD9\x87\xD9\x88\xD9\x8A",
        "\xD8\xA3\xD8\xA8\xD8\xAC\xD8\xAF\xD9\x87\xD9\x88\xD8\xB2\xD8\xAD\xD8\xB7\xD9\x8A\xD9\x83\xD9\x84\xD9\x85\xD9\x86"
        "\xD8\xB3\xD8\xB9\xD9\x81\xD8\xB5\xD9\x82\xD8\xB1\xD8\xB4\xD8\xAA\xD8\xAB\xD8\xAE\xD8\xB0\xD8\xB6\xD8\xB8\xD8\xBA",
        "\xD7\x90\xD7\x91\xD7\x92\xD7\x93\xD7\x94\xD7\x95\xD7\x96\xD7\x97\xD7\x98\xD7\x99\xD7\x9B\xD7\x9C\xD7\x9E\xD7\xA0"
        "\xD7\xA1\xD7\xA2\xD7\xA4\xD7\xA6\xD7\xA7\xD7\xA8\xD7\xA9\xD7\xAA",
        "\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84\xE0\xB8\x87\xE0\xB8\x88\xE0\xB8\x89\xE0\xB8\x8A\xE0\xB8\x8B\xE0\xB8\x8C"
        "\xE0\xB8\x8D\xE0\xB8\x8E\xE0\xB8\x8F\xE0\xB8\x90\xE0\xB8\x91\xE0\xB8\x92\xE0\xB8\x93\xE0\xB8\x94\xE0\xB8\x95"
        "\xE0\xB8\x96\xE0\xB8\x97\xE0\xB8\x98\xE0\xB8\x99\xE0\xB8\x9A\xE0\xB8\x9B\xE0\xB8\x9C\xE0\xB8\x9D\xE0\xB8\x9E"
        "\xE0\xB8\x9F\xE0\xB8\xA0\xE0\xB8\xA1\xE0\xB8\xA2\xE0\xB8\xA3\xE0\xB8\xA5\xE0\xB8\xA7\xE0\xB8\xA8\xE0\xB8\xA9"
        "\xE0\xB8\xAA\xE0\xB8\xAB\xE0\xB8\xAC\xE0\xB8\xAD\xE0\xB8\xAE",
        "\xE0\xA4\x95\xE0\xA4\x96\xE0\xA4\x97\xE0\xA4\x98\xE0\xA4\x99\xE0\xA4\x9A\xE0\xA4\x9B\xE0\xA4\x9C\xE0\xA4\x9D"
        "\xE0\xA4\x9E\xE0\xA4\x9F\xE0\xA4\xA0\xE0\xA4\xA1\xE0\xA4\xA2\xE0\xA4\xA3\xE0\xA4\xA4\xE0\xA4\xA5\xE0\xA4\xA6"
        "\xE0\xA4\xA7\xE0\xA4\xA8\xE0\xA4\xAA\xE0\xA4\xAB\xE0\xA4\xAC\xE0\xA4\xAD\xE0\xA4\xAE\xE0\xA4\xAF\xE0\xA4\xB0"
        "\xE0\xA4\xB2\xE0\xA4\xB5\xE0\xA4\xB6\xE0\xA4\xB7\xE0\xA4\xB8\xE0\xA4\xB9",
        "\xE0\xA4\x85\xE0\xA4\x86\xE0\xA4\x87\xE0\xA4\x88\xE0\xA4\x89\xE0\xA4\x8A\xE0\xA4\x8B\xE0\xA4\x8F\xE0\xA4\x90"
        "\xE0\xA4\x93\xE0\xA4\x94"
    };
    char num[48];
    const char* suf, *pre = "";
    size_t k;
    int fmt = PD_NUM_DECIMAL;

    for (k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        if (!strncmp(s, kinds[k].name, strlen(kinds[k].name))) {
            fmt = kinds[k].fmt;
            break;
        }
    }

    if (fmt >= 0) {
        pd_doc_format_number(v, fmt, num, sizeof(num));
    } else if (fmt >= -6) {     /* a letter of an alphabet, each the same length; decimal past its end */
        const char* a = abc[-fmt - 1];
        size_t w = fmt >= -3 ? 2 : 3, len = strlen(a) / w;

        if (v >= 1 && (size_t)v <= len) {
            snprintf(num, sizeof(num), "%.*s", (int)w, a + (size_t)(v - 1) * w);
        } else {
            snprintf(num, sizeof(num), "%d", v);
        }
    } else if (fmt == -9 && v >= 1 && v <= 10) {    /* white on a black circle: U+2776.. */
        snprintf(num, sizeof(num), "\xE2\x9D%c", (char)(0xB6 + v - 1));
    } else if (fmt == -9) {
        snprintf(num, sizeof(num), "%d", v);
    } else {    /* Thai or Devanagari digits */
        char dec[16];
        size_t q = 0, i;

        snprintf(dec, sizeof(dec), "%d", v);

        for (i = 0; dec[i] && q + 4 < sizeof(num); i++) {
            int dg = dec[i] - '0';

            if (dg < 0 || dg > 9) {
                num[q++] = dec[i];
            } else if (fmt == -7) {     /* U+0E50.. */
                num[q++] = (char)0xE0;
                num[q++] = (char)0xB9;
                num[q++] = (char)(0x90 + dg);
            } else {    /* U+0966.. */
                num[q++] = (char)0xE0;
                num[q++] = (char)0xA5;
                num[q++] = (char)(0xA6 + dg);
            }
        }

        num[q] = '\0';
    }

    /* what is round it */
    suf = strstr(s, "ParenBoth") ? ")" : strstr(s, "ParenR") ? ")" : strstr(s, "DbPeriod") ? "\xEF\xBC\x8E" :
          strstr(s, "Period") ? "." : strstr(s, "Minus") ? "-" : "";

    if (strstr(s, "ParenBoth")) {
        pre = "(";
    }

    snprintf(out, cap, "%s%s%s", pre, num, suf);
}

static void put_paragraph(pd_buf* o, tctx* T, const xdoc* d, int p, autonum* an) {
    static const char* const types[4] = { "buNone", "buAutoNum", "buChar", "buBlip" };
    static const char* const clrs[2] = { "buClr", "buClrTx" }, *const szs[3] = { "buSzPct", "buSzPts", "buSzTx" };
    static const char* const faces[2] = { "buFont", "buFontTx" };
    int ppr = xd_kid(d, p, "pPr"), k, first_rpr = -1, pct, sp, any = 0, rtl, which, lvl;
    char v[64], bu[64] = "", bu_face[64] = "";
    xref r;
    long long marl, ind;
    long bu_colour = -1, bu_sz = 0;
    double bu_scale = 0, line;
    const char* blip = NULL;

    for (k = d->v[p].kid; k >= 0; k = d->v[k].next) {
        if (!strcmp(d->v[k].name, "r") && first_rpr < 0) {
            first_rpr = xd_kid(d, k, "rPr");
            any = 1;
        } else if (!strcmp(d->v[k].name, "m")) {
            any = 1;    /* an equation */
        }
    }

    if (!any) {
        first_rpr = xd_kid(d, p, "endParaRPr");
    }

    /* a line of its text, in twips: PowerPoint's 1.2 times its size */
    line = (rpr_attr(T, d, first_rpr, "sz", v, sizeof(v)) ? atof(v) : T->def_sz > 0 ? T->def_sz : 1800) * T->scale *
           1.2 / 5;
    rtl = ppr_attr(T, d, ppr, "rtl", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));
    pb_puts(o, rtl ? "<w:p><w:pPr><w:bidi/>" : "<w:p><w:pPr>");    /* right to left: its margins from the right */
    /* line spacing and the space around (a percentage: of a line); autofit's line spacing reduction takes from
       the space around too */
    pb_puts(o, "<w:spacing");
    sp = spacing_of(ppr_kid(T, d, ppr, "spcBef"), &pct);

    if (sp >= 0) {
        pb_printf(o, " w:before=\"%d\"", (int)((pct ? sp * line / 240 : sp) * T->spacing + 0.5));
    }

    sp = spacing_of(ppr_kid(T, d, ppr, "spcAft"), &pct);
    pb_printf(o, " w:after=\"%d\"", sp >= 0 ? (int)((pct ? sp * line / 240 : sp) * T->spacing + 0.5) : 0);
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

    if (ppr_attr(T, d, ppr, "algn", v, sizeof(v))) {    /* (DrawingML's sides, Word's from the text's start) */
        const char* jc = !strcmp(v, "ctr") ? "center" : !strcmp(v, "r") ? (rtl ? "left" : "right") :
                         !strcmp(v, "just") || !strcmp(v, "dist") ? "both" : rtl ? "right" : "left";

        pb_printf(o, "<w:jc w:val=\"%s\"/>", jc);
    }

    /* the paragraph mark's size: an empty line as tall as its text would be */
    put_rpr(o, T, d, first_rpr);
    pb_puts(o, "</w:pPr>");

    /* its bullet: as text before it. A number counts on from the last paragraph at its level numbered the same way
       (from startAt, or 1); a paragraph less deep starts the deeper levels' numbers again, and one not numbered
       its own level's; an empty one is passed over */
    lvl = ppr >= 0 ? (int)xd_int(d, ppr, "lvl", 0) : 0;
    lvl = lvl < 0 ? 0 : lvl > 8 ? 8 : lvl;
    r = ppr_first(T, d, ppr, types, 4, &which);

    if (any) {
        for (k = lvl + 1; k < 9; k++) {
            an->n[k] = 0;
        }

        if (which == 1) {
            char sch[32] = "arabicPeriod";

            xd_attr(r.d, r.n, "type", sch, sizeof(sch));

            if (an->n[lvl] > 0 && !strcmp(an->scheme[lvl], sch)) {
                an->n[lvl]++;
            } else {
                an->n[lvl] = (int)xd_int(r.d, r.n, "startAt", 1);
                snprintf(an->scheme[lvl], sizeof(an->scheme[0]), "%s", sch);
            }

            autonum_label(sch, an->n[lvl], bu, sizeof(bu));
        } else {
            an->n[lvl] = 0;
        }
    }

    if (any && which >= 1) {
        xref f = ppr_first(T, d, ppr, faces, 2, &k);

        if (k == 0) {
            xd_attr(f.d, f.n, "typeface", bu_face, sizeof(bu_face));
        }

        if (which == 2) {
            xd_attr(r.d, r.n, "char", v, sizeof(v));
            snprintf(bu, sizeof(bu), "%.12s", symbol_bullet(v, bu_face));

            if (strcmp(bu, v) || !strncmp(bu_face, "Wingdings", 9) || !strcmp(bu_face, "Symbol") ||
                    !strcmp(bu_face, "Webdings") || !strcmp(bu_face, "Marlett")) {
                bu_face[0] = '\0';     /* a symbol font's character made Unicode: in the text's face */
            }
        } else if (which == 3) {   /* a picture: drawn as big as the text (a square where there is none) */
            int bl = xd_kid(r.d, r.n, "blip");

            for (k = 0; k < 3 && !blip; k++) {
                if (T->parts[k] && &T->parts[k]->x == r.d && xd_attr(r.d, bl, "r:embed", v, sizeof(v))) {
                    blip = media_id(T->P, T->parts[k], v);
                }
            }

            snprintf(bu, sizeof(bu), "%s", blip ? " " : "\xE2\x96\xA0");
        }

        if (bu[0]) {
            xref c = ppr_first(T, d, ppr, clrs, 2, &k), z;

            bu_colour = k == 0 && c.d->v[c.n].kid >= 0 ? colour_of(T->P, c.d, c.d->v[c.n].kid) : -1;
            z = ppr_first(T, d, ppr, szs, 3, &k);
            bu_scale = k == 0 ? xd_int(z.d, z.n, "val", 100000) / 100000.0 : 0;
            bu_sz = k == 1 ? (long)xd_int(z.d, z.n, "val", 0) : 0;
        }
    }

    if (bu[0]) {
        tctx B = *T;

        B.bullet = 1;
        B.bu_colour = bu_colour;
        B.bu_scale = bu_scale;
        B.bu_sz = bu_sz;
        B.bu_face = bu_face[0] ? bu_face : NULL;
        pb_puts(o, "<w:r>");
        put_rpr(o, &B, d, first_rpr);

        if (blip) {     /* a picture as tall as the text, square */
            long long e = (long long)((bu_sz > 0 ? bu_sz : (rpr_attr(T, d, first_rpr, "sz", v, sizeof(v)) ? atof(v) :
                                       T->def_sz > 0 ? T->def_sz : 1800) * (bu_scale > 0 ? bu_scale : 1)) *
                                      T->scale * 127 + 0.5);

            pb_printf(o, "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%lld\" "
                      "cy=\"%lld\"/><wp:docPr id=\"%d\" name=\"Bullet\"/><a:graphic><a:graphicData uri=\"http://schemas."
                      "openxmlformats.org/drawingml/2006/picture\"><pic:pic><pic:nvPicPr><pic:cNvPr id=\"0\" name=\"Bullet\"/>"
                      "<pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"%s\"/><a:stretch><a:fillRect/>"
                      "</a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/>"
                      "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
                      "</a:graphic></wp:inline></w:drawing></w:r><w:r>", e, e, 3700000 + ++T->P->docpr, blip, e, e);
            put_rpr(o, T, d, first_rpr);
            pb_puts(o, "<w:tab/></w:r>");
        } else {
            pb_puts(o, "<w:t xml:space=\"preserve\">");
            put_esc(o, bu);
            pb_puts(o, "</w:t></w:r><w:r>");
            put_rpr(o, T, d, first_rpr);
            pb_puts(o, "<w:tab/></w:r>");
        }
    }

    for (k = d->v[p].kid; k >= 0; k = d->v[k].next) {
        const char* n = d->v[k].name;

        if (!strcmp(n, "r") || !strcmp(n, "fld")) {
            int t = xd_kid(d, k, "t"), hl = xd_path(d, k, "rPr/hlinkClick"), q;
            char ft[32];
            const prel* link = NULL;

            for (q = 0; q < 3 && hl >= 0 && !link && xd_attr(d, hl, "r:id", ft, sizeof(ft)); q++) {
                const ppart* pp = T->parts[q];
                int i;

                for (i = 0; pp && &pp->x == d && i < pp->nrels; i++) {
                    if (!strcmp(pp->rels[i].id, ft) && pp->rels[i].external) {
                        link = &pp->rels[i];
                    }
                }
            }

            if (link) {     /* a link to a page or an address: the .docx's own */
                pb_printf(&T->P->rels, "<Relationship Id=\"rIdPh%d\" Type=\"http://schemas.openxmlformats.org/"
                          "officeDocument/2006/relationships/hyperlink\" Target=\"", ++T->P->nlinks);
                put_esc(&T->P->rels, link->target);
                pb_printf(&T->P->rels, "\" TargetMode=\"External\"/>");
                pb_printf(o, "<w:hyperlink r:id=\"rIdPh%d\">", T->P->nlinks);
            }

            pb_puts(o, "<w:r>");
            put_rpr(o, T, d, xd_kid(d, k, "rPr"));
            pb_puts(o, "<w:t xml:space=\"preserve\">");

            if (!strcmp(n, "fld") && xd_attr(d, k, "type", ft, sizeof(ft)) && !strcmp(ft, "slidenum")) {
                pb_printf(o, "%d", T->slide_no);    /* the slide's number, whatever it said when saved */
            } else if (t >= 0) {
                xd_inner(o, d, t);  /* already escaped */
            }

            pb_puts(o, link ? "</w:t></w:r></w:hyperlink>" : "</w:t></w:r>");
        } else if (!strcmp(n, "m")) {   /* an equation (a14:m): its Office Math, as Word has it in a paragraph */
            xd_inner(o, d, k);
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
    const xdoc* dm;             /* a SmartArt's data model, while its drawing is put, */
    const xdoc* dmc;            /* and its colours */
    pd_buf* o;
    pd_buf* charts;             /* the slide's charts: drawings of their own, over its canvas */
    const xdoc* th;             /* the slide's theme when it is not the document's (another master's), else NULL */
    long long gx, gy;           /* while a group's shapes are put: where its children's (0, 0) is on the slide, */
    double gsx, gsy;            /* and how much bigger it draws them (0: not in a group) */
    int depth;                  /* groups the shape being put is in */
    int isph;                   /* it is a placeholder: its type and index as the part says them ("" none) */
    char ph_type[32], ph_idx[24];
} conv;

/* What a writer needs to put a slide's shape back where it came from, which a Word canvas has no place for: the
   part it is from (the slide, its layout, its master) and the placeholder it is, as an extension of its non-visual
   properties (Word passes over an extension it does not know; the .docx writer writes it back as it is). */
#define PD_PPTX_EXT "<a:extLst><a:ext uri=\"{5E1E1C10-9A4B-4C35-9F2D-50415241444F}\"><pd:sp xmlns:pd=\"urn:parade:pptx\""

static void put_marker(conv* C, int from, int bg) {
    static const char* const part[3] = { "slide", "layout", "master" };

    pb_printf(C->o, PD_PPTX_EXT " from=\"%s\"", part[from > 2 ? 2 : from < 0 ? 0 : from]);

    if (bg) {
        pb_puts(C->o, " bg=\"1\"");
    } else if (C->isph) {
        pb_puts(C->o, " ph=\"");
        put_esc(C->o, C->ph_type);
        pb_putc(C->o, '"');

        if (C->ph_idx[0]) {
            pb_puts(C->o, " idx=\"");
            put_esc(C->o, C->ph_idx);
            pb_putc(C->o, '"');
        }
    }

    pb_puts(C->o, "/></a:ext></a:extLst>");
}

/* the part the shapes being put are from: 0 the slide, 1 its layout, 2 its master */
static int from_of(const conv* C) {
    return C->id_base >= 2000000 ? 2 : C->id_base >= 1000000 ? 1 : 0;
}

/* a top-level shape that is not the slide's own, or is a placeholder, says so */
static int marked(const conv* C) {
    return C->depth == 0 && (from_of(C) || C->isph);
}

/* a SmartArt drawing shape's text in the data model (its drawing a copy PowerPoint may not have redrawn): the
   point its presentation point (the shape's modelId) presents */
static int dm_text(const xdoc* m, const char* model) {
    char v[64], src[64] = "";
    int k;

    for (k = 0; k < m->nv && !src[0]; k++) {
        if (!strcmp(m->v[k].name, "cxn") && xd_attr(m, k, "type", v, sizeof(v)) && !strcmp(v, "presOf") &&
                xd_attr(m, k, "destId", v, sizeof(v)) && !strcmp(v, model)) {
            xd_attr(m, k, "srcId", src, sizeof(src));
        }
    }

    for (k = 0; src[0] && k < m->nv; k++) {
        if (!strcmp(m->v[k].name, "pt") && xd_attr(m, k, "modelId", v, sizeof(v)) && !strcmp(v, src)) {
            return xd_kid(m, k, "t");
        }
    }

    return -1;
}

/* the text colour a SmartArt drawing's shape takes from its colours: its presentation point's style label's
   txFillClrLst, -1 none */
static long dm_text_colour(const pptx* P, const xdoc* m, const xdoc* cs, const xdoc* d, int sp) {
    char id[64], v[64], lbl[64] = "";
    int k, l;

    if (!xd_attr(d, sp, "modelId", id, sizeof(id))) {
        return -1;
    }

    for (k = 0; k < m->nv && !lbl[0]; k++) {
        if (!strcmp(m->v[k].name, "pt") && xd_attr(m, k, "modelId", v, sizeof(v)) && !strcmp(v, id)) {
            xd_attr(m, xd_kid(m, k, "prSet"), "presStyleLbl", lbl, sizeof(lbl));
        }
    }

    for (k = 0; lbl[0] && k < cs->nv; k++) {
        if (!strcmp(cs->v[k].name, "styleLbl") && xd_attr(cs, k, "name", v, sizeof(v)) && !strcmp(v, lbl)) {
            l = xd_kid(cs, k, "txFillClrLst");
            return l >= 0 && cs->v[l].kid >= 0 ? colour_of(P, cs, cs->v[l].kid) : -1;
        }
    }

    return -1;
}

/* whether two text bodies have the same text */
static int same_text(const xdoc* a, int ta, const xdoc* b, int tb) {
    pd_buf x, y;
    int k, same;

    memset(&x, 0, sizeof(x));
    memset(&y, 0, sizeof(y));

    for (k = ta; k < a->nv && a->v[k].a < a->v[ta].b; k++) {
        if (!strcmp(a->v[k].name, "t") && k != ta) {
            xd_inner(&x, a, k);
        } else if (!strcmp(a->v[k].name, "p")) {
            pb_putc(&x, '\n');
        }
    }

    for (k = tb; k < b->nv && b->v[k].a < b->v[tb].b; k++) {
        if (!strcmp(b->v[k].name, "t") && k != tb) {
            xd_inner(&y, b, k);
        } else if (!strcmp(b->v[k].name, "p")) {
            pb_putc(&y, '\n');
        }
    }

    same = x.n == y.n && (!x.n || !memcmp(x.p, y.p, x.n));
    pb_free(&x);
    pb_free(&y);
    return same;
}

/* an element under another name: its attributes and inside kept, its colours the slide's */
static void put_renamed(const pptx* P, pd_buf* o, const xdoc* d, int n, const char* name) {
    pb_printf(o, "<%s", name);

    if (d->v[n].alen) {
        pb_putc(o, ' ');
        pb_put(o, d->v[n].attrs, d->v[n].alen);
    }

    if (d->v[n].ib > d->v[n].ia) {
        pb_putc(o, '>');
        put_mapped(P, o, d->s + d->v[n].ia, d->v[n].ib - d->v[n].ia);
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

/* a shape's style (p:style: its theme line, fill, effect and font): its own, else its layout placeholder's, else its
   master's */
static xref shape_style(const conv* C, const xdoc* d, int sp, int lsp, int msp) {
    xref r = { d, xd_kid(d, sp, "style") };

    if (r.n < 0 && lsp >= 0 && (r.n = xd_kid(&C->layout->x, lsp, "style")) >= 0) {
        r.d = &C->layout->x;
    } else if (r.n < 0 && msp >= 0 && (r.n = xd_kid(&C->master->x, msp, "style")) >= 0) {
        r.d = &C->master->x;
    }

    return r;
}

/* a text body as a text box's content: its paragraphs, each with what its level takes from the list styles */
static void put_txbody(conv* C, ppart* pt, int sp, int tx, int kind, int lsp, int msp) {
    const xdoc* d = &pt->x;
    const xdoc* ds[6];
    int ns[6], nn = 0, p, ts;
    autonum an;
    tctx T;
    int bp = xd_kid(d, tx, "bodyPr"), na = bp >= 0 ? xd_kid(d, bp, "normAutofit") : -1, fr;

    memset(&T, 0, sizeof(T));
    T.P = C->P;
    T.parts[0] = pt;
    T.parts[1] = C->layout;
    T.parts[2] = C->master;
    T.slide_no = C->slide_no;
    T.title = kind == 1;
    T.scale = na >= 0 ? xd_int(d, na, "fontScale", 100000) / 100000.0 : 1;
    T.spacing = na >= 0 ? 1 - xd_int(d, na, "lnSpcReduction", 0) / 100000.0 : 1;
    {
        xref sty = shape_style(C, d, sp, lsp, msp);

        fr = sty.n >= 0 ? xd_kid(sty.d, sty.n, "fontRef") : -1;
        T.font_colour = fr >= 0 && sty.d->v[fr].kid >= 0 ? colour_of(C->P, sty.d, sty.d->v[fr].kid) : -1;
    }
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

    if (ts >= 0 && (kind != 0 || lsp >= 0 || msp >= 0)) {  /* a text box not a placeholder: the presentation's */
        ds[nn] = &C->master->x;
        ns[nn++] = xd_kid(&C->master->x, ts, kind == 1 ? "titleStyle" : kind == 2 ? "bodyStyle" : "otherStyle");
    }

    if (C->pres && C->pres_style >= 0) {
        ds[nn] = C->pres;
        ns[nn++] = C->pres_style;
    }

    if (C->dm && C->dmc) {  /* its text the colour its colours give its style label (PowerPoint's, over fontRef) */
        long c = dm_text_colour(C->P, C->dm, C->dmc, d, sp);

        if (c >= 0) {
            T.font_colour = c;
        }
    }

    if (C->dm) {
        char id[64];
        int t;

        if (xd_attr(d, sp, "modelId", id, sizeof(id)) && (t = dm_text(C->dm, id)) >= 0 &&
                !same_text(d, tx, C->dm, t)) {
            int k;

            for (k = tx; k < d->nv && d->v[k].a < d->v[tx].b; k++) {   /* its runs' size, the drawing's */
                if (!strcmp(d->v[k].name, "rPr") && xd_int(d, k, "sz", 0) > 0) {
                    T.def_sz = (long)xd_int(d, k, "sz", 0);
                    break;
                }
            }

            d = C->dm;      /* the text it has now */
            tx = t;
        }
    }

    memset(&an, 0, sizeof(an));
    pb_puts(C->o, "<wps:txbx><w:txbxContent>");

    for (p = d->v[tx].kid; p >= 0; p = d->v[p].next) {
        if (!strcmp(d->v[p].name, "p")) {
            int ppr = xd_kid(d, p, "pPr");

            lvl_chain(&T, ds, ns, nn, ppr >= 0 ? (int)xd_int(d, ppr, "lvl", 0) : 0);
            put_paragraph(C->o, &T, d, p, &an);
        }
    }

    pb_puts(C->o, "</w:txbxContent></wps:txbx>");
}

/* a shape's style's reference (Q 1 its fill, 2 its line, 3 its effects) when the slide's theme is not the document's:
   the theme's fill, line or effects it names, in the reference's colour -- what the .docx's theme (the first master's)
   would give it is not what this master's does */
static void put_style_ref(conv* C, const xdoc* d, int sp, int lsp, int msp, int q) {
    static const char* const refs[4] = { "", "fillRef", "lnRef", "effectRef" };
    static const char* const lists[4] = { "", "fillStyleLst", "lnStyleLst", "effectStyleLst" };
    xref sty = shape_style(C, d, sp, lsp, msp);
    int r = sty.n >= 0 ? xd_kid(sty.d, sty.n, refs[q]) : -1, lst, e;
    long idx = r >= 0 ? (long)xd_int(sty.d, r, "idx", 0) : 0;

    if (idx <= 0) {
        return;
    }

    lst = xd_find(C->th, q == 1 && idx > 1000 ? "bgFillStyleLst" : lists[q]);
    idx = q == 1 && idx > 1000 ? idx - 1000 : idx;

    for (e = lst >= 0 ? C->th->v[lst].kid : -1; e >= 0 && idx > 1; e = C->th->v[e].next) {
        idx--;
    }

    if (e >= 0 && q == 3) {
        e = xd_kid(C->th, e, "effectLst");
    }

    if (e >= 0) {
        C->P->ph = sty.d->v[r].kid >= 0 ? colour_of(C->P, sty.d, sty.d->v[r].kid) : -1;
        xd_raw_mapped(C->P, C->o, C->th, e);
        C->P->ph = -1;
    }
}

/* one of a set of spPr children that stand for one another (the fills; the geometries) under N, -1 none */
static int sppr_one(const xdoc* d, int n, const char* const* names, int nn) {
    int k, q;

    for (k = n >= 0 ? d->v[n].kid : -1; k >= 0; k = d->v[k].next) {
        for (q = 0; q < nn; q++) {
            if (!strcmp(d->v[k].name, names[q])) {
                return k;
            }
        }
    }

    return -1;
}

/* a text body's bodyPr: its own attributes, else its placeholders' */
static void put_bodypr(conv* C, const xdoc* d, int tx, int lsp, int msp) {
    static const char* names[] = { "lIns", "tIns", "rIns", "bIns", "anchor", "wrap", "vert", "anchorCtr", "rot",
                                   "upright", "numCol", "spcCol", "rtlCol", "vertOverflow", "horzOverflow"
                                 };
    static const char* const fits[3] = { "normAutofit", "spAutoFit", "noAutofit" }, *const warps[1] = { "prstTxWarp" };
    static const char* const* const sets[2] = { warps, fits };
    static const int nset[2] = { 1, 3 };
    int q;
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

    pb_putc(C->o, '>');

    for (q = 0; q < 2; q++) {   /* its warp and how it fits: its own, else its placeholders' */
        int e = sppr_one(d, bp, sets[q], nset[q]);
        const xdoc* from = d;

        if (e < 0 && lb >= 0 && (e = sppr_one(&C->layout->x, lb, sets[q], nset[q])) >= 0) {
            from = &C->layout->x;
        } else if (e < 0 && mb >= 0 && (e = sppr_one(&C->master->x, mb, sets[q], nset[q])) >= 0) {
            from = &C->master->x;
        }

        if (e >= 0) {
            xd_raw(C->o, from, e);
        }
    }

    pb_puts(C->o, "</wps:bodyPr>");
}

static int has_text(const xdoc* d, int tx) {
    int p, r;

    for (p = tx >= 0 ? d->v[tx].kid : -1; p >= 0; p = d->v[p].next) {
        for (r = d->v[p].kid; r >= 0; r = d->v[r].next) {
            if (!strcmp(d->v[r].name, "r") || !strcmp(d->v[r].name, "fld") || !strcmp(d->v[r].name, "m")) {
                return 1;
            }
        }
    }

    return 0;
}

/* a shape's spPr: its own, with where it is, its geometry, fill, outline and effects from its placeholders (its
   layout's, else its master's) when it has not its own */
static int put_sppr(conv* C, ppart* pt, int sp, int lsp, int msp, const char* el, int widen) {
    static const char* const fills[6] = { "noFill", "solidFill", "gradFill", "blipFill", "pattFill", "grpFill" };
    static const char* const geoms[2] = { "prstGeom", "custGeom" }, *const lns[1] = { "ln" };
    static const char* const effs[2] = { "effectLst", "effectDag" };
    static const char* const* const sets[4] = { geoms, fills, lns, effs };
    static const int nset[4] = { 2, 6, 1, 2 };
    const xdoc* d = &pt->x;
    int spr = xd_kid(d, sp, "spPr"), xf = xd_kid(d, spr, "xfrm"), k, q;
    int lspr = lsp >= 0 ? xd_kid(&C->layout->x, lsp, "spPr") : -1, mspr = msp >= 0 ? xd_kid(&C->master->x, msp, "spPr") : -1;
    xref pxf = { d, xf };

    if (xf < 0 && lsp >= 0 && (k = xd_kid(&C->layout->x, lspr, "xfrm")) >= 0) {
        pxf.d = &C->layout->x;
        pxf.n = k;
    } else if (xf < 0 && msp >= 0 && (k = xd_kid(&C->master->x, mspr, "xfrm")) >= 0) {
        pxf.d = &C->master->x;
        pxf.n = k;
    }

    if (pxf.n < 0) {
        return 0;   /* nowhere */
    }

    pb_printf(C->o, "<%s>", el);

    if (widen) {    /* text that is not to wrap: a box wide enough, on the side its alignment keeps it */
        int off = xd_kid(pxf.d, pxf.n, "off"), ext = xd_kid(pxf.d, pxf.n, "ext");
        long long x = xd_int(pxf.d, off, "x", 0), y = xd_int(pxf.d, off, "y", 0), cx = xd_int(pxf.d, ext, "cx", 0);
        long long cy = xd_int(pxf.d, ext, "cy", 0), more = C->P->sw;

        pb_puts(C->o, "<a:xfrm");

        if (pxf.d->v[pxf.n].alen) {
            pb_putc(C->o, ' ');
            pb_put(C->o, pxf.d->v[pxf.n].attrs, pxf.d->v[pxf.n].alen);
        }

        pb_printf(C->o, "><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>",
                  widen == 2 ? x - more / 2 : widen == 3 ? x - more : x, y, cx + more, cy);
    } else {
        xd_raw(C->o, pxf.d, pxf.n);
    }

    /* the geometry, the fill, the outline, the effects, in that order: its own, else its placeholders' */
    for (q = 0; q < 4; q++) {
        const ppart* from = pt;
        int e = sppr_one(d, spr, sets[q], nset[q]);

        if (e < 0 && lspr >= 0 && (e = sppr_one(&C->layout->x, lspr, sets[q], nset[q])) >= 0) {
            from = C->layout;
        } else if (e < 0 && mspr >= 0 && (e = sppr_one(&C->master->x, mspr, sets[q], nset[q])) >= 0) {
            from = C->master;
        }

        if (e < 0 && q > 0 && C->th) {  /* another master's theme: what the style refers to in it, made the shape's */
            put_style_ref(C, d, sp, lsp, msp, q);
            continue;
        }

        if (e < 0 && q == 0) {
            pb_puts(C->o, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>");
        } else if (e >= 0 && !strcmp(from->x.v[e].name, "blipFill")) {
            pb_puts(C->o, "<a:blipFill>");
            put_with_media(C->o, C, from, from->x.s + from->x.v[e].ia, from->x.v[e].ib - from->x.v[e].ia);
            pb_puts(C->o, "</a:blipFill>");
        } else if (e >= 0) {
            xd_raw_mapped(C->P, C->o, &from->x, e);
        }
    }

    for (k = spr >= 0 ? d->v[spr].kid : -1; k >= 0; k = d->v[k].next) {    /* the rest of its own (3-D) */
        int known = !strcmp(d->v[k].name, "xfrm");

        for (q = 0; q < 4 && !known; q++) {
            int i;

            for (i = 0; i < nset[q]; i++) {
                known |= !strcmp(d->v[k].name, sets[q][i]);
            }
        }

        if (!known) {
            xd_raw_mapped(C->P, C->o, d, k);
        }
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

    if (marked(C)) {
        pb_puts(C->o, "\">");
        put_marker(C, from_of(C), 0);
        pb_printf(C->o, "</%s>", el);
    } else {
        pb_puts(C->o, "\"/>");
    }
}

/* a cell's edge: its width (EMU) and colour; set 1 a line, -1 none, 0 not said */
typedef struct {
    long long w;
    long c;
    int set;
} tedge;

/* what a cell takes from its table's style: its fill, its text's colour and whether it is bold (-1: not said), its
   edges (left, top, right, bottom) */
typedef struct {
    long fill, text;
    int bold;
    tedge e[4];
} tcell;

/* an edge from a line (a:ln, a:lnL...: its width and fill) or a reference to the theme's (a:lnRef: its colour) */
static void edge_of(const pptx* P, const xdoc* d, int ln, tedge* e) {
    int k;

    if (ln < 0) {
        return;
    }

    if (!strcmp(d->v[ln].name, "lnRef")) {
        k = (int)xd_int(d, ln, "idx", 1);
        e->w = (long long)P->line_w[k >= 1 && k <= 3 ? k - 1 : 0];
        e->c = d->v[ln].kid >= 0 ? colour_of(P, d, d->v[ln].kid) : 0;
        e->set = k > 0 ? 1 : -1;
        return;
    }

    if (xd_kid(d, ln, "noFill") >= 0) {
        e->set = -1;
        return;
    }

    if (xd_kid(d, ln, "solidFill") < 0 && !d->v[ln].alen) {
        return;     /* says nothing */
    }

    e->set = 1;
    e->w = xd_int(d, ln, "w", 12700);
    e->c = fill_colour(P, d, ln);
    e->c = e->c >= 0 ? e->c : 0;
}

/* a table style's part (wholeTbl, band1H, firstRow, ...) over a cell: the fill its cells take, their text's colour,
   whether it is bold, and the edges its tcBdr gives the cell. REGION is what the part covers -- 0 the table, 1 a row,
   2 a column -- for which of its edges are the cell's (the outer ones where the cell is at the region's side, the
   inside ones between) */
static void tstyle_part(const pptx* P, int style, const char* part, int region, int top, int bottom, int left,
                        int right, tcell* tc) {
    static const char* const outer[4] = { "left", "top", "right", "bottom" };
    static const char* const inner[4] = { "insideV", "insideH", "insideV", "insideH" };
    const xdoc* d = P->tstyles;
    int pn = style >= 0 ? xd_kid(d, style, part) : -1, k, tx, f, bd;
    char v[16];

    if (pn < 0) {
        return;
    }

    f = xd_path(d, pn, "tcStyle/fill");

    if (f >= 0 && fill_colour(P, d, f) >= 0) {
        tc->fill = fill_colour(P, d, f);
    } else if ((f = xd_path(d, pn, "tcStyle/fillRef")) >= 0 && d->v[f].kid >= 0 && colour_of(P, d, d->v[f].kid) >= 0) {
        tc->fill = colour_of(P, d, d->v[f].kid);
    }

    tx = xd_kid(d, pn, "tcTxStyle");

    if (tx >= 0) {
        if (xd_attr(d, tx, "b", v, sizeof(v))) {
            tc->bold = !strcmp(v, "on");
        }

        for (k = d->v[tx].kid; k >= 0; k = d->v[k].next) {     /* its colour; a font reference's, else */
            long c = !strcmp(d->v[k].name, "fontRef") ? (d->v[k].kid >= 0 ? colour_of(P, d, d->v[k].kid) : -1) :
                     colour_of(P, d, k);

            if (c >= 0) {
                tc->text = c;
            }
        }
    }

    bd = xd_path(d, pn, "tcStyle/tcBdr");

    for (k = 0; bd >= 0 && k < 4; k++) {
        int at_side = k == 0 ? left : k == 1 ? top : k == 2 ? right : bottom, side;
        tedge e = { 0, 0, 0 };

        if (region == 1 && (k == 1 || k == 3)) {
            at_side = 1;    /* a row's top and bottom are its own */
        } else if (region == 2 && (k == 0 || k == 2)) {
            at_side = 1;    /* a column's sides */
        }

        side = xd_kid(d, bd, at_side ? outer[k] : inner[k]);

        if (side >= 0) {
            edge_of(P, d, xd_kid(d, side, "ln") >= 0 ? xd_kid(d, side, "ln") : xd_kid(d, side, "lnRef"), &e);
        }

        if (e.set) {
            tc->e[k] = e;
        }
    }
}

/* the table style a table names (tableStyleId), or the presentation's default one; -1 if the file has neither */
static int tstyle_of(const pptx* P, const xdoc* d, int tbl) {
    const xdoc* t = P->tstyles;
    int id = xd_path(d, tbl, "tblPr/tableStyleId"), lst, k;
    char want[64] = "", v[64];

    if (!t || (lst = xd_find(t, "tblStyleLst")) < 0) {
        return -1;
    }

    if (id >= 0) {
        snprintf(want, sizeof(want), "%.*s", (int)(d->v[id].ib - d->v[id].ia), d->s + d->v[id].ia);
    } else {
        xd_attr(t, lst, "def", want, sizeof(want));
    }

    for (k = t->v[lst].kid; k >= 0; k = t->v[k].next) {
        if (xd_attr(t, k, "styleId", v, sizeof(v)) && !strcmp(v, want)) {
            return k;
        }
    }

    return -1;
}


/* a table (a:tbl) as a text box holding a Word table, drawn with a plain style: a header row in the theme's
   first accent, banded rows */
static void put_table(conv* C, ppart* pt, int gf, int tbl) {
    const xdoc* d = &pt->x;
    int grid = xd_kid(d, tbl, "tblGrid"), tp = xd_kid(d, tbl, "tblPr"), k, r, row = 0;
    int first = (int)xd_int(d, tp, "firstRow", 0), band = (int)xd_int(d, tp, "bandRow", 0);
    int last = (int)xd_int(d, tp, "lastRow", 0), nrows = 0, style = tstyle_of(C->P, d, tbl), rr;
    int fcol = (int)xd_int(d, tp, "firstCol", 0), lcol = (int)xd_int(d, tp, "lastCol", 0);
    int bandc = (int)xd_int(d, tp, "bandCol", 0), ncols = 0;
    int xf = xd_kid(d, gf, "xfrm"), col;
    long long total = 0, colx[65], x0, y0, rowy = 0;
    uint32_t acc = C->P->theme[4];
    pd_buf fills, tb, *out = C->o;

    if (xf < 0) {
        return;
    }

    /* the cells' fills drawn as rectangles where the table is among the slide's shapes (a table's own shading would
       be drawn over every shape of the slide, those on the table too); the table, unshaded, over them */
    memset(&fills, 0, sizeof(fills));
    memset(&tb, 0, sizeof(tb));
    C->o = &tb;
    x0 = xd_int(d, xd_kid(d, xf, "off"), "x", 0);
    y0 = xd_int(d, xd_kid(d, xf, "off"), "y", 0);
    colx[0] = 0;
    col = 0;

    for (k = grid >= 0 ? d->v[grid].kid : -1; k >= 0 && col < 64; k = d->v[k].next) {
        colx[col + 1] = colx[col] + xd_int(d, k, "w", 0);
        col++;
    }

    ncols = col;

    pb_puts(C->o, "<wps:wsp>");
    put_nvpr(C, d, gf, "nvGraphicFramePr/cNvPr", "wps:cNvPr");
    pb_puts(C->o, "<wps:cNvSpPr txBox=\"1\"/><wps:spPr><a:xfrm>");
    xd_inner(C->o, d, xf);
    pb_puts(C->o, "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></wps:spPr>"
            "<wps:txbx><w:txbxContent><w:tbl><w:tblPr><w:tblLayout w:type=\"fixed\"/><w:tblBorders>"
            "<w:top w:val=\"nil\"/><w:left w:val=\"nil\"/><w:bottom w:val=\"nil\"/><w:right w:val=\"nil\"/>"
            "<w:insideH w:val=\"nil\"/><w:insideV w:val=\"nil\"/></w:tblBorders><w:tblCellMar><w:left w:w=\"144\" w:type=\"dxa\"/><w:right w:w=\"144\" "
            "w:type=\"dxa\"/></w:tblCellMar></w:tblPr><w:tblGrid>");

    for (k = grid >= 0 ? d->v[grid].kid : -1; k >= 0; k = d->v[k].next) {
        pb_printf(C->o, "<w:gridCol w:w=\"%lld\"/>", xd_int(d, k, "w", 0) / 635);
        total += xd_int(d, k, "w", 0);
    }

    pb_puts(C->o, "</w:tblGrid>");

    for (rr = d->v[tbl].kid; rr >= 0; rr = d->v[rr].next) {
        nrows += !strcmp(d->v[rr].name, "tr");
    }

    for (r = d->v[tbl].kid; r >= 0; r = d->v[r].next) {
        int c;

        if (strcmp(d->v[r].name, "tr")) {
            continue;
        }

        pb_printf(C->o, "<w:tr><w:trPr><w:trHeight w:val=\"%lld\"/></w:trPr>", xd_int(d, r, "h", 0) / 635);
        col = 0;

        for (c = d->v[r].kid; c >= 0; c = d->v[c].next) {
            int tc = xd_kid(d, c, "tcPr"), tx = xd_kid(d, c, "txBody"), p;
            autonum an;
            long fill = tc >= 0 ? fill_colour(C->P, d, tc) : -1;
            long long span = xd_int(d, c, "gridSpan", 1);
            tctx T;

            if (strcmp(d->v[c].name, "tc")) {
                continue;
            }

            if (xd_int(d, c, "hMerge", 0)) {
                col++;
                continue;
            }

            tcell st;

            memset(&st, 0, sizeof(st));
            st.fill = st.text = -1;
            st.bold = -1;

            if (style >= 0) {   /* the table's style, its parts in the order they win: the whole table's, the column
                                   and row bands', the last and first columns', the last and first rows' */
                int top = row == 0, bottom = row + (int)xd_int(d, c, "rowSpan", 1) >= nrows, left = col == 0;
                int right = col + (int)span >= ncols, hrow = first && row == 0, frow = last && row == nrows - 1;
                int fc = fcol && col == 0, lc = lcol && right;

                tstyle_part(C->P, style, "wholeTbl", 0, top, bottom, left, right, &st);

                if (bandc && !fc && !lc) {
                    tstyle_part(C->P, style, (col - fcol) % 2 == 0 ? "band1V" : "band2V", 2, top, bottom, left, right,
                                &st);
                }

                if (band && !hrow && !frow) {
                    tstyle_part(C->P, style, (row - first) % 2 == 0 ? "band1H" : "band2H", 1, top, bottom, left, right,
                                &st);
                }

                if (lc) {
                    tstyle_part(C->P, style, "lastCol", 2, top, bottom, left, right, &st);
                }

                if (fc) {
                    tstyle_part(C->P, style, "firstCol", 2, top, bottom, left, right, &st);
                }

                if (frow) {
                    tstyle_part(C->P, style, "lastRow", 1, top, bottom, left, right, &st);
                }

                if (hrow) {
                    tstyle_part(C->P, style, "firstRow", 1, top, bottom, left, right, &st);
                }

                if (fill < 0) {
                    fill = st.fill;
                }
            } else if (fill < 0 && first && row == 0) {     /* none the file has: a plain one */
                fill = (long)acc;
            } else if (fill < 0 && band) {
                double h, s, l;

                rgb_hsl(acc, &h, &s, &l);
                fill = (long)hsl_rgb(h, s, (row - first) % 2 ? 0.92 : 0.84);
            }

            {   /* its own edges over the style's */
                static const char* const own[4] = { "lnL", "lnT", "lnR", "lnB" };
                int q;

                for (q = 0; q < 4; q++) {
                    edge_of(C->P, d, tc >= 0 ? xd_kid(d, tc, own[q]) : -1, &st.e[q]);
                }
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

            if (st.e[0].set || st.e[1].set || st.e[2].set || st.e[3].set) {
                static const char* const wn[4] = { "left", "top", "right", "bottom" };
                static const int order[4] = { 1, 0, 3, 2 };     /* Word's: top, left, bottom, right */
                int q;

                pb_puts(C->o, "<w:tcBorders>");

                for (q = 0; q < 4; q++) {
                    const tedge* e = &st.e[order[q]];

                    if (e->set > 0) {
                        pb_printf(C->o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"%06lX\"/>", wn[order[q]],
                                  (int)((e->w * 8 + 6350) / 12700), (unsigned long)e->c & 0xFFFFFFul);
                    } else if (e->set < 0) {
                        pb_printf(C->o, "<w:%s w:val=\"nil\"/>", wn[order[q]]);
                    }
                }

                pb_puts(C->o, "</w:tcBorders>");
            }

            if (fill >= 0 && col < 64 && !xd_int(d, c, "vMerge", 0)) {   /* its fill: a rectangle under the table */
                long long cw = colx[col + span < 64 ? col + span : 64] - colx[col], ch = 0;
                int rs = (int)xd_int(d, c, "rowSpan", 1), q2;
                int rn;

                for (rn = r, q2 = 0; rn >= 0 && q2 < rs; rn = d->v[rn].next) {
                    if (!strcmp(d->v[rn].name, "tr")) {
                        ch += xd_int(d, rn, "h", 0);
                        q2++;
                    }
                }

                pb_printf(&fills, "<wps:wsp><wps:cNvPr id=\"%d\" name=\"Cell\"/><wps:cNvSpPr/><wps:spPr><a:xfrm><a:off "
                          "x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm><a:prstGeom prst=\"rect\">"
                          "<a:avLst/></a:prstGeom><a:solidFill><a:srgbClr val=\"%06lX\"/></a:solidFill><a:ln><a:noFill/>"
                          "</a:ln></wps:spPr><wps:bodyPr/></wps:wsp>", 3000000 + C->P->cells++, x0 + colx[col] + 6350,
                          y0 + rowy + 6350, cw > 12700 ? cw - 12700 : cw, ch > 12700 ? ch - 12700 : ch, (unsigned long)fill);
            }

            col += (int)span;

            pb_puts(C->o, "</w:tcPr>");
            memset(&T, 0, sizeof(T));
            memset(&an, 0, sizeof(an));
            T.P = C->P;
            T.parts[0] = pt;
            T.parts[2] = C->master;
            T.scale = T.spacing = 1;
            T.font_colour = style >= 0 ? st.text : first && row == 0 ? 0xFFFFFF : -1;
            T.force_bold = style >= 0 && st.bold == 1;

            for (p = tx >= 0 ? d->v[tx].kid : -1; p >= 0; p = d->v[p].next) {
                if (!strcmp(d->v[p].name, "p")) {
                    const xdoc* ds[2] = { d, C->master ? &C->master->x : d };
                    int ns[2] = { xd_kid(d, tx, "lstStyle"), C->master ? xd_path(&C->master->x,
                                  xd_find(&C->master->x, "txStyles"), "otherStyle") : -1
                                };

                    lvl_chain(&T, ds, ns, 2, 0);
                    put_paragraph(C->o, &T, d, p, &an);
                }
            }

            if (tx < 0 || !has_text(d, tx)) {
                pb_puts(C->o, "<w:p/>");
            }

            pb_puts(C->o, "</w:tc>");
        }

        pb_puts(C->o, "</w:tr>");
        rowy += xd_int(d, r, "h", 0);
        row++;
    }

    (void)total;
    pb_puts(C->o, "</w:tbl><w:p/></w:txbxContent></wps:txbx><wps:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\"/>"
            "</wps:wsp>");
    C->o = out;
    pb_put(C->o, fills.p, fills.n);
    pb_put(C->o, tb.p, tb.n);
    pb_free(&fills);
    pb_free(&tb);
}

/* SmartArt laid out from its definition, when it has no drawing saved of it (or PD_SMARTART=layout asks): the
   drawing made, 0 when it cannot be */
static int smartart_layout(conv* C, ppart* pt, int ri, const ppart* dmp, long long cx, long long cy, ppart* out) {
    const xdoc* d = &pt->x;
    static const char* names[3] = { "r:lo", "r:qs", "r:cs" };
    ppart parts[3], theme;
    int have[3], k, ok = 0, have_t = C->P->theme_path[0] && part_load(C->P, &theme, C->P->theme_path);
    pd_dgm_in in;
    pd_buf b;

    memset(&b, 0, sizeof(b));

    for (k = 0; k < 3; k++) {
        char id[64];
        const char* path;

        have[k] = xd_attr(d, ri, names[k], id, sizeof(id)) && (path = rel_of(pt, id)) != NULL &&
                  part_load(C->P, &parts[k], path);
    }

    if (have[0]) {
        memset(&in, 0, sizeof(in));
        in.data = &dmp->x;
        in.layout = &parts[0].x;
        in.style = have[1] ? &parts[1].x : NULL;
        in.colors = have[2] ? &parts[2].x : NULL;
        in.cx = (double)cx;
        in.cy = (double)cy;
        memcpy(in.line_w, C->P->line_w, sizeof(in.line_w));
        in.theme = have_t ? &theme.x : NULL;
        in.font = C->P->minor;

        if (pd_dgm_layout(&in, &b) && !b.err) {
            memset(out, 0, sizeof(*out));
            snprintf(out->path, sizeof(out->path), "%s", pt->path);

            if (getenv("PD_SMARTART_DUMP")) {   /* for a look at what it was laid out as */
                FILE* f = fopen(getenv("PD_SMARTART_DUMP"), "ab");

                if (f) {
                    fwrite(b.p, 1, b.n, f);
                    fputc('\n', f);
                    fclose(f);
                }
            }

            ok = xd_parse(&out->x, b.p, b.n);   /* (the text taken) */
            b.p = NULL;

            if (!ok) {
                xd_free(&out->x);
            } else if (dmp->nrels && (out->rels = (prel*)malloc(sizeof(prel) * (size_t)dmp->nrels)) != NULL) {
                memcpy(out->rels, dmp->rels, sizeof(prel) * (size_t)dmp->nrels);   /* its pictures: the model's */
                out->nrels = dmp->nrels;
            }
        }
    }

    pb_free(&b);

    for (k = 0; k < 3; k++) {
        if (have[k]) {
            part_free(&parts[k]);
        }
    }

    if (have_t) {
        part_free(&theme);
    }

    return ok;
}

/* SmartArt's shapes (RI its relIds): the drawing Office keeps of it (as it last laid it out), else it laid out here,
   in a frame CX by CY; 0 none */
static int smartart_shapes(conv* C, ppart* pt, int ri, long long cx, long long cy) {
    const xdoc* d = &pt->x;
    int k, saved = 0, ok = 0, forced;
    char dm[64];
    const char* dmpath, *drawpath = NULL, *mode = getenv("PD_SMARTART");
    ppart dmp, dr;

    if (!xd_attr(d, ri, "r:dm", dm, sizeof(dm)) || !(dmpath = rel_of(pt, dm)) || !part_load(C->P, &dmp, dmpath)) {
        return 0;
    }

    k = xd_find(&dmp.x, "dataModelExt");

    if (k >= 0) {
        char rid[64];

        if (xd_attr(&dmp.x, k, "relId", rid, sizeof(rid))) {
            drawpath = rel_of(pt, rid);
        }
    }

    /* the saved drawing, else laid out here (or, asked, the other way round) */
    forced = mode && !strcmp(mode, "layout");
    ok = forced && smartart_layout(C, pt, ri, &dmp, cx, cy, &dr);

    if (!ok && drawpath) {
        ok = saved = part_load(C->P, &dr, drawpath);
    }

    if (!ok && !forced) {
        ok = smartart_layout(C, pt, ri, &dmp, cx, cy, &dr);
    }

    if (ok) {
        int tree = xd_find(&dr.x, "spTree"), have_cs;
        ppart cs;
        char cid[64];
        const char* cpath;

        if ((ok = tree >= 0) != 0) {
            have_cs = saved && xd_attr(d, ri, "r:cs", cid, sizeof(cid)) && (cpath = rel_of(pt, cid)) != NULL &&
                      part_load(C->P, &cs, cpath);
            C->dm = saved ? &dmp.x : NULL;  /* the text the saved drawing has may be older than the model's */
            C->dmc = have_cs ? &cs.x : NULL;
            put_tree(C, &dr, tree, 0);
            C->dm = C->dmc = NULL;

            if (have_cs) {
                part_free(&cs);
            }
        }

        part_free(&dr);
    }

    part_free(&dmp);
    return ok;
}

/* SmartArt: its shapes in a group at the frame */
static void put_smartart(conv* C, ppart* pt, int gf) {
    const xdoc* d = &pt->x;
    int ri = xd_path(d, gf, "graphic/graphicData/relIds"), xf = xd_kid(d, gf, "xfrm"), off, ext;
    pd_buf shapes, *o = C->o;
    long long cx, cy;

    if (ri < 0 || xf < 0 || (off = xd_kid(d, xf, "off")) < 0 || (ext = xd_kid(d, xf, "ext")) < 0) {
        return;
    }

    cx = xd_int(d, ext, "cx", 0);
    cy = xd_int(d, ext, "cy", 0);
    memset(&shapes, 0, sizeof(shapes));
    C->o = &shapes;

    if (smartart_shapes(C, pt, ri, cx, cy)) {
        pb_printf(o, "<wpg:grpSp><wpg:cNvGrpSpPr/><wpg:grpSpPr><a:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" "
                  "cy=\"%lld\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"%lld\" cy=\"%lld\"/></a:xfrm></wpg:grpSpPr>",
                  xd_int(d, off, "x", 0), xd_int(d, off, "y", 0), cx, cy, cx, cy);
        pb_put(o, shapes.p, shapes.n);
        pb_puts(o, "</wpg:grpSp>");
    }

    C->o = o;
    pb_free(&shapes);
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

    if ((k = xd_kid(d, spr, "prstGeom")) >= 0) {    /* in its shape (a circle's picture round) */
        xd_raw(C->o, d, k);
    } else {
        pb_puts(C->o, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>");
    }

    pb_puts(C->o, "</pic:spPr></pic:pic>");
}

/* an embedded object (an OLE object: a worksheet, an equation, a document) as the picture of it its frame keeps (the
   oleObj's pic, of whichever of its alternatives has one), where the frame is */
static void put_ole(conv* C, ppart* pt, int gf, int gd) {
    const xdoc* d = &pt->x;
    int k, pic = -1, bf, xf = xd_kid(d, gf, "xfrm");

    for (k = gd; k < d->nv && d->v[k].a < d->v[gd].b && pic < 0; k++) {
        if (!strcmp(d->v[k].name, "oleObj")) {
            pic = xd_kid(d, k, "pic");
        }
    }

    if (pic < 0 || (bf = xd_kid(d, pic, "blipFill")) < 0 || (xf < 0 && (xf = xd_path(d, pic, "spPr/xfrm")) < 0)) {
        return;
    }

    pb_puts(C->o, "<pic:pic><pic:nvPicPr>");
    put_nvpr(C, d, gf, "nvGraphicFramePr/cNvPr", "pic:cNvPr");
    pb_puts(C->o, "<pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>");
    put_with_media(C->o, C, pt, d->s + d->v[bf].ia, d->v[bf].ib - d->v[bf].ia);
    pb_puts(C->o, "</pic:blipFill><pic:spPr><a:xfrm");

    if (d->v[xf].alen) {
        pb_putc(C->o, ' ');
        pb_put(C->o, d->v[xf].attrs, d->v[xf].alen);
    }

    pb_putc(C->o, '>');
    xd_inner(C->o, d, xf);
    pb_puts(C->o, "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>");
}

/* a chart's part in the .docx: its number there (the part, its relationships and the parts they name -- its
   workbook, its colours and style -- copied in the first time, from ppt/ to word/ so the relationships hold); 0 none */
static int chart_no(pptx* P, const char* path) {
    unsigned char* data;
    size_t len = 0;
    char dst[300], rpath[300], rdst[300];
    const char* slash = strrchr(path, '/');
    ppart cp;
    int k;

    for (k = 0; k < P->ncharts; k++) {
        if (!strcmp(P->charts[k], path)) {
            return k + 1;
        }
    }

    if (!P->zw || P->ncharts >= 64 || (data = pd_zip_get(P->zip, P->zn, path, &len)) == NULL) {
        return 0;
    }

    snprintf(dst, sizeof(dst), "word/charts/pchart%d.xml", P->ncharts + 1);
    pd_zipw_add(P->zw, dst, data, len);
    free(data);
    snprintf(rpath, sizeof(rpath), "%.*s_rels/%s.rels", slash ? (int)(slash - path + 1) : 0, path, slash ? slash + 1 : path);

    if ((data = pd_zip_get(P->zip, P->zn, rpath, &len)) != NULL) {
        snprintf(rdst, sizeof(rdst), "word/charts/_rels/pchart%d.xml.rels", P->ncharts + 1);
        pd_zipw_add(P->zw, rdst, data, len);
        free(data);
        memset(&cp, 0, sizeof(cp));
        snprintf(cp.path, sizeof(cp.path), "%s", path);
        part_rels(P, &cp);

        for (k = 0; k < cp.nrels; k++) {    /* (an external one names no part here: nothing read) */
            unsigned char* b;

            if (!strncmp(cp.rels[k].target, "ppt/", 4) &&
                    (b = pd_zip_get(P->zip, P->zn, cp.rels[k].target, &len)) != NULL) {
                snprintf(rdst, sizeof(rdst), "word/%s", cp.rels[k].target + 4);
                pd_zipw_add(P->zw, rdst, b, len);
                free(b);
            }
        }

        free(cp.rels);
    }

    snprintf(P->charts[P->ncharts], sizeof(P->charts[0]), "%.127s", path);
    pb_printf(&P->rels, "<Relationship Id=\"rIdPc%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
              "relationships/chart\" Target=\"charts/pchart%d.xml\"/>", P->ncharts + 1, P->ncharts + 1);
    return ++P->ncharts;
}

/* a chart (a frame's c:chart): a drawing of its own where the frame is on the slide, in front of the slide's canvas
   (a Word canvas holds no chart) */
static void put_chart(conv* C, ppart* pt, int gf, int ch) {
    const xdoc* d = &pt->x;
    int xf = xd_kid(d, gf, "xfrm"), off = xd_kid(d, xf, "off"), ext = xd_kid(d, xf, "ext"), no, id;
    long long x = xd_int(d, off, "x", 0), y = xd_int(d, off, "y", 0), cx = xd_int(d, ext, "cx", 0);
    long long cy = xd_int(d, ext, "cy", 0);
    char rid[64];
    const char* path;

    if (!C->charts || off < 0 || ext < 0 || !xd_attr(d, ch, "r:id", rid, sizeof(rid)) || !(path = rel_of(pt, rid)) ||
            !(no = chart_no(C->P, path))) {
        return;
    }

    if (C->gsx > 0) {   /* in a group: where the group draws it */
        x = C->gx + (long long)(x * C->gsx);
        y = C->gy + (long long)(y * C->gsy);
        cx = (long long)(cx * C->gsx);
        cy = (long long)(cy * C->gsy);
    }

    id = 3600000 + ++C->P->docpr;
    pb_printf(C->charts, "<w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\" simplePos=\"0\" "
              "relativeHeight=\"%d\" behindDoc=\"0\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos "
              "x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"page\"><wp:posOffset>%lld</wp:posOffset></wp:positionH>"
              "<wp:positionV relativeFrom=\"page\"><wp:posOffset>%lld</wp:posOffset></wp:positionV><wp:extent cx=\"%lld\" "
              "cy=\"%lld\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/><wp:wrapNone/><wp:docPr id=\"%d\" "
              "name=\"Chart\"/><wp:cNvGraphicFramePr/><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/"
              "drawingml/2006/chart\"><c:chart xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" "
              "r:id=\"rIdPc%d\"/></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r>", id, x, y, cx, cy, id, no);
}

/* whether there is an equation (a14:m, Office Math) under element N */
static int has_math(const xdoc* d, int n) {
    int k;

    for (k = n; k < d->nv && d->v[k].a < d->v[n].b; k++) {
        if (!strcmp(d->v[k].name, "oMath")) {
            return 1;
        }
    }

    return 0;
}

static void put_shape(conv* C, ppart* pt, int n, int skip_ph) {
    const xdoc* d = &pt->x;
    const char* el = d->v[n].name;
    char type[32], v[16];
    long long idx;
    int isph = 0, lsp = -1, msp = -1, kind = 0;

    C->isph = 0;
    C->ph_type[0] = C->ph_idx[0] = '\0';

    if (!strcmp(el, "sp") || !strcmp(el, "pic") || !strcmp(el, "graphicFrame")) {
        isph = ph_of(d, n, type, sizeof(type), &idx);

        if (isph) {     /* as the part says it, for a writer */
            int nv;

            C->isph = 1;
            snprintf(C->ph_type, sizeof(C->ph_type), "%s", type);

            for (nv = d->v[n].kid; nv >= 0; nv = d->v[nv].next) {
                if (xd_path(d, nv, "nvPr/ph") >= 0) {
                    xd_attr(d, xd_path(d, nv, "nvPr/ph"), "idx", C->ph_idx, sizeof(C->ph_idx));
                    break;
                }
            }
        }

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
        int tx = xd_kid(d, n, "txBody"), cn = xd_path(d, n, "nvSpPr/cNvSpPr");
        size_t mark = C->o->n;
        int widen = 0, spr = xd_kid(d, n, "spPr");
        char wr[16] = "", al[16] = "";
        xref sty = shape_style(C, d, n, lsp, msp);

        /* text not to wrap (wrap="none"), in a box with no fill or outline of its own: the box made wide */
        if (tx >= 0 && (xd_attr(d, xd_kid(d, tx, "bodyPr"), "wrap", wr, sizeof(wr)) ||
                        (lsp >= 0 && xd_attr(&C->layout->x, xd_path(&C->layout->x, lsp, "txBody/bodyPr"), "wrap", wr,
                                             sizeof(wr)))) && !strcmp(wr, "none") && sty.n < 0 &&
                xd_kid(d, spr, "solidFill") < 0 && xd_kid(d, spr, "gradFill") < 0 && xd_kid(d, spr, "blipFill") < 0 &&
                (xd_kid(d, spr, "ln") < 0 || xd_path(d, spr, "ln/noFill") >= 0)) {
            xd_attr(d, xd_path(d, tx, "p/pPr"), "algn", al, sizeof(al));
            widen = !strcmp(al, "ctr") ? 2 : !strcmp(al, "r") ? 3 : 1;
        }

        pb_puts(C->o, "<wps:wsp>");
        put_nvpr(C, d, n, "nvSpPr/cNvPr", "wps:cNvPr");
        pb_puts(C->o, xd_attr(d, cn, "txBox", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true")) ?
                "<wps:cNvSpPr txBox=\"1\"/>" : "<wps:cNvSpPr/>");

        if (!put_sppr(C, pt, n, lsp, msp, "wps:spPr", widen)) {
            C->o->n = mark;     /* nowhere to put it */
            return;
        }

        if (sty.n >= 0) {
            put_renamed(C->P, C->o, sty.d, sty.n, "wps:style");
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

        if (!put_sppr(C, pt, n, -1, -1, "wps:spPr", 0)) {
            C->o->n = mark;
            return;
        }

        if (st >= 0) {
            put_renamed(C->P, C->o, d, st, "wps:style");
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

        if (!put_sppr(C, pt, n, lsp, msp, "pic:spPr", 0)) {
            C->o->n = mark;
            return;
        }

        pb_puts(C->o, "</pic:pic>");
    } else if (!strcmp(el, "grpSp")) {
        int gp = xd_kid(d, n, "grpSpPr");

        pb_puts(C->o, "<wpg:grpSp>");

        if (marked(C)) {
            pb_printf(C->o, "<wpg:cNvPr id=\"%lld\" name=\"Group\">", xd_int(d, xd_path(d, n, "nvGrpSpPr/cNvPr"), "id", 0) +
                      C->id_base);
            put_marker(C, from_of(C), 0);
            pb_puts(C->o, "</wpg:cNvPr>");
        }

        pb_puts(C->o, "<wpg:cNvGrpSpPr/>");

        long long gx = C->gx, gy = C->gy;
        double gsx = C->gsx, gsy = C->gsy;
        int xf = xd_kid(d, gp, "xfrm");

        if (gp >= 0) {
            put_renamed(C->P, C->o, d, gp, "wpg:grpSpPr");
        } else {
            pb_puts(C->o, "<wpg:grpSpPr/>");
        }

        if (xf >= 0) {  /* where its children are on the slide (for a chart among them, drawn apart) */
            int off = xd_kid(d, xf, "off"), ext = xd_kid(d, xf, "ext"), co = xd_kid(d, xf, "chOff");
            int ce = xd_kid(d, xf, "chExt");
            double sx = xd_int(d, ce, "cx", 0) > 0 ? (double)xd_int(d, ext, "cx", 0) / (double)xd_int(d, ce, "cx", 0) : 1;
            double sy = xd_int(d, ce, "cy", 0) > 0 ? (double)xd_int(d, ext, "cy", 0) / (double)xd_int(d, ce, "cy", 0) : 1;
            double ox = (double)xd_int(d, off, "x", 0) - xd_int(d, co, "x", 0) * sx;
            double oy = (double)xd_int(d, off, "y", 0) - xd_int(d, co, "y", 0) * sy;

            C->gx = gsx > 0 ? gx + (long long)(ox * gsx) : (long long)ox;
            C->gy = gsy > 0 ? gy + (long long)(oy * gsy) : (long long)oy;
            C->gsx = gsx > 0 ? gsx * sx : sx;
            C->gsy = gsy > 0 ? gsy * sy : sy;
        }

        C->depth++;
        put_tree(C, pt, n, skip_ph);
        C->depth--;
        pb_puts(C->o, "</wpg:grpSp>");
        C->gx = gx;
        C->gy = gy;
        C->gsx = gsx;
        C->gsy = gsy;
    } else if (!strcmp(el, "graphicFrame")) {
        int gd = xd_path(d, n, "graphic/graphicData"), tbl = gd >= 0 ? xd_kid(d, gd, "tbl") : -1;

        if (tbl >= 0) {
            put_table(C, pt, n, tbl);
        } else if (gd >= 0 && xd_kid(d, gd, "relIds") >= 0) {
            put_smartart(C, pt, n);
        } else if (gd >= 0 && xd_kid(d, gd, "chart") >= 0) {
            put_chart(C, pt, n, xd_kid(d, gd, "chart"));
        } else if (gd >= 0) {
            put_ole(C, pt, n, gd);
        }
    } else if (!strcmp(el, "AlternateContent")) {   /* what a newer PowerPoint has: its fallback, but an equation */
        int fb = xd_kid(d, n, "Fallback"), k, ch;

        for (ch = d->v[n].kid; ch >= 0; ch = d->v[ch].next) {
            char req[64];

            if (!strcmp(d->v[ch].name, "Choice") && xd_attr(d, ch, "Requires", req, sizeof(req)) &&
                    !strcmp(req, "a14") && has_math(d, ch)) {
                fb = ch;    /* (its fallback a picture of it) */
                break;
            }
        }

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

/* a part's background fill when it is a picture or a gradient (bgPr/blipFill, bgPr/gradFill): -1 none */
static int background_fill(const ppart* pt) {
    const xdoc* d = &pt->x;
    int bg = xd_find(d, "bg"), pr = bg >= 0 ? xd_kid(d, bg, "bgPr") : -1, k;

    if (pr < 0) {
        return -1;
    }

    k = xd_kid(d, pr, "blipFill");
    return k >= 0 ? k : xd_kid(d, pr, "gradFill");
}

/* a picture or a gradient over the whole slide, under everything on it */
static void put_background_fill(conv* C, const ppart* pt, int f, int from) {
    const xdoc* d = &pt->x;

    if (f < 0) {
        return;
    }

    if (!strcmp(d->v[f].name, "blipFill")) {
        pb_printf(C->o, "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"Background\">", 3900000 + C->slide_no);
        put_marker(C, from, 1);
        pb_puts(C->o, "</pic:cNvPr><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill>");
        put_with_media(C->o, C, pt, d->s + d->v[f].ia, d->v[f].ib - d->v[f].ia);
        pb_printf(C->o, "</pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/>"
                  "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>", C->P->sw, C->P->sh);
        return;
    }

    pb_printf(C->o, "<wps:wsp><wps:cNvPr id=\"%d\" name=\"Background\">", 3900000 + C->slide_no);
    put_marker(C, from, 1);
    pb_printf(C->o, "</wps:cNvPr><wps:cNvSpPr/><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/>"
              "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>", C->P->sw, C->P->sh);
    put_mapped(C->P, C->o, d->s + d->v[f].ia, d->v[f].ib - d->v[f].ia);
    pb_puts(C->o, "<a:ln><a:noFill/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>");
}

static void put_slide(pptx* P, const xdoc* pres, int pres_style, const char* path, int no, int last) {
    ppart slide, layout, master, theme;
    int have_l = 0, have_m = 0, tree, show = 1;
    const char* lp, *mp;
    long bg;
    conv C;
    pd_buf cv, charts;
    char v[16];

    memset(&cv, 0, sizeof(cv));
    memset(&charts, 0, sizeof(charts));

    if (!part_load(P, &slide, path)) {
        return;
    }

    if ((lp = rel_like(&slide, "slideLayouts/")) != NULL && part_load(P, &layout, lp)) {
        have_l = 1;

        if ((mp = rel_like(&layout, "slideMasters/")) != NULL && part_load(P, &master, mp)) {
            have_m = 1;
        }
    }

    /* its master's theme, and the colour map the master, then the layout, then the slide give */
    P->alias[0] = 1;
    P->alias[1] = 0;
    P->alias[2] = 3;
    P->alias[3] = 2;

    if (have_m) {
        theme_load(P, rel_like(&master, "theme/"));
        clrmap_load(P, &master.x);
    }

    if (have_l && xd_find(&layout.x, "overrideClrMapping") >= 0) {
        clrmap_load(P, &layout.x);
    }

    if (xd_find(&slide.x, "overrideClrMapping") >= 0) {
        clrmap_load(P, &slide.x);
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
    C.charts = &charts;

    if (P->theme_path[0] && strcmp(P->theme_path, P->doc_theme) && part_load(P, &theme, P->theme_path)) {
        C.th = &theme.x;    /* another master's theme */
    }

    /* the background: the slide's, else its layout's, else its master's; white. A picture or a gradient is a
       shape of its own over the whole slide (the canvas's own background its colour, where the picture is not) */
    {
        const ppart* from = NULL;

        bg = background_of(P, &slide);
        from = bg >= 0 || xd_find(&slide.x, "bg") >= 0 ? &slide : NULL;

        if (bg < 0 && !from && have_l) {
            bg = background_of(P, &layout);
            from = bg >= 0 || xd_find(&layout.x, "bg") >= 0 ? &layout : NULL;
        }

        if (bg < 0 && !from && have_m) {
            bg = background_of(P, &master);
            from = &master;
        }

        pb_printf(&cv, "<wpc:bg><a:solidFill><a:srgbClr val=\"%06lX\"/></a:solidFill></wpc:bg><wpc:whole/>",
                  (unsigned long)(bg >= 0 ? bg : 0xFFFFFF));

        if (from && background_fill(from) >= 0) {
            put_background_fill(&C, from, background_fill(from), from == &slide ? 0 : from == &master ? 2 : 1);
        } else if (from) {   /* the theme's background style (bgRef 1002, 1003...), its placeholder colour the ref's */
            int br = xd_path(&from->x, xd_find(&from->x, "bg"), "bgRef");
            long idx = br >= 0 ? xd_int(&from->x, br, "idx", 0) : 0;
            ppart th;

            if (idx > 1001 && P->theme_path[0] && part_load(P, &th, P->theme_path)) {
                int lst = xd_find(&th.x, "bgFillStyleLst"), e = lst >= 0 ? th.x.v[lst].kid : -1;

                for (; e >= 0 && idx > 1001; e = th.x.v[e].next, idx--) {
                }

                if (e >= 0 && (!strcmp(th.x.v[e].name, "gradFill") || !strcmp(th.x.v[e].name, "blipFill"))) {
                    P->ph = bg >= 0 ? bg : 0xFFFFFF;
                    put_background_fill(&C, &th, e, from == &slide ? 0 : from == &master ? 2 : 1);
                    P->ph = -1;
                }

                part_free(&th);
            }
        }
    }

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

    if (P->nrec >= P->caprec) {     /* what the slide is on, for a writer */
        int nc = P->caprec ? P->caprec * 2 : 32;
        struct pslide* r = (struct pslide*)realloc(P->rec, sizeof(*r) * (size_t)nc);

        if (r) {
            P->rec = r;
            P->caprec = nc;
        }
    }

    if (P->nrec < P->caprec) {
        struct pslide* r = &P->rec[P->nrec++];

        snprintf(r->layout, sizeof(r->layout), "%s", have_l ? layout.path : "");
        r->own_bg = xd_find(&slide.x, "bg") >= 0;
        r->no_master = !show;
        r->bg = bg >= 0 ? bg : 0xFFFFFF;
        r->clr[0] = '\0';
        r->tail = NULL;
        snprintf(r->notes, sizeof(r->notes), "%s", rel_like(&slide, "notesSlides/") ? rel_like(&slide, "notesSlides/") :
                 "");

        {   /* its transition and timing (animations) as they were, to be written back unchanged */
            int cm = xd_find(&slide.x, "clrMapOvr"), cs = xd_find(&slide.x, "cSld");
            size_t a = cm >= 0 ? slide.x.v[cm].b : cs >= 0 ? slide.x.v[cs].b : 0, e = slide.x.v[0].ib, i;
            int any = 0;

            for (i = a; a && i < e; i++) {
                any |= slide.x.s[i] == '<';
            }

            if (any) {
                pd_buf t;
                const char* at = slide.x.v[0].attrs, *end = at ? at + slide.x.v[0].alen : NULL;

                memset(&t, 0, sizeof(t));

                while (at && at < end) {    /* the root's namespaces and mc:Ignorable, for what it holds */
                    const char* q = at;

                    while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
                        q++;
                    }

                    at = q;

                    while (at < end && *at != '"' && *at != '\'') {
                        at++;
                    }

                    if (at < end) {
                        char quote = *at;
                        const char* v2 = memchr(at + 1, quote, (size_t)(end - at - 1));

                        if (!v2) {
                            break;
                        }

                        if (!strncmp(q, "xmlns:", 6) || !strncmp(q, "mc:Ignorable", 12)) {
                            pb_putc(&t, ' ');
                            pb_put(&t, q, (size_t)(v2 + 1 - q));
                        }

                        at = v2 + 1;
                    }
                }

                pb_putc(&t, '\n');
                pb_put(&t, slide.x.s + a, e - a);
                pb_putc(&t, '\0');
                r->tail = t.err ? NULL : t.p;

                if (t.err) {
                    pb_free(&t);
                }
            }
        }

        {   /* its own colour map, kept to be written back */
            static const char* an[12] = { "bg1", "tx1", "bg2", "tx2", "accent1", "accent2", "accent3", "accent4",
                                          "accent5", "accent6", "hlink", "folHlink"
                                        };
            int om = xd_find(&slide.x, "overrideClrMapping"), q;
            size_t cl = 0;

            for (q = 0; om >= 0 && q < 12; q++) {
                char cm[32];

                if (xd_attr(&slide.x, om, an[q], cm, sizeof(cm)) && cl < sizeof(r->clr) - 40) {
                    cl += (size_t)snprintf(r->clr + cl, sizeof(r->clr) - cl, "%s%s=\"%s\"", cl ? " " : "", an[q], cm);
                }
            }
        }
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
    pb_puts(&P->doc, "</wpc:wpc></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r>");
    pb_put(&P->doc, charts.p, charts.n);
    pb_puts(&P->doc, "</w:p>");
    pb_free(&cv);
    pb_free(&charts);

    if (C.th) {
        part_free(&theme);
    }

    if (have_m) {
        part_free(&master);
    }

    if (have_l) {
        part_free(&layout);
    }

    part_free(&slide);
}

/* whether a slide is hidden (show="0"): not shown in the slide show */
static int slide_hidden(pptx* P, const char* path) {
    size_t len = 0;
    char* s = (char*)pd_zip_get(P->zip, P->zn, path, &len);
    char v[16];
    int hidden = 0;
    xdoc x;

    if (s) {
        if (xd_parse(&x, s, len)) {
            hidden = x.nv > 0 && xd_attr(&x, 0, "show", v, sizeof(v)) && (!strcmp(v, "0") || !strcmp(v, "false"));
        }

        xd_free(&x);
    }

    return hidden;
}

/* ---- the package's masters kept for a writer ---- */

/* The masters, layouts and themes a presentation came with, kept so that a writer can put its slides back on them:
   one resource of the document (PD_PPTX_KEEP_MIME), entries of name NUL content-type NUL source NUL extra NUL
   length(4, little endian) bytes, as the .docx reader keeps parts. Sources: "master" (extra: its id in the
   presentation), "" (a part a master reaches: its layouts, theme, pictures, and their relationships), "pres"
   (presentation.xml, for its default text style and notes size), "tablestyles", "map" (a line per page: its
   section's block id, its layout, its own background (1/0), whether it hides the master's shapes, its background
   colour; first a line "theme" and the first master's theme, which the document's theme is). */
#define PD_PPTX_KEEP_MIME "application/vnd.parade.pptx-kept-parts"

typedef struct {
    pd_buf b;
    char (*names)[160];
    int n, cap;
} pkeep;

static int pkeep_has(const pkeep* k, const char* name) {
    int i;

    for (i = 0; i < k->n; i++) {
        if (!strcmp(k->names[i], name)) {
            return 1;
        }
    }

    return 0;
}

static void pkeep_entry(pkeep* k, const char* name, const char* ctype, const char* src, const char* extra,
                        const void* data, size_t len) {
    unsigned char l4[4];

    if (strlen(name) >= 160 || pkeep_has(k, name)) {
        return;
    }

    if (k->n >= k->cap) {
        int nc = k->cap ? k->cap * 2 : 64;
        char (*t)[160] = (char (*)[160])realloc(k->names, sizeof(*t) * (size_t)nc);

        if (!t) {
            return;
        }

        k->names = t;
        k->cap = nc;
    }

    snprintf(k->names[k->n++], 160, "%s", name);
    pb_put(&k->b, name, strlen(name) + 1);
    pb_put(&k->b, ctype, strlen(ctype) + 1);
    pb_put(&k->b, src, strlen(src) + 1);
    pb_put(&k->b, extra, strlen(extra) + 1);
    l4[0] = (unsigned char)(len & 255);
    l4[1] = (unsigned char)((len >> 8) & 255);
    l4[2] = (unsigned char)((len >> 16) & 255);
    l4[3] = (unsigned char)((len >> 24) & 255);
    pb_put(&k->b, l4, 4);
    pb_put(&k->b, data, len);
}

static int ext_eq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
    }

    return *a == *b;
}

/* a part's content type in the package: its override, else its extension's default */
static void ctype_of(const xdoc* ct, const char* name, char* out, size_t cap) {
    const char* dot = strrchr(name, '.');
    char v[300];
    int k;

    out[0] = '\0';

    for (k = 0; ct && k < ct->nv; k++) {
        if (!strcmp(ct->v[k].name, "Override") && xd_attr(ct, k, "PartName", v, sizeof(v)) &&
                !strcmp(v[0] == '/' ? v + 1 : v, name)) {
            xd_attr(ct, k, "ContentType", out, cap);
            return;
        }
    }

    for (k = 0; ct && dot && k < ct->nv; k++) {
        if (!strcmp(ct->v[k].name, "Default") && xd_attr(ct, k, "Extension", v, sizeof(v)) &&
                ext_eq(v, dot + 1)) {
            xd_attr(ct, k, "ContentType", out, cap);
            return;
        }
    }

    {   /* a package that does not say: what the part is, by where it is */
        static const char* const by_dir[][2] = {
            { "/slideMasters/", "application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml" },
            { "/slideLayouts/", "application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml" },
            { "/theme/", "application/vnd.openxmlformats-officedocument.theme+xml" },
            { "/tags/", "application/vnd.openxmlformats-officedocument.presentationml.tags+xml" }
        };

        for (k = 0; k < 4; k++) {
            if (strstr(name, by_dir[k][0]) && dot && ext_eq(dot + 1, "xml")) {
                snprintf(out, cap, "%s", by_dir[k][1]);
                return;
            }
        }
    }
}

/* a part kept, with its relationships and the parts they reach */
static void pkeep_part(pptx* P, pkeep* k, const xdoc* ct, const char* name, const char* src, const char* extra,
                       int depth) {
    unsigned char* data;
    size_t len = 0;
    char ctype[256], rn[300];
    const char* slash = strrchr(name, '/');
    ppart pt;
    int i;

    if (depth > 4 || pkeep_has(k, name) || (data = pd_zip_get(P->zip, P->zn, name, &len)) == NULL) {
        return;
    }

    ctype_of(ct, name, ctype, sizeof(ctype));
    pkeep_entry(k, name, ctype, src, extra, data, len);
    free(data);
    snprintf(rn, sizeof(rn), "%.*s_rels/%s.rels", slash ? (int)(slash - name + 1) : 0, name, slash ? slash + 1 : name);

    if ((data = pd_zip_get(P->zip, P->zn, rn, &len)) == NULL) {
        return;
    }

    pkeep_entry(k, rn, "application/vnd.openxmlformats-package.relationships+xml", "", "", data, len);
    free(data);
    memset(&pt, 0, sizeof(pt));
    snprintf(pt.path, sizeof(pt.path), "%s", name);
    part_rels(P, &pt);  /* (an external target names no part of the package: not found, nothing kept) */

    for (i = 0; i < pt.nrels; i++) {
        if (!strstr(pt.rels[i].target, "slides/slide")) {   /* not the slides themselves */
            pkeep_part(P, k, ct, pt.rels[i].target, "", "", depth + 1);
        }
    }

    free(pt.rels);
}

/* Each slide's speaker notes (its notes slide's body placeholder) made a story of its section (pd_section_props.notes):
   a paragraph of the story for each of the body's, their text as it is */
static void notes_read(pptx* P, pd_doc* d) {
    int i;

    for (i = 0; i < P->nrec; i++) {
        pd_block_id sec = pd_doc_child(d, pd_doc_root(d), i), story = 0;
        pd_section_props sp;
        ppart np;
        int n, first = 1;

        if (!P->rec[i].notes[0] || !sec || pd_doc_section_props(d, sec, &sp) != PD_OK ||
                !part_load(P, &np, P->rec[i].notes)) {
            continue;
        }

        for (n = 0; n < np.x.nv; n++) {     /* the shape with the body placeholder: its text's paragraphs */
            int ph, tb, p;
            char ty[32] = "";

            if (strcmp(np.x.v[n].name, "sp") || (ph = xd_path(&np.x, n, "nvSpPr/nvPr/ph")) < 0 ||
                    !xd_attr(&np.x, ph, "type", ty, sizeof(ty)) || strcmp(ty, "body") ||
                    (tb = xd_kid(&np.x, n, "txBody")) < 0) {
                continue;
            }

            for (p = np.x.v[tb].kid; p >= 0; p = np.x.v[p].next) {
                pd_buf txt;
                pd_block_id para = 0;
                int r;

                if (strcmp(np.x.v[p].name, "p")) {
                    continue;
                }

                memset(&txt, 0, sizeof(txt));

                for (r = p + 1; r < np.x.nv && np.x.v[r].a < np.x.v[p].b; r++) {   /* its runs' text, in order */
                    if (!strcmp(np.x.v[r].name, "t")) {
                        mu_decode(np.x.s + np.x.v[r].ia, np.x.v[r].ib - np.x.v[r].ia, &txt);
                    } else if (!strcmp(np.x.v[r].name, "br")) {
                        pb_putc(&txt, '\n');
                    }
                }

                if (!story && pd_doc_insert_block(d, 0, -1, PD_BLOCK_STORY, &story) != PD_OK) {
                    pb_free(&txt);
                    break;
                }

                if (first && pd_doc_child(d, story, 0)) {   /* a new story's own empty paragraph: the first */
                    para = pd_doc_child(d, story, 0);
                } else {
                    pd_doc_insert_block(d, story, -1, PD_BLOCK_PARAGRAPH, &para);
                }

                first = 0;

                if (para && txt.n) {
                    pd_pos at;

                    at.block = para;
                    at.offset = 0;
                    pd_doc_insert_text(d, at, txt.p, (uint32_t)txt.n, PD_FORMAT_INHERIT, NULL);
                }

                pb_free(&txt);
            }

            break;
        }

        part_free(&np);

        if (story) {
            sp.notes = story;
            pd_doc_set_section_props(d, sec, &sp);
        }
    }

    pd_doc_clear_undo(d);
}

/* the presentation's masters and what they reach, and what each page of d was on, made a resource of d */
static void pkeep_read(pptx* P, pd_doc* d) {
    pkeep k;
    ppart pres;
    xdoc ct;
    size_t len = 0;
    char* cts = (char*)pd_zip_get(P->zip, P->zn, "[Content_Types].xml", &len);
    int have_ct = cts && xd_parse(&ct, cts, len), lst, m, i, nmasters = 0;
    pd_block_info ri;
    pd_buf map;

    memset(&k, 0, sizeof(k));
    memset(&map, 0, sizeof(map));

    if (!have_ct && cts) {
        xd_free(&ct);
    }

    if (pd_doc_block_info(d, pd_doc_root(d), &ri) != PD_OK || ri.child_count != P->nrec ||
            !part_load(P, &pres, "ppt/presentation.xml")) {
        if (have_ct) {
            xd_free(&ct);
        }

        return;
    }

    lst = xd_find(&pres.x, "sldMasterIdLst");

    for (m = lst >= 0 ? pres.x.v[lst].kid : -1; m >= 0; m = pres.x.v[m].next) {
        char rid[64], id[32] = "";
        const char* path;

        if (xd_attr(&pres.x, m, "r:id", rid, sizeof(rid)) && (path = rel_of(&pres, rid)) != NULL) {
            xd_attr(&pres.x, m, "id", id, sizeof(id));
            pkeep_part(P, &k, have_ct ? &ct : NULL, path, "master", id, 0);
            nmasters++;
        }
    }

    for (i = 0; nmasters && i < P->nrec; i++) {     /* the layouts the slides are on (a master may not list them) */
        if (P->rec[i].layout[0]) {
            pkeep_part(P, &k, have_ct ? &ct : NULL, P->rec[i].layout, "", "", 1);
        }
    }

    if (nmasters) {
        unsigned char* data;

        if ((data = pd_zip_get(P->zip, P->zn, "ppt/presentation.xml", &len)) != NULL) {
            pkeep_entry(&k, "ppt/presentation.xml", "", "pres", "", data, len);
            free(data);
        }

        if ((data = pd_zip_get(P->zip, P->zn, "ppt/tableStyles.xml", &len)) != NULL) {
            pkeep_entry(&k, "ppt/tableStyles.xml", "application/vnd.openxmlformats-officedocument.presentationml."
                        "tableStyles+xml", "tablestyles", "", data, len);
            free(data);
        }

        pb_printf(&map, "theme\t%s\n", P->doc_theme);

        for (i = 0; i < P->nrec; i++) {
            pb_printf(&map, "%u\t%s\t%d\t%d\t%06lX\t%s\n", (unsigned)pd_doc_child(d, pd_doc_root(d), i), P->rec[i].layout,
                      P->rec[i].own_bg, P->rec[i].no_master, (unsigned long)P->rec[i].bg & 0xFFFFFFul, P->rec[i].clr);
        }

        pkeep_entry(&k, "map", "", "map", "", map.p ? map.p : "", map.n);

        for (i = 0; i < P->nrec; i++) {     /* each slide's transition and timing, by its section */
            if (P->rec[i].tail) {
                char key[16], name[32];

                snprintf(key, sizeof(key), "%u", (unsigned)pd_doc_child(d, pd_doc_root(d), i));
                snprintf(name, sizeof(name), "tail/%s", key);   /* one each: entries are kept once by name */
                pkeep_entry(&k, name, "", "tail", key, P->rec[i].tail, strlen(P->rec[i].tail));
            }
        }

        if (!k.b.err) {
            pd_res_id r;

            pd_doc_add_resource(d, PD_PPTX_KEEP_MIME, k.b.p, k.b.n, &r);
        }
    }

    pb_free(&map);
    pb_free(&k.b);
    free(k.names);
    part_free(&pres);

    if (have_ct) {
        xd_free(&ct);
    }
}

pd_status pd_pptx_import(pd_doc* d, const unsigned char* s, size_t n) {
    pptx P;
    ppart pres, ts;
    int have_ts = 0;
    pd_buf zip;
    int lst, k, nslides = 0, idx = 0, ds, no, nos[1024], hid[1024];
    const char* paths[1024];
    char tp[300];
    pd_status st;

    memset(&P, 0, sizeof(P));
    P.ph = -1;
    P.zip = s;
    P.zn = n;
    P.alias[0] = 1;
    P.alias[1] = 0;
    P.alias[2] = 3;
    P.alias[3] = 2;

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

        snprintf(P.doc_theme, sizeof(P.doc_theme), "%s", P.theme_path);
    }

    if (part_load(&P, &ts, "ppt/tableStyles.xml")) {   /* the table styles the tables name */
        P.tstyles = &ts.x;
        have_ts = 1;
    }

    lst = xd_find(&pres.x, "sldIdLst");

    /* the slides, hidden ones too (marked so once read: edited as the others, left out of a PDF) */
    for (k = lst >= 0 ? pres.x.v[lst].kid : -1, no = 0; k >= 0 && nslides < 1024; k = pres.x.v[k].next) {
        if (xd_attr(&pres.x, k, "r:id", tp, sizeof(tp)) && rel_of(&pres, tp) && ++no) {
            hid[nslides] = slide_hidden(&P, rel_of(&pres, tp));
            nos[nslides] = no;
            paths[nslides++] = rel_of(&pres, tp);
        }
    }

    memset(&zip, 0, sizeof(zip));
    P.zw = pd_zipw_new(&zip);

    if (!P.zw) {
        if (have_ts) {
            part_free(&ts);
        }

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
            "xmlns:wpc=\"http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas\" "
            "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"><w:body>");
    pb_puts(&P.rels, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><Relationships "
            "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rIdPt\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "theme\" Target=\"theme/theme1.xml\"/>");

    for (idx = 0; idx < nslides; idx++) {
        put_slide(&P, &pres.x, ds, paths[idx], nos[idx], idx == nslides - 1);
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
    {   /* what text takes that is in no run -- an equation -- PowerPoint's 18 points in the body font */
        pd_buf sty;

        memset(&sty, 0, sizeof(sty));
        pb_puts(&sty, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><w:styles "
                "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:docDefaults><w:rPrDefault>"
                "<w:rPr><w:rFonts w:ascii=\"");
        put_esc(&sty, P.minor);
        pb_puts(&sty, "\" w:hAnsi=\"");
        put_esc(&sty, P.minor);
        pb_puts(&sty, "\"/><w:sz w:val=\"36\"/><w:szCs w:val=\"36\"/></w:rPr></w:rPrDefault></w:docDefaults></w:styles>");
        pd_zipw_add(P.zw, "word/styles.xml", sty.p, sty.n);
        pb_free(&sty);
    }

    {   /* the font table: what kind each face is, so one not here is stood in for by one like it (a sans for a
           sans: most slides' faces are) */
        pd_buf ft;
        int q;

        memset(&ft, 0, sizeof(ft));
        pb_puts(&ft, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><w:fonts "
                "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">");

        for (q = 0; q < P.nfonts; q++) {
            const char* f = P.fonts[q];
            int serif = (strstr(f, "Times") || strstr(f, "Georgia") || strstr(f, "Garamond") || strstr(f, "Cambria") ||
                         strstr(f, "Palatino") || strstr(f, "Book") || strstr(f, "Roman") || strstr(f, "Minion") ||
                         strstr(f, "Baskerville") || strstr(f, "Caslon") || strstr(f, "Didot") || strstr(f, "Bodoni") ||
                         strstr(f, "Song") || strstr(f, "Ming") || (strstr(f, "Serif") && !strstr(f, "Sans")));
            int mono = strstr(f, "Mono") || strstr(f, "Courier") || strstr(f, "Consol") || strstr(f, "Code");

            pb_puts(&ft, "<w:font w:name=\"");
            put_esc(&ft, f);
            pb_printf(&ft, "\"><w:family w:val=\"%s\"/><w:pitch w:val=\"%s\"/></w:font>",
                      mono ? "modern" : serif ? "roman" : "swiss", mono ? "fixed" : "variable");
        }

        pb_puts(&ft, "</w:fonts>");
        pd_zipw_add(P.zw, "word/fontTable.xml", ft.p, ft.n);
        pb_free(&ft);
    }
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

    if (have_ts) {
        part_free(&ts);
    }

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

    if (st == PD_OK) {
        pkeep_read(&P, d);
        notes_read(&P, d);

        for (k = 0; k < nslides; k++) {     /* the hidden ones marked */
            pd_block_id sec = pd_doc_child(d, pd_doc_root(d), k);
            pd_section_props sp;

            if (hid[k] && sec && pd_doc_section_props(d, sec, &sp) == PD_OK) {
                sp.hidden = 1;
                pd_doc_set_section_props(d, sec, &sp);
            }
        }

        pd_doc_clear_undo(d);
    }

    {
        int ri;

        for (ri = 0; P.rec && ri < P.nrec; ri++) {
            free(P.rec[ri].tail);
        }
    }

    free(P.rec);
    return st;
}

/* ---- Word's SmartArt ---- */

static const char* find_in(const char* s, const char* e, const char* key) {
    size_t n = strlen(key);

    for (; s + n <= e; s++) {
        if (*s == *key && !memcmp(s, key, n)) {
            return s;
        }
    }

    return NULL;
}

char* pd_docx_smartart(const unsigned char* zip, size_t zn, const char* xml, size_t len, size_t* out_len,
                       char** rels, size_t* rels_len) {
    static const char key[] = "<a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\"";
    static const char end[] = "</a:graphicData>";
    const char* e = xml + len, *p = xml, *g;
    pptx P;
    ppart doc;
    conv C;
    pd_buf out;
    int any = 0;

    if (!find_in(xml, e, key)) {
        return NULL;
    }

    memset(&P, 0, sizeof(P));
    P.ph = -1;
    P.zip = zip;
    P.zn = zn;
    P.alias[0] = 1;     /* Word's colour map: bg1 lt1, tx1 dk1 */
    P.alias[1] = 0;
    P.alias[2] = 3;
    P.alias[3] = 2;
    P.docx = 1;
    theme_load(&P, "word/theme/theme1.xml");
    memset(&doc, 0, sizeof(doc));
    snprintf(doc.path, sizeof(doc.path), "word/document.xml");
    part_rels(&P, &doc);
    memset(&C, 0, sizeof(C));
    C.P = &P;
    memset(&out, 0, sizeof(out));

    while ((g = find_in(p, e, key)) != NULL) {
        const char* q = find_in(g, e, end), *x, *ex = NULL;
        long long cx = 0, cy = 0;
        char* seg;
        ppart pt;
        pd_buf shapes;
        int ri, ok = 0;

        if (!q) {
            break;
        }

        q += sizeof(end) - 1;

        for (x = p; (x = find_in(x, g, "<wp:extent ")) != NULL; x++) {     /* the frame: its drawing's extent */
            ex = x;
        }

        if (ex) {
            const char* a = find_in(ex, g, "cx=\""), *b = find_in(ex, g, "cy=\"");

            cx = a ? atoll(a + 4) : 0;
            cy = b ? atoll(b + 4) : 0;
        }

        memset(&pt, 0, sizeof(pt));
        memset(&shapes, 0, sizeof(shapes));
        seg = (char*)malloc((size_t)(q - g) + 1);

        if (seg && cx > 0 && cy > 0) {
            memcpy(seg, g, (size_t)(q - g));
            seg[q - g] = '\0';

            if (xd_parse(&pt.x, seg, (size_t)(q - g))) {    /* (the text taken) */
                pt.rels = doc.rels;
                pt.nrels = doc.nrels;
                snprintf(pt.path, sizeof(pt.path), "%s", doc.path);
                ri = xd_find(&pt.x, "relIds");
                C.o = &shapes;
                ok = ri >= 0 && smartart_shapes(&C, &pt, ri, cx, cy);
            }

            xd_free(&pt.x);
            seg = NULL;
        }

        free(seg);
        pb_put(&out, p, (size_t)(g - p));

        if (ok) {   /* a group of its shapes, as big as the frame */
            pb_printf(&out, "<a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingGroup\">"
                      "<wpg:wgp xmlns:wpg=\"http://schemas.microsoft.com/office/word/2010/wordprocessingGroup\" "
                      "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wpg:cNvGrpSpPr/>"
                      "<wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/><a:chOff x=\"0\" "
                      "y=\"0\"/><a:chExt cx=\"%lld\" cy=\"%lld\"/></a:xfrm></wpg:grpSpPr>", cx, cy, cx, cy);
            pb_put(&out, shapes.p, shapes.n);
            pb_puts(&out, "</wpg:wgp></a:graphicData>");
            any = 1;
        } else {
            pb_put(&out, g, (size_t)(q - g));
        }

        pb_free(&shapes);
        p = q;
    }

    pb_put(&out, p, (size_t)(e - p));
    free(doc.rels);
    *rels = NULL;
    *rels_len = 0;

    if (!any || out.err || P.rels.err) {
        pb_free(&out);
        pb_free(&P.rels);
        return NULL;
    }

    if (P.rels.n) {     /* its pictures' relationships, for the document's */
        *rels = P.rels.p;
        *rels_len = P.rels.n;
    }

    pb_putc(&out, '\0');     /* (ended, as what it stands for is) */
    *out_len = out.n - 1;
    return out.p;
}
