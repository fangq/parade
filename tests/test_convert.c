/*
 * Parade import/export tests: every format round-trips a document with
 * headings, character formats, links, lists, quotes, code, a table, a
 * figure and a footnote; clipboard ranges and paste; malformed input.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_convert.h"
#include "parade_layout.h"
#include "../src/pd_conv.h"

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
        { PD_CONV_DOCX, "docx" }, { PD_CONV_TEXT, "txt" }, { PD_CONV_JDATA, "jdoc" }
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
    if (o.mask & PD_PP_SHADING) r.shading = o.shading;
    if (o.mask & PD_PP_SHADING) r.shading_theme = o.shading_theme;
    if (o.mask & PD_PP_BORDER) {
        r.border_color = o.border_color;
        r.border_width = o.border_width;
        r.border_sides = o.border_sides;
        r.border_space = o.border_space;
    }
    if (o.mask & PD_PP_KEEP_NEXT) r.keep_with_next = o.keep_with_next;
    if (o.mask & PD_PP_KEEP_LINES) r.keep_lines = o.keep_lines;
    if (o.mask & PD_PP_HYPHENATE) r.hyphenate = o.hyphenate;
    if (o.mask & PD_PP_SNAP_GRID) r.snap_grid = o.snap_grid;
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

/* the first paragraph under a block, depth first, whose text begins with T; 0 if none */
static pd_block_id para_under_with(const pd_doc* d, pd_block_id b, const char* t) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(d, b, &bi) != PD_OK) {
        return 0;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        const char* s;
        uint32_t n;

        return pd_doc_para_text(d, b, &s, &n) == PD_OK && n >= strlen(t) && !memcmp(s, t, strlen(t)) ? b : 0;
    }

    for (i = 0; i < bi.child_count; i++) {
        pd_block_id f = para_under_with(d, pd_doc_child(d, b, i), t);

        if (f) {
            return f;
        }
    }

    return 0;
}

/* the paragraph of a story (a drawing's text box, a note) whose text begins with T; 0 if none */
static pd_block_id story_para_with(const pd_doc* d, const char* t) {
    int32_t i;

    for (i = 0; i < pd_doc_story_count(d); i++) {
        pd_block_id f = para_under_with(d, pd_doc_story_at(d, i), t);

        if (f) {
            return f;
        }
    }

    return 0;
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

    /* a custom style, a Parade style of its name: justified Arial 11, its paragraph mark's size ignored */
    pd_doc_block_info(d, p[1], &bi);
    CHECK(bi.style != 0 && strcmp(pd_doc_style_name(d, bi.style), "LeadingPara") == 0);
    pd_doc_block_info(d, p[2], &bi);
    CHECK(bi.style != 0 && strcmp(pd_doc_style_name(d, bi.style), "Body2") == 0);
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

        if (strncmp(nm, "word/media/", 11) == 0 || strncmp(nm, "word/fonts/", 11) == 0) {
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
    pd_doc_cell_resolve(d, pd_doc_child(d, pd_doc_child(d, t, r), c), &cp);     /* as it shows */
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

        /* Normal as the document has it, and the style built on it, by name */
        cp = chars_at(d, k[0], 0);
        pp = para_resolved(d, k[0]);
        CHECK(strcmp(cp.family, "Arial") == 0 && cp.size == PD_PT(11) && !cp.italic);
        CHECK(pp.align == PD_ALIGN_JUSTIFY && pp.space_after == PD_PT(4));
        {
            pd_block_info bi;

            CHECK(pd_doc_block_info(d, k[1], &bi) == PD_OK && bi.style == pd_doc_style_find(d, "Lead Para") &&
                  bi.style != 0);
        }

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

/* contextual spacing read from Word's styles and kept; Word's sections add
   the space after to the space before */
static void test_docx_contextual(void) {
    pd_doc* d = docx_doc(
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"160\"/></w:pPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/>"
        "<w:basedOn w:val=\"Normal\"/><w:pPr><w:ind w:left=\"720\"/><w:contextualSpacing/></w:pPr></w:style>"
        "</w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/></w:pPr><w:r><w:t>one</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"ListParagraph\"/></w:pPr><w:r><w:t>two</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>body</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec;
        pd_block_info bi;
        pd_para_props pp;
        pd_section_props sp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        CHECK(pd_doc_section_props(d, sec, &sp) == PD_OK && sp.add_spacing == 1);
        pd_doc_block_info(d, pd_doc_child(d, sec, 1), &bi);
        CHECK(bi.style != 0 && strcmp(pd_doc_style_name(d, bi.style), "List Paragraph") == 0);
        pd_doc_style_resolve(d, bi.style, &pp, NULL);
        CHECK(pp.contextual == 1 && pp.indent_left == PD_PT(36) && pp.space_after == PD_PT(8));
        pd_doc_block_info(d, pd_doc_child(d, sec, 2), &bi);
        pd_doc_style_resolve(d, bi.style, &pp, NULL);
        CHECK(pp.contextual == 0);
    }

    pd_doc_free(d);
}

/* Word's paragraph borders and shading: edges by side, their space and
   colour, rules between, a style's border taken away by the paragraph --
   read and kept through DOCX */
static void test_docx_borders(void) {
    pd_doc* d = docx_doc(
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Boxed\"><w:name w:val=\"Boxed\"/><w:basedOn w:val=\"Normal\"/>"
        "<w:pPr><w:pBdr><w:top w:val=\"single\" w:sz=\"8\" w:space=\"4\" w:color=\"FF0000\"/>"
        "<w:left w:val=\"single\" w:sz=\"8\" w:space=\"4\" w:color=\"FF0000\"/>"
        "<w:bottom w:val=\"single\" w:sz=\"8\" w:space=\"4\" w:color=\"FF0000\"/>"
        "<w:right w:val=\"single\" w:sz=\"8\" w:space=\"4\" w:color=\"FF0000\"/>"
        "<w:between w:val=\"single\" w:sz=\"4\" w:space=\"1\" w:color=\"FF0000\"/></w:pBdr>"
        "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"DDEEFF\"/></w:pPr></w:style></w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:pPr><w:pStyle w:val=\"Boxed\"/></w:pPr><w:r><w:t>boxed</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"12\" w:space=\"1\" w:color=\"auto\"/></w:pBdr></w:pPr>"
        "<w:r><w:t>ruled</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Boxed\"/><w:pBdr><w:top w:val=\"nil\"/><w:left w:val=\"nil\"/>"
        "<w:bottom w:val=\"nil\"/><w:right w:val=\"nil\"/><w:between w:val=\"nil\"/></w:pBdr>"
        "<w:shd w:val=\"clear\" w:fill=\"auto\"/></w:pPr><w:r><w:t>bare</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec;
        pd_para_props pp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        pp = para_resolved(d, pd_doc_child(d, sec, 0));
        CHECK(pp.border_color == 0xFFFF0000u && pp.border_width == PD_PT(1) && pp.border_space == PD_PT(4));
        CHECK(pp.border_sides == (PD_BORDER_TOP | PD_BORDER_LEFT | PD_BORDER_BOTTOM | PD_BORDER_RIGHT |
                                  PD_BORDER_BETWEEN) && pp.shading == 0xFFDDEEFFu);
        pp = para_resolved(d, pd_doc_child(d, sec, 1));
        CHECK(pp.border_color == 0xFF000000u && pp.border_width == PD_PT(1.5) && pp.border_sides == PD_BORDER_BOTTOM);
        CHECK(pp.border_space == PD_PT(1) && pp.shading == 0);
        pp = para_resolved(d, pd_doc_child(d, sec, 2));
        CHECK(pp.border_color == 0 && pp.shading == 0);
    }

    pd_doc_free(d);
}

static char* drawing_json(const pd_doc* d, pd_res_id r);

/* The font table: alternate names, kind, pitch and PANOSE, kept in the model and written back; a resolver's
   substitute chosen by them before the name: a sans that is not called one, a fixed-pitch face. */
static void test_docx_font_table(void) {
    pd_doc* d = docx_doc(
        "word/fontTable.xml",
        "<w:fonts xmlns:w=\"w\">"
        "<w:font w:name=\"Grotesk Pro\"><w:panose1 w:val=\"020b0604020202020204\"/><w:family w:val=\"roman\"/>"
        "<w:pitch w:val=\"variable\"/></w:font>"
        "<w:font w:name=\"Ledger\"><w:family w:val=\"modern\"/><w:pitch w:val=\"fixed\"/></w:font>"
        "<w:font w:name=\"Plain Helvet\"><w:family w:val=\"swiss\"/></w:font>"
        "<w:font w:name=\"MS Mincho\"><w:altName w:val=\"\xef\xbc\xad\xef\xbc\xb3 \xe6\x98\x8e\xe6\x9c\x9d\"/>"
        "<w:panose1 w:val=\"02020609040205080304\"/><w:family w:val=\"modern\"/><w:pitch w:val=\"fixed\"/></w:font>"
        "<w:font w:name=\"Arial\"/>"
        "<w:font w:name=\"FreeSans\"><w:family w:val=\"roman\"/></w:font>"
        "<w:font w:name=\"Kaku\"><w:charset w:val=\"80\"/><w:family w:val=\"modern\"/><w:pitch w:val=\"fixed\"/></w:font>"
        "<w:font w:name=\"TNRPSMT\"><w:altName w:val=\"Foo,Times New Roman\"/><w:panose1 w:val=\"020b0604020202020204\"/>"
        "</w:font></w:fonts>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>x</w:t></w:r></w:p></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_font_info fi;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(pd_doc_font_class(d, "Grotesk Pro") == PD_FAMILY_SANS);   /* PANOSE over w:family and the name */
        CHECK(pd_doc_font_class(d, "Ledger") == PD_FAMILY_MONO);
        CHECK(pd_doc_font_class(d, "plain helvet") == PD_FAMILY_SANS);  /* ignoring case */
        CHECK(pd_doc_font_class(d, "MS Mincho") == PD_FAMILY_SERIF);     /* the name says, not its fixed pitch */
        CHECK(pd_doc_font_class(d, "FreeSans") == PD_FAMILY_SANS);       /* nor a generator's "roman" */
        CHECK(pd_doc_font_class(d, "Kaku") == PD_FAMILY_SERIF);          /* a CJK face's fixed pitch: not mono */
        CHECK(pd_doc_font_class(d, "TNRPSMT") == PD_FAMILY_SERIF);       /* an alternate name that says */
        CHECK(pd_font_family_class("Arial Unicode MS") == PD_FAMILY_SANS && pd_font_family_class("SimHei") == PD_FAMILY_SANS);
        CHECK(pd_font_family_class("Fira Code") == PD_FAMILY_MONO && pd_font_family_class("Noto Serif") == PD_FAMILY_SERIF);
        CHECK(pd_doc_font_class(d, "Unlisted Sans") == PD_FAMILY_SANS && pd_doc_font_class(NULL, "Arial") == PD_FAMILY_SANS);
        CHECK(pd_doc_font_info(d, "MS Mincho", &fi) == PD_OK && strcmp(fi.alt, "\xef\xbc\xad\xef\xbc\xb3 \xe6\x98\x8e\xe6\x9c\x9d") == 0);
        CHECK(fi.generic == PD_FONT_GENERIC_MODERN && fi.pitch == 1 && fi.panose[0] == 2 && fi.panose[9] == 4);
        CHECK(fi.charset == -1 && pd_doc_font_info(d, "Kaku", &fi) == PD_OK && fi.charset == 0x80);
        CHECK(pd_doc_font_info(d, "Arial", &fi) == PD_OK && fi.generic == 0 && fi.pitch == 0 && fi.panose[0] == 0);
        CHECK(pd_doc_font_info(d, "Nowhere", &fi) == PD_ERR_RANGE);
    }

    pd_doc_free(d);
}

/* the byte offset of the n-th inline object (U+FFFC) of a paragraph, or UINT32_MAX */
static uint32_t nth_object(const pd_doc* d, pd_block_id p, int n) {
    const char* s;
    uint32_t len, i;

    if (pd_doc_para_text(d, p, &s, &len) != PD_OK) {
        return UINT32_MAX;
    }

    for (i = 0; i + 3 <= len; i++) {
        if ((unsigned char)s[i] == 0xEF && (unsigned char)s[i + 1] == 0xBF && (unsigned char)s[i + 2] == 0xBC && n-- == 0) {
            return i;
        }
    }

    return UINT32_MAX;
}

/* Content controls: a check box, a drop-down list, a date and a plain text box in a paragraph, one inside
   another; one around paragraphs keeps its content only. Read as controls with what they hold, and kept
   through DOCX; exports that have no such thing keep the content. */
static void test_docx_controls(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:w14=\"w14\"><w:body>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Agree </w:t></w:r>"
        "<w:sdt><w:sdtPr><w:alias w:val=\"Agree\"/><w:tag w:val=\"ok\"/><w:id w:val=\"7\"/><w14:checkbox>"
        "<w14:checked w14:val=\"1\"/><w14:checkedState w14:val=\"2612\" w14:font=\"MS Gothic\"/>"
        "<w14:uncheckedState w14:val=\"2610\" w14:font=\"MS Gothic\"/></w14:checkbox></w:sdtPr><w:sdtEndPr/>"
        "<w:sdtContent><w:r><w:t>\xe2\x98\x92</w:t></w:r></w:sdtContent></w:sdt>"
        "<w:r><w:t xml:space=\"preserve\"> colour </w:t></w:r>"
        "<w:sdt><w:sdtPr><w:tag w:val=\"col\"/><w:dropDownList w:lastValue=\"g\"><w:listItem w:displayText=\"Red\" "
        "w:value=\"r\"/><w:listItem w:displayText=\"Green\" w:value=\"g\"/></w:dropDownList></w:sdtPr>"
        "<w:sdtContent><w:r><w:t>Green</w:t></w:r></w:sdtContent></w:sdt>"
        "<w:r><w:t xml:space=\"preserve\"> on </w:t></w:r>"
        "<w:sdt><w:sdtPr><w:date w:fullDate=\"2026-10-07T00:00:00Z\"><w:dateFormat w:val=\"M/d/yyyy\"/>"
        "<w:lid w:val=\"en-US\"/></w:date></w:sdtPr><w:sdtContent><w:r><w:t>10/7/2026</w:t></w:r></w:sdtContent></w:sdt>"
        "<w:r><w:t xml:space=\"preserve\"> by </w:t></w:r>"
        "<w:sdt><w:sdtPr><w:showingPlcHdr/><w:text/></w:sdtPr><w:sdtContent><w:r><w:t>name: </w:t></w:r>"
        "<w:sdt><w:sdtPr><w:text/></w:sdtPr><w:sdtContent><w:r><w:t>inner</w:t></w:r></w:sdtContent></w:sdt>"
        "</w:sdtContent></w:sdt></w:p>"
        "<w:sdt><w:sdtPr><w:docPartObj><w:docPartGallery w:val=\"Cover Pages\"/></w:docPartObj></w:sdtPr>"
        "<w:sdtContent><w:p><w:r><w:t>Cover</w:t></w:r></w:p></w:sdtContent></w:sdt>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, p;
        pd_inline o;
        pd_pos a, b;
        const char* s;
        uint32_t n, k;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        p = pd_doc_child(d, sec, 0);
        CHECK(pd_doc_para_text(d, p, &s, &n) == PD_OK);
        k = nth_object(d, p, 0);
        CHECK(k == 6 && pd_doc_inline_at(d, at(p, k), &o) == PD_OK && o.kind == PD_INLINE_CONTROL);
        CHECK(!strcmp(o.name, "checkbox") && o.source_len > 0);
        CHECK(strstr(o.source, "\"checked\":1") && strstr(o.source, "\"on\":\"2612\"") && strstr(o.source, "\"tag\":\"ok\""));
        CHECK(strstr(o.source, "\"onfont\":\"MS Gothic\"") && strstr(o.source, "\"title\":\"Agree\""));
        CHECK(pd_doc_control_at(d, at(p, k + 3), &a, &b) == PD_OK && a.offset == k && b.offset == k + 6);  /* ☒ */
        CHECK(memcmp(s + k + 3, "\xe2\x98\x92", 3) == 0);
        CHECK(pd_doc_control_at(d, at(p, k), &a, &b) == PD_ERR_RANGE);          /* before it */
        CHECK(pd_doc_control_at(d, at(p, k + 9), &a, &b) == PD_ERR_RANGE);      /* after its end */
        k = nth_object(d, p, 2);
        CHECK(pd_doc_inline_at(d, at(p, k), &o) == PD_OK && !strcmp(o.name, "dropdown"));
        CHECK(strstr(o.source, "\"items\":[[\"Red\",\"r\"],[\"Green\",\"g\"]]") && strstr(o.source, "\"value\":\"g\""));
        CHECK(pd_doc_control_at(d, at(p, k + 5), &a, &b) == PD_OK && b.offset - a.offset - 3 == 5);   /* Green */
        k = nth_object(d, p, 4);
        CHECK(pd_doc_inline_at(d, at(p, k), &o) == PD_OK && !strcmp(o.name, "date"));
        CHECK(strstr(o.source, "\"date\":\"2026-10-07T00:00:00Z\"") && strstr(o.source, "\"format\":\"M/d/yyyy\""));
        k = nth_object(d, p, 6);
        CHECK(pd_doc_inline_at(d, at(p, k), &o) == PD_OK && !strcmp(o.name, "text") && strstr(o.source, "\"placeholder\":1"));
        /* inside the inner box: the inner one; after it, the outer one */
        CHECK(pd_doc_control_at(d, at(p, nth_object(d, p, 7) + 4), &a, &b) == PD_OK && a.offset == nth_object(d, p, 7));
        CHECK(b.offset == nth_object(d, p, 8));
        CHECK(pd_doc_control_at(d, at(p, nth_object(d, p, 8) + 3), &a, &b) == PD_OK && a.offset == k);
        CHECK(b.offset == nth_object(d, p, 9) && nth_object(d, p, 10) == UINT32_MAX);
        /* around a paragraph: its content, no control */
        p = pd_doc_child(d, sec, 1);
        CHECK(pd_doc_para_text(d, p, &s, &n) == PD_OK && n == 5 && memcmp(s, "Cover", 5) == 0);
    }

    if (d) {
        buf_t t = { NULL, 0 };

        CHECK(pd_doc_export(d, PD_CONV_TEXT, to_buf, &t) == PD_OK);
        CHECK(t.p && strstr(t.p, "Agree \xe2\x98\x92 colour Green on 10/7/2026 by name: inner") != NULL);
        free(t.p);
        t.p = NULL;
        t.n = 0;
        CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &t) == PD_OK);
        CHECK(t.p && strstr(t.p, "colour Green on") != NULL);
        free(t.p);
    }

    pd_doc_free(d);
}

/* the bytes of needle in the n bytes at p */
static int has_mem(const char* p, size_t n, const char* needle) {
    size_t k = strlen(needle), i;

    for (i = 0; p && i + k <= n; i++) {
        if (!memcmp(p + i, needle, k)) {
            return 1;
        }
    }

    return 0;
}

/* how many times needle is in s */
static int count_of(const char* s, const char* needle) {
    int n = 0;

    for (; s && (s = strstr(s, needle)) != NULL; s += strlen(needle)) {
        n++;
    }

    return n;
}

