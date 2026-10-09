/*
 * SmartArt laid out from its definition, as PowerPoint lays one out before it saves the drawing of it: the
 * data model's points (its nodes, and the transitions between them) made the layout definition's tree of
 * layout nodes (its forEach and choose worked through), their constraints worked out from the frame down, the
 * algorithms placing each node's children (composite, lin, snake, hierRoot and hierChild; tx, sp and conn
 * the leaves), the text fitted to its shapes by the rules (fonts made smaller, shapes taller), and the shapes
 * written with the quick style's and the colour definition's fills and lines. ECMA-376 Part 1, 21.4.
 *
 * Its text is measured with a sans face's widths, scaled for the face named: the fonts are the host's, and
 * only there when the document is laid out, after it is read.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_dgm.h"
#include "pd_preset.h"

#define EMU_MM 36000.0
#define EMU_PT 12700.0
#define MAXPT 4096
#define MAXPN 4096
#define MAXSEQ 4096
#define BIT(c) (1ULL << (c))

/* ---- the data model ---- */

enum { PT_NODE, PT_ASST, PT_DOC, PT_PAR, PT_SIB, PT_PRES };

typedef struct {
    char id[48];
    int type;
    int el;                     /* its element */
    int t, pr;                  /* its text (dgm:t) and its prSet, -1 none */
    int parent;                 /* a node: the node it is a child of */
    int kid, next;              /* a node: its children, in order */
    int partr, sibtr;           /* a node: the transitions to it and after it */
    int owner;                  /* a transition: the node it is of */
    int depth;
    long ord;
    char pname[64];             /* a presentation point: the layout node it is of, */
    int assoc;                  /* and the point it presents */
} dpt;

/* ---- constraints ---- */

enum {
    C_W, C_H, C_L, C_T, C_R, C_B, C_CX, C_CY, C_FONT, C_SFONT, C_SP, C_SIBSP, C_SECSIBSP, C_TM, C_BM, C_LM, C_RM,
    C_BEGPAD, C_ENDPAD, C_CONNDIST, C_ALIGNOFF, C_BENDDIST, C_WARH, C_HARH, C_STEM, C_DIAM, C_BEGM, C_ENDM, C_USER,
    C_N = C_USER + 26          /* userA to userZ */
};

static const char* const ct_name[C_USER] = {
    "w", "h", "l", "t", "r", "b", "ctrX", "ctrY", "primFontSz", "secFontSz", "sp", "sibSp", "secSibSp", "tMarg",
    "bMarg", "lMarg", "rMarg", "begPad", "endPad", "connDist", "alignOff", "bendDist", "wArH", "hArH", "stemThick",
    "diam", "begMarg", "endMarg"
};

/* what each is in: L lengths (mm, kept in EMU), F font sizes (points), M margins (points, kept in EMU), R ratios */
static const char ct_unit[C_N + 1] = "LLLLLLLLFFLLLMMMMLLLRLRRLLLLLLLLLLLLLLLLLLLLLLLLLLLLLL";

/* ---- the layout nodes made ---- */

typedef struct {
    int ln;                     /* its layoutNode */
    char name[64];
    int pt;                     /* the point it is for */
    int parent, kid, next, last;
    int alg, shape;             /* its alg and shape elements, -1 none */
    int con[16], ncon, rul[8], nrul, var[8], nvar;
    int of, nof;                /* the points whose text it shows: D->ofs[of ...] */
    int pres;                   /* its presentation point in the data model, -1 none */
    char lbl[48];               /* its style label */
    int sidx, scnt;             /* which of those with it it is, of how many */
    double v[C_N], lo[C_N], hi[C_N];
    unsigned long long set, lom, him;   /* which of them are given; bounds given */
    unsigned long long fdep;    /* which follow a font size */
    int fgrp, hgrp;             /* the constraints making font sizes, heights the same (op="equ") */
    double padf[2];             /* begPad, endPad as parts of the distance it connects */
    unsigned padm;
    double mf[4];               /* its margins as parts of its font size (t, b, l, r), -1 not */
    double fmax;                /* its font size before the text was fitted */
    double hb;                  /* its height as its constraints have it, before its text's */
    double x, y, w, h;          /* relative to its parent's while laid out, then absolute */
    double bw, bh;              /* a composite: how far its children reach */
    double scl;                 /* how much smaller it was made than asked for */
    double csx, csy;            /* the user's scaling of it (custScaleX, custScaleY) */
    double font;                /* its text's size (points) */
    double ofont, oh;           /* from its text fitted: its font size, its height asked for; -1 none */
    double fcap;                /* the size its parent's room lets its text be, -1 none */
    double flo, fhi;            /* sizes known to fit its parent's room, and not to */
    double fitf;                /* the size its text was last fitted at */
    int tried;                  /* the time round its size was last tried */
    unsigned long long keep;    /* its w, h given (BIT(C_W), BIT(C_H)): its own constraints not to change them */
    int custt;                  /* its text's sizes the user's */
} pnode;

typedef struct {
    const pd_dgm_in* in;
    const xdoc* dm, *lo;
    dpt* p;
    int np, doc;
    pnode* n;
    int nn, root;
    int* ofs;
    int nofs, cofs;
    int* t1, *t2, *t3;          /* nav's */
    int navbase;                /* the depth of the points nav's first step came to */
    int fail, again, depth, final, iter;
} dgm;

/* ---- small things ---- */

static int at(const xdoc* d, int n, const char* name, char* buf, size_t cap) {
    return xd_attr(d, n, name, buf, cap) && buf[0];
}

static double atf(const xdoc* d, int n, const char* name, double def) {
    char v[40];

    return at(d, n, name, v, sizeof(v)) ? strtod(v, NULL) : def;
}

/* the K-th of a list of words (from 0), 0 when it has not as many */
static int word(const char* s, int k, char* out, size_t cap) {
    while (*s) {
        size_t n = 0;

        while (*s == ' ') {
            s++;
        }

        if (!*s) {
            break;
        }

        while (s[n] && s[n] != ' ') {
            n++;
        }

        if (k-- == 0) {
            snprintf(out, cap, "%.*s", (int)n, s);
            return 1;
        }

        s += n;
    }

    out[0] = '\0';
    return 0;
}

static int nwords(const char* s) {
    char w[64];
    int k = 0;

    while (word(s, k, w, sizeof(w))) {
        k++;
    }

    return k;
}

static int has(const pnode* N, int c) {
    return (N->set & BIT(c)) != 0;
}

static double unit_of(int c) {
    return ct_unit[c] == 'L' ? EMU_MM : ct_unit[c] == 'M' ? EMU_PT : 1;
}

static int ct_index(const char* s) {
    int k;

    if (!strncmp(s, "user", 4) && s[4] >= 'A' && s[4] <= 'Z' && !s[5]) {
        return C_USER + (s[4] - 'A');
    }

    for (k = 0; k < C_USER; k++) {
        if (!strcmp(ct_name[k], s)) {
            return k;
        }
    }

    return -1;
}

/* ---- text measured ---- */

/* Helvetica's advances (1/1000 em), space to tilde */
static const short sans_w[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556, 556, 556,
    556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667,
    556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556, 333, 556,
    556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722,
    500, 500, 500, 334, 260, 334, 584
};

/* how wide a face is against Helvetica */
static double face_factor(const char* f) {
    static const struct {
        const char* name;
        double k;
    } t[] = {
        { "Calibri", 0.89 }, { "Arial", 1.0 }, { "Helvetica", 1.0 }, { "Liberation Sans", 1.0 }, { "Aptos", 0.93 },
        { "Segoe", 0.97 }, { "Verdana", 1.13 }, { "Tahoma", 0.98 }, { "Trebuchet", 0.97 }, { "Century Gothic", 1.1 },
        { "Gill Sans", 0.9 }, { "Times", 0.9 }, { "Cambria", 0.93 }, { "Georgia", 1.0 }, { "Garamond", 0.86 }
    };
    size_t k;

    for (k = 0; f && k < sizeof(t) / sizeof(t[0]); k++) {
        if (strstr(f, t[k].name)) {
            return t[k].k;
        }
    }

    return 0.95;
}

