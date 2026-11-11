/*
 * Parade PDF reader: annotations, form fields, page text and the
 * incremental update
 *
 * Every change is copy on write: an object changed is a new value put in
 * the cross-reference in place of the old, which is kept for undo. The
 * objects changed or made are what a save appends to the file as it was
 * (with a cross-reference section, or stream, pointing back to the last
 * one), so a signature or anything else in the file stays as it was.
 *
 * Annotations made here carry their appearance (12.5.5), so any viewer
 * shows them as Parade does; the same appearances are made, not kept, for
 * annotations in a file that have none.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pd_internal.h"
#include "pd_pdfr_dl.h"

int32_t pd_text_build(dlist* L, pd_pdfr_char* buf, int32_t cap, int32_t* map);

/* ------------------------------------------------------------------ */
/* undo                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t num;
    pxref before, after;
} uchange;

typedef struct {
    uchange* c;
    int32_t n, cap;
} urecord;

static urecord* cur_record(pdoc* d) {
    urecord* u = (urecord*)d->undo;

    return d->nundo > 0 ? &u[d->nundo - 1] : NULL;
}

/* a new step of undo (the steps undone are gone) */
static void edit_begin(pdoc* d) {
    urecord* u = (urecord*)d->undo;
    int32_t i;

    for (i = d->nundo; i < d->nundo + d->nredo; i++) {
        free(u[i].c);
    }

    d->nredo = 0;

    if (d->nundo >= d->capundo) {
        int32_t nc = d->capundo ? d->capundo * 2 : 16;
        urecord* q = (urecord*)realloc(u, sizeof(urecord) * (size_t)nc);

        if (!q) {
            return;
        }

        d->undo = q;
        u = q;
        d->capundo = nc;
    }

    memset(&u[d->nundo], 0, sizeof(urecord));
    d->nundo++;
}

static void refresh(pdoc* d) {
    pd_pages_load(d);
    d->changes++;
    d->modified = d->changes != d->saved_changes;
}

static void edit_end(pdoc* d) {
    urecord* r = cur_record(d);

    if (r && r->n == 0) {   /* nothing changed: no step */
        free(r->c);
        d->nundo--;
    }

    refresh(d);
}

/* an object's new value (recorded for undo) */
static void set_obj(pdoc* d, int32_t num, pv v) {
    urecord* r = cur_record(d);
    pxref* x;
    int32_t i;

    if (num <= 0 || num >= d->nxref) {
        return;
    }

    if (r) {
        for (i = 0; i < r->n && r->c[i].num != num; i++) {
        }

        if (i == r->n) {
            if (r->n >= r->cap) {
                int32_t nc = r->cap ? r->cap * 2 : 8;
                uchange* q = (uchange*)realloc(r->c, sizeof(uchange) * (size_t)nc);

                if (!q) {
                    return;
                }

                r->c = q;
                r->cap = nc;
            }

            pd_object(d, num);      /* the value before, parsed if it was not */
            r->c[r->n].num = num;
            r->c[r->n].before = d->xref[num];
            r->n++;
        }
    }

    x = &d->xref[num];
    x->obj = (pv*)pa_alloc(&d->arena, sizeof(pv));

    if (x->obj) {
        *x->obj = v;
    }

    if (x->type == 0) {
        x->type = 1;
        x->off = -1;
        x->gen = 0;
    }

    x->dirty = 1;

    if (r) {
        for (i = 0; i < r->n; i++) {
            if (r->c[i].num == num) {
                r->c[i].after = *x;
            }
        }
    }
}

static int32_t new_obj(pdoc* d, pv v) {
    int32_t num = d->nxref > 0 ? d->nxref : 1;
    pxref* q = (pxref*)realloc(d->xref, sizeof(pxref) * (size_t)(num + 1));

    if (!q) {
        return 0;
    }

    d->xref = q;
    memset(&d->xref[num], 0, sizeof(pxref));
    d->nxref = num + 1;
    set_obj(d, num, v);
    return num;
}

int32_t pd_pdfr_can_undo(const pd_pdfr* d) {
    return d && d->nundo > 0;
}

int32_t pd_pdfr_can_redo(const pd_pdfr* d) {
    return d && d->nredo > 0;
}

pd_status pd_pdfr_undo(pd_pdfr* d) {
    urecord* r;
    int32_t i;

    if (!d || d->nundo == 0) {
        return PD_ERR_STATE;
    }

    r = &((urecord*)d->undo)[d->nundo - 1];

    for (i = r->n - 1; i >= 0; i--) {
        d->xref[r->c[i].num] = r->c[i].before;
    }

    d->nundo--;
    d->nredo++;
    refresh(d);
    return PD_OK;
}

pd_status pd_pdfr_redo(pd_pdfr* d) {
    urecord* r;
    int32_t i;

    if (!d || d->nredo == 0) {
        return PD_ERR_STATE;
    }

    r = &((urecord*)d->undo)[d->nundo];

    for (i = 0; i < r->n; i++) {
        d->xref[r->c[i].num] = r->c[i].after;
    }

    d->nundo++;
    d->nredo--;
    refresh(d);
    return PD_OK;
}

void pd_edit_free(pdoc* d) {
    urecord* u = (urecord*)d->undo;
    int32_t i;

    for (i = 0; i < d->nundo + d->nredo; i++) {
        free(u[i].c);
    }

    free(u);
    free(d->fields);
    d->undo = NULL;
    d->fields = NULL;
}

/* ------------------------------------------------------------------ */
/* values                                                             */
/* ------------------------------------------------------------------ */

/* a dictionary's copy, its entries its own (a change to it changes nothing else) */
static pv dict_copy(pdoc* d, pv v) {
    int32_t n, i;
    pkv* e = pv_entries(v, &n);
    pv r = pv_dict(d, n);

    for (i = 0; i < n; i++) {
        r.u.d[i] = e[i];
    }

    r.n = n;
    return r;
}

static pv arr_of(pdoc* d, const double* v, int n) {
    pv a = pv_array(d, n);
    int i;

    for (i = 0; i < n; i++) {
        a.u.a[i] = pv_real(v[i]);
    }

    return a;
}

static pv rgb_arr(pdoc* d, uint32_t rgb) {
    double c[3] = { ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0 };

    return arr_of(d, c, 3);
}

static uint32_t arr_rgb(pdoc* d, pv a) {
    double c[4] = { 0, 0, 0, 0 };
    int32_t n, i;

    a = pd_resolve(d, a);

    if (a.t != PV_ARRAY) {
        return 0;
    }

    n = a.n < 4 ? a.n : 4;

    for (i = 0; i < n; i++) {
        pd_num(d, a.u.a[i], &c[i]);
    }

    if (n == 1) {
        c[1] = c[2] = c[0];
    } else if (n == 4) {
        double k = c[3];

        c[0] = (1 - c[0]) * (1 - k);
        c[1] = (1 - c[1]) * (1 - k);
        c[2] = (1 - c[2]) * (1 - k);
    }

    return ((uint32_t)(c[0] * 255 + 0.5) << 16) | ((uint32_t)(c[1] * 255 + 0.5) << 8) | (uint32_t)(c[2] * 255 + 0.5);
}

/* a stream made now: its dictionary and data (deflated when that helps) */
static pv make_stream(pdoc* d, pv dict, const unsigned char* data, size_t n) {
    pstream* st = (pstream*)pa_alloc(&d->arena, sizeof(pstream));
    pv v = pv_null();
    unsigned char* z = NULL;
    size_t zn = 0;

    if (n > 128 && pd_deflate(data, n, 1, &z, &zn) == 0 && zn < n) {
        pd_dict_set(d, &dict, "Filter", pv_name(d, "FlateDecode"));
        data = z;
        n = zn;
    }

    st->data = (const uint8_t*)pa_strdup(&d->arena, (const char*)data, n);
    st->len = (int64_t)n;
    st->d = dict.u.d;
    st->nd = dict.n;
    free(z);
    v.t = PV_STREAM;
    v.u.st = st;
    pd_dict_set(d, &v, "Length", pv_int((int64_t)n));
    return v;
}

/* ------------------------------------------------------------------ */
/* coordinates                                                        */
/* ------------------------------------------------------------------ */

/* the page as shown to user space */
static void page_inv(pdoc* d, int32_t page, double* m) {
    double b[6];

    pd_page_base(d, page, b);

    if (!mat_inv(b, m)) {
        m[0] = m[3] = 1;
        m[1] = m[2] = m[4] = m[5] = 0;
    }
}

static void to_user(pdoc* d, int32_t page, double x, double y, double* ux, double* uy) {
    double m[6];

    page_inv(d, page, m);
    mat_pt(m, x, y, ux, uy);
}

/* a user-space rectangle shown: x0 y0 x1 y1, top left first */
static void rect_shown(pdoc* d, int32_t page, const double* r, double* o) {
    double b[6], xs[4], ys[4];
    int k;

    pd_page_base(d, page, b);
    mat_pt(b, r[0], r[1], &xs[0], &ys[0]);
    mat_pt(b, r[2], r[1], &xs[1], &ys[1]);
    mat_pt(b, r[2], r[3], &xs[2], &ys[2]);
    mat_pt(b, r[0], r[3], &xs[3], &ys[3]);
    o[0] = o[2] = xs[0];
    o[1] = o[3] = ys[0];

    for (k = 1; k < 4; k++) {
        o[0] = xs[k] < o[0] ? xs[k] : o[0];
        o[1] = ys[k] < o[1] ? ys[k] : o[1];
        o[2] = xs[k] > o[2] ? xs[k] : o[2];
        o[3] = ys[k] > o[3] ? ys[k] : o[3];
    }
}

/* ------------------------------------------------------------------ */
/* appearances                                                        */
/* ------------------------------------------------------------------ */

static void col_op(pbuf* b, uint32_t rgb, const char* op) {
    qb_num(b, ((rgb >> 16) & 255) / 255.0);
    qb_puts(b, " ");
    qb_num(b, ((rgb >> 8) & 255) / 255.0);
    qb_puts(b, " ");
    qb_num(b, (rgb & 255) / 255.0);
    qb_printf(b, " %s\n", op);
}

static void pt(pbuf* b, double x, double y, const char* op) {
    qb_num(b, x);
    qb_puts(b, " ");
    qb_num(b, y);
    qb_printf(b, " %s\n", op);
}

static void nums(pbuf* b, const double* v, int n) {
    int i;

    for (i = 0; i < n; i++) {
        qb_num(b, v[i]);
        qb_puts(b, " ");
    }
}

static void curve(pbuf* b, double x1, double y1, double x2, double y2, double x3, double y3) {
    double v[6] = { x1, y1, x2, y2, x3, y3 };

    nums(b, v, 6);
    qb_puts(b, "c\n");
}

static void ellipse(pbuf* b, double x0, double y0, double x1, double y1) {
    double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, rx = (x1 - x0) / 2, ry = (y1 - y0) / 2, k = 0.5523;

    pt(b, cx + rx, cy, "m");
    curve(b, cx + rx, cy + ry * k, cx + rx * k, cy + ry, cx, cy + ry);
    curve(b, cx - rx * k, cy + ry, cx - rx, cy + ry * k, cx - rx, cy);
    curve(b, cx - rx, cy - ry * k, cx - rx * k, cy - ry, cx, cy - ry);
    curve(b, cx + rx * k, cy - ry, cx + rx, cy - ry * k, cx + rx, cy);
    qb_puts(b, "h\n");
}

static void rect_op(pbuf* b, double x, double y, double w, double h, const char* after) {
    double v[4] = { x, y, w, h };

    nums(b, v, 4);
    qb_printf(b, "re %s\n", after);
}

static void pdf_string(pbuf* b, const unsigned char* s, size_t n) {
    size_t i;

    qb_puts(b, "(");

    for (i = 0; i < n; i++) {
        unsigned char c = s[i];

        if (c == '(' || c == ')' || c == '\\') {
            qb_put(b, "\\", 1);
            qb_put(b, &c, 1);
        } else if (c < 32 || c > 126) {
            qb_printf(b, "\\%03o", c);
        } else {
            qb_put(b, &c, 1);
        }
    }

    qb_puts(b, ")");
}

/* a standard 14 font as a resource dictionary (WinAnsi) */
static pv std_font(pdoc* d, const char* name) {
    pv f = pv_dict(d, 4);

    pd_dict_set(d, &f, "Type", pv_name(d, "Font"));
    pd_dict_set(d, &f, "Subtype", pv_name(d, "Type1"));
    pd_dict_set(d, &f, "BaseFont", pv_name(d, name));
    pd_dict_set(d, &f, "Encoding", pv_name(d, "WinAnsiEncoding"));
    return f;
}

/* Helvetica as an annotation's or field's font */
static pv helv_font(pdoc* d) {
    return std_font(d, "Helvetica");
}

/* text in a font, a code a character it has ('?' for one it has not) */
static size_t encode_in(pfont* f, const char* utf8, unsigned char* out, size_t cap) {
    const unsigned char* p = (const unsigned char*)utf8;
    size_t k = 0;

    while (*p && k + 4 < cap) {
        char one[8];
        int len = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 1, j;
        size_t got;

        for (j = 0; j < len && p[j]; j++) {
            one[j] = (char)p[j];
        }

        one[j] = 0;
        got = f ? pf_encode(f, one, out + k, cap - k) : 0;

        if (got == 0) {
            size_t q = f ? pf_encode(f, "?", out + k, cap - k) : 0;

            if (q) {
                k += q;
            } else {
                out[k++] = len == 1 && *p >= 32 && *p < 127 ? *p : '?';
            }
        } else {
            k += got;
        }

        p += j;
    }

    return k;
}

