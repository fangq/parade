/* fuzz target: the TeX math parser and OpenType MATH layout */
#include <stdint.h>
#include <stdlib.h>
#include "fuzz_common.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const pd_font* f = fuzz_math_font();
    pd_math_item* items;
    pd_math_metrics m;
    int32_t n = 0;

    if (!f || size < 1) {
        return 0;
    }

    if (pd_math_layout(f, PD_PT(10), (const char*)data + 1, size - 1, data[0] & 1, NULL, 0, &n, &m) != PD_OK) {
        return 0;
    }

    items = n > 0 ? (pd_math_item*)malloc(sizeof(pd_math_item) * (size_t)n) : NULL;

    if (items) {
        pd_math_layout(f, PD_PT(10), (const char*)data + 1, size - 1, data[0] & 1, items, n, &n, &m);
        free(items);
    }

    return 0;
}
