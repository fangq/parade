/*
 * Parade unit tests
 *
 * Fonts come from PARADE_TEST_FONT (default: Liberation Serif) and
 * PARADE_TEST_CJK_FONT (optional). Tests that need a missing font are
 * skipped, not failed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade.h"

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char* text_en =
    "In olden times when wishing still helped one, there lived a king whose daughters were all "
    "beautiful, but the youngest was so beautiful that the sun itself, which has seen so much, "
    "was astonished whenever it shone in her face. Close by the king's castle lay a great dark "
    "forest, and under an old lime-tree in the forest was a well, and when the day was very warm, "
    "the king's child went out into the forest and sat down by the side of the cool fountain; and "
    "when she was bored she took a golden ball, and threw it up on high and caught it; and this "
    "ball was her favorite plaything.";

static pd_font* load_env_font(const char* env, const char* fallback) {
    const char* path = getenv(env) ? getenv(env) : fallback;
    pd_font* f = NULL;

    if (!path || pd_font_load_file(path, 0, &f) != PD_OK) {
        return NULL;
    }

    return f;
}

static pd_para* make_para(const pd_font* font, const char* txt, pd_sp size) {
    pd_para* p = NULL;
    pd_style st;

    pd_para_new(&p);
    pd_style_init(&st, font, size);
    pd_para_add_text(p, txt, strlen(txt), &st);
    return p;
}

/* FNV-1a over every line and glyph: a layout fingerprint */
static uint64_t layout_hash(const pd_para* p) {
    uint64_t h = 1469598103934665603ULL;
    int32_t i, n, k;

    for (i = 0; i < pd_para_line_count(p); i++) {
        pd_line L;
        pd_glyph g[1024];

        pd_para_get_line(p, i, &L);
        pd_para_get_glyphs(p, i, g, 1024, &n);

        for (k = 0; k < n; k++) {
            int32_t v[4] = { (int32_t)g[k].glyph, (int32_t)g[k].cluster, g[k].x, g[k].y };
            size_t b;

            for (b = 0; b < sizeof(v); b++) {
                h = (h ^ ((const unsigned char*)v)[b]) * 1099511628211ULL;
            }
        }

        h = (h ^ (uint64_t)L.text_start) * 1099511628211ULL;
    }

    return h;
}

static void test_font(const pd_font* f) {
    pd_font_metrics m;
    pd_font* bad = NULL;
    unsigned char junk[4096];
    int i;

    CHECK(pd_font_get_metrics(f, &m) == PD_OK);
    CHECK(m.units_per_em > 0);
    CHECK(m.ascender > 0 && m.descender < 0);
    CHECK(pd_font_glyph_index(f, 'A') != 0);
    CHECK(pd_font_glyph_index(f, 0x10FFFD) == 0);
    CHECK(pd_font_glyph_advance(f, pd_font_glyph_index(f, 'A')) > 0);

    /* malformed input is rejected, never crashes */
    CHECK(pd_font_load_memory("abc", 3, 0, &bad) == PD_ERR_ARG);
    srand(1);

    for (i = 0; i < (int)sizeof(junk); i++) {
        junk[i] = (unsigned char)(rand() & 0xFF);
    }

    junk[0] = 0;
    junk[1] = 1;
    junk[2] = 0;
    junk[3] = 0;
    pd_font_load_memory(junk, sizeof(junk), 0, &bad);
    pd_font_free(bad);
}

/* Liberation Serif 2.x: known design-unit values (also checked by tools/font_oracle.py) */
static void test_liberation(const pd_font* f) {
    pd_font_metrics m;

    pd_font_get_metrics(f, &m);

    if (m.units_per_em != 2048 || pd_font_glyph_advance(f, pd_font_glyph_index(f, 'A')) != 1479) {
        printf("  (not Liberation Serif, skipping known-value checks)\n");
        return;
    }

    CHECK(pd_font_glyph_advance(f, pd_font_glyph_index(f, ' ')) == 512);
    CHECK(m.has_kerning);
    CHECK(pd_font_kerning(f, pd_font_glyph_index(f, 'A'), pd_font_glyph_index(f, 'V')) < 0);
}

static void test_justify(const pd_font* f) {
    pd_para* p = make_para(f, text_en, PD_PT(10));
    pd_params prm;
    pd_break_info info;
    int32_t i, n;

    pd_params_init(&prm);
    prm.width = PD_PT(200);
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    CHECK(info.lines > 5);
    CHECK(info.overfull == 0);
    CHECK(info.underfull == 0);

    /* every justified line except the last ends exactly at the right margin */
    for (i = 0; i < info.lines - 1; i++) {
        pd_line L;
        pd_glyph g[512];

        pd_para_get_line(p, i, &L);
        CHECK(pd_para_get_glyphs(p, i, g, 512, &n) == PD_OK);
        CHECK(n > 0);
        CHECK(g[n - 1].x + g[n - 1].advance == L.x + prm.width);
        CHECK(L.width == prm.width);
    }

    /* lines tile the text */
    {
        pd_line a, b;

        for (i = 0; i + 1 < info.lines; i++) {
            pd_para_get_line(p, i, &a);
            pd_para_get_line(p, i + 1, &b);
            CHECK(a.text_end == b.text_start);
            CHECK(b.baseline > a.baseline);
        }

        pd_para_get_line(p, info.lines - 1, &a);
        CHECK(a.text_end == pd_para_text_length(p));
    }

    /* the size query */
    CHECK(pd_para_get_glyphs(p, 0, NULL, 0, &n) == PD_OK && n > 0);
    pd_para_free(p);
}

