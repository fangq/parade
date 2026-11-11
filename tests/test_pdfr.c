/*
 * Parade PDF reader tests: files made here (hand-written objects, broken
 * cross-references, object and xref streams, Flate and predictors), files
 * Parade's own writer makes, and encrypted ones from Ghostscript
 * (tests/data/enc_*.pdf) -- each opened, drawn and its text read back, the
 * pixels checked where the content says they are.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parade_layout.h"
#include "parade_pdfread.h"

static int failures = 0, checks = 0;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int pd_deflate(const void* data, size_t n, int zlib, unsigned char** out, size_t* outlen);
int pd_jbig2_mq_check(void);

typedef struct {
    unsigned char* p;
    size_t n, cap;
} buf_t;

static void put(buf_t* b, const void* d, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = (unsigned char*)realloc(b->p, b->cap);
    }

    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void puts_(buf_t* b, const char* s) {
    put(b, s, strlen(s));
}

static int sink(void* user, const void* data, size_t len) {
    put((buf_t*)user, data, len);
    return 0;
}

/* a PDF of the objects given (1..n, each its body after "n 0 obj"), its xref right unless broken says so */
static buf_t make_pdf(const char* const* objs, int n, int broken) {
    buf_t b = { NULL, 0, 0 };
    long* off = (long*)calloc((size_t)n + 1, sizeof(long));
    long xref;
    int i;
    char tmp[128];

    puts_(&b, "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n");

    for (i = 1; i <= n; i++) {
        off[i] = (long)b.n;
        snprintf(tmp, sizeof(tmp), "%d 0 obj\n", i);
        puts_(&b, tmp);
        puts_(&b, objs[i - 1]);
        puts_(&b, "\nendobj\n");
    }

    xref = (long)b.n;
    snprintf(tmp, sizeof(tmp), "xref\n0 %d\n0000000000 65535 f \n", n + 1);
    puts_(&b, tmp);

    for (i = 1; i <= n; i++) {
        snprintf(tmp, sizeof(tmp), "%010ld 00000 n \n", broken ? off[i] + 7 : off[i]);
        puts_(&b, tmp);
    }

    snprintf(tmp, sizeof(tmp), "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", n + 1, xref);
    puts_(&b, tmp);
    free(off);
    return b;
}

static char* stream_obj(const char* dict, const char* data) {
    size_t n = strlen(data);
    char* s = (char*)malloc(strlen(dict) + n + 64);

    sprintf(s, "<< %s /Length %zu >>\nstream\n%s\nendstream", dict, n, data);
    return s;
}

/* the page drawn at 72 dpi: a pixel's red, green and blue at (x, y) from the top left */
static uint8_t* draw(pd_pdfr* p, int page, int32_t* w, int32_t* h) {
    double pw, ph;
    uint8_t* buf;

    if (pd_pdfr_page_size(p, page, &pw, &ph) != PD_OK) {
        return NULL;
    }

    *w = (int32_t)(pw + 0.5);
    *h = (int32_t)(ph + 0.5);
    buf = (uint8_t*)malloc((size_t)*w * *h * 4);

    if (pd_pdfr_render(p, page, 1, 0, 0, buf, *w, *h, *w * 4, 0) != PD_OK) {
        free(buf);
        return NULL;
    }

    return buf;
}

static uint32_t px(const uint8_t* buf, int32_t w, int x, int y) {
    const uint8_t* q = buf + ((size_t)y * w + x) * 4;

    return ((uint32_t)q[0] << 16) | ((uint32_t)q[1] << 8) | q[2];
}

static int has_bytes(const unsigned char* p, size_t n, const char* s) {
    size_t k = strlen(s), i;

    for (i = 0; i + k <= n; i++) {
        if (memcmp(p + i, s, k) == 0) {
            return 1;
        }
    }

    return 0;
}

static int inked_in(const uint8_t* buf, int32_t w, int x0, int y0, int x1, int y1) {
    int x, y, n = 0;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if (px(buf, w, x, y) != 0xFFFFFF) {
                n++;
            }
        }
    }

    return n;
}

static int dark_in(const uint8_t* buf, int32_t w, int x0, int y0, int x1, int y1) {
    int x, y, n = 0;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            if ((px(buf, w, x, y) & 0xFF) < 128) {
                n++;
            }
        }
    }

    return n;
}

static char* page_text(pd_pdfr* p, int page) {
    int32_t n = 0, i, k = 0;
    pd_pdfr_char* c;
    char* s;

    pd_pdfr_page_text(p, page, NULL, 0, &n);
    c = (pd_pdfr_char*)calloc((size_t)n + 1, sizeof(pd_pdfr_char));
    s = (char*)malloc((size_t)n * 4 + 1);
    pd_pdfr_page_text(p, page, c, n, &n);

    for (i = 0; i < n; i++) {
        uint32_t cp = c[i].cp;

        if (cp < 0x80) {
            s[k++] = (char)cp;
        } else if (cp < 0x800) {
            s[k++] = (char)(0xC0 | (cp >> 6));
            s[k++] = (char)(0x80 | (cp & 63));
        } else {
            s[k++] = (char)(0xE0 | (cp >> 12));
            s[k++] = (char)(0x80 | ((cp >> 6) & 63));
            s[k++] = (char)(0x80 | (cp & 63));
        }
    }

    s[k] = 0;
    free(c);
    return s;
}

/* a page with a red box and black Helvetica text */
static const char* content1 = "1 0 0 rg 100 600 200 100 re f\n"
                              "BT /F1 24 Tf 72 500 Td (Hello PDF) Tj ET\n"
                              "0 0 1 RG 4 w 100 300 m 400 300 l S\n";

static void test_basic(int broken) {
    char* cs = stream_obj("", content1);
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        cs,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
    };
    buf_t b = make_pdf(objs, 5, broken);
    pd_pdfr* p = NULL;
    int32_t w, h;
    uint8_t* img;
    char* text;

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    CHECK(pd_pdfr_page_count(p) == 1);
    img = draw(p, 0, &w, &h);
    CHECK(img && w == 612 && h == 792);

    if (img) {
        /* the box: user space 100..300 x 600..700 is 92..192 down from the top */
        CHECK(px(img, w, 200, 142) == 0xFF0000);
        CHECK(px(img, w, 50, 142) == 0xFFFFFF);
        CHECK(px(img, w, 99, 142) == 0xFFFFFF && px(img, w, 101, 142) == 0xFF0000);
        /* the line: 4 wide about y = 300 (492 from the top) */
        CHECK(px(img, w, 250, 492) == 0x0000FF);
        CHECK(px(img, w, 250, 497) == 0xFFFFFF);
        /* the text: dark pixels on its baseline's line, none above the cap height */
        CHECK(dark_in(img, w, 72, 275, 200, 292) > 50);
        CHECK(dark_in(img, w, 72, 240, 200, 262) == 0);
        free(img);
    }

    text = page_text(p, 0);
    CHECK(strcmp(text, "Hello PDF") == 0);
    free(text);

    {
        pd_pdfr_char c[16];
        int32_t n;

        CHECK(pd_pdfr_page_text(p, 0, c, 16, &n) == PD_OK && n == 9);
        /* "H" starts at x 72; its box spans the baseline (y 292 down) */
        CHECK(fabs(c[0].x0 - 72) < 0.5 && c[0].y0 < 292 && c[0].y1 > 292);
        CHECK(c[1].x0 >= c[0].x1 - 0.5);
        CHECK(fabs(c[0].size - 24) < 0.5);
    }

    pd_pdfr_free(p);
    free(cs);
    free(b.p);
}

