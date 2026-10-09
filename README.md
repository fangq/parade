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
- **Native format**: BJData (`.pdoc`, binary) or its JSON text (`.jdoc`), using
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
- Footnotes: a line carries the bodies of the notes it marks; the column
  keeps room for them at its bottom, under a 2in rule (TeX insertions).
  The note opens with its raised number.
- Tables: automatic column widths from the cells' narrowest and widest
  content (CSS-style; fixed widths and column spans honoured), cell
  padding, borders, backgrounds and vertical alignment. Rows are
  unbreakable boxes; columns break between rows and header rows repeat at
  the top of every continuation.
- Floats with `wrap` left/right sit at their anchor and the following
  paragraphs are re-broken with a narrower `\parshape` for the float's
  height (kept together with it).
- Continuous sections start below the previous one on the same page;
  a multi-column section ending there gets balanced columns.
- `PD_PAGES_OPTIMAL` sections choose all column breaks at once (shortest
  path over break candidates, states by column and page) and try
  paragraph variants a line looser or tighter (TeX `\looseness`, kept
  within TeX's default tolerance) near the worst columns, Mittelbach-style.
- 3000 paragraphs / 150 pages: full layout 67 ms; a keystroke plus update
  0.35 ms (1.6 ms with optimal page breaking).

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
  Paragraphs that cannot contain right-to-left text (no R/AL/AN
  characters or RLE/RLO/RLI, not an RTL paragraph) skip the bidi pass;
  `make conformance` also checks that this shortcut never misjudges a
  conformance case or any single code point.
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

## Math and microtypography

- Equations (`pd_math_layout`, `src/pd_math.c`): LaTeX notation laid out
  as TeX's Appendix G with an OpenType MATH font
  (`pd_doc_set_math_font`, e.g. Latin Modern Math): fractions, scripts,
  radicals, big operators with limits, `\left..\right` delimiters from
  size variants and glyph assemblies, accents, matrices, `\text` and
  math alphabets. Imported from Markdown `$..$`/`$$..$$` and HTML
  `span.math`; exported to LaTeX. Lengths saturate at 1024pt, so a
  malformed font cannot overflow the layout.
- Margin kerning and font expansion as in pdfTeX/microtype
  (`pd_params.protrusion`, `pd_params.expansion`, or
  `pd_doc_set_microtype` for a document): punctuation and hyphens hang
  into the margins, and glyphs may widen or narrow by up to the limit
  inside the Knuth-Plass search. Scaled glyphs carry `pd_glyph.scale`
  and are written with `Tz` in PDF.

## Import and export (`include/parade_convert.h`)

`pd_doc_import`, `pd_doc_export`, plus `pd_doc_export_range` and
`pd_doc_paste` for clipboards (one undo step). Built on the public
document API only.

| format   | in | out | keeps |
|----------|----|-----|-------|
| HTML     | yes | yes | headings, alignment, lists, quotes, code, character formats, links, data: images, tables (spans, backgrounds, header rows), figures, footnotes; tolerant reader (web pages, Windows CF_HTML) |
| Markdown | yes | yes | CommonMark (spec emphasis algorithm), GFM tables/strikethrough/footnotes, `$math$`, `{width=..}` images |
| LaTeX    | -   | yes | article that compiles with LuaLaTeX/pdfLaTeX: sections, lists, longtable, figures with `\caption`/`\label`/`\ref`, footnotes, equations |
| RTF      | yes | yes | style sheet, list table, tables (merged cells), footnotes, hyperlink fields, PNG/JPEG |
| DOCX     | yes | yes | styles (basedOn chains), numbering, tables, footnotes, hyperlinks (also field codes), images, sections, headers/footers, theme colours with tints and shades, the font table (alternate names, PANOSE, pitch: `pd_doc_font_class`), content controls (check boxes, drop-down lists, dates, text: `PD_INLINE_CONTROL`, `pd_doc_control_at`), charts (bar, column, line, area, pie, doughnut, scatter, drawn from their cached values by `src/pd_chart.c`; the chart part, its workbook and the theme kept and written back), East Asian and complex-script font slots (`family_ea`, `family_cs`, complex scripts' size, bold, italic), ruby (`PD_INLINE_RUBY`), the document grid (`line_pitch`, `snap_grid`); deterministic zip |
| text     | yes | yes | one paragraph per line |

`tools/pd_conv in.x out.y` converts between any of them and to PDF via
Parade's own layout. `make conv-check` checks the output with other
software: lxml, mistune, python-docx, LuaLaTeX, LibreOffice Writer when
installed, and reads python-docx/LibreOffice-written files back.

## Collaboration (`include/parade_sync.h`, optional)

Real-time co-editing through [yrs](https://github.com/y-crdt/y-crdt), the
Rust port of the Yjs CRDT, through its C API. Each editor's document gets
a `pd_sync`: local edits become updates for the others (`pd_sync_set_sender`),
updates received are merged and the document is edited to match
(`pd_sync_receive`), through the ordinary operations, so carets and layout
follow. A replica that was away sends `pd_sync_state_vector` and applies the
`pd_sync_diff` it gets back.

Shared: the block tree, text with each character property merged on its
own, inline objects and their pictures, paragraph and block properties,
styles, lists, tracked changes, and comments with replies (a comment's
range is the characters carrying its mark, so it moves with them).
`pd_sync_undo`/`pd_sync_redo` undo this replica's own edits only; with a
`pd_sync` the document's own undo history is not used. Not yet shared:
metadata.

Updates must reach each replica in an order that keeps every update after
the ones it depends on -- what a relay forwarding them in the order received
gives (yrs 0.28 loses an update that arrives before one it depends on).

    # yrs 0.28.0 with Rust >= 1.91 (Ubuntu: apt install rustc-1.91 cargo-1.91)
    git clone --depth 1 --branch v0.28.0 https://github.com/y-crdt/y-crdt build/yrs/src
    (cd build/yrs/src && git apply ../../../tools/yrs-rust-1.91.patch && cargo build --release --locked -p yffi)
    make SYNC=yrs BUILD=build-sync test     # adds pd_sync and test_sync
    python3 tools/test_relay.py             # the relay (aiohttp, PyJWT)
    make pascal-sync                        # two editors sharing a document through it

`tools/yrs-rust-1.91.patch` rewrites the one `if let` guard yrs uses, which
Rust 1.91 does not have yet.

**A plain text** (`include/parade_tsync.h`, `pd_tsync`) is shared the same
way, for a code or text editor: one yrs text, positions in UTF-16 code
units, the same relay and wire format. The host reports what it typed or
deleted (`pd_tsync_insert`/`pd_tsync_delete`) and is told every other change
-- the others', or an undo of its own -- as one replacement between whole
characters, at a position in its text as it has it (yrs's own deltas can
split an emoji in two; these are found by comparing the text the host has
with the shared one). `tests/test_tsync.c` runs three replicas through a
relay-ordered log, with undo, CJK and emoji, at random for 3000 steps
(`SYNC_SEED`, `SYNC_STEPS` to vary it).

**The relay** (`tools/parade_relay.py`) is what editors share a document
through: one append-only log per document, numbered as updates come in and
read in that order by everyone, over HTTP (catch-up is "everything after
the last number I have", live updates a long poll on the same request),
plus presence for carets. Tokens are HS256 JWTs with a user, a document and
a role (viewer, commenter, editor); a viewer cannot write (a commenter is
let write like an editor: updates are opaque to the relay).

    python3 tools/parade_relay.py secret > relay.secret
    python3 tools/parade_relay.py serve --db relay.sqlite --secret-file relay.secret --host 0.0.0.0
    python3 tools/parade_relay.py token --secret-file relay.secret --user ann --doc proposal --role editor

The log is SQLite by default, Postgres with `--db postgresql://...`
(asyncpg). With `--compactor build-sync/pd_compact` the relay compacts it:
once `--compact-every` updates (500) follow a document's snapshot,
`pd_compact` (a small yrs program, built by `make SYNC=yrs`) merges the
snapshot and them into one update, which replaces them in one transaction;
a reader from before the snapshot gets it first, then what follows. Deleted
text is kept in the snapshot, so undo across it still works. A refused merge
leaves the log as it was. `parade_relay.py compact --db ... --compactor ...
--doc ...` does one by hand; `GET /d/<doc>/info` tells how far it got.

**The relay in Pascal** (`pascal/paraderelay.pas`, `TParadeRelay`) does the
same without Python or a database server: SQLite through SQLdb (the same
tables, so either relay serves a file the other wrote), fphttpserver with a
thread per request, the same tokens (HS256 done in the unit), compaction
in-process with `pd_sync_merge`. An editor can run it to host the
documents it shares (led's Host button); `pascal/relay/parade_relay.lpr` is
the command line one, with the Python relay's arguments:

    make pascal-relay           # build-sync/pascal/parade_relay, and the relay tests run against it too
    build-sync/pascal/parade_relay secret > relay.secret
    build-sync/pascal/parade_relay serve --db relay.sqlite --secret-file relay.secret --host 0.0.0.0
    build-sync/pascal/parade_relay token --secret-file relay.secret --user ann --doc proposal
    make pascal-sync-native     # the two-editor test through it
    make pascal-sync-inproc     # ... and with it inside the test program, as an editor hosting runs it

**The editor side** (`pascal/paradesync.pas`, `TParadeSync`): an outbox one
thread sends in order and retries until the relay takes it, kept on disk
too when `OutboxDir` is set (what was typed offline survives quitting: the
next join of that document by that user merges it back and sends it),
another thread long-polls the log,
the main thread merges; the others' carets and selections are drawn in their
colours with their names; Ctrl+Z undoes one's own edits; a viewer's editor
is read-only. led has Share/Join/Host and a status in the visual editor's
toolbar when Parade's yrs library is built; Host runs a `TParadeRelay` in
led itself and shows an invitation link for each role to send to the
others -- `http://host:8765/d/<document>#t=<token>`, everything in one
(`ParadeInviteLink`/`ParadeParseInvite`; the token after the `#` stays out
of a browser's requests, and the relay answers the link with a page saying
how to join). File > Join Shared Document takes the link.

`TParadeSync` runs a session for a `TParadeSyncTarget`: `TParadeRichTarget`
is a `TParadeEdit` with its `pd_sync`; `TParadeTextTarget`
(`pascal/paradetextsync.pas`) is any lines-based editor, which says what its
lines are and how to replace a range (a SynEdit in led; `TParadeStringsTarget`
for a `TStrings`). It finds what was typed by comparing the lines with the
ones it had, so an editor needs no hook beyond "something changed from this
line on". An invitation link for a text says so (`#t=...&k=text`), and
`ParadeRelayKind` asks the relay which kind a document is.

## Lazarus / Free Pascal (`pascal/`)

- `parade.pas`: the binding for all three headers (static link by default,
  `-dPARADE_DYNAMIC` for the shared library). Record layouts are checked
  against the C compiler's on every `make pascal` (about 220 sizes/offsets).
- `paradefonts.pas`: the fonts installed on the system (fontconfig, or the
  font folders read through the files' own name tables), for a font list;
  `TParadeEdit.AddSystemFonts` registers them and loads each face the
  first time text uses it.
- `paradeedit.pas`: `TParadeEdit`, a page-view rich text editor control.
  Glyphs are rasterized by Parade (exact-area coverage, integer only), so
  the screen shows exactly the computed layout on every widgetset. Caret
  and selection are document markers; typing, Enter/Shift+Enter,
  Backspace/Delete across paragraphs, arrows/Home/End/PgUp/PgDn with
  Shift selection, mouse click/drag/double-click, Ctrl+Z/Y/A/B/I/U/C/X/V,
  zoom (Ctrl+wheel), paragraph styles. For a toolbar: font family and
  size (and a step up or down), bold/italic/underline/strike,
  super/subscript, colour, highlight, clear formatting -- on the
  selection, or with none on what is typed next -- alignment, indents (a
  list level in a list), line and paragraph spacing, bullets and
  numbering, the current character/paragraph properties and style, and
  an OnSelectionChange event to show them by; and inserting a table,
  picture, link, page/column break or rule, equation (LaTeX), footnote or
  endnote, or field (page number, pages, date); page setup of the
  caret's section (margins, orientation, paper, columns, a section break,
  header and footer with page-number fields, first page number, line
  numbers); and table editing (rows and columns in or out, merging and
  splitting cells, shading, borders, a repeated header row, even columns);
  references (a table of contents with dot leaders and live page numbers,
  marked by the TOC styles so it can be updated, also after .docx;
  numbered captions; cross-references to a caption's number or page or a
  heading; bookmarks); and for viewing, page-width and whole-page zoom,
  formatting marks, the headings for a navigation list, GoToPos. Copy puts the selection on the
  clipboard as Parade JData, HTML (CF_HTML on Windows), RTF and text;
  paste takes the richest. Load/save by extension: .pdoc/.jdoc, .docx,
  .rtf, .html, .md, .tex (save), .txt, .pdf (save).
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
    make conformance   # Unicode conformance files (fetched once into build/ucd)
    make fuzz-smoke    # every fuzz target: seeds + deterministic mutations under ASan/UBSan
    make fuzz FUZZ=doc FUZZ_TIME=600   # coverage-guided libFuzzer run (clang)

Test fonts: `PARADE_TEST_FONT` (default Liberation Serif) and
`PARADE_TEST_CJK_FONT` (default Noto Sans CJK). Memory caps: `MEMLIMIT_KB`
for test/bench, `ORACLE_MAX_MEM` (MB) for the fontTools worker.

Fuzz targets (`fuzz/`): `font` (loading, outlines, rasterizing, MATH),
`doc` (native load, layout, display lists, PDF, export), `import` (every
importer, paste), `para` (segmentation, bidi, breaking, carets) and `math`
(the TeX parser). `fuzz/driver.c` replays and mutates without libFuzzer,
so any compiler can run them; inputs that once crashed are kept in
`fuzz/seeds/` as regressions, and libFuzzer findings land in
`build/fuzz/crash/`. CI (`.github/workflows/ci.yml`) runs gcc and clang
with `-Werror`, the sanitizers, HarfBuzz, fuzzing, conformance, the
external PDF/converter readers, the Pascal ABI check, formatting and a
macOS build.

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
    src/pd_math.c        OpenType MATH reader and TeX math layout
    src/pd_conv.c        converters: dispatch, builder, clipboard copy/paste, text
    src/pd_markup.c      HTML/XML tokenizer
    src/pd_html.c src/pd_markdown.c src/pd_latex.c src/pd_rtf.c src/pd_docx.c
    src/pd_pptx.c        PowerPoint read: each slide a page that is a canvas (placeholders from the layout
                         and master, theme colours and fonts, pictures, tables, SmartArt drawings), made a
                         .docx in memory and read as one
    src/pd_preset.c      Office's preset shapes worked out (guides, arcs, handles); pd_presets.inc is
                         generated from ECMA-376 presetShapeDefinitions.xml by tools/presets.py
    tests/               unit tests, Unicode conformance
    fuzz/                fuzz targets, stand-in driver, regression seeds
    bench/               quality and speed benchmark
    tools/font_oracle.py fontTools cross-check through the C ABI (ctypes)
    tools/render.py      visual viewer: Parade positions + fontTools outlines -> SVG
    tools/pd_dump.c      builds/loads a document, dumps its pages as JSON
    tools/render_pages.py draws the dumped pages
    proto/               original standalone prototype (kp.c)

## Next

Vectorizing the breaker does not pay: a branchless, batched evaluation of
candidate starts (bit-identical results) was slower with gcc -O2 (245 vs
209 ns/word) and only tied with clang -O3 -march=native (AVX-512, 204 vs
203); the time is in the per-candidate state relaxation, not the arithmetic.
