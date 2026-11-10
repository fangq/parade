/*
 * Parade zlib streams: deflate and inflate
 *
 * A dependency-free implementation for PDF streams, PNG data, DOCX (zip)
 * parts and the resources of a saved document. The decompressor handles
 * stored, fixed and dynamic Huffman blocks and is bounds-checked for
 * untrusted input.
 *
 * The compressor is mimamo's (mmm_zlib.hpp, by the same author, BSD),
 * ported to C: hash chains over a 32 KiB window, matched lazily (zlib's
 * level 6 settings), each block written with whichever of dynamic Huffman
 * codes, the fixed codes or a stored block is smallest. An input larger
 * than one zlibmt block (4 MiB) is compressed as zlibmt does it -- zmat's,
 * pyjdata's, mimamo's: cut into blocks, each deflated on a thread of its
 * own and ended with a full flush (an empty stored block) so that none
 * refers back into another, joined behind one header with an empty final
 * block and one Adler-32 over the whole -- an ordinary zlib stream any
 * inflate reads. The bytes depend on the data alone (not on the number of
 * threads), so a save is the same everywhere.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_paint.h"

#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* deflate                                                            */
/* ------------------------------------------------------------------ */

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258
                                     };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,
                                       5, 5, 0
                                     };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
                                        769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
                                      };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                        11, 11, 12, 12, 13, 13
                                      };
static const uint8_t cl_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/* the symbols the compressor looks up: made once, before any thread starts */
static struct {
    int ready;
    uint8_t len_sym[259];       /* a match length to its code, 0 to 28 */
    uint8_t dist_lo[256];       /* distance - 1 below 256 to its code */
    uint8_t dist_hi[256];       /* (distance - 1) >> 7 above */
    uint8_t fixed_len[288];
    uint16_t fixed_code[288];
    uint8_t fixed_dlen[30];
    uint16_t fixed_dcode[30];
} T;

static uint32_t reverse_bits(uint32_t code, int len) {
    uint32_t r = 0;
    int i;

    for (i = 0; i < len; i++) {
        r = (r << 1) | (code & 1);
        code >>= 1;
    }

    return r;
}

/* canonical codes for a set of lengths, bit-reversed for writing */
static void huff_codes(const uint8_t* len, int n, uint16_t* code) {
    uint16_t count[16] = { 0 }, next[16] = { 0 };
    uint32_t c = 0;
    int i, b;

    for (i = 0; i < n; i++) {
        count[len[i]]++;
    }

    count[0] = 0;

    for (b = 1; b < 16; b++) {
        c = (c + count[b - 1]) << 1;
        next[b] = (uint16_t)c;
    }

    for (i = 0; i < n; i++) {
        code[i] = len[i] ? (uint16_t)reverse_bits(next[len[i]]++, len[i]) : 0;
    }
}

static void tables_init(void) {
    int c, l, d, i;

    if (T.ready) {
        return;
    }

    for (c = 0; c < 28; c++) {
        for (l = len_base[c]; l < len_base[c] + (1 << len_extra[c]); l++) {
            T.len_sym[l] = (uint8_t)c;
        }
    }

    T.len_sym[258] = 28;

    for (c = 0; c < 30; c++) {
        for (d = dist_base[c]; d < dist_base[c] + (1 << dist_extra[c]); d++) {
            if (d <= 256) {
                T.dist_lo[d - 1] = (uint8_t)c;
            } else {
                T.dist_hi[(d - 1) >> 7] = (uint8_t)c;
            }
        }
    }

    for (i = 0; i < 288; i++) {
        T.fixed_len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    }

    huff_codes(T.fixed_len, 288, T.fixed_code);

    for (i = 0; i < 30; i++) {
        T.fixed_dlen[i] = 5;
    }

    huff_codes(T.fixed_dlen, 30, T.fixed_dcode);
    T.ready = 1;
}

static int dist_sym(unsigned d) {
    return d <= 256 ? T.dist_lo[d - 1] : T.dist_hi[(d - 1) >> 7];
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;

    return x < y ? -1 : x > y;
}

/* Code lengths of at most `limit` bits for these frequencies: Moffat and Katajainen's in-place method over the
   frequencies in ascending order; a code longer than the limit is repaired by moving the counts down and dealing
   the lengths back out, longest to the rarest. Fewer than two symbols get two one-bit codes. */
static void huff_lengths(const uint32_t* freq, int n, int limit, uint8_t* len) {
    uint64_t key[288];
    int sym[288], count[300] = { 0 };
    uint32_t a[288];
    int m = 0, i, root, leaf, next, avail, used, depth, maxlen = 0, at, l, k;

    memset(len, 0, (size_t)n);

    for (i = 0; i < n; i++) {
        if (freq[i]) {
            key[m++] = (uint64_t)freq[i] << 16 | (uint64_t)i;
        }
    }

    if (m < 2) {
        int s0 = m ? (int)(key[0] & 0xFFFF) : 0;

        len[s0] = 1;
        len[s0 == 0 ? 1 : 0] = 1;
        return;
    }

    qsort(key, (size_t)m, sizeof(key[0]), cmp_u64);

    for (i = 0; i < m; i++) {
        sym[i] = (int)(key[i] & 0xFFFF);
        a[i] = (uint32_t)(key[i] >> 16);
    }

    /* pass one: pair the two smallest of leaves and internal nodes */
    a[0] += a[1];
    root = 0;
    leaf = 2;

    for (next = 1; next < m - 1; next++) {
        if (leaf >= m || a[root] < a[leaf]) {
            a[next] = a[root];
            a[root++] = (uint32_t)next;
        } else {
            a[next] = a[leaf++];
        }

        if (leaf >= m || (root < next && a[root] < a[leaf])) {
            a[next] += a[root];
            a[root++] = (uint32_t)next;
        } else {
            a[next] += a[leaf++];
        }
    }

    /* pass two: every internal node's depth, from the root down */
    a[m - 2] = 0;

    for (next = m - 3; next >= 0; next--) {
        a[next] = a[a[next]] + 1;
    }

    /* pass three: every leaf's depth */
    avail = 1;
    used = 0;
    depth = 0;
    root = m - 2;
    next = m - 1;

    while (avail > 0) {
        while (root >= 0 && (int)a[root] == depth) {
            used++;
            root--;
        }

        while (avail > used) {
            a[next--] = (uint32_t)depth;
            avail--;
        }

        avail = 2 * used;
        depth++;
        used = 0;
    }

    for (i = 0; i < m; i++) {
        count[a[i]]++;
        maxlen = (int)a[i] > maxlen ? (int)a[i] : maxlen;
    }

    if (maxlen > limit) {
        uint64_t total = 0;

        for (l = limit + 1; l <= maxlen; l++) {
            count[limit] += count[l];
            count[l] = 0;
        }

        for (l = 1; l <= limit; l++) {
            total += (uint64_t)count[l] << (limit - l);
        }

        while (total > ((uint64_t)1 << limit)) {
            count[limit]--;

            for (l = limit - 1; l > 0; l--) {
                if (count[l]) {
                    count[l]--;
                    count[l + 1] += 2;
                    break;
                }
            }

            total--;
        }

        maxlen = limit;
    }

    at = 0;

    for (l = maxlen; l > 0; l--) {
        for (k = 0; k < count[l]; k++) {
            len[sym[at++]] = (uint8_t)l;
        }
    }
}

/* bits out, least significant first */
typedef struct {
    unsigned char* p;
    size_t n, cap;
    uint64_t acc;
    int nb;
    int err;
} bitw;

static unsigned char* bw_grow(bitw* w, size_t k) {
    if (w->n + k > w->cap) {
        size_t nc = w->cap ? w->cap : 1024;
        unsigned char* q;

        while (nc < w->n + k) {
            nc *= 2;
        }

        if ((q = (unsigned char*)realloc(w->p, nc)) == NULL) {
            w->err = 1;
            return NULL;
        }

        w->p = q;
        w->cap = nc;
    }

    w->n += k;
    return w->p + w->n - k;
}

static void bw_append(bitw* w, const void* p, size_t k) {
    unsigned char* q = bw_grow(w, k);

    if (q && k) {
        memcpy(q, p, k);
    }
}

/* k bits of v (at most 32) */
static void bw_put(bitw* w, uint32_t v, int k) {
    w->acc |= (uint64_t)v << w->nb;
    w->nb += k;

    if (w->nb >= 32) {
        unsigned char* q = bw_grow(w, 4);

        if (q) {
            q[0] = (unsigned char)w->acc;
            q[1] = (unsigned char)(w->acc >> 8);
            q[2] = (unsigned char)(w->acc >> 16);
            q[3] = (unsigned char)(w->acc >> 24);
        }

        w->acc >>= 32;
        w->nb -= 32;
    }
}

