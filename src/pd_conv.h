/*
 * Parade converters: shared internals (output buffer, paragraph spans,
 * counters, the document builder used by importers)
 */

#ifndef PD_CONV_H
#define PD_CONV_H

#include <stdarg.h>
#include "parade_convert.h"
#include "pd_internal.h"

/* ------------------------------------------------------------------ */
/* output                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char* p;
    size_t n, cap;
    int err;
} pd_buf;

void pb_put(pd_buf* b, const void* s, size_t n);
void pb_puts(pd_buf* b, const char* s);
void pb_putc(pd_buf* b, char c);
void pb_printf(pd_buf* b, const char* fmt, ...);
void pb_free(pd_buf* b);
/* hand the buffer to a writer */
pd_status pb_flush(pd_buf* b, pd_writer fn, void* user);
void pb_base64(pd_buf* b, const unsigned char* data, size_t n);
size_t pd_base64_decode(const char* s, size_t n, unsigned char* out);

/* ------------------------------------------------------------------ */
/* reading a document                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int is_object;
    const char* text;           /* text spans: no U+FFFC inside */
    uint32_t len;
    uint32_t offset;            /* byte offset in the paragraph */
    pd_format_id format;
    pd_char_props cp;           /* resolved */
    pd_inline obj;              /* object spans */
} pd_span;

typedef int (*pd_span_fn)(void* user, const pd_span* s);

/* spans of a paragraph in order; stops early when fn returns nonzero */
int pd_conv_spans(const pd_doc* d, pd_block_id para, pd_span_fn fn, void* user);

/* numbers the exporters print: SEQ counters, footnotes, cross-references */
typedef struct {
    pd_block_id block;
    uint32_t offset;
    int32_t value;
} pd_numval;

typedef struct {
    const pd_doc* d;
    pd_numval* v;
    int32_t n, cap;
} pd_numbers;

void pd_numbers_init(pd_numbers* nb, const pd_doc* d);
void pd_numbers_free(pd_numbers* nb);
int32_t pd_numbers_at(const pd_numbers* nb, pd_block_id block, uint32_t offset);
/* the number shown for a REF field's target (its first SEQ field), 0 if none */
int32_t pd_numbers_ref(const pd_numbers* nb, pd_block_id target);

/* style name of a paragraph and whether its resolved weight is bold etc. */
const char* pd_conv_style_name(const pd_doc* d, pd_block_id para);
/* character properties a run of a paragraph gets from the paragraph alone */
void pd_conv_base_props(const pd_doc* d, pd_block_id para, pd_char_props* out);
int pd_conv_is_mono(const pd_char_props* cp);

/* list kind of a paragraph: 0 none, 1 bullet, 2 numbered; level 0-8 */
int pd_conv_list_kind(const pd_doc* d, pd_block_id para, int32_t* level);

/* ------------------------------------------------------------------ */
/* building a document                                                */
/* ------------------------------------------------------------------ */

#define BLD_DEPTH 24

/* placeholder width of an imported equation until the math font sizes it: 5pt per source byte, capped */
static inline pd_sp pd_conv_equation_width(size_t source_len) {
    return (pd_sp)(source_len < 4096 ? source_len : 4096) * PD_PT(5);
}

typedef struct {
    pd_block_id id;
    pd_block_id fresh;          /* its initial empty paragraph, still unused */
    pd_block_id saved_para;     /* the paragraph to go on with after this level */
    pd_char_props saved_cp;
} bld_level;

