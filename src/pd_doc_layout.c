/*
 * Parade layout bridge: a document paragraph as a pd_para
 *
 * Every inline object becomes a pd_para object, which also occupies
 * U+FFFC, so byte offsets are identical in the document and in the
 * layout: a caret or hit-test result needs no translation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include "pd_doc_internal.h"
#include "pd_unidata.h"

static const pd_font* resolve_font(const pd_doc* d, const pd_char_props* cp) {
    const pd_font* f = NULL;

    if (d->resolver) {
        f = d->resolver(d->resolver_user, cp->family, cp->weight, cp->italic);
    }

    return f ? f : d->default_font;
}

pd_status pd_doc_run_style(const pd_doc* d, pd_block_id para, pd_format_id fmt, pd_style* st, pd_char_props* cp) {
    const pd_font* f;

    if (pd_doc_format_resolve(d, para, fmt, cp) != PD_OK) {
        return PD_ERR_FORMAT;
    }

    pd_doc_markup_props(d, cp);

    f = resolve_font(d, cp);

    if (!f) {
        return PD_ERR_STATE;    /* no resolver answer and no default font */
    }

    /* super/subscripts are smaller in the layout too, so their advances shrink */
    pd_style_init(st, f, cp->shift == PD_SHIFT_SUPER || cp->shift == PD_SHIFT_SUB ? cp->size * 7 / 10 : cp->size);
    st->kerning = cp->kerning;
    st->color = cp->color;
    st->user = (int32_t)fmt;
    st->text_case = cp->caps || cp->small_caps ? 1 : 0;
    st->letter_space = cp->letter_space;
    st->hidden = cp->hidden;
    return PD_OK;
}

/* the style of a list paragraph's label: its first characters', with what
   the list level says of its label's font over them */
pd_status pd_doc_label_style(const pd_doc* d, pd_block_id para, pd_style* st) {
    const blk* b = pd_doc_blk(d, para);
    const pd_list_level* L;
    pd_char_props cp;
    const pd_font* f;

    if (!b || pd_doc_run_style(d, para, b->st.nruns ? b->st.runs[0].format : b->st.empty_format, st, &cp) != PD_OK) {
        return PD_ERR_STATE;
    }

    if (!b->st.list || (int32_t)b->st.list > d->nlists) {
        return PD_OK;
    }

    L = &d->lists[b->st.list - 1].lv[b->st.list_level < d->lists[b->st.list - 1].n ? b->st.list_level :
                                      d->lists[b->st.list - 1].n - 1];

    if (!L->label_family[0] && !L->label_size && !L->label_weight && !L->label_italic && !L->label_color) {
        return PD_OK;
    }

    if (L->label_family[0]) {
        snprintf(cp.family, sizeof(cp.family), "%s", L->label_family);
    }

    cp.size = L->label_size > 0 ? L->label_size : cp.size;
    cp.weight = L->label_weight > 0 ? L->label_weight : cp.weight;
    cp.italic = L->label_italic ? L->label_italic > 0 : cp.italic;
    cp.color = L->label_color ? L->label_color : cp.color;

    if ((f = resolve_font(d, &cp)) != NULL) {
        int32_t user = st->user;

        pd_style_init(st, f, cp.size);
        st->color = cp.color;
        st->user = user;
    }

    return PD_OK;
}

/* a style for characters described outright (a drawing's text boxes) */
pd_status pd_doc_cp_style(const pd_doc* d, const pd_char_props* cp, pd_style* st) {
    const pd_font* f = resolve_font(d, cp);

    if (!f) {
        return PD_ERR_STATE;
    }

    pd_style_init(st, f, cp->shift == PD_SHIFT_SUPER || cp->shift == PD_SHIFT_SUB ? cp->size * 7 / 10 : cp->size);
    st->color = cp->color;
    st->kerning = cp->kerning;
    st->user = -1;
    return PD_OK;
}

