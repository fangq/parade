/* fuzz target: Unicode segmentation, shaping, bidi, line breaking, hit testing and carets */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "fuzz_common.h"

/* the first 4 bytes are break parameters, the rest is UTF-8 (possibly invalid) */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const char* text;
    size_t len, off;
    uint8_t* brk;
    pd_para* p = NULL;
    pd_params par;
    pd_style st;
    pd_line ln;
    pd_glyph* g;
    int32_t i, n, line;
    uint32_t o, hit;
    pd_sp x, base;

    if (size < 4 || !fuzz_text_font()) {
        return 0;
    }

    text = (const char*)data + 4;
    len = size - 4;

    brk = (uint8_t*)malloc(len + 1);

    if (brk) {
        pd_text_line_breaks(text, len, brk);
        free(brk);
    }

    for (off = 0; off < len; off = pd_text_next_grapheme(text, len, off)) {
        if (pd_text_next_grapheme(text, len, off) <= off) {
            break;
        }
    }

    for (off = len; off > 0; off = pd_text_prev_grapheme(text, len, off)) {
        if (pd_text_prev_grapheme(text, len, off) >= off) {
            break;
        }
    }

    pd_params_init(&par);
    par.mode = data[0] & 1;
    par.align = (data[0] >> 1) & 3;
    par.direction = (data[0] >> 3) % 3;
    par.protrusion = (data[0] >> 5) & 1;
    par.expansion = (data[0] & 0x40) ? 20 : 0;
    par.tex_badness = data[0] >> 7;
    par.width = PD_PT(20 + data[1] * 2);
    par.indent = PD_PT(data[2] & 31);
    par.looseness = (int8_t)data[3] % 3;

    pd_style_init(&st, fuzz_text_font(), PD_PT(10));

    if (pd_para_new(&p) != PD_OK) {
        return 0;
    }

    if (pd_para_add_text(p, text, len, &st) == PD_OK && pd_para_break(p, &par, NULL) == PD_OK) {
        for (i = 0; i < pd_para_line_count(p); i++) {
            pd_para_get_line(p, i, &ln);
            n = 0;
            pd_para_get_glyphs(p, i, NULL, 0, &n);
            g = n > 0 ? (pd_glyph*)malloc(sizeof(pd_glyph) * (size_t)n) : NULL;

            if (g) {
                pd_para_get_glyphs(p, i, g, n, &n);
                free(g);
            }
        }

        for (o = 0; o <= pd_para_text_length(p); o += 1 + o / 8) {
            pd_para_caret(p, o, &line, &x, &base);
            pd_para_hit_test(p, x, base, &hit, &line);
        }

        /* reflow after an edit-like change of width, with hysteresis */
        par.width += PD_PT(7);
        par.hysteresis = 5000;
        pd_para_break(p, &par, NULL);
    }

    pd_para_free(p);
    return 0;
}
