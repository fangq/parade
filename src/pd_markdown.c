/*
 * Parade Markdown: CommonMark blocks and inlines (emphasis by the spec's
 * delimiter-run algorithm), GFM tables, strikethrough and footnotes,
 * $math$, pandoc-style image sizes {width=.. height=..}; underline,
 * superscript and subscript as inline HTML.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

/* ------------------------------------------------------------------ */
/* export                                                             */
/* ------------------------------------------------------------------ */

enum {
    MF_BOLD = 1, MF_ITAL = 2, MF_STRIKE = 4, MF_UNDER = 8, MF_SUP = 16, MF_SUB = 32, MF_CODE = 64
};

typedef struct {
    const pd_doc* d;
    pd_buf* o;
    pd_numbers nb;
    pd_block_id para;
    pd_char_props base;
    int open[8];                /* stack of open MF_ flags */
    int nopen;
    char link[2048];            /* URL of the open link */
    size_t link_at;             /* where its text starts in the output */
    int in_link, in_table, in_code;
    pd_block_id notes[4096];
    int32_t nnotes;
    /* lists: marker widths of the enclosing items */
    int lw[10];
    int lnum[10];
    pd_list_id lid[10];         /* the list each level's last item was in */
    int lalt[10];               /* that list's marker is the other one: '*' for '-', ')' for '.' */
    int lkind[10];
    int32_t ldepth;
    int prev_block;             /* 0 none, 1 list item, 2 other */
    int code_open;
    char prefix[64];            /* "> " in quotes */
} mx;

static int span_flags(const mx* x, const pd_char_props* c) {
    const pd_char_props* b = &x->base;
    int f = 0;

    f |= c->weight >= 600 && b->weight < 600 ? MF_BOLD : 0;
    f |= c->italic && !b->italic ? MF_ITAL : 0;
    f |= c->strike && !b->strike ? MF_STRIKE : 0;
    f |= c->underline && !b->underline ? MF_UNDER : 0;
    f |= c->shift == PD_SHIFT_SUPER ? MF_SUP : c->shift == PD_SHIFT_SUB ? MF_SUB : 0;
    f |= pd_conv_is_mono(c) && !pd_conv_is_mono(b) && !x->in_code ? MF_CODE : 0;
    return f;
}

static const char* mark_open(int f) {
    return f == MF_BOLD ? "**" : f == MF_ITAL ? "*" : f == MF_STRIKE ? "~~" : f == MF_UNDER ? "<u>" :
           f == MF_SUP ? "<sup>" : f == MF_SUB ? "<sub>" : "";
}

static const char* mark_close(int f) {
    return f == MF_BOLD ? "**" : f == MF_ITAL ? "*" : f == MF_STRIKE ? "~~" : f == MF_UNDER ? "</u>" :
           f == MF_SUP ? "</sup>" : f == MF_SUB ? "</sub>" : "";
}

/* close formats down to (and including) the deepest one not in keep */
static void fmt_to(mx* x, int want) {
    int i, k;

    for (i = 0; i < x->nopen && (want & x->open[i]); i++) {
    }

    for (k = x->nopen - 1; k >= i; k--) {
        pb_puts(x->o, mark_close(x->open[k]));
    }

    x->nopen = i;

    for (k = 1; k <= MF_SUB; k <<= 1) {
        int j, have = 0;

        for (j = 0; j < x->nopen; j++) {
            have |= x->open[j] == k;
        }

        if ((want & k) && !have && x->nopen < 8) {
            pb_puts(x->o, mark_open(k));
            x->open[x->nopen++] = k;
        }
    }
}

/* Markdown text escaping: punctuation that could start markup, and what a line may not start with */
static void md_esc(mx* x, const char* s, size_t n, int at_line_start) {
    pd_buf* o = x->o;
    size_t i;

    for (i = 0; i < n; i++) {
        char c = s[i];
        int start = (i == 0 && at_line_start) || (i > 0 && s[i - 1] == '\n');

        if (c == '\n') {
            if (x->in_table) {
                pb_puts(o, "<br>");
            } else {
                pb_puts(o, "\\\n");
                pb_puts(o, x->prefix);

                if (x->ldepth > 0) {
                    pb_printf(o, "%*s", x->lw[x->ldepth - 1], "");
                }
            }

            continue;
        }

        if (strchr("\\`*_[]<>$~", c) || (c == '|' && x->in_table) || (c == '&' && i + 1 < n &&
                (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '#'))) {
            pb_putc(o, '\\');
        } else if (start && (c == '#' || c == '+' || c == '-' || c == '=' || c == '>')) {
            pb_putc(o, '\\');
        } else if (start && isdigit((unsigned char)c)) {
            size_t k = i;

            while (k < n && isdigit((unsigned char)s[k])) {
                k++;
            }

            if (k < n && (s[k] == '.' || s[k] == ')')) {    /* "1." would start a list */
                pb_put(o, s + i, k - i);
                pb_putc(o, '\\');
                i = k - 1;
                continue;
            }
        }

        pb_putc(o, c);
    }
}

static void code_span(pd_buf* o, const char* s, size_t n) {
    size_t i, run = 0, maxrun = 0;
    int k;

    for (i = 0; i < n; i++) {
        run = s[i] == '`' ? run + 1 : 0;
        maxrun = run > maxrun ? run : maxrun;
    }

    for (k = 0; k <= (int)maxrun; k++) {
        pb_putc(o, '`');
    }

    if (n && (s[0] == '`' || s[n - 1] == '`' || s[0] == ' ')) {
        pb_putc(o, ' ');
    }

    pb_put(o, s, n);

    if (n && (s[0] == '`' || s[n - 1] == '`' || s[0] == ' ')) {
        pb_putc(o, ' ');
    }

    for (k = 0; k <= (int)maxrun; k++) {
        pb_putc(o, '`');
    }
}

/* the end of a link: ](url), or the whole link as <url> when its text is the address */
static void mx_end_link(mx* x) {
    pd_buf* o = x->o;
    size_t tl = o->n - x->link_at, ul = strlen(x->link);

    if (tl == ul && memcmp(o->p + x->link_at, x->link, ul) == 0 && strchr(x->link, ':') && !strpbrk(x->link, "<> ")) {
        o->p[x->link_at - 1] = '<';
        pb_putc(o, '>');
    } else {
        pb_printf(o, "](%s)", x->link);
    }

    x->in_link = 0;
}

static int mx_span(void* user, const pd_span* sp) {
    mx* x = (mx*)user;
    pd_buf* o = x->o;

    if (sp->is_object) {
        const pd_inline* ob = &sp->obj;

        switch (ob->kind) {
            case PD_INLINE_IMAGE: {
                const char* mime;
                const void* data;
                size_t len;

                if (pd_doc_resource(x->d, ob->resource, &mime, &data, &len) == PD_OK) {
                    fmt_to(x, 0);
                    pb_printf(o, "![](data:%s;base64,", mime);
                    pb_base64(o, (const unsigned char*)data, len);
                    pb_printf(o, "){width=%gpt height=%gpt}", ob->width / 65536.0, ob->height / 65536.0);
                }

                break;
            }

            case PD_INLINE_EQUATION:
                fmt_to(x, 0);
                pb_putc(o, '$');

                if (ob->source) {
                    pb_put(o, ob->source, (size_t)ob->source_len);
                }

                pb_putc(o, '$');
                break;

            case PD_INLINE_FIELD:
                if (ob->field == PD_FIELD_SEQ) {
                    pb_printf(o, "%d", (int)pd_numbers_at(&x->nb, x->para, sp->offset));
                } else if (ob->field == PD_FIELD_REF_NUMBER) {
                    pb_printf(o, "%d", (int)pd_numbers_ref(&x->nb, ob->target));
                }

                break;

            case PD_INLINE_FOOTNOTE:
                fmt_to(x, 0);

                if (x->nnotes < 4096) {
                    x->notes[x->nnotes++] = ob->target;
                }

                pb_printf(o, "[^%d]", (int)x->nnotes);
                break;

            case PD_INLINE_LINK:
                fmt_to(x, 0);

                if (x->in_link) {
                    mx_end_link(x);
                }

                if (ob->source && ob->source_len > 0 && (size_t)ob->source_len < sizeof(x->link)) {
                    size_t i, k = 0;

                    for (i = 0; i < (size_t)ob->source_len && k + 4 < sizeof(x->link); i++) {
                        if (ob->source[i] == ' ' || ob->source[i] == '(' || ob->source[i] == ')') {
                            k += (size_t)snprintf(x->link + k, sizeof(x->link) - k, "%%%02X", (unsigned char)ob->source[i]);
                        } else {
                            x->link[k++] = ob->source[i];
                        }
                    }

                    x->link[k] = '\0';
                    pb_putc(o, '[');
                    x->link_at = o->n;
                    x->in_link = 1;
                }

                break;

            case PD_INLINE_TAB:
                pb_putc(o, '\t');
                break;
        }

        return o->err;
    }

    {
        int f = span_flags(x, &sp->cp);
        size_t a = 0, e = sp->len;

        if (f & MF_CODE) {
            fmt_to(x, f & ~MF_CODE);
            code_span(o, sp->text, sp->len);
            return o->err;
        }

        /* emphasis cannot start or end at a space: keep spaces outside the markers */
        while (a < e && sp->text[a] == ' ') {
            a++;
        }

        while (e > a && sp->text[e - 1] == ' ') {
            e--;
        }

        if (a > 0) {
            fmt_to(x, 0);
            pb_put(o, sp->text, a);
        }

        if (e > a) {
            fmt_to(x, f);
            md_esc(x, sp->text + a, e - a, o->n == 0 || o->p[o->n - 1] == '\n');
        }

        if (e < sp->len) {
            fmt_to(x, 0);
            pb_put(o, sp->text + e, sp->len - e);
        }
    }

    return o->err;
}