static double text_width(pfont* f, const unsigned char* s, size_t n, double size) {
    double w = 0;
    size_t i = 0;

    while (f && i < n) {
        uint32_t code;
        int k = pf_next(f, s + i, n - i, &code);

        if (k <= 0) {
            break;
        }

        w += pf_width(f, code) * size;
        i += (size_t)k;
    }

    return w;
}

/* lines of (single-byte) text broken to a width: each line's byte range */
static int wrap(pfont* f, const unsigned char* s, size_t n, double size, double width, size_t* starts, size_t* ends,
                int max) {
    int nl = 0;
    size_t i = 0;

    while (i <= n && nl < max) {
        size_t ls = i, last_space = (size_t)-1, j = i;
        double w = 0;

        while (j < n && s[j] != '\n' && s[j] != '\r') {
            double cw = text_width(f, s + j, 1, size);

            if (w + cw > width && j > ls) {
                break;
            }

            if (s[j] == ' ') {
                last_space = j;
            }

            w += cw;
            j++;
        }

        if (j < n && s[j] != '\n' && s[j] != '\r' && last_space != (size_t)-1 && last_space > ls) {
            j = last_space;     /* broken at the last space */
        }

        starts[nl] = ls;
        ends[nl] = j;
        nl++;

        if (j >= n) {
            break;
        }

        i = j + 1;

        if (s[j] == '\r' && i < n && s[i] == '\n') {
            i++;
        }
    }

    return nl;
}

/* an appearance stream: its content, its box, its resources */
static pv appearance(pdoc* d, const pbuf* content, const double* bbox, pv res) {
    pv dict = pv_dict(d, 6);

    pd_dict_set(d, &dict, "Type", pv_name(d, "XObject"));
    pd_dict_set(d, &dict, "Subtype", pv_name(d, "Form"));
    pd_dict_set(d, &dict, "BBox", arr_of(d, bbox, 4));

    if (res.t != PV_NULL) {
        pd_dict_set(d, &dict, "Resources", res);
    }

    return make_stream(d, dict, content->p ? content->p : (const unsigned char*)"", content->n);
}

static pv gs_res(pdoc* d, double alpha, int multiply) {
    pv res = pv_dict(d, 2), ext = pv_dict(d, 1), g = pv_dict(d, 3);

    pd_dict_set(d, &g, "Type", pv_name(d, "ExtGState"));
    pd_dict_set(d, &g, "CA", pv_real(alpha));
    pd_dict_set(d, &g, "ca", pv_real(alpha));

    if (multiply) {
        pd_dict_set(d, &g, "BM", pv_name(d, "Multiply"));
    }

    pd_dict_set(d, &ext, "G0", g);
    pd_dict_set(d, &res, "ExtGState", ext);
    return res;
}

/* a DA string's size (before Tf) and fill colour */
static void parse_da(pv da, char* fname, size_t fcap, double* size, uint32_t* col) {
    char tmp[256];
    const char* sl, *rg, *g;
    double c1, c2, c3;

    *size = 0;
    *col = 0;

    if (fname && fcap) {
        snprintf(fname, fcap, "Helv");
    }

    if (da.t != PV_STRING) {
        return;
    }

    snprintf(tmp, sizeof(tmp), "%.*s", da.n, da.u.s);
    sl = strchr(tmp, '/');

    if (sl) {
        size_t k = 0;

        sl++;

        while (*sl && *sl != ' ') {
            if (fname && k + 1 < fcap) {
                fname[k++] = *sl;
            }

            sl++;
        }

        if (fname && fcap) {
            fname[k] = 0;
        }

        while (*sl == ' ') {
            sl++;
        }

        *size = atof(sl);
    }

    rg = strstr(tmp, " rg");
    g = strstr(tmp, " g");

    if (rg) {
        const char* q = rg;
        int sp = 0;

        while (q > tmp && sp < 3) {
            q--;

            if (*q == ' ' && q[1] != ' ') {
                sp++;
            }
        }

        if (sscanf(q, "%lf %lf %lf", &c1, &c2, &c3) == 3) {
            *col = ((uint32_t)(c1 * 255 + 0.5) << 16) | ((uint32_t)(c2 * 255 + 0.5) << 8) | (uint32_t)(c3 * 255 + 0.5);
        }
    } else if (g) {
        const char* q = g;

        while (q > tmp && q[-1] != ' ') {
            q--;
        }

        c1 = atof(q);
        *col = (uint32_t)(c1 * 255 + 0.5) * 0x010101u;
    }
}

/* the appearance an annotation of a kind Parade makes is drawn with (from its own entries) */
static pv make_ap(pdoc* d, pv a) {
    const char* sub = pd_getname(d, a, "Subtype");
    double r[4], w = 1;
    uint32_t c = arr_rgb(d, pd_get(d, a, "C"));
    pv ic = pd_get(d, a, "IC"), res = pv_null();
    pbuf b;
    double ca = pd_getnum(d, a, "CA", 1);
    int has_fill = ic.t == PV_ARRAY && ic.n >= 1;
    pv bs = pd_get(d, a, "BS");

    if (!sub || !pd_rect(d, pd_get(d, a, "Rect"), r)) {
        return pv_null();
    }

    if (bs.t == PV_DICT) {
        w = pd_getnum(d, bs, "W", 1);
    } else {
        pv border = pd_get(d, a, "Border");

        if (border.t == PV_ARRAY && border.n >= 3) {
            pd_num(d, pd_index(d, border, 2), &w);
        }
    }

    memset(&b, 0, sizeof(b));

    if (strcmp(sub, "Highlight") == 0 || strcmp(sub, "Underline") == 0 || strcmp(sub, "StrikeOut") == 0 ||
        strcmp(sub, "Squiggly") == 0) {
        pv q = pd_get(d, a, "QuadPoints");
        int32_t i;
        int hl = sub[0] == 'H';

        res = gs_res(d, ca, hl);
        qb_puts(&b, "/G0 gs\n");
        col_op(&b, c, hl ? "rg" : "RG");

        for (i = 0; q.t == PV_ARRAY && i + 8 <= q.n; i += 8) {
            double v[8];
            int k;

            for (k = 0; k < 8; k++) {
                v[k] = 0;
                pd_num(d, q.u.a[i + k], &v[k]);
            }

            /* x1 y1 (top left) x2 y2 (top right) x3 y3 (bottom left) x4 y4 (bottom right) */
            if (hl) {
                pt(&b, v[0], v[1], "m");
                pt(&b, v[2], v[3], "l");
                pt(&b, v[6], v[7], "l");
                pt(&b, v[4], v[5], "l");
                qb_puts(&b, "h f\n");
            } else {
                double hgt = sqrt((v[0] - v[4]) * (v[0] - v[4]) + (v[1] - v[5]) * (v[1] - v[5]));
                double lw = hgt / 14 > 0.5 ? hgt / 14 : 0.5, t = sub[1] == 't' ? 0.45 : sub[1] == 'q' ? 0.06 : 0.08;
                double ax = v[4] + (v[0] - v[4]) * t, ay = v[5] + (v[1] - v[5]) * t;
                double bx = v[6] + (v[2] - v[6]) * t, by = v[7] + (v[3] - v[7]) * t;

                qb_num(&b, lw);
                qb_puts(&b, " w\n");

                if (sub[1] == 'q') {    /* a zigzag along the bottom */
                    double len = sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)), step = hgt / 6;
                    double ux = len > 0 ? (bx - ax) / len : 1, uy = len > 0 ? (by - ay) / len : 0, s;
                    int up = 0;

                    pt(&b, ax, ay, "m");

                    for (s = step; s < len && step > 0; s += step) {
                        double off = up ? 0 : step;

                        pt(&b, ax + ux * s - uy * off, ay + uy * s + ux * off, "l");
                        up = !up;
                    }

                    qb_puts(&b, "S\n");
                } else {
                    pt(&b, ax, ay, "m");
                    pt(&b, bx, by, "l");
                    qb_puts(&b, "S\n");
                }
            }
        }
    } else if (strcmp(sub, "Ink") == 0) {
        pv ink = pd_get(d, a, "InkList");
        int32_t i, k;

        res = gs_res(d, ca, 0);
        qb_puts(&b, "/G0 gs 1 J 1 j\n");
        qb_num(&b, w);
        qb_puts(&b, " w\n");
        col_op(&b, c, "RG");

        for (i = 0; ink.t == PV_ARRAY && i < ink.n; i++) {
            pv s = pd_index(d, ink, i);

            for (k = 0; s.t == PV_ARRAY && k + 1 < s.n; k += 2) {
                double x = 0, y = 0;

                pd_num(d, s.u.a[k], &x);
                pd_num(d, s.u.a[k + 1], &y);
                pt(&b, x, y, k == 0 ? "m" : "l");

                if (s.n == 2) {     /* a dot */
                    pt(&b, x, y, "l");
                }
            }

            qb_puts(&b, "S\n");
        }
    } else if (strcmp(sub, "Square") == 0 || strcmp(sub, "Circle") == 0) {
        double h = w / 2;

        res = gs_res(d, ca, 0);
        qb_puts(&b, "/G0 gs\n");
        qb_num(&b, w);
        qb_puts(&b, " w\n");
        col_op(&b, c, "RG");

        if (has_fill) {
            col_op(&b, arr_rgb(d, ic), "rg");
        }

        if (sub[1] == 'q') {
            rect_op(&b, r[0] + h, r[1] + h, r[2] - r[0] - w, r[3] - r[1] - w, "");
        } else {
            ellipse(&b, r[0] + h, r[1] + h, r[2] - h, r[3] - h);
        }

        qb_puts(&b, has_fill ? (w > 0 ? "B\n" : "f\n") : "S\n");
    } else if (strcmp(sub, "Line") == 0) {
        double l[4] = { 0, 0, 0, 0 };
        pv le = pd_get(d, a, "LE");
        pv le0 = pd_index(d, le, 0), le1 = pd_index(d, le, 1);
        const char* start = le0.t == PV_NAME ? le0.u.s : "None", *end = le1.t == PV_NAME ? le1.u.s : "None";
        int k;

        for (k = 0; k < 4; k++) {
            pd_num(d, pd_index(d, pd_get(d, a, "L"), k), &l[k]);
        }

        res = gs_res(d, ca, 0);
        qb_puts(&b, "/G0 gs 1 J 0 j\n");
        qb_num(&b, w);
        qb_puts(&b, " w\n");
        col_op(&b, c, "RG");
        col_op(&b, c, "rg");
        pt(&b, l[0], l[1], "m");
        pt(&b, l[2], l[3], "l");
        qb_puts(&b, "S\n");

        for (k = 0; k < 2; k++) {
            const char* e = k ? end : start;
            double tx = k ? l[2] : l[0], ty = k ? l[3] : l[1], fx = k ? l[0] : l[2], fy = k ? l[1] : l[3];
            double dx = tx - fx, dy = ty - fy, len = sqrt(dx * dx + dy * dy), sz = 3 * w + 6;

            if (len <= 0 || strcmp(e, "None") == 0) {
                continue;
            }

            dx /= len;
            dy /= len;

            /* the arrowhead: two sides back from the end, at 30 degrees */
            {
                double ax = tx - sz * (dx * 0.866 - dy * 0.5), ay = ty - sz * (dy * 0.866 + dx * 0.5);
                double bx = tx - sz * (dx * 0.866 + dy * 0.5), by = ty - sz * (dy * 0.866 - dx * 0.5);

                pt(&b, ax, ay, "m");
                pt(&b, tx, ty, "l");
                pt(&b, bx, by, "l");
                qb_puts(&b, strcmp(e, "ClosedArrow") == 0 ? "h B\n" : "S\n");
            }
        }
    } else if (strcmp(sub, "Text") == 0) {
        /* a note: a sheet with a turned corner and lines on it */
        double x0 = r[0], y0 = r[1], x1 = r[2], y1 = r[3], s = (x1 - x0) / 20;

        res = gs_res(d, ca, 0);
        qb_puts(&b, "/G0 gs 1 j\n");
        qb_num(&b, s > 0.5 ? s : 0.5);
        qb_puts(&b, " w\n");
        col_op(&b, c ? c : 0xFFD700, "rg");
        col_op(&b, 0x404040, "RG");
        pt(&b, x0 + s, y0 + s, "m");
        pt(&b, x1 - s, y0 + s, "l");
        pt(&b, x1 - s, y1 - 6 * s, "l");
        pt(&b, x1 - 6 * s, y1 - s, "l");
        pt(&b, x0 + s, y1 - s, "l");
        qb_puts(&b, "h B\n");
        pt(&b, x0 + 5 * s, y1 - 6 * s, "m");
        pt(&b, x1 - 8 * s, y1 - 6 * s, "l");
        pt(&b, x0 + 5 * s, y1 - 10 * s, "m");
        pt(&b, x1 - 5 * s, y1 - 10 * s, "l");
        pt(&b, x0 + 5 * s, y1 - 14 * s, "m");
        pt(&b, x1 - 5 * s, y1 - 14 * s, "l");
        qb_puts(&b, "S\n");
    } else if (strcmp(sub, "FreeText") == 0) {
        pv contents = pd_get(d, a, "Contents");
        char* text = contents.t == PV_STRING ? pd_text_utf8(contents, NULL) : NULL;
        pv font = helv_font(d), fonts = pv_dict(d, 1);
        double size;
        uint32_t tc;
        pfont* f;

        parse_da(pd_get(d, a, "DA"), NULL, 0, &size, &tc);

        if (!(size > 0)) {
            size = 12;
        }

        pd_dict_set(d, &fonts, "Helv", font);
        res = gs_res(d, ca, 0);
        pd_dict_set(d, &res, "Font", fonts);
        f = pf_load(d, font);
        qb_puts(&b, "/G0 gs\n");

        if (c) {    /* a border in its colour */
            qb_puts(&b, "1 w ");
            col_op(&b, c, "RG");
            rect_op(&b, r[0] + 0.5, r[1] + 0.5, r[2] - r[0] - 1, r[3] - r[1] - 1, "S");
        }

        if (text) {
            unsigned char enc[4096];
            size_t n = encode_in(f, text, enc, sizeof(enc)), starts[256], ends[256];
            int nl = wrap(f, enc, n, size, r[2] - r[0] - 4, starts, ends, 256), i;

            qb_puts(&b, "q ");
            rect_op(&b, r[0] + 1, r[1] + 1, r[2] - r[0] - 2, r[3] - r[1] - 2, "W n");
            qb_puts(&b, "BT /Helv ");
            qb_num(&b, size);
            qb_puts(&b, " Tf ");
            col_op(&b, tc, "rg");

            for (i = 0; i < nl; i++) {
                qb_puts(&b, "1 0 0 1 ");
                qb_num(&b, r[0] + 2);
                qb_puts(&b, " ");
                qb_num(&b, r[3] - 2 - size * 0.9 - i * size * 1.15);
                qb_puts(&b, " Tm ");
                pdf_string(&b, enc + starts[i], ends[i] - starts[i]);
                qb_puts(&b, " Tj\n");
            }

            qb_puts(&b, "ET Q\n");
            free(text);
        }
    } else {
        qb_free(&b);
        return pv_null();
    }

    {
        pv ap = appearance(d, &b, r, res);

        qb_free(&b);
        return ap;
    }
}

