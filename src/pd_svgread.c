/*
 * pd_svgread.c - SVG pictures as Parade drawings
 *
 * An SVG is read into a small tree and played into the drawing description
 * the layout draws (application/vnd.parade.drawing+json), as metafiles are
 * (pd_emf.c): paths (lines, Bezier curves and arcs flattened; each ring
 * oriented so that the layout's non-zero filling gives what fill-rule
 * asks), rectangles with round corners, circles, ellipses, lines,
 * polylines and polygons; fills and strokes with their opacity, widths,
 * dashes, ends and corners; linear and radial gradients (their stops put
 * along the shape's box the way the layout runs gradients); text as
 * labels at their reference points; pictures given as data: URIs; <use>
 * and <symbol>; nested <svg> viewports; transforms. Styles come from
 * presentation attributes, the style attribute and <style> sheets (type,
 * class and id selectors; of a selector with combinators only its last
 * part is matched). Not drawn: clip paths and masks (what they cut off is
 * shown), filters, patterns (their fallback colour), markers, and pictures
 * by address.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"
#include "pd_doc_internal.h"
#include "pd_internal.h"
#include "parade_layout.h"
#include "pd_paint.h"

#define SV_PI 3.14159265358979323846
#define SV_MAX_DEPTH 48
#define SV_MAX_POINTS 4000000      /* points over the whole picture: past this, shapes are left out */
#define SP_PER_PX (65536.0 * 0.75)
#define SV_FAR 4e8                 /* sp (85 inches): points past this are held there */

/* ------------------------------------------------------------------ */
/* the tree                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char tag[24];               /* local name; "" for text */
    const char* a;              /* attributes, raw */
    size_t an;
    const char* t;              /* text (raw, entities not decoded) */
    size_t tn;
    int parent, kid, last, next;
} snode;

typedef struct {
    int spec;                   /* specificity: id 100, class 10, type 1 */
    int order;
    char tag[24];
    char id[64];
    char cls[4][48];
    int ncls;
    const char* d;              /* declarations */
    size_t dn;
} srule;

typedef struct {
    double m[6];                /* x' = m0 x + m2 y + m4, y' = m1 x + m3 y + m5: user units to the drawing's (sp) */
    /* inherited */
    char fill[96], stroke[96], color[48];
    double fill_op, stroke_op, sw;
    int evenodd, cap, join;
    char dash[160];
    double dash_off;
    double fsize;               /* font size, user units */
    char family[64];
    int weight, italic, anchor;
    int hidden;
    /* not inherited */
    double opacity;             /* the group opacities above, multiplied */
} sstate;

typedef struct {
    pd_doc* d;
    const char* s;
    size_t n;
    snode* nd;
    int nn, capn;
    srule* ru;
    int nru, capru;
    pd_buf* o;
    int first;
    double vw, vh;              /* the viewport, for percentages */
    long npts;
    int depth;
} sv_t;

/* a coordinate as written: held within SV_FAR, 0 for what is no number (an overflow's infinity times zero) */
static long far(double v) {
    return v != v ? 0 : v > SV_FAR ? (long)SV_FAR : v < -SV_FAR ? -(long)SV_FAR : lround(v);
}

