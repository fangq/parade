/*
 * Parade hyphenation: Frank Liang's pattern algorithm (as in TeX)
 *
 * Loads libhyphen/hunspell .dic pattern files (the format LibreOffice and
 * most Linux systems ship, e.g. /usr/share/hyphen/hyph_en_US.dic): an
 * encoding line (UTF-8 or ISO8859-1), optional LEFTHYPHENMIN and
 * RIGHTHYPHENMIN, then one pattern per line ("1ba", ".ach4"). Patterns
 * are kept in a hash table keyed by their letters; a word is hyphenated
 * by matching every substring of ".word." against it and taking the
 * largest digit at each position, odd digits allowing a break.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

#define MAX_PAT 32

typedef struct {
    uint32_t key[MAX_PAT];      /* letters, '.' for word boundary */
    uint8_t len;
    uint8_t digits[MAX_PAT + 1];
    int32_t next;               /* hash chain */
} hpat;

struct pd_hyph {
    hpat* pats;
    int32_t n, cap;
    int32_t* buckets;
    int32_t nbuckets;
    int32_t left, right;        /* minimum letters before / after a hyphen */
    int32_t maxlen;
};

static uint32_t hash_key(const uint32_t* k, int32_t n) {
    uint32_t h = 2166136261u;
    int32_t i;

    for (i = 0; i < n; i++) {
        h = (h ^ k[i]) * 16777619u;
    }

    return h;
}

/* lower case for the scripts the shipped patterns cover (Latin, Greek, Cyrillic) */
static uint32_t lower(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) {
        return c + 32;
    }

    if (c >= 0x100 && c < 0x180 && !(c & 1)) {
        return c + 1;   /* Latin Extended-A pairs */
    }

    if ((c >= 0x391 && c <= 0x3A9) || (c >= 0x410 && c <= 0x42F)) {
        return c + 32;
    }

    if (c >= 0x400 && c <= 0x40F) {
        return c + 80;
    }

    return c;
}

static int add_pattern(pd_hyph* h, const uint32_t* cps, int32_t n) {
    hpat p;
    int32_t i;
    uint32_t b;

    memset(&p, 0, sizeof(p));

    for (i = 0; i < n; i++) {
        if (cps[i] >= '0' && cps[i] <= '9') {
            p.digits[p.len] = (uint8_t)(cps[i] - '0');
        } else if (p.len < MAX_PAT) {
            p.key[p.len++] = lower(cps[i]);
        } else {
            return 0;   /* longer than any real pattern: ignore */
        }
    }

    if (p.len == 0 || pd_grow((void**)&h->pats, &h->cap, (int64_t)h->n + 1, sizeof(hpat))) {
        return p.len == 0 ? 0 : -1;
    }

    b = hash_key(p.key, p.len) % (uint32_t)h->nbuckets;
    p.next = h->buckets[b];
    h->buckets[b] = h->n;
    h->pats[h->n++] = p;
    h->maxlen = p.len > h->maxlen ? p.len : h->maxlen;
    return 0;
}

