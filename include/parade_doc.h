/*
 * Parade document model - DRAFT for review, not implemented yet
 *
 * A structured, editable document: sections with page geometry and
 * headers/footers, blocks (paragraphs, headings, lists, figures, tables,
 * footnotes), named styles with inheritance, inline objects and fields.
 * The model holds no fonts or pixels; layout (pd_para for paragraphs and
 * a page builder on top) reads it and caches per-block results.
 *
 * Design rules
 *  - Every edit goes through an operation that records its inverse, so
 *    undo/redo, dirty tracking and change notification come for free.
 *  - Blocks have stable ids (never reused within a document), so
 *    positions, cross-references and layout caches survive edits.
 *  - Semantic role (heading, caption, list item...) is separate from
 *    appearance (style), so the model exports cleanly to HTML, Markdown,
 *    DOCX and LaTeX.
 *  - All structs are POD with fixed-size fields (strings are fixed char
 *    arrays or doc-owned pointers), for a mechanical Pascal binding.
 *    Enum-valued fields are int32_t: enum size is compiler-dependent.
 *  - Text is UTF-8; a position is (block id, byte offset).
 */

#ifndef PARADE_DOC_H
#define PARADE_DOC_H

#include "parade.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pd_doc pd_doc;

typedef uint32_t pd_block_id;   /* 0 = none */
typedef uint32_t pd_style_id;   /* 0 = none / default */
typedef uint32_t pd_format_id;  /* interned character style + direct formatting; 0 = default */
#define PD_FORMAT_INHERIT 0xFFFFFFFFu   /* insert_text: continue the format of the text to the left */
typedef uint32_t pd_list_id;    /* 0 = not in a list */
typedef uint32_t pd_res_id;     /* embedded resource (image bytes, ...); 0 = none */
typedef uint32_t pd_marker_id;  /* 0 = none */
typedef uint32_t pd_rev_id;     /* tracked change (author, date, kind); 0 = none */
typedef uint32_t pd_comment_id; /* 0 = none */

/** a position: byte offset into a paragraph's text */
typedef struct {
    pd_block_id block;
    uint32_t offset;
} pd_pos;

typedef struct {
    pd_pos start, end;          /**< start <= end in document order */
} pd_range;

/* ------------------------------------------------------------------ */
/* Document                                                           */
/* ------------------------------------------------------------------ */

/**
 * Resolves a font family name to a loaded font. The document never loads
 * fonts itself; the host owns them and they must outlive the document.
 * weight is CSS-style (400 regular, 700 bold); italic is 0/1.
 * Return NULL to fall back to the document's default font.
 */
typedef const pd_font* (*pd_font_resolver)(void* user, const char* family, int32_t weight, int32_t italic);

/** a new document with one empty section holding one empty paragraph */
PD_API pd_status pd_doc_new(pd_doc** out);
PD_API void      pd_doc_free(pd_doc* doc);
PD_API void      pd_doc_set_font_resolver(pd_doc* doc, pd_font_resolver fn, void* user);
/**
 * Fonts tried in order for characters the run's font has no glyph for
 * (e.g. CJK or symbol fonts behind a Latin text font). The fonts must
 * outlive the document; the list is copied. n = 0 clears it.
 */
PD_API pd_status pd_doc_set_fallback_fonts(pd_doc* doc, const pd_font* const* fonts, int32_t n);
/**
 * Hyphenation patterns for a language: used for runs whose language tag
 * starts with lang ("en" matches "en-US"; the longest match wins, "" matches
 * runs without a language) in paragraphs with hyphenate on. hyph must
 * outlive the document; NULL removes the entry.
 */
PD_API pd_status pd_doc_set_hyphenator(pd_doc* doc, const char* lang, const pd_hyph* hyph);
/** typeset equation objects (their LaTeX source) with an OpenType math font; NULL: host-sized boxes */
PD_API void      pd_doc_set_math_font(pd_doc* doc, const pd_font* font);
/**
 * Microtypography for every paragraph but code: margin kerning
 * (protrusion 1) and font expansion up to expansion per-mille of a glyph's
 * width (pdfTeX's \pdfprotrudechars and \pdfadjustspacing). 0, 0 = off.
 */
PD_API pd_status pd_doc_set_microtype(pd_doc* doc, int32_t protrusion, int32_t expansion);
/** font used when the resolver returns NULL or none is set */
PD_API void      pd_doc_set_default_font(pd_doc* doc, const pd_font* font);

/** increases by one with every applied operation (including undo/redo) */
PD_API uint64_t  pd_doc_revision(const pd_doc* doc);

/* ------------------------------------------------------------------ */
/* Blocks: the document tree                                          */
/* ------------------------------------------------------------------ */

/*
 * Tree shape:
 *   ROOT
 *    +- SECTION*                 page geometry, header/footer stories
 *        +- PARAGRAPH            body, heading, caption, list item, ...
 *        +- FLOAT                figure/table float; children are its content
 *        |   +- PARAGRAPH        e.g. one holding an inline image
 *        |   +- PARAGRAPH        role CAPTION
 *        +- TABLE > ROW > CELL > blocks
 *        +- BREAK                explicit page/column break
 *   STORY                        header/footer/footnote content; not in the
 *                                main flow, referenced by id. Created with
 *                                pd_doc_insert_block(doc, 0, -1, PD_BLOCK_STORY)
 *                                and listed by pd_doc_story_count/pd_doc_story_at
 * A float appears in the flow where it is authored (its anchor); the
 * page builder decides where it actually lands, LaTeX-style.
 */
typedef enum {
    PD_BLOCK_ROOT = 0,
    PD_BLOCK_SECTION = 1,
    PD_BLOCK_PARAGRAPH = 2,
    PD_BLOCK_FLOAT = 3,
    PD_BLOCK_TABLE = 4,         /**< grid of ROWs of CELLs; pd_table_props */
    PD_BLOCK_ROW = 5,
    PD_BLOCK_CELL = 6,
    PD_BLOCK_BREAK = 7,
    PD_BLOCK_STORY = 8
} pd_block_kind;

