/*
 * Parade PDF reader: open a PDF, draw its pages, read their text, and change
 * it -- annotations, form fields, page text -- saved as an incremental
 * update appended to the file as it was.
 *
 * Page coordinates here are points (1/72 in) on the page as it is shown:
 * its crop box, turned by its /Rotate, origin at the top left, y down.
 */

#ifndef PARADE_PDFREAD_H
#define PARADE_PDFREAD_H

#include "parade.h"
#include "parade_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_pdfr pd_pdfr;

/** open a PDF held in memory (the bytes are copied); password may be NULL (the empty user password is tried) */
PD_API pd_status pd_pdfr_open_memory(const void* data, size_t len, const char* password, pd_pdfr** out);
PD_API pd_status pd_pdfr_open_file(const char* path, const char* password, pd_pdfr** out);
PD_API void      pd_pdfr_free(pd_pdfr* pdf);
/** why the last open or call failed, in a few words ("" when it did not) */
PD_API const char* pd_pdfr_error(const pd_pdfr* pdf);
/** 1 if the file needs a password not given (pd_pdfr_open_* returned PD_ERR_STATE) */
PD_API int32_t   pd_pdfr_encrypted(const pd_pdfr* pdf);

/** where fonts not in the file are looked for, besides the usual system folders */
PD_API void      pd_pdfr_add_font_dir(pd_pdfr* pdf, const char* dir);

PD_API int32_t   pd_pdfr_page_count(const pd_pdfr* pdf);
/** a page's size as shown (points) */
PD_API pd_status pd_pdfr_page_size(pd_pdfr* pdf, int32_t page, double* width, double* height);
/** the document's title and the like, UTF-8 ("Title", "Author", ...); *len set; NULL if none. Valid until freed */
PD_API pd_status pd_pdfr_info(pd_pdfr* pdf, const char* key, char* buf, size_t cap);

#define PD_PDFR_BGRA      1     /**< bytes B G R A (else R G B A) */
#define PD_PDFR_NO_ANNOTS 2     /**< annotations not drawn */
#define PD_PDFR_NO_WHITE  4     /**< the page not filled with white first (drawn over what buf holds) */

/**
 * Draw a page at scale pixels a point into buf: the part of the page whose
 * top-left pixel is (ox, oy) in the page drawn whole at that scale, width
 * by height pixels, rows stride bytes apart, 4 bytes a pixel, opaque.
 */
PD_API pd_status pd_pdfr_render(pd_pdfr* pdf, int32_t page, double scale, int32_t ox, int32_t oy, uint8_t* buf,
                                int32_t width, int32_t height, int32_t stride, int32_t flags);

#define PD_PDFR_CHAR_SPACE   1  /**< a space not in the file, put where words are apart */
#define PD_PDFR_CHAR_NEWLINE 2  /**< a line break not in the file, put where a line ends */

/** a character of a page's text as the file has it in order, with its box on the page */
typedef struct {
    uint32_t cp;                /**< the code point (a glyph with no meaning known: U+FFFD) */
    int32_t flags;              /**< PD_PDFR_CHAR_* */
    float x0, y0, x1, y1;       /**< its box on the page as shown */
    float size;                 /**< its font size there (points) */
    int32_t run;                /**< the text-showing operation it came from, counted through the page */
} pd_pdfr_char;

/** a page's characters; the size-query convention of pd_layout_page_items */
PD_API pd_status pd_pdfr_page_text(pd_pdfr* pdf, int32_t page, pd_pdfr_char* buf, int32_t cap, int32_t* count);

