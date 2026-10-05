/*
 * Parade document model tests: operations, undo/redo, markers, lists,
 * styles, JData/BJData round trips, loader fuzzing and the layout bridge.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_doc.h"

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef struct {
    unsigned char* p;
    size_t n;
} buf_t;

static int buf_write(void* user, const void* data, size_t len) {
    buf_t* b = (buf_t*)user;
    unsigned char* q = (unsigned char*)realloc(b->p, b->n + len + 1);

    if (!q) {
        return 1;
    }

    b->p = q;
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = '\0';
    return 0;
}

static buf_t save(const pd_doc* d, pd_jdata_format f) {
    buf_t b = { NULL, 0 };
    CHECK(pd_doc_save(d, f, buf_write, &b) == PD_OK);
    return b;
}

static int same(buf_t a, buf_t b) {
    return a.n == b.n && memcmp(a.p, b.p, a.n) == 0;
}

static int contains(buf_t a, const char* pat, size_t n) {
    size_t i;

    for (i = 0; i + n <= a.n; i++) {
        if (memcmp(a.p + i, pat, n) == 0) {
            return 1;
        }
    }

    return 0;
}

/* the first paragraph in reading order under a block (depth first) */
static pd_block_id first_para_under(const pd_doc* d, pd_block_id b) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(d, b, &bi) != PD_OK) {
        return 0;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        return b;
    }

    for (i = 0; i < bi.child_count; i++) {
        pd_block_id r = first_para_under(d, pd_doc_child(d, b, i));

        if (r) {
            return r;
        }
    }

    return 0;
}

static pd_block_id first_para(const pd_doc* d) {
    return first_para_under(d, pd_doc_root(d));
}

static pd_pos at(pd_block_id b, uint32_t off) {
    pd_pos p;
    p.block = b;
    p.offset = off;
    return p;
}

static pd_range rng(pd_block_id b1, uint32_t o1, pd_block_id b2, uint32_t o2) {
    pd_range r;
    r.start = at(b1, o1);
    r.end = at(b2, o2);
    return r;
}

static int text_is(const pd_doc* d, pd_block_id b, const char* s) {
    const char* t;
    uint32_t n;

    return pd_doc_para_text(d, b, &t, &n) == PD_OK && n == strlen(s) && memcmp(t, s, n) == 0;
}

/* runs cover the text exactly, every paragraph in the main flow */
static int invariants(const pd_doc* d) {
    pd_block_id b;
    int ok = 1;

    for (b = first_para(d); b; b = pd_doc_next_paragraph(d, b)) {
        pd_run runs[4096];
        int32_t n, i;
        uint32_t pos = 0;
        const char* t;
        uint32_t len;

        pd_doc_para_text(d, b, &t, &len);

        if (pd_doc_para_runs(d, b, runs, 4096, &n) != PD_OK) {
            return 0;
        }

        for (i = 0; i < n; i++) {
            ok &= runs[i].start == pos && runs[i].end > runs[i].start;
            ok &= i == 0 || runs[i].format != runs[i - 1].format;     /* coalesced */
            pos = runs[i].end;
        }

        ok &= pos == len;
    }

    return ok;
}

static void test_basics(void) {
    pd_doc* d;
    pd_block_info bi;
    pd_block_id p;
    pd_pos c;
    uint64_t rev;

    CHECK(pd_doc_new(&d) == PD_OK);
    p = first_para(d);
    CHECK(pd_doc_block_info(d, p, &bi) == PD_OK && bi.kind == PD_BLOCK_PARAGRAPH && bi.text_length == 0);
    CHECK(pd_doc_style_find(d, "Normal") == bi.style && bi.style != 0);
    CHECK(pd_doc_style_find(d, "Heading 3") != 0 && pd_doc_style_count(d) >= 12);

    rev = pd_doc_revision(d);
    CHECK(pd_doc_insert_text(d, at(p, 0), "Hello", 5, PD_FORMAT_INHERIT, &c) == PD_OK);
    CHECK(c.offset == 5 && pd_doc_revision(d) == rev + 1);
    CHECK(pd_doc_insert_text(d, c, " world", 6, PD_FORMAT_INHERIT, &c) == PD_OK);
    CHECK(pd_doc_insert_text(d, c, "!", 1, PD_FORMAT_INHERIT, &c) == PD_OK);
    CHECK(text_is(d, p, "Hello world!"));
    CHECK(pd_doc_block_info(d, p, &bi) == PD_OK && bi.revision == pd_doc_revision(d));

    /* consecutive typing is one undo step */
    CHECK(pd_doc_can_undo(d) && strcmp(pd_doc_undo_label(d), "Typing") == 0);
    CHECK(pd_doc_undo(d) == PD_OK);
    CHECK(text_is(d, p, "") && !pd_doc_can_undo(d) && pd_doc_can_redo(d));
    CHECK(pd_doc_redo(d) == PD_OK && text_is(d, p, "Hello world!"));

    /* a caret jump starts a new step */
    CHECK(pd_doc_insert_text(d, at(p, 0), ">", 1, PD_FORMAT_INHERIT, NULL) == PD_OK);
    CHECK(pd_doc_undo(d) == PD_OK && text_is(d, p, "Hello world!"));

    /* rejected input */
    CHECK(pd_doc_insert_text(d, at(p, 0), "\xFF", 1, PD_FORMAT_INHERIT, NULL) == PD_ERR_ARG);
    CHECK(pd_doc_insert_text(d, at(p, 0), "\xEF\xBF\xBC", 3, PD_FORMAT_INHERIT, NULL) == PD_ERR_ARG);
    CHECK(pd_doc_insert_text(d, at(p, 99), "x", 1, PD_FORMAT_INHERIT, NULL) == PD_ERR_RANGE);
    CHECK(pd_doc_insert_text(d, at(p, 0), "x", 1, 12345, NULL) == PD_ERR_ARG);
    CHECK(pd_doc_insert_text(d, at(999, 0), "x", 1, PD_FORMAT_INHERIT, NULL) == PD_ERR_RANGE);
    {
        pd_block_id q = p;
        pd_pos e;

        CHECK(pd_doc_insert_text(d, at(q, 12), " \xE5\xA4\xA9", 4, PD_FORMAT_INHERIT, &e) == PD_OK);
        CHECK(pd_doc_insert_text(d, at(q, 14), "x", 1, PD_FORMAT_INHERIT, NULL) == PD_ERR_RANGE);   /* mid-char */
    }
    CHECK(invariants(d));
    pd_doc_free(d);
}

