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

static void test_fallback(void) {
    pd_font* cjk = NULL;
    pd_block_id sec, p;
    pd_doc* d;
    pd_layout* L;
    pd_draw it[256];
    int32_t n, k, latin = 0, han = 0, wrong = 0;
    const char* text = "Mixed \xE4\xB8\xAD\xE6\x96\x87 text";
    const pd_font* fb[1];

    if (pd_font_load_file("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0, &cjk) != PD_OK) {
        printf("  (no CJK font, skipped)\n");
        return;
    }

    d = new_doc(&sec);
    p = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p, 0), text, strlen(text), PD_FORMAT_INHERIT, NULL);
    pd_layout_new(d, &L);
    pd_layout_update(L, NULL);
    pd_layout_page_items(L, 0, it, 256, &n);
    CHECK(n > 0);

    for (k = 0; k < n; k++) {   /* without fallback the Han characters are .notdef in the text font */
        wrong += it[k].kind == PD_DRAW_GLYPH && it[k].glyph == 0;
    }

    CHECK(wrong == 2);
    fb[0] = cjk;
    CHECK(pd_doc_set_fallback_fonts(d, fb, 1) == PD_OK);
    pd_layout_update(L, NULL);
    pd_layout_page_items(L, 0, it, 256, &n);

    for (k = 0; k < n; k++) {
        if (it[k].kind != PD_DRAW_GLYPH) {
            continue;
        }

        if (it[k].text >= 0x4E00) {
            han += it[k].font == cjk && it[k].glyph != 0;
        } else {
            latin += it[k].font == font;
        }
    }

    CHECK(han == 2 && latin == 9);     /* "Mixed" + "text": spaces are glue, not glyphs */
    pd_layout_free(L);
    pd_doc_free(d);
    pd_font_free(cjk);
}

/* hyphenated lines of a paragraph laid out at a width */
static int32_t count_hyphens(const pd_doc* d, pd_block_id para, pd_sp col, int32_t* lines) {
    pd_para* pp;
    pd_params prm;
    pd_line ln;
    int32_t i, h = 0;

    pd_para_new(&pp);
    pd_doc_para_build(d, para, col, pp, &prm);
    pd_para_break(pp, &prm, NULL);
    *lines = pd_para_line_count(pp);

    for (i = 0; i < *lines; i++) {
        pd_para_get_line(pp, i, &ln);
        h += ln.hyphenated;
    }

    pd_para_free(pp);
    return h;
}

static void test_hyphenation(void) {
    pd_hyph* h;
    uint8_t pts[16];
    const char* text = "Characteristically, international organizations demonstrate extraordinary "
                       "responsibilities concerning multidimensional considerations and "
                       "incomprehensibilities of contemporary administrative representations.";
    pd_block_id sec, p;
    pd_doc* d;
    pd_char_props cp;
    int32_t lines0, lines1, h0, h1, n, i;
    pd_range all;

    if (pd_hyph_load_file("/usr/share/hyphen/hyph_en_US.dic", &h) != PD_OK) {
        printf("  (no hyph_en_US.dic, skipped)\n");
        return;
    }

    CHECK(pd_hyph_word(h, "hyphenation", 11, pts) == PD_OK);    /* hy-phen-ation */
    n = 0;

    for (i = 0; i <= 11; i++) {
        n += pts[i];
    }

    CHECK(n == 2 && pts[2] && pts[6]);
    CHECK(pd_hyph_word(h, "Table", 5, pts) == PD_OK && pts[2] && !pts[1] && !pts[3]);   /* Ta-ble, case-folded */
    CHECK(pd_hyph_load_memory("UTF-8\n", 6, &(pd_hyph*) {
        NULL
    }) == PD_ERR_FORMAT);

    d = new_doc(&sec);
    p = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p, 0), text, strlen(text), PD_FORMAT_INHERIT, NULL);
    h0 = count_hyphens(d, p, PD_PT(110), &lines0);
    CHECK(h0 == 0);                                     /* no patterns registered */

    CHECK(pd_doc_set_hyphenator(d, "de", h) == PD_OK);  /* wrong language: still none */
    CHECK(count_hyphens(d, p, PD_PT(110), &n) == 0);

    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_LANG;
    strcpy(cp.lang, "en-US");
    all.start = at(p, 0);
    all.end = at(p, (uint32_t)strlen(text));
    CHECK(pd_doc_set_char_props(d, all, &cp) == PD_OK);
    CHECK(pd_doc_set_hyphenator(d, "EN", h) == PD_OK);  /* prefix match, case-insensitive */
    h1 = count_hyphens(d, p, PD_PT(110), &lines1);
    CHECK(h1 > 0);
    printf("  hyphens: %d lines -> %d lines, %d hyphenated\n", lines0, lines1, h1);

    CHECK(pd_doc_set_hyphenator(d, "EN", NULL) == PD_OK);
    CHECK(count_hyphens(d, p, PD_PT(110), &n) == 0 && n == lines0);
    pd_doc_free(d);
    pd_hyph_free(h);
}