/** semantic role of a paragraph, independent of its style */
typedef enum {
    PD_ROLE_BODY = 0,
    PD_ROLE_TITLE = 1,
    PD_ROLE_HEADING = 2,        /**< level in pd_block_info.level (1-6) */
    PD_ROLE_CAPTION = 3,
    PD_ROLE_QUOTE = 4,
    PD_ROLE_CODE = 5,           /**< preformatted: no justification, no hyphenation */
    PD_ROLE_EQUATION = 6,       /**< display equation: holds one equation inline */
    PD_ROLE_FIGURE_CONTENT = 7, /**< the content paragraph of a float (image, ...) */
    PD_ROLE_RAW = 8,            /**< markup passed through untouched (an HTML block in Markdown): shown as code */
    PD_ROLE_TERM = 9,           /**< the term of a definition list */
    PD_ROLE_DEFINITION = 10     /**< its definition, indented below it */
} pd_role;

typedef enum {
    PD_BREAK_PAGE = 0,
    PD_BREAK_COLUMN = 1,
    PD_BREAK_ODD_PAGE = 2,
    PD_BREAK_EVEN_PAGE = 3,
    PD_BREAK_RULE = 4           /**< no break at all: a horizontal rule across the column (Markdown ***, HTML <hr>) */
} pd_break_kind;

typedef struct {
    int32_t kind;               /**< pd_block_kind */
    pd_block_id id;
    pd_block_id parent;
    int32_t child_count;
    int32_t index;              /**< position among the parent's children */
    int32_t role;               /**< pd_role (paragraphs) */
    int32_t level;              /**< heading level 1-6 (role HEADING), else 0 */
    int32_t list_level;         /**< level within the list, 0-8 */
    pd_style_id style;          /**< paragraph style (paragraphs) */
    pd_list_id list;            /**< paragraphs in a list */
    int32_t break_kind;         /**< pd_break_kind for BREAK blocks */
    uint32_t text_length;       /**< paragraphs: UTF-8 bytes */
    uint64_t revision;          /**< document revision of the last change to this block */
} pd_block_info;

PD_API pd_block_id pd_doc_root(const pd_doc* doc);
PD_API int32_t     pd_doc_story_count(const pd_doc* doc);
PD_API pd_block_id pd_doc_story_at(const pd_doc* doc, int32_t index);
PD_API pd_status   pd_doc_block_info(const pd_doc* doc, pd_block_id block, pd_block_info* out);
PD_API pd_block_id pd_doc_child(const pd_doc* doc, pd_block_id parent, int32_t index);
/** next/previous paragraph in reading order, descending into floats/cells; 0 at the end. From 0: the first/last */
PD_API pd_block_id pd_doc_next_paragraph(const pd_doc* doc, pd_block_id block);
PD_API pd_block_id pd_doc_prev_paragraph(const pd_doc* doc, pd_block_id block);

/* ------------------------------------------------------------------ */
/* Styles                                                             */
/* ------------------------------------------------------------------ */

typedef enum {
    PD_STYLE_PARAGRAPH = 0,     /**< carries paragraph and character properties */
    PD_STYLE_CHARACTER = 1      /**< character properties only */
} pd_style_kind;

/* character property mask bits: a style or override sets only the masked fields */
#define PD_CP_FAMILY      (1u << 0)
#define PD_CP_SIZE        (1u << 1)
#define PD_CP_WEIGHT      (1u << 2)
#define PD_CP_ITALIC      (1u << 3)
#define PD_CP_COLOR       (1u << 4)
#define PD_CP_BACKGROUND  (1u << 5)
#define PD_CP_UNDERLINE   (1u << 6)
#define PD_CP_STRIKE      (1u << 7)
#define PD_CP_SHIFT       (1u << 8)
#define PD_CP_LETTERSPACE (1u << 9)
#define PD_CP_KERNING     (1u << 10)
#define PD_CP_LANG        (1u << 11)
#define PD_CP_SMALLCAPS   (1u << 12)
#define PD_CP_LINK        (1u << 13)
#define PD_CP_CAPS        (1u << 14)
#define PD_CP_HIDDEN      (1u << 15)
#define PD_CP_POSITION    (1u << 16)
#define PD_CP_REVISION    (1u << 17)

typedef enum {
    PD_UNDERLINE_NONE = 0,
    PD_UNDERLINE_SINGLE = 1,
    PD_UNDERLINE_DOUBLE = 2,
    PD_UNDERLINE_THICK = 3,
    PD_UNDERLINE_DOTTED = 4,
    PD_UNDERLINE_DASHED = 5,
    PD_UNDERLINE_WAVY = 6,
    PD_UNDERLINE_WORDS = 7      /**< single, under the words and not the spaces between them */
} pd_underline;

typedef enum {
    PD_SHIFT_NONE = 0,
    PD_SHIFT_SUPER = 1,
    PD_SHIFT_SUB = 2
} pd_shift;

typedef struct {
    uint32_t mask;              /**< PD_CP_* bits that are set */
    char family[64];            /**< font family, UTF-8, passed to the resolver */
    pd_sp size;
    int32_t weight;             /**< 100-900 */
    int32_t italic;
    uint32_t color;             /**< 0xAARRGGBB */
    uint32_t background;        /**< 0 = none */
    int32_t underline;          /**< pd_underline */
    int32_t strike;
    int32_t shift;              /**< pd_shift */
    pd_sp letter_space;
    int32_t kerning;
    char lang[16];              /**< BCP 47 tag ("en-US", "zh-Hans"): hyphenation, line breaking */
    int32_t small_caps;
    pd_block_id link_target;    /**< internal link (cross-reference); external URLs are inlines */
    int32_t caps;               /**< shown in capitals; the text keeps its case */
    int32_t hidden;             /**< kept in the text, not shown and taking no room */
    pd_sp position;             /**< baseline raised (positive) or lowered, at full size (not a super/subscript) */
    pd_rev_id revision;         /**< tracked insertion or deletion the text belongs to (0: none) */
} pd_char_props;