static void test_format_split_merge(void) {
    pd_doc* d;
    pd_block_id p, q;
    pd_char_props bold;
    pd_run runs[16];
    int32_t n;
    pd_char_props cp;
    pd_pos c;
    pd_marker_id m1, m2, m3;
    pd_pos mp;
    buf_t before, after;

    pd_doc_new(&d);
    p = first_para(d);
    pd_doc_insert_text(d, at(p, 0), "The quick brown fox", 19, PD_FORMAT_INHERIT, NULL);

    memset(&bold, 0, sizeof(bold));
    bold.mask = PD_CP_WEIGHT;
    bold.weight = 700;
    CHECK(pd_doc_set_char_props(d, rng(p, 4, p, 9), &bold) == PD_OK);
    CHECK(pd_doc_para_runs(d, p, runs, 16, &n) == PD_OK && n == 3);
    CHECK(runs[1].start == 4 && runs[1].end == 9);
    CHECK(pd_doc_format_resolve(d, p, runs[1].format, &cp) == PD_OK && cp.weight == 700);
    CHECK(pd_doc_format_resolve(d, p, runs[0].format, &cp) == PD_OK && cp.weight == 400);

    /* typing at the end of bold continues bold */
    CHECK(pd_doc_insert_text(d, at(p, 9), "er", 2, PD_FORMAT_INHERIT, NULL) == PD_OK);
    CHECK(pd_doc_para_runs(d, p, runs, 16, &n) == PD_OK && n == 3 && runs[1].end == 11);
    CHECK(pd_doc_clear_char_props(d, rng(p, 0, p, 21), PD_CP_WEIGHT) == PD_OK);
    CHECK(pd_doc_para_runs(d, p, runs, 16, &n) == PD_OK && n == 1);
    CHECK(pd_doc_undo(d) == PD_OK && pd_doc_para_runs(d, p, runs, 16, &n) == PD_OK && n == 3);

    /* markers follow edits; split moves the tail with its markers */
    pd_doc_marker_new(d, at(p, 4), PD_GRAVITY_LEFT, &m1);
    pd_doc_marker_new(d, at(p, 16), PD_GRAVITY_RIGHT, &m2);
    pd_doc_marker_new(d, at(p, 10), PD_GRAVITY_RIGHT, &m3);
    before = save(d, PD_JDATA_TEXT);
    CHECK(pd_doc_split(d, at(p, 10), &c) == PD_OK);
    q = c.block;
    CHECK(q != p && c.offset == 0);
    CHECK(text_is(d, p, "The quicke") && text_is(d, q, "r brown fox"));
    CHECK(pd_doc_marker_get(d, m1, &mp) == PD_OK && mp.block == p && mp.offset == 4);
    CHECK(pd_doc_marker_get(d, m2, &mp) == PD_OK && mp.block == q && mp.offset == 6);
    CHECK(pd_doc_marker_get(d, m3, &mp) == PD_OK && mp.block == q && mp.offset == 0);
    CHECK(pd_doc_next_paragraph(d, p) == q && pd_doc_prev_paragraph(d, q) == p);
    CHECK(pd_doc_para_runs(d, q, runs, 16, &n) == PD_OK && n == 2);  /* "r" stays bold */

    /* delete across the break merges back */
    CHECK(pd_doc_delete(d, rng(p, 10, q, 0), &c) == PD_OK);
    CHECK(text_is(d, p, "The quicker brown fox") && c.block == p && c.offset == 10);
    CHECK(pd_doc_marker_get(d, m2, &mp) == PD_OK && mp.block == p && mp.offset == 16);
    after = save(d, PD_JDATA_TEXT);
    CHECK(pd_doc_undo(d) == PD_OK && pd_doc_undo(d) == PD_OK);
    {
        buf_t again = save(d, PD_JDATA_TEXT);
        CHECK(same(before, again));
        free(again.p);
    }
    CHECK(pd_doc_redo(d) == PD_OK && pd_doc_redo(d) == PD_OK);
    {
        buf_t again = save(d, PD_JDATA_TEXT);
        CHECK(same(after, again));
        free(again.p);
    }
    free(before.p);
    free(after.p);
    CHECK(invariants(d));
    pd_doc_free(d);
}