/* the next character of UTF-8 text, entities read */
static unsigned next_char(const char* s, size_t n, size_t* i) {
    unsigned char c = (unsigned char)s[*i];
    unsigned u;
    int more;

    if (c == '&') {
        static const struct {
            const char* e;
            unsigned u;
        } ent[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' } };
        size_t k;

        for (k = 0; k < sizeof(ent) / sizeof(ent[0]); k++) {
            size_t l = strlen(ent[k].e);

            if (*i + l <= n && !memcmp(s + *i, ent[k].e, l)) {
                *i += l;
                return ent[k].u;
            }
        }
    }

    (*i)++;

    if (c < 0x80) {
        return c;
    }

    more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    u = c & (0x3F >> more);

    while (more-- && *i < n && ((unsigned char)s[*i] & 0xC0) == 0x80) {
        u = (u << 6) | ((unsigned char)s[(*i)++] & 0x3F);
    }

    return u;
}

static double char_w(unsigned u) {
    return u >= 32 && u < 127 ? sans_w[u - 32] : u >= 0x2E80 ? 1000 : u == 0x2022 ? 350 : 556;
}

/* the lines text takes at a width (EMU) and size (points), broken at spaces and after hyphens; *over when a
   word is wider than the line */
static int text_lines(const char* s, size_t n, double width, double f, double k, int* over) {
    double line = 0, wordw = 0, gap = 0, space = char_w(' ') * f * k * EMU_PT / 1000;
    int lines = 1, inword = 0;
    size_t i = 0;

    while (i <= n) {
        unsigned u = i < n ? next_char(s, n, &i) : (i++, ' ');
        int hyphen = u == '-' && inword;

        if (hyphen) {
            wordw += char_w(u) * f * k * EMU_PT / 1000;
        }

        if (u == ' ' || u == '\t' || hyphen) {
            if (inword) {
                if (wordw > width) {
                    *over = 1;
                }

                if (line > 0 && line + gap + wordw > width) {
                    lines++;
                    line = wordw;
                } else {
                    line += (line > 0 ? gap : 0) + wordw;
                }

                wordw = 0;
                inword = 0;
            }

            gap = hyphen ? 0 : space;
        } else {
            wordw += char_w(u) * f * k * EMU_PT / 1000;
            inword = 1;
        }
    }

    return lines;
}

/* ---- the data model read ---- */

static int pt_find(const dgm* D, const char* id) {
    int k;

    for (k = 0; id[0] && k < D->np; k++) {
        if (!strcmp(D->p[k].id, id)) {
            return k;
        }
    }

    return -1;
}

static void set_depth(dgm* D, int p, int depth) {
    int c;

    D->p[p].depth = depth;

    for (c = D->p[p].kid; c >= 0 && depth < 64; c = D->p[c].next) {
        set_depth(D, c, depth + 1);
    }
}

static int data_load(dgm* D) {
    const xdoc* m = D->dm;
    char v[64];
    int k;

    for (k = 0; k < m->nv && D->np < MAXPT; k++) {
        dpt* P;

        if (strcmp(m->v[k].name, "pt") || m->v[k].parent < 0 || strcmp(m->v[m->v[k].parent].name, "ptLst")) {
            continue;
        }

        P = &D->p[D->np++];
        memset(P, 0, sizeof(*P));
        at(m, k, "modelId", P->id, sizeof(P->id));
        at(m, k, "type", v, sizeof(v));
        P->type = !strcmp(v, "asst") ? PT_ASST : !strcmp(v, "doc") ? PT_DOC : !strcmp(v, "parTrans") ? PT_PAR :
                  !strcmp(v, "sibTrans") ? PT_SIB : !strcmp(v, "pres") ? PT_PRES : PT_NODE;
        P->el = k;
        P->t = xd_kid(m, k, "t");
        P->pr = xd_kid(m, k, "prSet");
        P->parent = P->kid = P->next = P->partr = P->sibtr = P->owner = P->assoc = -1;
        at(m, P->pr, "presName", P->pname, sizeof(P->pname));
    }

    for (k = 0; k < D->np; k++) {
        if (D->p[k].type == PT_PRES && at(m, D->p[k].pr, "presAssocID", v, sizeof(v))) {
            D->p[k].assoc = pt_find(D, v);
        }

        if (D->p[k].type == PT_DOC && D->doc < 0) {
            D->doc = k;
        }
    }

    for (k = 0; k < m->nv; k++) {   /* the children: parOf connections */
        int par, c, *link;

        if (strcmp(m->v[k].name, "cxn") || (at(m, k, "type", v, sizeof(v)) && strcmp(v, "parOf"))) {
            continue;
        }

        at(m, k, "srcId", v, sizeof(v));
        par = pt_find(D, v);
        at(m, k, "destId", v, sizeof(v));
        c = pt_find(D, v);

        if (par < 0 || c < 0 || c == par || D->p[c].parent >= 0) {
            continue;
        }

        D->p[c].parent = par;
        D->p[c].ord = (long)atf(m, k, "srcOrd", 0);
        at(m, k, "parTransId", v, sizeof(v));

        if ((D->p[c].partr = pt_find(D, v)) >= 0) {
            D->p[D->p[c].partr].owner = c;
        }

        at(m, k, "sibTransId", v, sizeof(v));

        if ((D->p[c].sibtr = pt_find(D, v)) >= 0) {
            D->p[D->p[c].sibtr].owner = c;
        }

        for (link = &D->p[par].kid; *link >= 0 && D->p[*link].ord <= D->p[c].ord; link = &D->p[*link].next) {
        }

        D->p[c].next = *link;
        *link = c;
    }

    if (D->doc < 0) {
        return 0;
    }

    set_depth(D, D->doc, 0);
    return 1;
}

/* ---- the axes ---- */

static int add(int* out, int n, int cap, int v) {
    if (n < cap && v >= 0) {
        out[n++] = v;
    }

    return n;
}

static int ch_of(const dgm* D, int p, int hide, int* out, int n, int cap) {
    int c;

    if (p < 0 || D->p[p].type == PT_PAR || D->p[p].type == PT_SIB || D->p[p].type == PT_PRES) {
        return n;
    }

    for (c = D->p[p].kid; c >= 0; c = D->p[c].next) {
        n = add(out, n, cap, D->p[c].partr);
        n = add(out, n, cap, c);

        if (!hide || D->p[c].next >= 0) {
            n = add(out, n, cap, D->p[c].sibtr);
        }
    }

    return n;
}

static int des_of(const dgm* D, int p, int hide, int* out, int n, int cap, int depth) {
    int c;

    if (p < 0 || depth > 64 || D->p[p].type == PT_PAR || D->p[p].type == PT_SIB || D->p[p].type == PT_PRES) {
        return n;
    }

    for (c = D->p[p].kid; c >= 0; c = D->p[c].next) {
        n = add(out, n, cap, D->p[c].partr);
        n = add(out, n, cap, c);
        n = des_of(D, c, hide, out, n, cap, depth + 1);

        if (!hide || D->p[c].next >= 0) {
            n = add(out, n, cap, D->p[c].sibtr);
        }
    }

    return n;
}

static int par_of(const dgm* D, int p) {
    if (p < 0) {
        return -1;
    }

    if (D->p[p].type == PT_PAR || D->p[p].type == PT_SIB) {
        return D->p[p].owner >= 0 ? D->p[D->p[p].owner].parent : -1;
    }

    return D->p[p].parent;
}

/* the points along an axis from one, in order */
static int axis_of(const dgm* D, int p, const char* axis, int hide, int* out, int cap) {
    int n = 0, q, k;

    if (!strcmp(axis, "self")) {
        n = add(out, n, cap, p);
    } else if (!strcmp(axis, "ch")) {
        n = ch_of(D, p, hide, out, n, cap);
    } else if (!strcmp(axis, "des") || !strcmp(axis, "desOrSelf")) {
        if (axis[3]) {
            n = add(out, n, cap, p);
        }

        n = des_of(D, p, hide, out, n, cap, 0);
    } else if (!strcmp(axis, "par")) {
        n = add(out, n, cap, par_of(D, p));
    } else if (!strcmp(axis, "ancst") || !strcmp(axis, "ancstOrSelf")) {
        for (q = axis[5] ? p : par_of(D, p), k = 0; q >= 0 && k < 64; q = par_of(D, q), k++) {
            n = add(out, n, cap, q);
        }
    } else if (!strcmp(axis, "root")) {
        n = add(out, n, cap, D->doc);
    } else if (!strcmp(axis, "followSib") || !strcmp(axis, "precedSib")) {
        int m = ch_of(D, par_of(D, p), hide, D->t3, 0, MAXSEQ), i = 0;

        while (i < m && D->t3[i] != p) {
            i++;
        }

        if (i < m && axis[0] == 'f') {
            for (k = i + 1; k < m; k++) {
                n = add(out, n, cap, D->t3[k]);
            }
        } else if (i < m) {
            for (k = 0; k < i; k++) {
                n = add(out, n, cap, D->t3[k]);
            }
        }
    }

    return n;
}

static int pt_is(int type, const char* t) {
    if (!*t || !strcmp(t, "all")) {
        return 1;
    }

    if (!strcmp(t, "node")) {
        return type == PT_NODE || type == PT_ASST;
    }

    if (!strcmp(t, "norm") || !strcmp(t, "nonAsst")) {
        return type == PT_NODE;
    }

    if (!strcmp(t, "nonNorm")) {
        return type != PT_NODE;
    }

    return (!strcmp(t, "asst") && type == PT_ASST) || (!strcmp(t, "doc") && type == PT_DOC) ||
           (!strcmp(t, "parTrans") && type == PT_PAR) || (!strcmp(t, "sibTrans") && type == PT_SIB) ||
           (!strcmp(t, "pres") && type == PT_PRES);
}

/* the points an element's axis, ptType, st, cnt and step come to from one (its own when it has no axis and
   SELF) */
static int nav(dgm* D, int ctx, int el, int self, int* out, int cap) {
    const xdoc* L = D->lo;
    char axis[160], ptt[160], st[80], cnt[80], step[80], hl[16], w[64];
    int ncur = 1, steps, s, i;

    D->navbase = ctx >= 0 ? D->p[ctx].depth : 0;

    if (!at(L, el, "axis", axis, sizeof(axis))) {
        if (!self || cap < 1) {
            return 0;
        }

        out[0] = ctx;
        return 1;
    }

    at(L, el, "ptType", ptt, sizeof(ptt));
    at(L, el, "st", st, sizeof(st));
    at(L, el, "cnt", cnt, sizeof(cnt));
    at(L, el, "step", step, sizeof(step));
    at(L, el, "hideLastTrans", hl, sizeof(hl));
    steps = nwords(axis);
    D->t1[0] = ctx;

    for (s = 0; s < steps; s++) {
        int nn = 0, hide = !(hl[0] == '0' || !strcmp(hl, "false")), a = 1, c = 0, by = 1;
        char ax[32], pt[32];

        word(axis, s, ax, sizeof(ax));

        if (!word(ptt, s, pt, sizeof(pt))) {
            snprintf(pt, sizeof(pt), "all");
        }

        if (word(st, s, w, sizeof(w))) {
            a = atoi(w);
        }

        if (word(cnt, s, w, sizeof(w))) {
            c = atoi(w);
        }

        if (word(step, s, w, sizeof(w)) && atoi(w)) {
            by = atoi(w);
        }

        for (i = 0; i < ncur; i++) {
            int m = axis_of(D, D->t1[i], ax, hide, D->t3, MAXSEQ), f = 0, k, took = 0;

            for (k = 0; k < m; k++) {   /* those of its type */
                if (pt_is(D->p[D->t3[k]].type, pt)) {
                    D->t3[f++] = D->t3[k];
                }
            }

            for (k = a > 0 ? a - 1 : f + a; k >= 0 && k < f && (!c || took < c); k += by, took++) {
                nn = add(D->t2, nn, MAXSEQ, D->t3[k]);
            }
        }

        memcpy(D->t1, D->t2, sizeof(int) * (size_t)nn);
        ncur = nn;

        if (s == 0 && nn) {
            D->navbase = D->p[D->t1[0]].depth;
        }
    }

    if (ncur > cap) {
        ncur = cap;
    }

    memcpy(out, D->t1, sizeof(int) * (size_t)ncur);
    return ncur;
}

/* ---- variables, conditions ---- */

static const char* var_default(const char* name) {
    static const char* const t[][2] = {
        { "dir", "norm" }, { "hierBranch", "std" }, { "chMax", "-1" }, { "chPref", "-1" }, { "bulletEnabled", "0" },
        { "animLvl", "none" }, { "animOne", "one" }, { "orgChart", "0" }, { "resizeHandles", "rel" }
    };
    size_t k;

    for (k = 0; k < sizeof(t) / sizeof(t[0]); k++) {
        if (!strcmp(t[k][0], name)) {
            return t[k][1];
        }
    }

    return "";
}

/* a variable of a layout node: its presentation point's, its own, its ancestors' */
static void var_of(const dgm* D, int n, const char* name, char* out, size_t cap) {
    int k, i;

    for (k = n; k >= 0; k = D->n[k].parent) {
        const pnode* N = &D->n[k];

        if (N->pres >= 0) {
            int e = xd_kid(D->dm, xd_kid(D->dm, D->p[N->pres].pr, "presLayoutVars"), name);

            if (e >= 0 && at(D->dm, e, "val", out, cap)) {
                return;
            }
        }

        for (i = N->nvar - 1; i >= 0; i--) {
            int e = xd_kid(D->lo, N->var[i], name);

            if (e >= 0) {
                if (!at(D->lo, e, "val", out, cap)) {
                    snprintf(out, cap, "%s", var_default(name));
                }

                return;
            }
        }
    }

    snprintf(out, cap, "%s", var_default(name));
}

/* where a point is among its parent's children of its type: from 1; *of how many */
static int pos_of(dgm* D, int p, int* of) {
    int m = ch_of(D, par_of(D, p), 1, D->t3, 0, MAXSEQ), k, i = 0, pos = 0;

    for (k = 0; k < m; k++) {
        int q = D->t3[k];

        if ((D->p[q].type == PT_NODE || D->p[q].type == PT_ASST) == (D->p[p].type == PT_NODE ||
                D->p[p].type == PT_ASST) && (D->p[q].type == D->p[p].type || D->p[p].type <= PT_ASST)) {
            i++;

            if (q == p) {
                pos = i;
            }
        }
    }

    *of = i;
    return pos;
}

static int cond(dgm* D, int cur, int ctx, int el) {
    const xdoc* L = D->lo;
    char fn[32], arg[64], op[16], val[64], sv[64] = "";
    double x = 0, y;
    char* end;
    int numeric;

    at(L, el, "func", fn, sizeof(fn));
    at(L, el, "arg", arg, sizeof(arg));
    at(L, el, "op", op, sizeof(op));
    at(L, el, "val", val, sizeof(val));

    if (!strcmp(fn, "var")) {
        var_of(D, cur, arg, sv, sizeof(sv));
        x = strtod(sv, &end);
        numeric = end != sv && !*end;
    } else {
        int* pts = (int*)malloc(sizeof(int) * MAXSEQ), n, k, of;

        if (!pts) {
            return 0;
        }

        n = nav(D, ctx, el, 1, pts, MAXSEQ);
        numeric = 1;

        if (!strcmp(fn, "cnt")) {
            x = n;
        } else if (!strcmp(fn, "depth")) {
            x = n ? D->p[pts[0]].depth : 0;
        } else if (!strcmp(fn, "maxDepth")) {
            for (k = 0, x = 0; k < n; k++) {
                if (D->p[pts[k]].depth - D->navbase > x) {
                    x = D->p[pts[k]].depth - D->navbase;
                }
            }
        } else if (n && (!strcmp(fn, "pos") || !strcmp(fn, "revPos") || !strcmp(fn, "posEven") ||
                         !strcmp(fn, "posOdd"))) {
            int pos = pos_of(D, pts[0], &of);

            x = !strcmp(fn, "pos") ? pos : !strcmp(fn, "revPos") ? of - pos + 1 : !strcmp(fn, "posEven") ?
                pos % 2 == 0 : pos % 2 == 1;
        }

        free(pts);
        snprintf(sv, sizeof(sv), "%g", x);
    }

    y = strtod(val, &end);

    if (numeric && end != val && !*end) {
        return !strcmp(op, "equ") ? x == y : !strcmp(op, "neq") ? x != y : !strcmp(op, "gt") ? x > y :
               !strcmp(op, "lt") ? x < y : !strcmp(op, "gte") ? x >= y : !strcmp(op, "lte") ? x <= y : 0;
    }

    return !strcmp(op, "equ") ? !strcmp(sv, val) : !strcmp(op, "neq") ? strcmp(sv, val) != 0 : 0;
}

/* ---- the layout nodes made from the definition ---- */

static int pn_new(dgm* D, int ln, int ctx, int parent) {
    pnode* N;
    int k;

    if (D->nn >= MAXPN) {
        return -1;
    }

    N = &D->n[D->nn];
    memset(N, 0, sizeof(*N));
    N->ln = ln;
    at(D->lo, ln, "name", N->name, sizeof(N->name));
    at(D->lo, ln, "styleLbl", N->lbl, sizeof(N->lbl));
    N->pt = ctx;
    N->parent = parent;
    N->kid = N->next = N->last = N->alg = N->shape = N->pres = -1;
    N->ofont = N->oh = N->fcap = N->fitf = -1;

    for (k = 0; k < D->np; k++) {
        if (D->p[k].type == PT_PRES && D->p[k].assoc == ctx && !strcmp(D->p[k].pname, N->name)) {
            N->pres = k;
            break;
        }
    }

    if (parent >= 0) {
        pnode* P = &D->n[parent];

        if (P->last >= 0) {
            D->n[P->last].next = D->nn;
        } else {
            P->kid = D->nn;
        }

        P->last = D->nn;
    }

    return D->nn++;
}

static int foreach_named(const xdoc* L, const char* name) {
    char v[64];
    int k;

    for (k = 0; k < L->nv; k++) {
        if (!strcmp(L->v[k].name, "forEach") && at(L, k, "name", v, sizeof(v)) && !strcmp(v, name)) {
            return k;
        }
    }

    return -1;
}

static void build(dgm* D, int el, int ctx, int cur) {
    const xdoc* L = D->lo;
    int k;

    if (++D->depth > 128) {
        D->depth--;
        return;
    }

    for (k = L->v[el].kid; k >= 0 && !D->fail; k = L->v[k].next) {
        const char* nm = L->v[k].name;

        if (!strcmp(nm, "layoutNode")) {
            int n = pn_new(D, k, ctx, cur);

            if (n < 0) {
                D->fail = 1;
                break;
            }

            if (cur < 0 && D->root < 0) {
                D->root = n;
            }

            build(D, k, ctx, n);
        } else if (!strcmp(nm, "forEach")) {
            int body = k, *pts, np, i;
            char ref[64];

            if (at(L, k, "ref", ref, sizeof(ref))) {
                body = foreach_named(L, ref);
            }

            if (body < 0 || (pts = (int*)malloc(sizeof(int) * MAXSEQ)) == NULL) {
                continue;
            }

            np = nav(D, ctx, body, 0, pts, MAXSEQ);

            for (i = 0; i < np && !D->fail; i++) {
                build(D, body, pts[i], cur);
            }

            free(pts);
        } else if (!strcmp(nm, "choose")) {
            int c;

            for (c = L->v[k].kid; c >= 0; c = L->v[c].next) {
                if (!strcmp(L->v[c].name, "if") ? cond(D, cur, ctx, c) : !strcmp(L->v[c].name, "else")) {
                    build(D, c, ctx, cur);
                    break;
                }
            }
        } else if (cur >= 0) {
            pnode* N = &D->n[cur];

            if (!strcmp(nm, "alg")) {
                N->alg = k;
            } else if (!strcmp(nm, "shape")) {
                N->shape = k;
            } else if (!strcmp(nm, "constrLst") && N->ncon < 16) {
                N->con[N->ncon++] = k;
            } else if (!strcmp(nm, "ruleLst") && N->nrul < 8) {
                N->rul[N->nrul++] = k;
            } else if (!strcmp(nm, "varLst") && N->nvar < 8) {
                N->var[N->nvar++] = k;
            } else if (!strcmp(nm, "presOf")) {
                int* pts = (int*)malloc(sizeof(int) * MAXSEQ), np, i;

                if (!pts) {
                    continue;
                }

                np = nav(D, ctx, k, 0, pts, MAXSEQ);

                if (D->nofs + np > D->cofs) {
                    int nc = (D->nofs + np) * 2 + 64;
                    int* t = (int*)realloc(D->ofs, sizeof(int) * (size_t)nc);

                    if (!t) {
                        free(pts);
                        continue;
                    }

                    D->ofs = t;
                    D->cofs = nc;
                }

                N->of = D->nofs;
                N->nof = 0;

                for (i = 0; i < np; i++) {
                    if (D->p[pts[i]].type != PT_PRES) {
                        D->ofs[D->nofs++] = pts[i];
                        N->nof++;
                    }
                }

                free(pts);
            }
        }
    }

    D->depth--;
}

/* ---- constraints worked out ---- */

static void alg_type(const dgm* D, int n, char* buf, size_t cap) {
    buf[0] = '\0';

    if (D->n[n].alg >= 0) {
        at(D->lo, D->n[n].alg, "type", buf, cap);
    }
}

static void param(const dgm* D, int n, const char* name, char* out, size_t cap, const char* def) {
    char v[32];
    int k;

    snprintf(out, cap, "%s", def);

    for (k = D->n[n].alg >= 0 ? D->lo->v[D->n[n].alg].kid : -1; k >= 0; k = D->lo->v[k].next) {
        if (!strcmp(D->lo->v[k].name, "param") && at(D->lo, k, "type", v, sizeof(v)) && !strcmp(v, name)) {
            at(D->lo, k, "val", out, cap);
            return;
        }
    }
}

static int matches(const dgm* D, int k, const char* fname, const char* ptt) {
    return (!*fname || !strcmp(D->n[k].name, fname)) && (!*ptt || pt_is(D->p[D->n[k].pt].type, ptt));
}

/* the first descendant (or child) of N matching */
static int first_of(const dgm* D, int n, int deep, const char* fname, const char* ptt) {
    int k;

    for (k = D->n[n].kid; k >= 0; k = D->n[k].next) {
        int r;

        if (matches(D, k, fname, ptt)) {
            return k;
        }

        if (deep && (r = first_of(D, k, 1, fname, ptt)) >= 0) {
            return r;
        }
    }

    return -1;
}

static int targets(const dgm* D, int n, int deep, const char* fname, const char* ptt, int* out, int m, int cap) {
    int k;

    for (k = D->n[n].kid; k >= 0; k = D->n[k].next) {
        if (matches(D, k, fname, ptt) && m < cap) {
            out[m++] = k;
        }

        if (deep) {
            m = targets(D, k, 1, fname, ptt, out, m, cap);
        }
    }

    return m;
}

/* a node's value of a constraint as the number constraints give it (mm, points, a ratio) */
static int value_of(const dgm* D, int r, int c, double* out) {
    const pnode* R = &D->n[r];

    if (has(R, c)) {
        *out = R->v[c] / unit_of(c);
        return 1;
    }

    return 0;
}

/* one constraint of N's: 1 when what it refers to is not yet worked out */
static int constr_apply(dgm* D, int n, int k) {
    const xdoc* L = D->lo;
    char type[32], fr[16], fname[64], ptt[32], rtype[32], rfor[16], rname[64], rpt[32], op[16], sv[40];
    double fact = atf(L, k, "fact", 1), num = NAN;
    int c, rc = -1, r = -1, i, nt, tg[MAXPN > 512 ? 512 : MAXPN];
    int dep = 0;

    at(L, k, "type", type, sizeof(type));

    if ((c = ct_index(type)) < 0) {
        return 0;
    }

    at(L, k, "for", fr, sizeof(fr));
    at(L, k, "forName", fname, sizeof(fname));
    at(L, k, "ptType", ptt, sizeof(ptt));
    at(L, k, "refType", rtype, sizeof(rtype));
    at(L, k, "refFor", rfor, sizeof(rfor));
    at(L, k, "refForName", rname, sizeof(rname));
    at(L, k, "refPtType", rpt, sizeof(rpt));
    at(L, k, "op", op, sizeof(op));

    if (!*fr || !strcmp(fr, "self")) {
        tg[0] = n;
        nt = 1;
    } else {
        nt = targets(D, n, !strcmp(fr, "des"), fname, ptt, tg, 0, (int)(sizeof(tg) / sizeof(tg[0])));
    }

    if (*rtype && strcmp(rtype, "none")) {
        if ((rc = ct_index(rtype)) < 0) {
            return 0;
        }

        if (rc == C_CONNDIST && (c == C_BEGPAD || c == C_ENDPAD)) {     /* worked out when it is drawn */
            for (i = 0; i < nt; i++) {
                D->n[tg[i]].padf[c - C_BEGPAD] = fact;
                D->n[tg[i]].padm |= BIT(c - C_BEGPAD);
            }

            return 0;
        }

        r = !*rfor || !strcmp(rfor, "self") ? n : first_of(D, n, !strcmp(rfor, "des"), rname, rpt);

        if (r < 0) {
            return 0;
        }

        if (!value_of(D, r, rc, &num)) {
            return 1;
        }

        num *= fact;
        dep = rc == C_FONT || rc == C_SFONT || (D->n[r].fdep & BIT(rc));
    } else if (at(L, k, "val", sv, sizeof(sv))) {
        num = strtod(sv, NULL);
    }

    for (i = 0; i < nt; i++) {
        pnode* T = &D->n[tg[i]];

        if (!strcmp(op, "gte") || !strcmp(op, "lte")) {
            if (isfinite(num)) {
                if (op[0] == 'g') {
                    T->lo[c] = num * unit_of(c);
                    T->lom |= BIT(c);
                } else {
                    T->hi[c] = num * unit_of(c);
                    T->him |= BIT(c);
                }
            }

            continue;
        }

        if (c == C_FONT && (nt > 1 || !strcmp(op, "equ"))) {   /* those it gives a size: all of a size */
            T->fgrp = k + 1;
        }

        if (!strcmp(op, "equ")) {
            if (c == C_H) {
                T->hgrp = k + 1;
            }

            if (isnan(num)) {
                continue;
            }
        }

        if (isnan(num)) {   /* (no value: it is there, 0 unless something gave it one) */
            if (has(T, c)) {
                continue;
            }

            num = 0;
        }

        if (tg[i] == n && (T->keep & BIT(c))) {
            continue;
        }

        if (c == C_H) {
            T->hb = num * unit_of(c);

            if (T->oh >= 0 && num * unit_of(c) < T->oh) {   /* as tall as its text has it */
                num = T->oh / unit_of(c);
            }
        }

        if ((c == C_FONT || c == C_SFONT) && !strcmp(op, "equ") && r >= 0 && r != tg[i] &&
                (rc == C_FONT || rc == C_SFONT)) {  /* its size to stay that one's: the same group */
            if (!D->n[r].fgrp) {
                D->n[r].fgrp = k + 1;
            }

            T->fgrp = D->n[r].fgrp;
        }

        if (c >= C_TM && c <= C_RM) {
            T->mf[c - C_TM] = rc == C_FONT && r == tg[i] ? fact : -1;
        }

        if (c == C_FONT) {
            T->fmax = num;

            if (T->ofont >= 0 && T->ofont < num) {
                num = T->ofont;
            }

            if (T->fcap >= 0 && T->fcap < num) {
                num = T->fcap;
            }
        }

        T->v[c] = num * unit_of(c);
        T->set |= BIT(c);
        T->fdep = dep ? T->fdep | BIT(c) : T->fdep & ~BIT(c);
    }

    return 0;
}

static void constrain(dgm* D, int n) {
    pnode* N = &D->n[n];
    int pass, i, k;

    for (pass = 0; pass < 4; pass++) {
        int pending = 0;

        for (i = 0; i < N->ncon; i++) {
            for (k = D->lo->v[N->con[i]].kid; k >= 0; k = D->lo->v[k].next) {
                if (!strcmp(D->lo->v[k].name, "constr")) {
                    pending += constr_apply(D, n, k);
                }
            }
        }

        if (!pending) {
            break;
        }
    }

    for (i = 0; i < C_N; i++) {     /* in its bounds */
        if ((N->lom & BIT(i)) && !(N->keep & BIT(i)) && (!has(N, i) || N->v[i] < N->lo[i])) {
            N->v[i] = N->lo[i];
            N->set |= BIT(i);
        }

        if ((N->him & BIT(i)) && !(N->keep & BIT(i)) && has(N, i) && N->v[i] > N->hi[i] && i != C_FONT) {
            N->v[i] = N->hi[i];
        }
    }

    if (N->oh >= 0 && !(N->keep & BIT(C_H)) && (!has(N, C_H) || N->v[C_H] < N->oh)) {
        N->v[C_H] = N->oh;
        N->set |= BIT(C_H);
    }

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        constrain(D, k);
    }
}

