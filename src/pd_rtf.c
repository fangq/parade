/*
 * Parade RTF: export (Word-compatible: style sheet, list table, tables,
 * footnotes, hyperlink fields, PNG/JPEG pictures, sections) and import
 * (the same, from Word, LibreOffice, WordPad and macOS TextEdit output)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

#define TWIPS(sp) ((int)((int64_t)(sp) * 20 / 65536))

/* ------------------------------------------------------------------ */
/* export                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const pd_doc* d;
    pd_buf* o;
    pd_numbers nb;
    pd_block_id para;
    pd_char_props base;
    char fonts[32][64];
    int nfonts;
    uint32_t colors[64];
    int ncolors;
    int in_link, in_cell;
    pd_sp text_w;
} rx;

static int font_index(rx* x, const char* family) {
    int i;

    for (i = 0; i < x->nfonts; i++) {
        if (strcmp(x->fonts[i], family) == 0) {
            return i;
        }
    }

    if (x->nfonts < 32) {
        snprintf(x->fonts[x->nfonts], sizeof(x->fonts[0]), "%s", family);
        return x->nfonts++;
    }

    return 0;
}

static int color_index(rx* x, uint32_t c) {
    int i;

    c &= 0xFFFFFF;

    for (i = 0; i < x->ncolors; i++) {
        if (x->colors[i] == c) {
            return i + 1;   /* entry 0 is "auto" */
        }
    }

    if (x->ncolors < 64) {
        x->colors[x->ncolors++] = c;
        return x->ncolors;
    }

    return 0;
}

/* UTF-8 text as RTF: escapes, \uN? for anything beyond ASCII */
static void rtf_text(pd_buf* o, const char* s, size_t n, int code) {
    size_t i = 0;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        int k, len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;

        if (c < 0x80) {
            if (c == '\\' || c == '{' || c == '}') {
                pb_putc(o, '\\');
                pb_putc(o, (char)c);
            } else if (c == '\n') {
                pb_puts(o, "\\line ");
            } else if (c == '\t') {
                pb_puts(o, "\\tab ");
            } else if (c >= 32) {
                pb_putc(o, (char)c);
            }

            i++;
            continue;
        }

        cp = c & (0x7F >> len);

        for (k = 1; k < len && i + (size_t)k < n; k++) {
            cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 0x3F);
        }

        i += (size_t)len;

        if (cp == 0xA0) {
            pb_puts(o, "\\~");
        } else if (cp > 0xFFFF) {   /* a surrogate pair */
            uint32_t v = cp - 0x10000;

            pb_printf(o, "\\u%d?\\u%d?", (int)(int16_t)(0xD800 + (v >> 10)), (int)(int16_t)(0xDC00 + (v & 0x3FF)));
        } else {
            pb_printf(o, "\\u%d?", (int)(int16_t)cp);
        }
    }

    (void)code;
}

static void rx_font_cmds(rx* x, const pd_char_props* c, const pd_char_props* b) {
    pd_buf* o = x->o;

    if (c->weight >= 600 && !(b && b->weight >= 600)) {
        pb_puts(o, "\\b");
    }

    if (c->italic && !(b && b->italic)) {
        pb_puts(o, "\\i");
    }

    if (c->underline && !(b && b->underline == c->underline)) {
        static const char* ul[] = { "\\ulnone", "\\ul", "\\uldb", "\\ulth", "\\uld", "\\uldash", "\\ulwave", "\\ulw" };

        pb_puts(o, ul[c->underline >= 0 && c->underline <= PD_UNDERLINE_WORDS ? c->underline : 1]);
    }

    if (c->caps && !(b && b->caps)) {
        pb_puts(o, "\\caps");
    }

    if (c->small_caps && !(b && b->small_caps)) {
        pb_puts(o, "\\scaps");
    }

    if (c->strike && !(b && b->strike)) {
        pb_puts(o, "\\strike");
    }

    if (c->shift == PD_SHIFT_SUPER) {
        pb_puts(o, "\\super");
    } else if (c->shift == PD_SHIFT_SUB) {
        pb_puts(o, "\\sub");
    }

    if (!b || strcmp(c->family, b->family)) {
        pb_printf(o, "\\f%d", font_index(x, pd_conv_is_mono(c) ? "Courier New" : c->family));
    }

    if (!b || c->size != b->size) {
        pb_printf(o, "\\fs%d", (int)((int64_t)c->size * 2 / 65536));
    }

    if (!b || c->color != b->color) {
        pb_printf(o, "\\cf%d", color_index(x, c->color));
    }

    if (c->background && (!b || c->background != b->background)) {
        pb_printf(o, "\\chcbpat%d\\highlight%d", color_index(x, c->background), color_index(x, c->background));
    }

    if (o->n && o->p[o->n - 1] != '{') {    /* the delimiter of the last control word; after '{' a space is text */
        pb_putc(o, ' ');
    }
}

