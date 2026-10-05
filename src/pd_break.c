/*
 * Parade line breaking: integer total-fit (Knuth-Plass) and first-fit
 *
 * The total-fit search is written in "pull" form: for every breakpoint b
 * we scan back over earlier breakpoints a that can start a line ending at
 * b. The state of b then depends only on items up to b, which makes the
 * search restartable: after an edit at item k, all states before k are
 * reused and only the tail is recomputed.
 *
 * States are kept per (breakpoint, line class, fitness class). Line
 * classes distinguish the lines of a \parshape (and the indented first
 * line); all later lines share the last class, as in TeX.
 *
 * All arithmetic is integer: widths in sp, demerits in int64. Ties are
 * broken by scan order (later start, lower class), so results are
 * identical on every platform.
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"

#define DEM_INF ((int64_t)1 << 62)
#define DEM_OVERFULL ((int64_t)1 << 50)   /* cost of an overfull emergency line */
#define BAD_MAX 30000000                  /* uncapped badness ceiling, keeps d^2 < 2^50 */
#define NFIT 4

typedef struct {
    pd_para* p;
    const pd_params* prm;
    int32_t K;              /* line classes */
    int32_t* bp_start;      /* first content item of a line starting at bp */
    uint8_t* oldmark;       /* item is a break of the previous layout (hysteresis) */
    pd_sp maxw;
    int ragged;
    int protrude;
    int32_t expand;         /* per-mille */
} ctx_t;

static int64_t sat_add(int64_t a, int64_t b) {
    int64_t s = a + b;
    return s > DEM_INF ? DEM_INF : s;
}

static int64_t badness(int64_t t, int64_t s, int tex) {
    int64_t r;

    if (t == 0) {
        return 0;
    }

    if (tex) {      /* TeX's approximation of 100 (t/s)^3, capped at 10000 */
        if (s <= 0) {
            return 10000;
        }

        if (t <= 7230584) {
            r = (t * 297) / s;
        } else if (s >= 1663497) {
            r = t / (s / 297);
        } else {
            r = t;
        }

        return r > 1290 ? 10000 : (r * r * r + 0x20000) / 0x40000;
    }

    /* uncapped cubic: a ratio of 50 must cost more than a ratio of 3 */
    if (s <= 0) {
        return BAD_MAX;
    }

    r = t * 1000 / s;

    if (r > 100000) {
        r = 100000;
    }

    r = r * r * r / 10000000;
    return r > BAD_MAX ? BAD_MAX : r;
}

static pd_sp line_width(const ctx_t* c, int32_t lc) {
    const pd_para* p = c->p;
    pd_sp w = p->n_shape ? p->shape_width[lc < p->n_shape ? lc : p->n_shape - 1] : c->prm->width;
    return lc == 0 ? w - c->prm->indent : w;
}

static pd_sp line_x(const ctx_t* c, int32_t lc) {
    const pd_para* p = c->p;
    pd_sp x = p->n_shape ? p->shape_indent[lc < p->n_shape ? lc : p->n_shape - 1] : 0;
    return lc == 0 ? x + c->prm->indent : x;
}

static int is_breakpoint(const pd_para* p, const pd_params* prm, int32_t i) {
    const pd_item* it = &p->items[i];

    if (it->type == PD_ITEM_GLUE) {
        return i > 0 && p->items[i - 1].type == PD_ITEM_BOX && !(it->flags & PD_FLAG_NOBREAK);
    }

    if (it->type == PD_ITEM_PENALTY) {
        return pd_item_penalty(it, prm) < PD_INF_PENALTY;
    }

    return 0;
}

static int is_forced(const pd_para* p, const pd_params* prm, int32_t item) {
    return item >= 0 && p->items[item].type == PD_ITEM_PENALTY &&
           pd_item_penalty(&p->items[item], prm) <= -PD_INF_PENALTY;
}

static int is_flagged(const pd_para* p, int32_t item) {
    return item >= 0 && p->items[item].type == PD_ITEM_PENALTY && (p->items[item].flags & PD_FLAG_FLAGGED);
}

/* line measures between breakpoints a and b */
typedef struct {
    int64_t nat, st, fil, sh;
    int64_t lp;             /* protrusion into the left margin */
} measure_t;

