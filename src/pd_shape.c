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

int pd_shape_available(void) {
    return 0;
}

void pd_shape_font_init(pd_font* f) {
    f->hb_font = NULL;
}

void pd_shape_font_free(pd_font* f) {
    (void)f;
}

int32_t pd_shape(const pd_font* f, const char* text, uint32_t len, uint32_t from, uint32_t to, pd_shaped** out) {
    (void)f;
    (void)text;
    (void)len;
    (void)from;
    (void)to;
    *out = NULL;
    return -1;
}

#endif
