/*
 * Parade complex shaping through HarfBuzz (optional)
 *
 * Built with -DPD_WITH_HARFBUZZ (make HARFBUZZ=1), words that contain
 * characters needing contextual shaping (Arabic joining, Indic reordering
 * and conjuncts, marks, emoji sequences...) are shaped by HarfBuzz with
 * the whole paragraph as context. Without it, these functions report
 * that shaping is unavailable and Parade's own 1:1 shaper is used.
 * Latin, Greek, Cyrillic and CJK always use Parade's shaper, so their
 * layout does not depend on whether HarfBuzz is present.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_unidata.h"

#ifdef PD_WITH_HARFBUZZ
    #include <hb.h>
#endif

int pd_shape_needed(uint32_t cp) {
    int g = pd_uni_gcb(cp);

    if (g == GCB_EXTEND || g == GCB_ZWJ || g == GCB_SPACINGMARK || g == GCB_PREPEND) {
        return 1;   /* marks, joiners, Indic vowel signs */
    }

    return (cp >= 0x0590 && cp <= 0x08FF) ||  /* Hebrew points, Arabic, Syriac, Thaana, NKo... */
           (cp >= 0x0900 && cp <= 0x0DFF) ||  /* Indic */
           (cp >= 0x0E00 && cp <= 0x0FFF) ||  /* Thai, Lao, Tibetan */
           (cp >= 0x1000 && cp <= 0x109F) ||  /* Myanmar */
           (cp >= 0x1780 && cp <= 0x18AF) ||  /* Khmer, Mongolian */
           (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF) ||   /* presentation forms */
           (cp >= 0x1F000);                   /* emoji and beyond */
}

#ifdef PD_WITH_HARFBUZZ

int pd_shape_available(void) {
    return 1;
}

void pd_shape_font_init(pd_font* f) {
    hb_blob_t* blob = hb_blob_create((const char*)f->data, (unsigned)f->len, HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t* face = hb_face_create(blob, (unsigned)f->face_index);
    hb_font_t* font = hb_font_create(face);

    hb_font_set_scale(font, f->m.units_per_em, f->m.units_per_em);    /* positions in font units */
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    f->hb_font = font;
}

void pd_shape_font_free(pd_font* f) {
    if (f->hb_font) {
        hb_font_destroy((hb_font_t*)f->hb_font);
        f->hb_font = NULL;
    }
}

int32_t pd_shape(const pd_font* f, const char* text, uint32_t len, uint32_t from, uint32_t to, pd_shaped** out) {
    hb_buffer_t* buf;
    hb_glyph_info_t* info;
    hb_glyph_position_t* pos;
    unsigned n, i;
    int rtl;

    *out = NULL;

    if (!f->hb_font || to <= from) {
        return -1;
    }

    buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text, (int)len, from, (int)(to - from));
    hb_buffer_set_cluster_level(buf, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
    hb_buffer_guess_segment_properties(buf);
    rtl = hb_buffer_get_direction(buf) == HB_DIRECTION_RTL;
    hb_shape((hb_font_t*)f->hb_font, buf, NULL, 0);
    info = hb_buffer_get_glyph_infos(buf, &n);
    pos = hb_buffer_get_glyph_positions(buf, &n);
    *out = (pd_shaped*)malloc((n ? n : 1) * sizeof(pd_shaped));

    if (!*out) {
        hb_buffer_destroy(buf);
        return -1;
    }

    for (i = 0; i < n; i++) {   /* right-to-left output is visual: store it in logical order */
        unsigned k = rtl ? n - 1 - i : i;

        (*out)[i].glyph = info[k].codepoint;
        (*out)[i].cluster = info[k].cluster;
        (*out)[i].advance = pos[k].x_advance;
        (*out)[i].xoff = pos[k].x_offset;
        (*out)[i].yoff = pos[k].y_offset;
    }

    hb_buffer_destroy(buf);
    return (int32_t)n;
}

#else

/*
 * Parade's own shaping, without HarfBuzz: Arabic joining. Each letter takes the form its neighbours give it
 * (isolated, initial, medial, final: ArabicShaping.txt's joining types, marks transparent), from the font's GSUB
 * features isol/init/medi/fina, else the presentation forms its cmap has; lam with alef the font's ligature (rlig,
 * else the presentation forms); marks over the middle of the letter before them. Other scripts: the 1:1 shaper.
 */

#define TAG4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

enum { JT_U, JT_R, JT_D, JT_C, JT_T };

static int is_arabic(uint32_t cp) {
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08FF);
}