/* an appearance for an annotation that has none (drawn only; nothing in the document changes) */
void pd_annot_appearance(pdoc* d, pv annot, pv* form) {
    const char* sub = pd_getname(d, annot, "Subtype");

    *form = pv_null();

    if (!sub || strcmp(sub, "Widget") == 0) {
        return;
    }

    *form = make_ap(d, annot);
}

/* ------------------------------------------------------------------ */
/* annotations                                                        */
/* ------------------------------------------------------------------ */

static const struct {
    const char* name;
    int kind;
} kinds[] = {
    {"Text", PD_ANNOT_TEXT}, {"Link", PD_ANNOT_LINK}, {"FreeText", PD_ANNOT_FREETEXT}, {"Line", PD_ANNOT_LINE},
    {"Square", PD_ANNOT_SQUARE}, {"Circle", PD_ANNOT_CIRCLE}, {"Polygon", PD_ANNOT_POLYGON},
    {"PolyLine", PD_ANNOT_POLYLINE}, {"Highlight", PD_ANNOT_HIGHLIGHT}, {"Underline", PD_ANNOT_UNDERLINE},
    {"Squiggly", PD_ANNOT_SQUIGGLY}, {"StrikeOut", PD_ANNOT_STRIKEOUT}, {"Stamp", PD_ANNOT_STAMP},
    {"Ink", PD_ANNOT_INK}, {"Widget", PD_ANNOT_WIDGET}, {"Popup", PD_ANNOT_POPUP}
};

static pv annots_of(pdoc* d, int32_t page) {
    return page >= 0 && page < d->npages ? pd_get(d, d->pages[page].dict, "Annots") : pv_null();
}

int32_t pd_pdfr_annot_count(pd_pdfr* d, int32_t page) {
    pv a;

    if (!d || page < 0 || page >= d->npages) {
        return 0;
    }

    a = annots_of(d, page);
    return a.t == PV_ARRAY ? a.n : 0;
}

static int field_index_of(pdoc* d, pv widget);

static void link_target(pdoc* d, pv a, char* uri, size_t cap) {
    pv act = pd_get(d, a, "A"), dest = pd_get(d, a, "Dest");
    const char* s = pd_getname(d, act, "S");
    pv root = pd_get(d, d->trailer, "Root");

    if (s && strcmp(s, "URI") == 0) {
        pv u = pd_get(d, act, "URI");

        if (u.t == PV_STRING) {
            snprintf(uri, cap, "%.*s", u.n, u.u.s);
        }

        return;
    }

    if (s && strcmp(s, "GoTo") == 0) {
        dest = pd_get(d, act, "D");
    }

    if (dest.t == PV_NAME && pd_get(d, root, "Dests").t == PV_DICT) {
        dest = pd_get(d, pd_get(d, root, "Dests"), dest.u.s);
    } else if (dest.t == PV_STRING) {    /* a named destination: the name tree looked through */
        pv stack[32];
        int sp = 0, found = 0;
        pv names = pd_get(d, pd_get(d, root, "Names"), "Dests");

        if (names.t == PV_DICT) {
            stack[sp++] = names;
        }

        while (sp > 0 && !found) {
            pv node = stack[--sp], ns = pd_get(d, node, "Names"), kids = pd_get(d, node, "Kids");
            int32_t i;

            for (i = 0; ns.t == PV_ARRAY && i + 1 < ns.n; i += 2) {
                pv key = pd_index(d, ns, i);

                if (key.t == PV_STRING && key.n == dest.n && memcmp(key.u.s, dest.u.s, (size_t)dest.n) == 0) {
                    dest = pd_index(d, ns, i + 1);
                    found = 1;
                    break;
                }
            }

            for (i = 0; !found && kids.t == PV_ARRAY && i < kids.n && sp < 32; i++) {
                stack[sp++] = pd_index(d, kids, i);
            }
        }
    }

    if (dest.t == PV_DICT) {
        dest = pd_get(d, dest, "D");
    }

    if (dest.t == PV_ARRAY && dest.n >= 1) {
        pv pg = dest.u.a[0];
        int32_t i;

        if (pg.t == PV_INT) {
            snprintf(uri, cap, "#page=%d", (int)pg.u.i + 1);
        }

        for (i = 0; pg.t == PV_REF && i < d->npages; i++) {
            if (d->pages[i].ref.t == PV_REF && d->pages[i].ref.u.i == pg.u.i) {
                snprintf(uri, cap, "#page=%d", (int)i + 1);
                break;
            }
        }
    }
}

