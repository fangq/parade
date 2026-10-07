/*
 * pd_chart: a Word chart (a DrawingML chart part, c:chartSpace) drawn.
 *
 * Word stores no picture of a chart: what it shows is drawn from the part,
 * the series and the values they cache (c:numCache, c:strCache). The chart
 * here becomes the items of a Parade drawing -- boxes, paths and one-line
 * labels, in the drawing's coordinates -- the way Word lays it out by
 * default: the title above, the legend at the side it names, the axes'
 * labels outside the plot area, gridlines at a "nice" step.
 *
 * Drawn: bar and column charts (clustered, stacked, 100% stacked), line
 * and area charts, pie and doughnut charts, scatter charts; series and
 * point colours from the part or the theme's accent cycle; data labels;
 * axis titles (horizontal); number formats (#,##0.00, 0%, General). Not
 * drawn: 3-D perspective (a 3-D chart is drawn flat), secondary axes (all
 * groups share the primary one), trendlines, error bars, rotated text.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

#define CH_MAX_SER 32
#define CH_MAX_PT 512
#define CH_MAX_GRP 4

enum { CT_NONE, CT_BAR, CT_LINE, CT_AREA, CT_PIE, CT_DOUGHNUT, CT_SCATTER };
enum { CG_CLUSTERED, CG_STACKED, CG_PERCENT };

typedef struct {
    char name[96];
    double v[CH_MAX_PT], x[CH_MAX_PT];  /* values; a scatter's x */
    unsigned char has[CH_MAX_PT], hasx[CH_MAX_PT];
    int n;                              /* points: one past the highest index */
    uint32_t fill, line;                /* 0: not said */
    uint32_t pt_fill[64];               /* c:dPt, by index */
    int marker;                         /* -1 not said, 0 none, 1 some */
    int labels;                         /* c:dLbls showVal: -1 not said */
    char fmt[48];                       /* its values' number format (what its labels show them in) */
} cser;

typedef struct {
    int type, horiz, grouping, gap, overlap, vary, hole, first_angle, labels, percent_labels, lines, smooth;
    int marker;                         /* the group's c:marker (line charts) */
    cser* ser;
    int nser;
} cgrp;

typedef struct {
    cgrp grp[CH_MAX_GRP];
    int ngrp;
    char cat[CH_MAX_PT][48], cat2[CH_MAX_PT][48];  /* categories: the inner level and, multi-level, the outer */
    int ncat;
    char title[160];
    int has_title, title_deleted;
    char val_title[96], cat_title[96];
    char fmt[48];                       /* the value axis's number format */
    int legend, legend_pos;             /* legend_pos: 'r', 'l', 't', 'b' */
    int legend_overlay;
    double leg_ml[4], plot_ml[4];       /* manual layouts, fractions of the chart: x, y, w, h; -1 not said */
    int plot_inner, plot_edge, leg_edge;
    int grid, val_deleted, cat_deleted;
    int has_min, has_max;
    double vmin, vmax;
    pd_sp base;                         /* the text's size */
    const uint32_t* theme;
} chart;

/* ------------------------------------------------------------------ */
/* reading the part                                                   */
/* ------------------------------------------------------------------ */

static int is_group(const char* t, int* type) {
    static const struct {
        const char* n;
        int t;
    } g[] = { { "barChart", CT_BAR }, { "bar3DChart", CT_BAR }, { "lineChart", CT_LINE }, { "line3DChart", CT_LINE },
        { "areaChart", CT_AREA }, { "area3DChart", CT_AREA }, { "pieChart", CT_PIE }, { "pie3DChart", CT_PIE },
        { "ofPieChart", CT_PIE }, { "doughnutChart", CT_DOUGHNUT }, { "scatterChart", CT_SCATTER },
        { "bubbleChart", CT_SCATTER }, { "stockChart", CT_LINE }, { "radarChart", CT_LINE }
    };
    size_t i;

    for (i = 0; i < sizeof(g) / sizeof(g[0]); i++) {
        if (!strcmp(t, g[i].n)) {
            *type = g[i].t;
            return 1;
        }
    }

    return 0;
}

/* the element names open around the current one, innermost last */
typedef struct {
    char n[40][32];
    int d;
} cpath;

static int inside(const cpath* p, const char* name) {
    int i;

    for (i = 0; i < p->d; i++) {
        if (!strcmp(p->n[i], name)) {
            return 1;
        }
    }

    return 0;
}

static const char* parent(const cpath* p, int up) {
    return p->d - 1 - up >= 0 ? p->n[p->d - 1 - up] : "";
}

static void append(char* dst, size_t cap, const char* s, size_t n) {
    size_t k = strlen(dst);

    if (k + 1 < cap) {
        snprintf(dst + k, cap - k, "%.*s", (int)n, s);
    }
}

