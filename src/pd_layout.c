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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_doc_internal.h"
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
    pd_sp ws_w;                 /* wrap shape: the first ws_k lines are ws_w narrower */
    int32_t ws_side, ws_k;
    struct pcache* next;        /* other layouts (variants, shapes) of the same paragraph */
} pcache;

typedef struct {                /* lines narrowed beside a wrapping float */
    pd_sp w;
    int32_t side;               /* PD_WRAP_LEFT: the float is on the left */
    int32_t k;
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
    VI_ROW = 5
};

typedef struct {
    int32_t kind;
    pd_sp h;
    int32_t pen;
    pcache* pc;
    int32_t line;               /* lines: line index; floats: float index; rows: row index */
    pd_block_id block;          /* floats, rows */
    int32_t brk;
    int32_t tbl;                /* rows: table index */
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
    int32_t wrap_side;
    int32_t floor_page;         /* continuous section: page shared with the previous section */
    pd_sp floor_y;              /* and the height its content takes there */
    uint8_t* chosen;            /* optimal page breaking: penalty items to break at */
    int8_t* loose;              /* optimal page breaking: \looseness by block id */
    int32_t page_start_item;    /* first item of the current page (balancing) */
    int32_t page_start_n, page_start_nrules;
    int resume;                 /* fill: continue on the current page */
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

    c->block = id;
    c->width = width;
    c->used = 1;
    pd_doc_effective_pp(d, b, &c->pp, &c->label_x);
    st = pd_doc_para_build_ex(d, id, width, c->para, &prm, fn, user);

    if (st == PD_OK && c->ws_k > 0) {   /* the first lines run beside a float */
        pd_sp ind[65], wid[65], w = width - c->pp.indent_left - c->pp.indent_right;
        int32_t k = c->ws_k < 64 ? c->ws_k : 64;

        for (i = 0; i <= k; i++) {
            pd_sp bi0 = i == 0 ? c->pp.indent_left + c->pp.indent_first : c->pp.indent_left;
            pd_sp bw = i == 0 ? w - c->pp.indent_first : w;

            ind[i] = bi0 + (i < k && c->ws_side == PD_WRAP_LEFT ? c->ws_w : 0);
            wid[i] = i < k ? bw - c->ws_w : bw;
            wid[i] = wid[i] < PD_PT(12) ? PD_PT(12) : wid[i];
        }

        st = pd_para_set_shape(c->para, k + 1, ind, wid);
    }

    prm.looseness = c->loose;

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
    wrapshape none = { 0, 0, 0 };

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
                                  c->ws_side == ws->side); c = c->next) {
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
    even = sp->facing_pages && number % 2 == 0;
    p->text_x = even ? sp->margin_right : sp->margin_left;
    p->text_w = sp->page_width - sp->margin_left - sp->margin_right;

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

    return 0;
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

/* lay out the paragraphs of a block stack at a width; returns total height */
static pd_sp stack_height(pd_layout* L, pd_block_id container, pd_sp width, pd_status* st) {
    pd_block_id ids[512];
    int32_t n = 0, i;
    pd_sp h = 0, prev_after = 0;

    collect_paras(L->doc, container, ids, &n, 512);

    for (i = 0; i < n && i < 512; i++) {
        pcache* c = layout_para(L, ids[i], width, st);

        if (!c) {
            return -1;
        }

        if (i > 0) {
            h += c->pp.space_before > prev_after ? c->pp.space_before : prev_after;
        }

        h += c->height;
        prev_after = c->pp.space_after;
    }

    return h;
}

/* place a block stack's paragraphs from (x, y) */
static void place_stack(pd_layout* L, pd_block_id container, pd_sp width, int32_t page, pd_sp x, pd_sp y,
                        int32_t region) {
    pd_block_id ids[512];
    int32_t n = 0, i, k;
    pd_sp prev_after = 0;
    pd_status st;

    collect_paras(L->doc, container, ids, &n, 512);

    for (i = 0; i < n && i < 512; i++) {
        pcache* c = layout_para(L, ids[i], width, &st);

        if (!c) {
            return;
        }

        if (i > 0) {
            y += c->pp.space_before > prev_after ? c->pp.space_before : prev_after;
        }

        for (k = 0; k < c->nlines; k++) {
            add_line(L, page, c, k, x, y, region);
        }

        y += c->height;
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
    if (b->st.list && (int32_t)b->st.list <= d->nlists && b->st.list <= 64) {
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

/* text beside a wrapping float ends: what is left of the float's height becomes space */
static void clear_wrap(filler* F) {
    if (F->wrap_rem > 0) {
        push(F, VI_GLUE, F->wrap_rem, 0, NULL, 0, 0);
    }

    F->wrap_rem = 0;
}

/* narrowest and widest useful width of a cell's content */
static void cell_minmax(filler* F, pd_block_id cell, pd_sp* mn, pd_sp* mx) {
    pd_layout* L = F->L;
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
static pd_status build_table(filler* F, const blk* t, pd_sp* prev_after, int* prev_keep, int* first) {
    const pd_doc* d = F->L->doc;
    const pd_table_props* tp = &t->st.tp;
    ptable T;
    pd_sp mn[PD_TABLE_MAX_COLS], mx[PD_TABLE_MAX_COLS], w[PD_TABLE_MAX_COLS], avail, fixed = 0;
    int64_t smin = 0, smax = 0;
    int32_t r, k, c, pass, nauto = 0, ti;
    pd_sp pad = tp->cell_padding;
    pd_status st = PD_OK;

    memset(&T, 0, sizeof(T));
    T.block = t->id;
    T.tp = *tp;
    T.ncols = tp->ncols;

    for (r = 0; r < t->nkids; r++) {    /* grid width: the widest row */
        const blk* row = d->tab[t->kids[r]];
        int32_t cols = 0;

        for (k = 0; k < row->nkids; k++) {
            int32_t sp = d->tab[row->kids[k]]->st.cell.col_span;
            cols += sp < 1 ? 1 : sp;
        }

        T.ncols = cols > T.ncols ? cols : T.ncols;
    }

    T.ncols = T.ncols > PD_TABLE_MAX_COLS ? PD_TABLE_MAX_COLS : T.ncols;

    if (T.ncols == 0) {
        return PD_OK;
    }

    memset(mn, 0, sizeof(mn));
    memset(mx, 0, sizeof(mx));

    /* single-column cells first, then spanning cells widen the columns they cover */
    for (pass = 0; pass < 2; pass++) {
        for (r = 0; r < t->nkids; r++) {
            const blk* row = d->tab[t->kids[r]];

            for (k = 0, c = 0; k < row->nkids && c < T.ncols; k++) {
                const blk* cell = d->tab[row->kids[k]];
                int32_t span = cell_span(&T, cell, c), j;
                pd_sp a, b;

                if ((span == 1) != (pass == 0) || cell->st.cell.merge_up) {
                    c += span;
                    continue;
                }

                cell_minmax(F, cell->id, &a, &b);
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

    avail = tp->width > 0 && tp->width < F->colw ? tp->width : F->colw;

    for (c = 0; c < T.ncols; c++) {
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

    for (c = 0; c < T.ncols; c++) {
        int64_t rest = (int64_t)avail - fixed;

        if (w[c] >= 0) {
            continue;
        }

        if (smax <= rest) {     /* everything fits unbroken; a table of given width stretches */
            w[c] = mx[c];

            if (tp->width > 0) {
                w[c] += (pd_sp)(smax > 0 ? (rest - smax) * mx[c] / smax : (rest - smax) / nauto);
            }
        } else if (smin >= rest) {
            w[c] = mn[c];
        } else {
            w[c] = (pd_sp)(mn[c] + (int64_t)(mx[c] - mn[c]) * (rest - smin) / (smax - smin));
        }
    }

    for (c = 0; c < T.ncols; c++) {
        T.colx[c + 1] = T.colx[c] + (w[c] > 0 ? w[c] : 0);
    }

    T.width = T.colx[T.ncols];
    T.x = tp->align == PD_ALIGN_CENTER ? (F->colw - T.width) / 2 : tp->align == PD_ALIGN_RIGHT ? F->colw - T.width : 0;
    T.x = T.x < 0 ? 0 : T.x;
    T.header_rows = tp->header_rows < t->nkids ? tp->header_rows : t->nkids - 1;
    T.header_rows = T.header_rows < 0 ? 0 : T.header_rows;

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

                h += 2 * pad;

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

        if (push(F, VI_ROW, rowh, 0, NULL, r, row->id)) {
            return PD_ERR_NOMEM;
        }

        F->it[F->n - 1].tbl = ti;

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
    *prev_keep = 0;
    *first = 0;
    return PD_OK;
}

static pd_status build_flow(filler* F, pd_block_id container, pd_sp* prev_after, int* prev_keep, int* first) {
    const pd_doc* d = F->L->doc;
    blk* c = pd_doc_blk(d, container);
    int32_t i, k;
    pd_status st = PD_OK;

    for (i = 0; c && i < c->nkids && st == PD_OK; i++) {
        blk* b = d->tab[c->kids[i]];

        if (b->kind == PD_BLOCK_PARAGRAPH) {
            pd_sp gl, rem;
            int32_t loose = F->loose ? F->loose[b->id] : 0, kwrap = 0;
            pcache* pc = layout_para_ex(F->L, b->id, F->colw, F->wrap_rem > 0 ? 0 : loose, NULL, &st);
            const pd_para_props* pp;

            if (!pc) {
                return st;
            }

            pp = &pc->pp;
            gl = *first ? 0 : (pp->space_before > *prev_after ? pp->space_before : *prev_after);
            rem = F->wrap_rem - gl;

            if (F->wrap_rem > 0 && rem > 0) {   /* beside a float: narrower lines while it lasts */
                wrapshape ws;
                int32_t pass;

                ws.w = F->wrap_w;
                ws.side = F->wrap_side;

                for (pass = 0; pass < 3; pass++) {
                    for (ws.k = 0; ws.k < pc->nlines && pc->top[ws.k] < rem; ws.k++) {
                    }

                    if (ws.k == pc->ws_k || ws.k == 0) {
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
            }

            if (pp->page_break_before && !*first) {
                clear_wrap(F);
                push(F, VI_BREAK, 0, 0, NULL, 0, 0);
                F->it[F->n - 1].brk = PD_BREAK_PAGE;
            } else if (!*first) {
                push(F, VI_PEN, 0, *prev_keep || F->wrap_rem > 0 ? INF_PEN : 0, NULL, 0, 0);
            }

            push(F, VI_GLUE, gl, 0, NULL, 0, 0);

            for (k = 0; k < pc->nlines; k++) {
                pd_line ln;

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

            if (F->wrap_rem > 0) {
                F->wrap_rem = rem - pc->height;
                F->wrap_rem = F->wrap_rem < 0 ? 0 : F->wrap_rem;
            }

            *prev_after = pp->space_after;
            *prev_keep = pp->keep_with_next;
            *first = 0;
        } else if (b->kind == PD_BLOCK_FLOAT) {
            pfloat* f;

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

            /* text wraps only beside a float that leaves it room */
            if (f->fp.wrap != PD_WRAP_NONE && (int64_t)(f->w + f->fp.gap) * 4 > (int64_t)F->colw * 3) {
                f->fp.wrap = PD_WRAP_NONE;
            }

            f->item = F->n;

            if (f->fp.wrap != PD_WRAP_NONE) {
                push(F, VI_FLOAT, 0, 0, NULL, 0, b->id);
                F->wrap_rem = f->h + f->fp.gap;
                F->wrap_w = f->w + f->fp.gap;
                F->wrap_side = f->fp.wrap;
            } else {
                push(F, VI_FLOAT, f->h + 2 * f->fp.gap, 0, NULL, 0, b->id);
            }

            F->it[F->n - 1].line = F->nfl - 1;  /* float index */
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
    st = build_flow(F, F->sec->id, &prev_after, &prev_keep, &first);

    /* after the last section's text, the endnotes */
    {
        const blk* root = pd_doc_blk(F->L->doc, PD_ROOT_ID);

        if (st == PD_OK && F->L->nendnotes > 0 && root && root->nkids > 0 &&
                root->kids[root->nkids - 1] == F->sec->id) {
            clear_wrap(F);
            prev_after = prev_after > PD_PT(18) ? prev_after : PD_PT(18);

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
                    F->sp->margin_top + y + f->fp.gap, 3);
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
                        F->sp->margin_top + F->top_used + f->fp.gap, 3);
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

/* one table row: cell backgrounds, contents and grid rules */
static void place_row(filler* F, const vitem* v, pd_sp x, pd_sp y) {
    const pd_doc* d = F->L->doc;
    const ptable* T = &F->tb[v->tbl];
    const blk* row = pd_doc_blk(d, v->block);
    const blk* t = pd_doc_blk(d, T->block);
    const blk* next = t && v->line + 1 < t->nkids ? d->tab[t->kids[v->line + 1]] : NULL;
    pd_sp pad = T->tp.cell_padding, bw = T->tp.border, tx = x + T->x;
    uint32_t bc = T->tp.border_color;
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
        pd_sp ch = 0;

        for (j = v->line; j < v->line + down && T->rowh && j < T->nrows; j++) {
            ch += T->rowh[j];   /* the merged cell's height: every row it covers */
        }

        ch = down > 1 && ch > 0 ? ch : v->h;
        inner = inner < PD_PT(1) ? PD_PT(1) : inner;

        if (!cell->st.cell.merge_up) {
            if (cell->st.cell.background) {
                add_rule(F->L, F->page, cx, y, cw, ch, cell->st.cell.background, 0, cell->id);
            }

            h = stack_height(F->L, cell->id, inner, &st);

            if (h >= 0 && cell->st.cell.valign == 1) {
                off = (ch - 2 * pad - h) / 2;
            } else if (h >= 0 && cell->st.cell.valign == 2) {
                off = ch - 2 * pad - h;
            }

            place_stack(F->L, cell->id, inner, F->page, cx + pad, y + pad + (off > 0 ? off : 0), 0);
        }

        if (bw > 0) {
            add_rule(F->L, F->page, cx - bw / 2, y, bw, v->h, bc, 0, cell->id);

            if (!cell->st.cell.merge_up) {      /* no rule inside a merged cell */
                add_rule(F->L, F->page, cx - bw / 2, y - bw / 2, cw + bw, bw, bc, 0, cell->id);
            }

            if (!below || !below->st.cell.merge_up) {
                add_rule(F->L, F->page, cx - bw / 2, y + v->h - bw / 2, cw + bw, bw, bc, 0, cell->id);
            }
        }

        c += span;
    }

    if (bw > 0) {
        add_rule(F->L, F->page, tx + T->colx[c] - bw / 2, y, bw, v->h, bc, 0, v->block);
    }
}

/* emit a column's content (records before cut), its footnotes, and bottom floats in what is left */
static void commit(filler* F, const rec* r, int32_t nr, int32_t cut, pd_sp used_at_cut, pd_sp fn_at_cut) {
    pd_sp x = col_x(F, F->page, F->col), y0 = F->sp->margin_top + F->top_used;
    pd_sp avail = F->colh - F->top_used, bottom = avail - fn_area(F, fn_at_cut);
    int32_t i, k;

    for (i = 0; i < nr && r[i].item < cut; i++) {
        const vitem* v = &F->it[r[i].item];

        if (v->kind == VI_LINE) {
            add_line(F->L, F->page, v->pc, v->line, x, y0 + r[i].y - v->pc->top[v->line], 0);
        } else if (v->kind == VI_ROW) {
            place_row(F, v, x, y0 + r[i].y);
        } else if (v->kind == VI_FLOAT) {
            pfloat* f = &F->fl[v->line];

            if (f->fp.wrap != PD_WRAP_NONE) {   /* at the anchor, against the column edge */
                place_stack(F->L, f->block, f->w, F->page, f->fp.wrap == PD_WRAP_LEFT ? x : x + F->colw - f->w,
                            y0 + r[i].y, 3);
            } else {
                place_stack(F->L, f->block, f->w, F->page, x + (F->colw - f->w) / 2, y0 + r[i].y + f->fp.gap, 3);
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
                    if (used + f->h + f->fp.gap + fn_area(F, fn) > avail && !empty) {
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
                                F->sp->margin_top + F->top_used + f->fp.gap, 3);
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

                    if (v && (v->kind == VI_LINE || v->kind == VI_ROW)) {
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

/* a header or footer, laid out for this page so its fields have their values */
static void place_story(pd_layout* L, int32_t page, pd_block_id story, int footer) {
    ppage* p = &L->pages[page];
    pd_block_id ids[64];
    int32_t n = 0, i, k, first = p->nowned;
    pd_sp h = 0, y, prev_after = 0;
    page_ctx ctx;

    if (!story || !pd_doc_blk(L->doc, story)) {
        return;
    }

    ctx.L = L;
    ctx.page = page;
    collect_paras(L->doc, story, ids, &n, 64);

    for (i = 0; i < n && i < 64; i++) {
        pcache* c = (pcache*)calloc(1, sizeof(pcache));

        if (!c || pd_para_new(&c->para) != PD_OK ||
                grow((void**)&p->owned, &p->capowned, (int64_t)p->nowned + 1, sizeof(pcache*))) {
            if (c) {
                pd_para_free(c->para);
            }

            free(c);
            return;
        }

        p->owned[p->nowned++] = c;

        if (build_into(L, c, ids[i], p->text_w, page_field, &ctx) != PD_OK) {
            return;
        }

        h += (i > 0 ? (c->pp.space_before > prev_after ? c->pp.space_before : prev_after) : 0) + c->height;
        prev_after = c->pp.space_after;
    }

    y = footer ? p->h - p->sp->footer_distance - h : p->sp->header_distance;
    prev_after = 0;

    for (i = first; i < p->nowned; i++) {
        pcache* c = p->owned[i];

        if (i > first) {
            y += c->pp.space_before > prev_after ? c->pp.space_before : prev_after;
        }

        for (k = 0; k < c->nlines; k++) {
            add_line(L, page, c, k, p->text_x, y, footer ? 2 : 1);
        }

        y += c->height;
        prev_after = c->pp.space_after;
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

    L->doc = doc;
    *out = L;
    return PD_OK;
}

static void clear_pages(pd_layout* L) {
    int32_t i, k;

    for (i = 0; i < L->npages; i++) {
        free(L->pages[i].lines);
        free(L->pages[i].rules);

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
        F.colh = F.sp->page_height - F.sp->margin_top - F.sp->margin_bottom;
        F.colw = (F.sp->page_width - F.sp->margin_left - F.sp->margin_right - (F.ncols - 1) * F.gap) / F.ncols;
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

            bottom += PD_PT(12) - F.sp->margin_top;

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
} dlist_t;

static void emit(dlist_t* D, const pd_draw* x) {
    if (grow((void**)&D->d, &D->cap, (int64_t)D->n + 1, sizeof(pd_draw))) {
        D->err = 1;
        return;
    }

    D->d[D->n++] = *x;
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

            if (pd_doc_format_resolve(d, b->id, f, &cp) != PD_OK || !(which ? cp.strike : cp.underline)) {
                return g[i].advance;
            }
        }
    }

    return space ? g[i + 1].x - g[i].x : g[i].advance;
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
        } else if (l->pc->note) {     /* a footnote's number: raised, small, just before its text */
            dlist_t M;
            pd_sp w;

            ls.size = ls.size * 7 / 10;
            memset(&M, 0, sizeof(M));
            w = emit_text(L, &M, l->pc->label, &ls, 0, 0, b->id, 0, l->region);
            free(M.d);
            emit_text(L, D, l->pc->label, &ls, l->ox + ln.x - w - PD_PT(1), l->oy + ln.baseline - ls.size / 2, b->id, 0,
                      l->region);
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
            cached_style = g[i].style;
        }

        if (g[i].kind == PD_OBJECT) {
            const dinline* q = g[i].user >= 0 && g[i].user < b->st.ninl ? &b->st.inl[g[i].user] : NULL;

            if (!q) {
                continue;
            }

            a.h = q->obj.height + q->obj.depth;
            a.y = l->oy + g[i].y - q->obj.height;

            if (q->obj.kind == PD_INLINE_IMAGE) {
                a.kind = PD_DRAW_IMAGE;
                a.resource = q->obj.resource;
                emit(D, &a);
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

        emit(D, &a);

        if (cp.underline || cp.strike) {
            pd_draw u = a;

            u.kind = PD_DRAW_RULE;
            u.h = ps.size / 20 > PD_SP_PER_PT / 3 ? ps.size / 20 : PD_SP_PER_PT / 3;

            if (cp.underline) {
                u.y = a.y + ps.size / 8;
                u.w = rule_reach(d, b, g, n, i, 0);
                emit(D, &u);
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

pd_status pd_layout_page_items(const pd_layout* L, int32_t page, pd_draw* buf, int32_t cap, int32_t* count) {
    dlist_t D;
    int32_t i;

    if (!L || !count || cap < 0) {
        return PD_ERR_ARG;
    }

    if (page < 0 || page >= L->npages) {
        return PD_ERR_RANGE;
    }

    memset(&D, 0, sizeof(D));

    for (i = 0; i < L->pages[page].nrules; i++) {  /* rules first: backgrounds sit under the text */
        const prule* r = &L->pages[page].rules[i];
        pd_draw a;

        memset(&a, 0, sizeof(a));
        a.kind = PD_DRAW_RULE;
        a.x = r->x;
        a.y = r->y;
        a.w = r->w;
        a.h = r->h;
        a.color = r->color;
        a.region = r->region;
        a.block = r->block;
        emit(&D, &a);
    }

    for (i = 0; i < L->pages[page].n; i++) {
        emit_line(L, &D, &L->pages[page], &L->pages[page].lines[i]);
    }

    if (D.err) {
        free(D.d);
        return PD_ERR_NOMEM;
    }

    *count = D.n;

    if (buf) {
        if (cap < D.n) {
            free(D.d);
            return PD_ERR_RANGE;
        }

        if (D.n) {
            memcpy(buf, D.d, (size_t)D.n * sizeof(pd_draw));
        }
    }

    free(D.d);
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

        if (!best || dist < best_d) {
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