/* ---- the algorithms ---- */

static void size(dgm* D, int n);

static void scale_tree(dgm* D, int n, double s) {
    pnode* N = &D->n[n];
    int k;

    N->w *= s;
    N->h *= s;
    N->scl *= s;

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        D->n[k].x *= s;
        D->n[k].y *= s;
        scale_tree(D, k, s);
    }
}

/* a node made a size (KEEP: which of w, h it is made; the other as its own constraints have it then): what is in
   it laid out again for it */
static void resize_keep(dgm* D, int c, double w, double h, unsigned long long keep) {
    pnode* C = &D->n[c];
    double x = C->x, y = C->y, s = C->scl;

    if (keep & BIT(C_W)) {
        C->v[C_W] = w;
        C->set |= BIT(C_W);
    }

    if (keep & BIT(C_H)) {
        C->v[C_H] = h;
        C->set |= BIT(C_H);
    }

    C->keep = keep;
    constrain(D, c);
    size(D, c);
    C->keep = 0;
    C->x = x;
    C->y = y;
    C->scl = s;
}

static void resize(dgm* D, int c, double w, double h) {
    resize_keep(D, c, w, h, BIT(C_W) | BIT(C_H));
}

static int is_alg(const dgm* D, int n, const char* type) {
    char a[32];

    alg_type(D, n, a, sizeof(a));
    return !strcmp(a, type);
}

/* the rules of N's for a constraint, its parents' for it among theirs: their limits to how far they go */
static int rules_of(const dgm* D, int n, int c, double* val, double* fact, int cap) {
    int m = 0, a, i, k, up = 0;

    for (a = n; a >= 0 && m < cap; a = D->n[a].parent, up++) {
        for (i = 0; i < D->n[a].nrul && m < cap; i++) {
            for (k = D->lo->v[D->n[a].rul[i]].kid; k >= 0 && m < cap; k = D->lo->v[k].next) {
                char type[32], fr[16], fname[64], ptt[32];

                if (strcmp(D->lo->v[k].name, "rule") || !at(D->lo, k, "type", type, sizeof(type)) ||
                        ct_index(type) != c) {
                    continue;
                }

                at(D->lo, k, "for", fr, sizeof(fr));
                at(D->lo, k, "forName", fname, sizeof(fname));
                at(D->lo, k, "ptType", ptt, sizeof(ptt));

                if ((up == 0 && (!*fr || !strcmp(fr, "self"))) || (up == 1 && !strcmp(fr, "ch") &&
                        matches(D, n, fname, ptt)) || (up >= 1 && !strcmp(fr, "des") && matches(D, n, fname, ptt))) {
                    val[m] = atf(D->lo, k, "val", NAN);
                    fact[m++] = atf(D->lo, k, "fact", NAN);
                }
            }
        }
    }

    return m;
}

static void size_leaf(dgm* D, int n) {
    pnode* N = &D->n[n];
    int k;

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        size(D, k);
    }

    N->w = has(N, C_W) ? N->v[C_W] : 0;
    N->h = has(N, C_H) ? N->v[C_H] : 0;
}

static int lin_fonts(dgm* D, const int* kids, int nk, int hor, double room, double main);

static void size_composite(dgm* D, int n) {
    pnode* N = &D->n[n];
    double bw = 0, bh = 0;
    int k, kids[256], nk = 0;

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        pnode* K = &D->n[k];
        double cw, ch, x, y;

        if (nk < 256) {
            kids[nk++] = k;
        }

        size(D, k);
        cw = has(K, C_W) ? K->v[C_W] : has(K, C_L) && has(K, C_R) ? K->v[C_R] - K->v[C_L] : K->w;
        ch = has(K, C_H) ? K->v[C_H] : has(K, C_T) && has(K, C_B) ? K->v[C_B] - K->v[C_T] : K->h;

        if (fabs(cw - K->w) > 0.5 || fabs(ch - K->h) > 0.5) {
            resize(D, k, cw, ch);
        }

        x = has(K, C_L) ? K->v[C_L] : has(K, C_R) ? K->v[C_R] - cw : has(K, C_CX) ? K->v[C_CX] - cw / 2 : 0;
        y = has(K, C_T) ? K->v[C_T] : has(K, C_B) ? K->v[C_B] - ch : has(K, C_CY) ? K->v[C_CY] - ch / 2 : 0;
        K->x = x;
        K->y = y;
        bw = fmax(bw, x + cw);
        bh = fmax(bh, y + ch);
    }

    if (has(N, C_H) && bh > N->v[C_H] * 1.001) {    /* taller than it: fonts their sizes follow smaller, if they may */
        lin_fonts(D, kids, nk, 0, N->v[C_H], bh);
    } else if (has(N, C_W) && bw > N->v[C_W] * 1.001) {
        lin_fonts(D, kids, nk, 1, N->v[C_W], bw);
    }

    N->w = has(N, C_W) ? N->v[C_W] : bw;
    N->h = has(N, C_H) ? N->v[C_H] : bh;
    N->bw = bw;
    N->bh = bh;
}

