# Parade

A portable, deterministic text layout core with TeX-quality line breaking,
built to be edited interactively. Plain C99, no dependencies, with a C API
designed to be called from Lazarus/Free Pascal, C, C++ or Python.

## Status (v0.1, paragraph layer)

- **Fonts**: reads TrueType/OpenType/TTC metrics directly: `cmap` 4/12,
  `hmtx`, `hhea`/`OS/2` vertical metrics, legacy `kern`, and GPOS pair
  kerning (PairPos 1/2, Extension lookups). Checked against fontTools on
  1033 installed fonts with 0 mismatches (`make oracle`).
- **Paragraphs**: UTF-8 text in styled runs, inline objects, raw
  box/glue/penalty items, spaces, NBSP, ZWSP, soft and explicit hyphens,
  forced newlines, CJK breaking with basic kinsoku.
- **Line breaking**: integer Knuth–Plass (fitness classes, adjacency,
  double/final hyphen demerits, uncapped cubic badness), greedy mode,
  justified or ragged left/right/center, `\parshape` for wrapping around
  figures.
- **Editing**: after an edit, `pd_para_break` reuses the DP states before
  the first changed item. `hysteresis` biases toward the previous breaks,
  and `freeze_offset` keeps the lines above the caret fixed.
- **Output**: positioned glyphs and objects per line, caret position from
  a text offset, and text offset from a point (hit testing).
- **Determinism**: every layout quantity is an integer in scaled points
  (1/65536 pt), and glue is set by exact cumulative integer division.

## Document model (`include/parade_doc.h`)

- **Tree**: root → sections (page geometry, columns, header/footer
  stories) → paragraphs, floats (figures/tables with placement and
  wrap), tables, breaks; stories hold headers, footers and footnotes.
- **Semantics vs appearance**: a paragraph has a role (heading 1–6,
  caption, quote, code, equation, …) and a style; lists are separate.
- **Styles**: named paragraph and character styles with inheritance;
  direct formatting is interned as style + overrides, so runs are ids.
- **Inline objects** (images, equations, fields, footnote marks, links,
  bookmarks, tabs) occupy U+FFFC, so document and layout offsets agree.
- **Editing**: every change is an operation recording its inverse —
  undo/redo, labelled groups, typing coalescing, markers that follow
  edits, per-block revisions and a change listener.
- **Native format**: JData (`.pdoc`, JSON) or BJData (`.bpdoc`), using
  `_TreeNode_`/`_TreeChildren_`, `_ByteStream_` and optimized N-D arrays;
  the codec (`src/pd_json.c`) is a C99 port of mimamo's
  mmm_json/mmm_bjdata/mmm_base64 conventions. Loading untrusted files is
  fuzzed under ASan.
- **Layout bridge**: `pd_doc_para_build` turns a document paragraph into a
  `pd_para` with fonts from a host resolver.

## Page builder (`include/parade_layout.h`)

- Sections give page size, margins, columns and page numbering; each
  paragraph is broken at its real width and cached by (revision, width,
  style revision, field values), so an edit re-breaks one paragraph.
- Columns are cut TeX-style from line boxes, collapsed paragraph spacing
  and penalties: widows, orphans, keep-lines and keep-with-next are rules;
  the break is the cheapest candidate (penalty + cubic underfill badness).
- Floats are placed LaTeX-style: here if it fits, else queued for the top
  of a later column, the bottom of a page showing the anchor, or a float
  page; order is kept, the queue is flushed at the section end.
- Headers and footers (first/even variants) are laid out per page; fields
  (page, pages, section page, SEQ counters, cross-references to number or
  page, running headings) are sized by their values, with page references
  settled in a second pass as in LaTeX. List labels hang in the margin.
- Output: per-page display lists (glyphs, images, object boxes, rules) in
  page coordinates, hit testing and caret positions across pages.
- 3000 paragraphs / 150 pages: full layout 63 ms; a keystroke plus update
  0.3 ms.
- Not yet: footnote bodies, table layout (cells are stacked), text
  wrapping beside floats, continuous section breaks, optimal (rather than
  greedy-with-lookback) page breaking.

## PDF (`pd_layout_write_pdf`)

PDF 1.7 from the same display list the screen uses. TrueType fonts are
embedded as subsets (CIDFontType2, Identity-H, glyph numbering kept),
CFF/CJK fonts as Type 3 fonts drawn from Parade's outlines (a 16 MB Noto
CJK costs only the glyphs used), ToUnicode maps carry each glyph's code
point (generated text such as page numbers and list labels included),
JPEG passes through, PNG (gray/RGB/palette/alpha) is re-encoded with soft
masks, headings become bookmarks. Output is deterministic (no dates, id
from content). Streams use Parade's own deflate (`src/pd_zlib.c`,
cross-checked against zlib). `make pdf-check` runs Ghostscript and
poppler over the output.

## International text

- Unicode 15.1 tables generated from the UCD (`make unidata`,
  `tools/gen_unidata.py`). UAX #14 line breaking, UAX #29 grapheme
  clusters (carets move by grapheme) and the full UAX #9 bidi algorithm,
  each passing 100% of the Unicode conformance files (`make conformance`).