/* ------------------------------------------------------------------ */
/* annotations                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    PD_ANNOT_OTHER = 0, PD_ANNOT_TEXT, PD_ANNOT_LINK, PD_ANNOT_FREETEXT, PD_ANNOT_LINE, PD_ANNOT_SQUARE,
    PD_ANNOT_CIRCLE, PD_ANNOT_POLYGON, PD_ANNOT_POLYLINE, PD_ANNOT_HIGHLIGHT, PD_ANNOT_UNDERLINE,
    PD_ANNOT_SQUIGGLY, PD_ANNOT_STRIKEOUT, PD_ANNOT_STAMP, PD_ANNOT_INK, PD_ANNOT_WIDGET, PD_ANNOT_POPUP,
    PD_ANNOT_ARROW              /**< a line with an arrowhead at its end (made by pd_pdfr_add_shape) */
} pd_annot_kind;

typedef struct {
    int32_t kind;               /**< pd_annot_kind */
    double x0, y0, x1, y1;      /**< its rectangle on the page as shown */
    uint32_t color;             /**< 0xRRGGBB its colour (stroke) */
    uint32_t fill;              /**< 0xAARRGGBB its inside (0: none) */
    double width;               /**< its line width */
    int32_t hidden;             /**< not shown (its Hidden flag, or a popup) */
    int32_t field;              /**< a widget: its form field (pd_pdfr_field_*), else -1 */
    char contents[512];         /**< its text, UTF-8 (cut short if longer) */
    char uri[512];              /**< a link's URI ("" for none); a link to a page: "#page=N" */
} pd_pdfr_annot;

PD_API int32_t   pd_pdfr_annot_count(pd_pdfr* pdf, int32_t page);
PD_API pd_status pd_pdfr_annot_get(pd_pdfr* pdf, int32_t page, int32_t index, pd_pdfr_annot* out);
/** the topmost annotation shown at a point on the page, or -1 */
PD_API int32_t   pd_pdfr_annot_at(pd_pdfr* pdf, int32_t page, double x, double y);
PD_API pd_status pd_pdfr_annot_delete(pd_pdfr* pdf, int32_t page, int32_t index);
/** moved by dx, dy points (its appearance moves with it) */
PD_API pd_status pd_pdfr_annot_move(pd_pdfr* pdf, int32_t page, int32_t index, double dx, double dy);
PD_API pd_status pd_pdfr_annot_set_contents(pd_pdfr* pdf, int32_t page, int32_t index, const char* utf8);
PD_API pd_status pd_pdfr_annot_set_color(pd_pdfr* pdf, int32_t page, int32_t index, uint32_t rgb);

/** text marked: quads are 8 numbers each (x, y of top-left, top-right, bottom-left, bottom-right on the page);
    kind PD_ANNOT_HIGHLIGHT, _UNDERLINE, _SQUIGGLY or _STRIKEOUT; returns the new annotation's index */
PD_API int32_t   pd_pdfr_add_markup(pd_pdfr* pdf, int32_t page, int32_t kind, const double* quads, int32_t nquads,
                                    uint32_t rgb, const char* contents);
/** a freehand drawing: strokes of points (x, y pairs), counts[i] points in stroke i */
PD_API int32_t   pd_pdfr_add_ink(pd_pdfr* pdf, int32_t page, const double* points, const int32_t* counts,
                                 int32_t nstrokes, uint32_t rgb, double width);
/** a rectangle, ellipse, line or arrow (PD_ANNOT_SQUARE, _CIRCLE, _LINE, _ARROW) from (x0, y0) to (x1, y1);
    fill 0xAARRGGBB, 0 for none */
PD_API int32_t   pd_pdfr_add_shape(pd_pdfr* pdf, int32_t page, int32_t kind, double x0, double y0, double x1,
                                   double y1, uint32_t rgb, uint32_t fill, double width);
/** a sticky note at (x, y) */
PD_API int32_t   pd_pdfr_add_note(pd_pdfr* pdf, int32_t page, double x, double y, const char* utf8, uint32_t rgb);
/** text in a box on the page */
PD_API int32_t   pd_pdfr_add_text_box(pd_pdfr* pdf, int32_t page, double x0, double y0, double x1, double y1,
                                      const char* utf8, double size, uint32_t rgb);