static void bw_align(bitw* w) {
    while (w->nb > 0) {
        unsigned char* q = bw_grow(w, 1);

        if (q) {
            *q = (unsigned char)w->acc;
        }

        w->acc >>= 8;
        w->nb -= 8;
    }

    w->acc = 0;
    w->nb = 0;
}

/* zlib's level 6: a match this long already searches a quarter as far (good), no lazy search beyond lazy, stop at
   nice, follow at most chain links */
#define LV_GOOD 8
#define LV_LAZY 16
#define LV_NICE 128
#define LV_CHAIN 128

#define HBITS 15
#define HSIZE ((size_t)1 << HBITS)
#define WSIZE ((size_t)32768)
#define WMASK (WSIZE - 1)
#define SYMMAX ((size_t)32768)
#define TOO_FAR 4096

typedef struct {
    uint32_t* head;             /* position + 1 of the newest with this hash */
    uint32_t* prev;             /* position + 1 of the one before, by position */
    uint32_t* syms;             /* (length or literal) << 16 | distance */
    size_t nsym;
    uint32_t lfreq[286], dfreq[30];
    const unsigned char* p;
    size_t n, start, emitted;   /* the first byte of the open block; the first not yet in a symbol */
} dfl;

static int dfl_init(dfl* D) {
    memset(D, 0, sizeof(*D));
    D->head = (uint32_t*)malloc(HSIZE * sizeof(uint32_t));
    D->prev = (uint32_t*)malloc(WSIZE * sizeof(uint32_t));
    D->syms = (uint32_t*)malloc(SYMMAX * sizeof(uint32_t));
    return D->head && D->prev && D->syms ? 0 : -1;
}

static void dfl_free(dfl* D) {
    free(D->head);
    free(D->prev);
    free(D->syms);
}

static void stored(bitw* w, const unsigned char* p, size_t n, int last) {
    size_t at = 0;

    do {
        size_t k = n - at < 65535 ? n - at : 65535;
        unsigned char* q;

        bw_put(w, last && at + k >= n ? 1 : 0, 1);
        bw_put(w, 0, 2);
        bw_align(w);

        if ((q = bw_grow(w, 4)) != NULL) {
            q[0] = (unsigned char)k;
            q[1] = (unsigned char)(k >> 8);
            q[2] = (unsigned char)~k;
            q[3] = (unsigned char)(~k >> 8);
        }

        bw_append(w, p + at, k);
        at += k;
    } while (at < n);
}

static uint32_t dhash(const dfl* D, size_t pos) {
    uint32_t v = (uint32_t)D->p[pos] | ((uint32_t)D->p[pos + 1] << 8) | ((uint32_t)D->p[pos + 2] << 16);

    return (v * 2654435761u) >> (32 - HBITS);
}

/* pos put on its chain; the chain's previous head, as position + 1 */
static uint32_t dinsert(dfl* D, size_t pos) {
    uint32_t h = dhash(D, pos), old = D->head[h];

    D->prev[pos & WMASK] = old;
    D->head[h] = (uint32_t)(pos + 1);
    return old;
}

static size_t common(const unsigned char* a, const unsigned char* b, size_t max) {
    size_t l = 0;
#if defined(__GNUC__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

    while (l + 8 <= max) {
        uint64_t x, y;

        memcpy(&x, a + l, 8);
        memcpy(&y, b + l, 8);

        if (x != y) {
            return l + (size_t)(__builtin_ctzll(x ^ y) >> 3);
        }

        l += 8;
    }

#endif

    while (l < max && a[l] == b[l]) {
        l++;
    }

    return l;
}

/* the longest match at pos longer than `better`, following the chain from cand; 0 when there is none */
static size_t longest(const dfl* D, size_t pos, uint32_t cand, size_t better, size_t* dist) {
    size_t max = D->n - pos < 258 ? D->n - pos : 258, nice, best = better, found = 0;
    const unsigned char* s = D->p + pos;
    unsigned chain = LV_CHAIN;

    if (max < 3 || better >= max) {
        return 0;
    }

    nice = LV_NICE < max ? LV_NICE : max;

    if (better >= LV_GOOD) {
        chain >>= 2;
    }

    while (cand) {
        size_t c = cand - 1;
        const unsigned char* t = D->p + c;
        uint32_t nx;

        if (pos - c > WSIZE) {
            break;
        }

        if (t[best] == s[best] && t[0] == s[0] && t[1] == s[1]) {
            size_t l = common(t, s, max);

            if (l > best) {
                best = l;
                found = pos - c;

                if (l >= nice) {
                    break;
                }
            }
        }

        if (--chain == 0) {
            break;
        }

        nx = D->prev[c & WMASK];

        if (nx == 0 || nx - 1 >= c) {
            break;
        }

        cand = nx;
    }

    if (!found) {
        return 0;
    }

    *dist = found;
    return best;
}

static void literal(dfl* D, size_t pos) {
    unsigned char b = D->p[pos];

    D->syms[D->nsym++] = (uint32_t)b << 16;
    D->lfreq[b]++;
    D->emitted = pos + 1;
}

static void match(dfl* D, size_t pos, size_t len, size_t dist) {
    D->syms[D->nsym++] = ((uint32_t)len << 16) | (uint32_t)dist;
    D->lfreq[257 + T.len_sym[len]]++;
    D->dfreq[dist_sym((unsigned)dist)]++;
    D->emitted = pos + len;
}

static void symbols(const dfl* D, bitw* w, const uint16_t* lcode, const uint8_t* llen, const uint16_t* dcode,
                    const uint8_t* dlen) {
    size_t i;

    for (i = 0; i < D->nsym; i++) {
        uint32_t s = D->syms[i], v = s >> 16, d = s & 0xFFFF, bits;
        int c, nb, dc;

        if (!d) {
            bw_put(w, lcode[v], llen[v]);
            continue;
        }

        c = T.len_sym[v];
        bits = lcode[257 + c];
        nb = llen[257 + c];

        if (len_extra[c]) {
            bits |= (v - len_base[c]) << nb;
            nb += len_extra[c];
        }

        bw_put(w, bits, nb);
        dc = dist_sym(d);
        bw_put(w, dcode[dc] | ((d - dist_base[dc]) << dlen[dc]), dlen[dc] + dist_extra[dc]);
    }

    bw_put(w, lcode[256], llen[256]);
}