/* all draw items of a page (caller frees) */
static pd_draw* items(const pd_layout* L, int32_t pg, int32_t* n) {
    pd_draw* it;

    pd_layout_page_items(L, pg, NULL, 0, n);
    it = (pd_draw*)malloc(((size_t) * n + 1) * sizeof(pd_draw));
    pd_layout_page_items(L, pg, it, *n, n);
    return it;
}

/* a story holding one paragraph of text */
static pd_block_id add_story(pd_doc* d, const char* text) {
    pd_block_id st;

    pd_doc_insert_block(d, 0, -1, PD_BLOCK_STORY, &st);
    pd_doc_insert_text(d, at(pd_doc_child(d, st, 0), 0), text, strlen(text), PD_FORMAT_INHERIT, NULL);
    return st;
}

static void add_footnote(pd_doc* d, pd_block_id para, uint32_t off, pd_block_id story) {
    pd_inline o;

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FOOTNOTE;
    o.target = story;
    pd_doc_insert_inline(d, at(para, off), &o, NULL);
}

static void test_footnotes(void) {
    pd_block_id sec, p[30], notes[5];
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info info;
    pd_section_props sp;
    int32_t i, k, n, pg, ok_pages = 0;
    char buf[64];

    for (i = 0; i < 30; i++) {
        p[i] = add_para(d, sec, frog);
    }

    for (i = 0; i < 5; i++) {
        snprintf(buf, sizeof(buf), "Note %d: the well was deep and dark, and the ball sank.", i + 1);
        notes[i] = add_story(d, buf);
        add_footnote(d, p[i * 6 + 1], 20, notes[i]);
    }

    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, &info) == PD_OK);
    CHECK(info.overfull == 0);
    pd_doc_section_props(d, sec, &sp);

    for (i = 0; i < 5; i++) {   /* each body is on the page of its mark, below all body text */
        pd_sp ymark = 0, ynote = 0, body_bottom = 0, rule_y = -1;
        pd_draw* it;
        int32_t mark_page = page_of(L, p[i * 6 + 1], 20, &ymark), note_lines = 0;
        pd_block_id np = pd_doc_child(d, notes[i], 0);

        pg = page_of(L, np, 0, &ynote);
        CHECK(pg == mark_page && pg >= 0);

        if (pg < 0) {
            continue;
        }

        it = items(L, pg, &n);

        for (k = 0; k < n; k++) {
            if (it[k].region == 0 && it[k].kind == PD_DRAW_GLYPH && it[k].y > body_bottom) {
                body_bottom = it[k].y;
            }

            if (it[k].region == 4 && it[k].kind == PD_DRAW_RULE) {
                rule_y = it[k].y;
            }

            note_lines += it[k].region == 4 && it[k].kind == PD_DRAW_GLYPH && it[k].block == np;
        }

        CHECK(note_lines > 10);
        CHECK(rule_y > body_bottom && ynote > rule_y);
        CHECK(ynote < sp.page_height - sp.margin_bottom);
        ok_pages += rule_y > body_bottom && ynote > rule_y;
        free(it);
    }

    /* the note number is set before the body */
    {
        pd_draw* it;
        int32_t found = 0;
        pd_block_id np = pd_doc_child(d, notes[2], 0);

        pg = page_of(L, np, 0, NULL);
        it = items(L, pg, &n);

        for (k = 0; k < n; k++) {
            found += it[k].block == np && it[k].kind == PD_DRAW_GLYPH && it[k].text == '3' && it[k].offset == 0;
        }

        CHECK(found >= 1);
        free(it);
    }

    printf("  %d notes placed on %d pages (%d checked)\n", 5, info.pages, ok_pages);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* a table of rows x cols with text from a callback */