/* Charts: Word draws them from the values the part caches, and so does Parade -- a column chart's bars,
   gridlines, labels, legend and title; a pie's wedges and percentages. The part and the workbook it was made
   from are kept, and go back into the file as a chart. */
static void test_docx_charts(void) {
    static const char* ser =
        "<c:ser><c:idx val=\"%d\"/><c:order val=\"%d\"/><c:tx><c:strRef><c:strCache><c:ptCount val=\"1\"/><c:pt idx=\"0\">"
        "<c:v>%s</c:v></c:pt></c:strCache></c:strRef></c:tx>%s<c:cat><c:strRef><c:strCache><c:ptCount val=\"3\"/>"
        "<c:pt idx=\"0\"><c:v>North</c:v></c:pt><c:pt idx=\"1\"><c:v>South</c:v></c:pt><c:pt idx=\"2\"><c:v>West</c:v>"
        "</c:pt></c:strCache></c:strRef></c:cat><c:val><c:numRef><c:numCache><c:formatCode>General</c:formatCode>"
        "<c:ptCount val=\"3\"/><c:pt idx=\"0\"><c:v>%s</c:v></c:pt><c:pt idx=\"1\"><c:v>%s</c:v></c:pt>"
        "<c:pt idx=\"2\"><c:v>%s</c:v></c:pt></c:numCache></c:numRef></c:val></c:ser>";
    char s1[2048], s2[2048], col[8192], pie[4096];
    pd_doc* d;
    int pass;

    snprintf(s1, sizeof(s1), ser, 0, 0, "2025", "<c:spPr><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill></c:spPr>"
             "<c:dLbls><c:showVal val=\"1\"/></c:dLbls>", "120", "80", "45.5");
    snprintf(s2, sizeof(s2), ser, 1, 1, "2026", "", "150", "95", "60");
    snprintf(col, sizeof(col),
             "<c:chartSpace xmlns:c=\"c\" xmlns:a=\"a\" xmlns:r=\"r\"><c:chart><c:title><c:tx><c:rich><a:p><a:r><a:t>Sales"
             "</a:t></a:r></a:p></c:rich></c:tx></c:title><c:autoTitleDeleted val=\"0\"/><c:plotArea><c:barChart>"
             "<c:barDir val=\"col\"/><c:grouping val=\"clustered\"/>%s%s<c:gapWidth val=\"100\"/></c:barChart><c:catAx>"
             "<c:delete val=\"0\"/></c:catAx><c:valAx><c:majorGridlines/><c:numFmt formatCode=\"#,##0\" sourceLinked=\"1\"/>"
             "</c:valAx></c:plotArea><c:legend><c:legendPos val=\"b\"/></c:legend></c:chart>"
             "<c:externalData r:id=\"rId1\"><c:autoUpdate val=\"0\"/></c:externalData><c:userShapes r:id=\"rId2\"/>"
             "</c:chartSpace>", s1, s2);
    snprintf(pie, sizeof(pie),
             "<c:chartSpace xmlns:c=\"c\" xmlns:a=\"a\"><c:chart><c:autoTitleDeleted val=\"1\"/><c:plotArea><c:pieChart>"
             "<c:varyColors val=\"1\"/>%s<c:dLbls><c:showPercent val=\"1\"/></c:dLbls><c:firstSliceAng val=\"0\"/>"
             "</c:pieChart></c:plotArea><c:legend><c:legendPos val=\"r\"/></c:legend></c:chart></c:chartSpace>", s2);
    d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId9\" Type=\"t/chart\" Target=\"charts/chart1.xml\"/>"
        "<Relationship Id=\"rId10\" Type=\"t/chart\" Target=\"charts/chart7.xml\"/></Relationships>",
        "word/charts/chart1.xml", col,
        "word/charts/_rels/chart1.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId1\" Type=\"t/package\" "
        "Target=\"../embeddings/Microsoft_Excel_Worksheet.xlsx\"/><Relationship Id=\"rId2\" Type=\"t/chartUserShapes\" "
        "Target=\"../drawings/drawing1.xml\"/></Relationships>",
        "word/embeddings/Microsoft_Excel_Worksheet.xlsx", "PK-the-workbook",
        "word/charts/chart7.xml", pie,
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:c=\"c\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"5486400\" cy=\"3200400\"/><a:graphic><a:graphicData "
        "uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart r:id=\"rId9\"/></a:graphicData>"
        "</a:graphic></wp:inline></w:drawing></w:r></w:p>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"3657600\" cy=\"2743200\"/><a:graphic><a:graphicData "
        "uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\"><c:chart r:id=\"rId10\"/></a:graphicData>"
        "</a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>",
        NULL);

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec;
        pd_inline o;
        char* js;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, sec, 0), 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE);
        CHECK(o.width == PD_PT(432) && o.height == PD_PT(252));
        js = drawing_json(d, o.resource);
        CHECK(js != NULL);

        if (js) {
            CHECK(strstr(js, "\"chart\":") != NULL && strstr(js, "\"data\":") != NULL);    /* the part and its workbook */
            CHECK(count_of(js, "\"shape\":\"rect\"") >= 6 + 2);                          /* six bars, two legend keys */
            CHECK(strstr(js, "\"fill\":4294901760") != NULL);                            /* the first series' own red */
            CHECK(strstr(js, "\"label\":\"Sales\"") && strstr(js, "\"label\":\"North\"") && strstr(js, "\"label\":\"2026\""));
            CHECK(strstr(js, "\"label\":\"45.5\"") != NULL && strstr(js, "\"label\":\"95\"") == NULL);   /* its labels only */
            CHECK(strstr(js, "\"label\":\"160\"") != NULL);                              /* the scale, to 160 by 20 */
            free(js);
        }

        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, sec, 1), 0), &o) == PD_OK);
        js = drawing_json(d, o.resource);
        CHECK(js != NULL);

        if (js) {   /* three wedges of 150, 95 and 60: 49%, 31%, 20%; a legend of the categories */
            CHECK(count_of(js, "\"closed\":1") >= 3 && strstr(js, "\"label\":\"49%\"") && strstr(js, "\"label\":\"20%\""));
            CHECK(strstr(js, "\"label\":\"West\"") != NULL && strstr(js, "\"data\":") == NULL);
            free(js);
        }
    }

    if (d) {    /* written as charts (read back as charts above): the parts, the workbook, its relationship; not the
                   drawing over the chart, which is not kept */
        buf_t z = { NULL, 0 };

        CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &z) == PD_OK);
        CHECK(z.p && has_mem(z.p, z.n, "word/charts/chart1.xml") && has_mem(z.p, z.n, "word/charts/chart2.xml"));
        CHECK(z.p && has_mem(z.p, z.n, "word/embeddings/Microsoft_Excel_Worksheet1.xlsx") &&
              has_mem(z.p, z.n, "PK-the-workbook"));
        CHECK(z.p && has_mem(z.p, z.n, "word/charts/_rels/chart1.xml.rels") && !has_mem(z.p, z.n, "word/drawings/"));
        free(z.p);
        pd_doc_free(d);
    }
}

/* colours and fonts linked to the theme: read as links, changed with the theme (and back on undo), kept through
   Parade's format and .docx, the theme part made again with the new colours and fonts */
static void test_theme_links(void) {
    pd_doc* d = docx_doc(
        "word/theme/theme1.xml",
        "<a:theme xmlns:a=\"a\" name=\"Mine\"><a:themeElements><a:clrScheme name=\"Mine\"><a:dk1><a:sysClr val=\"windowText\" "
        "lastClr=\"000000\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1><a:dk2><a:srgbClr "
        "val=\"222222\"/></a:dk2><a:lt2><a:srgbClr val=\"EEEEEE\"/></a:lt2><a:accent1><a:srgbClr val=\"112233\"/></a:accent1>"
        "<a:accent2><a:srgbClr val=\"808080\"/></a:accent2><a:accent3><a:srgbClr val=\"00FF00\"/></a:accent3><a:accent4>"
        "<a:srgbClr val=\"444444\"/></a:accent4><a:accent5><a:srgbClr val=\"555555\"/></a:accent5><a:accent6><a:srgbClr "
        "val=\"666666\"/></a:accent6><a:hlink><a:srgbClr val=\"0000FF\"/></a:hlink><a:folHlink><a:srgbClr val=\"800080\"/>"
        "</a:folHlink></a:clrScheme><a:fontScheme name=\"Mine\"><a:majorFont><a:latin typeface=\"Georgia\"/><a:ea "
        "typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont><a:minorFont><a:latin typeface=\"Verdana\"/><a:ea "
        "typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme></a:themeElements></a:theme>",
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:styleId=\"Accent\"><w:name w:val=\"Accent\"/><w:rPr>"
        "<w:color w:val=\"112233\" w:themeColor=\"accent1\"/></w:rPr></w:style></w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"accent1\"/></w:rPr><w:t>a</w:t></w:r>"
        "<w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"accent2\" w:themeShade=\"80\"/></w:rPr><w:t>b</w:t></w:r>"
        "<w:r><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" w:asciiTheme=\"majorHAnsi\" w:hAnsiTheme=\"majorHAnsi\"/>"
        "</w:rPr><w:t>c</w:t></w:r><w:r><w:rPr><w:color w:val=\"FF0000\"/></w:rPr><w:t>d</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pStyle w:val=\"Accent\"/><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"FFFFFF\" "
        "w:themeFill=\"accent2\" w:themeFillTint=\"80\"/></w:pPr><w:r><w:t>e</w:t></w:r></w:p>"
        "<w:tbl><w:tblPr><w:tblW w:w=\"2000\" w:type=\"dxa\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"2000\"/></w:tblGrid>"
        "<w:tr><w:tc><w:tcPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"00FF00\" w:themeFill=\"accent3\"/></w:tcPr>"
        "<w:p><w:r><w:t>f</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "<w:p/></w:body></w:document>",
        NULL);
    pd_block_id body, p0, p1, cell;
    pd_char_props cp;
    pd_para_props pp;
    pd_cell_props ce;
    pd_theme th, th2;
    int pass;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    /* the references: a slot and its luminance change, as Word's tints and shades come to */
    CHECK(pd_theme_color(PD_THEME_ACCENT1, 100000, 0) != 0 && pd_theme_color(-1, 100000, 0) == 0);
    {
        int32_t sl, lm, lo;

        CHECK(pd_theme_color_parts(pd_theme_color(PD_THEME_ACCENT2, 75000, 25000), &sl, &lm, &lo) && sl == 5 &&
              lm == 75000 && lo == 25000);
        CHECK(pd_theme_color_parts(0, &sl, &lm, &lo) == 0);
    }

    CHECK(pd_doc_theme(d, &th) == PD_OK && !strcmp(th.name, "Mine") && !strcmp(th.major, "Georgia") &&
          !strcmp(th.minor, "Verdana") && th.color[PD_THEME_ACCENT1] == 0xFF112233u);

    for (pass = 0; pass < 3; pass++) {  /* as read, through Parade's format, through .docx */
        body = pd_doc_child(d, pd_doc_root(d), 0);
        p0 = pd_doc_child(d, body, 0);
        p1 = pd_doc_child(d, body, 1);
        cell = pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, body, 2), 0), 0);
        cp = chars_at(d, p0, 0);
        CHECK(cp.color == 0xFF112233u && cp.color_theme == pd_theme_color(PD_THEME_ACCENT1, 100000, 0));
        cp = chars_at(d, p0, 1);
        CHECK(cp.color == 0xFF404040u && cp.color_theme != 0);    /* 50% grey shaded to half its lightness */
        cp = chars_at(d, p0, 2);
        CHECK(!strcmp(cp.family, "Georgia") && PD_FONT_THEME_TEXT(cp.font_theme) == PD_FONT_THEME_MAJOR);
        cp = chars_at(d, p0, 3);
        CHECK(cp.color == 0xFFFF0000u && cp.color_theme == 0);
        cp = chars_at(d, p1, 0);
        CHECK(cp.color == 0xFF112233u && cp.color_theme != 0);    /* from its style */
        pp = para_resolved(d, p1);
        CHECK(pp.shading == 0xFFBFBFBFu && pp.shading_theme != 0);    /* 50% grey tinted halfway to white */
        CHECK(pd_doc_cell_props(d, cell, &ce) == PD_OK && ce.background == 0xFF00FF00u && ce.background_theme != 0);

        if (pass < 2) {
            buf_t b = { NULL, 0 };
            pd_doc* back = NULL;

            CHECK((pass ? pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) : pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b)) ==
                  PD_OK);
            CHECK((pass ? pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) : pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &back)) ==
                  PD_OK);
            free(b.p);

            if (!back) {
                break;
            }

            pd_doc_free(d);
            d = back;
            CHECK(pd_doc_theme(d, &th2) == PD_OK && !memcmp(&th, &th2, sizeof(th)));
        }
    }

    /* another theme: what is linked follows, what is not stays; undone, back */
    th2 = th;
    th2.color[PD_THEME_ACCENT1] = 0xFFAA0000u;
    th2.color[PD_THEME_ACCENT3] = 0xFF0000AAu;
    snprintf(th2.major, sizeof(th2.major), "%s", "Cambria");
    CHECK(pd_doc_set_theme(d, &th2) == PD_OK);
    body = pd_doc_child(d, pd_doc_root(d), 0);
    p0 = pd_doc_child(d, body, 0);
    p1 = pd_doc_child(d, body, 1);
    cell = pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, body, 2), 0), 0);
    CHECK(chars_at(d, p0, 0).color == 0xFFAA0000u && chars_at(d, p1, 0).color == 0xFFAA0000u);
    CHECK(!strcmp(chars_at(d, p0, 2).family, "Cambria") && chars_at(d, p0, 3).color == 0xFFFF0000u);
    CHECK(pd_doc_cell_props(d, cell, &ce) == PD_OK && ce.background == 0xFF0000AAu);
    CHECK(pd_doc_undo(d) == PD_OK && chars_at(d, p0, 0).color == 0xFF112233u &&
          !strcmp(chars_at(d, p0, 2).family, "Georgia"));
    CHECK(pd_doc_redo(d) == PD_OK && chars_at(d, p0, 0).color == 0xFFAA0000u);

    {   /* written: the theme part has the new colours and fonts, and Word's attributes keep the links */
        buf_t b = { NULL, 0 };
        pd_doc* back = NULL;
        pd_theme th3;

        CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);

        if (back) {
            CHECK(pd_doc_theme(back, &th3) == PD_OK && th3.color[PD_THEME_ACCENT1] == 0xFFAA0000u &&
                  !strcmp(th3.major, "Cambria") && !strcmp(th3.minor, "Verdana"));
            body = pd_doc_child(back, pd_doc_root(back), 0);
            CHECK(chars_at(back, pd_doc_child(back, body, 0), 0).color_theme != 0);
            pd_doc_free(back);
        }
    }

    pd_doc_free(d);

    {   /* a document of Parade's own given a theme: written with a theme part made for it */
        pd_doc* n = NULL;
        buf_t b = { NULL, 0 };
        pd_doc* back = NULL;
        pd_theme t;

        CHECK(pd_doc_new(&n) == PD_OK);
        pd_theme_init(&t);
        t.color[PD_THEME_ACCENT1] = 0xFF123456u;
        snprintf(t.minor, sizeof(t.minor), "%s", "Gill Sans");
        CHECK(pd_doc_set_theme(n, &t) == PD_OK);
        CHECK(pd_doc_export(n, PD_CONV_DOCX, to_buf, &b) == PD_OK && pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);

        if (back) {
            CHECK(pd_doc_theme(back, &t) == PD_OK && t.color[PD_THEME_ACCENT1] == 0xFF123456u &&
                  !strcmp(t.minor, "Gill Sans"));
            pd_doc_free(back);
        }

        pd_doc_free(n);
    }
}

/* East Asian and complex-script fonts (named, and the theme's), complex scripts' size, bold and italic; a ruby
   over its base; the document grid and a paragraph off it. Read, and kept through DOCX. */
static void test_docx_east_asian(void) {
    pd_doc* d = docx_doc(
        "word/theme/theme1.xml",
        "<a:theme xmlns:a=\"a\"><a:themeElements><a:fontScheme name=\"F\"><a:majorFont><a:latin typeface=\"Cambria\"/>"
        "<a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont><a:minorFont><a:latin typeface=\"Calibri\"/>"
        "<a:ea typeface=\"MS Mincho\"/><a:cs typeface=\"Arial\"/></a:minorFont></a:fontScheme></a:themeElements></a:theme>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" w:eastAsia=\"SimSun\" "
        "w:cs=\"Traditional Arabic\"/><w:b/><w:bCs w:val=\"0\"/><w:iCs/><w:sz w:val=\"24\"/><w:szCs w:val=\"32\"/></w:rPr>"
        "<w:t>abc</w:t></w:r><w:r><w:rPr><w:rFonts w:eastAsiaTheme=\"minorEastAsia\" w:cstheme=\"minorBidi\"/></w:rPr>"
        "<w:t>def</w:t></w:r>"
        "<w:r><w:ruby><w:rubyPr><w:rubyAlign w:val=\"distributeSpace\"/><w:hps w:val=\"10\"/><w:hpsRaise w:val=\"18\"/>"
        "<w:hpsBaseText w:val=\"21\"/><w:lid w:val=\"ja-JP\"/></w:rubyPr><w:rt><w:r><w:rPr><w:sz w:val=\"10\"/></w:rPr>"
        "<w:t>\xe3\x81\x8b\xe3\x82\x93</w:t></w:r></w:rt><w:rubyBase><w:r><w:t>\xe6\xbc\xa2</w:t></w:r></w:rubyBase></w:ruby></w:r>"
        "<w:r><w:t>!</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:snapToGrid w:val=\"0\"/></w:pPr><w:r><w:t>off</w:t></w:r></w:p>"
        "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:docGrid w:type=\"lines\" w:linePitch=\"312\"/></w:sectPr>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, p;
        pd_char_props cp;
        pd_para_props pp;
        pd_section_props sp;
        pd_inline o;
        uint32_t k;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        p = pd_doc_child(d, sec, 0);
        cp = chars_at(d, p, 0);
        CHECK(!strcmp(cp.family, "Times New Roman") && !strcmp(cp.family_ea, "SimSun") &&
              !strcmp(cp.family_cs, "Traditional Arabic"));
        CHECK(cp.weight == 700 && cp.weight_cs == 400 && cp.italic == 0 && cp.italic_cs == 1);
        CHECK(cp.size == PD_PT(12) && cp.size_cs == PD_PT(16));
        cp = chars_at(d, p, 3);
        CHECK(!strcmp(cp.family_ea, "MS Mincho") && !strcmp(cp.family_cs, "Arial"));      /* the theme's */
        k = nth_object(d, p, 0);
        CHECK(k == 6 && pd_doc_inline_at(d, at(p, k), &o) == PD_OK && o.kind == PD_INLINE_RUBY);
        CHECK(o.source_len == 6 && !memcmp(o.source, "\xe3\x81\x8b\xe3\x82\x93", 6));
        CHECK(o.height == PD_PT(5) && o.depth == PD_PT(9));
        CHECK(pd_doc_inline_at(d, at(p, k + 6), &o) == PD_OK && o.kind == PD_INLINE_RUBY && o.source_len == 0);
        CHECK(pd_doc_section_props(d, sec, &sp) == PD_OK && sp.line_pitch == PD_PT(15.6));
        pp = para_resolved(d, pd_doc_child(d, sec, 1));
        CHECK(pp.snap_grid == 0 && para_resolved(d, p).snap_grid == 1);
    }

    if (d) {    /* the guide in a text export: the base only; in HTML, its own markup */
        buf_t t = { NULL, 0 };

        CHECK(pd_doc_export(d, PD_CONV_TEXT, to_buf, &t) == PD_OK && t.p && strstr(t.p, "abcdef\xe6\xbc\xa2!"));
        free(t.p);
        t.p = NULL;
        t.n = 0;
        CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &t) == PD_OK && t.p &&
              strstr(t.p, "<ruby>\xe6\xbc\xa2<rt>\xe3\x81\x8b\xe3\x82\x93</rt></ruby>"));
        free(t.p);
    }

    pd_doc_free(d);
}

