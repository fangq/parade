{ Parade - Free Pascal binding for the Parade text layout core

  Mirrors include/parade.h, include/parade_doc.h and include/parade_layout.h.
  Records use C layout (PACKRECORDS C); every enum-valued field is an
  Int32 on both sides, and tests/abi_test.pas checks sizes and offsets
  against the C compiler's.

  Linking: by default the static library is linked (-Fl<dir of libparade.a>,
  plus libc and libm); define PARADE_DYNAMIC to load libparade.so/.dll. }

unit parade;

{$mode objfpc}{$H+}
{$PACKRECORDS C}
{$MACRO ON}

interface

uses
  ctypes;

{$IFDEF PARADE_DYNAMIC}
  {$DEFINE PDEXT := external 'parade'}
{$ELSE}
  {$LINKLIB parade}
  {$LINKLIB m}
  {$LINKLIB c}
  {$DEFINE PDEXT := external}
{$ENDIF}

type
  pd_sp = Int32;
  pd_status = Int32;
  pd_block_id = UInt32;
  pd_style_id = UInt32;
  pd_format_id = UInt32;
  pd_list_id = UInt32;
  pd_res_id = UInt32;
  pd_marker_id = UInt32;
  pd_rev_id = UInt32;
  pd_comment_id = UInt32;

  Ppd_sp = ^pd_sp;
  Ppd_font = Pointer;
  Ppd_para = Pointer;
  Ppd_doc = Pointer;
  Ppd_layout = Pointer;

