/*
 * Parade import/export tests: every format round-trips a document with
 * headings, character formats, links, lists, quotes, code, a table, a
 * figure and a footnote; clipboard ranges and paste; malformed input.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_convert.h"

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef struct {
    char* p;
    size_t n;
} buf_t;

static int to_buf(void* user, const void* data, size_t len) {
    buf_t* b = (buf_t*)user;
    char* p = (char*)realloc(b->p, b->n + len + 1);

    if (!p) {
        return 1;
    }

    memcpy(p + b->n, data, len);
    b->p = p;
    b->n += len;
    b->p[b->n] = '\0';
    return 0;
}

static pd_pos at(pd_block_id b, uint32_t o) {
    pd_pos p;
    p.block = b;
    p.offset = o;
    return p;
}

static pd_pos end_of(const pd_doc* d, pd_block_id p) {
    const char* t;
    uint32_t n;

    pd_doc_para_text(d, p, &t, &n);
    return at(p, n);
}

static void text(pd_doc* d, pd_block_id p, const char* s) {
    pd_doc_insert_text(d, end_of(d, p), s, strlen(s), 0, NULL);     /* plain, not continuing the format before */
}

/* text with direct character properties */
static void ftext(pd_doc* d, pd_block_id p, const char* s, const pd_char_props* cp) {
    pd_doc_insert_text(d, end_of(d, p), s, strlen(s), pd_doc_format(d, 0, cp), NULL);
}

static pd_block_id para(pd_doc* d, pd_block_id parent, const char* style, int role, int level, const char* s) {
    pd_block_id p;

    pd_doc_insert_block(d, parent, -1, PD_BLOCK_PARAGRAPH, &p);

    if (style) {
        pd_doc_set_para_style(d, p, pd_doc_style_find(d, style));
    }

    if (role) {
        pd_doc_set_role(d, p, (pd_role)role, level);
    }

    if (s) {
        text(d, p, s);
    }

    return p;
}

static const unsigned char png1x1[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 0x49,
    0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB0,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

static pd_doc* rich(void) {
    pd_doc* d;
    pd_block_id sec, p, t, fl, story;
    pd_char_props cp;
    pd_list_level lv[2];
    pd_list_id bul, num;
    pd_inline o;
    pd_res_id res;
    pd_table_props tp;
    pd_cell_props cep;
    int r, c;

    pd_doc_new(&d);
    sec = pd_doc_child(d, pd_doc_root(d), 0);
    p = pd_doc_child(d, sec, 0);
    pd_doc_set_para_style(d, p, pd_doc_style_find(d, "Title"));
    pd_doc_set_role(d, p, PD_ROLE_TITLE, 0);
    text(d, p, "Conversion Test");
    para(d, sec, "Heading 1", PD_ROLE_HEADING, 1, "First Section");

    p = para(d, sec, NULL, 0, 0, "Plain, ");
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_WEIGHT;
    cp.weight = 700;
    ftext(d, p, "bold", &cp);
    text(d, p, ", ");
    cp.mask = PD_CP_ITALIC;
    cp.italic = 1;
    ftext(d, p, "italic", &cp);
    text(d, p, ", ");
    cp.mask = PD_CP_UNDERLINE;
    cp.underline = 1;
    ftext(d, p, "underlined", &cp);
    text(d, p, ", ");
    cp.mask = PD_CP_STRIKE;
    cp.strike = 1;
    ftext(d, p, "struck", &cp);
    text(d, p, ", x");
    cp.mask = PD_CP_SHIFT;
    cp.shift = PD_SHIFT_SUPER;
    ftext(d, p, "2", &cp);
    text(d, p, " and H");
    cp.shift = PD_SHIFT_SUB;
    ftext(d, p, "2", &cp);
    text(d, p, "O, ");
    cp.mask = PD_CP_FAMILY;
    strcpy(cp.family, "monospace");
    ftext(d, p, "code_span", &cp);
    text(d, p, ", ");
    cp.mask = PD_CP_COLOR;
    cp.color = 0xFFCC0000u;
    ftext(d, p, "red", &cp);
    text(d, p, " & <special> \"chars\" * _ # \\ ` [x], ");
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_LINK;
    o.source = "https://example.com/a?b=1&c=2";
    o.source_len = (int32_t)strlen(o.source);
    pd_doc_insert_inline(d, end_of(d, p), &o, NULL);
    text(d, p, "a link");
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_LINK;
    pd_doc_insert_inline(d, end_of(d, p), &o, NULL);
    text(d, p, ", unicode \xE5\xA4\xA9\xE5\x9C\xB0 \xC3\xA9\xC3\xA8 \xF0\x9F\x98\x80 end.");

    memset(lv, 0, sizeof(lv));
    lv[0].format = PD_NUM_BULLET;
    strcpy(lv[0].text, "\xE2\x80\xA2");
    lv[0].indent = PD_PT(18);
    lv[1] = lv[0];
    lv[1].indent = PD_PT(36);
    pd_doc_list_define(d, 2, lv, &bul);
    lv[0].format = lv[1].format = PD_NUM_DECIMAL;
    lv[0].start = lv[1].start = 1;
    strcpy(lv[0].text, "%1.");
    strcpy(lv[1].text, "%2.");
    pd_doc_list_define(d, 2, lv, &num);
    p = para(d, sec, NULL, 0, 0, "Item one");
    pd_doc_set_list(d, p, bul, 0);
    p = para(d, sec, NULL, 0, 0, "Nested item");
    pd_doc_set_list(d, p, bul, 1);
    p = para(d, sec, NULL, 0, 0, "Item two");
    pd_doc_set_list(d, p, bul, 0);
    p = para(d, sec, NULL, 0, 0, "First step");
    pd_doc_set_list(d, p, num, 0);
    p = para(d, sec, NULL, 0, 0, "Second step");
    pd_doc_set_list(d, p, num, 0);

    para(d, sec, "Quote", PD_ROLE_QUOTE, 0, "A quoted paragraph.");
    para(d, sec, "Code", PD_ROLE_CODE, 0, "int main(void) {\n    return 0; /* <ok> */\n}");
    para(d, sec, "Heading 2", PD_ROLE_HEADING, 2, "Tables & Figures");

    pd_doc_insert_block(d, sec, -1, PD_BLOCK_TABLE, &t);
    pd_doc_table_props(d, t, &tp);
    tp.header_rows = 1;
    pd_doc_set_table_props(d, t, &tp);

    for (r = 0; r < 3; r++) {
        pd_block_id row = r == 0 ? pd_doc_child(d, t, 0) : 0;
        static const char* cells[3][3] = { { "Name", "Value", "Unit" }, { "alpha", "1.5", "m" }, { "beta", "2", "" } };

        if (r > 0) {
            pd_doc_insert_block(d, t, -1, PD_BLOCK_ROW, &row);
        }

        for (c = 0; c < 3; c++) {
            pd_block_id cell = c == 0 ? pd_doc_child(d, row, 0) : 0;

            if (r == 2 && c == 2) {
                break;
            }

            if (c > 0) {
                pd_doc_insert_block(d, row, -1, PD_BLOCK_CELL, &cell);
            }

            if (r == 2 && c == 1) {
                pd_doc_cell_props(d, cell, &cep);
                cep.col_span = 2;
                cep.background = 0xFFDDEEFFu;
                pd_doc_set_cell_props(d, cell, &cep);
            }

            text(d, pd_doc_child(d, cell, 0), cells[r][c]);
        }
    }

    pd_doc_add_resource(d, "image/png", png1x1, sizeof(png1x1), &res);
    pd_doc_insert_block(d, sec, -1, PD_BLOCK_FLOAT, &fl);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.resource = res;
    o.width = PD_PT(60);
    o.height = PD_PT(30);
    pd_doc_insert_inline(d, at(pd_doc_child(d, fl, 0), 0), &o, NULL);
    p = para(d, fl, "Caption", PD_ROLE_CAPTION, 0, "Figure ");
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = PD_FIELD_SEQ;
    strcpy(o.name, "Figure");
    pd_doc_insert_inline(d, end_of(d, p), &o, NULL);
    text(d, p, ": a tiny image.");

    p = para(d, sec, NULL, 0, 0, "A footnote here");
    pd_doc_insert_block(d, 0, -1, PD_BLOCK_STORY, &story);
    text(d, pd_doc_child(d, story, 0), "The note text.");
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FOOTNOTE;
    o.target = story;
    pd_doc_insert_inline(d, end_of(d, p), &o, NULL);
    text(d, p, " and an equation ");
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_EQUATION;
    o.source = "E = mc^2";
    o.source_len = 8;
    o.width = PD_PT(30);
    o.height = PD_PT(8);
    pd_doc_insert_inline(d, end_of(d, p), &o, NULL);
    text(d, p, " inline.");
    p = para(d, sec, NULL, PD_ROLE_EQUATION, 0, NULL);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_EQUATION;
    o.source = "\\sum_{i=1}^n i^2";
    o.source_len = (int32_t)strlen(o.source);
    o.width = PD_PT(40);
    o.height = PD_PT(12);
    pd_doc_insert_inline(d, at(p, 0), &o, NULL);
    para(d, sec, NULL, 0, 0, "Last paragraph.");
    return d;
}

/* ------------------------------------------------------------------ */
/* inspecting an imported document                                    */
/* ------------------------------------------------------------------ */

/* the paragraph whose text contains s */
static pd_block_id find_para(const pd_doc* d, const char* s) {
    pd_block_id p;

    for (p = pd_doc_next_paragraph(d, 0); p; p = pd_doc_next_paragraph(d, p)) {
        const char* t;
        uint32_t n;

        if (pd_doc_para_text(d, p, &t, &n) == PD_OK && n >= strlen(s)) {
            uint32_t i;

            for (i = 0; i + strlen(s) <= n; i++) {
                if (memcmp(t + i, s, strlen(s)) == 0) {
                    return p;
                }
            }
        }
    }

    return 0;
}

/* resolved character properties of the first occurrence of s */
static int props_of(const pd_doc* d, const char* s, pd_char_props* out) {
    pd_block_id p = find_para(d, s);
    const char* t;
    uint32_t n, i;
    pd_run runs[256];
    int32_t nr, r;

    if (!p) {
        return 0;
    }

    pd_doc_para_text(d, p, &t, &n);

    for (i = 0; i + strlen(s) <= n && memcmp(t + i, s, strlen(s)); i++) {
    }

    pd_doc_para_runs(d, p, runs, 256, &nr);

    for (r = 0; r < nr; r++) {
        if (runs[r].start <= i && i < runs[r].end) {
            return pd_doc_format_resolve(d, p, runs[r].format, out) == PD_OK;
        }
    }

    return 0;
}

/* the first inline object of a kind in a paragraph */
static int object_in(const pd_doc* d, pd_block_id p, int kind, pd_inline* out) {
    const char* t;
    uint32_t n, i;

    if (!p || pd_doc_para_text(d, p, &t, &n) != PD_OK) {
        return 0;
    }

    for (i = 0; i + 2 < n; i++) {
        if ((unsigned char)t[i] == 0xEF && (unsigned char)t[i + 1] == 0xBF && (unsigned char)t[i + 2] == 0xBC &&
                pd_doc_inline_at(d, at(p, i), out) == PD_OK && out->kind == kind) {
            return 1;
        }
    }

    return 0;
}

static int list_kind(const pd_doc* d, pd_block_id p, int32_t* level) {
    pd_block_info bi;
    pd_list_level lv[9];
    int32_t n;

    if (!p || pd_doc_block_info(d, p, &bi) != PD_OK || !bi.list || pd_doc_list_info(d, bi.list, &n, lv) != PD_OK) {
        return 0;
    }

    *level = bi.list_level;
    return lv[bi.list_level].format == PD_NUM_BULLET ? 1 : 2;
}

static pd_block_id find_kind(const pd_doc* d, pd_block_id id, int kind) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(d, id, &bi) != PD_OK) {
        return 0;
    }

    if (bi.kind == kind) {
        return id;
    }

    for (i = 0; i < bi.child_count; i++) {
        pd_block_id r = find_kind(d, pd_doc_child(d, id, i), kind);

        if (r) {
            return r;
        }
    }

    return 0;
}

