/*
 * Unicode conformance: LineBreakTest.txt and GraphemeBreakTest.txt (and
 * BidiCharacterTest.txt once bidi is in). usage: conformance UCD_DIR [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

static int verbose = 0;

/* "× 0041 ÷ 0020 × ..." -> code points and expected boundaries (1 = break) */
#define MAXCP 2048

static int parse(const char* line, uint32_t* cp, int* expect, int32_t* n) {
    const char* p = line;

    *n = 0;

    while (*p && *p != '#' && *n < MAXCP) {
        if (!strncmp(p, "\xC3\x97", 2)) {           /* × */
            expect[*n] = 0;
            p += 2;
        } else if (!strncmp(p, "\xC3\xB7", 2)) {    /* ÷ */
            expect[*n] = 1;
            p += 2;
        } else if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')) {
            cp[(*n)++] = (uint32_t)strtoul(p, (char**)&p, 16);
        } else {
            p++;
        }
    }

    return *n > 0;
}

static void run(const char* dir, const char* name, int lb) {
    char path[1024];
    static char line[65536];
    FILE* fp;
    int pass = 0, fail = 0;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fp = fopen(path, "r");

    if (!fp) {
        printf("%s: missing\n", name);
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        static uint32_t cp[MAXCP + 1];
        static int expect[MAXCP + 1];
        static uint8_t brk[MAXCP + 1];
        int32_t n, i, ok = 1;

        if (line[0] == '#' || !parse(line, cp, expect, &n)) {
            continue;
        }

        if (lb) {
            pd_linebreaks(cp, n, brk);

            for (i = 1; i < n; i++) {
                ok &= (brk[i] != 0) == expect[i];
            }
        } else {
            for (i = 1; i < n; i++) {
                ok &= pd_grapheme_boundary(cp, n, i) == expect[i];
            }
        }

        if (ok) {
            pass++;
        } else {
            fail++;

            if (verbose && fail <= 15) {
                printf("  FAIL %s", line);
            }
        }
    }

    fclose(fp);
    printf("%s: %d passed, %d failed (%.2f%%)\n", name, pass, fail, 100.0 * pass / (pass + fail));
}

/* BidiCharacterTest.txt: code points; paragraph direction; paragraph level; levels; visual order */
static void run_bidi(const char* dir) {
    char path[1024];
    static char line[65536];
    FILE* fp;
    int pass = 0, fail = 0;

    snprintf(path, sizeof(path), "%s/BidiCharacterTest.txt", dir);
    fp = fopen(path, "r");

    if (!fp) {
        printf("BidiCharacterTest.txt: missing\n");
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        static uint32_t cp[MAXCP];
        static uint8_t lev[MAXCP], llev[MAXCP];
        static int32_t order[MAXCP], want_order[MAXCP];
        static int want_lev[MAXCP];
        int32_t n = 0, nw = 0, i, cnt, ok = 1;
        int pdir, plevel, para;
        char* f[5];
        char* q = line;

        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        for (i = 0; i < 5; i++) {
            f[i] = q;
            q = strchr(q, ';');

            if (!q && i < 4) {
                break;
            }

            if (q) {
                *q++ = '\0';
            }
        }

        if (i < 5) {
            continue;
        }

        for (q = f[0]; *q && n < MAXCP;) {
            while (*q == ' ') {
                q++;
            }

            if (!*q) {
                break;
            }

            cp[n++] = (uint32_t)strtoul(q, &q, 16);
        }

        pdir = atoi(f[1]);
        plevel = atoi(f[2]);

        for (q = f[3], i = 0; *q && i < n;) {
            while (*q == ' ') {
                q++;
            }

            if (*q == 'x') {
                want_lev[i++] = -1;
                q++;
            } else if (*q >= '0' && *q <= '9') {
                want_lev[i++] = (int)strtol(q, &q, 10);
            } else {
                break;
            }
        }

        for (q = f[4]; *q && *q != '\n';) {
            while (*q == ' ') {
                q++;
            }

            if (*q < '0' || *q > '9') {
                break;
            }

            want_order[nw++] = (int32_t)strtol(q, &q, 10);
        }

        para = pd_bidi_levels(cp, n, pdir == 2 ? -1 : pdir, lev);
        ok &= para == plevel;
        cnt = pd_bidi_line(cp, lev, 0, n, para, llev, order);

        for (i = 0; i < n; i++) {
            ok &= want_lev[i] < 0 ? llev[i] == 0xFF : llev[i] == want_lev[i];
        }

        ok &= cnt == nw;

        for (i = 0; i < cnt && i < nw; i++) {
            ok &= order[i] == want_order[i];
        }

        if (ok) {
            pass++;
        } else if (++fail <= 10 && verbose) {
            printf("  FAIL %s;%s;%s;%s;%s", f[0], f[1], f[2], f[3], f[4]);
        }
    }

    fclose(fp);
    printf("BidiCharacterTest.txt: %d passed, %d failed (%.2f%%)\n", pass, fail, 100.0 * pass / (pass + fail));
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "build/ucd";

    verbose = argc > 2 && !strcmp(argv[2], "-v");
    run(dir, "LineBreakTest.txt", 1);
    run(dir, "GraphemeBreakTest.txt", 0);
    run_bidi(dir);
    return 0;
}