static void test_structure(void) {
    pd_doc* d;
    pd_block_id sec, p, fl, cap, img, story, brk;
    pd_block_info bi;
    pd_inline im;
    pd_res_id res;
    pd_float_props fp;
    pd_section_props sp;
    pd_pos c;
    pd_inline got;

    pd_doc_new(&d);
    sec = pd_doc_child(d, pd_doc_root(d), 0);
    p = first_para(d);
    pd_doc_insert_text(d, at(p, 0), "Body text.", 10, PD_FORMAT_INHERIT, NULL);

    pd_doc_begin_group(d, "Insert figure");
    CHECK(pd_doc_insert_block(d, sec, 1, PD_BLOCK_FLOAT, &fl) == PD_OK);
    CHECK(pd_doc_block_info(d, fl, &bi) == PD_OK && bi.child_count == 1 && bi.index == 1);
    img = pd_doc_child(d, fl, 0);
    CHECK(pd_doc_block_info(d, img, &bi) == PD_OK && bi.role == PD_ROLE_FIGURE_CONTENT);
    CHECK(pd_doc_add_resource(d, "image/png", "\x89PNG\r\n", 6, &res) == PD_OK);
    memset(&im, 0, sizeof(im));
    im.kind = PD_INLINE_IMAGE;
    im.resource = res;
    im.width = PD_PT(200);
    im.height = PD_PT(100);
    CHECK(pd_doc_insert_inline(d, at(img, 0), &im, &c) == PD_OK && c.offset == 3);
    CHECK(pd_doc_insert_block(d, fl, -1, PD_BLOCK_PARAGRAPH, &cap) == PD_OK);
    CHECK(pd_doc_set_role(d, cap, PD_ROLE_CAPTION, 0) == PD_OK);
    CHECK(pd_doc_set_para_style(d, cap, pd_doc_style_find(d, "Caption")) == PD_OK);
    CHECK(pd_doc_insert_text(d, at(cap, 0), "A figure.", 9, PD_FORMAT_INHERIT, NULL) == PD_OK);
    memset(&fp, 0, sizeof(fp));
    fp.placement = PD_PLACE_TOP | PD_PLACE_BOTTOM;
    fp.width_fraction = 800;
    strcpy(fp.sequence, "Figure");
    CHECK(pd_doc_set_float_props(d, fl, &fp) == PD_OK);
    pd_doc_end_group(d);

    CHECK(strcmp(pd_doc_undo_label(d), "Insert figure") == 0);
    CHECK(pd_doc_inline_at(d, at(img, 0), &got) == PD_OK && got.kind == PD_INLINE_IMAGE && got.width == PD_PT(200));
    CHECK(pd_doc_next_paragraph(d, p) == img && pd_doc_next_paragraph(d, img) == cap);
    CHECK(pd_doc_undo(d) == PD_OK);
    CHECK(pd_doc_block_info(d, fl, &bi) == PD_ERR_ARG);     /* gone with everything inside */
    CHECK(pd_doc_next_paragraph(d, p) == 0);
    CHECK(pd_doc_redo(d) == PD_OK && pd_doc_block_info(d, fl, &bi) == PD_OK);
    CHECK(pd_doc_float_props(d, fl, &fp) == PD_OK && fp.width_fraction == 800);
    CHECK(text_is(d, cap, "A figure."));

    /* containers keep a child; moves and removes undo */
    CHECK(pd_doc_remove_block(d, img) == PD_OK);
    CHECK(pd_doc_remove_block(d, cap) == PD_ERR_STATE);
    CHECK(pd_doc_undo(d) == PD_OK && pd_doc_child(d, fl, 0) == img);
    CHECK(pd_doc_move_block(d, fl, sec, 0) == PD_OK && pd_doc_child(d, sec, 0) == fl);
    CHECK(pd_doc_move_block(d, sec, fl, 0) == PD_ERR_ARG);   /* into itself */
    CHECK(pd_doc_undo(d) == PD_OK && pd_doc_child(d, sec, 1) == fl);

    /* a deletion over an inline object removes it; undo restores it */
    CHECK(pd_doc_delete(d, rng(img, 0, img, 3), NULL) == PD_OK && pd_doc_inline_at(d, at(img, 0), &got) != PD_OK);
    CHECK(pd_doc_undo(d) == PD_OK && pd_doc_inline_at(d, at(img, 0), &got) == PD_OK);

    /* stories as headers; a referenced story cannot be removed */
    CHECK(pd_doc_insert_block(d, 0, -1, PD_BLOCK_STORY, &story) == PD_OK);
    CHECK(pd_doc_story_count(d) == 1 && pd_doc_story_at(d, 0) == story);
    CHECK(pd_doc_insert_block(d, sec, -1, PD_BLOCK_PARAGRAPH, NULL) == PD_OK);
    CHECK(pd_doc_insert_block(d, sec, 0, PD_BLOCK_STORY, NULL) == PD_ERR_ARG);
    CHECK(pd_doc_section_props(d, sec, &sp) == PD_OK);
    sp.header = story;
    CHECK(pd_doc_set_section_props(d, sec, &sp) == PD_OK);
    CHECK(pd_doc_remove_block(d, story) == PD_ERR_STATE);
    sp.margin_left = sp.page_width;
    CHECK(pd_doc_set_section_props(d, sec, &sp) == PD_ERR_ARG);
    CHECK(pd_doc_insert_text(d, at(pd_doc_child(d, story, 0), 0), "Running head", 12, PD_FORMAT_INHERIT, NULL) == PD_OK);

    CHECK(pd_doc_insert_block(d, sec, 1, PD_BLOCK_BREAK, &brk) == PD_OK);
    CHECK(pd_doc_set_break(d, brk, PD_BREAK_ODD_PAGE) == PD_OK);
    CHECK(pd_doc_insert_block(d, sec, -1, PD_BLOCK_TABLE, NULL) == PD_OK);
    CHECK(invariants(d));
    pd_doc_free(d);
}

