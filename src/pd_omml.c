/*
 * pd_omml.c - Word's equations (Office MathML, OMML) to and from LaTeX
 *
 * Parade typesets equations from LaTeX. Word keeps them as OMML: m:oMath
 * with fractions (m:f), scripts (m:sSup, m:sSub, m:sSubSup, m:sPre),
 * radicals, n-ary operators, delimiters, accents, bars, functions, limits,
 * group characters, matrices and equation arrays, and runs of text whose
 * characters are Unicode math symbols. Reading turns that tree into LaTeX
 * the math layout knows; writing turns the LaTeX Parade holds back into
 * OMML for the same constructs, and plain runs for the rest.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

/* ------------------------------------------------------------------ */
/* symbols                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t cp;
    const char* tex;
} msym;

static const msym SYMS[] = {
    { 0x03B1, "\\alpha" }, { 0x03B2, "\\beta" }, { 0x03B3, "\\gamma" }, { 0x03B4, "\\delta" },
    { 0x03B5, "\\epsilon" }, { 0x03F5, "\\epsilon" }, { 0x03B6, "\\zeta" }, { 0x03B7, "\\eta" },
    { 0x03B8, "\\theta" }, { 0x03D1, "\\vartheta" }, { 0x03B9, "\\iota" }, { 0x03BA, "\\kappa" },
    { 0x03BB, "\\lambda" }, { 0x03BC, "\\mu" }, { 0x00B5, "\\mu" }, { 0x03BD, "\\nu" }, { 0x03BE, "\\xi" },
    { 0x03C0, "\\pi" }, { 0x03C1, "\\rho" }, { 0x03C3, "\\sigma" }, { 0x03C2, "\\varsigma" }, { 0x03C4, "\\tau" },
    { 0x03C5, "\\upsilon" }, { 0x03C6, "\\phi" }, { 0x03D5, "\\phi" }, { 0x03C7, "\\chi" }, { 0x03C8, "\\psi" },
    { 0x03C9, "\\omega" }, { 0x0393, "\\Gamma" }, { 0x0394, "\\Delta" }, { 0x0398, "\\Theta" },
    { 0x039B, "\\Lambda" }, { 0x2126, "\\Omega" }, { 0x039E, "\\Xi" }, { 0x03A0, "\\Pi" }, { 0x03A3, "\\Sigma" }, { 0x03A5, "\\Upsilon" },
    { 0x03A6, "\\Phi" }, { 0x03A8, "\\Psi" }, { 0x03A9, "\\Omega" },
    { 0x221E, "\\infty" }, { 0x2264, "\\le" }, { 0x2265, "\\ge" }, { 0x2260, "\\ne" }, { 0x00B1, "\\pm" },
    { 0x2213, "\\mp" }, { 0x00D7, "\\times" }, { 0x00F7, "\\div" }, { 0x22C5, "\\cdot" }, { 0x00B7, "\\cdot" },
    { 0x2219, "\\cdot" }, { 0x2192, "\\to" }, { 0x2190, "\\leftarrow" }, { 0x21D2, "\\Rightarrow" },
    { 0x21D0, "\\Leftarrow" }, { 0x2194, "\\leftrightarrow" }, { 0x21D4, "\\iff" }, { 0x2202, "\\partial" },
    { 0x2207, "\\nabla" }, { 0x2208, "\\in" }, { 0x2209, "\\notin" }, { 0x220B, "\\ni" }, { 0x2248, "\\approx" },
    { 0x2261, "\\equiv" }, { 0x223C, "\\sim" }, { 0x2243, "\\simeq" }, { 0x2245, "\\cong" }, { 0x221D, "\\propto" },
    { 0x2026, "\\ldots" }, { 0x22EF, "\\cdots" }, { 0x22EE, "\\vdots" }, { 0x22F1, "\\ddots" },
    { 0x2200, "\\forall" }, { 0x2203, "\\exists" }, { 0x2205, "\\emptyset" }, { 0x2229, "\\cap" },
    { 0x222A, "\\cup" }, { 0x2282, "\\subset" }, { 0x2283, "\\supset" }, { 0x2286, "\\subseteq" },
    { 0x2287, "\\supseteq" }, { 0x2227, "\\wedge" }, { 0x2228, "\\vee" }, { 0x00AC, "\\neg" },
    { 0x226A, "\\ll" }, { 0x226B, "\\gg" }, { 0x2295, "\\oplus" }, { 0x2297, "\\otimes" }, { 0x2218, "\\circ" },
    { 0x2032, "\\prime" }, { 0x2033, "\\prime\\prime" }, { 0x22A5, "\\perp" }, { 0x2225, "\\parallel" },
    { 0x2016, "\\Vert" }, { 0x27E8, "\\langle" }, { 0x27E9, "\\rangle" }, { 0x2329, "\\langle" },
    { 0x232A, "\\rangle" }, { 0x230A, "\\lfloor" }, { 0x230B, "\\rfloor" }, { 0x2308, "\\lceil" },
    { 0x2309, "\\rceil" }, { 0x2211, "\\sum" }, { 0x220F, "\\prod" }, { 0x222B, "\\int" }, { 0x222C, "\\iint" },
    { 0x222D, "\\iiint" }, { 0x222E, "\\oint" }, { 0x22C3, "\\bigcup" }, { 0x22C2, "\\bigcap" },
    { 0x2210, "\\coprod" }, { 0x210F, "\\hbar" }, { 0x2113, "\\ell" }, { 0x211C, "\\Re" }, { 0x2111, "\\Im" },
    { 0x2135, "\\aleph" }, { 0x00B0, "^{\\circ}" }, { 0x2212, "-" }, { 0x2061, "" }, { 0x2062, "" },
    { 0x2063, "," }, { 0x200B, "" }, { 0x2009, "\\," }, { 0x2005, "\\;" }, { 0x2003, "\\quad" }
};

static const char* sym_tex(uint32_t cp) {
    size_t i;

    for (i = 0; i < sizeof(SYMS) / sizeof(SYMS[0]); i++) {
        if (SYMS[i].cp == cp) {
            return SYMS[i].tex;
        }
    }

    return NULL;
}

static uint32_t utf8_next(const char* s, size_t n, size_t* i) {
    const unsigned char* u = (const unsigned char*)s + *i;
    uint32_t c = u[0];
    int k = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;

    if (*i + (size_t)k >= n + (k ? 0 : 1) && k) {
        (*i)++;
        return 0xFFFD;
    }

    c = k == 0 ? c : k == 1 ? c & 0x1F : k == 2 ? c & 0x0F : c & 0x07;

    for (; k > 0; k--) {
        (*i)++;
        c = (c << 6) | (s[*i] & 0x3F);
    }

    (*i)++;
    return c;
}

static void put_utf8(pd_buf* o, uint32_t c) {
    if (c < 0x80) {
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

/* a run's text as LaTeX: symbols as commands, specials escaped; upright text (sty p) as \mathrm,
   normal text as \text */