pd_status pd_hyph_load_memory(const void* data, size_t len, pd_hyph** out) {
    const unsigned char* s = (const unsigned char*)data;
    size_t i = 0;
    int latin1 = 0, first = 1;
    pd_hyph* h;
    int32_t k;

    if (!data || !out) {
        return PD_ERR_ARG;
    }

    *out = NULL;
    h = (pd_hyph*)calloc(1, sizeof(pd_hyph));

    if (!h) {
        return PD_ERR_NOMEM;
    }

    h->nbuckets = 16381;
    h->buckets = (int32_t*)malloc((size_t)h->nbuckets * sizeof(int32_t));
    h->left = 2;
    h->right = 3;

    if (!h->buckets) {
        free(h);
        return PD_ERR_NOMEM;
    }

    for (k = 0; k < h->nbuckets; k++) {
        h->buckets[k] = -1;
    }

    while (i < len) {
        size_t e = i;
        uint32_t cps[128];
        int32_t n = 0;

        while (e < len && s[e] != '\n' && s[e] != '\r') {
            e++;
        }

        if (first) {        /* the encoding line */
            latin1 = (e - i >= 9 && !memcmp(s + i, "ISO8859-1", 9));
            first = 0;
        } else if (e - i >= 9 && !memcmp(s + i, "NEXTLEVEL", 9)) {
            break;          /* compound-word levels are not used */
        } else if (e - i > 14 && !memcmp(s + i, "LEFTHYPHENMIN ", 14)) {
            h->left = atoi((const char*)s + i + 14);
        } else if (e - i > 15 && !memcmp(s + i, "RIGHTHYPHENMIN ", 15)) {
            h->right = atoi((const char*)s + i + 15);
        } else if (e > i && s[i] != '%' && s[i] != '#' && !memchr(s + i, '/', e - i) && !memchr(s + i, ' ', e - i)) {
            size_t q = i;

            while (q < e && n < 128) {      /* decode the line */
                uint32_t c = s[q];
                int extra = latin1 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

                if (extra) {
                    c &= 0x3F >> extra;
                }

                q++;

                while (extra-- > 0 && q < e) {
                    c = (c << 6) | (s[q++] & 0x3F);
                }

                cps[n++] = c;
            }

            if (add_pattern(h, cps, n)) {
                pd_hyph_free(h);
                return PD_ERR_NOMEM;
            }
        }

        i = e;

        while (i < len && (s[i] == '\n' || s[i] == '\r')) {
            i++;
        }
    }

    if (h->n == 0) {
        pd_hyph_free(h);
        return PD_ERR_FORMAT;
    }

    *out = h;
    return PD_OK;
}

pd_status pd_hyph_load_file(const char* path, pd_hyph** out) {
    FILE* fp;
    long n;
    void* buf;
    pd_status st;

    if (!path || !out) {
        return PD_ERR_ARG;
    }

    fp = fopen(path, "rb");

    if (!fp) {
        return PD_ERR_IO;
    }

    if (fseek(fp, 0, SEEK_END) || (n = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return PD_ERR_IO;
    }

    buf = malloc((size_t)n);

    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return PD_ERR_IO;
    }

    fclose(fp);
    st = pd_hyph_load_memory(buf, (size_t)n, out);
    free(buf);
    return st;
}

void pd_hyph_free(pd_hyph* h) {
    if (h) {
        free(h->pats);
        free(h->buckets);
        free(h);
    }
}

/* points[i] = 1 if a hyphen may go before letter i of a word of n letters */
int pd_hyph_points(const pd_hyph* h, const uint32_t* word, int32_t n, uint8_t* points) {
    uint32_t w[256];
    uint8_t v[258];
    int32_t m = n + 2, i, j, k;

    memset(points, 0, (size_t)n + 1);

    if (n < h->left + h->right || n > 250) {
        return 0;
    }

    w[0] = '.';

    for (i = 0; i < n; i++) {
        w[i + 1] = lower(word[i]);
    }

    w[n + 1] = '.';
    memset(v, 0, sizeof(v));

    for (i = 0; i < m; i++) {
        for (j = 1; j <= h->maxlen && i + j <= m; j++) {
            int32_t q = h->buckets[hash_key(w + i, j) % (uint32_t)h->nbuckets];

            for (; q >= 0; q = h->pats[q].next) {
                const hpat* p = &h->pats[q];

                if (p->len == j && !memcmp(p->key, w + i, (size_t)j * sizeof(uint32_t))) {
                    for (k = 0; k <= j; k++) {
                        if (p->digits[k] > v[i + k]) {
                            v[i + k] = p->digits[k];
                        }
                    }
                }
            }
        }
    }

    /* v[k] sits between w[k-1] and w[k]; letter i of the word is w[i+1] */
    for (i = h->left; i <= n - h->right; i++) {
        points[i] = v[i + 1] & 1;
    }

    return 0;
}

pd_status pd_hyph_word(const pd_hyph* h, const char* utf8, size_t len, uint8_t* out) {
    uint32_t* cps, *offs;
    uint8_t pts[256];
    int32_t n, i;

    if (!h || !out || (!utf8 && len)) {
        return PD_ERR_ARG;
    }

    memset(out, 0, len + 1);
    n = pd_text_decode(utf8 ? utf8 : "", len, &cps, &offs);

    if (n < 0) {
        return PD_ERR_NOMEM;
    }

    if (n <= 250) {
        pd_hyph_points(h, cps, n, pts);

        for (i = 1; i < n; i++) {
            out[offs[i]] = pts[i];
        }
    }

    free(cps);
    free(offs);
    return PD_OK;
}