/* an xref stream and an object stream, the content deflated with a PNG predictor on the xref */
static void test_streams(void) {
    buf_t b = { NULL, 0, 0 };
    const char* page_objs = "1 0 2 50 3 100 ";
    char objstm[1024], tmp[256];
    const char* o1 = "<< /Type /Catalog /Pages 2 0 R >>";
    const char* o2 = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    const char* o3 = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R >>";
    const char* content = "0 1 0 rg 50 50 100 100 re f";
    unsigned char* z;
    size_t zn;
    long off4, off5, offx;
    int k;

    /* the object stream: its header of numbers and offsets, then the objects at those offsets */
    memset(objstm, ' ', sizeof(objstm));
    memcpy(objstm, page_objs, strlen(page_objs));
    memcpy(objstm + 16, o1, strlen(o1));
    memcpy(objstm + 16 + 50, o2, strlen(o2));
    memcpy(objstm + 16 + 100, o3, strlen(o3));
    objstm[16 + 100 + strlen(o3)] = 0;

    puts_(&b, "%PDF-1.5\n");
    off4 = (long)b.n;
    pd_deflate(content, strlen(content), 1, &z, &zn);
    snprintf(tmp, sizeof(tmp), "4 0 obj\n<< /Length %zu /Filter /FlateDecode >>\nstream\n", zn);
    puts_(&b, tmp);
    put(&b, z, zn);
    puts_(&b, "\nendstream\nendobj\n");
    free(z);
    off5 = (long)b.n;
    snprintf(tmp, sizeof(tmp), "5 0 obj\n<< /Type /ObjStm /N 3 /First 16 /Length %zu >>\nstream\n", strlen(objstm));
    puts_(&b, tmp);
    puts_(&b, objstm);
    puts_(&b, "\nendstream\nendobj\n");
    offx = (long)b.n;

    {
        /* rows of W [1 4 2], each with a PNG Up predictor byte */
        unsigned char rows[7][8], prev[7], raw[7 * 8];
        long fields[7][3] = { {0, 0, 65535}, {2, 5, 0}, {2, 5, 1}, {2, 5, 2}, {1, off4, 0}, {1, off5, 0}, {1, offx, 0} };
        int r, j;

        memset(prev, 0, sizeof(prev));

        for (r = 0; r < 7; r++) {
            unsigned char cur[7];

            cur[0] = (unsigned char)fields[r][0];

            for (j = 0; j < 4; j++) {
                cur[1 + j] = (unsigned char)(fields[r][1] >> (24 - 8 * j));
            }

            cur[5] = (unsigned char)(fields[r][2] >> 8);
            cur[6] = (unsigned char)fields[r][2];
            rows[r][0] = 2;

            for (j = 0; j < 7; j++) {
                rows[r][1 + j] = (unsigned char)(cur[j] - prev[j]);
            }

            memcpy(prev, cur, 7);
            memcpy(raw + 8 * r, rows[r], 8);
        }

        pd_deflate(raw, sizeof(raw), 1, &z, &zn);
        snprintf(tmp, sizeof(tmp), "6 0 obj\n<< /Type /XRef /Size 7 /W [1 4 2] /Root 1 0 R /Filter /FlateDecode "
                 "/DecodeParms << /Predictor 12 /Columns 7 >> /Length %zu >>\nstream\n", zn);
        puts_(&b, tmp);
        put(&b, z, zn);
        puts_(&b, "\nendstream\nendobj\n");
        free(z);
        snprintf(tmp, sizeof(tmp), "startxref\n%ld\n%%%%EOF\n", offx);
        puts_(&b, tmp);
    }

    {
        pd_pdfr* p = NULL;
        int32_t w, h;
        uint8_t* img;

        CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
        CHECK(pd_pdfr_page_count(p) == 1);
        img = draw(p, 0, &w, &h);
        CHECK(img && w == 200 && h == 200);

        if (img) {
            CHECK(px(img, w, 100, 100) == 0x00FF00);
            CHECK(px(img, w, 20, 20) == 0xFFFFFF);
            free(img);
        }

        pd_pdfr_free(p);
    }

    (void)k;
    free(b.p);
}

/* a page turned 90 degrees and a crop box: what is drawn where */
static void test_rotate(void) {
    char* cs = stream_obj("", "1 0 0 rg 0 0 50 50 re f");
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 /Rotate 90 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 300] /Contents 4 0 R >>",
        cs
    };
    buf_t b = make_pdf(objs, 4, 0);
    pd_pdfr* p = NULL;
    double w, h;
    int32_t W, H;
    uint8_t* img;

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    CHECK(pd_pdfr_page_size(p, 0, &w, &h) == PD_OK && w == 300 && h == 400);
    img = draw(p, 0, &W, &H);

    if (img) {
        /* the user space origin (bottom left) turned clockwise goes to the top left */
        CHECK(px(img, W, 10, 10) == 0xFF0000);
        CHECK(px(img, W, 10, 390) == 0xFFFFFF);
        CHECK(px(img, W, 290, 10) == 0xFFFFFF);
        free(img);
    }

    pd_pdfr_free(p);
    free(cs);
    free(b.p);
}