static void text_tex(pd_buf* o, const char* s, size_t n, int upright, int normal) {
    size_t i = 0;
    int letters = 0;

    for (i = 0; i < n; i++) {
        letters += isalpha((unsigned char)s[i]) != 0;
    }

    if (normal) {
        pb_puts(o, "\\text{");
    } else if (upright && letters > 0) {
        pb_puts(o, "\\mathrm{");
    }

    for (i = 0; i < n;) {
        uint32_t c = utf8_next(s, n, &i);
        const char* t = c >= 0x80 ? sym_tex(c) : NULL;

        if (t) {
            pb_puts(o, t);

            if (t[0] == '\\' && isalpha((unsigned char)t[strlen(t) - 1]) && i < n && isalpha((unsigned char)s[i])) {
                pb_putc(o, ' ');    /* \alpha x, not \alphax */
            }
        } else if (c == '{' || c == '}' || c == '#' || c == '$' || c == '%' || c == '&' || c == '_') {
            pb_putc(o, '\\');
            pb_putc(o, (char)c);
        } else if (c == '\\') {
            pb_puts(o, "\\backslash ");
        } else if (c == '^') {
            pb_puts(o, "\\hat{}");
        } else if (c == '~') {
            pb_puts(o, "\\sim ");
        } else if (c == ' ' && !normal) {
            pb_puts(o, "\\ ");
        } else {
            put_utf8(o, c);
        }
    }

    if (normal || (upright && letters > 0)) {
        pb_putc(o, '}');
    }
}

/* a delimiter character as LaTeX after \left or \right */
static void delim_tex(pd_buf* o, const char* v, int present) {
    size_t i = 0;
    uint32_t c;

    if (!present) {
        return;
    }

    if (!v[0]) {
        pb_putc(o, '.');
        return;
    }

    c = utf8_next(v, strlen(v), &i);
    pb_puts(o, c == '{' ? "\\{" : c == '}' ? "\\}" : c == 0x2016 ? "\\|" : c == 0x27E8 || c == 0x2329 ? "\\langle " :
            c == 0x27E9 || c == 0x232A ? "\\rangle " : c == 0x230A ? "\\lfloor " : c == 0x230B ? "\\rfloor " :
            c == 0x2308 ? "\\lceil " : c == 0x2309 ? "\\rceil " : c == '[' ? "[" : c == ']' ? "]" : c == '(' ? "(" :
            c == ')' ? ")" : c == '|' ? "|" : ".");
}

/* ------------------------------------------------------------------ */
/* OMML to LaTeX                                                      */
/* ------------------------------------------------------------------ */

#define MAXP 64

typedef struct {
    char name[16];
    pd_buf b;
} mpart;

typedef struct {
    mpart part[MAXP];
    int np;
    pd_buf seq;                 /* everything, in order: for elements that only group */
    char chr[16], beg[16], end[16], sep[16], pos[8], type[16], lim[16];
    int has_chr, has_beg, has_end, has_sep, deg_hide, sty_p, nor;
} mel;

static const pd_buf* part_of(const mel* e, const char* name, int nth) {
    int i;

    for (i = 0; i < e->np; i++) {
        if (!strcmp(e->part[i].name, name) && nth-- == 0) {
            return &e->part[i].b;
        }
    }

    return NULL;
}

static void put_part(pd_buf* o, const mel* e, const char* name) {
    const pd_buf* b = part_of(e, name, 0);

    if (b && b->n) {
        pb_put(o, b->p, b->n);
    }
}

static void braced(pd_buf* o, const mel* e, const char* name) {
    pb_putc(o, '{');
    put_part(o, e, name);
    pb_putc(o, '}');
}

static int convert(pd_markup* g, const char* name, pd_buf* out, int depth);

