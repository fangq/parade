/*
 * Parade page builder
 *
 * 1. Every paragraph is laid out with the paragraph engine at the width it
 *    will occupy (column, float or text width) and cached by block,
 *    keyed by its revision, its width and the style revision.
 * 2. A section's flow becomes a vertical list: one box per line (its band
 *    from line top to next line top, so boxes stack exactly), glue
 *    between paragraphs (collapsed spacing, dropped at a column top) and
 *    penalties carrying widow, orphan, keep-lines and keep-with-next.
 * 3. Columns are filled TeX-style: when a line no longer fits, the column
 *    breaks at the cheapest earlier candidate (penalty + cubic badness of
 *    the space left), and filling resumes after it.
 * 4. Floats go here if they fit, else into a queue: they land at the top
 *    of a later column, at the bottom of a page that already shows their
 *    anchor, or on a float page, in order; the queue is flushed at the
 *    section end.
 * 5. Footnote bodies referenced by a line ride with it: the column keeps
 *    room for them at its bottom, below a short rule.
 * 6. Tables become one unbreakable box per row (column widths from the
 *    cell contents, CSS-style); header rows repeat after every break.
 * 7. A float with wrap sits at its anchor and the following paragraphs
 *    are re-broken with a narrower \parshape beside it.
 * 8. Continuous sections start below the previous one on the same page;
 *    a multi-column section ending there has its last columns balanced.
 * 9. Optimal page breaking chooses all column breaks of a section at once
 *    and may set paragraphs a line looser or tighter (Mittelbach's
 *    paragraph variants) to avoid short pages.
 * 10. Headers and footers are placed per page; fields are filled in when
 *    the display list is generated.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_doc_internal.h"
#include "pd_json.h"
#include "parade_layout.h"

#define INF_PEN 10000
#define INTERLINE_PEN 0          /* TeX's \\interlinepenalty: widows and orphans are rules, not costs */
#define TOP_FRACTION 700        /* per-mille of a column that top floats may take */
#define STUCK_PAGES 2           /* a float waiting longer than this is forced out */
#define FN_RULE_WIDTH PD_PT(144)    /* the footnote rule: 2in by 0.4pt, as in LaTeX */
#define FN_RULE_HEIGHT PD_PT(0.4)
#define VARIANT_DEMERITS 2000       /* cost of setting a paragraph a line looser or tighter */
#define VARIANT_TRIALS 48           /* paragraph variants tried per section */
#define DEM_HUGE ((int64_t)1 << 56)

/* ------------------------------------------------------------------ */
/* data                                                               */
/* ------------------------------------------------------------------ */

typedef struct pcache {         /* one paragraph laid out at one width */
    pd_block_id block;
    uint64_t revision, style_rev, epoch;
    pd_sp width;
    pd_para* para;
    int32_t nlines;
    pd_sp height;
    pd_sp* top;                 /* nlines + 1 entries: line tops, then the height */
    pd_para_props pp;           /* effective properties */
    pd_sp label_x;
    char label[32];             /* list label, recomputed every update */
    uint64_t fieldsig;          /* hash of the field values the layout was sized with */
    int used;
    int note;                   /* label is a footnote mark (set before the text) */
    int32_t loose;              /* \looseness of this variant */
    pd_sp ws_w;                 /* wrap shape: lines ws_k0 to ws_k (not that one) are ws_w narrower */
    int32_t ws_side, ws_k, ws_k0;
    struct pcache* next;        /* other layouts (variants, shapes) of the same paragraph */
} pcache;

typedef struct {                /* lines narrowed beside a wrapping float */
    pd_sp w;
    int32_t side;               /* PD_WRAP_LEFT: the float is on the left */
    int32_t k;                  /* the lines before this one ... */
    int32_t k0;                 /* ... from this one (the float lower down the paragraph) */
} wrapshape;

typedef struct {                /* a filled rectangle: rules, borders, cell backgrounds */
    pd_sp x, y, w, h;
    uint32_t color;
    int32_t region;
    pd_block_id block;
} prule;

typedef struct {                /* a line (or a whole paragraph region) on a page */
    pcache* pc;
    int32_t line;
    pd_sp ox, oy;               /* paragraph origin in page coordinates */
    pd_sp top, bottom;          /* band on the page */
    int32_t region;
} pline;

typedef struct {
    pd_sp w, h;
    pd_block_id section;
    int32_t number, section_index;
    char label[16];
    int float_page;
    pline* lines;
    int32_t n, cap;
    pcache** owned;             /* header/footer paragraphs, laid out with this page's field values */
    int32_t nowned, capowned;
    pd_block_id heading[7];     /* last heading of each level at the end of the page */
    pd_sp text_x, text_w;       /* text area, for headers and footers */
    const pd_section_props* sp;
    prule* rules;
    int32_t nrules, caprules;
    int32_t lnum_first;         /* line numbering: lines counted before this page's first, in its scope */
    pd_sp* pts;                 /* the points of the paths last listed for the page */
    pd_draw* items;             /* the page's draw list, made on the first listing and kept until the page is
                                   laid out again: a drawing's text boxes are laid out to make it */
    int32_t nitems;
    int items_ok;
} ppage;

typedef struct {
    pd_block_id block, offset;
    int32_t value;
} fieldval;

enum {
    VI_LINE = 0,
    VI_GLUE = 1,
    VI_PEN = 2,
    VI_FLOAT = 3,
    VI_BREAK = 4,
    VI_ROW = 5,
    VI_RULE = 6                 /* a horizontal rule (PD_BREAK_RULE): a line of its own height */
};

#define RULE_H PD_PT(12)        /* the space a horizontal rule takes, the rule in its middle */

typedef struct {
    int32_t kind;
    pd_sp h;
    int32_t pen;
    pcache* pc;
    int32_t line;               /* lines: line index; floats: float index; rows: row index */
    pd_block_id block;          /* floats, rows */
    int32_t brk;
    int32_t tbl;                /* rows: table index */
    int32_t part;               /* rows: 0 whole; a slice of a row taller than a column: 1 first, 2 middle, 3 last */
    pd_sp from;                 /* rows, a slice: where in the row it starts */
    pd_sp fn_h;                 /* footnote bodies referenced here */
    int32_t fn_first, fn_n;     /* their stories in filler.notes */
} vitem;

typedef struct {                /* a table: column grid and header rows */
    pd_block_id block;
    int32_t ncols, header_rows;
    pd_sp x, width;             /* offset in the column, total width */
    pd_sp colx[PD_TABLE_MAX_COLS + 1];
    int32_t* hdr_item;          /* vitem index of each header row */
    pd_sp* rowh;                /* every row's height, for cells merged across rows */
    int32_t nrows;
    pd_sp header_h;
    pd_table_props tp;
} ptable;

enum {
    FL_NONE = 0,
    FL_QUEUED = 1,
    FL_PLACED = 2
};

typedef struct {
    pd_block_id block;
    int32_t item;               /* index of its anchor in the vertical list */
    int32_t state, queued_page;
    pd_float_props fp;
    pd_sp w, h;                 /* content box, gap excluded */
} pfloat;

struct pd_layout {
    const pd_doc* doc;
    pcache** cache;             /* by block id */
    uint32_t ncache;
    uint64_t epoch;             /* bumped by pd_layout_invalidate */
    ppage* pages;
    int32_t npages, cappages;
    fieldval* fv;
    int32_t nfv, capfv;
    pd_block_id* endnotes;      /* endnote bodies in reading order: they follow the last section */
    int32_t nendnotes, capendnotes;
    int32_t* first_page;        /* by block id: first page showing it, -1 = none */
    pd_para* scratch;           /* shaping of labels and field values */
    pd_layout_info info;
    /* the previous pass, for fields that need pagination (page references, page count) */
    int32_t* prev_first_page;
    char (*prev_labels)[16];
    int32_t prev_npages;
    int want_rerun;
    struct {                    /* where floats placed from the page's top were anchored, the last time */
        pd_block_id b;
        pd_sp y;
    } *fly;
    int32_t nfly, capfly;
    int32_t stable;             /* hybrid line breaking: -1 as the document says, 0 off, 1 on */
    int32_t dtext_depth;        /* text boxes of drawings being placed, one inside another */
};

/* filler state for one section */
typedef struct {
    pd_layout* L;
    const blk* sec;
    const pd_section_props* sp;
    vitem* it;
    int32_t n, cap;
    pfloat* fl;
    int32_t nfl, capfl;
    int32_t* queue;             /* indices into fl, FIFO */
    int32_t nq, capq;
    int32_t ncols;
    pd_sp colw, colh, gap;
    int32_t page;               /* current page index */
    int32_t col;
    pd_sp top_used;             /* height taken by top floats in this column */
    int32_t section_page;
    int32_t number;
    pd_block_id* notes;         /* footnote stories, by reference order */
    int32_t nnotes, capnotes;
    ptable* tb;
    int32_t ntb, captb;
    pd_sp wrap_rem, wrap_w;     /* building: height still beside a wrapping float */
    pd_sp wrap_skip;            /* and before that, the height of text above it (a float lower down) */
    pd_sp gap_skip, gap_h;      /* a float across the column lower down: the text before it, then its room */
    int32_t wrap_side;
    int32_t floor_page;         /* continuous section: page shared with the previous section */
    pd_sp floor_y;              /* and the height its content takes there */
    uint8_t* chosen;            /* optimal page breaking: penalty items to break at */
    int8_t* loose;              /* optimal page breaking: \looseness by block id */
    int32_t page_start_item;    /* first item of the current page (balancing) */
    int32_t page_start_n, page_start_nrules;
    int resume;                 /* fill: continue on the current page */
    pd_block_id prev_para;      /* building: the paragraph just before, for the space between */
    pd_sp top;                  /* where the text starts: the top margin, or below a header taller than it */
} filler;

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int64_t badness(int64_t t, int64_t s) {
    int64_t r;

    if (t <= 0) {
        return 0;
    }

    if (s <= 0) {
        return 1000000;
    }

    r = t * 1000 / s;
    r = r > 100000 ? 100000 : r;
    r = r * r * r / 10000000;
    return r > 1000000 ? 1000000 : r;
}

static int grow(void** p, int32_t* cap, int64_t need, size_t elem) {
    return pd_grow(p, cap, need, elem);
}

static void pcache_free(pcache* c) {
    while (c) {
        pcache* nx = c->next;

        pd_para_free(c->para);
        free(c->top);
        free(c);
        c = nx;
    }
}

static void field_text(const pd_layout* L, const ppage* p, const blk* b, const dinline* q, char* buf, size_t cap);
static void collect_ids(const pd_doc* d, pd_block_id id, pd_block_id* out, int32_t* n, int32_t cap);

/* field values known while paragraphs are laid out: counters now, pages from the previous pass */
static int body_field(void* user, const blk* b, const dinline* q, char* buf, size_t cap) {
    pd_layout* L = (pd_layout*)user;
    const pd_inline* o = &q->obj;

    if (o->kind == PD_INLINE_FOOTNOTE || (o->kind == PD_INLINE_FIELD && (o->field == PD_FIELD_SEQ ||
                                          o->field == PD_FIELD_REF_NUMBER))) {
        field_text(L, NULL, b, q, buf, cap);
        return 1;
    }

    if (o->kind == PD_INLINE_FIELD && o->field == PD_FIELD_REF_PAGE) {
        pd_block_id ids[1];
        int32_t n = 0;

        L->want_rerun = 1;
        collect_ids(L->doc, o->target, ids, &n, 1);

        if (n && L->prev_first_page && ids[0] <= L->doc->captab && L->prev_first_page[ids[0]] >= 0 &&
                L->prev_first_page[ids[0]] < L->prev_npages) {
            snprintf(buf, cap, "%s", L->prev_labels[L->prev_first_page[ids[0]]]);
            return 1;
        }

        return 0;
    }

    if (o->kind == PD_INLINE_FIELD && o->field == PD_FIELD_PAGES) {
        L->want_rerun = 1;

        if (L->prev_npages > 0) {
            snprintf(buf, cap, "%d", (int)L->prev_npages);
            return 1;
        }
    }

    return 0;   /* page-dependent in the body: placeholder */
}

typedef struct {
    pd_layout* L;
    int32_t page;
} page_ctx;

static int page_field(void* user, const blk* b, const dinline* q, char* buf, size_t cap) {
    page_ctx* c = (page_ctx*)user;

    field_text(c->L, &c->L->pages[c->page], b, q, buf, cap);
    return 1;
}

/* a hash of the values a paragraph's fields would be sized with */
static uint64_t field_signature(pd_layout* L, const blk* b) {
    uint64_t h = 1469598103934665603ULL;
    int32_t i;

    for (i = 0; i < b->st.ninl; i++) {
        char v[96];
        const char* t;

        if (b->st.inl[i].obj.kind != PD_INLINE_FIELD && b->st.inl[i].obj.kind != PD_INLINE_FOOTNOTE) {
            continue;
        }

        if (!body_field(L, b, &b->st.inl[i], v, sizeof(v))) {
            strcpy(v, "\x01");
        }

        for (t = v; ; t++) {
            h = (h ^ (unsigned char) * t) * 1099511628211ULL;

            if (!*t) {
                break;
            }
        }
    }

    return h;
}

/* lay a paragraph out into c (no caching decisions here) */
static pd_status build_into(pd_layout* L, pcache* c, pd_block_id id, pd_sp width, pd_field_fn fn, void* user) {
    const pd_doc* d = L->doc;
    blk* b = pd_doc_blk(d, id);
    pd_params prm;
    pd_break_info bi;
    pd_status st;
    int32_t i;
    int again = c->nlines > 0 && c->width == width;     /* laid out before, at this width: an edit */

    c->block = id;
    c->width = width;
    c->used = 1;
    pd_doc_effective_pp(d, b, &c->pp, &c->label_x);
    st = pd_doc_para_build_ex(d, id, width, c->para, &prm, fn, user);

    if (st == PD_OK && c->ws_k > 0) {   /* lines beside a float: the first ones, or from ws_k0 */
        pd_sp ind[65], wid[65], w = width - c->pp.indent_left - c->pp.indent_right;
        int32_t k = c->ws_k < 64 ? c->ws_k : 64;

        for (i = 0; i <= k; i++) {
            pd_sp bi0 = i == 0 ? c->pp.indent_left + c->pp.indent_first : c->pp.indent_left;
            pd_sp bw = i == 0 ? w - c->pp.indent_first : w;
            int beside = i < k && i >= c->ws_k0;

            ind[i] = bi0 + (beside && c->ws_side == PD_WRAP_LEFT ? c->ws_w : 0);
            wid[i] = beside ? bw - c->ws_w : bw;
            wid[i] = wid[i] < PD_PT(12) ? PD_PT(12) : wid[i];
        }

        st = pd_para_set_shape(c->para, k + 1, ind, wid);
    }

    prm.looseness = c->loose;

    /* hybrid breaking: after an edit, the lines before it as they were, those after it kept unless a new
       break is much better; a paragraph's first layout (and the looser or tighter variants page breaking
       tries) optimal */
    if (again && c->loose == 0 && prm.mode == PD_BREAK_OPTIMAL &&
            (L->stable < 0 ? pd_doc_stable_breaks(d) : L->stable)) {
        prm.freeze_offset = INT32_MAX;
        prm.hysteresis = 5000;
    }

    if (st == PD_OK) {
        st = pd_para_break(c->para, &prm, &bi);
    }

    if (st != PD_OK) {
        return st;
    }

    c->nlines = bi.lines;
    c->height = bi.height;
    free(c->top);
    c->top = (pd_sp*)malloc(((size_t)c->nlines + 1) * sizeof(pd_sp));

    if (!c->top) {
        return PD_ERR_NOMEM;
    }

    for (i = 0; i < c->nlines; i++) {
        pd_line ln;

        pd_para_get_line(c->para, i, &ln);
        c->top[i] = i == 0 ? 0 : ln.baseline - ln.ascent;
    }

    c->top[c->nlines] = c->height;

    for (i = 1; i <= c->nlines; i++) {     /* bands never run backwards */
        if (c->top[i] < c->top[i - 1]) {
            c->top[i] = c->top[i - 1];
        }
    }

    L->info.paragraphs_broken++;
    return PD_OK;
}

/* the cached layout of a paragraph at a width, (re)built when stale; variants by looseness and wrap shape */
static pcache* layout_para_ex(pd_layout* L, pd_block_id id, pd_sp width, int32_t loose, const wrapshape* ws,
                              pd_status* st) {
    const pd_doc* d = L->doc;
    blk* b = pd_doc_blk(d, id);
    pcache* c;
    uint64_t sig;
    wrapshape none = { 0, 0, 0, 0 };

    if (!ws || ws->k <= 0) {
        ws = &none;
    }

    if (!b || b->kind != PD_BLOCK_PARAGRAPH || width <= 0) {
        *st = PD_ERR_ARG;
        return NULL;
    }

    if (id >= L->ncache) {
        uint32_t nc = d->captab > id ? d->captab : id + 1;
        pcache** t = (pcache**)realloc(L->cache, nc * sizeof(pcache*));

        if (!t) {
            *st = PD_ERR_NOMEM;
            return NULL;
        }

        memset(t + L->ncache, 0, (nc - L->ncache) * sizeof(pcache*));
        L->cache = t;
        L->ncache = nc;
    }

    for (c = L->cache[id]; c && !(c->loose == loose && c->ws_k == ws->k && c->ws_w == ws->w &&
                                  c->ws_side == ws->side && c->ws_k0 == ws->k0); c = c->next) {
    }

    sig = field_signature(L, b);

    if (c && c->revision == b->revision && c->style_rev == d->style_rev && c->width == width &&
            c->epoch == L->epoch && c->fieldsig == sig) {
        c->used = 1;
        L->info.paragraphs_reused++;
        return c;
    }

    if (!c) {
        pcache** tail;

        c = (pcache*)calloc(1, sizeof(pcache));

        if (!c || pd_para_new(&c->para) != PD_OK) {
            free(c);
            *st = PD_ERR_NOMEM;
            return NULL;
        }

        c->loose = loose;
        c->ws_w = ws->w;
        c->ws_side = ws->side;
        c->ws_k = ws->k;
        c->ws_k0 = ws->k0;

        for (tail = &L->cache[id]; *tail; tail = &(*tail)->next) {
        }

        *tail = c;  /* the plain layout comes first in the chain */
    }

    c->revision = b->revision;
    c->style_rev = d->style_rev;
    c->epoch = L->epoch;
    c->fieldsig = sig;
    *st = build_into(L, c, id, width, body_field, L);

    if (*st != PD_OK) {
        c->revision = (uint64_t) -1;
        return NULL;
    }

    return c;
}

static pcache* layout_para(pd_layout* L, pd_block_id id, pd_sp width, pd_status* st) {
    return layout_para_ex(L, id, width, 0, NULL, st);
}

static ppage* new_page(pd_layout* L, const blk* sec, int32_t number, int32_t section_index) {
    ppage* p;
    const pd_section_props* sp = &sec->st.sp;
    int even;

    if (grow((void**)&L->pages, &L->cappages, (int64_t)L->npages + 1, sizeof(ppage))) {
        return NULL;
    }

    p = &L->pages[L->npages++];
    memset(p, 0, sizeof(*p));
    p->w = sp->page_width;
    p->h = sp->page_height;
    p->section = sec->id;
    p->number = number;
    p->section_index = section_index;
    p->sp = sp;
    pd_doc_format_number(number, sp->page_number_format, p->label, sizeof(p->label));
    /* margins: mirrored on even pages of a two-sided document; the gutter on the inside */
    even = (sp->mirror_margins > 0 || (sp->mirror_margins == 0 && sp->facing_pages)) && number % 2 == 0;
    p->text_x = even ? sp->margin_right : sp->margin_left + sp->gutter;
    p->text_w = sp->page_width - sp->margin_left - sp->margin_right - sp->gutter;

    if (L->npages > 1) {
        memcpy(p->heading, L->pages[L->npages - 2].heading, sizeof(p->heading));
    }

    return p;
}

static int add_rule(pd_layout* L, int32_t page, pd_sp x, pd_sp y, pd_sp w, pd_sp h, uint32_t color, int32_t region,
                    pd_block_id block) {
    ppage* p = &L->pages[page];
    prule* r;

    if (w <= 0 || h <= 0 || grow((void**)&p->rules, &p->caprules, (int64_t)p->nrules + 1, sizeof(prule))) {
        return -1;
    }

    r = &p->rules[p->nrules++];
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->color = color;
    r->region = region;
    r->block = block;
    return 0;
}

static void place_stack(pd_layout* L, pd_block_id container, pd_sp width, int32_t page, pd_sp x, pd_sp y,
                        int32_t region);
static pd_sp stack_height(pd_layout* L, pd_block_id container, pd_sp width, pd_status* st);
static void place_drawing_text(pd_layout* L, int32_t page, const pcache* pc, int32_t line, pd_sp ox, pd_sp oy);
static pd_sp jsp(const pj_node* o, const char* k);

/* whether N bytes of DATA hold the K bytes of KEY */
static int pd_memfind(const void* data, size_t n, const char* key, size_t k) {
    const char* p = (const char*)data;
    size_t i;

    for (i = 0; k && i + k <= n; i++) {
        if (p[i] == key[0] && !memcmp(p + i, key, k)) {
            return 1;
        }
    }

    return 0;
}

static int add_line(pd_layout* L, int32_t page, pcache* pc, int32_t line, pd_sp ox, pd_sp oy, int32_t region) {
    ppage* p = &L->pages[page];
    pline* l;

    if (grow((void**)&p->lines, &p->cap, (int64_t)p->n + 1, sizeof(pline))) {
        return -1;
    }

    l = &p->lines[p->n++];
    l->pc = pc;
    l->line = line;
    l->ox = ox;
    l->oy = oy;
    l->top = oy + pc->top[line];
    l->bottom = oy + pc->top[line + 1];
    l->region = region;

    if (region == 0 || region == 3 || region == 4) {
        blk* b = pd_doc_blk(L->doc, pc->block);

        if (pc->block <= L->doc->captab && L->first_page[pc->block] < 0) {
            L->first_page[pc->block] = page;
        }

        if (b && b->st.role == PD_ROLE_HEADING && line == 0 && b->st.level >= 1 && b->st.level <= 6) {
            int32_t k;

            p->heading[b->st.level] = pc->block;

            for (k = b->st.level + 1; k <= 6; k++) {
                p->heading[k] = 0;
            }
        }
    }

    place_drawing_text(L, page, pc, line, ox, oy);
    return 0;
}

/* The text boxes of the drawings in a line just placed: each one's story laid out where the drawing puts the box,
   as lines of the page (region 5) -- drawn, found by a click and edited as any text is. Their insets, and the
   box's anchor (top, middle, bottom), as the drawing says. */
static void place_drawing_text(pd_layout* L, int32_t page, const pcache* pc, int32_t line, pd_sp ox, pd_sp oy) {
    const blk* b = pd_doc_blk(L->doc, pc->block);
    pd_glyph* g;
    int32_t n = 0, i, k, any = 0;

    if (!b || b->st.ninl == 0 || L->dtext_depth >= 3) {
        return;
    }

    for (k = 0; k < b->st.ninl && !any; k++) {  /* a drawing with a story in it, or nothing to do */
        const char* mime = NULL;
        const void* data = NULL;
        size_t len = 0;

        any = b->st.inl[k].obj.kind == PD_INLINE_IMAGE &&
              pd_doc_resource(L->doc, b->st.inl[k].obj.resource, &mime, &data, &len) == PD_OK &&
              !strcmp(mime, "application/vnd.parade.drawing+json") && pd_memfind(data, len, "\"story\":", 8);
    }

    if (!any || pd_para_get_glyphs(pc->para, line, NULL, 0, &n) != PD_OK || n <= 0 ||
            (g = (pd_glyph*)malloc((size_t)n * sizeof(pd_glyph))) == NULL) {
        return;
    }

    pd_para_get_glyphs(pc->para, line, g, n, &n);

    for (i = 0; i < n; i++) {
        const dinline* q = g[i].kind == PD_OBJECT && g[i].user >= 0 && g[i].user < b->st.ninl ? &b->st.inl[g[i].user] :
                           NULL;
        const char* mime = NULL;
        const void* data = NULL;
        size_t len = 0;
        pj_doc* jd;
        const pj_node* root, *it;
        pd_sp iw, ih, x0, y0;
        double sx, sy;

        if (!q || q->obj.kind != PD_INLINE_IMAGE ||
                pd_doc_resource(L->doc, q->obj.resource, &mime, &data, &len) != PD_OK ||
                strcmp(mime, "application/vnd.parade.drawing+json") || !pd_memfind(data, len, "\"story\":", 8)) {
            continue;
        }

        pd_doc_image_size(L->doc, &q->obj, &iw, &ih);
        x0 = ox + g[i].x;
        y0 = oy + g[i].y - ih;
        jd = pj_parse(data, len, 0, NULL);
        root = jd ? pj_root(jd) : NULL;
        sx = root && jsp(root, "w") > 0 ? (double)iw / jsp(root, "w") : 1;
        sy = root && jsp(root, "h") > 0 ? (double)ih / jsp(root, "h") : 1;

        for (it = root && pj_get(root, "items") ? pj_get(root, "items")->child : NULL; it; it = it->next) {
            const pj_node* ins = pj_get(it, "ins"), *an = pj_get(it, "anchor");
            pd_block_id story = (pd_block_id)pj_int_or(pj_get(it, "story"), 0);
            pd_sp bx = x0 + (pd_sp)(jsp(it, "x") * sx), by = y0 + (pd_sp)(jsp(it, "y") * sy);
            pd_sp bw = (pd_sp)(jsp(it, "w") * sx), bh = (pd_sp)(jsp(it, "h") * sy);
            pd_sp il = (pd_sp)(pj_int_or(pj_at(ins, 0), 0) * sx), it_ = (pd_sp)(pj_int_or(pj_at(ins, 1), 0) * sy);
            pd_sp ir = (pd_sp)(pj_int_or(pj_at(ins, 2), 0) * sx), ib = (pd_sp)(pj_int_or(pj_at(ins, 3), 0) * sy);
            pd_sp width = bw - il - ir, top = by + it_, h;
            pd_status st;

            if (!story || !pd_doc_blk(L->doc, story) || width <= 0) {
                continue;
            }

            if (an && an->type == PJ_STR && an->len >= 1 && (an->s[0] == 'c' || an->s[0] == 'b')) {
                h = stack_height(L, story, width, &st);
                top = an->s[0] == 'c' ? by + (bh - h) / 2 : by + bh - ib - h;
            }

            L->dtext_depth++;
            place_stack(L, story, width, page, bx + il, top, 5);
            L->dtext_depth--;
        }

        pj_free(jd);
    }

    free(g);
}

