/*
 * Parade converters: dispatch, plain text, clipboard ranges and paste,
 * and the helpers every format shares
 *
 * Everything here uses the public document API only.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

/* ------------------------------------------------------------------ */
/* output buffer                                                      */
/* ------------------------------------------------------------------ */

void pb_put(pd_buf* b, const void* s, size_t n) {
    if (b->err || n == 0) {
        return;
    }

    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char* p;

        while (cap < b->n + n + 1) {
            cap *= 2;
        }

        if ((p = (char*)realloc(b->p, cap)) == NULL) {
            b->err = 1;
            return;
        }

        b->p = p;
        b->cap = cap;
    }

    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

void pb_puts(pd_buf* b, const char* s) {
    pb_put(b, s, strlen(s));
}

void pb_putc(pd_buf* b, char c) {
    pb_put(b, &c, 1);
}

void pb_printf(pd_buf* b, const char* fmt, ...) {
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0) {
        b->err = 1;
    } else if ((size_t)n < sizeof(tmp)) {
        pb_put(b, tmp, (size_t)n);
    } else {
        char* big = (char*)malloc((size_t)n + 1);

        if (!big) {
            b->err = 1;
            return;
        }

        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        pb_put(b, big, (size_t)n);
        free(big);
    }
}

void pb_free(pd_buf* b) {
    free(b->p);
    memset(b, 0, sizeof(*b));
}

pd_status pb_flush(pd_buf* b, pd_writer fn, void* user) {
    pd_status st = PD_OK;

    if (b->err) {
        st = PD_ERR_NOMEM;
    } else if (b->n && fn(user, b->p, b->n)) {
        st = PD_ERR_IO;
    }

    pb_free(b);
    return st;
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void pb_base64(pd_buf* b, const unsigned char* d, size_t n) {
    size_t i;

    for (i = 0; i + 2 < n; i += 3) {
        char q[4] = { b64[d[i] >> 2], b64[((d[i] & 3) << 4) | (d[i + 1] >> 4)],
                      b64[((d[i + 1] & 15) << 2) | (d[i + 2] >> 6)], b64[d[i + 2] & 63]
                    };
        pb_put(b, q, 4);
    }

    if (n - i == 1) {
        char q[4] = { b64[d[i] >> 2], b64[(d[i] & 3) << 4], '=', '=' };
        pb_put(b, q, 4);
    } else if (n - i == 2) {
        char q[4] = { b64[d[i] >> 2], b64[((d[i] & 3) << 4) | (d[i + 1] >> 4)], b64[(d[i + 1] & 15) << 2], '=' };
        pb_put(b, q, 4);
    }
}

size_t pd_base64_decode(const char* s, size_t n, unsigned char* out) {
    uint32_t acc = 0;
    int bits = 0;
    size_t i, o = 0;

    for (i = 0; i < n; i++) {
        const char* p;
        int v;

        if (s[i] == '=') {
            break;
        }

        if ((p = strchr(b64, s[i])) == NULL || s[i] == '\0') {
            continue;   /* whitespace and line breaks */
        }

        v = (int)(p - b64);
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;

        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
        }
    }

    return o;
}

/* ------------------------------------------------------------------ */
/* reading                                                            */
/* ------------------------------------------------------------------ */

static int spans(const pd_doc* d, pd_block_id para, pd_span_fn fn, void* user, int deleted) {
    const char* t;
    uint32_t len;
    pd_run* runs = NULL;
    int32_t nr = 0, r;
    int stop = 0;

    if (pd_doc_para_text(d, para, &t, &len) != PD_OK || len == 0) {
        return 0;
    }

    pd_doc_para_runs(d, para, NULL, 0, &nr);

    if (nr <= 0 || (runs = (pd_run*)malloc((size_t)nr * sizeof(pd_run))) == NULL) {
        return 0;
    }

    pd_doc_para_runs(d, para, runs, nr, &nr);

    for (r = 0; r < nr && !stop; r++) {
        uint32_t pos = runs[r].start, end = runs[r].end;
        pd_span sp;

        memset(&sp, 0, sizeof(sp));
        sp.format = runs[r].format;
        pd_doc_format_resolve(d, para, runs[r].format, &sp.cp);

        if (!deleted && sp.cp.revision) {     /* exported as if accepted: tracked deletions are left out */
            pd_revision rv;

            if (pd_doc_revision_get(d, sp.cp.revision, &rv) == PD_OK && rv.kind == PD_REV_DELETE) {
                continue;
            }
        }

        while (pos < end && !stop) {
            uint32_t k = pos;

            while (k < end && !(k + 3 <= len && (unsigned char)t[k] == 0xEF && (unsigned char)t[k + 1] == 0xBF &&
                                (unsigned char)t[k + 2] == 0xBC)) {
                k++;
            }

            if (k > pos) {
                sp.is_object = 0;
                sp.text = t + pos;
                sp.len = k - pos;
                sp.offset = pos;
                stop = fn(user, &sp);
            }

            if (!stop && k < end) {
                pd_pos at;

                at.block = para;
                at.offset = k;
                sp.is_object = 1;
                sp.text = t + k;
                sp.len = 3;
                sp.offset = k;

                if (pd_doc_inline_at(d, at, &sp.obj) == PD_OK) {
                    stop = fn(user, &sp);
                }

                k += 3;
            }

            pos = k;
        }
    }

    free(runs);
    return stop;
}

int pd_conv_spans(const pd_doc* d, pd_block_id para, pd_span_fn fn, void* user) {
    return spans(d, para, fn, user, 0);
}

int pd_conv_spans_all(const pd_doc* d, pd_block_id para, pd_span_fn fn, void* user) {
    return spans(d, para, fn, user, 1);
}

static void numbers_walk(pd_numbers* nb, pd_block_id id, int32_t seq_val[16], char seq_name[16][32], int32_t* nseq,
                         int32_t* notes) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(nb->d, id, &bi) != PD_OK) {
        return;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        const char* t;
        uint32_t len, k;

        pd_doc_para_text(nb->d, id, &t, &len);

        for (k = 0; k + 2 < len; k++) {
            pd_inline o;
            pd_pos at;
            pd_numval v;

            if ((unsigned char)t[k] != 0xEF || (unsigned char)t[k + 1] != 0xBF || (unsigned char)t[k + 2] != 0xBC) {
                continue;
            }

            at.block = id;
            at.offset = k;

            if (pd_doc_inline_at(nb->d, at, &o) != PD_OK) {
                continue;
            }

            v.block = id;
            v.offset = k;

            if (o.kind == PD_INLINE_FOOTNOTE) {
                v.value = ++*notes;
            } else if (o.kind == PD_INLINE_FIELD && o.field == PD_FIELD_SEQ) {
                int32_t s;

                for (s = 0; s < *nseq && strcmp(seq_name[s], o.name); s++) {
                }

                if (s == *nseq && s < 16) {
                    strcpy(seq_name[s], o.name);
                    seq_val[s] = 0;
                    (*nseq)++;
                }

                v.value = s < 16 ? ++seq_val[s] : 0;
            } else {
                continue;
            }

            if (!pd_grow((void**)&nb->v, &nb->cap, (int64_t)nb->n + 1, sizeof(pd_numval))) {
                nb->v[nb->n++] = v;
            }
        }

        return;
    }

    for (i = 0; i < bi.child_count; i++) {
        numbers_walk(nb, pd_doc_child(nb->d, id, i), seq_val, seq_name, nseq, notes);
    }
}