/* the collected symbols written as one block, in its cheapest form */
static void block(dfl* D, bitw* w, int last) {
    uint8_t llen[286], dlen[30], all[316], rle[316], rlx[316], cllen[19];
    uint32_t clfreq[19] = { 0 };
    int hlit = 286, hdist = 30, total, nr = 0, i, hclen = 19, s, c;
    uint64_t extra = 0, dyn, fix, sto, chunks;
    size_t raw;

    D->lfreq[256] = 1;
    huff_lengths(D->lfreq, 286, 15, llen);
    huff_lengths(D->dfreq, 30, 15, dlen);

    while (hlit > 257 && llen[hlit - 1] == 0) {
        hlit--;
    }

    while (hdist > 1 && dlen[hdist - 1] == 0) {
        hdist--;
    }

    /* the code lengths, run-length coded with 16, 17 and 18 */
    memcpy(all, llen, (size_t)hlit);
    memcpy(all + hlit, dlen, (size_t)hdist);
    total = hlit + hdist;

#define EMIT(sy, x) do { rle[nr] = (uint8_t)(sy); rlx[nr++] = (uint8_t)(x); clfreq[sy]++; } while (0)

    for (i = 0; i < total;) {
        uint8_t cur = all[i];
        int run = 1;

        while (i + run < total && all[i + run] == cur) {
            run++;
        }

        i += run;

        if (cur == 0) {
            while (run >= 11) {
                int r = run < 138 ? run : 138;

                EMIT(18, r - 11);
                run -= r;
            }

            if (run >= 3) {
                EMIT(17, run - 3);
                run = 0;
            }
        } else {
            EMIT(cur, 0);
            run--;

            while (run >= 3) {
                int r = run < 6 ? run : 6;

                EMIT(16, r - 3);
                run -= r;
            }
        }

        while (run-- > 0) {
            EMIT(cur, 0);
        }
    }

#undef EMIT

    huff_lengths(clfreq, 19, 7, cllen);

    while (hclen > 4 && cllen[cl_order[hclen - 1]] == 0) {
        hclen--;
    }

    /* the bits each form would take */
    for (c = 0; c < 29; c++) {
        extra += (uint64_t)D->lfreq[257 + c] * len_extra[c];
    }

    for (c = 0; c < 30; c++) {
        extra += (uint64_t)D->dfreq[c] * dist_extra[c];
    }

    dyn = 17 + 3 * (uint64_t)hclen + extra;
    fix = 3 + extra;

    for (s = 0; s < 19; s++) {
        dyn += (uint64_t)clfreq[s] * cllen[s];
    }

    dyn += 2 * (uint64_t)clfreq[16] + 3 * (uint64_t)clfreq[17] + 7 * (uint64_t)clfreq[18];

    for (s = 0; s < 286; s++) {
        dyn += (uint64_t)D->lfreq[s] * llen[s];
        fix += (uint64_t)D->lfreq[s] * T.fixed_len[s];
    }

    for (s = 0; s < 30; s++) {
        dyn += (uint64_t)D->dfreq[s] * dlen[s];
        fix += (uint64_t)D->dfreq[s] * 5;
    }

    raw = D->emitted - D->start;
    chunks = (raw + 65534) / 65535 > 1 ? (raw + 65534) / 65535 : 1;
    sto = chunks * 35 + 7 + 8 * (uint64_t)raw;

    if (raw > 0 && sto < dyn && sto < fix) {
        stored(w, D->p + D->start, raw, last);
    } else if (fix <= dyn) {
        bw_put(w, last ? 1 : 0, 1);
        bw_put(w, 1, 2);
        symbols(D, w, T.fixed_code, T.fixed_len, T.fixed_dcode, T.fixed_dlen);
    } else {
        static const int xbits[3] = { 2, 3, 7 };
        uint16_t lcode[286], dcode[30], clcode[19];

        huff_codes(llen, 286, lcode);
        huff_codes(dlen, 30, dcode);
        huff_codes(cllen, 19, clcode);
        bw_put(w, last ? 1 : 0, 1);
        bw_put(w, 2, 2);
        bw_put(w, (uint32_t)(hlit - 257), 5);
        bw_put(w, (uint32_t)(hdist - 1), 5);
        bw_put(w, (uint32_t)(hclen - 4), 4);

        for (i = 0; i < hclen; i++) {
            bw_put(w, cllen[cl_order[i]], 3);
        }

        for (i = 0; i < nr; i++) {
            bw_put(w, clcode[rle[i]], cllen[rle[i]]);

            if (rle[i] >= 16) {
                bw_put(w, rlx[i], xbits[rle[i] - 16]);
            }
        }

        symbols(D, w, lcode, llen, dcode, dlen);
    }

    D->nsym = 0;
    D->start = D->emitted;
    memset(D->lfreq, 0, sizeof(D->lfreq));
    memset(D->dfreq, 0, sizeof(D->dfreq));
}

/* p[0, n) deflated as complete blocks, lazily matched: a match found at pos - 1 waits a byte, and is written only
   when the match at pos is no longer. Last: the final block carries BFINAL; else the run ends with a full flush,
   so what follows starts afresh on a byte boundary */
static void dfl_run(dfl* D, const unsigned char* p, size_t n, bitw* w, int last) {
    size_t pos = 0, plen = 0, pdist = 0;
    int pending = 0;

    memset(D->head, 0, HSIZE * sizeof(uint32_t));
    D->p = p;
    D->n = n;
    D->nsym = 0;
    D->start = D->emitted = 0;
    memset(D->lfreq, 0, sizeof(D->lfreq));
    memset(D->dfreq, 0, sizeof(D->dfreq));

    while (pos < n) {
        uint32_t cand = pos + 3 <= n ? dinsert(D, pos) : 0;
        size_t len = 0, dist = 0;

        if (cand && plen < LV_LAZY) {
            len = longest(D, pos, cand, plen > 2 ? plen : 2, &dist);

            if (len == 3 && dist > TOO_FAR) {
                len = 0;
            }
        }

        if (pending && plen >= 3 && len <= plen) {
            size_t end = pos - 1 + plen, k;

            match(D, pos - 1, plen, pdist);

            for (k = pos + 1; k < end && k + 3 <= n; k++) {
                dinsert(D, k);
            }

            pos = end;
            pending = 0;
            plen = 0;
        } else {
            if (pending) {
                literal(D, pos - 1);
            }

            pending = 1;
            plen = len;
            pdist = dist;
            pos++;
        }

        if (D->nsym >= SYMMAX - 2) {
            block(D, w, 0);
        }
    }

    if (pending) {
        literal(D, n - 1);
    }

    if (D->nsym || last) {
        block(D, w, last);
    }

    if (!last) {    /* a full flush: an empty stored block */
        static const unsigned char marker[4] = { 0, 0, 0xFF, 0xFF };

        bw_put(w, 0, 3);
        bw_align(w);
        bw_append(w, marker, 4);
    }
}

static uint32_t adler32_part(uint32_t adler, const unsigned char* p, size_t n) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;

    while (n) {
        size_t k = n < 5552 ? n : 5552;    /* the most bytes before b can pass 2^32 */

        n -= k;

        while (k--) {
            a += *p++;
            b += a;
        }

        a %= 65521;
        b %= 65521;
    }

    return (b << 16) | a;
}

/* the Adler-32 of A followed by B (len2 bytes) */
static uint32_t adler32_combine(uint32_t adler1, uint32_t adler2, uint64_t len2) {
    const uint64_t M = 65521;
    uint64_t a1 = adler1 & 0xFFFF, b1 = adler1 >> 16, a2 = adler2 & 0xFFFF, b2 = adler2 >> 16;
    uint64_t a = (a1 + a2 + M - 1) % M, b = (b1 + b2 + (len2 % M) * ((a1 + M - 1) % M)) % M;

    return (uint32_t)((b << 16) | a);
}

/* ------------------------------------------------------------------ */
/* the system zlib, opened at run time                                */
/* ------------------------------------------------------------------ */

/* As mimamo does: when libz.so.1 (libz.dylib, zlib1.dll) can be opened at run time it does zlibmt's per-block
   deflate and the inflating; nothing links against it, and PD_NO_LIBZ=1 in the environment forces the code here.
   The plain deflate of an input of one zlibmt block or less is always this file's, so what most saves write does
   not depend on what is installed. */

typedef struct {
    const unsigned char* next_in;
    unsigned avail_in;
    unsigned long total_in;
    unsigned char* next_out;
    unsigned avail_out;
    unsigned long total_out;
    const char* msg;
    void* state;
    void* zalloc;
    void* zfree;
    void* opaque;
    int data_type;
    unsigned long adler;
    unsigned long reserved;
} zstream;     /* zlib's z_stream: its layout is part of zlib's ABI */

#define Z_NO_FLUSH 0
#define Z_FULL_FLUSH 3
#define Z_BLOCK 5
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_BUF_ERROR (-5)

static struct {
    int ok;
    char version[32];
    const char* (*zlibVersion)(void);
    int (*deflateInit2_)(zstream*, int, int, int, int, int, const char*, int);
    int (*deflate)(zstream*, int);
    int (*deflateEnd)(zstream*);
    int (*deflateReset)(zstream*);
    unsigned long (*deflateBound)(zstream*, unsigned long);
    int (*inflateInit2_)(zstream*, int, const char*, int);
    int (*inflate)(zstream*, int);
    int (*inflateEnd)(zstream*);
    int (*inflateSetDictionary)(zstream*, const unsigned char*, unsigned);
    int (*inflatePrime)(zstream*, int, int);
} Z;

static void* zsym(void* lib, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

/* open the system zlib and resolve what is used here (once) */
static void libz_open(void) {
    static const char* const names[] = {
#if defined(_WIN32)
        "zlib1.dll", "zlib.dll",
#elif defined(__APPLE__)
        "libz.1.dylib", "libz.dylib",
#else
        "libz.so.1", "libz.so",
#endif
        NULL
    };
    void* lib = NULL;
    const char* v;
    int i;

    for (i = 0; names[i] && !lib; i++) {
#ifdef _WIN32
        lib = (void*)LoadLibraryA(names[i]);
#else
        lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);
#endif
    }

    if (!lib) {
        return;
    }

    /* a function pointer from a data pointer, as POSIX allows dlsym's to be used */
#define ZLOAD(f) do { void* s_ = zsym(lib, #f); memcpy(&Z.f, &s_, sizeof(s_)); if (!s_) return; } while (0)
    ZLOAD(zlibVersion);
    ZLOAD(deflateInit2_);
    ZLOAD(deflate);
    ZLOAD(deflateEnd);
    ZLOAD(deflateReset);
    ZLOAD(deflateBound);
    ZLOAD(inflateInit2_);
    ZLOAD(inflate);
    ZLOAD(inflateEnd);
    ZLOAD(inflateSetDictionary);
    ZLOAD(inflatePrime);
#undef ZLOAD
    v = Z.zlibVersion();

    if (!v || v[0] != '1') {    /* deflateInit2_ refuses another major version anyway */
        return;
    }

    strncpy(Z.version, v, sizeof(Z.version) - 1);
    Z.ok = 1;
}