/* the style of the document's Normal text: line numbers and the like */
pd_status pd_doc_default_style(const pd_doc* d, pd_style* st) {
    pd_char_props cp;
    const pd_font* f;

    if (pd_doc_style_resolve(d, pd_doc_style_find(d, "Normal"), NULL, &cp) != PD_OK || (f = resolve_font(d, &cp)) == NULL) {
        return PD_ERR_STATE;
    }

    pd_style_init(st, f, cp.size);
    st->color = cp.color;
    st->user = -1;
    return PD_OK;
}

/* width of n copies of a character in a style: placeholder fields, tabs */
static pd_sp char_width(const pd_style* st, uint32_t cp, int n) {
    pd_font_metrics m;
    int32_t adv;

    pd_font_get_metrics(st->font, &m);
    adv = pd_font_glyph_advance(st->font, pd_font_glyph_index(st->font, cp));
    return (pd_sp)((int64_t)adv * st->size / m.units_per_em) * n;
}

void pd_doc_effective_pp(const pd_doc* d, const blk* b, pd_para_props* pp, pd_sp* label_x) {
    const bstate* s = &b->st;

    pd_doc_style_resolve(d, s->style, pp, NULL);

    /* direct properties over the style's */
#define OVER(bit, f) if (s->pp.mask & (bit)) { pp->f = s->pp.f; }
    OVER(PD_PP_ALIGN, align);
    OVER(PD_PP_INDENT_LEFT, indent_left);
    OVER(PD_PP_INDENT_RIGHT, indent_right);
    OVER(PD_PP_INDENT_FIRST, indent_first);
    OVER(PD_PP_SPACE_BEFORE, space_before);
    OVER(PD_PP_SPACE_AFTER, space_after);
    OVER(PD_PP_LINE_SPACING, line_spacing);
    OVER(PD_PP_KEEP_NEXT, keep_with_next);
    OVER(PD_PP_KEEP_LINES, keep_lines);
    OVER(PD_PP_WIDOWS, widows);
    OVER(PD_PP_ORPHANS, orphans);
    OVER(PD_PP_BREAK_BEFORE, page_break_before);
    OVER(PD_PP_HYPHENATE, hyphenate);
    OVER(PD_PP_BREAK_MODE, break_mode);
    OVER(PD_PP_DIRECTION, direction);
    OVER(PD_PP_CONTEXTUAL, contextual);
    OVER(PD_PP_SNAP_GRID, snap_grid);
    OVER(PD_PP_SHADING, shading);

    if (s->pp.mask & PD_PP_BORDER) {
        pp->border_color = s->pp.border_color;
        pp->border_width = s->pp.border_width;
        pp->border_sides = s->pp.border_sides;
        pp->border_space = s->pp.border_space;
    }

    if (s->pp.mask & PD_PP_TABS) {
        pp->ntabs = s->pp.ntabs;
        memcpy(pp->tabs, s->pp.tabs, sizeof(pp->tabs));
        pp->tab_interval = s->pp.tab_interval;
    }
#undef OVER

    if (label_x) {
        *label_x = 0;
    }

    /* block quotes inside block quotes: one step in for each beyond the one its QUOTE role has */
    if (s->at.quote_depth > (s->role == PD_ROLE_QUOTE ? 1 : 0)) {
        pp->indent_left += PD_PT(24) * (s->at.quote_depth - (s->role == PD_ROLE_QUOTE ? 1 : 0));
    }

    /* a list item's text sits at the level's indent; its label hangs to the left */
    if (s->list && (int32_t)s->list <= d->nlists) {
        const pd_list_level* L = &d->lists[s->list - 1].lv[s->list_level];

        pp->indent_left += L->indent;

        if (label_x) {
            *label_x = pp->indent_left - L->hanging;
        }
    }
}

/* advance width of a short UTF-8 text in a style, without kerning */
static pd_sp text_width(const pd_style* st, const char* s) {
    pd_font_metrics m;
    const unsigned char* p = (const unsigned char*)s;
    int64_t units = 0;

    pd_font_get_metrics(st->font, &m);

    while (*p) {
        uint32_t cp = *p++;

        if (cp >= 0xC0) {   /* a multi-byte sequence (input is valid UTF-8) */
            int n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : 1;

            cp &= 0x3F >> n;

            while (n-- && (*p & 0xC0) == 0x80) {
                cp = (cp << 6) | (*p++ & 0x3F);
            }
        }

        units += pd_font_glyph_advance(st->font, pd_font_glyph_index(st->font, cp));
    }

    return (pd_sp)(units * st->size / m.units_per_em);
}

