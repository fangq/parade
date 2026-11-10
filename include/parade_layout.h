/*
 * Parade page builder: a document laid out into pages
 *
 * A pd_layout turns a pd_doc into pages: sections give page geometry and
 * columns, paragraphs are broken with the paragraph engine, lines are cut
 * into columns TeX-style (box/glue/penalty with widow, orphan and keep
 * rules), floats are placed LaTeX-style (here, top, bottom, float page),
 * and headers/footers are set per page with their page-number fields.
 *
 * The result is a per-page display list of positioned glyphs, images,
 * object boxes and rules, in page coordinates (sp, origin at the page's
 * top-left, y down; glyph y is the baseline), plus hit testing and caret
 * mapping between pages and document positions.
 *
 * Updating is incremental: each paragraph's line breaking is cached and
 * redone only when the paragraph, its width or a style changed.
 *
 * Footnote bodies go to the bottom of the column that shows their mark;
 * tables get automatic column widths, break between rows and repeat their
 * header rows; floats with wrap have the text flow beside them; continuous
 * sections share a page (multi-column ones balanced). Sections may ask for
 * optimal page breaking (PD_PAGES_OPTIMAL).
 */

#ifndef PARADE_LAYOUT_H
#define PARADE_LAYOUT_H

#include "parade_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_layout pd_layout;

/** the document must outlive the layout; the layout never modifies it */
PD_API pd_status pd_layout_new(const pd_doc* doc, pd_layout** out);
PD_API void      pd_layout_free(pd_layout* layout);

typedef struct {
    int32_t pages;
    int32_t paragraphs_broken;  /**< paragraphs (re)broken by this update */
    int32_t paragraphs_reused;  /**< paragraphs taken from the cache */
    int32_t float_pages;
    int32_t overfull;           /**< columns that could not hold their content */
    int32_t variants;           /**< paragraphs set a line looser or tighter by optimal page breaking */
} pd_layout_info;

/** bring the pages up to date with the document; info may be NULL */
PD_API pd_status pd_layout_update(pd_layout* layout, pd_layout_info* info);
/** drop all cached paragraph layouts, e.g. after changing the font resolver */
PD_API void      pd_layout_invalidate(pd_layout* layout);
/**
 * Hybrid line breaking for this layout: -1 as the document says
 * (pd_doc_stable_breaks, the default), 0 off (every paragraph broken as if
 * fresh, e.g. for export), 1 on. Takes effect as paragraphs are next broken.
 */
PD_API void      pd_layout_set_stable_breaks(pd_layout* layout, int32_t mode);

/* how comments (and, in PD_MARKUP_BALLOONS, deletions) are shown */
#define PD_LAYOUT_COMMENTS_NONE   0     /**< not drawn (pd_layout_page_markup lists them for the host) */
#define PD_LAYOUT_COMMENTS_MARGIN 1     /**< balloons in a pane right of the page */

/**
 * Comments drawn as Word prints its markup: when the document has comments,
 * every page is PD_LAYOUT_COMMENT_PANE wider (pd_page_info.width), and each
 * comment starting on a page is a balloon in that pane -- its author and
 * text, its replies under it -- joined by a line to where its range starts;
 * in PD_MARKUP_BALLOONS a deletion is one too ("Deleted: ..."). They are
 * display-list items (region 6), so screen and PDF show the same. Default
 * PD_LAYOUT_COMMENTS_NONE.
 */
PD_API void      pd_layout_set_comments(pd_layout* layout, int32_t mode);
#define PD_LAYOUT_COMMENT_PANE PD_PT(216)

PD_API int32_t   pd_layout_page_count(const pd_layout* layout);

typedef struct {
    pd_sp width, height;
    pd_block_id section;
    int32_t number;             /**< page number as printed */
    char label[16];             /**< the number in the section's format ("iv", "12") */
    int32_t float_page;         /**< 1 if the page holds only floats */
    pd_pos first, last;         /**< first and last main-flow positions on the page (block 0 if none) */
} pd_page_info;

PD_API pd_status pd_layout_page_info(const pd_layout* layout, int32_t page, pd_page_info* out);