#ifdef _WIN32
static INIT_ONCE libz_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK libz_once_fn(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o;
    (void)p;
    (void)c;
    libz_open();
    return TRUE;
}
#else
static pthread_once_t libz_once = PTHREAD_ONCE_INIT;
#endif

/* 1 when the system zlib is loaded and not refused (PD_NO_LIBZ) */
static int libz_use(void) {
    const char* e = getenv("PD_NO_LIBZ");

    if (e && *e && strcmp(e, "0") != 0) {
        return 0;
    }

#ifdef _WIN32
    InitOnceExecuteOnce(&libz_once, libz_once_fn, NULL, NULL);
#else
    pthread_once(&libz_once, libz_open);
#endif
    return Z.ok;
}

const char* pd_zlib_engine(void) {
    static char what[48];

    if (!libz_use()) {
        return "built-in";
    }

    snprintf(what, sizeof(what), "system zlib %s", Z.version);
    return what;
}

/* the system zlib's raw deflate of one zlibmt block: p[0, n), then a full flush, appended to w (the input in
   with Z_NO_FLUSH, the flush on its own, as pyjdata's zlibmt does it, so that the two give the same bytes) */
static int libz_block(zstream* z, const unsigned char* p, size_t n, bitw* w) {
    size_t at = 0;
    int flush = Z_NO_FLUSH;

    if (Z.deflateReset(z) != Z_OK) {
        return -1;
    }

    for (;;) {
        unsigned char* q;
        size_t room;
        int r;

        if (z->avail_in == 0 && at < n && flush == Z_NO_FLUSH) {
            size_t k = n - at < ((size_t)1 << 30) ? n - at : (size_t)1 << 30;

            z->next_in = p + at;
            z->avail_in = (unsigned)k;
            at += k;
        }

        if (z->avail_in == 0 && at >= n) {
            flush = Z_FULL_FLUSH;
        }

        room = 65536 + (flush == Z_NO_FLUSH ? z->avail_in / 2 : 0);

        if ((q = bw_grow(w, room)) == NULL) {
            return -1;
        }

        z->next_out = q;
        z->avail_out = (unsigned)room;
        r = Z.deflate(z, flush);
        w->n -= z->avail_out;   /* what it did not use */

        if (r != Z_OK && r != Z_BUF_ERROR) {
            return -1;
        }

        if (flush == Z_FULL_FLUSH && z->avail_out != 0) {
            return 0;   /* the flush is out whole */
        }
    }
}

/* ------------------------------------------------------------------ */
/* zlibmt                                                             */
/* ------------------------------------------------------------------ */

#define MT_BLOCK ((size_t)4 << 20)

typedef struct {
    const unsigned char* in;
    size_t n, nb;
    bitw* seg;
    uint32_t* sum;
    size_t next;                /* the next block to take */
    int err;
    int sys;                    /* the system zlib deflates the blocks */
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} mtjob;

static size_t mt_take(mtjob* J) {
    size_t i;

#ifdef _WIN32
    EnterCriticalSection(&J->lock);
#else
    pthread_mutex_lock(&J->lock);
#endif
    i = J->next++;
#ifdef _WIN32
    LeaveCriticalSection(&J->lock);
#else
    pthread_mutex_unlock(&J->lock);
#endif
    return i;
}

static void mt_work(mtjob* J) {
    dfl D;
    zstream z;
    size_t i;

    memset(&D, 0, sizeof(D));
    memset(&z, 0, sizeof(z));

    if (J->sys ? Z.deflateInit2_(&z, 6, 8, -15, 8, 0, Z.version, (int)sizeof(zstream)) != Z_OK : dfl_init(&D) != 0) {
        J->err = 1;
        dfl_free(&D);
        return;
    }

    while ((i = mt_take(J)) < J->nb) {
        size_t off = i * MT_BLOCK, len = J->n - off < MT_BLOCK ? J->n - off : MT_BLOCK;

        J->sum[i] = adler32_part(1, J->in + off, len);

        if (J->sys) {
            if (libz_block(&z, J->in + off, len, &J->seg[i])) {
                J->err = 1;
            }
        } else {
            dfl_run(&D, J->in + off, len, &J->seg[i], 0);
        }

        if (J->seg[i].err) {
            J->err = 1;
        }
    }

    if (J->sys) {
        Z.deflateEnd(&z);
    }

    dfl_free(&D);
}

#ifdef _WIN32
static DWORD WINAPI mt_thread(LPVOID arg) {
    mt_work((mtjob*)arg);
    return 0;
}
#else
static void* mt_thread(void* arg) {
    mt_work((mtjob*)arg);
    return NULL;
}
#endif

static int cpu_count(void) {
    const char* e = getenv("PD_ZLIB_THREADS");     /* for tests: a fixed number of threads */
    long n;

    if (e && atoi(e) > 0) {
        return atoi(e);
    }

#ifdef _WIN32
    {
        SYSTEM_INFO si;

        GetSystemInfo(&si);
        n = (long)si.dwNumberOfProcessors;
    }
#else
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? (int)(n < 64 ? n : 64) : 1;
}

/* the blocks of an input past one zlibmt block, deflated on threads and joined behind the header already out */
static int deflate_mt(const unsigned char* in, size_t n, bitw* w, uint32_t* adler, int sys) {
    mtjob J;
    int threads, t, started = 0;
    size_t i;
#ifdef _WIN32
    HANDLE th[64];
#else
    pthread_t th[64];
#endif

    memset(&J, 0, sizeof(J));
    J.in = in;
    J.n = n;
    J.nb = (n + MT_BLOCK - 1) / MT_BLOCK;
    J.sys = sys;
    J.seg = (bitw*)calloc(J.nb, sizeof(bitw));
    J.sum = (uint32_t*)calloc(J.nb, sizeof(uint32_t));

    if (!J.seg || !J.sum) {
        free(J.seg);
        free(J.sum);
        return -1;
    }

#ifdef _WIN32
    InitializeCriticalSection(&J.lock);
#else
    pthread_mutex_init(&J.lock, NULL);
#endif
    threads = cpu_count();
    threads = (size_t)threads < J.nb ? threads : (int)J.nb;

    for (t = 1; t < threads; t++) {     /* this thread is one of them */
#ifdef _WIN32
        if ((th[started] = CreateThread(NULL, 0, mt_thread, &J, 0, NULL)) != NULL) {
            started++;
        }
#else
        if (pthread_create(&th[started], NULL, mt_thread, &J) == 0) {
            started++;
        }
#endif
    }

    mt_work(&J);

    for (t = 0; t < started; t++) {
#ifdef _WIN32
        WaitForSingleObject(th[t], INFINITE);
        CloseHandle(th[t]);
#else
        pthread_join(th[t], NULL);
#endif
    }

#ifdef _WIN32
    DeleteCriticalSection(&J.lock);
#else
    pthread_mutex_destroy(&J.lock);
#endif

    for (i = 0; i < J.nb && !J.err; i++) {
        bw_append(w, J.seg[i].p, J.seg[i].n);
        *adler = i ? adler32_combine(*adler, J.sum[i], J.n - i * MT_BLOCK < MT_BLOCK ? J.n - i * MT_BLOCK : MT_BLOCK) :
                 J.sum[0];
    }

    for (i = 0; i < J.nb; i++) {
        free(J.seg[i].p);
    }

    free(J.seg);
    free(J.sum);

    if (J.err) {
        return -1;
    }

    {   /* an empty, final, fixed block */
        static const unsigned char tail[2] = { 0x03, 0x00 };

        bw_append(w, tail, 2);
    }

    return 0;
}

/* deflate of data (raw; zlib = 1 adds the zlib header and Adler-32 trailer): one stream, its blocks on threads
   when it is larger than one zlibmt block */