/* src into dst (cap bytes), cut short to fit */
static void scpy(char* dst, size_t cap, const char* src) {
    size_t n = strlen(src);

    n = n < cap ? n : cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static int name_ch(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == ':' || c == '.';
}

static int ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

static int add_node(sv_t* S, int parent, const char* tag) {
    snode* x;

    if (S->nn >= S->capn) {
        int cap = S->capn ? S->capn * 2 : 256;
        snode* g = (snode*)realloc(S->nd, (size_t)cap * sizeof(snode));

        if (!g) {
            return -1;
        }

        S->nd = g;
        S->capn = cap;
    }

    x = &S->nd[S->nn];
    memset(x, 0, sizeof(*x));
    scpy(x->tag, sizeof(x->tag), tag);
    x->parent = parent;
    x->kid = x->last = x->next = -1;

    if (parent >= 0) {
        if (S->nd[parent].last >= 0) {
            S->nd[S->nd[parent].last].next = S->nn;
        } else {
            S->nd[parent].kid = S->nn;
        }

        S->nd[parent].last = S->nn;
    }

    return S->nn++;
}

static int build_tree(sv_t* S) {
    pd_markup m;
    int cur = -1, root = -1, t;

    mu_init(&m, S->s, S->n, 0);

    while ((t = mu_next(&m)) != MT_END) {
        if (t == MT_OPEN || t == MT_EMPTY) {
            int k = add_node(S, cur, mu_local(m.name));

            if (k < 0) {
                return -1;
            }

            S->nd[k].a = m.attrs;
            S->nd[k].an = m.alen;

            if (root < 0 && !strcmp(S->nd[k].tag, "svg")) {
                root = k;
            }

            if (t == MT_OPEN) {
                cur = k;
            }
        } else if (t == MT_CLOSE) {
            if (cur >= 0) {
                cur = S->nd[cur].parent;
            }
        } else if (t == MT_TEXT && cur >= 0 && m.tlen > 0) {
            int k = add_node(S, cur, "");

            if (k < 0) {
                return -1;
            }

            S->nd[k].t = m.text;
            S->nd[k].tn = m.tlen;
        }
    }

    return root;
}

/* an attribute's raw value (entities not decoded), its length in *len; NULL if absent */
static const char* attr(const snode* x, const char* name, size_t* len) {
    const char* a = x->a;
    size_t n = x->an, i = 0, ln = strlen(name);

    while (a && i < n) {
        size_t ks, ke;
        char q;

        while (i < n && !name_ch(a[i])) {
            i++;
        }

        ks = i;

        while (i < n && name_ch(a[i])) {
            i++;
        }

        ke = i;

        while (i < n && ws(a[i])) {
            i++;
        }

        if (i >= n || a[i] != '=') {
            continue;
        }

        i++;

        while (i < n && ws(a[i])) {
            i++;
        }

        if (i >= n) {
            break;
        }

        q = a[i];

        if (q == '"' || q == '\'') {
            size_t vs = ++i;

            while (i < n && a[i] != q) {
                i++;
            }

            if (ke - ks == ln && !memcmp(a + ks, name, ln)) {
                *len = i - vs;
                return a + vs;
            }

            i++;
        }
    }

    return NULL;
}

/* an attribute, entities decoded, into buf; 1 if present */
static int attr_s(const snode* x, const char* name, char* buf, size_t cap) {
    size_t n = 0, i, j = 0;
    const char* v = attr(x, name, &n);

    if (!v || cap == 0) {
        return 0;
    }

    for (i = 0; i < n && j + 1 < cap; i++) {
        if (v[i] == '&') {
            size_t e = i;

            while (e < n && e - i < 12 && v[e] != ';') {
                e++;
            }

            if (e < n && v[e] == ';') {
                pd_buf t;

                memset(&t, 0, sizeof(t));
                mu_decode(v + i, e - i + 1, &t);

                if (t.p && t.n + j < cap) {
                    memcpy(buf + j, t.p, t.n);
                    j += t.n;
                }

                pb_free(&t);
                i = e;
                continue;
            }
        }

        buf[j++] = v[i];
    }

    buf[j] = 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* style sheets                                                       */
/* ------------------------------------------------------------------ */

static void add_rule(sv_t* S, const char* sel, size_t sn, const char* d, size_t dn) {
    srule r;
    size_t i = sn, k;

    while (i > 0 && ws(sel[i - 1])) {
        i--;
    }

    sn = i;

    while (i > 0 && !ws(sel[i - 1]) && sel[i - 1] != '>' && sel[i - 1] != '+' && sel[i - 1] != '~') {
        i--;    /* the last compound selector */
    }

    memset(&r, 0, sizeof(r));
    r.d = d;
    r.dn = dn;
    r.order = S->nru;

    for (k = i; k < sn;) {
        char kind = sel[k];
        size_t e = k + (kind == '.' || kind == '#' ? 1 : 0), q = 0;
        char word[64];

        while (e < sn && sel[e] != '.' && sel[e] != '#' && sel[e] != '[' && sel[e] != ':') {
            if (q + 1 < sizeof(word)) {
                word[q++] = sel[e];
            }

            e++;
        }

        word[q] = 0;

        if (kind == '[' || kind == ':') {   /* attribute selectors and pseudo-classes: not matched, so no match */
            return;
        } else if (kind == '.') {
            if (r.ncls < 4) {
                scpy(r.cls[r.ncls++], sizeof(r.cls[0]), word);
            }

            r.spec += 10;
        } else if (kind == '#') {
            scpy(r.id, sizeof(r.id), word);
            r.spec += 100;
        } else if (strcmp(word, "*") != 0) {
            scpy(r.tag, sizeof(r.tag), word);
            r.spec += 1;
        }

        k = e > k ? e : k + 1;
    }

    if (S->nru >= S->capru) {
        int cap = S->capru ? S->capru * 2 : 32;
        srule* g = (srule*)realloc(S->ru, (size_t)cap * sizeof(srule));

        if (!g) {
            return;
        }

        S->ru = g;
        S->capru = cap;
    }

    S->ru[S->nru++] = r;
}

static void parse_css(sv_t* S, const char* s, size_t n) {
    size_t i = 0;

    while (i < n) {
        size_t a, b, c;

        while (i < n && ws(s[i])) {
            i++;
        }

        if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
            for (i += 2; i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'); i++) {
            }

            i += 2;
            continue;
        }

        if (i < n && s[i] == '@') {     /* @media, @font-face, @import: left out, with their block */
            int depth = 0;

            for (; i < n; i++) {
                if (s[i] == '{') {
                    depth++;
                } else if (s[i] == '}' && --depth <= 0) {
                    i++;
                    break;
                } else if (s[i] == ';' && depth == 0) {
                    i++;
                    break;
                }
            }

            continue;
        }

        a = i;

        while (i < n && s[i] != '{') {
            i++;
        }

        if (i >= n) {
            break;
        }

        b = i++;
        c = i;

        while (i < n && s[i] != '}') {
            i++;
        }

        {   /* each selector of a list */
            size_t p = a, q;

            for (q = a; q <= b; q++) {
                if (q == b || s[q] == ',') {
                    add_rule(S, s + p, q - p, s + c, i - c);
                    p = q + 1;
                }
            }
        }

        i++;
    }
}

/* the value of a property in declarations ("fill: red; stroke: none"), into buf; 1 if there */
static int decl_get(const char* d, size_t n, const char* name, char* buf, size_t cap) {
    size_t i = 0, ln = strlen(name);

    while (i < n) {
        size_t ks, ke, vs, ve;

        while (i < n && (ws(d[i]) || d[i] == ';')) {
            i++;
        }

        ks = i;

        while (i < n && d[i] != ':' && d[i] != ';') {
            i++;
        }

        ke = i;

        while (ke > ks && ws(d[ke - 1])) {
            ke--;
        }

        if (i >= n || d[i] != ':') {
            continue;
        }

        vs = ++i;

        while (i < n && d[i] != ';') {
            i++;
        }

        ve = i;

        while (vs < ve && ws(d[vs])) {
            vs++;
        }

        while (ve > vs && ws(d[ve - 1])) {
            ve--;
        }

        if (ve - vs > 10 && !memcmp(d + ve - 10, "!important", 10)) {
            ve -= 10;

            while (ve > vs && ws(d[ve - 1])) {
                ve--;
            }
        }

        if (ke - ks == ln && !memcmp(d + ks, name, ln)) {
            size_t len = ve - vs < cap - 1 ? ve - vs : cap - 1;

            memcpy(buf, d + vs, len);
            buf[len] = 0;
            return 1;
        }
    }

    return 0;
}

static int has_class(const snode* x, const char* cls) {
    size_t n = 0, i = 0, ln = strlen(cls);
    const char* v = attr(x, "class", &n);

    while (v && i < n) {
        size_t s0;

        while (i < n && ws(v[i])) {
            i++;
        }

        s0 = i;

        while (i < n && !ws(v[i])) {
            i++;
        }

        if (i - s0 == ln && !memcmp(v + s0, cls, ln)) {
            return 1;
        }
    }

    return 0;
}

static int rule_matches(const srule* r, const snode* x) {
    int k;

    if (r->tag[0] && strcmp(r->tag, x->tag) != 0) {
        return 0;
    }

    if (r->id[0]) {
        char id[64];

        if (!attr_s(x, "id", id, sizeof(id)) || strcmp(id, r->id) != 0) {
            return 0;
        }
    }

    for (k = 0; k < r->ncls; k++) {
        if (!has_class(x, r->cls[k])) {
            return 0;
        }
    }

    return 1;
}

/* a property of an element as it sets it: its style attribute, then the style sheets (most specific, then last),
   then its attribute of that name */
static int prop(const sv_t* S, const snode* x, const char* name, char* buf, size_t cap) {
    size_t n = 0;
    const char* st = attr(x, "style", &n);
    int k, best = -1;

    if (st && decl_get(st, n, name, buf, cap)) {
        return 1;
    }

    for (k = 0; k < S->nru; k++) {
        const srule* r = &S->ru[k];

        if ((best < 0 || r->spec > S->ru[best].spec || (r->spec == S->ru[best].spec && r->order > S->ru[best].order)) &&
                rule_matches(r, x)) {
            char tmp[8];

            if (decl_get(r->d, r->dn, name, tmp, sizeof(tmp))) {
                best = k;
            }
        }
    }

    if (best >= 0 && decl_get(S->ru[best].d, S->ru[best].dn, name, buf, cap)) {
        return 1;
    }

    return attr_s(x, name, buf, cap);
}

/* ------------------------------------------------------------------ */
/* numbers, lengths, colours                                          */
/* ------------------------------------------------------------------ */

/* a number at s (after spaces and a comma), its end in *end; 0 if none */
static int num(const char* s, const char** end, double* v) {
    const char* p = s;
    double r = 0, f = 0.1;
    int neg = 0, digits = 0;

    while (ws(*p) || *p == ',') {
        p++;
    }

    if (*p == '-' || *p == '+') {
        neg = *p++ == '-';
    }

    while (*p >= '0' && *p <= '9') {
        r = r * 10 + (*p++ - '0');
        digits++;
    }

    if (*p == '.') {
        for (p++; *p >= '0' && *p <= '9'; p++, f /= 10) {
            r += (*p - '0') * f;
            digits++;
        }
    }

    if (!digits) {
        return 0;
    }

    if ((*p == 'e' || *p == 'E') && (p[1] == '-' || p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
        int en = 0, eneg = 0;

        p++;

        if (*p == '-' || *p == '+') {
            eneg = *p++ == '-';
        }

        while (*p >= '0' && *p <= '9') {
            en = en * 10 + (*p++ - '0');

            if (en > 300) {
                en = 300;
            }
        }

        r *= pow(10, eneg ? -en : en);
    }

    *v = neg ? -r : r;
    *end = p;
    return 1;
}

/* a length in user units: px and no unit 1, pt, pc, mm, cm, in, em (of the font size), % of ref */
static double length(const char* s, double ref, double em, double def) {
    const char* e;
    double v;

    if (!s || !num(s, &e, &v)) {
        return def;
    }

    if (!strncmp(e, "pt", 2)) {
        v *= 96.0 / 72;
    } else if (!strncmp(e, "pc", 2)) {
        v *= 16;
    } else if (!strncmp(e, "mm", 2)) {
        v *= 96 / 25.4;
    } else if (!strncmp(e, "cm", 2)) {
        v *= 96 / 2.54;
    } else if (!strncmp(e, "in", 2)) {
        v *= 96;
    } else if (!strncmp(e, "em", 2)) {
        v *= em;
    } else if (!strncmp(e, "ex", 2)) {
        v *= em / 2;
    } else if (*e == '%') {
        v *= ref / 100;
    }

    return v;
}

static double attr_len(const snode* x, const char* name, double ref, double em, double def) {
    char b[64];

    return attr_s(x, name, b, sizeof(b)) ? length(b, ref, em, def) : def;
}

static const struct { const char* name; uint32_t rgb; } NAMED[] = {
    { "aliceblue", 0xF0F8FF }, { "antiquewhite", 0xFAEBD7 }, { "aqua", 0x00FFFF }, { "aquamarine", 0x7FFFD4 },
    { "azure", 0xF0FFFF }, { "beige", 0xF5F5DC }, { "bisque", 0xFFE4C4 }, { "black", 0x000000 },
    { "blanchedalmond", 0xFFEBCD }, { "blue", 0x0000FF }, { "blueviolet", 0x8A2BE2 }, { "brown", 0xA52A2A },
    { "burlywood", 0xDEB887 }, { "cadetblue", 0x5F9EA0 }, { "chartreuse", 0x7FFF00 }, { "chocolate", 0xD2691E },
    { "coral", 0xFF7F50 }, { "cornflowerblue", 0x6495ED }, { "cornsilk", 0xFFF8DC }, { "crimson", 0xDC143C },
    { "cyan", 0x00FFFF }, { "darkblue", 0x00008B }, { "darkcyan", 0x008B8B }, { "darkgoldenrod", 0xB8860B },
    { "darkgray", 0xA9A9A9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xA9A9A9 }, { "darkkhaki", 0xBDB76B },
    { "darkmagenta", 0x8B008B }, { "darkolivegreen", 0x556B2F }, { "darkorange", 0xFF8C00 },
    { "darkorchid", 0x9932CC }, { "darkred", 0x8B0000 }, { "darksalmon", 0xE9967A }, { "darkseagreen", 0x8FBC8F },
    { "darkslateblue", 0x483D8B }, { "darkslategray", 0x2F4F4F }, { "darkslategrey", 0x2F4F4F },
    { "darkturquoise", 0x00CED1 }, { "darkviolet", 0x9400D3 }, { "deeppink", 0xFF1493 }, { "deepskyblue", 0x00BFFF },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "dodgerblue", 0x1E90FF }, { "firebrick", 0xB22222 },
    { "floralwhite", 0xFFFAF0 }, { "forestgreen", 0x228B22 }, { "fuchsia", 0xFF00FF }, { "gainsboro", 0xDCDCDC },
    { "ghostwhite", 0xF8F8FF }, { "gold", 0xFFD700 }, { "goldenrod", 0xDAA520 }, { "gray", 0x808080 },
    { "grey", 0x808080 }, { "green", 0x008000 }, { "greenyellow", 0xADFF2F }, { "honeydew", 0xF0FFF0 },
    { "hotpink", 0xFF69B4 }, { "indianred", 0xCD5C5C }, { "indigo", 0x4B0082 }, { "ivory", 0xFFFFF0 },
    { "khaki", 0xF0E68C }, { "lavender", 0xE6E6FA }, { "lavenderblush", 0xFFF0F5 }, { "lawngreen", 0x7CFC00 },
    { "lemonchiffon", 0xFFFACD }, { "lightblue", 0xADD8E6 }, { "lightcoral", 0xF08080 }, { "lightcyan", 0xE0FFFF },
    { "lightgoldenrodyellow", 0xFAFAD2 }, { "lightgray", 0xD3D3D3 }, { "lightgreen", 0x90EE90 },
    { "lightgrey", 0xD3D3D3 }, { "lightpink", 0xFFB6C1 }, { "lightsalmon", 0xFFA07A }, { "lightseagreen", 0x20B2AA },
    { "lightskyblue", 0x87CEFA }, { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 },
    { "lightsteelblue", 0xB0C4DE }, { "lightyellow", 0xFFFFE0 }, { "lime", 0x00FF00 }, { "limegreen", 0x32CD32 },
    { "linen", 0xFAF0E6 }, { "magenta", 0xFF00FF }, { "maroon", 0x800000 }, { "mediumaquamarine", 0x66CDAA },
    { "mediumblue", 0x0000CD }, { "mediumorchid", 0xBA55D3 }, { "mediumpurple", 0x9370DB },
    { "mediumseagreen", 0x3CB371 }, { "mediumslateblue", 0x7B68EE }, { "mediumspringgreen", 0x00FA9A },
    { "mediumturquoise", 0x48D1CC }, { "mediumvioletred", 0xC71585 }, { "midnightblue", 0x191970 },
    { "mintcream", 0xF5FFFA }, { "mistyrose", 0xFFE4E1 }, { "moccasin", 0xFFE4B5 }, { "navajowhite", 0xFFDEAD },
    { "navy", 0x000080 }, { "oldlace", 0xFDF5E6 }, { "olive", 0x808000 }, { "olivedrab", 0x6B8E23 },
    { "orange", 0xFFA500 }, { "orangered", 0xFF4500 }, { "orchid", 0xDA70D6 }, { "palegoldenrod", 0xEEE8AA },
    { "palegreen", 0x98FB98 }, { "paleturquoise", 0xAFEEEE }, { "palevioletred", 0xDB7093 },
    { "papayawhip", 0xFFEFD5 }, { "peachpuff", 0xFFDAB9 }, { "peru", 0xCD853F }, { "pink", 0xFFC0CB },
    { "plum", 0xDDA0DD }, { "powderblue", 0xB0E0E6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 },
    { "red", 0xFF0000 }, { "rosybrown", 0xBC8F8F }, { "royalblue", 0x4169E1 }, { "saddlebrown", 0x8B4513 },
    { "salmon", 0xFA8072 }, { "sandybrown", 0xF4A460 }, { "seagreen", 0x2E8B57 }, { "seashell", 0xFFF5EE },
    { "sienna", 0xA0522D }, { "silver", 0xC0C0C0 }, { "skyblue", 0x87CEEB }, { "slateblue", 0x6A5ACD },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xFFFAFA }, { "springgreen", 0x00FF7F },
    { "steelblue", 0x4682B4 }, { "tan", 0xD2B48C }, { "teal", 0x008080 }, { "thistle", 0xD8BFD8 },
    { "tomato", 0xFF6347 }, { "turquoise", 0x40E0D0 }, { "violet", 0xEE82EE }, { "wheat", 0xF5DEB3 },
    { "white", 0xFFFFFF }, { "whitesmoke", 0xF5F5F5 }, { "yellow", 0xFFFF00 }, { "yellowgreen", 0x9ACD32 }
};

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* a colour as 0xAARRGGBB; 0 when none or unreadable; cur: currentColor's */
static uint32_t color(const char* s, uint32_t cur) {
    size_t n, k;
    char low[32];

    while (ws(*s)) {
        s++;
    }

    n = strlen(s);

    while (n > 0 && ws(s[n - 1])) {
        n--;
    }

    if (n == 0 || (n == 4 && !strncmp(s, "none", 4)) || (n == 11 && !strncmp(s, "transparent", 11))) {
        return 0;
    }

    if (n == 12 && !strncmp(s, "currentColor", 12)) {
        return cur;
    }

    if (s[0] == '#') {
        int v[8], i;

        for (i = 1; i < (int)n && i <= 8; i++) {
            if ((v[i - 1] = hexv(s[i])) < 0) {
                return 0;
            }
        }

        if (n == 4 || n == 5) {     /* #rgb, #rgba */
            uint32_t a = n == 5 ? (uint32_t)(v[3] * 17) : 255;

            return a << 24 | (uint32_t)(v[0] * 17) << 16 | (uint32_t)(v[1] * 17) << 8 | (uint32_t)(v[2] * 17);
        }

        if (n == 7 || n == 9) {
            uint32_t a = n == 9 ? (uint32_t)(v[6] * 16 + v[7]) : 255;

            return a << 24 | (uint32_t)(v[0] * 16 + v[1]) << 16 | (uint32_t)(v[2] * 16 + v[3]) << 8 |
                   (uint32_t)(v[4] * 16 + v[5]);
        }

        return 0;
    }

    if (!strncmp(s, "rgb", 3)) {    /* rgb(r, g, b), rgba(r, g, b, a), percentages or not */
        const char* p = strchr(s, '('), *e;
        double c[4] = { 0, 0, 0, 1 };
        int i;

        if (!p) {
            return 0;
        }

        p++;

        for (i = 0; i < 4 && num(p, &e, &c[i]); i++) {
            if (*e == '%') {
                c[i] = i < 3 ? c[i] * 2.55 : c[i] / 100;
                e++;
            }

            while (ws(*e) || *e == ',' || *e == '/') {
                e++;
            }

            p = e;
        }

        if (i < 3) {
            return 0;
        }

        for (i = 0; i < 3; i++) {
            c[i] = c[i] < 0 ? 0 : c[i] > 255 ? 255 : c[i];
        }

        c[3] = c[3] < 0 ? 0 : c[3] > 1 ? 1 : c[3];
        return (uint32_t)lround(c[3] * 255) << 24 | (uint32_t)lround(c[0]) << 16 | (uint32_t)lround(c[1]) << 8 |
               (uint32_t)lround(c[2]);
    }

    for (k = 0; k < n && k + 1 < sizeof(low); k++) {
        low[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]);
    }

    low[k] = 0;

    for (k = 0; k < sizeof(NAMED) / sizeof(NAMED[0]); k++) {
        if (!strcmp(low, NAMED[k].name)) {
            return 0xFF000000u | NAMED[k].rgb;
        }
    }

    return 0;
}

