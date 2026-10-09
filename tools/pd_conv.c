/*
 * pd_conv: convert between document formats with Parade
 *
 *   pd_conv in.{pdoc,jdoc,html,md,rtf,docx,pptx,txt} out.{pdoc,jdoc,html,md,tex,rtf,docx,txt,pdf}
 *   (.pdoc: the native document in BJData; .jdoc: the same in JSON text)
 *
 * Formats come from the file extensions (or, for input, the content).
 * PDF output lays the document out with Parade itself (Liberation Serif, and
 * Liberation Sans for sans-serif families such as Arial, its narrow faces for
 * Arial Narrow and the like; Carlito and Caladea for Calibri and Cambria,
 * whose metrics they share; DejaVu Sans Mono for code, Noto CJK and DejaVu Sans as fallbacks
 * when installed).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_convert.h"
#include "parade_layout.h"

#define NFONTS 23                   /* serif x4, sans x4, mono, CJK, narrow sans x4, Carlito x4, Caladea x4, DejaVu Sans */
static pd_font* fonts[NFONTS];
static pd_font* mathf;

/* the fonts the document carries (a DOCX's embedded ones): family, weight, italic */
static struct {
    char family[64];
    int weight, italic;
    pd_font* font;
} embedded[64];
static int nembedded;

static void load_embedded(const pd_doc* d) {
    pd_res_id r;

    for (r = 1; nembedded < 64; r++) {
        const char* mime;
        const void* data;
        size_t len;
        const char* f;

        if (pd_doc_resource(d, r, &mime, &data, &len) != PD_OK) {
            break;
        }

        if (strncmp(mime, "font/", 5) != 0 || (f = strstr(mime, "family=\"")) == NULL ||
                pd_font_load_memory(data, len, 0, &embedded[nembedded].font) != PD_OK) {
            continue;
        }

        snprintf(embedded[nembedded].family, sizeof(embedded[0].family), "%.*s", (int)strcspn(f + 8, "\""), f + 8);
        embedded[nembedded].weight = strstr(mime, "weight=700") ? 700 : 400;
        embedded[nembedded].italic = strstr(mime, "italic=1") != NULL;
        nembedded++;
    }
}

/* name is family, or one of the names the document's font table says it also goes by */
static int named(const pd_doc* d, const char* family, const char* name) {
    pd_font_info fi;
    const char* p;
    size_t n = strlen(name);

    if (!strcmp(family, name)) {
        return 1;
    }

    if (!d || pd_doc_font_info(d, family, &fi) != PD_OK) {
        return 0;
    }

    for (p = fi.alt; (p = strstr(p, name)) != NULL; p += n) {
        if ((p == fi.alt || p[-1] == ',') && (p[n] == ',' || !p[n])) {
            return 1;
        }
    }

    return 0;
}

static const pd_font* resolve(void* user, const char* family, int32_t weight, int32_t italic) {
    const pd_doc* d = (const pd_doc*)user;
    int32_t cls = pd_doc_font_class(d, family);
    int face = (weight >= 600 ? 1 : 0) + (italic ? 2 : 0), i, best = -1, bscore = -1;

    for (i = 0; family && i < nembedded; i++) {     /* the document's own font, the closest face */
        if (named(d, family, embedded[i].family)) {
            int score = ((weight >= 600) == (embedded[i].weight >= 600)) * 2 + (!italic == !embedded[i].italic);

            if (score > bscore) {
                bscore = score;
                best = i;
            }
        }
    }

    if (best >= 0) {
        return embedded[best].font;
    }

    if (cls == PD_FAMILY_MONO) {
        return fonts[8];
    }

    /* Calibri and Cambria: the faces with their metrics, so lines break where they did in Word */
    if (family && !strncmp(family, "Calibri", 7) && fonts[14 + face]) {
        return fonts[14 + face];
    }

    if (family && !strncmp(family, "Cambria", 7) && strcmp(family, "Cambria Math") && fonts[18 + face]) {
        return fonts[18 + face];
    }

    if (cls == PD_FAMILY_SANS && family && (strstr(family, "Narrow") || strstr(family, "Condensed")) &&
            fonts[10 + face]) {
        return fonts[10 + face];    /* Arial Narrow and the like: a narrow face, so lines break where they did */
    }

    return cls == PD_FAMILY_SANS && fonts[4 + face] ? fonts[4 + face] : fonts[face];
}

