/*
 * Parade PDF reader: internal interface shared by its parts
 *
 *   pd_pdfr.c       objects, cross-reference, streams and their filters,
 *                   the standard security handler
 *   pd_pdfr_jpeg.c  a JPEG decoder (baseline and progressive) for DCTDecode
 *   pd_pdfr_font.c  fonts: embedded TrueType, CFF and Type 1, encodings,
 *                   ToUnicode, CMaps, stand-ins for fonts not embedded
 *   pd_pdfr_draw.c  the content-stream interpreter, its display list, the
 *                   rasterizer and the page's text
 *   pd_pdfr_edit.c  annotations, form fields and the incremental update
 *
 * Objects are values (pv) held in an arena that lives as long as the
 * document; nothing parsed is freed on its own.
 */

#ifndef PD_PDFR_H
#define PD_PDFR_H

#include <stddef.h>
#include <stdint.h>
#include "parade_pdfread.h"

enum {
    PV_NULL = 0, PV_BOOL, PV_INT, PV_REAL, PV_NAME, PV_STRING, PV_ARRAY, PV_DICT, PV_REF, PV_STREAM,
    PV_OP,          /* a content-stream operator (in content parsing only) */
    PV_ERR          /* a parse error, the end of the data */
};

typedef struct pv pv;
typedef struct pkv pkv;
typedef struct pstream pstream;

struct pv {
    uint8_t t;
    uint8_t hex;                /* a string written in hex */
    int32_t n;                  /* string, name: bytes; array, dict: entries; ref: generation */
    union {
        int64_t i;              /* bool, int; ref: object number */
        double r;
        const char* s;          /* name, string, op (NUL ended too) */
        pv* a;
        pkv* d;
        pstream* st;
    } u;
};

struct pkv {
    const char* key;
    pv val;
};

struct pstream {
    pkv* d;                     /* its dictionary */
    int32_t nd;
    int64_t off, len;           /* raw data in the file */
    const uint8_t* data;        /* or here (a stream made or changed in this session) */
    int32_t num, gen;           /* the object it is (for decryption), 0 if inside an object stream */
};

/* an arena of chunks */
typedef struct parena {
    struct parena* next;
    size_t used, cap;
    unsigned char mem[1];
} parena;

void* pa_alloc(parena** a, size_t n);
char* pa_strdup(parena** a, const char* s, size_t n);
void pa_free(parena* a);

/* a growing byte buffer */
typedef struct {
    unsigned char* p;
    size_t n, cap;
    int err;
} pbuf;

void qb_put(pbuf* b, const void* d, size_t n);
void qb_puts(pbuf* b, const char* s);
void qb_printf(pbuf* b, const char* fmt, ...);
void qb_num(pbuf* b, double v);    /* a number as PDF writes it: no exponent, trimmed */
void qb_free(pbuf* b);

typedef struct {
    uint8_t type;               /* 0 free, 1 in the file at off, 2 in object stream off (index gen) */
    uint8_t loading;            /* being parsed (a loop guard) */
    uint8_t dirty;              /* changed or made in this session: written in the update */
    int32_t gen;
    int64_t off;
    pv* obj;                    /* parsed, NULL until then */
} pxref;

typedef struct pfont pfont;     /* pd_pdfr_font.c */
typedef struct ppage_cache ppage_cache;   /* pd_pdfr_draw.c */

typedef struct {
    pv ref;                     /* the page object (a PV_REF) */
    pv dict;                    /* its dictionary */
    double media[4], crop[4];   /* llx lly urx ury */
    int32_t rotate;             /* 0 90 180 270 */
    pv resources;               /* inherited */
} ppage;