enum {
    F_HEAD = 1, F_BOLD = 2, F_ITAL = 4, F_UNDER = 8, F_STRIKE = 16, F_SHIFT = 32, F_MONO = 64, F_LINK = 128,
    F_LIST = 256, F_QUOTE = 512, F_CODE = 1024, F_TABLE = 2048, F_SPAN = 4096, F_IMAGE = 8192, F_NOTE = 16384,
    F_COLOR = 32768, F_TITLE = 65536, F_HEADER_ROW = 131072, F_MATH = 262144
};

static void check_import(const pd_doc* d, uint32_t feat, const char* name) {
    pd_char_props cp;
    pd_block_id p;
    pd_block_info bi;
    pd_inline o;
    int32_t level = -1;
    int fails0 = failures;

    if (feat & F_TITLE) {
        p = find_para(d, "Conversion Test");
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && bi.role == PD_ROLE_TITLE);
    }

    if (feat & F_HEAD) {
        p = find_para(d, "First Section");
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && bi.role == PD_ROLE_HEADING && bi.level == 1);
        p = find_para(d, "Tables & Figures");
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && bi.role == PD_ROLE_HEADING && bi.level == 2);
    }

    CHECK(find_para(d, "Last paragraph.") != 0);
    CHECK(find_para(d, "& <special> \"chars\" * _ # \\ ` [x]") != 0);
    CHECK(find_para(d, "unicode \xE5\xA4\xA9\xE5\x9C\xB0 \xC3\xA9\xC3\xA8 \xF0\x9F\x98\x80 end.") != 0);
    CHECK(props_of(d, "Plain", &cp) && cp.weight < 600 && !cp.italic);

    if (feat & F_BOLD) {
        CHECK(props_of(d, "bold", &cp) && cp.weight >= 600);
        CHECK(props_of(d, ", italic", &cp) && cp.weight < 600);
    }

    if (feat & F_ITAL) {
        CHECK(props_of(d, "italic", &cp) && cp.italic);
    }

    if (feat & F_UNDER) {
        CHECK(props_of(d, "underlined", &cp) && cp.underline);
    }

    if (feat & F_STRIKE) {
        CHECK(props_of(d, "struck", &cp) && cp.strike);
    }

    if (feat & F_SHIFT) {
        CHECK(props_of(d, "x2", &cp) && cp.shift == PD_SHIFT_NONE);
        CHECK(props_of(d, "2 and H", &cp) && cp.shift == PD_SHIFT_SUPER);
        CHECK(props_of(d, "2O, ", &cp) && cp.shift == PD_SHIFT_SUB);
    }

    if (feat & F_MONO) {
        CHECK(props_of(d, "code_span", &cp) && (strstr(cp.family, "mono") || strstr(cp.family, "Courier") ||
                                                strstr(cp.family, "Mono")));
    }

    if (feat & F_COLOR) {
        CHECK(props_of(d, "red", &cp) && (cp.color & 0xFFFFFF) == 0xCC0000);
    }

    if (feat & F_LINK) {
        p = find_para(d, "a link");
        CHECK(object_in(d, p, PD_INLINE_LINK, &o) && o.source_len == 29 && memcmp(o.source, "https://example.com/a?b=1&c=2",
                29) == 0);
    }

    if (feat & F_LIST) {
        CHECK(list_kind(d, find_para(d, "Item one"), &level) == 1 && level == 0);
        CHECK(list_kind(d, find_para(d, "Nested item"), &level) == 1 && level == 1);
        CHECK(list_kind(d, find_para(d, "Item two"), &level) == 1 && level == 0);
        CHECK(list_kind(d, find_para(d, "Second step"), &level) == 2 && level == 0);
    }

    if (feat & F_QUOTE) {
        p = find_para(d, "A quoted paragraph.");
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && bi.role == PD_ROLE_QUOTE);
    }

    if (feat & F_CODE) {
        p = find_para(d, "int main(void) {");
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && bi.role == PD_ROLE_CODE);
        CHECK(find_para(d, "    return 0; /* <ok> */") != 0);
    }

    if (feat & F_TABLE) {
        pd_block_id t = find_kind(d, pd_doc_root(d), PD_BLOCK_TABLE);
        pd_table_props tp;

        CHECK(t && pd_doc_block_info(d, t, &bi) == PD_OK && bi.child_count == 3);
        CHECK(t && pd_doc_block_info(d, pd_doc_child(d, t, 1), &bi) == PD_OK && bi.child_count == 3);

        if (feat & F_HEADER_ROW) {
            CHECK(t && pd_doc_table_props(d, t, &tp) == PD_OK && tp.header_rows == 1);
        }

        p = find_para(d, "1.5");
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && pd_doc_block_info(d, bi.parent, &bi) == PD_OK &&
              bi.kind == PD_BLOCK_CELL);

        if (feat & F_SPAN) {
            pd_cell_props cp2;

            p = find_para(d, "2");
            p = t ? pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 2), 1), 0) : 0;
            CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && pd_doc_cell_props(d, bi.parent, &cp2) == PD_OK &&
                  cp2.col_span == 2 && (cp2.background & 0xFFFFFF) == 0xDDEEFF);
        }
    }

    if (feat & F_IMAGE) {
        pd_block_id img = 0;
        const char* mime;
        const void* data;
        size_t len;

        for (p = pd_doc_next_paragraph(d, 0); p && !img; p = pd_doc_next_paragraph(d, p)) {
            if (object_in(d, p, PD_INLINE_IMAGE, &o)) {
                img = p;
            }
        }

        CHECK(img && pd_doc_resource(d, o.resource, &mime, &data, &len) == PD_OK && len == sizeof(png1x1) &&
              memcmp(data, png1x1, len) == 0 && strcmp(mime, "image/png") == 0);
        CHECK(img && o.width > PD_PT(55) && o.width < PD_PT(65) && o.height > PD_PT(25) && o.height < PD_PT(35));
    }

    if (feat & F_NOTE) {
        p = find_para(d, "A footnote here");
        CHECK(object_in(d, p, PD_INLINE_FOOTNOTE, &o) && o.target);

        if (o.target) {
            const char* t;
            uint32_t n;

            CHECK(pd_doc_para_text(d, pd_doc_child(d, o.target, 0), &t, &n) == PD_OK && n >= 14 &&
                  memcmp(t, "The note text.", 14) == 0);
        }

        CHECK(p && find_para(d, " and an equation ") == p);
    }

    if (feat & F_MATH) {
        p = find_para(d, " and an equation ");
        CHECK(object_in(d, p, PD_INLINE_EQUATION, &o) && o.source_len == 8 && memcmp(o.source, "E = mc^2", 8) == 0);
        p = find_para(d, "Last paragraph.");
        p = pd_doc_prev_paragraph(d, p);
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && bi.role == PD_ROLE_EQUATION &&
              object_in(d, p, PD_INLINE_EQUATION, &o) && o.source_len == 16 && memcmp(o.source, "\\sum_{i=1}^n i^2", 16) == 0);
    }

    printf("  %s: %s\n", name, failures == fails0 ? "all features survive" : "FAILED");
}

static void test_roundtrip(void) {
    static const struct {
        pd_conv_format f;
        const char* name;
        uint32_t feat;
    } fmts[] = {
        { PD_CONV_HTML, "HTML", 0x7FFFF },
        {
            PD_CONV_MARKDOWN, "Markdown", F_HEAD | F_BOLD | F_ITAL | F_UNDER | F_STRIKE | F_SHIFT | F_MONO | F_LINK | F_LIST |
            F_QUOTE | F_CODE | F_TABLE | F_IMAGE | F_NOTE | F_HEADER_ROW
        },
        {
            PD_CONV_RTF, "RTF", F_HEAD | F_BOLD | F_ITAL | F_UNDER | F_STRIKE | F_SHIFT | F_MONO | F_COLOR | F_LINK | F_LIST |
            F_TABLE | F_IMAGE | F_NOTE | F_TITLE | F_CODE | F_QUOTE | F_SPAN | F_HEADER_ROW
        },
        { PD_CONV_DOCX, "DOCX", 0x3FFFF },
        { PD_CONV_JDATA, "JData", 0x7FFFF },
    };
    pd_doc* d = rich();
    size_t i;

    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        buf_t b = { NULL, 0 };
        pd_doc* t = NULL;
        pd_status st = pd_doc_export(d, fmts[i].f, to_buf, &b);

        if (st == PD_ERR_ARG) {
            printf("  %s: not built yet\n", fmts[i].name);
            continue;
        }

        CHECK(st == PD_OK && b.n > 0);
        CHECK(pd_conv_detect(b.p, b.n) == fmts[i].f);
        CHECK(pd_doc_import(b.p, b.n, fmts[i].f, &t) == PD_OK);

        if (t) {
            check_import(t, fmts[i].feat, fmts[i].name);
            CHECK(!pd_doc_can_undo(t));
            pd_doc_free(t);
        }

        free(b.p);
    }

    pd_doc_free(d);
}

/* the test document in every format, for checks by other software (make conv-check) */
static void write_files(const char* dir) {
    static const struct {
        pd_conv_format f;
        const char* ext;
    } fmts[] = { { PD_CONV_HTML, "html" }, { PD_CONV_MARKDOWN, "md" }, { PD_CONV_LATEX, "tex" }, { PD_CONV_RTF, "rtf" },
        { PD_CONV_DOCX, "docx" }, { PD_CONV_TEXT, "txt" }, { PD_CONV_JDATA, "pdoc" }
    };
    pd_doc* d = rich();
    size_t i;
    char path[1024];

    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        buf_t b = { NULL, 0 };
        FILE* f;

        snprintf(path, sizeof(path), "%s/rich.%s", dir, fmts[i].ext);

        if (pd_doc_export(d, fmts[i].f, to_buf, &b) == PD_OK && (f = fopen(path, "wb")) != NULL) {
            fwrite(b.p, 1, b.n, f);
            fclose(f);
        }

        free(b.p);
    }

    pd_doc_free(d);
}

static const char* para_text(const pd_doc* d, pd_block_id p, uint32_t* n) {
    const char* t = "";

    *n = 0;
    pd_doc_para_text(d, p, &t, n);
    return t;
}

static int text_is(const pd_doc* d, pd_block_id p, const char* want) {
    uint32_t n;
    const char* t = para_text(d, p, &n);

    return n == strlen(want) && memcmp(t, want, n) == 0;
}