static void test_greedy_vs_optimal(const pd_font* f) {
    pd_para* p = make_para(f, text_en, PD_PT(10));
    pd_params prm;
    pd_break_info g, o;

    pd_params_init(&prm);
    prm.width = PD_PT(160);
    prm.mode = PD_BREAK_GREEDY;
    pd_para_break(p, &prm, &g);
    prm.mode = PD_BREAK_OPTIMAL;
    pd_para_break(p, &prm, &o);
    CHECK(o.demerits <= g.demerits);
    printf("  demerits at 160pt: greedy %lld, optimal %lld\n", (long long)g.demerits, (long long)o.demerits);
    pd_para_free(p);
}

/* TeX \looseness: one line more or fewer than the optimum, when feasible */
static void test_looseness(const pd_font* f) {
    pd_para* p = make_para(f, text_en, PD_PT(10));
    pd_params prm;
    pd_break_info o, plus, minus, zero;

    pd_params_init(&prm);
    prm.width = PD_PT(250);
    pd_para_break(p, &prm, &o);
    prm.looseness = 1;
    pd_para_break(p, &prm, &plus);
    CHECK(plus.lines == o.lines + 1 && plus.overfull == 0);
    CHECK(plus.demerits >= o.demerits);
    prm.looseness = -1;
    pd_para_break(p, &prm, &minus);
    CHECK(minus.lines <= o.lines && minus.lines >= o.lines - 1 && minus.overfull == 0);
    prm.looseness = 50;     /* unreachable: as long as it can get */
    pd_para_break(p, &prm, &zero);
    CHECK(zero.lines > plus.lines);
    prm.looseness = 0;
    pd_para_break(p, &prm, &zero);
    CHECK(zero.lines == o.lines && zero.demerits == o.demerits);
    printf("  lines: optimal %d, looser %d, tighter %d\n", o.lines, plus.lines, minus.lines);
    pd_para_free(p);
}

/* right edge of a line's ink: the end of its last glyph */
static pd_sp line_right(const pd_para* p, int32_t li, uint32_t* last_cp) {
    pd_glyph g[1024];
    int32_t n = 0;
    const char* t = text_en;

    pd_para_get_glyphs(p, li, g, 1024, &n);

    if (n == 0) {
        return 0;
    }

    *last_cp = (unsigned char)t[g[n - 1].cluster];
    return g[n - 1].x + g[n - 1].advance;
}

/* microtypography: margin kerning and font expansion */
static void test_microtype(const pd_font* f) {
    pd_para* p = make_para(f, text_en, PD_PT(10));
    pd_params prm;
    pd_break_info plain, prot, exp;
    pd_sp W = PD_PT(160);
    int32_t i, hang = 0, exact = 0, lines, ok_scale = 1, scaled = 0;

    pd_params_init(&prm);
    prm.width = W;
    pd_para_break(p, &prm, &plain);

    prm.protrusion = 1;
    pd_para_break(p, &prm, &prot);
    lines = pd_para_line_count(p);

    for (i = 0; i + 1 < lines; i++) {
        uint32_t cp = 0;
        pd_sp r = line_right(p, i, &cp);

        if (cp == '.' || cp == ',' || cp == ';' || cp == '-') {
            hang += r > W && r < W + PD_PT(4);   /* punctuation hangs out, a little */
        } else if (cp >= 'a' && cp <= 'z') {
            exact += r >= W - 2 && r <= W + 2;    /* letters end at the margin (to rounding) */
        }
    }

    CHECK(hang > 0 && exact > 0);
    printf("  protrusion: %d lines hang punctuation, %d end flush\n", hang, exact);

    prm.protrusion = 0;
    prm.expansion = 20;
    pd_para_break(p, &prm, &exp);
    lines = pd_para_line_count(p);

    for (i = 0; i < lines; i++) {
        pd_glyph g[1024];
        int32_t n = 0, k;
        uint32_t cp;

        pd_para_get_glyphs(p, i, g, 1024, &n);

        for (k = 0; k < n; k++) {
            ok_scale &= g[k].scale >= 65536 * 98 / 100 && g[k].scale <= 65536 * 102 / 100 + 1;
            scaled += g[k].scale != 65536;
        }

        if (i + 1 < lines) {
            pd_sp r = line_right(p, i, &cp);

            ok_scale &= r >= W - 2 && r <= W + 2;     /* still justified exactly */
        }
    }

    CHECK(ok_scale && scaled > 0);
    CHECK(exp.demerits <= plain.demerits);
    printf("  expansion: demerits %lld -> %lld, %d glyphs scaled\n", (long long)plain.demerits,
           (long long)exp.demerits, scaled);
    pd_para_free(p);
}