pd_status pd_pdfr_annot_get(pd_pdfr* d, int32_t page, int32_t index, pd_pdfr_annot* out) {
    pv arr, a;
    const char* sub;
    double r[4];
    size_t k;

    if (!d || !out || page < 0 || page >= d->npages) {
        return PD_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    arr = annots_of(d, page);
    a = pd_index(d, arr, index);

    if (a.t != PV_DICT) {
        return PD_ERR_RANGE;
    }

    sub = pd_getname(d, a, "Subtype");
    out->kind = PD_ANNOT_OTHER;

    for (k = 0; sub && k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        if (strcmp(kinds[k].name, sub) == 0) {
            out->kind = kinds[k].kind;
        }
    }

    if (out->kind == PD_ANNOT_LINE) {
        pv le1 = pd_index(d, pd_get(d, a, "LE"), 1);

        if (le1.t == PV_NAME && strstr(le1.u.s, "Arrow")) {
            out->kind = PD_ANNOT_ARROW;
        }
    }

    if (pd_rect(d, pd_get(d, a, "Rect"), r)) {
        double o[4];

        rect_shown(d, page, r, o);
        out->x0 = o[0];
        out->y0 = o[1];
        out->x1 = o[2];
        out->y1 = o[3];
    }

    out->color = arr_rgb(d, pd_get(d, a, "C"));

    {
        pv ic = pd_get(d, a, "IC");

        out->fill = ic.t == PV_ARRAY && ic.n ? 0xFF000000u | arr_rgb(d, ic) : 0;
    }

    out->width = pd_getnum(d, pd_get(d, a, "BS"), "W", 1);
    out->hidden = (pd_getint(d, a, "F", 0) & 2) || out->kind == PD_ANNOT_POPUP;
    out->field = out->kind == PD_ANNOT_WIDGET ? field_index_of(d, arr.u.a[index]) : -1;

    {
        pv c = pd_get(d, a, "Contents");

        if (c.t == PV_STRING) {
            char* s = pd_text_utf8(c, NULL);

            if (s) {
                snprintf(out->contents, sizeof(out->contents), "%s", s);
                free(s);
            }
        }
    }

    if (out->kind == PD_ANNOT_LINK) {
        link_target(d, a, out->uri, sizeof(out->uri));
    }

    return PD_OK;
}

int32_t pd_pdfr_annot_at(pd_pdfr* d, int32_t page, double x, double y) {
    int32_t n = pd_pdfr_annot_count(d, page), i;

    for (i = n - 1; i >= 0; i--) {
        pd_pdfr_annot a;

        if (pd_pdfr_annot_get(d, page, i, &a) == PD_OK && !a.hidden && x >= a.x0 - 2 && x <= a.x1 + 2 &&
            y >= a.y0 - 2 && y <= a.y1 + 2) {
            return i;
        }
    }

    return -1;
}

/* the page's dictionary copied, with an Annots array of its own */
static pv page_for_write(pdoc* d, int32_t page) {
    pv pg = dict_copy(d, d->pages[page].dict);
    pv arr = pd_get(d, pg, "Annots"), copy;
    int32_t i;

    copy = pv_array(d, arr.t == PV_ARRAY ? arr.n : 0);

    for (i = 0; i < copy.n; i++) {
        copy.u.a[i] = arr.u.a[i];
    }

    pd_dict_set(d, &pg, "Annots", copy);
    return pg;
}

static void page_store(pdoc* d, int32_t page, pv pg) {
    if (d->pages[page].ref.t == PV_REF) {
        set_obj(d, (int32_t)d->pages[page].ref.u.i, pg);
        d->pages[page].dict = pg;
    }
}

static void now_date(char* out, size_t cap) {
    time_t t = time(NULL);
    struct tm* g = gmtime(&t);

    if (g) {
        snprintf(out, cap, "D:%04d%02d%02d%02d%02d%02dZ", g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour,
                 g->tm_min, g->tm_sec);
    } else {
        snprintf(out, cap, "D:20260101000000Z");
    }
}

static int32_t add_annot(pdoc* d, int32_t page, pv a) {
    pv pg, arr, ap;
    int32_t num, at;
    char nm[64], date[48];

    now_date(date, sizeof(date));
    pd_dict_set(d, &a, "Type", pv_name(d, "Annot"));
    pd_dict_set(d, &a, "F", pv_int(4));
    pd_dict_set(d, &a, "P", d->pages[page].ref);
    pd_dict_set(d, &a, "M", pv_string(d, date, strlen(date)));
    pd_dict_set(d, &a, "CreationDate", pv_string(d, date, strlen(date)));
    snprintf(nm, sizeof(nm), "parade-%d-%d", (int)d->nxref, (int)d->changes);
    pd_dict_set(d, &a, "NM", pv_string(d, nm, strlen(nm)));
    ap = make_ap(d, a);

    if (ap.t == PV_STREAM) {
        pv apd = pv_dict(d, 1);

        pd_dict_set(d, &apd, "N", pv_ref(new_obj(d, ap), 0));
        pd_dict_set(d, &a, "AP", apd);
    }

    num = new_obj(d, a);
    pg = page_for_write(d, page);
    arr = pd_array_add(d, pd_get_raw(pg, "Annots"), pv_ref(num, 0));
    at = arr.n - 1;
    pd_dict_set(d, &pg, "Annots", arr);
    page_store(d, page, pg);
    return at;
}

/* the user-space rectangle round points, padded */
static void bounds_of(const double* xy, int32_t n, double pad, double* r) {
    int32_t i;

    r[0] = r[2] = xy[0];
    r[1] = r[3] = xy[1];

    for (i = 1; i < n; i++) {
        r[0] = xy[2 * i] < r[0] ? xy[2 * i] : r[0];
        r[1] = xy[2 * i + 1] < r[1] ? xy[2 * i + 1] : r[1];
        r[2] = xy[2 * i] > r[2] ? xy[2 * i] : r[2];
        r[3] = xy[2 * i + 1] > r[3] ? xy[2 * i + 1] : r[3];
    }

    r[0] -= pad;
    r[1] -= pad;
    r[2] += pad;
    r[3] += pad;
}

static int valid_page(const pdoc* d, int32_t page) {
    return d && d->cache && page >= 0 && page < d->npages && d->pages[page].ref.t == PV_REF;
}

int32_t pd_pdfr_add_markup(pd_pdfr* d, int32_t page, int32_t kind, const double* quads, int32_t nquads,
                           uint32_t rgb, const char* contents) {
    const char* sub = kind == PD_ANNOT_HIGHLIGHT ? "Highlight" : kind == PD_ANNOT_UNDERLINE ? "Underline" :
                      kind == PD_ANNOT_SQUIGGLY ? "Squiggly" : kind == PD_ANNOT_STRIKEOUT ? "StrikeOut" : NULL;
    double* u, r[4];
    int32_t i, at;
    pv a;

    if (!valid_page(d, page) || !sub || !quads || nquads <= 0) {
        return -1;
    }

    u = (double*)malloc(sizeof(double) * 8 * (size_t)nquads);

    if (!u) {
        return -1;
    }

    for (i = 0; i < 4 * nquads; i++) {
        to_user(d, page, quads[2 * i], quads[2 * i + 1], &u[2 * i], &u[2 * i + 1]);
    }

    bounds_of(u, 4 * nquads, 1, r);
    edit_begin(d);
    a = pv_dict(d, 12);
    pd_dict_set(d, &a, "Subtype", pv_name(d, sub));
    pd_dict_set(d, &a, "Rect", arr_of(d, r, 4));
    pd_dict_set(d, &a, "QuadPoints", arr_of(d, u, 8 * nquads));
    pd_dict_set(d, &a, "C", rgb_arr(d, rgb));

    if (contents && *contents) {
        pd_dict_set(d, &a, "Contents", pd_utf8_text(d, contents));
    }

    at = add_annot(d, page, a);
    edit_end(d);
    free(u);
    return at;
}

int32_t pd_pdfr_add_ink(pd_pdfr* d, int32_t page, const double* points, const int32_t* counts, int32_t nstrokes,
                        uint32_t rgb, double width) {
    pv a, ink, bs;
    double r[4] = { 1e30, 1e30, -1e30, -1e30 };
    int32_t s, k, at, total = 0;

    if (!valid_page(d, page) || !points || !counts || nstrokes <= 0) {
        return -1;
    }

    edit_begin(d);
    ink = pv_array(d, nstrokes);

    for (s = 0; s < nstrokes; s++) {
        pv st = pv_array(d, 2 * (counts[s] > 0 ? counts[s] : 0));

        for (k = 0; k < counts[s]; k++) {
            double ux, uy;

            to_user(d, page, points[2 * (total + k)], points[2 * (total + k) + 1], &ux, &uy);
            st.u.a[2 * k] = pv_real(ux);
            st.u.a[2 * k + 1] = pv_real(uy);
            r[0] = ux < r[0] ? ux : r[0];
            r[1] = uy < r[1] ? uy : r[1];
            r[2] = ux > r[2] ? ux : r[2];
            r[3] = uy > r[3] ? uy : r[3];
        }

        total += counts[s];
        ink.u.a[s] = st;
    }

    r[0] -= width + 1;
    r[1] -= width + 1;
    r[2] += width + 1;
    r[3] += width + 1;
    a = pv_dict(d, 12);
    bs = pv_dict(d, 1);
    pd_dict_set(d, &bs, "W", pv_real(width));
    pd_dict_set(d, &a, "Subtype", pv_name(d, "Ink"));
    pd_dict_set(d, &a, "Rect", arr_of(d, r, 4));
    pd_dict_set(d, &a, "InkList", ink);
    pd_dict_set(d, &a, "C", rgb_arr(d, rgb));
    pd_dict_set(d, &a, "BS", bs);
    at = add_annot(d, page, a);
    edit_end(d);
    return at;
}

int32_t pd_pdfr_add_shape(pd_pdfr* d, int32_t page, int32_t kind, double x0, double y0, double x1, double y1,
                          uint32_t rgb, uint32_t fill, double width) {
    double u[4], r[4];
    pv a, bs;
    int32_t at;

    if (!valid_page(d, page) || (kind != PD_ANNOT_SQUARE && kind != PD_ANNOT_CIRCLE && kind != PD_ANNOT_LINE &&
                                 kind != PD_ANNOT_ARROW)) {
        return -1;
    }

    to_user(d, page, x0, y0, &u[0], &u[1]);
    to_user(d, page, x1, y1, &u[2], &u[3]);
    edit_begin(d);
    a = pv_dict(d, 12);
    bs = pv_dict(d, 1);
    pd_dict_set(d, &bs, "W", pv_real(width));
    pd_dict_set(d, &a, "BS", bs);
    pd_dict_set(d, &a, "C", rgb_arr(d, rgb));

    if (fill >> 24) {
        pd_dict_set(d, &a, "IC", rgb_arr(d, fill & 0xFFFFFF));
    }

    if (kind == PD_ANNOT_SQUARE || kind == PD_ANNOT_CIRCLE) {
        bounds_of(u, 2, 0, r);
        pd_dict_set(d, &a, "Subtype", pv_name(d, kind == PD_ANNOT_SQUARE ? "Square" : "Circle"));
    } else {
        pv le = pv_array(d, 2);

        bounds_of(u, 2, 3 * width + 8, r);
        pd_dict_set(d, &a, "Subtype", pv_name(d, "Line"));
        pd_dict_set(d, &a, "L", arr_of(d, u, 4));
        le.u.a[0] = pv_name(d, "None");
        le.u.a[1] = pv_name(d, kind == PD_ANNOT_ARROW ? "OpenArrow" : "None");
        pd_dict_set(d, &a, "LE", le);
    }

    pd_dict_set(d, &a, "Rect", arr_of(d, r, 4));
    at = add_annot(d, page, a);
    edit_end(d);
    return at;
}

int32_t pd_pdfr_add_note(pd_pdfr* d, int32_t page, double x, double y, const char* utf8, uint32_t rgb) {
    double u[2], r[4];
    pv a;
    int32_t at;

    if (!valid_page(d, page)) {
        return -1;
    }

    to_user(d, page, x, y, &u[0], &u[1]);
    r[0] = u[0];
    r[1] = u[1] - 20;
    r[2] = u[0] + 20;
    r[3] = u[1];
    edit_begin(d);
    a = pv_dict(d, 10);
    pd_dict_set(d, &a, "Subtype", pv_name(d, "Text"));
    pd_dict_set(d, &a, "Rect", arr_of(d, r, 4));
    pd_dict_set(d, &a, "C", rgb_arr(d, rgb));
    pd_dict_set(d, &a, "Name", pv_name(d, "Note"));
    pd_dict_set(d, &a, "Contents", pd_utf8_text(d, utf8 ? utf8 : ""));
    at = add_annot(d, page, a);
    edit_end(d);
    return at;
}

int32_t pd_pdfr_add_text_box(pd_pdfr* d, int32_t page, double x0, double y0, double x1, double y1, const char* utf8,
                             double size, uint32_t rgb) {
    double u[4], r[4];
    char da[96];
    pv a;
    int32_t at;

    if (!valid_page(d, page)) {
        return -1;
    }

    to_user(d, page, x0, y0, &u[0], &u[1]);
    to_user(d, page, x1, y1, &u[2], &u[3]);
    bounds_of(u, 2, 0, r);
    snprintf(da, sizeof(da), "/Helv %.2f Tf %.3f %.3f %.3f rg", size > 0 ? size : 12, ((rgb >> 16) & 255) / 255.0,
             ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0);
    edit_begin(d);
    a = pv_dict(d, 10);
    pd_dict_set(d, &a, "Subtype", pv_name(d, "FreeText"));
    pd_dict_set(d, &a, "Rect", arr_of(d, r, 4));
    pd_dict_set(d, &a, "DA", pv_string(d, da, strlen(da)));
    pd_dict_set(d, &a, "Contents", pd_utf8_text(d, utf8 ? utf8 : ""));
    at = add_annot(d, page, a);
    edit_end(d);
    return at;
}

/* an annotation's own object, copied for a change: its number (0 if it is not an object of its own) */
static int32_t annot_for_write(pdoc* d, int32_t page, int32_t index, pv* copy) {
    pv arr = annots_of(d, page), ref;

    if (arr.t != PV_ARRAY || index < 0 || index >= arr.n) {
        return 0;
    }

    ref = arr.u.a[index];

    if (ref.t != PV_REF || pd_resolve(d, ref).t != PV_DICT) {
        return 0;
    }

    *copy = dict_copy(d, pd_resolve(d, ref));
    return (int32_t)ref.u.i;
}

/* an annotation of a kind Parade draws gets its appearance made again after a change */
static void remake_ap(pdoc* d, pv* a) {
    pv ap = make_ap(d, *a);

    if (ap.t == PV_STREAM) {
        pv apd = pv_dict(d, 1);

        pd_dict_set(d, &apd, "N", pv_ref(new_obj(d, ap), 0));
        pd_dict_set(d, a, "AP", apd);
    }
}

pd_status pd_pdfr_annot_delete(pd_pdfr* d, int32_t page, int32_t index) {
    pv pg, arr, out, a, popup;
    int32_t i, k = 0;

    if (!valid_page(d, page) || index < 0 || index >= pd_pdfr_annot_count(d, page)) {
        return PD_ERR_RANGE;
    }

    edit_begin(d);
    pg = page_for_write(d, page);
    arr = pd_get_raw(pg, "Annots");
    a = pd_index(d, arr, index);
    popup = pd_get_raw(a, "Popup");
    out = pv_array(d, arr.n);

    for (i = 0; i < arr.n; i++) {
        if (i == index || (popup.t == PV_REF && arr.u.a[i].t == PV_REF && arr.u.a[i].u.i == popup.u.i)) {
            continue;
        }

        out.u.a[k++] = arr.u.a[i];
    }

    out.n = k;
    pd_dict_set(d, &pg, "Annots", out);
    page_store(d, page, pg);
    edit_end(d);
    return PD_OK;
}

static void shift_nums(pdoc* d, pv* arr, double dx, double dy) {
    pv a = pd_resolve(d, *arr), o;
    int32_t i;

    if (a.t != PV_ARRAY) {
        return;
    }

    o = pv_array(d, a.n);

    for (i = 0; i < a.n; i++) {
        pv e = pd_resolve(d, a.u.a[i]);
        double v;

        if (e.t == PV_ARRAY) {
            shift_nums(d, &e, dx, dy);
            o.u.a[i] = e;
        } else if (pd_num(d, e, &v)) {
            o.u.a[i] = pv_real(v + (i % 2 ? dy : dx));
        } else {
            o.u.a[i] = e;
        }
    }

    *arr = o;
}

pd_status pd_pdfr_annot_move(pd_pdfr* d, int32_t page, int32_t index, double dx, double dy) {
    pv a;
    int32_t num;
    double m[6], ux, uy;
    static const char* const keys[] = { "Rect", "QuadPoints", "InkList", "L", "Vertices", "CL" };
    size_t k;

    if (!valid_page(d, page)) {
        return PD_ERR_ARG;
    }

    num = annot_for_write(d, page, index, &a);

    if (!num) {
        return PD_ERR_RANGE;
    }

    page_inv(d, page, m);
    ux = m[0] * dx + m[2] * dy;
    uy = m[1] * dx + m[3] * dy;
    edit_begin(d);

    for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        pv v = pd_get(d, a, keys[k]);

        if (v.t == PV_ARRAY) {
            shift_nums(d, &v, ux, uy);
            pd_dict_set(d, &a, keys[k], v);
        }
    }

    set_obj(d, num, a);

    {
        pv pp = pd_get_raw(a, "Popup");     /* its popup goes with it */

        if (pp.t == PV_REF && pd_resolve(d, pp).t == PV_DICT) {
            pv p = dict_copy(d, pd_resolve(d, pp)), r = pd_get(d, p, "Rect");

            if (r.t == PV_ARRAY) {
                shift_nums(d, &r, ux, uy);
                pd_dict_set(d, &p, "Rect", r);
                set_obj(d, (int32_t)pp.u.i, p);
            }
        }
    }

    edit_end(d);
    return PD_OK;
}

pd_status pd_pdfr_annot_set_contents(pd_pdfr* d, int32_t page, int32_t index, const char* utf8) {
    pv a;
    int32_t num = valid_page(d, page) ? annot_for_write(d, page, index, &a) : 0;
    const char* sub;

    if (!num) {
        return PD_ERR_RANGE;
    }

    edit_begin(d);
    pd_dict_set(d, &a, "Contents", pd_utf8_text(d, utf8 ? utf8 : ""));
    sub = pd_getname(d, a, "Subtype");

    if (sub && strcmp(sub, "FreeText") == 0) {
        remake_ap(d, &a);
    }

    set_obj(d, num, a);
    edit_end(d);
    return PD_OK;
}

pd_status pd_pdfr_annot_set_color(pd_pdfr* d, int32_t page, int32_t index, uint32_t rgb) {
    pv a;
    int32_t num = valid_page(d, page) ? annot_for_write(d, page, index, &a) : 0;

    if (!num) {
        return PD_ERR_RANGE;
    }

    edit_begin(d);
    pd_dict_set(d, &a, "C", rgb_arr(d, rgb));
    remake_ap(d, &a);
    set_obj(d, num, a);
    edit_end(d);
    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* form fields                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    pv ref;                     /* the field (its terminal node) */
    char name[256];
    char ft[8];                 /* inherited */
    int64_t ff;
    pv da;
    int64_t q;
} fld;

static void walk_fields(pdoc* d, pv ref, const char* prefix, const char* ft, int64_t ff, pv da, int64_t q, pbuf* out,
                        int depth) {
    pv f = pd_resolve(d, ref), kids, t;
    char name[256];
    const char* myft;
    int has_field_kids = 0;
    int32_t i;

    if (f.t != PV_DICT || depth > 32) {
        return;
    }

    t = pd_get(d, f, "T");

    if (t.t == PV_STRING) {
        char* s = pd_text_utf8(t, NULL);

        snprintf(name, sizeof(name), "%s%s%s", prefix, *prefix ? "." : "", s ? s : "");
        free(s);
    } else {
        snprintf(name, sizeof(name), "%s", prefix);
    }

    myft = pd_getname(d, f, "FT");

    if (myft) {
        ft = myft;
    }

    if (pd_get(d, f, "Ff").t == PV_INT) {
        ff = pd_get(d, f, "Ff").u.i;
    }

    if (pd_get(d, f, "DA").t == PV_STRING) {
        da = pd_get(d, f, "DA");
    }

    if (pd_get(d, f, "Q").t == PV_INT) {
        q = pd_get(d, f, "Q").u.i;
    }

    kids = pd_get(d, f, "Kids");

    for (i = 0; kids.t == PV_ARRAY && i < kids.n; i++) {
        if (pd_get_raw(pd_index(d, kids, i), "T").t != PV_NULL) {
            has_field_kids = 1;
        }
    }

    if (has_field_kids) {
        for (i = 0; i < kids.n; i++) {
            walk_fields(d, kids.u.a[i], name, ft, ff, da, q, out, depth + 1);
        }

        return;
    }

    {
        fld x;

        memset(&x, 0, sizeof(x));
        x.ref = ref;
        snprintf(x.name, sizeof(x.name), "%s", name);
        snprintf(x.ft, sizeof(x.ft), "%s", ft ? ft : "");
        x.ff = ff;
        x.da = da;
        x.q = q;
        qb_put(out, &x, sizeof(x));
    }
}