/*
 * Add text in a style, switching to a fallback font for characters the
 * style's font lacks. Combining marks stay with their base character's
 * font, spaces with the run's own font.
 */
static pd_status add_with_fallback(const pd_doc* d, pd_para* out, const char* text, uint32_t len, const pd_style* ps) {
    uint32_t* cps, *offs, seg = 0;
    int32_t n, i, cur = -1;     /* -1: the run's font, else a fallback index */
    pd_status st = PD_OK;

    if (d->nfallback == 0) {
        return pd_para_add_text(out, text, len, ps);
    }

    n = pd_text_decode(text, len, &cps, &offs);

    if (n < 0) {
        return PD_ERR_NOMEM;
    }

    for (i = 0; i <= n && st == PD_OK; i++) {
        int32_t want = cur;

        if (i < n) {
            uint32_t c = cps[i];

            if (c == ' ' || c == '\t' || c < 32 || c == 0x00A0 || c == 0x200B || c == 0x00AD) {
                want = -1;
            } else if (pd_uni_gcb(c) == GCB_EXTEND || pd_uni_gcb(c) == GCB_ZWJ) {
                want = cur;     /* marks follow their base */
            } else if (pd_font_glyph_index(ps->font, c)) {
                want = -1;
            } else {
                int32_t k;

                want = -1;

                for (k = 0; k < d->nfallback; k++) {
                    if (pd_font_glyph_index(d->fallback[k], c)) {
                        want = k;
                        break;
                    }
                }
            }
        }

        if (i == n || (want != cur && i > 0)) {
            pd_style fs = *ps;

            if (cur >= 0) {
                fs.font = d->fallback[cur];
            }

            if (offs[i] > seg) {
                st = pd_para_add_text(out, text + seg, offs[i] - seg, &fs);
            }

            seg = offs[i < n ? i : n];
        }

        cur = want;
    }

    free(cps);
    free(offs);
    return st;
}

/* the script slot a character's face comes from: 1 East Asian (CJK ideographs, kana, hangul, bopomofo,
   full-width forms), 2 complex (right-to-left, Indic, Southeast Asian), 0 the rest; -1 neutral (spaces,
   digits, punctuation of the basic Latin block), which goes with what is around it */
static int script_slot(uint32_t c) {
    if (c < 0x80) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ? 0 : -1;
    }

    if ((c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x2FF0 && c <= 0x9FFF) ||
            (c >= 0xA960 && c <= 0xA97F) || (c >= 0xAC00 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFAFF) ||
            (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF)) {
        return 1;
    }

    if ((c >= 0x0590 && c <= 0x08FF) || (c >= 0x0900 && c <= 0x0DFF) || (c >= 0x0E00 && c <= 0x0FFF) ||
            (c >= 0x1000 && c <= 0x109F) || (c >= 0x1780 && c <= 0x17FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
            (c >= 0xFE70 && c <= 0xFEFF)) {
        return 2;
    }

    return 0;
}

/* the style of a run's text in another script: its own face (and, complex scripts, size, weight, italic), the
   rest of the run's style kept; 0 when the run says nothing for the script */