static void mx_inline(mx* x, pd_block_id p) {
    x->para = p;
    x->in_link = 0;
    x->nopen = 0;
    pd_conv_base_props(x->d, p, &x->base);
    pd_conv_spans(x->d, p, mx_span, x);
    fmt_to(x, 0);

    if (x->in_link) {
        mx_end_link(x);
    }
}

static void mx_close_code(mx* x) {
    if (x->code_open) {
        pb_puts(x->o, "\n```\n");
        x->code_open = 0;
        x->prev_block = 2;
    }
}

static void blank(mx* x) {
    mx_close_code(x);

    if (x->prev_block) {
        pb_putc(x->o, '\n');
    }
}

static void mx_block(mx* x, pd_block_id id);

static void mx_para(mx* x, pd_block_id p) {
    pd_block_info bi;
    int32_t level = 0, i;
    int kind = pd_conv_list_kind(x->d, p, &level);

    pd_doc_block_info(x->d, p, &bi);

    if (bi.role == PD_ROLE_CODE) {
        const char* t;
        uint32_t n;

        if (!x->code_open) {
            blank(x);
            pb_puts(x->o, "```\n");
            x->code_open = 1;
        } else {
            pb_putc(x->o, '\n');
        }

        pd_doc_para_text(x->d, p, &t, &n);
        pb_put(x->o, t, n);
        x->ldepth = 0;
        return;
    }

    mx_close_code(x);

    if (kind) {
        char marker[16];
        pd_list_level lv[9];
        int32_t nlv = 0, start = 1;

        if (x->prev_block == 2) {
            pb_putc(x->o, '\n');
        }

        if (level > x->ldepth) {
            level = x->ldepth;  /* a level can only go one deeper than the last item */
        }

        if (pd_doc_list_info(x->d, bi.list, &nlv, lv) == PD_OK && level < nlv && lv[level].start >= 0) {
            start = lv[level].start;
        }

        if (level >= x->ldepth || x->lid[level] != bi.list) {
            /* a list of its own: counted from its start; right after another of the same kind at this
               level it takes the other marker, or Markdown would read the two as one */
            x->lalt[level] = level < x->ldepth && x->lid[level] != bi.list && x->lkind[level] == kind ?
                             !x->lalt[level] : 0;
            x->lnum[level] = start;
            x->lid[level] = bi.list;
            x->lkind[level] = kind;
        } else {
            x->lnum[level]++;
        }

        if (kind == 1) {
            snprintf(marker, sizeof(marker), "%c ", x->lalt[level] ? '*' : '-');
        } else {
            snprintf(marker, sizeof(marker), "%d%c ", x->lnum[level], x->lalt[level] ? ')' : '.');
        }

        for (i = 0; i < level; i++) {
            pb_printf(x->o, "%*s", i == 0 ? x->lw[0] : x->lw[i] - x->lw[i - 1], "");
        }

        pb_puts(x->o, marker);
        x->lw[level] = (level ? x->lw[level - 1] : 0) + (int)strlen(marker);
        x->ldepth = level + 1;
        mx_inline(x, p);
        pb_putc(x->o, '\n');
        x->prev_block = 1;
        return;
    }

    x->ldepth = 0;
    blank(x);

    switch (bi.role) {
        case PD_ROLE_TITLE:
            pb_puts(x->o, "# ");
            break;

        case PD_ROLE_HEADING:
            for (i = 0; i < bi.level; i++) {
                pb_putc(x->o, '#');
            }

            pb_putc(x->o, ' ');
            break;

        case PD_ROLE_QUOTE:
            strcpy(x->prefix, "> ");
            pb_puts(x->o, "> ");
            break;

        case PD_ROLE_EQUATION: {
            pd_inline o;
            const char* t;
            uint32_t n, k;

            pd_doc_para_text(x->d, p, &t, &n);

            for (k = 0; k + 2 < n; k++) {
                pd_pos at;

                at.block = p;
                at.offset = k;

                if ((unsigned char)t[k] == 0xEF && pd_doc_inline_at(x->d, at, &o) == PD_OK &&
                        o.kind == PD_INLINE_EQUATION && o.source) {
                    pb_puts(x->o, "$$");
                    pb_put(x->o, o.source, (size_t)o.source_len);
                    pb_puts(x->o, "$$\n");
                    x->prev_block = 2;
                    return;
                }
            }

            break;
        }
    }

    mx_inline(x, p);

    if (bi.role == PD_ROLE_HEADING || bi.role == PD_ROLE_TITLE) {    /* an anchor at its start: {#id} */
        pd_inline o;
        pd_pos at0;

        at0.block = p;
        at0.offset = 0;

        if (pd_doc_inline_at(x->d, at0, &o) == PD_OK && o.kind == PD_INLINE_BOOKMARK && o.name[0]) {
            pb_printf(x->o, " {#%s}", o.name);
        }
    }

    x->prefix[0] = '\0';
    pb_putc(x->o, '\n');
    x->prev_block = 2;
}

static void mx_table(mx* x, pd_block_id t) {
    pd_block_info ti, ri;
    int32_t r, c, k, ncols = 0;

    pd_doc_block_info(x->d, t, &ti);

    for (r = 0; r < ti.child_count; r++) {     /* columns: the widest row, spans counted */
        int32_t w = 0;

        pd_doc_block_info(x->d, pd_doc_child(x->d, t, r), &ri);

        for (c = 0; c < ri.child_count; c++) {
            pd_cell_props cp;

            pd_doc_cell_props(x->d, pd_doc_child(x->d, pd_doc_child(x->d, t, r), c), &cp);
            w += cp.col_span;
        }

        ncols = w > ncols ? w : ncols;
    }

    blank(x);
    x->in_table = 1;

    for (r = 0; r < ti.child_count; r++) {
        pd_block_id row = pd_doc_child(x->d, t, r);
        int32_t w = 0;

        pd_doc_block_info(x->d, row, &ri);
        pb_putc(x->o, '|');

        for (c = 0; c < ri.child_count; c++) {
            pd_block_id cell = pd_doc_child(x->d, row, c);
            pd_block_info ci;
            pd_cell_props cp;

            pd_doc_block_info(x->d, cell, &ci);
            pd_doc_cell_props(x->d, cell, &cp);
            pb_putc(x->o, ' ');

            for (k = 0; k < ci.child_count; k++) {
                pd_block_info pi;

                if (pd_doc_block_info(x->d, pd_doc_child(x->d, cell, k), &pi) == PD_OK && pi.kind == PD_BLOCK_PARAGRAPH) {
                    if (k > 0) {
                        pb_puts(x->o, "<br>");
                    }

                    mx_inline(x, pi.id);
                }
            }

            pb_puts(x->o, " |");

            for (k = 1; k < cp.col_span; k++) {
                pb_puts(x->o, " |");
            }

            w += cp.col_span;
        }

        for (; w < ncols; w++) {
            pb_puts(x->o, " |");
        }

        pb_putc(x->o, '\n');

        if (r == 0) {   /* each column's alignment, from its header cell */
            int32_t col = 0;

            pb_putc(x->o, '|');

            for (c = 0; c < ri.child_count && col < ncols; c++) {
                pd_block_id cell = pd_doc_child(x->d, row, c);
                pd_cell_props cp;
                pd_para_props pp;
                int al = -1;

                pd_doc_cell_props(x->d, cell, &cp);

                if (pd_doc_para_props(x->d, pd_doc_child(x->d, cell, 0), &pp) == PD_OK && (pp.mask & PD_PP_ALIGN)) {
                    al = pp.align;
                }

                for (k = 0; k < cp.col_span && col < ncols; k++, col++) {
                    pb_puts(x->o, al == PD_ALIGN_CENTER ? " :-: |" : al == PD_ALIGN_RIGHT ? " --: |" :
                            al == PD_ALIGN_LEFT ? " :-- |" : " --- |");
                }
            }

            for (; col < ncols; col++) {
                pb_puts(x->o, " --- |");
            }

            pb_putc(x->o, '\n');
        }
    }

    x->in_table = 0;
    x->prev_block = 2;
}

static void mx_block(mx* x, pd_block_id id) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(x->d, id, &bi) != PD_OK) {
        return;
    }

    switch (bi.kind) {
        case PD_BLOCK_PARAGRAPH:
            mx_para(x, id);
            break;

        case PD_BLOCK_TABLE:
            mx_close_code(x);
            x->ldepth = 0;
            mx_table(x, id);
            break;

        case PD_BLOCK_BREAK:
            blank(x);
            x->ldepth = 0;
            pb_puts(x->o, "<div style=\"break-after:page\"></div>\n");
            x->prev_block = 2;
            break;

        default:
            for (i = 0; i < bi.child_count; i++) {
                mx_block(x, pd_doc_child(x->d, id, i));
            }
    }
}