/* paragraph property mask bits */
#define PD_PP_ALIGN        (1u << 0)
#define PD_PP_INDENT_LEFT  (1u << 1)
#define PD_PP_INDENT_RIGHT (1u << 2)
#define PD_PP_INDENT_FIRST (1u << 3)
#define PD_PP_SPACE_BEFORE (1u << 4)
#define PD_PP_SPACE_AFTER  (1u << 5)
#define PD_PP_LINE_SPACING (1u << 6)
#define PD_PP_KEEP_NEXT    (1u << 7)
#define PD_PP_KEEP_LINES   (1u << 8)
#define PD_PP_WIDOWS       (1u << 9)
#define PD_PP_ORPHANS      (1u << 10)
#define PD_PP_BREAK_BEFORE (1u << 11)
#define PD_PP_HYPHENATE    (1u << 12)
#define PD_PP_BREAK_MODE   (1u << 13)
#define PD_PP_NEXT_STYLE   (1u << 14)
#define PD_PP_BORDER       (1u << 15)
#define PD_PP_SHADING      (1u << 16)
#define PD_PP_DIRECTION    (1u << 17)
#define PD_PP_TABS         (1u << 18) /**< tab stops and the default interval, together */
#define PD_PP_CONTEXTUAL   (1u << 19) /**< no space between this and a neighbour of the same style */

typedef struct {
    uint32_t mask;              /**< PD_PP_* bits that are set */
    int32_t align;              /**< pd_align */
    pd_sp indent_left;
    pd_sp indent_right;
    pd_sp indent_first;         /**< relative to indent_left; negative = hanging */
    pd_sp space_before;
    pd_sp space_after;
    int32_t line_spacing;       /**< per-mille of the font-derived line height (1000 = single) */
    int32_t keep_with_next;     /**< no page break between this and the next block */
    int32_t keep_lines;         /**< no page break inside the paragraph */
    int32_t widows;             /**< minimum lines at the top of a page (TeX \widowpenalty as a hard limit) */
    int32_t orphans;            /**< minimum lines at the bottom of a page */
    int32_t page_break_before;
    int32_t hyphenate;
    int32_t break_mode;         /**< pd_break_mode used when not interactively editing */
    pd_style_id next_style;     /**< style of the paragraph created by Enter at the end */
    uint32_t border_color;      /**< 0 = no border */
    pd_sp border_width;
    uint32_t shading;           /**< 0 = none */
    int32_t direction;          /**< pd_direction: auto (first strong character), LTR or RTL */
    int32_t ntabs;              /**< tab stops set, in order of position, from the left margin */
    pd_tab_stop tabs[PD_MAX_TABS];
    pd_sp tab_interval;         /**< default stops past the last set one, 0 = every 36pt */
    int32_t contextual;         /**< its space before and after left out next to a paragraph of the same style */
    int32_t border_sides;       /**< PD_BORDER_* edges the border has (with PD_PP_BORDER); 0 = all four */
    pd_sp border_space;         /**< between the text and the border */
} pd_para_props;

/* the edges of a paragraph border; paragraphs one after another with the
   same border share one box, and BETWEEN rules them off from each other */
#define PD_BORDER_TOP     (1 << 0)
#define PD_BORDER_RIGHT   (1 << 1)
#define PD_BORDER_BOTTOM  (1 << 2)
#define PD_BORDER_LEFT    (1 << 3)
#define PD_BORDER_BETWEEN (1 << 4)

/**
 * Define a named style. parent is inherited for every unmasked field
 * (0 = the document defaults). Names are unique; built-in names such as
 * "Normal", "Heading 1".."Heading 6", "Caption", "Title" exist in every
 * new document and may be redefined.
 */
PD_API pd_status pd_doc_style_define(pd_doc* doc, const char* name, pd_style_kind kind, pd_style_id parent,
                                     const pd_para_props* para, const pd_char_props* chr, pd_style_id* out);
PD_API pd_style_id pd_doc_style_find(const pd_doc* doc, const char* name);
/** fully resolved properties of a style (inheritance applied, all mask bits set) */
PD_API pd_status pd_doc_style_resolve(const pd_doc* doc, pd_style_id style, pd_para_props* para,
                                      pd_char_props* chr);
PD_API int32_t   pd_doc_style_count(const pd_doc* doc);
PD_API pd_style_id pd_doc_style_at(const pd_doc* doc, int32_t index);
/** name of a style; the pointer is owned by the document and valid until the style is redefined */
PD_API const char* pd_doc_style_name(const pd_doc* doc, pd_style_id style);

/**
 * Intern a character format: a character style plus direct overrides.
 * The same combination always yields the same id, so text runs stay
 * compact and comparing formats is comparing ids.
 */
PD_API pd_format_id pd_doc_format(pd_doc* doc, pd_style_id char_style, const pd_char_props* overrides);
/** fully resolved character properties of a run inside a given paragraph */
PD_API pd_status pd_doc_format_resolve(const pd_doc* doc, pd_block_id paragraph, pd_format_id format,
                                       pd_char_props* out);
/** what a format was interned from: its character style and direct overrides (either may be NULL) */
PD_API pd_status pd_doc_format_info(const pd_doc* doc, pd_format_id format, pd_style_id* char_style,
                                    pd_char_props* overrides);
/** a style's own definition, before inheritance: kind, parent, and the masked properties it sets */
PD_API pd_status pd_doc_style_info(const pd_doc* doc, pd_style_id style, int32_t* kind, pd_style_id* parent,
                                   pd_para_props* para, pd_char_props* chr);

/* ------------------------------------------------------------------ */
/* Lists                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    PD_NUM_BULLET = 0,
    PD_NUM_DECIMAL = 1,         /**< 1, 2, 3 */
    PD_NUM_LOWER_ALPHA = 2,
    PD_NUM_UPPER_ALPHA = 3,
    PD_NUM_LOWER_ROMAN = 4,
    PD_NUM_UPPER_ROMAN = 5,
    PD_NUM_NONE = 6
} pd_num_format;

