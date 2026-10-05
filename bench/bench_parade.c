/*
 * Parade benchmark: greedy vs optimal quality and speed on a text corpus
 *
 * usage: bench_parade [width_pt] [corpus.txt] [font.ttf]
 * Paragraphs are separated by blank lines; newlines inside a paragraph
 * are treated as spaces.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "parade.h"

#define MAXP 4000

static double now(void) {
    return (double)clock() / CLOCKS_PER_SEC;
}

typedef struct {
    int lines, gappy;
    double sum_r, max_r;
    int nr;
} stat_t;

static void collect(const pd_para* p, stat_t* s) {
    int32_t i, n = pd_para_line_count(p);

    for (i = 0; i + 1 < n; i++) {   /* the last line is not justified */
        pd_line L;
        double r;

        pd_para_get_line(p, i, &L);
        r = L.ratio / 1000.0;
        s->sum_r += r < 0 ? -r : r;
        s->max_r = r > s->max_r ? r : s->max_r;
        s->gappy += r > 1.0;
        s->nr++;
    }

    s->lines += n;
}

int main(int argc, char** argv) {
    double width = argc > 1 ? atof(argv[1]) : 345;
    const char* corpus = argc > 2 ? argv[2] : "/usr/share/common-licenses/GPL-3";
    const char* fontpath = argc > 3 ? argv[3] : "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf";
    static char buf[1 << 21];
    static char* paras[MAXP];
    static size_t plen[MAXP];
    int np = 0, i, r, R = 200, words = 0;
    pd_font* font;
    pd_style st;
    pd_params prm;
    pd_para** pp;
    stat_t sg = {0}, so = {0};
    double t0, tg, to, tb, tinc = 0, tfull = 0;
    size_t len;
    char* s;
    FILE* fp;

    if (pd_font_load_file(fontpath, 0, &font) != PD_OK) {
        fprintf(stderr, "cannot load %s\n", fontpath);
        return 1;
    }

    fp = fopen(corpus, "rb");

    if (!fp) {
        fprintf(stderr, "cannot open %s\n", corpus);
        return 1;
    }

    len = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[len] = 0;
    fclose(fp);

    for (s = buf; *s && np < MAXP;) {
        char* e;
        char* q;
        int nw = 0;

        while (*s == '\n') {
            s++;
        }

        if (!*s) {
            break;
        }

        e = strstr(s, "\n\n");
        e = e ? e : s + strlen(s);

        for (q = s; q < e; q++) {
            if (*q == '\n') {
                *q = ' ';
            }

            nw += (*q == ' ');
        }

        if (nw >= 11) {
            paras[np] = s;
            plen[np++] = (size_t)(e - s);
            words += nw + 1;
        }

        s = e;
    }

    pd_style_init(&st, font, PD_PT(10));
    pd_params_init(&prm);
    prm.width = (pd_sp)(width * PD_SP_PER_PT);
    pp = (pd_para**)calloc(np, sizeof(pd_para*));

    /* build once: shaping and item construction */
    t0 = now();

    for (i = 0; i < np; i++) {
        pd_para_new(&pp[i]);
        pd_para_add_text(pp[i], paras[i], plen[i], &st);
    }

    tb = now() - t0;

    prm.mode = PD_BREAK_GREEDY;

    for (i = 0; i < np; i++) {
        pd_para_break(pp[i], &prm, NULL);
        collect(pp[i], &sg);
    }

    prm.mode = PD_BREAK_OPTIMAL;

    for (i = 0; i < np; i++) {
        pd_para_break(pp[i], &prm, NULL);
        collect(pp[i], &so);
    }

    /* timing: breaking only; params alternate so nothing is served from cache */
    t0 = now();

    for (r = 0; r < R; r++) {
        prm.mode = PD_BREAK_GREEDY;

        for (i = 0; i < np; i++) {
            pd_para_break(pp[i], &prm, NULL);
        }
    }

    tg = now() - t0;
    t0 = now();

    for (r = 0; r < R; r++) {
        prm.mode = PD_BREAK_OPTIMAL;
        prm.line_penalty = 10 + (r & 1);   /* defeat the incremental cache */

        for (i = 0; i < np; i++) {
            pd_para_break(pp[i], &prm, NULL);
        }
    }

    to = now() - t0;
    prm.line_penalty = 10;

    /* incremental: insert a word at 2/3 of each paragraph, relayout, undo */
    for (i = 0; i < np; i++) {
        size_t cut = plen[i] * 2 / 3;
        double a;

        while (cut < plen[i] && paras[i][cut] != ' ') {
            cut++;
        }

        for (r = 0; r < 20; r++) {
            pd_para_clear(pp[i]);
            pd_para_add_text(pp[i], paras[i], cut, &st);
            pd_para_add_text(pp[i], (r & 1) ? " extraordinarily" : " remarkably", (r & 1) ? 16 : 11, &st);
            pd_para_add_text(pp[i], paras[i] + cut, plen[i] - cut, &st);
            a = now();
            pd_para_break(pp[i], &prm, NULL);
            tinc += now() - a;
        }
    }

    prm.line_penalty = 11;

    for (i = 0; i < np; i++) {
        double a;

        for (r = 0; r < 20; r++) {
            prm.line_penalty = 10 + (r & 1);
            a = now();
            pd_para_break(pp[i], &prm, NULL);
            tfull += now() - a;
        }
    }

    printf("width %.0fpt, %d paragraphs, %d words, %s\n", width, np, words, fontpath);
    printf("%-8s lines %5d  mean|r| %.3f  max r %6.2f  lines r>1 %4d\n", "greedy", sg.lines, sg.sum_r / sg.nr,
           sg.max_r, sg.gappy);
    printf("%-8s lines %5d  mean|r| %.3f  max r %6.2f  lines r>1 %4d\n", "optimal", so.lines, so.sum_r / so.nr,
           so.max_r, so.gappy);
    printf("build (shape+items): %.1f ns/word\n", tb * 1e9 / words);
    printf("break greedy:  %.1f ns/word, %.2f us/paragraph\n", tg * 1e9 / R / words, tg * 1e6 / R / np);
    printf("break optimal: %.1f ns/word, %.2f us/paragraph\n", to * 1e9 / R / words, to * 1e6 / R / np);
    printf("edit at 2/3: incremental %.2f us vs full %.2f us per paragraph\n", tinc * 1e6 / np / 20,
           tfull * 1e6 / np / 20);

    for (i = 0; i < np; i++) {
        pd_para_free(pp[i]);
    }

    free(pp);
    pd_font_free(font);
    return 0;
}