/* Cell shading by pattern -- solid is the foreground (auto: black), pctN that much of it over the fill -- and each
   edge of a cell its own width (a timeline whose year columns are ruled heavier than its quarters). Kept. */
static void test_docx_shading_edges(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:tbl><w:tblGrid><w:gridCol w:w=\"2000\"/><w:gridCol w:w=\"2000\"/>"
        "<w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr>"
        "<w:tc><w:tcPr><w:tcBorders><w:left w:val=\"single\" w:sz=\"12\"/><w:bottom w:val=\"single\" w:sz=\"4\"/></w:tcBorders>"
        "<w:shd w:val=\"solid\" w:color=\"auto\" w:fill=\"auto\"/></w:tcPr><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc>"
        "<w:tc><w:tcPr><w:shd w:val=\"pct20\" w:color=\"auto\" w:fill=\"auto\"/></w:tcPr><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc>"
        "<w:tc><w:tcPr><w:shd w:val=\"pct50\" w:color=\"FF0000\" w:fill=\"0000FF\"/></w:tcPr><w:p><w:r><w:t>c</w:t></w:r></w:p>"
        "</w:tc></w:tr></w:tbl><w:p><w:pPr><w:shd w:val=\"solid\" w:color=\"00FF00\" w:fill=\"auto\"/></w:pPr><w:r><w:t>p</w:t>"
        "</w:r></w:p></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, t, row;
        pd_cell_props c0, c1, c2;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        t = pd_doc_child(d, sec, 0);
        row = pd_doc_child(d, t, 0);
        CHECK(pd_doc_cell_props(d, pd_doc_child(d, row, 0), &c0) == PD_OK && c0.background == 0xFF000000u);
        CHECK(pd_doc_cell_props(d, pd_doc_child(d, row, 1), &c1) == PD_OK && c1.background == 0xFFCCCCCCu);
        CHECK(pd_doc_cell_props(d, pd_doc_child(d, row, 2), &c2) == PD_OK && (c2.background & 0xFFFFFF) == 0x800080);
        /* the left edge 1.5pt, the bottom 0.5pt: not both the widest */
        CHECK((c0.border_on & PD_BORDER_LEFT) && (c0.border_on & PD_BORDER_BOTTOM));
        CHECK((c0.edge_width[3] ? c0.edge_width[3] : c0.border_width) == PD_PT(1.5));
        CHECK((c0.edge_width[2] ? c0.edge_width[2] : c0.border_width) == PD_PT(0.5));
        CHECK(para_resolved(d, pd_doc_child(d, sec, 1)).shading == 0xFF00FF00u);
    }

    pd_doc_free(d);
}

/* Shapes in a group: a translucent fill keeps its alpha; a box turned 45 degrees, or seen through an isometric
   camera, is drawn as the outline it comes to, not as an upright box. */
static void test_docx_group_turned_shapes(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpg=\"wpg\" xmlns:wps=\"wps\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"2540000\"/><a:graphic><a:graphicData><wpg:wgp>"
        "<wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"2540000\"/><a:chOff x=\"0\" y=\"0\"/>"
        "<a:chExt cx=\"2540000\" cy=\"2540000\"/></a:xfrm></wpg:grpSpPr>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"0000FF\"><a:alpha val=\"50000\"/></a:srgbClr>"
        "</a:solidFill></wps:spPr><wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm rot=\"2700000\"><a:off x=\"1270000\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/>"
        "</a:xfrm><a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill></wps:spPr>"
        "<wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"1270000\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill><a:scene3d><a:camera "
        "prst=\"isometricOffAxis1Top\"/></a:scene3d></wps:spPr><wps:bodyPr/></wps:wsp>"
        "</wpg:wgp></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>",
        NULL);
    pd_inline o;
    char* js;
    unsigned long alpha = 0;
    const char* f;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
    js = drawing_json(d, o.resource);
    CHECK(js != NULL);

    if (js) {
        f = strstr(js, "\"shape\":\"rect\"");      /* the upright one: a box, half see-through */
        CHECK(f != NULL && (f = strstr(f, "\"fill\":")) != NULL);
        alpha = f ? (strtoul(f + 7, NULL, 10) >> 24) : 0;
        CHECK(alpha >= 0x7E && alpha <= 0x81);
        f = strstr(js, "\"path\"");
        CHECK(f != NULL && strstr(f + 1, "\"path\"") != NULL);  /* the turned box and the camera's: outlines */
        CHECK(strstr(js, "\"shape\":\"rect\"") == strstr(js, "\"shape\""));
        CHECK(strstr(strstr(js, "\"shape\"") + 1, "\"shape\"") == NULL);   /* only the upright one a box */
        free(js);
    }

    pd_doc_free(d);
}

/* A drawing made again from its kept XML: unchanged, the same drawing (its text box the same story); a shape's
   offset changed in the XML, that shape moved. */
static void test_docx_drawing_rebuild(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpc=\"wpc\" xmlns:wps=\"wps\"><w:body>"
                    "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"1270000\"/><a:graphic><a:graphicData><wpc:wpc>"
                    "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
                    "<a:prstGeom prst=\"rect\"/><a:solidFill><a:schemeClr val=\"accent1\"/></a:solidFill></wps:spPr><wps:bodyPr/>"
                    "</wps:wsp><wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"700000\"/><a:ext cx=\"2540000\" cy=\"500000\"/>"
                    "</a:xfrm><a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx><w:txbxContent><w:p><w:r><w:t>Caption</w:t></w:r>"
                    "</w:p></w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp></wpc:wpc></a:graphicData></a:graphic></wp:inline>"
                    "</w:drawing></w:r></w:p></w:body></w:document>",
                    NULL);
    pd_inline o;
    char* js, *js2, *xml, *items0, *items1;
    pd_res_id r2;
    int32_t stories;
    const char* mime;
    const void* data;
    size_t len;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
    js = drawing_json(d, o.resource);
    CHECK(js != NULL && strstr(js, "\"xml\":\"<wpc:wpc>") != NULL && strstr(js, "\"theme\":[") != NULL);

    if (js) {
        const char* x0 = strstr(js, "\"xml\":\"") + 7, *x1 = strstr(x0, "</wpc:wpc>") + 10;
        size_t xn = (size_t)(x1 - x0);
        char* e;

        /* the XML as it is in the drawing: unescaped (its quotes) */
        xml = (char*)malloc(xn + 1);

        for (e = xml; x0 < x1; x0++) {
            if (*x0 == '\\' && x0 + 1 < x1) {
                x0++;
            }

            *e++ = *x0;
        }

        *e = '\0';
        stories = pd_doc_story_count(d);
        items0 = strstr(js, "\"items\":[");
        CHECK(pd_docx_drawing_rebuild(d, o.resource, xml, strlen(xml), &r2) == PD_OK && r2 != 0);
        js2 = r2 ? drawing_json(d, r2) : NULL;
        items1 = js2 ? strstr(js2, "\"items\":[") : NULL;
        CHECK(items0 && items1 && !strncmp(items0, items1, (size_t)(strstr(items0, "],\"kind\"") - items0)));
        CHECK(pd_doc_story_count(d) == stories);    /* the text box's story the same, none made */
        free(js2);

        /* the first shape moved half an inch right: its box with it */
        e = strstr(xml, "<a:off x=\"0\" y=\"0\"/>");
        CHECK(e != NULL);

        if (e) {
            char* moved = (char*)malloc(strlen(xml) + 16);

            memcpy(moved, xml, (size_t)(e - xml));
            strcpy(moved + (e - xml), "<a:off x=\"457200\" y=\"0\"/>");
            strcat(moved, e + strlen("<a:off x=\"0\" y=\"0\"/>"));
            CHECK(pd_docx_drawing_rebuild(d, o.resource, moved, strlen(moved), &r2) == PD_OK);
            js2 = r2 ? drawing_json(d, r2) : NULL;
            CHECK(js2 && pd_doc_resource(d, r2, &mime, &data, &len) == PD_OK);

            if (js2) {
                char want[64];

                snprintf(want, sizeof(want), "{\"sid\":0,\"box\":[%d,0,", (int)PD_PT(36));
                CHECK(strstr(js2, want) != NULL);
            }

            free(js2);
            free(moved);
        }

        /* the editor's marks: the text box marked as story 0 keeps it, one new (unmarked) gets a story of its own,
           read from its XML; the marks are not kept */
        {
            const char* tb = strstr(xml, "<w:txbxContent>"), *end = strstr(xml, "</wpc:wpc>");
            const char* nb = "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"635000\" cy=\"300000\"/>"
                             "</a:xfrm><a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx><w:txbxContent><w:p><w:r><w:t>Fresh"
                             "</w:t></w:r></w:p></w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp>";
            char* mk = (char*)malloc(strlen(xml) + strlen(nb) + 64);
            size_t k;
            int found = 0;

            CHECK(tb != NULL && end != NULL);

            if (tb && end && mk) {
                size_t at = (size_t)(tb - xml) + strlen("<w:txbxContent>");

                memcpy(mk, xml, at);
                strcpy(mk + at, "<!--pd-story:0-->");
                strncat(mk, xml + at, (size_t)(end - xml) - at);
                strcat(mk, nb);
                strcat(mk, end);
                stories = pd_doc_story_count(d);
                CHECK(pd_docx_drawing_rebuild(d, o.resource, mk, strlen(mk), &r2) == PD_OK);
                CHECK(pd_doc_story_count(d) == stories + 1);
                js2 = r2 ? drawing_json(d, r2) : NULL;
                CHECK(js2 && strstr(js2, "pd-story") == NULL);

                for (k = 0; k < (size_t)pd_doc_story_count(d); k++) {
                    const char* t;
                    uint32_t tn = 0;

                    if (pd_doc_para_text(d, pd_doc_child(d, pd_doc_story_at(d, (int32_t)k), 0), &t, &tn) == PD_OK) {
                        found += tn == 5 && !memcmp(t, "Fresh", 5);
                    }
                }

                CHECK(found == 1);
                free(js2);
            }

            free(mk);
        }

        free(xml);
        free(js);
    }

    pd_doc_free(d);
}

/* A canvas without VML beside it (one made in the editor, or edited: its old VML is not kept) written with a
   fallback made from what it draws, for the readers that show a canvas only as VML: its box, its picture, its
   text box with the story's text. */
/* Office's presets worked out: an ellipse's arcs (3cd4 a name, not the number 3), a handle dragged */
static void test_preset_eval(void) {
    char buf[8192];
    size_t n;

    CHECK(pd_preset_count() >= 180 && pd_preset_name(0) != NULL && pd_preset_name(pd_preset_count()) == NULL);
    n = pd_preset_json("ellipse", 1000, 500, "", buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf));
    /* four quarter arcs from the left middle: through the top, the right, the bottom, and back */
    CHECK(strstr(buf, "[\"m\",0,250]") && strstr(buf, ",1000,250]") &&
          strstr(buf, ",500,500]"));
    n = pd_preset_json("roundRect", 1000, 500, "adj=20000", buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "[\"adj\",20000,16667]") && strstr(buf, "\"g1\":\"adj\",\"g2\":\"\",\"x\":100,"));
    /* its handle dragged to x = 150 of 1000 by 500: the radius 150, adj 30000 of the shorter side */
    n = pd_preset_drag("roundRect", 1000, 500, "adj=20000", 0, 150, 0, buf, sizeof(buf));
    CHECK(n > 0 && !strncmp(buf, "adj=", 4) && fabs(atof(buf + 4) - 30000) < 50);
    CHECK(pd_preset_json("noSuchShape", 10, 10, "", buf, sizeof(buf)) == 0);
}

/* a preset's adjustments move its corners: a triangle's apex where adj puts it, at the middle without one */
static void test_docx_preset_adjust(void) {
    static const char* avs[2] = { "<a:avLst/>", "<a:avLst><a:gd name=\"adj\" fmla=\"val 0\"/></a:avLst>" };
    int k;

    for (k = 0; k < 2; k++) {
        char doc[2048];
        pd_doc* d;
        pd_inline o;
        char* js;
        const char* p;
        int a[4] = { -1, -1, -1, -1 };

        snprintf(doc, sizeof(doc), "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpc=\"wpc\" "
                 "xmlns:wps=\"wps\"><w:body><w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"1270000\"/>"
                 "<a:graphic><a:graphicData><wpc:wpc><wpc:bg/><wpc:whole/><wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" "
                 "y=\"0\"/><a:ext cx=\"2540000\" cy=\"1270000\"/></a:xfrm><a:prstGeom prst=\"triangle\">%s</a:prstGeom>"
                 "<a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill></wps:spPr><wps:bodyPr/></wps:wsp></wpc:wpc>"
                 "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>", avs[k]);
        d = docx_doc("word/document.xml", doc, NULL);
        CHECK(d != NULL);

        if (!d) {
            continue;
        }

        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
        js = drawing_json(d, o.resource);
        p = js ? strstr(js, "\"path\":[") : NULL;
        CHECK(p && sscanf(p + 8, "%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3]) == 4);
        /* from the bottom left corner to the apex, at the top: in the middle (200 pt across), or over the corner */
        CHECK(a[0] == 0 && a[3] == 0 && a[1] > 0);
        CHECK(k == 0 ? abs(a[2] - 100 * 65536) <= 2 : a[2] == 0);
        free(js);
        pd_doc_free(d);
    }
}

static void test_docx_canvas_fallback(void) {
    static const char canvas[] = "<wpc:wpc><wpc:bg/><wpc:whole/>"
                                 "<pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
                                 "<a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm></pic:spPr></pic:pic>"
                                 "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
                                 "<a:prstGeom prst=\"ellipse\"/><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill></wps:spPr>"
                                 "<wps:bodyPr/></wps:wsp>"
                                 "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"700000\"/><a:ext cx=\"2540000\" cy=\"500000\"/></a:xfrm>"
                                 "<a:prstGeom prst=\"triangle\"/><a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill></wps:spPr>"
                                 "<wps:txbx><w:txbxContent><w:p><w:r><w:t>Caption</w:t></w:r></w:p></w:txbxContent></wps:txbx>"
                                 "<wps:bodyPr/></wps:wsp></wpc:wpc>";
    char doc[4096];
    pd_doc* d;
    int pass;

    snprintf(doc, sizeof(doc), "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\" "
             "xmlns:wpc=\"wpc\" xmlns:wps=\"wps\"><w:body><w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" "
             "cy=\"1270000\"/><a:graphic><a:graphicData>%s</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
             "</w:body></w:document>", canvas);
    d = docx_doc("word/_rels/document.xml.rels",
                 "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
                 "</Relationships>",
                 "word/media/image1.png", "tests/data/rgba.png", "word/document.xml", doc, NULL);
    CHECK(d != NULL);

    for (pass = 0; pass < 2 && d; pass++) {
        pd_inline o;
        char* js;

        d = docx_again(d);
        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
        js = drawing_json(d, o.resource);
        CHECK(js && strstr(js, "\"fallback\":\"<mc:Fallback><w:pict><v:group editas=\\\"canvas\\\"") != NULL);

        if (js) {   /* read back as VML: kept as it came the second time round */
            const char* fb = strstr(js, "\"fallback\":");

            CHECK(fb && strstr(fb, "coordsize=\\\"20000,10000\\\"") && strstr(fb, "style=\\\"width:200.00pt;height:100.00pt;"));
            CHECK(fb && strstr(fb, "<v:imagedata r:id=\\\"rIdm1\\\"") != NULL);
            CHECK(fb && strstr(fb, "<v:oval style=\\\"position:absolute;left:10000;top:0;width:5000;height:5000\\\" "
                               "fillcolor=\\\"#FF0000\\\"") != NULL);
            CHECK(fb && strstr(fb, "fillcolor=\\\"#00FF00\\\"") && strstr(fb, "path=\\\"m"));     /* the triangle */
            CHECK(fb && strstr(fb, "<v:textbox") && strstr(fb, "Caption</w:t>"));
            free(js);
        }
    }

    if (d) {    /* made again from its XML (edited): the VML it had not kept with it */
        pd_inline o;
        pd_res_id r2 = 0;
        char* js, edited[2048];

        /* without the picture (named rId5 here, not as the package written has it) */
        snprintf(edited, sizeof(edited), "<wpc:wpc><wpc:bg/><wpc:whole/>%s", strstr(canvas, "</pic:pic>") + 10);
        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
        CHECK(pd_docx_drawing_rebuild(d, o.resource, edited, strlen(edited), &r2) == PD_OK && r2 != 0);
        js = r2 ? drawing_json(d, r2) : NULL;
        CHECK(js && strstr(js, "\"xml\":") && !strstr(js, "\"fallback\""));
        free(js);
        pd_doc_free(d);
    }
}

/* Through DOCX unchanged: a picture and a SEQ field inside a tracked deletion stay deleted (a figure deleted
   came back on every save, and moved the pages after it), the field in its text's size; a link Word shows without
   an underline stays without; a one-column section keeps its column gap. */
static void test_docx_tracked_objects(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "<Relationship Id=\"rId9\" Type=\"t/hyperlink\" Target=\"https://example.org\" TargetMode=\"External\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Kept </w:t></w:r><w:del w:id=\"1\" w:author=\"A\"><w:r><w:drawing>"
        "<wp:inline><wp:extent cx=\"127000\" cy=\"127000\"/><a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip "
        "r:embed=\"rId5\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>"
        "</w:del></w:p>"
        "<w:p><w:del w:id=\"2\" w:author=\"A\"><w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr><w:delText xml:space=\"preserve\">Fig. "
        "</w:delText></w:r><w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r>"
        "<w:rPr><w:sz w:val=\"20\"/></w:rPr><w:delInstrText> SEQ fig \\* ARABIC </w:delInstrText></w:r><w:r><w:rPr>"
        "<w:sz w:val=\"20\"/></w:rPr><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:rPr><w:sz w:val=\"20\"/>"
        "</w:rPr><w:delText>1</w:delText></w:r><w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr><w:fldChar "
        "w:fldCharType=\"end\"/></w:r><w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr><w:delText>. Gone.</w:delText></w:r>"
        "</w:del></w:p>"
        "<w:p><w:hyperlink r:id=\"rId9\"><w:r><w:rPr><w:color w:val=\"0563C1\"/></w:rPr><w:t>plain link</w:t></w:r>"
        "</w:hyperlink></w:p>"
        "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:cols w:space=\"720\"/></w:sectPr>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, p0, p1, p2;
        pd_char_props c;
        pd_section_props sp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        p0 = pd_doc_child(d, sec, 0);
        p1 = pd_doc_child(d, sec, 1);
        p2 = pd_doc_child(d, sec, 2);
        c = chars_at(d, p0, 5);     /* the picture, after "Kept " */
        CHECK((c.mask & PD_CP_REVISION) && c.revision != 0);
        c = chars_at(d, p1, 5);     /* the field, after "Fig. " */
        CHECK((c.mask & PD_CP_REVISION) && c.revision != 0 && c.size == PD_PT(10));
        CHECK(chars_at(d, p2, 0).underline == PD_UNDERLINE_NONE);
        CHECK(pd_doc_section_props(d, sec, &sp) == PD_OK && sp.column_gap == PD_PT(36));
    }

    pd_doc_free(d);
}

/* A drawing's caption is a story: a copy of the drawing, pasted, has a caption of its own with the same text --
   editing it leaves the original's alone -- and one pasted into another document has its text too. */
static void test_drawing_story_copy(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpc=\"wpc\" xmlns:wps=\"wps\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"1270000\" cy=\"635000\"/><a:graphic><a:graphicData><wpc:wpc>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx><w:txbxContent><w:p><w:r><w:t>Caption one</w:t></w:r></w:p>"
        "</w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp></wpc:wpc></a:graphicData></a:graphic></wp:inline>"
        "</w:drawing></w:r></w:p><w:p><w:r><w:t>after</w:t></w:r></w:p></w:body></w:document>",
        NULL);
    buf_t b = { NULL, 0 };
    pd_doc* e = NULL;
    pd_block_id sec, p0, p1;
    pd_range r;
    pd_pos after;
    int32_t before;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    sec = pd_doc_child(d, pd_doc_root(d), 0);
    p0 = pd_doc_child(d, sec, 0);
    p1 = pd_doc_child(d, sec, 1);
    before = pd_doc_story_count(d);
    CHECK(before == 1);
    r.start = at(p0, 0);
    r.end = at(p0, 3);
    CHECK(pd_doc_export_range(d, r, PD_CONV_JDATA, to_buf, &b) == PD_OK);
    CHECK(pd_doc_paste(d, at(p1, 0), b.p, b.n, PD_CONV_JDATA, &after) == PD_OK);
    CHECK(pd_doc_story_count(d) == before + 1);     /* its own caption */

    if (pd_doc_story_count(d) == before + 1) {
        pd_block_id s0 = pd_doc_child(d, pd_doc_story_at(d, 0), 0), s1 = pd_doc_child(d, pd_doc_story_at(d, 1), 0);
        const char* t;
        uint32_t n;

        CHECK(pd_doc_para_text(d, s1, &t, &n) == PD_OK && n == 11 && !memcmp(t, "Caption one", 11));
        pd_doc_insert_text(d, at(s1, 0), "Copy ", 5, PD_FORMAT_INHERIT, NULL);
        CHECK(pd_doc_para_text(d, s0, &t, &n) == PD_OK && n == 11);     /* the original's unchanged */
    }

    CHECK(pd_doc_import(b.p, b.n, PD_CONV_JDATA, &e) == PD_OK && e && pd_doc_story_count(e) == 1);
    if (e) {
        const char* t;
        uint32_t n;

        CHECK(pd_doc_para_text(e, pd_doc_child(e, pd_doc_story_at(e, 0), 0), &t, &n) == PD_OK && n == 11);
        pd_doc_free(e);
    }

    free(b.p);
    pd_doc_free(d);
}