static int math_box(const pd_font* mf, const char* tex, int display, pd_math_metrics* m, int32_t* nrules) {
    pd_math_item it[2048];
    int32_t n = 0, k;

    if (pd_math_layout(mf, PD_PT(10), tex, strlen(tex), display, it, 2048, &n, m) != PD_OK) {
        return -1;
    }

    if (nrules) {
        *nrules = 0;

        for (k = 0; k < n; k++) {
            *nrules += it[k].kind == 1;
        }
    }

    return n;
}

static void test_math(const pd_font* mf) {
    pd_math_metrics x, x2, fr, sumd, sumt, sq, del, a, b;
    int32_t rules = 0, n, k;
    unsigned seed = 99;

    CHECK(pd_font_has_math(mf));
    CHECK(math_box(mf, "x", 0, &x, NULL) == 1);
    CHECK(math_box(mf, "x^2", 0, &x2, NULL) == 2 && x2.width > x.width && x2.height > x.height);
    CHECK(math_box(mf, "\\frac{a}{b}", 0, &fr, &rules) == 3 && rules == 1 && fr.height > x.height && fr.depth > PD_PT(2));
    CHECK(math_box(mf, "\\sum_{i=1}^n i", 1, &sumd, NULL) > 0 && math_box(mf, "\\sum_{i=1}^n i", 0, &sumt, NULL) > 0);
    CHECK(sumd.height > sumt.height && sumd.depth > sumt.depth);    /* display: bigger, limits stacked */
    CHECK(math_box(mf, "\\sqrt{x}", 0, &sq, &rules) >= 2 && rules == 1 && sq.width > x.width);
    CHECK(math_box(mf, "\\left( \\frac{\\frac{a}{b}}{\\frac{c}{d}} \\right)", 0, &del, NULL) > 0);
    CHECK(math_box(mf, "\\frac{\\frac{a}{b}}{\\frac{c}{d}}", 0, &a, NULL) > 0);
    CHECK(del.height >= a.height && del.depth >= a.depth && del.width > a.width);   /* the parentheses enclose it */
    CHECK(math_box(mf, "\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}", 0, &b, NULL) >= 6);
    CHECK(math_box(mf, "\\mathbb{R} \\alpha \\unknowncommand", 0, &a, NULL) > 3);

    /* the same input, the same layout */
    {
        pd_math_item p1[512], p2[512];
        int32_t n1, n2;
        const char* t = "\\int_0^\\infty e^{-x^2}\\,dx = \\frac{\\sqrt{\\pi}}{2}";

        pd_math_layout(mf, PD_PT(10), t, strlen(t), 1, p1, 512, &n1, NULL);
        pd_math_layout(mf, PD_PT(10), t, strlen(t), 1, p2, 512, &n2, NULL);
        CHECK(n1 == n2 && memcmp(p1, p2, (size_t)n1 * sizeof(pd_math_item)) == 0);
    }

    /* malformed input: never a crash */
    for (k = 0; k < 3000; k++) {
        static const char* bits[] = { "\\frac", "{", "}", "^", "_", "\\left(", "\\right)", "\\sqrt", "[", "]", "x",
                                      "&", "\\\\", "\\begin{matrix}", "\\end{matrix}", "\\sum", "'", "\\hat", "\\",
                                      "\\text{", "\\mathbb", "\\big(", "2", " "
                                    };
        char buf[256];
        size_t len = 0;
        int j, parts = 1 + (int)(seed % 20);

        for (j = 0; j < parts; j++) {
            const char* piece;

            seed = seed * 1103515245u + 12345u;
            piece = bits[(seed >> 8) % (sizeof(bits) / sizeof(bits[0]))];

            if (len + strlen(piece) < sizeof(buf)) {
                memcpy(buf + len, piece, strlen(piece));
                len += strlen(piece);
            }
        }

        pd_math_layout(mf, PD_PT(10), buf, len, (int)(seed & 1), NULL, 0, &n, NULL);
    }

    printf("  x^2 %.1fpt wide; display sum %.1f+%.1fpt vs text %.1f+%.1fpt; 3000 mangled formulas\n",
           x2.width / 65536.0, sumd.height / 65536.0, sumd.depth / 65536.0, sumt.height / 65536.0, sumt.depth / 65536.0);
}

