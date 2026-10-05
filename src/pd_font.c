/*
 * Parade font reader: just the metrics layout needs.
 *
 * Reads head, hhea, maxp, OS/2, hmtx, cmap (formats 4 and 12), the legacy
 * kern table (format 0) and GPOS pair kerning (PairPos formats 1 and 2,
 * also through Extension lookups). Outlines are never touched: rendering
 * belongs to the host. Every read is bounds-checked against the file, so
 * a malformed font yields wrong numbers, never an out-of-bounds access.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

/* ------------------------------------------------------------------ */
/* bounds-checked big-endian readers                                  */
/* ------------------------------------------------------------------ */

static uint32_t U16(const pd_font* f, uint64_t off) {
    return off + 2 <= f->len ? ((uint32_t)f->data[off] << 8) | f->data[off + 1] : 0;
}
static int32_t S16(const pd_font* f, uint64_t off) {
    return (int16_t)U16(f, off);
}
static uint32_t U32(const pd_font* f, uint64_t off) {
    return off + 4 <= f->len ? ((uint32_t)f->data[off] << 24) | ((uint32_t)f->data[off + 1] << 16) |
           ((uint32_t)f->data[off + 2] << 8) | f->data[off + 3] : 0;
}
#define TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* find a table in the directory at base; returns offset, 0 if absent */
static uint32_t find_table(const pd_font* f, uint32_t base, uint32_t tag, uint32_t* len) {
    uint32_t n = U16(f, base + 4), i;

    for (i = 0; i < n; i++) {
        uint64_t rec = (uint64_t)base + 12 + 16 * i;

        if (U32(f, rec) == tag) {
            uint32_t off = U32(f, rec + 8), l = U32(f, rec + 12);

            if ((uint64_t)off + l > f->len) {
                return 0;
            }

            if (len) {
                *len = l;
            }

            return off;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* cmap                                                               */
/* ------------------------------------------------------------------ */

static void select_cmap(pd_font* f, uint32_t cmap) {
    uint32_t n = U16(f, cmap + 2), i, best = 0;
    int32_t best_rank = 0;

    for (i = 0; i < n; i++) {
        uint32_t plat = U16(f, cmap + 4 + 8 * i), enc = U16(f, cmap + 6 + 8 * i);
        uint32_t sub = cmap + U32(f, cmap + 8 + 8 * i);
        uint32_t fmt = U16(f, sub);
        int32_t rank = 0;

        if (fmt == 12 && ((plat == 3 && enc == 10) || plat == 0)) {
            rank = 4;
        } else if (fmt == 4 && ((plat == 3 && enc == 1) || plat == 0)) {
            rank = 3;
        } else if (fmt == 4 && plat == 3 && enc == 0) {
            rank = 2;
        }

        if (rank > best_rank) {
            best_rank = rank;
            best = sub;
            f->cmap_symbol = (rank == 2);
        }
    }

    f->cmap_sub = best;
    f->cmap_format = best ? (int32_t)U16(f, best) : 0;
}

static uint32_t cmap_lookup(const pd_font* f, uint32_t cp) {
    uint32_t s = f->cmap_sub;

    if (!s) {
        return 0;
    }

    if (f->cmap_format == 12) {
        uint32_t lo = 0, hi = U32(f, s + 12);

        if (hi > f->len / 12) {
            hi = (uint32_t)(f->len / 12);
        }

        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint64_t g = (uint64_t)s + 16 + 12 * mid;
            uint32_t start = U32(f, g), end = U32(f, g + 4);

            if (cp < start) {
                hi = mid;
            } else if (cp > end) {
                lo = mid + 1;
            } else {
                return U32(f, g + 8) + (cp - start);
            }
        }

        return 0;
    }

    if (f->cmap_format == 4) {
        uint32_t segs = U16(f, s + 6) / 2, lo = 0, hi = segs;
        uint64_t ends = (uint64_t)s + 14, starts = ends + 2 * segs + 2;
        uint64_t deltas = starts + 2 * segs, ranges = deltas + 2 * segs;

        if (cp > 0xFFFF) {
            return 0;
        }

        while (lo < hi) {   /* first segment with end >= cp */
            uint32_t mid = (lo + hi) / 2;

            if (U16(f, ends + 2 * mid) < cp) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        if (lo < segs && U16(f, starts + 2 * lo) <= cp) {
            uint32_t start = U16(f, starts + 2 * lo), delta = U16(f, deltas + 2 * lo);
            uint32_t ro = U16(f, ranges + 2 * lo), g;

            if (ro == 0) {
                return (cp + delta) & 0xFFFF;
            }

            g = U16(f, ranges + 2 * lo + ro + 2 * (cp - start));
            return g ? (g + delta) & 0xFFFF : 0;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* kerning                                                            */
/* ------------------------------------------------------------------ */

static void setup_kern(pd_font* f, uint32_t kern, uint32_t klen) {
    uint32_t n, i;
    uint64_t sub;

    if (!kern || U16(f, kern) != 0) {   /* only the Microsoft version-0 table */
        return;
    }

    n = U16(f, kern + 2);
    sub = (uint64_t)kern + 4;

    for (i = 0; i < n && sub < (uint64_t)kern + klen; i++) {
        uint32_t len = U16(f, sub + 2), cov = U16(f, sub + 4);

        /* format 0, horizontal, not minimum, not cross-stream */
        if ((cov >> 8) == 0 && (cov & 0x7) == 0x1) {
            f->kern_npairs = U16(f, sub + 6);
            f->kern_pairs = (uint32_t)(sub + 14);
            return;
        }

        if (len == 0) {
            break;
        }

        sub += len;
    }
}

static int32_t kern_legacy(const pd_font* f, uint32_t l, uint32_t r) {
    uint32_t key = (l << 16) | r, lo = 0, hi = f->kern_npairs;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint64_t rec = (uint64_t)f->kern_pairs + 6 * mid;
        uint32_t k = U32(f, rec);

        if (k < key) {
            lo = mid + 1;
        } else if (k > key) {
            hi = mid;
        } else {
            return S16(f, rec + 4);
        }
    }

    return 0;
}

/* coverage index of a glyph, -1 if not covered */
static int32_t coverage(const pd_font* f, uint32_t cov, uint32_t g) {
    uint32_t fmt = U16(f, cov), lo = 0, hi = U16(f, cov + 2);

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;

        if (fmt == 1) {
            uint32_t v = U16(f, (uint64_t)cov + 4 + 2 * mid);

            if (v < g) {
                lo = mid + 1;
            } else if (v > g) {
                hi = mid;
            } else {
                return (int32_t)mid;
            }
        } else if (fmt == 2) {
            uint64_t rr = (uint64_t)cov + 4 + 6 * mid;

            if (U16(f, rr + 2) < g) {
                lo = mid + 1;
            } else if (U16(f, rr) > g) {
                hi = mid;
            } else {
                return (int32_t)(U16(f, rr + 4) + g - U16(f, rr));
            }
        } else {
            return -1;
        }
    }

    return -1;
}

static uint32_t class_of(const pd_font* f, uint32_t cd, uint32_t g) {
    uint32_t fmt = U16(f, cd);

    if (fmt == 1) {
        uint32_t start = U16(f, cd + 2), n = U16(f, cd + 4);
        return (g >= start && g < start + n) ? U16(f, (uint64_t)cd + 6 + 2 * (g - start)) : 0;
    }

    if (fmt == 2) {
        uint32_t lo = 0, hi = U16(f, cd + 2);

        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint64_t rr = (uint64_t)cd + 4 + 6 * mid;

            if (U16(f, rr + 2) < g) {
                lo = mid + 1;
            } else if (U16(f, rr) > g) {
                hi = mid;
            } else {
                return U16(f, rr + 4);
            }
        }
    }

    return 0;
}

static uint32_t popcount16(uint32_t v) {
    uint32_t c = 0;

    for (; v; v &= v - 1) {
        c++;
    }

    return c;
}

/* XAdvance of value record 1 in a PairPos subtable; *hit = 1 if the pair applies */
static int32_t pairpos(const pd_font* f, uint32_t sub, uint32_t l, uint32_t r, int* hit) {
    uint32_t fmt = U16(f, sub), vf1 = U16(f, sub + 4), vf2 = U16(f, sub + 6);
    uint32_t s1 = 2 * popcount16(vf1), s2 = 2 * popcount16(vf2);
    uint32_t xadv = 2 * popcount16(vf1 & 0x3);     /* XPlacement, YPlacement precede XAdvance */
    int32_t ci = coverage(f, sub + U16(f, sub + 2), l);

    *hit = 0;

    if (ci < 0) {
        return 0;
    }

    if (fmt == 1) {
        uint32_t set, lo = 0, hi, rs = 2 + s1 + s2;

        if ((uint32_t)ci >= U16(f, sub + 8)) {
            return 0;
        }

        set = sub + U16(f, (uint64_t)sub + 10 + 2 * ci);
        hi = U16(f, set);

        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint64_t rec = (uint64_t)set + 2 + rs * mid;
            uint32_t g2 = U16(f, rec);

            if (g2 < r) {
                lo = mid + 1;
            } else if (g2 > r) {
                hi = mid;
            } else {
                *hit = 1;
                return (vf1 & 0x4) ? S16(f, rec + 2 + xadv) : 0;
            }
        }

        return 0;
    }

    if (fmt == 2) {
        uint32_t c1 = class_of(f, sub + U16(f, sub + 8), l);
        uint32_t c2 = class_of(f, sub + U16(f, sub + 10), r);
        uint32_t n1 = U16(f, sub + 12), n2 = U16(f, sub + 14);

        if (c1 >= n1 || c2 >= n2) {
            return 0;
        }

        *hit = 1;
        return (vf1 & 0x4) ? S16(f, (uint64_t)sub + 16 + ((uint64_t)c1 * n2 + c2) * (s1 + s2) + xadv) : 0;
    }

    return 0;
}

static int add_gpos_sub(pd_font* f, uint32_t off, uint32_t lookup, int32_t* cap) {
    if (f->gpos_nsub >= *cap) {
        int32_t nc = *cap ? *cap * 2 : 16;
        uint32_t* p = (uint32_t*)realloc(f->gpos_sub, nc * sizeof(uint32_t));
        uint16_t* q;

        if (!p) {
            return -1;
        }

        f->gpos_sub = p;
        q = (uint16_t*)realloc(f->gpos_lookup, nc * sizeof(uint16_t));

        if (!q) {
            return -1;
        }

        f->gpos_lookup = q;
        *cap = nc;
    }

    f->gpos_lookup[f->gpos_nsub] = (uint16_t)lookup;
    f->gpos_sub[f->gpos_nsub++] = off;
    return 0;
}

/* collect the PairPos subtables of all lookups referenced by 'kern' features */
static int setup_gpos(pd_font* f, uint32_t gpos) {
    uint32_t fl, ll, nf, nl, i, j, k;
    uint8_t* used;
    int32_t cap = 0;

    if (!gpos || U16(f, gpos) != 1) {
        return 0;
    }

    fl = gpos + U16(f, gpos + 6);
    ll = gpos + U16(f, gpos + 8);
    nf = U16(f, fl);
    nl = U16(f, ll);
    used = (uint8_t*)calloc(nl + 1, 1);

    if (!used) {
        return -1;
    }

    for (i = 0; i < nf; i++) {
        uint64_t rec = (uint64_t)fl + 2 + 6 * i;

        if (U32(f, rec) == TAG('k', 'e', 'r', 'n')) {
            uint32_t ft = fl + U16(f, rec + 4), n = U16(f, ft + 2);

            for (j = 0; j < n; j++) {
                uint32_t li = U16(f, (uint64_t)ft + 4 + 2 * j);

                if (li < nl) {
                    used[li] = 1;
                }
            }
        }
    }

    for (i = 0; i < nl; i++) {      /* lookup-list order, as OpenType applies them */
        uint32_t lk, type, ns;

        if (!used[i]) {
            continue;
        }

        lk = ll + U16(f, (uint64_t)ll + 2 + 2 * i);
        type = U16(f, lk);
        ns = U16(f, lk + 4);

        for (k = 0; k < ns; k++) {
            uint32_t sub = lk + U16(f, (uint64_t)lk + 6 + 2 * k);

            if (type == 9 && U16(f, sub) == 1 && U16(f, sub + 2) == 2) {
                sub += U32(f, sub + 4);
            } else if (type != 2) {
                continue;
            }

            if (add_gpos_sub(f, sub, i, &cap)) {
                free(used);
                return -1;
            }
        }
    }

    free(used);
    return 0;
}

int32_t pd_font_kerning(const pd_font* f, uint32_t l, uint32_t r) {
    int32_t i, sum = 0;

    if (!f) {
        return 0;
    }

    if (f->gpos_nsub) {
        /* within a lookup the first subtable that applies wins; lookups add up */
        for (i = 0; i < f->gpos_nsub; i++) {
            int hit;
            int32_t v = pairpos(f, f->gpos_sub[i], l, r, &hit);

            if (hit) {
                sum += v;

                while (i + 1 < f->gpos_nsub && f->gpos_lookup[i + 1] == f->gpos_lookup[i]) {
                    i++;
                }
            }
        }

        return sum;
    }

    return f->kern_npairs ? kern_legacy(f, l, r) : 0;
}

/* ------------------------------------------------------------------ */
/* loading                                                            */
/* ------------------------------------------------------------------ */

static pd_status parse_font(pd_font* f, int32_t face) {
    uint32_t base = 0, head, hhea, maxp, os2, os2len = 0, cmap, kern, klen = 0, gpos;
    uint32_t sfnt = U32(f, 0);

    if (sfnt == TAG('t', 't', 'c', 'f')) {
        if (face < 0 || (uint32_t)face >= U32(f, 8)) {
            return PD_ERR_ARG;
        }

        base = U32(f, 12 + 4 * (uint64_t)face);
        sfnt = U32(f, base);
    } else if (face != 0) {
        return PD_ERR_ARG;
    }

    if (sfnt != 0x00010000 && sfnt != TAG('O', 'T', 'T', 'O') && sfnt != TAG('t', 'r', 'u', 'e')) {
        return PD_ERR_FONT;
    }

    head = find_table(f, base, TAG('h', 'e', 'a', 'd'), NULL);
    hhea = find_table(f, base, TAG('h', 'h', 'e', 'a'), NULL);
    maxp = find_table(f, base, TAG('m', 'a', 'x', 'p'), NULL);
    f->hmtx = find_table(f, base, TAG('h', 'm', 't', 'x'), NULL);
    f->base = base;
    f->glyf = find_table(f, base, TAG('g', 'l', 'y', 'f'), &f->glyf_len);
    f->loca = find_table(f, base, TAG('l', 'o', 'c', 'a'), &f->loca_len);
    f->cff = find_table(f, base, TAG('C', 'F', 'F', ' '), &f->cff_len);
    f->math = find_table(f, base, TAG('M', 'A', 'T', 'H'), &f->math_len);
    f->loca_long = head ? S16(f, head + 50) : 0;
    cmap = find_table(f, base, TAG('c', 'm', 'a', 'p'), NULL);

    if (!head || !hhea || !maxp || !f->hmtx || !cmap) {
        return PD_ERR_FONT;
    }

    f->m.units_per_em = (int32_t)U16(f, head + 18);

    if (f->m.units_per_em < 16 || f->m.units_per_em > 16384) {
        return PD_ERR_FONT;
    }

    f->m.num_glyphs = (int32_t)U16(f, maxp + 4);
    f->m.ascender = S16(f, hhea + 4);
    f->m.descender = S16(f, hhea + 6);
    f->m.line_gap = S16(f, hhea + 8);
    f->num_hmetrics = U16(f, hhea + 34);

    if (f->num_hmetrics == 0) {
        return PD_ERR_FONT;
    }

    os2 = find_table(f, base, TAG('O', 'S', '/', '2'), &os2len);

    if (os2 && os2len >= 78) {
        if (U16(f, os2 + 62) & 0x80) {  /* USE_TYPO_METRICS */
            f->m.ascender = S16(f, os2 + 68);
            f->m.descender = S16(f, os2 + 70);
            f->m.line_gap = S16(f, os2 + 72);
        }

        if (U16(f, os2) >= 2 && os2len >= 90) {
            f->m.x_height = S16(f, os2 + 86);
            f->m.cap_height = S16(f, os2 + 88);
        }
    }

    select_cmap(f, cmap);
    kern = find_table(f, base, TAG('k', 'e', 'r', 'n'), &klen);
    setup_kern(f, kern, klen);
    gpos = find_table(f, base, TAG('G', 'P', 'O', 'S'), NULL);

    if (setup_gpos(f, gpos)) {
        return PD_ERR_NOMEM;
    }

    f->m.has_kerning = (f->gpos_nsub > 0 || f->kern_npairs > 0);
    f->face_index = face;
    pd_shape_font_init(f);
    return PD_OK;
}

pd_status pd_font_load_memory(const void* data, size_t len, int32_t face, pd_font** out) {
    pd_font* f;
    pd_status st;

    if (!data || !out || len < 12) {
        return PD_ERR_ARG;
    }

    *out = NULL;
    f = (pd_font*)calloc(1, sizeof(pd_font));

    if (!f) {
        return PD_ERR_NOMEM;
    }

    f->data = (uint8_t*)malloc(len);

    if (!f->data) {
        free(f);
        return PD_ERR_NOMEM;
    }

    memcpy(f->data, data, len);
    f->len = len;
    st = parse_font(f, face);

    if (st != PD_OK) {
        pd_font_free(f);
        return st;
    }

    *out = f;
    return PD_OK;
}

pd_status pd_font_load_file(const char* path, int32_t face, pd_font** out) {
    FILE* fp;
    long len;
    void* buf;
    pd_status st;

    if (!path || !out) {
        return PD_ERR_ARG;
    }

    fp = fopen(path, "rb");

    if (!fp) {
        return PD_ERR_IO;
    }

    if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return PD_ERR_IO;
    }

    buf = malloc((size_t)len);

    if (!buf) {
        fclose(fp);
        return PD_ERR_NOMEM;
    }

    if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
        free(buf);
        fclose(fp);
        return PD_ERR_IO;
    }

    fclose(fp);
    st = pd_font_load_memory(buf, (size_t)len, face, out);
    free(buf);
    return st;
}

void pd_font_free(pd_font* f) {
    if (f) {
        pd_shape_font_free(f);
        free(f->gpos_sub);
        free(f->gpos_lookup);
        free(f->data);
        free(f);
    }
}

pd_status pd_font_get_metrics(const pd_font* f, pd_font_metrics* out) {
    if (!f || !out) {
        return PD_ERR_ARG;
    }

    *out = f->m;
    return PD_OK;
}

uint32_t pd_font_glyph_index(const pd_font* f, uint32_t cp) {
    uint32_t g;

    if (!f) {
        return 0;
    }

    g = cmap_lookup(f, cp);

    if (!g && f->cmap_symbol && cp < 0x100) {
        g = cmap_lookup(f, 0xF000 + cp);
    }

    return (int32_t)g < f->m.num_glyphs ? g : 0;
}

int32_t pd_font_glyph_advance(const pd_font* f, uint32_t g) {
    uint32_t i;

    if (!f) {
        return 0;
    }

    i = g < f->num_hmetrics ? g : f->num_hmetrics - 1;
    return (int32_t)U16(f, (uint64_t)f->hmtx + 4 * i);
}