static int script_style(const pd_doc* d, const pd_char_props* cp, const pd_style* ps, int slot, pd_style* out) {
    pd_char_props c = *cp;
    const pd_font* f;
    pd_sp size;

    if (slot == 1) {
        if (!cp->family_ea[0] || !strcmp(cp->family_ea, cp->family)) {
            return 0;
        }

        memcpy(c.family, cp->family_ea, sizeof(c.family));
    } else {
        if ((!cp->family_cs[0] || !strcmp(cp->family_cs, cp->family)) && (!cp->size_cs || cp->size_cs == cp->size) &&
                (!cp->weight_cs || cp->weight_cs == cp->weight) && (cp->italic_cs < 0 || cp->italic_cs == cp->italic)) {
            return 0;
        }

        if (cp->family_cs[0]) {
            memcpy(c.family, cp->family_cs, sizeof(c.family));
        }

        c.weight = cp->weight_cs ? cp->weight_cs : cp->weight;
        c.italic = cp->italic_cs >= 0 ? cp->italic_cs : cp->italic;
        c.size = cp->size_cs ? cp->size_cs : cp->size;
    }

    if ((f = resolve_font(d, &c)) == NULL) {
        return 0;
    }

    size = c.shift == PD_SHIFT_SUPER || c.shift == PD_SHIFT_SUB ? c.size * 7 / 10 : c.size;
    pd_style_init(out, f, size);
    out->kerning = ps->kerning;
    out->color = ps->color;
    out->user = ps->user;
    out->text_case = ps->text_case;
    out->letter_space = ps->letter_space;
    out->hidden = ps->hidden;
    out->hyph = ps->hyph;
    return 1;
}

/* Add a run's text, its East Asian and complex-script stretches in their own faces when the run names them
   (Word's font slots); the rest as small capitals or plainly */
static pd_status add_scripts(const pd_doc* d, pd_para* out, const char* text, uint32_t len, const pd_style* ps,
                             const pd_char_props* cp, int small_caps);

/* Add text in small capitals: the lowercase letters as capitals at 4/5 of
   the size (the text itself keeps its case), the rest at full size. */
static pd_status add_small_caps(const pd_doc* d, pd_para* out, const char* text, uint32_t len, const pd_style* ps) {
    uint32_t* cps, *offs, seg = 0;
    int32_t n, i, cur = -1;
    pd_status st = PD_OK;

    n = pd_text_decode(text, len, &cps, &offs);

    if (n < 0) {
        return PD_ERR_NOMEM;
    }

    for (i = 0; i <= n && st == PD_OK; i++) {
        int32_t low = i < n ? pd_uni_upper(cps[i]) != cps[i] : -1;

        if (i == n || (low != cur && i > 0)) {
            pd_style fs = *ps;

            if (cur == 1) {
                fs.size = ps->size * 4 / 5;
            }

            if (offs[i] > seg) {
                st = add_with_fallback(d, out, text + seg, offs[i] - seg, &fs);
            }

            seg = offs[i < n ? i : n];
        }

        cur = low;
    }

    free(cps);
    free(offs);
    return st;
}

static pd_status add_scripts(const pd_doc* d, pd_para* out, const char* text, uint32_t len, const pd_style* ps,
                             const pd_char_props* cp, int small_caps) {
    pd_style st[3];
    int have[3] = { 1, 0, 0 };
    uint32_t* cps, *offs, seg = 0;
    int32_t n, i, cur = 0;
    pd_status r = PD_OK;

    st[0] = *ps;
    have[1] = script_style(d, cp, ps, 1, &st[1]);
    have[2] = script_style(d, cp, ps, 2, &st[2]);

    if (!have[1] && !have[2]) {     /* one face for all of it */
        return small_caps ? add_small_caps(d, out, text, len, ps) : add_with_fallback(d, out, text, len, ps);
    }

    if ((n = pd_text_decode(text, len, &cps, &offs)) < 0) {
        return PD_ERR_NOMEM;
    }

    for (i = 0; i <= n && r == PD_OK; i++) {
        int want = cur;

        if (i < n) {
            int sl = script_slot(cps[i]);

            want = sl < 0 ? cur : have[sl] ? sl : 0;
        }

        if (i == n || (want != cur && i > 0)) {
            if (offs[i] > seg) {
                r = cur == 0 && small_caps ? add_small_caps(d, out, text + seg, offs[i] - seg, &st[0]) :
                    add_with_fallback(d, out, text + seg, offs[i] - seg, &st[cur]);
            }

            seg = offs[i < n ? i : n];
        }

        cur = want;
    }

    free(cps);
    free(offs);
    return r;
}