static int read_chart(chart* C, const char* xml, size_t n, cser* store) {
    pd_markup m;
    cpath P;
    cgrp* g = NULL;
    cser* s = NULL;
    int nstore = 0, pt = -1, dpt = -1, lvl = -1, in_title = 0, in_axis = 0, ty;
    uint32_t* clr = NULL;           /* the colour being read, for its modifiers */
    int clr_depth = 0;
    char v[128];

    memset(&P, 0, sizeof(P));
    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = m.type == MT_TEXT ? "" : mu_local(m.name);
        int open = m.type == MT_OPEN || m.type == MT_EMPTY;

        if (m.type == MT_CLOSE) {
            if (P.d > 0) {
                P.d--;
            }

            if (!strcmp(t, "ser")) {
                s = NULL;
            } else if (is_group(t, &ty) && g) {
                g = NULL;
            } else if (!strcmp(t, "title")) {
                in_title = 0;
            } else if (!strcmp(t, "valAx") || !strcmp(t, "catAx") || !strcmp(t, "dateAx") || !strcmp(t, "serAx")) {
                in_axis = 0;
            } else if (!strcmp(t, "dPt")) {
                dpt = -1;
            } else if (!strcmp(t, "lvl")) {
                lvl++;
            }

            if (clr && P.d < clr_depth) {
                clr = NULL;
            }

            pt = -1;
            continue;
        }

        if (m.type == MT_TEXT) {
            pd_buf txt;
            const char* here = parent(&P, 0);

            memset(&txt, 0, sizeof(txt));
            mu_decode(m.text, m.tlen, &txt);

            if (!txt.p) {
                continue;
            }

            if (!strcmp(here, "t") && in_title && !in_axis) {
                append(C->title, sizeof(C->title), txt.p, txt.n);
                C->has_title = 1;
            } else if (!strcmp(here, "t") && in_title && in_axis == 'v') {
                append(C->val_title, sizeof(C->val_title), txt.p, txt.n);
            } else if (!strcmp(here, "t") && in_title && in_axis == 'c') {
                append(C->cat_title, sizeof(C->cat_title), txt.p, txt.n);
            } else if (!strcmp(here, "formatCode") && s && inside(&P, "val") && !s->fmt[0]) {
                snprintf(s->fmt, sizeof(s->fmt), "%.*s", (int)(txt.n < 47 ? txt.n : 47), txt.p);
            } else if (!strcmp(here, "v") && s) {
                if (inside(&P, "tx") && !inside(&P, "cat") && !inside(&P, "val")) {
                    snprintf(s->name, sizeof(s->name), "%.*s", (int)txt.n, txt.p);
                } else if (pt >= 0 && pt < CH_MAX_PT && (inside(&P, "cat") || inside(&P, "xVal")) &&
                           g && g->type != CT_SCATTER) {
                    if (lvl <= 0) {
                        snprintf(C->cat[pt], sizeof(C->cat[pt]), "%.*s", (int)txt.n, txt.p);
                    } else if (lvl == 1) {
                        snprintf(C->cat2[pt], sizeof(C->cat2[pt]), "%.*s", (int)txt.n, txt.p);
                    }

                    C->ncat = pt + 1 > C->ncat ? pt + 1 : C->ncat;
                } else if (pt >= 0 && pt < CH_MAX_PT && inside(&P, "xVal")) {
                    s->x[pt] = strtod(txt.p, NULL);
                    s->hasx[pt] = 1;
                    snprintf(C->cat[pt], sizeof(C->cat[pt]), "%.*s", (int)txt.n, txt.p);
                } else if (pt >= 0 && pt < CH_MAX_PT && (inside(&P, "val") || inside(&P, "yVal"))) {
                    s->v[pt] = strtod(txt.p, NULL);
                    s->has[pt] = 1;
                    s->n = pt + 1 > s->n ? pt + 1 : s->n;
                }
            }

            pb_free(&txt);
            continue;
        }

        if (m.type == MT_OPEN && P.d < 40) {
            snprintf(P.n[P.d++], sizeof(P.n[0]), "%s", t);
        }

        if (!open) {
            continue;
        }

        if (is_group(t, &ty) && C->ngrp < CH_MAX_GRP) {
            g = &C->grp[C->ngrp++];
            memset(g, 0, sizeof(*g));
            g->type = ty;
            g->gap = 150;
            g->hole = 50;
            g->labels = -1;
            g->marker = -1;
            g->ser = store + nstore;
            g->lines = 1;
        } else if (!strcmp(t, "ser") && g && nstore < CH_MAX_SER) {
            s = &store[nstore++];
            memset(s, 0, sizeof(*s));
            s->marker = -1;
            s->labels = -1;
            g->nser++;
            lvl = -1;
        } else if (!strcmp(t, "lvl")) {
            lvl = lvl < 0 ? 0 : lvl;
        } else if (!strcmp(t, "pt") && mu_attr(&m, "idx", v, sizeof(v))) {
            pt = atoi(v);
        } else if (!strcmp(t, "idx") && !strcmp(parent(&P, m.type == MT_OPEN ? 1 : 0), "dPt") && mu_attr(&m, "val", v,
                   sizeof(v))) {
            dpt = atoi(v);
        } else if (!strcmp(t, "barDir") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->horiz = !strcmp(v, "bar");
        } else if (!strcmp(t, "grouping") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->grouping = !strcmp(v, "stacked") ? CG_STACKED : !strcmp(v, "percentStacked") ? CG_PERCENT : CG_CLUSTERED;
        } else if (!strcmp(t, "gapWidth") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->gap = atoi(v);
        } else if (!strcmp(t, "overlap") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->overlap = atoi(v);
        } else if (!strcmp(t, "varyColors") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->vary = !strcmp(v, "1") || !strcmp(v, "true");
        } else if (!strcmp(t, "holeSize") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->hole = atoi(v);
        } else if (!strcmp(t, "firstSliceAng") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->first_angle = atoi(v);
        } else if (!strcmp(t, "scatterStyle") && g && mu_attr(&m, "val", v, sizeof(v))) {
            g->lines = strcmp(v, "marker") != 0;
            g->smooth = strncmp(v, "smooth", 6) == 0;
        } else if (!strcmp(t, "marker") && mu_attr(&m, "val", v, sizeof(v)) && g && !s) {
            g->marker = !strcmp(v, "1") || !strcmp(v, "true");
        } else if (!strcmp(t, "symbol") && s && mu_attr(&m, "val", v, sizeof(v))) {
            s->marker = strcmp(v, "none") != 0;
        } else if (!strcmp(t, "showVal") && mu_attr(&m, "val", v, sizeof(v)) && !inside(&P, "dLbl")) {
            int on = !strcmp(v, "1") || !strcmp(v, "true");

            if (s) {
                s->labels = on;
            } else if (g) {
                g->labels = on;
            }
        } else if (!strcmp(t, "showPercent") && g && mu_attr(&m, "val", v, sizeof(v)) && !inside(&P, "dLbl")) {
            g->percent_labels |= !strcmp(v, "1") || !strcmp(v, "true");
        } else if (!strcmp(t, "title")) {
            in_title = 1;

            if (!in_axis) {
                C->has_title = 1;
            }
        } else if (!strcmp(t, "autoTitleDeleted") && mu_attr(&m, "val", v, sizeof(v))) {
            C->title_deleted = !strcmp(v, "1") || !strcmp(v, "true");
        } else if (!strcmp(t, "valAx")) {
            in_axis = 'v';
        } else if (!strcmp(t, "catAx") || !strcmp(t, "dateAx") || !strcmp(t, "serAx")) {
            in_axis = 'c';
        } else if (!strcmp(t, "delete") && in_axis && mu_attr(&m, "val", v, sizeof(v))) {
            int del = !strcmp(v, "1") || !strcmp(v, "true");

            if (in_axis == 'v') {
                C->val_deleted = del;
            } else {
                C->cat_deleted = del;
            }
        } else if (!strcmp(t, "majorGridlines") && in_axis == 'v') {
            C->grid = 1;
        } else if (!strcmp(t, "numFmt") && s && inside(&P, "dLbls") && mu_attr(&m, "formatCode", v, sizeof(v))) {
            snprintf(s->fmt, sizeof(s->fmt), "%.47s", v);
        } else if (!strcmp(t, "numFmt") && in_axis == 'v' && mu_attr(&m, "formatCode", v, sizeof(v))) {
            snprintf(C->fmt, sizeof(C->fmt), "%.47s", v);
        } else if (!strcmp(t, "min") && in_axis == 'v' && mu_attr(&m, "val", v, sizeof(v))) {
            C->vmin = strtod(v, NULL);
            C->has_min = 1;
        } else if (!strcmp(t, "max") && in_axis == 'v' && mu_attr(&m, "val", v, sizeof(v))) {
            C->vmax = strtod(v, NULL);
            C->has_max = 1;
        } else if ((!strcmp(t, "x") || !strcmp(t, "y") || !strcmp(t, "w") || !strcmp(t, "h")) &&
                   !strcmp(parent(&P, m.type == MT_OPEN ? 1 : 0), "manualLayout") && mu_attr(&m, "val", v, sizeof(v)) &&
                   !inside(&P, "dLbl") && !inside(&P, "dLbls") && !inside(&P, "title") && !inside(&P, "trendlineLbl")) {
            int k2 = t[0] == 'x' ? 0 : t[0] == 'y' ? 1 : t[0] == 'w' ? 2 : 3;

            if (inside(&P, "legend")) {
                C->leg_ml[k2] = strtod(v, NULL);
            } else if (inside(&P, "plotArea")) {
                C->plot_ml[k2] = strtod(v, NULL);
            }
        } else if (!strcmp(t, "layoutTarget") && inside(&P, "plotArea") && mu_attr(&m, "val", v, sizeof(v))) {
            C->plot_inner = !strcmp(v, "inner");
        } else if ((!strcmp(t, "xMode") || !strcmp(t, "yMode")) && mu_attr(&m, "val", v, sizeof(v)) &&
                   !inside(&P, "dLbl") && !inside(&P, "title")) {
            if (inside(&P, "legend")) {
                C->leg_edge = !strcmp(v, "edge");
            } else if (inside(&P, "plotArea")) {
                C->plot_edge = !strcmp(v, "edge");
            }
        } else if (!strcmp(t, "overlay") && inside(&P, "legend") && mu_attr(&m, "val", v, sizeof(v))) {
            C->legend_overlay = !strcmp(v, "1") || !strcmp(v, "true");
        } else if (!strcmp(t, "legend")) {
            C->legend = 1;
            C->legend_pos = 'r';
        } else if (!strcmp(t, "legendPos") && mu_attr(&m, "val", v, sizeof(v))) {
            C->legend_pos = v[0] == 't' && v[1] == 'r' ? 'r' : v[0];
        } else if (!strcmp(t, "defRPr") && !in_title && !s && !g && mu_attr(&m, "sz", v, sizeof(v)) &&
                   atoi(v) >= 400 && atoi(v) <= 4000 && inside(&P, "txPr") && P.d <= 6) {
            C->base = (pd_sp)((int64_t)atoi(v) * PD_SP_PER_PT / 100);  /* the chart's own text size */
        } else if (!strcmp(t, "srgbClr") || !strcmp(t, "schemeClr") || !strcmp(t, "sysClr")) {
            uint32_t c = 0xFF000000u;
            uint32_t* tgt = NULL;

            if (!strcmp(t, "schemeClr") && mu_attr(&m, "val", v, sizeof(v)) && pd_conv_theme_slot(v) >= 0) {
                c = C->theme[pd_conv_theme_slot(v)];
            } else if (mu_attr(&m, !strcmp(t, "sysClr") ? "lastClr" : "val", v, sizeof(v))) {
                c = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
            }

            /* a series' (or a point's) fill, or its line: what the colour is of */
            if (s && inside(&P, "spPr") && !inside(&P, "dLbls") && !inside(&P, "marker") && !inside(&P, "trendline") &&
                    !inside(&P, "errBars")) {
                int in_ln = inside(&P, "ln");

                if (dpt >= 0 && dpt < 64 && !in_ln && inside(&P, "dPt")) {
                    tgt = &s->pt_fill[dpt];
                } else if (!inside(&P, "dPt")) {
                    tgt = in_ln ? &s->line : &s->fill;
                }
            }

            if (tgt && (inside(&P, "solidFill") || (inside(&P, "gradFill") && !*tgt))) {
                *tgt = c;
                clr = m.type == MT_OPEN ? tgt : NULL;
                clr_depth = P.d;
            }
        } else if (clr && (!strcmp(t, "lumMod") || !strcmp(t, "lumOff") || !strcmp(t, "tint") || !strcmp(t, "shade"))) {
            *clr = pd_conv_clr_modify(*clr, t, &m);
        }
    }

    return C->ngrp;
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_buf* o;
    int first;
    const char* font;
} cout;

