/*
 * Parade zlib streams: deflate (LZ77 + fixed Huffman) and inflate
 *
 * A small, dependency-free implementation for PDF streams, PNG data and
 * DOCX (zip) parts. The compressor is deterministic (same bytes in, same
 * bytes out, everywhere); the decompressor handles stored, fixed and
 * dynamic Huffman blocks and is bounds-checked for untrusted input.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

/* ------------------------------------------------------------------ */
/* output bits                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned char* p;
    size_t n, cap;
    uint32_t bits;
    int nbits;
    int err;
} bitw;

static void put_byte(bitw* w, unsigned char c) {
    if (w->n >= w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 1024;
        unsigned char* q = (unsigned char*)realloc(w->p, nc);

        if (!q) {
            w->err = 1;
            return;
        }

        w->p = q;
        w->cap = nc;
    }

    w->p[w->n++] = c;
}

static void put_bits(bitw* w, uint32_t v, int n) {
    w->bits |= v << w->nbits;
    w->nbits += n;

    while (w->nbits >= 8) {
        put_byte(w, (unsigned char)w->bits);
        w->bits >>= 8;
        w->nbits -= 8;
    }
}

/* Huffman codes are sent most significant bit first */
static void put_code(bitw* w, uint32_t code, int len) {
    uint32_t r = 0;
    int k;

    for (k = 0; k < len; k++) {
        r = (r << 1) | ((code >> k) & 1);
    }

    put_bits(w, r, len);
}

/* fixed literal/length code */
static void put_lit(bitw* w, int sym) {
    if (sym < 144) {
        put_code(w, 0x30 + sym, 8);
    } else if (sym < 256) {
        put_code(w, 0x190 + sym - 144, 9);
    } else if (sym < 280) {
        put_code(w, sym - 256, 7);
    } else {
        put_code(w, 0xC0 + sym - 280, 8);
    }
}

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

static void put_match(bitw* w, int len, int dist) {
    int i;

    for (i = 28; len_base[i] > len; i--) {
    }

    put_lit(w, 257 + i);
    put_bits(w, (uint32_t)(len - len_base[i]), len_extra[i]);

    for (i = 29; dist_base[i] > dist; i--) {
    }

    put_code(w, (uint32_t)i, 5);
    put_bits(w, (uint32_t)(dist - dist_base[i]), dist_extra[i]);
}

static uint32_t adler32(const unsigned char* p, size_t n) {
    uint32_t a = 1, b = 0;
    size_t i;

    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }

    return (b << 16) | a;
}

#define HASH_BITS 15
#define WINDOW 32768
#define MAX_CHAIN 64

/* raw deflate of data; zlib = 1 adds the zlib header and Adler-32 trailer */
int pd_deflate(const void* data, size_t n, int zlib, unsigned char** out, size_t* outlen) {
    const unsigned char* in = (const unsigned char*)data;
    bitw w;
    int32_t* head;
    int32_t* prev;
    size_t i = 0;

    memset(&w, 0, sizeof(w));
    head = (int32_t*)malloc(((size_t)1 << HASH_BITS) * sizeof(int32_t));
    prev = (int32_t*)malloc((n ? n : 1) * sizeof(int32_t));

    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }

    memset(head, 0xFF, ((size_t)1 << HASH_BITS) * sizeof(int32_t));

    if (zlib) {
        put_byte(&w, 0x78);
        put_byte(&w, 0x9C);
    }

    put_bits(&w, 1, 1);     /* one final block */
    put_bits(&w, 1, 2);     /* fixed Huffman */

    while (i < n) {
        int best_len = 0, best_dist = 0;

        if (i + 2 < n) {
            uint32_t h = ((uint32_t)in[i] << 10 ^ (uint32_t)in[i + 1] << 5 ^ in[i + 2]) & ((1u << HASH_BITS) - 1);
            int32_t cand = head[h];
            int chain = 0;

            while (cand >= 0 && i - (size_t)cand <= WINDOW && chain++ < MAX_CHAIN) {
                int l = 0, lim = (int)(n - i < 258 ? n - i : 258);

                while (l < lim && in[cand + l] == in[i + l]) {
                    l++;
                }

                if (l > best_len) {
                    best_len = l;
                    best_dist = (int)(i - (size_t)cand);

                    if (l == lim) {
                        break;
                    }
                }

                cand = prev[cand];
            }

            prev[i] = head[h];
            head[h] = (int32_t)i;
        }

        if (best_len >= 3) {
            size_t k;

            put_match(&w, best_len, best_dist);

            for (k = 1; k < (size_t)best_len && i + k + 2 < n; k++) {   /* index the skipped positions */
                uint32_t h = ((uint32_t)in[i + k] << 10 ^ (uint32_t)in[i + k + 1] << 5 ^ in[i + k + 2]) &
                             ((1u << HASH_BITS) - 1);
                prev[i + k] = head[h];
                head[h] = (int32_t)(i + k);
            }

            i += (size_t)best_len;
        } else {
            put_lit(&w, in[i]);
            i++;
        }
    }

    put_lit(&w, 256);   /* end of block */

    if (w.nbits > 0) {
        put_byte(&w, (unsigned char)w.bits);
    }

    if (zlib) {
        uint32_t a = adler32(in, n);

        put_byte(&w, (unsigned char)(a >> 24));
        put_byte(&w, (unsigned char)(a >> 16));
        put_byte(&w, (unsigned char)(a >> 8));
        put_byte(&w, (unsigned char)a);
    }

    free(head);
    free(prev);

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
