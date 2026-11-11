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

int pd_inflate(const void* data, size_t n, int zlib, size_t limit, unsigned char** out, size_t* outlen);

/* a picture in a format the PDF writer decodes itself (BMP, TIFF uncompressed, PackBits, LZW, Deflate, with a
   palette): its pixels, exactly, in the PDF's image -- as they are, or in its stream inflated (an image stream is
   deflated whenever that makes it smaller) */
static void check_decoded(const char* path, const char* mime, const unsigned char* px, size_t npx, int w, int h) {
    pd_doc* d;
    pd_layout* L = NULL;
    pd_pdf_options opt;
    buf_t u = { NULL, 0 };
    char dict[64];

    pd_doc_new(&d);
    pd_doc_set_font_resolver(d, resolver, NULL);
    add_image(d, pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0), path, mime, PD_PT(72), PD_PT(48));
    pd_layout_new(d, &L);
    pd_layout_update(L, NULL);
    pd_pdf_options_init(&opt);
    opt.compress = 0;
    CHECK(pd_layout_write_pdf(L, &opt, sink, &u) == PD_OK);
    snprintf(dict, sizeof(dict), "/Width %d /Height %d /ColorSpace /DeviceRGB", w, h);
    CHECK(has(&u, dict));

    {
        size_t i;
        int found = 0;
        const char* at = NULL;

        for (i = 0; i + npx <= u.n && !found; i++) {
            found = memcmp(u.p + i, px, npx) == 0;
        }

        for (i = 0; i + strlen(dict) <= u.n && !at; i++) {     /* or in the image's stream, deflated */
            if (!memcmp(u.p + i, dict, strlen(dict))) {
                at = (const char*)u.p + i;
            }
        }

        if (!found && at) {
            const char* len = strstr(at, "/Length "), *st = strstr(at, "stream\n"), *fl = strstr(at, "/FlateDecode");
            unsigned char* raw = NULL;
            size_t rn = 0, zn = len ? strtoul(len + 8, NULL, 10) : 0;

            if (len && st && fl && fl < st && (size_t)(st + 7 - (const char*)u.p) + zn <= u.n &&
                    pd_inflate(st + 7, zn, 1, 0, &raw, &rn) == 0) {
                found = rn == npx && memcmp(raw, px, npx) == 0;
            }

            free(raw);
        }

        if (!found) {
            fprintf(stderr, "  %s: its pixels are not in the PDF\n", path);
        }

        CHECK(found);
    }

    free(u.p);
    pd_layout_free(L);
    pd_doc_free(d);
}

static void test_decoded_images(void) {
    static const unsigned char px[18] = { 0x12, 0x34, 0x56, 0xAB, 0xCD, 0xEF, 0xF0, 0x0F, 0x80, 0x01, 0x80, 0xFE,
                                          0x99, 0x11, 0x22, 0x33, 0xEE, 0x77
                                        };
    unsigned char* big = (unsigned char*)malloc(70 * 60 * 3);
    int x, y;

    for (y = 0; y < 60; y++) {  /* what tests/data/big_lzw.tif holds: enough to fill LZW's table past 9 bits */
        for (x = 0; x < 70; x++) {
            big[(y * 70 + x) * 3] = (unsigned char)((x * 7) & 255);
            big[(y * 70 + x) * 3 + 1] = (unsigned char)((y * 5) & 255);
            big[(y * 70 + x) * 3 + 2] = (unsigned char)((x * y + x / 3) & 255);
        }
    }

    check_decoded("tests/data/px24.bmp", "image/bmp", px, 18, 3, 2);
    check_decoded("tests/data/px_lzw.tif", "image/tiff", px, 18, 3, 2);
    check_decoded("tests/data/px_pack.tif", "image/tiff", px, 18, 3, 2);
    check_decoded("tests/data/px_zip.tif", "image/tiff", px, 18, 3, 2);
    check_decoded("tests/data/px_pal.tif", "image/tiff", px, 18, 3, 2);
    check_decoded("tests/data/big_lzw.tif", "image/tiff", big, 70 * 60 * 3, 70, 60);
    free(big);
}

