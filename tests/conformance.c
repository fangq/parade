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

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "build/ucd";

    verbose = argc > 2 && !strcmp(argv[2], "-v");
    run(dir, "LineBreakTest.txt", 1);
    run(dir, "GraphemeBreakTest.txt", 0);
    return 0;
}