const
  PD_SP_PER_PT = 65536;

  PD_OK = 0;
  PD_ERR_ARG = -1;
  PD_ERR_NOMEM = -2;
  PD_ERR_IO = -3;
  PD_ERR_FONT = -4;
  PD_ERR_RANGE = -5;
  PD_ERR_STATE = -6;
  PD_ERR_FORMAT = -7;

  PD_BREAK_OPTIMAL = 0;
  PD_BREAK_GREEDY = 1;

  PD_FAMILY_SERIF = 0;
  PD_FAMILY_SANS = 1;
  PD_FAMILY_MONO = 2;
  PD_FONT_GENERIC_UNKNOWN = 0;
  PD_FONT_GENERIC_ROMAN = 1;
  PD_FONT_GENERIC_SWISS = 2;
  PD_FONT_GENERIC_MODERN = 3;
  PD_FONT_GENERIC_SCRIPT = 4;
  PD_FONT_GENERIC_DECORATIVE = 5;
  PD_FONT_TABLE_MIME = 'text/vnd.parade.font-table';

  PD_ALIGN_JUSTIFY = 0;
  PD_ALIGN_LEFT = 1;
  PD_ALIGN_RIGHT = 2;
  PD_ALIGN_CENTER = 3;

  { pd_glyph_kind; C names PD_GLYPH/PD_OBJECT clash with the pd_glyph type in Pascal }
  PD_KIND_GLYPH = 0;
  PD_KIND_OBJECT = 1;

  PD_FORMAT_INHERIT = $FFFFFFFF;

  PD_BLOCK_ROOT = 0;
  PD_BLOCK_SECTION = 1;
  PD_BLOCK_PARAGRAPH = 2;
  PD_BLOCK_FLOAT = 3;
  PD_BLOCK_TABLE = 4;
  PD_BLOCK_ROW = 5;
  PD_BLOCK_CELL = 6;
  PD_BLOCK_BREAK = 7;
  PD_BLOCK_STORY = 8;

  PD_ROLE_BODY = 0;
  PD_ROLE_TITLE = 1;
  PD_ROLE_HEADING = 2;
  PD_ROLE_CAPTION = 3;
  PD_ROLE_QUOTE = 4;
  PD_ROLE_CODE = 5;
  PD_ROLE_EQUATION = 6;
  PD_ROLE_FIGURE_CONTENT = 7;
  PD_ROLE_RAW = 8;
  PD_ROLE_TERM = 9;
  PD_ROLE_DEFINITION = 10;

  PD_BREAK_PAGE = 0;
  PD_BREAK_COLUMN = 1;
  PD_BREAK_ODD_PAGE = 2;
  PD_BREAK_EVEN_PAGE = 3;
  PD_BREAK_RULE = 4;

  PD_STYLE_PARAGRAPH = 0;
  PD_STYLE_CHARACTER = 1;

  PD_CP_FAMILY = 1 shl 0;
  PD_CP_SIZE = 1 shl 1;
  PD_CP_WEIGHT = 1 shl 2;
  PD_CP_ITALIC = 1 shl 3;
  PD_CP_COLOR = 1 shl 4;
  PD_CP_BACKGROUND = 1 shl 5;
  PD_CP_UNDERLINE = 1 shl 6;
  PD_CP_STRIKE = 1 shl 7;
  PD_CP_SHIFT = 1 shl 8;
  PD_CP_LETTERSPACE = 1 shl 9;
  PD_CP_KERNING = 1 shl 10;
  PD_CP_LANG = 1 shl 11;
  PD_CP_SMALLCAPS = 1 shl 12;
  PD_CP_LINK = 1 shl 13;
  PD_CP_CAPS = 1 shl 14;
  PD_CP_HIDDEN = 1 shl 15;
  PD_CP_POSITION = 1 shl 16;
  PD_CP_REVISION = 1 shl 17;
  PD_CP_FAMILY_EA = 1 shl 18;
  PD_CP_FAMILY_CS = 1 shl 19;
  PD_CP_SIZE_CS = 1 shl 20;
  PD_CP_WEIGHT_CS = 1 shl 21;
  PD_CP_ITALIC_CS = 1 shl 22;
  PD_REV_INSERT = 1;
  PD_REV_DELETE = 2;
  PD_MARKUP_BALLOONS = 0;
  PD_MARKUP_INLINE = 1;
  PD_MARKUP_FINAL = 2;
  PD_MARKUP_ORIGINAL = 3;
  PD_MARK_DELETION = 1;
  PD_MARK_INSERTION = 2;
  PD_MARK_COMMENT = 3;
  PD_UNDERLINE_NONE = 0;
  PD_UNDERLINE_SINGLE = 1;
  PD_UNDERLINE_DOUBLE = 2;
  PD_UNDERLINE_THICK = 3;
  PD_UNDERLINE_DOTTED = 4;
  PD_UNDERLINE_DASHED = 5;
  PD_UNDERLINE_WAVY = 6;
  PD_UNDERLINE_WORDS = 7;

  PD_SHIFT_NONE = 0;
  PD_SHIFT_SUPER = 1;
  PD_SHIFT_SUB = 2;

  PD_PP_ALIGN = 1 shl 0;
  PD_PP_INDENT_LEFT = 1 shl 1;
  PD_PP_INDENT_RIGHT = 1 shl 2;
  PD_PP_INDENT_FIRST = 1 shl 3;
  PD_PP_SPACE_BEFORE = 1 shl 4;
  PD_PP_SPACE_AFTER = 1 shl 5;
  PD_PP_LINE_SPACING = 1 shl 6;
  PD_PP_KEEP_NEXT = 1 shl 7;
  PD_PP_KEEP_LINES = 1 shl 8;
  PD_PP_WIDOWS = 1 shl 9;
  PD_PP_ORPHANS = 1 shl 10;
  PD_PP_BREAK_BEFORE = 1 shl 11;
  PD_PP_HYPHENATE = 1 shl 12;
  PD_PP_BREAK_MODE = 1 shl 13;
  PD_PP_NEXT_STYLE = 1 shl 14;
  PD_PP_BORDER = 1 shl 15;
  PD_PP_SHADING = 1 shl 16;
  PD_PP_DIRECTION = 1 shl 17;

  PD_DIR_AUTO = 0;
  PD_DIR_LTR = 1;
  PD_DIR_RTL = 2;

  PD_NUM_BULLET = 0;
  PD_NUM_DECIMAL = 1;
  PD_NUM_LOWER_ALPHA = 2;
  PD_NUM_UPPER_ALPHA = 3;
  PD_NUM_LOWER_ROMAN = 4;
  PD_NUM_UPPER_ROMAN = 5;
  PD_NUM_NONE = 6;

  PD_INLINE_IMAGE = 0;
  PD_INLINE_EQUATION = 1;
  PD_INLINE_FIELD = 2;
  PD_INLINE_FOOTNOTE = 3;
  PD_INLINE_LINK = 4;
  PD_INLINE_BOOKMARK = 5;
  PD_INLINE_TAB = 6;
  PD_INLINE_USER = 7;
  PD_INLINE_RAW = 8;
  PD_INLINE_RUBY = 10;        { a phonetic guide starts (source: its text; height: its size; depth: its raise) }
  PD_INLINE_CONTROL = 9;      { a content control starts (name: its kind; source: JSON); an empty name ends it }

  PD_FIELD_PAGE = 0;
  PD_FIELD_PAGES = 1;
  PD_FIELD_SECTION_PAGE = 2;
  PD_FIELD_REF_NUMBER = 3;
  PD_FIELD_REF_PAGE = 4;
  PD_FIELD_SEQ = 5;
  PD_FIELD_HEADING = 6;
  PD_FIELD_DATE = 7;

  PD_PLACE_HERE = 1;
  PD_PLACE_TOP = 2;
  PD_PLACE_BOTTOM = 4;
  PD_PLACE_PAGE = 8;
  PD_PLACE_FORCE = 16;
  PD_PLACE_OFFSET = 32;

  PD_WRAP_NONE = 0;
  PD_WRAP_LEFT = 1;
  PD_WRAP_RIGHT = 2;
  PD_PAGES_GREEDY = 0;
  PD_PAGES_OPTIMAL = 1;
  PD_TABLE_MAX_COLS = 32;

  PD_MAX_TABS = 16;
  PD_TAB_LEFT = 0;
  PD_TAB_CENTER = 1;
  PD_TAB_RIGHT = 2;
  PD_TAB_DECIMAL = 3;
  PD_LEADER_NONE = 0;
  PD_LEADER_DOT = 1;
  PD_LEADER_HYPHEN = 2;
  PD_LEADER_UNDERSCORE = 3;
  PD_PP_TABS = 1 shl 18;
  PD_PP_CONTEXTUAL = 1 shl 19;
  PD_PP_SNAP_GRID = 1 shl 20;
  PD_BORDER_TOP = 1;
  PD_BORDER_RIGHT = 2;
  PD_BORDER_BOTTOM = 4;
  PD_BORDER_LEFT = 8;
  PD_BORDER_BETWEEN = 16;
  PD_LINENUM_PAGE = 0;
  PD_LINENUM_SECTION = 1;
  PD_LINENUM_CONTINUOUS = 2;
  PD_TBORDER_TOP = 1;
  PD_TBORDER_RIGHT = 2;
  PD_TBORDER_BOTTOM = 4;
  PD_TBORDER_LEFT = 8;
  PD_TBORDER_INSIDE_H = 16;
  PD_TBORDER_INSIDE_V = 32;

  PD_GRAVITY_LEFT = 0;
  PD_GRAVITY_RIGHT = 1;

  PD_CHANGE_TEXT = 0;
  PD_CHANGE_FORMAT = 1;
  PD_CHANGE_STRUCTURE = 2;
  PD_CHANGE_STYLE = 3;
  PD_CHANGE_SECTION = 4;

  PD_JDATA_AUTO = -1;
  PD_JDATA_TEXT = 0;
  PD_JDATA_BINARY = 1;

  PD_DRAW_GLYPH = 0;
  PD_DRAW_IMAGE = 1;
  PD_DRAW_BOX = 2;
  PD_DRAW_RULE = 3;
  PD_DRAW_LINK = 4;
  PD_DRAW_PATH = 5;
  PD_PATH_CLOSED = 1;
  PD_PATH_BREAK = Low(Int32);