/* the properties of a construct (m:fPr, m:dPr, ...) into e, read to the element's end */
static void read_props(pd_markup* g, const char* name, mel* e) {
    int depth = 1;

    while (depth > 0 && mu_next(g) != MT_END) {
        const char* t = mu_local(g->name);
        char v[32];

        if (g->type == MT_OPEN) {
            depth++;
        } else if (g->type == MT_CLOSE) {
            depth -= !strcmp(t, name) || depth > 1 ? 1 : 0;
            continue;
        }

        if (g->type != MT_OPEN && g->type != MT_EMPTY) {
            continue;
        }

        if (!mu_attr(g, "m:val", v, sizeof(v))) {
            v[0] = '\0';
        }

        if (!strcmp(t, "chr")) {
            snprintf(e->chr, sizeof(e->chr), "%s", v);
            e->has_chr = 1;
        } else if (!strcmp(t, "begChr")) {
            snprintf(e->beg, sizeof(e->beg), "%s", v);
            e->has_beg = 1;
        } else if (!strcmp(t, "endChr")) {
            snprintf(e->end, sizeof(e->end), "%s", v);
            e->has_end = 1;
        } else if (!strcmp(t, "sepChr")) {
            snprintf(e->sep, sizeof(e->sep), "%s", v);
            e->has_sep = 1;
        } else if (!strcmp(t, "pos")) {
            snprintf(e->pos, sizeof(e->pos), "%s", v);
        } else if (!strcmp(t, "type")) {
            snprintf(e->type, sizeof(e->type), "%s", v);
        } else if (!strcmp(t, "limLoc")) {
            snprintf(e->lim, sizeof(e->lim), "%s", v);
        } else if (!strcmp(t, "degHide")) {
            e->deg_hide = !v[0] || !strcmp(v, "1") || !strcmp(v, "on") || !strcmp(v, "true");
        } else if (!strcmp(t, "sty")) {
            e->sty_p = !strcmp(v, "p") || !strcmp(v, "b");
        } else if (!strcmp(t, "nor")) {
            e->nor = !v[0] || !strcmp(v, "1") || !strcmp(v, "on");
        }
    }
}

/* a math run: its text, upright or normal as its properties say */
static void read_run(pd_markup* g, pd_buf* out) {
    int depth = 1, in_t = 0;
    mel props;
    pd_buf text;

    memset(&props, 0, sizeof(props));
    memset(&text, 0, sizeof(text));

    while (depth > 0 && mu_next(g) != MT_END) {
        const char* t = mu_local(g->name);

        if (g->type == MT_OPEN) {
            if (!strcmp(t, "rPr") && depth == 1) {
                read_props(g, "rPr", &props);
                continue;
            }

            depth++;
            in_t = !strcmp(t, "t");
        } else if (g->type == MT_CLOSE) {
            depth--;
            in_t = 0;
        } else if (g->type == MT_TEXT && in_t) {
            mu_decode(g->text, g->tlen, &text);
        }
    }

    if (text.n) {
        /* a command just written (\times) and a letter now: apart, or they would make one name */
        if (out->n > 1 && isalpha((unsigned char)out->p[out->n - 1]) && isalpha((unsigned char)text.p[0]) &&
                !props.sty_p && !props.nor) {
            size_t k = out->n;

            while (k > 0 && isalpha((unsigned char)out->p[k - 1])) {
                k--;
            }

            if (k > 0 && out->p[k - 1] == '\\') {
                pb_putc(out, ' ');
            }
        }

        text_tex(out, text.p, text.n, props.sty_p, props.nor);
    }

    pb_free(&text);
}

