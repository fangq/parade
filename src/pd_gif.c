/*
 * Parade GIF animation: every frame of a GIF as it is shown
 *
 * mimamo's GIF reader (mmm_image.hpp: gif_parse, gif_lzw, gif_draw; by the
 * same author, BSD), ported to C. The blocks are read once for where each
 * image is; the images are then drawn one over another as a viewer shows
 * them, what each frame asks become of it before the next (left, cleared,
 * put back) done. Unlike mimamo's, which keeps MATLAB's opaque indexed
 * screen, the screen here has alpha: cleared and see-through pixels are
 * transparent, as a browser and PowerPoint show an animated GIF on a slide.
 *
 * A file cut short keeps the frames before the cut.
 */

#include <stdlib.h>
#include <string.h>
#include "parade_doc.h"

typedef struct {
    uint32_t x, y, w, h;
    int interlaced;
    const unsigned char* pal;   /* its own colour table, or NULL */
    int npal;                   /* colours in it */
    int transparent;            /* the index left undrawn, -1 none */
    int delay;                  /* hundredths of a second */
    int disposal;
    int min_code;
    size_t data, end;           /* its image data's sub-blocks */
} gframe;

typedef struct {
    uint32_t w, h;
    const unsigned char* pal;   /* the global colour table, or NULL */
    int npal;
    int loops;                  /* NETSCAPE2.0's count, 0 for ever, -1 absent */
    gframe* frames;
    int32_t nframes;
} gif;

static uint32_t le16(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* past a run of sub-blocks, each a length and that many bytes */
static size_t skip_blocks(const unsigned char* b, size_t n, size_t p) {
    while (p < n && b[p] != 0) {
        p += 1u + b[p];
    }

    return p + 1 < n ? p + 1 : n;
}

static int gif_parse(const unsigned char* b, size_t n, gif* g) {
    size_t at = 13;
    gframe pending;
    int32_t cap = 0;

    memset(g, 0, sizeof(*g));
    g->loops = -1;

    if (n < 13 || memcmp(b, "GIF8", 4) != 0 || (b[4] != '7' && b[4] != '9') || b[5] != 'a') {
        return -1;
    }

    g->w = le16(b + 6);
    g->h = le16(b + 8);

    if (b[10] & 0x80u) {
        size_t k = (size_t)3 << ((b[10] & 7u) + 1u);

        if (at + k > n) {
            return -1;
        }

        g->pal = b + at;
        g->npal = (int)(k / 3);
        at += k;
    }

    memset(&pending, 0, sizeof(pending));
    pending.transparent = -1;

    while (at < n) {
        unsigned char tag = b[at++];

        if (tag == 0x3B) {
            break;
        }

        if (tag == 0x21) {
            unsigned char label;

            if (at >= n) {
                break;
            }

            label = b[at++];

            if (label == 0xF9 && at + 5 <= n && b[at] >= 4) {
                unsigned p = b[at + 1];

                pending.disposal = (int)((p >> 2) & 7u);
                pending.delay = (int)le16(b + at + 2);
                pending.transparent = (p & 1u) ? b[at + 4] : -1;
            } else if (label == 0xFF && at + 16 <= n && b[at] == 11 &&
                       (memcmp(b + at + 1, "NETSCAPE2.0", 11) == 0 || memcmp(b + at + 1, "ANIMEXTS1.0", 11) == 0) &&
                       b[at + 12] >= 3 && b[at + 13] == 1) {
                g->loops = (int)le16(b + at + 14);
            }

            at = skip_blocks(b, n, at);
            continue;
        }

        /* anything else is not a GIF block: what came before it stands */
        if (tag != 0x2C || at + 9 > n) {
            break;
        }

        {
            gframe f = pending;
            unsigned p = b[at + 8];

            memset(&pending, 0, sizeof(pending));
            pending.transparent = -1;
            f.x = le16(b + at);
            f.y = le16(b + at + 2);
            f.w = le16(b + at + 4);
            f.h = le16(b + at + 6);
            at += 9;
            f.interlaced = (p & 0x40u) != 0;
            f.pal = NULL;
            f.npal = 0;

            if (p & 0x80u) {
                size_t k = (size_t)3 << ((p & 7u) + 1u);

                if (at + k > n) {
                    break;
                }

                f.pal = b + at;
                f.npal = (int)(k / 3);
                at += k;
            }

            if (at >= n) {
                break;
            }

            f.min_code = b[at++];
            f.data = at;
            at = skip_blocks(b, n, at);
            f.end = at;

            if (g->nframes == cap) {
                int32_t nc = cap ? cap * 2 : 16;
                gframe* q = (gframe*)realloc(g->frames, sizeof(gframe) * (size_t)nc);

                if (!q) {
                    break;
                }

                g->frames = q;
                cap = nc;
            }

            g->frames[g->nframes++] = f;
        }
    }

    if (g->nframes == 0 || g->w == 0 || g->h == 0) {
        free(g->frames);
        g->frames = NULL;
        return -1;
    }

    return 0;
}

/* GIF's LZW: codes least significant bit first, no early change; up to want indices into out, how many */
static size_t gif_lzw(const unsigned char* src, size_t n, int min_code, unsigned char* out, size_t want) {
    static const int MAXC = 4096;
    uint16_t* prefix, *len;
    unsigned char* suffix, *first;
    int clear, eoi, next, width, prev = -1, nbits = 0, i;
    uint32_t acc = 0;
    size_t at = 0, got = 0;

    if (min_code < 1 || min_code > 11) {
        return 0;
    }

    prefix = (uint16_t*)malloc(sizeof(uint16_t) * MAXC);
    len = (uint16_t*)malloc(sizeof(uint16_t) * MAXC);
    suffix = (unsigned char*)malloc(MAXC);
    first = (unsigned char*)malloc(MAXC);

    if (!prefix || !len || !suffix || !first) {
        free(prefix);
        free(len);
        free(suffix);
        free(first);
        return 0;
    }

    clear = 1 << min_code;
    eoi = clear + 1;

    for (i = 0; i < clear; i++) {
        suffix[i] = first[i] = (unsigned char)i;
        len[i] = 1;
        prefix[i] = 0;
    }

    next = clear + 2;
    width = min_code + 1;

    while (got < want) {
        int code, c;
        size_t l, k;

        while (nbits < width && at < n) {
            acc |= (uint32_t)src[at++] << nbits;
            nbits += 8;
        }

        if (nbits < width) {
            break;
        }

        code = (int)(acc & ((1u << width) - 1u));
        acc >>= width;
        nbits -= width;

        if (code == clear) {
            next = clear + 2;
            width = min_code + 1;
            prev = -1;
            continue;
        }

        if (code == eoi) {
            break;
        }

        if (prev < 0) {
            if (code > clear) {
                break;
            }

            out[got++] = (unsigned char)code;
            prev = code;
            continue;
        }

        /* a code not yet in the table is damage, bar the one being made */
        if (code > next || (code == next && next >= MAXC)) {
            break;
        }

        if (next < MAXC) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = code == next ? first[prev] : first[code];
            first[next] = first[prev];
            len[next] = (uint16_t)(len[prev] + 1);
            next++;

            if (next == (1 << width) && width < 12) {
                width++;
            }
        }

        /* the string written from its end */
        l = len[code];

        if (got + l > want) {
            /* only what fits: the string's start */
            unsigned char tmp[4096];

            c = code;

            for (k = l; k-- > 0;) {
                tmp[k] = suffix[c];
                c = prefix[c];
            }

            memcpy(out + got, tmp, want - got);
            got = want;
            break;
        }

        c = code;

        for (k = l; k-- > 0;) {
            out[got + k] = suffix[c];
            c = prefix[c];
        }

        got += l;
        prev = code;
    }

    free(prefix);
    free(len);
    free(suffix);
    free(first);
    return got;
}