pd_status pd_md_export(const pd_doc* d, pd_buf* o) {
    mx* x = (mx*)calloc(1, sizeof(mx));
    int32_t i, k;

    if (!x) {
        return PD_ERR_NOMEM;
    }

    x->d = d;
    x->o = o;
    pd_numbers_init(&x->nb, d);
    mx_block(x, pd_doc_root(d));
    mx_close_code(x);

    for (i = 0; i < x->nnotes; i++) {   /* footnote definitions; continuation paragraphs indented */
        pd_block_info si;

        pd_doc_block_info(d, x->notes[i], &si);
        pb_printf(o, "\n[^%d]: ", (int)i + 1);

        for (k = 0; k < si.child_count; k++) {
            if (k > 0) {
                pb_puts(o, "\n\n    ");
            }

            mx_inline(x, pd_doc_child(d, x->notes[i], k));
        }

        pb_putc(o, '\n');
    }

    pd_numbers_free(&x->nb);
    free(x);
    return o->err ? PD_ERR_NOMEM : PD_OK;
}

/* ------------------------------------------------------------------ */
/* import: inlines                                                    */
/* ------------------------------------------------------------------ */

enum {
    N_TEXT = 0, N_DELIM, N_CODE, N_BREAK, N_LINK_OPEN, N_LINK_CLOSE, N_IMAGE, N_NOTE, N_MATH, N_TAG
};

typedef struct {
    int type;
    size_t a, b;                /* source range: text, code content, delimiter run, URL, ... */
    char ch;                    /* delimiter character */
    int count, orig;            /* delimiter: left, original length */
    int can_open, can_close;
    int ev[6];                  /* matched formats (MF_*), in matching order */
    int nev;
    int tag;                    /* N_TAG: +flag opens, -flag closes */
    pd_sp w, h;                 /* images */
    const char* url;            /* links and images by reference: the destination, in place of a..b */
    size_t ulen;
} mnode;

/* a link reference definition: [label]: destination "title" */
typedef struct {
    char label[128];            /* normalized: case folded, inner whitespace collapsed */
    char* url;
    size_t ulen;
} mref;

typedef struct {
    mref* v;
    int32_t n, cap;
} mrefs;

typedef struct {
    char label[64];
    size_t a, b;                /* the definition's text */
} mnote;

typedef struct {
    mnote* v;
    int32_t n, cap;
} mnote_list;

typedef struct {
    pd_bld* b;
    const char* s;              /* the text being parsed */
    const char* doc;            /* the whole document (footnote definitions) */
    mnote_list* notes;
    const mrefs* refs;
    int depth;
} mctx;

/* a label as CommonMark matches them: ASCII case folded, whitespace runs one space, trimmed */
static void ref_label(const char* s, size_t n, char* out, size_t cap) {
    size_t i, k = 0;
    int sp = 0;

    for (i = 0; i < n && k + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            sp = k > 0;
            continue;
        }

        if (sp && k + 2 < cap) {
            out[k++] = ' ';
        }

        sp = 0;
        out[k++] = (char)tolower(c);
    }

    out[k] = '\0';
}

static const mref* find_ref(const mrefs* r, const char* s, size_t n) {
    char key[128];
    int32_t i;

    if (!r || n == 0 || n > 999) {
        return NULL;
    }

    ref_label(s, n, key, sizeof(key));

    for (i = 0; key[0] && i < r->n; i++) {
        if (strcmp(r->v[i].label, key) == 0) {
            return &r->v[i];
        }
    }

    return NULL;
}

static int is_punct(int c) {
    return c >= 0 && c < 128 && ispunct(c);
}

