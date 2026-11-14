/*
 * Parade import/export tests: every format round-trips a document with
 * headings, character formats, links, lists, quotes, code, a table, a
 * figure and a footnote; clipboard ranges and paste; malformed input.
 */

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_convert.h"
#include "parade_layout.h"
#include "../src/pd_conv.h"
#include "../src/pd_paint.h"

void pd_doc_format_number(int32_t v, int32_t fmt, char* buf, size_t cap);  /* pd_doc_internal.h */

static int failures = 0, checks = 0;

#define CHECK_STR(got, want) do { checks++; if (strcmp((got), (want)) != 0) { failures++; \
        fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); } } while (0)
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
        { PD_CONV_DOCX, "docx" }, { PD_CONV_TEXT, "txt" }, { PD_CONV_JDATA, "jdoc" }, { PD_CONV_PPTX, "pptx" }
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
    if (o.mask & PD_PP_LINE_SPACING) r.line_rule = o.line_rule;
    if (o.mask & PD_PP_LINE_SPACING) r.line_height = o.line_height;
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
    CHECK(pp.align == PD_ALIGN_CENTER && pp.line_rule == PD_LINE_EXACT && pp.line_height == PD_PT(24));

    /* a heading in the theme's heading font; a run that turns the bold off */
    pd_doc_block_info(d, p[4], &bi);
    CHECK(bi.role == PD_ROLE_HEADING && bi.level == 1);
    cp = chars_at(d, p[4], 0);
    CHECK(strcmp(cp.family, "Calibri Light") == 0 && cp.size == PD_PT(16) && cp.weight == 700);
    cp = chars_at(d, p[4], 5);
    CHECK(strcmp(cp.family, "Calibri Light") == 0 && cp.size == PD_PT(10) && cp.weight == 400);
}

/* a document from the parts of a package in format FMT: PRE's NPRE names and texts, then name, text, name, text,
   ..., NULL. A word/media/ (ppt/media/) part names a file whose bytes it holds. */
static pd_doc* parts_doc(int fmt, const char* const* pre, int npre, const char* first, va_list ap) {
    const char* names[16];
    const char* texts[16];
    size_t lens[16];
    char* files[16];
    int n = 0, i;
    const char* nm;
    buf_t z;
    pd_doc* d = NULL;

    for (; n < npre; n++) {
        names[n] = pre[2 * n];
        texts[n] = pre[2 * n + 1];
        lens[n] = strlen(texts[n]);
        files[n] = NULL;
    }

    for (nm = first; nm && n < 16; nm = va_arg(ap, const char*)) {
        names[n] = nm;
        texts[n] = va_arg(ap, const char*);
        lens[n] = strlen(texts[n]);
        files[n] = NULL;

        if (strncmp(nm, "word/media/", 11) == 0 || strncmp(nm, "word/fonts/", 11) == 0 ||
                strncmp(nm, "ppt/media/", 10) == 0) {
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

    z = stored_zip_n(names, texts, lens, n);

    for (i = 0; i < n; i++) {
        free(files[i]);
    }


    if (pd_doc_import(z.p, z.n, fmt, &d) != PD_OK) {
        d = NULL;
    }

    free(z.p);
    return d;
}

/* a document from the parts of a .docx: name, text, name, text, ..., NULL */
static pd_doc* docx_doc(const char* first, ...) {
    va_list ap;
    pd_doc* d;

    va_start(ap, first);
    d = parts_doc(PD_CONV_DOCX, NULL, 0, first, ap);
    va_end(ap);
    return d;
}

/* a presentation of N slides, ppt/slides/slide1.xml.., from the parts given (name, text, ..., NULL); its
   presentation part made here, its slides 10 by 7.5 inches */
static pd_doc* pptx_doc(int nslides, const char* first, ...) {
    static char pres[2048], prels[2048];
    const char* pre[4] = { "ppt/presentation.xml", pres, "ppt/_rels/presentation.xml.rels", prels };
    va_list ap;
    pd_doc* d;
    size_t a, b;
    int k;

    a = (size_t)snprintf(pres, sizeof(pres), "<p:presentation xmlns:p=\"p\" xmlns:r=\"r\"><p:sldIdLst>");
    b = (size_t)snprintf(prels, sizeof(prels), "<Relationships>");

    for (k = 1; k <= nslides; k++) {
        a += (size_t)snprintf(pres + a, sizeof(pres) - a, "<p:sldId id=\"%d\" r:id=\"rIdS%d\"/>", 255 + k, k);
        b += (size_t)snprintf(prels + b, sizeof(prels) - b, "<Relationship Id=\"rIdS%d\" Target=\"slides/slide%d.xml\"/>",
                              k, k);
    }

    snprintf(pres + a, sizeof(pres) - a, "</p:sldIdLst><p:sldSz cx=\"9144000\" cy=\"6858000\"/></p:presentation>");
    snprintf(prels + b, sizeof(prels) - b, "</Relationships>");
    va_start(ap, first);
    d = parts_doc(PD_CONV_PPTX, pre, 2, first, ap);
    va_end(ap);
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

/* One picture kept by a .docx as two media files (Word does this): one resource, both pictures showing it, a
   different picture another; a resource from a file loaded keeps the place it had */
static void test_docx_duplicate_media(void) {
#define PIC(rid) "<w:r><w:drawing><wp:inline><wp:extent cx=\"1270000\" cy=\"635000\"/><a:graphic><a:graphicData>" \
    "<pic:pic><pic:blipFill><a:blip r:embed=\"" rid "\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic>" \
    "</wp:inline></w:drawing></w:r>"
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "<Relationship Id=\"rId6\" Type=\"t/image\" Target=\"media/image2.png\"/>"
        "<Relationship Id=\"rId7\" Type=\"t/image\" Target=\"media/image3.jpg\"/></Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/media/image2.png", "tests/data/rgba.png",
        "word/media/image3.jpg", "tests/data/photo.jpg",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
        "<w:p>" PIC("rId5") "</w:p><w:p>" PIC("rId6") "</w:p><w:p>" PIC("rId7") "</w:p></w:body></w:document>",
        NULL);
#undef PIC
    pd_res_id ids[3] = { 0, 0, 0 };
    pd_block_id b;
    int k = 0, pngs = 0, r;
    const char* m;
    const void* data;
    size_t len;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    for (b = pd_doc_next_paragraph(d, 0); b && k < 3; b = pd_doc_next_paragraph(d, b)) {
        pd_inline o;

        if (pd_doc_inline_at(d, (pd_pos){ b, 0 }, &o) == PD_OK && o.kind == PD_INLINE_IMAGE) {
            ids[k++] = o.resource;
        }
    }

    for (r = 1; pd_doc_resource(d, (pd_res_id)r, &m, &data, &len) == PD_OK; r++) {
        pngs += !strcmp(m, "image/png");
    }

    CHECK(k == 3 && ids[0] != 0 && ids[0] == ids[1] && ids[2] != ids[0]);  /* the same picture: one resource */
    CHECK(pngs == 1);
    pd_doc_free(d);
}

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

    {   /* the presets, and a theme read from a package */
        pd_theme q0, q1;
        buf_t z = { NULL, 0 };
        pd_doc* n = NULL;

        CHECK(pd_theme_preset_count() >= 8 && pd_theme_preset(0, &q0) && !pd_theme_preset(-1, &q0) &&
              !pd_theme_preset(pd_theme_preset_count(), &q0));
        CHECK(pd_theme_preset(1, &q1) && !strcmp(q1.name, "Office 2007") && q1.color[PD_THEME_ACCENT1] == 0xFF4F81BDu &&
              !strcmp(q1.major, "Cambria"));
        CHECK(pd_doc_new(&n) == PD_OK && pd_doc_set_theme(n, &q1) == PD_OK);
        CHECK(pd_doc_export(n, PD_CONV_DOCX, to_buf, &z) == PD_OK && pd_theme_read(z.p, z.n, &q0) == PD_OK &&
              q0.color[PD_THEME_ACCENT1] == 0xFF4F81BDu && !strcmp(q0.minor, "Calibri"));
        CHECK(pd_theme_read("junk", 4, &q0) == PD_ERR_FORMAT);
        free(z.p);
        pd_doc_free(n);
    }

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

/* a template: read as a document, written as a template; its styles, theme and page applied to another document
   (and undone) */
static void test_templates(void) {
    const char* names[3] = { "word/theme/theme1.xml", "word/styles.xml", "word/document.xml" };
    const char* texts[3];
    size_t lens[3];
    pd_doc* tmpl = NULL;
    buf_t z;
    int i;
    pd_doc* d = NULL, *again = NULL;
    buf_t b = { NULL, 0 };
    pd_block_id body, h;
    pd_section_props sp;
    pd_char_props cp;
    pd_theme th;

    texts[0] =
        "<a:theme xmlns:a=\"a\" name=\"Tmpl\"><a:themeElements><a:clrScheme name=\"T\"><a:accent1><a:srgbClr "
        "val=\"AA2200\"/></a:accent1></a:clrScheme><a:fontScheme name=\"T\"><a:majorFont><a:latin typeface=\"Georgia\"/>"
        "</a:majorFont><a:minorFont><a:latin typeface=\"Verdana\"/></a:minorFont></a:fontScheme></a:themeElements>"
        "</a:theme>";
    texts[1] =
        "<w:styles xmlns:w=\"w\"><w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/>"
        "<w:rPr><w:rFonts w:asciiTheme=\"majorHAnsi\" w:hAnsiTheme=\"majorHAnsi\"/><w:color w:val=\"AA2200\" "
        "w:themeColor=\"accent1\"/><w:sz w:val=\"40\"/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Remark\"><w:name w:val=\"Remark\"/><w:rPr><w:i/></w:rPr></w:style>"
        "<w:style w:type=\"table\" w:styleId=\"Fancy\"><w:name w:val=\"Fancy\"/><w:tblStylePr w:type=\"firstRow\">"
        "<w:tcPr><w:shd w:val=\"clear\" w:fill=\"AA2200\" w:themeFill=\"accent1\"/></w:tcPr></w:tblStylePr></w:style>"
        "</w:styles>";
    texts[2] =
        "<w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:pStyle w:val=\"Remark\"/></w:pPr><w:r><w:t>boilerplate</w:t>"
        "</w:r></w:p><w:tbl><w:tblPr><w:tblStyle w:val=\"Fancy\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"1000\"/>"
        "</w:tblGrid><w:tr><w:tc><w:p/></w:tc></w:tr></w:tbl><w:p/><w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
        "<w:pgMar w:top=\"720\" w:right=\"720\" w:bottom=\"720\" w:left=\"720\"/></w:sectPr></w:body></w:document>";

    for (i = 0; i < 3; i++) {
        lens[i] = strlen(texts[i]);
    }

    z = stored_zip_n(names, texts, lens, 3);
    CHECK(pd_doc_import(z.p, z.n, PD_CONV_DOTX, &tmpl) == PD_OK);   /* read as a template: every style it has */
    free(z.p);
    CHECK(tmpl != NULL);

    if (!tmpl) {
        return;
    }

    /* written as a template and read back: a new document of what it has */
    CHECK(pd_doc_export(tmpl, PD_CONV_DOTX, to_buf, &b) == PD_OK);
    CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOTX, &again) == PD_OK && again);
    free(b.p);

    if (again) {
        CHECK(pd_doc_style_find(again, "Remark") && pd_doc_style_find(again, "Fancy"));
        pd_doc_free(again);
    }

    /* another document, a heading in it, given the template's look */
    CHECK(pd_doc_new(&d) == PD_OK);
    body = pd_doc_child(d, pd_doc_root(d), 0);
    h = pd_doc_child(d, body, 0);
    pd_doc_insert_text(d, (pd_pos) { h, 0 }, "Title", 5, PD_FORMAT_INHERIT, NULL);
    CHECK(pd_doc_set_para_style(d, h, pd_doc_style_find(d, "Heading 1")) == PD_OK);
    cp = chars_at(d, h, 0);
    CHECK(cp.color != 0xFFAA2200u && strcmp(cp.family, "Georgia") != 0);
    CHECK(pd_doc_adopt(d, tmpl, PD_ADOPT_STYLES | PD_ADOPT_THEME | PD_ADOPT_PAGE) == PD_OK);
    cp = chars_at(d, h, 0);
    CHECK(cp.color == 0xFFAA2200u && !strcmp(cp.family, "Georgia") && cp.size == PD_PT(20));
    CHECK(pd_doc_style_find(d, "Remark") && pd_doc_style_find(d, "Fancy"));
    CHECK(pd_doc_theme(d, &th) == PD_OK && th.color[PD_THEME_ACCENT1] == 0xFFAA2200u);
    CHECK(pd_doc_section_props(d, body, &sp) == PD_OK && sp.page_width == PD_PT(595.3) && sp.margin_left == PD_PT(36));

    /* one step back: as it was */
    CHECK(pd_doc_undo(d) == PD_OK);
    cp = chars_at(d, h, 0);
    CHECK(cp.color != 0xFFAA2200u && !pd_doc_style_find(d, "Remark"));
    CHECK(pd_doc_section_props(d, body, &sp) == PD_OK && sp.margin_left != PD_PT(36));
    pd_doc_free(d);
    pd_doc_free(tmpl);
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

/* An empty paragraph's mark sized smaller than its style (a table's empty cells at 8 points): written back in its
   w:pPr's w:rPr, so read again it is as tall as it was -- not at the style's size, every row of the table taller */
static void test_docx_empty_mark_size(void) {
    pd_doc* d = docx_doc("word/document.xml",
                         "<w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:rPr><w:sz w:val=\"16\"/></w:rPr></w:pPr></w:p>"
                         "<w:p><w:r><w:t>Text.</w:t></w:r></w:p></w:body></w:document>", NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; pass < 2 && d; pass++) {     /* written, read back, written again */
        buf_t b = { NULL, 0 };
        size_t n = 0;
        char* x;
        pd_doc* e = NULL;

        CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        x = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "word/document.xml", &n) : NULL;
        CHECK(x && has_mem(x, n, "<w:rPr><w:sz w:val=\"16\"/></w:rPr></w:pPr></w:p>"));
        free(x);
        CHECK(b.p && pd_doc_import(b.p, b.n, PD_CONV_DOCX, &e) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = e;
    }

    pd_doc_free(d);
}

/* A canvas Word wrote with its VML fallback beside it: the fallback kept for readers without DrawingML (Word 2007),
   but not the o:gfxdata on its shapes -- the DrawingML of the whole drawing again, pictures and all, in base64,
   for Word 2007 to carry through a save, never shown. Written back without it, the VML otherwise as it was. */
static void test_docx_fallback_gfxdata(void) {
    static const char doc[] =
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\" xmlns:wpc=\"wpc\" "
        "xmlns:wps=\"wps\" xmlns:mc=\"mc\" xmlns:v=\"v\" xmlns:o=\"o\"><w:body><w:p><w:r><mc:AlternateContent>"
        "<mc:Choice Requires=\"wpc\"><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"1270000\"/><a:graphic>"
        "<a:graphicData><wpc:wpc><wpc:bg/><wpc:whole/><wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
        "<a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm><a:prstGeom prst=\"ellipse\"/><a:solidFill><a:srgbClr "
        "val=\"FF0000\"/></a:solidFill></wps:spPr><wps:bodyPr/></wps:wsp></wpc:wpc></a:graphicData></a:graphic>"
        "</wp:inline></w:drawing></mc:Choice><mc:Fallback><w:pict><v:group id=\"Canvas 1\" o:spid=\"_x0000_s1\" "
        "editas=\"canvas\" style=\"width:200pt;height:100pt\" coordsize=\"20000,10000\" "
        "o:gfxdata=\"UEsDBBQABgAIAAAAIQBTHEGFILLERPICTURESBASE64&#xA;MOREBASE64==\"><v:oval id=\"Oval 2\" "
        "style=\"position:absolute;left:0;top:0;width:5000;height:5000\" fillcolor=\"#FF0000\" "
        "o:gfxdata=\"UEsDBBQSMALLONE==\"/></v:group></w:pict></mc:Fallback></mc:AlternateContent></w:r></w:p>"
        "</w:body></w:document>";
    pd_doc* d = docx_doc("word/document.xml", doc, NULL);
    pd_inline o;
    char* js;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
    js = drawing_json(d, o.resource);
    CHECK(js && strstr(js, "\"fallback\":\"<mc:Fallback><w:pict><v:group id=\\\"Canvas 1\\\"") != NULL);
    CHECK(js && strstr(js, "gfxdata") == NULL && strstr(js, "PICTURESBASE64") == NULL);    /* not kept */
    CHECK(js && strstr(js, "coordsize=\\\"20000,10000\\\"><v:oval id=\\\"Oval 2\\\"") != NULL);  /* the rest as it was */
    CHECK(js && strstr(js, "fillcolor=\\\"#FF0000\\\"/></v:group>") != NULL);
    free(js);

    {   /* written back: the fallback, without it */
        buf_t b = { NULL, 0 };
        size_t n = 0;
        char* x;

        CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        x = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "word/document.xml", &n) : NULL;
        CHECK(x && has_mem(x, n, "<mc:Fallback>") && has_mem(x, n, "Oval 2") && !has_mem(x, n, "gfxdata"));
        free(x);
        free(b.p);
    }

    pd_doc_free(d);
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

/* the drawing JSON's item holding NEEDLE among its points: from its opening brace to its closing one */
static int item_with(const char* js, const char* needle, char* out, size_t cap) {
    const char* p = strstr(js, needle), *a, *b;

    if (!p) {
        return 0;
    }

    for (a = p; a > js && strncmp(a, "{\"path\"", 7); a--) {
    }

    b = strchr(p, '}');

    if (!b || (size_t)(b - a) + 1 >= cap) {
        return 0;
    }

    memcpy(out, a, (size_t)(b - a) + 1);
    out[b - a + 1] = '\0';
    return 1;
}

/* whether a drawing JSON item's "path" passes within 4 sp of (x, y) */
static int item_has_pt(const char* item, long x, long y) {
    const char* p = strstr(item, "\"path\":[");
    char* e;

    for (p = p ? p + 8 : NULL; p && *p && *p != ']'; ) {
        long px = strtol(p, &e, 10), py;

        if (*e != ',') {
            return 0;
        }

        py = strtol(e + 1, &e, 10);

        if (labs(px - x) <= 4 && labs(py - y) <= 4) {
            return 1;
        }

        p = *e == ',' ? e + 1 : e;
    }

    return 0;
}

/* A freeform of three paths: a half disc drawn with arcTo from the left end of its diameter over the top, a line
   in a path of its own size (2 by 2) that is not filled, and a filled square that is not outlined. */
static void test_docx_custgeom_paths(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpg=\"wpg\" xmlns:wps=\"wps\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"1270000\" cy=\"1270000\"/><a:graphic><a:graphicData><wpg:wgp>"
        "<wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"1270000\"/><a:chOff x=\"0\" y=\"0\"/>"
        "<a:chExt cx=\"1270000\" cy=\"1270000\"/></a:xfrm></wpg:grpSpPr>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"1270000\"/></a:xfrm><a:custGeom>"
        "<a:pathLst><a:path w=\"1000\" h=\"1000\"><a:moveTo><a:pt x=\"0\" y=\"500\"/></a:moveTo><a:arcTo wR=\"500\" "
        "hR=\"500\" stAng=\"10800000\" swAng=\"10800000\"/><a:close/></a:path>"
        "<a:path w=\"2\" h=\"2\" fill=\"none\"><a:moveTo><a:pt x=\"0\" y=\"2\"/></a:moveTo><a:lnTo><a:pt x=\"2\" y=\"2\"/>"
        "</a:lnTo></a:path>"
        "<a:path w=\"4\" h=\"4\" stroke=\"0\"><a:moveTo><a:pt x=\"1\" y=\"3\"/></a:moveTo><a:lnTo><a:pt x=\"3\" y=\"3\"/>"
        "</a:lnTo><a:lnTo><a:pt x=\"3\" y=\"4\"/></a:lnTo><a:close/></a:path></a:pathLst></a:custGeom>"
        "<a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill><a:ln w=\"12700\"><a:solidFill><a:srgbClr val=\"0000FF\"/>"
        "</a:solidFill></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>"
        "</wpg:wgp></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>",
        NULL);
    pd_inline o;
    char* js;
    char item[4096], want[64];

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    CHECK(pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK);
    js = drawing_json(d, o.resource);
    CHECK(js != NULL);

    if (js) {
        /* the arc passes through the top of the circle, half way across (50 pt, 0), and ends at the right (100 pt,
           50 pt); filled green, outlined blue */
        snprintf(want, sizeof(want), "[0,%d,", (int)PD_PT(50));
        CHECK(item_with(js, want, item, sizeof(item)));
        CHECK(strstr(item, "\"fill\":4278255360") != NULL && strstr(item, "\"line\":4278190335") != NULL);
        CHECK(item_has_pt(item, PD_PT(50), 0) && item_has_pt(item, PD_PT(100), PD_PT(50)));
        CHECK(!item_has_pt(item, PD_PT(50), PD_PT(100)) && strstr(item, "\"closed\":1") != NULL);
        /* the line along the bottom: its own 2-by-2 space stretched over the box, outlined and not filled */
        snprintf(want, sizeof(want), "[0,%d,%d,%d]", (int)PD_PT(100), (int)PD_PT(100), (int)PD_PT(100));
        CHECK(item_with(js, want, item, sizeof(item)));
        CHECK(strstr(item, "\"fill\":0,") != NULL && strstr(item, "\"line\":4278190335") != NULL);
        /* the square in its 4-by-4 space: filled, not outlined */
        snprintf(want, sizeof(want), "[%d,%d,%d,%d", (int)PD_PT(25), (int)PD_PT(75), (int)PD_PT(75), (int)PD_PT(75));
        CHECK(item_with(js, want, item, sizeof(item)));
        CHECK(strstr(item, "\"fill\":4278255360") != NULL && strstr(item, "\"line\":0,") != NULL);
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

/* a docx of one group (2540000 EMU square) holding SHAPES (wps:wsp elements), laid out; its page 0 items */
static pd_draw* group_items(const char* shapes, pd_doc** dp, pd_layout** lp, int32_t* np) {
    char xml[16384];
    pd_draw* it = NULL;

    snprintf(xml, sizeof(xml),
             "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpg=\"wpg\" xmlns:wps=\"wps\"><w:body>"
             "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"2540000\"/><a:graphic><a:graphicData>"
             "<wpg:wgp><wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"2540000\"/>"
             "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"2540000\" cy=\"2540000\"/></a:xfrm></wpg:grpSpPr>%s</wpg:wgp>"
             "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>", shapes);
    *dp = docx_doc("word/document.xml", xml, NULL);
    *lp = NULL;
    *np = 0;

    if (!*dp || !have_layout_font()) {
        return NULL;
    }

    pd_doc_set_font_resolver(*dp, layout_resolver, NULL);

    if (pd_layout_new(*dp, lp) == PD_OK && pd_layout_update(*lp, NULL) == PD_OK &&
            pd_layout_page_items(*lp, 0, NULL, 0, np) == PD_OK &&
            (it = (pd_draw*)malloc(((size_t)*np + 1) * sizeof(pd_draw))) != NULL) {
        pd_layout_page_items(*lp, 0, it, *np, np);
    }

    return it;
}

/* Outline styles: a red line 2 pt wide, dashed (Office's "dash": 4 widths on, 3 off) with round corners, an oval
   at its start and a large stealth arrowhead at its end; a box outlined in a double line 6 pt wide (two lines a
   third of that, a third apart). The dashes are in the display list, Parade's rasterizer leaves the gaps empty,
   and the PDF strokes with a dash array. */
static void test_docx_line_styles(void) {
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    int32_t n = 0, k, line = -1, oval = -1, stealth = -1, dbl[2] = { -1, -1 }, nd = 0;
    pd_draw* it = group_items(
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"635000\"/><a:ext cx=\"1270000\" cy=\"0\"/></a:xfrm>"
                      "<a:prstGeom prst=\"line\"/><a:ln w=\"25400\"><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill>"
                      "<a:prstDash val=\"dash\"/><a:round/><a:headEnd type=\"oval\"/><a:tailEnd type=\"stealth\" w=\"lg\" "
                      "len=\"lg\"/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>"
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"635000\" y=\"1270000\"/><a:ext cx=\"635000\" cy=\"635000\"/>"
                      "</a:xfrm><a:prstGeom prst=\"rect\"/><a:noFill/><a:ln w=\"76200\" cmpd=\"dbl\"><a:solidFill>"
                      "<a:srgbClr val=\"0000FF\"/></a:solidFill></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>", &d, &L, &n);

    if (!it) {
        pd_layout_free(L);
        pd_doc_free(d);
        return;
    }

    for (k = 0; k < n; k++) {
        if (it[k].kind != PD_DRAW_PATH) {
            continue;
        }

        if (it[k].color == 0xFFFF0000u && it[k].line_width == PD_PT(2)) {
            line = k;
        } else if (it[k].fill == 0xFFFF0000u && it[k].npoints == 24) {
            oval = k;
        } else if (it[k].fill == 0xFFFF0000u && it[k].npoints == 4) {
            stealth = k;
        } else if (it[k].color == 0xFF0000FFu && it[k].line_width == PD_PT(2) && nd < 2) {
            dbl[nd++] = k;
        }
    }

    CHECK(line >= 0 && oval >= 0 && stealth >= 0 && nd == 2);

    if (line >= 0 && oval >= 0 && stealth >= 0) {
        const pd_draw* a = &it[line];
        pd_sp x0 = a->points[0], y0 = a->points[1], xe = a->points[2 * a->npoints - 2];
        uint8_t* px;
        int32_t W, H;

        CHECK(a->style && a->style->dash_count == 2 && a->style->dash[0] == PD_PT(8) && a->style->dash[1] == PD_PT(6));
        CHECK(a->style && a->style->join == PD_JOIN_ROUND && a->style->cap == PD_CAP_FLAT);
        /* the oval about the line's start; the stealth's point 100 pt along, the line stopping short of it */
        CHECK(llabs((long long)it[oval].x + it[oval].w / 2 - x0) <= 2 && llabs((long long)it[oval].y + it[oval].h / 2 - y0) <= 2);
        CHECK(llabs((long long)it[stealth].x + it[stealth].w - (x0 + PD_PT(100))) <= 2 && xe < x0 + PD_PT(100) - PD_PT(5));
        /* drawn at 4 pixels a point: the first dash covers 4 pt along, the first gap 11 pt along is empty */
        W = 4 * 40;
        H = 4 * 8;
        px = (uint8_t*)calloc((size_t)W * H, 4);

        if (px) {
            CHECK(pd_draw_render(a, 1, x0, y0 - PD_PT(4), 4 << 16, px, W, H, W * 4) == PD_OK);
            CHECK(px[(16 * W + 16) * 4 + 3] == 255 && px[(16 * W + 16) * 4 + 0] == 255);
            CHECK(px[(16 * W + 44) * 4 + 3] == 0 && px[(16 * W + 60) * 4 + 3] == 255);
            CHECK(px[(8 * W + 16) * 4 + 3] == 0);   /* 2 pt wide: 4 pt off the line is clear */
            free(px);
        }
    }

    if (nd == 2) {  /* the double line's two lines 4 pt apart (each a third of 6 pt wide, a third between) */
        CHECK(llabs((long long)it[dbl[0]].w - it[dbl[1]].w) == PD_PT(4) || llabs((long long)it[dbl[0]].x - it[dbl[1]].x) == PD_PT(4));
    }

    {   /* the PDF strokes the line with its dashes and round corners */
        pd_pdf_options opt;
        buf_t b;

        memset(&b, 0, sizeof(b));
        pd_pdf_options_init(&opt);
        opt.compress = 0;
        CHECK(pd_layout_write_pdf(L, &opt, to_buf, &b) == PD_OK);
        CHECK(b.p && strstr(b.p, "w 0 J 1 j 8 M [8 6 ] 0 d") != NULL);
        free(b.p);
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* Curves kept as curves: a freeform's arc (two quarter-turn cubics) and an ellipse (four) reach the display list as
   cubic Beziers beside their chords; the PDF draws them with c, and the rasterizer fills from the curve. */
static void test_docx_curves(void) {
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    int32_t n = 0, k, disc = -1, ell = -1;
    pd_draw* it = group_items(
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"1270000\"/></a:xfrm>"
                      "<a:custGeom><a:pathLst><a:path w=\"1000\" h=\"1000\"><a:moveTo><a:pt x=\"0\" y=\"500\"/></a:moveTo>"
                      "<a:arcTo wR=\"500\" hR=\"500\" stAng=\"10800000\" swAng=\"10800000\"/><a:close/></a:path></a:pathLst>"
                      "</a:custGeom><a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill><a:ln><a:noFill/></a:ln></wps:spPr>"
                      "<wps:bodyPr/></wps:wsp>"
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"1270000\"/><a:ext cx=\"635000\" cy=\"317500\"/>"
                      "</a:xfrm><a:prstGeom prst=\"ellipse\"/><a:solidFill><a:srgbClr val=\"0000FF\"/></a:solidFill>"
                      "<a:ln><a:noFill/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>", &d, &L, &n);

    if (!it) {
        pd_layout_free(L);
        pd_doc_free(d);
        return;
    }

    for (k = 0; k < n; k++) {
        disc = it[k].kind == PD_DRAW_PATH && it[k].fill == 0xFF00FF00u ? k : disc;
        ell = it[k].kind == PD_DRAW_PATH && it[k].fill == 0xFF0000FFu ? k : ell;
    }

    CHECK(disc >= 0 && ell >= 0);

    if (disc >= 0 && ell >= 0) {
        const pd_draw* a = &it[disc];
        int32_t cubics = 0;

        for (k = 0; a->curve && k < a->ncurve; k++) {
            cubics += a->curve[2 * k] == PD_PATH_CUBIC;
        }

        /* from the diameter's left end, two cubics, the last ending at its right end 100 pt along */
        CHECK(a->curve && cubics == 2 && a->ncurve == 9);
        CHECK(a->curve && a->curve[0] == a->points[0] && a->curve[1] == a->points[1] &&
              llabs((long long)a->curve[16] - (a->curve[0] + PD_PT(100))) <= 2 && llabs((long long)a->curve[17] - a->curve[1]) <= 2);
        CHECK(it[ell].curve && it[ell].ncurve == 17 && it[ell].npoints == 64);

        if (a->curve) {     /* the rasterizer fills from the curve: given only a stub of chords, the arc's top is there */
            pd_draw b = *a;
            pd_sp stub[4] = { a->points[0], a->points[1], a->points[0] + 1, a->points[1] };
            int32_t W = 4 * 110, H = 4 * 60;
            uint8_t* px = (uint8_t*)calloc((size_t)W * H, 4);

            b.points = stub;
            b.npoints = 2;

            if (px) {
                CHECK(pd_draw_render(&b, 1, a->points[0] - PD_PT(5), a->points[1] - PD_PT(55), 4 << 16, px, W, H, W * 4) ==
                      PD_OK);
                CHECK(px[((4 * 15) * W + 4 * 55) * 4 + 3] == 255 && px[((4 * 15) * W + 4 * 55) * 4 + 1] == 255);
                CHECK(px[((4 * 15) * W + 4 * 10) * 4 + 3] == 0);    /* outside the circle, inside its box */
                free(px);
            }
        }
    }

    {   /* the PDF draws the arc and the ellipse with six curveto operators */
        pd_pdf_options opt;
        buf_t b;
        const char* p;
        int cs = 0;

        memset(&b, 0, sizeof(b));
        pd_pdf_options_init(&opt);
        opt.compress = 0;
        CHECK(pd_layout_write_pdf(L, &opt, to_buf, &b) == PD_OK);

        for (p = b.p; p && (p = strstr(p, " c ")) != NULL; p++) {
            cs++;
        }

        CHECK(cs == 6);
        free(b.p);
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* the text drawn in a region (-1: any) on every page of a fresh layout, as UTF-8 in display order; the
   number of pages, -1 when it cannot be laid out */
static int layout_text(pd_doc* d, int32_t region, char* out, size_t cap) {
    pd_layout* L = NULL;
    int32_t pg, k, n, np;
    size_t o = 0;

    out[0] = '\0';
    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) != PD_OK || pd_layout_update(L, NULL) != PD_OK) {
        pd_layout_free(L);
        return -1;
    }

    np = pd_layout_page_count(L);

    for (pg = 0; pg < np; pg++) {
        pd_draw* it;

        pd_layout_page_items(L, pg, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, pg, it, n, &n);

        for (k = 0; k < n; k++) {
            uint32_t c = it[k].text;

            if (it[k].kind != PD_DRAW_GLYPH || !c || (region >= 0 && it[k].region != region) || o + 5 > cap) {
                continue;
            }

            if (c < 0x80) {
                out[o++] = (char)c;
            } else if (c < 0x800) {
                out[o++] = (char)(0xC0 | (c >> 6));
                out[o++] = (char)(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                out[o++] = (char)(0xE0 | (c >> 12));
                out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            } else {
                out[o++] = (char)(0xF0 | (c >> 18));
                out[o++] = (char)(0x80 | ((c >> 12) & 0x3F));
                out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }

        free(it);
    }

    out[o] = '\0';
    pd_layout_free(L);
    return np;
}

/* Gradients as Office has them: three stops left to right (red, green half way, blue), a rectangular one out from
   the middle, and a circle fading from opaque blue to clear. The display list carries the stops and where they run;
   the rasterizer colours each pixel by them; the PDF uses shadings, a stitching function for three stops and a soft
   mask for the fading one, not bands. */
static void test_docx_gradients(void) {
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    int32_t n = 0, k, lin = -1, rect = -1, fade = -1;
    pd_draw* it = group_items(
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
                      "<a:prstGeom prst=\"rect\"/><a:gradFill><a:gsLst><a:gs pos=\"0\"><a:srgbClr val=\"FF0000\"/></a:gs>"
                      "<a:gs pos=\"100000\"><a:srgbClr val=\"0000FF\"/></a:gs><a:gs pos=\"50000\"><a:srgbClr val=\"00FF00\"/>"
                      "</a:gs></a:gsLst><a:lin ang=\"0\" scaled=\"0\"/></a:gradFill><a:ln><a:noFill/></a:ln></wps:spPr>"
                      "<wps:bodyPr/></wps:wsp>"
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"1270000\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
                      "<a:prstGeom prst=\"rect\"/><a:gradFill><a:gsLst><a:gs pos=\"0\"><a:srgbClr val=\"FFFF00\"/></a:gs>"
                      "<a:gs pos=\"100000\"><a:srgbClr val=\"000000\"/></a:gs></a:gsLst><a:path path=\"rect\"><a:fillToRect "
                      "l=\"50000\" t=\"50000\" r=\"50000\" b=\"50000\"/></a:path></a:gradFill><a:ln><a:noFill/></a:ln>"
                      "</wps:spPr><wps:bodyPr/></wps:wsp>"
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"1270000\"/><a:ext cx=\"635000\" cy=\"635000\"/>"
                      "</a:xfrm><a:prstGeom prst=\"ellipse\"/><a:gradFill><a:gsLst><a:gs pos=\"0\"><a:srgbClr val=\"0000FF\"/>"
                      "</a:gs><a:gs pos=\"100000\"><a:srgbClr val=\"0000FF\"><a:alpha val=\"0\"/></a:srgbClr></a:gs></a:gsLst>"
                      "<a:path path=\"circle\"><a:fillToRect l=\"50000\" t=\"50000\" r=\"50000\" b=\"50000\"/></a:path>"
                      "</a:gradFill><a:ln><a:noFill/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>", &d, &L, &n);

    if (!it) {
        pd_layout_free(L);
        pd_doc_free(d);
        return;
    }

    for (k = 0; k < n; k++) {
        const pd_path_style* st = it[k].kind == PD_DRAW_PATH ? it[k].style : NULL;

        lin = st && st->grad_kind == PD_GRAD_LINEAR ? k : lin;
        rect = st && st->grad_kind == PD_GRAD_RECT ? k : rect;
        fade = st && st->grad_kind == PD_GRAD_CIRCLE ? k : fade;
    }

    CHECK(lin >= 0 && rect >= 0 && fade >= 0);

    if (lin >= 0) {
        const pd_draw* a = &it[lin];
        const pd_path_style* st = a->style;
        int32_t W = 100, H = 10, x;
        uint8_t* px = (uint8_t*)calloc((size_t)W * H, 4);

        /* the stops in order, the middle one green; the gradient from the box's left edge to its right */
        CHECK(st->nstops == 3 && st->stop_pos[1] == 50000 && st->stop_color[1] == 0xFF00FF00u &&
              st->stop_color[2] == 0xFF0000FFu);
        CHECK(st->grad_x0 == a->x && st->grad_x1 == a->x + PD_PT(100) && st->grad_y0 == st->grad_y1);

        if (px) {   /* a pixel a point: red at the left, green in the middle, blue at the right, mixed between */
            CHECK(pd_draw_render(a, 1, a->x, a->y + PD_PT(20), 1 << 16, px, W, H, W * 4) == PD_OK);
            x = 1;
            CHECK(px[4 * x] > 240 && px[4 * x + 1] < 15 && px[4 * x + 2] == 0);
            x = 50;
            CHECK(px[4 * x] < 10 && px[4 * x + 1] > 245 && px[4 * x + 2] < 10);
            x = 98;
            CHECK(px[4 * x] == 0 && px[4 * x + 1] < 15 && px[4 * x + 2] > 240);
            x = 25;
            CHECK(px[4 * x] > 110 && px[4 * x] < 145 && px[4 * x + 1] > 110 && px[4 * x + 1] < 145);
            free(px);
        }
    }

    if (rect >= 0 && fade >= 0) {
        const pd_path_style* st = it[rect].style;

        /* the rectangle out from a point in the middle to the box's edges */
        CHECK(st->grad_x0 == st->grad_x1 && st->grad_x0 == it[rect].x + PD_PT(25) && st->grad_bx0 == it[rect].x &&
              st->grad_by1 == it[rect].y + PD_PT(50));
        CHECK(it[fade].style->stop_color[1] == 0x010000FFu);    /* (alpha 1 at the least: 0 is opaque) */
    }

    {
        pd_pdf_options opt;
        buf_t b;

        memset(&b, 0, sizeof(b));
        pd_pdf_options_init(&opt);
        opt.compress = 0;
        CHECK(pd_layout_write_pdf(L, &opt, to_buf, &b) == PD_OK);
        CHECK(b.p && strstr(b.p, "/ShadingType 2") && strstr(b.p, "/ShadingType 3"));
        CHECK(b.p && strstr(b.p, "/FunctionType 3 /Domain [0 1] /Functions [") && strstr(b.p, "/Bounds [0.50000 ]"));
        CHECK(b.p && strstr(b.p, "/SMask << /Type /Mask /S /Luminosity") && strstr(b.p, "/CS /DeviceGray"));
        free(b.p);
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* A pattern fill (Office's dark downward diagonal, red on white): its 8-by-8 bitmap reaches the display list, the
   rasterizer tiles it cell by cell from the page's corner, and the PDF fills with a tiling pattern. */
static void test_docx_pattern_fill(void) {
    static const uint8_t dk[8] = { 0xCC, 0x66, 0x33, 0x99, 0xCC, 0x66, 0x33, 0x99 };
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    int32_t n = 0, k, pat = -1;
    pd_draw* it = group_items(
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
                      "<a:prstGeom prst=\"rect\"/><a:pattFill prst=\"dkDnDiag\"><a:fgClr><a:srgbClr val=\"FF0000\"/></a:fgClr>"
                      "<a:bgClr><a:srgbClr val=\"FFFFFF\"/></a:bgClr></a:pattFill><a:ln><a:noFill/></a:ln></wps:spPr>"
                      "<wps:bodyPr/></wps:wsp>", &d, &L, &n);

    if (!it) {
        pd_layout_free(L);
        pd_doc_free(d);
        return;
    }

    for (k = 0; k < n; k++) {
        pat = it[k].kind == PD_DRAW_PATH && it[k].style && it[k].style->pattern ? k : pat;
    }

    CHECK(pat >= 0);

    if (pat >= 0) {
        const pd_draw* a = &it[pat];
        pd_sp tile = 8 * PD_PATTERN_CELL, x0 = a->x / tile * tile + tile, y0 = a->y / tile * tile + tile;
        int32_t W = 48, H = 48, cx, cy, bad = 0;
        uint8_t* px = (uint8_t*)calloc((size_t)W * H, 4);

        CHECK(!memcmp(a->style->pattern_bits, dk, 8) && a->style->pattern_fg == 0xFFFF0000u &&
              a->style->pattern_bg == 0xFFFFFFFFu);

        if (px) {   /* 4 pixels a point, a cell 3 pixels: two tiles, from a tile's corner inside the box */
            CHECK(pd_draw_render(a, 1, x0, y0, 4 << 16, px, W, H, W * 4) == PD_OK);

            for (cy = 0; cy < 16; cy++) {
                for (cx = 0; cx < 16; cx++) {
                    const uint8_t* q = px + ((size_t)(3 * cy + 1) * W + (size_t)(3 * cx + 1)) * 4;
                    int on = (dk[cy & 7] >> (7 - (cx & 7))) & 1;

                    bad += !(q[0] == 255 && q[3] == 255 && (on ? q[1] == 0 : q[1] == 255));
                }
            }

            CHECK(bad == 0);
            free(px);
        }
    }

    {
        pd_pdf_options opt;
        buf_t b;

        memset(&b, 0, sizeof(b));
        pd_pdf_options_init(&opt);
        opt.compress = 0;
        CHECK(pd_layout_write_pdf(L, &opt, to_buf, &b) == PD_OK);
        CHECK(b.p && strstr(b.p, "/PatternType 1 /PaintType 1") && strstr(b.p, "/Pattern cs /Pt"));
        free(b.p);
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* Shapes filled with a picture: an ellipse with the picture stretched over it and cut to its outline, and a box
   tiled with the picture at half its size (64 by 40 pixels: 24 pt by 15 pt) from its top left corner. The display
   list has the pictures, each shown only inside its shape, the outlines over them; the PDF draws them clipped. */
static void test_docx_picture_fill(void) {
    pd_doc* d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wpg=\"wpg\" xmlns:wps=\"wps\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"2540000\" cy=\"2540000\"/><a:graphic><a:graphicData>"
        "<wpg:wgp><wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"2540000\"/>"
        "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"2540000\" cy=\"2540000\"/></a:xfrm></wpg:grpSpPr>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"635000\"/></a:xfrm>"
        "<a:prstGeom prst=\"ellipse\"/><a:blipFill><a:blip r:embed=\"rId5\"/><a:stretch><a:fillRect/></a:stretch>"
        "</a:blipFill><a:ln w=\"12700\"><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill></a:ln></wps:spPr>"
        "<wps:bodyPr/></wps:wsp>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"1270000\"/><a:ext cx=\"1270000\" cy=\"762000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/><a:blipFill><a:blip r:embed=\"rId5\"/><a:tile tx=\"0\" ty=\"0\" sx=\"50000\" "
        "sy=\"50000\" algn=\"tl\"/></a:blipFill><a:ln><a:noFill/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>"
        "</wpg:wgp></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p></w:body></w:document>", NULL);
    pd_layout* L = NULL;
    pd_draw* it = NULL;
    int32_t n = 0, k, stretched = 0, tiles = 0, outline = -1, first = -1;

    if (!d || !have_layout_font()) {
        pd_doc_free(d);
        return;
    }

    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK &&
            pd_layout_page_items(L, 0, NULL, 0, &n) == PD_OK &&
            (it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw))) != NULL) {
        pd_layout_page_items(L, 0, it, n, &n);
    }

    for (k = 0; it && k < n; k++) {
        if (it[k].kind == PD_DRAW_IMAGE && it[k].resource && it[k].clip_npoints == 64) {
            stretched++;
            first = first < 0 ? k : first;
            CHECK(it[k].w == PD_PT(100) && it[k].h == PD_PT(50) && it[k].clip_w == PD_PT(100));
        } else if (it[k].kind == PD_DRAW_IMAGE && it[k].resource && it[k].clip_npoints == 4) {
            tiles++;
            CHECK(it[k].clip_w == PD_PT(24) && it[k].clip_h == PD_PT(15));
        } else if (it[k].kind == PD_DRAW_PATH && it[k].color == 0xFFFF0000u) {
            outline = k;
            CHECK(it[k].fill == 0);     /* the picture, not a colour, inside */
        }
    }

    /* the picture once in the ellipse; tiles 5 across (100 pt) and 4 down (60 pt); the outline drawn after */
    CHECK(stretched == 1 && tiles == 20 && outline > first && first >= 0);

    if (L) {
        pd_pdf_options opt;
        buf_t b;
        const char* p;
        int dos = 0;

        memset(&b, 0, sizeof(b));
        pd_pdf_options_init(&opt);
        opt.compress = 0;
        CHECK(pd_layout_write_pdf(L, &opt, to_buf, &b) == PD_OK);

        for (p = b.p; p && (p = strstr(p, "W n ")) != NULL; p++) {  /* each picture cut to its shape */
            dos++;
        }

        CHECK(dos >= 21 && b.p && strstr(b.p, "/Im0 Do"));
        free(b.p);
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* the alpha (and its pixel) of an item drawn alone at a pixel a point, at page point (x, y) */
static int alpha_at(const pd_draw* a, pd_sp x, pd_sp y, uint8_t* rgb) {
    uint8_t px[4] = { 0, 0, 0, 0 };

    if (pd_draw_render(a, 1, x, y, 1 << 16, px, 1, 1, 4) != PD_OK) {
        return -1;
    }

    if (rgb) {
        memcpy(rgb, px, 3);
    }

    return px[3];
}

/* Effects (50 pt white boxes unless said): a black shadow blurred 6 pt cast 6 pt right; a gold glow 6 pt round an
   ellipse; a soft edge 6 pt deep; a black inner shadow cast down and right; a reflection under the box at full
   opacity fading to none half the box's height down. The rasterizer draws each as pixels, and the PDF places them
   as pictures with soft masks. */
static void test_docx_shape_effects(void) {
    const char* sp = "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"%d\" y=\"%d\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
                     "<a:prstGeom prst=\"%s\"/><a:solidFill><a:srgbClr val=\"%s\"/></a:solidFill><a:ln><a:noFill/></a:ln>"
                     "<a:effectLst>%s</a:effectLst></wps:spPr><wps:bodyPr/></wps:wsp>";
    char shapes[8192];
    size_t sl = 0;
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    int32_t n = 0, k, sh = -1, gl = -1, so = -1, in = -1, re = -1, rb = -1, bv = -1;
    pd_draw* it;
    uint8_t rgb[3];

    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, sp, 0, 0, "rect", "FFFFFF",
                           "<a:outerShdw blurRad=\"76200\" dist=\"76200\" dir=\"0\"><a:srgbClr val=\"000000\"/></a:outerShdw>");
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, sp, 1016000, 0, "ellipse", "0000FF",
                           "<a:glow rad=\"76200\"><a:srgbClr val=\"FFC000\"/></a:glow>");
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, sp, 0, 1016000, "rect", "00FF00", "<a:softEdge rad=\"76200\"/>");
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, sp, 1016000, 1016000, "rect", "FFFFFF",
                           "<a:innerShdw blurRad=\"25400\" dist=\"76200\" dir=\"2700000\"><a:srgbClr val=\"000000\"/></a:innerShdw>");
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, sp, 1905000, 0, "rect", "FF0000",
                           "<a:reflection stA=\"100000\" endA=\"0\" endPos=\"50000\" dist=\"0\"/>");
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, sp, 1905000, 1016000, "rect", "FF0000",
                           "<a:reflection blurRad=\"76200\" stA=\"100000\" endA=\"0\" endPos=\"50000\" dist=\"0\"/>");
    snprintf(shapes + sl, sizeof(shapes) - sl, sp, 0, 1905000, "rect", "808080",
             "</a:effectLst><a:scene3d><a:camera prst=\"orthographicFront\"/><a:lightRig rig=\"threePt\" dir=\"tl\"/>"
             "</a:scene3d><a:sp3d><a:bevelT w=\"76200\" h=\"25400\"/></a:sp3d><a:effectLst>");
    it = group_items(shapes, &d, &L, &n);

    if (!it) {
        pd_layout_free(L);
        pd_doc_free(d);
        return;
    }

    for (k = 0; k < n; k++) {
        const pd_path_style* st = it[k].kind == PD_DRAW_PATH ? it[k].style : NULL;

        sh = st && st->shadow_color ? k : sh;
        gl = st && st->glow_color ? k : gl;
        so = st && st->soft_edge ? k : so;
        in = st && st->inner_color ? k : in;
        re = st && st->refl_alpha0 && !st->refl_blur ? k : re;
        rb = st && st->refl_alpha0 && st->refl_blur ? k : rb;
        bv = st && st->bevel_width ? k : bv;
    }

    CHECK(sh >= 0 && gl >= 0 && so >= 0 && in >= 0 && re >= 0);

    if (sh >= 0 && gl >= 0 && so >= 0 && in >= 0 && re >= 0) {
        const pd_draw* a = &it[sh];
        pd_sp my = a->y + PD_PT(25), r = a->x + a->w;
        int v;

        CHECK(a->style->shadow_blur == PD_PT(6) && a->style->shadow_dx == PD_PT(6) && a->style->shadow_dy == 0);
        /* blurred: part dark just past the box's right edge, clear well past the shadow's own */
        v = alpha_at(a, r + PD_PT(5), my, rgb);
        CHECK(v > 40 && v < 230 && rgb[0] == 0);
        CHECK(alpha_at(a, r + PD_PT(20), my, NULL) == 0 && alpha_at(a, a->x - PD_PT(4), my, NULL) == 0);
        /* the glow gold 3 pt outside the ellipse's right */
        v = alpha_at(&it[gl], it[gl].x + it[gl].w + PD_PT(3), it[gl].y + PD_PT(25), rgb);
        CHECK(v > 60 && rgb[0] > rgb[2] && rgb[1] > rgb[2]);
        /* the soft edge: see-through 1 pt inside, solid in the middle */
        CHECK(alpha_at(&it[so], it[so].x + PD_PT(1), it[so].y + PD_PT(25), NULL) < 128);
        CHECK(alpha_at(&it[so], it[so].x + PD_PT(25), it[so].y + PD_PT(25), NULL) == 255);
        /* the inner shadow dark along the top left inside, the bottom right white */
        alpha_at(&it[in], it[in].x + PD_PT(2), it[in].y + PD_PT(25), rgb);
        CHECK(rgb[0] < 100);
        alpha_at(&it[in], it[in].x + PD_PT(48), it[in].y + PD_PT(25), rgb);
        CHECK(rgb[0] == 255);
        /* the reflection: red just under the box, fading, gone past half its height */
        v = alpha_at(&it[re], it[re].x + PD_PT(25), it[re].y + it[re].h + PD_PT(2), rgb);
        CHECK(v > 180 && rgb[0] > 180);
        CHECK(alpha_at(&it[re], it[re].x + PD_PT(25), it[re].y + it[re].h + PD_PT(15), NULL) < v);
        CHECK(alpha_at(&it[re], it[re].x + PD_PT(25), it[re].y + it[re].h + PD_PT(30), NULL) == 0);
        /* blurred 6 pt: it spills past the box's side, where the sharp one leaves nothing */
        CHECK(alpha_at(&it[re], it[re].x + it[re].w + PD_PT(2), it[re].y + it[re].h + PD_PT(4), NULL) == 0);
        CHECK(rb >= 0 && alpha_at(&it[rb], it[rb].x + it[rb].w + PD_PT(2), it[rb].y + it[rb].h + PD_PT(4), NULL) > 0);
        /* a bevel lit from the top left: its top and left edges lighter than the grey, the bottom and right darker,
           the middle the grey itself */
        CHECK(bv >= 0);

        if (bv >= 0) {
            uint8_t top[3], left[3], bottom[3], right[3], mid[3];
            const pd_draw* b = &it[bv];

            CHECK(b->style->bevel_width == PD_PT(6) && b->style->bevel_light == 225 * 60000);
            alpha_at(b, b->x + PD_PT(25), b->y + PD_PT(2), top);
            alpha_at(b, b->x + PD_PT(2), b->y + PD_PT(25), left);
            alpha_at(b, b->x + PD_PT(25), b->y + b->h - PD_PT(2), bottom);
            alpha_at(b, b->x + b->w - PD_PT(2), b->y + PD_PT(25), right);
            alpha_at(b, b->x + PD_PT(25), b->y + PD_PT(25), mid);
            CHECK(top[0] > mid[0] + 20 && left[0] > mid[0] + 20);
            CHECK(bottom[0] + 20 < mid[0] && right[0] + 20 < mid[0]);
            CHECK(mid[0] >= 0x7C && mid[0] <= 0x84);
        }
    }

    {
        pd_pdf_options opt;
        buf_t b;
        int pics = 0;

        memset(&b, 0, sizeof(b));
        pd_pdf_options_init(&opt);
        opt.compress = 0;
        CHECK(pd_layout_write_pdf(L, &opt, to_buf, &b) == PD_OK);

        for (k = 0; b.p && (size_t)k + 27 <= b.n; k++) {     /* (the pictures' bytes hold zeros: not strstr) */
            pics += !memcmp(b.p + k, "/BitsPerComponent 8 /SMask ", 27);
        }

        CHECK(pics == 7);   /* shadow, glow, the softened fill, the inner shadow, the two reflections, the bevel */
        free(b.p);
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* Text in shapes: a box in two columns 10 pt apart holding six one-line paragraphs too tall for one column; a box
   shrunk to half (normAutofit fontScale); WordArt waving its word (each glyph a path through the warp); and East
   Asian vertical text, the CJK character standing upright among Latin letters turned with the line. */
static void test_docx_text_in_shapes(void) {
    const char* box = "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"%d\" y=\"%d\"/><a:ext cx=\"%d\" cy=\"%d\"/></a:xfrm>"
                      "<a:prstGeom prst=\"rect\"/><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr><wps:txbx><w:txbxContent>"
                      "%s</w:txbxContent></wps:txbx><wps:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" %s>%s"
                      "</wps:bodyPr></wps:wsp>";
    const char* para = "<w:p><w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr><w:t>%s</w:t></w:r></w:p>";
    char shapes[16384], six[2048], one[256];
    size_t sl = 0, xl = 0;
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    int32_t n = 0, k, right = 0, left = 0, halves = 0, wavy = 0, waved = 0, up = 0, turned = 0;
    pd_draw* it;
    const char* words[6] = { "Qa", "Qb", "Qc", "Qd", "Qe", "Qf" };

    for (k = 0; k < 6; k++) {
        xl += (size_t)snprintf(six + xl, sizeof(six) - xl, para, words[k]);
    }

    /* 100 pt wide (two 45 pt columns), 40 pt high: three 10 pt lines a column */
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, box, 0, 0, 1270000, 508000, six, "numCol=\"2\" spcCol=\"127000\"", "");
    snprintf(one, sizeof(one), para, "Zz");
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, box, 0, 1270000, 1270000, 508000, one, "",
                           "<a:normAutofit fontScale=\"50000\"/>");
    snprintf(one, sizeof(one), para, "Wavy");
    sl += (size_t)snprintf(shapes + sl, sizeof(shapes) - sl, box, 1270000, 0, 1270000, 508000, one, "wrap=\"none\"",
                           "<a:prstTxWarp prst=\"textWave1\"><a:avLst/></a:prstTxWarp>");
    snprintf(one, sizeof(one), para, "Y\xE4\xB8\xADY");
    snprintf(shapes + sl, sizeof(shapes) - sl, box, 1270000, 1270000, 508000, 1270000, one, "vert=\"eaVert\"", "");
    it = group_items(shapes, &d, &L, &n);

    if (!it) {
        pd_layout_free(L);
        pd_doc_free(d);
        return;
    }

    {   /* the box's left edge: the leftmost Q */
        pd_sp x0 = INT32_MAX;

        for (k = 0; k < n; k++) {
            if (it[k].kind == PD_DRAW_GLYPH && it[k].text == 'Q') {
                x0 = it[k].x < x0 ? it[k].x : x0;
            }
        }

        for (k = 0; k < n; k++) {
            if (it[k].kind == PD_DRAW_GLYPH && it[k].text == 'Q') {
                right += it[k].x >= x0 + PD_PT(55);
                left += it[k].x < x0 + PD_PT(45);
            }

            halves += it[k].kind == PD_DRAW_GLYPH && (it[k].text == 'Z' || it[k].text == 'z') && it[k].size == PD_PT(5);
            wavy += it[k].kind == PD_DRAW_GLYPH && (it[k].text == 'W' || it[k].text == 'v');
            waved += it[k].kind == PD_DRAW_PATH && it[k].region == 5 && (it[k].text == 'W' || it[k].text == 'v');
            up += it[k].kind == PD_DRAW_GLYPH && it[k].text == 0x4E2D && it[k].rotation == 0;
            turned += it[k].kind == PD_DRAW_GLYPH && it[k].text == 'Y' && it[k].rotation == 5400000;
        }
    }

    CHECK(left == 3 && right == 3);     /* three lines down the first column, three in the second */
    CHECK(halves == 2);                 /* 10 pt text at half */
    CHECK(wavy == 0 && waved == 2);     /* the warped word's glyphs paths, none left as glyphs */
    CHECK(up == 1 && turned == 2);

    {   /* the shrunk box: the caret drawn at its 'z' as shrunk, and a click there finds the 'z' */
        pd_block_id zp = story_para_with(d, "Zz");
        pd_sp gx = -1, gy = 0, cx = 0, base = 0, asc, desc;
        int32_t pg;
        pd_pos hit;

        for (k = 0; k < n; k++) {
            if (it[k].kind == PD_DRAW_GLYPH && it[k].text == 'z') {
                gx = it[k].x;
                gy = it[k].y;
            }
        }

        CHECK(zp != 0 && gx >= 0);

        if (zp && gx >= 0) {
            CHECK(pd_layout_caret(L, at(zp, 1), &pg, &cx, &base, &asc, &desc) == PD_OK);
            CHECK(labs((long)(cx - gx)) <= PD_PT(0.5) && labs((long)(base - gy)) <= PD_PT(0.5));
            CHECK(pd_layout_hit_test(L, 0, gx + PD_PT(1), gy - PD_PT(1), &hit) == PD_OK && hit.block == zp &&
                  hit.offset == 1);
        }
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* SVG: a page with a dashed curve, a three-stop gradient and a pattern as SVG's own -- a dash array, a cubic, a
   linear gradient with its stops, a pattern -- and the drawing alone the same; HTML shows the drawing as an SVG
   picture and reads the drawing itself back. */
static void test_svg_out(void) {
    pd_doc* d = NULL, *back = NULL;
    pd_layout* L = NULL;
    int32_t n = 0;
    pd_draw* it = group_items(
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"1270000\"/></a:xfrm>"
                      "<a:prstGeom prst=\"ellipse\"/><a:noFill/><a:ln w=\"25400\"><a:solidFill><a:srgbClr val=\"FF0000\"/>"
                      "</a:solidFill><a:prstDash val=\"dash\"/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>"
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"0\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
                      "<a:prstGeom prst=\"rect\"/><a:gradFill><a:gsLst><a:gs pos=\"0\"><a:srgbClr val=\"FF0000\"/></a:gs>"
                      "<a:gs pos=\"50000\"><a:srgbClr val=\"00FF00\"/></a:gs><a:gs pos=\"100000\"><a:srgbClr val=\"0000FF\"/>"
                      "</a:gs></a:gsLst><a:lin ang=\"0\" scaled=\"0\"/></a:gradFill></wps:spPr><wps:bodyPr/></wps:wsp>"
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"1270000\"/><a:ext cx=\"635000\" cy=\"635000\"/></a:xfrm>"
                      "<a:prstGeom prst=\"rect\"/><a:pattFill prst=\"smCheck\"><a:fgClr><a:srgbClr val=\"000000\"/></a:fgClr>"
                      "<a:bgClr><a:srgbClr val=\"FFFFFF\"/></a:bgClr></a:pattFill></wps:spPr><wps:bodyPr/></wps:wsp>",
                      &d, &L, &n);
    buf_t b;
    pd_inline o;

    if (!it) {
        pd_layout_free(L);
        pd_doc_free(d);
        return;
    }

    memset(&b, 0, sizeof(b));
    CHECK(pd_layout_write_svg(L, 0, to_buf, &b) == PD_OK);
    CHECK(b.p && strstr(b.p, "<svg xmlns=\"http://www.w3.org/2000/svg\"") && strstr(b.p, "viewBox=\"0 0 "));
    CHECK(b.p && strstr(b.p, "stroke-dasharray=\"8 6\"") && strstr(b.p, "stroke=\"#ff0000\""));
    CHECK(b.p && strstr(b.p, "C") && strstr(b.p, "<linearGradient") && strstr(b.p, "<stop offset=\"0.5000\" stop-color=\"#00ff00\"/>"));
    CHECK(b.p && strstr(b.p, "<pattern id=") && strstr(b.p, "fill=\"url(#p"));
    free(b.p);

    if (pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK) {
        memset(&b, 0, sizeof(b));   /* the drawing alone, 200 pt square */
        CHECK(pd_drawing_to_svg(d, o.resource, PD_PT(200), PD_PT(200), to_buf, &b) == PD_OK);
        CHECK(b.p && strstr(b.p, "viewBox=\"0 0 200 200\"") && strstr(b.p, "stroke-dasharray=\"8 6\"") &&
              strstr(b.p, "<linearGradient"));
        free(b.p);
    }

    memset(&b, 0, sizeof(b));   /* HTML: an SVG picture, the drawing kept beside it and read back */
    CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &b) == PD_OK);
    CHECK(b.p && strstr(b.p, "<img src=\"data:image/svg+xml;base64,") &&
          strstr(b.p, "data-parade=\"data:application/vnd.parade.drawing+json;base64,"));

    if (b.p && pd_doc_import(b.p, b.n, PD_CONV_HTML, &back) == PD_OK) {
        const char* mime = NULL;
        const void* data;
        size_t len;
        int found = pd_doc_resource(back, 1, &mime, &data, &len) == PD_OK &&
                    !strcmp(mime, "application/vnd.parade.drawing+json");

        CHECK(found);
        pd_doc_free(back);
    }

    free(b.p);
    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* A drawing as a picture: rasterized on its own (a red box over its left half, a blue ellipse at the right), as a
   PNG file, and in RTF -- which took only PNG and JPEG pictures -- as a PNG picture of it. */
static void test_drawing_png(void) {
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    int32_t n = 0;
    pd_draw* it = group_items(
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"2540000\"/></a:xfrm>"
                      "<a:prstGeom prst=\"rect\"/><a:solidFill><a:srgbClr val=\"FF0000\"/></a:solidFill><a:ln><a:noFill/>"
                      "</a:ln></wps:spPr><wps:bodyPr/></wps:wsp>"
                      "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"1270000\" y=\"635000\"/><a:ext cx=\"1270000\" cy=\"1270000\"/>"
                      "</a:xfrm><a:prstGeom prst=\"ellipse\"/><a:solidFill><a:srgbClr val=\"0000FF\"/></a:solidFill><a:ln>"
                      "<a:noFill/></a:ln></wps:spPr><wps:bodyPr/></wps:wsp>", &d, &L, &n);
    pd_inline o;

    if (it && pd_doc_inline_at(d, at(pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), 0), &o) == PD_OK) {
        uint8_t* px = (uint8_t*)calloc(200 * 200, 4);
        buf_t b;

        if (px) {   /* 200 pt square at a pixel a point */
            CHECK(pd_drawing_render(d, o.resource, PD_PT(200), PD_PT(200), 1 << 16, px, 200, 200, 800) == PD_OK);
            CHECK(px[(100 * 200 + 50) * 4] == 255 && px[(100 * 200 + 50) * 4 + 3] == 255);    /* red at the left */
            CHECK(px[(100 * 200 + 150) * 4 + 2] == 255 && px[(100 * 200 + 150) * 4] == 0);    /* blue in the ellipse */
            CHECK(px[(10 * 200 + 190) * 4 + 3] == 0);   /* clear at the top right, outside the ellipse */
            free(px);
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_drawing_to_png(d, o.resource, PD_PT(200), PD_PT(200), 1 << 16, to_buf, &b) == PD_OK);
        CHECK(b.p && b.n > 33 && !memcmp(b.p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(b.p + 12, "IHDR\0\0\0\xc8\0\0\0\xc8", 12));
        free(b.p);
        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, PD_CONV_RTF, to_buf, &b) == PD_OK);
        CHECK(b.p && strstr(b.p, "{\\pict\\pngblip") && strstr(b.p, "89504e470d0a1a0a"));
        free(b.p);
    }

    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
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

/* A picture in the text deleted with track changes, its replacement inserted after it -- how Word keeps a picture
   swapped for another: where deletions are not shown in the text (balloons, final) it is not drawn and takes no
   room, as a deleted float already was not; shown inline (and in the original), it is.  Drawn regardless, the
   deleted one stood beside its replacement in a narrow box and the two overlapped (a proposal's Fig. 2b, 2f). */
static void test_docx_deleted_inline_picture(void) {
    static const char* pic =
        "<w:drawing><wp:inline><wp:extent cx=\"1270000\" cy=\"1270000\"/><a:graphic><a:graphicData><pic:pic>"
        "<pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline>"
        "</w:drawing>";
    char xml[4096];
    pd_doc* d;
    int pass;

    snprintf(xml, sizeof(xml),
             "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body>"
             "<w:p><w:del w:id=\"1\" w:author=\"A\"><w:r>%s</w:r></w:del><w:ins w:id=\"2\" w:author=\"A\"><w:r>%s"
             "</w:r></w:ins></w:p></w:body></w:document>", pic, pic);
    d = docx_doc(
        "word/_rels/document.xml.rels",
        "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
        "</Relationships>",
        "word/media/image1.png", "tests/data/rgba.png",
        "word/document.xml", xml,
        NULL);

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        CHECK(d != NULL);

        if (!d) {
            return;
        }

        CHECK(pd_doc_revision_count(d) >= 2);

        if (!have_layout_font()) {
            continue;
        }

        pd_doc_set_markup(d, PD_MARKUP_BALLOONS);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 1);
        pd_doc_set_markup(d, PD_MARKUP_FINAL);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 1);
        pd_doc_set_markup(d, PD_MARKUP_INLINE);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 2);
        pd_doc_set_markup(d, PD_MARKUP_ORIGINAL);
        CHECK(count_draws(d, PD_DRAW_IMAGE, 0) == 1);
        pd_doc_set_markup(d, PD_MARKUP_BALLOONS);
    }

    pd_doc_free(d);
}

/* A text box deleted with track changes, an insertion inside it: everything in a deleted shape is deleted, as Word
   has it -- the inserted text too, which was shown (a lone "7" of a figure's old caption) and with it the box's
   room, an empty block the text wrapped round */
static void test_docx_deleted_box_insertion(void) {
    pd_doc* d = docx_doc(
        "word/document.xml",
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wps=\"wps\" xmlns:r=\"r\"><w:body>"
        "<w:p><w:r><w:t xml:space=\"preserve\">Text beside the box, enough to wrap. </w:t></w:r>"
        "<w:del w:id=\"1\" w:author=\"A\"><w:r><w:drawing><wp:anchor distL=\"114300\" distR=\"114300\">"
        "<wp:positionH relativeFrom=\"column\"><wp:posOffset>2671445</wp:posOffset></wp:positionH>"
        "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>"
        "<wp:extent cx=\"2540000\" cy=\"1270000\"/><wp:wrapSquare wrapText=\"bothSides\"/><a:graphic><a:graphicData>"
        "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"1270000\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx><w:txbxContent><w:p>"
        "<w:del w:id=\"2\" w:author=\"A\"><w:r><w:delText>Fig. 6</w:delText></w:r></w:del>"
        "<w:ins w:id=\"3\" w:author=\"A\"><w:r><w:t>7</w:t></w:r></w:ins>"
        "</w:p></w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp></a:graphicData></a:graphic></wp:anchor></w:drawing>"
        "</w:r></w:del><w:r><w:t>And on.</w:t></w:r></w:p></w:body></w:document>",
        NULL);
    pd_block_id p = 0, q;
    pd_char_props cp;
    pd_revision rv;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    for (q = pd_doc_next_paragraph(d, 0); q && !p; q = pd_doc_next_paragraph(d, q)) {
        uint32_t n;
        const char* t = para_text(d, q, &n);

        if (n >= 1 && t[n - 1] == '7') {
            p = q;
        }
    }

    if (!p) {
        p = story_para_with(d, "7");
    }

    CHECK(p != 0);
    cp = chars_at(d, p, p ? 0 : 0);

    {   /* the inserted "7": in the deleted box, deleted */
        uint32_t n, k;
        const char* t = p ? para_text(d, p, &n) : NULL;

        for (k = 0; t && k < n && t[k] != '7'; k++) {
        }

        cp = chars_at(d, p, k);
        CHECK(t && k < n && cp.revision && pd_doc_revision_get(d, cp.revision, &rv) == PD_OK &&
              rv.kind == PD_REV_DELETE);
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

        if (pd_doc_block_info(d, p, &bi) != PD_OK || bi.kind != PD_BLOCK_PARAGRAPH) {
            if (bi.kind == PD_BLOCK_TABLE) {
                pd_block_id c = pd_doc_child(d, pd_doc_child(d, p, 0), 0);
                pd_cell_props cp;

                CHECK(pd_doc_cell_props(d, c, &cp) == PD_OK);
                CHECK(text_is(d, pd_doc_child(d, c, 0), "Name"));
            }

            continue;
        }

        found_title |= text_is(d, p, "Hello Slides");
        {   /* the bullets list labels, as PowerPoint's are bullets and not text */
            char lb[16] = "", lb2[16] = "";

            found_bullet |= text_is(d, p, "First point") && pd_doc_list_label(d, p, lb, sizeof(lb)) == PD_OK &&
                            !strcmp(lb, "\xE2\x80\xA2");
            found_sub |= pd_doc_child(d, st, 1) && text_is(d, pd_doc_child(d, st, 1), "A detail") &&
                         pd_doc_list_label(d, pd_doc_child(d, st, 1), lb2, sizeof(lb2)) == PD_OK &&
                         !strcmp(lb2, "\xE2\x80\x93");
        }
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

/* a slide of a presentation made by pptx_doc: its attributes, its shapes */
#define PSLD(attrs, body) "<p:sld xmlns:p=\"p\" xmlns:a=\"a\" xmlns:r=\"r\"" attrs "><p:cSld><p:spTree>" body \
    "</p:spTree></p:cSld></p:sld>"
/* a text box (ID a string) at the slide's corner, 4 by 2 inches, with paragraphs BODY */
#define PSP(id, body) "<p:sp><p:nvSpPr><p:cNvPr id=\"" id "\" name=\"s\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>" \
    "<p:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"3657600\" cy=\"1828800\"/></a:xfrm><a:prstGeom prst=\"rect\">" \
    "<a:avLst/></a:prstGeom></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/>" body "</p:txBody></p:sp>"
#define PP(text) "<a:p><a:r><a:t>" text "</a:t></a:r></a:p>"

/* the drawing items of slide K's canvas, as text (caller frees); NULL if none */
static char* slide_json(const pd_doc* d, int k) {
    pd_block_id fl = pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), k), 0);
    pd_inline o;

    return pd_doc_inline_at(d, at(pd_doc_child(d, fl, 0), 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE ?
           drawing_json(d, o.resource) : NULL;
}

/* a hidden slide (show="0") is kept and edited as the others, marked hidden: left out of a PDF (two pages of three),
   written back hidden; the slides after it numbered as PowerPoint numbers them */
static void test_pptx_hidden(void) {
    pd_doc* d = pptx_doc(3,
                         "ppt/slides/slide1.xml", PSLD("", PSP("2", PP("One"))),
                         "ppt/slides/slide2.xml", PSLD(" show=\"0\"", PSP("2", PP("Two"))),
                         "ppt/slides/slide3.xml", PSLD("", PSP("2", PP("Three")
                                 "<a:p><a:fld id=\"{1}\" type=\"slidenum\"><a:t>9</a:t></a:fld></a:p>")),
                         NULL);
    pd_block_info ri;
    pd_section_props sp;
    int k;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    CHECK(pd_doc_block_info(d, pd_doc_root(d), &ri) == PD_OK && ri.child_count == 3);
    CHECK(story_para_with(d, "One") && story_para_with(d, "Two") && story_para_with(d, "Three"));
    CHECK(story_para_with(d, "3") && text_is(d, story_para_with(d, "3"), "3"));

    for (k = 0; k < 3; k++) {
        CHECK(pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), k), &sp) == PD_OK && sp.hidden == (k == 1));
    }

    if (have_layout_font()) {   /* the PDF: the hidden slide's page left out */
        pd_layout* L = NULL;
        buf_t b = { 0 };

        pd_doc_set_font_resolver(d, layout_resolver, NULL);
        CHECK(pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK && pd_layout_page_count(L) == 3);
        CHECK(pd_layout_write_pdf(L, NULL, to_buf, &b) == PD_OK && b.p && strstr(b.p, "/Count 2 >>"));
        free(b.p);
        pd_layout_free(L);
    }

    {   /* written back hidden */
        buf_t b = { 0 };
        char* x;
        size_t n;

        CHECK(pd_doc_export(d, PD_CONV_PPTX, to_buf, &b) == PD_OK);
        x = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide2.xml", &n) : NULL;
        CHECK(x && strstr(x, " show=\"0\""));
        free(x);
        x = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide1.xml", &n) : NULL;
        CHECK(x && !strstr(x, " show=\"0\""));
        free(x);
        free(b.p);
    }

    pd_doc_free(d);
}

/* a deck of one slide: its canvas at the page's corner. (Its section's margins, none, are read after the canvas, the
   last section's being at the document's end; a picture placed from the page's edge was put an inch to the left of
   it, Word's default margin.) */
static void test_pptx_one_slide(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", PSP("2", PP("Alone"))), NULL);
    pd_float_props fp;

    CHECK(d && pd_doc_float_props(d, pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), &fp) == PD_OK &&
          fp.offset_x == 0);
    pd_doc_free(d);
}

/* a paragraph with properties PPR and text T */
#define PPP(ppr, text) "<a:p><a:pPr" ppr "</a:pPr><a:r><a:rPr sz=\"2000\"/><a:t>" text "</a:t></a:r></a:p>"

/* the text of the story paragraph whose text has NEEDLE in it, up to the tab after its bullet */
static void bullet_of(const pd_doc* d, const char* needle, char* out, size_t cap) {
    int32_t i;

    out[0] = '\0';

    for (i = 0; i < pd_doc_story_count(d); i++) {
        pd_block_id st = pd_doc_story_at(d, i);
        pd_block_info bi;
        int32_t k;

        for (k = 0; pd_doc_block_info(d, st, &bi) == PD_OK && k < bi.child_count; k++) {
            uint32_t n;
            const char* t = para_text(d, pd_doc_child(d, st, k), &n);
            const char* tab = (const char*)memchr(t, '\t', n);

            if (tab && n - (size_t)(tab - t) - 1 == strlen(needle) && !memcmp(tab + 1, needle, strlen(needle))) {
                snprintf(out, cap, "%.*s", (int)(tab - t), t);
                return;
            }

            if (n == strlen(needle) && !memcmp(t, needle, n) &&      /* a bullet made a list label */
                    pd_doc_list_label(d, pd_doc_child(d, st, k), out, (int32_t)cap) == PD_OK && out[0]) {
                return;
            }
        }
    }
}

/* autonumbered paragraphs: the scheme's numbers and what is round them, startAt, each level counting on its own and
   starting again under a paragraph less deep; a bullet's face and size in points */
static void test_pptx_autonum(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", PSP("2",
                         PPP("><a:buAutoNum type=\"alphaLcParenR\" startAt=\"3\"/>", "c1")
                         PPP("><a:buAutoNum type=\"alphaLcParenR\" startAt=\"3\"/>", "c2")
                         PPP(" lvl=\"1\"><a:buAutoNum type=\"romanUcPeriod\"/>", "r1")
                         PPP(" lvl=\"1\"><a:buAutoNum type=\"romanUcPeriod\"/>", "r2")
                         "<a:p><a:pPr lvl=\"1\"><a:buAutoNum type=\"romanUcPeriod\"/></a:pPr></a:p>"
                         PPP("><a:buAutoNum type=\"alphaLcParenR\" startAt=\"3\"/>", "c3")
                         PPP(" lvl=\"1\"><a:buAutoNum type=\"romanUcPeriod\"/>", "r3")
                         PPP("><a:buAutoNum type=\"circleNumWdBlackPlain\"/>", "k1")
                         PPP("><a:buAutoNum type=\"ea1JpnChsDbPeriod\" startAt=\"12\"/>", "e1")
                         PPP("><a:buAutoNum type=\"arabicParenBoth\"/>", "p1")
                         PPP("><a:buNone/>", "none")
                         PPP("><a:buAutoNum type=\"arabicParenBoth\"/>", "p2")
                         PPP("><a:buAutoNum type=\"thaiNumPeriod\" startAt=\"5\"/>", "t1")
                         PPP("><a:buAutoNum type=\"hebrew2Minus\" startAt=\"2\"/>", "h1")
                         PPP("><a:buAutoNum type=\"circleNumDbPlain\" startAt=\"3\"/>", "o1")
                         PPP("><a:buSzPts val=\"1200\"/><a:buFont typeface=\"Arial\"/><a:buChar char=\"-\"/>", "dash"))),
                         NULL);
    static const char* const want[][2] = {
        { "c1", "c)" }, { "c2", "d)" }, { "r1", "I." }, { "r2", "II." }, { "c3", "e)" }, { "r3", "I." },
        { "k1", "\xE2\x9D\xB6" }, { "e1", "\xE5\x8D\x81\xE4\xBA\x8C\xEF\xBC\x8E" }, { "p1", "(1)" },
        { "p2", "(1)" }, { "t1", "\xE0\xB9\x95." }, { "h1", "\xD7\x91-" }, { "o1", "\xE2\x91\xA2" }, { "dash", "-" }
    };
    char buf[64];
    size_t k;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    for (k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        bullet_of(d, want[k][0], buf, sizeof(buf));
        CHECK_STR(buf, want[k][1]);
    }

    {   /* the dash a list label 12 points, in Arial; the text after it 20 */
        pd_block_id p = story_para_with(d, "dash");
        pd_char_props b = chars_at(d, p, 1);
        pd_block_info bi;
        pd_list_level lv[9];
        int32_t nl = 0;

        nl = 9;
        CHECK(p && pd_doc_block_info(d, p, &bi) == PD_OK && bi.list != 0 &&
              pd_doc_list_info(d, bi.list, &nl, lv) == PD_OK && bi.list_level < nl &&
              lv[bi.list_level].label_size == PD_PT(12) && !strcmp(lv[bi.list_level].label_family, "Arial") &&
              b.size == PD_PT(20));
    }

    pd_doc_free(d);
}

/* a picture bullet (buBlip): the picture before the text, as tall as the text, square */
static void test_pptx_picture_bullet(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", PSP("2",
                         PPP("><a:buBlip><a:blip r:embed=\"rIdB\"/></a:buBlip>", "pictured"))),
                         "ppt/slides/_rels/slide1.xml.rels", "<Relationships><Relationship Id=\"rIdB\" "
                         "Target=\"../media/b.png\"/></Relationships>",
                         "ppt/media/b.png", "tests/data/rgba.png", NULL);
    pd_block_id p = 0;
    pd_inline o;
    int32_t i;

    CHECK(d != NULL);

    for (i = 0; d && i < pd_doc_story_count(d); i++) {
        pd_block_id q = pd_doc_child(d, pd_doc_story_at(d, i), 0);
        uint32_t n;
        const char* t = para_text(d, q, &n);

        if (n >= 8 && !memcmp(t + n - 8, "pictured", 8)) {
            p = q;
        }
    }

    CHECK(p && pd_doc_inline_at(d, at(p, 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE && o.width == PD_PT(20) &&
          o.height == PD_PT(20));
    pd_doc_free(d);
}

/* PowerPoint's Set Transparent Color (a:clrChange to a colour of alpha 0): the picture's pixels of that colour
   see-through, the others as they were; the same picture without it left opaque */
static int keyed_found(const pd_doc* d, int* opaque_white) {
    pd_res_id r;
    int found = 0;

    *opaque_white = 0;

    for (r = 1; r < 400; r++) {
        const char* mime;
        const void* data;
        size_t len;
        int32_t w, h;
        unsigned char *rgb = NULL, *alpha = NULL;

        if (pd_doc_resource(d, r, &mime, &data, &len) != PD_OK || strcmp(mime, "image/png") != 0) {
            continue;
        }

        if (pd_image_decode((const unsigned char*)data, len, &w, &h, &rgb, &alpha) == 0 && w == 2 && h == 1) {
            if (alpha && alpha[0] == 0 && alpha[1] == 255 && rgb[3] == 255 && rgb[4] == 0 && rgb[5] == 0) {
                found = 1;
            }

            if ((!alpha || alpha[0] == 255) && rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255) {
                *opaque_white = 1;
            }
        }

        free(rgb);
        free(alpha);
    }

    return found;
}

/* a LaTeX file read: its parts as what they mean, numbered as LaTeX numbers them, its macros expanded */
/* a document's text, paragraphs joined by '|' */
static void doc_text(pd_doc* d, char* all, size_t cap) {
    pd_block_id p;
    size_t w = 0;

    for (p = pd_doc_next_paragraph(d, 0); p; p = pd_doc_next_paragraph(d, p)) {
        uint32_t n;
        const char* t = para_text(d, p, &n);

        if (w + n + 2 < cap) {
            memcpy(all + w, t, n);
            w += n;
            all[w++] = '|';
        }
    }

    all[w] = '\0';
}

/* the .bib led puts in where \bibliography names it: the cited entries set out, numbered as cited or sorted */
/* LaTeX's text justified as TeX sets it, its margins kerned; \raggedright, centred and code text not */
/* SVG pictures: drawn as Parade draws them (checked by the pixels: a class's colour from a style sheet, an
   even-odd hole, a gradient's way, text), kept through .pdoc, and written to and read from a .docx as Office has
   them (a PNG of it in the blip, the SVG in asvg:svgBlip) */
static const char svg_doc[] =
    "<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"100\" viewBox=\"0 0 200 200\">"
    "<style>.g { fill: #00ff00 }</style>"
    "<defs><linearGradient id=\"lg\"><stop offset=\"0\" stop-color=\"black\"/><stop offset=\"1\" stop-color=\"white\"/>"
    "</linearGradient></defs>"
    "<rect x=\"0\" y=\"0\" width=\"100\" height=\"100\" fill=\"#ff0000\"/>"
    "<circle class=\"g\" cx=\"150\" cy=\"50\" r=\"40\"/>"
    "<g transform=\"translate(0 100)\"><rect width=\"100\" height=\"100\" fill=\"url(#lg)\"/></g>"
    "<path d=\"M100 100 h100 v100 h-100 Z M130 130 h40 v40 h-40 Z\" fill=\"blue\" fill-rule=\"evenodd\"/>"
    "<text x=\"10\" y=\"90\" font-size=\"20\" fill=\"#123456\">Hi &amp; bye</text>"
    "</svg>";

static int svg_px(const uint8_t* px, int x, int y, int c) {
    return px[(y * 100 + x) * 4 + c];
}

static int svg_drawn(const pd_doc* d, pd_res_id r, uint8_t* px) {
    memset(px, 0, 100 * 100 * 4);
    return pd_drawing_render(d, r, PD_PT(75), PD_PT(75), 65536 * 4 / 3, px, 100, 100, 400) == PD_OK;
}

static void test_svg(void) {
    pd_doc* d = NULL, *x = NULL;
    pd_res_id r = 0, src;
    const char* mime;
    const void* data;
    size_t len;
    uint8_t* px = (uint8_t*)malloc(100 * 100 * 4);
    buf_t b = { NULL, 0 };

    pd_doc_new(&d);
    CHECK(pd_doc_add_picture(d, NULL, svg_doc, sizeof(svg_doc) - 1, &r) == PD_OK && r);   /* known by its bytes */
    CHECK(pd_doc_resource(d, r, &mime, &data, &len) == PD_OK && !strcmp(mime, "application/vnd.parade.drawing+json"));
    src = pd_doc_picture_source(d, r);
    CHECK(src && pd_doc_resource(d, src, &mime, &data, &len) == PD_OK && !strcmp(mime, "image/svg+xml") &&
          len == sizeof(svg_doc) - 1);
    CHECK(pd_doc_resource(d, r, &mime, &data, &len) == PD_OK && strstr((const char*)data, "\"label\":\"Hi & bye\"") &&
          strstr((const char*)data, "\"c\":4279383126"));     /* its text, its colour (0xFF123456) */
    CHECK(pd_doc_resource(d, r, &mime, &data, &len) == PD_OK && strstr((const char*)data, "\"w\":4915200"));  /* 100 px */

    if (px && svg_drawn(d, r, px)) {
        CHECK(svg_px(px, 25, 25, 0) > 240 && svg_px(px, 25, 25, 1) < 15);     /* red, top left */
        CHECK(svg_px(px, 75, 25, 1) > 240 && svg_px(px, 75, 25, 0) < 15);     /* the style sheet's green */
        CHECK(svg_px(px, 2, 25, 3) > 240);
        CHECK(svg_px(px, 60, 60, 2) > 240 && svg_px(px, 60, 60, 3) > 240);    /* the ring blue */
        CHECK(svg_px(px, 75, 75, 3) < 10);      /* its hole clear: even-odd, though both rings turn the same way */
        CHECK(svg_px(px, 5, 75, 0) < 40 && svg_px(px, 45, 75, 0) > 200);     /* dark to light, left to right */
        CHECK(svg_px(px, 0, 99, 3) > 240 && svg_px(px, 99, 0, 3) < 10);      /* (nothing past the circle) */
    }

    /* the native file: the SVG kept, the drawing made again from it */
    CHECK(pd_doc_save(d, PD_JDATA_BINARY, to_buf, &b) == PD_OK);
    CHECK(pd_doc_load(b.p, b.n, PD_JDATA_BINARY, &x) == PD_OK && x);

    if (x) {
        const void* d1, *d2;
        size_t l1, l2;

        CHECK(pd_doc_picture_source(x, r) == src);
        CHECK(pd_doc_resource(d, r, &mime, &d1, &l1) == PD_OK && pd_doc_resource(x, r, &mime, &d2, &l2) == PD_OK &&
              l1 == l2 && !memcmp(d1, d2, l1));
        pd_doc_free(x);
        x = NULL;
    }

    free(b.p);
    b.p = NULL;
    b.n = 0;

    {   /* in a .docx: the SVG beside a PNG of it, and read back as the SVG */
        pd_block_id p = pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0);
        pd_inline o;
        pd_pos c;
        unsigned char* part;
        size_t pn;

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_IMAGE;
        o.resource = r;
        o.width = o.height = PD_PT(75);
        CHECK(pd_doc_insert_inline(d, at(p, 0), &o, &c) == PD_OK);
        CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        part = pd_zip_get((const unsigned char*)b.p, b.n, "word/document.xml", &pn);
        CHECK(part && strstr((char*)part, "<asvg:svgBlip") && strstr((char*)part, "<a:blip r:embed=\"rIdm1\">"));
        free(part);
        part = pd_zip_get((const unsigned char*)b.p, b.n, "word/media/image1.png", &pn);
        CHECK(part && pn > 8 && !memcmp(part, "\x89PNG", 4));      /* the picture Word shows without SVG */
        free(part);
        part = pd_zip_get((const unsigned char*)b.p, b.n, "word/media/image2.svg", &pn);
        CHECK(part && pn == sizeof(svg_doc) - 1 && !memcmp(part, svg_doc, pn));   /* the SVG as it came */
        free(part);
        part = pd_zip_get((const unsigned char*)b.p, b.n, "[Content_Types].xml", &pn);
        CHECK(part && strstr((char*)part, "Extension=\"svg\" ContentType=\"image/svg+xml\""));
        free(part);
        CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &x) == PD_OK && x);

        if (x) {
            pd_block_id q = pd_doc_next_paragraph(x, 0);
            pd_inline got;

            CHECK(pd_doc_inline_at(x, at(q, 0), &got) == PD_OK && got.kind == PD_INLINE_IMAGE);
            src = pd_doc_picture_source(x, got.resource);
            CHECK(src && pd_doc_resource(x, src, &mime, &data, &len) == PD_OK && !strcmp(mime, "image/svg+xml"));

            if (px && svg_drawn(x, got.resource, px)) {
                CHECK(svg_px(px, 75, 25, 1) > 240 && svg_px(px, 75, 75, 3) < 10);   /* drawn from the SVG again */
            }

            pd_doc_free(x);
            x = NULL;
        }

        free(b.p);
        b.p = NULL;
        b.n = 0;
    }

    {   /* a slide's picture as PowerPoint has one (a PNG, its SVG in asvg:svgBlip): drawn from the SVG, and written
           back with both */
        pd_doc* pp = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("",
                              "<p:pic><p:nvPicPr><p:cNvPr id=\"4\" name=\"Graphic 3\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>"
                              "<p:blipFill><a:blip r:embed=\"rId2\"><a:extLst><a:ext uri=\"{96DAC541-7B7A-43D3-8B79-37D633B846F1}\">"
                              "<asvg:svgBlip xmlns:asvg=\"http://schemas.microsoft.com/office/drawing/2016/SVG/main\" r:embed=\"rId3\"/>"
                              "</a:ext></a:extLst></a:blip><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr><a:xfrm>"
                              "<a:off x=\"914400\" y=\"914400\"/><a:ext cx=\"952500\" cy=\"952500\"/></a:xfrm>"
                              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>"),
                              "ppt/slides/_rels/slide1.xml.rels",
                              "<Relationships xmlns=\"r\"><Relationship Id=\"rId2\" Type=\"t/image\" Target=\"../media/image1.png\"/>"
                              "<Relationship Id=\"rId3\" Type=\"t/image\" Target=\"../media/image2.svg\"/></Relationships>",
                              "ppt/media/image1.png", "tests/data/rgba.png", "ppt/media/image2.svg", svg_doc, NULL);
        int32_t k, svgs = 0, drawn = 0;

        CHECK(pp != NULL);

        for (k = 1; pp && k <= pd_doc_res_count(pp); k++) {
            src = pd_doc_picture_source(pp, (pd_res_id)k);
            drawn += src && pd_doc_resource(pp, src, &mime, &data, &len) == PD_OK && !strcmp(mime, "image/svg+xml");
        }

        CHECK(drawn == 1);     /* the SVG made a drawing */
        CHECK(pp && count_draws(pp, PD_DRAW_PATH, 0) >= 4);     /* and it is what the slide draws: its shapes */

        if (pp && pd_doc_export(pp, PD_CONV_PPTX, to_buf, &b) == PD_OK) {
            unsigned char* part = pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide1.xml", &len);
            char name[64];

            CHECK(part && strstr((char*)part, "<asvg:svgBlip"));
            free(part);

            for (k = 1; k <= 8; k++) {
                snprintf(name, sizeof(name), "ppt/media/%simage%d.svg", k > 4 ? "pd" : "", k > 4 ? k - 4 : k);
                part = pd_zip_get((const unsigned char*)b.p, b.n, name, &len);
                svgs += part && len == sizeof(svg_doc) - 1 && !memcmp(part, svg_doc, len);
                free(part);
            }

            CHECK(svgs == 1);   /* the SVG as it came */
            CHECK(pd_doc_import(b.p, b.n, PD_CONV_PPTX, &x) == PD_OK && x);

            for (k = 1, drawn = 0; x && k <= pd_doc_res_count(x); k++) {
                src = pd_doc_picture_source(x, (pd_res_id)k);
                drawn += src && pd_doc_resource(x, src, &mime, &data, &len) == PD_OK && !strcmp(mime, "image/svg+xml");
            }

            CHECK(drawn == 1);  /* and read back as the SVG */
            pd_doc_free(x);
            x = NULL;
        } else {
            CHECK(0);
        }

        free(b.p);
        b.p = NULL;
        b.n = 0;
        pd_doc_free(pp);
    }

    /* what is not a picture is refused; an SVG with nothing to draw, too */
    CHECK(pd_doc_add_picture(d, NULL, "hello", 5, &r) == PD_ERR_FORMAT);
    CHECK(pd_doc_add_picture(d, "image/svg+xml", "<svg xmlns=\"http://www.w3.org/2000/svg\"/>", 40, &r) == PD_ERR_FORMAT);
    pd_doc_free(d);
    free(px);
}

static void test_latex_justified(void) {
    static const char tex[] =
        "\\documentclass{article}\\begin{document}\n"
        "\\section{Intro}\n"
        "Body text of the paper.\n\n"
        "\\begin{quote}A quoted line.\\end{quote}\n"
        "\\begin{center}Centred words.\\end{center}\n"
        "\\begin{minipage}{3cm}\\raggedright Ragged inside.\\end{minipage}\n\n"
        "Justified again after it.\n"
        "\\begin{verbatim}\ncode here\n\\end{verbatim}\n"
        "\\end{document}\n";
    pd_doc* d = NULL;

    CHECK(pd_doc_import(tex, sizeof(tex) - 1, PD_CONV_LATEX, &d) == PD_OK && d);

    if (d) {
        CHECK(para_resolved(d, find_para(d, "Body text")).align == PD_ALIGN_JUSTIFY);
        CHECK(para_resolved(d, find_para(d, "A quoted")).align == PD_ALIGN_JUSTIFY);
        CHECK(para_resolved(d, find_para(d, "Centred")).align == PD_ALIGN_CENTER);
        CHECK(para_resolved(d, find_para(d, "Ragged inside")).align == PD_ALIGN_LEFT);
        CHECK(para_resolved(d, find_para(d, "Justified again")).align == PD_ALIGN_JUSTIFY);    /* its group over */
        CHECK(para_resolved(d, find_para(d, "Intro")).align != PD_ALIGN_JUSTIFY);
        CHECK(para_resolved(d, find_para(d, "code here")).align != PD_ALIGN_JUSTIFY);

        if (have_layout_font()) {   /* protrusion on for the text, not for the code */
            pd_para* para = NULL;
            pd_params prm;

            pd_doc_set_font_resolver(d, layout_resolver, NULL);
            CHECK(pd_para_new(&para) == PD_OK);
            CHECK(pd_doc_para_build(d, find_para(d, "Body text"), PD_PT(300), para, &prm) == PD_OK && prm.protrusion == 1);
            CHECK(pd_doc_para_build(d, find_para(d, "code here"), PD_PT(300), para, &prm) == PD_OK && prm.protrusion == 0);
            pd_para_free(para);
        }

        pd_doc_free(d);
    }

    {   /* long words broken with a hyphen from the patterns: shown, and said, as a hyphen (its cluster is the letter
           after it, which a PDF's text would otherwise repeat) */
        char t2[4096];
        pd_hyph* h = NULL;
        pd_layout* L = NULL;
        int n = snprintf(t2, sizeof(t2), "\\documentclass{article}\\begin{document}\n"), k, hyphens = 0, letters = 0;
        const char* path = "/usr/share/hyphen/hyph_en_US.dic";

        for (k = 0; k < 120; k++) {
            n += snprintf(t2 + n, sizeof(t2) - (size_t)n, "%s ", k % 2 ? "characterization" : "photoacoustic");
        }

        n += snprintf(t2 + n, sizeof(t2) - (size_t)n, "end.\n\\end{document}\n");

        if (have_layout_font() && pd_hyph_load_file(path, &h) == PD_OK &&
            pd_doc_import(t2, (size_t)n, PD_CONV_LATEX, &d) == PD_OK) {
            int32_t c = 0, i;
            pd_draw* it;

            pd_doc_set_font_resolver(d, layout_resolver, NULL);
            pd_doc_set_hyphenator(d, "", h);
            CHECK(pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK);
            pd_layout_page_items(L, 0, NULL, 0, &c);
            it = (pd_draw*)calloc((size_t)c + 1, sizeof(*it));
            pd_layout_page_items(L, 0, it, c, &c);

            for (i = 0; i + 1 < c; i++) {
                if (it[i].kind == PD_DRAW_GLYPH && it[i + 1].kind == PD_DRAW_GLYPH && it[i + 1].y != it[i].y) {
                    hyphens += it[i].text == '-';
                    letters += it[i].text >= 'a' && it[i].text <= 'z';
                }
            }

            CHECK(hyphens >= 3 && letters >= 1);    /* (not every line broken inside a word) */
            free(it);
            pd_layout_free(L);
            pd_doc_free(d);
            pd_hyph_free(h);
        } else {
            printf("  (no hyphenation patterns at %s: skipped)\n", path);
            pd_hyph_free(h);
        }
    }
}

/* LaTeX's paragraph indents: every paragraph's first line by \parindent, but the first after a heading, one after
   \noindent, and text going on after a display or a list with no blank line (the same paragraph to LaTeX) */
static void test_latex_indent(void) {
    static const char tex[] =
        "\\documentclass{article}\\begin{document}\n"
        "Opening.\n\n"
        "\\section{A}\n\nFirst.\n\nSecond.\n\n\\noindent Third.\n\n"
        "Fourth\n\\[ x = 1 \\]\ncontinued.\n\n"
        "Fifth\n\\begin{itemize}\\item An item\\end{itemize}\nafter the list.\n\n"
        "\\begin{center}Centred\\end{center}\n\nSixth.\n\n"
        "\\setlength{\\parindent}{2em}Seventh.\n\n\\parindent=0pt Eighth.\n"
        "\\end{document}\n";
    static const char pk[] =
        "\\documentclass{article}\\usepackage[skip=4pt]{parskip}\\begin{document}\n"
        "One.\n\nTwo.\n\\end{document}\n";
    pd_doc* d = NULL;
    char all[4096];

    CHECK(pd_doc_import(tex, sizeof(tex) - 1, PD_CONV_LATEX, &d) == PD_OK && d);

    if (d) {
        CHECK(para_resolved(d, find_para(d, "Opening")).indent_first == PD_PT(15));
        CHECK(para_resolved(d, find_para(d, "First")).indent_first == 0);      /* after the heading, a blank line or not */
        CHECK(para_resolved(d, find_para(d, "Second")).indent_first == PD_PT(15));
        CHECK(para_resolved(d, find_para(d, "Third")).indent_first == 0);
        CHECK(para_resolved(d, find_para(d, "Fourth")).indent_first == PD_PT(15));
        CHECK(para_resolved(d, find_para(d, "continued")).indent_first == 0);
        CHECK(para_resolved(d, find_para(d, "Fifth")).indent_first == PD_PT(15));
        CHECK(para_resolved(d, find_para(d, "after the list")).indent_first == 0);
        CHECK(para_resolved(d, find_para(d, "An item")).indent_first != PD_PT(15));
        CHECK(para_resolved(d, find_para(d, "Centred")).indent_first == 0);
        CHECK(para_resolved(d, find_para(d, "Sixth")).indent_first == PD_PT(15));
        CHECK(para_resolved(d, find_para(d, "Seventh")).indent_first == PD_PT(20));
        CHECK(para_resolved(d, find_para(d, "Eighth")).indent_first == 0);
        CHECK(para_resolved(d, find_para(d, "Second")).space_before == 0);
        doc_text(d, all, sizeof(all));
        CHECK(strstr(all, "0pt") == NULL && strstr(all, "2em") == NULL);
        pd_doc_free(d);
        d = NULL;
    }

    CHECK(pd_doc_import(pk, sizeof(pk) - 1, PD_CONV_LATEX, &d) == PD_OK && d);

    if (d) {    /* parskip: no indents, a space between */
        CHECK(para_resolved(d, find_para(d, "Two")).indent_first == 0);
        CHECK(para_resolved(d, find_para(d, "Two")).space_before == PD_PT(6));
        pd_doc_free(d);
    }
}

static void test_latex_bibtex(void) {
    static const char bib[] =
        "@string{jbo = \"J. Biomed.\"}\n"
        "@comment{ @article{ghost, title={Not an entry}} }\n"
        "@article{zed, author = {Zed, Zoe and van der Berg, Jan}, title = {Last {GPU} Paper},\n"
        "  journal = jbo # \" Opt.\", volume = 17, number = {4}, pages = {1-5}, month = jul, year = 2012,\n"
        "  doi = {10.1117/1.JBO.17.4.047004}}\n"
        "@ARTICLE{Alpha, author = \"Ann Alpha and Bob Beta\", title = \"First?\", journal = {Optics}, year = {2001}}\n"
        "@inproceedings{many, author = {Gina Gamma and H. Hill and {\\\"O}zg{\\\"u}r Ilk}, title = {Talk},\n"
        "  crossref = {conf}, pages = {7--9}}\n"
        "@proceedings{conf, title = {Proc. SPIE Photonics}, year = 2020, publisher = {SPIE}}\n"
        "@misc{extra, title = {A Tool}, howpublished = {Software}, url = {https://x.org/a%20b}, year = {2023}}\n"
        "@book{unused, author = {No One}, title = {Never Cited}, publisher = {P}, year = 1999}\n";
    char tex[4096], all[8192];
    pd_doc* d = NULL;
    int n;

    /* the order cited (no style named: unsrt's) */
    n = snprintf(tex, sizeof(tex),
                 "\\documentclass{article}\\begin{document}\n"
                 "A \\cite{zed,Alpha} B \\citet{Alpha} C \\citeauthor{many} D \\cite[p.~2]{many} E \\cite{nokey}.\n"
                 "\\nocite{extra}\n\\begin{parade-bibtex}%s\n\\end{parade-bibtex}\n\\end{document}\n", bib);
    CHECK(pd_doc_import(tex, (size_t)n, PD_CONV_LATEX, &d) == PD_OK && d);

    if (d) {
        doc_text(d, all, sizeof(all));
        CHECK(strstr(all, "A [1, 2] B Alpha and Beta [2] C Gamma et\xC2\xA0" "al. D [3, p.\xC2\xA0" "2] E [nokey].|"));
        CHECK(strstr(all, "|References|[1] Zoe\xC2\xA0Zed and Jan\xC2\xA0van der Berg. Last GPU Paper. J. Biomed. Opt., "
                     "17(4):1\xE2\x80\x93" "5, July 2012. doi:") != NULL);
        CHECK(strstr(all, "\xEF\xBF\xBC" "10.1117/1.JBO.17.4.047004\xEF\xBF\xBC|") != NULL);    /* a link */
        CHECK(strstr(all, "|[2] Ann\xC2\xA0" "Alpha and Bob\xC2\xA0" "Beta. First? Optics, 2001.|") != NULL);
        CHECK(strstr(all, "|[3] Gina\xC2\xA0Gamma, H.\xC2\xA0Hill, and \xC3\x96zg\xC3\xBCr\xC2\xA0Ilk. Talk. In Proc. SPIE "
                     "Photonics, pages 7\xE2\x80\x93" "9, SPIE, 2020.|") != NULL);    /* crossref */
        CHECK(strstr(all, "|[4] A Tool. Software, 2023. \xEF\xBF\xBChttps://x.org/a%20b\xEF\xBF\xBC|") != NULL);    /* \nocite */
        CHECK(strstr(all, "Never Cited") == NULL && strstr(all, "ghost") == NULL && strstr(all, "Not an entry") == NULL);
        pd_doc_free(d);
        d = NULL;
    }

    /* abbrv: sorted by author, given names as initials */
    n = snprintf(tex, sizeof(tex),
                 "\\documentclass{article}\\begin{document}\n"
                 "A \\cite{zed,Alpha,many}.\n\\bibliographystyle{abbrv}\n"
                 "\\begin{parade-bibtex}%s\n\\end{parade-bibtex}\n\\end{document}\n", bib);
    CHECK(pd_doc_import(tex, (size_t)n, PD_CONV_LATEX, &d) == PD_OK && d);

    if (d) {
        doc_text(d, all, sizeof(all));
        CHECK(strstr(all, "A [3, 1, 2].|") != NULL);
        CHECK(strstr(all, "|[1] A.\xC2\xA0" "Alpha and B.\xC2\xA0" "Beta.") != NULL);
        CHECK(strstr(all, "|[2] G.\xC2\xA0Gamma, H.\xC2\xA0Hill, and \xC3\x96.\xC2\xA0Ilk.") != NULL);
        CHECK(strstr(all, "|[3] Z.\xC2\xA0Zed and J.\xC2\xA0van der Berg.") != NULL);
        pd_doc_free(d);
        d = NULL;
    }

    /* an entry with no type, listed with \nocite{*}: there, and an end to it (it once went round for ever) */
    {
        static const char odd[] =
            "\\documentclass{article}\\begin{document}\\nocite{*}\\begin{parade-bibtex}@{t, title={Typeless}}\n"
            "@article{u, title={Unclosed\n\\end{parade-bibtex}\\end{document}\n";

        CHECK(pd_doc_import(odd, sizeof(odd) - 1, PD_CONV_LATEX, &d) == PD_OK && d);

        if (d) {
            doc_text(d, all, sizeof(all));
            CHECK(strstr(all, "[1] Typeless.|") != NULL);
            pd_doc_free(d);
            d = NULL;
        }
    }

    /* a .bbl's list (natbib): \citet has the names it gives, \bibinfo its text */
    {
        static const char bbl[] =
            "\\documentclass{article}\\begin{document}\n\\citet{k} and \\citeyear{k}.\n"
            "\\begin{thebibliography}{1}\n\\providecommand{\\natexlab}[1]{#1}\n"
            "\\bibitem[Fang and Boas(2009)]{k}\n\\bibinfo{author}{Q.~Fang} and D.~Boas.\n\\newblock Monte Carlo."
            "\\BibitemShut{NoStop}\n\\end{thebibliography}\n\\end{document}\n";

        CHECK(pd_doc_import(bbl, sizeof(bbl) - 1, PD_CONV_LATEX, &d) == PD_OK && d);

        if (d) {
            doc_text(d, all, sizeof(all));
            CHECK(strstr(all, "Fang and Boas [1] and 2009.|") != NULL);
            CHECK(strstr(all, "[1] Q.\xC2\xA0" "Fang and D.\xC2\xA0" "Boas. Monte Carlo.|") != NULL);
            CHECK(strstr(all, "author") == NULL && strstr(all, "NoStop") == NULL);
            pd_doc_free(d);
        }
    }
}

static void test_latex_import(void) {
    static const char tex[] =
        "\\documentclass{article}\n"
        "\\usepackage{amsmath}\n"
        "\\newcommand{\\R}{\\mathbb{R}}\n"
        "\\newcommand{\\name}[1]{\\textbf{#1}}\n"
        "\\DeclareMathOperator{\\tr}{tr}\n"
        "\\title{A Test \\& More \\LaTeX{} too}\n\\author{Ann One \\and Bob Two}\n\\date{}\n"
        "\\begin{document}\n\\maketitle\n"
        "\\begin{abstract}Short.\\end{abstract}\n"
        "\\section{Intro}\\label{sec:intro}\n"
        "Caf\\'e na\\\"ive, ``quoted'' -- dash --- em~nbsp \\emph{it} \\name{bold} % a comment\n"
        "and $x\\in\\R$ and \\tr.\n\n"
        "See Section~\\ref{sec:two}, Fig.~\\ref{fig:a}, Eq.~\\eqref{eq:e} and \\cite{k2,k1}.\\footnote{A note.}\n"
        "\\subsection{Sub}\n"
        "\\begin{itemize}\\item First\\item Second\\end{itemize}\n"
        "\\begin{enumerate}\\item One\\label{it:one}\\end{enumerate}\n"
        "\\section{Two}\\label{sec:two}\n"
        "\\begin{equation}E=mc^2\\label{eq:e}\\end{equation}\n"
        "\\begin{figure}\\centering\\includegraphics[width=0.5\\textwidth]{pic.png}\\caption{A pic}\\label{fig:a}"
        "\\end{figure}\n"
        "\\begin{table}\\caption{Data}\\begin{tabular}{lcr}\\toprule A & B & C\\\\\\midrule 1 & 2 & 3\\\\"
        "\\multicolumn{2}{c}{wide} & 4\\\\\\bottomrule\\end{tabular}\\end{table}\n"
        "\\begin{verbatim}\ncode  line\n  two\n\\end{verbatim}\n"
        "\\href{https://example.com}{a link}\n"
        "\\begin{thebibliography}{9}\\bibitem{k1} First ref.\\bibitem{k2} Second ref.\\end{thebibliography}\n"
        "\\end{document}\n";
    pd_doc* d = NULL;
    pd_block_id p;
    char all[8192];
    size_t w = 0;
    int title = 0, h1 = 0, h2 = 0, list_items = 0, eq_display = 0, code_lines = 0, caption = 0, tables = 0;
    int found_eq_inline = 0, found_eq_display = 0, footnotes = 0, links = 0;

    CHECK(pd_doc_import(tex, sizeof(tex) - 1, PD_CONV_LATEX, &d) == PD_OK && d);

    if (!d) {
        return;
    }

    for (p = pd_doc_next_paragraph(d, 0); p; p = pd_doc_next_paragraph(d, p)) {
        pd_block_info bi;
        uint32_t n, k;
        const char* t = para_text(d, p, &n);

        if (pd_doc_block_info(d, p, &bi) != PD_OK) {
            continue;
        }

        title += bi.role == PD_ROLE_TITLE;
        h1 += bi.role == PD_ROLE_HEADING && bi.level == 1;
        h2 += bi.role == PD_ROLE_HEADING && bi.level == 2;
        caption += bi.role == PD_ROLE_CAPTION;
        code_lines += bi.role == PD_ROLE_CODE;
        eq_display += bi.role == PD_ROLE_EQUATION;
        list_items += bi.list_level >= 0 && bi.list != 0;

        for (k = 0; k + 3 <= n; k++) {
            if ((unsigned char)t[k] == 0xEF && (unsigned char)t[k + 1] == 0xBF && (unsigned char)t[k + 2] == 0xBC) {
                pd_inline o;

                if (pd_doc_inline_at(d, at(p, k), &o) == PD_OK) {
                    if (o.kind == PD_INLINE_EQUATION && o.source_len == 14 &&
                        !strncmp(o.source, "x\\in\\mathbb{R}", 14)) {
                        found_eq_inline = 1;
                    }

                    if (o.kind == PD_INLINE_EQUATION && bi.role == PD_ROLE_EQUATION &&
                        o.source_len == 6 && !strncmp(o.source, "E=mc^2", 6)) {
                        found_eq_display = 1;
                    }

                    footnotes += o.kind == PD_INLINE_FOOTNOTE;
                    links += o.kind == PD_INLINE_LINK && o.source_len > 0;
                }
            }
        }

        if (w + n + 2 < sizeof(all)) {
            memcpy(all + w, t, n);
            w += n;
            all[w++] = '|';
        }
    }

    all[w] = '\0';
    CHECK(title == 1 && strstr(all, "A Test & More LaTeX too|"));
    CHECK(strstr(all, "Ann One\nBob Two|") != NULL);
    CHECK(h1 == 3 && h2 == 1 && strstr(all, "1  Intro|") && strstr(all, "1.1  Sub|") && strstr(all, "2  Two|"));
    CHECK(strstr(all, "Caf\xC3\xA9 na\xC3\xAFve, \xE2\x80\x9Cquoted\xE2\x80\x9D \xE2\x80\x93 dash \xE2\x80\x94 em\xC2\xA0nbsp it bold and ") != NULL);
    CHECK(strstr(all, "See Section\xC2\xA0" "2, Fig.\xC2\xA0" "1, Eq.\xC2\xA0(1) and [2, 1].") != NULL);    /* forward refs */
    CHECK(found_eq_inline);         /* the document's \R expanded in the formula */
    CHECK(strstr(all, " and \\operatorname{tr}") == NULL);  /* \tr in text: not shown as source */
    CHECK(found_eq_display && eq_display == 1);
    CHECK(list_items == 3 && strstr(all, "First|Second|One|"));
    CHECK(caption == 2 && strstr(all, "Figure 1: A pic|") && strstr(all, "Table 1: Data|"));
    CHECK(code_lines == 2 && strstr(all, "|code  line|  two|"));
    CHECK(footnotes == 1 && links == 1 && strstr(all, "a link"));
    CHECK(strstr(all, "[1] First ref.|[2] Second ref.|") != NULL && strstr(all, "References|"));
    CHECK(strstr(all, "comment") == NULL && strstr(all, "usepackage") == NULL && strstr(all, "amsmath") == NULL);

    /* the table: three rows, the first the head; the merged cell spans two */
    {
        pd_block_id sec = pd_doc_child(d, pd_doc_root(d), 0), b, tbl = 0;
        pd_block_info si, bi;
        int32_t k;

        if (pd_doc_block_info(d, sec, &si) == PD_OK) {
            for (k = 0; k < si.child_count; k++) {
                b = pd_doc_child(d, sec, k);

                if (pd_doc_block_info(d, b, &bi) == PD_OK && bi.kind == PD_BLOCK_TABLE) {
                    tbl = b;
                    tables++;
                }
            }
        }

        CHECK(tables == 1);

        if (tbl) {
            pd_block_id r0 = pd_doc_child(d, tbl, 0), r2 = pd_doc_child(d, tbl, 2);
            pd_block_info ti, i0, i2;
            pd_cell_props cp;

            CHECK(pd_doc_block_info(d, tbl, &ti) == PD_OK && ti.child_count == 3);
            CHECK(pd_doc_block_info(d, r0, &i0) == PD_OK && i0.child_count == 3);
            CHECK(pd_doc_block_info(d, r2, &i2) == PD_OK && i2.child_count == 2);
            CHECK(pd_doc_cell_props(d, pd_doc_child(d, r2, 0), &cp) == PD_OK && cp.col_span == 2);
        }
    }

    pd_doc_free(d);
    /* a part of a document (no \begin{document}): all of it text */
    CHECK(pd_doc_import("\\section{Only}\nText here.", 25, PD_CONV_LATEX, &d) == PD_OK && d);

    if (d) {
        p = pd_doc_next_paragraph(d, 0);
        CHECK(text_is(d, p, "1  Only") && text_is(d, pd_doc_next_paragraph(d, p), "Text here."));
        pd_doc_free(d);
    }

    /* a macro that expands for ever: stopped, not hung */
    {
        static const char bomb[] = "\\def\\a{\\a\\a}\\a x";

        CHECK(pd_doc_import(bomb, sizeof(bomb) - 1, PD_CONV_LATEX, &d) == PD_OK && d);
        pd_doc_free(d);
    }
}

typedef struct {
    const unsigned char* all;
    int n, same, delay_sum, stop_after;
} gif_seen;

static int gif_each(void* user, int32_t index, const uint8_t* rgba, int32_t delay_ms) {
    gif_seen* v = (gif_seen*)user;

    v->same += memcmp(rgba, v->all + (size_t)index * 256, 256) == 0;
    v->delay_sum += delay_ms;
    v->n++;
    return v->stop_after && v->n >= v->stop_after;
}

/* an animated GIF's frames as they are shown: kept, cleared to see-through, put back as they were (disposals 1, 2,
   3), see-through pixels over the frame before, interlaced; the delays (0 shown as 100 ms); a run of them from the
   middle the same as the same frames of the whole. What Pillow shows for the same file */
static void test_gif_frames(void) {
    FILE* fp = fopen("tests/data/anim.gif", "rb");
    unsigned char data[8192], all[4 * 8 * 8 * 4], part[2 * 8 * 8 * 4];
    size_t n = fp ? fread(data, 1, sizeof(data), fp) : 0;
    int32_t w = 0, h = 0, k = 0, loops = -2, delays[4], d2[2];
    static const struct { int f, x, y; uint32_t rgba; } want[] = {
        { 0, 0, 0, 0xFF0000FF }, { 0, 6, 6, 0xFF0000FF },
        { 1, 0, 0, 0x00FF00FF }, { 1, 6, 0, 0xFF0000FF }, { 1, 0, 6, 0x00FF00FF },
        { 2, 0, 0, 0 }, { 2, 6, 0, 0xFF0000FF }, { 2, 0, 6, 0x0000FFFF }, { 2, 7, 7, 0x0000FFFF },
        { 3, 0, 0, 0 }, { 3, 0, 6, 0 }, { 3, 6, 6, 0xFF0000FF }, { 3, 7, 7, 0x00FF00FF }
    };
    size_t i;

    if (fp) {
        fclose(fp);
    }

    CHECK(n > 100 && pd_gif_info(data, n, &w, &h, &k, &loops) == PD_OK && w == 8 && h == 8 && k == 4 && loops == 0);
    CHECK(pd_gif_info("GIF89", 5, &w, &h, &k, &loops) == PD_ERR_FORMAT);
    CHECK(pd_gif_frames(data, n, 0, 4, all, delays) == PD_OK);
    CHECK(delays[0] == 50 && delays[1] == 100 && delays[2] == 200 && delays[3] == 100);

    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const unsigned char* q = all + ((size_t)want[i].f * 64 + (size_t)want[i].y * 8 + (size_t)want[i].x) * 4;
        uint32_t got = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];

        if (want[i].rgba == 0) {
            got &= 0xFF;    /* see-through: its colour is no matter */
        }

        CHECK(got == want[i].rgba);
    }

    CHECK(pd_gif_frames(data, n, 2, 2, part, d2) == PD_OK && memcmp(part, all + 2 * 256, sizeof(part)) == 0 &&
          d2[0] == 200);
    CHECK(pd_gif_frames(data, n, 3, 2, part, NULL) == PD_ERR_RANGE);
    CHECK(pd_gif_frames(data, n / 2, 0, 1, all, NULL) == PD_OK);    /* cut short: the frames before the cut */
    /* in one pass: the same frames and delays */
    {
        gif_seen v;

        memset(&v, 0, sizeof(v));
        v.all = all;
        CHECK(pd_gif_each_frame(data, n, gif_each, &v) == PD_OK && v.n == 4 && v.same == 4 && v.delay_sum == 450);
        v.n = v.same = v.delay_sum = 0;
        v.stop_after = 2;
        CHECK(pd_gif_each_frame(data, n, gif_each, &v) == PD_OK && v.n == 2);   /* stopped */
    }
}

static void test_pptx_transparent_color(void) {
#define PKPIC(id, x, cc) "<p:pic><p:nvPicPr><p:cNvPr id=\"" id "\" name=\"P\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>" \
    "<p:blipFill><a:blip r:embed=\"rIdI\">" cc "</a:blip><a:stretch><a:fillRect/></a:stretch></p:blipFill>" \
    "<p:spPr><a:xfrm><a:off x=\"" x "\" y=\"0\"/><a:ext cx=\"1000000\" cy=\"500000\"/></a:xfrm>" \
    "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>"
#define PKCC "<a:clrChange><a:clrFrom><a:srgbClr val=\"FFFFFF\"><a:alpha val=\"100000\"/></a:srgbClr></a:clrFrom>" \
    "<a:clrTo><a:srgbClr val=\"FFFFFF\"><a:alpha val=\"100000\"/><a:alpha val=\"0\"/></a:srgbClr></a:clrTo></a:clrChange>"
    const char* rels = "<Relationships><Relationship Id=\"rIdI\" Target=\"../media/k.png\"/></Relationships>";
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", PKPIC("3", "0", PKCC) PKPIC("4", "2000000", "")),
                         "ppt/slides/_rels/slide1.xml.rels", rels, "ppt/media/k.png", "tests/data/key2.png", NULL);
    int white = 0;

    CHECK(d != NULL);
    CHECK(d && keyed_found(d, &white));     /* the keyed one: white see-through, red kept */
    CHECK(white);                           /* the other, the same file: as it was */
    pd_doc_free(d);
    /* without the clrChange: nothing made transparent */
    d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", PKPIC("3", "0", "")), "ppt/slides/_rels/slide1.xml.rels", rels,
                 "ppt/media/k.png", "tests/data/key2.png", NULL);
    CHECK(d && !keyed_found(d, &white) && white);
    pd_doc_free(d);
#undef PKPIC
#undef PKCC
}

/* an embedded object (a worksheet, here) drawn as the picture its frame keeps of it, from whichever alternative has
   one, where the frame is */
/* a slide whose text has a link, and whose picture has its SVG beside its PNG (PowerPoint's icons): its shapes kept as
   they came all the same -- a slide, not a page of drawings made again (and so the deck a presentation) */
static void test_pptx_kept_links(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("",
                         PSP("2", "<a:p><a:r><a:rPr lang=\"en-US\"><a:hlinkClick r:id=\"rIdL\"/></a:rPr><a:t>a link"
                             "</a:t></a:r></a:p>")
                         "<p:pic><p:nvPicPr><p:cNvPr id=\"3\" name=\"Icon\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>"
                         "<p:blipFill><a:blip r:embed=\"rIdI\"><a:extLst><a:ext uri=\"{96DAC541-7B7A-43D3-8B79-37D633B846F1}\">"
                         "<asvg:svgBlip xmlns=\"\" xmlns:asvg=\"http://schemas.microsoft.com/office/drawing/2016/SVG/main\" "
                         "r:embed=\"rIdS\"/></a:ext></a:extLst></a:blip><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
                         "<p:spPr><a:xfrm><a:off x=\"4000000\" y=\"0\"/><a:ext cx=\"1000000\" cy=\"1000000\"/></a:xfrm>"
                         "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>"),
                         "ppt/slides/_rels/slide1.xml.rels", "<Relationships><Relationship Id=\"rIdL\" "
                         "Target=\"https://example.com/\" TargetMode=\"External\"/><Relationship Id=\"rIdI\" "
                         "Target=\"../media/i.png\"/><Relationship Id=\"rIdS\" Target=\"../media/s.svg\"/></Relationships>",
                         "ppt/media/i.png", "tests/data/rgba.png", "ppt/media/s.svg", "tests/data/icon.svg", NULL);
    char* js = d ? slide_json(d, 0) : NULL;

    CHECK(js && strstr(js, "\"kind\":\"wpc\"") && strstr(js, "\"xml\":\"<wpc:wpc"));
    CHECK(js && strstr(js, "svgBlip") && strstr(js, "{\"img\":"));
    free(js);
    pd_doc_free(d);
}

static void test_pptx_ole(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("",
                         "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"60\" name=\"Ole\"/><p:cNvGraphicFramePr/><p:nvPr/>"
                         "</p:nvGraphicFramePr><p:xfrm><a:off x=\"254000\" y=\"508000\"/><a:ext cx=\"1270000\" "
                         "cy=\"635000\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/"
                         "presentationml/2006/ole\"><mc:AlternateContent xmlns:mc=\"mc\"><mc:Choice Requires=\"v\">"
                         "<p:oleObj spid=\"_x0000_s1\" name=\"Worksheet\" r:id=\"rIdO\" progId=\"Excel.Sheet.12\"><p:embed/>"
                         "</p:oleObj></mc:Choice><mc:Fallback><p:oleObj name=\"Worksheet\" r:id=\"rIdO\" "
                         "progId=\"Excel.Sheet.12\"><p:embed/><p:pic><p:nvPicPr><p:cNvPr id=\"0\" name=\"\"/><p:cNvPicPr/>"
                         "<p:nvPr/></p:nvPicPr><p:blipFill><a:blip r:embed=\"rIdI\"/><a:stretch><a:fillRect/></a:stretch>"
                         "</p:blipFill><p:spPr><a:xfrm><a:off x=\"254000\" y=\"508000\"/><a:ext cx=\"1270000\" "
                         "cy=\"635000\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>"
                         "</p:oleObj></mc:Fallback></mc:AlternateContent></a:graphicData></a:graphic></p:graphicFrame>"),
                         "ppt/slides/_rels/slide1.xml.rels", "<Relationships><Relationship Id=\"rIdI\" "
                         "Target=\"../media/i.png\"/><Relationship Id=\"rIdO\" Target=\"../embeddings/o.bin\"/></Relationships>",
                         "ppt/media/i.png", "tests/data/rgba.png", NULL);
    char* js = d ? slide_json(d, 0) : NULL;

    CHECK(js && strstr(js, "\"x\":1310720,\"y\":2621440,\"w\":6553600,\"h\":3276800,\"nv\":60}"));  /* 20, 40; 100 by 50;
                                                                                         the frame's id */
    free(js);
    pd_doc_free(d);
}

static pd_doc* pptx_again(const pd_doc* d, buf_t* out);

/* a chart on a slide: drawn where its frame is, its part and its workbook kept; one in a group where the group
   draws it. Written as a .pptx, each a graphic frame of the chart's part where it was */
static void test_pptx_chart(void) {
#define PCHART(id, x, y, cx, cy, rid) "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" id "\" name=\"Chart\"/>" \
    "<p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" x "\" y=\"" y "\"/><a:ext cx=\"" cx \
    "\" cy=\"" cy "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">" \
    "<c:chart xmlns:c=\"c\" r:id=\"" rid "\"/></a:graphicData></a:graphic></p:graphicFrame>"
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", PCHART("4", "1270000", "635000", "3810000", "2540000", "rIdC")
                         "<p:grpSp><p:nvGrpSpPr><p:cNvPr id=\"5\" name=\"G\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
                         "<p:grpSpPr><a:xfrm><a:off x=\"5080000\" y=\"3810000\"/><a:ext cx=\"2540000\" cy=\"1270000\"/>"
                         "<a:chOff x=\"100\" y=\"100\"/><a:chExt cx=\"1270000\" cy=\"635000\"/></a:xfrm></p:grpSpPr>"
                         PCHART("6", "100", "100", "1270000", "635000", "rIdC") "</p:grpSp>"),
                         "ppt/slides/_rels/slide1.xml.rels", "<Relationships><Relationship Id=\"rIdC\" "
                         "Target=\"../charts/chart3.xml\"/></Relationships>",
                         "ppt/charts/chart3.xml", "<c:chartSpace xmlns:c=\"c\" xmlns:a=\"a\" xmlns:r=\"r\"><c:chart><c:plotArea>"
                         "<c:barChart><c:barDir val=\"col\"/><c:ser><c:idx val=\"0\"/><c:val><c:numRef><c:numCache><c:ptCount "
                         "val=\"3\"/><c:pt idx=\"0\"><c:v>3</c:v></c:pt><c:pt idx=\"1\"><c:v>5</c:v></c:pt><c:pt idx=\"2\">"
                         "<c:v>2</c:v></c:pt></c:numCache></c:numRef></c:val></c:ser></c:barChart></c:plotArea></c:chart>"
                         "<c:externalData r:id=\"rId1\"/></c:chartSpace>",
                         "ppt/charts/_rels/chart3.xml.rels", "<Relationships><Relationship Id=\"rId1\" "
                         "Target=\"../embeddings/Book1.xlsx\"/></Relationships>",
                         "ppt/embeddings/Book1.xlsx", "PK-not-really", NULL);
    pd_block_id sec = d ? pd_doc_child(d, pd_doc_root(d), 0) : 0;
    int k;

    CHECK(d != NULL);

    for (k = 0; d && k < 2; k++) {  /* the first at (100, 50) points, 300 by 200; the second at (400, 300), 200 by 100 */
        pd_block_id fl = pd_doc_child(d, sec, 1 + k);
        pd_float_props fp;
        pd_inline o;
        char* js = NULL;

        memset(&o, 0, sizeof(o));
        CHECK(pd_doc_float_props(d, fl, &fp) == PD_OK && fp.offset_x == PD_PT(k ? 400 : 100) &&
              fp.offset_y == PD_PT(k ? 300 : 50) && fp.offset_from == PD_FROM_PAGE);

        if (pd_doc_inline_at(d, at(pd_doc_child(d, fl, 0), 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE) {
            js = drawing_json(d, o.resource);
        }

        CHECK(o.width == PD_PT(k ? 200 : 300) && o.height == PD_PT(k ? 100 : 200));
        CHECK(js && strstr(js, "\"chart\":") && strstr(js, "\"data\":") && count_of(js, "\"shape\":\"rect\"") >= 3);
        free(js);
    }

    if (d) {    /* written as a .pptx: each chart a frame of its part (and its workbook), where it was, read again */
        buf_t b;
        pd_doc* back = pptx_again(d, &b);
        size_t n = 0;
        char* part = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide1.xml", &n) : NULL;
        char* rels = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/charts/_rels/chart1.xml.rels", &n) :
                     NULL;
        char* xlsx = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n,
                                             "ppt/embeddings/Microsoft_Excel_Worksheet1.xlsx", &n) : NULL;

        CHECK(part && count_of(part, "<c:chart ") == 2 && strstr(part, "<a:off x=\"1270000\" y=\"635000\"/>"));
        CHECK(rels && strstr(rels, "Target=\"../embeddings/Microsoft_Excel_Worksheet1.xlsx\"") && xlsx &&
              n == 13 && !memcmp(xlsx, "PK-not-really", 13));
        CHECK(back != NULL);

        for (k = 0; back && k < 2; k++) {
            pd_block_id fl = pd_doc_child(back, pd_doc_child(back, pd_doc_root(back), 0), 1 + k);
            pd_float_props fp;
            pd_inline o;
            char* js = NULL;

            memset(&o, 0, sizeof(o));
            CHECK(pd_doc_float_props(back, fl, &fp) == PD_OK && llabs(fp.offset_x - PD_PT(k ? 400 : 100)) <= PD_PT(1) &&
                  llabs(fp.offset_y - PD_PT(k ? 300 : 50)) <= PD_PT(1) && fp.offset_from == PD_FROM_PAGE);

            if (pd_doc_inline_at(back, at(pd_doc_child(back, fl, 0), 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE) {
                js = drawing_json(back, o.resource);
            }

            CHECK(llabs(o.width - PD_PT(k ? 200 : 300)) <= PD_PT(1) && llabs(o.height - PD_PT(k ? 100 : 200)) <= PD_PT(1));
            CHECK(js && strstr(js, "\"chart\":") && strstr(js, "\"data\":") && count_of(js, "\"shape\":\"rect\"") >= 3);
            free(js);
        }

        free(part);
        free(rels);
        free(xlsx);
        free(b.p);
        pd_doc_free(back);
    }

    pd_doc_free(d);
#undef PCHART
}

/* a placeholder with nothing of its own takes its layout placeholder's geometry, fill, outline and text box (its warp,
   its columns), and its master placeholder's style (its text's colour from the style's font) */
static void test_pptx_ph_inherit(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"B\"/><p:cNvSpPr/>"
                         "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/>"
                         PP("Inherits") "</p:txBody></p:sp>"),
                         "ppt/slides/_rels/slide1.xml.rels", "<Relationships><Relationship Id=\"rId1\" "
                         "Target=\"../slideLayouts/slideLayout1.xml\"/></Relationships>",
                         "ppt/slideLayouts/slideLayout1.xml", "<p:sldLayout xmlns:p=\"p\" xmlns:a=\"a\"><p:cSld><p:spTree>"
                         "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"B\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"body\" idx=\"1\"/>"
                         "</p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"1270000\" y=\"1270000\"/><a:ext cx=\"2540000\" "
                         "cy=\"1270000\"/></a:xfrm><a:prstGeom prst=\"ellipse\"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr "
                         "val=\"FF0000\"/></a:solidFill><a:ln w=\"25400\"><a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill>"
                         "</a:ln></p:spPr><p:txBody><a:bodyPr numCol=\"2\"><a:prstTxWarp prst=\"textArchUp\"><a:avLst/>"
                         "</a:prstTxWarp></a:bodyPr><a:lstStyle/><a:p/></p:txBody></p:sp></p:spTree></p:cSld></p:sldLayout>",
                         "ppt/slideLayouts/_rels/slideLayout1.xml.rels", "<Relationships><Relationship Id=\"rId1\" "
                         "Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>",
                         "ppt/slideMasters/slideMaster1.xml", "<p:sldMaster xmlns:p=\"p\" xmlns:a=\"a\"><p:cSld><p:spTree>"
                         "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"B\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"body\" idx=\"1\"/>"
                         "</p:nvPr></p:nvSpPr><p:spPr/><p:style><a:lnRef idx=\"0\"><a:srgbClr val=\"000000\"/></a:lnRef>"
                         "<a:fillRef idx=\"0\"><a:srgbClr val=\"000000\"/></a:fillRef><a:effectRef idx=\"0\"><a:srgbClr "
                         "val=\"000000\"/></a:effectRef><a:fontRef idx=\"minor\"><a:srgbClr val=\"0000FF\"/></a:fontRef>"
                         "</p:style><p:txBody><a:bodyPr/><a:lstStyle/><a:p/></p:txBody></p:sp></p:spTree></p:cSld></p:sldMaster>",
                         NULL);
    char* js = d ? slide_json(d, 0) : NULL;
    pd_block_id p = d ? story_para_with(d, "Inherits") : 0;

    CHECK(js && strstr(js, "\"shape\":\"ellipse\",\"x\":6553600,\"y\":6553600,\"w\":13107200,\"h\":6553600,"
                       "\"fill\":4294901760,\"line\":4278255360,\"lw\":131072"));
    CHECK(js && strstr(js, "numCol=\\\"2\\\"") && strstr(js, "<a:prstTxWarp prst=\\\"textArchUp\\\">"));
    CHECK(p && (chars_at(d, p, 0).color & 0xFFFFFFu) == 0x0000FFu);
    free(js);
    pd_doc_free(d);
}

/* a run's capitals and small capitals, its letter spacing, its East Asian face, its highlight; a link to an address
   a link of the document's */
static void test_pptx_run_props(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", PSP("2",
                         "<a:p><a:r><a:rPr sz=\"2000\" cap=\"all\" spc=\"300\"/><a:t>Start </a:t></a:r>"
                         "<a:r><a:rPr sz=\"2000\" cap=\"small\"><a:hlinkClick r:id=\"rIdL\"/></a:rPr><a:t>linked</a:t></a:r>"
                         "<a:r><a:rPr sz=\"2000\"><a:highlight><a:srgbClr val=\"FFFF00\"/></a:highlight>"
                         "<a:ea typeface=\"Noto Sans CJK JP\"/></a:rPr><a:t>\xE6\x97\xA5\xE6\x9C\xAC</a:t></a:r></a:p>")),
                         "ppt/slides/_rels/slide1.xml.rels", "<Relationships><Relationship Id=\"rIdL\" "
                         "Target=\"https://example.com/x?a=1&amp;b=2\" TargetMode=\"External\"/></Relationships>", NULL);
    pd_block_id p = d ? story_para_with(d, "Start") : 0;
    const char* t;
    uint32_t n = 0, i, ln = 0, ja = 0;
    pd_char_props a, b, c;
    pd_inline o;

    CHECK(p != 0);

    if (!p) {
        pd_doc_free(d);
        return;
    }

    pd_doc_para_text(d, p, &t, &n);

    for (i = 0; i + 2 < n; i++) {
        ln = !ln && t[i] == 'l' && !memcmp(t + i, "linked", 6) ? i : ln;
        ja = !ja && (unsigned char)t[i] == 0xE6 ? i : ja;
    }

    a = chars_at(d, p, 0);
    b = chars_at(d, p, ln);
    c = chars_at(d, p, ja);
    CHECK(a.caps && !a.small_caps && a.letter_space == PD_PT(3));
    CHECK(ln && b.small_caps && !b.caps && b.letter_space == 0);
    CHECK(ja && (c.background & 0xFFFFFFu) == 0xFFFF00u && !strcmp(c.family_ea, "Noto Sans CJK JP") &&
          (b.background & 0xFFFFFFu) == 0);
    CHECK(object_in(d, p, PD_INLINE_LINK, &o) && o.source_len == 29 && !memcmp(o.source, "https://example.com/x?a=1&b=2",
            29));
    pd_doc_free(d);
}

/* an equation (Office Math in a14:m, in the Choice of an mc:AlternateContent that asks for a14) read as one, not its
   fallback */
static void test_pptx_math(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", "<mc:AlternateContent xmlns:mc=\"mc\"><mc:Choice "
                         "xmlns:a14=\"a14\" Requires=\"a14\">" PSP("2", "<a:p><a14:m><m:oMathPara xmlns:m=\"m\"><m:oMath>"
                                 "<m:f><m:num><m:r><m:t>a</m:t></m:r></m:num><m:den><m:r><m:t>b</m:t></m:r></m:den></m:f>"
                                 "</m:oMath></m:oMathPara></a14:m></a:p>") "</mc:Choice><mc:Fallback>"
                         PSP("2", PP("MATHFALLBACK")) "</mc:Fallback></mc:AlternateContent>"), NULL);
    int32_t i;
    int found = 0;
    pd_inline o;

    CHECK(d != NULL);

    for (i = 0; d && i < pd_doc_story_count(d); i++) {
        pd_block_id p = pd_doc_child(d, pd_doc_story_at(d, i), 0);

        if (object_in(d, p, PD_INLINE_EQUATION, &o) && o.source && strstr(o.source, "\\frac{a}{b}")) {
            found = 1;
            CHECK(chars_at(d, p, 0).size == PD_PT(18));     /* PowerPoint's size, not Word's */
        }
    }

    CHECK(found && !story_para_with(d, "MATHFALLBACK"));
    pd_doc_free(d);
}

/* a table style's first and last columns and its column bands; the edges its tcBdr gives, the inside ones between
   rows, and a cell's own over them */
static void test_pptx_table_style(void) {
#define PTC(t, pr) "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>" t "</a:t></a:r></a:p></a:txBody><a:tcPr>" \
    pr "</a:tcPr></a:tc>"
#define PROW(a, b, c, e) "<a:tr h=\"370000\">" PTC(a, "") PTC(b, e) PTC(c, "") PTC("z", "") "</a:tr>"
#define PFILL(part, rgb, more) "<a:" part ">" more "<a:tcStyle><a:fill><a:solidFill><a:srgbClr val=\"" rgb \
    "\"/></a:solidFill></a:fill></a:tcStyle></a:" part ">"
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"4\" "
                         "name=\"T\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"0\" y=\"0\"/>"
                         "<a:ext cx=\"4000000\" cy=\"1110000\"/></p:xfrm><a:graphic><a:graphicData uri=\"t\"><a:tbl><a:tblPr "
                         "firstCol=\"1\" lastCol=\"1\" bandCol=\"1\"><a:tableStyleId>{S}</a:tableStyleId></a:tblPr><a:tblGrid>"
                         "<a:gridCol w=\"1000000\"/><a:gridCol w=\"1000000\"/><a:gridCol w=\"1000000\"/><a:gridCol "
                         "w=\"1000000\"/></a:tblGrid>" PROW("a", "b", "c", "") PROW("d", "e", "f", "<a:lnB w=\"25400\">"
                                 "<a:solidFill><a:srgbClr val=\"00FF00\"/></a:solidFill></a:lnB>") PROW("g", "h", "i", "")
                         "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>"),
                         "ppt/tableStyles.xml", "<a:tblStyleLst xmlns:a=\"a\"><a:tblStyle styleId=\"{S}\"><a:wholeTbl>"
                         "<a:tcStyle><a:tcBdr><a:insideH><a:ln w=\"12700\"><a:solidFill><a:srgbClr val=\"FF0000\"/>"
                         "</a:solidFill></a:ln></a:insideH></a:tcBdr><a:fill><a:solidFill><a:srgbClr val=\"111111\"/>"
                         "</a:solidFill></a:fill></a:tcStyle></a:wholeTbl>" PFILL("band1V", "444444", "") PFILL("band2V", "555555", "")
                         PFILL("firstCol", "222222", "<a:tcTxStyle b=\"on\"/>") PFILL("lastCol", "333333", "")
                         "</a:tblStyle></a:tblStyleLst>", NULL);
    char* js = d ? slide_json(d, 0) : NULL;
    pd_block_id t = 0;
    int32_t i;

    CHECK(js && count_of(js, "\"fill\":4280427042") == 3 && count_of(js, "\"fill\":4282664004") == 3 &&
          count_of(js, "\"fill\":4283782485") == 3 && count_of(js, "\"fill\":4281545523") == 3 &&
          count_of(js, "\"fill\":4279308561") == 0);  /* first, band 1, band 2, last; nothing the whole table's */

    for (i = 0; d && i < pd_doc_story_count(d) && !t; i++) {
        pd_block_info bi;
        pd_block_id b = pd_doc_child(d, pd_doc_story_at(d, i), 0);

        if (pd_doc_block_info(d, b, &bi) == PD_OK && bi.kind == PD_BLOCK_TABLE) {
            t = b;
        }
    }

    CHECK(t != 0);

    if (t) {
        pd_cell_props a = cell_props_at(d, t, 0, 0), e = cell_props_at(d, t, 1, 1);

        /* a ruled below and not above (the style has no top edge); e: its own 2 points below, the inside one above */
        CHECK((a.border_on & PD_BORDER_BOTTOM) && !(a.border_on & PD_BORDER_TOP));
        CHECK((e.border_on & (PD_BORDER_BOTTOM | PD_BORDER_TOP)) == (PD_BORDER_BOTTOM | PD_BORDER_TOP) &&
              (e.edge_width[2] ? e.edge_width[2] : e.border_width) == PD_PT(2) && e.edge_width[0] == PD_PT(1));
        CHECK(chars_at(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 0), 0), 0), 0).weight >= 700 &&
              chars_at(d, pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, 0), 1), 0), 0).weight < 700);
    }

    free(js);
    pd_doc_free(d);
#undef PTC
#undef PROW
#undef PFILL
}

/* a slide whose master's theme is not the document's (the first master's): its shapes' style references are to its
   own theme's fills and lines */
static void test_pptx_other_theme(void) {
    pd_doc* d = pptx_doc(1, "ppt/slides/slide1.xml", PSLD("", "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"S\"/><p:cNvSpPr/>"
                         "<p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"1270000\" cy=\"1270000\"/>"
                         "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr><p:style><a:lnRef idx=\"2\">"
                         "<a:srgbClr val=\"00FF00\"/></a:lnRef><a:fillRef idx=\"2\"><a:srgbClr val=\"FF0000\"/></a:fillRef>"
                         "<a:effectRef idx=\"0\"><a:srgbClr val=\"000000\"/></a:effectRef><a:fontRef idx=\"minor\">"
                         "<a:srgbClr val=\"000000\"/></a:fontRef></p:style></p:sp>"),
                         "ppt/slides/_rels/slide1.xml.rels", "<Relationships><Relationship Id=\"rId1\" "
                         "Target=\"../slideLayouts/slideLayout1.xml\"/></Relationships>",
                         "ppt/slideLayouts/slideLayout1.xml", "<p:sldLayout xmlns:p=\"p\"><p:cSld><p:spTree/></p:cSld>"
                         "</p:sldLayout>",
                         "ppt/slideLayouts/_rels/slideLayout1.xml.rels", "<Relationships><Relationship Id=\"rId1\" "
                         "Target=\"../slideMasters/slideMaster2.xml\"/></Relationships>",
                         "ppt/slideMasters/slideMaster2.xml", "<p:sldMaster xmlns:p=\"p\"><p:cSld><p:spTree/></p:cSld>"
                         "</p:sldMaster>",
                         "ppt/slideMasters/_rels/slideMaster2.xml.rels", "<Relationships><Relationship Id=\"rId1\" "
                         "Target=\"../theme/theme2.xml\"/></Relationships>",
                         "ppt/theme/theme2.xml", "<a:theme xmlns:a=\"a\"><a:themeElements><a:fmtScheme><a:fillStyleLst>"
                         "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill><a:solidFill><a:srgbClr val=\"0000FF\"/>"
                         "</a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w=\"9525\"><a:solidFill><a:schemeClr "
                         "val=\"phClr\"/></a:solidFill></a:ln><a:ln w=\"50800\"><a:solidFill><a:schemeClr val=\"phClr\"/>"
                         "</a:solidFill></a:ln></a:lnStyleLst></a:fmtScheme></a:themeElements></a:theme>", NULL);
    char* js = d ? slide_json(d, 0) : NULL;

    /* its own blue, the line 4 points in the reference's green */
    CHECK(js && strstr(js, "\"fill\":4278190335,\"line\":4278255360,\"lw\":262144"));
    free(js);
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

/* ---- PowerPoint written ---- */

static pd_doc* read_doc(const char* path, pd_conv_format fmt) {
    FILE* f = fopen(path, "rb");
    unsigned char* data = NULL;
    long sz = 0;
    pd_doc* d = NULL;

    if (f && fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
            (data = (unsigned char*)malloc((size_t)sz)) != NULL && fread(data, 1, (size_t)sz, f) != (size_t)sz) {
        sz = 0;
    }

    if (f) {
        fclose(f);
    }

    if (data && sz > 0 && pd_doc_import(data, (size_t)sz, fmt, &d) != PD_OK) {
        d = NULL;
    }

    free(data);
    return d;
}

/* a document written as a .pptx and read back */
static pd_doc* pptx_again(const pd_doc* d, buf_t* out) {
    pd_doc* back = NULL;

    memset(out, 0, sizeof(*out));

    if (pd_doc_export(d, PD_CONV_PPTX, to_buf, out) != PD_OK || !out->p ||
            pd_doc_import(out->p, out->n, PD_CONV_PPTX, &back) != PD_OK) {
        return NULL;
    }

    return back;
}

/* a block's text, paragraphs (and cells) by '|', into o */
static void block_text(const pd_doc* d, pd_block_id b, char* o, size_t cap) {
    pd_block_info bi;
    int32_t k;

    if (pd_doc_block_info(d, b, &bi) != PD_OK) {
        return;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        uint32_t n;
        const char* t = para_text(d, b, &n);
        size_t at = strlen(o);

        snprintf(o + at, cap - at, "%.*s|", (int)n, t);
        return;
    }

    for (k = 0; k < bi.child_count; k++) {
        block_text(d, pd_doc_child(d, b, k), o, cap);
    }
}

/* what a page that is a canvas shows: each item's box (sp) and kind, and its text boxes' text in their order */
typedef struct {
    pd_sp w, h;
    int nitems, npics;
    long long box[256][4];
    char kind[256];
    char text[4096];
} slide_sum;

static int slide_of(const pd_doc* d, int k, slide_sum* s) {
    pd_block_id sec = pd_doc_child(d, pd_doc_root(d), k), fl = pd_doc_child(d, sec, 0);
    pd_section_props sp;
    pd_inline o;
    char* js;
    const char* p;

    memset(s, 0, sizeof(*s));

    if (pd_doc_section_props(d, sec, &sp) != PD_OK ||
            pd_doc_inline_at(d, at(pd_doc_child(d, fl, 0), 0), &o) != PD_OK || !(js = drawing_json(d, o.resource))) {
        return 0;
    }

    s->w = sp.page_width;
    s->h = sp.page_height;
    p = strstr(js, "\"items\":[");

    if (strstr(js, "\"xml\":")) {     /* the items alone, not the braces in the XML kept */
        *strstr(js, "\"xml\":") = '\0';
    }

    while (p && (p = strchr(p + 1, '{')) != NULL && s->nitems < 256) {
        const char* e = strchr(p, '}');
        long long x = 0, y = 0, w = 0, h = 0;
        long st = 0;
        char kind = 0;

        if (!e) {
            break;
        }

        if (!strncmp(p, "{\"sid\"", 6) || !strncmp(p, "{\"shape\":\"rect\",\"x\":0,\"y\":0,", 25)) {
            /* an index, not a thing drawn; the page's background */
            p = e;
            continue;
        }

        kind = !strncmp(p, "{\"shape\"", 8) ? 's' : !strncmp(p, "{\"img\"", 6) ? 'i' : !strncmp(p, "{\"path\"", 7) ?
               'p' : !strncmp(p, "{\"story\"", 8) ? 't' : '?';

        if (kind == 'p') {
            sscanf(p, "{\"path\":[%lld,%lld", &x, &y);
        } else {
            const char* q = strstr(p, "\"x\":");

            if (q && q < e) {
                sscanf(q, "\"x\":%lld,\"y\":%lld,\"w\":%lld,\"h\":%lld", &x, &y, &w, &h);
            }
        }

        if (kind == 't' && sscanf(p, "{\"story\":%ld", &st) == 1) {
            block_text(d, (pd_block_id)st, s->text, sizeof(s->text));
            strncat(s->text, "/", sizeof(s->text) - strlen(s->text) - 1);
        }

        s->npics += kind == 'i';
        s->kind[s->nitems] = kind;
        s->box[s->nitems][0] = x;
        s->box[s->nitems][1] = y;
        s->box[s->nitems][2] = w;
        s->box[s->nitems][3] = h;
        s->nitems++;
        p = kind == 'p' ? strchr(p, ']') : e;   /* a path's points: past them */
    }

    free(js);
    return 1;
}

/* two documents' slides the same: as many, as big, the same things where they were (within a point), the same text
   in the same order, as many pictures */
static int slides_same(const pd_doc* a, const pd_doc* b) {
    pd_block_info ra, rb;
    int k, i, q, same = 1;
    static slide_sum sa, sb;

    if (pd_doc_block_info(a, pd_doc_root(a), &ra) != PD_OK || pd_doc_block_info(b, pd_doc_root(b), &rb) != PD_OK ||
            ra.child_count != rb.child_count) {
        fprintf(stderr, "slides: %d, back %d\n", ra.child_count, rb.child_count);
        return 0;
    }

    for (k = 0; k < ra.child_count; k++) {
        if (!slide_of(a, k, &sa) || !slide_of(b, k, &sb)) {
            return 0;
        }

        if (sa.w != sb.w || sa.h != sb.h || sa.nitems != sb.nitems || sa.npics != sb.npics || strcmp(sa.text, sb.text)) {
            fprintf(stderr, "slide %d: %dx%d %d items %d pictures \"%s\"; back %dx%d %d %d \"%s\"\n", k + 1, (int)sa.w,
                    (int)sa.h, sa.nitems, sa.npics, sa.text, (int)sb.w, (int)sb.h, sb.nitems, sb.npics, sb.text);
            return 0;
        }

        for (i = 0; i < sa.nitems; i++) {
            for (q = 0; q < 4; q++) {
                if (sa.kind[i] != sb.kind[i] || llabs(sa.box[i][q] - sb.box[i][q]) > PD_PT(1)) {
                    fprintf(stderr, "slide %d item %d (%c): %lld, back %lld (%c)\n", k + 1, i, sa.kind[i], sa.box[i][q],
                            sb.box[i][q], sb.kind[i]);
                    same = 0;
                    break;
                }
            }
        }
    }

    return same;
}

/* A presentation read, written as a .pptx and read again comes back the same: its slides as many and as big, every
   shape, picture and text box where it was, the text in the same order; the same bytes each time it is written */
static void test_pptx_write_roundtrip(void) {
    static const char* const files[] = { "tests/data/slides.pptx", "tests/data/smartart.pptx" };
    int f;

    for (f = 0; f < 2; f++) {
        pd_doc* d = read_doc(files[f], PD_CONV_PPTX), *back;
        buf_t b, b2;

        CHECK(d != NULL);

        if (!d) {
            continue;
        }

        back = pptx_again(d, &b);
        CHECK(back != NULL && slides_same(d, back));
        CHECK(b.p && b.n > 4 && !memcmp(b.p, "PK", 2));
        memset(&b2, 0, sizeof(b2));
        CHECK(pd_doc_export(d, PD_CONV_PPTX, to_buf, &b2) == PD_OK && b2.n == b.n && !memcmp(b.p, b2.p, b.n));

        if (f == 0 && b.p) {    /* PowerPoint's own: shapes, a connector between two of them, a picture, a table */
            size_t n = 0;
            char* s1 = (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide1.xml", &n), *s2;

            CHECK(s1 && strstr(s1, "<p:cxnSp>") && strstr(s1, "<a:stCxn id=\"5\" idx=\"3\"/><a:endCxn id=\"6\" idx=\"2\"/>"));
            CHECK(s1 && strstr(s1, "<p:cNvPr id=\"5\" name=\"Box\"/>") && strstr(s1, "<p:cNvPr id=\"6\" name=\"Oval\"/>"));
            /* the title placeholder a placeholder again; the shapes not one plain */
            CHECK(s1 && strstr(s1, "name=\"Title 1\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr>") &&
                  strstr(s1, "name=\"Box\"/><p:cNvSpPr/><p:nvPr/>"));
            CHECK(s1 && strstr(s1, "<a:buChar char=\"\xE2\x80\xA2\"/></a:pPr><a:r>") && strstr(s1, "<a:t>First point</a:t>"));
            free(s1);
            s2 = (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide2.xml", &n);
            CHECK(s2 && strstr(s2, "<p:pic>") && strstr(s2, "<a:blip r:embed=\"rId2\"/>") && strstr(s2, "<a:tbl>"));
            free(s2);
            s2 = (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/_rels/slide2.xml.rels", &n);
            CHECK(s2 && strstr(s2, "Target=\"../media/pdimage1.png\""));
            free(s2);
            /* on its own master and layout again: the master's band drawn by the master, not on the slide */
            s2 = (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/_rels/slide1.xml.rels", &n);
            CHECK(s2 && strstr(s2, "Target=\"../slideLayouts/slideLayout1.xml\""));
            free(s2);
            s2 = (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide1.xml", &n);
            CHECK(s2 && !strstr(s2, "name=\"Band\"") && strstr(s2, "name=\"Title 1\""));
            free(s2);
            s2 = (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slideMasters/slideMaster1.xml", &n);
            CHECK(s2 && strstr(s2, "name=\"Band\""));
            free(s2);
            s2 = (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/presentation.xml", &n);
            CHECK(s2 && strstr(s2, "<p:sldSz cx=\"9144000\" cy=\"5143500\"/>"));
            free(s2);
        }

        if (f == 0) {   /* the placeholders marked in the slides' drawings; written as a .docx, read the same */
            buf_t x;
            pd_doc* dd = NULL;
            pd_inline o;
            char* js = NULL, *docxml;
            size_t n = 0;

            memset(&x, 0, sizeof(x));
            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &x) == PD_OK && x.p);
            docxml = x.p ? (char*)pd_zip_get((const unsigned char*)x.p, x.n, "word/document.xml", &n) : NULL;
            CHECK(docxml && strstr(docxml, "xmlns:pd=\"urn:parade:pptx\"") &&
                  strstr(docxml, "<pd:sp xmlns:pd=\"urn:parade:pptx\" from=\"slide\" ph=\"title\"/>") &&
                  strstr(docxml, "from=\"master\"/>"));
            CHECK(x.p && pd_doc_import(x.p, x.n, PD_CONV_DOCX, &dd) == PD_OK && dd && slides_same(d, dd));

            if (back && pd_doc_inline_at(back, at(pd_doc_child(back, pd_doc_child(back, pd_doc_child(back,
                                         pd_doc_root(back), 0), 0), 0), 0), &o) == PD_OK) {
                js = drawing_json(back, o.resource);
            }

            CHECK(js && strstr(js, "from=\\\"slide\\\" ph=\\\"title\\\""));
            free(js);

            {   /* the document's theme changed: the master's theme part as it is now */
                pd_theme th;
                buf_t t;
                char* part;

                pd_doc_theme(d, &th);
                th.color[PD_THEME_ACCENT1] = 0xFF123456u;
                memset(&t, 0, sizeof(t));
                CHECK(pd_doc_set_theme(d, &th) == PD_OK && pd_doc_export(d, PD_CONV_PPTX, to_buf, &t) == PD_OK);
                part = t.p ? (char*)pd_zip_get((const unsigned char*)t.p, t.n, "ppt/theme/theme1.xml", &n) : NULL;
                CHECK(part && strstr(part, "<a:accent1><a:srgbClr val=\"123456\"/>"));
                free(part);
                free(t.p);
            }
            free(docxml);
            free(x.p);
            pd_doc_free(dd);
        }

        free(b.p);
        free(b2.p);
        pd_doc_free(back);
        pd_doc_free(d);
    }
}

/* Slides an editor made (a page made a canvas, its shapes and a text box in the DrawingML kept with it, the text
   box's text edited after) written as a .pptx: read back, the shapes where they were and the text as edited; and a
   document of plain text, each section a slide of one text box with its text */
static void test_pptx_write_made(void) {
    static const char xml[] = "<wpc:wpc><wpc:bg/><wpc:whole/><wps:wsp><wps:cNvPr id=\"2\" name=\"Rectangle 2\"/>"
                              "<wps:cNvSpPr/><wps:spPr><a:xfrm><a:off x=\"914400\" y=\"457200\"/><a:ext cx=\"1828800\" "
                              "cy=\"914400\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr "
                              "val=\"FF0000\"/></a:solidFill></wps:spPr><wps:bodyPr/></wps:wsp><wps:wsp><wps:cNvPr id=\"3\" "
                              "name=\"Text Box 3\"/><wps:cNvSpPr txBox=\"1\"/><wps:spPr><a:xfrm><a:off x=\"914400\" y=\"1828800\"/>"
                              "<a:ext cx=\"3657600\" cy=\"914400\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
                              "<a:noFill/></wps:spPr><wps:txbx><w:txbxContent><w:p><w:r><w:t>Draft</w:t></w:r></w:p>"
                              "</w:txbxContent></wps:txbx><wps:bodyPr/></wps:wsp></wpc:wpc>";
    pd_doc* d = NULL, *back;
    pd_block_id sec, fl, para, story = 0;
    pd_section_props sp;
    pd_float_props fp;
    pd_inline o;
    pd_res_id r0, r1 = 0;
    char json[2048];
    buf_t b;
    int32_t k;

    CHECK(pd_doc_new(&d) == PD_OK);

    if (!d) {
        return;
    }

    /* as the editor's StartCanvasPage: a landscape page with no margins, a canvas in front of it from its corner */
    sec = pd_doc_child(d, pd_doc_root(d), 0);
    pd_doc_section_props(d, sec, &sp);
    sp.page_width = PD_PT(720);
    sp.page_height = PD_PT(540);
    sp.margin_left = sp.margin_right = sp.margin_top = sp.margin_bottom = 0;
    pd_doc_set_section_props(d, sec, &sp);
    snprintf(json, sizeof(json), "{\"w\":%d,\"h\":%d,\"items\":[],\"kind\":\"wpc\",\"rels\":{},\"xml\":\"<wpc:wpc><wpc:bg/>"
             "<wpc:whole/></wpc:wpc>\"}", (int)sp.page_width, (int)sp.page_height);
    CHECK(pd_doc_add_resource(d, "application/vnd.parade.drawing+json", json, strlen(json), &r0) == PD_OK);
    CHECK(pd_docx_drawing_rebuild(d, r0, xml, strlen(xml), &r1) == PD_OK && r1);
    pd_doc_insert_block(d, sec, 0, PD_BLOCK_FLOAT, &fl);
    pd_doc_float_props(d, fl, &fp);
    fp.placement = PD_PLACE_HERE | PD_PLACE_FORCE | PD_PLACE_OFFSET;
    fp.wrap = PD_WRAP_FRONT;
    fp.width = sp.page_width;
    fp.gap = fp.offset_x = fp.offset_y = 0;
    fp.offset_from = PD_FROM_PAGE;
    pd_doc_set_float_props(d, fl, &fp);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.resource = r1;
    o.width = sp.page_width;
    o.height = sp.page_height;
    CHECK(pd_doc_insert_inline(d, at(pd_doc_child(d, fl, 0), 0), &o, NULL) == PD_OK);

    for (k = 0; k < pd_doc_story_count(d); k++) {  /* the text box's story: its text edited */
        pd_block_id st = pd_doc_story_at(d, k);

        if (text_is(d, pd_doc_child(d, st, 0), "Draft")) {
            story = st;
        }
    }

    CHECK(story != 0);

    if (story) {
        para = pd_doc_child(d, story, 0);
        pd_doc_insert_text(d, end_of(d, para), " two", 4, PD_FORMAT_INHERIT, NULL);
    }

    back = pptx_again(d, &b);
    CHECK(back && slides_same(d, back));

    {
        slide_sum s;

        CHECK(back && slide_of(back, 0, &s) && !strcmp(s.text, "Draft two|/") && s.nitems >= 3);
        CHECK(back && s.w == PD_PT(720) && s.h == PD_PT(540) && s.box[0][0] == PD_PT(72) && s.box[0][2] == PD_PT(144) &&
              s.kind[0] == 's');
    }

    free(b.p);
    pd_doc_free(back);
    pd_doc_free(d);

    /* plain text in two sections: two slides, each a text box of its section's text */
    d = NULL;
    CHECK(pd_doc_import("First slide\nmore of it", 22, PD_CONV_TEXT, &d) == PD_OK && d);

    if (d) {
        pd_block_id s2, p2;
        pd_block_info bi;

        pd_doc_insert_block(d, pd_doc_root(d), 1, PD_BLOCK_SECTION, &s2);
        pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp);
        pd_doc_set_section_props(d, s2, &sp);
        p2 = pd_doc_child(d, s2, 0);
        text(d, p2, "Second slide");
        back = pptx_again(d, &b);
        CHECK(back && pd_doc_block_info(back, pd_doc_root(back), &bi) == PD_OK && bi.child_count == 2);

        if (back && bi.child_count == 2) {
            slide_sum s;

            CHECK(slide_of(back, 0, &s) && !strcmp(s.text, "First slide|more of it|/") &&
                  labs((long)(s.w - sp.page_width)) < PD_PT(1));
            CHECK(slide_of(back, 1, &s) && !strcmp(s.text, "Second slide|/"));
            CHECK(llabs(s.box[0][0] - sp.margin_left) < PD_PT(1) && llabs(s.box[0][1] - sp.margin_top) < PD_PT(1));
        }

        free(b.p);
        pd_doc_free(back);
        pd_doc_free(d);
    }

    /* a document of every kind of text: the numbers its fields and notes show, as text */
    d = rich();
    back = d ? pptx_again(d, &b) : NULL;

    {
        slide_sum s;

        CHECK(back && slide_of(back, 0, &s) && strstr(s.text, "Conversion Test|") && strstr(s.text, "|Figure 1: ") &&
              strstr(s.text, "footnote here1 and"));
    }

    free(b.p);
    pd_doc_free(back);
    pd_doc_free(d);
}


static pd_doc* pres_doc(const char* first, ...) {
    va_list ap;
    pd_doc* d;

    va_start(ap, first);
    d = parts_doc(PD_CONV_PPTX, NULL, 0, first, ap);
    va_end(ap);
    return d;
}

/* A deck written back on the master and layouts it came with: each slide on its own layout again, what the master
   and the layouts draw left to them (not copied onto the slides), a slide's own background and its hiding of the
   master's shapes kept, its title a placeholder of the layout's; read back, the same slides */
static void test_pptx_write_kept_masters(void) {
#define KSP(id, name, ph, x) "<p:sp><p:nvSpPr><p:cNvPr id=\"" id "\" name=\"" name "\"/><p:cNvSpPr/><p:nvPr>" ph \
    "</p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"" x "\" y=\"254000\"/><a:ext cx=\"2540000\" cy=\"635000\"/></a:xfrm>" \
    "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:solidFill><a:srgbClr val=\"70AD47\"/></a:solidFill></p:spPr>" \
    "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr sz=\"2000\"/><a:t>" name "</a:t></a:r></a:p></p:txBody></p:sp>"
#define KTREE(body) "<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>" \
    "<p:grpSpPr/>" body "</p:spTree></p:cSld>"
#define KNS "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" xmlns:a=\"http://schemas." \
    "openxmlformats.org/drawingml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
#define KREL(id, t) "<Relationship Id=\"" id "\" Type=\"x\" Target=\"" t "\"/>"
    pd_doc* d = pres_doc(
                    "ppt/presentation.xml", "<p:presentation " KNS "><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" "
                    "r:id=\"rIdM\"/></p:sldMasterIdLst><p:sldIdLst><p:sldId id=\"256\" r:id=\"rIdA\"/><p:sldId id=\"257\" "
                    "r:id=\"rIdB\"/></p:sldIdLst><p:sldSz cx=\"9144000\" cy=\"6858000\"/><p:notesSz cx=\"6858000\" "
                    "cy=\"9144000\"/></p:presentation>",
                    "ppt/_rels/presentation.xml.rels", "<Relationships>" KREL("rIdM", "slideMasters/slideMaster1.xml")
                    KREL("rIdA", "slides/slide1.xml") KREL("rIdB", "slides/slide2.xml") "</Relationships>",
                    "ppt/slideMasters/slideMaster1.xml", "<p:sldMaster " KNS ">" KTREE(KSP("4", "Band", "", "254000"))
                    "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/><p:sldLayoutId id=\"2147483650\" "
                    "r:id=\"rId2\"/></p:sldLayoutIdLst></p:sldMaster>",
                    "ppt/slideMasters/_rels/slideMaster1.xml.rels", "<Relationships>"
                    KREL("rId1", "../slideLayouts/slideLayout1.xml") KREL("rId2", "../slideLayouts/slideLayout2.xml")
                    "</Relationships>",
                    "ppt/slideLayouts/slideLayout1.xml", "<p:sldLayout " KNS ">" KTREE("") "</p:sldLayout>",
                    "ppt/slideLayouts/_rels/slideLayout1.xml.rels", "<Relationships>"
                    KREL("rId1", "../slideMasters/slideMaster1.xml") "</Relationships>",
                    "ppt/slideLayouts/slideLayout2.xml", "<p:sldLayout " KNS ">" KTREE(KSP("5", "Logo", "", "5080000")
                            KSP("2", "Title", "<p:ph type=\"title\"/>", "2540000")) "</p:sldLayout>",
                    "ppt/slideLayouts/_rels/slideLayout2.xml.rels", "<Relationships>"
                    KREL("rId1", "../slideMasters/slideMaster1.xml") "</Relationships>",
                    "ppt/slides/slide1.xml", "<p:sld " KNS " xmlns:p14=\"p14ns\" xmlns:mc=\"mcns\" mc:Ignorable=\"p14\">"
                    KTREE(KSP("3", "One", "", "254000")) "<mc:AlternateContent><mc:Choice Requires=\"p14\"><p:transition "
                    "spd=\"slow\" p14:dur=\"2000\"><p:fade/></p:transition></mc:Choice><mc:Fallback><p:transition "
                    "spd=\"slow\"><p:fade/></p:transition></mc:Fallback></mc:AlternateContent><p:timing><p:tnLst><p:par>"
                    "<p:cTn id=\"1\" dur=\"indefinite\" restart=\"never\" nodeType=\"tmRoot\"/></p:par></p:tnLst>"
                    "</p:timing></p:sld>",
                    "ppt/slides/_rels/slide1.xml.rels", "<Relationships>" KREL("rId1", "../slideLayouts/slideLayout1.xml")
                    KREL("rId9", "../notesSlides/notesSlide1.xml") "</Relationships>",
                    "ppt/notesSlides/notesSlide1.xml", "<p:notes " KNS "><p:cSld><p:spTree><p:sp><p:nvSpPr><p:cNvPr id=\"2\" "
                    "name=\"I\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"sldImg\"/></p:nvPr></p:nvSpPr><p:spPr/></p:sp><p:sp>"
                    "<p:nvSpPr><p:cNvPr id=\"3\" name=\"N\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr>"
                    "</p:nvSpPr><p:spPr/><p:txBody><a:bodyPr/><a:p><a:r><a:t>Say hello &amp; smile.</a:t></a:r></a:p><a:p>"
                    "<a:r><a:t>Then the </a:t></a:r><a:r><a:t>results.</a:t></a:r></a:p></p:txBody></p:sp></p:spTree>"
                    "</p:cSld></p:notes>",
                    "ppt/slides/slide2.xml", "<p:sld " KNS " showMasterSp=\"0\"><p:cSld><p:bg><p:bgPr><a:solidFill>"
                    "<a:srgbClr val=\"FF0000\"/></a:solidFill><a:effectLst/></p:bgPr></p:bg><p:spTree><p:nvGrpSpPr>"
                    "<p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
                    KSP("40", "Two", "<p:ph type=\"title\"/>", "2540000") "</p:spTree></p:cSld><p:clrMapOvr>"
                    "<a:overrideClrMapping bg1=\"dk1\" tx1=\"lt1\" bg2=\"dk2\" tx2=\"lt2\" accent1=\"accent1\" "
                    "accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" "
                    "hlink=\"hlink\" folHlink=\"folHlink\"/></p:clrMapOvr><p:timing><p:tnLst><p:par><p:cTn id=\"1\" "
                    "nodeType=\"tmRoot\"><p:childTnLst><p:set><p:cBhvr><p:cTn id=\"2\"/><p:tgtEl><p:spTgt spid=\"40\"/>"
                    "</p:tgtEl></p:cBhvr></p:set></p:childTnLst></p:cTn></p:par></p:tnLst><p:bldLst><p:bldP spid=\"40\" "
                    "grpId=\"0\"/></p:bldLst></p:timing></p:sld>",
                    "ppt/slides/_rels/slide2.xml.rels", "<Relationships>" KREL("rId1", "../slideLayouts/slideLayout2.xml")
                    "</Relationships>", NULL);
    pd_doc* back = NULL;
    buf_t b;
    size_t n = 0;
    char* s1, *s2, *r2;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    back = pptx_again(d, &b);
    CHECK(back && slides_same(d, back));
    s1 = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide1.xml", &n) : NULL;
    s2 = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/slide2.xml", &n) : NULL;
    r2 = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slides/_rels/slide2.xml.rels", &n) : NULL;
    CHECK(s1 && strstr(s1, "name=\"One\"") && !strstr(s1, "name=\"Band\"") && !strstr(s1, "<p:bg>"));
    CHECK(s2 && strstr(s2, "showMasterSp=\"0\"") && strstr(s2, "<p:bg><p:bgPr><a:solidFill><a:srgbClr val=\"FF0000\"/>"));
    CHECK(s2 && strstr(s2, "name=\"Two\"/><p:cNvSpPr/><p:nvPr><p:ph type=\"title\"/></p:nvPr>") &&
          !strstr(s2, "name=\"Logo\""));
    CHECK(r2 && strstr(r2, "Target=\"../slideLayouts/slideLayout2.xml\""));
    CHECK(s2 && strstr(s2, "<a:overrideClrMapping bg1=\"dk1\" tx1=\"lt1\"") && s1 && strstr(s1, "<a:masterClrMapping/>"));
    /* the first slide's transition and timing as they were, after its colour map, with the namespaces they use */
    CHECK(s1 && strstr(s1, "</p:clrMapOvr><mc:AlternateContent><mc:Choice Requires=\"p14\"><p:transition spd=\"slow\" "
                       "p14:dur=\"2000\">") && strstr(s1, "<p:timing><p:tnLst>") && strstr(s1, "xmlns:p14=\"p14ns\"") &&
          strstr(s1, "mc:Ignorable=\"p14\""));
    CHECK(s2 && !strstr(s2, "<p:transition"));
    {   /* the second slide's animation kept too (not the first's alone), naming its shape by the id it has now */
        const char* nm = s2 ? strstr(s2, "\" name=\"Two\"") : NULL;
        const char* id = nm;
        char want[48], want2[48];
        size_t tn = 0;

        while (id && id > s2 && *(id - 1) != '"') {
            id--;
        }

        snprintf(want, sizeof(want), "<p:spTgt spid=\"%.*s\"/>", nm && id ? (int)(nm - id) : 0, id ? id : "");
        snprintf(want2, sizeof(want2), "<p:bldP spid=\"%.*s\"", nm && id ? (int)(nm - id) : 0, id ? id : "");
        CHECK(s2 && strstr(s2, "<p:timing><p:tnLst>") && strstr(s2, want) && strstr(s2, want2));
        CHECK(nm && id && nm - id < 6 && strncmp(id, "40", 2) != 0);    /* renumbered, so the remapping is seen */
        CHECK(!pd_zip_get((const unsigned char*)b.p, b.n, "tail", &tn) &&
              !pd_zip_get((const unsigned char*)b.p, b.n, "tail/3", &tn));
    }
    {   /* what is drawn says which shape it is drawn for, by the id the animations name it by: its box and its text;
           the master's shape behind a slide's by its own */
        pd_layout* L = NULL;
        int32_t pg, k, cnt;
        int box40 = 0, text40 = 0, one3 = 0, band = 0, stray = 0;

        pd_doc_set_font_resolver(d, layout_resolver, NULL);

        if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
            for (pg = 0; pg < 2 && pg < pd_layout_page_count(L); pg++) {
                pd_draw* it;

                cnt = 0;
                pd_layout_page_items(L, pg, NULL, 0, &cnt);
                it = (pd_draw*)malloc(((size_t)cnt + 1) * sizeof(pd_draw));

                if (it && pd_layout_page_items(L, pg, it, cnt, &cnt) == PD_OK) {
                    for (k = 0; k < cnt; k++) {
                        box40 += pg == 1 && it[k].shape == 40 && it[k].kind != PD_DRAW_GLYPH &&
                                 (it[k].fill & 0xFFFFFF) == 0x70AD47;
                        box40 += pg == 1 && it[k].shape == 40 && it[k].kind == PD_DRAW_RULE &&
                                 (it[k].color & 0xFFFFFF) == 0x70AD47;
                        text40 += pg == 1 && it[k].shape == 40 && it[k].kind == PD_DRAW_GLYPH && it[k].text == 'T';
                        one3 += pg == 0 && it[k].shape == 3 && it[k].kind == PD_DRAW_GLYPH && it[k].text == 'O';
                        band += pg == 0 && it[k].shape >= 2000000 && it[k].kind == PD_DRAW_GLYPH && it[k].text == 'B';
                        stray += pg == 1 && it[k].kind == PD_DRAW_GLYPH && it[k].text == 'T' && it[k].shape != 40;
                    }
                }

                free(it);
            }
        }

        {   /* the slide's timing for a slide show: as read, its shapes by the ids they had, in one element */
            int32_t tl = 0;
            char* tb;

            CHECK(pd_pptx_slide_timing(d, pd_doc_child(d, pd_doc_root(d), 1), NULL, 0, &tl) == PD_OK && tl > 0);
            tb = (char*)calloc((size_t)tl + 1, 1);
            CHECK(tb && pd_pptx_slide_timing(d, pd_doc_child(d, pd_doc_root(d), 1), tb, tl, &tl) == PD_OK &&
                  !strncmp(tb, "<pd:tail ", 9) && strstr(tb, "<p:spTgt spid=\"40\"/>") &&
                  strstr(tb, "</p:timing></pd:tail>"));
            CHECK(tb && pd_pptx_slide_timing(d, pd_doc_child(d, pd_doc_root(d), 1), tb, tl - 1, &tl) == PD_ERR_RANGE);
            CHECK(tb && strstr(tb, "xmlns:p=\"") && strstr(tb, "xmlns:a=\""));     /* its prefixes declared */
            free(tb);
        }
        {   /* set since: read back as set, written in its place by the shapes' new ids; set empty, none */
            static const char nt[] = "<?xml version=\"1.0\"?>\n<pd:tail xmlns:pd=\"urn:parade\" xmlns:p=\"pns\" "
                "xmlns:p14=\"p14ns\"><p:transition spd=\"fast\"><p:wipe dir=\"u\"/></p:transition><p:timing><p:tnLst>"
                "<p:par><p:cTn id=\"1\" nodeType=\"tmRoot\"><p:childTnLst><p:set><p:cBhvr><p:cTn id=\"2\"/><p:tgtEl>"
                "<p:spTgt spid=\"40\"/></p:tgtEl></p:cBhvr></p:set></p:childTnLst></p:cTn></p:par></p:tnLst></p:timing>"
                "</pd:tail>\n";
            pd_block_id s1s = pd_doc_child(d, pd_doc_root(d), 1), s0s = pd_doc_child(d, pd_doc_root(d), 0);
            int32_t tl = 0;
            char tb[2048], *x2, *x1;
            pd_doc* again;
            buf_t b2;
            size_t n2 = 0;

            CHECK(pd_pptx_set_slide_timing(d, s1s, nt, (int32_t)strlen(nt)) == PD_OK);
            CHECK(pd_pptx_set_slide_timing(d, s1s, "<p:timing/>", 11) == PD_ERR_ARG);    /* not in one: refused */
            CHECK(pd_pptx_slide_timing(d, s1s, tb, (int32_t)sizeof(tb) - 1, &tl) == PD_OK);
            tb[tl > 0 && tl < (int32_t)sizeof(tb) ? tl : 0] = '\0';
            CHECK(strstr(tb, "<p:wipe dir=\"u\"/>") && strstr(tb, "xmlns:p14=\"p14ns\"") &&
                  !strstr(tb, "<p:bldLst>") && strstr(tb, "</p:timing></pd:tail>"));
            CHECK(pd_pptx_set_slide_timing(d, s0s, "<pd:tail xmlns:pd=\"urn:parade\"/>", 33) == PD_OK);
            CHECK(pd_pptx_slide_timing(d, s0s, NULL, 0, &tl) == PD_ERR_ARG);
            {   /* each set one step of undo: taken back, the timing read; again, the one set; saved and loaded with it */
                pd_doc* j = NULL;
                buf_t bj;
                char t2[2048];
                int32_t l2 = 0;

                CHECK(pd_doc_undo(d) == PD_OK && pd_pptx_slide_timing(d, s0s, NULL, 0, &tl) == PD_OK && tl > 0);
                CHECK(pd_doc_undo(d) == PD_OK && pd_pptx_slide_timing(d, s1s, tb, (int32_t)sizeof(tb) - 1, &tl) == PD_OK);
                tb[tl > 0 && tl < (int32_t)sizeof(tb) ? tl : 0] = '\0';
                CHECK(strstr(tb, "<p:spTgt spid=\"40\"/>") && !strstr(tb, "<p:wipe"));
                CHECK(pd_doc_redo(d) == PD_OK && pd_doc_redo(d) == PD_OK &&
                      pd_pptx_slide_timing(d, s0s, NULL, 0, &tl) == PD_ERR_ARG);
                memset(&bj, 0, sizeof(bj));
                CHECK(pd_doc_save(d, PD_JDATA_BINARY, to_buf, &bj) == PD_OK && pd_doc_load(bj.p, bj.n, PD_JDATA_AUTO, &j) ==
                      PD_OK);
                CHECK(j && pd_pptx_slide_timing(j, pd_doc_child(j, pd_doc_root(j), 1), t2, (int32_t)sizeof(t2) - 1, &l2) ==
                      PD_OK && l2 > 0 && (t2[l2] = '\0', strstr(t2, "<p:wipe dir=\"u\"/>") != NULL) &&
                      pd_pptx_slide_timing(j, pd_doc_child(j, pd_doc_root(j), 0), NULL, 0, &l2) == PD_ERR_ARG);
                pd_doc_free(j);
                free(bj.p);
            }
            again = pptx_again(d, &b2);
            x1 = b2.p ? (char*)pd_zip_get((const unsigned char*)b2.p, b2.n, "ppt/slides/slide1.xml", &n2) : NULL;
            x2 = b2.p ? (char*)pd_zip_get((const unsigned char*)b2.p, b2.n, "ppt/slides/slide2.xml", &n2) : NULL;
            CHECK(x1 && !strstr(x1, "<p:transition") && !strstr(x1, "<p:timing"));
            CHECK(x2 && strstr(x2, "<p:transition spd=\"fast\"><p:wipe dir=\"u\"/></p:transition><p:timing>") &&
                  !strstr(x2, "<p:spTgt spid=\"40\"/>") && strstr(x2, "xmlns:p14=\"p14ns\"") && !strstr(x2, "<p:bldLst>"));
            CHECK(again && pd_pptx_slide_timing(again, pd_doc_child(again, pd_doc_root(again), 1), tb,
                                                (int32_t)sizeof(tb) - 1, &tl) == PD_OK && tl > 0);
            free(x1);
            free(x2);
            free(b2.p);
            pd_doc_free(again);
        }
        {
            pd_doc* plain = NULL;
            int32_t tl = 0;

            pd_doc_new(&plain);
            CHECK(plain && pd_pptx_slide_timing(plain, pd_doc_child(plain, pd_doc_root(plain), 0), NULL, 0, &tl) ==
                  PD_ERR_ARG && tl == 0);
            pd_doc_free(plain);
        }
        CHECK(box40 >= 1 && text40 == 1 && !stray);
        CHECK(one3 == 1 && band == 1);
        pd_layout_free(L);
    }
    {   /* the first slide's speaker notes: a story of its section, two paragraphs; written back as a notes slide on a
           notes master the presentation names, and read again */
        pd_section_props sp;
        pd_block_id st;
        const char* t;
        uint32_t tl;
        char* ns, *pres;
        size_t nn;
        int pass;

        for (pass = 0; pass < 2; pass++) {
            pd_doc* e = pass ? back : d;

            CHECK(e && pd_doc_section_props(e, pd_doc_child(e, pd_doc_root(e), 0), &sp) == PD_OK && sp.notes);
            st = e ? sp.notes : 0;
            CHECK(st && pd_doc_para_text(e, pd_doc_child(e, st, 0), &t, &tl) == PD_OK && tl == 18 &&
                  !memcmp(t, "Say hello & smile.", 18));
            CHECK(st && pd_doc_para_text(e, pd_doc_child(e, st, 1), &t, &tl) == PD_OK && tl == 17 &&
                  !memcmp(t, "Then the results.", 17));
            CHECK(e && pd_doc_section_props(e, pd_doc_child(e, pd_doc_root(e), 1), &sp) == PD_OK && !sp.notes);
        }

        ns = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/notesSlides/notesSlide1.xml", &nn) : NULL;
        CHECK(ns && strstr(ns, "<a:t>Say hello &amp; smile.</a:t>") && strstr(ns, "<p:ph type=\"body\" idx=\"1\"/>"));
        free(ns);
        pres = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/presentation.xml", &nn) : NULL;
        CHECK(pres && strstr(pres, "</p:sldMasterIdLst><p:notesMasterIdLst><p:notesMasterId r:id=\"rIdNM\"/>"));
        free(pres);
        ns = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/notesMasters/notesMaster1.xml", &nn) : NULL;
        CHECK(ns != NULL);
        free(ns);
        CHECK(b.p && !pd_zip_get((const unsigned char*)b.p, b.n, "ppt/notesSlides/notesSlide2.xml", &nn));
    }
    free(r2);
    r2 = b.p ? (char*)pd_zip_get((const unsigned char*)b.p, b.n, "ppt/slideLayouts/slideLayout2.xml", &n) : NULL;
    CHECK(r2 && strstr(r2, "name=\"Logo\""));

    {   /* read back: the first slide has the master's band and its own box, the second its title and not the
           layout's logo (it hides them), on red */
        slide_sum sa;

        CHECK(back && slide_of(back, 0, &sa) && !strcmp(sa.text, "Band|/One|/"));
        CHECK(back && slide_of(back, 1, &sa) && !strcmp(sa.text, "Two|/"));
    }

    {   /* a slide added after the second (as the editor's New Slide does): on the second's layout, not the first's */
        pd_block_id ns = 0;
        buf_t b3 = { 0 };
        char* r3;
        size_t n3;

        CHECK(pd_doc_insert_block(d, pd_doc_root(d), 2, PD_BLOCK_SECTION, &ns) == PD_OK);
        CHECK(pd_doc_export(d, PD_CONV_PPTX, to_buf, &b3) == PD_OK);
        r3 = b3.p ? (char*)pd_zip_get((const unsigned char*)b3.p, b3.n, "ppt/slides/_rels/slide3.xml.rels", &n3) : NULL;
        CHECK(r3 && strstr(r3, "Target=\"../slideLayouts/slideLayout2.xml\""));
        free(r3);
        free(b3.p);
    }

    free(s1);
    free(s2);
    free(r2);
    free(b.p);
    pd_doc_free(back);
    pd_doc_free(d);
#undef KSP
#undef KTREE
#undef KNS
#undef KREL
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

/* Metafile shapes the player skipped and pens and brushes it flattened: a pie (a quarter of a circle, the top right,
   from the radial on the right anticlockwise to the one at the top), a horizontal red-to-blue GRADIENTFILL, a box
   in a dashed pen and a cross-hatched brush, and an open arc. */
static void test_emf_shapes(void) {
    buf_t e = { NULL, 0 };
    uint32_t hdr[20];
    pd_doc* d;
    pd_res_id r, src;
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    char* js, want[96];
    double W = 25 / 25.4 * 72 * 65536;

    memset(hdr, 0, sizeof(hdr));
    hdr[2] = 99; hdr[3] = 99;
    hdr[6] = 2500; hdr[7] = 2500;
    hdr[8] = 0x464D4520u;
    hdr[9] = 0x10000; hdr[12] = 4;
    hdr[16] = 100; hdr[17] = 100;
    hdr[18] = 25; hdr[19] = 25;
    emr(&e, 1, hdr, 20);
    {
        uint32_t brush[4] = { 1, 0, 0x0000FF00, 0 }, sel[1] = { 1 }, pie[8] = { 10, 10, 50, 50, 50, 30, 30, 10 };

        emr(&e, 39, brush, 4);
        emr(&e, 37, sel, 1);
        emr(&e, 47, pie, 8);
    }
    {   /* two vertices, one rectangle, horizontal: red at the left, blue at the right */
        uint32_t g[7 + 8 + 2] = { 0, 0, 0, 0, 2, 1, 0, 60, 10, 0xFF00u, 0, 90, 30, 0, 0xFF00u, 0, 1 };

        emr(&e, 118, g, 17);
    }
    {
        uint32_t pen[5] = { 2, 1, 1, 0, 0x00000000 }, hatch[4] = { 3, 2, 0x00000000, 4 }, s2[1] = { 2 }, s3[1] = { 3 };
        uint32_t box[4] = { 60, 60, 90, 90 }, arcw[8] = { 10, 60, 50, 90, 50, 75, 10, 75 };

        emr(&e, 38, pen, 5);    /* a dashed pen, one unit */
        emr(&e, 39, hatch, 4);  /* a cross-hatched brush, black */
        emr(&e, 37, s2, 1);
        emr(&e, 37, s3, 1);
        emr(&e, 43, box, 4);
        emr(&e, 45, arcw, 8);
    }
    emr(&e, 14, hdr, 3);

    pd_doc_new(&d);
    CHECK(pd_doc_add_resource(d, "image/x-emf", e.p, e.n, &src) == PD_OK);
    r = pd_metafile_drawing(d, (const unsigned char*)e.p, e.n, src);
    CHECK(r != 0 && pd_doc_resource(d, r, &mime, &data, &len) == PD_OK);
    js = (char*)malloc(len + 1);

    if (js && r) {
        char item[8192];

        memcpy(js, data, len);
        js[len] = '\0';
        /* the pie: green, from its centre, through the right and the top, not the bottom */
        snprintf(want, sizeof(want), "[%d,%d,", (int)(W * 0.5), (int)(W * 0.3));
        CHECK(item_with(js, want, item, sizeof(item)) && strstr(item, "\"fill\":4278255360") && strstr(item, "\"closed\":1"));
        CHECK(item_has_pt(item, (long)(W * 0.3), (long)(W * 0.3)) && item_has_pt(item, (long)(W * 0.3), (long)(W * 0.1)));
        CHECK(!item_has_pt(item, (long)(W * 0.3), (long)(W * 0.5)) && !item_has_pt(item, (long)(W * 0.1), (long)(W * 0.3)));
        /* the gradient */
        CHECK(strstr(js, "\"gk\":1,\"f2\":4278190335,\"ga\":0,\"gs\":[0,4294901760,100000,4278190335]") != NULL);
        /* the box: dashed, cross-hatched */
        CHECK(strstr(js, "\"dash\":[300000,100000]") != NULL && strstr(js, "\"patt\":[255,128,128,128,128,128,128,128]") != NULL);
        /* the arc: open, over the top of its ellipse (anticlockwise from the right to the left) */
        snprintf(want, sizeof(want), "[%d,%d,", (int)(W * 0.5), (int)(W * 0.75));
        CHECK(item_with(js, want, item, sizeof(item)) && strstr(item, "\"closed\":0") &&
              item_has_pt(item, (long)(W * 0.3), (long)(W * 0.6)) && !item_has_pt(item, (long)(W * 0.3), (long)(W * 0.9)));
    }

    free(js);
    free(e.p);
    pd_doc_free(d);
}

static void le_f(buf_t* b, float f) {
    uint32_t v;

    memcpy(&v, &f, 4);
    le32(b, v);
}

/* an EMF+ record: type, flags, sizes, then its data */
static void plus_rec(buf_t* b, uint16_t type, uint16_t flags, const buf_t* data) {
    le(b, type, 2);
    le(b, flags, 2);
    le32(b, 12 + (uint32_t)data->n);
    le32(b, (uint32_t)data->n);
    to_buf(b, data->p, data->n);
}

/* EMF+ records into a comment of an EMF */
static void emr_plus(buf_t* e, buf_t* recs) {
    while (recs->n & 3) {
        le(recs, 0, 1);
    }

    le32(e, 70);
    le32(e, 16 + (uint32_t)recs->n);
    le32(e, 4 + (uint32_t)recs->n);
    le32(e, 0x2B464D45u);
    to_buf(e, recs->p, recs->n);
    free(recs->p);
    recs->p = NULL;
    recs->n = 0;
}

/* an EMF header: a 100 by 100 device of 4 pixels a millimetre, the frame all of it (25 mm) */
static void emf_head(buf_t* e) {
    uint32_t hdr[20];

    memset(hdr, 0, sizeof(hdr));
    hdr[2] = 99; hdr[3] = 99;
    hdr[6] = 2500; hdr[7] = 2500;
    hdr[8] = 0x464D4520u;
    hdr[9] = 0x10000; hdr[12] = 4;
    hdr[16] = 100; hdr[17] = 100;
    hdr[18] = 25; hdr[19] = 25;
    emr(e, 1, hdr, 20);
}

static void plus_head(buf_t* r) {
    buf_t d = { NULL, 0 };

    le32(&d, 0xDBC01002u);
    le32(&d, 1);
    le32(&d, 96);
    le32(&d, 96);
    plus_rec(r, 0x4001, 1, &d);
    free(d.p);
}

/* a rectangle filled with a colour of its own (EMF+ FillRects, floats) */
static void plus_fill_rect(buf_t* r, uint32_t argb, float x, float y, float w, float h) {
    buf_t d = { NULL, 0 };

    le32(&d, argb);
    le32(&d, 1);
    le_f(&d, x);
    le_f(&d, y);
    le_f(&d, w);
    le_f(&d, h);
    plus_rec(r, 0x400A, 0x8000, &d);
    free(d.p);
}

static void gdi_rect(buf_t* e, uint32_t bgr, uint32_t slot, int l, int t, int rr, int bb) {
    uint32_t brush[4] = { slot, 0, bgr, 0 }, sel[1] = { slot }, box[4] = { (uint32_t)l, (uint32_t)t, (uint32_t)rr,
                                                                          (uint32_t)bb };

    emr(e, 39, brush, 4);
    emr(e, 37, sel, 1);
    emr(e, 43, box, 4);
}

/* GDI text, "Up", at (x, y), under a world transform turning it a quarter turn clockwise about (50, 50) */
static void gdi_turned_text(buf_t* e) {
    float xf[6] = { 0, 1, -1, 0, 50, 50 };
    uint32_t w[6], t[18];

    memcpy(w, xf, sizeof(xf));
    emr(e, 35, w, 6);
    memset(t, 0, sizeof(t));
    t[4] = 1;
    t[7] = 0;
    t[8] = 0;
    t[9] = 2;
    t[10] = 76;
    t[17] = 'U' | ('p' << 16);
    emr(e, 84, t, 18);
}

static char* emf_json(buf_t* e, pd_doc** dp) {
    pd_res_id src, r;

    pd_doc_new(dp);
    pd_doc_add_resource(*dp, "image/x-emf", e->p, e->n, &src);
    r = pd_metafile_drawing(*dp, (const unsigned char*)e->p, e->n, src);
    return r ? drawing_json(*dp, r) : NULL;
}

/* A metafile GDI+ wrote (MATLAB's, Office's): its EMF+ records drawn, not the plain records beside them but for those
   after a GetDC; a metafile drawn inside it (an EMF+ object, sent in two pieces) drawn where DrawImagePoints puts
   it; a string turned by the world transform turned; a linear gradient's stops; a pen's own dashes. A metafile
   with an EMF+ record not played here drawn from its plain records, as before. */
static void test_emf_plus(void) {
    buf_t e = { NULL, 0 }, r = { NULL, 0 }, d = { NULL, 0 }, in = { NULL, 0 }, ir = { NULL, 0 };
    pd_doc* doc;
    char* js, item[8192], want[96];
    double W = 25 / 25.4 * 72 * 65536, U = W / 100;    /* the drawing's units a device pixel */
    size_t half;

    /* the metafile drawn inside: EMF+ only, green over all of it */
    emf_head(&in);
    plus_head(&ir);
    plus_fill_rect(&ir, 0xFF00FF00u, 0, 0, 100, 100);
    emr_plus(&in, &ir);
    emr(&in, 14, (const uint32_t*)"\0\0\0\0\0\0\0\0\0\0\0\0", 3);

    emf_head(&e);
    plus_head(&r);
    plus_fill_rect(&r, 0xFFFF0000u, 10, 10, 20, 20);   /* red: drawn */
    emr_plus(&e, &r);
    gdi_rect(&e, 0x00FF0000u, 1, 60, 60, 90, 90);      /* blue, plain, beside the EMF+ records: not */
    {   /* a 2 by 2 picture, plain, beside the EMF+ records: not drawn, and not made a picture either */
        static const uint32_t w[] = { 0, 0, 99, 99, 60, 60, 0, 0, 2, 2, 80, 40, 120, 16, 0, 0x00CC0020u, 30, 30,
                                      40, 2, 2, 0x00180001u, 0, 16, 0, 0, 0, 0,
                                      0x00FF0000u, 0x0000FF00u, 0x00FF0000u, 0x0000FF00u
                                    };

        emr(&e, 81, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    plus_rec(&r, 0x4004, 0, &d);                        /* GetDC */
    emr_plus(&e, &r);
    gdi_rect(&e, 0x0000FFFFu, 2, 0, 0, 5, 5);          /* yellow, plain, after the GetDC: drawn */
    {   /* "Hi" under a world transform turning it a quarter turn */
        static const float xf[6] = { 0, 1, -1, 0, 50, 50 };
        int k;

        for (k = 0; k < 6; k++) {
            le_f(&d, xf[k]);
        }

        plus_rec(&r, 0x402A, 0, &d);
        free(d.p); d.p = NULL; d.n = 0;
        le32(&d, 0xDBC01002u); le_f(&d, 10); le32(&d, 2); le32(&d, 0); le32(&d, 0); le32(&d, 2);
        le(&d, 'H', 2); le(&d, 'i', 2);
        plus_rec(&r, 0x4008, 0x0600, &d);               /* font 0 */
        free(d.p); d.p = NULL; d.n = 0;
        le32(&d, 0xFF000000u); le32(&d, 63); le32(&d, 2);
        le_f(&d, 0); le_f(&d, 0); le_f(&d, 0); le_f(&d, 0);
        le(&d, 'H', 2); le(&d, 'i', 2);
        plus_rec(&r, 0x401C, 0x8000, &d);
        free(d.p); d.p = NULL; d.n = 0;
        plus_rec(&r, 0x402B, 0, &d);
    }
    {   /* a linear gradient, red to blue left to right, filling a square */
        le32(&d, 0xDBC01002u); le32(&d, 4); le32(&d, 0); le32(&d, 0);
        le_f(&d, 0); le_f(&d, 0); le_f(&d, 10); le_f(&d, 0);
        le32(&d, 0xFFFF0000u); le32(&d, 0xFF0000FFu); le32(&d, 0); le32(&d, 0);
        plus_rec(&r, 0x4008, 0x0101, &d);               /* brush 1 */
        free(d.p); d.p = NULL; d.n = 0;
        le32(&d, 1); le32(&d, 1); le_f(&d, 40); le_f(&d, 10); le_f(&d, 20); le_f(&d, 20);
        plus_rec(&r, 0x400A, 0, &d);
        free(d.p); d.p = NULL; d.n = 0;
    }
    {   /* a pen with dashes of its own (3 on, 1 off) and no dash style, drawing a path across */
        le32(&d, 0xDBC01002u); le32(&d, 0); le32(&d, 0x100); le32(&d, 2); le_f(&d, 2);
        le32(&d, 2); le_f(&d, 3); le_f(&d, 1);
        le32(&d, 0xDBC01002u); le32(&d, 0); le32(&d, 0xFF000000u);
        plus_rec(&r, 0x4008, 0x0202, &d);               /* pen 2 */
        free(d.p); d.p = NULL; d.n = 0;
        le32(&d, 0xDBC01002u); le32(&d, 2); le32(&d, 0);
        le_f(&d, 10); le_f(&d, 80); le_f(&d, 90); le_f(&d, 80);
        le(&d, 0, 1); le(&d, 1, 1); le(&d, 0, 2);
        plus_rec(&r, 0x4008, 0x0303, &d);               /* path 3 */
        free(d.p); d.p = NULL; d.n = 0;
        le32(&d, 2);
        plus_rec(&r, 0x4015, 3, &d);
        free(d.p); d.p = NULL; d.n = 0;
    }
    {   /* the metafile inside as image 4, in two pieces, drawn into (50, 50)-(70, 70) */
        buf_t obj = { NULL, 0 };

        le32(&obj, 0xDBC01002u); le32(&obj, 2); le32(&obj, 4); le32(&obj, (uint32_t)in.n);
        to_buf(&obj, in.p, in.n);
        half = obj.n / 2;
        le32(&d, (uint32_t)obj.n);
        to_buf(&d, obj.p, half);
        plus_rec(&r, 0x4008, 0x8504, &d);
        free(d.p); d.p = NULL; d.n = 0;
        le32(&d, (uint32_t)obj.n);
        to_buf(&d, obj.p + half, obj.n - half);
        plus_rec(&r, 0x4008, 0x8504, &d);
        free(d.p); d.p = NULL; d.n = 0;
        free(obj.p);
        le32(&d, 0xFFFFFFFFu); le32(&d, 2); le_f(&d, 0); le_f(&d, 0); le_f(&d, 100); le_f(&d, 100);
        le32(&d, 3); le_f(&d, 50); le_f(&d, 50); le_f(&d, 70); le_f(&d, 50); le_f(&d, 50); le_f(&d, 70);
        plus_rec(&r, 0x401B, 4, &d);
        free(d.p); d.p = NULL; d.n = 0;
    }
    plus_rec(&r, 0x4002, 0, &d);
    emr_plus(&e, &r);
    emr(&e, 14, (const uint32_t*)"\0\0\0\0\0\0\0\0\0\0\0\0", 3);

    js = emf_json(&e, &doc);
    CHECK(js != NULL);
    {   /* the metafile, its drawing, the metafile inside's drawing... but no PNG of the plain picture nothing shows */
        const char* mm;
        const void* dd;
        size_t ll;
        int k, pngs = 0;

        for (k = 1; pd_doc_resource(doc, (pd_res_id)k, &mm, &dd, &ll) == PD_OK; k++) {
            pngs += !strcmp(mm, "image/png");
        }

        CHECK(pngs == 0);
    }

    if (js) {
        snprintf(want, sizeof(want), "[%d,%d,", (int)(10 * U), (int)(10 * U));
        CHECK(item_with(js, want, item, sizeof(item)) && strstr(item, "\"fill\":4294901760"));     /* red */
        snprintf(want, sizeof(want), "[%d,%d,", (int)(60 * U), (int)(60 * U));
        CHECK(!item_with(js, want, item, sizeof(item)));                                        /* no blue box */
        CHECK(strstr(js, "\"fill\":4294967040") != NULL);                                       /* yellow, after GetDC */
        CHECK(strstr(js, "{\"label\":\"Hi\"") != NULL && strstr(strstr(js, "{\"label\":\"Hi\""), "\"rot\":5400000"));
        CHECK(strstr(js, "\"gk\":1,\"f2\":4278190335,\"ga\":0,\"gs\":[0,4294901760,100000,4278190335]") != NULL);
        CHECK(strstr(js, "\"dash\":[300000,100000]") != NULL);
        snprintf(want, sizeof(want), "[%d,%d,", (int)(50 * U), (int)(50 * U));
        CHECK(item_with(js, want, item, sizeof(item)) && strstr(item, "\"fill\":4278255360") &&
              item_has_pt(item, (long)(70 * U), (long)(70 * U)));                              /* green, inside */
        free(js);
    }

    pd_doc_free(doc);
    free(e.p);
    e.p = NULL;
    e.n = 0;

    /* an EMF+ record not played here (DrawDriverString): the plain records, as before */
    emf_head(&e);
    plus_head(&r);
    le32(&d, 0);
    plus_rec(&r, 0x4036, 0, &d);
    free(d.p); d.p = NULL; d.n = 0;
    plus_fill_rect(&r, 0xFFFF0000u, 10, 10, 20, 20);
    emr_plus(&e, &r);
    gdi_rect(&e, 0x00FF0000u, 1, 60, 60, 90, 90);
    gdi_turned_text(&e);
    emr(&e, 14, (const uint32_t*)"\0\0\0\0\0\0\0\0\0\0\0\0", 3);
    js = emf_json(&e, &doc);
    CHECK(js != NULL);

    if (js) {
        snprintf(want, sizeof(want), "[%d,%d,", (int)(60 * U), (int)(60 * U));
        CHECK(item_with(js, want, item, sizeof(item)) && strstr(item, "\"fill\":4278190335"));     /* blue */
        CHECK(strstr(js, "\"fill\":4294901760") == NULL);                                       /* no red */
        /* GDI text turned by its world transform: turned */
        CHECK(strstr(js, "{\"label\":\"Up\"") != NULL && strstr(strstr(js, "{\"label\":\"Up\""), "\"rot\":5400000"));
        free(js);
    }

    pd_doc_free(doc);
    free(e.p);
    free(in.p);
    free(d.p);
}

static int bytes_in(const char* p, size_t n, const char* q, size_t k) {
    size_t i;

    for (i = 0; i + k <= n; i++) {
        if (!memcmp(p + i, q, k)) {
            return 1;
        }
    }

    return 0;
}

/* Curves kept as curves: an EMF ellipse and a poly-Bezier come out as drawing items with their cubics ("cv") beside
   the points they flatten to, and laid out as paths with a curve of cubics (what PDF and SVG draw); a figure the
   clip cuts has its flattened points alone. */
static void test_emf_curves(void) {
    buf_t e = { NULL, 0 };
    pd_doc* d;
    pd_res_id src, dr;
    char* js, item[16384], want[64];
    double U = 25 / 25.4 * 72 * 65536 / 100;

    emf_head(&e);
    {
        uint32_t brush[4] = { 1, 0, 0x0000FF00u, 0 }, sel[1] = { 1 }, ell[4] = { 10, 10, 40, 30 };
        uint32_t bez[5 + 4] = { 0, 0, 99, 99, 7, 50 | (50u << 16), 60 | (40u << 16), 70 | (60u << 16),
                                80 | (50u << 16)
                              };
        uint32_t clip[4] = { 0, 0, 60, 99 }, ell2[4] = { 50, 70, 90, 90 };

        emr(&e, 39, brush, 4);
        emr(&e, 37, sel, 1);
        emr(&e, 42, ell, 4);    /* ELLIPSE */
        bez[4] = 4;
        emr(&e, 85, bez, 9);    /* POLYBEZIER16: a start and one cubic */
        emr(&e, 30, clip, 4);   /* INTERSECTCLIPRECT to x = 60 */
        emr(&e, 42, ell2, 4);   /* an ellipse across the clip's edge */
    }
    emr(&e, 14, (const uint32_t*)"\0\0\0\0\0\0\0\0\0\0\0\0", 3);
    pd_doc_new(&d);
    pd_doc_add_resource(d, "image/x-emf", e.p, e.n, &src);
    dr = pd_metafile_drawing(d, (const unsigned char*)e.p, e.n, src);
    js = drawing_json(d, dr);
    CHECK(js != NULL);

    if (js) {
        snprintf(want, sizeof(want), "[%d,%d,", (int)(40 * U), (int)(20 * U));     /* the ellipse, from its right */
        CHECK(item_with(js, want, item, sizeof(item)) && strstr(item, "\"cv\":[") &&
              strstr(strstr(item, "\"cv\":["), "-2147483647,0"));
        snprintf(want, sizeof(want), "[%d,%d,", (int)(50 * U), (int)(50 * U));     /* the Bezier */
        CHECK(item_with(js, want, item, sizeof(item)) && strstr(item, "\"cv\":[") &&
              strstr(item, "\"closed\":0"));
        {   /* its one cubic: start, marker, the two control points and the end as they are */
            char cvw[160];

            snprintf(cvw, sizeof(cvw), "\"cv\":[%d,%d,-2147483647,0,%d,%d,%d,%d,%d,%d]", (int)(50 * U), (int)(50 * U),
                     (int)(60 * U), (int)(40 * U), (int)(70 * U), (int)(60 * U), (int)(80 * U), (int)(50 * U));
            CHECK(strstr(item, cvw) != NULL);
        }
        {   /* the ellipse, the Bezier, and the cut ellipse's fill and outline: those two without curves */
            int paths = 0, curves = 0;
            const char* q;

            for (q = js; (q = strstr(q, "\"path\":")) != NULL; q++) {
                paths++;
            }

            for (q = js; (q = strstr(q, "\"cv\":")) != NULL; q++) {
                curves++;
            }

            CHECK(paths == 4 && curves == 2);   /* the cut one's fill and outline apart, flattened */
        }
        free(js);
    }

    if (have_layout_font()) {   /* laid out: paths with cubics in their curves */
        pd_inline o;
        pd_layout* L = NULL;
        pd_draw* it;
        int32_t n = 0, k, cubics = 0;
        pd_block_id sec = pd_doc_child(d, pd_doc_root(d), 0), p0 = sec ? pd_doc_child(d, sec, 0) : 0;

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_IMAGE;
        o.resource = dr;
        o.width = o.height = PD_PT(71);
        CHECK(p0 != 0 && pd_doc_insert_inline(d, (pd_pos){ p0, 0 }, &o, NULL) == PD_OK);
        pd_doc_set_font_resolver(d, layout_resolver, NULL);
        CHECK(pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK);
        pd_layout_page_items(L, 0, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, 0, it, n, &n);

        for (k = 0; k < n; k++) {
            int32_t q;

            for (q = 0; it[k].kind == PD_DRAW_PATH && it[k].curve && q < it[k].ncurve; q++) {
                cubics += it[k].curve[2 * q] == PD_PATH_CUBIC;
            }
        }

        CHECK(cubics == 4 + 1);    /* the ellipse's four quarters and the Bezier; the cut ellipse none */
        free(it);
        pd_layout_free(L);
    }

    free(e.p);
    pd_doc_free(d);
}

/* A drawing a metafile makes is saved as where it comes from -- the metafile, the pictures it made -- not its own
   bytes, and made again on load: the same bytes, its picture the one saved (not a new resource), a second save the
   same as the first. A placeholder naming what is no metafile loads as an empty drawing. */
static void test_emf_drawing_saved_as_source(void) {
    buf_t e = { NULL, 0 }, s1 = { NULL, 0 }, s2 = { NULL, 0 };
    pd_doc* d, *x = NULL;
    pd_res_id src, dr, other, k;
    const char* m1, *m2;
    const void* d1, *d2;
    size_t l1, l2;
    int f;

    emf_head(&e);
    gdi_rect(&e, 0x0000FF00u, 1, 10, 10, 50, 50);
    {   /* a 2 by 2 picture, 24 bits, stretched over (60, 60)-(90, 90) */
        static const uint32_t w[] = { 0, 0, 99, 99, 60, 60, 0, 0, 2, 2, 80, 40, 120, 16, 0, 0x00CC0020u, 30, 30,
                                      40, 2, 2, 0x00180001u, 0, 16, 0, 0, 0, 0,
                                      0x00FF0000u, 0x0000FF00u, 0x00FF0000u, 0x0000FF00u
                                    };

        emr(&e, 81, w, (int)(sizeof(w) / sizeof(w[0])));
    }
    emr(&e, 14, (const uint32_t*)"\0\0\0\0\0\0\0\0\0\0\0\0", 3);

    pd_doc_new(&d);
    CHECK(pd_doc_add_resource(d, "image/x-emf", e.p, e.n, &src) == PD_OK);
    dr = pd_metafile_drawing(d, (const unsigned char*)e.p, e.n, src);
    CHECK(dr == src + 2);   /* the picture it made, then the drawing */
    CHECK(pd_doc_resource(d, src + 1, &m1, &d1, &l1) == PD_OK && !strcmp(m1, "image/png"));
    CHECK(pd_doc_add_resource(d, "text/plain", "after", 5, &other) == PD_OK);
    CHECK(pd_doc_resource(d, dr, &m1, &d1, &l1) == PD_OK && strstr((const char*)d1, "\"img\":") != NULL);

    for (f = 0; f < 2; f++) {
        s1.n = s2.n = 0;
        free(s1.p);
        free(s2.p);
        s1.p = s2.p = NULL;
        CHECK(pd_doc_save(d, f ? PD_JDATA_BINARY : PD_JDATA_TEXT, to_buf, &s1) == PD_OK);
        CHECK(bytes_in(s1.p, s1.n, "MadeFrom", 8));
        CHECK(pd_doc_resource(d, dr, &m1, &d1, &l1) == PD_OK && (!f || !bytes_in(s1.p, s1.n, (const char*)d1, l1)));
        /* the drawing's own bytes not saved */
        CHECK(pd_doc_load(s1.p, s1.n, PD_JDATA_AUTO, &x) == PD_OK);

        if (x) {
            for (k = 1; k <= other; k++) {
                CHECK(pd_doc_resource(d, k, &m1, &d1, &l1) == PD_OK && pd_doc_resource(x, k, &m2, &d2, &l2) == PD_OK &&
                      !strcmp(m1, m2) && l1 == l2 && !memcmp(d1, d2, l1));
            }

            CHECK(pd_doc_resource(x, other + 1, &m2, &d2, &l2) != PD_OK);  /* its picture not made a second time */
            CHECK(pd_doc_save(x, f ? PD_JDATA_BINARY : PD_JDATA_TEXT, to_buf, &s2) == PD_OK);
            CHECK(s1.n == s2.n && !memcmp(s1.p, s2.p, s1.n));
            pd_doc_free(x);
            x = NULL;
        }
    }

    {   /* a placeholder made to name the text resource: an empty drawing, the rest as it was */
        char* t, *at;

        s1.n = 0;
        free(s1.p);
        s1.p = NULL;
        pd_doc_save(d, PD_JDATA_TEXT, to_buf, &s1);
        t = (char*)malloc(s1.n + 1);
        memcpy(t, s1.p, s1.n);
        t[s1.n] = '\0';
        at = strstr(t, "\"MadeFrom\": 1");

        if (!at) {
            at = strstr(t, "\"MadeFrom\":1");
        }

        CHECK(at != NULL);

        if (at) {
            char* one = strchr(at, '1');

            *one = '2';     /* the picture, not a metafile */
            CHECK(pd_doc_load(t, s1.n, PD_JDATA_TEXT, &x) == PD_OK);
            CHECK(x && pd_doc_resource(x, dr, &m2, &d2, &l2) == PD_OK && strstr((const char*)d2, "\"items\":[]"));
            CHECK(x && pd_doc_resource(x, other, &m2, &d2, &l2) == PD_OK && l2 == 5);
            pd_doc_free(x);
        }

        free(t);
    }

    free(s1.p);
    free(s2.p);
    free(e.p);
    pd_doc_free(d);
}

/* A metafile that clips (as MATLAB's do, to the axes): a rectangle clip from 20 to 80; a green square from 0 to 100
   filled only inside it; a line across from 0 to 100 drawn only from 20 to 80; a clip narrowed inside SaveDC and
   given back by RestoreDC; a label outside the clip left out */
static void test_emf_clip(void) {
    buf_t e = { NULL, 0 };
    uint32_t hdr[20];
    pd_doc* d;
    pd_res_id r, src;
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    char* js, item[8192];
    double U = 25 / 25.4 * 72 * 65536 / 100;   /* a logical unit, sp */

    memset(hdr, 0, sizeof(hdr));
    hdr[2] = 99; hdr[3] = 99;
    hdr[6] = 2500; hdr[7] = 2500;
    hdr[8] = 0x464D4520u;
    hdr[9] = 0x10000; hdr[12] = 4;
    hdr[16] = 100; hdr[17] = 100;
    hdr[18] = 25; hdr[19] = 25;
    emr(&e, 1, hdr, 20);
    {
        uint32_t clip[4] = { 20, 20, 80, 80 }, brush[4] = { 0, 0x0000FF00, 0 }, sel1[1] = { 1 }, sq[4] = { 0, 0, 100, 100 };
        uint32_t pen[5] = { 2, 0, 1, 0, 0x000000FF }, sel2[1] = { 2 };
        uint32_t line[4 + 1 + 4] = { 0, 0, 100, 100, 2, 0, 50, 100, 50 };
        uint32_t save[1] = { 0 }, inner[4] = { 50, 50, 60, 60 }, restore[1] = { (uint32_t)-1 }, small[4] = { 10, 10, 30, 30 };

        emr(&e, 30, clip, 4);       /* INTERSECTCLIPRECT */
        brush[0] = 1;
        {
            uint32_t b2[4] = { 1, 0, 0x0000FF00, 0 };

            emr(&e, 39, b2, 4);     /* CREATEBRUSHINDIRECT: green */
        }
        emr(&e, 37, sel1, 1);
        emr(&e, 43, sq, 4);         /* RECTANGLE 0..100 */
        emr(&e, 38, pen, 5);        /* CREATEPEN: red */
        emr(&e, 37, sel2, 1);
        emr(&e, 4, line, 9);        /* POLYLINE (0,50)-(100,50) */
        emr(&e, 33, save, 0);       /* SAVEDC */
        emr(&e, 30, inner, 4);
        emr(&e, 34, restore, 1);    /* RESTOREDC -1: the 20..80 clip again */
        {
            uint32_t b3[4] = { 4, 0, 0x00FF0000, 0 }, sel4[1] = { 4 };

            emr(&e, 39, b3, 4);     /* blue */
            emr(&e, 37, sel4, 1);
        }
        emr(&e, 43, small, 4);      /* RECTANGLE 10..30: inside from 20 */
        (void)brush;
    }
    emr(&e, 14, hdr, 3);

    pd_doc_new(&d);
    CHECK(pd_doc_add_resource(d, "image/x-emf", e.p, e.n, &src) == PD_OK);
    r = pd_metafile_drawing(d, (const unsigned char*)e.p, e.n, src);
    CHECK(r != 0 && pd_doc_resource(d, r, &mime, &data, &len) == PD_OK);
    js = (char*)malloc(len + 1);

    if (js && r) {
        memcpy(js, data, len);
        js[len] = '\0';
        /* the green square: its corners at the clip's, none at the square's own */
        CHECK(item_with(js, "\"fill\":4278255360", item, sizeof(item)));
        CHECK(item_has_pt(item, (long)(20 * U), (long)(20 * U)) && item_has_pt(item, (long)(80 * U), (long)(80 * U)));
        CHECK(!item_has_pt(item, 0, 0) && !item_has_pt(item, (long)(100 * U), (long)(100 * U)));
        /* the red line: from 20 to 80 */
        CHECK(item_with(js, "\"line\":4294901760", item, sizeof(item)));
        CHECK(item_has_pt(item, (long)(20 * U), (long)(50 * U)) && item_has_pt(item, (long)(80 * U), (long)(50 * U)));
        CHECK(!item_has_pt(item, 0, (long)(50 * U)));
        /* the blue square after RestoreDC: cut by the 20..80 clip, not the 50..60 one */
        CHECK(item_with(js, "\"fill\":4278190335", item, sizeof(item)));
        CHECK(item_has_pt(item, (long)(20 * U), (long)(20 * U)) && item_has_pt(item, (long)(30 * U), (long)(30 * U)));
        CHECK(!item_has_pt(item, (long)(10 * U), (long)(10 * U)));
    }

    free(js);
    free(e.p);
    pd_doc_free(d);
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

    if (d) {    /* and carried on through Parade's own .docx: embedded again, obfuscated, the same font read back */
        buf_t b = { 0 };
        pd_doc* t = NULL;
        unsigned char* tab;
        size_t tn;

        found = 0;
        CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        tab = pd_zip_get((const unsigned char*)b.p, b.n, "word/fontTable.xml", &tn);
        CHECK(tab && strstr((char*)tab, "<w:embedBold r:id="));
        free(tab);
        tab = pd_zip_get((const unsigned char*)b.p, b.n, "word/fonts/font1.odttf", &tn);
        CHECK(tab && tn == (size_t)n && memcmp(tab, font, 32) != 0 && !memcmp(tab + 32, font + 32, tn - 32));
        free(tab);
        CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &t) == PD_OK);

        for (r = 1; t; r++) {
            const char* mime;
            const void* data;
            size_t len;

            if (pd_doc_resource(t, r, &mime, &data, &len) != PD_OK) {
                break;
            }

            if (!strncmp(mime, "font/ttf", 8)) {
                found = 1;
                CHECK(strstr(mime, "weight=700") && len == (size_t)n && !memcmp(data, font, len));
            }
        }

        CHECK(found);
        pd_doc_free(t);
        free(b.p);
    }

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

/* a part of a .docx written out, NUL-terminated (caller frees); "" when it has none */
static char* docx_part(const buf_t* b, const char* name) {
    size_t n = 0;
    unsigned char* x = b->p ? pd_zip_get((const unsigned char*)b->p, b->n, name, &n) : NULL;
    char* s = (char*)malloc(n + 1);

    if (s) {
        if (x) {
            memcpy(s, x, n);
        }

        s[n] = '\0';
    }

    free(x);
    return s;
}

/* Small things of Word's text: all sixteen highlight colours (written back as highlights, not shading), w:sym
   in the Symbol font as the Unicode character it shows and in Wingdings as its code in that font, w:lang, and
   how notes are numbered (footnotePr/endnotePr: format, start, restart), shown so and written back. */
static void test_docx_text_small(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\"><w:body>"
                    "<w:p><w:r><w:rPr><w:highlight w:val=\"darkCyan\"/><w:lang w:val=\"de-DE\"/></w:rPr><w:t>hi</w:t></w:r>"
                    "<w:r><w:rPr><w:highlight w:val=\"lightGray\"/></w:rPr><w:t>lo</w:t></w:r>"
                    "<w:r><w:sym w:font=\"Symbol\" w:char=\"F06D\"/><w:sym w:font=\"Wingdings\" w:char=\"F04A\"/></w:r></w:p>"
                    "<w:p><w:r><w:t>One</w:t></w:r><w:r><w:endnoteReference w:id=\"1\"/></w:r>"
                    "<w:r><w:t xml:space=\"preserve\"> two</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r>"
                    "<w:r><w:footnoteReference w:id=\"2\"/></w:r></w:p>"
                    "<w:sectPr><w:footnotePr><w:numFmt w:val=\"lowerLetter\"/><w:numStart w:val=\"3\"/></w:footnotePr>"
                    "<w:endnotePr><w:numFmt w:val=\"upperRoman\"/><w:numRestart w:val=\"eachSect\"/></w:endnotePr>"
                    "</w:sectPr></w:body></w:document>",
                    "word/endnotes.xml",
                    "<w:endnotes xmlns:w=\"w\"><w:endnote w:id=\"1\"><w:p><w:r><w:endnoteRef/></w:r><w:r><w:t>E</w:t>"
                    "</w:r></w:p></w:endnote></w:endnotes>",
                    "word/footnotes.xml",
                    "<w:footnotes xmlns:w=\"w\"><w:footnote w:id=\"1\"><w:p><w:r><w:footnoteRef/></w:r>"
                    "<w:r><w:t>F</w:t></w:r></w:p></w:footnote><w:footnote w:id=\"2\"><w:p><w:r><w:footnoteRef/></w:r>"
                    "<w:r><w:t>G</w:t></w:r></w:p></w:footnote></w:footnotes>",
                    "word/settings.xml",
                    "<w:settings xmlns:w=\"w\"><w:footnotePr><w:numFmt w:val=\"upperLetter\"/>"
                    "<w:footnote w:id=\"-1\"/></w:footnotePr></w:settings>",
                    NULL);
    pd_doc* back = NULL;
    pd_block_id sec, p0;
    pd_section_props sp;
    pd_char_props cp;
    const char* t;
    uint32_t n;
    char shown[256];
    buf_t b;
    int pass;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    memset(&b, 0, sizeof(b));
    CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
    CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) == PD_OK);
    {
        char* xml = docx_part(&b, "word/document.xml");

        CHECK(strstr(xml, "<w:highlight w:val=\"darkCyan\"/>") && strstr(xml, "<w:highlight w:val=\"lightGray\"/>"));
        CHECK(strstr(xml, "<w:numFmt w:val=\"lowerLetter\"/><w:numStart w:val=\"3\"/>") &&
              strstr(xml, "<w:numFmt w:val=\"upperRoman\"/><w:numRestart w:val=\"eachSect\"/>"));
        free(xml);
    }

    free(b.p);

    for (pass = 0; pass < 2; pass++) {
        pd_doc* x = pass ? back : d;

        if (!x) {
            continue;
        }

        sec = pd_doc_child(x, pd_doc_root(x), 0);
        p0 = pd_doc_child(x, sec, 0);
        cp = chars_at(x, p0, 0);
        CHECK(cp.background == 0xFF008080u && strcmp(cp.lang, "de-DE") == 0);
        CHECK(chars_at(x, p0, 2).background == 0xFFC0C0C0u);
        pd_doc_para_text(x, p0, &t, &n);
        CHECK(n == 9 && memcmp(t + 4, "\xCE\xBC\xEF\x81\x8A", 5) == 0);   /* mu, then Wingdings' U+F04A */
        CHECK(strcmp(chars_at(x, p0, 6).family, "Wingdings") == 0);
        CHECK(pd_doc_section_props(x, sec, &sp) == PD_OK && sp.note_format[0] == PD_NUM_LOWER_ALPHA &&
              sp.note_start[0] == 3 && sp.note_format[1] == PD_NUM_UPPER_ROMAN &&
              sp.note_restart[1] == PD_NOTES_SECTION);

        if (have_layout_font()) {   /* the marks as the section numbers them: endnote I, footnotes c and d */
            layout_text(x, 0, shown, sizeof(shown));
            CHECK(strstr(shown, "OneItwocd") != NULL);
        }
    }

    pd_doc_free(back);
    pd_doc_free(d);
}

/* the baseline (y) of the first glyph drawn for a code point on page 0; 0 when there is none */
static pd_sp glyph_y(pd_doc* d, uint32_t cp) {
    pd_layout* L = NULL;
    int32_t n = 0, k;
    pd_sp y = 0;
    pd_draw* it;

    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK &&
            pd_layout_page_items(L, 0, NULL, 0, &n) == PD_OK && (it = (pd_draw*)malloc(((size_t)n + 1) *
                    sizeof(pd_draw))) != NULL) {
        pd_layout_page_items(L, 0, it, n, &n);

        for (k = 0; k < n && !y; k++) {
            y = it[k].kind == PD_DRAW_GLYPH && it[k].text == cp ? it[k].y : 0;
        }

        free(it);
    }

    pd_layout_free(L);
    return y;
}

/* Word's exact and at-least line spacing are lengths, not multiples of the font size: 24pt exactly on 12pt text,
   6pt exactly (the lines overlap, as in Word), at least 30pt, and at least 6pt (a single line then); read, laid
   out at those distances, and written back with their rules. */
static void test_docx_line_rules(void) {
    static const char* const rules[4][2] = { { "480", "exact" }, { "120", "exact" }, { "600", "atLeast" },
        { "120", "atLeast" }
    };
    static const int32_t want[4] = { 24, 6, 30, 0 };
    int k, pass;

    for (k = 0; k < 4; k++) {
        char xml[600];
        pd_doc* d;
        pd_doc* back = NULL;
        buf_t b;

        snprintf(xml, sizeof(xml), "<w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:spacing w:line=\"%s\" "
                 "w:lineRule=\"%s\"/></w:pPr><w:r><w:rPr><w:sz w:val=\"24\"/></w:rPr><w:t>A</w:t><w:br/>"
                 "<w:t>B</w:t></w:r></w:p></w:body></w:document>", rules[k][0], rules[k][1]);
        d = docx_doc("word/document.xml", xml, NULL);
        CHECK(d != NULL);

        if (!d) {
            continue;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);

        for (pass = 0; pass < 2 && have_layout_font(); pass++) {
            pd_doc* x = pass ? back : d;
            pd_sp step;

            if (!x) {
                continue;
            }

            step = glyph_y(x, 'B') - glyph_y(x, 'A');

            if (want[k]) {
                CHECK(step == PD_PT(want[k]));
            } else {    /* at least 6pt: the font's own single line, as with no rule (Liberation Serif 12pt: 13.8) */
                CHECK(step > PD_PT(13) && step < PD_PT(15));
            }
        }

        pd_doc_free(back);
        pd_doc_free(d);
    }
}

/* the rules (PD_DRAW_RULE) on page 0 whose left edge is at x, at most n of their heights into h; how many */
static int rules_at(pd_doc* d, pd_sp x, pd_sp* h, int n) {
    pd_layout* L = NULL;
    int32_t cnt = 0, k, c = 0;
    pd_draw* it;

    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK &&
            pd_layout_page_items(L, 0, NULL, 0, &cnt) == PD_OK && (it = (pd_draw*)malloc(((size_t)cnt + 1) *
                    sizeof(pd_draw))) != NULL) {
        pd_layout_page_items(L, 0, it, cnt, &cnt);

        for (k = 0; k < cnt; k++) {
            if (it[k].kind == PD_DRAW_RULE && it[k].x == x) {
                if (c < n) {
                    h[c] = it[k].h;
                }

                c++;
            }
        }

        free(it);
    }

    pd_layout_free(L);
    return c;
}

/* A bar tab is a vertical rule through each line of its paragraph at the stop (it was dropped), and is written
   back as one; the tab stop after it still takes the text. */
static void test_docx_bar_tab(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:tabs><w:tab w:val=\"bar\" w:pos=\"2880\"/>"
                    "<w:tab w:val=\"left\" w:pos=\"4320\"/></w:tabs></w:pPr><w:r><w:t>A</w:t><w:tab/><w:t>B</w:t><w:br/>"
                    "<w:t>C</w:t></w:r></w:p></w:body></w:document>",
                    NULL);
    pd_doc* back = NULL;
    pd_sp h[4];
    buf_t b;
    int pass;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    memset(&b, 0, sizeof(b));
    CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
    CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) == PD_OK);
    free(b.p);

    for (pass = 0; pass < 2 && have_layout_font(); pass++) {
        pd_doc* x = pass ? back : d;
        pd_sp lo, hi;
        pd_layout* L = NULL;

        if (!x) {
            continue;
        }

        /* the margin is 1in: the bar 2in into the column, half a point wide, down both lines */
        CHECK(rules_at(x, PD_PT(72 + 144) - PD_PT(1) / 4, h, 4) == 2 && h[0] > PD_PT(10) && h[1] > PD_PT(10));
        pd_doc_set_font_resolver(x, layout_resolver, NULL);

        if (pd_layout_new(x, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
            CHECK(glyph_span(L, 'B', &lo, &hi) && lo == PD_PT(72 + 216));   /* B at the 3in stop, past the bar */
        }

        pd_layout_free(L);
    }

    pd_doc_free(back);
    pd_doc_free(d);
}

/* the leftmost x of the glyphs on each baseline of page 0, region 0, top to bottom, into xs (at most n); how many */
static int line_starts(pd_doc* d, pd_sp* xs, pd_sp* ys, int n) {
    pd_layout* L = NULL;
    int32_t cnt = 0, k, j, c = 0;
    pd_draw* it;

    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK &&
            pd_layout_page_items(L, 0, NULL, 0, &cnt) == PD_OK && (it = (pd_draw*)malloc(((size_t)cnt + 1) *
                    sizeof(pd_draw))) != NULL) {
        pd_layout_page_items(L, 0, it, cnt, &cnt);

        for (k = 0; k < cnt; k++) {
            if (it[k].kind != PD_DRAW_GLYPH || it[k].region != 0) {
                continue;
            }

            for (j = 0; j < c && ys[j] != it[k].y; j++) {
            }

            if (j == c && c < n) {
                ys[c] = it[k].y;
                xs[c++] = it[k].x;
            } else if (j < c && it[k].x < xs[j]) {
                xs[j] = it[k].x;
            }
        }

        free(it);
    }

    pd_layout_free(L);

    for (k = 1; k < c; k++) {   /* top to bottom */
        for (j = k; j > 0 && ys[j - 1] > ys[j]; j--) {
            pd_sp t = ys[j];

            ys[j] = ys[j - 1];
            ys[j - 1] = t;
            t = xs[j];
            xs[j] = xs[j - 1];
            xs[j - 1] = t;
        }
    }

    return c;
}

/* A drop cap (framePr dropCap="drop" lines="3") is a float of its letter beside the first three lines of the
   paragraph after it, which wrap round it, and is written back as a frame; a frame with a width and wrapping
   around is a float of that width at the left with the text beside it. */
static void test_docx_drop_cap(void) {
    static const char lorem[] = "<w:p><w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr><w:t>he quick brown fox jumps over the lazy dog and keeps on running far "
                                "away across the wide green fields until the sun goes down behind the hills and the "
                                "night comes on, and then it runs again all through the dark and quiet night, past "
                                "farms and rivers and sleeping towns, never stopping once to rest its tired legs, and on "
                                "and on it goes over the hills and far away, until the morning light comes up again "
                                "over the edge of the world and the birds begin to sing in the trees.</w:t>"
                                "</w:r></w:p>";
    char xml[2400];
    pd_doc* d, *back = NULL;
    buf_t b;
    int pass;

    snprintf(xml, sizeof(xml), "<w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:framePr w:dropCap=\"drop\" "
             "w:lines=\"3\" w:wrap=\"around\" w:vAnchor=\"text\" w:hAnchor=\"text\"/><w:spacing w:line=\"680\" "
             "w:lineRule=\"exact\"/></w:pPr><w:r><w:rPr><w:position w:val=\"-8\"/><w:sz w:val=\"90\"/></w:rPr>"
             "<w:t>T</w:t></w:r></w:p>%s</w:body></w:document>", lorem);
    d = docx_doc("word/document.xml", xml, NULL);
    CHECK(d != NULL);

    if (!d) {
        return;
    }

    memset(&b, 0, sizeof(b));
    CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
    CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &back) == PD_OK);

    {
        char* dx = docx_part(&b, "word/document.xml");

        CHECK(strstr(dx, "<w:framePr w:dropCap=\"drop\" w:lines=\"3\"") != NULL);
        free(dx);
    }

    free(b.p);

    for (pass = 0; pass < 2; pass++) {
        pd_doc* x = pass ? back : d;
        pd_block_id sec, fl;
        pd_block_info bi;
        pd_float_props fp;
        pd_sp xs[16], ys[16];
        int n;

        if (!x) {
            continue;
        }

        sec = pd_doc_child(x, pd_doc_root(x), 0);
        fl = pd_doc_child(x, sec, 0);
        CHECK(pd_doc_block_info(x, fl, &bi) == PD_OK && bi.kind == PD_BLOCK_FLOAT);
        CHECK(pd_doc_float_props(x, fl, &fp) == PD_OK && fp.frame == PD_FRAME_DROP && fp.frame_lines == 3 &&
              fp.wrap == PD_WRAP_LEFT);

        if (!have_layout_font()) {
            continue;
        }

        /* the cap at the margin, the first three lines of text right of it, the fourth back at the margin */
        n = line_starts(x, xs, ys, 16);
        CHECK(n >= 5 && xs[0] > PD_PT(72 + 20) && xs[1] == xs[0] && xs[2] == xs[0] && xs[3] == PD_PT(72));
        {
            pd_layout* L = NULL;
            pd_sp lo, hi;

            if (pd_layout_new(x, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
                CHECK(glyph_span(L, 'T', &lo, &hi) && lo == PD_PT(72) && hi <= xs[0]);
            }

            pd_layout_free(L);
        }
    }

    pd_doc_free(back);
    pd_doc_free(d);

    /* a frame 2in wide at the margin, the text round it */
    snprintf(xml, sizeof(xml), "<w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:framePr w:w=\"2880\" "
             "w:hSpace=\"180\" w:wrap=\"around\" w:hAnchor=\"margin\" w:x=\"0\"/></w:pPr><w:r><w:t>Boxed</w:t></w:r>"
             "</w:p>%s</w:body></w:document>", lorem);
    d = docx_doc("word/document.xml", xml, NULL);
    CHECK(d != NULL);

    if (d && have_layout_font()) {
        pd_float_props fp;
        pd_sp xs[16], ys[16];
        int n;

        CHECK(pd_doc_float_props(d, pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), &fp) == PD_OK &&
              fp.frame == PD_FRAME_BOX && fp.width == PD_PT(144));
        n = line_starts(d, xs, ys, 16);
        CHECK(n >= 1 && xs[0] == PD_PT(72 + 144 + 9));
    }

    pd_doc_free(d);
}

/* Text boxes that lost their text: a DrawingML text box in the line (wp:inline), a VML text box (v:textbox in a
   w:pict) and VML WordArt (v:textpath, a header's watermark) are floats holding their text, laid out and written
   back (as text boxes). The watermark is centred on the page's text area, in the fill made lighter by its
   opacity (silver at half: E0E0E0). */
static void test_docx_text_boxes(void) {
    pd_doc* d = docx_doc(
                    "word/_rels/document.xml.rels",
                    "<Relationships xmlns=\"r\"><Relationship Id=\"rId1\" Type=\"t/header\" Target=\"header1.xml\"/>"
                    "</Relationships>",
                    "word/header1.xml",
                    "<w:hdr xmlns:w=\"w\" xmlns:v=\"v\"><w:p><w:r><w:pict><v:shape style=\"position:absolute;margin-left:0;"
                    "margin-top:0;width:400pt;height:100pt;rotation:315;z-index:-251657216;mso-position-horizontal:center;"
                    "mso-position-horizontal-relative:margin;mso-position-vertical:center;mso-position-vertical-relative:margin\" "
                    "fillcolor=\"silver\" stroked=\"f\"><v:fill opacity=\".5\"/><v:textpath style=\"font-family:&quot;Calibri&quot;;"
                    "font-size:1pt\" string=\"DRAFT\"/></v:shape></w:pict></w:r></w:p></w:hdr>",
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\" xmlns:r=\"r\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wps=\"wps\" xmlns:v=\"v\" "
                    "xmlns:w10=\"w10\"><w:body>"
                    "<w:p><w:r><w:t>Before.</w:t></w:r></w:p>"
                    "<w:p><w:r><w:drawing><wp:inline><wp:extent cx=\"1828800\" cy=\"457200\"/><wp:docPr id=\"1\" name=\"T\"/>"
                    "<a:graphic><a:graphicData><wps:wsp><wps:txbx><w:txbxContent><w:p><w:r><w:t>Inline</w:t></w:r></w:p>"
                    "</w:txbxContent></wps:txbx></wps:wsp></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>"
                    "<w:p><w:r><w:pict><v:shape style=\"position:absolute;margin-left:0;margin-top:0;width:144pt;height:36pt;"
                    "z-index:1\"><v:textbox><w:txbxContent><w:p><w:r><w:t>Legacy</w:t></w:r></w:p></w:txbxContent></v:textbox>"
                    "<w10:wrap type=\"square\"/></v:shape></w:pict></w:r><w:r><w:t>After.</w:t></w:r></w:p>"
                    "<w:sectPr><w:headerReference w:type=\"default\" r:id=\"rId1\"/><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
                    "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" "
                    "w:footer=\"720\"/></w:sectPr></w:body></w:document>",
                    NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        char shown[512];
        pd_section_props sp;
        pd_block_id fl, p;
        pd_float_props fp;

        CHECK(d != NULL);

        if (!d) {
            break;
        }

        CHECK(pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp) == PD_OK && sp.header);
        fl = pd_doc_child(d, sp.header, 0);
        p = pd_doc_child(d, fl, 0);
        CHECK(pd_doc_float_props(d, fl, &fp) == PD_OK && fp.wrap == PD_WRAP_BEHIND && fp.width == PD_PT(400));
        CHECK(chars_at(d, p, 0).color == 0xFFE0E0E0u);

        if (pass == 0) {    /* centred on the text area: the page 612 x 792pt, 1in margins */
            CHECK(fp.offset_x == PD_PT(34) && fp.offset_y == PD_PT(274) && fp.offset_from == PD_FROM_MARGIN);
        }

        if (have_layout_font()) {
            layout_text(d, -1, shown, sizeof(shown));
            CHECK(strstr(shown, "DRAFT") && strstr(shown, "Inline") && strstr(shown, "Legacy") &&
                  strstr(shown, "After."));
        }
    }

    pd_doc_free(d);
}

#define FLD(ins, res) "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText xml:space=\"preserve\">" ins \
    "</w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>" res "</w:t></w:r>" \
    "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"

/* Fields nest (IF { PAGE } = 1 broke the outer field, shown "1 1"); a field Parade does not compute keeps its
   instruction and Word's result, shown and written back as a field; dates follow their \@ picture; document
   properties, IF and = are worked out when read; STYLEREF "Heading 1" is a running head. */
static void test_docx_nested_fields(void) {
    pd_doc* d = docx_doc(
                    "docProps/core.xml",
                    "<cp:coreProperties xmlns:cp=\"cp\" xmlns:dc=\"dc\" xmlns:dcterms=\"dcterms\"><dc:title>MyTitle</dc:title>"
                    "<dcterms:modified>2026-10-06T03:40:00Z</dcterms:modified></cp:coreProperties>",
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\"><w:body>"
                    "<w:p><w:r><w:t>A:</w:t></w:r><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
                    "<w:r><w:instrText xml:space=\"preserve\"> IF </w:instrText></w:r>" FLD(" PAGE ", "1")
                    "<w:r><w:instrText xml:space=\"preserve\"> = 1 \"first\" \"other\" </w:instrText></w:r>"
                    "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:t>first</w:t></w:r>"
                    "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>"
                    "<w:p><w:r><w:t>B:</w:t></w:r>" FLD(" DOCPROPERTY Title ", "old") "</w:p>"
                    "<w:p><w:r><w:t>C:</w:t></w:r>" FLD(" IF \"a\" = \"a\" \"yes\" \"no\" ", "no") "</w:p>"
                    "<w:p><w:r><w:t>D:</w:t></w:r>" FLD(" = 2+3*4 ", "0") "</w:p>"
                    "<w:p><w:r><w:t>E:</w:t></w:r>" FLD(" MERGEFIELD Name ", "Kept") "</w:p>"
                    "<w:p><w:r><w:t>F:</w:t></w:r>" FLD(" SAVEDATE \\@ \"dddd, MMMM d, yyyy\" ", "x") "</w:p>"
                    "<w:p><w:r><w:t>G:</w:t></w:r><w:fldSimple w:instr=\" DATE \\@ &quot;yyyy&quot; \"><w:r><w:t>z</w:t></w:r>"
                    "</w:fldSimple></w:p>"
                    "<w:p><w:r><w:t>H:</w:t></w:r>" FLD(" STYLEREF \"Heading 1\" ", "q") "</w:p>"
                    "</w:body></w:document>",
                    NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        char shown[600];
        const char* g;
        pd_block_id p;
        pd_inline o;

        CHECK(d != NULL);

        if (!d) {
            break;
        }

        p = pd_doc_next_paragraph(d, 0);
        CHECK(pd_doc_inline_at(d, at(p, 2), &o) == PD_OK && o.kind == PD_INLINE_FIELD && o.field == PD_FIELD_KEPT &&
              o.source_len > 4 && strstr(o.source, "\xEF\xBF\xB9 PAGE \xEF\xBF\xBA" "1\xEF\xBF\xBB"));     /* PAGE in IF */

        if (!have_layout_font()) {
            continue;
        }

        layout_text(d, 0, shown, sizeof(shown));
        CHECK(strstr(shown, "A:firstB:MyTitleC:yesD:14E:KeptF:Tuesday,October6,2026G:") != NULL);
        g = strstr(shown, "G:");
        CHECK(g && g[2] >= '2' && g[2] <= '9' && g[5] >= '0' && g[5] <= '9' && g[6] == 'H');
    }

    {
        /* written back as fields, the nested one inside the IF's code */
        buf_t b = { NULL, 0 };
        char* xml;

        CHECK(d && pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        xml = docx_part(&b, "word/document.xml");
        CHECK(strstr(xml, "<w:instrText xml:space=\"preserve\"> IF </w:instrText></w:r><w:r><w:fldChar "
                     "w:fldCharType=\"begin\"/>") && strstr(xml, "MERGEFIELD Name") &&
              strstr(xml, "STYLEREF &quot;Heading 1&quot;"));
        free(xml);
        free(b.p);
    }

    pd_doc_free(d);
}

/* Comments drawn as Word prints them (pd_layout_set_comments): the page a pane wider, a balloon there with the
   author and each paragraph of the text on its own line, a line out to it from the commented text and the box
   round it in the author's colour; a reply under it. Off, the page is as it was and nothing is drawn. */
static void test_comment_balloons(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\" xmlns:w14=\"w14\"><w:body>"
                    "<w:p><w:r><w:t xml:space=\"preserve\">The </w:t></w:r><w:commentRangeStart w:id=\"7\"/><w:r><w:t>fox</w:t>"
                    "</w:r><w:commentRangeEnd w:id=\"7\"/><w:r><w:commentReference w:id=\"7\"/></w:r>"
                    "<w:r><w:commentReference w:id=\"8\"/></w:r></w:p></w:body></w:document>",
                    "word/comments.xml",
                    "<w:comments xmlns:w=\"w\" xmlns:w14=\"w14\">"
                    "<w:comment w:id=\"7\" w:author=\"Cy\"><w:p w14:paraId=\"0A000001\"><w:r><w:t>Which fox?</w:t></w:r></w:p>"
                    "<w:p w14:paraId=\"0A000002\"><w:r><w:t>Really.</w:t></w:r></w:p></w:comment>"
                    "<w:comment w:id=\"8\" w:author=\"Ann\"><w:p w14:paraId=\"0A000003\"><w:r><w:t>Red.</w:t></w:r></w:p>"
                    "</w:comment></w:comments>",
                    "word/commentsExtended.xml",
                    "<w15:commentsEx xmlns:w15=\"w15\"><w15:commentEx w15:paraId=\"0A000003\" w15:paraIdParent=\"0A000002\"/>"
                    "</w15:commentsEx>",
                    NULL);
    pd_layout* L = NULL;
    int mode;

    CHECK(d != NULL);

    if (!d || !have_layout_font()) {
        pd_doc_free(d);
        return;
    }

    pd_doc_set_font_resolver(d, layout_resolver, NULL);
    CHECK(pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK);

    for (mode = 0; mode < 2 && L; mode++) {
        pd_page_info pi;
        pd_draw* it;
        int32_t n = 0, k, paths = 0;
        char text[256];
        size_t o = 0;
        pd_sp y_which = 0, y_really = 0, y_red = 0;

        pd_layout_set_comments(L, mode ? PD_LAYOUT_COMMENTS_MARGIN : PD_LAYOUT_COMMENTS_NONE);
        CHECK(pd_layout_page_info(L, 0, &pi) == PD_OK);
        CHECK(pd_layout_page_items(L, 0, NULL, 0, &n) == PD_OK);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, 0, it, n, &n);

        for (k = 0; k < n; k++) {
            if (it[k].region != 6) {
                continue;
            }

            paths += it[k].kind == PD_DRAW_PATH;

            if (it[k].kind == PD_DRAW_GLYPH && it[k].text < 128 && o + 1 < sizeof(text)) {
                text[o++] = (char)it[k].text;
                y_which = it[k].text == 'W' && !y_which ? it[k].y : y_which;
                y_really = it[k].text == 'R' && !y_really ? it[k].y : y_really;
                y_red = it[k].text == 'd' && it[k].y > y_really && y_really ? it[k].y : y_red;
                CHECK(it[k].x > PD_PT(595));    /* in the pane, right of the A4 page */
            }
        }

        text[o] = '\0';
        free(it);

        if (mode == 0) {
            CHECK(o == 0 && paths == 0);
            CHECK(pi.width < PD_PT(600));
        } else {
            CHECK(pi.width > PD_PT(595) + PD_LAYOUT_COMMENT_PANE - PD_PT(1));
            CHECK(strcmp(text, "Cy:Whichfox?Really.Ann:Red.") == 0);
            CHECK(paths == 2);  /* one box and its line: the reply shares them */
            CHECK(y_which > 0 && y_really > y_which && y_red > y_really);
        }
    }

    pd_layout_free(L);
    pd_doc_free(d);
}

/* Moves stay moves (moveFrom/moveTo: a deletion and an insertion sharing a move number, written back as moves)
   and a run's formatting change (rPrChange) is a revision of kind format holding the old formatting: listed for
   the margin, written back, accepted (kept as it is) or rejected (the old formatting back). */
static void test_docx_moves_formats(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\"><w:body>"
                    "<w:p><w:moveFromRangeStart w:id=\"1\" w:name=\"move1\" w:author=\"Al\"/><w:moveFrom w:id=\"2\" w:author=\"Al\">"
                    "<w:r><w:t>Gone</w:t></w:r></w:moveFrom><w:moveFromRangeEnd w:id=\"1\"/><w:r><w:t xml:space=\"preserve\"> stay "
                    "</w:t></w:r><w:r><w:rPr><w:b/><w:rPrChange w:id=\"3\" w:author=\"Bo\"><w:rPr><w:i/></w:rPr></w:rPrChange>"
                    "</w:rPr><w:t>bold</w:t></w:r></w:p>"
                    "<w:p><w:moveToRangeStart w:id=\"4\" w:name=\"move1\" w:author=\"Al\"/><w:moveTo w:id=\"5\" w:author=\"Al\">"
                    "<w:r><w:t>Gone</w:t></w:r></w:moveTo><w:moveToRangeEnd w:id=\"4\"/></w:p>"
                    "</w:body></w:document>",
                    NULL);
    int pass;

    for (pass = 0; pass < 2; pass++) {
        pd_block_id p0, p1;
        pd_char_props a, f, t;
        pd_revision ra, rt, rf;
        pd_range r;

        CHECK(d != NULL);

        if (!d) {
            break;
        }

        p0 = pd_doc_next_paragraph(d, 0);
        p1 = pd_doc_next_paragraph(d, p0);
        a = chars_at(d, p0, 0);
        t = chars_at(d, p1, 0);
        f = chars_at(d, p0, 10);
        CHECK(pd_doc_revision_get(d, a.revision, &ra) == PD_OK && ra.kind == PD_REV_DELETE && ra.move > 0);
        CHECK(pd_doc_revision_get(d, t.revision, &rt) == PD_OK && rt.kind == PD_REV_INSERT && rt.move == ra.move);
        CHECK(f.weight == 700 && pd_doc_revision_get(d, f.revision, &rf) == PD_OK && rf.kind == PD_REV_FORMAT &&
              (rf.old.mask & PD_CP_ITALIC) && rf.old.italic && !strcmp(rf.author, "Bo"));

        if (have_layout_font()) {   /* the formatting change listed for the margin */
            pd_layout* L = NULL;
            pd_markup_item mk[8];
            int32_t n = 0, k, fmt = 0;

            pd_doc_set_font_resolver(d, layout_resolver, NULL);

            if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK &&
                    pd_layout_page_markup(L, 0, mk, 8, &n) == PD_OK) {
                for (k = 0; k < n; k++) {
                    fmt += mk[k].kind == PD_MARK_FORMAT && mk[k].range.start.offset == 10;
                }
            }

            CHECK(fmt == 1);
            pd_layout_free(L);
        }

        if (pass == 0) {
            buf_t b = { NULL, 0 };
            char* xml;

            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            xml = docx_part(&b, "word/document.xml");
            CHECK(strstr(xml, "<w:moveFrom ") && strstr(xml, "<w:moveTo ") && strstr(xml, "w:name=\"move") &&
                  strstr(xml, "<w:rPrChange "));
            free(xml);
            free(b.p);
            d = docx_again(d);
            continue;
        }

        {
            /* kept in Parade's own format */
            buf_t b = { NULL, 0 };
            pd_doc* j = NULL;

            CHECK(pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b) == PD_OK && pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &j) == PD_OK);
            CHECK(j && pd_doc_revision_get(j, chars_at(j, pd_doc_next_paragraph(j, 0), 10).revision, &rf) == PD_OK &&
                  rf.kind == PD_REV_FORMAT && rf.old.italic && pd_doc_revision_get(j, chars_at(j, pd_doc_next_paragraph(j, 0),
                          0).revision, &ra) == PD_OK && ra.move > 0);
            pd_doc_free(j);
            free(b.p);
        }

        /* rejected: italic and not bold, no revision; accepted (undone first): bold, no revision */
        r.start = at(p0, 10);
        r.end = at(p0, 14);
        CHECK(pd_doc_revision_resolve(d, r, 0) == PD_OK);
        f = chars_at(d, p0, 10);
        CHECK(f.weight == 400 && f.italic && !f.revision);
        CHECK(pd_doc_undo(d) == PD_OK);
        CHECK(pd_doc_revision_resolve(d, r, 1) == PD_OK);
        f = chars_at(d, p0, 10);
        CHECK(f.weight == 700 && !f.italic && !f.revision);
    }

    pd_doc_free(d);
}

/* A REF field shows the bookmarked text, not Word's stale result, with its \h link; a bookmark holding a
   caption's number keeps Word's (test_docx_fields). */
static void test_docx_ref_text(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\"><w:body>"
                    "<w:p><w:bookmarkStart w:id=\"3\" w:name=\"intro\"/><w:r><w:t>Hello world</w:t></w:r>"
                    "<w:bookmarkEnd w:id=\"3\"/><w:r><w:t xml:space=\"preserve\"> more</w:t></w:r></w:p>"
                    "<w:p><w:r><w:t xml:space=\"preserve\">See </w:t></w:r>" FLD(" REF intro \\h ", "stale text")
                    "<w:r><w:t>.</w:t></w:r></w:p></w:body></w:document>",
                    NULL);
    int pass;

    for (pass = 0; pass < 2; pass++, d = docx_again(d)) {
        pd_block_id p;
        const char* t;
        uint32_t n, k;
        int links = 0;
        pd_inline o;

        CHECK(d != NULL);

        if (!d) {
            break;
        }

        p = pd_doc_next_paragraph(d, pd_doc_next_paragraph(d, 0));
        pd_doc_para_text(d, p, &t, &n);
        CHECK(has_bytes(t, n, "Hello world") && !has_bytes(t, n, "stale"));

        for (k = 0; k + 3 <= n; k++) {
            if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at(p, k), &o) == PD_OK) {
                links += o.kind == PD_INLINE_LINK && o.source_len == 6 && !memcmp(o.source, "#intro", 6);
            }
        }

        CHECK(links == 1);
    }

    pd_doc_free(d);
}

/* Word's character scale (w:w): a run at 200% is twice as wide, its glyphs drawn scaled (PDF Tz), and the scale
   is written back and kept in Parade's format. */
static void test_docx_char_scale(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>MM</w:t></w:r><w:r><w:rPr><w:w w:val=\"200\"/></w:rPr>"
                    "<w:t>NN</w:t></w:r></w:p></w:body></w:document>",
                    NULL);
    int pass;

    for (pass = 0; pass < 3 && d; pass++) {
        pd_layout* L = NULL;

        CHECK(chars_at(d, pd_doc_next_paragraph(d, 0), 2).hscale == 2000);

        if (have_layout_font()) {
            pd_sp ml = 0, mh = 0, nl = 0, nh = 0;
            pd_draw* it;
            int32_t n = 0, k, scaled = 0;

            pd_doc_set_font_resolver(d, layout_resolver, NULL);

            if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
                CHECK(glyph_span(L, 'M', &ml, &mh) && glyph_span(L, 'N', &nl, &nh));
                /* N is 0.81 of M in this face: two Ns at 200% are 1.6 times two Ms, unscaled 0.8 */
                CHECK(nh - nl > (mh - ml) * 3 / 2);
                pd_layout_page_items(L, 0, NULL, 0, &n);
                it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
                pd_layout_page_items(L, 0, it, n, &n);

                for (k = 0; k < n; k++) {
                    scaled += it[k].kind == PD_DRAW_GLYPH && it[k].text == 'N' && it[k].scale == 2 * 65536;
                }

                free(it);
                CHECK(scaled == 2);
            }

            pd_layout_free(L);
        }

        if (pass == 0) {
            buf_t b = { NULL, 0 };
            char* xml;

            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            xml = docx_part(&b, "word/document.xml");
            CHECK(strstr(xml, "<w:w w:val=\"200\"/>") != NULL);
            free(xml);
            free(b.p);
            d = docx_again(d);
        } else if (pass == 1) {
            buf_t b = { NULL, 0 };
            pd_doc* j = NULL;

            CHECK(pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b) == PD_OK && pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &j) == PD_OK);
            free(b.p);
            pd_doc_free(d);
            d = j;
        }
    }

    pd_doc_free(d);
}

/* A plain picture cropped (a:srcRect, in the line or floating) is drawn cropped -- the whole picture larger,
   clipped to its box -- and the crop is written back and kept in Parade's format. */
static void test_docx_picture_crop(void) {
    pd_doc* d = docx_doc(
                    "word/_rels/document.xml.rels",
                    "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
                    "</Relationships>",
                    "word/media/image1.png", "tests/data/rgba.png",
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body><w:p>"
                    "<w:r><w:drawing><wp:inline><wp:extent cx=\"1270000\" cy=\"635000\"/><a:graphic><a:graphicData><pic:pic>"
                    "<pic:blipFill><a:blip r:embed=\"rId5\"/><a:srcRect l=\"25000\" t=\"10000\" r=\"25000\" b=\"10000\"/>"
                    "<a:stretch><a:fillRect/></a:stretch></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline>"
                    "</w:drawing></w:r></w:p></w:body></w:document>",
                    NULL);
    int pass;

    for (pass = 0; pass < 3 && d; pass++) {
        pd_block_id p = pd_doc_next_paragraph(d, 0);
        pd_inline o;

        CHECK(pd_doc_inline_at(d, at(p, 0), &o) == PD_OK && o.kind == PD_INLINE_IMAGE && o.crop[0] == 25000 &&
              o.crop[1] == 10000 && o.crop[2] == 25000 && o.crop[3] == 10000);

        if (have_layout_font()) {   /* 100 x 50pt shown, of a picture drawn 200 x 62.5pt */
            pd_layout* L = NULL;
            pd_draw* it;
            int32_t n = 0, k, found = 0;

            pd_doc_set_font_resolver(d, layout_resolver, NULL);

            if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
                pd_layout_page_items(L, 0, NULL, 0, &n);
                it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
                pd_layout_page_items(L, 0, it, n, &n);

                for (k = 0; k < n; k++) {
                    if (it[k].kind == PD_DRAW_IMAGE) {
                        found = 1;
                        CHECK(it[k].clip_w == PD_PT(100) && it[k].clip_h == PD_PT(50));
                        CHECK(it[k].w == PD_PT(200) && it[k].h == PD_PT(62.5));
                        CHECK(it[k].x == it[k].clip_x - PD_PT(50) && it[k].y == it[k].clip_y - PD_PT(6.25));
                    }
                }

                free(it);
            }

            CHECK(found);
            pd_layout_free(L);
        }

        if (pass == 0) {
            buf_t b = { NULL, 0 };
            char* xml;

            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            xml = docx_part(&b, "word/document.xml");
            CHECK(strstr(xml, "<a:srcRect l=\"25000\" t=\"10000\" r=\"25000\" b=\"10000\"/>") != NULL);
            free(xml);
            free(b.p);
            d = docx_again(d);
        } else if (pass == 1) {
            buf_t b = { NULL, 0 };
            pd_doc* j = NULL;

            CHECK(pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b) == PD_OK && pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &j) == PD_OK);
            free(b.p);
            pd_doc_free(d);
            d = j;
        }
    }

    pd_doc_free(d);
}

/* Changes of whole blocks: a paragraph's properties (pPrChange), its mark deleted (w:del in the mark's rPr) and a
   table row deleted (w:del in trPr) are revisions of the paragraph and the row: listed for the margin, the row's
   text shown as deleted text, written back, kept in Parade's format; rejected, the paragraph gets its old
   properties and the row stays; accepted, the paragraph joins the next and the row goes. */
static void test_docx_block_revisions(void) {
    pd_doc* d = docx_doc(
                    "word/document.xml",
                    "<w:document xmlns:w=\"w\"><w:body>"
                    "<w:p><w:pPr><w:jc w:val=\"center\"/><w:rPr><w:del w:id=\"1\" w:author=\"Ed\"/></w:rPr>"
                    "<w:pPrChange w:id=\"2\" w:author=\"Ed\"><w:pPr><w:jc w:val=\"right\"/></w:pPr></w:pPrChange></w:pPr>"
                    "<w:r><w:t>One</w:t></w:r></w:p>"
                    "<w:p><w:r><w:t>Two</w:t></w:r></w:p>"
                    "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>Kept</w:t></w:r></w:p></w:tc></w:tr>"
                    "<w:tr><w:trPr><w:del w:id=\"3\" w:author=\"Ed\"/></w:trPr><w:tc><w:p><w:r><w:t>Zed</w:t></w:r></w:p></w:tc></w:tr>"
                    "</w:tbl><w:p/></w:body></w:document>",
                    NULL);
    int pass;

    for (pass = 0; pass < 3 && d; pass++) {
        pd_block_id sec = pd_doc_child(d, pd_doc_root(d), 0), p1 = pd_doc_child(d, sec, 0), tbl = pd_doc_child(d, sec, 2);
        pd_block_id row2 = pd_doc_child(d, tbl, 1);
        pd_para_attrs pa;
        pd_row_props rp;
        pd_revision rv;
        pd_block_info bi;

        CHECK(pd_doc_para_attrs(d, p1, &pa) == PD_OK && pd_doc_revision_get(d, pa.mark_rev, &rv) == PD_OK &&
              rv.kind == PD_REV_DELETE && !strcmp(rv.author, "Ed"));
        CHECK(pd_doc_revision_get(d, pa.format_rev, &rv) == PD_OK && rv.kind == PD_REV_FORMAT &&
              (rv.old_para.mask & PD_PP_ALIGN) && rv.old_para.align == PD_ALIGN_RIGHT);
        CHECK(pd_doc_block_info(d, tbl, &bi) == PD_OK && bi.kind == PD_BLOCK_TABLE && bi.child_count == 2);
        CHECK(pd_doc_row_props(d, row2, &rp) == PD_OK && pd_doc_revision_get(d, rp.revision, &rv) == PD_OK &&
              rv.kind == PD_REV_DELETE);

        if (have_layout_font()) {   /* listed for the margin; the row's text in the author's colour */
            pd_layout* L = NULL;
            pd_markup_item mk[16];
            pd_draw* it;
            int32_t n = 0, k, fmt = 0, dels = 0, zed = 0;

            pd_doc_set_markup(d, PD_MARKUP_INLINE);
            pd_doc_set_font_resolver(d, layout_resolver, NULL);

            if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK &&
                    pd_layout_page_markup(L, 0, mk, 16, &n) == PD_OK) {
                for (k = 0; k < n; k++) {
                    fmt += mk[k].kind == PD_MARK_FORMAT && mk[k].range.start.block == p1;
                    dels += mk[k].kind == PD_MARK_DELETION;
                }

                pd_layout_page_items(L, 0, NULL, 0, &n);
                it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
                pd_layout_page_items(L, 0, it, n, &n);

                for (k = 0; k < n; k++) {
                    zed += it[k].kind == PD_DRAW_GLYPH && it[k].text == 'Z' && it[k].color == pd_doc_author_color(d, "Ed");
                }

                free(it);
            }

            CHECK(fmt == 1 && dels == 2 && zed == 1);
            pd_layout_free(L);
            pd_doc_set_markup(d, PD_MARKUP_BALLOONS);
        }

        if (pass == 0) {
            buf_t b = { NULL, 0 };
            char* xml;

            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            xml = docx_part(&b, "word/document.xml");
            CHECK(strstr(xml, "<w:rPr><w:del ") && strstr(xml, "<w:pPrChange ") && strstr(xml, "<w:jc w:val=\"right\"/>"));
            CHECK(strstr(xml, "<w:trPr><w:del ") != NULL);
            free(xml);
            free(b.p);
            d = docx_again(d);
        } else if (pass == 1) {
            buf_t b = { NULL, 0 };
            pd_doc* j = NULL;

            CHECK(pd_doc_save(d, PD_JDATA_TEXT, to_buf, &b) == PD_OK && pd_doc_load(b.p, b.n, PD_JDATA_AUTO, &j) == PD_OK);
            free(b.p);
            pd_doc_free(d);
            d = j;
        } else {
            pd_range r;
            pd_para_props pp;
            const char* t;
            uint32_t tn;

            r.start = at(p1, 0);
            r.end = at(pd_doc_child(d, sec, 3), 0);
            CHECK(pd_doc_revision_resolve(d, r, 0) == PD_OK);    /* rejected */
            CHECK(pd_doc_para_resolve(d, p1, &pp) == PD_OK && pp.align == PD_ALIGN_RIGHT);
            CHECK(pd_doc_para_attrs(d, p1, &pa) == PD_OK && !pa.mark_rev && !pa.format_rev);
            CHECK(pd_doc_block_info(d, tbl, &bi) == PD_OK && bi.child_count == 2);
            CHECK(pd_doc_row_props(d, row2, &rp) == PD_OK && !rp.revision);
            CHECK(pd_doc_undo(d) == PD_OK);
            CHECK(pd_doc_revision_resolve(d, r, 1) == PD_OK);    /* accepted */
            pd_doc_para_text(d, p1, &t, &tn);
            CHECK(tn == 6 && !memcmp(t, "OneTwo", 6));
            CHECK(pd_doc_para_resolve(d, p1, &pp) == PD_OK && pp.align == PD_ALIGN_CENTER);
            CHECK(pd_doc_block_info(d, tbl, &bi) == PD_OK && bi.child_count == 1);
        }
    }

    pd_doc_free(d);
}

/* the structure of the equations (m:oMath) in XML, one after another: each m: element's name and its children in
   brackets, properties (m:...Pr) left out but for a run's style and script, and the runs' text */
static void omml_shape(const char* x, char* out, size_t cap) {
    size_t o = 0;
    const char* p = x;
    int in_math = 0;

    out[0] = '\0';

    while ((p = strchr(p, '<')) != NULL && o + 64 < cap) {
        char name[32];
        size_t k = 0;
        int close = p[1] == '/';
        const char* q = p + 1 + close;

        if (strncmp(q, "m:", 2)) {
            p++;
            continue;
        }

        for (q += 2; *q && (isalnum((unsigned char)*q)) && k + 1 < sizeof(name); q++) {
            name[k++] = *q;
        }

        name[k] = '\0';

        if (!strcmp(name, "oMath")) {
            in_math = !close;
            o += (size_t)snprintf(out + o, cap - o, close ? "]" : "[");
            p = q;
            continue;
        }

        if (!in_math) {
            p = q;
            continue;
        }

        if (!close && k > 2 && !strcmp(name + k - 2, "Pr")) {   /* properties: only a run's style and script */
            char endtag[40];
            const char* e;

            snprintf(endtag, sizeof(endtag), "</m:%s>", name);
            e = strstr(q, endtag);

            if (!strcmp(name, "rPr") && e) {
                const char* v = q;

                while ((v = strstr(v, "m:val=\"")) != NULL && v < e) {
                    v += 7;
                    o += (size_t)snprintf(out + o, cap - o, "%.*s;", (int)(strchr(v, '"') - v), v);
                }
            }

            p = e ? e + strlen(endtag) : q;
            continue;
        }

        if (!close && !strcmp(name, "t")) {     /* the text */
            const char* g = strchr(q, '>'), *e = g ? strstr(g, "</m:t>") : NULL;

            if (g && e) {
                o += (size_t)snprintf(out + o, cap - o, "'%.*s'", (int)(e - g - 1), g + 1);
                p = e + 6;
                continue;
            }
        }

        if (!strcmp(name, "t")) {
            p = q;
            continue;
        }

        if (close) {
            o += (size_t)snprintf(out + o, cap - o, ")");
        } else {
            const char* g = strchr(q, '>');

            o += (size_t)snprintf(out + o, cap - o, "%s(%s", name, g && g[-1] == '/' ? ")" : "");
        }

        p = q;
    }
}

/* Equations from Word come back the same after a trip through Parade's LaTeX: fractions, n-ary operators with
   limits, matrices, delimiters with separators, accents, group characters, sub-and-superscripts, functions,
   equation arrays, boxes round them, phantoms, limits over and under, and runs in bold, double-struck and
   fraktur -- the m:oMath structure written out is the one read in. */
static void test_docx_omml_roundtrip(void) {
#define MR(t) "<m:r><m:t>" t "</m:t></m:r>"
#define MRS(pr, t) "<m:r><m:rPr>" pr "</m:rPr><m:t>" t "</m:t></m:r>"
#define EQ(x) "<w:p><m:oMath>" x "</m:oMath></w:p>"
    static const char body[] =
        EQ("<m:f><m:num>" MR("a") "</m:num><m:den>" MR("b") "</m:den></m:f>")
        EQ("<m:nary><m:naryPr><m:chr m:val=\"\xE2\x88\x91\"/></m:naryPr><m:sub>" MR("i=1") "</m:sub><m:sup>" MR("n")
           "</m:sup><m:e>" MR("i") "</m:e></m:nary>")
        EQ("<m:m><m:mr><m:e>" MR("a") "</m:e><m:e>" MR("b") "</m:e></m:mr><m:mr><m:e>" MR("c") "</m:e><m:e>" MR("d")
           "</m:e></m:mr></m:m>")
        EQ("<m:d><m:dPr><m:begChr m:val=\"[\"/><m:endChr m:val=\"]\"/></m:dPr><m:e>" MR("x") "</m:e><m:e>" MR("y")
           "</m:e></m:d>")
        EQ("<m:acc><m:accPr><m:chr m:val=\"\xCC\x82\"/></m:accPr><m:e>" MR("x") "</m:e></m:acc>")
        EQ("<m:groupChr><m:e>" MR("x+y") "</m:e></m:groupChr>")
        EQ("<m:sSubSup><m:e>" MR("x") "</m:e><m:sub>" MR("i") "</m:sub><m:sup>" MR("2") "</m:sup></m:sSubSup>")
        EQ("<m:func><m:fName>" MRS("<m:sty m:val=\"p\"/>", "sin") "</m:fName><m:e>" MR("x") "</m:e></m:func>")
        EQ("<m:eqArr><m:e>" MR("a=b") "</m:e><m:e>" MR("c=d") "</m:e></m:eqArr>")
        EQ("<m:borderBox><m:e>" MR("E") "</m:e></m:borderBox>")
        EQ("<m:phant><m:phantPr><m:show m:val=\"0\"/></m:phantPr><m:e>" MR("x") "</m:e></m:phant>")
        EQ("<m:limUpp><m:e>" MR("x") "</m:e><m:lim>" MR("n") "</m:lim></m:limUpp>")
        EQ(MRS("<m:sty m:val=\"b\"/>", "v") MRS("<m:scr m:val=\"double-struck\"/><m:sty m:val=\"p\"/>", "R")
           MRS("<m:scr m:val=\"fraktur\"/><m:sty m:val=\"p\"/>", "g"));
    char xml[6000], want[3000], got[3000];
    pd_doc* d;
    buf_t b = { NULL, 0 };

    snprintf(xml, sizeof(xml), "<w:document xmlns:w=\"w\" xmlns:m=\"m\"><w:body>%s</w:body></w:document>", body);
    d = docx_doc("word/document.xml", xml, NULL);
    CHECK(d != NULL);

    if (d && pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK) {
        char* out = docx_part(&b, "word/document.xml");

        omml_shape(xml, want, sizeof(want));
        omml_shape(out, got, sizeof(got));
        CHECK_STR(got, want);
        free(out);
    }

    free(b.p);
    pd_doc_free(d);
#undef MR
#undef MRS
#undef EQ
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

/* lists past the 64th abstractNum keep their numbering (a list using #70 was bullets), Word's other number
   formats are counted in them (01, 1st, ten-one in CJK, Twenty-one), and they come back from Parade's .docx */
static void test_docx_num_formats(void) {
    static const char* fm[4] = { "decimalZero", "ordinal", "chineseCounting", "cardinalText" };
    static const int st[4] = { 9, 1, 11, 21 };
    char* num = (char*)malloc(80000), *doc = (char*)malloc(4000), buf[32];
    size_t o = 0, q = 0;
    int i, k, pass;
    pd_doc* d, *t = NULL;
    buf_t b = { 0 };

    o += (size_t)sprintf(num + o, "<w:numbering xmlns:w=\"w\">");

    for (i = 0; i < 74; i++) {  /* abstractNum 70..73 have the formats; the rest are bullets */
        const char* f = i >= 70 ? fm[i - 70] : "bullet";

        o += (size_t)sprintf(num + o, "<w:abstractNum w:abstractNumId=\"%d\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"%d\"/>"
                             "<w:numFmt w:val=\"%s\"/><w:lvlText w:val=\"%s\"/></w:lvl></w:abstractNum>", i,
                             i >= 70 ? st[i - 70] : 1, f, i >= 70 ? "%1)" : "o");
    }

    for (i = 0; i < 74; i++) {
        o += (size_t)sprintf(num + o, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/></w:num>", i + 1, i);
    }

    sprintf(num + o, "</w:numbering>");
    q += (size_t)sprintf(doc + q, "<w:document xmlns:w=\"w\"><w:body>");

    for (i = 0; i < 4; i++) {
        for (k = 0; k < 2; k++) {
            q += (size_t)sprintf(doc + q, "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"%d\"/></w:numPr>"
                                 "</w:pPr><w:r><w:t>x</w:t></w:r></w:p>", 71 + i);
        }
    }

    sprintf(doc + q, "</w:body></w:document>");
    d = docx_doc("word/numbering.xml", num, "word/document.xml", doc, NULL);
    CHECK(d != NULL);

    for (pass = 0; d && pass < 2; pass++) {
        pd_doc* e = pass == 0 ? d : t;
        static const char* want[8] = { "09)", "10)", "1st)", "2nd)", "\xE5\x8D\x81\xE4\xB8\x80)",
                                       "\xE5\x8D\x81\xE4\xBA\x8C)", "Twenty-one)", "Twenty-two)"
                                     };

        if (!e) {
            CHECK(0);
            break;
        }

        for (i = 0; i < 8; i++) {
            label_of(e, pd_doc_child(e, pd_doc_child(e, pd_doc_root(e), 0), i), buf);
            CHECK_STR(buf, want[i]);
        }

        if (pass == 0) {    /* and once more through Parade's own .docx */
            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &t) == PD_OK);
        }
    }

    pd_doc_format_number(100, PD_NUM_CJK_COUNTING, buf, sizeof(buf));
    CHECK_STR(buf, "\xE4\xB8\x80\xE7\x99\xBE");     /* one hundred */
    pd_doc_format_number(105, PD_NUM_CJK_COUNTING, buf, sizeof(buf));
    CHECK_STR(buf, "\xE4\xB8\x80\xE7\x99\xBE\xE9\x9B\xB6\xE4\xBA\x94");     /* one hundred zero five */
    pd_doc_format_number(3, PD_NUM_CIRCLED, buf, sizeof(buf));
    CHECK_STR(buf, "\xE2\x91\xA2");
    pd_doc_format_number(12, PD_NUM_ORDINAL, buf, sizeof(buf));
    CHECK_STR(buf, "12th");
    pd_doc_format_number(30, PD_NUM_ORDINAL_TEXT, buf, sizeof(buf));
    CHECK_STR(buf, "Thirtieth");
    pd_doc_free(t);
    pd_doc_free(d);
    free(b.p);
    free(num);
    free(doc);
}

/* what Parade does not model goes back as it came: extended and custom properties, custom XML data with its
   own relationships, and settings.xml -- track changes, protection, the compatibility mode and extensions kept,
   merged in the schema's order with what Parade writes; through two saves, nothing doubled */
static void test_docx_kept_parts(void) {
    static const char* names[] = { "[Content_Types].xml", "_rels/.rels", "word/_rels/document.xml.rels",
                                   "word/document.xml", "word/settings.xml", "docProps/app.xml", "docProps/custom.xml",
                                   "customXml/item1.xml", "customXml/itemProps1.xml", "customXml/_rels/item1.xml.rels"
                                 };
    static const char* texts[] = {
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"><Default Extension=\"rels\" "
        "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/><Default Extension=\"xml\" "
        "ContentType=\"application/xml\"/><Override PartName=\"/word/document.xml\" ContentType=\"application/"
        "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/><Override PartName=\"/docProps/"
        "app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
        "<Override PartName=\"/docProps/custom.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
        "custom-properties+xml\"/><Override PartName=\"/customXml/itemProps1.xml\" ContentType=\"application/"
        "vnd.openxmlformats-officedocument.customXmlProperties+xml\"/></Types>",
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/"
        "document.xml\"/><Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
        "relationships/extended-properties\" Target=\"docProps/app.xml\"/><Relationship Id=\"rId4\" Type=\"http://"
        "schemas.openxmlformats.org/officeDocument/2006/relationships/custom-properties\" Target=\"docProps/"
        "custom.xml\"/></Relationships>",
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId9\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXml\" Target=\"../customXml/"
        "item1.xml\"/></Relationships>",
        "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>kept</w:t></w:r></w:p><w:sectPr><w:pgSz w:w=\"12240\" "
        "w:h=\"15840\"/></w:sectPr></w:body></w:document>",
        "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" xmlns:w15=\"x15\">"
        "<w:zoom w:percent=\"120\"/><w:mirrorMargins/><w:trackRevisions/><w:documentProtection w:edit=\"readOnly\" "
        "w:enforcement=\"1\"/><w:defaultTabStop w:val=\"720\"/><w:compat><w:compatSetting w:name=\"compatibilityMode\" "
        "w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"14\"/></w:compat><w:docVars><w:docVar w:name=\"a\" "
        "w:val=\"b\"/></w:docVars><w15:chartTrackingRefBased/></w:settings>",
        "<Properties xmlns=\"x\"><Application>Microsoft Office Word</Application><Company>Lab</Company></Properties>",
        "<Properties xmlns=\"y\"><property name=\"Grant\"><vt:lpwstr xmlns:vt=\"v\">R01</vt:lpwstr></property></Properties>",
        "<b:Sources xmlns:b=\"bib\"/>",
        "<ds:datastoreItem xmlns:ds=\"ds\" ds:itemID=\"{1}\"/>",
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\"><Relationship Id=\"rId1\" "
        "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/customXmlProps\" "
        "Target=\"itemProps1.xml\"/></Relationships>"
    };
    buf_t z = stored_zip(names, texts, 10), b = { 0 };
    pd_doc* d = NULL, *t = NULL;
    int pass;

    CHECK(pd_doc_import(z.p, z.n, PD_CONV_DOCX, &d) == PD_OK);

    for (pass = 0; d && pass < 2; pass++) {
        unsigned char* p;
        size_t n;
        char* st, *a, *c;

        b.n = 0;
        CHECK(pd_doc_export(pass ? t : d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "docProps/custom.xml", &n);
        CHECK(p && strstr((char*)p, "R01"));
        free(p);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "docProps/app.xml", &n);
        CHECK(p && strstr((char*)p, "<Company>Lab</Company>"));
        free(p);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "customXml/itemProps1.xml", &n);
        CHECK(p != NULL);
        free(p);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "customXml/_rels/item1.xml.rels", &n);
        CHECK(p != NULL);
        free(p);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "_rels/.rels", &n);
        CHECK(p && strstr((char*)p, "Target=\"docProps/custom.xml\"") &&
              strstr((char*)p, "/extended-properties\" Target=\"docProps/app.xml\""));
        free(p);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "word/_rels/document.xml.rels", &n);
        CHECK(p && strstr((char*)p, "/customXml\" Target=\"../customXml/item1.xml\""));
        free(p);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "[Content_Types].xml", &n);
        CHECK(p && strstr((char*)p, "PartName=\"/docProps/custom.xml\"") &&
              strstr((char*)p, "PartName=\"/customXml/itemProps1.xml\""));
        free(p);
        p = pd_zip_get((const unsigned char*)b.p, b.n, "word/settings.xml", &n);
        st = (char*)p;
        CHECK(st && strstr(st, "<w:trackRevisions/>") && strstr(st, "w:edit=\"readOnly\"") &&
              strstr(st, "<w15:chartTrackingRefBased/>") && strstr(st, "w:name=\"a\"") &&
              strstr(st, "xmlns:w15=\"x15\""));
        CHECK(st && strstr(st, "w:val=\"14\"") && !strstr(st, "w:val=\"15\""));  /* its own compatibility mode */
        a = st ? strstr(st, "<w:compat>") : NULL;
        CHECK(a && !strstr(a + 1, "<w:compat>"));     /* once */
        a = st ? strstr(st, "<w:mirrorMargins/>") : NULL;
        CHECK(a && !strstr(a + 1, "<w:mirrorMargins/>"));
        a = st ? strstr(st, "<w:trackRevisions/>") : NULL;     /* the schema's order */
        c = st ? strstr(st, "<w:defaultTabStop") : NULL;
        CHECK(a && c && a < c && strstr(st, "<w:zoom") < a);
        c = st ? strstr(st, "<w:docVars>") : NULL;
        CHECK(c && strstr(st, "<w:compat>") < c && c < strstr(st, "<w15:chartTrackingRefBased/>"));
        free(p);

        if (pass == 0) {
            CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &t) == PD_OK);
        }
    }

    pd_doc_free(t);
    pd_doc_free(d);
    free(b.p);
    free(z.p);
}

/* ------------------------------------------------------------------ */
/* tables and pages laid out                                          */
/* ------------------------------------------------------------------ */

/* a laid-out document (fonts from have_layout_font), NULL when it cannot be */
static pd_layout* laid_out(pd_doc* d) {
    pd_layout* L = NULL;

    if (!d || !have_layout_font()) {
        return NULL;
    }

    pd_doc_set_font_resolver(d, layout_resolver, NULL);

    if (pd_layout_new(d, &L) != PD_OK || pd_layout_update(L, NULL) != PD_OK) {
        pd_layout_free(L);
        return NULL;
    }

    return L;
}

/* the display list of a page, malloc'ed, *n items */
static pd_draw* page_draws(const pd_layout* L, int32_t page, int32_t* n) {
    pd_draw* it;

    *n = 0;

    if (pd_layout_page_items(L, page, NULL, 0, n) != PD_OK ||
            (it = (pd_draw*)malloc(((size_t) * n + 1) * sizeof(pd_draw))) == NULL) {
        return NULL;
    }

    pd_layout_page_items(L, page, it, *n, n);
    return it;
}

/* where the first glyph of the paragraph holding s is drawn: its page, pen x and baseline; 0 if nowhere */
static int text_drawn(const pd_doc* d, const pd_layout* L, const char* s, int32_t* page, pd_sp* x, pd_sp* y) {
    pd_block_id p = find_para(d, s);
    int32_t pg, k, n;

    for (pg = 0; p && pg < pd_layout_page_count(L); pg++) {
        pd_draw* it = page_draws(L, pg, &n);

        for (k = 0; it && k < n; k++) {
            if (it[k].kind == PD_DRAW_GLYPH && it[k].block == p) {
                *page = pg;
                *x = it[k].x;
                *y = it[k].y;
                free(it);
                return 1;
            }
        }

        free(it);
    }

    return 0;
}

#define WDOC(body) "<w:document xmlns:w=\"w\"><w:body>" body "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>" \
    "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" " \
    "w:gutter=\"0\"/></w:sectPr></w:body></w:document>"

/* A table of 40 columns (Word allows 63): every cell laid out in its own column, left to right, and the
   last ones still there after a trip through .docx and Parade's own format */
static void test_wide_table(void) {
    char* body = (char*)malloc(32768), *p;
    pd_doc* d;
    pd_layout* L;
    int pass, c;

    if (!body) {
        return;
    }

    p = body + sprintf(body, "<w:document xmlns:w=\"w\"><w:body><w:tbl><w:tblGrid>");

    for (c = 0; c < 40; c++) {
        p += sprintf(p, "<w:gridCol w:w=\"220\"/>");
    }

    p += sprintf(p, "</w:tblGrid><w:tr>");

    for (c = 0; c < 40; c++) {
        p += sprintf(p, "<w:tc><w:p><w:r><w:t>k%02d</w:t></w:r></w:p></w:tc>", c);
    }

    sprintf(p, "</w:tr></w:tbl><w:p/></w:body></w:document>");
    d = docx_doc("word/document.xml", body, NULL);
    free(body);
    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_doc* back = NULL;
        buf_t b;

        if ((L = laid_out(d)) != NULL) {
            int32_t pg[3];
            pd_sp x[3], y[3];

            CHECK(text_drawn(d, L, "k31", &pg[0], &x[0], &y[0]) && text_drawn(d, L, "k32", &pg[1], &x[1], &y[1]) &&
                  text_drawn(d, L, "k39", &pg[2], &x[2], &y[2]));
            CHECK(x[1] > x[0] + PD_PT(10) && x[2] > x[1] + PD_PT(70) && y[2] == y[0]);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
        CHECK(d && find_para(d, "k39") != 0);
    }

    pd_doc_free(d);

    {
        /* HTML: a rowspan in column 70 of 80 still makes a merged cell below it */
        char* h = (char*)malloc(8192), *q;

        if (!h) {
            return;
        }

        q = h + sprintf(h, "<table><tr>");

        for (c = 0; c < 80; c++) {
            q += sprintf(q, c == 70 ? "<td rowspan=\"2\">m</td>" : "<td>a</td>");
        }

        q += sprintf(q, "</tr><tr>");

        for (c = 0; c < 79; c++) {
            q += sprintf(q, "<td>b</td>");
        }

        sprintf(q, "</tr></table>");
        d = NULL;
        CHECK(pd_doc_import(h, strlen(h), PD_CONV_HTML, &d) == PD_OK);
        free(h);

        if (d) {
            pd_block_id t = first_table(d);

            CHECK(cell_props_at(d, t, 1, 70).merge_up == 1 && cell_props_at(d, t, 1, 69).merge_up == 0 &&
                  has_text(d, t, 1, 71, "b"));
            pd_doc_free(d);
        }
    }
}

/* Rows that leave grid columns empty (gridBefore, gridAfter): a row's one cell after a skipped column sits under
   the second column, not the first, also after .docx and native round trips; HTML gets an empty cell for it */
static void test_grid_before(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/><w:gridCol w:w=\"3000\"/><w:gridCol w:w=\"3000\"/>"
                             "</w:tblGrid><w:tr><w:tc><w:p><w:r><w:t>AA</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>BB</w:t>"
                             "</w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>CC</w:t></w:r></w:p></w:tc></w:tr>"
                             "<w:tr><w:trPr><w:gridBefore w:val=\"1\"/><w:gridAfter w:val=\"1\"/><w:wBefore w:w=\"3000\" "
                             "w:type=\"dxa\"/></w:trPr><w:tc><w:p><w:r><w:t>GB</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
                             "<w:p/>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t pg[3];
            pd_sp x[3], y[3];

            CHECK(text_drawn(d, L, "AA", &pg[0], &x[0], &y[0]) && text_drawn(d, L, "BB", &pg[1], &x[1], &y[1]) &&
                  text_drawn(d, L, "GB", &pg[2], &x[2], &y[2]));
            CHECK(x[2] == x[1] && x[1] > x[0] + PD_PT(140) && y[2] > y[1]);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    if (d) {
        buf_t b;

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &b) == PD_OK);
        CHECK(b.p && strstr(b.p, "<tr><td colspan=\"1\" style=\"border:none\"></td><td>") != NULL);
        free(b.p);
    }

    pd_doc_free(d);
}

/* A row of exact height (trHeight hRule="exact") keeps it, and what its cell holds past it is cut off, the next
   row right below; also after .docx and native round trips */
static void test_exact_row(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/></w:tblGrid>"
                             "<w:tr><w:trPr><w:trHeight w:val=\"400\" w:hRule=\"exact\"/></w:trPr><w:tc>"
                             "<w:p><w:r><w:t>L1</w:t></w:r></w:p><w:p><w:r><w:t>L2</w:t></w:r></w:p>"
                             "<w:p><w:r><w:t>L3</w:t></w:r></w:p><w:p><w:r><w:t>L4</w:t></w:r></w:p></w:tc></w:tr>"
                             "<w:tr><w:tc><w:p><w:r><w:t>NEXT</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t pg[2];
            pd_sp x[2], y[2];

            CHECK(text_drawn(d, L, "L1", &pg[0], &x[0], &y[0]) && text_drawn(d, L, "NEXT", &pg[1], &x[1], &y[1]));
            CHECK(y[1] - y[0] > PD_PT(19) && y[1] - y[0] < PD_PT(21));    /* 20pt below: the row's height */
            CHECK(!text_drawn(d, L, "L2", &pg[0], &x[0], &y[0]) && !text_drawn(d, L, "L4", &pg[0], &x[0], &y[0]));
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    if (d) {
        buf_t b;

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &b) == PD_OK && b.p && strstr(b.p, "<tr style=\"height:20.00pt\">"));
        free(b.p);
    }

    pd_doc_free(d);
}

/* A table row near a page's foot: Word breaks it there (its first line on one page, its last on the next) unless
   it is cantSplit, when it goes whole to the next page; cantSplit kept through .docx and the native format */
static void test_cant_split(void) {
    int cant;

    for (cant = 0; cant < 2; cant++) {
        char* body = (char*)malloc(16384), *p;
        pd_doc* d;
        int i, pass;

        if (!body) {
            return;
        }

        p = body + sprintf(body, "<w:document xmlns:w=\"w\"><w:body>");

        for (i = 0; i < 40; i++) {
            p += sprintf(p, "<w:p><w:r><w:t>filler %d</w:t></w:r></w:p>", i);
        }

        p += sprintf(p, "<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/><w:gridCol w:w=\"3000\"/></w:tblGrid><w:tr>%s<w:tc>",
                     cant ? "<w:trPr><w:cantSplit/></w:trPr>" : "");

        for (i = 1; i <= 20; i++) {
            p += sprintf(p, "<w:p><w:r><w:t>R%d</w:t></w:r></w:p>", i);
        }

        sprintf(p, "</w:tc><w:tc><w:p><w:r><w:t>side</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/>"
                "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" "
                "w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>"
                "</w:body></w:document>");
        d = docx_doc("word/document.xml", body, NULL);
        free(body);
        CHECK(d != NULL);

        for (pass = 0; d && pass < 3; pass++) {
            pd_layout* L = laid_out(d);
            pd_doc* back = NULL;
            buf_t b;

            if (L) {
                int32_t pg[3];
                pd_sp x, y;

                CHECK(text_drawn(d, L, "R1", &pg[0], &x, &y) && text_drawn(d, L, "R20", &pg[1], &x, &y) &&
                      text_drawn(d, L, "filler 0", &pg[2], &x, &y));
                CHECK(cant ? pg[0] == 1 && pg[1] == 1 : pg[0] == 0 && pg[1] == 1);
                pd_layout_free(L);
            }

            if (pass == 2) {
                break;
            }

            memset(&b, 0, sizeof(b));
            CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
            CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
            free(b.p);
            pd_doc_free(d);
            d = back;
        }

        pd_doc_free(d);
    }
}

/* A cell's preferred width (tcW) sizes a column the table leaves automatic: 4000 twips (200pt) puts the next
   cell 200pt to the right, also after .docx and native round trips */
static void test_cell_width(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:tbl><w:tr><w:tc><w:tcPr><w:tcW w:w=\"4000\" w:type=\"dxa\"/></w:tcPr><w:p><w:r><w:t>WIDE"
                             "</w:t></w:r></w:p></w:tc><w:tc><w:tcPr><w:tcW w:w=\"0\" w:type=\"auto\"/></w:tcPr><w:p><w:r>"
                             "<w:t>NARROW</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t pg[2];
            pd_sp x[2], y[2];

            CHECK(text_drawn(d, L, "WIDE", &pg[0], &x[0], &y[0]) && text_drawn(d, L, "NARROW", &pg[1], &x[1], &y[1]));
            CHECK(x[1] - x[0] > PD_PT(199) && x[1] - x[0] < PD_PT(201));
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* a chart's items (pd_chart_items) for a plot area's XML, as a NUL-terminated string (malloc'ed) */
static char* chart_of(const char* plot) {
    static const uint32_t theme[12] = { 0xFF000000u, 0xFFFFFFFFu, 0xFF44546Au, 0xFFE7E6E6u, 0xFF4472C4u, 0xFFED7D31u,
                                        0xFFA5A5A5u, 0xFFFFC000u, 0xFF5B9BD5u, 0xFF70AD47u, 0xFF0563C1u, 0xFF954F72u
                                      };
    char xml[16384];
    pd_buf o;

    memset(&o, 0, sizeof(o));
    snprintf(xml, sizeof(xml), "<c:chartSpace xmlns:c=\"c\" xmlns:a=\"a\"><c:chart><c:autoTitleDeleted val=\"1\"/>"
             "<c:plotArea>%s</c:plotArea></c:chart></c:chartSpace>", plot);

    if (!pd_chart_items(xml, strlen(xml), PD_PT(400), PD_PT(300), theme, "Calibri", &o)) {
        pb_free(&o);
        return NULL;
    }

    pb_putc(&o, '\0');
    return o.p;
}

/* a series of three values (or, with x and sz, a bubble's x, y and size) */
static void chart_ser(char* out, size_t cap, int idx, const char* v, const char* extra) {
    double a, b, c;

    sscanf(v, "%lf %lf %lf", &a, &b, &c);
    snprintf(out, cap, "<c:ser><c:idx val=\"%d\"/><c:order val=\"%d\"/>%s<c:cat><c:strRef><c:strCache>"
             "<c:pt idx=\"0\"><c:v>A</c:v></c:pt><c:pt idx=\"1\"><c:v>B</c:v></c:pt><c:pt idx=\"2\"><c:v>C</c:v></c:pt>"
             "</c:strCache></c:strRef></c:cat><c:val><c:numRef><c:numCache><c:pt idx=\"0\"><c:v>%g</c:v></c:pt>"
             "<c:pt idx=\"1\"><c:v>%g</c:v></c:pt><c:pt idx=\"2\"><c:v>%g</c:v></c:pt></c:numCache></c:numRef></c:val>"
             "</c:ser>", idx, idx, extra, a, b, c);
}

/* charts drawn as what they are: a radar's rings and spokes, a stock chart's high-low lines and up and down bars,
   bubbles as big as their sizes, a second value axis with a scale of its own at the right, a linear trendline */
static void test_chart_kinds(void) {
    char s1[2048], s2[2048], s3[2048], s4[2048], plot[12000], *js;

    /* radar: three categories, two rings (steps of 5 to 10), three spokes, a closed line per series */
    chart_ser(s1, sizeof(s1), 0, "4 8 6", "");
    snprintf(plot, sizeof(plot), "<c:radarChart><c:radarStyle val=\"marker\"/>%s<c:axId val=\"1\"/><c:axId val=\"2\"/>"
             "</c:radarChart><c:catAx><c:axId val=\"1\"/></c:catAx><c:valAx><c:axId val=\"2\"/></c:valAx>", s1);
    js = chart_of(plot);
    CHECK(js && strstr(js, "\"label\":\"A\"") && strstr(js, "\"label\":\"C\""));
    CHECK(js && count_of(js, "\"closed\":1") >= 2 + 1);     /* rings, then the series' line */
    CHECK(js && count_of(js, "\"shape\":\"rect\"") == 0);   /* nothing drawn as a column */
    free(js);
    snprintf(plot, sizeof(plot), "<c:radarChart><c:radarStyle val=\"filled\"/>%s</c:radarChart><c:valAx/>", s1);
    js = chart_of(plot);
    CHECK(js && strstr(js, "\"closed\":1,\"fill\":4282675908") != NULL);  /* filled in the first accent */
    free(js);

    /* stock: open, high, low, close; up and down bars: two up (white), one down (black) */
    chart_ser(s1, sizeof(s1), 0, "10 20 30", "");
    chart_ser(s2, sizeof(s2), 1, "25 35 40", "");
    chart_ser(s3, sizeof(s3), 2, "5 15 20", "");
    chart_ser(s4, sizeof(s4), 3, "20 30 25", "");
    snprintf(plot, sizeof(plot), "<c:stockChart>%s%s%s%s<c:hiLowLines/><c:upDownBars><c:gapWidth val=\"150\"/>"
             "</c:upDownBars></c:stockChart><c:catAx/><c:valAx/>", s1, s2, s3, s4);
    js = chart_of(plot);
    CHECK(js && count_of(js, "\"shape\":\"rect\"") == 3);
    CHECK(js && count_of(js, "\"fill\":4294967295,\"line\":4278190080") == 2 &&
          count_of(js, "\"fill\":4278190080,\"line\":4278190080") == 1);
    CHECK(js && count_of(js, "\"closed\":0,\"fill\":0,\"line\":4278190080,\"lw\":49152") == 3);   /* high-low lines */
    free(js);

    /* bubbles: sizes 1 and 4: radii one and two to one */
    snprintf(plot, sizeof(plot), "<c:bubbleChart><c:ser><c:idx val=\"0\"/><c:xVal><c:numRef><c:numCache>"
             "<c:pt idx=\"0\"><c:v>1</c:v></c:pt><c:pt idx=\"1\"><c:v>3</c:v></c:pt></c:numCache></c:numRef></c:xVal>"
             "<c:yVal><c:numRef><c:numCache><c:pt idx=\"0\"><c:v>2</c:v></c:pt><c:pt idx=\"1\"><c:v>4</c:v></c:pt>"
             "</c:numCache></c:numRef></c:yVal><c:bubbleSize><c:numRef><c:numCache><c:pt idx=\"0\"><c:v>1</c:v></c:pt>"
             "<c:pt idx=\"1\"><c:v>4</c:v></c:pt></c:numCache></c:numRef></c:bubbleSize></c:ser></c:bubbleChart>"
             "<c:valAx/><c:valAx/>");
    js = chart_of(plot);
    CHECK(js != NULL);

    if (js) {   /* the circles: their widths from the first two points of each 32-point path */
        const char* p = js;
        double r[2] = { 0, 0 };
        int n = 0;

        while (n < 2 && (p = strstr(p, "{\"path\":[")) != NULL) {
            int x0, y0, k, cnt = 1;
            const char* q = p + 9;

            sscanf(q, "%d,%d", &x0, &y0);

            for (k = 0; q[k] && q[k] != ']'; k++) {
                cnt += q[k] == ',';
            }

            if (cnt == 64) {    /* 32 points */
                int x16, y16, i2;
                const char* z = q;

                for (i2 = 0; i2 < 32; i2++) {
                    z = strchr(z, ',') + 1;
                }

                sscanf(z, "%d,%d", &x16, &y16);
                r[n++] = (x0 - x16) / 2.0;
            }

            p += 9;
        }

        CHECK(n == 2 && r[0] > 0 && fabs(r[1] / r[0] - 2) < 0.05);
    }

    free(js);

    /* a second value axis: the line's values (hundreds) on a scale of their own at the right */
    chart_ser(s1, sizeof(s1), 0, "1 2 3", "");
    chart_ser(s2, sizeof(s2), 1, "100 300 500", "<c:trendline><c:trendlineType val=\"linear\"/></c:trendline>");
    snprintf(plot, sizeof(plot), "<c:barChart><c:barDir val=\"col\"/>%s<c:axId val=\"1\"/><c:axId val=\"2\"/>"
             "</c:barChart><c:lineChart>%s<c:axId val=\"3\"/><c:axId val=\"4\"/></c:lineChart><c:catAx><c:axId val=\"1\"/>"
             "</c:catAx><c:valAx><c:axId val=\"2\"/><c:axPos val=\"l\"/></c:valAx><c:catAx><c:axId val=\"3\"/>"
             "<c:delete val=\"1\"/></c:catAx><c:valAx><c:axId val=\"4\"/><c:axPos val=\"r\"/></c:valAx>", s1, s2);
    js = chart_of(plot);
    CHECK(js && strstr(js, "\"label\":\"500\"") != NULL && strstr(js, "\"label\":\"3\"") != NULL);

    if (js) {   /* the line reaches the top of the plot: its 500 is the scale's top, not off a 0..3 scale */
        const char* p = strstr(js, "\"label\":\"500\"");
        int lx = 0;

        CHECK(p && sscanf(strstr(p, "\"x\":") + 4, "%d", &lx) == 1 && lx > PD_PT(330));     /* at the right */
        CHECK(count_of(js, "\"lw\":147456") >= 1 && count_of(js, "\"lw\":98304") == 1);  /* the line, the trend */
    }

    free(js);
}

/* the first two glyphs drawn of the paragraph holding s, on page 0: 1 if there are two */
static int two_glyphs(const pd_doc* d, const pd_layout* L, const char* s, pd_draw* g) {
    pd_block_id p = find_para(d, s);
    int32_t k, n, got = 0;
    pd_draw* it = page_draws(L, 0, &n);

    for (k = 0; it && k < n && got < 2; k++) {
        if (it[k].kind == PD_DRAW_GLYPH && it[k].block == p) {
            g[got++] = it[k];
        }
    }

    free(it);
    return got == 2;
}

/* Text turned in a cell (w:textDirection): btLr runs up the cell, its glyphs turned a quarter left, tbRl runs
   down, turned a quarter right, each within its cell; also after .docx and native round trips */
static void test_cell_text_dir(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:tbl><w:tblGrid><w:gridCol w:w=\"1000\"/><w:gridCol w:w=\"1000\"/></w:tblGrid><w:tr>"
                             "<w:trPr><w:trHeight w:val=\"2000\" w:hRule=\"exact\"/></w:trPr>"
                             "<w:tc><w:tcPr><w:textDirection w:val=\"btLr\"/></w:tcPr><w:p><w:r><w:t>UPWARD</w:t></w:r>"
                             "</w:p></w:tc><w:tc><w:tcPr><w:textDirection w:val=\"tbRl\"/></w:tcPr><w:p><w:r><w:t>"
                             "DOWNWARD</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p><w:r><w:t>below</w:t></w:r></w:p>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            pd_draw u[2], w[2];
            int32_t pg;
            pd_sp bx = 0, by = 0;

            CHECK(two_glyphs(d, L, "UPWARD", u) && two_glyphs(d, L, "DOWNWARD", w) &&
                  text_drawn(d, L, "below", &pg, &bx, &by));
            CHECK(u[0].rotation == 16200000 && u[1].y < u[0].y - PD_PT(4) && u[1].x == u[0].x);
            CHECK(w[0].rotation == 5400000 && w[1].y > w[0].y + PD_PT(4) && w[1].x == w[0].x);
            CHECK(u[0].x > bx && u[0].x < bx + PD_PT(50) && w[0].x > bx + PD_PT(50) && w[0].x < bx + PD_PT(100));
            CHECK(u[0].y < by && w[0].y < by - PD_PT(20));      /* in the row, above the next paragraph */
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* Rules corner to corner across a cell (tl2br, tr2bl): a line from its top left to its bottom right in the first
   colour, one from top right to bottom left in the second, also after .docx and native round trips */
static void test_cell_diagonals(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:tbl><w:tblGrid><w:gridCol w:w=\"3000\"/></w:tblGrid><w:tr><w:tc><w:tcPr><w:tcBorders>"
                             "<w:tl2br w:val=\"single\" w:sz=\"8\" w:color=\"FF0000\"/>"
                             "<w:tr2bl w:val=\"single\" w:sz=\"8\" w:color=\"0000FF\"/></w:tcBorders></w:tcPr>"
                             "<w:p><w:r><w:t>X</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t n, k, down = 0, up = 0;
            pd_draw* it = page_draws(L, 0, &n);

            for (k = 0; it && k < n; k++) {
                const pd_sp* q = it[k].points;

                if (it[k].kind != PD_DRAW_PATH || it[k].npoints != 2 || it[k].line_width != PD_PT(1)) {
                    continue;
                }

                down += it[k].color == 0xFFFF0000u && q[0] < q[2] - PD_PT(140) && q[1] < q[3] - PD_PT(5);
                up += it[k].color == 0xFF0000FFu && q[0] > q[2] + PD_PT(140) && q[1] < q[3] - PD_PT(5);
            }

            CHECK(down == 1 && up == 1);
            free(it);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* Cells apart (tblCellSpacing): each cell its own box of rules, 5pt in from the table's frame and 10pt from the
   next cell, so a row shows six upright rules where shared edges would show three; also after .docx and native
   round trips, and HTML spaces its cells */
static void test_cell_spacing(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:tbl><w:tblPr><w:tblCellSpacing w:w=\"100\" w:type=\"dxa\"/><w:tblBorders>"
                             "<w:top w:val=\"single\" w:sz=\"4\"/><w:left w:val=\"single\" w:sz=\"4\"/>"
                             "<w:bottom w:val=\"single\" w:sz=\"4\"/><w:right w:val=\"single\" w:sz=\"4\"/>"
                             "<w:insideH w:val=\"single\" w:sz=\"4\"/><w:insideV w:val=\"single\" w:sz=\"4\"/>"
                             "</w:tblBorders></w:tblPr><w:tblGrid><w:gridCol w:w=\"3000\"/><w:gridCol w:w=\"3000\"/>"
                             "</w:tblGrid><w:tr><w:tc><w:p><w:r><w:t>A</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>B"
                             "</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t n, k, j, nx = 0;
            pd_sp xs[16];
            pd_draw* it = page_draws(L, 0, &n);

            for (k = 0; it && k < n; k++) {
                if (it[k].kind == PD_DRAW_RULE && it[k].h > PD_PT(5) && it[k].w < PD_PT(2)) {
                    for (j = 0; j < nx && xs[j] != it[k].x; j++) {
                    }

                    if (j == nx && nx < 16) {
                        xs[nx++] = it[k].x;
                    }
                }
            }

            CHECK(nx == 6);
            free(it);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    if (d) {
        buf_t b;

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &b) == PD_OK && b.p && strstr(b.p, "border-spacing:10.00pt"));
        free(b.p);
    }

    pd_doc_free(d);
}

/* A floating table (w:tblpPr) 350pt from the page's left and 100pt from its top: drawn there, with the text
   beside it on its left and none running under it; also after .docx and native round trips */
static void test_floating_table(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:tbl><w:tblPr><w:tblpPr w:leftFromText=\"180\" w:rightFromText=\"180\" "
                             "w:vertAnchor=\"page\" w:horzAnchor=\"page\" w:tblpX=\"7000\" w:tblpY=\"2000\"/>"
                             "<w:tblW w:w=\"2500\" w:type=\"dxa\"/></w:tblPr><w:tblGrid><w:gridCol w:w=\"2500\"/>"
                             "</w:tblGrid><w:tr><w:tc><w:p><w:r><w:t>FLOATING</w:t></w:r></w:p><w:p><w:r><w:t>two"
                             "</w:t></w:r></w:p><w:p><w:r><w:t>three</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
                             "<w:p><w:r><w:t>Body text beside the table and on below it, Body text beside the table and "
                             "on below it, Body text beside the table and on below it, Body text beside the table and on "
                             "below it, Body text beside the table and on below it, Body text beside the table and on "
                             "below it, Body text beside the table and on below it.</w:t></w:r></w:p>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t pg, n, k, beside = 0, under = 0;
            pd_sp x, y;
            pd_block_id body = find_para(d, "Body text");
            pd_draw* it = page_draws(L, 0, &n);

            CHECK(text_drawn(d, L, "FLOATING", &pg, &x, &y) && pg == 0);
            CHECK(x > PD_PT(350) && x < PD_PT(360) && y > PD_PT(100) && y < PD_PT(120));

            for (k = 0; it && k < n; k++) {
                if (it[k].kind == PD_DRAW_GLYPH && it[k].block == body && it[k].y > PD_PT(102) &&
                        it[k].y < PD_PT(140)) {
                    beside += it[k].x < PD_PT(330);
                    under += it[k].x >= PD_PT(330);
                }
            }

            CHECK(beside > 10 && under == 0);
            free(it);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);

        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* Page features of a section: the pages' colour under everything, a page border 24pt in from the edge, a line
   between two columns, and a section that starts on an odd page getting a blank page before it; also after
   .docx and native round trips */
static void test_page_features(void) {
    pd_doc* d = docx_doc("word/document.xml",
                         "<w:document xmlns:w=\"w\"><w:background w:color=\"FFFFCC\"/><w:body>"
                         "<w:p><w:r><w:t>FIRST one two three four five six seven eight nine ten eleven twelve thirteen "
                         "fourteen fifteen sixteen seventeen eighteen nineteen twenty one two three four five six seven "
                         "eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen "
                         "twenty</w:t></w:r></w:p>"
                         "<w:p><w:pPr><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" "
                         "w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" "
                         "w:gutter=\"0\"/><w:pgBorders w:offsetFrom=\"page\"><w:top w:val=\"single\" w:sz=\"8\" "
                         "w:space=\"24\" w:color=\"FF0000\"/></w:pgBorders><w:cols w:num=\"2\" w:space=\"720\" "
                         "w:sep=\"1\"/></w:sectPr></w:pPr></w:p>"
                         "<w:p><w:r><w:t>SECOND</w:t></w:r></w:p>"
                         "<w:sectPr><w:type w:val=\"oddPage\"/><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar "
                         "w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" "
                         "w:footer=\"720\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>", NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t n, k, pg, border = 0, sep = 0;
            pd_sp x, y;
            pd_draw* it = page_draws(L, 0, &n);

            CHECK(it && n > 0 && it[0].kind == PD_DRAW_RULE && it[0].color == 0xFFFFFFCCu && it[0].x == 0 &&
                  it[0].w == PD_PT(612) && it[0].h == PD_PT(792));

            for (k = 0; it && k < n; k++) {
                border += it[k].kind == PD_DRAW_RULE && it[k].color == 0xFFFF0000u && it[k].y == PD_PT(24) &&
                          it[k].h == PD_PT(1) && it[k].w > PD_PT(500);
                sep += it[k].kind == PD_DRAW_RULE && it[k].color == 0xFF000000u && it[k].w == PD_PT(0.5) &&
                       it[k].h > PD_PT(5) && it[k].x > PD_PT(300) && it[k].x < PD_PT(312);
            }

            CHECK(border == 1 && sep == 1);
            free(it);
            CHECK(pd_layout_page_count(L) == 3 && text_drawn(d, L, "SECOND", &pg, &x, &y) && pg == 2);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* Paragraph borders, each edge its own line: a double blue top (two strokes 1.5pt apart), dashed red left
   (dashes, longer than wide), a thick-thin green bottom (a 3pt and a thinner stroke), a dotted right (square
   dots); two paragraphs sharing a box with a rule between them; a bar left of a paragraph. Also after .docx and
   native round trips */
static void test_para_border_lines(void) {
    pd_doc* d = docx_doc("word/document.xml", WDOC(
                             "<w:p><w:pPr><w:pBdr><w:top w:val=\"double\" w:sz=\"6\" w:space=\"1\" w:color=\"0000FF\"/>"
                             "<w:left w:val=\"dashed\" w:sz=\"6\" w:space=\"4\" w:color=\"FF0000\"/>"
                             "<w:bottom w:val=\"thickThinSmallGap\" w:sz=\"24\" w:space=\"1\" w:color=\"00FF00\"/>"
                             "<w:right w:val=\"dotted\" w:sz=\"6\" w:space=\"4\" w:color=\"112233\"/></w:pBdr></w:pPr>"
                             "<w:r><w:t>BOXED</w:t></w:r></w:p><w:p/>"
                             "<w:p><w:pPr><w:pBdr><w:top w:val=\"single\" w:sz=\"8\" w:color=\"445566\"/><w:between "
                             "w:val=\"single\" w:sz=\"8\" w:color=\"778899\"/></w:pBdr></w:pPr><w:r><w:t>PAIR1</w:t></w:r>"
                             "</w:p><w:p><w:pPr><w:pBdr><w:top w:val=\"single\" w:sz=\"8\" w:color=\"445566\"/><w:between "
                             "w:val=\"single\" w:sz=\"8\" w:color=\"778899\"/></w:pBdr></w:pPr><w:r><w:t>PAIR2</w:t></w:r>"
                             "</w:p><w:p/><w:p><w:pPr><w:ind w:left=\"720\"/><w:pBdr><w:bar w:val=\"single\" w:sz=\"16\" "
                             "w:space=\"6\" w:color=\"AA00AA\"/></w:pBdr></w:pPr><w:r><w:t>BARRED</w:t></w:r></w:p>"), NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t n, k, blue = 0, red = 0, green3 = 0, greenthin = 0, dots = 0, between = 0, tops = 0, bar = 0;
            pd_sp by[2] = { 0, 0 };
            pd_draw* it = page_draws(L, 0, &n);

            for (k = 0; it && k < n; k++) {
                const pd_draw* a = &it[k];

                if (a->kind != PD_DRAW_RULE) {
                    continue;
                }

                if (a->color == 0xFF0000FFu && a->h == PD_PT(0.75) && blue < 2) {
                    by[blue++] = a->y;
                }

                red += a->color == 0xFFFF0000u && a->w == PD_PT(0.75) && a->h == PD_PT(2.25);
                green3 += a->color == 0xFF00FF00u && a->h == PD_PT(3);
                greenthin += a->color == 0xFF00FF00u && a->h == PD_PT(0.75);
                dots += a->color == 0xFF112233u && a->w == PD_PT(0.75) && a->h == PD_PT(0.75);
                tops += a->color == 0xFF445566u;
                between += a->color == 0xFF778899u;
                bar += a->color == 0xFFAA00AAu && a->w == PD_PT(2) && a->x < PD_PT(108 - 6) &&
                       a->x > PD_PT(108 - 10);
            }

            CHECK(blue == 2 && by[1] - by[0] == PD_PT(1.5));
            CHECK(red >= 3 && green3 == 1 && greenthin == 1 && dots > 3);
            CHECK(tops == 1 && between == 1 && bar == 1);
            free(it);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

#define TIGHT_PIC(id, x, wrap) \
    "<w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"114300\" distR=\"114300\">" \
    "<wp:positionH relativeFrom=\"column\"><wp:posOffset>" x "</wp:posOffset></wp:positionH>" \
    "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>" \
    "<wp:extent cx=\"1828800\" cy=\"1828800\"/>" wrap "<wp:docPr id=\"" id "\" name=\"p\"/>" \
    "<a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill></pic:pic>" \
    "</a:graphicData></a:graphic></wp:anchor></w:drawing></w:r>"

/* the leftmost glyph of the paragraph holding s on each of its lines on page 0, top down: up to cap of them */
static int para_line_starts(const pd_doc* d, const pd_layout* L, const char* s, pd_sp* xs, pd_sp* ys, int cap) {
    pd_block_id p = find_para(d, s);
    int32_t k, n, j, got = 0;
    pd_draw* it = page_draws(L, 0, &n);

    for (k = 0; it && k < n; k++) {
        if (it[k].kind != PD_DRAW_GLYPH || it[k].block != p) {
            continue;
        }

        for (j = 0; j < got && ys[j] != it[k].y; j++) {
        }

        if (j == got && got < cap) {
            xs[got] = it[k].x;
            ys[got++] = it[k].y;
        } else if (j < got && it[k].x < xs[j]) {
            xs[j] = it[k].x;
        }
    }

    free(it);
    return got;
}

/* Text round a float's outline (wrapTight with a wrapPolygon): beside a picture whose outline is a triangle
   narrow at its top and full width at its bottom, the lines start near the column's edge at the top and past
   most of the picture lower down. A float with wrapText "largest" a little left of the middle has the text on its
   left, the wider side. Ten pictures anchored in one paragraph are ten floats. Also after .docx and native round trips */
static void test_wrap_outline(void) {
    char body[16384], *p;
    pd_doc* d;
    int pass, i;

    p = body + sprintf(body, "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\">"
                       "<w:body><w:p>%s<w:r><w:t>OUTLINED", TIGHT_PIC("1", "0", "<wp:wrapTight wrapText=\"right\">"
                               "<wp:wrapPolygon edited=\"1\"><wp:start x=\"0\" y=\"0\"/><wp:lineTo x=\"0\" y=\"21600\"/>"
                               "<wp:lineTo x=\"21600\" y=\"21600\"/><wp:lineTo x=\"0\" y=\"0\"/></wp:wrapPolygon>"
                               "</wp:wrapTight>"));

    for (i = 0; i < 80; i++) {
        p += sprintf(p, " words to wrap round it");
    }

    p += sprintf(p, "</w:t></w:r></w:p><w:p>%s<w:r><w:t>LARGEST beside</w:t></w:r></w:p><w:p>",
                 TIGHT_PIC("2", "2103120", "<wp:wrapSquare wrapText=\"largest\"/>"));

    for (i = 0; i < 10; i++) {
        p += sprintf(p, "%s", TIGHT_PIC("3", "0", "<wp:wrapTopAndBottom/>"));
    }

    sprintf(p, "<w:r><w:t>TEN</w:t></w:r></w:p><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar "
            "w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" "
            "w:gutter=\"0\"/></w:sectPr></w:body></w:document>");
    d = docx_doc("word/_rels/document.xml.rels",
                 "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
                 "</Relationships>", "word/media/image1.png", "tests/data/rgba.png", "word/document.xml", body, NULL);
    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        pd_block_id sec = pd_doc_child(d, pd_doc_root(d), 0);
        pd_block_info si, bi;
        int32_t k, nfl = 0;
        buf_t b;

        pd_doc_block_info(d, sec, &si);

        for (k = 0; k < si.child_count; k++) {
            nfl += pd_doc_block_info(d, pd_doc_child(d, sec, k), &bi) == PD_OK && bi.kind == PD_BLOCK_FLOAT;
        }

        CHECK(nfl == 12);

        if (L) {
            pd_sp xs[64], ys[64], top = 0, low = 0, lx, ly;
            int32_t n = para_line_starts(d, L, "OUTLINED", xs, ys, 64), pg;

            for (k = 0; k < n; k++) {
                if (ys[k] < PD_PT(72 + 20)) {
                    top = xs[k];
                } else if (ys[k] > PD_PT(72 + 100) && ys[k] < PD_PT(72 + 125)) {
                    low = xs[k];
                }
            }

            CHECK(n > 10 && top > PD_PT(72) && top < PD_PT(72 + 40) && low > PD_PT(72 + 95));
            CHECK(text_drawn(d, L, "LARGEST", &pg, &lx, &ly) && lx < PD_PT(80));     /* left of it: more room */
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* a row of exact height cuts its text off at its edge, as Word does: a line that mostly fits is still there (a
   form's 12pt rows of 10pt text), and a cell merged down over exact rows has all of them to fill */
static void exact_rows_case(int in_box) {
    char out[4096], doc[8192];
    const char* tbl = "<w:tbl><w:tblPr><w:tblW w:w=\"5000\" w:type=\"dxa\"/>"
                      "</w:tblPr><w:tblGrid><w:gridCol w:w=\"2500\"/><w:gridCol w:w=\"2500\"/></w:tblGrid>"
                      "<w:tr><w:trPr><w:trHeight w:hRule=\"exact\" w:val=\"240\"/></w:trPr>"
                      "<w:tc><w:p><w:r><w:rPr><w:sz w:val=\"22\"/></w:rPr><w:t>Source of Support</w:t></w:r></w:p></w:tc>"
                      "<w:tc><w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr><w:p><w:r><w:rPr><w:sz w:val=\"20\"/>"
                      "</w:rPr><w:t>Upper</w:t></w:r></w:p><w:p><w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr>"
                      "<w:t>Lower</w:t></w:r></w:p></w:tc></w:tr>"
                      "<w:tr><w:trPr><w:trHeight w:hRule=\"exact\" w:val=\"240\"/></w:trPr><w:tc><w:p/></w:tc>"
                      "<w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc></w:tr>"
                      "<w:tr><w:trPr><w:trHeight w:hRule=\"exact\" w:val=\"40\"/></w:trPr><w:tc><w:p><w:r>"
                      "<w:t>Hidden</w:t></w:r></w:p></w:tc><w:tc><w:p/></w:tc></w:tr>"
                      "</w:tbl>";
    pd_doc* d;

    if (in_box) {   /* in a floating text box: the table laid out in the float */
        snprintf(doc, sizeof(doc), "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wps=\"wps\"><w:body>"
                 "<w:p><w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\" simplePos=\"0\" "
                 "relativeHeight=\"1\" behindDoc=\"0\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\">"
                 "<wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\"><wp:posOffset>0</wp:posOffset>"
                 "</wp:positionH><wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>"
                 "<wp:extent cx=\"3810000\" cy=\"1270000\"/><wp:wrapTopAndBottom/><wp:docPr id=\"1\" name=\"T\"/>"
                 "<a:graphic><a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\">"
                 "<wps:wsp><wps:cNvSpPr txBox=\"1\"/><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"3810000\" "
                 "cy=\"1270000\"/></a:xfrm><a:prstGeom prst=\"rect\"/></wps:spPr><wps:txbx><w:txbxContent>%s<w:p/>"
                 "</w:txbxContent></wps:txbx><wps:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\"/></wps:wsp>"
                 "</a:graphicData></a:graphic></wp:anchor></w:drawing></w:r></w:p></w:body></w:document>", tbl);
    } else {
        snprintf(doc, sizeof(doc), "<w:document xmlns:w=\"w\"><w:body>%s<w:p/></w:body></w:document>", tbl);
    }

    d = docx_doc("word/document.xml", doc, NULL);

    CHECK(d != NULL);

    if (d && have_layout_font()) {
        CHECK(layout_text(d, -1, out, sizeof(out)) >= 1);
        CHECK(strstr(out, "SourceofSupport") != NULL);      /* an 11pt line in a 12pt row (no glyph for a space) */
        CHECK(strstr(out, "Upper") && strstr(out, "Lower"));    /* two lines in a cell over two 12pt rows */
        CHECK(strstr(out, "Hidden") == NULL);                /* a 2pt row shows none of a 12pt line */
    }

    pd_doc_free(d);
}

static void test_docx_exact_rows_keep_text(void) {
    exact_rows_case(0);
    exact_rows_case(1);
}

/* Text on both sides of a float in the middle of the column (wrapSquare bothSides): beside it each row of the
   paragraph is a left part and a right part on one baseline, none of it under the picture, read left part then
   right part; a click on the right part finds text after the left part's, and its caret is drawn there. Also
   after .docx and native round trips */
static void test_wrap_both_sides(void) {
    char body[16384], *p;
    pd_doc* d;
    int pass, i;

    p = body + sprintf(body, "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\">"
                       "<w:body><w:p>"
                       "<w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"114300\" distR=\"114300\">"
                       "<wp:positionH relativeFrom=\"column\"><wp:posOffset>2286000</wp:posOffset></wp:positionH>"
                       "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>"
                       "<wp:extent cx=\"914400\" cy=\"914400\"/><wp:wrapSquare wrapText=\"bothSides\"/>"
                       "<wp:docPr id=\"1\" name=\"p\"/><a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip "
                       "r:embed=\"rId5\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:anchor>"
                       "</w:drawing></w:r><w:r><w:t>BOTH");

    for (i = 0; i < 60; i++) {
        p += sprintf(p, " words on either side");
    }

    sprintf(p, "</w:t></w:r></w:p><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" "
            "w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
            "</w:sectPr></w:body></w:document>");
    d = docx_doc("word/_rels/document.xml.rels",
                 "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
                 "</Relationships>", "word/media/image1.png", "tests/data/rgba.png", "word/document.xml", body, NULL);
    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            pd_block_id para = find_para(d, "BOTH");
            int32_t n, k, left = 0, right = 0, under = 0, pg;
            pd_sp ly = -1, ry = -1, lo = PD_PT(72 + 180), hi = PD_PT(72 + 252), cx, cb, ca, cd;
            pd_draw* it = page_draws(L, 0, &n);
            pd_pos at, at2;

            for (k = 0; it && k < n; k++) {
                if (it[k].kind != PD_DRAW_GLYPH || it[k].block != para || it[k].y > PD_PT(72 + 70)) {
                    continue;
                }

                left += it[k].x + it[k].w <= lo;
                right += it[k].x >= hi;
                under += it[k].x + it[k].w > lo && it[k].x < hi;
                ly = ly < 0 && it[k].x < lo ? it[k].y : ly;
                ry = ry < 0 && it[k].x >= hi ? it[k].y : ry;
            }

            free(it);
            CHECK(left > 20 && right > 20 && under == 0 && ly == ry && ly > 0);
            CHECK(pd_layout_hit_test(L, 0, PD_PT(80), ly, &at) == PD_OK && at.block == para);
            CHECK(pd_layout_hit_test(L, 0, PD_PT(72 + 300), ly, &at2) == PD_OK && at2.block == para &&
                  at2.offset > at.offset + 10);
            CHECK(pd_layout_caret(L, at2, &pg, &cx, &cb, &ca, &cd) == PD_OK && pg == 0 && cb == ly && cx > hi);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* A drawing over the text (wrapNone), placed 15pt below the paragraph it is anchored in: when that paragraph's
   first line goes over to the next page, the drawing goes with it rather than staying at the foot of the page
   before (where it would run off it). Laid out with as many lines before it as fill the first page to the line */
static void test_anchor_follows_paragraph(void) {
    int n, found = 0;

    for (n = 40; n < 70 && !found; n++) {
        char* body = (char*)malloc(32768), *p;
        pd_doc* d;
        pd_layout* L;
        int i;

        if (!body) {
            return;
        }

        p = body + sprintf(body, "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" "
                           "xmlns:r=\"r\"><w:body>");

        for (i = 0; i < n; i++) {
            p += sprintf(p, "<w:p><w:r><w:t>filler %d</w:t></w:r></w:p>", i);
        }

        sprintf(p, "<w:p><w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\" behindDoc=\"0\">"
                "<wp:positionH relativeFrom=\"column\"><wp:posOffset>0</wp:posOffset></wp:positionH>"
                "<wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>190500</wp:posOffset></wp:positionV>"
                "<wp:extent cx=\"2540000\" cy=\"2540000\"/><wp:wrapNone/><wp:docPr id=\"1\" name=\"p\"/><a:graphic>"
                "<a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=\"rId5\"/></pic:blipFill></pic:pic>"
                "</a:graphicData></a:graphic></wp:anchor></w:drawing></w:r><w:r><w:t>ANCHOR</w:t></w:r></w:p>"
                "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" "
                "w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>"
                "</w:body></w:document>");
        d = docx_doc("word/_rels/document.xml.rels",
                     "<Relationships xmlns=\"r\"><Relationship Id=\"rId5\" Type=\"t/image\" Target=\"media/image1.png\"/>"
                     "</Relationships>", "word/media/image1.png", "tests/data/rgba.png", "word/document.xml", body, NULL);
        free(body);

        if (d && (L = laid_out(d)) != NULL) {
            int32_t pa, pf, pg, k, m, img = -1;
            pd_sp x, y;
            char last[32];

            snprintf(last, sizeof(last), "filler %d", n - 1);

            if (text_drawn(d, L, "ANCHOR", &pa, &x, &y) && text_drawn(d, L, last, &pf, &x, &y) && pa == 1 && pf == 0) {
                found = 1;

                for (pg = 0; pg < pd_layout_page_count(L); pg++) {
                    pd_draw* it = page_draws(L, pg, &m);

                    for (k = 0; it && k < m; k++) {
                        img = it[k].kind == PD_DRAW_IMAGE ? pg : img;
                    }

                    free(it);
                }

                CHECK(img == pa);
            }

            pd_layout_free(L);
        }

        pd_doc_free(d);
    }

    CHECK(found || !have_layout_font());
}

/* Columns of their own widths (w:cols equalWidth="0"): a paragraph running from a 100pt column into a 332pt one
   is broken at each column's width, its lines in the first no wider than it, those in the second starting where
   it does and reaching across it; also after .docx and native round trips */
static void test_unequal_columns(void) {
    char* body = (char*)malloc(32768), *p;
    pd_doc* d;
    int pass, i;

    if (!body) {
        return;
    }

    p = body + sprintf(body, "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>COLUMNS");

    for (i = 0; i < 260; i++) {
        p += sprintf(p, " some words");
    }

    sprintf(p, "</w:t></w:r></w:p><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" "
            "w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>"
            "<w:cols w:num=\"2\" w:space=\"720\" w:equalWidth=\"0\"><w:col w:w=\"2000\" w:space=\"720\"/>"
            "<w:col w:w=\"6640\"/></w:cols></w:sectPr></w:body></w:document>");
    d = docx_doc("word/document.xml", body, NULL);
    free(body);
    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t n, k, in0 = 0, in1 = 0;
            pd_sp right0 = 0, left1 = PD_PT(1000), right1 = 0;
            pd_block_id para = find_para(d, "COLUMNS");
            pd_draw* it = page_draws(L, 0, &n);

            for (k = 0; it && k < n; k++) {
                if (it[k].kind != PD_DRAW_GLYPH || it[k].block != para) {
                    continue;
                }

                if (it[k].x < PD_PT(72 + 118)) {
                    in0++;
                    right0 = it[k].x + it[k].w > right0 ? it[k].x + it[k].w : right0;
                } else {
                    in1++;
                    left1 = it[k].x < left1 ? it[k].x : left1;
                    right1 = it[k].x + it[k].w > right1 ? it[k].x + it[k].w : right1;
                }
            }

            free(it);
            CHECK(in0 > 100 && in1 > 100 && right0 <= PD_PT(72 + 100) + PD_PT(0.5));
            CHECK(left1 >= PD_PT(72 + 136) - PD_PT(0.5) && left1 < PD_PT(72 + 140) && right1 > PD_PT(72 + 136 + 300));
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* A section that starts in the next column (w:type nextColumn): its text at the top of the second column of the
   page the section before it ended on, not on a new page; also after .docx and native round trips */
static void test_next_column_section(void) {
    pd_doc* d = docx_doc("word/document.xml",
                         "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>ALPHA</w:t></w:r></w:p>"
                         "<w:p><w:pPr><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" "
                         "w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" w:footer=\"720\" "
                         "w:gutter=\"0\"/><w:cols w:num=\"2\" w:space=\"720\"/></w:sectPr></w:pPr></w:p>"
                         "<w:p><w:r><w:t>BETA</w:t></w:r></w:p><w:sectPr><w:type w:val=\"nextColumn\"/><w:pgSz "
                         "w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
                         "w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/><w:cols w:num=\"2\" "
                         "w:space=\"720\"/></w:sectPr></w:body></w:document>", NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            int32_t pa, pb;
            pd_sp xa, ya, xb, yb;

            CHECK(text_drawn(d, L, "ALPHA", &pa, &xa, &ya) && text_drawn(d, L, "BETA", &pb, &xb, &yb));
            CHECK(pa == 0 && pb == 0 && pd_layout_page_count(L) == 1 && yb == ya && xb == PD_PT(72 + 252));
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* Paragraph borders (each edge its own) and the pages' border and colour in HTML (CSS borders on the paragraph,
   the body's for the page) and RTF (\brdr... with their style, width, colour and space; \pgbrdr...; a background
   shape) */
static void test_border_export(void) {
    pd_doc* d = docx_doc("word/document.xml",
                         "<w:document xmlns:w=\"w\"><w:background w:color=\"FFFFCC\"/><w:body><w:p><w:pPr><w:pBdr>"
                         "<w:top w:val=\"double\" w:sz=\"6\" w:space=\"1\" w:color=\"0000FF\"/>"
                         "<w:left w:val=\"dashed\" w:sz=\"6\" w:space=\"4\" w:color=\"FF0000\"/></w:pBdr></w:pPr>"
                         "<w:r><w:t>BORDERED</w:t></w:r></w:p><w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
                         "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" "
                         "w:footer=\"720\" w:gutter=\"0\"/><w:pgBorders w:offsetFrom=\"page\"><w:top w:val=\"single\" "
                         "w:sz=\"8\" w:space=\"24\" w:color=\"00FF00\"/></w:pgBorders></w:sectPr></w:body></w:document>",
                         NULL);
    buf_t b;

    CHECK(d != NULL);

    if (!d) {
        return;
    }

    memset(&b, 0, sizeof(b));
    CHECK(pd_doc_export(d, PD_CONV_HTML, to_buf, &b) == PD_OK && b.p);
    CHECK(b.p && strstr(b.p, "border-top:2.25pt double #0000FF;padding-top:1.00pt;") &&
          strstr(b.p, "border-left:0.75pt dashed #FF0000;padding-left:4.00pt;"));
    CHECK(b.p && strstr(b.p, "<body style=\"border-top:1.00pt solid #00FF00;padding-top:24.00pt;"
                        "background-color:#FFFFCC;\">"));
    free(b.p);
    memset(&b, 0, sizeof(b));
    CHECK(pd_doc_export(d, PD_CONV_RTF, to_buf, &b) == PD_OK && b.p);
    CHECK(b.p && strstr(b.p, "\\brdrt\\brdrdb\\brdrw15\\brdrcf") && strstr(b.p, "\\brdrl\\brdrdash\\brdrw15\\brdrcf"));
    CHECK(b.p && strstr(b.p, "\\pgbrdropt32\\pgbrdrt\\brdrs\\brdrw20\\brdrcf") && strstr(b.p, "\\brsp480"));
    CHECK(b.p && strstr(b.p, "\\viewbksp1{\\*\\background{\\shp{\\*\\shpinst{\\sp{\\sn fillColor}{\\sv 13434879}}}}}"));
    free(b.p);
    pd_doc_free(d);
}

/* A section of vertical text (w:textDirection tbRl): its lines run down the page, the first at the right (near
   the right margin), the next to its left; Latin letters turned a quarter right, East Asian characters
   upright; also after .docx and native round trips */
static void test_vertical_section(void) {
    char body[8192], *p;
    pd_doc* d;
    int pass, i;

    p = body + sprintf(body, "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>Vertical");

    for (i = 0; i < 40; i++) {
        p += sprintf(p, " words");
    }

    sprintf(p, "</w:t></w:r></w:p><w:p><w:r><w:t>\xE4\xB8\xAD\xE6\x96\x87</w:t></w:r></w:p><w:sectPr>"
            "<w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
            "w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/><w:textDirection w:val=\"tbRl\"/>"
            "</w:sectPr></w:body></w:document>");
    d = docx_doc("word/document.xml", body, NULL);
    CHECK(d != NULL);

    for (pass = 0; d && pass < 3; pass++) {
        pd_layout* L = laid_out(d);
        pd_doc* back = NULL;
        buf_t b;

        if (L) {
            pd_draw v[2], c[2];
            int32_t n, k;
            pd_sp maxx = 0, minx = PD_PT(1000);
            pd_block_id para = find_para(d, "Vertical");
            pd_draw* it = page_draws(L, 0, &n);

            for (k = 0; it && k < n; k++) {
                if (it[k].kind == PD_DRAW_GLYPH && it[k].block == para) {
                    maxx = it[k].x > maxx ? it[k].x : maxx;
                    minx = it[k].x < minx ? it[k].x : minx;
                }
            }

            free(it);
            CHECK(two_glyphs(d, L, "Vertical", v) && v[0].rotation == 5400000 && v[1].x == v[0].x &&
                  v[1].y > v[0].y && v[0].y > PD_PT(72) - 1 && v[0].y < PD_PT(80));
            CHECK(maxx > PD_PT(612 - 72 - 20) && maxx <= PD_PT(612 - 72) && minx < maxx - PD_PT(10));  /* lines leftward */
            CHECK(two_glyphs(d, L, "\xE4\xB8\xAD", c) && c[0].rotation == 0 && c[1].rotation == 0 &&
                  c[1].y > c[0].y && c[0].x < v[0].x);
            pd_layout_free(L);
        }

        if (pass == 2) {
            break;
        }

        memset(&b, 0, sizeof(b));
        CHECK(pd_doc_export(d, pass ? PD_CONV_JDATA : PD_CONV_DOCX, to_buf, &b) == PD_OK);
        CHECK(pd_doc_import(b.p, b.n, pass ? PD_CONV_JDATA : PD_CONV_DOCX, &back) == PD_OK);
        free(b.p);
        pd_doc_free(d);
        d = back;
    }

    pd_doc_free(d);
}

/* WordArt along a path: in a square box, "AMZ" arched up goes over the top (A at the left, M at the top middle, Z
   at the right, its glyphs turned with the arch), arched down under the bottom (M at the bottom middle) */
static void test_wordart_on_path(void) {
    static const char* box = "<wps:wsp><wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"2540000\"/>"
                             "</a:xfrm><a:prstGeom prst=\"rect\"/><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr><wps:txbx>"
                             "<w:txbxContent><w:p><w:r><w:rPr><w:sz w:val=\"40\"/></w:rPr><w:t>AMZ</w:t></w:r></w:p>"
                             "</w:txbxContent></wps:txbx><wps:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" wrap=\"none\">"
                             "<a:prstTxWarp prst=\"%s\"><a:avLst/></a:prstTxWarp></wps:bodyPr></wps:wsp>";
    static const char* kinds[2] = { "textArchUp", "textArchDown" };
    int pass;

    for (pass = 0; pass < 2; pass++) {
        char shapes[2048];
        pd_doc* d = NULL;
        pd_layout* L = NULL;
        int32_t n = 0, k;
        pd_draw* it;
        double cx[3] = { 0, 0, 0 }, cy[3] = { 0, 0, 0 };
        int got[3] = { 0, 0, 0 };

        snprintf(shapes, sizeof(shapes), box, kinds[pass]);
        it = group_items(shapes, &d, &L, &n);

        if (!it) {
            CHECK(0);
            pd_layout_free(L);
            pd_doc_free(d);
            continue;
        }

        for (k = 0; k < n; k++) {   /* the glyphs' paths' middles, and the group's extent from them all */
            const char* q = it[k].kind == PD_DRAW_PATH ? strchr("AMZ", (int)it[k].text) : NULL;

            if (q && it[k].text) {
                int i = (int)(q - "AMZ");

                cx[i] = it[k].x + it[k].w / 2.0;
                cy[i] = it[k].y + it[k].h / 2.0;
                got[i] = 1;
            }
        }

        CHECK(got[0] && got[1] && got[2]);

        if (got[0] && got[1] && got[2]) {
            CHECK(cx[0] < cx[1] && cx[1] < cx[2]);                     /* read left to right */
            CHECK(fabs(cy[0] - cy[2]) < PD_PT(20));                   /* the ends level, at the sides */
            /* the middle over (under) the ends by about half the ellipse's height: a fifth of their span at least */
            CHECK(pass == 0 ? cy[0] - cy[1] > 0.2 * (cx[2] - cx[0]) : cy[1] - cy[0] > 0.2 * (cx[2] - cx[0]));
        }

        free(it);
        pd_layout_free(L);
        pd_doc_free(d);
    }
}

/* the y on page 0 of the first glyph showing code point c (-1: none) */
static pd_sp glyph_y_of(pd_layout* L, uint32_t c) {
    int32_t n = 0, k;
    pd_draw* it;
    pd_sp y = -1;

    if (pd_layout_page_items(L, 0, NULL, 0, &n) != PD_OK || !(it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw)))) {
        return -1;
    }

    pd_layout_page_items(L, 0, it, n, &n);

    for (k = 0; k < n && y < 0; k++) {
        if (it[k].kind == PD_DRAW_GLYPH && it[k].text == c) {
            y = it[k].y;
        }
    }

    free(it);
    return y;
}

/* Linked text boxes (wps:linkedTxbx): the first box 40 pt high holds eight one-word paragraphs, Alpha to Hotel, that
   do not fit; what does not goes on in the second box lower down the page. Kept so through a .docx */
static void test_docx_linked_textboxes(void) {
    static const char* anchor = "<w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\" simplePos=\"0\" "
                                "relativeHeight=\"1\" behindDoc=\"0\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\">"
                                "<wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"page\"><wp:posOffset>914400"
                                "</wp:posOffset></wp:positionH><wp:positionV relativeFrom=\"page\"><wp:posOffset>%d"
                                "</wp:posOffset></wp:positionV><wp:extent cx=\"2540000\" cy=\"%d\"/><wp:wrapNone/>"
                                "<wp:docPr id=\"%d\" name=\"Text Box %d\"/><a:graphic><a:graphicData uri=\"http://schemas."
                                "microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp><wps:cNvSpPr txBox=\"1\"/>"
                                "<wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"2540000\" cy=\"%d\"/></a:xfrm>"
                                "<a:prstGeom prst=\"rect\"/></wps:spPr>%s<wps:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" "
                                "bIns=\"0\"><a:noAutofit/></wps:bodyPr></wps:wsp></a:graphicData></a:graphic></wp:anchor>"
                                "</w:drawing></w:r>";
    static const char* words[8] = { "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel" };
    char paras[2048], first[4096], second[2048], doc[16384];
    size_t pl = 0;
    int k, pass;
    pd_doc* d;

    for (k = 0; k < 8; k++) {
        pl += (size_t)snprintf(paras + pl, sizeof(paras) - pl, "<w:p><w:pPr><w:spacing w:before=\"0\" w:after=\"0\"/></w:pPr>"
                               "<w:r><w:rPr><w:sz w:val=\"20\"/></w:rPr><w:t>%s</w:t></w:r></w:p>", words[k]);
    }

    snprintf(first, sizeof(first), "<wps:txbx id=\"1\"><w:txbxContent>%s</w:txbxContent></wps:txbx>", paras);
    snprintf(second, sizeof(second), "<wps:linkedTxbx id=\"1\" seq=\"1\"/>");
    {
        char a1[8192], a2[4096];

        snprintf(a1, sizeof(a1), anchor, 914400, 508000, 1, 1, 508000, first);         /* 72 pt down, 40 pt high */
        snprintf(a2, sizeof(a2), anchor, 5080000, 2540000, 2, 2, 2540000, second);    /* 400 pt down, 200 pt high */
        snprintf(doc, sizeof(doc), "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:wps=\"wps\"><w:body>"
                 "<w:p>%s%s<w:r><w:t>Body</w:t></w:r></w:p></w:body></w:document>", a1, a2);
    }

    d = docx_doc("word/document.xml", doc, NULL);
    CHECK(d != NULL);

    for (pass = 0; d && pass < 2 && have_layout_font(); pass++) {
        pd_layout* L = NULL;
        pd_sp ya, yh;

        pd_doc_set_font_resolver(d, layout_resolver, NULL);

        if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
            ya = glyph_y_of(L, 'A');
            yh = glyph_y_of(L, 'H');
            CHECK(ya > PD_PT(72) && ya < PD_PT(112));    /* Alpha in the first box */
            CHECK(yh > PD_PT(400) && yh < PD_PT(600));   /* Hotel gone on into the second */
        } else {
            CHECK(0);
        }

        pd_layout_free(L);

        if (pass == 0) {    /* and once more through Parade's .docx: the link kept */
            buf_t b = { 0 };
            unsigned char* x;
            size_t n;
            pd_doc* t = NULL;

            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            x = pd_zip_get((const unsigned char*)b.p, b.n, "word/document.xml", &n);
            CHECK(x && strstr((char*)x, "<wps:txbx id=\"1\">") && strstr((char*)x, "<wps:linkedTxbx id=\"1\" seq=\"1\"/>"));
            free(x);
            CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &t) == PD_OK);
            free(b.p);
            pd_doc_free(d);
            d = t;
        }
    }

    pd_doc_free(d);
}

/* a picture placed down the page by alignment (wp:positionV wp:align center, from the page): its middle at the
   page's, 72 pt high on a 792 pt page, so its top at 360 pt; still so after a .docx */
static void test_docx_float_valign(void) {
    static const char* doc =
        "<w:document xmlns:w=\"w\" xmlns:wp=\"wp\" xmlns:a=\"a\" xmlns:pic=\"pic\" xmlns:r=\"r\"><w:body><w:p><w:r><w:drawing>"
        "<wp:anchor distT=\"114300\" distB=\"114300\" distL=\"114300\" distR=\"114300\" simplePos=\"0\" relativeHeight=\"1\" "
        "behindDoc=\"0\" locked=\"0\" layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/>"
        "<wp:positionH relativeFrom=\"column\"><wp:align>left</wp:align></wp:positionH><wp:positionV relativeFrom=\"page\">"
        "<wp:align>center</wp:align></wp:positionV><wp:extent cx=\"914400\" cy=\"914400\"/><wp:wrapSquare wrapText=\"right\"/>"
        "<wp:docPr id=\"1\" name=\"P\"/><a:graphic><a:graphicData uri=\"pic\"><pic:pic><pic:blipFill><a:blip r:embed=\"rId1\"/>"
        "</pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"914400\" cy=\"914400\"/></a:xfrm>"
        "</pic:spPr></pic:pic></a:graphicData></a:graphic></wp:anchor></w:drawing></w:r><w:r><w:t>Text.</w:t></w:r></w:p>"
        "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
        "w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>";
    pd_doc* d = docx_doc("word/_rels/document.xml.rels",
                         "<Relationships xmlns=\"r\"><Relationship Id=\"rId1\" Type=\"t/image\" Target=\"media/image1.png\"/>"
                         "</Relationships>", "word/media/image1.png", "tests/data/rgba.png", "word/document.xml", doc, NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 2 && have_layout_font(); pass++) {
        pd_layout* L = NULL;
        pd_draw* it;
        int32_t n = 0, k, found = 0;

        pd_doc_set_font_resolver(d, layout_resolver, NULL);
        CHECK(pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK);
        pd_layout_page_items(L, 0, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, 0, it, n, &n);

        for (k = 0; k < n; k++) {
            if (it[k].kind == PD_DRAW_IMAGE) {
                found = 1;
                CHECK(labs((long)(it[k].y - PD_PT(360))) <= PD_PT(2));
            }
        }

        CHECK(found);
        free(it);
        pd_layout_free(L);

        if (pass == 0) {
            buf_t b = { 0 };
            unsigned char* x;
            size_t xn;
            pd_doc* t = NULL;

            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            x = pd_zip_get((const unsigned char*)b.p, b.n, "word/document.xml", &xn);
            CHECK(x && strstr((char*)x, "<wp:positionV relativeFrom=\"page\"><wp:align>center</wp:align>") &&
                  strstr((char*)x, "distT=\"114300\""));
            free(x);
            CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &t) == PD_OK);
            free(b.p);
            pd_doc_free(d);
            d = t;
        }
    }

    pd_doc_free(d);
}

/* art page borders: hearts (red, 20 pt) across the top and bottom drawn as a row of motifs with their strokes, black
   dots down the sides as small squares; the art and its size in points kept through a .docx */
static void test_docx_art_borders(void) {
    static const char* doc =
        "<w:document xmlns:w=\"w\"><w:body><w:p><w:r><w:t>Art</w:t></w:r></w:p><w:sectPr><w:pgSz w:w=\"12240\" "
        "w:h=\"15840\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" w:header=\"720\" "
        "w:footer=\"720\" w:gutter=\"0\"/><w:pgBorders w:offsetFrom=\"page\"><w:top w:val=\"hearts\" w:sz=\"20\" "
        "w:space=\"24\" w:color=\"FF0000\"/><w:left w:val=\"basicBlackDots\" w:sz=\"12\" w:space=\"24\" w:color=\"000000\"/>"
        "<w:bottom w:val=\"hearts\" w:sz=\"20\" w:space=\"24\" w:color=\"FF0000\"/><w:right w:val=\"basicBlackDots\" "
        "w:sz=\"12\" w:space=\"24\" w:color=\"000000\"/></w:pgBorders></w:sectPr></w:body></w:document>";
    pd_doc* d = docx_doc("word/document.xml", doc, NULL);
    int pass;

    CHECK(d != NULL);

    for (pass = 0; d && pass < 2 && have_layout_font(); pass++) {
        pd_layout* L = NULL;
        pd_draw* it;
        int32_t n = 0, k, red_strokes = 0, red_top = 0, dots_left = 0;

        pd_doc_set_font_resolver(d, layout_resolver, NULL);
        CHECK(pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK);
        pd_layout_page_items(L, 0, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, 0, it, n, &n);

        for (k = 0; k < n; k++) {   /* hearts: red, along the top 24 pt down; dots: black squares down the left */
            uint32_t col = it[k].kind == PD_DRAW_PATH ? (it[k].color ? it[k].color : it[k].fill) : it[k].color;

            red_strokes += it[k].kind == PD_DRAW_PATH && col == 0xFFFF0000u;
            red_top += col == 0xFFFF0000u && it[k].y < PD_PT(24 + 20 + 1) && it[k].y >= PD_PT(23);
            dots_left += it[k].kind == PD_DRAW_RULE && col == 0xFF000000u && it[k].x >= PD_PT(23) &&
                         it[k].x < PD_PT(24 + 12) && it[k].w < PD_PT(8) && it[k].h < PD_PT(8);
        }

        CHECK(red_top > 40);        /* a row of hearts across the 564 pt between the borders' ends */
        CHECK(red_strokes > 40);    /* their diamonds' strokes */
        CHECK(dots_left > 40);      /* a 12 pt dot a cell down 744 pt */
        free(it);
        pd_layout_free(L);

        if (pass == 0) {
            buf_t b = { 0 };
            unsigned char* x;
            size_t xn;
            pd_doc* t = NULL;

            CHECK(pd_doc_export(d, PD_CONV_DOCX, to_buf, &b) == PD_OK);
            x = pd_zip_get((const unsigned char*)b.p, b.n, "word/document.xml", &xn);
            CHECK(x && strstr((char*)x, "w:val=\"hearts\" w:sz=\"20\"") && strstr((char*)x, "w:val=\"basicBlackDots\""));
            free(x);
            CHECK(pd_doc_import(b.p, b.n, PD_CONV_DOCX, &t) == PD_OK);
            free(b.p);
            pd_doc_free(d);
            d = t;
        }
    }

    pd_doc_free(d);
}

/* cells set apart by the rows (w:trPr's tblCellSpacing, as tables pasted from the web have it), the table saying
   nothing: the table's spacing, and kept so through a .docx */
static void test_docx_row_cell_spacing(void) {
    pd_doc* d = docx_doc("word/document.xml",
                         "<w:document xmlns:w=\"w\"><w:body><w:tbl><w:tblPr><w:tblW w:w=\"4000\" w:type=\"dxa\"/></w:tblPr>"
                         "<w:tblGrid><w:gridCol w:w=\"2000\"/><w:gridCol w:w=\"2000\"/></w:tblGrid><w:tr><w:trPr>"
                         "<w:tblCellSpacing w:w=\"100\" w:type=\"dxa\"/></w:trPr><w:tc><w:p><w:r><w:t>a</w:t></w:r></w:p>"
                         "</w:tc><w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr></w:tbl><w:p/></w:body></w:document>",
                         NULL);
    pd_table_props tp;

    CHECK(d != NULL);

    if (d) {
        pd_block_id t = pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0);

        CHECK(pd_doc_table_props(d, t, &tp) == PD_OK && tp.cell_spacing == PD_PT(5));
        d = docx_again(d);
        t = d ? pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0) : 0;
        CHECK(d && pd_doc_table_props(d, t, &tp) == PD_OK && tp.cell_spacing == PD_PT(5));
    }

    pd_doc_free(d);
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
    test_docx_num_formats();
    test_docx_kept_parts();
    test_docx_exact_rows_keep_text();
    printf("docx headers and footers\n");
    test_docx_headers();
    printf("docx tables\n");
    test_docx_tables();
    printf("tables and pages\n");
    test_wide_table();
    test_grid_before();
    test_exact_row();
    test_cant_split();
    test_cell_width();
    test_cell_text_dir();
    test_cell_diagonals();
    test_cell_spacing();
    test_floating_table();
    test_page_features();
    test_para_border_lines();
    test_wrap_outline();
    test_wrap_both_sides();
    test_anchor_follows_paragraph();
    test_unequal_columns();
    test_next_column_section();
    test_border_export();
    test_vertical_section();
    printf("docx floats\n");
    test_docx_floats();
    test_docx_duplicate_media();
    test_docx_empty_mark_size();
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
    test_pptx_hidden();
    test_pptx_one_slide();
    test_pptx_autonum();
    test_pptx_picture_bullet();
    test_pptx_ole();
    test_pptx_kept_links();
    test_pptx_transparent_color();
    test_gif_frames();
    test_latex_import();
    test_latex_bibtex();
    test_latex_justified();
    test_svg();
    test_latex_indent();
    test_pptx_chart();
    test_pptx_ph_inherit();
    test_pptx_run_props();
    test_pptx_math();
    test_pptx_table_style();
    test_pptx_other_theme();
    printf("pptx writer\n");
    test_pptx_write_roundtrip();
    test_pptx_write_made();
    test_pptx_write_kept_masters();
    test_docx_smartart();
    test_docx_turned_text();
    test_docx_rtl();
    test_theme_links();
    test_templates();
    test_rtl_html_rtf();
    printf("docx drawings and text boxes\n");
    test_docx_drawings();
    printf("EMF pictures\n");
    test_emf();
    test_emf_clip();
    test_emf_shapes();
    test_emf_plus();
    test_emf_drawing_saved_as_source();
    test_emf_curves();
    printf("docx equations\n");
    test_docx_omml();
    printf("docx fields and references\n");
    test_docx_fields();
    test_docx_review();
    test_docx_theme_colors();
    test_docx_font_table();
    test_docx_controls();
    test_docx_charts();
    test_chart_kinds();
    test_docx_east_asian();
    test_docx_shading_edges();
    test_docx_group_textbox_table();
    test_docx_deleted_float();
    test_docx_deleted_inline_picture();
    test_docx_deleted_box_insertion();
    test_docx_group_turned_shapes();
    test_docx_canvas_3d();
    test_docx_custgeom_paths();
    test_docx_line_styles();
    test_docx_curves();
    test_docx_gradients();
    test_docx_pattern_fill();
    test_docx_picture_fill();
    test_docx_shape_effects();
    test_docx_text_in_shapes();
    test_wordart_on_path();
    test_docx_linked_textboxes();
    test_docx_float_valign();
    test_docx_art_borders();
    test_docx_row_cell_spacing();
    test_svg_out();
    test_drawing_png();
    test_docx_canvas_kept();
    test_drawing_story_copy();
    test_docx_tracked_objects();
    test_docx_drawing_rebuild();
    test_docx_canvas_fallback();
    test_docx_fallback_gfxdata();
    test_docx_preset_adjust();
    test_preset_eval();
    printf("docx embedded fonts\n");
    test_docx_embedded_font();
    printf("docx properties and page\n");
    test_docx_page_meta();
    printf("docx endnotes\n");
    test_docx_endnotes();
    test_docx_text_small();
    test_docx_line_rules();
    test_docx_bar_tab();
    test_docx_drop_cap();
    test_docx_text_boxes();
    test_docx_nested_fields();
    test_comment_balloons();
    test_docx_moves_formats();
    test_docx_omml_roundtrip();
    test_docx_ref_text();
    test_docx_char_scale();
    test_docx_picture_crop();
    test_docx_block_revisions();
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