/* frame k drawn on the screen (RGBA, straight alpha), the frames before it drawn already; saved holds the screen as
   it was before a frame that asks to be put back. 0 on success */
static int gif_draw(const gif* g, const unsigned char* b, size_t n, int32_t k, unsigned char* screen,
                    unsigned char** saved) {
    const gframe* f = &g->frames[k];
    size_t W = g->w, H = g->h, want, got, s, y, x, rows_done = 0;
    const unsigned char* pal = f->pal ? f->pal : g->pal;
    int npal = f->pal ? f->npal : g->npal;
    unsigned char* data, *px;
    size_t nd = 0, p;
    uint32_t r;
    static const uint32_t start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };

    /* what the frame before asked to become of it */
    if (k > 0) {
        const gframe* q = &g->frames[k - 1];

        if (q->disposal == 2) {     /* cleared: see-through */
            for (y = q->y; y < H && y < (size_t)q->y + q->h; y++) {
                for (x = q->x; x < W && x < (size_t)q->x + q->w; x++) {
                    memset(screen + (y * W + x) * 4, 0, 4);
                }
            }
        } else if (q->disposal == 3 && *saved) {
            memcpy(screen, *saved, W * H * 4);
        }
    }

    if (f->disposal == 3) {
        if (!*saved) {
            *saved = (unsigned char*)malloc(W * H * 4);
        }

        if (*saved) {
            memcpy(*saved, screen, W * H * 4);
        }
    }

    /* the sub-blocks joined, and decoded: no more than the screen holds (a frame's own size is the file's word) */
    data = (unsigned char*)malloc(f->end > f->data ? f->end - f->data : 1);

    if (!data) {
        return -1;
    }

    for (p = f->data; p < f->end && p < n && b[p] != 0 && p + 1 + b[p] <= n; p += 1u + b[p]) {
        memcpy(data + nd, b + p + 1, b[p]);
        nd += b[p];
    }

    want = (size_t)f->w * f->h;
    want = want < W * H ? want : W * H;
    px = (unsigned char*)malloc(want ? want : 1);

    if (!px) {
        free(data);
        return -1;
    }

    got = gif_lzw(data, nd, f->min_code, px, want);
    free(data);

    /* an interlaced image's rows come every eighth from 0, every eighth from 4, every fourth from 2, then every
       second from 1 */
    for (s = 0; s < 4 || !f->interlaced; s++) {
        uint32_t r0 = f->interlaced ? start[s] : 0, dr = f->interlaced ? step[s] : 1;

        for (r = r0; r < f->h; r += dr) {
            y = f->y + (size_t)r;

            for (x = 0; x < f->w; x++) {
                size_t src = rows_done * f->w + x, X = f->x + x, i;
                unsigned char v;

                if (src >= got || X >= W || y >= H) {
                    continue;
                }

                v = px[src];

                if ((int)v == f->transparent || (int)v >= npal || !pal) {
                    continue;   /* see-through: what was there shows */
                }

                i = (y * W + X) * 4;
                screen[i] = pal[3 * v];
                screen[i + 1] = pal[3 * v + 1];
                screen[i + 2] = pal[3 * v + 2];
                screen[i + 3] = 255;
            }

            rows_done++;
        }

        if (!f->interlaced) {
            break;
        }
    }

    free(px);
    return 0;
}