void pd_numbers_init(pd_numbers* nb, const pd_doc* d) {
    int32_t seq_val[16], nseq = 0, notes = 0;
    char seq_name[16][32];

    memset(nb, 0, sizeof(*nb));
    nb->d = d;
    numbers_walk(nb, pd_doc_root(d), seq_val, seq_name, &nseq, &notes);
}

void pd_numbers_free(pd_numbers* nb) {
    free(nb->v);
    memset(nb, 0, sizeof(*nb));
}

int32_t pd_numbers_at(const pd_numbers* nb, pd_block_id block, uint32_t offset) {
    int32_t i;

    for (i = 0; i < nb->n; i++) {
        if (nb->v[i].block == block && nb->v[i].offset == offset) {
            return nb->v[i].value;
        }
    }

    return 0;
}

static int32_t first_seq(const pd_numbers* nb, pd_block_id id, int depth) {
    pd_block_info bi;
    int32_t i, v;

    if (depth > 32 || pd_doc_block_info(nb->d, id, &bi) != PD_OK) {
        return 0;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        for (i = 0; i < nb->n; i++) {
            if (nb->v[i].block == id) {
                pd_pos at;
                pd_inline o;

                at.block = id;
                at.offset = nb->v[i].offset;

                if (pd_doc_inline_at(nb->d, at, &o) == PD_OK && o.kind == PD_INLINE_FIELD) {
                    return nb->v[i].value;
                }
            }
        }

        return 0;
    }

    for (i = 0; i < bi.child_count; i++) {
        if ((v = first_seq(nb, pd_doc_child(nb->d, id, i), depth + 1)) != 0) {
            return v;
        }
    }

    return 0;
}

int32_t pd_numbers_ref(const pd_numbers* nb, pd_block_id target) {
    return first_seq(nb, target, 0);
}

const char* pd_conv_style_name(const pd_doc* d, pd_block_id para) {
    pd_block_info bi;
    const char* n;

    if (pd_doc_block_info(d, para, &bi) != PD_OK || !bi.style) {
        return "Normal";
    }

    n = pd_doc_style_name(d, bi.style);
    return n ? n : "Normal";
}

/* a paragraph's style's properties as they come to where it is: its table style's under them in a cell */
void pd_conv_style_pp(const pd_doc* d, pd_block_id para, pd_style_id style, pd_para_props* out) {
    pd_block_info pi;
    pd_table_style_part part;
    int in_cell = pd_doc_block_info(d, para, &pi) == PD_OK && pd_doc_cell_style(d, pi.parent, &part) == PD_OK;

    pd_doc_style_resolve_with(d, style, in_cell ? &part : NULL, out, NULL);
}

void pd_conv_base_props(const pd_doc* d, pd_block_id para, pd_char_props* out) {
    if (pd_doc_format_resolve(d, para, 0, out) != PD_OK) {
        memset(out, 0, sizeof(*out));
        out->weight = 400;
    }
}

int pd_conv_is_mono(const pd_char_props* cp) {
    char low[64];
    size_t i;

    for (i = 0; i < sizeof(low) - 1 && cp->family[i]; i++) {
        low[i] = (char)tolower((unsigned char)cp->family[i]);
    }

    low[i] = '\0';
    return strstr(low, "mono") || strstr(low, "courier") || strstr(low, "consol") || strstr(low, "code") ||
           strstr(low, "typewriter") || strstr(low, "menlo");
}

int pd_conv_list_kind(const pd_doc* d, pd_block_id para, int32_t* level) {
    pd_block_info bi;
    pd_list_level lv[9];
    int32_t n = 0;

    if (pd_doc_block_info(d, para, &bi) != PD_OK || !bi.list || pd_doc_list_info(d, bi.list, &n, lv) != PD_OK) {
        return 0;
    }

    *level = bi.list_level < n ? bi.list_level : n - 1;

    {
        pd_para_attrs at;

        if (pd_doc_para_attrs(d, para, &at) == PD_OK && at.cont) {
            return 0;   /* a later block of an item is no item (pd_conv_item_level finds its item) */
        }
    }

    return lv[*level].format == PD_NUM_BULLET || lv[*level].format == PD_NUM_NONE ? 1 : 2;
}

int pd_conv_item_level(const pd_doc* d, pd_block_id para, int32_t* level) {
    pd_block_info bi;
    pd_para_attrs at;

    if (pd_doc_block_info(d, para, &bi) != PD_OK || !bi.list || pd_doc_para_attrs(d, para, &at) != PD_OK ||
            !at.cont) {
        return 0;
    }

    *level = bi.list_level;
    return 1;
}

/* ------------------------------------------------------------------ */
/* the builder                                                        */
/* ------------------------------------------------------------------ */

static uint32_t para_len(const pd_doc* d, pd_block_id p) {
    const char* t;
    uint32_t n = 0;

    pd_doc_para_text(d, p, &t, &n);
    return n;
}

static void bld_reset_next(pd_bld* b) {
    b->pstyle[0] = '\0';
    b->role = PD_ROLE_BODY;
    b->level = 0;
    b->list_kind = 0;
    b->list_level = 0;
    b->list_id = 0;
    memset(&b->pp, 0, sizeof(b->pp));
}

void bld_init(pd_bld* b, pd_doc* d) {
    pd_block_id sec = pd_doc_child(d, pd_doc_root(d), 0);

    memset(b, 0, sizeof(*b));
    b->d = d;
    b->st[0].id = sec;
    b->st[0].fresh = pd_doc_child(d, sec, 0);
    b->depth = 1;
    b->pend_fmt = (pd_format_id) - 1;
    bld_reset_next(b);
}

/* where a new block goes: before the container's first paragraph while
   nothing has been written into it, so that the paragraph after the block
   is the one that takes it; at the end otherwise */
static int32_t bld_slot(const pd_bld* b) {
    const bld_level* L = &b->st[b->depth - 1];
    pd_block_info bi;

    return L->fresh && pd_doc_block_info(b->d, L->fresh, &bi) == PD_OK ? bi.index : -1;
}

pd_block_id bld_container(const pd_bld* b) {
    return b->st[b->depth - 1].id;
}

static void bld_flush(pd_bld* b) {
    if (b->pend.n && b->para) {
        pd_pos at;
        pd_status st;

        at.block = b->para;
        at.offset = para_len(b->d, b->para);
        st = pd_doc_insert_text(b->d, at, b->pend.p, b->pend.n, b->pend_fmt == (pd_format_id) - 1 ? 0 : b->pend_fmt,
                                NULL);

        if (st != PD_OK && b->err == PD_OK) {
            b->err = st;
        }
    }

    b->pend.n = 0;
}

void bld_para_style(pd_bld* b, const char* style, int32_t role, int32_t level) {
    snprintf(b->pstyle, sizeof(b->pstyle), "%s", style ? style : "");
    b->role = role;
    b->level = level;
}

