/*
 * Parade document model: tree, styles, formats, lists, operations, undo
 *
 * Undo is built from three symmetric records. Toggling a record twice
 * restores the original, so undo walks a step's records backwards and
 * redo walks them forwards with the same code:
 *   UR_STATE  swaps a block's content state with a saved copy
 *   UR_ATTACH attaches or detaches a whole subtree (insert, remove, move)
 *   UR_STYLE  swaps a style definition
 *   UR_COMMENT swaps a comment
 * Before an operation first changes a block in a step, the block's state
 * is saved once; later changes in the same step (typing) need nothing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

    if (m & PD_CP_CAPS) {
        z.caps = cp->caps;
    }

    if (m & PD_CP_HIDDEN) {
        z.hidden = cp->hidden;
    }

    if (m & PD_CP_POSITION) {
        z.position = cp->position;
    }

    if (m & PD_CP_REVISION) {
        z.revision = cp->revision;
    }

    if (m & PD_CP_FAMILY_EA) {
        memcpy(z.family_ea, cp->family_ea, sizeof(z.family_ea));
        z.family_ea[sizeof(z.family_ea) - 1] = '\0';
        memset(z.family_ea + strlen(z.family_ea), 0, sizeof(z.family_ea) - strlen(z.family_ea));
    }

    if (m & PD_CP_FAMILY_CS) {
        memcpy(z.family_cs, cp->family_cs, sizeof(z.family_cs));
        z.family_cs[sizeof(z.family_cs) - 1] = '\0';
        memset(z.family_cs + strlen(z.family_cs), 0, sizeof(z.family_cs) - strlen(z.family_cs));
    }

    if (m & PD_CP_SIZE_CS) {
        z.size_cs = cp->size_cs;
    }

    if (m & PD_CP_WEIGHT_CS) {
        z.weight_cs = cp->weight_cs;
    }

    z.italic_cs = m & PD_CP_ITALIC_CS ? cp->italic_cs : -1;     /* unset: as the text */
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
        z.border_sides = pp->border_sides & 31;
        z.border_space = pp->border_space;
    }

    KEEP(PD_PP_SHADING, shading);
    KEEP(PD_PP_DIRECTION, direction);
    KEEP(PD_PP_CONTEXTUAL, contextual);
    KEEP(PD_PP_SNAP_GRID, snap_grid);

    if (m & PD_PP_TABS) {
        int32_t i;

        z.ntabs = pp->ntabs < 0 ? 0 : pp->ntabs > PD_MAX_TABS ? PD_MAX_TABS : pp->ntabs;
        z.tab_interval = pp->tab_interval;

        for (i = 0; i < z.ntabs; i++) {
            z.tabs[i] = pp->tabs[i];
        }
    }
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

    if (m & PD_CP_CAPS) {
        dst->caps = src->caps;
    }

    if (m & PD_CP_HIDDEN) {
        dst->hidden = src->hidden;
    }

    if (m & PD_CP_POSITION) {
        dst->position = src->position;
    }

    if (m & PD_CP_REVISION) {
        dst->revision = src->revision;
    }

    if (m & PD_CP_FAMILY_EA) {
        memcpy(dst->family_ea, src->family_ea, sizeof(dst->family_ea));
    }

    if (m & PD_CP_FAMILY_CS) {
        memcpy(dst->family_cs, src->family_cs, sizeof(dst->family_cs));
    }

    if (m & PD_CP_SIZE_CS) {
        dst->size_cs = src->size_cs;
    }

    if (m & PD_CP_WEIGHT_CS) {
        dst->weight_cs = src->weight_cs;
    }

    if (m & PD_CP_ITALIC_CS) {
        dst->italic_cs = src->italic_cs;
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
        dst->border_sides = src->border_sides;
        dst->border_space = src->border_space;
    }

    SET(PD_PP_SHADING, shading);
    SET(PD_PP_DIRECTION, direction);
    SET(PD_PP_CONTEXTUAL, contextual);
    SET(PD_PP_SNAP_GRID, snap_grid);

    if (m & PD_PP_TABS) {
        dst->ntabs = src->ntabs;
        memcpy(dst->tabs, src->tabs, sizeof(dst->tabs));
        dst->tab_interval = src->tab_interval;
    }
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
        pp->snap_grid = 1;
        pp->break_mode = PD_BREAK_OPTIMAL;
    }

    if (cp) {
        memset(cp, 0, sizeof(*cp));
        cp->mask = PD_CP_ALL;
        cp->size = PD_PT(10);
        cp->weight = 400;
        cp->color = 0xFF000000u;
        cp->kerning = 1;
        cp->italic_cs = -1;
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
    } else if (kind == PD_BLOCK_TABLE) {
        pd_table_props_init(&s->tp);
    } else if (kind == PD_BLOCK_CELL) {
        s->cell.col_span = 1;
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
                size_t n = pd_inl_bytes(&src->inl[i].obj);

                dst->inl[i].source = (char*)malloc(n);

                if (!dst->inl[i].source) {
                    return -1;
                }

                memcpy(dst->inl[i].source, src->inl[i].source, n);
            }

            pd_inl_point(&dst->inl[i]);
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

        case PD_BLOCK_STORY:    /* a header's logo beside its text */
            return kk == PD_BLOCK_PARAGRAPH || kk == PD_BLOCK_TABLE || kk == PD_BLOCK_FLOAT;

        case PD_BLOCK_FLOAT:
        case PD_BLOCK_CELL:
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
        d->stable_breaks = 1;
    }

    return d;
}