static int is_space(int c) {
    return c <= 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int push_node(mnode** v, int32_t* n, int32_t* cap, const mnode* x) {
    if (pd_grow((void**)v, cap, (int64_t)*n + 1, sizeof(mnode))) {
        return -1;
    }

    (*v)[(*n)++] = *x;
    return 0;
}

/* the end of a link destination "(url "title")": returns the index after ')' or 0 */
static size_t link_dest(const char* s, size_t i, size_t n, size_t* ua, size_t* ub) {
    int depth = 0;

    if (i >= n || s[i] != '(') {
        return 0;
    }

    i++;

    while (i < n && (s[i] == ' ' || s[i] == '\n')) {
        i++;
    }

    if (i < n && s[i] == '<') {
        *ua = ++i;

        while (i < n && s[i] != '>' && s[i] != '\n') {
            i++;
        }

        if (i >= n || s[i] != '>') {
            return 0;
        }

        *ub = i++;
    } else {
        *ua = i;

        while (i < n && !is_space((unsigned char)s[i]) && !(s[i] == ')' && depth == 0)) {
            depth += s[i] == '(' ? 1 : s[i] == ')' ? -1 : 0;
            i += s[i] == '\\' && i + 1 < n ? 2 : 1;
        }

        *ub = i;
    }

    while (i < n && (s[i] == ' ' || s[i] == '\n')) {
        i++;
    }

    if (i < n && (s[i] == '"' || s[i] == '\'')) {     /* a title: skipped */
        char q = s[i++];

        while (i < n && s[i] != q) {
            i++;
        }

        i++;

        while (i < n && (s[i] == ' ' || s[i] == '\n')) {
            i++;
        }
    }

    return i < n && s[i] == ')' ? i + 1 : 0;
}

/* the matching ']' for '[' at i, skipping code spans and escapes */
static size_t close_bracket(const char* s, size_t i, size_t n) {
    int depth = 0;

    for (; i < n; i++) {
        if (s[i] == '\\') {
            i++;
        } else if (s[i] == '`') {
            size_t r = 0, k;

            while (i + r < n && s[i + r] == '`') {
                r++;
            }

            for (k = i + r; k + r <= n; k++) {
                size_t q = 0;

                while (k + q < n && s[k + q] == '`') {
                    q++;
                }

                if (q == r) {
                    break;
                }

                k += q;
            }

            i = k + r <= n ? k + r - 1 : i + r - 1;
        } else if (s[i] == '[') {
            depth++;
        } else if (s[i] == ']' && --depth == 0) {
            return i;
        }
    }

    return 0;
}

static const struct {
    const char* t;
    int flag;
} html_tags[] = {
    { "u", MF_UNDER }, { "ins", MF_UNDER }, { "sup", MF_SUP }, { "sub", MF_SUB }, { "b", MF_BOLD }, { "strong", MF_BOLD },
    { "i", MF_ITAL }, { "em", MF_ITAL }, { "s", MF_STRIKE }, { "del", MF_STRIKE }, { "code", MF_CODE }
};

/* tokenize inline content of s[a..b) into nodes */
static void tokenize(const char* s, size_t a, size_t b, mnode** v, int32_t* nv, int32_t* cap, const mrefs* refs) {
    size_t i = a, t = a;

    while (i < b) {
        char c = s[i];
        mnode x;

        memset(&x, 0, sizeof(x));

#define FLUSH() do { if (i > t) { mnode y; memset(&y, 0, sizeof(y)); y.type = N_TEXT; y.a = t; y.b = i; \
            push_node(v, nv, cap, &y); } } while (0)

        if (c == '\\' && i + 1 < b) {
            if (s[i + 1] == '\n') {     /* hard break */
                FLUSH();
                x.type = N_BREAK;
                push_node(v, nv, cap, &x);
                i += 2;
                t = i;
                continue;
            }

            if (is_punct((unsigned char)s[i + 1])) {
                FLUSH();
                x.type = N_TEXT;
                x.a = i + 1;
                x.b = i + 2;
                x.ch = 1;   /* literal, no entity decoding */
                push_node(v, nv, cap, &x);
                i += 2;
                t = i;
                continue;
            }
        }

        if (c == '\n') {    /* soft break, or hard with two trailing spaces */
            size_t k = i;

            while (k > t && s[k - 1] == ' ') {
                k--;
            }

            if (i - k >= 2) {
                size_t save = i;

                i = k;
                FLUSH();
                i = save;
                x.type = N_BREAK;
                push_node(v, nv, cap, &x);
            } else {
                size_t save = i;

                i = k;
                FLUSH();
                i = save;
                x.type = N_TEXT;
                x.a = i;
                x.b = i + 1;
                x.ch = 2;   /* a space */
                push_node(v, nv, cap, &x);
            }

            i++;

            while (i < b && s[i] == ' ') {
                i++;
            }

            t = i;
            continue;
        }

        if (c == '`') {     /* code span: a closing run of the same length */
            size_t r = 0, k;

            while (i + r < b && s[i + r] == '`') {
                r++;
            }

            for (k = i + r; k < b; k++) {
                size_t q = 0;

                while (k + q < b && s[k + q] == '`') {
                    q++;
                }

                if (q == r) {
                    break;
                }

                k += q ? q - 1 : 0;
            }

            if (k < b) {
                FLUSH();
                x.type = N_CODE;
                x.a = i + r;
                x.b = k;

                if (x.b - x.a >= 2 && s[x.a] == ' ' && s[x.b - 1] == ' ') {
                    x.a++;
                    x.b--;
                }

                push_node(v, nv, cap, &x);
                i = k + r;
                t = i;
                continue;
            }

            i += r;
            continue;
        }

        if (c == '*' || c == '_' || c == '~') {
            size_t r = 0;
            int before = i > a ? (unsigned char)s[i - 1] : ' ', after;
            int lf, rf;

            while (i + r < b && s[i + r] == c) {
                r++;
            }

            after = i + r < b ? (unsigned char)s[i + r] : ' ';
            lf = !is_space(after) && (!is_punct(after) || is_space(before) || is_punct(before));
            rf = !is_space(before) && (!is_punct(before) || is_space(after) || is_punct(after));
            FLUSH();
            x.type = N_DELIM;
            x.a = i;
            x.b = i + r;
            x.ch = c;
            x.count = x.orig = (int)r;

            if (c == '_') {
                x.can_open = lf && (!rf || is_punct(before));
                x.can_close = rf && (!lf || is_punct(after));
            } else {
                x.can_open = lf;
                x.can_close = rf;
            }

            push_node(v, nv, cap, &x);
            i += r;
            t = i;
            continue;
        }

        if (c == '$' && i + 1 < b && s[i + 1] != '$' && s[i + 1] != ' ') {     /* $math$ */
            size_t k = i + 1;

            while (k < b && !(s[k] == '$' && s[k - 1] != ' ' && s[k - 1] != '\\')) {
                k++;
            }

            if (k < b && k > i + 1 && (k + 1 >= b || !isdigit((unsigned char)s[k + 1]))) {
                FLUSH();
                x.type = N_MATH;
                x.a = i + 1;
                x.b = k;
                push_node(v, nv, cap, &x);
                i = k + 1;
                t = i;
                continue;
            }
        }

        if (c == '!' && i + 1 < b && s[i + 1] == '[') {     /* image */
            size_t rb = close_bracket(s, i + 1, b), ua = 0, ub = 0, e;

            if (rb && refs && !link_dest(s, rb + 1, b, &ua, &ub)) {     /* by reference */
                size_t la = i + 2, lb = rb, r2;
                const mref* r;

                e = rb + 1;

                if (rb + 1 < b && s[rb + 1] == '[' && (r2 = close_bracket(s, rb + 1, b)) != 0) {
                    if (r2 > rb + 2) {
                        la = rb + 2;
                        lb = r2;
                    }

                    e = r2 + 1;
                }

                if ((r = find_ref(refs, s + la, lb - la)) != NULL) {
                    FLUSH();
                    x.type = N_IMAGE;
                    x.url = r->url;
                    x.ulen = r->ulen;
                    push_node(v, nv, cap, &x);
                    i = e;
                    t = i;
                    continue;
                }
            }

            if (rb && (e = link_dest(s, rb + 1, b, &ua, &ub)) != 0) {
                FLUSH();
                x.type = N_IMAGE;
                x.a = ua;
                x.b = ub;

                if (e < b && s[e] == '{') {     /* {width=.. height=..} */
                    size_t z = e;

                    while (z < b && s[z] != '}') {
                        z++;
                    }

                    if (z < b) {
                        char attr[128];
                        const char* w, *h;
                        size_t al = z - e < sizeof(attr) - 1 ? z - e : sizeof(attr) - 1;

                        memcpy(attr, s + e, al);
                        attr[al] = '\0';

                        if ((w = strstr(attr, "width=")) != NULL) {
                            x.w = (pd_sp)(atof(w + 6) * (strstr(w, "pt") == w + 6 + strspn(w + 6, "0123456789.") ? 1.0 :
                                                         0.75) * 65536);
                        }

                        if ((h = strstr(attr, "height=")) != NULL) {
                            x.h = (pd_sp)(atof(h + 7) * (strstr(h, "pt") == h + 7 + strspn(h + 7, "0123456789.") ? 1.0 :
                                                         0.75) * 65536);
                        }

                        e = z + 1;
                    }
                }

                push_node(v, nv, cap, &x);
                i = e;
                t = i;
                continue;
            }
        }

        if (c == '[' && i + 2 < b && s[i + 1] == '^') {    /* footnote reference */
            size_t k = i + 2;

            while (k < b && s[k] != ']' && !is_space((unsigned char)s[k])) {
                k++;
            }

            if (k < b && s[k] == ']' && k > i + 2) {
                FLUSH();
                x.type = N_NOTE;
                x.a = i + 2;
                x.b = k;
                push_node(v, nv, cap, &x);
                i = k + 1;
                t = i;
                continue;
            }
        }

        if (c == '[') {     /* link: [text](dest) */
            size_t rb = close_bracket(s, i, b), ua = 0, ub = 0, e;

            if (rb && (e = link_dest(s, rb + 1, b, &ua, &ub)) != 0) {
                FLUSH();
                x.type = N_LINK_OPEN;
                x.a = ua;
                x.b = ub;
                push_node(v, nv, cap, &x);
                tokenize(s, i + 1, rb, v, nv, cap, refs);
                memset(&x, 0, sizeof(x));
                x.type = N_LINK_CLOSE;
                push_node(v, nv, cap, &x);
                i = e;
                t = i;
                continue;
            }

            if (rb && refs) {   /* by reference: [text][label], [text][], [label] */
                size_t la = i + 1, lb = rb, r2;
                const mref* r;

                e = rb + 1;

                if (rb + 1 < b && s[rb + 1] == '[' && (r2 = close_bracket(s, rb + 1, b)) != 0) {
                    if (r2 > rb + 2) {
                        la = rb + 2;
                        lb = r2;
                    }

                    e = r2 + 1;
                }

                if ((r = find_ref(refs, s + la, lb - la)) != NULL) {
                    FLUSH();
                    x.type = N_LINK_OPEN;
                    x.url = r->url;
                    x.ulen = r->ulen;
                    push_node(v, nv, cap, &x);
                    tokenize(s, i + 1, rb, v, nv, cap, refs);
                    memset(&x, 0, sizeof(x));
                    x.type = N_LINK_CLOSE;
                    push_node(v, nv, cap, &x);
                    i = e;
                    t = i;
                    continue;
                }
            }
        }

        if (c == '<') {     /* autolink or a few inline HTML tags */
            size_t k = i + 1;

            while (k < b && s[k] != '>' && s[k] != '<' && s[k] != ' ' && s[k] != '\n') {
                k++;
            }

            if (k < b && s[k] == '>' && (memchr(s + i, ':', k - i) && (s[i + 1] != '/'))) {
                FLUSH();
                x.type = N_LINK_OPEN;
                x.a = i + 1;
                x.b = k;
                push_node(v, nv, cap, &x);
                memset(&x, 0, sizeof(x));
                x.type = N_TEXT;
                x.a = i + 1;
                x.b = k;
                x.ch = 1;
                push_node(v, nv, cap, &x);
                memset(&x, 0, sizeof(x));
                x.type = N_LINK_CLOSE;
                push_node(v, nv, cap, &x);
                i = k + 1;
                t = i;
                continue;
            }

            {
                int close = i + 1 < b && s[i + 1] == '/';
                size_t ns = i + 1 + close, ne = ns, z;
                size_t q;

                while (ne < b && isalnum((unsigned char)s[ne])) {
                    ne++;
                }

                for (z = ne; z < b && s[z] != '>' && s[z] != '<'; z++) {
                }

                if (z < b && s[z] == '>' && ne > ns) {
                    int flag = 0, br = 0;

                    for (q = 0; q < sizeof(html_tags) / sizeof(html_tags[0]); q++) {
                        if (strlen(html_tags[q].t) == ne - ns && strncmp(s + ns, html_tags[q].t, ne - ns) == 0) {
                            flag = html_tags[q].flag;
                        }
                    }

                    br = ne - ns == 2 && strncmp(s + ns, "br", 2) == 0;

                    if (flag || br || (ne - ns <= 10 && isalpha((unsigned char)s[ns]))) {
                        FLUSH();

                        if (br) {
                            x.type = N_BREAK;
                            push_node(v, nv, cap, &x);
                        } else if (flag) {
                            x.type = N_TAG;
                            x.tag = close ? -flag : flag;
                            push_node(v, nv, cap, &x);
                        }

                        i = z + 1;
                        t = i;
                        continue;
                    }
                }
            }
        }

        i++;
#undef FLUSH
    }

    if (i > t) {
        mnode y;

        memset(&y, 0, sizeof(y));
        y.type = N_TEXT;
        y.a = t;
        y.b = i;
        push_node(v, nv, cap, &y);
    }
}

/* the CommonMark "process emphasis" procedure over the delimiter runs */
static void emphasis(mnode* v, int32_t n) {
    int32_t c, o, k;

    for (c = 0; c < n; c++) {
        mnode* cl = &v[c];

        while (cl->type == N_DELIM && cl->can_close && cl->count > 0) {
            int found = 0;

            for (o = c - 1; o >= 0; o--) {
                mnode* op = &v[o];

                if (op->type == N_LINK_OPEN || op->type == N_LINK_CLOSE) {
                    break;      /* emphasis never crosses a link boundary */
                }

                if (op->type != N_DELIM || op->ch != cl->ch || !op->can_open || op->count <= 0) {
                    continue;
                }

                if ((op->can_close || cl->can_open) && (op->orig + cl->orig) % 3 == 0 &&
                        !(op->orig % 3 == 0 && cl->orig % 3 == 0)) {
                    continue;   /* rule of three */
                }

                if (cl->ch == '~' && (op->count < 2 || cl->count < 2)) {
                    continue;
                }

                {
                    int use = cl->ch == '~' ? 2 : (op->count >= 2 && cl->count >= 2 ? 2 : 1);
                    int f = cl->ch == '~' ? MF_STRIKE : use == 2 ? MF_BOLD : MF_ITAL;

                    op->count -= use;
                    cl->count -= use;

                    if (op->nev < 6) {
                        op->ev[op->nev++] = f;
                    }

                    if (cl->nev < 6) {
                        cl->ev[cl->nev++] = f;
                    }

                    for (k = o + 1; k < c; k++) {   /* delimiters between can no longer match */
                        if (v[k].type == N_DELIM && v[k].count > 0) {
                            v[k].can_open = v[k].can_close = 0;
                        }
                    }
                }

                found = 1;
                break;
            }

            if (!found) {
                break;
            }
        }
    }
}