static uint32_t with_alpha(uint32_t c, double op) {
    double a = ((c >> 24) & 255) * (op != op ? 1 : op < 0 ? 0 : op > 1 ? 1 : op);

    return c ? ((uint32_t)lround(a) << 24) | (c & 0xFFFFFFu) : 0;
}

/* ------------------------------------------------------------------ */
/* transforms                                                         */
/* ------------------------------------------------------------------ */

static void mul(double* m, const double* t) {     /* m = m * t (t applied first) */
    double r[6];

    r[0] = m[0] * t[0] + m[2] * t[1];
    r[1] = m[1] * t[0] + m[3] * t[1];
    r[2] = m[0] * t[2] + m[2] * t[3];
    r[3] = m[1] * t[2] + m[3] * t[3];
    r[4] = m[0] * t[4] + m[2] * t[5] + m[4];
    r[5] = m[1] * t[4] + m[3] * t[5] + m[5];
    memcpy(m, r, sizeof(r));
}

static void apply(const double* m, double x, double y, double* ox, double* oy) {
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}

static double scale_of(const double* m) {
    return sqrt(fabs(m[0] * m[3] - m[1] * m[2]));
}

/* a transform attribute ("translate(10 20) rotate(45)") onto m */
static void transform(double* m, const char* s) {
    while (s && *s) {
        const char* p = s, *e;
        double v[6], t[6] = { 1, 0, 0, 1, 0, 0 };
        int nv = 0;
        char name[16];
        size_t k = 0;

        while (ws(*p) || *p == ',') {
            p++;
        }

        while (*p && *p != '(' && k + 1 < sizeof(name)) {
            if (!ws(*p)) {
                name[k++] = *p;
            }

            p++;
        }

        name[k] = 0;

        if (*p != '(') {
            return;
        }

        p++;

        while (nv < 6 && num(p, &e, &v[nv])) {
            nv++;
            p = e;
        }

        while (*p && *p != ')') {
            p++;
        }

        s = *p ? p + 1 : p;

        if (!strcmp(name, "matrix") && nv == 6) {
            memcpy(t, v, sizeof(t));
        } else if (!strcmp(name, "translate") && nv >= 1) {
            t[4] = v[0];
            t[5] = nv > 1 ? v[1] : 0;
        } else if (!strcmp(name, "scale") && nv >= 1) {
            t[0] = v[0];
            t[3] = nv > 1 ? v[1] : v[0];
        } else if (!strcmp(name, "rotate") && nv >= 1) {
            double a = v[0] * SV_PI / 180, c = cos(a), sn = sin(a);
            double cx = nv >= 3 ? v[1] : 0, cy = nv >= 3 ? v[2] : 0;

            t[0] = c;
            t[1] = sn;
            t[2] = -sn;
            t[3] = c;
            t[4] = cx - c * cx + sn * cy;
            t[5] = cy - sn * cx - c * cy;
        } else if (!strcmp(name, "skewX") && nv >= 1) {
            t[2] = tan(v[0] * SV_PI / 180);
        } else if (!strcmp(name, "skewY") && nv >= 1) {
            t[1] = tan(v[0] * SV_PI / 180);
        } else {
            continue;
        }

        mul(m, t);
    }
}

/* a viewBox shown in a viewport (x, y, w, h) as preserveAspectRatio says, onto m; 0 if there is no viewBox */
static int viewbox(double* m, const snode* x, double vx, double vy, double vw, double vh, double* bw, double* bh) {
    char b[128], par[64];
    const char* p = b, *e;
    double v[4];
    int k;

    if (!attr_s(x, "viewBox", b, sizeof(b))) {
        return 0;
    }

    for (k = 0; k < 4 && num(p, &e, &v[k]); k++) {
        p = e;
    }

    if (k < 4 || v[2] <= 0 || v[3] <= 0) {
        return 0;
    }

    {
        double sx = vw / v[2], sy = vh / v[3], tx, ty, t[6];
        int none = 0, slice = 0;
        char ax = 'd', ay = 'd';     /* mid */

        if (attr_s(x, "preserveAspectRatio", par, sizeof(par))) {
            none = strstr(par, "none") != NULL;
            slice = strstr(par, "slice") != NULL;
            ax = strstr(par, "xMin") ? 'n' : strstr(par, "xMax") ? 'x' : 'd';
            ay = strstr(par, "YMin") ? 'n' : strstr(par, "YMax") ? 'x' : 'd';
        }

        if (!none) {
            sx = sy = slice ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
        }

        tx = vx - v[0] * sx + (ax == 'n' ? 0 : ax == 'x' ? vw - v[2] * sx : (vw - v[2] * sx) / 2);
        ty = vy - v[1] * sy + (ay == 'n' ? 0 : ay == 'x' ? vh - v[3] * sy : (vh - v[3] * sy) / 2);
        t[0] = sx;
        t[1] = 0;
        t[2] = 0;
        t[3] = sy;
        t[4] = tx;
        t[5] = ty;
        mul(m, t);
    }

    if (bw) {
        *bw = v[2];
        *bh = v[3];
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* outlines                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    double* p;                  /* x, y in the drawing's units */
    int n, cap;
    int* ring;                  /* where each ring starts */
    int* closed;
    int nr, capr;
    double tol;                 /* flattening tolerance, drawing units */
    const double* m;
} outline;

static int ol_pt(outline* O, double ux, double uy) {
    if (O->n >= O->cap) {
        int cap = O->cap ? O->cap * 2 : 64;
        double* g = (double*)realloc(O->p, (size_t)cap * 2 * sizeof(double));

        if (!g) {
            return -1;
        }

        O->p = g;
        O->cap = cap;
    }

    apply(O->m, ux, uy, &O->p[2 * O->n], &O->p[2 * O->n + 1]);
    O->n++;
    return 0;
}

static int ol_ring(outline* O) {
    if (O->nr >= O->capr) {
        int cap = O->capr ? O->capr * 2 : 8;
        int* a = (int*)realloc(O->ring, (size_t)cap * sizeof(int)), *b;

        if (!a) {
            return -1;
        }

        O->ring = a;
        b = (int*)realloc(O->closed, (size_t)cap * sizeof(int));

        if (!b) {
            return -1;
        }

        O->closed = b;
        O->capr = cap;
    }

    if (O->nr > 0 && O->ring[O->nr - 1] == O->n) {
        O->closed[O->nr - 1] = 0;   /* an empty ring before: this one in its place */
        return 0;
    }

    O->ring[O->nr] = O->n;
    O->closed[O->nr] = 0;
    O->nr++;
    return 0;
}

static void ol_close(outline* O) {
    if (O->nr > 0) {
        O->closed[O->nr - 1] = 1;
    }
}

static void ol_free(outline* O) {
    free(O->p);
    free(O->ring);
    free(O->closed);
}

/* segments a curve is drawn with: enough that no chord strays past the tolerance */
static int segs(const outline* O, double len_user) {
    double l = len_user * scale_of(O->m), k = sqrt(l / (O->tol > 1 ? O->tol : 1)) * 1.5;

    return k < 2 ? 2 : k > 96 ? 96 : (int)k;
}

static void ol_cubic(outline* O, double x0, double y0, double x1, double y1, double x2, double y2, double x3,
                     double y3) {
    double l = hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x3 - x2, y3 - y2);
    int n = segs(O, l), i;

    for (i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;

        ol_pt(O, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
              u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
}

static void ol_quad(outline* O, double x0, double y0, double x1, double y1, double x2, double y2) {
    double l = hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1);
    int n = segs(O, l), i;

    for (i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;

        ol_pt(O, u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2);
    }
}

/* an elliptical arc from (x0, y0) to (x, y), as SVG gives it: radii, the ellipse's turn, which of the four */
static void ol_arc(outline* O, double x0, double y0, double rx, double ry, double phi, int large, int sweep, double x,
                   double y) {
    double c = cos(phi * SV_PI / 180), s = sin(phi * SV_PI / 180);
    double dx = (x0 - x) / 2, dy = (y0 - y) / 2, x1 = c * dx + s * dy, y1 = -s * dx + c * dy;
    double lam, sq, cx1, cy1, cx, cy, t1, dt;
    int n, i;

    rx = fabs(rx);
    ry = fabs(ry);

    if (rx < 1e-12 || ry < 1e-12 || (x0 == x && y0 == y)) {
        ol_pt(O, x, y);
        return;
    }

    lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);

    if (lam > 1) {
        rx *= sqrt(lam);
        ry *= sqrt(lam);
    }

    sq = (rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1) / (rx * rx * y1 * y1 + ry * ry * x1 * x1);
    sq = sq < 0 ? 0 : sqrt(sq);

    if (large == sweep) {
        sq = -sq;
    }

    cx1 = sq * rx * y1 / ry;
    cy1 = -sq * ry * x1 / rx;
    cx = c * cx1 - s * cy1 + (x0 + x) / 2;
    cy = s * cx1 + c * cy1 + (y0 + y) / 2;
    t1 = atan2((y1 - cy1) / ry, (x1 - cx1) / rx);
    dt = atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx) - t1;

    if (sweep && dt < 0) {
        dt += 2 * SV_PI;
    } else if (!sweep && dt > 0) {
        dt -= 2 * SV_PI;
    }

    n = segs(O, fabs(dt) * (rx > ry ? rx : ry));
    n = n < 4 ? 4 : n;

    for (i = 1; i <= n; i++) {
        double t = t1 + dt * i / n, ex = rx * cos(t), ey = ry * sin(t);

        ol_pt(O, i == n ? x : c * ex - s * ey + cx, i == n ? y : s * ex + c * ey + cy);
    }
}