void bld_list(pd_bld* b, int32_t kind, int32_t level) {
    b->list_kind = kind;
    b->list_level = level < 0 ? 0 : level > 8 ? 8 : level;
}

pd_list_id bld_list_new(pd_bld* b, int32_t kind, int32_t level, int32_t start) {
    static const char* bullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };
    pd_list_level lv[9];
    pd_list_id id = 0;
    int32_t i;

    memset(lv, 0, sizeof(lv));

    for (i = 0; i < 9; i++) {
        lv[i].format = kind == 1 ? PD_NUM_BULLET : PD_NUM_DECIMAL;
        lv[i].start = i == level && start >= 0 ? start : 1;
        lv[i].indent = PD_PT(18) * (i + 1);
        lv[i].hanging = PD_PT(18);

        if (kind == 1) {
            strcpy(lv[i].text, bullets[i % 3]);
        } else {
            snprintf(lv[i].text, sizeof(lv[i].text), "%%%d.", (int)i + 1);
        }
    }

    pd_doc_list_define(b->d, 9, lv, &id);
    return id;
}

static pd_list_id bld_list_id(pd_bld* b, int32_t kind) {
    if (!b->lists[kind]) {
        pd_list_level lv[9];
        int32_t i;
        static const char* bullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };

        memset(lv, 0, sizeof(lv));

        for (i = 0; i < 9; i++) {
            lv[i].format = kind == 1 ? PD_NUM_BULLET : PD_NUM_DECIMAL;
            lv[i].start = 1;
            lv[i].indent = PD_PT(18) * (i + 1);
            lv[i].hanging = PD_PT(18);

            if (kind == 1) {
                strcpy(lv[i].text, bullets[i % 3]);
            } else {
                snprintf(lv[i].text, sizeof(lv[i].text), "%%%d.", (int)i + 1);
            }
        }

        pd_doc_list_define(b->d, 9, lv, &b->lists[kind]);
    }

    return b->lists[kind];
}

pd_pos bld_pos(pd_bld* b) {
    pd_pos at;
    const char* t;
    uint32_t n = 0;

    bld_flush(b);
    at.block = b->para;
    at.offset = b->para && pd_doc_para_text(b->d, b->para, &t, &n) == PD_OK ? n : 0;
    return at;
}

pd_block_id bld_begin_para(pd_bld* b) {
    bld_level* L = &b->st[b->depth - 1];
    pd_block_id p = 0;
    pd_style_id sid;

    bld_end_para(b);

    if (L->fresh) {
        p = L->fresh;
        L->fresh = 0;
    } else if (pd_doc_insert_block(b->d, L->id, -1, PD_BLOCK_PARAGRAPH, &p) != PD_OK) {
        b->err = PD_ERR_STATE;
        return 0;
    }

    if (b->pstyle[0] && (sid = pd_doc_style_find(b->d, b->pstyle)) != 0) {
        pd_doc_set_para_style(b->d, p, sid);
    }

    if (b->role != PD_ROLE_BODY) {
        pd_doc_set_role(b->d, p, (pd_role)b->role, b->role == PD_ROLE_HEADING ? (b->level < 1 ? 1 : b->level > 6 ? 6 :
                        b->level) : 0);
    }

    if (b->list_id) {
        pd_doc_set_list(b->d, p, b->list_id, b->list_level);
    } else if (b->list_kind) {
        pd_doc_set_list(b->d, p, bld_list_id(b, b->list_kind), b->list_level);
    }

    if (b->pp.mask) {
        pd_doc_set_para_props(b->d, p, &b->pp);
    }

    bld_reset_next(b);
    b->para = p;
    return p;
}

void bld_end_para(pd_bld* b) {
    bld_flush(b);
    b->para = 0;
}

void bld_set_format(pd_bld* b, const pd_char_props* cp) {
    b->cp = *cp;
}

static pd_format_id bld_format(pd_bld* b) {
    return b->cp.mask || b->cstyle ? pd_doc_format(b->d, b->cstyle, &b->cp) : 0;
}

void bld_mark_format(pd_bld* b) {
    if (b->para && !b->pend.n) {
        pd_doc_set_mark_format(b->d, b->para, bld_format(b));
    }
}

void bld_text(pd_bld* b, const char* s, size_t n) {
    pd_format_id f;
    size_t i, j;

    if (n == 0) {
        return;
    }

    if (!b->para && !bld_begin_para(b)) {
        return;
    }

    f = bld_format(b);

    if (b->pend.n && f != b->pend_fmt) {
        bld_flush(b);
    }

    b->pend_fmt = f;

    /* U+FFFC is reserved for objects; NUL and other C0 controls but tab and line feed are dropped */
    for (i = j = 0; i <= n; i++) {
        int drop = i < n && (((unsigned char)s[i] < 32 && s[i] != '\t' && s[i] != '\n') ||
                             (i + 2 < n && (unsigned char)s[i] == 0xEF && (unsigned char)s[i + 1] == 0xBF &&
                              (unsigned char)s[i + 2] == 0xBC));

        if (i == n || drop) {
            pb_put(&b->pend, s + j, i - j);

            if (drop && (unsigned char)s[i] == 0xEF) {
                i += 2;
            }

            j = i + 1;
        }
    }
}

void bld_inline(pd_bld* b, const pd_inline* o) {
    pd_pos at;

    if (!b->para && !bld_begin_para(b)) {
        return;
    }

    bld_flush(b);
    at.block = b->para;
    at.offset = para_len(b->d, b->para);

    if (pd_doc_insert_inline(b->d, at, o, NULL) == PD_OK) {
        /* the format the builder has, not the one of the character before it (a link's, a heading run's) */
        pd_range r;

        r.start = at;
        r.end.block = at.block;
        r.end.offset = at.offset + 3;
        pd_doc_clear_char_props(b->d, r, 0xFFFFFFFFu);

        if (b->cstyle) {
            pd_doc_set_char_style(b->d, r, b->cstyle);
        }

        if (b->cp.mask) {
            pd_doc_set_char_props(b->d, r, &b->cp);
        }
    }
}

static void bld_push(pd_bld* b, pd_block_id id) {
    bld_flush(b);

    if (b->depth >= BLD_DEPTH) {
        b->err = PD_ERR_RANGE;
        return;
    }

    b->st[b->depth].id = id;
    b->st[b->depth].fresh = pd_doc_child(b->d, id, 0);
    b->st[b->depth].saved_para = b->para;
    b->st[b->depth].saved_cp = b->cp;
    b->para = 0;
    b->depth++;
}

/* a container's initial empty paragraph goes if anything else filled it */
static void bld_tidy(pd_bld* b, bld_level* L) {
    pd_block_info bi;

    if (L->fresh && pd_doc_block_info(b->d, L->id, &bi) == PD_OK && bi.child_count > 1 &&
            para_len(b->d, L->fresh) == 0) {
        pd_doc_remove_block(b->d, L->fresh);
    }

    L->fresh = 0;
}

static void bld_pop(pd_bld* b) {
    if (b->depth > 1) {
        bld_end_para(b);
        bld_tidy(b, &b->st[b->depth - 1]);
        b->depth--;
        b->para = b->st[b->depth].saved_para;
        b->cp = b->st[b->depth].saved_cp;
    }
}