static void test_determinism(const pd_font* f) {
    pd_params prm;
    uint64_t h1, h2;
    pd_para* p = make_para(f, text_en, PD_PT(10));
    pd_para* q = make_para(f, text_en, PD_PT(10));

    pd_params_init(&prm);
    prm.width = PD_PT(250);
    pd_para_break(p, &prm, NULL);
    pd_para_break(q, &prm, NULL);
    h1 = layout_hash(p);
    h2 = layout_hash(q);
    CHECK(h1 == h2);
    printf("  layout fingerprint (compare across platforms): %016llx\n", (unsigned long long)h1);
    pd_para_free(p);
    pd_para_free(q);
}

/* an edit followed by an incremental break equals a fresh break of the edited text */
static void test_incremental(const pd_font* f) {
    char edited[4096];
    pd_params prm;
    pd_break_info info;
    pd_style st;
    pd_para* p = make_para(f, text_en, PD_PT(10));
    pd_para* fresh;
    size_t cut = strlen(text_en) * 2 / 3;
    int widths[3] = { 140, 220, 345 }, w;

    while (text_en[cut] != ' ') {
        cut++;
    }

    snprintf(edited, sizeof(edited), "%.*s extraordinarily%s", (int)cut, text_en, text_en + cut);
    pd_style_init(&st, f, PD_PT(10));

    for (w = 0; w < 3; w++) {
        pd_params_init(&prm);
        prm.width = PD_PT(widths[w]);
        pd_para_clear(p);
        pd_para_add_text(p, text_en, strlen(text_en), &st);
        pd_para_break(p, &prm, NULL);

        pd_para_clear(p);
        pd_para_add_text(p, edited, strlen(edited), &st);
        CHECK(pd_para_break(p, &prm, &info) == PD_OK);
        CHECK(info.reused_breakpoints > 1);

        fresh = make_para(f, edited, PD_PT(10));
        pd_para_break(fresh, &prm, NULL);
        CHECK(layout_hash(fresh) == layout_hash(p));
        pd_para_free(fresh);
    }

    printf("  reused %d breakpoint states after a 2/3-way edit\n", info.reused_breakpoints);
    pd_para_free(p);
}

static void test_freeze(const pd_font* f) {
    char edited[4096];
    pd_params prm;
    pd_break_info info;
    pd_style st;
    pd_line before[64], L;
    int32_t nb, i, same = 0;
    pd_para* p = make_para(f, text_en, PD_PT(10));
    size_t cut = strlen(text_en) / 2;

    while (text_en[cut] != ' ') {
        cut++;
    }

    pd_params_init(&prm);
    prm.width = PD_PT(200);
    pd_para_break(p, &prm, NULL);
    nb = pd_para_line_count(p);

    for (i = 0; i < nb && i < 64; i++) {
        pd_para_get_line(p, i, &before[i]);
    }

    snprintf(edited, sizeof(edited), "%.*s extraordinarily%s", (int)cut, text_en, text_en + cut);
    pd_style_init(&st, f, PD_PT(10));
    pd_para_clear(p);
    pd_para_add_text(p, edited, strlen(edited), &st);
    prm.freeze_offset = (int32_t)cut;
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    CHECK(info.frozen_lines > 0);

    for (i = 0; i < info.frozen_lines; i++) {
        pd_para_get_line(p, i, &L);
        same += (L.text_start == before[i].text_start && L.text_end == before[i].text_end);
        CHECK(L.text_end <= (uint32_t)cut);
    }

    CHECK(same == info.frozen_lines);

    /* hysteresis: a break with stability bias still yields a valid layout */
    prm.freeze_offset = -1;
    prm.hysteresis = 100000;
    pd_para_clear(p);
    pd_para_add_text(p, text_en, strlen(text_en), &st);
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    CHECK(info.lines > 0 && info.overfull == 0);
    pd_para_free(p);
}

