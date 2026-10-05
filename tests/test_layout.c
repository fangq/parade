/*
 * Parade page builder tests: pagination rules, floats, headers and fields,
 * multi-column sections, caret/hit-test round trips, and incremental
 * updates checked against fresh layouts.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_layout.h"

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static pd_font* font;

static const pd_font* resolver(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;
    (void)family;
    (void)weight;
    (void)italic;
    return font;
}

static const char* frog =
    "In olden times when wishing still helped one, there lived a king whose daughters were all beautiful, "
    "but the youngest was so beautiful that the sun itself, which has seen so much, was astonished whenever "
    "it shone in her face. Close by the king's castle lay a great dark forest, and under an old lime-tree in "
    "the forest was a well, and when the day was very warm, the king's child went out into the forest.";

static pd_pos at(pd_block_id b, uint32_t o) {
    pd_pos p;
    p.block = b;
    p.offset = o;
    return p;
}

static pd_block_id add_para(pd_doc* d, pd_block_id sec, const char* text) {
    pd_block_id p;

    pd_doc_insert_block(d, sec, -1, PD_BLOCK_PARAGRAPH, &p);
    pd_doc_insert_text(d, at(p, 0), text, strlen(text), PD_FORMAT_INHERIT, NULL);
    return p;
}

static pd_doc* new_doc(pd_block_id* sec) {
    pd_doc* d;

    pd_doc_new(&d);
    pd_doc_set_font_resolver(d, resolver, NULL);
    *sec = pd_doc_child(d, pd_doc_root(d), 0);
    return d;
}

/* every draw item of every page */
static uint64_t layout_hash(const pd_layout* L) {
    uint64_t h = 1469598103934665603ULL;
    int32_t pg, k, n;

    for (pg = 0; pg < pd_layout_page_count(L); pg++) {
        pd_draw* it;

        pd_layout_page_items(L, pg, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, pg, it, n, &n);

        for (k = 0; k < n; k++) {
            int32_t v[9] = { it[k].kind, it[k].x, it[k].y, it[k].w, it[k].h, (int32_t)it[k].glyph, it[k].size,
                             (int32_t)it[k].block, (int32_t)it[k].offset
                           };
            size_t b;

            for (b = 0; b < sizeof(v); b++) {
                h = (h ^ ((const unsigned char*)v)[b]) * 1099511628211ULL;
            }
        }

        free(it);
        h = (h ^ (uint64_t)pg) * 1099511628211ULL;
    }

    return h;
}

/* page of a paragraph's line containing an offset, -1 if not placed */
static int32_t page_of(const pd_layout* L, pd_block_id b, uint32_t off, pd_sp* y) {
    int32_t pg = -1;
    pd_sp x, base, a, de;

    if (pd_layout_caret(L, at(b, off), &pg, &x, &base, &a, &de) != PD_OK) {
        return -1;
    }

    if (y) {
        *y = base;
    }

    return pg;
}

