/* SmartArt laid out from its definition (pd_dgm.c): for the PowerPoint and Word readers, when a diagram has no
   drawing saved of it */
#ifndef PD_DGM_H
#define PD_DGM_H

#include "pd_xdoc.h"

typedef struct {
    const xdoc* data;           /* the data model (dgm:dataModel) */
    const xdoc* layout;         /* its layout definition (dgm:layoutDef) */
    const xdoc* style;          /* its quick style (dgm:styleDef), NULL for none */
    const xdoc* colors;         /* its colours (dgm:colorsDef), NULL for none */
    double cx, cy;              /* the frame it is in (EMU) */
    double line_w[3];           /* the theme's line widths, lnRef 1 to 3 (EMU) */
    const char* font;           /* the face its text is in (the theme's minor font), for measuring it */
} pd_dgm_in;

/* the diagram laid out as the drawing PowerPoint saves of one: a dsp:drawing, its shapes in the frame's
   coordinates; 0 when it cannot be (a definition or data model not as the schema has them) */
int pd_dgm_layout(const pd_dgm_in* in, pd_buf* out);

#endif
