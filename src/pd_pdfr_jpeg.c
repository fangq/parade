/*
 * Parade PDF reader: a JPEG decoder for DCTDecode
 *
 * Baseline and progressive Huffman JPEG, 8 bits a sample, any sampling
 * factors, restart intervals; YCbCr, RGB, grey, CMYK and YCCK (Adobe's
 * APP14 says which, and that CMYK is stored inverted). The coefficients of
 * the whole image are kept, so a progressive file is decoded the same way
 * as a baseline one: scans fill them in, the IDCT runs once at the end.
 * The IDCT is the integer one of stb_image (public domain).
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr.h"

typedef struct {
    uint8_t fast[512];          /* 9-bit lookahead: symbol index + 1, 0 none */
    uint16_t code[256];
    uint8_t vals[256];
    uint8_t size[257];
    int32_t maxcode[18];
    int32_t delta[17];
    int present;
} huff;

typedef struct {
    int id, h, v, tq;
    int bw, bh;                 /* blocks across and down, padded to whole MCUs */
    int cw, ch;                 /* blocks the component itself covers */
    int16_t* coef;              /* bw * bh * 64 */
    int dc_pred;
    int dc_tab, ac_tab;
    int eobrun;
    unsigned char* pix;         /* bw*8 x bh*8 */
} jcomp;

typedef struct {
    const unsigned char* p;
    size_t n, i;
    uint32_t acc;
    int nacc;
    int marker;                 /* a marker met in the entropy data */
    huff dc[4], ac[4];
    uint16_t q[4][64];
    jcomp c[4];
    int nc, w, h, hmax, vmax, mcux, mcuy;
    int progressive;
    int restart;
    int adobe, adobe_transform, jfif;
    int ss, se, ah, al;
    int scomp[4], nscomp;
    int err;
} jdec;

static const uint8_t dezig[64 + 15] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
    21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53,
    60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63
};

static int build_huff(huff* h, const uint8_t* counts) {
    int i, j, k = 0, code = 0;

    memset(h, 0, sizeof(*h));

    for (i = 0; i < 16; i++) {
        for (j = 0; j < counts[i]; j++) {
            if (k >= 256) {
                return -1;
            }

            h->size[k++] = (uint8_t)(i + 1);
        }
    }

    h->size[k] = 0;
    k = 0;

    for (j = 1; j <= 16; j++) {
        h->delta[j] = k - code;

        if (h->size[k] == j) {
            while (h->size[k] == j) {
                h->code[k++] = (uint16_t)code++;
            }

            if (code - 1 >= (1 << j)) {
                return -1;
            }
        }

        h->maxcode[j] = code << (16 - j);
        code <<= 1;
    }

    h->maxcode[17] = 0x7fffffff;

    for (i = 0; i < k; i++) {
        int s = h->size[i];

        if (s <= 9) {
            int c = h->code[i] << (9 - s), m = 1 << (9 - s);

            for (j = 0; j < m; j++) {
                h->fast[c + j] = (uint8_t)(i + 1);
            }
        }
    }

    h->present = 1;
    return 0;
}

static void fill(jdec* J) {
    while (J->nacc <= 24) {
        int b = 0;

        if (!J->marker && J->i < J->n) {
            b = J->p[J->i];

            if (b == 0xFF) {
                int c = J->i + 1 < J->n ? J->p[J->i + 1] : 0xD9;

                while (c == 0xFF && J->i + 2 < J->n) {  /* fill bytes */
                    J->i++;
                    c = J->p[J->i + 1];
                }

                if (c != 0) {
                    J->marker = c;
                    b = 0;
                } else {
                    J->i += 2;
                }
            } else {
                J->i++;
            }
        }

        J->acc |= (uint32_t)b << (24 - J->nacc);
        J->nacc += 8;
    }
}

static int getbits(jdec* J, int n) {
    int v;

    if (n == 0) {
        return 0;
    }

    fill(J);
    v = (int)(J->acc >> (32 - n));
    J->acc <<= n;
    J->nacc -= n;
    return v;
}

static int getbit(jdec* J) {
    return getbits(J, 1);
}