/* how far a character may hang into the margin, per-mille of its width (microtype's defaults) */
static int protrude_factor(uint32_t cp, int right) {
    if (right) {
        switch (cp) {
            case '.':
            case '-':
            case 0x2010:
            case 0x2011:
            case 0x00AD:
            case 0x2019:
            case 0x201D:
                return 700;

            case ',':
            case ':':
            case '"':
            case '\'':
                return 500;

            case ';':
            case 0x2013:
                return 300;

            case '!':
            case '?':
            case 0x2014:
                return 200;

            case ')':
            case ']':
            case 'A':
            case 'T':
            case 'V':
            case 'W':
            case 'Y':
            case 'v':
            case 'w':
            case 'y':
                return 50;
        }

        return 0;
    }

    switch (cp) {
        case 0x2018:
        case 0x201C:
        case '"':
        case '\'':
            return 500;

        case '(':
        case '[':
            return 100;

        case 'A':
        case 'T':
        case 'V':
        case 'W':
        case 'Y':
        case 'J':
        case 'v':
        case 'w':
        case 'y':
            return 50;
    }

    return 0;
}

static uint32_t cp_at(const pd_para* p, uint32_t off) {
    const unsigned char* s;
    uint32_t cp;
    int n, k;

    if (!p->text || off >= p->n_text) {
        return 0;
    }

    s = (const unsigned char*)p->text + off;
    cp = s[0];
    n = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
    cp &= n ? 0x3F >> n : 0x7F;

    for (k = 1; k <= n && off + (uint32_t)k < p->n_text; k++) {
        cp = (cp << 6) | (s[k] & 0x3F);
    }

    return cp;
}

/* protrusion of a glyph box at one side, in sp */
static int64_t box_protrusion(const pd_para* p, const pd_item* it, int right) {
    const pd_gl* gl;

    if (it->type != PD_ITEM_BOX || (it->flags & PD_FLAG_OBJECT) || it->glyph_count == 0) {
        return 0;
    }

    gl = &p->glyphs[it->glyph_start + (right ? it->glyph_count - 1 : 0)];
    return (int64_t)gl->advance * protrude_factor(cp_at(p, gl->cluster), right) / 1000;
}

static void measure(const ctx_t* c, int32_t a, int32_t b, measure_t* m) {
    const pd_para* p = c->p;
    int32_t ib = p->bp_item[b], s = c->bp_start[a];

    if (s > ib) {
        s = ib;
    }

    m->nat = p->sum_w[ib] - p->sum_w[s];
    m->st = p->sum_st[ib] - p->sum_st[s];
    m->fil = p->sum_fil[ib] - p->sum_fil[s];
    m->sh = p->sum_sh[ib] - p->sum_sh[s];

    if (p->items[ib].type == PD_ITEM_PENALTY) {
        m->nat += p->items[ib].width;
    }

    if (c->expand > 0) {    /* glyphs may widen or narrow a little */
        int64_t bx = (p->sum_bx[ib] - p->sum_bx[s]) * c->expand / 1000;

        m->st += bx;
        m->sh += bx;
    }

    m->lp = 0;

    if (c->protrude) {  /* the line may reach into both margins */
        int64_t rp = 0;
        int32_t k;

        if (s < ib) {
            m->lp = box_protrusion(p, &p->items[s], 0);
        }

        if (p->items[ib].type == PD_ITEM_PENALTY && p->items[ib].glyph_count == 1) {
            rp = (int64_t)p->items[ib].width * 700 / 1000;     /* a hyphen added at the break */
        } else {
            for (k = ib - 1; k >= s && k >= ib - 4; k--) {
                if (p->items[k].type == PD_ITEM_BOX) {
                    rp = box_protrusion(p, &p->items[k], 1);
                    break;
                }
            }
        }

        m->nat -= m->lp + rp;
    }

    if (c->ragged) {    /* spaces keep their natural width; the rag absorbs the slack */
        m->st = c->prm->rag_stretch;
        m->sh = 0;
    }
}

/*
 * Cost of a line from a to b in line class lc. Returns the fitness class,
 * or -1 if the line cannot be fitted. *dem excludes adjacency demerits.
 */