/* an element's children converted, then the element itself into out */
static int convert(pd_markup* g, const char* name, pd_buf* out, int depth) {
    mel* e;
    int i;

    if (depth > 40 || (e = (mel*)calloc(1, sizeof(mel))) == NULL) {
        return -1;
    }

    while (mu_next(g) != MT_END) {
        const char* t = mu_local(g->name);
        size_t tl = strlen(t);

        if (g->type == MT_CLOSE) {
            break;      /* this element's end (the markup is well nested here) */
        }

        if (g->type == MT_EMPTY) {
            continue;
        }

        if (g->type != MT_OPEN) {
            continue;
        }

        if (tl > 2 && !strcmp(t + tl - 2, "Pr")) {      /* fPr, dPr, naryPr, ctrlPr, ... */
            read_props(g, t, e);
        } else if (!strcmp(t, "r")) {
            read_run(g, &e->seq);
        } else {
            pd_buf b;

            memset(&b, 0, sizeof(b));
            convert(g, t, &b, depth + 1);

            if (e->np < MAXP) {
                snprintf(e->part[e->np].name, sizeof(e->part[0].name), "%s", t);
                e->part[e->np].b = b;
                e->np++;

                if (b.n) {
                    pb_put(&e->seq, b.p, b.n);
                }
            } else {
                pb_put(&e->seq, b.p, b.n);
                pb_free(&b);
            }
        }
    }

    if (!strcmp(name, "f")) {
        if (!strcmp(e->type, "lin")) {      /* a/b */
            put_part(out, e, "num");
            pb_putc(out, '/');
            put_part(out, e, "den");
        } else if (!strcmp(e->type, "noBar")) {
            pb_puts(out, "\\binom");
            braced(out, e, "num");
            braced(out, e, "den");
        } else {
            pb_puts(out, "\\frac");
            braced(out, e, "num");
            braced(out, e, "den");
        }
    } else if (!strcmp(name, "sSup")) {
        braced(out, e, "e");
        pb_putc(out, '^');
        braced(out, e, "sup");
    } else if (!strcmp(name, "sSub")) {
        braced(out, e, "e");
        pb_putc(out, '_');
        braced(out, e, "sub");
    } else if (!strcmp(name, "sSubSup")) {
        braced(out, e, "e");
        pb_putc(out, '_');
        braced(out, e, "sub");
        pb_putc(out, '^');
        braced(out, e, "sup");
    } else if (!strcmp(name, "sPre")) {
        pb_puts(out, "{}_");
        braced(out, e, "sub");
        pb_putc(out, '^');
        braced(out, e, "sup");
        braced(out, e, "e");
    } else if (!strcmp(name, "rad")) {
        const pd_buf* deg = part_of(e, "deg", 0);

        pb_puts(out, "\\sqrt");

        if (!e->deg_hide && deg && deg->n) {
            pb_putc(out, '[');
            pb_put(out, deg->p, deg->n);
            pb_putc(out, ']');
        }

        braced(out, e, "e");
    } else if (!strcmp(name, "nary")) {
        size_t k = 0;
        uint32_t c = e->has_chr && e->chr[0] ? utf8_next(e->chr, strlen(e->chr), &k) : 0x222B;
        const char* op = sym_tex(c);
        const pd_buf* sb = part_of(e, "sub", 0), *sp = part_of(e, "sup", 0);

        pb_puts(out, op && op[0] == '\\' ? op : "\\int");

        if (sb && sb->n) {
            pb_puts(out, "_{");
            pb_put(out, sb->p, sb->n);
            pb_putc(out, '}');
        }

        if (sp && sp->n) {
            pb_puts(out, "^{");
            pb_put(out, sp->p, sp->n);
            pb_putc(out, '}');
        }

        pb_putc(out, ' ');
        braced(out, e, "e");
    } else if (!strcmp(name, "d")) {
        const pd_buf* b;
        char sep[16];

        snprintf(sep, sizeof(sep), "%s", e->has_sep ? e->sep : "|");
        pb_puts(out, "\\left");
        delim_tex(out, e->has_beg ? e->beg : "(", 1);

        for (i = 0; (b = part_of(e, "e", i)) != NULL; i++) {
            if (i) {
                if (!strcmp(sep, "|")) {
                    pb_puts(out, "\\mid ");
                } else {
                    text_tex(out, sep, strlen(sep), 0, 0);
                }
            }

            pb_put(out, b->p, b->n);
        }

        pb_puts(out, "\\right");
        delim_tex(out, e->has_end ? e->end : ")", 1);
    } else if (!strcmp(name, "acc")) {
        size_t k = 0;
        uint32_t c = e->has_chr && e->chr[0] ? utf8_next(e->chr, strlen(e->chr), &k) : 0x0302;

        pb_puts(out, c == 0x0303 || c == 0x02DC ? "\\tilde" : c == 0x0304 || c == 0x0305 || c == 0x00AF ? "\\bar" :
                c == 0x0307 || c == 0x02D9 ? "\\dot" : c == 0x0308 || c == 0x00A8 ? "\\ddot" : c == 0x20D7 ||
                c == 0x2192 ? "\\vec" : c == 0x030C ? "\\check" : c == 0x0306 ? "\\breve" : "\\hat");
        braced(out, e, "e");
    } else if (!strcmp(name, "bar")) {
        pb_puts(out, !strcmp(e->pos, "top") ? "\\overline" : "\\underline");
        braced(out, e, "e");
    } else if (!strcmp(name, "groupChr")) {
        pb_puts(out, !strcmp(e->pos, "top") ? "\\overline" : "\\underline");
        braced(out, e, "e");
    } else if (!strcmp(name, "func")) {
        const pd_buf* fn = part_of(e, "fName", 0);
        static const char* known[] = { "sin", "cos", "tan", "cot", "sec", "csc", "sinh", "cosh", "tanh", "log", "ln",
                                       "exp", "lim", "max", "min", "sup", "inf", "det", "arg", "deg", "dim", "ker",
                                       "arcsin", "arccos", "arctan", "gcd", "Pr", NULL
                                     };
        int k, plain = 0;

        if (fn && fn->n > 10 && !strncmp(fn->p, "\\mathrm{", 8) && fn->p[fn->n - 1] == '}') {
            for (k = 0; known[k]; k++) {    /* \mathrm{sin} is \sin */
                if (fn->n - 9 == strlen(known[k]) && !strncmp(fn->p + 8, known[k], fn->n - 9)) {
                    pb_printf(out, "\\%s", known[k]);
                    plain = 1;
                    break;
                }
            }
        }

        if (!plain && fn) {
            pb_put(out, fn->p, fn->n);
        }

        pb_putc(out, ' ');
        braced(out, e, "e");
    } else if (!strcmp(name, "limLow") || !strcmp(name, "limUpp")) {
        braced(out, e, "e");
        pb_puts(out, name[3] == 'L' ? "_" : "^");
        braced(out, e, "lim");
    } else if (!strcmp(name, "mr")) {
        const pd_buf* b;

        for (i = 0; (b = part_of(e, "e", i)) != NULL; i++) {
            if (i) {
                pb_puts(out, " & ");
            }

            pb_put(out, b->p, b->n);
        }
    } else if (!strcmp(name, "m")) {
        const pd_buf* b;

        pb_puts(out, "\\begin{matrix}");

        for (i = 0; (b = part_of(e, "mr", i)) != NULL; i++) {
            if (i) {
                pb_puts(out, " \\\\ ");
            }

            pb_put(out, b->p, b->n);
        }

        pb_puts(out, "\\end{matrix}");
    } else if (!strcmp(name, "eqArr")) {
        const pd_buf* b;

        pb_puts(out, "\\begin{matrix}");

        for (i = 0; (b = part_of(e, "e", i)) != NULL; i++) {
            if (i) {
                pb_puts(out, " \\\\ ");
            }

            pb_put(out, b->p, b->n);
        }

        pb_puts(out, "\\end{matrix}");
    } else {    /* e, num, den, sub, sup, deg, fName, lim, box, borderBox, oMath, ...: their content */
        pb_put(out, e->seq.p, e->seq.n);
    }

    for (i = 0; i < e->np; i++) {
        pb_free(&e->part[i].b);
    }

    pb_free(&e->seq);
    free(e);
    return 0;
}