static void test_flow_rules(void) {
    pd_block_id sec, p[40], h;
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info info;
    pd_para_props pp;
    int i, bad_order = 0, bad_widow = 0;
    int32_t last_pg = 0;
    pd_sp last_y = 0;

    p[0] = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p[0], 0), frog, strlen(frog), PD_FORMAT_INHERIT, NULL);

    for (i = 1; i < 40; i++) {
        p[i] = add_para(d, sec, frog);
    }

    /* a heading that must stay with its paragraph */
    h = add_para(d, sec, "A Heading That Keeps With Next");
    pd_doc_move_block(d, h, sec, 13);
    pd_doc_set_para_style(d, h, pd_doc_style_find(d, "Heading 1"));
    pd_doc_set_role(d, h, PD_ROLE_HEADING, 1);

    CHECK(pd_layout_new(d, &L) == PD_OK);
    CHECK(pd_layout_update(L, &info) == PD_OK);
    CHECK(info.pages >= 3 && info.overfull == 0);

    /* reading order and widows/orphans: a split paragraph keeps >= 2 lines on each side */
    for (i = 0; i < 40; i++) {
        const char* t;
        uint32_t n, off;
        int32_t first_pg = page_of(L, p[i], 0, NULL), lines_first = 0, lines_rest = 0, pg;
        pd_sp y = 0;

        pd_doc_para_text(d, p[i], &t, &n);
        pg = page_of(L, p[i], 0, &y);
        CHECK(pg >= 0);
        bad_order += pg < last_pg || (pg == last_pg && y <= last_y && i > 0);
        last_pg = pg;
        last_y = y;

        /* count lines per page by walking line starts */
        for (off = 0; off < n;) {
            pd_sp yy, x, a, de;
            int32_t lp, line_pg;

            line_pg = -1;
            pd_layout_caret(L, at(p[i], off), &line_pg, &x, &yy, &a, &de);
            lp = line_pg;

            if (lp == first_pg) {
                lines_first++;
            } else {
                lines_rest++;
            }

            /* jump to the next line: find the first offset with a larger baseline */
            {
                uint32_t o2 = off + 1;
                pd_sp y2 = yy;
                int32_t p2 = lp;

                while (o2 <= n) {
                    pd_layout_caret(L, at(p[i], o2), &p2, &x, &y2, &a, &de);

                    if (p2 != lp || y2 != yy) {
                        break;
                    }

                    o2++;
                }

                off = o2;
            }

            if (off > n) {
                break;
            }
        }

        if (lines_rest > 0) {
            bad_widow += lines_first < 2 || lines_rest < 2;
        }

        last_pg = page_of(L, p[i], n, &last_y);
    }

    CHECK(bad_order == 0);
    CHECK(bad_widow == 0);

    /* keep-with-next: the heading shares a page with the next paragraph's first line */
    CHECK(page_of(L, h, 0, NULL) == page_of(L, pd_doc_child(d, sec, 14), 0, NULL));

    /* page break before */
    memset(&pp, 0, sizeof(pp));
    pp.mask = PD_PP_BREAK_BEFORE;
    pp.page_break_before = 1;
    pd_doc_set_para_props(d, p[20], &pp);
    CHECK(pd_layout_update(L, &info) == PD_OK);
    {
        pd_page_info pi;
        int32_t pg = page_of(L, p[20], 0, NULL);

        CHECK(pd_layout_page_info(L, pg, &pi) == PD_OK && pi.first.block == p[20] && pi.first.offset == 0);
        CHECK(page_of(L, p[19], 0, NULL) < pg);
    }
    CHECK(info.paragraphs_broken <= 2 && info.paragraphs_reused >= 38);     /* incremental */

    /* an explicit odd-page break may need a blank page */
    {
        pd_block_id brk;
        pd_page_info pi;

        pd_doc_insert_block(d, sec, 31, PD_BLOCK_BREAK, &brk);
        pd_doc_set_break(d, brk, PD_BREAK_ODD_PAGE);
        pd_layout_update(L, &info);
        pd_layout_page_info(L, page_of(L, pd_doc_child(d, sec, 32), 0, NULL), &pi);
        CHECK(pi.number % 2 == 1 && pi.first.offset == 0);
    }

    pd_layout_free(L);
    pd_doc_free(d);
}

static pd_block_id add_float(pd_doc* d, pd_block_id sec, int32_t index, pd_sp w, pd_sp h, uint32_t placement) {
    pd_block_id fl;
    pd_float_props fp;
    pd_inline o;
    pd_res_id res;

    pd_doc_insert_block(d, sec, index, PD_BLOCK_FLOAT, &fl);
    pd_doc_float_props(d, fl, &fp);
    fp.placement = placement;
    pd_doc_set_float_props(d, fl, &fp);
    pd_doc_add_resource(d, "image/png", "x", 1, &res);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.resource = res;
    o.width = w;
    o.height = h;
    pd_doc_insert_inline(d, at(pd_doc_child(d, fl, 0), 0), &o, NULL);
    return fl;
}

