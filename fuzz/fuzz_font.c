/* fuzz target: font loading, metrics, outlines, rasterizing, math layout */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "parade.h"

static void nop2(void* u, int32_t x, int32_t y) {
    (void)u;
    (void)x;
    (void)y;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    pd_font* f = NULL;
    pd_font_metrics m;
    uint32_t g, cp;
    unsigned char bmp[64 * 64];
    pd_glyph_image gi;
    pd_outline_sink sk;

    if (pd_font_load_memory(data, size, 0, &f) != PD_OK) {
        return 0;
    }

    memset(&sk, 0, sizeof(sk));
    sk.move_to = nop2;
    sk.line_to = nop2;
    pd_font_get_metrics(f, &m);

    for (cp = 32; cp < 128; cp += 7) {
        g = pd_font_glyph_index(f, cp);
        pd_font_glyph_advance(f, g);
        pd_font_kerning(f, g, pd_font_glyph_index(f, cp + 1));
        pd_font_glyph_outline(f, g, &sk, NULL);
        pd_font_glyph_render(f, g, 16 << 16, (int32_t)(cp & 255), bmp, (int32_t)sizeof(bmp), &gi);
    }

    for (g = 0; g < 64 && (int32_t)g < m.num_glyphs; g++) {
        pd_font_glyph_outline(f, g, &sk, NULL);
    }

    if (pd_font_has_math(f)) {
        static const char* tex = "\\left( \\sum_{i=1}^n \\frac{a_i}{\\sqrt[3]{b}} \\right) \\hat{x}";
        int32_t n;

        pd_math_layout(f, PD_PT(10), tex, strlen(tex), 1, NULL, 0, &n, NULL);
    }

    pd_font_free(f);
    return 0;
}