/* clipping, an even-odd fill, a dash, an axial shading, an inline image */
static void test_paint(void) {
    char* cs = stream_obj("",
                          "q 0 0 100 100 re W n 0 0 1 rg 0 0 200 200 re f Q\n"                 /* clipped to 100 square */
                          "0 g 120 0 m 200 0 l 200 80 l 120 80 l h 140 20 m 180 20 l 180 60 l 140 60 l h f*\n"
                          "q 240 0 160 400 re W n /Sh sh Q\n"
                          "q 50 0 0 50 0 150 cm BI /W 2 /H 1 /BPC 8 /CS /RGB /F /AHx ID FF0000 00FF00> EI Q\n");
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Contents 4 0 R "
        "/Resources << /Shading << /Sh 5 0 R >> >> >>",
        cs,
        "<< /ShadingType 2 /ColorSpace /DeviceRGB /Coords [250 0 350 0] /Extend [false false] "
        "/Function << /FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >>"
    };
    buf_t b = make_pdf(objs, 5, 0);
    pd_pdfr* p = NULL;
    int32_t W, H;
    uint8_t* img;

    (void)0;
    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    img = draw(p, 0, &W, &H);

    if (img) {
        /* the clip: blue inside 100 x 100 at the bottom left, none past it */
        CHECK(px(img, W, 50, 350) == 0x0000FF);
        CHECK(px(img, W, 150, 260) == 0xFFFFFF || px(img, W, 150, 260) != 0x0000FF);
        /* the even-odd hole */
        CHECK(px(img, W, 125, 395) == 0x000000);
        CHECK(px(img, W, 160, 360) == 0xFFFFFF);
        /* the shading's colours along x: red at its start, blue at its end, white outside (no extend) */
        {
            uint32_t a = px(img, W, 252, 100), c = px(img, W, 348, 100), m = px(img, W, 300, 100);

            CHECK((a >> 16) > 240 && (a & 0xFF) < 16);
            CHECK((c & 0xFF) > 240 && (c >> 16) < 16);
            CHECK((m >> 16) > 100 && (m >> 16) < 160 && (m & 0xFF) > 100 && (m & 0xFF) < 160);
            CHECK(px(img, W, 360, 100) == 0xFFFFFF);
        }
        /* the inline image: two pixels, red then green, over 50 x 50 at the top left of user y 150..200 */
        CHECK(px(img, W, 5, 225) == 0xFF0000);
        CHECK(px(img, W, 45, 225) == 0x00FF00);
        free(img);
    }

    pd_pdfr_free(p);
    free(cs);
    free(b.p);
}

static pd_font* latin;

static const pd_font* resolver(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;
    (void)family;
    (void)weight;
    (void)italic;
    return latin;
}

/* q nested 300 deep, a colour set at the bottom: all of it undone by the Qs, nothing read past what was kept */
static void test_deep_q(void) {
    char content[8192] = "", *cs;
    int i;

    for (i = 0; i < 300; i++) {
        strcat(content, "q ");
    }

    strcat(content, "0 1 0 rg 0 0 50 50 re f ");

    for (i = 0; i < 300; i++) {
        strcat(content, "Q ");
    }

    strcat(content, "60 0 50 50 re f");
    cs = stream_obj("", content);

    {
        const char* objs[] = {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R >>",
            cs
        };
        buf_t b = make_pdf(objs, 4, 0);
        pd_pdfr* p = NULL;
        int32_t w, h;
        uint8_t* img;

        CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
        img = draw(p, 0, &w, &h);

        if (img) {
            CHECK(px(img, w, 25, 175) == 0x00FF00);     /* green, at the bottom of the nesting */
            CHECK(px(img, w, 85, 175) == 0x000000);     /* black again once it is all restored */
            free(img);
        }

        pd_pdfr_free(p);
        free(b.p);
    }

    free(cs);
}

/* Parade's own PDF read back: its text, in order, and its pages */
static void test_roundtrip(void) {
    pd_doc* d;
    pd_layout* L;
    pd_pdf_options opt;
    buf_t out = { NULL, 0, 0 };
    pd_block_id sec, para;
    pd_pos at;
    const char* text = "Round trip through Parade's writer and reader, caf\xC3\xA9.";
    pd_pdfr* p = NULL;

    if (pd_font_load_file("/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf", 0, &latin) != PD_OK) {
        printf("no Liberation Serif: round trip skipped\n");
        return;
    }

    pd_doc_new(&d);
    pd_doc_set_font_resolver(d, resolver, NULL);
    sec = pd_doc_child(d, pd_doc_root(d), 0);
    para = pd_doc_child(d, sec, 0);
    at.block = para;
    at.offset = 0;
    pd_doc_insert_text(d, at, text, strlen(text), PD_FORMAT_INHERIT, NULL);

    pd_layout_new(d, &L);
    pd_layout_update(L, NULL);
    pd_pdf_options_init(&opt);
    CHECK(pd_layout_write_pdf(L, &opt, sink, &out) == PD_OK);
    CHECK(pd_pdfr_open_memory(out.p, out.n, NULL, &p) == PD_OK);

    if (p) {
        char* s = page_text(p, 0);

        CHECK(strstr(s, "Round trip through Parade") != NULL);
        CHECK(strstr(s, "caf\xC3\xA9.") != NULL);
        free(s);
    }

    pd_pdfr_free(p);
    pd_layout_free(L);
    pd_doc_free(d);
    pd_font_free(latin);
    free(out.p);
}

static void test_encrypted(void) {
    pd_pdfr* p = NULL;
    pd_status st = pd_pdfr_open_file("tests/data/enc_rc4.pdf", NULL, &p);
    char* s;

    CHECK(st == PD_OK);

    if (st == PD_OK) {
        s = page_text(p, 0);
        CHECK(strcmp(s, "Secret text here") == 0);
        free(s);
    }

    pd_pdfr_free(p);
    p = NULL;
    CHECK(pd_pdfr_open_file("tests/data/enc_rc4_user.pdf", NULL, &p) == PD_ERR_STATE);
    CHECK(p && pd_pdfr_encrypted(p) == 1);
    pd_pdfr_free(p);
    p = NULL;
    CHECK(pd_pdfr_open_file("tests/data/enc_rc4_user.pdf", "user", &p) == PD_OK);

    if (p) {
        s = page_text(p, 0);
        CHECK(strcmp(s, "Secret text here") == 0);
        free(s);
    }

    pd_pdfr_free(p);
    p = NULL;
    CHECK(pd_pdfr_open_file("tests/data/enc_rc4_user.pdf", "owner", &p) == PD_OK);
    pd_pdfr_free(p);
}

/* garbage, truncation, an empty file: refused, never crashing */
static void test_bad(void) {
    pd_pdfr* p = NULL;
    const char* junk = "not a pdf at all";
    char* cs = stream_obj("", content1);
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R >>",
        cs
    };
    buf_t b = make_pdf(objs, 4, 0);
    size_t cut;

    CHECK(pd_pdfr_open_memory(junk, strlen(junk), NULL, &p) == PD_ERR_FORMAT);
    pd_pdfr_free(p);
    p = NULL;
    CHECK(pd_pdfr_open_memory("", 0, NULL, &p) != PD_OK);
    pd_pdfr_free(p);

    /* cut short anywhere: opens or refuses, and draws what it opened */
    for (cut = 20; cut < b.n; cut += 37) {
        p = NULL;

        if (pd_pdfr_open_memory(b.p, cut, NULL, &p) == PD_OK) {
            int32_t w, h;
            uint8_t* img = draw(p, 0, &w, &h);

            free(img);
        }

        pd_pdfr_free(p);
    }

    checks++;
    free(cs);
    free(b.p);
}


/* ---- editing ---- */

static buf_t saved(pd_pdfr* p) {
    buf_t b = { NULL, 0, 0 };

    CHECK(pd_pdfr_save(p, sink, &b) == PD_OK);
    return b;
}

