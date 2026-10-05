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

## Build

    make            # build/libparade.a, build/libparade.so, tests, bench
    make test       # unit tests (run under a 2 GB address-space cap)
    make asan       # tests under AddressSanitizer + UBSan
    make bench      # greedy vs optimal quality and speed (GPL-3 text corpus)
    make oracle     # font reader vs fontTools on every installed font
    make view       # render test scenes to build/parade_view.svg/.png
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

    include/parade.h     public C API
    src/pd_font.c        font metrics reader
    src/pd_para.c        content building, simple shaper, output, hit testing
    src/pd_break.c       total-fit / first-fit line breaking
    tests/               unit tests
    bench/               quality and speed benchmark
    tools/font_oracle.py fontTools cross-check through the C ABI (ctypes)
    tools/render.py      visual viewer: Parade positions + fontTools outlines -> SVG
    proto/               original standalone prototype (kp.c)

## Next

Liang hyphenation patterns, UAX #14/#29 segmentation and grapheme-aware
carets, bidi, a pluggable full shaper (kb_text_shape or HarfBuzz),
paragraph variants (looseness ±1), and the page builder (floats,
headers/footers, incremental repagination), followed by the Free Pascal
binding unit.