struct pd_pdfr {
    unsigned char* data;        /* the file */
    size_t len;
    int own;
    parena* arena;
    pxref* xref;
    int32_t nxref;
    pv trailer;
    int64_t last_xref;          /* offset of the last cross-reference section */
    int xref_stream;            /* that section a stream */
    int repaired;
    ppage* pages;
    int32_t npages;
    /* encryption */
    int crypt;                  /* 0 none, 1 RC4, 2 AES-128, 3 AES-256 */
    int crypt_strings, crypt_streams;   /* the method strings and streams take (0 identity) */
    int crypt_v, crypt_r, crypt_meta;
    unsigned char key[32];
    int keylen;
    pv encrypt_ref;
    /* fonts and pages */
    pfont* fonts;               /* every font loaded, a list */
    ppage_cache** cache;        /* each page's display list once made */
    char font_dirs[4][512];
    int nfont_dirs;
    int32_t modified;
    int32_t changes;            /* bumped with every edit, so a host can tell */
    pv acroform;
    void* imgcache;             /* decoded pictures, the most recently drawn last (pd_pdfr_draw.c) */
    int32_t nimg, capimg;
    size_t imgbytes;
    void* gcache;               /* glyph bitmaps drawn (pd_pdfr_raster.c) */
    size_t gbytes;
    void* undo;                 /* edits that can be undone and redone (pd_pdfr_edit.c) */
    int32_t nundo, capundo, nredo;
    void* fields;               /* the form's fields, listed when first asked for */
    int32_t nfields, fields_changes;
    int32_t saved_changes;      /* changes at the last save */
    int32_t* page_lru;          /* pages whose lists are kept, the most recent last */
    int32_t nlru;
    char error[128];
};

typedef pd_pdfr pdoc;

/* values */
pv pv_null(void);
pv pv_int(int64_t i);
pv pv_real(double r);
pv pv_bool(int b);
pv pv_name(pdoc* d, const char* s);
pv pv_string(pdoc* d, const void* s, size_t n);
pv pv_ref(int32_t num, int32_t gen);
pv pv_array(pdoc* d, int32_t n);
pv pv_dict(pdoc* d, int32_t n);
int pv_is_name(pv v, const char* s);

pv pd_resolve(pdoc* d, pv v);                   /* follows references */
pv pd_get(pdoc* d, pv dict, const char* key);   /* resolved; a stream's dictionary too */
pv pd_get_raw(pv dict, const char* key);        /* not resolved */
pv pd_index(pdoc* d, pv arr, int32_t i);        /* resolved */
int pd_num(pdoc* d, pv v, double* out);
double pd_getnum(pdoc* d, pv dict, const char* key, double dflt);
int64_t pd_getint(pdoc* d, pv dict, const char* key, int64_t dflt);
const char* pd_getname(pdoc* d, pv dict, const char* key);     /* NULL if not a name */
pv pd_object(pdoc* d, int32_t num);
int pd_rect(pdoc* d, pv v, double* r);          /* 4 numbers, normalized */
int pd_matrix(pdoc* d, pv v, double* m);        /* 6 numbers */

/* the dictionary of a pv dict or stream (entries, count) */
pkv* pv_entries(pv v, int32_t* n);
void pd_dict_set(pdoc* d, pv* dict, const char* key, pv val);   /* replaces or adds (the dict grows in the arena) */
void pd_dict_del(pv* dict, const char* key);
pv pd_array_add(pdoc* d, pv arr, pv item);     /* a new array one longer */

/* stream data, decoded through its filters; *raw_jpeg etc. set for an image filter left undone. Free with free() */
enum { PF_NONE = 0, PF_DCT = 1, PF_JPX = 2, PF_JBIG2 = 3, PF_CCITT = 4 };
unsigned char* pd_stream_data(pdoc* d, pv stream, size_t* len, int* image_filter, pv* image_parms);
/* the raw bytes (still filtered, but decrypted) */
unsigned char* pd_stream_raw(pdoc* d, pv stream, size_t* len);
/* a filter chain applied to bytes */
unsigned char* pd_decode(pdoc* d, const unsigned char* data, size_t len, pv filter, pv parms, size_t* outlen,
                         int* image_filter, pv* image_parms);

/* a lexer over bytes, for files and content streams */
typedef struct {
    const unsigned char* p;
    size_t n, i;
    pdoc* d;
    int content;                /* content stream: bare words are operators */
} plex;