static int line_eval(const ctx_t* c, int32_t a, int32_t b, int32_t lc, const measure_t* m, int64_t* dem,
                     int64_t* bad_out) {
    const pd_para* p = c->p;
    const pd_params* prm = c->prm;
    int32_t ib = p->bp_item[b], ia = p->bp_item[a];
    int64_t w = line_width(c, lc), bad, l, d, pen = 0;
    int fc;

    if (m->nat <= w) {
        if (m->fil > 0) {
            bad = 0;
        } else {
            bad = badness(w - m->nat, m->st, prm->tex_badness);
        }

        fc = bad > 99 ? 0 : bad > 12 ? 1 : 2;
    } else {
        if (m->nat - w > m->sh) {
            return -1;
        }

        bad = badness(m->nat - w, m->sh, prm->tex_badness);
        fc = bad > 12 ? 3 : 2;
    }

    l = prm->line_penalty + bad;
    d = l * l;

    if (p->items[ib].type == PD_ITEM_PENALTY) {
        pen = pd_item_penalty(&p->items[ib], prm);
    }

    if (pen > 0) {
        d += pen * pen;
    } else if (pen > -PD_INF_PENALTY) {
        d -= pen * pen;
    }

    if (is_flagged(p, ib) && is_flagged(p, ia)) {
        d += prm->double_hyphen_demerits;
    }

    if (b == p->n_bp - 1 && is_flagged(p, ia)) {
        d += prm->final_hyphen_demerits;
    }

    if (c->oldmark && b != p->n_bp - 1 && !c->oldmark[ib]) {
        d += prm->hysteresis;
    }

    *dem = d;

    if (bad_out) {
        *bad_out = bad;
    }

    return fc;
}

#define SIDX(c, bp, lc, f) ((((int64_t)(bp) * (c)->K + (lc)) * NFIT) + (f))

static void clear_states(ctx_t* c, int32_t bp) {
    int32_t k;
    pd_state* s = &c->p->states[SIDX(c, bp, 0, 0)];

    for (k = 0; k < c->K * NFIT; k++) {
        s[k].total = DEM_INF;
        s[k].prev = -1;
    }
}

/* fill states for breakpoints from..n_bp-1, scanning starts no earlier than lo */
static void total_fit(ctx_t* c, int32_t from, int32_t lo) {
    pd_para* p = c->p;
    int32_t b, a, lc, fa;

    for (b = from; b < p->n_bp; b++) {
        int any = 0;

        clear_states(c, b);

        for (a = b - 1; a >= lo; a--) {
            measure_t m;

            measure(c, a, b, &m);

            if (m.nat - m.sh > c->maxw) {
                break;      /* overfull for every line class; earlier a only longer */
            }

            for (lc = 0; lc < c->K; lc++) {
                const pd_state* sa = &p->states[SIDX(c, a, lc, 0)];
                int32_t nlc = lc + 1 < c->K ? lc + 1 : c->K - 1;
                int64_t d;
                int fc;

                if (sa[0].total >= DEM_INF && sa[1].total >= DEM_INF && sa[2].total >= DEM_INF &&
                        sa[3].total >= DEM_INF) {
                    continue;
                }

                fc = line_eval(c, a, b, lc, &m, &d, NULL);

                if (fc < 0) {
                    continue;
                }

                for (fa = 0; fa < NFIT; fa++) {
                    pd_state* sb = &p->states[SIDX(c, b, nlc, fc)];
                    int64_t t;

                    if (sa[fa].total >= DEM_INF) {
                        continue;
                    }

                    t = sat_add(sa[fa].total, d + ((fa - fc > 1 || fc - fa > 1) ? c->prm->adj_demerits : 0));

                    if (t < sb->total) {
                        sb->total = t;
                        sb->prev = (int32_t)SIDX(c, a, lc, fa);
                        any = 1;
                    }
                }
            }

            if (is_forced(p, c->prm, p->bp_item[a])) {
                break;      /* a line cannot run across a forced break */
            }
        }

        /* nothing fits (e.g. a word wider than the line): emergency overfull line from b-1 */
        if (!any && b - 1 >= lo) {
            a = b - 1;

            for (lc = 0; lc < c->K; lc++) {
                int32_t nlc = lc + 1 < c->K ? lc + 1 : c->K - 1;

                for (fa = 0; fa < NFIT; fa++) {
                    const pd_state* sa = &p->states[SIDX(c, a, lc, fa)];
                    pd_state* sb = &p->states[SIDX(c, b, nlc, 2)];
                    int64_t t;

                    if (sa->total >= DEM_INF) {
                        continue;
                    }

                    t = sat_add(sa->total, DEM_OVERFULL);

                    if (t < sb->total) {
                        sb->total = t;
                        sb->prev = (int32_t)SIDX(c, a, lc, fa);
                    }
                }
            }
        }
    }
}

/*
 * TeX's \looseness: the best breaks whose line count is as close as
 * possible to target. States are kept per (breakpoint, line count,
 * fitness); the line class follows from the count. Returns the number of
 * lines written to seq, or 0 if nothing is feasible.
 */