static void test_paste(void) {
    pd_doc* d;
    pd_block_id sec, p, q, r;
    pd_pos after;
    pd_char_props cp;
    pd_block_info bi;
    const char* html1 = "<b>big</b> fat";      /* (a trailing space would collapse, as in a browser) */
    const char* html2 = "<p>one</p><h2>two</h2><p>three</p>";
    const char* html3 = "<table><tr><td>a</td><td>b</td></tr></table>";
    const char* lines = "x\ny\nz";

    pd_doc_new(&d);
    sec = pd_doc_child(d, pd_doc_root(d), 0);
    p = pd_doc_child(d, sec, 0);
    text(d, p, "Hello world");
    pd_doc_clear_undo(d);

    /* inline: into the paragraph */
    CHECK(pd_doc_paste(d, at(p, 6), html1, strlen(html1), PD_CONV_HTML, &after) == PD_OK);
    CHECK(text_is(d, p, "Hello big fatworld") && after.block == p && after.offset == 13);
    CHECK(props_of(d, "big", &cp) && cp.weight >= 600 && props_of(d, "Hello", &cp) && cp.weight < 600);
    CHECK(pd_doc_undo_label(d) && strcmp(pd_doc_undo_label(d), "Paste") == 0);
    CHECK(pd_doc_undo(d) == PD_OK && text_is(d, p, "Hello world"));

    /* several paragraphs: the first joins the head, the last the tail */
    CHECK(pd_doc_paste(d, at(p, 5), html2, strlen(html2), PD_CONV_HTML, &after) == PD_OK);
    CHECK(text_is(d, p, "Helloone"));
    q = pd_doc_child(d, sec, 1);
    r = pd_doc_child(d, sec, 2);
    CHECK(text_is(d, q, "two") && pd_doc_block_info(d, q, &bi) == PD_OK && bi.role == PD_ROLE_HEADING && bi.level == 2);
    CHECK(text_is(d, r, "three world") && after.block == r && after.offset == 5);
    CHECK(pd_doc_undo(d) == PD_OK && text_is(d, pd_doc_child(d, sec, 0), "Hello world") &&
          pd_doc_child(d, sec, 1) == 0);

    /* a table between the halves */
    CHECK(pd_doc_paste(d, at(p, 5), html3, strlen(html3), PD_CONV_HTML, &after) == PD_OK);
    CHECK(pd_doc_block_info(d, pd_doc_child(d, sec, 1), &bi) == PD_OK && bi.kind == PD_BLOCK_TABLE);
    CHECK(text_is(d, pd_doc_child(d, sec, 0), "Hello") && text_is(d, pd_doc_child(d, sec, 2), " world"));
    CHECK(pd_doc_undo(d) == PD_OK);

    /* plain text lines */
    CHECK(pd_doc_paste(d, at(p, 11), lines, strlen(lines), PD_CONV_TEXT, &after) == PD_OK);
    CHECK(text_is(d, pd_doc_child(d, sec, 0), "Hello worldx") && text_is(d, pd_doc_child(d, sec, 1), "y") &&
          text_is(d, pd_doc_child(d, sec, 2), "z"));
    CHECK(pd_doc_undo(d) == PD_OK);

    /* native clipboard: a range of another document, formats intact */
    {
        pd_doc* src = rich();
        pd_block_id sp = find_para(src, "Plain, ");
        pd_range rg;
        buf_t b = { NULL, 0 };

        rg.start = at(sp, 7);
        rg.end = at(sp, 22);   /* "bold, italic, u" */
        CHECK(pd_doc_export_range(src, rg, PD_CONV_JDATA, to_buf, &b) == PD_OK);
        CHECK(pd_doc_paste(d, at(p, 0), b.p, b.n, pd_conv_detect(b.p, b.n), &after) == PD_OK);
        CHECK(text_is(d, p, "bold, italic, uHello world") && after.offset == 15);
        CHECK(props_of(d, "bold", &cp) && cp.weight >= 600 && props_of(d, "italic", &cp) && cp.italic);
        free(b.p);
        pd_doc_free(src);
    }

    pd_doc_free(d);
}

static void test_range(void) {
    pd_doc* d = rich();
    pd_block_id a = find_para(d, "Plain, "), b = find_para(d, "Item one");
    pd_range rg;
    buf_t t = { NULL, 0 }, h = { NULL, 0 };

    rg.start = at(a, 7);
    rg.end = at(b, 4);
    CHECK(pd_doc_export_range(d, rg, PD_CONV_TEXT, to_buf, &t) == PD_OK);
    CHECK(t.n > 10 && strncmp(t.p, "bold, italic", 12) == 0 && strstr(t.p, "\xE2\x80\xA2 Item\n") != NULL);
    CHECK(strstr(t.p, "Item one") == NULL);
    rg.end = at(a, 11);
    CHECK(pd_doc_export_range(d, rg, PD_CONV_HTML, to_buf, &h) == PD_OK);
    CHECK(strstr(h.p, "<strong>bold</strong>") != NULL && strstr(h.p, "italic") == NULL);
    rg.end = at(a, 3);     /* backwards */
    CHECK(pd_doc_export_range(d, rg, PD_CONV_TEXT, to_buf, &t) == PD_ERR_ARG);
    free(t.p);
    free(h.p);
    pd_doc_free(d);
}

static uint32_t rnd_state = 12345;

static uint32_t rnd(uint32_t n) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 8) % (n ? n : 1);
}

/* truncated and corrupted input never crashes an importer; what loads is a valid document */
static void test_fuzz(void) {
    static const pd_conv_format fmts[] = { PD_CONV_HTML, PD_CONV_MARKDOWN, PD_CONV_RTF, PD_CONV_DOCX, PD_CONV_TEXT };
    pd_doc* d = rich();
    size_t i, k;
    int loaded = 0, rejected = 0;

    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        buf_t b = { NULL, 0 };
        unsigned char* m;

        pd_doc_export(d, fmts[i], to_buf, &b);
        m = (unsigned char*)malloc(b.n + 1);

        for (k = 0; k < 600; k++) {
            pd_doc* x = NULL;
            size_t len = b.n;
            int j, flips = 1 + (int)rnd(6);

            memcpy(m, b.p, b.n);

            if (k % 3 == 0) {
                len = rnd((uint32_t)b.n);   /* truncated */
            }

            for (j = 0; j < flips && k % 3 != 0; j++) {
                size_t at_ = rnd((uint32_t)b.n);

                m[at_] = k % 2 ? (unsigned char)rnd(256) : (unsigned char)"<>{}\\*_[]|`#&;\"'\n "[rnd(19)];
            }

            if (pd_doc_import(m, len, fmts[i], &x) == PD_OK) {
                buf_t again = { NULL, 0 };

                loaded++;
                CHECK(pd_doc_export(x, PD_CONV_HTML, to_buf, &again) == PD_OK);   /* and it is a usable document */
                free(again.p);
                pd_doc_free(x);
            } else {
                rejected++;
            }
        }

        free(m);
        free(b.p);
    }

    /* garbage */
    for (k = 0; k < 300; k++) {
        unsigned char g[512];
        size_t j, n = rnd(512);
        pd_doc* x = NULL;

        for (j = 0; j < n; j++) {
            g[j] = (unsigned char)rnd(256);
        }

        if (k % 4 == 0 && n > 4) {
            memcpy(g, "PK\x03\x04", 4);
        } else if (k % 4 == 1 && n > 5) {
            memcpy(g, "{\\rtf", 5);
        }

        if (pd_doc_import(g, n, pd_conv_detect(g, n), &x) == PD_OK) {
            loaded++;
            pd_doc_free(x);
        } else {
            rejected++;
        }
    }

    printf("  %d mangled inputs loaded, %d rejected, no crash\n", loaded, rejected);
    pd_doc_free(d);
}

/* ---------------- DOCX styles, defaults and theme ---------------- */

static uint32_t crc32_of(const char* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        c ^= (unsigned char)p[i];

        for (k = 0; k < 8; k++) {
            c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
    }

    return ~c;
}

static void le(buf_t* b, uint32_t v, int bytes) {
    char c[4];
    int i;

    for (i = 0; i < bytes; i++) {
        c[i] = (char)(v >> (8 * i));
    }

    to_buf(b, c, (size_t)bytes);
}

/* a zip of uncompressed entries: names[i] holds texts[i], lens[i] bytes of it (all of a string when NULL) */
static buf_t stored_zip_n(const char** names, const char** texts, const size_t* lens, int n);

static buf_t stored_zip(const char** names, const char** texts, int n) {
    return stored_zip_n(names, texts, NULL, n);
}

static buf_t stored_zip_n(const char** names, const char** texts, const size_t* lens, int n) {
    buf_t z, cd;
    uint32_t offs[16];
    int i;

    memset(&z, 0, sizeof(z));
    memset(&cd, 0, sizeof(cd));

    for (i = 0; i < n; i++) {
        uint32_t len = (uint32_t)(lens ? lens[i] : strlen(texts[i])), crc = crc32_of(texts[i], len);
        uint16_t nl = (uint16_t)strlen(names[i]);

        offs[i] = (uint32_t)z.n;
        le(&z, 0x04034b50u, 4); le(&z, 20, 2); le(&z, 0, 2); le(&z, 0, 2); le(&z, 0, 4);
        le(&z, crc, 4); le(&z, len, 4); le(&z, len, 4); le(&z, nl, 2); le(&z, 0, 2);
        to_buf(&z, names[i], nl);
        to_buf(&z, texts[i], len);

        le(&cd, 0x02014b50u, 4); le(&cd, 20, 2); le(&cd, 20, 2); le(&cd, 0, 2); le(&cd, 0, 2); le(&cd, 0, 4);
        le(&cd, crc, 4); le(&cd, len, 4); le(&cd, len, 4); le(&cd, nl, 2); le(&cd, 0, 2); le(&cd, 0, 2);
        le(&cd, 0, 2); le(&cd, 0, 2); le(&cd, 0, 4); le(&cd, offs[i], 4);
        to_buf(&cd, names[i], nl);
    }

    {
        uint32_t at_cd = (uint32_t)z.n;

        to_buf(&z, cd.p, cd.n);
        le(&z, 0x06054b50u, 4); le(&z, 0, 2); le(&z, 0, 2); le(&z, (uint32_t)n, 2); le(&z, (uint32_t)n, 2);
        le(&z, (uint32_t)cd.n, 4); le(&z, at_cd, 4); le(&z, 0, 2);
    }

    free(cd.p);
    return z;
}

/* a paragraph's properties as laid out: its style resolved, its own on top */
static pd_para_props para_resolved(const pd_doc* d, pd_block_id p) {
    pd_block_info bi;
    pd_para_props r, o;

    pd_doc_block_info(d, p, &bi);
    pd_doc_style_resolve(d, bi.style, &r, NULL);
    pd_doc_para_props(d, p, &o);

    if (o.mask & PD_PP_ALIGN) r.align = o.align;
    if (o.mask & PD_PP_INDENT_LEFT) r.indent_left = o.indent_left;
    if (o.mask & PD_PP_INDENT_RIGHT) r.indent_right = o.indent_right;
    if (o.mask & PD_PP_INDENT_FIRST) r.indent_first = o.indent_first;
    if (o.mask & PD_PP_SPACE_BEFORE) r.space_before = o.space_before;
    if (o.mask & PD_PP_SPACE_AFTER) r.space_after = o.space_after;
    if (o.mask & PD_PP_LINE_SPACING) r.line_spacing = o.line_spacing;
    if (o.mask & PD_PP_KEEP_NEXT) r.keep_with_next = o.keep_with_next;
    if (o.mask & PD_PP_KEEP_LINES) r.keep_lines = o.keep_lines;
    if (o.mask & PD_PP_HYPHENATE) r.hyphenate = o.hyphenate;
    return r;
}

/* the characters at a byte offset of a paragraph, resolved */
static pd_char_props chars_at(const pd_doc* d, pd_block_id p, uint32_t off) {
    pd_run runs[32];
    int32_t n = 0, i;
    pd_char_props cp;

    memset(&cp, 0, sizeof(cp));
    pd_doc_para_runs(d, p, runs, 32, &n);

    for (i = 0; i < n; i++) {
        if (off >= runs[i].start && off < runs[i].end) {
            pd_doc_format_resolve(d, p, runs[i].format, &cp);
        }
    }

    return cp;
}

/* Word keeps most of what a paragraph looks like in its styles: the
   document defaults, a default paragraph style, styles based on styles,
   theme fonts, character styles. The shape of a proposal written in Word:
   Times New Roman 12 by default, the body in a custom justified Arial 11
   style with hyphenation off. */
/* the document written as DOCX and read back: what the checks of an import
   must still find after the round trip. Frees the original. */
static pd_doc* docx_again(pd_doc* d) {
    buf_t b = { NULL, 0 };
    pd_doc* t = NULL;

    if (d && pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK && pd_doc_import(b.p, b.n, PD_CONV_DOCX, &t) != PD_OK) {
        t = NULL;
    }

    free(b.p);
    pd_doc_free(d);
    return t;
}

static void check_docx_styles(const pd_doc* d);