static void test_styles_lists(void) {
    pd_doc* d;
    pd_style_id h1, mine, other;
    pd_para_props pp;
    pd_char_props cp;
    pd_list_level lv[2];
    pd_list_id list, bullets;
    pd_block_id p[6];
    pd_pos c;
    char label[32];
    int i;

    pd_doc_new(&d);
    h1 = pd_doc_style_find(d, "Heading 1");
    CHECK(pd_doc_style_resolve(d, h1, &pp, &cp) == PD_OK && cp.weight == 700 && cp.size == PD_PT(20));
    CHECK(pp.keep_with_next == 1 && pp.next_style == pd_doc_style_find(d, "Normal"));

    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_SIZE | PD_CP_COLOR;
    cp.size = PD_PT(30);
    cp.color = 0xFF2040A0u;
    CHECK(pd_doc_style_define(d, "Heading 1", PD_STYLE_PARAGRAPH, pd_doc_style_find(d, "Normal"), NULL, &cp, NULL) ==
          PD_OK);
    CHECK(pd_doc_style_resolve(d, h1, &pp, &cp) == PD_OK && cp.size == PD_PT(30) && cp.weight == 400);
    CHECK(pd_doc_undo(d) == PD_OK);
    CHECK(pd_doc_style_resolve(d, h1, &pp, &cp) == PD_OK && cp.size == PD_PT(20) && cp.weight == 700);

    CHECK(pd_doc_style_define(d, "Mine", PD_STYLE_PARAGRAPH, h1, NULL, NULL, &mine) == PD_OK);
    CHECK(pd_doc_style_define(d, "Other", PD_STYLE_PARAGRAPH, mine, NULL, NULL, &other) == PD_OK);
    CHECK(pd_doc_style_define(d, "Mine", PD_STYLE_PARAGRAPH, other, NULL, NULL, NULL) == PD_ERR_ARG);   /* cycle */
    CHECK(pd_doc_style_define(d, "Mine", PD_STYLE_CHARACTER, 0, NULL, NULL, NULL) == PD_ERR_ARG);       /* kind */
    CHECK(pd_doc_undo(d) == PD_OK && pd_doc_style_find(d, "Other") == 0);
    CHECK(strcmp(pd_doc_style_name(d, mine), "Mine") == 0);

    /* Enter at the end of a heading gives Normal */
    p[0] = first_para(d);
    pd_doc_set_para_style(d, p[0], h1);
    pd_doc_set_role(d, p[0], PD_ROLE_HEADING, 1);
    pd_doc_insert_text(d, at(p[0], 0), "Title", 5, PD_FORMAT_INHERIT, &c);
    CHECK(pd_doc_split(d, c, &c) == PD_OK);
    {
        pd_block_info bi;

        CHECK(pd_doc_block_info(d, c.block, &bi) == PD_OK && bi.style == pd_doc_style_find(d, "Normal") &&
              bi.role == PD_ROLE_BODY);
    }

    memset(lv, 0, sizeof(lv));
    lv[0].format = PD_NUM_DECIMAL;
    lv[0].start = 1;
    strcpy(lv[0].text, "%1.");
    lv[1].format = PD_NUM_LOWER_ALPHA;
    lv[1].start = 1;
    strcpy(lv[1].text, "%1.%2)");
    CHECK(pd_doc_list_define(d, 2, lv, &list) == PD_OK);
    lv[0].format = PD_NUM_BULLET;
    strcpy(lv[0].text, "\xE2\x80\xA2");
    CHECK(pd_doc_list_define(d, 1, lv, &bullets) == PD_OK);

    p[1] = c.block;

    for (i = 2; i < 6; i++) {
        CHECK(pd_doc_split(d, c, &c) == PD_OK);
        p[i] = c.block;
    }

    pd_doc_set_list(d, p[1], list, 0);
    pd_doc_set_list(d, p[2], list, 1);
    pd_doc_set_list(d, p[3], list, 1);
    pd_doc_set_list(d, p[4], list, 0);
    pd_doc_set_list(d, p[5], bullets, 0);
    CHECK(pd_doc_set_list(d, p[5], list, 5) == PD_ERR_ARG);
    pd_doc_list_label(d, p[1], label, sizeof(label));
    CHECK(strcmp(label, "1.") == 0);
    pd_doc_list_label(d, p[3], label, sizeof(label));
    CHECK(strcmp(label, "1.b)") == 0);
    pd_doc_list_label(d, p[4], label, sizeof(label));
    CHECK(strcmp(label, "2.") == 0);
    pd_doc_list_label(d, p[5], label, sizeof(label));
    CHECK(strcmp(label, "\xE2\x80\xA2") == 0);
    pd_doc_list_label(d, p[0], label, sizeof(label));
    CHECK(label[0] == '\0');
    pd_doc_free(d);
}