static int32_t loose_fit(ctx_t* c, int32_t target, int32_t maxl, int32_t* seq) {
    pd_para* p = c->p;
    int32_t b, a, k, fa, M = maxl + 1, best_k = -1, best_f = 0, n;
    int64_t ns = (int64_t)p->n_bp * M * NFIT, best = DEM_INF;
    pd_state* S;

    if (ns > INT32_MAX / 2 || (S = (pd_state*)malloc((size_t)ns * sizeof(pd_state))) == NULL) {
        return 0;
    }

#define LIDX(bp, k, f) ((((int64_t)(bp) * M + (k)) * NFIT) + (f))

    for (k = 0; k < ns; k++) {
        S[k].total = DEM_INF;
        S[k].prev = -1;
    }

    S[LIDX(0, 0, 2)].total = 0;

    for (b = 1; b < p->n_bp; b++) {
        for (a = b - 1; a >= 0; a--) {
            measure_t m;

            measure(c, a, b, &m);

            if (m.nat - m.sh > c->maxw) {
                break;
            }

            for (k = 0; k + 1 < M; k++) {
                int32_t lc = k < c->K ? k : c->K - 1;
                int64_t d;
                int fc = -2;

                for (fa = 0; fa < NFIT; fa++) {
                    const pd_state* sa = &S[LIDX(a, k, fa)];
                    pd_state* sb;
                    int64_t t;

                    if (sa->total >= DEM_INF) {
                        continue;
                    }

                    if (fc == -2) {
                        fc = line_eval(c, a, b, lc, &m, &d, NULL);
                    }

                    if (fc < 0) {
                        break;
                    }

                    sb = &S[LIDX(b, k + 1, fc)];
                    t = sat_add(sa->total, d + ((fa - fc > 1 || fc - fa > 1) ? c->prm->adj_demerits : 0));

                    if (t < sb->total) {
                        sb->total = t;
                        sb->prev = (int32_t)LIDX(a, k, fa);
                    }
                }
            }

            if (is_forced(p, c->prm, p->bp_item[a])) {
                break;
            }
        }
    }

    for (k = 1; k < M; k++) {       /* closest count to the target, then fewest demerits */
        for (fa = 0; fa < NFIT; fa++) {
            int64_t t = S[LIDX(p->n_bp - 1, k, fa)].total;

            if (t >= DEM_INF) {
                continue;
            }

            if (best_k < 0 || abs(k - target) < abs(best_k - target) ||
                    (abs(k - target) == abs(best_k - target) && t < best)) {
                best_k = k;
                best_f = fa;
                best = t;
            }
        }
    }

    n = best_k > 0 ? best_k : 0;

    if (n > 0) {
        int64_t idx = LIDX(p->n_bp - 1, best_k, best_f);

        for (k = n - 1; k >= 0; k--) {
            seq[k] = (int32_t)(idx / ((int64_t)M * NFIT));
            idx = S[idx].prev;
        }
    }

#undef LIDX
    free(S);
    return n;
}

/* first-fit: as many items per line as fit at natural width */
static int32_t greedy(ctx_t* c, int32_t* seq) {
    pd_para* p = c->p;
    int32_t a = 0, n = 0;

    while (a < p->n_bp - 1) {
        int32_t b, pick = a + 1;
        pd_sp w = line_width(c, n < c->K ? n : c->K - 1);

        for (b = a + 1; b < p->n_bp; b++) {
            measure_t m;

            measure(c, a, b, &m);

            if (m.nat > w && b > a + 1) {
                break;
            }

            if (m.nat <= w) {
                pick = b;
            }

            if (is_forced(p, c->prm, p->bp_item[b])) {
                break;
            }
        }

        seq[n++] = pick;
        a = pick;
    }

    return n;
}

/* style-derived vertical extents */
static void style_extents(const pd_para* p, int32_t style, pd_sp* asc, pd_sp* desc, pd_sp* gap) {
    pd_font_metrics fm;
    const pd_style* s;

    *asc = *desc = *gap = 0;

    if (style < 0 || style >= p->n_styles) {
        return;
    }

    s = &p->styles[style];
    pd_font_get_metrics(s->font, &fm);
    *asc = pd_scale(fm.ascender, s->size, fm.units_per_em);
    *desc = pd_scale(-fm.descender, s->size, fm.units_per_em);
    *gap = pd_scale(fm.line_gap, s->size, fm.units_per_em);
}

