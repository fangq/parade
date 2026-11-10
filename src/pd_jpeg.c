/*
 * A baseline JPEG encoder (ITU T.81), for the PDF writer's photographs:
 * YCbCr, the chroma halved each way (4:2:0), the standard's example
 * quantization tables scaled to a quality as IJG's libjpeg scales them, and
 * its example Huffman tables (Annex K). No markers but those a DCTDecode
 * filter needs: SOI, DQT, SOF0, DHT, SOS, EOI.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63
};

static const uint8_t q_luma[64] = {     /* in the block's own order, row by row */
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};

static const uint8_t q_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

static const uint8_t dc_luma_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_chroma_bits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };

static const uint8_t ac_luma_bits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

static const uint8_t ac_chroma_bits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa
};

typedef struct {
    uint16_t code[256];
    uint8_t size[256];
} huff;

typedef struct {
    unsigned char* p;
    size_t n, cap;
    uint32_t acc;       /* bits not yet written, the first the highest */
    int nacc;
    int err;
} jout;

static void put_byte(jout* o, unsigned b) {
    if (o->n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        unsigned char* q = (unsigned char*)realloc(o->p, cap);

        if (!q) {
            o->err = 1;
            return;
        }

        o->p = q;
        o->cap = cap;
    }

    o->p[o->n++] = (unsigned char)b;
}

static void put16(jout* o, unsigned v) {
    put_byte(o, v >> 8);
    put_byte(o, v & 0xFF);
}

/* n bits of v, the highest first; an 0xFF in the data followed by a 0 */
static void put_bits(jout* o, uint32_t v, int n) {
    o->acc = (o->acc << n) | (v & ((1u << n) - 1));
    o->nacc += n;

    while (o->nacc >= 8) {
        unsigned b = (o->acc >> (o->nacc - 8)) & 0xFF;

        put_byte(o, b);

        if (b == 0xFF) {
            put_byte(o, 0);
        }

        o->nacc -= 8;
    }
}

static void make_huff(huff* h, const uint8_t* bits, const uint8_t* vals) {
    int len, i, k = 0;
    unsigned code = 0;

    memset(h, 0, sizeof(*h));

    for (len = 1; len <= 16; len++) {
        for (i = 0; i < bits[len - 1]; i++) {
            h->code[vals[k]] = (uint16_t)code;
            h->size[vals[k]] = (uint8_t)len;
            k++;
            code++;
        }

        code <<= 1;
    }
}

static void put_dht(jout* o, int cls_id, const uint8_t* bits, const uint8_t* vals) {
    int i, n = 0;

    for (i = 0; i < 16; i++) {
        n += bits[i];
    }

    put16(o, 0xFFC4);
    put16(o, (unsigned)(2 + 1 + 16 + n));
    put_byte(o, (unsigned)cls_id);

    for (i = 0; i < 16; i++) {
        put_byte(o, bits[i]);
    }

    for (i = 0; i < n; i++) {
        put_byte(o, vals[i]);
    }
}

/* the bits a value takes, and the bits written for it (a negative one's complement) */
static int category(int v) {
    int n = 0;

    v = v < 0 ? -v : v;

    while (v) {
        n++;
        v >>= 1;
    }

    return n;
}

static void put_value(jout* o, int v, int n) {
    if (n) {
        put_bits(o, (uint32_t)(v < 0 ? v - 1 : v), n);
    }
}

/* a block: its DCT, quantized, coded after the block before's DC */
static void code_block(jout* o, const float* blk, const float* cosu, const int* q, int* dc, const huff* hdc,
                       const huff* hac) {
    float tmp[64], f;
    int coef[64], u, v, x, y, k, run, s;

    for (y = 0; y < 8; y++) {      /* rows: tmp[y][u] */
        for (u = 0; u < 8; u++) {
            f = 0;

            for (x = 0; x < 8; x++) {
                f += cosu[u * 8 + x] * blk[y * 8 + x];
            }

            tmp[y * 8 + u] = f;
        }
    }

    for (v = 0; v < 8; v++) {      /* columns */
        for (u = 0; u < 8; u++) {
            f = 0;

            for (y = 0; y < 8; y++) {
                f += cosu[v * 8 + y] * tmp[y * 8 + u];
            }

            coef[v * 8 + u] = (int)lrintf(f / (float)q[v * 8 + u]);
        }
    }

    k = coef[0] - *dc;
    *dc = coef[0];
    s = category(k);
    put_bits(o, hdc->code[s], hdc->size[s]);
    put_value(o, k, s);
    run = 0;

    for (k = 1; k < 64; k++) {
        int c = coef[zigzag[k]];

        if (!c) {
            run++;
            continue;
        }

        while (run > 15) {
            put_bits(o, hac->code[0xF0], hac->size[0xF0]);
            run -= 16;
        }

        s = category(c);

        if (s > 10) {   /* (past what baseline codes: as large as it can be) */
            s = 10;
            c = c < 0 ? -1023 : 1023;
        }

        put_bits(o, hac->code[(run << 4) | s], hac->size[(run << 4) | s]);
        put_value(o, c, s);
        run = 0;
    }

    if (run) {
        put_bits(o, hac->code[0], hac->size[0]);
    }
}