char* pd_omml_to_latex(const char* xml, size_t n, const char* root) {
    pd_markup g;
    pd_buf out;
    char* s;

    memset(&out, 0, sizeof(out));
    mu_init(&g, xml, n, 0);
    convert(&g, root, &out, 0);

    if (out.err || (s = (char*)malloc(out.n + 1)) == NULL) {
        pb_free(&out);
        return NULL;
    }

    if (out.n) {
        memcpy(s, out.p, out.n);
    }

    s[out.n] = '\0';
    pb_free(&out);
    return s;
}

/* ------------------------------------------------------------------ */
/* LaTeX to OMML                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char* s;
    size_t n, i;
    pd_buf* o;
    int depth;
} tp;

static void tex_list(tp* p, int stop_brace);

static void omml_text(pd_buf* o, const char* s, size_t n, int upright) {
    if (!n) {
        return;
    }

    pb_puts(o, "<m:r>");

    if (upright) {
        pb_puts(o, "<m:rPr><m:sty m:val=\"p\"/></m:rPr>");
    }

    pb_puts(o, "<m:t xml:space=\"preserve\">");

    {
        size_t i;

        for (i = 0; i < n; i++) {
            char c = s[i];

            pb_puts(o, c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : "");

            if (c != '<' && c != '>' && c != '&' && c != '"') {
                pb_putc(o, c);
            }
        }
    }

    pb_puts(o, "</m:t></m:r>");
}

static void skip_ws(tp* p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n')) {
        p->i++;
    }
}

/* a command's name after the backslash */
static size_t cmd_name(tp* p, char* buf, size_t cap) {
    size_t k = 0;

    if (p->i < p->n && !isalpha((unsigned char)p->s[p->i])) {
        buf[k++] = p->s[p->i++];
    } else {
        while (p->i < p->n && isalpha((unsigned char)p->s[p->i]) && k + 1 < cap) {
            buf[k++] = p->s[p->i++];
        }
    }

    buf[k] = '\0';
    return k;
}

/* one argument: a braced group or one token, as OMML into the output */
static void tex_arg(tp* p) {
    skip_ws(p);

    if (p->i < p->n && p->s[p->i] == '{') {
        p->i++;
        tex_list(p, 1);
    } else if (p->i < p->n) {
        size_t save = p->n;

        /* exactly one token: limit the list to it */
        if (p->s[p->i] == '\\') {
            size_t j = p->i + 1;

            if (j < p->n && isalpha((unsigned char)p->s[j])) {
                while (j < p->n && isalpha((unsigned char)p->s[j])) {
                    j++;
                }
            } else {
                j++;
            }

            p->n = j < save ? j : save;
        } else {
            size_t j = p->i;

            utf8_next(p->s, save, &j);
            p->n = j;
        }

        tex_list(p, 0);
        p->n = save;
    }
}

