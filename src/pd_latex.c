/*
 * Parade LaTeX export: an article that compiles with pdfLaTeX (Latin
 * text) or LuaLaTeX/XeLaTeX (any script). Sectioning, lists, quotes,
 * verbatim code, longtables with repeated headers, figures with \caption
 * and \label, cross-references, footnotes, links and equations. Images are
 * referenced as image<N>.<ext> and drawn as a framed box when the file is
 * not there, so the output compiles on its own.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

typedef struct {
    const pd_doc* d;
    pd_buf* o;
    pd_numbers nb;
    pd_block_id para;
    pd_char_props base;
    int in_link, in_code, in_caption, skip_until_seq;
    uint32_t cap_from;          /* captions: text before this offset ("Figure 3: ") is LaTeX's own */
    int ldepth, lkind[10];
    int quote_open, code_open, multicols;
} lx;

static void tex_esc(pd_buf* o, const char* s, size_t n) {
    size_t i, j = 0;

    for (i = 0; i < n; i++) {
        const char* r = NULL;

        switch (s[i]) {
            case '\\':
                r = "\\textbackslash{}";
                break;

            case '{':
                r = "\\{";
                break;

            case '}':
                r = "\\}";
                break;

            case '$':
                r = "\\$";
                break;

            case '&':
                r = "\\&";
                break;

            case '%':
                r = "\\%";
                break;

            case '#':
                r = "\\#";
                break;

            case '_':
                r = "\\_";
                break;

            case '~':
                r = "\\textasciitilde{}";
                break;

            case '^':
                r = "\\textasciicircum{}";
                break;

            case '<':
                r = "\\textless{}";
                break;

            case '>':
                r = "\\textgreater{}";
                break;

            case '\n':
                r = "\\newline{}";
                break;

            case '\t':
                r = "\\quad{}";
                break;
        }

        if (r) {
            pb_put(o, s + j, i - j);
            pb_puts(o, r);
            j = i + 1;
        }
    }

    pb_put(o, s + j, n - j);
}

static void url_esc(pd_buf* o, const char* s, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '%' || s[i] == '#' || s[i] == '\\' || s[i] == '{' || s[i] == '}') {
            pb_putc(o, '\\');
        }

        pb_putc(o, s[i]);
    }
}

static const char* ext_of(const char* mime) {
    return strstr(mime, "png") ? "png" : strstr(mime, "jpeg") || strstr(mime, "jpg") ? "jpg" : strstr(mime, "pdf") ? "pdf" :
           "img";
}

/* the label of a float or block that REF fields point at */
static void label_of(pd_buf* o, pd_block_id id) {
    pb_printf(o, "b%u", (unsigned)id);
}