/* path data into the outline */
static void path_data(outline* O, const char* d) {
    const char* p = d, *e;
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0;
    char cmd = 0, last = 0;

    while (p && *p) {
        double v[7];
        int need, k, rel;

        while (ws(*p) || *p == ',') {
            p++;
        }

        if (!*p) {
            break;
        }

        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            cmd = *p++;

            if (cmd == 'Z' || cmd == 'z') {
                ol_close(O);
                cx = sx;
                cy = sy;
                last = 'Z';
                continue;
            }
        } else if (!cmd) {
            return;
        }

        rel = cmd >= 'a';

        switch (cmd | 32) {
            case 'm': case 'l': case 't': need = 2; break;
            case 'h': case 'v': need = 1; break;
            case 'c': need = 6; break;
            case 's': case 'q': need = 4; break;
            case 'a': need = 7; break;
            default: return;
        }

        for (k = 0; k < need; k++) {
            if ((cmd | 32) == 'a' && (k == 3 || k == 4)) {  /* flags: one digit, with or without a separator */
                while (ws(*p) || *p == ',') {
                    p++;
                }

                if (*p != '0' && *p != '1') {
                    return;
                }

                v[k] = *p++ - '0';
                continue;
            }

            if (!num(p, &e, &v[k])) {
                return;
            }

            p = e;
        }

        switch (cmd | 32) {
            case 'm':
                cx = rel ? cx + v[0] : v[0];
                cy = rel ? cy + v[1] : v[1];
                sx = cx;
                sy = cy;
                ol_ring(O);
                ol_pt(O, cx, cy);
                cmd = rel ? 'l' : 'L';  /* more pairs after it: lines */
                break;

            case 'l':
                if (last == 'Z') {
                    ol_ring(O);
                    ol_pt(O, cx, cy);
                }

                cx = rel ? cx + v[0] : v[0];
                cy = rel ? cy + v[1] : v[1];
                ol_pt(O, cx, cy);
                break;

            case 'h':
                if (last == 'Z') {
                    ol_ring(O);
                    ol_pt(O, cx, cy);
                }

                cx = rel ? cx + v[0] : v[0];
                ol_pt(O, cx, cy);
                break;

            case 'v':
                if (last == 'Z') {
                    ol_ring(O);
                    ol_pt(O, cx, cy);
                }

                cy = rel ? cy + v[0] : v[0];
                ol_pt(O, cx, cy);
                break;

            case 'c':
            case 's': {
                double x1, y1, x2, y2, x, y;
                int i0 = (cmd | 32) == 's' ? 0 : 2;

                if (last == 'Z') {
                    ol_ring(O);
                    ol_pt(O, cx, cy);
                }

                if ((cmd | 32) == 'c') {
                    x1 = rel ? cx + v[0] : v[0];
                    y1 = rel ? cy + v[1] : v[1];
                } else if (last == 'c' || last == 's') {
                    x1 = 2 * cx - lcx;
                    y1 = 2 * cy - lcy;
                } else {
                    x1 = cx;
                    y1 = cy;
                }

                x2 = rel ? cx + v[i0] : v[i0];
                y2 = rel ? cy + v[i0 + 1] : v[i0 + 1];
                x = rel ? cx + v[i0 + 2] : v[i0 + 2];
                y = rel ? cy + v[i0 + 3] : v[i0 + 3];
                ol_cubic(O, cx, cy, x1, y1, x2, y2, x, y);
                lcx = x2;
                lcy = y2;
                cx = x;
                cy = y;
                break;
            }

            case 'q':
            case 't': {
                double x1, y1, x, y;

                if (last == 'Z') {
                    ol_ring(O);
                    ol_pt(O, cx, cy);
                }

                if ((cmd | 32) == 'q') {
                    x1 = rel ? cx + v[0] : v[0];
                    y1 = rel ? cy + v[1] : v[1];
                    x = rel ? cx + v[2] : v[2];
                    y = rel ? cy + v[3] : v[3];
                } else {
                    x1 = last == 'q' || last == 't' ? 2 * cx - lcx : cx;
                    y1 = last == 'q' || last == 't' ? 2 * cy - lcy : cy;
                    x = rel ? cx + v[0] : v[0];
                    y = rel ? cy + v[1] : v[1];
                }

                ol_quad(O, cx, cy, x1, y1, x, y);
                lcx = x1;
                lcy = y1;
                cx = x;
                cy = y;
                break;
            }

            case 'a': {
                double x = rel ? cx + v[5] : v[5], y = rel ? cy + v[6] : v[6];

                if (last == 'Z') {
                    ol_ring(O);
                    ol_pt(O, cx, cy);
                }

                ol_arc(O, cx, cy, v[0], v[1], v[2], (int)v[3], (int)v[4], x, y);
                cx = x;
                cy = y;
                break;
            }
        }

        last = (char)(cmd | 32);
    }
}

static double ring_area(const outline* O, int r) {
    int a = O->ring[r], b = r + 1 < O->nr ? O->ring[r + 1] : O->n, i;
    double s = 0;

    for (i = a; i < b; i++) {
        int j = i + 1 < b ? i + 1 : a;

        s += O->p[2 * i] * O->p[2 * j + 1] - O->p[2 * j] * O->p[2 * i + 1];
    }

    return s / 2;
}

static int inside(const outline* O, int r, double x, double y) {
    int a = O->ring[r], b = r + 1 < O->nr ? O->ring[r + 1] : O->n, i, j, in = 0;

    for (i = a, j = b - 1; i < b; j = i++) {
        double xi = O->p[2 * i], yi = O->p[2 * i + 1], xj = O->p[2 * j], yj = O->p[2 * j + 1];

        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
            in = !in;
        }
    }

    return in;
}