/* a 24-bit BMP of w by h: a photograph's noise over a gradient (photo), else three flat colours (a drawing) */
static void write_bmp(const char* path, int w, int h, int photo) {
    FILE* fp = fopen(path, "wb");
    int stride = (w * 3 + 3) & ~3, x, y;
    uint32_t size = 54 + (uint32_t)stride * (uint32_t)h, seed = 12345;
    unsigned char hd[54];

    if (!fp) {
        return;
    }

    memset(hd, 0, sizeof(hd));
    hd[0] = 'B';
    hd[1] = 'M';
    memcpy(hd + 2, &size, 4);
    hd[10] = 54;
    hd[14] = 40;
    memcpy(hd + 18, &w, 4);
    memcpy(hd + 22, &h, 4);
    hd[26] = 1;
    hd[28] = 24;
    fwrite(hd, 1, 54, fp);

    for (y = 0; y < h; y++) {
        unsigned char row[4096];

        memset(row, 0, sizeof(row));

        for (x = 0; x < w; x++) {
            int v[3], k;

            for (k = 0; k < 3; k++) {
                seed = seed * 1103515245u + 12345u;

                if (photo) {
                    v[k] = (x * (k + 1) * 255 / w + y * 255 / h) / 2 + (int)((seed >> 16) % 61) - 30;
                } else {
                    v[k] = x < w / 3 ? 40 * k : x < 2 * w / 3 ? 200 - 50 * k : 250;
                }

                row[x * 3 + 2 - k] = (unsigned char)(v[k] < 0 ? 0 : v[k] > 255 ? 255 : v[k]);
            }
        }

        fwrite(row, 1, (size_t)stride, fp);
    }

    fclose(fp);
}

static size_t count(const buf_t* b, const char* s) {
    size_t n = strlen(s), i, c = 0;

    for (i = 0; i + n <= b->n; i++) {
        c += memcmp(b->p + i, s, n) == 0;
    }

    return c;
}