static void test_special_chars(const pd_font* f) {
    pd_params prm;
    pd_break_info info;
    pd_line L;
    pd_glyph g[256];
    int32_t n, i, saw_hyphen = 0;
    pd_para* p;

    pd_params_init(&prm);

    /* forced line breaks */
    p = make_para(f, "first\nsecond\n\nfourth", PD_PT(10));
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    CHECK(info.lines == 4);
    pd_para_get_line(p, 2, &L);
    CHECK(L.ascent > 0);    /* the empty line still has height */
    pd_para_free(p);

    /* a no-break space never breaks: narrow column, two words glued */
    p = make_para(f, "aaaa\xC2\xA0" "bbbb cccc", PD_PT(10));
    prm.width = PD_PT(30);
    pd_para_break(p, &prm, &info);
    pd_para_get_line(p, 0, &L);
    CHECK(L.text_end > 6);  /* "aaaa bbbb" stays on one line */
    pd_para_free(p);

    /* soft hyphens: breaking there shows a hyphen glyph */
    p = make_para(f, "su\xC2\xAD" "per\xC2\xAD" "cal\xC2\xAD" "i\xC2\xAD" "fra\xC2\xAD" "gi\xC2\xAD" "lis\xC2\xAD"
                  "tic ex\xC2\xAD" "pi\xC2\xAD" "ali\xC2\xAD" "do\xC2\xAD" "cious", PD_PT(10));
    prm.width = PD_PT(40);
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);

    for (i = 0; i < info.lines; i++) {
        pd_para_get_line(p, i, &L);

        if (L.hyphenated) {
            pd_para_get_glyphs(p, i, g, 256, &n);
            saw_hyphen += (n > 0 && g[n - 1].glyph == pd_font_glyph_index(f, 0x2010)) ||
                          (n > 0 && g[n - 1].glyph == pd_font_glyph_index(f, '-'));
        }
    }

    CHECK(saw_hyphen > 0);

    /* "supercalifra-" alone on a 40pt line has nothing to stretch: underfull */
    {
        int32_t under = 0;

        for (i = 0; i < info.lines; i++) {
            pd_para_get_line(p, i, &L);

            if (L.underfull) {
                under++;
                CHECK(L.ratio == 0 && L.width < prm.width && !L.overfull);
            }
        }

        CHECK(under > 0 && under == info.underfull);
    }

    /* ragged lines are never underfull */
    prm.align = PD_ALIGN_LEFT;
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    CHECK(info.underfull == 0);
    prm.align = PD_ALIGN_JUSTIFY;
    pd_para_free(p);

    /* a word wider than the line: an overfull line, not a failure */
    p = make_para(f, "a supercalifragilisticexpialidocious b", PD_PT(10));
    prm.width = PD_PT(50);
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    CHECK(info.overfull >= 1);
    pd_para_free(p);

    /* invalid UTF-8 is replaced, not trusted */
    p = make_para(f, "ok \xFF\xC3 end", PD_PT(10));
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    pd_para_free(p);
}

static void test_shape(const pd_font* f) {
    pd_params prm;
    pd_break_info info;
    pd_line L;
    pd_sp ind[5], wid[5];
    int32_t i;
    pd_para* p = make_para(f, text_en, PD_PT(10));

    /* a 120pt figure on the left for the first four lines */
    for (i = 0; i < 4; i++) {
        ind[i] = PD_PT(130);
        wid[i] = PD_PT(170);
    }

    ind[4] = 0;
    wid[4] = PD_PT(300);
    CHECK(pd_para_set_shape(p, 5, ind, wid) == PD_OK);
    pd_params_init(&prm);
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);

    for (i = 0; i < info.lines - 1; i++) {
        pd_para_get_line(p, i, &L);
        CHECK(L.x == (i < 4 ? PD_PT(130) : 0));
        CHECK(L.width == (i < 4 ? PD_PT(170) : PD_PT(300)));
    }

    pd_para_free(p);
}

static void test_ragged(const pd_font* f) {
    pd_params prm;
    pd_break_info info;
    pd_line L;
    int32_t i, a;
    pd_para* p = make_para(f, text_en, PD_PT(10));
    pd_align al[3] = { PD_ALIGN_LEFT, PD_ALIGN_RIGHT, PD_ALIGN_CENTER };

    for (a = 0; a < 3; a++) {
        pd_params_init(&prm);
        prm.width = PD_PT(200);
        prm.align = al[a];
        CHECK(pd_para_break(p, &prm, &info) == PD_OK);

        for (i = 0; i < info.lines; i++) {
            pd_para_get_line(p, i, &L);
            CHECK(L.width <= prm.width);
            CHECK(L.ratio >= 0);

            if (al[a] == PD_ALIGN_LEFT) {
                CHECK(L.x == 0);
            } else if (al[a] == PD_ALIGN_RIGHT) {
                CHECK(L.x + L.width == prm.width);
            }
        }
    }

    pd_para_free(p);
}

static void test_caret(const pd_font* f) {
    pd_params prm;
    pd_para* p = make_para(f, text_en, PD_PT(10));
    uint32_t off, got, len = (uint32_t)strlen(text_en);
    int32_t line, hl, bad = 0;
    pd_sp x, y;

    pd_params_init(&prm);
    prm.width = PD_PT(180);
    pd_para_break(p, &prm, NULL);

    /* caret -> point -> offset round trip at every character start */
    for (off = 0; off <= len; off++) {
        pd_line L;

        CHECK(pd_para_caret(p, off, &line, &x, &y) == PD_OK);
        pd_para_get_line(p, line, &L);
        CHECK(x >= L.x && x <= L.x + L.width);
        CHECK(pd_para_hit_test(p, x, y, &got, &hl) == PD_OK);

        /* the trailing space of a broken line maps to the line end */
        if (got != off && !(text_en[off] == ' ' && got == off && hl == line) &&
                !(off < len && text_en[off] == ' ' && off + 1 == L.text_end)) {
            bad++;
        }
    }

    CHECK(bad == 0);

    if (bad) {
        printf("  caret round-trip mismatches: %d\n", bad);
    }

    CHECK(pd_para_caret(p, len + 1, &line, &x, &y) == PD_ERR_RANGE);
    pd_para_free(p);
}