static int lx_span(void* user, const pd_span* sp) {
    lx* x = (lx*)user;
    pd_buf* o = x->o;

    if (sp->is_object) {
        const pd_inline* ob = &sp->obj;

        switch (ob->kind) {
            case PD_INLINE_IMAGE: {
                const char* mime;
                const void* data;
                size_t len;

                pd_sp iw, ih;

                pd_doc_image_display_size(x->d, ob, &iw, &ih);

                if (pd_doc_resource(x->d, ob->resource, &mime, &data, &len) == PD_OK) {
                    pb_printf(o, "\\pdimage{image%u.%s}{%gpt}{%gpt}", (unsigned)ob->resource, ext_of(mime),
                              iw / 65536.0, ih / 65536.0);
                } else if (ob->source_len > 0 && !strpbrk(ob->source, "{}%#\\")) {     /* by address */
                    pb_printf(o, "\\pdimage{%s}{%gpt}{%gpt}", ob->source, iw / 65536.0, ih / 65536.0);
                }

                break;
            }

            case PD_INLINE_EQUATION:
                pb_puts(o, "\\(");

                if (ob->source) {
                    pb_put(o, ob->source, (size_t)ob->source_len);
                }

                pb_puts(o, "\\)");
                break;

            case PD_INLINE_FIELD:
                switch (ob->field) {
                    case PD_FIELD_SEQ:
                        if (x->in_caption) {
                            x->skip_until_seq = 2;  /* the caption's own "Figure N: " is LaTeX's */
                        } else {
                            pb_printf(o, "%d", (int)pd_numbers_at(&x->nb, x->para, sp->offset));
                        }

                        break;

                    case PD_FIELD_REF_NUMBER:
                        pb_puts(o, "\\ref{");
                        label_of(o, ob->target);
                        pb_putc(o, '}');
                        break;

                    case PD_FIELD_REF_PAGE:
                        pb_puts(o, "\\pageref{");
                        label_of(o, ob->target);
                        pb_putc(o, '}');
                        break;

                    case PD_FIELD_PAGE:
                        pb_puts(o, "\\thepage{}");
                        break;
                }

                break;

            case PD_INLINE_FOOTNOTE: {
                pd_block_info si;
                int32_t k;
                lx inner = *x;

                inner.in_caption = 0;
                inner.skip_until_seq = 0;
                pb_puts(o, "\\footnote{");

                if (pd_doc_block_info(x->d, ob->target, &si) == PD_OK) {
                    for (k = 0; k < si.child_count; k++) {
                        pd_block_id p = pd_doc_child(x->d, ob->target, k);

                        if (k > 0) {
                            pb_puts(o, "\\par ");
                        }

                        inner.para = p;
                        inner.in_link = 0;
                        pd_conv_base_props(x->d, p, &inner.base);
                        pd_conv_spans(x->d, p, lx_span, &inner);

                        if (inner.in_link) {
                            pb_putc(o, '}');
                        }
                    }
                }

                pb_putc(o, '}');
                break;
            }

            case PD_INLINE_LINK:
                if (x->in_link) {
                    pb_putc(o, '}');
                    x->in_link = 0;
                }

                if (ob->source && ob->source_len > 0) {
                    pb_puts(o, "\\href{");
                    url_esc(o, ob->source, (size_t)ob->source_len);
                    pb_puts(o, "}{");
                    x->in_link = 1;
                }

                break;

            case PD_INLINE_BOOKMARK:
                pb_printf(o, "\\label{%s}", ob->name);
                break;

            case PD_INLINE_TAB:
                pb_puts(o, "\\quad{}");
                break;
        }

        return o->err;
    }

    {
        const pd_char_props* c = &sp->cp, *b = &x->base;
        const char* t = sp->text;
        size_t n = sp->len;
        int close = 0;

        if (x->in_caption && sp->offset < x->cap_from) {   /* the label LaTeX writes itself */
            size_t cut = x->cap_from - sp->offset;

            if (cut >= n) {
                return 0;
            }

            t += cut;
            n -= cut;
        }

        if (x->skip_until_seq) {    /* and the ": " after the number */
            while (n > 0 && (*t == ':' || *t == ' ' || *t == '.')) {
                t++;
                n--;
            }

            x->skip_until_seq = n == 0;

            if (n == 0) {
                return 0;
            }
        }

#define WRAP(cond, cmd) do { if (cond) { pb_puts(o, cmd); close++; } } while (0)
        WRAP(c->weight >= 600 && b->weight < 600, "\\textbf{");
        WRAP(c->italic && !b->italic, "\\textit{");
        WRAP(c->underline && !b->underline, "\\uline{");
        WRAP(c->strike && !b->strike, "\\sout{");
        WRAP(c->shift == PD_SHIFT_SUPER, "\\textsuperscript{");
        WRAP(c->shift == PD_SHIFT_SUB, "\\textsubscript{");
        WRAP(!x->in_code && pd_conv_is_mono(c) && !pd_conv_is_mono(b), "\\texttt{");
#undef WRAP

        if (c->color != b->color) {
            pb_printf(o, "\\textcolor[HTML]{%06X}{", (unsigned)(c->color & 0xFFFFFF));
            close++;
        }

        if (x->in_code) {
            pb_put(o, t, n);
        } else {
            tex_esc(o, t, n);
        }

        while (close-- > 0) {
            pb_putc(o, '}');
        }
    }

    return o->err;
}

static void lx_inline(lx* x, pd_block_id p) {
    x->para = p;
    x->in_link = 0;
    pd_conv_base_props(x->d, p, &x->base);
    pd_conv_spans(x->d, p, lx_span, x);

    if (x->in_link) {
        pb_putc(x->o, '}');
        x->in_link = 0;
    }
}

static void lx_close_lists(lx* x, int keep) {
    while (x->ldepth > keep) {
        x->ldepth--;
        pb_puts(x->o, x->lkind[x->ldepth] == 2 ? "\\end{enumerate}\n" : "\\end{itemize}\n");
    }
}