static void test_docx_styles(void) {
    static const char* names[] = { "[Content_Types].xml", "word/document.xml", "word/styles.xml",
                                   "word/theme/theme1.xml", "word/settings.xml"
                                 };
    static const char* texts[] = {
        "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>",

        "<?xml version=\"1.0\"?><w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:r><w:t>Plain body.</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"LeadingPara\"/></w:pPr>"
        "<w:r><w:t xml:space=\"preserve\">While the </w:t></w:r>"
        "<w:r><w:rPr><w:u w:val=\"single\"/></w:rPr><w:t>new DOT</w:t></w:r>"
        "<w:r><w:rPr><w:rStyle w:val=\"Emph\"/></w:rPr><w:t xml:space=\"preserve\"> works</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Body2\"/></w:pPr><w:r><w:t>Indented.</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:spacing w:line=\"480\" w:lineRule=\"exact\"/><w:jc w:val=\"center\"/></w:pPr>"
        "<w:r><w:t>Exact.</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Heading1\"/></w:pPr><w:r><w:t xml:space=\"preserve\">Head </w:t></w:r>"
        "<w:r><w:rPr><w:b w:val=\"0\"/><w:sz w:val=\"20\"/></w:rPr><w:t>light</w:t></w:r></w:p>"
        "</w:body></w:document>",

        "<?xml version=\"1.0\"?><w:styles xmlns:w=\"w\">"
        "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\"/>"
        "</w:rPr></w:rPrDefault><w:pPrDefault/></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
        "<w:rPr><w:sz w:val=\"24\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:customStyle=\"1\" w:styleId=\"LeadingPara\"><w:name w:val=\"LeadingPara\"/>"
        "<w:basedOn w:val=\"Normal\"/><w:pPr><w:suppressAutoHyphens/><w:jc w:val=\"both\"/>"
        "<w:rPr><w:sz w:val=\"40\"/></w:rPr></w:pPr>"     /* the paragraph mark's run: not the text's */
        "<w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\"/><w:sz w:val=\"22\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Body2\"><w:name w:val=\"Body2\"/><w:basedOn w:val=\"LeadingPara\"/>"
        "<w:pPr><w:spacing w:before=\"240\" w:after=\"120\" w:line=\"360\" w:lineRule=\"auto\"/>"
        "<w:ind w:left=\"720\" w:firstLine=\"360\"/></w:pPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/>"
        "<w:basedOn w:val=\"Normal\"/><w:pPr><w:keepNext/><w:outlineLvl w:val=\"0\"/></w:pPr>"
        "<w:rPr><w:rFonts w:asciiTheme=\"majorHAnsi\" w:hAnsiTheme=\"majorHAnsi\"/><w:b/><w:sz w:val=\"32\"/>"
        "</w:rPr></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"Emph\"><w:name w:val=\"Emph\"/><w:rPr><w:i/></w:rPr></w:style>"
        "<w:style w:type=\"table\" w:styleId=\"Grid\"><w:name w:val=\"Grid\"/><w:pPr><w:jc w:val=\"right\"/></w:pPr>"
        "<w:tblStylePr w:type=\"firstRow\"><w:rPr><w:b/></w:rPr></w:tblStylePr></w:style>"
        "</w:styles>",

        "<?xml version=\"1.0\"?><a:theme xmlns:a=\"a\"><a:themeElements><a:fontScheme>"
        "<a:majorFont><a:latin typeface=\"Calibri Light\"/></a:majorFont>"
        "<a:minorFont><a:latin typeface=\"Calibri\"/></a:minorFont></a:fontScheme></a:themeElements></a:theme>",

        "<?xml version=\"1.0\"?><w:settings xmlns:w=\"w\"/>"
    };
    buf_t z = stored_zip(names, texts, 5);
    pd_doc* d = NULL;
    int pass;

    CHECK(pd_doc_import(z.p, z.n, PD_CONV_DOCX, &d) == PD_OK);
    free(z.p);

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        CHECK(d != NULL);

        if (!d) {
            return;
        }

        check_docx_styles(d);
    }

    pd_doc_free(d);

    /* fonts by kind, for a resolver that lacks the family asked for */
    CHECK(pd_font_family_class("Arial") == PD_FAMILY_SANS && pd_font_family_class("Calibri Light") == PD_FAMILY_SANS);
    CHECK(pd_font_family_class("Courier New") == PD_FAMILY_MONO);
    CHECK(pd_font_family_class("DejaVu Sans Mono") == PD_FAMILY_MONO);
    CHECK(pd_font_family_class("Times New Roman") == PD_FAMILY_SERIF && pd_font_family_class(NULL) == PD_FAMILY_SERIF);
}

static void check_docx_styles(const pd_doc* d) {
    pd_block_id sec, p[5];
    pd_para_props pp;
    pd_char_props cp;
    pd_block_info bi;
    int i;

    sec = pd_doc_child(d, pd_doc_root(d), 0);

    for (i = 0; i < 5; i++) {
        p[i] = pd_doc_child(d, sec, i);
    }

    /* the defaults and the default paragraph style: Times New Roman 12, no hyphenation */
    cp = chars_at(d, p[0], 0);
    CHECK(strcmp(cp.family, "Times New Roman") == 0 && cp.size == PD_PT(12));
    pp = para_resolved(d, p[0]);
    CHECK(pp.align == PD_ALIGN_LEFT && pp.hyphenate == 0);

    /* a custom style: justified Arial 11, its paragraph mark's size ignored */
    pp = para_resolved(d, p[1]);
    CHECK(pp.align == PD_ALIGN_JUSTIFY && pp.hyphenate == 0);
    cp = chars_at(d, p[1], 0);
    CHECK(strcmp(cp.family, "Arial") == 0 && cp.size == PD_PT(11) && !cp.underline && !cp.italic);
    cp = chars_at(d, p[1], 10);     /* "new DOT" */
    CHECK(strcmp(cp.family, "Arial") == 0 && cp.size == PD_PT(11) && cp.underline == 1);
    cp = chars_at(d, p[1], 18);     /* " works", a character style */
    CHECK(strcmp(cp.family, "Arial") == 0 && cp.italic && !cp.underline);

    /* based on it: indents, spacing and a 1.5 line height on top */
    pp = para_resolved(d, p[2]);
    CHECK(pp.align == PD_ALIGN_JUSTIFY && pp.indent_left == PD_PT(36) && pp.indent_first == PD_PT(18));
    CHECK(pp.space_before == PD_PT(12) && pp.space_after == PD_PT(6) && pp.line_spacing == 1500);
    cp = chars_at(d, p[2], 0);
    CHECK(strcmp(cp.family, "Arial") == 0 && cp.size == PD_PT(11));

    /* direct paragraph properties: centred, exactly 24pt lines on 12pt text */
    pp = para_resolved(d, p[3]);
    CHECK(pp.align == PD_ALIGN_CENTER && pp.line_spacing > 1700 && pp.line_spacing < 1780);

    /* a heading in the theme's heading font; a run that turns the bold off */
    pd_doc_block_info(d, p[4], &bi);
    CHECK(bi.role == PD_ROLE_HEADING && bi.level == 1);
    cp = chars_at(d, p[4], 0);
    CHECK(strcmp(cp.family, "Calibri Light") == 0 && cp.size == PD_PT(16) && cp.weight == 700);
    cp = chars_at(d, p[4], 5);
    CHECK(strcmp(cp.family, "Calibri Light") == 0 && cp.size == PD_PT(10) && cp.weight == 400);
}

/* a document from the parts of a .docx: name, text, name, text, ..., NULL.
   A word/media/ part names a file whose bytes it holds. */
static pd_doc* docx_doc(const char* first, ...) {
    const char* names[16];
    const char* texts[16];
    size_t lens[16];
    char* files[16];
    int n = 0, i;
    va_list ap;
    const char* nm;
    buf_t z;
    pd_doc* d = NULL;

    va_start(ap, first);

    for (nm = first; nm && n < 16; nm = va_arg(ap, const char*)) {
        names[n] = nm;
        texts[n] = va_arg(ap, const char*);
        lens[n] = strlen(texts[n]);
        files[n] = NULL;

        if (strncmp(nm, "word/media/", 11) == 0) {
            FILE* f = fopen(texts[n], "rb");
            long sz;

            if (f && fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
                    (files[n] = (char*)malloc((size_t)sz)) != NULL && fread(files[n], 1, (size_t)sz, f) == (size_t)sz) {
                texts[n] = files[n];
                lens[n] = (size_t)sz;
            }

            if (f) {
                fclose(f);
            }
        }

        n++;
    }

    va_end(ap);
    z = stored_zip_n(names, texts, lens, n);

    for (i = 0; i < n; i++) {
        free(files[i]);
    }


    if (pd_doc_import(z.p, z.n, PD_CONV_DOCX, &d) != PD_OK) {
        d = NULL;
    }

    free(z.p);
    return d;
}

static void label_of(const pd_doc* d, pd_block_id p, char* buf) {
    buf[0] = '\0';
    pd_doc_list_label(d, p, buf, 32);
}

/* Word's numbering: formats, label templates, start values, overrides,
   bullets from the Symbol font, and indents measured from the margin. */
static void test_docx_lists(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>b</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>c</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"3\"/></w:numPr></w:pPr><w:r><w:t>d</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"2\"/></w:numPr></w:pPr><w:r><w:t>e</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"4\"/></w:numPr><w:ind w:left=\"1440\"/></w:pPr>"
        "<w:r><w:t>f</w:t></w:r></w:p>"
        "</w:body></w:document>",
        "word/numbering.xml",
        "<w:numbering xmlns:w=\"w\">"
        "<w:abstractNum w:abstractNumId=\"0\">"
        "<w:lvl w:ilvl=\"0\"><w:start w:val=\"3\"/><w:numFmt w:val=\"lowerRoman\"/><w:lvlText w:val=\"(%1)\"/>"
        "<w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl>"
        "<w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"upperLetter\"/><w:lvlText w:val=\"%1.%2\"/>"
        "<w:pPr><w:ind w:left=\"1080\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>"
        "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"bullet\"/>"
        "<w:lvlText w:val=\"\xEF\x82\xB7\"/><w:pPr><w:ind w:left=\"360\" w:hanging=\"360\"/></w:pPr>"
        "<w:rPr><w:rFonts w:ascii=\"Symbol\"/></w:rPr></w:lvl></w:abstractNum>"
        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
        "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
        "<w:num w:numId=\"3\"><w:abstractNumId w:val=\"0\"/><w:lvlOverride w:ilvl=\"0\">"
        "<w:startOverride w:val=\"1\"/></w:lvlOverride></w:num>"
        "<w:num w:numId=\"4\"><w:abstractNumId w:val=\"0\"/></w:num>"
        "</w:numbering>",
        NULL);
    pd_block_id sec, p[6];
    pd_para_props pp;
    char lab[32];
    int i;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    sec = pd_doc_child(d, pd_doc_root(d), 0);

    for (i = 0; i < 6; i++) {
        p[i] = pd_doc_child(d, sec, i);
    }

    label_of(d, p[0], lab);
    CHECK(strcmp(lab, "(iii)") == 0);              /* lower roman, from 3 */
    label_of(d, p[1], lab);
    CHECK(strcmp(lab, "iii.A") == 0);              /* a template naming two levels */
    label_of(d, p[2], lab);
    CHECK(strcmp(lab, "(iv)") == 0);               /* counting on */
    label_of(d, p[3], lab);
    CHECK(strcmp(lab, "(i)") == 0);                /* another num restarting at 1 */
    label_of(d, p[4], lab);
    CHECK(strcmp(lab, "\xE2\x80\xA2") == 0);       /* Symbol's bullet as the bullet it is */
    label_of(d, p[5], lab);
    CHECK(strcmp(lab, "(v)") == 0);                /* a num without overrides shares the count */

    /* text at the level's indent, from the margin; a paragraph's own indent replaces it */
    pd_doc_para_props(d, p[0], &pp);
    CHECK((pp.mask & PD_PP_INDENT_LEFT) && pp.indent_left == 0);
    pd_doc_para_props(d, p[5], &pp);
    CHECK((pp.mask & PD_PP_INDENT_LEFT) && pp.indent_left == PD_PT(72) - PD_PT(36));
    {
        pd_list_level lv[9];
        int32_t nl = 0;
        pd_block_info bi;

        pd_doc_block_info(d, p[0], &bi);
        CHECK(pd_doc_list_info(d, bi.list, &nl, lv) == PD_OK && lv[0].indent == PD_PT(36) &&
              lv[0].hanging == PD_PT(18) && lv[1].indent == PD_PT(54));
    }

    pd_doc_free(d);
}

