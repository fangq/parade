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
 * 5. Headers and footers are placed per page; fields are filled in when
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

/* ------------------------------------------------------------------ */
/* data                                                               */
/* ------------------------------------------------------------------ */

typedef struct {                /* one paragraph laid out at one width */
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
} pcache;

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
    VI_BREAK = 4
};

typedef struct {
    int32_t kind;
    pd_sp h;
    int32_t pen;
    pcache* pc;
    int32_t line;
    pd_block_id block;          /* floats */
    int32_t brk;
} vitem;

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
    if (c) {
        pd_para_free(c->para);
        free(c->top);
        free(c);
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

/* the cached layout of a paragraph at a width, (re)built when stale */
static pcache* layout_para(pd_layout* L, pd_block_id id, pd_sp width, pd_status* st) {
    const pd_doc* d = L->doc;
    blk* b = pd_doc_blk(d, id);
    pcache* c;
    uint64_t sig;

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

    c = L->cache[id];
    sig = field_signature(L, b);

    if (c && c->revision == b->revision && c->style_rev == d->style_rev && c->width == width &&
            c->epoch == L->epoch && c->fieldsig == sig) {
        c->used = 1;
        L->info.paragraphs_reused++;
        return c;
    }

    if (!c) {
        c = (pcache*)calloc(1, sizeof(pcache));

        if (!c || pd_para_new(&c->para) != PD_OK) {
            free(c);
            *st = PD_ERR_NOMEM;
            return NULL;
        }

        L->cache[id] = c;
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

    if (region == 0 || region == 3) {
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
            v.value = ++*footnotes;
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

        if (id < L->ncache && L->cache[id]) {
            strcpy(L->cache[id]->label, buf);
        }
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

static pd_status build_flow(filler* F, pd_block_id container, pd_sp* prev_after, int* prev_keep, int* first) {
    const pd_doc* d = F->L->doc;
    blk* c = pd_doc_blk(d, container);
    int32_t i, k;
    pd_status st = PD_OK;

    for (i = 0; c && i < c->nkids && st == PD_OK; i++) {
        blk* b = d->tab[c->kids[i]];

        if (b->kind == PD_BLOCK_PARAGRAPH) {
            pcache* pc = layout_para(F->L, b->id, F->colw, &st);
            const pd_para_props* pp;

            if (!pc) {
                return st;
            }

            pp = &pc->pp;

            if (pp->page_break_before && !*first) {
                push(F, VI_BREAK, 0, 0, NULL, 0, 0);
                F->it[F->n - 1].brk = PD_BREAK_PAGE;
            } else if (!*first) {
                push(F, VI_PEN, 0, *prev_keep ? INF_PEN : 0, NULL, 0, 0);
            }

            push(F, VI_GLUE, *first ? 0 : (pp->space_before > *prev_after ? pp->space_before : *prev_after), 0, NULL,
                 0, 0);

            for (k = 0; k < pc->nlines; k++) {
                if (k > 0) {    /* widows, orphans, keep-lines */
                    int ok = !pp->keep_lines && k >= pp->orphans && pc->nlines - k >= pp->widows;

                    push(F, VI_PEN, 0, ok ? INTERLINE_PEN : INF_PEN, NULL, 0, 0);
                }

                push(F, VI_LINE, pc->top[k + 1] - pc->top[k], 0, pc, k, b->id);
            }

            *prev_after = pp->space_after;
            *prev_keep = pp->keep_with_next;
            *first = 0;
        } else if (b->kind == PD_BLOCK_FLOAT) {
            pfloat* f;

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

            f->item = F->n;
            push(F, VI_FLOAT, f->h + 2 * f->fp.gap, 0, NULL, 0, b->id);
            F->it[F->n - 1].line = F->nfl - 1;  /* float index */
        } else if (b->kind == PD_BLOCK_BREAK) {
            push(F, VI_BREAK, 0, 0, NULL, 0, 0);
            F->it[F->n - 1].brk = b->st.break_kind;
        } else if (b->kind == PD_BLOCK_TABLE || b->kind == PD_BLOCK_ROW || b->kind == PD_BLOCK_CELL) {
            st = build_flow(F, b->id, prev_after, prev_keep, first);  /* stacked until tables are laid out */
        }
    }

    return st;
}

static pd_sp col_x(const filler* F, int32_t page, int32_t col) {
    return F->L->pages[page].text_x + col * (F->colw + F->gap);
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

    F->top_used = 0;

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
} rec;

/* emit a column's content (records before cut) and seat bottom floats in what is left */
static void commit(filler* F, const rec* r, int32_t nr, int32_t cut, pd_sp used_at_cut) {
    pd_sp x = col_x(F, F->page, F->col), y0 = F->sp->margin_top + F->top_used;
    pd_sp avail = F->colh - F->top_used, bottom = avail;
    int32_t i;

    for (i = 0; i < nr && r[i].item < cut; i++) {
        const vitem* v = &F->it[r[i].item];

        if (v->kind == VI_LINE) {
            add_line(F->L, F->page, v->pc, v->line, x, y0 + r[i].y - v->pc->top[v->line], 0);
        } else if (v->kind == VI_FLOAT) {
            pfloat* f = &F->fl[v->line];

            place_stack(F->L, f->block, f->w, F->page, x + (F->colw - f->w) / 2, y0 + r[i].y + f->fp.gap, 3);
            f->state = FL_PLACED;
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

static pd_status fill(filler* F) {
    rec* r = NULL;
    int32_t nr = 0, capr = 0, s = 0, i, best = -1;
    int64_t best_cost = 0;
    pd_sp used = 0, avail;
    int empty = 1;

    if (next_column(F, 1)) {
        return PD_ERR_NOMEM;
    }

    avail = F->colh - F->top_used;

    for (i = s; i <= F->n;) {
        vitem* v = i < F->n ? &F->it[i] : NULL;
        int cut = 0, forced = 0;
        int32_t at = 0;

        if (!v) {   /* end of the flow */
            commit(F, r, nr, F->n, used);
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
                    int64_t cost = v->pen + badness(avail - used, avail / 5);

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

                if ((f->fp.placement & (PD_PLACE_HERE | PD_PLACE_FORCE)) && (used + v->h <= avail || empty)) {
                    if (grow((void**)&r, &capr, (int64_t)nr + 1, sizeof(rec))) {
                        free(r);
                        return PD_ERR_NOMEM;
                    }

                    r[nr].item = i;
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
                if (used + v->h > avail && !empty) {
                    cut = 1;
                    at = best >= 0 ? best : i;     /* no legal break: emergency, right here */
                    break;
                }

                if (used + v->h > avail) {
                    F->L->info.overfull++;
                }

                if (grow((void**)&r, &capr, (int64_t)nr + 1, sizeof(rec))) {
                    free(r);
                    return PD_ERR_NOMEM;
                }

                r[nr].item = i;
                r[nr++].y = used;
                used += v->h;
                empty = 0;
                i++;
                continue;
        }

        if (cut) {
            pd_sp used_at = 0;
            int32_t k, brk = forced ? F->it[at].brk : -1;

            for (k = 0; k < nr && r[k].item < at; k++) {
                used_at = r[k].y + F->it[r[k].item].h;
            }

            commit(F, r, nr, at, used_at);
            nr = 0;
            best = -1;
            used = 0;
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

    for (k = 0; k < L->ncache; k++) {   /* labels are recomputed from scratch */
        if (L->cache[k]) {
            L->cache[k]->label[0] = '\0';
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
        if (L->cache[k]) {
            L->cache[k]->used = 0;
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
        pd_sp prev_after = 0;
        int prev_keep = 0, first = 1;
        blk* sec = d->tab[root->kids[s]];

        memset(&F, 0, sizeof(F));
        F.L = L;
        F.sec = sec;
        F.sp = &sec->st.sp;
        F.ncols = F.sp->columns < 1 ? 1 : F.sp->columns;
        F.gap = F.sp->column_gap;
        F.colh = F.sp->page_height - F.sp->margin_top - F.sp->margin_bottom;
        F.colw = (F.sp->page_width - F.sp->margin_left - F.sp->margin_right - (F.ncols - 1) * F.gap) / F.ncols;
        F.number = F.sp->first_page_number > 0 ? F.sp->first_page_number : number;

        if (F.colw <= 0 || F.colh <= 0) {
            st = PD_ERR_RANGE;
            break;
        }

        st = build_flow(&F, sec->id, &prev_after, &prev_keep, &first);

        if (st == PD_OK) {
            st = fill(&F);
        }

        number = F.number;
        free(F.it);
        free(F.fl);
        free(F.queue);
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

    /* paragraphs no longer in the document leave the cache */
    for (k = 0; k < L->ncache; k++) {
        if (L->cache[k] && !L->cache[k]->used) {
            pcache_free(L->cache[k]);
            L->cache[k] = NULL;
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

        if (pd_doc_run_style(d, b->id, b->st.nruns ? b->st.runs[0].format : b->st.empty_format, &ls, &lcp) == PD_OK) {
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
                emit(D, &u);
            }

            if (cp.strike) {
                u.y = a.y - ps.size / 4;
                emit(D, &u);
            }
        }
    }

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
    int32_t pg, i, line;
    pd_sp cx, cb;
    pcache* c;

    if (!L || pos.block >= L->ncache || !L->cache[pos.block]) {
        return PD_ERR_ARG;
    }

    c = L->cache[pos.block];

    if (pd_para_caret(c->para, pos.offset, &line, &cx, &cb) != PD_OK) {
        return PD_ERR_RANGE;
    }

    for (pg = 0; pg < L->npages; pg++) {
        for (i = 0; i < L->pages[pg].n; i++) {
            const pline* l = &L->pages[pg].lines[i];

            if (l->pc == c && l->line == line) {
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

    return PD_ERR_STATE;
}