/* a lin's (or a composite's) children whose fonts their rules let be smaller, their sizes following them (or their
   text's): the
   biggest font that lets them fit it, halving between one known to fit and one known not to, a try each time round;
   1 while it is trying (the children not to be made smaller meanwhile) */
static int lin_fonts(dgm* D, const int* kids, int nk, int hor, double room, double main) {
    int c = hor ? C_W : C_H, i, j, settled = 1, any = 0, change = 0;
    double val[8], fact[8];

    for (i = 0; i < nk; i++) {
        pnode* K = &D->n[kids[i]];

        if (K->nof && has(K, C_FONT) && fabs(K->fitf - K->v[C_FONT]) > 0.05) {
            settled = 0;    /* its text not fitted at the size it is now */
        }
    }

    for (i = 0; i < nk; i++) {
        pnode* K = &D->n[kids[i]];
        double f = K->v[C_FONT], lo, nf = -1;

        if (!has(K, C_FONT) || !((K->fdep & BIT(c)) || (c == C_H && K->oh >= 0)) ||
                rules_of(D, kids[i], C_FONT, val, fact, 8) < 1) {
            continue;
        }

        any = 1;
        lo = isfinite(val[0]) ? val[0] : 5;

        if (!settled || D->final || K->tried == D->iter) {     /* (once each time round) */
            continue;
        }

        K->tried = D->iter;

        if (main > room * 1.001) {
            K->fhi = f;
            nf = K->flo > 0 ? (K->flo + K->fhi) / 2 : fmax(f * sqrt(room / main), lo);
        } else if (K->fcap >= 0) {
            K->flo = f;

            if (K->fhi > 0 && K->fhi - f > 0.5) {
                nf = (f + K->fhi) / 2;
            }
        }

        if (nf > 0 && nf >= lo && fabs(nf - f) > 0.1) {
            K->fcap = nf;
            change = 1;
        }
    }

    if (change) {   /* the heights their text took at the sizes they were (not those of the user's sizes) */
        for (j = 0; j < nk; j++) {
            if (!D->n[kids[j]].custt) {
                D->n[kids[j]].oh = -1;
            }
        }

        D->again = 1;
    }

    if (any && !settled && !D->final) {
        D->again = 1;
    }

    return any && !D->final && (change || !settled);
}

static void size_lin(dgm* D, int n) {
    pnode* N = &D->n[n];
    char dir[16], al[16];
    int* kids, nk = 0, k, i, hor, rev, capped = 0, mc, cc;
    double W = has(N, C_W) ? N->v[C_W] : -1, H = has(N, C_H) ? N->v[C_H] : -1, sp = has(N, C_SP) ? N->v[C_SP] : 0;
    double main = 0, cross = 0, room, croom, pos, total, (*cs)[2];

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        nk++;
    }

    kids = (int*)malloc(sizeof(int) * (size_t)(nk + 1));
    cs = (double(*)[2])malloc(sizeof(double[2]) * (size_t)(nk + 1));
    nk = 0;

    if (!kids || !cs) {
        free(kids);
        free(cs);
        D->fail = 1;
        return;
    }

    param(D, n, "linDir", dir, sizeof(dir), "fromL");
    hor = !strcmp(dir, "fromL") || !strcmp(dir, "fromR");
    rev = !strcmp(dir, "fromR") || !strcmp(dir, "fromB");
    param(D, n, hor ? "nodeVertAlign" : "nodeHorzAlign", al, sizeof(al), hor ? "mid" : "ctr");
    mc = hor ? C_W : C_H;
    cc = hor ? C_H : C_W;
    room = hor ? W : H;
    croom = hor ? H : W;

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        pnode* K = &D->n[k];

        cs[nk][0] = cs[nk][1] = 1;

        if (K->pres >= 0) {     /* the user's scaling of it, on the size it would be */
            cs[nk][0] = atf(D->dm, D->p[K->pres].pr, hor ? "custScaleX" : "custScaleY", 100000) / 100000;
            cs[nk][1] = atf(D->dm, D->p[K->pres].pr, hor ? "custScaleY" : "custScaleX", 100000) / 100000;

            if (cs[nk][0] <= 0 || cs[nk][1] <= 0) {
                cs[nk][0] = cs[nk][1] = 1;
            }
        }

        kids[nk++] = k;
        size(D, k);
    }

    for (i = 0; i < nk; i++) {
        main += (hor ? D->n[kids[i]].w : D->n[kids[i]].h) * cs[i][0];
    }

    main += sp * (nk > 1 ? nk - 1 : 0);

    if (room > 0) {
        capped = lin_fonts(D, kids, nk, hor, room, main);
    }

    if (room > 0 && main > room * 1.001 && !capped) {
        /* longer than it is: each made shorter along it, the other way as its own constraints then have it (other
           fonts are tried first, the next time round, while their rules let them be smaller) */
        double s = room / main;

        for (i = 0; i < nk; i++) {
            pnode* K = &D->n[kids[i]];

            resize_keep(D, kids[i], K->w * s, K->h * s, BIT(mc));
        }

        sp *= s;
    }

    for (i = 0, main = 0; i < nk; i++) {
        pnode* K = &D->n[kids[i]];

        if (fabs(cs[i][0] - 1) > 1e-6 || fabs(cs[i][1] - 1) > 1e-6) {
            K->csx = hor ? cs[i][0] : cs[i][1];
            K->csy = hor ? cs[i][1] : cs[i][0];
            resize(D, kids[i], K->w * K->csx, K->h * K->csy);
        }

        if (croom > 0 && (hor ? K->h : K->w) > croom * 1.001) {     /* too wide for it the other way: as its */
            double c = croom;                                           /* children reach, if they are less */

            if (is_alg(D, kids[i], "composite") && (hor ? K->bh : K->bw) > 0 && (hor ? K->bh : K->bw) < croom) {
                c = hor ? K->bh : K->bw;
            }

            resize_keep(D, kids[i], hor ? K->w : c, hor ? c : K->h, BIT(cc));
        }

        main += hor ? K->w : K->h;
        cross = fmax(cross, hor ? K->h : K->w);
    }

    main += sp * (nk > 1 ? nk - 1 : 0);
    total = room > 0 ? room : main;
    pos = (total - main) / 2;

    if (croom <= 0) {
        croom = cross;
    }

    for (i = 0; i < nk; i++) {
        pnode* K = &D->n[kids[i]];
        double m = hor ? K->w : K->h, c = hor ? K->h : K->w;
        double cp = !strcmp(al, "t") || !strcmp(al, "l") ? 0 : !strcmp(al, "b") || !strcmp(al, "r") ? croom - c :
                    (croom - c) / 2;
        double mp = rev ? total - pos - m : pos;

        K->x = hor ? mp : cp;
        K->y = hor ? cp : mp;
        pos += m + sp;
    }

    N->w = W > 0 ? W : hor ? main : cross;
    N->h = H > 0 ? H : hor ? cross : main;
    free(kids);
    free(cs);
}

static void size_snake(dgm* D, int n) {
    pnode* N = &D->n[n];
    char gr[16], flow[16], cont[16], bk[16], off[16], fixed[16];
    int* kids, nk = 0, k, i, best = 1, cols, rows;
    double W = has(N, C_W) ? N->v[C_W] : -1, H = has(N, C_H) ? N->v[C_H] : -1, sp = has(N, C_SP) ? N->v[C_SP] : 0;
    double w0 = 0, h0 = 0, bs = -1, gw, gh, ox, oy;

    param(D, n, "grDir", gr, sizeof(gr), "tL");
    param(D, n, "flowDir", flow, sizeof(flow), "row");
    param(D, n, "contDir", cont, sizeof(cont), "sameDir");
    param(D, n, "bkpt", bk, sizeof(bk), "endCnv");
    param(D, n, "bkPtFixedVal", fixed, sizeof(fixed), "2");
    param(D, n, "off", off, sizeof(off), "ctr");

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        nk++;
    }

    if ((kids = (int*)malloc(sizeof(int) * (size_t)(nk + 1))) == NULL) {
        D->fail = 1;
        return;
    }

    for (k = N->kid, nk = 0; k >= 0; k = D->n[k].next) {
        size(D, k);

        if (!is_alg(D, k, "sp") && !is_alg(D, k, "conn")) {
            kids[nk++] = k;
            w0 = fmax(w0, D->n[k].w);
            h0 = fmax(h0, D->n[k].h);
        } else {
            D->n[k].w = D->n[k].h = 0;
        }
    }

    if (!nk || w0 <= 0 || h0 <= 0) {
        N->w = W > 0 ? W : 0;
        N->h = H > 0 ? H : 0;
        free(kids);
        return;
    }

    if (W <= 0) {
        W = nk * w0 + (nk - 1) * sp;
    }

    if (H <= 0) {
        H = h0;
    }

    for (i = 1; i <= nk; i++) {     /* the number across that makes them biggest */
        int across = i, down = (nk + i - 1) / i;
        double s;

        if (!strcmp(flow, "col")) {
            down = i;
            across = (nk + i - 1) / i;
        }

        s = fmin(W / (across * w0 + (across - 1) * sp), H / (down * h0 + (down - 1) * sp));

        if (s > bs * 1.0001) {
            bs = s;
            best = i;
        }
    }

    if (!strcmp(bk, "fixed")) {
        int across = atoi(fixed) > 0 ? atoi(fixed) : 1, down = (nk + across - 1) / across;

        best = !strcmp(flow, "col") ? down : across;
        bs = fmin(W / (across * w0 + (across - 1) * sp), H / (down * h0 + (down - 1) * sp));
    }

    cols = !strcmp(flow, "col") ? (nk + best - 1) / best : best;
    rows = !strcmp(flow, "col") ? best : (nk + best - 1) / best;
    w0 *= bs;
    h0 *= bs;
    sp *= bs;
    gw = cols * w0 + (cols - 1) * sp;
    gh = rows * h0 + (rows - 1) * sp;
    ox = (W - gw) / 2;
    oy = (H - gh) / 2;

    for (i = 0; i < nk; i++) {
        pnode* K = &D->n[kids[i]];
        int r = !strcmp(flow, "col") ? i % rows : i / cols, c = !strcmp(flow, "col") ? i / rows : i % cols;
        int inrow = !strcmp(flow, "col") ? rows : cols, last = !strcmp(flow, "col") ? nk - (cols - 1) * rows :
                    nk - (rows - 1) * cols;
        double x, y;

        if (!strcmp(cont, "revDir") && (!strcmp(flow, "col") ? c : r) % 2 == 1) {
            if (!strcmp(flow, "col")) {
                r = rows - 1 - r;
            } else {
                c = cols - 1 - c;
            }
        }

        x = ox + c * (w0 + sp);
        y = oy + r * (h0 + sp);

        if (!strcmp(off, "ctr") && (!strcmp(flow, "col") ? c == cols - 1 : r == rows - 1) && last < inrow) {
            if (!strcmp(flow, "col")) {
                y += (inrow - last) * (h0 + sp) / 2;
            } else {
                x += (inrow - last) * (w0 + sp) / 2;
            }
        }

        if (gr[1] == 'R') {
            x = W - x - w0;
        }

        if (gr[0] == 'b') {
            y = H - y - h0;
        }

        resize(D, kids[i], w0, h0);
        K->x = x;
        K->y = y;
    }

    N->w = W;
    N->h = H;
    free(kids);
}

/* the solid boxes of a subtree, its origin at OX, OY: its shapes, not its connectors */
static int solid(const dgm* D, int n, double ox, double oy, double* b, int m, int cap) {
    const pnode* N = &D->n[n];
    int k;

    if (is_alg(D, n, "conn")) {
        return m;
    }

    if ((is_alg(D, n, "tx") || is_alg(D, n, "composite")) && N->w > 0 && m < cap) {
        b[4 * m] = ox;
        b[4 * m + 1] = oy;
        b[4 * m + 2] = ox + N->w;
        b[4 * m + 3] = oy + N->h;
        return m + 1;
    }

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        m = solid(D, k, ox + D->n[k].x, oy + D->n[k].y, b, m, cap);
    }

    return m;
}

#define MAXBOX 2048

/* hierChild: the subtrees of the children side by side (or one under another), as close as their shapes let
   them be */
