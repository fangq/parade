/*
 * Parade paragraphs: content building, simple shaping, output and hit testing
 *
 * Text is turned into a TeX item list as it is added: glyph runs become
 * boxes, spaces become glue, and break opportunities become glue or
 * penalties. The built-in shaper maps code points 1:1 to glyphs through
 * cmap and applies pair kerning, which covers Latin, Greek, Cyrillic and
 * CJK; a full OpenType shaper can replace shape_run() later.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_unidata.h"

/* ------------------------------------------------------------------ */
/* small utilities                                                    */
/* ------------------------------------------------------------------ */

int pd_grow(void** ptr, int32_t* cap, int64_t need, size_t elem) {
    int64_t nc;
    void* np;

    if (need <= *cap) {
        return 0;
    }

    if (need > INT32_MAX) {
        return -1;
    }

    nc = *cap ? *cap : 16;

    while (nc < need) {
        nc *= 2;
    }

    if (nc > INT32_MAX) {
        nc = INT32_MAX;
    }

    np = realloc(*ptr, (size_t)nc * elem);

    if (!np) {
        return -1;
    }

    *ptr = np;
    *cap = (int32_t)nc;
    return 0;
}

/* ideographic scripts that break between characters */
static int is_cjk(uint32_t c) {
    return (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3040 && c <= 0x30FF) || (c >= 0x3100 && c <= 0x312F) ||
           (c >= 0x31A0 && c <= 0x31FF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x3000 && c <= 0x303F) ||
           (c >= 0x20000 && c <= 0x3FFFF);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

pd_status pd_para_new(pd_para** out) {
    if (!out) {
        return PD_ERR_ARG;
    }

    *out = (pd_para*)calloc(1, sizeof(pd_para));
    return *out ? PD_OK : PD_ERR_NOMEM;
}

void pd_para_free(pd_para* p) {
    if (!p) {
        return;
    }

    free(p->items);
    free(p->glyphs);
    free(p->text);
    free(p->styles);
    free(p->old_items);
    free(p->old_text);
    free(p->shape_indent);
    free(p->shape_width);
    free(p->bp_item);
    free(p->states);
    free(p->sum_w);
    free(p->sum_st);
    free(p->sum_fil);
    free(p->sum_sh);
    free(p->sum_bx);
    free(p->lines);
    free(p->old_lines);
    free(p->blev);
    free(p);
}

void pd_para_clear(pd_para* p) {
    if (!p) {
        return;
    }

    /* the layout of the last pd_para_break stays as the reference for the next one */
    p->n_items = 0;
    p->n_glyphs = 0;
    p->n_text = 0;
    p->n_styles = 0;
    p->n_lines = 0;
    p->broken = 0;
    p->finalized = 0;
    p->height = 0;
}

void pd_style_init(pd_style* s, const pd_font* font, pd_sp size) {
    if (!s) {
        return;
    }

    memset(s, 0, sizeof(*s));
    s->font = font;
    s->size = size;
    s->space_stretch = 500;
    s->space_shrink = 333;
    s->kerning = 1;
    s->color = 0xFF000000u;
}

void pd_para_natural(const pd_para* p, pd_sp* minw, pd_sp* maxw) {
    pd_params prm;
    int64_t line = 0, word = 0, mn = 0, mx = 0;
    int32_t i;

    pd_params_init(&prm);

    for (i = 0; i < p->n_items; i++) {
        const pd_item* it = &p->items[i];

        if (it->type == PD_ITEM_BOX) {
            line += it->width;
            word += it->width;
        } else if (it->type == PD_ITEM_GLUE) {
            if (i > 0 && p->items[i - 1].type == PD_ITEM_BOX && !(it->flags & PD_FLAG_NOBREAK)) {
                mn = word > mn ? word : mn;
                word = 0;
            } else {
                word += it->width;
            }

            line += it->width;
        } else {
            int32_t pen = pd_item_penalty(it, &prm);

            if (pen <= -PD_INF_PENALTY) {
                mn = word > mn ? word : mn;
                mx = line > mx ? line : mx;
                line = word = 0;
            } else if (pen < PD_INF_PENALTY) {
                mn = word + it->width > mn ? word + it->width : mn;
                word = 0;
            }
        }
    }

    mn = word > mn ? word : mn;
    mx = line > mx ? line : mx;
    *minw = (pd_sp)(mn < INT32_MAX ? mn : INT32_MAX);
    *maxw = (pd_sp)(mx < INT32_MAX ? mx : INT32_MAX);
}

void pd_params_init(pd_params* prm) {
    if (!prm) {
        return;
    }

    memset(prm, 0, sizeof(*prm));
    prm->mode = PD_BREAK_OPTIMAL;
    prm->align = PD_ALIGN_JUSTIFY;
    prm->width = PD_PT(345);
    prm->line_penalty = 10;
    prm->adj_demerits = 10000;
    prm->double_hyphen_demerits = 10000;
    prm->final_hyphen_demerits = 5000;
    prm->hyphen_penalty = 50;
    prm->ex_hyphen_penalty = 50;
    prm->rag_stretch = PD_PT(30);
    prm->line_spacing = 1000;
    prm->freeze_offset = -1;
}

/* ------------------------------------------------------------------ */
/* content building                                                   */
/* ------------------------------------------------------------------ */

/* drop the paragraph-end items so more content can be appended */
static void unfinalize(pd_para* p) {
    while (p->n_items > 0 && (p->items[p->n_items - 1].flags & PD_FLAG_FINAL)) {
        p->n_items--;
    }

    p->finalized = 0;
    p->broken = 0;
}

static pd_item* new_item(pd_para* p, int type) {
    pd_item* it;

    if (pd_grow((void**)&p->items, &p->cap_items, (int64_t)p->n_items + 1, sizeof(pd_item))) {
        return NULL;
    }

    it = &p->items[p->n_items++];
    memset(it, 0, sizeof(*it));
    it->type = (uint8_t)type;
    it->style = -1;
    it->text_start = it->text_end = p->n_text;
    return it;
}

static int32_t intern_style(pd_para* p, const pd_style* s) {
    int32_t i;

    for (i = p->n_styles - 1; i >= 0; i--) {
        if (memcmp(&p->styles[i], s, sizeof(*s)) == 0) {
            return i;
        }
    }

    if (pd_grow((void**)&p->styles, &p->cap_styles, (int64_t)p->n_styles + 1, sizeof(pd_style))) {
        return -1;
    }

    p->styles[p->n_styles] = *s;
    return p->n_styles++;
}

static int append_text(pd_para* p, const char* s, size_t len) {
    if (pd_grow((void**)&p->text, &p->cap_text, (int64_t)p->n_text + (int64_t)len + 1, 1)) {
        return -1;
    }

    memcpy(p->text + p->n_text, s, len);
    p->n_text += (uint32_t)len;
    p->text[p->n_text] = '\0';
    return 0;
}

typedef struct {
    pd_para* p;
    const pd_style* st;
    int32_t style;
    pd_item* box;           /* open box, NULL if none (index-stable via box_index) */
    int32_t box_index;
    uint32_t prev_glyph;
    int32_t asc, desc;      /* scaled font extents */
    int32_t upem;
} builder;

static void close_box(builder* b) {
    b->box_index = -1;
    b->prev_glyph = 0;
}

static int add_glyph(builder* b, uint32_t cp, uint32_t cluster, uint32_t cluster_end) {
    pd_para* p = b->p;
    const pd_font* f = b->st->font;
    uint32_t g = pd_font_glyph_index(f, cp);
    pd_item* box;
    pd_gl* gl;

    if (b->box_index < 0) {
        box = new_item(p, PD_ITEM_BOX);

        if (!box) {
            return -1;
        }

        box->style = b->style;
        box->height = b->asc;
        box->depth = b->desc;
        box->text_start = cluster;
        box->glyph_start = (uint32_t)p->n_glyphs;
        b->box_index = p->n_items - 1;
    }

    box = &p->items[b->box_index];

    if (pd_grow((void**)&p->glyphs, &p->cap_glyphs, (int64_t)p->n_glyphs + 1, sizeof(pd_gl))) {
        return -1;
    }

    if (box->glyph_count > 0 && b->st->kerning) {
        int32_t k = pd_font_kerning(f, b->prev_glyph, g);

        if (k) {
            int32_t ks = pd_scale(k, b->st->size, b->upem);
            p->glyphs[p->n_glyphs - 1].advance += ks;
            box->width += ks;
        }
    }

    gl = &p->glyphs[p->n_glyphs++];
    gl->glyph = g;
    gl->cluster = cluster;
    gl->xoff = gl->yoff = 0;
    gl->advance = pd_scale(pd_font_glyph_advance(f, g), b->st->size, b->upem);
    box->width += gl->advance;
    box->glyph_count++;
    box->text_end = cluster_end;
    b->prev_glyph = g;
    return 0;
}

/* complex-shaped glyphs (logical order) into the open box */
static int add_shaped(builder* b, const pd_shaped* g, int32_t n, uint32_t text_start, uint32_t text_end) {
    pd_para* p = b->p;
    pd_item* box;
    int32_t k;

    if (b->box_index < 0) {
        box = new_item(p, PD_ITEM_BOX);

        if (!box) {
            return -1;
        }

        box->style = b->style;
        box->height = b->asc;
        box->depth = b->desc;
        box->text_start = text_start;
        box->glyph_start = (uint32_t)p->n_glyphs;
        b->box_index = p->n_items - 1;
    }

    if (pd_grow((void**)&p->glyphs, &p->cap_glyphs, (int64_t)p->n_glyphs + n, sizeof(pd_gl))) {
        return -1;
    }

    box = &p->items[b->box_index];

    for (k = 0; k < n; k++) {
        pd_gl* gl = &p->glyphs[p->n_glyphs++];

        gl->glyph = g[k].glyph;
        gl->cluster = g[k].cluster;
        gl->advance = pd_scale(g[k].advance, b->st->size, b->upem);
        gl->xoff = pd_scale(g[k].xoff, b->st->size, b->upem);
        gl->yoff = pd_scale(g[k].yoff, b->st->size, b->upem);
        box->width += gl->advance;
        box->glyph_count++;
    }

    box->text_end = text_end;
    b->prev_glyph = 0;
    return 0;
}

/* letters of the scripts hyphenation patterns cover */
static int is_letter(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
           (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x52F);
}

/* characters with a meaning of their own in the item list (spaces, breaks, hyphens) */
static int is_special(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0x00A0 || cp == 0x202F || cp == 0x200B || cp == 0x00AD || cp == '\r' ||
           cp == '\n' || cp == 0x0B || cp == 0x0C || cp == 0x85 || cp == 0x2028 || cp == 0x2029;
}

static int add_space(builder* b, uint32_t cluster, uint32_t cluster_end, int nobreak) {
    const pd_font* f = b->st->font;
    int32_t w = pd_scale(pd_font_glyph_advance(f, pd_font_glyph_index(f, ' ')), b->st->size, b->upem);
    pd_item* it;

    close_box(b);
    it = new_item(b->p, PD_ITEM_GLUE);

    if (!it) {
        return -1;
    }

    it->width = w;
    it->stretch = (int32_t)((int64_t)w * b->st->space_stretch / 1000);
    it->shrink = (int32_t)((int64_t)w * b->st->space_shrink / 1000);
    it->style = b->style;
    it->flags = nobreak ? PD_FLAG_NOBREAK : 0;
    it->text_start = cluster;
    it->text_end = cluster_end;
    return 0;
}

static int add_raw_glue(builder* b, uint32_t at, int32_t stretch, int fil) {
    pd_item* it;

    close_box(b);
    it = new_item(b->p, PD_ITEM_GLUE);

    if (!it) {
        return -1;
    }

    it->stretch = stretch;
    it->stretch_order = (uint8_t)(fil ? 1 : 0);
    it->style = b->style;
    it->text_start = it->text_end = at;
    return 0;
}

static int add_pen(builder* b, int32_t pen, int flags, uint32_t at, uint32_t at_end) {
    pd_item* it;

    close_box(b);
    it = new_item(b->p, PD_ITEM_PENALTY);

    if (!it) {
        return -1;
    }

    it->penalty = pen;
    it->flags = (uint8_t)flags;
    it->style = b->style;
    it->text_start = at;
    it->text_end = at_end;
    return 0;
}

/* soft hyphen: a flagged penalty carrying the hyphen glyph shown if we break there */
static int add_soft_hyphen(builder* b, uint32_t at, uint32_t at_end) {
    pd_para* p = b->p;
    const pd_font* f = b->st->font;
    uint32_t g = pd_font_glyph_index(f, 0x2010);
    pd_item* it;

    if (!g) {
        g = pd_font_glyph_index(f, '-');
    }

    if (add_pen(b, 0, PD_FLAG_FLAGGED | PD_FLAG_SOFTHYPHEN, at, at_end)) {
        return -1;
    }

    if (pd_grow((void**)&p->glyphs, &p->cap_glyphs, (int64_t)p->n_glyphs + 1, sizeof(pd_gl))) {
        return -1;
    }

    it = &p->items[p->n_items - 1];
    it->glyph_start = (uint32_t)p->n_glyphs;
    it->glyph_count = 1;
    p->glyphs[p->n_glyphs].glyph = g;
    p->glyphs[p->n_glyphs].cluster = at;
    p->glyphs[p->n_glyphs].xoff = p->glyphs[p->n_glyphs].yoff = 0;
    p->glyphs[p->n_glyphs].advance = pd_scale(pd_font_glyph_advance(f, g), b->st->size, b->upem);
    it->width = p->glyphs[p->n_glyphs].advance;
    p->n_glyphs++;
    return 0;
}

pd_status pd_para_add_text(pd_para* p, const char* utf8, size_t len, const pd_style* st) {
    builder b;
    pd_font_metrics fm;
    uint32_t base, *cps, *offs;
    uint8_t* brk, *hyp = NULL;
    int32_t n, first, ci;

    if (!p || (!utf8 && len) || !st || !st->font || st->size <= 0) {
        return PD_ERR_ARG;
    }

    if (len > (size_t)INT32_MAX - p->n_text) {
        return PD_ERR_RANGE;
    }

    unfinalize(p);
    base = p->n_text;

    if (append_text(p, utf8, len)) {
        return PD_ERR_NOMEM;
    }

    memset(&b, 0, sizeof(b));
    b.p = p;
    b.st = st;
    b.style = intern_style(p, st);

    if (b.style < 0) {
        return PD_ERR_NOMEM;
    }

    pd_font_get_metrics(st->font, &fm);
    b.upem = fm.units_per_em;
    b.asc = pd_scale(fm.ascender, st->size, b.upem);
    b.desc = pd_scale(-fm.descender, st->size, b.upem);
    b.box_index = -1;

    /* UAX #14 over the paragraph text so far: a boundary at the start of this
       run sees the text before it, so a style change inside a word is no break */
    n = pd_text_decode(p->text, p->n_text, &cps, &offs);

    if (n < 0) {
        return PD_ERR_NOMEM;
    }

    brk = (uint8_t*)malloc((size_t)n + 1);

    if (!brk || pd_linebreaks(cps, n, brk)) {
        free(brk);
        free(cps);
        free(offs);
        return PD_ERR_NOMEM;
    }

    for (first = 0; first < n && offs[first] < base; first++) {
    }

    /* automatic hyphenation points in the words of this run */
    if (st->hyph && (hyp = (uint8_t*)calloc((size_t)n + 1, 1)) != NULL) {
        int32_t w0 = first;

        while (w0 < n) {
            int32_t w1;

            while (w0 < n && !is_letter(cps[w0])) {
                w0++;
            }

            for (w1 = w0; w1 < n && is_letter(cps[w1]); w1++) {
            }

            if (w1 > w0 && w1 - w0 <= 250 && (w0 == 0 || !is_letter(cps[w0 - 1]))) {
                uint8_t pts[256];
                int32_t j;

                pd_hyph_points(st->hyph, cps + w0, w1 - w0, pts);

                for (j = 1; j < w1 - w0; j++) {
                    hyp[w0 + j] = pts[j];
                }
            }

            w0 = w1;
        }
    }

    for (ci = first; ci < n; ci++) {
        uint32_t at = offs[ci], at_end = offs[ci + 1], cp = cps[ci], prev = ci > 0 ? cps[ci - 1] : 0;
        int cls = pd_uni_lb(cp), pcls = ci > 0 ? pd_uni_lb(prev) : -1, rc = 0;

        /* a break opportunity before this character (spaces carry their own: their glue) */
        if (ci > 0 && brk[ci] == 1 && pcls != LB_SP && cls != LB_SP && pcls != LB_ZW && prev != 0x00AD &&
                pcls != LB_BK && pcls != LB_CR && pcls != LB_LF && pcls != LB_NL) {
            if (pcls == LB_HY || (pcls == LB_BA && prev >= 0x2010 && prev <= 0x2015)) {
                rc = add_pen(&b, 0, PD_FLAG_FLAGGED | PD_FLAG_EXHYPHEN, at, at);    /* after a hyphen or dash */
            } else if (is_cjk(cp) || is_cjk(prev) || cls == LB_ID || pcls == LB_ID) {
                rc = add_raw_glue(&b, at, st->size / 4, 0);  /* inter-character space that may stretch */
            } else {
                rc = add_pen(&b, 0, PD_FLAG_EXHYPHEN, at, at);   /* any other opportunity, unflagged */
            }
        }

        /* a word that needs contextual shaping goes to the complex shaper as a whole */
        if (!rc && pd_shape_available() && !is_special(cp)) {
            int32_t k = ci + 1, need = pd_shape_needed(cp), ns;
            pd_shaped* sh = NULL;

            while (k < n && !is_special(cps[k]) && brk[k] == 0) {
                need |= pd_shape_needed(cps[k]);
                k++;
            }

            if (need && (ns = pd_shape(st->font, p->text, p->n_text, offs[ci], offs[k], &sh)) > 0) {
                rc = add_shaped(&b, sh, ns, offs[ci], offs[k]);
                free(sh);

                if (rc) {
                    free(brk);
                    free(hyp);
                    free(cps);
                    free(offs);
                    return PD_ERR_NOMEM;
                }

                ci = k - 1;
                continue;
            }

            free(sh);
        }

        if (!rc && hyp && hyp[ci]) {     /* a discretionary hyphen from the patterns */
            rc = add_soft_hyphen(&b, at, at);
        }

        if (!rc) {
            switch (cp) {
                case ' ':
                case '\t': {
                    /* breakable unless UAX #14 forbids a break after this run of spaces */
                    int32_t k = ci + 1;

                    while (k < n && pd_uni_lb(cps[k]) == LB_SP) {
                        k++;
                    }

                    rc = add_space(&b, at, at_end, k < n && brk[k] == 0);
                    break;
                }

                case 0x00A0:
                case 0x202F:
                    rc = add_space(&b, at, at_end, 1);
                    break;

                case 0x200B:        /* zero-width space */
                    rc = add_raw_glue(&b, at, 0, 0);
                    p->items[p->n_items - 1].text_end = at_end;
                    break;

                case 0x00AD:        /* soft hyphen */
                    rc = add_soft_hyphen(&b, at, at_end);
                    break;

                case '\r':
                    if (ci + 1 < n && cps[ci + 1] == '\n') {
                        break;      /* CR LF is one line end */
                    }

                /* fall through */
                case '\n':
                case 0x0B:
                case 0x0C:
                case 0x85:
                case 0x2028:
                case 0x2029:        /* mandatory line break (BK, LF, NL classes) */
                    rc = add_raw_glue(&b, at, 0, 1);

                    if (!rc) {
                        rc = add_pen(&b, -PD_INF_PENALTY, 0, at, at_end);
                    }

                    break;

                default:
                    rc = add_glyph(&b, cp, at, at_end);
                    break;
            }
        }

        if (rc) {
            free(brk);
            free(hyp);
            free(cps);
            free(offs);
            return PD_ERR_NOMEM;
        }
    }

    free(brk);
    free(hyp);
    free(cps);
    free(offs);
    return PD_OK;
}

pd_status pd_para_add_object(pd_para* p, pd_sp width, pd_sp height, pd_sp depth, int32_t user) {
    static const char obj[] = "\xEF\xBF\xBC";  /* U+FFFC OBJECT REPLACEMENT CHARACTER */
    uint32_t at;
    pd_item* it;

    if (!p || width < 0) {
        return PD_ERR_ARG;
    }

    unfinalize(p);
    at = p->n_text;

    if (append_text(p, obj, 3)) {
        return PD_ERR_NOMEM;
    }

    it = new_item(p, PD_ITEM_BOX);

    if (!it) {
        return PD_ERR_NOMEM;
    }

    it->flags = PD_FLAG_OBJECT;
    it->width = width;
    it->height = height;
    it->depth = depth;
    it->user = user;
    it->text_start = at;
    it->text_end = at + 3;
    return PD_OK;
}

pd_status pd_para_add_glue(pd_para* p, pd_sp width, pd_sp stretch, pd_sp shrink, int32_t fil) {
    pd_item* it;

    if (!p || stretch < 0 || shrink < 0) {
        return PD_ERR_ARG;
    }

    unfinalize(p);
    it = new_item(p, PD_ITEM_GLUE);

    if (!it) {
        return PD_ERR_NOMEM;
    }

    it->width = width;
    it->stretch = stretch;
    it->shrink = shrink;
    it->stretch_order = (uint8_t)(fil ? 1 : 0);
    return PD_OK;
}

pd_status pd_para_add_penalty(pd_para* p, int32_t pen, pd_sp width, int32_t flagged) {
    pd_item* it;

    if (!p) {
        return PD_ERR_ARG;
    }

    unfinalize(p);
    it = new_item(p, PD_ITEM_PENALTY);

    if (!it) {
        return PD_ERR_NOMEM;
    }

    it->penalty = pen < -PD_INF_PENALTY ? -PD_INF_PENALTY : pen > PD_INF_PENALTY ? PD_INF_PENALTY : pen;
    it->width = width;
    it->flags = flagged ? PD_FLAG_FLAGGED : 0;
    return PD_OK;
}

pd_status pd_para_set_shape(pd_para* p, int32_t n, const pd_sp* indent, const pd_sp* width) {
    int32_t i;

    if (!p || n < 0 || (n > 0 && (!indent || !width))) {
        return PD_ERR_ARG;
    }

    free(p->shape_indent);
    free(p->shape_width);
    p->shape_indent = p->shape_width = NULL;
    p->n_shape = 0;
    p->shape_dirty = 1;
    p->broken = 0;

    if (n == 0) {
        return PD_OK;
    }

    p->shape_indent = (pd_sp*)malloc(n * sizeof(pd_sp));
    p->shape_width = (pd_sp*)malloc(n * sizeof(pd_sp));

    if (!p->shape_indent || !p->shape_width) {
        return PD_ERR_NOMEM;
    }

    for (i = 0; i < n; i++) {
        if (width[i] <= 0) {
            return PD_ERR_ARG;
        }

        p->shape_indent[i] = indent[i];
        p->shape_width[i] = width[i];
    }

    p->n_shape = n;
    return PD_OK;
}

pd_status pd_para_break(pd_para* p, const pd_params* prm, pd_break_info* info) {
    pd_params def;

    if (!p) {
        return PD_ERR_ARG;
    }

    if (!prm) {
        pd_params_init(&def);
        prm = &def;
    }

    if (prm->width <= 0 && p->n_shape == 0) {
        return PD_ERR_ARG;
    }

    if (!p->finalized) {
        /* TeX's paragraph end: \penalty10000 \parfillskip \penalty-10000 */
        pd_item* it = new_item(p, PD_ITEM_PENALTY);

        if (!it) {
            return PD_ERR_NOMEM;
        }

        it->penalty = PD_INF_PENALTY;
        it->flags = PD_FLAG_FINAL;
        it = new_item(p, PD_ITEM_GLUE);

        if (!it) {
            return PD_ERR_NOMEM;
        }

        it->stretch = 1;
        it->stretch_order = 1;
        it->flags = PD_FLAG_FINAL;
        it = new_item(p, PD_ITEM_PENALTY);

        if (!it) {
            return PD_ERR_NOMEM;
        }

        it->penalty = -PD_INF_PENALTY;
        it->flags = PD_FLAG_FINAL;
        p->finalized = 1;
    }

    /* bidi levels for the whole paragraph (none kept when it is all left-to-right) */
    {
        uint32_t* cps, *offs;
        int32_t ncp = pd_text_decode(p->text ? p->text : "", p->n_text, &cps, &offs), k;
        uint8_t* lev;
        int any = 0, dir = prm->direction == PD_DIR_LTR ? 0 : prm->direction == PD_DIR_RTL ? 1 : -1;

        if (ncp < 0) {
            return PD_ERR_NOMEM;
        }

        lev = (uint8_t*)malloc((size_t)ncp + 1);
        p->para_level = 0;

        if (lev) {
            p->para_level = pd_bidi_levels(cps, ncp, dir, lev);

            for (k = 0; k < ncp; k++) {
                if (lev[k] == 0xFF) {   /* removed controls take the previous level */
                    lev[k] = k > 0 ? lev[k - 1] : (uint8_t)p->para_level;
                }

                any |= lev[k] & 1;
            }
        }

        any |= p->para_level & 1;

        if (lev && any && pd_grow((void**)&p->blev, &p->cap_blev, (int64_t)p->n_text + 1, 1) == 0) {
            for (k = 0; k < ncp; k++) {
                memset(p->blev + offs[k], lev[k], offs[k + 1] - offs[k]);
            }

            p->blev[p->n_text] = (uint8_t)p->para_level;
        } else if (!any) {
            free(p->blev);
            p->blev = NULL;
            p->cap_blev = 0;
        }

        free(lev);
        free(cps);
        free(offs);
    }

    return pd_break_lines(p, prm, info);
}

/* ------------------------------------------------------------------ */
/* output                                                             */
/* ------------------------------------------------------------------ */

int32_t pd_para_line_count(const pd_para* p) {
    return (p && p->broken) ? p->n_lines : 0;
}

uint32_t pd_para_text_length(const pd_para* p) {
    return p ? p->n_text : 0;
}

pd_status pd_para_get_line(const pd_para* p, int32_t i, pd_line* out) {
    if (!p || !out) {
        return PD_ERR_ARG;
    }

    if (!p->broken) {
        return PD_ERR_STATE;
    }

    if (i < 0 || i >= p->n_lines) {
        return PD_ERR_RANGE;
    }

    *out = p->lines[i].pub;
    return PD_OK;
}

pd_status pd_para_get_style(const pd_para* p, int32_t i, pd_style* out) {
    if (!p || !out) {
        return PD_ERR_ARG;
    }

    if (i < 0 || i >= p->n_styles) {
        return PD_ERR_RANGE;
    }

    *out = p->styles[i];
    return PD_OK;
}

/* floor((num * slack) / den) with exact integer arithmetic */
static int64_t share(int64_t num, int64_t slack, int64_t den) {
    int64_t v = num * slack;
    int64_t q = v / den;

    if ((v % den) != 0 && ((v < 0) != (den < 0))) {
        q--;
    }

    return q;
}

#define PD_STOP_GLUE 2  /* internal pd_glyph.kind: caret stop at a space */

/* the glyph for the mirrored form of a character (UAX #9 L4), or the glyph itself */
static uint32_t mirror_glyph(const pd_para* p, const pd_glyph* g) {
    const pd_style* st;
    uint32_t cp, m;
    const unsigned char* s;
    int n, k;

    if (g->style < 0 || g->cluster >= p->n_text) {
        return g->glyph;
    }

    st = &p->styles[g->style];
    s = (const unsigned char*)p->text + g->cluster;
    cp = s[0];
    n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    cp &= n ? 0x3F >> n : 0x7F;

    for (k = 1; k <= n && g->cluster + (uint32_t)k < p->n_text; k++) {
        cp = (cp << 6) | (s[k] & 0x3F);
    }

    m = pd_uni_mirror(cp);

    if (m != cp) {
        uint32_t mg = pd_font_glyph_index(st->font, m);

        return mg ? mg : g->glyph;
    }

    return g->glyph;
}

/*
 * Walk one line: glyphs, objects and (with stops) a caret stop at every
 * glue, in visual order. Glue is set by distributing the slack in
 * proportion to stretch or shrink with cumulative integer rounding, so the
 * pieces sum exactly to the slack. With right-to-left text the atoms are
 * reordered by their bidi levels (L1 for trailing spaces, L2 reversal) and
 * mirrored characters swapped (L4). lev_out, if given, receives each
 * emitted entry's level.
 */
static int32_t walk_line_lv(const pd_para* p, int32_t li, pd_glyph* out, int32_t cap, int stops, uint8_t* lev_out) {
    const pd_lineinfo* L = &p->lines[li];
    int32_t expand = p->last_params.expansion < 0 ? 0 : p->last_params.expansion > 100 ? 100 :
                     p->last_params.expansion;
    int capped = 0;         /* expansion at its limit, the rest of the slack in the glue */
    int64_t box_total = 0;
    int64_t slack = L->slack, cum = 0, den = 0, given = 0;
    int32_t i, n = 0, mode = 0, na = 0, capa = 64;     /* mode: 0 none, 1 finite stretch, 2 fil, 3 shrink */
    pd_sp y = L->pub.baseline;
    pd_glyph* at = (pd_glyph*)malloc((size_t)capa * sizeof(pd_glyph));
    uint8_t* lv = NULL;

    if (!at) {
        return 0;
    }

#define ATOM(...) do { \
        if (na == capa) { \
            pd_glyph* q_ = (pd_glyph*)realloc(at, (size_t)capa * 2 * sizeof(pd_glyph)); \
            if (!q_) { free(at); return 0; } \
            at = q_; capa *= 2; \
        } \
        memset(&at[na], 0, sizeof(pd_glyph)); \
        at[na].scale = 65536; \
        __VA_ARGS__; \
        na++; \
    } while (0)

    if (slack > 0 && L->total_stretch[1] > 0) {
        mode = 2;
        den = L->total_stretch[1];
    } else if (slack > 0 && L->total_stretch[0] > 0) {
        mode = 1;
        den = L->total_stretch[0];
    } else if (slack < 0 && L->total_shrink > 0) {
        mode = 3;
        den = L->total_shrink;
        slack = slack < -den ? -den : slack;
    }

    if (expand > 0 && (mode == 1 || mode == 3)) {   /* glyph boxes take their share of the slack too */
        den = 0;

        for (i = L->start; i < L->end; i++) {
            const pd_item* it = &p->items[i];

            if (it->type == PD_ITEM_GLUE) {
                den += mode == 1 ? (it->stretch_order == 0 ? it->stretch : 0) : it->shrink;
            } else if (it->type == PD_ITEM_BOX && !(it->flags & PD_FLAG_OBJECT)) {
                den += (int64_t)it->width * expand / 1000;
            }
        }

        if (mode == 3) {
            slack = L->slack < -den ? -den : L->slack;
        }

        if (den <= 0) {
            mode = 0;
        } else if (mode == 1 && slack > den) {
            /* looser than the stretch allows: glyphs stop at their limit, the glue takes the rest */
            capped = 1;
            box_total = 0;

            for (i = L->start; i < L->end; i++) {
                const pd_item* it = &p->items[i];

                if (it->type == PD_ITEM_BOX && !(it->flags & PD_FLAG_OBJECT)) {
                    box_total += (int64_t)it->width * expand / 1000;
                }
            }

            den -= box_total;
            slack -= box_total;

            if (den <= 0) {
                mode = 0;
            }
        }
    }

    for (i = L->start; i < L->end; i++) {
        const pd_item* it = &p->items[i];

        if (it->type == PD_ITEM_BOX) {
            if (it->flags & PD_FLAG_OBJECT) {
                ATOM(at[na].cluster = it->text_start; at[na].advance = it->width; at[na].style = -1;
                     at[na].kind = PD_OBJECT; at[na].user = it->user);
            } else {
                uint32_t k;
                int64_t neww = it->width, acc = 0, prev = 0;

                if (expand > 0 && (mode == 1 || mode == 3 || capped) && it->width > 0) {
                    int64_t part = (int64_t)it->width * expand / 1000;

                    if (capped) {
                        neww += part;
                    } else {
                        int64_t next = share(cum + part, slack, den);

                        neww += next - given;
                        given = next;
                        cum += part;
                    }
                }

                for (k = 0; k < it->glyph_count; k++) {
                    const pd_gl* gl = &p->glyphs[it->glyph_start + k];
                    int64_t pos;

                    acc += gl->advance;     /* scaled advances that add up exactly to the new width */
                    pos = it->width > 0 ? acc * neww / it->width : acc;
                    ATOM(at[na].glyph = gl->glyph; at[na].cluster = gl->cluster; at[na].advance = (pd_sp)(pos - prev);
                         at[na].style = it->style; at[na].kind = PD_GLYPH; at[na].x = gl->xoff; at[na].y = gl->yoff;
                         at[na].user = it->style >= 0 ? p->styles[it->style].user : 0;
                         at[na].scale = it->width > 0 ? (int32_t)(neww * 65536 / it->width) : 65536);
                    prev = pos;
                }
            }
        } else if (it->type == PD_ITEM_GLUE) {
            int64_t w = it->width, part = 0;

            if (mode == 1 && it->stretch_order == 0) {
                part = it->stretch;
            } else if (mode == 2 && it->stretch_order == 1) {
                part = it->stretch;
            } else if (mode == 3) {
                part = it->shrink;
            }

            if (part) {
                int64_t next = share(cum + part, slack, den);
                w += next - given;
                given = next;
                cum += part;
            }

            /* glue is always an atom (it moves with reordering); it is emitted only as a stop */
            ATOM(at[na].cluster = it->text_start; at[na].advance = (pd_sp)w; at[na].style = it->style;
                 at[na].kind = PD_STOP_GLUE; at[na].user = it->text_end > it->text_start);
        }
    }

    /* the hyphen of a line broken at a soft hyphen */
    if (L->brk < p->n_items) {
        const pd_item* it = &p->items[L->brk];

        if (it->type == PD_ITEM_PENALTY && it->glyph_count == 1) {
            const pd_gl* gl = &p->glyphs[it->glyph_start];

            ATOM(at[na].glyph = gl->glyph; at[na].cluster = gl->cluster; at[na].advance = gl->advance;
                 at[na].style = it->style; at[na].kind = PD_GLYPH;
                 at[na].user = it->style >= 0 ? p->styles[it->style].user : 0);
        }
    }

#undef ATOM

    /* bidi: L1 (trailing spaces to the paragraph level), L2 (reverse runs), L4 (mirror) */
    if (p->blev && na > 0) {
        int32_t* ord = (int32_t*)malloc((size_t)na * sizeof(int32_t));
        pd_glyph* vis = (pd_glyph*)malloc((size_t)na * sizeof(pd_glyph));
        int maxl = 0, minodd = 255, l;

        lv = (uint8_t*)malloc((size_t)na);

        if (!ord || !vis || !lv) {
            free(ord);
            free(vis);
            free(lv);
            free(at);
            return 0;
        }

        for (i = 0; i < na; i++) {
            lv[i] = at[i].cluster < p->n_text ? p->blev[at[i].cluster] : (uint8_t)p->para_level;
            ord[i] = i;
        }

        for (i = na - 1; i >= 0 && at[i].kind == PD_STOP_GLUE; i--) {
            lv[i] = (uint8_t)p->para_level;
        }

        for (i = 0; i < na; i++) {
            maxl = lv[i] > maxl ? lv[i] : maxl;
            minodd = (lv[i] & 1) && lv[i] < minodd ? lv[i] : minodd;
        }

        for (l = maxl; l >= minodd && l > 0; l--) {
            for (i = 0; i < na; i++) {
                if (lv[ord[i]] >= l) {
                    int32_t a0 = i, b0;

                    while (i < na && lv[ord[i]] >= l) {
                        i++;
                    }

                    for (b0 = i - 1; a0 < b0; a0++, b0--) {
                        int32_t t = ord[a0];

                        ord[a0] = ord[b0];
                        ord[b0] = t;
                    }
                }
            }
        }

        for (i = 0; i < na; i++) {
            vis[i] = at[ord[i]];

            if ((lv[ord[i]] & 1) && vis[i].kind == PD_GLYPH) {
                vis[i].glyph = mirror_glyph(p, &vis[i]);
            }
        }

        for (i = 0; i < na; i++) {  /* levels in visual order */
            ord[i] = lv[ord[i]];
        }

        for (i = 0; i < na; i++) {
            lv[i] = (uint8_t)ord[i];
        }

        free(ord);
        free(at);
        at = vis;
    }

    /* positions, left to right */
    {
        int64_t x = L->pub.x;

        for (i = 0; i < na; i++) {
            int emit = at[i].kind != PD_STOP_GLUE || (stops && at[i].user);

            if (emit) {
                if (n < cap) {
                    out[n] = at[i];
                    out[n].x = (pd_sp)x + at[i].x;     /* plus any shaping offset (y up) */
                    out[n].y = y - at[i].y;

                    if (out[n].kind == PD_STOP_GLUE) {
                        out[n].user = 0;
                    }

                    if (lev_out) {
                        lev_out[n] = lv ? lv[i] : 0;
                    }
                }

                n++;
            }

            x += at[i].advance;
        }
    }

    free(at);
    free(lv);
    return n;
}

