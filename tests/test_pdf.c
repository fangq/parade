/*
 * Parade PDF writer tests: structure, xref integrity, determinism,
 * TrueType subsets, Type 3 (CFF) fonts, PNG/JPEG images, bookmarks.
 * External viewers (Ghostscript, poppler) are checked by make pdf-check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_layout.h"

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static pd_font* latin, *cjk;

static const pd_font* resolver(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;
    (void)weight;
    (void)italic;
    return (family && strcmp(family, "cjk") == 0 && cjk) ? cjk : latin;
}

typedef struct {
    unsigned char* p;
    size_t n;
} buf_t;

static int sink(void* user, const void* data, size_t len) {
    buf_t* b = (buf_t*)user;
    unsigned char* q = (unsigned char*)realloc(b->p, b->n + len + 1);

    if (!q) {
        return 1;
    }

    b->p = q;
    memcpy(b->p + b->n, data, len);
    b->n += len;
    b->p[b->n] = '\0';
    return 0;
}

static int has(const buf_t* b, const char* s) {
    size_t n = strlen(s), i;

    for (i = 0; i + n <= b->n; i++) {
        if (memcmp(b->p + i, s, n) == 0) {
            return 1;
        }
    }

    return 0;
}

static unsigned char* slurp(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    unsigned char* d;
    long len;

    if (!fp || fseek(fp, 0, SEEK_END) || (len = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET)) {
        if (fp) {
            fclose(fp);
        }

        return NULL;
    }

    d = (unsigned char*)malloc((size_t)len + 1);

    if (d && fread(d, 1, (size_t)len, fp) != (size_t)len) {
        free(d);
        d = NULL;
    }

    fclose(fp);
    *n = (size_t)len;
    return d;
}

static pd_pos at(pd_block_id b, uint32_t o) {
    pd_pos p;
    p.block = b;
    p.offset = o;
    return p;
}

static void add_image(pd_doc* d, pd_block_id para, const char* path, const char* mime, pd_sp w, pd_sp h) {
    size_t n;
    unsigned char* data = slurp(path, &n);
    pd_res_id res;
    pd_inline o;
    const char* t;
    uint32_t len;

    if (!data) {
        fprintf(stderr, "missing %s\n", path);
        failures++;
        return;
    }

    pd_doc_add_resource(d, mime, data, n, &res);
    free(data);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.resource = res;
    o.width = w;
    o.height = h;
    pd_doc_para_text(d, para, &t, &len);
    pd_doc_insert_inline(d, at(para, len), &o, NULL);
}

/* every xref entry points at "N 0 obj" */
static int xref_ok(const buf_t* b) {
    const char* x = NULL;
    int first, count, i, ok = 1;
    size_t k;

    for (k = b->n; k >= 6 && !x; k--) {     /* binary-safe: streams contain NUL bytes */
        if (memcmp(b->p + k - 6, "\nxref\n", 6) == 0) {
            x = (const char*)b->p + k - 6;
        }
    }

    if (!x || sscanf(x + 6, "%d %d", &first, &count) != 2) {
        return 0;
    }

    x = strchr(x + 6, '\n') + 1;

    for (i = 0; i < count; i++, x += 20) {
        unsigned long long off;
        char kind;

        if (sscanf(x, "%10llu %*5d %c", &off, &kind) != 2) {
            return 0;
        }

        if (kind == 'n') {
            char want[32];

            snprintf(want, sizeof(want), "%d 0 obj", i);
            ok &= off < b->n && strncmp((const char*)b->p + off, want, strlen(want)) == 0;
        }
    }

    return ok;
}

