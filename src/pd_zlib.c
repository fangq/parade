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

#ifdef _WIN32
#include <windows.h>
#else
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
    size_t i;

    if (dfl_init(&D)) {
        J->err = 1;
        dfl_free(&D);
        return;
    }

    while ((i = mt_take(J)) < J->nb) {
        size_t off = i * MT_BLOCK, len = J->n - off < MT_BLOCK ? J->n - off : MT_BLOCK;

        J->sum[i] = adler32_part(1, J->in + off, len);
        dfl_run(&D, J->in + off, len, &J->seg[i], 0);

        if (J->seg[i].err) {
            J->err = 1;
        }
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
static int deflate_mt(const unsigned char* in, size_t n, bitw* w, uint32_t* adler) {
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
        if (deflate_mt(in, n, &w, &a)) {
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

/* the Adler-32 of a whole buffer, for inflate's check */
static uint32_t adler32(const unsigned char* p, size_t n) {
    return adler32_part(1, p, n);
}

/* ------------------------------------------------------------------ */
/* inflate                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const unsigned char* p;
    size_t n, i;
    uint32_t bits;
    int nbits;
    int err;
} bitr;

static uint32_t get_bits(bitr* r, int n) {
    uint32_t v;

    while (r->nbits < n) {
        if (r->i >= r->n) {
            r->err = 1;
            return 0;
        }

        r->bits |= (uint32_t)r->p[r->i++] << r->nbits;
        r->nbits += 8;
    }

    v = r->bits & ((1u << n) - 1);
    r->bits >>= n;
    r->nbits -= n;
    return v;
}

typedef struct {
    uint16_t count[16];
    uint16_t sym[320];
} huff;

static int huff_build(huff* h, const uint8_t* lens, int n) {
    uint16_t offs[16];
    int i, left = 1;

    memset(h->count, 0, sizeof(h->count));

    for (i = 0; i < n; i++) {
        h->count[lens[i]]++;
    }

    h->count[0] = 0;

    for (i = 1; i < 16; i++) {      /* over-subscribed sets are invalid */
        left <<= 1;
        left -= h->count[i];

        if (left < 0) {
            return -1;
        }
    }

    offs[1] = 0;

    for (i = 1; i < 15; i++) {
        offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    }

    for (i = 0; i < n; i++) {
        if (lens[i]) {
            h->sym[offs[lens[i]]++] = (uint16_t)i;
        }
    }

    return 0;
}

static int decode(bitr* r, const huff* h) {
    int code = 0, first = 0, index = 0, len;

    for (len = 1; len < 16; len++) {
        code |= (int)get_bits(r, 1);

        if (r->err) {
            return -1;
        }

        if (code - h->count[len] < first) {
            return h->sym[index + (code - first)];
        }

        index += h->count[len];
        first += h->count[len];
        first <<= 1;
        code <<= 1;
    }

    r->err = 1;
    return -1;
}

typedef struct {
    unsigned char* p;
    size_t n, cap, limit;
    int err;
} outbuf;

static void out_byte(outbuf* o, unsigned char c) {
    if (o->n >= o->limit) {
        o->err = 1;
        return;
    }

    if (o->n >= o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 4096;
        unsigned char* q;

        nc = nc > o->limit ? o->limit : nc;
        q = (unsigned char*)realloc(o->p, nc);

        if (!q) {
            o->err = 1;
            return;
        }

        o->p = q;
        o->cap = nc;
    }

    o->p[o->n++] = c;
}

static int inflate_codes(bitr* r, outbuf* o, const huff* lit, const huff* dist) {
    for (;;) {
        int sym = decode(r, lit);

        if (r->err || o->err || sym < 0) {
            return -1;
        }

        if (sym < 256) {
            out_byte(o, (unsigned char)sym);
        } else if (sym == 256) {
            return 0;
        } else {
            int len, ds, d;
            size_t k;

            sym -= 257;

            if (sym >= 29) {
                return -1;
            }

            len = len_base[sym] + (int)get_bits(r, len_extra[sym]);
            ds = decode(r, dist);

            if (ds < 0 || ds >= 30) {
                return -1;
            }

            d = dist_base[ds] + (int)get_bits(r, dist_extra[ds]);

            if (r->err || (size_t)d > o->n) {
                return -1;
            }

            for (k = 0; k < (size_t)len && !o->err; k++) {
                out_byte(o, o->p[o->n - (size_t)d]);
            }
        }
    }
}

/* inflate raw deflate data (zlib = 1: skip the zlib header and check Adler-32); limit caps the output */
int pd_inflate(const void* data, size_t n, int zlib, size_t limit, unsigned char** out, size_t* outlen) {
    bitr r;
    outbuf o;
    int last = 0;

    memset(&r, 0, sizeof(r));
    memset(&o, 0, sizeof(o));
    r.p = (const unsigned char*)data;
    r.n = n;
    o.limit = limit ? limit : (size_t)1 << 30;

    if (zlib) {
        if (n < 6 || (r.p[0] & 0x0F) != 8 || ((r.p[0] << 8) | r.p[1]) % 31 != 0 || (r.p[1] & 0x20)) {
            return -1;
        }

        r.i = 2;
    }

    while (!last) {
        int type;

        last = (int)get_bits(&r, 1);
        type = (int)get_bits(&r, 2);

        if (r.err) {
            break;
        }

        if (type == 0) {    /* stored */
            uint32_t len, nlen;

            r.bits = 0;
            r.nbits = 0;

            if (r.i + 4 > r.n) {
                r.err = 1;
                break;
            }

            len = r.p[r.i] | ((uint32_t)r.p[r.i + 1] << 8);
            nlen = r.p[r.i + 2] | ((uint32_t)r.p[r.i + 3] << 8);
            r.i += 4;

            if ((len ^ 0xFFFF) != nlen || r.i + len > r.n) {
                r.err = 1;
                break;
            }

            while (len-- && !o.err) {
                out_byte(&o, r.p[r.i++]);
            }
        } else if (type == 1) {     /* fixed */
            uint8_t lens[320];
            huff lit, dist;
            int k;

            for (k = 0; k < 144; k++) {
                lens[k] = 8;
            }

            for (; k < 256; k++) {
                lens[k] = 9;
            }

            for (; k < 280; k++) {
                lens[k] = 7;
            }

            for (; k < 288; k++) {
                lens[k] = 8;
            }

            huff_build(&lit, lens, 288);

            for (k = 0; k < 30; k++) {
                lens[k] = 5;
            }

            huff_build(&dist, lens, 30);

            if (inflate_codes(&r, &o, &lit, &dist)) {
                r.err = 1;
            }
        } else if (type == 2) {     /* dynamic */
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t lens[320], cl[19];
            huff lit, dist, clh;
            int nlit = (int)get_bits(&r, 5) + 257, ndist = (int)get_bits(&r, 5) + 1, ncl = (int)get_bits(&r, 4) + 4;
            int k, idx = 0;

            if (r.err || nlit > 286 || ndist > 30) {
                r.err = 1;
                break;
            }

            memset(cl, 0, sizeof(cl));

            for (k = 0; k < ncl; k++) {
                cl[order[k]] = (uint8_t)get_bits(&r, 3);
            }

            if (huff_build(&clh, cl, 19)) {
                r.err = 1;
                break;
            }

            while (idx < nlit + ndist && !r.err) {
                int sym = decode(&r, &clh), rep = 0;
                uint8_t v = 0;

                if (sym < 0) {
                    r.err = 1;
                    break;
                }

                if (sym < 16) {
                    lens[idx++] = (uint8_t)sym;
                    continue;
                }

                if (sym == 16) {
                    if (idx == 0) {
                        r.err = 1;
                        break;
                    }

                    v = lens[idx - 1];
                    rep = 3 + (int)get_bits(&r, 2);
                } else if (sym == 17) {
                    rep = 3 + (int)get_bits(&r, 3);
                } else {
                    rep = 11 + (int)get_bits(&r, 7);
                }

                if (idx + rep > nlit + ndist) {
                    r.err = 1;
                    break;
                }

                while (rep--) {
                    lens[idx++] = v;
                }
            }

            if (r.err || huff_build(&lit, lens, nlit) || huff_build(&dist, lens + nlit, ndist) ||
                    inflate_codes(&r, &o, &lit, &dist)) {
                r.err = 1;
            }
        } else {
            r.err = 1;
        }

        if (r.err || o.err) {
            break;
        }
    }

    if (r.err || o.err) {
        free(o.p);
        return -1;
    }

    if (zlib) {
        size_t at;
        uint32_t want;

        r.bits = 0;
        r.nbits = 0;
        at = r.i;

        if (at + 4 > r.n) {
            free(o.p);
            return -1;
        }

        want = ((uint32_t)r.p[at] << 24) | ((uint32_t)r.p[at + 1] << 16) | ((uint32_t)r.p[at + 2] << 8) | r.p[at + 3];

        if (want != adler32(o.p ? o.p : (const unsigned char*)"", o.n)) {
            free(o.p);
            return -1;
        }
    }

    *out = o.p ? o.p : (unsigned char*)malloc(1);
    *outlen = o.n;
    return *out ? 0 : -1;
}

uint32_t pd_crc32(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        c ^= p[i];

        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
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