/* turn a breakpoint sequence (excluding the start) into lines */
static pd_status build_lines(ctx_t* c, const int32_t* seq, int32_t n) {
    pd_para* p = c->p;
    const pd_params* prm = c->prm;
    int32_t i, a = 0, prev_fc = 2;
    pd_sp y = 0, prev_desc = 0;

    if (pd_grow((void**)&p->lines, &p->cap_lines, n, sizeof(pd_lineinfo))) {
        return PD_ERR_NOMEM;
    }

    for (i = 0; i < n; i++) {
        pd_lineinfo* L = &p->lines[i];
        int32_t b = seq[i], ib = p->bp_item[b], lc = i < c->K ? i : c->K - 1, k;
        int32_t s = c->bp_start[a] < ib ? c->bp_start[a] : ib;
        measure_t m;
        int64_t d = 0, bad = 0, w = line_width(c, lc), slack;
        pd_sp asc = 0, desc = 0, gap = 0, la, ld, lg;
        int fc, found = 0;

        measure(c, a, b, &m);
        fc = line_eval(c, a, b, lc, &m, &d, &bad);

        memset(L, 0, sizeof(*L));
        L->start = s;
        L->end = ib;
        L->brk = ib;
        L->fitness = fc < 0 ? 2 : fc;
        L->demerits = fc < 0 ? DEM_OVERFULL : d + ((L->fitness - prev_fc > 1 || prev_fc - L->fitness > 1) ? prm->adj_demerits : 0);
        prev_fc = L->fitness;
        slack = w - m.nat;

        L->pub.text_start = i == 0 ? 0 : (s < ib ? p->items[s].text_start : p->items[ib].text_start);
        L->pub.badness = (int32_t)bad;
        L->pub.hyphenated = is_flagged(p, ib);
        L->pub.overfull = (fc < 0) || (slack < 0 && -slack > m.sh);
        L->pub.underfull = !c->ragged && slack > 0 && m.fil == 0 && m.st <= 0;

        if (!c->ragged) {
            L->slack = slack;
            L->total_stretch[0] = m.st;
            L->total_stretch[1] = m.fil;
            L->total_shrink = m.sh;
            L->pub.x = line_x(c, lc) - (pd_sp)m.lp;

            if (slack > 0) {
                L->pub.width = (pd_sp)(m.fil > 0 ? m.nat : m.st > 0 ? w : m.nat);
                L->pub.ratio = (m.fil > 0 || m.st <= 0) ? 0 : (int32_t)(slack * 1000 / m.st);
            } else {
                int64_t give = (-slack < m.sh) ? -slack : m.sh;
                L->pub.width = (pd_sp)(m.nat - give);
                L->pub.ratio = m.sh > 0 ? (int32_t)(slack * 1000 / m.sh) : 0;
            }
        } else {
            int64_t off = slack > 0 ? slack : 0;

            L->pub.width = (pd_sp)m.nat;
            L->pub.ratio = m.fil > 0 || prm->rag_stretch <= 0 ? 0 : (int32_t)(off * 1000 / prm->rag_stretch);
            L->pub.x = line_x(c, lc) - (pd_sp)m.lp + (pd_sp)(prm->align == PD_ALIGN_RIGHT ? off :
                       prm->align == PD_ALIGN_CENTER ? off / 2 : 0);
        }

        /* vertical extents from the boxes on the line */
        for (k = s; k <= ib && k < p->n_items; k++) {
            const pd_item* it = &p->items[k];

            if (it->type == PD_ITEM_BOX) {
                if (it->height > asc) {
                    asc = it->height;
                }

                if (it->depth > desc) {
                    desc = it->depth;
                }

                if (!(it->flags & PD_FLAG_OBJECT)) {
                    style_extents(p, it->style, &la, &ld, &lg);
                    gap = lg > gap ? lg : gap;
                }

                found = 1;
            }
        }

        if (!found) {   /* empty line: size it by the nearest style */
            int32_t st = p->items[ib].style;

            for (k = ib; st < 0 && k >= 0; k--) {
                st = p->items[k].style;
            }

            style_extents(p, st >= 0 ? st : (p->n_styles ? 0 : -1), &asc, &desc, &gap);
        }

        L->pub.ascent = asc;
        L->pub.descent = desc;

        if (i == 0) {
            y = asc;
        } else {
            int64_t step = ((int64_t)prev_desc + asc + gap) * prm->line_spacing / 1000;

            if (step < prm->baseline_skip) {
                step = prm->baseline_skip;
            }

            y += (pd_sp)step;
        }

        L->pub.baseline = y;
        prev_desc = desc;
        a = b;
    }

    for (i = 0; i < n; i++) {
        p->lines[i].pub.text_end = i + 1 < n ? p->lines[i + 1].pub.text_start : p->n_text;
    }

    p->n_lines = n;
    p->height = n ? y + prev_desc : 0;
    return PD_OK;
}

