/*
 * pd_dump: lay out a document and dump its pages as JSON for the viewer
 *
 * usage: pd_dump [in.pdoc|in.bpdoc] > pages.json
 * Without an input it builds a sample document (and saves it next to the
 * output as build/sample.pdoc) exercising headings, lists, styles, floats,
 * fields, a footnote mark, headers/footers, breaks and a two-column section.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_layout.h"

#define FONT_DIR "/usr/share/fonts/truetype/"

static const char* font_paths[] = {
    FONT_DIR "liberation/LiberationSerif-Regular.ttf",
    FONT_DIR "liberation/LiberationSerif-Bold.ttf",
    FONT_DIR "liberation/LiberationSerif-Italic.ttf",
    FONT_DIR "liberation/LiberationSerif-BoldItalic.ttf",
    FONT_DIR "dejavu/DejaVuSansMono.ttf"
};

#define NFONTS 5
static pd_font* fonts[NFONTS];
static pd_font* mathfont;

static const pd_font* resolve(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;

    if (family && strcmp(family, "monospace") == 0) {
        return fonts[4];
    }

    return fonts[(weight >= 600 ? 1 : 0) + (italic ? 2 : 0)];
}

static const char* frog =
    "In olden times when wishing still helped one, there lived a king whose daughters were all beautiful, "
    "but the youngest was so beautiful that the sun itself, which has seen so much, was astonished whenever "
    "it shone in her face. Close by the king's castle lay a great dark forest, and under an old lime-tree in "
    "the forest was a well, and when the day was very warm, the king's child went out into the forest and sat "
    "down by the side of the cool fountain; and when she was bored she took a golden ball, and threw it up on "
    "high and caught it; and this ball was her favorite plaything.";

static const char* light =
    "Light propagation in biological tissue is dominated by scattering. Photons random-walk through the "
    "medium, losing direction after a few transport mean free paths, so that deep tissue is reached only by "
    "diffuse light. The radiative transfer equation describes this process exactly, and Monte Carlo methods "
    "solve it by following many independent photon packets, each a sample of the underlying stochastic "
    "process. Because packets do not interact, the method parallelizes almost perfectly on graphics hardware.";

typedef struct {
    pd_doc* d;
    pd_block_id sec;
} ctx;

static pd_block_id para(ctx* c, const char* style, int role, int level, const char* text) {
    pd_block_id p;
    pd_pos at;

    pd_doc_insert_block(c->d, c->sec, -1, PD_BLOCK_PARAGRAPH, &p);

    if (style) {
        pd_doc_set_para_style(c->d, p, pd_doc_style_find(c->d, style));
    }

    pd_doc_set_role(c->d, p, (pd_role)role, level);
    at.block = p;
    at.offset = 0;
    pd_doc_insert_text(c->d, at, text, strlen(text), PD_FORMAT_INHERIT, NULL);
    return p;
}

static pd_pos end_of(const pd_doc* d, pd_block_id p) {
    pd_pos at;
    const char* t;
    uint32_t n;

    pd_doc_para_text(d, p, &t, &n);
    at.block = p;
    at.offset = n;
    return at;
}

static void add_field(pd_doc* d, pd_pos at, int field, const char* seq, pd_block_id target) {
    pd_inline o;

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = field;
    o.target = target;

    if (seq) {
        strcpy(o.name, seq);
    }

    pd_doc_insert_inline(d, at, &o, NULL);
}

static pd_block_id figure(ctx* c, const char* caption, pd_res_id res, pd_sp w, pd_sp h, uint32_t placement) {
    pd_block_id fl, img, cap;
    pd_float_props fp;
    pd_inline o;
    pd_pos at;

    pd_doc_insert_block(c->d, c->sec, -1, PD_BLOCK_FLOAT, &fl);
    pd_doc_float_props(c->d, fl, &fp);
    fp.placement = placement;
    fp.width_fraction = 900;
    pd_doc_set_float_props(c->d, fl, &fp);
    img = pd_doc_child(c->d, fl, 0);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.resource = res;
    o.width = w;
    o.height = h;
    at.block = img;
    at.offset = 0;
    pd_doc_insert_inline(c->d, at, &o, NULL);
    {
        pd_para_props pp;

        memset(&pp, 0, sizeof(pp));
        pp.mask = PD_PP_ALIGN;
        pp.align = PD_ALIGN_CENTER;
        pd_doc_set_para_props(c->d, img, &pp);
    }
    pd_doc_insert_block(c->d, fl, -1, PD_BLOCK_PARAGRAPH, &cap);
    pd_doc_set_para_style(c->d, cap, pd_doc_style_find(c->d, "Caption"));
    pd_doc_set_role(c->d, cap, PD_ROLE_CAPTION, 0);
    pd_doc_insert_text(c->d, end_of(c->d, cap), "Figure ", 7, PD_FORMAT_INHERIT, NULL);
    add_field(c->d, end_of(c->d, cap), PD_FIELD_SEQ, "Figure", 0);
    pd_doc_insert_text(c->d, end_of(c->d, cap), caption, strlen(caption), PD_FORMAT_INHERIT, NULL);
    return fl;
}

static pd_doc* sample(void) {
    ctx c;
    pd_block_id p, hdr, ftr, first, fig1, fig2, sec2;
    pd_section_props sp;
    pd_char_props cp;
    pd_para_props pp;
    pd_list_level lv[2];
    pd_list_id num, bul;
    pd_res_id res;
    pd_range r;
    pd_pos at;
    int i;

    pd_doc_new(&c.d);
    c.sec = pd_doc_child(c.d, pd_doc_root(c.d), 0);
    first = pd_doc_child(c.d, c.sec, 0);
    pd_doc_add_resource(c.d, "image/png", "placeholder", 11, &res);

    /* Normal: justified, first-line indent, 10.5pt */
    memset(&pp, 0, sizeof(pp));
    pp.mask = PD_PP_ALIGN | PD_PP_INDENT_FIRST | PD_PP_SPACE_AFTER;
    pp.align = PD_ALIGN_JUSTIFY;
    pp.indent_first = PD_PT(14);
    pp.space_after = PD_PT(4);
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_SIZE;
    cp.size = PD_PT(10.5);
    pd_doc_style_define(c.d, "Normal", PD_STYLE_PARAGRAPH, 0, &pp, &cp, NULL);

    /* header and footer */
    pd_doc_insert_block(c.d, 0, -1, PD_BLOCK_STORY, &hdr);
    p = pd_doc_child(c.d, hdr, 0);
    memset(&pp, 0, sizeof(pp));
    pp.mask = PD_PP_ALIGN;
    pp.align = PD_ALIGN_RIGHT;
    pd_doc_set_para_props(c.d, p, &pp);
    pd_doc_insert_text(c.d, end_of(c.d, p), "Parade sample \xE2\x80\x94 ", 18, PD_FORMAT_INHERIT, NULL);
    {
        pd_inline o;

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_FIELD;
        o.field = PD_FIELD_HEADING;
        o.level = 1;
        pd_doc_insert_inline(c.d, end_of(c.d, p), &o, NULL);
    }
    pd_doc_insert_block(c.d, 0, -1, PD_BLOCK_STORY, &ftr);
    p = pd_doc_child(c.d, ftr, 0);
    pp.align = PD_ALIGN_CENTER;
    pd_doc_set_para_props(c.d, p, &pp);
    pd_doc_insert_text(c.d, end_of(c.d, p), "Page ", 5, PD_FORMAT_INHERIT, NULL);
    add_field(c.d, end_of(c.d, p), PD_FIELD_PAGE, NULL, 0);
    pd_doc_insert_text(c.d, end_of(c.d, p), " of ", 4, PD_FORMAT_INHERIT, NULL);
    add_field(c.d, end_of(c.d, p), PD_FIELD_PAGES, NULL, 0);
    pd_doc_section_props(c.d, c.sec, &sp);
    sp.header = hdr;
    sp.footer = ftr;
    pd_doc_set_section_props(c.d, c.sec, &sp);

    /* title and introduction */
    pd_doc_set_para_style(c.d, first, pd_doc_style_find(c.d, "Title"));
    pd_doc_set_role(c.d, first, PD_ROLE_TITLE, 0);
    pd_doc_insert_text(c.d, end_of(c.d, first), "A Box, Glue and Penalty Document", 32, PD_FORMAT_INHERIT, NULL);
    para(&c, "Heading 1", PD_ROLE_HEADING, 1, "Introduction");
    p = para(&c, NULL, 0, 0, frog);

    /* character formatting in one paragraph */
    p = para(&c, NULL, 0, 0, "Character formats: bold, italic, underlined and struck text, a superscript "
             "x2 and a subscript H2O, and an inline code span, all within one justified paragraph that wraps.");
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_WEIGHT;
    cp.weight = 700;
    r.start.block = r.end.block = p;
    r.start.offset = 19;
    r.end.offset = 23;
    pd_doc_set_char_props(c.d, r, &cp);
    cp.mask = PD_CP_ITALIC;
    cp.italic = 1;
    r.start.offset = 25;
    r.end.offset = 31;
    pd_doc_set_char_props(c.d, r, &cp);
    cp.mask = PD_CP_UNDERLINE;
    cp.underline = 1;
    r.start.offset = 33;
    r.end.offset = 43;
    pd_doc_set_char_props(c.d, r, &cp);
    cp.mask = PD_CP_STRIKE;
    cp.strike = 1;
    r.start.offset = 48;
    r.end.offset = 54;
    pd_doc_set_char_props(c.d, r, &cp);
    cp.mask = PD_CP_SHIFT;
    cp.shift = PD_SHIFT_SUPER;
    r.start.offset = 76;
    r.end.offset = 77;
    pd_doc_set_char_props(c.d, r, &cp);
    cp.shift = PD_SHIFT_SUB;
    r.start.offset = 95;
    r.end.offset = 96;
    pd_doc_set_char_props(c.d, r, &cp);
    cp.mask = PD_CP_FAMILY;
    strcpy(cp.family, "monospace");
    r.start.offset = 113;
    r.end.offset = 122;
    pd_doc_set_char_props(c.d, r, &cp);

    fig1 = figure(&c, ": a here-or-top float with its caption number from a SEQ field.", res, PD_PT(300),
                  PD_PT(140), PD_PLACE_HERE | PD_PLACE_TOP | PD_PLACE_BOTTOM | PD_PLACE_PAGE);
    para(&c, "Heading 2", PD_ROLE_HEADING, 2, "Lists");

    memset(lv, 0, sizeof(lv));
    lv[0].format = PD_NUM_DECIMAL;
    lv[0].start = 1;
    strcpy(lv[0].text, "%1.");
    lv[0].indent = PD_PT(24);
    lv[0].hanging = PD_PT(18);
    lv[1].format = PD_NUM_LOWER_ALPHA;
    lv[1].start = 1;
    strcpy(lv[1].text, "%1.%2)");
    lv[1].indent = PD_PT(48);
    lv[1].hanging = PD_PT(24);
    pd_doc_list_define(c.d, 2, lv, &num);
    lv[0].format = PD_NUM_BULLET;
    strcpy(lv[0].text, "\xE2\x80\xA2");
    pd_doc_list_define(c.d, 1, lv, &bul);
    {
        static const char long_item[] = "Break paragraphs with the total-fit algorithm, which balances "
                                        "spacing over the whole paragraph rather than line by line.";
        const char* items[] = { "Shape the text into glyphs with kerning.", long_item,
                                "Cut columns with widow and orphan rules.", "Place floats.", "Draw."
                              };
        int lvl[] = { 0, 0, 1, 1, 0 };

        for (i = 0; i < 5; i++) {
            p = para(&c, NULL, 0, 0, items[i]);
            memset(&pp, 0, sizeof(pp));
            pp.mask = PD_PP_INDENT_FIRST | PD_PP_SPACE_AFTER;
            pd_doc_set_para_props(c.d, p, &pp);
            pd_doc_set_list(c.d, p, num, lvl[i]);
        }

        for (i = 0; i < 3; i++) {
            p = para(&c, NULL, 0, 0, i == 1 ? "A bullet item long enough to wrap onto a second line, showing that "
                     "continuation lines align with the text and not with the bullet." : "A bullet item.");
            pd_doc_set_para_props(c.d, p, &pp);
            pd_doc_set_list(c.d, p, bul, 0);
        }
    }

    para(&c, "Heading 1", PD_ROLE_HEADING, 1, "Floats and References");
    p = para(&c, NULL, 0, 0, "As shown in Figure ");
    add_field(c.d, end_of(c.d, p), PD_FIELD_REF_NUMBER, NULL, fig1);
    pd_doc_insert_text(c.d, end_of(c.d, p), " on page ", 9, PD_FORMAT_INHERIT, NULL);
    add_field(c.d, end_of(c.d, p), PD_FIELD_REF_PAGE, NULL, fig1);
    pd_doc_insert_text(c.d, end_of(c.d, p), ", fields resolve after pagination. ", 35, PD_FORMAT_INHERIT, NULL);
    pd_doc_insert_text(c.d, end_of(c.d, p), light, strlen(light), PD_FORMAT_INHERIT, NULL);

    for (i = 0; i < 4; i++) {
        para(&c, NULL, 0, 0, i % 2 ? frog : light);
    }

    fig2 = figure(&c, ": a large top-only float, deferred to the top of the next page.", res, PD_PT(380),
                  PD_PT(260), PD_PLACE_TOP | PD_PLACE_PAGE);
    (void)fig2;

    for (i = 0; i < 6; i++) {
        para(&c, NULL, 0, 0, i % 2 ? light : frog);
    }

    /* footnotes, a table, and text wrapped beside a figure */
    para(&c, "Heading 1", PD_ROLE_HEADING, 1, "Tables, Notes and Wrapping");
    p = para(&c, NULL, 0, 0, "Footnote bodies are set at the bottom of the column that shows their mark");
    {
        pd_block_id note;
        pd_inline o;

        pd_doc_insert_block(c.d, 0, -1, PD_BLOCK_STORY, &note);
        static const char nt[] = "The page builder reserves room for the note together with the line that "
                                 "refers to it, as TeX does with insertions.";

        pd_doc_insert_text(c.d, end_of(c.d, pd_doc_child(c.d, note, 0)), nt, strlen(nt), PD_FORMAT_INHERIT, NULL);
        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_FOOTNOTE;
        o.target = note;
        pd_doc_insert_inline(c.d, end_of(c.d, p), &o, NULL);
        pd_doc_insert_text(c.d, end_of(c.d, p), ", and tables take their column widths from their content.", 57,
                           PD_FORMAT_INHERIT, NULL);
    }
    {
        static const char* names[] = { "Method", "Lines", "Notes" };
        static const char* rows[][3] = {
            { "first-fit", "11", "fast, uneven spacing" },
            { "total-fit", "10", "Knuth and Plass: the whole paragraph at once" },
            { "total-fit + looseness", "11", "a paragraph variant, one line longer" },
            { "greedy pages", "-", "best break on each column" },
            { "optimal pages", "-", "all column breaks of a section chosen together" },
        };
        pd_block_id t, row, cell;
        pd_table_props tp;
        pd_cell_props cep;
        int ri, k;

        pd_doc_insert_block(c.d, c.sec, -1, PD_BLOCK_TABLE, &t);
        pd_doc_table_props(c.d, t, &tp);
        tp.header_rows = 1;
        tp.align = PD_ALIGN_CENTER;
        pd_doc_set_table_props(c.d, t, &tp);

        for (ri = 0; ri < 7; ri++) {
            if (ri == 0) {
                row = pd_doc_child(c.d, t, 0);
            } else {
                pd_doc_insert_block(c.d, t, -1, PD_BLOCK_ROW, &row);
            }

            for (k = 0; k < (ri == 6 ? 1 : 3); k++) {
                const char* txt = ri == 0 ? names[k] : ri == 6 ? "A cell spanning all three columns, on a gray "
                                  "background." : rows[ri - 1][k];

                if (k == 0) {
                    cell = pd_doc_child(c.d, row, 0);
                } else {
                    pd_doc_insert_block(c.d, row, -1, PD_BLOCK_CELL, &cell);
                }

                pd_doc_insert_text(c.d, end_of(c.d, pd_doc_child(c.d, cell, 0)), txt, strlen(txt),
                                   PD_FORMAT_INHERIT, NULL);

                if (ri == 0 || ri == 6) {
                    pd_doc_cell_props(c.d, cell, &cep);
                    cep.background = ri == 0 ? 0xFFDDE4EEu : 0xFFEEEEEEu;
                    cep.col_span = ri == 6 ? 3 : 1;
                    pd_doc_set_cell_props(c.d, cell, &cep);
                }
            }
        }
    }
    {
        pd_block_id fl = figure(&c, ": wrapped.", res, PD_PT(140), PD_PT(110), PD_PLACE_HERE);
        pd_float_props fp;

        pd_doc_float_props(c.d, fl, &fp);
        fp.wrap = PD_WRAP_LEFT;
        fp.width = PD_PT(150);
        fp.gap = PD_PT(10);
        pd_doc_set_float_props(c.d, fl, &fp);
        para(&c, NULL, 0, 0, light);
        para(&c, NULL, 0, 0, frog);
    }

    p = para(&c, "Code", PD_ROLE_CODE, 0, "for (b = 0; b < nbreaks; b++)\n    best[b] = min over a of cost(a, b);");
    para(&c, "Quote", PD_ROLE_QUOTE, 0, "Whatever is worth doing at all is worth doing well.");

    /* mathematics, typeset with the math font */
    {
        static const char* inl = "e^{i\\pi} + 1 = 0";
        static const char* disp = "\\int_{-\\infty}^{\\infty} e^{-x^2}\\,dx = \\sqrt{\\pi}, \\qquad "
                                  "\\sum_{k=0}^{n} \\binom{n}{k} = 2^n";
        pd_inline o;

        p = para(&c, NULL, 0, 0, "Formulas are laid out the TeX way, inline like ");
        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_EQUATION;
        o.source = inl;
        o.source_len = (int32_t)strlen(inl);
        pd_doc_insert_inline(c.d, end_of(c.d, p), &o, NULL);
        pd_doc_insert_text(c.d, end_of(c.d, p), " or on a line of their own:", 27, PD_FORMAT_INHERIT, NULL);
        p = para(&c, NULL, PD_ROLE_EQUATION, 0, "");
        o.source = disp;
        o.source_len = (int32_t)strlen(disp);
        pd_doc_insert_inline(c.d, end_of(c.d, p), &o, NULL);
    }

    /* a second section: two columns, roman page numbers */
    pd_doc_insert_block(c.d, pd_doc_root(c.d), -1, PD_BLOCK_SECTION, &sec2);
    pd_doc_section_props(c.d, sec2, &sp);
    sp.columns = 2;
    sp.first_page_number = 1;
    sp.page_number_format = PD_NUM_LOWER_ROMAN;
    sp.header = hdr;
    sp.footer = ftr;
    pd_doc_set_section_props(c.d, sec2, &sp);
    c.sec = sec2;
    p = pd_doc_child(c.d, sec2, 0);
    pd_doc_set_para_style(c.d, p, pd_doc_style_find(c.d, "Heading 1"));
    pd_doc_set_role(c.d, p, PD_ROLE_HEADING, 1);
    at.block = p;
    at.offset = 0;
    pd_doc_insert_text(c.d, at, "Appendix in Two Columns", 23, PD_FORMAT_INHERIT, NULL);

    for (i = 0; i < 5; i++) {
        para(&c, NULL, 0, 0, i % 3 == 0 ? frog : light);
    }

    /* a continuous one-column section closes the page; the two columns above end even */
    {
        static const char ct[] = "A continuous section: it starts right below the balanced columns of the "
                                 "appendix instead of on a new page.";
        pd_block_id sec3;

        pd_doc_insert_block(c.d, pd_doc_root(c.d), -1, PD_BLOCK_SECTION, &sec3);
        pd_doc_section_props(c.d, sec3, &sp);
        sp.continuous = 1;
        sp.header = hdr;
        sp.footer = ftr;
        pd_doc_set_section_props(c.d, sec3, &sp);
        c.sec = sec3;
        p = pd_doc_child(c.d, sec3, 0);
        pd_doc_insert_text(c.d, end_of(c.d, p), ct, strlen(ct), PD_FORMAT_INHERIT, NULL);
    }

    return c.d;
}