static void step_free(pd_doc* d, ustep* s, int dropping) {
    int32_t i;

    for (i = 0; i < s->n; i++) {
        urec* r = &s->r[i];

        if (r->type == UR_STATE) {
            bstate_free(&r->saved);
        } else if (r->type == UR_COMMENT) {
            free(r->csave.text);
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

    for (i = 0; i < d->ncomments; i++) {
        free(d->comments[i].text);
    }

    free(d->comments);
    free(d->revs);
    free(d->dnew);
    free(d->meta);
    free(d->tab);
    free(d->styles);
    free(d->formats);
    free(d->lists);
    free(d->res);
    free(d->markers);
    free(d->undo);
    free(d->redo);
    free(d->touched);
    free(d->fallback);
    free(d->hyphs);
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
        pp.mask = PD_PP_SPACE_BEFORE | PD_PP_SPACE_AFTER | PD_PP_KEEP_NEXT | PD_PP_NEXT_STYLE | PD_PP_HYPHENATE |
                  PD_PP_ALIGN | PD_PP_INDENT_FIRST;
        pp.align = PD_ALIGN_LEFT;
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
    pp.mask = PD_PP_ALIGN | PD_PP_SPACE_AFTER | PD_PP_NEXT_STYLE | PD_PP_HYPHENATE | PD_PP_INDENT_FIRST;
    pp.align = PD_ALIGN_CENTER;
    pp.space_after = PD_PT(18);
    pp.next_style = normal;
    cp.mask = PD_CP_SIZE | PD_CP_WEIGHT;
    cp.size = PD_PT(24);
    cp.weight = 700;
    add_style_raw(d, "Title", PD_STYLE_PARAGRAPH, normal, &pp, &cp, NULL);

    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    pp.mask = PD_PP_SPACE_BEFORE | PD_PP_SPACE_AFTER | PD_PP_ALIGN | PD_PP_INDENT_FIRST;
    pp.align = PD_ALIGN_CENTER;
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
    pp.mask = PD_PP_ALIGN | PD_PP_HYPHENATE | PD_PP_BREAK_MODE | PD_PP_INDENT_FIRST;
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

    /* a definition list: the term in bold, its definition indented below it */
    memset(&pp, 0, sizeof(pp));
    pp.mask = PD_PP_KEEP_NEXT | PD_PP_SPACE_AFTER;
    pp.keep_with_next = 1;
    add_style_raw(d, "Term", PD_STYLE_PARAGRAPH, normal, &pp, &cp, NULL);
    memset(&pp, 0, sizeof(pp));
    memset(&cp, 0, sizeof(cp));
    pp.mask = PD_PP_INDENT_LEFT;
    pp.indent_left = PD_PT(36);
    add_style_raw(d, "Definition", PD_STYLE_PARAGRAPH, normal, &pp, NULL, NULL);
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

void pd_doc_set_math_font(pd_doc* d, const pd_font* font) {
    if (d) {
        d->math_font = font;
        d->style_rev++;
    }
}

pd_status pd_doc_set_microtype(pd_doc* d, int32_t protrusion, int32_t expansion) {
    if (!d || protrusion < 0 || protrusion > 1 || expansion < 0 || expansion > 100) {
        return PD_ERR_ARG;
    }

    d->protrusion = protrusion;
    d->expansion = expansion;
    d->style_rev++;
    return PD_OK;
}

void pd_doc_set_stable_breaks(pd_doc* d, int32_t on) {
    if (d) {
        d->stable_breaks = on != 0;
    }
}

int32_t pd_doc_stable_breaks(const pd_doc* d) {
    return d ? d->stable_breaks : 0;
}

pd_status pd_doc_set_hyphenator(pd_doc* d, const char* lang, const pd_hyph* hyph) {
    int32_t i;

    if (!d || !lang || strlen(lang) >= sizeof(d->hyphs[0].lang)) {
        return PD_ERR_ARG;
    }

    for (i = 0; i < d->nhyphs && strcmp(d->hyphs[i].lang, lang); i++) {
    }

    if (!hyph) {
        if (i < d->nhyphs) {
            d->hyphs[i] = d->hyphs[--d->nhyphs];
        }
    } else {
        if (i == d->nhyphs) {
            void* p = realloc(d->hyphs, (size_t)(d->nhyphs + 1) * sizeof(d->hyphs[0]));

            if (!p) {
                return PD_ERR_NOMEM;
            }

            d->hyphs = p;
            strcpy(d->hyphs[d->nhyphs++].lang, lang);
        }

        d->hyphs[i].hyph = hyph;
    }

    d->style_rev++;
    return PD_OK;
}

pd_status pd_doc_set_fallback_fonts(pd_doc* d, const pd_font* const* fonts, int32_t n) {
    const pd_font** f = NULL;

    if (!d || n < 0 || (n > 0 && !fonts)) {
        return PD_ERR_ARG;
    }

    if (n > 0) {
        f = (const pd_font**)malloc((size_t)n * sizeof(pd_font*));

        if (!f) {
            return PD_ERR_NOMEM;
        }

        memcpy(f, fonts, (size_t)n * sizeof(pd_font*));
    }

    free(d->fallback);
    d->fallback = f;
    d->nfallback = n;
    d->style_rev++;     /* every paragraph may lay out differently now */
    return PD_OK;
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

/* the first (dir 1) or last (dir -1) paragraph under a block, depth first */
static pd_block_id edge_para(const pd_doc* d, pd_block_id id, int dir, int depth) {
    blk* b = pd_doc_blk(d, id);
    int32_t i;

    if (!b || depth > 64) {
        return 0;
    }

    if (b->kind == PD_BLOCK_PARAGRAPH) {
        return id;
    }

    for (i = 0; i < b->nkids; i++) {
        pd_block_id p = edge_para(d, b->kids[dir > 0 ? i : b->nkids - 1 - i], dir, depth + 1);

        if (p) {
            return p;
        }
    }

    return 0;
}

pd_block_id pd_doc_next_paragraph(const pd_doc* d, pd_block_id id) {
    if (d && id == 0) {
        return edge_para(d, PD_ROOT_ID, 1, 0);
    }

    return para_of(d, id) ? step_para(d, id, 1) : 0;
}

pd_block_id pd_doc_prev_paragraph(const pd_doc* d, pd_block_id id) {
    if (d && id == 0) {
        return edge_para(d, PD_ROOT_ID, -1, 0);
    }

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

pd_status pd_doc_format_info(const pd_doc* d, pd_format_id fmt, pd_style_id* style, pd_char_props* overrides) {
    const dformat* f = d ? format_of(d, fmt) : NULL;

    if (!f) {
        return PD_ERR_ARG;
    }

    if (style) {
        *style = f->style;
    }

    if (overrides) {
        *overrides = f->cp;
    }

    return PD_OK;
}

pd_status pd_doc_style_info(const pd_doc* d, pd_style_id style, int32_t* kind, pd_style_id* parent,
                            pd_para_props* para, pd_char_props* chr) {
    dstyle* s = d ? style_of(d, style) : NULL;

    if (!s) {
        return PD_ERR_ARG;
    }

    if (kind) {
        *kind = s->kind;
    }

    if (parent) {
        *parent = s->parent;
    }

    if (para) {
        *para = s->pp;
    }

    if (chr) {
        *chr = s->cp;
    }

    return PD_OK;
}

pd_status pd_doc_para_props(const pd_doc* d, pd_block_id para, pd_para_props* out) {
    blk* b = d ? para_of(d, para) : NULL;

    if (!b || !out) {
        return PD_ERR_ARG;
    }

    *out = b->st.pp;
    return PD_OK;
}

int32_t pd_doc_para_rtl(const pd_doc* d, pd_block_id para) {
    blk* b = d ? para_of(d, para) : NULL;
    pd_para_props pp;

    if (!b) {
        return 0;
    }

    pd_doc_effective_pp(d, b, &pp, NULL);

    if (pp.direction == PD_DIR_RTL || (pp.direction == PD_DIR_AUTO && b->st.text &&
                                       pd_bidi_para_rtl(b->st.text, b->st.len))) {
        return 1;
    }

    return b->st.text && pd_bidi_maybe_rtl(b->st.text, b->st.len, 0) ? 2 : 0;
}

int32_t pd_doc_list_count(const pd_doc* d) {
    return d ? d->nlists : 0;
}

pd_status pd_doc_list_info(const pd_doc* d, pd_list_id list, int32_t* nlevels, pd_list_level* levels) {
    const dlist* l;

    if (!d || list < 1 || (int32_t)list > d->nlists) {
        return PD_ERR_ARG;
    }

    l = &d->lists[list - 1];

    if (nlevels) {
        *nlevels = l->n;
    }

    if (levels) {
        memcpy(levels, l->lv, (size_t)l->n * sizeof(pd_list_level));
    }

    return PD_OK;
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
        l->lv[i].label_family[sizeof(l->lv[i].label_family) - 1] = '\0';
    }

    d->nlists++;
    *out = (pd_list_id)d->nlists;
    return PD_OK;
}

void pd_doc_format_number(int32_t v, int32_t fmt, char* buf, size_t cap) {
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

        if (c->st.list != b->st.list || (c->st.at.cont && cur != para)) {
            continue;   /* a later block of an item is not an item */
        }

        if (cur == para && c->st.at.cont) {
            return PD_OK;   /* and has no label */
        }

        lv = lv < l->n ? lv : l->n - 1;
        cnt[lv] = seen[lv] ? cnt[lv] + 1 : l->lv[lv].start;
        seen[lv] = 1;

        for (k = lv + 1; k < 9; k++) {      /* deeper levels count again, as each says */
            int32_t ra = k < l->n ? l->lv[k].restart_after : 0;

            if (ra == 0 || (ra > 0 && lv < ra)) {
                seen[k] = 0;
            }
        }

        if (cur == para) {
            const pd_list_level* L = &l->lv[lv];

            if (L->format == PD_NUM_BULLET) {
                strncpy(buf, L->text, (size_t)cap - 1);
                buf[cap - 1] = '\0';
                pd_doc_task_label(b->st.at.task, 1, buf, (size_t)cap);
                return PD_OK;
            }

            for (t = L->text; *t && o + 1 < (size_t)cap; t++) {
                if (t[0] == '%' && t[1] >= '1' && t[1] <= '9') {
                    char num[32];
                    int32_t at = t[1] - '1';
                    size_t q;

                    pd_doc_format_number(at < l->n ? cnt[at] : 0, at < l->n ? l->lv[at].format : PD_NUM_DECIMAL, num,
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
            pd_doc_task_label(b->st.at.task, 0, buf, (size_t)cap);
            return PD_OK;
        }
    }

    return PD_OK;
}

void pd_doc_task_label(int32_t task, int bullet, char* buf, size_t cap) {
    const char* box = task == 2 ? "\xE2\x98\x91" : "\xE2\x98\x90";    /* U+2611 checked, U+2610 open */
    size_t n = strlen(buf);

    if (!task || cap < 5) {
        return;
    }

    if (bullet || n + 5 > cap) {    /* a checklist's box stands for its bullet */
        snprintf(buf, cap, "%s", box);
    } else {                        /* and follows its number */
        snprintf(buf + n, cap - n, " %s", box);
    }
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
    out->source = b->st.inl[i].obj.source;
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
    sp->footnote_skip = PD_PT(12);
}

void pd_table_props_init(pd_table_props* tp) {
    if (tp) {
        memset(tp, 0, sizeof(*tp));
        tp->align = PD_ALIGN_LEFT;
        tp->cell_padding = PD_PT(4);
        tp->cell_padding_v = -1;
        tp->border = PD_PT(0.4);
        tp->border_color = 0xFF000000u;
    }
}

pd_status pd_doc_table_props(const pd_doc* d, pd_block_id id, pd_table_props* out) {
    blk* b = pd_doc_blk(d, id);

    if (!b || b->kind != PD_BLOCK_TABLE || !out) {
        return PD_ERR_ARG;
    }

    *out = b->st.tp;
    return PD_OK;
}

pd_status pd_doc_cell_props(const pd_doc* d, pd_block_id id, pd_cell_props* out) {
    blk* b = pd_doc_blk(d, id);

    if (!b || b->kind != PD_BLOCK_CELL || !out) {
        return PD_ERR_ARG;
    }

    *out = b->st.cell;
    return PD_OK;
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

    if (d->delta_fn && !d->applying) {
        pd_doc_delta_emit(d);
    }

    if (d->sync_fn) {
        d->sync_fn(d->sync_user);
    }

    d->ndnew = 0;

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
/* a subtree attached in this operation: a delta carries it whole */
static void delta_new(pd_doc* d, pd_block_id id) {
    if (d->delta_fn && !grow((void**)&d->dnew, &d->capdnew, (int64_t)d->ndnew + 1, sizeof(pd_block_id))) {
        d->dnew[d->ndnew++] = id;
    }
}

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
    delta_new(d, id);
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
            delta_new(d, r->id);
            r->attached = 1;
        }

        touch(d, PD_CHANGE_STRUCTURE, r->parent, 0);
    } else if (r->type == UR_STYLE) {
        dstyle t = d->styles[r->style - 1];

        d->styles[r->style - 1] = r->sdef;
        r->sdef = t;
        d->style_rev++;
        touch(d, PD_CHANGE_STYLE, 0, r->style);
    } else if (r->type == UR_COMMENT) {
        dcomment t = d->comments[r->comment - 1];

        d->comments[r->comment - 1] = r->csave;
        r->csave = t;
        d->comment_rev++;
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
        } else if (mode == 3) {     /* one exact format, given in style */
            r->format = (pd_format_id)style;
            continue;
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

static pd_format_id with_revision(pd_doc* d, pd_format_id fmt, pd_rev_id rev);
static pd_rev_id track_rev(pd_doc* d, int32_t kind);
static pd_status track_delete(pd_doc* d, pd_range r, pd_pos* after);

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

    if (fmt == PD_FORMAT_INHERIT) {     /* typing continues the format, not a neighbour's tracked change */
        fmt = with_revision(d, format_at(&b->st, at.offset, 1), track_rev(d, PD_REV_INSERT));
    } else if (!format_of(d, fmt)) {
        return PD_ERR_ARG;
    } else if (d->track[0]) {
        fmt = with_revision(d, fmt, track_rev(d, PD_REV_INSERT));
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

    if (obj->kind < PD_INLINE_IMAGE || obj->kind > PD_INLINE_RUBY || obj->width < 0 ||
            (obj->kind == PD_INLINE_IMAGE && (obj->resource < (obj->source_len > 0 ? 0u : 1u) ||
                    (int32_t)obj->resource > d->nres)) ||    /* embedded, or by address */
            obj->title_len < 0 || (obj->title_len && !obj->title) || obj->alt_len < 0 || (obj->alt_len && !obj->alt) ||
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

    if (obj->source_len || obj->title_len || obj->alt_len) {
        char* q;

        if ((obj->source_len && !pd_doc_utf8_valid(obj->source, (size_t)obj->source_len, 0)) ||
                (obj->title_len && !pd_doc_utf8_valid(obj->title, (size_t)obj->title_len, 0)) ||
                (obj->alt_len && !pd_doc_utf8_valid(obj->alt, (size_t)obj->alt_len, 0))) {
            return PD_ERR_ARG;
        }

        x.source = q = (char*)malloc(pd_inl_bytes(obj));

        if (!x.source) {
            return PD_ERR_NOMEM;
        }

        memcpy(q, obj->source ? obj->source : "", (size_t)obj->source_len);
        q += obj->source_len;
        *q++ = '\0';
        memcpy(q, obj->title ? obj->title : "", (size_t)obj->title_len);
        q += obj->title_len;
        *q++ = '\0';
        memcpy(q, obj->alt ? obj->alt : "", (size_t)obj->alt_len);
        q[obj->alt_len] = '\0';
    }

    pd_inl_point(&x);

    if (op_begin(d, "Insert object", 0, at.block, at.offset) || snap(d, b) ||
            content_insert(&b->st, at.offset, "\xEF\xBF\xBC", 3,
                           with_revision(d, format_at(&b->st, at.offset, 1), track_rev(d, PD_REV_INSERT))) ||
            grow((void**)&b->st.inl, &b->st.capinl, (int64_t)b->st.ninl + 1, sizeof(dinline))) {
        free(x.source);
        st = PD_ERR_NOMEM;
    } else {
        int32_t i;

        for (i = 0; i < b->st.ninl && b->st.inl[i].offset < at.offset; i++) {
        }

        memmove(b->st.inl + i + 1, b->st.inl + i, (size_t)(b->st.ninl - i) * sizeof(dinline));
        b->st.inl[i] = x;
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
        q->st.at = b->st.at;    /* in the same quote, code block, list */
        q->st.at.task = q->st.at.task ? 1 : 0;  /* a new task is not done yet */
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

    if (d->track[0]) {
        return track_delete(d, r, after);
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

/* an empty paragraph's format, from a file (its paragraph mark's): what its line is sized by */
void pd_doc_set_mark_format(pd_doc* d, pd_block_id para, pd_format_id fmt) {
    blk* b = para_of(d, para);

    if (b && b->st.len == 0 && (!fmt || format_of(d, fmt))) {
        b->st.empty_format = fmt;
    }
}

pd_status pd_doc_set_format(pd_doc* d, pd_range r, pd_format_id fmt) {
    return d && format_of(d, fmt) ? for_range(d, r, 3, NULL, 0, (pd_style_id)fmt, "Format") : PD_ERR_ARG;
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

int pd_doc_tabs_ok(const pd_para_props* pp) {
    int32_t i;

    if (!(pp->mask & PD_PP_TABS)) {
        return 1;
    }

    if (pp->ntabs < 0 || pp->ntabs > PD_MAX_TABS || pp->tab_interval < 0) {
        return 0;
    }

    for (i = 0; i < pp->ntabs; i++) {
        if (pp->tabs[i].align < PD_TAB_LEFT || pp->tabs[i].align > PD_TAB_DECIMAL ||
                pp->tabs[i].leader < PD_LEADER_NONE || pp->tabs[i].leader > PD_LEADER_UNDERSCORE) {
            return 0;
        }
    }

    return 1;
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
             pp.align <= PD_ALIGN_CENTER)) && pd_doc_tabs_ok(&pp), b->st.pp = pp);
}

pd_status pd_doc_set_role(pd_doc* d, pd_block_id para, pd_role role, int32_t level) {
    blk* b = d ? para_of(d, para) : NULL;

    BLOCK_OP("Paragraph role", b && role >= PD_ROLE_BODY && role <= PD_ROLE_DEFINITION &&
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

    BLOCK_OP("Float", b && b->kind == PD_BLOCK_FLOAT && fp && fp->wrap >= PD_WRAP_NONE && fp->wrap <= PD_WRAP_BEHIND &&
             fp->width >= 0 && fp->width_fraction >= 0 && fp->width_fraction <= 1000 && (fp->placement & 31) &&
             fp->placement <= 63,
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
             story_ok(d, sp->footer) && story_ok(d, sp->footer_first) && story_ok(d, sp->footer_even) &&
             (sp->continuous == 0 || sp->continuous == 1) && sp->page_breaking >= PD_PAGES_GREEDY &&
             sp->page_breaking <= PD_PAGES_OPTIMAL && sp->footnote_skip >= 0 && sp->line_numbers >= 0 &&
             sp->line_numbers <= 100 && sp->line_number_start >= 0 && sp->line_number_distance >= 0 &&
             sp->line_number_restart >= PD_LINENUM_PAGE && sp->line_number_restart <= PD_LINENUM_CONTINUOUS &&
             sp->direction >= PD_DIR_AUTO && sp->direction <= PD_DIR_RTL &&
             sp->mirror_margins >= -1 && sp->mirror_margins <= 1 && sp->gutter >= 0 &&
             (int64_t)sp->margin_left + sp->margin_right + sp->gutter < sp->page_width && sp->page_valign >= 0 &&
             sp->page_valign <= 2,
             b->st.sp = *sp);
}

int pd_doc_table_props_ok(const pd_table_props* tp) {
    int32_t i;

    if (!tp || tp->width < 0 || tp->align < PD_ALIGN_JUSTIFY || tp->align > PD_ALIGN_CENTER || tp->header_rows < 0 ||
            tp->header_rows > 1000 || tp->cell_padding < 0 || tp->border < 0 || tp->ncols < 0 ||
            tp->ncols > PD_TABLE_MAX_COLS || tp->indent < -PD_PT(10000) || tp->indent > PD_PT(10000) ||
            tp->width_pct < 0 || tp->width_pct > 1000 || tp->border_sides < 0 || tp->border_sides > 63 ||
            tp->direction < PD_DIR_AUTO || tp->direction > PD_DIR_RTL) {
        return 0;
    }

    for (i = 0; i < tp->ncols; i++) {
        if (tp->col_width[i] < 0) {
            return 0;
        }
    }

    return 1;
}

pd_status pd_doc_set_table_props(pd_doc* d, pd_block_id id, const pd_table_props* tp) {
    blk* b = d ? pd_doc_blk(d, id) : NULL;

    BLOCK_OP("Table", b && b->kind == PD_BLOCK_TABLE && pd_doc_table_props_ok(tp),
             (b->st.tp = *tp, memset(b->st.tp.col_width + tp->ncols, 0,
                                     (size_t)(PD_TABLE_MAX_COLS - tp->ncols) * sizeof(pd_sp))));
}

pd_status pd_doc_set_cell_props(pd_doc* d, pd_block_id id, const pd_cell_props* cp) {
    blk* b = d ? pd_doc_blk(d, id) : NULL;

    BLOCK_OP("Cell", b && b->kind == PD_BLOCK_CELL && cp && cp->col_span >= 1 &&
             cp->col_span <= PD_TABLE_MAX_COLS && cp->valign >= 0 && cp->valign <= 2 && cp->merge_up >= 0 &&
             cp->merge_up <= 1 && cp->min_height >= 0 && cp->border_width >= 0 && cp->edge_width[0] >= 0 && cp->edge_width[1] >= 0 &&
             cp->edge_width[2] >= 0 && cp->edge_width[3] >= 0 && (cp->border_set & ~15) == 0 &&
             (cp->border_on & ~cp->border_set) == 0, b->st.cell = *cp);
}

pd_status pd_doc_set_break(pd_doc* d, pd_block_id id, pd_break_kind kind) {
    blk* b = d ? pd_doc_blk(d, id) : NULL;

    BLOCK_OP("Break", b && b->kind == PD_BLOCK_BREAK && kind >= PD_BREAK_PAGE && kind <= PD_BREAK_RULE,
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
    d->style_rev++;
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

void pd_doc_clear_undo(pd_doc* d) {
    int32_t i;

    if (!d || d->cur) {
        return;
    }

    for (i = 0; i < d->nundo; i++) {
        step_free(d, &d->undo[i], 1);
    }

    d->nundo = 0;
    clear_redo(d);
    d->typing_open = 0;
}

void pd_doc_set_undo_limit(pd_doc* d, int32_t steps) {
    if (d && steps >= 0) {
        d->undo_limit = steps;
        enforce_limit(d);
    }
}

/* ------------------------------------------------------------------ */
/* paragraph attributes, metadata, pictures by address                */
/* ------------------------------------------------------------------ */

void pd_doc_image_display_size(const pd_doc* d, const pd_inline* o, pd_sp* w, pd_sp* h) {
    pd_doc_image_size(d, o, w, h);
}

void pd_doc_image_size(const pd_doc* d, const pd_inline* o, pd_sp* w, pd_sp* h) {
    int32_t pw = 0, ph = 0;

    *w = o->width;
    *h = o->height;

    if (*w > 0 && *h > 0) {
        return;
    }

    if (o->resource >= 1 && (int32_t)o->resource <= d->nres) {
        pd_doc_image_info(d->res[o->resource - 1].data, d->res[o->resource - 1].len, &pw, &ph);
    }

    if (pw <= 0 || ph <= 0) {   /* not loaded, or not a picture this can read: a placeholder */
        pw = 128;
        ph = 96;
    }

    if (*w > 0) {
        *h = (pd_sp)((int64_t)*w * ph / pw);
    } else if (*h > 0) {
        *w = (pd_sp)((int64_t)*h * pw / ph);
    } else {
        *w = (pd_sp)((int64_t)pw * 3 * 65536 / 4);  /* pixels at 96 dpi */
        *h = (pd_sp)((int64_t)ph * 3 * 65536 / 4);
    }
}

pd_status pd_doc_para_attrs(const pd_doc* d, pd_block_id para, pd_para_attrs* out) {
    blk* b = d ? para_of(d, para) : NULL;

    if (!b || !out) {
        return PD_ERR_ARG;
    }

    *out = b->st.at;
    return PD_OK;
}

pd_status pd_doc_set_para_attrs(pd_doc* d, pd_block_id para, const pd_para_attrs* at) {
    blk* b = d ? para_of(d, para) : NULL;
    pd_para_attrs a;

    memset(&a, 0, sizeof(a));

    if (at) {
        a = *at;
        a.lang[sizeof(a.lang) - 1] = '\0';
        a.div_class[sizeof(a.div_class) - 1] = '\0';
    }

    BLOCK_OP("Paragraph attributes", b && at && a.quote_depth >= 0 && a.quote_depth <= 9 && a.task >= 0 &&
             a.task <= 2 && (a.loose == 0 || a.loose == 1) && (a.cont == 0 || a.cont == 1) &&
             pd_doc_utf8_valid(a.lang, strlen(a.lang), 0) && pd_doc_utf8_valid(a.div_class, strlen(a.div_class), 0),
             b->st.at = a);
}

pd_status pd_doc_set_metadata(pd_doc* d, const char* text, size_t len) {
    char* m = NULL;

    if (!d || (len && !text) || len > 0x10000000 || (len && !pd_doc_utf8_valid(text, len, 1))) {
        return PD_ERR_ARG;
    }

    if (len && (m = (char*)malloc(len + 1)) == NULL) {
        return PD_ERR_NOMEM;
    }

    if (m) {
        memcpy(m, text, len);
        m[len] = '\0';
    }

    free(d->meta);
    d->meta = m;
    d->meta_len = len;
    d->meta_rev++;
    return PD_OK;
}

const char* pd_doc_metadata(const pd_doc* d, size_t* len) {
    if (len) {
        *len = d ? d->meta_len : 0;
    }

    return d && d->meta ? d->meta : "";
}

/* the font table: a line per font, name TAB alternates TAB generic TAB pitch TAB panose */
static int same_name(const char* a, size_t na, const char* b) {
    size_t i;

    for (i = 0; i < na && b[i]; i++) {
        char x = a[i] >= 'A' && a[i] <= 'Z' ? (char)(a[i] + 32) : a[i];
        char y = b[i] >= 'A' && b[i] <= 'Z' ? (char)(b[i] + 32) : b[i];

        if (x != y) {
            return 0;
        }
    }

    return i == na && !b[i];
}

pd_status pd_doc_font_info(const pd_doc* d, const char* family, pd_font_info* out) {
    int32_t r;

    if (!d || !family || !out) {
        return PD_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->charset = -1;

    for (r = d->nres - 1; r >= 0; r--) {    /* the latest table */
        const char* p, *end;

        if (strcmp(d->res[r].mime, PD_FONT_TABLE_MIME) != 0) {
            continue;
        }

        p = (const char*)d->res[r].data;
        end = p + d->res[r].len;

        while (p < end) {
            const char* eol = (const char*)memchr(p, '\n', (size_t)(end - p)), *f[6];
            size_t fl[6];
            int k;

            eol = eol ? eol : end;

            for (k = 0; k < 6; k++) {   /* the fields */
                const char* tab = p < eol ? (const char*)memchr(p, '\t', (size_t)(eol - p)) : NULL;

                f[k] = p;
                fl[k] = (size_t)((tab ? tab : eol) - p);
                p = tab ? tab + 1 : eol;
            }

            if (same_name(f[0], fl[0], family)) {
                snprintf(out->alt, sizeof(out->alt), "%.*s", (int)fl[1], f[1]);
                out->generic = fl[2] ? f[2][0] - '0' : 0;
                out->generic = out->generic < 0 || out->generic > 5 ? 0 : out->generic;
                out->pitch = fl[3] ? f[3][0] - '0' : 0;
                out->pitch = out->pitch < 0 || out->pitch > 2 ? 0 : out->pitch;

                for (k = 0; k < 10 && fl[4] >= 20; k++) {
                    char h[3] = { f[4][2 * k], f[4][2 * k + 1], 0 };

                    out->panose[k] = (uint8_t)strtoul(h, NULL, 16);
                }

                out->charset = fl[5] == 2 ? (int32_t)strtoul(f[5], NULL, 16) : -1;

                return PD_OK;
            }

            p = eol + 1;
        }

        break;
    }

    return PD_ERR_RANGE;
}

int32_t pd_doc_font_class(const pd_doc* d, const char* family) {
    pd_font_info fi;
    int sure, cjk;
    int32_t cls = pd_font_name_class(family, &sure);
    const char* p;

    if (sure || !d || !family || pd_doc_font_info(d, family, &fi) != PD_OK) {
        return cls;     /* a name that says what it is beats a table: generators write roman for anything */
    }

    for (p = fi.alt; *p;) {     /* a name it also goes by that says */
        char one[128];
        size_t n = strcspn(p, ",");
        int32_t c;

        snprintf(one, sizeof(one), "%.*s", (int)n, p);
        c = pd_font_name_class(one, &sure);

        if (sure) {
            return c;
        }

        p += n + (p[n] == ',');
    }

    /* a CJK face is fixed pitch for its ideographs, and Word says so: not a typewriter's */
    cjk = fi.charset == 0x80 || fi.charset == 0x81 || fi.charset == 0x82 || fi.charset == 0x86 || fi.charset == 0x88;

    if (fi.panose[0] == 2 && fi.panose[3] == 9 && !cjk) {
        return PD_FAMILY_MONO;      /* PANOSE's proportion: monospaced */
    }

    if (fi.panose[0] == 2 && fi.panose[1] >= 11 && fi.panose[1] <= 15) {
        return PD_FAMILY_SANS;      /* serif style: normal, obtuse, perpendicular sans; flared; rounded */
    }

    if (fi.panose[0] == 2 && fi.panose[1] >= 2 && fi.panose[1] <= 10) {
        return PD_FAMILY_SERIF;
    }

    if (fi.pitch == 1 && !cjk) {
        return PD_FAMILY_MONO;
    }

    if (fi.generic == PD_FONT_GENERIC_SWISS) {
        return PD_FAMILY_SANS;
    }

    if (fi.generic == PD_FONT_GENERIC_MODERN && fi.pitch != 2 && !cjk) {
        return PD_FAMILY_MONO;
    }

    return PD_FAMILY_SERIF;
}

pd_status pd_doc_control_at(const pd_doc* d, pd_pos pos, pd_pos* start, pd_pos* end) {
    const char* s;
    uint32_t n, i, open[32];
    int depth = 0;

    if (!d || !start || !end || pd_doc_para_text(d, pos.block, &s, &n) != PD_OK || pos.offset > n) {
        return d && start && end ? PD_ERR_RANGE : PD_ERR_ARG;
    }

    /* the controls open at pos: each start pushed, each end popping the latest */
    for (i = 0; i + 3 <= pos.offset; i++) {
        pd_inline o;
        pd_pos at;

        if ((unsigned char)s[i] != 0xEF || (unsigned char)s[i + 1] != 0xBF || (unsigned char)s[i + 2] != 0xBC) {
            continue;
        }

        at.block = pos.block;
        at.offset = i;

        if (pd_doc_inline_at(d, at, &o) == PD_OK && o.kind == PD_INLINE_CONTROL) {
            if (o.name[0] && depth < 32) {
                open[depth++] = i;
            } else if (!o.name[0] && depth > 0) {
                depth--;
            }
        }

        i += 2;
    }

    if (depth == 0) {
        return PD_ERR_RANGE;
    }

    start->block = end->block = pos.block;
    start->offset = open[depth - 1];
    end->offset = n;
    depth = 0;

    /* its end: the end object that closes it, counting those of controls inside */
    for (i = start->offset + 3; i + 3 <= n; i++) {
        pd_inline o;
        pd_pos at;

        if ((unsigned char)s[i] != 0xEF || (unsigned char)s[i + 1] != 0xBF || (unsigned char)s[i + 2] != 0xBC) {
            continue;
        }

        at.block = pos.block;
        at.offset = i;

        if (pd_doc_inline_at(d, at, &o) == PD_OK && o.kind == PD_INLINE_CONTROL) {
            if (o.name[0]) {
                depth++;
            } else if (depth-- == 0) {
                end->offset = i;
                break;
            }
        }

        i += 2;
    }

    return PD_OK;
}

/* a picture's type and pixel size from its first bytes: PNG, JPEG, GIF */
const char* pd_doc_image_info(const unsigned char* p, size_t n, int32_t* w, int32_t* h) {
    *w = *h = 0;

    if (n >= 24 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(p + 12, "IHDR", 4) == 0) {
        *w = (int32_t)(((uint32_t)p[16] << 24) | ((uint32_t)p[17] << 16) | ((uint32_t)p[18] << 8) | p[19]);
        *h = (int32_t)(((uint32_t)p[20] << 24) | ((uint32_t)p[21] << 16) | ((uint32_t)p[22] << 8) | p[23]);
        return "image/png";
    }

    if (n >= 10 && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0)) {
        *w = p[6] | (p[7] << 8);
        *h = p[8] | (p[9] << 8);
        return "image/gif";
    }

    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        size_t i = 2;

        while (i + 9 < n && p[i] == 0xFF) {
            int mk = p[i + 1];
            size_t len = ((size_t)p[i + 2] << 8) | p[i + 3];

            if ((mk >= 0xC0 && mk <= 0xCF && mk != 0xC4 && mk != 0xC8 && mk != 0xCC)) {     /* a frame header */
                *h = (p[i + 5] << 8) | p[i + 6];
                *w = (p[i + 7] << 8) | p[i + 8];
                break;
            }

            i += 2 + len;
        }

        return "image/jpeg";
    }

    return NULL;
}

typedef struct {
    unsigned char* p;
    size_t n, cap;
    int err;
} ibuf;

static int to_ibuf(void* user, const void* data, size_t len) {
    ibuf* b = (ibuf*)user;

    if (b->err || len > ((size_t)1 << 30) - b->n) {     /* a picture over a gigabyte is not one */
        return b->err = 1;
    }

    if (b->n + len > b->cap) {
        size_t nc = (b->n + len) * 2;
        unsigned char* q = (unsigned char*)realloc(b->p, nc);

        if (!q) {
            return b->err = 1;
        }

        b->p = q;
        b->cap = nc;
    }

    memcpy(b->p + b->n, data, len);
    b->n += len;
    return 0;
}

static int32_t load_images_in(pd_doc* d, blk* b, pd_image_fetch fetch, void* user) {
    int32_t i, n = 0;

    if (!b) {
        return 0;
    }

    for (i = 0; i < b->nkids; i++) {
        n += load_images_in(d, d->tab[b->kids[i]], fetch, user);
    }

    for (i = 0; b->kind == PD_BLOCK_PARAGRAPH && i < b->st.ninl; i++) {
        pd_inline* o = &b->st.inl[i].obj;
        ibuf data;
        const char* mime;
        int32_t w, h;
        pd_res_id rid;

        if (o->kind != PD_INLINE_IMAGE || o->resource || o->source_len <= 0) {
            continue;
        }

        memset(&data, 0, sizeof(data));

        if (fetch(user, o->source, to_ibuf, &data) == 0 && !data.err && data.n > 0 &&
                (mime = pd_doc_image_info(data.p, data.n, &w, &h)) != NULL &&
                pd_doc_add_resource(d, mime, data.p, data.n, &rid) == PD_OK) {
            o->resource = rid;

            if (w > 0 && h > 0 && (o->width <= 0 || o->height <= 0)) {     /* at 96 dpi; one side given: keep the shape */
                pd_sp nw = (pd_sp)((int64_t)w * 3 * 65536 / 4), nh = (pd_sp)((int64_t)h * 3 * 65536 / 4);

                if (o->width > 0) {
                    nh = (pd_sp)((int64_t)nh * o->width / nw);
                    nw = o->width;
                } else if (o->height > 0) {
                    nw = (pd_sp)((int64_t)nw * o->height / nh);
                    nh = o->height;
                }

                o->width = nw;
                o->height = nh;
            }

            touch(d, PD_CHANGE_TEXT, b->id, 0);
            n++;
        }

        free(data.p);
    }

    return n;
}

int32_t pd_doc_load_images(pd_doc* d, pd_image_fetch fetch, void* user) {
    int32_t n;

    if (!d || !fetch) {
        return 0;
    }

    d->revision++;
    d->ntouched = 0;
    n = load_images_in(d, d->tab[PD_ROOT_ID], fetch, user) + load_images_in(d, pd_doc_blk(d, PD_STORYROOT_ID), fetch,
            user);
    notify(d);
    return n;
}

/* ------------------------------------------------------------------ */
/* tracked changes                                                    */
/* ------------------------------------------------------------------ */

static const pd_revision* rev_of(const pd_doc* d, pd_rev_id id) {
    return id >= 1 && (int32_t)id <= d->nrevs ? &d->revs[id - 1] : NULL;
}

static pd_rev_id format_rev(const pd_doc* d, pd_format_id fmt) {
    const dformat* f = format_of(d, fmt);
    return f && (f->cp.mask & PD_CP_REVISION) ? f->cp.revision : 0;
}

/* fmt with its revision replaced (0: none) */
static pd_format_id with_revision(pd_doc* d, pd_format_id fmt, pd_rev_id rev) {
    const dformat* f = format_of(d, fmt);
    dformat nf;

    if (!f || format_rev(d, fmt) == rev) {
        return fmt;
    }

    nf = *f;
    nf.cp.revision = rev;
    nf.cp.mask = rev ? nf.cp.mask | PD_CP_REVISION : nf.cp.mask & ~PD_CP_REVISION;
    return intern(d, nf.style, &nf.cp);
}

pd_status pd_doc_revision_add(pd_doc* d, const pd_revision* rev, pd_rev_id* out) {
    pd_revision r;
    int32_t i;

    if (!d || !rev || !out || (rev->kind != PD_REV_INSERT && rev->kind != PD_REV_DELETE)) {
        return PD_ERR_ARG;
    }

    memset(&r, 0, sizeof(r));
    r.kind = rev->kind;
    memcpy(r.author, rev->author, sizeof(r.author) - 1);
    memcpy(r.date, rev->date, sizeof(r.date) - 1);
    r.author[strlen(r.author)] = '\0';     /* bytes after the end are cleared, so equal revisions compare equal */
    memset(r.author + strlen(r.author), 0, sizeof(r.author) - strlen(r.author));
    memset(r.date + strlen(r.date), 0, sizeof(r.date) - strlen(r.date));

    if (!pd_doc_utf8_valid(r.author, strlen(r.author), 0) || !pd_doc_utf8_valid(r.date, strlen(r.date), 0)) {
        return PD_ERR_ARG;
    }

    for (i = 0; i < d->nrevs; i++) {
        if (d->revs[i].kind == r.kind && !strcmp(d->revs[i].author, r.author) && !strcmp(d->revs[i].date, r.date)) {
            *out = (pd_rev_id)(i + 1);
            return PD_OK;
        }
    }

    if (d->nrevs >= 0xFFFFFF || grow((void**)&d->revs, &d->caprevs, (int64_t)d->nrevs + 1, sizeof(pd_revision))) {
        return PD_ERR_NOMEM;
    }

    d->revs[d->nrevs++] = r;
    *out = (pd_rev_id)d->nrevs;
    return PD_OK;
}

pd_status pd_doc_revision_get(const pd_doc* d, pd_rev_id rev, pd_revision* out) {
    const pd_revision* r = d ? rev_of(d, rev) : NULL;

    if (!r || !out) {
        return PD_ERR_ARG;
    }

    *out = *r;
    return PD_OK;
}

int32_t pd_doc_revision_count(const pd_doc* d) {
    return d ? d->nrevs : 0;
}

int32_t pd_doc_author_index(const pd_doc* d, const char* author) {
    int32_t i, j, n = 0;

    if (!d || !author) {
        return -1;
    }

    for (i = 0; i < d->nrevs + d->ncomments; i++) {
        const char* a = i < d->nrevs ? d->revs[i].author : d->comments[i - d->nrevs].author;
        int seen = 0;

        for (j = 0; j < i && !seen; j++) {
            seen = !strcmp(a, j < d->nrevs ? d->revs[j].author : d->comments[j - d->nrevs].author);
        }

        if (!seen) {
            if (!strcmp(a, author)) {
                return n;
            }

            n++;
        }
    }

    return -1;
}

uint32_t pd_doc_author_color(const pd_doc* d, const char* author) {
    static const uint32_t pal[8] = {0xFF1565C0u, 0xFFC2185Bu, 0xFF2E7D32u, 0xFFE65100u,
                                    0xFF6A1B9Au, 0xFF00838Fu, 0xFF8D6E00u, 0xFF5D4037u
                                   };
    int32_t i = pd_doc_author_index(d, author);

    return pal[(i < 0 ? 0 : i) % 8];
}

/* this tracking session's revision of a kind, made on first use; 0 when not tracking */
static pd_rev_id track_rev(pd_doc* d, int32_t kind) {
    pd_rev_id* id = kind == PD_REV_INSERT ? &d->track_ins : &d->track_del;

    if (!d->track[0]) {
        return 0;
    }

    if (!*id) {
        pd_revision r;
        time_t now = time(NULL);
        struct tm* t = gmtime(&now);

        memset(&r, 0, sizeof(r));
        r.kind = kind;
        memcpy(r.author, d->track, sizeof(r.author));

        if (t) {
            strftime(r.date, sizeof(r.date), "%Y-%m-%dT%H:%M:%SZ", t);
        }

        if (pd_doc_revision_add(d, &r, id) != PD_OK) {
            *id = 0;
        }
    }

    return *id;
}

pd_status pd_doc_set_tracking(pd_doc* d, const char* author) {
    if (!d || (author && (strlen(author) >= sizeof(d->track) || !pd_doc_utf8_valid(author, strlen(author), 0)))) {
        return PD_ERR_ARG;
    }

    memset(d->track, 0, sizeof(d->track));

    if (author) {
        strcpy(d->track, author);
    }

    d->track_ins = d->track_del = 0;
    return PD_OK;
}

const char* pd_doc_tracking(const pd_doc* d) {
    return d && d->track[0] ? d->track : NULL;
}

void pd_doc_set_markup(pd_doc* d, int32_t mode) {
    if (d && mode >= PD_MARKUP_BALLOONS && mode <= PD_MARKUP_ORIGINAL && mode != d->markup) {
        d->markup = mode;
        d->style_rev++;     /* every paragraph may look different */
    }
}

int32_t pd_doc_markup(const pd_doc* d) {
    return d ? d->markup : 0;
}

void pd_doc_markup_props(const pd_doc* d, pd_char_props* cp) {
    const pd_revision* r = cp->revision ? rev_of(d, cp->revision) : NULL;
    int ins;

    if (!r) {
        return;
    }

    ins = r->kind == PD_REV_INSERT;

    if (d->markup == PD_MARKUP_FINAL || d->markup == PD_MARKUP_ORIGINAL) {
        if (ins == (d->markup == PD_MARKUP_ORIGINAL)) {
            cp->hidden = 1;
        }

        return;
    }

    cp->color = pd_doc_author_color(d, r->author);

    if (ins) {
        cp->underline = PD_UNDERLINE_SINGLE;
    } else if (d->markup == PD_MARKUP_BALLOONS) {
        cp->hidden = 1;
    } else {
        cp->strike = 1;
    }
}

/* a paragraph range in reading order: start before end */
static int range_ok(const pd_doc* d, pd_range r) {
    blk* a = para_of(d, r.start.block), *e = para_of(d, r.end.block);
    pd_block_id cur;

    if (!a || !e || !is_boundary(&a->st, r.start.offset) || !is_boundary(&e->st, r.end.offset)) {
        return 0;
    }

    if (a == e) {
        return r.start.offset <= r.end.offset;
    }

    for (cur = a->id; cur && cur != e->id; cur = step_para(d, cur, 1)) {
    }

    return cur != 0;
}

/* the runs of [a, e) of a paragraph, split at a and e, copied out */
static pd_run* runs_in(bstate* s, uint32_t a, uint32_t e, int32_t* n) {
    pd_run* out;
    int32_t i;

    *n = 0;

    if (runs_split(s, a) || runs_split(s, e) || (out = (pd_run*)malloc(((size_t)s->nruns + 1) * sizeof(pd_run))) == NULL) {
        return NULL;
    }

    for (i = 0; i < s->nruns; i++) {
        if (s->runs[i].start >= a && s->runs[i].end <= e) {
            out[(*n)++] = s->runs[i];
        }
    }

    return out;
}

/* what happens to one run: 0 keep, 1 remove the text, 2 set the format to rev */
typedef int (*run_action)(pd_doc* d, const pd_revision* r, void* user);

static int act_track(pd_doc* d, const pd_revision* r, void* user) {
    (void)user;

    if (r && r->kind == PD_REV_INSERT && !strcmp(r->author, d->track)) {
        return 1;   /* one's own pending insertion just goes */
    }

    return r && r->kind == PD_REV_DELETE ? 0 : 2;
}

static int act_resolve(pd_doc* d, const pd_revision* r, void* user) {
    int accept = *(const int*)user;

    (void)d;

    if (!r) {
        return 0;
    }

    return (r->kind == PD_REV_DELETE) == (accept != 0) ? 1 : 3;     /* 3: back to plain text */
}

/* apply an action to every run of a range; whole paragraphs emptied by removal go when drop is set */
static pd_status for_revisions(pd_doc* d, pd_range r, const char* label, run_action fn, void* user, pd_rev_id rev,
                               int drop) {
    pd_block_id cur, next;
    pd_status st = PD_OK;

    if (op_begin(d, label, 0, 0, 0)) {
        op_end(d, 0, 0, 0);
        return PD_ERR_NOMEM;
    }

    for (cur = r.start.block; cur && st == PD_OK; cur = next) {
        blk* b = d->tab[cur];
        uint32_t s0 = cur == r.start.block ? r.start.offset : 0, e0 = cur == r.end.block ? r.end.offset : b->st.len;
        uint32_t had = b->st.len;
        int32_t n, i, removed = 0;
        pd_run* rs;

        next = cur == r.end.block ? 0 : step_para(d, cur, 1);

        if (s0 >= e0) {
            continue;
        }

        if (snap(d, b) || (rs = runs_in(&b->st, s0, e0, &n)) == NULL) {
            st = PD_ERR_NOMEM;
            break;
        }

        for (i = n - 1; i >= 0; i--) {     /* backwards: offsets before a removal stay put */
            int a = fn(d, rev_of(d, format_rev(d, rs[i].format)), user);

            if (a == 1) {
                content_delete(&b->st, rs[i].start, rs[i].end);
                markers_delete(d, cur, rs[i].start, rs[i].end);
                removed = 1;
            } else if (a >= 2) {
                pd_char_props cp;

                memset(&cp, 0, sizeof(cp));
                cp.mask = PD_CP_REVISION;
                cp.revision = a == 2 ? rev : 0;

                if (range_map_formats(d, &b->st, rs[i].start, rs[i].end, a == 2 ? 0 : 1, &cp, PD_CP_REVISION, 0)) {
                    st = PD_ERR_NOMEM;
                }
            }
        }

        free(rs);
        touch(d, PD_CHANGE_TEXT, cur, 0);

        /* a paragraph whose whole text was a deletion goes with it, as its mark would in Word */
        if (drop && removed && had > 0 && b->st.len == 0 && s0 == 0 && e0 == had && b->kind == PD_BLOCK_PARAGRAPH) {
            blk* p = d->tab[b->parent];
            pd_pos fb;

            if (p && p->nkids > 1 && (next || step_para(d, cur, -1))) {
                fb.block = next ? next : step_para(d, cur, -1);
                fb.offset = 0;

                if (do_detach(d, cur)) {
                    st = PD_ERR_NOMEM;
                }

                clamp_markers(d, fb);
            }
        }
    }

    op_end(d, 0, 0, 0);
    return st;
}

static pd_status track_delete(pd_doc* d, pd_range r, pd_pos* after) {
    pd_rev_id del;

    if (!range_ok(d, r)) {
        return PD_ERR_ARG;
    }

    if (after) {
        *after = r.start;
    }

    if (r.start.block == r.end.block && r.start.offset == r.end.offset) {
        return PD_OK;
    }

    if ((del = track_rev(d, PD_REV_DELETE)) == 0) {
        return PD_ERR_NOMEM;
    }

    return for_revisions(d, r, "Delete", act_track, NULL, del, 0);
}

pd_status pd_doc_revision_resolve(pd_doc* d, pd_range r, int32_t accept) {
    int a = accept != 0;

    if (!d || !range_ok(d, r)) {
        return PD_ERR_ARG;
    }

    return for_revisions(d, r, a ? "Accept changes" : "Reject changes", act_resolve, &a, 0, 1);
}

pd_status pd_doc_revision_find(const pd_doc* d, pd_pos from, int32_t dir, pd_range* out, pd_rev_id* rev) {
    pd_block_id cur;
    int first = 1;

    if (!d || !out || !para_of(d, from.block) || dir == 0) {
        return PD_ERR_ARG;
    }

    dir = dir > 0 ? 1 : -1;

    for (cur = from.block; cur; cur = step_para(d, cur, dir), first = 0) {
        const bstate* s = &d->tab[cur]->st;
        int32_t i, k;

        for (k = 0; k < s->nruns; k++) {
            int32_t j;
            pd_rev_id v;

            i = dir > 0 ? k : s->nruns - 1 - k;
            v = format_rev(d, s->runs[i].format);

            if (!v || (first && (dir > 0 ? s->runs[i].start < from.offset : s->runs[i].end > from.offset))) {
                continue;
            }

            /* the stretch: neighbouring runs of the same revision */
            for (j = i; j + 1 < s->nruns && s->runs[j + 1].start == s->runs[j].end &&
                    format_rev(d, s->runs[j + 1].format) == v && !(first && dir < 0 && s->runs[j + 1].end > from.offset); j++) {
            }

            for (; i > 0 && s->runs[i - 1].end == s->runs[i].start && format_rev(d, s->runs[i - 1].format) == v &&
                    !(first && dir > 0 && s->runs[i - 1].start < from.offset); i--) {
            }

            out->start.offset = s->runs[i].start;
            out->end.offset = s->runs[j].end;
            out->start.block = out->end.block = cur;

            if (rev) {
                *rev = v;
            }

            return PD_OK;
        }
    }

    return PD_ERR_RANGE;
}

/* ------------------------------------------------------------------ */
/* comments                                                           */
/* ------------------------------------------------------------------ */

static dcomment* comment_of(const pd_doc* d, pd_comment_id id) {
    return id >= 1 && (int32_t)id <= d->ncomments && d->comments[id - 1].alive ? &d->comments[id - 1] : NULL;
}

/* fill a comment record from the public form (text copied); -1 bad input, -2 no memory */
static int comment_fill(dcomment* c, const pd_comment* in) {
    if (!memchr(in->author, 0, sizeof(in->author)) || !memchr(in->date, 0, sizeof(in->date)) ||
            !pd_doc_utf8_valid(in->author, strlen(in->author), 0) || !pd_doc_utf8_valid(in->date, strlen(in->date), 0) ||
            (in->text_len && !in->text) || in->text_len > INT32_MAX / 2 ||
            !pd_doc_utf8_valid(in->text ? in->text : "", in->text_len, 0)) {
        return -1;
    }

    memcpy(c->author, in->author, sizeof(c->author));
    memcpy(c->date, in->date, sizeof(c->date));
    c->resolved = in->resolved != 0;
    c->len = in->text_len;

    if ((c->text = (char*)malloc((size_t)in->text_len + 1)) == NULL) {
        return -2;
    }

    if (in->text_len) {
        memcpy(c->text, in->text, in->text_len);
    }

    c->text[in->text_len] = '\0';
    return 0;
}

/* swap a comment for a new version under undo */
static pd_status comment_swap(pd_doc* d, pd_comment_id id, const dcomment* nv, const char* label) {
    urec* r;

    if (op_begin(d, label, 0, 0, 0) || (r = add_rec(d, UR_COMMENT)) == NULL) {
        op_end(d, 0, 0, 0);
        free(nv->text);
        return PD_ERR_NOMEM;
    }

    r->comment = id;
    r->csave = d->comments[id - 1];
    d->comments[id - 1] = *nv;
    d->comment_rev++;
    touch(d, PD_CHANGE_FORMAT, 0, 0);
    op_end(d, 0, 0, 0);
    return PD_OK;
}

static pd_status comment_add(pd_doc* d, const pd_comment* in, pd_comment_id* out, int undo) {
    dcomment c;
    const dcomment* p;
    int e;

    if (!d || !in || !out) {
        return PD_ERR_ARG;
    }

    p = in->parent ? comment_of(d, in->parent) : NULL;

    if (p && p->parent) {   /* a reply to a reply is one more in the thread */
        p = comment_of(d, p->parent);
    }

    if ((in->parent && !p) || (!p && !range_ok(d, in->range))) {
        return PD_ERR_ARG;
    }

    memset(&c, 0, sizeof(c));

    if ((e = comment_fill(&c, in)) != 0) {
        return e == -1 ? PD_ERR_ARG : PD_ERR_NOMEM;
    }

    c.alive = 1;
    c.parent = p ? (pd_comment_id)(p - d->comments + 1) : 0;

    if (p) {
        c.start = p->start;
        c.end = p->end;
    } else if (pd_doc_marker_new(d, in->range.start, PD_GRAVITY_RIGHT, &c.start) != PD_OK ||
               pd_doc_marker_new(d, in->range.end, PD_GRAVITY_LEFT, &c.end) != PD_OK) {
        free(c.text);
        return PD_ERR_NOMEM;
    }

    if (grow((void**)&d->comments, &d->capcomments, (int64_t)d->ncomments + 1, sizeof(dcomment))) {
        free(c.text);
        return PD_ERR_NOMEM;
    }

    memset(&d->comments[d->ncomments], 0, sizeof(dcomment));     /* the record of "not there yet" */
    d->comments[d->ncomments].start = c.start;
    d->comments[d->ncomments].end = c.end;
    d->ncomments++;
    *out = (pd_comment_id)d->ncomments;

    if (!undo) {
        d->comments[d->ncomments - 1] = c;
        d->comment_rev++;
        return PD_OK;
    }

    return comment_swap(d, *out, &c, "Comment");
}

pd_status pd_doc_comment_add(pd_doc* d, const pd_comment* in, pd_comment_id* out) {
    return comment_add(d, in, out, 1);
}

pd_status pd_doc_comment_add_raw(pd_doc* d, const pd_comment* in, pd_comment_id* out) {
    return comment_add(d, in, out, 0);
}

pd_status pd_doc_comment_get(const pd_doc* d, pd_comment_id id, pd_comment* out) {
    const dcomment* c = d ? comment_of(d, id) : NULL;

    if (!c || !out) {
        return PD_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->author, c->author, sizeof(out->author));
    memcpy(out->date, c->date, sizeof(out->date));
    out->text = c->text;
    out->text_len = c->len;
    out->parent = c->parent;
    out->resolved = c->resolved;
    pd_doc_marker_get(d, c->start, &out->range.start);
    pd_doc_marker_get(d, c->end, &out->range.end);

    if (!range_ok(d, out->range)) {     /* edits crossed the ends over */
        out->range.end = out->range.start;
    }

    return PD_OK;
}

pd_status pd_doc_comment_set(pd_doc* d, pd_comment_id id, const pd_comment* in) {
    const dcomment* c = d ? comment_of(d, id) : NULL;
    dcomment nv;
    int e;

    if (!c || !in) {
        return PD_ERR_ARG;
    }

    nv = *c;

    if ((e = comment_fill(&nv, in)) != 0) {
        return e == -1 ? PD_ERR_ARG : PD_ERR_NOMEM;
    }

    return comment_swap(d, id, &nv, "Edit comment");
}

pd_status pd_doc_comment_remove(pd_doc* d, pd_comment_id id) {
    int32_t i;
    pd_status st = PD_OK;

    if (!d || !comment_of(d, id)) {
        return PD_ERR_ARG;
    }

    pd_doc_begin_group(d, "Delete comment");

    for (i = 0; i < d->ncomments && st == PD_OK; i++) {
        if (d->comments[i].alive && ((pd_comment_id)(i + 1) == id || d->comments[i].parent == id)) {
            dcomment dead = d->comments[i];

            dead.alive = 0;
            dead.text = NULL;
            dead.len = 0;
            st = comment_swap(d, (pd_comment_id)(i + 1), &dead, "Delete comment");
        }
    }

    pd_doc_end_group(d);
    return st;
}

int32_t pd_doc_comment_count(const pd_doc* d) {
    return d ? d->ncomments : 0;
}

/* ------------------------------------------------------------------ */
/* deltas: the parts pd_doc_io.c needs of the operation framework     */
/* ------------------------------------------------------------------ */

void pd_doc_set_alive(pd_doc* d, pd_block_id id, int alive) {
    set_alive(d, id, alive);
}

void pd_doc_bstate_free(bstate* s) {
    bstate_free(s);
}

int32_t pd_doc_touched_count(const pd_doc* d) {
    return d->ntouched;
}

void pd_doc_touched(const pd_doc* d, int32_t i, int32_t* kind, pd_block_id* block, pd_style_id* style) {
    *kind = d->touched[i].kind;
    *block = d->touched[i].block;
    *style = d->touched[i].style;
}

void pd_doc_delta_touch(pd_doc* d, int32_t kind, pd_block_id block, pd_style_id style) {
    if (kind == PD_CHANGE_STYLE) {
        d->style_rev++;
    }

    touch(d, kind, block, style);
}

/* an applied delta is reported like an operation, and the undo steps, which describe what was there, go */
void pd_doc_delta_commit(pd_doc* d) {
    pd_pos none;

    none.block = 0;
    none.offset = 0;
    clamp_markers(d, none);
    d->applying = 1;
    notify(d);
    d->applying = 0;
    pd_doc_clear_undo(d);
}