static pd_pdfr* reopen(const buf_t* b, const char* pw) {
    pd_pdfr* q = NULL;

    CHECK(pd_pdfr_open_memory(b->p, b->n, pw, &q) == PD_OK);
    return q;
}

static buf_t hello_pdf(void) {
    static char* cs;
    const char* objs[5];

    cs = stream_obj("", content1);
    objs[0] = "<< /Type /Catalog /Pages 2 0 R >>";
    objs[1] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
              "/Resources << /Font << /F1 5 0 R >> >> >>";
    objs[3] = cs;
    objs[4] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    {
        buf_t b = make_pdf(objs, 5, 0);

        free(cs);
        return b;
    }
}

static void test_annots(void) {
    buf_t b = hello_pdf(), out;
    pd_pdfr* p = NULL, *q;
    pd_pdfr_char c[16];
    int32_t n, w, h, i;
    double quad[8];
    uint8_t* img;
    pd_pdfr_annot a;

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    pd_pdfr_page_text(p, 0, c, 16, &n);
    /* a highlight over "Hello" */
    quad[0] = c[0].x0;
    quad[1] = c[0].y0;
    quad[2] = c[4].x1;
    quad[3] = c[0].y0;
    quad[4] = c[0].x0;
    quad[5] = c[0].y1;
    quad[6] = c[4].x1;
    quad[7] = c[0].y1;
    CHECK(pd_pdfr_modified(p) == 0);
    CHECK(pd_pdfr_add_markup(p, 0, PD_ANNOT_HIGHLIGHT, quad, 1, 0xFFFF00, "hi") == 0);
    CHECK(pd_pdfr_modified(p) == 1);
    CHECK(pd_pdfr_annot_count(p, 0) == 1);
    img = draw(p, 0, &w, &h);

    if (img) {
        /* yellow over the white between the letters, the letters still dark: multiplied, not painted over */
        int yellow = 0, x, y;

        for (y = (int)c[0].y0 + 1; y < (int)c[0].y1; y++) {
            for (x = (int)c[0].x0; x < (int)c[4].x1; x++) {
                if (px(img, w, x, y) == 0xFFFF00) {
                    yellow++;
                }
            }
        }

        CHECK(yellow > 100);
        CHECK(dark_in(img, w, (int)c[0].x0, (int)c[0].y0, (int)c[4].x1, (int)c[0].y1) > 50);
        CHECK(px(img, w, (int)c[4].x1 + 30, (int)(c[0].y0 + c[0].y1) / 2) == 0xFFFFFF);
        free(img);
    }

    /* a square, an ellipse, an arrow, ink, a note, a text box */
    CHECK(pd_pdfr_add_shape(p, 0, PD_ANNOT_SQUARE, 400, 400, 500, 480, 0xFF0000, 0xFF00FF00, 4) == 1);
    CHECK(pd_pdfr_add_shape(p, 0, PD_ANNOT_CIRCLE, 100, 600, 200, 700, 0x0000FF, 0, 2) == 2);
    CHECK(pd_pdfr_add_shape(p, 0, PD_ANNOT_ARROW, 300, 600, 400, 700, 0x000000, 0, 2) == 3);
    {
        double pts[] = { 50, 50, 60, 60, 70, 50, 80, 60 };
        int32_t cnt[] = { 4 };

        CHECK(pd_pdfr_add_ink(p, 0, pts, cnt, 1, 0x00AA00, 3) == 4);
    }
    CHECK(pd_pdfr_add_note(p, 0, 550, 30, "A note", 0xFFD700) == 5);
    CHECK(pd_pdfr_add_text_box(p, 0, 50, 700, 300, 760, "Typed in a box", 14, 0x000080) == 6);
    CHECK(pd_pdfr_annot_get(p, 0, 3, &a) == PD_OK && a.kind == PD_ANNOT_ARROW);
    CHECK(pd_pdfr_annot_get(p, 0, 1, &a) == PD_OK && a.kind == PD_ANNOT_SQUARE && a.color == 0xFF0000);
    CHECK(fabs(a.x0 - 400) < 0.01 && fabs(a.y0 - 400) < 0.01 && fabs(a.x1 - 500) < 0.01);
    img = draw(p, 0, &w, &h);

    if (img) {
        CHECK(px(img, w, 402, 440) == 0xFF0000);    /* the border */
        CHECK(px(img, w, 450, 440) == 0x00FF00);    /* the fill */
        CHECK(px(img, w, 395, 440) == 0xFFFFFF);
        CHECK(px(img, w, 65, 56) != 0xFFFFFF);      /* the ink */
        CHECK(inked_in(img, w, 52, 705, 250, 740) > 20);    /* the box's text */
        CHECK(inked_in(img, w, 52, 742, 250, 758) == 0);    /* one line of it */
        free(img);
    }

    /* saved as an update: the file as it was, then the update */
    out = saved(p);
    CHECK(out.n > b.n && memcmp(out.p, b.p, b.n) == 0);
    CHECK(has_bytes(out.p + b.n, out.n - b.n, "/Prev"));
    q = reopen(&out, NULL);

    if (q) {
        CHECK(pd_pdfr_annot_count(q, 0) == 7);
        CHECK(pd_pdfr_annot_get(q, 0, 0, &a) == PD_OK && a.kind == PD_ANNOT_HIGHLIGHT && strcmp(a.contents, "hi") == 0);
        CHECK(pd_pdfr_annot_get(q, 0, 5, &a) == PD_OK && a.kind == PD_ANNOT_TEXT && strcmp(a.contents, "A note") == 0);
        CHECK(pd_pdfr_annot_get(q, 0, 6, &a) == PD_OK && a.kind == PD_ANNOT_FREETEXT);
        img = draw(q, 0, &w, &h);

        if (img) {
            CHECK(px(img, w, 450, 440) == 0x00FF00);
            free(img);
        }

        /* and a second update on top of the first */
        CHECK(pd_pdfr_annot_move(q, 0, 1, 20, 10) == PD_OK);
        CHECK(pd_pdfr_annot_get(q, 0, 1, &a) == PD_OK && fabs(a.x0 - 420) < 0.01 && fabs(a.y0 - 410) < 0.01);
        {
            buf_t out2 = saved(q);
            pd_pdfr* r = reopen(&out2, NULL);

            CHECK(out2.n > out.n && memcmp(out2.p, out.p, out.n) == 0);
            CHECK(r && pd_pdfr_annot_get(r, 0, 1, &a) == PD_OK && fabs(a.x0 - 420) < 0.01);
            CHECK(r && pd_pdfr_annot_count(r, 0) == 7);
            pd_pdfr_free(r);
            free(out2.p);
        }

        pd_pdfr_free(q);
    }

    /* delete, undo, redo */
    CHECK(pd_pdfr_annot_delete(p, 0, 2) == PD_OK && pd_pdfr_annot_count(p, 0) == 6);
    CHECK(pd_pdfr_annot_get(p, 0, 2, &a) == PD_OK && a.kind == PD_ANNOT_ARROW);
    CHECK(pd_pdfr_can_undo(p) && pd_pdfr_undo(p) == PD_OK && pd_pdfr_annot_count(p, 0) == 7);
    CHECK(pd_pdfr_annot_get(p, 0, 2, &a) == PD_OK && a.kind == PD_ANNOT_CIRCLE);
    CHECK(pd_pdfr_can_redo(p) && pd_pdfr_redo(p) == PD_OK && pd_pdfr_annot_count(p, 0) == 6);

    for (i = 0; i < 8; i++) {   /* the seven made, the delete redone */
        CHECK(pd_pdfr_undo(p) == PD_OK);
    }

    CHECK(pd_pdfr_annot_count(p, 0) == 0);
    CHECK(!pd_pdfr_can_undo(p));
    pd_pdfr_free(p);
    free(out.p);
    free(b.p);
}

