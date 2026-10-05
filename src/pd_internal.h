/*
 * Parade internal definitions, not part of the public API
 */

#ifndef PD_INTERNAL_H
#define PD_INTERNAL_H

#include "parade.h"

/* ------------------------------------------------------------------ */
/* fonts                                                              */
/* ------------------------------------------------------------------ */

struct pd_font {
    uint8_t* data;
    size_t len;
    pd_font_metrics m;
    uint32_t num_hmetrics;
    uint32_t hmtx;          /* table offsets, 0 = absent */
    uint32_t cmap_sub;      /* selected cmap subtable */
    int32_t cmap_format;    /* 4 or 12 */
    int32_t cmap_symbol;    /* (3,0) symbol encoding: also try U+F000 + c */
    uint32_t kern_pairs;    /* first pair of a format-0 kern subtable */
    uint32_t kern_npairs;
    uint32_t* gpos_sub;     /* absolute offsets of PairPos subtables, in lookup order */
    uint16_t* gpos_lookup;  /* lookup index of each subtable */
    int32_t gpos_nsub;
};

/* ------------------------------------------------------------------ */
/* paragraph content: TeX-style items                                 */
/* ------------------------------------------------------------------ */

enum {
    PD_ITEM_BOX = 0,        /* glyph run or object: rigid */
    PD_ITEM_GLUE = 1,       /* flexible space, breakable after a box */
    PD_ITEM_PENALTY = 2     /* breakpoint with a cost */
};

#define PD_INF_PENALTY 10000
#define PD_FLAG_FLAGGED 1   /* penalty: hyphenation point */
#define PD_FLAG_OBJECT 2    /* box: inline object, not glyphs */
#define PD_FLAG_NOBREAK 4   /* glue: never a breakpoint (no-break space) */
#define PD_FLAG_SOFTHYPHEN 8 /* penalty: value is pd_params.hyphen_penalty */
#define PD_FLAG_EXHYPHEN 16 /* penalty: value is pd_params.ex_hyphen_penalty */
#define PD_FLAG_FINAL 32    /* paragraph-end items appended by pd_para_break */

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint8_t stretch_order;  /* glue: 0 finite, 1 fil */
    uint8_t pad;
    int32_t width;
    int32_t stretch;
    int32_t shrink;
    int32_t penalty;
    int32_t style;          /* -1 if none */
    int32_t height, depth;  /* boxes */
    int32_t user;           /* objects */
    uint32_t text_start, text_end;
    uint32_t glyph_start, glyph_count; /* boxes; penalty: hyphen glyph if count = 1 */
} pd_item;

typedef struct {
    uint32_t glyph;
    uint32_t cluster;
    int32_t advance;        /* sp, kerning folded in */
} pd_gl;

/* one chosen line */
typedef struct {
    int32_t start, end;     /* item range [start, end): content; end = break item */
    int32_t brk;            /* break item index (== end), n_items-1 for the final break */
    int32_t fitness;
    pd_line pub;
    int64_t slack;          /* width - natural, distributed over glue */
    int64_t total_stretch[2];
    int64_t total_shrink;
    int64_t demerits;
} pd_lineinfo;

/* dynamic-programming state per breakpoint, per fitness class, per line class */
typedef struct {
    int64_t total;
    int32_t prev;           /* index into the state array, -1 = start */
    int32_t pad;
} pd_state;

struct pd_para {
    /* current content */
    pd_item* items;
    int32_t n_items, cap_items;
    pd_gl* glyphs;
    int32_t n_glyphs, cap_glyphs;
    char* text;
    uint32_t n_text;
    int32_t cap_text;
    pd_style* styles;
    int32_t n_styles, cap_styles;
    /* previous content, kept by pd_para_clear for incremental breaking */
    pd_item* old_items;
    int32_t n_old_items, cap_old_items;
    char* old_text;
    uint32_t n_old_text;
    int32_t cap_old_text;
    /* shape */
    pd_sp* shape_indent;
    pd_sp* shape_width;
    int32_t n_shape;
    int32_t shape_dirty;
    /* breaking cache */
    pd_params last_params;
    int32_t have_cache;
    int32_t* bp_item;       /* breakpoint -> item index */
    int32_t n_bp, cap_bp;
    pd_state* states;       /* n_bp * 4 * line_classes */
    int32_t cap_states;
    int32_t line_classes;
    int64_t* sum_w, *sum_st, *sum_fil, *sum_sh; /* prefix sums over items */
    int32_t cap_sums;
    /* result */
    pd_lineinfo* lines;
    int32_t n_lines, cap_lines;
    pd_lineinfo* old_lines;
    int32_t n_old_lines, cap_old_lines;
    int32_t broken;
    int32_t finalized;      /* paragraph-end items are appended */
    pd_sp height;
};

/* growth helper: ensure *cap >= need for an array of elem-sized entries */
int pd_grow(void** ptr, int32_t* cap, int64_t need, size_t elem);

/* scale design units to sp at the given size, round half away from zero */
static inline int32_t pd_scale(int32_t units, pd_sp size, int32_t upem) {
    int64_t v = (int64_t)units * size;
    return (int32_t)(v >= 0 ? (v + upem / 2) / upem : -((-v + upem / 2) / upem));
}

/* penalty value of an item under the given parameters */
static inline int32_t pd_item_penalty(const pd_item* it, const pd_params* prm) {
    if (it->flags & PD_FLAG_SOFTHYPHEN) {
        return prm->hyphen_penalty;
    }

    if (it->flags & PD_FLAG_EXHYPHEN) {
        return prm->ex_hyphen_penalty;
    }

    return it->penalty;
}

/* line breaking, implemented in pd_break.c */
pd_status pd_break_lines(pd_para* p, const pd_params* prm, pd_break_info* info);

#endif /* PD_INTERNAL_H */