static fld* fields(pdoc* d, int32_t* n) {
    if (!d->fields || d->fields_changes != d->changes) {
        pbuf b;
        pv af = pd_get(d, pd_get(d, d->trailer, "Root"), "AcroForm"), fs = pd_get(d, af, "Fields");
        int32_t i;

        memset(&b, 0, sizeof(b));
        free(d->fields);

        for (i = 0; fs.t == PV_ARRAY && i < fs.n; i++) {
            walk_fields(d, fs.u.a[i], "", NULL, 0, pd_get(d, af, "DA"), 0, &b, 0);
        }

        d->fields = b.p;
        d->nfields = (int32_t)(b.n / sizeof(fld));
        d->fields_changes = d->changes;
    }

    *n = d->nfields;
    return (fld*)d->fields;
}

/* the widgets of a field: itself, or its kids */
static int32_t widgets(pdoc* d, const fld* f, pv* out, int32_t cap) {
    pv node = pd_resolve(d, f->ref), kids = pd_get(d, node, "Kids");
    int32_t n = 0, i;

    if (kids.t == PV_ARRAY && kids.n > 0) {
        for (i = 0; i < kids.n && n < cap; i++) {
            out[n++] = kids.u.a[i];
        }
    } else if (cap > 0) {
        out[n++] = f->ref;
    }

    return n;
}

static int field_index_of(pdoc* d, pv widget) {
    int32_t n, i, k;
    fld* f = fields(d, &n);

    for (i = 0; i < n; i++) {
        pv w[64];
        int32_t nw = widgets(d, &f[i], w, 64);

        for (k = 0; k < nw; k++) {
            if (widget.t == PV_REF && w[k].t == PV_REF && w[k].u.i == widget.u.i) {
                return i;
            }
        }
    }

    return -1;
}

int32_t pd_pdfr_field_count(pd_pdfr* d) {
    int32_t n;

    if (!d || !d->cache) {
        return 0;
    }

    fields(d, &n);
    return n;
}

static int page_of(pdoc* d, pv widget) {
    pv p = pd_get_raw(pd_resolve(d, widget), "P");
    int32_t i, k;

    for (i = 0; i < d->npages; i++) {
        if (p.t == PV_REF && d->pages[i].ref.t == PV_REF && d->pages[i].ref.u.i == p.u.i) {
            return i;
        }
    }

    for (i = 0; i < d->npages; i++) {
        pv annots = pd_get(d, d->pages[i].dict, "Annots");

        for (k = 0; annots.t == PV_ARRAY && k < annots.n; k++) {
            if (widget.t == PV_REF && annots.u.a[k].t == PV_REF && annots.u.a[k].u.i == widget.u.i) {
                return i;
            }
        }
    }

    return -1;
}

/* the on states of a check box or radio button: its widgets' appearance names other than Off */
static int32_t on_states(pdoc* d, const fld* f, char (*out)[64], int32_t cap) {
    pv w[64];
    int32_t nw = widgets(d, f, w, 64), n = 0, i, k, j;

    for (i = 0; i < nw; i++) {
        pv nap = pd_get(d, pd_get(d, w[i], "AP"), "N");
        int32_t ne;
        pkv* e = pv_entries(nap, &ne);

        for (k = 0; k < ne && n < cap; k++) {
            int dup = 0;

            if (strcmp(e[k].key, "Off") == 0) {
                continue;
            }

            for (j = 0; j < n; j++) {
                if (strcmp(out[j], e[k].key) == 0) {
                    dup = 1;
                }
            }

            if (!dup) {
                snprintf(out[n++], 64, "%s", e[k].key);
            }
        }
    }

    return n;
}

pd_status pd_pdfr_field_get(pd_pdfr* d, int32_t index, pd_pdfr_field* out) {
    int32_t n;
    fld* f;
    pv node, v, w[1];

    if (!d || !out) {
        return PD_ERR_ARG;
    }

    f = fields(d, &n);

    if (index < 0 || index >= n) {
        return PD_ERR_RANGE;
    }

    memset(out, 0, sizeof(*out));
    f = &f[index];
    node = pd_resolve(d, f->ref);
    snprintf(out->name, sizeof(out->name), "%s", f->name);

    if (strcmp(f->ft, "Tx") == 0) {
        out->kind = PD_PDF_FIELD_TEXT;
    } else if (strcmp(f->ft, "Btn") == 0) {
        out->kind = (f->ff & (1 << 16)) ? PD_PDF_FIELD_PUSH : (f->ff & (1 << 15)) ? PD_PDF_FIELD_RADIO : PD_PDF_FIELD_CHECK;
    } else if (strcmp(f->ft, "Ch") == 0) {
        out->kind = (f->ff & (1 << 17)) ? PD_PDF_FIELD_COMBO : PD_PDF_FIELD_LIST;
    } else if (strcmp(f->ft, "Sig") == 0) {
        out->kind = PD_PDF_FIELD_SIGNATURE;
    }

    out->readonly = (f->ff & 1) != 0;
    out->required = (f->ff & 2) != 0;
    out->multiline = out->kind == PD_PDF_FIELD_TEXT && (f->ff & (1 << 12));
    out->password = out->kind == PD_PDF_FIELD_TEXT && (f->ff & (1 << 13));
    out->maxlen = (int32_t)pd_getint(d, node, "MaxLen", 0);
    v = pd_get(d, node, "V");

    if (v.t == PV_NULL) {
        v = pd_get(d, pd_get(d, node, "Parent"), "V");
    }

    if (v.t == PV_STRING) {
        char* s = pd_text_utf8(v, NULL);

        snprintf(out->value, sizeof(out->value), "%s", s ? s : "");
        free(s);
    } else if (v.t == PV_NAME) {
        snprintf(out->value, sizeof(out->value), "%s", v.u.s);
    } else if (v.t == PV_ARRAY && v.n > 0 && pd_index(d, v, 0).t == PV_STRING) {
        char* s = pd_text_utf8(pd_index(d, v, 0), NULL);

        snprintf(out->value, sizeof(out->value), "%s", s ? s : "");
        free(s);
    } else if (out->kind == PD_PDF_FIELD_CHECK || out->kind == PD_PDF_FIELD_RADIO) {
        snprintf(out->value, sizeof(out->value), "Off");
    }

    if (out->kind == PD_PDF_FIELD_CHECK || out->kind == PD_PDF_FIELD_RADIO) {
        char st[16][64];

        out->nopts = on_states(d, f, st, 16);

        if (v.t == PV_NULL) {   /* the widget's state, when V says nothing */
            pv w1[1];

            if (widgets(d, f, w1, 1) == 1 && pd_getname(d, w1[0], "AS")) {
                snprintf(out->value, sizeof(out->value), "%s", pd_getname(d, w1[0], "AS"));
            }
        }
    } else {
        pv opt = pd_get(d, node, "Opt");

        out->nopts = opt.t == PV_ARRAY ? opt.n : 0;
    }

    {
        uint32_t col;

        parse_da(f->da, NULL, 0, &out->font_size, &col);
    }

    out->page = -1;

    if (widgets(d, f, w, 1) == 1) {
        double r[4], o[4];

        out->page = page_of(d, w[0]);

        if (out->page >= 0 && pd_rect(d, pd_get(d, w[0], "Rect"), r)) {
            rect_shown(d, out->page, r, o);
            out->x0 = o[0];
            out->y0 = o[1];
            out->x1 = o[2];
            out->y1 = o[3];
        }
    }

    return PD_OK;
}

pd_status pd_pdfr_field_option(pd_pdfr* d, int32_t index, int32_t i, char* buf, size_t cap) {
    int32_t n;
    fld* f;
    pv opt, e;

    if (!d || !buf || !cap) {
        return PD_ERR_ARG;
    }

    f = fields(d, &n);
    buf[0] = 0;

    if (index < 0 || index >= n) {
        return PD_ERR_RANGE;
    }

    f = &f[index];

    if (strcmp(f->ft, "Btn") == 0) {
        char st[16][64];
        int32_t k = on_states(d, f, st, 16);

        if (i < 0 || i >= k) {
            return PD_ERR_RANGE;
        }

        snprintf(buf, cap, "%s", st[i]);
        return PD_OK;
    }

    opt = pd_get(d, pd_resolve(d, f->ref), "Opt");
    e = pd_index(d, opt, i);

    if (e.t == PV_ARRAY) {      /* [export display] */
        e = pd_index(d, e, 1);
    }

    if (e.t != PV_STRING) {
        return PD_ERR_RANGE;
    }

    {
        char* s = pd_text_utf8(e, NULL);

        snprintf(buf, cap, "%s", s ? s : "");
        free(s);
    }

    return PD_OK;
}

int32_t pd_pdfr_field_at(pd_pdfr* d, int32_t page, double x, double y) {
    int32_t n = pd_pdfr_annot_count(d, page), i;

    for (i = n - 1; i >= 0; i--) {
        pd_pdfr_annot a;

        if (pd_pdfr_annot_get(d, page, i, &a) == PD_OK && a.kind == PD_ANNOT_WIDGET && !a.hidden &&
            x >= a.x0 && x <= a.x1 && y >= a.y0 && y <= a.y1) {
            return a.field;
        }
    }

    return -1;
}

pd_status pd_pdfr_widget_state(pd_pdfr* d, int32_t page, int32_t annot, char* buf, size_t cap) {
    pv a, nap;
    int32_t ne, k;
    pkv* e;

    if (!d || !buf || !cap || page < 0 || page >= d->npages) {
        return PD_ERR_ARG;
    }

    buf[0] = 0;
    a = pd_index(d, annots_of(d, page), annot);

    if (a.t != PV_DICT) {
        return PD_ERR_RANGE;
    }

    nap = pd_get(d, pd_get(d, a, "AP"), "N");
    e = pv_entries(nap, &ne);

    for (k = 0; k < ne; k++) {
        if (strcmp(e[k].key, "Off") != 0) {
            snprintf(buf, cap, "%s", e[k].key);
            break;
        }
    }

    return PD_OK;
}

/* a text field's widget appearance: its text in the font its DA names, inside its border */
static pv text_ap(pdoc* d, pv widget, const fld* f, const char* utf8) {
    double r[4], w, h, size = 0;
    char fname[64];
    pv dr = pd_get(d, pd_get(d, pd_get(d, d->trailer, "Root"), "AcroForm"), "DR"), font, res, fonts, wda;
    pfont* pf;
    pbuf b;
    unsigned char enc[4096];
    size_t n;
    uint32_t col = 0;
    int multiline = (f->ff & (1 << 12)) != 0, comb = (f->ff & (1 << 24)) != 0;
    int64_t maxlen = pd_getint(d, pd_resolve(d, f->ref), "MaxLen", 0), q = pd_getint(d, widget, "Q", f->q);

    if (!pd_rect(d, pd_get(d, widget, "Rect"), r)) {
        return pv_null();
    }

    w = r[2] - r[0];
    h = r[3] - r[1];
    wda = pd_get(d, widget, "DA");
    parse_da(wda.t == PV_STRING ? wda : f->da, fname, sizeof(fname), &size, &col);
    font = pd_get_raw(pd_get(d, pd_get(d, widget, "DR"), "Font"), fname);

    if (font.t == PV_NULL) {
        font = pd_get_raw(pd_get(d, dr, "Font"), fname);
    }

    if (pd_resolve(d, font).t != PV_DICT) {
        font = helv_font(d);
    }

    pf = pf_load(d, font);
    n = encode_in(pf, utf8, enc, sizeof(enc));

    if (!(size > 0)) {  /* auto: as large as the box allows */
        size = multiline ? 12 : (h - 4) * 0.75;

        if (!multiline && pf && n) {
            double tw = text_width(pf, enc, n, 1);

            if (tw > 0 && tw * size > w - 4) {
                size = (w - 4) / tw;
            }
        }

        size = size < 4 ? 4 : size;
    }

    memset(&b, 0, sizeof(b));
    qb_puts(&b, "/Tx BMC q\n");
    rect_op(&b, 1, 1, w - 2, h - 2, "W n");
    qb_printf(&b, "BT /%s ", fname);
    qb_num(&b, size);
    qb_puts(&b, " Tf ");
    col_op(&b, col, "rg");

    if (comb && maxlen > 0 && !multiline) {     /* a character a cell */
        size_t i = 0;
        int cell = 0;

        while (i < n && cell < maxlen) {
            uint32_t code;
            int k = pf_next(pf, enc + i, n - i, &code);
            double cw;

            if (k <= 0) {
                break;
            }

            cw = pf_width(pf, code) * size;
            qb_puts(&b, "1 0 0 1 ");
            qb_num(&b, w / (double)maxlen * (cell + 0.5) - cw / 2);
            qb_puts(&b, " ");
            qb_num(&b, (h - size * 0.7) / 2);
            qb_puts(&b, " Tm ");
            pdf_string(&b, enc + i, (size_t)k);
            qb_puts(&b, " Tj\n");
            i += (size_t)k;
            cell++;
        }
    } else if (multiline) {
        size_t starts[256], ends[256];
        int nl = wrap(pf, enc, n, size, w - 4, starts, ends, 256), i;

        for (i = 0; i < nl; i++) {
            double lw = text_width(pf, enc + starts[i], ends[i] - starts[i], size);
            double x = q == 1 ? (w - lw) / 2 : q == 2 ? w - 2 - lw : 2;

            qb_puts(&b, "1 0 0 1 ");
            qb_num(&b, x);
            qb_puts(&b, " ");
            qb_num(&b, h - 2 - size * (i + 1) * 1.1 + size * 0.1);
            qb_puts(&b, " Tm ");
            pdf_string(&b, enc + starts[i], ends[i] - starts[i]);
            qb_puts(&b, " Tj\n");
        }
    } else {
        double lw = text_width(pf, enc, n, size);
        double x = q == 1 ? (w - lw) / 2 : q == 2 ? w - 2 - lw : 2;

        qb_puts(&b, "1 0 0 1 ");
        qb_num(&b, x);
        qb_puts(&b, " ");
        qb_num(&b, (h - size * 0.7) / 2);
        qb_puts(&b, " Tm ");
        pdf_string(&b, enc, n);
        qb_puts(&b, " Tj\n");
    }

    qb_puts(&b, "ET Q EMC\n");
    res = pv_dict(d, 1);
    fonts = pv_dict(d, 1);
    pd_dict_set(d, &fonts, fname, font);
    pd_dict_set(d, &res, "Font", fonts);

    {
        double bbox[4] = { 0, 0, w, h };
        pv ap = appearance(d, &b, bbox, res);

        qb_free(&b);
        return ap;
    }
}