typedef struct {
    int n;
    int structure;
} listen_t;

static void on_change(void* user, const pd_change* ch) {
    listen_t* l = (listen_t*)user;

    l->n++;
    l->structure += ch->kind == PD_CHANGE_STRUCTURE;
}

static void test_groups_limits_listener(void) {
    pd_doc* d;
    pd_block_id p;
    pd_pos c;
    listen_t l = { 0, 0 };
    int i;

    pd_doc_new(&d);
    p = first_para(d);
    pd_doc_set_listener(d, on_change, &l);
    pd_doc_begin_group(d, "Paste");
    pd_doc_insert_text(d, at(p, 0), "one", 3, PD_FORMAT_INHERIT, &c);
    pd_doc_begin_group(d, "inner");
    pd_doc_split(d, c, &c);
    pd_doc_end_group(d);
    pd_doc_insert_text(d, c, "two", 3, PD_FORMAT_INHERIT, &c);
    CHECK(!pd_doc_can_undo(d));     /* not while a group is open */
    pd_doc_end_group(d);
    CHECK(strcmp(pd_doc_undo_label(d), "Paste") == 0);
    CHECK(l.n >= 3 && l.structure >= 1);
    CHECK(pd_doc_undo(d) == PD_OK && text_is(d, p, "") && pd_doc_next_paragraph(d, p) == 0 && !pd_doc_can_undo(d));

    /* an empty group leaves nothing behind and keeps redo */
    pd_doc_begin_group(d, "nothing");
    pd_doc_end_group(d);
    CHECK(pd_doc_can_redo(d));

    pd_doc_set_undo_limit(d, 5);

    for (i = 0; i < 20; i++) {
        pd_doc_insert_text(d, at(p, 0), "x", 1, PD_FORMAT_INHERIT, NULL);   /* caret jumps: separate steps */
    }

    CHECK(!pd_doc_can_redo(d));

    for (i = 0; i < 5; i++) {
        CHECK(pd_doc_undo(d) == PD_OK);
    }

    CHECK(!pd_doc_can_undo(d));
    pd_doc_free(d);
}

/* ------------------------------------------------------------------ */
/* random operations: undo all returns to the start, redo all to the end */
/* ------------------------------------------------------------------ */

static uint32_t rnd_state = 12345;

static uint32_t rnd(uint32_t n) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return n ? (rnd_state >> 8) % n : 0;
}

static pd_block_id random_para(const pd_doc* d) {
    pd_block_id b, list[4096];
    int n = 0;

    for (b = first_para(d); b && n < 4096; b = pd_doc_next_paragraph(d, b)) {
        list[n++] = b;
    }

    return list[rnd((uint32_t)n)];
}

static uint32_t random_offset(const pd_doc* d, pd_block_id b) {
    const char* t;
    uint32_t n, o;

    pd_doc_para_text(d, b, &t, &n);
    o = rnd(n + 1);

    while (o > 0 && o < n && (t[o] & 0xC0) == 0x80) {
        o--;
    }

    return o;
}

