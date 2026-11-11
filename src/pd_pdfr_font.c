/*
 * Parade PDF reader: fonts
 *
 * A font dictionary becomes a pfont: how its codes are read from a string
 * (one byte, or a CMap's codes), each code's width, its glyph and that
 * glyph's outline, and its text (ToUnicode, else the glyph's name, else the
 * font's own cmap). Programs: TrueType and OpenType (Parade's outline
 * reader on a pd_font made over the embedded bytes), bare CFF (the same
 * reader's CFF interpreter), Type 1 (a charstring interpreter here, flex
 * and seac included) and Type 3 (content streams run by pd_pdfr_draw.c).
 * A font not embedded is drawn with a system font of its kind: the URW
 * clones of the standard 14 where they are, Liberation, DejaVu, Windows'
 * and macOS's own.
 */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr.h"

#include "pd_pdfr_enc.inc"

enum { FT_TYPE1 = 1, FT_TRUETYPE, FT_TYPE3, FT_TYPE0 };
enum { PG_NONE = 0, PG_SFNT, PG_CFF, PG_T1 };

typedef struct {
    uint32_t lo, hi;
    uint32_t cid;               /* CID of lo; the rest follow */
    uint8_t nbytes;
} crange;

typedef struct {
    uint32_t code;
    uint8_t n;
    uint32_t cp[4];
} tuni;

typedef struct {
    uint32_t lo, hi;
    double w;
} wrange;

typedef struct {
    const char* name;
    const unsigned char* data;
    int32_t len;
} t1glyph;

typedef struct {
    int32_t n;                  /* commands */
    float* v;                   /* op, then its numbers: 0 move x y, 1 line x y, 2 cubic 6, 3 close */
    int32_t nv;
} gpath;

struct pfont {
    pfont* next;
    pv dict;
    pdoc* doc;
    int kind;
    int vertical;
    int symbolic;
    char base[96];
    /* the program */
    int prog;
    unsigned char* data;        /* its bytes (a CFF has one byte before it, so its offset is not 0) */
    size_t len;
    pd_font pf;                 /* Parade's view of an sfnt or CFF program */
    pd_font* sys;               /* a system font standing in */
    unsigned char* sysdata;
    int sys_symbol;
    double fm[6];               /* glyph space to text space x 1000 (1000 an em) */
    /* sfnt */
    uint32_t cmap31, cmap30, cmap10, cmap_any, post, hmtx, nhm;
    int32_t upem, nglyphs;
    /* CFF */
    int cff_cid;
    uint16_t* charset;          /* GID -> SID or CID */
    int32_t cff_nglyphs;
    uint32_t cff_strings;       /* the strings INDEX's offset */
    uint16_t cff_enc[256];      /* built-in encoding: code -> GID */
    int cff_has_enc;
    /* Type 1 */
    t1glyph* t1;
    int32_t nt1;
    t1glyph* subrs;
    int32_t nsubrs;
    int lenIV;
    unsigned char* t1plain;     /* the decrypted program */
    const char* t1enc[256];
    /* simple fonts */
    const char* names[256];
    int32_t gid[256];           /* code -> glyph, -1 not yet, -2 none */
    double widths[256];
    uint8_t has_width[256];
    double missing;
    /* composite */
    crange* cmap;
    int32_t ncmap;
    int identity;               /* Identity-H/V */
    uint8_t cs_nbytes[256];     /* codespace: bytes in a code by its first byte (0: 1 or 2 by default) */
    int has_codespace;
    uint16_t* cid2gid;
    int32_t ncid2gid;
    wrange* w;
    int32_t nw;
    double dw;
    /* text */
    tuni* tu;
    int32_t ntu;
    uint32_t* gid2uni;          /* from the font's own cmap, for CID fonts without ToUnicode */
    int32_t ngid2uni;
    /* Type 3 */
    pv charprocs, resources;
    /* outlines once read */
    gpath* cache;
    int32_t ncache;
    uint8_t* cached;
};

/* ------------------------------------------------------------------ */
/* glyph names                                                        */
/* ------------------------------------------------------------------ */

static uint32_t name_unicode(const char* n) {
    int i;

    if (!n) {
        return 0;
    }

    for (i = 0; i < AGL_N; i++) {
        if (strcmp(agl[i].name, n) == 0) {
            return agl[i].cp;
        }
    }

    if (n[0] && !n[1]) {
        return (unsigned char)n[0];     /* "A" .. "z" */
    }

    if (strncmp(n, "uni", 3) == 0 && strlen(n) >= 7) {
        return (uint32_t)strtoul(n + 3, NULL, 16) & 0xFFFF ? (uint32_t)strtoul(n + 3, NULL, 16) : 0;
    }

    if (n[0] == 'u' && strlen(n) >= 5 && strlen(n) <= 7 && isxdigit((unsigned char)n[1])) {
        char* e;
        unsigned long v = strtoul(n + 1, &e, 16);

        return *e == 0 ? (uint32_t)v : 0;
    }

    {
        /* "a.sc", "f_f", "one.oldstyle": the part before the dot */
        const char* dot = strchr(n, '.');

        if (dot && dot > n && dot - n < 40) {
            char tmp[48];

            memcpy(tmp, n, (size_t)(dot - n));
            tmp[dot - n] = 0;
            return name_unicode(tmp);
        }
    }

    return 0;
}

static const char* unicode_name(uint32_t cp) {
    int i;

    if (cp >= 'A' && cp <= 'Z') {
        return cff_std[cp - 'A' + 34];
    }

    if (cp >= 'a' && cp <= 'z') {
        return cff_std[cp - 'a' + 66];
    }

    for (i = 0; i < AGL_N; i++) {
        if (agl[i].cp == cp) {
            return agl[i].name;
        }
    }

    return NULL;
}

static const char* ascii_name(int c) {
    if (c == 39) {
        return "quotesingle";
    }

    if (c == 96) {
        return "grave";
    }

    if (c >= 32 && c <= 126) {
        return unicode_name((uint32_t)c);
    }

    return NULL;
}