static void test_object(const pd_font* f) {
    pd_params prm;
    pd_break_info info;
    pd_style st;
    pd_glyph g[256];
    int32_t n, i, found = 0, li;
    pd_para* p = NULL;

    pd_para_new(&p);
    pd_style_init(&st, f, PD_PT(10));
    pd_para_add_text(p, "Equation ", 9, &st);
    CHECK(pd_para_add_object(p, PD_PT(40), PD_PT(15), PD_PT(5), 42) == PD_OK);
    pd_para_add_text(p, " follows the text.", 18, &st);
    pd_params_init(&prm);
    pd_para_break(p, &prm, &info);

    for (li = 0; li < info.lines; li++) {
        pd_para_get_glyphs(p, li, g, 256, &n);

        for (i = 0; i < n; i++) {
            if (g[i].kind == PD_OBJECT) {
                found = (g[i].user == 42 && g[i].advance == PD_PT(40));
            }
        }
    }

    CHECK(found);
    {
        pd_line L;
        pd_para_get_line(p, 0, &L);
        CHECK(L.ascent >= PD_PT(15));
    }
    pd_para_free(p);
}

static void test_cjk(const pd_font* f) {
    /* 天地玄黄，宇宙洪荒。日月盈昃，辰宿列张。 repeated */
    const char* s = "\xE5\xA4\xA9\xE5\x9C\xB0\xE7\x8E\x84\xE9\xBB\x84\xEF\xBC\x8C\xE5\xAE\x87\xE5\xAE\x99"
                    "\xE6\xB4\xAA\xE8\x8D\x92\xE3\x80\x82\xE6\x97\xA5\xE6\x9C\x88\xE7\x9B\x88\xE6\x98\x83"
                    "\xEF\xBC\x8C\xE8\xBE\xB0\xE5\xAE\xBF\xE5\x88\x97\xE5\xBC\xA0\xE3\x80\x82";
    char buf[1024];
    pd_params prm;
    pd_break_info info;
    pd_line L;
    int32_t i;
    pd_para* p;

    snprintf(buf, sizeof(buf), "%s%s%s", s, s, s);
    p = make_para(f, buf, PD_PT(10));
    pd_params_init(&prm);
    prm.width = PD_PT(75);
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);
    CHECK(info.lines >= 3);

    /* kinsoku: no line starts with an ideographic comma or full stop */
    for (i = 1; i < info.lines; i++) {
        pd_para_get_line(p, i, &L);
        CHECK(memcmp(buf + L.text_start, "\xEF\xBC\x8C", 3) != 0);
        CHECK(memcmp(buf + L.text_start, "\xE3\x80\x82", 3) != 0);
    }

    pd_para_free(p);
}