static int decode(jdec* J, const huff* h) {
    int k, c;

    fill(J);
    c = (int)(J->acc >> 23);
    k = h->fast[c];

    if (k) {
        int s = h->size[k - 1];

        J->acc <<= s;
        J->nacc -= s;
        return h->vals[k - 1];
    }

    {
        uint32_t t = J->acc >> 16;

        for (k = 10; k <= 16; k++) {
            if (t < (uint32_t)h->maxcode[k]) {
                break;
            }
        }

        if (k > 16) {
            J->err = 1;
            return 0;
        }

        c = (int)(J->acc >> (32 - k)) + h->delta[k];

        if (c < 0 || c >= 256) {
            J->err = 1;
            return 0;
        }

        J->acc <<= k;
        J->nacc -= k;
        return h->vals[c];
    }
}

static int extend(int v, int s) {
    return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int16_t* block_of(jcomp* c, int bx, int by) {
    return c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64;
}

static void block_baseline(jdec* J, jcomp* c, int16_t* blk) {
    int s = decode(J, &J->dc[c->dc_tab]), k = 1, diff;

    if (s > 16) {
        J->err = 1;
        return;
    }

    diff = extend(getbits(J, s), s);
    c->dc_pred += diff;
    blk[0] = (int16_t)c->dc_pred;

    while (k < 64) {
        int rs = decode(J, &J->ac[c->ac_tab]), r = rs >> 4;

        s = rs & 15;

        if (J->err) {
            return;
        }

        if (s == 0) {
            if (r != 15) {
                break;
            }

            k += 16;
            continue;
        }

        k += r;

        if (k > 63) {
            break;
        }

        blk[dezig[k++]] = (int16_t)extend(getbits(J, s), s);
    }
}

static void block_dc_prog(jdec* J, jcomp* c, int16_t* blk) {
    if (J->ah == 0) {
        int s = decode(J, &J->dc[c->dc_tab]);

        if (s > 16) {
            J->err = 1;
            return;
        }

        c->dc_pred += extend(getbits(J, s), s);
        blk[0] = (int16_t)(c->dc_pred * (1 << J->al));
    } else if (getbit(J)) {
        blk[0] = (int16_t)(blk[0] | (1 << J->al));
    }
}

static void block_ac_prog(jdec* J, jcomp* c, int16_t* blk) {
    int k = J->ss;

    if (J->ah == 0) {
        if (c->eobrun) {
            c->eobrun--;
            return;
        }

        while (k <= J->se) {
            int rs = decode(J, &J->ac[c->ac_tab]), r = rs >> 4, s = rs & 15;

            if (J->err) {
                return;
            }

            if (s == 0) {
                if (r < 15) {
                    c->eobrun = (1 << r) - 1;

                    if (r) {
                        c->eobrun += getbits(J, r);
                    }

                    break;
                }

                k += 16;
                continue;
            }

            k += r;

            if (k > 63) {
                break;
            }

            blk[dezig[k++]] = (int16_t)(extend(getbits(J, s), s) * (1 << J->al));
        }

        return;
    }

    /* refinement */
    {
        int p1 = 1 << J->al, m1 = -1 * (1 << J->al);

        if (c->eobrun) {
            for (; k <= J->se; k++) {
                int16_t* z = &blk[dezig[k]];

                if (*z && getbit(J) && (*z & p1) == 0) {
                    *z = (int16_t)(*z >= 0 ? *z + p1 : *z + m1);
                }
            }

            c->eobrun--;
            return;
        }

        while (k <= J->se) {
            int rs = decode(J, &J->ac[c->ac_tab]), r = rs >> 4, s = rs & 15, val = 0;

            if (J->err) {
                return;
            }

            if (s == 0) {
                if (r < 15) {
                    c->eobrun = (1 << r);

                    if (r) {
                        c->eobrun += getbits(J, r);
                    }

                    r = 64;     /* the rest of the band refined below, then the run counted down */
                }
            } else {
                val = getbit(J) ? p1 : m1;
            }

            while (k <= J->se) {
                int16_t* z = &blk[dezig[k++]];

                if (*z) {
                    if (getbit(J) && (*z & p1) == 0) {
                        *z = (int16_t)(*z >= 0 ? *z + p1 : *z + m1);
                    }
                } else {
                    if (r == 0) {
                        *z = (int16_t)val;
                        break;
                    }

                    r--;
                }
            }

            if (c->eobrun) {
                c->eobrun--;
                break;
            }
        }
    }
}

static void reset_scan(jdec* J) {
    int i;

    J->acc = 0;
    J->nacc = 0;
    J->marker = 0;

    for (i = 0; i < J->nc; i++) {
        J->c[i].dc_pred = 0;
        J->c[i].eobrun = 0;
    }
}

static void decode_block(jdec* J, jcomp* c, int bx, int by) {
    int16_t* blk;

    if (bx >= c->bw || by >= c->bh) {
        int16_t dummy[64];

        memset(dummy, 0, sizeof(dummy));

        if (!J->progressive) {
            block_baseline(J, c, dummy);
        }

        return;
    }

    blk = block_of(c, bx, by);

    if (!J->progressive) {
        block_baseline(J, c, blk);
    } else if (J->ss == 0) {
        block_dc_prog(J, c, blk);
    } else {
        block_ac_prog(J, c, blk);
    }
}

static void restart_marker(jdec* J) {
    /* past the RSTn marker the bit reader stopped at */
    if (J->marker >= 0xD0 && J->marker <= 0xD7) {
        J->i += 2;
    } else {
        /* look for it: a damaged stream */
        while (J->i + 1 < J->n && !(J->p[J->i] == 0xFF && J->p[J->i + 1] >= 0xD0 && J->p[J->i + 1] <= 0xD7)) {
            J->i++;
        }

        if (J->i + 1 < J->n) {
            J->i += 2;
        }
    }

    reset_scan(J);
}

static void decode_scan(jdec* J) {
    int n = 0;

    reset_scan(J);

    if (J->nscomp == 1) {
        jcomp* c = &J->c[J->scomp[0]];
        int x, y;

        for (y = 0; y < c->ch; y++) {
            for (x = 0; x < c->cw; x++) {
                decode_block(J, c, x, y);

                if (J->err) {
                    return;
                }

                if (J->restart && ++n % J->restart == 0 && !(y == c->ch - 1 && x == c->cw - 1)) {
                    restart_marker(J);
                }
            }
        }
    } else {
        int mx, my;

        for (my = 0; my < J->mcuy; my++) {
            for (mx = 0; mx < J->mcux; mx++) {
                int k;

                for (k = 0; k < J->nscomp; k++) {
                    jcomp* c = &J->c[J->scomp[k]];
                    int x, y;

                    for (y = 0; y < c->v; y++) {
                        for (x = 0; x < c->h; x++) {
                            decode_block(J, c, mx * c->h + x, my * c->v + y);
                        }
                    }
                }

                if (J->err) {
                    return;
                }

                if (J->restart && ++n % J->restart == 0 && !(my == J->mcuy - 1 && mx == J->mcux - 1)) {
                    restart_marker(J);
                }
            }
        }
    }

    /* to the marker after the data */
    while (J->i + 1 < J->n && !(J->p[J->i] == 0xFF && J->p[J->i + 1] != 0 && !(J->p[J->i + 1] >= 0xD0 &&
                                J->p[J->i + 1] <= 0xD7))) {
        J->i++;
    }
}

/* ---- IDCT (stb_image's) ---- */

#define F2F(x) ((int)(((x) * 4096 + 0.5)))
#define FSH(x) ((x) * 4096)
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    int64_t t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
    p2 = s2; \
    p3 = s6; \
    p1 = (p2 + p3) * F2F(0.5411961f); \
    t2 = p1 + p3 * F2F(-1.847759065f); \
    t3 = p1 + p2 * F2F(0.765366865f); \
    p2 = s0; \
    p3 = s4; \
    t0 = FSH(p2 + p3); \
    t1 = FSH(p2 - p3); \
    x0 = t0 + t3; \
    x3 = t0 - t3; \
    x1 = t1 + t2; \
    x2 = t1 - t2; \
    t0 = s7; \
    t1 = s5; \
    t2 = s3; \
    t3 = s1; \
    p3 = t0 + t2; \
    p4 = t1 + t3; \
    p1 = t0 + t3; \
    p2 = t1 + t2; \
    p5 = (p3 + p4) * F2F(1.175875602f); \
    t0 = t0 * F2F(0.298631336f); \
    t1 = t1 * F2F(2.053119869f); \
    t2 = t2 * F2F(3.072711026f); \
    t3 = t3 * F2F(1.501321110f); \
    p1 = p5 + p1 * F2F(-0.899976223f); \
    p2 = p5 + p2 * F2F(-2.562915447f); \
    p3 = p3 * F2F(-1.961570560f); \
    p4 = p4 * F2F(-0.390180644f); \
    t3 += p1 + p4; \
    t2 += p2 + p3; \
    t1 += p2 + p4; \
    t0 += p1 + p3;

static unsigned char clampb(int64_t x) {
    return (unsigned char)(x < 0 ? 0 : x > 255 ? 255 : x);
}

/* a damaged stream's coefficients kept within what eight bits of samples can come from */
static int clampv(int64_t x) {
    return (int)(x < -(1 << 24) ? -(1 << 24) : x > (1 << 24) ? (1 << 24) : x);
}

static void idct_block(unsigned char* out, int stride, const int16_t* in, const uint16_t* q) {
    int i, val[64], *v = val, d[64];
    unsigned char* o;

    for (i = 0; i < 64; i++) {
        d[i] = clampv((int64_t)in[i] * q[i]);
    }

    for (i = 0; i < 8; i++, v++) {
        const int* c = d + i;

        if (c[8] == 0 && c[16] == 0 && c[24] == 0 && c[32] == 0 && c[40] == 0 && c[48] == 0 && c[56] == 0) {
            int dc = clampv((int64_t)c[0] * 4);

            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D(c[0], c[8], c[16], c[24], c[32], c[40], c[48], c[56])
            x0 += 512;
            x1 += 512;
            x2 += 512;
            x3 += 512;
            v[0] = clampv((x0 + t3) >> 10);
            v[56] = clampv((x0 - t3) >> 10);
            v[8] = clampv((x1 + t2) >> 10);
            v[48] = clampv((x1 - t2) >> 10);
            v[16] = clampv((x2 + t1) >> 10);
            v[40] = clampv((x2 - t1) >> 10);
            v[24] = clampv((x3 + t0) >> 10);
            v[32] = clampv((x3 - t0) >> 10);
        }
    }

    for (i = 0, v = val, o = out; i < 8; i++, v += 8, o += stride) {
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17);
        x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17);
        x3 += 65536 + (128 << 17);
        o[0] = clampb((x0 + t3) >> 17);
        o[7] = clampb((x0 - t3) >> 17);
        o[1] = clampb((x1 + t2) >> 17);
        o[6] = clampb((x1 - t2) >> 17);
        o[2] = clampb((x2 + t1) >> 17);
        o[5] = clampb((x2 - t1) >> 17);
        o[3] = clampb((x3 + t0) >> 17);
        o[4] = clampb((x3 - t0) >> 17);
    }
}

