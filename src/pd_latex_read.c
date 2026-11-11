/*
 * Parade LaTeX import: a .tex file read as a document
 *
 * Not TeX: no catcodes, no boxes, no output routine. What an article,
 * report or book written the usual way has, read for what it means and
 * built with the document builder -- the title block, sectioning (numbered
 * as LaTeX numbers it), paragraphs, the text commands and declarations,
 * accents and special characters, lists, quotations, verbatim and listings,
 * tables (tabular and its kin: spans, booktabs rules, the head row),
 * figures and tables as floats with their captions, pictures (by address,
 * loaded later as a Markdown file's are), footnotes, links, inline and
 * display mathematics (kept as LaTeX source for Parade's math layout),
 * labels with \ref, \eqref and \autoref, and citations against a
 * thebibliography list.
 *
 * Macros a document defines (\newcommand, \renewcommand, \def, \let,
 * \DeclareMathOperator, \newenvironment) are expanded, in mathematics too.
 * It reads the source twice: the first time only counting (sections,
 * figures, tables, equations, bibliography entries) and noting what each
 * \label names, so a reference to something further on has its number.
 * Anything it does not know is read as its arguments' text; commands that
 * only set the page (\vspace, \usepackage, ...) are left out.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pd_conv.h"

#define LX_MAX_DEPTH 48             /* macro expansion and nesting */
#define LX_EXPAND_BUDGET (64u << 20) /* bytes macros may expand to, in all */

typedef struct {
    char name[48];
    int nargs;
    char* opt;                      /* the first argument's default: it is optional ([...]) */
    char* body;
    int is_env;
    char* end_body;                 /* an environment's end code */
} tmacro;

typedef struct {
    char key[80];
    char num[48];
    char kind[16];                  /* "section", "figure", "table", "equation", "item", "" */
} tlabel;

typedef struct {
    char key[80];
    char who[96];                   /* natbib's \bibitem[Smith et al.(2020)]: "Smith et al." */
    char year[16];
    char label[24];                 /* \bibitem[Fan09]: cited as [Fan09], not a number */
} tbib;

typedef struct {
    pd_bld* b;
    int dry;                        /* counting only: nothing built */
    const char* src;                /* the whole file */
    size_t len;
    tmacro* mac;
    int nmac, capmac;
    tlabel* lab;
    int nlab, caplab;
    tbib* bib;                      /* bibliography entries, in order */
    int nbib, capbib;
    char (*cited)[80];              /* the keys cited, in the order first cited (the first time through) */
    int ncited, capcited, cite_all; /* cite_all: \nocite{*} */
    char bibstyle[48];
    char* bibdata;                  /* a .bib file's text the document's \bibliography brought (parade-bibtex) */
    size_t bibdata_len;
    char* bibgen;                   /* that set out as a thebibliography list */
    size_t bibgen_len;
    int bib_shown;                  /* that list put in (once: a second \bibliography has it already) */
    /* counters */
    int has_chapter, appendix;
    int sec[7];                     /* chapter section subsection subsubsection paragraph subparagraph */
    int fig, tab, eq, fn;
    char cur_num[48], cur_kind[16];  /* what a \label names now */
    /* the paragraph */
    int in_para, space_pending, at_start;
    const char* style;
    int32_t role, level, align;     /* align -1: the style's */
    int ragged;                     /* \raggedright: body text not justified (until its environment ends) */
    /* lists */
    int ldepth;
    int lkind[10];                  /* 1 bullets, 2 numbers, 3 description */
    pd_list_id lid[10];
    int item_count[10];
    int item_pending;
    /* the format */
    pd_char_props cp;
    int in_float;                   /* 1 figure, 2 table */
    int in_sub;                     /* in a subfigure: its pictures side by side, its caption "(a) ..." */
    int sub_count;                  /* the float's subfigures so far */
    int line_start;                 /* just after a line break: blanks there are the source's, not the text's */
    int verbatim_code;
    /* the title block */
    char* title, * author, * date;
    int has_date;
    int depth;
    size_t expanded;
    int in_document;
} lt;

static void run(lt* L, const char* s, size_t n);

/* ------------------------------------------------------------------ */
/* reading the source                                                 */
/* ------------------------------------------------------------------ */

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '@';
}

/* past spaces, line ends and comments */
static void skip_ws(const char* s, size_t n, size_t* i) {
    while (*i < n) {
        if (s[*i] == ' ' || s[*i] == '\t' || s[*i] == '\n' || s[*i] == '\r') {
            (*i)++;
        } else if (s[*i] == '%') {
            while (*i < n && s[*i] != '\n') {
                (*i)++;
            }
        } else {
            break;
        }
    }
}

/* the end of a group opened at s[i] == '{' (the index of its '}', n if it has none) */
static size_t group_end(const char* s, size_t n, size_t i) {
    int d = 0;

    for (; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
        } else if (s[i] == '%') {
            while (i < n && s[i] != '\n') {
                i++;
            }
        } else if (s[i] == '{') {
            d++;
        } else if (s[i] == '}' && --d == 0) {
            return i;
        }
    }

    return n;
}

/* an argument: a group's content, or one token (a command's name with its backslash, or a character) */
static int read_arg(const char* s, size_t n, size_t* i, size_t* a, size_t* b) {
    skip_ws(s, n, i);

    if (*i >= n) {
        return 0;
    }

    if (s[*i] == '{') {
        size_t e = group_end(s, n, *i);

        *a = *i + 1;
        *b = e;
        *i = e < n ? e + 1 : n;
        return 1;
    }

    if (s[*i] == '}') {
        return 0;
    }

    *a = *i;

    if (s[*i] == '\\' && *i + 1 < n) {
        (*i)++;

        if (is_letter(s[*i])) {
            while (*i < n && is_letter(s[*i])) {
                (*i)++;
            }
        } else {
            (*i)++;
        }
    } else {
        /* one character: a whole UTF-8 sequence */
        unsigned char c = (unsigned char)s[*i];

        (*i) += c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;

        if (*i > n) {
            *i = n;
        }
    }

    *b = *i;
    return 1;
}

/* an optional argument [...] (braces in it kept whole) */
static int read_opt(const char* s, size_t n, size_t* i, size_t* a, size_t* b) {
    size_t j = *i;
    int d = 0;

    skip_ws(s, n, &j);

    if (j >= n || s[j] != '[') {
        return 0;
    }

    *a = j + 1;

    for (j++; j < n; j++) {
        if (s[j] == '{') {
            d++;
        } else if (s[j] == '}') {
            d--;
        } else if (s[j] == ']' && d <= 0) {
            break;
        }
    }

    *b = j;
    *i = j < n ? j + 1 : n;
    return 1;
}

/* a * after a command's name (\section*) */
static int read_star(const char* s, size_t n, size_t* i) {
    if (*i < n && s[*i] == '*') {
        (*i)++;
        return 1;
    }

    return 0;
}

/* where \end{name} is for a \begin{name} whose content starts at from, the content's end in *ce; n if none */
static size_t env_end(const char* s, size_t n, size_t from, const char* name, size_t* ce) {
    size_t i = from, ln = strlen(name);
    int d = 1;

    while (i < n) {
        if (s[i] == '%') {
            while (i < n && s[i] != '\n') {
                i++;
            }

            continue;
        }

        if (s[i] == '\\' && i + 6 < n && (!strncmp(s + i, "\\begin", 6) || !strncmp(s + i, "\\end", 4))) {
            int is_begin = s[i + 1] == 'b';
            size_t j = i + (is_begin ? 6 : 4), a, b;

            if (read_arg(s, n, &j, &a, &b) && b - a == ln && !strncmp(s + a, name, ln)) {
                if (is_begin) {
                    d++;
                } else if (--d == 0) {
                    *ce = i;
                    return j;
                }
            }

            i = j > i ? j : i + 1;
            continue;
        }

        if (s[i] == '\\' && i + 1 < n) {
            i += 2;
            continue;
        }

        i++;
    }

    *ce = n;
    return n;
}

/* verbatim: the raw end of \end{name}, no nesting, no comments */
static size_t verb_end(const char* s, size_t n, size_t from, const char* name, size_t* ce) {
    char pat[64];
    const char* p;
    size_t pl;

    snprintf(pat, sizeof(pat), "\\end{%s}", name);
    pl = strlen(pat);

    for (p = s + from; (size_t)(p - s) + pl <= n; p++) {
        if (*p == '\\' && !strncmp(p, pat, pl)) {
            *ce = (size_t)(p - s);
            return *ce + pl;
        }
    }

    *ce = n;
    return n;
}

static char* dup_range(const char* s, size_t a, size_t b) {
    char* r = (char*)malloc(b - a + 1);

    if (r) {
        memcpy(r, s + a, b - a);
        r[b - a] = '\0';
    }

    return r;
}

/* ------------------------------------------------------------------ */
/* macros                                                             */
/* ------------------------------------------------------------------ */

static tmacro* find_macro(lt* L, const char* name, size_t len, int env) {
    int k;

    for (k = L->nmac - 1; k >= 0; k--) {
        if (L->mac[k].is_env == env && strlen(L->mac[k].name) == len && !strncmp(L->mac[k].name, name, len)) {
            return &L->mac[k];
        }
    }

    return NULL;
}

static void define_macro(lt* L, const char* name, size_t nlen, int nargs, const char* opt, size_t optlen,
                         const char* body, size_t blen, int env, const char* endb, size_t elen) {
    tmacro* m = find_macro(L, name, nlen, env);

    if (nlen == 0 || nlen >= sizeof(m->name)) {
        return;
    }

    if (!m) {
        if (L->nmac == L->capmac) {
            int nc = L->capmac ? L->capmac * 2 : 32;
            tmacro* q = (tmacro*)realloc(L->mac, sizeof(tmacro) * (size_t)nc);

            if (!q) {
                return;
            }

            L->mac = q;
            L->capmac = nc;
        }

        m = &L->mac[L->nmac++];
        memset(m, 0, sizeof(*m));
    } else {
        free(m->opt);
        free(m->body);
        free(m->end_body);
    }

    memcpy(m->name, name, nlen);
    m->name[nlen] = '\0';
    m->nargs = nargs < 0 ? 0 : nargs > 9 ? 9 : nargs;
    m->opt = opt ? dup_range(opt, 0, optlen) : NULL;
    m->body = dup_range(body, 0, blen);
    m->is_env = env;
    m->end_body = endb ? dup_range(endb, 0, elen) : NULL;
}

/* a macro's body with its arguments put in for #1..#9 */
static void substitute(pd_buf* o, const char* body, char** args, size_t* alen, int nargs) {
    const char* p;

    for (p = body; *p; p++) {
        if (*p == '#' && p[1] >= '1' && p[1] <= '9') {
            int k = p[1] - '1';

            if (k < nargs && args[k]) {
                pb_put(o, args[k], alen[k]);
            }

            p++;
        } else if (*p == '#' && p[1] == '#') {
            pb_putc(o, '#');
            p++;
        } else {
            pb_putc(o, *p);
        }
    }
}

/* a macro used at s[*i] (just past its name): its arguments read, its expansion into o */
static int expand_macro(lt* L, const tmacro* m, const char* s, size_t n, size_t* i, pd_buf* o) {
    char* args[9];
    size_t alen[9];
    int k;

    memset(args, 0, sizeof(args));

    for (k = 0; k < m->nargs; k++) {
        size_t a, b;

        if (k == 0 && m->opt) {
            if (read_opt(s, n, i, &a, &b)) {
                args[0] = (char*)s + a;
                alen[0] = b - a;
            } else {
                args[0] = m->opt;
                alen[0] = strlen(m->opt);
            }

            continue;
        }

        if (!read_arg(s, n, i, &a, &b)) {
            break;
        }

        args[k] = (char*)s + a;
        alen[k] = b - a;
    }

    substitute(o, m->body, args, alen, m->nargs);

    if (L->expanded + o->n > LX_EXPAND_BUDGET) {
        return 0;   /* a macro that grows for ever: no more */
    }

    L->expanded += o->n;
    return 1;
}

/* mathematics with the document's macros put in (Parade's math knows LaTeX's own, not the document's) */
static void expand_math(lt* L, const char* s, size_t n, pd_buf* o, int depth) {
    size_t i = 0;

    while (i < n) {
        if (s[i] == '%') {
            while (i < n && s[i] != '\n') {
                i++;
            }

            continue;
        }

        if (s[i] == '\\' && i + 1 < n && is_letter(s[i + 1])) {
            size_t a = i + 1, e = a;
            tmacro* m;

            while (e < n && is_letter(s[e])) {
                e++;
            }

            m = find_macro(L, s + a, e - a, 0);

            /* labels and numbering switches: not mathematics */
            if ((e - a == 5 && !strncmp(s + a, "label", 5))) {
                size_t j = e, x, y;

                read_arg(s, n, &j, &x, &y);
                i = j;
                continue;
            }

            if ((e - a == 8 && !strncmp(s + a, "nonumber", 8)) || (e - a == 5 && !strncmp(s + a, "notag", 5))) {
                i = e;
                continue;
            }

            if (m && depth < LX_MAX_DEPTH) {
                pd_buf x;
                size_t j = e;

                memset(&x, 0, sizeof(x));

                if (expand_macro(L, m, s, n, &j, &x)) {
                    expand_math(L, x.p ? x.p : "", x.n, o, depth + 1);
                }

                pb_free(&x);
                i = j;
                continue;
            }

            pb_put(o, s + i, e - i);
            i = e;
            continue;
        }

        if (s[i] == '\\' && i + 1 < n) {
            pb_put(o, s + i, 2);
            i += 2;
            continue;
        }

        pb_putc(o, s[i]);
        i++;
    }
}

/* ------------------------------------------------------------------ */
/* labels, bibliography                                               */
/* ------------------------------------------------------------------ */

