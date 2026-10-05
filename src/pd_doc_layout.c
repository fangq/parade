/*
 * Parade layout bridge: a document paragraph as a pd_para
 *
 * Every inline object becomes a pd_para object, which also occupies
 * U+FFFC, so byte offsets are identical in the document and in the
 * layout: a caret or hit-test result needs no translation.
 */

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

    f = resolve_font(d, cp);

    if (!f) {
        return PD_ERR_STATE;    /* no resolver answer and no default font */
    }

    /* super/subscripts are smaller in the layout too, so their advances shrink */
    pd_style_init(st, f, cp->shift == PD_SHIFT_SUPER || cp->shift == PD_SHIFT_SUB ? cp->size * 7 / 10 : cp->size);
    st->kerning = cp->kerning;
    st->color = cp->color;
    st->user = (int32_t)fmt;
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

    if (s->pp.mask & PD_PP_TABS) {
        pp->ntabs = s->pp.ntabs;
        memcpy(pp->tabs, s->pp.tabs, sizeof(pp->tabs));
        pp->tab_interval = s->pp.tab_interval;
    }
#undef OVER

    if (label_x) {
        *label_x = 0;
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

    if (s->role == PD_ROLE_EQUATION && !(s->pp.mask & PD_PP_ALIGN)) {
        prm->align = PD_ALIGN_CENTER;   /* a display equation sits in the middle */
    }

    if (s->role != PD_ROLE_CODE) {
        prm->protrusion = d->protrusion;
        prm->expansion = d->expansion;
    }

    w = column - pp.indent_left - pp.indent_right;

    if (w <= 0 || w - pp.indent_first <= 0) {
        return PD_ERR_RANGE;
    }

    prm->width = w;
    ind[0] = pp.indent_left + pp.indent_first;
    wid[0] = w - pp.indent_first;
    ind[1] = pp.indent_left;
    wid[1] = w;
    pd_para_set_shape(out, 2, ind, wid);
    pd_para_set_tabs(out, pp.ntabs, pp.tabs, pp.tab_interval, 0);   /* measured from the column's edge */

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

            if (stop > pos && (st = add_with_fallback(d, out, s->text + pos, stop - pos, &ps)) != PD_OK) {
                return st;
            }

            pos = stop;

            if (k < s->ninl && s->inl[k].offset == pos && pos < end) {
                const pd_inline* o = &s->inl[k].obj;
                pd_sp ow = o->width, oh = o->height, od = o->depth;

                switch (o->kind) {
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
                        ow = oh = od = 0;
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