static void test_forms(void) {
    char* off = stream_obj("/BBox [0 0 20 20]", "");
    char* yes = stream_obj("/BBox [0 0 20 20]", "0 0 1 rg 3 3 14 14 re f");
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 7 0 R] /DA (/Helv 0 Tf 0 g) "
        "/DR << /Font << /Helv 5 0 R >> >> >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 400] /Contents 4 0 R /Annots [6 0 R 7 0 R] >>",
        "<< /Length 0 >>\nstream\n\nendstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        "<< /FT /Tx /T (name) /Subtype /Widget /Rect [50 300 250 330] /P 3 0 R /DA (/Helv 14 Tf 0 g) /F 4 >>",
        "<< /FT /Btn /T (agree) /Subtype /Widget /Rect [50 200 70 220] /P 3 0 R /AS /Off /F 4 "
        "/AP << /N << /Yes 8 0 R /Off 9 0 R >> >> >>",
        yes,
        off
    };
    buf_t b = make_pdf(objs, 9, 0), out;
    pd_pdfr* p = NULL, *q;
    pd_pdfr_field f;
    int32_t w, h;
    uint8_t* img;
    char opt[64];

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    CHECK(pd_pdfr_field_count(p) == 2);
    CHECK(pd_pdfr_field_get(p, 0, &f) == PD_OK && f.kind == PD_PDF_FIELD_TEXT && strcmp(f.name, "name") == 0);
    CHECK(f.page == 0 && fabs(f.x0 - 50) < 0.01 && fabs(f.y0 - 70) < 0.01 && fabs(f.font_size - 14) < 0.01);
    CHECK(pd_pdfr_field_get(p, 1, &f) == PD_OK && f.kind == PD_PDF_FIELD_CHECK && strcmp(f.value, "Off") == 0);
    CHECK(f.nopts == 1 && pd_pdfr_field_option(p, 1, 0, opt, sizeof(opt)) == PD_OK && strcmp(opt, "Yes") == 0);
    CHECK(pd_pdfr_field_at(p, 0, 100, 85) == 0);
    CHECK(pd_pdfr_field_at(p, 0, 60, 190) == 1);
    CHECK(pd_pdfr_field_at(p, 0, 300, 300) == -1);
    CHECK(pd_pdfr_widget_state(p, 0, 1, opt, sizeof(opt)) == PD_OK && strcmp(opt, "Yes") == 0);
    CHECK(pd_pdfr_widget_state(p, 0, 0, opt, sizeof(opt)) == PD_OK && opt[0] == 0);

    img = draw(p, 0, &w, &h);

    if (img) {
        CHECK(dark_in(img, w, 52, 72, 248, 98) == 0);
        CHECK(px(img, w, 60, 190) == 0xFFFFFF);
        free(img);
    }

    CHECK(pd_pdfr_field_set(p, 0, "Ada Lovelace") == PD_OK);
    CHECK(pd_pdfr_field_set(p, 1, NULL) == PD_OK);
    CHECK(pd_pdfr_field_get(p, 0, &f) == PD_OK && strcmp(f.value, "Ada Lovelace") == 0);
    CHECK(pd_pdfr_field_get(p, 1, &f) == PD_OK && strcmp(f.value, "Yes") == 0);
    img = draw(p, 0, &w, &h);

    if (img) {
        CHECK(dark_in(img, w, 52, 72, 248, 98) > 40);    /* the text in its box */
        CHECK(px(img, w, 60, 190) == 0x0000FF);         /* the box ticked */
        free(img);
    }

    out = saved(p);
    q = reopen(&out, NULL);

    if (q) {
        CHECK(pd_pdfr_field_get(q, 0, &f) == PD_OK && strcmp(f.value, "Ada Lovelace") == 0);
        CHECK(pd_pdfr_field_get(q, 1, &f) == PD_OK && strcmp(f.value, "Yes") == 0);
        CHECK(has_bytes(out.p + b.n, out.n - b.n, "/NeedAppearances true"));
        img = draw(q, 0, &w, &h);

        if (img) {
            CHECK(px(img, w, 60, 190) == 0x0000FF);
            free(img);
        }

        /* unticked again */
        CHECK(pd_pdfr_field_set(q, 1, "Off") == PD_OK);
        CHECK(pd_pdfr_field_get(q, 1, &f) == PD_OK && strcmp(f.value, "Off") == 0);
        img = draw(q, 0, &w, &h);

        if (img) {
            CHECK(px(img, w, 60, 190) == 0xFFFFFF);
            free(img);
        }

        pd_pdfr_free(q);
    }

    pd_pdfr_free(p);
    free(out.p);
    free(b.p);
    free(off);
    free(yes);
}

static void test_replace_text(void) {
    buf_t b = hello_pdf(), out;
    pd_pdfr* p = NULL, *q;
    char* s;

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    /* in place: the font has the letters */
    CHECK(pd_pdfr_replace_text(p, 0, 0, 5, "Howdy") == PD_OK);
    s = page_text(p, 0);
    CHECK(strcmp(s, "Howdy PDF") == 0);
    free(s);
    /* a letter WinAnsi has not got: covered and written over */
    CHECK(pd_pdfr_replace_text(p, 0, 6, 9, "\xCE\xA9") == PD_OK);
    s = page_text(p, 0);
    CHECK(strstr(s, "Howdy") != NULL && strstr(s, "?") != NULL);
    free(s);
    out = saved(p);
    q = reopen(&out, NULL);

    if (q) {
        s = page_text(q, 0);
        CHECK(strstr(s, "Howdy") != NULL);
        free(s);
        pd_pdfr_free(q);
    }

    CHECK(pd_pdfr_undo(p) == PD_OK);
    s = page_text(p, 0);
    CHECK(strcmp(s, "Howdy PDF") == 0);
    free(s);
    CHECK(pd_pdfr_undo(p) == PD_OK);
    s = page_text(p, 0);
    CHECK(strcmp(s, "Hello PDF") == 0);
    free(s);
    pd_pdfr_free(p);
    free(out.p);
    free(b.p);
}