static void add_label(lt* L, const char* key, size_t kn) {
    tlabel* t;

    if (!L->dry || kn == 0 || kn >= sizeof(t->key)) {
        return;     /* (the first time through only: the second has them all already) */
    }

    if (L->nlab == L->caplab) {
        int nc = L->caplab ? L->caplab * 2 : 64;
        tlabel* q = (tlabel*)realloc(L->lab, sizeof(tlabel) * (size_t)nc);

        if (!q) {
            return;
        }

        L->lab = q;
        L->caplab = nc;
    }

    t = &L->lab[L->nlab++];
    memcpy(t->key, key, kn);
    t->key[kn] = '\0';
    snprintf(t->num, sizeof(t->num), "%s", L->cur_num);
    snprintf(t->kind, sizeof(t->kind), "%s", L->cur_kind);
}

static const tlabel* find_label(const lt* L, const char* key, size_t kn) {
    int k;

    for (k = 0; k < L->nlab; k++) {
        if (strlen(L->lab[k].key) == kn && !strncmp(L->lab[k].key, key, kn)) {
            return &L->lab[k];
        }
    }

    return NULL;
}

static int bib_index(lt* L, const char* key, size_t kn, int add) {
    int k;

    for (k = 0; k < L->nbib; k++) {
        if (strlen(L->bib[k].key) == kn && !strncmp(L->bib[k].key, key, kn)) {
            return k;
        }
    }

    if (!add || kn >= sizeof(L->bib[0].key)) {
        return -1;
    }

    if (L->nbib == L->capbib) {
        int nc = L->capbib ? L->capbib * 2 : 64;
        tbib* q = (tbib*)realloc(L->bib, sizeof(L->bib[0]) * (size_t)nc);

        if (!q) {
            return -1;
        }

        L->bib = q;
        L->capbib = nc;
    }

    memset(&L->bib[L->nbib], 0, sizeof(L->bib[0]));
    memcpy(L->bib[L->nbib].key, key, kn);
    L->bib[L->nbib].key[kn] = '\0';
    return L->nbib++;
}

/* a key cited, the first time through: the order a .bib's entries are listed in */
static void note_cited(lt* L, const char* key, size_t kn) {
    int k;

    if (!L->dry || kn == 0 || kn >= sizeof(L->cited[0])) {
        return;
    }

    if (kn == 1 && key[0] == '*') {
        L->cite_all = 1;
        return;
    }

    for (k = 0; k < L->ncited; k++) {
        if (strlen(L->cited[k]) == kn && !strncmp(L->cited[k], key, kn)) {
            return;
        }
    }

    if (L->ncited == L->capcited) {
        int nc = L->capcited ? L->capcited * 2 : 64;
        char (*q)[80] = (char (*)[80])realloc(L->cited, sizeof(L->cited[0]) * (size_t)nc);

        if (!q) {
            return;
        }

        L->cited = q;
        L->capcited = nc;
    }

    memcpy(L->cited[L->ncited], key, kn);
    L->cited[L->ncited][kn] = '\0';
    L->ncited++;
}

/* each key of a list ("a, b,c"): fn with it */
static void each_key(lt* L, const char* s, size_t a, size_t b, void (*fn)(lt*, const char*, size_t)) {
    size_t i = a;

    while (i < b) {
        size_t e = i, ka = i, kb;

        while (e < b && s[e] != ',') {
            e++;
        }

        kb = e;

        while (ka < kb && isspace((unsigned char)s[ka])) {
            ka++;
        }

        while (kb > ka && isspace((unsigned char)s[kb - 1])) {
            kb--;
        }

        if (kb > ka) {
            fn(L, s + ka, kb - ka);
        }

        i = e + 1;
    }
}

static void set_label_target(lt* L, const char* num, const char* kind) {
    snprintf(L->cur_num, sizeof(L->cur_num), "%s", num);
    snprintf(L->cur_kind, sizeof(L->cur_kind), "%s", kind);
}

/* ------------------------------------------------------------------ */
/* output                                                             */
/* ------------------------------------------------------------------ */

static void apply_format(lt* L) {
    if (!L->dry) {
        bld_set_format(L->b, &L->cp);
    }
}

static void par_end(lt* L) {
    if (L->in_para && !L->dry) {
        bld_end_para(L->b);
    }

    L->in_para = 0;
    L->space_pending = 0;
}

static void par_begin(lt* L) {
    if (L->in_para) {
        return;
    }

    L->in_para = 1;
    L->space_pending = 0;
    L->line_start = 0;

    if (L->dry) {
        return;
    }

    bld_para_style(L->b, L->style, L->role, L->level);

    if (L->ldepth > 0 && L->item_pending && L->lkind[L->ldepth] != 3) {
        bld_list(L->b, L->lkind[L->ldepth], L->ldepth - 1);
        L->b->list_id = L->lid[L->ldepth];
    } else if (L->ldepth > 0) {
        /* a later paragraph of an item, or a description's: indented as the item's text */
        L->b->pp.mask |= PD_PP_INDENT_LEFT;
        L->b->pp.indent_left = PD_PT(18) * L->ldepth;
    }

    L->item_pending = 0;

    if (L->align >= 0) {
        L->b->pp.mask |= PD_PP_ALIGN;
        L->b->pp.align = L->align;
    } else if (!L->ragged && (L->role == PD_ROLE_BODY || L->role == PD_ROLE_QUOTE ||
                              L->role == PD_ROLE_DEFINITION || L->role == PD_ROLE_CAPTION)) {
        L->b->pp.mask |= PD_PP_ALIGN;   /* TeX justifies its text */
        L->b->pp.align = PD_ALIGN_JUSTIFY;
    }

    bld_begin_para(L->b);
    bld_set_format(L->b, &L->cp);
}

static void out_raw(lt* L, const char* t, size_t n) {
    if (n == 0) {
        return;
    }

    par_begin(L);
    L->line_start = 0;

    if (L->space_pending) {
        if (!L->dry) {
            bld_text(L->b, " ", 1);
        }

        L->space_pending = 0;
    }

    if (!L->dry) {
        bld_text(L->b, t, n);
    }
}

static void out_str(lt* L, const char* t) {
    out_raw(L, t, strlen(t));
}

static void out_space(lt* L) {
    if (L->in_para && !L->line_start) {
        L->space_pending = 1;
    }
}

/* the code point c as UTF-8 */
static void out_cp(lt* L, uint32_t c) {
    char u[4];
    size_t k = 0;

    if (c < 0x80) {
        u[k++] = (char)c;
    } else if (c < 0x800) {
        u[k++] = (char)(0xC0 | (c >> 6));
        u[k++] = (char)(0x80 | (c & 63));
    } else if (c < 0x10000) {
        u[k++] = (char)(0xE0 | (c >> 12));
        u[k++] = (char)(0x80 | ((c >> 6) & 63));
        u[k++] = (char)(0x80 | (c & 63));
    } else {
        u[k++] = (char)(0xF0 | (c >> 18));
        u[k++] = (char)(0x80 | ((c >> 12) & 63));
        u[k++] = (char)(0x80 | ((c >> 6) & 63));
        u[k++] = (char)(0x80 | (c & 63));
    }

    out_raw(L, u, k);
}

/* a line break in the paragraph */
static void out_newline(lt* L) {
    if (L->in_para) {
        L->space_pending = 0;

        if (!L->dry) {
            bld_text(L->b, "\n", 1);
        }

        L->line_start = 1;
    }
}

static void out_equation(lt* L, const char* src, size_t n, int display) {
    pd_buf x;
    pd_inline o;

    memset(&x, 0, sizeof(x));
    expand_math(L, src, n, &x, 0);

    /* blank space at its ends dropped: what the source had around it is not the formula's */
    while (x.n > 0 && (x.p[x.n - 1] == ' ' || x.p[x.n - 1] == '\n' || x.p[x.n - 1] == '\t')) {
        x.n--;
    }

    {
        size_t a = 0;

        while (a < x.n && (x.p[a] == ' ' || x.p[a] == '\n' || x.p[a] == '\t')) {
            a++;
        }

        if (a == x.n) {     /* nothing in it: no formula, no paragraph for one */
            pb_free(&x);
            return;
        }
    }

    if (display) {
        int32_t role = L->role, level = L->level, align = L->align;
        const char* style = L->style;

        par_end(L);
        L->role = PD_ROLE_EQUATION;
        L->style = NULL;
        L->level = 0;
        L->align = -1;
        par_begin(L);
        L->role = role;
        L->style = style;
        L->level = level;
        L->align = align;
    } else {
        par_begin(L);

        if (L->space_pending) {
            if (!L->dry) {
                bld_text(L->b, " ", 1);
            }

            L->space_pending = 0;
        }
    }

    if (!L->dry) {
        size_t a = 0;

        while (a < x.n && (x.p[a] == ' ' || x.p[a] == '\n' || x.p[a] == '\t')) {
            a++;
        }

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_EQUATION;
        o.source = x.p ? x.p + a : "";
        o.source_len = (int32_t)(x.n - a);
        o.width = pd_conv_equation_width(x.n - a);
        o.height = PD_PT(display ? 10 : 8);
        o.depth = PD_PT(2);

        if (o.source_len > 0) {
            bld_inline(L->b, &o);
        }
    }

    pb_free(&x);

    if (display) {
        par_end(L);
    }
}

/* ------------------------------------------------------------------ */
/* characters                                                         */
/* ------------------------------------------------------------------ */

/* a letter with an accent: the precomposed character where Unicode has one, else the letter and the mark */
static void out_accent(lt* L, char acc, const char* s, size_t a, size_t b) {
    static const struct { char acc, base; uint32_t cp; } T[] = {
        { '\'', 'a', 0xE1 }, { '\'', 'e', 0xE9 }, { '\'', 'i', 0xED }, { '\'', 'o', 0xF3 }, { '\'', 'u', 0xFA },
        { '\'', 'y', 0xFD }, { '\'', 'A', 0xC1 }, { '\'', 'E', 0xC9 }, { '\'', 'I', 0xCD }, { '\'', 'O', 0xD3 },
        { '\'', 'U', 0xDA }, { '\'', 'Y', 0xDD }, { '\'', 'c', 0x107 }, { '\'', 'C', 0x106 }, { '\'', 'n', 0x144 },
        { '\'', 'N', 0x143 }, { '\'', 's', 0x15B }, { '\'', 'S', 0x15A }, { '\'', 'z', 0x17A }, { '\'', 'Z', 0x179 },
        { '`', 'a', 0xE0 }, { '`', 'e', 0xE8 }, { '`', 'i', 0xEC }, { '`', 'o', 0xF2 }, { '`', 'u', 0xF9 },
        { '`', 'A', 0xC0 }, { '`', 'E', 0xC8 }, { '`', 'I', 0xCC }, { '`', 'O', 0xD2 }, { '`', 'U', 0xD9 },
        { '^', 'a', 0xE2 }, { '^', 'e', 0xEA }, { '^', 'i', 0xEE }, { '^', 'o', 0xF4 }, { '^', 'u', 0xFB },
        { '^', 'A', 0xC2 }, { '^', 'E', 0xCA }, { '^', 'I', 0xCE }, { '^', 'O', 0xD4 }, { '^', 'U', 0xDB },
        { '"', 'a', 0xE4 }, { '"', 'e', 0xEB }, { '"', 'i', 0xEF }, { '"', 'o', 0xF6 }, { '"', 'u', 0xFC },
        { '"', 'y', 0xFF }, { '"', 'A', 0xC4 }, { '"', 'E', 0xCB }, { '"', 'I', 0xCF }, { '"', 'O', 0xD6 },
        { '"', 'U', 0xDC }, { '~', 'a', 0xE3 }, { '~', 'n', 0xF1 }, { '~', 'o', 0xF5 }, { '~', 'A', 0xC3 },
        { '~', 'N', 0xD1 }, { '~', 'O', 0xD5 }, { 'c', 'c', 0xE7 }, { 'c', 'C', 0xC7 }, { 'c', 's', 0x15F },
        { 'c', 'S', 0x15E }, { 'v', 'c', 0x10D }, { 'v', 'C', 0x10C }, { 'v', 's', 0x161 }, { 'v', 'S', 0x160 },
        { 'v', 'z', 0x17E }, { 'v', 'Z', 0x17D }, { 'v', 'r', 0x159 }, { 'v', 'R', 0x158 }, { 'v', 'e', 0x11B },
        { 'v', 'E', 0x11A }, { 'v', 'n', 0x148 }, { 'v', 'N', 0x147 }, { '=', 'a', 0x101 }, { '=', 'e', 0x113 },
        { '=', 'i', 0x12B }, { '=', 'o', 0x14D }, { '=', 'u', 0x16B }, { 'u', 'g', 0x11F }, { 'u', 'G', 0x11E },
        { 'H', 'o', 0x151 }, { 'H', 'O', 0x150 }, { 'H', 'u', 0x171 }, { 'H', 'U', 0x170 }, { 'r', 'a', 0xE5 },
        { 'r', 'A', 0xC5 }, { 'r', 'u', 0x16F }, { 'k', 'a', 0x105 }, { 'k', 'e', 0x119 }, { '.', 'z', 0x17C },
        { '.', 'Z', 0x17B }, { '.', 'I', 0x130 }
    };
    static const struct { char acc; uint32_t mark; } M[] = {
        { '\'', 0x301 }, { '`', 0x300 }, { '^', 0x302 }, { '"', 0x308 }, { '~', 0x303 }, { '=', 0x304 },
        { '.', 0x307 }, { 'u', 0x306 }, { 'v', 0x30C }, { 'H', 0x30B }, { 'c', 0x327 }, { 'k', 0x328 },
        { 'r', 0x30A }, { 'd', 0x323 }, { 'b', 0x331 }, { 't', 0x361 }
    };
    char base = 0;
    size_t k;

    /* \i and \j: the dotless letters, as i and j take an accent */
    if (b - a == 2 && s[a] == '\\' && (s[a + 1] == 'i' || s[a + 1] == 'j')) {
        base = s[a + 1];
    } else if (b - a == 1) {
        base = s[a];
    }

    for (k = 0; base && k < sizeof(T) / sizeof(T[0]); k++) {
        if (T[k].acc == acc && T[k].base == base) {
            out_cp(L, T[k].cp);
            return;
        }
    }

    if (base) {
        out_raw(L, &base, 1);
    } else if (b > a) {
        run(L, s + a, b - a);
    }

    for (k = 0; k < sizeof(M) / sizeof(M[0]); k++) {
        if (M[k].acc == acc) {
            out_cp(L, M[k].mark);
            return;
        }
    }
}