static pd_block_id add_table(pd_doc* d, pd_block_id sec, int32_t rows, int32_t cols, int32_t header_rows,
                             const char* (*text)(int32_t r, int32_t c, char* buf)) {
    pd_block_id t, row, cell;
    pd_table_props tp;
    int32_t r, c;
    char buf[256];

    pd_doc_insert_block(d, sec, -1, PD_BLOCK_TABLE, &t);
    pd_doc_table_props(d, t, &tp);
    tp.header_rows = header_rows;
    pd_doc_set_table_props(d, t, &tp);

    for (r = 0; r < rows; r++) {
        if (r == 0) {
            row = pd_doc_child(d, t, 0);
        } else {
            pd_doc_insert_block(d, t, -1, PD_BLOCK_ROW, &row);
        }

        for (c = 0; c < cols; c++) {
            const char* s;

            if (c > 0) {
                pd_doc_insert_block(d, row, -1, PD_BLOCK_CELL, &cell);
            } else {
                cell = pd_doc_child(d, row, 0);
            }

            s = text(r, c, buf);
            pd_doc_insert_text(d, at(pd_doc_child(d, cell, 0), 0), s, strlen(s), PD_FORMAT_INHERIT, NULL);
        }
    }

    return t;
}

static const char* cell_text(int32_t r, int32_t c, char* buf) {
    if (r == 0) {
        return c == 0 ? "Name" : c == 1 ? "Count" : "Description";
    }

    if (c == 0) {
        sprintf(buf, "Item %d", (int)r);
    } else if (c == 1) {
        sprintf(buf, "%d", (int)(r * 37 % 1000));
    } else {
        sprintf(buf, "%s", r % 3 ? "a short note" :
                "a longer description that will have to wrap onto several lines in its cell");
    }

    return buf;
}

static pd_block_id cell_para(const pd_doc* d, pd_block_id t, int32_t r, int32_t c) {
    return pd_doc_child(d, pd_doc_child(d, pd_doc_child(d, t, r), c), 0);
}