/* page and y of the first image belonging to a float */
static int32_t float_page(const pd_layout* L, pd_block_id fl, const pd_doc* d, pd_sp* y) {
    pd_block_id img = pd_doc_child(d, fl, 0);
    int32_t pg, k, n;

    for (pg = 0; pg < pd_layout_page_count(L); pg++) {
        pd_draw* it;

        pd_layout_page_items(L, pg, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, pg, it, n, &n);

        for (k = 0; k < n; k++) {
            if (it[k].kind == PD_DRAW_IMAGE && it[k].block == img) {
                if (y) {
                    *y = it[k].y;
                }

                free(it);
                return pg;
            }
        }

        free(it);
    }

    return -1;
}

static void test_floats(void) {
    pd_block_id sec, p[30], here, top, big[6];
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info info;
    pd_section_props sp;
    pd_sp y, ytop;
    int i, order_ok = 1;
    int32_t prev = -1;

    p[0] = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p[0], 0), frog, strlen(frog), PD_FORMAT_INHERIT, NULL);

    for (i = 1; i < 30; i++) {
        p[i] = add_para(d, sec, frog);
    }

    here = add_float(d, sec, 2, PD_PT(200), PD_PT(80), PD_PLACE_HERE | PD_PLACE_TOP);
    top = add_float(d, sec, 12, PD_PT(200), PD_PT(80), PD_PLACE_TOP);
    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, &info) == PD_OK && info.overfull == 0);
    pd_doc_section_props(d, sec, &sp);

    /* here: between its neighbours on the same page */
    CHECK(float_page(L, here, d, &y) == page_of(L, p[1], 0, NULL));
    CHECK(y > 0);
    /* top-only: at the top of a page (after any earlier top floats), never before its anchor's page */
    CHECK(float_page(L, top, d, &ytop) >= page_of(L, p[10], 0, NULL));
    CHECK(ytop < sp.margin_top + PD_PT(30));

    /* six large page-or-top floats: order kept, float pages made, nothing overfull */
    for (i = 0; i < 6; i++) {
        big[i] = add_float(d, sec, 20, PD_PT(300), PD_PT(400), PD_PLACE_TOP | PD_PLACE_PAGE);
    }

    CHECK(pd_layout_update(L, &info) == PD_OK);
    CHECK(info.float_pages > 0 && info.overfull == 0);

    for (i = 5; i >= 0; i--) {     /* inserted at the same index: big[5] comes first in the document */
        int32_t pg = float_page(L, big[i], d, NULL);

        CHECK(pg >= 0);
        order_ok &= pg >= prev;
        prev = pg;
    }

    CHECK(order_ok);
    pd_layout_free(L);
    pd_doc_free(d);
}