/* jpeg_quality: a photograph written as a JPEG, much smaller; a drawing and a JPEG as they are */
static void test_jpeg_option(void) {
    pd_doc* d;
    pd_layout* L = NULL;
    pd_pdf_options opt;
    buf_t lossless = { NULL, 0 }, lossy = { NULL, 0 };
    pd_block_id p;

    write_bmp("build/test_photo.bmp", 300, 200, 1);
    write_bmp("build/test_drawing.bmp", 300, 200, 0);
    pd_doc_new(&d);
    pd_doc_set_font_resolver(d, resolver, NULL);
    p = pd_doc_child(d, pd_doc_child(d, pd_doc_root(d), 0), 0);
    add_image(d, p, "build/test_photo.bmp", "image/bmp", PD_PT(150), PD_PT(100));
    add_image(d, p, "build/test_drawing.bmp", "image/bmp", PD_PT(150), PD_PT(100));
    add_image(d, p, "tests/data/photo.jpg", "image/jpeg", PD_PT(90), PD_PT(54));
    pd_layout_new(d, &L);
    pd_layout_update(L, NULL);
    pd_pdf_options_init(&opt);
    CHECK(opt.jpeg_quality == 0);
    CHECK(pd_layout_write_pdf(L, &opt, sink, &lossless) == PD_OK);
    opt.jpeg_quality = 85;
    CHECK(pd_layout_write_pdf(L, &opt, sink, &lossy) == PD_OK);
    CHECK(xref_ok(&lossless) && xref_ok(&lossy));
    CHECK(count(&lossless, "/DCTDecode") == 1);     /* the JPEG only */
    CHECK(count(&lossy, "/DCTDecode") == 2);        /* and the photograph; not the drawing */
    CHECK(has(&lossy, "/Width 300 /Height 200 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode"));
    /* (deflated: its dictionary, then its length) both lossless, the drawing only when the photograph is a JPEG */
    CHECK(count(&lossless, "/Width 300 /Height 200 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length") == 2);
    CHECK(count(&lossy, "/Width 300 /Height 200 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Length") == 1);
    CHECK(has(&lossy, "\xFF\xD8\xFF\xDB") && has(&lossy, "\xFF\xD9\nendstream"));
    CHECK(lossy.n * 2 < lossless.n);
    printf("  pictures: %zu bytes lossless, %zu with the photograph a JPEG\n", lossless.n, lossy.n);
    free(lossless.p);
    free(lossy.p);
    pd_layout_free(L);
    pd_doc_free(d);
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

    /* a show: the page cut in two parts, each a form written once; two pages that draw them, the second the latter
       part moved and half see-through, shown 50 ms */
    {
        pd_pdf_part parts[2];
        pd_pdf_show_page pages[2];
        pd_pdf_show show;
        buf_t v = { NULL, 0 };
        int32_t n = 0;
        size_t k, forms = 0;

        pd_layout_page_items(L, 0, NULL, 0, &n);
        parts[0].page = 0;
        parts[0].first = 0;
        parts[0].last = n / 2 - 1;
        parts[0].group = 0;
        parts[1].page = 0;
        parts[1].first = n / 2;
        parts[1].last = n - 1;
        parts[1].group = 1;
        pages[0].page = 0;
        pages[0].content = "/P0 Do /P1 Do";
        pages[0].extra = NULL;
        pages[1].page = 0;
        pages[1].content = "/P0 Do q /GO128 gs 1 0 0 1 10 0 cm /P1 Do Q";
        pages[1].extra = "/Dur 0.05";
        memset(&show, 0, sizeof(show));
        show.parts = parts;
        show.nparts = 2;
        show.pages = pages;
        show.npages = 2;
        show.catalog = "/PageMode /FullScreen";
        CHECK(pd_layout_write_pdf_show(L, &opt, &show, sink, &v) == PD_OK);
        CHECK(v.n > 1000 && xref_ok(&v));

        for (k = 0; k + 14 <= v.n; k++) {
            forms += memcmp(v.p + k, "/Subtype /Form", 14) == 0;
        }

        CHECK(forms == 2);
        CHECK(has(&v, "/Group << /S /Transparency >>") && has(&v, "/P1 "));
        CHECK(has(&v, "/GO128 << /ca 0.5020") && has(&v, "/Dur 0.05 >>") && has(&v, "/Count 2 >>"));
        CHECK(has(&v, "/PageMode /FullScreen") && !has(&v, "/Outlines"));
        CHECK(has(&v, "/P0 Do q /GO128 gs 1 0 0 1 10 0 cm /P1 Do Q"));
        CHECK(has(&v, "/FontFile2") && has(&v, "/DCTDecode"));   /* what the parts draw, as the pages have it */
        /* the catalog's script and a page's entries as long as they are: a document's JavaScript runs to kilobytes */
        {
            static char cat[4096], ext[2048];
            size_t q;

            strcpy(cat, "/OpenAction << /S /JavaScript /JS (var a=[");
            for (q = 0; q < 1500; q++) {
                strcat(cat, "0,");
            }
            strcat(cat, "7];) >>");
            strcpy(ext, "/Dur 0.05 /PieceInfo << /X << /D [");
            for (q = 0; q < 400; q++) {
                strcat(ext, "1 ");
            }
            strcat(ext, "9] >> >>");
            show.catalog = cat;
            pages[1].extra = ext;
            free(v.p);
            v.p = NULL;
            v.n = 0;
            CHECK(pd_layout_write_pdf_show(L, &opt, &show, sink, &v) == PD_OK);
            CHECK(xref_ok(&v) && has(&v, ",0,7];) >>") && has(&v, "1 9] >> >>"));
            show.catalog = "/PageMode /FullScreen";
            pages[1].extra = "/Dur 0.05";
        }
        pages[1].page = 5;
        CHECK(pd_layout_write_pdf_show(L, &opt, &show, sink, &v) == PD_ERR_ARG);
        pages[1].page = 0;
        pages[1].content = NULL;
        CHECK(pd_layout_write_pdf_show(L, &opt, &show, sink, &v) == PD_ERR_ARG);
        free(v.p);
    }

    free(a.p);
    free(b.p);
    free(u.p);
    pd_layout_free(L);
    pd_doc_free(d);
    test_decoded_images();
    test_jpeg_option();
    pd_font_free(latin);
    pd_font_free(cjk);
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