/* even-odd filling as non-zero gives it: each ring turned the way its depth (the rings around it) says */
static void evenodd_rings(outline* O) {
    int r, q;

    if (O->nr < 2 || O->nr > 2000) {
        return;
    }

    for (r = 0; r < O->nr; r++) {
        int a = O->ring[r], b = r + 1 < O->nr ? O->ring[r + 1] : O->n, depth = 0;
        double area = ring_area(O, r);

        if (b - a < 3) {
            continue;
        }

        for (q = 0; q < O->nr; q++) {
            int qa = O->ring[q], qb = q + 1 < O->nr ? O->ring[q + 1] : O->n;

            if (q != r && qb - qa >= 3 && inside(O, q, O->p[2 * a], O->p[2 * a + 1])) {
                depth++;
            }
        }

        if ((area > 0) != (depth % 2 == 0)) {   /* reversed */
            int i, j;

            for (i = a, j = b - 1; i < j; i++, j--) {
                double tx = O->p[2 * i], ty = O->p[2 * i + 1];

                O->p[2 * i] = O->p[2 * j];
                O->p[2 * i + 1] = O->p[2 * j + 1];
                O->p[2 * j] = tx;
                O->p[2 * j + 1] = ty;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* output                                                             */
/* ------------------------------------------------------------------ */

static void sep(sv_t* S) {
    if (!S->first) {
        pb_putc(S->o, ',');
    }

    S->first = 0;
}

static int find_id(const sv_t* S, const char* id) {
    int k;
    size_t n = strlen(id);

    for (k = 0; k < S->nn; k++) {
        size_t vn = 0;
        const char* v = S->nd[k].tag[0] ? attr(&S->nd[k], "id", &vn) : NULL;

        if (v && vn == n && !memcmp(v, id, n)) {
            return k;
        }
    }

    return -1;
}

/* the element a "url(#id)" or "#id" names; -1 if none */
static int url_ref(const sv_t* S, const char* s) {
    char id[96];
    const char* h = strchr(s, '#'), *e;
    size_t k = 0;

    if (!h) {
        return -1;
    }

    for (e = h + 1; *e && *e != ')' && *e != '\'' && *e != '"' && !ws(*e) && k + 1 < sizeof(id); e++) {
        id[k++] = *e;
    }

    id[k] = 0;
    return k ? find_id(S, id) : -1;
}

static int href(const sv_t* S, const snode* x) {
    char b[160];

    if (attr_s(x, "href", b, sizeof(b)) || attr_s(x, "xlink:href", b, sizeof(b))) {
        return url_ref(S, b);
    }

    return -1;
}

/* a gradient's stops (following its href to the one it takes them from), as position (0..1) and colour */
static int stops(const sv_t* S, int g, double* pos, uint32_t* col, int cap) {
    int hops, k, n = 0;

    for (hops = 0; g >= 0 && hops < 8; hops++) {
        for (k = S->nd[g].kid; k >= 0 && n < cap; k = S->nd[k].next) {
            char b[64], c[64];
            double op = 1, o;

            if (strcmp(S->nd[k].tag, "stop") != 0) {
                continue;
            }

            o = attr_s(&S->nd[k], "offset", b, sizeof(b)) ? length(b, 1, 0, 0) : 0;

            if (!prop(S, &S->nd[k], "stop-color", c, sizeof(c))) {
                strcpy(c, "black");
            }

            if (prop(S, &S->nd[k], "stop-opacity", b, sizeof(b))) {
                op = atof(b);
            }

            o = o < 0 ? 0 : o > 1 ? 1 : o;
            pos[n] = n > 0 && o < pos[n - 1] ? pos[n - 1] : o;
            col[n] = with_alpha(color(c, 0xFF000000u), op);    /* (none: clear) */
            n++;
        }

        if (n > 0) {
            break;
        }

        g = href(S, &S->nd[g]);
    }

    return n;
}

/* a gradient attribute of g or the one it takes after (gradients pass their attributes on by href) */
static int grad_attr(const sv_t* S, int g, const char* name, char* buf, size_t cap) {
    int hops;

    for (hops = 0; g >= 0 && hops < 8; hops++) {
        if (attr_s(&S->nd[g], name, buf, cap)) {
            return 1;
        }

        g = href(S, &S->nd[g]);
    }

    return 0;
}

/* a gradient fill ("gk" and the rest) for a shape whose outline spans bbox (drawing units); 0 if none written */
static int put_gradient(sv_t* S, int g, const sstate* st, const double* bb, double op) {
    double pos[16], q[16];
    uint32_t col[16];
    int n = stops(S, g, pos, col, 16), k, user, radial = !strcmp(S->nd[g].tag, "radialGradient");
    char b[160];
    double gt[6] = { 1, 0, 0, 1, 0, 0 }, m[6];
    double bw = bb[2] - bb[0], bh = bb[3] - bb[1];

    if (n == 0) {
        return 0;
    }

    for (k = 0; k < n; k++) {
        col[k] = with_alpha(col[k], op);
    }

    if (n == 1) {
        pb_printf(S->o, ",\"fill\":%u", (unsigned)col[0]);
        return 1;
    }

    user = grad_attr(S, g, "gradientUnits", b, sizeof(b)) && !strcmp(b, "userSpaceOnUse");

    if (grad_attr(S, g, "gradientTransform", b, sizeof(b))) {
        transform(gt, b);
    }

    /* the gradient's own space to the drawing's: the user space, or the shape's box */
    if (user) {
        memcpy(m, st->m, sizeof(m));
    } else {
        m[0] = bw;
        m[1] = 0;
        m[2] = 0;
        m[3] = bh;
        m[4] = bb[0];
        m[5] = bb[1];
    }

    mul(m, gt);

    if (!radial) {
        double x1 = 0, y1 = 0, x2 = 1, y2 = 0, ax, ay, bx, by, ux, uy, l, lo = 1e300, hi = -1e300, t0, t1, ang;
        double ref = user ? S->vw : 1, refh = user ? S->vh : 1;

        x1 = grad_attr(S, g, "x1", b, sizeof(b)) ? length(b, ref, 16, 0) : 0;
        y1 = grad_attr(S, g, "y1", b, sizeof(b)) ? length(b, refh, 16, 0) : 0;
        x2 = grad_attr(S, g, "x2", b, sizeof(b)) ? length(b, ref, 16, 1) : (user ? S->vw : 1);
        y2 = grad_attr(S, g, "y2", b, sizeof(b)) ? length(b, refh, 16, 0) : 0;

        apply(m, x1, y1, &ax, &ay);
        apply(m, x2, y2, &bx, &by);
        ux = bx - ax;
        uy = by - ay;
        l = hypot(ux, uy);

        if (l < 1e-9) {
            pb_printf(S->o, ",\"fill\":%u", (unsigned)col[n - 1]);
            return 1;
        }

        ux /= l;
        uy /= l;

        for (k = 0; k < 4; k++) {   /* the box's extent along the way it runs: where the layout puts 0 and 1 */
            double t = ((k & 1 ? bb[2] : bb[0]) - (bb[0] + bb[2]) / 2) * ux + ((k & 2 ? bb[3] : bb[1]) - (bb[1] + bb[3]) / 2) * uy;

            lo = t < lo ? t : lo;
            hi = t > hi ? t : hi;
        }

        t0 = (ax - (bb[0] + bb[2]) / 2) * ux + (ay - (bb[1] + bb[3]) / 2) * uy;
        t1 = t0 + l;

        for (k = 0; k < n; k++) {
            q[k] = hi > lo ? (t0 + pos[k] * (t1 - t0) - lo) / (hi - lo) : pos[k];
        }

        ang = atan2(uy, ux) * 180 / SV_PI;
        ang = ang < 0 ? ang + 360 : ang;
        pb_printf(S->o, ",\"fill\":%u,\"gk\":1,\"f2\":%u,\"ga\":%d", (unsigned)col[0], (unsigned)col[n - 1],
                  (int)(far(ang * 60000) % 21600000));
    } else {
        double cx, cy, r, ocx, ocy, R = 0, rr, ex, ey;
        double refw = user ? S->vw : 1, refh = user ? S->vh : 1, refd = user ? hypot(S->vw, S->vh) / sqrt(2) : 1;

        cx = grad_attr(S, g, "cx", b, sizeof(b)) ? length(b, refw, 16, 0.5) : (user ? S->vw / 2 : 0.5);
        cy = grad_attr(S, g, "cy", b, sizeof(b)) ? length(b, refh, 16, 0.5) : (user ? S->vh / 2 : 0.5);
        r = grad_attr(S, g, "r", b, sizeof(b)) ? length(b, refd, 16, 0.5) : (user ? refd / 2 : 0.5);

        apply(m, cx, cy, &ocx, &ocy);
        apply(m, cx + r, cy, &ex, &ey);
        rr = hypot(ex - ocx, ey - ocy);

        for (k = 0; k < 4; k++) {   /* the layout's circle: from the centre to the box's farthest corner */
            double dx = (k & 1 ? bb[2] : bb[0]) - ocx, dy = (k & 2 ? bb[3] : bb[1]) - ocy;

            R = hypot(dx, dy) > R ? hypot(dx, dy) : R;
        }

        for (k = 0; k < n; k++) {
            q[k] = R > 0 ? pos[k] * rr / R : pos[k];
        }

        {
            double fl = bw > 0 ? (ocx - bb[0]) / bw : 0.5, ft = bh > 0 ? (ocy - bb[1]) / bh : 0.5;

            fl = fl < 0 ? 0 : fl > 1 ? 1 : fl;
            ft = ft < 0 ? 0 : ft > 1 ? 1 : ft;
            pb_printf(S->o, ",\"fill\":%u,\"gk\":2,\"gp\":1,\"f2\":%u,\"gft\":[%d,%d,%d,%d]", (unsigned)col[0],
                      (unsigned)col[n - 1], (int)far(fl * 100000), (int)far(ft * 100000),
                      (int)far((1 - fl) * 100000), (int)far((1 - ft) * 100000));
        }
    }

    pb_puts(S->o, ",\"gs\":[");

    for (k = 0; k < n; k++) {   /* past the last stop the colour holds: a stop at 1 for it */
        double v = q[k] < 0 ? 0 : q[k] > 1 ? 1 : q[k];

        pb_printf(S->o, "%s%d,%u", k ? "," : "", (int)far(v * 100000), (unsigned)col[k]);
    }

    if (q[n - 1] < 1) {
        pb_printf(S->o, ",100000,%u", (unsigned)col[n - 1]);
    }

    pb_putc(S->o, ']');
    return 1;
}

/* a paint ("fill" or "stroke" value): its colour, or the gradient it names (*grad); 0 for none */
static uint32_t paint(const sv_t* S, const sstate* st, const char* v, double op, int* grad) {
    uint32_t cur = color(st->color, 0xFF000000u);

    *grad = -1;

    if (!strncmp(v, "url(", 4)) {
        int g = url_ref(S, v);
        const char* fb = strchr(v, ')');

        if (g >= 0 && (!strcmp(S->nd[g].tag, "linearGradient") || !strcmp(S->nd[g].tag, "radialGradient"))) {
            *grad = g;
            return 0xFF000000u;
        }

        /* a pattern or something missing: the colour given after it, else none */
        return fb && fb[1] ? with_alpha(color(fb + 1, cur), op) : g >= 0 ? with_alpha(0xFF808080u, op) : 0;
    }

    return with_alpha(color(v, cur), op);
}

static void put_points(sv_t* S, const outline* O, int r0, int r1) {
    int r, i, first = 1;

    for (r = r0; r < r1; r++) {
        int a = O->ring[r], b = r + 1 < O->nr ? O->ring[r + 1] : O->n;

        if (b - a < 2) {
            continue;
        }

        if (!first) {
            pb_printf(S->o, ",%d,%d", (int)INT32_MIN, (int)INT32_MIN);
        }

        for (i = a; i < b; i++) {
            double x = O->p[2 * i], y = O->p[2 * i + 1];

            pb_printf(S->o, "%s%ld,%ld", first && i == a ? "" : ",", far(x), far(y));
        }

        first = 0;
    }
}

static void put_stroke_style(sv_t* S, const sstate* st, double lw) {
    if (st->dash[0] && strcmp(st->dash, "none") != 0 && st->sw > 0) {
        const char* p = st->dash, *e;
        double v[8];
        int n = 0, k, sum = 0;

        while (n < 8 && num(p, &e, &v[n])) {
            if (*e == '%') {
                v[n] *= S->vw / 100;
                e++;
            }

            n++;
            p = e;
        }

        if (n % 2 == 1 && n <= 4) {     /* an odd count: said twice */
            for (k = 0; k < n; k++) {
                v[n + k] = v[k];
            }

            n *= 2;
        }

        n &= ~1;

        for (k = 0; k < n; k++) {
            sum += v[k] > 0;
        }

        if (n >= 2 && sum > 0) {
            pb_puts(S->o, ",\"dash\":[");

            for (k = 0; k < n; k++) {
                pb_printf(S->o, "%s%ld", k ? "," : "", far(v[k] / st->sw * 100000));
            }

            pb_putc(S->o, ']');
        }
    }

    (void)lw;

    if (st->cap) {
        pb_printf(S->o, ",\"cap\":%d", st->cap);
    }

    pb_printf(S->o, ",\"join\":%d", st->join);
}

/* a shape's outline drawn: filled, stroked, or both */
static void draw_outline(sv_t* S, outline* O, const sstate* st, int closed_shape) {
    int gfill = -1, gline = -1, r, anyopen = 0, anyclosed = 0;
    double op = st->opacity;
    uint32_t fc = paint(S, st, st->fill, st->fill_op * op, &gfill);
    uint32_t lc = st->sw > 0 ? paint(S, st, st->stroke, st->stroke_op * op, &gline) : 0;
    double lw = st->sw * scale_of(st->m), bb[4] = { 1e300, 1e300, -1e300, -1e300 };
    int i;

    if (O->n < 2 || (!fc && !lc) || st->hidden) {
        return;
    }

    if (S->npts + O->n > SV_MAX_POINTS) {
        return;
    }

    S->npts += O->n;

    if (gline >= 0) {   /* a gradient stroke: its first colour */
        double pos[2];
        uint32_t col[2];

        lc = stops(S, gline, pos, col, 2) > 0 ? with_alpha(col[0], st->stroke_op * op) : 0;
    }

    for (i = 0; i < O->n; i++) {
        bb[0] = O->p[2 * i] < bb[0] ? O->p[2 * i] : bb[0];
        bb[1] = O->p[2 * i + 1] < bb[1] ? O->p[2 * i + 1] : bb[1];
        bb[2] = O->p[2 * i] > bb[2] ? O->p[2 * i] : bb[2];
        bb[3] = O->p[2 * i + 1] > bb[3] ? O->p[2 * i + 1] : bb[3];
    }

    for (r = 0; r < O->nr; r++) {
        if (O->closed[r] || closed_shape) {
            anyclosed = 1;
        } else {
            anyopen = 1;
        }
    }

    if (fc) {
        if (st->evenodd) {
            evenodd_rings(O);
        }

        sep(S);
        pb_puts(S->o, "{\"path\":[");
        put_points(S, O, 0, O->nr);
        pb_printf(S->o, "],\"closed\":1,\"line\":%u,\"lw\":%ld", (unsigned)(anyopen ? 0 : lc),
                  far(lw < 1 ? 1 : lw));

        if (gfill < 0 || !put_gradient(S, gfill, st, bb, st->fill_op * op)) {
            pb_printf(S->o, ",\"fill\":%u", (unsigned)fc);
        }

        if (lc && !anyopen) {
            put_stroke_style(S, st, lw);
        }

        pb_putc(S->o, '}');

        if (!anyopen) {
            return;
        }
    }

    if (!lc) {
        return;
    }

    for (r = 0; r < 2; r++) {   /* the closed rings, then the open ones */
        int k, want = r == 0, n = 0;

        if ((want && !anyclosed) || (!want && !anyopen)) {
            continue;
        }

        sep(S);
        pb_puts(S->o, "{\"path\":[");

        for (k = 0; k < O->nr; k++) {
            int isc = O->closed[k] || closed_shape;

            if (isc == want) {
                if (n++) {
                    pb_printf(S->o, ",%d,%d,", (int)INT32_MIN, (int)INT32_MIN);
                }

                {
                    int a = O->ring[k], b = k + 1 < O->nr ? O->ring[k + 1] : O->n, i2;

                    for (i2 = a; i2 < b; i2++) {
                        double x = O->p[2 * i2], y = O->p[2 * i2 + 1];

                        pb_printf(S->o, "%s%ld,%ld", i2 == a ? "" : ",", far(x), far(y));
                    }
                }
            }
        }

        pb_printf(S->o, "],\"closed\":%d,\"fill\":0,\"line\":%u,\"lw\":%ld", want, (unsigned)lc, far(lw < 1 ? 1 : lw));
        put_stroke_style(S, st, lw);
        pb_putc(S->o, '}');
    }
}

static void json_str(pd_buf* o, const char* s, size_t n) {
    size_t i;

    pb_putc(o, '"');

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '"' || c == '\\') {
            pb_putc(o, '\\');
            pb_putc(o, (char)c);
        } else if (c < 32) {
            pb_printf(o, "\\u%04x", c);
        } else {
            pb_putc(o, (char)c);
        }
    }

    pb_putc(o, '"');
}

/* ------------------------------------------------------------------ */
/* elements                                                           */
/* ------------------------------------------------------------------ */

static void element(sv_t* S, int k, const sstate* parent);

/* the inherited style of an element over its parent's, its transform applied */
static void style_of(sv_t* S, int k, const sstate* parent, sstate* st) {
    const snode* x = &S->nd[k];
    char b[192];

    *st = *parent;

    if (prop(S, x, "color", b, sizeof(b))) scpy(st->color, sizeof(st->color), b);
    if (prop(S, x, "fill", b, sizeof(b))) scpy(st->fill, sizeof(st->fill), b);
    if (prop(S, x, "stroke", b, sizeof(b))) scpy(st->stroke, sizeof(st->stroke), b);
    if (prop(S, x, "fill-opacity", b, sizeof(b))) st->fill_op = strchr(b, '%') ? atof(b) / 100 : atof(b);
    if (prop(S, x, "stroke-opacity", b, sizeof(b))) st->stroke_op = strchr(b, '%') ? atof(b) / 100 : atof(b);
    if (prop(S, x, "font-size", b, sizeof(b))) {
        double fs = !strcmp(b, "small") ? 13 : !strcmp(b, "medium") ? 16 : !strcmp(b, "large") ? 18 :
                    !strcmp(b, "x-large") ? 24 : !strcmp(b, "smaller") ? st->fsize / 1.2 :
                    !strcmp(b, "larger") ? st->fsize * 1.2 : length(b, st->fsize, st->fsize, st->fsize);

        st->fsize = fs > 0 ? fs : st->fsize;
    }

    if (prop(S, x, "stroke-width", b, sizeof(b))) st->sw = length(b, hypot(S->vw, S->vh) / sqrt(2), st->fsize, 1);
    if (prop(S, x, "fill-rule", b, sizeof(b))) st->evenodd = !strcmp(b, "evenodd");
    if (prop(S, x, "stroke-linecap", b, sizeof(b))) st->cap = !strcmp(b, "round") ? 1 : !strcmp(b, "square") ? 2 : 0;
    if (prop(S, x, "stroke-linejoin", b, sizeof(b))) st->join = !strcmp(b, "round") ? 0 : !strcmp(b, "bevel") ? 1 : 2;
    if (prop(S, x, "stroke-dasharray", b, sizeof(b))) scpy(st->dash, sizeof(st->dash), b);
    if (prop(S, x, "font-family", b, sizeof(b))) {
        char* c = strchr(b, ',');
        char* p = b;
        size_t n;

        if (c) {
            *c = 0;
        }

        while (ws(*p) || *p == '\'' || *p == '"') {
            p++;
        }

        n = strlen(p);

        while (n > 0 && (ws(p[n - 1]) || p[n - 1] == '\'' || p[n - 1] == '"')) {
            p[--n] = 0;
        }

        if (!strcmp(p, "sans-serif")) {
            p = "Arial";
        } else if (!strcmp(p, "serif")) {
            p = "Times New Roman";
        } else if (!strcmp(p, "monospace")) {
            p = "Courier New";
        }

        scpy(st->family, sizeof(st->family), p);
    }

    if (prop(S, x, "font-weight", b, sizeof(b))) {
        st->weight = !strcmp(b, "bold") || !strcmp(b, "bolder") ? 700 : !strcmp(b, "normal") || !strcmp(b, "lighter") ?
                     400 : atoi(b) > 0 ? atoi(b) : st->weight;
    }

    if (prop(S, x, "font-style", b, sizeof(b))) st->italic = !strcmp(b, "italic") || !strcmp(b, "oblique");
    if (prop(S, x, "text-anchor", b, sizeof(b))) st->anchor = !strcmp(b, "middle") ? 1 : !strcmp(b, "end") ? 2 : 0;
    if (prop(S, x, "visibility", b, sizeof(b))) st->hidden = !strcmp(b, "hidden") || !strcmp(b, "collapse");

    if (prop(S, x, "opacity", b, sizeof(b))) {
        double v = strchr(b, '%') ? atof(b) / 100 : atof(b);

        st->opacity *= v != v ? 1 : v < 0 ? 0 : v > 1 ? 1 : v;
    }

    if (strcmp(x->tag, "svg") != 0 && prop(S, x, "transform", b, sizeof(b))) {
        transform(st->m, b);
    }
}

static int is_shown(sv_t* S, const snode* x) {
    char b[32];

    return !(prop(S, x, "display", b, sizeof(b)) && !strcmp(b, "none"));
}

static void rect_outline(outline* O, double x, double y, double w, double h, double rx, double ry) {
    ol_ring(O);

    if (rx <= 0 && ry <= 0) {
        ol_pt(O, x, y);
        ol_pt(O, x + w, y);
        ol_pt(O, x + w, y + h);
        ol_pt(O, x, y + h);
    } else {
        rx = rx <= 0 ? ry : rx;
        ry = ry <= 0 ? rx : ry;
        rx = rx > w / 2 ? w / 2 : rx;
        ry = ry > h / 2 ? h / 2 : ry;
        ol_pt(O, x + rx, y);
        ol_pt(O, x + w - rx, y);
        ol_arc(O, x + w - rx, y, rx, ry, 0, 0, 1, x + w, y + ry);
        ol_pt(O, x + w, y + h - ry);
        ol_arc(O, x + w, y + h - ry, rx, ry, 0, 0, 1, x + w - rx, y + h);
        ol_pt(O, x + rx, y + h);
        ol_arc(O, x + rx, y + h, rx, ry, 0, 0, 1, x, y + h - ry);
        ol_pt(O, x, y + ry);
        ol_arc(O, x, y + ry, rx, ry, 0, 0, 1, x + rx, y);
    }

    ol_close(O);
}

static void ellipse_outline(outline* O, double cx, double cy, double rx, double ry) {
    int n = segs(O, 2 * SV_PI * (rx > ry ? rx : ry) / 4) * 4, i;

    n = n < 16 ? 16 : n > 256 ? 256 : n;
    ol_ring(O);

    for (i = 0; i < n; i++) {
        double t = 2 * SV_PI * i / n;

        ol_pt(O, cx + rx * cos(t), cy + ry * sin(t));
    }

    ol_close(O);
}

/* the text of an element and the elements in it, as one line (spaces collapsed, unless xml:space preserves them) */
static void text_of(sv_t* S, int k, pd_buf* out, int preserve) {
    int c, up = 0;

    for (c = k; c >= 0 && up <= SV_MAX_DEPTH; c = S->nd[c].parent) {
        up++;
    }

    if (up > SV_MAX_DEPTH) {    /* (spans in spans past all reason) */
        return;
    }

    for (c = S->nd[k].kid; c >= 0; c = S->nd[c].next) {
        if (!S->nd[c].tag[0]) {
            pd_buf t;
            size_t i;

            memset(&t, 0, sizeof(t));
            mu_decode(S->nd[c].t, S->nd[c].tn, &t);

            for (i = 0; t.p && i < t.n; i++) {
                char ch = t.p[i];

                if (!preserve && ws(ch)) {
                    if (out->n == 0 || out->p[out->n - 1] == ' ') {
                        continue;
                    }

                    ch = ' ';
                }

                pb_putc(out, ch == '\n' || ch == '\t' || ch == '\r' ? ' ' : ch);
            }

            pb_free(&t);
        } else if (!strcmp(S->nd[c].tag, "tspan") || !strcmp(S->nd[c].tag, "a") ||
                   !strcmp(S->nd[c].tag, "textPath")) {
            text_of(S, c, out, preserve);
        }
    }
}

static void put_label(sv_t* S, const sstate* st, double x, double y, const char* s, size_t n) {
    double ox, oy, ex, ey, rot, sz = st->fsize * scale_of(st->m);
    int gr = -1;
    uint32_t c = paint(S, st, st->fill, st->fill_op * st->opacity, &gr);

    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }

    while (n > 0 && *s == ' ') {
        s++;
        n--;
    }

    if (n == 0 || st->hidden || sz <= 0) {
        return;
    }

    if (gr >= 0) {
        double pos[2];
        uint32_t col[2];

        c = stops(S, gr, pos, col, 2) > 0 ? col[0] : 0xFF000000u;
    }

    if (!c) {
        return;
    }

    apply(st->m, x, y, &ox, &oy);
    apply(st->m, x + 100, y, &ex, &ey);
    rot = atan2(ey - oy, ex - ox) * 180 / SV_PI;
    sep(S);
    pb_puts(S->o, "{\"label\":");
    json_str(S->o, s, n);
    pb_printf(S->o, ",\"x\":%ld,\"y\":%ld,\"sz\":%ld,\"w\":%d,\"i\":%d,\"c\":%u,\"ha\":%d", far(ox), far(oy),
              far(sz > 65536 ? sz : 65536), st->weight, st->italic, (unsigned)c, st->anchor);

    if (fabs(rot) > 0.5) {
        pb_printf(S->o, ",\"rot\":%ld", far((rot < 0 ? rot + 360 : rot) * 60000));
    }

    if (st->family[0]) {
        pb_puts(S->o, ",\"f\":");
        json_str(S->o, st->family, strlen(st->family));
    }

    pb_putc(S->o, '}');
}

static void text_el(sv_t* S, int k, const sstate* st) {
    char b[64];
    double x = attr_s(&S->nd[k], "x", b, sizeof(b)) ? length(b, S->vw, st->fsize, 0) : 0;
    double y = attr_s(&S->nd[k], "y", b, sizeof(b)) ? length(b, S->vh, st->fsize, 0) : 0;
    int preserve = attr_s(&S->nd[k], "xml:space", b, sizeof(b)) && !strcmp(b, "preserve");
    int c;
    pd_buf run;

    x += attr_s(&S->nd[k], "dx", b, sizeof(b)) ? length(b, S->vw, st->fsize, 0) : 0;
    y += attr_s(&S->nd[k], "dy", b, sizeof(b)) ? length(b, S->vh, st->fsize, 0) : 0;
    memset(&run, 0, sizeof(run));

    /* the text up to a tspan placed on its own (an x or y of its own): a label; each such tspan another */
    for (c = S->nd[k].kid; c >= 0; c = S->nd[c].next) {
        int placed = !strcmp(S->nd[c].tag, "tspan") && (attr(&S->nd[c], "x", &(size_t){0}) ||
                     attr(&S->nd[c], "y", &(size_t){0}));

        if (placed) {
            sstate ts;

            put_label(S, st, x, y, run.p ? run.p : "", run.n);
            run.n = 0;
            style_of(S, c, st, &ts);

            if (is_shown(S, &S->nd[c])) {
                pd_buf t;
                double tx = attr_s(&S->nd[c], "x", b, sizeof(b)) ? length(b, S->vw, ts.fsize, x) : x;
                double ty = attr_s(&S->nd[c], "y", b, sizeof(b)) ? length(b, S->vh, ts.fsize, y) : y;

                tx += attr_s(&S->nd[c], "dx", b, sizeof(b)) ? length(b, S->vw, ts.fsize, 0) : 0;
                ty += attr_s(&S->nd[c], "dy", b, sizeof(b)) ? length(b, S->vh, ts.fsize, 0) : 0;
                memset(&t, 0, sizeof(t));
                text_of(S, c, &t, preserve);
                put_label(S, &ts, tx, ty, t.p ? t.p : "", t.n);
                pb_free(&t);
                x = tx;
                y = ty;
            }
        } else if (!S->nd[c].tag[0]) {
            pd_buf t;

            memset(&t, 0, sizeof(t));
            mu_decode(S->nd[c].t, S->nd[c].tn, &t);

            if (t.p) {
                size_t i;

                for (i = 0; i < t.n; i++) {
                    char ch = t.p[i];

                    if (!preserve && ws(ch)) {
                        if (run.n == 0 || run.p[run.n - 1] == ' ') {
                            continue;
                        }

                        ch = ' ';
                    }

                    pb_putc(&run, ch == '\n' || ch == '\t' || ch == '\r' ? ' ' : ch);
                }
            }

            pb_free(&t);
        } else if (S->nd[c].tag[0]) {
            text_of(S, c, &run, preserve);
        }
    }

    put_label(S, st, x, y, run.p ? run.p : "", run.n);
    pb_free(&run);
}

/* a picture by a data: URI (PNG, JPEG, GIF; SVG drawn in its place); others are left out */
static void image_el(sv_t* S, int k, const sstate* st) {
    const snode* x = &S->nd[k];
    size_t n = 0;
    const char* h = attr(x, "href", &n);
    char b[64], mime[48];
    double ix, iy, iw, ih;
    const char* comma, *semi;
    unsigned char* data;
    size_t len;
    pd_res_id res;

    if (!h) {
        h = attr(x, "xlink:href", &n);
    }

    if (!h || n < 6 || memcmp(h, "data:", 5) != 0 || (comma = memchr(h, ',', n)) == NULL) {
        return;
    }

    semi = memchr(h, ';', (size_t)(comma - h));
    snprintf(mime, sizeof(mime), "%.*s", (int)((semi ? semi : comma) - (h + 5)), h + 5);

    if (!semi || strncmp(semi, ";base64", 7) != 0) {
        return;
    }

    data = (unsigned char*)malloc(n);

    if (!data) {
        return;
    }

    len = pd_base64_decode(comma + 1, n - (size_t)(comma + 1 - h), data);
    ix = attr_s(x, "x", b, sizeof(b)) ? length(b, S->vw, st->fsize, 0) : 0;
    iy = attr_s(x, "y", b, sizeof(b)) ? length(b, S->vh, st->fsize, 0) : 0;
    iw = attr_s(x, "width", b, sizeof(b)) ? length(b, S->vw, st->fsize, 0) : 0;
    ih = attr_s(x, "height", b, sizeof(b)) ? length(b, S->vh, st->fsize, 0) : 0;

    if (len > 0 && iw > 0 && ih > 0 && strncmp(mime, "image/", 6) == 0 && strstr(mime, "svg") == NULL &&
            pd_doc_add_resource(S->d, mime, data, len, &res) == PD_OK) {
        double ax, ay, bx, by;

        apply(st->m, ix, iy, &ax, &ay);
        apply(st->m, ix + iw, iy + ih, &bx, &by);
        sep(S);
        pb_printf(S->o, "{\"img\":%d,\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld}", (int)res, far(ax < bx ? ax : bx),
                  far(ay < by ? ay : by), far(fabs(bx - ax)), far(fabs(by - ay)));
    }

    free(data);
}

static void children(sv_t* S, int k, const sstate* st) {
    int c;

    for (c = S->nd[k].kid; c >= 0; c = S->nd[c].next) {
        if (S->nd[c].tag[0]) {
            element(S, c, st);
        }
    }
}

/* a nested svg, or the symbol a use shows: its viewport (x, y, w, h in the parent's units) and viewBox */
static void viewport(sv_t* S, int k, const sstate* parent, double x, double y, double w, double h) {
    sstate st;
    double sw = S->vw, shh = S->vh, bw = 0, bh = 0;

    style_of(S, k, parent, &st);

    if (w <= 0 || h <= 0) {
        return;
    }

    if (viewbox(st.m, &S->nd[k], x, y, w, h, &bw, &bh)) {
        S->vw = bw;
        S->vh = bh;
    } else {
        double t[6] = { 1, 0, 0, 1, x, y };

        mul(st.m, t);
        S->vw = w;
        S->vh = h;
    }

    children(S, k, &st);
    S->vw = sw;
    S->vh = shh;
}

static void element(sv_t* S, int k, const sstate* parent) {
    const snode* x = &S->nd[k];
    const char* t = x->tag;
    sstate st;
    outline O;

    if (S->depth >= SV_MAX_DEPTH || !is_shown(S, x)) {
        return;
    }

    if (!strcmp(t, "defs") || !strcmp(t, "symbol") || !strcmp(t, "clipPath") || !strcmp(t, "mask") ||
            !strcmp(t, "linearGradient") || !strcmp(t, "radialGradient") || !strcmp(t, "pattern") ||
            !strcmp(t, "marker") || !strcmp(t, "filter") || !strcmp(t, "style") || !strcmp(t, "title") ||
            !strcmp(t, "desc") || !strcmp(t, "metadata") || !strcmp(t, "script") || !strcmp(t, "foreignObject")) {
        return;
    }

    S->depth++;

    if (!strcmp(t, "svg")) {    /* a viewport inside: its own coordinates */
        double w = attr_len(x, "width", S->vw, parent->fsize, S->vw), h = attr_len(x, "height", S->vh, parent->fsize, S->vh);

        viewport(S, k, parent, attr_len(x, "x", S->vw, parent->fsize, 0), attr_len(x, "y", S->vh, parent->fsize, 0), w, h);
        S->depth--;
        return;
    }

    style_of(S, k, parent, &st);

    if (!strcmp(t, "g") || !strcmp(t, "a")) {
        children(S, k, &st);
    } else if (!strcmp(t, "switch")) {  /* the first it may show */
        int c;

        for (c = x->kid; c >= 0; c = S->nd[c].next) {
            if (S->nd[c].tag[0]) {
                element(S, c, &st);
                break;
            }
        }
    } else if (!strcmp(t, "use")) {
        int ref = href(S, x);
        double ux = attr_len(x, "x", S->vw, st.fsize, 0), uy = attr_len(x, "y", S->vh, st.fsize, 0);

        if (ref >= 0 && ref != k) {
            double tr[6] = { 1, 0, 0, 1, ux, uy };
            int a;

            for (a = S->nd[k].parent; a >= 0 && a != ref; a = S->nd[a].parent) {
            }

            if (a < 0) {    /* (not something it is inside of) */
                mul(st.m, tr);

                if (!strcmp(S->nd[ref].tag, "symbol") || !strcmp(S->nd[ref].tag, "svg")) {
                    double w = attr_len(x, "width", S->vw, st.fsize, attr_len(&S->nd[ref], "width", S->vw, st.fsize, S->vw));
                    double h = attr_len(x, "height", S->vh, st.fsize, attr_len(&S->nd[ref], "height", S->vh, st.fsize, S->vh));

                    viewport(S, ref, &st, 0, 0, w, h);
                } else {
                    element(S, ref, &st);
                }
            }
        }
    } else if (!strcmp(t, "text")) {
        text_el(S, k, &st);
    } else if (!strcmp(t, "image")) {
        image_el(S, k, &st);
    } else {
        int closed = 0;

        memset(&O, 0, sizeof(O));
        O.m = st.m;
        O.tol = 0.15 * 65536;   /* a fifth of a pixel at 96 dpi */

        if (!strcmp(t, "path")) {
            size_t n = 0;
            const char* d = attr(x, "d", &n);

            if (d) {
                char* z = (char*)malloc(n + 1);

                if (z) {
                    memcpy(z, d, n);
                    z[n] = 0;
                    path_data(&O, z);
                    free(z);
                }
            }
        } else if (!strcmp(t, "rect")) {
            double w = attr_len(x, "width", S->vw, st.fsize, 0), h = attr_len(x, "height", S->vh, st.fsize, 0);
            double rx = attr_len(x, "rx", S->vw, st.fsize, -1), ry = attr_len(x, "ry", S->vh, st.fsize, -1);

            if (w > 0 && h > 0) {
                rect_outline(&O, attr_len(x, "x", S->vw, st.fsize, 0), attr_len(x, "y", S->vh, st.fsize, 0), w, h, rx, ry);
                closed = 1;
            }
        } else if (!strcmp(t, "circle") || !strcmp(t, "ellipse")) {
            double cx = attr_len(x, "cx", S->vw, st.fsize, 0), cy = attr_len(x, "cy", S->vh, st.fsize, 0);
            double rd = hypot(S->vw, S->vh) / sqrt(2);
            double rx = t[0] == 'c' ? attr_len(x, "r", rd, st.fsize, 0) : attr_len(x, "rx", S->vw, st.fsize, 0);
            double ry = t[0] == 'c' ? rx : attr_len(x, "ry", S->vh, st.fsize, 0);

            if (t[0] == 'e' && rx <= 0 && ry > 0) {
                rx = ry;
            } else if (t[0] == 'e' && ry <= 0 && rx > 0) {
                ry = rx;
            }

            if (rx > 0 && ry > 0) {
                ellipse_outline(&O, cx, cy, rx, ry);
                closed = 1;
            }
        } else if (!strcmp(t, "line")) {
            ol_ring(&O);
            ol_pt(&O, attr_len(x, "x1", S->vw, st.fsize, 0), attr_len(x, "y1", S->vh, st.fsize, 0));
            ol_pt(&O, attr_len(x, "x2", S->vw, st.fsize, 0), attr_len(x, "y2", S->vh, st.fsize, 0));
            strcpy(st.fill, "none");
        } else if (!strcmp(t, "polyline") || !strcmp(t, "polygon")) {
            size_t n = 0;
            const char* pts = attr(x, "points", &n);

            if (pts) {
                char* z = (char*)malloc(n + 1);
                const char* p, *e;
                double v0, v1;

                if (z) {
                    memcpy(z, pts, n);
                    z[n] = 0;
                    ol_ring(&O);

                    for (p = z; num(p, &e, &v0) && num(e, &p, &v1);) {
                        ol_pt(&O, v0, v1);
                    }

                    if (t[4] == 'g') {
                        ol_close(&O);
                        closed = 1;
                    }

                    free(z);
                }
            }
        }

        draw_outline(S, &O, &st, closed);
        ol_free(&O);
    }

    S->depth--;
}

/* ------------------------------------------------------------------ */
/* entry points                                                       */
/* ------------------------------------------------------------------ */

int pd_svg_is(const unsigned char* p, size_t n) {
    size_t i, lim = n < 4096 ? n : 4096;

    for (i = 0; i + 4 <= lim; i++) {
        if (p[i] == '<' && !memcmp(p + i + 1, "svg", 3) && (i + 4 == n || ws((char)p[i + 4]) || p[i + 4] == '>')) {
            return 1;
        }
    }

    return 0;
}

/* the drawing description of an SVG into o: {"w", "h", "src", "items": [...]}; 1 if anything was drawn */
int pd_svg_json(pd_doc* d, const unsigned char* p, size_t n, pd_res_id src, pd_buf* o) {
    sv_t S;
    int root, k, ok = 0;
    sstate st;
    double w, h, bw = 0, bh = 0;
    char b[128];

    memset(&S, 0, sizeof(S));
    S.d = d;
    S.s = (const char*)p;
    S.n = n;
    S.o = o;
    S.first = 1;

    if ((root = build_tree(&S)) < 0) {
        free(S.nd);
        return 0;
    }

    for (k = 0; k < S.nn; k++) {    /* the style sheets */
        if (!strcmp(S.nd[k].tag, "style")) {
            int c;

            for (c = S.nd[k].kid; c >= 0; c = S.nd[c].next) {
                if (!S.nd[c].tag[0]) {
                    parse_css(&S, S.nd[c].t, S.nd[c].tn);
                }
            }
        }
    }

    {   /* its size: width and height (100% and none: the viewBox's), 300 x 150 as a browser's without either */
        const snode* r = &S.nd[root];
        double vb[4] = { 0, 0, 0, 0 };
        const char* q = b, *e;
        int nv = 0;

        if (attr_s(r, "viewBox", b, sizeof(b))) {
            while (nv < 4 && num(q, &e, &vb[nv])) {
                q = e;
                nv++;
            }
        }

        w = attr_s(r, "width", b, sizeof(b)) && !strchr(b, '%') ? length(b, 0, 16, 0) : 0;
        h = attr_s(r, "height", b, sizeof(b)) && !strchr(b, '%') ? length(b, 0, 16, 0) : 0;

        if (nv == 4 && vb[2] > 0 && vb[3] > 0) {
            if (w <= 0 && h <= 0) {
                w = vb[2];
                h = vb[3];
            } else if (w <= 0) {
                w = h * vb[2] / vb[3];
            } else if (h <= 0) {
                h = w * vb[3] / vb[2];
            }
        }

        w = w > 0 && w < 1e5 ? w : w >= 1e5 ? 1e5 : 300;      /* (a picture over 100000 px: that big) */
        h = h > 0 && h < 1e5 ? h : h >= 1e5 ? 1e5 : 150;
    }

    memset(&st, 0, sizeof(st));
    st.m[0] = st.m[3] = SP_PER_PX;  /* px to sp */
    strcpy(st.fill, "black");
    strcpy(st.stroke, "none");
    strcpy(st.color, "black");
    st.fill_op = st.stroke_op = st.opacity = 1;
    st.sw = 1;
    st.join = 2;
    st.fsize = 16;
    st.weight = 400;
    S.vw = w;
    S.vh = h;

    {
        sstate rs;

        style_of(&S, root, &st, &rs);

        if (viewbox(rs.m, &S.nd[root], 0, 0, w, h, &bw, &bh)) {
            S.vw = bw;
            S.vh = bh;
        }

        pb_printf(o, "{\"w\":%ld,\"h\":%ld,\"src\":%d,\"kind\":\"svg\",\"items\":[", far(w * SP_PER_PX),
                  far(h * SP_PER_PX), (int)src);
        children(&S, root, &rs);
        pb_puts(o, "]}");
    }

    ok = !o->err && !S.first;
    free(S.nd);
    free(S.ru);
    return ok;
}

pd_res_id pd_svg_drawing(pd_doc* d, const unsigned char* p, size_t n, pd_res_id src) {
    pd_buf o;
    pd_res_id res = 0;
    int32_t n0 = pd_doc_res_count(d);

    memset(&o, 0, sizeof(o));

    if (pd_svg_json(d, p, n, src, &o) && pd_doc_add_resource(d, PD_DRAWING_MIME, o.p, o.n, &res) == PD_OK) {
        pd_doc_res_derived(d, res, src, (pd_res_id)n0 + 1, res - 1);   /* made again from the SVG on load */
    } else {
        res = 0;
    }

    pb_free(&o);
    return res;
}

pd_status pd_doc_add_picture(pd_doc* d, const char* mime, const void* data, size_t len, pd_res_id* shown) {
    const unsigned char* p = (const unsigned char*)data;
    int32_t w, h, meta;
    pd_res_id src;
    pd_status st;

    if (!d || !data || len == 0 || !shown) {
        return PD_ERR_ARG;
    }

    *shown = 0;
    meta = pd_metafile_kind(p, len);

    if (!mime || !mime[0]) {
        mime = meta == 1 ? "image/x-emf" : meta == 2 ? "image/x-wmf" : pd_svg_is(p, len) ? "image/svg+xml" :
               pd_doc_image_info(p, len, &w, &h);
    }

    if (!mime) {
        return PD_ERR_FORMAT;
    }

    if ((st = pd_doc_add_resource(d, mime, data, len, &src)) != PD_OK) {
        return st;
    }

    if (!strcmp(mime, "image/svg+xml")) {
        *shown = pd_svg_drawing(d, p, len, src);
    } else if (meta) {
        *shown = pd_metafile_drawing(d, p, len, src);
    } else {
        *shown = src;
    }

    return *shown ? PD_OK : PD_ERR_FORMAT;
}

/* the SVG a drawing was made from (0 if it is no drawing, or was made from something else): what Office's
   asvg:svgBlip names, beside a picture of it */
pd_res_id pd_svg_source(const pd_doc* d, pd_res_id res) {
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    pd_res_id src = 0, lo, hi;

    if (pd_doc_resource(d, res, &mime, &data, &len) != PD_OK || strcmp(mime, PD_DRAWING_MIME) != 0) {
        return 0;
    }

    if (!pd_doc_res_origin(d, res, &src, &lo, &hi) || pd_doc_resource(d, src, &mime, &data, &len) != PD_OK ||
            strcmp(mime, "image/svg+xml") != 0) {
        return 0;
    }

    return src;
}

typedef struct {
    pd_buf* b;
} png_sink;

static int to_buf(void* user, const void* data, size_t len) {
    pb_put(((png_sink*)user)->b, data, len);
    return ((png_sink*)user)->b->err;
}

/* a PNG of a drawing for a reader that cannot show SVG: at twice its size in pixels a point, the longer side at
   most 2048; 1 if made */
int pd_svg_fallback_png(const pd_doc* d, pd_res_id drawing, pd_buf* out) {
    const char* mime = NULL, *j;
    const void* data = NULL;
    size_t len = 0, i;
    double w = 0, h = 0, sc;
    png_sink k;

    if (pd_doc_resource(d, drawing, &mime, &data, &len) != PD_OK) {
        return 0;
    }

    j = (const char*)data;
    w = h = 0;

    for (i = 0; i + 5 < len && i < 256; i++) {
        if (!memcmp(j + i, "\"w\":", 4) && w == 0) {
            w = atof(j + i + 4);
        } else if (!memcmp(j + i, "\"h\":", 4) && h == 0) {
            h = atof(j + i + 4);
        }
    }

    if (w <= 0 || h <= 0) {
        return 0;
    }

    sc = 2;     /* pixels a point */

    if (w / 65536 * sc > 2048 || h / 65536 * sc > 2048) {
        sc = 2048 / ((w > h ? w : h) / 65536);
    }

    k.b = out;

    if (pd_drawing_to_png(d, drawing, (pd_sp)w, (pd_sp)h, (int32_t)(sc * 65536), to_buf, &k) == PD_OK && !out->err &&
            out->n > 0) {
        return 1;
    }

    out->n = 0;     /* (no fonts to draw it with, say): a clear pixel, so that the package still holds together */
    out->err = 0;
    {
        static const uint8_t clear[4] = { 0, 0, 0, 0 };
        unsigned char* png = NULL;
        size_t pn = 0;

        if (pd_png_encode(clear, 1, 1, 4, &png, &pn) == 0) {
            pb_put(out, png, pn);
            free(png);
            return !out->err;
        }
    }

    return 0;
}

void pd_put_blip(pd_buf* o, const char* rid, const char* svg_rid, const char* attrs) {
    pb_printf(o, "<a:blip r:embed=\"%s\"%s", rid, attrs ? attrs : "");

    if (svg_rid && svg_rid[0]) {
        pb_printf(o, "><a:extLst><a:ext uri=\"{96DAC541-7B7A-43D3-8B79-37D633B846F1}\"><asvg:svgBlip "
                  "xmlns:asvg=\"http://schemas.microsoft.com/office/drawing/2016/SVG/main\" r:embed=\"%s\"/></a:ext>"
                  "</a:extLst></a:blip>", svg_rid);
    } else {
        pb_puts(o, "/>");
    }
}

pd_res_id pd_doc_picture_source(const pd_doc* d, pd_res_id res) {
    pd_res_id src = 0, lo, hi;
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;

    if (!d || pd_doc_resource(d, res, &mime, &data, &len) != PD_OK || strcmp(mime, PD_DRAWING_MIME) != 0 ||
            !pd_doc_res_origin(d, res, &src, &lo, &hi)) {
        return 0;
    }

    return src;
}