int main(void) {
    pd_doc* d;
    pd_layout* L;
    pd_block_id sec, p, h, c;
    pd_pdf_options opt;
    buf_t a = { NULL, 0 }, b = { NULL, 0 }, u = { NULL, 0 };
    pd_char_props cp;
    int i;
    const char* text = "Parade writes PDF from the same display list the screen uses. ";

    if (pd_font_load_file("/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf", 0, &latin) != PD_OK) {
        printf("no test font\n");
        return 77;
    }

    if (pd_font_load_file("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0, &cjk) != PD_OK) {
        cjk = NULL;
    }

    pd_doc_new(&d);
    pd_doc_set_font_resolver(d, resolver, NULL);
    sec = pd_doc_child(d, pd_doc_root(d), 0);
    h = pd_doc_child(d, sec, 0);
    pd_doc_set_para_style(d, h, pd_doc_style_find(d, "Heading 1"));
    pd_doc_set_role(d, h, PD_ROLE_HEADING, 1);
    {
        const char* title = "PDF Output (\xC3\xA9t\xC3\xA9)";

        pd_doc_insert_text(d, at(h, 0), title, strlen(title), PD_FORMAT_INHERIT, NULL);
    }
    pd_doc_insert_block(d, sec, -1, PD_BLOCK_PARAGRAPH, &p);

    for (i = 0; i < 30; i++) {
        pd_doc_insert_text(d, at(p, (uint32_t)(i * strlen(text))), text, strlen(text), PD_FORMAT_INHERIT, NULL);
    }

    if (cjk) {
        pd_doc_insert_block(d, sec, -1, PD_BLOCK_PARAGRAPH, &c);
        {
            const char* han = "\xE4\xB8\xAD\xE6\x96\x87\xE6\xAE\xB5\xE8\x90\xBD\xEF\xBC\x8C"
                              "\xE7\x94\xA8 CFF \xE5\xAD\x97\xE4\xBD\x93\xE3\x80\x82";

            pd_doc_insert_text(d, at(c, 0), han, strlen(han), PD_FORMAT_INHERIT, NULL);
            memset(&cp, 0, sizeof(cp));
            cp.mask = PD_CP_FAMILY;
            strcpy(cp.family, "cjk");
            pd_doc_set_char_props(d, (pd_range) {
                at(c, 0), at(c, (uint32_t)strlen(han))
            }, &cp);
        }
    }

    pd_doc_insert_block(d, sec, -1, PD_BLOCK_PARAGRAPH, &p);
    add_image(d, p, "tests/data/rgba.png", "image/png", PD_PT(96), PD_PT(60));
    add_image(d, p, "tests/data/palette.png", "image/png", PD_PT(72), PD_PT(48));
    add_image(d, p, "tests/data/photo.jpg", "image/jpeg", PD_PT(90), PD_PT(54));

    pd_layout_new(d, &L);
    CHECK(pd_layout_update(L, NULL) == PD_OK);
    pd_pdf_options_init(&opt);
    strcpy(opt.author, "Parade (tests)");
    CHECK(pd_layout_write_pdf(L, &opt, sink, &a) == PD_OK);
    CHECK(pd_layout_write_pdf(L, &opt, sink, &b) == PD_OK);
    opt.compress = 0;
    CHECK(pd_layout_write_pdf(L, &opt, sink, &u) == PD_OK);

    CHECK(a.n > 1000 && memcmp(a.p, "%PDF-1.7", 8) == 0);
    CHECK(a.n == b.n && memcmp(a.p, b.p, a.n) == 0);   /* deterministic */
    CHECK(xref_ok(&a) && xref_ok(&u));
    CHECK(has(&u, "/FontFile2") && has(&u, "/CIDFontType2") && has(&u, "/ToUnicode"));
    CHECK(has(&u, "/SMask") && has(&u, "/DCTDecode"));
    CHECK(has(&u, "/Outlines") && has(&u, "/Author (Parade \\(tests\\))"));
    CHECK(has(&u, "<0050>"));   /* glyph ids as two-byte codes */

    if (cjk) {
        CHECK(has(&u, "/Subtype /Type3") && has(&u, " d0"));
    }

    printf("  PDF %zu bytes compressed, %zu uncompressed%s\n", a.n, u.n, cjk ? ", with a CFF (Type 3) font" : "");

    {
        FILE* fp = fopen("build/test_pdf.pdf", "wb");

        if (fp) {
            fwrite(a.p, 1, a.n, fp);
            fclose(fp);
        }
    }

    free(a.p);
    free(b.p);
    free(u.p);
    pd_layout_free(L);
    pd_doc_free(d);
    pd_font_free(latin);
    pd_font_free(cjk);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