static int rx_span(void* user, const pd_span* sp) {
    rx* x = (rx*)user;
    pd_buf* o = x->o;

    if (sp->is_object) {
        const pd_inline* ob = &sp->obj;

        switch (ob->kind) {
            case PD_INLINE_IMAGE: {
                const char* mime;
                const void* data;
                size_t len, i;

                if (pd_doc_resource(x->d, ob->resource, &mime, &data, &len) == PD_OK &&
                        (strstr(mime, "png") || strstr(mime, "jpeg") || strstr(mime, "jpg"))) {
                    pd_sp iw, ih;
                    int w, h;

                    pd_doc_image_display_size(x->d, ob, &iw, &ih);
                    w = TWIPS(iw);
                    h = TWIPS(ih);

                    pb_printf(o, "{\\pict%s\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\n", strstr(mime, "png") ? "\\pngblip" :
                              "\\jpegblip", w / 15 > 0 ? w / 15 : 1, h / 15 > 0 ? h / 15 : 1, w, h);

                    for (i = 0; i < len; i++) {
                        pb_printf(o, "%02x", ((const unsigned char*)data)[i]);

                        if (i % 64 == 63) {
                            pb_putc(o, '\n');
                        }
                    }

                    pb_puts(o, "}");
                }

                break;
            }

            case PD_INLINE_EQUATION:
                pb_puts(o, "{\\i ");

                if (ob->source) {
                    rtf_text(o, ob->source, (size_t)ob->source_len, 0);
                }

                pb_puts(o, "}");
                break;

            case PD_INLINE_FIELD:
                if (ob->field == PD_FIELD_SEQ) {
                    pb_printf(o, "{\\field{\\*\\fldinst SEQ %s \\\\* ARABIC}{\\fldrslt %d}}", ob->name[0] ? ob->name :
                              "Figure", (int)pd_numbers_at(&x->nb, x->para, sp->offset));
                } else if (ob->field == PD_FIELD_REF_NUMBER) {
                    pb_printf(o, "%d", (int)pd_numbers_ref(&x->nb, ob->target));
                } else if (ob->field == PD_FIELD_PAGE) {
                    pb_puts(o, "{\\field{\\*\\fldinst PAGE}{\\fldrslt 1}}");
                } else if (ob->field == PD_FIELD_PAGES) {
                    pb_puts(o, "{\\field{\\*\\fldinst NUMPAGES}{\\fldrslt 1}}");
                }

                break;

            case PD_INLINE_FOOTNOTE: {
                pd_block_info si;
                int32_t k;
                rx inner = *x;

                pb_puts(o, "{\\super\\chftn}{\\footnote\\pard\\plain\\fs20{\\super\\chftn}");

                if (pd_doc_block_info(x->d, ob->target, &si) == PD_OK) {
                    for (k = 0; k < si.child_count; k++) {
                        pd_block_id p = pd_doc_child(x->d, ob->target, k);

                        if (k > 0) {
                            pb_puts(o, "\\par ");
                        }

                        inner.para = p;
                        inner.in_link = 0;
                        pd_conv_base_props(x->d, p, &inner.base);
                        pd_conv_spans(x->d, p, rx_span, &inner);

                        if (inner.in_link) {
                            pb_puts(o, "}}}");
                        }
                    }
                }

                /* fonts and colors the note introduced */
                x->nfonts = inner.nfonts;
                memcpy(x->fonts, inner.fonts, sizeof(x->fonts));
                x->ncolors = inner.ncolors;
                memcpy(x->colors, inner.colors, sizeof(x->colors));
                pb_puts(o, "}");
                break;
            }

            case PD_INLINE_LINK:
                if (x->in_link) {
                    pb_puts(o, "}}}");
                    x->in_link = 0;
                }

                if (ob->source && ob->source_len > 0) {
                    pb_puts(o, "{\\field{\\*\\fldinst HYPERLINK \"");
                    rtf_text(o, ob->source, (size_t)ob->source_len, 0);
                    pb_puts(o, "\"}{\\fldrslt{");
                    x->in_link = 1;
                }

                break;

            case PD_INLINE_TAB:
                pb_puts(o, "\\tab ");
                break;
        }

        return o->err;
    }

    pb_putc(o, '{');
    rx_font_cmds(x, &sp->cp, &x->base);
    rtf_text(o, sp->text, sp->len, 0);
    pb_putc(o, '}');
    return o->err;
}

static void rx_inline(rx* x, pd_block_id p) {
    x->para = p;
    x->in_link = 0;
    pd_conv_base_props(x->d, p, &x->base);
    pd_conv_spans(x->d, p, rx_span, x);

    if (x->in_link) {
        pb_puts(x->o, "}}}");
        x->in_link = 0;
    }
}

/* style sheet entries: index, name, outline level */
static const struct {
    const char* name;
    int role, level;
} rstyles[] = {
    { "Normal", PD_ROLE_BODY, 0 }, { "heading 1", PD_ROLE_HEADING, 1 }, { "heading 2", PD_ROLE_HEADING, 2 },
    { "heading 3", PD_ROLE_HEADING, 3 }, { "heading 4", PD_ROLE_HEADING, 4 }, { "heading 5", PD_ROLE_HEADING, 5 },
    { "heading 6", PD_ROLE_HEADING, 6 }, { "Title", PD_ROLE_TITLE, 0 }, { "Quote", PD_ROLE_QUOTE, 0 },
    { "Code", PD_ROLE_CODE, 0 }, { "Caption", PD_ROLE_CAPTION, 0 }
};

static int rx_style_of(const pd_block_info* bi) {
    int i;

    for (i = 0; i < (int)(sizeof(rstyles) / sizeof(rstyles[0])); i++) {
        if (rstyles[i].role == bi->role && (bi->role != PD_ROLE_HEADING || rstyles[i].level == bi->level)) {
            return i;
        }
    }

    return 0;
}

/* paragraph properties after \pard */
static void rx_para_props(rx* x, pd_block_id p, int in_table) {
    pd_block_info bi;
    pd_para_props pp, dp;
    int32_t level = 0;
    int kind = pd_conv_list_kind(x->d, p, &level), s;
    pd_char_props base;

    pd_doc_block_info(x->d, p, &bi);
    pd_doc_style_resolve(x->d, bi.style, &pp, NULL);

    if (pd_doc_para_props(x->d, p, &dp) == PD_OK) {
        if (dp.mask & PD_PP_ALIGN) {
            pp.align = dp.align;
        }
    }

    s = rx_style_of(&bi);
    pb_printf(x->o, "\\pard\\plain\\s%d", s);

    if (bi.role == PD_ROLE_HEADING) {
        pb_printf(x->o, "\\outlinelevel%d", (int)bi.level - 1);
    }

    if (pd_doc_para_rtl(x->d, p) == 1) {   /* right to left: its start on the right */
        pb_puts(x->o, pp.align == PD_ALIGN_CENTER ? "\\rtlpar\\qc" : pp.align == PD_ALIGN_RIGHT ? "\\rtlpar\\ql" :
                pp.align == PD_ALIGN_JUSTIFY ? "\\rtlpar\\qj" : "\\rtlpar\\qr");
    } else {
        pb_puts(x->o, pp.align == PD_ALIGN_CENTER ? "\\qc" : pp.align == PD_ALIGN_RIGHT ? "\\qr" :
                pp.align == PD_ALIGN_JUSTIFY ? "\\qj" : "\\ql");
    }

    if (bi.role == PD_ROLE_QUOTE) {
        pb_puts(x->o, "\\li720\\ri720");
    } else if (kind) {
        pb_printf(x->o, "\\ls%d\\ilvl%d\\fi-360\\li%d", kind, (int)level, 720 + 360 * (int)level);
    }

    pb_printf(x->o, "\\sb%d\\sa%d", TWIPS(pp.space_before), TWIPS(pp.space_after) ? TWIPS(pp.space_after) : 120);

    if (in_table) {
        pb_puts(x->o, "\\intbl");
    }

    /* the paragraph's own character look (readers apply no style formatting) */
    pd_conv_base_props(x->d, p, &base);

    if (bi.role == PD_ROLE_CODE) {
        strcpy(base.family, "Courier New");
    }

    rx_font_cmds(x, &base, NULL);

    if (kind) {     /* the label for readers without list support */
        char label[32];

        if (pd_doc_list_label(x->d, p, label, sizeof(label)) == PD_OK) {
            pb_puts(x->o, "{\\listtext ");
            rtf_text(x->o, label, strlen(label), 0);
            pb_puts(x->o, "\\tab}");
        }
    }
}

static void rx_block(rx* x, pd_block_id id, int in_table);

