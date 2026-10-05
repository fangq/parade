/* fuzz target: native document loading (JData text and BJData), layout, display lists, PDF and export */
#include <stdint.h>
#include <stdlib.h>
#include "parade_convert.h"
#include "parade_layout.h"
#include "fuzz_common.h"

static const pd_font* resolve(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;
    (void)family;
    (void)weight;
    (void)italic;
    return fuzz_text_font();
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    pd_draw* items;
    pd_pos pos;
    int32_t pg, n, page;
    pd_sp x, base, asc, desc;
    size_t bytes = 0;
    int fmt;

    /* malformed native data still reaches layout through the format-detecting importers */
    if (pd_doc_load(data, size, PD_JDATA_AUTO, &d) != PD_OK &&
            pd_doc_import(data, size, pd_conv_detect(data, size), &d) != PD_OK) {
        return 0;
    }

    for (fmt = PD_CONV_TEXT; fmt <= PD_CONV_JDATA; fmt++) {
        pd_doc_export(d, (pd_conv_format)fmt, fuzz_discard, &bytes);
    }

    pd_doc_save(d, PD_JDATA_BINARY, fuzz_discard, &bytes);

    if (fuzz_text_font()) {
        pd_doc_set_font_resolver(d, resolve, NULL);
        pd_doc_set_default_font(d, fuzz_text_font());
        pd_doc_set_microtype(d, size & 1, (size & 2) ? 20 : 0);

        if (fuzz_math_font()) {
            pd_doc_set_math_font(d, fuzz_math_font());
        }

        if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
            for (pg = 0; pg < pd_layout_page_count(L) && pg < 64; pg++) {
                n = 0;
                pd_layout_page_items(L, pg, NULL, 0, &n);
                items = n > 0 ? (pd_draw*)malloc(sizeof(pd_draw) * (size_t)n) : NULL;

                if (items) {
                    pd_layout_page_items(L, pg, items, n, &n);
                    free(items);
                }

                pd_layout_hit_test(L, pg, PD_PT(100), PD_PT(200), &pos);
            }

            pos.block = pd_doc_next_paragraph(d, 0);
            pos.offset = 0;

            if (pos.block) {
                pd_layout_caret(L, pos, &page, &x, &base, &asc, &desc);
            }

            pd_layout_write_pdf(L, NULL, fuzz_discard, &bytes);
        }

        pd_layout_free(L);
    }

    pd_doc_free(d);
    return 0;
}