static void lx_close_groups(lx* x) {
    if (x->quote_open) {
        pb_puts(x->o, "\\end{quote}\n\n");
        x->quote_open = 0;
    }

    if (x->code_open) {
        pb_puts(x->o, "\n\\end{verbatim}\n\n");
        x->code_open = 0;
    }
}

static void lx_block(lx* x, pd_block_id id, int in_figure);

static void lx_para(lx* x, pd_block_id p, int in_figure) {
    pd_block_info bi;
    pd_para_props pp, dp;
    int32_t level = 0;
    int kind = pd_conv_list_kind(x->d, p, &level);
    static const char* sect[] = { "section*", "section*", "subsection*", "subsubsection*", "paragraph*",
                                  "subparagraph*", "subparagraph*"
                                };

    pd_doc_block_info(x->d, p, &bi);
    pd_conv_style_pp(x->d, p, bi.style, &pp);

    if (pd_doc_para_props(x->d, p, &dp) == PD_OK && (dp.mask & PD_PP_ALIGN)) {
        pp.align = dp.align;
    }

    if (bi.role != PD_ROLE_CODE && x->code_open) {
        pb_puts(x->o, "\n\\end{verbatim}\n\n");
        x->code_open = 0;
    }

    if (bi.role != PD_ROLE_QUOTE && x->quote_open) {
        pb_puts(x->o, "\\end{quote}\n\n");
        x->quote_open = 0;
    }

    if (kind) {
        level = level > 3 ? 3 : level;     /* LaTeX nests lists four deep */
        lx_close_lists(x, level + 1);

        if (x->ldepth == level + 1 && x->lkind[level] != kind) {
            lx_close_lists(x, level);
        }

        while (x->ldepth < level + 1) {
            x->lkind[x->ldepth] = x->ldepth == level ? kind : 1;
            pb_puts(x->o, x->lkind[x->ldepth] == 2 ? "\\begin{enumerate}\n" : "\\begin{itemize}\n");

            if (x->ldepth < level) {
                pb_puts(x->o, "\\item[]\n");
            }

            x->ldepth++;
        }

        pb_puts(x->o, "\\item ");
        lx_inline(x, p);
        pb_putc(x->o, '\n');
        return;
    }

    lx_close_lists(x, 0);

    switch (bi.role) {
        case PD_ROLE_TITLE:
            pb_puts(x->o, "\\begin{center}\n{\\LARGE ");
            lx_inline(x, p);
            pb_puts(x->o, "\\par}\n\\end{center}\n\n");
            return;

        case PD_ROLE_HEADING:
            pb_printf(x->o, "\\%s{", sect[bi.level >= 1 && bi.level <= 6 ? bi.level : 1]);
            lx_inline(x, p);
            pb_puts(x->o, "}\n\n");
            return;

        case PD_ROLE_CAPTION:
            if (in_figure) {
                const char* t;
                uint32_t n, k;

                x->cap_from = 0;
                pd_doc_para_text(x->d, p, &t, &n);

                for (k = 0; k + 2 < n; k++) {   /* the caption's number field */
                    pd_pos at;
                    pd_inline o;

                    at.block = p;
                    at.offset = k;

                    if ((unsigned char)t[k] == 0xEF && pd_doc_inline_at(x->d, at, &o) == PD_OK &&
                            o.kind == PD_INLINE_FIELD && o.field == PD_FIELD_SEQ) {
                        x->cap_from = k;
                        break;
                    }
                }

                x->in_caption = 1;
                pb_puts(x->o, "\\caption{");
                lx_inline(x, p);
                pb_puts(x->o, "}\n");
                x->in_caption = 0;
                return;
            }

            break;

        case PD_ROLE_QUOTE:
            if (!x->quote_open) {
                pb_puts(x->o, "\\begin{quote}\n");
                x->quote_open = 1;
            }

            lx_inline(x, p);
            pb_puts(x->o, "\n\n");
            return;

        case PD_ROLE_CODE:
            pb_puts(x->o, x->code_open ? "\n" : "\\begin{verbatim}\n");
            x->code_open = 1;
            x->in_code = 1;
            {
                const char* t;
                uint32_t n;

                pd_doc_para_text(x->d, p, &t, &n);
                pb_put(x->o, t, n);
            }
            x->in_code = 0;
            return;

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
                    pb_puts(x->o, "\\[\n");
                    pb_put(x->o, o.source, (size_t)o.source_len);
                    pb_puts(x->o, "\n\\]\n\n");
                    return;
                }
            }

            break;
        }
    }

    if (pp.align == PD_ALIGN_CENTER || pp.align == PD_ALIGN_RIGHT) {
        const char* env = pp.align == PD_ALIGN_CENTER ? "center" : "flushright";

        pb_printf(x->o, "\\begin{%s}\n", env);
        lx_inline(x, p);
        pb_printf(x->o, "\n\\end{%s}\n\n", env);
    } else {
        lx_inline(x, p);
        pb_puts(x->o, "\n\n");
    }
}