/* A canvas kept as Word wrote it: through DOCX its own XML comes back -- the 3-D, the preset shapes -- with its
   picture's reference renamed to the new package's, and the styles only its text uses are the document's; a canvas
   whose text the writer would renumber (a list) is made from the drawing instead. */
static void test_docx_canvas_kept(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId7\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
        "</w:style><w:style w:type=\"paragraph\" w:styleId=\"MCBodySP\"><w:name w:val=\"MC Body SP\"/><w:rPr>"
        "<w:sz w:val=\"14\"/></w:rPr></w:style><w:style w:type=\"character\" w:styleId=\"FigCap\"><w:name "
        "w:val=\"FigCap\"/><w:rPr><w:b/></w:rPr></w:style></w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\" xmlns:wpc=\"wpc\" "
        "xmlns:wps=\"wps\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"1270000\"/><a:graphic><a:graphicData><wpc:wpc>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"roundRect\"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr val=\"808080\"/></a:solidFill>"
        "<a:scene3d><a:camera prst=\"isometricOffAxis1Top\"/></a:scene3d><a:sp3d extrusionH=\"133350\"/></wps:spPr>"
        "<wps:bodyPr/></wps:wsp>"
        "<pic:pic><pic:blipFill><a:blip r:embed=\"rId7\"/></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"1270000\" y=\"0\"/>"
        "<a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm></pic:spPr></pic:pic>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"700000\"/><a:ext cx=\"2540000\" cy=\"500000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:noFill/></wps:spPr><wps:txbx><w:txbxContent><w:p><w:pPr><w:pStyle "
        "w:val=\"MCBodySP\"/></w:pPr><w:r><w:rPr><w:rStyle w:val=\"FigCap\"/></w:rPr><w:t>Fig. 2.</w:t></w:r></w:p>"
        "</w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp>"
        "</wpc:wpc></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"1270000\" cy=\"635000\"/><a:graphic><a:graphicData><wpc:wpc>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx><w:txbxContent><w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/>"
        "<w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>listed</w:t></w:r></w:p></w:txbxContent></wps:txbx>"
        "<wps:bodyPr/></wps:wsp></wpc:wpc></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec;
        pd_inline o;
        char* js;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, sec, 0), 0), &o) == PD_OK);
        js = drawing_json(d, o.resource);
        CHECK(js != NULL);

        if (js) {
            const char* r = strstr(js, "\"rels\":{\"");
            const char* mime = NULL;
            const void* data = NULL;
            size_t len = 0;

            CHECK(strstr(js, "\"kind\":\"wpc\"") != NULL && strstr(js, "\"xml\":\"<wpc:wpc>") != NULL);
            CHECK(strstr(js, "extrusionH=\\\"133350\\\"") != NULL && strstr(js, "prst=\\\"roundRect\\\"") != NULL);
            CHECK(strstr(js, "{\"img\":") != NULL);     /* the picture still drawn */
            CHECK(r != NULL);   /* and its reference a picture of the document */
            if (r) {
                const char* c = strchr(r + 9, ':');
                pd_res_id res = c ? (pd_res_id)atoi(c + 1) : 0;

                CHECK(res && pd_doc_resource(d, res, &mime, &data, &len) == PD_OK && !strcmp(mime, "image/png"));
            }
            free(js);
        }

        CHECK(pd_doc_style_find(d, "MC Body SP") != 0);     /* used in the drawing alone */
        CHECK(story_para_with(d, "Fig. 2.") != 0 && chars_at(d, story_para_with(d, "Fig. 2."), 0).weight == 700);

        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, sec, 1), 0), &o) == PD_OK);
        js = drawing_json(d, o.resource);
        /* made, not kept, as read (once written, the drawing's XML is the writer's own: kept from then on) */
        CHECK(js != NULL && (pass > 0 || strstr(js, "\"xml\"") == NULL) && story_para_with(d, "listed") != 0);
        free(js);
    }

    pd_doc_free(d);
}

/* A canvas as Word draws Fig. 3-style diagrams: its background and frame; a shape filled from its group (grpFill);
   a freeform whose points are guides; a box extruded and seen through an isometric camera (a prism: several faces);
   a curved connector (a curve, not a chord) with a medium arrowhead (three line widths long); a 60% pattern
   (its colours mixed); and a text box's paragraph justified as the default paragraph style is. */
static void test_docx_canvas_3d(void) {
    pd_doc* d = docx_doc(
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
        "<w:pPr><w:jc w:val=\"both\"/></w:pPr></w:style></w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpc=\"wpc\" xmlns:wpg=\"wpg\" xmlns:wps=\"wps\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"2540000\"/><a:graphic><a:graphicData><wpc:wpc>"
        "<wpc:bg><a:solidFill><a:prstClr val=\"white\"/></a:solidFill></wpc:bg>"
        "<wpc:whole><a:ln><a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill></a:ln></wpc:whole>"
        "<wpg:wgp><wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/><a:chOff x=\"0\" y=\"0\"/>"
        "<a:chExt cx=\"635000\" cy=\"635000\"/></a:xfrm><a:solidFill><a:srgbClr val=\"808000\"/></a:solidFill></wpg:grpSpPr>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:grpFill/></wps:spPr><wps:bodyPr/></wps:wsp></wpg:wgp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm><a:custGeom>"
        "<a:gdLst><a:gd name=\"gx\" fmla=\"*/ 1 w 2\"/><a:gd name=\"gy\" fmla=\"+- h 0 0\"/></a:gdLst><a:pathLst>"
        "<a:path w=\"635000\" h=\"635000\"><a:moveTo><a:pt x=\"0\" y=\"0\"/></a:moveTo><a:lnTo><a:pt x=\"gx\" y=\"gy\"/>"
        "</a:lnTo><a:lnTo><a:pt x=\"w\" y=\"0\"/></a:lnTo><a:close/></a:path></a:pathLst></a:custGeom>"
        "<a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill></wps:spPr><wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"1270000\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"0000FF\"/></a:solidFill><a:scene3d><a:camera "
        "prst=\"isometricOffAxis1Top\"/></a:scene3d><a:sp3d extrusionH=\"254000\"/></wps:spPr><wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"1270000\"/><a:ext cx=\"635000\" cy=\"317500\"/></a:xfrm>"
        "<a:prstGeom prst=\"curvedConnector3\"/><a:ln w=\"25400\"><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill>"
        "<a:tailEnd type=\"triangle\"/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1905000\" y=\"1905000\"/><a:ext cx=\"317500\" cy=\"317500\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:pattFill prst=\"pct60\"><a:fgClr><a:srgbClr val=\"000000\"/></a:fgClr><a:bgClr>"
        "<a:srgbClr val=\"FFFFFF\"/></a:bgClr></a:pattFill></wps:spPr><wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"2000000\"/><a:ext cx=\"1270000\" cy=\"500000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:noFill/></wps:spPr><wps:txbx><w:txbxContent><w:p><w:r><w:t>Fig. 1. A caption.</w:t>"
        "</w:r></w:p></w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp>"
        "</wpc:wpc></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>",
        NULL);
    pd_inline o;
    char* js;
    char want[96];
    const char* p;
    int faces = 0, bluish = 0;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
    js = drawing_json(d, o.resource);
    CHECK(js != NULL);

    if (js) {
        /* the background first, white, and the frame */
        CHECK(strstr(js, "\"shape\":\"rect\",\"x\":0,\"y\":0") != NULL && strstr(js, "\"fill\":4294967295") != NULL);
        CHECK(strstr(js, "\"fill\":0,\"line\":4278190080") != NULL);
        CHECK(strstr(js, "\"fill\":4286611456") != NULL);         /* the group's olive, 0xFF808000 */
        /* the freeform's middle point from its guides: half the width, the full height (50 pt, 50 pt into it) */
        snprintf(want, sizeof(want), "%d,%d", (int)(PD_PT(100) + PD_PT(25)), (int)PD_PT(50));
        CHECK(strstr(js, want) != NULL);
        /* the extruded box: a front face and more, all blue or shaded from it */
        for (p = js; (p = strstr(p, "\"fill\":")) != NULL; p++) {
            unsigned long c = strtoul(p + 7, NULL, 10);

            if ((c & 0xFFFF00) == 0 && (c & 0xFF) > 0x40) {
                bluish++;
            }
        }

        CHECK(bluish >= 4);
        faces = bluish;
        /* the connector a curve: a path of many points ending at the box's far corner */
        CHECK(strstr(js, "\"line\":4294901760") != NULL);
        p = strstr(js, "\"line\":4294901760");
        while (p && p > js && strncmp(p, "{\"path\"", 7)) {
            p--;
        }
        if (p) {
            int commas = 0;
            const char* e = strchr(p, ']');

            for (; e && p < e; p++) {
                commas += *p == ',';
            }

            CHECK(commas > 20);
        }
        CHECK(strstr(js, "\"fill\":4284900966") != NULL);     /* 60% black on white: 0xFF666666 */
        CHECK(story_para_with(d, "Fig. 1.") != 0 &&
              para_resolved(d, story_para_with(d, "Fig. 1.")).align == PD_ALIGN_JUSTIFY);     /* as Normal is */
        free(js);
    }

    (void)faces;
    pd_doc_free(d);
}

static pd_font* layout_font;

static const pd_font* layout_resolver(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;
    (void)family;
    (void)weight;
    (void)italic;
    return layout_font;
}

/* a font to lay documents out in, as test_layout's; 0 if there is none (those checks skipped) */
static int have_layout_font(void) {
    const char* path = getenv("PARADE_TEST_FONT") ? getenv("PARADE_TEST_FONT") :
                       "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf";

    return layout_font || pd_font_load_file(path, 0, &layout_font) == PD_OK;
}

/* draw items of a kind over all pages of a fresh layout (colour 0: any) */
static int count_draws(pd_doc* d, int32_t kind, uint32_t color) {
    pd_layout* L = NULL;
    int32_t pg, k, n, c = 0;

    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) != PD_OK || pd_layout_update(L, NULL) != PD_OK) {
        pd_layout_free(L);
        return -1;
    }

    for (pg = 0; pg < pd_layout_page_count(L); pg++) {
        pd_draw* it;

        pd_layout_page_items(L, pg, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, pg, it, n, &n);

        for (k = 0; k < n; k++) {
            c += it[k].kind == kind && (!color || it[k].color == color);
        }

        free(it);
    }

    pd_layout_free(L);
    return c;
}

static pd_font* rtl_font;

static const pd_font* rtl_resolver(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;
    (void)family;
    (void)weight;
    (void)italic;
    return rtl_font;
}

/* the glyphs drawn on page 0 for a code point: the leftmost x, the rightmost x + w; 0 when there are none */
static int glyph_span(pd_layout* L, uint32_t cp, pd_sp* lo, pd_sp* hi) {
    int32_t n = 0, k, any = 0;
    pd_draw* it;

    if (pd_layout_page_items(L, 0, NULL, 0, &n) != PD_OK || !(it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw)))) {
        return 0;
    }

    pd_layout_page_items(L, 0, it, n, &n);

    for (k = 0; k < n; k++) {
        if (it[k].kind == PD_DRAW_GLYPH && it[k].text == cp) {
            *lo = any && *lo < it[k].x ? *lo : it[k].x;
            *hi = any && *hi > it[k].x + it[k].w ? *hi : it[k].x + it[k].w;
            any = 1;
        }
    }

    free(it);
    return any;
}

/* Right-to-left text from Word: a bidi paragraph at the right (Word's left alignment, its start), its indent and
   first-line indent from the right, a justified one's last line at the right, a list label right of its text; a
   left-to-right paragraph at the left; a bidiVisual table's first column at the right; Arabic letters joined (the
   font's initial, medial, final forms; lam with alef one glyph); the caret stepping as the text is shown */
static void test_docx_rtl(void) {
    pd_doc* d;
    pd_layout* L = NULL;
    pd_sp lo = 0, hi = 0, lo2 = 0, hi2 = 0, right = PD_PT(612 - 72), left = PD_PT(72);

    if (!rtl_font && pd_font_load_file("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 0, &rtl_font) != PD_OK) {
        return;     /* no font with Hebrew and Arabic: skipped */
    }

    d = docx_doc("word/numbering.xml",
                 "<w:numbering xmlns:w=\"w\"><w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:start "
                 "w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%1.\"/><w:pPr><w:ind w:left=\"720\" "
                 "w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId "
                 "w:val=\"1\"/></w:num></w:numbering>",
                 "word/document.xml",
                 "<w:document xmlns:w=\"w\"><w:body>"
                 "<w:p><w:pPr><w:bidi/></w:pPr><w:r><w:t>\xD7\x90\xD7\x91\xD7\x92</w:t></w:r></w:p>"     /* aleph bet gimel */
                 "<w:p><w:pPr><w:bidi/><w:ind w:left=\"1440\" w:firstLine=\"720\"/></w:pPr><w:r><w:t>\xD7\x93</w:t>"
                 "</w:r></w:p>"                                                                            /* dalet */
                 "<w:p><w:r><w:t>Q</w:t></w:r></w:p>"
                 "<w:p><w:pPr><w:bidi/><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r>"
                 "<w:t>\xD7\x94</w:t></w:r></w:p>"                                                        /* he */
                 "<w:tbl><w:tblPr><w:bidiVisual/><w:tblW w:w=\"4000\" w:type=\"dxa\"/></w:tblPr><w:tblGrid><w:gridCol "
                 "w:w=\"2000\"/><w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr><w:tc><w:p><w:r><w:t>X</w:t></w:r></w:p>"
                 "</w:tc><w:tc><w:p><w:r><w:t>Y</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
                 "<w:p><w:pPr><w:bidi/></w:pPr><w:r><w:t>\xD8\xA8\xD9\x8A\xD8\xAA \xD9\x84\xD8\xA7</w:t></w:r></w:p>"
                 "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" "
                 "w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>"
                 "</w:body></w:document>", NULL);
    CHECK(d != NULL);

    if (!d) {
        return;
    }

    pd_doc_set_font_resolver(d, rtl_resolver, NULL);
    CHECK(pd_doc_para_rtl(d, pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0)) == 1);
    CHECK(pd_doc_para_rtl(d, pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 2)) == 0);

    if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
        pd_pos at, to;

        CHECK(glyph_span(L, 0x05D0, &lo, &hi) && hi > right - PD_PT(1) && hi <= right + PD_PT(1));     /* at the right */
        CHECK(glyph_span(L, 0x05D2, &lo2, &hi2) && hi2 <= lo + 1);     /* gimel left of aleph: read from the right */
        CHECK(glyph_span(L, 0x05D3, &lo, &hi) && hi > right - PD_PT(109) && hi < right - PD_PT(107));  /* 1.5in */
        CHECK(glyph_span(L, 'Q', &lo, &hi) && lo > left - PD_PT(1) && lo < left + PD_PT(1));
        CHECK(glyph_span(L, 0x05D4, &lo, &hi) && glyph_span(L, '1', &lo2, &hi2) && lo2 > hi);         /* label right */
        CHECK(glyph_span(L, 'X', &lo, &hi) && glyph_span(L, 'Y', &lo2, &hi2) && lo > hi2);             /* X right of Y */
        CHECK(glyph_span(L, 'X', &lo, &hi) && hi > right - PD_PT(101) && hi <= right);                 /* from the right */

        at.block = pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0);
        at.offset = 0;
        CHECK(pd_layout_caret_step(L, at, -1, &to) == PD_OK && to.offset == 2);    /* left: forward, past aleph */
        CHECK(pd_layout_caret_step(L, at, 1, &to) == PD_ERR_RANGE);                /* right: its start, the edge */
    }

    {   /* Arabic joined: beh initial, yeh medial, teh final (not their isolated glyphs); lam with alef one glyph */
        pd_para* p = NULL;
        pd_style st;
        pd_params prm;
        pd_glyph g[16];
        int32_t n = 0;
        const char* ar = "\xD8\xA8\xD9\x8A\xD8\xAA \xD9\x84\xD8\xA7";

        pd_style_init(&st, rtl_font, PD_PT(12));
        pd_params_init(&prm);
        prm.width = PD_PT(300);
        prm.direction = PD_DIR_RTL;
        CHECK(pd_para_new(&p) == PD_OK && pd_para_add_text(p, ar, strlen(ar), &st) == PD_OK &&
              pd_para_break(p, &prm, NULL) == PD_OK && pd_para_get_glyphs(p, 0, g, 16, &n) == PD_OK);

        if (p && n > 0) {
            int32_t k, glyphs = 0, isolated = 0;

            for (k = 0; k < n; k++) {
                if (g[k].kind == PD_GLYPH) {
                    glyphs++;
                    isolated += g[k].glyph == pd_font_glyph_index(rtl_font, 0x0628) ||
                                g[k].glyph == pd_font_glyph_index(rtl_font, 0x064A) ||
                                g[k].glyph == pd_font_glyph_index(rtl_font, 0x062A);
                }
            }

            CHECK(glyphs == 4 && isolated == 0);
        }

        pd_para_free(p);
    }

    pd_layout_free(L);
    pd_doc_free(d);
}