/* the text of a story's first paragraph */
static const char* story_text(const pd_doc* d, pd_block_id story, uint32_t* n) {
    const char* t = "";

    *n = 0;

    if (story) {
        pd_doc_para_text(d, pd_doc_child(d, story, 0), &t, n);
    }

    return t;
}

static int field_at(const pd_doc* d, pd_block_id para, uint32_t off) {
    pd_inline o;

    return pd_doc_inline_at(d, at(para, off), &o) == PD_OK && o.kind == PD_INLINE_FIELD ? o.field : -1;
}

/* Headers and footers: the parts a section names, carried over to a
   section that names none, page-number fields computed rather than frozen,
   a title page, the page-number format and the header distance. */
static void test_docx_headers(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\">"
        "<Relationship Id=\"rId1\" Type=\"t/header\" Target=\"header1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"t/footer\" Target=\"footer1.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"t/footer\" Target=\"/word/footer2.xml\"/></Relationships>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:pPr><w:sectPr><w:headerReference w:type=\"default\" r:id=\"rId1\"/>"
        "<w:footerReference w:type=\"first\" r:id=\"rId2\"/><w:titlePg/>"
        "<w:pgNumType w:fmt=\"lowerRoman\" w:start=\"3\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"500\" w:footer=\"600\"/>"
        "</w:sectPr></w:pPr><w:r><w:t>One.</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>Two.</w:t></w:r></w:p>"
        "<w:sectPr><w:footerReference w:type=\"default\" r:id=\"rId3\"/></w:sectPr>"
        "</w:body></w:document>",
        "word/header1.xml",
        "<w:hdr xmlns:w=\"w\"><w:p><w:r><w:t xml:space=\"preserve\">Page </w:t></w:r>"
        "<w:fldSimple w:instr=\" PAGE \"><w:r><w:t>7</w:t></w:r></w:fldSimple>"
        "<w:r><w:t xml:space=\"preserve\"> of </w:t></w:r>"
        "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> NUMPAGES \\* MERGEFORMAT </w:instrText></w:r>"
        "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>9</w:t></w:r>"
        "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p></w:hdr>",
        "word/footer1.xml",
        "<w:ftr xmlns:w=\"w\"><w:p><w:r><w:t>First footer</w:t></w:r></w:p></w:ftr>",
        "word/footer2.xml",
        "<w:ftr xmlns:w=\"w\"><w:p><w:r><w:t>Second footer</w:t></w:r></w:p></w:ftr>",
        NULL);
    pd_block_id s1, s2, hp;
    pd_section_props a, b;
    const char* t;
    uint32_t n;
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        CHECK(d != NULL);

        if (!d) {
            return;
        }

        s1 = pd_doc_child(d, pd_doc_root(d), 0);
        s2 = pd_doc_child(d, pd_doc_root(d), 1);
        CHECK(s2 != 0 && pd_doc_section_props(d, s1, &a) == PD_OK && pd_doc_section_props(d, s2, &b) == PD_OK);

        CHECK(a.header != 0 && a.footer == 0 && a.footer_first != 0 && a.title_page == 1);
        CHECK(a.page_number_format == PD_NUM_LOWER_ROMAN && a.first_page_number == 3);
        CHECK(a.header_distance == 500 * 65536 / 20 && a.footer_distance == 600 * 65536 / 20);

        t = story_text(d, a.header, &n);
        CHECK(n == 15 && memcmp(t, "Page \xEF\xBF\xBC of \xEF\xBF\xBC", 15) == 0);    /* no frozen 7 or 9 */
        hp = pd_doc_child(d, a.header, 0);
        CHECK(field_at(d, hp, 5) == PD_FIELD_PAGE && field_at(d, hp, 12) == PD_FIELD_PAGES);
        t = story_text(d, a.footer_first, &n);
        CHECK(n == 12 && memcmp(t, "First footer", 12) == 0);

        /* the second section keeps the header, and the part is read once */
        CHECK(b.header == a.header && b.footer_first == a.footer_first);
        t = story_text(d, b.footer, &n);
        CHECK(n == 13 && memcmp(t, "Second footer", 13) == 0);
        CHECK(pd_doc_story_count(d) == 3);
    }

    pd_doc_free(d);
}

static pd_block_id first_table(const pd_doc* d) {
    pd_block_id sec = pd_doc_child(d, pd_doc_root(d), 0);
    pd_block_info si, bi;
    int32_t i;

    pd_doc_block_info(d, sec, &si);

    for (i = 0; i < si.child_count; i++) {
        pd_block_id k = pd_doc_child(d, sec, i);

        if (pd_doc_block_info(d, k, &bi) == PD_OK && bi.kind == PD_BLOCK_TABLE) {
            return k;
        }
    }

    return 0;
}

static pd_cell_props cell_props_at(const pd_doc* d, pd_block_id t, int32_t r, int32_t c) {
    pd_cell_props cp;

    memset(&cp, 0, sizeof(cp));
    pd_doc_cell_props(d, pd_doc_child(d, pd_doc_child(d, t, r), c), &cp);
    return cp;
}

static int has_text(const pd_doc* d, pd_block_id t, int32_t r, int32_t c, const char* s) {
    const char* tx;
    uint32_t n;

    pd_doc_para_text(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, r), c), 0), &tx, &n);
    return n == strlen(s) && memcmp(tx, s, n) == 0;
}

/* Word's table grid, width, alignment and borders; cells merged down the
   rows (vMerge) and their vertical alignment -- and the merge through the
   HTML and DOCX writers and back. */
static void test_docx_tables(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:tbl>"
        "<w:tblPr><w:tblW w:w=\"5000\" w:type=\"dxa\"/><w:jc w:val=\"center\"/><w:tblBorders>"
        "<w:top w:val=\"none\" w:sz=\"0\"/><w:left w:val=\"nil\"/><w:bottom w:val=\"none\"/><w:right w:val=\"none\"/>"
        "<w:insideH w:val=\"none\"/><w:insideV w:val=\"none\"/></w:tblBorders></w:tblPr>"
        "<w:tblGrid><w:gridCol w:w=\"2000\"/><w:gridCol w:w=\"3000\"/></w:tblGrid>"
        "<w:tr><w:tc><w:tcPr><w:vMerge w:val=\"restart\"/><w:vAlign w:val=\"center\"/></w:tcPr>"
        "<w:p><w:r><w:t>A</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>B1</w:t></w:r></w:p></w:tc></w:tr>"
        "<w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc><w:tc><w:p><w:r><w:t>B2</w:t></w:r></w:p></w:tc></w:tr>"
        "<w:tr><w:tc><w:p><w:r><w:t>C</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>D</w:t></w:r></w:p></w:tc></w:tr>"
        "</w:tbl><w:p/></w:body></w:document>",
        NULL);
    pd_doc* back = NULL;
    pd_block_id t;
    pd_table_props tp;
    pd_cell_props cp;
    buf_t b;
    int pass;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    t = first_table(d);
    CHECK(pd_doc_table_props(d, t, &tp) == PD_OK);
    CHECK(tp.width == 5000 * 65536 / 20 && tp.align == PD_ALIGN_CENTER && tp.border == 0);
    CHECK(tp.ncols == 2 && tp.col_width[0] == 2000 * 65536 / 20 && tp.col_width[1] == 3000 * 65536 / 20);
    cp = cell_props_at(d, t, 0, 0);
    CHECK(cp.merge_up == 0 && cp.valign == 1);
    CHECK(cell_props_at(d, t, 1, 0).merge_up == 1 && cell_props_at(d, t, 1, 1).merge_up == 0);
    CHECK(cell_props_at(d, t, 2, 0).merge_up == 0);

    /* out and back in: HTML as a rowspan, DOCX as vMerge */
    for (pass = 0; pass < 2; pass++) {
        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_DOCX : PD_CONV_HTML, to_buf, &b) == PD_OK);

        if (!pass) {
            CHECK(b.p && strstr(b.p, "rowspan=\"2\"") != NULL);
        }

        back = NULL;
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_DOCX : PD_CONV_HTML, &back) == PD_OK);
        free(b.p);

        if (back) {
            pd_block_id bt = first_table(back);

            CHECK(cell_props_at(back, bt, 1, 0).merge_up == 1 && has_text(back, bt, 1, 1, "B2"));
            CHECK(has_text(back, bt, 0, 0, "A") && has_text(back, bt, 2, 0, "C") && has_text(back, bt, 2, 1, "D"));
            CHECK(cell_props_at(back, bt, 2, 0).merge_up == 0);

            if (pass) {
                CHECK(pd_doc_table_props(back, bt, &tp) == PD_OK && tp.col_width[1] == 3000 * 65536 / 20);
            }

            pd_doc_free(back);
        }
    }

    pd_doc_free(d);
}

#define DRAWING(kind, inner) \
    "<w:r><w:drawing><wp:" kind " distT=\"0\" distB=\"0\" distL=\"114300\" distR=\"114300\">" inner \
    "<wp:extent cx=\"1270000\" cy=\"635000\"/>" \
    "<a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill></pic:pic>" \
    "</a:graphicData></a:graphic></wp:" kind "></w:drawing></w:r>"

/* Floating pictures: beside the text on the side Word put them when text
   wraps round them, across the column when it goes above and below; before
   the paragraph they are anchored at its start, after it otherwise. */
static void test_docx_floats(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
        "<w:p>" DRAWING("anchor", "<wp:positionH relativeFrom=\"column\"><wp:align>right</wp:align></wp:positionH>"
                        "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>12700</wp:posOffset></wp:positionV>"
                        "<wp:wrapSquare wrapText=\"bothSides\"/>")
        "<w:r><w:t>Text beside.</w:t></w:r></w:p>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Before </w:t></w:r>"
        DRAWING("anchor", "<wp:positionH relativeFrom=\"column\"><wp:posOffset>0</wp:posOffset></wp:positionH>"
                "<wp:wrapTopAndBottom/>")
        "<w:r><w:t>after.</w:t></w:r></w:p>"
        "<w:p><w:r><w:t xml:space=\"preserve\">In line </w:t></w:r>" DRAWING("inline", "") "</w:p>"
        "</w:body></w:document>",
        NULL);
    pd_block_id sec, k[6];
    pd_block_info bi[6];
    pd_float_props fp;
    const char* t;
    uint32_t n;
    int i;
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);

        for (i = 0; i < 6; i++) {
            k[i] = pd_doc_child(d, sec, i);
            memset(&bi[i], 0, sizeof(bi[i]));
            pd_doc_block_info(d, k[i], &bi[i]);
        }

        /* the first picture before its paragraph, on the right, text round it */
        CHECK(bi[0].kind == PD_BLOCK_FLOAT && bi[1].kind == PD_BLOCK_PARAGRAPH);
        CHECK(pd_doc_float_props(d, k[0], &fp) == PD_OK && fp.wrap == PD_WRAP_RIGHT && fp.width == PD_PT(100));
        CHECK(fp.gap == PD_PT(9) && (fp.placement & PD_PLACE_FORCE));
        pd_doc_para_text(d, k[1], &t, &n);
        CHECK(n == 12 && memcmp(t, "Text beside.", 12) == 0);

        /* the second after the paragraph it was anchored in the middle of, across the column */
        CHECK(bi[2].kind == PD_BLOCK_PARAGRAPH && bi[3].kind == PD_BLOCK_FLOAT);
        pd_doc_para_text(d, k[2], &t, &n);
        CHECK(n == 13 && memcmp(t, "Before after.", 13) == 0);
        CHECK(pd_doc_float_props(d, k[3], &fp) == PD_OK && fp.wrap == PD_WRAP_NONE);

        /* an inline picture stays in its line */
        pd_doc_para_text(d, k[4], &t, &n);
        CHECK(bi[4].kind == PD_BLOCK_PARAGRAPH && n == 11 && memcmp(t + 8, "\xEF\xBF\xBC", 3) == 0);
    }

    pd_doc_free(d);
}

