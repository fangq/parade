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
    char link[2048];            /* destination of the open link, with its title */
    size_t link_at;             /* where its text starts in the output */
    char link_raw[2048];        /* its address as it is, for <url> */
    int link_url;               /* link is only the address (no title): it may be written <url> */
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
    int code_multi;             /* the open fence holds a block of several lines */
    char code_lang[32];
    char prefix[64];            /* "> " in quotes */
    int prev_q;                 /* the quote depth of the last block */
    int prev_loose;             /* the last list item was in a loose list */
    int prev_term;              /* the last block was a definition list's term */
    char div[32];               /* the ::: fenced div open */
    char alert[32];             /* the alert of the quote being written */
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

/* a link destination: spaces and parentheses encoded */
static void md_dest(pd_buf* o, const char* s, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == ' ' || s[i] == '(' || s[i] == ')') {
            pb_printf(o, "%%%02X", (unsigned char)s[i]);
        } else {
            pb_putc(o, s[i]);
        }
    }
}

/* an image's alt text, which is Markdown itself: as written, its brackets kept from closing it */
static void md_alt(pd_buf* o, const char* t, int32_t n) {
    int32_t i;

    for (i = 0; t && i < n; i++) {
        if ((t[i] == '[' || t[i] == ']') && (i == 0 || t[i - 1] != '\\')) {
            pb_putc(o, '\\');
        }

        pb_putc(o, t[i] == '\n' ? ' ' : t[i]);
    }
}

/* a link or image title: ' "title"', its quotes escaped */
static void md_title(pd_buf* o, const char* t, int32_t n) {
    int32_t i;

    if (!t || n <= 0) {
        return;
    }

    pb_puts(o, " \"");

    for (i = 0; i < n; i++) {
        if (t[i] == '"' || t[i] == '\\') {
            pb_putc(o, '\\');
        }

        pb_putc(o, t[i] == '\n' ? ' ' : t[i]);
    }

    pb_putc(o, '"');
}