/* Direction through HTML and RTF: dir="rtl" (CSS's right its start), a right-to-left table; RTF's \rtlpar with
   \qr (the right, its start) and \taprtl; each written back the same */
static void test_rtl_html_rtf(void) {
    static const char html[] = "<p dir=\"rtl\" style=\"text-align:right\">\xD7\x90</p><p style=\"text-align:right\">"
                               "Q</p><table dir=\"rtl\"><tr><td>X</td><td>Y</td></tr></table>";
    static const char rtf[] = "{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}\\pard\\rtlpar\\qr A\\par\\pard\\ql B\\par"
                              "\\trowd\\taprtl\\cellx2000\\cellx4000\\pard\\intbl X\\cell Y\\cell\\row\\pard C\\par}";
    pd_doc* d = NULL;
    pd_para_props pp;
    pd_table_props tp;
    buf_t b = { NULL, 0 };
    pd_block_id sec;

    CHECK(pd_doc_import(html, sizeof(html) - 1, PD_CONV_HTML, &d) == PD_OK && d);

    if (d) {
        sec = pd_doc_child(d, pd_doc_root(d), 0);
        CHECK(pd_doc_para_props(d, pd_doc_child(d, sec, 0), &pp) == PD_OK && pp.direction == PD_DIR_RTL &&
              pp.align == PD_ALIGN_LEFT);   /* right: its start */
        CHECK(pd_doc_para_props(d, pd_doc_child(d, sec, 1), &pp) == PD_OK && pp.align == PD_ALIGN_RIGHT);
        CHECK(pd_doc_table_props(d, pd_doc_child(d, sec, 2), &tp) == PD_OK && tp.direction == PD_DIR_RTL);
        CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &b) == PD_OK && b.p && strstr(b.p, "<p dir=\"rtl\">") &&
              count_of(b.p, " dir=\"rtl\">") == 2);
        free(b.p);
        b.p = NULL;
        b.n = 0;
        CHECK(pd_doc_export(d, PD_CONV_RTF, to_buf, &b) == PD_OK && b.p && strstr(b.p, "\\rtlpar\\qr") &&
              strstr(b.p, "\\taprtl"));
        free(b.p);
        b.p = NULL;
        b.n = 0;
        pd_doc_free(d);
        d = NULL;
    }

    CHECK(pd_doc_import(rtf, sizeof(rtf) - 1, PD_CONV_RTF, &d) == PD_OK && d);

    if (d) {
        int32_t k, n = 0, tables = 0, rtl = 0;
        pd_block_info bi;

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        CHECK(pd_doc_para_props(d, pd_doc_child(d, sec, 0), &pp) == PD_OK && pp.direction == PD_DIR_RTL &&
              pp.align == PD_ALIGN_LEFT);

        if (pd_doc_block_info(d, sec, &bi) == PD_OK) {
            n = bi.child_count;
        }

        for (k = 0; k < n; k++) {
            pd_block_info ci;

            if (pd_doc_block_info(d, pd_doc_child(d, sec, k), &ci) == PD_OK && ci.kind == PD_BLOCK_TABLE) {
                tables++;
                rtl += pd_doc_table_props(d, pd_doc_child(d, sec, k), &tp) == PD_OK && tp.direction == PD_DIR_RTL;
            }
        }

        CHECK(tables == 1 && rtl == 1);
        pd_doc_free(d);
    }
}

/* A text box in a shape turned a quarter: its text laid out across the shape's own width and drawn turned with it
   (each glyph turned, the line running down the page); one whose bodyPr keeps it upright: drawn as it is */
static void test_docx_turned_text(void) {
    pd_doc* d;
    pd_layout* L = NULL;
    pd_draw* it;
    int32_t n = 0, k, turned = 0, upright = 0;
    pd_sp x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    if (!have_layout_font()) {
        return;
    }

    d = docx_doc("word/document.xml",
                 "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wps=\"wps\" xmlns:wpg=\"wpg\">"
                 "<w:body><w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"2540000\"/><a:graphic>"
                 "<a:graphicData><wpg:wgp><wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" "
                 "cy=\"2540000\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"2540000\" cy=\"2540000\"/></a:xfrm>"
                 "</wpg:grpSpPr><wps:wsp><wps:spPr><a:xfrm rot=\"5400000\"><a:off x=\"0\" y=\"1016000\"/><a:ext "
                 "cx=\"2540000\" cy=\"508000\"/></a:xfrm><a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx>"
                 "<w:txbxContent><w:p><w:r><w:t>Turn</w:t></w:r></w:p></w:txbxContent></wps:txbx><wps:bodyPr/>"
                 "</wps:wsp><wps:wsp><wps:spPr><a:xfrm rot=\"5400000\"><a:off x=\"0\" y=\"0\"/><a:ext "
                 "cx=\"2540000\" cy=\"508000\"/></a:xfrm><a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx>"
                 "<w:txbxContent><w:p><w:r><w:t>Up</w:t></w:r></w:p></w:txbxContent></wps:txbx>"
                 "<wps:bodyPr upright=\"1\"/></wps:wsp></wpg:wgp></a:graphicData></a:graphic></wp:inline></w:drawing>"
                 "</w:r></w:p></w:body></w:document>", NULL);
    CHECK(d != NULL);

    if (!d) {
        return;
    }

    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK &&
            pd_layout_page_items(L, 0, NULL, 0, &n) == PD_OK && (it = (pd_draw*)malloc(((size_t)n + 1) *
                    sizeof(pd_draw))) != NULL) {
        pd_layout_page_items(L, 0, it, n, &n);

        for (k = 0; k < n; k++) {
            if (it[k].kind != PD_DRAW_GLYPH) {
                continue;
            }

            if (it[k].rotation == 5400000) {
                if (!turned++) {
                    x0 = it[k].x;
                    y0 = it[k].y;
                }

                x1 = it[k].x;
                y1 = it[k].y;
            } else if (it[k].rotation == 0 && it[k].region == 5) {
                upright++;
            }
        }

        free(it);
    }

    CHECK(turned == 4 && upright == 2);
    CHECK(y1 - y0 > PD_PT(10) && x1 - x0 < PD_PT(1) && x0 - x1 < PD_PT(1));     /* down the page */
    pd_layout_free(L);
    pd_doc_free(d);
}

/* A text box in a group holding a table (a spanned head row, a shaded cell), a paragraph in a style of its own
   with a superscript, and a picture in its line: the drawing has them all, laid out as a table, and keeps them
   through DOCX. */
static void test_docx_group_textbox_table(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:styleId=\"Small\"><w:name w:val=\"Small\"/>"
        "<w:rPr><w:sz w:val=\"14\"/></w:rPr></w:style></w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\" "
        "xmlns:wpg=\"wpg\" xmlns:wps=\"wps\" xmlns:mc=\"mc\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"2540000\"/><a:graphic><a:graphicData><wpg:wgp>"
        "<wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"2540000\"/><a:chOff x=\"0\" y=\"0\"/>"
        "<a:chExt cx=\"2540000\" cy=\"2540000\"/></a:xfrm></wpg:grpSpPr>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"2540000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx><w:txbxContent>"
        "<w:tbl><w:tblPr><w:jc w:val=\"center\"/><w:tblBorders><w:top w:val=\"single\" w:sz=\"4\"/></w:tblBorders>"
        "</w:tblPr><w:tblGrid><w:gridCol w:w=\"1000\"/><w:gridCol w:w=\"2000\"/></w:tblGrid>"
        "<w:tr><w:tc><w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr><w:p><w:r><w:t>Head</w:t></w:r></w:p></w:tc></w:tr>"
        "<w:tr><w:tc><w:tcPr><w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"CCCCCC\"/></w:tcPr><w:p><w:r>"
        "<w:t>a</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
        "<w:p><w:pPr><w:pStyle w:val=\"Small\"/></w:pPr><w:r><w:t>cm</w:t></w:r><w:r><w:rPr><w:vertAlign "
        "w:val=\"superscript\"/></w:rPr><w:t>2</w:t></w:r><w:r><w:drawing><wp:inline><wp:extent cx=\"254000\" "
        "cy=\"127000\"/><a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill>"
        "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"254000\" cy=\"127000\"/></a:xfrm></pic:spPr></pic:pic>"
        "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
        "</w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp></wpg:wgp></a:graphicData></a:graphic></wp:inline>"
        "</w:drawing></w:r></w:p></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_inline o;
        char* js;
        char want[64];

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
        js = drawing_json(d, o.resource);
        CHECK(js != NULL);

        if (js) {
            pd_block_id head = story_para_with(d, "Head"), cm = story_para_with(d, "cm");
            pd_block_info hb;
            pd_inline pic;

            CHECK(strstr(js, "\"story\":") != NULL);
            /* the text box a story: its table a table (Head in a cell of it), its paragraph in its style */
            CHECK(head != 0 && story_para_with(d, "b") != 0);
            CHECK(head && pd_doc_block_info(d, head, &hb) == PD_OK && pd_doc_block_info(d, hb.parent, &hb) == PD_OK &&
                  hb.kind == PD_BLOCK_CELL);
            CHECK(cm != 0 && chars_at(d, cm, 0).size == PD_PT(7));     /* the paragraph's style's size */
            CHECK(cm != 0 && chars_at(d, cm, 2).shift == PD_SHIFT_SUPER);
            CHECK(cm != 0 && pd_doc_inline_at(d, at(cm, 3), &pic) == PD_OK && pic.kind == PD_INLINE_IMAGE);
            (void)want;
            free(js);
        }

        /* laid out: the shaded cell, the rules, the picture in the line */
        if (!have_layout_font()) {
            continue;
        }

        CHECK(count_draws(d, PD_DRAW_RULE, 0xFFCCCCCCu) == 1);
        CHECK(count_draws(d, PD_DRAW_RULE, 0xFF000000u) >= 1);     /* the table's top border */
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 1);
    }

    pd_doc_free(d);
}

/* A floating picture deleted with tracked changes: it keeps its place in the document and its deletion through
   DOCX, but takes no room where deletions are not shown, and is drawn where they are. */
static void test_docx_deleted_float(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Text </w:t></w:r><w:del w:id=\"1\" w:author=\"A\"><w:r><w:drawing>"
        "<wp:anchor distL=\"114300\" distR=\"114300\"><wp:positionH relativeFrom=\"column\"><wp:align>right</wp:align>"
        "</wp:positionH><wp:extent cx=\"1270000\" cy=\"1270000\"/><wp:wrapSquare wrapText=\"bothSides\"/><a:graphic>"
        "<a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill></pic:pic></a:graphicData>"
        "</a:graphic></wp:anchor></w:drawing></w:r></w:del><w:r><w:t>beside.</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(pd_doc_revision_count(d) >= 1);

        if (!have_layout_font()) {
            continue;
        }

        pd_doc_set_markup(d, PD_MARKUP_BALLOONS);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 0);
        pd_doc_set_markup(d, PD_MARKUP_FINAL);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 0);
        pd_doc_set_markup(d, PD_MARKUP_INLINE);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 1);
        pd_doc_set_markup(d, PD_MARKUP_ORIGINAL);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 1);
        pd_doc_set_markup(d, PD_MARKUP_BALLOONS);
    }

    pd_doc_free(d);
}

/* within a step or two of an expected colour, channel by channel: Word's own rounding is not quite anyone's */
static int near_color(uint32_t got, uint32_t want) {
    int k;

    for (k = 0; k < 32; k += 8) {
        int a = (int)((got >> k) & 255), b = (int)((want >> k) & 255);

        if (a - b > 2 || b - a > 2) {
            return 0;
        }
    }

    return 1;
}

/* Theme colours: the document's own scheme (an srgbClr, a sysClr's last colour), named by w:themeColor on text,
   shading and borders, lightened by w:themeTint and darkened by w:themeShade; a document with no theme gets
   Office's. Kept through DOCX as the colours they come to. */
static void test_docx_theme_colors(void) {
    static const char* theme =
        "<a:theme xmlns:a=\"a\"><a:themeElements><a:clrScheme name=\"T\">"
        "<a:dk1><a:sysClr val=\"windowText\" lastClr=\"111111\"/></a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
        "<a:dk2><a:srgbClr val=\"222222\"/></a:dk2><a:lt2><a:srgbClr val=\"EEEEEE\"/></a:lt2>"
        "<a:accent1><a:srgbClr val=\"336699\"/></a:accent1><a:accent2><a:srgbClr val=\"C0504D\"/></a:accent2>"
        "<a:accent3><a:srgbClr val=\"9BBB59\"/></a:accent3><a:accent4><a:srgbClr val=\"8064A2\"/></a:accent4>"
        "<a:accent5><a:srgbClr val=\"4BACC6\"/></a:accent5><a:accent6><a:srgbClr val=\"F79646\"/></a:accent6>"
        "<a:hlink><a:srgbClr val=\"0000FF\"/></a:hlink><a:folHlink><a:srgbClr val=\"800080\"/></a:folHlink>"
        "</a:clrScheme><a:fontScheme name=\"F\"><a:majorFont><a:latin typeface=\"Cambria\"/></a:majorFont>"
        "<a:minorFont><a:latin typeface=\"Calibri\"/></a:minorFont></a:fontScheme></a:themeElements></a:theme>";
    static const char* body =
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:r><w:rPr><w:color w:val=\"FF0000\" w:themeColor=\"accent1\"/></w:rPr><w:t>a</w:t></w:r>"
        "<w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"accent1\" w:themeTint=\"99\"/></w:rPr><w:t>b</w:t></w:r>"
        "<w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"accent1\" w:themeShade=\"BF\"/></w:rPr><w:t>c</w:t></w:r>"
        "<w:r><w:rPr><w:color w:val=\"000000\" w:themeColor=\"text1\"/></w:rPr><w:t>d</w:t></w:r>"
        "<w:r><w:rPr><w:shd w:val=\"clear\" w:fill=\"auto\" w:themeFill=\"accent2\" w:themeFillShade=\"BF\"/></w:rPr>"
        "<w:t>e</w:t></w:r><w:r><w:rPr><w:color w:val=\"00FF00\"/></w:rPr><w:t>f</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"8\" w:color=\"000000\" w:themeColor=\"accent2\"/></w:pBdr>"
        "<w:shd w:val=\"clear\" w:fill=\"FFFFFF\" w:themeFill=\"accent1\" w:themeFillTint=\"33\"/></w:pPr>"
        "<w:r><w:t>g</w:t></w:r></w:p></w:body></w:document>";
    pd_doc* d = docx_doc("word/theme/theme1.xml", theme, "word/document.xml", body, NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, p;
        pd_para_props pp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        p = pd_doc_child(d, sec, 0);
        CHECK(chars_at(d, p, 0).color == 0xFF336699u);              /* the theme's, not w:val's */
        CHECK(near_color(chars_at(d, p, 1).color, 0xFF75A3D1u));    /* 60% of it, the rest white */
        CHECK(near_color(chars_at(d, p, 2).color, 0xFF264C73u));    /* 75% of its luminance */
        CHECK(chars_at(d, p, 3).color == 0xFF111111u);              /* text1: dk1, a system colour's last value */
        CHECK(near_color(chars_at(d, p, 4).background, 0xFF953735u));
        CHECK(chars_at(d, p, 5).color == 0xFF00FF00u);
        pp = para_resolved(d, pd_doc_child(d, sec, 1));
        CHECK(pp.border_color == 0xFFC0504Du);
        CHECK(near_color(pp.shading, 0xFFD1E0F0u));
    }

    pd_doc_free(d);

    /* a shape's scheme colour, its luminance modified (DrawingML's lumMod, lumOff) */
    d = docx_doc("word/theme/theme1.xml", theme, "word/document.xml",
                 "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpc=\"wpc\" xmlns:wps=\"wps\"><w:body>"
                 "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"1270000\" cy=\"635000\"/><a:graphic><a:graphicData>"
                 "<wpc:wpc><wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
                 "<a:prstGeom prst=\"rect\"/><a:solidFill><a:schemeClr val=\"accent2\"><a:lumMod val=\"40000\"/>"
                 "<a:lumOff val=\"60000\"/></a:schemeClr></a:solidFill><a:ln><a:noFill/></a:ln></wps:spPr><wps:bodyPr/>"
                 "</wps:wsp></wpc:wpc></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>",
                 NULL);
    CHECK(d != NULL);

    if (d) {
        pd_inline o;
        char* js;
        const char* f;

        CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
        js = drawing_json(d, o.resource);
        f = js ? strstr(js, "\"fill\":") : NULL;
        CHECK(f != NULL && near_color((uint32_t)strtoul(f + 7, NULL, 10), 0xFFE6B9B8u));
        free(js);
        pd_doc_free(d);
    }

    /* no theme part: Office's */
    d = docx_doc("word/document.xml", "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:rPr>"
                 "<w:color w:val=\"000000\" w:themeColor=\"accent1\"/></w:rPr><w:t>x</w:t></w:r></w:p></w:body></w:document>",
                 NULL);
    CHECK(d != NULL);

    if (d) {
        CHECK(chars_at(d, pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0).color == 0xFF4472C4u);
        pd_doc_free(d);
    }
}

/* Word's table styles: the default style's cell margins, a style's rules
   (top, bottom and between rows only), its header row (bold, shaded, ruled
   under) and banded rows, as the table's tblLook allows; the table's own
   indent, width in percent, a row height and a cell's own edges. Read, and
   what the model holds kept through DOCX. */
