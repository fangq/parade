{ Parade from Free Pascal: document, layout, JData round trip, glyph rendering }
program api_test;

{$mode objfpc}{$H+}

uses
  SysUtils, Classes, ctypes, parade;

var
  Font: Ppd_font;
  Failures: Integer = 0;
  Checks: Integer = 0;

procedure Check(Cond: Boolean; const What: string);
begin
  Inc(Checks);
  if not Cond then
  begin
    Inc(Failures);
    WriteLn('CHECK failed: ', What);
  end;
end;

function Resolve(user: Pointer; family: PAnsiChar; weight, italic: Int32): Ppd_font; cdecl;
begin
  Result := Font;
end;

function WriteStream(user: Pointer; data: Pointer; len: csize_t): cint; cdecl;
begin
  TMemoryStream(user).WriteBuffer(data^, len);
  Result := 0;
end;

var
  Doc, Doc2: Ppd_doc;
  Layout: Ppd_layout;
  Info: pd_layout_info;
  Sec, P: pd_block_id;
  After: pd_pos;
  S: string;
  Txt: PAnsiChar;
  Len: UInt32;
  Bold: pd_char_props;
  Img: pd_glyph_image;
  Buf: array of Byte;
  Ms: TMemoryStream;
  Items: array of pd_draw;
  N, I, Glyphs: Int32;
  Page, PageOfCaret: Int32;
  X, Base, Asc, Desc: pd_sp;
  Hit: pd_pos;
  Kind: string;
begin
  WriteLn('parade ', pd_version());
  ParadeCheck(pd_font_load_file('/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf', 0, Font), 'font');

  { a document with a heading and body text, edited and undone }
  ParadeCheck(pd_doc_new(Doc), 'doc');
  pd_doc_set_font_resolver(Doc, @Resolve, nil);
  Sec := pd_doc_child(Doc, pd_doc_root(Doc), 0);
  P := pd_doc_child(Doc, Sec, 0);
  S := 'Pascal says hello to Parade. ';
  for I := 1 to 40 do
    ParadeCheck(pd_doc_insert_text(Doc, PdPos(P, 0), PAnsiChar(S), Length(S), PD_FORMAT_INHERIT, nil), 'insert');
  pd_doc_para_text(Doc, P, Txt, Len);
  Check(Len = 40 * Length(S), 'text length');
  FillChar(Bold, SizeOf(Bold), 0);
  Bold.mask := PD_CP_WEIGHT;
  Bold.weight := 700;
  Check(pd_doc_set_char_props(Doc, PdRange(PdPos(P, 0), PdPos(P, 6)), Bold) = PD_OK, 'bold');
  Check(pd_doc_split(Doc, PdPos(P, 29), @After) = PD_OK, 'split');
  Check(After.block <> P, 'new paragraph');
  Check(pd_doc_undo(Doc) = PD_OK, 'undo');
  Check(pd_doc_redo(Doc) = PD_OK, 'redo');

  { layout, display list, caret and hit test }
  ParadeCheck(pd_layout_new(Doc, Layout), 'layout');
  Check(pd_layout_update(Layout, @Info) = PD_OK, 'update');
  Check(Info.pages >= 1, 'pages');
  Check(pd_layout_page_items(Layout, 0, nil, 0, N) = PD_OK, 'size query');
  SetLength(Items, N);
  Check(pd_layout_page_items(Layout, 0, @Items[0], N, N) = PD_OK, 'items');
  Glyphs := 0;
  for I := 0 to N - 1 do
    if Items[I].kind = PD_DRAW_GLYPH then
      Inc(Glyphs);
  Check(Glyphs > 500, 'glyph count');
  Check(pd_layout_caret(Layout, PdPos(After.block, 10), PageOfCaret, X, Base, Asc, Desc) = PD_OK, 'caret');
  Check(pd_layout_hit_test(Layout, PageOfCaret, X + 1, Base - Asc div 2, Hit) = PD_OK, 'hit test');
  Check((Hit.block = After.block) and (Hit.offset = 10), 'caret/hit round trip');

  { JData round trip through a Pascal writer callback }
  for Page := 0 to 1 do
  begin
    Ms := TMemoryStream.Create;
    Check(pd_doc_save(Doc, Page, @WriteStream, Ms) = PD_OK, 'save');
    Check(pd_doc_load(Ms.Memory, Ms.Size, PD_JDATA_AUTO, Doc2) = PD_OK, 'load');
    pd_doc_para_text(Doc2, P, Txt, Len);
    if Page = 0 then Kind := 'JData' else Kind := 'BJData';
    WriteLn('  ', Kind, ': ', Ms.Size, ' bytes, first paragraph ', Len, ' bytes');
    Check(Len = 29, 'loaded split paragraph');
    pd_doc_free(Doc2);
    Ms.Free;
  end;

  { the rasterizer }
  Check(pd_font_glyph_render(Font, pd_font_glyph_index(Font, Ord('g')), PT(48), 0, nil, 0, Img) = PD_OK, 'render size');
  SetLength(Buf, Img.width * Img.height);
  Check(pd_font_glyph_render(Font, pd_font_glyph_index(Font, Ord('g')), PT(48), 0, @Buf[0], Length(Buf), Img) = PD_OK,
    'render');
  Check((Img.width > 10) and (Img.top > 10), 'glyph box');

  pd_layout_free(Layout);
  pd_doc_free(Doc);
  pd_font_free(Font);
  WriteLn(Checks, ' checks, ', Failures, ' failures');
  if Failures > 0 then
    Halt(1);
end.