static void rx_table(rx* x, pd_block_id t) {
    pd_block_info ti, ri;
    pd_table_props tp;
    int32_t r, c, k, ncols = 0;

    pd_doc_block_info(x->d, t, &ti);
    pd_doc_table_props(x->d, t, &tp);

    for (r = 0; r < ti.child_count; r++) {
        int32_t w = 0;

        pd_doc_block_info(x->d, pd_doc_child(x->d, t, r), &ri);

        for (c = 0; c < ri.child_count; c++) {
            pd_cell_props cp;

            pd_doc_cell_props(x->d, pd_doc_child(x->d, pd_doc_child(x->d, t, r), c), &cp);
            w += cp.col_span;
        }

        ncols = w > ncols ? w : ncols;
    }

    for (r = 0; r < ti.child_count && ncols > 0; r++) {
        pd_block_id row = pd_doc_child(x->d, t, r);
        int colw = TWIPS(x->text_w) / ncols, edge = 0;

        pd_doc_block_info(x->d, row, &ri);
        pb_printf(x->o, "\\trowd\\trgaph108\\trleft0%s%s\n", r < tp.header_rows ? "\\trhdr" : "",
                  tp.direction == PD_DIR_RTL ? "\\taprtl" : "");

        for (c = 0; c < ri.child_count; c++) {
            pd_cell_props cp;

            pd_doc_cell_props(x->d, pd_doc_child(x->d, row, c), &cp);

            if (cp.background) {
                pb_printf(x->o, "\\clcbpat%d", color_index(x, cp.background));
            }

            if (cp.valign) {
                pb_puts(x->o, cp.valign == 1 ? "\\clvertalc" : "\\clvertalb");
            }

            if (tp.border) {
                pb_puts(x->o, "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10"
                        "\\clbrdrr\\brdrs\\brdrw10");
            }

            if (cp.col_span > 1) {
                pb_puts(x->o, "\\clmgf");
            }

            edge += colw;
            pb_printf(x->o, "\\cellx%d", edge);

            for (k = 1; k < cp.col_span; k++) {     /* the merged continuation cells */
                edge += colw;
                pb_printf(x->o, "%s\\clmrg\\cellx%d", tp.border ? "\\clbrdrt\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10" :
                          "", edge);
            }
        }

        pb_putc(x->o, '\n');

        for (c = 0; c < ri.child_count; c++) {
            pd_block_id cell = pd_doc_child(x->d, row, c);
            pd_block_info ci;
            pd_cell_props cp;

            pd_doc_block_info(x->d, cell, &ci);
            pd_doc_cell_props(x->d, cell, &cp);

            for (k = 0; k < ci.child_count; k++) {
                pd_block_id p = pd_doc_child(x->d, cell, k);
                pd_block_info pi;

                if (pd_doc_block_info(x->d, p, &pi) != PD_OK || pi.kind != PD_BLOCK_PARAGRAPH) {
                    continue;
                }

                rx_para_props(x, p, 1);

                if (r < tp.header_rows) {
                    pb_puts(x->o, "\\b ");
                }

                rx_inline(x, p);
                pb_puts(x->o, k + 1 < ci.child_count ? "\\par\n" : "\\cell\n");
            }

            if (ci.child_count == 0) {
                pb_puts(x->o, "\\pard\\intbl\\cell\n");
            }

            for (k = 1; k < cp.col_span; k++) {
                pb_puts(x->o, "\\pard\\intbl\\cell\n");
            }
        }

        pb_printf(x->o, "\\trowd\\trgaph108\\trleft0%s\\row\n", r < tp.header_rows ? "\\trhdr" : "");
    }

    pb_puts(x->o, "\\pard\n");
}

static void rx_block(rx* x, pd_block_id id, int in_table) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(x->d, id, &bi) != PD_OK) {
        return;
    }

    switch (bi.kind) {
        case PD_BLOCK_PARAGRAPH:
            rx_para_props(x, id, in_table);
            rx_inline(x, id);
            pb_puts(x->o, "\\par\n");
            break;

        case PD_BLOCK_TABLE:
            rx_table(x, id);
            break;

        case PD_BLOCK_BREAK:
            pb_puts(x->o, bi.break_kind == PD_BREAK_RULE ? "\\pard\\brdrb\\brdrs\\brdrw10\\brsp20\\par\\pard\n" :
                    bi.break_kind == PD_BREAK_COLUMN ? "\\column\n" : "\\page\n");
            break;

        case PD_BLOCK_SECTION: {
            pd_section_props sp;

            pd_doc_section_props(x->d, id, &sp);
            x->text_w = sp.page_width - sp.margin_left - sp.margin_right;

            if (bi.index > 0) {
                pb_printf(x->o, "\\sect\\sectd%s", sp.continuous ? "\\sbknone" : "\\sbkpage");
            } else {
                pb_puts(x->o, "\\sectd");
            }

            pb_printf(x->o, "\\pgwsxn%d\\pghsxn%d\\marglsxn%d\\margrsxn%d\\margtsxn%d\\margbsxn%d",
                      TWIPS(sp.page_width), TWIPS(sp.page_height), TWIPS(sp.margin_left), TWIPS(sp.margin_right),
                      TWIPS(sp.margin_top), TWIPS(sp.margin_bottom));

            if (sp.columns > 1) {
                pb_printf(x->o, "\\cols%d\\colsx%d", (int)sp.columns, TWIPS(sp.column_gap));
            }

            pb_putc(x->o, '\n');

            for (i = 0; i < bi.child_count; i++) {
                rx_block(x, pd_doc_child(x->d, id, i), in_table);
            }

            break;
        }

        default:    /* root, floats */
            for (i = 0; i < bi.child_count; i++) {
                rx_block(x, pd_doc_child(x->d, id, i), in_table);
            }
    }
}