typedef struct {
    pd_doc* d;
    bld_level st[BLD_DEPTH];
    int32_t depth;
    pd_block_id para;           /* paragraph being filled, 0 = none */
    pd_buf pend;                /* text not yet inserted, in pend_fmt */
    pd_format_id pend_fmt;
    pd_char_props cp;           /* current character overrides */
    pd_style_id cstyle;
    pd_list_id lists[3];        /* [1] bullets, [2] numbers */
    /* the next paragraph */
    char pstyle[64];
    int32_t role, level, list_kind, list_level;
    pd_list_id list_id;         /* an importer's own list definition, over list_kind */
    pd_para_props pp;
    /* tables under construction */
    pd_block_id table[8], row[8];
    int32_t nrows[8], ncells[8], ntables;
    int32_t header_rows[8];
    pd_status err;
} pd_bld;

void bld_init(pd_bld* b, pd_doc* d);
/* properties of the next paragraph (the current one, if it has no text yet) */
void bld_para_style(pd_bld* b, const char* style, int32_t role, int32_t level);
void bld_list(pd_bld* b, int32_t kind, int32_t level);
/* start a paragraph now (ends the current one) */
pd_block_id bld_begin_para(pd_bld* b);
void bld_end_para(pd_bld* b);
void bld_text(pd_bld* b, const char* s, size_t n);
void bld_inline(pd_bld* b, const pd_inline* o);
void bld_set_format(pd_bld* b, const pd_char_props* cp);
/* containers */
void bld_table_begin(pd_bld* b);
void bld_row_begin(pd_bld* b, int header);
void bld_cell_begin(pd_bld* b, int32_t col_span, uint32_t background);
void bld_cell_end(pd_bld* b);
void bld_table_end(pd_bld* b);
pd_block_id bld_footnote_begin(pd_bld* b);
void bld_footnote_end(pd_bld* b);
pd_block_id bld_float_begin(pd_bld* b);
void bld_float_end(pd_bld* b);
void bld_break(pd_bld* b, int32_t kind);
pd_block_id bld_section(pd_bld* b, const pd_section_props* sp);
pd_status bld_finish(pd_bld* b);
pd_block_id bld_container(const pd_bld* b);

/* ------------------------------------------------------------------ */
/* the formats                                                        */
/* ------------------------------------------------------------------ */

pd_status pd_html_export(const pd_doc* d, pd_buf* out);
pd_status pd_html_import(pd_doc* d, const char* s, size_t n);
pd_status pd_md_export(const pd_doc* d, pd_buf* out);
pd_status pd_md_import(pd_doc* d, const char* s, size_t n);
pd_status pd_latex_export(const pd_doc* d, pd_buf* out);
pd_status pd_rtf_export(const pd_doc* d, pd_buf* out);
pd_status pd_rtf_import(pd_doc* d, const char* s, size_t n);
pd_status pd_docx_export(const pd_doc* d, pd_buf* out);
pd_status pd_docx_import(pd_doc* d, const unsigned char* s, size_t n);

/* ------------------------------------------------------------------ */
/* markup tokenizer (HTML and XML)                                    */
/* ------------------------------------------------------------------ */

enum {
    MT_END = 0,
    MT_TEXT = 1,
    MT_OPEN = 2,                /* <name attrs> */
    MT_CLOSE = 3,               /* </name> */
    MT_EMPTY = 4                /* <name attrs/> */
};

typedef struct {
    const char* s;
    size_t n, pos;
    int html;                   /* HTML rules: raw text in script/style, lowercase names */
    /* the current token */
    int type;
    char name[64];
    const char* text;           /* MT_TEXT: raw (entities not decoded) */
    size_t tlen;
    const char* attrs;          /* raw attribute text of a tag */
    size_t alen;
} pd_markup;

void mu_init(pd_markup* m, const char* s, size_t n, int html);
int mu_next(pd_markup* m);
/* attribute value (entities decoded) into buf; 1 if present */
int mu_attr(const pd_markup* m, const char* name, char* buf, size_t cap);
/* decode entities of raw text into out (grows); collapse: HTML whitespace collapsing */
void mu_decode(const char* s, size_t n, pd_buf* out);
/* local name of a tag without namespace prefix */
const char* mu_local(const char* name);

#endif /* PD_CONV_H */