static int params_same(const pd_params* a, const pd_params* b) {
    return a->mode == b->mode && a->align == b->align && a->width == b->width && a->indent == b->indent &&
           a->line_penalty == b->line_penalty && a->adj_demerits == b->adj_demerits &&
           a->double_hyphen_demerits == b->double_hyphen_demerits &&
           a->final_hyphen_demerits == b->final_hyphen_demerits && a->hyphen_penalty == b->hyphen_penalty &&
           a->ex_hyphen_penalty == b->ex_hyphen_penalty && a->tex_badness == b->tex_badness &&
           a->rag_stretch == b->rag_stretch && a->hysteresis == b->hysteresis &&
           a->freeze_offset == b->freeze_offset && a->looseness == b->looseness && a->protrusion == b->protrusion &&
           a->expansion == b->expansion;
}

static int item_same(const pd_item* x, const pd_item* y) {
    return x->type == y->type && x->flags == y->flags && x->stretch_order == y->stretch_order &&
           x->width == y->width && x->stretch == y->stretch && x->shrink == y->shrink && x->penalty == y->penalty;
}

/* mark the items that were breaks in the previous layout, mapped through the text edit */
static int mark_old_breaks(pd_para* p, uint8_t* mark) {
    uint32_t pre = 0, suf = 0, lo = p->n_old_text < p->n_text ? p->n_old_text : p->n_text;
    int64_t delta = (int64_t)p->n_text - p->n_old_text;
    int32_t j, it = 0;

    while (pre < lo && p->old_text[pre] == p->text[pre]) {
        pre++;
    }

    while (suf < lo - pre && p->old_text[p->n_old_text - 1 - suf] == p->text[p->n_text - 1 - suf]) {
        suf++;
    }

    for (j = 0; j + 1 < p->n_old_lines; j++) {     /* the final break is always kept */
        const pd_item* ob = &p->old_items[p->old_lines[j].brk];
        int64_t o = ob->text_start;

        if (o < pre) {
            /* unchanged prefix */
        } else if (o >= (int64_t)p->n_old_text - suf) {
            o += delta;
        } else {
            continue;
        }

        while (it < p->n_items && (p->items[it].text_start < o ||
                                   (p->items[it].text_start == o && p->items[it].type != ob->type))) {
            it++;
        }

        if (it < p->n_items && p->items[it].text_start == o) {
            mark[it] = 1;
        }
    }

    return 0;
}

static int grow_sums(pd_para* p, int32_t need) {
    int32_t c1 = p->cap_sums, c2 = p->cap_sums, c3 = p->cap_sums, c4 = p->cap_sums, c5 = p->cap_sums;

    if (pd_grow((void**)&p->sum_w, &c1, need, sizeof(int64_t)) ||
            pd_grow((void**)&p->sum_st, &c2, need, sizeof(int64_t)) ||
            pd_grow((void**)&p->sum_fil, &c3, need, sizeof(int64_t)) ||
            pd_grow((void**)&p->sum_sh, &c4, need, sizeof(int64_t)) ||
            pd_grow((void**)&p->sum_bx, &c5, need, sizeof(int64_t))) {
        return -1;
    }

    p->cap_sums = c1 < c2 ? c1 : c2;
    p->cap_sums = p->cap_sums < c3 ? p->cap_sums : c3;
    p->cap_sums = p->cap_sums < c4 ? p->cap_sums : c4;
    p->cap_sums = p->cap_sums < c5 ? p->cap_sums : c5;
    return 0;
}

/* remember this layout as the reference for the next incremental break */
static pd_status snapshot(pd_para* p) {
    if (pd_grow((void**)&p->old_items, &p->cap_old_items, p->n_items, sizeof(pd_item)) ||
            pd_grow((void**)&p->old_text, &p->cap_old_text, (int64_t)p->n_text + 1, 1) ||
            pd_grow((void**)&p->old_lines, &p->cap_old_lines, p->n_lines, sizeof(pd_lineinfo))) {
        return PD_ERR_NOMEM;
    }

    memcpy(p->old_items, p->items, (size_t)p->n_items * sizeof(pd_item));

    if (p->n_text) {
        memcpy(p->old_text, p->text, p->n_text);
    }

    memcpy(p->old_lines, p->lines, (size_t)p->n_lines * sizeof(pd_lineinfo));
    p->n_old_items = p->n_items;
    p->n_old_text = p->n_text;
    p->n_old_lines = p->n_lines;
    return PD_OK;
}

