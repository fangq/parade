/* fuzz target: native document loading (JData text and BJData), layout, display lists, PDF and export */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "parade_convert.h"
#include "parade_layout.h"
#include "fuzz_common.h"

static const pd_font* resolve(void* user, const char* family, int32_t weight, int32_t italic) {
    (void)user;
    (void)family;
    (void)weight;
    (void)italic;
    return fuzz_text_font();
}

/* a follower kept in step with the document by its deltas */
static pd_doc* follower;

static void to_follower(void* user, const char* json, size_t len) {
    (void)user;

    if (follower && pd_doc_apply_delta(follower, json, len) != PD_OK) {
        pd_doc_free(follower);      /* out of step: a real follower would reload */
        follower = NULL;
    }
}

typedef struct {
    unsigned char* p;
    size_t n;
} grow_buf;

static int to_buf(void* user, const void* data, size_t len) {
    grow_buf* b = (grow_buf*)user;
    unsigned char* q = (unsigned char*)realloc(b->p, b->n + len);

    if (!q) {
        return 1;
    }

    memcpy(q + b->n, data, len);
    b->p = q;
    b->n += len;
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    pd_doc* d = NULL;
    pd_layout* L = NULL;
    pd_draw* items;
    pd_pos pos;
    pd_block_info bi;
    int32_t pg, n, page;
    pd_sp x, base, asc, desc;
    size_t bytes = 0;
    int fmt;

    /* malformed native data still reaches layout through the format-detecting importers */
    if (pd_doc_load(data, size, PD_JDATA_AUTO, &d) != PD_OK &&
            pd_doc_import(data, size, pd_conv_detect(data, size), &d) != PD_OK) {
        return 0;
    }

    for (fmt = PD_CONV_TEXT; fmt <= PD_CONV_JDATA; fmt++) {
        pd_doc_export(d, (pd_conv_format)fmt, fuzz_discard, &bytes);
    }

    pd_doc_save(d, PD_JDATA_BINARY, fuzz_discard, &bytes);

    if (fuzz_text_font()) {
        pd_doc_set_font_resolver(d, resolve, NULL);
        pd_doc_set_default_font(d, fuzz_text_font());
        pd_doc_set_microtype(d, size & 1, (size & 2) ? 20 : 0);

        if (fuzz_math_font()) {
            pd_doc_set_math_font(d, fuzz_math_font());
        }

        if (pd_layout_new(d, &L) == PD_OK && pd_layout_update(L, NULL) == PD_OK) {
            for (pg = 0; pg < pd_layout_page_count(L) && pg < 64; pg++) {
                n = 0;
                pd_layout_page_items(L, pg, NULL, 0, &n);
                items = n > 0 ? (pd_draw*)malloc(sizeof(pd_draw) * (size_t)n) : NULL;

                if (items) {
                    pd_layout_page_items(L, pg, items, n, &n);
                    free(items);
                }

                pd_layout_hit_test(L, pg, PD_PT(100), PD_PT(200), &pos);
                n = 0;
                pd_layout_page_markup(L, pg, NULL, 0, &n);
            }

            pos.block = pd_doc_next_paragraph(d, 0);
            pos.offset = 0;

            if (pos.block) {
                pd_layout_caret(L, pos, &page, &x, &base, &asc, &desc);
            }

            pd_layout_write_pdf(L, NULL, fuzz_discard, &bytes);

            /* review: tracked edits, then every change accepted or rejected, and undone -- followed by a
               copy through deltas, which also takes the input itself as one */
            if (pos.block) {
                grow_buf snap = { NULL, 0 };

                if (pd_doc_snapshot(d, PD_JDATA_BINARY, to_buf, &snap, to_follower, NULL) == PD_OK &&
                        pd_doc_load(snap.p, snap.n, PD_JDATA_BINARY, &follower) != PD_OK) {
                    follower = NULL;
                }

                free(snap.p);
            }

            if (pos.block) {
                pd_range all;
                pd_pos e = pos, q;
                pd_block_id nb;

                for (nb = pos.block; nb; nb = pd_doc_next_paragraph(d, nb)) {
                    e.block = nb;
                }

                pd_doc_block_info(d, e.block, &bi);
                e.offset = bi.text_length;
                all.start = pos;
                all.end = e;
                pd_doc_set_tracking(d, "fuzz");
                pd_doc_insert_text(d, pos, "ab", 2, PD_FORMAT_INHERIT, &q);
                if (e.block == pos.block) {
                    all.end.offset += 2;
                }
                pd_doc_delete(d, all, &q);
                pd_doc_set_tracking(d, NULL);
                pd_doc_revision_resolve(d, all, (int32_t)(size & 1));
                pd_layout_update(L, NULL);
                pd_doc_undo(d);
                pd_doc_undo(d);
                pd_doc_undo(d);
                pd_doc_snapshot(d, PD_JDATA_TEXT, NULL, NULL, NULL, NULL);

                if (follower) {
                    pd_doc_apply_delta(follower, (const char*)data, size);
                    pd_doc_save(follower, PD_JDATA_BINARY, fuzz_discard, &bytes);
                    pd_doc_free(follower);
                    follower = NULL;
                }
            }
        }

        pd_layout_free(L);
    }

    pd_doc_free(d);
    return 0;
}