/* a story holding one paragraph of text */
static pd_block_id story_of(pd_doc* d, const char* s) {
    pd_block_id st;

    pd_doc_insert_block(d, 0, -1, PD_BLOCK_STORY, &st);
    text(d, pd_doc_child(d, st, 0), s);
    return st;
}

/* what the DOCX writer keeps of a document made in Parade: its own styles
   (a redefined Normal, a custom style on top), direct paragraph properties,
   first and even page headers with the page numbers' format, and bookmarks */
static void test_docx_writer(void) {
    pd_doc* d;
    pd_block_id sec, p;
    pd_para_props pp;
    pd_char_props cp;
    pd_section_props sp;
    pd_style_id normal, lead;
    pd_inline o;
    int pass;

    pd_doc_new(&d);
    normal = pd_doc_style_find(d, "Normal");
    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    pp.mask = PD_PP_ALIGN | PD_PP_SPACE_AFTER;
    pp.align = PD_ALIGN_JUSTIFY;
    pp.space_after = PD_PT(4);
    cp.mask = PD_CP_FAMILY | PD_CP_SIZE;
    snprintf(cp.family, sizeof(cp.family), "Arial");
    cp.size = PD_PT(11);
    CHECK(pd_doc_style_define(d, "Normal", PD_STYLE_PARAGRAPH, 0, &pp, &cp, &normal) == PD_OK);
    pp.mask = PD_PP_INDENT_LEFT;
    pp.indent_left = PD_PT(18);
    cp.mask = PD_CP_ITALIC;
    cp.italic = 1;
    CHECK(pd_doc_style_define(d, "Lead Para", PD_STYLE_PARAGRAPH, normal, &pp, &cp, &lead) == PD_OK);

    sec = pd_doc_child(d, pd_doc_root(d), 0);
    p = pd_doc_child(d, sec, 0);
    text(d, p, "Plain.");
    p = para(d, sec, "Lead Para", 0, 0, "Lead.");
    p = para(d, sec, NULL, 0, 0, "Direct.");
    memset(&pp, 0, sizeof(pp));
    pp.mask = PD_PP_ALIGN | PD_PP_INDENT_LEFT | PD_PP_INDENT_RIGHT | PD_PP_INDENT_FIRST | PD_PP_SPACE_BEFORE |
              PD_PP_SPACE_AFTER | PD_PP_LINE_SPACING | PD_PP_KEEP_NEXT | PD_PP_KEEP_LINES;
    pp.align = PD_ALIGN_RIGHT;
    pp.indent_left = PD_PT(36);
    pp.indent_right = PD_PT(9);
    pp.indent_first = -PD_PT(18);
    pp.space_before = PD_PT(12);
    pp.space_after = PD_PT(3);
    pp.line_spacing = 1500;
    pp.keep_with_next = 1;
    pp.keep_lines = 1;
    CHECK(pd_doc_set_para_props(d, p, &pp) == PD_OK);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_BOOKMARK;
    snprintf(o.name, sizeof(o.name), "here");
    CHECK(pd_doc_insert_inline(d, at(p, 3), &o, NULL) == PD_OK);

    pd_doc_section_props(d, sec, &sp);
    sp.header = story_of(d, "Odd head");
    sp.header_first = story_of(d, "First head");
    sp.header_even = story_of(d, "Even head");
    sp.footer_even = story_of(d, "Even foot");
    sp.title_page = 1;
    sp.facing_pages = 1;
    sp.page_number_format = PD_NUM_UPPER_ROMAN;
    sp.first_page_number = 4;
    CHECK(pd_doc_set_section_props(d, sec, &sp) == PD_OK);

    for (pass = 0; pass < 3; pass++, d = docx_again(d)) {
        pd_block_id k[3];
        const char* t;
        uint32_t n;
        int i;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);

        for (i = 0; i < 3; i++) {
            k[i] = pd_doc_child(d, sec, i);
        }

        /* Normal as the document has it, and the look of the style built on it
           (read back as the paragraph's own properties) */
        cp = chars_at(d, k[0], 0);
        pp = para_resolved(d, k[0]);
        CHECK(strcmp(cp.family, "Arial") == 0 && cp.size == PD_PT(11) && !cp.italic);
        CHECK(pp.align == PD_ALIGN_JUSTIFY && pp.space_after == PD_PT(4));
        cp = chars_at(d, k[1], 0);
        pp = para_resolved(d, k[1]);
        CHECK(strcmp(cp.family, "Arial") == 0 && cp.italic && pp.indent_left == PD_PT(18));
        CHECK(pp.align == PD_ALIGN_JUSTIFY);

        /* the direct properties, and the bookmark in the text */
        pp = para_resolved(d, k[2]);
        CHECK(pp.align == PD_ALIGN_RIGHT && pp.indent_left == PD_PT(36) && pp.indent_right == PD_PT(9));
        CHECK(pp.indent_first == -PD_PT(18) && pp.space_before == PD_PT(12) && pp.space_after == PD_PT(3));
        CHECK(pp.line_spacing == 1500 && pp.keep_with_next && pp.keep_lines);
        CHECK(pd_doc_inline_at(d, at(k[2], 3), &o) == PD_OK && o.kind == PD_INLINE_BOOKMARK);
        CHECK(strcmp(o.name, "here") == 0);
        pd_doc_para_text(d, k[2], &t, &n);
        CHECK(n == 10 && memcmp(t, "Dir\xEF\xBF\xBC" "ect.", 10) == 0);

        /* the headers for each kind of page */
        CHECK(pd_doc_section_props(d, sec, &sp) == PD_OK && sp.title_page == 1 && sp.facing_pages == 1);
        CHECK(sp.page_number_format == PD_NUM_UPPER_ROMAN && sp.first_page_number == 4);
        t = story_text(d, sp.header, &n);
        CHECK(n == 8 && memcmp(t, "Odd head", 8) == 0);
        t = story_text(d, sp.header_first, &n);
        CHECK(n == 10 && memcmp(t, "First head", 10) == 0);
        t = story_text(d, sp.header_even, &n);
        CHECK(n == 9 && memcmp(t, "Even head", 9) == 0);
        t = story_text(d, sp.footer_even, &n);
        CHECK(n == 9 && memcmp(t, "Even foot", 9) == 0 && sp.footer == 0 && sp.footer_first == 0);
    }

    pd_doc_free(d);
}

/* Word's character effects: capitals, small capitals, hidden text, letter
   spacing, a raised run, kerning and the kinds of underline -- read, and
   kept through DOCX, HTML, RTF (what it has) and JData. */
static void check_effects(const pd_doc* d, const char* fmt) {
    pd_block_id p = pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0);
    pd_char_props c;
    int all = strcmp(fmt, "RTF") != 0;

    c = chars_at(d, p, 0);      /* "Caps" */
    CHECK(c.caps == 1 && !c.small_caps);
    c = chars_at(d, p, 5);      /* "Small" */
    CHECK(c.small_caps == 1 && !c.caps);
    c = chars_at(d, p, 11);     /* "dbl" */
    CHECK(c.underline == PD_UNDERLINE_DOUBLE);
    c = chars_at(d, p, 15);     /* "dot" */
    CHECK(c.underline == PD_UNDERLINE_DOTTED);
    c = chars_at(d, p, 19);     /* "wave" */
    CHECK(c.underline == PD_UNDERLINE_WAVY);
    c = chars_at(d, p, 24);     /* "words" */
    CHECK(c.underline == PD_UNDERLINE_WORDS);

    if (all) {
        c = chars_at(d, p, 30); /* "hide" */
        CHECK(c.hidden == 1);
        c = chars_at(d, p, 35); /* "wide" */
        CHECK(c.letter_space == PD_PT(2) && !c.hidden);
        c = chars_at(d, p, 40); /* "up" */
        CHECK(c.position == PD_PT(3));
    }

    if (strcmp(fmt, "DOCX") == 0 || strcmp(fmt, "JData") == 0) {
        c = chars_at(d, p, 0);  /* Word kerns only where it says so */
        CHECK(c.kerning == 0);
        c = chars_at(d, p, 40);
        CHECK(c.kerning == 1);
    }

    c = chars_at(d, p, 40);     /* and no further than their runs */
    CHECK(!c.caps && !c.small_caps && c.underline == 0);
}

static void test_docx_effects(void) {
    static const struct {
        pd_conv_format f;
        const char* name;
    } fmts[] = { { PD_CONV_DOCX, "DOCX" }, { PD_CONV_HTML, "HTML" }, { PD_CONV_RTF, "RTF" }, { PD_CONV_JDATA, "JData" } };
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:p>"
        "<w:r><w:rPr><w:caps/></w:rPr><w:t xml:space=\"preserve\">Caps </w:t></w:r>"
        "<w:r><w:rPr><w:smallCaps/></w:rPr><w:t xml:space=\"preserve\">Small </w:t></w:r>"
        "<w:r><w:rPr><w:u w:val=\"double\"/></w:rPr><w:t>dbl</w:t></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>"
        "<w:r><w:rPr><w:u w:val=\"dottedHeavy\"/></w:rPr><w:t>dot</w:t></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>"
        "<w:r><w:rPr><w:u w:val=\"wave\"/></w:rPr><w:t>wave</w:t></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>"
        "<w:r><w:rPr><w:u w:val=\"words\"/></w:rPr><w:t>words</w:t></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>"
        "<w:r><w:rPr><w:vanish/></w:rPr><w:t>hide</w:t></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>"
        "<w:r><w:rPr><w:spacing w:val=\"40\"/></w:rPr><w:t>wide</w:t></w:r><w:r><w:t xml:space=\"preserve\"> </w:t></w:r>"
        "<w:r><w:rPr><w:kern w:val=\"16\"/><w:position w:val=\"6\"/></w:rPr><w:t>up</w:t></w:r>"
        "</w:p></w:body></w:document>",
        NULL);
    size_t i;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    check_effects(d, "DOCX");

    for (i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
        buf_t b = { NULL, 0 };
        pd_doc* t = NULL;

        CHECK(pd_doc_export(d, fmts[i].f, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, fmts[i].f, &t) == PD_OK);

        if (t) {
            check_effects(t, fmts[i].name);
            pd_doc_free(t);
        }

        free(b.p);
    }

    pd_doc_free(d);
}

/* the note mark at a paragraph's byte offset: 0 footnote, 1 endnote, -1 none */
static int note_at(const pd_doc* d, pd_block_id para, uint32_t off, pd_block_id* story) {
    pd_inline o;

    if (pd_doc_inline_at(d, at(para, off), &o) != PD_OK || o.kind != PD_INLINE_FOOTNOTE) {
        return -1;
    }

    *story = o.target;
    return o.level;
}

/* Endnotes come in as endnotes, beside footnotes, and go out as endnotes:
   through DOCX and through JData. */
static void test_docx_endnotes(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>One</w:t></w:r>"
        "<w:r><w:endnoteReference w:id=\"1\"/></w:r><w:r><w:t xml:space=\"preserve\"> two</w:t></w:r>"
        "<w:r><w:footnoteReference w:id=\"1\"/></w:r></w:p></w:body></w:document>",
        "word/endnotes.xml",
        "<w:endnotes xmlns:w=\"w\"><w:endnote w:type=\"separator\" w:id=\"-1\"><w:p/></w:endnote>"
        "<w:endnote w:id=\"1\"><w:p><w:r><w:endnoteRef/></w:r><w:r><w:t xml:space=\"preserve\"> An endnote.</w:t>"
        "</w:r></w:p></w:endnote></w:endnotes>",
        "word/footnotes.xml",
        "<w:footnotes xmlns:w=\"w\"><w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/></w:r>"
        "<w:r><w:t xml:space=\"preserve\"> A footnote.</w:t></w:r></w:p></w:footnote></w:footnotes>",
        NULL);
    pd_doc* back;
    pd_block_id para, st = 0;
    const char* t;
    uint32_t n;
    buf_t b;
    int pass;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    for (pass = 0; pass < 3; pass++) {
        const pd_doc* x = d;

        back = NULL;

        if (pass > 0) {     /* 1: DOCX out and in, 2: JData */
            memset(&b, 0, sizeof(b));
            CHECK((pass == 1 ? pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) : pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b)) ==
                  PD_OK);
            CHECK((pass == 1 ? pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) : pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &back))
                  == PD_OK);
            free(b.p);

            if (!back) {
                continue;
            }

            x = back;
        }

        para = pd_doc_child(x, pd_doc_child(x, pd_doc_root(x), 0), 0);
        CHECK(note_at(x, para, 3, &st) == 1);
        pd_doc_para_text(x, pd_doc_child(x, st, 0), &t, &n);
        CHECK(n == 11 && memcmp(t, "An endnote.", 11) == 0);
        CHECK(note_at(x, para, 10, &st) == 0);
        pd_doc_para_text(x, pd_doc_child(x, st, 0), &t, &n);
        CHECK(n == 11 && memcmp(t, "A footnote.", 11) == 0);
        pd_doc_free(back);
    }

    pd_doc_free(d);
}

