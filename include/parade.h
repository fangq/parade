/*
 * Parade - a portable, deterministic, TeX-quality text layout core
 *
 * Public C API. Everything crossing this boundary is a plain C type:
 * opaque handles, fixed-size integers and POD structs. All layout values
 * are integers in scaled points (1 pt = 65536 sp), so the same input gives
 * bit-identical output on every compiler, CPU and operating system.
 *
 * Memory rule: whatever Parade allocates, Parade frees (pd_*_free).
 * Threading rule: one object is used by one thread at a time; distinct
 * objects may be used concurrently. A pd_font may be shared read-only by
 * any number of paragraphs on any threads.
 */

#ifndef PARADE_H
#define PARADE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(PD_BUILD_DLL)
#define PD_API __declspec(dllexport)
#elif defined(_WIN32) && defined(PD_USE_DLL)
#define PD_API __declspec(dllimport)
#else
#define PD_API
#endif

#define PD_VERSION_MAJOR 0
#define PD_VERSION_MINOR 1
#define PD_VERSION_PATCH 0

/** scaled points: 1/65536 pt, range +/-32767 pt */
typedef int32_t pd_sp;
#define PD_SP_PER_PT 65536
#define PD_PT(x) ((pd_sp)((x) * PD_SP_PER_PT))

/** error codes; every fallible call returns one of these */
typedef enum {
    PD_OK = 0,
    PD_ERR_ARG = -1,        /**< invalid argument */
    PD_ERR_NOMEM = -2,      /**< allocation failed */
    PD_ERR_IO = -3,         /**< file could not be read */
    PD_ERR_FONT = -4,       /**< font data malformed or unsupported */
    PD_ERR_RANGE = -5,      /**< index or offset out of range */
    PD_ERR_STATE = -6       /**< call not valid in current state (e.g. not broken yet) */
} pd_status;

PD_API const char* pd_version(void);
PD_API const char* pd_status_string(pd_status s);

/* ------------------------------------------------------------------ */
/* Fonts                                                              */
/* ------------------------------------------------------------------ */

typedef struct pd_font pd_font;

/** font-wide metrics in font design units (unhinted) */
typedef struct {
    int32_t units_per_em;
    int32_t ascender;       /**< positive, above baseline */
    int32_t descender;      /**< negative, below baseline */
    int32_t line_gap;
    int32_t x_height;       /**< 0 if the font does not say */
    int32_t cap_height;     /**< 0 if the font does not say */
    int32_t num_glyphs;
    int32_t has_kerning;    /**< 1 if a kern table or GPOS kern feature was found */
} pd_font_metrics;

/** load a TrueType/OpenType (.ttf/.otf/.ttc) font; face_index selects a face in a collection */
PD_API pd_status pd_font_load_file(const char* path, int32_t face_index, pd_font** out);
/** load from memory; the bytes are copied, the caller keeps ownership of data */
PD_API pd_status pd_font_load_memory(const void* data, size_t len, int32_t face_index, pd_font** out);
PD_API void      pd_font_free(pd_font* font);
PD_API pd_status pd_font_get_metrics(const pd_font* font, pd_font_metrics* out);
/** glyph id for a Unicode code point, 0 (.notdef) if not mapped */
PD_API uint32_t  pd_font_glyph_index(const pd_font* font, uint32_t codepoint);
/** advance width of a glyph in design units */
PD_API int32_t   pd_font_glyph_advance(const pd_font* font, uint32_t glyph);
/** horizontal pair kerning between two glyphs in design units */
PD_API int32_t   pd_font_kerning(const pd_font* font, uint32_t left, uint32_t right);

/* ------------------------------------------------------------------ */
/* Styles                                                             */
/* ------------------------------------------------------------------ */