static void sep(cout* w) {
    if (!w->first) {
        pb_putc(w->o, ',');
    }

    w->first = 0;
}

static void box(cout* w, double x, double y, double bw, double bh, uint32_t fill, uint32_t line, double lw) {
    if (bw < 0) {
        x += bw;
        bw = -bw;
    }

    if (bh < 0) {
        y += bh;
        bh = -bh;
    }

    sep(w);
    pb_printf(w->o, "{\"shape\":\"rect\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d}",
              (int)x, (int)y, (int)bw, (int)bh, (unsigned)fill, (unsigned)line, (int)lw);
}

static void path(cout* w, const double* xy, int n, int closed, uint32_t fill, uint32_t line, double lw) {
    int i;

    if (n < 2) {
        return;
    }

    sep(w);
    pb_puts(w->o, "{\"path\":[");

    for (i = 0; i < n; i++) {
        pb_printf(w->o, "%s%d,%d", i ? "," : "", (int)xy[2 * i], (int)xy[2 * i + 1]);
    }

    pb_printf(w->o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d}", closed, (unsigned)fill, (unsigned)line, (int)lw);
}

static void hline(cout* w, double x0, double x1, double y, uint32_t c, double lw) {
    double xy[4] = { x0, y, x1, y };

    path(w, xy, 2, 0, 0, c, lw);
}