typedef struct {
    int32_t format;             /**< pd_num_format */
    int32_t start;
    char text[32];              /**< label template: "%1." or "%1.%2)" for numbers, the bullet for bullets */
    pd_sp indent;               /**< text indent of this level */
    pd_sp hanging;              /**< label hangs this far to the left of the text */
    int32_t restart_after;      /**< 0: counts again after any shallower level's item; -1: never; k: after an
                                     item of levels 1..k (counted from 1) */
    char label_family[32];      /**< the label's own font, "" = the text's */
    pd_sp label_size;           /**< 0 = the text's */
    int32_t label_weight;       /**< 0 = the text's */
    int32_t label_italic;       /**< 0 as the text, 1 italic, -1 upright */
    uint32_t label_color;       /**< 0 = the text's */
} pd_list_level;

/** a list definition with up to 9 levels; paragraphs join it through pd_doc_set_list */
PD_API pd_status pd_doc_list_define(pd_doc* doc, int32_t nlevels, const pd_list_level* levels, pd_list_id* out);
/** number of list definitions; their ids are 1..count */
PD_API int32_t   pd_doc_list_count(const pd_doc* doc);
/** the levels of a list definition; levels holds up to 9 entries (may be NULL) */
PD_API pd_status pd_doc_list_info(const pd_doc* doc, pd_list_id list, int32_t* nlevels, pd_list_level* levels);
/** the computed label of a list paragraph ("3.", "b)", "•"), UTF-8, into buf */
PD_API pd_status pd_doc_list_label(const pd_doc* doc, pd_block_id paragraph, char* buf, int32_t cap);

/* ------------------------------------------------------------------ */
/* Paragraph content                                                  */
/* ------------------------------------------------------------------ */

/** a run of text in one character format: [start, end) byte offsets */
typedef struct {
    uint32_t start, end;
    pd_format_id format;
} pd_run;

/** text of a paragraph; the pointer is owned by the document and valid until the next edit */
PD_API pd_status pd_doc_para_text(const pd_doc* doc, pd_block_id paragraph, const char** utf8, uint32_t* len);
/** format runs covering the whole text; same size-query convention as pd_para_get_glyphs */
PD_API pd_status pd_doc_para_runs(const pd_doc* doc, pd_block_id paragraph, pd_run* buf, int32_t cap,
                                  int32_t* count);

/** a paragraph's direct properties (only the masked fields are set on the paragraph itself) */
PD_API pd_status pd_doc_para_props(const pd_doc* doc, pd_block_id paragraph, pd_para_props* out);

/* inline objects occupy U+FFFC (3 bytes) in the paragraph text */
typedef enum {
    PD_INLINE_IMAGE = 0,        /**< resource + display size */
    PD_INLINE_EQUATION = 1,     /**< source text (LaTeX or MathML) + box size from the host/math engine */
    PD_INLINE_FIELD = 2,        /**< computed text: page number, reference, counter */
    PD_INLINE_FOOTNOTE = 3,     /**< reference mark; the note body is a STORY block */
    PD_INLINE_LINK = 4,         /**< start of an external hyperlink (URL in source); an empty URL ends it */
    PD_INLINE_BOOKMARK = 5,     /**< named anchor for cross-references */
    PD_INLINE_TAB = 6,          /**< tab stop (positions from the paragraph style) */
    PD_INLINE_USER = 7,         /**< host-defined object of the given size */
    PD_INLINE_RAW = 8           /**< markup passed through untouched (inline HTML in Markdown), in source: no size */
} pd_inline_kind;

typedef enum {
    PD_FIELD_PAGE = 0,          /**< current page number */
    PD_FIELD_PAGES = 1,         /**< total pages */
    PD_FIELD_SECTION_PAGE = 2,
    PD_FIELD_REF_NUMBER = 3,    /**< number of the target (figure 3, section 2.1) */
    PD_FIELD_REF_PAGE = 4,      /**< page of the target */
    PD_FIELD_SEQ = 5,           /**< counter in a named sequence ("Figure", "Table", "Equation") */
    PD_FIELD_HEADING = 6,       /**< text of the current heading at a level (running heads) */
    PD_FIELD_DATE = 7
} pd_field_kind;

typedef struct {
    int32_t kind;               /**< pd_inline_kind */
    pd_res_id resource;         /**< images */
    pd_sp width, height, depth; /**< box size; images: display size, equations: from the host */
    int32_t field;              /**< pd_field_kind */
    pd_block_id target;         /**< REF fields: the referenced block; FOOTNOTE: the note STORY */
    int32_t level;              /**< HEADING field level; FOOTNOTE: 0 a footnote, 1 an endnote (numbered i, ii, ...
                                     on its own, laid out after the last section's text) */
    char name[32];              /**< SEQ name or bookmark name */
    const char* source;         /**< equation source, link URL, the address of an image not embedded (resource 0)
                                     or raw markup; UTF-8, copied on insert, doc-owned on read */
    int32_t source_len;
    int32_t user;
    const char* title;          /**< links and images: the tooltip title; UTF-8, as source */
    int32_t title_len;
    const char* alt;            /**< images: the text that stands for the picture; UTF-8, as source */
    int32_t alt_len;
} pd_inline;

/** the inline object at a byte offset (which must hold U+FFFC) */
PD_API pd_status pd_doc_inline_at(const pd_doc* doc, pd_pos pos, pd_inline* out);

/**
 * The size an image inline is shown at: its own when set, else the
 * picture's pixel size at 96 dpi (the other side kept in proportion when one
 * is given), else -- not loaded, or not a PNG, JPEG or GIF -- a placeholder.
 */
PD_API void pd_doc_image_display_size(const pd_doc* doc, const pd_inline* image, pd_sp* width, pd_sp* height);

/** embed binary data (image bytes, ...); the document copies it */
PD_API pd_status pd_doc_add_resource(pd_doc* doc, const char* mime, const void* data, size_t len, pd_res_id* out);
PD_API pd_status pd_doc_resource(const pd_doc* doc, pd_res_id res, const char** mime, const void** data,
                                 size_t* len);

/* ------------------------------------------------------------------ */
/* Floats, sections, tables                                           */
/* ------------------------------------------------------------------ */