/* a word in the middle of a TJ: that word changed, the others as they were */
static void test_replace_in_tj(void) {
    char* cs = stream_obj("", "BT /F1 20 Tf 72 700 Td [(alpha)-250(beta)-250(gamma)] TJ ET");
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        cs,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
    };
    buf_t b = make_pdf(objs, 5, 0);
    pd_pdfr* p = NULL;
    char* s;
    pd_pdfr_char c[32];
    int32_t n, i, at = -1;
    float x_gamma = 0, x_gamma2 = 0;

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    s = page_text(p, 0);
    CHECK(strcmp(s, "alpha beta gamma") == 0);
    free(s);
    pd_pdfr_page_text(p, 0, c, 32, &n);

    for (i = 0; i < n; i++) {
        if (c[i].cp == 'b' && at < 0) {
            at = i;
        }

        if (c[i].cp == 'g') {
            x_gamma = c[i].x0;
        }
    }

    CHECK(at > 0 && pd_pdfr_replace_text(p, 0, at, at + 4, "BETA") == PD_OK);
    s = page_text(p, 0);
    CHECK(strcmp(s, "alpha BETA gamma") == 0);
    free(s);
    pd_pdfr_page_text(p, 0, c, 32, &n);

    for (i = 0; i < n; i++) {
        if (c[i].cp == 'g') {
            x_gamma2 = c[i].x0;
        }
    }

    /* the words after it moved by what the new one is wider, the kerning kept */
    CHECK(x_gamma2 > x_gamma);
    pd_pdfr_free(p);
    free(cs);
    free(b.p);
}

/* lines and blocks found; a block replaced wrapped to its width, at its place, the old glyphs taken out (no white
   box over the page) and what followed them on their line where it was */
static void test_blocks(void) {
    char* cs = stream_obj("",
        "0 0 1 rg 0 0 612 792 re f 0 g\n"
        "BT /F1 20 Tf 72 720 Td (Heading here) Tj ET\n"
        "BT /F1 12 Tf 14 TL 72 680 Td (The first line of a paragraph) Tj T* (the second line of it) Tj T* "
        "(and its third line.) Tj ET\n"
        "BT /F1 12 Tf 14 TL 72 600 Td (Another paragraph begins) Tj T* (and ends here.) Tj ( Tail) Tj ET");
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        cs,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
    };
    buf_t b = make_pdf(objs, 5, 0), out;
    pd_pdfr* p = NULL;
    pd_pdfr_span sp[16], ln[16], old2;
    pd_pdfr_char c[400];
    int32_t n, nl, nc, i, tail = -1, a = -1, k;
    float tail_x = 0, tail_x2 = -1;
    char* s;

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    CHECK(pd_pdfr_page_spans(p, 0, PD_PDFR_LINES, ln, 16, &nl) == PD_OK && nl == 6);
    CHECK(pd_pdfr_page_spans(p, 0, PD_PDFR_BLOCKS, sp, 16, &n) == PD_OK && n == 3);
    CHECK(pd_pdfr_page_spans(p, 0, 7, sp, 16, &n) == PD_ERR_ARG);
    pd_pdfr_page_spans(p, 0, PD_PDFR_BLOCKS, sp, 16, &n);
    CHECK(n == 3 && sp[0].lines == 1 && sp[1].lines == 3 && sp[2].lines == 2);
    CHECK(fabs(sp[0].size - 20) < 0.5 && fabs(sp[1].size - 12) < 0.5);
    CHECK(fabs(sp[1].x0 - 72) < 1 && sp[1].y0 > sp[0].y1 && sp[2].y0 > sp[1].y1);
    CHECK(sp[1].align == 0);
    old2 = sp[1];

    /* the second block: written again, longer, wrapped to its width */
    CHECK(pd_pdfr_replace_text(p, 0, sp[1].first, sp[1].end - 1,
                               "Written anew, this paragraph is long enough that it has to wrap onto further lines "
                               "at the width the old one had.") == PD_OK);
    s = page_text(p, 0);
    CHECK(strstr(s, "first line") == NULL && strstr(s, "third line") == NULL);
    CHECK(strstr(s, "Written anew") != NULL && strstr(s, "Heading here") != NULL && strstr(s, "Tail") != NULL);
    free(s);
    pd_pdfr_page_spans(p, 0, PD_PDFR_LINES, ln, 16, &nl);
    k = 0;

    for (i = 0; i < nl; i++) {
        if (ln[i].y0 > old2.y0 - 2 && ln[i].y1 < old2.y1 + 40 && ln[i].x0 < 300) {
            k++;
            CHECK(ln[i].x1 <= old2.x1 + 3);            /* within the old width */
            CHECK(fabs(ln[i].x0 - 72) < 1);             /* from the old left edge */
        }
    }

    CHECK(k >= 3);
    pd_pdfr_page_text(p, 0, c, 400, &nc);

    for (i = 0; i < nc; i++) {
        if (c[i].cp == 'W' && a < 0) {
            a = i;
        }
    }

    CHECK(a >= 0 && fabs(c[a].y0 - old2.y0) < 1 && fabs(c[a].size - 12) < 0.5);    /* on the old first line */

    /* two lines of the last block taken out, " Tail" after them on the line kept where it was */
    pd_pdfr_page_spans(p, 0, PD_PDFR_BLOCKS, sp, 16, &n);
    pd_pdfr_page_text(p, 0, c, 400, &nc);

    for (i = 0; i + 3 < nc; i++) {
        if (c[i].cp == 'T' && c[i + 1].cp == 'a' && c[i + 2].cp == 'i' && c[i + 3].cp == 'l') {
            tail = i;
            tail_x = c[i].x0;
        }
    }

    a = -1;

    for (i = 0; i + 1 < nc; i++) {
        if (c[i].cp == 'A' && c[i + 1].cp == 'n') {
            a = i;
        }
    }

    CHECK(tail > 0 && a >= 0);

    if (tail > 0 && a >= 0) {
        CHECK(pd_pdfr_replace_text(p, 0, a, tail - 1, "Short") == PD_OK);   /* up to the space before Tail */
        pd_pdfr_page_text(p, 0, c, 400, &nc);

        for (i = 0; i + 3 < nc; i++) {
            if (c[i].cp == 'T' && c[i + 1].cp == 'a' && c[i + 2].cp == 'i' && c[i + 3].cp == 'l') {
                tail_x2 = c[i].x0;
            }
        }

        CHECK(fabs(tail_x2 - tail_x) < 0.01);
        s = page_text(p, 0);
        CHECK(strstr(s, "Another") == NULL && strstr(s, "ends here") == NULL && strstr(s, "Short") != NULL);
        free(s);
    }

    /* no white laid over the page's blue: the old text taken out, not hidden */
    {
        int32_t w, h;
        uint8_t* img = draw(p, 0, &w, &h);

        if (img) {
            int32_t x, y, white = 0;

            for (y = 0; y < h; y++) {
                for (x = 0; x < w; x++) {
                    white += px(img, w, x, y) == 0xFFFFFF;
                }
            }

            CHECK(white == 0);
            free(img);
        }
    }

    out = saved(p);
    {
        pd_pdfr* q = reopen(&out, NULL);

        if (q) {
            s = page_text(q, 0);
            CHECK(strstr(s, "Written anew") != NULL && strstr(s, "first line") == NULL);
            free(s);
            pd_pdfr_free(q);
        }
    }

    CHECK(pd_pdfr_undo(p) == PD_OK && pd_pdfr_undo(p) == PD_OK);
    s = page_text(p, 0);
    CHECK(strstr(s, "The first line of a paragraph") != NULL && strstr(s, "Another paragraph begins") != NULL);
    free(s);
    pd_pdfr_free(p);
    free(out.p);
    free(cs);
    free(b.p);
}