/* ------------------------------------------------------------------ */
/* form fields                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    PD_PDF_FIELD_TEXT = 1, PD_PDF_FIELD_CHECK, PD_PDF_FIELD_RADIO, PD_PDF_FIELD_PUSH, PD_PDF_FIELD_COMBO, PD_PDF_FIELD_LIST,
    PD_PDF_FIELD_SIGNATURE
} pd_pdf_field_kind;

typedef struct {
    int32_t kind;               /**< pd_pdf_field_kind */
    char name[256];             /**< its full name, UTF-8 */
    char value[1024];           /**< text: its text; check box, radio: its state name ("Off" for none); choice:
                                     the option chosen */
    int32_t page;               /**< the page of its first widget, -1 none */
    double x0, y0, x1, y1;      /**< that widget's rectangle on the page */
    int32_t readonly, multiline, password, required;
    int32_t nopts;              /**< a choice's options; a check box or radio button: its on states */
    int32_t maxlen;             /**< text: its most characters (0: no limit) */
    double font_size;           /**< its text's size (0: fit to the box) */
} pd_pdfr_field;

PD_API int32_t   pd_pdfr_field_count(pd_pdfr* pdf);
PD_API pd_status pd_pdfr_field_get(pd_pdfr* pdf, int32_t index, pd_pdfr_field* out);
/** a choice's option i, or a check box's on state i (UTF-8) */
PD_API pd_status pd_pdfr_field_option(pd_pdfr* pdf, int32_t index, int32_t i, char* buf, size_t cap);
/** the field a page's widget at a point belongs to, or -1 */
PD_API int32_t   pd_pdfr_field_at(pd_pdfr* pdf, int32_t page, double x, double y);
/** the on state of a page's widget annotation (a radio button's own; "" if it has none) */
PD_API pd_status pd_pdfr_widget_state(pd_pdfr* pdf, int32_t page, int32_t annot, char* buf, size_t cap);
/** a text or choice field's value; a check box or radio button's state ("Off" or one of its on states, NULL or ""
    for its first on state) */
PD_API pd_status pd_pdfr_field_set(pd_pdfr* pdf, int32_t index, const char* utf8);

/* ------------------------------------------------------------------ */
/* page text                                                          */
/* ------------------------------------------------------------------ */

/**
 * Replace characters from..to-1 of a page's text (pd_pdfr_page_text's
 * order) with utf8: in the content stream itself when the text is all of
 * one run and its font has the characters, else by covering it in white
 * and writing the new text over it in a standard font.
 */
PD_API pd_status pd_pdfr_replace_text(pd_pdfr* pdf, int32_t page, int32_t from, int32_t to, const char* utf8);

/* ------------------------------------------------------------------ */
/* undo                                                               */
/* ------------------------------------------------------------------ */

PD_API int32_t   pd_pdfr_can_undo(const pd_pdfr* pdf);
PD_API int32_t   pd_pdfr_can_redo(const pd_pdfr* pdf);
PD_API pd_status pd_pdfr_undo(pd_pdfr* pdf);
PD_API pd_status pd_pdfr_redo(pd_pdfr* pdf);

/* ------------------------------------------------------------------ */
/* saving                                                             */
/* ------------------------------------------------------------------ */

/** 1 if anything was changed since it was opened or last saved */
PD_API int32_t   pd_pdfr_modified(const pd_pdfr* pdf);
/** a number bumped by every change (a host's redraw signal) */
PD_API int32_t   pd_pdfr_changes(const pd_pdfr* pdf);
/** the file as it was with the changes appended (an incremental update); nothing appended when unchanged */
PD_API pd_status pd_pdfr_save(pd_pdfr* pdf, pd_writer fn, void* user);
PD_API pd_status pd_pdfr_save_file(pd_pdfr* pdf, const char* path);

#ifdef __cplusplus
}
#endif

#endif