pd_status pd_pdfr_field_set(pd_pdfr* d, int32_t index, const char* utf8) {
    int32_t n, nw, i, num;
    fld* fs;
    fld f;
    pv node, w[64];

    if (!d || !d->cache) {
        return PD_ERR_ARG;
    }

    fs = fields(d, &n);

    if (index < 0 || index >= n) {
        return PD_ERR_RANGE;
    }

    f = fs[index];

    if (f.ref.t != PV_REF) {
        return PD_ERR_FORMAT;
    }

    if (f.ff & 1) {
        return PD_ERR_STATE;    /* read only */
    }

    num = (int32_t)f.ref.u.i;
    nw = widgets(d, &f, w, 64);
    edit_begin(d);
    node = dict_copy(d, pd_resolve(d, f.ref));

    if (strcmp(f.ft, "Btn") == 0) {
        char st[16][64];
        int32_t k = on_states(d, &f, st, 16);
        const char* on = utf8 && *utf8 ? utf8 : k > 0 ? st[0] : "Yes";

        pd_dict_set(d, &node, "V", pv_name(d, on));
        set_obj(d, num, node);

        for (i = 0; i < nw; i++) {
            pv wd, states;
            int32_t wn;

            if (w[i].t != PV_REF) {
                continue;
            }

            wn = (int32_t)w[i].u.i;
            wd = wn == num ? node : dict_copy(d, pd_resolve(d, w[i]));
            states = pd_get(d, pd_get(d, wd, "AP"), "N");
            pd_dict_set(d, &wd, "AS", pv_name(d, strcmp(on, "Off") != 0 && pd_get(d, states, on).t != PV_NULL ? on : "Off"));
            set_obj(d, wn, wd);

            if (wn == num) {
                node = wd;
            }
        }
    } else {
        pd_dict_set(d, &node, "V", pd_utf8_text(d, utf8 ? utf8 : ""));
        set_obj(d, num, node);

        for (i = 0; i < nw; i++) {
            pv wd, ap;
            int32_t wn;

            if (w[i].t != PV_REF) {
                continue;
            }

            wn = (int32_t)w[i].u.i;
            wd = wn == num ? node : dict_copy(d, pd_resolve(d, w[i]));
            ap = text_ap(d, wd, &f, utf8 ? utf8 : "");

            if (ap.t == PV_STREAM) {
                pv apd = pv_dict(d, 1);

                pd_dict_set(d, &apd, "N", pv_ref(new_obj(d, ap), 0));
                pd_dict_set(d, &wd, "AP", apd);
            }

            set_obj(d, wn, wd);

            if (wn == num) {
                node = wd;
            }
        }
    }

    {
        /* other viewers make their own appearances too, from the value */
        pv rootref = pd_get_raw(d->trailer, "Root"), root = pd_resolve(d, rootref);
        pv afref = pd_get_raw(root, "AcroForm");

        if (afref.t == PV_REF && pd_resolve(d, afref).t == PV_DICT) {
            pv af = dict_copy(d, pd_resolve(d, afref));

            pd_dict_set(d, &af, "NeedAppearances", pv_bool(1));
            set_obj(d, (int32_t)afref.u.i, af);
        } else if (afref.t == PV_DICT && rootref.t == PV_REF) {     /* the form in the catalog itself */
            pv af = dict_copy(d, afref), cat = dict_copy(d, root);

            pd_dict_set(d, &af, "NeedAppearances", pv_bool(1));
            pd_dict_set(d, &cat, "AcroForm", af);
            set_obj(d, (int32_t)rootref.u.i, cat);
        }
    }

    edit_end(d);
    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* page text                                                          */
/* ------------------------------------------------------------------ */

/* a page content stream made new from bytes, in place of the old */
static int replace_content(pdoc* d, int32_t page, int32_t stream, const unsigned char* data, size_t n) {
    pv c = pd_get_raw(d->pages[page].dict, "Contents"), cr = pd_resolve(d, c), st, ref;

    if (cr.t == PV_ARRAY) {
        if (stream < 0 || stream >= cr.n) {
            return 0;
        }

        ref = cr.u.a[stream];
    } else {
        ref = c;
    }

    st = make_stream(d, pv_dict(d, 2), data, n);

    if (ref.t == PV_REF) {
        set_obj(d, (int32_t)ref.u.i, st);
    } else {
        pv pg = dict_copy(d, d->pages[page].dict);

        pd_dict_set(d, &pg, "Contents", pv_ref(new_obj(d, st), 0));
        page_store(d, page, pg);
    }

    return 1;
}

static unsigned char* content_of(pdoc* d, int32_t page, int32_t stream, size_t* n) {
    pv c = pd_get(d, d->pages[page].dict, "Contents");

    if (c.t == PV_ARRAY) {
        c = pd_index(d, c, stream);
    } else if (stream != 0) {
        return NULL;
    }

    return pd_stream_data(d, c, n, NULL, NULL);
}

/* the codes of characters a..z replaced in the operation that shows them, when one operation shows them all and
   the font has the new text's characters: the operation written again as a TJ of what it showed, the new codes in
   place of the old */
static int edit_in_place(pdoc* d, int32_t page, const tchar* first, const tchar* last, const char* utf8) {
    size_t n, start = first->op_start, end = first->op_end, ne = 0;
    unsigned char* data, enc[4096];
    plex L;
    pv ops[8], o;
    int nops = 0, ok = 0;
    pbuf b;

    if (first->stream < 0 || first->op_start != last->op_start || first->stream != last->stream ||
        first->font != last->font || !first->font || first->str_start != last->str_start) {
        return 0;
    }

    if (*utf8) {
        ne = pf_encode(first->font, utf8, enc, sizeof(enc));

        if (ne == 0) {
            return 0;   /* the font has not got the characters */
        }
    }

    data = content_of(d, page, first->stream, &n);

    if (!data || end > n || start >= end) {
        free(data);
        return 0;
    }

    memset(&L, 0, sizeof(L));
    L.p = data;
    L.n = end;
    L.i = start;
    L.d = d;
    L.content = 1;

    for (;;) {
        o = pl_read(&L);

        if (o.t == PV_ERR || o.t == PV_OP) {
            break;
        }

        if (nops < 8) {
            ops[nops++] = o;
        }
    }

    if (o.t != PV_OP || nops == 0) {
        free(data);
        return 0;
    }

    memset(&b, 0, sizeof(b));
    qb_put(&b, data, start);

    {
        pv arr = ops[nops - 1];
        int32_t count = arr.t == PV_ARRAY ? arr.n : 1, i;
        size_t from = first->byte_off, to = last->byte_off + last->byte_len;
        uint32_t sidx = 0;

        if (strcmp(o.u.s, "'") == 0 || strcmp(o.u.s, "\"") == 0) {
            if (strcmp(o.u.s, "\"") == 0 && nops >= 3) {    /* aw ac string ": the spacing kept */
                qb_num(&b, ops[nops - 3].t == PV_INT ? (double)ops[nops - 3].u.i : ops[nops - 3].u.r);
                qb_puts(&b, " Tw ");
                qb_num(&b, ops[nops - 2].t == PV_INT ? (double)ops[nops - 2].u.i : ops[nops - 2].u.r);
                qb_puts(&b, " Tc ");
            }

            qb_puts(&b, "T* ");
        }

        qb_puts(&b, "[");

        for (i = 0; i < count; i++) {
            pv e = arr.t == PV_ARRAY ? arr.u.a[i] : arr;

            if (e.t == PV_STRING) {
                if (sidx == first->str_start) {
                    size_t f0 = from < (size_t)e.n ? from : (size_t)e.n;

                    pdf_string(&b, (const unsigned char*)e.u.s, f0);
                    pdf_string(&b, enc, ne);

                    if (to < (size_t)e.n) {
                        pdf_string(&b, (const unsigned char*)e.u.s + to, (size_t)e.n - to);
                    }

                    ok = 1;
                } else {
                    pdf_string(&b, (const unsigned char*)e.u.s, (size_t)e.n);
                }

                sidx++;
            } else if (e.t == PV_INT || e.t == PV_REAL) {
                qb_num(&b, e.t == PV_INT ? (double)e.u.i : e.u.r);
            }

            qb_puts(&b, " ");
        }

        qb_puts(&b, "] TJ");
    }

    qb_put(&b, data + end, n - end);

    if (ok) {
        ok = replace_content(d, page, first->stream, b.p, b.n);
    }

    qb_free(&b);
    free(data);
    return ok;
}

/* ---- text written anew: wrapped, at the old text's place ---- */

/* the place of text a..z: where its first character begins, the way its lines run, and how they sit */
typedef struct {
    double ox, oy;              /* the first character's origin, user space */
    double ux, uy;              /* along the baseline */
    double size;                /* the font size in user space */
    double left, width;         /* along the baseline from the origin: where the lines begin, how far they reach */
    double leading;             /* from a baseline to the next */
    int lines, align;           /* 0 left, 1 centred, 2 right */
    uint32_t rgb;
} tplace;

static void text_place(const dlist* L, int32_t a, int32_t z, tplace* P) {
    const tchar* f = &L->chars[a];
    double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 }, cur_t = 0, first_t = 0, last_t = 0;
    double line_l = 1e30, line_r = -1e30, minl = 1e30, maxr = -1e30, r;
    int32_t i;
    int nl = 0;

    memset(P, 0, sizeof(*P));
    P->ox = f->um[4];
    P->oy = f->um[5];
    r = atan2(f->um[1], f->um[0]);
    P->ux = cos(r);
    P->uy = sin(r);
    P->size = sqrt(f->um[2] * f->um[2] + f->um[3] * f->um[3]);

    if (!(P->size > 0)) {
        P->size = f->fs > 0 ? f->fs : 10;
    }

    P->rgb = f->rgb;

    for (i = a; i <= z + 1; i++) {
        double s = 0, e = 0, t = 0;
        int k;

        if (i <= z) {
            const tchar* c = &L->chars[i];
            double dx = c->um[4] - P->ox, dy = c->um[5] - P->oy;
            double ex = dx + c->um[0] * c->w0, ey = dy + c->um[1] * c->w0;

            s = dx * P->ux + dy * P->uy;
            e = ex * P->ux + ey * P->uy;
            t = -dx * P->uy + dy * P->ux;

            if (c->pub.cp == ' ') {
                continue;   /* a space at a line's end is not where it reaches */
            }
        }

        /* a new line: the baseline moved across by half the size or more */
        if (i > z || (nl > 0 && fabs(t - cur_t) > 0.5 * P->size)) {
            if (nl > 0) {
                double v[3] = { line_l, (line_l + line_r) / 2, line_r };

                for (k = 0; k < 3; k++) {
                    lo[k] = v[k] < lo[k] ? v[k] : lo[k];
                    hi[k] = v[k] > hi[k] ? v[k] : hi[k];
                }

                minl = line_l < minl ? line_l : minl;
                maxr = line_r > maxr ? line_r : maxr;
                last_t = cur_t;
            }

            if (i > z) {
                break;
            }

            line_l = 1e30;
            line_r = -1e30;
        }

        if (nl == 0 || fabs(t - cur_t) > 0.5 * P->size) {
            if (nl == 0) {
                first_t = t;
            }

            nl++;
            cur_t = t;
        }

        line_l = s < line_l ? s : line_l;
        line_r = e > line_r ? e : line_r;
    }

    P->lines = nl > 0 ? nl : 1;
    P->left = nl > 1 ? minl : 0;
    P->width = maxr - P->left;
    P->leading = nl > 1 ? fabs(first_t - last_t) / (nl - 1) : 1.2 * P->size;

    if (nl > 1 && hi[0] - lo[0] > 0.5 * P->size) {
        P->align = hi[1] - lo[1] <= 0.5 * P->size ? 1 : hi[2] - lo[2] <= 0.5 * P->size ? 2 : 0;
    }
}

/* text's codes in the font: its own codes (0 when it has not got every character), or a standard font's */
static size_t wencode(pfont* pf, int own, const char* utf8, size_t len, unsigned char* out, size_t cap) {
    char tmp[2048];

    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }

    memcpy(tmp, utf8, len);
    tmp[len] = 0;

    if (len == 0) {
        return 0;
    }

    return own ? pf_encode(pf, tmp, out, cap) : encode_in(pf, tmp, out, cap);
}

static double wwidth(pfont* pf, const unsigned char* s, size_t n) {
    size_t i = 0;
    double w = 0;

    while (i < n) {
        uint32_t code;
        int k = pf_next(pf, s + i, n - i, &code);

        if (k <= 0) {
            break;
        }

        w += pf_width(pf, code);
        i += (size_t)k;
    }

    return w;
}

