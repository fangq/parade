/*
 * Parade import and export: HTML, Markdown, LaTeX, RTF, DOCX, plain text
 *
 * The converters sit on top of the public document API: an import builds a
 * new document through ordinary operations (its undo history is cleared),
 * an export walks the document read-only. What each format keeps:
 *
 *   HTML      headings, paragraphs (alignment), lists (start numbers,
 *             checkboxes), nested quotes, code (its language), definition
 *             lists, rules, character formats, links (titles), images
 *             (data: URIs or by address, alt text, titles), tables,
 *             figures with captions, footnotes; import is tolerant (real
 *             web pages, office clipboard fragments)
 *   Markdown  CommonMark blocks and inlines -- quotes and lists nested in
 *             each other, several blocks in an item, reference links (also
 *             defined in quotes and items), fenced code with its language,
 *             rules, raw HTML kept as it is -- plus GFM tables (column
 *             alignment), task lists, strikethrough, bare www./https://
 *             links, footnotes and alerts ([!NOTE]); pandoc heading ids,
 *             code attributes, definition lists, image sizes, ^sup^ and
 *             H~2~O subscripts, inline notes ^[..], line blocks and :::
 *             fenced divs; YAML front matter as the document's metadata;
 *             underline, super/subscript as inline HTML. Pictures by
 *             address are kept as addresses (pd_doc_load_images fetches
 *             them)
 *   LaTeX     export only: article class, sectioning, lists, tables
 *             (longtable with repeated header), figures, footnotes,
 *             cross-references, inline and display equations
 *   RTF       paragraphs, character and paragraph formats, headings
 *             (outline levels), lists as text labels, tables, footnotes,
 *             PNG/JPEG pictures, page breaks
 *   DOCX      WordprocessingML: styles, numbering, tables, footnotes,
 *             hyperlinks, images, sections (size, margins, columns),
 *             headers/footers with page fields
 *   TEXT      UTF-8, one paragraph per line
 *
 * Clipboard: pd_doc_export_range writes the selected part of a document
 * and pd_doc_paste inserts data of any importable format at a position
 * (one undo step, "Paste").
 */

#ifndef PARADE_CONVERT_H
#define PARADE_CONVERT_H

#include "parade_doc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PD_CONV_TEXT = 0,
    PD_CONV_HTML = 1,
    PD_CONV_MARKDOWN = 2,
    PD_CONV_LATEX = 3,          /**< export only */
    PD_CONV_RTF = 4,
    PD_CONV_DOCX = 5,
    PD_CONV_JDATA = 6           /**< the native format (JData text), for clipboards */
} pd_conv_format;

/** the whole document */
PD_API pd_status pd_doc_export(const pd_doc* doc, pd_conv_format format, pd_writer fn, void* user);
/** part of a document: the paragraphs from range.start to range.end, cut at the offsets */
PD_API pd_status pd_doc_export_range(const pd_doc* doc, pd_range range, pd_conv_format format, pd_writer fn,
                                     void* user);
/** a new document from data; malformed input gives PD_ERR_FORMAT (or as much as could be read) */
PD_API pd_status pd_doc_import(const void* data, size_t len, pd_conv_format format, pd_doc** out);
/** insert imported content at a position (splitting the paragraph for multi-paragraph content) */
PD_API pd_status pd_doc_paste(pd_doc* doc, pd_pos at, const void* data, size_t len, pd_conv_format format,
                              pd_pos* after);
/**
 * A drawing read from DOCX (a canvas or group whose Word XML is kept with it) made again from edited XML: the
 * same pictures, styles, text boxes' stories and fallback, its shapes as the new XML has them. Its new
 * description is a new resource (*out); the old one is left as it is (an editor swaps the object's resource,
 * which undo can take back).
 */
PD_API pd_status pd_docx_drawing_rebuild(pd_doc* doc, pd_res_id drawing, const char* xml, size_t len, pd_res_id* out);
/** guess the format of data from its first bytes (DOCX zip, RTF, HTML, JData; else Markdown or text) */
PD_API pd_conv_format pd_conv_detect(const void* data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* PARADE_CONVERT_H */