#define PD_PLACE_HERE   1u      /**< at the anchor if it fits */
#define PD_PLACE_TOP    2u      /**< top of a page */
#define PD_PLACE_BOTTOM 4u      /**< bottom of a page */
#define PD_PLACE_PAGE   8u      /**< on a float-only page */
#define PD_PLACE_FORCE  16u     /**< exactly at the anchor, even if it leaves a gap (LaTeX [H]) */
#define PD_PLACE_OFFSET 32u     /**< a wrapped float at offset_x, not against its side of the column */

typedef enum {
    PD_WRAP_NONE = 0,           /**< full width; text above and below */
    PD_WRAP_LEFT = 1,           /**< float on the left, text flows on the right */
    PD_WRAP_RIGHT = 2
} pd_wrap;

typedef struct {
    uint32_t placement;         /**< PD_PLACE_* bits, tried in the order here, top, bottom, page */
    int32_t wrap;               /**< pd_wrap */
    pd_sp width;                /**< 0 = width_fraction of the column applies */
    int32_t width_fraction;     /**< per-mille of the column width */
    int32_t span_columns;       /**< 1 = spans all columns in multi-column sections */
    pd_sp gap;                  /**< space between float and text */
    char sequence[32];          /**< numbering sequence of its caption ("Figure", "Table") */
    pd_sp offset_x;             /**< with PD_PLACE_OFFSET: its left edge from the column's (negative: in the
                                     margin); the text gives way only where it reaches into the column */
} pd_float_props;

typedef struct {
    pd_sp page_width, page_height;
    pd_sp margin_top, margin_bottom, margin_left, margin_right;
    pd_sp header_distance;      /**< from the page top to the header baseline area */
    pd_sp footer_distance;
    int32_t columns;
    pd_sp column_gap;
    int32_t first_page_number;  /**< 0 = continue from the previous section */
    int32_t page_number_format; /**< pd_num_format */
    int32_t title_page;         /**< 1 = the first page uses header_first/footer_first */
    int32_t facing_pages;       /**< 1 = even pages use header_even/footer_even, margins mirror */
    pd_block_id header, header_first, header_even;  /**< STORY blocks, 0 = none */
    pd_block_id footer, footer_first, footer_even;
    int32_t continuous;         /**< 1 = starts on the current page below the previous section (same page size) */
    int32_t page_breaking;      /**< pd_page_breaking */
    pd_sp footnote_skip;        /**< space between the text and the footnotes (a rule sits in it) */
    int32_t add_spacing;        /**< 1: a paragraph's space after and the next one's before add up (word
                                     processors); 0: the larger of the two (CSS, TeX) */
    int32_t line_numbers;       /**< lines of the text numbered in the margin, every this many (0 = none) */
    int32_t line_number_start;  /**< the first number (0 = 1) */
    pd_sp line_number_distance; /**< from the numbers to the text (0 = 18pt) */
    int32_t line_number_restart;    /**< PD_LINENUM_* */
    int32_t mirror_margins;     /**< 1: left and right margins swap on even pages; -1: never; 0: as facing_pages */
    pd_sp gutter;               /**< room for the binding, on the inside of the page */
    int32_t page_valign;        /**< text on the page: 0 at the top, 1 centred, 2 at the bottom */
} pd_section_props;

typedef enum {
    PD_PAGES_GREEDY = 0,        /**< fill each column, best break on it (TeX) */
    PD_PAGES_OPTIMAL = 1        /**< breaks chosen for the whole section at once, paragraphs may run a
                                     line longer or shorter to avoid bad pages (Mittelbach) */
} pd_page_breaking;

#define PD_TABLE_MAX_COLS 32

typedef struct {
    pd_sp width;                /**< 0 = automatic: from the content, at most the text column */
    int32_t align;              /**< pd_align LEFT/CENTER/RIGHT: where a narrower table sits */
    int32_t header_rows;        /**< leading rows repeated at the top of every page or column */
    pd_sp cell_padding;
    pd_sp border;               /**< grid rule thickness, 0 = no rules */
    uint32_t border_color;      /**< 0xAARRGGBB */
    int32_t ncols;              /**< entries used in col_width */
    pd_sp col_width[PD_TABLE_MAX_COLS]; /**< fixed column widths, 0 = automatic */
    pd_sp indent;               /**< a left-aligned table's distance from the column's left edge */
    int32_t width_pct;          /**< width in per-mille of the text column, 0 = width says */
    int32_t border_sides;       /**< PD_TBORDER_* rules the grid has, 0 = all */
    pd_sp cell_padding_v;       /**< top and bottom padding of the cells, < 0 = cell_padding */
} pd_table_props;

/* when line numbers start again */
#define PD_LINENUM_PAGE       0
#define PD_LINENUM_SECTION    1
#define PD_LINENUM_CONTINUOUS 2

/* the rules of a table grid: its outer edges and those between the rows and columns */
#define PD_TBORDER_TOP      (1 << 0)
#define PD_TBORDER_RIGHT    (1 << 1)
#define PD_TBORDER_BOTTOM   (1 << 2)
#define PD_TBORDER_LEFT     (1 << 3)
#define PD_TBORDER_INSIDE_H (1 << 4)
#define PD_TBORDER_INSIDE_V (1 << 5)

typedef struct {
    int32_t col_span;           /**< columns this cell covers, >= 1 */
    int32_t valign;             /**< 0 top, 1 middle, 2 bottom */
    uint32_t background;        /**< 0xAARRGGBB, 0 = none */
    int32_t merge_up;           /**< 1 = continues the cell above it (a vertical merge, Word's vMerge, HTML's
                                     rowspan): the first cell of the run holds the content and is drawn
                                     across every row of it, with no rules in between; the run's rows are
                                     kept on one page. The continuing cells' own content is not shown. */
    pd_sp min_height;           /**< the row is at least this tall */
    int32_t border_set;         /**< PD_BORDER_TOP/RIGHT/BOTTOM/LEFT edges whose rule the cell decides */
    int32_t border_on;          /**< of those, the ones ruled (the others have none) */
    pd_sp border_width;         /**< the cell's own rules */
    uint32_t border_color;
} pd_cell_props;