static void test_raster(const pd_font* f) {
    pd_glyph_image gi, gi2;
    uint8_t buf[200 * 200];
    uint32_t A = pd_font_glyph_index(f, 'A'), space = pd_font_glyph_index(f, ' ');
    uint64_t h1 = 0, h2 = 0;
    int64_t ink = 0, ink2 = 0;
    int32_t i, s;
    pd_font* cjk = NULL;

    CHECK(pd_font_glyph_render(f, A, PD_PT(64), 0, NULL, 0, &gi) == PD_OK);     /* size query */
    CHECK(gi.width > 30 && gi.width < 70 && gi.height > 35 && gi.height < 50 && gi.top > 35);
    CHECK(pd_font_glyph_render(f, A, PD_PT(64), 0, buf, 10, &gi) == PD_ERR_RANGE);
    CHECK(pd_font_glyph_render(f, A, PD_PT(64), 0, buf, sizeof(buf), &gi) == PD_OK);

    for (i = 0; i < gi.width * gi.height; i++) {
        ink += buf[i];
        h1 = (h1 ^ buf[i]) * 1099511628211ULL;
    }

    /* Liberation Serif 'A' covers 0.1187 em^2 per fontTools AreaPen (64 px: 486.0 px^2) */
    CHECK(ink / 255 > 478 && ink / 255 < 494);
    CHECK(pd_font_glyph_render(f, A, PD_PT(64), 0, buf, sizeof(buf), &gi2) == PD_OK);

    for (i = 0; i < gi2.width * gi2.height; i++) {
        h2 = (h2 ^ buf[i]) * 1099511628211ULL;
    }

    CHECK(h1 == h2);    /* deterministic */
    printf("  'A' at 64 px: %dx%d, bitmap hash %016llx\n", gi.width, gi.height, (unsigned long long)h1);

    /* subpixel shifts move ink, they do not change it */
    for (s = 0; s < 256; s += 64) {
        ink2 = 0;
        pd_font_glyph_render(f, A, PD_PT(64), s, buf, sizeof(buf), &gi2);

        for (i = 0; i < gi2.width * gi2.height; i++) {
            ink2 += buf[i];
        }

        CHECK(ink2 / 255 > 478 && ink2 / 255 < 494);
    }

    CHECK(pd_font_glyph_render(f, space, PD_PT(64), 0, buf, sizeof(buf), &gi) == PD_OK && gi.width == 0);
    CHECK(pd_font_glyph_render(f, 0xFFFFFF, PD_PT(64), 0, buf, sizeof(buf), &gi) == PD_ERR_FONT);
    CHECK(pd_font_glyph_render(f, A, 0, 0, buf, sizeof(buf), &gi) == PD_ERR_ARG);

    if (pd_font_load_file("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0, &cjk) == PD_OK) {
        CHECK(pd_font_glyph_render(cjk, pd_font_glyph_index(cjk, 0x5929), PD_PT(32), 0, buf, sizeof(buf), &gi) ==
              PD_OK && gi.width > 20);   /* CFF outline */
        pd_font_free(cjk);
    }

    /* a font with corrupted glyph data renders garbage or fails, never crashes */
    {
        FILE* fp = fopen(getenv("PARADE_TEST_FONT") ? getenv("PARADE_TEST_FONT") :
                         "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf", "rb");
        long n;
        unsigned char* data;
        int k;

        if (fp && fseek(fp, 0, SEEK_END) == 0 && (n = ftell(fp)) > 0 && fseek(fp, 0, SEEK_SET) == 0) {
            data = (unsigned char*)malloc((size_t)n);

            if (data && fread(data, 1, (size_t)n, fp) == (size_t)n) {
                srand(5);

                for (k = 0; k < 200; k++) {
                    pd_font* bad = NULL;
                    int j;

                    for (j = 0; j < 50; j++) {
                        data[rand() % n] ^= (unsigned char)(1 << (rand() % 8));
                    }

                    if (pd_font_load_memory(data, (size_t)n, 0, &bad) == PD_OK) {
                        for (j = 0; j < 30; j++) {
                            pd_font_glyph_render(bad, (uint32_t)(rand() % 3000), PD_PT(20 + rand() % 60), 0, buf, sizeof(buf),
                                                 &gi);
                        }

                        pd_font_free(bad);
                    }
                }
            }

            free(data);
        }

        if (fp) {
            fclose(fp);
        }
    }
}

static void test_unicode(const pd_font* f) {
    /* family emoji (ZWJ sequence), flag pair, e + combining acute */
    const char* fam = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7x";
    const char* flags = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
    const char* comb = "cafe\xCC\x81 ok";
    uint8_t lb[64];
    pd_params prm;
    pd_break_info info;
    pd_line L;
    int32_t i;
    uint32_t off;
    pd_para* p;

    CHECK(pd_text_next_grapheme(fam, strlen(fam), 0) == 18);    /* the whole family is one cluster */
    CHECK(pd_text_prev_grapheme(fam, strlen(fam), 18) == 0);
    CHECK(pd_text_next_grapheme(flags, strlen(flags), 0) == 8);   /* JP */
    CHECK(pd_text_next_grapheme(flags, strlen(flags), 8) == 16);  /* US */
    CHECK(pd_text_next_grapheme(comb, strlen(comb), 3) == 6);     /* e + U+0301 */
    CHECK(pd_text_line_breaks("ab cd-ef $(12.35)", 17, lb) == PD_OK);
    CHECK(lb[3] == 1 && lb[1] == 0 && lb[6] == 1 && lb[10] == 0 && lb[14] == 0 && lb[17] == 2);

    pd_params_init(&prm);

    /* numbers and closing brackets stay attached even in a narrow column */
    p = make_para(f, "price $(12.35) and x ) y", PD_PT(10));
    prm.width = PD_PT(25);
    CHECK(pd_para_break(p, &prm, &info) == PD_OK);

    for (i = 0; i < info.lines; i++) {
        const char* text = "price $(12.35) and x ) y";

        pd_para_get_line(p, i, &L);
        CHECK(text[L.text_start] != ')');
        CHECK(!(L.text_start > 6 && L.text_start < 14));    /* "$(12.35)" is never split */
    }

    pd_para_free(p);

    /* hit testing never lands between e and its combining accent */
    p = make_para(f, comb, PD_PT(20));
    prm.width = PD_PT(300);
    pd_para_break(p, &prm, &info);

    for (i = 0; i < 4000; i += 50) {
        CHECK(pd_para_hit_test(p, PD_PT(i / 50), PD_PT(10), &off, NULL) == PD_OK && off != 4);
    }

    pd_para_free(p);
}

/* glyph clusters of line 0 in visual order */
static int32_t visual_clusters(pd_para* p, uint32_t* out, int32_t cap) {
    pd_glyph g[256];
    int32_t n = 0, i;

    pd_para_get_glyphs(p, 0, g, 256, &n);

    for (i = 0; i < n && i < cap; i++) {
        out[i] = g[i].cluster;
    }

    return n;
}

static void test_bidi(const pd_font* f) {
    /* "ab " + Hebrew alef bet gimel + " (cd)" : the Hebrew word is reversed in place */
    const char* mixed = "ab \xD7\x90\xD7\x91\xD7\x92 cd";
    const char* rtl = "\xD7\x90\xD7\x91 (x) \xD7\x92";
    pd_params prm;
    pd_para* p;
    uint32_t cl[64], off;
    int32_t n, i, line, bad = 0;
    pd_sp x, y;

    pd_params_init(&prm);
    prm.width = PD_PT(400);
    prm.align = PD_ALIGN_LEFT;

    p = make_para(f, mixed, PD_PT(12));
    CHECK(pd_para_break(p, &prm, NULL) == PD_OK);
    n = visual_clusters(p, cl, 64);
    /* a b | gimel bet alef | c d */
    CHECK(n == 7 && cl[0] == 0 && cl[1] == 1 && cl[2] == 7 && cl[3] == 5 && cl[4] == 3 && cl[5] == 10 && cl[6] == 11);

    /* caret and hit test agree inside each run */
    for (off = 0; off <= 12; off++) {
        uint32_t back;

        if (off == 3 || off == 9 || (off > 3 && off < 9 && (off % 2) == 0)) {
            continue;   /* run boundaries are visually ambiguous; skip continuation bytes */
        }

        if (off > 3 && off < 9 && (off % 2) == 0) {
            continue;
        }

        pd_para_caret(p, off, &line, &x, &y);
        pd_para_hit_test(p, x + (off >= 3 && off < 9 ? -PD_PT(0.5) : PD_PT(0.5)), y, &back, NULL);
        bad += back != off;
    }

    CHECK(bad == 0);
    pd_para_free(p);

    /* an RTL paragraph: runs reversed, brackets mirrored, the line end on the left */
    prm.direction = PD_DIR_RTL;
    p = make_para(f, rtl, PD_PT(12));
    CHECK(pd_para_break(p, &prm, NULL) == PD_OK);
    n = visual_clusters(p, cl, 64);
    /* visual: gimel ( x ) bet alef  -> clusters 10, 7(')' mirrored), 6, 5('(' mirrored), 2, 0 */
    CHECK(n == 6 && cl[0] == 9 && cl[1] == 7 && cl[2] == 6 && cl[3] == 5 && cl[4] == 2 && cl[5] == 0);
    {
        pd_glyph g[16];

        pd_para_get_glyphs(p, 0, g, 16, &n);
        CHECK(g[1].glyph == pd_font_glyph_index(f, '('));   /* ')' at cluster 7 is drawn as '(' */
        CHECK(g[3].glyph == pd_font_glyph_index(f, ')'));
    }
    pd_para_caret(p, (uint32_t)strlen(rtl), &line, &x, &y);
    {
        pd_line L;

        pd_para_get_line(p, 0, &L);
        CHECK(x == L.x);    /* the end of an RTL line is its left edge */
    }

    pd_para_free(p);
    (void)i;
}

int main(void) {
    pd_font* f = load_env_font("PARADE_TEST_FONT", "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf");
    pd_font* cjk = load_env_font("PARADE_TEST_CJK_FONT", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");

    printf("parade %s\n", pd_version());

    if (!f) {
        printf("no test font; set PARADE_TEST_FONT\n");
        return 77;
    }

    printf("font\n");
    test_font(f);
    printf("rasterizer\n");
    test_raster(f);
    printf("unicode segmentation\n");
    test_unicode(f);
    printf("bidi\n");
    test_bidi(f);
    test_liberation(f);
    printf("justify\n");
    test_justify(f);
    printf("greedy vs optimal\n");
    test_greedy_vs_optimal(f);
    printf("looseness\n");
    test_looseness(f);
    printf("microtypography\n");
    test_microtype(f);
    printf("determinism\n");
    test_determinism(f);
    printf("incremental\n");
    test_incremental(f);
    printf("freeze/hysteresis\n");
    test_freeze(f);
    printf("special characters\n");
    test_special_chars(f);
    printf("parshape\n");
    test_shape(f);
    printf("ragged\n");
    test_ragged(f);
    printf("caret/hit test\n");
    test_caret(f);
    printf("inline object\n");
    test_object(f);

    {
        pd_font* mf = load_env_font("PARADE_TEST_MATH_FONT",
                                    "/usr/share/texmf/fonts/opentype/public/lm-math/latinmodern-math.otf");

        if (mf) {
            printf("math\n");
            test_math(mf);
            pd_font_free(mf);
        } else {
            printf("math (skipped, no math font)\n");
        }
    }

    if (cjk) {
        printf("cjk\n");
        test_cjk(cjk);
        pd_font_free(cjk);
    } else {
        printf("cjk (skipped, no CJK font)\n");
    }

    pd_font_free(f);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
