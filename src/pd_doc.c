/*
 * Parade document model: tree, styles, formats, lists, operations, undo
 *
 * Undo is built from three symmetric records. Toggling a record twice
 * restores the original, so undo walks a step's records backwards and
 * redo walks them forwards with the same code:
 *   UR_STATE  swaps a block's content state with a saved copy
 *   UR_ATTACH attaches or detaches a whole subtree (insert, remove, move)
 *   UR_STYLE  swaps a style definition
 * Before an operation first changes a block in a step, the block's state
 * is saved once; later changes in the same step (typing) need nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_doc_internal.h"

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static int grow(void** p, int32_t* cap, int64_t need, size_t elem) {
    return pd_grow(p, cap, need, elem);
}

/* valid UTF-8 without U+FFFC (unless allowed) or control characters other than \t \n */
int pd_doc_utf8_valid(const char* str, size_t len, int allow_obj) {
    const unsigned char* s = (const unsigned char*)str;
    size_t i = 0;

    while (i < len) {
        unsigned c = s[i];
        uint32_t cp;
        int n, k;

        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\n') {
                return 0;
            }

            i++;
            continue;
        }

        if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return 0;
        }

        if (i + (size_t)n >= len) {
            return 0;   /* truncated sequence */
        }

        for (k = 1; k <= n; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return 0;
            }

            cp = (cp << 6) | (s[i + k] & 0x3F);
        }

        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF) || (cp == 0xFFFC && !allow_obj)) {
            return 0;
        }

        i += (size_t)n + 1;
    }

    return 1;
}

static int is_boundary(const bstate* s, uint32_t off) {
    return off <= s->len && (off == s->len || (s->text[off] & 0xC0) != 0x80);
}

void pd_doc_cp_normalize(pd_char_props* cp) {
    pd_char_props z;
    uint32_t m = cp->mask & PD_CP_ALL;

    memset(&z, 0, sizeof(z));
    z.mask = m;

    if (m & PD_CP_FAMILY) {
        memcpy(z.family, cp->family, sizeof(z.family));
        z.family[sizeof(z.family) - 1] = '\0';
        memset(z.family + strlen(z.family), 0, sizeof(z.family) - strlen(z.family));
    }

    if (m & PD_CP_SIZE) {
        z.size = cp->size;
    }

    if (m & PD_CP_WEIGHT) {
        z.weight = cp->weight;
    }

    if (m & PD_CP_ITALIC) {
        z.italic = cp->italic;
    }

    if (m & PD_CP_COLOR) {
        z.color = cp->color;
    }

    if (m & PD_CP_BACKGROUND) {
        z.background = cp->background;
    }

    if (m & PD_CP_UNDERLINE) {
        z.underline = cp->underline;
    }

    if (m & PD_CP_STRIKE) {
        z.strike = cp->strike;
    }

    if (m & PD_CP_SHIFT) {
        z.shift = cp->shift;
    }

    if (m & PD_CP_LETTERSPACE) {
        z.letter_space = cp->letter_space;
    }

    if (m & PD_CP_KERNING) {
        z.kerning = cp->kerning;
    }

    if (m & PD_CP_LANG) {
        memcpy(z.lang, cp->lang, sizeof(z.lang));
        z.lang[sizeof(z.lang) - 1] = '\0';
        memset(z.lang + strlen(z.lang), 0, sizeof(z.lang) - strlen(z.lang));
    }

    if (m & PD_CP_SMALLCAPS) {
        z.small_caps = cp->small_caps;
    }

    if (m & PD_CP_LINK) {
        z.link_target = cp->link_target;
    }

    *cp = z;
}

void pd_doc_pp_normalize(pd_para_props* pp) {
    pd_para_props z;
    uint32_t m = pp->mask & PD_PP_ALL;

    memset(&z, 0, sizeof(z));
    z.mask = m;
#define KEEP(bit, f) if (m & (bit)) { z.f = pp->f; }
    KEEP(PD_PP_ALIGN, align);
    KEEP(PD_PP_INDENT_LEFT, indent_left);
    KEEP(PD_PP_INDENT_RIGHT, indent_right);
    KEEP(PD_PP_INDENT_FIRST, indent_first);
    KEEP(PD_PP_SPACE_BEFORE, space_before);
    KEEP(PD_PP_SPACE_AFTER, space_after);
    KEEP(PD_PP_LINE_SPACING, line_spacing);
    KEEP(PD_PP_KEEP_NEXT, keep_with_next);
    KEEP(PD_PP_KEEP_LINES, keep_lines);
    KEEP(PD_PP_WIDOWS, widows);
    KEEP(PD_PP_ORPHANS, orphans);
    KEEP(PD_PP_BREAK_BEFORE, page_break_before);
    KEEP(PD_PP_HYPHENATE, hyphenate);
    KEEP(PD_PP_BREAK_MODE, break_mode);
    KEEP(PD_PP_NEXT_STYLE, next_style);

    if (m & PD_PP_BORDER) {
        z.border_color = pp->border_color;
        z.border_width = pp->border_width;
    }

    KEEP(PD_PP_SHADING, shading);
#undef KEEP
    *pp = z;
}

static void cp_apply(pd_char_props* dst, const pd_char_props* src) {
    uint32_t m = src->mask;

    if (m & PD_CP_FAMILY) {
        memcpy(dst->family, src->family, sizeof(dst->family));
    }

    if (m & PD_CP_SIZE) {
        dst->size = src->size;
    }

    if (m & PD_CP_WEIGHT) {
        dst->weight = src->weight;
    }

    if (m & PD_CP_ITALIC) {
        dst->italic = src->italic;
    }

    if (m & PD_CP_COLOR) {
        dst->color = src->color;
    }

    if (m & PD_CP_BACKGROUND) {
        dst->background = src->background;
    }

    if (m & PD_CP_UNDERLINE) {
        dst->underline = src->underline;
    }

    if (m & PD_CP_STRIKE) {
        dst->strike = src->strike;
    }

    if (m & PD_CP_SHIFT) {
        dst->shift = src->shift;
    }

    if (m & PD_CP_LETTERSPACE) {
        dst->letter_space = src->letter_space;
    }

    if (m & PD_CP_KERNING) {
        dst->kerning = src->kerning;
    }

    if (m & PD_CP_LANG) {
        memcpy(dst->lang, src->lang, sizeof(dst->lang));
    }

    if (m & PD_CP_SMALLCAPS) {
        dst->small_caps = src->small_caps;
    }

    if (m & PD_CP_LINK) {
        dst->link_target = src->link_target;
    }

    dst->mask |= m;
}

static void pp_apply(pd_para_props* dst, const pd_para_props* src) {
    uint32_t m = src->mask;
#define SET(bit, f) if (m & (bit)) { dst->f = src->f; }
    SET(PD_PP_ALIGN, align);
    SET(PD_PP_INDENT_LEFT, indent_left);
    SET(PD_PP_INDENT_RIGHT, indent_right);
    SET(PD_PP_INDENT_FIRST, indent_first);
    SET(PD_PP_SPACE_BEFORE, space_before);
    SET(PD_PP_SPACE_AFTER, space_after);
    SET(PD_PP_LINE_SPACING, line_spacing);
    SET(PD_PP_KEEP_NEXT, keep_with_next);
    SET(PD_PP_KEEP_LINES, keep_lines);
    SET(PD_PP_WIDOWS, widows);
    SET(PD_PP_ORPHANS, orphans);
    SET(PD_PP_BREAK_BEFORE, page_break_before);
    SET(PD_PP_HYPHENATE, hyphenate);
    SET(PD_PP_BREAK_MODE, break_mode);
    SET(PD_PP_NEXT_STYLE, next_style);

    if (m & PD_PP_BORDER) {
        dst->border_color = src->border_color;
        dst->border_width = src->border_width;
    }

    SET(PD_PP_SHADING, shading);
#undef SET
    dst->mask |= m;
}

static void default_props(pd_para_props* pp, pd_char_props* cp) {
    if (pp) {
        memset(pp, 0, sizeof(*pp));
        pp->mask = PD_PP_ALL;
        pp->align = PD_ALIGN_LEFT;
        pp->line_spacing = 1000;
        pp->widows = 2;
        pp->orphans = 2;
        pp->hyphenate = 1;
        pp->break_mode = PD_BREAK_OPTIMAL;
    }

    if (cp) {
        memset(cp, 0, sizeof(*cp));
        cp->mask = PD_CP_ALL;
        cp->size = PD_PT(10);
        cp->weight = 400;
        cp->color = 0xFF000000u;
        cp->kerning = 1;
        strcpy(cp->lang, "en-US");
    }
}

/* ------------------------------------------------------------------ */
/* blocks                                                             */
/* ------------------------------------------------------------------ */

void pd_doc_bstate_init(bstate* s, int32_t kind) {
    memset(s, 0, sizeof(*s));

    if (kind == PD_BLOCK_FLOAT) {
        s->fp.placement = PD_PLACE_HERE | PD_PLACE_TOP | PD_PLACE_BOTTOM | PD_PLACE_PAGE;
        s->fp.width_fraction = 1000;
        s->fp.gap = PD_PT(10);
        strcpy(s->fp.sequence, "Figure");
    } else if (kind == PD_BLOCK_SECTION) {
        pd_section_props_init(&s->sp);
    }
}

static void bstate_free(bstate* s) {
    int32_t i;

    free(s->text);
    free(s->runs);

    for (i = 0; i < s->ninl; i++) {
        free(s->inl[i].source);
    }

    free(s->inl);
    memset(s, 0, sizeof(*s));
}

static int bstate_copy(bstate* dst, const bstate* src) {
    int32_t i;

    *dst = *src;
    dst->text = NULL;
    dst->runs = NULL;
    dst->inl = NULL;
    dst->cap = dst->caprun = dst->capinl = 0;

    if (src->len) {
        dst->text = (char*)malloc(src->len + 1);

        if (!dst->text) {
            return -1;
        }

        memcpy(dst->text, src->text, src->len);
        dst->text[src->len] = '\0';
        dst->cap = (uint32_t)src->len + 1;
    }

    if (src->nruns) {
        dst->runs = (pd_run*)malloc((size_t)src->nruns * sizeof(pd_run));

        if (!dst->runs) {
            return -1;
        }

        memcpy(dst->runs, src->runs, (size_t)src->nruns * sizeof(pd_run));
        dst->caprun = src->nruns;
    }

    if (src->ninl) {
        dst->inl = (dinline*)calloc((size_t)src->ninl, sizeof(dinline));

        if (!dst->inl) {
            return -1;
        }

        dst->capinl = src->ninl;

        for (i = 0; i < src->ninl; i++) {
            dst->inl[i] = src->inl[i];

            if (src->inl[i].source) {
                size_t n = (size_t)src->inl[i].obj.source_len;

                dst->inl[i].source = (char*)malloc(n + 1);

                if (!dst->inl[i].source) {
                    return -1;
                }

                memcpy(dst->inl[i].source, src->inl[i].source, n + 1);
            }

            dst->inl[i].obj.source = dst->inl[i].source;
        }
    }

    return 0;
}