PD_API pd_status pd_doc_float_props(const pd_doc* doc, pd_block_id flt, pd_float_props* out);
PD_API pd_status pd_doc_section_props(const pd_doc* doc, pd_block_id section, pd_section_props* out);
/** fill section props with A4 portrait, 1in margins, one column */
PD_API void      pd_section_props_init(pd_section_props* props);
PD_API pd_status pd_doc_table_props(const pd_doc* doc, pd_block_id table, pd_table_props* out);
PD_API pd_status pd_doc_cell_props(const pd_doc* doc, pd_block_id cell, pd_cell_props* out);
/** automatic width and columns, 4pt padding, 0.4pt black rules, no header rows */
PD_API void      pd_table_props_init(pd_table_props* props);

/* ------------------------------------------------------------------ */
/* Operations: every change, all undoable                             */
/* ------------------------------------------------------------------ */

/*
 * Text and paragraph edits. Positions passed in are adjusted for the
 * edit and, where noted, the resulting position is returned through
 * *after (may be NULL), ready to become the new caret.
 */

/**
 * insert UTF-8 text; format PD_FORMAT_INHERIT continues the format of the
 * text to the left (or of the paragraph start). U+FFFC is reserved for
 * inline objects and is rejected, as is invalid UTF-8.
 */
PD_API pd_status pd_doc_insert_text(pd_doc* doc, pd_pos at, const char* utf8, size_t len, pd_format_id format,
                                    pd_pos* after);
/**
 * delete a range; a range spanning paragraphs merges the first and last
 * (Backspace/Delete across a break) and removes the blocks between them.
 * Both ends must be paragraphs under the same parent.
 */
PD_API pd_status pd_doc_delete(pd_doc* doc, pd_range range, pd_pos* after);
/** Enter: split a paragraph; the new paragraph gets the next_style of the old one at its end */
PD_API pd_status pd_doc_split(pd_doc* doc, pd_pos at, pd_pos* after);
/** insert an inline object at a position */
PD_API pd_status pd_doc_insert_inline(pd_doc* doc, pd_pos at, const pd_inline* obj, pd_pos* after);

/** apply character overrides to a range (direct formatting: bold, size, ...) */
PD_API pd_status pd_doc_set_char_props(pd_doc* doc, pd_range range, const pd_char_props* props);
/** remove direct formatting in a range, keeping character styles */
PD_API pd_status pd_doc_clear_char_props(pd_doc* doc, pd_range range, uint32_t mask);
PD_API pd_status pd_doc_set_char_style(pd_doc* doc, pd_range range, pd_style_id style);

/**
 * paragraph-level: style, direct properties (replacing the paragraph's
 * current direct properties; mask 0 clears them), role/level, list membership
 */
PD_API pd_status pd_doc_set_para_style(pd_doc* doc, pd_block_id paragraph, pd_style_id style);
PD_API pd_status pd_doc_set_para_props(pd_doc* doc, pd_block_id paragraph, const pd_para_props* props);
PD_API pd_status pd_doc_set_role(pd_doc* doc, pd_block_id paragraph, pd_role role, int32_t level);

/** what a paragraph is beyond its role, for formats that say so (Markdown, HTML) */
typedef struct {
    int32_t quote_depth;        /**< block quotes it is inside, 0-9; a QUOTE role with 0 counts as one */
    int32_t task;               /**< a list item's checkbox: 0 none, 1 open, 2 checked */
    int32_t loose;              /**< 1 = an item of a loose list (blank lines between its items) */
    char lang[32];              /**< a code block's language ("python"), UTF-8, "" = none */
    int32_t cont;               /**< 1 = a later block of the list item above (its list and level): no label of
                                     its own, not counted, indented as the item's text */
    char div_class[32];         /**< a named block it is in (Markdown's ::: warning, HTML's <div class>): a run of
                                     paragraphs with one name is one block; "!note" etc. are GitHub's alerts,
                                     block quotes of a kind. UTF-8, "" = none */
} pd_para_attrs;

PD_API pd_status pd_doc_para_attrs(const pd_doc* doc, pd_block_id paragraph, pd_para_attrs* out);
PD_API pd_status pd_doc_set_para_attrs(pd_doc* doc, pd_block_id paragraph, const pd_para_attrs* attrs);

/**
 * Document metadata as the source format wrote it -- a Markdown file's YAML
 * front matter, without its --- lines. UTF-8; NULL/0 clears it. Saved with
 * the document; not part of the undo history.
 */
PD_API pd_status pd_doc_set_metadata(pd_doc* doc, const char* text, size_t len);
PD_API const char* pd_doc_metadata(const pd_doc* doc, size_t* len);

PD_API pd_status pd_doc_set_list(pd_doc* doc, pd_block_id paragraph, pd_list_id list, int32_t level);

/**
 * structure: create a block of a kind as child index of parent (index -1 =
 * append). New paragraphs are empty; new sections and floats get one empty
 * paragraph; a STORY is created with parent 0 and gets one empty paragraph.
 */
PD_API pd_status pd_doc_insert_block(pd_doc* doc, pd_block_id parent, int32_t index, pd_block_kind kind,
                                     pd_block_id* out);
PD_API pd_status pd_doc_remove_block(pd_doc* doc, pd_block_id block);
PD_API pd_status pd_doc_move_block(pd_doc* doc, pd_block_id block, pd_block_id new_parent, int32_t index);
PD_API pd_status pd_doc_set_float_props(pd_doc* doc, pd_block_id flt, const pd_float_props* props);
PD_API pd_status pd_doc_set_section_props(pd_doc* doc, pd_block_id section, const pd_section_props* props);
PD_API pd_status pd_doc_set_table_props(pd_doc* doc, pd_block_id table, const pd_table_props* props);
PD_API pd_status pd_doc_set_cell_props(pd_doc* doc, pd_block_id cell, const pd_cell_props* props);
PD_API pd_status pd_doc_set_break(pd_doc* doc, pd_block_id brk, pd_break_kind kind);

/* ------------------------------------------------------------------ */
/* Undo / redo                                                        */
/* ------------------------------------------------------------------ */