static void size_hchild(dgm* D, int n) {
    pnode* N = &D->n[n];
    char dir[16], al[16];
    int* items, ni = 0, k, i, hor, nb = 0;
    double sib = has(N, C_SIBSP) ? N->v[C_SIBSP] : 0, cross = 0, maxx = 0, maxy = 0, minx = 0, miny = 0;
    double* placed = (double*)malloc(sizeof(double) * 4 * MAXBOX), *mine = (double*)malloc(sizeof(double) * 4 * MAXBOX);

    param(D, n, "linDir", dir, sizeof(dir), "fromL");
    hor = !strcmp(dir, "fromL") || !strcmp(dir, "fromR");
    param(D, n, "chAlign", al, sizeof(al), hor ? "t" : "l");

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        ni++;
    }

    items = (int*)malloc(sizeof(int) * (size_t)(ni + 1));
    ni = 0;

    for (k = N->kid; k >= 0 && items; k = D->n[k].next) {
        size(D, k);

        if (is_alg(D, k, "conn")) {
            D->n[k].w = D->n[k].h = 0;
        } else {
            items[ni++] = k;
            cross = fmax(cross, hor ? D->n[k].h : D->n[k].w);
        }
    }

    if (!placed || !mine || !items) {
        free(placed);
        free(mine);
        free(items);
        D->fail = 1;
        return;
    }

    for (i = 0; i < ni; i++) {
        pnode* K = &D->n[items[i]];
        double c = hor ? K->h : K->w, cp = !strcmp(al, "b") || !strcmp(al, "r") ? cross - c :
                   !strcmp(al, "ctr") ? (cross - c) / 2 : 0, mp = 0;
        int m = solid(D, items[i], 0, 0, mine, 0, MAXBOX), a, b, hit = 0;

        if (hor) {
            K->y = cp;
        } else {
            K->x = cp;
        }

        for (b = 0; b < m; b++) {   /* placed: where it clears all that is placed, a gap apart */
            mine[4 * b + (hor ? 1 : 0)] += cp;
            mine[4 * b + (hor ? 3 : 2)] += cp;
        }

        for (a = 0; i && a < nb; a++) {
            for (b = 0; b < m; b++) {
                double* A = placed + 4 * a, *B = mine + 4 * b;
                int over = hor ? A[1] < B[3] - 1 && B[1] < A[3] - 1 : A[0] < B[2] - 1 && B[0] < A[2] - 1;

                if (over) {
                    double need = hor ? A[2] + sib - B[0] : A[3] + sib - B[1];

                    if (!hit || need > mp) {
                        mp = need;
                        hit = 1;
                    }
                }
            }
        }

        if (i && !hit) {
            mp = hor ? maxx + sib : maxy + sib;
        }

        if (hor) {
            K->x = mp;
        } else {
            K->y = mp;
        }

        for (b = 0; b < m && nb < MAXBOX; b++, nb++) {
            double* B = mine + 4 * b;

            placed[4 * nb] = B[0] + (hor ? mp : 0);
            placed[4 * nb + 1] = B[1] + (hor ? 0 : mp);
            placed[4 * nb + 2] = B[2] + (hor ? mp : 0);
            placed[4 * nb + 3] = B[3] + (hor ? 0 : mp);
        }

        maxx = fmax(maxx, K->x + K->w);
        maxy = fmax(maxy, K->y + K->h);
        minx = fmin(minx, K->x);
        miny = fmin(miny, K->y);
    }

    for (i = 0; i < ni; i++) {
        pnode* K = &D->n[items[i]];

        K->x -= minx;
        K->y -= miny;

        if (!strcmp(dir, "fromR")) {
            K->x = maxx - minx - K->x - K->w;
        } else if (!strcmp(dir, "fromB")) {
            K->y = maxy - miny - K->y - K->h;
        }
    }

    N->w = maxx - minx;
    N->h = maxy - miny;
    free(placed);
    free(mine);
    free(items);
}

/* the shape of a hierRoot's own: its first child not a hierChild */
static int root_box(const dgm* D, int n) {
    int k;

    for (k = D->n[n].kid; k >= 0; k = D->n[k].next) {
        if (!is_alg(D, k, "hierChild") && !is_alg(D, k, "conn")) {
            return k;
        }
    }

    return -1;
}

/* the middle of a hierChild's first and last children's own shapes, across */
static double kids_mid(const dgm* D, int c, int hor) {
    double a = 0, b = 0;
    int k, first = 1;

    for (k = D->n[c].kid; k >= 0; k = D->n[k].next) {
        const pnode* K = &D->n[k];
        int bx;
        double m;

        if (is_alg(D, k, "conn")) {
            continue;
        }

        bx = is_alg(D, k, "hierRoot") ? root_box(D, k) : -1;
        m = bx >= 0 ? (hor ? K->x + D->n[bx].x + D->n[bx].w / 2 : K->y + D->n[bx].y + D->n[bx].h / 2) :
            (hor ? K->x + K->w / 2 : K->y + K->h / 2);

        if (first) {
            a = m;
            first = 0;
        }

        b = m;
    }

    return first ? (hor ? D->n[c].w : D->n[c].h) / 2 : (a + b) / 2;
}

/* hierRoot: its own shape, its children's subtrees under it (or beside it) */
static void size_hroot(dgm* D, int n) {
    pnode* N = &D->n[n];
    char al[16];
    int box = root_box(D, n), conts[8], nc = 0, k, i, down;
    double sp = has(N, C_SP) ? N->v[C_SP] : 0, off = has(N, C_ALIGNOFF) ? N->v[C_ALIGNOFF] : 0, at;
    double minx = 0, miny = 0, maxx = 0, maxy = 0;

    param(D, n, "hierAlign", al, sizeof(al), "tCtrCh");
    down = al[0] == 't' || al[0] == 'b';

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        size(D, k);

        if (is_alg(D, k, "hierChild") && nc < 8) {
            int j;

            for (j = D->n[k].kid; j >= 0 && is_alg(D, j, "conn"); j = D->n[j].next) {
            }

            if (j >= 0) {   /* those of assistants first */
                if (D->p[D->n[j].pt].type == PT_ASST || (is_alg(D, j, "hierRoot") &&
                        D->p[D->n[j].pt].type == PT_ASST)) {
                    memmove(conts + 1, conts, sizeof(int) * (size_t)nc);
                    conts[0] = k;
                    nc++;
                } else {
                    conts[nc++] = k;
                }
            } else {
                D->n[k].w = D->n[k].h = 0;
            }
        } else if (k != box) {
            D->n[k].w = D->n[k].h = 0;
        }
    }

    if (box < 0) {
        N->w = N->h = 0;
        return;
    }

    D->n[box].x = D->n[box].y = 0;
    at = (down ? D->n[box].h : D->n[box].w) + sp;

    for (i = 0; i < nc; i++) {
        pnode* C = &D->n[conts[i]], *B = &D->n[box];
        double bw = down ? B->w : B->h, cw = down ? C->w : C->h, x;

        if (al[1] == 'L' || al[1] == 'T') {
            x = off * bw;
        } else if (al[1] == 'R' || al[1] == 'B') {
            x = bw - off * bw - cw;
        } else if (!strcmp(al + 1, "CtrDes")) {
            x = (bw - cw) / 2;
        } else {
            x = bw / 2 - kids_mid(D, conts[i], down);
        }

        if (down) {
            C->x = x;
            C->y = at;
        } else {
            C->y = x;
            C->x = at;
        }

        at += (down ? C->h : C->w) + sp;
    }

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        pnode* K = &D->n[k];

        if (k == box || K->w > 0 || K->h > 0) {
            minx = fmin(minx, K->x);
            miny = fmin(miny, K->y);
            maxx = fmax(maxx, K->x + K->w);
            maxy = fmax(maxy, K->y + K->h);
        }
    }

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        pnode* K = &D->n[k];

        K->x -= minx;
        K->y -= miny;

        if (al[0] == 'b') {
            K->y = maxy - miny - K->y - K->h;
        } else if (al[0] == 'r') {
            K->x = maxx - minx - K->x - K->w;
        }
    }

    N->w = maxx - minx;
    N->h = maxy - miny;
}

static void size(dgm* D, int n) {
    char a[32];

    alg_type(D, n, a, sizeof(a));
    D->n[n].x = D->n[n].y = 0;

    if (!strcmp(a, "composite")) {
        size_composite(D, n);
    } else if (!strcmp(a, "lin")) {
        size_lin(D, n);
    } else if (!strcmp(a, "snake")) {
        size_snake(D, n);
    } else if (!strcmp(a, "hierRoot")) {
        size_hroot(D, n);
    } else if (!strcmp(a, "hierChild")) {
        size_hchild(D, n);
    } else if (!*a || !strcmp(a, "tx") || !strcmp(a, "sp") || !strcmp(a, "conn")) {
        size_leaf(D, n);
    } else {
        D->fail = 1;    /* cycle, pyra: not laid out here */
        size_leaf(D, n);
    }
}

static void place_abs(dgm* D, int n, double ox, double oy) {
    pnode* N = &D->n[n];
    int k;

    N->x += ox;
    N->y += oy;

    for (k = N->kid; k >= 0; k = D->n[k].next) {
        place_abs(D, k, N->x, N->y);
    }
}

/* ---- the text fitted ---- */

typedef struct {
    int lvl;
    size_t a, n;                /* its text in the buffer */
    int bullet;
} tpara;

/* the paragraphs a node shows: their text, levels, bullets */
static int paras_of(dgm* D, int n, pd_buf* b, tpara* ps, int cap) {
    const xdoc* m = D->dm;
    pnode* N = &D->n[n];
    char be[8], stb[8];
    int i, np = 0, base = 99, bullets, st;

    var_of(D, n, "bulletEnabled", be, sizeof(be));
    param(D, n, "stBulletLvl", stb, sizeof(stb), "");
    bullets = !strcmp(be, "1") || !strcmp(be, "true");
    st = stb[0] ? atoi(stb) : 1;

    for (i = 0; i < N->nof; i++) {
        int d = D->p[D->ofs[N->of + i]].depth - D->p[N->pt].depth;

        if (d < base) {
            base = d;
        }
    }

    for (i = 0; i < N->nof && np < cap; i++) {
        int q = D->ofs[N->of + i], p, d = D->p[q].depth - D->p[N->pt].depth;

        for (p = D->p[q].t >= 0 ? m->v[D->p[q].t].kid : -1; p >= 0 && np < cap; p = m->v[p].next) {
            int r;

            if (strcmp(m->v[p].name, "p")) {
                continue;
            }

            ps[np].lvl = d - base;
            ps[np].bullet = bullets && d >= st && (d > base || stb[0]);
            ps[np].a = b->n;

            for (r = m->v[p].kid; r >= 0; r = m->v[r].next) {
                int t = xd_kid(m, r, "t");

                if (!strcmp(m->v[r].name, "br")) {
                    pb_putc(b, ' ');
                } else if (t >= 0) {
                    xd_inner(b, m, t);
                }
            }

            ps[np].n = b->n - ps[np].a;
            np++;
        }
    }

    return np;
}

/* the first run size given in a node's text (points), 0 for none */
static double run_size(const dgm* D, int n) {
    const xdoc* m = D->dm;
    const pnode* N = &D->n[n];
    int i, k;

    for (i = 0; i < N->nof; i++) {
        int t = D->p[D->ofs[N->of + i]].t;

        for (k = t; k >= 0 && k < m->nv && (k == t || m->v[k].a < m->v[t].b); k++) {
            if ((!strcmp(m->v[k].name, "rPr") || !strcmp(m->v[k].name, "endParaRPr")) && xd_int(m, k, "sz", 0) > 0) {
                return xd_int(m, k, "sz", 0) / 100.0;
            }
        }
    }

    return 0;
}

static void adj_of(const dgm* D, int n, const char* prst, char* out, size_t cap) {
    int a, k = 0;

    out[0] = '\0';

    for (a = D->n[n].shape >= 0 ? xd_kid(D->lo, D->n[n].shape, "adjLst") : -1, a = a >= 0 ? D->lo->v[a].kid : -1;
            a >= 0; a = D->lo->v[a].next) {
        char nm[40];

        if (!strcmp(D->lo->v[a].name, "adj") &&
                pd_preset_adj_name(prst, (int)atf(D->lo, a, "idx", 1), nm, sizeof(nm))) {
            k += snprintf(out + k, cap - (size_t)k, "%s%s=%.0f", k ? " " : "", nm,
                          atf(D->lo, a, "val", 0) * 100000);

            if ((size_t)k >= cap) {
                break;
            }
        }
    }
}

static void shape_type(const dgm* D, int n, char* type, size_t cap) {
    char hg[8];

    type[0] = '\0';

    if (D->n[n].shape >= 0) {
        at(D->lo, D->n[n].shape, "type", type, cap);

        if (at(D->lo, D->n[n].shape, "hideGeom", hg, sizeof(hg)) && (hg[0] == '1' || !strcmp(hg, "true"))) {
            type[0] = '\0';
        }
    }

    if (!strcmp(type, "none")) {
        type[0] = '\0';
    }
}

/* the height a node's text takes at a size, in a width (EMU); *over when a word is wider than it */
static double text_h(const tpara* ps, int np, const char* s, double width, double f, double k, int* over) {
    double h = 0;
    int i;

    for (i = 0; i < np; i++) {
        double pf = f, ind = ps[i].bullet ? f * 0.9 * EMU_PT : 0;
        int lines = text_lines(s + ps[i].a, ps[i].n, width - ind, pf, k, over);

        h += lines * pf * 1.2 * 0.9 * EMU_PT;

        if (i + 1 < np) {
            h += pf * (ps[i + 1].lvl > 0 ? 0.15 : 0.35) * EMU_PT;
        }
    }

    return h;
}

/* where a node's text goes in its shape: the shape's text rectangle, less its margins at a size */
static void text_room(const dgm* D, int n, double f, double ww, double hh, double* w, double* h) {
    const pnode* N = &D->n[n];
    char type[48], adj[160];
    double l = 0, t = 0, r = ww, b = hh, m[4];
    int i;

    shape_type(D, n, type, sizeof(type));

    if (type[0] && pd_preset_known(type)) {
        adj_of(D, n, type, adj, sizeof(adj));

        if (!pd_preset_text_rect(type, ww, hh, adj, &l, &t, &r, &b)) {
            l = t = 0;
            r = ww;
            b = hh;
        }
    }

    for (i = 0; i < 4; i++) {
        m[i] = N->mf[i] >= 0 ? N->mf[i] * f * EMU_PT : has(N, C_TM + i) ? N->v[C_TM + i] : 0;
    }

    *w = r - l - m[2] - m[3];
    *h = b - t - m[0] - m[1];
}

/* a node's text made to fit it by its rules: the size it is to be, the height it is to be (EMU, actual); at a size
   FIXED (> 0), only the height */