static void test_tables(void) {
    pd_block_id sec, t, span_row, span_cell;
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info info;
    pd_section_props sp;
    pd_cell_props cp;
    int32_t r, pg, n, k, pages, rows_ok = 1, rules = 0, row50_pages = 0;
    pd_sp x0, x1, x2, y, ya = 0, yb = 0;

    add_para(d, sec, frog);
    t = add_table(d, sec, 80, 3, 1, cell_text);
    add_para(d, sec, frog);

    /* a row with one cell spanning all three columns */
    pd_doc_insert_block(d, t, 5, PD_BLOCK_ROW, &span_row);
    span_cell = pd_doc_child(d, span_row, 0);
    pd_doc_cell_props(d, span_cell, &cp);
    cp.col_span = 3;
    cp.background = 0xFFE0E0E0u;
    CHECK(pd_doc_set_cell_props(d, span_cell, &cp) == PD_OK);
    pd_doc_insert_text(d, at(pd_doc_child(d, span_cell, 0), 0), "spanning all columns", 20, PD_FORMAT_INHERIT, NULL);

    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, &info) == PD_OK);
    CHECK(info.overfull == 0);
    pages = pd_layout_page_count(L);
    CHECK(pages >= 3);
    pd_doc_section_props(d, sec, &sp);

    /* columns left to right inside the text column; the description column is the widest */
    {
        pd_sp cx[4];
        int32_t c;

        for (c = 0; c < 3; c++) {
            pd_layout_caret(L, at(cell_para(d, t, 3, c), 0), &pg, &cx[c], &y, NULL, NULL);
        }

        x0 = cx[0];
        x1 = cx[1];
        x2 = cx[2];
        CHECK(x0 >= sp.margin_left && x1 > x0 && x2 > x1);
    }

    {
        /* the widest description ends inside the text column */
        pd_block_id lp = cell_para(d, t, 3, 2);
        pd_sp xe;
        const char* tx;
        uint32_t len;

        pd_doc_para_text(d, lp, &tx, &len);
        pd_layout_caret(L, at(lp, len), &pg, &xe, &y, NULL, NULL);
        CHECK(xe <= sp.page_width - sp.margin_right && xe > x2);
    }

    /* every row whole on one page; the header row on every page the table touches */
    for (r = 1; r < 81; r++) {
        int32_t p0 = page_of(L, cell_para(d, t, r, 0), 0, &ya);
        int32_t c, nc = r == 5 ? 1 : 3;

        for (c = 1; c < nc; c++) {
            int32_t p1 = page_of(L, cell_para(d, t, r, c), 0, &yb);
            rows_ok &= p1 == p0 && yb == ya;
        }
    }

    CHECK(rows_ok);

    for (pg = 0; pg < pages; pg++) {
        pd_draw* it = items(L, pg, &n);
        int32_t hdr = 0, body = 0;

        for (k = 0; k < n; k++) {
            hdr += it[k].kind == PD_DRAW_GLYPH && it[k].block == cell_para(d, t, 0, 0);
            body += it[k].kind == PD_DRAW_GLYPH && it[k].block == cell_para(d, t, 50, 0);
            rules += it[k].kind == PD_DRAW_RULE;
        }

        if (pg > 0 && pg < pages - 1) {
            CHECK(hdr == 4);    /* "Name" repeated */
        }

        row50_pages += body > 0;
        free(it);
    }

    CHECK(row50_pages == 1);    /* a one-line row is never split across pages */
    CHECK(rules > 100);
    /* a fresh layout equals the updated one */
    {
        pd_layout* L2;
        uint64_t h1 = layout_hash(L), h2;

        pd_layout_new(d, &L2);
        pd_layout_update(L2, NULL);
        h2 = layout_hash(L2);
        CHECK(h1 == h2);
        pd_layout_free(L2);
    }

    printf("  80-row table on %d pages, column x %.1f/%.1f/%.1f pt\n", pages, x0 / 65536.0, x1 / 65536.0,
           x2 / 65536.0);
    pd_layout_free(L);
    pd_doc_free(d);
}

static void test_wrap(void) {
    pd_block_id sec, fl, p1, p2;
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_float_props fp;
    pd_section_props sp;
    pd_draw* it;
    pd_sp fy = 0, fx = 0, fw = 0, fh = 0;
    int32_t k, n, beside = 0, below = 0, bad = 0;

    add_para(d, sec, frog);
    fl = add_float(d, sec, -1, PD_PT(120), PD_PT(90), PD_PLACE_HERE);
    pd_doc_float_props(d, fl, &fp);
    fp.wrap = PD_WRAP_LEFT;
    fp.width = PD_PT(120);
    fp.gap = PD_PT(8);
    pd_doc_set_float_props(d, fl, &fp);
    p1 = add_para(d, sec, frog);
    p2 = add_para(d, sec, frog);
    pd_doc_section_props(d, sec, &sp);

    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, NULL) == PD_OK);
    it = items(L, 0, &n);

    for (k = 0; k < n; k++) {
        if (it[k].kind == PD_DRAW_IMAGE) {
            fx = it[k].x;
            fy = it[k].y;
            fw = it[k].w;
            fh = it[k].h;
        }
    }

    CHECK(fh == PD_PT(90) && fx == sp.margin_left);

    for (k = 0; k < n; k++) {
        if (it[k].kind != PD_DRAW_GLYPH || (it[k].block != p1 && it[k].block != p2)) {
            continue;
        }

        if (it[k].y - PD_PT(8) < fy + fh) {     /* a line overlapping the float's height */
            beside++;
            bad += it[k].x < fx + fw + PD_PT(8);
        } else {
            below++;
        }
    }

    CHECK(beside > 50 && below > 50 && bad == 0);
    printf("  %d glyphs beside the float, %d below\n", beside, below);
    free(it);

    /* right side */
    fp.wrap = PD_WRAP_RIGHT;
    pd_doc_set_float_props(d, fl, &fp);
    pd_layout_update(L, NULL);
    it = items(L, 0, &n);
    bad = beside = 0;

    for (k = 0; k < n; k++) {
        if (it[k].kind == PD_DRAW_IMAGE) {
            fx = it[k].x;
        }
    }

    CHECK(fx + PD_PT(120) == sp.page_width - sp.margin_right);

    for (k = 0; k < n; k++) {
        if (it[k].kind == PD_DRAW_GLYPH && it[k].block == p1 && it[k].y - PD_PT(8) < fy + fh) {
            beside++;
            bad += it[k].x + it[k].w > fx;
        }
    }

    CHECK(beside > 50 && bad == 0);
    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

