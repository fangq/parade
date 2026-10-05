/*
 * Unicode conformance: LineBreakTest.txt and GraphemeBreakTest.txt (and
 * BidiCharacterTest.txt once bidi is in). usage: conformance UCD_DIR [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

static int verbose = 0;
static int failures = 0;     /* exit status: any failed check */

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
    failures += fail;
}

static size_t utf8_encode(const uint32_t* cp, int32_t n, char* out) {
    size_t k = 0;
    int32_t i;

    for (i = 0; i < n; i++) {
        uint32_t c = cp[i];

        if (c < 0x80) {
            out[k++] = (char)c;
        } else if (c < 0x800) {
            out[k++] = (char)(0xC0 | (c >> 6));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out[k++] = (char)(0xE0 | (c >> 12));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[k++] = (char)(0xF0 | (c >> 18));
            out[k++] = (char)(0x80 | ((c >> 12) & 0x3F));
            out[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[k++] = (char)(0x80 | (c & 0x3F));
        }
    }

    return k;
}

/* pd_bidi_maybe_rtl == 0 must mean the paragraph level and every resolved level are even */
static int bidi_skip_wrong(const uint32_t* cp, int32_t n, int dir, int para, const uint8_t* lev) {
    static char u[4 * 4096];
    int32_t i;

    if (n > 4096 || pd_bidi_maybe_rtl(u, utf8_encode(cp, n, u), dir)) {
        return 0;
    }

    if (para & 1) {
        return 1;
    }

    for (i = 0; i < n; i++) {
        if (lev[i] != 0xFF && (lev[i] & 1)) {
            return 1;
        }
    }

    return 0;
}

/* every code point alone, auto and left-to-right */
static void run_bidi_skip(void) {
    uint32_t c;
    uint8_t lev[2];
    int d, para, skipped = 0, wrong = 0;
    char u[4];

    for (c = 0; c <= 0x10FFFF; c++) {
        if (c >= 0xD800 && c <= 0xDFFF) {
            continue;
        }

        for (d = -1; d <= 0; d++) {
            para = pd_bidi_levels(&c, 1, d, lev);
            skipped += !pd_bidi_maybe_rtl(u, utf8_encode(&c, 1, u), d);
            wrong += bidi_skip_wrong(&c, 1, d, para, lev);
        }
    }

    printf("bidi fast path, every code point: %d of %d paragraphs skip the UBA, %d wrongly\n", skipped,
           2 * (0x110000 - 0x800), wrong);
    failures += wrong;
}

/* BidiCharacterTest.txt: code points; paragraph direction; paragraph level; levels; visual order */
static void run_bidi(const char* dir) {
    char path[1024];
    static char line[65536];
    FILE* fp;
    int pass = 0, fail = 0, skip_wrong = 0;

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
        skip_wrong += bidi_skip_wrong(cp, n, pdir == 2 ? -1 : pdir, para, lev);
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
    printf("BidiCharacterTest.txt: %d paragraphs wrongly skip the UBA\n", skip_wrong);
    failures += fail + skip_wrong;
    run_bidi_skip();
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "build/ucd";

    verbose = argc > 2 && !strcmp(argv[2], "-v");
    run(dir, "LineBreakTest.txt", 1);
    run(dir, "GraphemeBreakTest.txt", 0);
    run_bidi(dir);
    return failures ? 1 : 0;
}