typedef enum {
    PD_DRAW_GLYPH = 0,          /**< glyph of font at size, x/y = pen position on the baseline */
    PD_DRAW_IMAGE = 1,          /**< resource drawn into the rectangle x, y (top), w, h */
    PD_DRAW_BOX = 2,            /**< an inline object the host draws (equation, user object) */
    PD_DRAW_RULE = 3,           /**< filled rectangle: underline, strike, border, background */
    PD_DRAW_LINK = 4,           /**< link area (not drawn); source holds the URL */
    PD_DRAW_PATH = 5            /**< polygon or polyline through points: filled with fill, stroked in color at
                                     line_width (0: not stroked); x, y, w, h bound it */
} pd_draw_kind;

#define PD_PATH_CLOSED 1        /**< the last point of each ring joins its first */
#define PD_PATH_BREAK  INT32_MIN /**< a point (PD_PATH_BREAK, PD_PATH_BREAK) ends one ring and starts the next:
                                      rings filled together, non-zero winding, so holes stay holes */
#define PD_PATH_CUBIC  (INT32_MIN + 1)  /**< in a path's curve: (PD_PATH_CUBIC, 0), then a cubic Bezier's two control
                                             points and its end, from where the path is */

#define PD_CAP_FLAT    0        /**< a stroke's ends square at its end points */
#define PD_CAP_ROUND   1        /**< half a disc past each end */
#define PD_CAP_SQUARE  2        /**< half the width past each end */
#define PD_JOIN_ROUND  0        /**< corners rounded (what a path without a style has) */
#define PD_JOIN_BEVEL  1        /**< corners cut across */
#define PD_JOIN_MITER  2        /**< corners pointed, cut across where the point would pass 8 line widths */

#define PD_GRAD_LINEAR 1        /**< colours along the line from (grad_x0, grad_y0) to (grad_x1, grad_y1) */
#define PD_GRAD_CIRCLE 2        /**< out in circles from (grad_x0, grad_y0) to one through (grad_x1, grad_y1) */
#define PD_GRAD_RECT   3        /**< out in rectangles from the rectangle grad_x0..grad_x1, grad_y0..grad_y1 to the
                                     rectangle grad_bx0..grad_bx1, grad_by0..grad_by1 */
#define PD_MAX_STOPS   10

/** A path's further paint, where it has one (pd_draw.style): what the plain fields cannot say */
typedef struct {
    int32_t dash_count;         /**< 0: a solid stroke; else the lengths in dash, dash first, then gap, ... */
    pd_sp dash[8];              /**< on the page */
    int32_t cap;                /**< PD_CAP_* (of each dash too) */
    int32_t join;               /**< PD_JOIN_* */
    int32_t grad_kind;          /**< the fill's gradient, PD_GRAD_*; 0: none here (fill, fill2 and grad say) */
    int32_t nstops;             /**< its colour stops, in order */
    uint32_t stop_color[PD_MAX_STOPS];  /**< 0xAARRGGBB, alpha its own */
    int32_t stop_pos[PD_MAX_STOPS];     /**< where along it, 0 at its start to 100000 at its end */
    pd_sp grad_x0, grad_y0, grad_x1, grad_y1;       /**< on the page, as PD_GRAD_* says */
    pd_sp grad_bx0, grad_by0, grad_bx1, grad_by1;
    int32_t pattern;            /**< the fill a pattern: pattern_bits in pattern_fg on pattern_bg (0: clear) */
    uint32_t pattern_fg, pattern_bg;
    uint8_t pattern_bits[8];    /**< 8 rows of 8 cells, top first, the high bit leftmost, a set bit the foreground;
                                     a cell 3/4 pt square (a pixel at 96 dpi), the cells counted from the page's
                                     top left corner */
    uint32_t shadow_color;      /**< a soft shadow of the fill under it (0: none; a hard one is a path of its own) */
    pd_sp shadow_blur, shadow_dx, shadow_dy;    /**< its blur radius; how far it is cast */
    int32_t shadow_sx, shadow_sy;   /**< scaled (65536 = 1) ... */
    int32_t shadow_kx, shadow_ky;   /**< ... and skewed (60000ths of a degree) about (shadow_ox, shadow_oy) */
    pd_sp shadow_ox, shadow_oy;
    uint32_t inner_color;       /**< a shadow inside the fill, cast from its edge (0: none) */
    pd_sp inner_blur, inner_dx, inner_dy;
    uint32_t glow_color;        /**< a glow round the fill, glow_radius out (0: none) */
    pd_sp glow_radius;
    pd_sp soft_edge;            /**< the fill fading out over this much inside its edge (0: none) */
    int32_t refl_alpha0, refl_alpha1;   /**< a reflection of the shape below it (refl_alpha0 0: none), its opacity
                                             (100000ths) going from alpha0 at pos0 to alpha1 at pos1 ... */
    int32_t refl_pos0, refl_pos1;   /**< ... (100000ths of the shape's height down from the mirror line) */
    pd_sp refl_blur, refl_dist;     /**< blurred; the mirror line this far below the shape */
    pd_sp bevel_width;          /**< a bevel round the fill's edge this wide (0: none), lit from bevel_light */
    int32_t bevel_light;        /**< where the light comes from, 60000ths of a degree clockwise from the right
                                     (Office's top left rig: 225 degrees) */
    int32_t bevel_kind;         /**< PD_BEVEL_*: its profile */
} pd_path_style;