pd_status pd_break_lines(pd_para* p, const pd_params* prm, pd_break_info* info) {
    ctx_t c;
    int32_t i, n = p->n_items, k = 0, from = 1, lo = 0, nseq = 0, frozen = 0, reused = 0;
    int32_t* seq = NULL;
    int64_t best = DEM_INF, sidx = -1, total = 0;
    int stable_mode = (prm->hysteresis > 0 || prm->freeze_offset >= 0) && p->n_old_lines > 0;
    pd_status st = PD_OK;

    memset(&c, 0, sizeof(c));
    c.p = p;
    c.prm = prm;
    c.K = p->n_shape > 2 ? p->n_shape : 2;
    c.ragged = prm->align != PD_ALIGN_JUSTIFY;
    c.protrude = prm->protrusion != 0;
    c.expand = prm->expansion < 0 ? 0 : prm->expansion > 100 ? 100 : prm->expansion;

    for (i = 0; i < c.K; i++) {
        pd_sp w = line_width(&c, i);
        c.maxw = (i == 0 || w > c.maxw) ? w : c.maxw;
    }

    /* prefix sums over items */
    if (grow_sums(p, n + 1)) {
        return PD_ERR_NOMEM;
    }

    p->sum_w[0] = p->sum_st[0] = p->sum_fil[0] = p->sum_sh[0] = p->sum_bx[0] = 0;

    for (i = 0; i < n; i++) {
        const pd_item* it = &p->items[i];
        int glue = it->type == PD_ITEM_GLUE;

        p->sum_w[i + 1] = p->sum_w[i] + (it->type == PD_ITEM_PENALTY ? 0 : it->width);
        p->sum_st[i + 1] = p->sum_st[i] + (glue && it->stretch_order == 0 ? it->stretch : 0);
        p->sum_fil[i + 1] = p->sum_fil[i] + (glue && it->stretch_order == 1 ? it->stretch : 0);
        p->sum_sh[i + 1] = p->sum_sh[i] + (glue ? it->shrink : 0);
        p->sum_bx[i + 1] = p->sum_bx[i] + (it->type == PD_ITEM_BOX && !(it->flags & PD_FLAG_OBJECT) ? it->width : 0);
    }

    /* breakpoints; bp 0 is the paragraph start */
    if (pd_grow((void**)&p->bp_item, &p->cap_bp, (int64_t)n + 1, sizeof(int32_t))) {
        return PD_ERR_NOMEM;
    }

    p->n_bp = 0;
    p->bp_item[p->n_bp++] = -1;

    for (i = 0; i < n; i++) {
        if (is_breakpoint(p, prm, i)) {
            p->bp_item[p->n_bp++] = i;
        }
    }

    c.bp_start = (int32_t*)malloc((size_t)p->n_bp * sizeof(int32_t));
    seq = (int32_t*)malloc((size_t)p->n_bp * sizeof(int32_t));

    if (!c.bp_start || !seq) {
        st = PD_ERR_NOMEM;
        goto done;
    }

    c.bp_start[0] = 0;

    for (i = 1; i < p->n_bp; i++) {     /* glue and penalties after a break vanish, fil glue stays */
        int32_t s = p->bp_item[i] + 1;

        while (s < n && p->items[s].type != PD_ITEM_BOX && !is_forced(p, prm, s) &&
                !(p->items[s].type == PD_ITEM_GLUE && p->items[s].stretch_order == 1)) {
            s++;
        }

        c.bp_start[i] = s;
    }

    /* first item that differs from the previous layout */
    {
        int32_t lim = n < p->n_old_items ? n : p->n_old_items;

        while (k < lim && item_same(&p->items[k], &p->old_items[k])) {
            k++;
        }
    }

    if (prm->mode == PD_BREAK_GREEDY) {
        nseq = greedy(&c, seq);
        st = build_lines(&c, seq, nseq);
        p->have_cache = 0;
        goto finish;
    }

    if ((int64_t)p->n_bp * c.K * NFIT > INT32_MAX) {
        st = PD_ERR_RANGE;
        goto done;
    }

    if (pd_grow((void**)&p->states, &p->cap_states, (int64_t)p->n_bp * c.K * NFIT, sizeof(pd_state))) {
        st = PD_ERR_NOMEM;
        goto done;
    }

    if (stable_mode && prm->hysteresis > 0) {
        c.oldmark = (uint8_t*)calloc((size_t)n + 1, 1);

        if (!c.oldmark) {
            st = PD_ERR_NOMEM;
            goto done;
        }

        mark_old_breaks(p, c.oldmark);
    }

    /* freeze: keep previous lines that end in the unchanged prefix before freeze_offset */
    if (stable_mode && prm->freeze_offset >= 0) {
        int32_t j, b = 0;

        for (j = 0; j + 1 < p->n_old_lines; j++) {
            const pd_lineinfo* ol = &p->old_lines[j];

            if (ol->brk >= k || ol->pub.text_end > (uint32_t)prm->freeze_offset) {
                break;
            }

            while (b < p->n_bp && p->bp_item[b] < ol->brk) {
                b++;
            }

            if (b >= p->n_bp || p->bp_item[b] != ol->brk) {
                break;
            }

            seq[frozen++] = b;
        }

        if (frozen) {
            int32_t lc = frozen < c.K ? frozen : c.K - 1;
            int64_t sum = 0;

            for (j = 0; j < frozen; j++) {
                sum += p->old_lines[j].demerits;
            }

            lo = seq[frozen - 1];
            from = lo + 1;
            clear_states(&c, lo);
            p->states[SIDX(&c, lo, lc, p->old_lines[frozen - 1].fitness)].total = sum;
        }
    }

    if (!frozen) {
        /* reuse the states of breakpoints before the first changed item */
        if (p->have_cache && !stable_mode && !p->shape_dirty && c.K == p->line_classes &&
                params_same(prm, &p->last_params)) {
            while (from < p->n_bp && p->bp_item[from] < k) {
                from++;
            }

            reused = from;
        } else {
            from = 1;
        }

        if (from <= 1) {
            from = 1;
            clear_states(&c, 0);
            p->states[SIDX(&c, 0, 0, 2)].total = 0;
        }
    }

    total_fit(&c, from, lo);

    /* best end state, then walk back */
    {
        int32_t last = p->n_bp - 1, lc, f, nb = 0;
        int64_t idx;

        for (lc = 0; lc < c.K; lc++) {
            for (f = 0; f < NFIT; f++) {
                const pd_state* s = &p->states[SIDX(&c, last, lc, f)];

                if (s->total < best) {
                    best = s->total;
                    sidx = SIDX(&c, last, lc, f);
                }
            }
        }

        if (sidx < 0) {
            st = PD_ERR_STATE;
            goto done;
        }

        for (idx = sidx; idx >= 0; idx = p->states[idx].prev) {
            int32_t bp = (int32_t)(idx / (c.K * NFIT));

            if (bp == lo) {
                break;
            }

            nb++;
        }

        nseq = frozen + nb;

        for (idx = sidx, i = nseq - 1; i >= frozen; i--) {
            seq[i] = (int32_t)(idx / (c.K * NFIT));
            idx = p->states[idx].prev;
        }
    }

    /* \looseness: the optimum fixes the line count to aim from */
    if (prm->looseness != 0 && !frozen) {
        int32_t target = nseq + prm->looseness, maxl = target > nseq ? target : nseq, nl;

        target = target < 1 ? 1 : target;
        nl = loose_fit(&c, target, maxl, seq);   /* 0: infeasible without emergency lines, keep the optimum */

        if (nl > 0) {
            nseq = nl;
        }
    }

    st = build_lines(&c, seq, nseq);
    p->have_cache = !stable_mode;

finish:

    if (st != PD_OK) {
        goto done;
    }

    p->broken = 1;
    p->shape_dirty = 0;
    p->line_classes = c.K;
    p->last_params = *prm;

    for (i = 0; i < p->n_lines; i++) {
        total = sat_add(total, p->lines[i].demerits);
    }

    if (info) {
        memset(info, 0, sizeof(*info));
        info->lines = p->n_lines;
        info->demerits = total;
        info->reused_breakpoints = reused;
        info->frozen_lines = frozen;
        info->height = p->height;

        for (i = 0; i < p->n_lines; i++) {
            info->overfull += p->lines[i].pub.overfull;
            info->underfull += p->lines[i].pub.underfull;
        }
    }

    st = snapshot(p);

done:
    free(c.bp_start);
    free(c.oldmark);
    free(seq);
    return st;
}