int pd_jpeg_encode(const unsigned char* rgb, int32_t w, int32_t h, int quality, unsigned char** out, size_t* outlen) {
    jout o;
    huff hdl, hal, hdc, hac;
    int ql[64], qc[64], i, scale, dcy = 0, dcb = 0, dcr = 0;
    float cosu[64], blk[64], cb[64], cr[64];
    int32_t mx, my, bx, by, x, y;

    *out = NULL;
    *outlen = 0;

    if (!rgb || w <= 0 || h <= 0 || w > 65535 || h > 65535) {
        return 1;
    }

    quality = quality < 1 ? 1 : quality > 100 ? 100 : quality;
    scale = quality < 50 ? 5000 / quality : 200 - quality * 2;

    for (i = 0; i < 64; i++) {
        ql[i] = (q_luma[i] * scale + 50) / 100;
        qc[i] = (q_chroma[i] * scale + 50) / 100;
        ql[i] = ql[i] < 1 ? 1 : ql[i] > 255 ? 255 : ql[i];
        qc[i] = qc[i] < 1 ? 1 : qc[i] > 255 ? 255 : qc[i];
    }

    for (i = 0; i < 64; i++) {      /* C(u)/2 cos((2x + 1) u pi / 16) */
        int u = i / 8, xx = i % 8;

        cosu[i] = (float)((u ? 0.5 : 0.5 / sqrt(2.0)) * cos((2 * xx + 1) * u * 3.14159265358979323846 / 16));
    }

    make_huff(&hdl, dc_luma_bits, dc_vals);
    make_huff(&hal, ac_luma_bits, ac_luma_vals);
    make_huff(&hdc, dc_chroma_bits, dc_vals);
    make_huff(&hac, ac_chroma_bits, ac_chroma_vals);
    memset(&o, 0, sizeof(o));

    put16(&o, 0xFFD8);
    put16(&o, 0xFFDB);         /* the tables, in zigzag order */
    put16(&o, 2 + 2 * 65);
    put_byte(&o, 0);

    for (i = 0; i < 64; i++) {
        put_byte(&o, (unsigned)ql[zigzag[i]]);
    }

    put_byte(&o, 1);

    for (i = 0; i < 64; i++) {
        put_byte(&o, (unsigned)qc[zigzag[i]]);
    }

    put16(&o, 0xFFC0);
    put16(&o, 17);
    put_byte(&o, 8);
    put16(&o, (unsigned)h);
    put16(&o, (unsigned)w);
    put_byte(&o, 3);
    put_byte(&o, 1);
    put_byte(&o, 0x22);
    put_byte(&o, 0);
    put_byte(&o, 2);
    put_byte(&o, 0x11);
    put_byte(&o, 1);
    put_byte(&o, 3);
    put_byte(&o, 0x11);
    put_byte(&o, 1);
    put_dht(&o, 0x00, dc_luma_bits, dc_vals);
    put_dht(&o, 0x10, ac_luma_bits, ac_luma_vals);
    put_dht(&o, 0x01, dc_chroma_bits, dc_vals);
    put_dht(&o, 0x11, ac_chroma_bits, ac_chroma_vals);
    put16(&o, 0xFFDA);
    put16(&o, 12);
    put_byte(&o, 3);
    put_byte(&o, 1);
    put_byte(&o, 0x00);
    put_byte(&o, 2);
    put_byte(&o, 0x11);
    put_byte(&o, 3);
    put_byte(&o, 0x11);
    put_byte(&o, 0);
    put_byte(&o, 63);
    put_byte(&o, 0);

    /* each 16 by 16: its four luma blocks, then the halved Cb and Cr; past the edge, the edge again */
    for (my = 0; my < h && !o.err; my += 16) {
        for (mx = 0; mx < w && !o.err; mx += 16) {
            memset(cb, 0, sizeof(cb));
            memset(cr, 0, sizeof(cr));

            for (by = 0; by < 2; by++) {
                for (bx = 0; bx < 2; bx++) {
                    for (y = 0; y < 8; y++) {
                        for (x = 0; x < 8; x++) {
                            int32_t px = mx + bx * 8 + x, py = my + by * 8 + y;
                            const unsigned char* s;
                            float r, g, b;

                            px = px < w ? px : w - 1;
                            py = py < h ? py : h - 1;
                            s = rgb + ((size_t)py * (size_t)w + (size_t)px) * 3;
                            r = s[0];
                            g = s[1];
                            b = s[2];
                            blk[y * 8 + x] = 0.299f * r + 0.587f * g + 0.114f * b - 128;
                            cb[((by * 8 + y) / 2) * 8 + (bx * 8 + x) / 2] +=
                                0.25f * (-0.168736f * r - 0.331264f * g + 0.5f * b);
                            cr[((by * 8 + y) / 2) * 8 + (bx * 8 + x) / 2] +=
                                0.25f * (0.5f * r - 0.418688f * g - 0.081312f * b);
                        }
                    }

                    code_block(&o, blk, cosu, ql, &dcy, &hdl, &hal);
                }
            }

            code_block(&o, cb, cosu, qc, &dcb, &hdc, &hac);
            code_block(&o, cr, cosu, qc, &dcr, &hdc, &hac);
        }
    }

    if (o.nacc) {   /* the last byte filled with ones */
        put_bits(&o, 0x7F, 8 - o.nacc);
    }

    put16(&o, 0xFFD9);

    if (o.err) {
        free(o.p);
        return 1;
    }

    *out = o.p;
    *outlen = o.n;
    return 0;
}