/* a character's joining type (the common letters; the rest of the blocks dual-joining, marks transparent) */
static int joining(uint32_t cp) {
    static const struct {
        uint16_t a, b;
        uint8_t t;
    } R[] = {
        { 0x0621, 0x0621, JT_U }, { 0x0622, 0x0625, JT_R }, { 0x0626, 0x0626, JT_D }, { 0x0627, 0x0627, JT_R },
        { 0x0628, 0x0628, JT_D }, { 0x0629, 0x0629, JT_R }, { 0x062A, 0x062E, JT_D }, { 0x062F, 0x0632, JT_R },
        { 0x0633, 0x063F, JT_D }, { 0x0640, 0x0640, JT_C }, { 0x0641, 0x0647, JT_D }, { 0x0648, 0x0648, JT_R },
        { 0x0649, 0x064A, JT_D }, { 0x064B, 0x065F, JT_T }, { 0x0660, 0x066D, JT_U }, { 0x066E, 0x066F, JT_D },
        { 0x0670, 0x0670, JT_T }, { 0x0671, 0x0673, JT_R }, { 0x0674, 0x0674, JT_U }, { 0x0675, 0x0677, JT_R },
        { 0x0678, 0x0687, JT_D }, { 0x0688, 0x0699, JT_R }, { 0x069A, 0x06BF, JT_D }, { 0x06C0, 0x06C0, JT_R },
        { 0x06C1, 0x06C2, JT_D }, { 0x06C3, 0x06CB, JT_R }, { 0x06CC, 0x06CC, JT_D }, { 0x06CD, 0x06CD, JT_R },
        { 0x06CE, 0x06CE, JT_D }, { 0x06CF, 0x06CF, JT_R }, { 0x06D0, 0x06D1, JT_D }, { 0x06D2, 0x06D3, JT_R },
        { 0x06D4, 0x06D4, JT_U }, { 0x06D5, 0x06D5, JT_R }, { 0x06D6, 0x06DC, JT_T }, { 0x06DD, 0x06DE, JT_U },
        { 0x06DF, 0x06E4, JT_T }, { 0x06E5, 0x06E6, JT_U }, { 0x06E7, 0x06E8, JT_T }, { 0x06E9, 0x06E9, JT_U },
        { 0x06EA, 0x06ED, JT_T }, { 0x06EE, 0x06EF, JT_R }, { 0x06F0, 0x06F9, JT_U }, { 0x06FA, 0x06FC, JT_D },
        { 0x06FD, 0x06FE, JT_U }, { 0x06FF, 0x06FF, JT_D }, { 0x0759, 0x075B, JT_R }, { 0x076B, 0x076C, JT_R },
        { 0x0771, 0x0771, JT_R }, { 0x0773, 0x0774, JT_R }, { 0x0778, 0x0779, JT_R }, { 0x08D3, 0x08FF, JT_T }
    };
    size_t i;

    if (cp == 0x200D) {
        return JT_C;
    }

    for (i = 0; i < sizeof(R) / sizeof(R[0]); i++) {
        if (cp >= R[i].a && cp <= R[i].b) {
            return R[i].t;
        }
    }

    return (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0x08A0 && cp <= 0x08C9) ? JT_D : JT_U;
}

/* the presentation form (Arabic Presentation Forms-B) of a basic letter: form 0 isolated, 1 final, 2 initial,
   3 medial; 0 when there is none */
static uint32_t pres_form(uint32_t cp, int form) {
    static const uint16_t base[] = {    /* U+0621 to U+064A: the isolated form, 0 none (dual-joining: 4 forms) */
        0xFE80, 0xFE81, 0xFE83, 0xFE85, 0xFE87, 0xFE89, 0xFE8D, 0xFE8F, 0xFE93, 0xFE95, 0xFE99, 0xFE9D, 0xFEA1,
        0xFEA5, 0xFEA9, 0xFEAB, 0xFEAD, 0xFEAF, 0xFEB1, 0xFEB5, 0xFEB9, 0xFEBD, 0xFEC1, 0xFEC5, 0xFEC9, 0xFECD, 0, 0,
        0, 0, 0, 0, 0xFED1, 0xFED5, 0xFED9, 0xFEDD, 0xFEE1, 0xFEE5, 0xFEE9, 0xFEED, 0xFEEF, 0xFEF1
    };
    uint32_t b;
    int jt = joining(cp), n;

    if (cp < 0x0621 || cp > 0x064A || !(b = base[cp - 0x0621])) {
        return 0;
    }

    n = cp == 0x0621 ? 1 : jt == JT_D ? 4 : 2;
    return form < n ? b + (uint32_t)form : 0;
}

int pd_shape_available(void) {
    return 1;
}

void pd_shape_font_init(pd_font* f) {
    f->hb_font = NULL;
}

void pd_shape_font_free(pd_font* f) {
    (void)f;
}

static uint32_t next_cp(const char* s, uint32_t n, uint32_t* i) {
    const unsigned char* u = (const unsigned char*)s;
    uint32_t c = u[*i], k, extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ?
                                   3 : 0;

    if (extra == 0 || *i + extra >= n) {
        (*i)++;
        return c < 0x80 ? c : 0xFFFD;
    }

    c &= 0x3F >> extra;

    for (k = 1; k <= extra; k++) {
        c = (c << 6) | (u[*i + k] & 0x3F);
    }

    *i += extra + 1;
    return c;
}