static void fit_node(dgm* D, int n, double fixed, double* fout, double* hout) {
    pnode* N = &D->n[n];
    pd_buf b;
    tpara ps[64];
    int np, i, over = 0;
    double k = face_factor(D->in->font), f, w = N->w / N->csx, h0 = N->h / N->csy, h = h0, rw, rh, need;

    memset(&b, 0, sizeof(b));
    np = is_alg(D, n, "tx") ? paras_of(D, n, &b, ps, 64) : 0;
    f = N->custt ? N->font : N->fmax > 0 ? N->fmax : 65;

    if (!N->custt && (N->him & BIT(C_FONT)) && N->hi[C_FONT] < f) {
        f = N->hi[C_FONT];
    }

    if (!N->custt && N->fcap >= 0 && N->fcap < f) {
        f = N->fcap;
    }

    if (!N->custt && fixed > 0) {
        f = fixed;
    }

    *fout = f;
    *hout = h;

    if (!np || N->w <= 0) {
        pb_free(&b);
        return;
    }

    text_room(D, n, f, w, h, &rw, &rh);
    need = text_h(ps, np, b.p, rw, f, k, &over);

    if (need <= rh && !over) {
        pb_free(&b);
        return;
    }

    {
        /* the rules in order: font sizes made smaller, the shape made taller, till it fits */
        const xdoc* L = D->lo;
        int a, up = 0, done = 0;

        for (a = n; a >= 0 && !done; a = D->n[a].parent, up++) {
            int r, e;

            for (r = 0; r < D->n[a].nrul && !done; r++) {
                for (e = L->v[D->n[a].rul[r]].kid; e >= 0 && !done; e = L->v[e].next) {
                    char type[32], fr[16], fname[64], ptt[32];
                    double rv, rf;
                    int c;

                    if (strcmp(L->v[e].name, "rule") || !at(L, e, "type", type, sizeof(type))) {
                        continue;
                    }

                    at(L, e, "for", fr, sizeof(fr));
                    at(L, e, "forName", fname, sizeof(fname));
                    at(L, e, "ptType", ptt, sizeof(ptt));

                    if (!((up == 0 && (!*fr || !strcmp(fr, "self"))) || (up == 1 && !strcmp(fr, "ch") &&
                            matches(D, n, fname, ptt)) || (up >= 1 && !strcmp(fr, "des") && matches(D, n, fname,
                                    ptt)))) {
                        continue;
                    }

                    c = ct_index(type);
                    rv = atf(L, e, "val", NAN);
                    rf = atf(L, e, "fact", NAN);

                    if (c == C_FONT && !N->custt && fixed <= 0) {
                        double lo = isfinite(rv) ? rv : isfinite(rf) ? f * rf : f, hi = f;

                        if (lo < 1) {
                            lo = 1;
                        }

                        if (lo >= f) {
                            continue;
                        }

                        for (i = 0; i < 24 && hi - lo > 0.05; i++) {     /* the biggest that fits */
                            double mid = (lo + hi) / 2;

                            over = 0;
                            text_room(D, n, mid, w, h, &rw, &rh);

                            if (text_h(ps, np, b.p, rw, mid, k, &over) <= rh && !over) {
                                lo = mid;
                            } else {
                                hi = mid;
                            }
                        }

                        f = floor(lo * 2 + 1e-6) / 2;
                        over = 0;
                        text_room(D, n, f, w, h, &rw, &rh);
                        done = text_h(ps, np, b.p, rw, f, k, &over) <= rh && !over;
                    } else if (c == C_H) {
                        double lim = isinf(rv) ? HUGE_VAL : isfinite(rv) ? rv * EMU_MM * N->scl :
                                     isfinite(rf) ? rf * N->hb * N->scl : h;

                        if ((N->him & BIT(C_H)) && N->hi[C_H] * N->scl < lim) {
                            lim = N->hi[C_H] * N->scl;
                        }

                        over = 0;
                        text_room(D, n, f, w, h, &rw, &rh);
                        need = text_h(ps, np, b.p, rw, f, k, &over) + (h - rh);

                        if (need <= lim) {
                            h = fmax(h, need);
                            done = 1;
                        } else if (lim > h) {
                            h = lim;
                        }
                    }
                }
            }
        }
    }

    *fout = f;
    *hout = h;

    if (getenv("PD_DGM_DEBUG")) {
        text_room(D, n, f, w, h, &rw, &rh);
        over = 0;
        fprintf(stderr, "fit %s %.*s: box %.0f x %.0f (scl %.3f) room %.0f x %.0f font %.1f -> h %.0f need %.0f\n",
                N->name, (int)(b.n < 20 ? b.n : 20), b.p, N->w, N->h, N->scl, rw, rh, f, h,
                text_h(ps, np, b.p, rw, f, k, &over));
    }

    pb_free(&b);
}

/* the text of all fitted: fonts made the same where they are to be, heights asked for; 1 when any changed */
static int fit_text(dgm* D) {
    double* f = (double*)malloc(sizeof(double) * (size_t)D->nn), *h = (double*)malloc(sizeof(double) * (size_t)D->nn);
    double* g2 = (double*)malloc(sizeof(double) * (size_t)D->nn);
    int k, j, changed = 0;

    if (!f || !h || !g2) {
        free(f);
        free(h);
        free(g2);
        return 0;
    }

    for (k = 0; k < D->nn; k++) {
        pnode* N = &D->n[k];
        double rs = run_size(D, k);

        N->custt = N->nof && rs > 0;    /* the sizes its runs have (custT: the user's), not fitted */
        N->fitf = has(N, C_FONT) ? N->v[C_FONT] : -1;

        N->font = N->custt ? rs : N->fmax > 0 ? N->fmax : 65;
        fit_node(D, k, 0, &f[k], &h[k]);
    }

    for (k = 0; k < D->nn; k++) {   /* the same size in each group, the smallest of those with text; heights at it */
        pnode* N = &D->n[k];
        double g = f[k];

        for (j = 0; j < D->nn && N->fgrp; j++) {
            if (D->n[j].fgrp == N->fgrp && D->n[j].nof && f[j] < g) {
                g = f[j];
            }
        }

        g2[k] = g;

        if (!N->custt && g < f[k] - 0.05) {
            fit_node(D, k, g, &f[k], &h[k]);
        }
    }

    for (k = 0; k < D->nn; k++) {   /* the tallest in each group */
        pnode* N = &D->n[k];
        double g = g2[k], hh = h[k] > N->h / N->csy + 1 ? h[k] / N->scl : -1;

        for (j = 0; j < D->nn && N->hgrp; j++) {
            const pnode* J = &D->n[j];

            if (J->hgrp == N->hgrp && h[j] > J->h / J->csy + 1 && h[j] / J->scl > hh) {
                hh = h[j] / J->scl;
            }
        }

        if (!N->custt && (N->nof || N->fgrp) && (N->ofont < 0 || fabs(N->ofont - g) > 0.2)) {
            if (N->ofont >= 0 || g < N->font - 0.2) {
                N->ofont = g;
                changed = 1;
            }
        }

        N->font = N->custt ? N->font : N->ofont >= 0 ? N->ofont : g;

        if (hh > 0 && (N->oh < 0 || fabs(hh - N->oh) > 0.01 * hh)) {    /* asked for, before it is made smaller */
            N->oh = hh;
            changed = 1;
        }
    }

    free(f);
    free(h);
    free(g2);
    return changed;
}

/* ---- the drawing written ---- */

static int style_lbl(const xdoc* d, const char* name) {
    char v[48];
    int k;

    for (k = 0; d && k < d->nv; k++) {
        if (!strcmp(d->v[k].name, "styleLbl") && at(d, k, "name", v, sizeof(v)) && !strcmp(v, name)) {
            return k;
        }
    }

    return -1;
}

/* the colour a style label gives a node from one of its lists (fillClrLst, linClrLst, txFillClrLst), -1 none */
static int colour_pick(const dgm* D, const pnode* N, const char* list) {
    const xdoc* c = D->in->colors;
    int sl = style_lbl(c, N->lbl), l = sl >= 0 ? xd_kid(c, sl, list) : -1, k, m = 0, i;
    char meth[16];

    for (k = l >= 0 ? c->v[l].kid : -1; k >= 0; k = c->v[k].next) {
        m++;
    }

    if (!m) {
        return -1;
    }

    at(c, l, "meth", meth, sizeof(meth));

    if (!strcmp(meth, "span") && m > 1) {
        i = N->scnt > 1 ? (int)floor((double)N->sidx * (m - 1) / (N->scnt - 1) + 0.5) : 0;
    } else {
        i = N->sidx % m;
    }

    for (k = c->v[l].kid; i-- > 0; k = c->v[k].next) {
    }

    return k;
}

/* the index a quick style's style label gives a reference (fillRef, lnRef), and the colour of its fontRef */
static int style_ref(const dgm* D, const pnode* N, const char* ref, int* colour) {
    const xdoc* s = D->in->style;
    int sl = style_lbl(s, N->lbl), r = sl >= 0 ? xd_path(s, sl, "style") : -1;

    r = r >= 0 ? xd_kid(s, r, ref) : -1;

    if (colour) {
        *colour = r >= 0 ? s->v[r].kid : -1;
    }

    return r >= 0 ? (int)xd_int(s, r, "idx", 0) : -1;
}

static void put_colour(pd_buf* o, const xdoc* d, int c, const char* dflt) {
    if (c >= 0) {
        xd_raw(o, d, c);
    } else {
        pb_printf(o, "<a:schemeClr val=\"%s\"/>", dflt);
    }
}

static void put_id(pd_buf* o, const dgm* D, int n) {
    if (D->n[n].pres >= 0) {
        pb_printf(o, "<dsp:sp modelId=\"%s\">", D->p[D->n[n].pres].id);
    } else {
        pb_printf(o, "<dsp:sp modelId=\"{D6000000-0000-0000-0000-%012d}\">", n);
    }

    pb_puts(o, "<dsp:nvSpPr><dsp:cNvPr id=\"0\" name=\"\"/><dsp:cNvSpPr/></dsp:nvSpPr><dsp:spPr>");
}

static void put_xfrm(pd_buf* o, double x, double y, double w, double h, double rot) {
    if (fabs(rot) > 1e-6) {
        pb_printf(o, "<a:xfrm rot=\"%.0f\">", rot * 60000);
    } else {
        pb_puts(o, "<a:xfrm>");
    }

    pb_printf(o, "<a:off x=\"%.0f\" y=\"%.0f\"/><a:ext cx=\"%.0f\" cy=\"%.0f\"/></a:xfrm>", x + 0.0, y + 0.0, w, h);
}

/* a colour element, in the part it is in (n < 0: none) */
typedef struct {
    const xdoc* d;
    int n;
} cref;

/* a node's own shape properties: its presentation point's, else its point's (when they have any) */
static int own_sppr(const dgm* D, const pnode* N) {
    const xdoc* m = D->dm;
    int k;

    if (N->pres >= 0 && (k = xd_kid(m, D->p[N->pres].el, "spPr")) >= 0 && m->v[k].kid >= 0) {
        return k;
    }

    return (k = xd_kid(m, D->p[N->pt].el, "spPr")) >= 0 && m->v[k].kid >= 0 ? k : -1;
}

/* the first of an element's children named one of a list ("solidFill gradFill ..."), -1 none */
static int kid_of(const xdoc* d, int n, const char* names) {
    char w[32];
    int k, i;

    for (k = n >= 0 ? d->v[n].kid : -1; k >= 0; k = d->v[k].next) {
        for (i = 0; word(names, i, w, sizeof(w)); i++) {
            if (!strcmp(d->v[k].name, w)) {
                return k;
            }
        }
    }

    return -1;
}

/* a node's fill and outline: its own (spPr, its point's style), else its style label's; its text colour into *TX */
static void put_fill_line(pd_buf* o, const dgm* D, const pnode* N, int geom, cref* tx) {
    const xdoc* c = D->in->colors, *m = D->dm;
    const dpt* P = &D->p[N->pt];
    int ost = P->pr >= 0 ? xd_kid(m, P->pr, "style") : -1, osp = own_sppr(D, N), k, fi, li;
    cref fc, lc;

    tx->d = D->in->style;
    style_ref(D, N, "fontRef", &tx->n);

    if ((k = colour_pick(D, N, "txFillClrLst")) >= 0) {
        tx->d = c;
        tx->n = k;
    }

    if ((k = xd_kid(m, ost, "fontRef")) >= 0 && m->v[k].kid >= 0) {
        tx->d = m;
        tx->n = m->v[k].kid;
    }

    if (!geom) {
        pb_puts(o, "<a:noFill/><a:ln><a:noFill/></a:ln>");
        return;
    }

    fi = style_ref(D, N, "fillRef", NULL);
    li = style_ref(D, N, "lnRef", NULL);
    fc.d = lc.d = c;
    fc.n = colour_pick(D, N, "fillClrLst");
    lc.n = colour_pick(D, N, "linClrLst");

    if ((k = xd_kid(m, ost, "fillRef")) >= 0) {
        fi = (int)xd_int(m, k, "idx", fi);

        if (m->v[k].kid >= 0) {
            fc.d = m;
            fc.n = m->v[k].kid;
        }
    }

    if ((k = xd_kid(m, ost, "lnRef")) >= 0) {
        li = (int)xd_int(m, k, "idx", li);

        if (m->v[k].kid >= 0) {
            lc.d = m;
            lc.n = m->v[k].kid;
        }
    }

    if ((k = kid_of(m, osp, "noFill solidFill gradFill pattFill")) >= 0) {
        xd_raw(o, m, k);
    } else if (fi != 0 && (fc.n >= 0 || !c)) {
        pb_puts(o, "<a:solidFill>");
        put_colour(o, fc.d, fc.n, "accent1");
        pb_puts(o, "</a:solidFill>");
    } else {
        pb_puts(o, "<a:noFill/>");
    }

    if ((k = xd_kid(m, osp, "ln")) >= 0) {
        xd_raw(o, m, k);
    } else if (li > 0 && li <= 3 && (lc.n >= 0 || !c)) {
        pb_printf(o, "<a:ln w=\"%.0f\"><a:solidFill>", D->in->line_w[li - 1]);
        put_colour(o, lc.d, lc.n, "lt1");
        pb_puts(o, "</a:solidFill></a:ln>");
    } else if (li < 0 && !c) {
        pb_puts(o, "<a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill></a:ln>");
    } else {
        pb_puts(o, "<a:ln><a:noFill/></a:ln>");
    }
}