typedef struct {
    int bold, ital, strike, under, sup, sub, code;
} mstate;

static void apply_state(pd_bld* b, const mstate* st) {
    pd_char_props cp;

    memset(&cp, 0, sizeof(cp));

    if (st->bold > 0) {
        cp.mask |= PD_CP_WEIGHT;
        cp.weight = 700;
    }

    if (st->ital > 0) {
        cp.mask |= PD_CP_ITALIC;
        cp.italic = 1;
    }

    if (st->strike > 0) {
        cp.mask |= PD_CP_STRIKE;
        cp.strike = 1;
    }

    if (st->under > 0) {
        cp.mask |= PD_CP_UNDERLINE;
        cp.underline = 1;
    }

    if (st->sup > 0 || st->sub > 0) {
        cp.mask |= PD_CP_SHIFT;
        cp.shift = st->sup > 0 ? PD_SHIFT_SUPER : PD_SHIFT_SUB;
    }

    if (st->code > 0) {
        cp.mask |= PD_CP_FAMILY;
        strcpy(cp.family, "monospace");
    }

    bld_set_format(b, &cp);
}

static void flag_add(mstate* st, int f, int d) {
    st->bold += f == MF_BOLD ? d : 0;
    st->ital += f == MF_ITAL ? d : 0;
    st->strike += f == MF_STRIKE ? d : 0;
    st->under += f == MF_UNDER ? d : 0;
    st->sup += f == MF_SUP ? d : 0;
    st->sub += f == MF_SUB ? d : 0;
    st->code += f == MF_CODE ? d : 0;
}

static void md_inline(mctx* m, size_t a, size_t b);

static void decoded_text(pd_bld* b, const char* s, size_t n) {
    pd_buf t;

    memset(&t, 0, sizeof(t));
    mu_decode(s, n, &t);
    bld_text(b, t.p ? t.p : "", t.n);
    pb_free(&t);
}

static void add_image(mctx* m, const mnode* x) {
    const char* u = x->url ? x->url : m->s + x->a;
    size_t un = x->url ? x->ulen : x->b - x->a;
    char mime[64];
    const char* semi, *data;

    if (un > 5 && strncmp(u, "data:", 5) == 0 && (semi = (const char*)memchr(u, ';', un)) != NULL &&
            (size_t)(semi - u) - 5 < sizeof(mime) && un - (size_t)(semi - u) > 8 && strncmp(semi, ";base64,", 8) == 0) {
        size_t ml = (size_t)(semi - u) - 5, dl;
        unsigned char* bytes;
        pd_inline o;

        memcpy(mime, u + 5, ml);
        mime[ml] = '\0';
        data = semi + 8;
        dl = (size_t)(u + un - data);

        if ((bytes = (unsigned char*)malloc(dl + 4)) != NULL) {
            size_t nb = pd_base64_decode(data, dl, bytes);

            memset(&o, 0, sizeof(o));
            o.kind = PD_INLINE_IMAGE;
            o.width = x->w > 0 ? x->w : PD_PT(100);
            o.height = x->h > 0 ? x->h : PD_PT(100);

            if (nb && pd_doc_add_resource(m->b->d, mime, bytes, nb, &o.resource) == PD_OK) {
                bld_inline(m->b, &o);
            }

            free(bytes);
        }
    }
}

/* which events of each delimiter run open (+1) and which close (-1): pair them in order */
static void pair_events(const mnode* v, int32_t n, int (*role)[6]) {
    int32_t sp = 0, q, i;
    int sf[256], sn[256], se[256];

    for (i = 0; i < n; i++) {
        int j;

        if (v[i].type != N_DELIM) {
            continue;
        }

        for (j = 0; j < v[i].nev; j++) {
            for (q = sp - 1; q >= 0; q--) {
                if (sf[q] == v[i].ev[j] && v[sn[q]].ch == v[i].ch && sn[q] != i) {
                    break;
                }
            }

            if (q >= 0) {
                role[i][j] = -1;
                role[sn[q]][se[q]] = 1;
                memmove(&sf[q], &sf[q + 1], (size_t)(sp - q - 1) * sizeof(int));
                memmove(&sn[q], &sn[q + 1], (size_t)(sp - q - 1) * sizeof(int));
                memmove(&se[q], &se[q + 1], (size_t)(sp - q - 1) * sizeof(int));
                sp--;
            } else if (sp < 256) {
                sf[sp] = v[i].ev[j];
                sn[sp] = i;
                se[sp] = j;
                sp++;
            }
        }
    }
}

/* a footnote's body: its paragraphs are separated by blank lines */
static void note_body(mctx* m, size_t a, size_t b) {
    size_t p0 = a;

    while (p0 < b) {
        size_t p1 = p0, t0;

        while (p1 < b) {    /* up to a blank line */
            if (m->s[p1] == '\n') {
                size_t k = p1 + 1;

                while (k < b && (m->s[k] == ' ' || m->s[k] == '\t')) {
                    k++;
                }

                if (k < b && m->s[k] == '\n') {
                    break;
                }
            }

            p1++;
        }

        for (t0 = p0; t0 < p1 && (m->s[t0] == ' ' || m->s[t0] == '\n'); t0++) {
        }

        if (t0 < p1) {
            bld_begin_para(m->b);
            md_inline(m, t0, p1);
            bld_end_para(m->b);
        }

        p0 = p1 + 1;
    }
}

static void md_inline(mctx* m, size_t a, size_t b) {
    mnode* v = NULL;
    int32_t n = 0, cap = 0, i;
    int (*role)[6];
    mstate st;
    pd_char_props saved = m->b->cp;

    tokenize(m->s, a, b, &v, &n, &cap, m->refs);
    emphasis(v, n);

    if ((role = (int (*)[6])calloc((size_t)n + 1, sizeof(int[6]))) == NULL) {
        free(v);
        return;
    }

    pair_events(v, n, role);
    memset(&st, 0, sizeof(st));
    apply_state(m->b, &st);

    for (i = 0; i < n; i++) {
        const mnode* x = &v[i];
        int j;

        switch (x->type) {
            case N_TEXT:
                if (x->ch == 2) {
                    bld_text(m->b, " ", 1);
                } else if (x->ch == 1) {
                    bld_text(m->b, m->s + x->a, x->b - x->a);
                } else {
                    decoded_text(m->b, m->s + x->a, x->b - x->a);
                }

                break;

            case N_DELIM:

                /* closing events (innermost first), the unused characters, then opening (outermost first) */
                for (j = 0; j < x->nev; j++) {
                    if (role[i][j] == -1) {
                        flag_add(&st, x->ev[j], -1);
                    }
                }

                apply_state(m->b, &st);

                if (x->count > 0) {
                    bld_text(m->b, m->s + x->a, (size_t)x->count);
                }

                for (j = x->nev - 1; j >= 0; j--) {
                    if (role[i][j] == 1) {
                        flag_add(&st, x->ev[j], 1);
                    }
                }

                apply_state(m->b, &st);
                break;

            case N_CODE: {
                mstate c2 = st;

                c2.code++;
                apply_state(m->b, &c2);
                bld_text(m->b, m->s + x->a, x->b - x->a);
                apply_state(m->b, &st);
                break;
            }

            case N_BREAK:
                bld_text(m->b, "\n", 1);
                break;

            case N_TAG:
                flag_add(&st, x->tag > 0 ? x->tag : -x->tag, x->tag > 0 ? 1 : -1);
                apply_state(m->b, &st);
                break;

            case N_MATH: {
                pd_inline o;

                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_EQUATION;
                o.source = m->s + x->a;
                o.source_len = (int32_t)(x->b - x->a);
                o.width = pd_conv_equation_width((size_t)(x->b - x->a));
                o.height = PD_PT(8);
                o.depth = PD_PT(2);
                bld_inline(m->b, &o);
                break;
            }

            case N_IMAGE:
                add_image(m, x);
                break;

            case N_LINK_OPEN: {
                pd_inline o;
                pd_buf url;
                size_t r, w = 0;

                memset(&url, 0, sizeof(url));
                mu_decode(x->url ? x->url : m->s + x->a, x->url ? x->ulen : x->b - x->a, &url);

                for (r = 0; r < url.n; r++) {   /* undo what the exporter encoded, and escapes */
                    unsigned hx;

                    if (url.p[r] == '%' && r + 2 < url.n && sscanf(url.p + r + 1, "%2x", &hx) == 1 &&
                            (hx == ' ' || hx == '(' || hx == ')')) {
                        url.p[w++] = (char)hx;
                        r += 2;
                    } else if (url.p[r] == '\\' && r + 1 < url.n && is_punct((unsigned char)url.p[r + 1])) {
                        url.p[w++] = url.p[++r];
                    } else {
                        url.p[w++] = url.p[r];
                    }
                }

                url.n = w;
                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_LINK;
                o.source = url.p ? url.p : "";
                o.source_len = (int32_t)url.n;

                if (url.n) {
                    bld_inline(m->b, &o);
                }

                pb_free(&url);
                break;
            }

            case N_LINK_CLOSE: {
                pd_inline o;

                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_LINK;
                bld_inline(m->b, &o);
                break;
            }

            case N_NOTE: {
                mnote_list* nl = m->notes;
                int32_t q;

                for (q = 0; nl && q < nl->n; q++) {
                    if (strlen(nl->v[q].label) == x->b - x->a && strncmp(nl->v[q].label, m->s + x->a, x->b - x->a) == 0) {
                        break;
                    }
                }

                if (nl && q < nl->n && m->depth < 4) {
                    pd_char_props keep = m->b->cp;
                    mctx nm = *m;

                    nm.s = m->doc;      /* definitions are ranges of the whole document */
                    nm.depth = m->depth + 1;
                    bld_footnote_begin(m->b);
                    note_body(&nm, nl->v[q].a, nl->v[q].b);
                    bld_footnote_end(m->b);
                    m->b->cp = keep;
                } else {
                    bld_text(m->b, m->s + x->a - 2, x->b - x->a + 3);
                }

                break;
            }
        }
    }

    free(role);
    bld_set_format(m->b, &saved);
    free(v);
}