static void test_continuous(void) {
    pd_block_id s1, s2, s3, q[16];
    pd_doc* d = new_doc(&s1);
    pd_layout* L;
    pd_section_props sp;
    pd_sp y_last1 = 0, y_first2 = 0, y, ybot[2] = { 0, 0 }, xcol;
    int32_t i, pg;

    for (i = 0; i < 2; i++) {
        add_para(d, s1, frog);
    }

    pd_doc_insert_block(d, pd_doc_root(d), -1, PD_BLOCK_SECTION, &s2);
    pd_doc_section_props(d, s2, &sp);
    sp.continuous = 1;
    sp.columns = 2;
    CHECK(pd_doc_set_section_props(d, s2, &sp) == PD_OK);

    for (i = 0; i < 5; i++) {
        q[i] = add_para(d, s2, frog);
    }

    pd_doc_insert_block(d, pd_doc_root(d), -1, PD_BLOCK_SECTION, &s3);
    pd_doc_section_props(d, s3, &sp);
    sp.continuous = 1;
    pd_doc_set_section_props(d, s3, &sp);
    add_para(d, s3, frog);

    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, NULL) == PD_OK);
    CHECK(pd_layout_page_count(L) == 1);
    page_of(L, pd_doc_child(d, s1, 2), 0, &y_last1);
    pg = page_of(L, q[0], 0, &y_first2);
    CHECK(pg == 0 && y_first2 > y_last1);

    /* balanced: both columns end within a line of each other */
    xcol = sp.margin_left + (sp.page_width - sp.margin_left - sp.margin_right) / 2;

    for (i = 0; i < 5; i++) {
        pd_sp x;
        uint32_t len = (uint32_t)strlen(frog), o;

        for (o = 0; o <= len; o += 8) {
            pd_layout_caret(L, at(q[i], o), &pg, &x, &y, NULL, NULL);
            ybot[x >= xcol] = y > ybot[x >= xcol] ? y : ybot[x >= xcol];
        }
    }

    CHECK(ybot[0] > 0 && ybot[1] > 0);
    CHECK(ybot[0] - ybot[1] < PD_PT(15) && ybot[1] - ybot[0] < PD_PT(15));
    page_of(L, pd_doc_child(d, s3, 1), 0, &y);
    CHECK(y > ybot[0] && y > ybot[1]);
    printf("  balanced columns end at %.1f / %.1f pt; next section at %.1f pt\n", ybot[0] / 65536.0,
           ybot[1] / 65536.0, y / 65536.0);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* sum of squared empty space at the bottom of every column but the last */
static double page_slack(const pd_layout* L, const pd_section_props* sp) {
    double s = 0;
    int32_t pg, k, n, c, nc = sp->columns < 1 ? 1 : sp->columns, np = pd_layout_page_count(L);
    pd_sp colw = (sp->page_width - sp->margin_left - sp->margin_right + sp->column_gap) / nc;

    for (pg = 0; pg < np; pg++) {
        pd_draw* it = items(L, pg, &n);
        pd_sp bottom[16];

        memset(bottom, 0, sizeof(bottom));

        for (k = 0; k < n; k++) {
            c = (int32_t)((it[k].x - sp->margin_left) / colw);
            c = c < 0 ? 0 : c >= nc ? nc - 1 : c;

            if (it[k].region == 0 && it[k].y > bottom[c]) {
                bottom[c] = it[k].y;
            }
        }

        for (c = 0; c < nc && bottom[c] > 0; c++) {
            double gap = (double)(sp->page_height - sp->margin_bottom - bottom[c]) / 65536.0;

            if (pg + 1 < np || (c + 1 < nc && bottom[c + 1] > 0)) {
                s += gap * gap;
            }
        }

        free(it);
    }

    return s;
}