int pd_deflate(const void* data, size_t n, int zlib, unsigned char** out, size_t* outlen) {
    const unsigned char* in = (const unsigned char*)data;
    bitw w;
    uint32_t a = 1;

    tables_init();
    memset(&w, 0, sizeof(w));

    if (zlib) {
        unsigned char* q = bw_grow(&w, 2);

        if (q) {    /* deflate, a 32 KiB window, level 6's flag, and the check bits */
            q[0] = 0x78;
            q[1] = 0x9C;
        }
    }

    if (n > MT_BLOCK) {
        size_t head = w.n;

        /* the system zlib's blocks when it is there; should it fail, this file's */
        if (deflate_mt(in, n, &w, &a, libz_use()) && (w.n = head, w.err = 0, deflate_mt(in, n, &w, &a, 0))) {
            free(w.p);
            return -1;
        }
    } else {
        dfl D;

        if (dfl_init(&D)) {
            dfl_free(&D);
            free(w.p);
            return -1;
        }

        dfl_run(&D, in, n, &w, 1);
        bw_align(&w);
        dfl_free(&D);
        a = zlib ? adler32_part(1, in, n) : 1;
    }

    if (zlib) {
        unsigned char* q = bw_grow(&w, 4);

        if (q) {
            q[0] = (unsigned char)(a >> 24);
            q[1] = (unsigned char)(a >> 16);
            q[2] = (unsigned char)(a >> 8);
            q[3] = (unsigned char)a;
        }
    }

    if (w.err) {
        free(w.p);
        return -1;
    }

    *out = w.p;
    *outlen = w.n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* inflate                                                            */
/* ------------------------------------------------------------------ */

/* mimamo's inflate, ported: table driven -- a ten-bit first-level table for literals and lengths, eight bits for
   distances, second-level tables for longer codes -- read from a 64-bit bit buffer refilled eight bytes at a time.

   And in parallel where it can be proven: a full flush leaves no reliable mark (00 00 FF FF also occurs by chance
   inside compressed data), so the stream is split among threads, each looks for the first mark in its share and
   inflates from just after it with no history, stopping at the first block boundary past the next share's start.
   A start is accepted only when the inflate before it, itself accepted, ends exactly there at a block boundary:
   the serial parse then provably reaches the same point, and as the speculative inflate refused any
   back-reference before its own start, its output is what a serial one makes. A chance match, or a stream with
   no full flushes, fails that test, and the gap is inflated serially, with the history in place. The Adler-32 is
   checked over the whole either way. So any stream inflates correctly, and a zlibmt one on every core. */

enum { OP_LIT = 0, OP_LEN = 1, OP_EOB = 2, OP_SUB = 3, OP_BAD = 4 };

/* table entries: bits 0-3 the code bits to drop, 4-7 the kind, 8-15 the extra bits (or a second-level table's
   index bits), 16-31 the literal, base length or distance, or the second-level table's offset */
#define ENTRY(op, bits, extra, value) (((uint32_t)(value) << 16) | ((uint32_t)(extra) << 8) | ((uint32_t)(op) << 4) | \
                                       (uint32_t)(bits))

#define LIT_ROOT 10
#define DIST_ROOT 8
#define LIT_SIZE (1024 + 286 * 32)  /* the root and, at most, a 32-entry table for every long code */
#define DIST_SIZE (256 + 30 * 128)

typedef struct {
    uint32_t* t;
    int root, size;
} htab;

static int lengths_ok(const uint8_t* len, int n) {
    int count[16] = { 0 }, left = 1, i, b;

    for (i = 0; i < n; i++) {
        count[len[i]]++;
    }

    for (b = 1; b < 16; b++) {
        left = (left << 1) - count[b];

        if (left < 0) {
            return 0;
        }
    }

    return 1;
}

/* a decoding table from code lengths (dist: the distance alphabet); 0 for an over-subscribed set. An incomplete one
   is accepted: its unused codes decode as errors */
static int htab_build(const uint8_t* len, int n, int root, int dist, htab* h, int cap) {
    int count[16] = { 0 }, i, b, s;
    uint32_t next[16] = { 0 }, c = 0, rsize = 1u << root, rmask = rsize - 1, pre, k;
    uint8_t subbits[1 << 10];
    uint32_t rev[320];

    if (!lengths_ok(len, n)) {
        return 0;
    }

    for (i = 0; i < n; i++) {
        count[len[i]]++;
    }

    count[0] = 0;

    for (b = 1; b < 16; b++) {
        c = (c + (uint32_t)count[b - 1]) << 1;
        next[b] = c;
    }

    memset(subbits, 0, sizeof(subbits));
    h->root = root;
    h->size = (int)rsize;

    for (k = 0; k < rsize; k++) {
        h->t[k] = ENTRY(OP_BAD, 0, 0, 0);
    }

    for (s = 0; s < n; s++) {
        if (len[s]) {
            rev[s] = reverse_bits(next[len[s]]++, len[s]);

            if (len[s] > root) {
                pre = rev[s] & rmask;
                subbits[pre] = (uint8_t)(len[s] - root) > subbits[pre] ? (uint8_t)(len[s] - root) : subbits[pre];
            }
        }
    }

    for (pre = 0; pre < rsize; pre++) {
        if (subbits[pre]) {
            uint32_t sz = 1u << subbits[pre];

            if (h->size + (int)sz > cap) {
                return 0;
            }

            h->t[pre] = ENTRY(OP_SUB, root, subbits[pre], h->size);

            for (k = 0; k < sz; k++) {
                h->t[h->size + (int)k] = ENTRY(OP_BAD, 0, 0, 0);
            }

            h->size += (int)sz;
        }
    }

    for (s = 0; s < n; s++) {
        int l = len[s];
        uint32_t op = OP_BAD, extra = 0, value = 0;

        if (!l) {
            continue;
        }

        if (dist) {
            if (s < 30) {
                op = OP_LEN;
                extra = dist_extra[s];
                value = dist_base[s];
            }
        } else if (s < 256) {
            op = OP_LIT;
            value = (uint32_t)s;
        } else if (s == 256) {
            op = OP_EOB;
        } else if (s < 286) {
            op = OP_LEN;
            extra = len_extra[s - 257];
            value = len_base[s - 257];
        }

        if (l <= root) {
            uint32_t e = ENTRY(op, l, extra, value);

            for (k = rev[s]; k < rsize; k += 1u << l) {
                h->t[k] = e;
            }
        } else {
            uint32_t off, sb, e = ENTRY(op, l - root, extra, value);

            pre = rev[s] & rmask;
            off = h->t[pre] >> 16;
            sb = subbits[pre];

            for (k = rev[s] >> root; k < (1u << sb); k += 1u << (l - root)) {
                h->t[off + k] = e;
            }
        }
    }

    return 1;
}

static struct {
    int ready;
    uint32_t lit[LIT_SIZE], dist[DIST_SIZE];
    htab hl, hd;
} FT;

static void fixed_init(void) {
    uint8_t l[288], d[30];
    int i;

    if (FT.ready) {
        return;
    }

    tables_init();

    for (i = 0; i < 288; i++) {
        l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    }

    for (i = 0; i < 30; i++) {
        d[i] = 5;
    }

    FT.hl.t = FT.lit;
    FT.hd.t = FT.dist;
    htab_build(l, 288, LIT_ROOT, 0, &FT.hl, LIT_SIZE);
    htab_build(d, 30, DIST_ROOT, 1, &FT.hd, DIST_SIZE);
    FT.ready = 1;
}

/* where inflate writes: a fixed span, or one that grows to at most limit. Everything before op is the history */
typedef struct {
    unsigned char* base;
    size_t op, cap, limit;
    int grow;
} iout;

static int iout_room(iout* o, size_t k) {
    size_t nc;
    unsigned char* q;

    if (o->op + k <= o->cap) {
        return 1;
    }

    if (!o->grow || o->op + k > o->limit) {
        return 0;
    }

    nc = o->cap * 2 > (size_t)1 << 16 ? o->cap * 2 : (size_t)1 << 16;
    nc = nc < o->op + k ? o->op + k : nc;
    nc = nc > o->limit ? o->limit : nc;

    if ((q = (unsigned char*)realloc(o->base, nc)) == NULL) {
        return 0;
    }

    o->base = q;
    o->cap = nc;
    return 1;
}

enum { SEG_FINAL, SEG_STOPPED, SEG_ERROR };

typedef struct {
    const unsigned char* p;
    size_t n, i;                /* the next byte to load; past n, zeros are loaded */
    uint64_t buf;
    int cnt;
    iout* out;
    uint32_t lit[LIT_SIZE], dist[DIST_SIZE];
} infl;

static size_t inf_pos(const infl* f) {
    return f->i * 8 - (size_t)f->cnt;
}

static void inf_refill(infl* f) {
    if (f->i + 8 <= f->n) {
        uint64_t v;
        const unsigned char* q = f->p + f->i;

        v = (uint64_t)q[0] | (uint64_t)q[1] << 8 | (uint64_t)q[2] << 16 | (uint64_t)q[3] << 24 |
            (uint64_t)q[4] << 32 | (uint64_t)q[5] << 40 | (uint64_t)q[6] << 48 | (uint64_t)q[7] << 56;
        f->buf |= v << f->cnt;
        f->i += (size_t)((63 - f->cnt) >> 3);
        f->cnt |= 56;
    } else {
        while (f->cnt <= 56) {
            uint64_t b = f->i < f->n ? f->p[f->i] : 0;

            f->buf |= b << f->cnt;
            f->i++;
            f->cnt += 8;
        }
    }
}