type
  { ---- parade.h ---- }
  pd_font_metrics = record
    units_per_em, ascender, descender, line_gap, x_height, cap_height, num_glyphs, has_kerning: Int32;
  end;
  Ppd_font_metrics = ^pd_font_metrics;

  pd_move_fn = procedure(user: Pointer; x, y: Int32); cdecl;
  pd_quad_fn = procedure(user: Pointer; cx, cy, x, y: Int32); cdecl;
  pd_cubic_fn = procedure(user: Pointer; c1x, c1y, c2x, c2y, x, y: Int32); cdecl;
  pd_close_fn = procedure(user: Pointer); cdecl;

  pd_outline_sink = record
    move_to: pd_move_fn;
    line_to: pd_move_fn;
    quad_to: pd_quad_fn;
    cubic_to: pd_cubic_fn;
    close: pd_close_fn;
  end;
  Ppd_outline_sink = ^pd_outline_sink;

  pd_glyph_image = record
    width, height, left, top: Int32;
  end;
  Ppd_glyph_image = ^pd_glyph_image;

  pd_style = record
    font: Ppd_font;
    size: pd_sp;
    space_stretch, space_shrink, kerning: Int32;
    color: UInt32;
    user: Int32;
    hyph: Pointer;           { const pd_hyph* }
    text_case: Int32;
    letter_space: pd_sp;
    hidden: Int32;
  end;
  Ppd_style = ^pd_style;

  pd_params = record
    mode, align: Int32;
    width, indent: pd_sp;
    line_penalty, adj_demerits, double_hyphen_demerits, final_hyphen_demerits: Int32;
    hyphen_penalty, ex_hyphen_penalty, tex_badness: Int32;
    rag_stretch, baseline_skip: pd_sp;
    line_spacing: Int32;
    hysteresis: Int64;
    freeze_offset: Int32;
    direction: Int32;
    looseness: Int32;
    protrusion, expansion: Int32;
    full_lines: Int32;
    line_grid: pd_sp;
  end;
  Ppd_params = ^pd_params;

  pd_break_info = record
    lines: Int32;
    demerits: Int64;
    overfull, underfull, reused_breakpoints, frozen_lines: Int32;
    height: pd_sp;
  end;
  Ppd_break_info = ^pd_break_info;

  pd_line = record
    text_start, text_end: UInt32;
    x, baseline, width, ascent, descent: pd_sp;
    ratio, badness, hyphenated, overfull, underfull: Int32;
  end;
  Ppd_line = ^pd_line;

  pd_glyph = record
    glyph, cluster: UInt32;
    x, y, advance: pd_sp;
    style, kind, user: Int32;
    scale: Int32;
  end;
  Ppd_glyph = ^pd_glyph;

  { ---- parade_doc.h ---- }
  pd_pos = record
    block: pd_block_id;
    offset: UInt32;
  end;
  Ppd_pos = ^pd_pos;

  pd_range = record
    start, finish: pd_pos;    { C: start, end }
  end;

  pd_font_resolver = function(user: Pointer; family: PAnsiChar; weight, italic: Int32): Ppd_font; cdecl;

  pd_block_info = record
    kind: Int32;
    id, parent: pd_block_id;
    child_count, index, role, level, list_level: Int32;
    style: pd_style_id;
    list: pd_list_id;
    break_kind: Int32;
    text_length: UInt32;
    revision: UInt64;
  end;
  Ppd_block_info = ^pd_block_info;

  pd_char_props = record
    mask: UInt32;
    family: array[0..63] of AnsiChar;
    size: pd_sp;
    weight, italic: Int32;
    color, background: UInt32;
    underline, strike, shift: Int32;
    letter_space: pd_sp;
    kerning: Int32;
    lang: array[0..15] of AnsiChar;
    small_caps: Int32;
    link_target: pd_block_id;
    caps, hidden: Int32;
    position: pd_sp;
    revision: pd_rev_id;
    family_ea: array[0..63] of AnsiChar;   { East Asian text's family; '' = family }
    family_cs: array[0..63] of AnsiChar;   { complex scripts' family; '' = family }
    size_cs: pd_sp;           { 0 = size }
    weight_cs: Int32;         { 0 = weight }
    italic_cs: Int32;         { -1 = italic }
  end;
  Ppd_char_props = ^pd_char_props;

  pd_revision = record
    kind: Int32;
    author: array[0..63] of AnsiChar;
    date: array[0..31] of AnsiChar;
  end;
  Ppd_revision = ^pd_revision;

  pd_comment = record
    author: array[0..63] of AnsiChar;
    date: array[0..31] of AnsiChar;
    text: PAnsiChar;
    text_len: UInt32;
    parent: pd_comment_id;
    resolved: Int32;
    range: pd_range;
  end;
  Ppd_comment = ^pd_comment;

  pd_tab_stop = record
    position: pd_sp;
    align, leader: Int32;     { pd_tab_align, pd_tab_leader }
  end;
  Ppd_tab_stop = ^pd_tab_stop;

  pd_para_props = record
    mask: UInt32;
    align: Int32;
    indent_left, indent_right, indent_first, space_before, space_after: pd_sp;
    line_spacing, keep_with_next, keep_lines, widows, orphans, page_break_before, hyphenate, break_mode: Int32;
    next_style: pd_style_id;
    border_color: UInt32;
    border_width: pd_sp;
    shading: UInt32;
    direction: Int32;
    ntabs: Int32;
    tabs: array[0..PD_MAX_TABS - 1] of pd_tab_stop;
    tab_interval: pd_sp;
    contextual: Int32;
    border_sides: Int32;
    border_space: pd_sp;
    snap_grid: Int32;         { lines on the section's grid (1) }
  end;
  Ppd_para_props = ^pd_para_props;

  pd_list_level = record
    format, start: Int32;
    text: array[0..31] of AnsiChar;
    indent, hanging: pd_sp;
    restart_after: Int32;
    label_family: array[0..31] of AnsiChar;
    label_size: pd_sp;
    label_weight, label_italic: Int32;
    label_color: UInt32;
  end;
  Ppd_list_level = ^pd_list_level;

  pd_run = record
    start, finish: UInt32;    { C: start, end }
    format: pd_format_id;
  end;
  Ppd_run = ^pd_run;

  pd_inline = record
    kind: Int32;
    resource: pd_res_id;
    width, height, depth: pd_sp;
    field: Int32;
    target: pd_block_id;
    level: Int32;
    name: array[0..31] of AnsiChar;
    source: PAnsiChar;
    source_len, user: Int32;
    title: PAnsiChar;         { links and images }
    title_len: Int32;
    alt: PAnsiChar;           { images }
    alt_len: Int32;
  end;
  Ppd_inline = ^pd_inline;

  pd_para_attrs = record
    quote_depth, task, loose: Int32;
    lang: array[0..31] of AnsiChar;
    cont: Int32;              { a later block of the list item above }
    div_class: array[0..31] of AnsiChar;   { a named block it is in: ::: name, <div class> }
  end;
  Ppd_para_attrs = ^pd_para_attrs;

  pd_font_info = record
    alt: array[0..127] of AnsiChar;   { other names, comma-separated }
    generic: Int32;           { PD_FONT_GENERIC_* }
    pitch: Int32;             { 0 not said, 1 fixed, 2 variable }
    panose: array[0..9] of Byte;
    charset: Int32;           { Windows' character set, -1 not said }
  end;

  pd_float_props = record
    placement: UInt32;
    wrap: Int32;
    width: pd_sp;
    width_fraction, span_columns: Int32;
    gap: pd_sp;
    sequence: array[0..31] of AnsiChar;
    offset_x: pd_sp;
  end;
  Ppd_float_props = ^pd_float_props;

  pd_section_props = record
    page_width, page_height: pd_sp;
    margin_top, margin_bottom, margin_left, margin_right: pd_sp;
    header_distance, footer_distance: pd_sp;
    columns: Int32;
    column_gap: pd_sp;
    first_page_number, page_number_format, title_page, facing_pages: Int32;
    header, header_first, header_even: pd_block_id;
    footer, footer_first, footer_even: pd_block_id;
    continuous, page_breaking: Int32;
    footnote_skip: pd_sp;
    add_spacing: Int32;
    line_numbers, line_number_start: Int32;
    line_number_distance: pd_sp;
    line_number_restart: Int32;
    mirror_margins: Int32;
    gutter: pd_sp;
    page_valign: Int32;
    line_pitch: pd_sp;        { the document grid's line pitch; 0 none }
  end;
  Ppd_section_props = ^pd_section_props;

  pd_table_props = record
    width: pd_sp;
    align, header_rows: Int32;
    cell_padding, border: pd_sp;
    border_color: UInt32;
    ncols: Int32;
    col_width: array[0..PD_TABLE_MAX_COLS - 1] of pd_sp;
    indent: pd_sp;
    width_pct, border_sides: Int32;
    cell_padding_v: pd_sp;
  end;
  Ppd_table_props = ^pd_table_props;

  pd_cell_props = record
    col_span, valign: Int32;
    background: UInt32;
    merge_up: Int32;         { continues the cell above (vertical merge) }
    min_height: pd_sp;
    border_set, border_on: Int32;
    border_width: pd_sp;
    border_color: UInt32;
    edge_width: array[0..3] of pd_sp;   { each edge's width (top, right, bottom, left); 0: border_width }
  end;
  Ppd_cell_props = ^pd_cell_props;

  pd_change = record
    kind: Int32;
    block: pd_block_id;
    style: pd_style_id;
    revision: UInt64;
  end;
  Ppd_change = ^pd_change;

  pd_doc_listener = procedure(user: Pointer; change: Ppd_change); cdecl;
  pd_writer = function(user: Pointer; data: Pointer; len: csize_t): cint; cdecl;
  pd_delta_fn = procedure(user: Pointer; json: PAnsiChar; len: csize_t); cdecl;
  { fetches a picture by its address: writes its bytes through write(sink, ...), 0 on success }
  pd_image_fetch = function(user: Pointer; address: PAnsiChar; write: pd_writer; sink: Pointer): cint; cdecl;

  { ---- parade_layout.h ---- }
  pd_layout_info = record
    pages, paragraphs_broken, paragraphs_reused, float_pages, overfull, variants: Int32;
  end;
  Ppd_layout_info = ^pd_layout_info;

  pd_page_info = record
    width, height: pd_sp;
    section: pd_block_id;
    number: Int32;
    label_: array[0..15] of AnsiChar;   { C: label }
    float_page: Int32;
    first, last: pd_pos;
  end;
  Ppd_page_info = ^pd_page_info;

  pd_draw = record
    kind: Int32;
    x, y, w, h: pd_sp;
    glyph: UInt32;
    font: Ppd_font;
    size: pd_sp;
    color: UInt32;
    resource: pd_res_id;
    block: pd_block_id;
    offset: UInt32;
    region: Int32;
    text: UInt32;
    scale: Int32;
    points: Ppd_sp;          { paths: x, y pairs }
    npoints, path_flags: Int32;
    line_width: pd_sp;
    fill: UInt32;
  end;
  Ppd_draw = ^pd_draw;

  pd_markup_item = record
    kind: Int32;
    id: UInt32;
    range: pd_range;
    x, y, top, bottom: pd_sp;
    color: UInt32;
  end;
  Ppd_markup_item = ^pd_markup_item;

  pd_pdf_options = record
    compress, outlines: Int32;
    title: array[0..255] of AnsiChar;
    author: array[0..127] of AnsiChar;
  end;
  Ppd_pdf_options = ^pd_pdf_options;

{ ---- parade.h ---- }
function pd_version: PAnsiChar; cdecl; PDEXT;
function pd_status_string(s: pd_status): PAnsiChar; cdecl; PDEXT;

function pd_font_load_file(path: PAnsiChar; face_index: Int32; out font: Ppd_font): pd_status; cdecl; PDEXT;
function pd_font_load_memory(data: Pointer; len: csize_t; face_index: Int32; out font: Ppd_font): pd_status; cdecl; PDEXT;
procedure pd_font_free(font: Ppd_font); cdecl; PDEXT;
function pd_font_get_metrics(font: Ppd_font; out m: pd_font_metrics): pd_status; cdecl; PDEXT;
function pd_font_glyph_index(font: Ppd_font; codepoint: UInt32): UInt32; cdecl; PDEXT;
function pd_font_family_class(family: PAnsiChar): Int32; cdecl; PDEXT;
function pd_font_glyph_advance(font: Ppd_font; glyph: UInt32): Int32; cdecl; PDEXT;
function pd_font_kerning(font: Ppd_font; left, right: UInt32): Int32; cdecl; PDEXT;
function pd_font_glyph_outline(font: Ppd_font; glyph: UInt32; constref sink: pd_outline_sink; user: Pointer): pd_status; cdecl; PDEXT;
function pd_font_glyph_render(font: Ppd_font; glyph: UInt32; px_per_em: pd_sp; subpixel: Int32; buf: PByte;
  cap: Int32; out info: pd_glyph_image): pd_status; cdecl; PDEXT;

procedure pd_style_init(out style: pd_style; font: Ppd_font; size: pd_sp); cdecl; PDEXT;

function pd_text_next_grapheme(utf8: PAnsiChar; len, offset: csize_t): csize_t; cdecl; PDEXT;
function pd_text_prev_grapheme(utf8: PAnsiChar; len, offset: csize_t): csize_t; cdecl; PDEXT;
function pd_text_line_breaks(utf8: PAnsiChar; len: csize_t; out_: PByte): pd_status; cdecl; PDEXT;

function pd_hyph_load_file(path: PAnsiChar; out hyph: Pointer): pd_status; cdecl; PDEXT;
function pd_hyph_load_memory(data: Pointer; len: csize_t; out hyph: Pointer): pd_status; cdecl; PDEXT;
procedure pd_hyph_free(hyph: Pointer); cdecl; PDEXT;
function pd_hyph_word(hyph: Pointer; utf8: PAnsiChar; len: csize_t; out_: PByte): pd_status; cdecl; PDEXT;
procedure pd_params_init(out params: pd_params); cdecl; PDEXT;

function pd_para_new(out para: Ppd_para): pd_status; cdecl; PDEXT;
procedure pd_para_free(para: Ppd_para); cdecl; PDEXT;
procedure pd_para_clear(para: Ppd_para); cdecl; PDEXT;
function pd_para_add_text(para: Ppd_para; utf8: PAnsiChar; len: csize_t; constref style: pd_style): pd_status; cdecl; PDEXT;
function pd_para_add_object(para: Ppd_para; width, height, depth: pd_sp; user: Int32): pd_status; cdecl; PDEXT;
function pd_para_add_glue(para: Ppd_para; width, stretch, shrink: pd_sp; stretch_fil: Int32): pd_status; cdecl; PDEXT;
function pd_para_add_penalty(para: Ppd_para; penalty: Int32; width: pd_sp; flagged: Int32): pd_status; cdecl; PDEXT;
function pd_para_set_shape(para: Ppd_para; n: Int32; indent, width: Ppd_sp): pd_status; cdecl; PDEXT;
function pd_para_set_tabs(para: Ppd_para; n: Int32; stops: Ppd_tab_stop; interval, origin: pd_sp): pd_status; cdecl;
  PDEXT;
function pd_para_break(para: Ppd_para; params: Ppd_params; info: Ppd_break_info): pd_status; cdecl; PDEXT;
function pd_para_line_count(para: Ppd_para): Int32; cdecl; PDEXT;
function pd_para_get_line(para: Ppd_para; index: Int32; out line: pd_line): pd_status; cdecl; PDEXT;
function pd_para_get_glyphs(para: Ppd_para; line: Int32; buf: Ppd_glyph; cap: Int32; out count: Int32): pd_status; cdecl; PDEXT;
function pd_para_get_style(para: Ppd_para; index: Int32; out style: pd_style): pd_status; cdecl; PDEXT;
function pd_para_hit_test(para: Ppd_para; x, y: pd_sp; out offset: UInt32; line: PInt32): pd_status; cdecl; PDEXT;
function pd_para_caret(para: Ppd_para; offset: UInt32; line: PInt32; out x: pd_sp; baseline: Ppd_sp): pd_status; cdecl; PDEXT;
function pd_para_text_length(para: Ppd_para): UInt32; cdecl; PDEXT;

{ ---- parade_doc.h ---- }
function pd_doc_new(out doc: Ppd_doc): pd_status; cdecl; PDEXT;
procedure pd_doc_free(doc: Ppd_doc); cdecl; PDEXT;
procedure pd_doc_set_font_resolver(doc: Ppd_doc; fn: pd_font_resolver; user: Pointer); cdecl; PDEXT;
procedure pd_doc_set_default_font(doc: Ppd_doc; font: Ppd_font); cdecl; PDEXT;
function pd_doc_set_fallback_fonts(doc: Ppd_doc; fonts: PPointer; n: Int32): pd_status; cdecl; PDEXT;
function pd_doc_set_hyphenator(doc: Ppd_doc; lang: PAnsiChar; hyph: Pointer): pd_status; cdecl; PDEXT;
function pd_doc_set_microtype(doc: Ppd_doc; protrusion, expansion: Int32): pd_status; cdecl; PDEXT;
procedure pd_doc_set_stable_breaks(doc: Ppd_doc; on_: Int32); cdecl; PDEXT;
function pd_doc_stable_breaks(doc: Ppd_doc): Int32; cdecl; PDEXT;
procedure pd_doc_set_math_font(doc: Ppd_doc; font: Ppd_font); cdecl; PDEXT;
function pd_doc_revision(doc: Ppd_doc): UInt64; cdecl; PDEXT;

function pd_doc_root(doc: Ppd_doc): pd_block_id; cdecl; PDEXT;
function pd_doc_story_count(doc: Ppd_doc): Int32; cdecl; PDEXT;
function pd_doc_story_at(doc: Ppd_doc; index: Int32): pd_block_id; cdecl; PDEXT;
function pd_doc_block_info(doc: Ppd_doc; block: pd_block_id; out info: pd_block_info): pd_status; cdecl; PDEXT;
function pd_doc_child(doc: Ppd_doc; parent: pd_block_id; index: Int32): pd_block_id; cdecl; PDEXT;
function pd_doc_next_paragraph(doc: Ppd_doc; block: pd_block_id): pd_block_id; cdecl; PDEXT;
function pd_doc_prev_paragraph(doc: Ppd_doc; block: pd_block_id): pd_block_id; cdecl; PDEXT;

function pd_doc_style_define(doc: Ppd_doc; name: PAnsiChar; kind: Int32; parent: pd_style_id; para: Ppd_para_props;
  chr: Ppd_char_props; out_style: PUInt32): pd_status; cdecl; PDEXT;
function pd_doc_style_find(doc: Ppd_doc; name: PAnsiChar): pd_style_id; cdecl; PDEXT;
function pd_doc_style_resolve(doc: Ppd_doc; style: pd_style_id; para: Ppd_para_props; chr: Ppd_char_props): pd_status; cdecl; PDEXT;
function pd_doc_style_count(doc: Ppd_doc): Int32; cdecl; PDEXT;
function pd_doc_style_at(doc: Ppd_doc; index: Int32): pd_style_id; cdecl; PDEXT;
function pd_doc_style_name(doc: Ppd_doc; style: pd_style_id): PAnsiChar; cdecl; PDEXT;
function pd_doc_format(doc: Ppd_doc; char_style: pd_style_id; overrides: Ppd_char_props): pd_format_id; cdecl; PDEXT;
function pd_doc_format_resolve(doc: Ppd_doc; paragraph: pd_block_id; format: pd_format_id; out props: pd_char_props): pd_status; cdecl; PDEXT;

function pd_doc_list_define(doc: Ppd_doc; nlevels: Int32; levels: Ppd_list_level; out list: pd_list_id): pd_status; cdecl; PDEXT;
function pd_doc_list_label(doc: Ppd_doc; paragraph: pd_block_id; buf: PAnsiChar; cap: Int32): pd_status; cdecl; PDEXT;

function pd_doc_para_text(doc: Ppd_doc; paragraph: pd_block_id; out utf8: PAnsiChar; out len: UInt32): pd_status; cdecl; PDEXT;
function pd_doc_para_runs(doc: Ppd_doc; paragraph: pd_block_id; buf: Ppd_run; cap: Int32; out count: Int32): pd_status; cdecl; PDEXT;
function pd_doc_inline_at(doc: Ppd_doc; pos: pd_pos; out obj: pd_inline): pd_status; cdecl; PDEXT;
function pd_doc_control_at(doc: Ppd_doc; pos: pd_pos; out start, finish: pd_pos): pd_status; cdecl; PDEXT;
function pd_doc_add_resource(doc: Ppd_doc; mime: PAnsiChar; data: Pointer; len: csize_t; out res: pd_res_id): pd_status; cdecl; PDEXT;
function pd_doc_resource(doc: Ppd_doc; res: pd_res_id; mime: PPAnsiChar; data: PPointer; len: pcsize_t): pd_status; cdecl; PDEXT;

function pd_doc_float_props(doc: Ppd_doc; flt: pd_block_id; out props: pd_float_props): pd_status; cdecl; PDEXT;
function pd_doc_section_props(doc: Ppd_doc; section: pd_block_id; out props: pd_section_props): pd_status; cdecl; PDEXT;
procedure pd_section_props_init(out props: pd_section_props); cdecl; PDEXT;
function pd_doc_table_props(doc: Ppd_doc; table: pd_block_id; out props: pd_table_props): pd_status; cdecl; PDEXT;
function pd_doc_cell_props(doc: Ppd_doc; cell: pd_block_id; out props: pd_cell_props): pd_status; cdecl; PDEXT;
procedure pd_table_props_init(out props: pd_table_props); cdecl; PDEXT;

function pd_doc_insert_text(doc: Ppd_doc; at: pd_pos; utf8: PAnsiChar; len: csize_t; format: pd_format_id; after: Ppd_pos): pd_status; cdecl; PDEXT;
function pd_doc_delete(doc: Ppd_doc; range: pd_range; after: Ppd_pos): pd_status; cdecl; PDEXT;
function pd_doc_split(doc: Ppd_doc; at: pd_pos; after: Ppd_pos): pd_status; cdecl; PDEXT;
function pd_doc_insert_inline(doc: Ppd_doc; at: pd_pos; constref obj: pd_inline; after: Ppd_pos): pd_status; cdecl; PDEXT;
function pd_doc_set_char_props(doc: Ppd_doc; range: pd_range; constref props: pd_char_props): pd_status; cdecl; PDEXT;
function pd_doc_clear_char_props(doc: Ppd_doc; range: pd_range; mask: UInt32): pd_status; cdecl; PDEXT;
function pd_doc_set_char_style(doc: Ppd_doc; range: pd_range; style: pd_style_id): pd_status; cdecl; PDEXT;
function pd_doc_set_para_style(doc: Ppd_doc; paragraph: pd_block_id; style: pd_style_id): pd_status; cdecl; PDEXT;
function pd_doc_set_para_props(doc: Ppd_doc; paragraph: pd_block_id; constref props: pd_para_props): pd_status; cdecl; PDEXT;
function pd_doc_set_role(doc: Ppd_doc; paragraph: pd_block_id; role, level: Int32): pd_status; cdecl; PDEXT;
function pd_doc_para_attrs(doc: Ppd_doc; paragraph: pd_block_id; out attrs: pd_para_attrs): pd_status; cdecl; PDEXT;
function pd_doc_set_para_attrs(doc: Ppd_doc; paragraph: pd_block_id; constref attrs: pd_para_attrs): pd_status; cdecl;
  PDEXT;
function pd_doc_set_metadata(doc: Ppd_doc; text: PAnsiChar; len: csize_t): pd_status; cdecl; PDEXT;
function pd_doc_metadata(doc: Ppd_doc; out len: csize_t): PAnsiChar; cdecl; PDEXT;
function pd_doc_font_info(doc: Ppd_doc; family: PAnsiChar; out info: pd_font_info): pd_status; cdecl; PDEXT;
function pd_doc_font_class(doc: Ppd_doc; family: PAnsiChar): Int32; cdecl; PDEXT;
procedure pd_doc_image_display_size(doc: Ppd_doc; constref image: pd_inline; out width, height: pd_sp); cdecl; PDEXT;
function pd_doc_set_list(doc: Ppd_doc; paragraph: pd_block_id; list: pd_list_id; level: Int32): pd_status; cdecl; PDEXT;
function pd_doc_insert_block(doc: Ppd_doc; parent: pd_block_id; index, kind: Int32; out block: pd_block_id): pd_status; cdecl; PDEXT;
function pd_doc_remove_block(doc: Ppd_doc; block: pd_block_id): pd_status; cdecl; PDEXT;
function pd_doc_move_block(doc: Ppd_doc; block, new_parent: pd_block_id; index: Int32): pd_status; cdecl; PDEXT;
function pd_doc_set_float_props(doc: Ppd_doc; flt: pd_block_id; constref props: pd_float_props): pd_status; cdecl; PDEXT;
function pd_doc_set_section_props(doc: Ppd_doc; section: pd_block_id; constref props: pd_section_props): pd_status; cdecl; PDEXT;
function pd_doc_set_table_props(doc: Ppd_doc; table: pd_block_id; constref props: pd_table_props): pd_status; cdecl; PDEXT;
function pd_doc_set_cell_props(doc: Ppd_doc; cell: pd_block_id; constref props: pd_cell_props): pd_status; cdecl; PDEXT;
function pd_doc_set_break(doc: Ppd_doc; brk: pd_block_id; kind: Int32): pd_status; cdecl; PDEXT;

procedure pd_doc_begin_group(doc: Ppd_doc; label_: PAnsiChar); cdecl; PDEXT;
procedure pd_doc_end_group(doc: Ppd_doc); cdecl; PDEXT;
procedure pd_doc_seal_undo(doc: Ppd_doc); cdecl; PDEXT;
function pd_doc_undo(doc: Ppd_doc): pd_status; cdecl; PDEXT;
function pd_doc_redo(doc: Ppd_doc): pd_status; cdecl; PDEXT;
function pd_doc_can_undo(doc: Ppd_doc): Int32; cdecl; PDEXT;
function pd_doc_can_redo(doc: Ppd_doc): Int32; cdecl; PDEXT;
function pd_doc_undo_label(doc: Ppd_doc): PAnsiChar; cdecl; PDEXT;
procedure pd_doc_set_undo_limit(doc: Ppd_doc; steps: Int32); cdecl; PDEXT;
procedure pd_doc_clear_undo(doc: Ppd_doc); cdecl; PDEXT;
function pd_doc_format_info(doc: Ppd_doc; format: pd_format_id; char_style: PUInt32; overrides: Ppd_char_props): pd_status; cdecl; PDEXT;
function pd_doc_style_info(doc: Ppd_doc; style: pd_style_id; kind: PInt32; parent: PUInt32; para: Ppd_para_props;
  chr: Ppd_char_props): pd_status; cdecl; PDEXT;
function pd_doc_para_props(doc: Ppd_doc; paragraph: pd_block_id; out props: pd_para_props): pd_status; cdecl; PDEXT;
function pd_doc_list_count(doc: Ppd_doc): Int32; cdecl; PDEXT;
function pd_doc_list_info(doc: Ppd_doc; list: pd_list_id; nlevels: PInt32; levels: Ppd_list_level): pd_status; cdecl; PDEXT;

function pd_doc_marker_new(doc: Ppd_doc; pos: pd_pos; gravity: Int32; out marker: pd_marker_id): pd_status; cdecl; PDEXT;
procedure pd_doc_marker_free(doc: Ppd_doc; marker: pd_marker_id); cdecl; PDEXT;
function pd_doc_marker_get(doc: Ppd_doc; marker: pd_marker_id; out pos: pd_pos): pd_status; cdecl; PDEXT;
function pd_doc_marker_set(doc: Ppd_doc; marker: pd_marker_id; pos: pd_pos): pd_status; cdecl; PDEXT;

function pd_doc_revision_add(doc: Ppd_doc; constref rev: pd_revision; out id: pd_rev_id): pd_status; cdecl; PDEXT;
function pd_doc_revision_get(doc: Ppd_doc; rev: pd_rev_id; out info: pd_revision): pd_status; cdecl; PDEXT;
function pd_doc_revision_count(doc: Ppd_doc): Int32; cdecl; PDEXT;
function pd_doc_author_index(doc: Ppd_doc; author: PAnsiChar): Int32; cdecl; PDEXT;
function pd_doc_author_color(doc: Ppd_doc; author: PAnsiChar): UInt32; cdecl; PDEXT;
function pd_doc_set_tracking(doc: Ppd_doc; author: PAnsiChar): pd_status; cdecl; PDEXT;
function pd_doc_tracking(doc: Ppd_doc): PAnsiChar; cdecl; PDEXT;
procedure pd_doc_set_markup(doc: Ppd_doc; mode: Int32); cdecl; PDEXT;
function pd_doc_markup(doc: Ppd_doc): Int32; cdecl; PDEXT;
function pd_doc_revision_resolve(doc: Ppd_doc; range: pd_range; accept: Int32): pd_status; cdecl; PDEXT;
function pd_doc_revision_find(doc: Ppd_doc; from: pd_pos; dir: Int32; out range: pd_range; rev: PUInt32): pd_status;
  cdecl; PDEXT;
function pd_doc_comment_add(doc: Ppd_doc; constref comment: pd_comment; out id: pd_comment_id): pd_status; cdecl; PDEXT;
function pd_doc_comment_get(doc: Ppd_doc; id: pd_comment_id; out comment: pd_comment): pd_status; cdecl; PDEXT;
function pd_doc_comment_set(doc: Ppd_doc; id: pd_comment_id; constref comment: pd_comment): pd_status; cdecl; PDEXT;
function pd_doc_comment_remove(doc: Ppd_doc; id: pd_comment_id): pd_status; cdecl; PDEXT;
function pd_doc_comment_count(doc: Ppd_doc): Int32; cdecl; PDEXT;

procedure pd_doc_set_listener(doc: Ppd_doc; fn: pd_doc_listener; user: Pointer); cdecl; PDEXT;
function pd_doc_save(doc: Ppd_doc; format: Int32; fn: pd_writer; user: Pointer): pd_status; cdecl; PDEXT;
function pd_doc_load_images(doc: Ppd_doc; fetch: pd_image_fetch; user: Pointer): Int32; cdecl; PDEXT;
function pd_doc_load(data: Pointer; len: csize_t; format: Int32; out doc: Ppd_doc): pd_status; cdecl; PDEXT;
function pd_doc_snapshot(doc: Ppd_doc; format: Int32; fn: pd_writer; user: Pointer; delta: pd_delta_fn;
  delta_user: Pointer): pd_status; cdecl; PDEXT;
function pd_doc_apply_delta(doc: Ppd_doc; json: PAnsiChar; len: csize_t): pd_status; cdecl; PDEXT;
function pd_doc_para_build(doc: Ppd_doc; paragraph: pd_block_id; column_width: pd_sp; para: Ppd_para;
  params: Ppd_params): pd_status; cdecl; PDEXT;

{ ---- parade_layout.h ---- }
function pd_layout_new(doc: Ppd_doc; out layout: Ppd_layout): pd_status; cdecl; PDEXT;
procedure pd_layout_free(layout: Ppd_layout); cdecl; PDEXT;
function pd_layout_update(layout: Ppd_layout; info: Ppd_layout_info): pd_status; cdecl; PDEXT;
procedure pd_layout_invalidate(layout: Ppd_layout); cdecl; PDEXT;
procedure pd_layout_set_stable_breaks(layout: Ppd_layout; mode: Int32); cdecl; PDEXT;
function pd_layout_page_count(layout: Ppd_layout): Int32; cdecl; PDEXT;
function pd_layout_page_info(layout: Ppd_layout; page: Int32; out info: pd_page_info): pd_status; cdecl; PDEXT;
function pd_layout_page_items(layout: Ppd_layout; page: Int32; buf: Ppd_draw; cap: Int32; out count: Int32): pd_status; cdecl; PDEXT;
function pd_layout_page_markup(layout: Ppd_layout; page: Int32; buf: Ppd_markup_item; cap: Int32;
  out count: Int32): pd_status; cdecl; PDEXT;
function pd_layout_hit_test(layout: Ppd_layout; page: Int32; x, y: pd_sp; out pos: pd_pos): pd_status; cdecl; PDEXT;
function pd_layout_caret(layout: Ppd_layout; pos: pd_pos; out page: Int32; out x, baseline, ascent, descent: pd_sp): pd_status; cdecl; PDEXT;

procedure pd_pdf_options_init(out options: pd_pdf_options); cdecl; PDEXT;
function pd_layout_write_pdf(layout: Ppd_layout; options: Ppd_pdf_options; fn: pd_writer; user: Pointer): pd_status; cdecl; PDEXT;

{ ---- parade_convert.h ---- }
const
  PD_CONV_TEXT = 0;
  PD_CONV_HTML = 1;
  PD_CONV_MARKDOWN = 2;
  PD_CONV_LATEX = 3;          { export only }
  PD_CONV_RTF = 4;
  PD_CONV_DOCX = 5;
  PD_CONV_JDATA = 6;

function pd_doc_export(doc: Ppd_doc; format: Int32; fn: pd_writer; user: Pointer): pd_status; cdecl; PDEXT;
function pd_doc_export_range(doc: Ppd_doc; range: pd_range; format: Int32; fn: pd_writer; user: Pointer): pd_status; cdecl; PDEXT;
function pd_doc_import(data: Pointer; len: csize_t; format: Int32; out doc: Ppd_doc): pd_status; cdecl; PDEXT;
function pd_doc_paste(doc: Ppd_doc; at: pd_pos; data: Pointer; len: csize_t; format: Int32; after: Ppd_pos): pd_status; cdecl; PDEXT;
function pd_conv_detect(data: Pointer; len: csize_t): Int32; cdecl; PDEXT;
{ Office's preset shapes: how many, each one's name; one at a size with its adjustments as JSON (its adjustments,
  handles and paths), a handle dragged to a point as the adjustments that put it there -- each written into buf,
  the length it needs returned }
function pd_preset_count: Int32; cdecl; PDEXT;
function pd_preset_name(i: Int32): PAnsiChar; cdecl; PDEXT;
function pd_preset_json(name: PAnsiChar; w, h: Double; adj: PAnsiChar; buf: PAnsiChar; cap: csize_t): csize_t;
  cdecl; PDEXT;
function pd_preset_drag(name: PAnsiChar; w, h: Double; adj: PAnsiChar; handle: Int32; u, v: Double; buf: PAnsiChar;
  cap: csize_t): csize_t; cdecl; PDEXT;
function pd_docx_drawing_rebuild(doc: Ppd_doc; drawing: pd_res_id; xml: PAnsiChar; len: csize_t;
  out res: pd_res_id): pd_status; cdecl; PDEXT;

{ helpers }
function PT(v: Double): pd_sp; inline;
function PdPos(block: pd_block_id; offset: UInt32): pd_pos; inline;
function PdRange(const a, b: pd_pos): pd_range; inline;
function ParadeCheck(s: pd_status; const what: string): pd_status;

implementation

uses
  SysUtils;

function PT(v: Double): pd_sp; inline;
begin
  Result := Round(v * PD_SP_PER_PT);
end;

function PdPos(block: pd_block_id; offset: UInt32): pd_pos; inline;
begin
  Result.block := block;
  Result.offset := offset;
end;

function PdRange(const a, b: pd_pos): pd_range; inline;
begin
  Result.start := a;
  Result.finish := b;
end;

function ParadeCheck(s: pd_status; const what: string): pd_status;
begin
  if s <> PD_OK then
    raise Exception.CreateFmt('%s: %s', [what, pd_status_string(s)]);
  Result := s;
end;

end.
