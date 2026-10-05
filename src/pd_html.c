/*
 * Parade HTML: export (a self-contained page: images as data: URIs,
 * footnotes as an endnote list) and a tolerant import (web pages, office
 * and browser clipboard fragments, Parade's and pandoc's own output)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

/* ------------------------------------------------------------------ */
/* export                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const pd_doc* d;
    pd_buf* o;
    pd_numbers nb;
    pd_block_id notes[4096];
    int32_t nnotes;
    /* open lists */
    int32_t ldepth;
    int lkind[10];
    /* current paragraph */
    pd_block_id para;
    pd_char_props base;
    int in_link, in_code;
    int quote_open, code_open;
} hx;

static void esc(pd_buf* o, const char* s, size_t n, int attr) {
    size_t i, j = 0;

    for (i = 0; i < n; i++) {
        const char* r = s[i] == '&' ? "&amp;" : s[i] == '<' ? "&lt;" : s[i] == '>' ? "&gt;" :
                        attr && s[i] == '"' ? "&quot;" : NULL;

        if (r) {
            pb_put(o, s + j, i - j);
            pb_puts(o, r);
            j = i + 1;
        }
    }

    pb_put(o, s + j, n - j);
}

static void color_css(pd_buf* o, uint32_t c) {
    pb_printf(o, "#%02x%02x%02x", (unsigned)((c >> 16) & 255), (unsigned)((c >> 8) & 255), (unsigned)(c & 255));
}

static void close_lists(hx* x, int32_t keep) {
    while (x->ldepth > keep) {
        pb_puts(x->o, x->lkind[x->ldepth - 1] == 2 ? "</li></ol>\n" : "</li></ul>\n");
        x->ldepth--;
    }
}

static void close_groups(hx* x) {
    if (x->quote_open) {
        pb_puts(x->o, "</blockquote>\n");
        x->quote_open = 0;
    }

    if (x->code_open) {
        pb_puts(x->o, "</code></pre>\n");
        x->code_open = 0;
    }
}

static int hx_span(void* user, const pd_span* sp) {
    hx* x = (hx*)user;
    pd_buf* o = x->o;
    const pd_char_props* c = &sp->cp, *b = &x->base;

    if (sp->is_object) {
        const pd_inline* ob = &sp->obj;

        switch (ob->kind) {
            case PD_INLINE_IMAGE: {
                const char* mime;
                const void* data;
                size_t len;

                if (pd_doc_resource(x->d, ob->resource, &mime, &data, &len) == PD_OK) {
                    pb_puts(o, "<img src=\"data:");
                    esc(o, mime, strlen(mime), 1);
                    pb_puts(o, ";base64,");
                    pb_base64(o, (const unsigned char*)data, len);
                    pb_printf(o, "\" width=\"%d\" height=\"%d\" alt=\"\">", (int)(ob->width / 65536 * 4 / 3),
                              (int)(ob->height / 65536 * 4 / 3));
                }

                break;
            }

            case PD_INLINE_EQUATION:
                pb_puts(o, "<span class=\"math\">\\(");

                if (ob->source) {
                    esc(o, ob->source, (size_t)ob->source_len, 0);
                }

                pb_puts(o, "\\)</span>");
                break;

            case PD_INLINE_FIELD:
                if (ob->field == PD_FIELD_SEQ) {
                    pb_printf(o, "%d", (int)pd_numbers_at(&x->nb, x->para, sp->offset));
                } else if (ob->field == PD_FIELD_REF_NUMBER) {
                    pb_printf(o, "<a href=\"#b%u\">%d</a>", (unsigned)ob->target, (int)pd_numbers_ref(&x->nb, ob->target));
                }

                break;

            case PD_INLINE_FOOTNOTE: {
                int32_t n = pd_numbers_at(&x->nb, x->para, sp->offset);

                if (x->nnotes < 4096) {
                    x->notes[x->nnotes++] = ob->target;
                }

                pb_printf(o, "<sup class=\"footnote-ref\"><a href=\"#fn%d\" id=\"fnref%d\">%d</a></sup>", (int)n, (int)n,
                          (int)n);
                break;
            }

            case PD_INLINE_LINK:
                if (x->in_link) {
                    pb_puts(o, "</a>");
                    x->in_link = 0;
                }

                if (ob->source && ob->source_len > 0) {
                    pb_puts(o, "<a href=\"");
                    esc(o, ob->source, (size_t)ob->source_len, 1);
                    pb_puts(o, "\">");
                    x->in_link = 1;
                }

                break;

            case PD_INLINE_BOOKMARK:
                pb_printf(o, "<a id=\"%s\"></a>", ob->name);
                break;

            case PD_INLINE_TAB:
                pb_putc(o, '\t');
                break;
        }

        return o->err;
    }

    {
        int bold = c->weight >= 600 && b->weight < 600, it = c->italic && !b->italic;
        int mono = !x->in_code && pd_conv_is_mono(c) && !pd_conv_is_mono(b);
        int span = c->color != b->color || c->background != b->background || c->size != b->size ||
                   (!mono && strcmp(c->family, b->family) && !pd_conv_is_mono(c));
        size_t i, j = 0;

        if (bold) {
            pb_puts(o, "<strong>");
        }

        if (it) {
            pb_puts(o, "<em>");
        }

        if (c->underline && !b->underline) {
            pb_puts(o, "<u>");
        }

        if (c->strike && !b->strike) {
            pb_puts(o, "<s>");
        }

        if (c->shift == PD_SHIFT_SUPER) {
            pb_puts(o, "<sup>");
        } else if (c->shift == PD_SHIFT_SUB) {
            pb_puts(o, "<sub>");
        }

        if (mono) {
            pb_puts(o, "<code>");
        }

        if (span) {
            pb_puts(o, "<span style=\"");

            if (c->color != b->color) {
                pb_puts(o, "color:");
                color_css(o, c->color);
                pb_putc(o, ';');
            }

            if (c->background != b->background && c->background) {
                pb_puts(o, "background-color:");
                color_css(o, c->background);
                pb_putc(o, ';');
            }

            if (c->size != b->size) {
                pb_printf(o, "font-size:%gpt;", c->size / 65536.0);
            }

            if (strcmp(c->family, b->family) && !pd_conv_is_mono(c)) {
                pb_puts(o, "font-family:'");
                esc(o, c->family, strlen(c->family), 1);
                pb_puts(o, "';");
            }

            pb_puts(o, "\">");
        }

        for (i = 0; i < sp->len; i++) {     /* line breaks inside a paragraph */
            if (sp->text[i] == '\n' && !x->in_code) {
                esc(o, sp->text + j, i - j, 0);
                pb_puts(o, "<br>");
                j = i + 1;
            }
        }

        esc(o, sp->text + j, sp->len - j, 0);

        if (span) {
            pb_puts(o, "</span>");
        }

        if (mono) {
            pb_puts(o, "</code>");
        }

        if (c->shift == PD_SHIFT_SUPER) {
            pb_puts(o, "</sup>");
        } else if (c->shift == PD_SHIFT_SUB) {
            pb_puts(o, "</sub>");
        }

        if (c->strike && !b->strike) {
            pb_puts(o, "</s>");
        }

        if (c->underline && !b->underline) {
            pb_puts(o, "</u>");
        }

        if (it) {
            pb_puts(o, "</em>");
        }

        if (bold) {
            pb_puts(o, "</strong>");
        }
    }

    return o->err;
}