pv pl_read(plex* L);            /* the next object (or PV_OP / PV_ERR) */
void pl_skip_ws(plex* L);

/* strings: a text string (PDFDocEncoding or UTF-16) as UTF-8, NUL ended; *n its length. malloc'ed */
char* pd_text_utf8(pv s, size_t* n);
/* UTF-8 as a PDF text string: ASCII as is, else UTF-16BE with its mark */
pv pd_utf8_text(pdoc* d, const char* utf8);

/* crypto (pd_pdfr.c) */
void pd_md5(const unsigned char* data, size_t n, unsigned char out[16]);
void pd_rc4(const unsigned char* key, int keylen, unsigned char* data, size_t n);
unsigned char* pd_decrypt(pdoc* d, int32_t num, int32_t gen, int method, const unsigned char* data, size_t n,
                          size_t* outlen);
unsigned char* pd_encrypt(pdoc* d, int32_t num, int32_t gen, int method, const unsigned char* data, size_t n,
                          size_t* outlen);

/* JPEG (pd_pdfr_jpeg.c): pixels 8 bits a component, comps 1, 3 or 4 (CMYK, Adobe-inverted already undone when
   *adobe says so); malloc'ed */
unsigned char* pd_jpeg_decode(const unsigned char* data, size_t n, int32_t* w, int32_t* h, int32_t* comps,
                              int* adobe_inverted);

/* CCITT fax (pd_pdfr.c): 1 bit a pixel rows, 1 black unless BlackIs1 */
unsigned char* pd_ccitt_decode(const unsigned char* data, size_t n, int32_t k, int32_t columns, int32_t rows,
                               int black1, int byte_align, size_t* outlen);

/* JBIG2 (pd_pdfr_jbig2.c): an embedded stream and its globals as rows of 1-bit samples, 0 black */
unsigned char* pd_jbig2_decode(const unsigned char* data, size_t n, const unsigned char* globals, size_t gn,
                               int32_t w, int32_t h, size_t* outlen);

/* page tree */
int pd_pages_load(pdoc* d);

/* fonts (pd_pdfr_font.c) */
pfont* pf_load(pdoc* d, pv fontdict);
void pf_free_all(pdoc* d);
/* the next character code in a string: returns bytes used; *cid the code/CID; *width in text space (1/1000 em
   units already divided); *gid; *uni up to 4 code points written, returns count in *nuni */
int pf_next(pfont* f, const unsigned char* s, size_t n, uint32_t* code);
double pf_width(pfont* f, uint32_t code);       /* glyph space / 1000 */
int pf_unicode(pfont* f, uint32_t code, uint32_t* out, int cap);
int pf_is_type3(pfont* f);
int pf_vertical(pfont* f);
pv pf_type3_proc(pfont* f, uint32_t code, double* matrix, pv* resources);
/* the outline of a code's glyph, in glyph space units (1000 an em for most fonts, the font's matrix applied),
   given to sink as doubles; returns 0 if it has one */
typedef struct {
    void (*move)(void* u, double x, double y);
    void (*line)(void* u, double x, double y);
    void (*cubic)(void* u, double x1, double y1, double x2, double y2, double x, double y);
    void (*close)(void* u);
} pf_sink;
int pf_outline(pfont* f, uint32_t code, const pf_sink* sink, void* user);
/* the font a code shows the same glyph of (for an outline cache's key), and its glyph id */
uint32_t pf_glyph_key(pfont* f, uint32_t code);
/* the standard 14 font most like it (Times, Helvetica, Courier and their weights and slants) */
const char* pf_std14(pfont* f);
/* encode UTF-8 in the font (for edits): bytes written, 0 if a character has no code */
size_t pf_encode(pfont* f, const char* utf8, unsigned char* out, size_t cap);
pv pf_dict(pfont* f);                /* the font's dictionary */

/* the page's display list (pd_pdfr_draw.c) */
void pd_pages_cache_free(pdoc* d);
void pd_page_cache_drop(pdoc* d, int32_t page);

/* the edit layer's own (pd_pdfr_edit.c) */
void pd_edit_free(pdoc* d);
#endif