pd_status pd_gif_info(const void* data, size_t len, int32_t* width, int32_t* height, int32_t* frames,
                      int32_t* loops) {
    gif g;

    if (!data) {
        return PD_ERR_ARG;
    }

    if (gif_parse((const unsigned char*)data, len, &g) != 0) {
        return PD_ERR_FORMAT;
    }

    if (width) {
        *width = (int32_t)g.w;
    }

    if (height) {
        *height = (int32_t)g.h;
    }

    if (frames) {
        *frames = g.nframes;
    }

    if (loops) {
        *loops = g.loops;
    }

    free(g.frames);
    return PD_OK;
}

pd_status pd_gif_each_frame(const void* data, size_t len, pd_gif_frame_fn fn, void* user) {
    const unsigned char* b = (const unsigned char*)data;
    gif g;
    unsigned char* screen, *saved = NULL;
    int32_t k;
    pd_status st = PD_OK;

    if (!data || !fn) {
        return PD_ERR_ARG;
    }

    if (gif_parse(b, len, &g) != 0) {
        return PD_ERR_FORMAT;
    }

    if ((size_t)g.w * g.h > ((size_t)1 << 28) || (screen = (unsigned char*)calloc((size_t)g.w * g.h, 4)) == NULL) {
        free(g.frames);
        return PD_ERR_NOMEM;
    }

    for (k = 0; k < g.nframes; k++) {
        int d = g.frames[k].delay;

        if (gif_draw(&g, b, len, k, screen, &saved) != 0) {
            st = PD_ERR_NOMEM;
            break;
        }

        if (fn(user, k, screen, d <= 1 ? 100 : d * 10) != 0) {
            break;
        }
    }

    free(screen);
    free(saved);
    free(g.frames);
    return st;
}

pd_status pd_gif_frames(const void* data, size_t len, int32_t first, int32_t count, uint8_t* rgba,
                        int32_t* delays_ms) {
    const unsigned char* b = (const unsigned char*)data;
    gif g;
    unsigned char* screen, *saved = NULL;
    size_t W, H;
    int32_t k;
    pd_status st = PD_OK;

    if (!data || !rgba || first < 0 || count <= 0) {
        return PD_ERR_ARG;
    }

    if (gif_parse(b, len, &g) != 0) {
        return PD_ERR_FORMAT;
    }

    W = g.w;
    H = g.h;

    if (first + (int64_t)count > g.nframes || W * H > ((size_t)1 << 28)) {
        free(g.frames);
        return PD_ERR_RANGE;
    }

    screen = (unsigned char*)calloc(W * H, 4);     /* the screen before the first: see-through */

    if (!screen) {
        free(g.frames);
        return PD_ERR_NOMEM;
    }

    for (k = 0; k < first + count; k++) {
        if (gif_draw(&g, b, len, k, screen, &saved) != 0) {
            st = PD_ERR_NOMEM;
            break;
        }

        if (k >= first) {
            memcpy(rgba + (size_t)(k - first) * W * H * 4, screen, W * H * 4);

            if (delays_ms) {
                /* a delay of 0 or 1 hundredths is shown as a tenth of a second, as browsers do */
                int d = g.frames[k].delay;

                delays_ms[k - first] = d <= 1 ? 100 : d * 10;
            }
        }
    }

    free(screen);
    free(saved);
    free(g.frames);
    return st;
}