static void put_style(pd_buf* o, cref tx) {
    pb_puts(o, "<dsp:style><a:lnRef idx=\"0\"><a:scrgbClr r=\"0\" g=\"0\" b=\"0\"/></a:lnRef><a:fillRef idx=\"0\">"
            "<a:scrgbClr r=\"0\" g=\"0\" b=\"0\"/></a:fillRef><a:effectRef idx=\"0\"><a:scrgbClr r=\"0\" g=\"0\" "
            "b=\"0\"/></a:effectRef><a:fontRef idx=\"minor\">");

    if (tx.d && tx.n >= 0) {
        xd_raw(o, tx.d, tx.n);
    }

    pb_puts(o, "</a:fontRef></dsp:style>");
}

/* a run's properties with its size made F (unless it keeps its own) */
static void put_rpr(pd_buf* o, const xdoc* m, int rpr, const char* el, double f, int keep) {
    const char* s = rpr >= 0 ? m->v[rpr].attrs : NULL;
    size_t n = rpr >= 0 ? m->v[rpr].alen : 0, i = 0;
    int had = 0;

    pb_printf(o, "<a:%s", el);

    while (i < n) {     /* its attributes but its size */
        size_t a, e;
        char q;

        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
            i++;
        }

        a = i;

        while (i < n && s[i] != '=') {
            i++;
        }

        if (i + 1 >= n) {
            break;
        }

        e = i;
        q = s[i + 1];
        i += 2;

        while (i < n && s[i] != q) {
            i++;
        }

        i++;

        if (e - a == 2 && !memcmp(s + a, "sz", 2)) {
            if (keep) {
                pb_put(o, " ", 1);
                pb_put(o, s + a, i - a);
                had = 1;
            }

            continue;
        }

        pb_put(o, " ", 1);
        pb_put(o, s + a, i > n ? n - a : i - a);
    }

    if (!had) {
        pb_printf(o, " sz=\"%.0f\"", f * 100);
    }

    if (rpr >= 0 && m->v[rpr].kid >= 0) {
        pb_puts(o, ">");
        xd_inner(o, m, rpr);
        pb_printf(o, "</a:%s>", el);
    } else {
        pb_puts(o, "/>");
    }
}

static void put_text(pd_buf* o, dgm* D, int n, const char* anchor) {
    const xdoc* m = D->dm;
    pnode* N = &D->n[n];
    char be[8], stb[8], algn[16];
    int i, base = 99, bullets, st, any = 0;
    double f = N->font > 0 ? N->font : 18, mg[4];

    var_of(D, n, "bulletEnabled", be, sizeof(be));
    param(D, n, "stBulletLvl", stb, sizeof(stb), "");
    param(D, n, "parTxLTRAlign", algn, sizeof(algn), "ctr");
    bullets = !strcmp(be, "1") || !strcmp(be, "true");
    st = stb[0] ? atoi(stb) : 1;

    for (i = 0; i < 4; i++) {
        mg[i] = N->mf[i] >= 0 ? N->mf[i] * f * EMU_PT : has(N, C_TM + i) ? N->v[C_TM + i] : 0;
    }

    pb_printf(o, "<dsp:txBody><a:bodyPr spcFirstLastPara=\"0\" vert=\"horz\" wrap=\"square\" lIns=\"%.0f\" "
              "tIns=\"%.0f\" rIns=\"%.0f\" bIns=\"%.0f\" numCol=\"1\" spcCol=\"1270\" anchor=\"%s\" anchorCtr=\"0\">"
              "<a:noAutofit/></a:bodyPr><a:lstStyle/>", mg[2], mg[0], mg[3], mg[1], anchor);

    for (i = 0; i < N->nof; i++) {
        int d = D->p[D->ofs[N->of + i]].depth - D->p[N->pt].depth;

        if (d < base) {
            base = d;
        }
    }

    for (i = 0; i < N->nof; i++) {
        int q = D->ofs[N->of + i], p, d = D->p[q].depth - D->p[N->pt].depth;
        int bul = bullets && d >= st && (d > base || stb[0]), lvl = d - base;

        for (p = D->p[q].t >= 0 ? m->v[D->p[q].t].kid : -1; p >= 0; p = m->v[p].next) {
            int r, end = -1;

            if (strcmp(m->v[p].name, "p")) {
                continue;
            }

            any = 1;

            if (bul) {
                pb_printf(o, "<a:p><a:pPr marL=\"%.0f\" lvl=\"%d\" indent=\"%.0f\" algn=\"l\" defTabSz=\"%.0f\">"
                          "<a:lnSpc><a:spcPct val=\"90000\"/></a:lnSpc><a:spcBef><a:spcPct val=\"0\"/></a:spcBef>"
                          "<a:spcAft><a:spcPct val=\"15000\"/></a:spcAft><a:buChar char=\"&#8226;\"/></a:pPr>",
                          f * 0.9 * EMU_PT * (lvl > 0 ? lvl : 1), lvl, -f * 0.9 * EMU_PT, f * 4 * EMU_PT);
            } else {
                pb_printf(o, "<a:p><a:pPr lvl=\"%d\" algn=\"%s\" defTabSz=\"%.0f\"><a:lnSpc><a:spcPct val=\"90000\"/>"
                          "</a:lnSpc><a:spcBef><a:spcPct val=\"0\"/></a:spcBef><a:spcAft><a:spcPct val=\"35000\"/>"
                          "</a:spcAft><a:buNone/></a:pPr>", lvl, algn, f * 4 * EMU_PT);
            }

            for (r = m->v[p].kid; r >= 0; r = m->v[r].next) {
                const char* rn = m->v[r].name;

                if (!strcmp(rn, "r") || !strcmp(rn, "fld")) {
                    int t = xd_kid(m, r, "t");

                    pb_printf(o, "<a:%s>", rn);
                    put_rpr(o, m, xd_kid(m, r, "rPr"), "rPr", f, N->custt);

                    if (t >= 0) {
                        xd_raw(o, m, t);
                    }

                    pb_printf(o, "</a:%s>", rn);
                } else if (!strcmp(rn, "br")) {
                    pb_puts(o, "<a:br>");
                    put_rpr(o, m, xd_kid(m, r, "rPr"), "rPr", f, N->custt);
                    pb_puts(o, "</a:br>");
                } else if (!strcmp(rn, "endParaRPr")) {
                    end = r;
                }
            }

            put_rpr(o, m, end, "endParaRPr", f, N->custt);
            pb_puts(o, "</a:p>");
        }
    }

    if (!any) {
        pb_printf(o, "<a:p><a:pPr algn=\"%s\"/><a:endParaRPr lang=\"en-US\" sz=\"%.0f\"/></a:p>", algn, f * 100);
    }

    pb_puts(o, "</dsp:txBody>");
}

static int has_text(const dgm* D, int n) {
    const xdoc* m = D->dm;
    int i, k;

    for (i = 0; i < D->n[n].nof; i++) {
        int t = D->p[D->ofs[D->n[n].of + i]].t;

        for (k = t; k >= 0 && k < m->nv && (k == t || m->v[k].a < m->v[t].b); k++) {
            if (!strcmp(m->v[k].name, "t") && k != t && m->v[k].ib > m->v[k].ia) {
                return 1;
            }
        }
    }

    return 0;
}

/* the shape of a point to connect to: the one named, else its first with a shape */
static int shape_of(const dgm* D, int pt, const char* name) {
    int k, first = -1;

    for (k = 0; k < D->nn; k++) {
        char type[48];

        if (D->n[k].pt != pt || is_alg(D, k, "conn")) {
            continue;
        }

        if (*name) {
            if (!strcmp(D->n[k].name, name)) {
                return k;
            }

            continue;
        }

        shape_type(D, k, type, sizeof(type));

        if (type[0] && first < 0) {
            first = k;
        }
    }

    return first;
}

/* a point of a box by its name (bCtr, midL, ...) */
static void box_pt(const pnode* B, const char* w, double* x, double* y) {
    *x = B->x + B->w / 2;
    *y = B->y + B->h / 2;

    if (!strcmp(w, "tCtr") || !strcmp(w, "tL") || !strcmp(w, "tR")) {
        *y = B->y;
    } else if (!strcmp(w, "bCtr") || !strcmp(w, "bL") || !strcmp(w, "bR")) {
        *y = B->y + B->h;
    }

    if (!strcmp(w, "midL") || !strcmp(w, "tL") || !strcmp(w, "bL")) {
        *x = B->x;
    } else if (!strcmp(w, "midR") || !strcmp(w, "tR") || !strcmp(w, "bR")) {
        *x = B->x + B->w;
    }
}

/* of a list of a box's points, the one nearest another */
static void near_pt(const pnode* B, const char* list, double tx, double ty, double* x, double* y) {
    char w[16];
    int k;
    double best = HUGE_VAL;

    for (k = 0; word(list, k, w, sizeof(w)); k++) {
        double px, py, d;

        if (!strcmp(w, "auto") || !strcmp(w, "radial")) {
            double cx = B->x + B->w / 2, cy = B->y + B->h / 2, dx = tx - cx, dy = ty - cy;

            if (fabs(dx) * B->h > fabs(dy) * B->w) {
                snprintf(w, sizeof(w), dx > 0 ? "midR" : "midL");
            } else {
                snprintf(w, sizeof(w), dy > 0 ? "bCtr" : "tCtr");
            }
        }

        box_pt(B, w, &px, &py);
        d = (px - tx) * (px - tx) + (py - ty) * (py - ty);

        if (d < best) {
            best = d;
            *x = px;
            *y = py;
        }
    }

    if (best == HUGE_VAL) {
        box_pt(B, "ctr", x, y);
    }
}

/* how far from a box's middle its edge is, going along (ux, uy) */
static double edge_dist(const pnode* B, double ux, double uy) {
    double tx = fabs(ux) > 1e-9 ? B->w / 2 / fabs(ux) : HUGE_VAL, ty = fabs(uy) > 1e-9 ? B->h / 2 / fabs(uy) : HUGE_VAL;

    return fmin(tx, ty);
}