#define PD_BEVEL_ROUND  0       /**< a rounded rise (circle, softRound, convex, relaxedInset, ...) */
#define PD_BEVEL_ANGLE  1       /**< a flat slope, sharply lit (angle, slope, coolSlant, hardEdge, cross) */
#define PD_BEVEL_GROOVE 2       /**< a rim cut in (divot, riblet, artDeco): lit from the other side too */

#define PD_PATTERN_CELL 49152   /**< a pattern cell's side, sp */

typedef struct {
    int32_t kind;               /**< pd_draw_kind */
    pd_sp x, y, w, h;
    uint32_t glyph;
    const pd_font* font;
    pd_sp size;
    uint32_t color;
    pd_res_id resource;
    pd_block_id block;          /**< source paragraph */
    uint32_t offset;            /**< source byte offset */
    int32_t region;             /**< 0 body, 1 header, 2 footer, 3 float, 4 footnote, 5 a text box in a drawing, 6 a
                                     comment balloon (pd_layout_set_comments) */
    uint32_t text;              /**< glyphs: the code point shown (also for generated text), 0 if none */
    int32_t scale;              /**< glyphs: horizontal scale, 65536 = 1 (font expansion) */
    const pd_sp* points;        /**< paths: x, y pairs on the page; valid until the page is laid out or listed again */
    int32_t npoints;            /**< paths: points (pairs) */
    int32_t path_flags;         /**< paths: PD_PATH_* */
    pd_sp line_width;           /**< paths: stroke width, 0 = no stroke */
    uint32_t fill;              /**< paths: fill colour 0xAARRGGBB, 0 = not filled */
    pd_sp clip_x, clip_y, clip_w, clip_h;   /**< images: shown only within this (a cropped picture); clip_w 0: all */
    uint32_t fill2;             /**< paths: a gradient: the colour it goes to from fill (0xAARRGGBB) */
    int32_t grad;               /**< paths: 0 one colour, 1 a linear gradient (grad_angle), 2 a radial one, out from
                                     the middle */
    int32_t grad_angle;         /**< paths: the way a linear gradient runs, 60000ths of a degree clockwise from right */
    int32_t rotation;           /**< images: turned about their middle, 60000ths of a degree clockwise; glyphs (a
                                     turned shape's text): turned about the pen position */
    int32_t flip;               /**< images: PD_FLIP_H, PD_FLIP_V */
    const pd_sp* clip_points;   /**< images: shown only inside this polygon (x, y pairs on the page, as paths' are) */
    int32_t clip_npoints;
    const pd_path_style* style; /**< paths: dashes, caps, joins, ...; NULL for none (valid as points are) */
    const pd_sp* curve;         /**< paths: the same rings with their curves as curves (x, y pairs; PD_PATH_BREAK
                                     pairs between rings, a PD_PATH_CUBIC pair before each cubic's three points), for
                                     output that draws curves; NULL: points is all there is. points stays the path
                                     made of short chords, for hit testing and hosts that draw polygons */
    int32_t ncurve;             /**< paths: pairs in curve */
    int32_t shape;              /**< the top-level shape of a drawing it was drawn for (its text too): the shape's
                                     DrawingML id (cNvPr), as a slide's animations name it; 0 none or not known */
} pd_draw;