/* a run of the source as OMML: a braced group's to its end (stop_brace), or to the end */
static void tex_list(tp* p, int stop_brace) {
    pd_buf* o = p->o;
    size_t lit = p->i;      /* where plain characters began */
    size_t atom = (size_t)-1;   /* where in the output the last group or command began: a script's base */

    if (++p->depth > 60) {
        p->i = p->n;
        p->depth--;
        return;
    }

#define FLUSH() do { omml_text(o, p->s + lit, p->i - lit, 0); } while (0)
    while (p->i < p->n) {
        char c = p->s[p->i];

        if (c == '}' && stop_brace) {
            FLUSH();
            p->i++;
            p->depth--;
            return;
        }

        if (c == ' ' || c == '\t' || c == '\n') {     /* spaces mean nothing in math */
            FLUSH();
            p->i++;
            lit = p->i;
            continue;
        }

        if (c == '{') {
            FLUSH();
            atom = o->n;
            p->i++;
            tex_list(p, 1);
            lit = p->i;
            continue;
        }

        if (c == '^' || c == '_') {     /* scripts on what comes last: the last character run, crudely */
            size_t base_start = lit, k;
            const char* sub_or_sup = c == '^' ? "sup" : "sub";

            /* the base: the last character before the script, if plain text is pending */
            if (p->i > lit) {
                k = p->i - 1;

                while (k > lit && ((unsigned char)p->s[k] & 0xC0) == 0x80) {
                    k--;
                }

                omml_text(o, p->s + lit, k - lit, 0);
                base_start = k;
            }

            if (p->i == lit && atom != (size_t)-1 && atom < o->n) {     /* the base: the group or command just written */
                size_t tl = o->n - atom;
                char* tail = (char*)malloc(tl);

                if (tail) {
                    memcpy(tail, o->p + atom, tl);
                    o->n = atom;
                    pb_printf(o, "<m:s%s><m:e>", c == '^' ? "Sup" : "Sub");
                    pb_put(o, tail, tl);
                    free(tail);
                }
            } else {
                atom = o->n;
                pb_printf(o, "<m:s%s><m:e>", c == '^' ? "Sup" : "Sub");
                omml_text(o, p->s + base_start, p->i - base_start, 0);
            }

            {   /* the script, and the other one after it: then both, on one base */
                pd_buf first, second, *keep = p->o;
                int both = 0;

                memset(&first, 0, sizeof(first));
                memset(&second, 0, sizeof(second));
                p->i++;
                p->o = &first;
                tex_arg(p);
                skip_ws(p);

                if (p->i < p->n && p->s[p->i] == (c == '^' ? '_' : '^')) {
                    p->i++;
                    p->o = &second;
                    tex_arg(p);
                    both = 1;
                }

                p->o = keep;

                if (both) {     /* rename the element: sSub or sSup becomes sSubSup */
                    const char* tag = c == '^' ? "<m:sSup><m:e>" : "<m:sSub><m:e>";
                    size_t tl = strlen(tag), q;

                    for (q = o->n >= tl ? o->n - tl : 0; ; q--) {
                        if (q + tl <= o->n && !memcmp(o->p + q, tag, tl)) {
                            size_t rest = o->n - (q + tl);
                            char* tail = (char*)malloc(rest + 1);

                            if (tail) {
                                memcpy(tail, o->p + q + tl, rest);
                                o->n = q;
                                pb_puts(o, "<m:sSubSup><m:e>");
                                pb_put(o, tail, rest);
                                free(tail);
                            }

                            break;
                        }

                        if (q == 0) {
                            break;
                        }
                    }

                    pb_puts(o, "</m:e><m:sub>");
                    pb_put(o, c == '_' ? first.p : second.p, c == '_' ? first.n : second.n);
                    pb_puts(o, "</m:sub><m:sup>");
                    pb_put(o, c == '^' ? first.p : second.p, c == '^' ? first.n : second.n);
                    pb_puts(o, "</m:sup></m:sSubSup>");
                } else {
                    pb_printf(o, "</m:e><m:%s>", sub_or_sup);
                    pb_put(o, first.p, first.n);
                    pb_printf(o, "</m:%s></m:s%s>", sub_or_sup, c == '^' ? "Sup" : "Sub");
                }

                pb_free(&first);
                pb_free(&second);
            }

            lit = p->i;
            continue;
        }

        if (c == '\\') {
            char name[32];
            const msym* sy = NULL;
            size_t k;

            FLUSH();
            atom = o->n;
            p->i++;
            cmd_name(p, name, sizeof(name));

            if (!strcmp(name, "frac") || !strcmp(name, "dfrac") || !strcmp(name, "tfrac") || !strcmp(name, "cfrac") ||
                    !strcmp(name, "binom")) {
                pb_puts(o, name[0] == 'b' ? "<m:f><m:fPr><m:type m:val=\"noBar\"/></m:fPr><m:num>" : "<m:f><m:num>");
                tex_arg(p);
                pb_puts(o, "</m:num><m:den>");
                tex_arg(p);
                pb_puts(o, "</m:den></m:f>");
            } else if (!strcmp(name, "sqrt")) {
                skip_ws(p);
                pb_puts(o, "<m:rad>");

                if (p->i < p->n && p->s[p->i] == '[') {     /* [degree] */
                    size_t j = p->i + 1, save = p->n;

                    while (j < p->n && p->s[j] != ']') {
                        j++;
                    }

                    pb_puts(o, "<m:deg>");
                    p->i++;
                    p->n = j;
                    tex_list(p, 0);
                    p->n = save;
                    p->i = j < save ? j + 1 : save;
                    pb_puts(o, "</m:deg>");
                } else {
                    pb_puts(o, "<m:radPr><m:degHide m:val=\"1\"/></m:radPr><m:deg/>");
                }

                pb_puts(o, "<m:e>");
                tex_arg(p);
                pb_puts(o, "</m:e></m:rad>");
            } else if (!strcmp(name, "left")) {     /* \left( ... \right): a delimiter */
                char b[8] = "", e[8] = "";
                size_t j, deep = 0, save = p->n, close = p->n;

                skip_ws(p);

                if (p->i < p->n) {
                    if (p->s[p->i] == '\\' && p->i + 1 < p->n) {
                        p->i++;
                        cmd_name(p, b, sizeof(b));
                        snprintf(b, sizeof(b), "%s", !strcmp(b, "{") ? "{" : !strcmp(b, "|") ? "\xE2\x80\x96" :
                                 !strcmp(b, "langle") ? "\xE2\x9F\xA8" : !strcmp(b, "lfloor") ? "\xE2\x8C\x8A" :
                                 !strcmp(b, "lceil") ? "\xE2\x8C\x88" : "");
                    } else {
                        b[0] = p->s[p->i] == '.' ? '\0' : p->s[p->i];
                        b[1] = '\0';
                        p->i++;
                    }
                }

                /* the matching \right */
                for (j = p->i; j + 6 <= p->n; j++) {
                    if (!strncmp(p->s + j, "\\left", 5)) {
                        deep++;
                    } else if (!strncmp(p->s + j, "\\right", 6)) {
                        if (deep == 0) {
                            close = j;
                            break;
                        }

                        deep--;
                    }
                }

                pb_puts(o, "<m:d><m:dPr><m:begChr m:val=\"");
                pb_puts(o, b[0] == '<' ? "&lt;" : b);
                pb_puts(o, "\"/>");
                j = close + 6;

                if (close < p->n) {
                    skip_ws(p);

                    if (j < p->n && p->s[j] == '\\') {
                        size_t q = j + 1;

                        if (q < p->n && !isalpha((unsigned char)p->s[q])) {
                            e[0] = p->s[q] == '}' ? '}' : p->s[q] == '|' ? '|' : '\0';
                            e[1] = '\0';
                            j = q + 1;

                            if (!strcmp(e, "|")) {
                                snprintf(e, sizeof(e), "\xE2\x80\x96");
                            }
                        } else {
                            size_t r = q;

                            while (r < p->n && isalpha((unsigned char)p->s[r])) {
                                r++;
                            }

                            snprintf(e, sizeof(e), "%s", !strncmp(p->s + q, "rangle", 6) ? "\xE2\x9F\xA9" :
                                     !strncmp(p->s + q, "rfloor", 6) ? "\xE2\x8C\x8B" : !strncmp(p->s + q, "rceil", 5) ?
                                     "\xE2\x8C\x89" : "");
                            j = r;
                        }
                    } else if (j < p->n) {
                        e[0] = p->s[j] == '.' ? '\0' : p->s[j];
                        e[1] = '\0';
                        j++;
                    }
                }

                pb_puts(o, "<m:endChr m:val=\"");
                pb_puts(o, e[0] == '>' ? "&gt;" : e);
                pb_puts(o, "\"/></m:dPr><m:e>");
                p->n = close;
                tex_list(p, 0);
                p->n = save;
                pb_puts(o, "</m:e></m:d>");
                p->i = j < save ? j : save;
            } else if (!strcmp(name, "mathrm") || !strcmp(name, "operatorname") || !strcmp(name, "text") ||
                       !strcmp(name, "textrm") || !strcmp(name, "mbox")) {  /* upright text */
                size_t j, deep = 0, start;

                skip_ws(p);

                if (p->i < p->n && p->s[p->i] == '{') {
                    start = ++p->i;

                    for (j = start; j < p->n; j++) {
                        if (p->s[j] == '{') {
                            deep++;
                        } else if (p->s[j] == '}' && deep-- == 0) {
                            break;
                        }
                    }

                    {   /* the text, its escapes undone */
                        pd_buf u;
                        size_t q;

                        memset(&u, 0, sizeof(u));

                        for (q = start; q < j; q++) {
                            if (p->s[q] == '\\' && q + 1 < j && isalpha((unsigned char)p->s[q + 1])) {
                                /* a command: the character it stands for (\cdot, \alpha, \backslash) */
                                size_t r = q + 1, z;
                                char nm[32];

                                while (r < j && isalpha((unsigned char)p->s[r]) && r - q - 1 < sizeof(nm) - 1) {
                                    r++;
                                }

                                snprintf(nm, sizeof(nm), "%.*s", (int)(r - q - 1), p->s + q + 1);

                                if (!strcmp(nm, "backslash")) {
                                    pb_putc(&u, '\\');
                                } else {
                                    for (z = 0; z < sizeof(SYMS) / sizeof(SYMS[0]); z++) {
                                        if (SYMS[z].tex[0] == '\\' && !strcmp(SYMS[z].tex + 1, nm)) {
                                            put_utf8(&u, SYMS[z].cp);
                                            break;
                                        }
                                    }
                                }

                                while (r < j && p->s[r] == ' ') {   /* the space that ends the name */
                                    r++;
                                }

                                q = r - 1;
                            } else if (p->s[q] == '\\' && q + 1 < j) {
                                pb_putc(&u, p->s[++q]);     /* \  \{  \}  \_ ... */
                            } else {
                                pb_putc(&u, p->s[q]);
                            }
                        }

                        omml_text(o, u.p ? u.p : "", u.n, 1);
                        pb_free(&u);
                    }

                    p->i = j < p->n ? j + 1 : p->n;
                }
            } else if (!strcmp(name, "overline") || !strcmp(name, "underline")) {
                pb_printf(o, "<m:bar><m:barPr><m:pos m:val=\"%s\"/></m:barPr><m:e>", name[0] == 'o' ? "top" : "bot");
                tex_arg(p);
                pb_puts(o, "</m:e></m:bar>");
            } else if (!strcmp(name, "hat") || !strcmp(name, "widehat") || !strcmp(name, "tilde") ||
                       !strcmp(name, "widetilde") || !strcmp(name, "bar") || !strcmp(name, "dot") ||
                       !strcmp(name, "ddot") || !strcmp(name, "vec") || !strcmp(name, "check") || !strcmp(name, "breve")) {
                const char* ch = strstr(name, "tilde") ? "\xCC\x83" : !strcmp(name, "bar") ? "\xCC\x85" :
                                 !strcmp(name, "dot") ? "\xCC\x87" : !strcmp(name, "ddot") ? "\xCC\x88" :
                                 !strcmp(name, "vec") ? "\xE2\x83\x97" : !strcmp(name, "check") ? "\xCC\x8C" :
                                 !strcmp(name, "breve") ? "\xCC\x86" : "\xCC\x82";

                pb_printf(o, "<m:acc><m:accPr><m:chr m:val=\"%s\"/></m:accPr><m:e>", ch);
                tex_arg(p);
                pb_puts(o, "</m:e></m:acc>");
            } else if (!strcmp(name, "sum") || !strcmp(name, "prod") || !strcmp(name, "int") || !strcmp(name, "iint") ||
                       !strcmp(name, "iiint") || !strcmp(name, "oint") || !strcmp(name, "bigcup") ||
                       !strcmp(name, "bigcap") || !strcmp(name, "coprod")) {
                const char* ch = NULL;

                for (k = 0; k < sizeof(SYMS) / sizeof(SYMS[0]); k++) {
                    if (!strcmp(SYMS[k].tex + 1, name)) {
                        sy = &SYMS[k];
                        break;
                    }
                }

                pb_puts(o, "<m:nary><m:naryPr><m:chr m:val=\"");

                if (sy) {
                    pd_buf tmp;

                    memset(&tmp, 0, sizeof(tmp));
                    put_utf8(&tmp, sy->cp);
                    pb_put(o, tmp.p, tmp.n);
                    pb_free(&tmp);
                }

                (void)ch;
                pb_puts(o, "\"/></m:naryPr><m:sub>");
                skip_ws(p);

                /* the limits, in either order */
                {
                    int got_sub = 0, got_sup = 0;
                    pd_buf* keep = p->o;
                    pd_buf sub, sup;

                    memset(&sub, 0, sizeof(sub));
                    memset(&sup, 0, sizeof(sup));

                    while (p->i < p->n && (p->s[p->i] == '_' || p->s[p->i] == '^' || !strncmp(p->s + p->i, "\\limits", 7))) {
                        if (p->s[p->i] == '\\') {
                            p->i += 7;
                        } else {
                            int is_sub = p->s[p->i] == '_';

                            p->i++;
                            p->o = is_sub ? &sub : &sup;
                            tex_arg(p);
                            got_sub |= is_sub;
                            got_sup |= !is_sub;
                        }

                        skip_ws(p);
                    }

                    p->o = keep;
                    pb_put(o, sub.p, sub.n);
                    pb_puts(o, "</m:sub><m:sup>");
                    pb_put(o, sup.p, sup.n);
                    pb_puts(o, "</m:sup><m:e>");
                    pb_free(&sub);
                    pb_free(&sup);
                    (void)got_sub;
                    (void)got_sup;
                }

                tex_arg(p);
                pb_puts(o, "</m:e></m:nary>");
            } else if (!strcmp(name, "begin")) {    /* a matrix: rows and cells */
                char env[24] = "";
                size_t j, start, end_at;

                skip_ws(p);

                if (p->i < p->n && p->s[p->i] == '{') {
                    j = ++p->i;

                    while (p->i < p->n && p->s[p->i] != '}') {
                        p->i++;
                    }

                    snprintf(env, sizeof(env), "%.*s", (int)(p->i - j), p->s + j);
                    p->i = p->i < p->n ? p->i + 1 : p->n;
                }

                start = p->i;

                {
                    char want[40];
                    const char* hit;

                    snprintf(want, sizeof(want), "\\end{%s}", env);
                    hit = NULL;

                    for (j = start; j + strlen(want) <= p->n; j++) {
                        if (!strncmp(p->s + j, want, strlen(want))) {
                            hit = p->s + j;
                            break;
                        }
                    }

                    end_at = hit ? (size_t)(hit - p->s) : p->n;
                    pb_puts(o, "<m:m><m:mr><m:e>");

                    {
                        size_t q = start, cell = start, save = p->n;
                        int deep = 0;

                        for (q = start; q <= end_at; q++) {
                            int at_end = q == end_at;

                            if (!at_end && p->s[q] == '{') {
                                deep++;
                            } else if (!at_end && p->s[q] == '}') {
                                deep--;
                            }

                            if (at_end || (deep == 0 && (p->s[q] == '&' || (p->s[q] == '\\' && q + 1 < end_at &&
                                                                           p->s[q + 1] == '\\')))) {
                                p->i = cell;
                                p->n = q;
                                tex_list(p, 0);
                                p->n = save;

                                if (at_end) {
                                    break;
                                }

                                if (p->s[q] == '&') {
                                    pb_puts(o, "</m:e><m:e>");
                                    cell = q + 1;
                                } else {
                                    pb_puts(o, "</m:e></m:mr><m:mr><m:e>");
                                    cell = q + 2;
                                    q++;
                                }
                            }
                        }
                    }

                    pb_puts(o, "</m:e></m:mr></m:m>");
                    p->i = hit ? end_at + strlen(want) : p->n;
                }
            } else {    /* a symbol, a function name, spacing, or one not known: as text */
                for (k = 0; k < sizeof(SYMS) / sizeof(SYMS[0]); k++) {
                    if (SYMS[k].tex[0] == '\\' && !strcmp(SYMS[k].tex + 1, name)) {
                        sy = &SYMS[k];
                        break;
                    }
                }

                if (sy) {
                    pd_buf tmp;

                    memset(&tmp, 0, sizeof(tmp));
                    put_utf8(&tmp, sy->cp);
                    omml_text(o, tmp.p, tmp.n, 0);
                    pb_free(&tmp);
                } else if (!strcmp(name, ",") || !strcmp(name, ";") || !strcmp(name, " ") || !strcmp(name, "quad") ||
                           !strcmp(name, "qquad") || !strcmp(name, "!")) {
                    omml_text(o, " ", strcmp(name, "!") ? 1 : 0, 0);
                } else if (!strcmp(name, "{") || !strcmp(name, "}") || !strcmp(name, "%") || !strcmp(name, "&") ||
                           !strcmp(name, "_") || !strcmp(name, "#") || !strcmp(name, "$")) {
                    omml_text(o, name, 1, 0);
                } else if (!strcmp(name, "mathbf") || !strcmp(name, "mathit") || !strcmp(name, "boldsymbol") ||
                           !strcmp(name, "mathcal") || !strcmp(name, "mathbb") || !strcmp(name, "mathsf")) {
                    tex_arg(p);     /* the letters, without the alphabet */
                } else if (name[0]) {   /* \sin, \log, ...: upright */
                    omml_text(o, name, strlen(name), 1);
                }
            }

            lit = p->i;
            continue;
        }

        p->i++;
    }

    FLUSH();
#undef FLUSH
    p->depth--;
}

void pd_latex_to_omml(const char* tex, size_t n, int display, pd_buf* out) {
    tp p;

    memset(&p, 0, sizeof(p));
    p.s = tex;
    p.n = n;
    p.o = out;

    if (display) {
        pb_puts(out, "<m:oMathPara>");
    }

    pb_puts(out, "<m:oMath>");
    tex_list(&p, 0);
    pb_puts(out, "</m:oMath>");

    if (display) {
        pb_puts(out, "</m:oMathPara>");
    }
}