static void test_docx_table_styles(void) {
    pd_doc* d = docx_doc(
        "word/styles.xml",
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
        "<w:name w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"160\"/></w:pPr></w:style>"
        "<w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableNormal\"><w:name w:val=\"Normal Table\"/>"
        "<w:tblPr><w:tblCellMar><w:top w:w=\"0\" w:type=\"dxa\"/><w:left w:w=\"108\" w:type=\"dxa\"/>"
        "<w:bottom w:w=\"0\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar></w:tblPr></w:style>"
        "<w:style w:type=\"table\" w:styleId=\"Lined\"><w:name w:val=\"Lined\"/><w:basedOn w:val=\"TableNormal\"/>"
        "<w:pPr><w:spacing w:after=\"0\"/></w:pPr>"
        "<w:tblPr><w:tblBorders><w:top w:val=\"single\" w:sz=\"12\" w:color=\"000000\"/>"
        "<w:bottom w:val=\"single\" w:sz=\"12\" w:color=\"000000\"/>"
        "<w:insideH w:val=\"single\" w:sz=\"12\" w:color=\"000000\"/></w:tblBorders></w:tblPr>"
        "<w:tblStylePr w:type=\"firstRow\"><w:rPr><w:b/></w:rPr><w:tcPr><w:shd w:val=\"clear\" w:fill=\"4472C4\"/>"
        "<w:tcBorders><w:bottom w:val=\"single\" w:sz=\"24\" w:color=\"FF0000\"/></w:tcBorders></w:tcPr></w:tblStylePr>"
        "<w:tblStylePr w:type=\"band1Horz\"><w:tcPr><w:shd w:val=\"clear\" w:fill=\"D9E2F3\"/></w:tcPr></w:tblStylePr>"
        "</w:style></w:styles>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:tbl><w:tblPr><w:tblStyle w:val=\"Lined\"/>"
        "<w:tblW w:w=\"2500\" w:type=\"pct\"/><w:tblInd w:w=\"720\" w:type=\"dxa\"/>"
        "<w:tblLook w:firstRow=\"1\" w:noHBand=\"0\" w:noVBand=\"1\"/></w:tblPr>"
        "<w:tblGrid><w:gridCol w:w=\"1000\"/><w:gridCol w:w=\"1000\"/></w:tblGrid>"
        "<w:tr><w:tc><w:p><w:r><w:t>H1</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>H2</w:t></w:r></w:p></w:tc></w:tr>"
        "<w:tr><w:trPr><w:trHeight w:val=\"720\"/></w:trPr>"
        "<w:tc><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr>"
        "<w:tr><w:tc><w:tcPr><w:tcBorders><w:top w:val=\"nil\"/></w:tcBorders></w:tcPr>"
        "<w:p><w:r><w:t>c</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>d</w:t></w:r></w:p></w:tc></w:tr>"
        "</w:tbl><w:p/></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id t;
        pd_table_props tp;
        pd_cell_props c;
        pd_char_props cp;
        pd_para_props pp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        t = first_table(d);
        CHECK(t != 0 && pd_doc_table_props(d, t, &tp) == PD_OK);
        CHECK(tp.style != 0 && !strcmp(pd_doc_style_name(d, tp.style), "Lined") && tp.look == (PD_TLOOK_FIRST_ROW |
                PD_TLOOK_NO_VBAND) && tp.border_given == 0);   /* the style itself, named, its rules its own */
        CHECK(pd_doc_table_resolve(d, t, &tp) == PD_OK);
        CHECK(tp.border == PD_PT(1.5) && tp.border_sides == (PD_TBORDER_TOP | PD_TBORDER_BOTTOM | PD_TBORDER_INSIDE_H));
        CHECK(tp.cell_padding == PD_PT(5.4) && tp.cell_padding_v == 0 && tp.indent == PD_PT(36) &&
              tp.width_pct == 500);

        /* the header row: shaded, a rule of its own under it, bold text; the first body row banded */
        c = cell_props_at(d, t, 0, 0);
        CHECK(c.background == 0xFF4472C4u && (c.border_set & PD_BORDER_BOTTOM) && (c.border_on & PD_BORDER_BOTTOM) &&
              c.border_width == PD_PT(3) && c.border_color == 0xFFFF0000u);
        cp = chars_at(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 0), 0), 0), 0);
        CHECK(cp.weight == 700);
        c = cell_props_at(d, t, 1, 1);
        CHECK(c.background == 0xFFD9E2F3u && c.min_height == PD_PT(36));
        cp = chars_at(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 1), 0), 0), 0);
        CHECK(cp.weight == 400);

        /* the second body row: not banded; a cell that takes its top rule away */
        c = cell_props_at(d, t, 2, 0);
        CHECK(c.background == 0 && c.border_set == PD_BORDER_TOP && c.border_on == 0);

        /* the style's paragraph spacing in the cells, over Normal's */
        CHECK(pd_doc_para_resolve(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 2), 1), 0), &pp) == PD_OK &&
              pp.space_after == 0);
        (void)para_resolved;
    }

    pd_doc_free(d);
}

/* a list level's label font (bold, a family, a colour) and its restart
   rule: a second level that never counts again, so its items go on 1, 2
   then 3 under the next first-level item */
static void test_docx_list_levels(void) {
    pd_doc* d = docx_doc(
        "word/numbering.xml",
        "<w:numbering xmlns:w=\"w\"><w:abstractNum w:abstractNumId=\"0\">"
        "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%1.\"/>"
        "<w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>"
        "<w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\"/><w:b/><w:color w:val=\"C00000\"/></w:rPr></w:lvl>"
        "<w:lvl w:ilvl=\"1\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/><w:lvlRestart w:val=\"0\"/>"
        "<w:lvlText w:val=\"%2)\"/><w:pPr><w:ind w:left=\"1440\" w:hanging=\"360\"/></w:pPr></w:lvl>"
        "</w:abstractNum><w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num></w:numbering>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>A</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>b</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>B</w:t></w:r></w:p>"
        "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/><w:numId w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>c</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec;
        pd_block_info bi;
        pd_list_level lv[9];
        int32_t nlv = 0;
        char lab[32];

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        label_of(d, pd_doc_child(d, sec, 3), lab);
        CHECK(strcmp(lab, "2.") == 0);
        label_of(d, pd_doc_child(d, sec, 4), lab);
        CHECK(strcmp(lab, "3)") == 0);      /* not 1) */

        pd_doc_block_info(d, pd_doc_child(d, sec, 0), &bi);
        CHECK(pd_doc_list_info(d, bi.list, &nlv, lv) == PD_OK && nlv >= 2);
        CHECK(strcmp(lv[0].label_family, "Arial") == 0 && lv[0].label_weight == 700 && lv[0].label_color == 0xFFC00000u);
        CHECK(lv[0].restart_after == 0 && lv[1].restart_after == -1 && lv[1].label_family[0] == 0);
    }

    pd_doc_free(d);
}

/* Word's line numbering: every fifth line from 1 (Word writes 0), 18pt
   from the text, counted again in each section */
static void test_docx_line_numbers(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>One.</w:t></w:r></w:p>"
        "<w:sectPr><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\"/>"
        "<w:lnNumType w:countBy=\"5\" w:start=\"0\" w:distance=\"360\" w:restart=\"newSection\"/></w:sectPr>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_section_props sp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp) == PD_OK);
        CHECK(sp.line_numbers == 5 && sp.line_number_start == 1 && sp.line_number_distance == PD_PT(18) &&
              sp.line_number_restart == PD_LINENUM_SECTION);
    }

    pd_doc_free(d);
}

/* A header's logo: its picture named in the header's own relationships
   (rId5 means something else in the document's), a float in the header
   beside its text, placed from the page's edge -- kept through DOCX. */
static void test_docx_header_logo(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId1\" Type=\"t/header\" Target=\"header1.xml\"/>"
        "<Relationship Id=\"rId5\" Type=\"t/styles\" Target=\"styles.xml\"/></Relationships>",
        "word/_rels/header1.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/header1.xml",
        "<w:hdr xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:p><w:pPr><w:jc w:val=\"right\"/>"
        "</w:pPr>" DRAWING("anchor", "<wp:positionH relativeFrom=\"page\"><wp:posOffset>457200</wp:posOffset></wp:positionH>"
                           "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>"
                           "<wp:wrapSquare wrapText=\"bothSides\"/>")
        "<w:r><w:t>Title</w:t></w:r></w:p></w:hdr>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:r=\"r\"><w:body><w:p><w:r><w:t>Body.</w:t></w:r></w:p>"
        "<w:sectPr><w:headerReference w:type=\"default\" r:id=\"rId1\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\"/>"
        "</w:sectPr></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_section_props sp;
        pd_block_id fl, p;
        pd_block_info bi;
        pd_float_props fp;
        pd_inline o;
        const char* mime = NULL;
        const void* data;
        size_t len = 0;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp) == PD_OK && sp.header != 0);
        fl = pd_doc_child(d, sp.header, 0);
        CHECK(pd_doc_block_info(d, fl, &bi) == PD_OK && bi.kind == PD_BLOCK_FLOAT);
        CHECK(pd_doc_float_props(d, fl, &fp) == PD_OK && fp.wrap == PD_WRAP_LEFT && (fp.placement & PD_PLACE_OFFSET));
        CHECK(fp.offset_x == PD_PT(36) - PD_PT(72));    /* half an inch from the page's edge: in the margin */
        p = pd_doc_child(d, fl, 0);
        CHECK(pd_doc_inline_at(d, at(p, 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE);
        CHECK(pd_doc_resource(d, o.resource, &mime, &data, &len) == PD_OK && strcmp(mime, "image/png") == 0 && len > 8);
        CHECK(text_is(d, pd_doc_child(d, sp.header, 1), "Title"));
    }

    pd_doc_free(d);
}

/* a drawing placed from the page's top: so when read, and written back so */
static void test_docx_float_from_page(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:r><w:t>Some text.</w:t></w:r>"
        DRAWING("anchor", "<wp:positionH relativeFrom=\"column\"><wp:posOffset>0</wp:posOffset></wp:positionH>"
                "<wp:positionV relativeFrom=\"page\"><wp:posOffset>3810000</wp:posOffset></wp:positionV>"
                "<wp:wrapSquare wrapText=\"right\"/>")
        "</w:p></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, fl = 0;
        pd_block_info bi;
        pd_float_props fp;
        int i;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);

        for (i = 0; pd_doc_block_info(d, pd_doc_child(d, sec, i), &bi) == PD_OK; i++) {
            if (bi.kind == PD_BLOCK_FLOAT) {
                fl = bi.id;
            }
        }

        CHECK(fl && pd_doc_float_props(d, fl, &fp) == PD_OK && fp.offset_from == PD_FROM_PAGE &&
              fp.offset_y == PD_PT(300));
    }

    pd_doc_free(d);
}

/* a drawing Word moved down the paragraph it is anchored in: as far down when read, and written back so */
static void test_docx_float_offset_y(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:r><w:t>Above.</w:t></w:r></w:p><w:p>"
        DRAWING("anchor", "<wp:positionH relativeFrom=\"column\"><wp:posOffset>0</wp:posOffset></wp:positionH>"
                "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>457200</wp:posOffset></wp:positionV>"
                "<wp:wrapSquare wrapText=\"right\"/>")
        "<w:r><w:t>Beside.</w:t></w:r></w:p></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, fl;
        pd_block_info bi;
        pd_float_props fp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        fl = pd_doc_child(d, sec, 1);
        CHECK(pd_doc_block_info(d, fl, &bi) == PD_OK && bi.kind == PD_BLOCK_FLOAT);
        CHECK(pd_doc_float_props(d, fl, &fp) == PD_OK && fp.offset_y == PD_PT(36) && fp.wrap == PD_WRAP_LEFT);
        CHECK(text_is(d, pd_doc_child(d, sec, 2), "Beside."));
    }

    pd_doc_free(d);
}

/* the items of a drawing resource, as text (caller frees) */
static char* drawing_json(const pd_doc* d, pd_res_id res) {
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    char* out;

    if (pd_doc_resource(d, res, &mime, &data, &len) != PD_OK || strcmp(mime, "application/vnd.parade.drawing+json")) {
        return NULL;
    }

    out = (char*)malloc(len + 1);
    memcpy(out, data, len);
    out[len] = '\0';
    return out;
}

/* PowerPoint: each slide a page as big, its canvas in front of the text from the page's corner; placeholders where
   the master has them, their text styled as its text styles say; a shape's text, a connector, the slide number, a
   cropped picture, a table */
static void test_pptx(void) {
    FILE* f = fopen("tests/data/slides.pptx", "rb");
    unsigned char* data = NULL;
    long sz = 0;
    pd_doc* d = NULL;
    int i, found_title = 0, found_bullet = 0, found_sub = 0, found_box = 0, found_num = 0, title_big = 0;
    int32_t ns;

    if (f && fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
            (data = (unsigned char*)malloc((size_t)sz)) != NULL) {
        if (fread(data, 1, (size_t)sz, f) != (size_t)sz) {
            sz = 0;
        }
    }

    if (f) {
        fclose(f);
    }

    CHECK(data && sz > 0);

    if (!data || sz <= 0) {
        free(data);
        return;
    }

    CHECK(pd_conv_detect(data, (size_t)sz) == PD_CONV_PPTX);
    CHECK(pd_doc_import(data, (size_t)sz, PD_CONV_PPTX, &d) == PD_OK && d);
    free(data);

    if (!d) {
        return;
    }

    {   /* two slides: two pages, each as big as a slide (10 by 5.625 inches), no margins, a canvas over it */
        pd_block_info ri, bi;
        int k;

        CHECK(pd_doc_block_info(d, pd_doc_root(d), &ri) == PD_OK && ri.child_count == 2);

        for (k = 0; k < ri.child_count; k++) {
            pd_block_id sec = pd_doc_child(d, pd_doc_root(d), k), fl = pd_doc_child(d, sec, 0);
            pd_section_props sp;
            pd_float_props fp;
            pd_inline o;

            CHECK(pd_doc_section_props(d, sec, &sp) == PD_OK && sp.page_width == PD_PT(720) &&
                  sp.page_height == PD_PT(405) && sp.margin_left == 0 && sp.margin_top == 0);
            CHECK(pd_doc_block_info(d, fl, &bi) == PD_OK && bi.kind == PD_BLOCK_FLOAT &&
                  pd_doc_float_props(d, fl, &fp) == PD_OK && fp.wrap == PD_WRAP_FRONT && fp.offset_from == PD_FROM_PAGE);
            CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, fl, 0), 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE &&
                  o.width == PD_PT(720));

            if (k == 1) {   /* the picture cropped, the table */
                char* js = drawing_json(d, o.resource);

                CHECK(js && strstr(js, "\"crop\":[25000,0,25000,0]") && strstr(js, "\"story\":"));
                CHECK(js && strstr(js, "\"rot\":2700000,\"fh\":1") && strstr(js, "\"clip\":["));
                CHECK(js && strstr(js, "\"fill\":4282675908"));   /* the header row's fill: a rectangle under it */
                free(js);
            } else {    /* a gradient, from red to blue across; a shadow down */
                char* js = drawing_json(d, o.resource);

                CHECK(js && strstr(js, "\"gk\":1,\"f2\":4278190335,\"ga\":0") && strstr(js, "\"shd\":[139022,139022,"));
                free(js);
            }
        }
    }

    ns = pd_doc_story_count(d);

    for (i = 0; i < ns; i++) {     /* the text boxes' text */
        pd_block_id st = pd_doc_story_at(d, i), p = pd_doc_child(d, st, 0);
        pd_block_info bi;
        uint32_t n;
        const char* t;

        if (pd_doc_block_info(d, p, &bi) != PD_OK || bi.kind != PD_BLOCK_PARAGRAPH) {
            if (bi.kind == PD_BLOCK_TABLE) {
                pd_block_id c = pd_doc_child(d, pd_doc_child(d, p, 0), 0);
                pd_cell_props cp;

                CHECK(pd_doc_cell_props(d, c, &cp) == PD_OK);
                CHECK(text_is(d, pd_doc_child(d, c, 0), "Name"));
            }

            continue;
        }

        t = para_text(d, p, &n);
        found_title |= text_is(d, p, "Hello Slides");
        found_bullet |= n > 4 && !memcmp(t, "\xE2\x80\xA2\t", 4) && strstr(t, "First point") != NULL;
        found_sub |= pd_doc_child(d, st, 1) && text_is(d, pd_doc_child(d, st, 1), "\xE2\x80\x93\tA detail");
        found_box |= text_is(d, p, "Box & text");
        found_num |= text_is(d, p, "1");

        if (text_is(d, p, "Hello Slides")) {    /* the master's title style: 44 points, bold, its colour */
            pd_run r;
            int32_t nr = 0;
            pd_char_props cp;

            if (pd_doc_para_runs(d, p, &r, 1, &nr) == PD_OK && nr >= 1 &&
                    pd_doc_format_resolve(d, p, r.format, &cp) == PD_OK) {
                title_big = cp.size == PD_PT(44) && cp.weight >= 700 && (cp.color & 0xFFFFFFu) == 0x1F3864u;
            }
        }
    }

    CHECK(found_title && title_big);
    CHECK(found_bullet && found_sub);
    CHECK(found_box && found_num);
    pd_doc_free(d);
}

/* SmartArt with no drawing saved of it, laid out from its definition: a block list in rows as many to a row as
   makes them biggest, the last row centred; a process with arrows in the gaps between its steps; a hierarchy, each
   box over its children's, lines bent down to them. And one with a drawing saved of it older than its text. */