static void random_op(pd_doc* d, pd_res_id res) {
    static const char* words[] = { "a", "fox ", "\xE5\xA4\xA9\xE5\x9C\xB0", "\xF0\x9F\x98\x80", " jumps", "\t", "\n" };
    pd_block_id b = random_para(d), b2;
    pd_block_info bi;
    uint32_t o = random_offset(d, b), o2;
    pd_char_props cp;
    pd_inline im;

    switch (rnd(12)) {
        case 0:
        case 1:
        case 2: {
            const char* w = words[rnd(7)];

            pd_doc_insert_text(d, at(b, o), w, strlen(w), PD_FORMAT_INHERIT, NULL);
            break;
        }

        case 3:
            pd_doc_split(d, at(b, o), NULL);
            break;

        case 4:     /* delete within or across siblings */
            b2 = rnd(3) ? b : pd_doc_next_paragraph(d, b);

            if (!b2) {
                b2 = b;
            }

            pd_doc_block_info(d, b2, &bi);
            o2 = random_offset(d, b2);

            if (b2 == b && o2 < o) {
                uint32_t t = o;
                o = o2;
                o2 = t;
            }

            pd_doc_delete(d, rng(b, o, b2, o2), NULL);  /* may be refused across parents */
            break;

        case 5:
            memset(&cp, 0, sizeof(cp));
            cp.mask = rnd(2) ? PD_CP_WEIGHT : PD_CP_ITALIC | PD_CP_SIZE;
            cp.weight = 700;
            cp.italic = 1;
            cp.size = PD_PT(8 + rnd(8));
            o2 = random_offset(d, b);
            pd_doc_set_char_props(d, rng(b, o < o2 ? o : o2, b, o < o2 ? o2 : o), &cp);
            break;

        case 6:
            memset(&im, 0, sizeof(im));
            im.kind = PD_INLINE_IMAGE;
            im.resource = res;
            im.width = PD_PT(10);
            pd_doc_insert_inline(d, at(b, o), &im, NULL);
            break;

        case 7:
            pd_doc_block_info(d, b, &bi);
            pd_doc_insert_block(d, bi.parent, bi.index + (int32_t)rnd(2), rnd(2) ? PD_BLOCK_FLOAT : PD_BLOCK_PARAGRAPH,
                                NULL);
            break;

        case 8:
            pd_doc_block_info(d, b, &bi);
            pd_doc_remove_block(d, rnd(2) ? b : bi.parent);
            break;

        case 9:
            pd_doc_block_info(d, b, &bi);
            pd_doc_move_block(d, b, bi.parent, (int32_t)rnd(2));
            break;

        case 10:
            pd_doc_set_role(d, b, PD_ROLE_HEADING, 1 + (int32_t)rnd(6));
            break;

        default:
            pd_doc_undo(d);

            if (rnd(2)) {
                pd_doc_redo(d);
            }
    }
}

static void test_random_undo(void) {
    pd_doc* d;
    buf_t start, end, now;
    pd_res_id res;
    int i, steps = 0, bad = 0;

    pd_doc_new(&d);
    pd_doc_add_resource(d, "image/png", "png", 3, &res);
    pd_doc_set_undo_limit(d, 0);
    pd_doc_insert_text(d, at(first_para(d), 0), "Seed text.", 10, PD_FORMAT_INHERIT, NULL);

    while (pd_doc_can_undo(d)) {
        pd_doc_undo(d);
    }

    pd_doc_redo(d);
    start = save(d, PD_JDATA_TEXT);

    for (i = 0; i < 4000; i++) {
        random_op(d, res);
        pd_doc_seal_undo(d);
        bad += !invariants(d);
    }

    CHECK(bad == 0);
    end = save(d, PD_JDATA_TEXT);

    while (pd_doc_can_undo(d) && steps < 100000) {
        CHECK(pd_doc_undo(d) == PD_OK);
        steps++;
    }

    pd_doc_redo(d);     /* the seed step was undone too; replay it */
    now = save(d, PD_JDATA_TEXT);
    CHECK(same(start, now));
    free(now.p);

    while (pd_doc_can_redo(d)) {
        CHECK(pd_doc_redo(d) == PD_OK);
    }

    now = save(d, PD_JDATA_TEXT);
    CHECK(same(end, now));
    printf("  %d random operations, %d undo steps: undo-all and redo-all reproduce the saved states\n", i, steps);
    free(now.p);
    free(start.p);
    free(end.p);
    pd_doc_free(d);
}

/* ------------------------------------------------------------------ */
/* JData / BJData                                                     */
/* ------------------------------------------------------------------ */

