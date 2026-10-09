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
    int32_t region;             /**< 0 body, 1 header, 2 footer, 3 float, 4 footnote, 5 a text box in a drawing */
    uint32_t text;              /**< glyphs: the code point shown (also for generated text), 0 if none */
    int32_t scale;              /**< glyphs: horizontal scale, 65536 = 1 (font expansion) */
    const pd_sp* points;        /**< paths: x, y pairs on the page; valid until the page is laid out or listed again */
    int32_t npoints;            /**< paths: points (pairs) */
    int32_t path_flags;         /**< paths: PD_PATH_* */
    pd_sp line_width;           /**< paths: stroke width, 0 = no stroke */
    uint32_t fill;              /**< paths: fill colour 0xAARRGGBB, 0 = not filled */
    pd_sp clip_x, clip_y, clip_w, clip_h;   /**< images: shown only within this (a cropped picture); clip_w 0: all */
} pd_draw;

/** the display list of a page; same size-query convention as pd_para_get_glyphs */
PD_API pd_status pd_layout_page_items(const pd_layout* layout, int32_t page, pd_draw* buf, int32_t cap,
                                      int32_t* count);

typedef enum {
    PD_MARK_DELETION = 1,       /**< deleted text: a balloon in PD_MARKUP_BALLOONS, else struck through in place */
    PD_MARK_INSERTION = 2,      /**< inserted text (in the text, in the author's colour) */
    PD_MARK_COMMENT = 3         /**< a comment (not its replies, which share its range) */
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

/** the document position nearest to a point on a page */
PD_API pd_status pd_layout_hit_test(const pd_layout* layout, int32_t page, pd_sp x, pd_sp y, pd_pos* out);

/** where the caret for a position is drawn: page, x, baseline and line ascent/descent */
PD_API pd_status pd_layout_caret(const pd_layout* layout, pd_pos pos, int32_t* page, pd_sp* x, pd_sp* baseline,
                                 pd_sp* ascent, pd_sp* descent);

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

#ifdef __cplusplus
}
#endif

#endif /* PARADE_LAYOUT_H */