void bld_table_begin(pd_bld* b) {
    pd_block_id t;
    int32_t k;

    bld_end_para(b);

    if (b->ntables >= 8 || pd_doc_insert_block(b->d, bld_container(b), bld_slot(b), PD_BLOCK_TABLE, &t) != PD_OK) {
        b->err = b->err ? b->err : PD_ERR_RANGE;
        b->ntables++;   /* keep begin/end balanced */
        return;
    }

    k = b->ntables++;
    b->table[k] = t;
    b->row[k] = 0;
    b->nrows[k] = b->ncells[k] = b->header_rows[k] = 0;
}

static int bld_table_ok(const pd_bld* b) {
    return b->ntables > 0 && b->ntables <= 8 && b->table[b->ntables - 1];
}

void bld_row_begin(pd_bld* b, int header) {
    int32_t k = b->ntables - 1;

    if (!bld_table_ok(b)) {
        return;
    }

    bld_end_para(b);

    if (b->nrows[k] == 0) {
        b->row[k] = pd_doc_child(b->d, b->table[k], 0);
    } else if (pd_doc_insert_block(b->d, b->table[k], -1, PD_BLOCK_ROW, &b->row[k]) != PD_OK) {
        b->err = PD_ERR_STATE;
        return;
    }

    if (header && b->header_rows[k] == b->nrows[k]) {
        b->header_rows[k]++;
    }

    b->nrows[k]++;
    b->ncells[k] = 0;
}

void bld_cell_begin(pd_bld* b, int32_t col_span, uint32_t background) {
    int32_t k = b->ntables - 1;
    pd_block_id cell;

    if (!bld_table_ok(b)) {
        bld_push(b, bld_container(b));     /* keep begin/end balanced */
        b->st[b->depth - 1].fresh = 0;
        return;
    }

    if (b->nrows[k] == 0) {
        bld_row_begin(b, 0);
    }

    bld_end_para(b);

    if (b->ncells[k] == 0) {
        cell = pd_doc_child(b->d, b->row[k], 0);
    } else if (pd_doc_insert_block(b->d, b->row[k], -1, PD_BLOCK_CELL, &cell) != PD_OK) {
        b->err = PD_ERR_STATE;
        cell = pd_doc_child(b->d, b->row[k], 0);
    }

    b->ncells[k]++;

    if (col_span > 1 || background) {
        pd_cell_props cp;

        pd_doc_cell_props(b->d, cell, &cp);
        cp.col_span = col_span < 1 ? 1 : col_span > PD_TABLE_MAX_COLS ? PD_TABLE_MAX_COLS : col_span;
        cp.background = background;
        pd_doc_set_cell_props(b->d, cell, &cp);
    }

    bld_push(b, cell);
}

void bld_cell_end(pd_bld* b) {
    bld_pop(b);
}

void bld_cell_merge_up(pd_bld* b) {
    pd_block_id cell = bld_container(b);
    pd_cell_props cp;
    pd_block_info bi;

    if (pd_doc_block_info(b->d, cell, &bi) == PD_OK && bi.kind == PD_BLOCK_CELL &&
            pd_doc_cell_props(b->d, cell, &cp) == PD_OK) {
        cp.merge_up = 1;
        pd_doc_set_cell_props(b->d, cell, &cp);
    }
}

void bld_table_end(pd_bld* b) {
    int32_t k;

    bld_end_para(b);

    if (b->ntables <= 0) {
        return;
    }

    k = --b->ntables;

    if (k < 8 && b->table[k] && b->header_rows[k] > 0) {
        pd_table_props tp;

        pd_doc_table_props(b->d, b->table[k], &tp);
        tp.header_rows = b->header_rows[k];
        pd_doc_set_table_props(b->d, b->table[k], &tp);
    }
}

pd_block_id bld_footnote_begin(pd_bld* b) {
    return bld_note_begin(b, 0);
}

pd_block_id bld_note_begin(pd_bld* b, int endnote) {
    pd_block_id story;
    pd_inline o;

    if (pd_doc_insert_block(b->d, 0, -1, PD_BLOCK_STORY, &story) != PD_OK) {
        b->err = PD_ERR_STATE;
        return 0;
    }

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FOOTNOTE;
    o.target = story;
    o.level = endnote ? 1 : 0;
    bld_inline(b, &o);
    bld_push(b, story);     /* saves the paragraph the note's mark is in */
    return story;
}

void bld_footnote_end(pd_bld* b) {
    bld_pop(b);
}

pd_block_id bld_story_begin(pd_bld* b) {
    pd_block_id story;

    if (pd_doc_insert_block(b->d, 0, -1, PD_BLOCK_STORY, &story) != PD_OK) {
        b->err = PD_ERR_STATE;
        return 0;
    }

    bld_push(b, story);
    return story;
}

void bld_story_end(pd_bld* b) {
    bld_pop(b);
}

pd_block_id bld_float_begin(pd_bld* b) {
    pd_block_id fl;

    bld_end_para(b);

    if (pd_doc_insert_block(b->d, bld_container(b), bld_slot(b), PD_BLOCK_FLOAT, &fl) != PD_OK) {
        bld_push(b, bld_container(b));
        b->st[b->depth - 1].fresh = 0;
        return 0;
    }

    bld_push(b, fl);
    return fl;
}

void bld_float_end(pd_bld* b) {
    bld_pop(b);
}

void bld_break(pd_bld* b, int32_t kind) {
    pd_block_id br;

    bld_end_para(b);

    if (pd_doc_insert_block(b->d, bld_container(b), -1, PD_BLOCK_BREAK, &br) == PD_OK) {
        pd_doc_set_break(b->d, br, (pd_break_kind)kind);
    }
}

pd_block_id bld_section(pd_bld* b, const pd_section_props* sp) {
    pd_block_id sec;

    while (b->depth > 1) {
        bld_pop(b);
    }

    bld_end_para(b);

    if (pd_doc_insert_block(b->d, pd_doc_root(b->d), -1, PD_BLOCK_SECTION, &sec) != PD_OK) {
        return 0;
    }

    if (sp) {
        pd_doc_set_section_props(b->d, sec, sp);
    }

    bld_tidy(b, &b->st[0]);
    b->st[0].id = sec;
    b->st[0].fresh = pd_doc_child(b->d, sec, 0);
    return sec;
}

pd_status bld_finish(pd_bld* b) {
    while (b->depth > 1) {
        bld_pop(b);
    }

    bld_end_para(b);
    bld_tidy(b, &b->st[0]);
    pb_free(&b->pend);
    pd_doc_clear_undo(b->d);
    return b->err;
}

