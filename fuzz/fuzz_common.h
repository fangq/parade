/* shared by the fuzz targets: fonts loaded once, a writer that discards */
#ifndef PARADE_FUZZ_COMMON_H
#define PARADE_FUZZ_COMMON_H

#include <stdlib.h>
#include "parade.h"

#define FUZZ_TEXT_FONT "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf"
#define FUZZ_MATH_FONT "/usr/share/texmf/fonts/opentype/public/lm-math/latinmodern-math.otf"

/* PARADE_FUZZ_FONT / PARADE_FUZZ_MATH_FONT override the defaults; NULL if missing */
static inline const pd_font* fuzz_font(const char* env, const char* fallback) {
    const char* path = getenv(env);
    pd_font* f = NULL;

    if (pd_font_load_file(path ? path : fallback, 0, &f) != PD_OK) {
        return NULL;
    }

    return f;
}

static inline const pd_font* fuzz_text_font(void) {
    static int done;
    static const pd_font* f;

    if (!done) {
        done = 1;
        f = fuzz_font("PARADE_FUZZ_FONT", FUZZ_TEXT_FONT);
    }

    return f;
}

static inline const pd_font* fuzz_math_font(void) {
    static int done;
    static const pd_font* f;

    if (!done) {
        done = 1;
        f = fuzz_font("PARADE_FUZZ_MATH_FONT", FUZZ_MATH_FONT);
    }

    return f;
}

static inline int fuzz_discard(void* user, const void* data, size_t len) {
    (void)data;
    *(size_t*)user += len;
    return 0;
}

#endif