/* Word's tab stops: a style's, a paragraph adding to them and clearing one,
   leaders, the document's default interval; through JData and DOCX. */
static void test_docx_tabs(void) {
    pd_doc* d = docx_doc(
        "word/settings.xml", "<w:settings xmlns:w=\"w\"><w:defaultTabStop w:val=\"360\"/></w:settings>",
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"TOC\"><w:name w:val=\"TOC\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:tabs><w:tab w:val=\"left\" w:pos=\"2880\"/><w:tab w:val=\"right\" w:leader=\"dot\" w:pos=\"9000\"/>"
        "</w:tabs></w:pPr></w:style></w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"TOC\"/></w:pPr><w:r><w:t>One</w:t></w:r><w:r><w:tab/></w:r>"
        "<w:r><w:t>1</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"TOC\"/><w:tabs><w:tab w:val=\"clear\" w:pos=\"2880\"/>"
        "<w:tab w:val=\"decimal\" w:pos=\"7200\"/></w:tabs></w:pPr><w:r><w:t>Two</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>Plain</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    pd_block_id sec, p[3];
    pd_para_props pp, np;
    pd_block_info bi;
    int pass, i;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    for (pass = 0; pass < 3; pass++) {
        pd_doc* x = d;
        buf_t b;

        if (pass > 0) {     /* 1: DOCX out and in, 2: JData */
            memset(&b, 0, sizeof(b));
            x = NULL;
            CHECK((pass == 1 ? pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) : pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b)) ==
                  PD_OK);
            CHECK((pass == 1 ? pd_doc_import(b.p, b.n, PD_CONV_DOCX, &x) : pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &x)) ==
                  PD_OK);
            free(b.p);

            if (!x) {
                continue;
            }
        }

        sec = pd_doc_child(x, pd_doc_root(x), 0);

        for (i = 0; i < 3; i++) {
            p[i] = pd_doc_child(x, sec, i);
        }

        pp = para_resolved(x, p[0]);    /* the style's two stops */
        pd_doc_para_props(x, p[0], &np);

        if (np.mask & PD_PP_TABS) {
            pp.ntabs = np.ntabs;
            memcpy(pp.tabs, np.tabs, sizeof(pp.tabs));
        }

        CHECK(pp.ntabs == 2 && pp.tabs[0].position == PD_PT(144) && pp.tabs[0].align == PD_TAB_LEFT);
        CHECK(pp.tabs[1].position == PD_PT(450) && pp.tabs[1].align == PD_TAB_RIGHT &&
              pp.tabs[1].leader == PD_LEADER_DOT);

        pd_doc_para_props(x, p[1], &np);    /* one cleared, one added */
        CHECK((np.mask & PD_PP_TABS) && np.ntabs == 2 && np.tabs[0].position == PD_PT(360) &&
              np.tabs[0].align == PD_TAB_DECIMAL && np.tabs[1].position == PD_PT(450));

        if (pass < 2) {     /* the document's interval, on Normal (DOCX has no other place for it) */
            pd_doc_block_info(x, p[2], &bi);
            pd_doc_style_resolve(x, bi.style, &pp, NULL);
            CHECK(pass == 1 || pp.tab_interval == PD_PT(18));
        }

        if (x != d) {
            pd_doc_free(x);
        }
    }

    pd_doc_free(d);
}

static pd_doc* md_doc(const char* md) {
    pd_doc* d = NULL;

    return pd_doc_import(md, strlen(md), PD_CONV_MARKDOWN, &d) == PD_OK ? d : NULL;
}

/* the Markdown a document writes, NUL-terminated (caller frees) */
static char* md_of(const pd_doc* d) {
    buf_t b;

    memset(&b, 0, sizeof(b));

    if (pd_doc_export(d, PD_CONV_MARKDOWN, to_buf, &b) != PD_OK) {
        free(b.p);
        return NULL;
    }

    return b.p;
}

/* the n-th paragraph of the first section, in reading order of its top-level blocks */
static pd_block_id nth_para(const pd_doc* d, int32_t n) {
    return pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), n);
}

static int para_is(const pd_doc* d, pd_block_id p, const char* s) {
    const char* t;
    uint32_t n;

    return pd_doc_para_text(d, p, &t, &n) == PD_OK && n == strlen(s) && memcmp(t, s, n) == 0;
}

/* Markdown import: every list numbered from its own first item, nested
   lists restarting under each item; reference links and images; table
   column alignment; heading ids. And back out again. */
static void test_md_import(void) {
    pd_doc* d = md_doc("3. three\n4. four\n   1. sub one\n   2. sub two\n5. five\n   1. again one\n\n"
                       "- bullet\n\n7) other\n\n"
                       "See [the site][Site] and [Site][] and [site], not [nothing].\n\n"
                       "[site]:  <http://example.org/a b>  \"Title\"\n\n"
                       "| L | C | R | N |\n|:--|:-:|--:|---|\n| a | b | c | d |\n\n"
                       "## Section {#sec-1}\n\nSetext {#set}\n------\n\n<http://x.org> and [y](http://y.org)\n");
    pd_block_id p;
    char lab[32], *md;
    pd_para_props pp;
    pd_inline o;
    pd_block_id t;
    int i;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    {
        static const char* want[] = { "3.", "4.", "1.", "2.", "5.", "1.", "\xE2\x80\xA2", "7." };

        for (i = 0; i < 8; i++) {
            label_of(d, nth_para(d, i), lab);
            CHECK(strcmp(lab, want[i]) == 0);
        }
    }

    /* the references: three forms of one, case and spacing aside; the undefined one stays text */
    p = nth_para(d, 8);
    CHECK(pd_doc_inline_at(d, at(p, 4), &o) == PD_OK && o.kind == PD_INLINE_LINK && o.source &&
          o.source_len == 22 && memcmp(o.source, "http://example.org/a b", 22) == 0);
    {
        const char* tx;
        uint32_t n, k, links = 0;

        pd_doc_para_text(d, p, &tx, &n);

        for (k = 0; k + 2 < n; k++) {
            if (memcmp(tx + k, "\xEF\xBF\xBC", 3) == 0 && pd_doc_inline_at(d, at(p, k), &o) == PD_OK &&
                    o.kind == PD_INLINE_LINK && o.source_len > 0) {
                links++;
            }
        }

        CHECK(links == 3);
        for (k = 0; k + 9 <= n && memcmp(tx + k, "[nothing]", 9) != 0; k++) {
        }

        CHECK(k + 9 <= n);
    }

    t = first_table(d);
    {
        static const int want[] = { PD_ALIGN_LEFT, PD_ALIGN_CENTER, PD_ALIGN_RIGHT };

        for (i = 0; i < 3; i++) {
            pd_doc_para_props(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 1), i), 0), &pp);
            CHECK((pp.mask & PD_PP_ALIGN) && pp.align == want[i]);
        }

        pd_doc_para_props(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 1), 3), 0), &pp);
        CHECK(!(pp.mask & PD_PP_ALIGN));
    }

    /* the headings: the id an anchor at the start, not text */
    for (i = 0; i < 20 && !para_is(d, nth_para(d, i), "\xEF\xBF\xBCSection"); i++) {
    }

    p = nth_para(d, i);
    CHECK(i < 20 && pd_doc_inline_at(d, at(p, 0), &o) == PD_OK && o.kind == PD_INLINE_BOOKMARK &&
          strcmp(o.name, "sec-1") == 0);
    CHECK(para_is(d, nth_para(d, i + 1), "\xEF\xBF\xBCSetext"));

    /* and back out */
    md = md_of(d);
    CHECK(md && strstr(md, "3. three\n4. four\n   1. sub one\n   2. sub two\n5. five\n   1. again one") != NULL);
    CHECK(md && strstr(md, "7. other") != NULL);
    CHECK(md && strstr(md, "| :-- | :-: | --: | --- |") != NULL);
    CHECK(md && strstr(md, "## Section {#sec-1}") != NULL && strstr(md, "## Setext {#set}") != NULL);
    CHECK(md && strstr(md, "<http://x.org> and [y](http://y.org)") != NULL);

    if (md) {   /* which reads back the same */
        pd_doc* d2 = md_doc(md);
        char* md2 = d2 ? md_of(d2) : NULL;

        CHECK(md2 && strcmp(md, md2) == 0);
        free(md2);
        pd_doc_free(d2);
    }

    free(md);
    pd_doc_free(d);

    /* two lists of one kind side by side stay two */
    d = md_doc("- a\n- b\n\n<!-- -->\n\n- c\n");

    if (d) {
        md = md_of(d);
        label_of(d, nth_para(d, 2), lab);
        CHECK(md && strstr(md, "- a\n- b") && (strstr(md, "* c") || strstr(md, "\n\n- c")));
        free(md);
        pd_doc_free(d);
    }
}

static pd_para_attrs attrs_of(const pd_doc* d, pd_block_id p) {
    pd_para_attrs at;

    memset(&at, 0, sizeof(at));
    pd_doc_para_attrs(d, p, &at);
    return at;
}

static const char* md_all =
    "---\ntitle: Front matter\nauthor: Q\n---\n\n"
    "Text with [a link](http://x.org \"Link title\"), <span class=\"k\">raw</span> and "
    "![Alt *text*](pic.png \"Image title\").\n\n"
    "***\n\n"
    "> Quote one\n>\n> > Quote two\n>\n> ```c\n> int x;\n> ```\n>\n> - item in a quote\n\n"
    "- [ ] open task\n- [x] done task\n\n"
    "1. loose one\n\n2. loose two\n\n"
    "```python\ndef f():\n    pass\n```\n\n"
    "<div class=\"raw\">\n<b>raw</b> block\n</div>\n\n"
    "Term\n: Its definition\n";

/* Markdown's elements into the document model: a front matter, link and image titles, image alt
   text and a picture by address, raw inline and block HTML, a rule, quotes in quotes and code
   and lists in quotes, tasks, a loose list, a code block's language, a definition list. */