/* ------------------------------------------------------------------ */
/* copying between documents (clipboard)                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const pd_doc* s;
    pd_doc* d;
    pd_style_id smap[512];
    uint8_t sdone[512];
    pd_list_id lmap[256];
    pd_res_id rmap[1024];
} copier;

static pd_style_id map_style(copier* C, pd_style_id sid, int depth) {
    const char* name;
    pd_style_id did;
    int32_t kind;
    pd_style_id parent;
    pd_para_props pp;
    pd_char_props cp;

    if (sid == 0 || depth > 16) {
        return 0;
    }

    if (sid < 512 && C->sdone[sid]) {
        return C->smap[sid];
    }

    if ((name = pd_doc_style_name(C->s, sid)) == NULL) {
        return 0;
    }

    did = pd_doc_style_find(C->d, name);

    if (!did && pd_doc_style_info(C->s, sid, &kind, &parent, &pp, &cp) == PD_OK) {
        if (sid < 512) {    /* a style naming itself as next is not a cycle to follow */
            C->sdone[sid] = 1;
            C->smap[sid] = 0;
        }

        parent = map_style(C, parent, depth + 1);

        if (pp.mask & PD_PP_NEXT_STYLE) {
            pp.next_style = map_style(C, pp.next_style, depth + 1);
        }

        cp.mask &= ~PD_CP_LINK;
        pd_doc_style_define(C->d, name, (pd_style_kind)kind, parent, &pp, &cp, &did);
    }

    if (sid < 512) {
        C->sdone[sid] = 1;
        C->smap[sid] = did;
    }

    return did;
}

static pd_format_id map_format(copier* C, pd_format_id f) {
    pd_style_id cs = 0;
    pd_char_props ov;

    if (f == 0 || pd_doc_format_info(C->s, f, &cs, &ov) != PD_OK) {
        return 0;
    }

    ov.mask &= ~PD_CP_LINK;

    if (ov.mask & PD_CP_REVISION) {     /* a tracked change keeps its author and date */
        pd_revision rv;

        if (pd_doc_revision_get(C->s, ov.revision, &rv) != PD_OK || pd_doc_revision_add(C->d, &rv, &ov.revision) != PD_OK) {
            ov.mask &= ~PD_CP_REVISION;
        }
    }

    return pd_doc_format(C->d, map_style(C, cs, 0), &ov);
}

static pd_list_id map_list(copier* C, pd_list_id l) {
    pd_list_level lv[9];
    int32_t n;

    if (l == 0 || l >= 256) {
        return 0;
    }

    if (!C->lmap[l] && pd_doc_list_info(C->s, l, &n, lv) == PD_OK) {
        pd_doc_list_define(C->d, n, lv, &C->lmap[l]);
    }

    return C->lmap[l];
}

static pd_status copy_block(copier* C, pd_block_id src, pd_block_id parent, int32_t index, pd_block_id* out);

static pd_status copy_children(copier* C, pd_block_id src, pd_block_id dst) {
    pd_block_info bi;
    pd_block_id first = pd_doc_child(C->d, dst, 0);
    int32_t i;
    pd_status st = PD_OK;

    pd_doc_block_info(C->s, src, &bi);

    for (i = 0; i < bi.child_count && st == PD_OK; i++) {
        st = copy_block(C, pd_doc_child(C->s, src, i), dst, -1, NULL);
    }

    if (first && bi.child_count > 0) {  /* the container's own initial paragraph */
        pd_doc_remove_block(C->d, first);
    }

    return st;
}

/* A drawing whose text boxes are stories: the stories copied too, and the drawing made again naming the copies --
   each copy of a drawing its own text, not the original's (nor, in another document, nothing) */
static int copy_drawing_stories(copier* C, const char* data, size_t len, pd_res_id* out) {
    pd_buf b;
    size_t i = 0, from = 0;
    int ok = 1;

    memset(&b, 0, sizeof(b));

    while (i + 8 < len) {
        if (!memcmp(data + i, "\"story\":", 8)) {
            size_t j = i + 8;
            long v = 0;
            pd_block_id story = 0;

            while (j < len && data[j] >= '0' && data[j] <= '9') {
                v = v * 10 + (data[j++] - '0');
            }

            if (pd_doc_insert_block(C->d, 0, -1, PD_BLOCK_STORY, &story) != PD_OK) {
                ok = 0;
                break;
            }

            if (v > 0) {
                copy_children(C, (pd_block_id)v, story);
            }

            pb_put(&b, data + from, i + 8 - from);
            pb_printf(&b, "%d", (int)story);
            from = j;
            i = j;
            continue;
        }

        i++;
    }

    pb_put(&b, data + from, len - from);
    ok = ok && !b.err && pd_doc_add_resource(C->d, "application/vnd.parade.drawing+json", b.p, b.n, out) == PD_OK;
    pb_free(&b);
    return ok;
}

static int copy_object(copier* C, const pd_inline* o, pd_inline* out) {
    *out = *o;

    if (o->kind == PD_INLINE_IMAGE) {
        const char* mime;
        const void* data;
        size_t len;

        if (o->resource >= 1024 || pd_doc_resource(C->s, o->resource, &mime, &data, &len) != PD_OK) {
            return 0;
        }

        if (!strcmp(mime, "application/vnd.parade.drawing+json") && len > 8) {
            const char* p = (const char*)data;
            size_t k;

            for (k = 0; k + 8 <= len; k++) {
                if (p[k] == '"' && !memcmp(p + k, "\"story\":", 8)) {
                    return copy_drawing_stories(C, p, len, &out->resource);
                }
            }
        }

        if (!C->rmap[o->resource] && pd_doc_add_resource(C->d, mime, data, len, &C->rmap[o->resource]) != PD_OK) {
            return 0;
        }

        out->resource = C->rmap[o->resource];
    } else if (o->kind == PD_INLINE_FOOTNOTE) {
        pd_block_id story;

        if (pd_doc_insert_block(C->d, 0, -1, PD_BLOCK_STORY, &story) != PD_OK) {
            return 0;
        }

        copy_children(C, o->target, story);
        out->target = story;
    } else if (o->kind == PD_INLINE_FIELD && (o->field == PD_FIELD_REF_NUMBER || o->field == PD_FIELD_REF_PAGE)) {
        out->target = 0;    /* block ids do not carry over */
    }

    return 1;
}

typedef struct {
    copier* C;
    pd_block_id dp;
    uint32_t at, from, to;
    pd_status st;
} content_ctx;

static int copy_span(void* user, const pd_span* sp) {
    content_ctx* x = (content_ctx*)user;
    pd_pos at;

    at.block = x->dp;
    at.offset = x->at;

    if (sp->is_object) {
        pd_inline o;

        if (sp->offset >= x->from && sp->offset < x->to && copy_object(x->C, &sp->obj, &o)) {
            if ((x->st = pd_doc_insert_inline(x->C->d, at, &o, NULL)) == PD_OK) {
                x->at += 3;
            }
        }
    } else {
        uint32_t a = sp->offset > x->from ? sp->offset : x->from;
        uint32_t e = sp->offset + sp->len < x->to ? sp->offset + sp->len : x->to;

        if (e > a) {
            x->st = pd_doc_insert_text(x->C->d, at, sp->text + (a - sp->offset), e - a, map_format(x->C, sp->format),
                                       NULL);
            x->at += x->st == PD_OK ? e - a : 0;
        }
    }

    return x->st != PD_OK;
}

static pd_status copy_content(copier* C, pd_block_id sp, uint32_t from, uint32_t to, pd_block_id dp, uint32_t* at) {
    content_ctx x;

    x.C = C;
    x.dp = dp;
    x.at = *at;
    x.from = from;
    x.to = to;
    x.st = PD_OK;
    pd_conv_spans_all(C->s, sp, copy_span, &x);
    *at = x.at;
    return x.st;
}