static void vline(cout* w, double x, double y0, double y1, uint32_t c, double lw) {
    double xy[4] = { x, y0, x, y1 };

    path(w, xy, 2, 0, 0, c, lw);
}

/* a line of text: its baseline at y; ha 0 from x, 1 centred on it, 2 ending at it */
static void label(cout* w, const char* s, double x, double y, pd_sp sz, int ha, uint32_t c, int bold) {
    size_t i, n = strlen(s);

    if (!n) {
        return;
    }

    sep(w);
    pb_puts(w->o, "{\"label\":\"");

    for (i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];

        if (ch == '"' || ch == '\\') {
            pb_putc(w->o, '\\');
            pb_putc(w->o, (char)ch);
        } else if (ch < 32) {
            pb_putc(w->o, ' ');
        } else {
            pb_putc(w->o, (char)ch);
        }
    }

    pb_printf(w->o, "\",\"x\":%d,\"y\":%d,\"sz\":%d,\"ha\":%d,\"c\":%u", (int)x, (int)y, (int)sz, ha, (unsigned)c);

    if (bold) {
        pb_puts(w->o, ",\"w\":700");
    }

    if (w->font && w->font[0]) {
        pb_puts(w->o, ",\"f\":\"");

        for (i = 0; w->font[i]; i++) {
            if (w->font[i] != '"' && w->font[i] != '\\') {
                pb_putc(w->o, w->font[i]);
            }
        }

        pb_putc(w->o, '"');
    }

    pb_putc(w->o, '}');
}

/* how wide a line of text is about to be: half its size a character (counted as UTF-8 lead bytes) */
static double text_w(const char* s, pd_sp sz) {
    double n = 0;

    for (; *s; s++) {
        n += ((unsigned char)*s & 0xC0) != 0x80 ? (((unsigned char)*s >= 0xE0) ? 1.0 : 0.55) : 0;
    }

    return n * sz;
}

/* the colour of series (or point) k in Office's cycle: the six accents, then darker, then lighter */
static uint32_t cycle(const chart* C, int k) {
    uint32_t c = C->theme[4 + k % 6];
    int round = k / 6;
    double h_mul = round == 1 ? 0.6 : round == 2 ? 0.8 : round == 3 ? 0.5 : 1;

    if (round == 0) {
        return c;
    }

    {
        /* through lumMod: the same modifier the parts use */
        pd_markup fake;
        char attrs[48];

        snprintf(attrs, sizeof(attrs), " val=\"%d\"", (int)(h_mul * 100000));
        memset(&fake, 0, sizeof(fake));
        fake.attrs = attrs;
        fake.alen = strlen(attrs);
        return pd_conv_clr_modify(c, "lumMod", &fake);
    }
}

/* a number as the format code says: decimals after the point, thousands grouped, a percentage */
static void fmt_num(double v, const char* code, char* out, size_t cap) {
    const char* dot, *end;
    int dec = 0, group = 0, pct = 0;
    char raw[64];
    char* p;

    if (!code || !code[0] || !strcmp(code, "General")) {
        if (fabs(v - floor(v + 0.5)) < 1e-9 * (fabs(v) + 1)) {
            snprintf(out, cap, "%.0f", v);
        } else {
            snprintf(out, cap, "%.6g", v);
        }

        return;
    }

    end = strchr(code, ';');
    end = end ? end : code + strlen(code);
    pct = memchr(code, '%', (size_t)(end - code)) != NULL;
    dot = memchr(code, '.', (size_t)(end - code));

    for (p = (char*)(dot ? dot + 1 : end); dot && p < end && (*p == '0' || *p == '#'); p++) {
        dec++;
    }

    {
        const char* c;

        for (c = code; c < (dot ? dot : end); c++) {
            group |= *c == ',';
        }
    }

    dec = dec > 10 ? 10 : dec;
    snprintf(raw, sizeof(raw), "%.*f", dec, pct ? v * 100 : v);

    if (group) {    /* digits grouped by three, a comma between */
        char tmp[96];
        char* d = strchr(raw, '.');
        int lead = (int)(d ? d - raw : (long)strlen(raw)), i, k = 0, start = raw[0] == '-';

        for (i = 0; i < lead; i++) {
            tmp[k++] = raw[i];

            if (i >= start && i < lead - 1 && (lead - 1 - i) % 3 == 0) {
                tmp[k++] = ',';
            }
        }

        snprintf(tmp + k, sizeof(tmp) - (size_t)k, "%.16s", d ? d : "");
        snprintf(out, cap, "%.60s%s", tmp, pct ? "%" : "");
    } else {
        snprintf(out, cap, "%.60s%s", raw, pct ? "%" : "");
    }
}

/* an axis from lo to hi at a step of 1, 2 or 5 times a power of ten, about five of them */
static void nice(double lo, double hi, double* a, double* b, double* step) {
    double range, raw, mag, nrm;

    if (hi <= lo) {
        hi = lo + (lo == 0 ? 1 : fabs(lo));
    }

    range = hi - lo;
    raw = range / 8;    /* about eight steps, as Excel takes */
    mag = pow(10, floor(log10(raw)));
    nrm = raw / mag;
    *step = (nrm < 1.5 ? 1 : nrm < 3 ? 2 : nrm < 7 ? 5 : 10) * mag;
    *a = floor(lo / *step + 1e-9) * *step;
    *b = ceil(hi / *step - 1e-9) * *step;

    if (*b <= *a) {
        *b = *a + *step;
    }
}

static int ncats(const chart* C) {
    int i, j, n = C->ncat;

    for (i = 0; i < C->ngrp; i++) {
        for (j = 0; j < C->grp[i].nser; j++) {
            n = C->grp[i].ser[j].n > n ? C->grp[i].ser[j].n : n;
        }
    }

    return n > CH_MAX_PT ? CH_MAX_PT : n;
}

/* the series' fill: its own, else the cycle's */
static uint32_t ser_fill(const chart* C, const cser* s, int k) {
    return s->fill ? s->fill : s->line ? s->line : cycle(C, k);
}

static uint32_t ser_line(const chart* C, const cser* s, int k) {
    return s->line ? s->line : s->fill ? s->fill : cycle(C, k);
}