static int to_file(void* user, const void* data, size_t len) {
    return fwrite(data, 1, len, (FILE*)user) != len;
}

int main(int argc, char** argv) {
    const char* pdf = NULL;
    pd_doc* d = NULL;
    pd_layout* L;
    pd_layout_info info;
    int32_t pg, i, k;

    if (argc > 2 && strcmp(argv[1], "--pdf") == 0) {     /* --pdf out.pdf [in.pdoc] */
        pdf = argv[2];
        argv += 2;
        argc -= 2;
    }

    for (i = 0; i < NFONTS; i++) {
        if (pd_font_load_file(font_paths[i], 0, &fonts[i]) != PD_OK) {
            fprintf(stderr, "cannot load %s\n", font_paths[i]);
            return 1;
        }
    }

    if (argc > 1) {
        FILE* fp = fopen(argv[1], "rb");
        long n;
        char* buf;

        if (!fp || fseek(fp, 0, SEEK_END) || (n = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET)) {
            fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }

        buf = (char*)malloc((size_t)n);

        if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n ||
                pd_doc_load(buf, (size_t)n, PD_JDATA_AUTO, &d) != PD_OK) {
            fprintf(stderr, "cannot load document %s\n", argv[1]);
            return 1;
        }

        fclose(fp);
        free(buf);
    } else {
        FILE* fp;

        d = sample();
        fp = fopen("build/sample.pdoc", "wb");

        if (fp) {
            pd_doc_save(d, PD_JDATA_TEXT, to_file, fp);
            fclose(fp);
        }
    }

    pd_doc_set_font_resolver(d, resolve, NULL);
    pd_doc_set_microtype(d, 1, 20);     /* margin kerning and font expansion, as with microtype */

    if (pd_font_load_file("/usr/share/texmf/fonts/opentype/public/lm-math/latinmodern-math.otf", 0, &mathfont) ==
            PD_OK) {
        pd_doc_set_math_font(d, mathfont);
    }

    if (pd_layout_new(d, &L) != PD_OK || pd_layout_update(L, &info) != PD_OK) {
        fprintf(stderr, "layout failed\n");
        return 1;
    }

    fprintf(stderr, "%d pages, %d paragraphs broken, %d float pages, %d overfull columns\n", info.pages,
            info.paragraphs_broken, info.float_pages, info.overfull);

    if (pdf) {
        FILE* fp = fopen(pdf, "wb");

        if (!fp || pd_layout_write_pdf(L, NULL, to_file, fp) != PD_OK) {
            fprintf(stderr, "cannot write %s\n", pdf);
            return 1;
        }

        fclose(fp);
        fprintf(stderr, "wrote %s\n", pdf);
    }
    printf("{\"fonts\": [");

    for (i = 0; i < NFONTS; i++) {
        printf("%s\"%s\"", i ? ", " : "", font_paths[i]);
    }

    printf("],\n\"pages\": [\n");

    for (pg = 0; pg < pd_layout_page_count(L); pg++) {
        pd_page_info pi;
        pd_draw* items;
        int32_t n = 0;

        pd_layout_page_info(L, pg, &pi);
        pd_layout_page_items(L, pg, NULL, 0, &n);
        items = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, pg, items, n, &n);
        printf("%s{\"w\": %d, \"h\": %d, \"label\": \"%s\", \"float_page\": %d, \"items\": [", pg ? ",\n" : "",
               pi.width, pi.height, pi.label, pi.float_page);

        for (k = 0; k < n; k++) {
            const pd_draw* a = &items[k];
            int f = -1;

            for (i = 0; i < NFONTS; i++) {
                if (a->font == fonts[i]) {
                    f = i;
                }
            }

            printf("%s[%d,%d,%d,%d,%d,%u,%d,%d,%u,%d]", k ? "," : "", a->kind, a->x, a->y, a->w, a->h, a->glyph, f,
                   a->size, a->color, a->region);
        }

        printf("]}");
        free(items);
    }

    printf("\n]}\n");
    pd_layout_free(L);
    pd_doc_free(d);

    for (i = 0; i < NFONTS; i++) {
        pd_font_free(fonts[i]);
    }

    return 0;
}