blk* pd_doc_new_blk(pd_doc* d, int32_t kind) {
    blk* b;

    if (d->next_id >= PD_MAX_BLOCKS) {
        return NULL;
    }

    if (d->next_id >= d->captab) {
        uint32_t nc = d->captab ? d->captab * 2 : 64;
        blk** t = (blk**)realloc(d->tab, nc * sizeof(blk*));

        if (!t) {
            return NULL;
        }

        memset(t + d->captab, 0, (nc - d->captab) * sizeof(blk*));
        d->tab = t;
        d->captab = nc;
    }

    b = (blk*)calloc(1, sizeof(blk));

    if (!b) {
        return NULL;
    }

    b->kind = kind;
    b->id = d->next_id++;
    b->born = d->cur ? d->cur->serial : 0;
    pd_doc_bstate_init(&b->st, kind);

    if (kind == PD_BLOCK_PARAGRAPH) {
        b->st.style = pd_doc_style_find(d, "Normal");
    }

    d->tab[b->id] = b;
    return b;
}

void pd_doc_free_blk(blk* b) {
    if (b) {
        bstate_free(&b->st);
        free(b->kids);
        free(b);
    }
}

/* free a detached subtree for good */
static void free_subtree(pd_doc* d, pd_block_id id) {
    blk* b = id < d->captab ? d->tab[id] : NULL;
    int32_t i;

    if (!b) {
        return;
    }

    for (i = 0; i < b->nkids; i++) {
        free_subtree(d, b->kids[i]);
    }

    d->tab[id] = NULL;
    pd_doc_free_blk(b);
}

blk* pd_doc_blk(const pd_doc* d, pd_block_id id) {
    blk* b;

    if (!d || id == 0 || id >= d->captab) {
        return NULL;
    }

    b = d->tab[id];
    return (b && b->alive) ? b : NULL;
}

static blk* para_of(const pd_doc* d, pd_block_id id) {
    blk* b = pd_doc_blk(d, id);
    return (b && b->kind == PD_BLOCK_PARAGRAPH) ? b : NULL;
}

int pd_doc_add_kid(blk* parent, pd_block_id kid, int32_t index) {
    if (grow((void**)&parent->kids, &parent->capkids, (int64_t)parent->nkids + 1, sizeof(pd_block_id))) {
        return -1;
    }

    if (index < 0 || index > parent->nkids) {
        index = parent->nkids;
    }

    memmove(parent->kids + index + 1, parent->kids + index, (size_t)(parent->nkids - index) * sizeof(pd_block_id));
    parent->kids[index] = kid;
    parent->nkids++;
    return 0;
}

static int32_t kid_index(const blk* parent, pd_block_id kid) {
    int32_t i;

    for (i = 0; i < parent->nkids; i++) {
        if (parent->kids[i] == kid) {
            return i;
        }
    }

    return -1;
}

static void set_alive(pd_doc* d, pd_block_id id, int alive) {
    blk* b = d->tab[id];
    int32_t i;

    b->alive = alive;

    for (i = 0; i < b->nkids; i++) {
        set_alive(d, b->kids[i], alive);
    }
}

