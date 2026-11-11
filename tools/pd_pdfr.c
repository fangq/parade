/* pd_pdfr: a PDF's pages drawn to PNG, its text printed -- for checking the reader by eye and by script
 *
 *   pd_pdfr render file.pdf page dpi out.png
 *   pd_pdfr text file.pdf page
 *   pd_pdfr info file.pdf
 *   pd_pdfr time file.pdf dpi          (every page drawn, the time each took)
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