static void draw_pie(const chart* C, const cgrp* g, cout* w, double px, double py, double pw, double ph) {
    const cser* s = g->nser ? &g->ser[0] : NULL;
    double total = 0, a0, cx = px + pw / 2, cy = py + ph / 2, r = (pw < ph ? pw : ph) / 2 * 0.92;
    double ri = g->type == CT_DOUGHNUT ? r * g->hole / 100.0 : 0;
    int i, k;

    if (!s) {
        return;
    }

    for (i = 0; i < s->n; i++) {
        total += s->has[i] && s->v[i] > 0 ? s->v[i] : 0;
    }

    if (total <= 0) {
        return;
    }

    a0 = g->first_angle;

    for (i = 0; i < s->n; i++) {
        double v = s->has[i] && s->v[i] > 0 ? s->v[i] : 0, sweep = v / total * 360, xy[2 * 2 * 130];
        int steps = (int)ceil(sweep / 3) + 1, np = 0;
        uint32_t c = i < 64 && s->pt_fill[i] ? s->pt_fill[i] : cycle(C, i);

        if (v <= 0) {
            continue;
        }

        steps = steps > 128 ? 128 : steps;

        if (ri <= 0) {
            xy[np++] = cx;
            xy[np++] = cy;
        }

        for (k = 0; k <= steps; k++) {     /* clockwise from twelve o'clock */
            double t = (a0 + sweep * k / steps - 90) * 3.141592653589793 / 180;

            xy[np++] = cx + r * cos(t);
            xy[np++] = cy + r * sin(t);
        }

        for (k = steps; ri > 0 && k >= 0; k--) {
            double t = (a0 + sweep * k / steps - 90) * 3.141592653589793 / 180;

            xy[np++] = cx + ri * cos(t);
            xy[np++] = cy + ri * sin(t);
        }

        path(w, xy, np / 2, 1, c, 0xFFFFFFFFu, PD_SP_PER_PT);

        if (g->labels > 0 || s->labels > 0 || g->percent_labels) {
            double t = (a0 + sweep / 2 - 90) * 3.141592653589793 / 180, lr = ri > 0 ? (r + ri) / 2 : r * 0.65;
            char txt[48];

            if (g->percent_labels) {
                snprintf(txt, sizeof(txt), "%.0f%%", v / total * 100);
            } else {
                fmt_num(v, C->fmt, txt, sizeof(txt));
            }

            label(w, txt, cx + lr * cos(t), cy + lr * sin(t) + C->base * 0.35, C->base, 1, 0xFF404040u, 0);
        }

        a0 += sweep;
    }
}

/* a name in one line, or two split at the last space that leaves the first within cap */
static void wrap2(const char* s, double cap, pd_sp sz, char* a, char* b, size_t n) {
    const char* sp = NULL, *p;

    snprintf(a, n, "%s", s);
    b[0] = '\0';

    if (text_w(s, sz) <= cap) {
        return;
    }

    for (p = s; *p; p++) {
        if (*p == ' ') {
            char head[96];

            snprintf(head, sizeof(head), "%.*s", (int)(p - s), s);

            if (text_w(head, sz) <= cap || !sp) {
                sp = p;
            }
        }
    }

    if (sp) {
        snprintf(a, n, "%.*s", (int)(sp - s), s);
        snprintf(b, n, "%s", sp + 1);
    }
}