- Complex scripts: build with `HARFBUZZ=1` and word segments containing
  Arabic, Indic, Southeast Asian etc. characters are shaped by HarfBuzz
  (with the whole paragraph as context); Latin/CJK keep the built-in
  shaper. Without HarfBuzz the interface stays and returns nothing.
- Font fallback: `pd_doc_set_fallback_fonts` lists fonts tried for
  characters the run's font lacks (marks follow their base).
- Hyphenation: Liang patterns from libhyphen `.dic` files
  (`pd_hyph_load_file("/usr/share/hyphen/hyph_en_US.dic", &h)`), either per
  style (`pd_style.hyph`) or per language in a document
  (`pd_doc_set_hyphenator(doc, "en", h)`, used where the paragraph has
  `hyphenate` on). Pattern points become TeX discretionaries (flagged,
  penalty 50 by default).

## Lazarus / Free Pascal (`pascal/`)

- `parade.pas`: the binding for all three headers (static link by default,
  `-dPARADE_DYNAMIC` for the shared library). Record layouts are checked
  against the C compiler's on every `make pascal` (about 220 sizes/offsets).
- `paradeedit.pas`: `TParadeEdit`, a page-view rich text editor control.
  Glyphs are rasterized by Parade (exact-area coverage, integer only), so
  the screen shows exactly the computed layout on every widgetset. Caret
  and selection are document markers; typing, Enter/Shift+Enter,
  Backspace/Delete across paragraphs, arrows/Home/End/PgUp/PgDn with
  Shift selection, mouse click/drag/double-click, Ctrl+Z/Y/A/B/I/U/C/X/V,
  zoom (Ctrl+wheel), paragraph styles, multi-line paste as paragraphs,
  JData/BJData load and save.
- `demo/paradedemo`: a small word processor on the control.

```
make pascal                                   # ABI + API tests (no GUI)
make pascal-edit LAZDIR=/path/to/Lazarus42/   # editor driven headless under Xvfb
make pascal-demo LAZDIR=/path/to/Lazarus42/
```

## Build

    make            # build/libparade.a, build/libparade.so, tests, bench
    make test       # unit tests (run under a 2 GB address-space cap)
    make asan       # tests under AddressSanitizer + UBSan (in build-asan/)
    make bench      # greedy vs optimal quality and speed (GPL-3 text corpus)
    make oracle     # font reader vs fontTools on every installed font
    make view       # render test scenes to build/parade_view.svg/.png
    make pages      # lay out a sample document, draw its pages to build/pages.svg
    make pretty     # astyle formatting

Test fonts: `PARADE_TEST_FONT` (default Liberation Serif) and
`PARADE_TEST_CJK_FONT` (default Noto Sans CJK). Memory caps: `MEMLIMIT_KB`
for test/bench, `ORACLE_MAX_MEM` (MB) for the fontTools worker.

## Minimal use

```c
pd_font* font;  pd_para* p;  pd_style st;  pd_params prm;
pd_font_load_file("LiberationSerif-Regular.ttf", 0, &font);
pd_para_new(&p);
pd_style_init(&st, font, PD_PT(10));
pd_para_add_text(p, text, len, &st);
pd_params_init(&prm);
prm.width = PD_PT(345);
pd_para_break(p, &prm, NULL);
/* for each line: pd_para_get_line, pd_para_get_glyphs -> draw */
/* on edit: pd_para_clear, add the new text, pd_para_break (incremental) */
```

## Layout

    include/parade.h     public C API: fonts, rasterizer, paragraphs, line breaking
    include/parade_doc.h public C API: document model, editing, JData I/O
    include/parade_layout.h public C API: pages, display lists, carets
    src/pd_font.c        font metrics reader
    src/pd_raster.c      glyph outlines (glyf) and the integer rasterizer
    src/pd_cff.c         CFF Type 2 charstrings
    src/pd_para.c        content building, simple shaper, output, hit testing
    src/pd_break.c       total-fit / first-fit line breaking
    src/pd_json.c        JData/BJData codec (port of mimamo conventions)
    src/pd_doc.c         document tree, styles, lists, operations, undo
    src/pd_doc_io.c      native JData/BJData save and load
    src/pd_doc_layout.c  document paragraph -> pd_para
    src/pd_layout.c      page builder: columns, floats, headers, fields, display lists
    src/pd_pdf.c         PDF writer
    src/pd_zlib.c        deflate/inflate/crc32
    src/pd_unidata.c     generated Unicode property tables
    src/pd_text.c        UTF-8, UAX #14 line breaks, UAX #29 graphemes
    src/pd_bidi.c        UAX #9 bidi
    src/pd_shape.c       optional HarfBuzz shaping
    src/pd_hyph.c        Liang hyphenation
    tests/               unit tests
    bench/               quality and speed benchmark
    tools/font_oracle.py fontTools cross-check through the C ABI (ctypes)
    tools/render.py      visual viewer: Parade positions + fontTools outlines -> SVG
    tools/pd_dump.c      builds/loads a document, dumps its pages as JSON
    tools/render_pages.py draws the dumped pages
    proto/               original standalone prototype (kp.c)

## Next

Page builder: footnote bodies, tables, wrap beside floats, continuous
sections, optimal pagination. Then import/export (HTML, Markdown, LaTeX,
RTF, DOCX), math, microtypography, fuzzing and CI.