static int32_t walk_line(const pd_para* p, int32_t li, pd_glyph* out, int32_t cap, int stops) {
    return walk_line_lv(p, li, out, cap, stops, NULL);
}

pd_status pd_para_get_glyphs(const pd_para* p, int32_t li, pd_glyph* buf, int32_t cap, int32_t* count) {
    int32_t n;

    if (!p || !count || cap < 0) {
        return PD_ERR_ARG;
    }

    if (!p->broken) {
        return PD_ERR_STATE;
    }

    if (li < 0 || li >= p->n_lines) {
        return PD_ERR_RANGE;
    }

    n = walk_line(p, li, buf, buf ? cap : 0, 0);
    *count = n;

    if (!buf) {
        return PD_OK;
    }

    return n <= cap ? PD_OK : PD_ERR_RANGE;
}

/* caret stops of a line, in visual order, only at grapheme cluster boundaries
   (never between a letter and its combining mark); for each stop its bidi
   level and the logical position just after its cluster */
static pd_glyph* line_stops(const pd_para* p, int32_t li, int32_t* n, uint8_t** levels, uint32_t** after) {
    int32_t cnt = walk_line(p, li, NULL, 0, 1), k, o = 0, ncp;
    pd_glyph* buf = (pd_glyph*)malloc(((size_t)cnt + 1) * sizeof(pd_glyph));
    uint8_t* lv = (uint8_t*)malloc((size_t)cnt + 1);
    uint32_t* af = (uint32_t*)malloc(((size_t)cnt + 1) * sizeof(uint32_t));
    uint32_t* cps, *offs;

    if (!buf || !lv || !af) {
        free(buf);
        free(lv);
        free(af);
        return NULL;
    }

    *n = walk_line_lv(p, li, buf, cnt, 1, lv);
    ncp = pd_text_decode(p->text ? p->text : "", p->n_text, &cps, &offs);

    if (ncp < 0) {
        free(buf);
        free(lv);
        free(af);
        return NULL;
    }

    for (k = 0; k < *n; k++) {
        int32_t lo = 0, hi = ncp, e;

        while (lo < hi) {   /* the code point starting at the stop's cluster */
            int32_t mid = (lo + hi) / 2;

            if (offs[mid] < buf[k].cluster) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        if (lo >= ncp || offs[lo] != buf[k].cluster || pd_grapheme_boundary(cps, ncp, lo)) {
            for (e = lo + 1; e < ncp && !pd_grapheme_boundary(cps, ncp, e); e++) {
            }

            buf[o] = buf[k];
            lv[o] = lv[k];
            af[o] = lo >= ncp ? p->n_text : offs[e > ncp ? ncp : e];
            o++;
        } else {    /* a mark: widen the stop of its base, wherever that is */
            int32_t q;

            for (q = o - 1; q >= 0; q--) {
                if (buf[q].cluster < buf[k].cluster) {
                    if (buf[k].x < buf[q].x) {
                        buf[q].advance += buf[q].x - buf[k].x;
                        buf[q].x = buf[k].x;
                    } else {
                        buf[q].advance += buf[k].advance;
                    }

                    break;
                }
            }
        }
    }

    *n = o;
    free(cps);
    free(offs);
    *levels = lv;
    *after = af;
    return buf;
}

/* byte offset at the end of a line's content (before the break) */
static uint32_t line_end_offset(const pd_para* p, int32_t li) {
    const pd_lineinfo* L = &p->lines[li];

    if (li == p->n_lines - 1) {
        return p->n_text;
    }

    return p->items[L->brk].text_start;
}

pd_status pd_para_caret(const pd_para* p, uint32_t off, int32_t* line, pd_sp* x, pd_sp* baseline) {
    int32_t li, n, k, best = -1;
    pd_glyph* st;
    uint8_t* lv;
    uint32_t* af;
    pd_sp cx;
    int rtl_para;

    if (!p || !x) {
        return PD_ERR_ARG;
    }

    if (!p->broken) {
        return PD_ERR_STATE;
    }

    if (off > p->n_text) {
        return PD_ERR_RANGE;
    }

    for (li = 0; li < p->n_lines - 1; li++) {
        if (off < p->lines[li + 1].pub.text_start) {
            break;
        }
    }

    st = line_stops(p, li, &n, &lv, &af);

    if (!st) {
        return PD_ERR_NOMEM;
    }

    /* the stop of the character at (or logically right after) off */
    rtl_para = p->para_level & 1;
    cx = rtl_para ? p->lines[li].pub.x : p->lines[li].pub.x + p->lines[li].pub.width;

    if (off < line_end_offset(p, li) || li == p->n_lines - 1) {
        for (k = 0; k < n; k++) {
            if (st[k].cluster >= off && (best < 0 || st[k].cluster < st[best].cluster)) {
                best = k;
            }
        }

        if (best >= 0) {    /* the logical start of a character: its left edge, or its right edge in RTL */
            cx = (lv[best] & 1) ? st[best].x + st[best].advance : st[best].x;
        }
    }

    free(st);
    free(lv);
    free(af);
    *x = cx;

    if (line) {
        *line = li;
    }

    if (baseline) {
        *baseline = p->lines[li].pub.baseline;
    }

    return PD_OK;
}

pd_status pd_para_hit_test(const pd_para* p, pd_sp x, pd_sp y, uint32_t* off, int32_t* line) {
    int32_t li, n, k;
    pd_glyph* st;
    uint8_t* lv;
    uint32_t* af;
    int64_t best_d;
    uint32_t best;
    pd_sp end_x;

    if (!p || !off) {
        return PD_ERR_ARG;
    }

    if (!p->broken) {
        return PD_ERR_STATE;
    }

    for (li = 0; li < p->n_lines - 1; li++) {
        if (y <= p->lines[li].pub.baseline + p->lines[li].pub.descent) {
            break;
        }
    }

    st = line_stops(p, li, &n, &lv, &af);

    if (!st) {
        return PD_ERR_NOMEM;
    }

    /* the line end is a stop too: on the right of a left-to-right line, the left of a right-to-left one */
    end_x = (p->para_level & 1) ? p->lines[li].pub.x : p->lines[li].pub.x + p->lines[li].pub.width;
    best = line_end_offset(p, li);
    best_d = (int64_t)x - end_x;
    best_d = best_d < 0 ? -best_d : best_d;

    for (k = 0; k < n; k++) {
        int rtl = lv[k] & 1;
        uint32_t left = rtl ? af[k] : st[k].cluster, right = rtl ? st[k].cluster : af[k];
        int64_t d = (int64_t)x - st[k].x;

        if (right > line_end_offset(p, li) && li < p->n_lines - 1) {
            right = line_end_offset(p, li);
        }

        if (left > line_end_offset(p, li) && li < p->n_lines - 1) {
            left = line_end_offset(p, li);
        }

        d = d < 0 ? -d : d;

        if (d < best_d || (d == best_d && left < best)) {
            best_d = d;
            best = left;
        }

        d = (int64_t)x - ((int64_t)st[k].x + st[k].advance);
        d = d < 0 ? -d : d;

        if (d < best_d) {
            best_d = d;
            best = right;
        }
    }

    free(st);
    free(lv);
    free(af);
    *off = best;

    if (line) {
        *line = li;
    }

    return PD_OK;
}

const char* pd_version(void) {
    return "0.1.0";
}

const char* pd_status_string(pd_status s) {
    switch (s) {
        case PD_OK:
            return "ok";

        case PD_ERR_ARG:
            return "invalid argument";

        case PD_ERR_NOMEM:
            return "out of memory";

        case PD_ERR_IO:
            return "i/o error";

        case PD_ERR_FONT:
            return "malformed or unsupported font";

        case PD_ERR_RANGE:
            return "out of range";

        case PD_ERR_STATE:
            return "invalid state";

        case PD_ERR_FORMAT:
            return "malformed document data";
    }

    return "unknown error";
}