/** a character style; copied into the paragraph when text is added */
typedef struct {
    const pd_font* font;    /**< must outlive every paragraph that uses it */
    pd_sp size;             /**< font size, e.g. PD_PT(10) */
    int32_t space_stretch;  /**< interword stretch, per-mille of the space width (TeX: 500) */
    int32_t space_shrink;   /**< interword shrink, per-mille of the space width (TeX: 333) */
    int32_t kerning;        /**< 1 to apply pair kerning */
    uint32_t color;         /**< 0xAARRGGBB, opaque to the layout engine */
    int32_t user;           /**< caller tag, returned with every glyph */
} pd_style;

/** fill a style with defaults for the given font and size */
PD_API void pd_style_init(pd_style* style, const pd_font* font, pd_sp size);

/* ------------------------------------------------------------------ */
/* Paragraph parameters                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    PD_BREAK_OPTIMAL = 0,   /**< total-fit (Knuth-Plass) over the whole paragraph */
    PD_BREAK_GREEDY = 1     /**< first-fit, word-processor style */
} pd_break_mode;

typedef enum {
    PD_ALIGN_JUSTIFY = 0,
    PD_ALIGN_LEFT = 1,      /**< ragged right, spaces at natural width */
    PD_ALIGN_RIGHT = 2,
    PD_ALIGN_CENTER = 3
} pd_align;

typedef struct {
    pd_break_mode mode;
    pd_align align;
    pd_sp width;            /**< line width when no shape is set */
    pd_sp indent;           /**< first-line indent */
    int32_t line_penalty;   /**< TeX \linepenalty (10) */
    int32_t adj_demerits;   /**< TeX \adjdemerits (10000) */
    int32_t double_hyphen_demerits; /**< TeX \doublehyphendemerits (10000) */
    int32_t final_hyphen_demerits;  /**< TeX \finalhyphendemerits (5000) */
    int32_t hyphen_penalty;         /**< soft-hyphen break penalty (50) */
    int32_t ex_hyphen_penalty;      /**< break after an explicit '-' (50) */
    int32_t tex_badness;    /**< 1: TeX badness capped at 10000; 0 (default): uncapped cubic */
    pd_sp rag_stretch;      /**< non-justified: per-line slack that costs badness 100 (default 3em at 10pt) */
    pd_sp baseline_skip;    /**< minimum baseline-to-baseline distance; 0 = from font metrics */
    int32_t line_spacing;   /**< per-mille multiplier for font-derived line height (1000) */
    /* editing stability */
    int64_t hysteresis;     /**< extra demerits for a break that differs from the previous layout (0 = off) */
    int32_t freeze_offset;  /**< keep previous lines that end at or before this byte offset (-1 = off) */
} pd_params;

PD_API void pd_params_init(pd_params* params);

/* ------------------------------------------------------------------ */
/* Paragraphs                                                         */
/* ------------------------------------------------------------------ */

typedef struct pd_para pd_para;

PD_API pd_status pd_para_new(pd_para** out);
PD_API void      pd_para_free(pd_para* para);

/**
 * Start (re)building the content. The previous content and layout are
 * kept internally, so the next pd_para_break() only recomputes what the
 * edit affected and can keep unchanged lines stable.
 */
PD_API void pd_para_clear(pd_para* para);

/** append UTF-8 text in a style; byte offsets continue across calls */
PD_API pd_status pd_para_add_text(pd_para* para, const char* utf8, size_t len, const pd_style* style);
/** append an inline object (image, equation, ...); it occupies U+FFFC (3 bytes) in the text */
PD_API pd_status pd_para_add_object(pd_para* para, pd_sp width, pd_sp height, pd_sp depth, int32_t user);
/** append raw TeX-style glue; it is a legal breakpoint when it follows a box */
PD_API pd_status pd_para_add_glue(pd_para* para, pd_sp width, pd_sp stretch, pd_sp shrink, int32_t stretch_fil);
/** append a TeX-style penalty: >= 10000 forbids, <= -10000 forces a break */
PD_API pd_status pd_para_add_penalty(pd_para* para, int32_t penalty, pd_sp width, int32_t flagged);