#define PD_FLIP_H 1             /**< mirrored left to right */
#define PD_FLIP_V 2             /**< upside down */

/** the display list of a page; same size-query convention as pd_para_get_glyphs */
PD_API pd_status pd_layout_page_items(const pd_layout* layout, int32_t page, pd_draw* buf, int32_t cap,
                                      int32_t* count);

typedef enum {
    PD_MARK_DELETION = 1,       /**< deleted text: a balloon in PD_MARKUP_BALLOONS, else struck through in place */
    PD_MARK_INSERTION = 2,      /**< inserted text (in the text, in the author's colour) */
    PD_MARK_COMMENT = 3,        /**< a comment (not its replies, which share its range) */
    PD_MARK_FORMAT = 4          /**< formatting changed (the text shown as it is now) */
} pd_mark_kind;

typedef struct {
    int32_t kind;               /**< pd_mark_kind */
    uint32_t id;                /**< pd_rev_id or pd_comment_id */
    pd_range range;             /**< the text concerned: a change's stretch of one paragraph, a comment's range */
    pd_sp x, y;                 /**< anchor on the page: where the range starts, on its baseline */
    pd_sp top, bottom;          /**< the lines it covers on this page, for a change bar */
    uint32_t color;             /**< the author's colour */
} pd_markup_item;

/**
 * The tracked changes and comments starting on a page, top to bottom, for
 * a host's margin balloons and change bars; same size-query convention as
 * pd_layout_page_items. Changes are not listed in PD_MARKUP_FINAL and
 * PD_MARKUP_ORIGINAL.
 */
PD_API pd_status pd_layout_page_markup(const pd_layout* layout, int32_t page, pd_markup_item* buf, int32_t cap,
                                       int32_t* count);

/** a table cell where it is on a page (a cell broken across pages: its part on each) */
typedef struct {
    pd_block_id cell;
    int32_t region;             /**< as pd_draw's: 0 body, 3 float, 5 a text box in a drawing... */
    pd_sp x, y, w, h;           /**< the cell's box, borders on its edges */
} pd_cell_box;

/**
 * The table cells on a page, in the order they were placed; same size-query
 * convention as pd_layout_page_items. A text box's tables are listed once
 * the page's items have been (they are laid out with the drawing).
 */
PD_API pd_status pd_layout_page_cells(const pd_layout* layout, int32_t page, pd_cell_box* buf, int32_t cap,
                                      int32_t* count);

/** the document position nearest to a point on a page */
PD_API pd_status pd_layout_hit_test(const pd_layout* layout, int32_t page, pd_sp x, pd_sp y, pd_pos* out);

/** where the caret for a position is drawn: page, x, baseline and line ascent/descent */
PD_API pd_status pd_layout_caret(const pd_layout* layout, pd_pos pos, int32_t* page, pd_sp* x, pd_sp* baseline,
                                 pd_sp* ascent, pd_sp* descent);

/** the position one step left (dir < 0) or right (dir > 0) of pos as its line is shown: in right-to-left text
    left goes forward; PD_ERR_RANGE at that end of the line (the caller goes on to the next or previous line) */
PD_API pd_status pd_layout_caret_step(const pd_layout* layout, pd_pos pos, int32_t dir, pd_pos* out);

/**
 * Draw display-list items into an RGBA image (8 bits a channel, R G B A,
 * premultiplied by alpha, rows stride bytes apart), over what it holds:
 * paths (with their styles) and rules; glyphs and images are left to the
 * caller. The image's top-left pixel is the page point (x0, y0); scale is
 * pixels per point in 16.16 (65536: 72 dpi). Integer arithmetic for the
 * coverage, so the same items give the same pixels everywhere.
 */
PD_API pd_status pd_draw_render(const pd_draw* items, int32_t n, pd_sp x0, pd_sp y0, int32_t scale, uint8_t* rgba,
                                int32_t width, int32_t height, int32_t stride);

/** what an item covers when drawn, on the page: its box with its stroke, arrowheads aside, and its effects
    (shadow, glow, reflection) */