/* a connector: an arrow between the shapes of the points it is between, or a line (bent, as its routing says) */
static void put_conn(pd_buf* o, dgm* D, int n) {
    pnode* N = &D->n[n];
    const dpt* T = &D->p[N->pt];
    char dim[8], sn[64], dn[64], bp[32], ep[32], rout[16], es[16], bs[16], bend[16];
    int src = -1, dst = -1, s, d;
    cref tx;

    param(D, n, "dim", dim, sizeof(dim), "2D");
    param(D, n, "srcNode", sn, sizeof(sn), "");
    param(D, n, "dstNode", dn, sizeof(dn), "");

    if (T->type == PT_SIB && T->owner >= 0) {
        src = T->owner;
        dst = D->p[T->owner].next;
    } else if (T->type == PT_PAR && T->owner >= 0) {
        src = D->p[T->owner].parent;
        dst = T->owner;
    }

    if (src < 0 || dst < 0 || (s = shape_of(D, src, sn)) < 0 || (d = shape_of(D, dst, dn)) < 0) {
        return;
    }

    if (!strcmp(dim, "1D")) {
        double x0, y0, x1, y1, pts[8], minx, miny, maxx, maxy, bd;
        int np = 0, i, vb, ve;
        const pnode* S = &D->n[s], *E = &D->n[d];

        param(D, n, "begPts", bp, sizeof(bp), "auto");
        param(D, n, "endPts", ep, sizeof(ep), "auto");
        param(D, n, "connRout", rout, sizeof(rout), "stra");
        param(D, n, "begSty", bs, sizeof(bs), "noArr");
        param(D, n, "endSty", es, sizeof(es), "arr");
        param(D, n, "bendPt", bend, sizeof(bend), "beg");
        near_pt(S, bp, E->x + E->w / 2, E->y + E->h / 2, &x0, &y0);
        near_pt(E, ep, x0, y0, &x1, &y1);
        vb = fabs(y0 - S->y) < 1 || fabs(y0 - S->y - S->h) < 1;
        ve = fabs(y1 - E->y) < 1 || fabs(y1 - E->y - E->h) < 1;
        pts[np++] = x0;
        pts[np++] = y0;

        if (strcmp(rout, "stra") && fabs(x1 - x0) > 1 && fabs(y1 - y0) > 1) {
            if (vb && ve) {     /* down, across, down: the bend where it is asked for */
                bd = has(N, C_BENDDIST) ? N->v[C_BENDDIST] * N->scl : (y1 - y0) / 2;

                if (fabs(bd) > fabs(y1 - y0)) {
                    bd = (y1 - y0) / 2;
                }

                bd = y1 > y0 ? fabs(bd) : -fabs(bd);
                pts[np++] = x0;
                pts[np++] = y0 + bd;
                pts[np++] = x1;
                pts[np++] = y0 + bd;
            } else if (vb) {
                pts[np++] = x0;
                pts[np++] = y1;
            } else if (ve) {
                pts[np++] = x1;
                pts[np++] = y0;
            } else {
                pts[np++] = (x0 + x1) / 2;
                pts[np++] = y0;
                pts[np++] = (x0 + x1) / 2;
                pts[np++] = y1;
            }
        }

        pts[np++] = x1;
        pts[np++] = y1;
        minx = maxx = pts[0];
        miny = maxy = pts[1];

        for (i = 2; i < np; i += 2) {
            minx = fmin(minx, pts[i]);
            maxx = fmax(maxx, pts[i]);
            miny = fmin(miny, pts[i + 1]);
            maxy = fmax(maxy, pts[i + 1]);
        }

        put_id(o, D, n);
        put_xfrm(o, minx, miny, fmax(maxx - minx, 1), fmax(maxy - miny, 1), 0);
        pb_printf(o, "<a:custGeom><a:avLst/><a:gdLst/><a:ahLst/><a:cxnLst/><a:rect l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>"
                  "<a:pathLst><a:path w=\"%.0f\" h=\"%.0f\" fill=\"none\">", fmax(maxx - minx, 1), fmax(maxy - miny, 1));

        for (i = 0; i < np; i += 2) {
            pb_printf(o, "<a:%s><a:pt x=\"%.0f\" y=\"%.0f\"/></a:%s>", i ? "lnTo" : "moveTo", pts[i] - minx,
                      pts[i + 1] - miny, i ? "lnTo" : "moveTo");
        }

        pb_puts(o, "</a:path></a:pathLst></a:custGeom><a:noFill/>");
        {
            int li = style_ref(D, N, "lnRef", NULL), lc = colour_pick(D, N, "linClrLst");

            pb_printf(o, "<a:ln w=\"%.0f\"><a:solidFill>", li > 0 && li <= 3 ? D->in->line_w[li - 1] : 12700.0);
            put_colour(o, D->in->colors, lc, "accent1");
            pb_puts(o, "</a:solidFill>");

            if (strcmp(bs, "noArr")) {
                pb_puts(o, "<a:headEnd type=\"triangle\"/>");
            }

            if (strcmp(es, "noArr")) {
                pb_puts(o, "<a:tailEnd type=\"triangle\"/>");
            }

            pb_puts(o, "</a:ln>");
        }
        tx.d = D->in->style;
        style_ref(D, N, "fontRef", &tx.n);
        pb_puts(o, "</dsp:spPr>");
        put_style(o, tx);
        pb_puts(o, "</dsp:sp>");
    } else {    /* an arrow from the one to the other, in the gap between them */
        const pnode* S = &D->n[s], *E = &D->n[d];
        double sx = S->x + S->w / 2, sy = S->y + S->h / 2, ex = E->x + E->w / 2, ey = E->y + E->h / 2;
        double dx = ex - sx, dy = ey - sy, len = sqrt(dx * dx + dy * dy), ux, uy, gap, bp0, ep0, alen, thick, cx, cy;

        if (len < 1) {
            return;
        }

        ux = dx / len;
        uy = dy / len;
        gap = len - edge_dist(S, ux, uy) - edge_dist(E, ux, uy);
        bp0 = (N->padm & 1) ? N->padf[0] * gap : has(N, C_BEGPAD) ? N->v[C_BEGPAD] * N->scl : 0;
        ep0 = (N->padm & 2) ? N->padf[1] * gap : has(N, C_ENDPAD) ? N->v[C_ENDPAD] * N->scl : 0;
        alen = gap - bp0 - ep0;
        thick = fabs(ux) >= fabs(uy) ? N->h : N->w;

        if (alen <= 1 || thick <= 1) {
            return;
        }

        cx = sx + ux * (edge_dist(S, ux, uy) + bp0 + alen / 2);
        cy = sy + uy * (edge_dist(S, ux, uy) + bp0 + alen / 2);
        put_id(o, D, n);
        put_xfrm(o, cx - alen / 2, cy - thick / 2, alen, thick, atan2(uy, ux) * 180 / 3.14159265358979323846);
        pb_puts(o, "<a:prstGeom prst=\"rightArrow\"><a:avLst><a:gd name=\"adj1\" fmla=\"val 60000\"/><a:gd "
                "name=\"adj2\" fmla=\"val 50000\"/></a:avLst></a:prstGeom>");
        put_fill_line(o, D, N, 1, &tx);
        pb_puts(o, "</dsp:spPr>");
        put_style(o, tx);
        pb_puts(o, "</dsp:sp>");
    }
}

static void put_node(pd_buf* o, dgm* D, int n) {
    pnode* N = &D->n[n];
    char type[48], rot[32], adj[160], anchor[8] = "ctr";
    int text = N->nof && is_alg(D, n, "tx") && has_text(D, n), i;
    cref tx;

    if (is_alg(D, n, "conn")) {
        if (N->shape >= 0 && at(D->lo, N->shape, "type", type, sizeof(type)) && !strcmp(type, "conn")) {
            put_conn(o, D, n);
        }

        return;
    }

    shape_type(D, n, type, sizeof(type));

    if ((!type[0] && !text) || N->w < 1 || N->h < 1) {
        return;
    }

    if (!type[0] || !pd_preset_known(type)) {
        snprintf(type, sizeof(type), "rect");
    }

    put_id(o, D, n);
    put_xfrm(o, N->x, N->y, N->w, N->h, N->shape >= 0 && at(D->lo, N->shape, "rot", rot, sizeof(rot)) ?
             strtod(rot, NULL) : 0);
    pb_printf(o, "<a:prstGeom prst=\"%s\"><a:avLst>", type);
    adj_of(D, n, type, adj, sizeof(adj));

    for (i = 0; adj[i];) {   /* "adj1=5000 adj2=200" as guides */
        char nm[40];
        double v;
        int used = 0;

        if (sscanf(adj + i, "%39[^=]=%lf%n", nm, &v, &used) < 2 || used <= 0) {
            break;
        }

        pb_printf(o, "<a:gd name=\"%s\" fmla=\"val %.0f\"/>", nm, v);
        i += used;

        while (adj[i] == ' ') {
            i++;
        }
    }

    pb_puts(o, "</a:avLst></a:prstGeom>");
    shape_type(D, n, adj, sizeof(adj));
    put_fill_line(o, D, N, adj[0] != '\0', &tx);
    pb_puts(o, "</dsp:spPr>");
    put_style(o, tx);

    if (text) {
        tpara ps[64];
        pd_buf b;
        int np, j;

        memset(&b, 0, sizeof(b));
        np = paras_of(D, n, &b, ps, 64);

        for (j = 0; j < np; j++) {
            if (ps[j].bullet) {
                snprintf(anchor, sizeof(anchor), "t");
            }
        }

        param(D, n, "txAnchorVert", rot, sizeof(rot), "");

        if (!strcmp(rot, "t") || !strcmp(rot, "b")) {
            anchor[0] = rot[0];
            anchor[1] = '\0';
        } else if (!strcmp(rot, "mid")) {
            snprintf(anchor, sizeof(anchor), "ctr");
        }

        pb_free(&b);
        put_text(o, D, n, anchor);
    }

    pb_puts(o, "</dsp:sp>");
}

/* the style labels, and which of those with each a node is: from its presentation point, else worked out */
static void style_labels(dgm* D) {
    int k, j;

    for (k = 0; k < D->nn; k++) {
        pnode* N = &D->n[k];
        const xdoc* m = D->dm;

        if (N->pres >= 0) {
            char v[48];

            if (at(m, D->p[N->pres].pr, "presStyleLbl", v, sizeof(v))) {
                snprintf(N->lbl, sizeof(N->lbl), "%s", v);
            }
        }

        if (!N->lbl[0]) {
            int t = D->p[N->pt].type;
            char dim[8];

            param(D, k, "dim", dim, sizeof(dim), "2D");

            if (N->parent >= 0 && D->n[N->parent].lbl[0] && !is_alg(D, k, "conn")) {
                snprintf(N->lbl, sizeof(N->lbl), "%s", D->n[N->parent].lbl);
            } else {
                snprintf(N->lbl, sizeof(N->lbl), "%s", t == PT_SIB ? (strcmp(dim, "1D") ? "sibTrans2D1" :
                         "sibTrans1D1") : t == PT_PAR ? "parChTrans1D1" : "node1");
            }
        }
    }

    for (k = 0; k < D->nn; k++) {
        pnode* N = &D->n[k];

        if (N->pres >= 0 && xd_int(D->dm, D->p[N->pres].pr, "presStyleCnt", 0) > 0) {
            N->sidx = (int)xd_int(D->dm, D->p[N->pres].pr, "presStyleIdx", 0);
            N->scnt = (int)xd_int(D->dm, D->p[N->pres].pr, "presStyleCnt", 0);
            continue;
        }

        for (j = 0, N->sidx = N->scnt = 0; j < D->nn; j++) {
            if (!strcmp(D->n[j].lbl, N->lbl) && D->n[j].ln == N->ln) {
                if (j < k) {
                    N->sidx++;
                }

                N->scnt++;
            }
        }
    }
}

static void reset(dgm* D) {
    int k, i;

    for (k = 0; k < D->nn; k++) {
        pnode* N = &D->n[k];

        N->set = N->lom = N->him = N->fdep = N->padm = 0;
        N->keep = N->fgrp = N->hgrp = 0;
        N->fmax = N->hb = 0;
        N->scl = N->csx = N->csy = 1;
        N->x = N->y = N->w = N->h = 0;

        for (i = 0; i < 4; i++) {
            N->mf[i] = -1;
        }
    }
}

int pd_dgm_layout(const pd_dgm_in* in, pd_buf* o) {
    dgm D;
    int k, iter, ok = 0, *order = NULL, *z = NULL;

    if (!in || !in->data || !in->layout || in->cx <= 0 || in->cy <= 0) {
        return 0;
    }

    memset(&D, 0, sizeof(D));
    D.in = in;
    D.dm = in->data;
    D.lo = in->layout;
    D.doc = D.root = -1;
    D.p = (dpt*)calloc(MAXPT, sizeof(dpt));
    D.n = (pnode*)calloc(MAXPN, sizeof(pnode));
    D.t1 = (int*)malloc(sizeof(int) * MAXSEQ);
    D.t2 = (int*)malloc(sizeof(int) * MAXSEQ);
    D.t3 = (int*)malloc(sizeof(int) * MAXSEQ);

    if (!D.p || !D.n || !D.t1 || !D.t2 || !D.t3 || !data_load(&D)) {
        goto done;
    }

    k = xd_find(D.lo, "layoutDef");

    if (k < 0) {
        goto done;
    }

    build(&D, k, D.doc, -1);

    if (D.fail || D.root < 0) {
        goto done;
    }

    style_labels(&D);

    for (iter = 0; iter < 16; iter++) {  /* laid out, the text fitted, again while that changes what it asks */
        pnode* R = &D.n[D.root];
        char a[32];
        double s;

        reset(&D);
        D.again = 0;
        D.final = iter == 15;
        D.iter = iter + 1;
        R->v[C_W] = in->cx;
        R->v[C_H] = in->cy;
        R->set = BIT(C_W) | BIT(C_H);
        constrain(&D, D.root);
        size(&D, D.root);

        if (D.fail) {
            goto done;
        }

        alg_type(&D, D.root, a, sizeof(a));
        s = fmin(in->cx / fmax(R->w, 1), in->cy / fmax(R->h, 1));

        if ((!strcmp(a, "hierChild") || !strcmp(a, "hierRoot")) || s < 1) {    /* the whole made to fit the frame */
            for (k = R->kid; k >= 0; k = D.n[k].next) {
                D.n[k].x *= s;
                D.n[k].y *= s;
                scale_tree(&D, k, s);
            }

            R->w *= s;
            R->h *= s;
        }

        for (k = R->kid; k >= 0; k = D.n[k].next) {
            D.n[k].x += (in->cx - R->w) / 2;
            D.n[k].y += (in->cy - R->h) / 2;
        }

        R->x = R->y = 0;
        R->w = in->cx;
        R->h = in->cy;

        if (!fit_text(&D) && !D.again) {
            break;
        }
    }

    place_abs(&D, D.root, 0, 0);

    if (getenv("PD_DGM_DEBUG")) {
        for (k = 0; k < D.nn; k++) {
            char a[32];
            int d = 0, j;

            for (j = D.n[k].parent; j >= 0; j = D.n[j].parent) {
                d++;
            }

            alg_type(&D, k, a, sizeof(a));
            fprintf(stderr, "%*s%s [%s] %.0f %.0f %.0f %.0f font %.1f req w %.0f h %.0f oh %.0f\n", 2 * d, "",
                    D.n[k].name, a, D.n[k].x, D.n[k].y, D.n[k].w, D.n[k].h, D.n[k].font,
                    has(&D.n[k], C_W) ? D.n[k].v[C_W] : -1, has(&D.n[k], C_H) ? D.n[k].v[C_H] : -1, D.n[k].oh);
        }
    }
    order = (int*)malloc(sizeof(int) * (size_t)D.nn);
    z = (int*)malloc(sizeof(int) * (size_t)D.nn);

    if (!order || !z) {
        goto done;
    }

    for (k = 0; k < D.nn; k++) {    /* in order, those put back (zOrderOff) first */
        int j = k, zo = D.n[k].shape >= 0 ? (int)atf(D.lo, D.n[k].shape, "zOrderOff", 0) : 0;

        while (j > 0 && z[j - 1] > zo) {
            order[j] = order[j - 1];
            z[j] = z[j - 1];
            j--;
        }

        order[j] = k;
        z[j] = zo;
    }

    pb_puts(o, "<dsp:drawing xmlns:dgm=\"http://schemas.openxmlformats.org/drawingml/2006/diagram\" xmlns:dsp=\""
            "http://schemas.microsoft.com/office/drawing/2008/diagram\" xmlns:a=\"http://schemas.openxmlformats.org/"
            "drawingml/2006/main\"><dsp:spTree><dsp:nvGrpSpPr><dsp:cNvPr id=\"0\" name=\"\"/><dsp:cNvGrpSpPr/>"
            "</dsp:nvGrpSpPr><dsp:grpSpPr/>");

    for (k = 0; k < D.nn; k++) {
        put_node(o, &D, order[k]);
    }

    pb_puts(o, "</dsp:spTree></dsp:drawing>");
    ok = 1;
done:
    free(order);
    free(z);
    free(D.p);
    free(D.n);
    free(D.ofs);
    free(D.t1);
    free(D.t2);
    free(D.t3);
    return ok;
}
