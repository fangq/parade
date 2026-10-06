/*
 * Parade internal JData/BJData codec
 *
 * A C99 port of the conventions in mimamo's mmm_json.hpp, mmm_bjdata.hpp
 * and mmm_base64.hpp, cut down to what a document needs:
 *  - a streaming writer that emits the same tree as JSON text or BJData
 *    (Draft-2+: little-endian, byte by byte whatever the host is; sizes in
 *    the smallest integer marker; object keys without 'S'; N-D integer
 *    arrays as optimized `[$t#[$l#<nd> dims` containers, row-major)
 *  - binary blobs as JData byte streams: base64 text in JSON, a raw `H`
 *    payload in BJData
 *  - a reader for both into one arena-allocated tree, hardened for
 *    untrusted input (bounds, depth and count checks)
 * The reader also accepts `#`-only and optimized objects, `N` no-ops, `B`,
 * `C`, `h` markers, column-major N-D arrays and \u surrogate pairs.
 */

#ifndef PD_JSON_H
#define PD_JSON_H

#include <stddef.h>
#include <stdint.h>
#include "parade_doc.h"

/* ------------------------------------------------------------------ */
/* writer                                                             */
/* ------------------------------------------------------------------ */

#define PJ_MAX_DEPTH 128

typedef struct {
    int binary;
    pd_writer fn;
    void* user;
    int err;
    int depth;
    int32_t count[PJ_MAX_DEPTH];    /* values written at each depth */
    int after_key;
    int compact;                    /* JSON on one line, no indentation */
    unsigned char buf[8192];
    size_t nbuf;
} pj_writer;

void pj_init(pj_writer* w, int binary, pd_writer fn, void* user);
void pj_obj_begin(pj_writer* w);
void pj_obj_end(pj_writer* w);
void pj_arr_begin(pj_writer* w);
void pj_arr_end(pj_writer* w);
void pj_key(pj_writer* w, const char* key);
void pj_null(pj_writer* w);
void pj_bool(pj_writer* w, int v);
void pj_int(pj_writer* w, int64_t v);
void pj_str(pj_writer* w, const char* s, size_t len);
void pj_cstr(pj_writer* w, const char* s);
/* a byte stream: base64 string in JSON, raw `H` payload in BJData */
void pj_bytes(pj_writer* w, const void* data, size_t len);
/* a rows x cols integer matrix: nested arrays in JSON, one optimized N-D array in BJData */
void pj_int_matrix(pj_writer* w, const int64_t* data, int64_t rows, int32_t cols);
/* flush; returns 0 on success */
int  pj_finish(pj_writer* w);

/* ------------------------------------------------------------------ */
/* reader                                                             */
/* ------------------------------------------------------------------ */

enum {
    PJ_NULL = 0,
    PJ_BOOL = 1,
    PJ_INT = 2,         /* int64 in i */
    PJ_UINT = 3,        /* uint64 above INT64_MAX, in u */
    PJ_FLOAT = 4,
    PJ_STR = 5,         /* UTF-8, s/len, NUL-terminated */
    PJ_BYTES = 6,       /* raw bytes from a BJData `H` value */
    PJ_ARR = 7,
    PJ_OBJ = 8
};

typedef struct pj_node pj_node;

struct pj_node {
    int type;
    int64_t i;
    uint64_t u;
    double f;
    const char* s;
    size_t len;
    const char* key;    /* member name inside an object */
    size_t keylen;
    pj_node* child;     /* first element or member */
    pj_node* next;      /* next sibling */
    int32_t n;          /* number of children */
};

typedef struct pj_doc pj_doc;

/* parse JSON text (binary = 0) or BJData (binary = 1); NULL on malformed input */
pj_doc*        pj_parse(const void* data, size_t len, int binary, const char** error);
void           pj_free(pj_doc* d);
const pj_node* pj_root(const pj_doc* d);

/* lookups; NULL when absent */
const pj_node* pj_get(const pj_node* obj, const char* key);
const pj_node* pj_at(const pj_node* arr, int32_t index);

/* typed accessors with defaults */
int     pj_is_int(const pj_node* n);
int64_t pj_int_or(const pj_node* n, int64_t def);
/* the bytes of a JData byte stream node: a base64 string, raw bytes, or
   {"_DataInfo_":..., "Data": ...}; *out is malloc'ed, caller frees */
int     pj_stream_bytes(const pj_node* n, unsigned char** out, size_t* len);

/* base64 (RFC 4648), as in mmm_base64.hpp; decode skips non-alphabet bytes */
char*   pj_base64_encode(const void* data, size_t len, size_t* outlen);
int     pj_base64_decode(const char* text, size_t len, unsigned char** out, size_t* outlen);

#endif /* PD_JSON_H */
