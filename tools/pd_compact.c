/*
 * pd_compact: many yrs updates in, one out -- the relay's log compaction.
 *
 * Reads updates from stdin, each as [length u32 big-endian][bytes] (the
 * first is usually the previous snapshot), applies them in order to an
 * empty yrs document and writes the document's whole state to stdout as
 * one update. Deleted content is kept (as pd_sync keeps it), so editors
 * that undo across the compaction still find what they bring back. Exit
 * status 1 when an update does not apply or the input is cut short, and
 * then nothing is written: the relay keeps its log as it was.
 *
 * Built with `make SYNC=yrs` (build-sync/pd_compact).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libyrs.h"

int main(void) {
    YOptions o = yoptions();
    YDoc* d;
    YTransaction* t;
    unsigned char h[4];
    uint32_t n, out = 0;
    char* buf = NULL, *state;
    size_t cap = 0;
    long count = 0;

    o.flags = Y_OFFSET_UTF16 | Y_SKIP_GC;
    d = ydoc_new_with_options(o);

    while (fread(h, 1, 4, stdin) == 4) {
        n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];

        if (n > cap) {
            char* p = (char*)realloc(buf, n);

            if (!p) {
                fprintf(stderr, "pd_compact: out of memory\n");
                return 1;
            }

            buf = p;
            cap = n;
        }

        if (fread(buf, 1, n, stdin) != n) {
            fprintf(stderr, "pd_compact: input cut short\n");
            return 1;
        }

        t = ydoc_write_transaction(d, 0, NULL);

        if (ytransaction_apply(t, buf, n) != 0) {
            ytransaction_commit(t);
            fprintf(stderr, "pd_compact: update %ld does not apply\n", count);
            return 1;
        }

        ytransaction_commit(t);
        count++;
    }

    t = ydoc_read_transaction(d);

    {   /* an update still waiting for one it depends on means the log has a hole: not compacted */
        YPendingUpdate* pu = ytransaction_pending_update(t);

        if (pu) {
            ypending_update_destroy(pu);
            ytransaction_commit(t);
            fprintf(stderr, "pd_compact: updates missing in the log\n");
            return 1;
        }
    }

    state = ytransaction_state_diff_v1(t, NULL, 0, &out);
    ytransaction_commit(t);

    if (!state || fwrite(state, 1, out, stdout) != out || fflush(stdout) != 0) {
        fprintf(stderr, "pd_compact: could not write the state\n");
        return 1;
    }

    fprintf(stderr, "pd_compact: %ld updates, %u bytes\n", count, out);
    ybinary_destroy(state, out);
    free(buf);
    ydoc_destroy(d);
    return 0;
}