static void inf_need(infl* f, int k) {
    if (f->cnt < k) {
        inf_refill(f);
    }
}

static void inf_drop(infl* f, int k) {
    f->buf >>= k;
    f->cnt -= k;
}

static unsigned inf_take(infl* f, int k) {
    unsigned v = (unsigned)(f->buf & (((uint64_t)1 << k) - 1));

    inf_drop(f, k);
    return v;
}

static uint32_t inf_decode(infl* f, const htab* h) {
    uint32_t e = h->t[f->buf & ((1u << h->root) - 1)];

    if (((e >> 4) & 15) == OP_SUB) {
        inf_drop(f, (int)(e & 15));
        e = h->t[(e >> 16) + (f->buf & ((1u << ((e >> 8) & 0xFF)) - 1))];
    }

    inf_drop(f, (int)(e & 15));
    return e;
}

static int inf_stored(infl* f) {
    unsigned len, nlen;

    inf_drop(f, f->cnt & 7);
    inf_need(f, 32);
    len = inf_take(f, 16);
    nlen = inf_take(f, 16);

    if ((len ^ 0xFFFFu) != nlen) {
        return 0;
    }

    f->i -= (size_t)(f->cnt >> 3);  /* the whole bytes still held go back to the input */
    f->buf = 0;
    f->cnt = 0;

    if (f->i + len > f->n || !iout_room(f->out, len)) {
        return 0;
    }

    if (len) {
        memcpy(f->out->base + f->out->op, f->p + f->i, len);
    }

    f->out->op += len;
    f->i += len;
    return 1;
}

static int inf_header(infl* f, htab* lit, htab* dist) {
    int hlit, hdist, hclen, i, total;
    uint8_t cl[19] = { 0 }, clsym[128], clbits[128], lens[316];

    inf_need(f, 14);
    hlit = (int)inf_take(f, 5) + 257;
    hdist = (int)inf_take(f, 5) + 1;
    hclen = (int)inf_take(f, 4) + 4;

    if (hlit > 286 || hdist > 30) {
        return 0;
    }

    for (i = 0; i < hclen; i++) {
        inf_need(f, 3);
        cl[cl_order[i]] = (uint8_t)inf_take(f, 3);
    }

    if (!lengths_ok(cl, 19)) {
        return 0;
    }

    {   /* the code-length code is at most seven bits: one direct table */
        uint32_t next[8] = { 0 }, c = 0, k;
        int count[8] = { 0 }, b, s;

        memset(clbits, 0, sizeof(clbits));

        for (i = 0; i < 19; i++) {
            count[cl[i]]++;
        }

        count[0] = 0;

        for (b = 1; b < 8; b++) {
            c = (c + (uint32_t)count[b - 1]) << 1;
            next[b] = c;
        }

        for (s = 0; s < 19; s++) {
            if (cl[s]) {
                uint32_t r = reverse_bits(next[cl[s]]++, cl[s]);

                for (k = r; k < 128; k += 1u << cl[s]) {
                    clsym[k] = (uint8_t)s;
                    clbits[k] = cl[s];
                }
            }
        }
    }

    total = hlit + hdist;

    for (i = 0; i < total;) {
        unsigned k;
        int s, rep;
        uint8_t v = 0;

        inf_need(f, 14);
        k = (unsigned)(f->buf & 127);

        if (!clbits[k]) {
            return 0;
        }

        inf_drop(f, clbits[k]);
        s = clsym[k];

        if (s < 16) {
            lens[i++] = (uint8_t)s;
            continue;
        }

        if (s == 16) {
            if (i == 0) {
                return 0;
            }

            v = lens[i - 1];
            rep = 3 + (int)inf_take(f, 2);
        } else if (s == 17) {
            rep = 3 + (int)inf_take(f, 3);
        } else {
            rep = 11 + (int)inf_take(f, 7);
        }

        if (i + rep > total) {
            return 0;
        }

        while (rep--) {
            lens[i++] = v;
        }
    }

    if (lens[256] == 0) {
        return 0;
    }

    lit->t = f->lit;
    dist->t = f->dist;
    return htab_build(lens, hlit, LIT_ROOT, 0, lit, LIT_SIZE) && htab_build(lens + hlit, hdist, DIST_ROOT, 1, dist,
            DIST_SIZE);
}

static int inf_codes(infl* f, const htab* lit, const htab* dist) {
    iout* o = f->out;

    for (;;) {
        uint32_t e, op;
        size_t len, d, k;
        unsigned char* q;
        const unsigned char* s;

        if (f->cnt < 48) {
            /* past the end the bits are zeros, and zeros can spell a match that never ends the block: once the
               codes have used bits the input does not have, it is corrupt */
            if (inf_pos(f) > f->n * 8) {
                return 0;
            }

            inf_refill(f);
        }

        e = inf_decode(f, lit);
        op = (e >> 4) & 15;

        if (op == OP_LIT) {
            if (o->op >= o->cap && !iout_room(o, 1)) {
                return 0;
            }

            o->base[o->op++] = (unsigned char)(e >> 16);
            continue;
        }

        if (op != OP_LEN) {
            return op == OP_EOB;
        }

        len = (e >> 16) + inf_take(f, (int)((e >> 8) & 0xFF));
        e = inf_decode(f, dist);

        if (((e >> 4) & 15) != OP_LEN) {
            return 0;
        }

        d = (e >> 16) + inf_take(f, (int)((e >> 8) & 0xFF));

        if (d > o->op) {
            return 0;   /* before the start of the history */
        }

        if (o->op + len + 8 > o->cap && !iout_room(o, len + 8)) {
            if (!iout_room(o, len)) {
                return 0;
            }

            q = o->base + o->op;
            s = q - d;

            for (k = 0; k < len; k++) {
                q[k] = s[k];
            }

            o->op += len;
            continue;
        }

        q = o->base + o->op;
        s = q - d;

        if (d >= 8) {
            for (k = 0; k < len; k += 8) {
                memcpy(q + k, s + k, 8);
            }
        } else if (d == 1) {
            memset(q, s[0], len);
        } else {
            for (k = 0; k < len; k++) {
                q[k] = s[k];
            }
        }

        o->op += len;
    }
}

/* raw inflate from any bit up to the final block, or to a block boundary at or past stop_bit; *end_bit where the
   stream stood */
static int seg_builtin(const unsigned char* p, size_t n, size_t start_bit, size_t stop_bit, iout* out,
                       size_t* end_bit) {
    infl* f = (infl*)malloc(sizeof(infl));
    int st = SEG_ERROR;

    if (!f) {
        *end_bit = start_bit;
        return SEG_ERROR;
    }

    f->p = p;
    f->n = n;
    f->i = start_bit >> 3;
    f->buf = 0;
    f->cnt = 0;
    f->out = out;
    inf_refill(f);
    inf_drop(f, (int)(start_bit & 7));

    for (;;) {
        unsigned last, type;
        int ok = 0;
        htab lit, dist;

        if (inf_pos(f) >= stop_bit) {
            st = SEG_STOPPED;
            break;
        }

        inf_need(f, 3);
        last = inf_take(f, 1);
        type = inf_take(f, 2);

        if (type == 0) {
            ok = inf_stored(f);
        } else if (type == 1) {
            ok = inf_codes(f, &FT.hl, &FT.hd);
        } else if (type == 2) {
            ok = inf_header(f, &lit, &dist) && inf_codes(f, &lit, &dist);
        }

        if (!ok || inf_pos(f) > n * 8) {
            st = SEG_ERROR;
            break;
        }

        if (last) {
            st = SEG_FINAL;
            break;
        }
    }

    *end_bit = inf_pos(f);
    free(f);
    return st;
}

/* the same through the system zlib: the history already out given as its dictionary, a start inside a byte primed
   with that byte's remaining bits, and Z_BLOCK making it return at each block boundary to check the place */