/* an address's lines (each short of the width, the first bold) written again a paragraph a line: each paragraph in
   its line's font, centred as they were */
static void test_pieces(void) {
    char* cs = stream_obj("",
        "BT /F2 14 Tf 250 700 Td (Ann Author, PhD) Tj ET\n"
        "BT /F1 14 Tf 174 683 Td (Department of Examples, Some University) Tj ET\n"
        "BT /F1 14 Tf 251.5 666 Td (Boston, MA, USA) Tj ET");
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> >>",
        cs,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>"
    };
    buf_t b = make_pdf(objs, 6, 0);
    pd_pdfr* p = NULL;
    pd_pdfr_span sp[8], ln[8];
    pd_pdfr_char c[200];
    int32_t n, nl, nc, i;
    float w_first = 0, w_second = 0, mid[3] = { 0, 0, 0 };

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    CHECK(pd_pdfr_page_spans(p, 0, PD_PDFR_BLOCKS, sp, 8, &n) == PD_OK && n == 1 && sp[0].lines == 3);
    CHECK(sp[0].align == 1);
    CHECK(pd_pdfr_replace_text(p, 0, sp[0].first, sp[0].end, "AAAA\nAAAA AAAA\nAAAA") == PD_OK);
    pd_pdfr_page_text(p, 0, c, 200, &nc);
    pd_pdfr_page_spans(p, 0, PD_PDFR_LINES, ln, 8, &nl);
    CHECK(nl == 3);

    for (i = 0; i < nc; i++) {
        if (c[i].cp == 'A' && w_first == 0 && i < ln[0].end) {
            w_first = c[i].x1 - c[i].x0;
        }

        if (c[i].cp == 'A' && w_second == 0 && nl > 1 && i >= ln[1].first) {
            w_second = c[i].x1 - c[i].x0;
        }
    }

    CHECK(w_first > w_second * 1.05);     /* Helvetica-Bold's A (722) is wider than Helvetica's (667) */

    for (i = 0; i < nl && i < 3; i++) {
        mid[i] = (ln[i].x0 + ln[i].x1) / 2;
    }

    CHECK(nl == 3 && fabs(mid[0] - mid[1]) < 1 && fabs(mid[1] - mid[2]) < 1);     /* centred as they were */
    CHECK(nl == 3 && fabs(ln[0].y0 - sp[0].y0) < 1.5);

    /* two paragraphs: the address's two pieces (its first line short, its other two the full width), each's style */
    CHECK(pd_pdfr_undo(p) == PD_OK);
    pd_pdfr_page_spans(p, 0, PD_PDFR_BLOCKS, sp, 8, &n);
    CHECK(pd_pdfr_replace_text(p, 0, sp[0].first, sp[0].end, "AAAA\nAAAA") == PD_OK);
    pd_pdfr_page_text(p, 0, c, 200, &nc);
    w_first = w_second = 0;

    for (i = 0; i < nc; i++) {
        if (c[i].cp == 'A' && i < 4) {
            w_first = c[i].x1 - c[i].x0;
        }

        if (c[i].cp == 'A' && i > 5) {
            w_second = c[i].x1 - c[i].x0;
        }
    }

    CHECK(w_first > w_second * 1.05);
    pd_pdfr_free(p);
    free(cs);
    free(b.p);
}

/* a line moved: every glyph where it was, moved by the amount (its spacing, kerning and scale kept); what followed it
   on its line where it was; the old glyphs gone from where they were; undone */
static void test_move_text(void) {
    char* cs = stream_obj("",
        "BT /F1 18 Tf 1 Tc 3 Tw 90 Tz 72 700 Td [(Move) -300 (me, please)] TJ 0 Tc 0 Tw 100 Tz ( stay) Tj ET\n"
        "BT /F1 12 Tf 72 600 Td (Another line) Tj ET");
    const char* objs[] = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R >> >> >>",
        cs,
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
    };
    buf_t b = make_pdf(objs, 5, 0);
    pd_pdfr* p = NULL;
    pd_pdfr_char c0[64], c1[64];
    int32_t n0, n1, i, end = -1, stay = -1, ok = 1, stay1 = -1;
    char* s;

    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);
    pd_pdfr_page_text(p, 0, c0, 64, &n0);

    for (i = 0; i + 3 < n0; i++) {
        if (c0[i].cp == 'e' && c0[i + 1].cp == ' ' && c0[i + 2].cp == 's' && c0[i + 3].cp == 't') {
            end = i + 1;    /* "Move me, please" */
            stay = i + 2;
        }
    }

    CHECK(end > 0 && pd_pdfr_move_text(p, 0, 0, end, 50, 100) == PD_OK);
    CHECK(pd_pdfr_move_text(p, 0, 0, 0, 1, 1) == PD_ERR_ARG);
    pd_pdfr_page_text(p, 0, c1, 64, &n1);
    s = page_text(p, 0);
    CHECK(strstr(s, "stay") != NULL && strstr(s, "Another line") != NULL && strstr(s, "please") != NULL);
    free(s);

    /* the moved glyphs: the same letters, each 50 right and 100 down of where it was */
    for (i = 0; i < end; i++) {
        int32_t j, hit = 0;

        if (c0[i].flags || c0[i].cp == ' ') {
            continue;
        }

        for (j = 0; j < n1; j++) {
            if (c1[j].cp == c0[i].cp && fabs(c1[j].x0 - c0[i].x0 - 50) < 0.01 && fabs(c1[j].y0 - c0[i].y0 - 100) < 0.01 &&
                fabs((c1[j].x1 - c1[j].x0) - (c0[i].x1 - c0[i].x0)) < 0.01) {
                hit = 1;
            }

            if (fabs(c1[j].x0 - c0[i].x0) < 0.01 && fabs(c1[j].y0 - c0[i].y0) < 0.01 && c1[j].cp == c0[i].cp) {
                ok = 0;     /* still where it was */
            }
        }

        ok &= hit;
    }

    CHECK(ok);

    for (i = 0; i < n1; i++) {
        if (c1[i].cp == 's' && i + 1 < n1 && c1[i + 1].cp == 't') {
            stay1 = i;
        }
    }

    CHECK(stay > 0 && stay1 >= 0 && fabs(c1[stay1].x0 - c0[stay].x0) < 0.01 && fabs(c1[stay1].y0 - c0[stay].y0) < 0.01);
    CHECK(pd_pdfr_undo(p) == PD_OK);
    pd_pdfr_page_text(p, 0, c1, 64, &n1);
    CHECK(n1 == n0 && fabs(c1[0].x0 - c0[0].x0) < 0.01 && fabs(c1[0].y0 - c0[0].y0) < 0.01);
    pd_pdfr_free(p);
    free(cs);
    free(b.p);
}

