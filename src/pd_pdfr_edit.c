/*
 * Parade PDF reader: annotations, form fields and the incremental update
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr_dl.h"

void pd_annot_appearance(pdoc* d, pv annot, pv* form) {
    (void)d;
    (void)annot;
    *form = pv_null();
}
