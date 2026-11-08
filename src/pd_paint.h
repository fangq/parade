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

/* an item's layers, as pd_draw_render draws them in turn */
#define PD_LAYER_UNDER  1       /* its glow and soft shadow */
#define PD_LAYER_BODY   2       /* its fill (with its soft edge) */
#define PD_LAYER_INNER  4       /* its inner shadow */
#define PD_LAYER_STROKE 8       /* its outline */
#define PD_LAYER_REFL   16      /* its reflection */
#define PD_LAYER_ALL    31

/* the layers of an item that only pixels can draw (PD_LAYER_UNDER, a softened BODY, INNER, REFL); 0 for none */
int pd_draw_effects(const pd_draw* a);

/* an item's layers into an RGBA image, as pd_draw_render */
pd_status pd_draw_render_layers(const pd_draw* a, int layers, pd_sp x0, pd_sp y0, int32_t scale, uint8_t* rgba,
                                int32_t width, int32_t height, int32_t stride);

#endif