/* ------------------------------------------------------------------ */
/* import: blocks                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t a, b;                /* line without its newline */
} mline;

static int contains(const char* s, size_t n, const char* w) {
    size_t i, k = strlen(w);

    for (i = 0; i + k <= n; i++) {
        if (memcmp(s + i, w, k) == 0) {
            return 1;
        }
    }

    return 0;
}

static size_t indent_of(const char* s, size_t a, size_t b) {
    size_t i = a, col = 0;

    while (i < b && (s[i] == ' ' || s[i] == '\t')) {
        col += s[i] == '\t' ? 4 - col % 4 : 1;
        i++;
    }

    return col;
}

static size_t skip_ws(const char* s, size_t a, size_t b) {
    while (a < b && (s[a] == ' ' || s[a] == '\t')) {
        a++;
    }

    return a;
}

static int blank_line(const char* s, size_t a, size_t b) {
    return skip_ws(s, a, b) == b;
}

/* a list marker at i: returns its length incl. the following space, kind 1 bullet / 2 ordered */
static size_t list_marker_c(const char* s, size_t i, size_t b, int* kind, int* number, char* mark);

static size_t list_marker(const char* s, size_t i, size_t b, int* kind, int* number) {
    char mark;

    return list_marker_c(s, i, b, kind, number, &mark);
}

/* a list item's marker: its length with the space after it, 0 if none; mark is the bullet
   character or the delimiter after the number, which tells one list from the next */
static size_t list_marker_c(const char* s, size_t i, size_t b, int* kind, int* number, char* mark) {
    if (i < b && (s[i] == '-' || s[i] == '*' || s[i] == '+') && (i + 1 == b || s[i + 1] == ' ' || s[i + 1] == '\t')) {
        /* not a thematic break */
        size_t k = i, cnt = 0;

        while (k < b && (s[k] == s[i] || s[k] == ' ')) {
            cnt += s[k] == s[i];
            k++;
        }

        if (k == b && cnt >= 3) {
            return 0;
        }

        *kind = 1;
        *mark = s[i];
        return i + 1 == b ? 1 : 2;
    }

    if (i < b && isdigit((unsigned char)s[i])) {
        size_t k = i;

        while (k < b && k - i < 9 && isdigit((unsigned char)s[k])) {
            k++;
        }

        if (k < b && (s[k] == '.' || s[k] == ')') && (k + 1 == b || s[k + 1] == ' ')) {
            *kind = 2;
            *number = atoi(s + i);
            *mark = s[k];
            return k + 1 - i + (k + 1 < b);
        }
    }

    return 0;
}

static int thematic(const char* s, size_t a, size_t b) {
    size_t i = skip_ws(s, a, b), cnt = 0;
    char c = i < b ? s[i] : 0;

    if (c != '-' && c != '*' && c != '_') {
        return 0;
    }

    for (; i < b; i++) {
        if (s[i] == c) {
            cnt++;
        } else if (s[i] != ' ' && s[i] != '\t') {
            return 0;
        }
    }

    return cnt >= 3;
}

/* a GFM delimiter row: | --- | :-: | */
static int table_delim(const char* s, size_t a, size_t b) {
    size_t i = skip_ws(s, a, b);
    int cells = 0, dash = 0;

    for (; i < b; i++) {
        if (s[i] == '-') {
            dash = 1;
        } else if (s[i] == '|') {
            cells += dash;
            dash = 0;
        } else if (s[i] != ':' && s[i] != ' ' && s[i] != '\t') {
            return 0;
        }
    }

    return cells + dash > 0 && memchr(s + a, '-', b - a) != NULL;
}

/* split a table row into cells (ranges), honouring \| and code spans */
static int32_t table_cells(const char* s, size_t a, size_t b, size_t* ca, size_t* cb, int32_t cap) {
    size_t i = skip_ws(s, a, b), start;
    int32_t n = 0;

    while (b > i && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
        b--;
    }

    if (i < b && s[i] == '|') {
        i++;
    }

    if (b > i && s[b - 1] == '|' && (b < 2 || s[b - 2] != '\\')) {
        b--;
    }

    start = i;

    for (; i <= b; i++) {
        if (i == b || (s[i] == '|' && (i == 0 || s[i - 1] != '\\'))) {
            if (n < cap) {
                size_t x = skip_ws(s, start, i), y = i;

                while (y > x && (s[y - 1] == ' ' || s[y - 1] == '\t')) {
                    y--;
                }

                ca[n] = x;
                cb[n] = y;
                n++;
            }

            start = i + 1;
        } else if (s[i] == '`') {
            size_t k = i + 1;

            while (k < b && s[k] != '`') {
                k++;
            }

            i = k < b ? k : i;
        }
    }

    return n;
}

static int fence_start(const char* s, size_t a, size_t b, char* fc, size_t* flen) {
    size_t i = skip_ws(s, a, b), k = i;

    if (i - a > 3 || i >= b || (s[i] != '`' && s[i] != '~')) {
        return 0;
    }

    while (k < b && s[k] == s[i]) {
        k++;
    }

    if (k - i < 3 || (s[i] == '`' && memchr(s + k, '`', b - k))) {
        return 0;
    }

    *fc = s[i];
    *flen = k - i;
    return 1;
}

typedef struct {
    size_t content;             /* column where the item's content starts */
    int kind;
    pd_list_id id;
} mlist;

/* A heading's {#id} at the end of s[a..e): a bookmark of that name at the
   heading's start, and the end of the heading's text before it */
static size_t heading_id(pd_bld* b, const char* s, size_t a, size_t e) {
    size_t te = e, k;
    pd_inline o;

    while (te > a && (s[te - 1] == ' ' || s[te - 1] == '\t')) {
        te--;
    }

    if (te < a + 3 || s[te - 1] != '}') {
        return e;
    }

    for (k = te - 1; k > a && s[k] != '{'; k--) {
    }

    if (s[k] != '{' || s[k + 1] != '#' || te - k - 3 >= sizeof(o.name) || te - k - 3 == 0) {
        return e;
    }

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_BOOKMARK;
    memcpy(o.name, s + k + 2, te - k - 3);

    if (strpbrk(o.name, " \t}{")) {    /* {#id .class}: only the id */
        o.name[strcspn(o.name, " \t}{")] = '\0';
    }

    bld_inline(b, &o);

    while (k > a && (s[k - 1] == ' ' || s[k - 1] == '\t')) {
        k--;
    }

    return k;
}

/* A link reference definition on one line: [label]: destination "title",
   up to three spaces in. Added to refs (the first of a label wins); 0 when
   the line is not one. */
static int ref_def(const char* s, size_t a, size_t e, mrefs* refs) {
    size_t i = skip_ws(s, a, e), k, ua, ub;
    char key[128];
    int32_t q, cap = refs->cap;

    if (i - a > 3 || i + 3 >= e || s[i] != '[' || s[i + 1] == '^') {
        return 0;
    }

    for (k = i + 1; k < e && s[k] != ']'; k++) {
        if (s[k] == '[' || (s[k] == '\\' && ++k >= e)) {
            return 0;
        }
    }

    if (k + 1 >= e || s[k + 1] != ':' || k == i + 1 || k - i - 1 > 999) {
        return 0;
    }

    ref_label(s + i + 1, k - i - 1, key, sizeof(key));
    k = skip_ws(s, k + 2, e);

    if (k >= e) {
        return 0;
    }

    if (s[k] == '<') {
        ua = ++k;

        while (k < e && s[k] != '>') {
            k++;
        }

        if (k >= e) {
            return 0;
        }

        ub = k++;
    } else {
        ua = k;

        while (k < e && s[k] != ' ' && s[k] != '\t') {
            k++;
        }

        ub = k;
    }

    k = skip_ws(s, k, e);

    if (k < e) {    /* a title, then nothing */
        char q0 = s[k], q1 = q0 == '(' ? ')' : q0;

        if (q0 != '"' && q0 != '\'' && q0 != '(') {
            return 0;
        }

        for (k++; k < e && s[k] != q1; k++) {
        }

        if (k >= e || skip_ws(s, k + 1, e) != e) {
            return 0;
        }
    }

    for (q = 0; q < refs->n; q++) {
        if (strcmp(refs->v[q].label, key) == 0) {
            return 1;
        }
    }

    if (!key[0] || pd_grow((void**)&refs->v, &cap, (int64_t)refs->n + 1, sizeof(mref))) {
        return 1;
    }

    refs->cap = cap;
    memset(&refs->v[refs->n], 0, sizeof(mref));
    strcpy(refs->v[refs->n].label, key);

    if ((refs->v[refs->n].url = (char*)malloc(ub - ua + 1)) != NULL) {
        memcpy(refs->v[refs->n].url, s + ua, ub - ua);
        refs->v[refs->n].url[ub - ua] = '\0';
        refs->v[refs->n].ulen = ub - ua;
        refs->n++;
    }

    return 1;
}