static void test_optimal_pages(void) {
    pd_block_id sec;
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info gi, oi;
    pd_section_props sp;
    pd_para_props pp;
    double gs, os;
    int32_t i;
    unsigned seed = 7;

    for (i = 0; i < 40; i++) {  /* unbreakable paragraphs of 12-20 lines leave gaps at column bottoms */
        pd_block_id p;
        uint32_t len = (uint32_t)strlen(frog), cut;

        seed = seed * 1103515245u + 12345u;
        cut = 100 + (seed >> 8) % (len - 100);

        while (cut < len && frog[cut] != ' ') {
            cut++;
        }

        pd_doc_insert_block(d, sec, -1, PD_BLOCK_PARAGRAPH, &p);
        pd_doc_insert_text(d, at(p, 0), frog, strlen(frog), PD_FORMAT_INHERIT, NULL);
        pd_doc_insert_text(d, at(p, (uint32_t)strlen(frog)), frog, cut, PD_FORMAT_INHERIT, NULL);
        memset(&pp, 0, sizeof(pp));
        pp.mask = PD_PP_KEEP_LINES;
        pp.keep_lines = 1;
        pd_doc_set_para_props(d, p, &pp);
    }

    pd_doc_section_props(d, sec, &sp);
    sp.page_height = PD_PT(500);
    sp.columns = 2;
    pd_doc_set_section_props(d, sec, &sp);
    pd_layout_new(d, &L);
    pd_layout_update(L, &gi);
    gs = page_slack(L, &sp);

    sp.page_breaking = PD_PAGES_OPTIMAL;
    pd_doc_set_section_props(d, sec, &sp);
    CHECK(pd_layout_update(L, &oi) == PD_OK);
    os = page_slack(L, &sp);
    CHECK(oi.overfull == 0 && oi.pages <= gi.pages);
    CHECK(os < gs && oi.variants > 0);
    printf("  greedy: %d pages, slack %.0f; optimal: %d pages, slack %.0f, %d paragraph variants\n", gi.pages, gs,
           oi.pages, os, oi.variants);

    {
        /* deterministic, and a fresh layout agrees */
        pd_layout* L2;

        pd_layout_new(d, &L2);
        pd_layout_update(L2, NULL);
        CHECK(layout_hash(L) == layout_hash(L2));
        pd_layout_free(L2);
    }

    /* the caret finds lines of variant layouts */
    for (i = 0; i < 40; i++) {
        CHECK(page_of(L, pd_doc_child(d, sec, i + 1), 0, NULL) >= 0);
    }

    pd_layout_free(L);
    pd_doc_free(d);
}

/* An underline runs through the spaces that are underlined themselves and
   stops at those that are not: the space between words is glue, not a glyph. */
static void test_underline_spaces(void) {
    pd_block_id sec, p;
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info info;
    pd_char_props cp;
    pd_range r;
    pd_draw* it;
    int32_t n, k;
    pd_sp gx[32];
    int bridged = 0, gap_ruled = 0;

    p = pd_doc_child(d, sec, 0);
    pd_doc_insert_text(d, at(p, 0), "one two three four", 18, PD_FORMAT_INHERIT, NULL);
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_UNDERLINE;
    cp.underline = 1;
    r.start = at(p, 0);
    r.end = at(p, 7);              /* "one two": the space between them too */
    pd_doc_set_char_props(d, r, &cp);
    r.start = at(p, 8);
    r.end = at(p, 13);             /* "three", not the space before it */
    pd_doc_set_char_props(d, r, &cp);

    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, &info) == PD_OK);
    it = items(L, 0, &n);

    for (k = 0; k < 32; k++) {
        gx[k] = -1;
    }

    for (k = 0; k < n; k++) {
        if (it[k].kind == PD_DRAW_GLYPH && it[k].offset < 32) {
            gx[it[k].offset] = it[k].x;
        }
    }

    for (k = 0; k < n; k++) {
        if (it[k].kind == PD_DRAW_RULE) {
            /* through the space after "one", up to the "t" of "two" */
            bridged |= it[k].x <= gx[2] && it[k].x + it[k].w >= gx[4];
            /* anything through the middle of the gap between "two" and "three" */
            gap_ruled |= it[k].x < (gx[6] + gx[8]) / 2 + PD_PT(1) && it[k].x + it[k].w > gx[8] - PD_PT(1) &&
                         it[k].x + it[k].w < gx[8] + PD_PT(30) && it[k].x <= gx[6];
        }
    }

    CHECK(gx[0] >= 0 && gx[4] > gx[2] && gx[8] > gx[6]);
    CHECK(bridged);
    CHECK(!gap_ruled);
    free(it);
    pd_layout_free(L);
    pd_doc_free(d);
}

