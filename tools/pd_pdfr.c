/* pd_pdfr: a PDF's pages drawn to PNG, its text printed -- for checking the reader by eye and by script
 *
 *   pd_pdfr render file.pdf page dpi out.png
 *   pd_pdfr text file.pdf page
 *   pd_pdfr info file.pdf
 *   pd_pdfr time file.pdf dpi          (every page drawn, the time each took)
 *   pd_pdfr annotate in.pdf out.pdf    (a highlight, shapes, ink, a note, a text box on page 1; the first
 *                                       text field filled: saved as an update)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "parade_pdfread.h"

int pd_png_encode(const uint8_t* rgba, int32_t w, int32_t h, int32_t stride, unsigned char** out, size_t* n);

static double now(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static int render(pd_pdfr* p, int page, double dpi, const char* out) {
    double w, h;
    int32_t W, H;
    uint8_t* buf;
    unsigned char* png;
    size_t n;
    FILE* fp;

    if (pd_pdfr_page_size(p, page, &w, &h) != PD_OK) {
        return 1;
    }

    W = (int32_t)(w * dpi / 72 + 0.5);
    H = (int32_t)(h * dpi / 72 + 0.5);
    buf = (uint8_t*)malloc((size_t)W * H * 4);

    if (!buf || pd_pdfr_render(p, page, dpi / 72, 0, 0, buf, W, H, W * 4, 0) != PD_OK) {
        return 1;
    }

    if (pd_png_encode(buf, W, H, W * 4, &png, &n)) {
        return 1;
    }

    fp = fopen(out, "wb");

    if (!fp) {
        return 1;
    }

    fwrite(png, 1, n, fp);
    fclose(fp);
    free(png);
    free(buf);
    return 0;
}

static void put_utf8(uint32_t c) {
    char o[5];
    int n = 0;

    if (c < 0x80) {
        o[n++] = (char)c;
    } else if (c < 0x800) {
        o[n++] = (char)(0xC0 | (c >> 6));
        o[n++] = (char)(0x80 | (c & 63));
    } else if (c < 0x10000) {
        o[n++] = (char)(0xE0 | (c >> 12));
        o[n++] = (char)(0x80 | ((c >> 6) & 63));
        o[n++] = (char)(0x80 | (c & 63));
    } else {
        o[n++] = (char)(0xF0 | (c >> 18));
        o[n++] = (char)(0x80 | ((c >> 12) & 63));
        o[n++] = (char)(0x80 | ((c >> 6) & 63));
        o[n++] = (char)(0x80 | (c & 63));
    }

    fwrite(o, 1, (size_t)n, stdout);
}

int main(int argc, char** argv) {
    pd_pdfr* p = NULL;
    pd_status st;

    if (argc < 3) {
        fprintf(stderr, "usage: pd_pdfr render|text|info|time file.pdf ...\n");
        return 2;
    }

    st = pd_pdfr_open_file(argv[2], NULL, &p);

    if (st != PD_OK) {
        fprintf(stderr, "%s: %s (%d)\n", argv[2], p ? pd_pdfr_error(p) : "cannot open", st);
        pd_pdfr_free(p);
        return 1;
    }

    if (strcmp(argv[1], "info") == 0) {
        char title[256];
        int32_t i;

        pd_pdfr_info(p, "Title", title, sizeof(title));
        printf("pages %d title \"%s\"\n", pd_pdfr_page_count(p), title);

        for (i = 0; i < pd_pdfr_page_count(p) && i < 3; i++) {
            double w, h;

            pd_pdfr_page_size(p, i, &w, &h);
            printf("page %d: %.1f x %.1f\n", i + 1, w, h);
        }
    } else if (strcmp(argv[1], "render") == 0 && argc >= 6) {
        double t = now();
        int r = render(p, atoi(argv[3]) - 1, atof(argv[4]), argv[5]);

        fprintf(stderr, "%.3f s\n", now() - t);
        pd_pdfr_free(p);
        return r;
    } else if (strcmp(argv[1], "text") == 0 && argc >= 4) {
        int32_t n = 0, i;
        pd_pdfr_char* c;

        pd_pdfr_page_text(p, atoi(argv[3]) - 1, NULL, 0, &n);
        c = (pd_pdfr_char*)malloc(sizeof(pd_pdfr_char) * (size_t)(n + 1));
        pd_pdfr_page_text(p, atoi(argv[3]) - 1, c, n, &n);

        for (i = 0; i < n; i++) {
            put_utf8(c[i].cp);
        }

        printf("\n");
        free(c);
    } else if (strcmp(argv[1], "annotate") == 0 && argc >= 4) {
        int32_t n = 0, i;
        pd_pdfr_char* c;
        double pw, ph;

        pd_pdfr_page_size(p, 0, &pw, &ph);
        pd_pdfr_page_text(p, 0, NULL, 0, &n);
        c = (pd_pdfr_char*)malloc(sizeof(pd_pdfr_char) * (size_t)(n + 1));
        pd_pdfr_page_text(p, 0, c, n, &n);

        if (n > 12) {
            double q[8] = { c[0].x0, c[0].y0, c[11].x1, c[0].y0, c[0].x0, c[0].y1, c[11].x1, c[0].y1 };

            printf("highlight %d\n", pd_pdfr_add_markup(p, 0, PD_ANNOT_HIGHLIGHT, q, 1, 0xFFFF00, "Parade"));
        }

        printf("square %d\n", pd_pdfr_add_shape(p, 0, PD_ANNOT_SQUARE, pw * 0.6, ph * 0.1, pw * 0.9, ph * 0.2, 0xFF0000,
                                                0x400000FF, 3));
        printf("arrow %d\n", pd_pdfr_add_shape(p, 0, PD_ANNOT_ARROW, pw * 0.1, ph * 0.5, pw * 0.4, ph * 0.4, 0x0000FF,
                                               0, 2));
        {
            double pts[] = { 50, 300, 70, 320, 90, 300, 110, 320, 130, 300 };
            int32_t cnt[] = { 5 };

            printf("ink %d\n", pd_pdfr_add_ink(p, 0, pts, cnt, 1, 0x008800, 2.5));
        }
        printf("note %d\n", pd_pdfr_add_note(p, 0, pw - 40, 20, "Checked by Parade", 0xFFD700));
        printf("box %d\n", pd_pdfr_add_text_box(p, 0, pw * 0.55, ph * 0.3, pw * 0.95, ph * 0.38,
                                                "A text box typed in Parade, long enough to wrap.", 11, 0x000080));

        for (i = 0; i < pd_pdfr_field_count(p); i++) {
            pd_pdfr_field f;

            if (pd_pdfr_field_get(p, i, &f) == PD_OK && f.kind == PD_PDF_FIELD_TEXT && !f.readonly) {
                printf("field %s %d\n", f.name, pd_pdfr_field_set(p, i, "Parade"));
                break;
            }
        }

        free(c);
        printf("saved %d\n", pd_pdfr_save_file(p, argv[3]));
    } else if (strcmp(argv[1], "time") == 0 && argc >= 4) {
        int32_t i;
        double total = now(), worst = 0;

        for (i = 0; i < pd_pdfr_page_count(p); i++) {
            double t = now(), w, h;
            int32_t W, H;
            uint8_t* buf;

            pd_pdfr_page_size(p, i, &w, &h);
            W = (int32_t)(w * atof(argv[3]) / 72 + 0.5);
            H = (int32_t)(h * atof(argv[3]) / 72 + 0.5);
            buf = (uint8_t*)malloc((size_t)W * H * 4);
            pd_pdfr_render(p, i, atof(argv[3]) / 72, 0, 0, buf, W, H, W * 4, 0);
            free(buf);
            t = now() - t;
            worst = t > worst ? t : worst;
        }

        printf("%d pages, %.3f s, slowest %.3f s\n", pd_pdfr_page_count(p), now() - total, worst);
    }

    pd_pdfr_free(p);
    return 0;
}