pd_status pd_rtf_export(const pd_doc* d, pd_buf* out) {
    rx* x = (rx*)calloc(1, sizeof(rx));
    pd_buf body;
    pd_section_props sp;
    int i, lv;

    if (!x) {
        return PD_ERR_NOMEM;
    }

    memset(&body, 0, sizeof(body));
    x->d = d;
    x->o = &body;
    pd_numbers_init(&x->nb, d);
    font_index(x, "Times New Roman");
    font_index(x, "Courier New");
    color_index(x, 0xFF000000u);
    pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp);
    x->text_w = sp.page_width - sp.margin_left - sp.margin_right;
    rx_block(x, pd_doc_root(d), 0);

    /* the header, now that the fonts and colors are known */
    pb_puts(out, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1\n{\\fonttbl");

    for (i = 0; i < x->nfonts; i++) {
        pb_printf(out, "{\\f%d\\f%s\\fcharset0 ", i, i == 1 ? "modern" : "roman");
        rtf_text(out, x->fonts[i], strlen(x->fonts[i]), 0);
        pb_puts(out, ";}");
    }

    pb_puts(out, "}\n{\\colortbl;");

    for (i = 0; i < x->ncolors; i++) {
        pb_printf(out, "\\red%u\\green%u\\blue%u;", (unsigned)(x->colors[i] >> 16) & 255,
                  (unsigned)(x->colors[i] >> 8) & 255, (unsigned)x->colors[i] & 255);
    }

    pb_puts(out, "}\n{\\stylesheet");

    for (i = 0; i < (int)(sizeof(rstyles) / sizeof(rstyles[0])); i++) {
        pb_printf(out, "{\\s%d", i);

        if (rstyles[i].role == PD_ROLE_HEADING) {
            pb_printf(out, "\\outlinelevel%d", rstyles[i].level - 1);
        }

        pb_printf(out, " %s;}", rstyles[i].name);
    }

    /* two lists: bullets (\ls1) and decimal numbers (\ls2), nine levels each */
    pb_puts(out, "}\n{\\*\\listtable");

    for (i = 1; i <= 2; i++) {
        pb_printf(out, "\n{\\list\\listtemplateid%d\\listsimple0", i);

        for (lv = 0; lv < 9; lv++) {
            if (i == 1) {
                pb_printf(out, "{\\listlevel\\levelnfc23\\levelnfcn23\\leveljc0\\levelstartat1\\levelfollow0"
                          "{\\leveltext\\'01\\u%d ?;}{\\levelnumbers;}\\fi-360\\li%d}", lv % 3 == 0 ? 8226 : lv % 3 == 1 ? 9702 :
                          9642, 720 + 360 * lv);
            } else {
                pb_printf(out, "{\\listlevel\\levelnfc0\\levelnfcn0\\leveljc0\\levelstartat1\\levelfollow0"
                          "{\\leveltext\\'02\\'%02x.;}{\\levelnumbers\\'01;}\\fi-360\\li%d}", lv, 720 + 360 * lv);
            }
        }

        pb_printf(out, "\\listid%d}", i);
    }

    pb_puts(out, "}\n{\\*\\listoverridetable{\\listoverride\\listid1\\listoverridecount0\\ls1}"
            "{\\listoverride\\listid2\\listoverridecount0\\ls2}}\n");
    pb_printf(out, "\\paperw%d\\paperh%d\\margl%d\\margr%d\\margt%d\\margb%d\n", TWIPS(sp.page_width),
              TWIPS(sp.page_height), TWIPS(sp.margin_left), TWIPS(sp.margin_right), TWIPS(sp.margin_top),
              TWIPS(sp.margin_bottom));
    pb_put(out, body.p, body.n);
    pb_puts(out, "}\n");
    pb_free(&body);
    pd_numbers_free(&x->nb);
    free(x);
    return out->err ? PD_ERR_NOMEM : PD_OK;
}

/* ------------------------------------------------------------------ */
/* import                                                             */
/* ------------------------------------------------------------------ */

enum {
    D_TEXT = 0, D_SKIP, D_FONTTBL, D_COLORTBL, D_STYLESHEET, D_PICT, D_FOOTNOTE, D_FLDINST, D_FLDRSLT, D_FIELD,
    D_LISTTABLE, D_LISTOVERRIDE, D_STYLE_ENTRY, D_FONT_ENTRY
};

typedef struct {
    int dest;
    int bold, italic, ul, strike, shift, font, fs, cf, bg, uc, hidden, caps, scaps;
    int note_end;               /* this group is a footnote: end it on close */
    int saved[6];               /* footnotes: the paragraph state to go back to */
    int field_end;              /* this group is a field: end its link on close */
} rgroup;

typedef struct {
    int list_id, kind[9];
} rlist;

typedef struct {
    pd_bld* b;
    rgroup g[256];
    int depth;
    /* tables of the document */
    char fonts[64][64];
    int font_num[64], nfonts;
    uint32_t colors[256];
    int ncolors, color_parts[3], color_set;
    char styles[256][64];
    int nstyles_seen;
    rlist lists[16];
    int nlists, ls_to_list[32];
    int cur_list, cur_level;
    /* paragraph state (\pard resets it) */
    int style, align, outline, intbl, ls, ilvl, li;
    int rtl;                    /* \rtlpar 1, \ltrpar 0, -1 unsaid */
    int row_rtl;                /* \taprtl: the row's table from the right */
    /* tables */
    int table_open, row_open, cell_open, ncells_row;
    int cell_bg[64], cell_merge[64], cell_valign[64], ncelldefs, row_header;
    int pending_span;
    /* pictures */
    pd_buf hex;
    char pict_mime[32];
    int picw_goal, pich_goal, scalex, scaley, picw, pich;
    /* fields */
    pd_buf fldinst;
    int link_open;
    /* text pending in the current destination */
    pd_buf name;                /* font, style or color table entry text */
    int skip_chars;             /* \uc fallback characters still to drop */
    uint16_t high_surrogate;
    int codepage;
    int para_has_text;
    int list_text;              /* inside \listtext / \pntext: dropped */
} ri;

static rgroup* G(ri* r) {
    return &r->g[r->depth];
}

static void ri_format(ri* r) {
    rgroup* g = G(r);
    pd_char_props cp;

    memset(&cp, 0, sizeof(cp));

    if (g->bold) {
        cp.mask |= PD_CP_WEIGHT;
        cp.weight = 700;
    }

    if (g->italic) {
        cp.mask |= PD_CP_ITALIC;
        cp.italic = 1;
    }

    if (g->ul) {
        cp.mask |= PD_CP_UNDERLINE;
        cp.underline = g->ul;
    }

    if (g->strike) {
        cp.mask |= PD_CP_STRIKE;
        cp.strike = 1;
    }

    if (g->shift) {
        cp.mask |= PD_CP_SHIFT;
        cp.shift = g->shift;
    }

    if (g->caps) {
        cp.mask |= PD_CP_CAPS;
        cp.caps = 1;
    }

    if (g->scaps) {
        cp.mask |= PD_CP_SMALLCAPS;
        cp.small_caps = 1;
    }

    if (g->font > 0) {
        int i;

        for (i = 0; i < r->nfonts; i++) {
            if (r->font_num[i] == g->font) {
                const char* f = r->fonts[i];

                if (strstr(f, "Courier") || strstr(f, "Mono") || strstr(f, "mono") || strstr(f, "Consolas")) {
                    cp.mask |= PD_CP_FAMILY;
                    strcpy(cp.family, "monospace");
                }

                break;
            }
        }
    }

    if (g->cf > 0 && g->cf < r->ncolors && r->colors[g->cf] != 0xFF000000u) {
        cp.mask |= PD_CP_COLOR;
        cp.color = r->colors[g->cf];
    }

    if (g->bg > 0 && g->bg < r->ncolors) {
        cp.mask |= PD_CP_BACKGROUND;
        cp.background = r->colors[g->bg];
    }

    bld_set_format(r->b, &cp);
}

static void ri_close_table(ri* r) {
    if (r->cell_open) {
        bld_end_para(r->b);
        bld_cell_end(r->b);
        r->cell_open = 0;
    }

    if (r->table_open) {
        if (r->row_rtl && r->b->ntables >= 1 && r->b->ntables <= 8) {  /* \taprtl: its columns from the right */
            pd_table_props tp;

            if (pd_doc_table_props(r->b->d, r->b->table[r->b->ntables - 1], &tp) == PD_OK) {
                tp.direction = PD_DIR_RTL;
                pd_doc_set_table_props(r->b->d, r->b->table[r->b->ntables - 1], &tp);
            }
        }

        bld_table_end(r->b);
        r->table_open = r->row_open = 0;
    }
}

/* the paragraph properties become the builder's for the next paragraph */
static void ri_begin_para(ri* r) {
    pd_bld* b = r->b;
    const char* sname = r->style >= 0 && r->style < 256 ? r->styles[r->style] : "";
    char low[64];
    size_t k;

    if (G(r)->dest == D_FOOTNOTE) {
        /* a note's paragraphs never open or close tables */
    } else if (!r->intbl && r->table_open) {
        ri_close_table(r);
    }

    if (r->intbl && G(r)->dest != D_FOOTNOTE) {
        if (!r->table_open) {
            bld_table_begin(b);
            r->table_open = 1;
            r->row_open = 0;
        }

        if (!r->row_open) {
            bld_row_begin(b, r->row_header);
            r->row_open = 1;
            r->ncells_row = 0;
        }

        if (!r->cell_open) {
            int span = 1, idx = r->ncells_row;

            while (idx + span < r->ncelldefs && idx + span < 64 && r->cell_merge[idx + span] == 2) {
                span++;
            }

            bld_cell_begin(b, span, idx < 64 ? (uint32_t)(r->cell_bg[idx] > 0 && r->cell_bg[idx] < r->ncolors ?
                           r->colors[r->cell_bg[idx]] : 0) : 0);
            r->cell_open = 1;
            r->pending_span = span - 1;
        }
    }

    for (k = 0; k + 1 < sizeof(low) && sname[k]; k++) {
        low[k] = (char)tolower((unsigned char)sname[k]);
    }

    low[k] = '\0';

    if (r->outline >= 0 && r->outline < 6) {
        char st[16];

        snprintf(st, sizeof(st), "Heading %d", r->outline + 1);
        bld_para_style(b, st, PD_ROLE_HEADING, r->outline + 1);
    } else if (strncmp(low, "heading ", 8) == 0 && low[8] >= '1' && low[8] <= '6') {
        char st[16];

        snprintf(st, sizeof(st), "Heading %c", low[8]);
        bld_para_style(b, st, PD_ROLE_HEADING, low[8] - '0');
    } else if (strcmp(low, "title") == 0) {
        bld_para_style(b, "Title", PD_ROLE_TITLE, 0);
    } else if (strstr(low, "quote")) {
        bld_para_style(b, "Quote", PD_ROLE_QUOTE, 0);
    } else if (strcmp(low, "code") == 0 || strstr(low, "source") || strstr(low, "preformatted")) {
        bld_para_style(b, "Code", PD_ROLE_CODE, 0);
    } else if (strcmp(low, "caption") == 0) {
        bld_para_style(b, "Caption", PD_ROLE_CAPTION, 0);
    }

    if (r->ls > 0) {
        int kind = 1, i;

        if (r->ls < 32 && r->ls_to_list[r->ls]) {
            for (i = 0; i < r->nlists; i++) {
                if (r->lists[i].list_id == r->ls_to_list[r->ls]) {
                    kind = r->lists[i].kind[r->ilvl >= 0 && r->ilvl < 9 ? r->ilvl : 0];
                }
            }
        }

        bld_list(b, kind, r->ilvl);
    }

    if (r->rtl >= 0) {
        b->pp.mask |= PD_PP_DIRECTION;
        b->pp.direction = r->rtl ? PD_DIR_RTL : PD_DIR_LTR;
    }

    if (r->align >= 0) {    /* (RTF's \ql, \qr: sides of the page; right to left, the text's end and start) */
        b->pp.mask |= PD_PP_ALIGN;
        b->pp.align = r->rtl == 1 && r->align == PD_ALIGN_LEFT ? PD_ALIGN_RIGHT : r->rtl == 1 &&
                      r->align == PD_ALIGN_RIGHT ? PD_ALIGN_LEFT : r->align;
    }

    bld_begin_para(b);
    r->para_has_text = 0;
}

static void ri_char(ri* r, uint32_t cp) {
    rgroup* g = G(r);
    char u[4];
    int n;

    if (g->dest == D_FONTTBL || g->dest == D_STYLESHEET) {
        return;
    }

    if (g->dest == D_FONT_ENTRY || g->dest == D_STYLE_ENTRY) {   /* a table entry ends at ';' */
        if (cp == ';') {
            size_t k = r->name.n;

            while (k > 0 && r->name.p[k - 1] == ' ') {
                k--;
            }

            if (g->dest == D_FONT_ENTRY && r->nfonts < 64) {
                snprintf(r->fonts[r->nfonts], sizeof(r->fonts[0]), "%.*s", (int)k, r->name.p ? r->name.p : "");
                r->nfonts++;
            } else if (g->dest == D_STYLE_ENTRY && r->nstyles_seen >= 0 && r->nstyles_seen < 256) {
                snprintf(r->styles[r->nstyles_seen], sizeof(r->styles[0]), "%.*s", (int)k, r->name.p ? r->name.p : "");
            }

            r->name.n = 0;
            g->dest = g->dest == D_FONT_ENTRY ? D_FONTTBL : D_STYLESHEET;
        } else if (cp < 0x80 && (cp != ' ' || r->name.n > 0)) {
            pb_putc(&r->name, (char)cp);
        }

        return;
    }

    if (g->dest == D_FLDINST) {
        if (cp < 0x80) {
            pb_putc(&r->fldinst, (char)cp);
        }

        return;
    }

    if (g->dest == D_PICT) {
        if (isxdigit((int)cp)) {
            pb_putc(&r->hex, (char)cp);
        }

        return;
    }

    if ((g->dest != D_TEXT && g->dest != D_FOOTNOTE && g->dest != D_FLDRSLT) || g->hidden || r->list_text) {
        return;
    }

    if (cp >= 0xD800 && cp < 0xDC00) {
        r->high_surrogate = (uint16_t)cp;
        return;
    }

    if (cp >= 0xDC00 && cp < 0xE000) {
        if (!r->high_surrogate) {
            return;
        }

        cp = 0x10000 + (((uint32_t)r->high_surrogate - 0xD800) << 10) + (cp - 0xDC00);
        r->high_surrogate = 0;
    }

    if (!r->b->para) {
        ri_begin_para(r);
    }

    ri_format(r);

    if (cp < 0x80) {
        u[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }

    bld_text(r->b, u, (size_t)n);
    r->para_has_text = 1;
}

static uint32_t cp1252(unsigned c) {
    static const uint16_t w[32] = {
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,
        0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
        0x0153, 0x9D, 0x017E, 0x0178
    };
    return c >= 0x80 && c < 0xA0 ? w[c - 0x80] : c;
}

/* end of a paragraph: \par, \cell, \row */
static void ri_par(ri* r) {
    rgroup* g = G(r);

    if (g->dest != D_TEXT && g->dest != D_FOOTNOTE && g->dest != D_FLDRSLT) {
        return;
    }

    if (!r->b->para) {
        ri_begin_para(r);   /* an empty paragraph is a paragraph too */
    }

    bld_end_para(r->b);
}

static void ri_finish_pict(ri* r) {
    size_t n = r->hex.n / 2, i;
    unsigned char* data;
    pd_inline o;

    if (!r->pict_mime[0] || n == 0 || (data = (unsigned char*)malloc(n)) == NULL) {
        r->hex.n = 0;
        return;
    }

    for (i = 0; i < n; i++) {
        unsigned v;

        sscanf(r->hex.p + 2 * i, "%2x", &v);
        data[i] = (unsigned char)v;
    }

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.width = (pd_sp)((int64_t)(r->picw_goal > 0 ? r->picw_goal : r->picw * 15) * 65536 / 20 * r->scalex / 100);
    o.height = (pd_sp)((int64_t)(r->pich_goal > 0 ? r->pich_goal : r->pich * 15) * 65536 / 20 * r->scaley / 100);

    if (o.width > 0 && o.height > 0 && pd_doc_add_resource(r->b->d, r->pict_mime, data, n, &o.resource) == PD_OK) {
        if (!r->b->para) {
            ri_begin_para(r);
        }

        bld_inline(r->b, &o);
    }

    free(data);
    r->hex.n = 0;
}

static void ri_word(ri* r, const char* w, int has_num, int num) {
    rgroup* g = G(r);

#define IS(s) (strcmp(w, s) == 0)

    /* destinations */
    if (IS("fonttbl")) {
        g->dest = D_FONTTBL;
    } else if (IS("colortbl")) {
        g->dest = D_COLORTBL;
        r->ncolors = 0;
        r->color_set = 0;
    } else if (IS("stylesheet")) {
        g->dest = D_STYLESHEET;
    } else if (IS("pict")) {
        g->dest = D_PICT;
        r->hex.n = 0;
        r->pict_mime[0] = '\0';
        r->picw_goal = r->pich_goal = r->picw = r->pich = 0;
        r->scalex = r->scaley = 100;
    } else if (IS("footnote")) {
        if (g->dest == D_TEXT || g->dest == D_FLDRSLT) {
            pd_char_props keep = r->b->cp;

            if (!r->b->para) {
                ri_begin_para(r);
            }

            bld_footnote_begin(r->b);
            r->b->cp = keep;
            g->dest = D_FOOTNOTE;
            g->note_end = 1;
            g->shift = 0;
            g->saved[0] = r->style;
            g->saved[1] = r->align;
            g->saved[2] = r->outline;
            g->saved[3] = r->intbl;
            g->saved[4] = r->ls;
            g->saved[5] = r->ilvl;
            r->intbl = 0;
        } else {
            g->dest = D_SKIP;
        }
    } else if (IS("field")) {
        g->field_end = 1;
        r->fldinst.n = 0;
    } else if (IS("fldinst")) {
        g->dest = D_FLDINST;
        r->fldinst.n = 0;
    } else if (IS("fldrslt")) {
        size_t k;
        const char* s = r->fldinst.p;

        g->dest = r->depth > 0 && r->g[r->depth - 1].dest == D_FOOTNOTE ? D_FOOTNOTE : D_TEXT;

        for (k = 0; s && k + 9 < r->fldinst.n; k++) {
            if (strncmp(s + k, "HYPERLINK", 9) == 0) {
                const char* q = strchr(s + k, '"');
                const char* e = q ? strchr(q + 1, '"') : NULL;

                if (q && e && e > q + 1) {
                    pd_inline o;

                    memset(&o, 0, sizeof(o));
                    o.kind = PD_INLINE_LINK;
                    o.source = q + 1;
                    o.source_len = (int32_t)(e - q - 1);

                    if (!r->b->para) {
                        ri_begin_para(r);
                    }

                    bld_inline(r->b, &o);
                    r->link_open = 1;
                }

                break;
            }
        }
    } else if (IS("listtable")) {
        g->dest = D_LISTTABLE;
    } else if (IS("listoverridetable")) {
        g->dest = D_LISTOVERRIDE;
    } else if (IS("info") || IS("header") || IS("footer") || IS("headerl") || IS("headerr") || IS("headerf") ||
               IS("footerl") || IS("footerr") || IS("footerf") || IS("xmlnstbl") || IS("rsidtbl") || IS("generator") ||
               IS("themedata") || IS("colorschememapping") || IS("latentstyles") || IS("datastore") ||
               IS("mmathPr") || IS("pgdsctbl") || IS("bkmkstart") || IS("bkmkend") || IS("object")) {
        g->dest = D_SKIP;
    } else if (IS("nonshppict") || IS("pntext") || IS("listtext")) {
        r->list_text = 1;
        g->dest = D_SKIP;
    }

    /* tables of the header */
    else if (g->dest == D_COLORTBL) {
        if (IS("red")) {
            r->color_parts[0] = num;
            r->color_set = 1;
        } else if (IS("green")) {
            r->color_parts[1] = num;
            r->color_set = 1;
        } else if (IS("blue")) {
            r->color_parts[2] = num;
            r->color_set = 1;
        }
    } else if ((g->dest == D_FONTTBL || g->dest == D_FONT_ENTRY) && IS("f")) {
        g->dest = D_FONT_ENTRY;

        if (r->nfonts < 64) {
            r->font_num[r->nfonts] = num;
            r->name.n = 0;
        }
    } else if ((g->dest == D_STYLESHEET || g->dest == D_STYLE_ENTRY) && IS("s")) {
        g->dest = D_STYLE_ENTRY;
        r->nstyles_seen = num;
        r->name.n = 0;
    } else if ((g->dest == D_STYLESHEET || g->dest == D_STYLE_ENTRY) && (IS("cs") || IS("ds") || IS("ts"))) {
        g->dest = D_SKIP;
    } else if (g->dest == D_LISTTABLE) {
        if (IS("list") && r->nlists < 16) {
            memset(&r->lists[r->nlists], 0, sizeof(rlist));
            r->cur_list = r->nlists++;
            r->cur_level = -1;
        } else if (IS("listlevel") && r->cur_level < 8) {
            r->cur_level++;
        } else if (IS("levelnfc") && r->cur_level >= 0 && r->nlists > 0) {
            r->lists[r->cur_list].kind[r->cur_level] = num == 23 || num == 255 ? 1 : 2;
        } else if (IS("listid") && r->nlists > 0) {
            r->lists[r->cur_list].list_id = num;
        }
    } else if (g->dest == D_LISTOVERRIDE) {
        if (IS("listid")) {
            r->cur_list = num;
        } else if (IS("ls") && num > 0 && num < 32) {
            r->ls_to_list[num] = r->cur_list;
        }
    } else if (g->dest == D_PICT) {
        if (IS("pngblip")) {
            strcpy(r->pict_mime, "image/png");
        } else if (IS("jpegblip")) {
            strcpy(r->pict_mime, "image/jpeg");
        } else if (IS("picwgoal")) {
            r->picw_goal = num;
        } else if (IS("pichgoal")) {
            r->pich_goal = num;
        } else if (IS("picw")) {
            r->picw = num;
        } else if (IS("pich")) {
            r->pich = num;
        } else if (IS("picscalex")) {
            r->scalex = num > 0 ? num : 100;
        } else if (IS("picscaley")) {
            r->scaley = num > 0 ? num : 100;
        }
    }

    /* character formatting (group scoped) */
    else if (IS("plain")) {
        g->bold = g->italic = g->ul = g->strike = g->shift = g->font = g->cf = g->bg = g->hidden = 0;
        g->caps = g->scaps = 0;
        g->fs = 24;
    } else if (IS("b")) {
        g->bold = !has_num || num != 0;
    } else if (IS("i")) {
        g->italic = !has_num || num != 0;
    } else if (IS("ul")) {
        g->ul = !has_num || num != 0;
    } else if (IS("ulw")) {
        g->ul = PD_UNDERLINE_WORDS;
    } else if (IS("uld")) {
        g->ul = PD_UNDERLINE_DOTTED;
    } else if (IS("uldash") || IS("uldashd") || IS("uldashdd") || IS("ulldash")) {
        g->ul = PD_UNDERLINE_DASHED;
    } else if (IS("ulth") || IS("ulthd") || IS("ulthdash")) {
        g->ul = PD_UNDERLINE_THICK;
    } else if (IS("ulwave") || IS("ulhwave")) {
        g->ul = PD_UNDERLINE_WAVY;
    } else if (IS("uldb") || IS("ululdbwave")) {
        g->ul = PD_UNDERLINE_DOUBLE;
    } else if (IS("caps")) {
        g->caps = !has_num || num != 0;
    } else if (IS("scaps")) {
        g->scaps = !has_num || num != 0;
    } else if (IS("ulnone")) {
        g->ul = 0;
    } else if (IS("strike") || IS("striked")) {
        g->strike = !has_num || num != 0;
    } else if (IS("super")) {
        g->shift = PD_SHIFT_SUPER;
    } else if (IS("sub")) {
        g->shift = PD_SHIFT_SUB;
    } else if (IS("nosupersub")) {
        g->shift = 0;
    } else if (IS("up")) {
        g->shift = num > 0 ? PD_SHIFT_SUPER : 0;
    } else if (IS("dn")) {
        g->shift = num > 0 ? PD_SHIFT_SUB : 0;
    } else if (IS("f")) {
        g->font = num;
    } else if (IS("fs")) {
        g->fs = num;
    } else if (IS("cf")) {
        g->cf = num;
    } else if (IS("highlight") || IS("chcbpat") || IS("cb")) {
        g->bg = num;
    } else if (IS("v")) {
        g->hidden = !has_num || num != 0;
    } else if (IS("uc")) {
        g->uc = num;
    }

    /* paragraphs */
    else if (IS("pard")) {
        r->style = 0;
        r->align = -1;
        r->rtl = -1;
        r->outline = -1;
        r->intbl = 0;
        r->ls = 0;
        r->ilvl = 0;
        r->li = 0;
    } else if (IS("s")) {
        r->style = num;
    } else if (IS("ql")) {
        r->align = PD_ALIGN_LEFT;
    } else if (IS("qc")) {
        r->align = PD_ALIGN_CENTER;
    } else if (IS("qr")) {
        r->align = PD_ALIGN_RIGHT;
    } else if (IS("qj")) {
        r->align = PD_ALIGN_JUSTIFY;
    } else if (IS("rtlpar")) {
        r->rtl = 1;
    } else if (IS("ltrpar")) {
        r->rtl = 0;
    } else if (IS("taprtl")) {
        r->row_rtl = 1;
    } else if (IS("outlinelevel")) {
        r->outline = num;
    } else if (IS("intbl")) {
        r->intbl = 1;
    } else if (IS("ls")) {
        r->ls = num;
    } else if (IS("ilvl")) {
        r->ilvl = num < 0 ? 0 : num > 8 ? 8 : num;
    } else if (IS("pnlvlblt")) {
        r->ls = r->ls ? r->ls : 30;
    } else if (IS("par")) {
        if (g->dest == D_TEXT && r->table_open && !r->intbl) {
            ri_close_table(r);
        }

        ri_par(r);
    } else if (IS("line")) {
        ri_char(r, '\n');
    } else if (IS("tab")) {
        ri_char(r, '\t');
    } else if (IS("page")) {
        bld_end_para(r->b);
        ri_close_table(r);
        bld_break(r->b, PD_BREAK_PAGE);
    } else if (IS("emdash")) {
        ri_char(r, 0x2014);
    } else if (IS("endash")) {
        ri_char(r, 0x2013);
    } else if (IS("bullet")) {
        ri_char(r, 0x2022);
    } else if (IS("lquote")) {
        ri_char(r, 0x2018);
    } else if (IS("rquote")) {
        ri_char(r, 0x2019);
    } else if (IS("ldblquote")) {
        ri_char(r, 0x201C);
    } else if (IS("rdblquote")) {
        ri_char(r, 0x201D);
    } else if (IS("emspace") || IS("enspace") || IS("qmspace")) {
        ri_char(r, ' ');
    } else if (IS("u")) {
        ri_char(r, (uint32_t)(num < 0 ? num + 65536 : num));
        r->skip_chars = g->uc;
    } else if (IS("ansicpg")) {
        r->codepage = num;
    }

    /* tables */
    else if (IS("trowd")) {
        r->ncelldefs = 0;
        r->row_rtl = 0;
        r->row_header = 0;
        memset(r->cell_bg, 0, sizeof(r->cell_bg));
        memset(r->cell_merge, 0, sizeof(r->cell_merge));
    } else if (IS("trhdr")) {
        r->row_header = 1;
    } else if (IS("clcbpat") && r->ncelldefs < 64) {
        r->cell_bg[r->ncelldefs] = num;
    } else if (IS("clmgf") && r->ncelldefs < 64) {
        r->cell_merge[r->ncelldefs] = 1;
    } else if (IS("clmrg") && r->ncelldefs < 64) {
        r->cell_merge[r->ncelldefs] = 2;
    } else if (IS("cellx")) {
        r->ncelldefs++;
    } else if (IS("cell")) {
        if (!r->cell_open && r->pending_span > 0) {     /* a merged continuation cell */
            r->pending_span--;
            r->ncells_row++;
            return;
        }

        if (!r->table_open || !r->cell_open) {
            r->intbl = 1;
            ri_begin_para(r);
        }

        bld_end_para(r->b);
        bld_cell_end(r->b);
        r->cell_open = 0;
        r->ncells_row++;
    } else if (IS("row")) {
        if (r->cell_open) {
            bld_end_para(r->b);
            bld_cell_end(r->b);
            r->cell_open = 0;
        }

        r->row_open = 0;
    } else if (IS("sect")) {
        bld_end_para(r->b);
        ri_close_table(r);
    }

#undef IS
}

pd_status pd_rtf_import(pd_doc* d, const char* s, size_t n) {
    pd_bld b;
    ri* r = (ri*)calloc(1, sizeof(ri));
    size_t i = 0;
    pd_status st;

    if (!r) {
        return PD_ERR_NOMEM;
    }

    if (n < 5 || memcmp(s, "{\\rtf", 5) != 0) {
        free(r);
        return PD_ERR_FORMAT;
    }

    bld_init(&b, d);
    r->b = &b;
    r->g[0].dest = D_TEXT;
    r->g[0].uc = 1;
    r->g[0].fs = 24;
    r->style = 0;
    r->align = -1;
    r->rtl = -1;
    r->outline = -1;

    while (i < n) {
        char c = s[i];

        if (c == '{') {
            if (r->depth < 255) {
                r->g[r->depth + 1] = r->g[r->depth];
                r->g[r->depth + 1].note_end = 0;
                r->g[r->depth + 1].field_end = 0;

                if (r->g[r->depth].dest == D_FONTTBL || r->g[r->depth].dest == D_STYLESHEET) {
                    r->name.n = 0;
                }
            }

            r->depth++;
            i++;
            continue;
        }

        if (c == '}') {
            if (r->depth > 0 && r->depth <= 255) {
                rgroup* g = G(r);

                if (g->dest == D_PICT && (r->depth == 0 || r->g[r->depth - 1].dest != D_PICT)) {
                    ri_finish_pict(r);
                }

                if (g->note_end) {
                    bld_end_para(&b);
                    bld_footnote_end(&b);
                    r->style = g->saved[0];
                    r->align = g->saved[1];
                    r->outline = g->saved[2];
                    r->intbl = g->saved[3];
                    r->ls = g->saved[4];
                    r->ilvl = g->saved[5];
                }

                if (g->field_end && r->link_open) {
                    pd_inline o;

                    memset(&o, 0, sizeof(o));
                    o.kind = PD_INLINE_LINK;
                    bld_inline(&b, &o);
                    r->link_open = 0;
                }

                if (g->dest == D_SKIP && r->list_text && r->g[r->depth - 1].dest != D_SKIP) {
                    r->list_text = 0;
                }
            }

            r->depth--;

            if (r->depth < 0) {
                break;
            }

            i++;
            continue;
        }

        if (c == '\\' && i + 1 < n) {
            char d1 = s[i + 1];

            if (isalpha((unsigned char)d1)) {
                char w[32];
                size_t k = 0;
                int has_num = 0, num = 0, neg = 0;

                i++;

                while (i < n && isalpha((unsigned char)s[i]) && k + 1 < sizeof(w)) {
                    w[k++] = s[i++];
                }

                w[k] = '\0';

                if (i < n && (s[i] == '-' || isdigit((unsigned char)s[i]))) {
                    neg = s[i] == '-';
                    i += neg;
                    has_num = 1;

                    while (i < n && isdigit((unsigned char)s[i])) {
                        num = num < 100000000 ? num * 10 + (s[i] - '0') : num;
                        i++;
                    }

                    num = neg ? -num : num;
                }

                if (i < n && s[i] == ' ') {
                    i++;
                }

                if (r->depth >= 0 && r->depth <= 255) {
                    if (r->skip_chars > 0 && strcmp(w, "u") != 0) {
                        r->skip_chars = 0;  /* a control word ends the fallback */
                    }

                    ri_word(r, w, has_num, num);
                }

                continue;
            }

            if (d1 == '\'' && i + 3 < n) {
                unsigned v = 0;

                sscanf(s + i + 2, "%2x", &v);
                i += 4;

                if (r->skip_chars > 0) {
                    r->skip_chars--;
                } else if (r->depth >= 0 && r->depth <= 255) {
                    ri_char(r, cp1252(v));
                }

                continue;
            }

            if (d1 == '*') {    /* an ignorable destination, unless known */
                size_t k = i + 2;
                char w[32];
                size_t wl = 0;

                while (k < n && (s[k] == ' ' || s[k] == '\r' || s[k] == '\n')) {
                    k++;
                }

                if (k + 1 < n && s[k] == '\\') {
                    k++;

                    while (k < n && isalpha((unsigned char)s[k]) && wl + 1 < sizeof(w)) {
                        w[wl++] = s[k++];
                    }
                }

                w[wl] = '\0';

                if (r->depth >= 0 && r->depth <= 255 && strcmp(w, "fldinst") && strcmp(w, "listtable") &&
                        strcmp(w, "listoverridetable") && strcmp(w, "shppict")) {
                    G(r)->dest = D_SKIP;
                }

                i += 2;
                continue;
            }

            if (r->depth >= 0 && r->depth <= 255) {
                if (d1 == '\\' || d1 == '{' || d1 == '}') {
                    ri_char(r, (uint32_t)d1);
                } else if (d1 == '~') {
                    ri_char(r, 0xA0);
                } else if (d1 == '_') {
                    ri_char(r, 0x2011);
                } else if (d1 == '-') {
                    ri_char(r, 0xAD);
                } else if (d1 == '\n' || d1 == '\r') {
                    ri_word(r, "par", 0, 0);
                }
            }

            i += 2;
            continue;
        }

        if (c == '\r' || c == '\n') {
            i++;
            continue;
        }

        if (r->depth >= 0 && r->depth <= 255) {
            if (G(r)->dest == D_COLORTBL && c == ';') {
                if (r->ncolors < 256) {
                    r->colors[r->ncolors++] = r->color_set ? 0xFF000000u | ((uint32_t)r->color_parts[0] << 16) |
                                              ((uint32_t)r->color_parts[1] << 8) | (uint32_t)r->color_parts[2] : 0xFF000000u;
                }

                r->color_set = 0;
                memset(r->color_parts, 0, sizeof(r->color_parts));
            } else if (r->skip_chars > 0) {
                r->skip_chars--;
            } else {
                unsigned char uc = (unsigned char)c;

                ri_char(r, uc < 0x80 ? uc : cp1252(uc));
            }
        }

        i++;
    }

    bld_end_para(&b);
    ri_close_table(r);
    pb_free(&r->hex);
    pb_free(&r->fldinst);
    pb_free(&r->name);
    free(r);
    st = bld_finish(&b);
    return st;
}