/* the end of a link: ](url), or the whole link as <url> when its text is the address */
static void mx_end_link(mx* x) {
    pd_buf* o = x->o;
    size_t tl = o->n - x->link_at, ul = strlen(x->link), rl = strlen(x->link_raw), k, w = 0;
    char txt[2048];

    /* the text as written, its escapes taken off, to tell a link that is its own address */
    for (k = 0; k < tl && w + 1 < sizeof(txt); k++) {
        if (o->p[x->link_at + k] == '\\' && k + 1 < tl) {
            k++;
        }

        txt[w++] = o->p[x->link_at + k];
    }

    txt[w] = '\0';

    if (x->link_url && w == rl && memcmp(txt, x->link_raw, rl) == 0 && strchr(x->link_raw, ':') &&
            !strpbrk(x->link_raw, "<> ")) {
        o->n = x->link_at - 1;
        pb_putc(o, '<');
        pb_puts(o, x->link_raw);
        pb_putc(o, '>');
    } else if (x->link_url && ul == tl + 7 && strncmp(x->link, "http://www.", 11) == 0 &&
               memcmp(o->p + x->link_at, x->link + 7, tl) == 0) {    /* www.example.com, as it was written */
        memmove(o->p + x->link_at - 1, o->p + x->link_at, tl);
        o->n--;
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
                int sized = ob->width > 0 && ob->height > 0;

                fmt_to(x, 0);

                if (ob->source_len > 0) {   /* by address, as it was written */
                    pb_puts(o, "![");
                    md_alt(o, ob->alt, ob->alt_len);
                    pb_puts(o, "](");
                    md_dest(o, ob->source, (size_t)ob->source_len);
                    md_title(o, ob->title, ob->title_len);
                    pb_putc(o, ')');
                    sized = sized && ob->level == 1;
                } else if (pd_doc_resource(x->d, ob->resource, &mime, &data, &len) == PD_OK) {
                    pb_puts(o, "![");
                    md_alt(o, ob->alt, ob->alt_len);
                    pb_printf(o, "](data:%s;base64,", mime);
                    pb_base64(o, (const unsigned char*)data, len);
                    md_title(o, ob->title, ob->title_len);
                    pb_putc(o, ')');
                } else {
                    sized = 0;
                }

                if (sized) {
                    pb_printf(o, "{width=%gpt height=%gpt}", ob->width / 65536.0, ob->height / 65536.0);
                }

                break;
            }

            case PD_INLINE_RAW:     /* as it came */
                fmt_to(x, 0);

                if (ob->source) {
                    pb_put(o, ob->source, (size_t)ob->source_len);
                }

                break;

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
                    pd_buf dst;

                    memset(&dst, 0, sizeof(dst));
                    md_dest(&dst, ob->source, (size_t)ob->source_len);
                    md_title(&dst, ob->title, ob->title_len);
                    snprintf(x->link, sizeof(x->link), "%.*s", (int)dst.n, dst.p ? dst.p : "");
                    snprintf(x->link_raw, sizeof(x->link_raw), "%.*s", (int)ob->source_len, ob->source);
                    x->link_url = ob->title_len ? 0 : 1;
                    pb_free(&dst);
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

        if (f & MF_CODE) {     /* spaces at its ends go outside the backticks */
            while (a < e && sp->text[a] == ' ') {
                a++;
            }

            while (e > a && sp->text[e - 1] == ' ') {
                e--;
            }

            fmt_to(x, a > 0 || e == a ? 0 : f & ~MF_CODE);
            pb_put(o, sp->text, a);

            if (e > a) {
                fmt_to(x, f & ~MF_CODE);
                code_span(o, sp->text + a, e - a);
            }

            pb_put(o, sp->text + e, sp->len - e);
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

/* "> " for each quote a block is in */
static void quote_prefix(char* out, size_t cap, int q) {
    int i;

    out[0] = '\0';

    for (i = 0; i < q && (size_t)(2 * i + 3) <= cap; i++) {
        strcat(out, "> ");
    }
}

static void mx_close_code(mx* x) {
    if (x->code_open) {
        pb_putc(x->o, '\n');
        pb_puts(x->o, x->prefix);
        pb_puts(x->o, "```\n");
        x->code_open = 0;
        x->prev_block = 2;
    }
}

/* the blank line between two blocks: inside the quotes both are in, it is one of the quote's own lines */
static void blank(mx* x, int q) {
    mx_close_code(x);

    if (x->prev_block) {
        char pre[64];
        size_t n;

        quote_prefix(pre, sizeof(pre), q < x->prev_q ? q : x->prev_q);
        n = strlen(pre);

        while (n > 0 && pre[n - 1] == ' ') {
            pre[--n] = '\0';
        }

        pb_puts(x->o, pre);
        pb_putc(x->o, '\n');
    }
}

static void mx_block(mx* x, pd_block_id id);

/* the ::: fenced div a block is in: the open one closed when it is not, the block's opened */
static void mx_div(mx* x, const char* div) {
    if (div[0] == '!') {    /* an alert is a quote's, not a div */
        div = "";
    }

    if (strcmp(div, x->div) == 0) {
        return;
    }

    if (x->div[0]) {
        blank(x, 0);
        pb_puts(x->o, ":::\n");
        x->prev_block = 2;
        x->ldepth = 0;
    }

    if (div[0]) {
        blank(x, 0);
        pb_printf(x->o, "::: %s\n", div);
        x->prev_block = 2;
        x->ldepth = 0;
    }

    x->prev_q = 0;
    snprintf(x->div, sizeof(x->div), "%s", div);
}

/* text that goes out as it is, line by line behind the quote markers */
static void mx_lines(mx* x, const char* t, uint32_t n) {
    uint32_t k;

    pb_puts(x->o, x->prefix);

    for (k = 0; k < n; k++) {
        pb_putc(x->o, t[k]);

        if (t[k] == '\n') {
            pb_puts(x->o, x->prefix);
        }
    }
}

static void mx_para(mx* x, pd_block_id p) {
    pd_block_info bi;
    pd_para_attrs at;
    int32_t level = 0, i;
    int kind = pd_conv_list_kind(x->d, p, &level), q, ind = 0;

    pd_doc_block_info(x->d, p, &bi);
    memset(&at, 0, sizeof(at));
    pd_doc_para_attrs(x->d, p, &at);
    q = at.quote_depth > 0 ? at.quote_depth : bi.role == PD_ROLE_QUOTE ? 1 : 0;

    mx_div(x, at.div_class);

    if (pd_conv_item_level(x->d, p, &level)) {     /* a later block of an item: under its text */
        ind = level < x->ldepth && q == x->prev_q ? x->lw[level] : 0;
    }

    /* a quote of another kind is another quote: a plain blank line between */
    if (q > 0 && x->prev_q > 0 && strcmp(x->alert, at.div_class[0] == '!' ? at.div_class : "") != 0) {
        mx_close_code(x);
        x->prev_q = 0;
        x->ldepth = 0;
    }

    /* GitHub's alert: its quote opens with [!NOTE] */
    if (at.div_class[0] == '!' && q > 0 && (strcmp(x->alert, at.div_class) != 0 || q != x->prev_q)) {
        size_t k;

        mx_close_code(x);
        blank(x, q);
        quote_prefix(x->prefix, sizeof(x->prefix), q);
        pb_puts(x->o, x->prefix);
        pb_puts(x->o, "[!");

        for (k = 1; at.div_class[k]; k++) {
            pb_putc(x->o, (char)toupper((unsigned char)at.div_class[k]));
        }

        pb_puts(x->o, "]\n");
        x->prev_block = 0;      /* the quote's text follows on the next line */
        x->prev_q = q;
        x->ldepth = 0;
    }

    snprintf(x->alert, sizeof(x->alert), "%s", at.div_class[0] == '!' ? at.div_class : "");

    if (bi.role == PD_ROLE_CODE) {
        const char* t;
        uint32_t n;
        int multi;

        pd_doc_para_text(x->d, p, &t, &n);
        multi = memchr(t, '\n', n) != NULL;

        /* one fence per code block; code that came a line to a paragraph (HTML, DOCX) is run together */
        if (x->code_open && (multi || x->code_multi || strcmp(x->code_lang, at.lang) != 0 || q != x->prev_q)) {
            mx_close_code(x);
        }

        if (!x->code_open) {
            blank(x, q);
            quote_prefix(x->prefix, sizeof(x->prefix), q);

            if (ind > 0 && strlen(x->prefix) + (size_t)ind < sizeof(x->prefix)) {   /* in a list item */
                memset(x->prefix + strlen(x->prefix), ' ', (size_t)ind);
                x->prefix[strlen(x->prefix) + (size_t)ind] = '\0';
            }

            pb_puts(x->o, x->prefix);
            pb_puts(x->o, "```");
            pb_puts(x->o, at.lang);
            pb_putc(x->o, '\n');
            x->code_open = 1;
            snprintf(x->code_lang, sizeof(x->code_lang), "%s", at.lang);
        } else {
            pb_putc(x->o, '\n');
        }

        mx_lines(x, t, n);
        x->code_multi = multi;
        x->ldepth = ind ? x->ldepth : 0;
        x->prev_q = q;
        x->prev_term = 0;
        return;
    }

    mx_close_code(x);
    quote_prefix(x->prefix, sizeof(x->prefix), q);

    if (kind) {
        char marker[16];
        pd_list_level lv[9];
        int32_t nlv = 0, start = 1;

        if (q != x->prev_q) {   /* a list in other quotes is another list */
            x->ldepth = 0;
        }

        if (x->prev_block == 2 || (x->prev_block == 1 && (at.loose || x->prev_loose)) || q != x->prev_q) {
            blank(x, q);    /* a loose list's items stand apart */
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

        pb_puts(x->o, x->prefix);

        for (i = 0; i < level; i++) {
            pb_printf(x->o, "%*s", i == 0 ? x->lw[0] : x->lw[i] - x->lw[i - 1], "");
        }

        pb_puts(x->o, marker);

        if (at.task) {
            pb_puts(x->o, at.task == 2 ? "[x] " : "[ ] ");
        }

        if (bi.role == PD_ROLE_HEADING) {   /* - ## a heading as an item */
            for (i = 0; i < bi.level; i++) {
                pb_putc(x->o, '#');
            }

            pb_putc(x->o, ' ');
        }

        x->lw[level] = (level ? x->lw[level - 1] : 0) + (int)strlen(marker);
        x->ldepth = level + 1;
        mx_inline(x, p);
        pb_putc(x->o, '\n');
        x->prefix[0] = '\0';
        x->prev_block = 1;
        x->prev_loose = at.loose;
        x->prev_q = q;
        x->prev_term = 0;
        return;
    }

    if (!ind) {
        x->ldepth = 0;
    }

    if (!(bi.role == PD_ROLE_DEFINITION && x->prev_term)) {     /* a definition sits right under its term */
        blank(x, q);
    }

    if (bi.role == PD_ROLE_RAW) {   /* markup, as it came */
        const char* t;
        uint32_t n;

        pd_doc_para_text(x->d, p, &t, &n);

        if (ind > 0 && strlen(x->prefix) + (size_t)ind < sizeof(x->prefix)) {
            memset(x->prefix + strlen(x->prefix), ' ', (size_t)ind);
            x->prefix[strlen(x->prefix) + (size_t)ind] = '\0';
        }

        mx_lines(x, t, n);
        pb_putc(x->o, '\n');
        x->prefix[0] = '\0';
        x->prev_block = 2;
        x->prev_q = q;
        x->prev_term = 0;
        return;
    }

    pb_puts(x->o, x->prefix);

    if (ind > 0) {
        pb_printf(x->o, "%*s", ind, "");
    }

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

        case PD_ROLE_DEFINITION:
            pb_puts(x->o, ": ");
            break;

        case PD_ROLE_EQUATION: {
            pd_inline o;
            const char* t;
            uint32_t n, k;

            pd_doc_para_text(x->d, p, &t, &n);

            for (k = 0; k + 2 < n; k++) {
                pd_pos at0;

                at0.block = p;
                at0.offset = k;

                if ((unsigned char)t[k] == 0xEF && pd_doc_inline_at(x->d, at0, &o) == PD_OK &&
                        o.kind == PD_INLINE_EQUATION && o.source) {
                    pb_puts(x->o, "$$");
                    pb_put(x->o, o.source, (size_t)o.source_len);
                    pb_puts(x->o, "$$\n");
                    x->prefix[0] = '\0';
                    x->prev_block = 2;
                    x->prev_q = q;
                    x->prev_term = 0;
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
    x->prev_q = q;
    x->prev_term = bi.role == PD_ROLE_TERM;
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

    blank(x, 0);
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
            mx_div(x, "");
            x->ldepth = 0;
            mx_table(x, id);
            x->prev_q = 0;
            x->prev_term = 0;
            break;

        case PD_BLOCK_BREAK:
            mx_div(x, "");
            blank(x, 0);
            x->ldepth = 0;
            pb_puts(x->o, bi.break_kind == PD_BREAK_RULE ? "***\n" : "<div style=\"break-after:page\"></div>\n");
            x->prev_block = 2;
            x->prev_q = 0;
            x->prev_term = 0;
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

    {   /* front matter */
        size_t ml;
        const char* meta = pd_doc_metadata(d, &ml);

        if (ml) {
            pb_puts(o, "---\n");
            pb_put(o, meta, ml);
            pb_puts(o, meta[ml - 1] == '\n' ? "---\n" : "\n---\n");
            x->prev_block = 2;
        }
    }

    mx_block(x, pd_doc_root(d));
    mx_close_code(x);
    mx_div(x, "");

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
    N_TEXT = 0, N_DELIM, N_CODE, N_BREAK, N_LINK_OPEN, N_LINK_CLOSE, N_IMAGE, N_NOTE, N_MATH, N_TAG, N_RAW
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
    const char* title;          /* links and images: the title, NULL if none */
    size_t tlen;
    size_t alt_a, alt_b;        /* images: the bracketed text */
    int sized;                  /* images: {width= height=} given */
} mnode;

/* a link reference definition: [label]: destination "title" */
typedef struct {
    char label[128];            /* normalized: case folded, inner whitespace collapsed */
    char* url;
    size_t ulen;
    char* title;                /* NULL if none */
    size_t tlen;
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
static size_t link_dest_t(const char* s, size_t i, size_t n, size_t* ua, size_t* ub, size_t* ta, size_t* tb);

static size_t link_dest(const char* s, size_t i, size_t n, size_t* ua, size_t* ub) {
    size_t ta, tb;

    return link_dest_t(s, i, n, ua, ub, &ta, &tb);
}

/* ... and the title's range, ta == tb when there is none */
static size_t link_dest_t(const char* s, size_t i, size_t n, size_t* ua, size_t* ub, size_t* ta, size_t* tb) {
    int depth = 0;

    *ta = *tb = 0;

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

    if (i < n && (s[i] == '"' || s[i] == '\'' || s[i] == '(')) {     /* a title */
        char q = s[i] == '(' ? ')' : s[i];

        *ta = ++i;

        while (i < n && s[i] != q) {
            i += s[i] == '\\' && i + 1 < n ? 2 : 1;
        }

        *tb = i < n ? i : *ta;
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
/* GFM's extended autolink at i -- www.host, http://, https:// -- its end, 0 if none */
static size_t bare_link(const char* s, size_t a, size_t i, size_t b) {
    size_t k, dots = 0, e;
    int paren = 0;

    if (i > a && !(s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\n' || s[i - 1] == '(' || s[i - 1] == '*' ||
                   s[i - 1] == '_' || s[i - 1] == '~')) {
        return 0;
    }

    if (b - i > 4 && strncmp(s + i, "www.", 4) == 0) {
        k = i + 4;
    } else if (b - i > 7 && strncmp(s + i, "http://", 7) == 0) {
        k = i + 7;
    } else if (b - i > 8 && strncmp(s + i, "https://", 8) == 0) {
        k = i + 8;
    } else {
        return 0;
    }

    e = k;

    while (e < b && (isalnum((unsigned char)s[e]) || s[e] == '-' || s[e] == '_' || s[e] == '.')) {   /* the host */
        dots += s[e] == '.';
        e++;
    }

    if (e == k) {   /* a host is a name (www. supplies the period GFM wants) */
        return 0;
    }

    (void)dots;

    while (e < b && !isspace((unsigned char)s[e]) && s[e] != '<') {     /* the rest */
        paren += s[e] == '(' ? 1 : s[e] == ')' ? -1 : 0;
        e++;
    }

    /* trailing punctuation is the sentence's, and a ')' without its '(' */
    while (e > k && (strchr("?!.,:*_~'\"", s[e - 1]) || (s[e - 1] == ')' && paren < 0))) {
        paren += s[e - 1] == ')' ? 1 : 0;
        e--;
    }

    return e > k ? e : 0;
}

static void tokenize_l(const char* s, size_t a, size_t b, mnode** v, int32_t* nv, int32_t* cap, const mrefs* refs,
                       int in_link);

static void tokenize(const char* s, size_t a, size_t b, mnode** v, int32_t* nv, int32_t* cap, const mrefs* refs) {
    tokenize_l(s, a, b, v, nv, cap, refs, 0);
}

static void tokenize_l(const char* s, size_t a, size_t b, mnode** v, int32_t* nv, int32_t* cap, const mrefs* refs,
                       int in_link) {
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

        if (!in_link && (c == 'w' || c == 'h')) {    /* www.example.com, https://example.com */
            size_t e = bare_link(s, a, i, b);

            if (e) {
                FLUSH();
                x.type = N_LINK_OPEN;
                x.a = i;
                x.b = e;
                x.ch = s[i] == 'w' ? 'w' : 0;   /* www.: http:// in front */
                push_node(v, nv, cap, &x);
                memset(&x, 0, sizeof(x));
                x.type = N_TEXT;
                x.a = i;
                x.b = e;
                x.ch = 1;
                push_node(v, nv, cap, &x);
                memset(&x, 0, sizeof(x));
                x.type = N_LINK_CLOSE;
                push_node(v, nv, cap, &x);
                i = e;
                t = i;
                continue;
            }
        }

        if (c == '^' && i + 1 < b && s[i + 1] == '[') {     /* pandoc's inline note: ^[text] */
            size_t rb = close_bracket(s, i + 1, b);

            if (rb > i + 2) {
                FLUSH();
                x.type = N_NOTE;
                x.ch = 'i';
                x.a = i + 2;
                x.b = rb;
                push_node(v, nv, cap, &x);
                i = rb + 1;
                t = i;
                continue;
            }
        }

        if (c == '^' && i + 1 < b && s[i + 1] != ' ' && s[i + 1] != '^') {     /* pandoc's ^superscript^ */
            size_t k = i + 1;

            while (k < b && s[k] != '^' && s[k] != ' ' && s[k] != '\n' && s[k] != '\t') {
                k += s[k] == '\\' && k + 1 < b ? 2 : 1;
            }

            if (k < b && s[k] == '^' && k > i + 1) {
                FLUSH();
                x.type = N_TAG;
                x.tag = MF_SUP;
                push_node(v, nv, cap, &x);
                tokenize_l(s, i + 1, k, v, nv, cap, refs, in_link);
                memset(&x, 0, sizeof(x));
                x.type = N_TAG;
                x.tag = -MF_SUP;
                push_node(v, nv, cap, &x);
                i = k + 1;
                t = i;
                continue;
            }
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
            size_t rb = close_bracket(s, i + 1, b), ua = 0, ub = 0, e, ta = 0, tb = 0;

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
                    x.title = r->title;
                    x.tlen = r->tlen;
                    x.alt_a = i + 2;
                    x.alt_b = rb;
                    push_node(v, nv, cap, &x);
                    i = e;
                    t = i;
                    continue;
                }
            }

            if (rb && (e = link_dest_t(s, rb + 1, b, &ua, &ub, &ta, &tb)) != 0) {
                FLUSH();
                x.type = N_IMAGE;
                x.a = ua;
                x.b = ub;
                x.alt_a = i + 2;
                x.alt_b = rb;

                if (tb > ta) {
                    x.title = s + ta;
                    x.tlen = tb - ta;
                }

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

                        x.sized = 1;

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
            size_t rb = close_bracket(s, i, b), ua = 0, ub = 0, e, ta = 0, tb = 0;

            if (rb && (e = link_dest_t(s, rb + 1, b, &ua, &ub, &ta, &tb)) != 0) {
                FLUSH();
                x.type = N_LINK_OPEN;
                x.a = ua;
                x.b = ub;

                if (tb > ta) {
                    x.title = s + ta;
                    x.tlen = tb - ta;
                }
                push_node(v, nv, cap, &x);
                tokenize_l(s, i + 1, rb, v, nv, cap, refs, 1);
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
                    x.title = r->title;
                    x.tlen = r->tlen;
                    push_node(v, nv, cap, &x);
                    tokenize_l(s, i + 1, rb, v, nv, cap, refs, 1);
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

            if (i + 3 < b && memcmp(s + i, "<!--", 4) == 0) {   /* a comment: raw */
                size_t z = i + 4;

                while (z + 2 < b && memcmp(s + z, "-->", 3) != 0) {
                    z++;
                }

                if (z + 2 < b) {
                    FLUSH();
                    x.type = N_RAW;
                    x.a = i;
                    x.b = z + 3;
                    push_node(v, nv, cap, &x);
                    i = z + 3;
                    t = i;
                    continue;
                }
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
                        } else {    /* any other tag goes through as it is */
                            x.type = N_RAW;
                            x.a = i;
                            x.b = z + 1;
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
static void emphasis(mnode* v, int32_t n, const char* s, size_t a, size_t b) {
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

                if (cl->ch == '~' && op->orig != cl->orig) {
                    continue;   /* ~ with ~, ~~ with ~~ */
                }

                {
                    int use = cl->ch == '~' ? (int)op->orig : (op->count >= 2 && cl->count >= 2 ? 2 : 1);
                    int f = cl->ch == '~' ? MF_STRIKE : use == 2 ? MF_BOLD : MF_ITAL;

                    /* one ~ inside a word, as in H~2~O, is pandoc's subscript; elsewhere GFM's strike */
                    if (cl->ch == '~' && use == 1 && ((op->a > a && isalnum((unsigned char)s[op->a - 1])) ||
                                                      (cl->b < b && isalnum((unsigned char)s[cl->b])))) {
                        f = MF_SUB;
                    }

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

/* an image's alt text and title, then the image */
static void image_text(mctx* m, const mnode* x, pd_inline* o) {
    pd_buf alt, title;

    memset(&alt, 0, sizeof(alt));
    memset(&title, 0, sizeof(title));

    if (x->alt_b > x->alt_a) {
        mu_decode(m->s + x->alt_a, x->alt_b - x->alt_a, &alt);
        o->alt = alt.p;
        o->alt_len = (int32_t)alt.n;
    }

    if (x->title && x->tlen) {
        mu_decode(x->title, x->tlen, &title);
        o->title = title.p;
        o->title_len = (int32_t)title.n;
    }

    o->level = x->sized;    /* the document gave its size */
    bld_inline(m->b, o);
    pb_free(&alt);
    pb_free(&title);
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
            o.width = x->w;
            o.height = x->h;

            if (nb && pd_doc_add_resource(m->b->d, mime, bytes, nb, &o.resource) == PD_OK) {
                image_text(m, x, &o);
            }

            free(bytes);
        }
    } else if (un > 0) {    /* by address: loaded later, if at all (pd_doc_load_images) */
        pd_inline o;
        pd_buf url;

        memset(&o, 0, sizeof(o));
        memset(&url, 0, sizeof(url));
        mu_decode(u, un, &url);
        o.kind = PD_INLINE_IMAGE;
        o.source = url.p ? url.p : "";
        o.source_len = (int32_t)url.n;
        o.width = x->w;
        o.height = x->h;
        image_text(m, x, &o);
        pb_free(&url);
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
    emphasis(v, n, m->s, a, b);

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

            case N_RAW: {
                pd_inline o;

                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_RAW;
                o.source = m->s + x->a;
                o.source_len = (int32_t)(x->b - x->a);
                bld_inline(m->b, &o);
                break;
            }

            case N_LINK_OPEN: {
                pd_inline o;
                pd_buf url, title;
                size_t r, w = 0;

                memset(&url, 0, sizeof(url));

                if (x->ch == 'w') {
                    pb_puts(&url, "http://");
                }

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
                memset(&title, 0, sizeof(title));

                if (x->title && x->tlen) {
                    mu_decode(x->title, x->tlen, &title);
                    o.title = title.p;
                    o.title_len = (int32_t)title.n;
                }

                if (url.n) {
                    bld_inline(m->b, &o);
                }

                pb_free(&url);
                pb_free(&title);
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

                if (x->ch == 'i') {     /* an inline note: its text is right here */
                    pd_char_props keep = m->b->cp;
                    mctx nm = *m;

                    if (m->depth < 4) {
                        nm.depth = m->depth + 1;
                        bld_footnote_begin(m->b);
                        bld_begin_para(m->b);
                        md_inline(&nm, x->a, x->b);
                        bld_end_para(m->b);
                        bld_footnote_end(m->b);
                        m->b->cp = keep;
                    }

                    break;
                }

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
    size_t i = skip_ws(s, a, e), k, ua, ub, ta = 0, tb = 0;
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

        for (ta = ++k; k < e && s[k] != q1; k++) {
        }

        tb = k;

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

        if (tb > ta && (refs->v[refs->n].title = (char*)malloc(tb - ta + 1)) != NULL) {
            memcpy(refs->v[refs->n].title, s + ta, tb - ta);
            refs->v[refs->n].title[tb - ta] = '\0';
            refs->v[refs->n].tlen = tb - ta;
        }

        refs->n++;
    }

    return 1;
}

/* The list one list in the text is: Markdown numbers each list from its own first item and starts a
   new count in every list nested under an item */
static pd_list_id md_list(pd_bld* b, int kind, int32_t level, int start) {
    return bld_list_new(b, kind, level, start);
}

/* ---------------- the block parser ---------------- */

enum { LF_NONE = 0, LF_PARA, LF_FENCE, LF_INDENT, LF_HTML, LF_MATH, LF_TABLE };

/* an open container: a block quote, or a list item */
typedef struct {
    char type;                  /* 'q' or 'l' */
    size_t content;             /* list item: columns its content is indented, from the container's start */
    int kind;                   /* list item: 1 bullet, 2 ordered */
    char mark;
    pd_list_id list;
    int32_t level;              /* list item: its list's nesting level */
    int labelled;               /* the item's first paragraph, which carries its label, is made */
    int task;                   /* 0, 1 open, 2 checked */
    char alert[16];             /* a quote: GitHub's [!NOTE] and the like, as "!note" */
} mcont;

/* a list in the text, for its looseness */
typedef struct {
    pd_list_id id;
    int blank;                  /* a blank line inside it, not yet followed by more of it */
    int loose;
    pd_block_id* paras;
    int32_t n, cap;
} mlistinfo;

typedef struct {
    pd_bld* b;
    mctx* m;
    const char* s;
    mcont c[32];
    int32_t nc;
    mcont sib[32];              /* the item last closed at each depth, which a next item may continue */
    int sibok[32];
    mlistinfo* li;
    int32_t nli, capli;
    /* the open leaf block */
    int leaf;
    pd_buf text;
    int32_t nlines;
    char fc;                    /* fence */
    size_t flen, find;
    char lang[32];
    char html_end[16];          /* the text that ends the HTML block, "" = a blank line */
    int def;                    /* the paragraph is a definition */
    int32_t table_line;         /* LF_TABLE: index of the header line */
    char divs[8][32];           /* ::: fenced divs open, innermost last */
    int ndiv;
} mparse;

static mlistinfo* list_info(mparse* P, pd_list_id id) {
    int32_t i, cap = P->capli;

    for (i = 0; i < P->nli; i++) {
        if (P->li[i].id == id) {
            return &P->li[i];
        }
    }

    if (pd_grow((void**)&P->li, &cap, (int64_t)P->nli + 1, sizeof(mlistinfo))) {
        return NULL;
    }

    P->capli = cap;
    memset(&P->li[P->nli], 0, sizeof(mlistinfo));
    P->li[P->nli].id = id;
    return &P->li[P->nli++];
}

static int32_t quote_depth(const mparse* P) {
    int32_t i, q = 0;

    for (i = 0; i < P->nc; i++) {
        q += P->c[i].type == 'q';
    }

    return q;
}

/* more of every open list arrives: a blank line it had makes it loose */
static void list_more(mparse* P) {
    int32_t i;

    for (i = 0; i < P->nc; i++) {
        if (P->c[i].type == 'l') {
            mlistinfo* L = list_info(P, P->c[i].list);

            if (L && L->blank) {
                L->loose = 1;
                L->blank = 0;
            }
        }
    }
}

/* Begin a paragraph in the open containers: the first of a list item is
   the item (it carries the label), later ones are indented under it; a
   paragraph in a quote that has no role of its own is a QUOTE. */
static pd_block_id md_begin(mparse* P, const char* style, int32_t role, int32_t level, const char* lang) {
    pd_bld* b = P->b;
    mcont* item = NULL;
    int32_t i, q = quote_depth(P);
    pd_block_id id;
    pd_para_attrs at;

    for (i = P->nc - 1; i >= 0 && !item; i--) {
        if (P->c[i].type == 'l') {
            item = &P->c[i];
        }
    }

    if (q > 0 && role == PD_ROLE_BODY) {
        style = "Quote";
        role = PD_ROLE_QUOTE;
    }

    if (style || role) {
        bld_para_style(b, style, role, level);
    }

    if (item) {     /* the item, or a later block of it: in its list either way */
        bld_list(b, item->kind, item->level);
        b->list_id = item->list;
    }

    id = bld_begin_para(b);
    memset(&at, 0, sizeof(at));
    at.quote_depth = q;
    at.cont = item && item->labelled;

    if (item && !item->labelled) {
        mlistinfo* L = list_info(P, item->list);

        at.task = item->task;
        item->labelled = 1;

        if (L && !pd_grow((void**)&L->paras, &L->cap, (int64_t)L->n + 1, sizeof(pd_block_id))) {
            L->paras[L->n++] = id;
        }
    }

    if (lang) {
        snprintf(at.lang, sizeof(at.lang), "%s", lang);
    }

    for (i = P->nc - 1; i >= 0 && !at.div_class[0]; i--) {     /* an alert's quote, else a fenced div */
        if (P->c[i].type == 'q' && P->c[i].alert[0]) {
            snprintf(at.div_class, sizeof(at.div_class), "%s", P->c[i].alert);
        }
    }

    if (!at.div_class[0] && P->ndiv > 0) {
        snprintf(at.div_class, sizeof(at.div_class), "%s", P->divs[P->ndiv - 1]);
    }

    if (id && (at.quote_depth || at.task || at.lang[0] || at.cont || at.div_class[0])) {
        pd_doc_set_para_attrs(b->d, id, &at);
    }

    return id;
}

/* a paragraph of inline content */
static void md_para(mparse* P, const char* style, int32_t role, int32_t level, const char* t, size_t n, int head) {
    mctx pm = *P->m;
    char* text = (char*)malloc(n + 1);

    if (!text) {
        return;
    }

    memcpy(text, t, n);
    text[n] = '\0';
    pm.s = text;
    md_begin(P, style, role, level, NULL);

    if (head) {
        n = heading_id(P->b, text, 0, n);
    }

    md_inline(&pm, 0, n);
    bld_end_para(P->b);
    free(text);
}

/* a block of text taken as it is: code, raw HTML */
static void md_verbatim(mparse* P, const char* style, int32_t role, const char* lang) {
    md_begin(P, style, role, 0, lang);
    bld_text(P->b, P->text.p ? P->text.p : "", P->text.n);
    bld_end_para(P->b);
}

static void md_table(mparse* P, const char* lines, size_t n);

/* the open leaf block, made */
static void leaf_close(mparse* P) {
    pd_buf* t = &P->text;

    switch (P->leaf) {
        case LF_PARA:
            while (t->n && (t->p[t->n - 1] == ' ' || t->p[t->n - 1] == '\t')) {
                t->n--;
            }

            if (t->n) {
                md_para(P, P->def ? "Definition" : NULL, P->def ? PD_ROLE_DEFINITION : PD_ROLE_BODY, 0, t->p, t->n, 0);
            }

            break;

        case LF_FENCE:
            md_verbatim(P, "Code", PD_ROLE_CODE, P->lang[0] ? P->lang : NULL);
            break;

        case LF_INDENT:
            while (t->n && t->p[t->n - 1] == '\n') {    /* trailing blank lines are not the code's */
                t->n--;
            }

            md_verbatim(P, "Code", PD_ROLE_CODE, NULL);
            break;

        case LF_HTML:
            while (t->n && t->p[t->n - 1] == '\n') {
                t->n--;
            }

            if (contains(t->p ? t->p : "", t->n, "break-after:page")) {
                bld_break(P->b, PD_BREAK_PAGE);     /* how the writer marks a page break */
            } else {
                md_verbatim(P, "Code", PD_ROLE_RAW, NULL);
            }

            break;

        case LF_MATH: {
            pd_inline o;

            memset(&o, 0, sizeof(o));
            o.kind = PD_INLINE_EQUATION;
            o.source = t->p ? t->p : "";
            o.source_len = (int32_t)t->n;
            o.width = pd_conv_equation_width(t->n);
            o.height = PD_PT(10);
            md_begin(P, NULL, PD_ROLE_EQUATION, 0, NULL);

            if (t->n) {
                bld_inline(P->b, &o);
            }

            bld_end_para(P->b);
            break;
        }

        case LF_TABLE:
            md_table(P, t->p ? t->p : "", t->n);
            break;
    }

    P->leaf = LF_NONE;
    P->text.n = 0;
    P->nlines = 0;
    P->def = 0;
    P->lang[0] = '\0';
}

static void leaf_add(mparse* P, const char* s, size_t n) {
    if (P->nlines++ > 0) {
        pb_putc(&P->text, '\n');
    }

    pb_put(&P->text, s, n);
}

/* a GFM table from its lines, one per row, the delimiter row second */
static void md_table(mparse* P, const char* s, size_t n) {
    size_t ls[512], le[512], a = 0, ca[64], cb[64];
    int32_t nl = 0, j, na, q;
    int align[64];

    while (a <= n && nl < 512) {
        size_t e = a;

        while (e < n && s[e] != '\n') {
            e++;
        }

        ls[nl] = a;
        le[nl++] = e;
        a = e + 1;

        if (e >= n) {
            break;
        }
    }

    if (nl < 2) {
        return;
    }

    na = table_cells(s, ls[1], le[1], ca, cb, 64);

    for (q = 0; q < na; q++) {  /* the delimiter row's colons: :-- left, :-: centre, --: right */
        int l = cb[q] > ca[q] && s[ca[q]] == ':', r = cb[q] > ca[q] && s[cb[q] - 1] == ':';

        align[q] = l && r ? PD_ALIGN_CENTER : r ? PD_ALIGN_RIGHT : l ? PD_ALIGN_LEFT : -1;
    }

    bld_table_begin(P->b);

    for (j = 0; j < nl; j++) {
        int32_t nc2;

        if (j == 1) {
            continue;
        }

        nc2 = table_cells(s, ls[j], le[j], ca, cb, 64);
        bld_row_begin(P->b, j == 0);

        for (q = 0; q < nc2; q++) {
            size_t r, w = 0;
            char* cell = (char*)malloc(cb[q] - ca[q] + 1);
            mctx pm = *P->m;

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
            bld_cell_begin(P->b, 1, 0);

            if (w) {
                if (q < na && align[q] >= 0) {
                    P->b->pp.mask |= PD_PP_ALIGN;
                    P->b->pp.align = align[q];
                }

                bld_begin_para(P->b);
                md_inline(&pm, 0, w);
                bld_end_para(P->b);
            }

            bld_cell_end(P->b);
            free(cell);
        }
    }

    bld_table_end(P->b);
}

/* the column a byte offset is at, counting from from (tabs to multiples of 4) */
static size_t cols_to(const char* s, size_t from, size_t at) {
    size_t i, col = 0;

    for (i = from; i < at; i++) {
        col += s[i] == '\t' ? 4 - col % 4 : 1;
    }

    return col;
}

/* past n columns of white space */
static size_t skip_cols(const char* s, size_t a, size_t e, size_t n) {
    size_t col = 0;

    while (a < e && col < n && (s[a] == ' ' || s[a] == '\t')) {
        col += s[a] == '\t' ? 4 - col % 4 : 1;
        a++;
    }

    return a;
}

/* the start of a raw HTML block (CommonMark's kinds 1-6), and what ends it */
static int html_start(const char* s, size_t c, size_t e, char* end, size_t cap) {
    static const char* blocks[] = { "address", "article", "aside", "blockquote", "body", "details", "dialog", "dd",
                                    "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form", "h1",
                                    "h2", "h3", "h4", "h5", "h6", "header", "hr", "html", "li", "main", "nav", "ol",
                                    "p", "section", "summary", "table", "tbody", "td", "tfoot", "th", "thead", "tr",
                                    "ul", "center", "iframe", "noscript", "video", "audio", "canvas", NULL
                                  };
    static const char* raw[] = { "script", "pre", "style", "textarea", NULL };
    char name[16];
    size_t k = c + 1, nn = 0;
    int i;

    if (c >= e || s[c] != '<') {
        return 0;
    }

    if (e - c >= 4 && memcmp(s + c, "<!--", 4) == 0) {
        snprintf(end, cap, "-->");
        return 1;
    }

    if (e - c >= 2 && s[c + 1] == '?') {
        snprintf(end, cap, "?>");
        return 1;
    }

    if (e - c >= 3 && s[c + 1] == '!' && isalpha((unsigned char)s[c + 2])) {
        snprintf(end, cap, ">");
        return 1;
    }

    if (k < e && s[k] == '/') {
        k++;
    }

    while (k < e && nn + 1 < sizeof(name) && isalnum((unsigned char)s[k])) {
        name[nn++] = (char)tolower((unsigned char)s[k++]);
    }

    name[nn] = '\0';

    if (!nn || (k < e && s[k] != ' ' && s[k] != '\t' && s[k] != '>' && !(s[k] == '/' && k + 1 < e && s[k + 1] == '>'))) {
        return 0;
    }

    for (i = 0; raw[i]; i++) {
        if (strcmp(name, raw[i]) == 0 && s[c + 1] != '/') {
            snprintf(end, cap, "</%s>", raw[i]);
            return 1;
        }
    }

    for (i = 0; blocks[i]; i++) {
        if (strcmp(name, blocks[i]) == 0) {
            end[0] = '\0';  /* to a blank line */
            return 1;
        }
    }

    return 0;
}

/* a complete open or closing tag alone on its line (CommonMark's HTML block kind 7) */
static int lone_tag(const char* s, size_t c, size_t e) {
    size_t k = c + 1;

    if (c >= e || s[c] != '<') {
        return 0;
    }

    if (k < e && s[k] == '/') {
        k++;
    }

    if (k >= e || !isalpha((unsigned char)s[k])) {
        return 0;
    }

    while (k < e && (isalnum((unsigned char)s[k]) || s[k] == '-')) {
        k++;
    }

    while (k < e && s[k] != '>') {     /* attributes, quoted values may hold > */
        if (s[k] == '"' || s[k] == '\'') {
            char q = s[k++];

            while (k < e && s[k] != q) {
                k++;
            }
        }

        k++;
    }

    return k < e && s[k] == '>' && skip_ws(s, k + 1, e) == e;
}

/* ignoring ASCII case */
static int contains_ci(const char* s, size_t n, const char* w) {
    size_t i, k = strlen(w), j;

    for (i = 0; i + k <= n; i++) {
        for (j = 0; j < k && tolower((unsigned char)s[i + j]) == w[j]; j++) {
        }

        if (j == k) {
            return 1;
        }
    }

    return 0;
}

/* would this line, at c, start a block that ends a paragraph? */
static int interrupts(const char* s, size_t a, size_t c, size_t e) {
    char fc, end[16];
    size_t fl, k = c;
    int kind = 0, number = 0;

    if (indent_of(s, a, e) >= 4 + cols_to(s, a, a)) {
        return 0;
    }

    while (k < e && s[k] == '#') {
        k++;
    }

    return (k > c && k - c <= 6 && (k == e || s[k] == ' ' || s[k] == '\t')) || fence_start(s, c, e, &fc, &fl) ||
           thematic(s, c, e) || (c < e && s[c] == '>') || (c + 1 < e && s[c] == '$' && s[c + 1] == '$') ||
           html_start(s, c, e, end, sizeof(end)) ||
           (list_marker(s, c, e, &kind, &number) && skip_ws(s, c + list_marker(s, c, e, &kind, &number), e) < e &&
            (kind == 1 || number == 1));
}

pd_status pd_md_import(pd_doc* d, const char* s, size_t n) {
    pd_bld b;
    mctx m;
    mparse P;
    mline* lines = NULL;
    int32_t nl = 0, capl = 0, i, k;
    mnote_list notes;
    mrefs refs;
    uint8_t* skip;
    size_t i0 = 0;

    memset(&notes, 0, sizeof(notes));
    memset(&refs, 0, sizeof(refs));

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

    /* front matter: --- at the very top, to a --- or ... line */
    if (nl > 1 && lines[0].b - lines[0].a == 3 && memcmp(s + lines[0].a, "---", 3) == 0) {
        for (i = 1; i < nl; i++) {
            size_t len = lines[i].b - lines[i].a;

            if (len == 3 && (memcmp(s + lines[i].a, "---", 3) == 0 || memcmp(s + lines[i].a, "...", 3) == 0)) {
                break;
            }
        }

        if (i < nl && i > 1) {
            pd_doc_set_metadata(d, s + lines[1].a, lines[i - 1].b - lines[1].a);

            for (k = 0; k <= i; k++) {
                skip[k] = 1;
            }
        }
    }

    /* footnote definitions: [^label]: text, with indented continuation lines */
    for (i = 0; i < nl; i++) {
        size_t a = lines[i].a, e = lines[i].b, q;

        if (skip[i]) {
            continue;
        }

        if (e - a > 4 && s[a] == '[' && s[a + 1] == '^') {
            for (q = a + 2; q < e && s[q] != ']'; q++) {
            }

            if (q + 1 < e && s[q + 1] == ':' && q - a - 2 < 64 && q > a + 2) {
                mnote x;
                int32_t j = i + 1, cap = notes.cap;

                memset(&x, 0, sizeof(x));
                memcpy(x.label, s + a + 2, q - a - 2);
                x.a = skip_ws(s, q + 2, e);
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
        int fenced = 0, can = 1, in_item = 0;
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

            {
                size_t b0 = a;
                int quoted = 0, kk, nn;

                /* inside block quotes, and indented into list items, too */
                while (skip_ws(s, b0, e) < e && s[skip_ws(s, b0, e)] == '>' && cols_to(s, b0, skip_ws(s, b0, e)) < 4) {
                    b0 = skip_ws(s, b0, e) + 1;
                    b0 += b0 < e && s[b0] == ' ';
                    quoted = 1;
                }

                if (blank_line(s, b0, e)) {
                    can = 1;
                } else if (can && (cols_to(s, b0, skip_ws(s, b0, e)) < 4 || (in_item && cols_to(s, b0, skip_ws(s, b0, e)) < 8)) &&
                           ref_def(s, skip_ws(s, b0, e), e, &refs)) {
                    skip[i] = 1;
                } else {
                    can = s[skip_ws(s, b0, e)] == '#';   /* after a heading, a definition may follow */
                }

                /* whether the lines that follow are in a list item */
                if (!blank_line(s, b0, e)) {
                    in_item = list_marker(s, skip_ws(s, b0, e), e, &kk, &nn) != 0 ||
                              (in_item && cols_to(s, b0, skip_ws(s, b0, e)) >= 2);
                }

                (void)quoted;
            }
        }
    }

    bld_init(&b, d);
    memset(&m, 0, sizeof(m));
    m.b = &b;
    m.s = s;
    m.doc = s;
    m.notes = &notes;
    m.refs = &refs;
    memset(&P, 0, sizeof(P));
    P.b = &b;
    P.m = &m;
    P.s = s;

    for (i = 0; i < nl; i++) {
        size_t a = lines[i].a, e = lines[i].b, pos = a, c;
        int32_t matched = 0, lazy = 0;
        int blank;

        if (skip[i]) {
            continue;
        }

        /* the open containers this line continues */
        while (matched < P.nc) {
            mcont* C = &P.c[matched];

            if (C->type == 'q') {
                size_t q = skip_ws(s, pos, e);

                if (cols_to(s, pos, q) > 3 || q >= e || s[q] != '>') {
                    break;
                }

                pos = q + 1;

                if (pos < e && (s[pos] == ' ' || s[pos] == '\t')) {
                    pos++;
                }
            } else if (blank_line(s, pos, e)) {
                /* a blank line goes on in an item -- unless the item has nothing yet and this is its second */
            } else if (indent_of(s, pos, e) >= C->content) {
                pos = skip_cols(s, pos, e, C->content);
            } else {
                break;
            }

            matched++;
        }

        blank = blank_line(s, pos, e);
        c = skip_ws(s, pos, e);

        /* a paragraph goes on in containers the line does not continue ("lazy" lines) */
        if (matched < P.nc && P.leaf == LF_PARA && !blank && !interrupts(s, pos, c, e) &&
                !(list_marker(s, c, e, &k, &k) && cols_to(s, pos, c) < 4)) {
            lazy = 1;
        }

        if (!lazy && matched < P.nc) {  /* the others end here */
            leaf_close(&P);

            while (P.nc > matched) {
                P.nc--;

                if (P.c[P.nc].type == 'l') {    /* a next item at this depth may continue its list */
                    P.sib[P.nc] = P.c[P.nc];
                    P.sibok[P.nc] = 1;
                }
            }
        }

        if (lazy) {
            leaf_add(&P, s + c, e - c);
            continue;
        }

        /* leaves that take every line until their end */
        if (P.leaf == LF_FENCE) {
            size_t q = skip_ws(s, pos, e), r = 0;

            while (q + r < e && s[q + r] == P.fc) {
                r++;
            }

            if (r >= P.flen && skip_ws(s, q + r, e) == e && cols_to(s, pos, q) <= 3) {
                leaf_close(&P);
            } else {
                size_t from = skip_cols(s, pos, e, P.find);

                leaf_add(&P, s + from, e - from);
            }

            continue;
        }

        if (P.leaf == LF_HTML) {
            if (!P.html_end[0] && blank) {
                leaf_close(&P);
                continue;
            }

            leaf_add(&P, s + pos, e - pos);

            if (P.html_end[0] && contains_ci(s + pos, e - pos, P.html_end)) {
                leaf_close(&P);
            }

            continue;
        }

        if (P.leaf == LF_MATH) {
            size_t q;

            for (q = pos; q + 1 < e && !(s[q] == '$' && s[q + 1] == '$'); q++) {
            }

            if (P.text.n) {
                pb_putc(&P.text, ' ');
            }

            pb_put(&P.text, s + pos, (q + 1 < e ? q : e) - pos);

            if (q + 1 < e) {
                leaf_close(&P);
            }

            continue;
        }

        if (P.leaf == LF_TABLE) {
            if (!blank && memchr(s + pos, '|', e - pos)) {
                leaf_add(&P, s + pos, e - pos);
                continue;
            }

            leaf_close(&P);
        }

        if (blank) {
            int32_t j;

            if (P.leaf == LF_INDENT) {  /* blank lines inside indented code are kept */
                leaf_add(&P, "", 0);
                continue;
            }

            /* a term followed, after a blank line, by its definition */
            for (j = i + 1; j < nl && blank_line(s, lines[j].a, lines[j].b); j++) {
            }

            if (P.leaf == LF_PARA && !P.def && P.nlines == 1 && j < nl && P.nc == 0 &&
                    s[skip_ws(s, lines[j].a, lines[j].b)] == ':' &&
                    skip_ws(s, lines[j].a, lines[j].b) + 1 < lines[j].b &&
                    (s[skip_ws(s, lines[j].a, lines[j].b) + 1] == ' ' || s[skip_ws(s, lines[j].a, lines[j].b) + 1] == '\t')) {
                continue;
            }

            leaf_close(&P);

            for (k = 0; k < P.nc; k++) {
                if (P.c[k].type == 'l') {
                    mlistinfo* L = list_info(&P, P.c[k].list);

                    if (L && P.c[k].labelled) {
                        L->blank = 1;
                    }
                }
            }

            continue;
        }

        /* new containers */
        for (;;) {
            int kind = 0, number = 0;
            char mark = 0;
            size_t ml;

            c = skip_ws(s, pos, e);

            if (cols_to(s, pos, c) >= 4 && P.leaf != LF_PARA) {
                break;
            }

            if (c < e && s[c] == '>' && cols_to(s, pos, c) < 4) {
                leaf_close(&P);

                if (P.nc < 32) {
                    memset(&P.c[P.nc], 0, sizeof(mcont));
                    P.c[P.nc++].type = 'q';
                }

                pos = c + 1;

                if (pos < e && (s[pos] == ' ' || s[pos] == '\t')) {
                    pos++;
                }

                {   /* GitHub's alerts: a quote opening with [!NOTE], [!TIP], [!IMPORTANT], [!WARNING], [!CAUTION] */
                    size_t z = skip_ws(s, pos, e), ze = z + 2;

                    while (ze < e && isalpha((unsigned char)s[ze])) {
                        ze++;
                    }

                    if (z + 3 < e && s[z] == '[' && s[z + 1] == '!' && ze < e && s[ze] == ']' && ze - z - 2 < 12 &&
                            skip_ws(s, ze + 1, e) == e && P.nc > 0) {
                        size_t q2;

                        P.c[P.nc - 1].alert[0] = '!';

                        for (q2 = 0; q2 < ze - z - 2; q2++) {
                            P.c[P.nc - 1].alert[q2 + 1] = (char)tolower((unsigned char)s[z + 2 + q2]);
                        }

                        P.c[P.nc - 1].alert[ze - z - 1] = '\0';
                        pos = e;
                    }
                }

                continue;
            }

            if (cols_to(s, pos, c) < 4 && (ml = list_marker_c(s, c, e, &kind, &number, &mark)) != 0 && !thematic(s, c, e) &&
                    (P.leaf != LF_PARA || (skip_ws(s, c + ml, e) < e && (kind == 1 || number == 1)))) {
                mcont it;
                int32_t lvl = 0, q, depth = P.nc;
                size_t after = c + ml, content;
                mlistinfo* L;

                leaf_close(&P);

                for (q = 0; q < P.nc; q++) {
                    lvl += P.c[q].type == 'l';
                }

                /* content column: one space after the marker, or as many as there are up to four */
                content = cols_to(s, pos, c) + ml;

                if (skip_ws(s, after, e) == e) {
                    content = cols_to(s, pos, c) + (ml > 1 ? ml - 1 : ml) + 1;
                } else if (indent_of(s, after, e) < 4) {
                    content += indent_of(s, after, e);
                    after = skip_ws(s, after, e);
                }

                memset(&it, 0, sizeof(it));
                it.type = 'l';
                it.kind = kind;
                it.mark = mark;
                it.level = lvl < 9 ? lvl : 8;
                it.content = content;

                if (depth < 32 && P.sibok[depth] && P.sib[depth].kind == kind && P.sib[depth].mark == mark &&
                        P.sib[depth].level == it.level) {
                    it.list = P.sib[depth].list;    /* the next item of the same list */
                } else {
                    it.list = md_list(&b, kind, it.level, kind == 2 ? number : 1);
                }

                if (depth < 32) {
                    P.sibok[depth] = 0;
                }

                for (q = depth + 1; q < 32; q++) {
                    P.sibok[q] = 0;
                }

                list_more(&P);

                if ((L = list_info(&P, it.list)) != NULL && L->blank) {
                    L->loose = 1;
                    L->blank = 0;
                }

                /* a task: [ ] or [x] opening the item */
                if (after + 3 <= e && s[after] == '[' && (s[after + 1] == ' ' || s[after + 1] == 'x' || s[after + 1] == 'X') &&
                        s[after + 2] == ']' && (after + 3 == e || s[after + 3] == ' ' || s[after + 3] == '\t')) {
                    it.task = s[after + 1] == ' ' ? 1 : 2;
                    after = skip_ws(s, after + 3, e);
                }

                if (P.nc < 32) {
                    P.c[P.nc++] = it;
                }

                pos = after;

                if (skip_ws(s, pos, e) == e) {
                    break;
                }

                continue;
            }

            break;
        }

        c = skip_ws(s, pos, e);

        if (c >= e) {
            continue;   /* an item with nothing after its marker yet */
        }

        for (k = P.nc; k < 32; k++) {   /* a block between items ends their list */
            P.sibok[k] = 0;
        }

        /* pandoc's fenced divs: ::: name (or {.name}) opens one, ::: closes it */
        if (cols_to(s, pos, c) < 4 && c + 2 < e && s[c] == ':' && s[c + 1] == ':' && s[c + 2] == ':') {
            size_t z = c, ne;
            char name[32] = "";

            while (z < e && s[z] == ':') {
                z++;
            }

            z = skip_ws(s, z, e);

            if (z < e && s[z] == '{') {     /* {.name ...} */
                while (z < e && s[z] != '.' && s[z] != '}') {
                    z++;
                }

                z += z < e && s[z] == '.';
            }

            for (ne = z; ne < e && (isalnum((unsigned char)s[ne]) || s[ne] == '-' || s[ne] == '_'); ne++) {
            }

            snprintf(name, sizeof(name), "%.*s", (int)(ne - z < 31 ? ne - z : 31), s + z);

            if (name[0] || P.ndiv > 0) {
                leaf_close(&P);

                if (name[0] && P.ndiv < 8) {
                    snprintf(P.divs[P.ndiv++], sizeof(P.divs[0]), "%s", name);
                } else if (!name[0]) {
                    P.ndiv--;
                }

                continue;
            }
        }

        /* the paragraph under way: a setext underline, a table, a definition, or more of it */
        if (P.leaf == LF_PARA) {
            size_t q = c;

            if (cols_to(s, pos, c) < 4 && (s[c] == '=' || s[c] == '-') && !P.def) {
                char u = s[c];

                while (q < e && s[q] == u) {
                    q++;
                }

                if (skip_ws(s, q, e) == e) {
                    char st[16];

                    snprintf(st, sizeof(st), "Heading %d", u == '=' ? 1 : 2);
                    md_para(&P, st, PD_ROLE_HEADING, u == '=' ? 1 : 2, P.text.p ? P.text.p : "", P.text.n, 1);
                    P.leaf = LF_NONE;
                    P.text.n = 0;
                    P.nlines = 0;
                    continue;
                }
            }

            if (P.nlines == 1 && !P.def && table_delim(s, pos, e) && memchr(P.text.p ? P.text.p : "", '|', P.text.n)) {
                P.leaf = LF_TABLE;
                leaf_add(&P, s + pos, e - pos);
                continue;
            }

            if (s[c] == ':' && c + 1 < e && (s[c + 1] == ' ' || s[c + 1] == '\t') && P.nc == 0 &&
                    (P.def || P.nlines == 1)) {
                if (!P.def) {   /* the paragraph so far is the term */
                    md_para(&P, "Term", PD_ROLE_TERM, 0, P.text.p ? P.text.p : "", P.text.n, 0);
                } else {
                    leaf_close(&P);
                }

                P.text.n = 0;
                P.nlines = 0;
                P.leaf = LF_PARA;
                P.def = 1;
                leaf_add(&P, s + skip_ws(s, c + 1, e), e - skip_ws(s, c + 1, e));
                continue;
            }

            if (!interrupts(s, pos, c, e)) {
                leaf_add(&P, s + c, e - c);
                continue;
            }

            leaf_close(&P);
        }

        if (P.leaf == LF_INDENT) {
            if (indent_of(s, pos, e) >= 4) {
                size_t from = skip_cols(s, pos, e, 4);

                leaf_add(&P, s + from, e - from);
                continue;
            }

            leaf_close(&P);
        }

        list_more(&P);

        /* leaf starts */
        {
            char fc, st[16];
            size_t flen, q = c;
            int32_t lvl;

            if (cols_to(s, pos, c) >= 4) {  /* indented code */
                size_t from = skip_cols(s, pos, e, 4);

                P.leaf = LF_INDENT;
                leaf_add(&P, s + from, e - from);
                continue;
            }

            while (q < e && s[q] == '#') {
                q++;
            }

            if (q > c && q - c <= 6 && (q == e || s[q] == ' ' || s[q] == '\t')) {     /* ATX heading */
                size_t te = e;

                lvl = (int32_t)(q - c);

                while (te > q && (s[te - 1] == ' ' || s[te - 1] == '#')) {  /* closing #s */
                    te--;
                }

                snprintf(st, sizeof(st), "Heading %d", (int)lvl);
                q = skip_ws(s, q, te);
                md_para(&P, st, PD_ROLE_HEADING, lvl, s + q, te > q ? te - q : 0, 1);
                continue;
            }

            if (fence_start(s, c, e, &fc, &flen)) {
                size_t info = skip_ws(s, c + flen, e), ie;

                if (info < e && s[info] == '{') {   /* pandoc: {.python .numberLines}, the first class */
                    size_t z = info + 1;

                    while (z < e && s[z] != '.' && s[z] != '}') {
                        z++;
                    }

                    info = z < e && s[z] == '.' ? z + 1 : e;
                }

                ie = info;

                while (ie < e && s[ie] != ' ' && s[ie] != '\t' && s[ie] != '{' && s[ie] != '}') {
                    ie++;
                }

                P.leaf = LF_FENCE;
                P.fc = fc;
                P.flen = flen;
                P.find = cols_to(s, pos, c);
                snprintf(P.lang, sizeof(P.lang), "%.*s", (int)(ie - info < sizeof(P.lang) ? ie - info : 0), s + info);
                P.nlines = 0;
                continue;
            }

            if (thematic(s, c, e)) {
                bld_break(&b, PD_BREAK_RULE);
                continue;
            }

            if (c + 1 < e && s[c] == '$' && s[c + 1] == '$') {     /* $$display math$$ */
                size_t from = c + 2, z;

                for (z = from; z + 1 < e && !(s[z] == '$' && s[z + 1] == '$'); z++) {
                }

                P.leaf = LF_MATH;
                pb_put(&P.text, s + from, (z + 1 < e ? z : e) - from);

                if (z + 1 < e) {
                    leaf_close(&P);
                }

                continue;
            }

            if (html_start(s, c, e, P.html_end, sizeof(P.html_end)) || lone_tag(s, c, e)) {
                if (lone_tag(s, c, e) && !html_start(s, c, e, P.html_end, sizeof(P.html_end))) {
                    P.html_end[0] = '\0';  /* CommonMark's kind 7: to a blank line */
                }

                P.leaf = LF_HTML;
                leaf_add(&P, s + pos, e - pos);

                if (P.html_end[0] && contains_ci(s + c + 1, e - c - 1, P.html_end)) {
                    leaf_close(&P);
                }

                continue;
            }

            /* pandoc's line block: | lines, each kept a line; not a table, which has a delimiter row */
            if (c + 1 < e && s[c] == '|' && s[c + 1] == ' ' && !(i + 1 < nl && table_delim(s, lines[i + 1].a,
                    lines[i + 1].b))) {
                int32_t j = i;

                P.leaf = LF_PARA;

                while (j < nl) {
                    size_t la = skip_cols(s, lines[j].a, lines[j].b, cols_to(s, lines[i].a, pos)), lc = skip_ws(s, la, lines[j].b);

                    if (lc + 1 < lines[j].b && s[lc] == '|' && (s[lc + 1] == ' ' || lc + 1 == lines[j].b)) {
                        if (j > i) {
                            pb_puts(&P.text, "\\\n");    /* a hard break */
                        }

                        pb_put(&P.text, s + lc + 2, lines[j].b - lc - 2);
                    } else if (j > i && lc < lines[j].b && lc > la) {
                        pb_putc(&P.text, ' ');  /* an indented line goes on the one before */
                        pb_put(&P.text, s + lc, lines[j].b - lc);
                    } else {
                        break;
                    }

                    j++;
                }

                P.nlines = 1;
                leaf_close(&P);
                i = j - 1;
                continue;
            }

            P.leaf = LF_PARA;
            leaf_add(&P, s + c, e - c);
        }
    }

    leaf_close(&P);

    /* a loose list's items say so */
    for (i = 0; i < P.nli; i++) {
        for (k = 0; P.li[i].loose && k < P.li[i].n; k++) {
            pd_para_attrs at;

            if (pd_doc_para_attrs(d, P.li[i].paras[k], &at) == PD_OK) {
                at.loose = 1;
                pd_doc_set_para_attrs(d, P.li[i].paras[k], &at);
            }
        }

        free(P.li[i].paras);
    }

    free(P.li);
    pb_free(&P.text);
    free(lines);
    free(skip);
    free(notes.v);

    for (i = 0; i < refs.n; i++) {
        free(refs.v[i].url);
        free(refs.v[i].title);
    }

    free(refs.v);
    return bld_finish(&b);
}