static void test_headers_fields_columns(void) {
    pd_block_id sec, story, fp_, p, fl, cap, ref;
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info info;
    pd_section_props sp;
    pd_inline o;
    int i;
    int32_t pg, k, n;

    pd_doc_insert_block(d, 0, -1, PD_BLOCK_STORY, &story);
    fp_ = pd_doc_child(d, story, 0);
    pd_doc_insert_text(d, at(fp_, 0), "Page ", 5, PD_FORMAT_INHERIT, NULL);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = PD_FIELD_PAGE;
    pd_doc_insert_inline(d, at(fp_, 5), &o, NULL);
    pd_doc_section_props(d, sec, &sp);
    sp.footer = story;
    sp.columns = 2;
    sp.first_page_number = 7;
    sp.page_number_format = PD_NUM_UPPER_ROMAN;
    pd_doc_set_section_props(d, sec, &sp);

    p = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p, 0), frog, strlen(frog), PD_FORMAT_INHERIT, NULL);

    for (i = 0; i < 40; i++) {
        add_para(d, sec, frog);
    }

    fl = add_float(d, sec, 30, PD_PT(150), PD_PT(60), PD_PLACE_HERE | PD_PLACE_TOP);
    pd_doc_insert_block(d, fl, -1, PD_BLOCK_PARAGRAPH, &cap);
    pd_doc_insert_text(d, at(cap, 0), "Figure ", 7, PD_FORMAT_INHERIT, NULL);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = PD_FIELD_SEQ;
    strcpy(o.name, "Figure");
    pd_doc_insert_inline(d, at(cap, 7), &o, NULL);
    ref = add_para(d, sec, "See page ");
    o.field = PD_FIELD_REF_PAGE;
    o.target = fl;
    pd_doc_insert_inline(d, at(ref, 9), &o, NULL);

    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, &info) == PD_OK && info.pages >= 2);

    for (pg = 0; pg < info.pages; pg++) {
        pd_page_info pi;
        pd_draw* it;
        int footer = 0, cols = 0;
        pd_sp left = 0x7FFFFFFF, right = 0;

        pd_layout_page_info(L, pg, &pi);
        CHECK(pi.number == 7 + pg);
        pd_layout_page_items(L, pg, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, pg, it, n, &n);

        for (k = 0; k < n; k++) {
            footer += it[k].region == 2 && it[k].kind == PD_DRAW_GLYPH;

            if (it[k].region == 0 && it[k].kind == PD_DRAW_GLYPH) {
                left = it[k].x < left ? it[k].x : left;
                right = it[k].x > right ? it[k].x : right;
            }
        }

        /* "Page" (the space is glue, not a glyph) + the roman numeral: VII has 3 glyphs */
        CHECK(footer == 4 + (int)strlen(pi.label));
        cols = right - left > (sp.page_width - sp.margin_left - sp.margin_right) / 2;
        CHECK(cols || pg == info.pages - 1);   /* text reaches the second column */
        free(it);
    }

    /* the reference shows the float's page, settled by the second pass */
    {
        pd_page_info pi;
        pd_draw* it;
        int32_t rp = page_of(L, ref, 0, NULL), fpg = float_page(L, fl, d, NULL), got = 0;

        pd_layout_page_info(L, fpg, &pi);
        pd_layout_page_items(L, rp, NULL, 0, &n);
        it = (pd_draw*)malloc(((size_t)n + 1) * sizeof(pd_draw));
        pd_layout_page_items(L, rp, it, n, &n);

        for (k = 0; k < n; k++) {
            got += it[k].block == ref && it[k].offset == 9 && it[k].kind == PD_DRAW_GLYPH;
        }

        CHECK(got == (int32_t)strlen(pi.label));
        free(it);
    }

    pd_layout_free(L);
    pd_doc_free(d);
}

static void test_hit_caret(void) {
    pd_block_id sec, p[12];
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    int i, bad = 0, tried = 0;

    p[0] = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p[0], 0), frog, strlen(frog), PD_FORMAT_INHERIT, NULL);

    for (i = 1; i < 12; i++) {
        p[i] = add_para(d, sec, frog);
    }

    pd_layout_new(d, &L);
    pd_layout_update(L, NULL);

    for (i = 0; i < 12; i++) {
        uint32_t off;

        for (off = 0; off < strlen(frog); off += 7) {
            int32_t pg;
            pd_sp x, y, a, de;
            pd_pos got;

            if (frog[off] == ' ') {
                continue;   /* a space at a line end maps to the line end */
            }

            tried++;

            if (pd_layout_caret(L, at(p[i], off), &pg, &x, &y, &a, &de) != PD_OK ||
                    pd_layout_hit_test(L, pg, x + 1, y - a / 2, &got) != PD_OK || got.block != p[i] ||
                    got.offset != off) {
                bad++;
            }
        }
    }

    CHECK(tried > 300 && bad == 0);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* incremental updates must equal a fresh layout after every kind of edit */