PD_API void pd_draw_bounds(const pd_draw* item, pd_sp* x0, pd_sp* y0, pd_sp* x1, pd_sp* y1);

/**
 * Draw a drawing resource, width by height on the page, into an RGBA image
 * as pd_draw_render does (premultiplied, over what it holds; scale pixels
 * a point in 16.16): its paths, its glyphs, its PNG and GIF pictures. For
 * hosts that need a picture of it -- an RTF file, the clipboard. Its text
 * boxes' stories, laid out with the page, are not in it; turned glyphs and
 * turned pictures are drawn upright.
 */
PD_API pd_status pd_drawing_render(const pd_doc* doc, pd_res_id res, pd_sp width, pd_sp height, int32_t scale,
                                   uint8_t* rgba, int32_t w, int32_t h, int32_t stride);

/** the same as a PNG file (transparent where nothing is drawn), as large as width and height at scale */
PD_API pd_status pd_drawing_to_png(const pd_doc* doc, pd_res_id res, pd_sp width, pd_sp height, int32_t scale,
                                   pd_writer fn, void* user);

/* ------------------------------------------------------------------ */
/* SVG                                                                */
/* ------------------------------------------------------------------ */

/**
 * Write a page as an SVG 1.1 document from its display list, in points:
 * paths with their curves, dashes, ends and corners, gradients and
 * patterns as SVG's own, glyphs as paths from the fonts' outlines,
 * pictures as data: URIs, effects as PNG pictures from Parade's
 * rasterizer.
 */
PD_API pd_status pd_layout_write_svg(const pd_layout* layout, int32_t page, pd_writer fn, void* user);

/** a drawing resource (application/vnd.parade.drawing+json) as SVG, width by height; PD_ERR_ARG if it is none.
    Its text boxes' stories, laid out with the page, are not in it */
PD_API pd_status pd_drawing_to_svg(const pd_doc* doc, pd_res_id res, pd_sp width, pd_sp height, pd_writer fn,
                                   void* user);

/* ------------------------------------------------------------------ */
/* PDF                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int32_t compress;           /**< deflate content, fonts and images (default 1) */
    int32_t outlines;           /**< bookmarks from titles and headings (default 1) */
    char title[256];            /**< UTF-8; empty = the document's title paragraph */
    char author[128];
} pd_pdf_options;

PD_API void pd_pdf_options_init(pd_pdf_options* options);

/**
 * Write the pages as PDF 1.7: TrueType fonts embedded as subsets, CFF
 * fonts as Type 3 outlines, ToUnicode maps from the document text, JPEG
 * and PNG images, bookmarks. Deterministic: the same layout gives the same
 * bytes. options may be NULL.
 */
PD_API pd_status pd_layout_write_pdf(const pd_layout* layout, const pd_pdf_options* options, pd_writer fn,
                                     void* user);

/** a part of a page drawn on its own: items first..last of the page's display list, a form (/P<n>, n its index) */
typedef struct {
    int32_t page, first, last;
    int32_t group;              /**< a transparency group: an opacity it is drawn with is the whole part's */
} pd_pdf_part;

/** a page of the PDF made of parts */
typedef struct {
    int32_t page;               /**< the layout's page whose size it has */
    const char* content;        /**< its content stream: the parts as /P<n>, opacities as /GO0../GO255 (n/255) */
    const char* extra;          /**< more of the page's dictionary (/Dur 0.05 /Trans << .. >>), or NULL */
} pd_pdf_show_page;

typedef struct {
    const pd_pdf_part* parts;
    int32_t nparts;
    const pd_pdf_show_page* pages;
    int32_t npages;
    const char* catalog;        /**< more of the catalog's dictionary (/PageMode /FullScreen), or NULL */
} pd_pdf_show;

/**
 * Write a PDF of pages made of parts of the layout's pages, each part
 * written once (fonts, images and all as pd_layout_write_pdf has them) and
 * drawn by any page as its content says: moved, turned, cut or see-through,
 * an animation as pages shown one after another. No bookmarks.
 */
PD_API pd_status pd_layout_write_pdf_show(const pd_layout* layout, const pd_pdf_options* options,
                                          const pd_pdf_show* show, pd_writer fn, void* user);

#ifdef __cplusplus
}
#endif

#endif /* PARADE_LAYOUT_H */