static void hx_inline(hx* x, pd_block_id p) {
    x->para = p;
    x->in_link = 0;
    pd_conv_base_props(x->d, p, &x->base);
    pd_conv_spans(x->d, p, hx_span, x);

    if (x->in_link) {
        pb_puts(x->o, "</a>");
        x->in_link = 0;
    }
}

static void hx_block(hx* x, pd_block_id id, int in_figure);

static void hx_para(hx* x, pd_block_id p, int in_figure) {
    pd_block_info bi;
    pd_para_props pp;
    int32_t level = 0;
    int kind = pd_conv_list_kind(x->d, p, &level);
    const char* align = "";

    pd_doc_block_info(x->d, p, &bi);
    pd_doc_style_resolve(x->d, bi.style, &pp, NULL);

    {
        pd_para_props dp;

        if (pd_doc_para_props(x->d, p, &dp) == PD_OK && (dp.mask & PD_PP_ALIGN)) {
            pp.align = dp.align;
        }
    }

    align = pp.align == PD_ALIGN_CENTER ? " style=\"text-align:center\"" : pp.align == PD_ALIGN_RIGHT ?
            " style=\"text-align:right\"" : "";

    if (bi.role != PD_ROLE_CODE && x->code_open) {
        pb_puts(x->o, "</code></pre>\n");
        x->code_open = 0;
    }

    if (bi.role != PD_ROLE_QUOTE && x->quote_open) {
        pb_puts(x->o, "</blockquote>\n");
        x->quote_open = 0;
    }

    /* lists: open, close or continue to this item's level */
    if (kind) {
        close_lists(x, level + 1);

        if (x->ldepth == level + 1 && x->lkind[level] != kind) {
            close_lists(x, level);
        }

        if (x->ldepth == level + 1) {
            pb_puts(x->o, "</li>\n");
        }

        while (x->ldepth < level + 1) {
            x->lkind[x->ldepth] = x->ldepth == level ? kind : 1;
            pb_puts(x->o, x->lkind[x->ldepth] == 2 ? "<ol>\n" : "<ul>\n");

            if (x->ldepth < level) {
                pb_puts(x->o, "<li>");
            }

            x->ldepth++;
        }

        pb_printf(x->o, "<li%s>", align);
        hx_inline(x, p);
        return;
    }

    close_lists(x, 0);

    switch (bi.role) {
        case PD_ROLE_TITLE:
            pb_printf(x->o, "<h1 class=\"title\"%s>", align);
            hx_inline(x, p);
            pb_puts(x->o, "</h1>\n");
            return;

        case PD_ROLE_HEADING:
            pb_printf(x->o, "<h%d%s>", (int)bi.level, align);
            hx_inline(x, p);
            pb_printf(x->o, "</h%d>\n", (int)bi.level);
            return;

        case PD_ROLE_CAPTION:
            pb_printf(x->o, in_figure ? "<figcaption%s>" : "<p class=\"caption\"%s>", align);
            hx_inline(x, p);
            pb_puts(x->o, in_figure ? "</figcaption>\n" : "</p>\n");
            return;

        case PD_ROLE_QUOTE:
            if (!x->quote_open) {
                pb_puts(x->o, "<blockquote>\n");
                x->quote_open = 1;
            }

            pb_printf(x->o, "<p%s>", align);
            hx_inline(x, p);
            pb_puts(x->o, "</p>\n");
            return;

        case PD_ROLE_EQUATION:
            pb_puts(x->o, "<p class=\"equation\">");
            hx_inline(x, p);
            pb_puts(x->o, "</p>\n");
            return;

        case PD_ROLE_CODE:
            pb_puts(x->o, x->code_open ? "\n" : "<pre><code>");
            x->code_open = 1;
            x->in_code = 1;
            hx_inline(x, p);
            x->in_code = 0;
            return;

        default:
            pb_printf(x->o, "<p%s>", align);
            hx_inline(x, p);
            pb_puts(x->o, "</p>\n");
    }
}

/* the content of a cell: a single paragraph goes inline */
static void hx_cell_content(hx* x, pd_block_id cell) {
    pd_block_info ci, pi;

    pd_doc_block_info(x->d, cell, &ci);

    if (ci.child_count == 1 && pd_doc_block_info(x->d, pd_doc_child(x->d, cell, 0), &pi) == PD_OK &&
            pi.kind == PD_BLOCK_PARAGRAPH && pi.role == PD_ROLE_BODY && !pi.list) {
        hx_inline(x, pi.id);
    } else {
        int32_t i;

        pb_putc(x->o, '\n');

        for (i = 0; i < ci.child_count; i++) {
            hx_block(x, pd_doc_child(x->d, cell, i), 0);
        }

        close_lists(x, 0);
        close_groups(x);
    }
}