static void write_line(pbuf* b, pfont* pf, int own, const tplace* P, const char* s, size_t len, int k) {
    unsigned char enc[4096];
    size_t n = wencode(pf, own, s, len, enc, sizeof(enc));
    double w = wwidth(pf, enc, n) * P->size, off = 0, x, y;
    double m[6];

    if (P->align == 1) {
        off = (P->width - w) / 2;
    } else if (P->align == 2) {
        off = P->width - w;
    }

    x = P->ox + P->ux * (P->left + off) + P->uy * (k * P->leading);
    y = P->oy + P->uy * (P->left + off) - P->ux * (k * P->leading);
    m[0] = P->ux;
    m[1] = P->uy;
    m[2] = -P->uy;
    m[3] = P->ux;
    m[4] = x;
    m[5] = y;
    nums(b, m, 6);
    qb_puts(b, "Tm ");
    pdf_string(b, enc, n);
    qb_puts(b, " Tj\n");
}

/* utf8 written at P in the font named fname: wrapped to P's width when it was on more than one line, a '\n' a new
   line; the lines written */
static int write_text(pbuf* b, pfont* pf, int own, const char* fname, const tplace* P, const char* utf8) {
    const char* p = utf8;
    int k = 0;
    int wrap = P->lines > 1 && P->width > 0;

    qb_puts(b, "BT /");
    qb_puts(b, fname);
    qb_puts(b, " ");
    qb_num(b, P->size);
    qb_puts(b, " Tf ");
    col_op(b, P->rgb, "rg");

    for (;;) {
        const char* nl = strchr(p, '\n');
        size_t plen = nl ? (size_t)(nl - p) : strlen(p);

        if (!wrap) {
            write_line(b, pf, own, P, p, plen, k++);
        } else {
            /* the words of the paragraph, as many on a line as fit */
            size_t ls = 0, le = 0, i = 0;

            while (i <= plen) {
                size_t ws = i, we;
                unsigned char enc[4096];
                double w;

                while (ws < plen && p[ws] == ' ') {
                    ws++;
                }

                we = ws;

                while (we < plen && p[we] != ' ') {
                    we++;
                }

                if (ws >= plen) {
                    break;
                }

                if (le > ls) {
                    w = wwidth(pf, enc, wencode(pf, own, p + ls, we - ls, enc, sizeof(enc))) * P->size;

                    if (w > P->width * 1.02) {
                        write_line(b, pf, own, P, p + ls, le - ls, k++);
                        ls = ws;
                    }
                } else {
                    ls = ws;
                }

                le = we;
                i = we;
            }

            write_line(b, pf, own, P, p + ls, le > ls ? le - ls : 0, k++);
        }

        if (!nl) {
            break;
        }

        p = nl + 1;
    }

    qb_puts(b, "ET");
    return k;
}

/* a name for a font not yet among the page's (each edit's text keeps the font it was written in) */
static void new_font_name(pdoc* d, int32_t page, char* name, size_t cap) {
    pv res = pd_get(d, d->pages[page].dict, "Resources"), fonts;
    int i;

    if (res.t != PV_DICT) {
        res = d->pages[page].resources;
    }

    fonts = pd_get(d, res, "Font");

    for (i = 1; i < 10000; i++) {
        snprintf(name, cap, "PdT%d", i);

        if (fonts.t != PV_DICT || pd_get_raw(fonts, name).t == PV_NULL) {
            return;
        }
    }
}

/* b added to the page after what it showed (that wrapped in q ... Q), with the font under fname */
static void append_to_page(pdoc* d, int32_t page, const pbuf* b, const char* fname, pv font) {
    pv pg = dict_copy(d, d->pages[page].dict), res, fonts, c, arr, list;
    int32_t num, qs, qe, i;

    res = pd_get(d, pg, "Resources");

    if (res.t != PV_DICT) {
        res = d->pages[page].resources;
    }

    res = dict_copy(d, res);
    fonts = dict_copy(d, pd_get(d, res, "Font"));
    pd_dict_set(d, &fonts, fname, font);
    pd_dict_set(d, &res, "Font", fonts);
    pd_dict_set(d, &pg, "Resources", res);
    c = pd_get_raw(pg, "Contents");
    arr = pd_resolve(d, c);
    num = new_obj(d, make_stream(d, pv_dict(d, 2), b->p, b->n));
    qs = new_obj(d, make_stream(d, pv_dict(d, 1), (const unsigned char*)"q\n", 2));
    qe = new_obj(d, make_stream(d, pv_dict(d, 1), (const unsigned char*)"\nQ\n", 3));

    if (arr.t == PV_ARRAY) {
        list = pv_array(d, arr.n + 3);
        list.u.a[0] = pv_ref(qs, 0);

        for (i = 0; i < arr.n; i++) {
            list.u.a[i + 1] = arr.u.a[i];
        }

        list.u.a[arr.n + 1] = pv_ref(qe, 0);
        list.u.a[arr.n + 2] = pv_ref(num, 0);
    } else {
        list = pv_array(d, 4);
        list.u.a[0] = pv_ref(qs, 0);
        list.u.a[1] = c;
        list.u.a[2] = pv_ref(qe, 0);
        list.u.a[3] = pv_ref(num, 0);
    }

    pd_dict_set(d, &pg, "Contents", list);
    page_store(d, page, pg);
}

/* the font the new text is written in: the old text's own when it has every character (and is not a Type 3's
   procedures), else the standard font most like it */
static pfont* pick_font(pdoc* d, const tchar* f, const char* utf8, int* own, pv* font) {
    unsigned char enc[4096];
    char tmp[2048];
    size_t i, k = 0;

    for (i = 0; utf8[i] && k + 1 < sizeof(tmp); i++) {
        if (utf8[i] != '\n') {
            tmp[k++] = utf8[i];
        }
    }

    tmp[k] = 0;
    *own = 0;

    if (f->font && !pf_is_type3(f->font) && !f->vertical && pf_dict(f->font).t == PV_DICT &&
        (k == 0 || pf_encode(f->font, tmp, enc, sizeof(enc)) > 0)) {
        *own = 1;
        *font = pf_dict(f->font);
        return f->font;
    }

    *font = std_font(d, pf_std14(f->font));
    return pf_load(d, *font);
}

/* the old text covered in white, the new written over it (a stream added to the page) */
static int edit_overlay(pdoc* d, int32_t page, dlist* L, int32_t a, int32_t z, const char* utf8) {
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    int32_t i;
    pbuf b;
    pv font = std_font(d, pf_std14(L->chars[a].font));
    pfont* pf = pf_load(d, font);
    double binv[6];
    char fname[32];
    tplace P;

    page_inv(d, page, binv);

    /* the characters' boxes back in user space */
    for (i = a; i <= z; i++) {
        const tchar* t = &L->chars[i];
        double xs[4], ys[4];
        int k;

        mat_pt(binv, t->pub.x0, t->pub.y0, &xs[0], &ys[0]);
        mat_pt(binv, t->pub.x1, t->pub.y0, &xs[1], &ys[1]);
        mat_pt(binv, t->pub.x1, t->pub.y1, &xs[2], &ys[2]);
        mat_pt(binv, t->pub.x0, t->pub.y1, &xs[3], &ys[3]);

        for (k = 0; k < 4; k++) {
            x0 = xs[k] < x0 ? xs[k] : x0;
            y0 = ys[k] < y0 ? ys[k] : y0;
            x1 = xs[k] > x1 ? xs[k] : x1;
            y1 = ys[k] > y1 ? ys[k] : y1;
        }
    }

    if (!pf) {
        return 0;
    }

    text_place(L, a, z, &P);
    new_font_name(d, page, fname, sizeof(fname));
    memset(&b, 0, sizeof(b));
    qb_puts(&b, "q 1 1 1 rg ");
    rect_op(&b, x0, y0, x1 - x0, y1 - y0, "f");
    write_text(&b, pf, 0, fname, &P, utf8);
    qb_puts(&b, " Q\n");
    append_to_page(d, page, &b, fname, font);
    qb_free(&b);
    return 1;
}

/* ---- characters taken out ---- */

typedef struct {
    int32_t stream;
    uint32_t start, end;
} topref;

static int topref_cmp(const void* x, const void* y) {
    const topref* a = (const topref*)x;
    const topref* b = (const topref*)y;

    if (a->stream != b->stream) {
        return a->stream < b->stream ? -1 : 1;
    }

    return a->start > b->start ? -1 : a->start < b->start ? 1 : 0;     /* the last first: offsets before it hold */
}

/* operation o (the one at start..end of data) written again without characters a..z of it: each string's
   characters taken out leave a TJ gap of their advance, so what is shown after them stays where it was */
static int rewrite_op(pdoc* d, const unsigned char* data, uint32_t start, uint32_t end, const dlist* L, int32_t a,
                      int32_t z, pbuf* b) {
    plex X;
    pv ops[8], o;
    int nops = 0;
    pv arr;
    int32_t count, i;
    uint32_t sidx = 0;

    memset(&X, 0, sizeof(X));
    X.p = data;
    X.n = end;
    X.i = start;
    X.d = d;
    X.content = 1;

    for (;;) {
        o = pl_read(&X);

        if (o.t == PV_ERR || o.t == PV_OP) {
            break;
        }

        if (nops < 8) {
            ops[nops++] = o;
        }
    }

    if (o.t != PV_OP || nops == 0) {
        return 0;
    }

    if (strcmp(o.u.s, "'") == 0 || strcmp(o.u.s, "\"") == 0) {
        if (strcmp(o.u.s, "\"") == 0 && nops >= 3) {
            qb_num(b, ops[nops - 3].t == PV_INT ? (double)ops[nops - 3].u.i : ops[nops - 3].u.r);
            qb_puts(b, " Tw ");
            qb_num(b, ops[nops - 2].t == PV_INT ? (double)ops[nops - 2].u.i : ops[nops - 2].u.r);
            qb_puts(b, " Tc ");
        }

        qb_puts(b, "T* ");
    } else if (strcmp(o.u.s, "Tj") != 0 && strcmp(o.u.s, "TJ") != 0) {
        return 0;
    }

    arr = ops[nops - 1];
    count = arr.t == PV_ARRAY ? arr.n : 1;
    qb_puts(b, "[");

    for (i = 0; i < count; i++) {
        pv e = arr.t == PV_ARRAY ? arr.u.a[i] : arr;

        if (e.t == PV_STRING) {
            size_t from = (size_t)-1, to = 0;
            double gap = 0;
            int32_t j;
            int64_t lastoff = -1;

            for (j = a; j <= z; j++) {
                const tchar* c = &L->chars[j];

                if (c->op_start != start || c->str_start != sidx) {
                    continue;
                }

                from = c->byte_off < from ? c->byte_off : from;
                to = (size_t)c->byte_off + c->byte_len > to ? (size_t)c->byte_off + c->byte_len : to;

                if ((int64_t)c->byte_off != lastoff) {  /* a ligature's characters: one glyph */
                    int space = c->byte_len == 1 && c->byte_off < (uint32_t)e.n && e.u.s[c->byte_off] == ' ';

                    gap += c->w0 + (c->tc + (space ? c->tw : 0)) / c->fs;
                    lastoff = c->byte_off;
                }
            }

            if (from == (size_t)-1) {
                pdf_string(b, (const unsigned char*)e.u.s, (size_t)e.n);
            } else {
                to = to < (size_t)e.n ? to : (size_t)e.n;
                from = from < to ? from : to;

                if (from > 0) {
                    pdf_string(b, (const unsigned char*)e.u.s, from);
                    qb_puts(b, " ");
                }

                qb_num(b, -1000 * gap);

                if (to < (size_t)e.n) {
                    qb_puts(b, " ");
                    pdf_string(b, (const unsigned char*)e.u.s + to, (size_t)e.n - to);
                }
            }

            sidx++;
        } else if (e.t == PV_INT || e.t == PV_REAL) {
            qb_num(b, e.t == PV_INT ? (double)e.u.i : e.u.r);
        }

        qb_puts(b, " ");
    }

    qb_puts(b, "] TJ");
    return 1;
}

/* characters a..z taken out of the page's content streams; 0 (nothing changed) when one of them cannot be: in a
   form XObject, an operation across two streams, a vertical font */
static int take_out(pdoc* d, int32_t page, const dlist* L, int32_t a, int32_t z) {
    topref* ops;
    int32_t nops = 0, i, j;
    int ok = 1;

    for (i = a; i <= z; i++) {
        const tchar* c = &L->chars[i];

        if (c->stream < 0 || !c->font || c->vertical || !(fabs(c->fs) > 0)) {
            return 0;
        }
    }

    ops = (topref*)malloc(sizeof(topref) * (size_t)(z - a + 1));

    if (!ops) {
        return 0;
    }

    for (i = a; i <= z; i++) {
        const tchar* c = &L->chars[i];

        for (j = 0; j < nops; j++) {
            if (ops[j].stream == c->stream && ops[j].start == c->op_start) {
                break;
            }
        }

        if (j == nops) {
            ops[nops].stream = c->stream;
            ops[nops].start = c->op_start;
            ops[nops].end = c->op_end;
            nops++;
        }
    }

    qsort(ops, (size_t)nops, sizeof(topref), topref_cmp);

    /* each stream once: its operations written again from the last, the bytes before each kept */
    for (i = 0; i < nops && ok;) {
        int32_t s = ops[i].stream, k;
        size_t n;
        unsigned char* data = content_of(d, page, s, &n);
        pbuf work;

        if (!data) {
            ok = 0;
            break;
        }

        memset(&work, 0, sizeof(work));
        qb_put(&work, data, n);

        for (k = i; k < nops && ops[k].stream == s; k++) {
            pbuf op, next;

            if (ops[k].end > n || ops[k].start >= ops[k].end) {
                ok = 0;
                break;
            }

            memset(&op, 0, sizeof(op));

            if (!rewrite_op(d, data, ops[k].start, ops[k].end, L, a, z, &op)) {
                qb_free(&op);
                ok = 0;
                break;
            }

            /* work holds data with the later operations done: the bytes after this one's end are where they were
               less what those changed, so the splice is made from the end of work */
            memset(&next, 0, sizeof(next));
            qb_put(&next, work.p, ops[k].start);
            qb_put(&next, op.p, op.n);
            qb_put(&next, work.p + ops[k].end, work.n - ops[k].end);
            qb_free(&work);
            work = next;
            qb_free(&op);
        }

        if (ok) {
            ok = replace_content(d, page, s, work.p, work.n);
        }

        qb_free(&work);
        free(data);
        i = k;
    }

    free(ops);
    return ok;
}