/* the registered patterns whose language prefix best matches a tag */
static const pd_hyph* find_hyph(const pd_doc* d, const char* lang) {
    const pd_hyph* best = NULL;
    size_t blen = 0;
    int32_t i;

    for (i = 0; i < d->nhyphs; i++) {
        const char* p = d->hyphs[i].lang;
        size_t n = strlen(p), k;

        for (k = 0; k < n && lang[k] && tolower((unsigned char)lang[k]) == tolower((unsigned char)p[k]); k++) {
        }

        if (k == n && (lang[n] == 0 || lang[n] == '-' || lang[n] == '_' || n == 0) && (!best || n > blen)) {
            best = d->hyphs[i].hyph;
            blen = n;
        }
    }

    return best;
}

pd_status pd_doc_para_build(const pd_doc* d, pd_block_id para, pd_sp column, pd_para* out, pd_params* prm) {
    return pd_doc_para_build_ex(d, para, column, out, prm, NULL, NULL);
}

pd_status pd_doc_para_build_ex(const pd_doc* d, pd_block_id para, pd_sp column, pd_para* out, pd_params* prm,
                               pd_field_fn fn, void* user) {
    blk* b = pd_doc_blk(d, para);
    const bstate* s;
    pd_para_props pp;
    pd_sp ind[2], wid[2], w;
    int32_t r, k = 0;
    pd_status st;

    if (!b || b->kind != PD_BLOCK_PARAGRAPH || !out || !prm || column <= 0) {
        return PD_ERR_ARG;
    }

    s = &b->st;
    pd_doc_effective_pp(d, b, &pp, NULL);

    pd_para_clear(out);
    pd_params_init(prm);
    prm->align = pp.align;
    prm->mode = pp.break_mode;
    prm->line_spacing = pp.line_spacing;
    prm->direction = pp.direction;

    {   /* a word processor's section: full line boxes, so that paragraphs stack as lines do */
        const blk* sb = b;
        int32_t hops;

        for (hops = 0; sb && sb->kind != PD_BLOCK_SECTION && sb->parent && hops < 64; hops++) {
            sb = pd_doc_blk(d, sb->parent);
        }

        if (!sb || sb->kind != PD_BLOCK_SECTION) {  /* a story: the first section's */
            const blk* root = pd_doc_blk(d, PD_ROOT_ID);

            sb = root && root->nkids > 0 ? pd_doc_blk(d, root->kids[0]) : NULL;
        }

        prm->full_lines = sb && sb->kind == PD_BLOCK_SECTION && sb->st.sp.add_spacing;
        prm->line_grid = sb && sb->kind == PD_BLOCK_SECTION && pp.snap_grid ? sb->st.sp.line_pitch : 0;
    }

    if (s->role == PD_ROLE_EQUATION && !(s->pp.mask & PD_PP_ALIGN)) {
        prm->align = PD_ALIGN_CENTER;   /* a display equation sits in the middle */
    }

    if (s->role != PD_ROLE_CODE && s->role != PD_ROLE_RAW) {
        prm->protrusion = d->protrusion;
        prm->expansion = d->expansion;
    }

    if (column <= 0) {
        return PD_ERR_RANGE;
    }

    w = column - pp.indent_left - pp.indent_right;

    {   /* indents wider than the column (a narrow table cell): squeezed, as Word does, to leave the text some room */
        pd_sp minw = column < PD_PT(24) ? column : PD_PT(12);

        if (w < minw) {
            pd_sp over = minw - w, cut = pp.indent_right > 0 ? (pp.indent_right < over ? pp.indent_right : over) : 0;

            pp.indent_right -= cut;
            over -= cut;
            pp.indent_left -= over;
            w = minw;
        }

        if (w - pp.indent_first < minw) {
            pp.indent_first = w - minw;
        }
    }

    prm->width = w;
    ind[0] = pp.indent_left + pp.indent_first;
    wid[0] = w - pp.indent_first;
    ind[1] = pp.indent_left;
    wid[1] = w;
    pd_para_set_shape(out, 2, ind, wid);
    pd_para_set_tabs(out, pp.ntabs, pp.tabs, pp.tab_interval, 0);   /* measured from the column's edge */
    out->mirror_w = column;     /* (right to left: indents and tabs from the start edge, the right one) */

    for (r = 0; r < s->nruns; r++) {
        uint32_t pos = s->runs[r].start, end = s->runs[r].end;
        pd_style ps;
        pd_char_props cp;

        st = pd_doc_run_style(d, para, s->runs[r].format, &ps, &cp);

        if (st != PD_OK) {
            return st;
        }

        if (pp.hyphenate && d->nhyphs) {
            cp.lang[sizeof(cp.lang) - 1] = 0;
            ps.hyph = find_hyph(d, cp.lang);
        }

        while (pos < end) {
            uint32_t stop = end;

            while (k < s->ninl && s->inl[k].offset < pos) {
                k++;
            }

            if (k < s->ninl && s->inl[k].offset < end) {
                stop = s->inl[k].offset;
            }

            if (stop > pos && (st = add_scripts(d, out, s->text + pos, stop - pos, &ps, &cp,
                                                cp.small_caps && !cp.caps && !cp.hidden)) != PD_OK) {
                return st;
            }

            pos = stop;

            if (k < s->ninl && s->inl[k].offset == pos && pos < end) {
                const pd_inline* o = &s->inl[k].obj;
                pd_sp ow = o->width, oh = o->height, od = o->depth;

                switch (o->kind) {
                    case PD_INLINE_IMAGE:
                        pd_doc_image_size(d, o, &ow, &oh);
                        break;

                    case PD_INLINE_EQUATION:    /* typeset with the math font: its real size */
                        if (d->math_font && o->source && o->source_len > 0) {
                            pd_math_metrics mm;
                            int32_t cnt;

                            if (pd_math_layout(d->math_font, cp.size, o->source, (size_t)o->source_len,
                                               s->role == PD_ROLE_EQUATION, NULL, 0, &cnt, &mm) == PD_OK) {
                                ow = mm.width;
                                oh = mm.height;
                                od = mm.depth;
                            }
                        }

                        break;

                    case PD_INLINE_FIELD:
                    case PD_INLINE_FOOTNOTE: {  /* its value's width, or a placeholder until known */
                        char val[96];

                        if (fn && fn(user, b, &s->inl[k], val, sizeof(val))) {
                            ow = text_width(&ps, val);

                            if (o->kind == PD_INLINE_FOOTNOTE) {
                                ow = ow * 7 / 10;   /* drawn smaller and raised */
                            }
                        } else {
                            ow = char_width(&ps, '0', o->kind == PD_INLINE_FIELD ? 2 : 1);
                        }

                        oh = cp.size * 7 / 10;
                        od = 0;
                        break;
                    }

                    case PD_INLINE_TAB:
                        ow = char_width(&ps, ' ', 4);
                        oh = od = 0;
                        break;

                    case PD_INLINE_LINK:
                    case PD_INLINE_BOOKMARK:
                    case PD_INLINE_RAW:
                    case PD_INLINE_CONTROL:
                        ow = oh = od = 0;
                        break;

                    case PD_INLINE_RUBY:    /* no width; the room above the line its guide needs */
                        ow = od = 0;
                        oh = o->source_len > 0 ? (o->depth > 0 ? o->depth : cp.size) +
                             (o->height > 0 ? o->height : cp.size / 2) * 9 / 10 : 0;
                        break;
                }

                if ((st = pd_para_add_object(out, ow, oh, od, k)) != PD_OK) {
                    return st;
                }

                pos += 3;
                k++;
            }
        }
    }

    if (s->len == 0) {  /* an empty paragraph still has a line of the right height */
        pd_style ps;
        pd_char_props cp;

        st = pd_doc_run_style(d, para, s->empty_format, &ps, &cp);

        if (st != PD_OK) {
            return st;
        }

        pd_para_add_text(out, "", 0, &ps);
    }

    return PD_OK;
}