static void test_pptx_smartart(void) {
    FILE* f = fopen("tests/data/smartart.pptx", "rb");
    unsigned char* data = NULL;
    long sz = 0;
    pd_doc* d = NULL;
    int k;

    if (f && fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
            (data = (unsigned char*)malloc((size_t)sz)) != NULL && fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        sz = 0;
    }

    if (f) {
        fclose(f);
    }

    CHECK(data && sz > 0 && pd_doc_import(data, (size_t)sz, PD_CONV_PPTX, &d) == PD_OK && d);
    free(data);

    if (!d) {
        return;
    }

    for (k = 0; k < 13; k++) {
        pd_block_id fl = pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), k), 0);
        pd_inline o;
        char* js = NULL;

        if (pd_doc_inline_at(d, at(pd_doc_child(d, fl, 0), 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE) {
            js = drawing_json(d, o.resource);
        }

        CHECK(js != NULL);

        if (!js) {
            continue;
        }

        if (k == 0) {   /* five blocks, two to a row (the biggest they can be in the frame), the fifth centred */
            CHECK(count_of(js, "\"fill\":4282675908") == 5);
            CHECK(strstr(js, "<a:off x=\\\"947738\\\" y=\\\"0\\\"/><a:ext cx=\\\"2000250\\\" cy=\\\"1200150\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"2047875\\\" y=\\\"2800350\\\"/>") != NULL);
        } else if (k == 1) {    /* three steps across the frame, two arrows between them, centred down it */
            CHECK(count_of(js, "prst=\\\"roundRect\\\"") == 3 && count_of(js, "prst=\\\"rightArrow\\\"") == 2);
            CHECK(strstr(js, "<a:off x=\\\"0\\\" y=\\\"1518987\\\"/><a:ext cx=\\\"1604211\\\" cy=\\\"962526\\\"/>") != NULL);
        } else if (k == 2) {    /* six boxes, five lines; the root over its three children, Middle over its two */
            CHECK(count_of(js, "\"fill\":4282675908") == 6 && count_of(js, "<a:custGeom>") == 5);
            CHECK(strstr(js, "<a:off x=\\\"2151529\\\" y=\\\"207309\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"2151529\\\" y=\\\"1552015\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"1075765\\\" y=\\\"2896721\\\"/>") != NULL);
        } else if (k == 4) {    /* four round a circle, top, right, bottom, left; four arrows round, the last's back */
            CHECK(count_of(js, "prst=\\\"ellipse\\\"") == 4 && count_of(js, "prst=\\\"rightArrow\\\"") == 4);
            CHECK(strstr(js, "<a:off x=\\\"2376752\\\" y=\\\"0\\\"/><a:ext cx=\\\"1342495\\\" cy=\\\"1342495\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"3705755\\\" y=\\\"1329002\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"2376752\\\" y=\\\"2658005\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"1047750\\\" y=\\\"1329002\\\"/>") != NULL);
        } else if (k == 5) {    /* the hub in the middle, five round it, sp from it, lines out to them */
            CHECK(count_of(js, "prst=\\\"ellipse\\\"") == 6 && count_of(js, "<a:custGeom>") == 5);
            CHECK(strstr(js, "<a:off x=\\\"2247730\\\" y=\\\"1360460\\\"/><a:ext cx=\\\"1600541\\\" cy=\\\"1600541\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"2567838\\\" y=\\\"0\\\"/><a:ext cx=\\\"960324\\\" cy=\\\"960324\\\"/>") != NULL);
        } else if (k == 6) {    /* three levels, a triangle on two trapezoids, no wider than equilateral */
            CHECK(count_of(js, "prst=\\\"trapezoid\\\"") == 3 && count_of(js, "val 57735") == 3);
            CHECK(strstr(js, "<a:off x=\\\"2278103\\\" y=\\\"0\\\"/><a:ext cx=\\\"1539793\\\" cy=\\\"1333500\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"738310\\\" y=\\\"2667000\\\"/><a:ext cx=\\\"4619380\\\" cy=\\\"1333500\\\"/>") != NULL);
        } else if (k == 7) {    /* upside down: wide at the top, as shapes of their own (their text upright) */
            CHECK(count_of(js, "prst=\\\"trapezoid\\\"") == 0 && count_of(js, "<a:custGeom>") == 2);
            CHECK(strstr(js, "<a:off x=\\\"738310\\\" y=\\\"0\\\"/><a:ext cx=\\\"4619380\\\" cy=\\\"2000250\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"1893155\\\" y=\\\"2000250\\\"/><a:ext cx=\\\"2309690\\\" cy=\\\"2000250\\\"/>") != NULL);
        } else if (k == 8) {    /* hanging: Init's last level to the right, one under another; Both's two to a row */
            CHECK(count_of(js, "prst=\\\"rect\\\"") == 13 && count_of(js, "<a:custGeom>") == 12);
            CHECK(strstr(js, "<a:off x=\\\"411307\\\" y=\\\"1697182\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"411307\\\" y=\\\"3394364\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"1866034\\\" y=\\\"1697182\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"3320761\\\" y=\\\"2545773\\\"/>") != NULL);
            CHECK(strstr(js, "<a:off x=\\\"4775489\\\" y=\\\"2545773\\\"/>") != NULL);
        } else if (k == 9) {    /* the picture in its round placeholder; the other placeholder its style's fill */
            CHECK(count_of(js, "<pic:pic>") == 1 && count_of(js, "prst=\\\"ellipse\\\"") == 2);
            CHECK(strstr(js, "<a:off x=\\\"1600200\\\" y=\\\"640080\\\"/><a:ext cx=\\\"4495800\\\" cy=\\\"800100\\\"/>") != NULL);
        } else if (k == 10) {   /* the arrows curved: bands of their own shape, not straight arrows */
            CHECK(count_of(js, "prst=\\\"roundRect\\\"") == 3 && count_of(js, "<a:custGeom>") == 2 &&
                  count_of(js, "Arrow") == 0);
        } else if (k == 11) {   /* turned a quarter, and so their text (gravity: reading up), centred across */
            CHECK(count_of(js, "\"story\":") == 2 && count_of(js, ",\"rot\":16200000,\"actr\":1}") == 2);
        } else if (k == 12) {   /* the theme's gradient and shadow for the style's fillRef and effectRef, its bevel */
            CHECK(count_of(js, "<a:gradFill") == 2 && count_of(js, "<a:outerShdw") == 2 &&
                  count_of(js, "<a:bevelT") == 2 && count_of(js, "\"gk\":") == 2 && count_of(js, "\"shd\":") == 2);
        }

        free(js);
    }

    {   /* slide 4, drawn as saved: the model's text, at the drawing's size, in the colour the colours give text */
        int32_t ns = pd_doc_story_count(d), i, fresh = 0, stale = 0;

        for (i = 0; i < ns; i++) {
            pd_block_id p = pd_doc_child(d, pd_doc_story_at(d, i), 0);
            pd_run r;
            int32_t nr = 0;
            pd_char_props cp;

            stale |= text_is(d, p, "Stale");

            if (text_is(d, p, "Fresh") && pd_doc_para_runs(d, p, &r, 1, &nr) == PD_OK && nr >= 1 &&
                    pd_doc_format_resolve(d, p, r.format, &cp) == PD_OK) {
                fresh = cp.size == PD_PT(10) && (cp.color & 0xFFFFFFu) == 0xFFC000u;
            }
        }

        CHECK(fresh && !stale);
    }

    pd_doc_free(d);
}

/* SmartArt in a Word document: a cycle with no drawing laid out from its definition, a diagram with a drawing
   drawn as saved (with the model's newer text), a list with a picture; each a group of shapes inline where it
   was */
static void test_docx_smartart(void) {
    FILE* f = fopen("tests/data/smartart.docx", "rb");
    unsigned char* data = NULL;
    long sz = 0;
    pd_doc* d = NULL;
    char* js = NULL;
    pd_inline o;
    int32_t ns, i, fresh = 0, stale = 0;

    if (f && fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
            (data = (unsigned char*)malloc((size_t)sz)) != NULL && fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        sz = 0;
    }

    if (f) {
        fclose(f);
    }

    CHECK(data && sz > 0 && pd_doc_import(data, (size_t)sz, PD_CONV_DOCX, &d) == PD_OK && d);
    free(data);

    if (!d) {
        return;
    }

    if (pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 1), 0), &o) == PD_OK &&
            o.kind == PD_INLINE_IMAGE) {
        js = drawing_json(d, o.resource);
    }

    CHECK(js && count_of(js, "prst=\\\"ellipse\\\"") == 4 && count_of(js, "prst=\\\"rightArrow\\\"") == 4);
    free(js);
    js = NULL;

    if (pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 5), 0), &o) == PD_OK &&
            o.kind == PD_INLINE_IMAGE) {    /* the pictures' list: its picture the document's */
        js = drawing_json(d, o.resource);
    }

    CHECK(js && count_of(js, "<pic:pic>") == 1 && count_of(js, "prst=\\\"ellipse\\\"") == 2);
    free(js);
    ns = pd_doc_story_count(d);

    for (i = 0; i < ns; i++) {
        pd_block_id p = pd_doc_child(d, pd_doc_story_at(d, i), 0);

        stale |= text_is(d, p, "Stale");
        fresh |= text_is(d, p, "Fresh");
    }

    CHECK(fresh && !stale);
    pd_doc_free(d);
}

/* A Word drawing canvas with pictures, a group inside it (whose own
   coordinates scale its picture), a filled box and a text box: one picture
   of the whole, every part where the canvas has it. And a floating text box
   holding a picture and its caption: a float with those paragraphs. Both
   kept through DOCX. */
static void test_docx_drawings(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\" xmlns:wpc=\"wpc\" "
        "xmlns:wpg=\"wpg\" xmlns:wps=\"wps\" xmlns:mc=\"mc\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"1270000\"/><a:graphic><a:graphicData><wpc:wpc>"
        "<pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
        "<a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm></pic:spPr></pic:pic>"
        "<wpg:wgp><wpg:grpSpPr><a:xfrm><a:off x=\"1270000\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/>"
        "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"2540000\" cy=\"1270000\"/></a:xfrm></wpg:grpSpPr>"
        "<pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
        "<a:ext cx=\"2540000\" cy=\"1270000\"/></a:xfrm></pic:spPr></pic:pic></wpg:wgp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"635000\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill><a:ln><a:noFill/></a:ln>"
        "</wps:spPr><wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"635000\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr><wps:txbx><w:txbxContent>"
        "<w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:rPr><w:b/><w:sz w:val=\"18\"/></w:rPr><w:t>(b) label</w:t>"
        "</w:r></w:p></w:txbxContent></wps:txbx><wps:bodyPr anchor=\"ctr\"/></wps:wsp>"
        "</wpc:wpc></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Text </w:t></w:r><w:r><mc:AlternateContent><mc:Choice Requires=\"wps\">"
        "<w:drawing><wp:anchor distL=\"114300\" distR=\"114300\"><wp:positionH relativeFrom=\"column\"><wp:align>right"
        "</wp:align></wp:positionH><wp:extent cx=\"2540000\" cy=\"1905000\"/><wp:wrapSquare wrapText=\"bothSides\"/>"
        "<a:graphic><a:graphicData><wps:wsp><wps:txbx><w:txbxContent>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"1270000\"/><a:graphic><a:graphicData><pic:pic>"
        "<pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline>"
        "</w:drawing></w:r></w:p><w:p><w:r><w:t>Fig. 9. A caption.</w:t></w:r></w:p>"
        "</w:txbxContent></wps:txbx></wps:wsp></a:graphicData></a:graphic></wp:anchor></w:drawing></mc:Choice>"
        "<mc:Fallback><w:pict><w:t>old</w:t></w:pict></mc:Fallback></mc:AlternateContent></w:r>"
        "<w:r><w:t>beside.</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id sec, p0, fl = 0;
        pd_inline o;
        char* js;
        int32_t i, n;
        pd_block_info bi;
        pd_float_props fp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        sec = pd_doc_child(d, pd_doc_root(d), 0);
        p0 = pd_doc_child(d, sec, 0);
        CHECK(pd_doc_inline_at(d, at(p0, 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE);
        CHECK(o.width == PD_PT(200) && o.height == PD_PT(100));
        js = drawing_json(d, o.resource);
        CHECK(js != NULL);

        if (js) {   /* the group's picture at half scale in the right half; the box below left; the label */
            char want[160];

            snprintf(want, sizeof(want), "\"x\":%d,\"y\":0,\"w\":%d,\"h\":%d", (int)PD_PT(100), (int)PD_PT(100), (int)PD_PT(50));
            CHECK(strstr(js, want) != NULL);
            CHECK(strstr(js, "\"fill\":4294901760") != NULL);   /* 0xFFFF0000 */
            CHECK(strstr(js, "\"story\":") != NULL && strstr(js, "\"anchor\":\"ctr\"") != NULL);
            {   /* the label a story of the document: bold, 9pt */
                pd_block_id lp = story_para_with(d, "(b) label");
                pd_char_props lc = chars_at(d, lp, 0);

                CHECK(lp != 0 && lc.weight == 700 && lc.size == PD_PT(9));
            }
            free(js);
        }

        /* the text box: a float on the right holding the picture and the caption; the anchoring text goes on */
        pd_doc_block_info(d, sec, &bi);
        n = bi.child_count;

        for (i = 0; i < n; i++) {
            pd_doc_block_info(d, pd_doc_child(d, sec, i), &bi);

            if (bi.kind == PD_BLOCK_FLOAT) {
                fl = bi.id;
            }
        }

        CHECK(fl != 0);

        if (fl) {
            CHECK(pd_doc_float_props(d, fl, &fp) == PD_OK && fp.wrap == PD_WRAP_RIGHT && fp.width == PD_PT(200));
            pd_doc_block_info(d, fl, &bi);
            CHECK(bi.child_count == 2 && text_is(d, pd_doc_child(d, fl, 1), "Fig. 9. A caption."));
        }

        CHECK(find_para(d, "Text beside.") != 0);
    }

    pd_doc_free(d);
}

static void le32(buf_t* b, uint32_t v) {
    le(b, v, 4);
}

/* an EMF record: type, size, then the words */
static void emr(buf_t* b, uint32_t type, const uint32_t* w, int n) {
    int i;

    le32(b, type);
    le32(b, 8 + 4 * (uint32_t)n);

    for (i = 0; i < n; i++) {
        le32(b, w[i]);
    }
}

/* A small EMF made here: a red-filled rectangle, a polygon with a hole
   drawn as a path, a line of text, a mask-and-picture pair of bitmaps
   (only the picture shows) and a white "nothing" blit that must not cover
   it all. Played into a drawing; the DOCX keeps the metafile itself. */
static void test_emf(void) {
    buf_t e = { NULL, 0 };
    uint32_t hdr[20];
    pd_doc* d;
    pd_res_id r, src;
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    char* js;
    int k;

    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 0; hdr[1] = 0; hdr[2] = 99; hdr[3] = 99;         /* bounds, device */
    hdr[4] = 0; hdr[5] = 0; hdr[6] = 2500; hdr[7] = 2500;     /* frame: 25 mm square, the 100 px of the device */
    hdr[8] = 0x464D4520u;                                       /* " EMF" */
    hdr[9] = 0x10000; hdr[10] = 0; hdr[11] = 0; hdr[12] = 4;   /* version, bytes (unchecked), records, handles */
    hdr[13] = 0; hdr[14] = 0;
    hdr[16] = 100; hdr[17] = 100;                               /* device: 100 x 100 px */
    hdr[18] = 25; hdr[19] = 25;                                 /* that is 25 x 25 mm: 4 px per mm */
    emr(&e, 1, hdr, 20);
    {
        uint32_t brush[4] = { 1, 0, 0x000000FF, 0 };            /* solid red (COLORREF 0x00BBGGRR) */
        uint32_t sel[1] = { 1 }, nullpen[1] = { 0x80000008u };
        uint32_t rectw[4] = { 10, 10, 50, 30 };

        emr(&e, 39, brush, 4);
        emr(&e, 37, sel, 1);
        emr(&e, 37, nullpen, 1);
        emr(&e, 43, rectw, 4);
    }
    {   /* a path: a square with a square hole, POLYPOLYGON inside BEGINPATH .. FILLPATH */
        uint32_t pp[4 + 2 + 2 + 16] = { 0, 0, 0, 0, 2, 8, 4, 4, 60, 60, 90, 60, 90, 90, 60, 90, 70, 70, 80, 70, 80, 80, 70, 80 };

        emr(&e, 59, NULL, 0);
        emr(&e, 8, pp, 4 + 2 + 2 + 16);
        emr(&e, 60, NULL, 0);
        emr(&e, 62, hdr, 4);
    }
    {   /* text: "Hi" at (20, 80), baseline-aligned */
        uint32_t al[1] = { 24 };
        uint32_t t[19 + 1];

        emr(&e, 22, al, 1);
        memset(t, 0, sizeof(t));
        t[7] = 20; t[8] = 80;           /* reference point */
        t[9] = 2; t[10] = 8 + 76;       /* chars, offset of the string from the record's start */
        t[19] = 'H' | ('i' << 16);
        emr(&e, 84, t, 20);
    }
    {   /* a 2 x 1 picture: a 1-bit mask ORed (left out), then 24-bit pixels ANDed */
        static const uint32_t bmi1[12] = { 40, 2, 1, 1 | (1 << 16), 0, 0, 0, 0, 2, 0, 0x000000, 0xFFFFFF };
        static const uint32_t bmi24[10] = { 40, 2, 1, 1 | (24 << 16), 0, 0, 0, 0, 0, 0 };
        uint32_t rec[18 + 12 + 2];
        int pass;

        for (pass = 0; pass < 2; pass++) {
            memset(rec, 0, sizeof(rec));
            rec[4] = 0; rec[5] = 40;        /* xDest, yDest */
            rec[8] = 2; rec[9] = 1;         /* cxSrc, cySrc */
            rec[10] = 80;                   /* offBmi: after the 8-byte record head and 18 words */
            rec[11] = pass ? 40 : 48;
            rec[12] = 80 + rec[11];         /* offBits */
            rec[13] = pass ? 8 : 4;         /* a row of 2 pixels: 6 bytes, padded to 8; or 1 bit each, to 4 */
            rec[15] = pass ? 0x008800C6u : 0x00EE0086u;
            rec[16] = 20; rec[17] = 10;     /* cxDest, cyDest */
            memcpy(rec + 18, pass ? bmi24 : bmi1, rec[11]);
            rec[18 + rec[11] / 4] = pass ? 0x00FF0000u : 0;    /* blue, green pixels (BGR) */
            emr(&e, 81, rec, 18 + (int)rec[11] / 4 + (pass ? 2 : 1));
        }
    }
    {   /* BITBLT with no source and a raster operation that leaves the page alone */
        uint32_t b[23];

        memset(b, 0, sizeof(b));
        b[6] = 100; b[7] = 100;
        b[8] = 0x00AA0029u;
        emr(&e, 76, b, 23);
    }
    emr(&e, 14, hdr, 3);

    pd_doc_new(&d);
    CHECK(pd_metafile_kind((const unsigned char*)e.p, e.n) == 1);
    CHECK(pd_doc_add_resource(d, "image/x-emf", e.p, e.n, &src) == PD_OK);
    r = pd_metafile_drawing(d, (const unsigned char*)e.p, e.n, src);
    CHECK(r != 0 && pd_doc_resource(d, r, &mime, &data, &len) == PD_OK);
    js = (char*)malloc(len + 1);
    memcpy(js, data, len);
    js[len] = '\0';

    {   /* 25 mm square; the red rectangle from device pixel 10 to 50 across, 10 to 30 down, of 100 */
        double W = 25 / 25.4 * 72 * 65536;
        char want[160];

        snprintf(want, sizeof(want), "\"w\":%d,\"h\":%d", (int)W, (int)W);
        CHECK(strstr(js, want) != NULL);
        snprintf(want, sizeof(want), "{\"path\":[%d,%d,%d,%d,%d,%d,%d,%d]", (int)(W * 0.1), (int)(W * 0.1), (int)(W * 0.5),
                 (int)(W * 0.1), (int)(W * 0.5), (int)(W * 0.3), (int)(W * 0.1), (int)(W * 0.3));
        CHECK(strstr(js, want) != NULL || !printf("  %s\n  %.300s\n", want, js));
    }
    CHECK(strstr(js, "\"fill\":4294901760") != NULL);
    {   /* the path's two rings: a break between them */
        char want[64];

        snprintf(want, sizeof(want), "%d,%d", (int)INT32_MIN, (int)INT32_MIN);
        CHECK(strstr(js, want) != NULL);
    }
    CHECK(strstr(js, "\"label\":\"Hi\"") != NULL);
    for (k = 0; js[k] && strncmp(js + k, "{\"img\":", 7); k++) {
    }
    CHECK(js[k] != 0 && strstr(js + k + 1, "{\"img\":") == NULL);  /* one picture: the mask left out */
    CHECK(strstr(js, "\"src\":") != NULL);
    CHECK(strstr(js, "4294967295") == NULL);    /* no white sheet over it all */
    free(js);
    pd_doc_free(d);

    {   /* in a DOCX: a drawing on the way in, the metafile itself on the way out */
        FILE* f = fopen("build/test_emf.emf", "wb");
        int pass;

        if (f) {
            fwrite(e.p, 1, e.n, f);
            fclose(f);
        }

        d = docx_doc("word/_rels/document.xml.rels",
                     "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.emf\"/>"
                     "</Relationships>",
                     "word/media/image1.emf", "build/test_emf.emf",
                     "word/document.xml",
                     "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
                     "<w:p>" DRAWING("inline", "") "</w:p></w:body></w:document>", NULL);

        for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
            pd_inline o;
            buf_t b = { NULL, 0 };

            CHECK(d != NULL);

            if (!d) {
                break;
            }

            CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
            CHECK(pd_doc_resource(d, o.resource, &mime, &data, &len) == PD_OK &&
                  strcmp(mime, "application/vnd.parade.drawing+json") == 0);
            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK && b.n > 0);
            free(b.p);
        }

        pd_doc_free(d);
    }

    free(e.p);
}