static pd_doc* rich_doc(void) {
    pd_doc* d;
    pd_res_id res;
    pd_block_id sec, story, fl, p;
    pd_section_props sp;
    pd_inline o;
    pd_char_props cp;
    pd_list_level lv;
    pd_list_id list;
    pd_pos c;
    int i;

    rnd_state = 777;
    pd_doc_new(&d);
    pd_doc_add_resource(d, "image/png", "\x89PNG\0\x01\x02\xff", 8, &res);
    sec = pd_doc_child(d, pd_doc_root(d), 0);
    p = first_para(d);
    pd_doc_insert_text(d, at(p, 0), "Intro \xE5\xA4\xA9\xE5\x9C\xB0 \xF0\x9F\x98\x80 \"quoted\"\ttab", 31,
                       PD_FORMAT_INHERIT, &c);
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_FAMILY | PD_CP_ITALIC | PD_CP_LANG | PD_CP_SHIFT;
    strcpy(cp.family, "Liberation Serif");
    cp.italic = 1;
    strcpy(cp.lang, "zh-Hans");
    cp.shift = PD_SHIFT_SUPER;
    pd_doc_set_char_props(d, rng(p, 6, p, 12), &cp);
    pd_doc_insert_block(d, 0, -1, PD_BLOCK_STORY, &story);
    pd_doc_insert_text(d, at(pd_doc_child(d, story, 0), 0), "Footnote body", 13, PD_FORMAT_INHERIT, NULL);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FOOTNOTE;
    o.target = story;
    pd_doc_insert_inline(d, c, &o, &c);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_EQUATION;
    o.source = "\\int_0^1 x\\,dx";
    o.source_len = (int32_t)strlen(o.source);
    o.width = PD_PT(40);
    o.height = PD_PT(12);
    o.depth = PD_PT(4);
    pd_doc_insert_inline(d, c, &o, &c);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = PD_FIELD_SEQ;
    strcpy(o.name, "Figure");
    pd_doc_insert_inline(d, c, &o, &c);
    pd_doc_section_props(d, sec, &sp);
    sp.header = story;
    sp.columns = 2;
    pd_doc_set_section_props(d, sec, &sp);
    pd_doc_insert_block(d, sec, -1, PD_BLOCK_FLOAT, &fl);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.resource = res;
    o.width = PD_PT(100);
    pd_doc_insert_inline(d, at(pd_doc_child(d, fl, 0), 0), &o, NULL);
    memset(&lv, 0, sizeof(lv));
    lv.format = PD_NUM_UPPER_ROMAN;
    lv.start = 4;
    strcpy(lv.text, "%1.");
    pd_doc_list_define(d, 1, &lv, &list);
    pd_doc_insert_block(d, sec, -1, PD_BLOCK_PARAGRAPH, &p);
    pd_doc_set_list(d, p, list, 0);
    pd_doc_insert_block(d, sec, -1, PD_BLOCK_BREAK, NULL);
    pd_doc_insert_block(d, sec, -1, PD_BLOCK_TABLE, NULL);
    pd_doc_style_define(d, "Custom", PD_STYLE_PARAGRAPH, pd_doc_style_find(d, "Quote"), NULL, NULL, NULL);
    pd_doc_style_define(d, "Undone", PD_STYLE_PARAGRAPH, 0, NULL, NULL, NULL);
    pd_doc_undo(d);     /* leaves a dead style slot, which must survive a round trip */

    for (i = 0; i < 40; i++) {
        random_op(d, res);
    }

    return d;
}

static void test_jdata(void) {
    pd_doc* d = rich_doc(), *t = NULL, *b = NULL;
    buf_t txt = save(d, PD_JDATA_TEXT), bin = save(d, PD_JDATA_BINARY), txt2, txt3;

    CHECK(pd_doc_load(txt.p, txt.n, PD_JDATA_AUTO, &t) == PD_OK);
    CHECK(pd_doc_load(bin.p, bin.n, PD_JDATA_AUTO, &b) == PD_OK);

    if (t && b) {
        txt2 = save(t, PD_JDATA_TEXT);
        txt3 = save(b, PD_JDATA_TEXT);
        CHECK(same(txt, txt2));     /* text -> doc -> text is lossless */
        CHECK(same(txt, txt3));     /* binary -> doc -> text equals the original text */
        CHECK(invariants(t) && invariants(b));
        /* the loaded document is editable and undoable */
        CHECK(pd_doc_insert_text(t, at(first_para(t), 0), "New ", 4, PD_FORMAT_INHERIT, NULL) == PD_OK);
        CHECK(pd_doc_undo(t) == PD_OK);
        free(txt2.p);
        free(txt3.p);
    }

    printf("  JData text %zu bytes, BJData %zu bytes (%.0f%%)\n", txt.n, bin.n, 100.0 * bin.n / txt.n);
    CHECK(strstr((const char*)txt.p, "\"_ByteStream_\"") && strstr((const char*)txt.p, "\"_TreeNode_(section)\""));
    CHECK(strstr((const char*)txt.p, "iVBORwABAv8=") != NULL);  /* base64 resource in text */
    CHECK(contains(bin, "H" "i\x08" "\x89PNG", 7));     /* raw H byte stream in BJData, no base64 */
    pd_doc_free(t);
    pd_doc_free(b);

    /* explicit format mismatch is refused, not misread */
    CHECK(pd_doc_load(bin.p, bin.n, PD_JDATA_TEXT, &t) == PD_ERR_FORMAT);

    /* fuzz: every truncation and many corruptions are rejected or loaded, never a crash */
    {
        size_t k;
        int f, loaded = 0, rejected = 0;

        for (f = 0; f < 2; f++) {
            buf_t s = f ? bin : txt;
            unsigned char* m = (unsigned char*)malloc(s.n + 1);

            for (k = 0; k < s.n; k += 1 + s.n / 3000) {
                pd_doc* x = NULL;

                if (pd_doc_load(s.p, k, PD_JDATA_AUTO, &x) == PD_OK) {
                    loaded++;
                    pd_doc_free(x);
                } else {
                    rejected++;
                }
            }

            for (k = 0; k < 20000; k++) {
                pd_doc* x = NULL;
                int j, nflip = 1 + (int)rnd(4);

                memcpy(m, s.p, s.n);

                for (j = 0; j < nflip; j++) {
                    m[rnd((uint32_t)s.n)] ^= (unsigned char)(1u << rnd(8));
                }

                if (pd_doc_load(m, s.n, PD_JDATA_AUTO, &x) == PD_OK) {
                    loaded++;
                    CHECK(invariants(x));
                    pd_doc_free(x);
                } else {
                    rejected++;
                }
            }

            free(m);
        }

        printf("  loader fuzz: %d rejected, %d loaded (all loaded documents valid)\n", rejected, loaded);
    }

    free(txt.p);
    free(bin.p);
    pd_doc_free(d);
}

