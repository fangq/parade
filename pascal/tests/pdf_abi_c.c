/* the PDF reader's records as C lays them out (pdf_abi_test.pas prints the same from Pascal) */
#include <stddef.h>
#include <stdio.h>
#include "parade_pdfread.h"

int main(void) {
    printf("char %zu %zu %zu %zu\n", sizeof(pd_pdfr_char), offsetof(pd_pdfr_char, x0), offsetof(pd_pdfr_char, size),
           offsetof(pd_pdfr_char, run));
    printf("annot %zu %zu %zu %zu %zu %zu %zu\n", sizeof(pd_pdfr_annot), offsetof(pd_pdfr_annot, x0),
           offsetof(pd_pdfr_annot, color), offsetof(pd_pdfr_annot, width), offsetof(pd_pdfr_annot, field),
           offsetof(pd_pdfr_annot, contents), offsetof(pd_pdfr_annot, uri));
    printf("field %zu %zu %zu %zu %zu %zu %zu\n", sizeof(pd_pdfr_field), offsetof(pd_pdfr_field, value),
           offsetof(pd_pdfr_field, page), offsetof(pd_pdfr_field, x0), offsetof(pd_pdfr_field, readonly),
           offsetof(pd_pdfr_field, maxlen), offsetof(pd_pdfr_field, font_size));
    printf("span %zu %zu %zu %zu %zu\n", sizeof(pd_pdfr_span), offsetof(pd_pdfr_span, x0), offsetof(pd_pdfr_span, size),
           offsetof(pd_pdfr_span, lines), offsetof(pd_pdfr_span, align));
    printf("bookmark %zu %zu %zu %zu %zu\n", sizeof(pd_pdfr_bookmark), offsetof(pd_pdfr_bookmark, page),
           offsetof(pd_pdfr_bookmark, top), offsetof(pd_pdfr_bookmark, title), offsetof(pd_pdfr_bookmark, uri));
    return 0;
}