static void copy_attrs(copier* C, pd_block_id sp, pd_block_id dp) {
    pd_block_info bi;
    pd_para_props pp;

    if (pd_doc_block_info(C->s, sp, &bi) != PD_OK) {
        return;
    }

    if (bi.style) {
        pd_doc_set_para_style(C->d, dp, map_style(C, bi.style, 0));
    }

    if (bi.role != PD_ROLE_BODY) {
        pd_doc_set_role(C->d, dp, (pd_role)bi.role, bi.level);
    }

    if (bi.list) {
        pd_doc_set_list(C->d, dp, map_list(C, bi.list), bi.list_level);
    }

    if (pd_doc_para_props(C->s, sp, &pp) == PD_OK && pp.mask) {
        if (pp.mask & PD_PP_NEXT_STYLE) {
            pp.next_style = map_style(C, pp.next_style, 0);
        }

        pd_doc_set_para_props(C->d, dp, &pp);
    }

    {
        pd_para_attrs at;

        if (pd_doc_para_attrs(C->s, sp, &at) == PD_OK && (at.quote_depth || at.task || at.loose || at.lang[0] ||
                at.cont || at.div_class[0])) {
            pd_doc_set_para_attrs(C->d, dp, &at);
        }
    }
}

static pd_status copy_block(copier* C, pd_block_id src, pd_block_id parent, int32_t index, pd_block_id* out) {
    pd_block_info bi;
    pd_block_id nb;
    pd_status st;
    int32_t r, c;

    if (pd_doc_block_info(C->s, src, &bi) != PD_OK) {
        return PD_ERR_ARG;
    }

    if (bi.kind == PD_BLOCK_SECTION || bi.kind == PD_BLOCK_STORY || bi.kind == PD_BLOCK_ROW ||
            bi.kind == PD_BLOCK_CELL) {
        return PD_OK;   /* flattened by the callers */
    }

    if ((st = pd_doc_insert_block(C->d, parent, index, (pd_block_kind)bi.kind, &nb)) != PD_OK) {
        /* not allowed here (a float in a cell): its paragraphs instead */
        if (bi.kind == PD_BLOCK_FLOAT) {
            for (c = 0; c < bi.child_count; c++) {
                copy_block(C, pd_doc_child(C->s, src, c), parent, index < 0 ? -1 : index + c, NULL);
            }

            return PD_OK;
        }

        return st;
    }

    if (out) {
        *out = nb;
    }

    switch (bi.kind) {
        case PD_BLOCK_PARAGRAPH: {
            uint32_t at = 0;

            copy_attrs(C, src, nb);
            return copy_content(C, src, 0, UINT32_MAX, nb, &at);
        }

        case PD_BLOCK_BREAK:
            return pd_doc_set_break(C->d, nb, (pd_break_kind)bi.break_kind);

        case PD_BLOCK_FLOAT: {
            pd_float_props fp;

            if (pd_doc_float_props(C->s, src, &fp) == PD_OK) {
                pd_doc_set_float_props(C->d, nb, &fp);
            }

            return copy_children(C, src, nb);
        }

        case PD_BLOCK_TABLE: {
            pd_table_props tp;

            if (pd_doc_table_props(C->s, src, &tp) == PD_OK) {
                if (tp.style) {     /* its table style, by name: the target's of the name, else the source's made one */
                    const char* sn = pd_doc_style_name(C->s, tp.style);
                    pd_style_id ds = sn ? pd_doc_style_find(C->d, sn) : 0;
                    pd_table_style* ts;

                    if (sn && !ds && (ts = (pd_table_style*)malloc(sizeof(pd_table_style))) != NULL) {
                        if (pd_doc_table_style_resolve(C->s, tp.style, ts) != PD_OK ||
                                pd_doc_table_style_define(C->d, sn, 0, ts, &ds) != PD_OK) {
                            ds = 0;
                        }

                        free(ts);
                    }

                    tp.style = ds && pd_doc_table_style_info(C->d, ds, NULL, NULL) == PD_OK ? ds : 0;
                }

                pd_doc_set_table_props(C->d, nb, &tp);
            }

            for (r = 0; r < bi.child_count; r++) {
                pd_block_id srow = pd_doc_child(C->s, src, r), drow;
                pd_block_info ri;

                if (r == 0) {
                    drow = pd_doc_child(C->d, nb, 0);
                } else if (pd_doc_insert_block(C->d, nb, -1, PD_BLOCK_ROW, &drow) != PD_OK) {
                    return PD_ERR_STATE;
                }

                pd_doc_block_info(C->s, srow, &ri);

                for (c = 0; c < ri.child_count; c++) {
                    pd_block_id scell = pd_doc_child(C->s, srow, c), dcell;
                    pd_cell_props cp;

                    if (c == 0) {
                        dcell = pd_doc_child(C->d, drow, 0);
                    } else if (pd_doc_insert_block(C->d, drow, -1, PD_BLOCK_CELL, &dcell) != PD_OK) {
                        return PD_ERR_STATE;
                    }

                    if (pd_doc_cell_props(C->s, scell, &cp) == PD_OK) {
                        pd_doc_set_cell_props(C->d, dcell, &cp);
                    }

                    copy_children(C, scell, dcell);
                }
            }

            return PD_OK;
        }
    }

    return PD_OK;
}

static void copier_init(copier* C, const pd_doc* s, pd_doc* d) {
    memset(C, 0, sizeof(*C));
    C->s = s;
    C->d = d;
}

/* ------------------------------------------------------------------ */
/* plain text                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_buf* out;
    pd_numbers* nb;
    pd_block_id para;
} text_ctx;

static int text_span(void* user, const pd_span* sp) {
    text_ctx* x = (text_ctx*)user;

    if (!sp->is_object) {
        pb_put(x->out, sp->text, sp->len);
    } else if (sp->obj.kind == PD_INLINE_FOOTNOTE) {
        pb_printf(x->out, "[%d]", (int)pd_numbers_at(x->nb, x->para, sp->offset));
    } else if (sp->obj.kind == PD_INLINE_FIELD && sp->obj.field == PD_FIELD_SEQ) {
        pb_printf(x->out, "%d", (int)pd_numbers_at(x->nb, x->para, sp->offset));
    } else if (sp->obj.kind == PD_INLINE_FIELD && sp->obj.field == PD_FIELD_REF_NUMBER) {
        pb_printf(x->out, "%d", (int)pd_numbers_ref(x->nb, sp->obj.target));
    } else if (sp->obj.kind == PD_INLINE_EQUATION && sp->obj.source) {
        pb_put(x->out, sp->obj.source, (size_t)sp->obj.source_len);
    } else if (sp->obj.kind == PD_INLINE_TAB) {
        pb_putc(x->out, '\t');
    }

    return x->out->err;
}

static void text_walk(const pd_doc* d, pd_block_id id, text_ctx* x) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(d, id, &bi) != PD_OK) {
        return;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        char label[32];

        if (bi.list && pd_doc_list_label(d, id, label, sizeof(label)) == PD_OK && label[0]) {
            pb_printf(x->out, "%*s%s ", (int)(bi.list_level * 2), "", label);
        }

        x->para = id;
        pd_conv_spans(d, id, text_span, x);
        pb_putc(x->out, '\n');
        return;
    }

    if (bi.kind == PD_BLOCK_BREAK && bi.break_kind == PD_BREAK_RULE) {
        pb_puts(x->out, "----------\n");
        return;
    }

    for (i = 0; i < bi.child_count; i++) {
        text_walk(d, pd_doc_child(d, id, i), x);

        if (bi.kind == PD_BLOCK_ROW && i + 1 < bi.child_count && x->out->n && x->out->p[x->out->n - 1] == '\n') {
            x->out->p[x->out->n - 1] = '\t';    /* cells of a row on one line */
        }
    }
}