/* characters a..z taken out and utf8 written where they began, wrapped as they were */
static int edit_reflow(pdoc* d, int32_t page, dlist* L, int32_t a, int32_t z, const char* utf8) {
    tplace P;
    pv font;
    int own;
    pfont* pf;
    char fname[32];
    pbuf b;

    text_place(L, a, z, &P);
    pf = pick_font(d, &L->chars[a], utf8, &own, &font);

    if (!pf) {
        return 0;
    }

    if (!take_out(d, page, L, a, z)) {
        return 0;
    }

    if (*utf8) {
        new_font_name(d, page, fname, sizeof(fname));
        memset(&b, 0, sizeof(b));
        qb_puts(&b, "q ");
        write_text(&b, pf, own, fname, &P, utf8);
        qb_puts(&b, " Q\n");
        append_to_page(d, page, &b, fname, font);
        qb_free(&b);
    }

    return 1;
}

pd_status pd_pdfr_replace_text(pd_pdfr* d, int32_t page, int32_t from, int32_t to, const char* utf8) {
    dlist* L;
    int32_t n, *map, a = -1, z = -1, i;
    int ok;

    if (!valid_page(d, page) || !utf8 || from < 0 || to <= from) {
        return PD_ERR_ARG;
    }

    L = pd_page_list(d, page);

    if (!L) {
        return PD_ERR_RANGE;
    }

    n = pd_text_build(L, NULL, 0, NULL);
    map = (int32_t*)malloc(sizeof(int32_t) * (size_t)(n + 1));

    if (!map) {
        return PD_ERR_NOMEM;
    }

    pd_text_build(L, NULL, n, map);

    for (i = from; i < to && i < n; i++) {
        if (map[i] >= 0) {
            if (a < 0) {
                a = map[i];
            }

            z = map[i];
        }
    }

    free(map);

    if (a < 0) {
        return PD_ERR_RANGE;
    }

    edit_begin(d);
    ok = !strchr(utf8, '\n') && edit_in_place(d, page, &L->chars[a], &L->chars[z], utf8);

    if (!ok) {
        ok = edit_reflow(d, page, L, a, z, utf8);
    }

    if (!ok) {
        ok = edit_overlay(d, page, L, a, z, utf8);
    }

    edit_end(d);
    return ok ? PD_OK : PD_ERR_FORMAT;
}

/* ------------------------------------------------------------------ */
/* writing                                                            */
/* ------------------------------------------------------------------ */

static void write_name(pbuf* b, const char* s) {
    qb_puts(b, "/");

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (c < 33 || c > 126 || c == '#' || c == '/' || c == '(' || c == ')' || c == '<' || c == '>' || c == '[' ||
            c == ']' || c == '{' || c == '}' || c == '%') {
            qb_printf(b, "#%02X", c);
        } else {
            qb_put(b, &c, 1);
        }
    }
}

static void write_string(pdoc* d, pbuf* b, pv v, int32_t num, int32_t gen) {
    const unsigned char* p = (const unsigned char*)v.u.s;
    size_t n = (size_t)v.n, i;
    unsigned char* enc = NULL;

    if (d->keylen && d->crypt_strings && num > 0) {
        size_t en;

        enc = pd_encrypt(d, num, gen, d->crypt_strings, p, n, &en);

        if (enc) {
            p = enc;
            n = en;
        }
    }

    qb_puts(b, "<");

    for (i = 0; i < n; i++) {
        qb_printf(b, "%02X", p[i]);
    }

    qb_puts(b, ">");
    free(enc);
}

static void write_value(pdoc* d, pbuf* b, pv v, int32_t num, int32_t gen, int depth) {
    int32_t i, n;
    pkv* e;

    if (depth > 64) {
        qb_puts(b, "null");
        return;
    }

    switch (v.t) {
        case PV_NULL: qb_puts(b, "null"); break;
        case PV_BOOL: qb_puts(b, v.u.i ? "true" : "false"); break;
        case PV_INT: qb_printf(b, "%lld", (long long)v.u.i); break;
        case PV_REAL: qb_num(b, v.u.r); break;
        case PV_NAME: write_name(b, v.u.s); break;
        case PV_STRING: write_string(d, b, v, num, gen); break;
        case PV_REF: qb_printf(b, "%d %d R", (int)v.u.i, (int)v.n); break;

        case PV_ARRAY:
            qb_puts(b, "[");

            for (i = 0; i < v.n; i++) {
                if (i) {
                    qb_puts(b, " ");
                }

                write_value(d, b, v.u.a[i], num, gen, depth + 1);
            }

            qb_puts(b, "]");
            break;

        case PV_DICT: case PV_STREAM:
            e = pv_entries(v, &n);
            qb_puts(b, "<<");

            for (i = 0; i < n; i++) {
                if (v.t == PV_STREAM && strcmp(e[i].key, "Length") == 0) {
                    continue;   /* written with the data */
                }

                write_name(b, e[i].key);
                qb_puts(b, " ");
                write_value(d, b, e[i].val, num, gen, depth + 1);
                qb_puts(b, " ");
            }

            if (v.t == PV_DICT) {
                qb_puts(b, ">>");
            }

            break;

        default:
            qb_puts(b, "null");
    }
}

static void write_object(pdoc* d, pbuf* b, int32_t num) {
    pxref* x = &d->xref[num];
    pv v = x->obj ? *x->obj : pv_null();
    int32_t gen = x->gen > 0 ? x->gen : 0;

    qb_printf(b, "%d %d obj\n", (int)num, (int)gen);
    write_value(d, b, v, num, gen, 0);

    if (v.t == PV_STREAM) {
        pstream* st = v.u.st;
        const unsigned char* data;
        size_t n;
        unsigned char* enc = NULL;

        if (st->data) {     /* made in this session: encrypted now if the file is */
            data = st->data;
            n = (size_t)st->len;

            if (d->keylen && d->crypt_streams) {
                size_t en;

                enc = pd_encrypt(d, num, gen, d->crypt_streams, data, n, &en);

                if (enc) {
                    data = enc;
                    n = en;
                }
            }
        } else {            /* the file's own bytes, as they were (encrypted, if they were, with this object's key) */
            data = d->data + st->off;
            n = (size_t)st->len;

            if ((size_t)st->off + n > d->len) {
                n = st->off < (int64_t)d->len ? d->len - (size_t)st->off : 0;
            }
        }

        qb_printf(b, "/Length %zu>>\nstream\n", n);
        qb_put(b, data, n);
        qb_puts(b, "\nendstream");
        free(enc);
    }

    qb_puts(b, "\nendobj\n");
}

pd_status pd_pdfr_save(pd_pdfr* d, pd_writer fn, void* user) {
    pbuf b;
    int32_t i, *nums_ = NULL, ndirty = 0;
    int64_t* offs = NULL;
    size_t base;
    int64_t xref_at;
    pv t;
    static const char* const keep[] = { "Root", "Info", "Encrypt", "ID" };
    size_t k;

    if (!d || !fn) {
        return PD_ERR_ARG;
    }

    for (i = 1; i < d->nxref; i++) {
        if (d->xref[i].dirty) {
            ndirty++;
        }
    }

    if (fn(user, d->data, d->len)) {
        return PD_ERR_IO;
    }

    if (ndirty == 0) {
        return PD_OK;
    }

    memset(&b, 0, sizeof(b));
    base = d->len;

    if (d->len && d->data[d->len - 1] != '\n' && d->data[d->len - 1] != '\r') {
        qb_puts(&b, "\n");
    }

    offs = (int64_t*)calloc((size_t)ndirty + 1, sizeof(int64_t));
    nums_ = (int32_t*)calloc((size_t)ndirty + 1, sizeof(int32_t));

    if (!offs || !nums_) {
        free(offs);
        free(nums_);
        return PD_ERR_NOMEM;
    }

    ndirty = 0;

    for (i = 1; i < d->nxref; i++) {
        if (d->xref[i].dirty) {
            offs[ndirty] = (int64_t)(base + b.n);
            nums_[ndirty] = i;
            ndirty++;
            write_object(d, &b, i);
        }
    }

    xref_at = (int64_t)(base + b.n);

    /* the trailer's entries kept, Prev to the last section, Size the objects there are */
    t = pv_dict(d, 8);

    for (k = 0; k < sizeof(keep) / sizeof(keep[0]); k++) {
        pv v = pd_get_raw(d->trailer, keep[k]);

        if (v.t != PV_NULL) {
            pd_dict_set(d, &t, keep[k], v);
        }
    }

    if (d->last_xref > 0) {
        pd_dict_set(d, &t, "Prev", pv_int(d->last_xref));
    }

    if (d->xref_stream) {
        /* a cross-reference stream, as the file has: W [1 4 2], the objects in runs, the stream itself last */
        pbuf rows;
        pv index, w;
        int32_t self = d->nxref, j, start = -1, cnt = 0, nidx = 0;
        pv runs[512];

        memset(&rows, 0, sizeof(rows));
        offs[ndirty] = xref_at;
        nums_[ndirty] = self;

        for (j = 0; j <= ndirty; j++) {
            unsigned char row[7];
            int64_t o = offs[j];

            if (start < 0 || nums_[j] != start + cnt) {
                if (start >= 0 && nidx + 2 <= 512) {
                    runs[nidx++] = pv_int(start);
                    runs[nidx++] = pv_int(cnt);
                }

                start = nums_[j];
                cnt = 0;
            }

            row[0] = 1;
            row[1] = (unsigned char)(o >> 24);
            row[2] = (unsigned char)(o >> 16);
            row[3] = (unsigned char)(o >> 8);
            row[4] = (unsigned char)o;
            row[5] = 0;
            row[6] = 0;
            qb_put(&rows, row, 7);
            cnt++;
        }

        if (nidx + 2 <= 512) {
            runs[nidx++] = pv_int(start);
            runs[nidx++] = pv_int(cnt);
        }

        index = pv_array(d, nidx);
        memcpy(index.u.a, runs, sizeof(pv) * (size_t)nidx);
        w = pv_array(d, 3);
        w.u.a[0] = pv_int(1);
        w.u.a[1] = pv_int(4);
        w.u.a[2] = pv_int(2);
        pd_dict_set(d, &t, "Type", pv_name(d, "XRef"));
        pd_dict_set(d, &t, "Size", pv_int(self + 1));
        pd_dict_set(d, &t, "W", w);
        pd_dict_set(d, &t, "Index", index);
        pd_dict_set(d, &t, "Length", pv_int((int64_t)rows.n));
        qb_printf(&b, "%d 0 obj\n", (int)self);
        write_value(d, &b, t, 0, 0, 0);
        qb_puts(&b, "\nstream\n");
        qb_put(&b, rows.p, rows.n);
        qb_puts(&b, "\nendstream\nendobj\n");
        qb_free(&rows);
    } else {
        int32_t j = 0;

        pd_dict_set(d, &t, "Size", pv_int(d->nxref));
        qb_puts(&b, "xref\n");

        while (j < ndirty) {
            int32_t e = j;

            while (e + 1 < ndirty && nums_[e + 1] == nums_[e] + 1) {
                e++;
            }

            qb_printf(&b, "%d %d\n", (int)nums_[j], (int)(e - j + 1));

            for (; j <= e; j++) {
                qb_printf(&b, "%010lld %05d n \n", (long long)offs[j],
                          (int)(d->xref[nums_[j]].gen > 0 ? d->xref[nums_[j]].gen : 0));
            }
        }

        qb_puts(&b, "trailer\n");
        write_value(d, &b, t, 0, 0, 0);
        qb_puts(&b, "\n");
    }

    qb_printf(&b, "startxref\n%lld\n%%%%EOF\n", (long long)xref_at);
    free(offs);
    free(nums_);

    if (b.err || fn(user, b.p, b.n)) {
        qb_free(&b);
        return PD_ERR_IO;
    }

    qb_free(&b);
    return PD_OK;
}

typedef struct {
    FILE* fp;
} fsink;

static int file_sink(void* user, const void* data, size_t len) {
    return fwrite(data, 1, len, ((fsink*)user)->fp) == len ? 0 : 1;
}

pd_status pd_pdfr_save_file(pd_pdfr* d, const char* path) {
    char tmp[1100];
    fsink s;
    pd_status st;

    if (!d || !path) {
        return PD_ERR_ARG;
    }

    /* written beside it, then put in its place: the file is never half written */
    snprintf(tmp, sizeof(tmp), "%s.parade-tmp", path);
    s.fp = fopen(tmp, "wb");

    if (!s.fp) {
        return PD_ERR_IO;
    }

    st = pd_pdfr_save(d, file_sink, &s);

    if (fclose(s.fp) != 0 && st == PD_OK) {
        st = PD_ERR_IO;
    }

    if (st != PD_OK) {
        remove(tmp);
        return st;
    }

    remove(path);

    if (rename(tmp, path) != 0) {
        return PD_ERR_IO;
    }

    d->saved_changes = d->changes;
    d->modified = 0;
    return PD_OK;
}

int32_t pd_pdfr_modified(const pd_pdfr* d) {
    return d ? d->modified : 0;
}

int32_t pd_pdfr_changes(const pd_pdfr* d) {
    return d ? d->changes : 0;
}