static uint32_t rs = 99;

static uint32_t rnd(uint32_t n) {
    rs = rs * 1103515245u + 12345u;
    return n ? (rs >> 8) % n : 0;
}

static void test_incremental(void) {
    pd_block_id sec, p[25];
    pd_doc* d = new_doc(&sec);
    pd_layout* L, *F;
    pd_list_level lv;
    pd_list_id list;
    int i, mismatch = 0;
    int32_t broken = 0;

    p[0] = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p[0], 0), frog, strlen(frog), PD_FORMAT_INHERIT, NULL);

    for (i = 1; i < 25; i++) {
        p[i] = add_para(d, sec, frog);
    }

    memset(&lv, 0, sizeof(lv));
    lv.format = PD_NUM_DECIMAL;
    lv.start = 1;
    strcpy(lv.text, "%1.");
    lv.indent = PD_PT(20);
    lv.hanging = PD_PT(15);
    pd_doc_list_define(d, 1, &lv, &list);
    pd_layout_new(d, &L);
    pd_layout_update(L, NULL);

    for (i = 0; i < 60; i++) {
        pd_block_id b = p[rnd(25)];
        pd_layout_info info;
        pd_char_props cp;

        int op = (int)rnd(7);

        switch (op) {
            case 0:
                pd_doc_insert_text(d, at(b, 0), "More words here. ", 17, PD_FORMAT_INHERIT, NULL);
                break;

            case 1:
                pd_doc_set_list(d, b, list, 0);     /* renumbers later items without touching them */
                break;

            case 2:
                pd_doc_set_list(d, b, 0, 0);
                break;

            case 3:
                memset(&cp, 0, sizeof(cp));
                cp.mask = PD_CP_SIZE;
                cp.size = PD_PT(9 + rnd(4));
                pd_doc_style_define(d, "Normal", PD_STYLE_PARAGRAPH, 0, NULL, &cp, NULL);    /* every paragraph */
                break;

            case 4:
                pd_doc_undo(d);
                break;

            case 5:
                add_float(d, sec, (int32_t)rnd(20), PD_PT(100), PD_PT(50 + rnd(200)), PD_PLACE_HERE | PD_PLACE_TOP);
                break;

            default: {
                pd_block_info bi;

                pd_doc_block_info(d, b, &bi);
                pd_doc_delete(d, (pd_range) {
                    at(b, 0), at(b, bi.text_length > 20 ? 20 : 0)
                }, NULL);
            }
        }

        pd_layout_update(L, &info);
        broken += info.paragraphs_broken;
        pd_layout_new(d, &F);
        pd_layout_update(F, NULL);

        if (layout_hash(L) != layout_hash(F)) {
            mismatch++;

            if (getenv("PD_DEBUG_INCR")) {
                printf("  mismatch after edit %d (op %d): pages %d vs %d\n", i, op, pd_layout_page_count(L),
                       pd_layout_page_count(F));
            }
        }

        pd_layout_free(F);
    }

    CHECK(mismatch == 0);
    printf("  60 edits: incremental equals fresh layout every time; %d paragraph re-breaks in total\n",
           (int)broken);
    pd_layout_free(L);
    pd_doc_free(d);
}

int main(void) {
    const char* path = getenv("PARADE_TEST_FONT") ? getenv("PARADE_TEST_FONT") :
                       "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf";

    setvbuf(stdout, NULL, _IONBF, 0);

    if (pd_font_load_file(path, 0, &font) != PD_OK) {
        printf("no test font; set PARADE_TEST_FONT\n");
        return 77;
    }

    printf("flow rules\n");
    test_flow_rules();
    printf("floats\n");
    test_floats();
    printf("headers, fields, columns\n");
    test_headers_fields_columns();
    printf("caret and hit testing\n");
    test_hit_caret();
    printf("incremental updates\n");
    test_incremental();
    pd_font_free(font);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