static pd_status text_export(const pd_doc* d, pd_buf* out) {
    pd_numbers nb;
    text_ctx x;

    pd_numbers_init(&nb, d);
    x.out = out;
    x.nb = &nb;
    x.para = 0;
    text_walk(d, pd_doc_root(d), &x);
    pd_numbers_free(&nb);
    return out->err ? PD_ERR_NOMEM : PD_OK;
}

static pd_status text_import(pd_doc* d, const char* s, size_t n) {
    pd_bld b;
    size_t i = 0;

    bld_init(&b, d);

    if (n >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        i = 3;  /* BOM */
    }

    while (i < n) {
        size_t e = i;

        while (e < n && s[e] != '\n' && s[e] != '\r') {
            e++;
        }

        bld_begin_para(&b);
        bld_text(&b, s + i, e - i);
        bld_end_para(&b);
        i = e + (e < n && s[e] == '\r' && e + 1 < n && s[e + 1] == '\n' ? 2 : 1);
    }

    return bld_finish(&b);
}

/* ------------------------------------------------------------------ */
/* public entry points                                                */
/* ------------------------------------------------------------------ */

static int utf8_ok(const char* s, size_t n) {
    size_t i = 0;

    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        size_t k, len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;

        if (!len || i + len > n) {
            return 0;
        }

        for (k = 1; k < len; k++) {
            if (((unsigned char)s[i + k] >> 6) != 2) {
                return 0;
            }
        }

        i += len;
    }

    return 1;
}

/* text that is not valid UTF-8 is taken as Windows-1252 (as browsers do for unlabeled pages) */
static char* to_utf8(const char* s, size_t n, size_t* out_n) {
    static const uint16_t w1252[32] = {
        0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152,
        0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122,
        0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178
    };
    char* o = (char*)malloc(n * 3 + 1);
    size_t i, k = 0;

    if (!o) {
        return NULL;
    }

    for (i = 0; i < n; i++) {
        uint32_t c = (unsigned char)s[i];

        if (c >= 0x80 && c < 0xA0) {
            c = w1252[c - 0x80];
        }

        if (c < 0x80) {
            o[k++] = (char)c;
        } else if (c < 0x800) {
            o[k++] = (char)(0xC0 | (c >> 6));
            o[k++] = (char)(0x80 | (c & 0x3F));
        } else {
            o[k++] = (char)(0xE0 | (c >> 12));
            o[k++] = (char)(0x80 | ((c >> 6) & 0x3F));
            o[k++] = (char)(0x80 | (c & 0x3F));
        }
    }

    o[k] = '\0';
    *out_n = k;
    return o;
}

pd_status pd_doc_export(const pd_doc* d, pd_conv_format fmt, pd_writer fn, void* user) {
    pd_buf out;
    pd_status st;

    if (!d || !fn) {
        return PD_ERR_ARG;
    }

    memset(&out, 0, sizeof(out));

    switch (fmt) {
        case PD_CONV_TEXT:
            st = text_export(d, &out);
            break;

        case PD_CONV_HTML:
            st = pd_html_export(d, &out);
            break;

        case PD_CONV_MARKDOWN:
            st = pd_md_export(d, &out);
            break;

        case PD_CONV_LATEX:
            st = pd_latex_export(d, &out);
            break;

        case PD_CONV_RTF:
            st = pd_rtf_export(d, &out);
            break;

        case PD_CONV_DOCX:
        case PD_CONV_DOTX:
            st = pd_docx_export_as(d, &out, fmt == PD_CONV_DOTX);
            break;

        case PD_CONV_JDATA:
            return pd_doc_save(d, PD_JDATA_TEXT, fn, user);

        default:
            return PD_ERR_ARG;
    }

    if (st != PD_OK) {
        pb_free(&out);
        return st;
    }

    return pb_flush(&out, fn, user);
}

pd_status pd_doc_import(const void* data, size_t len, pd_conv_format fmt, pd_doc** out) {
    pd_doc* d;
    pd_status st;
    int tmpl = 0;
    const char* s = (const char*)data;
    char* conv = NULL;

    if (!out || (!data && len)) {
        return PD_ERR_ARG;
    }

    *out = NULL;

    if (fmt == PD_CONV_JDATA) {
        return pd_doc_load(data, len, PD_JDATA_AUTO, out);
    }

    if (fmt == PD_CONV_DOTX) {  /* a template: a document of what it has, with every style it has */
        tmpl = 1;
        fmt = PD_CONV_DOCX;
    }

    if (fmt == PD_CONV_LATEX || (int)fmt < 0 || fmt > PD_CONV_PPTX) {
        return PD_ERR_ARG;
    }

    /* text formats: UTF-8, or Windows-1252 when it is not valid UTF-8 (RTF does its own decoding) */
    if (fmt != PD_CONV_DOCX && fmt != PD_CONV_PPTX && fmt != PD_CONV_RTF && !utf8_ok(s, len)) {
        if ((conv = to_utf8(s, len, &len)) == NULL) {
            return PD_ERR_NOMEM;
        }

        s = conv;
    }

    if ((st = pd_doc_new(&d)) != PD_OK) {
        free(conv);
        return st;
    }

    switch (fmt) {
        case PD_CONV_TEXT:
            st = text_import(d, s, len);
            break;

        case PD_CONV_HTML:
            st = pd_html_import(d, s, len);
            break;

        case PD_CONV_MARKDOWN:
            st = pd_md_import(d, s, len);
            break;

        case PD_CONV_RTF:
            st = pd_rtf_import(d, s, len);
            break;

        case PD_CONV_DOCX:
            st = pd_docx_import_ex(d, (const unsigned char*)s, len, tmpl);
            break;

        case PD_CONV_PPTX:
            st = pd_pptx_import(d, (const unsigned char*)s, len);
            break;

        default:
            st = PD_ERR_ARG;
    }

    free(conv);

    if (st != PD_OK) {
        pd_doc_free(d);
        return st;
    }

    *out = d;
    return PD_OK;
}