static int seg_libz(const unsigned char* p, size_t n, size_t start_bit, size_t stop_bit, iout* out, size_t* end_bit) {
    zstream z;
    size_t at = start_bit >> 3, fed;
    int skip = (int)(start_bit & 7), ok, st = SEG_ERROR;
    unsigned char spare;

    *end_bit = start_bit;
    memset(&z, 0, sizeof(z));

    if (Z.inflateInit2_(&z, -15, Z.version, (int)sizeof(zstream)) != Z_OK) {
        return SEG_ERROR;
    }

    ok = at <= n;

    if (ok && out->op) {
        size_t k = out->op < 32768 ? out->op : 32768;

        ok = Z.inflateSetDictionary(&z, out->base + out->op - k, (unsigned)k) == Z_OK;
    }

    if (ok && skip) {
        ok = at < n && Z.inflatePrime(&z, 8 - skip, p[at] >> skip) == Z_OK;
        at++;
    }

    fed = at;   /* the first byte not yet handed over */

    while (ok) {
        unsigned before_out, before_in;
        size_t made, bit;
        int ret, scratch = 0;

        if (z.avail_in == 0 && fed < n) {
            size_t k = n - fed < ((size_t)1 << 30) ? n - fed : (size_t)1 << 30;

            z.next_in = p + fed;
            z.avail_in = (unsigned)k;
            fed += k;
        }

        if (out->op >= out->cap && !iout_room(out, 65536)) {
            z.next_out = &spare;    /* a span that is full: anything more is too much */
            z.avail_out = 1;
            scratch = 1;
        } else {
            size_t room = out->cap - out->op < ((size_t)1 << 30) ? out->cap - out->op : (size_t)1 << 30;

            z.next_out = out->base + out->op;
            z.avail_out = (unsigned)room;
        }

        before_out = z.avail_out;
        before_in = z.avail_in;
        ret = Z.inflate(&z, Z_BLOCK);
        made = before_out - z.avail_out;

        if (scratch) {
            if (made) {
                break;
            }
        } else {
            out->op += made;
        }

        bit = (fed - z.avail_in) * 8 - (size_t)(z.data_type & 63);

        /* the end: Z_STREAM_END, or (with Z_BLOCK) a block boundary just after the final block -- zlib reports that
           as Z_OK, 64 and 128 in data_type, and the stream end only on the call after: not a stop */
        if (ret == Z_STREAM_END || (ret == Z_OK && (z.data_type & 192) == 192)) {
            st = SEG_FINAL;
            *end_bit = bit;
            break;
        }

        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            break;
        }

        if ((z.data_type & 128) && bit >= stop_bit) {
            st = SEG_STOPPED;
            *end_bit = bit;
            break;
        }

        if (made == 0 && before_in == z.avail_in && fed >= n && ret == Z_BUF_ERROR) {
            break;  /* out of input before the end */
        }
    }

    Z.inflateEnd(&z);
    return st;
}

static int segment(int sys, const unsigned char* p, size_t n, size_t start_bit, size_t stop_bit, iout* out,
                   size_t* end_bit) {
    return sys ? seg_libz(p, n, start_bit, stop_bit, out, end_bit) : seg_builtin(p, n, start_bit, stop_bit, out, end_bit);
}

/* just past the first full-flush mark 00 00 FF FF in [lo, hi), or 0 */
static size_t find_flush(const unsigned char* p, size_t lo, size_t hi) {
    size_t i = lo > 2 ? lo : 2;

    while (i + 1 < hi) {
        const unsigned char* q = (const unsigned char*)memchr(p + i, 0xFF, hi - 1 - i);
        size_t j;

        if (!q) {
            return 0;
        }

        j = (size_t)(q - p);

        if (p[j + 1] == 0xFF && p[j - 1] == 0 && p[j - 2] == 0) {
            return j + 2;
        }

        i = j + 1;
    }

    return 0;
}

/* f(i, ctx) for i in [0, count) on up to `threads` threads */
typedef struct {
    size_t count, next;
    void (*fn)(size_t, void*);
    void* ctx;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} pfor_job;

static void pfor_work(pfor_job* J) {
    for (;;) {
        size_t i;

#ifdef _WIN32
        EnterCriticalSection(&J->lock);
#else
        pthread_mutex_lock(&J->lock);
#endif
        i = J->next++;
#ifdef _WIN32
        LeaveCriticalSection(&J->lock);
#else
        pthread_mutex_unlock(&J->lock);
#endif

        if (i >= J->count) {
            return;
        }

        J->fn(i, J->ctx);
    }
}

#ifdef _WIN32
static DWORD WINAPI pfor_thread(LPVOID arg) {
    pfor_work((pfor_job*)arg);
    return 0;
}
#else
static void* pfor_thread(void* arg) {
    pfor_work((pfor_job*)arg);
    return NULL;
}
#endif

static void pfor(size_t count, int threads, void (*fn)(size_t, void*), void* ctx) {
    pfor_job J;
    int t, started = 0;
#ifdef _WIN32
    HANDLE th[64];
#else
    pthread_t th[64];
#endif

    memset(&J, 0, sizeof(J));
    J.count = count;
    J.fn = fn;
    J.ctx = ctx;
    threads = (size_t)threads < count ? threads : (int)count;
    threads = threads > 64 ? 64 : threads;
#ifdef _WIN32
    InitializeCriticalSection(&J.lock);
#else
    pthread_mutex_init(&J.lock, NULL);
#endif

    for (t = 1; t < threads; t++) {
#ifdef _WIN32
        if ((th[started] = CreateThread(NULL, 0, pfor_thread, &J, 0, NULL)) != NULL) {
            started++;
        }
#else
        if (pthread_create(&th[started], NULL, pfor_thread, &J) == 0) {
            started++;
        }
#endif
    }

    pfor_work(&J);

    for (t = 0; t < started; t++) {
#ifdef _WIN32
        WaitForSingleObject(th[t], INFINITE);
        CloseHandle(th[t]);
#else
        pthread_join(th[t], NULL);
#endif
    }

#ifdef _WIN32
    DeleteCriticalSection(&J.lock);
#else
    pthread_mutex_destroy(&J.lock);
#endif
}

/* the speculative inflates: piece j from cand[j - 1] (j 0: the start, into the output itself) */
typedef struct {
    const unsigned char* p;
    size_t n, m;
    const size_t* cand;
    iout* out;
    iout* piece;
    int* st;
    size_t* end;
    int sys;
} spec_job;

static void spec_one(size_t j, void* ctx) {
    spec_job* S = (spec_job*)ctx;

    if (j == 0) {
        S->st[0] = segment(S->sys, S->p, S->n, 0, S->cand[0] * 8, S->out, &S->end[0]);
    } else {
        iout* o = &S->piece[j - 1];
        size_t to = j < S->m ? S->cand[j] : S->n;

        o->grow = 1;
        o->limit = S->out->limit;
        o->cap = (to - S->cand[j - 1]) * 3 + 65536;
        o->cap = o->cap > o->limit ? o->limit : o->cap;
        o->base = (unsigned char*)malloc(o->cap ? o->cap : 1);

        if (!o->base) {
            o->cap = 0;
            S->st[j] = SEG_ERROR;
            return;
        }

        S->st[j] = segment(S->sys, S->p, S->n, S->cand[j - 1] * 8, j < S->m ? S->cand[j] * 8 : (size_t)-1, o, &S->end[j]);
    }
}

typedef struct {
    unsigned char* dst;
    const unsigned char* src;
    size_t len;
} copy_job;

static void copy_one(size_t i, void* ctx) {
    copy_job* C = (copy_job*)ctx + i;

    if (C->len) {
        memcpy(C->dst, C->src, C->len);
    }
}