/* A cell merged down two rows below it: its text does not make its own row
   tall, the last row it covers grows to hold it, no rule crosses it, and
   the text after the table starts below it. */
static void test_merged_cells(void) {
    pd_block_id sec, t, after;
    pd_doc* d = new_doc(&sec);
    pd_layout* L;
    pd_layout_info info;
    pd_cell_props cp;
    pd_draw* it;
    int32_t r, n, k, pg, hrules = 0;
    pd_sp y0, y1, y2, ylong, yafter, x0, x1, xe;
    const char* tx;
    uint32_t len;

    t = add_table(d, sec, 3, 2, 0, cell_text);
    pd_doc_delete(d, (pd_range) {
        at(cell_para(d, t, 0, 0), 0), at(cell_para(d, t, 0, 0), 4)
    }, NULL);
    pd_doc_insert_text(d, at(cell_para(d, t, 0, 0), 0), frog, strlen(frog), PD_FORMAT_INHERIT, NULL);

    for (r = 1; r < 3; r++) {
        pd_block_id cell = pd_doc_child(d, pd_doc_child(d, t, r), 0);

        pd_doc_cell_props(d, cell, &cp);
        cp.merge_up = 1;
        CHECK(pd_doc_set_cell_props(d, cell, &cp) == PD_OK);
    }

    {
        pd_table_props tp;

        pd_doc_table_props(d, t, &tp);     /* narrow, so the merged text needs more than its rows */
        tp.ncols = 2;
        tp.col_width[0] = PD_PT(120);
        tp.col_width[1] = PD_PT(120);
        pd_doc_set_table_props(d, t, &tp);
    }

    after = add_para(d, sec, "After the table.");
    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, &info) == PD_OK);

    page_of(L, cell_para(d, t, 0, 1), 0, &y0);
    page_of(L, cell_para(d, t, 1, 1), 0, &y1);
    page_of(L, cell_para(d, t, 2, 1), 0, &y2);
    pd_doc_para_text(d, cell_para(d, t, 0, 0), &tx, &len);
    page_of(L, cell_para(d, t, 0, 0), len, &ylong);
    page_of(L, after, 0, &yafter);
    CHECK(y1 - y0 < PD_PT(30) && y2 - y1 < PD_PT(30));     /* the rows stay one line high */
    printf("  rows at %.1f %.1f %.1f, merged text ends %.1f, after %.1f pt\n", y0 / 65536.0, y1 / 65536.0, y2 / 65536.0, ylong / 65536.0, yafter / 65536.0);
    CHECK(ylong > y2 + PD_PT(20));                         /* its text runs past the last row's first line */
    CHECK(yafter > ylong);                                 /* and the table ends below it */

    pd_layout_caret(L, at(cell_para(d, t, 0, 0), 0), &pg, &x0, &y0, NULL, NULL);
    pd_layout_caret(L, at(cell_para(d, t, 0, 1), 0), &pg, &x1, &y0, NULL, NULL);
    it = items(L, 0, &n);

    for (k = 0; k < n; k++) {
        if (it[k].kind == PD_DRAW_RULE && it[k].w > it[k].h && it[k].x < x1 - PD_PT(6) &&
                it[k].x + it[k].w > x0 + PD_PT(1)) {
            hrules++;   /* across the merged cell's column: only its top and bottom */
        }
    }

    CHECK(hrules == 2);
    free(it);
    (void)xe;
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
    printf("font fallback\n");
    test_fallback();
    test_hyphenation();
    printf("footnotes\n");
    test_footnotes();
    printf("tables\n");
    test_tables();
    printf("wrap beside floats\n");
    test_wrap();
    printf("continuous sections\n");
    test_continuous();
    printf("optimal page breaking\n");
    test_optimal_pages();
    printf("underlines through spaces\n");
    test_underline_spaces();
    printf("cells merged across rows\n");
    test_merged_cells();
    pd_font_free(font);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