int pd_chart_items(const char* xml, size_t n, pd_sp W, pd_sp H, const uint32_t* theme, const char* font, pd_buf* o) {
    chart* C = (chart*)calloc(1, sizeof(chart));
    cser* store = (cser*)calloc(CH_MAX_SER, sizeof(cser));
    cout w;
    double pad, px, py, pw, ph, lo = 0, hi = 0, a, b, step, sz, t;
    int i, j, k, nc, horiz = 0, any = 0, pie = 0, scatter = 0, nleg = 0;
    double xlo = 0, xhi = 0, xa = 0, xb = 1, xstep = 1;
    const uint32_t grid = 0xFFD9D9D9u, axis = 0xFFBFBFBFu, ink = 0xFF595959u;
    char txt[64];
    size_t start = o->n;

    if (!C || !store || !theme) {
        free(C);
        free(store);
        return 0;
    }

    C->theme = theme;
    C->base = PD_PT(10);

    for (i = 0; i < 4; i++) {
        C->leg_ml[i] = C->plot_ml[i] = -1;
    }

    if (!read_chart(C, xml, n, store)) {
        free(C);
        free(store);
        return 0;
    }

    nc = ncats(C);
    pie = C->grp[0].type == CT_PIE || C->grp[0].type == CT_DOUGHNUT;
    scatter = C->grp[0].type == CT_SCATTER;
    horiz = C->grp[0].type == CT_BAR && C->grp[0].horiz;
    sz = (double)C->base;
    w.o = o;
    w.first = 1;
    w.font = font;
    pad = sz * 0.8;
    px = pad;
    py = pad;
    pw = W - 2 * pad;
    ph = H - 2 * pad;

    /* the title, above everything */
    if (C->has_title && !C->title_deleted) {
        if (!C->title[0] && C->grp[0].nser == 1 && C->grp[0].ser[0].name[0]) {
            snprintf(C->title, sizeof(C->title), "%s", C->grp[0].ser[0].name);   /* an automatic one: the series' */
        }

        if (C->title[0]) {
            label(&w, C->title, W / 2, py + sz * 1.4, (pd_sp)(sz * 1.4), 1, ink, 0);
            py += sz * 2.2;
            ph -= sz * 2.2;
        }
    }

    /* the legend, on its side: a coloured square and a name for each series (each point of a pie) */
    if (C->legend) {
        const char* names[CH_MAX_PT];
        uint32_t cols[CH_MAX_PT];
        double lw = 0, lx, ly, row = sz * 1.5;

        if (pie || C->grp[0].vary) {
            const cser* s = C->grp[0].nser ? &C->grp[0].ser[0] : NULL;

            for (i = 0; s && i < nc && nleg < CH_MAX_PT; i++) {
                names[nleg] = C->cat[i];
                cols[nleg++] = i < 64 && s->pt_fill[i] ? s->pt_fill[i] : cycle(C, i);
            }
        } else {
            for (i = 0, k = 0; i < C->ngrp; i++) {
                for (j = 0; j < C->grp[i].nser && nleg < CH_MAX_PT; j++, k++) {
                    const cser* s = &C->grp[i].ser[j];

                    names[nleg] = s->name[0] ? s->name : "Series";
                    cols[nleg++] = C->grp[i].type == CT_LINE || C->grp[i].type == CT_SCATTER ? ser_line(C, s, k) :
                                   ser_fill(C, s, k);
                }
            }
        }

        for (i = 0; i < nleg; i++) {
            t = text_w(names[i], C->base) + sz * 1.4;
            lw = t > lw ? t : lw;
        }

        if (C->leg_ml[0] >= 0 && C->leg_ml[1] >= 0 && C->leg_edge) {
            /* where it was put: over the plot, which keeps its room; names wrapped to its width */
            double cap = C->leg_ml[2] > 0 ? C->leg_ml[2] * W - sz * 1.4 : W * 0.4, yy;

            lx = C->leg_ml[0] * W;
            yy = C->leg_ml[1] * H + sz * 0.3;

            for (i = 0; i < nleg; i++) {
                char a1[96], a2[96];

                wrap2(names[i], cap, C->base, a1, a2, sizeof(a1));
                box(&w, lx + sz * 0.2, yy + row / 2 - sz * 0.35, sz * 0.7, sz * 0.7, cols[i], 0, 0);
                label(&w, a1, lx + sz * 1.2, yy + row / 2 + sz * 0.35, C->base, 0, ink, 0);

                if (a2[0]) {
                    yy += sz * 1.2;
                    label(&w, a2, lx + sz * 1.2, yy + row / 2 + sz * 0.35, C->base, 0, ink, 0);
                }

                yy += row;
            }
        } else if (C->legend_pos == 'b' || C->legend_pos == 't') {
            double total = 0;

            for (i = 0; i < nleg; i++) {
                total += text_w(names[i], C->base) + sz * 2.4;
            }

            lx = W / 2 - total / 2;
            ly = C->legend_pos == 'b' ? py + ph - row : py;

            for (i = 0; i < nleg; i++) {
                box(&w, lx, ly + row / 2 - sz * 0.35, sz * 0.7, sz * 0.7, cols[i], 0, 0);
                label(&w, names[i], lx + sz, ly + row / 2 + sz * 0.35, C->base, 0, ink, 0);
                lx += text_w(names[i], C->base) + sz * 2.4;
            }

            if (C->legend_overlay) {
                ;
            } else if (C->legend_pos == 'b') {
                ph -= row + sz * 0.5;
            } else {
                py += row + sz * 0.5;
                ph -= row + sz * 0.5;
            }
        } else {
            double cap = W * 0.3, yy;
            int lines = 0;

            lw = lw > cap + sz * 1.4 ? cap + sz * 1.4 : lw;

            for (i = 0; i < nleg; i++) {
                char a1[96], a2[96];

                wrap2(names[i], cap, C->base, a1, a2, sizeof(a1));
                lines += a2[0] ? 2 : 1;
            }

            lx = C->legend_pos == 'l' ? px : px + pw - lw;
            yy = py + ph / 2 - (row * nleg + sz * 1.2 * (lines - nleg)) / 2;

            for (i = 0; i < nleg; i++) {
                char a1[96], a2[96];

                wrap2(names[i], cap, C->base, a1, a2, sizeof(a1));
                box(&w, lx, yy + row / 2 - sz * 0.35, sz * 0.7, sz * 0.7, cols[i], 0, 0);
                label(&w, a1, lx + sz, yy + row / 2 + sz * 0.35, C->base, 0, ink, 0);

                if (a2[0]) {
                    yy += sz * 1.2;
                    label(&w, a2, lx + sz, yy + row / 2 + sz * 0.35, C->base, 0, ink, 0);
                }

                yy += row;
            }

            if (!C->legend_overlay) {
                if (C->legend_pos == 'l') {
                    px += lw + sz;
                }

                pw -= lw + sz;
            }
        }
    }

    if (pie) {
        draw_pie(C, &C->grp[0], &w, px, py, pw, ph);
        free(C);
        free(store);
        return o->n > start;
    }

    /* the value scale: over every series, stacked ones summed, from zero */
    for (i = 0; i < C->ngrp; i++) {
        const cgrp* g = &C->grp[i];

        for (k = 0; k < nc; k++) {
            double pos = 0, neg = 0;

            for (j = 0; j < g->nser; j++) {
                const cser* s = &g->ser[j];
                double v = k < s->n && s->has[k] ? s->v[k] : 0;

                if (!(k < s->n && s->has[k])) {
                    continue;
                }

                if (g->grouping != CG_CLUSTERED && (g->type == CT_BAR || g->type == CT_AREA || g->type == CT_LINE)) {
                    if (v >= 0) {
                        pos += v;
                    } else {
                        neg += v;
                    }
                } else {
                    hi = !any || v > hi ? v : hi;
                    lo = !any || v < lo ? v : lo;
                }

                if (scatter && k < s->n && s->hasx[k]) {
                    xhi = !any || s->x[k] > xhi ? s->x[k] : xhi;
                    xlo = !any || s->x[k] < xlo ? s->x[k] : xlo;
                }

                any = 1;
            }

            if (g->grouping == CG_PERCENT && (pos > 0 || neg < 0)) {
                hi = hi > 1 ? hi : 1;
                lo = neg < 0 && lo > -1 ? -1 : lo;
            } else if (g->grouping != CG_CLUSTERED) {
                hi = pos > hi ? pos : hi;
                lo = neg < lo ? neg : lo;
            }
        }
    }

    if (!any) {
        free(C);
        free(store);
        return 0;
    }

    lo = lo > 0 ? 0 : lo;
    hi = hi < 0 ? 0 : hi;
    nice(C->has_min ? C->vmin : lo, C->has_max ? C->vmax : hi, &a, &b, &step);
    a = C->has_min ? C->vmin : a;
    b = C->has_max ? C->vmax : b;

    if (C->grp[0].grouping == CG_PERCENT && !C->fmt[0]) {
        snprintf(C->fmt, sizeof(C->fmt), "0%%");
    }

    if (scatter) {
        nice(xlo < 0 ? xlo : (xlo > 0 && xlo < (xhi - xlo) ? 0 : xlo), xhi, &xa, &xb, &xstep);
    }

    /* room for the axes' labels and titles */
    {
        double vw = 0, cw = 0, lines = 1;

        for (t = a; t <= b + step * 1e-6; t += step) {
            fmt_num(t, C->fmt, txt, sizeof(txt));
            vw = text_w(txt, C->base) > vw ? text_w(txt, C->base) : vw;
        }

        for (k = 0; k < nc; k++) {
            cw = text_w(C->cat[k], C->base) > cw ? text_w(C->cat[k], C->base) : cw;
            cw = text_w(C->cat2[k], C->base) > cw ? text_w(C->cat2[k], C->base) : cw;
            lines = C->cat2[k][0] ? 2 : lines;
        }

        if (horiz) {    /* categories down the left, values along the bottom */
            double left = C->cat_deleted ? 0 : cw + sz * 0.8, bottom = C->val_deleted ? 0 : sz * 1.8;

            bottom += C->val_title[0] ? sz * 1.8 : 0;
            px += left;
            pw -= left;
            ph -= bottom;
        } else {        /* values up the left, categories (or a scatter's x) along the bottom */
            double left = C->val_deleted ? 0 : vw + sz * 0.8, bottom = C->cat_deleted ? 0 : sz * (1.2 + 1.2 * lines);

            if (scatter) {
                double xw = 0;

                for (t = xa; t <= xb + xstep * 1e-6; t += xstep) {
                    fmt_num(t, NULL, txt, sizeof(txt));
                    xw = text_w(txt, C->base) > xw ? text_w(txt, C->base) : xw;
                }

                bottom = sz * 2.4;
                pw -= xw / 2;
            }

            if (C->val_title[0]) {
                py += sz * 1.8;
                ph -= sz * 1.8;
            }

            bottom += C->cat_title[0] ? sz * 1.8 : 0;
            px += left;
            pw -= left;
            ph -= bottom;
        }
    }

    /* the plot area where it was put: the rectangle the bars are in (inner), or that and its labels (outer) */
    if (C->plot_ml[0] >= 0 && C->plot_ml[1] >= 0 && C->plot_ml[2] > 0 && C->plot_ml[3] > 0 && C->plot_edge &&
            C->plot_inner) {
        px = C->plot_ml[0] * W;
        py = C->plot_ml[1] * H;
        pw = C->plot_ml[2] * W;
        ph = C->plot_ml[3] * H;
    }

    /* no more value labels than there is room for: a larger step */
    {
        double vw = 0;
        int guard = 0;

        for (t = a; t <= b + step * 1e-6; t += step) {
            fmt_num(t, C->fmt, txt, sizeof(txt));
            vw = text_w(txt, C->base) > vw ? text_w(txt, C->base) : vw;
        }

        while (guard++ < 8 && ((horiz && ((b - a) / step + 1) * (vw + sz) > pw) || (!horiz && ((b - a) / step + 1) * sz * 1.4 >
                ph))) {
            double mag = pow(10, floor(log10(step))), nrm = step / mag;

            step = (nrm < 1.5 ? 2 : nrm < 3 ? 5 : 10) * mag;

            if (!C->has_min) {
                a = floor(a / step + 1e-9) * step;
            }

            if (!C->has_max) {
                b = ceil(b / step - 1e-9) * step;
            }
        }
    }

    if (pw < sz || ph < sz) {
        free(C);
        free(store);
        return o->n > start;
    }

#define VY(v) (py + ph * (b - (v)) / (b - a))   /* a value's place: up a column chart, along a bar chart */
#define VX(v) (px + pw * ((v) - a) / (b - a))
#define XX(v) (px + pw * ((v) - xa) / (xb - xa))

    /* gridlines and the value axis's labels */
    for (t = a; t <= b + step * 1e-6; t += step) {
        fmt_num(fabs(t) < step * 1e-9 ? 0 : t, C->fmt, txt, sizeof(txt));

        if (horiz) {
            if (C->grid) {
                vline(&w, VX(t), py, py + ph, grid, PD_SP_PER_PT * 0.75);
            }

            if (!C->val_deleted) {
                label(&w, txt, VX(t), py + ph + sz * 1.3, C->base, 1, ink, 0);
            }
        } else {
            if (C->grid) {
                hline(&w, px, px + pw, VY(t), grid, PD_SP_PER_PT * 0.75);
            }

            if (!C->val_deleted) {
                label(&w, txt, px - sz * 0.5, VY(t) + sz * 0.35, C->base, 2, ink, 0);
            }
        }
    }

    if (scatter) {  /* the x axis of a scatter chart: values too */
        for (t = xa; t <= xb + xstep * 1e-6; t += xstep) {
            fmt_num(fabs(t) < xstep * 1e-9 ? 0 : t, NULL, txt, sizeof(txt));
            label(&w, txt, XX(t), py + ph + sz * 1.3, C->base, 1, ink, 0);
        }
    }

    /* the category axis's line, at zero */
    if (horiz) {
        vline(&w, VX(a < 0 && b > 0 ? 0 : a), py, py + ph, axis, PD_SP_PER_PT * 0.75);
    } else {
        hline(&w, px, px + pw, VY(a < 0 && b > 0 ? 0 : a), axis, PD_SP_PER_PT * 0.75);
    }

    /* the categories: each in its slot, the first nearest the origin */
    for (k = 0; !scatter && !C->cat_deleted && k < nc; k++) {
        double slot = (horiz ? ph : pw) / (nc ? nc : 1);

        if (horiz) {
            double cy = py + ph - slot * (k + 0.5);

            if (C->cat2[k][0]) {
                label(&w, C->cat[k], px - sz * 0.5, cy - sz * 0.1, C->base, 2, ink, 0);
                label(&w, C->cat2[k], px - sz * 0.5, cy + sz * 1.0, C->base, 2, ink, 0);
            } else {
                label(&w, C->cat[k], px - sz * 0.5, cy + sz * 0.35, C->base, 2, ink, 0);
            }
        } else {
            double cx = px + slot * (k + 0.5);

            label(&w, C->cat[k], cx, py + ph + sz * 1.3, C->base, 1, ink, 0);

            if (C->cat2[k][0]) {
                label(&w, C->cat2[k], cx, py + ph + sz * 2.5, C->base, 1, ink, 0);
            }
        }
    }

    /* the axes' titles: across, the value one above its labels (Word turns it; this does not) */
    if (horiz) {
        if (C->val_title[0]) {
            label(&w, C->val_title, px + pw / 2, py + ph + sz * 3.0, C->base, 1, ink, 1);
        }

        if (C->cat_title[0]) {
            label(&w, C->cat_title, pad, py - sz * 0.4 > pad ? py - sz * 0.4 : pad + sz, C->base, 0, ink, 1);
        }
    } else {
        if (C->val_title[0]) {
            label(&w, C->val_title, pad, py - sz * 0.8, C->base, 0, ink, 1);
        }

        if (C->cat_title[0]) {
            label(&w, C->cat_title, px + pw / 2, H - pad, C->base, 1, ink, 1);
        }
    }

    /* the series */
    for (i = 0, k = 0; i < C->ngrp; i++) {
        const cgrp* g = &C->grp[i];
        double slot = (horiz ? ph : pw) / (nc ? nc : 1);
        double groupw = slot / (1 + g->gap / 100.0), stack_pos[CH_MAX_PT], stack_neg[CH_MAX_PT], sum[CH_MAX_PT];
        int nb = g->grouping == CG_CLUSTERED ? (g->nser ? g->nser : 1) : 1;
        double ov = g->grouping == CG_CLUSTERED ? g->overlap / 100.0 : 0, barw = groupw / (nb - (nb - 1) * ov);

        memset(stack_pos, 0, sizeof(stack_pos));
        memset(stack_neg, 0, sizeof(stack_neg));
        memset(sum, 0, sizeof(sum));

        for (j = 0; j < g->nser; j++) {
            for (int q = 0; q < nc && q < g->ser[j].n; q++) {
                sum[q] += g->ser[j].has[q] ? fabs(g->ser[j].v[q]) : 0;
            }
        }

        for (j = 0; j < g->nser; j++, k++) {
            const cser* s = &g->ser[j];
            uint32_t fill = ser_fill(C, s, g->vary ? 0 : k), line = ser_line(C, s, k);
            double xy[2 * CH_MAX_PT + 4];
            int np = 0, q, labels = s->labels >= 0 ? s->labels : g->labels > 0, at[CH_MAX_PT];

            for (q = 0; q < nc && q < s->n; q++) {
                double v = s->has[q] ? s->v[q] : 0, v0 = 0, v1 = v;

                if (!s->has[q]) {
                    continue;
                }

                if (g->grouping == CG_PERCENT && sum[q] > 0) {
                    v = v / sum[q];
                    v1 = v;
                }

                if (g->grouping != CG_CLUSTERED) {
                    double* acc = v >= 0 ? &stack_pos[q] : &stack_neg[q];

                    v0 = *acc;
                    v1 = *acc + v;
                    *acc = v1;
                }

                if (g->type == CT_BAR) {
                    double off = slot * q + (slot - groupw) / 2 + (g->grouping == CG_CLUSTERED ? j * barw * (1 - ov) : 0);
                    double z0 = v0 < a ? a : v0 > b ? b : v0, z1 = v1 < a ? a : v1 > b ? b : v1;
                    uint32_t c = g->vary || (q < 64 && s->pt_fill[q]) ? (q < 64 && s->pt_fill[q] ? s->pt_fill[q] :
                                 cycle(C, q)) : fill;

                    if (g->grouping == CG_CLUSTERED) {
                        z0 = a < 0 && b > 0 ? 0 : a;
                    }

                    if (horiz) {
                        double y0 = py + ph - off - barw;

                        box(&w, VX(z0), y0, VX(z1) - VX(z0), barw, c, 0, 0);

                        if (labels) {
                            fmt_num(s->v[q], s->fmt[0] ? s->fmt : C->fmt, txt, sizeof(txt));

                            if (g->grouping == CG_CLUSTERED) {
                                label(&w, txt, VX(z1) + sz * 0.3, y0 + barw / 2 + sz * 0.35, C->base, 0, ink, 0);
                            } else if (fabs(VX(z1) - VX(z0)) > text_w(txt, C->base) + sz * 0.3) {
                                label(&w, txt, (VX(z0) + VX(z1)) / 2, y0 + barw / 2 + sz * 0.35, C->base, 1, ink, 0);
                            }
                        }
                    } else {
                        double x0 = px + off;

                        box(&w, x0, VY(z1), barw, VY(z0) - VY(z1), c, 0, 0);

                        if (labels) {
                            fmt_num(s->v[q], s->fmt[0] ? s->fmt : C->fmt, txt, sizeof(txt));

                            if (g->grouping == CG_CLUSTERED) {
                                label(&w, txt, x0 + barw / 2, VY(z1) - sz * 0.3, C->base, 1, ink, 0);
                            } else if (fabs(VY(z0) - VY(z1)) > sz * 1.1 && barw > text_w(txt, C->base)) {
                                label(&w, txt, x0 + barw / 2, (VY(z0) + VY(z1)) / 2 + sz * 0.35, C->base, 1, ink, 0);
                            }
                        }
                    }
                } else {
                    at[np / 2] = q;
                    xy[np++] = scatter ? XX(s->hasx[q] ? s->x[q] : q + 1) : px + slot * (q + 0.5);
                    xy[np++] = VY(v1);
                }
            }

            if (g->type == CT_AREA && np >= 4) {     /* down to the axis, closed */
                double base = VY(a < 0 && b > 0 ? 0 : a);

                double last = xy[np - 2];

                xy[np++] = last;
                xy[np++] = base;
                xy[np++] = xy[0];
                xy[np++] = base;
                path(&w, xy, np / 2, 1, fill, 0, 0);
            } else if (g->type == CT_LINE || scatter) {
                int marks = s->marker >= 0 ? s->marker : g->marker != 0;

                if (g->lines && np >= 4) {
                    path(&w, xy, np / 2, 0, 0, line, PD_SP_PER_PT * 2.25);
                }

                for (q = 0; marks && q < np / 2; q++) {
                    double r = sz * 0.3;
                    double m[8] = { xy[2 * q] - r, xy[2 * q + 1], xy[2 * q], xy[2 * q + 1] - r, xy[2 * q] + r, xy[2 * q + 1],
                                    xy[2 * q], xy[2 * q + 1] + r
                                  };

                    path(&w, m, 4, 1, line, line, PD_SP_PER_PT * 0.75);
                }

                for (q = 0; labels && q < np / 2; q++) {
                    fmt_num(s->v[at[q]], s->fmt[0] ? s->fmt : C->fmt, txt, sizeof(txt));
                    label(&w, txt, xy[2 * q], xy[2 * q + 1] - sz * 0.6, C->base, 1, ink, 0);
                }
            }
        }
    }

#undef VY
#undef VX
#undef XX
    free(C);
    free(store);
    return o->n > start;
}