/* raw deflate data into out, in parallel where that can be proven; 1 when it reaches its final block */
static int inflate_body(const unsigned char* p, size_t n, iout* out, int threads, int sys, size_t* end_bit) {
    const size_t share = (size_t)1 << 20;
    size_t* cand = NULL, m = 0, k, ns, j, bit, np = 0;
    int wide = !out->grow && out->cap - out->op >= 2 * share && n >= 1024, cur, ok;
    spec_job S;
    copy_job* pend = NULL;

    /* a stream that comes out large is worth splitting however small it is: zlibmt's blocks of a long run of alike
       numbers deflate to a few hundred bytes each, and inflating them is the work */
    if (threads > 1 && (n >= 4 * share || wide)) {
        ns = n >= 4 * share ? n / share : n / 512;
        ns = ns < (size_t)threads * 8 ? ns : (size_t)threads * 8;

        if (ns > 1 && (cand = (size_t*)malloc(ns * sizeof(size_t))) != NULL) {
            for (k = 1; k < ns; k++) {
                size_t at = find_flush(p, k * n / ns, (k + 1) * n / ns);

                if (at && at < n && (m == 0 || at > cand[m - 1])) {
                    cand[m++] = at;
                }
            }
        }
    }

    if (m == 0) {
        free(cand);
        return segment(sys, p, n, 0, (size_t)-1, out, end_bit) == SEG_FINAL;
    }

    memset(&S, 0, sizeof(S));
    S.p = p;
    S.n = n;
    S.m = m;
    S.cand = cand;
    S.out = out;
    S.sys = sys;
    S.piece = (iout*)calloc(m, sizeof(iout));
    S.st = (int*)calloc(m + 1, sizeof(int));
    S.end = (size_t*)calloc(m + 1, sizeof(size_t));
    pend = (copy_job*)calloc(m, sizeof(copy_job));

    if (!S.piece || !S.st || !S.end || !pend) {
        free(S.piece);
        free(S.st);
        free(S.end);
        free(pend);
        free(cand);
        return segment(sys, p, n, 0, (size_t)-1, out, end_bit) == SEG_FINAL;
    }

    /* the output's own start runs as piece 0 while the others run beside it: it grows (or not) as the output does,
       and the pieces only read the input */
    pfor(m + 1, threads, spec_one, &S);

    cur = S.st[0];
    bit = S.end[0];
    j = 1;  /* the next candidate is cand[j - 1], its result S.st[j] */
    ok = 1;

    while (cur == SEG_STOPPED && ok) {
        while (j <= m && cand[j - 1] * 8 < bit) {
            j++;
        }

        if (j <= m && cand[j - 1] * 8 == bit && S.st[j] != SEG_ERROR) {     /* proven: its output is in place */
            iout* b = &S.piece[j - 1];

            if (!iout_room(out, b->op)) {
                ok = 0;
                break;
            }

            pend[np].dst = NULL;    /* where it goes, fixed once no growth can move the output */
            pend[np].src = b->base;
            pend[np].len = b->op;
            pend[np].dst = (unsigned char*)(uintptr_t)out->op;
            np++;
            out->op += b->op;
            cur = S.st[j];
            bit = S.end[j];
            j++;
            continue;
        }

        if (j <= m && cand[j - 1] * 8 == bit) {
            j++;    /* a real boundary whose piece failed */
        }

        for (k = 0; k < np; k++) {  /* the pieces placed before the serial stretch needs them as history */
            pend[k].dst = out->base + (size_t)(uintptr_t)pend[k].dst;
        }

        pfor(np, threads, copy_one, pend);
        np = 0;
        cur = segment(sys, p, n, bit, j <= m ? cand[j - 1] * 8 : (size_t)-1, out, &bit);
    }

    for (k = 0; k < np; k++) {
        pend[k].dst = out->base + (size_t)(uintptr_t)pend[k].dst;
    }

    if (ok) {
        pfor(np, threads, copy_one, pend);
    }

    for (k = 0; k < m; k++) {
        free(S.piece[k].base);
    }

    free(S.piece);
    free(S.st);
    free(S.end);
    free(pend);
    free(cand);
    *end_bit = bit;
    return ok && cur == SEG_FINAL;
}

/* the Adler-32 of a large buffer, in pieces on every thread */
typedef struct {
    const unsigned char* p;
    size_t n;
    uint32_t* part;
} sum_job;

#define SUM_PIECE ((size_t)4 << 20)

static void sum_one(size_t i, void* ctx) {
    sum_job* S = (sum_job*)ctx;
    size_t len = S->n - i * SUM_PIECE < SUM_PIECE ? S->n - i * SUM_PIECE : SUM_PIECE;

    S->part[i] = adler32_part(1, S->p + i * SUM_PIECE, len);
}

static uint32_t adler32_mt(const unsigned char* p, size_t n, int threads) {
    size_t np = (n + SUM_PIECE - 1) / SUM_PIECE, i;
    sum_job S;
    uint32_t s;

    if (np <= 1 || threads <= 1 || (S.part = (uint32_t*)malloc(np * sizeof(uint32_t))) == NULL) {
        return adler32_part(1, p, n);
    }

    S.p = p;
    S.n = n;
    pfor(np, threads, sum_one, &S);
    s = S.part[0];

    for (i = 1; i < np; i++) {
        s = adler32_combine(s, S.part[i], n - i * SUM_PIECE < SUM_PIECE ? n - i * SUM_PIECE : SUM_PIECE);
    }

    free(S.part);
    return s;
}

/* inflate raw deflate data (zlib = 1: skip the zlib header and check Adler-32); limit caps the output. With a limit
   given (an expected size, as callers have) the output is that span, and a large one is inflated in parallel */
int pd_inflate(const void* data, size_t n, int zlib, size_t limit, unsigned char** out, size_t* outlen) {
    const unsigned char* p = (const unsigned char*)data;
    iout o;
    size_t end_bit = 0, body = zlib ? 2 : 0;
    int threads;

    fixed_init();
    memset(&o, 0, sizeof(o));
    o.limit = limit ? limit : (size_t)1 << 30;

    if (zlib && (n < 6 || (p[0] & 0x0F) != 8 || (p[0] >> 4) > 7 || ((p[0] << 8) | p[1]) % 31 != 0 || (p[1] & 0x20))) {
        return -1;  /* not deflate, a bad check, or a preset dictionary */
    }

    /* a span of the size expected -- no more than deflate can make of this much input (1032 to 1), so a claimed
       size alone cannot make it allocate much */
    if (limit && limit <= (size_t)256 << 20 && limit / 1032 <= n + 64) {
        o.cap = limit;
        o.grow = 0;
    } else {
        o.cap = n * 4 + 4096 < o.limit ? n * 4 + 4096 : o.limit;
        o.grow = 1;
    }

    if ((o.base = (unsigned char*)malloc(o.cap ? o.cap : 1)) == NULL) {
        return -1;
    }

    threads = cpu_count();

    if (!inflate_body(p + body, n - body, &o, threads, libz_use(), &end_bit)) {
        free(o.base);
        return -1;
    }

    if (zlib) {
        size_t at = body + (end_bit + 7) / 8;
        uint32_t want;

        if (at + 4 > n) {
            free(o.base);
            return -1;
        }

        want = ((uint32_t)p[at] << 24) | ((uint32_t)p[at + 1] << 16) | ((uint32_t)p[at + 2] << 8) | p[at + 3];

        if (want != adler32_mt(o.base, o.op, threads)) {
            free(o.base);
            return -1;
        }
    }

    *out = o.base;
    *outlen = o.op;
    return 0;
}

/* CRC-32 (zip, PNG), a table a byte */
uint32_t pd_crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static int ready;
    const unsigned char* p = (const unsigned char*)data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;

    if (!ready) {
        uint32_t k, v;
        int b;

        for (k = 0; k < 256; k++) {
            v = k;

            for (b = 0; b < 8; b++) {
                v = (v >> 1) ^ (0xEDB88320u & (0u - (v & 1)));
            }

            table[k] = v;
        }

        ready = 1;
    }

    for (i = 0; i < n; i++) {
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    }

    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* PNG out                                                            */
/* ------------------------------------------------------------------ */

static void png_be32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* a chunk at *at in buf: length, type, data, CRC of type and data */
static size_t png_put_chunk(unsigned char* buf, size_t at, const char* type, const unsigned char* data, size_t n) {
    png_be32(buf + at, (uint32_t)n);
    memcpy(buf + at + 4, type, 4);

    if (n) {
        memcpy(buf + at + 8, data, n);
    }

    png_be32(buf + at + 8 + n, pd_crc32(buf + at + 4, n + 4));
    return at + 12 + n;
}

int pd_png_encode(const uint8_t* rgba, int32_t w, int32_t h, int32_t stride, unsigned char** out, size_t* outlen) {
    size_t row = (size_t)w * 4 + 1, zn = 0, at;
    unsigned char* raw, *z = NULL, ihdr[13];
    int32_t x, y;

    *out = NULL;
    *outlen = 0;

    if (w <= 0 || h <= 0 || !(raw = (unsigned char*)malloc(row * (size_t)h))) {
        return 1;
    }

    for (y = 0; y < h; y++) {   /* filter 0; the colours unmultiplied by alpha */
        const uint8_t* s = rgba + (size_t)y * (size_t)stride;
        unsigned char* d = raw + row * (size_t)y;

        d[0] = 0;

        for (x = 0; x < w; x++) {
            int k, a = s[4 * x + 3];

            for (k = 0; k < 3; k++) {
                d[1 + 4 * x + k] = (unsigned char)(a ? (s[4 * x + k] * 255 + a / 2) / a : 0);
            }

            d[1 + 4 * x + 3] = (unsigned char)a;
        }
    }

    if (pd_deflate(raw, row * (size_t)h, 1, &z, &zn) != 0) {
        free(raw);
        return 1;
    }

    free(raw);
    png_be32(ihdr, (uint32_t)w);
    png_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    /* 8 bits, RGBA, deflate, filter 0, no interlace */
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    if (!(*out = (unsigned char*)malloc(8 + 25 + 12 + zn + 12))) {
        free(z);
        return 1;
    }

    memcpy(*out, "\x89PNG\r\n\x1a\n", 8);
    at = png_put_chunk(*out, 8, "IHDR", ihdr, 13);
    at = png_put_chunk(*out, at, "IDAT", z, zn);
    at = png_put_chunk(*out, at, "IEND", NULL, 0);
    free(z);
    *outlen = at;
    return 0;
}