static void hx_table(hx* x, pd_block_id t) {
    pd_block_info ti;
    pd_table_props tp;
    int32_t r, c;

    pd_doc_block_info(x->d, t, &ti);
    pd_doc_table_props(x->d, t, &tp);
    pb_puts(x->o, tp.border ? "<table class=\"grid\">\n" : "<table>\n");

    for (r = 0; r < ti.child_count; r++) {
        pd_block_id row = pd_doc_child(x->d, t, r);
        pd_block_info ri;
        int head = r < tp.header_rows;

        if (r == 0 && head) {
            pb_puts(x->o, "<thead>\n");
        } else if (r == tp.header_rows) {
            pb_puts(x->o, "<tbody>\n");
        }

        pd_doc_block_info(x->d, row, &ri);
        pb_puts(x->o, "<tr>");

        for (c = 0; c < ri.child_count; c++) {
            pd_block_id cell = pd_doc_child(x->d, row, c);
            pd_cell_props cp;

            pd_doc_cell_props(x->d, cell, &cp);
            pb_puts(x->o, head ? "<th" : "<td");

            if (cp.col_span > 1) {
                pb_printf(x->o, " colspan=\"%d\"", (int)cp.col_span);
            }

            if (cp.background || cp.valign) {
                pb_puts(x->o, " style=\"");

                if (cp.background) {
                    pb_puts(x->o, "background-color:");
                    color_css(x->o, cp.background);
                    pb_putc(x->o, ';');
                }

                if (cp.valign) {
                    pb_puts(x->o, cp.valign == 1 ? "vertical-align:middle;" : "vertical-align:bottom;");
                }

                pb_putc(x->o, '"');
            }

            pb_putc(x->o, '>');
            hx_cell_content(x, cell);
            pb_puts(x->o, head ? "</th>" : "</td>");
        }

        pb_puts(x->o, "</tr>\n");

        if (head && r + 1 == tp.header_rows) {
            pb_puts(x->o, "</thead>\n");
        }
    }

    if (ti.child_count > tp.header_rows) {
        pb_puts(x->o, "</tbody>\n");
    }

    pb_puts(x->o, "</table>\n");
}

static void hx_block(hx* x, pd_block_id id, int in_figure) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(x->d, id, &bi) != PD_OK) {
        return;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        hx_para(x, id, in_figure);
        return;
    }

    close_lists(x, 0);
    close_groups(x);

    switch (bi.kind) {
        case PD_BLOCK_FLOAT:
            pb_printf(x->o, "<figure id=\"b%u\">\n", (unsigned)id);

            for (i = 0; i < bi.child_count; i++) {
                hx_block(x, pd_doc_child(x->d, id, i), 1);
            }

            close_lists(x, 0);
            close_groups(x);
            pb_puts(x->o, "</figure>\n");
            break;

        case PD_BLOCK_TABLE:
            hx_table(x, id);
            break;

        case PD_BLOCK_BREAK:
            pb_puts(x->o, "<div class=\"page-break\" style=\"break-after:page\"></div>\n");
            break;

        default:
            for (i = 0; i < bi.child_count; i++) {
                hx_block(x, pd_doc_child(x->d, id, i), in_figure);
            }

            close_lists(x, 0);
            close_groups(x);
    }
}

/* the document title: the first TITLE paragraph's text */
static void hx_title(hx* x) {
    pd_block_id p = pd_doc_next_paragraph(x->d, 0);
    int k;

    for (k = 0; p && k < 50; k++, p = pd_doc_next_paragraph(x->d, p)) {
        pd_block_info bi;

        if (pd_doc_block_info(x->d, p, &bi) == PD_OK && (bi.role == PD_ROLE_TITLE || bi.role == PD_ROLE_HEADING)) {
            const char* t;
            uint32_t len, i, j = 0;

            pd_doc_para_text(x->d, p, &t, &len);

            for (i = 0; i + 2 < len; i++) {     /* without objects */
                if ((unsigned char)t[i] == 0xEF && (unsigned char)t[i + 1] == 0xBF && (unsigned char)t[i + 2] == 0xBC) {
                    esc(x->o, t + j, i - j, 0);
                    j = i + 3;
                }
            }

            esc(x->o, t + j, len - j, 0);
            return;
        }
    }

    pb_puts(x->o, "Document");
}

pd_status pd_html_export(const pd_doc* d, pd_buf* o) {
    hx* x = (hx*)calloc(1, sizeof(hx));
    int32_t i, k;

    if (!x) {
        return PD_ERR_NOMEM;
    }

    x->d = d;
    x->o = o;
    pd_numbers_init(&x->nb, d);
    pb_puts(o, "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n<title>");
    hx_title(x);
    pb_puts(o, "</title>\n<style>\n"
            "body { font-family: serif; max-width: 42em; margin: 2em auto; line-height: 1.4; }\n"
            "table { border-collapse: collapse; margin: 1em 0; }\n"
            "table.grid td, table.grid th { border: 1px solid #000; padding: 4px; }\n"
            "figure { text-align: center; }\n"
            ".footnotes { font-size: smaller; }\n"
            "</style>\n</head>\n<body>\n");
    hx_block(x, pd_doc_root(d), 0);
    close_lists(x, 0);
    close_groups(x);

    if (x->nnotes) {
        pb_puts(o, "<section class=\"footnotes\">\n<hr>\n<ol>\n");

        for (i = 0; i < x->nnotes; i++) {
            pd_block_info si;

            pb_printf(o, "<li id=\"fn%d\">", (int)i + 1);
            pd_doc_block_info(d, x->notes[i], &si);

            for (k = 0; k < si.child_count; k++) {
                pd_block_id p = pd_doc_child(d, x->notes[i], k);

                if (si.child_count == 1) {
                    hx_inline(x, p);
                } else {
                    hx_block(x, p, 0);
                }
            }

            close_lists(x, 0);
            close_groups(x);
            pb_printf(o, " <a href=\"#fnref%d\" class=\"footnote-back\">\xE2\x86\xA9</a></li>\n", (int)i + 1);
        }

        pb_puts(o, "</ol>\n</section>\n");
    }

    pb_puts(o, "</body>\n</html>\n");
    pd_numbers_free(&x->nb);
    free(x);
    return o->err ? PD_ERR_NOMEM : PD_OK;
}

/* ------------------------------------------------------------------ */
/* import                                                             */
/* ------------------------------------------------------------------ */

enum {
    E_P = 1, E_H, E_LI, E_UL, E_OL, E_PRE, E_QUOTE, E_TABLE, E_TR, E_CELL, E_FIG, E_CAP, E_DIV, E_THEAD, E_SKIP,
    E_INLINE, E_BARRIER
};

typedef struct {
    char name[64];
    int kind;
    int level;                  /* headings */
    int align;                  /* -1 = none */
    pd_char_props cp;           /* character state to restore on close */
    int title;
} hel;

