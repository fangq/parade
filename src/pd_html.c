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
    char ruby[8][256];          /* phonetic guides open: their text, written after the base */
    int nruby;
    int quote_open, code_open;  /* quote_open: <blockquote>s open */
    int code_multi;             /* the open <pre> holds a block of several lines */
    char code_lang[32];
    int dl_open;
    char div[32];               /* the <div class> open */
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

static void close_code(hx* x) {
    if (x->code_open) {
        pb_puts(x->o, "</code></pre>\n");
        x->code_open = 0;
    }
}

static void close_dl(hx* x) {
    if (x->dl_open) {
        pb_puts(x->o, "</dl>\n");
        x->dl_open = 0;
    }
}

/* to a depth of block quotes, lists and code closed first when quotes change */
static void quotes_to(hx* x, int q) {
    if (q == x->quote_open) {
        return;
    }

    close_code(x);
    close_dl(x);
    close_lists(x, 0);

    while (x->quote_open > q) {
        pb_puts(x->o, "</blockquote>\n");
        x->quote_open--;
    }

    while (x->quote_open < q) {
        pb_puts(x->o, "<blockquote>\n");
        x->quote_open++;
    }
}

/* the named block a paragraph is in: <div class="name">, an alert GitHub's markdown-alert */
static void div_to(hx* x, const char* div) {
    if (strcmp(div, x->div) == 0) {
        return;
    }

    close_code(x);
    close_dl(x);
    quotes_to(x, 0);
    close_lists(x, 0);

    if (x->div[0]) {
        pb_puts(x->o, "</div>\n");
    }

    if (div[0] == '!') {
        pb_puts(x->o, "<div class=\"markdown-alert markdown-alert-");
        esc(x->o, div + 1, strlen(div + 1), 1);
        pb_puts(x->o, "\">\n");
    } else if (div[0]) {
        pb_puts(x->o, "<div class=\"");
        esc(x->o, div, strlen(div), 1);
        pb_puts(x->o, "\">\n");
    }

    snprintf(x->div, sizeof(x->div), "%s", div);
}