/* The list one list in the text is. Markdown numbers each list from its own
   first item and starts a new count in every list nested under an item, so
   each gets a definition of its own; its levels are all of one kind, and
   the level it sits at starts where its first item does. */
static pd_list_id md_list(pd_bld* b, int kind, int32_t level, int start) {
    static const char* bullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };
    pd_list_level lv[9];
    pd_list_id id = 0;
    int32_t i;

    memset(lv, 0, sizeof(lv));

    for (i = 0; i < 9; i++) {
        lv[i].format = kind == 1 ? PD_NUM_BULLET : PD_NUM_DECIMAL;
        lv[i].start = i == level && start >= 0 ? start : 1;
        lv[i].indent = PD_PT(18) * (i + 1);
        lv[i].hanging = PD_PT(18);

        if (kind == 1) {
            strcpy(lv[i].text, bullets[i % 3]);
        } else {
            snprintf(lv[i].text, sizeof(lv[i].text), "%%%d.", (int)i + 1);
        }
    }

    pd_doc_list_define(b->d, 9, lv, &id);
    return id;
}

pd_status pd_md_import(pd_doc* d, const char* s, size_t n) {
    pd_bld b;
    mctx m;
    mline* lines = NULL;
    int32_t nl = 0, capl = 0, i;
    mnote_list notes;
    mrefs refs;
    uint8_t* skip;
    mlist ls[10];
    mlist last[10];             /* the list last open at each level, which a next item continues */
    char lmark[10];
    int32_t nls = 0, seen = 0;  /* seen: levels of last[] still current */
    char mark = 0;
    pd_buf para;                /* the paragraph's lines, joined with \n */
    int para_kind = 0;          /* 0 body, 1 quote */
    size_t i0 = 0;

    memset(&notes, 0, sizeof(notes));
    memset(&refs, 0, sizeof(refs));
    memset(&para, 0, sizeof(para));

    /* lines */
    while (i0 <= n) {
        size_t e = i0;
        mline ln;

        while (e < n && s[e] != '\n') {
            e++;
        }

        ln.a = i0;
        ln.b = e > i0 && s[e - 1] == '\r' ? e - 1 : e;

        if (pd_grow((void**)&lines, &capl, (int64_t)nl + 1, sizeof(mline))) {
            free(lines);
            return PD_ERR_NOMEM;
        }

        lines[nl++] = ln;
        i0 = e + 1;

        if (e >= n) {
            break;
        }
    }

    skip = (uint8_t*)calloc((size_t)nl + 1, 1);

    if (!skip) {
        free(lines);
        return PD_ERR_NOMEM;
    }

    /* footnote definitions: [^label]: text, with indented continuation lines */
    for (i = 0; i < nl; i++) {
        size_t a = lines[i].a, e = lines[i].b, k;

        if (e - a > 4 && s[a] == '[' && s[a + 1] == '^') {
            for (k = a + 2; k < e && s[k] != ']'; k++) {
            }

            if (k + 1 < e && s[k + 1] == ':' && k - a - 2 < 64 && k > a + 2) {
                mnote x;
                int32_t j = i + 1, cap = notes.cap;

                memset(&x, 0, sizeof(x));
                memcpy(x.label, s + a + 2, k - a - 2);
                x.a = skip_ws(s, k + 2, e);
                skip[i] = 1;

                while (j < nl && (blank_line(s, lines[j].a, lines[j].b) ? j + 1 < nl &&
                                  indent_of(s, lines[j + 1].a, lines[j + 1].b) >= 4 : indent_of(s, lines[j].a, lines[j].b) >= 2)) {
                    skip[j++] = 1;
                }

                x.b = lines[j - 1].b;

                if (!pd_grow((void**)&notes.v, &cap, (int64_t)notes.n + 1, sizeof(mnote))) {
                    notes.cap = cap;
                    notes.v[notes.n++] = x;
                }

                i = j - 1;
            }
        }
    }

    /* link reference definitions: where a paragraph could start, outside code */
    {
        int fenced = 0, can = 1;
        char f0;
        size_t fl;

        for (i = 0; i < nl; i++) {
            size_t a = lines[i].a, e = lines[i].b;

            if (skip[i]) {
                continue;
            }

            if (fence_start(s, a, e, &f0, &fl)) {
                fenced = !fenced;
                can = 0;
                continue;
            }

            if (fenced) {
                continue;
            }

            if (blank_line(s, a, e)) {
                can = 1;
            } else if (can && ref_def(s, a, e, &refs)) {
                skip[i] = 1;
            } else {
                can = s[skip_ws(s, a, e)] == '#';   /* after a heading, a definition may follow */
            }
        }
    }

    bld_init(&b, d);
    m.b = &b;
    m.s = s;
    m.doc = s;
    m.notes = &notes;
    m.refs = &refs;
    m.depth = 0;

    for (i = 0; i < nl; i++) {
        size_t a = lines[i].a, e = lines[i].b, ind, c;
        int kind = 0, number = 0;
        size_t ml;
        char fc;
        size_t flen;

        if (skip[i]) {
            continue;
        }

        /* the end of the paragraph collected so far */
        {
            int ends = blank_line(s, a, e);
            size_t c0 = skip_ws(s, a, e);

            if (!ends && para.n) {
                /* setext heading underline */
                size_t k = c0;

                if (k < e && (s[k] == '=' || s[k] == '-') && para_kind == 0 && nls == 0) {
                    char u = s[k];

                    while (k < e && s[k] == u) {
                        k++;
                    }

                    if (skip_ws(s, k, e) == e && (u == '=' || k - c0 >= 2)) {
                        bld_para_style(&b, u == '=' ? "Heading 1" : "Heading 2", PD_ROLE_HEADING, u == '=' ? 1 : 2);
                        para_kind = 2;
                    }
                }
            }

            if (ends || para_kind == 2 || (para.n && (s[c0] == '#' || fence_start(s, a, e, &fc, &flen) ||
                                           (thematic(s, a, e)) || (list_marker(s, c0, e, &kind, &number) &&
                                                   (nls > 0 || kind == 1 || number == 1)) || (s[c0] == '>') != (para_kind == 1) ||
                                           (s[c0] == '|' && i + 1 < nl && table_delim(s, lines[i + 1].a, lines[i + 1].b))))) {
                if (para.n) {
                    if (para_kind == 1) {
                        bld_para_style(&b, "Quote", PD_ROLE_QUOTE, 0);
                    }

                    if (nls > 0) {
                        bld_list(&b, ls[nls - 1].kind, nls - 1);
                        b.list_id = ls[nls - 1].id;
                    }

                    if (para_kind == 2) {
                        /* style already set */
                    }

                    {
                        mctx pm = m;
                        char* text = (char*)malloc(para.n + 1);

                        if (text) {
                            size_t te = para.n;

                            memcpy(text, para.p, para.n);
                            text[para.n] = '\0';
                            pm.s = text;
                            bld_begin_para(&b);

                            if (para_kind == 2) {
                                te = heading_id(&b, text, 0, te);
                            }

                            md_inline(&pm, 0, te);
                            bld_end_para(&b);
                            free(text);
                        }
                    }

                    para.n = 0;
                }

                if (para_kind == 2) {
                    para_kind = 0;
                    continue;
                }

                para_kind = 0;

                if (ends) {
                    /* a blank line ends lists unless the next line is indented into the item */
                    int32_t j = i + 1;

                    while (j < nl && blank_line(s, lines[j].a, lines[j].b)) {
                        j++;
                    }

                    if (j < nl && nls > 0) {
                        size_t nind = indent_of(s, lines[j].a, lines[j].b);
                        int k2, n2;

                        while (nls > 0 && nind < ls[nls - 1].content &&
                                !(list_marker(s, skip_ws(s, lines[j].a, lines[j].b), lines[j].b, &k2, &n2) &&
                                  nind >= (nls > 1 ? ls[nls - 2].content : 0))) {
                            nls--;
                        }

                        seen = nls < seen ? nls + (j < nl && list_marker(s, skip_ws(s, lines[j].a, lines[j].b),
                                                   lines[j].b, &k2, &n2) ? 1 : 0) : seen;
                    } else if (j >= nl) {
                        nls = seen = 0;
                    }

                    continue;
                }
            }
        }

        ind = indent_of(s, a, e);
        c = skip_ws(s, a, e);

        /* fenced code */
        if (fence_start(s, a, e, &fc, &flen)) {
            int32_t j;
            pd_buf code;

            memset(&code, 0, sizeof(code));

            for (j = i + 1; j < nl; j++) {
                size_t ca = lines[j].a, ce = lines[j].b, k = skip_ws(s, ca, ce), r = 0;

                while (k + r < ce && s[k + r] == fc) {
                    r++;
                }

                if (r >= flen && skip_ws(s, k + r, ce) == ce && k - ca <= 3) {
                    break;
                }

                if (code.n || j > i + 1) {
                    pb_putc(&code, '\n');
                }

                {
                    /* the fence's indentation is removed from content lines */
                    size_t strip = 0;

                    while (strip < ind && ca + strip < ce && s[ca + strip] == ' ') {
                        strip++;
                    }

                    pb_put(&code, s + ca + strip, ce - ca - strip);
                }
            }

            bld_para_style(&b, "Code", PD_ROLE_CODE, 0);
            bld_begin_para(&b);
            bld_text(&b, code.p ? code.p : "", code.n);
            bld_end_para(&b);
            pb_free(&code);
            i = j;
            nls = seen = 0;
            continue;
        }

        /* $$display math$$ on lines of its own */
        if (c + 1 < e && s[c] == '$' && s[c + 1] == '$' && !para.n) {
            pd_buf tex;
            int32_t j = i;
            size_t from = c + 2;
            pd_inline o;

            memset(&tex, 0, sizeof(tex));

            for (;;) {
                size_t le = lines[j].b, k;
                int closed = 0;

                for (k = from; k + 1 < le; k++) {
                    if (s[k] == '$' && s[k + 1] == '$') {
                        closed = 1;
                        break;
                    }
                }

                pb_put(&tex, s + from, (closed ? k : le) - from);

                if (closed || j + 1 >= nl) {
                    break;
                }

                pb_putc(&tex, ' ');
                from = lines[++j].a;
            }

            memset(&o, 0, sizeof(o));
            o.kind = PD_INLINE_EQUATION;
            o.source = tex.p ? tex.p : "";
            o.source_len = (int32_t)tex.n;
            o.width = pd_conv_equation_width(tex.n);
            o.height = PD_PT(10);
            bld_para_style(&b, NULL, PD_ROLE_EQUATION, 0);
            bld_begin_para(&b);

            if (tex.n) {
                bld_inline(&b, &o);
            }

            bld_end_para(&b);
            pb_free(&tex);
            i = j;
            nls = seen = 0;
            continue;
        }

        /* ATX heading */
        if (c < e && s[c] == '#' && ind < 4) {
            size_t k = c;

            while (k < e && s[k] == '#') {
                k++;
            }

            if (k - c <= 6 && (k == e || s[k] == ' ' || s[k] == '\t')) {
                size_t te = e;
                char st[16];
                int lvl = (int)(k - c);

                while (te > k && (s[te - 1] == ' ' || s[te - 1] == '#')) {  /* closing #s */
                    te--;
                }

                snprintf(st, sizeof(st), "Heading %d", lvl);
                bld_para_style(&b, st, PD_ROLE_HEADING, lvl);
                bld_begin_para(&b);
                te = heading_id(&b, s, skip_ws(s, k, te), te);
                md_inline(&m, skip_ws(s, k, te), te);
                bld_end_para(&b);
                nls = seen = 0;
                continue;
            }
        }

        if (thematic(s, a, e) && !para.n) {
            nls = seen = 0;
            continue;
        }

        /* raw HTML block: a page break, or text */
        if (c < e && s[c] == '<' && !para.n && contains(s + c, e - c, "break-after:page")) {
            bld_break(&b, PD_BREAK_PAGE);
            continue;
        }

        /* GFM table */
        if (!para.n && c < e && memchr(s + c, '|', e - c) && i + 1 < nl && table_delim(s, lines[i + 1].a, lines[i + 1].b)) {
            int32_t j = i, na, q0;
            size_t ca[64], cb[64];
            int align[64];

            /* the delimiter row's colons: :-- left, :-: centre, --: right */
            na = table_cells(s, lines[i + 1].a, lines[i + 1].b, ca, cb, 64);

            for (q0 = 0; q0 < na; q0++) {
                int l = cb[q0] > ca[q0] && s[ca[q0]] == ':', r = cb[q0] > ca[q0] && s[cb[q0] - 1] == ':';

                align[q0] = l && r ? PD_ALIGN_CENTER : r ? PD_ALIGN_RIGHT : l ? PD_ALIGN_LEFT : -1;
            }

            bld_table_begin(&b);

            for (j = i; j < nl && !blank_line(s, lines[j].a, lines[j].b) && memchr(s + lines[j].a, '|',
                    lines[j].b - lines[j].a); j++) {
                int32_t nc, q;

                if (j == i + 1) {
                    continue;   /* the delimiter row */
                }

                nc = table_cells(s, lines[j].a, lines[j].b, ca, cb, 64);
                bld_row_begin(&b, j == i);

                for (q = 0; q < nc; q++) {
                    size_t r, w = 0;
                    char* cell = (char*)malloc(cb[q] - ca[q] + 1);
                    mctx pm = m;

                    if (!cell) {
                        continue;
                    }

                    for (r = ca[q]; r < cb[q]; r++) {   /* \| is a literal bar */
                        if (s[r] == '\\' && r + 1 < cb[q] && s[r + 1] == '|') {
                            continue;
                        }

                        cell[w++] = s[r];
                    }

                    cell[w] = '\0';
                    pm.s = cell;
                    bld_cell_begin(&b, 1, 0);

                    if (w) {
                        if (q < na && align[q] >= 0) {
                            b.pp.mask |= PD_PP_ALIGN;
                            b.pp.align = align[q];
                        }

                        bld_begin_para(&b);
                        md_inline(&pm, 0, w);
                        bld_end_para(&b);
                    }

                    bld_cell_end(&b);
                    free(cell);
                }
            }

            bld_table_end(&b);
            i = j - 1;
            nls = seen = 0;
            continue;
        }

        /* indented code (not in a list, not continuing a paragraph) */
        if (ind >= 4 && !para.n && nls == 0) {
            int32_t j;
            pd_buf code;

            memset(&code, 0, sizeof(code));

            for (j = i; j < nl && (indent_of(s, lines[j].a, lines[j].b) >= 4 || (blank_line(s, lines[j].a, lines[j].b) &&
                                   j + 1 < nl && indent_of(s, lines[j + 1].a, lines[j + 1].b) >= 4)); j++) {
                size_t ca = lines[j].a, ce = lines[j].b, strip = 0;

                while (strip < 4 && ca + strip < ce && s[ca + strip] == ' ') {
                    strip++;
                }

                if (j > i) {
                    pb_putc(&code, '\n');
                }

                pb_put(&code, s + ca + strip, ce - ca - strip);
            }

            bld_para_style(&b, "Code", PD_ROLE_CODE, 0);
            bld_begin_para(&b);
            bld_text(&b, code.p ? code.p : "", code.n);
            bld_end_para(&b);
            pb_free(&code);
            i = j - 1;
            continue;
        }

        /* list item */
        if ((ml = list_marker_c(s, c, e, &kind, &number, &mark)) != 0 && (nls > 0 || !para.n)) {
            size_t col = ind;
            int32_t k;

            while (nls > 0 && col < ls[nls - 1].content) {
                nls--;
            }

            if (nls < 10) {
                /* the next item of the list at this level, or the first of a new one */
                if (!(nls < seen && last[nls].id && last[nls].kind == kind && lmark[nls] == mark)) {
                    last[nls].id = md_list(&b, kind, nls, kind == 2 ? number : 1);
                    last[nls].kind = kind;
                    lmark[nls] = mark;
                }

                ls[nls].content = col + ml;
                ls[nls].kind = kind;
                ls[nls].id = last[nls].id;
                nls++;
                seen = nls;

                for (k = nls; k < 10; k++) {    /* a list under this item is a new one */
                    last[k].id = 0;
                }
            }

            pb_put(&para, s + c + ml, e - c - ml);
            continue;
        }

        /* block quote */
        if (c < e && s[c] == '>') {
            size_t k = c + 1;

            if (k < e && s[k] == ' ') {
                k++;
            }

            if (para.n) {
                pb_putc(&para, '\n');
            }

            para_kind = 1;
            pb_put(&para, s + k, e - k);
            continue;
        }

        /* paragraph text (or a continuation) */
        if (para.n) {
            pb_putc(&para, '\n');
        } else if (nls > 0 && ind < ls[nls - 1].content) {
            nls = seen = 0;     /* a paragraph not indented into the list ends it */
        }

        pb_put(&para, s + c, e - c);
    }

    if (para.n) {
        mctx pm = m;
        char* text = (char*)malloc(para.n + 1);

        if (para_kind == 1) {
            bld_para_style(&b, "Quote", PD_ROLE_QUOTE, 0);
        }

        if (nls > 0) {
            bld_list(&b, ls[nls - 1].kind, nls - 1);
            b.list_id = ls[nls - 1].id;
        }

        if (text) {
            memcpy(text, para.p, para.n);
            text[para.n] = '\0';
            pm.s = text;
            bld_begin_para(&b);
            md_inline(&pm, 0, para.n);
            bld_end_para(&b);
            free(text);
        }
    }

    pb_free(&para);
    free(lines);
    free(skip);
    free(notes.v);

    for (i = 0; i < refs.n; i++) {
        free(refs.v[i].url);
    }

    free(refs.v);
    return bld_finish(&b);
}