/* ------------------------------------------------------------------ */
/* layout bridge                                                      */
/* ------------------------------------------------------------------ */

static const pd_font* resolver(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)family;
    (void)weight;
    (void)italic;
    return (const pd_font*)user;
}

static void test_layout_bridge(void) {
    const char* path = getenv("PARADE_TEST_FONT") ? getenv("PARADE_TEST_FONT") :
                       "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf";
    pd_font* font = NULL;
    pd_doc* d;
    pd_para* para;
    pd_params prm;
    pd_break_info bi;
    pd_block_id p;
    pd_inline o;
    pd_pos c;
    pd_res_id res;
    pd_char_props big;
    pd_para_props pp;
    uint32_t off;
    int32_t line;
    pd_sp x, y;
    const char* text;
    uint32_t len;

    if (pd_font_load_file(path, 0, &font) != PD_OK) {
        printf("  (no test font, skipped)\n");
        return;
    }

    pd_doc_new(&d);
    p = first_para(d);
    CHECK(pd_para_new(&para) == PD_OK);
    CHECK(pd_doc_para_build(d, p, PD_PT(300), para, &prm) == PD_ERR_STATE);     /* no font yet */
    pd_doc_set_font_resolver(d, resolver, font);
    CHECK(pd_doc_para_build(d, p, PD_PT(300), para, &prm) == PD_OK);           /* empty paragraph */
    CHECK(pd_para_break(para, &prm, &bi) == PD_OK && bi.lines == 1 && bi.height > 0);

    pd_doc_insert_text(d, at(p, 0), "An image ", 9, PD_FORMAT_INHERIT, &c);
    pd_doc_add_resource(d, "image/png", "x", 1, &res);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.resource = res;
    o.width = PD_PT(50);
    o.height = PD_PT(30);
    pd_doc_insert_inline(d, c, &o, &c);
    pd_doc_insert_text(d, c, " sits inside text that runs long enough to wrap onto more lines.", 64,
                       PD_FORMAT_INHERIT, NULL);
    memset(&big, 0, sizeof(big));
    big.mask = PD_CP_SIZE;
    big.size = PD_PT(18);
    pd_doc_set_char_props(d, rng(p, 0, p, 2), &big);
    memset(&pp, 0, sizeof(pp));
    pp.mask = PD_PP_INDENT_LEFT | PD_PP_INDENT_FIRST | PD_PP_ALIGN;
    pp.indent_left = PD_PT(20);
    pp.indent_first = PD_PT(15);
    pp.align = PD_ALIGN_JUSTIFY;
    pd_doc_set_para_props(d, p, &pp);

    CHECK(pd_doc_para_build(d, p, PD_PT(200), para, &prm) == PD_OK);
    CHECK(pd_para_break(para, &prm, &bi) == PD_OK && bi.lines >= 2);
    pd_doc_para_text(d, p, &text, &len);
    CHECK(pd_para_text_length(para) == len);    /* offsets map 1:1 */
    {
        pd_line L0, L1;

        pd_para_get_line(para, 0, &L0);
        pd_para_get_line(para, 1, &L1);
        CHECK(L0.x == PD_PT(35) && L1.x == PD_PT(20));      /* first-line and left indents */
        CHECK(L0.ascent >= PD_PT(30));                      /* the image is on line 0 */
    }

    /* a caret at the image's document offset lands on the image */
    CHECK(pd_para_caret(para, 9, &line, &x, &y) == PD_OK);
    {
        pd_glyph g[256];
        int32_t n, k, found = 0;

        pd_para_get_glyphs(para, line, g, 256, &n);

        for (k = 0; k < n; k++) {
            found |= g[k].kind == PD_OBJECT && g[k].x == x && g[k].cluster == 9;
        }

        CHECK(found);
    }
    CHECK(pd_para_hit_test(para, x + 1, y, &off, &line) == PD_OK && off == 9);

    pd_para_free(para);
    pd_doc_free(d);
    pd_font_free(font);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* progress stays in order with sanitizer reports */
    printf("basics\n");
    test_basics();
    printf("format, split, merge, markers\n");
    test_format_split_merge();
    printf("structure, floats, stories\n");
    test_structure();
    printf("styles, lists\n");
    test_styles_lists();
    printf("groups, undo limit, listener\n");
    test_groups_limits_listener();
    printf("random operations with undo/redo\n");
    test_random_undo();
    printf("JData / BJData\n");
    test_jdata();
    printf("layout bridge\n");
    test_layout_bridge();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