static void test_md_model(void) {
    pd_doc* d = md_doc(md_all), *d2;
    pd_block_id sec, p, q[16];
    pd_block_info bi;
    pd_inline o;
    const char* meta;
    size_t ml;
    char* md, *md2;
    int i, n;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    meta = pd_doc_metadata(d, &ml);
    CHECK(ml == 29 && memcmp(meta, "title: Front matter\nauthor: Q", 29) == 0);

    sec = pd_doc_child(d, pd_doc_root(d), 0);
    pd_doc_block_info(d, sec, &bi);
    n = bi.child_count < 16 ? bi.child_count : 16;

    for (i = 0; i < n; i++) {
        q[i] = pd_doc_child(d, sec, i);
    }

    /* the first paragraph: a titled link, raw tags, a picture by address with alt and title */
    p = q[0];
    CHECK(pd_doc_inline_at(d, at(p, 10), &o) == PD_OK && o.kind == PD_INLINE_LINK && o.title_len == 10 &&
          memcmp(o.title, "Link title", 10) == 0);
    {
        const char* t;
        uint32_t len, k;
        int raws = 0, img = 0;

        pd_doc_para_text(d, p, &t, &len);

        for (k = 0; k + 2 < len; k++) {
            if (memcmp(t + k, "\xEF\xBF\xBC", 3) == 0 && pd_doc_inline_at(d, at(p, k), &o) == PD_OK) {
                if (o.kind == PD_INLINE_RAW) {
                    raws++;
                    CHECK(raws != 1 || (o.source_len == 16 && memcmp(o.source, "<span class=\"k\">", 16) == 0));
                } else if (o.kind == PD_INLINE_IMAGE) {
                    img = o.resource == 0 && o.source_len == 7 && memcmp(o.source, "pic.png", 7) == 0 &&
                          o.alt_len == 10 && memcmp(o.alt, "Alt *text*", 10) == 0 && o.title_len == 11;
                }
            }
        }

        CHECK(raws == 2 && img);
    }

    pd_doc_block_info(d, q[1], &bi);
    CHECK(bi.kind == PD_BLOCK_BREAK && bi.break_kind == PD_BREAK_RULE);

    /* quotes: one, two deep, code in one, a list item in one */
    pd_doc_block_info(d, q[2], &bi);
    CHECK(bi.role == PD_ROLE_QUOTE && attrs_of(d, q[2]).quote_depth == 1 && para_is(d, q[2], "Quote one"));
    CHECK(attrs_of(d, q[3]).quote_depth == 2 && para_is(d, q[3], "Quote two"));
    pd_doc_block_info(d, q[4], &bi);
    CHECK(bi.role == PD_ROLE_CODE && attrs_of(d, q[4]).quote_depth == 1 && strcmp(attrs_of(d, q[4]).lang, "c") == 0 &&
          para_is(d, q[4], "int x;"));
    pd_doc_block_info(d, q[5], &bi);
    CHECK(bi.list && attrs_of(d, q[5]).quote_depth == 1 && para_is(d, q[5], "item in a quote"));

    /* tasks, tight; then a loose list */
    CHECK(attrs_of(d, q[6]).task == 1 && para_is(d, q[6], "open task") && !attrs_of(d, q[6]).loose);
    CHECK(attrs_of(d, q[7]).task == 2 && para_is(d, q[7], "done task"));
    CHECK(attrs_of(d, q[8]).loose && attrs_of(d, q[9]).loose);

    pd_doc_block_info(d, q[10], &bi);
    CHECK(bi.role == PD_ROLE_CODE && strcmp(attrs_of(d, q[10]).lang, "python") == 0 &&
          para_is(d, q[10], "def f():\n    pass"));
    pd_doc_block_info(d, q[11], &bi);
    CHECK(bi.role == PD_ROLE_RAW && para_is(d, q[11], "<div class=\"raw\">\n<b>raw</b> block\n</div>"));
    pd_doc_block_info(d, q[12], &bi);
    CHECK(bi.role == PD_ROLE_TERM && para_is(d, q[12], "Term"));
    pd_doc_block_info(d, q[13], &bi);
    CHECK(bi.role == PD_ROLE_DEFINITION && para_is(d, q[13], "Its definition"));

    /* back out, as it was written; and the same again from that */
    md = md_of(d);
    CHECK(md && strcmp(md, md_all) == 0);

    if (md && strcmp(md, md_all) != 0) {
        printf("--- got:\n%s--- wanted:\n%s---\n", md, md_all);
    }

    d2 = md ? md_doc(md) : NULL;
    md2 = d2 ? md_of(d2) : NULL;
    CHECK(md2 && md && strcmp(md, md2) == 0);
    free(md2);
    pd_doc_free(d2);
    free(md);

    /* the same through JData */
    {
        buf_t b;
        pd_doc* j = NULL;

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_save(d, PD_JDATA_BINARY, to_buf, &b) == PD_OK);
        CHECK(pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &j) == PD_OK);
        free(b.p);
        md = j ? md_of(j) : NULL;
        CHECK(md && strcmp(md, md_all) == 0);
        free(md);
        pd_doc_free(j);
    }

    /* and through HTML, which has all of it but the front matter, the raw markup and looseness */
    {
        buf_t b;
        pd_doc* h = NULL;

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &b) == PD_OK);
        CHECK(b.p && strstr(b.p, "<hr>") && strstr(b.p, "<blockquote>\n<p>Quote one</p>\n<blockquote>") &&
              strstr(b.p, "class=\"language-python\"") && strstr(b.p, "<input type=\"checkbox\" disabled checked>") &&
              strstr(b.p, "<dt>Term</dt>") && strstr(b.p, "title=\"Link title\"") &&
              strstr(b.p, "<img src=\"pic.png\" alt=\"Alt *text*\" title=\"Image title\">"));
        CHECK(pd_doc_import(b.p, b.n, PD_CONV_HTML, &h) == PD_OK);
        free(b.p);
        md = h ? md_of(h) : NULL;
        CHECK(md && strstr(md, "[a link](http://x.org \"Link title\")") && strstr(md, "***\n") &&
              strstr(md, "> Quote one\n>\n> > Quote two") && strstr(md, "```python\n") &&
              strstr(md, "- [ ] open task\n- [x] done task") && strstr(md, "Term\n: Its definition") &&
              strstr(md, "![Alt *text*](pic.png \"Image title\")"));
        free(md);
        pd_doc_free(h);
    }

    pd_doc_free(d);
}

/* the Markdown a document writes after reading md */
static char* md_again(const char* md) {
    pd_doc* d = md_doc(md);
    char* out = d ? md_of(d) : NULL;

    pd_doc_free(d);
    return out;
}

/* more of Markdown: several blocks in a list item, headings in items, pandoc's code attributes,
   GFM's bare links and single-tilde strike, pandoc's sub/superscript, inline notes, line blocks and
   fenced divs, GitHub's alerts, HTML blocks of any tag, references inside quotes and items */
static void test_md_more(void) {
    static const char* canon =
        "1. first item\n\n   second paragraph\n\n   ```c\n   int x;\n   ```\n\n2. next item\n\n"
        "- ## A heading item\n- plain item\n\n"
        "::: warning\n\nIn a div.\n\n:::\n\n"
        "> [!TIP]\n> A tip.\n\n> Plain quote.\n";
    pd_doc* d;
    pd_block_id p;
    pd_para_attrs pa;
    pd_block_info bi;
    pd_inline o;
    char* md, lab[32];
    int i;

    /* the canonical form reads back as itself, also through HTML and JData */
    md = md_again(canon);
    CHECK(md && strcmp(md, canon) == 0);

    if (md && strcmp(md, canon) != 0) {
        printf("--- got:\n%s--- wanted:\n%s---\n", md, canon);
    }

    free(md);
    d = md_doc(canon);

    if (!d) {
        CHECK(0);
        return;
    }

    {
        buf_t b;
        pd_doc* h = NULL, *j = NULL;

        memset(&b, 0, sizeof(b));
        pd_doc_export(d, PD_CONV_HTML, to_buf, &b);
        pd_doc_import(b.p, b.n, PD_CONV_HTML, &h);
        free(b.p);
        md = h ? md_of(h) : NULL;
        CHECK(md && strcmp(md, canon) == 0);
        free(md);
        memset(&b, 0, sizeof(b));
        pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b);
        pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &j);
        free(b.p);
        md = j ? md_of(j) : NULL;
        CHECK(md && strcmp(md, canon) == 0);
        free(md);
        pd_doc_free(h);
        pd_doc_free(j);
    }

    /* the item's later blocks: in its list, no label, not counted */
    p = nth_para(d, 1);
    pa = attrs_of(d, p);
    pd_doc_block_info(d, p, &bi);
    CHECK(pa.cont == 1 && bi.list != 0 && para_is(d, p, "second paragraph"));
    label_of(d, p, lab);
    CHECK(lab[0] == '\0');
    label_of(d, nth_para(d, 3), lab);
    CHECK(strcmp(lab, "2.") == 0);
    pd_doc_block_info(d, nth_para(d, 4), &bi);
    CHECK(bi.role == PD_ROLE_HEADING && bi.level == 2 && bi.list != 0);
    CHECK(strcmp(attrs_of(d, nth_para(d, 6)).div_class, "warning") == 0);
    CHECK(strcmp(attrs_of(d, nth_para(d, 7)).div_class, "!tip") == 0 && attrs_of(d, nth_para(d, 7)).quote_depth == 1);
    CHECK(attrs_of(d, nth_para(d, 8)).div_class[0] == '\0' && attrs_of(d, nth_para(d, 8)).quote_depth == 1);
    pd_doc_free(d);

    /* inline extensions */
    d = md_doc("Go to www.example.com, or https://x.org/a_(b).\n\nH~2~O, ~gone~, E=mc^2^, a note^[Inline *note*].\n");

    if (d) {
        const char* t;
        uint32_t n, k;
        int links = 0, notes = 0;

        p = nth_para(d, 0);
        pd_doc_para_text(d, p, &t, &n);

        for (k = 0; k + 2 < n; k++) {
            if (memcmp(t + k, "\xEF\xBF\xBC", 3) == 0 && pd_doc_inline_at(d, at(p, k), &o) == PD_OK &&
                    o.kind == PD_INLINE_LINK && o.source_len > 0) {
                links++;
                CHECK((o.source_len == 22 && memcmp(o.source, "http://www.example.com", 22) == 0) ||
                      (o.source_len == 19 && memcmp(o.source, "https://x.org/a_(b)", 19) == 0));
            }
        }

        CHECK(links == 2);
        p = nth_para(d, 1);
        pd_doc_para_text(d, p, &t, &n);

        for (k = 0; k + 2 < n; k++) {
            notes += memcmp(t + k, "\xEF\xBF\xBC", 3) == 0 && pd_doc_inline_at(d, at(p, k), &o) == PD_OK &&
                     o.kind == PD_INLINE_FOOTNOTE;
        }

        CHECK(notes == 1);
        CHECK(chars_at(d, p, 1).shift == PD_SHIFT_SUB && chars_at(d, p, 6).strike && !chars_at(d, p, 6).shift);
        CHECK(chars_at(d, p, 15).shift == PD_SHIFT_SUPER);
        md = md_of(d);
        CHECK(md && strstr(md, "Go to www.example.com, or <https://x.org/a_(b)>.") != NULL);
        free(md);
        pd_doc_free(d);
    }

    /* pandoc's code attributes, line blocks; any tag alone on a line; references in containers */
    d = md_doc("```{.python .numberLines}\nx = 1\n```\n\n| one\n| two\n\n<my-tag a=\"1\">\nraw\n</my-tag>\n\n"
               "> See [q].\n>\n> [q]: http://q.org\n\n- See [i].\n\n  [i]: http://i.org\n");

    if (d) {
        CHECK(strcmp(attrs_of(d, nth_para(d, 0)).lang, "python") == 0);
        CHECK(para_is(d, nth_para(d, 1), "one\ntwo"));
        pd_doc_block_info(d, nth_para(d, 2), &bi);
        CHECK(bi.role == PD_ROLE_RAW && para_is(d, nth_para(d, 2), "<my-tag a=\"1\">\nraw\n</my-tag>"));

        for (i = 3; i <= 4; i++) {
            CHECK(pd_doc_inline_at(d, at(nth_para(d, i), 4), &o) == PD_OK && o.kind == PD_INLINE_LINK &&
                  o.source_len == 12);
        }

        pd_doc_free(d);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    if (getenv("PARADE_CONV_OUT")) {
        write_files(getenv("PARADE_CONV_OUT"));
    }

    printf("round trips\n");
    test_roundtrip();
    printf("paste\n");
    test_paste();
    printf("ranges\n");
    test_range();
    printf("docx styles\n");
    test_docx_styles();
    printf("docx lists\n");
    test_docx_lists();
    printf("docx headers and footers\n");
    test_docx_headers();
    printf("docx tables\n");
    test_docx_tables();
    printf("docx floats\n");
    test_docx_floats();
    printf("docx writer\n");
    test_docx_writer();
    printf("docx character effects\n");
    test_docx_effects();
    printf("docx endnotes\n");
    test_docx_endnotes();
    printf("docx tab stops\n");
    test_docx_tabs();
    printf("markdown import\n");
    test_md_import();
    printf("markdown elements\n");
    test_md_model();
    printf("markdown, more\n");
    test_md_more();
    printf("malformed input\n");
    test_fuzz();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