/**
 * Operations between begin and end undo as one step ("Insert figure",
 * "Paste"). Groups nest; only the outermost counts. Consecutive typing
 * (inserts at the advancing caret) coalesces into one step on its own
 * until pd_doc_seal_undo, a caret jump, or a different operation.
 */
PD_API void      pd_doc_begin_group(pd_doc* doc, const char* label);
PD_API void      pd_doc_end_group(pd_doc* doc);
PD_API void      pd_doc_seal_undo(pd_doc* doc);
PD_API pd_status pd_doc_undo(pd_doc* doc);
PD_API pd_status pd_doc_redo(pd_doc* doc);
PD_API int32_t   pd_doc_can_undo(const pd_doc* doc);
PD_API int32_t   pd_doc_can_redo(const pd_doc* doc);
/** label of the step undo would revert, NULL if none; doc-owned */
PD_API const char* pd_doc_undo_label(const pd_doc* doc);
/** maximum remembered steps (default 1000; 0 = unlimited) */
PD_API void      pd_doc_set_undo_limit(pd_doc* doc, int32_t steps);
/** forget all undo and redo steps (e.g. after building a document by import); not inside a group */
PD_API void      pd_doc_clear_undo(pd_doc* doc);

/* ------------------------------------------------------------------ */
/* Markers: positions that follow edits (caret, selection, bookmarks) */
/* ------------------------------------------------------------------ */

typedef enum {
    PD_GRAVITY_LEFT = 0,        /**< stays before text inserted at its position */
    PD_GRAVITY_RIGHT = 1        /**< moves after text inserted at its position (caret) */
} pd_gravity;

PD_API pd_status pd_doc_marker_new(pd_doc* doc, pd_pos pos, pd_gravity gravity, pd_marker_id* out);
PD_API void      pd_doc_marker_free(pd_doc* doc, pd_marker_id marker);
PD_API pd_status pd_doc_marker_get(const pd_doc* doc, pd_marker_id marker, pd_pos* out);
PD_API pd_status pd_doc_marker_set(pd_doc* doc, pd_marker_id marker, pd_pos pos);

/* ------------------------------------------------------------------ */
/* Tracked changes                                                    */
/* ------------------------------------------------------------------ */

/*
 * A tracked change is text whose character format names a revision: an
 * insertion still shown as such, or a deletion kept in the text until it
 * is accepted. Revisions are a table of (kind, author, date); the same
 * three give the same id. The table is not part of the undo history.
 */
typedef enum {
    PD_REV_INSERT = 1,
    PD_REV_DELETE = 2
} pd_rev_kind;

typedef struct {
    int32_t kind;               /**< pd_rev_kind */
    char author[64];            /**< UTF-8 */
    char date[32];              /**< ISO 8601 ("2026-10-06T03:40:00Z"), may be empty */
} pd_revision;

PD_API pd_status pd_doc_revision_add(pd_doc* doc, const pd_revision* rev, pd_rev_id* out);
PD_API pd_status pd_doc_revision_get(const pd_doc* doc, pd_rev_id rev, pd_revision* out);
/** revisions are numbered 1..count */
PD_API int32_t   pd_doc_revision_count(const pd_doc* doc);
/** authors in order of first appearance among revisions and comments (0-based), -1 if unknown */
PD_API int32_t   pd_doc_author_index(const pd_doc* doc, const char* author);
/** a colour told apart from other authors' (0xFFRRGGBB) */
PD_API uint32_t  pd_doc_author_color(const pd_doc* doc, const char* author);

/**
 * Record edits as tracked changes by author (NULL or "" stops): text
 * typed or pasted becomes an insertion; deleted text is kept and marked
 * deleted, except the author's own pending insertions, which simply go.
 * Paragraph breaks are not tracked: deleting across paragraphs marks
 * their text and keeps the paragraphs.
 */
PD_API pd_status pd_doc_set_tracking(pd_doc* doc, const char* author);
/** the tracking author, NULL when not tracking */
PD_API const char* pd_doc_tracking(const pd_doc* doc);

typedef enum {
    PD_MARKUP_BALLOONS = 0,     /**< insertions in the author's colour, deletions out of the text (balloons) */
    PD_MARKUP_INLINE = 1,       /**< insertions underlined, deletions struck through, in the author's colour */
    PD_MARKUP_FINAL = 2,        /**< as if every change were accepted */
    PD_MARKUP_ORIGINAL = 3      /**< as if every change were rejected */
} pd_markup_mode;

/** how layouts show tracked changes (default PD_MARKUP_BALLOONS) */
PD_API void      pd_doc_set_markup(pd_doc* doc, int32_t mode);
PD_API int32_t   pd_doc_markup(const pd_doc* doc);
/** accept (1) or reject (0) every change in a range, as one undo step */
PD_API pd_status pd_doc_revision_resolve(pd_doc* doc, pd_range range, int32_t accept);
/**
 * The next change from a position (dir > 0) or the previous one (dir < 0):
 * the stretch of one paragraph's text in one revision. PD_ERR_RANGE when
 * there is none.
 */
PD_API pd_status pd_doc_revision_find(const pd_doc* doc, pd_pos from, int32_t dir, pd_range* out,
                                      pd_rev_id* rev);

/* ------------------------------------------------------------------ */
/* Comments                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char author[64];
    char date[32];
    const char* text;           /**< UTF-8, paragraphs separated by '\n'; get: doc-owned until the comment changes */
    uint32_t text_len;
    pd_comment_id parent;       /**< the comment this replies to (0: none); a reply shares its range */
    int32_t resolved;
    pd_range range;             /**< get: where the commented text is now (markers follow edits) */
} pd_comment;

/** add a comment on a range (ignored for a reply), as an undo step */
PD_API pd_status pd_doc_comment_add(pd_doc* doc, const pd_comment* comment, pd_comment_id* out);
/** PD_ERR_ARG for an id never given out or removed */
PD_API pd_status pd_doc_comment_get(const pd_doc* doc, pd_comment_id id, pd_comment* out);
/** change text, author, date and resolved (the range and parent stay) */
PD_API pd_status pd_doc_comment_set(pd_doc* doc, pd_comment_id id, const pd_comment* comment);
/** remove a comment and its replies */
PD_API pd_status pd_doc_comment_remove(pd_doc* doc, pd_comment_id id);
/** ids are 1..count, removed ones included */
PD_API int32_t   pd_doc_comment_count(const pd_doc* doc);

