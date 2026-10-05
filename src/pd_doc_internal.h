/*
 * Parade document model internals
 */

#ifndef PD_DOC_INTERNAL_H
#define PD_DOC_INTERNAL_H

#include "pd_internal.h"
#include "parade_doc.h"

#define PD_ROOT_ID 1            /* the main tree */
#define PD_STORYROOT_ID 2       /* hidden container of all STORY blocks */
#define PD_MAX_BLOCKS 0x10000000

/* an inline object; obj.source points at the owned copy */
typedef struct {
    uint32_t offset;
    pd_inline obj;
    char* source;
} dinline;

/* everything about a block that an operation can change; snapshotted for undo */
typedef struct {
    int32_t role, level;        /* level: heading level */
    pd_style_id style;
    pd_list_id list;
    int32_t list_level;
    int32_t break_kind;
    pd_para_props pp;           /* direct paragraph properties */
    char* text;
    uint32_t len, cap;
    pd_run* runs;
    int32_t nruns, caprun;
    dinline* inl;
    int32_t ninl, capinl;
    pd_format_id empty_format;  /* format for typing into the empty paragraph */
    pd_float_props fp;
    pd_section_props sp;
} bstate;

typedef struct {
    int32_t kind;
    pd_block_id id, parent;
    pd_block_id* kids;
    int32_t nkids, capkids;
    int alive;                  /* attached to the tree */
    uint64_t born;              /* serial of the undo step that created it */
    bstate st;
    uint64_t revision;
} blk;

typedef struct {
    char name[64];
    int32_t kind;
    pd_style_id parent;
    pd_para_props pp;
    pd_char_props cp;
    int alive;
} dstyle;

typedef struct {
    pd_style_id style;
    pd_char_props cp;
} dformat;

typedef struct {
    int32_t n;
    pd_list_level lv[9];
} dlist;

typedef struct {
    char mime[64];
    unsigned char* data;
    size_t len;
} dres;

typedef struct {
    pd_pos pos;
    int32_t gravity;
    int alive;
} dmarker;

enum {
    UR_STATE = 0,               /* swap a block's state */
    UR_ATTACH = 1,              /* attach <-> detach a subtree */
    UR_STYLE = 2                /* swap a style definition */
};

typedef struct {
    int type;
    pd_block_id id;
    bstate saved;               /* UR_STATE */
    int attached;               /* UR_ATTACH: current state in the document */
    pd_block_id parent;
    int32_t index;
    pd_style_id style;          /* UR_STYLE */
    dstyle sdef;
} urec;

typedef struct {
    urec* r;
    int32_t n, cap;
    char label[64];
    uint64_t serial;
} ustep;

typedef struct {
    int32_t kind;
    pd_block_id block;
    pd_style_id style;
} dtouch;

struct pd_doc {
    blk** tab;                  /* by id; NULL = freed */
    uint32_t next_id, captab;
    dstyle* styles;
    int32_t nstyles, capstyles;
    dformat* formats;
    int32_t nformats, capformats;
    dlist* lists;
    int32_t nlists, caplists;
    dres* res;
    int32_t nres, capres;
    dmarker* markers;
    int32_t nmarkers, capmarkers;
    pd_font_resolver resolver;
    const pd_font** fallback;   /* fonts tried, in order, for characters the run's font lacks */
    int32_t nfallback;
    struct {
        char lang[16];
        const pd_hyph* hyph;
    }* hyphs;                   /* hyphenation patterns by language tag prefix */
    int32_t nhyphs;
    void* resolver_user;
    const pd_font* default_font;
    pd_doc_listener listener;
    void* listener_user;
    uint64_t revision;
    uint64_t style_rev;         /* bumped by every style change, including undo/redo */
    /* undo */
    ustep* undo;
    int32_t nundo, capundo;
    ustep* redo;
    int32_t nredo, capredo;
    int32_t undo_limit;
    int32_t group_depth;
    char group_label[64];
    uint64_t serial;            /* last step serial handed out */
    ustep* cur;                 /* step receiving records */
    int typing_open;            /* the top undo step may absorb more typing */
    pd_block_id typing_block;
    uint32_t typing_end;
    dtouch* touched;
    int32_t ntouched, captouched;
    int in_op;
};

/* shared by pd_doc.c and pd_doc_io.c */
blk*      pd_doc_blk(const pd_doc* d, pd_block_id id);   /* alive block or NULL */
blk*      pd_doc_new_blk(pd_doc* d, int32_t kind);
void      pd_doc_free_blk(blk* b);
int       pd_doc_add_kid(blk* parent, pd_block_id kid, int32_t index);
void      pd_doc_bstate_init(bstate* s, int32_t kind);
int       pd_doc_child_allowed(int32_t parent_kind, int32_t kid_kind);
int       pd_doc_utf8_valid(const char* s, size_t len, int allow_obj);
void      pd_doc_cp_normalize(pd_char_props* cp);
void      pd_doc_pp_normalize(pd_para_props* pp);
pd_doc*   pd_doc_alloc(void);
void      pd_doc_install_builtin_styles(pd_doc* d);
int       pd_doc_intern_raw(pd_doc* d, const dformat* f, pd_format_id* out);
void      pd_doc_format_number(int32_t v, int32_t fmt, char* buf, size_t cap);
/* a paragraph's effective properties: style chain, direct properties, list indent */
void      pd_doc_effective_pp(const pd_doc* d, const blk* b, pd_para_props* pp, pd_sp* label_x);
/* the text of a field or footnote mark if known yet (returns 1), for sizing it */
typedef int (*pd_field_fn)(void* user, const blk* b, const dinline* q, char* buf, size_t cap);
pd_status pd_doc_para_build_ex(const pd_doc* d, pd_block_id para, pd_sp column, pd_para* out, pd_params* prm,
                               pd_field_fn fn, void* user);
/* the document a layout shows (pd_layout.c) */
struct pd_layout;
const pd_doc* pd_layout_doc(const struct pd_layout* L);

/* the paragraph-engine style (font through the resolver) of a format in a paragraph */
pd_status pd_doc_run_style(const pd_doc* d, pd_block_id para, pd_format_id fmt, pd_style* st, pd_char_props* cp);

#define PD_CP_ALL ((1u << 14) - 1)
#define PD_PP_ALL ((1u << 18) - 1)

#endif /* PD_DOC_INTERNAL_H */
