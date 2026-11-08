/* What a display-list path is painted with, worked out once for every output (pd_raster.c): the rasterizer, PDF
   and SVG read the same gradient from an item, whether it has a style of its own or only fill, fill2 and grad. */
#ifndef PD_PAINT_H
#define PD_PAINT_H

#include "parade_layout.h"

typedef struct {
    int kind;                   /* PD_GRAD_LINEAR, PD_GRAD_CIRCLE, PD_GRAD_RECT */
    int n;                      /* stops: 2 or more, the first at 0, the last at 65536 */
    uint32_t color[PD_MAX_STOPS + 2];
    int32_t pos[PD_MAX_STOPS + 2];  /* 0..65536, not decreasing */
    double x0, y0, x1, y1;      /* page sp, as pd_path_style's grad_* */
    double bx0, by0, bx1, by1;
    int alpha;                  /* a stop is see-through */
} pd_gradient;

/* the gradient an item's fill is, 0 when it is one colour */
int pd_draw_gradient(const pd_draw* a, pd_gradient* g);

/* where a page point is along a gradient, 0..65536 */
int32_t pd_gradient_at(const pd_gradient* g, double x, double y);

/* the gradient's colour at t (0..65536), 0xAARRGGBB */
uint32_t pd_gradient_color(const pd_gradient* g, int32_t t);

#endif