static void lx_table(lx* x, pd_block_id t) {
    pd_block_info ti, ri;
    pd_table_props tp;
    int32_t r, c, k, ncols = 0;
    double colw;

    pd_doc_block_info(x->d, t, &ti);
    pd_doc_table_resolve(x->d, t, &tp);

    for (r = 0; r < ti.child_count; r++) {
        int32_t w = 0;

        pd_doc_block_info(x->d, pd_doc_child(x->d, t, r), &ri);

        for (c = 0; c < ri.child_count; c++) {
            pd_cell_props cp;

            pd_doc_cell_resolve(x->d, pd_doc_child(x->d, pd_doc_child(x->d, t, r), c), &cp);
            w += cp.col_span;
        }

        ncols = w > ncols ? w : ncols;
    }

    if (ncols == 0) {
        return;
    }

    colw = 0.92 / ncols;
    pb_puts(x->o, "\\begin{longtable}{");

    for (c = 0; c < ncols; c++) {
        pb_printf(x->o, "%sp{%.3f\\linewidth}", tp.border ? "|" : "", colw);
    }

    pb_printf(x->o, "%s}\n%s", tp.border ? "|" : "", tp.border ? "\\hline\n" : "");

    for (r = 0; r < ti.child_count; r++) {
        pd_block_id row = pd_doc_child(x->d, t, r);

        pd_doc_block_info(x->d, row, &ri);

        for (c = 0; c < ri.child_count; c++) {
            pd_block_id cell = pd_doc_child(x->d, row, c);
            pd_cell_props cp;
            pd_block_info ci;

            pd_doc_cell_resolve(x->d, cell, &cp);
            pd_doc_block_info(x->d, cell, &ci);

            if (c > 0) {
                pb_puts(x->o, " & ");
            }

            if (cp.col_span > 1) {
                pb_printf(x->o, "\\multicolumn{%d}{%sp{%.3f\\linewidth}%s}{", (int)cp.col_span, tp.border ? "|" : "",
                          colw * cp.col_span, tp.border ? "|" : "");
            }

            if (cp.background) {
                pb_printf(x->o, "\\cellcolor[HTML]{%06X}", (unsigned)(cp.background & 0xFFFFFF));
            }

            if (r < tp.header_rows) {
                pb_puts(x->o, "\\textbf{");
            }

            for (k = 0; k < ci.child_count; k++) {
                pd_block_info pi;

                if (pd_doc_block_info(x->d, pd_doc_child(x->d, cell, k), &pi) == PD_OK &&
                        pi.kind == PD_BLOCK_PARAGRAPH) {
                    if (k > 0) {
                        pb_puts(x->o, "\\newline ");
                    }

                    lx_inline(x, pi.id);
                }
            }

            if (r < tp.header_rows) {
                pb_putc(x->o, '}');
            }

            if (cp.col_span > 1) {
                pb_putc(x->o, '}');
            }
        }

        pb_printf(x->o, " \\\\%s\n", tp.border ? " \\hline" : "");

        if (r + 1 == tp.header_rows) {
            pb_puts(x->o, "\\endhead\n");
        }
    }

    pb_puts(x->o, "\\end{longtable}\n\n");
}