/* the source of the first equation in a document */
static int first_equation(const pd_doc* d, char* out, size_t cap, int32_t* role) {
    pd_block_id p;

    for (p = pd_doc_next_paragraph(d, 0); p; p = pd_doc_next_paragraph(d, p)) {
        const char* t;
        uint32_t n, k;

        pd_doc_para_text(d, p, &t, &n);

        for (k = 0; k + 3 <= n; k++) {
            pd_inline o;

            if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at(p, k), &o) == PD_OK &&
                    o.kind == PD_INLINE_EQUATION) {
                pd_block_info bi;

                snprintf(out, cap, "%.*s", (int)o.source_len, o.source);
                pd_doc_block_info(d, p, &bi);
                *role = bi.role;
                return 1;
            }
        }
    }

    return 0;
}

/* Word's equations (OMML) as LaTeX: a fraction, scripts on one base, a
   radical, a sum with limits, a delimiter, Greek and operators, an upright
   function name; a display equation on its own line. Written back as OMML
   and read again the same. */
static void test_docx_omml(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:m=\"m\"><w:body><w:p><m:oMathPara><m:oMath>"
        "<m:f><m:num><m:r><m:t>a</m:t></m:r></m:num><m:den><m:r><m:t>b</m:t></m:r></m:den></m:f>"
        "<m:r><m:t>+</m:t></m:r>"
        "<m:sSubSup><m:e><m:r><m:t>x</m:t></m:r></m:e><m:sub><m:r><m:t>i</m:t></m:r></m:sub>"
        "<m:sup><m:r><m:t>2</m:t></m:r></m:sup></m:sSubSup>"
        "<m:r><m:t>+</m:t></m:r>"
        "<m:rad><m:radPr><m:degHide m:val=\"1\"/></m:radPr><m:deg/><m:e><m:r><m:t>y</m:t></m:r></m:e></m:rad>"
        "<m:nary><m:naryPr><m:chr m:val=\"\xE2\x88\x91\"/></m:naryPr><m:sub><m:r><m:t>k=1</m:t></m:r></m:sub>"
        "<m:sup><m:r><m:t>n</m:t></m:r></m:sup><m:e><m:r><m:t>\xCE\xB1</m:t></m:r></m:e></m:nary>"
        "<m:d><m:dPr><m:begChr m:val=\"[\"/><m:endChr m:val=\"]\"/></m:dPr><m:e><m:r><m:t>z\xE2\x89\xA4</m:t></m:r>"
        "<m:r><m:rPr><m:sty m:val=\"p\"/></m:rPr><m:t>max</m:t></m:r></m:e></m:d>"
        "</m:oMath></m:oMathPara></w:p>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Inline </w:t></w:r><m:oMath><m:r><m:t>E=m</m:t></m:r>"
        "<m:sSup><m:e><m:r><m:t>c</m:t></m:r></m:e><m:sup><m:r><m:t>2</m:t></m:r></m:sup></m:sSup></m:oMath>"
        "<w:r><w:t xml:space=\"preserve\"> here.</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    char first[512] = "", again[512] = "";
    int32_t role = 0;
    int pass;

    for (pass = 0; pass < 3; pass++, d = docx_again(d)) {
        char tex[512];

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(first_equation(d, tex, sizeof(tex), &role) && role == PD_ROLE_EQUATION);

        if (pass == 0) {
            snprintf(first, sizeof(first), "%s", tex);
            CHECK(strstr(tex, "\\frac{a}{b}") != NULL);
            CHECK(strstr(tex, "{x}_{i}^{2}") != NULL);
            CHECK(strstr(tex, "\\sqrt{y}") != NULL);
            CHECK(strstr(tex, "\\sum_{k=1}^{n} {\\alpha}") != NULL);
            CHECK(strstr(tex, "\\left[z\\le\\mathrm{max}\\right]") != NULL || !printf("  %s\n", tex));
            CHECK(find_para(d, "Inline \xEF\xBF\xBC here.") != 0);
        } else if (pass == 1) {
            snprintf(again, sizeof(again), "%s", tex);
        } else {
            CHECK(strcmp(tex, again) == 0 || !printf("  %s\n  %s\n", again, tex));    /* stable from the first round trip on */
        }
    }

    (void)first;
    pd_doc_free(d);
}

static int has_bytes(const char* t, uint32_t n, const char* want) {
    size_t w = strlen(want), k;

    for (k = 0; k + w <= n; k++) {
        if (!memcmp(t + k, want, w)) {
            return 1;
        }
    }

    return 0;
}

/* Word's fields and cross-references: a page reference to a bookmark
   further on (a table of contents' kind) is the live page of its
   paragraph; a SEQ caption number is a live counter; a REF keeps Word's
   text and links to its bookmark; a link to a place in the document goes
   there. All of it kept through DOCX. */
/* the revision kind and author of the text at an offset */
static int rev_kind_at(const pd_doc* d, pd_block_id p, uint32_t off, const char* author) {
    pd_run runs[64];
    int32_t n = 0, i;
    pd_style_id cs;
    pd_char_props cp;
    pd_revision rv;

    pd_doc_para_runs(d, p, runs, 64, &n);

    for (i = 0; i < n; i++) {
        if (runs[i].start <= off && off < runs[i].end && pd_doc_format_info(d, runs[i].format, &cs, &cp) == PD_OK &&
                (cp.mask & PD_CP_REVISION) && pd_doc_revision_get(d, cp.revision, &rv) == PD_OK &&
                (!author || !strcmp(rv.author, author))) {
            return rv.kind;
        }
    }

    return 0;
}

static void test_docx_review(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:w14=\"w14\"><w:body>"
        "<w:p><w:r><w:t xml:space=\"preserve\">The </w:t></w:r>"
        "<w:del w:id=\"1\" w:author=\"Bob\" w:date=\"2026-01-02T03:04:05Z\"><w:r><w:delText>slow </w:delText></w:r></w:del>"
        "<w:ins w:id=\"2\" w:author=\"Ann\" w:date=\"2026-01-02T03:04:06Z\"><w:r><w:t xml:space=\"preserve\">quick </w:t></w:r></w:ins>"
        "<w:commentRangeStart w:id=\"7\"/><w:r><w:t>fox</w:t></w:r><w:commentRangeEnd w:id=\"7\"/>"
        "<w:r><w:commentReference w:id=\"7\"/></w:r>"
        "<w:commentRangeStart w:id=\"8\"/><w:commentRangeEnd w:id=\"8\"/><w:r><w:commentReference w:id=\"8\"/></w:r>"
        "<w:r><w:t>.</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>Next.</w:t></w:r></w:p>"
        "</w:body></w:document>",
        "word/comments.xml",
        "<w:comments xmlns:w=\"w\" xmlns:w14=\"w14\">"
        "<w:comment w:id=\"7\" w:author=\"Cy\" w:date=\"2026-01-03T00:00:00Z\"><w:p w14:paraId=\"0A000001\">"
        "<w:r><w:annotationRef/></w:r><w:r><w:t>Which fox?</w:t></w:r></w:p><w:p w14:paraId=\"0A000002\">"
        "<w:r><w:t>Really.</w:t></w:r></w:p></w:comment>"
        "<w:comment w:id=\"8\" w:author=\"Ann\"><w:p w14:paraId=\"0A000003\"><w:r><w:t>The red one.</w:t></w:r></w:p>"
        "</w:comment></w:comments>",
        "word/commentsExtended.xml",
        "<w15:commentsEx xmlns:w15=\"w15\"><w15:commentEx w15:paraId=\"0A000002\" w15:done=\"1\"/>"
        "<w15:commentEx w15:paraId=\"0A000003\" w15:paraIdParent=\"0A000002\" w15:done=\"0\"/></w15:commentsEx>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id p0;
        pd_comment c;
        buf_t md = { NULL, 0 };

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        p0 = pd_doc_next_paragraph(d, 0);
        CHECK(text_is(d, p0, "The slow quick fox."));
        CHECK(rev_kind_at(d, p0, 4, "Bob") == PD_REV_DELETE && rev_kind_at(d, p0, 9, "Ann") == PD_REV_INSERT &&
              rev_kind_at(d, p0, 0, NULL) == 0 && rev_kind_at(d, p0, 15, NULL) == 0);
        CHECK(pd_doc_comment_count(d) == 2);
        CHECK(pd_doc_comment_get(d, 1, &c) == PD_OK && !strcmp(c.author, "Cy") && c.text_len == 18 &&
              !memcmp(c.text, "Which fox?\nReally.", 18) && c.resolved && c.range.start.offset == 15 &&
              c.range.end.offset == 18 && !strcmp(c.date, "2026-01-03T00:00:00Z"));
        CHECK(pd_doc_comment_get(d, 2, &c) == PD_OK && c.parent == 1 && !c.resolved && c.text_len == 12);
        /* other formats get the text as if the changes were accepted */
        CHECK(pd_doc_export(d, PD_CONV_MARKDOWN, to_buf, &md) == PD_OK && md.p && strstr((char*)md.p, "The quick fox.") &&
              !strstr((char*)md.p, "slow"));
        free(md.p);
    }

    pd_doc_free(d);
}

static void test_docx_fields(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body>"
        "<w:p><w:hyperlink w:anchor=\"_Toc1\"><w:r><w:t xml:space=\"preserve\">Intro </w:t></w:r></w:hyperlink>"
        "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> PAGEREF _Toc1 \\h </w:instrText></w:r>"
        "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>9</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>"
        "<w:p><w:r><w:br w:type=\"page\"/></w:r><w:bookmarkStart w:id=\"0\" w:name=\"_Toc1\"/><w:r><w:t>Intro</w:t></w:r>"
        "<w:bookmarkEnd w:id=\"0\"/></w:p>"
        "<w:p><w:bookmarkStart w:id=\"1\" w:name=\"_Ref5\"/><w:r><w:t xml:space=\"preserve\">Figure </w:t></w:r>"
        "<w:fldSimple w:instr=\" SEQ Figure \\* ARABIC \"><w:r><w:t>7</w:t></w:r></w:fldSimple><w:bookmarkEnd w:id=\"1\"/></w:p>"
        "<w:p><w:r><w:t xml:space=\"preserve\">See </w:t></w:r><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
        "<w:r><w:instrText> REF _Ref5 \\h </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
        "<w:r><w:t>Figure 1</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r><w:r><w:t>.</w:t></w:r></w:p>"
        "</w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id p0, p1, p2, p3;
        pd_inline o;
        const char* t;
        uint32_t n, k;
        int found_ref = 0, found_seq = 0, links = 0;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        p0 = pd_doc_next_paragraph(d, 0);
        p1 = pd_doc_next_paragraph(d, p0);
        p2 = pd_doc_next_paragraph(d, p1);
        p3 = pd_doc_next_paragraph(d, p2);

        for (pd_doc_para_text(d, p0, &t, &n), k = 0; k + 3 <= n; k++) {
            if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at(p0, k), &o) == PD_OK) {
                found_ref |= o.kind == PD_INLINE_FIELD && o.field == PD_FIELD_REF_PAGE && o.target == p1;
                links += o.kind == PD_INLINE_LINK && o.source_len == 6 && !memcmp(o.source, "#_Toc1", 6);
            }
        }

        CHECK(found_ref && links == 1);     /* the page of the heading's paragraph; the entry links to it */
        CHECK(memchr(t, '9', n) == NULL);   /* Word's old page number not kept */

        for (pd_doc_para_text(d, p2, &t, &n), k = 0; k + 3 <= n; k++) {
            if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at(p2, k), &o) == PD_OK) {
                found_seq |= o.kind == PD_INLINE_FIELD && o.field == PD_FIELD_SEQ && !strcmp(o.name, "Figure");
            }
        }

        CHECK(found_seq);
        links = 0;

        for (pd_doc_para_text(d, p3, &t, &n), k = 0; k + 3 <= n; k++) {
            if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at(p3, k), &o) == PD_OK) {
                links += o.kind == PD_INLINE_LINK && o.source_len == 6 && !memcmp(o.source, "#_Ref5", 6);
            }
        }

        CHECK(links == 1 && has_bytes(t, n, "Figure 1"));     /* Word's text, linked to the caption */
    }

    pd_doc_free(d);
}

/* A font the document carries: obfuscated as Word does (the first 32
   bytes XORed with the key its GUID gives), read back as the original,
   named by family, weight and italic in its resource type. */
static void test_docx_embedded_font(void) {
    static const char* path = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf";
    static const char* key = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
    static const int pos[16] = { 35, 33, 31, 29, 27, 25, 22, 20, 17, 15, 12, 10, 7, 5, 3, 1 };
    FILE* f = fopen(path, "rb");
    unsigned char* font, k[16];
    long n;
    int i;
    pd_doc* d;
    pd_res_id r;
    int found = 0;

    if (!f) {
        printf("  (no %s: skipped)\n", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    font = (unsigned char*)malloc((size_t)n);
    CHECK(fread(font, 1, (size_t)n, f) == (size_t)n);
    fclose(f);

    for (i = 0; i < 16; i++) {
        char h[3] = { key[pos[i]], key[pos[i] + 1], 0 };

        k[i] = (unsigned char)strtoul(h, NULL, 16);
    }

    for (i = 0; i < 16; i++) {      /* obfuscated, as Word stores it */
        font[i] ^= k[i];
        font[i + 16] ^= k[i];
    }

    f = fopen("build/test_font.odttf", "wb");
    fwrite(font, 1, (size_t)n, f);
    fclose(f);

    for (i = 0; i < 16; i++) {
        font[i] ^= k[i];
        font[i + 16] ^= k[i];
    }

    d = docx_doc("word/fontTable.xml",
                 "<w:fonts xmlns:w=\"w\" xmlns:r=\"r\"><w:font w:name=\"Liberation Sans\">"
                 "<w:embedBold r:id=\"rId1\" w:fontKey=\"{01234567-89AB-CDEF-0123-456789ABCDEF}\"/></w:font></w:fonts>",
                 "word/_rels/fontTable.xml.rels",
                 "<Relationships xmlns=\"r\"><Relationship Id=\"rId1\" Type=\"t/font\" Target=\"fonts/font1.odttf\"/></Relationships>",
                 "word/fonts/font1.odttf", "build/test_font.odttf",
                 "word/document.xml", "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>Hi</w:t></w:r></w:p></w:body></w:document>",
                 NULL);
    CHECK(d != NULL);

    for (r = 1; d; r++) {
        const char* mime;
        const void* data;
        size_t len;

        if (pd_doc_resource(d, r, &mime, &data, &len) != PD_OK) {
            break;
        }

        if (!strncmp(mime, "font/ttf", 8)) {
            found = 1;
            CHECK(strstr(mime, "family=\"Liberation Sans\"") && strstr(mime, "weight=700") && strstr(mime, "italic=0"));
            CHECK(len == (size_t)n && !memcmp(data, font, len));    /* the original font again */
        }
    }

    CHECK(found);
    pd_doc_free(d);
    free(font);
}

/* Word's document properties as the document's metadata (YAML), and the
   page: a gutter, mirrored margins, text centred down the page, a
   right-to-left paragraph -- all kept through DOCX */
static void test_docx_page_meta(void) {
    pd_doc* d = docx_doc(
        "docProps/core.xml",
        "<cp:coreProperties xmlns:cp=\"cp\" xmlns:dc=\"dc\"><dc:title>A \"quoted\" title</dc:title>"
        "<dc:creator>Q. Fang</dc:creator><cp:keywords>DOT, MCX</cp:keywords></cp:coreProperties>",
        "word/settings.xml", "<w:settings xmlns:w=\"w\"><w:mirrorMargins/></w:settings>",
        "word/document.xml",
        "<w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:bidi/></w:pPr><w:r><w:t>Title page</w:t></w:r></w:p>"
        "<w:sectPr><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" "
        "w:footer=\"720\" w:gutter=\"720\"/><w:vAlign w:val=\"center\"/></w:sectPr></w:body></w:document>",
        NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        size_t n = 0;
        const char* y;
        pd_section_props sp;
        pd_para_props pp;

        CHECK(d != NULL);

        if (!d) {
            return;
        }

        y = pd_doc_metadata(d, &n);
        CHECK(y && has_bytes(y, (uint32_t)n, "title: \"A \\\"quoted\\\" title\"") && has_bytes(y, (uint32_t)n, "author: \"Q. Fang\"") &&
              has_bytes(y, (uint32_t)n, "keywords: \"DOT, MCX\""));
        CHECK(pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp) == PD_OK);
        CHECK(sp.gutter == PD_PT(36) && sp.mirror_margins == 1 && sp.page_valign == 1);
        pd_doc_para_props(d, pd_doc_next_paragraph(d, 0), &pp);
        CHECK((pp.mask & PD_PP_DIRECTION) && pp.direction == PD_DIR_RTL);
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
    printf("docx contextual spacing\n");
    test_docx_contextual();
    printf("docx paragraph borders\n");
    test_docx_borders();
    printf("docx table styles\n");
    test_docx_table_styles();
    printf("docx list levels\n");
    test_docx_list_levels();
    printf("docx line numbers\n");
    test_docx_line_numbers();
    printf("docx header logo\n");
    test_docx_header_logo();
    test_docx_float_offset_y();
    test_docx_float_from_page();
    test_pptx();
    test_pptx_smartart();
    test_docx_smartart();
    test_docx_turned_text();
    test_docx_rtl();
    test_theme_links();
    test_rtl_html_rtf();
    printf("docx drawings and text boxes\n");
    test_docx_drawings();
    printf("EMF pictures\n");
    test_emf();
    printf("docx equations\n");
    test_docx_omml();
    printf("docx fields and references\n");
    test_docx_fields();
    test_docx_review();
    test_docx_theme_colors();
    test_docx_font_table();
    test_docx_controls();
    test_docx_charts();
    test_docx_east_asian();
    test_docx_shading_edges();
    test_docx_group_textbox_table();
    test_docx_deleted_float();
    test_docx_group_turned_shapes();
    test_docx_canvas_3d();
    test_docx_canvas_kept();
    test_drawing_story_copy();
    test_docx_tracked_objects();
    test_docx_drawing_rebuild();
    test_docx_canvas_fallback();
    test_docx_preset_adjust();
    test_preset_eval();
    printf("docx embedded fonts\n");
    test_docx_embedded_font();
    printf("docx properties and page\n");
    test_docx_page_meta();
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
    pd_font_free(layout_font);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