pd_conv_format pd_conv_detect(const void* data, size_t len) {
    const char* s = (const char*)data;
    size_t i = 0, k;

    if (!s || len == 0) {
        return PD_CONV_TEXT;
    }

    if (len >= 4 && memcmp(s, "PK\x03\x04", 4) == 0) {   /* a package: a presentation, or a Word document */
        for (k = 0; k + 20 <= len && k < ((size_t)8 << 20); k++) {
            if (s[k] == 'p' && memcmp(s + k, "ppt/presentation.xml", 20) == 0) {
                return PD_CONV_PPTX;
            }
        }

        return PD_CONV_DOCX;
    }

    if (len >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        i = 3;
    }

    while (i < len && isspace((unsigned char)s[i])) {
        i++;
    }

    if (len - i >= 5 && memcmp(s + i, "{\\rtf", 5) == 0) {
        return PD_CONV_RTF;
    }

    if (s[i] == '{' && len - i > 12) {
        for (k = i; k + 10 < len && k < i + 200; k++) {
            if (memcmp(s + k, "_DataInfo_", 10) == 0) {
                return PD_CONV_JDATA;
            }
        }
    }

    if (s[i] == '<') {
        return PD_CONV_HTML;
    }

    /* Markdown if a line starts like a Markdown block, or inline links/emphasis appear */
    for (k = i; k < len; k++) {
        if (k == i || s[k - 1] == '\n') {
            const char* p = s + k;
            size_t left = len - k;

            if ((left > 2 && p[0] == '#' && (p[1] == ' ' || p[1] == '#')) || (left > 3 && !memcmp(p, "```", 3)) ||
                    (left > 2 && (p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') ||
                    (left > 2 && p[0] == '>' && p[1] == ' ') || (left > 3 && p[0] == '|' && memchr(p + 1, '|', left - 1))) {
                return PD_CONV_MARKDOWN;
            }
        }

        if (k + 3 < len && ((s[k] == '*' && s[k + 1] == '*') || (s[k] == ']' && s[k + 1] == '('))) {
            return PD_CONV_MARKDOWN;
        }
    }

    return PD_CONV_TEXT;
}

/* a fragment document holding the paragraphs of a range */
static pd_status make_fragment(const pd_doc* d, pd_range r, pd_doc** out) {
    pd_doc* f;
    copier C;
    pd_block_id p = r.start.block, sec, first;
    pd_status st;
    int single = r.start.block == r.end.block;

    if ((st = pd_doc_new(&f)) != PD_OK) {
        return st;
    }

    copier_init(&C, d, f);
    sec = pd_doc_child(f, pd_doc_root(f), 0);
    first = pd_doc_child(f, sec, 0);

    while (p && st == PD_OK) {
        pd_block_id dp = first;
        uint32_t at = 0, from = p == r.start.block ? r.start.offset : 0, to = p == r.end.block ? r.end.offset : UINT32_MAX;

        if (!dp) {
            st = pd_doc_insert_block(f, sec, -1, PD_BLOCK_PARAGRAPH, &dp);
        }

        first = 0;

        if (st == PD_OK && !single) {
            copy_attrs(&C, p, dp);
        }

        if (st == PD_OK) {
            st = copy_content(&C, p, from, to, dp, &at);
        }

        if (p == r.end.block) {
            break;
        }

        p = pd_doc_next_paragraph(d, p);
    }

    pd_doc_clear_undo(f);

    if (st != PD_OK) {
        pd_doc_free(f);
        return st;
    }

    *out = f;
    return PD_OK;
}

pd_status pd_doc_export_range(const pd_doc* d, pd_range r, pd_conv_format fmt, pd_writer fn, void* user) {
    pd_doc* f;
    pd_status st;
    pd_block_info a, b;

    if (!d || !fn || pd_doc_block_info(d, r.start.block, &a) != PD_OK || pd_doc_block_info(d, r.end.block, &b) != PD_OK ||
            a.kind != PD_BLOCK_PARAGRAPH || b.kind != PD_BLOCK_PARAGRAPH ||
            (r.start.block == r.end.block && r.start.offset > r.end.offset)) {
        return PD_ERR_ARG;
    }

    if ((st = make_fragment(d, r, &f)) != PD_OK) {
        return st;
    }

    st = pd_doc_export(f, fmt, fn, user);
    pd_doc_free(f);
    return st;
}

/* top-level blocks of a fragment: the children of all its sections */
static int32_t fragment_items(const pd_doc* f, pd_block_id* items, int32_t cap) {
    pd_block_info ri, si;
    int32_t s, k, n = 0;

    pd_doc_block_info(f, pd_doc_root(f), &ri);

    for (s = 0; s < ri.child_count; s++) {
        pd_block_id sec = pd_doc_child(f, pd_doc_root(f), s);

        pd_doc_block_info(f, sec, &si);

        for (k = 0; k < si.child_count; k++) {
            if (n < cap) {
                items[n] = pd_doc_child(f, sec, k);
            }

            n++;
        }
    }

    return n < cap ? n : cap;
}

static int is_para(const pd_doc* d, pd_block_id id) {
    pd_block_info bi;
    return pd_doc_block_info(d, id, &bi) == PD_OK && bi.kind == PD_BLOCK_PARAGRAPH;
}

pd_status pd_doc_paste(pd_doc* d, pd_pos at, const void* data, size_t len, pd_conv_format fmt, pd_pos* after) {
    pd_doc* f = NULL;
    pd_block_id* items;
    pd_block_info pi;
    copier C;
    int32_t n, i, k = 0, last = -1;
    pd_status st;
    pd_pos q;
    uint32_t off;

    if (!d || pd_doc_block_info(d, at.block, &pi) != PD_OK || pi.kind != PD_BLOCK_PARAGRAPH || at.offset > pi.text_length) {
        return PD_ERR_ARG;
    }

    if ((st = pd_doc_import(data, len, fmt, &f)) != PD_OK) {
        return st;
    }

    if ((items = (pd_block_id*)malloc(65536 * sizeof(pd_block_id))) == NULL) {
        pd_doc_free(f);
        return PD_ERR_NOMEM;
    }

    n = fragment_items(f, items, 65536);
    copier_init(&C, f, d);
    pd_doc_begin_group(d, "Paste");

    if (n == 1 && is_para(f, items[0])) {   /* inline: into the paragraph at the caret */
        off = at.offset;
        st = copy_content(&C, items[0], 0, UINT32_MAX, at.block, &off);
        q.block = at.block;
        q.offset = off;
    } else if ((st = pd_doc_split(d, at, &q)) == PD_OK) {
        pd_block_id P = at.block, parent;
        int32_t index;

        pd_doc_block_info(d, P, &pi);
        parent = pi.parent;
        index = pi.index + 1;

        if (n > 0 && is_para(f, items[0])) {    /* the first paragraph joins the head */
            off = pi.text_length;

            if (off == 0) {
                copy_attrs(&C, items[0], P);
            }

            st = copy_content(&C, items[0], 0, UINT32_MAX, P, &off);
            k = 1;
        }

        if (n > k && is_para(f, items[n - 1])) {
            last = n - 1;
        }

        for (i = k; i < n && st == PD_OK; i++) {
            if (i == last) {
                break;
            }

            st = copy_block(&C, items[i], parent, index++, NULL);
        }

        if (st == PD_OK && last >= 0) {     /* the last one joins the tail */
            off = 0;
            st = copy_content(&C, items[last], 0, UINT32_MAX, q.block, &off);
            q.offset = off;
        }
    }

    pd_doc_end_group(d);
    free(items);
    pd_doc_free(f);

    if (st == PD_OK && after) {
        *after = q;
    }

    return st;
}