/* a command that is a character (or a few), 0 for none */
static uint32_t symbol(const char* name, size_t n) {
    static const struct { const char* name; uint32_t cp; } T[] = {
        { "ss", 0xDF }, { "ae", 0xE6 }, { "AE", 0xC6 }, { "oe", 0x153 }, { "OE", 0x152 }, { "o", 0xF8 },
        { "O", 0xD8 }, { "aa", 0xE5 }, { "AA", 0xC5 }, { "l", 0x142 }, { "L", 0x141 }, { "i", 0x131 },
        { "j", 0x237 }, { "dag", 0x2020 }, { "ddag", 0x2021 }, { "S", 0xA7 }, { "P", 0xB6 },
        { "copyright", 0xA9 }, { "textcopyright", 0xA9 }, { "pounds", 0xA3 }, { "textsterling", 0xA3 },
        { "textregistered", 0xAE }, { "texttrademark", 0x2122 }, { "textdegree", 0xB0 }, { "degree", 0xB0 },
        { "texttimes", 0xD7 }, { "textendash", 0x2013 }, { "textemdash", 0x2014 }, { "textbullet", 0x2022 },
        { "textquoteleft", 0x2018 }, { "textquoteright", 0x2019 }, { "textquotedblleft", 0x201C },
        { "textquotedblright", 0x201D }, { "ldots", 0x2026 }, { "dots", 0x2026 }, { "textellipsis", 0x2026 },
        { "textbackslash", '\\' }, { "textasciitilde", '~' }, { "textasciicircum", '^' }, { "textbar", '|' },
        { "textless", '<' }, { "textgreater", '>' }, { "textunderscore", '_' }, { "euro", 0x20AC },
        { "texteuro", 0x20AC }, { "textmu", 0xB5 }, { "textpm", 0xB1 }, { "textonehalf", 0xBD },
        { "guillemotleft", 0xAB }, { "guillemotright", 0xBB }, { "textexclamdown", 0xA1 },
        { "textquestiondown", 0xBF }, { "checkmark", 0x2713 }, { "textperthousand", 0x2030 },
        { "quad", 0x2003 }, { "qquad", 0x2003 }, { "enspace", 0x2002 }, { "thinspace", 0x2009 },
        { "nobreakspace", 0xA0 }, { "textvisiblespace", 0x2423 }, { "textdagger", 0x2020 },
        { "textdaggerdbl", 0x2021 }, { "textsection", 0xA7 }, { "textparagraph", 0xB6 }
    };
    size_t k;

    for (k = 0; k < sizeof(T) / sizeof(T[0]); k++) {
        if (strlen(T[k].name) == n && !strncmp(T[k].name, name, n)) {
            return T[k].cp;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* numbers                                                            */
/* ------------------------------------------------------------------ */

/* the number of sectioning level k (0 chapter ... 5 subparagraph) as LaTeX prints it ("2.1", "A.3") */
static void sec_number(const lt* L, int k, char* buf, size_t cap) {
    int from = L->has_chapter ? 0 : 1, j;
    size_t w = 0;

    buf[0] = '\0';

    for (j = from; j <= k && w + 12 < cap; j++) {
        if (j == from && L->appendix) {
            w += (size_t)snprintf(buf + w, cap - w, "%c", 'A' + (L->sec[j] - 1) % 26);
        } else {
            w += (size_t)snprintf(buf + w, cap - w, "%s%d", j > from ? "." : "", L->sec[j]);
        }
    }
}

/* a float's or an equation's number: "3", or "2.3" in a book's chapter 2 */
static void item_number(const lt* L, int v, char* buf, size_t cap) {
    if (L->has_chapter && L->sec[0] > 0) {
        if (L->appendix) {
            snprintf(buf, cap, "%c.%d", 'A' + (L->sec[0] - 1) % 26, v);
        } else {
            snprintf(buf, cap, "%d.%d", L->sec[0], v);
        }
    } else {
        snprintf(buf, cap, "%d", v);
    }
}

/* ------------------------------------------------------------------ */
/* the parts                                                          */
/* ------------------------------------------------------------------ */

static const char* const SECTIONS[] = { "chapter", "section", "subsection", "subsubsection", "paragraph",
                                        "subparagraph" };

static void heading(lt* L, int k, int star, const char* s, size_t a, size_t b) {
    char num[40], style[24];
    int level = L->has_chapter ? k + 1 : k;
    int j;
    const char* keep = L->style;
    int32_t role = L->role, lvl = L->level, align = L->align;

    if (level < 1) {
        level = 1;
    }

    par_end(L);

    if (!star) {
        L->sec[k]++;

        for (j = k + 1; j < 6; j++) {
            L->sec[j] = 0;
        }

        if (k == 0) {   /* a chapter: its floats and equations counted from 1 */
            L->fig = L->tab = L->eq = 0;
        }

        sec_number(L, k, num, sizeof(num));
        set_label_target(L, num, k == 0 ? "chapter" : "section");
    }

    snprintf(style, sizeof(style), "Heading %d", level > 6 ? 6 : level);
    L->style = style;
    L->role = PD_ROLE_HEADING;
    L->level = level > 6 ? 6 : level;
    L->align = -1;

    if (k >= 4) {   /* \paragraph: a run-in heading, as a bold paragraph of its own here */
        L->style = NULL;
        L->role = PD_ROLE_BODY;
        L->level = 0;
    }

    {
        pd_char_props was = L->cp;

        if (k >= 4) {
            L->cp.mask |= PD_CP_WEIGHT;
            L->cp.weight = 700;
        }

        if (!star && k < 4) {
            if (k == 0) {
                out_str(L, L->appendix ? "Appendix " : "Chapter ");
            }

            out_str(L, num);
            out_str(L, "  ");
        }

        run(L, s + a, b - a);
        par_end(L);
        L->cp = was;
        apply_format(L);
    }

    L->style = keep;
    L->role = role;
    L->level = lvl;
    L->align = align;
}

/* a subfigure begun: its letter, what a \label in it names ("3(b)", the figure's number to come) */
static void sub_begin(lt* L) {
    char num[48], fig[32];

    L->sub_count++;
    item_number(L, (L->in_float == 2 ? L->tab : L->fig) + 1, fig, sizeof(fig));
    snprintf(num, sizeof(num), "%s(%c)", fig, 'a' + (L->sub_count - 1) % 26);
    set_label_target(L, num, L->in_float == 2 ? "table" : "figure");
}

static void caption(lt* L, const char* s, size_t a, size_t b) {
    char num[32];
    const char* keep = L->style;
    int32_t role = L->role, align = L->align;

    if (L->in_sub) {    /* a subfigure's: "(a) ..." under its picture, the figure's number not counted */
        out_newline(L);

        if (b > a) {
            out_cp(L, '(');
            out_cp(L, (uint32_t)('a' + (L->sub_count - 1) % 26));
            out_str(L, ") ");
            run(L, s + a, b - a);
        }

        return;
    }

    par_end(L);
    L->sub_count = 0;

    if (L->in_float == 2) {
        item_number(L, ++L->tab, num, sizeof(num));
        set_label_target(L, num, "table");
    } else {
        item_number(L, ++L->fig, num, sizeof(num));
        set_label_target(L, num, "figure");
    }

    L->style = "Caption";
    L->role = PD_ROLE_CAPTION;
    L->align = PD_ALIGN_CENTER;
    out_str(L, L->in_float == 2 ? "Table " : "Figure ");
    out_str(L, num);
    out_str(L, ": ");
    run(L, s + a, b - a);
    par_end(L);
    L->style = keep;
    L->role = role;
    L->align = align;
}

/* a length as points: "3cm", "0.5\textwidth" (of a 6 in text), "2in"; 0 when it is not one */
static double length_pt(const char* s, size_t n) {
    char buf[64];
    double v;
    char* e;

    if (n >= sizeof(buf)) {
        return 0;
    }

    memcpy(buf, s, n);
    buf[n] = '\0';
    v = strtod(buf, &e);

    if (e == buf) {
        v = 1;      /* \textwidth alone */
    }

    while (*e == ' ') {
        e++;
    }

    if (!strncmp(e, "\\textwidth", 10) || !strncmp(e, "\\linewidth", 10) || !strncmp(e, "\\columnwidth", 12) ||
        !strncmp(e, "\\hsize", 6)) {
        return v * 432;
    }

    if (!strncmp(e, "\\textheight", 11)) {
        return v * 600;
    }

    if (!strncmp(e, "cm", 2)) {
        return v * 72 / 2.54;
    }

    if (!strncmp(e, "mm", 2)) {
        return v * 72 / 25.4;
    }

    if (!strncmp(e, "in", 2)) {
        return v * 72;
    }

    if (!strncmp(e, "pt", 2) || !strncmp(e, "bp", 2)) {
        return v;
    }

    if (!strncmp(e, "em", 2)) {
        return v * 10;
    }

    if (!strncmp(e, "ex", 2)) {
        return v * 4.3;
    }

    return 0;
}

/* a key=value option's value ("width=0.5\linewidth") */
static int opt_value(const char* s, size_t a, size_t b, const char* key, size_t* va, size_t* vb) {
    size_t kl = strlen(key), i = a;

    while (i < b) {
        size_t e = i;
        int d = 0;

        while (e < b && (s[e] != ',' || d > 0)) {
            d += s[e] == '{' ? 1 : s[e] == '}' ? -1 : 0;
            e++;
        }

        {
            size_t p = i, q;

            while (p < e && s[p] == ' ') {
                p++;
            }

            if (e - p > kl && !strncmp(s + p, key, kl)) {
                q = p + kl;

                while (q < e && s[q] == ' ') {
                    q++;
                }

                if (q < e && s[q] == '=') {
                    q++;

                    while (q < e && s[q] == ' ') {
                        q++;
                    }

                    *va = q;
                    *vb = e;

                    while (*vb > *va && s[*vb - 1] == ' ') {
                        (*vb)--;
                    }

                    if (*vb > *va && s[*va] == '{' && s[*vb - 1] == '}') {
                        (*va)++;
                        (*vb)--;
                    }

                    return 1;
                }
            }
        }

        i = e + 1;
    }

    return 0;
}

static void picture(lt* L, const char* s, size_t oa, size_t ob, int has_opt, size_t fa, size_t fb) {
    pd_inline o;
    size_t va, vb;
    double w = 0, h = 0;

    if (has_opt) {
        if (opt_value(s, oa, ob, "width", &va, &vb)) {
            w = length_pt(s + va, vb - va);
        }

        if (opt_value(s, oa, ob, "height", &va, &vb)) {
            h = length_pt(s + va, vb - va);
        }

    }

    while (fa < fb && s[fa] == ' ') {
        fa++;
    }

    while (fb > fa && s[fb - 1] == ' ') {
        fb--;
    }

    if (L->in_float && !L->in_sub && !L->sub_count) {
        par_end(L);
    }

    par_begin(L);

    if ((L->space_pending || (L->sub_count && !L->line_start && L->in_para)) && !L->dry) {
        bld_text(L->b, " ", 1);
    }

    L->space_pending = 0;

    if (!L->dry && fb > fa) {
        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_IMAGE;
        o.source = s + fa;      /* by address: the host loads it (pd_doc_load_images) */
        o.source_len = (int32_t)(fb - fa);
        o.width = w > 0 ? (pd_sp)(w * 65536) : 0;
        o.height = h > 0 ? (pd_sp)(h * 65536) : 0;

        bld_inline(L->b, &o);
    }

    L->line_start = 0;

    if (L->in_float && !L->in_sub && !L->sub_count) {
        par_end(L);
    }
}

/* a list's definition: LaTeX's labels at its depth (1. (a) i. A.; bullet, dash, asterisk, dot) */
static pd_list_id list_def(lt* L, int kind, int depth) {
    pd_list_level lv[9];
    pd_list_id id = 0;
    int i;
    static const char* bullets[4] = { "\xE2\x80\xA2", "\xE2\x80\x93", "\xE2\x88\x97", "\xC2\xB7" };
    static const int fmt[4] = { PD_NUM_DECIMAL, PD_NUM_LOWER_ALPHA, PD_NUM_LOWER_ROMAN, PD_NUM_UPPER_ALPHA };

    if (L->dry) {
        return 0;
    }

    memset(lv, 0, sizeof(lv));

    for (i = 0; i < 9; i++) {
        int d = i % 4;

        lv[i].indent = PD_PT(18) * (i + 1);
        lv[i].hanging = PD_PT(18);
        lv[i].start = 1;

        if (kind == 1) {
            lv[i].format = PD_NUM_BULLET;
            strcpy(lv[i].text, bullets[d]);
        } else {
            lv[i].format = fmt[d];
            snprintf(lv[i].text, sizeof(lv[i].text), d == 1 ? "(%%%d)" : "%%%d.", i + 1);
        }
    }

    (void)depth;
    pd_doc_list_define(L->b->d, 9, lv, &id);
    return id;
}

/* ---- tables ---- */

/* a tabular's rows and cells: cut at & and \\ outside braces and inner environments */
static void table_body(lt* L, const char* s, size_t n, const char* spec, size_t specn) {
    size_t i = 0, cell_a = 0;
    int d = 0, env = 0, row_open = 0, row = 0, col = 0, head_rows = -1;
    char aligns[64];
    int na = 0;
    size_t k;

    /* the columns' alignment: l c r (p{} m{} b{} X as left) */
    for (k = 0; k < specn && na < 63; k++) {
        char c = spec[k];

        if (c == '{') {
            size_t e = group_end(spec, specn, k);

            k = e;
            continue;
        }

        if (c == 'l' || c == 'p' || c == 'm' || c == 'b' || c == 'X' || c == 'L') {
            aligns[na++] = 'l';
        } else if (c == 'c' || c == 'C') {
            aligns[na++] = 'c';
        } else if (c == 'r' || c == 'R') {
            aligns[na++] = 'r';
        }
    }

    /* booktabs: the rows above \midrule are the head */
    {
        const char* mid = NULL, *p;

        for (p = s; (size_t)(p - s) + 8 <= n; p++) {
            if (*p == '\\' && !strncmp(p, "\\midrule", 8)) {
                mid = p;
                break;
            }
        }

        if (mid) {
            head_rows = 0;

            for (p = s; p < mid; p++) {
                if (p[0] == '\\' && p[1] == '\\') {
                    head_rows++;
                    p++;
                }
            }
        }
    }

    if (!L->dry) {
        bld_table_begin(L->b);
    }

    for (i = 0; i <= n; i++) {
        int end_cell = i == n, end_row = 0;

        if (i < n) {
            if (s[i] == '%') {
                while (i < n && s[i] != '\n') {
                    i++;
                }

                continue;
            }

            if (s[i] == '\\' && i + 1 < n) {
                if (!strncmp(s + i, "\\begin", 6)) {
                    env++;
                } else if (!strncmp(s + i, "\\end", 4)) {
                    env--;
                } else if (s[i + 1] == '\\' && d == 0 && env == 0) {
                    end_cell = end_row = 1;
                }

                if (!end_row) {
                    i++;
                    continue;
                }
            } else if (s[i] == '{') {
                d++;
            } else if (s[i] == '}') {
                d--;
            } else if (s[i] == '&' && d == 0 && env == 0) {
                end_cell = 1;
            }
        }

        if (!end_cell) {
            continue;
        }

        {
            size_t ca = cell_a, cb = i, x, y;
            int span = 1, all_space = 1;
            char al = col < na ? aligns[col] : 'l';

            /* the cell's own rules and spacing left out; \multicolumn{n}{spec}{text} spans */
            for (x = ca; x < cb; x++) {
                if (!isspace((unsigned char)s[x])) {
                    all_space = 0;
                    break;
                }
            }

            {
                size_t j = ca;

                skip_ws(s, cb, &j);

                while (j < cb && s[j] == '\\') {
                    size_t e = j + 1, a, b;

                    while (e < cb && is_letter(s[e])) {
                        e++;
                    }

                    if ((e - j == 6 && !strncmp(s + j, "\\hline", 6)) ||
                        (e - j == 8 && (!strncmp(s + j, "\\toprule", 8) || !strncmp(s + j, "\\midrule", 8))) ||
                        (e - j == 11 && !strncmp(s + j, "\\bottomrule", 11))) {
                        j = e;
                    } else if ((e - j == 6 && !strncmp(s + j, "\\cline", 6)) ||
                               (e - j == 8 && !strncmp(s + j, "\\cmidrule", 9))) {
                        j = e;
                        read_opt(s, cb, &j, &a, &b);
                        read_arg(s, cb, &j, &a, &b);
                    } else {
                        break;
                    }

                    skip_ws(s, cb, &j);
                }

                ca = j;

                if (cb - ca > 12 && !strncmp(s + ca, "\\multicolumn", 12)) {
                    size_t j2 = ca + 12, a1, b1, a2, b2, a3, b3;

                    if (read_arg(s, cb, &j2, &a1, &b1) && read_arg(s, cb, &j2, &a2, &b2) &&
                        read_arg(s, cb, &j2, &a3, &b3)) {
                        char nb[8];

                        if (b1 - a1 < sizeof(nb)) {
                            memcpy(nb, s + a1, b1 - a1);
                            nb[b1 - a1] = '\0';
                            span = atoi(nb) > 0 ? atoi(nb) : 1;
                        }

                        for (y = a2; y < b2; y++) {
                            if (s[y] == 'c' || s[y] == 'l' || s[y] == 'r') {
                                al = s[y];
                                break;
                            }
                        }

                        ca = a3;
                        cb = b3;
                    }
                }
            }

            /* a row of nothing but rules: none at all */
            if (!(end_row && col == 0 && (all_space || ca >= cb) && !row_open && i + 2 < n)) {
                if (!(i == n && col == 0 && !row_open && (all_space || ca >= cb))) {
                    if (!row_open) {
                        if (!L->dry) {
                            bld_row_begin(L->b, head_rows >= 0 ? row < head_rows : 0);
                        }

                        row_open = 1;
                    }

                    if (!L->dry) {
                        bld_cell_begin(L->b, span, 0);
                    }

                    {
                        int32_t align = L->align;
                        int keep_in = L->in_para;

                        L->in_para = 0;
                        L->align = al == 'c' ? PD_ALIGN_CENTER : al == 'r' ? PD_ALIGN_RIGHT : PD_ALIGN_LEFT;
                        run(L, s + ca, cb - ca);
                        par_end(L);
                        L->align = align;
                        L->in_para = keep_in;
                    }

                    if (!L->dry) {
                        bld_cell_end(L->b);
                    }

                    col += span;
                }
            }
        }

        if (end_row) {
            if (row_open) {
                row++;
            }

            row_open = 0;
            col = 0;
            i++;    /* past the second backslash */

            /* \\[2pt]: its spacing left out */
            if (i + 1 < n && s[i + 1] == '[') {
                size_t j = i + 1, a, b;

                read_opt(s, n, &j, &a, &b);
                i = j - 1;
            }
        }

        cell_a = i + 1;
    }

    if (!L->dry) {
        int last = L->b->ntables - 1;
        pd_block_id tbl = last >= 0 && last < 8 ? L->b->table[last] : 0;

        bld_table_end(L->b);

        /* in a centred float or \begin{center}: the table in the middle, as LaTeX sets it */
        if (tbl && L->align == PD_ALIGN_CENTER) {
            pd_table_props tp;

            if (pd_doc_table_props(L->b->d, tbl, &tp) == PD_OK) {
                tp.align = PD_ALIGN_CENTER;
                pd_doc_set_table_props(L->b->d, tbl, &tp);
            }
        }
    }
}

/* ---- the environments ---- */

/* a display formula's environment: its numbers (one per line for align and its kin, unless \nonumber), its
   labels, and the formula */
static void math_env(lt* L, const char* name, const char* s, size_t a, size_t b) {
    int numbered = name[strlen(name) - 1] != '*' && strcmp(name, "displaymath") != 0;
    int multi = !strncmp(name, "align", 5) || !strncmp(name, "gather", 6) || !strncmp(name, "eqnarray", 8) ||
                !strncmp(name, "flalign", 7) || !strncmp(name, "alignat", 7);
    size_t i, line_a = a;
    pd_buf src;

    memset(&src, 0, sizeof(src));

    if (!strncmp(name, "multline", 8)) {
        multi = 0;
    }

    /* each line's number and labels */
    for (i = a; i <= b; i++) {
        if (i == b || (multi && s[i] == '\\' && i + 1 < b && s[i + 1] == '\\')) {
            int nonum = 0;
            size_t j;

            for (j = line_a; j + 5 < i; j++) {
                if (s[j] == '\\' && (!strncmp(s + j, "\\nonumber", 9) || !strncmp(s + j, "\\notag", 6))) {
                    nonum = 1;
                }
            }

            if (numbered && !nonum && (i < b || line_a < b)) {
                char num[32];

                item_number(L, ++L->eq, num, sizeof(num));
                set_label_target(L, num, "equation");
            }

            for (j = line_a; j + 6 < i; j++) {
                if (s[j] == '\\' && !strncmp(s + j, "\\label", 6)) {
                    size_t q = j + 6, ka, kb;

                    if (read_arg(s, i, &q, &ka, &kb)) {
                        add_label(L, s + ka, kb - ka);
                    }
                }
            }

            line_a = i + 2;
            i++;
        }
    }

    /* the formula: align's lines as aligned's, gather's as gathered's */
    if (multi || !strncmp(name, "multline", 8)) {
        const char* inner = !strncmp(name, "gather", 6) ? "gathered" : "aligned";

        pb_printf(&src, "\\begin{%s}", inner);
        pb_put(&src, s + a, b - a);
        pb_printf(&src, "\\end{%s}", inner);
    } else {
        pb_put(&src, s + a, b - a);
    }

    out_equation(L, src.p ? src.p : "", src.n, 1);
    pb_free(&src);
}

static void verbatim_block(lt* L, const char* s, size_t a, size_t b) {
    const char* keep = L->style;
    int32_t role = L->role, align = L->align;
    pd_char_props was = L->cp;
    size_t i = a, ls;

    par_end(L);
    L->style = "Code";
    L->role = PD_ROLE_CODE;
    L->align = -1;

    /* the line the \begin is on ends there */
    if (i < b && s[i] == '\n') {
        i++;
    }

    while (i < b) {
        ls = i;

        while (i < b && s[i] != '\n') {
            i++;
        }

        par_begin(L);

        if (!L->dry && i > ls) {
            bld_text(L->b, s + ls, i - ls);
        }

        par_end(L);
        i++;
    }

    L->style = keep;
    L->role = role;
    L->align = align;
    L->cp = was;
    apply_format(L);
}

static void bibliography(lt* L, const char* s, size_t a, size_t b) {
    size_t i = a;
    int count = 0;
    const char* keep = L->style;
    int32_t role = L->role;

    par_end(L);
    L->style = "Heading 1";     /* a \chapter* in a book, a \section* in an article: the top level either way */
    L->role = PD_ROLE_HEADING;
    L->level = 1;
    out_str(L, L->has_chapter ? "Bibliography" : "References");
    par_end(L);
    L->style = keep;
    L->role = role;
    L->level = 0;

    /* each \bibitem's text, from it to the next */
    while (i < b) {
        size_t at = i, next, ka, kb, oa = 0, ob = 0;
        char num[40];
        int k;

        while (at + 8 <= b && strncmp(s + at, "\\bibitem", 8)) {
            at++;
        }

        if (at + 8 > b) {
            break;
        }

        i = at + 8;

        if (!read_opt(s, b, &i, &oa, &ob)) {
            oa = ob = 0;
        }

        if (!read_arg(s, b, &i, &ka, &kb)) {
            break;
        }

        next = i;

        while (next + 8 <= b && strncmp(s + next, "\\bibitem", 8)) {
            next++;
        }

        if (next + 8 > b) {
            next = b;
        }

        count++;
        k = bib_index(L, s + ka, kb - ka, 1);

        if (k >= 0 && ob > oa) {
            tbib* t = &L->bib[k];
            const char* lp = memchr(s + oa, '(', ob - oa);

            if (lp) {     /* natbib: Smith et al.(2020) */
                const char* rp = memchr(lp, ')', (size_t)(s + ob - lp));
                size_t wn = (size_t)(lp - (s + oa)), yn = rp ? (size_t)(rp - lp - 1) : 0;

                snprintf(t->who, sizeof(t->who), "%.*s", (int)wn, s + oa);
                snprintf(t->year, sizeof(t->year), "%.*s", (int)yn, lp + 1);
            } else if (ob - oa < sizeof(t->label)) {
                memcpy(t->label, s + oa, ob - oa);
                t->label[ob - oa] = '\0';
            }
        }

        if (k >= 0 && L->bib[k].label[0]) {
            snprintf(num, sizeof(num), "[%s] ", L->bib[k].label);
        } else {
            snprintf(num, sizeof(num), "[%d] ", k + 1);
        }

        out_str(L, num);
        L->line_start = 1;      /* (the blanks after the key: not the entry's) */
        run(L, s + i, next - i);
        par_end(L);
        i = next;
    }
}

static void list_env(lt* L, int kind, const char* s, size_t a, size_t b) {
    par_end(L);

    if (L->ldepth >= 9) {
        run(L, s + a, b - a);
        return;
    }

    L->ldepth++;
    L->lkind[L->ldepth] = kind;
    L->lid[L->ldepth] = kind == 3 ? 0 : list_def(L, kind, L->ldepth - 1);
    L->item_count[L->ldepth] = 0;
    run(L, s + a, b - a);
    par_end(L);
    L->item_pending = 0;
    L->ldepth--;
}

static void title_block(lt* L) {
    const char* keep = L->style;
    int32_t role = L->role, align = L->align;
    pd_char_props was = L->cp;

    par_end(L);

    if (L->title) {
        L->style = "Title";
        L->role = PD_ROLE_TITLE;
        L->align = PD_ALIGN_CENTER;
        run(L, L->title, strlen(L->title));
        par_end(L);
    }

    L->style = NULL;
    L->role = PD_ROLE_BODY;
    L->align = PD_ALIGN_CENTER;

    if (L->author) {   /* each author (\and between them) a line of the paragraph */
        const char* p = L->author, *q;

        L->cp.mask |= PD_CP_SIZE;
        L->cp.size = PD_PT(12);
        apply_format(L);

        while (p && *p) {
            q = strstr(p, "\\and");

            while (q && is_letter(q[4])) {
                q = strstr(q + 4, "\\and");
            }

            run(L, p, q ? (size_t)(q - p) : strlen(p));

            if (!q) {
                break;
            }

            out_newline(L);
            p = q + 4;
        }

        par_end(L);
    }

    if (L->date && L->date[0]) {
        run(L, L->date, strlen(L->date));
        par_end(L);
    }

    L->cp = was;
    apply_format(L);
    L->style = keep;
    L->role = role;
    L->align = align;
}

static void environment(lt* L, const char* s, size_t n, size_t* i) {
    size_t na, nb, ca, ce, after;
    char name[48];
    int32_t align = L->align;
    int ragged = L->ragged;
    pd_char_props was = L->cp;

    if (!read_arg(s, n, i, &na, &nb) || nb - na >= sizeof(name)) {
        return;
    }

    memcpy(name, s + na, nb - na);
    name[nb - na] = '\0';

    /* a .bib file's text, as led puts it where \bibliography names it: its entries set out as BibTeX would */
    if (!strcmp(name, "parade-bibtex")) {
        after = verb_end(s, n, *i, name, &ce);

        if (L->dry && !L->bibdata) {
            L->bibdata = dup_range(s, *i, ce);
            L->bibdata_len = L->bibdata ? ce - *i : 0;
        } else if (!L->dry && L->bibgen && !L->bib_shown) {
            size_t j = 0, a, b;

            L->bib_shown = 1;
            read_arg(L->bibgen, L->bibgen_len, &j, &a, &b);
            bibliography(L, L->bibgen, j, L->bibgen_len);
        }

        *i = after;
        return;
    }

    /* verbatim: raw to its end */
    if (!strcmp(name, "verbatim") || !strcmp(name, "verbatim*") || !strcmp(name, "lstlisting") ||
        !strcmp(name, "Verbatim") || !strcmp(name, "minted") || !strcmp(name, "comment") ||
        !strcmp(name, "BVerbatim")) {
        size_t oa, ob;

        if (strcmp(name, "verbatim") && strcmp(name, "verbatim*")) {
            read_opt(s, n, i, &oa, &ob);
        }

        if (!strcmp(name, "minted")) {
            read_arg(s, n, i, &oa, &ob);
        }

        after = verb_end(s, n, *i, name, &ce);

        if (strcmp(name, "comment")) {
            verbatim_block(L, s, *i, ce);
        }

        *i = after;
        return;
    }

    ca = *i;
    after = env_end(s, n, ca, name, &ce);
    *i = after;

    /* a document's own environment */
    {
        tmacro* m = find_macro(L, name, strlen(name), 1);

        if (m && L->depth < LX_MAX_DEPTH) {
            pd_buf x;
            size_t j = ca;

            memset(&x, 0, sizeof(x));

            if (expand_macro(L, m, s, ce, &j, &x)) {
                if (j < ce) {
                    pb_put(&x, s + j, ce - j);
                }

                if (m->end_body) {
                    pb_puts(&x, m->end_body);
                }

                L->depth++;
                run(L, x.p ? x.p : "", x.n);
                L->depth--;
            }

            pb_free(&x);
            L->cp = was;
            apply_format(L);
            return;
        }
    }

    if (!strcmp(name, "document")) {
        L->in_document = 1;
        run(L, s + ca, ce - ca);
        par_end(L);
    } else if (!strcmp(name, "itemize")) {
        list_env(L, 1, s, ca, ce);
    } else if (!strcmp(name, "enumerate")) {
        size_t oa, ob, j = ca;

        if (read_opt(s, ce, &j, &oa, &ob)) {
            ca = j;
        }

        list_env(L, 2, s, ca, ce);
    } else if (!strcmp(name, "description")) {
        list_env(L, 3, s, ca, ce);
    } else if (!strcmp(name, "equation") || !strcmp(name, "equation*") || !strncmp(name, "align", 5) ||
               !strncmp(name, "gather", 6) || !strncmp(name, "multline", 8) || !strncmp(name, "eqnarray", 8) ||
               !strncmp(name, "flalign", 7) || !strcmp(name, "displaymath") || !strcmp(name, "math")) {
        if (!strcmp(name, "math")) {
            out_equation(L, s + ca, ce - ca, 0);
        } else {
            size_t j = ca, a, b;

            if (!strncmp(name, "alignat", 7)) {
                read_arg(s, ce, &j, &a, &b);
            }

            math_env(L, name, s, j, ce);
        }
    } else if ((!strcmp(name, "subfigure") || !strcmp(name, "subtable")) && L->in_float) {
        size_t oa, ob, j = ca;
        int keep = L->in_sub;

        read_opt(s, ce, &j, &oa, &ob);
        read_arg(s, ce, &j, &oa, &ob);   /* its width */
        sub_begin(L);
        L->in_sub = 1;
        run(L, s + j, ce - j);
        L->in_sub = keep;
        out_raw(L, " ", 1);
    } else if (!strcmp(name, "figure") || !strcmp(name, "figure*") || !strcmp(name, "table") ||
               !strcmp(name, "table*") || !strcmp(name, "wrapfigure") || !strcmp(name, "SCfigure") ||
               !strcmp(name, "sidewaysfigure") || !strcmp(name, "sidewaystable")) {
        size_t oa, ob, j = ca;
        int kind = (name[0] == 't' || !strcmp(name, "sidewaystable")) ? 2 : 1, keep = L->in_float;

        if (!strcmp(name, "wrapfigure")) {
            read_opt(s, ce, &j, &oa, &ob);
            read_arg(s, ce, &j, &oa, &ob);
        } else {
            read_opt(s, ce, &j, &oa, &ob);
        }

        par_end(L);
        L->in_float = kind;
        L->sub_count = 0;
        L->align = PD_ALIGN_CENTER;
        run(L, s + j, ce - j);
        par_end(L);
        L->in_float = keep;
        L->sub_count = 0;
    } else if (!strcmp(name, "tabular") || !strcmp(name, "tabular*") || !strcmp(name, "tabularx") ||
               !strcmp(name, "longtable") || !strcmp(name, "tabulary") || !strcmp(name, "array") ||
               !strcmp(name, "supertabular") || !strcmp(name, "tblr")) {
        size_t j = ca, sa = 0, sb = 0, oa, ob;

        if (!strcmp(name, "tabular*") || !strcmp(name, "tabularx") || !strcmp(name, "tabulary")) {
            read_arg(s, ce, &j, &oa, &ob);   /* its width */
        }

        read_opt(s, ce, &j, &oa, &ob);
        read_arg(s, ce, &j, &sa, &sb);
        par_end(L);
        table_body(L, s + j, ce - j, s + sa, sb - sa);
    } else if (!strcmp(name, "quote") || !strcmp(name, "quotation") || !strcmp(name, "verse")) {
        const char* keep = L->style;
        int32_t role = L->role;

        par_end(L);
        L->style = "Quote";
        L->role = PD_ROLE_QUOTE;
        run(L, s + ca, ce - ca);
        par_end(L);
        L->style = keep;
        L->role = role;
    } else if (!strcmp(name, "center") || !strcmp(name, "flushleft") || !strcmp(name, "flushright")) {
        par_end(L);
        L->align = name[0] == 'c' ? PD_ALIGN_CENTER : name[5] == 'l' ? PD_ALIGN_LEFT : PD_ALIGN_RIGHT;
        run(L, s + ca, ce - ca);
        par_end(L);
    } else if (!strcmp(name, "abstract")) {
        pd_char_props keep = L->cp;

        par_end(L);
        L->align = PD_ALIGN_CENTER;
        L->cp.mask |= PD_CP_WEIGHT;
        L->cp.weight = 700;
        out_str(L, "Abstract");
        par_end(L);
        L->cp = keep;
        L->align = align;
        apply_format(L);
        {
            const char* ks = L->style;
            int32_t role = L->role;

            L->style = "Quote";
            L->role = PD_ROLE_QUOTE;
            run(L, s + ca, ce - ca);
            par_end(L);
            L->style = ks;
            L->role = role;
        }
    } else if (!strcmp(name, "thebibliography")) {
        size_t j = ca, a, b;

        read_arg(s, ce, &j, &a, &b);
        bibliography(L, s, j, ce);
    } else if (!strcmp(name, "tikzpicture") || !strcmp(name, "pgfpicture") || !strcmp(name, "picture") ||
               !strcmp(name, "axis")) {
        out_str(L, "[picture]");
    } else if (!strcmp(name, "minipage") || !strcmp(name, "subfigure")) {
        size_t j = ca, a, b;

        read_opt(s, ce, &j, &a, &b);
        read_arg(s, ce, &j, &a, &b);
        run(L, s + j, ce - j);
    } else if (!strcmp(name, "theorem") || !strcmp(name, "lemma") || !strcmp(name, "proof") ||
               !strcmp(name, "definition") || !strcmp(name, "proposition") || !strcmp(name, "corollary") ||
               !strcmp(name, "remark") || !strcmp(name, "example")) {
        pd_char_props keep = L->cp;
        size_t j = ca, a, b;
        int has = read_opt(s, ce, &j, &a, &b);
        char head[48];

        par_end(L);
        snprintf(head, sizeof(head), "%c%s", toupper((unsigned char)name[0]), name + 1);
        L->cp.mask |= PD_CP_WEIGHT | PD_CP_ITALIC;
        L->cp.weight = 700;
        L->cp.italic = !strcmp(name, "proof");
        apply_format(L);
        out_str(L, head);

        if (has) {
            out_str(L, " (");
            run(L, s + a, b - a);
            out_str(L, ")");
        }

        out_str(L, ". ");
        L->cp = keep;

        if (strcmp(name, "proof") && strcmp(name, "definition") && strcmp(name, "remark") &&
            strcmp(name, "example")) {
            L->cp.mask |= PD_CP_ITALIC;
            L->cp.italic = 1;
        }

        apply_format(L);
        run(L, s + j, ce - j);

        if (!strcmp(name, "proof")) {
            out_cp(L, 0x220E);
        }

        par_end(L);
    } else {
        /* anything else: its content -- past an option and a first argument that is a setting (a number, a
           length, a name: no spaces in it), as spacing{2}, multicols{2}, adjustbox{width=...} have */
        size_t j = ca, a, b, k2;

        read_opt(s, ce, &j, &a, &b);
        skip_ws(s, ce, &j);

        if (j < ce && s[j] == '{') {
            size_t e = group_end(s, ce, j);
            int setting = e < ce && e - j < 40;

            for (k2 = j + 1; setting && k2 < e; k2++) {
                if (s[k2] == ' ' || s[k2] == '\n' || s[k2] == '\\') {
                    setting = s[k2] == '\\' && (!strncmp(s + k2, "\\textwidth", 10) || !strncmp(s + k2, "\\linewidth", 10));
                }
            }

            if (setting) {
                j = e + 1;
            } else {
                j = ca;
            }
        } else {
            j = ca;
        }

        run(L, s + j, ce - j);
    }

    L->align = align;
    L->ragged = ragged;
    L->cp = was;
    apply_format(L);
}

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */

/* commands whose arguments are not text: left out with them ({} arguments, [..] options before them) */
static int skip_args(const char* name, size_t n) {
    static const struct { const char* name; int nargs; } T[] = {
        { "usepackage", 1 }, { "RequirePackage", 1 }, { "documentclass", 1 }, { "vspace", 1 }, { "hspace", 1 },
        { "setlength", 2 }, { "addtolength", 2 }, { "setcounter", 2 }, { "addtocounter", 2 },
        { "pagestyle", 1 }, { "thispagestyle", 1 }, { "bibliographystyle", 1 }, { "geometry", 1 },
        { "hypersetup", 1 }, { "graphicspath", 1 }, { "addcontentsline", 3 }, { "input", 1 }, { "include", 1 },
        { "includeonly", 1 }, { "newcounter", 1 }, { "setmainfont", 1 }, { "setsansfont", 1 },
        { "setmonofont", 1 }, { "newtheorem", 2 }, { "theoremstyle", 1 }, { "pagenumbering", 1 },
        { "numberwithin", 2 }, { "renewenvironment", 3 }, { "captionsetup", 1 }, { "definecolor", 3 },
        { "lstset", 1 }, { "newlength", 1 }, { "settowidth", 2 }, { "vskip", 0 }, { "hskip", 0 },
        { "linespread", 1 }, { "fontsize", 2 }, { "selectfont", 0 }, { "bibliography", 1 },
        { "addbibresource", 1 }, { "nocite", 1 }, { "markboth", 2 }, { "markright", 1 }, { "enlargethispage", 1 },
        { "titleformat", 5 }, { "titlespacing", 4 }, { "fancyhead", 1 }, { "fancyfoot", 1 }, { "rhead", 1 },
        { "lhead", 1 }, { "chead", 1 }, { "rfoot", 1 }, { "lfoot", 1 }, { "cfoot", 1 }, { "affiliation", 1 }, { "includepdf", 1 }, { "affil", 1 }, { "address", 1 }, { "institute", 1 },
        { "keywords", 1 }, { "email", 1 }, { "orcid", 1 }, { "pdfbookmark", 2 }, { "phantomsection", 0 },
        { "allowdisplaybreaks", 0 }, { "raggedbottom", 0 }, { "flushbottom", 0 }, { "onecolumn", 0 },
        { "twocolumn", 0 }, { "sloppy", 0 }, { "fussy", 0 }, { "frenchspacing", 0 }, { "hfill", 0 },
        { "vfill", 0 }, { "smallskip", 0 }, { "medskip", 0 }, { "bigskip", 0 }, { "noindent", 0 },
        { "indent", 0 }, { "centering", 0 }, { "raggedright", 0 }, { "raggedleft", 0 }, { "tableofcontents", 0 },
        { "listoffigures", 0 }, { "listoftables", 0 }, { "maketitle", 0 }, { "protect", 0 }, { "relax", 0 },
        { "nobreak", 0 }, { "noalign", 0 }, { "par", 0 }, { "clearpage", 0 }, { "newpage", 0 },
        { "cleardoublepage", 0 }, { "pagebreak", 0 }, { "nopagebreak", 0 }, { "hline", 0 }, { "toprule", 0 },
        { "midrule", 0 }, { "bottomrule", 0 }, { "cline", 1 }, { "cmidrule", 1 }, { "rule", 2 },
        { "makeatletter", 0 }, { "makeatother", 0 }, { "ignorespaces", 0 }, { "strut", 0 }, { "null", 0 },
        { "label", 1 }, { "index", 1 }, { "glossary", 1 }, { "FloatBarrier", 0 }, { "printbibliography", 0 },
        { "appendix", 0 }, { "frontmatter", 0 }, { "mainmatter", 0 }, { "backmatter", 0 }, { "linewidth", 0 },
        { "textwidth", 0 }, { "columnwidth", 0 }, { "baselineskip", 0 }, { "parindent", 0 }, { "parskip", 0 },
        { "tabcolsep", 0 }, { "arraystretch", 0 }, { "newline", 0 }, { "linebreak", 0 }, { "thanks", 1 },
        { "acknowledgments", 0 }, { "IEEEoverridecommandlockouts", 0 }, { "IEEEpeerreviewmaketitle", 0 },
        { "balance", 0 }, { "and", 0 }, { "printindex", 0 }, { "makeindex", 0 }, { "date", 1 }, { "title", 1 },
        { "author", 1 }, { "newblock", 0 }, { "BibitemOpen", 0 }, { "BibitemShut", 1 }, { "bibAnnoteFile", 1 },
        { "bibAnnote", 2 }, { "providebibliographyfont", 0 }
    };
    size_t k;

    for (k = 0; k < sizeof(T) / sizeof(T[0]); k++) {
        if (strlen(T[k].name) == n && !strncmp(T[k].name, name, n)) {
            return T[k].nargs;
        }
    }

    return -1;
}

/* a font change: 1 when name is one, the format changed */
static int font_change(lt* L, const char* name, size_t n) {
#define IS(x) (strlen(x) == n && !strncmp(name, x, n))
    pd_char_props* c = &L->cp;

    if (IS("bfseries") || IS("bf")) {
        c->mask |= PD_CP_WEIGHT;
        c->weight = 700;
    } else if (IS("mdseries")) {
        c->mask |= PD_CP_WEIGHT;
        c->weight = 400;
    } else if (IS("itshape") || IS("it") || IS("slshape") || IS("sl")) {
        c->mask |= PD_CP_ITALIC;
        c->italic = 1;
    } else if (IS("em")) {
        c->mask |= PD_CP_ITALIC;
        c->italic = !c->italic;
    } else if (IS("upshape")) {
        c->mask |= PD_CP_ITALIC;
        c->italic = 0;
    } else if (IS("ttfamily") || IS("tt")) {
        c->mask |= PD_CP_FAMILY;
        strcpy(c->family, "monospace");
    } else if (IS("rmfamily") || IS("rm") || IS("sffamily") || IS("sf")) {
        c->mask &= ~PD_CP_FAMILY;
        c->family[0] = '\0';

        if (IS("sffamily") || IS("sf")) {
            c->mask |= PD_CP_FAMILY;
            strcpy(c->family, "sans-serif");
        }
    } else if (IS("scshape") || IS("sc")) {
        c->mask |= PD_CP_SMALLCAPS;
        c->small_caps = 1;
    } else if (IS("normalfont")) {
        c->mask &= ~(PD_CP_WEIGHT | PD_CP_ITALIC | PD_CP_FAMILY | PD_CP_SMALLCAPS);
    } else if (IS("tiny") || IS("scriptsize") || IS("footnotesize") || IS("small") || IS("normalsize") ||
               IS("large") || IS("Large") || IS("LARGE") || IS("huge") || IS("Huge")) {
        static const struct { const char* n; int tenths; } S[] = {
            { "tiny", 50 }, { "scriptsize", 70 }, { "footnotesize", 80 }, { "small", 90 }, { "normalsize", 0 },
            { "large", 120 }, { "Large", 144 }, { "LARGE", 173 }, { "huge", 207 }, { "Huge", 249 }
        };
        size_t k;

        for (k = 0; k < sizeof(S) / sizeof(S[0]); k++) {
            if (strlen(S[k].n) == n && !strncmp(name, S[k].n, n)) {
                if (S[k].tenths) {
                    c->mask |= PD_CP_SIZE;
                    c->size = (pd_sp)((int64_t)PD_PT(1) * S[k].tenths / 10);
                } else {
                    c->mask &= ~PD_CP_SIZE;
                }
            }
        }
    } else {
        return 0;
    }

    apply_format(L);
    return 1;
#undef IS
}

/* a colour by name (the usual ones, xcolor's mixes like red!50 taken as the colour) or as an HTML value */
static uint32_t colour(const char* s, size_t n) {
    static const struct { const char* n; uint32_t v; } T[] = {
        { "red", 0xFF0000 }, { "green", 0x00A000 }, { "blue", 0x0000FF }, { "black", 0x000000 },
        { "white", 0xFFFFFF }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "cyan", 0x00B0C0 },
        { "magenta", 0xFF00FF }, { "yellow", 0xE0C000 }, { "orange", 0xFF8000 }, { "purple", 0x800080 },
        { "brown", 0x8B4513 }, { "violet", 0x8000FF }, { "teal", 0x008080 }, { "olive", 0x808000 },
        { "darkgray", 0x404040 }, { "lightgray", 0xC0C0C0 }, { "pink", 0xFFC0CB }, { "lime", 0x80FF00 }
    };
    size_t k, e = 0;

    while (e < n && s[e] != '!') {
        e++;
    }

    for (k = 0; k < sizeof(T) / sizeof(T[0]); k++) {
        if (strlen(T[k].n) == e && !strncmp(s, T[k].n, e)) {
            return 0xFF000000u | T[k].v;
        }
    }

    return 0;
}

static void reference(lt* L, const char* key, size_t kn, int how) {
    const tlabel* t = find_label(L, key, kn);
    char buf[64];

    if (!t) {
        out_str(L, "??");
        return;
    }

    if (how == 1) {     /* \eqref */
        snprintf(buf, sizeof(buf), "(%s)", t->num);
    } else if (how == 2) {     /* \autoref, \cref */
        const char* k = t->kind;

        snprintf(buf, sizeof(buf), "%s%s%s", !strcmp(k, "figure") ? "Figure " : !strcmp(k, "table") ? "Table " :
                 !strcmp(k, "equation") ? "Equation " : !strcmp(k, "chapter") ? "Chapter " :
                 !strcmp(k, "section") ? "Section " : "", !strcmp(k, "equation") ? "(" : "", t->num);

        if (!strcmp(k, "equation")) {
            strncat(buf, ")", sizeof(buf) - strlen(buf) - 1);
        }
    } else {
        snprintf(buf, sizeof(buf), "%s", t->num);
    }

    out_str(L, buf);
}

/* a citation: how 0 "[1, 2]", 1 "Smith et al. [1]" (\citet), 2 "Smith et al." (\citeauthor), 3 "2020" (\citeyear);
   a key the bibliography has not: the key itself */
/* a URL as written in a group: \% \# \& \_ \{ \} \~ are its characters (as hyperref reads them) */
static void url_text(pd_buf* u, const char* s, size_t n) {
    size_t k;

    memset(u, 0, sizeof(*u));

    for (k = 0; k < n; k++) {
        if (s[k] == '\\' && k + 1 < n && strchr("%#&_{}~$", s[k + 1])) {
            k++;
        }

        pb_putc(u, s[k]);
    }
}

static void citation(lt* L, const char* s, size_t a, size_t b, size_t na, size_t nb, int has_note, int how) {
    pd_buf o;
    size_t i = a;
    int first = 1, bracket = 0;

    memset(&o, 0, sizeof(o));
    each_key(L, s, a, b, note_cited);

    while (i < b) {
        size_t e = i, ka, kb;

        while (e < b && s[e] != ',') {
            e++;
        }

        ka = i;
        kb = e;

        while (ka < kb && isspace((unsigned char)s[ka])) {
            ka++;
        }

        while (kb > ka && isspace((unsigned char)s[kb - 1])) {
            kb--;
        }

        if (kb > ka) {
            int k = bib_index(L, s + ka, kb - ka, 0);
            const tbib* t = k >= 0 ? &L->bib[k] : NULL;
            int named = how && t && t->who[0];

            if (named) {     /* the names in the text */
                if (bracket) {
                    pb_putc(&o, ']');
                    bracket = 0;
                }

                if (!first) {
                    pb_puts(&o, how == 3 ? ", " : "; ");
                }

                /* (the names as LaTeX: et~al., {\"O}) */
                out_raw(L, o.p ? o.p : "", o.n);
                o.n = 0;
                run(L, how == 3 ? t->year : t->who, strlen(how == 3 ? t->year : t->who));

                if (how == 1) {
                    pb_printf(&o, " [%s", t->label[0] ? t->label : "");

                    if (!t->label[0]) {
                        pb_printf(&o, "%d", k + 1);
                    }

                    bracket = 1;
                }
            } else {
                if (!bracket) {
                    if (!first) {
                        pb_putc(&o, ' ');
                    }

                    pb_putc(&o, '[');
                    bracket = 1;
                } else {
                    pb_puts(&o, ", ");
                }

                if (t && t->label[0]) {
                    pb_puts(&o, t->label);
                } else if (t) {
                    pb_printf(&o, "%d", k + 1);
                } else {
                    pb_put(&o, s + ka, kb - ka);
                }
            }

            first = 0;
        }

        i = e + 1;
    }

    if (has_note && nb > na) {
        if (!bracket) {
            pb_puts(&o, " [");
            bracket = 1;
        } else {
            pb_puts(&o, ", ");
        }

        out_raw(L, o.p ? o.p : "", o.n);
        o.n = 0;
        run(L, s + na, nb - na);   /* p.~2 */
    }

    if (bracket) {
        pb_putc(&o, ']');
    }

    out_raw(L, o.p ? o.p : "", o.n);
    pb_free(&o);
}

/* \newcommand and its kin: a macro defined */
static void define(lt* L, const char* s, size_t n, size_t* i, int env) {
    size_t na, nb, oa, ob, ba, bb, ea = 0, eb = 0;
    int nargs = 0, has_def = 0;
    size_t da = 0, db = 0;

    read_star(s, n, i);

    if (!read_arg(s, n, i, &na, &nb)) {
        return;
    }

    if (read_opt(s, n, i, &oa, &ob)) {
        char nbuf[8];

        if (ob - oa < sizeof(nbuf)) {
            memcpy(nbuf, s + oa, ob - oa);
            nbuf[ob - oa] = '\0';
            nargs = atoi(nbuf);
        }

        if (read_opt(s, n, i, &da, &db)) {
            has_def = 1;
        }
    }

    if (!read_arg(s, n, i, &ba, &bb)) {
        return;
    }

    if (env && !read_arg(s, n, i, &ea, &eb)) {
        return;
    }

    if (!env && nb - na > 1 && s[na] == '\\') {
        na++;
    }

    define_macro(L, s + na, nb - na, nargs, has_def ? s + da : NULL, has_def ? db - da : 0, s + ba, bb - ba, env,
                 env ? s + ea : NULL, env ? eb - ea : 0);
}

/* \def\name#1#2{body} */
static void def_tex(lt* L, const char* s, size_t n, size_t* i) {
    size_t na, ne, ba, bb;
    int nargs = 0;

    skip_ws(s, n, i);

    if (*i >= n || s[*i] != '\\') {
        return;
    }

    na = ++(*i);

    while (*i < n && is_letter(s[*i])) {
        (*i)++;
    }

    ne = *i;

    while (*i < n && s[*i] != '{') {
        if (s[*i] == '#' && *i + 1 < n && s[*i + 1] >= '1' && s[*i + 1] <= '9') {
            nargs = s[*i + 1] - '0';
        }

        (*i)++;
    }

    if (!read_arg(s, n, i, &ba, &bb)) {
        return;
    }

    define_macro(L, s + na, ne - na, nargs, NULL, 0, s + ba, bb - ba, 0, NULL, 0);
}

static void command(lt* L, const char* s, size_t n, size_t* i) {
    size_t a = *i + 1, e = a, x, y, oa, ob;
    const char* name;
    size_t nl;
    int k;
    uint32_t sym;

    if (a >= n) {
        *i = n;
        return;
    }

    /* a one-character command */
    if (!is_letter(s[a])) {
        char c = s[a];

        *i = a + 1;

        switch (c) {
            case '\\':
                if (*i < n && s[*i] == '*') {
                    (*i)++;
                }

                read_opt(s, n, i, &oa, &ob);
                out_newline(L);
                return;

            case ' ': case '\n': case '\t':
                out_raw(L, " ", 1);
                return;

            case ',': case ';': case ':':
                out_cp(L, 0x2009);
                return;

            case '!': case '/': case '-': case '@': case '>': case '<':
                return;

            case '(': {   /* \( ... \) */
                size_t j = *i;

                while (j + 1 < n && !(s[j] == '\\' && s[j + 1] == ')')) {
                    j++;
                }

                out_equation(L, s + *i, j - *i, 0);
                *i = j + 2 < n ? j + 2 : n;
                return;
            }

            case '[': {   /* \[ ... \] */
                size_t j = *i;

                while (j + 1 < n && !(s[j] == '\\' && s[j + 1] == ']')) {
                    j++;
                }

                out_equation(L, s + *i, j - *i, 1);
                *i = j + 2 < n ? j + 2 : n;
                return;
            }

            case '\'': case '`': case '^': case '"': case '~': case '=': case '.':
                if (read_arg(s, n, i, &x, &y)) {
                    out_accent(L, c, s, x, y);
                }

                return;

            default:
                out_raw(L, &c, 1);     /* \& \% \$ \# \_ \{ \} */
                return;
        }
    }

    while (e < n && is_letter(s[e])) {
        e++;
    }

    name = s + a;
    nl = e - a;
    *i = e;

#define IS(x) (strlen(x) == nl && !strncmp(name, x, nl))

    /* a macro of the document's */
    {
        tmacro* m = find_macro(L, name, nl, 0);

        if (m && L->depth < LX_MAX_DEPTH) {
            pd_buf o;

            memset(&o, 0, sizeof(o));

            if (expand_macro(L, m, s, n, i, &o)) {
                L->depth++;
                run(L, o.p ? o.p : "", o.n);
                L->depth--;
            }

            pb_free(&o);
            return;
        }
    }

    /* the space after a command's name is the name's end */
    if (IS("newcommand") || IS("renewcommand") || IS("providecommand") || IS("DeclareRobustCommand")) {
        define(L, s, n, i, 0);
        return;
    }

    if (IS("newenvironment")) {
        define(L, s, n, i, 1);
        return;
    }

    if (IS("def") || IS("gdef") || IS("edef")) {
        def_tex(L, s, n, i);
        return;
    }

    if (IS("let")) {   /* \let\a\b: \a as \b is, when \b is the document's */
        size_t p = *i, q, r;

        if (read_arg(s, n, &p, &x, &y) && x < y && s[x] == '\\') {
            if (p < n && s[p] == '=') {
                p++;
            }

            if (read_arg(s, n, &p, &q, &r) && q < r && s[q] == '\\') {
                tmacro* m = find_macro(L, s + q + 1, r - q - 1, 0);

                if (m) {
                    tmacro c = *m;

                    define_macro(L, s + x + 1, y - x - 1, c.nargs, c.opt, c.opt ? strlen(c.opt) : 0, c.body,
                                 strlen(c.body), 0, NULL, 0);
                } else {
                    /* a TeX primitive's new name: the old name's text */
                    define_macro(L, s + x + 1, y - x - 1, 0, NULL, 0, s + q, r - q, 0, NULL, 0);
                }
            }
        }

        *i = p;
        return;
    }

    if (IS("DeclareMathOperator")) {
        size_t p = *i, q, r;
        pd_buf body;

        read_star(s, n, &p);

        if (read_arg(s, n, &p, &x, &y) && read_arg(s, n, &p, &q, &r) && x < y && s[x] == '\\') {
            memset(&body, 0, sizeof(body));
            pb_puts(&body, "\\operatorname{");
            pb_put(&body, s + q, r - q);
            pb_putc(&body, '}');
            define_macro(L, s + x + 1, y - x - 1, 0, NULL, 0, body.p, body.n, 0, NULL, 0);
            pb_free(&body);
        }

        *i = p;
        return;
    }

    if (IS("begin")) {
        environment(L, s, n, i);
        return;
    }

    if (IS("end")) {   /* (one whose \begin was not seen) */
        read_arg(s, n, i, &x, &y);
        return;
    }

    if (IS("title") || IS("author") || IS("date")) {
        char** slot = IS("title") ? &L->title : IS("author") ? &L->author : &L->date;

        read_opt(s, n, i, &oa, &ob);

        if (read_arg(s, n, i, &x, &y)) {
            if (IS("author") && *slot) {    /* another author (\author once each): after the others */
                pd_buf o;

                memset(&o, 0, sizeof(o));
                pb_puts(&o, *slot);
                pb_puts(&o, " \\and ");
                pb_put(&o, s + x, y - x);
                free(*slot);
                *slot = dup_range(o.p, 0, o.n);
                pb_free(&o);
            } else {
                free(*slot);
                *slot = dup_range(s, x, y);
            }

            if (IS("date")) {
                L->has_date = 1;
            }
        }

        return;
    }

    if (IS("maketitle")) {
        title_block(L);
        return;
    }

    if (IS("today")) {
        static const char* const months[12] = { "January", "February", "March", "April", "May", "June", "July",
                                                "August", "September", "October", "November", "December" };
        time_t t = time(NULL);
        struct tm* tm = localtime(&t);
        char buf[48];

        if (tm) {
            snprintf(buf, sizeof(buf), "%s %d, %d", months[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
            out_str(L, buf);
        }

        return;
    }

    if (IS("raggedright") || IS("RaggedRight") || IS("justifying")) {
        L->ragged = name[0] != 'j';     /* (ragged2e's \justifying undoes it) */
        return;
    }

    if (IS("appendix")) {
        L->appendix = 1;
        L->sec[L->has_chapter ? 0 : 1] = 0;
        return;
    }

    for (k = 0; k < 6; k++) {
        if (strlen(SECTIONS[k]) == nl && !strncmp(name, SECTIONS[k], nl)) {
            int star = read_star(s, n, i);

            read_opt(s, n, i, &oa, &ob);

            if (read_arg(s, n, i, &x, &y)) {
                heading(L, k, star, s, x, y);
            }

            return;
        }
    }

    if (IS("item")) {
        int has = 0;

        par_end(L);
        L->item_pending = 1;

        if (L->ldepth > 0) {
            L->item_count[L->ldepth]++;

            if (L->lkind[L->ldepth] == 2) {
                char num[16];

                snprintf(num, sizeof(num), "%d", L->item_count[L->ldepth]);
                set_label_target(L, num, "item");
            }
        }

        has = read_opt(s, n, i, &oa, &ob);

        if (has && L->ldepth > 0 && L->lkind[L->ldepth] == 3) {     /* a description's term, bold */
            pd_char_props keep = L->cp;

            L->cp.mask |= PD_CP_WEIGHT;
            L->cp.weight = 700;
            apply_format(L);
            run(L, s + oa, ob - oa);
            L->cp = keep;
            apply_format(L);
            out_raw(L, " ", 1);
        } else if (has) {
            run(L, s + oa, ob - oa);
            out_raw(L, " ", 1);
        }

        skip_ws(s, n, i);
        return;
    }

    if ((IS("subfigure") || IS("subfloat") || IS("subtable")) && L->in_float) {
        size_t ca2 = 0, cb2 = 0;
        int has = read_opt(s, n, i, &ca2, &cb2), keep = L->in_sub;

        if (read_arg(s, n, i, &x, &y)) {
            sub_begin(L);
            L->in_sub = 1;
            run(L, s + x, y - x);

            if (has) {
                caption(L, s, ca2, cb2);
            }

            L->in_sub = keep;
            out_raw(L, " ", 1);
        }

        return;
    }

    if (IS("caption")) {
        read_opt(s, n, i, &oa, &ob);

        if (read_arg(s, n, i, &x, &y)) {
            caption(L, s, x, y);
        }

        return;
    }

    if (IS("label")) {
        if (read_arg(s, n, i, &x, &y)) {
            add_label(L, s + x, y - x);
        }

        return;
    }

    if (IS("ref") || IS("eqref") || IS("autoref") || IS("cref") || IS("Cref") || IS("pageref") || IS("nameref")) {
        read_star(s, n, i);

        if (read_arg(s, n, i, &x, &y)) {
            if (IS("pageref")) {
                out_str(L, "?");
            } else {
                size_t p = x;

                /* \cref{a,b}: each */
                while (p < y) {
                    size_t q = p;

                    while (q < y && s[q] != ',') {
                        q++;
                    }

                    if (p > x) {
                        out_str(L, ", ");
                    }

                    reference(L, s + p, q - p, IS("eqref") ? 1 : (IS("autoref") || IS("cref") || IS("Cref")) ? 2 : 0);
                    p = q + 1;
                }
            }
        }

        return;
    }

    if (IS("cite") || IS("citep") || IS("citet") || IS("citealp") || IS("citeauthor") || IS("citeyear") ||
        IS("parencite") || IS("textcite") || IS("autocite") || IS("supercite") || IS("Cite") || IS("Citet") ||
        IS("citealt") || IS("Citeauthor") || IS("citeyearpar") || IS("footcite") || IS("smartcite")) {
        size_t na = 0, nb = 0, pa, pb;
        int note, how = IS("citet") || IS("Citet") || IS("textcite") || IS("citealt") ? 1 :
                        IS("citeauthor") || IS("Citeauthor") ? 2 : IS("citeyear") || IS("citeyearpar") ? 3 : 0;

        read_star(s, n, i);
        note = read_opt(s, n, i, &na, &nb);

        if (read_opt(s, n, i, &pa, &pb)) {   /* [pre][post]: the post note */
            na = pa;
            nb = pb;
        }

        if (read_arg(s, n, i, &x, &y)) {
            citation(L, s, x, y, na, nb, note, how);
        }

        return;
    }

    if (IS("nocite")) {
        if (read_arg(s, n, i, &x, &y)) {
            each_key(L, s, x, y, note_cited);
        }

        return;
    }

    if (IS("bibliographystyle")) {
        if (read_arg(s, n, i, &x, &y) && y - x < sizeof(L->bibstyle)) {
            memcpy(L->bibstyle, s + x, y - x);
            L->bibstyle[y - x] = '\0';
        }

        return;
    }

    if (IS("bibinfo") || IS("bibfield")) {     /* a .bbl's \bibinfo{author}{...}: the second its text */
        read_arg(s, n, i, &x, &y);
        return;
    }

    if (IS("urlprefix")) {
        out_str(L, "URL ");
        return;
    }

    if (IS("bibitem")) {    /* outside thebibliography: a line of its own */
        read_opt(s, n, i, &oa, &ob);
        read_arg(s, n, i, &x, &y);
        par_end(L);
        return;
    }

    if (IS("footnote") || IS("footnotetext")) {
        read_opt(s, n, i, &oa, &ob);

        if (read_arg(s, n, i, &x, &y)) {
            if (!L->dry) {
                pd_char_props keep = L->cp;
                int in_para = L->in_para, sp = L->space_pending, ld = L->ldepth;
                const char* st = L->style;
                int32_t role = L->role, al = L->align;

                par_begin(L);
                bld_footnote_begin(L->b);
                L->in_para = 0;
                L->ldepth = 0;
                L->style = NULL;
                L->role = PD_ROLE_BODY;
                L->align = -1;
                memset(&L->cp, 0, sizeof(L->cp));
                run(L, s + x, y - x);
                par_end(L);
                bld_footnote_end(L->b);
                L->cp = keep;
                L->in_para = in_para;
                L->space_pending = sp;
                L->ldepth = ld;
                L->style = st;
                L->role = role;
                L->align = al;
                bld_set_format(L->b, &L->cp);
            }

            L->fn++;
        }

        return;
    }

    if (IS("href")) {
        size_t ua, ub;

        if (read_arg(s, n, i, &ua, &ub) && read_arg(s, n, i, &x, &y)) {
            pd_inline o;
            pd_buf u;

            url_text(&u, s + ua, ub - ua);

            par_begin(L);

            if (L->space_pending && !L->dry) {
                bld_text(L->b, " ", 1);
            }

            L->space_pending = 0;

            if (!L->dry) {
                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_LINK;
                o.source = u.p ? u.p : "";
                o.source_len = (int32_t)u.n;
                bld_inline(L->b, &o);
            }

            pb_free(&u);
            run(L, s + x, y - x);

            if (!L->dry) {
                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_LINK;
                bld_inline(L->b, &o);
            }
        }

        return;
    }

    if (IS("url") || IS("nolinkurl") || IS("path")) {
        if (read_arg(s, n, i, &x, &y)) {
            pd_inline o;
            pd_char_props keep = L->cp;
            pd_buf u;

            url_text(&u, s + x, y - x);

            par_begin(L);

            if (L->space_pending && !L->dry) {
                bld_text(L->b, " ", 1);
            }

            L->space_pending = 0;

            if (!L->dry && IS("url")) {
                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_LINK;
                o.source = u.p ? u.p : "";
                o.source_len = (int32_t)u.n;
                bld_inline(L->b, &o);
            }

            L->cp.mask |= PD_CP_FAMILY;
            strcpy(L->cp.family, "monospace");
            apply_format(L);
            out_raw(L, u.p ? u.p : "", u.n);   /* (as it is: a URL's characters are its own) */
            pb_free(&u);
            L->cp = keep;
            apply_format(L);

            if (!L->dry && IS("url")) {
                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_LINK;
                bld_inline(L->b, &o);
            }
        }

        return;
    }

    if (IS("includegraphics")) {
        int has;

        read_star(s, n, i);
        has = read_opt(s, n, i, &oa, &ob);

        if (read_arg(s, n, i, &x, &y)) {
            picture(L, s, has ? oa : 0, has ? ob : 0, has, x, y);
        }

        return;
    }

    if (IS("verb") || IS("lstinline")) {
        char delim;
        size_t p = *i, q;
        pd_char_props keep = L->cp;

        read_star(s, n, &p);

        if (IS("lstinline")) {
            read_opt(s, n, &p, &oa, &ob);
        }

        if (p >= n) {
            *i = p;
            return;
        }

        delim = s[p] == '{' ? '}' : s[p];
        q = p + 1;

        while (q < n && s[q] != delim && s[q] != '\n') {
            q++;
        }

        L->cp.mask |= PD_CP_FAMILY;
        strcpy(L->cp.family, "monospace");
        apply_format(L);
        out_raw(L, s + p + 1, q - p - 1);
        L->cp = keep;
        apply_format(L);
        *i = q < n ? q + 1 : n;
        return;
    }

    /* the text commands: their argument in a format */
    {
        static const char* const TEXT[] = { "textbf", "textit", "emph", "texttt", "textsc", "textsl", "textrm",
                                            "textsf", "textmd", "textup", "textnormal", "underline", "uline",
                                            "sout", "st", "textsuperscript", "textsubscript", "mbox", "hbox",
                                            "fbox", "framebox", "makebox", "text", "textcolor", "colorbox",
                                            "emph", "enquote", "mathrm", "textstrikeout", "hl", "ul" };
        size_t t;

        for (t = 0; t < sizeof(TEXT) / sizeof(TEXT[0]); t++) {
            if (strlen(TEXT[t]) == nl && !strncmp(name, TEXT[t], nl)) {
                pd_char_props keep = L->cp;
                pd_char_props* c = &L->cp;
                uint32_t col = 0;

                if (IS("textcolor") || IS("colorbox")) {
                    read_opt(s, n, i, &oa, &ob);

                    if (read_arg(s, n, i, &x, &y)) {
                        col = colour(s + x, y - x);
                    }
                }

                if (IS("framebox") || IS("makebox")) {
                    read_opt(s, n, i, &oa, &ob);
                    read_opt(s, n, i, &oa, &ob);
                }

                if (!read_arg(s, n, i, &x, &y)) {
                    return;
                }

                if (IS("textbf")) {
                    c->mask |= PD_CP_WEIGHT;
                    c->weight = 700;
                } else if (IS("textit") || IS("textsl")) {
                    c->mask |= PD_CP_ITALIC;
                    c->italic = 1;
                } else if (IS("emph")) {
                    c->mask |= PD_CP_ITALIC;
                    c->italic = !c->italic;
                } else if (IS("texttt")) {
                    c->mask |= PD_CP_FAMILY;
                    strcpy(c->family, "monospace");
                } else if (IS("textsf")) {
                    c->mask |= PD_CP_FAMILY;
                    strcpy(c->family, "sans-serif");
                } else if (IS("textsc")) {
                    c->mask |= PD_CP_SMALLCAPS;
                    c->small_caps = 1;
                } else if (IS("textup") || IS("textrm") || IS("textnormal") || IS("textmd") || IS("mathrm")) {
                    c->mask |= PD_CP_ITALIC;
                    c->italic = 0;

                    if (IS("textmd") || IS("textnormal")) {
                        c->mask |= PD_CP_WEIGHT;
                        c->weight = 400;
                    }

                    if (IS("textrm") || IS("textnormal")) {
                        c->mask &= ~PD_CP_FAMILY;
                    }
                } else if (IS("underline") || IS("uline") || IS("ul")) {
                    c->mask |= PD_CP_UNDERLINE;
                    c->underline = PD_UNDERLINE_SINGLE;
                } else if (IS("sout") || IS("st") || IS("textstrikeout")) {
                    c->mask |= PD_CP_STRIKE;
                    c->strike = 1;
                } else if (IS("textsuperscript")) {
                    c->mask |= PD_CP_SHIFT;
                    c->shift = PD_SHIFT_SUPER;
                } else if (IS("textsubscript")) {
                    c->mask |= PD_CP_SHIFT;
                    c->shift = PD_SHIFT_SUB;
                } else if (IS("textcolor") && col) {
                    c->mask |= PD_CP_COLOR;
                    c->color = col;
                } else if ((IS("colorbox") || IS("hl")) && (col || IS("hl"))) {
                    c->mask |= PD_CP_BACKGROUND;
                    c->background = col ? col : 0xFFFFFF00u;
                }

                apply_format(L);

                if (IS("enquote")) {
                    out_cp(L, 0x201C);
                }

                run(L, s + x, y - x);

                if (IS("enquote")) {
                    out_cp(L, 0x201D);
                }

                L->cp = keep;
                apply_format(L);
                return;
            }
        }
    }

    if (IS("color")) {   /* a declaration: the colour to the group's end */
        read_opt(s, n, i, &oa, &ob);

        if (read_arg(s, n, i, &x, &y)) {
            uint32_t col = colour(s + x, y - x);

            if (col) {
                L->cp.mask |= PD_CP_COLOR;
                L->cp.color = col;
                apply_format(L);
            }
        }

        return;
    }

    if (font_change(L, name, nl)) {
        return;
    }

    if (IS("LaTeX") || IS("LaTeXe") || IS("TeX") || IS("BibTeX")) {
        if (IS("LaTeXe")) {
            out_str(L, "LaTeX2e");
        } else {
            out_raw(L, name, nl);
        }
        return;
    }

    if (IS("newpage") || IS("clearpage") || IS("cleardoublepage") || IS("pagebreak")) {
        par_end(L);

        if (!L->dry && L->in_document) {
            bld_break(L->b, PD_BREAK_PAGE);
        }

        read_opt(s, n, i, &oa, &ob);
        return;
    }

    if (IS("par")) {
        par_end(L);
        return;
    }

    if (IS("newline") || IS("linebreak")) {
        read_opt(s, n, i, &oa, &ob);
        out_newline(L);
        return;
    }

    if (IS("c") || IS("v") || IS("u") || IS("H") || IS("r") || IS("k") || IS("d") || IS("b") || IS("t")) {
        if (read_arg(s, n, i, &x, &y)) {
            out_accent(L, name[0], s, x, y);
        }

        return;
    }

    sym = symbol(name, nl);

    if (sym) {
        out_cp(L, sym);

        if (*i < n && s[*i] == '{' && *i + 1 < n && s[*i + 1] == '}') {
            *i += 2;    /* \ss{} */
        }

        return;
    }

    k = skip_args(name, nl);

    if (k >= 0) {
        int j;

        read_star(s, n, i);

        for (j = 0; j < k; j++) {
            read_opt(s, n, i, &oa, &ob);

            if (!read_arg(s, n, i, &x, &y)) {
                break;
            }
        }

        if (k == 0) {
            read_opt(s, n, i, &oa, &ob) ? (void)0 : (void)0;
        }

        return;
    }

    /* anything else: nothing of its own, its arguments read as text where they come (an option right after its
       name is a setting, not text: left out) */
    if (*i < n && s[*i] == '[') {
        read_opt(s, n, i, &oa, &ob);
    }
#undef IS
}

/* ------------------------------------------------------------------ */
/* the text                                                           */
/* ------------------------------------------------------------------ */

static void run(lt* L, const char* s, size_t n) {
    size_t i = 0;

    if (L->depth > LX_MAX_DEPTH) {
        return;
    }

    L->depth++;

    while (i < n) {
        char c = s[i];

        switch (c) {
            case '%':
                while (i < n && s[i] != '\n') {
                    i++;
                }

                /* the line end and the next line's leading blanks: gone with the comment */
                if (i < n) {
                    i++;
                }

                while (i < n && (s[i] == ' ' || s[i] == '\t')) {
                    i++;
                }

                continue;

            case '\\': {
                size_t at = i, ne = i + 1;

                while (ne < n && is_letter(s[ne])) {
                    ne++;
                }

                command(L, s, n, &i);

                /* the blanks after a command's name end it (one that took nothing after its name) */
                if (ne > at + 1 && i == ne) {
                    while (i < n && (s[i] == ' ' || s[i] == '\t')) {
                        i++;
                    }

                    if (i < n && s[i] == '\n') {
                        size_t j = i + 1;

                        while (j < n && (s[j] == ' ' || s[j] == '\t')) {
                            j++;
                        }

                        if (j < n && s[j] != '\n') {
                            i = j;
                        }
                    }
                }

                continue;
            }

            case '{': {
                size_t e = group_end(s, n, i);
                pd_char_props keep = L->cp;

                run(L, s + i + 1, (e < n ? e : n) - i - 1);
                L->cp = keep;
                apply_format(L);
                i = e < n ? e + 1 : n;
                continue;
            }

            case '}':
                i++;
                continue;

            case '$': {
                int display = i + 1 < n && s[i + 1] == '$';
                size_t a = i + (display ? 2 : 1), j = a;

                while (j < n && !(s[j] == '$' && s[j - 1] != '\\')) {
                    j++;
                }

                out_equation(L, s + a, j - a, display);
                i = j + (display ? 2 : 1);

                if (i > n) {
                    i = n;
                }

                continue;
            }

            case '~':
                out_cp(L, 0xA0);
                i++;
                continue;

            case '&':   /* outside a table: a column's mark, nothing */
                i++;
                continue;

            case '#': case '^': case '_':
                i++;
                continue;

            case '-':
                if (i + 2 < n && s[i + 1] == '-' && s[i + 2] == '-') {
                    out_cp(L, 0x2014);
                    i += 3;
                } else if (i + 1 < n && s[i + 1] == '-') {
                    out_cp(L, 0x2013);
                    i += 2;
                } else {
                    out_raw(L, "-", 1);
                    i++;
                }

                continue;

            case '`':
                if (i + 1 < n && s[i + 1] == '`') {
                    out_cp(L, 0x201C);
                    i += 2;
                } else {
                    out_cp(L, 0x2018);
                    i++;
                }

                continue;

            case '\'':
                if (i + 1 < n && s[i + 1] == '\'') {
                    out_cp(L, 0x201D);
                    i += 2;
                } else {
                    out_cp(L, 0x2019);
                    i++;
                }

                continue;

            case ' ': case '\t': case '\r':
                out_space(L);
                i++;
                continue;

            case '\n': {
                size_t j = i + 1;
                int blank = 0;

                /* a blank line (spaces on it, or a comment line, count as blank only when empty) */
                while (j < n && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) {
                    j++;
                }

                if (j < n && s[j] == '\n') {
                    blank = 1;
                }

                if (blank) {
                    par_end(L);

                    while (j < n && (s[j] == '\n' || s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) {
                        j++;
                    }

                    i = j;
                } else {
                    out_space(L);
                    i++;
                }

                continue;
            }

            default: {
                /* a run of plain text */
                size_t j = i;

                while (j < n && !strchr("%\\{}$~&#^_-`' \t\r\n", s[j])) {
                    j++;
                }

                if (j == i) {
                    j++;
                }

                if (L->in_document || L->in_para) {
                    out_raw(L, s + i, j - i);
                }

                i = j;
                continue;
            }
        }
    }

    L->depth--;
}

/* ------------------------------------------------------------------ */
/* the file                                                           */
/* ------------------------------------------------------------------ */

static void lx_reset(lt* L) {
    memset(L->sec, 0, sizeof(L->sec));
    L->fig = L->tab = L->eq = L->fn = 0;
    L->appendix = 0;
    L->cur_num[0] = L->cur_kind[0] = '\0';
    L->in_para = L->space_pending = 0;
    L->style = NULL;
    L->role = PD_ROLE_BODY;
    L->level = 0;
    L->align = -1;
    L->ragged = 0;
    L->ldepth = 0;
    L->item_pending = 0;
    memset(&L->cp, 0, sizeof(L->cp));
    L->in_float = 0;
    L->depth = 0;
    L->expanded = 0;
    L->in_document = 0;
}

static void lx_free(lt* L) {
    int k;

    for (k = 0; k < L->nmac; k++) {
        free(L->mac[k].opt);
        free(L->mac[k].body);
        free(L->mac[k].end_body);
    }

    free(L->mac);
    free(L->lab);
    free(L->bib);
    free(L->cited);
    free(L->bibdata);
    free(L->bibgen);
    free(L->title);
    free(L->author);
    free(L->date);
}

static void lx_forget_macros(lt* L) {
    int k;

    for (k = 0; k < L->nmac; k++) {
        free(L->mac[k].opt);
        free(L->mac[k].body);
        free(L->mac[k].end_body);
    }

    L->nmac = 0;
    free(L->title);
    free(L->author);
    free(L->date);
    L->title = L->author = L->date = NULL;
}

static int starts(const char* s, const char* const* list) {
    for (; *list; list++) {
        if (!strncmp(s, *list, strlen(*list))) {
            return 1;
        }
    }

    return 0;
}

static int contains(const char* s, const char* const* list) {
    for (; *list; list++) {
        if (strstr(s, *list)) {
            return 1;
        }
    }

    return 0;
}

/* how the style a document names lists a .bib's entries: sorted by author (plain, abbrv, natbib's author-year
   styles, biblatex unless sorting=none) or in the order cited (unsrt, ieeetr, the journals'); given names as
   initials (abbrv, IEEE, the physics and optics journals') or whole */
static void bib_style(const lt* L, const char* s, size_t n, int* sorted, int* abbrev) {
    static const char* const SORTED[] = {
        "plain", "abbrv", "alpha", "acm", "siam", "apalike", "apa", "chicago", "harvard", "agsm", "dcu",
        "named", "elsarticle-harv", "spbasic", "authordate", "jss", "mla", "ametsoc", "agu", "copernicus", NULL
    };
    static const char* const ABBREV[] = {
        "abbrv", "ieee", "IEEE", "spie", "osa", "opt", "nature", "aps", "aip", "acs", "ama", "vancouver",
        "splncs", "elsarticle-num", "bmc", "plos", "unsrtabbrv", NULL
    };
    static const char* const BL_UNSORTED[] = { "sorting=none", "style=ieee", "style=nature", "style=phys",
                                               "style=vancouver", NULL };
    static const char* const BL_ABBREV[] = { "giveninits=true", "firstinits=true", "style=ieee",
                                             "style=nature", "style=phys", NULL };

    *sorted = *abbrev = 0;

    if (L->bibstyle[0]) {
        *sorted = starts(L->bibstyle, SORTED);
        *abbrev = contains(L->bibstyle, ABBREV);
        return;
    }

    {
        /* biblatex: its options in the preamble */
        size_t pre = n;
        const char* p;
        char* head;

        for (p = s; (size_t)(p - s) + 16 <= n; p++) {
            if (*p == '\\' && !strncmp(p, "\\begin{document}", 16)) {
                pre = (size_t)(p - s);
                break;
            }
        }

        head = dup_range(s, 0, pre);

        if (head && strstr(head, "{biblatex}")) {
            *sorted = !contains(head, BL_UNSORTED);
            *abbrev = contains(head, BL_ABBREV);
        }

        free(head);
    }
}

/* the entries of the .bib the document brought, the cited ones, set out; numbered before the text's citations
   are read the second time */
static void bib_prepare(lt* L, const char* s, size_t n) {
    const char** keys;
    int k, sorted, abbrev;

    if (!L->bibdata) {
        return;
    }

    keys = (const char**)malloc(sizeof(char*) * (size_t)(L->ncited + 1));

    if (!keys) {
        return;
    }

    for (k = 0; k < L->ncited; k++) {
        keys[k] = L->cited[k];
    }

    bib_style(L, s, n, &sorted, &abbrev);
    L->bibgen = pd_bibtex_thebibliography(L->bibdata, L->bibdata_len, keys, L->ncited, L->cite_all, sorted, abbrev,
                                          &L->bibgen_len);
    free(keys);

    if (L->bibgen) {
        size_t j = 0, a, b;

        read_arg(L->bibgen, L->bibgen_len, &j, &a, &b);
        bibliography(L, L->bibgen, j, L->bibgen_len);     /* (dry: the keys and names noted) */
    }
}

pd_status pd_latex_import(pd_doc* d, const char* s, size_t n) {
    pd_bld b;
    lt L;
    const char* p;
    int pass;

    memset(&L, 0, sizeof(L));
    L.src = s;
    L.len = n;

    /* a book or report: chapters at the top */
    for (p = s; p && (size_t)(p - s) + 8 < n; p++) {
        if (*p == '\\' && (!strncmp(p, "\\chapter", 8) && !is_letter(p[8]))) {
            L.has_chapter = 1;
            break;
        }
    }

    bld_init(&b, d);
    L.b = &b;

    for (pass = 0; pass < 2; pass++) {
        int has_document = 0;

        lx_reset(&L);
        lx_forget_macros(&L);
        L.dry = pass == 0;

        for (p = s; (size_t)(p - s) + 16 <= n; p++) {
            if (*p == '\\' && !strncmp(p, "\\begin{document}", 16)) {
                has_document = 1;
                break;
            }
        }

        /* a file that is a part of one (\input into another): all of it the document's text */
        L.in_document = !has_document;
        run(&L, s, n);
        par_end(&L);

        if (pass == 0) {
            bib_prepare(&L, s, n);
        }
    }

    lx_free(&L);
    pd_doc_set_microtype(d, 1, 0);  /* the margins kerned as microtype's protrusion does, as LaTeX papers mostly are */
    return bld_finish(&b);
}