static int u16(const unsigned char* p) {
    return (p[0] << 8) | p[1];
}

unsigned char* pd_jpeg_decode(const unsigned char* data, size_t n, int32_t* ow, int32_t* oh, int32_t* ocomps,
                              int* adobe_inverted) {
    jdec* J = (jdec*)calloc(1, sizeof(jdec));
    unsigned char* out = NULL;
    int frame = 0, i;

    if (!J) {
        return NULL;
    }

    J->p = data;
    J->n = n;
    J->adobe_transform = -1;

    /* the start of image, past anything before it */
    while (J->i + 1 < n && !(data[J->i] == 0xFF && data[J->i + 1] == 0xD8)) {
        J->i++;
    }

    J->i += 2;

    while (J->i + 3 < n && !J->err) {
        int m, len;
        const unsigned char* s;

        if (data[J->i] != 0xFF) {
            J->i++;
            continue;
        }

        m = data[J->i + 1];

        if (m == 0xFF) {
            J->i++;
            continue;
        }

        if (m == 0xD9) {
            break;
        }

        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
            J->i += 2;
            continue;
        }

        len = u16(data + J->i + 2);

        if (len < 2 || J->i + 2 + (size_t)len > n) {
            break;
        }

        s = data + J->i + 4;
        len -= 2;

        switch (m) {
            case 0xC4: {    /* DHT */
                int k = 0;

                while (k + 17 <= len) {
                    int tc = s[k] >> 4, th = s[k] & 15, total = 0, j;
                    huff* h;

                    for (j = 0; j < 16; j++) {
                        total += s[k + 1 + j];
                    }

                    if (th > 3 || total > 256 || k + 17 + total > len) {
                        break;
                    }

                    h = tc ? &J->ac[th] : &J->dc[th];

                    if (build_huff(h, s + k + 1) == 0) {
                        memcpy(h->vals, s + k + 17, (size_t)total);
                    }

                    k += 17 + total;
                }

                break;
            }

            case 0xDB: {    /* DQT */
                int k = 0;

                while (k < len) {
                    int pq = s[k] >> 4, tq = s[k] & 15, j;

                    if (tq > 3) {
                        break;
                    }

                    for (j = 0; j < 64 && k + 1 + (pq ? 2 * j + 1 : j) < len; j++) {
                        J->q[tq][dezig[j]] = (uint16_t)(pq ? u16(s + k + 1 + 2 * j) : s[k + 1 + j]);
                    }

                    k += 1 + 64 * (pq ? 2 : 1);
                }

                break;
            }

            case 0xDD:      /* DRI */
                J->restart = u16(s);
                break;

            case 0xE0:
                if (len >= 4 && memcmp(s, "JFIF", 4) == 0) {
                    J->jfif = 1;
                }

                break;

            case 0xEE:
                if (len >= 12 && memcmp(s, "Adobe", 5) == 0) {
                    J->adobe = 1;
                    J->adobe_transform = s[11];
                }

                break;

            case 0xC0: case 0xC1: case 0xC2: {  /* SOF: baseline, extended, progressive */
                int k;

                if (frame || len < 6 || s[0] != 8) {
                    J->err = 1;
                    break;
                }

                frame = 1;
                J->progressive = m == 0xC2;
                J->h = u16(s + 1);
                J->w = u16(s + 3);
                J->nc = s[5];

                if (J->w <= 0 || J->nc < 1 || J->nc > 4 || len < 6 + 3 * J->nc ||
                    (int64_t)J->w * (J->h ? J->h : 1) > ((int64_t)1 << 28)) {
                    J->err = 1;
                    break;
                }

                J->hmax = J->vmax = 1;

                for (k = 0; k < J->nc; k++) {
                    jcomp* c = &J->c[k];

                    c->id = s[6 + 3 * k];
                    c->h = s[7 + 3 * k] >> 4;
                    c->v = s[7 + 3 * k] & 15;
                    c->tq = s[8 + 3 * k] & 3;

                    if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) {
                        J->err = 1;
                    }

                    J->hmax = c->h > J->hmax ? c->h : J->hmax;
                    J->vmax = c->v > J->vmax ? c->v : J->vmax;
                }

                if (J->err) {
                    break;
                }

                if (J->h == 0) {    /* the height given by a DNL later: guess from the data's size */
                    J->h = J->w;
                }

                J->mcux = (J->w + 8 * J->hmax - 1) / (8 * J->hmax);
                J->mcuy = (J->h + 8 * J->vmax - 1) / (8 * J->vmax);

                for (k = 0; k < J->nc; k++) {
                    jcomp* c = &J->c[k];

                    c->bw = J->mcux * c->h;
                    c->bh = J->mcuy * c->v;
                    c->cw = ((J->w * c->h + J->hmax - 1) / J->hmax + 7) / 8;
                    c->ch = ((J->h * c->v + J->vmax - 1) / J->vmax + 7) / 8;
                    c->coef = (int16_t*)calloc((size_t)c->bw * (size_t)c->bh * 64, sizeof(int16_t));

                    if (!c->coef) {
                        J->err = 1;
                    }
                }

                break;
            }

            case 0xDA: {    /* SOS */
                int k, ns = s[0];

                if (!frame || ns < 1 || ns > 4 || len < 4 + 2 * ns) {
                    J->err = 1;
                    break;
                }

                J->nscomp = ns;

                for (k = 0; k < ns; k++) {
                    int id = s[1 + 2 * k], j;

                    J->scomp[k] = 0;

                    for (j = 0; j < J->nc; j++) {
                        if (J->c[j].id == id) {
                            J->scomp[k] = j;
                        }
                    }

                    J->c[J->scomp[k]].dc_tab = s[2 + 2 * k] >> 4 & 3;
                    J->c[J->scomp[k]].ac_tab = s[2 + 2 * k] & 3;
                }

                J->ss = s[1 + 2 * ns];
                J->se = s[2 + 2 * ns];
                J->ah = s[3 + 2 * ns] >> 4;
                J->al = s[3 + 2 * ns] & 15;

                if (!J->progressive) {
                    J->ss = 0;
                    J->se = 63;
                    J->ah = J->al = 0;
                }

                if (J->se > 63) {
                    J->se = 63;
                }

                J->i += 2 + 2 + (size_t)len;
                decode_scan(J);
                J->err = 0;     /* a damaged scan: what it gave is kept */
                continue;
            }

            default:
                if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                    J->err = 1;     /* lossless, hierarchical or arithmetic: not done */
                }

                break;
        }

        J->i += 2 + 2 + (size_t)len;
    }

    if (frame && !J->err) {
        int k;

        /* each component's pixels */
        for (k = 0; k < J->nc && !J->err; k++) {
            jcomp* c = &J->c[k];
            int x, y, stride = c->bw * 8;

            c->pix = (unsigned char*)malloc((size_t)stride * (size_t)c->bh * 8);

            if (!c->pix) {
                J->err = 1;
                break;
            }

            for (y = 0; y < c->bh; y++) {
                for (x = 0; x < c->bw; x++) {
                    idct_block(c->pix + (size_t)y * 8 * (size_t)stride + (size_t)x * 8, stride, block_of(c, x, y),
                               J->q[c->tq]);
                }
            }
        }

        if (!J->err) {
            int comps = J->nc == 2 ? 1 : J->nc, x, y;
            int transform = J->nc == 3 ? 1 : 0;

            if (J->nc == 3 && J->adobe && J->adobe_transform == 0) {
                transform = 0;
            } else if (J->nc == 3 && !J->jfif && !J->adobe && J->c[0].id == 'R' && J->c[1].id == 'G' &&
                       J->c[2].id == 'B') {
                transform = 0;
            }

            if (J->nc == 4) {
                transform = J->adobe && J->adobe_transform == 2 ? 2 : 0;
            }

            out = (unsigned char*)malloc((size_t)J->w * (size_t)J->h * (size_t)comps);

            if (out) {
                for (y = 0; y < J->h; y++) {
                    unsigned char* o = out + (size_t)y * (size_t)J->w * (size_t)comps;

                    for (x = 0; x < J->w; x++) {
                        int v[4], q;

                        for (q = 0; q < comps; q++) {
                            jcomp* c = &J->c[q];
                            int sx, sy;

                            /* upsampled by replication, the sample whose area holds the pixel's middle */
                            sx = x * c->h / J->hmax;
                            sy = y * c->v / J->vmax;
                            v[q] = c->pix[(size_t)sy * (size_t)c->bw * 8 + (size_t)sx];
                        }

                        if (transform) {
                            int Y = v[0], cb = v[1] - 128, cr = v[2] - 128;

                            v[0] = clampb(Y + ((91881 * cr + 32768) >> 16));
                            v[1] = clampb(Y - ((22554 * cb + 46802 * cr - 32768) >> 16));
                            v[2] = clampb(Y + ((116130 * cb + 32768) >> 16));

                            if (transform == 2) {
                                v[0] = 255 - v[0];
                                v[1] = 255 - v[1];
                                v[2] = 255 - v[2];
                            }
                        }

                        if (comps == 4 && J->adobe) {   /* Adobe's CMYK is stored inverted */
                            for (q = 0; q < 4; q++) {
                                v[q] = 255 - v[q];
                            }
                        }

                        for (q = 0; q < comps; q++) {
                            o[x * comps + q] = (unsigned char)v[q];
                        }
                    }
                }

                *ow = J->w;
                *oh = J->h;
                *ocomps = comps;

                if (adobe_inverted) {
                    *adobe_inverted = comps == 4 && J->adobe;
                }
            }
        }
    }

    for (i = 0; i < 4; i++) {
        free(J->c[i].coef);
        free(J->c[i].pix);
    }

    free(J);
    return out;
}