int pd_doc_child_allowed(int32_t pk, int32_t kk) {
    switch (pk) {
        case PD_BLOCK_ROOT:
            return kk == PD_BLOCK_SECTION;

        case PD_BLOCK_SECTION:
            return kk == PD_BLOCK_PARAGRAPH || kk == PD_BLOCK_FLOAT || kk == PD_BLOCK_TABLE || kk == PD_BLOCK_BREAK;

        case PD_BLOCK_FLOAT:
        case PD_BLOCK_CELL:
        case PD_BLOCK_STORY:
            return kk == PD_BLOCK_PARAGRAPH || kk == PD_BLOCK_TABLE;

        case PD_BLOCK_TABLE:
            return kk == PD_BLOCK_ROW;

        case PD_BLOCK_ROW:
            return kk == PD_BLOCK_CELL;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* document lifecycle                                                 */
/* ------------------------------------------------------------------ */

pd_doc* pd_doc_alloc(void) {
    pd_doc* d = (pd_doc*)calloc(1, sizeof(pd_doc));

    if (d) {
        d->next_id = 1;
        d->undo_limit = 1000;
    }

    return d;
}

static void step_free(pd_doc* d, ustep* s, int dropping) {
    int32_t i;

    for (i = 0; i < s->n; i++) {
        urec* r = &s->r[i];

        if (r->type == UR_STATE) {
            bstate_free(&r->saved);
        } else if (r->type == UR_ATTACH && !r->attached && dropping) {
            /* the only owner of a detached subtree is the record that detached it */
            blk* b = r->id < d->captab ? d->tab[r->id] : NULL;

            if (b && !b->alive && b->parent == r->parent) {
                free_subtree(d, r->id);
            }
        }
    }

    free(s->r);
    memset(s, 0, sizeof(*s));
}

void pd_doc_free(pd_doc* d) {
    int32_t i;
    uint32_t k;

    if (!d) {
        return;
    }

    for (i = 0; i < d->nundo; i++) {
        step_free(d, &d->undo[i], 0);
    }

    for (i = 0; i < d->nredo; i++) {
        step_free(d, &d->redo[i], 0);
    }

    for (k = 0; k < d->captab; k++) {
        pd_doc_free_blk(d->tab[k]);
    }

    for (i = 0; i < d->nres; i++) {
        free(d->res[i].data);
    }

    free(d->tab);
    free(d->styles);
    free(d->formats);
    free(d->lists);
    free(d->res);
    free(d->markers);
    free(d->undo);
    free(d->redo);
    free(d->touched);
    free(d);
}

static int add_style_raw(pd_doc* d, const char* name, int32_t kind, pd_style_id parent, const pd_para_props* pp,
                         const pd_char_props* cp, pd_style_id* out) {
    dstyle* s;

    if (grow((void**)&d->styles, &d->capstyles, (int64_t)d->nstyles + 1, sizeof(dstyle))) {
        return -1;
    }

    s = &d->styles[d->nstyles];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->kind = kind;
    s->parent = parent;

    if (pp) {
        s->pp = *pp;
        pd_doc_pp_normalize(&s->pp);
    }

    if (cp) {
        s->cp = *cp;
        pd_doc_cp_normalize(&s->cp);
    }

    s->alive = 1;
    d->nstyles++;

    if (out) {
        *out = (pd_style_id)d->nstyles;
    }

    return 0;
}

void pd_doc_install_builtin_styles(pd_doc* d) {
    static const int32_t hsize[6] = { 20, 16, 14, 12, 11, 10 };
    pd_para_props pp;
    pd_char_props cp;
    pd_style_id normal;
    char name[32];
    int k;

    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    add_style_raw(d, "Normal", PD_STYLE_PARAGRAPH, 0, &pp, &cp, &normal);

    for (k = 0; k < 6; k++) {
        memset(&pp, 0, sizeof(pp));
        memset(&cp, 0, sizeof(cp));
        pp.mask = PD_PP_SPACE_BEFORE | PD_PP_SPACE_AFTER | PD_PP_KEEP_NEXT | PD_PP_NEXT_STYLE | PD_PP_HYPHENATE;
        pp.space_before = PD_PT(hsize[k]);
        pp.space_after = PD_PT(hsize[k] / 2);
        pp.keep_with_next = 1;
        pp.next_style = normal;
        cp.mask = PD_CP_SIZE | PD_CP_WEIGHT;
        cp.size = PD_PT(hsize[k]);
        cp.weight = 700;
        snprintf(name, sizeof(name), "Heading %d", k + 1);
        add_style_raw(d, name, PD_STYLE_PARAGRAPH, normal, &pp, &cp, NULL);
    }

    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    pp.mask = PD_PP_ALIGN | PD_PP_SPACE_AFTER | PD_PP_NEXT_STYLE | PD_PP_HYPHENATE;
    pp.align = PD_ALIGN_CENTER;
    pp.space_after = PD_PT(18);
    pp.next_style = normal;
    cp.mask = PD_CP_SIZE | PD_CP_WEIGHT;
    cp.size = PD_PT(24);
    cp.weight = 700;
    add_style_raw(d, "Title", PD_STYLE_PARAGRAPH, normal, &pp, &cp, NULL);

    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    pp.mask = PD_PP_SPACE_BEFORE | PD_PP_SPACE_AFTER;
    pp.space_before = PD_PT(6);
    pp.space_after = PD_PT(10);
    cp.mask = PD_CP_SIZE | PD_CP_ITALIC;
    cp.size = PD_PT(9);
    cp.italic = 1;
    add_style_raw(d, "Caption", PD_STYLE_PARAGRAPH, normal, &pp, &cp, NULL);

    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    pp.mask = PD_PP_INDENT_LEFT | PD_PP_INDENT_RIGHT;
    pp.indent_left = pp.indent_right = PD_PT(24);
    cp.mask = PD_CP_ITALIC;
    cp.italic = 1;
    add_style_raw(d, "Quote", PD_STYLE_PARAGRAPH, normal, &pp, &cp, NULL);

    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    pp.mask = PD_PP_ALIGN | PD_PP_HYPHENATE | PD_PP_BREAK_MODE;
    pp.align = PD_ALIGN_LEFT;
    pp.break_mode = PD_BREAK_GREEDY;
    cp.mask = PD_CP_FAMILY | PD_CP_SIZE | PD_CP_KERNING;
    strcpy(cp.family, "monospace");
    cp.size = PD_PT(9);
    add_style_raw(d, "Code", PD_STYLE_PARAGRAPH, normal, &pp, &cp, NULL);

    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_ITALIC;
    cp.italic = 1;
    add_style_raw(d, "Emphasis", PD_STYLE_CHARACTER, 0, NULL, &cp, NULL);
    memset(&cp, 0, sizeof(cp));
    cp.mask = PD_CP_WEIGHT;
    cp.weight = 700;
    add_style_raw(d, "Strong", PD_STYLE_CHARACTER, 0, NULL, &cp, NULL);
}

/* a container with one empty paragraph */
static blk* new_container(pd_doc* d, int32_t kind) {
    blk* c = pd_doc_new_blk(d, kind), *p;

    if (!c) {
        return NULL;
    }

    if (kind == PD_BLOCK_SECTION || kind == PD_BLOCK_FLOAT || kind == PD_BLOCK_STORY || kind == PD_BLOCK_CELL) {
        p = pd_doc_new_blk(d, PD_BLOCK_PARAGRAPH);

        if (!p || pd_doc_add_kid(c, p->id, -1)) {
            return NULL;
        }

        p->parent = c->id;

        if (kind == PD_BLOCK_FLOAT) {
            p->st.role = PD_ROLE_FIGURE_CONTENT;
        }
    }

    return c;
}

pd_status pd_doc_new(pd_doc** out) {
    pd_doc* d;
    blk* root, *sr, *sec;

    if (!out) {
        return PD_ERR_ARG;
    }

    *out = NULL;
    d = pd_doc_alloc();

    if (!d) {
        return PD_ERR_NOMEM;
    }

    root = pd_doc_new_blk(d, PD_BLOCK_ROOT);
    sr = pd_doc_new_blk(d, PD_BLOCK_ROOT);
    sec = root && sr ? new_container(d, PD_BLOCK_SECTION) : NULL;

    if (!sec || pd_doc_add_kid(root, sec->id, -1)) {
        pd_doc_free(d);
        return PD_ERR_NOMEM;
    }

    sec->parent = root->id;
    set_alive(d, PD_ROOT_ID, 1);
    set_alive(d, PD_STORYROOT_ID, 1);
    pd_doc_install_builtin_styles(d);
    d->tab[sec->kids[0]]->st.style = 1;     /* Normal */
    *out = d;
    return PD_OK;
}

void pd_doc_set_font_resolver(pd_doc* d, pd_font_resolver fn, void* user) {
    if (d) {
        d->resolver = fn;
        d->resolver_user = user;
    }
}

void pd_doc_set_default_font(pd_doc* d, const pd_font* font) {
    if (d) {
        d->default_font = font;
    }
}

uint64_t pd_doc_revision(const pd_doc* d) {
    return d ? d->revision : 0;
}

void pd_doc_set_listener(pd_doc* d, pd_doc_listener fn, void* user) {
    if (d) {
        d->listener = fn;
        d->listener_user = user;
    }
}

/* ------------------------------------------------------------------ */
/* tree queries                                                       */
/* ------------------------------------------------------------------ */

pd_block_id pd_doc_root(const pd_doc* d) {
    return d ? PD_ROOT_ID : 0;
}

int32_t pd_doc_story_count(const pd_doc* d) {
    blk* sr = pd_doc_blk(d, PD_STORYROOT_ID);
    return sr ? sr->nkids : 0;
}

pd_block_id pd_doc_story_at(const pd_doc* d, int32_t i) {
    blk* sr = pd_doc_blk(d, PD_STORYROOT_ID);
    return (sr && i >= 0 && i < sr->nkids) ? sr->kids[i] : 0;
}

pd_status pd_doc_block_info(const pd_doc* d, pd_block_id id, pd_block_info* out) {
    blk* b = pd_doc_blk(d, id);
    blk* p;

    if (!b || !out || id == PD_STORYROOT_ID) {
        return PD_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->kind = b->kind;
    out->id = b->id;
    out->parent = b->parent == PD_STORYROOT_ID ? 0 : b->parent;
    out->child_count = b->nkids;
    p = pd_doc_blk(d, b->parent);
    out->index = p ? kid_index(p, id) : 0;
    out->role = b->st.role;
    out->level = b->st.level;
    out->list_level = b->st.list_level;
    out->style = b->st.style;
    out->list = b->st.list;
    out->break_kind = b->st.break_kind;
    out->text_length = b->st.len;
    out->revision = b->revision;
    return PD_OK;
}

pd_block_id pd_doc_child(const pd_doc* d, pd_block_id parent, int32_t i) {
    blk* p = pd_doc_blk(d, parent);
    return (p && parent != PD_STORYROOT_ID && i >= 0 && i < p->nkids) ? p->kids[i] : 0;
}

static pd_block_id first_para_in(const pd_doc* d, pd_block_id id, int last) {
    blk* b = pd_doc_blk(d, id);
    int32_t i;

    if (!b) {
        return 0;
    }

    if (b->kind == PD_BLOCK_PARAGRAPH) {
        return id;
    }

    for (i = 0; i < b->nkids; i++) {
        pd_block_id r = first_para_in(d, b->kids[last ? b->nkids - 1 - i : i], last);

        if (r) {
            return r;
        }
    }

    return 0;
}

/* next (dir = 1) or previous (dir = -1) paragraph in reading order, inside one flow */
static pd_block_id step_para(const pd_doc* d, pd_block_id id, int dir) {
    blk* b = pd_doc_blk(d, id);

    while (b) {
        blk* p = pd_doc_blk(d, b->parent);
        int32_t i;

        if (!p || b->kind == PD_BLOCK_STORY) {
            return 0;
        }

        for (i = kid_index(p, b->id) + dir; i >= 0 && i < p->nkids; i += dir) {
            pd_block_id r = first_para_in(d, p->kids[i], dir < 0);

            if (r) {
                return r;
            }
        }

        if (p->kind == PD_BLOCK_ROOT || p->kind == PD_BLOCK_STORY) {
            return 0;
        }

        b = p;
    }

    return 0;
}

pd_block_id pd_doc_next_paragraph(const pd_doc* d, pd_block_id id) {
    return para_of(d, id) ? step_para(d, id, 1) : 0;
}

pd_block_id pd_doc_prev_paragraph(const pd_doc* d, pd_block_id id) {
    return para_of(d, id) ? step_para(d, id, -1) : 0;
}

/* ------------------------------------------------------------------ */
/* styles and formats                                                 */
/* ------------------------------------------------------------------ */

static dstyle* style_of(const pd_doc* d, pd_style_id id) {
    return (id >= 1 && (int32_t)id <= d->nstyles && d->styles[id - 1].alive) ? &d->styles[id - 1] : NULL;
}

pd_style_id pd_doc_style_find(const pd_doc* d, const char* name) {
    int32_t i;

    if (!d || !name) {
        return 0;
    }

    for (i = 0; i < d->nstyles; i++) {
        if (d->styles[i].alive && strcmp(d->styles[i].name, name) == 0) {
            return (pd_style_id)(i + 1);
        }
    }

    return 0;
}

int32_t pd_doc_style_count(const pd_doc* d) {
    int32_t i, n = 0;

    for (i = 0; d && i < d->nstyles; i++) {
        n += d->styles[i].alive;
    }

    return n;
}

pd_style_id pd_doc_style_at(const pd_doc* d, int32_t index) {
    int32_t i;

    for (i = 0; d && i < d->nstyles; i++) {
        if (d->styles[i].alive && index-- == 0) {
            return (pd_style_id)(i + 1);
        }
    }

    return 0;
}

const char* pd_doc_style_name(const pd_doc* d, pd_style_id id) {
    dstyle* s = d ? style_of(d, id) : NULL;
    return s ? s->name : NULL;
}

/* apply a style's chain, root ancestor first */
static void apply_chain(const pd_doc* d, pd_style_id id, pd_para_props* pp, pd_char_props* cp, int depth) {
    dstyle* s = style_of(d, id);

    if (!s || depth > 64) {
        return;
    }

    apply_chain(d, s->parent, pp, cp, depth + 1);

    if (pp) {
        pp_apply(pp, &s->pp);
    }

    if (cp) {
        cp_apply(cp, &s->cp);
    }
}

pd_status pd_doc_style_resolve(const pd_doc* d, pd_style_id id, pd_para_props* pp, pd_char_props* cp) {
    if (!d || (id && !style_of(d, id))) {
        return PD_ERR_ARG;
    }

    default_props(pp, cp);
    apply_chain(d, id, pp, cp, 0);
    return PD_OK;
}

int pd_doc_intern_raw(pd_doc* d, const dformat* f, pd_format_id* out) {
    int32_t i;

    for (i = 0; i < d->nformats; i++) {
        if (memcmp(&d->formats[i], f, sizeof(*f)) == 0) {
            *out = (pd_format_id)(i + 1);
            return 0;
        }
    }

    if (grow((void**)&d->formats, &d->capformats, (int64_t)d->nformats + 1, sizeof(dformat))) {
        return -1;
    }

    d->formats[d->nformats++] = *f;
    *out = (pd_format_id)d->nformats;
    return 0;
}

static pd_format_id intern(pd_doc* d, pd_style_id style, const pd_char_props* cp) {
    dformat f;
    pd_format_id id = 0;

    memset(&f, 0, sizeof(f));
    f.style = style;

    if (cp) {
        f.cp = *cp;
    }

    pd_doc_cp_normalize(&f.cp);

    if (f.style == 0 && f.cp.mask == 0) {
        return 0;   /* the default format */
    }

    return pd_doc_intern_raw(d, &f, &id) ? 0 : id;
}

pd_format_id pd_doc_format(pd_doc* d, pd_style_id char_style, const pd_char_props* overrides) {
    dstyle* s;

    if (!d) {
        return 0;
    }

    s = style_of(d, char_style);

    if (char_style && (!s || s->kind != PD_STYLE_CHARACTER)) {
        return 0;
    }

    return intern(d, char_style, overrides);
}

static const dformat* format_of(const pd_doc* d, pd_format_id id) {
    static const dformat none;
    return (id >= 1 && (int32_t)id <= d->nformats) ? &d->formats[id - 1] : (id == 0 ? &none : NULL);
}

pd_status pd_doc_format_resolve(const pd_doc* d, pd_block_id para, pd_format_id fmt, pd_char_props* out) {
    blk* b = para_of(d, para);
    const dformat* f = d ? format_of(d, fmt) : NULL;

    if (!b || !f || !out) {
        return PD_ERR_ARG;
    }

    default_props(NULL, out);
    apply_chain(d, b->st.style, NULL, out, 0);
    apply_chain(d, f->style, NULL, out, 0);
    cp_apply(out, &f->cp);
    out->mask = PD_CP_ALL;
    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* lists                                                              */
/* ------------------------------------------------------------------ */

pd_status pd_doc_list_define(pd_doc* d, int32_t n, const pd_list_level* lv, pd_list_id* out) {
    dlist* l;
    int32_t i;

    if (!d || !lv || !out || n < 1 || n > 9) {
        return PD_ERR_ARG;
    }

    if (grow((void**)&d->lists, &d->caplists, (int64_t)d->nlists + 1, sizeof(dlist))) {
        return PD_ERR_NOMEM;
    }

    l = &d->lists[d->nlists];
    memset(l, 0, sizeof(*l));
    l->n = n;

    for (i = 0; i < n; i++) {
        if (lv[i].format < PD_NUM_BULLET || lv[i].format > PD_NUM_NONE) {
            return PD_ERR_ARG;
        }

        l->lv[i] = lv[i];
        l->lv[i].text[sizeof(l->lv[i].text) - 1] = '\0';
    }

    d->nlists++;
    *out = (pd_list_id)d->nlists;
    return PD_OK;
}

static void format_number(int32_t v, int32_t fmt, char* buf, size_t cap) {
    static const char* rn[13] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    static const int32_t rv[13] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    size_t o = 0;
    int k;

    buf[0] = '\0';

    if (fmt == PD_NUM_DECIMAL || v <= 0) {
        snprintf(buf, cap, "%d", (int)v);
    } else if (fmt == PD_NUM_LOWER_ALPHA || fmt == PD_NUM_UPPER_ALPHA) {
        char tmp[16];
        int n = 0;

        for (; v > 0 && n < 15; v = (v - 1) / 26) {
            tmp[n++] = (char)((fmt == PD_NUM_LOWER_ALPHA ? 'a' : 'A') + (v - 1) % 26);
        }

        while (n > 0 && o + 1 < cap) {
            buf[o++] = tmp[--n];
        }

        buf[o] = '\0';
    } else if (fmt == PD_NUM_LOWER_ROMAN || fmt == PD_NUM_UPPER_ROMAN) {
        for (k = 0; k < 13 && v < 4000; k++) {
            while (v >= rv[k]) {
                const char* s = rn[k];

                for (; *s && o + 1 < cap; s++) {
                    buf[o++] = (char)(fmt == PD_NUM_UPPER_ROMAN ? *s - 32 : *s);
                }

                v -= rv[k];
            }
        }

        buf[o] = '\0';
    }
}

pd_status pd_doc_list_label(const pd_doc* d, pd_block_id para, char* buf, int32_t cap) {
    blk* b = para_of(d, para);
    const dlist* l;
    int32_t cnt[9], seen[9], k;
    pd_block_id cur;
    size_t o = 0;
    const char* t;

    if (!b || !buf || cap < 1) {
        return PD_ERR_ARG;
    }

    buf[0] = '\0';

    if (!b->st.list || (int32_t)b->st.list > d->nlists) {
        return PD_OK;
    }

    l = &d->lists[b->st.list - 1];
    memset(cnt, 0, sizeof(cnt));
    memset(seen, 0, sizeof(seen));

    /* count list items from the start of this flow */
    for (cur = para; ; ) {
        pd_block_id prev = step_para(d, cur, -1);

        if (!prev) {
            break;
        }

        cur = prev;
    }

    for (; cur; cur = step_para(d, cur, 1)) {
        blk* c = d->tab[cur];
        int32_t lv = c->st.list_level;

        if (c->st.list != b->st.list) {
            continue;
        }

        lv = lv < l->n ? lv : l->n - 1;
        cnt[lv] = seen[lv] ? cnt[lv] + 1 : l->lv[lv].start;
        seen[lv] = 1;

        for (k = lv + 1; k < 9; k++) {
            seen[k] = 0;
        }

        if (cur == para) {
            const pd_list_level* L = &l->lv[lv];

            if (L->format == PD_NUM_BULLET) {
                strncpy(buf, L->text, (size_t)cap - 1);
                buf[cap - 1] = '\0';
                return PD_OK;
            }

            for (t = L->text; *t && o + 1 < (size_t)cap; t++) {
                if (t[0] == '%' && t[1] >= '1' && t[1] <= '9') {
                    char num[32];
                    int32_t at = t[1] - '1';
                    size_t q;

                    format_number(at < l->n ? cnt[at] : 0, at < l->n ? l->lv[at].format : PD_NUM_DECIMAL, num,
                                  sizeof(num));

                    for (q = 0; num[q] && o + 1 < (size_t)cap; q++) {
                        buf[o++] = num[q];
                    }

                    t++;
                } else {
                    buf[o++] = *t;
                }
            }

            buf[o] = '\0';
            return PD_OK;
        }
    }

    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* content queries                                                    */
/* ------------------------------------------------------------------ */

pd_status pd_doc_para_text(const pd_doc* d, pd_block_id para, const char** utf8, uint32_t* len) {
    blk* b = para_of(d, para);

    if (!b || !utf8 || !len) {
        return PD_ERR_ARG;
    }

    *utf8 = b->st.text ? b->st.text : "";
    *len = b->st.len;
    return PD_OK;
}

pd_status pd_doc_para_runs(const pd_doc* d, pd_block_id para, pd_run* buf, int32_t cap, int32_t* count) {
    blk* b = para_of(d, para);

    if (!b || !count || cap < 0) {
        return PD_ERR_ARG;
    }

    *count = b->st.nruns;

    if (!buf) {
        return PD_OK;
    }

    if (cap < b->st.nruns) {
        return PD_ERR_RANGE;
    }

    if (b->st.nruns) {
        memcpy(buf, b->st.runs, (size_t)b->st.nruns * sizeof(pd_run));
    }

    return PD_OK;
}

static int32_t inline_index(const bstate* s, uint32_t off) {
    int32_t i;

    for (i = 0; i < s->ninl; i++) {
        if (s->inl[i].offset == off) {
            return i;
        }
    }

    return -1;
}

pd_status pd_doc_inline_at(const pd_doc* d, pd_pos pos, pd_inline* out) {
    blk* b = para_of(d, pos.block);
    int32_t i;

    if (!b || !out) {
        return PD_ERR_ARG;
    }

    i = inline_index(&b->st, pos.offset);

    if (i < 0) {
        return PD_ERR_RANGE;
    }

    *out = b->st.inl[i].obj;
    out->source = b->st.inl[i].source;
    return PD_OK;
}

pd_status pd_doc_add_resource(pd_doc* d, const char* mime, const void* data, size_t len, pd_res_id* out) {
    dres* r;

    if (!d || !mime || (!data && len) || !out) {
        return PD_ERR_ARG;
    }

    if (grow((void**)&d->res, &d->capres, (int64_t)d->nres + 1, sizeof(dres))) {
        return PD_ERR_NOMEM;
    }

    r = &d->res[d->nres];
    memset(r, 0, sizeof(*r));
    strncpy(r->mime, mime, sizeof(r->mime) - 1);
    r->data = (unsigned char*)malloc(len ? len : 1);

    if (!r->data) {
        return PD_ERR_NOMEM;
    }

    if (len) {
        memcpy(r->data, data, len);
    }

    r->len = len;
    d->nres++;
    *out = (pd_res_id)d->nres;
    return PD_OK;
}

pd_status pd_doc_resource(const pd_doc* d, pd_res_id id, const char** mime, const void** data, size_t* len) {
    if (!d || id < 1 || (int32_t)id > d->nres) {
        return PD_ERR_ARG;
    }

    if (mime) {
        *mime = d->res[id - 1].mime;
    }

    if (data) {
        *data = d->res[id - 1].data;
    }

    if (len) {
        *len = d->res[id - 1].len;
    }

    return PD_OK;
}

pd_status pd_doc_float_props(const pd_doc* d, pd_block_id id, pd_float_props* out) {
    blk* b = pd_doc_blk(d, id);

    if (!b || b->kind != PD_BLOCK_FLOAT || !out) {
        return PD_ERR_ARG;
    }

    *out = b->st.fp;
    return PD_OK;
}

pd_status pd_doc_section_props(const pd_doc* d, pd_block_id id, pd_section_props* out) {
    blk* b = pd_doc_blk(d, id);

    if (!b || b->kind != PD_BLOCK_SECTION || !out) {
        return PD_ERR_ARG;
    }

    *out = b->st.sp;
    return PD_OK;
}

void pd_section_props_init(pd_section_props* sp) {
    if (!sp) {
        return;
    }

    memset(sp, 0, sizeof(*sp));
    sp->page_width = (pd_sp)(595.276 * PD_SP_PER_PT);  /* A4 */
    sp->page_height = (pd_sp)(841.890 * PD_SP_PER_PT);
    sp->margin_top = sp->margin_bottom = sp->margin_left = sp->margin_right = PD_PT(72);
    sp->header_distance = sp->footer_distance = PD_PT(36);
    sp->columns = 1;
    sp->column_gap = PD_PT(18);
    sp->first_page_number = 1;
    sp->page_number_format = PD_NUM_DECIMAL;
}

/* ------------------------------------------------------------------ */
/* operation framework                                                */
/* ------------------------------------------------------------------ */

static void touch(pd_doc* d, int32_t kind, pd_block_id block, pd_style_id style) {
    int32_t i;

    for (i = 0; i < d->ntouched; i++) {
        if (d->touched[i].kind == kind && d->touched[i].block == block && d->touched[i].style == style) {
            return;
        }
    }

    if (grow((void**)&d->touched, &d->captouched, (int64_t)d->ntouched + 1, sizeof(dtouch)) == 0) {
        d->touched[d->ntouched].kind = kind;
        d->touched[d->ntouched].block = block;
        d->touched[d->ntouched].style = style;
        d->ntouched++;
    }
}

static void clear_redo(pd_doc* d) {
    int32_t i;

    for (i = 0; i < d->nredo; i++) {
        step_free(d, &d->redo[i], 1);
    }

    d->nredo = 0;
}

static ustep* push_step(pd_doc* d, const char* label) {
    ustep* s;

    if (grow((void**)&d->undo, &d->capundo, (int64_t)d->nundo + 1, sizeof(ustep))) {
        return NULL;
    }

    s = &d->undo[d->nundo++];
    memset(s, 0, sizeof(*s));
    strncpy(s->label, label ? label : "", sizeof(s->label) - 1);
    s->serial = ++d->serial;
    return s;
}

/* begin a public operation; typing at the end of the previous typing extends its step */
static int op_begin(pd_doc* d, const char* label, int typing, pd_block_id block, uint32_t off) {
    d->revision++;
    d->ntouched = 0;
    d->in_op = 1;
    clear_redo(d);

    if (d->group_depth > 0) {
        if (!d->cur) {      /* the group's step is created by its first operation */
            d->cur = push_step(d, d->group_label[0] ? d->group_label : label);
        }

        return d->cur ? 0 : -1;
    }

    if (typing && d->typing_open && d->nundo > 0 && d->typing_block == block && d->typing_end == off) {
        d->cur = &d->undo[d->nundo - 1];
    } else {
        d->cur = push_step(d, label);
    }

    d->typing_open = 0;
    return d->cur ? 0 : -1;
}

static void enforce_limit(pd_doc* d) {
    if (d->undo_limit > 0 && d->nundo > d->undo_limit) {
        int32_t drop = d->nundo - d->undo_limit, i;

        for (i = 0; i < drop; i++) {
            step_free(d, &d->undo[i], 1);
        }

        memmove(d->undo, d->undo + drop, (size_t)(d->nundo - drop) * sizeof(ustep));
        d->nundo -= drop;
    }
}

static void notify(pd_doc* d) {
    int32_t i;

    for (i = 0; i < d->ntouched; i++) {
        blk* b = d->touched[i].block < d->captab ? d->tab[d->touched[i].block] : NULL;

        if (b) {
            b->revision = d->revision;
        }
    }

    if (d->listener) {
        for (i = 0; i < d->ntouched; i++) {
            pd_change c;

            c.kind = d->touched[i].kind;
            c.block = d->touched[i].block;
            c.style = d->touched[i].style;
            c.revision = d->revision;
            d->listener(d->listener_user, &c);
        }
    }

    d->ntouched = 0;
}

static void op_end(pd_doc* d, int typing, pd_block_id block, uint32_t end) {
    if (d->group_depth == 0 && d->cur) {
        if (d->cur->n == 0 && d->cur == &d->undo[d->nundo - 1]) {
            free(d->cur->r);
            d->nundo--;     /* nothing to undo */
        }

        d->cur = NULL;
        enforce_limit(d);
    }

    d->typing_open = typing && d->group_depth == 0;
    d->typing_block = block;
    d->typing_end = end;
    d->in_op = 0;
    notify(d);
}

static urec* add_rec(pd_doc* d, int type) {
    urec* r;

    if (!d->cur || grow((void**)&d->cur->r, &d->cur->cap, (int64_t)d->cur->n + 1, sizeof(urec))) {
        return NULL;
    }

    r = &d->cur->r[d->cur->n++];
    memset(r, 0, sizeof(*r));
    r->type = type;
    return r;
}

/* save a block's state once per step before changing it */
static int snap(pd_doc* d, blk* b) {
    int32_t i;
    urec* r;

    /* a block created in this step needs no copy: undoing its attach removes it whole */
    if (b->born == d->cur->serial) {
        return 0;
    }

    for (i = 0; i < d->cur->n; i++) {
        if (d->cur->r[i].type == UR_STATE && d->cur->r[i].id == b->id) {
            return 0;   /* already saved in this step */
        }
    }

    r = add_rec(d, UR_STATE);

    if (!r) {
        return -1;
    }

    r->id = b->id;

    if (bstate_copy(&r->saved, &b->st)) {
        d->cur->n--;
        bstate_free(&r->saved);
        return -1;
    }

    return 0;
}

/* attach a detached subtree under parent at index, recording it */
static int do_attach(pd_doc* d, pd_block_id id, pd_block_id parent, int32_t index) {
    blk* b = d->tab[id], *p = d->tab[parent];
    urec* r = add_rec(d, UR_ATTACH);

    if (!r || pd_doc_add_kid(p, id, index)) {
        if (r) {
            d->cur->n--;
        }

        return -1;
    }

    b->parent = parent;
    set_alive(d, id, 1);
    r->id = id;
    r->attached = 1;
    r->parent = parent;
    r->index = kid_index(p, id);
    touch(d, PD_CHANGE_STRUCTURE, parent, 0);
    return 0;
}

static int do_detach(pd_doc* d, pd_block_id id) {
    blk* b = d->tab[id], *p = d->tab[b->parent];
    urec* r = add_rec(d, UR_ATTACH);
    int32_t i;

    if (!r) {
        return -1;
    }

    i = kid_index(p, id);
    memmove(p->kids + i, p->kids + i + 1, (size_t)(p->nkids - i - 1) * sizeof(pd_block_id));
    p->nkids--;
    set_alive(d, id, 0);
    r->id = id;
    r->attached = 0;
    r->parent = b->parent;
    r->index = i;
    touch(d, PD_CHANGE_STRUCTURE, b->parent, 0);
    return 0;
}

/* toggle one record (undo or redo) */
static void toggle(pd_doc* d, urec* r) {
    if (r->type == UR_STATE) {
        blk* b = d->tab[r->id];
        bstate t = b->st;

        b->st = r->saved;
        r->saved = t;
        touch(d, PD_CHANGE_TEXT, r->id, 0);
    } else if (r->type == UR_ATTACH) {
        blk* b = d->tab[r->id], *p = d->tab[r->parent];

        if (r->attached) {
            int32_t i = kid_index(p, r->id);

            memmove(p->kids + i, p->kids + i + 1, (size_t)(p->nkids - i - 1) * sizeof(pd_block_id));
            p->nkids--;
            set_alive(d, r->id, 0);
            r->index = i;
            r->attached = 0;
        } else {
            pd_doc_add_kid(p, r->id, r->index);
            b->parent = r->parent;
            set_alive(d, r->id, 1);
            r->attached = 1;
        }

        touch(d, PD_CHANGE_STRUCTURE, r->parent, 0);
    } else if (r->type == UR_STYLE) {
        dstyle t = d->styles[r->style - 1];

        d->styles[r->style - 1] = r->sdef;
        r->sdef = t;
        touch(d, PD_CHANGE_STYLE, 0, r->style);
    }
}

/* ------------------------------------------------------------------ */
/* markers                                                            */
/* ------------------------------------------------------------------ */

static int pos_valid(const pd_doc* d, pd_pos p) {
    blk* b = para_of(d, p.block);
    return b && is_boundary(&b->st, p.offset);
}

/* any marker left on a detached block or a bad offset goes to the nearest valid place */
static void clamp_markers(pd_doc* d, pd_pos fallback) {
    int32_t i;

    for (i = 0; i < d->nmarkers; i++) {
        dmarker* m = &d->markers[i];
        blk* b;

        if (!m->alive || pos_valid(d, m->pos)) {
            continue;
        }

        b = para_of(d, m->pos.block);

        if (b) {
            uint32_t o = m->pos.offset > b->st.len ? b->st.len : m->pos.offset;

            while (o > 0 && !is_boundary(&b->st, o)) {
                o--;
            }

            m->pos.offset = o;
        } else if (pos_valid(d, fallback)) {
            m->pos = fallback;
        } else {
            m->pos.block = first_para_in(d, PD_ROOT_ID, 0);
            m->pos.offset = 0;
        }
    }
}

pd_status pd_doc_marker_new(pd_doc* d, pd_pos pos, pd_gravity g, pd_marker_id* out) {
    int32_t i;

    if (!d || !out || !pos_valid(d, pos)) {
        return PD_ERR_ARG;
    }

    for (i = 0; i < d->nmarkers && d->markers[i].alive; i++) {
    }

    if (i == d->nmarkers) {
        if (grow((void**)&d->markers, &d->capmarkers, (int64_t)d->nmarkers + 1, sizeof(dmarker))) {
            return PD_ERR_NOMEM;
        }

        d->nmarkers++;
    }

    d->markers[i].pos = pos;
    d->markers[i].gravity = g;
    d->markers[i].alive = 1;
    *out = (pd_marker_id)(i + 1);
    return PD_OK;
}

void pd_doc_marker_free(pd_doc* d, pd_marker_id id) {
    if (d && id >= 1 && (int32_t)id <= d->nmarkers) {
        d->markers[id - 1].alive = 0;
    }
}

pd_status pd_doc_marker_get(const pd_doc* d, pd_marker_id id, pd_pos* out) {
    if (!d || !out || id < 1 || (int32_t)id > d->nmarkers || !d->markers[id - 1].alive) {
        return PD_ERR_ARG;
    }

    *out = d->markers[id - 1].pos;
    return PD_OK;
}

pd_status pd_doc_marker_set(pd_doc* d, pd_marker_id id, pd_pos pos) {
    if (!d || id < 1 || (int32_t)id > d->nmarkers || !d->markers[id - 1].alive || !pos_valid(d, pos)) {
        return PD_ERR_ARG;
    }

    d->markers[id - 1].pos = pos;
    return PD_OK;
}

static void markers_insert(pd_doc* d, pd_block_id b, uint32_t off, uint32_t n) {
    int32_t i;

    for (i = 0; i < d->nmarkers; i++) {
        dmarker* m = &d->markers[i];

        if (m->alive && m->pos.block == b &&
                (m->pos.offset > off || (m->pos.offset == off && m->gravity == PD_GRAVITY_RIGHT))) {
            m->pos.offset += n;
        }
    }
}

static void markers_delete(pd_doc* d, pd_block_id b, uint32_t s, uint32_t e) {
    int32_t i;

    for (i = 0; i < d->nmarkers; i++) {
        dmarker* m = &d->markers[i];

        if (m->alive && m->pos.block == b && m->pos.offset > s) {
            m->pos.offset = m->pos.offset >= e ? m->pos.offset - (e - s) : s;
        }
    }
}

/* markers in block from at or after off move to block to, shifted by delta */
static void markers_move(pd_doc* d, pd_block_id from, uint32_t off, int strict, pd_block_id to, int64_t delta) {
    int32_t i;

    for (i = 0; i < d->nmarkers; i++) {
        dmarker* m = &d->markers[i];

        if (m->alive && m->pos.block == from && (m->pos.offset > off || (!strict && m->pos.offset == off) ||
                (strict && m->pos.offset == off && m->gravity == PD_GRAVITY_RIGHT))) {
            m->pos.block = to;
            m->pos.offset = (uint32_t)((int64_t)m->pos.offset + delta);
        }
    }
}

/* ------------------------------------------------------------------ */
/* paragraph content editing (no undo bookkeeping here)               */
/* ------------------------------------------------------------------ */

static void runs_coalesce(bstate* s) {
    int32_t i, o = 0;

    for (i = 0; i < s->nruns; i++) {
        if (s->runs[i].end <= s->runs[i].start) {
            continue;
        }

        if (o > 0 && s->runs[o - 1].format == s->runs[i].format && s->runs[o - 1].end == s->runs[i].start) {
            s->runs[o - 1].end = s->runs[i].end;
        } else {
            s->runs[o++] = s->runs[i];
        }
    }

    s->nruns = o;
}

/* split the run containing off so a run boundary falls on off */
static int runs_split(bstate* s, uint32_t off) {
    int32_t i;

    for (i = 0; i < s->nruns; i++) {
        if (s->runs[i].start < off && off < s->runs[i].end) {
            if (grow((void**)&s->runs, &s->caprun, (int64_t)s->nruns + 1, sizeof(pd_run))) {
                return -1;
            }

            memmove(s->runs + i + 1, s->runs + i, (size_t)(s->nruns - i) * sizeof(pd_run));
            s->nruns++;
            s->runs[i].end = off;
            s->runs[i + 1].start = off;
            return 0;
        }
    }

    return 0;
}

static pd_format_id format_at(const bstate* s, uint32_t off, int left) {
    int32_t i;

    if (s->len == 0) {
        return s->empty_format;
    }

    if (left && off > 0) {
        off--;
    } else if (off >= s->len) {
        off = s->len - 1;
    }

    for (i = 0; i < s->nruns; i++) {
        if (s->runs[i].start <= off && off < s->runs[i].end) {
            return s->runs[i].format;
        }
    }

    return 0;
}

static int content_insert(bstate* s, uint32_t off, const char* txt, uint32_t n, pd_format_id f) {
    int32_t i;

    if (!n) {
        return 0;
    }

    if ((uint64_t)s->len + n + 1 > INT32_MAX) {
        return -1;
    }

    if (s->len + n + 1 > s->cap) {
        uint32_t nc = s->cap ? s->cap : 32;
        char* t;

        while (nc < s->len + n + 1) {
            nc *= 2;
        }

        t = (char*)realloc(s->text, nc);

        if (!t) {
            return -1;
        }

        s->text = t;
        s->cap = nc;
    }

    if (runs_split(s, off) || grow((void**)&s->runs, &s->caprun, (int64_t)s->nruns + 1, sizeof(pd_run))) {
        return -1;
    }

    memmove(s->text + off + n, s->text + off, s->len - off);
    memcpy(s->text + off, txt, n);
    s->len += n;
    s->text[s->len] = '\0';

    for (i = 0; i < s->nruns && s->runs[i].start < off; i++) {
    }

    memmove(s->runs + i + 1, s->runs + i, (size_t)(s->nruns - i) * sizeof(pd_run));
    s->nruns++;
    s->runs[i].start = off;
    s->runs[i].end = off + n;
    s->runs[i].format = f;

    for (i = i + 1; i < s->nruns; i++) {
        s->runs[i].start += n;
        s->runs[i].end += n;
    }

    for (i = 0; i < s->ninl; i++) {
        if (s->inl[i].offset >= off) {
            s->inl[i].offset += n;
        }
    }

    runs_coalesce(s);
    return 0;
}

static void content_delete(bstate* s, uint32_t a, uint32_t e) {
    uint32_t n = e - a;
    int32_t i, o = 0;

    if (!n) {
        return;
    }

    if (a == 0 && e == s->len) {
        s->empty_format = format_at(s, 0, 0);
    }

    memmove(s->text + a, s->text + e, s->len - e);
    s->len -= n;
    s->text[s->len] = '\0';

    for (i = 0; i < s->nruns; i++) {
        pd_run r = s->runs[i];

        r.start = r.start <= a ? r.start : (r.start >= e ? r.start - n : a);
        r.end = r.end <= a ? r.end : (r.end >= e ? r.end - n : a);
        s->runs[i] = r;
    }

    runs_coalesce(s);

    for (i = 0; i < s->ninl; i++) {
        if (s->inl[i].offset >= a && s->inl[i].offset < e) {
            free(s->inl[i].source);
            continue;
        }

        s->inl[o] = s->inl[i];

        if (s->inl[o].offset >= e) {
            s->inl[o].offset -= n;
        }

        o++;
    }

    s->ninl = o;
}

/* move bytes [off, len) of src (with runs and inlines) to the end of dst */
static int content_append_tail(bstate* dst, bstate* src, uint32_t off) {
    uint32_t base = dst->len, n = src->len - off;
    int32_t i;

    if (n == 0) {
        return 0;
    }

    if (runs_split(src, off)) {
        return -1;
    }

    for (i = 0; i < src->nruns; i++) {
        if (src->runs[i].start >= off &&
                content_insert(dst, dst->len, src->text + src->runs[i].start, src->runs[i].end - src->runs[i].start,
                               src->runs[i].format)) {
            return -1;
        }
    }

    for (i = 0; i < src->ninl; i++) {
        if (src->inl[i].offset >= off) {
            dinline x = src->inl[i];

            if (grow((void**)&dst->inl, &dst->capinl, (int64_t)dst->ninl + 1, sizeof(dinline))) {
                return -1;
            }

            x.offset = x.offset - off + base;
            src->inl[i].source = NULL;      /* ownership moves */
            dst->inl[dst->ninl++] = x;
        }
    }

    content_delete(src, off, src->len);
    return 0;
}

static int range_map_formats(pd_doc* d, bstate* s, uint32_t a, uint32_t e, int mode, const pd_char_props* cp,
                             uint32_t mask, pd_style_id style) {
    int32_t i;

    if (a >= e) {
        return 0;
    }

    if (runs_split(s, a) || runs_split(s, e)) {
        return -1;
    }

    for (i = 0; i < s->nruns; i++) {
        pd_run* r = &s->runs[i];
        const dformat* f;
        dformat nf;

        if (r->start < a || r->end > e) {
            continue;
        }

        f = format_of(d, r->format);
        nf = *f;

        if (mode == 0) {            /* set overrides */
            cp_apply(&nf.cp, cp);
        } else if (mode == 1) {     /* clear overrides */
            nf.cp.mask &= ~mask;
        } else {                    /* character style */
            nf.style = style;
        }

        r->format = intern(d, nf.style, &nf.cp);
    }

    runs_coalesce(s);
    return 0;
}

/* is anc an ancestor of (or equal to) id */
static int is_within(const pd_doc* d, pd_block_id id, pd_block_id anc) {
    blk* b = pd_doc_blk(d, id);

    for (; b; b = pd_doc_blk(d, b->parent)) {
        if (b->id == anc) {
            return 1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* public operations                                                  */
/* ------------------------------------------------------------------ */

pd_status pd_doc_insert_text(pd_doc* d, pd_pos at, const char* utf8, size_t len, pd_format_id fmt, pd_pos* after) {
    blk* b;
    pd_status st = PD_OK;

    if (!d || (!utf8 && len)) {
        return PD_ERR_ARG;
    }

    b = para_of(d, at.block);

    if (!b || !is_boundary(&b->st, at.offset)) {
        return PD_ERR_RANGE;
    }

    if (len > INT32_MAX / 2 || !pd_doc_utf8_valid(utf8, len, 0)) {
        return PD_ERR_ARG;
    }

    if (fmt == PD_FORMAT_INHERIT) {
        fmt = format_at(&b->st, at.offset, 1);
    } else if (!format_of(d, fmt)) {
        return PD_ERR_ARG;
    }

    if (after) {
        *after = at;
    }

    if (len == 0) {
        return PD_OK;
    }

    if (op_begin(d, "Typing", 1, at.block, at.offset) || snap(d, b) ||
            content_insert(&b->st, at.offset, utf8, (uint32_t)len, fmt)) {
        st = PD_ERR_NOMEM;
    } else {
        markers_insert(d, at.block, at.offset, (uint32_t)len);
        touch(d, PD_CHANGE_TEXT, at.block, 0);

        if (after) {
            after->offset = at.offset + (uint32_t)len;
        }
    }

    op_end(d, st == PD_OK, at.block, at.offset + (uint32_t)len);
    return st;
}

pd_status pd_doc_insert_inline(pd_doc* d, pd_pos at, const pd_inline* obj, pd_pos* after) {
    blk* b;
    dinline x;
    pd_status st = PD_OK;

    if (!d || !obj) {
        return PD_ERR_ARG;
    }

    b = para_of(d, at.block);

    if (!b || !is_boundary(&b->st, at.offset)) {
        return PD_ERR_RANGE;
    }

    if (obj->kind < PD_INLINE_IMAGE || obj->kind > PD_INLINE_USER || obj->width < 0 ||
            (obj->kind == PD_INLINE_IMAGE && (obj->resource < 1 || (int32_t)obj->resource > d->nres)) ||
            (obj->kind == PD_INLINE_FOOTNOTE && (!pd_doc_blk(d, obj->target) ||
                    d->tab[obj->target]->kind != PD_BLOCK_STORY)) ||
            (obj->kind == PD_INLINE_FIELD && (obj->field < PD_FIELD_PAGE || obj->field > PD_FIELD_DATE)) ||
            obj->source_len < 0 || (obj->source_len && !obj->source)) {
        return PD_ERR_ARG;
    }

    memset(&x, 0, sizeof(x));
    x.offset = at.offset;
    x.obj = *obj;
    x.obj.name[sizeof(x.obj.name) - 1] = '\0';

    if (obj->source_len) {
        if (!pd_doc_utf8_valid(obj->source, (size_t)obj->source_len, 0)) {
            return PD_ERR_ARG;
        }

        x.source = (char*)malloc((size_t)obj->source_len + 1);

        if (!x.source) {
            return PD_ERR_NOMEM;
        }

        memcpy(x.source, obj->source, (size_t)obj->source_len);
        x.source[obj->source_len] = '\0';
    }

    x.obj.source = NULL;

    if (op_begin(d, "Insert object", 0, at.block, at.offset) || snap(d, b) ||
            content_insert(&b->st, at.offset, "\xEF\xBF\xBC", 3, format_at(&b->st, at.offset, 1)) ||
            grow((void**)&b->st.inl, &b->st.capinl, (int64_t)b->st.ninl + 1, sizeof(dinline))) {
        free(x.source);
        st = PD_ERR_NOMEM;
    } else {
        int32_t i;

        for (i = 0; i < b->st.ninl && b->st.inl[i].offset < at.offset; i++) {
        }

        memmove(b->st.inl + i + 1, b->st.inl + i, (size_t)(b->st.ninl - i) * sizeof(dinline));
        b->st.inl[i] = x;
        b->st.inl[i].obj.source = x.source;
        b->st.ninl++;
        markers_insert(d, at.block, at.offset, 3);
        touch(d, PD_CHANGE_TEXT, at.block, 0);

        if (after) {
            after->block = at.block;
            after->offset = at.offset + 3;
        }
    }

    op_end(d, 0, at.block, at.offset + 3);
    return st;
}

pd_status pd_doc_split(pd_doc* d, pd_pos at, pd_pos* after) {
    blk* b, *p, *q;
    pd_status st = PD_OK;
    int at_end;

    if (!d) {
        return PD_ERR_ARG;
    }

    b = para_of(d, at.block);

    if (!b || !is_boundary(&b->st, at.offset)) {
        return PD_ERR_RANGE;
    }

    p = d->tab[b->parent];
    at_end = at.offset == b->st.len;

    if (op_begin(d, "Split paragraph", 0, at.block, at.offset) || snap(d, b)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    q = pd_doc_new_blk(d, PD_BLOCK_PARAGRAPH);

    if (!q) {
        st = PD_ERR_NOMEM;
    } else {
        pd_para_props spp;

        q->st.role = b->st.role;
        q->st.level = b->st.level;
        q->st.list_level = b->st.list_level;
        q->st.style = b->st.style;
        q->st.list = b->st.list;
        q->st.pp = b->st.pp;
        q->st.pp.mask &= ~(uint32_t)PD_PP_BREAK_BEFORE;
        q->st.empty_format = format_at(&b->st, at.offset, 1);

        if (at_end) {   /* Enter at the end: the next paragraph takes the next style */
            pd_doc_style_resolve(d, b->st.style, &spp, NULL);

            if (spp.next_style && style_of(d, spp.next_style)) {
                q->st.style = spp.next_style;
                q->st.role = b->st.role == PD_ROLE_HEADING || b->st.role == PD_ROLE_TITLE ? PD_ROLE_BODY : q->st.role;
                q->st.level = q->st.role == PD_ROLE_HEADING ? q->st.level : 0;
            }
        }

        if (content_append_tail(&q->st, &b->st, at.offset) ||
                do_attach(d, q->id, p->id, kid_index(p, b->id) + 1)) {
            st = PD_ERR_NOMEM;
        } else {
            markers_move(d, b->id, at.offset, 1, q->id, -(int64_t)at.offset);
            touch(d, PD_CHANGE_TEXT, b->id, 0);
            touch(d, PD_CHANGE_TEXT, q->id, 0);

            if (after) {
                after->block = q->id;
                after->offset = 0;
            }
        }
    }

    op_end(d, 0, 0, 0);
    return st;
}

pd_status pd_doc_delete(pd_doc* d, pd_range r, pd_pos* after) {
    blk* a, *e, *p;
    pd_status st = PD_OK;
    int32_t ia, ie, k;

    if (!d) {
        return PD_ERR_ARG;
    }

    a = para_of(d, r.start.block);
    e = para_of(d, r.end.block);

    if (!a || !e || !is_boundary(&a->st, r.start.offset) || !is_boundary(&e->st, r.end.offset)) {
        return PD_ERR_RANGE;
    }

    if (a == e) {
        if (r.start.offset > r.end.offset) {
            return PD_ERR_ARG;
        }

        if (after) {
            *after = r.start;
        }

        if (r.start.offset == r.end.offset) {
            return PD_OK;
        }

        if (op_begin(d, "Delete", 0, a->id, 0) || snap(d, a)) {
            op_end(d, 0, 0, 0);
            return PD_ERR_NOMEM;
        }

        content_delete(&a->st, r.start.offset, r.end.offset);
        markers_delete(d, a->id, r.start.offset, r.end.offset);
        touch(d, PD_CHANGE_TEXT, a->id, 0);
        op_end(d, 0, 0, 0);
        return PD_OK;
    }

    if (a->parent != e->parent) {
        return PD_ERR_ARG;
    }

    p = d->tab[a->parent];
    ia = kid_index(p, a->id);
    ie = kid_index(p, e->id);

    if (ia > ie) {
        return PD_ERR_ARG;
    }

    if (op_begin(d, "Delete", 0, a->id, 0) || snap(d, a) || snap(d, e)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    /* markers first, from the offsets as they are now: everything deleted
       collapses to the join, the tail of e lands after it */
    {
        int32_t i;

        markers_delete(d, a->id, r.start.offset, a->st.len);

        for (i = 0; i < d->nmarkers; i++) {
            dmarker* m = &d->markers[i];
            int32_t j;

            if (!m->alive) {
                continue;
            }

            if (m->pos.block == e->id) {
                m->pos.offset = m->pos.offset >= r.end.offset ? r.start.offset + m->pos.offset - r.end.offset :
                                r.start.offset;
                m->pos.block = a->id;
                continue;
            }

            for (j = ia + 1; j < ie; j++) {
                if (is_within(d, m->pos.block, p->kids[j])) {
                    m->pos = r.start;
                    break;
                }
            }
        }
    }

    /* blocks strictly between go away whole, then e's tail joins a */
    for (k = ie - 1; k > ia && st == PD_OK; k--) {
        if (do_detach(d, p->kids[k])) {
            st = PD_ERR_NOMEM;
        }
    }

    if (st == PD_OK) {
        content_delete(&a->st, r.start.offset, a->st.len);

        if (content_append_tail(&a->st, &e->st, r.end.offset) || do_detach(d, e->id)) {
            st = PD_ERR_NOMEM;
        }

        touch(d, PD_CHANGE_TEXT, a->id, 0);
    }

    clamp_markers(d, r.start);

    if (after) {
        *after = r.start;
    }

    op_end(d, 0, 0, 0);
    return st;
}

/* call fn for each paragraph segment in a range, in reading order */
static pd_status for_range(pd_doc* d, pd_range r, int mode, const pd_char_props* cp, uint32_t mask,
                           pd_style_id style, const char* label) {
    blk* a = para_of(d, r.start.block), *e = para_of(d, r.end.block);
    pd_block_id cur;
    pd_status st = PD_OK;

    if (!a || !e || !is_boundary(&a->st, r.start.offset) || !is_boundary(&e->st, r.end.offset)) {
        return PD_ERR_RANGE;
    }

    if (a == e && r.start.offset > r.end.offset) {
        return PD_ERR_ARG;
    }

    if (a != e) {   /* e must follow a in reading order */
        for (cur = a->id; cur && cur != e->id; cur = step_para(d, cur, 1)) {
        }

        if (!cur) {
            return PD_ERR_ARG;
        }
    }

    if (op_begin(d, label, 0, 0, 0)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    for (cur = a->id; cur && st == PD_OK; cur = cur == e->id ? 0 : step_para(d, cur, 1)) {
        blk* b = d->tab[cur];
        uint32_t s0 = cur == a->id ? r.start.offset : 0, e0 = cur == e->id ? r.end.offset : b->st.len;

        if (s0 < e0) {
            if (snap(d, b) || range_map_formats(d, &b->st, s0, e0, mode, cp, mask, style)) {
                st = PD_ERR_NOMEM;
            }

            touch(d, PD_CHANGE_FORMAT, cur, 0);
        }
    }

    op_end(d, 0, 0, 0);
    return st;
}

pd_status pd_doc_set_char_props(pd_doc* d, pd_range r, const pd_char_props* props) {
    pd_char_props cp;

    if (!d || !props) {
        return PD_ERR_ARG;
    }

    cp = *props;
    pd_doc_cp_normalize(&cp);
    return for_range(d, r, 0, &cp, 0, 0, "Format");
}

pd_status pd_doc_clear_char_props(pd_doc* d, pd_range r, uint32_t mask) {
    return d ? for_range(d, r, 1, NULL, mask, 0, "Clear format") : PD_ERR_ARG;
}

pd_status pd_doc_set_char_style(pd_doc* d, pd_range r, pd_style_id style) {
    dstyle* s;

    if (!d) {
        return PD_ERR_ARG;
    }

    s = style_of(d, style);

    if (style && (!s || s->kind != PD_STYLE_CHARACTER)) {
        return PD_ERR_ARG;
    }

    return for_range(d, r, 2, NULL, 0, style, "Character style");
}

/* change one block's state field under undo */
#define BLOCK_OP(label, check, change)                    \
    do {                                                   \
        if (!(check)) {                                    \
            return PD_ERR_ARG;                             \
        }                                                  \
        if (op_begin(d, label, 0, 0, 0) || snap(d, b)) {   \
            op_end(d, 0, 0, 0);                            \
            return PD_ERR_NOMEM;                           \
        }                                                  \
        change;                                            \
        touch(d, PD_CHANGE_FORMAT, b->id, 0);              \
        op_end(d, 0, 0, 0);                                \
        return PD_OK;                                      \
    } while (0)

pd_status pd_doc_set_para_style(pd_doc* d, pd_block_id para, pd_style_id style) {
    blk* b = d ? para_of(d, para) : NULL;
    dstyle* s = d ? style_of(d, style) : NULL;

    BLOCK_OP("Paragraph style", b && (style == 0 || (s && s->kind == PD_STYLE_PARAGRAPH)), b->st.style = style);
}

pd_status pd_doc_set_para_props(pd_doc* d, pd_block_id para, const pd_para_props* props) {
    blk* b = d ? para_of(d, para) : NULL;
    pd_para_props pp;

    memset(&pp, 0, sizeof(pp));

    if (props) {
        pp = *props;
        pd_doc_pp_normalize(&pp);
    }

    BLOCK_OP("Paragraph format", b && props && (!(pp.mask & PD_PP_ALIGN) || (pp.align >= PD_ALIGN_JUSTIFY &&
             pp.align <= PD_ALIGN_CENTER)), b->st.pp = pp);
}

pd_status pd_doc_set_role(pd_doc* d, pd_block_id para, pd_role role, int32_t level) {
    blk* b = d ? para_of(d, para) : NULL;

    BLOCK_OP("Paragraph role", b && role >= PD_ROLE_BODY && role <= PD_ROLE_FIGURE_CONTENT &&
             (role == PD_ROLE_HEADING ? level >= 1 && level <= 6 : level == 0),
             (b->st.role = role, b->st.level = level));
}

pd_status pd_doc_set_list(pd_doc* d, pd_block_id para, pd_list_id list, int32_t level) {
    blk* b = d ? para_of(d, para) : NULL;

    BLOCK_OP("List", b && (list == 0 || ((int32_t)list <= d->nlists && level >= 0 && level < d->lists[list - 1].n)),
             (b->st.list = list, b->st.list_level = list ? level : 0));
}

pd_status pd_doc_set_float_props(pd_doc* d, pd_block_id id, const pd_float_props* fp) {
    blk* b = d ? pd_doc_blk(d, id) : NULL;

    BLOCK_OP("Float", b && b->kind == PD_BLOCK_FLOAT && fp && fp->wrap >= PD_WRAP_NONE && fp->wrap <= PD_WRAP_RIGHT &&
             fp->width >= 0 && fp->width_fraction >= 0 && fp->width_fraction <= 1000 && (fp->placement & 31),
             (b->st.fp = *fp, b->st.fp.sequence[sizeof(b->st.fp.sequence) - 1] = '\0'));
}

static int story_ok(const pd_doc* d, pd_block_id id) {
    blk* b = pd_doc_blk(d, id);
    return id == 0 || (b && b->kind == PD_BLOCK_STORY);
}

pd_status pd_doc_set_section_props(pd_doc* d, pd_block_id id, const pd_section_props* sp) {
    blk* b = d ? pd_doc_blk(d, id) : NULL;

    BLOCK_OP("Page setup", b && b->kind == PD_BLOCK_SECTION && sp && sp->page_width > 0 && sp->page_height > 0 &&
             sp->margin_left >= 0 && sp->margin_right >= 0 && sp->margin_top >= 0 && sp->margin_bottom >= 0 &&
             (int64_t)sp->margin_left + sp->margin_right < sp->page_width &&
             (int64_t)sp->margin_top + sp->margin_bottom < sp->page_height && sp->columns >= 1 && sp->columns <= 16 &&
             story_ok(d, sp->header) && story_ok(d, sp->header_first) && story_ok(d, sp->header_even) &&
             story_ok(d, sp->footer) && story_ok(d, sp->footer_first) && story_ok(d, sp->footer_even),
             b->st.sp = *sp);
}

pd_status pd_doc_set_break(pd_doc* d, pd_block_id id, pd_break_kind kind) {
    blk* b = d ? pd_doc_blk(d, id) : NULL;

    BLOCK_OP("Break", b && b->kind == PD_BLOCK_BREAK && kind >= PD_BREAK_PAGE && kind <= PD_BREAK_EVEN_PAGE,
             b->st.break_kind = kind);
}

pd_status pd_doc_insert_block(pd_doc* d, pd_block_id parent, int32_t index, pd_block_kind kind, pd_block_id* out) {
    blk* p, *c;

    if (!d) {
        return PD_ERR_ARG;
    }

    if (parent == 0) {
        if (kind != PD_BLOCK_STORY) {
            return PD_ERR_ARG;
        }

        parent = PD_STORYROOT_ID;
    } else if (kind == PD_BLOCK_STORY || parent == PD_STORYROOT_ID) {
        return PD_ERR_ARG;
    }

    p = pd_doc_blk(d, parent);

    if (!p || (parent != PD_STORYROOT_ID && !pd_doc_child_allowed(p->kind, kind)) || index > p->nkids) {
        return PD_ERR_ARG;
    }

    if (op_begin(d, "Insert block", 0, 0, 0)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    c = kind == PD_BLOCK_TABLE || kind == PD_BLOCK_ROW ? pd_doc_new_blk(d, kind) : new_container(d, kind);

    if (c && kind == PD_BLOCK_TABLE) {  /* a table starts as 1x1 */
        blk* row = new_container(d, PD_BLOCK_ROW), *cell = new_container(d, PD_BLOCK_CELL);

        if (!row || !cell || pd_doc_add_kid(row, cell->id, -1) || pd_doc_add_kid(c, row->id, -1)) {
            c = NULL;
        } else {
            cell->parent = row->id;
            row->parent = c->id;
        }
    } else if (c && kind == PD_BLOCK_ROW) {
        blk* cell = new_container(d, PD_BLOCK_CELL);

        if (!cell || pd_doc_add_kid(c, cell->id, -1)) {
            c = NULL;
        } else {
            cell->parent = c->id;
        }
    }

    if (c && kind == PD_BLOCK_PARAGRAPH) {
        c->st.style = pd_doc_style_find(d, "Normal");
    }

    if (!c || do_attach(d, c->id, parent, index)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    if (out) {
        *out = c->id;
    }

    op_end(d, 0, 0, 0);
    return PD_OK;
}

/* where markers inside a block about to leave should go */
static pd_pos evacuate(const pd_doc* d, pd_block_id id) {
    pd_pos p;
    pd_block_id first = first_para_in(d, id, 0), last = first_para_in(d, id, 1);

    p.offset = 0;
    p.block = last ? step_para(d, last, 1) : 0;

    if (!p.block && first) {
        p.block = step_para(d, first, -1);

        if (p.block) {
            p.offset = d->tab[p.block]->st.len;
        }
    }

    return p;
}

static void move_markers_out(pd_doc* d, pd_block_id id, pd_pos to) {
    int32_t i;

    for (i = 0; i < d->nmarkers; i++) {
        if (d->markers[i].alive && is_within(d, d->markers[i].pos.block, id) && to.block) {
            d->markers[i].pos = to;
        }
    }
}

/* is a story used by a section or a footnote in a live paragraph */
static int story_referenced(const pd_doc* d, pd_block_id id) {
    uint32_t k;
    int32_t i;

    for (k = 1; k < d->next_id && k < d->captab; k++) {
        blk* b = d->tab[k];

        if (!b || !b->alive) {
            continue;
        }

        if (b->kind == PD_BLOCK_SECTION) {
            const pd_section_props* sp = &b->st.sp;

            if (sp->header == id || sp->header_first == id || sp->header_even == id || sp->footer == id ||
                    sp->footer_first == id || sp->footer_even == id) {
                return 1;
            }
        }

        for (i = 0; i < b->st.ninl; i++) {
            if (b->st.inl[i].obj.kind == PD_INLINE_FOOTNOTE && b->st.inl[i].obj.target == id) {
                return 1;
            }
        }
    }

    return 0;
}

pd_status pd_doc_remove_block(pd_doc* d, pd_block_id id) {
    blk* b, *p;
    pd_pos to;

    if (!d) {
        return PD_ERR_ARG;
    }

    b = pd_doc_blk(d, id);

    if (!b || b->kind == PD_BLOCK_ROOT) {
        return PD_ERR_ARG;
    }

    p = d->tab[b->parent];

    if (p->nkids <= 1 && p->id != PD_STORYROOT_ID) {
        return PD_ERR_STATE;    /* containers keep at least one child */
    }

    if (b->kind == PD_BLOCK_STORY && story_referenced(d, id)) {
        return PD_ERR_STATE;    /* still a header, footer or footnote */
    }

    to = evacuate(d, id);

    if (op_begin(d, "Remove block", 0, 0, 0) || do_detach(d, id)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    move_markers_out(d, id, to);
    clamp_markers(d, to);
    op_end(d, 0, 0, 0);
    return PD_OK;
}

pd_status pd_doc_move_block(pd_doc* d, pd_block_id id, pd_block_id np, int32_t index) {
    blk* b, *p, *q;

    if (!d) {
        return PD_ERR_ARG;
    }

    b = pd_doc_blk(d, id);
    q = pd_doc_blk(d, np);

    if (!b || !q || b->kind == PD_BLOCK_ROOT || b->kind == PD_BLOCK_STORY || np == PD_STORYROOT_ID ||
            !pd_doc_child_allowed(q->kind, b->kind) || is_within(d, np, id)) {
        return PD_ERR_ARG;
    }

    p = d->tab[b->parent];

    if (p->nkids <= 1 && p != q) {
        return PD_ERR_STATE;
    }

    if (index > q->nkids - (p == q ? 1 : 0)) {
        return PD_ERR_ARG;
    }

    if (op_begin(d, "Move block", 0, 0, 0) || do_detach(d, id) || do_attach(d, id, np, index)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    op_end(d, 0, 0, 0);
    return PD_OK;
}

pd_status pd_doc_style_define(pd_doc* d, const char* name, pd_style_kind kind, pd_style_id parent,
                              const pd_para_props* pp, const pd_char_props* cp, pd_style_id* out) {
    pd_style_id id, a;
    dstyle ns, *ps;
    urec* r;
    int depth;

    if (!d || !name || !name[0] || strlen(name) >= sizeof(ns.name) || (kind != PD_STYLE_PARAGRAPH &&
            kind != PD_STYLE_CHARACTER) || !pd_doc_utf8_valid(name, strlen(name), 0)) {
        return PD_ERR_ARG;
    }

    ps = style_of(d, parent);

    if (parent && (!ps || ps->kind != (int32_t)kind)) {
        return PD_ERR_ARG;
    }

    id = pd_doc_style_find(d, name);

    if (id && d->styles[id - 1].kind != (int32_t)kind) {
        return PD_ERR_ARG;
    }

    for (a = parent, depth = 0; a && depth < 1000; a = d->styles[a - 1].parent, depth++) {
        if (a == id && id) {
            return PD_ERR_ARG;     /* would make a cycle */
        }
    }

    memset(&ns, 0, sizeof(ns));
    strcpy(ns.name, name);
    ns.kind = kind;
    ns.parent = parent;

    if (pp && kind == PD_STYLE_PARAGRAPH) {
        ns.pp = *pp;
        pd_doc_pp_normalize(&ns.pp);

        if ((ns.pp.mask & PD_PP_NEXT_STYLE) && ns.pp.next_style && !style_of(d, ns.pp.next_style)) {
            return PD_ERR_ARG;
        }
    }

    if (cp) {
        ns.cp = *cp;
        pd_doc_cp_normalize(&ns.cp);
    }

    ns.alive = 1;

    if (op_begin(d, "Define style", 0, 0, 0)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    if (!id) {
        dstyle dead = ns;

        dead.alive = 0;

        if (add_style_raw(d, name, kind, 0, NULL, NULL, &id)) {
            op_end(d, 0, 0, 0);
            return PD_ERR_NOMEM;
        }

        d->styles[id - 1] = dead;
    }

    r = add_rec(d, UR_STYLE);

    if (!r) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    r->style = id;
    r->sdef = d->styles[id - 1];
    d->styles[id - 1] = ns;
    touch(d, PD_CHANGE_STYLE, 0, id);

    if (out) {
        *out = id;
    }

    op_end(d, 0, 0, 0);
    return PD_OK;
}

/* ------------------------------------------------------------------ */
/* undo / redo                                                        */
/* ------------------------------------------------------------------ */

void pd_doc_begin_group(pd_doc* d, const char* label) {
    if (!d) {
        return;
    }

    if (d->group_depth++ == 0) {
        d->typing_open = 0;
        d->cur = NULL;
        memset(d->group_label, 0, sizeof(d->group_label));

        if (label) {
            strncpy(d->group_label, label, sizeof(d->group_label) - 1);
        }
    }
}

void pd_doc_end_group(pd_doc* d) {
    if (!d || d->group_depth == 0) {
        return;
    }

    if (--d->group_depth == 0) {
        if (d->cur && d->cur->n == 0 && d->nundo > 0 && d->cur == &d->undo[d->nundo - 1]) {
            free(d->cur->r);
            d->nundo--;
        }

        d->cur = NULL;
        d->typing_open = 0;
        enforce_limit(d);
    }
}

void pd_doc_seal_undo(pd_doc* d) {
    if (d) {
        d->typing_open = 0;
    }
}

static pd_status replay(pd_doc* d, ustep* from, int32_t* nfrom, ustep** to, int32_t* nto, int32_t* capto, int dir) {
    ustep s;
    int32_t i;

    if (!d || d->group_depth > 0 || *nfrom == 0) {
        return PD_ERR_STATE;
    }

    if (grow((void**)to, capto, (int64_t)*nto + 1, sizeof(ustep))) {
        return PD_ERR_NOMEM;
    }

    s = from[--*nfrom];
    d->revision++;
    d->ntouched = 0;

    for (i = 0; i < s.n; i++) {
        toggle(d, &s.r[dir < 0 ? s.n - 1 - i : i]);
    }

    (*to)[(*nto)++] = s;
    d->typing_open = 0;
    {
        pd_pos none;

        none.block = 0;
        none.offset = 0;
        clamp_markers(d, none);
    }
    notify(d);
    return PD_OK;
}

pd_status pd_doc_undo(pd_doc* d) {
    return d ? replay(d, d->undo, &d->nundo, &d->redo, &d->nredo, &d->capredo, -1) : PD_ERR_ARG;
}

pd_status pd_doc_redo(pd_doc* d) {
    return d ? replay(d, d->redo, &d->nredo, &d->undo, &d->nundo, &d->capundo, 1) : PD_ERR_ARG;
}

int32_t pd_doc_can_undo(const pd_doc* d) {
    return d && d->nundo > 0 && d->group_depth == 0;
}

int32_t pd_doc_can_redo(const pd_doc* d) {
    return d && d->nredo > 0 && d->group_depth == 0;
}

const char* pd_doc_undo_label(const pd_doc* d) {
    return (d && d->nundo > 0) ? d->undo[d->nundo - 1].label : NULL;
}

void pd_doc_set_undo_limit(pd_doc* d, int32_t steps) {
    if (d && steps >= 0) {
        d->undo_limit = steps;
        enforce_limit(d);
    }
}