typedef struct {
    char id[128];
    size_t start, end;          /* inner HTML of the note's <li> */
} hnote;

typedef struct {
    pd_bld* b;
    hel stack[128];
    int32_t depth;
    int space;                  /* collapsed whitespace pending before the next word */
    hnote* notes;
    int32_t nnotes;
    int skip_anchor;            /* inside a footnote back-link or reference: drop its text */
    const char* src;
} hi;

static int css_color(const char* v, uint32_t* out) {
    static const struct {
        const char* n;
        uint32_t c;
    } names[] = {
        { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
        { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
        { "maroon", 0x800000 }, { "navy", 0x000080 }, { "purple", 0x800080 }, { "teal", 0x008080 },
        { "orange", 0xFFA500 }, { "lime", 0x00FF00 }, { "aqua", 0x00FFFF }, { "fuchsia", 0xFF00FF }
    };
    size_t i;

    while (*v == ' ') {
        v++;
    }

    if (v[0] == '#') {
        unsigned r, g, b;
        size_t n = strspn(v + 1, "0123456789abcdefABCDEF");

        if (n == 6 && sscanf(v + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
            *out = 0xFF000000u | (r << 16) | (g << 8) | b;
            return 1;
        }

        if (n == 3 && sscanf(v + 1, "%1x%1x%1x", &r, &g, &b) == 3) {
            *out = 0xFF000000u | (r * 17 << 16) | (g * 17 << 8) | (b * 17);
            return 1;
        }

        return 0;
    }

    if (strncmp(v, "rgb", 3) == 0) {
        unsigned r, g, b;
        const char* p = strchr(v, '(');

        if (p && sscanf(p + 1, "%u ,%u ,%u", &r, &g, &b) == 3) {
            *out = 0xFF000000u | ((r & 255) << 16) | ((g & 255) << 8) | (b & 255);
            return 1;
        }

        return 0;
    }

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strncmp(v, names[i].n, strlen(names[i].n)) == 0) {
            *out = 0xFF000000u | names[i].c;
            return 1;
        }
    }

    return 0;
}

/* a CSS length in sp (pt, px, em at 12pt) */
static pd_sp css_length(const char* v) {
    double x = atof(v);
    const char* u = v;

    while (*u && (isdigit((unsigned char)*u) || *u == '.' || *u == ' ' || *u == '-')) {
        u++;
    }

    if (strncmp(u, "px", 2) == 0) {
        x *= 0.75;
    } else if (strncmp(u, "em", 2) == 0 || strncmp(u, "rem", 3) == 0) {
        x *= 12;
    } else if (*u == '%') {
        x *= 0.12;
    }

    return (pd_sp)(x * 65536);
}

/* the value of a CSS property in a style attribute */
static int css_prop(const char* style, const char* prop, char* out, size_t cap) {
    const char* p = style;
    size_t ln = strlen(prop);

    while ((p = strstr(p, prop)) != NULL) {
        const char* q = p + ln;

        if ((p == style || p[-1] == ';' || p[-1] == ' ' || p[-1] == '"') && (*q == ':' || *q == ' ')) {
            size_t k = 0;

            while (*q == ' ' || *q == ':') {
                q++;
            }

            while (q[k] && q[k] != ';' && k + 1 < cap) {
                out[k] = q[k];
                k++;
            }

            while (k > 0 && out[k - 1] == ' ') {
                k--;
            }

            out[k] = '\0';
            return 1;
        }

        p = q;
    }

    return 0;
}

static void apply_style(pd_char_props* cp, const char* style) {
    char v[96];
    uint32_t c;

    if (css_prop(style, "font-weight", v, sizeof(v))) {
        cp->mask |= PD_CP_WEIGHT;
        cp->weight = strstr(v, "bold") || atoi(v) >= 600 ? 700 : 400;
    }

    if (css_prop(style, "font-style", v, sizeof(v))) {
        cp->mask |= PD_CP_ITALIC;
        cp->italic = strstr(v, "italic") || strstr(v, "oblique");
    }

    if (css_prop(style, "text-decoration", v, sizeof(v)) || css_prop(style, "text-decoration-line", v, sizeof(v))) {
        if (strstr(v, "underline")) {
            cp->mask |= PD_CP_UNDERLINE;
            cp->underline = 1;
        }

        if (strstr(v, "line-through")) {
            cp->mask |= PD_CP_STRIKE;
            cp->strike = 1;
        }
    }

    if (css_prop(style, "color", v, sizeof(v)) && css_color(v, &c)) {
        cp->mask |= PD_CP_COLOR;
        cp->color = c;
    }

    if ((css_prop(style, "background-color", v, sizeof(v)) || css_prop(style, "background", v, sizeof(v))) &&
            css_color(v, &c)) {
        cp->mask |= PD_CP_BACKGROUND;
        cp->background = c;
    }

    if (css_prop(style, "font-size", v, sizeof(v)) && isdigit((unsigned char)v[0])) {
        pd_sp sz = css_length(v);

        if (sz >= PD_PT(1) && sz <= PD_PT(500)) {
            cp->mask |= PD_CP_SIZE;
            cp->size = sz;
        }
    }

    if (css_prop(style, "font-family", v, sizeof(v))) {
        char* f = v;
        size_t k;

        while (*f == '\'' || *f == '"' || *f == ' ') {
            f++;
        }

        for (k = 0; f[k] && f[k] != ',' && f[k] != '\'' && f[k] != '"' && k + 1 < sizeof(cp->family); k++) {
        }

        if (k > 0) {
            memcpy(cp->family, f, k);
            cp->family[k] = '\0';
            cp->mask |= PD_CP_FAMILY;
        }
    }

    if (css_prop(style, "vertical-align", v, sizeof(v))) {
        if (strstr(v, "super")) {
            cp->mask |= PD_CP_SHIFT;
            cp->shift = PD_SHIFT_SUPER;
        } else if (strstr(v, "sub")) {
            cp->mask |= PD_CP_SHIFT;
            cp->shift = PD_SHIFT_SUB;
        }
    }
}

static int align_of(const pd_markup* m) {
    char v[64], st[512];

    if (mu_attr(m, "style", st, sizeof(st)) && css_prop(st, "text-align", v, sizeof(v))) {
        return strstr(v, "center") ? PD_ALIGN_CENTER : strstr(v, "right") ? PD_ALIGN_RIGHT : strstr(v, "justify") ?
               PD_ALIGN_JUSTIFY : PD_ALIGN_LEFT;
    }

    if (mu_attr(m, "align", v, sizeof(v))) {
        return strstr(v, "center") ? PD_ALIGN_CENTER : strstr(v, "right") ? PD_ALIGN_RIGHT : strstr(v, "justify") ?
               PD_ALIGN_JUSTIFY : PD_ALIGN_LEFT;
    }

    return -1;
}

static int block_kind(const char* n, int* level) {
    if (n[0] == 'h' && n[1] >= '1' && n[1] <= '6' && n[2] == '\0') {
        *level = n[1] - '0';
        return E_H;
    }

    if (!strcmp(n, "p") || !strcmp(n, "dt") || !strcmp(n, "dd") || !strcmp(n, "address")) {
        return E_P;
    }

    if (!strcmp(n, "li")) {
        return E_LI;
    }

    if (!strcmp(n, "ul") || !strcmp(n, "menu") || !strcmp(n, "dl")) {
        return E_UL;
    }

    if (!strcmp(n, "ol")) {
        return E_OL;
    }

    if (!strcmp(n, "pre")) {
        return E_PRE;
    }

    if (!strcmp(n, "blockquote")) {
        return E_QUOTE;
    }

    if (!strcmp(n, "table")) {
        return E_TABLE;
    }

    if (!strcmp(n, "tr")) {
        return E_TR;
    }

    if (!strcmp(n, "td") || !strcmp(n, "th")) {
        return E_CELL;
    }

    if (!strcmp(n, "thead")) {
        return E_THEAD;
    }

    if (!strcmp(n, "figure")) {
        return E_FIG;
    }

    if (!strcmp(n, "figcaption") || !strcmp(n, "caption")) {
        return E_CAP;
    }

    if (!strcmp(n, "div") || !strcmp(n, "section") || !strcmp(n, "article") || !strcmp(n, "main") ||
            !strcmp(n, "header") || !strcmp(n, "footer") || !strcmp(n, "nav") || !strcmp(n, "aside") ||
            !strcmp(n, "body") || !strcmp(n, "html") || !strcmp(n, "center") || !strcmp(n, "tbody") ||
            !strcmp(n, "tfoot") || !strcmp(n, "form") || !strcmp(n, "fieldset")) {
        return E_DIV;
    }

    if (!strcmp(n, "head") || !strcmp(n, "script") || !strcmp(n, "style") || !strcmp(n, "noscript") ||
            !strcmp(n, "template") || !strcmp(n, "svg") || !strcmp(n, "math") || !strcmp(n, "object") ||
            !strcmp(n, "iframe") || !strcmp(n, "select") || !strcmp(n, "button")) {
        return E_SKIP;
    }

    return 0;
}

/* the paragraph a text run opens, from the enclosing elements */
static void hi_begin_para(hi* h) {
    pd_bld* b = h->b;
    int32_t i, lists = 0, lkind = 1, align = -1;
    const hel* inner = NULL;

    for (i = h->depth - 1; i >= 0; i--) {
        const hel* e = &h->stack[i];

        if (!inner && (e->kind == E_H || e->kind == E_PRE || e->kind == E_QUOTE || e->kind == E_LI || e->kind == E_CAP ||
                       (e->kind == E_P && e->title == 2))) {
            inner = e;
        }

        if (align < 0 && e->align >= 0) {
            align = e->align;
        }

        if (e->kind == E_UL || e->kind == E_OL) {
            if (lists == 0) {
                lkind = e->kind == E_OL ? 2 : 1;
            }

            lists++;
        }

        if (e->kind == E_CELL || e->kind == E_FIG || e->kind == E_BARRIER) {
            break;
        }
    }

    if (inner && inner->kind == E_H) {
        if (inner->title) {
            bld_para_style(b, "Title", PD_ROLE_TITLE, 0);
        } else {
            char st[16];

            snprintf(st, sizeof(st), "Heading %d", inner->level);
            bld_para_style(b, st, PD_ROLE_HEADING, inner->level);
        }
    } else if (inner && inner->kind == E_PRE) {
        bld_para_style(b, "Code", PD_ROLE_CODE, 0);
    } else if (inner && inner->kind == E_QUOTE) {
        bld_para_style(b, "Quote", PD_ROLE_QUOTE, 0);
    } else if (inner && inner->kind == E_CAP) {
        bld_para_style(b, "Caption", PD_ROLE_CAPTION, 0);
    } else if (inner && inner->kind == E_P && inner->title == 2) {
        bld_para_style(b, NULL, PD_ROLE_EQUATION, 0);
    }

    if (lists > 0) {
        bld_list(b, lkind, lists - 1);
    }

    if (align >= 0) {
        b->pp.mask |= PD_PP_ALIGN;
        b->pp.align = align;
    }

    bld_begin_para(b);
    h->space = 0;
}

static int in_pre(const hi* h) {
    int32_t i;

    for (i = h->depth - 1; i >= 0; i--) {
        if (h->stack[i].kind == E_PRE) {
            return 1;
        }
    }

    return 0;
}

static void hi_text(hi* h, const char* raw, size_t n) {
    pd_buf t;
    size_t i, j;

    if (h->skip_anchor) {
        return;
    }

    memset(&t, 0, sizeof(t));
    mu_decode(raw, n, &t);

    if (t.n == 0) {
        pb_free(&t);
        return;
    }

    if (in_pre(h)) {
        if (!h->b->para) {
            hi_begin_para(h);
        }

        bld_text(h->b, t.p, t.n);
        pb_free(&t);
        return;
    }

    /* collapse whitespace; a paragraph never starts or ends with it */
    for (i = 0; i < t.n;) {
        if (isspace((unsigned char)t.p[i]) || (unsigned char)t.p[i] == 0) {
            h->space = 1;
            i++;
            continue;
        }

        for (j = i; j < t.n && !isspace((unsigned char)t.p[j]); j++) {
        }

        if (!h->b->para) {
            hi_begin_para(h);
        } else if (h->space) {
            bld_text(h->b, " ", 1);
        }

        h->space = 0;
        bld_text(h->b, t.p + i, j - i);
        i = j;
    }

    pb_free(&t);
}

static void hi_end_para(hi* h) {
    bld_end_para(h->b);
    h->space = 0;
}

/* an HTML width/height attribute: pixels unless it says pt */
static pd_sp attr_length(const pd_markup* m, const char* name, pd_sp dflt) {
    char v[32];
    double x;

    if (!mu_attr(m, name, v, sizeof(v)) || strchr(v, '%') || (x = atof(v)) <= 0) {
        return dflt;
    }

    return (pd_sp)(x * (strstr(v, "pt") ? 1.0 : 0.75) * 65536);
}

static void hi_image(hi* h, const pd_markup* m) {
    char* src = (char*)malloc(m->alen + 1);
    pd_inline o;

    if (!src) {
        return;
    }

    if (mu_attr(m, "src", src, m->alen + 1) && strncmp(src, "data:", 5) == 0 && strstr(src, ";base64,")) {
        char mime[64];
        const char* semi = strchr(src, ';'), *data = strstr(src, ";base64,") + 8;
        size_t ml = (size_t)(semi - src - 5), dl = strlen(data);
        unsigned char* bytes = (unsigned char*)malloc(dl * 3 / 4 + 4);

        if (bytes && ml < sizeof(mime)) {
            size_t nb;

            memcpy(mime, src + 5, ml);
            mime[ml] = '\0';
            nb = pd_base64_decode(data, dl, bytes);
            memset(&o, 0, sizeof(o));
            o.kind = PD_INLINE_IMAGE;

            if (nb > 0 && pd_doc_add_resource(h->b->d, mime, bytes, nb, &o.resource) == PD_OK) {
                o.width = attr_length(m, "width", PD_PT(100));
                o.height = attr_length(m, "height", PD_PT(100));

                if (o.width > 0 && o.height > 0) {
                    if (!h->b->para) {
                        hi_begin_para(h);
                    }

                    bld_inline(h->b, &o);
                }
            }
        }

        free(bytes);
    } else if (mu_attr(m, "alt", src, m->alen + 1) && src[0]) {
        hi_text(h, src, strlen(src));
    }

    free(src);
}

static void hi_parse(hi* h, const char* s, size_t n, int depth);

static const hnote* find_note(const hi* h, const char* href) {
    int32_t i;

    if (href[0] != '#') {
        return NULL;
    }

    for (i = 0; i < h->nnotes; i++) {
        if (strcmp(h->notes[i].id, href + 1) == 0) {
            return &h->notes[i];
        }
    }

    return NULL;
}

/* end the elements from index i up */
static void close_from(hi* h, int32_t i) {
    int32_t k;

    for (k = h->depth - 1; k >= i && k >= 0; k--) {
        hel* e = &h->stack[k];

        switch (e->kind) {
            case E_INLINE:
                if (e->level == 1 && h->b->para) {  /* the end of a link */
                    pd_inline o;

                    memset(&o, 0, sizeof(o));
                    o.kind = PD_INLINE_LINK;
                    bld_inline(h->b, &o);
                }

                break;

            case E_CELL:
                hi_end_para(h);
                bld_cell_end(h->b);
                break;

            case E_TABLE:
                hi_end_para(h);
                bld_table_end(h->b);
                break;

            case E_FIG:
                hi_end_para(h);
                bld_float_end(h->b);
                break;

            case E_SKIP:
            case E_BARRIER:
            case 0:
                break;

            default:
                hi_end_para(h);
        }

        h->b->cp = e->cp;
    }

    h->depth = i < 0 ? 0 : i;
}

/* close elements down to (and including) the innermost one named name */
static void hi_close(hi* h, const char* name) {
    int32_t i;

    for (i = h->depth - 1; i >= 0 && strcmp(h->stack[i].name, name); i--) {
        if (h->stack[i].kind == E_BARRIER) {
            return;
        }

        if ((h->stack[i].kind == E_TABLE || h->stack[i].kind == E_CELL || h->stack[i].kind == E_FIG) &&
                strcmp(name, "table") && strcmp(name, "td") && strcmp(name, "th") && strcmp(name, "tr") &&
                strcmp(name, "figure")) {
            return;     /* never close across a table cell or figure for an unrelated end tag */
        }
    }

    if (i >= 0) {
        close_from(h, i);
    }
}

static int32_t innermost(const hi* h, int kind) {
    int32_t i;

    for (i = h->depth - 1; i >= 0; i--) {
        if (h->stack[i].kind == kind) {
            return i;
        }

        if (h->stack[i].kind == E_TABLE && kind != E_TABLE) {
            return -1;
        }
    }

    return -1;
}

static void hi_parse(hi* h, const char* s, size_t n, int depth) {
    pd_markup m;
    int32_t skip = 0;           /* nesting depth inside an ignored element */
    char skip_name[64] = "";

    mu_init(&m, s, n, 1);

    while (mu_next(&m) != MT_END) {
        if (skip) {
            if (m.type == MT_OPEN && !strcmp(m.name, skip_name)) {
                skip++;
            } else if (m.type == MT_CLOSE && !strcmp(m.name, skip_name)) {
                skip--;
            }

            continue;
        }

        if (m.type == MT_TEXT) {
            hi_text(h, m.text, m.tlen);
            continue;
        }

        if (m.type == MT_CLOSE) {
            if (!strcmp(m.name, "a") && h->skip_anchor) {
                h->skip_anchor = 0;
                continue;
            }

            if (!strcmp(m.name, "td") || !strcmp(m.name, "th")) {
                if (innermost(h, E_CELL) >= 0) {
                    hi_close(h, h->stack[innermost(h, E_CELL)].name);
                }

                continue;
            }

            hi_close(h, m.name);
            continue;
        }

        /* open or empty tags */
        {
            hel e;
            int level = 0, kind = block_kind(m.name, &level);
            char v[512];

            memset(&e, 0, sizeof(e));
            snprintf(e.name, sizeof(e.name), "%s", m.name);
            e.cp = h->b->cp;
            e.align = -1;
            e.kind = kind;

            if (!strcmp(m.name, "br")) {
                if (!h->b->para) {
                    hi_begin_para(h);
                }

                bld_text(h->b, "\n", 1);
                h->space = 0;
                continue;
            }

            if (!strcmp(m.name, "img")) {
                hi_image(h, &m);
                continue;
            }

            if (!strcmp(m.name, "hr") || !strcmp(m.name, "meta") || !strcmp(m.name, "link") ||
                    !strcmp(m.name, "input") || !strcmp(m.name, "wbr") || !strcmp(m.name, "col") ||
                    !strcmp(m.name, "source") || !strcmp(m.name, "area") || !strcmp(m.name, "base")) {
                continue;
            }

            if (kind == E_SKIP) {
                if (m.type == MT_OPEN) {
                    skip = 1;
                    snprintf(skip_name, sizeof(skip_name), "%s", m.name);
                }

                continue;
            }

            /* Parade's and pandoc's endnotes: read where they are referenced, skipped here */
            if ((kind == E_DIV || kind == E_P) && mu_attr(&m, "class", v, sizeof(v)) && strstr(v, "footnotes")) {
                if (m.type == MT_OPEN) {
                    skip = 1;
                    snprintf(skip_name, sizeof(skip_name), "%s", m.name);
                }

                continue;
            }

            if (kind == E_DIV && mu_attr(&m, "style", v, sizeof(v)) && (strstr(v, "break-after:page") ||
                    strstr(v, "page-break-after: always") || strstr(v, "page-break-after:always"))) {
                hi_end_para(h);
                bld_break(h->b, PD_BREAK_PAGE);
            }

            if (!strcmp(m.name, "a")) {
                const hnote* note;

                if (!mu_attr(&m, "href", v, sizeof(v))) {
                    v[0] = '\0';
                }

                if (strncmp(v, "#fnref", 6) == 0 || ((note = find_note(h, v)) != NULL && depth < 4)) {
                    if (strncmp(v, "#fnref", 6) != 0) {
                        pd_char_props saved = h->b->cp;

                        if (!h->b->para) {
                            hi_begin_para(h);
                        }

                        h->b->cp.mask &= ~PD_CP_SHIFT;     /* the mark is raised by the layout */
                        bld_footnote_begin(h->b);
                        {
                            int32_t d0 = h->depth;
                            int sp = h->space;

                            /* the note's body is parsed apart from the enclosing elements */
                            if (h->depth < 128) {
                                memset(&h->stack[h->depth], 0, sizeof(hel));
                                strcpy(h->stack[h->depth].name, "#note");
                                h->stack[h->depth].kind = E_BARRIER;
                                h->stack[h->depth].align = -1;
                                h->stack[h->depth].cp = h->b->cp;
                                h->depth++;
                            }

                            memset(&h->b->cp, 0, sizeof(h->b->cp));
                            hi_parse(h, h->src + note->start, note->end - note->start, depth + 1);
                            close_from(h, d0);
                            h->space = sp;
                        }
                        bld_footnote_end(h->b);
                        h->b->cp = saved;
                    }

                    if (m.type == MT_OPEN) {
                        h->skip_anchor = 1;
                    }

                    continue;
                }

                if (v[0] && v[0] != '#') {
                    pd_inline o;

                    memset(&o, 0, sizeof(o));
                    o.kind = PD_INLINE_LINK;
                    o.source = v;
                    o.source_len = (int32_t)strlen(v);

                    if (!h->b->para) {
                        hi_begin_para(h);
                    } else if (h->space) {
                        bld_text(h->b, " ", 1);
                        h->space = 0;
                    }

                    bld_inline(h->b, &o);
                    e.kind = E_INLINE;
                    e.level = 1;    /* ends the link on close */
                }

                if (m.type == MT_OPEN && h->depth < 128) {
                    h->stack[h->depth++] = e;
                }

                continue;
            }

            if (!strcmp(m.name, "span") && m.type == MT_OPEN && mu_attr(&m, "class", v, sizeof(v)) &&
                    strstr(v, "math")) {    /* <span class="math">\(..\)</span>: an equation */
                const char* body = s + m.pos, *end = body;
                pd_buf src;
                pd_inline o;

                while (end < s + n && strncmp(end, "</span>", 7) != 0) {
                    end++;
                }

                memset(&src, 0, sizeof(src));
                mu_decode(body, (size_t)(end - body), &src);

                if (src.n >= 4 && src.p[0] == '\\' && (src.p[1] == '(' || src.p[1] == '[')) {
                    memmove(src.p, src.p + 2, src.n - 2);
                    src.n -= 4;     /* and the closing \) */
                }

                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_EQUATION;
                o.source = src.p ? src.p : "";
                o.source_len = (int32_t)src.n;
                o.width = (pd_sp)src.n * PD_PT(5);
                o.height = PD_PT(8);

                if (!h->b->para) {
                    hi_begin_para(h);
                } else if (h->space) {
                    bld_text(h->b, " ", 1);
                    h->space = 0;
                }

                if (src.n) {
                    bld_inline(h->b, &o);
                }

                pb_free(&src);
                m.pos = end < s + n ? (size_t)(end - s) + 7 : n;
                continue;
            }

            if (kind == 0) {    /* inline formatting */
                pd_char_props* cp = &h->b->cp;
                const char* t = m.name;

                if (!strcmp(t, "b") || !strcmp(t, "strong")) {
                    cp->mask |= PD_CP_WEIGHT;
                    cp->weight = 700;
                } else if (!strcmp(t, "i") || !strcmp(t, "em") || !strcmp(t, "cite") || !strcmp(t, "var") ||
                           !strcmp(t, "dfn")) {
                    cp->mask |= PD_CP_ITALIC;
                    cp->italic = 1;
                } else if (!strcmp(t, "u") || !strcmp(t, "ins")) {
                    cp->mask |= PD_CP_UNDERLINE;
                    cp->underline = 1;
                } else if (!strcmp(t, "s") || !strcmp(t, "strike") || !strcmp(t, "del")) {
                    cp->mask |= PD_CP_STRIKE;
                    cp->strike = 1;
                } else if (!strcmp(t, "sup")) {
                    cp->mask |= PD_CP_SHIFT;
                    cp->shift = PD_SHIFT_SUPER;
                } else if (!strcmp(t, "sub")) {
                    cp->mask |= PD_CP_SHIFT;
                    cp->shift = PD_SHIFT_SUB;
                } else if (!strcmp(t, "code") || !strcmp(t, "kbd") || !strcmp(t, "samp") || !strcmp(t, "tt")) {
                    cp->mask |= PD_CP_FAMILY;
                    strcpy(cp->family, "monospace");
                } else if (!strcmp(t, "mark")) {
                    cp->mask |= PD_CP_BACKGROUND;
                    cp->background = 0xFFFFFF00u;
                } else if (!strcmp(t, "font")) {
                    uint32_t c;

                    if (mu_attr(&m, "color", v, sizeof(v)) && css_color(v, &c)) {
                        cp->mask |= PD_CP_COLOR;
                        cp->color = c;
                    }

                    if (mu_attr(&m, "face", v, sizeof(v))) {
                        size_t k = strcspn(v, ",");

                        k = k < sizeof(cp->family) - 1 ? k : sizeof(cp->family) - 1;
                        memcpy(cp->family, v, k);
                        cp->family[k] = '\0';
                        cp->mask |= PD_CP_FAMILY;
                    }
                }

                if (mu_attr(&m, "style", v, sizeof(v))) {
                    apply_style(cp, v);
                }

                e.kind = E_INLINE;

                if (m.type == MT_OPEN && h->depth < 128) {
                    h->stack[h->depth++] = e;
                } else {
                    h->b->cp = e.cp;
                }

                continue;
            }

            /* block elements */
            e.align = align_of(&m);
            e.level = level;

            if (kind == E_H && mu_attr(&m, "class", v, sizeof(v)) && strstr(v, "title")) {
                e.title = 1;
            }

            if (kind == E_P && mu_attr(&m, "class", v, sizeof(v)) && strstr(v, "equation")) {
                e.title = 2;    /* a display equation */
            }

            switch (kind) {
                case E_P:
                    if (innermost(h, E_P) >= 0 && innermost(h, E_P) == h->depth - 1) {
                        hi_close(h, "p");   /* <p> ends an open <p> */
                    }

                    hi_end_para(h);
                    break;

                case E_LI:
                    if (innermost(h, E_LI) >= 0 && innermost(h, E_LI) > innermost(h, E_UL) &&
                            innermost(h, E_LI) > innermost(h, E_OL)) {
                        hi_close(h, "li");
                    }

                    hi_end_para(h);
                    break;

                case E_TABLE:
                    hi_end_para(h);
                    bld_table_begin(h->b);
                    break;

                case E_TR: {
                    int32_t c = innermost(h, E_CELL);

                    if (c >= 0) {
                        hi_close(h, h->stack[c].name);
                    }

                    if (innermost(h, E_TR) >= 0) {
                        hi_close(h, "tr");
                    }

                    bld_row_begin(h->b, innermost(h, E_THEAD) >= 0);
                    break;
                }

                case E_CELL: {
                    int32_t c = innermost(h, E_CELL);
                    uint32_t bg = 0;
                    int span = 1;

                    if (c >= 0) {
                        hi_close(h, h->stack[c].name);
                    }

                    if (innermost(h, E_TR) < 0 && innermost(h, E_TABLE) >= 0) {
                        bld_row_begin(h->b, 0);
                    }

                    if (mu_attr(&m, "colspan", v, sizeof(v))) {
                        span = atoi(v);
                    }

                    if (mu_attr(&m, "style", v, sizeof(v))) {
                        char cv[64];

                        if ((css_prop(v, "background-color", cv, sizeof(cv)) || css_prop(v, "background", cv, sizeof(cv))) &&
                                !css_color(cv, &bg)) {
                            bg = 0;
                        }
                    } else if (mu_attr(&m, "bgcolor", v, sizeof(v)) && !css_color(v, &bg)) {
                        bg = 0;
                    }

                    if (innermost(h, E_TABLE) < 0) {    /* a stray cell: just a paragraph */
                        e.kind = E_P;
                        hi_end_para(h);
                        break;
                    }

                    hi_end_para(h);
                    bld_cell_begin(h->b, span, bg);

                    if (!strcmp(m.name, "th") && innermost(h, E_THEAD) < 0) {
                        int32_t t = h->b->ntables - 1;

                        if (t >= 0 && t < 8 && h->b->ncells[t] == 1 && h->b->header_rows[t] == h->b->nrows[t] - 1) {
                            h->b->header_rows[t]++;     /* a first row of <th> is a header row */
                        }
                    }

                    break;
                }

                case E_FIG:
                    hi_end_para(h);
                    bld_float_begin(h->b);
                    break;

                default:
                    hi_end_para(h);
            }

            if (m.type == MT_OPEN && h->depth < 128) {
                h->stack[h->depth++] = e;
            } else if (m.type == MT_EMPTY) {
                hi_close(h, e.name);
            }
        }
    }
}

/* footnote bodies by id: <li id=...> inside an element whose class mentions footnotes */
static void find_notes(hi* h, const char* s, size_t n) {
    pd_markup m;
    int in_notes = 0, depth = 0, li_depth = 0;
    char v[128];
    hnote cur;
    int32_t cap = 0;

    memset(&cur, 0, sizeof(cur));
    mu_init(&m, s, n, 1);

    while (mu_next(&m) != MT_END) {
        if (m.type == MT_OPEN && !in_notes && mu_attr(&m, "class", v, sizeof(v)) && strstr(v, "footnotes")) {
            in_notes = 1;
            depth = 0;
            continue;
        }

        if (!in_notes) {
            continue;
        }

        if (m.type == MT_OPEN) {
            depth++;

            if (!strcmp(m.name, "li") && mu_attr(&m, "id", v, sizeof(v))) {
                snprintf(cur.id, sizeof(cur.id), "%s", v);
                cur.start = m.pos;
                li_depth = depth;
            }
        } else if (m.type == MT_CLOSE) {
            if (!strcmp(m.name, "li") && li_depth == depth && cur.id[0]) {
                cur.end = m.pos - 5;

                if (!pd_grow((void**)&h->notes, &cap, (int64_t)h->nnotes + 1, sizeof(hnote))) {
                    h->notes[h->nnotes++] = cur;
                }

                cur.id[0] = '\0';
            }

            if (--depth < 0) {
                in_notes = 0;
            }
        }
    }
}

pd_status pd_html_import(pd_doc* d, const char* s, size_t n) {
    pd_bld b;
    hi h;
    const char* frag;

    /* Windows clipboard HTML: a header of offsets, then the page */
    if (n > 8 && memcmp(s, "Version:", 8) == 0 && (frag = strstr(s, "<")) != NULL) {
        n -= (size_t)(frag - s);
        s = frag;
    }

    bld_init(&b, d);
    memset(&h, 0, sizeof(h));
    h.b = &b;
    h.src = s;
    find_notes(&h, s, n);
    hi_parse(&h, s, n, 0);
    close_from(&h, 0);

    while (b.ntables > 0) {
        bld_table_end(&b);
    }

    free(h.notes);
    return bld_finish(&b);
}