/**
 * Per-line geometry (TeX \parshape): line i uses indent[min(i,n-1)] and
 * width[min(i,n-1)]. Use it to wrap text around a figure. n = 0 resets.
 */
PD_API pd_status pd_para_set_shape(pd_para* para, int32_t n, const pd_sp* indent, const pd_sp* width);

typedef struct {
    int32_t lines;
    int64_t demerits;       /**< total cost of the chosen breaks */
    int32_t overfull;       /**< lines that could not be fitted */
    int32_t underfull;      /**< justified lines left short for lack of stretchable glue */
    int32_t reused_breakpoints; /**< DP states reused from the previous layout */
    int32_t frozen_lines;   /**< lines copied unchanged because of freeze_offset */
    pd_sp height;           /**< paragraph height from top of first line to bottom of last */
} pd_break_info;

/** choose line breaks and set glue; info may be NULL */
PD_API pd_status pd_para_break(pd_para* para, const pd_params* params, pd_break_info* info);

typedef struct {
    uint32_t text_start;    /**< first byte offset on the line */
    uint32_t text_end;      /**< one past the last byte offset on the line */
    pd_sp x;                /**< left edge of the content (indent + alignment) */
    pd_sp baseline;         /**< baseline y from paragraph top, downward */
    pd_sp width;            /**< set width of the content */
    pd_sp ascent;
    pd_sp descent;          /**< positive, below baseline */
    int32_t ratio;          /**< adjustment ratio in per-mille (+stretch / -shrink) */
    int32_t badness;
    int32_t hyphenated;     /**< 1 if the line ends at a hyphenation point */
    int32_t overfull;       /**< 1 if the content is wider than the line even at full shrink */
    int32_t underfull;      /**< 1 if a justified line is short but has no stretchable glue
                                 (e.g. a single word fragment): ratio is 0, badness is at the ceiling */
} pd_line;

PD_API int32_t   pd_para_line_count(const pd_para* para);
PD_API pd_status pd_para_get_line(const pd_para* para, int32_t index, pd_line* out);

typedef enum {
    PD_GLYPH = 0,
    PD_OBJECT = 1           /**< inline object; glyph field is unused */
} pd_glyph_kind;

/** one positioned glyph or object, coordinates relative to the paragraph */
typedef struct {
    uint32_t glyph;
    uint32_t cluster;       /**< byte offset of the source text */
    pd_sp x;                /**< pen position (left edge) */
    pd_sp y;                /**< baseline */
    pd_sp advance;
    int32_t style;          /**< index into the paragraph's style table, -1 for objects */
    int32_t kind;           /**< pd_glyph_kind */
    int32_t user;           /**< style user tag or object user id */
} pd_glyph;

/**
 * Copy the positioned glyphs of one line into buf. If buf is NULL or too
 * small, *count receives the required size and PD_ERR_RANGE is returned
 * (PD_OK when buf is NULL).
 */
PD_API pd_status pd_para_get_glyphs(const pd_para* para, int32_t line, pd_glyph* buf, int32_t cap, int32_t* count);
/** the style table entry used by pd_glyph.style */
PD_API pd_status pd_para_get_style(const pd_para* para, int32_t index, pd_style* out);

/* ------------------------------------------------------------------ */
/* Hit testing, for carets and selection                              */
/* ------------------------------------------------------------------ */

/** byte offset closest to a point in paragraph coordinates */
PD_API pd_status pd_para_hit_test(const pd_para* para, pd_sp x, pd_sp y, uint32_t* offset, int32_t* line);
/** caret position for a byte offset; at a line break the caret goes to the start of the next line */
PD_API pd_status pd_para_caret(const pd_para* para, uint32_t offset, int32_t* line, pd_sp* x, pd_sp* baseline);

/** total UTF-8 text length of the paragraph in bytes */
PD_API uint32_t pd_para_text_length(const pd_para* para);

#ifdef __cplusplus
}
#endif

#endif /* PARADE_H */