/* all paragraphs of a container block, depth first (tables are stacked for now) */
static int collect_paras(const pd_doc* d, pd_block_id id, pd_block_id* out, int32_t* n, int32_t cap) {
    blk* b = pd_doc_blk(d, id);
    int32_t i;

    if (!b) {
        return 0;
    }

    if (b->kind == PD_BLOCK_PARAGRAPH) {
        if (*n < cap) {
            out[*n] = id;
        }

        (*n)++;
        return 0;
    }

    for (i = 0; i < b->nkids; i++) {
        collect_paras(d, b->kids[i], out, n, cap);
    }

    return 0;
}

static void collect_ids(const pd_doc* d, pd_block_id id, pd_block_id* out, int32_t* n, int32_t cap) {
    collect_paras(d, id, out, n, cap);
}

static int has_box(const pd_para_props* pp);
static int same_box(const pd_para_props* a, const pd_para_props* b);

/* the section a block is in; a story's (headers, notes): the first section */
static const pd_section_props* section_of(const pd_doc* d, pd_block_id id) {
    const blk* b = pd_doc_blk(d, id);
    int32_t hops;

    for (hops = 0; b && b->kind != PD_BLOCK_SECTION && b->parent && hops < 64; hops++) {
        b = pd_doc_blk(d, b->parent);
    }

    if (!b || b->kind != PD_BLOCK_SECTION) {
        const blk* root = pd_doc_blk(d, PD_ROOT_ID);

        b = root && root->nkids > 0 ? pd_doc_blk(d, root->kids[0]) : NULL;
    }

    return b && b->kind == PD_BLOCK_SECTION ? &b->st.sp : NULL;
}

/* The space between a paragraph and the one before it: the larger of the
   one's space after and the other's before, or both (a word processor's
   way, the section says which); none of either's that it leaves out next to
   a paragraph of its own style (contextual spacing). */
static pd_sp para_gap(const pd_doc* d, pd_block_id prev, pd_sp prev_after, pd_block_id cur, const pd_para_props* pp) {
    const blk* a = prev ? pd_doc_blk(d, prev) : NULL, *b = pd_doc_blk(d, cur);
    const pd_section_props* sp = section_of(d, cur);
    pd_sp before = pp->space_before, room = 0;
    pd_para_props ap;
    int top = pp->border_color && pp->border_width > 0 &&
              (!pp->border_sides || (pp->border_sides & PD_BORDER_TOP));

    if (a && b && a->kind == PD_BLOCK_PARAGRAPH) {
        pd_style_id normal = pd_doc_style_find(d, "Normal");
        pd_style_id sa = a->st.style ? a->st.style : normal, sb = b->st.style ? b->st.style : normal;

        pd_doc_effective_pp(d, a, &ap, NULL);

        if (sa == sb && pp->contextual) {
            before = 0;
        }

        if (sa == sb && ap.contextual) {
            prev_after = 0;
        }

        /* borders take room: a box's top and bottom edges and their space, a rule between */
        if (has_box(&ap) && same_box(&ap, pp)) {
            top = 0;
            room = (pp->border_sides & PD_BORDER_BETWEEN) && pp->border_color ? pp->border_width : 0;
        } else if (ap.border_color && ap.border_width > 0 && (!ap.border_sides || (ap.border_sides & PD_BORDER_BOTTOM))) {
            room = ap.border_width + ap.border_space;
        }
    }

    room += top ? pp->border_width + pp->border_space : 0;
    return room + (sp && sp->add_spacing ? before + prev_after : before > prev_after ? before : prev_after);
}

/* where a wrapped float goes across a column of width colw (*ox, from its
   left edge) and how much of the column's width it takes from the text */
static pd_sp float_taken(const pd_float_props* fp, pd_sp w, pd_sp colw, pd_sp* ox) {
    pd_sp x = (fp->placement & PD_PLACE_OFFSET) ? fp->offset_x : fp->wrap == PD_WRAP_LEFT ? 0 : colw - w;
    pd_sp t = fp->wrap == PD_WRAP_LEFT ? x + w + fp->gap : colw - x + fp->gap;

    *ox = x;
    return t < 0 ? 0 : t > colw ? colw : t;
}

/* lay out the paragraphs of a block stack at a width; returns total height */
static pd_sp place_table_box(pd_layout* L, const blk* t, pd_sp width, int32_t page, pd_sp x, pd_sp y, int32_t region,
                             int draw, pd_status* st);

/* A block stack (a float's, a text box's, a cell's, a story's) from (x, y), or only measured (draw 0): its
   paragraphs, and its tables as tables -- a table's paragraphs are not the stack's. Returns its height. */
static pd_sp stack_walk(pd_layout* L, pd_block_id container, pd_sp width, int draw, int32_t page, pd_sp x, pd_sp y,
                        int32_t region, pd_status* st) {
    const blk* cb = pd_doc_blk(L->doc, container);
    pd_block_id ids[512], prev = 0;
    int32_t n, i, kk, k;
    pd_sp h = 0, prev_after = 0;

    if (!cb) {
        return 0;
    }

    for (kk = 0; kk < (cb->kind == PD_BLOCK_PARAGRAPH ? 1 : cb->nkids); kk++) {
        pd_block_id kid = cb->kind == PD_BLOCK_PARAGRAPH ? container : cb->kids[kk];
        const blk* kb = pd_doc_blk(L->doc, kid);

        if (kb && kb->kind == PD_BLOCK_TABLE) {
            pd_sp th = place_table_box(L, kb, width, page, x, y + h + (prev ? prev_after : 0), region, draw, st);

            if (th < 0) {
                return -1;
            }

            h += (prev ? prev_after : 0) + th;
            prev = 0;       /* what follows a table starts afresh */
            prev_after = 0;
            continue;
        }

        n = 0;
        collect_paras(L->doc, kid, ids, &n, 512);

        for (i = 0; i < n && i < 512; i++) {
            pcache* c = layout_para(L, ids[i], width, st);

            if (!c) {
                return -1;
            }

            if (prev) {
                h += para_gap(L->doc, prev, prev_after, ids[i], &c->pp);
            }

            for (k = 0; draw && k < c->nlines; k++) {
                add_line(L, page, c, k, x, y + h, region);
            }

            h += c->height;
            prev_after = c->pp.space_after;
            prev = ids[i];
        }
    }

    return h;
}

static pd_sp stack_height(pd_layout* L, pd_block_id container, pd_sp width, pd_status* st) {
    return stack_walk(L, container, width, 0, 0, 0, 0, 0, st);
}

/* place a block stack's paragraphs (and tables) from (x, y) */
static void place_stack(pd_layout* L, pd_block_id container, pd_sp width, int32_t page, pd_sp x, pd_sp y,
                        int32_t region) {
    pd_status st;

    stack_walk(L, container, width, 1, page, x, y, region, &st);
}

/* the lines of a container whose tops are in [lo, hi) of it (a slice of a table row): placed with the
   container's top at y */
static void place_stack_clip(pd_layout* L, pd_block_id container, pd_sp width, int32_t page, pd_sp x, pd_sp y,
                             pd_sp lo, pd_sp hi) {
    pd_block_id ids[512];
    int32_t n = 0, i, k;
    pd_sp prev_after = 0, py = 0;
    pd_status st;

    collect_paras(L->doc, container, ids, &n, 512);

    for (i = 0; i < n && i < 512; i++) {
        pcache* c = layout_para(L, ids[i], width, &st);

        if (!c) {
            return;
        }

        if (i > 0) {
            py += para_gap(L->doc, ids[i - 1], prev_after, ids[i], &c->pp);
        }

        for (k = 0; k < c->nlines; k++) {
            if (py + c->top[k] >= lo && py + c->top[k] < hi) {
                add_line(L, page, c, k, x, y + py, 0);
            }
        }

        py += c->height;
        prev_after = c->pp.space_after;
    }
}

/* the line boxes of a container, from its top: tops and bottoms into lt/lb, *n of them (at most cap) */
static void stack_lines(pd_layout* L, pd_block_id container, pd_sp width, pd_sp* lt, pd_sp* lb, int32_t* n,
                        int32_t cap) {
    pd_block_id ids[512];
    int32_t np = 0, i, k;
    pd_sp prev_after = 0, py = 0;
    pd_status st;

    *n = 0;
    collect_paras(L->doc, container, ids, &np, 512);

    for (i = 0; i < np && i < 512; i++) {
        pcache* c = layout_para(L, ids[i], width, &st);

        if (!c) {
            return;
        }

        if (i > 0) {
            py += para_gap(L->doc, ids[i - 1], prev_after, ids[i], &c->pp);
        }

        for (k = 0; k < c->nlines && *n < cap; k++, (*n)++) {
            lt[*n] = py + c->top[k];
            lb[*n] = py + c->top[k + 1];
        }

        py += c->height;
        prev_after = c->pp.space_after;
    }
}

/* ------------------------------------------------------------------ */
/* fields, counters, labels                                           */
/* ------------------------------------------------------------------ */

static int fv_cmp(const void* a, const void* b) {
    const fieldval* x = (const fieldval*)a, *y = (const fieldval*)b;

    if (x->block != y->block) {
        return x->block < y->block ? -1 : 1;
    }

    return x->offset < y->offset ? -1 : (x->offset > y->offset);
}

static int32_t fv_find(const pd_layout* L, pd_block_id b, uint32_t off) {
    fieldval key, *hit;

    key.block = b;
    key.offset = off;
    hit = L->nfv ? (fieldval*)bsearch(&key, L->fv, (size_t)L->nfv, sizeof(fieldval), fv_cmp) : NULL;
    return hit ? hit->value : 0;
}

/* sequence counters, footnote numbers and list labels, in reading order */
typedef struct {
    char name[32];
    int32_t value;
} seqc;

/* a label for every cached layout of a paragraph */
static void set_label(pd_layout* L, pd_block_id id, const char* text, int note) {
    pcache* c;

    for (c = id < L->ncache ? L->cache[id] : NULL; c; c = c->next) {
        snprintf(c->label, sizeof(c->label), "%s", text);
        c->note = note;
    }
}