int32_t pd_shape(const pd_font* f, const char* text, uint32_t len, uint32_t from, uint32_t to, pd_shaped** out) {
    static const uint32_t feat[4] = { TAG4('i', 's', 'o', 'l'), TAG4('f', 'i', 'n', 'a'), TAG4('i', 'n', 'i', 't'),
                                      TAG4('m', 'e', 'd', 'i')
                                    };
    const uint32_t arab = TAG4('a', 'r', 'a', 'b');
    uint32_t* cp, *at, i;
    int32_t n = 0, m = 0, any = 0, j, *form;
    int gsub;

    *out = NULL;

    if (!f || to <= from || to > len) {
        return -1;
    }

    cp = (uint32_t*)malloc(sizeof(uint32_t) * (to - from + 1));
    at = (uint32_t*)malloc(sizeof(uint32_t) * (to - from + 2));
    form = (int32_t*)malloc(sizeof(int32_t) * (to - from + 1));

    if (!cp || !at || !form) {
        free(cp);
        free(at);
        free(form);
        return -1;
    }

    for (i = from; i < to; n++) {
        at[n] = i;
        cp[n] = next_cp(text, to, &i);
        any |= is_arabic(cp[n]) && joining(cp[n]) != JT_U;
    }

    at[n] = to;

    if (!any) {     /* nothing to join: the 1:1 shaper */
        free(cp);
        free(at);
        free(form);
        return -1;
    }

    for (j = 0; j < n; j++) {   /* forms, by the joining types either side (marks skipped) */
        int t = joining(cp[j]), pj = 0, nj = 0;
        int32_t q;

        form[j] = -1;

        if (t != JT_R && t != JT_D) {
            continue;
        }

        for (q = j - 1; q >= 0 && joining(cp[q]) == JT_T; q--) {
        }

        pj = q >= 0 && (joining(cp[q]) == JT_D || joining(cp[q]) == JT_C);

        for (q = j + 1; q < n && joining(cp[q]) == JT_T; q++) {
        }

        nj = t == JT_D && q < n && (joining(cp[q]) == JT_D || joining(cp[q]) == JT_R || joining(cp[q]) == JT_C);
        form[j] = pj && nj ? 3 : pj ? 1 : nj ? 2 : 0;
    }

    *out = (pd_shaped*)malloc(sizeof(pd_shaped) * (size_t)(n + 1));

    if (!*out) {
        free(cp);
        free(at);
        free(form);
        return -1;
    }

    gsub = pd_font_gsub_has(f, arab, feat[1]);

    for (j = 0; j < n; j++) {
        uint32_t g = pd_font_glyph_index(f, cp[j]), pf;
        pd_shaped* o = &(*out)[m];

        if (form[j] >= 0) {
            if (cp[j] == 0x0644 && j + 1 < n && (cp[j + 1] == 0x0622 || cp[j + 1] == 0x0623 || cp[j + 1] == 0x0625 ||
                                                 cp[j + 1] == 0x0627)) {   /* lam with alef: one */
                uint32_t pair[2], lig = 0;
                int fin = form[j] == 1 || form[j] == 3;

                pair[0] = gsub ? pd_font_gsub_single(f, arab, feat[fin ? 3 : 2], g) : g;
                pair[1] = gsub ? pd_font_gsub_single(f, arab, feat[1], pd_font_glyph_index(f, cp[j + 1])) : 0;

                if (gsub && (pd_font_gsub_ligature(f, arab, TAG4('r', 'l', 'i', 'g'), pair, 2, &lig) == 2 ||
                             pd_font_gsub_ligature(f, arab, TAG4('l', 'i', 'g', 'a'), pair, 2, &lig) == 2)) {
                    g = lig;
                } else if ((pf = pd_font_glyph_index(f, (cp[j + 1] == 0x0622 ? 0xFEF5 : cp[j + 1] == 0x0623 ? 0xFEF7 :
                                                       cp[j + 1] == 0x0625 ? 0xFEF9 : 0xFEFB) + (fin ? 1 : 0))) != 0) {
                    g = pf;
                } else {
                    pair[0] = 0;
                }

                if (pair[0]) {
                    o->glyph = g;
                    o->cluster = at[j];
                    o->advance = pd_font_glyph_advance(f, g);
                    o->xoff = o->yoff = 0;
                    m++;
                    j++;    /* the alef in it */
                    continue;
                }
            }

            if (gsub) {
                g = pd_font_gsub_single(f, arab, feat[form[j]], g);
            } else if ((pf = pres_form(cp[j], form[j])) != 0 && pd_font_glyph_index(f, pf)) {
                g = pd_font_glyph_index(f, pf);
            }
        }

        o->glyph = g;
        o->cluster = at[j];
        o->advance = pd_font_glyph_advance(f, g);
        o->xoff = o->yoff = 0;

        if (joining(cp[j]) == JT_T && m > 0) {  /* a mark: no room of its own, over the middle of its letter */
            const pd_shaped* b = &(*out)[m - 1];
            int32_t x0, x1;

            o->advance = 0;
            o->xoff = b->advance / 2 - (pd_font_glyph_xrange(f, g, &x0, &x1) ? (x0 + x1) / 2 :
                                        pd_font_glyph_advance(f, g) / 2);
        }

        m++;
    }

    free(cp);
    free(at);
    free(form);
    return m;
}

#endif