static void close_groups(hx* x) {
    close_code(x);
    close_dl(x);
    quotes_to(x, 0);
    div_to(x, "");
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

                if (ob->source_len > 0 || pd_doc_resource(x->d, ob->resource, &mime, &data, &len) == PD_OK) {
                    pb_puts(o, "<img src=\"");

                    if (ob->source_len > 0) {   /* by address, as the document gave it */
                        esc(o, ob->source, (size_t)ob->source_len, 1);
                    } else {
                        pb_puts(o, "data:");
                        esc(o, mime, strlen(mime), 1);
                        pb_puts(o, ";base64,");
                        pb_base64(o, (const unsigned char*)data, len);
                    }

                    pb_putc(o, '"');

                    if (ob->width > 0 && ob->height > 0 && (ob->source_len == 0 || ob->level == 1)) {
                        pb_printf(o, " width=\"%d\" height=\"%d\"", (int)(ob->width / 65536 * 4 / 3),
                                  (int)(ob->height / 65536 * 4 / 3));
                    }

                    pb_puts(o, " alt=\"");
                    esc(o, ob->alt ? ob->alt : "", (size_t)ob->alt_len, 1);
                    pb_putc(o, '"');

                    if (ob->title_len > 0) {
                        pb_puts(o, " title=\"");
                        esc(o, ob->title, (size_t)ob->title_len, 1);
                        pb_putc(o, '"');
                    }

                    pb_putc(o, '>');
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

                    if (ob->title_len > 0) {
                        pb_puts(o, "\" title=\"");
                        esc(o, ob->title, (size_t)ob->title_len, 1);
                    }

                    pb_puts(o, "\">");
                    x->in_link = 1;
                }

                break;

            case PD_INLINE_RAW:     /* markup the document carried: through as it is */
                if (ob->source) {
                    pb_put(o, ob->source, (size_t)ob->source_len);
                }

                break;

            case PD_INLINE_BOOKMARK:
                pb_printf(o, "<a id=\"%s\"></a>", ob->name);
                break;

            case PD_INLINE_RUBY:    /* <ruby>base<rt>guide</rt></ruby> */
                if (ob->source && ob->source_len > 0 && x->nruby < 8) {
                    snprintf(x->ruby[x->nruby++], sizeof(x->ruby[0]), "%.*s", ob->source_len < 255 ? (int)ob->source_len :
                             255, ob->source);
                    pb_puts(o, "<ruby>");
                } else if (!ob->source_len && x->nruby > 0) {
                    x->nruby--;
                    pb_puts(o, "<rt>");
                    esc(o, x->ruby[x->nruby], strlen(x->ruby[x->nruby]), 0);
                    pb_puts(o, "</rt></ruby>");
                }

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
                   (!mono && strcmp(c->family, b->family) && !pd_conv_is_mono(c)) || c->caps != b->caps ||
                   c->small_caps != b->small_caps || c->hidden != b->hidden || c->letter_space != b->letter_space ||
                   c->position != b->position || (c->underline > 1 && c->underline != b->underline);
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

            if (c->caps != b->caps) {
                pb_puts(o, c->caps ? "text-transform:uppercase;" : "text-transform:none;");
            }

            if (c->small_caps != b->small_caps) {
                pb_puts(o, c->small_caps ? "font-variant:small-caps;" : "font-variant:normal;");
            }

            if (c->hidden != b->hidden) {
                pb_puts(o, c->hidden ? "display:none;" : "display:inline;");
            }

            if (c->letter_space != b->letter_space) {
                pb_printf(o, "letter-spacing:%gpt;", c->letter_space / 65536.0);
            }

            if (c->position != b->position) {
                pb_printf(o, "vertical-align:%gpt;", c->position / 65536.0);
            }

            if (c->underline > 1 && c->underline != b->underline) {
                static const char* deco[] = { "", "", "double", "solid", "dotted", "dashed", "wavy", "solid" };

                pb_printf(o, "text-decoration-style:%s;", deco[c->underline <= PD_UNDERLINE_WORDS ? c->underline : 1]);

                if (c->underline == PD_UNDERLINE_THICK) {
                    pb_puts(o, "text-decoration-thickness:2px;");
                }

                if (c->underline == PD_UNDERLINE_WORDS) {
                    pb_puts(o, "text-decoration-skip:spaces;");
                }
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
    x->nruby = 0;
    pd_conv_base_props(x->d, p, &x->base);
    pd_conv_spans(x->d, p, hx_span, x);

    if (x->in_link) {
        pb_puts(x->o, "</a>");
        x->in_link = 0;
    }

    while (x->nruby > 0) {      /* a guide the paragraph does not end */
        x->nruby--;
        pb_puts(x->o, "<rt>");
        esc(x->o, x->ruby[x->nruby], strlen(x->ruby[x->nruby]), 0);
        pb_puts(x->o, "</rt></ruby>");
    }
}

static void hx_block(hx* x, pd_block_id id, int in_figure);

static void hx_para(hx* x, pd_block_id p, int in_figure) {
    pd_block_info bi;
    pd_para_props pp;
    int32_t level = 0;
    int kind = pd_conv_list_kind(x->d, p, &level), task = 0, cont = 0, loose = 0, rtl;
    const char* align = "";
    char lang[32];
    const char* ct;
    uint32_t cn;

    pd_doc_block_info(x->d, p, &bi);
    pd_doc_style_resolve(x->d, bi.style, &pp, NULL);

    {
        pd_para_props dp;

        if (pd_doc_para_props(x->d, p, &dp) == PD_OK && (dp.mask & PD_PP_ALIGN)) {
            pp.align = dp.align;
        }
    }

    rtl = pd_doc_para_rtl(x->d, p) == 1;    /* right to left: its end on the left */
    align = pp.align == PD_ALIGN_CENTER ? (rtl ? " dir=\"rtl\" style=\"text-align:center\"" :
                                           " style=\"text-align:center\"") :
            pp.align == PD_ALIGN_RIGHT ? (rtl ? " dir=\"rtl\" style=\"text-align:left\"" :
                                          " style=\"text-align:right\"") :
            rtl ? " dir=\"rtl\"" : "";

    {
        pd_para_attrs at;

        memset(&at, 0, sizeof(at));
        pd_doc_para_attrs(x->d, p, &at);
        div_to(x, at.div_class);
        quotes_to(x, at.quote_depth > 0 ? at.quote_depth : bi.role == PD_ROLE_QUOTE ? 1 : 0);
        task = at.task;
        cont = at.cont;
        loose = at.loose;
        snprintf(lang, sizeof(lang), "%s", at.lang);
    }

    if (bi.role != PD_ROLE_CODE) {
        close_code(x);
    }

    if (bi.role != PD_ROLE_TERM && bi.role != PD_ROLE_DEFINITION) {
        close_dl(x);
    }

    /* a later block of the item open at its level: inside it */
    if (cont && pd_conv_item_level(x->d, p, &level) && x->ldepth == level + 1) {
        close_lists(x, level + 1);

        if (bi.role == PD_ROLE_CODE) {
            pd_doc_para_text(x->d, p, &ct, &cn);
            pb_puts(x->o, "\n<pre><code");

            if (lang[0]) {
                pb_puts(x->o, " class=\"language-");
                esc(x->o, lang, strlen(lang), 1);
                pb_putc(x->o, '"');
            }

            pb_putc(x->o, '>');
            esc(x->o, ct, cn, 0);
            pb_puts(x->o, "</code></pre>");
        } else {
            pb_printf(x->o, "\n<p%s>", align);
            hx_inline(x, p);
            pb_puts(x->o, "</p>");
        }

        return;
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
            pd_list_level lv[9];
            int32_t nlv = 0;

            x->lkind[x->ldepth] = x->ldepth == level ? kind : 1;

            if (x->lkind[x->ldepth] == 2 && x->ldepth == level && pd_doc_list_info(x->d, bi.list, &nlv, lv) == PD_OK &&
                    level < nlv && lv[level].start != 1) {
                pb_printf(x->o, "<ol start=\"%d\">\n", (int)lv[level].start);
            } else {
                pb_puts(x->o, x->lkind[x->ldepth] == 2 ? "<ol>\n" : "<ul>\n");
            }

            if (x->ldepth < level) {
                pb_puts(x->o, "<li>");
            }

            x->ldepth++;
        }

        pb_printf(x->o, "<li%s>", align);

        if (task) {
            pb_puts(x->o, task == 2 ? "<input type=\"checkbox\" disabled checked> " :
                    "<input type=\"checkbox\" disabled> ");
        }

        if (bi.role == PD_ROLE_HEADING) {   /* a heading as an item */
            pb_printf(x->o, "<h%d>", (int)bi.level);
            hx_inline(x, p);
            pb_printf(x->o, "</h%d>", (int)bi.level);
            return;
        }

        if (loose) {    /* a loose list's item holds a paragraph, as CommonMark writes it */
            pb_puts(x->o, "<p>");
            hx_inline(x, p);
            pb_puts(x->o, "</p>");
            return;
        }

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
            pb_printf(x->o, "<p%s>", align);
            hx_inline(x, p);
            pb_puts(x->o, "</p>\n");
            return;

        case PD_ROLE_RAW: {     /* markup the document carried: through as it is */
            const char* t;
            uint32_t n;

            pd_doc_para_text(x->d, p, &t, &n);
            pb_put(x->o, t, n);
            pb_putc(x->o, '\n');
            return;
        }

        case PD_ROLE_TERM:
        case PD_ROLE_DEFINITION:
            if (!x->dl_open) {
                pb_puts(x->o, "<dl>\n");
                x->dl_open = 1;
            }

            pb_puts(x->o, bi.role == PD_ROLE_TERM ? "<dt>" : "<dd>");
            hx_inline(x, p);
            pb_puts(x->o, bi.role == PD_ROLE_TERM ? "</dt>\n" : "</dd>\n");
            return;

        case PD_ROLE_EQUATION:
            pb_puts(x->o, "<p class=\"equation\">");
            hx_inline(x, p);
            pb_puts(x->o, "</p>\n");
            return;

        case PD_ROLE_CODE: {
            const char* t;
            uint32_t n;
            int multi;

            pd_doc_para_text(x->d, p, &t, &n);
            multi = memchr(t, '\n', n) != NULL;

            /* one <pre> per code block; code that came a line to a paragraph is run together */
            if (x->code_open && (multi || x->code_multi || strcmp(lang, x->code_lang) != 0)) {
                close_code(x);
            }

            if (x->code_open) {
                pb_putc(x->o, '\n');
            } else if (lang[0]) {
                pb_puts(x->o, "<pre><code class=\"language-");
                esc(x->o, lang, strlen(lang), 1);
                pb_puts(x->o, "\">");
            } else {
                pb_puts(x->o, "<pre><code>");
            }

            x->code_open = 1;
            x->code_multi = multi;
            snprintf(x->code_lang, sizeof(x->code_lang), "%s", lang);
            x->in_code = 1;
            hx_inline(x, p);
            x->in_code = 0;
            return;
        }

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

/* whether the cell of a row starting at grid column col continues the one above */
static int hx_merges_at(const hx* x, pd_block_id row, int32_t col) {
    pd_block_info ri;
    int32_t c, at = 0;

    pd_doc_block_info(x->d, row, &ri);

    for (c = 0; c < ri.child_count && at <= col; c++) {
        pd_cell_props cp;

        pd_doc_cell_resolve(x->d, pd_doc_child(x->d, row, c), &cp);

        if (at == col) {
            return cp.merge_up;
        }

        at += cp.col_span;
    }

    return 0;
}

static void hx_table(hx* x, pd_block_id t) {
    pd_block_info ti;
    pd_table_props tp;
    int32_t r, c, col;

    pd_doc_block_info(x->d, t, &ti);
    pd_doc_table_resolve(x->d, t, &tp);
    pb_printf(x->o, "<table%s%s>\n", tp.border ? " class=\"grid\"" : "", tp.direction == PD_DIR_RTL ? " dir=\"rtl\"" : "");

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
        col = 0;

        for (c = 0; c < ri.child_count; c++) {
            pd_block_id cell = pd_doc_child(x->d, row, c);
            pd_cell_props cp;
            int32_t down = 1;

            pd_doc_cell_resolve(x->d, cell, &cp);

            if (cp.merge_up) {  /* covered by the rowspan of the cell above */
                col += cp.col_span;
                continue;
            }

            while (r + down < ti.child_count && hx_merges_at(x, pd_doc_child(x->d, t, r + down), col)) {
                down++;
            }

            col += cp.col_span;
            pb_puts(x->o, head ? "<th" : "<td");

            if (cp.col_span > 1) {
                pb_printf(x->o, " colspan=\"%d\"", (int)cp.col_span);
            }

            if (down > 1) {
                pb_printf(x->o, " rowspan=\"%d\"", (int)down);
            }

            {   /* the cell's alignment is its paragraph's */
                pd_para_props cpp;

                if (pd_doc_para_props(x->d, pd_doc_child(x->d, cell, 0), &cpp) == PD_OK && (cpp.mask & PD_PP_ALIGN) &&
                        cpp.align != PD_ALIGN_JUSTIFY) {
                    pb_puts(x->o, cpp.align == PD_ALIGN_CENTER ? " align=\"center\"" : cpp.align == PD_ALIGN_RIGHT ?
                            " align=\"right\"" : " align=\"left\"");
                }
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
            close_groups(x);
            pb_puts(x->o, bi.break_kind == PD_BREAK_RULE ? "<hr>\n" :
                    "<div class=\"page-break\" style=\"break-after:page\"></div>\n");
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
    int align;                  /* -1 = none; ALIGN_PHYS_LEFT, _RIGHT: CSS's sides, made the text's start or end */
    int dir;                    /* pd_direction from dir= (or CSS direction), -1 none */
    pd_char_props cp;           /* character state to restore on close */
    int title;                  /* E_P: 2 an equation, 3 a <dt>, 4 a <dd> */
    pd_list_id list;            /* E_UL/E_OL: its list, made with its first item */
    int start;                  /* E_OL: <ol start> */
    int task;                   /* E_LI: a checkbox opened it, 1 empty, 2 checked */
    int labelled;               /* E_LI: its first paragraph, which carries the label, is made */
    char lang[32];              /* E_PRE: <code class="language-...">; E_DIV: its class, "!note" for an alert */
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
    /* rowspan: per table being built, the rows each grid column is still covered for */
    int rs_left[8][PD_TABLE_MAX_COLS], rs_span[8][PD_TABLE_MAX_COLS], rs_col[8];
} hi;

/* The cells a rowspan above covers, as cells continuing it: those at the
   current column, or (at the end of a row) every one still owed. */
static void hi_merged_cells(hi* h, int to_end) {
    int t = h->b->ntables - 1;

    if (t < 0 || t >= 8) {
        return;
    }

    while (h->rs_col[t] < PD_TABLE_MAX_COLS) {
        int c = h->rs_col[t];

        if (h->rs_left[t][c] > 0) {
            bld_cell_begin(h->b, h->rs_span[t][c], 0);
            bld_cell_merge_up(h->b);
            bld_cell_end(h->b);
            h->rs_left[t][c]--;
            h->rs_col[t] += h->rs_span[t][c] > 0 ? h->rs_span[t][c] : 1;
        } else if (to_end) {
            h->rs_col[t]++;
        } else {
            break;
        }
    }
}

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

    x = x < -20000 ? -20000 : x > 20000 ? 20000 : x;    /* points, in what a pd_sp holds */
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

    if (css_prop(style, "text-decoration-style", v, sizeof(v))) {
        int u = strstr(v, "double") ? PD_UNDERLINE_DOUBLE : strstr(v, "dotted") ? PD_UNDERLINE_DOTTED :
                strstr(v, "dashed") ? PD_UNDERLINE_DASHED : strstr(v, "wavy") ? PD_UNDERLINE_WAVY : 0;

        if (u && ((cp->mask & PD_CP_UNDERLINE) ? cp->underline : 1)) {
            cp->mask |= PD_CP_UNDERLINE;
            cp->underline = u;
        }
    }

    if (css_prop(style, "text-decoration-skip", v, sizeof(v)) && strstr(v, "spaces") && (cp->mask & PD_CP_UNDERLINE) &&
            cp->underline == PD_UNDERLINE_SINGLE) {
        cp->underline = PD_UNDERLINE_WORDS;
    }

    if (css_prop(style, "text-decoration-thickness", v, sizeof(v)) && (cp->mask & PD_CP_UNDERLINE) &&
            cp->underline == PD_UNDERLINE_SINGLE) {
        cp->underline = PD_UNDERLINE_THICK;
    }

    if (css_prop(style, "text-transform", v, sizeof(v))) {
        cp->mask |= PD_CP_CAPS;
        cp->caps = strstr(v, "uppercase") != NULL;
    }

    if (css_prop(style, "font-variant", v, sizeof(v)) || css_prop(style, "font-variant-caps", v, sizeof(v))) {
        cp->mask |= PD_CP_SMALLCAPS;
        cp->small_caps = strstr(v, "small-caps") != NULL;
    }

    if (css_prop(style, "display", v, sizeof(v))) {
        cp->mask |= PD_CP_HIDDEN;
        cp->hidden = strstr(v, "none") != NULL;
    }

    if (css_prop(style, "letter-spacing", v, sizeof(v)) && (isdigit((unsigned char)v[0]) || v[0] == '-' || v[0] == '.')) {
        cp->mask |= PD_CP_LETTERSPACE;
        cp->letter_space = css_length(v);
    }

    if (css_prop(style, "vertical-align", v, sizeof(v)) && (isdigit((unsigned char)v[0]) || v[0] == '-' ||
            v[0] == '.')) {
        cp->mask |= PD_CP_POSITION;
        cp->position = css_length(v);
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

#define ALIGN_PHYS_LEFT 10      /* text-align: left, right -- sides of the page, not of the text */
#define ALIGN_PHYS_RIGHT 11

static int align_of(const pd_markup* m) {
    char v[64], st[512];

    if ((mu_attr(m, "style", st, sizeof(st)) && css_prop(st, "text-align", v, sizeof(v))) ||
            mu_attr(m, "align", v, sizeof(v))) {
        return strstr(v, "center") ? PD_ALIGN_CENTER : strstr(v, "justify") ? PD_ALIGN_JUSTIFY : strstr(v, "end") ?
               PD_ALIGN_RIGHT : strstr(v, "start") ? PD_ALIGN_LEFT : strstr(v, "right") ? ALIGN_PHYS_RIGHT :
               ALIGN_PHYS_LEFT;
    }

    return -1;
}

/* an element's direction: dir=, or CSS direction; -1 none */
static int dir_of(const pd_markup* m) {
    char v[64], st[512];

    if (mu_attr(m, "dir", v, sizeof(v)) || (mu_attr(m, "style", st, sizeof(st)) && css_prop(st, "direction", v,
                                            sizeof(v)))) {
        return strstr(v, "rtl") ? PD_DIR_RTL : strstr(v, "ltr") ? PD_DIR_LTR : PD_DIR_AUTO;
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

    if (!strcmp(n, "dl")) {
        return E_DIV;
    }

    if (!strcmp(n, "li")) {
        return E_LI;
    }

    if (!strcmp(n, "ul") || !strcmp(n, "menu")) {
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
    int32_t i, lists = 0, lkind = 1, align = -1, dir = -1, quotes = 0, inner_at = -1, item_at = -1;
    hel* inner = NULL, *list = NULL, *item = NULL;
    pd_para_attrs at;
    pd_block_id p;

    memset(&at, 0, sizeof(at));

    for (i = h->depth - 1; i >= 0; i--) {
        hel* e = &h->stack[i];

        if (!inner && (e->kind == E_H || e->kind == E_PRE || e->kind == E_QUOTE || e->kind == E_LI || e->kind == E_CAP ||
                       (e->kind == E_P && e->title >= 2))) {
            inner = e;
        }

        if (inner_at < 0 && e->kind == E_P) {
            inner_at = i;   /* the <p> it is in */
        }

        if (align < 0 && e->align >= 0) {
            align = e->align;
        }

        if (dir < 0 && e->dir >= 0) {
            dir = e->dir;
        }

        if (e->kind == E_LI && !item && lists == 0) {
            item = e;
            item_at = i;
        }

        if (e->kind == E_UL || e->kind == E_OL) {
            if (lists == 0) {
                lkind = e->kind == E_OL ? 2 : 1;
                list = e;
            }

            lists++;
        }

        quotes += e->kind == E_QUOTE;

        if (e->kind == E_CELL || e->kind == E_FIG || e->kind == E_BARRIER) {
            break;
        }
    }

    at.quote_depth = quotes < 9 ? quotes : 9;

    for (i = h->depth - 1; i >= 0 && !at.div_class[0]; i--) {   /* the innermost named <div> */
        if (h->stack[i].kind == E_BARRIER || h->stack[i].kind == E_CELL || h->stack[i].kind == E_FIG) {
            break;
        }

        if (h->stack[i].kind == E_DIV && h->stack[i].lang[0]) {
            snprintf(at.div_class, sizeof(at.div_class), "%s", h->stack[i].lang);
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
        snprintf(at.lang, sizeof(at.lang), "%s", inner->lang);
    } else if (inner && inner->kind == E_P && inner->title == 3) {
        bld_para_style(b, "Term", PD_ROLE_TERM, 0);
    } else if (inner && inner->kind == E_P && inner->title == 4) {
        bld_para_style(b, "Definition", PD_ROLE_DEFINITION, 0);
    } else if (inner && inner->kind == E_QUOTE) {
        bld_para_style(b, "Quote", PD_ROLE_QUOTE, 0);
    } else if (inner && inner->kind == E_CAP) {
        bld_para_style(b, "Caption", PD_ROLE_CAPTION, 0);
    } else if (inner && inner->kind == E_P && inner->title == 2) {
        bld_para_style(b, NULL, PD_ROLE_EQUATION, 0);
    }

    if (lists > 0) {
        int32_t lv = lists - 1 < 8 ? lists - 1 : 8;

        bld_list(b, lkind, lv);

        if (list && !list->list) {  /* each <ul>/<ol> a list of its own, from its start */
            list->list = bld_list_new(b, lkind, lv, list->start > 0 ? list->start : 1);
        }

        b->list_id = list ? list->list : 0;
    }

    if (item && inner == item) {
        at.task = item->task;
        item->task = 0;     /* the item's first paragraph has it */
    }

    if (item && lists > 0) {    /* a later paragraph of the item: in it, without a label */
        at.cont = item->labelled;

        if (!item->labelled && inner_at == item_at + 1) {
            at.loose = 1;   /* <li><p>: an item of a loose list */
        }

        item->labelled = 1;
    }

    if (dir >= 0) {
        b->pp.mask |= PD_PP_DIRECTION;
        b->pp.direction = dir;
    }

    if (align >= 0) {   /* (CSS's left and right: the start and end of left-to-right text, the other way round) */
        int rtl = dir == PD_DIR_RTL;

        b->pp.mask |= PD_PP_ALIGN;
        b->pp.align = align == ALIGN_PHYS_LEFT ? (rtl ? PD_ALIGN_RIGHT : PD_ALIGN_LEFT) :
                      align == ALIGN_PHYS_RIGHT ? (rtl ? PD_ALIGN_LEFT : PD_ALIGN_RIGHT) : align;
    }

    p = bld_begin_para(b);

    if (p && (at.quote_depth || at.task || at.lang[0] || at.cont || at.div_class[0] || at.loose)) {
        pd_doc_set_para_attrs(b->d, p, &at);
    }

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

static int strncasecmp_n(const char* a, const char* b, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }

    return 1;
}

/* a boolean attribute, which may stand without a value: <input checked> */
static int attr_flag(const pd_markup* m, const char* name) {
    size_t i, k = strlen(name);

    for (i = 0; m->attrs && i + k <= m->alen; i++) {
        if ((i == 0 || isspace((unsigned char)m->attrs[i - 1])) && strncasecmp_n(m->attrs + i, name, k) &&
                (i + k == m->alen || !isalnum((unsigned char)m->attrs[i + k]))) {
            return 1;
        }
    }

    return 0;
}

/* an HTML width/height attribute: pixels unless it says pt */
static pd_sp attr_length(const pd_markup* m, const char* name, pd_sp dflt) {
    char v[32];
    double x;

    if (!mu_attr(m, name, v, sizeof(v)) || strchr(v, '%') || (x = atof(v)) <= 0) {
        return dflt;
    }

    x *= strstr(v, "pt") ? 1.0 : 0.75;
    return (pd_sp)((x < 20000 ? x : 20000) * 65536);    /* no picture wider than about 7 m */
}

static void hi_image(hi* h, const pd_markup* m) {
    char* src = (char*)malloc(m->alen + 1);
    pd_inline o;

    if (!src) {
        return;
    }

    src[0] = '\0';

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
    } else if (src[0] || mu_attr(m, "src", src, m->alen + 1)) {   /* by address: loaded later, if at all */
        char* alt = (char*)malloc(m->alen + 1), *title = (char*)malloc(m->alen + 1);

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_IMAGE;
        o.source = src;
        o.source_len = (int32_t)strlen(src);
        o.width = attr_length(m, "width", 0);
        o.height = attr_length(m, "height", 0);
        o.level = o.width > 0 || o.height > 0;

        if (alt && mu_attr(m, "alt", alt, m->alen + 1)) {
            o.alt = alt;
            o.alt_len = (int32_t)strlen(alt);
        }

        if (title && mu_attr(m, "title", title, m->alen + 1)) {
            o.title = title;
            o.title_len = (int32_t)strlen(title);
        }

        if (o.source_len > 0) {
            if (!h->b->para) {
                hi_begin_para(h);
            }

            bld_inline(h->b, &o);
        }

        free(alt);
        free(title);
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
                hi_merged_cells(h, 1);

                if (e->dir == PD_DIR_RTL && h->b->ntables >= 1 && h->b->ntables <= 8) {    /* columns from the right */
                    pd_table_props tp;

                    if (pd_doc_table_props(h->b->d, h->b->table[h->b->ntables - 1], &tp) == PD_OK) {
                        tp.direction = PD_DIR_RTL;
                        pd_doc_set_table_props(h->b->d, h->b->table[h->b->ntables - 1], &tp);
                    }
                }

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
            e.dir = -1;
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

            if (!strcmp(m.name, "hr")) {    /* a rule across the column */
                hi_end_para(h);
                bld_break(h->b, PD_BREAK_RULE);
                continue;
            }

            if (!strcmp(m.name, "input") && mu_attr(&m, "type", v, sizeof(v)) && !strcmp(v, "checkbox")) {
                int32_t q;

                for (q = h->depth - 1; q >= 0 && h->stack[q].kind != E_LI; q--) {
                }

                if (q >= 0 && !h->b->para) {    /* a task list item */
                    h->stack[q].task = attr_flag(&m, "checked") ? 2 : 1;
                }

                continue;
            }

            if (!strcmp(m.name, "meta") || !strcmp(m.name, "link") ||
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

                if ((v[0] == '#' && strncmp(v + 1, "fnref", 5) == 0) || ((note = find_note(h, v)) != NULL && depth < 4)) {
                    if (!(v[0] == '#' && strncmp(v + 1, "fnref", 5) == 0)) {
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
                                h->stack[h->depth].dir = -1;
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

                {
                    char id[64];

                    if (!v[0] && (mu_attr(&m, "id", id, sizeof(id)) || mu_attr(&m, "name", id, sizeof(id))) && id[0]) {
                        pd_inline o;    /* an anchor: a bookmark */

                        memset(&o, 0, sizeof(o));
                        o.kind = PD_INLINE_BOOKMARK;
                        snprintf(o.name, sizeof(o.name), "%.31s", id);

                        if (!h->b->para) {
                            hi_begin_para(h);
                        }

                        bld_inline(h->b, &o);
                    }
                }

                if (v[0] && v[0] != '#') {
                    pd_inline o;
                    char ttl[256];

                    memset(&o, 0, sizeof(o));
                    o.kind = PD_INLINE_LINK;
                    o.source = v;
                    o.source_len = (int32_t)strlen(v);

                    if (mu_attr(&m, "title", ttl, sizeof(ttl)) && ttl[0]) {
                        o.title = ttl;
                        o.title_len = (int32_t)strlen(ttl);
                    }

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

                while (end < s + n && (s + n - end < 7 || memcmp(end, "</span>", 7) != 0)) {
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
                o.width = pd_conv_equation_width(src.n);
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

                if (!strcmp(t, "code") && m.type == MT_OPEN && mu_attr(&m, "class", v, sizeof(v))) {
                    const char* lg = strstr(v, "language-");
                    int32_t q = innermost(h, E_PRE);

                    if (lg && q >= 0 && !h->b->para) {   /* <pre><code class="language-x">: the block's language */
                        snprintf(h->stack[q].lang, sizeof(h->stack[q].lang), "%.*s", (int)strcspn(lg + 9, " \t"), lg + 9);
                    }
                }

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
            e.dir = dir_of(&m);
            e.level = level;

            if (kind == E_H && mu_attr(&m, "class", v, sizeof(v)) && strstr(v, "title")) {
                e.title = 1;
            }

            if (kind == E_P && mu_attr(&m, "class", v, sizeof(v)) && strstr(v, "equation")) {
                e.title = 2;    /* a display equation */
            }

            if (kind == E_P && (!strcmp(m.name, "dt") || !strcmp(m.name, "dd"))) {
                e.title = m.name[1] == 't' ? 3 : 4;     /* a definition list's term, its definition */
            }

            if (kind == E_OL && mu_attr(&m, "start", v, sizeof(v))) {
                e.start = atoi(v);
            }

            if (kind == E_DIV && !strcmp(m.name, "div") && mu_attr(&m, "class", v, sizeof(v)) && v[0] &&
                    !strstr(v, "page-break") && !strstr(v, "footnotes")) {
                const char* al = strstr(v, "markdown-alert-");

                if (al) {   /* GitHub's alert */
                    snprintf(e.lang, sizeof(e.lang), "!%.*s", (int)strcspn(al + 15, " \t"), al + 15);
                } else if (!strstr(v, "markdown-alert")) {
                    snprintf(e.lang, sizeof(e.lang), "%.*s", (int)strcspn(v, " \t"), v);
                }
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

                    if (h->b->ntables >= 1 && h->b->ntables <= 8) {
                        int t = h->b->ntables - 1;

                        memset(h->rs_left[t], 0, sizeof(h->rs_left[t]));
                        h->rs_col[t] = 0;
                    }

                    break;

                case E_TR: {
                    int32_t c = innermost(h, E_CELL);

                    if (c >= 0) {
                        hi_close(h, h->stack[c].name);
                    }

                    if (innermost(h, E_TR) >= 0) {
                        hi_close(h, "tr");
                    }

                    if (h->b->ntables >= 1 && h->b->ntables <= 8 && h->b->nrows[h->b->ntables - 1] > 0) {
                        hi_merged_cells(h, 1);  /* the row before ends with what its rowspans owe */
                    }

                    if (h->b->ntables >= 1 && h->b->ntables <= 8) {
                        h->rs_col[h->b->ntables - 1] = 0;
                    }

                    bld_row_begin(h->b, innermost(h, E_THEAD) >= 0);
                    break;
                }

                case E_CELL: {
                    int32_t c = innermost(h, E_CELL);
                    uint32_t bg = 0;
                    int span = 1, rows = 1;

                    if (c >= 0) {
                        hi_close(h, h->stack[c].name);
                    }

                    if (innermost(h, E_TR) < 0 && innermost(h, E_TABLE) >= 0) {
                        bld_row_begin(h->b, 0);
                    }

                    if (mu_attr(&m, "colspan", v, sizeof(v))) {
                        span = atoi(v);
                    }

                    if (mu_attr(&m, "rowspan", v, sizeof(v))) {
                        rows = atoi(v);
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

                    if (innermost(h, E_TR) < 0 && h->b->ntables >= 1 && h->b->ntables <= 8 &&
                            h->b->nrows[h->b->ntables - 1] == 0) {
                        h->rs_col[h->b->ntables - 1] = 0;   /* a row begun by its first cell */
                    }

                    hi_merged_cells(h, 0);
                    bld_cell_begin(h->b, span, bg);

                    if (h->b->ntables >= 1 && h->b->ntables <= 8) {
                        int t = h->b->ntables - 1, c0 = h->rs_col[t];

                        span = span < 1 ? 1 : span > PD_TABLE_MAX_COLS ? PD_TABLE_MAX_COLS : span;

                        if (rows > 1 && c0 < PD_TABLE_MAX_COLS) {
                            h->rs_left[t][c0] = rows - 1 > 1000 ? 1000 : rows - 1;
                            h->rs_span[t][c0] = span;
                        }

                        h->rs_col[t] = c0 + span;
                    }

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
