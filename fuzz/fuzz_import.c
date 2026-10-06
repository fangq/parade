/* fuzz target: the importers (text, HTML, Markdown, RTF, DOCX, JData), Windows metafiles, paste, and export
   of the result */
#include <stdint.h>
#include <stdlib.h>
#include "parade_convert.h"
#include "../src/pd_conv.h"
#include "fuzz_common.h"

/* first byte picks the format (or detection), the rest is the input */
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static const pd_conv_format formats[] = {PD_CONV_TEXT, PD_CONV_HTML, PD_CONV_MARKDOWN,
                                             PD_CONV_RTF, PD_CONV_DOCX, PD_CONV_JDATA
                                            };
    pd_conv_format in;
    pd_doc* d = NULL;
    pd_doc* p = NULL;
    pd_pos at, after;
    size_t bytes = 0;
    int fmt;

    if (size < 1) {
        return 0;
    }

    in = data[0] < 6 * 40 ? formats[data[0] % 6] : pd_conv_detect(data + 1, size - 1);
    data++;
    size--;

    if (pd_metafile_kind(data, size)) {     /* an EMF or WMF, played into a drawing */
        pd_doc* m = NULL;

        if (pd_doc_new(&m) == PD_OK) {
            pd_metafile_drawing(m, data, size, 0);
            pd_doc_free(m);
        }
    }

    if (pd_doc_import(data, size, in, &d) == PD_OK) {
        for (fmt = PD_CONV_TEXT; fmt <= PD_CONV_JDATA; fmt++) {
            pd_doc_export(d, (pd_conv_format)fmt, fuzz_discard, &bytes);
        }

        pd_doc_free(d);
    }

    /* the same data pasted into the middle of a two-paragraph document */
    if (pd_doc_import("first line\nsecond line", 22, PD_CONV_TEXT, &p) == PD_OK) {
        at.block = pd_doc_next_paragraph(p, 0);
        at.offset = 5;

        if (at.block && pd_doc_paste(p, at, data, size, in, &after) == PD_OK) {
            pd_doc_export(p, PD_CONV_HTML, fuzz_discard, &bytes);
            pd_doc_undo(p);
        }

        pd_doc_free(p);
    }

    return 0;
}