static unsigned char* slurp_file(const char* path, size_t* n) {
    FILE* fp = fopen(path, "rb");
    unsigned char* d;
    long len;

    if (!fp) {
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    d = (unsigned char*)malloc((size_t)len + 1);

    if (d && fread(d, 1, (size_t)len, fp) != (size_t)len) {
        free(d);
        d = NULL;
    }

    fclose(fp);
    *n = (size_t)len;
    return d;
}

/* the advance an AFM gives a code */
static int afm_width(const char* path, int code) {
    FILE* fp = fopen(path, "r");
    char line[512];
    int w = -1;

    while (fp && fgets(line, sizeof(line), fp)) {
        int c, wx;

        if (sscanf(line, "C %d ; WX %d", &c, &wx) == 2 && c == code) {
            w = wx;
            break;
        }
    }

    if (fp) {
        fclose(fp);
    }

    return w;
}

/* an embedded Type 1 font (a PFB), no Widths: drawn from its charstrings, each glyph as wide as they say */
static void test_type1(void) {
    const char* pfb = "/usr/share/fonts/type1/texlive-fonts-recommended/putr8a.pfb";
    const char* afm = "/usr/share/fonts/type1/texlive-fonts-recommended/putr8a.afm";
    size_t fn;
    unsigned char* font = slurp_file(pfb, &fn);
    buf_t b = { NULL, 0, 0 };
    const char* content = "BT /F1 40 Tf 50 700 Td (iMiM) Tj ET";
    long off[8], xref;
    char tmp[256];
    int i;
    pd_pdfr* p = NULL;
    pd_pdfr_char c[8];
    int32_t n, w, h;
    uint8_t* img;

    if (!font || afm_width(afm, 'i') <= 0) {
        printf("no Utopia Type 1 font: skipped\n");
        free(font);
        return;
    }

    puts_(&b, "%PDF-1.4\n");
    off[1] = (long)b.n;
    puts_(&b, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[2] = (long)b.n;
    puts_(&b, "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n");
    off[3] = (long)b.n;
    puts_(&b, "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
          "/Resources << /Font << /F1 5 0 R >> >> >>\nendobj\n");
    off[4] = (long)b.n;
    snprintf(tmp, sizeof(tmp), "4 0 obj\n<< /Length %zu >>\nstream\n", strlen(content));
    puts_(&b, tmp);
    puts_(&b, content);
    puts_(&b, "\nendstream\nendobj\n");
    off[5] = (long)b.n;
    puts_(&b, "5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Utopia-Regular /FontDescriptor 6 0 R >>\nendobj\n");
    off[6] = (long)b.n;
    puts_(&b, "6 0 obj\n<< /Type /FontDescriptor /FontName /Utopia-Regular /Flags 34 /FontFile 7 0 R >>\nendobj\n");
    off[7] = (long)b.n;
    snprintf(tmp, sizeof(tmp), "7 0 obj\n<< /Length %zu >>\nstream\n", fn);
    puts_(&b, tmp);
    put(&b, font, fn);
    puts_(&b, "\nendstream\nendobj\n");
    xref = (long)b.n;
    puts_(&b, "xref\n0 8\n0000000000 65535 f \n");

    for (i = 1; i <= 7; i++) {
        snprintf(tmp, sizeof(tmp), "%010ld 00000 n \n", off[i]);
        puts_(&b, tmp);
    }

    snprintf(tmp, sizeof(tmp), "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", xref);
    puts_(&b, tmp);
    CHECK(pd_pdfr_open_memory(b.p, b.n, NULL, &p) == PD_OK);

    if (p) {
        CHECK(pd_pdfr_page_text(p, 0, c, 8, &n) == PD_OK && n == 4);
        /* each advance the AFM's, at 40 points */
        CHECK(fabs((c[0].x1 - c[0].x0) - afm_width(afm, 'i') * 0.04) < 0.2);
        CHECK(fabs((c[1].x1 - c[1].x0) - afm_width(afm, 'M') * 0.04) < 0.2);
        CHECK(fabs(c[2].x0 - c[1].x1) < 0.01);
        img = draw(p, 0, &w, &h);

        if (img) {
            /* the M's strokes inked, the space above the letters not */
            CHECK(dark_in(img, w, (int)c[1].x0, 62, (int)c[1].x1, 92) > 200);
            CHECK(dark_in(img, w, (int)c[0].x0, 20, (int)c[3].x1, 50) == 0);
            free(img);
        }

        pd_pdfr_free(p);
    }

    free(font);
    free(b.p);
}

/* an encrypted file's update: new strings and streams encrypted as the file is */
static void test_encrypted_save(void) {
    pd_pdfr* p = NULL, *q;
    buf_t out;
    pd_pdfr_annot a;
    char* s;

    if (pd_pdfr_open_file("tests/data/enc_rc4_user.pdf", "user", &p) != PD_OK) {
        CHECK(0);
        return;
    }

    CHECK(pd_pdfr_add_note(p, 0, 100, 100, "encrypted note", 0xFFD700) == 0);
    out = saved(p);
    CHECK(!has_bytes(out.p, out.n, "encrypted note"));    /* not in the clear */
    q = reopen(&out, "user");

    if (q) {
        CHECK(pd_pdfr_annot_get(q, 0, 0, &a) == PD_OK && strcmp(a.contents, "encrypted note") == 0);
        s = page_text(q, 0);
        CHECK(strcmp(s, "Secret text here") == 0);
        free(s);
        pd_pdfr_free(q);
    }

    pd_pdfr_free(p);
    free(out.p);
}

int main(void) {
    test_basic(0);
    test_basic(1);      /* every offset in the xref wrong: read again by scanning */
    test_streams();
    test_rotate();
    test_paint();
    test_deep_q();
    test_roundtrip();
    test_encrypted();
    test_bad();
    test_annots();
    test_forms();
    test_replace_text();
    test_replace_in_tj();
    test_blocks();
    test_pieces();
    test_move_text();
    test_type1();
    CHECK(pd_jbig2_mq_check() == 0);    /* JBIG2's arithmetic decoder, T.88 H.2 */
    test_encrypted_save();
    printf("test_pdfr: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
