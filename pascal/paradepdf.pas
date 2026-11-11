{ Parade - Free Pascal binding for the PDF reader (include/parade_pdfread.h)

  Open a PDF, draw its pages, read their text; annotations, form fields and
  page text changed and saved as an incremental update. Page coordinates
  are points on the page as shown (crop box, /Rotate), top left origin, y
  down. Records use C layout; pascal/tests/pdf_abi_test.pas checks them
  against the C compiler's. Linked as parade.pas links. }

unit paradepdf;

{$mode objfpc}{$H+}
{$PACKRECORDS C}
{$MACRO ON}

interface

uses
  ctypes, parade;

{$IFDEF PARADE_DYNAMIC}
  {$DEFINE PDEXT := external 'parade'}
{$ELSE}
  {$DEFINE PDEXT := external}
{$ENDIF}

type
  Ppd_pdfr = Pointer;

const
  PD_PDFR_BGRA = 1;
  PD_PDFR_NO_ANNOTS = 2;
  PD_PDFR_NO_WHITE = 4;

  PD_PDFR_CHAR_SPACE = 1;
  PD_PDFR_CHAR_NEWLINE = 2;

  PD_PDFR_LINES = 0;
  PD_PDFR_BLOCKS = 1;

  PD_ANNOT_OTHER = 0;
  PD_ANNOT_TEXT = 1;
  PD_ANNOT_LINK = 2;
  PD_ANNOT_FREETEXT = 3;
  PD_ANNOT_LINE = 4;
  PD_ANNOT_SQUARE = 5;
  PD_ANNOT_CIRCLE = 6;
  PD_ANNOT_POLYGON = 7;
  PD_ANNOT_POLYLINE = 8;
  PD_ANNOT_HIGHLIGHT = 9;
  PD_ANNOT_UNDERLINE = 10;
  PD_ANNOT_SQUIGGLY = 11;
  PD_ANNOT_STRIKEOUT = 12;
  PD_ANNOT_STAMP = 13;
  PD_ANNOT_INK = 14;
  PD_ANNOT_WIDGET = 15;
  PD_ANNOT_POPUP = 16;
  PD_ANNOT_ARROW = 17;

  PD_PDF_FIELD_TEXT = 1;
  PD_PDF_FIELD_CHECK = 2;
  PD_PDF_FIELD_RADIO = 3;
  PD_PDF_FIELD_PUSH = 4;
  PD_PDF_FIELD_COMBO = 5;
  PD_PDF_FIELD_LIST = 6;
  PD_PDF_FIELD_SIGNATURE = 7;

type
  pd_pdfr_char = record
    cp: UInt32;
    flags: Int32;
    x0, y0, x1, y1: Single;
    size: Single;
    run: Int32;
  end;
  Ppd_pdfr_char = ^pd_pdfr_char;

  { a stretch of a page's text: characters first..end-1 of its text, and their box }
  pd_pdfr_span = record
    first, end_: Int32;
    x0, y0, x1, y1: Single;
    size: Single;
    lines: Int32;
    align: Int32;     { a block's lines: 0 left, 1 centred, 2 right }
  end;
  Ppd_pdfr_span = ^pd_pdfr_span;
  TPdfSpans = array of pd_pdfr_span;

  pd_pdfr_annot = record
    kind: Int32;
    x0, y0, x1, y1: Double;
    color: UInt32;
    fill: UInt32;
    width: Double;
    hidden: Int32;
    field: Int32;
    contents: array[0..511] of AnsiChar;
    uri: array[0..511] of AnsiChar;
  end;

  pd_pdfr_field = record
    kind: Int32;
    name: array[0..255] of AnsiChar;
    value: array[0..1023] of AnsiChar;
    page: Int32;
    x0, y0, x1, y1: Double;
    readonly, multiline, password, required: Int32;
    nopts: Int32;
    maxlen: Int32;
    font_size: Double;
  end;

  TPdfChars = array of pd_pdfr_char;

  pd_pdfr_writer = function(user: Pointer; data: Pointer; len: csize_t): cint; cdecl;

function pd_pdfr_open_memory(data: Pointer; len: csize_t; password: PAnsiChar; out pdf: Ppd_pdfr): pd_status; cdecl; PDEXT;
function pd_pdfr_open_file(path: PAnsiChar; password: PAnsiChar; out pdf: Ppd_pdfr): pd_status; cdecl; PDEXT;
procedure pd_pdfr_free(pdf: Ppd_pdfr); cdecl; PDEXT;
function pd_pdfr_error(pdf: Ppd_pdfr): PAnsiChar; cdecl; PDEXT;
function pd_pdfr_encrypted(pdf: Ppd_pdfr): Int32; cdecl; PDEXT;
procedure pd_pdfr_add_font_dir(pdf: Ppd_pdfr; dir: PAnsiChar); cdecl; PDEXT;
function pd_pdfr_page_count(pdf: Ppd_pdfr): Int32; cdecl; PDEXT;
function pd_pdfr_page_size(pdf: Ppd_pdfr; page: Int32; out width, height: Double): pd_status; cdecl; PDEXT;
function pd_pdfr_info(pdf: Ppd_pdfr; key: PAnsiChar; buf: PAnsiChar; cap: csize_t): pd_status; cdecl; PDEXT;
function pd_pdfr_render(pdf: Ppd_pdfr; page: Int32; scale: Double; ox, oy: Int32; buf: Pointer;
  width, height, stride, flags: Int32): pd_status; cdecl; PDEXT;
function pd_pdfr_page_text(pdf: Ppd_pdfr; page: Int32; buf: Ppd_pdfr_char; cap: Int32;
  out count: Int32): pd_status; cdecl; PDEXT;

function pd_pdfr_page_spans(pdf: Ppd_pdfr; page, level: Int32; buf: Ppd_pdfr_span; cap: Int32;
  out count: Int32): pd_status; cdecl; PDEXT;

function pd_pdfr_annot_count(pdf: Ppd_pdfr; page: Int32): Int32; cdecl; PDEXT;
function pd_pdfr_annot_get(pdf: Ppd_pdfr; page, index: Int32; out a: pd_pdfr_annot): pd_status; cdecl; PDEXT;
function pd_pdfr_annot_at(pdf: Ppd_pdfr; page: Int32; x, y: Double): Int32; cdecl; PDEXT;
function pd_pdfr_annot_delete(pdf: Ppd_pdfr; page, index: Int32): pd_status; cdecl; PDEXT;
function pd_pdfr_annot_move(pdf: Ppd_pdfr; page, index: Int32; dx, dy: Double): pd_status; cdecl; PDEXT;
function pd_pdfr_annot_set_contents(pdf: Ppd_pdfr; page, index: Int32; utf8: PAnsiChar): pd_status; cdecl; PDEXT;
function pd_pdfr_annot_set_color(pdf: Ppd_pdfr; page, index: Int32; rgb: UInt32): pd_status; cdecl; PDEXT;
function pd_pdfr_add_markup(pdf: Ppd_pdfr; page, kind: Int32; quads: PDouble; nquads: Int32; rgb: UInt32;
  contents: PAnsiChar): Int32; cdecl; PDEXT;
function pd_pdfr_add_ink(pdf: Ppd_pdfr; page: Int32; points: PDouble; counts: PInt32; nstrokes: Int32;
  rgb: UInt32; width: Double): Int32; cdecl; PDEXT;
function pd_pdfr_add_shape(pdf: Ppd_pdfr; page, kind: Int32; x0, y0, x1, y1: Double; rgb, fill: UInt32;
  width: Double): Int32; cdecl; PDEXT;
function pd_pdfr_add_note(pdf: Ppd_pdfr; page: Int32; x, y: Double; utf8: PAnsiChar; rgb: UInt32): Int32; cdecl; PDEXT;
function pd_pdfr_add_text_box(pdf: Ppd_pdfr; page: Int32; x0, y0, x1, y1: Double; utf8: PAnsiChar; size: Double;
  rgb: UInt32): Int32; cdecl; PDEXT;

function pd_pdfr_field_count(pdf: Ppd_pdfr): Int32; cdecl; PDEXT;
function pd_pdfr_field_get(pdf: Ppd_pdfr; index: Int32; out f: pd_pdfr_field): pd_status; cdecl; PDEXT;
function pd_pdfr_field_option(pdf: Ppd_pdfr; index, i: Int32; buf: PAnsiChar; cap: csize_t): pd_status; cdecl; PDEXT;
function pd_pdfr_field_at(pdf: Ppd_pdfr; page: Int32; x, y: Double): Int32; cdecl; PDEXT;
function pd_pdfr_field_set(pdf: Ppd_pdfr; index: Int32; utf8: PAnsiChar): pd_status; cdecl; PDEXT;
function pd_pdfr_widget_state(pdf: Ppd_pdfr; page, annot: Int32; buf: PAnsiChar; cap: csize_t): pd_status; cdecl; PDEXT;

function pd_pdfr_replace_text(pdf: Ppd_pdfr; page, from, upto: Int32; utf8: PAnsiChar): pd_status; cdecl; PDEXT;

function pd_pdfr_can_undo(pdf: Ppd_pdfr): Int32; cdecl; PDEXT;
function pd_pdfr_can_redo(pdf: Ppd_pdfr): Int32; cdecl; PDEXT;
function pd_pdfr_undo(pdf: Ppd_pdfr): pd_status; cdecl; PDEXT;
function pd_pdfr_redo(pdf: Ppd_pdfr): pd_status; cdecl; PDEXT;

function pd_pdfr_modified(pdf: Ppd_pdfr): Int32; cdecl; PDEXT;
function pd_pdfr_changes(pdf: Ppd_pdfr): Int32; cdecl; PDEXT;
function pd_pdfr_save(pdf: Ppd_pdfr; fn: pd_pdfr_writer; user: Pointer): pd_status; cdecl; PDEXT;
function pd_pdfr_save_file(pdf: Ppd_pdfr; path: PAnsiChar): pd_status; cdecl; PDEXT;

{ Pascal helpers }

{ the file as it was with the changes appended, as bytes ('' if it could not be written) }
function PdfSaveToString(pdf: Ppd_pdfr): RawByteString;
{ a page's text as UTF-8, and its characters }
function PdfPageChars(pdf: Ppd_pdfr; page: Integer): TPdfChars;
function PdfCharsText(const C: array of pd_pdfr_char; AFrom, ATo: Integer): UTF8String;
{ a page's lines (PD_PDFR_LINES) or blocks (PD_PDFR_BLOCKS) }
function PdfPageSpans(pdf: Ppd_pdfr; page, level: Integer): TPdfSpans;

implementation

function SaveSink(user: Pointer; data: Pointer; len: csize_t): cint; cdecl;
var
  S: ^RawByteString;
  L: SizeInt;
begin
  S := user;
  L := Length(S^);
  SetLength(S^, L + SizeInt(len));
  if len > 0 then
    Move(data^, S^[L + 1], len);
  Result := 0;
end;

function PdfSaveToString(pdf: Ppd_pdfr): RawByteString;
begin
  Result := '';
  if pd_pdfr_save(pdf, @SaveSink, @Result) <> 0 then
    Result := '';
end;

function PdfPageChars(pdf: Ppd_pdfr; page: Integer): TPdfChars;
var
  N: Int32;
begin
  Result := nil;
  N := 0;
  if pd_pdfr_page_text(pdf, page, nil, 0, N) <> 0 then Exit;
  SetLength(Result, N);
  if N > 0 then
    pd_pdfr_page_text(pdf, page, @Result[0], N, N);
end;

function PdfPageSpans(pdf: Ppd_pdfr; page, level: Integer): TPdfSpans;
var
  N: Int32;
begin
  Result := nil;
  N := 0;
  if pd_pdfr_page_spans(pdf, page, level, nil, 0, N) <> 0 then Exit;
  SetLength(Result, N);
  if N > 0 then
    pd_pdfr_page_spans(pdf, page, level, @Result[0], N, N);
end;

function PdfCharsText(const C: array of pd_pdfr_char; AFrom, ATo: Integer): UTF8String;
var
  i: Integer;
  U: UnicodeString;
  Cp: UInt32;
begin
  U := '';
  if AFrom < 0 then AFrom := 0;
  if ATo > Length(C) then ATo := Length(C);
  for i := AFrom to ATo - 1 do
  begin
    Cp := C[i].cp;
    if Cp >= $10000 then
      U := U + WideChar($D800 + ((Cp - $10000) shr 10)) + WideChar($DC00 + ((Cp - $10000) and $3FF))
    else
      U := U + WideChar(Cp);
  end;
  Result := UTF8Encode(U);
end;

end.