static void lx_block(lx* x, pd_block_id id, int in_figure) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(x->d, id, &bi) != PD_OK) {
        return;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        lx_para(x, id, in_figure);
        return;
    }

    lx_close_lists(x, 0);
    lx_close_groups(x);

    switch (bi.kind) {
        case PD_BLOCK_FLOAT:
            pb_puts(x->o, x->multicols ? "\\begin{figure*}[htbp]\n\\centering\n" : "\\begin{figure}[htbp]\n\\centering\n");

            for (i = 0; i < bi.child_count; i++) {
                lx_block(x, pd_doc_child(x->d, id, i), 1);
            }

            lx_close_lists(x, 0);
            lx_close_groups(x);
            pb_puts(x->o, "\\label{");
            label_of(x->o, id);
            pb_puts(x->o, x->multicols ? "}\n\\end{figure*}\n\n" : "}\n\\end{figure}\n\n");
            break;

        case PD_BLOCK_TABLE:
            lx_table(x, id);
            break;

        case PD_BLOCK_BREAK:
            pb_puts(x->o, bi.break_kind == PD_BREAK_RULE ? "\\noindent\\rule{\\linewidth}{0.4pt}\n\n" :
                    bi.break_kind == PD_BREAK_COLUMN && x->multicols ? "\\columnbreak\n\n" :
                    bi.break_kind == PD_BREAK_ODD_PAGE ? "\\cleardoublepage\n\n" : "\\newpage\n\n");
            break;

        case PD_BLOCK_SECTION: {
            pd_section_props sp;

            pd_doc_section_props(x->d, id, &sp);

            if (sp.columns > 1) {
                pb_printf(x->o, "\\begin{multicols}{%d}\n", (int)sp.columns);
                x->multicols = 1;
            }

            for (i = 0; i < bi.child_count; i++) {
                lx_block(x, pd_doc_child(x->d, id, i), 0);
            }

            lx_close_lists(x, 0);
            lx_close_groups(x);

            if (sp.columns > 1) {
                pb_puts(x->o, "\\end{multicols}\n");
                x->multicols = 0;
            }

            if (!sp.continuous && i > 0) {
                /* a new section starts a page unless it says otherwise; decided by the next one */
            }

            break;
        }

        default:
            for (i = 0; i < bi.child_count; i++) {
                pd_block_id k = pd_doc_child(x->d, id, i);
                pd_block_info ki;

                if (bi.kind == PD_BLOCK_ROOT && i > 0 && pd_doc_block_info(x->d, k, &ki) == PD_OK &&
                        ki.kind == PD_BLOCK_SECTION) {
                    pd_section_props sp;

                    pd_doc_section_props(x->d, k, &sp);

                    if (!sp.continuous) {
                        pb_puts(x->o, "\\clearpage\n\n");
                    }
                }

                lx_block(x, k, in_figure);
            }
    }
}

pd_status pd_latex_export(const pd_doc* d, pd_buf* o) {
    lx* x = (lx*)calloc(1, sizeof(lx));
    pd_section_props sp;

    if (!x) {
        return PD_ERR_NOMEM;
    }

    x->d = d;
    x->o = o;
    pd_numbers_init(&x->nb, d);
    pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp);
    pb_puts(o, "% Generated by Parade\n"
            "\\documentclass[10pt]{article}\n"
            "\\usepackage{iftex}\n"
            "\\ifPDFTeX\n"
            "  \\usepackage[T1]{fontenc}\n"
            "  \\usepackage[utf8]{inputenc}\n"
            "  \\usepackage{lmodern}\n"
            "\\else\n"
            "  \\usepackage{fontspec}\n"
            "\\fi\n");
    pb_printf(o, "\\usepackage[paperwidth=%gpt,paperheight=%gpt,left=%gpt,right=%gpt,top=%gpt,bottom=%gpt]{geometry}\n",
              sp.page_width / 65536.0, sp.page_height / 65536.0, sp.margin_left / 65536.0, sp.margin_right / 65536.0,
              sp.margin_top / 65536.0, sp.margin_bottom / 65536.0);
    pb_puts(o, "\\usepackage{graphicx}\n"
            "\\usepackage[table]{xcolor}\n"
            "\\usepackage[normalem]{ulem}\n"
            "\\usepackage{longtable}\n"
            "\\usepackage{multicol}\n"
            "\\usepackage{amsmath}\n"
            "\\usepackage{hyperref}\n"
            "\\setlength{\\parindent}{0pt}\n"
            "\\setlength{\\parskip}{0.5em}\n"
            "% an image file if it is there, else a framed placeholder of the same size\n"
            "\\newcommand{\\pdimage}[3]{\\IfFileExists{#1}{\\includegraphics[width=#2,height=#3]{#1}}"
            "{\\fbox{\\parbox[c][#3][c]{#2}{\\centering\\tiny #1}}}}\n"
            "\\begin{document}\n\n");
    lx_block(x, pd_doc_root(d), 0);
    lx_close_lists(x, 0);
    lx_close_groups(x);
    pb_puts(o, "\\end{document}\n");
    pd_numbers_free(&x->nb);
    free(x);
    return o->err ? PD_ERR_NOMEM : PD_OK;
}