/* a picture the document names, from beside the input file (user: its folder, with the separator) */
static int fetch_file(void* user, const char* address, pd_writer write, void* sink) {
    char path[4096];
    FILE* f;
    char buf[65536];
    size_t n;
    int rc = 0;

    if (strstr(address, "://") || strncmp(address, "data:", 5) == 0) {
        return 1;   /* nothing from the network */
    }

    snprintf(path, sizeof(path), "%s%s", address[0] == '/' ? "" : (const char*)user, address);

    if ((f = fopen(path, "rb")) == NULL) {
        return 1;
    }

    while (!rc && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
        rc = write(sink, buf, n);
    }

    fclose(f);
    return rc;
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

    if (!strcmp(ext, "pptx")) {
        return PD_CONV_PPTX;
    }

    if (!strcmp(ext, "txt")) {
        return PD_CONV_TEXT;
    }

    if (!strcmp(ext, "pdoc") || !strcmp(ext, "jdoc")) {
        *jdata_binary = !strcmp(ext, "pdoc");
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
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/liberation/LiberationSansNarrow-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSansNarrow-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSansNarrow-Italic.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSansNarrow-BoldItalic.ttf",
        "/usr/share/fonts/truetype/crosextra/Carlito-Regular.ttf",
        "/usr/share/fonts/truetype/crosextra/Carlito-Bold.ttf",
        "/usr/share/fonts/truetype/crosextra/Carlito-Italic.ttf",
        "/usr/share/fonts/truetype/crosextra/Carlito-BoldItalic.ttf",
        "/usr/share/fonts/truetype/crosextra/Caladea-Regular.ttf",
        "/usr/share/fonts/truetype/crosextra/Caladea-Bold.ttf",
        "/usr/share/fonts/truetype/crosextra/Caladea-Italic.ttf",
        "/usr/share/fonts/truetype/crosextra/Caladea-BoldItalic.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
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

    {   /* pictures the input only names, from its folder */
        char dir[4096];
        const char* sl = strrchr(argv[1], '/');

        snprintf(dir, sizeof(dir), "%.*s", sl ? (int)(sl - argv[1] + 1) : 0, argv[1]);
        pd_doc_load_images(d, fetch_file, dir);
    }

    if ((f = fopen(argv[2], "wb")) == NULL) {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }

    oext = ext_of(argv[2]);

    if (!strcmp(oext, "pdf")) {
        pd_layout* L;
        pd_layout_info info;
        const pd_font* fb[2];
        int nfb = 0;

        for (i = 0; i < NFONTS; i++) {     /* all but the serif faces and the mono face are optional */
            if (pd_font_load_file(paths[i], 0, &fonts[i]) != PD_OK && (i < 4 || i == 8)) {
                fprintf(stderr, "cannot load %s\n", paths[i]);
                return 1;
            }
        }

        load_embedded(d);
        pd_doc_set_font_resolver(d, resolve, d);

        if (pd_font_load_file("/usr/share/texmf/fonts/opentype/public/lm-math/latinmodern-math.otf", 0, &mathf) == PD_OK) {
            pd_doc_set_math_font(d, mathf);
        }

        if (fonts[9]) {     /* CJK, then the scripts DejaVu Sans has (Arabic, Hebrew, Greek, Cyrillic, symbols) */
            fb[nfb++] = fonts[9];
        }

        if (fonts[22]) {
            fb[nfb++] = fonts[22];
        }

        if (nfb) {
            pd_doc_set_fallback_fonts(d, fb, nfb);
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

    for (i = 0; i < nembedded; i++) {
        pd_font_free(embedded[i].font);
    }

    pd_font_free(mathf);

    if (st != PD_OK) {
        fprintf(stderr, "conversion failed: %s\n", pd_status_string(st));
        return 1;
    }

    return 0;
}