static void count_walk(pd_layout* L, pd_block_id id, seqc* seq, int32_t* nseq, int32_t* footnotes,
                       int32_t lcnt[][9], int32_t lseen[][9]) {
    const pd_doc* d = L->doc;
    blk* b = pd_doc_blk(d, id);
    int32_t i, k;

    if (!b) {
        return;
    }

    if (b->kind != PD_BLOCK_PARAGRAPH) {
        for (i = 0; i < b->nkids; i++) {
            count_walk(L, b->kids[i], seq, nseq, footnotes, lcnt, lseen);
        }

        return;
    }

    for (i = 0; i < b->st.ninl; i++) {
        const pd_inline* o = &b->st.inl[i].obj;
        fieldval v;

        v.block = id;
        v.offset = b->st.inl[i].offset;
        v.value = 0;

        if (o->kind == PD_INLINE_FIELD && o->field == PD_FIELD_SEQ) {
            for (k = 0; k < *nseq && strcmp(seq[k].name, o->name); k++) {
            }

            if (k == *nseq && k < 32) {
                strcpy(seq[k].name, o->name);
                seq[k].value = 0;
                (*nseq)++;
            }

            v.value = k < 32 ? ++seq[k].value : 0;
        } else if (o->kind == PD_INLINE_FOOTNOTE) {
            pd_block_id first[1];
            int32_t nf = 0;
            char num[16];

            if (o->level == 1) {    /* an endnote: numbered on its own, i, ii, iii as Word does */
                v.value = L->nendnotes + 1;

                if (!grow((void**)&L->endnotes, &L->capendnotes, (int64_t)L->nendnotes + 1, sizeof(pd_block_id))) {
                    L->endnotes[L->nendnotes++] = o->target;
                }

                pd_doc_format_number(v.value, PD_NUM_LOWER_ROMAN, num, sizeof(num));
            } else {
                v.value = ++*footnotes;
                snprintf(num, sizeof(num), "%d", (int)v.value);
            }

            collect_paras(d, o->target, first, &nf, 1);

            if (nf > 0) {   /* the note body opens with its number */
                set_label(L, first[0], num, 1);
            }
        } else {
            continue;
        }

        if (!grow((void**)&L->fv, &L->capfv, (int64_t)L->nfv + 1, sizeof(fieldval))) {
            L->fv[L->nfv++] = v;
        }
    }

    /* list labels, same numbering rule as pd_doc_list_label */
    if (b->st.list && !b->st.at.cont && (int32_t)b->st.list <= d->nlists && b->st.list <= 64) {
        const dlist* l = &d->lists[b->st.list - 1];
        int32_t lv = b->st.list_level, li = (int32_t)b->st.list - 1;
        char buf[32];

        lcnt[li][lv] = lseen[li][lv] ? lcnt[li][lv] + 1 : l->lv[lv].start;
        lseen[li][lv] = 1;

        for (k = lv + 1; k < 9; k++) {
            lseen[li][k] = 0;
        }

        buf[0] = '\0';

        if (l->lv[lv].format == PD_NUM_BULLET) {
            strncpy(buf, l->lv[lv].text, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
        } else {
            const char* t;
            size_t o = 0;

            for (t = l->lv[lv].text; *t && o + 1 < sizeof(buf); t++) {
                if (t[0] == '%' && t[1] >= '1' && t[1] <= '9') {
                    char num[16];
                    int32_t at = t[1] - '1';
                    size_t q;

                    pd_doc_format_number(at < l->n ? lcnt[li][at] : 0, at < l->n ? l->lv[at].format : PD_NUM_DECIMAL,
                                         num, sizeof(num));

                    for (q = 0; num[q] && o + 1 < sizeof(buf); q++) {
                        buf[o++] = num[q];
                    }

                    t++;
                } else {
                    buf[o++] = *t;
                }
            }

            buf[o] = '\0';
        }

        pd_doc_task_label(b->st.at.task, l->lv[lv].format == PD_NUM_BULLET, buf, sizeof(buf));
        set_label(L, id, buf, 0);
    }
}

/* ------------------------------------------------------------------ */
/* the section filler                                                 */
/* ------------------------------------------------------------------ */

static int push(filler* F, int32_t kind, pd_sp h, int32_t pen, pcache* pc, int32_t line, pd_block_id block) {
    vitem* v;

    if (grow((void**)&F->it, &F->cap, (int64_t)F->n + 1, sizeof(vitem))) {
        return -1;
    }

    v = &F->it[F->n++];
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    v->h = h;
    v->pen = pen;
    v->pc = pc;
    v->line = line;
    v->block = block;
    return 0;
}

/* room the footnotes of a column take: their bodies plus the skip holding the rule */
static pd_sp fn_area(const filler* F, pd_sp fn) {
    return fn > 0 ? fn + F->sp->footnote_skip : 0;
}

/* footnotes referenced in [from, to) of a paragraph ride with the last item */
static pd_status attach_notes(filler* F, const blk* b, uint32_t from, uint32_t to) {
    int32_t i;
    pd_status st = PD_OK;

    for (i = 0; i < b->st.ninl; i++) {
        const dinline* q = &b->st.inl[i];
        vitem* v = &F->it[F->n - 1];
        pd_sp h;

        if (q->obj.kind != PD_INLINE_FOOTNOTE || q->obj.level == 1 || q->offset < from || q->offset >= to ||
                !pd_doc_blk(F->L->doc, q->obj.target)) {   /* endnotes go at the end, not the foot */
            continue;
        }

        h = stack_height(F->L, q->obj.target, F->colw, &st);

        if (h < 0) {
            return st;
        }

        if (grow((void**)&F->notes, &F->capnotes, (int64_t)F->nnotes + 1, sizeof(pd_block_id))) {
            return PD_ERR_NOMEM;
        }

        v = &F->it[F->n - 1];

        if (v->fn_n == 0) {
            v->fn_first = F->nnotes;
        }

        F->notes[F->nnotes++] = q->obj.target;
        v->fn_n++;
        v->fn_h += h;
    }

    return PD_OK;
}

/* where a float was anchored on its page when last placed (1), or not known (0) */
static int fly_get(const pd_layout* L, pd_block_id b, pd_sp* y) {
    int32_t i;

    for (i = 0; i < L->nfly; i++) {
        if (L->fly[i].b == b) {
            *y = L->fly[i].y;
            return 1;
        }
    }

    return 0;
}

/* that remembered; whether it moved (by more than half a point) */
static int fly_set(pd_layout* L, pd_block_id b, pd_sp y) {
    int32_t i;

    for (i = 0; i < L->nfly; i++) {
        if (L->fly[i].b == b) {
            int moved = L->fly[i].y - y > PD_PT(0.5) || y - L->fly[i].y > PD_PT(0.5);

            L->fly[i].y = y;
            return moved;
        }
    }

    if (grow((void**)&L->fly, &L->capfly, (int64_t)L->nfly + 1, sizeof(L->fly[0]))) {
        return 0;
    }

    L->fly[L->nfly].b = b;
    L->fly[L->nfly++].y = y;
    return 1;
}

/* text beside a wrapping float ends: what is left of the float's height becomes space */
static void clear_wrap(filler* F) {
    if (F->wrap_rem > 0 && F->wrap_skip <= 0) {     /* (one still lower down is passed over) */
        push(F, VI_GLUE, F->wrap_rem, 0, NULL, 0, 0);
    }

    F->wrap_rem = 0;
    F->wrap_skip = 0;
    F->gap_h = 0;
}

/* narrowest and widest useful width of a cell's content */
static void cell_minmax(pd_layout* L, pd_block_id cell, pd_sp* mn, pd_sp* mx) {
    pd_block_id ids[256];
    int32_t n = 0, i;

    *mn = *mx = 0;
    collect_paras(L->doc, cell, ids, &n, 256);

    for (i = 0; i < n && i < 256; i++) {
        pd_params prm;
        pd_para_props pp;
        pd_sp a, b, extra;

        if (pd_doc_para_build_ex(L->doc, ids[i], PD_PT(16000), L->scratch, &prm, body_field, L) != PD_OK) {
            continue;
        }

        pd_doc_effective_pp(L->doc, pd_doc_blk(L->doc, ids[i]), &pp, NULL);
        pd_para_natural(L->scratch, &a, &b);
        extra = pp.indent_left + pp.indent_right + (pp.indent_first > 0 ? pp.indent_first : 0);
        *mn = a + extra > *mn ? a + extra : *mn;
        *mx = b + extra > *mx ? b + extra : *mx;
    }
}

/* the cells of a row: column index, span and the column count they cover */
static int32_t cell_span(const ptable* T, const blk* cell, int32_t col) {
    int32_t span = cell->st.cell.col_span < 1 ? 1 : cell->st.cell.col_span;
    return col + span > T->ncols ? T->ncols - col : span;
}

/* the cell of a row that starts at a grid column, NULL if none does */
static const blk* cell_at(const pd_doc* d, const ptable* T, const blk* row, int32_t col) {
    int32_t k, c = 0;

    for (k = 0; row && k < row->nkids && c < T->ncols; k++) {
        const blk* cell = d->tab[row->kids[k]];

        if (c == col) {
            return cell;
        }

        c += cell_span(T, cell, c);
    }

    return NULL;
}

/* how many rows a cell covers: itself and the cells below that continue it */
static int32_t merge_rows(const pd_doc* d, const ptable* T, const blk* t, int32_t r, int32_t col) {
    int32_t n = 1;

    while (r + n < t->nkids) {
        const blk* below = cell_at(d, T, d->tab[t->kids[r + n]], col);

        if (!below || !below->st.cell.merge_up) {
            break;
        }

        n++;
    }

    return n;
}

/* a table: column widths from the content (CSS automatic layout), then one box per row */
/* no cell of row r of t reaches into another row (merged down) */
static int row_alone(const pd_doc* d, const ptable* T, const blk* t, int32_t r) {
    int32_t c;

    for (c = 0; c < T->ncols; c++) {
        if (merge_rows(d, T, t, r, c) > 1) {
            return 0;
        }
    }

    return 1;
}

/* Where a row of height rowh may be cut: the ends of its tallest cell's lines (from the row's top) that no line of
   another cell runs across, increasing; *n of them. malloc'ed. */
static pd_sp* row_cuts(filler* F, const ptable* T, const blk* row, pd_sp pad, pd_sp padv, pd_sp rowh, int32_t* n) {
    const pd_doc* d = F->L->doc;
    enum { CAP = 4096 };
    pd_sp* lt = (pd_sp*)malloc(CAP * sizeof(pd_sp) * 4), *lb = lt ? lt + CAP : NULL, *ot = lb ? lb + CAP : NULL;
    pd_sp* ob = ot ? ot + CAP : NULL, *cuts;
    int32_t k, c = 0, best = -1, nl = 0, i, j, no;
    pd_sp besth = -1;

    *n = 0;

    if (!lt || (cuts = (pd_sp*)malloc(CAP * sizeof(pd_sp))) == NULL) {
        free(lt);
        return NULL;
    }

    for (k = 0, c = 0; k < row->nkids && c < T->ncols; k++) {  /* the tallest cell */
        const blk* cell = d->tab[row->kids[k]];
        int32_t span = cell_span(T, cell, c);
        pd_sp inner = T->colx[c + span] - T->colx[c] - 2 * pad;
        pd_status st;
        pd_sp h = stack_height(F->L, cell->id, inner < PD_PT(1) ? PD_PT(1) : inner, &st);

        if (h > besth) {
            besth = h;
            best = k;
        }

        c += span;
    }

    for (k = 0, c = 0; best >= 0 && k < row->nkids && c < T->ncols; k++) {
        const blk* cell = d->tab[row->kids[k]];
        int32_t span = cell_span(T, cell, c);
        pd_sp inner = T->colx[c + span] - T->colx[c] - 2 * pad;

        inner = inner < PD_PT(1) ? PD_PT(1) : inner;
        c += span;

        if (k == best) {
            stack_lines(F->L, cell->id, inner, lt, lb, &nl, CAP);
        }
    }

    for (i = 0; i < nl; i++) {      /* a cut at each of its lines' ends, unless another cell's line is across it */
        pd_sp at = padv + lb[i];
        int ok = at > 0 && at < rowh;

        for (k = 0, c = 0; ok && k < row->nkids && c < T->ncols; k++) {
            const blk* cell = d->tab[row->kids[k]];
            int32_t span = cell_span(T, cell, c);
            pd_sp inner = T->colx[c + span] - T->colx[c] - 2 * pad;

            inner = inner < PD_PT(1) ? PD_PT(1) : inner;
            c += span;

            if (k == best) {
                continue;
            }

            stack_lines(F->L, cell->id, inner, ot, ob, &no, CAP);

            for (j = 0; ok && j < no; j++) {
                ok = !(padv + ot[j] < at && at < padv + ob[j]);
            }
        }

        if (ok && *n < CAP) {
            cuts[(*n)++] = at;
        }
    }

    free(lt);
    return cuts;
}

/* A table's grid in a column colw wide: its columns' widths (natural, or as the table says), where it sits, its
   header rows. T->hdr_item and T->rowh are left for the caller. */
static void table_grid(pd_layout* L, const blk* t, pd_sp colw, ptable* T) {
    const pd_doc* d = L->doc;
    const pd_table_props* tp = &t->st.tp;
    pd_sp mn[PD_TABLE_MAX_COLS], mx[PD_TABLE_MAX_COLS], w[PD_TABLE_MAX_COLS], avail, fixed = 0;
    int64_t smin = 0, smax = 0;
    int32_t r, k, c, pass, nauto = 0;
    pd_sp pad = tp->cell_padding;

    memset(T, 0, sizeof(*T));
    T->block = t->id;
    T->tp = *tp;
    T->ncols = tp->ncols;

    for (r = 0; r < t->nkids; r++) {    /* grid width: the widest row */
        const blk* row = d->tab[t->kids[r]];
        int32_t cols = 0;

        for (k = 0; k < row->nkids; k++) {
            int32_t sp = d->tab[row->kids[k]]->st.cell.col_span;
            cols += sp < 1 ? 1 : sp;
        }

        T->ncols = cols > T->ncols ? cols : T->ncols;
    }

    T->ncols = T->ncols > PD_TABLE_MAX_COLS ? PD_TABLE_MAX_COLS : T->ncols;

    if (T->ncols == 0) {
        return;
    }

    memset(mn, 0, sizeof(mn));
    memset(mx, 0, sizeof(mx));

    /* single-column cells first, then spanning cells widen the columns they cover */
    for (pass = 0; pass < 2; pass++) {
        for (r = 0; r < t->nkids; r++) {
            const blk* row = d->tab[t->kids[r]];

            for (k = 0, c = 0; k < row->nkids && c < T->ncols; k++) {
                const blk* cell = d->tab[row->kids[k]];
                int32_t span = cell_span(T, cell, c), j;
                pd_sp a, b;

                if ((span == 1) != (pass == 0) || cell->st.cell.merge_up) {
                    c += span;
                    continue;
                }

                cell_minmax(L, cell->id, &a, &b);
                a += 2 * pad;
                b += 2 * pad;

                if (span == 1) {
                    mn[c] = a > mn[c] ? a : mn[c];
                    mx[c] = b > mx[c] ? b : mx[c];
                } else {
                    int64_t have_mn = 0, have_mx = 0;

                    for (j = c; j < c + span; j++) {
                        have_mn += mn[j];
                        have_mx += mx[j];
                    }

                    for (j = c; j < c + span; j++) {
                        if (a > have_mn) {
                            mn[j] += (pd_sp)((a - have_mn) / span);
                        }

                        if (b > have_mx) {
                            mx[j] += (pd_sp)((b - have_mx) / span);
                        }
                    }
                }

                c += span;
            }
        }
    }

    {   /* the width it is given: of the column, or its own; an indented table has less room */
        pd_sp want = tp->width_pct > 0 ? (pd_sp)((int64_t)colw * tp->width_pct / 1000) : tp->width;
        pd_sp room = colw - (tp->align == PD_ALIGN_LEFT && tp->indent > 0 ? tp->indent : 0);

        avail = want > 0 && want < room ? want : room;
        avail = avail > 0 ? avail : colw;
    }

    for (c = 0; c < T->ncols; c++) {
        mx[c] = mx[c] < mn[c] ? mn[c] : mx[c];

        if (c < tp->ncols && tp->col_width[c] > 0) {
            w[c] = tp->col_width[c];
            fixed += w[c];
        } else {
            w[c] = -1;
            nauto++;
            smin += mn[c];
            smax += mx[c];
        }
    }

    for (c = 0; c < T->ncols; c++) {
        int64_t rest = (int64_t)avail - fixed;

        if (w[c] >= 0) {
            continue;
        }

        if (smax <= rest) {     /* everything fits unbroken; a table of given width stretches */
            w[c] = mx[c];

            if (tp->width > 0 || tp->width_pct > 0) {
                w[c] += (pd_sp)(smax > 0 ? (rest - smax) * mx[c] / smax : (rest - smax) / nauto);
            }
        } else if (smin >= rest) {
            w[c] = mn[c];
        } else {
            w[c] = (pd_sp)(mn[c] + (int64_t)(mx[c] - mn[c]) * (rest - smin) / (smax - smin));
        }
    }

    for (c = 0; c < T->ncols; c++) {
        T->colx[c + 1] = T->colx[c] + (w[c] > 0 ? w[c] : 0);
    }

    T->width = T->colx[T->ncols];
    T->x = tp->align == PD_ALIGN_CENTER ? (colw - T->width) / 2 : tp->align == PD_ALIGN_RIGHT ? colw - T->width :
          tp->indent;
    T->x = T->x < 0 && tp->align != PD_ALIGN_LEFT ? 0 : T->x;
    T->header_rows = tp->header_rows < t->nkids ? tp->header_rows : t->nkids - 1;
    T->header_rows = T->header_rows < 0 ? 0 : T->header_rows;
}

static pd_status build_table(filler* F, const blk* t, pd_sp* prev_after, int* prev_keep, int* first) {
    const pd_doc* d = F->L->doc;
    const pd_table_props* tp = &t->st.tp;
    ptable T;
    int32_t r, k, c, pass, ti;
    pd_sp pad = tp->cell_padding, padv = tp->cell_padding_v >= 0 ? tp->cell_padding_v : pad;
    pd_status st = PD_OK;

    table_grid(F->L, t, F->colw, &T);

    if (T.ncols == 0) {
        return PD_OK;
    }

    if (T.header_rows > 0 && (T.hdr_item = (int32_t*)malloc((size_t)T.header_rows * sizeof(int32_t))) == NULL) {
        return PD_ERR_NOMEM;
    }

    if (grow((void**)&F->tb, &F->captb, (int64_t)F->ntb + 1, sizeof(ptable))) {
        free(T.hdr_item);
        return PD_ERR_NOMEM;
    }

    ti = F->ntb;
    F->tb[F->ntb++] = T;

    if (!*first) {
        push(F, VI_PEN, 0, *prev_keep ? INF_PEN : 0, NULL, 0, 0);
    }

    push(F, VI_GLUE, *first ? 0 : *prev_after, 0, NULL, 0, 0);

    if ((F->tb[ti].rowh = (pd_sp*)calloc((size_t)t->nkids + 1, sizeof(pd_sp))) == NULL) {
        return PD_ERR_NOMEM;
    }

    F->tb[ti].nrows = t->nkids;

    /* row heights: a cell merged down the rows below is left out of its own
       row's and what it needs beyond the rows it covers goes to the last */
    for (pass = 0; pass < 2; pass++) {
        for (r = 0; r < t->nkids; r++) {
            const blk* row = d->tab[t->kids[r]];

            for (k = 0, c = 0; k < row->nkids && c < T.ncols; k++) {
                const blk* cell = d->tab[row->kids[k]];
                int32_t span = cell_span(&T, cell, c), down = merge_rows(d, &T, t, r, c), j;
                pd_sp inner = T.colx[c + span] - T.colx[c] - 2 * pad, h, have = 0;

                c += span;

                if (cell->st.cell.merge_up || (down > 1) != (pass == 1)) {
                    continue;
                }

                h = stack_height(F->L, cell->id, inner < PD_PT(1) ? PD_PT(1) : inner, &st);

                if (h < 0) {
                    return st;
                }

                h += 2 * padv;
                h = h > cell->st.cell.min_height ? h : cell->st.cell.min_height;

                for (j = r; j < r + down; j++) {
                    have += F->tb[ti].rowh[j];
                }

                if (down == 1) {
                    F->tb[ti].rowh[r] = h > have ? h : have;
                } else if (h > have) {
                    F->tb[ti].rowh[r + down - 1] += h - have;
                }
            }
        }
    }

    for (r = 0; r < t->nkids; r++) {
        const blk* row = d->tab[t->kids[r]];
        pd_sp rowh = F->tb[ti].rowh[r];
        int merged = 0;

        for (c = 0; c < T.ncols; c++) {     /* a row continuing a merged cell stays with the row above */
            const blk* cell = cell_at(d, &T, row, c);

            merged |= cell && cell->st.cell.merge_up;
        }

        if (r > 0) {    /* header rows stay together and with the first body row */
            push(F, VI_PEN, 0, merged || r <= F->tb[ti].header_rows ? INF_PEN : 0, NULL, 0, 0);
        }

        if (rowh > F->colh && F->colh > 0 && !merged && r >= F->tb[ti].header_rows && row_alone(d, &T, t, r)) {
            /* taller than a column: in slices that break across columns and pages, cut where no cell has a line */
            int32_t ncut = 0, j2;
            pd_sp* cuts = row_cuts(F, &T, row, pad, padv, rowh, &ncut), at = 0;

            if (!cuts) {
                return PD_ERR_NOMEM;
            }

            for (j2 = 0; j2 <= ncut; j2++) {
                pd_sp to = j2 < ncut ? cuts[j2] : rowh;

                if (to <= at) {
                    continue;
                }

                if (at > 0) {
                    push(F, VI_PEN, 0, 0, NULL, 0, 0);
                }

                if (push(F, VI_ROW, to - at, 0, NULL, r, row->id)) {
                    free(cuts);
                    return PD_ERR_NOMEM;
                }

                F->it[F->n - 1].tbl = ti;
                F->it[F->n - 1].part = at == 0 ? 1 : to >= rowh ? 3 : 2;
                F->it[F->n - 1].from = at;
                at = to;
            }

            free(cuts);
        } else {
            if (push(F, VI_ROW, rowh, 0, NULL, r, row->id)) {
                return PD_ERR_NOMEM;
            }

            F->it[F->n - 1].tbl = ti;
        }

        for (k = 0; k < row->nkids; k++) {
            pd_block_id ids[256];
            int32_t n = 0, j;

            collect_paras(d, row->kids[k], ids, &n, 256);

            for (j = 0; j < n && j < 256; j++) {
                if ((st = attach_notes(F, pd_doc_blk(d, ids[j]), 0, UINT32_MAX)) != PD_OK) {
                    return st;
                }
            }
        }

        if (r < F->tb[ti].header_rows) {
            F->tb[ti].hdr_item[r] = F->n - 1;
            F->tb[ti].header_h += rowh;
        }
    }

    *prev_after = 0;
    F->prev_para = 0;
    *prev_keep = 0;
    *first = 0;
    return PD_OK;
}

/* whether all a block holds is hidden by how changes are shown (a deleted float where deletions go): 1 when
   something was seen and nothing of it shows, -1 when nothing was seen */
static int all_hidden(const pd_doc* d, pd_block_id id, int depth) {
    const blk* b = pd_doc_blk(d, id);
    int32_t i, seen = 0;

    if (!b || depth > 16) {
        return 0;
    }

    if (b->kind == PD_BLOCK_PARAGRAPH) {
        for (i = 0; i < b->st.nruns; i++) {
            pd_char_props cp;

            if (b->st.runs[i].end <= b->st.runs[i].start) {
                continue;
            }

            if (pd_doc_format_resolve(d, b->id, b->st.runs[i].format, &cp) != PD_OK) {
                return 0;
            }

            pd_doc_markup_props(d, &cp);

            if (!cp.hidden) {
                return 0;
            }

            seen = 1;
        }

        return seen ? 1 : -1;
    }

    for (i = 0; i < b->nkids; i++) {
        int h = all_hidden(d, b->kids[i], depth + 1);

        if (h == 0) {
            return 0;
        }

        seen |= h > 0;
    }

    return seen ? 1 : -1;
}

static pd_status build_flow(filler* F, pd_block_id container, pd_sp* prev_after, int* prev_keep, int* first) {
    const pd_doc* d = F->L->doc;
    blk* c = pd_doc_blk(d, container);
    int32_t i, k;
    pd_status st = PD_OK;

    for (i = 0; c && i < c->nkids && st == PD_OK; i++) {
        blk* b = d->tab[c->kids[i]];

        if (b->kind == PD_BLOCK_PARAGRAPH) {
            pd_sp gl, rem, skip, gs;
            int32_t loose = F->loose ? F->loose[b->id] : 0, kwrap = 0;
            pcache* pc = layout_para_ex(F->L, b->id, F->colw, F->wrap_rem > 0 ? 0 : loose, NULL, &st);
            const pd_para_props* pp;

            if (!pc) {
                return st;
            }

            pp = &pc->pp;
            gl = *first ? 0 : para_gap(d, F->prev_para, *prev_after, b->id, pp);
            skip = F->wrap_skip - gl;   /* the gap before it: out of the text above the float, then the float's */
            rem = skip >= 0 ? F->wrap_rem : F->wrap_rem + skip;
            skip = skip < 0 ? 0 : skip;

            if (F->wrap_rem > 0 && rem > 0) {   /* beside a float: narrower lines while it lasts */
                wrapshape ws;
                int32_t pass;

                ws.w = F->wrap_w;
                ws.side = F->wrap_side;

                for (pass = 0; pass < 3; pass++) {
                    for (ws.k0 = 0; ws.k0 < pc->nlines && pc->top[ws.k0 + 1] <= skip; ws.k0++) {
                    }

                    for (ws.k = ws.k0; ws.k < pc->nlines && pc->top[ws.k] < skip + rem; ws.k++) {
                    }

                    if ((ws.k == pc->ws_k && ws.k0 == pc->ws_k0) || ws.k <= ws.k0) {
                        break;
                    }

                    if ((pc = layout_para_ex(F->L, b->id, F->colw, 0, &ws, &st)) == NULL) {
                        return st;
                    }
                }

                kwrap = pc->ws_k;
                pp = &pc->pp;
            } else if (F->wrap_rem > 0) {
                F->wrap_rem = 0;
                F->wrap_skip = 0;
            }

            if (pp->page_break_before && !*first) {
                clear_wrap(F);
                push(F, VI_BREAK, 0, 0, NULL, 0, 0);
                F->it[F->n - 1].brk = PD_BREAK_PAGE;
            } else if (!*first) {
                push(F, VI_PEN, 0, *prev_keep || F->wrap_rem > 0 ? INF_PEN : 0, NULL, 0, 0);
            }

            push(F, VI_GLUE, gl, 0, NULL, 0, 0);
            gs = F->gap_skip - gl;

            for (k = 0; k < pc->nlines; k++) {
                pd_line ln;

                if (F->gap_h > 0 && pc->top[k] >= gs) {   /* down to a float across the column: its room */
                    push(F, VI_GLUE, F->gap_h, 0, NULL, 0, 0);
                    F->gap_h = 0;
                }

                if (k > 0) {    /* widows, orphans, keep-lines; never between a float and its text */
                    int ok = !pp->keep_lines && k >= pp->orphans && pc->nlines - k >= pp->widows &&
                             (k > kwrap || kwrap == 0);

                    push(F, VI_PEN, 0, ok ? INTERLINE_PEN : INF_PEN, NULL, 0, 0);
                }

                if (push(F, VI_LINE, pc->top[k + 1] - pc->top[k], 0, pc, k, b->id)) {
                    return PD_ERR_NOMEM;
                }

                pd_para_get_line(pc->para, k, &ln);

                if ((st = attach_notes(F, b, ln.text_start, k + 1 < pc->nlines ? ln.text_end : UINT32_MAX)) != PD_OK) {
                    return st;
                }
            }

            F->gap_skip = gs - pc->height;

            if (F->wrap_rem > 0) {  /* what is left of the float below this paragraph, and of the text above it */
                F->wrap_rem = skip >= pc->height ? rem : rem - (pc->height - skip);
                F->wrap_rem = F->wrap_rem < 0 ? 0 : F->wrap_rem;
                F->wrap_skip = skip > pc->height ? skip - pc->height : 0;
            }

            *prev_after = pp->space_after;
            *prev_keep = pp->keep_with_next;
            *first = 0;
            F->prev_para = b->id;
        } else if (b->kind == PD_BLOCK_FLOAT) {
            pfloat* f;
            pd_sp ox;

            if (pd_doc_revision_count(d) > 0 && all_hidden(d, b->id, 0) > 0) {
                continue;   /* deleted, and deletions not shown here */
            }

            clear_wrap(F);

            if (grow((void**)&F->fl, &F->capfl, (int64_t)F->nfl + 1, sizeof(pfloat))) {
                return PD_ERR_NOMEM;
            }

            f = &F->fl[F->nfl++];
            memset(f, 0, sizeof(*f));
            f->block = b->id;
            f->fp = b->st.fp;
            f->w = f->fp.width ? (f->fp.width < F->colw ? f->fp.width : F->colw) :
                   (pd_sp)((int64_t)F->colw * (f->fp.width_fraction ? f->fp.width_fraction : 1000) / 1000);
            f->h = stack_height(F->L, b->id, f->w, &st);

            if (f->h < 0) {
                return st;
            }

            /* text wraps only beside a float that leaves it room: an inch, as Word fills a gap a line of a few
               words fits in (a figure taking four-fifths of the column has text beside it there) */
            if ((f->fp.wrap == PD_WRAP_LEFT || f->fp.wrap == PD_WRAP_RIGHT) &&
                    F->colw - float_taken(&f->fp, f->w, F->colw, &ox) < PD_PT(72)) {
                f->fp.wrap = PD_WRAP_NONE;
            }

            f->item = F->n;

            if (f->fp.offset_from != PD_FROM_PARAGRAPH && f->fp.wrap < PD_WRAP_FRONT) {
                f->fp.offset_y = 0;     /* text round it: where it is anchored (the page's top is not known here) */
            } else if (f->fp.offset_from != PD_FROM_PARAGRAPH) {
                /* from the page's top (or its margin): that far below where it was anchored the last time, a
                   second pass when that is not known yet */
                pd_sp ay, abs_y = f->fp.offset_y + (f->fp.offset_from == PD_FROM_MARGIN ? F->sp->margin_top : 0);

                if (fly_get(F->L, b->id, &ay)) {
                    f->fp.offset_y = abs_y - ay;
                } else {
                    f->fp.offset_y = 0;
                    F->L->want_rerun = 1;
                }
            }

            if (f->fp.wrap >= PD_WRAP_FRONT) {
                push(F, VI_FLOAT, 0, 0, NULL, 0, b->id);     /* over or under the text: no room of its own */
            } else if (f->fp.wrap != PD_WRAP_NONE) {
                /* lower down: the text beside it from there; higher (over the text before it): the text after its
                   anchor beside what is left of it */
                pd_sp rem = f->h + f->fp.gap + (f->fp.offset_y < 0 ? f->fp.offset_y : 0);

                push(F, VI_FLOAT, 0, 0, NULL, 0, b->id);
                F->wrap_rem = rem > 0 ? rem : 0;
                F->wrap_skip = f->fp.offset_y > 0 ? f->fp.offset_y : 0;
                F->wrap_w = float_taken(&f->fp, f->w, F->colw, &ox);
                F->wrap_side = f->fp.wrap;
            } else if (f->fp.offset_y > 0) {
                /* lower down: the text before it runs on, then its room */
                push(F, VI_FLOAT, 0, 0, NULL, 0, b->id);
                F->gap_skip = f->fp.offset_y;
                F->gap_h = f->h + 2 * f->fp.gap;
            } else {
                pd_sp hh = f->fp.offset_y + f->h + 2 * f->fp.gap;

                push(F, VI_FLOAT, hh > 0 ? hh : 0, 0, NULL, 0, b->id);
            }

            F->it[F->n - 1].line = F->nfl - 1;  /* float index */
        } else if (b->kind == PD_BLOCK_BREAK && b->st.break_kind == PD_BREAK_RULE) {
            clear_wrap(F);

            if (!*first) {
                push(F, VI_PEN, 0, *prev_keep ? INF_PEN : 0, NULL, 0, 0);
            }

            push(F, VI_RULE, RULE_H, 0, NULL, 0, b->id);
            *prev_after = 0;
            F->prev_para = 0;
            *prev_keep = 0;
            *first = 0;
        } else if (b->kind == PD_BLOCK_BREAK) {
            clear_wrap(F);
            push(F, VI_BREAK, 0, 0, NULL, 0, 0);
            F->it[F->n - 1].brk = b->st.break_kind;
        } else if (b->kind == PD_BLOCK_TABLE) {
            clear_wrap(F);
            st = build_table(F, b, prev_after, prev_keep, first);
        }
    }

    return st;
}

/* the whole vertical list of the section, from scratch */
static pd_status rebuild_flow(filler* F) {
    pd_sp prev_after = 0;
    int prev_keep = 0, first = 1;
    int32_t i;
    pd_status st;

    for (i = 0; i < F->ntb; i++) {
        free(F->tb[i].hdr_item);
        free(F->tb[i].rowh);
    }

    F->n = F->nfl = F->nnotes = F->ntb = 0;
    F->wrap_rem = 0;
    F->wrap_skip = 0;
    F->gap_h = 0;
    F->prev_para = 0;
    st = build_flow(F, F->sec->id, &prev_after, &prev_keep, &first);

    /* after the last section's text, the endnotes */
    {
        const blk* root = pd_doc_blk(F->L->doc, PD_ROOT_ID);

        if (st == PD_OK && F->L->nendnotes > 0 && root && root->nkids > 0 &&
                root->kids[root->nkids - 1] == F->sec->id) {
            clear_wrap(F);
            prev_after = prev_after > PD_PT(18) ? prev_after : PD_PT(18);
            F->prev_para = 0;

            for (i = 0; i < F->L->nendnotes && st == PD_OK; i++) {
                if (pd_doc_blk(F->L->doc, F->L->endnotes[i])) {
                    st = build_flow(F, F->L->endnotes[i], &prev_after, &prev_keep, &first);
                }
            }
        }
    }

    clear_wrap(F);
    return st;
}

static pd_sp col_x(const filler* F, int32_t page, int32_t col) {
    return F->L->pages[page].text_x + col * (F->colw + F->gap);
}

/* height above the columns taken by the previous section on a shared page */
static pd_sp col_floor(const filler* F) {
    return F->page == F->floor_page ? F->floor_y : 0;
}

static int32_t next_number(filler* F) {
    return F->number++;
}

static int open_page(filler* F) {
    ppage* p = new_page(F->L, F->sec, next_number(F), F->section_page++);

    if (!p) {
        return -1;
    }

    F->page = F->L->npages - 1;
    F->col = 0;
    return 0;
}

/* a page of floats from the queue */
static int float_page(filler* F) {
    pd_sp y = 0;
    int32_t placed = 0;

    if (open_page(F)) {
        return -1;
    }

    F->L->pages[F->page].float_page = 1;
    F->L->info.float_pages++;

    while (F->nq > 0) {
        pfloat* f = &F->fl[F->queue[0]];
        pd_sp h = f->h + 2 * f->fp.gap;

        if (placed && y + h > F->colh) {
            break;
        }

        place_stack(F->L, f->block, f->w, F->page, col_x(F, F->page, 0) + (F->colw - f->w) / 2,
                    F->top + y + f->fp.gap, 3);
        f->state = FL_PLACED;
        y += h;
        placed++;
        memmove(F->queue, F->queue + 1, (size_t)(--F->nq) * sizeof(int32_t));
    }

    return 0;
}

/* start the next column (or page) and seat queued floats at its top */
static int next_column(filler* F, int force_page) {
    if (force_page || ++F->col >= F->ncols) {
        if (open_page(F)) {
            return -1;
        }
    }

    F->top_used = col_floor(F);

    while (F->nq > 0) {
        pfloat* f = &F->fl[F->queue[0]];
        pd_sp h = f->h + 2 * f->fp.gap;
        int stuck = F->page - f->queued_page > STUCK_PAGES;
        int top_ok = (f->fp.placement & PD_PLACE_TOP) || stuck;
        int small = (int64_t)(F->top_used + h) * 1000 <= (int64_t)F->colh * TOP_FRACTION;

        if (top_ok && (small || (F->top_used == 0 && (!(f->fp.placement & PD_PLACE_PAGE) || stuck)))) {
            place_stack(F->L, f->block, f->w, F->page, col_x(F, F->page, F->col) + (F->colw - f->w) / 2,
                        F->top + F->top_used + f->fp.gap, 3);
            f->state = FL_PLACED;
            F->top_used += h;
            memmove(F->queue, F->queue + 1, (size_t)(--F->nq) * sizeof(int32_t));
            continue;
        }

        if (F->top_used == 0 && F->col == 0 && ((f->fp.placement & PD_PLACE_PAGE) || stuck)) {
            /* this fresh page becomes a float page; text resumes on the next */
            F->L->npages--;
            F->number--;
            F->section_page--;

            if (float_page(F) || open_page(F)) {
                return -1;
            }

            continue;
        }

        break;  /* the head waits (bottom-only, or the column is full); order is kept */
    }

    return 0;
}

typedef struct {
    int32_t item;
    pd_sp y;
    pd_sp fn;                   /* footnote bodies so far in the column */
    int repeat;                 /* a repeated table header row */
} rec;

/* the cell of a row whose columns take in col */
static const blk* cell_over(const pd_doc* d, const ptable* T, const blk* row, int32_t col) {
    int32_t k, c = 0;

    for (k = 0; row && k < row->nkids && c < T->ncols; k++) {
        const blk* cell = d->tab[row->kids[k]];
        int32_t span = cell_span(T, cell, c);

        if (col >= c && col < c + span) {
            return cell;
        }

        c += span;
    }

    return NULL;
}

/* the width of one edge (a PD_BORDER_* bit) of a cell's own rules */
static pd_sp cell_edge_width(const pd_cell_props* cp, int edge) {
    int k = edge == PD_BORDER_TOP ? 0 : edge == PD_BORDER_RIGHT ? 1 : edge == PD_BORDER_BOTTOM ? 2 : 3;

    return cp->edge_width[k] > 0 ? cp->edge_width[k] : cp->border_width;
}

/* The rule on one edge of a cell: what the cell says of it, else what the
   cell across the edge says of its own side there, else the table's rule
   for an outer edge or one between cells. Returns its width, 0 for none. */
static pd_sp edge_rule(const ptable* T, const blk* cell, int edge, const blk* across, int across_edge, int outer_bit,
                       int inner_bit, int outer, uint32_t* color) {
    int sides = T->tp.border_sides ? T->tp.border_sides : 63;

    if (cell && (cell->st.cell.border_set & edge)) {
        *color = cell->st.cell.border_color;
        return (cell->st.cell.border_on & edge) ? cell_edge_width(&cell->st.cell, edge) : 0;
    }

    if (across && (across->st.cell.border_set & across_edge)) {
        *color = across->st.cell.border_color;
        return (across->st.cell.border_on & across_edge) ? cell_edge_width(&across->st.cell, across_edge) : 0;
    }

    *color = T->tp.border_color;
    return (sides & (outer ? outer_bit : inner_bit)) ? T->tp.border : 0;
}

/* A table inside a block stack (a float, a text box, a cell), whole where it is: its grid in width, each row as tall
   as its tallest cell, the cells' shading, content and rules. Measured only with draw 0. Returns its height. */
static pd_sp place_table_box(pd_layout* L, const blk* t, pd_sp width, int32_t page, pd_sp x, pd_sp y, int32_t region,
                             int draw, pd_status* st) {
    const pd_doc* d = L->doc;
    ptable T;
    pd_sp pad = t->st.tp.cell_padding, padv = t->st.tp.cell_padding_v >= 0 ? t->st.tp.cell_padding_v : pad, h = 0;
    pd_sp rowh[256];
    int32_t r, k, c;

    table_grid(L, t, width, &T);
    T.block = t->id;

    if (T.ncols == 0 || t->nkids == 0) {
        return 0;
    }

    for (r = 0; r < t->nkids && r < 256; r++) {     /* each row: its tallest cell */
        const blk* row = d->tab[t->kids[r]];

        rowh[r] = 0;

        for (k = 0, c = 0; k < row->nkids && c < T.ncols; k++) {
            const blk* cell = d->tab[row->kids[k]];
            int32_t span = cell_span(&T, cell, c);
            pd_sp inner = T.colx[c + span] - T.colx[c] - 2 * pad, ch;

            c += span;

            if (cell->st.cell.merge_up) {
                continue;
            }

            ch = stack_walk(L, cell->id, inner < PD_PT(1) ? PD_PT(1) : inner, 0, 0, 0, 0, 0, st);

            if (ch < 0) {
                return -1;
            }

            ch += 2 * padv;
            ch = ch > cell->st.cell.min_height ? ch : cell->st.cell.min_height;
            rowh[r] = ch > rowh[r] ? ch : rowh[r];
        }

        h += rowh[r];
    }

    if (!draw) {
        return h;
    }

    for (r = 0, h = 0; r < t->nkids && r < 256; r++) {
        const blk* row = d->tab[t->kids[r]];
        const blk* next = r + 1 < t->nkids ? d->tab[t->kids[r + 1]] : NULL;
        const blk* prev = r > 0 ? d->tab[t->kids[r - 1]] : NULL;
        pd_sp ry = y + h, tx = x + T.x;

        for (k = 0, c = 0; k < row->nkids && c < T.ncols; k++) {
            const blk* cell = d->tab[row->kids[k]];
            int32_t span = cell_span(&T, cell, c);
            pd_sp cx = tx + T.colx[c], cw = T.colx[c + span] - T.colx[c], inner = cw - 2 * pad, bw, ch, off = 0;
            uint32_t bc;

            inner = inner < PD_PT(1) ? PD_PT(1) : inner;

            if (cell->st.cell.background) {
                add_rule(L, page, cx, ry, cw, rowh[r], cell->st.cell.background, region, cell->id);
            }

            if (!cell->st.cell.merge_up) {
                ch = stack_walk(L, cell->id, inner, 0, 0, 0, 0, 0, st);
                off = cell->st.cell.valign == 1 ? (rowh[r] - 2 * padv - ch) / 2 : cell->st.cell.valign == 2 ?
                      rowh[r] - 2 * padv - ch : 0;
                stack_walk(L, cell->id, inner, 1, page, cx + pad, ry + padv + (off > 0 ? off : 0), region, st);
            }

            bw = edge_rule(&T, cell, PD_BORDER_LEFT, c > 0 ? cell_over(d, &T, row, c - 1) : NULL, PD_BORDER_RIGHT,
                           PD_TBORDER_LEFT, PD_TBORDER_INSIDE_V, c == 0, &bc);

            if (bw > 0) {
                add_rule(L, page, cx - bw / 2, ry, bw, rowh[r], bc, region, cell->id);
            }

            if (c + span >= T.ncols || k + 1 == row->nkids) {
                bw = edge_rule(&T, cell, PD_BORDER_RIGHT, NULL, 0, PD_TBORDER_RIGHT, PD_TBORDER_INSIDE_V,
                               c + span >= T.ncols, &bc);

                if (bw > 0) {
                    add_rule(L, page, cx + cw - bw / 2, ry, bw, rowh[r], bc, region, cell->id);
                }
            }

            bw = edge_rule(&T, cell, PD_BORDER_TOP, prev ? cell_over(d, &T, prev, c) : NULL, PD_BORDER_BOTTOM,
                           PD_TBORDER_TOP, PD_TBORDER_INSIDE_H, r == 0, &bc);

            if (bw > 0 && !cell->st.cell.merge_up) {
                add_rule(L, page, cx - bw / 2, ry - bw / 2, cw + bw, bw, bc, region, cell->id);
            }

            bw = edge_rule(&T, cell, PD_BORDER_BOTTOM, next ? cell_over(d, &T, next, c) : NULL, PD_BORDER_TOP,
                           PD_TBORDER_BOTTOM, PD_TBORDER_INSIDE_H, !next, &bc);

            if (bw > 0) {
                add_rule(L, page, cx - bw / 2, ry + rowh[r] - bw / 2, cw + bw, bw, bc, region, cell->id);
            }

            c += span;
        }

        h += rowh[r];
    }

    return h;
}

/* one table row: cell backgrounds, contents and grid rules */
static void place_row(filler* F, const vitem* v, pd_sp x, pd_sp y) {
    const pd_doc* d = F->L->doc;
    const ptable* T = &F->tb[v->tbl];
    const blk* row = pd_doc_blk(d, v->block);
    const blk* t = pd_doc_blk(d, T->block);
    const blk* next = t && v->line + 1 < t->nkids ? d->tab[t->kids[v->line + 1]] : NULL;
    const blk* prev = t && v->line > 0 ? d->tab[t->kids[v->line - 1]] : NULL;
    pd_sp pad = T->tp.cell_padding, padv = T->tp.cell_padding_v >= 0 ? T->tp.cell_padding_v : pad, tx = x + T->x;
    int32_t k, c = 0;
    pd_status st;

    if (!row) {
        return;
    }

    for (k = 0; k < row->nkids && c < T->ncols; k++) {
        const blk* cell = d->tab[row->kids[k]];
        const blk* below = next ? cell_at(d, T, next, c) : NULL;
        int32_t span = cell_span(T, cell, c), down = t ? merge_rows(d, T, t, v->line, c) : 1, j;
        pd_sp cx = tx + T->colx[c], cw = T->colx[c + span] - T->colx[c], inner = cw - 2 * pad, h, off = 0;
        pd_sp ch = 0, bw;
        uint32_t bc;

        for (j = v->line; j < v->line + down && T->rowh && j < T->nrows; j++) {
            ch += T->rowh[j];   /* the merged cell's height: every row it covers */
        }

        ch = down > 1 && ch > 0 ? ch : v->h;
        inner = inner < PD_PT(1) ? PD_PT(1) : inner;

        if (v->part && !cell->st.cell.merge_up) {     /* a slice: the lines that start in it, from the top */
            if (cell->st.cell.background) {
                add_rule(F->L, F->page, cx, y, cw, v->h, cell->st.cell.background, 0, cell->id);
            }

            place_stack_clip(F->L, cell->id, inner, F->page, cx + pad, y - v->from + padv, v->from - padv,
                             v->from - padv + v->h);
        } else if (!cell->st.cell.merge_up) {
            if (cell->st.cell.background) {
                add_rule(F->L, F->page, cx, y, cw, ch, cell->st.cell.background, 0, cell->id);
            }

            h = stack_height(F->L, cell->id, inner, &st);

            if (h >= 0 && cell->st.cell.valign == 1) {
                off = (ch - 2 * padv - h) / 2;
            } else if (h >= 0 && cell->st.cell.valign == 2) {
                off = ch - 2 * padv - h;
            }

            place_stack(F->L, cell->id, inner, F->page, cx + pad, y + padv + (off > 0 ? off : 0), 0);
        }

        /* left edge: every cell's; right edge: the last one's (the next cell's left is the rest) */
        bw = edge_rule(T, cell, PD_BORDER_LEFT, c > 0 ? cell_over(d, T, row, c - 1) : NULL, PD_BORDER_RIGHT,
                       PD_TBORDER_LEFT, PD_TBORDER_INSIDE_V, c == 0, &bc);

        if (bw > 0) {
            add_rule(F->L, F->page, cx - bw / 2, y, bw, v->h, bc, 0, cell->id);
        }

        if (c + span >= T->ncols || k + 1 == row->nkids) {
            bw = edge_rule(T, cell, PD_BORDER_RIGHT, NULL, 0, PD_TBORDER_RIGHT, PD_TBORDER_INSIDE_V,
                           c + span >= T->ncols, &bc);

            if (bw > 0) {
                add_rule(F->L, F->page, cx + cw - bw / 2, y, bw, v->h, bc, 0, cell->id);
            }
        }

        if (!cell->st.cell.merge_up && v->part <= 1) {      /* no rule inside a merged cell, or a sliced row */
            bw = edge_rule(T, cell, PD_BORDER_TOP, prev ? cell_over(d, T, prev, c) : NULL, PD_BORDER_BOTTOM,
                           PD_TBORDER_TOP, PD_TBORDER_INSIDE_H, v->line == 0, &bc);

            if (bw > 0) {
                add_rule(F->L, F->page, cx - bw / 2, y - bw / 2, cw + bw, bw, bc, 0, cell->id);
            }
        }

        if ((!below || !below->st.cell.merge_up) && (v->part == 0 || v->part == 3)) {
            bw = edge_rule(T, cell, PD_BORDER_BOTTOM, next ? cell_over(d, T, next, c) : NULL, PD_BORDER_TOP,
                           PD_TBORDER_BOTTOM, PD_TBORDER_INSIDE_H, !next, &bc);

            if (bw > 0) {
                add_rule(F->L, F->page, cx - bw / 2, y + v->h - bw / 2, cw + bw, bw, bc, 0, cell->id);
            }
        }

        c += span;
    }
}

/* emit a column's content (records before cut), its footnotes, and bottom floats in what is left */
static void commit(filler* F, const rec* r, int32_t nr, int32_t cut, pd_sp used_at_cut, pd_sp fn_at_cut) {
    pd_sp x = col_x(F, F->page, F->col), y0 = F->top + F->top_used;
    pd_sp avail = F->colh - F->top_used, bottom = avail - fn_area(F, fn_at_cut);
    int32_t i, k;

    /* a section whose pages hold their text centred or at the bottom (a title page) */
    if (F->sp->page_valign && F->ncols == 1 && F->page != F->floor_page && used_at_cut < bottom) {
        y0 += F->sp->page_valign == 1 ? (bottom - used_at_cut) / 2 : bottom - used_at_cut;
    }

    for (i = 0; i < nr && r[i].item < cut; i++) {
        const vitem* v = &F->it[r[i].item];

        if (v->kind == VI_LINE) {
            add_line(F->L, F->page, v->pc, v->line, x, y0 + r[i].y - v->pc->top[v->line], 0);
        } else if (v->kind == VI_ROW) {
            place_row(F, v, x, y0 + r[i].y);
        } else if (v->kind == VI_RULE) {
            add_rule(F->L, F->page, x, y0 + r[i].y + RULE_H / 2, F->colw, PD_SP_PER_PT / 2, 0xFF808080u, 0, v->block);
        } else if (v->kind == VI_FLOAT) {
            pfloat* f = &F->fl[v->line];

            if (fly_set(F->L, f->block, y0 + r[i].y) && f->fp.offset_from != PD_FROM_PARAGRAPH) {
                F->L->want_rerun = 1;   /* its anchor elsewhere than it was taken to be */
            }

            if (f->fp.wrap >= PD_WRAP_FRONT) {  /* over or under the text: where it is put, across or in the middle */
                pd_sp ox = (f->fp.placement & PD_PLACE_OFFSET) ? f->fp.offset_x : (F->colw - f->w) / 2;
                ppage* pg = &F->L->pages[F->page];
                int32_t n0 = pg->n;

                place_stack(F->L, f->block, f->w, F->page, x + ox, y0 + r[i].y + f->fp.offset_y, 3);

                if (f->fp.wrap == PD_WRAP_BEHIND && pg->n > n0 && n0 > 0) {     /* under: drawn first */
                    pline* tmp = (pline*)malloc(sizeof(pline) * (size_t)(pg->n - n0));

                    if (tmp) {
                        memcpy(tmp, pg->lines + n0, sizeof(pline) * (size_t)(pg->n - n0));
                        memmove(pg->lines + (pg->n - n0), pg->lines, sizeof(pline) * (size_t)n0);
                        memcpy(pg->lines, tmp, sizeof(pline) * (size_t)(pg->n - n0));
                        free(tmp);
                    }
                }
            } else if (f->fp.wrap != PD_WRAP_NONE) {   /* at the anchor, against the column edge */
                pd_sp ox;

                float_taken(&f->fp, f->w, F->colw, &ox);
                place_stack(F->L, f->block, f->w, F->page, x + ox,
                            y0 + r[i].y + f->fp.offset_y, 3);
            } else {
                place_stack(F->L, f->block, f->w, F->page, x + (F->colw - f->w) / 2,
                            y0 + r[i].y + f->fp.offset_y + f->fp.gap, 3);
            }

            f->state = FL_PLACED;
        }
    }

    /* footnotes at the bottom of the column, below a short rule */
    if (fn_at_cut > 0) {
        pd_sp y = y0 + avail - fn_at_cut;
        pd_status st;

        add_rule(F->L, F->page, x, y - F->sp->footnote_skip / 2, F->colw < FN_RULE_WIDTH ? F->colw : FN_RULE_WIDTH,
                 FN_RULE_HEIGHT, 0xFF000000u, 4, 0);

        for (i = 0; i < nr && r[i].item < cut; i++) {
            const vitem* v = &F->it[r[i].item];

            for (k = 0; !r[i].repeat && k < v->fn_n; k++) {
                pd_block_id story = F->notes[v->fn_first + k];
                pd_sp h = stack_height(F->L, story, F->colw, &st);

                place_stack(F->L, story, F->colw, F->page, x, y, 4);
                y += h > 0 ? h : 0;
            }
        }
    }

    /* bottom floats: anchored before the cut, in the space left over */
    for (i = 0; i < F->nq;) {
        pfloat* f = &F->fl[F->queue[i]];
        pd_sp h = f->h + 2 * f->fp.gap;

        if (i == 0 && (f->fp.placement & PD_PLACE_BOTTOM) && f->item < cut && used_at_cut + h <= bottom) {
            bottom -= h;
            place_stack(F->L, f->block, f->w, F->page, x + (F->colw - f->w) / 2, y0 + bottom + f->fp.gap, 3);
            f->state = FL_PLACED;
            memmove(F->queue + i, F->queue + i + 1, (size_t)(F->nq - i - 1) * sizeof(int32_t));
            F->nq--;
            continue;
        }

        break;  /* only the head may go, so floats stay in order */
    }
}

/* a column that resumes inside a table starts with its header rows again */
static int repeat_headers(filler* F, rec** r, int32_t* nr, int32_t* capr, int32_t i, pd_sp* used, pd_sp fn) {
    const ptable* T;
    int32_t j;

    if (i >= F->n || F->it[i].kind != VI_ROW) {
        return 0;
    }

    T = &F->tb[F->it[i].tbl];

    for (j = 0; F->it[i].line >= T->header_rows && j < T->header_rows; j++) {
        if (grow((void**)r, capr, (int64_t)*nr + 1, sizeof(rec))) {
            return -1;
        }

        (*r)[*nr].item = T->hdr_item[j];
        (*r)[*nr].y = *used;
        (*r)[*nr].fn = fn;
        (*r)[(*nr)++].repeat = 1;
        *used += F->it[T->hdr_item[j]].h;
    }

    return 0;
}

static pd_status fill(filler* F, int32_t start) {
    rec* r = NULL;
    int32_t nr = 0, capr = 0, i, best = -1;
    int64_t best_cost = 0;
    pd_sp used = 0, fn = 0, avail;
    int empty = 1;

    if (F->resume) {    /* below what is already on this page */
        F->col = 0;
        F->top_used = col_floor(F);
        F->page_start_n = F->L->pages[F->page].n;
        F->page_start_nrules = F->L->pages[F->page].nrules;
    } else {
        if (next_column(F, 1)) {
            return PD_ERR_NOMEM;
        }

        F->page_start_n = F->page_start_nrules = 0;
    }

    F->page_start_item = start;
    avail = F->colh - F->top_used;

    if (repeat_headers(F, &r, &nr, &capr, start, &used, fn)) {
        return PD_ERR_NOMEM;
    }

    for (i = start; i <= F->n;) {
        vitem* v = i < F->n ? &F->it[i] : NULL;
        int cut = 0, forced = 0;
        int32_t at = 0;

        if (!v) {   /* end of the flow */
            commit(F, r, nr, F->n, used, fn);
            break;
        }

        switch (v->kind) {
            case VI_GLUE:
                if (!empty) {
                    used += v->h;
                }

                i++;
                continue;

            case VI_PEN:
                if (!empty && v->pen < INF_PEN) {
                    int64_t cost = v->pen + badness(avail - used - fn_area(F, fn), avail / 5);

                    if (F->chosen && F->chosen[i]) {    /* optimal page breaking decided on this one */
                        cut = 1;
                        at = i;
                        break;
                    }

                    if (best < 0 || cost <= best_cost) {
                        best = i;
                        best_cost = cost;
                    }
                }

                i++;
                continue;

            case VI_BREAK:
                if (empty && F->col == 0 && F->L->pages[F->page].n == 0) {
                    int32_t num = F->L->pages[F->page].number, brk = v->brk;

                    /* already at the top of a fresh page: nothing to break, unless parity says so */
                    if (brk == PD_BREAK_PAGE || brk == PD_BREAK_COLUMN || (brk == PD_BREAK_ODD_PAGE && num % 2) ||
                            (brk == PD_BREAK_EVEN_PAGE && num % 2 == 0)) {
                        i++;
                        continue;
                    }
                }

                cut = 1;
                forced = 1;
                at = i;
                break;

            case VI_FLOAT: {
                pfloat* f = &F->fl[v->line];

                if (f->state != FL_NONE) {
                    i++;
                    continue;
                }

                if (f->fp.wrap != PD_WRAP_NONE) {   /* at its anchor, with the text beside it */
                    if (f->fp.wrap < PD_WRAP_FRONT && used + f->h + f->fp.gap + fn_area(F, fn) > avail && !empty) {
                        cut = 1;
                        at = i;
                        break;
                    }

                    if (grow((void**)&r, &capr, (int64_t)nr + 1, sizeof(rec))) {
                        free(r);
                        return PD_ERR_NOMEM;
                    }

                    r[nr].item = i;
                    r[nr].fn = fn;
                    r[nr].repeat = 0;
                    r[nr++].y = used;
                    empty = 0;
                    i++;
                    continue;
                }

                if ((f->fp.placement & (PD_PLACE_HERE | PD_PLACE_FORCE)) &&
                        (used + v->h + fn_area(F, fn) <= avail || empty)) {
                    if (grow((void**)&r, &capr, (int64_t)nr + 1, sizeof(rec))) {
                        free(r);
                        return PD_ERR_NOMEM;
                    }

                    r[nr].item = i;
                    r[nr].fn = fn;
                    r[nr].repeat = 0;
                    r[nr++].y = used;
                    used += v->h;
                    empty = 0;
                    i++;
                    continue;
                }

                if (f->fp.placement & PD_PLACE_FORCE) {     /* exactly here: on the next column */
                    cut = 1;
                    at = i;
                    break;
                }

                if (empty && (f->fp.placement & PD_PLACE_TOP) && F->nq == 0 &&
                        (int64_t)(F->top_used + v->h) * 1000 <= (int64_t)F->colh * TOP_FRACTION) {
                    /* nothing on this column yet: its top is still free */
                    place_stack(F->L, f->block, f->w, F->page, col_x(F, F->page, F->col) + (F->colw - f->w) / 2,
                                F->top + F->top_used + f->fp.gap, 3);
                    f->state = FL_PLACED;
                    F->top_used += v->h;
                    avail = F->colh - F->top_used;
                    i++;
                    continue;
                }

                f->state = FL_QUEUED;
                f->queued_page = F->page;

                if (grow((void**)&F->queue, &F->capq, (int64_t)F->nq + 1, sizeof(int32_t))) {
                    free(r);
                    return PD_ERR_NOMEM;
                }

                F->queue[F->nq++] = v->line;
                i++;
                continue;
            }

            case VI_LINE:
            case VI_ROW:
            case VI_RULE:
                if (used + v->h + fn_area(F, fn + v->fn_h) > avail && !empty) {
                    cut = 1;
                    at = best >= 0 ? best : i;     /* no legal break: emergency, right here */
                    break;
                }

                if (used + v->h + fn_area(F, fn + v->fn_h) > avail) {
                    F->L->info.overfull++;
                }

                if (grow((void**)&r, &capr, (int64_t)nr + 1, sizeof(rec))) {
                    free(r);
                    return PD_ERR_NOMEM;
                }

                used += v->h;
                fn += v->fn_h;
                r[nr].item = i;
                r[nr].fn = fn;
                r[nr].repeat = 0;
                r[nr++].y = used - v->h;
                empty = 0;
                i++;
                continue;
        }

        if (cut) {
            pd_sp used_at = 0, fn_at = 0;
            int32_t k, brk = forced ? F->it[at].brk : -1, page = F->page;

            for (k = 0; k < nr && r[k].item < at; k++) {
                used_at = r[k].y + F->it[r[k].item].h;
                fn_at = r[k].fn;
            }

            commit(F, r, nr, at, used_at, fn_at);
            nr = 0;
            best = -1;
            used = fn = 0;
            empty = 1;

            if (brk == PD_BREAK_COLUMN) {
                if (next_column(F, 0)) {
                    free(r);
                    return PD_ERR_NOMEM;
                }
            } else {
                int page_break = forced;

                if (next_column(F, page_break)) {
                    free(r);
                    return PD_ERR_NOMEM;
                }

                /* odd/even breaks may need a blank page first */
                if ((brk == PD_BREAK_ODD_PAGE && F->L->pages[F->page].number % 2 == 0) ||
                        (brk == PD_BREAK_EVEN_PAGE && F->L->pages[F->page].number % 2 != 0)) {
                    if (next_column(F, 1)) {
                        free(r);
                        return PD_ERR_NOMEM;
                    }
                }
            }

            avail = F->colh - F->top_used;
            i = at + (forced ? 1 : 0);

            /* a penalty break resumes after it; a line break resumes at the line */
            if (!forced && i < F->n && F->it[i].kind == VI_PEN) {
                i++;
            }

            if (F->page != page) {
                F->page_start_item = i;
                F->page_start_n = F->page_start_nrules = 0;
            }

            if (repeat_headers(F, &r, &nr, &capr, i, &used, fn)) {
                free(r);
                return PD_ERR_NOMEM;
            }
        }
    }

    free(r);

    /* the section ends: whatever is still queued goes on float pages */
    while (F->nq > 0) {
        if (float_page(F)) {
            return PD_ERR_NOMEM;
        }
    }

    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* optimal page breaking                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t total;
    int32_t prev;               /* state index */
} pstate;

/*
 * Column breaks for the whole flow at once: a shortest path over the
 * breakable penalties, states (candidate, column mod ncols, first page).
 * A column costs (10 + badness of its unfilled space)^2 plus its penalty
 * squared, the last one nothing. Fills chosen (1 per penalty item to
 * break at) and per-column costs/ends; returns the total or -1.
 */
static int64_t optimal_breaks(filler* F, uint8_t* chosen, int32_t* ends, int64_t* costs, int32_t* ncolumns) {
    int32_t* cand = NULL, m = 0, i, a, S, best = -1, nc = F->ncols, pos;
    int32_t* cidx = NULL;
    pstate* st = NULL;
    int64_t result = -1;

    cand = (int32_t*)malloc(((size_t)F->n + 2) * sizeof(int32_t));
    cidx = (int32_t*)malloc(((size_t)F->n + 1) * sizeof(int32_t));

    if (!cand || !cidx) {
        goto out;
    }

    cand[m++] = -1;

    for (i = 0; i < F->n; i++) {
        cidx[i] = -1;

        if ((F->it[i].kind == VI_PEN && F->it[i].pen < INF_PEN) || F->it[i].kind == VI_BREAK) {
            cidx[i] = m;
            cand[m++] = i;
        }
    }

    cidx[F->n] = m;
    cand[m++] = F->n;
    S = m * nc * 2;

    if ((int64_t)m * nc * 2 > INT32_MAX / 2 || (st = (pstate*)malloc((size_t)S * sizeof(pstate))) == NULL) {
        goto out;
    }

    for (i = 0; i < S; i++) {
        st[i].total = DEM_HUGE * 4;
        st[i].prev = -1;
    }

#define PIDX(c, col, fp) (((c) * nc + (col)) * 2 + (fp))
    st[PIDX(0, 0, 1)].total = 0;

    for (a = 0; a < m - 1; a++) {
        int32_t col, fp;

        for (col = 0; col < nc; col++) {
            for (fp = 0; fp < 2; fp++) {
                int64_t base = st[PIDX(a, col, fp)].total;
                pd_sp cap = F->colh - (fp && F->floor_page >= 0 && F->resume ? F->floor_y : 0), used = 0, fn = 0;
                int empty = 1;
                int32_t j = cand[a] + 1;

                if (base >= DEM_HUGE * 4) {
                    continue;
                }

                if (j < F->n && F->it[j].kind == VI_ROW) {
                    const ptable* T = &F->tb[F->it[j].tbl];
                    used = F->it[j].line >= T->header_rows ? T->header_h : 0;
                }

                for (; j <= F->n; j++) {
                    const vitem* v = j < F->n ? &F->it[j] : NULL;
                    int32_t ncol = col + 1 < nc ? col + 1 : 0, nfp = col + 1 < nc ? fp : 0, b;
                    int64_t d, t;

                    if (v && v->kind == VI_GLUE) {
                        used += empty ? 0 : v->h;
                        continue;
                    }

                    if (v && (v->kind == VI_LINE || v->kind == VI_ROW || v->kind == VI_RULE)) {
                        if (used + v->h + fn_area(F, fn + v->fn_h) > cap && !empty) {
                            break;
                        }

                        used += v->h;
                        fn += v->fn_h;
                        empty = 0;
                        continue;
                    }

                    if (v && (v->kind != VI_BREAK && (v->kind != VI_PEN || v->pen >= INF_PEN || empty))) {
                        continue;
                    }

                    if (v && v->kind == VI_BREAK && empty && col == 0) {
                        continue;   /* at a page top a page break does nothing */
                    }

                    b = cidx[j];

                    if (!v) {
                        d = 0;      /* the last column may stay short */
                    } else if (v->kind == VI_BREAK) {
                        d = 0;

                        if (v->brk != PD_BREAK_COLUMN) {
                            ncol = 0;
                            nfp = 0;
                        }
                    } else {
                        int64_t bad = badness(cap - used - fn_area(F, fn), cap / 5);

                        d = (10 + bad) * (10 + bad) + (int64_t)v->pen * v->pen * (v->pen < 0 ? -1 : 1);
                    }

                    t = base + d;

                    if (t < st[PIDX(b, ncol, nfp)].total) {
                        st[PIDX(b, ncol, nfp)].total = t;
                        st[PIDX(b, ncol, nfp)].prev = PIDX(a, col, fp);
                    }

                    if (!v || v->kind == VI_BREAK) {
                        break;
                    }
                }

            }
        }
    }

    for (i = 0; i < nc * 2; i++) {
        int32_t s = PIDX(m - 1, 0, 0) + i;

        if (st[s].total < DEM_HUGE * 4 && (best < 0 || st[s].total < st[best].total)) {
            best = s;
        }
    }

    if (best < 0) {
        goto out;
    }

    result = st[best].total;
    memset(chosen, 0, (size_t)F->n + 1);
    *ncolumns = 0;

    for (pos = best; pos >= 0 && st[pos].prev >= 0; pos = st[pos].prev) {
        int32_t c = pos / (nc * 2), item = cand[c];

        if (item < F->n && F->it[item].kind == VI_PEN) {
            chosen[item] = 1;
        }

        if (ends) {
            ends[*ncolumns] = item;
            costs[*ncolumns] = st[pos].total - st[st[pos].prev].total;
        }

        (*ncolumns)++;
    }

#undef PIDX
out:
    free(cand);
    free(cidx);
    free(st);
    return result;
}

/* the paragraphs with lines in (from, to] of the flow */
static int32_t paras_between(const filler* F, int32_t from, int32_t to, pd_block_id* out, int32_t cap) {
    int32_t i, n = 0;

    for (i = from + 1; i <= to && i < F->n; i++) {
        const vitem* v = &F->it[i];

        if (v->kind == VI_LINE && v->pc->ws_k == 0 && v->pc->nlines > 1 && (n == 0 || out[n - 1] != v->block) &&
                n < cap) {
            out[n++] = v->block;
        }
    }

    return n;
}

/*
 * Optimal page breaking with paragraph variants: breaks for the whole
 * section, then up to VARIANT_TRIALS paragraphs near the costliest
 * columns are tried a line looser or tighter; a variant stays when it
 * lowers the total (each costs VARIANT_DEMERITS).
 */
/* arrays sized for the current flow */
static int plan_arrays(filler* F, int32_t** ends, int64_t** costs) {
    uint8_t* c = (uint8_t*)realloc(F->chosen, (size_t)F->n + 1);
    int32_t* e;
    int64_t* k;

    if (!c) {
        return -1;
    }

    F->chosen = c;

    if (!ends) {
        return 0;
    }

    if ((e = (int32_t*)realloc(*ends, ((size_t)F->n + 2) * sizeof(int32_t))) == NULL) {
        return -1;
    }

    *ends = e;

    if ((k = (int64_t*)realloc(*costs, ((size_t)F->n + 2) * sizeof(int64_t))) == NULL) {
        return -1;
    }

    *costs = k;
    return 0;
}

/* a variant worth trying: a different line count, and every line within TeX's default tolerance */
static int variant_ok(filler* F, pd_block_id id, int32_t delta) {
    pd_status st;
    pcache* base = layout_para(F->L, id, F->colw, &st), *v;
    int32_t i, n0;

    if (!base) {
        return 0;
    }

    n0 = base->nlines;
    v = layout_para_ex(F->L, id, F->colw, delta, NULL, &st);

    if (!v || v->nlines == n0) {
        return 0;
    }

    for (i = 0; i + 1 < v->nlines; i++) {
        pd_line ln;

        pd_para_get_line(v->para, i, &ln);

        if (ln.ratio > 1260 || ln.ratio < -1000 || ln.overfull) {  /* TeX's \tolerance 200 */
            return 0;
        }
    }

    return 1;
}

static pd_status plan_optimal(filler* F) {
    const pd_doc* d = F->L->doc;
    int32_t i, ncol = 0, trials = 0, *ends = NULL;
    int64_t cost, *costs = NULL;
    pd_status st = PD_OK;
    int32_t nloose = 0;

    for (i = 0; i < F->n; i++) {
        if (F->it[i].kind == VI_FLOAT) {
            return PD_OK;   /* floats are placed while filling: greedy */
        }
    }

    if ((F->loose = (int8_t*)calloc((size_t)d->captab + 1, 1)) == NULL || plan_arrays(F, &ends, &costs)) {
        st = PD_ERR_NOMEM;
        goto out;
    }

    cost = optimal_breaks(F, F->chosen, ends, costs, &ncol);

    while (cost >= 0 && trials < VARIANT_TRIALS) {
        pd_block_id cands[32], best_b = 0;
        int32_t nc = 0, pick, best_delta = 0, k, delta;
        int64_t best_cost = cost;

        /* paragraphs in and right after the costliest columns, from the current flow */
        for (pick = 0; pick < 6 && nc < 28; pick++) {
            int32_t c, worst = -1;

            for (c = 1; c < ncol; c++) {    /* ends[] runs backwards; column 0 is the last one */
                if (costs[c] >= VARIANT_DEMERITS && (worst < 0 || costs[c] > costs[worst])) {
                    worst = c;
                }
            }

            if (worst < 0) {
                break;
            }

            costs[worst] = 0;
            nc += paras_between(F, worst + 1 < ncol ? ends[worst + 1] : -1, ends[worst], cands + nc, 3);

            for (k = ends[worst] + 1; k < F->n; k++) {      /* the paragraph that goes on after the break */
                if (F->it[k].kind == VI_LINE) {
                    if (F->it[k].pc->ws_k == 0 && F->it[k].pc->nlines > 1) {
                        cands[nc++] = F->it[k].block;
                    }

                    break;
                }
            }
        }

        for (k = 0; k < nc && trials < VARIANT_TRIALS; k++) {
            int32_t j, dup = 0;

            for (j = 0; j < k; j++) {
                dup |= cands[j] == cands[k];
            }

            if (dup || F->loose[cands[k]] != 0) {
                continue;
            }

            for (delta = -1; delta <= 1; delta += 2) {
                int64_t t;
                int32_t n2 = 0;

                if (!variant_ok(F, cands[k], delta)) {
                    continue;
                }

                F->loose[cands[k]] = (int8_t)delta;
                trials++;

                if ((st = rebuild_flow(F)) != PD_OK || plan_arrays(F, NULL, NULL)) {
                    st = st != PD_OK ? st : PD_ERR_NOMEM;
                    goto out;
                }

                t = optimal_breaks(F, F->chosen, NULL, NULL, &n2);

                if (t >= 0 && t + VARIANT_DEMERITS < best_cost) {
                    best_cost = t + VARIANT_DEMERITS;
                    best_b = cands[k];
                    best_delta = delta;
                }

                F->loose[cands[k]] = 0;
            }
        }

        if (!best_b) {
            break;
        }

        F->loose[best_b] = (int8_t)best_delta;
        nloose++;

        if ((st = rebuild_flow(F)) != PD_OK || plan_arrays(F, &ends, &costs)) {
            st = st != PD_OK ? st : PD_ERR_NOMEM;
            goto out;
        }

        cost = optimal_breaks(F, F->chosen, ends, costs, &ncol);
    }

    /* the flow of the final choice, and its breaks */
    if ((st = rebuild_flow(F)) != PD_OK || plan_arrays(F, NULL, NULL)) {
        st = st != PD_OK ? st : PD_ERR_NOMEM;
        goto out;
    }

    if (optimal_breaks(F, F->chosen, NULL, NULL, &ncol) < 0) {   /* no feasible plan: greedy */
        free(F->chosen);
        F->chosen = NULL;
    }

    F->L->info.variants += nloose;
out:
    free(ends);
    free(costs);
    return st;
}

/* ------------------------------------------------------------------ */
/* column balancing before a continuous section                       */
/* ------------------------------------------------------------------ */

static void drop_pages_after(pd_layout* L, int32_t page) {
    while (L->npages - 1 > page) {
        ppage* p = &L->pages[--L->npages];

        free(p->lines);
        free(p->rules);
        free(p->owned);
        free(p->pts);
        free(p->items);
    }
}

/* refill the last page's content into columns of height h; 1 if it fits on that page */
static int refill(filler* F, int32_t page, int32_t start, int32_t start_n, int32_t start_nrules, int32_t number,
                  int32_t section_page, pd_sp h) {
    ppage* p = &F->L->pages[page];

    drop_pages_after(F->L, page);
    p->n = start_n;
    p->nrules = start_nrules;
    F->page = page;
    F->number = number;
    F->section_page = section_page;
    F->colh = h;
    F->resume = 1;
    fill(F, start);
    F->resume = 0;
    return F->L->npages - 1 == page;
}

/* the last page of a multi-column section, columns made even (the shortest height that holds it all) */
static void balance(filler* F) {
    int32_t page = F->page, start = F->page_start_item, sn = F->page_start_n, sr = F->page_start_nrules;
    int32_t number = F->number, section_page = F->section_page, overfull = F->L->info.overfull;
    pd_sp colh = F->colh, lo, hi;
    uint8_t* chosen = F->chosen;

    if (F->ncols < 2 || F->nfl > 0 || start >= F->n) {
        return;
    }

    F->chosen = NULL;
    lo = col_floor(F);
    hi = colh;

    while (hi - lo > PD_PT(1)) {
        pd_sp mid = lo + (hi - lo) / 2;

        F->L->info.overfull = overfull;

        if (refill(F, page, start, sn, sr, number, section_page, mid) && F->L->info.overfull == overfull) {
            hi = mid;
        } else {
            lo = mid;
        }
    }

    F->L->info.overfull = overfull;
    refill(F, page, start, sn, sr, number, section_page, hi);
    F->colh = colh;
    F->chosen = chosen;
}

/* ------------------------------------------------------------------ */
/* update                                                             */
/* ------------------------------------------------------------------ */

/* a paragraph of a header or footer, where it goes relative to the story's top left */
typedef struct {
    pcache* c;
    pd_sp x, y;
} spart;

/* lay one paragraph of a story out at a width and add it to the parts */
static pcache* story_para(pd_layout* L, pd_block_id id, pd_sp width, pd_sp x, pd_sp y, pd_field_fn fn, void* user,
                          spart** parts, int32_t* n, int32_t* cap) {
    pcache* c = (pcache*)calloc(1, sizeof(pcache));

    if (!c || pd_para_new(&c->para) != PD_OK || grow((void**)parts, cap, (int64_t)*n + 1, sizeof(spart)) ||
            build_into(L, c, id, width, fn, user) != PD_OK) {
        pcache_free(c);
        return NULL;
    }

    (*parts)[*n].c = c;
    (*parts)[*n].x = x;
    (*parts)[*n].y = y;
    (*n)++;
    return c;
}

/* A header's or footer's paragraphs, laid out at the text width: one under
   the other, and a picture float that wraps (a logo) at its side of the
   width, the paragraphs that start beside it narrower by its width and gap.
   Returns the story's height; the parts' layouts belong to the caller. */
static pd_sp story_flow(pd_layout* L, pd_block_id story, pd_sp tw, pd_field_fn fn, void* user, spart** parts,
                        int32_t* n) {
    const pd_doc* d = L->doc;
    const blk* sb = pd_doc_blk(d, story);
    pd_block_id prev = 0;
    pd_sp y = 0, prev_after = 0, fl_bottom = 0, fl_w = 0, end = 0;
    int32_t i, cap = 0, fl_side = 0;

    *parts = NULL;
    *n = 0;

    for (i = 0; sb && i < sb->nkids; i++) {
        const blk* k = d->tab[sb->kids[i]];

        if (k->kind == PD_BLOCK_FLOAT && (k->st.fp.wrap == PD_WRAP_LEFT || k->st.fp.wrap == PD_WRAP_RIGHT)) {
            pd_block_id ids[16];
            int32_t m = 0, j;
            pd_sp fw = k->st.fp.width > 0 && k->st.fp.width < tw ? k->st.fp.width : tw / 3, fy = 0, fx, taken;

            taken = float_taken(&k->st.fp, fw, tw, &fx);
            collect_paras(d, k->id, ids, &m, 16);

            for (j = 0; j < m && j < 16; j++) {
                pcache* c = story_para(L, ids[j], fw, fx, y + fy, fn, user, parts, n, &cap);

                fy += c ? c->height : 0;
            }

            fl_side = k->st.fp.wrap;
            fl_w = taken;
            fl_bottom = y + fy;
            end = fl_bottom > end ? fl_bottom : end;
            continue;
        }

        {
            pd_block_id ids[64];
            int32_t m = 0, j;

            collect_paras(d, k->id, ids, &m, 64);

            for (j = 0; j < m && j < 64; j++) {
                pd_sp gy = y, beside, x = 0;
                pcache* c;

                if (prev) {     /* the gap needs the paragraph's own properties: lay it out first at full width */
                    pd_para_props pp;

                    pd_doc_effective_pp(d, pd_doc_blk(d, ids[j]), &pp, NULL);
                    gy = y + para_gap(d, prev, prev_after, ids[j], &pp);
                }

                beside = gy < fl_bottom ? fl_w : 0;
                x = beside && fl_side == PD_WRAP_LEFT ? fl_w : 0;
                c = story_para(L, ids[j], tw - beside > PD_PT(36) ? tw - beside : tw, x, gy, fn, user, parts, n, &cap);

                if (!c) {
                    continue;
                }

                y = gy + c->height;
                end = y > end ? y : end;
                prev = ids[j];
                prev_after = c->pp.space_after;
            }
        }
    }

    return end;
}

/* a header or footer, laid out for this page so its fields have their values */
static void place_story(pd_layout* L, int32_t page, pd_block_id story, int footer) {
    ppage* p = &L->pages[page];
    spart* parts;
    int32_t n, i, k;
    pd_sp h, y;
    page_ctx ctx;

    if (!story || !pd_doc_blk(L->doc, story)) {
        return;
    }

    ctx.L = L;
    ctx.page = page;
    h = story_flow(L, story, p->text_w, page_field, &ctx, &parts, &n);
    y = footer ? p->h - p->sp->footer_distance - h : p->sp->header_distance;

    for (i = 0; i < n; i++) {
        if (grow((void**)&p->owned, &p->capowned, (int64_t)p->nowned + 1, sizeof(pcache*))) {
            for (; i < n; i++) {
                pcache_free(parts[i].c);
            }

            break;
        }

        p->owned[p->nowned++] = parts[i].c;

        for (k = 0; k < parts[i].c->nlines; k++) {
            add_line(L, page, parts[i].c, k, p->text_x + parts[i].x, y + parts[i].y, footer ? 2 : 1);
        }
    }

    free(parts);
}

/* a header's or footer's height at a width, its fields at their placeholders */
static pd_sp story_height(pd_layout* L, pd_block_id story, pd_sp tw) {
    spart* parts;
    int32_t n, i;
    pd_sp h = story_flow(L, story, tw, body_field, L, &parts, &n);

    for (i = 0; i < n; i++) {
        pcache_free(parts[i].c);
    }

    free(parts);
    return h;
}

void pd_layout_set_stable_breaks(pd_layout* L, int32_t mode) {
    if (L) {
        L->stable = mode < 0 ? -1 : mode > 0;
    }
}

pd_status pd_layout_new(const pd_doc* doc, pd_layout** out) {
    pd_layout* L;

    if (!doc || !out) {
        return PD_ERR_ARG;
    }

    L = (pd_layout*)calloc(1, sizeof(pd_layout));

    if (!L || pd_para_new(&L->scratch) != PD_OK) {
        free(L);
        return PD_ERR_NOMEM;
    }

    L->stable = -1;

    L->doc = doc;
    *out = L;
    return PD_OK;
}

static void clear_pages(pd_layout* L) {
    int32_t i, k;

    for (i = 0; i < L->npages; i++) {
        free(L->pages[i].lines);
        free(L->pages[i].rules);
        free(L->pages[i].pts);
        free(L->pages[i].items);

        for (k = 0; k < L->pages[i].nowned; k++) {
            pcache_free(L->pages[i].owned[k]);
        }

        free(L->pages[i].owned);
    }

    L->npages = 0;
}

void pd_layout_free(pd_layout* L) {
    uint32_t k;

    if (!L) {
        return;
    }

    clear_pages(L);

    for (k = 0; k < L->ncache; k++) {
        pcache_free(L->cache[k]);
    }

    free(L->cache);
    free(L->pages);
    free(L->fv);
    free(L->endnotes);
    free(L->first_page);
    free(L->prev_first_page);
    free(L->prev_labels);
    free(L->fly);
    pd_para_free(L->scratch);
    free(L);
}

void pd_layout_invalidate(pd_layout* L) {
    if (L) {
        L->epoch++;
    }
}

static void run_counters(pd_layout* L) {
    const pd_doc* d = L->doc;
    blk* sr = pd_doc_blk(d, PD_STORYROOT_ID);
    seqc seq[32];
    int32_t nseq = 0, footnotes = 0, i;
    int32_t (*lcnt)[9] = (int32_t (*)[9])calloc(64, sizeof(*lcnt));
    int32_t (*lseen)[9] = (int32_t (*)[9])calloc(64, sizeof(*lseen));

    uint32_t k;

    L->nfv = 0;
    L->nendnotes = 0;

    for (k = 0; k < L->ncache; k++) {   /* labels are recomputed from scratch */
        pcache* c;

        for (c = L->cache[k]; c; c = c->next) {
            c->label[0] = '\0';
            c->note = 0;
        }
    }

    if (lcnt && lseen) {
        count_walk(L, PD_ROOT_ID, seq, &nseq, &footnotes, lcnt, lseen);

        for (i = 0; sr && i < sr->nkids; i++) {
            count_walk(L, sr->kids[i], seq, &nseq, &footnotes, lcnt, lseen);
        }
    }

    if (L->nfv > 1) {
        qsort(L->fv, (size_t)L->nfv, sizeof(fieldval), fv_cmp);
    }

    free(lcnt);
    free(lseen);
}

static pd_sp story_height(pd_layout* L, pd_block_id story, pd_sp tw);

/* The text area of a section's pages, top and height: inside the margins,
   or clear of a header or footer that reaches past them -- Word moves the
   text down (or the bottom up) rather than letting them overlap. The
   tallest of the section's headers (and footers) counts for all its pages. */
static void body_area(pd_layout* L, const pd_section_props* sp, pd_sp* top, pd_sp* height) {
    pd_block_id hs[3] = { sp->header, sp->title_page ? sp->header_first : 0, sp->facing_pages ? sp->header_even : 0 };
    pd_block_id fs[3] = { sp->footer, sp->title_page ? sp->footer_first : 0, sp->facing_pages ? sp->footer_even : 0 };
    pd_sp tw = sp->page_width - sp->margin_left - sp->margin_right, bottom = sp->margin_bottom, h;
    int k;

    *top = sp->margin_top;

    for (k = 0; k < 3 && tw > 0; k++) {
        if (hs[k] && pd_doc_blk(L->doc, hs[k]) && (h = story_height(L, hs[k], tw)) > 0 &&
                sp->header_distance + h > *top) {
            *top = sp->header_distance + h;
        }

        if (fs[k] && pd_doc_blk(L->doc, fs[k]) && (h = story_height(L, fs[k], tw)) > 0 &&
                sp->footer_distance + h > bottom) {
            bottom = sp->footer_distance + h;
        }
    }

    *height = sp->page_height - *top - bottom;

    if (*height < PD_PT(72) && sp->page_height - sp->margin_top - sp->margin_bottom >= PD_PT(72)) {
        *top = sp->margin_top;      /* headers that would leave no room: the margins as they are */
        *height = sp->page_height - sp->margin_top - sp->margin_bottom;
    }
}

static pd_status paginate(pd_layout* L) {
    const pd_doc* d = L->doc;
    blk* root;
    int32_t s, i, number = 1;
    uint32_t k;
    pd_status st = PD_OK;

    clear_pages(L);

    for (k = 0; k < L->ncache; k++) {
        pcache* c;

        for (c = L->cache[k]; c; c = c->next) {
            c->used = 0;
        }
    }

    free(L->first_page);
    L->first_page = (int32_t*)malloc((size_t)(d->captab + 1) * sizeof(int32_t));

    if (!L->first_page) {
        return PD_ERR_NOMEM;
    }

    for (k = 0; k <= d->captab; k++) {
        L->first_page[k] = -1;
    }

    root = pd_doc_blk(d, PD_ROOT_ID);

    for (s = 0; root && s < root->nkids && st == PD_OK; s++) {
        filler F;
        blk* sec = d->tab[root->kids[s]];
        const blk* nsec = s + 1 < root->nkids ? d->tab[root->kids[s + 1]] : NULL;

        memset(&F, 0, sizeof(F));
        F.L = L;
        F.sec = sec;
        F.sp = &sec->st.sp;
        F.ncols = F.sp->columns < 1 ? 1 : F.sp->columns;
        F.gap = F.sp->column_gap;
        body_area(L, F.sp, &F.top, &F.colh);
        F.colw = (F.sp->page_width - F.sp->margin_left - F.sp->margin_right - F.sp->gutter - (F.ncols - 1) * F.gap) /
                 F.ncols;
        F.number = F.sp->first_page_number > 0 && !F.sp->continuous ? F.sp->first_page_number : number;
        F.floor_page = -1;

        if (F.colw <= 0 || F.colh <= 0) {
            st = PD_ERR_RANGE;
            break;
        }

        /* a continuous section goes on below the previous one, if that page has room and the same size */
        if (s > 0 && F.sp->continuous && L->npages > 0) {
            const ppage* lp = &L->pages[L->npages - 1];
            pd_sp bottom = 0;
            int notes = 0;

            for (i = 0; i < lp->n; i++) {
                notes |= lp->lines[i].region == 4;
                bottom = (lp->lines[i].region == 0 || lp->lines[i].region == 3) && lp->lines[i].bottom > bottom ?
                         lp->lines[i].bottom : bottom;
            }

            for (i = 0; i < lp->nrules; i++) {
                notes |= lp->rules[i].region == 4;
                bottom = lp->rules[i].region == 0 && lp->rules[i].y + lp->rules[i].h > bottom ?
                         lp->rules[i].y + lp->rules[i].h : bottom;
            }

            bottom += PD_PT(12) - F.top;

            if (lp->w == F.sp->page_width && lp->h == F.sp->page_height && !lp->float_page && !notes &&
                    bottom > 0 && bottom < F.colh - PD_PT(36)) {
                F.floor_page = F.page = L->npages - 1;
                F.floor_y = bottom;
                F.resume = 1;
                F.number = number;
                F.section_page = 1;
            }
        }

        st = rebuild_flow(&F);

        if (st == PD_OK && F.sp->page_breaking == PD_PAGES_OPTIMAL) {
            st = plan_optimal(&F);
        }

        if (st == PD_OK) {
            st = fill(&F, 0);
        }

        F.resume = 0;

        /* columns before a continuous section are balanced */
        if (st == PD_OK && nsec && nsec->st.sp.continuous && nsec->st.sp.page_width == F.sp->page_width &&
                nsec->st.sp.page_height == F.sp->page_height) {
            balance(&F);
        }

        number = F.number;

        for (i = 0; i < F.ntb; i++) {
            free(F.tb[i].hdr_item);
            free(F.tb[i].rowh);
        }

        free(F.it);
        free(F.fl);
        free(F.queue);
        free(F.notes);
        free(F.tb);
        free(F.chosen);
        free(F.loose);
    }

    /* list labels into the paragraph caches, now that every paragraph has one */
    run_counters(L);

    for (i = 0; i < L->npages && st == PD_OK; i++) {
        const pd_section_props* sp = L->pages[i].sp;
        int first_page = L->pages[i].section_index == 0 && sp->title_page;
        int even = sp->facing_pages && L->pages[i].number % 2 == 0;

        place_story(L, i, first_page ? sp->header_first : even ? sp->header_even : sp->header, 0);
        place_story(L, i, first_page ? sp->footer_first : even ? sp->footer_even : sp->footer, 1);
    }

    return st;
}

/* remember page numbers and labels for fields that refer to pages */
static int remember_pages(pd_layout* L) {
    const pd_doc* d = L->doc;
    int32_t i;

    free(L->prev_first_page);
    free(L->prev_labels);
    L->prev_first_page = (int32_t*)malloc((size_t)(d->captab + 1) * sizeof(int32_t));
    L->prev_labels = (char (*)[16])malloc((size_t)(L->npages + 1) * 16);

    if (!L->prev_first_page || !L->prev_labels) {
        return -1;
    }

    memcpy(L->prev_first_page, L->first_page, (size_t)(d->captab + 1) * sizeof(int32_t));

    for (i = 0; i < L->npages; i++) {
        memcpy(L->prev_labels[i], L->pages[i].label, 16);
    }

    L->prev_npages = L->npages;
    return 0;
}

/* a body line that gets a number: not in a table, in a section that numbers its lines */
static const pd_section_props* line_numbered(const pd_layout* L, const pline* l) {
    const pd_doc* d = L->doc;
    const blk* b;
    const pd_section_props* sp;
    int32_t hops;

    if (l->region != 0 || l->line < 0 || !l->pc) {
        return NULL;
    }

    for (b = pd_doc_blk(d, l->pc->block), hops = 0; b && b->kind != PD_BLOCK_SECTION && hops < 64; hops++) {
        if (b->kind == PD_BLOCK_CELL || b->kind == PD_BLOCK_FLOAT) {
            return NULL;
        }

        b = b->parent ? pd_doc_blk(d, b->parent) : NULL;
    }

    sp = b && b->kind == PD_BLOCK_SECTION ? &b->st.sp : NULL;
    return sp && sp->line_numbers > 0 ? sp : NULL;
}

/* the count of the next numbered line: *cnt and *sec carried along the lines in order */
static int32_t line_number_step(const pd_section_props* sp, const pd_section_props** sec, int first_on_page,
                                int32_t* cnt) {
    if (sp != *sec && (sp->line_number_restart != PD_LINENUM_CONTINUOUS || !*sec)) {
        *cnt = 0;   /* a new section's numbering */
    }

    if (sp->line_number_restart == PD_LINENUM_PAGE && first_on_page) {
        *cnt = 0;
    }

    *sec = sp;
    return (*cnt)++;
}

/* where each page's line numbers start, after pagination */
static void number_lines(pd_layout* L) {
    const pd_section_props* sec = NULL;
    int32_t pg, i, cnt = 0;

    for (pg = 0; pg < L->npages; pg++) {
        ppage* p = &L->pages[pg];
        int first = 1;

        p->lnum_first = -1;

        for (i = 0; i < p->n; i++) {
            const pd_section_props* sp = line_numbered(L, &p->lines[i]);

            if (sp) {
                if (p->lnum_first < 0) {
                    const pd_section_props* s0 = sec;
                    int32_t c0 = cnt;

                    line_number_step(sp, &s0, 1, &c0);
                    p->lnum_first = c0 - 1;
                }

                line_number_step(sp, &sec, first, &cnt);
                first = 0;
            }
        }
    }
}

pd_status pd_layout_update(pd_layout* L, pd_layout_info* info) {
    pd_status st;
    uint32_t k;
    int pass;

    if (!L) {
        return PD_ERR_ARG;
    }

    memset(&L->info, 0, sizeof(L->info));
    run_counters(L);    /* caption numbers and footnote marks size their fields */

    /* page references and page counts settle in a second pass, as in LaTeX */
    for (pass = 0; pass < 2; pass++) {
        L->want_rerun = 0;
        L->info.float_pages = 0;
        L->info.overfull = 0;
        st = paginate(L);

        if (st != PD_OK || remember_pages(L)) {
            return st != PD_OK ? st : PD_ERR_NOMEM;
        }

        if (!L->want_rerun) {
            break;
        }
    }

    /* layouts no longer in use (paragraphs deleted, variants dropped) leave the cache */
    for (k = 0; k < L->ncache; k++) {
        pcache** pp = &L->cache[k];

        while (*pp) {
            if (!(*pp)->used) {
                pcache* dead = *pp;

                *pp = dead->next;
                dead->next = NULL;
                pcache_free(dead);
            } else {
                pp = &(*pp)->next;
            }
        }
    }

    number_lines(L);
    L->info.pages = L->npages;

    if (info) {
        *info = L->info;
    }

    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* queries                                                            */
/* ------------------------------------------------------------------ */

const pd_doc* pd_layout_doc(const pd_layout* L) {
    return L ? L->doc : NULL;
}

int32_t pd_layout_page_count(const pd_layout* L) {
    return L ? L->npages : 0;
}

pd_status pd_layout_page_info(const pd_layout* L, int32_t page, pd_page_info* out) {
    const ppage* p;
    int32_t i;

    if (!L || !out) {
        return PD_ERR_ARG;
    }

    if (page < 0 || page >= L->npages) {
        return PD_ERR_RANGE;
    }

    p = &L->pages[page];
    memset(out, 0, sizeof(*out));
    out->width = p->w;
    out->height = p->h;
    out->section = p->section;
    out->number = p->number;
    memcpy(out->label, p->label, sizeof(out->label));
    out->float_page = p->float_page;

    for (i = 0; i < p->n; i++) {
        const pline* l = &p->lines[i];
        pd_line ln;

        if (l->region != 0) {
            continue;
        }

        pd_para_get_line(l->pc->para, l->line, &ln);

        if (!out->first.block) {
            out->first.block = l->pc->block;
            out->first.offset = ln.text_start;
        }

        out->last.block = l->pc->block;
        out->last.offset = ln.text_end;
    }

    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* display list                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_draw* d;
    int32_t n, cap;
    int err;
    pd_sp* pts;                 /* the paths' points: items hold an index here until the list is handed out */
    int32_t npts, cappts;
} dlist_t;

static void emit(dlist_t* D, const pd_draw* x) {
    if (grow((void**)&D->d, &D->cap, (int64_t)D->n + 1, sizeof(pd_draw))) {
        D->err = 1;
        return;
    }

    D->d[D->n++] = *x;
}

/* a path through n points (x, y pairs): filled, stroked or both */
static void emit_path(dlist_t* D, const pd_sp* xy, int32_t n, int closed, uint32_t fill, uint32_t line, pd_sp lw,
                      pd_block_id block, int32_t region) {
    pd_draw a;
    pd_sp x0, y0, x1, y1;
    int32_t i;

    if (n < 2 || (!fill && (!line || lw <= 0)) ||
            grow((void**)&D->pts, &D->cappts, (int64_t)D->npts + 2 * n, sizeof(pd_sp))) {
        return;
    }

    memset(&a, 0, sizeof(a));
    x0 = y0 = INT32_MAX;
    x1 = y1 = INT32_MIN;

    for (i = 0; i < n; i++) {
        pd_sp px = xy[2 * i], py = xy[2 * i + 1];

        D->pts[D->npts + 2 * i] = px;
        D->pts[D->npts + 2 * i + 1] = py;

        if (px == PD_PATH_BREAK) {
            continue;
        }

        x0 = px < x0 ? px : x0;
        x1 = px > x1 ? px : x1;
        y0 = py < y0 ? py : y0;
        y1 = py > y1 ? py : y1;
    }

    if (x1 < x0) {
        return;
    }

    a.kind = PD_DRAW_PATH;
    a.x = x0;
    a.y = y0;
    a.w = x1 - x0;
    a.h = y1 - y0;
    a.points = (const pd_sp*)(intptr_t)D->npts;    /* an index for now */
    a.npoints = n;
    a.path_flags = closed ? PD_PATH_CLOSED : 0;
    a.fill = fill;
    a.color = line;
    a.line_width = line ? lw : 0;
    a.block = block;
    a.region = region;
    D->npts += 2 * n;
    emit(D, &a);
}

/* shape a short text (label, field value) in a style and emit it at a pen position */
/* the code point at a byte offset of UTF-8 text (soft hyphens show as hyphens) */
static uint32_t cp_at(const char* text, size_t len, uint32_t off) {
    const unsigned char* s = (const unsigned char*)text + off;
    uint32_t cp;
    int n, k;

    if (!text || off >= len) {
        return 0;
    }

    cp = s[0];
    n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    cp &= n ? 0x3F >> n : 0x7F;

    for (k = 1; k <= n && off + (uint32_t)k < len; k++) {
        cp = (cp << 6) | (s[k] & 0x3F);
    }

    return cp == 0xAD ? 0x2D : (cp == 0xFFFC || cp < 32) ? 0 : cp;
}

static pd_sp emit_text(const pd_layout* L, dlist_t* D, const char* text, const pd_style* st, pd_sp x, pd_sp y,
                       pd_block_id block, uint32_t off, int32_t region) {
    pd_params prm;
    pd_glyph g[64];
    int32_t n = 0, i;
    pd_sp end = x;

    pd_para_clear(L->scratch);
    pd_para_set_shape(L->scratch, 0, NULL, NULL);   /* measuring table cells may have left one */

    if (pd_para_add_text(L->scratch, text, strlen(text), st) != PD_OK) {
        return x;
    }

    pd_params_init(&prm);
    prm.width = PD_PT(30000);
    prm.align = PD_ALIGN_LEFT;
    prm.mode = PD_BREAK_GREEDY;

    if (pd_para_break(L->scratch, &prm, NULL) != PD_OK || pd_para_line_count(L->scratch) < 1) {
        return x;
    }

    pd_para_get_glyphs(L->scratch, 0, g, 64, &n);

    for (i = 0; i < n && i < 64; i++) {
        pd_draw a;

        memset(&a, 0, sizeof(a));
        a.kind = PD_DRAW_GLYPH;
        a.scale = 65536;
        a.x = x + g[i].x;
        a.y = y;
        a.w = g[i].advance;
        a.glyph = g[i].glyph;
        a.text = cp_at(text, strlen(text), g[i].cluster);
        a.font = st->font;
        a.size = st->size;
        a.color = st->color;
        a.block = block;
        a.offset = off;
        a.region = region;
        emit(D, &a);
        end = a.x + a.w;
    }

    return end;
}

static void field_text(const pd_layout* L, const ppage* p, const blk* b, const dinline* q, char* buf, size_t cap) {
    const pd_inline* o = &q->obj;
    int32_t v;

    buf[0] = '\0';

    if (o->kind == PD_INLINE_FOOTNOTE && o->level == 1) {
        pd_doc_format_number(fv_find(L, b->id, q->offset), PD_NUM_LOWER_ROMAN, buf, cap);
        return;
    }

    if (o->kind == PD_INLINE_FOOTNOTE) {
        snprintf(buf, cap, "%d", (int)fv_find(L, b->id, q->offset));
        return;
    }

    switch (o->field) {
        case PD_FIELD_PAGE:
            snprintf(buf, cap, "%s", p->label);
            break;

        case PD_FIELD_PAGES:
            snprintf(buf, cap, "%d", (int)L->npages);
            break;

        case PD_FIELD_SECTION_PAGE:
            snprintf(buf, cap, "%d", (int)p->section_index + 1);
            break;

        case PD_FIELD_SEQ:
            snprintf(buf, cap, "%d", (int)fv_find(L, b->id, q->offset));
            break;

        case PD_FIELD_REF_NUMBER:
        case PD_FIELD_REF_PAGE: {
            pd_block_id ids[64];
            int32_t n = 0, i, k;

            collect_paras(L->doc, o->target, ids, &n, 64);

            if (o->field == PD_FIELD_REF_PAGE) {
                if (n > 0 && ids[0] <= L->doc->captab && L->first_page[ids[0]] >= 0) {
                    snprintf(buf, cap, "%s", L->pages[L->first_page[ids[0]]].label);
                } else {
                    snprintf(buf, cap, "??");
                }

                break;
            }

            for (i = 0; i < n && i < 64; i++) {     /* the number of the target's first counter */
                const blk* t = pd_doc_blk(L->doc, ids[i]);

                for (k = 0; t && k < t->st.ninl; k++) {
                    if (t->st.inl[k].obj.kind == PD_INLINE_FIELD && t->st.inl[k].obj.field == PD_FIELD_SEQ) {
                        v = fv_find(L, t->id, t->st.inl[k].offset);
                        snprintf(buf, cap, "%d", (int)v);
                        return;
                    }
                }
            }

            snprintf(buf, cap, "??");
            break;
        }

        case PD_FIELD_HEADING: {
            pd_block_id h = o->level >= 1 && o->level <= 6 ? p->heading[o->level] : 0;
            const blk* t = pd_doc_blk(L->doc, h);
            size_t i, o2 = 0;

            for (i = 0; t && i < t->st.len && o2 + 1 < cap; i++) {     /* text without objects */
                if (i + 2 < t->st.len && memcmp(t->st.text + i, "\xEF\xBF\xBC", 3) == 0) {
                    i += 2;
                    continue;
                }

                buf[o2++] = t->st.text[i];
            }

            while (o2 > 0 && (buf[o2 - 1] & 0xC0) == 0x80) {   /* never end inside a character */
                o2--;
            }

            if (o2 > 0 && (unsigned char)buf[o2 - 1] >= 0xC0) {
                o2--;
            }

            buf[o2] = '\0';
            break;
        }
    }
}

/* How far an underline or a strike through glyph i reaches: its own
   advance, or on to the next glyph when what lies between them is spaces
   carrying the same decoration -- the inter-word glue is no glyph of its
   own, and Word draws the rule through an underlined space. which: 0
   underline, 1 strike. */
static pd_sp rule_reach(const pd_doc* d, const blk* b, const pd_glyph* g, int32_t n, int32_t i, int which) {
    uint32_t from, to, k;
    int32_t r;
    int space = 0;

    if (i + 1 >= n || g[i + 1].kind == PD_OBJECT || g[i + 1].y != g[i].y || g[i + 1].x <= g[i].x + g[i].advance) {
        return g[i].advance;
    }

    from = g[i].cluster;
    to = g[i + 1].cluster;

    if (to <= from || to > b->st.len) {
        return g[i].advance;
    }

    for (k = from; k < to; k++) {   /* the glyph's own characters, then only spaces */
        char c = b->st.text[k];

        if (c == ' ' || c == '\t') {
            space = 1;
        } else if (space) {
            return g[i].advance;
        }
    }

    for (k = from; k < to; k++) {
        if (b->st.text[k] == ' ' || b->st.text[k] == '\t') {
            pd_format_id f = b->st.empty_format;
            pd_char_props cp;

            for (r = 0; r < b->st.nruns; r++) {
                if (b->st.runs[r].start <= k && k < b->st.runs[r].end) {
                    f = b->st.runs[r].format;
                }
            }

            if (pd_doc_format_resolve(d, b->id, f, &cp) == PD_OK) {
                pd_doc_markup_props(d, &cp);
            }

            if (cp.hidden || !(which ? cp.strike : cp.underline)) {
                return g[i].advance;
            }
        }
    }

    return space ? g[i + 1].x - g[i].x : g[i].advance;
}

/* an underline of a kind over the span u describes (its top, width and
   thickness): double and thick rules, dots, dashes and waves as pieces on a
   grid of the page, so that neighbouring glyphs' pieces line up */
static void emit_underline(dlist_t* D, const pd_draw* u, int32_t kind) {
    pd_draw r = *u;
    pd_sp x, step, end = u->x + u->w;

    switch (kind) {
        case PD_UNDERLINE_DOUBLE:
            r.h = u->h * 2 / 3 > 1 ? u->h * 2 / 3 : 1;
            emit(D, &r);
            r.y += r.h * 2;
            emit(D, &r);
            return;

        case PD_UNDERLINE_THICK:
            r.h = u->h * 2;
            emit(D, &r);
            return;

        case PD_UNDERLINE_DOTTED:
        case PD_UNDERLINE_DASHED:
        case PD_UNDERLINE_WAVY:
            step = kind == PD_UNDERLINE_DOTTED ? u->h * 2 : kind == PD_UNDERLINE_DASHED ? u->h * 6 : u->h * 3;

            if (step <= 0 || u->w <= 0) {
                return;
            }

            for (x = u->x - (u->x % step + step) % step; x < end; x += step) {
                pd_sp a = x > u->x ? x : u->x, w = (kind == PD_UNDERLINE_DASHED ? step * 2 / 3 : step / 2);

                if (kind == PD_UNDERLINE_WAVY) {
                    w = step;   /* alternately up and down: a coarse wave */
                    r.y = u->y + ((x / step) & 1 ? u->h : 0);
                }

                w = (x + w < end ? x + w : end) - a;

                if (w > 0) {
                    r.x = a;
                    r.w = w;
                    emit(D, &r);
                }
            }

            return;

        default:
            emit(D, &r);
    }
}

/* what fills the space a tab leaves: dots or hyphens on a grid, so that the
   leaders of lines one above the other line up; or a rule */
static void emit_leaders(const pd_layout* L, dlist_t* D, const pline* l, const pd_glyph* g, int32_t n,
                         const pd_line* ln) {
    const pd_para* pa = l->pc->para;
    const pd_lineinfo* li;
    int32_t k, j;

    if (!pa || l->line < 0 || l->line >= pa->n_lines) {
        return;
    }

    li = &pa->lines[l->line];

    for (k = li->start; k < li->end && k < pa->n_items; k++) {
        const pd_item* it = &pa->items[k];
        pd_style ps;
        pd_sp x0, x1;
        int32_t prev = -1, next = -1;

        if (it->type != PD_ITEM_GLUE || !(it->flags & PD_FLAG_TAB) || it->user == PD_LEADER_NONE ||
                it->width <= 0 || it->style < 0 || pd_para_get_style(pa, it->style, &ps) != PD_OK) {
            continue;
        }

        for (j = 0; j < n; j++) {
            if (g[j].kind != PD_OBJECT && g[j].cluster < it->text_start) {
                prev = j;
            } else if (g[j].cluster >= it->text_end && next < 0) {
                next = j;
            }
        }

        x0 = l->ox + (prev >= 0 ? g[prev].x + g[prev].advance : ln->x);
        x1 = next >= 0 ? l->ox + g[next].x : x0 + it->width;

        if (it->user == PD_LEADER_UNDERSCORE) {
            pd_draw u;

            memset(&u, 0, sizeof(u));
            u.kind = PD_DRAW_RULE;
            u.x = x0;
            u.y = l->oy + ln->baseline + ps.size / 8;
            u.w = x1 - x0;
            u.h = ps.size / 20 > PD_SP_PER_PT / 3 ? ps.size / 20 : PD_SP_PER_PT / 3;
            u.color = ps.color;
            u.block = l->pc->block;
            u.offset = it->text_start;
            u.region = l->region;
            emit(D, &u);
        } else {
            const char* mark = it->user == PD_LEADER_DOT ? "." : "-";
            dlist_t M;
            pd_sp w, pitch, x;

            memset(&M, 0, sizeof(M));
            w = emit_text(L, &M, mark, &ps, 0, 0, l->pc->block, it->text_start, l->region);
            free(M.d);
            pitch = w * 2 > ps.size / 4 ? w * 2 : ps.size / 4;

            if (pitch <= 0) {
                continue;
            }

            for (x = l->ox + ((x0 - l->ox + pitch / 2) / pitch + 1) * pitch; x + w <= x1 - pitch / 2; x += pitch) {
                emit_text(L, D, mark, &ps, x, l->oy + ln->baseline, l->pc->block, it->text_start, l->region);
            }
        }
    }
}

/* a paragraph border or shading */
static int has_box(const pd_para_props* pp) {
    return (pp->border_color && pp->border_width > 0) || pp->shading;
}

/* paragraphs one after another that share one box */
static int same_box(const pd_para_props* a, const pd_para_props* b) {
    return a->border_color == b->border_color && a->border_width == b->border_width &&
           a->border_sides == b->border_sides && a->border_space == b->border_space && a->shading == b->shading &&
           a->indent_left == b->indent_left && a->indent_right == b->indent_right;
}

static void emit_rect(dlist_t* D, pd_sp x, pd_sp y, pd_sp w, pd_sp h, uint32_t color, pd_block_id block, int32_t region) {
    pd_draw a;

    if (w <= 0 || h <= 0) {
        return;
    }

    memset(&a, 0, sizeof(a));
    a.kind = PD_DRAW_RULE;
    a.x = x;
    a.y = y;
    a.w = w;
    a.h = h;
    a.color = color;
    a.block = block;
    a.region = region;
    emit(D, &a);
}

/* ------------------------------------------------------------------ */
/* drawings: a Word canvas or group, made one picture by the importer   */
/* ------------------------------------------------------------------ */

#define DRAWING_MIME "application/vnd.parade.drawing+json"

static void emit_rect(dlist_t* D, pd_sp x, pd_sp y, pd_sp w, pd_sp h, uint32_t color, pd_block_id block, int32_t region);

static pd_sp jsp(const pj_node* o, const char* k) {
    return (pd_sp)pj_int_or(pj_get(o, k), 0);
}

/* a text box of a drawing: its paragraphs set in its width less its insets,
   at its top, middle or bottom */
static pd_sp lay_table(const pd_layout* L, dlist_t* M, const pj_node* tbl, pd_sp x0, pd_sp y0, pd_sp width,
                       double sc, pd_block_id block, uint32_t off, int32_t region, int depth);

/* A drawing's text box's paragraphs (and tables) laid out into M, from (x0, 0) and width wide; their height */
static pd_sp lay_paras(const pd_layout* L, dlist_t* M, const pj_node* paras, pd_sp x0, pd_sp width, double sc,
                       pd_block_id block, uint32_t off, int32_t region, int depth) {
    const pj_node* p;
    pd_sp y = 0;
    int32_t i;

    for (p = paras->child; p; p = p->next) {
        const pj_node* runs = pj_get(p, "runs"), *r, *tbl = pj_get(p, "table");
        char text[4096];        /* the paragraph's text, for the code points drawn */
        size_t tl = 0;
        pd_params prm;
        pd_break_info bi;
        int32_t nl, li;

        if (tbl) {      /* a table among the paragraphs */
            if (depth < 4) {
                y += lay_table(L, M, tbl, x0, y, width, sc, block, off, region, depth + 1);
            }

            continue;
        }

        pd_para_clear(L->scratch);
        pd_para_set_shape(L->scratch, 0, NULL, NULL);

        for (r = runs ? runs->child : NULL; r; r = r->next) {
            const pj_node* tn = pj_get(r, "t"), *fn = pj_get(r, "f");
            pd_char_props c;
            pd_style st;

            if (pj_get(r, "img")) {     /* a picture in the line, sitting on its baseline */
                pd_sp iw = (pd_sp)(jsp(r, "w") * sc), ih = (pd_sp)(jsp(r, "h") * sc);

                if (iw > 0 && ih > 0 && tl + 3 < sizeof(text) &&
                        pd_para_add_object(L->scratch, iw, ih, 0, (int32_t)pj_int_or(pj_get(r, "img"), 0)) == PD_OK) {
                    memcpy(text + tl, "\xEF\xBF\xBC", 3);
                    tl += 3;
                }

                continue;
            }

            if (!tn || tn->type != PJ_STR || tn->len == 0) {
                continue;
            }

            memset(&c, 0, sizeof(c));
            c.size = (pd_sp)(jsp(r, "sz") * sc);
            c.size = c.size > 0 ? c.size : PD_PT(10);
            c.weight = (int32_t)pj_int_or(pj_get(r, "w"), 400);
            c.weight = c.weight > 0 ? c.weight : 400;
            c.italic = (int32_t)pj_int_or(pj_get(r, "i"), 0);
            c.color = (uint32_t)pj_int_or(pj_get(r, "c"), 0xFF000000LL);
            c.color = c.color ? c.color : 0xFF000000u;
            c.shift = (int32_t)pj_int_or(pj_get(r, "s"), 0);

            if (fn && fn->type == PJ_STR) {
                snprintf(c.family, sizeof(c.family), "%.*s", (int)(fn->len < 63 ? fn->len : 63), fn->s);
            }

            if (pd_doc_cp_style(L->doc, &c, &st) == PD_OK && (st.user = c.shift, 1) &&     /* raised or lowered below */
                    pd_para_add_text(L->scratch, tn->s, tn->len, &st) == PD_OK && tl + tn->len < sizeof(text)) {
                memcpy(text + tl, tn->s, tn->len);
                tl += tn->len;
            }
        }

        if (tl == 0) {      /* an empty paragraph: a line of the default size */
            pd_char_props c;
            pd_style st;

            memset(&c, 0, sizeof(c));
            c.size = (pd_sp)(PD_PT(10) * sc);
            c.weight = 400;
            c.color = 0xFF000000u;

            if (pd_doc_cp_style(L->doc, &c, &st) == PD_OK) {
                pd_para_add_text(L->scratch, "", 0, &st);
            }
        }

        pd_params_init(&prm);
        prm.width = width;
        prm.align = (int32_t)pj_int_or(pj_get(p, "a"), PD_ALIGN_LEFT);
        prm.mode = PD_BREAK_GREEDY;
        prm.full_lines = 1;

        if (pd_para_break(L->scratch, &prm, &bi) == PD_OK) {
            nl = pd_para_line_count(L->scratch);

            for (li = 0; li < nl; li++) {
                pd_line ln;
                pd_glyph g[512];
                int32_t n = 0;

                pd_para_get_line(L->scratch, li, &ln);
                pd_para_get_glyphs(L->scratch, li, g, 512, &n);

                for (i = 0; i < n && i < 512; i++) {
                    pd_style gs;
                    pd_draw a;

                    if (g[i].kind == PD_OBJECT) {
                        const pj_node* q;

                        for (q = runs ? runs->child : NULL; q; q = q->next) {   /* its height: its run's */
                            if (pj_int_or(pj_get(q, "img"), -1) == g[i].user) {
                                break;
                            }
                        }

                        memset(&a, 0, sizeof(a));
                        a.kind = PD_DRAW_IMAGE;
                        a.resource = (pd_res_id)g[i].user;
                        a.w = g[i].advance;
                        a.h = q ? (pd_sp)(jsp(q, "h") * sc) : g[i].advance;
                        a.x = x0 + g[i].x;
                        a.y = y + g[i].y - a.h;
                        a.block = block;
                        a.offset = off;
                        a.region = region;
                        emit(M, &a);
                        continue;
                    }

                    if (g[i].kind != PD_GLYPH || pd_para_get_style(L->scratch, g[i].style, &gs) != PD_OK) {
                        continue;
                    }

                    memset(&a, 0, sizeof(a));
                    a.kind = PD_DRAW_GLYPH;
                    a.scale = g[i].scale ? g[i].scale : 65536;
                    a.x = x0 + g[i].x;
                    a.y = y + g[i].y;
                    a.w = g[i].advance;
                    a.glyph = g[i].glyph;
                    a.text = tl ? cp_at(text, tl, g[i].cluster) : 0;
                    a.font = gs.font;
                    a.size = gs.size;
                    a.color = gs.color;
                    a.block = block;
                    a.offset = off;
                    a.region = region;

                    if (gs.user == PD_SHIFT_SUPER || gs.user == PD_SHIFT_SUB) {     /* as the text's: its size 7/10 */
                        pd_sp full = gs.size * 10 / 7;

                        a.y += gs.user == PD_SHIFT_SUPER ? -full / 3 : full / 6;
                    }

                    emit(M, &a);
                }
            }

            y += bi.height;
        }
    }

    return y;
}

/* A table of a text box ({"cols": widths in twips, "rules": width (0 none), "rows": [[cell...]...]}, a cell
   {"span", "bg", "paras"}) laid out into M at (x0, y0): its columns scaled to width when wider, each row as tall
   as its tallest cell, shading and rules under and over. Its height. */
static pd_sp lay_table(const pd_layout* L, dlist_t* M, const pj_node* tbl, pd_sp x0, pd_sp y0, pd_sp width,
                       double sc, pd_block_id block, uint32_t off, int32_t region, int depth) {
    const pj_node* cols = pj_get(tbl, "cols"), *rows = pj_get(tbl, "rows"), *row, *cell, *q;
    pd_sp colx[65], total = 0, y = y0, pad = (pd_sp)(PD_PT(5.4) * sc), rule = (pd_sp)(jsp(tbl, "rules") * sc);
    uint32_t rc = (uint32_t)pj_int_or(pj_get(tbl, "rc"), 0xFF000000LL);
    int32_t n = 0, k;
    double f;

    for (q = cols ? cols->child : NULL; q && n < 64; q = q->next) {
        colx[++n] = (pd_sp)(pj_int_or(q, 0) * 65536 / 20 * sc);
        total += colx[n];
    }

    if (n == 0 || total <= 0) {
        return 0;
    }

    f = total > width ? (double)width / total : 1;
    colx[0] = 0;

    for (k = 1; k <= n; k++) {
        colx[k] = colx[k - 1] + (pd_sp)(colx[k] * f);
    }

    k = (int32_t)pj_int_or(pj_get(tbl, "jc"), PD_ALIGN_LEFT);
    x0 += k == PD_ALIGN_CENTER ? (width - colx[n]) / 2 : k == PD_ALIGN_RIGHT ? width - colx[n] : 0;

    for (row = rows ? rows->child : NULL; row; row = row->next) {
        dlist_t R;
        pd_sp rh = 0, cx[65], cw[65];
        uint32_t bg[65];
        int32_t c = 0, nc = 0, start = 0;

        memset(&R, 0, sizeof(R));

        for (cell = row->child; cell && c < n && nc < 64; cell = cell->next) {   /* each cell laid out on its own */
            int32_t span = (int32_t)pj_int_or(pj_get(cell, "span"), 1), j;
            pd_sp ch;

            span = span < 1 ? 1 : c + span > n ? n - c : span;
            cx[nc] = colx[c];
            cw[nc] = colx[c + span] - colx[c];
            bg[nc] = (uint32_t)pj_int_or(pj_get(cell, "bg"), 0);
            start = R.n;
            ch = lay_paras(L, &R, pj_get(cell, "paras"), x0 + cx[nc] + pad, cw[nc] - 2 * pad > PD_PT(1) ?
                           cw[nc] - 2 * pad : PD_PT(1), sc, block, off, region, depth);

            for (j = start; j < R.n; j++) {
                R.d[j].y += y + rule;
            }

            rh = ch + 2 * rule > rh ? ch + 2 * rule : rh;
            c += span;
            nc++;
        }

        for (k = 0; k < nc; k++) {      /* shading under the text */
            if (bg[k]) {
                pd_draw a;

                memset(&a, 0, sizeof(a));
                a.kind = PD_DRAW_RULE;
                a.x = x0 + cx[k];
                a.y = y;
                a.w = cw[k];
                a.h = rh;
                a.color = bg[k];
                a.block = block;
                a.region = region;
                emit(M, &a);
            }
        }

        for (k = 0; k < R.n; k++) {
            emit(M, &R.d[k]);
        }

        free(R.d);
        free(R.pts);

        for (k = 0; rule > 0 && k < nc; k++) {  /* the grid's rules: each cell's box */
            pd_draw a;

            memset(&a, 0, sizeof(a));
            a.kind = PD_DRAW_RULE;
            a.color = rc;
            a.block = block;
            a.region = region;
            a.x = x0 + cx[k];
            a.y = y;
            a.w = cw[k];
            a.h = rule;
            emit(M, &a);
            a.y = y + rh - rule;
            emit(M, &a);
            a.y = y;
            a.w = rule;
            a.h = rh;
            emit(M, &a);
            a.x = x0 + cx[k] + cw[k] - rule;
            emit(M, &a);
        }

        y += rh;
    }

    return y - y0;
}

static void emit_textbox(const pd_layout* L, dlist_t* D, const pj_node* it, pd_sp bx, pd_sp by, pd_sp bw, pd_sp bh,
                         double sc, pd_block_id block, uint32_t off, int32_t region) {
    const pj_node* paras = pj_get(it, "text"), *ins = pj_get(it, "ins"), *an = pj_get(it, "anchor");
    pd_sp il = (pd_sp)(pj_int_or(pj_at(ins, 0), 0) * sc), it_ = (pd_sp)(pj_int_or(pj_at(ins, 1), 0) * sc);
    pd_sp ir = (pd_sp)(pj_int_or(pj_at(ins, 2), 0) * sc), ib = (pd_sp)(pj_int_or(pj_at(ins, 3), 0) * sc);
    pd_sp width = bw - il - ir, y = 0, dy;
    dlist_t M;
    int32_t i;

    if (!paras || width <= 0) {
        return;
    }

    memset(&M, 0, sizeof(M));

    y = lay_paras(L, &M, paras, bx + il, width, sc, block, off, region, 0);

    /* where the text sits in the box */
    dy = by + it_;

    if (an && an->type == PJ_STR && an->len >= 3 && !memcmp(an->s, "ctr", 3)) {
        dy = by + it_ + (bh - it_ - ib - y) / 2;
    } else if (an && an->type == PJ_STR && an->len >= 1 && an->s[0] == 'b') {
        dy = by + bh - ib - y;
    }

    for (i = 0; i < M.n; i++) {
        M.d[i].y += dy;
        emit(D, &M.d[i]);
    }

    free(M.d);
}

/* a single line of text of a drawing at its baseline point: left, centred or right (ha 0, 1, 2) of it */
static void emit_label(const pd_layout* L, dlist_t* D, const pj_node* it, pd_sp x, pd_sp y, double sc,
                       pd_block_id block, uint32_t off, int32_t region) {
    const pj_node* tn = pj_get(it, "label"), *fn = pj_get(it, "f");
    pd_char_props c;
    pd_style st;
    pd_params prm;
    pd_glyph g[256];
    pd_line ln;
    int32_t n = 0, i, ha = (int32_t)pj_int_or(pj_get(it, "ha"), 0);

    if (!tn || tn->type != PJ_STR || tn->len == 0) {
        return;
    }

    memset(&c, 0, sizeof(c));
    c.size = (pd_sp)(jsp(it, "sz") * sc);
    c.size = c.size > PD_SP_PER_PT / 4 ? c.size : PD_SP_PER_PT / 4;
    c.weight = (int32_t)pj_int_or(pj_get(it, "w"), 400);
    c.italic = (int32_t)pj_int_or(pj_get(it, "i"), 0);
    c.color = (uint32_t)pj_int_or(pj_get(it, "c"), 0xFF000000LL);

    if (fn && fn->type == PJ_STR) {
        snprintf(c.family, sizeof(c.family), "%.*s", (int)(fn->len < 63 ? fn->len : 63), fn->s);
    }

    pd_para_clear(L->scratch);
    pd_para_set_shape(L->scratch, 0, NULL, NULL);

    if (pd_doc_cp_style(L->doc, &c, &st) != PD_OK || pd_para_add_text(L->scratch, tn->s, tn->len, &st) != PD_OK) {
        return;
    }

    pd_params_init(&prm);
    prm.width = PD_PT(30000);
    prm.align = PD_ALIGN_LEFT;
    prm.mode = PD_BREAK_GREEDY;

    if (pd_para_break(L->scratch, &prm, NULL) != PD_OK || pd_para_get_line(L->scratch, 0, &ln) != PD_OK) {
        return;
    }

    pd_para_get_glyphs(L->scratch, 0, g, 256, &n);
    x -= ha == 1 ? ln.width / 2 : ha == 2 ? ln.width : 0;

    for (i = 0; i < n && i < 256; i++) {
        pd_draw a;

        if (g[i].kind != PD_GLYPH) {
            continue;
        }

        memset(&a, 0, sizeof(a));
        a.kind = PD_DRAW_GLYPH;
        a.scale = 65536;
        a.x = x + g[i].x - ln.x;
        a.y = y;
        a.w = g[i].advance;
        a.glyph = g[i].glyph;
        a.text = cp_at(tn->s, tn->len, g[i].cluster);
        a.font = st.font;
        a.size = st.size;
        a.color = st.color;
        a.block = block;
        a.offset = off;
        a.region = region;
        emit(D, &a);
    }
}

/* A drawing resource in the box (x, y, w, h): its pictures, its shapes'
   fills and straight edges, its text boxes. Returns 0 if res is no drawing. */
static int emit_drawing_at(const pd_layout* L, dlist_t* D, pd_res_id res, pd_sp x, pd_sp y, pd_sp w, pd_sp h,
                           pd_block_id block, uint32_t off, int32_t region, int depth) {
    const char* mime = NULL;
    const void* data = NULL;
    size_t len = 0;
    pj_doc* doc;
    const pj_node* root, *items, *it;
    double sx, sy;

    if (pd_doc_resource(L->doc, res, &mime, &data, &len) != PD_OK || !mime || strcmp(mime, DRAWING_MIME) != 0) {
        return 0;
    }

    if ((doc = pj_parse(data, len, 0, NULL)) == NULL) {
        return 1;   /* a drawing, but unreadable: nothing */
    }

    root = pj_root(doc);
    sx = jsp(root, "w") > 0 ? (double)w / jsp(root, "w") : 1;
    sy = jsp(root, "h") > 0 ? (double)h / jsp(root, "h") : 1;
    items = pj_get(root, "items");

    for (it = items ? items->child : NULL; it; it = it->next) {
        pd_sp ix = x + (pd_sp)(jsp(it, "x") * sx), iy = y + (pd_sp)(jsp(it, "y") * sy);
        pd_sp iw = (pd_sp)(jsp(it, "w") * sx), ih = (pd_sp)(jsp(it, "h") * sy);

        if (pj_get(it, "img")) {
            pd_draw a;
            pd_res_id cr = (pd_res_id)pj_int_or(pj_get(it, "img"), 0);

            if (cr != res && depth < 4 && emit_drawing_at(L, D, cr, ix, iy, iw, ih, block, off, region, depth + 1)) {
                continue;   /* a drawing of its own (a metafile in a canvas) */
            }

            memset(&a, 0, sizeof(a));
            a.kind = PD_DRAW_IMAGE;
            a.resource = cr;
            a.x = ix;
            a.y = iy;
            a.w = iw;
            a.h = ih;

            if (pj_get(it, "crop")) {   /* cropped: the whole picture, larger, shown only in its frame */
                const pj_node* cp = pj_get(it, "crop");
                double cl = pj_int_or(pj_at(cp, 0), 0) / 100000.0, ct = pj_int_or(pj_at(cp, 1), 0) / 100000.0;
                double cr2 = pj_int_or(pj_at(cp, 2), 0) / 100000.0, cb = pj_int_or(pj_at(cp, 3), 0) / 100000.0;

                if (cl + cr2 < 0.999 && ct + cb < 0.999) {
                    a.clip_x = ix;
                    a.clip_y = iy;
                    a.clip_w = iw;
                    a.clip_h = ih;
                    a.w = (pd_sp)(iw / (1 - cl - cr2));
                    a.h = (pd_sp)(ih / (1 - ct - cb));
                    a.x = ix - (pd_sp)(cl * a.w);
                    a.y = iy - (pd_sp)(ct * a.h);
                }
            }

            a.block = block;
            a.offset = off;
            a.region = region;
            emit(D, &a);
        } else if (pj_get(it, "shape")) {
            const pj_node* sh = pj_get(it, "shape");
            uint32_t fill = (uint32_t)pj_int_or(pj_get(it, "fill"), 0), line = (uint32_t)pj_int_or(pj_get(it, "line"), 0);
            pd_sp lw = (pd_sp)(jsp(it, "lw") * sx);
            int ell = sh->type == PJ_STR && sh->len == 7 && !memcmp(sh->s, "ellipse", 7);
            int ln = sh->type == PJ_STR && sh->len == 4 && !memcmp(sh->s, "line", 4);

            lw = lw > PD_SP_PER_PT / 4 ? lw : PD_SP_PER_PT / 4;

            if (ln) {           /* a straight line: drawn when it runs across or down */
                if (line && ih <= lw) {
                    emit_rect(D, ix, iy - lw / 2, iw, lw, line, block, region);
                } else if (line && iw <= lw) {
                    emit_rect(D, ix - lw / 2, iy, lw, ih, line, block, region);
                }
            } else if (ell) {   /* an ellipse: a polygon of 64 sides */
                pd_sp xy[128];
                int k;

                for (k = 0; k < 64; k++) {
                    double t = k * 6.283185307179586 / 64;

                    xy[2 * k] = ix + (pd_sp)(iw / 2.0 * (1 + cos(t)));
                    xy[2 * k + 1] = iy + (pd_sp)(ih / 2.0 * (1 + sin(t)));
                }

                emit_path(D, xy, 64, 1, fill, line, lw, block, region);
            } else {            /* a box: its fill, then its edges */
                if (fill) {
                    emit_rect(D, ix, iy, iw, ih, fill, block, region);
                }

                if (line) {
                    emit_rect(D, ix, iy, iw, lw, line, block, region);
                    emit_rect(D, ix, iy + ih - lw, iw, lw, line, block, region);
                    emit_rect(D, ix, iy, lw, ih, line, block, region);
                    emit_rect(D, ix + iw - lw, iy, lw, ih, line, block, region);
                }
            }
        } else if (pj_get(it, "text")) {
            emit_textbox(L, D, it, ix, iy, iw, ih, sx, block, off, region);
        } else if (pj_get(it, "label")) {
            emit_label(L, D, it, ix, iy, sy, block, off, region);
        } else if (pj_get(it, "path")) {    /* points in the drawing's coordinates */
            const pj_node* pts = pj_get(it, "path"), *q;
            int32_t n = pts->n / 2, k = 0;
            pd_sp* xy = n >= 2 ? (pd_sp*)malloc((size_t)n * 2 * sizeof(pd_sp)) : NULL;
            pd_sp lw = (pd_sp)(jsp(it, "lw") * sx);

            for (q = pts->child; xy && q && k < 2 * n; q = q->next, k++) {
                int64_t v = pj_int_or(q, 0);

                xy[k] = v == PD_PATH_BREAK ? PD_PATH_BREAK : (k & 1) ? y + (pd_sp)(v * sy) : x + (pd_sp)(v * sx);
            }

            if (xy) {
                emit_path(D, xy, n, (int)pj_int_or(pj_get(it, "closed"), 0), (uint32_t)pj_int_or(pj_get(it, "fill"), 0),
                          (uint32_t)pj_int_or(pj_get(it, "line"), 0), lw > PD_SP_PER_PT / 8 ? lw : PD_SP_PER_PT / 8, block,
                          region);
                free(xy);
            }
        }
    }

    pj_free(doc);
    return 1;
}

static int emit_drawing(const pd_layout* L, dlist_t* D, pd_res_id res, pd_sp x, pd_sp y, pd_sp w, pd_sp h,
                        pd_block_id block, uint32_t off, int32_t region) {
    return emit_drawing_at(L, D, res, x, y, w, h, block, off, region, 0);
}

/* Paragraph shading and borders, under the text: one box round the lines of
   paragraphs that share it, its top edge only where the first of them
   starts and its bottom where the last ends (a box broken by the page stays
   open), rules between them if the border asks for those. */
static void emit_para_boxes(dlist_t* D, const ppage* p) {
    int32_t i = 0, j, k;

    while (i < p->n) {
        const pline* l = &p->lines[i];
        const pd_para_props* pp;
        const pline* e;
        pd_sp x0, x1, y0, y1, bw;
        int sides;
        uint32_t bc;

        if (!l->pc || l->line < 0 || !has_box(&l->pc->pp)) {
            i++;
            continue;
        }

        pp = &l->pc->pp;

        for (j = i + 1; j < p->n; j++) {
            const pline* m = &p->lines[j], *q = &p->lines[j - 1];

            if (!m->pc || m->line < 0 || m->region != l->region || m->ox != l->ox || m->top < q->top ||
                    (m->pc->block != q->pc->block && !(m->line == 0 && q->line == q->pc->nlines - 1 &&
                            same_box(&m->pc->pp, pp)))) {
                break;
            }
        }

        e = &p->lines[j - 1];
        sides = pp->border_sides ? pp->border_sides : PD_BORDER_TOP | PD_BORDER_RIGHT | PD_BORDER_BOTTOM | PD_BORDER_LEFT;
        bw = pp->border_color ? pp->border_width : 0;
        bc = pp->border_color;
        x0 = l->ox + pp->indent_left - ((sides & PD_BORDER_LEFT) && bw ? pp->border_space : 0);
        x1 = l->ox + l->pc->width - pp->indent_right + ((sides & PD_BORDER_RIGHT) && bw ? pp->border_space : 0);
        y0 = l->top - (l->line == 0 && (sides & PD_BORDER_TOP) && bw ? pp->border_space : 0);
        y1 = e->bottom + (e->line == e->pc->nlines - 1 && (sides & PD_BORDER_BOTTOM) && bw ? pp->border_space : 0);

        if (pp->shading) {
            emit_rect(D, x0, y0, x1 - x0, y1 - y0, pp->shading, l->pc->block, l->region);
        }

        if (bw > 0) {
            pd_sp lx = x0 - ((sides & PD_BORDER_LEFT) ? bw : 0), rx = x1 + ((sides & PD_BORDER_RIGHT) ? bw : 0);

            if ((sides & PD_BORDER_TOP) && l->line == 0) {
                emit_rect(D, lx, y0 - bw, rx - lx, bw, bc, l->pc->block, l->region);
            }

            if ((sides & PD_BORDER_BOTTOM) && e->line == e->pc->nlines - 1) {
                emit_rect(D, lx, y1, rx - lx, bw, bc, l->pc->block, l->region);
            }

            if (sides & PD_BORDER_LEFT) {
                emit_rect(D, x0 - bw, y0, bw, y1 - y0, bc, l->pc->block, l->region);
            }

            if (sides & PD_BORDER_RIGHT) {
                emit_rect(D, x1, y0, bw, y1 - y0, bc, l->pc->block, l->region);
            }

            for (k = i + 1; (sides & PD_BORDER_BETWEEN) && k < j; k++) {
                const pline* a = &p->lines[k - 1], *b = &p->lines[k];

                if (a->pc->block != b->pc->block) {     /* half way between the two */
                    emit_rect(D, x0, (a->bottom + b->top - bw) / 2, x1 - x0, bw, bc, b->pc->block, b->region);
                }
            }
        }

        i = j;
    }
}

static void emit_line(const pd_layout* L, dlist_t* D, const ppage* p, const pline* l) {
    const pd_doc* d = L->doc;
    const blk* b = pd_doc_blk(d, l->pc->block);
    pd_glyph* g;
    int32_t n = 0, i;
    pd_line ln;
    int32_t cached_style = -1;
    pd_style ps;
    pd_char_props cp;

    if (!b) {
        return;
    }

    pd_para_get_line(l->pc->para, l->line, &ln);
    pd_para_get_glyphs(l->pc->para, l->line, NULL, 0, &n);
    g = (pd_glyph*)malloc(((size_t)n + 1) * sizeof(pd_glyph));

    if (!g) {
        D->err = 1;
        return;
    }

    pd_para_get_glyphs(l->pc->para, l->line, g, n, &n);
    memset(&ps, 0, sizeof(ps));
    memset(&cp, 0, sizeof(cp));

    /* the list label hangs left of the first line */
    if (l->line == 0 && l->pc->label[0] && n > 0) {
        pd_style ls;
        pd_char_props lcp;

        if (pd_doc_run_style(d, b->id, b->st.nruns ? b->st.runs[0].format : b->st.empty_format, &ls, &lcp) != PD_OK) {
            /* no style: no label */
        } else if (!l->pc->note && pd_doc_label_style(d, b->id, &ls) != PD_OK) {
            /* no font for it: no label */
        } else if (l->pc->note) {     /* a footnote's number: raised, small, just before its text */
            dlist_t M;
            pd_sp w;

            ls.size = ls.size * 7 / 10;
            memset(&M, 0, sizeof(M));
            w = emit_text(L, &M, l->pc->label, &ls, 0, 0, b->id, 0, l->region);
            free(M.d);
            emit_text(L, D, l->pc->label, &ls, l->ox + ln.x - w - PD_PT(1), l->oy + ln.baseline - ls.size / 2, b->id, 0,
                      l->region);
        } else if (b->st.at.task) {     /* a checklist's box, drawn, so that no font has to have one */
            char lab[32];
            size_t k = strlen(l->pc->label);
            pd_sp bx = l->ox + l->pc->label_x, side = ls.size * 3 / 4, bw = ls.size / 14 > PD_SP_PER_PT / 3 ?
                       ls.size / 14 : PD_SP_PER_PT / 3, top = l->oy + ln.baseline - side;
            pd_draw r;

            snprintf(lab, sizeof(lab), "%s", l->pc->label);

            if (k >= 3 && (memcmp(lab + k - 3, "\xE2\x98\x90", 3) == 0 || memcmp(lab + k - 3, "\xE2\x98\x91", 3) == 0)) {
                lab[k - 3] = '\0';     /* a number before it stays text */
                k -= 3;

                while (k > 0 && lab[k - 1] == ' ') {
                    lab[--k] = '\0';
                }
            }

            if (lab[0]) {
                bx += emit_text(L, D, lab, &ls, bx, l->oy + ln.baseline, b->id, 0, l->region) + ls.size / 4;
            }

            memset(&r, 0, sizeof(r));
            r.kind = PD_DRAW_RULE;
            r.color = ls.color;
            r.block = b->id;
            r.region = l->region;
#define BOXR(X, Y, W, H) do { r.x = (X); r.y = (Y); r.w = (W); r.h = (H); emit(D, &r); } while (0)
            BOXR(bx, top, side, bw);
            BOXR(bx, top + side - bw, side, bw);
            BOXR(bx, top, bw, side);
            BOXR(bx + side - bw, top, bw, side);

            if (b->st.at.task == 2) {   /* checked: filled inside */
                BOXR(bx + 3 * bw, top + 3 * bw, side - 6 * bw, side - 6 * bw);
            }
#undef BOXR
        } else {
            emit_text(L, D, l->pc->label, &ls, l->ox + l->pc->label_x, l->oy + ln.baseline, b->id, 0, l->region);
        }
    }

    for (i = 0; i < n; i++) {
        pd_draw a;

        memset(&a, 0, sizeof(a));
        a.block = b->id;
        a.offset = g[i].cluster;
        a.region = l->region;
        a.x = l->ox + g[i].x;
        a.y = l->oy + g[i].y;
        a.w = g[i].advance;

        if (g[i].style >= 0 && g[i].style != cached_style) {
            pd_para_get_style(l->pc->para, g[i].style, &ps);
            pd_doc_format_resolve(d, b->id, (pd_format_id)ps.user, &cp);
            pd_doc_markup_props(d, &cp);
            cached_style = g[i].style;
        }

        if (g[i].kind == PD_OBJECT) {
            const dinline* q = g[i].user >= 0 && g[i].user < b->st.ninl ? &b->st.inl[g[i].user] : NULL;

            if (!q) {
                continue;
            }

            a.h = q->obj.height + q->obj.depth;
            a.y = l->oy + g[i].y - q->obj.height;

            if (q->obj.kind == PD_INLINE_IMAGE) {   /* its natural size when none is set */
                pd_sp iw, ih;

                pd_doc_image_size(d, &q->obj, &iw, &ih);
                a.h = ih + q->obj.depth;
                a.y = l->oy + g[i].y - ih;
            }

            if (q->obj.kind == PD_INLINE_IMAGE) {
                pd_sp iw, ih;

                pd_doc_image_size(d, &q->obj, &iw, &ih);

                if (!emit_drawing(L, D, q->obj.resource, a.x, a.y, iw, ih, b->id, g[i].cluster, l->region)) {
                    a.kind = PD_DRAW_IMAGE;
                    a.resource = q->obj.resource;
                    emit(D, &a);
                }
            } else if (q->obj.kind == PD_INLINE_EQUATION && d->math_font && q->obj.source && q->obj.source_len > 0) {
                /* the formula itself: glyphs of the math font and rules */
                pd_math_item* mi = NULL;
                int32_t cnt = 0, k;
                pd_style es;
                pd_char_props ecp;
                pd_format_id ef = b->st.empty_format;
                int32_t r;

                for (r = 0; r < b->st.nruns; r++) {
                    if (b->st.runs[r].start <= q->offset && q->offset < b->st.runs[r].end) {
                        ef = b->st.runs[r].format;
                    }
                }

                if (pd_doc_run_style(d, b->id, ef, &es, &ecp) == PD_OK &&
                        pd_math_layout(d->math_font, ecp.size, q->obj.source, (size_t)q->obj.source_len,
                                       b->st.role == PD_ROLE_EQUATION, NULL, 0, &cnt, NULL) == PD_OK &&
                        (mi = (pd_math_item*)malloc(((size_t)cnt + 1) * sizeof(pd_math_item))) != NULL &&
                        pd_math_layout(d->math_font, ecp.size, q->obj.source, (size_t)q->obj.source_len,
                                       b->st.role == PD_ROLE_EQUATION, mi, cnt, &cnt, NULL) == PD_OK) {
                    for (k = 0; k < cnt; k++) {
                        pd_draw m = a;

                        m.x = a.x + mi[k].x;
                        m.color = ecp.color;
                        m.scale = 65536;

                        if (mi[k].kind == 1) {
                            m.kind = PD_DRAW_RULE;
                            m.y = l->oy + g[i].y + mi[k].y;
                            m.w = mi[k].w;
                            m.h = mi[k].h;
                        } else {
                            m.kind = PD_DRAW_GLYPH;
                            m.y = l->oy + g[i].y + mi[k].y;
                            m.glyph = mi[k].glyph;
                            m.font = d->math_font;
                            m.size = mi[k].size;
                            m.w = 0;
                            m.h = 0;
                            m.text = 0;
                        }

                        emit(D, &m);
                    }
                }

                free(mi);
            } else if (q->obj.kind == PD_INLINE_EQUATION || q->obj.kind == PD_INLINE_USER) {
                a.kind = PD_DRAW_BOX;
                emit(D, &a);
            } else if (q->obj.kind == PD_INLINE_RUBY && q->obj.source && q->obj.source_len > 0) {
                /* the guide above the text it is over, to its end or the line's, centred */
                char text[256];
                pd_style fs;
                pd_char_props fcp;
                pd_format_id ff = b->st.empty_format;
                pd_sp x1 = g[n - 1].x + g[n - 1].advance, w;
                int32_t j, r, depth = 0;
                dlist_t T;
                uint32_t c0;

                for (j = i + 1; j < n; j++) {
                    const dinline* e = g[j].kind == PD_OBJECT && g[j].user >= 0 && g[j].user < b->st.ninl ?
                                       &b->st.inl[g[j].user] : NULL;

                    if (e && e->obj.kind == PD_INLINE_RUBY) {
                        if (e->obj.source_len > 0) {
                            depth++;
                        } else if (depth-- == 0) {
                            x1 = g[j].x;
                            break;
                        }
                    }
                }

                for (r = 0; r < b->st.nruns; r++) {     /* the style of the text it is over */
                    if (b->st.runs[r].start <= q->offset + 3 && q->offset + 3 < b->st.runs[r].end) {
                        ff = b->st.runs[r].format;
                    }
                }

                if (pd_doc_run_style(d, b->id, ff, &fs, &fcp) != PD_OK) {
                    continue;
                }

                snprintf(text, sizeof(text), "%.*s", q->obj.source_len < 255 ? (int)q->obj.source_len : 255, q->obj.source);
                c0 = (unsigned char)text[0] < 0x80 ? (unsigned char)text[0] : (unsigned char)text[0] < 0xE0 ?
                     ((uint32_t)(text[0] & 0x1F) << 6 | (text[1] & 0x3F)) : (unsigned char)text[0] < 0xF0 ?
                     ((uint32_t)(text[0] & 0x0F) << 12 | (uint32_t)(text[1] & 0x3F) << 6 | (text[2] & 0x3F)) : 0;

                if (c0 && !pd_font_glyph_index(fs.font, c0)) {     /* kana a Latin face lacks: a fallback's */
                    int32_t k;

                    for (k = 0; k < d->nfallback; k++) {
                        if (pd_font_glyph_index(d->fallback[k], c0)) {
                            fs.font = d->fallback[k];
                            break;
                        }
                    }
                }

                fs.size = q->obj.height > 0 ? q->obj.height : fcp.size / 2;
                memset(&T, 0, sizeof(T));
                w = emit_text(L, &T, text, &fs, 0, 0, b->id, g[i].cluster, l->region);  /* measured */
                free(T.d);
                free(T.pts);
                emit_text(L, D, text, &fs, l->ox + (g[i].x + x1) / 2 - w / 2, l->oy + g[i].y -
                          (q->obj.depth > 0 ? q->obj.depth : fcp.size), b->id, g[i].cluster, l->region);
                ps = fs;
                cached_style = -1;
            } else if (q->obj.kind == PD_INLINE_FIELD || q->obj.kind == PD_INLINE_FOOTNOTE) {
                char text[96];
                pd_style fs;
                pd_char_props fcp;
                pd_format_id ff = b->st.empty_format;
                int32_t r;

                for (r = 0; r < b->st.nruns; r++) {     /* the style of the run holding the object */
                    if (b->st.runs[r].start <= q->offset && q->offset < b->st.runs[r].end) {
                        ff = b->st.runs[r].format;
                    }
                }

                if (pd_doc_run_style(d, b->id, ff, &fs, &fcp) != PD_OK) {
                    continue;
                }

                field_text(L, p, b, q, text, sizeof(text));
                ps = fs;
                cached_style = -1;  /* ps no longer matches the cached style index */

                if (q->obj.kind == PD_INLINE_FOOTNOTE) {    /* a raised, smaller mark */
                    fs.size = ps.size * 7 / 10;
                    emit_text(L, D, text, &fs, a.x, l->oy + g[i].y - ps.size / 3, b->id, g[i].cluster, l->region);
                } else {
                    emit_text(L, D, text, &fs, a.x, l->oy + g[i].y, b->id, g[i].cluster, l->region);
                }
            }

            continue;
        }

        if (cp.hidden) {
            continue;
        }

        if (cp.background) {
            pd_draw bg = a;

            bg.kind = PD_DRAW_RULE;
            bg.y = l->oy + ln.baseline - ln.ascent;
            bg.h = ln.ascent + ln.descent;
            bg.color = cp.background;
            emit(D, &bg);
        }

        a.kind = PD_DRAW_GLYPH;
        a.glyph = g[i].glyph;
        a.scale = g[i].scale ? g[i].scale : 65536;
        a.text = cp_at(b->st.text, b->st.len, g[i].cluster);
        a.font = ps.font;
        a.size = ps.size;
        a.color = ps.color;

        if (cp.shift == PD_SHIFT_SUPER || cp.shift == PD_SHIFT_SUB) {   /* already sized; only raise or lower */
            a.y += cp.shift == PD_SHIFT_SUPER ? -cp.size / 3 : cp.size / 6;
        }

        a.y -= cp.position;
        emit(D, &a);

        if (cp.underline || cp.strike) {
            pd_draw u = a;

            u.kind = PD_DRAW_RULE;
            u.h = ps.size / 20 > PD_SP_PER_PT / 3 ? ps.size / 20 : PD_SP_PER_PT / 3;

            if (cp.underline) {
                u.y = a.y + ps.size / 8;
                u.w = cp.underline == PD_UNDERLINE_WORDS ? g[i].advance : rule_reach(d, b, g, n, i, 0);
                emit_underline(D, &u, cp.underline);
            }

            if (cp.strike) {
                u.y = a.y - ps.size / 4;
                u.w = rule_reach(d, b, g, n, i, 1);
                emit(D, &u);
            }
        }
    }

    emit_leaders(L, D, l, g, n, &ln);
    free(g);
}

/* the numbers in the margin beside a page's numbered lines */
static void emit_line_numbers(const pd_layout* L, dlist_t* D, const ppage* p) {
    const pd_doc* d = L->doc;
    pd_style ns;
    int32_t i, cnt = p->lnum_first;

    if (p->lnum_first < 0 || pd_doc_default_style(d, &ns) != PD_OK) {
        return;
    }

    for (i = 0; i < p->n; i++) {
        const pline* l = &p->lines[i];
        const pd_section_props* sp = line_numbered(L, l);
        int32_t num;
        pd_line ln;
        dlist_t M;
        char text[16];
        pd_sp w;

        if (!sp) {
            continue;
        }

        num = (sp->line_number_start > 0 ? sp->line_number_start : 1) + cnt++;

        if (num % sp->line_numbers != 0 || pd_para_get_line(l->pc->para, l->line, &ln) != PD_OK) {
            continue;
        }

        snprintf(text, sizeof(text), "%d", (int)num);
        memset(&M, 0, sizeof(M));
        w = emit_text(L, &M, text, &ns, 0, 0, l->pc->block, 0, l->region);   /* its width, to set it flush right */
        free(M.d);
        emit_text(L, D, text, &ns, l->ox - (sp->line_number_distance > 0 ? sp->line_number_distance : PD_PT(18)) - w,
                  l->oy + ln.baseline, l->pc->block, 0, l->region);
    }
}

/* a page's rules from..to as drawn rectangles: those of a drawing's text boxes alone (only 5), all but those (0),
   or all (-1) */
static void emit_rules(dlist_t* D, const ppage* pg, int32_t from, int32_t to, int only) {
    int32_t i;

    for (i = from; i < to; i++) {
        const prule* r = &pg->rules[i];
        pd_draw a;

        if ((only == 5 && r->region != 5) || (only == 0 && r->region == 5)) {
            continue;
        }

        memset(&a, 0, sizeof(a));
        a.kind = PD_DRAW_RULE;
        a.x = r->x;
        a.y = r->y;
        a.w = r->w;
        a.h = r->h;
        a.color = r->color;
        a.region = r->region;
        a.block = r->block;
        emit(D, &a);
    }
}

pd_status pd_layout_page_items(const pd_layout* L, int32_t page, pd_draw* buf, int32_t cap, int32_t* count) {
    ppage* pg;
    dlist_t D;
    int32_t i, k;
    int boxed;

    if (!L || !count || cap < 0) {
        return PD_ERR_ARG;
    }

    if (page < 0 || page >= L->npages) {
        return PD_ERR_RANGE;
    }

    /* Made once and kept with the page, until it is laid out again.  A page is
       listed for every paint -- twice, for the count and then the items -- and
       making the list lays out every text box of a drawing on it again: a page
       with a figure's grid of pictures and a table took 23 ms a paint, which a
       scroll paid on every step that showed a sliver of it. */
    pg = (ppage*)&L->pages[page];

    if (!pg->items_ok) {
        memset(&D, 0, sizeof(D));

        emit_rules(&D, pg, 0, pg->nrules, 0);  /* rules first: backgrounds sit under the text */
        emit_para_boxes(&D, pg);
        k = pg->nrules;
        boxed = 0;

        for (i = 0; i < pg->n; i++) {
            if (pg->lines[i].region == 5 && !boxed) {
                emit_rules(&D, pg, 0, k, 5);    /* a drawing's text boxes' tables: over the drawing, under their text */
                boxed = 1;
            }

            emit_line(L, &D, pg, &pg->lines[i]);

            if (pg->nrules > k) {   /* a drawing's text boxes laid out with it: their tables' rules over it */
                emit_rules(&D, pg, k, pg->nrules, -1);
                k = pg->nrules;
            }
        }

        emit_line_numbers(L, &D, pg);

        if (D.err) {
            free(D.d);
            free(D.pts);
            return PD_ERR_NOMEM;
        }

        /* the paths' points: kept with the page as the list is */
        free(pg->pts);
        pg->pts = D.pts;

        for (i = 0; i < D.n; i++) {
            if (D.d[i].kind == PD_DRAW_PATH) {
                D.d[i].points = D.pts + (intptr_t)D.d[i].points;
            }
        }

        free(pg->items);
        pg->items = D.d;
        pg->nitems = D.n;
        pg->items_ok = 1;
    }

    *count = pg->nitems;

    if (buf) {
        if (cap < pg->nitems) {
            return PD_ERR_RANGE;
        }

        if (pg->nitems) {
            memcpy(buf, pg->items, (size_t)pg->nitems * sizeof(pd_draw));
        }
    }

    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* hit testing and carets                                             */
/* ------------------------------------------------------------------ */

pd_status pd_layout_hit_test(const pd_layout* L, int32_t page, pd_sp x, pd_sp y, pd_pos* out) {
    const ppage* p;
    const pline* best = NULL;
    int64_t best_d = 0;
    int32_t i;
    pd_line ln;
    uint32_t off;

    if (!L || !out) {
        return PD_ERR_ARG;
    }

    if (page < 0 || page >= L->npages) {
        return PD_ERR_RANGE;
    }

    p = &L->pages[page];

    /* the nearest line: vertical distance first, then horizontal */
    for (i = 0; i < p->n; i++) {
        const pline* l = &p->lines[i];
        int64_t dy = y < l->top ? l->top - y : (y >= l->bottom ? y - l->bottom + 1 : 0), dx, dist;
        pd_sp left, right;

        pd_para_get_line(l->pc->para, l->line, &ln);
        left = l->ox + ln.x;
        right = left + ln.width;
        dx = x < left ? left - x : (x > right ? x - right : 0);
        dist = dy * 4 + dx;

        /* a text box's line wins a tie with the line its drawing sits in, which a click inside the drawing also
           falls in: the caption is what the click was for */
        if (!best || dist < best_d || (dist == best_d && l->region == 5 && best->region != 5)) {
            best = l;
            best_d = dist;
        }
    }

    if (!best) {
        return PD_ERR_STATE;
    }

    pd_para_get_line(best->pc->para, best->line, &ln);

    if (pd_para_hit_test(best->pc->para, x - best->ox, ln.baseline, &off, NULL) != PD_OK) {
        return PD_ERR_STATE;
    }

    out->block = best->pc->block;
    out->offset = off;
    return PD_OK;
}

pd_status pd_layout_caret(const pd_layout* L, pd_pos pos, int32_t* page, pd_sp* x, pd_sp* baseline, pd_sp* ascent,
                          pd_sp* descent) {
    int32_t pg, i, line = -1;
    pd_sp cx = 0, cb = 0;
    const pcache* c = NULL;

    if (!L || !pd_doc_blk(L->doc, pos.block)) {
        return PD_ERR_ARG;
    }

    /* the layout of the paragraph that is on the pages (it may be a variant or a wrapped shape) */
    for (pg = 0; pg < L->npages; pg++) {
        for (i = 0; i < L->pages[pg].n; i++) {
            const pline* l = &L->pages[pg].lines[i];

            if (l->pc->block != pos.block) {
                continue;
            }

            if (l->pc != c) {
                c = l->pc;

                if (pd_para_caret(c->para, pos.offset, &line, &cx, &cb) != PD_OK) {
                    return PD_ERR_RANGE;
                }
            }

            if (l->line == line) {
                pd_line ln;

                pd_para_get_line(c->para, line, &ln);

                if (page) {
                    *page = pg;
                }

                if (x) {
                    *x = l->ox + cx;
                }

                if (baseline) {
                    *baseline = l->oy + cb;
                }

                if (ascent) {
                    *ascent = ln.ascent;
                }

                if (descent) {
                    *descent = ln.descent;
                }

                return PD_OK;
            }
        }
    }

    return c ? PD_ERR_STATE : PD_ERR_ARG;
}

/* ------------------------------------------------------------------ */
/* markup: tracked changes and comments on a page                     */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_markup_item* m;
    int32_t n, cap;
    int err;
} mlist_t;

/* add the item when its range starts on the page; returns the page it starts on */
static int32_t mark_add(const pd_layout* L, mlist_t* M, int32_t page, int32_t kind, uint32_t id, pd_range r,
                        const char* author) {
    pd_markup_item it;
    int32_t pg = -1, pe = -1;
    pd_sp x, base, asc, desc, ex, eb, ea, ed;

    if (pd_layout_caret(L, r.start, &pg, &x, &base, &asc, &desc) != PD_OK || pg != page) {
        return pg;
    }

    memset(&it, 0, sizeof(it));
    it.kind = kind;
    it.id = id;
    it.range = r;
    it.x = x;
    it.y = base;
    it.top = base - asc;
    it.bottom = base + desc;
    it.color = pd_doc_author_color(L->doc, author);

    if (pd_layout_caret(L, r.end, &pe, &ex, &eb, &ea, &ed) == PD_OK && pe == page && eb + ed > it.bottom) {
        it.bottom = eb + ed;
    }

    if (M->n >= M->cap) {
        int32_t nc = M->cap ? M->cap * 2 : 16;
        pd_markup_item* t = (pd_markup_item*)realloc(M->m, (size_t)nc * sizeof(*t));

        if (!t) {
            M->err = 1;
            return pg;
        }

        M->m = t;
        M->cap = nc;
    }

    M->m[M->n++] = it;
    return pg;
}

static int mark_cmp(const void* a, const void* b) {
    const pd_markup_item* p = (const pd_markup_item*)a, *q = (const pd_markup_item*)b;

    if (p->y != q->y) {
        return p->y < q->y ? -1 : 1;
    }

    if (p->x != q->x) {
        return p->x < q->x ? -1 : 1;
    }

    return p->kind - q->kind;
}

pd_status pd_layout_page_markup(const pd_layout* L, int32_t page, pd_markup_item* buf, int32_t cap, int32_t* count) {
    const pd_doc* d;
    pd_page_info pi;
    mlist_t M;
    int32_t i;

    if (!L || !count || cap < 0) {
        return PD_ERR_ARG;
    }

    if (page < 0 || page >= L->npages || pd_layout_page_info(L, page, &pi) != PD_OK) {
        return PD_ERR_RANGE;
    }

    d = L->doc;
    memset(&M, 0, sizeof(M));

    if (pi.first.block && pd_doc_markup(d) <= PD_MARKUP_INLINE && pd_doc_revision_count(d) > 0) {
        pd_block_id cur;

        int past = 0;

        for (cur = pi.first.block; cur && !M.err && !past; cur = cur == pi.last.block ? 0 : pd_doc_next_paragraph(d, cur)) {
            const blk* b = pd_doc_blk(d, cur);
            int32_t k = 0;

            while (b && k < b->st.nruns) {
                pd_style_id cs;
                pd_char_props ov;
                pd_revision rv;
                pd_range r;
                int32_t j = k;

                if (pd_doc_format_info(d, b->st.runs[k].format, &cs, &ov) != PD_OK || !(ov.mask & PD_CP_REVISION) ||
                        pd_doc_revision_get(d, ov.revision, &rv) != PD_OK) {
                    k++;
                    continue;
                }

                for (; j + 1 < b->st.nruns && b->st.runs[j + 1].start == b->st.runs[j].end; j++) {
                    pd_char_props o2;

                    if (pd_doc_format_info(d, b->st.runs[j + 1].format, &cs, &o2) != PD_OK ||
                            !(o2.mask & PD_CP_REVISION) || o2.revision != ov.revision) {
                        break;
                    }
                }

                r.start.block = r.end.block = cur;
                r.start.offset = b->st.runs[k].start;
                r.end.offset = b->st.runs[j].end;
                past = mark_add(L, &M, page, rv.kind == PD_REV_DELETE ? PD_MARK_DELETION : PD_MARK_INSERTION, ov.revision,
                                r, rv.author) > page;
                k = j + 1;
            }
        }
    }

    for (i = 1; i <= pd_doc_comment_count(d) && !M.err; i++) {
        pd_comment c;

        if (pd_doc_comment_get(d, (pd_comment_id)i, &c) == PD_OK && !c.parent) {
            mark_add(L, &M, page, PD_MARK_COMMENT, (uint32_t)i, c.range, c.author);
        }
    }

    if (M.err) {
        free(M.m);
        return PD_ERR_NOMEM;
    }

    if (M.n > 1) {
        qsort(M.m, (size_t)M.n, sizeof(*M.m), mark_cmp);
    }

    *count = M.n;

    if (buf) {
        if (cap < M.n) {
            free(M.m);
            return PD_ERR_RANGE;
        }

        if (M.n) {
            memcpy(buf, M.m, (size_t)M.n * sizeof(*M.m));
        }
    }

    free(M.m);
    return PD_OK;
}