/* a base encoding's name for a code */
static const char* enc_name(const char* enc, int c) {
    if (!enc || strcmp(enc, "StandardEncoding") == 0) {
        size_t i;

        if (c == 39) {
            return "quoteright";
        }

        if (c == 96) {
            return "quoteleft";
        }

        if (c < 128) {
            return ascii_name(c);
        }

        for (i = 0; i < sizeof(std_hi) / sizeof(std_hi[0]); i++) {
            if (std_hi[i].code == c) {
                return std_hi[i].name;
            }
        }

        return NULL;
    }

    if (strcmp(enc, "WinAnsiEncoding") == 0) {
        if (c < 128) {
            return c >= 32 ? ascii_name(c) : NULL;
        }

        if (c < 160) {
            return unicode_name(winansi_hi[c - 128]);
        }

        return c == 160 ? "space" : c == 173 ? "hyphen" : unicode_name((uint32_t)c);
    }

    if (strcmp(enc, "MacRomanEncoding") == 0) {
        if (c < 128) {
            return c >= 32 ? ascii_name(c) : NULL;
        }

        return c == 0xCA ? "space" : unicode_name(macroman_hi[c - 128]);
    }

    if (strcmp(enc, "Symbol") == 0) {
        return c >= 32 && symbol_enc[c - 32] ? unicode_name(symbol_enc[c - 32]) : NULL;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* sfnt                                                               */
/* ------------------------------------------------------------------ */

static uint32_t rd16(const pfont* f, uint64_t o) {
    return o + 2 <= f->len ? ((uint32_t)f->data[o] << 8) | f->data[o + 1] : 0;
}

static uint32_t rd32(const pfont* f, uint64_t o) {
    return o + 4 <= f->len ? (rd16(f, o) << 16) | rd16(f, o + 2) : 0;
}

static uint32_t sfnt_table(const pfont* f, uint32_t base, const char* tag, uint32_t* len) {
    uint32_t n = rd16(f, base + 4), i, t = ((uint32_t)tag[0] << 24) | ((uint32_t)tag[1] << 16) |
                 ((uint32_t)tag[2] << 8) | (uint32_t)tag[3];

    for (i = 0; i < n && i < 256; i++) {
        uint64_t rec = (uint64_t)base + 12 + 16 * i;

        if (rd32(f, rec) == t) {
            uint32_t off = rd32(f, rec + 8), l = rd32(f, rec + 12);

            if ((uint64_t)off + l > f->len) {
                l = off < f->len ? (uint32_t)(f->len - off) : 0;
            }

            if (len) {
                *len = l;
            }

            return off;
        }
    }

    return 0;
}

static uint32_t cmap_glyph(const pfont* f, uint32_t sub, uint32_t c) {
    uint32_t fmt;

    if (!sub) {
        return 0;
    }

    fmt = rd16(f, sub);

    if (fmt == 0) {
        return c < 256 ? f->data[sub + 6 + c < f->len ? sub + 6 + c : 0] : 0;
    }

    if (fmt == 4) {
        uint32_t segx2 = rd16(f, sub + 6), i;
        uint32_t ends = sub + 14, starts = ends + segx2 + 2, deltas = starts + segx2, ranges = deltas + segx2;

        for (i = 0; i < segx2; i += 2) {
            uint32_t end = rd16(f, ends + i), start;

            if (c > end) {
                continue;
            }

            start = rd16(f, starts + i);

            if (c < start) {
                return 0;
            }

            {
                uint32_t delta = rd16(f, deltas + i), ro = rd16(f, ranges + i);

                if (ro == 0) {
                    return (c + delta) & 0xFFFF;
                }

                {
                    uint32_t g = rd16(f, ranges + i + ro + 2 * (c - start));

                    return g ? (g + delta) & 0xFFFF : 0;
                }
            }
        }

        return 0;
    }

    if (fmt == 6) {
        uint32_t first = rd16(f, sub + 6), n = rd16(f, sub + 8);

        return c >= first && c < first + n ? rd16(f, sub + 10 + 2 * (c - first)) : 0;
    }

    if (fmt == 12) {
        uint32_t n = rd32(f, sub + 12), i;

        for (i = 0; i < n && i < 100000; i++) {
            uint32_t s = rd32(f, sub + 16 + 12 * i), e = rd32(f, sub + 20 + 12 * i);

            if (c >= s && c <= e) {
                return rd32(f, sub + 24 + 12 * i) + (c - s);
            }
        }
    }

    return 0;
}

/* a glyph's index by its name in the post table (format 2) */
static int32_t post_glyph(const pfont* f, const char* name) {
    uint32_t p = f->post, n, i, names;
    static const char* const mac[] = {
        ".notdef", ".null", "nonmarkingreturn", "space", "exclam", "quotedbl", "numbersign", "dollar", "percent",
        "ampersand", "quotesingle", "parenleft", "parenright", "asterisk", "plus", "comma", "hyphen", "period",
        "slash", "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "colon",
        "semicolon", "less", "equal", "greater", "question", "at", "A", "B", "C", "D", "E", "F", "G", "H", "I",
        "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "bracketleft",
        "backslash", "bracketright", "asciicircum", "underscore", "grave", "a", "b", "c", "d", "e", "f", "g",
        "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
        "braceleft", "bar", "braceright", "asciitilde"
    };

    if (!p || !name || rd32(f, p) != 0x00020000) {
        return -1;
    }

    n = rd16(f, p + 32);
    names = p + 34 + 2 * n;

    for (i = 0; i < n; i++) {
        uint32_t idx = rd16(f, p + 34 + 2 * i);

        if (idx < 258) {
            if (idx < sizeof(mac) / sizeof(mac[0]) && strcmp(mac[idx], name) == 0) {
                return (int32_t)i;
            }
        } else {
            uint32_t k = idx - 258, o = names, j;

            for (j = 0; j < k && o < f->len; j++) {
                o += 1 + f->data[o];
            }

            if (o < f->len) {
                size_t l = f->data[o];

                if (o + 1 + l <= f->len && strlen(name) == l && memcmp(f->data + o + 1, name, l) == 0) {
                    return (int32_t)i;
                }
            }
        }
    }

    return -1;
}

static int setup_sfnt(pfont* f) {
    uint32_t base = 0, head, maxp, cmap, n, i, hhea;

    if (f->len < 12) {
        return 0;
    }

    if (rd32(f, 0) == 0x74746366) {     /* 'ttcf' */
        base = rd32(f, 12);
    }

    head = sfnt_table(f, base, "head", NULL);
    maxp = sfnt_table(f, base, "maxp", NULL);
    hhea = sfnt_table(f, base, "hhea", NULL);
    memset(&f->pf, 0, sizeof(f->pf));
    f->pf.data = f->data;
    f->pf.len = f->len;
    f->pf.base = base;
    f->pf.glyf = sfnt_table(f, base, "glyf", &f->pf.glyf_len);
    f->pf.loca = sfnt_table(f, base, "loca", &f->pf.loca_len);
    f->pf.cff = sfnt_table(f, base, "CFF ", &f->pf.cff_len);
    f->pf.loca_long = head ? (int32_t)(int16_t)rd16(f, head + 50) : 0;
    f->upem = head ? (int32_t)rd16(f, head + 18) : 1000;

    if (f->upem < 16 || f->upem > 16384) {
        f->upem = 1000;
    }

    f->nglyphs = maxp ? (int32_t)rd16(f, maxp + 4) : 0;

    if (f->nglyphs == 0 && f->pf.loca) {
        f->nglyphs = (int32_t)(f->pf.loca_len / (f->pf.loca_long ? 4 : 2)) - 1;
    }

    f->pf.m.num_glyphs = f->nglyphs > 0 ? f->nglyphs : 65535;
    f->pf.m.units_per_em = f->upem;
    f->hmtx = sfnt_table(f, base, "hmtx", NULL);
    f->nhm = hhea ? rd16(f, hhea + 34) : 0;
    f->post = sfnt_table(f, base, "post", NULL);
    cmap = sfnt_table(f, base, "cmap", NULL);

    if (cmap) {
        n = rd16(f, cmap + 2);

        for (i = 0; i < n && i < 64; i++) {
            uint32_t plat = rd16(f, cmap + 4 + 8 * i), enc = rd16(f, cmap + 6 + 8 * i);
            uint32_t sub = cmap + rd32(f, cmap + 8 + 8 * i);

            if (sub >= f->len) {
                continue;
            }

            if (plat == 3 && (enc == 1 || enc == 10) && (!f->cmap31 || rd16(f, sub) == 12)) {
                f->cmap31 = sub;
            } else if (plat == 0 && !f->cmap31) {
                f->cmap31 = sub;
            } else if (plat == 3 && enc == 0) {
                f->cmap30 = sub;
            } else if (plat == 1 && enc == 0) {
                f->cmap10 = sub;
            }

            if (!f->cmap_any) {
                f->cmap_any = sub;
            }
        }
    }

    if (f->pf.cff) {    /* an OpenType CFF: read as bare CFF too, for its charset */
        f->fm[0] = f->fm[3] = 1000.0 / f->upem;
    } else {
        f->fm[0] = f->fm[3] = 1000.0 / f->upem;
    }

    return f->pf.glyf || f->pf.cff;
}

/* ------------------------------------------------------------------ */
/* CFF                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t off, count, offsize, data;
} cidx;

static uint32_t rdn(const pfont* f, uint64_t o, uint32_t n) {
    uint32_t v = 0, k;

    for (k = 0; k < n; k++) {
        v = (v << 8) | (o + k < f->len ? f->data[o + k] : 0);
    }

    return v;
}

static uint32_t cff_index(const pfont* f, uint32_t off, cidx* x) {
    memset(x, 0, sizeof(*x));
    x->off = off;
    x->count = rdn(f, off, 2);

    if (!x->count) {
        return off + 2;
    }

    x->offsize = rdn(f, off + 2, 1);

    if (x->offsize < 1 || x->offsize > 4) {
        x->count = 0;
        return off + 2;
    }

    x->data = off + 3 + (x->count + 1) * x->offsize - 1;
    return x->data + rdn(f, (uint64_t)off + 3 + (uint64_t)x->count * x->offsize, x->offsize);
}

static int cff_item(const pfont* f, const cidx* x, uint32_t i, uint32_t* s, uint32_t* l) {
    uint32_t a, b;

    if (i >= x->count) {
        return 0;
    }

    a = rdn(f, (uint64_t)x->off + 3 + (uint64_t)i * x->offsize, x->offsize);
    b = rdn(f, (uint64_t)x->off + 3 + (uint64_t)(i + 1) * x->offsize, x->offsize);

    if (b < a || (uint64_t)x->data + b > f->len) {
        return 0;
    }

    *s = x->data + a;
    *l = b - a;
    return 1;
}

/* a top dict's operands of an operator (12 x as 1200 + x) */
static int cff_dict(const pfont* f, uint32_t s, uint32_t l, int op, double* out, int max) {
    uint32_t p = s, e = s + l;
    double stack[48];
    int n = 0;

    while (p < e) {
        int b = f->data[p];

        if (b <= 21) {
            int o = b;

            p++;

            if (b == 12) {
                o = 1200 + (p < e ? f->data[p] : 0);
                p++;
            }

            if (o == op) {
                int k;

                for (k = 0; k < n && k < max; k++) {
                    out[k] = stack[k];
                }

                return n;
            }

            n = 0;
        } else {
            double v = 0;

            if (b == 28) {
                v = (int16_t)rdn(f, p + 1, 2);
                p += 3;
            } else if (b == 29) {
                v = (int32_t)rdn(f, p + 1, 4);
                p += 5;
            } else if (b == 30) {   /* a real, in nibbles */
                char buf[64];
                int k = 0, done = 0;

                p++;

                while (p < e && !done) {
                    int byte = f->data[p++], h;

                    for (h = 0; h < 2 && !done; h++) {
                        int nib = h ? byte & 15 : byte >> 4;

                        if (k > 60) {
                            done = 1;
                        } else if (nib <= 9) {
                            buf[k++] = (char)('0' + nib);
                        } else if (nib == 0xa) {
                            buf[k++] = '.';
                        } else if (nib == 0xb) {
                            buf[k++] = 'E';
                        } else if (nib == 0xc) {
                            buf[k++] = 'E';
                            buf[k++] = '-';
                        } else if (nib == 0xe) {
                            buf[k++] = '-';
                        } else if (nib == 0xf) {
                            done = 1;
                        }
                    }
                }

                buf[k] = 0;
                v = atof(buf);
            } else if (b >= 32 && b <= 246) {
                v = b - 139;
                p++;
            } else if (b >= 247 && b <= 250) {
                v = (b - 247) * 256 + (p + 1 < f->len ? f->data[p + 1] : 0) + 108;
                p += 2;
            } else if (b >= 251 && b <= 254) {
                v = -(b - 251) * 256 - (p + 1 < f->len ? f->data[p + 1] : 0) - 108;
                p += 2;
            } else {
                p++;
            }

            if (n < 48) {
                stack[n++] = v;
            }
        }
    }

    return 0;
}

static int setup_cff(pfont* f, uint32_t cff) {
    cidx names, tops, strings, chars;
    uint32_t p, td, tdl;
    double v[6];
    int32_t g;

    p = cff_index(f, cff + rdn(f, cff + 2, 1), &names);
    p = cff_index(f, p, &tops);
    f->cff_strings = p;
    cff_index(f, p, &strings);

    if (!cff_item(f, &tops, 0, &td, &tdl)) {
        return 0;
    }

    if (cff_dict(f, td, tdl, 17, v, 1) < 1) {
        return 0;
    }

    cff_index(f, cff + (uint32_t)v[0], &chars);
    f->cff_nglyphs = (int32_t)chars.count;
    f->cff_cid = cff_dict(f, td, tdl, 1230, v, 3) >= 3;

    if (cff_dict(f, td, tdl, 1207, v, 6) >= 6 && v[0] != 0) {   /* FontMatrix */
        int k;

        for (k = 0; k < 6; k++) {
            f->fm[k] = v[k] * 1000;
        }
    } else {
        f->fm[0] = f->fm[3] = 1;
        f->fm[1] = f->fm[2] = f->fm[4] = f->fm[5] = 0;
    }

    /* charset: GID -> SID (or CID) */
    f->charset = (uint16_t*)calloc((size_t)f->cff_nglyphs + 1, sizeof(uint16_t));

    if (!f->charset) {
        return 0;
    }

    {
        double cs = 0;
        int has = cff_dict(f, td, tdl, 15, v, 1) >= 1;

        cs = has ? v[0] : 0;

        if (cs <= 2) {      /* the predefined charsets: ISOAdobe (SID = GID) for the first 229 */
            for (g = 0; g < f->cff_nglyphs; g++) {
                f->charset[g] = (uint16_t)(g < 229 || f->cff_cid ? g : 0);
            }
        } else {
            uint32_t o = cff + (uint32_t)cs;
            int fmt = (int)rdn(f, o, 1);

            o++;
            g = 1;

            if (fmt == 0) {
                for (; g < f->cff_nglyphs; g++, o += 2) {
                    f->charset[g] = (uint16_t)rdn(f, o, 2);
                }
            } else {
                while (g < f->cff_nglyphs && o < f->len) {
                    uint32_t first = rdn(f, o, 2), left = rdn(f, o + 2, fmt == 1 ? 1 : 2), k;

                    o += fmt == 1 ? 3 : 4;

                    for (k = 0; k <= left && g < f->cff_nglyphs; k++) {
                        f->charset[g++] = (uint16_t)(first + k);
                    }
                }
            }
        }
    }

    /* the built-in encoding: code -> GID */
    {
        int has = cff_dict(f, td, tdl, 16, v, 1) >= 1;
        uint32_t enc = has ? (uint32_t)v[0] : 0;

        memset(f->cff_enc, 0, sizeof(f->cff_enc));

        if (enc > 1 && !f->cff_cid) {
            uint32_t o = cff + enc;
            int fmt = (int)rdn(f, o, 1);

            f->cff_has_enc = 1;

            if ((fmt & 0x7f) == 0) {
                uint32_t n = rdn(f, o + 1, 1), k;

                for (k = 0; k < n; k++) {
                    f->cff_enc[rdn(f, o + 2 + k, 1)] = (uint16_t)(k + 1);
                }

                o += 2 + n;
            } else if ((fmt & 0x7f) == 1) {
                uint32_t n = rdn(f, o + 1, 1), k, gg = 1;

                for (k = 0; k < n; k++) {
                    uint32_t first = rdn(f, o + 2 + 2 * k, 1), left = rdn(f, o + 3 + 2 * k, 1), j;

                    for (j = 0; j <= left && first + j < 256; j++) {
                        f->cff_enc[first + j] = (uint16_t)gg++;
                    }
                }

                o += 2 + 2 * n;
            }

            if (fmt & 0x80) {   /* supplements */
                uint32_t n = rdn(f, o, 1), k;

                for (k = 0; k < n; k++) {
                    uint32_t code = rdn(f, o + 1 + 3 * k, 1), sid = rdn(f, o + 2 + 3 * k, 2);

                    for (g = 0; g < f->cff_nglyphs; g++) {
                        if (f->charset[g] == sid) {
                            f->cff_enc[code] = (uint16_t)g;
                            break;
                        }
                    }
                }
            }
        } else if (enc == 0 && !f->cff_cid) {   /* StandardEncoding by name */
            int c;

            f->cff_has_enc = 2;

            for (c = 0; c < 256; c++) {
                const char* nm = enc_name("StandardEncoding", c);

                if (nm) {
                    for (g = 0; g < f->cff_nglyphs; g++) {
                        if (f->charset[g] < CFF_NSTD && strcmp(cff_std[f->charset[g]], nm) == 0) {
                            f->cff_enc[c] = (uint16_t)g;
                            break;
                        }
                    }
                }
            }
        }
    }

    return 1;
}

/* the name of a CFF glyph: its SID's string */
static int cff_glyph_name(const pfont* f, int32_t g, char* out, size_t cap) {
    uint32_t sid;

    if (!f->charset || g < 0 || g >= f->cff_nglyphs) {
        return 0;
    }

    sid = f->charset[g];

    if (sid < (uint32_t)CFF_NSTD) {
        snprintf(out, cap, "%s", cff_std[sid]);
        return 1;
    }

    {
        cidx strings;
        uint32_t s, l;

        cff_index(f, f->cff_strings, &strings);

        if (cff_item(f, &strings, sid - (uint32_t)CFF_NSTD, &s, &l)) {
            size_t k = l < cap - 1 ? l : cap - 1;

            memcpy(out, f->data + s, k);
            out[k] = 0;
            return 1;
        }
    }

    return 0;
}

static int32_t cff_by_name(pfont* f, const char* name) {
    int32_t g;
    char tmp[128];

    if (!name) {
        return -1;
    }

    for (g = 0; g < f->cff_nglyphs; g++) {
        if (cff_glyph_name(f, g, tmp, sizeof(tmp)) && strcmp(tmp, name) == 0) {
            return g;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Type 1                                                             */
/* ------------------------------------------------------------------ */

static void t1_decrypt(unsigned char* p, size_t n, uint16_t r) {
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = p[i];

        p[i] = (unsigned char)(c ^ (r >> 8));
        r = (uint16_t)((c + r) * 52845u + 22719u);
    }
}

/* a token after p (skipping spaces): returns its start, *len set */
static const unsigned char* t1_token(const unsigned char* p, const unsigned char* e, size_t* len) {
    const unsigned char* s;

    while (p < e && (isspace(*p) || *p == 0)) {
        p++;
    }

    s = p;

    if (p < e && (*p == '/' || *p == '{' || *p == '}' || *p == '[' || *p == ']')) {
        p++;

        if (*s == '/') {
            while (p < e && !isspace(*p) && *p != '/' && *p != '{' && *p != '[' && *p != '(' && *p != ']') {
                p++;
            }
        }
    } else {
        while (p < e && !isspace(*p) && *p != '/' && *p != '{' && *p != '}' && *p != '[' && *p != ']') {
            p++;
        }
    }

    *len = (size_t)(p - s);
    return s;
}

static const unsigned char* mem_find(const unsigned char* p, const unsigned char* e, const char* s) {
    size_t k = strlen(s);

    while (p + k <= e) {
        if (*p == (unsigned char)s[0] && memcmp(p, s, k) == 0) {
            return p;
        }

        p++;
    }

    return NULL;
}

static int setup_t1(pfont* f, pv stream) {
    pdoc* d = f->doc;
    size_t n;
    unsigned char* data = pd_stream_data(d, stream, &n, NULL, NULL), *plain;
    size_t l1 = (size_t)pd_getint(d, stream, "Length1", 0), i;
    const unsigned char* e, *p;
    pbuf body;

    if (!data) {
        return 0;
    }

    memset(&body, 0, sizeof(body));

    /* PFB segments taken apart */
    if (n > 6 && data[0] == 0x80 && data[1] == 1) {
        size_t at = 0;

        l1 = 0;

        while (at + 6 <= n && data[at] == 0x80 && data[at + 1] != 3) {
            size_t seg = (size_t)data[at + 2] | ((size_t)data[at + 3] << 8) | ((size_t)data[at + 4] << 16) |
                         ((size_t)data[at + 5] << 24);

            if (at + 6 + seg > n) {
                seg = n - at - 6;
            }

            if (data[at + 1] == 1 && l1 == 0) {
                l1 = seg;
            }

            qb_put(&body, data + at + 6, seg);
            at += 6 + seg;
        }

        free(data);
        data = body.p;
        n = body.n;
        memset(&body, 0, sizeof(body));
    }

    if (!data) {
        return 0;
    }

    /* where the encrypted part starts: after "eexec" */
    {
        const unsigned char* ex = mem_find(data, data + n, "eexec");

        if (ex) {
            size_t at = (size_t)(ex - data) + 5;

            while (at < n && (data[at] == 13 || data[at] == 10 || data[at] == ' ' || data[at] == 9)) {
                at++;
            }

            l1 = at;
        } else if (l1 == 0 || l1 > n) {
            free(data);
            return 0;
        }
    }

    /* the built-in encoding, in the clear part */
    {
        const unsigned char* enc = mem_find(data, data + l1, "/Encoding");

        if (enc) {
            const unsigned char* q = enc + 9, *end = data + l1;
            size_t tl;
            const unsigned char* t = t1_token(q, end, &tl);

            if (tl >= 16 && memcmp(t, "StandardEncoding", 16) == 0) {
                int c;

                for (c = 0; c < 256; c++) {
                    f->t1enc[c] = enc_name("StandardEncoding", c);
                }
            } else {
                /* dup code /name put ... */
                const unsigned char* stop = mem_find(q, end, "readonly def");

                if (!stop) {
                    stop = mem_find(q, end, " def");
                }

                if (!stop) {
                    stop = end;
                }

                while (q < stop) {
                    t = t1_token(q, stop, &tl);

                    if (tl == 0) {
                        break;
                    }

                    q = t + tl;

                    if (tl == 3 && memcmp(t, "dup", 3) == 0) {
                        size_t cl, nl;
                        const unsigned char* c = t1_token(q, stop, &cl), *nm = t1_token(c + cl, stop, &nl);
                        int code = atoi((const char*)c);

                        if (nl > 1 && nm[0] == '/' && code >= 0 && code < 256) {
                            f->t1enc[code] = pa_strdup(&d->arena, (const char*)nm + 1, nl - 1);
                        }

                        q = nm + nl;
                    }
                }
            }
        }

        {
            const unsigned char* fmx = mem_find(data, data + l1, "/FontMatrix");

            if (fmx) {
                const unsigned char* q = fmx + 11;
                double m[6];
                int k;

                while (q < data + l1 && *q != '[' && *q != '{') {
                    q++;
                }

                q++;

                for (k = 0; k < 6; k++) {
                    char* endp;

                    m[k] = strtod((const char*)q, &endp);

                    if ((const unsigned char*)endp == q) {
                        break;
                    }

                    q = (const unsigned char*)endp;
                }

                if (k == 6 && m[0] != 0) {
                    for (k = 0; k < 6; k++) {
                        f->fm[k] = m[k] * 1000;
                    }
                }
            }
        }
    }

    /* the encrypted part: binary, or hex */
    {
        size_t en = n - l1, k = 0;
        int hex = en >= 4;

        for (i = 0; i < 4 && i < en; i++) {
            if (!isxdigit(data[l1 + i])) {
                hex = 0;
            }
        }

        plain = (unsigned char*)malloc(en + 1);

        if (!plain) {
            free(data);
            return 0;
        }

        if (hex) {
            int hi = -1;

            for (i = 0; i < en; i++) {
                int c = data[l1 + i], h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;

                if (h < 0) {
                    continue;
                }

                if (hi < 0) {
                    hi = h;
                } else {
                    plain[k++] = (unsigned char)(hi * 16 + h);
                    hi = -1;
                }
            }
        } else {
            memcpy(plain, data + l1, en);
            k = en;
        }

        t1_decrypt(plain, k, 55665);
        f->t1plain = plain;
        e = plain + k;
        p = plain + (k > 4 ? 4 : k);
    }

    free(data);
    f->lenIV = 4;

    {
        const unsigned char* li = mem_find(p, e, "/lenIV");

        if (li) {
            f->lenIV = atoi((const char*)li + 6);
        }
    }

    /* Subrs: dup i n RD <n bytes> NP */
    {
        const unsigned char* s = mem_find(p, e, "/Subrs");

        if (s) {
            size_t tl;
            const unsigned char* t = t1_token(s + 6, e, &tl);
            int32_t cnt = atoi((const char*)t);

            if (cnt > 0 && cnt < 65536) {
                const unsigned char* q = t + tl;

                f->subrs = (t1glyph*)calloc((size_t)cnt, sizeof(t1glyph));
                f->nsubrs = f->subrs ? cnt : 0;

                while (q < e && f->subrs) {
                    const unsigned char* tk = t1_token(q, e, &tl);

                    if (tl == 0) {
                        break;
                    }

                    if (tl == 3 && memcmp(tk, "dup", 3) == 0) {
                        size_t il, nl, rl;
                        const unsigned char* it = t1_token(tk + 3, e, &il), *nt = t1_token(it + il, e, &nl);
                        const unsigned char* rd = t1_token(nt + nl, e, &rl);
                        int32_t idx = atoi((const char*)it), len = atoi((const char*)nt);
                        const unsigned char* bin = rd + rl + 1;

                        if (len < 0 || bin + len > e) {
                            break;
                        }

                        if (idx >= 0 && idx < cnt) {
                            f->subrs[idx].data = bin;
                            f->subrs[idx].len = len;
                        }

                        q = bin + len;
                        continue;
                    }

                    if ((tl == 3 && memcmp(tk, "end", 3) == 0) || (tl >= 12 && memcmp(tk, "/CharStrings", 12) == 0) ||
                        (tl == 1 && tk[0] == 'd' ) || (tl == 3 && memcmp(tk, "def", 3) == 0 && q > s + 64)) {
                        if (tl >= 12 && memcmp(tk, "/CharStrings", 12) == 0) {
                            break;
                        }

                        if (tl == 3 && memcmp(tk, "def", 3) == 0) {
                            break;
                        }
                    }

                    q = tk + tl;
                }
            }
        }
    }

    /* CharStrings: /name n RD <n bytes> ND */
    {
        const unsigned char* s = mem_find(p, e, "/CharStrings");
        pbuf gl;

        memset(&gl, 0, sizeof(gl));

        if (s) {
            const unsigned char* q = s + 12;
            size_t tl;

            while (q < e) {
                const unsigned char* tk = t1_token(q, e, &tl);

                if (tl == 0) {
                    break;
                }

                if (tk[0] == '/' && tl > 1) {
                    size_t nl, rl;
                    const unsigned char* nt = t1_token(tk + tl, e, &nl), *rd = t1_token(nt + nl, e, &rl);
                    int32_t len;
                    const unsigned char* bin;

                    if (!nl || !isdigit(nt[0])) {
                        q = tk + tl;
                        continue;
                    }

                    len = atoi((const char*)nt);
                    bin = rd + rl + 1;

                    if (len < 0 || bin + len > e) {
                        break;
                    }

                    {
                        t1glyph g;

                        g.name = pa_strdup(&d->arena, (const char*)tk + 1, tl - 1);
                        g.data = bin;
                        g.len = len;
                        qb_put(&gl, &g, sizeof(g));
                    }

                    q = bin + len;
                    continue;
                }

                if (tl == 3 && memcmp(tk, "end", 3) == 0 && gl.n) {
                    break;
                }

                q = tk + tl;
            }
        }

        f->t1 = (t1glyph*)gl.p;
        f->nt1 = (int32_t)(gl.n / sizeof(t1glyph));
    }

    if (f->fm[0] == 0) {
        f->fm[0] = f->fm[3] = 1;
    }

    return f->nt1 > 0;
}

static int32_t t1_by_name(const pfont* f, const char* name) {
    int32_t i;

    if (!name) {
        return -1;
    }

    for (i = 0; i < f->nt1; i++) {
        if (strcmp(f->t1[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

typedef struct {
    const pfont* f;
    const pf_sink* sink;
    void* user;
    double x, y, sbx, wx;
    double st[32];
    int n;
    double ps[32];
    int nps;
    int open;
    int flex;
    double fx[8], fy[8];
    int nflex;
    int depth;
    double ox, oy;              /* seac's accent offset */
    int done;
} t1run;

static void t1_move(t1run* R, double x, double y) {
    if (R->open && R->sink->close) {
        R->sink->close(R->user);
    }

    R->sink->move(R->user, x + R->ox, y + R->oy);
    R->open = 1;
}

static int t1_glyph(t1run* R, const unsigned char* cs, int32_t len, int dec);

static void t1_seac(t1run* R, double asb, double adx, double ady, int bchar, int achar) {
    const char* bn = enc_name("StandardEncoding", bchar), *an = enc_name("StandardEncoding", achar);
    int32_t b = t1_by_name(R->f, bn), a = t1_by_name(R->f, an);
    double sbx = R->sbx;

    if (b >= 0) {
        t1run S = *R;

        S.n = S.nps = 0;
        S.flex = 0;
        S.x = S.y = 0;
        S.ox = R->ox;
        S.oy = R->oy;
        S.done = 0;
        t1_glyph(&S, R->f->t1[b].data, R->f->t1[b].len, 1);
        R->open = S.open;
    }

    if (a >= 0) {
        t1run S = *R;

        S.n = S.nps = 0;
        S.flex = 0;
        S.x = S.y = 0;
        S.ox = R->ox + adx - asb + sbx - sbx;
        S.oy = R->oy + ady;
        S.done = 0;
        S.open = R->open;
        t1_glyph(&S, R->f->t1[a].data, R->f->t1[a].len, 1);
        R->open = S.open;
    }

    R->done = 1;
}

static int t1_glyph(t1run* R, const unsigned char* cs, int32_t len, int dec) {
    unsigned char* buf;
    int32_t i;

    if (R->depth > 10 || len <= 0) {
        return 0;
    }

    buf = (unsigned char*)malloc((size_t)len);

    if (!buf) {
        return 0;
    }

    memcpy(buf, cs, (size_t)len);

    if (dec) {
        t1_decrypt(buf, (size_t)len, 4330);
    }

    i = R->f->lenIV >= 0 ? R->f->lenIV : 0;

    if (R->f->lenIV < 0) {  /* not encrypted */
        memcpy(buf, cs, (size_t)len);
    }

    while (i < len && !R->done) {
        int b = buf[i++];

        if (b >= 32) {
            double v;

            if (b <= 246) {
                v = b - 139;
            } else if (b <= 250) {
                v = (b - 247) * 256 + (i < len ? buf[i] : 0) + 108;
                i++;
            } else if (b <= 254) {
                v = -(b - 251) * 256 - (i < len ? buf[i] : 0) - 108;
                i++;
            } else {
                int32_t x = 0;
                int k;

                for (k = 0; k < 4; k++) {
                    x = (int32_t)((uint32_t)x << 8 | (i < len ? buf[i] : 0));
                    i++;
                }

                v = x;
            }

            if (R->n < 32) {
                R->st[R->n++] = v;
            }

            continue;
        }

        switch (b) {
            case 1: case 3:     /* hstem vstem */
                R->n = 0;
                break;

            case 4:     /* vmoveto */
                if (R->n >= 1) {
                    R->y += R->st[R->n - 1];

                    if (!R->flex) {
                        t1_move(R, R->x, R->y);
                    }
                }

                R->n = 0;
                break;

            case 5:     /* rlineto */
                if (R->n >= 2) {
                    R->x += R->st[0];
                    R->y += R->st[1];
                    R->sink->line(R->user, R->x + R->ox, R->y + R->oy);
                }

                R->n = 0;
                break;

            case 6:     /* hlineto */
                if (R->n >= 1) {
                    R->x += R->st[0];
                    R->sink->line(R->user, R->x + R->ox, R->y + R->oy);
                }

                R->n = 0;
                break;

            case 7:     /* vlineto */
                if (R->n >= 1) {
                    R->y += R->st[0];
                    R->sink->line(R->user, R->x + R->ox, R->y + R->oy);
                }

                R->n = 0;
                break;

            case 8:     /* rrcurveto */
                if (R->n >= 6) {
                    double x1 = R->x + R->st[0], y1 = R->y + R->st[1], x2 = x1 + R->st[2], y2 = y1 + R->st[3];

                    R->x = x2 + R->st[4];
                    R->y = y2 + R->st[5];
                    R->sink->cubic(R->user, x1 + R->ox, y1 + R->oy, x2 + R->ox, y2 + R->oy, R->x + R->ox,
                                   R->y + R->oy);
                }

                R->n = 0;
                break;

            case 9:     /* closepath */
                if (R->open && R->sink->close) {
                    R->sink->close(R->user);
                }

                R->open = 0;
                R->n = 0;
                break;

            case 10: {  /* callsubr */
                int32_t s = R->n ? (int32_t)R->st[--R->n] : -1;

                if (s >= 0 && s < R->f->nsubrs && R->f->subrs[s].data) {
                    R->depth++;
                    t1_glyph(R, R->f->subrs[s].data, R->f->subrs[s].len, 1);
                    R->depth--;
                }

                break;
            }

            case 11:    /* return */
                free(buf);
                return 1;

            case 13:    /* hsbw */
                if (R->n >= 2) {
                    R->sbx = R->st[0];
                    R->wx = R->st[1];
                    R->x = R->st[0];
                    R->y = 0;
                }

                R->n = 0;
                break;

            case 14:    /* endchar */
                if (R->open && R->sink->close) {
                    R->sink->close(R->user);
                }

                R->open = 0;
                R->done = 1;
                break;

            case 21:    /* rmoveto */
                if (R->n >= 2) {
                    R->x += R->st[R->n - 2];
                    R->y += R->st[R->n - 1];

                    if (!R->flex) {
                        t1_move(R, R->x, R->y);
                    }
                }

                R->n = 0;
                break;

            case 22:    /* hmoveto */
                if (R->n >= 1) {
                    R->x += R->st[R->n - 1];

                    if (!R->flex) {
                        t1_move(R, R->x, R->y);
                    }
                }

                R->n = 0;
                break;

            case 30:    /* vhcurveto */
                if (R->n >= 4) {
                    double x1 = R->x, y1 = R->y + R->st[0], x2 = x1 + R->st[1], y2 = y1 + R->st[2];

                    R->x = x2 + R->st[3];
                    R->y = y2;
                    R->sink->cubic(R->user, x1 + R->ox, y1 + R->oy, x2 + R->ox, y2 + R->oy, R->x + R->ox,
                                   R->y + R->oy);
                }

                R->n = 0;
                break;

            case 31:    /* hvcurveto */
                if (R->n >= 4) {
                    double x1 = R->x + R->st[0], y1 = R->y, x2 = x1 + R->st[1], y2 = y1 + R->st[2];

                    R->x = x2;
                    R->y = y2 + R->st[3];
                    R->sink->cubic(R->user, x1 + R->ox, y1 + R->oy, x2 + R->ox, y2 + R->oy, R->x + R->ox,
                                   R->y + R->oy);
                }

                R->n = 0;
                break;

            case 12: {
                int e = i < len ? buf[i++] : 0;

                switch (e) {
                    case 6:     /* seac */
                        if (R->n >= 5) {
                            t1_seac(R, R->st[0], R->st[1], R->st[2], (int)R->st[3], (int)R->st[4]);
                        }

                        R->n = 0;
                        break;

                    case 7:     /* sbw */
                        if (R->n >= 4) {
                            R->sbx = R->st[0];
                            R->x = R->st[0];
                            R->y = R->st[1];
                            R->wx = R->st[2];
                        }

                        R->n = 0;
                        break;

                    case 12:    /* div */
                        if (R->n >= 2) {
                            R->st[R->n - 2] = R->st[R->n - 1] ? R->st[R->n - 2] / R->st[R->n - 1] : 0;
                            R->n--;
                        }

                        break;

                    case 16: {  /* callothersubr */
                        int other, cnt, k;

                        if (R->n < 2) {
                            R->n = 0;
                            break;
                        }

                        other = (int)R->st[--R->n];
                        cnt = (int)R->st[--R->n];

                        if (cnt < 0 || cnt > R->n) {
                            cnt = R->n;
                        }

                        R->nps = 0;

                        if (other == 1) {           /* flex starts */
                            R->flex = 1;
                            R->nflex = 0;
                        } else if (other == 2) {    /* a flex point */
                            if (R->nflex < 8) {
                                R->fx[R->nflex] = R->x;
                                R->fy[R->nflex] = R->y;
                                R->nflex++;
                            }
                        } else if (other == 0 && R->flex) {     /* flex ends: two curves through the points */
                            R->flex = 0;

                            if (R->nflex >= 7) {
                                R->sink->cubic(R->user, R->fx[1] + R->ox, R->fy[1] + R->oy, R->fx[2] + R->ox,
                                               R->fy[2] + R->oy, R->fx[3] + R->ox, R->fy[3] + R->oy);
                                R->sink->cubic(R->user, R->fx[4] + R->ox, R->fy[4] + R->oy, R->fx[5] + R->ox,
                                               R->fy[5] + R->oy, R->fx[6] + R->ox, R->fy[6] + R->oy);
                            }

                            R->ps[R->nps++] = R->y;
                            R->ps[R->nps++] = R->x;
                            R->n -= cnt;
                            break;
                        }

                        if (other == 3) {   /* hint replacement: the pop that follows gets 3 */
                            R->n -= cnt;
                            R->ps[R->nps++] = 3;
                            break;
                        }

                        for (k = 0; k < cnt && R->nps < 32; k++) {
                            R->ps[R->nps++] = R->st[R->n - 1 - k];
                        }

                        R->n -= cnt;
                        break;
                    }

                    case 17:    /* pop */
                        if (R->nps > 0 && R->n < 32) {
                            R->st[R->n++] = R->ps[--R->nps];
                        }

                        break;

                    case 33:    /* setcurrentpoint */
                        if (R->n >= 2) {
                            R->x = R->st[0];
                            R->y = R->st[1];
                        }

                        R->n = 0;
                        break;

                    default:    /* dotsection, vstem3, hstem3 */
                        R->n = 0;
                        break;
                }

                break;
            }

            default:
                R->n = 0;
                break;
        }
    }

    free(buf);
    return 1;
}

/* ------------------------------------------------------------------ */
/* CMaps                                                              */
/* ------------------------------------------------------------------ */

static uint32_t str_code(pv s) {
    uint32_t c = 0;
    int32_t i;

    for (i = 0; i < s.n && i < 4; i++) {
        c = (c << 8) | (unsigned char)s.u.s[i];
    }

    return c;
}

static void add_tu(pbuf* b, uint32_t code, const unsigned char* dst, int32_t dn, uint32_t add) {
    tuni t;
    int32_t i;

    memset(&t, 0, sizeof(t));
    t.code = code;

    for (i = 0; i + 1 < dn && t.n < 4; i += 2) {
        uint32_t c = ((uint32_t)dst[i] << 8) | dst[i + 1];

        if (c >= 0xD800 && c < 0xDC00 && i + 3 < dn) {
            uint32_t c2 = ((uint32_t)dst[i + 2] << 8) | dst[i + 3];

            c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
            i += 2;
        }

        t.cp[t.n++] = c;
    }

    if (dn == 1) {
        t.cp[t.n++] = dst[0];
    }

    if (t.n) {
        t.cp[t.n - 1] += add;
    }

    qb_put(b, &t, sizeof(t));
}

static int cmp_tu(const void* a, const void* b) {
    uint32_t x = ((const tuni*)a)->code, y = ((const tuni*)b)->code;

    return x < y ? -1 : x > y;
}

static void parse_tounicode(pfont* f, pv stream) {
    pdoc* d = f->doc;
    size_t n;
    unsigned char* data = pd_stream_data(d, stream, &n, NULL, NULL);
    plex L;
    pbuf b;
    pv prev[3];

    if (!data) {
        return;
    }

    memset(&L, 0, sizeof(L));
    memset(&b, 0, sizeof(b));
    memset(prev, 0, sizeof(prev));
    L.p = data;
    L.n = n;
    L.d = d;
    L.content = 1;

    for (;;) {
        pv v = pl_read(&L);

        if (v.t == PV_ERR) {
            break;
        }

        if (v.t == PV_OP && strcmp(v.u.s, "beginbfchar") == 0) {
            for (;;) {
                pv a = pl_read(&L), c;

                if (a.t != PV_STRING) {
                    break;
                }

                c = pl_read(&L);

                if (c.t == PV_STRING) {
                    add_tu(&b, str_code(a), (const unsigned char*)c.u.s, c.n, 0);
                } else if (c.t == PV_NAME) {
                    uint32_t cp = name_unicode(c.u.s);
                    unsigned char u[2] = { (unsigned char)(cp >> 8), (unsigned char)cp };

                    if (cp) {
                        add_tu(&b, str_code(a), u, 2, 0);
                    }
                }
            }
        } else if (v.t == PV_OP && strcmp(v.u.s, "beginbfrange") == 0) {
            for (;;) {
                pv lo = pl_read(&L), hi, dst;
                uint32_t a, z, k;

                if (lo.t != PV_STRING) {
                    break;
                }

                hi = pl_read(&L);
                dst = pl_read(&L);
                a = str_code(lo);
                z = hi.t == PV_STRING ? str_code(hi) : a;

                if (z < a || z - a > 65535) {
                    continue;
                }

                for (k = 0; k <= z - a; k++) {
                    if (dst.t == PV_STRING) {
                        add_tu(&b, a + k, (const unsigned char*)dst.u.s, dst.n, k);
                    } else if (dst.t == PV_ARRAY && (int32_t)k < dst.n && dst.u.a[k].t == PV_STRING) {
                        add_tu(&b, a + k, (const unsigned char*)dst.u.a[k].u.s, dst.u.a[k].n, 0);
                    }
                }
            }
        }
    }

    free(data);
    f->tu = (tuni*)b.p;
    f->ntu = (int32_t)(b.n / sizeof(tuni));

    if (f->ntu > 1) {
        qsort(f->tu, (size_t)f->ntu, sizeof(tuni), cmp_tu);
    }
}

static void parse_cmap(pfont* f, const unsigned char* data, size_t n, int depth);

static void load_cmap_file(pfont* f, const char* name, int depth) {
    static const char* const dirs[] = {
        "/usr/share/poppler/cMap/Adobe-Japan1/", "/usr/share/poppler/cMap/Adobe-GB1/",
        "/usr/share/poppler/cMap/Adobe-CNS1/", "/usr/share/poppler/cMap/Adobe-Korea1/",
        "/usr/share/fonts/cmap/adobe-japan1/", "/usr/share/fonts/cmap/adobe-gb1/",
        "/usr/share/fonts/cmap/adobe-cns1/", "/usr/share/fonts/cmap/adobe-korea1/"
    };
    size_t i;

    if (depth > 4) {
        return;
    }

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[600];
        FILE* fp;

        snprintf(path, sizeof(path), "%s%s", dirs[i], name);
        fp = fopen(path, "rb");

        if (fp) {
            pbuf b;
            char tmp[8192];
            size_t r;

            memset(&b, 0, sizeof(b));

            while ((r = fread(tmp, 1, sizeof(tmp), fp)) > 0) {
                qb_put(&b, tmp, r);
            }

            fclose(fp);

            if (b.p) {
                parse_cmap(f, b.p, b.n, depth + 1);
            }

            qb_free(&b);
            return;
        }
    }
}

static void add_range(pbuf* b, uint32_t lo, uint32_t hi, uint32_t cid, int nbytes) {
    crange r;

    r.lo = lo;
    r.hi = hi;
    r.cid = cid;
    r.nbytes = (uint8_t)nbytes;
    qb_put(b, &r, sizeof(r));
}

static void parse_cmap(pfont* f, const unsigned char* data, size_t n, int depth) {
    plex L;
    pbuf b;
    pv last = pv_null();

    memset(&L, 0, sizeof(L));
    memset(&b, 0, sizeof(b));
    L.p = data;
    L.n = n;
    L.d = f->doc;
    L.content = 1;

    if (f->cmap) {
        qb_put(&b, f->cmap, sizeof(crange) * (size_t)f->ncmap);
        free(f->cmap);
        f->cmap = NULL;
    }

    for (;;) {
        pv v = pl_read(&L);

        if (v.t == PV_ERR) {
            break;
        }

        if (v.t == PV_OP && strcmp(v.u.s, "usecmap") == 0 && last.t == PV_NAME) {
            f->cmap = (crange*)b.p;
            f->ncmap = (int32_t)(b.n / sizeof(crange));
            memset(&b, 0, sizeof(b));

            if (strncmp(last.u.s, "Identity", 8) == 0) {
                f->identity = 1;
            } else {
                load_cmap_file(f, last.u.s, depth);
            }

            if (f->cmap) {
                qb_put(&b, f->cmap, sizeof(crange) * (size_t)f->ncmap);
                free(f->cmap);
                f->cmap = NULL;
            }
        } else if (v.t == PV_OP && strcmp(v.u.s, "begincodespacerange") == 0) {
            for (;;) {
                pv lo = pl_read(&L), hi;
                uint32_t a, z;

                if (lo.t != PV_STRING) {
                    break;
                }

                hi = pl_read(&L);

                if (lo.n < 1 || lo.n > 4) {
                    continue;
                }

                a = (unsigned char)lo.u.s[0];
                z = hi.t == PV_STRING && hi.n ? (unsigned char)hi.u.s[0] : a;

                for (; a <= z && a < 256; a++) {
                    f->cs_nbytes[a] = (uint8_t)lo.n;
                }

                f->has_codespace = 1;
            }
        } else if (v.t == PV_OP && (strcmp(v.u.s, "begincidrange") == 0 || strcmp(v.u.s, "begincidchar") == 0)) {
            int range = v.u.s[8] == 'r';

            for (;;) {
                pv lo = pl_read(&L), hi, cid;

                if (lo.t != PV_STRING) {
                    break;
                }

                hi = range ? pl_read(&L) : lo;
                cid = pl_read(&L);

                if (cid.t == PV_INT && hi.t == PV_STRING) {
                    add_range(&b, str_code(lo), str_code(hi), (uint32_t)cid.u.i, lo.n);
                }
            }
        }

        last = v;
    }

    f->cmap = (crange*)b.p;
    f->ncmap = (int32_t)(b.n / sizeof(crange));
}

/* ------------------------------------------------------------------ */
/* system fonts                                                       */
/* ------------------------------------------------------------------ */

static unsigned char* read_file(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    long len;
    unsigned char* d;

    if (!fp) {
        return NULL;
    }

    if (fseek(fp, 0, SEEK_END) || (len = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return NULL;
    }

    d = (unsigned char*)malloc((size_t)len);

    if (d && fread(d, 1, (size_t)len, fp) != (size_t)len) {
        free(d);
        d = NULL;
    }

    fclose(fp);
    *n = (size_t)len;
    return d;
}

/* families: 0 sans, 1 serif, 2 mono, 3 symbol, 4 dingbats; style: 1 bold, 2 italic */
static const char* const sys_paths[5][4][8] = {
    {   /* sans */
        {"/usr/share/fonts/opentype/urw-base35/NimbusSans-Regular.otf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
         "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
         "C:/Windows/Fonts/arial.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusSans-Bold.otf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
         "/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
         "C:/Windows/Fonts/arialbd.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusSans-Italic.otf", "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
         "/usr/share/fonts/liberation-sans/LiberationSans-Italic.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Oblique.ttf",
         "C:/Windows/Fonts/ariali.ttf", "/System/Library/Fonts/Supplemental/Arial Italic.ttf", "/Library/Fonts/Arial Italic.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusSans-BoldItalic.otf", "/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf",
         "/usr/share/fonts/liberation-sans/LiberationSans-BoldItalic.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-BoldOblique.ttf",
         "C:/Windows/Fonts/arialbi.ttf", "/System/Library/Fonts/Supplemental/Arial Bold Italic.ttf", "/Library/Fonts/Arial Bold Italic.ttf", NULL}
    },
    {   /* serif */
        {"/usr/share/fonts/opentype/urw-base35/NimbusRoman-Regular.otf", "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
         "/usr/share/fonts/liberation-serif/LiberationSerif-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
         "C:/Windows/Fonts/times.ttf", "/System/Library/Fonts/Supplemental/Times New Roman.ttf", "/Library/Fonts/Times New Roman.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusRoman-Bold.otf", "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf",
         "/usr/share/fonts/liberation-serif/LiberationSerif-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf",
         "C:/Windows/Fonts/timesbd.ttf", "/System/Library/Fonts/Supplemental/Times New Roman Bold.ttf", "/Library/Fonts/Times New Roman Bold.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusRoman-Italic.otf", "/usr/share/fonts/truetype/liberation/LiberationSerif-Italic.ttf",
         "/usr/share/fonts/liberation-serif/LiberationSerif-Italic.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Italic.ttf",
         "C:/Windows/Fonts/timesi.ttf", "/System/Library/Fonts/Supplemental/Times New Roman Italic.ttf", "/Library/Fonts/Times New Roman Italic.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusRoman-BoldItalic.otf", "/usr/share/fonts/truetype/liberation/LiberationSerif-BoldItalic.ttf",
         "/usr/share/fonts/liberation-serif/LiberationSerif-BoldItalic.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSerif-BoldItalic.ttf",
         "C:/Windows/Fonts/timesbi.ttf", "/System/Library/Fonts/Supplemental/Times New Roman Bold Italic.ttf", "/Library/Fonts/Times New Roman Bold Italic.ttf", NULL}
    },
    {   /* mono */
        {"/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Regular.otf", "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
         "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
         "C:/Windows/Fonts/cour.ttf", "/System/Library/Fonts/Supplemental/Courier New.ttf", "/Library/Fonts/Courier New.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Bold.otf", "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
         "/usr/share/fonts/liberation-mono/LiberationMono-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
         "C:/Windows/Fonts/courbd.ttf", "/System/Library/Fonts/Supplemental/Courier New Bold.ttf", "/Library/Fonts/Courier New Bold.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-Italic.otf", "/usr/share/fonts/truetype/liberation/LiberationMono-Italic.ttf",
         "/usr/share/fonts/liberation-mono/LiberationMono-Italic.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Oblique.ttf",
         "C:/Windows/Fonts/couri.ttf", "/System/Library/Fonts/Supplemental/Courier New Italic.ttf", "/Library/Fonts/Courier New Italic.ttf", NULL},
        {"/usr/share/fonts/opentype/urw-base35/NimbusMonoPS-BoldItalic.otf", "/usr/share/fonts/truetype/liberation/LiberationMono-BoldItalic.ttf",
         "/usr/share/fonts/liberation-mono/LiberationMono-BoldItalic.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-BoldOblique.ttf",
         "C:/Windows/Fonts/courbi.ttf", "/System/Library/Fonts/Supplemental/Courier New Bold Italic.ttf", "/Library/Fonts/Courier New Bold Italic.ttf", NULL}
    },
    {   /* symbol */
        {"/usr/share/fonts/opentype/urw-base35/StandardSymbolsPS.otf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
         "C:/Windows/Fonts/symbol.ttf", "/System/Library/Fonts/Symbol.ttf", NULL, NULL, NULL, NULL},
        {NULL}, {NULL}, {NULL}
    },
    {   /* dingbats */
        {"/usr/share/fonts/opentype/urw-base35/D050000L.otf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
         "C:/Windows/Fonts/seguisym.ttf", "/System/Library/Fonts/ZapfDingbats.ttf", NULL, NULL, NULL, NULL},
        {NULL}, {NULL}, {NULL}
    }
};

static int has_word(const char* s, const char* w) {
    size_t n = strlen(w);

    for (; *s; s++) {
        size_t k;

        for (k = 0; k < n && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)w[k]); k++) {
        }

        if (k == n) {
            return 1;
        }
    }

    return 0;
}

/* a file in the host's folders whose name is the font's, spaces and dashes aside */
static unsigned char* find_named(pdoc* d, const char* base, size_t* n) {
    static const char* const ext[] = { ".ttf", ".otf", ".TTF", ".OTF", ".ttc" };
    int k;
    size_t e;

    for (k = 0; k < d->nfont_dirs; k++) {
        for (e = 0; e < sizeof(ext) / sizeof(ext[0]); e++) {
            char path[700];
            unsigned char* data;

            snprintf(path, sizeof(path), "%s/%s%s", d->font_dirs[k], base, ext[e]);
            data = read_file(path, n);

            if (data) {
                return data;
            }
        }
    }

    return NULL;
}

static void load_system(pfont* f, pv desc) {
    int64_t flags = pd_getint(f->doc, desc, "Flags", 0);
    const char* b = f->base;
    int fam, style = 0, i;
    double weight = pd_getnum(f->doc, desc, "FontWeight", 400);
    size_t n;
    unsigned char* data = NULL;

    if (has_word(b, "Symbol")) {
        fam = 3;
    } else if (has_word(b, "Dingbat") || has_word(b, "Wingding")) {
        fam = 4;
    } else if (has_word(b, "Courier") || has_word(b, "Mono") || has_word(b, "Consol") || (flags & 1)) {
        fam = 2;
    } else if (has_word(b, "Times") || has_word(b, "Roman") || has_word(b, "Serif") || has_word(b, "Georgia") ||
               has_word(b, "Garamond") || has_word(b, "Cambria") || has_word(b, "Palatino") ||
               has_word(b, "Book") || has_word(b, "Minion") || has_word(b, "Century") ||
               ((flags & 2) && !has_word(b, "Sans") && !has_word(b, "Arial") && !has_word(b, "Helvetica"))) {
        fam = 1;
    } else {
        fam = 0;
    }

    if (has_word(b, "Bold") || has_word(b, "Black") || has_word(b, "Heavy") || has_word(b, "Semibold") ||
        weight >= 600 || (flags & (1 << 18))) {
        style |= 1;
    }

    if (has_word(b, "Italic") || has_word(b, "Oblique") || (flags & 64)) {
        style |= 2;
    }

    {
        char clean[96];
        size_t k, j = 0;

        for (k = 0; b[k] && j < sizeof(clean) - 1; k++) {
            if (b[k] != ' ') {
                clean[j++] = b[k];
            }
        }

        clean[j] = 0;
        data = find_named(f->doc, clean, &n);
    }

    if (fam >= 3) {
        style = 0;
    }

    for (i = 0; !data && i < 8 && sys_paths[fam][style][i]; i++) {
        data = read_file(sys_paths[fam][style][i], &n);
    }

    for (i = 0; !data && i < 8 && sys_paths[fam][0][i]; i++) {
        data = read_file(sys_paths[fam][0][i], &n);
    }

    for (i = 0; !data && i < 8 && sys_paths[0][0][i]; i++) {
        data = read_file(sys_paths[0][0][i], &n);
    }

    if (!data) {
        return;
    }

    if (pd_font_load_memory(data, n, 0, &f->sys) != PD_OK) {
        f->sys = NULL;
        free(data);
        return;
    }

    free(data);
    f->sys_symbol = fam >= 3;
}

/* ------------------------------------------------------------------ */
/* loading                                                            */
/* ------------------------------------------------------------------ */

static void strip_subset(const char* in, char* out, size_t cap) {
    size_t k;

    if (strlen(in) > 7 && in[6] == '+') {
        for (k = 0; k < 6 && in[k] >= 'A' && in[k] <= 'Z'; k++) {
        }

        if (k == 6) {
            in += 7;
        }
    }

    snprintf(out, cap, "%s", in);
}

static void load_program(pfont* f, pv desc) {
    pdoc* d = f->doc;
    pv ff = pd_get(d, desc, "FontFile"), ff2 = pd_get(d, desc, "FontFile2"), ff3 = pd_get(d, desc, "FontFile3");

    f->fm[0] = f->fm[3] = 1;

    if (ff.t == PV_STREAM) {
        if (setup_t1(f, ff)) {
            f->prog = PG_T1;
            return;
        }
    }

    if (ff2.t == PV_STREAM || ff3.t == PV_STREAM) {
        pv st = ff2.t == PV_STREAM ? ff2 : ff3;
        size_t n;
        unsigned char* data = pd_stream_data(d, st, &n, NULL, NULL);

        if (!data || n < 4) {
            free(data);
            return;
        }

        if ((data[0] == 0 && data[1] == 1 && data[2] == 0 && data[3] == 0) || memcmp(data, "OTTO", 4) == 0 ||
            memcmp(data, "true", 4) == 0 || memcmp(data, "ttcf", 4) == 0) {
            f->data = data;
            f->len = n;

            if (setup_sfnt(f)) {
                f->prog = PG_SFNT;

                if (f->pf.cff) {    /* its CFF read for the charset */
                    pfont tmp = *f;

                    (void)tmp;

                    if (setup_cff(f, f->pf.cff)) {
                        /* font matrix of the CFF inside an OpenType font is in its upem */
                        f->fm[0] = f->fm[3] = 1000.0 / f->upem;
                        f->fm[1] = f->fm[2] = f->fm[4] = f->fm[5] = 0;
                    }
                }

                return;
            }

            free(data);
            f->data = NULL;
            f->len = 0;
            return;
        }

        if (data[0] == 1 && data[1] == 0) {     /* a bare CFF: one byte before it so its offset is not 0 */
            unsigned char* c = (unsigned char*)malloc(n + 1);

            if (!c) {
                free(data);
                return;
            }

            c[0] = 0;
            memcpy(c + 1, data, n);
            free(data);
            f->data = c;
            f->len = n + 1;
            memset(&f->pf, 0, sizeof(f->pf));
            f->pf.data = c;
            f->pf.len = n + 1;
            f->pf.cff = 1;
            f->pf.cff_len = (uint32_t)n;
            f->pf.m.units_per_em = 1000;

            if (setup_cff(f, 1)) {
                f->pf.m.num_glyphs = f->cff_nglyphs;
                f->prog = PG_CFF;
                return;
            }

            free(c);
            f->data = NULL;
            f->len = 0;
            return;
        }

        if (data[0] == '%' && data[1] == '!') {     /* a Type 1 program in FontFile3 */
            free(data);

            if (setup_t1(f, st)) {
                f->prog = PG_T1;
            }

            return;
        }

        free(data);
    }
}

static void simple_widths(pfont* f) {
    pdoc* d = f->doc;
    pv w = pd_get(d, f->dict, "Widths");
    int64_t first = pd_getint(d, f->dict, "FirstChar", 0);
    int32_t i;

    if (w.t == PV_ARRAY) {
        for (i = 0; i < w.n; i++) {
            double v;
            int64_t c = first + i;

            if (c >= 0 && c < 256 && pd_num(d, w.u.a[i], &v)) {
                f->widths[c] = v;
                f->has_width[c] = 1;
            }
        }
    }
}

static void simple_encoding(pfont* f) {
    pdoc* d = f->doc;
    pv enc = pd_get(d, f->dict, "Encoding");
    const char* base = NULL;
    int c, builtin = 0;

    if (enc.t == PV_NAME) {
        base = enc.u.s;
    } else if (enc.t == PV_DICT) {
        base = pd_getname(d, enc, "BaseEncoding");
    }

    if (!base) {
        /* the font's own encoding: for symbolic fonts and embedded Type 1 */
        if (f->prog == PG_T1 || f->prog == PG_CFF || f->symbolic || f->kind == FT_TYPE3) {
            builtin = 1;
        }

        if (f->sys_symbol || has_word(f->base, "Symbol")) {
            base = "Symbol";
            builtin = 0;
        }
    }

    for (c = 0; c < 256; c++) {
        const char* nm = NULL;

        if (builtin) {
            if (f->prog == PG_T1) {
                nm = f->t1enc[c];
            } else if (f->prog == PG_CFF && f->cff_has_enc) {
                static char buf[64];

                (void)buf;
                nm = NULL;  /* by GID below */
            }
        }

        if (!nm && !builtin) {
            nm = enc_name(base ? base : (f->kind == FT_TRUETYPE ? "WinAnsiEncoding" : "StandardEncoding"), c);
        }

        if (!nm && builtin && f->prog != PG_CFF && f->kind != FT_TYPE3 && f->prog != PG_SFNT) {
            nm = enc_name("StandardEncoding", c);
        }

        f->names[c] = nm;
    }

    if (enc.t == PV_DICT) {
        pv diffs = pd_get(d, enc, "Differences");
        int32_t i;
        int64_t code = 0;

        for (i = 0; diffs.t == PV_ARRAY && i < diffs.n; i++) {
            pv v = pd_resolve(d, diffs.u.a[i]);

            if (v.t == PV_INT) {
                code = v.u.i;
            } else if (v.t == PV_NAME) {
                if (code >= 0 && code < 256) {
                    f->names[code] = v.u.s;
                }

                code++;
            }
        }
    }
}

/* a simple font's glyph for a code */
static int32_t simple_gid(pfont* f, int c) {
    const char* nm = f->names[c];

    if (f->prog == PG_T1) {
        int32_t g = t1_by_name(f, nm);

        if (g < 0 && f->t1enc[c]) {
            g = t1_by_name(f, f->t1enc[c]);
        }

        return g;
    }

    if (f->prog == PG_CFF || (f->prog == PG_SFNT && f->pf.cff && f->charset && !f->cmap31 && !f->cmap30)) {
        int32_t g = nm ? cff_by_name(f, nm) : -1;

        if (g < 0 && f->cff_has_enc && f->cff_enc[c]) {
            g = f->cff_enc[c];
        }

        if (g < 0 && f->cff_cid) {
            g = c;
        }

        return g;
    }

    if (f->prog == PG_SFNT) {
        uint32_t g = 0;

        if (f->symbolic || !f->cmap31) {
            if (f->cmap30) {
                g = cmap_glyph(f, f->cmap30, (uint32_t)c);

                if (!g) {
                    g = cmap_glyph(f, f->cmap30, 0xF000 + (uint32_t)c);
                }

                if (!g) {
                    g = cmap_glyph(f, f->cmap30, 0xF100 + (uint32_t)c);
                }

                if (!g) {
                    g = cmap_glyph(f, f->cmap30, 0xF200 + (uint32_t)c);
                }
            }

            if (!g && f->cmap10) {
                g = cmap_glyph(f, f->cmap10, (uint32_t)c);
            }

            if (!g && f->cmap31 && nm) {
                g = cmap_glyph(f, f->cmap31, name_unicode(nm));
            }
        } else {
            uint32_t u = name_unicode(nm);

            g = u ? cmap_glyph(f, f->cmap31, u) : 0;

            if (!g && f->cmap10 && nm) {
                g = cmap_glyph(f, f->cmap10, (uint32_t)c);
            }

            if (!g && f->cmap30) {
                g = cmap_glyph(f, f->cmap30, 0xF000 + (uint32_t)c);
            }
        }

        if (!g && nm) {
            int32_t pg = post_glyph(f, nm);

            if (pg > 0) {
                g = (uint32_t)pg;
            }
        }

        if (!g && !f->cmap31 && !f->cmap30 && !f->cmap10) {
            g = (uint32_t)c;    /* no cmap at all: codes are glyphs */
        }

        return g ? (int32_t)g : -1;
    }

    if (f->sys) {
        uint32_t u = name_unicode(nm), g;

        if (!u && c >= 32 && c < 127) {
            u = (uint32_t)c;
        }

        if (f->sys_symbol && c >= 32 && !nm) {
            u = symbol_enc[c - 32];
        }

        g = u ? pd_font_glyph_index(f->sys, u) : 0;

        if (!g && f->sys_symbol) {
            g = pd_font_glyph_index(f->sys, 0xF000 + (uint32_t)c);
        }

        return g ? (int32_t)g : -1;
    }

    return -1;
}

static void composite(pfont* f) {
    pdoc* d = f->doc;
    pv desc_arr = pd_get(d, f->dict, "DescendantFonts"), cf = pd_index(d, desc_arr, 0), enc;
    pv desc = pd_get(d, cf, "FontDescriptor"), w, map;
    const char* sub = pd_getname(d, cf, "Subtype");

    enc = pd_get(d, f->dict, "Encoding");

    if (enc.t == PV_NAME) {
        if (strncmp(enc.u.s, "Identity", 8) == 0) {
            f->identity = 1;
        } else {
            load_cmap_file(f, enc.u.s, 0);

            if (!f->ncmap) {
                f->identity = 1;    /* a predefined CMap not to hand: two bytes a code */
            }
        }

        f->vertical = enc.u.s[strlen(enc.u.s) - 1] == 'V' && enc.u.s[strlen(enc.u.s) - 2] == '-';
    } else if (enc.t == PV_STREAM) {
        size_t n;
        unsigned char* data = pd_stream_data(d, enc, &n, NULL, NULL);
        pv use = pd_get(d, enc, "UseCMap");

        if (use.t == PV_NAME && strncmp(use.u.s, "Identity", 8) == 0) {
            f->identity = 1;
        }

        if (data) {
            parse_cmap(f, data, n, 0);
            free(data);
        }

        f->vertical = pd_getint(d, enc, "WMode", 0) == 1;
    } else {
        f->identity = 1;
    }

    load_program(f, desc);

    if (f->prog == PG_NONE) {
        char b[96];

        strip_subset(pd_getname(d, cf, "BaseFont") ? pd_getname(d, cf, "BaseFont") : f->base, b, sizeof(b));
        snprintf(f->base, sizeof(f->base), "%s", b);
        load_system(f, desc);
    }

    f->dw = pd_getnum(d, cf, "DW", 1000);
    w = pd_get(d, cf, "W");

    if (w.t == PV_ARRAY) {
        pbuf b;
        int32_t i = 0;

        memset(&b, 0, sizeof(b));

        while (i < w.n) {
            pv a = pd_index(d, w, i), c2 = pd_index(d, w, i + 1);

            if (a.t != PV_INT) {
                break;
            }

            if (c2.t == PV_ARRAY) {
                int32_t k;

                for (k = 0; k < c2.n; k++) {
                    wrange r;
                    double v = 0;

                    pd_num(d, c2.u.a[k], &v);
                    r.lo = r.hi = (uint32_t)(a.u.i + k);
                    r.w = v;
                    qb_put(&b, &r, sizeof(r));
                }

                i += 2;
            } else {
                wrange r;
                double v = 0;
                pv z = pd_index(d, w, i + 2);

                pd_num(d, z, &v);
                r.lo = (uint32_t)a.u.i;
                r.hi = c2.t == PV_INT ? (uint32_t)c2.u.i : r.lo;
                r.w = v;
                qb_put(&b, &r, sizeof(r));
                i += 3;
            }
        }

        f->w = (wrange*)b.p;
        f->nw = (int32_t)(b.n / sizeof(wrange));
    }

    map = pd_get(d, cf, "CIDToGIDMap");

    if (map.t == PV_STREAM) {
        size_t n, k;
        unsigned char* data = pd_stream_data(d, map, &n, NULL, NULL);

        if (data) {
            f->ncid2gid = (int32_t)(n / 2);
            f->cid2gid = (uint16_t*)malloc(sizeof(uint16_t) * ((size_t)f->ncid2gid + 1));

            for (k = 0; f->cid2gid && k < (size_t)f->ncid2gid; k++) {
                f->cid2gid[k] = (uint16_t)((data[2 * k] << 8) | data[2 * k + 1]);
            }

            free(data);
        }
    }

    (void)sub;
}

pfont* pf_load(pdoc* d, pv fontref) {
    pv fd = pd_resolve(d, fontref), desc, tu;
    pfont* f;
    const char* sub, *bf;

    if (fd.t != PV_DICT) {
        return NULL;
    }

    for (f = d->fonts; f; f = f->next) {
        if (f->dict.u.d == fd.u.d) {
            return f;
        }
    }

    f = (pfont*)calloc(1, sizeof(pfont));

    if (!f) {
        return NULL;
    }

    f->doc = d;
    f->dict = fd;
    f->next = d->fonts;
    d->fonts = f;
    sub = pd_getname(d, fd, "Subtype");
    bf = pd_getname(d, fd, "BaseFont");
    strip_subset(bf ? bf : "", f->base, sizeof(f->base));
    f->kind = !sub ? FT_TYPE1 : strcmp(sub, "TrueType") == 0 ? FT_TRUETYPE : strcmp(sub, "Type3") == 0 ? FT_TYPE3 :
              strcmp(sub, "Type0") == 0 ? FT_TYPE0 : FT_TYPE1;
    memset(f->gid, 0xFF, sizeof(f->gid));
    f->fm[0] = f->fm[3] = 1;

    if (f->kind == FT_TYPE0) {
        composite(f);
        desc = pd_get(d, pd_index(d, pd_get(d, fd, "DescendantFonts"), 0), "FontDescriptor");
    } else {
        desc = pd_get(d, fd, "FontDescriptor");
        f->symbolic = (pd_getint(d, desc, "Flags", 0) & 4) != 0;

        if (f->kind == FT_TYPE3) {
            double m[6];

            if (pd_matrix(d, pd_get(d, fd, "FontMatrix"), m)) {
                int k;

                for (k = 0; k < 6; k++) {
                    f->fm[k] = m[k] * 1000;
                }
            } else {
                f->fm[0] = f->fm[3] = 1;
            }

            f->charprocs = pd_get(d, fd, "CharProcs");
            f->resources = pd_get(d, fd, "Resources");
        } else {
            load_program(f, desc);

            if (f->prog == PG_NONE) {
                load_system(f, desc);
            }

            if (f->prog == PG_SFNT && !f->cmap31 && !f->cmap30 && f->cmap10) {
                f->symbolic = 1;
            }
        }

        simple_encoding(f);
        simple_widths(f);
    }

    f->missing = pd_getnum(d, desc, "MissingWidth", 0);
    tu = pd_get(d, fd, "ToUnicode");

    if (tu.t == PV_STREAM) {
        parse_tounicode(f, tu);
    }

    return f;
}

void pf_free_all(pdoc* d) {
    pfont* f = d->fonts;

    while (f) {
        pfont* n = f->next;
        int32_t i;

        free(f->data);
        free(f->charset);
        free(f->t1);
        free(f->subrs);
        free(f->t1plain);
        free(f->cmap);
        free(f->cid2gid);
        free(f->w);
        free(f->tu);
        free(f->gid2uni);

        if (f->sys) {
            pd_font_free(f->sys);
        }

        for (i = 0; i < f->ncache; i++) {
            free(f->cache[i].v);
        }

        free(f->cache);
        free(f->cached);
        free(f);
        f = n;
    }

    d->fonts = NULL;
}

/* ------------------------------------------------------------------ */
/* codes                                                              */
/* ------------------------------------------------------------------ */

int pf_next(pfont* f, const unsigned char* s, size_t n, uint32_t* code) {
    int k, nb;

    if (!n) {
        return 0;
    }

    if (f->kind != FT_TYPE0) {
        *code = s[0];
        return 1;
    }

    if (f->has_codespace && f->cs_nbytes[s[0]]) {
        nb = f->cs_nbytes[s[0]];
    } else if (f->ncmap && !f->identity) {
        /* the shortest code a range of the CMap has for this byte */
        int32_t i;

        nb = 2;

        for (i = 0; i < f->ncmap; i++) {
            if (f->cmap[i].nbytes == 1 && s[0] >= f->cmap[i].lo && s[0] <= f->cmap[i].hi) {
                nb = 1;
                break;
            }
        }
    } else {
        nb = 2;
    }

    if ((size_t)nb > n) {
        nb = (int)n;
    }

    *code = 0;

    for (k = 0; k < nb; k++) {
        *code = (*code << 8) | s[k];
    }

    return nb;
}

static uint32_t cid_of(pfont* f, uint32_t code) {
    int32_t i;

    if (f->kind != FT_TYPE0) {
        return code;
    }

    for (i = f->ncmap - 1; i >= 0; i--) {
        if (code >= f->cmap[i].lo && code <= f->cmap[i].hi) {
            return f->cmap[i].cid + (code - f->cmap[i].lo);
        }
    }

    return f->identity || !f->ncmap ? code : 0;
}

static void null_move(void* u, double x, double y) {
    (void)u;
    (void)x;
    (void)y;
}

static void null_cubic(void* u, double a, double b, double c, double d, double x, double y) {
    (void)u;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)x;
    (void)y;
}

static void null_close(void* u) {
    (void)u;
}

double pf_width(pfont* f, uint32_t code) {
    if (f->kind == FT_TYPE0) {
        uint32_t cid = cid_of(f, code);
        int32_t i;

        for (i = 0; i < f->nw; i++) {
            if (cid >= f->w[i].lo && cid <= f->w[i].hi) {
                return f->w[i].w / 1000;
            }
        }

        return f->dw / 1000;
    }

    if (code < 256 && f->has_width[code]) {
        double w = f->widths[code];

        return f->kind == FT_TYPE3 ? w * f->fm[0] / 1000 : w / 1000;
    }

    /* no width given: the program's own */
    if (code < 256) {
        int32_t g = f->gid[code] == -1 ? (f->gid[code] = simple_gid(f, (int)code) >= 0 ? simple_gid(f, (int)code) :
                                          -2) : f->gid[code];

        if (g >= 0) {
            if (f->sys) {
                pd_font_metrics m;

                pd_font_get_metrics(f->sys, &m);
                return m.units_per_em ? pd_font_glyph_advance(f->sys, (uint32_t)g) / (double)m.units_per_em : 0.5;
            }

            if (f->prog == PG_SFNT && f->hmtx && f->nhm) {
                uint32_t i = (uint32_t)g < f->nhm ? (uint32_t)g : f->nhm - 1;

                return rd16(f, f->hmtx + 4 * i) / (double)f->upem;
            }

            if (f->prog == PG_T1 && g < f->nt1) {
                /* the advance hsbw (or sbw) gives, the outline itself drawn into nothing */
                t1run R;
                pf_sink z;

                memset(&R, 0, sizeof(R));
                z.move = null_move;
                z.line = null_move;
                z.cubic = null_cubic;
                z.close = null_close;
                R.f = f;
                R.sink = &z;
                t1_glyph(&R, f->t1[g].data, f->t1[g].len, 1);
                return R.wx * f->fm[0] / 1000;
            }
        }
    }

    return f->missing ? f->missing / 1000 : 0.5;
}

int pf_is_type3(pfont* f) {
    return f->kind == FT_TYPE3;
}

int pf_vertical(pfont* f) {
    return f->vertical;
}

pv pf_type3_proc(pfont* f, uint32_t code, double* m, pv* res) {
    const char* nm = code < 256 ? f->names[code] : NULL;
    int k;

    for (k = 0; k < 6; k++) {
        m[k] = f->fm[k] / 1000;
    }

    *res = f->resources;
    return nm ? pd_get(f->doc, f->charprocs, nm) : pv_null();
}

/* the glyph id of a CID font's code */
static int32_t cid_gid(pfont* f, uint32_t code) {
    uint32_t cid = cid_of(f, code);

    if (f->prog == PG_CFF || (f->prog == PG_SFNT && f->pf.cff && f->charset)) {
        if (f->cff_cid && f->charset) {     /* CID -> GID through the charset */
            int32_t g;

            if (cid < (uint32_t)f->cff_nglyphs && f->charset[cid] == cid) {
                return (int32_t)cid;
            }

            for (g = 0; g < f->cff_nglyphs; g++) {
                if (f->charset[g] == cid) {
                    return g;
                }
            }

            return -1;
        }

        return (int32_t)cid;
    }

    if (f->prog == PG_SFNT) {
        if (f->cid2gid) {
            return cid < (uint32_t)f->ncid2gid ? f->cid2gid[cid] : -1;
        }

        return (int32_t)cid;
    }

    if (f->sys) {   /* not embedded: by its text */
        uint32_t u[4];

        if (pf_unicode(f, code, u, 4) > 0) {
            uint32_t g = pd_font_glyph_index(f->sys, u[0]);

            return g ? (int32_t)g : -1;
        }
    }

    return -1;
}

uint32_t pf_glyph_key(pfont* f, uint32_t code) {
    if (f->kind == FT_TYPE0) {
        int32_t g = cid_gid(f, code);

        return g < 0 ? 0xFFFFFFFFu : (uint32_t)g;
    }

    if (code < 256) {
        if (f->gid[code] == -1) {
            int32_t g = simple_gid(f, (int)code);

            f->gid[code] = g >= 0 ? g : -2;
        }

        return f->gid[code] >= 0 ? (uint32_t)f->gid[code] : 0xFFFFFFFFu;
    }

    return 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/* outlines                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    pbuf b;
    int32_t n;
    double sx, sy;              /* 26.6 to glyph space */
    double qx, qy;              /* the current point (quadratics need it) */
} grec;

static void gr_put(grec* g, int op, const double* v, int nv) {
    float f[7];
    int k;

    f[0] = (float)op;

    for (k = 0; k < nv; k++) {
        f[k + 1] = (float)v[k];
    }

    qb_put(&g->b, f, sizeof(float) * (size_t)(nv + 1));
    g->n++;
}

static void s_move(void* u, int32_t x, int32_t y) {
    grec* g = (grec*)u;
    double v[2] = { x * g->sx, y * g->sy };

    gr_put(g, 0, v, 2);
    g->qx = v[0];
    g->qy = v[1];
}

static void s_line(void* u, int32_t x, int32_t y) {
    grec* g = (grec*)u;
    double v[2] = { x * g->sx, y * g->sy };

    gr_put(g, 1, v, 2);
    g->qx = v[0];
    g->qy = v[1];
}

static void s_quad(void* u, int32_t cx, int32_t cy, int32_t x, int32_t y) {
    grec* g = (grec*)u;
    double qx = cx * g->sx, qy = cy * g->sy, ex = x * g->sx, ey = y * g->sy;
    double v[6] = { g->qx + 2.0 / 3 * (qx - g->qx), g->qy + 2.0 / 3 * (qy - g->qy), ex + 2.0 / 3 * (qx - ex),
                    ey + 2.0 / 3 * (qy - ey), ex, ey };

    gr_put(g, 2, v, 6);
    g->qx = ex;
    g->qy = ey;
}

static void s_cubic(void* u, int32_t a, int32_t b, int32_t c, int32_t d, int32_t x, int32_t y) {
    grec* g = (grec*)u;
    double v[6] = { a * g->sx, b * g->sy, c * g->sx, d * g->sy, x * g->sx, y * g->sy };

    gr_put(g, 2, v, 6);
    g->qx = v[4];
    g->qy = v[5];
}

static void s_close(void* u) {
    gr_put((grec*)u, 3, NULL, 0);
}

static void t_move(void* u, double x, double y) {
    double v[2] = { x, y };

    gr_put((grec*)u, 0, v, 2);
}

static void t_line(void* u, double x, double y) {
    double v[2] = { x, y };

    gr_put((grec*)u, 1, v, 2);
}

static void t_cubic(void* u, double a, double b, double c, double d, double x, double y) {
    double v[6] = { a, b, c, d, x, y };

    gr_put((grec*)u, 2, v, 6);
}

static void t_close(void* u) {
    gr_put((grec*)u, 3, NULL, 0);
}

/* the outline of glyph g of the program, recorded in its units (font matrix not yet applied) */
static void record_glyph(pfont* f, int32_t g, grec* R, int use_sys) {
    pd_outline_sink s;

    s.move_to = s_move;
    s.line_to = s_line;
    s.quad_to = s_quad;
    s.cubic_to = s_cubic;
    s.close = s_close;

    if (use_sys) {
        pd_font_metrics m;

        pd_font_get_metrics(f->sys, &m);
        R->sx = R->sy = 1000.0 / (64.0 * (m.units_per_em ? m.units_per_em : 1000));
        pd_font_glyph_outline(f->sys, (uint32_t)g, &s, R);
        return;
    }

    if (f->prog == PG_T1) {
        t1run T;
        pf_sink ts;

        if (g < 0 || g >= f->nt1) {
            return;
        }

        ts.move = t_move;
        ts.line = t_line;
        ts.cubic = t_cubic;
        ts.close = t_close;
        memset(&T, 0, sizeof(T));
        T.f = f;
        T.sink = &ts;
        T.user = R;
        t1_glyph(&T, f->t1[g].data, f->t1[g].len, 1);
        return;
    }

    R->sx = R->sy = 1.0 / 64;
    pd_font_glyph_outline(&f->pf, (uint32_t)g, &s, R);
}

int pf_outline(pfont* f, uint32_t code, const pf_sink* sink, void* user) {
    uint32_t key = pf_glyph_key(f, code);
    int use_sys = f->prog == PG_NONE && f->sys;
    gpath* gp;
    int32_t i;
    const float* v;
    double m[6];

    if (key == 0xFFFFFFFFu || key > 1000000) {
        return -1;
    }

    if ((int32_t)key >= f->ncache) {
        int32_t nc = (int32_t)key + 64;
        gpath* c = (gpath*)realloc(f->cache, sizeof(gpath) * (size_t)nc);
        uint8_t* cd = (uint8_t*)realloc(f->cached, (size_t)nc);

        if (!c || !cd) {
            if (c) {
                f->cache = c;
            }

            if (cd) {
                f->cached = cd;
            }

            return -1;
        }

        memset(c + f->ncache, 0, sizeof(gpath) * (size_t)(nc - f->ncache));
        memset(cd + f->ncache, 0, (size_t)(nc - f->ncache));
        f->cache = c;
        f->cached = cd;
        f->ncache = nc;
    }

    gp = &f->cache[key];

    if (!f->cached[key]) {
        grec R;

        memset(&R, 0, sizeof(R));
        record_glyph(f, (int32_t)key, &R, use_sys);
        gp->v = (float*)R.b.p;
        gp->nv = (int32_t)(R.b.n / sizeof(float));
        gp->n = R.n;
        f->cached[key] = 1;
    }

    if (use_sys) {
        m[0] = m[3] = 1;
        m[1] = m[2] = m[4] = m[5] = 0;
    } else {
        memcpy(m, f->fm, sizeof(m));
    }

    v = gp->v;

    for (i = 0; i < gp->nv;) {
        int op = (int)v[i];

#define TX(a, b) (m[0] * (a) + m[2] * (b) + m[4])
#define TY(a, b) (m[1] * (a) + m[3] * (b) + m[5])

        if (op == 0 && i + 2 < gp->nv) {
            sink->move(user, TX(v[i + 1], v[i + 2]), TY(v[i + 1], v[i + 2]));
            i += 3;
        } else if (op == 1 && i + 2 < gp->nv) {
            sink->line(user, TX(v[i + 1], v[i + 2]), TY(v[i + 1], v[i + 2]));
            i += 3;
        } else if (op == 2 && i + 6 < gp->nv) {
            sink->cubic(user, TX(v[i + 1], v[i + 2]), TY(v[i + 1], v[i + 2]), TX(v[i + 3], v[i + 4]),
                        TY(v[i + 3], v[i + 4]), TX(v[i + 5], v[i + 6]), TY(v[i + 5], v[i + 6]));
            i += 7;
        } else if (op == 3) {
            if (sink->close) {
                sink->close(user);
            }

            i += 1;
        } else {
            break;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* text                                                               */
/* ------------------------------------------------------------------ */

static void build_gid2uni(pfont* f) {
    uint32_t sub = f->cmap31, fmt;
    uint32_t* map;

    if (f->gid2uni || f->prog != PG_SFNT || !sub || f->nglyphs <= 0) {
        return;
    }

    map = (uint32_t*)calloc((size_t)f->nglyphs, sizeof(uint32_t));

    if (!map) {
        return;
    }

    fmt = rd16(f, sub);

    if (fmt == 4) {
        uint32_t segx2 = rd16(f, sub + 6), i;
        uint32_t ends = sub + 14, starts = ends + segx2 + 2;

        for (i = 0; i < segx2; i += 2) {
            uint32_t e = rd16(f, ends + i), s = rd16(f, starts + i), c;

            for (c = s; c <= e && c < 0xFFFF; c++) {
                uint32_t g = cmap_glyph(f, sub, c);

                if (g && (int32_t)g < f->nglyphs && !map[g]) {
                    map[g] = c;
                }
            }
        }
    } else if (fmt == 12) {
        uint32_t n = rd32(f, sub + 12), i;

        for (i = 0; i < n && i < 100000; i++) {
            uint32_t s = rd32(f, sub + 16 + 12 * i), e = rd32(f, sub + 20 + 12 * i), g0 = rd32(f, sub + 24 + 12 * i), c;

            for (c = s; c <= e && c - s < 65536; c++) {
                uint32_t g = g0 + (c - s);

                if ((int32_t)g < f->nglyphs && !map[g]) {
                    map[g] = c;
                }
            }
        }
    }

    f->gid2uni = map;
    f->ngid2uni = f->nglyphs;
}

int pf_unicode(pfont* f, uint32_t code, uint32_t* out, int cap) {
    if (f->ntu) {
        int32_t lo = 0, hi = f->ntu - 1;

        while (lo <= hi) {
            int32_t mid = (lo + hi) / 2;

            if (f->tu[mid].code == code) {
                int k, n = f->tu[mid].n < cap ? f->tu[mid].n : cap;

                for (k = 0; k < n; k++) {
                    out[k] = f->tu[mid].cp[k];
                }

                return n;
            }

            if (f->tu[mid].code < code) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
    }

    if (f->kind != FT_TYPE0) {
        uint32_t u = code < 256 ? name_unicode(f->names[code]) : 0;

        if (!u && code < 256 && f->prog == PG_CFF && f->charset) {  /* by the glyph's own name */
            char nm[64];
            uint32_t g = pf_glyph_key(f, code);

            if (g != 0xFFFFFFFFu && cff_glyph_name(f, (int32_t)g, nm, sizeof(nm))) {
                u = name_unicode(nm);
            }
        }

        if (!u && code < 256 && f->names[code] == NULL && f->symbolic && f->prog == PG_SFNT) {
            u = code >= 32 ? code : 0;
        }

        if (!u && code >= 32 && code < 127 && !f->names[code]) {
            u = code;
        }

        if (u >= 0xF000 && u <= 0xF0FF) {
            u -= 0xF000;
        }

        if (u && cap > 0) {
            out[0] = u;
            return 1;
        }

        return 0;
    }

    /* a CID font without ToUnicode: its glyph's code point in the program's cmap */
    {
        int32_t g = cid_gid(f, code);

        build_gid2uni(f);

        if (f->gid2uni && g >= 0 && g < f->ngid2uni && f->gid2uni[g] && cap > 0) {
            out[0] = f->gid2uni[g];
            return 1;
        }

        if (f->prog == PG_CFF && f->charset && !f->cff_cid && cap > 0) {
            char nm[64];

            if (cff_glyph_name(f, g, nm, sizeof(nm)) && (out[0] = name_unicode(nm)) != 0) {
                return 1;
            }
        }
    }

    return 0;
}

size_t pf_encode(pfont* f, const char* utf8, unsigned char* out, size_t cap) {
    const unsigned char* p = (const unsigned char*)utf8;
    size_t k = 0;

    while (*p) {
        uint32_t c = *p, best = 0xFFFFFFFFu;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, j;

        c &= extra == 3 ? 7 : extra == 2 ? 15 : extra == 1 ? 31 : 127;
        p++;

        for (j = 0; j < extra && *p; j++, p++) {
            c = (c << 6) | (*p & 63);
        }

        /* a code whose text is c and whose glyph the font has */
        if (f->kind == FT_TYPE0) {
            int32_t i;

            for (i = 0; i < f->ntu && best == 0xFFFFFFFFu; i++) {
                if (f->tu[i].n == 1 && f->tu[i].cp[0] == c && pf_glyph_key(f, f->tu[i].code) != 0xFFFFFFFFu) {
                    best = f->tu[i].code;
                }
            }

            if (best == 0xFFFFFFFFu || k + 2 > cap) {
                return 0;
            }

            out[k++] = (unsigned char)(best >> 8);
            out[k++] = (unsigned char)best;
        } else {
            uint32_t code;

            for (code = 0; code < 256 && best == 0xFFFFFFFFu; code++) {
                uint32_t u[4];

                if (pf_unicode(f, code, u, 4) == 1 && u[0] == c &&
                    (pf_glyph_key(f, code) != 0xFFFFFFFFu || c == ' ')) {
                    best = code;
                }
            }

            if (best == 0xFFFFFFFFu || k + 1 > cap) {
                return 0;
            }

            out[k++] = (unsigned char)best;
        }
    }

    return k;
}

/* the standard 14 font most like this one (for text written over it): Times, Helvetica or Courier, its weight and
   slant kept */
const char* pf_std14(pfont* f) {
    static const char* const names[3][4] = {
        { "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique" },
        { "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic" },
        { "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique" }
    };
    pv desc;
    int64_t flags;
    const char* b;
    int fam = 0, style = 0;

    if (!f) {
        return "Helvetica";
    }

    b = f->base;
    desc = pd_get(f->doc, f->dict, "FontDescriptor");

    if (f->kind == FT_TYPE0) {
        desc = pd_get(f->doc, pd_index(f->doc, pd_get(f->doc, f->dict, "DescendantFonts"), 0), "FontDescriptor");
    }

    flags = pd_getint(f->doc, desc, "Flags", 0);

    if (has_word(b, "Courier") || has_word(b, "Mono") || (flags & 1)) {
        fam = 2;
    } else if (has_word(b, "Times") || has_word(b, "Roman") || has_word(b, "Serif") || has_word(b, "Georgia") ||
               has_word(b, "Garamond") || has_word(b, "Cambria") || has_word(b, "Palatino") || has_word(b, "Book") ||
               has_word(b, "Minion") || has_word(b, "CMR") || has_word(b, "Century") ||
               ((flags & 2) && !has_word(b, "Sans") && !has_word(b, "Arial") && !has_word(b, "Helvetica"))) {
        fam = 1;
    }

    if (has_word(b, "Bold") || has_word(b, "Black") || has_word(b, "Semibold") || has_word(b, "Heavy") ||
        (flags & (1 << 18)) || pd_getnum(f->doc, desc, "FontWeight", 400) >= 600) {
        style |= 1;
    }

    if (has_word(b, "Italic") || has_word(b, "Oblique") || (flags & 64)) {
        style |= 2;
    }

    return names[fam][style];
}
