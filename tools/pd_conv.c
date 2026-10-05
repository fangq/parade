/*
 * pd_conv: convert between document formats with Parade
 *
 *   pd_conv in.{pdoc,bpdoc,html,md,rtf,docx,txt} out.{pdoc,bpdoc,html,md,tex,rtf,docx,txt,pdf}
 *
 * Formats come from the file extensions (or, for input, the content).
 * PDF output lays the document out with Parade itself (Liberation Serif, and
 * Liberation Sans for sans-serif families such as Arial; DejaVu Sans Mono for
 * code, Noto CJK as a fallback when installed).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_convert.h"
#include "parade_layout.h"

#define NFONTS 10                   /* serif x4, sans x4, mono, CJK */
static pd_font* fonts[NFONTS];
static pd_font* mathf;

static const pd_font* resolve(void* user, const char* family, int32_t weight, int32_t italic) {
    int32_t cls = pd_font_family_class(family);
    int face = (weight >= 600 ? 1 : 0) + (italic ? 2 : 0);

    (void)user;

    if (cls == PD_FAMILY_MONO) {
        return fonts[8];
    }

    return cls == PD_FAMILY_SANS && fonts[4 + face] ? fonts[4 + face] : fonts[face];
}

static int to_file(void* user, const void* data, size_t len) {
    return fwrite(data, 1, len, (FILE*)user) != len;
}

static const char* ext_of(const char* path) {
    const char* e = strrchr(path, '.');
    return e ? e + 1 : "";
}

static int fmt_of(const char* ext, int* jdata_binary) {
    *jdata_binary = 0;

    if (!strcmp(ext, "html") || !strcmp(ext, "htm")) {
        return PD_CONV_HTML;
    }

    if (!strcmp(ext, "md") || !strcmp(ext, "markdown")) {
        return PD_CONV_MARKDOWN;
    }

    if (!strcmp(ext, "tex")) {
        return PD_CONV_LATEX;
    }

    if (!strcmp(ext, "rtf")) {
        return PD_CONV_RTF;
    }

    if (!strcmp(ext, "docx")) {
        return PD_CONV_DOCX;
    }

    if (!strcmp(ext, "txt")) {
        return PD_CONV_TEXT;
    }

    if (!strcmp(ext, "pdoc") || !strcmp(ext, "bpdoc")) {
        *jdata_binary = !strcmp(ext, "bpdoc");
        return PD_CONV_JDATA;
    }

    return -1;
}

int main(int argc, char** argv) {
    static const char* paths[NFONTS] = {
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-Italic.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSerif-BoldItalic.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Italic.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-BoldItalic.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
    };
    FILE* f;
    char* data;
    long n;
    pd_doc* d = NULL;
    pd_status st;
    int in_bin, out_bin, in_fmt, out_fmt, i;
    const char* oext;

    if (argc != 3) {
        fprintf(stderr, "usage: pd_conv input output\n");
        return 2;
    }

    if ((f = fopen(argv[1], "rb")) == NULL || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    data = (char*)malloc((size_t)n + 1);

    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    fclose(f);
    in_fmt = fmt_of(ext_of(argv[1]), &in_bin);

    if (in_fmt < 0) {
        in_fmt = pd_conv_detect(data, (size_t)n);
    }

    st = in_fmt == PD_CONV_JDATA ? pd_doc_load(data, (size_t)n, PD_JDATA_AUTO, &d) :
         pd_doc_import(data, (size_t)n, (pd_conv_format)in_fmt, &d);
    free(data);

    if (st != PD_OK) {
        fprintf(stderr, "cannot import %s: %s\n", argv[1], pd_status_string(st));
        return 1;
    }

    if ((f = fopen(argv[2], "wb")) == NULL) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }

    oext = ext_of(argv[2]);

    if (!strcmp(oext, "pdf")) {
        pd_layout* L;
        pd_layout_info info;
        const pd_font* fb[1];

        for (i = 0; i < NFONTS; i++) {     /* the sans faces and CJK are optional */
            if (pd_font_load_file(paths[i], 0, &fonts[i]) != PD_OK && (i < 4 || i == 8)) {
                fprintf(stderr, "cannot load %s\n", paths[i]);
                return 1;
            }
        }

        pd_doc_set_font_resolver(d, resolve, NULL);

        if (pd_font_load_file("/usr/share/texmf/fonts/opentype/public/lm-math/latinmodern-math.otf", 0, &mathf) == PD_OK) {
            pd_doc_set_math_font(d, mathf);
        }

        if (fonts[9]) {
            fb[0] = fonts[9];
            pd_doc_set_fallback_fonts(d, fb, 1);
        }

        pd_layout_new(d, &L);
        st = pd_layout_update(L, &info);

        if (st == PD_OK) {
            st = pd_layout_write_pdf(L, NULL, to_file, f);
            fprintf(stderr, "%d pages\n", (int)info.pages);
        }

        pd_layout_free(L);
    } else if ((out_fmt = fmt_of(oext, &out_bin)) >= 0) {
        st = out_fmt == PD_CONV_JDATA ? pd_doc_save(d, out_bin ? PD_JDATA_BINARY : PD_JDATA_TEXT, to_file, f) :
             pd_doc_export(d, (pd_conv_format)out_fmt, to_file, f);
    } else {
        fprintf(stderr, "unknown output format .%s\n", oext);
        st = PD_ERR_ARG;
    }

    fclose(f);
    pd_doc_free(d);

    for (i = 0; i < NFONTS; i++) {
        pd_font_free(fonts[i]);
    }

    pd_font_free(mathf);

    if (st != PD_OK) {
        fprintf(stderr, "conversion failed: %s\n", pd_status_string(st));
        return 1;
    }

    return 0;
}