/* ------------------------------------------------------------------ */
/* Change notification                                                */
/* ------------------------------------------------------------------ */

typedef enum {
    PD_CHANGE_TEXT = 0,         /**< paragraph text or inline objects changed */
    PD_CHANGE_FORMAT = 1,       /**< runs, paragraph props, role or list changed */
    PD_CHANGE_STRUCTURE = 2,    /**< blocks inserted, removed or moved under parent */
    PD_CHANGE_STYLE = 3,        /**< a named style changed: every user may need relayout */
    PD_CHANGE_SECTION = 4       /**< page geometry or headers/footers changed */
} pd_change_kind;

typedef struct {
    int32_t kind;               /**< pd_change_kind */
    pd_block_id block;          /**< changed block (STRUCTURE: the parent) */
    pd_style_id style;          /**< STYLE changes */
    uint64_t revision;
} pd_change;

/**
 * Called after every applied operation, also during undo/redo. Layout
 * can instead poll pd_block_info.revision; the listener is for UIs.
 * The callback must not modify the document.
 */
typedef void (*pd_doc_listener)(void* user, const pd_change* change);
PD_API void pd_doc_set_listener(pd_doc* doc, pd_doc_listener fn, void* user);

/* ------------------------------------------------------------------ */
/* Serialization: JData (JSON text) and BJData (binary)               */
/* ------------------------------------------------------------------ */

/** receives output bytes; return nonzero to abort */
typedef int (*pd_writer)(void* user, const void* data, size_t len);

/**
 * Pictures given by address rather than embedded (PD_INLINE_IMAGE with
 * resource 0 and the address in source): fetch writes the bytes of one
 * through write(sink, data, len) and returns 0, or nonzero when it cannot.
 * Each picture fetched becomes a resource of the document; one with no size
 * takes its pixel size at 96 dpi. Not part of the undo history. Returns how
 * many were loaded.
 */
typedef int (*pd_image_fetch)(void* user, const char* address, pd_writer write, void* sink);
PD_API int32_t pd_doc_load_images(pd_doc* doc, pd_image_fetch fetch, void* user);

typedef enum {
    PD_JDATA_AUTO = -1,         /**< load only: detect from the first bytes */
    PD_JDATA_TEXT = 0,          /**< JData in JSON text (.pdoc) */
    PD_JDATA_BINARY = 1         /**< JData in BJData Draft-2+ (.bpdoc) */
} pd_jdata_format;

/**
 * Native format: a JData document. "_DataInfo_" records the generator and
 * format version; styles, formats, lists and resources are arrays; the
 * block tree uses JData's _TreeNode_/_TreeChildren_ keywords; resources
 * are JData byte streams ("_ByteStream_": base64 in text, raw H in
 * BJData); format runs are integer matrices (an optimized N-D array in
 * BJData). Lossless: load(save(doc)) == doc, block and style ids
 * included; character formats are saved compacted (only those in use,
 * numbered by first use), so a save depends on the content alone, not on
 * the editing history. Undo history is not saved. Lengths are in sp.
 * Importers/exporters for HTML, Markdown, RTF, DOCX and LaTeX sit on top
 * of this API rather than inside the core.
 */
PD_API pd_status pd_doc_save(const pd_doc* doc, pd_jdata_format format, pd_writer fn, void* user);
/** load untrusted data; malformed or inconsistent input gives PD_ERR_FORMAT, never a crash */
PD_API pd_status pd_doc_load(const void* data, size_t len, pd_jdata_format format, pd_doc** out);

/* ------------------------------------------------------------------ */
/* Deltas: journaling and replication                                 */
/* ------------------------------------------------------------------ */

/** receives one delta: compact JSON, valid for the call */
typedef void (*pd_delta_fn)(void* user, const char* json, size_t len);

/**
 * Write an exact snapshot of the document (native format, ids as they
 * are, nothing compacted) and from then on report every finished
 * operation -- undo and redo, tracked edits, comments included -- to
 * delta as a delta: the new state of what the operation changed, and the
 * format, revision, list and resource table entries made since the last
 * one. A document loaded from the snapshot and given the deltas in order
 * with pd_doc_apply_delta stays identical to this one, ids included: a
 * durable journal (snapshot + appended deltas) recovers the document after
 * a crash, and a follower mirrors an editor. Markers are not part of it.
 * fn may be NULL to only start reporting; delta NULL stops.
 */
PD_API pd_status pd_doc_snapshot(pd_doc* doc, pd_jdata_format format, pd_writer fn, void* user, pd_delta_fn delta,
                                 void* delta_user);
/**
 * Apply a delta made by the document this one was loaded from (its
 * snapshot, then every delta before this one). Not an undoable step; the
 * undo history is cleared, since its steps no longer describe the
 * document. PD_ERR_STATE when the delta does not follow on from this
 * document's state, PD_ERR_FORMAT when it is malformed; either way the
 * document may be partly updated and should be reloaded from a snapshot.
 */
PD_API pd_status pd_doc_apply_delta(pd_doc* doc, const char* json, size_t len);

/* ------------------------------------------------------------------ */
/* Layout bridge                                                      */
/* ------------------------------------------------------------------ */

/**
 * Fill a pd_para (cleared first) with a document paragraph: runs are
 * resolved to fonts through the resolver, inline objects become boxes of
 * their size, and the paragraph's alignment, indents and line spacing go
 * into *params (initialized here) and the paragraph shape for the given
 * column width. Fields render as their placeholder text until the page
 * builder supplies values.
 */
PD_API pd_status pd_doc_para_build(const pd_doc* doc, pd_block_id paragraph, pd_sp column_width,
                                   pd_para* para, pd_params* params);

#ifdef __cplusplus
}
#endif

#endif /* PARADE_DOC_H */
