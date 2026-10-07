{ Headless test of TParadeEdit: drives the editor like a user and renders pages.
  Run under a display (xvfb-run). Writes PNGs next to the executable. }
program edit_test;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads,{$ENDIF}
  Interfaces, Forms, Controls, Graphics, LCLType, SysUtils, Classes, StrUtils, IntfGraphics, FPImage, parade, paradeedit,
  paradefonts;

type
  TCounter = class
    N: Integer;
    procedure Hit(Sender: TObject);
  end;

procedure TCounter.Hit(Sender: TObject);
begin
  Inc(N);
end;

var
  Failures: Integer = 0;
  Checks: Integer = 0;

procedure Step(const S: string);
begin
  WriteLn(StdErr, '- ', S);
  Flush(StdErr);
end;

procedure Check(Cond: Boolean; const What: string);
begin
  Inc(Checks);
  if not Cond then
  begin
    Inc(Failures);
    WriteLn('CHECK failed: ', What);
  end;
end;

{ a page with text on it has dark pixels; a blank page passes every other check }
function DarkPixels(Bmp: TBitmap): Integer;
var
  Img: TLazIntfImage;
  X, Y: Integer;
begin
  Result := 0;
  Img := Bmp.CreateIntfImage;
  try
    for Y := 0 to Img.Height - 1 do
      for X := 0 to Img.Width - 1 do
        if Img.Colors[X, Y].green < $8000 then
          Inc(Result);
  finally
    Img.Free;
  end;
end;

procedure SavePage(E: TParadeEdit; Page: Integer; const FileName: string; Scale: Double);
var
  Bmp: TBitmap;
  Png: TPortableNetworkGraphic;
begin
  Bmp := TBitmap.Create;
  Png := TPortableNetworkGraphic.Create;
  try
    E.RenderPage(Page, Bmp, Scale);
    Check(DarkPixels(Bmp) > 1000, 'ink on ' + ExtractFileName(FileName));
    Png.Assign(Bmp);
    Png.SaveToFile(FileName);
  finally
    Png.Free;
    Bmp.Free;
  end;
end;

var
  Form: TForm;
  E: TParadeEdit;
  Dir, Sample, T: string;
  C: pd_pos;
  Props: pd_char_props;
  Info: pd_block_info;
  I: Integer;
  N: Int32;
  Runs: array of pd_run;
  Ms: TMemoryStream;
  Items: array of pd_draw;
  Pic: Integer;
  Bmp: TBitmap;
  PX: TColor;
  Cm: pd_comment;
  Pp: pd_para_props;
  Obj: pd_inline;
  Tb, Rw: pd_block_info;
  Pages: Integer;
  Sp: pd_section_props;
  Tp: pd_table_props;
  Cp: pd_cell_props;
  Cell, Row, Tbl: pd_block_id;
  Refs: TParadeRefTargets;
  Heads: TStringList;
  Z: Double;
  Sys, Scanned, One: TParadeSystemFaces;
  Before, Added, K: Integer;
  T0: QWord;
  Fam: string;
  Lst: TStringList;
  Lbl: array[0..63] of AnsiChar;
  Counter: TCounter;
{ how many blocks of a kind the document has, every story's included }
function CountKind(Doc: Ppd_doc; Kind: Integer): Integer;

  function Walk(B: pd_block_id): Integer;
  var
    Info: pd_block_info;
    I: Integer;
  begin
    Result := 0;
    if pd_doc_block_info(Doc, B, Info) <> PD_OK then
      Exit;
    if Info.kind = Kind then
      Inc(Result);
    for I := 0 to Info.child_count - 1 do
      Inc(Result, Walk(pd_doc_child(Doc, B, I)));
  end;

var
  I: Integer;
begin
  Result := Walk(pd_doc_root(Doc));
  for I := 0 to pd_doc_story_count(Doc) - 1 do
    Inc(Result, Walk(pd_doc_story_at(Doc, I)));
end;

{ how many paragraphs of the main text have the style }
function CountStyle(E: TParadeEdit; const Name: string): Integer;
var
  B: pd_block_id;
  Info: pd_block_info;
begin
  Result := 0;
  B := pd_doc_next_paragraph(E.Doc, 0);
  while B <> 0 do
  begin
    if (pd_doc_block_info(E.Doc, B, Info) = PD_OK) and (Info.style <> 0) and
       (string(pd_doc_style_name(E.Doc, Info.style)) = Name) then
      Inc(Result);
    B := pd_doc_next_paragraph(E.Doc, B);
  end;
end;

function ChildCountOf(Doc: Ppd_doc; B: pd_block_id): Integer;
var
  Info: pd_block_info;
begin
  Result := -1;
  if pd_doc_block_info(Doc, B, Info) = PD_OK then
    Result := Info.child_count;
end;

procedure Fail(E: Exception);
begin
  WriteLn(StdErr, 'exception: ', E.ClassName, ': ', E.Message);
  DumpExceptionBacktrace(StdErr);
  Halt(2);
end;

begin
  Step('init');
  Application.Initialize;
  try
  Dir := ExtractFilePath(ParamStr(0));
  Sample := ParamStr(1);
  Form := TForm.CreateNew(nil);
  Form.SetBounds(0, 0, 900, 700);
  E := TParadeEdit.Create(Form);
  E.Parent := Form;
  E.Align := alClient;
  Step('fonts');
  E.AddDefaultFonts;
  Step('show');
  Form.Show;
  Application.ProcessMessages;
  Step('shown');

  { 1. an existing document renders }
  if FileExists(Sample) then
  begin
    Step('load');
    E.LoadFromFile(Sample);
    Step('loaded');
    Check(E.PageCount >= 3, 'sample pages');
    SavePage(E, 0, Dir + 'edit_sample_p1.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
    SavePage(E, E.PageCount - 1, Dir + 'edit_sample_last.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  end;

  Step('editing');
  E.NewDocument;
  E.InsertText('Hello');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('World');
  Check(E.DocumentText = 'Hello'#10'World', 'typed: ' + E.DocumentText);
  E.ProcessKey(VK_BACK, []);
  Check(E.DocumentText = 'Hello'#10'Worl', 'backspace');
  E.ProcessKey(VK_Z, [ssCtrl]);
  Check(E.DocumentText = 'Hello'#10'World', 'undo backspace: ' + E.DocumentText);
  E.ProcessKey(VK_Y, [ssCtrl]);
  Check(E.DocumentText = 'Hello'#10'Worl', 'redo');

  { 3. Backspace at a paragraph start merges }
  E.ProcessKey(VK_HOME, []);
  C := E.CaretPos;
  Check(C.offset = 0, 'home');
  E.ProcessKey(VK_BACK, []);
  Check(E.DocumentText = 'HelloWorl', 'merge: ' + E.DocumentText);
  Check(E.CaretPos.offset = 5, 'caret after merge');

  { 4. selection with shift+arrows and bold }
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 5 do
    E.ProcessKey(VK_RIGHT, [ssShift]);
  Check(E.SelectedText = 'Hello', 'selection: ' + E.SelectedText);
  E.ProcessKey(VK_B, [ssCtrl]);
  pd_doc_format_resolve(E.Doc, E.CaretPos.block, 0, Props);
  E.InsertText('Bold ');      { replaces the selection and keeps its (bold) format }
  Check(Copy(E.DocumentText, 1, 9) = 'Bold Worl', 'replace selection: ' + E.DocumentText);
  { the replacement text is bold: the first run resolves to weight 700 }
  N := 0;
  pd_doc_para_runs(E.Doc, E.CaretPos.block, nil, 0, N);
  SetLength(Runs, N);
  if N > 0 then
    pd_doc_para_runs(E.Doc, E.CaretPos.block, @Runs[0], N, N);
  Check((N >= 2) and (Runs[0].finish = 5), 'bold run covers "Bold "');
  if N > 0 then
    pd_doc_format_resolve(E.Doc, E.CaretPos.block, Runs[0].format, Props);
  Check(Props.weight = 700, 'replacement keeps bold');

  { 5. multi-line paste becomes paragraphs; one undo removes it }
  E.ProcessKey(VK_END, [ssCtrl]);
  E.InsertText(#10'Line one'#10'Line two'#10'Line three');
  pd_doc_block_info(E.Doc, E.CaretPos.block, Info);
  Check(E.DocumentText = 'Bold Worl'#10'Line one'#10'Line two'#10'Line three', 'paste: ' + E.DocumentText);
  E.Undo;
  Check(E.DocumentText = 'Bold Worl', 'undo paste: ' + E.DocumentText);
  E.Redo;

  { 5b. rich paste: HTML at the caret, one undo step }
  Step('rich paste');
  E.ProcessKey(VK_END, [ssCtrl]);
  T := '<p>pasted <b>strong</b></p><h2>Head</h2>';
  E.PasteData(PAnsiChar(T), Length(T), PD_CONV_HTML);
  Check(Pos('pasted strong'#10'Head', E.DocumentText) > 0, 'html paste: ' + E.DocumentText);
  E.Undo;
  Check(E.DocumentText = 'Bold Worl'#10'Line one'#10'Line two'#10'Line three', 'undo html paste: ' + E.DocumentText);

  { 5c. copy and paste through the clipboard keeps formatting (Parade's own format) }
  Step('clipboard');
  E.ProcessKey(VK_HOME, [ssCtrl]);
  for I := 1 to 4 do
    E.ProcessKey(VK_RIGHT, [ssShift]);
  Check(E.SelectedText = 'Bold', 'select for copy');
  E.CopyToClipboard;
  E.ProcessKey(VK_END, [ssCtrl]);
  E.PasteFromClipboard;
  Check(Copy(E.DocumentText, Length(E.DocumentText) - 3, 4) = 'Bold', 'clipboard paste: ' + E.DocumentText);
  N := 0;
  pd_doc_para_runs(E.Doc, E.CaretPos.block, nil, 0, N);
  SetLength(Runs, N);
  if N > 0 then
    pd_doc_para_runs(E.Doc, E.CaretPos.block, @Runs[0], N, N);
  if N > 0 then
    pd_doc_format_resolve(E.Doc, E.CaretPos.block, Runs[N - 1].format, Props);
  Check((N > 0) and (Props.weight = 700), 'pasted text stays bold');
  E.Undo;

  { 5d. files in other formats: save as DOCX and Markdown, load them back }
  Step('formats');
  E.SaveToFile(Dir + 'edit_test.docx');
  E.SaveToFile(Dir + 'edit_test.md');
  T := E.DocumentText;
  E.LoadFromFile(Dir + 'edit_test.docx');
  Check(E.DocumentText = T, 'docx round trip: ' + E.DocumentText);
  E.LoadFromFile(Dir + 'edit_test.md');
  Check(E.DocumentText = T, 'markdown round trip: ' + E.DocumentText);
  { the same through streams, as a host holding the bytes itself does it }
  Ms := TMemoryStream.Create;
  try
    E.SaveToStream(Ms, PD_CONV_DOCX);
    Ms.Position := 0;
    E.LoadFromStream(Ms, PD_CONV_DOCX, 'memory.docx');
    Check((E.DocumentText = T) and (E.FileName = 'memory.docx') and not E.Modified, 'docx stream round trip');
    Ms.Clear;
    E.SaveToStream(Ms, PD_CONV_MARKDOWN);
    Ms.Position := 0;
    E.LoadFromStream(Ms, -1);
    Check(E.DocumentText = T, 'markdown stream round trip, format detected');
  finally
    Ms.Free;
  end;

  { 6. up/down keep the column; clicking places the caret }
  E.ProcessKey(VK_UP, []);
  T := E.DocumentText;
  Check(E.CaretPos.block <> 0, 'up');
  E.ClickAt(0, 72 + 1, 72 + 5);
  Check(E.CaretPos.offset <= 1, 'click at the text start');

  { 7. a long paragraph flows onto a second page, and survives a save/load round trip }
  E.ProcessKey(VK_END, [ssCtrl]);
  E.ProcessKey(VK_RETURN, []);
  for I := 1 to 120 do
    E.InsertText('Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor. ');
  Check(E.PageCount >= 2, 'second page');
  E.SaveToFile(Dir + 'edit_test.pdoc');
  T := E.DocumentText;
  E.LoadFromFile(Dir + 'edit_test.pdoc');
  Check(E.DocumentText = T, 'save/load');
  E.ProcessKey(VK_END, [ssCtrl]);
  E.ProcessKey(VK_LEFT, [ssShift]);
  E.ProcessKey(VK_LEFT, [ssShift]);
  E.ProcessKey(VK_U, [ssCtrl]);
  E.ProcessKey(VK_HOME, [ssCtrl]);
  for I := 1 to 4 do
    E.ProcessKey(VK_RIGHT, [ssShift]);
  SavePage(E, 0, Dir + 'edit_typed_p1.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  E.ExportPDF(Dir + 'edit_typed.pdf');
  Check(FileExists(Dir + 'edit_typed.pdf'), 'PDF export');

  { 8. a picture Markdown only names, beside the file: loaded and drawn }
  Step('pictures');
  with TStringList.Create do
    try
      Add('A photo:');
      Add('');
      Add('![a photo](edit_photo.jpg "The photo")');
      SaveToFile(Dir + 'edit_pictures.md');
    finally
      Free;
    end;
  with TMemoryStream.Create do
    try
      LoadFromFile('tests/data/photo.jpg');
      SaveToFile(Dir + 'edit_photo.jpg');
    finally
      Free;
    end;
  E.LoadFromFile(Dir + 'edit_pictures.md');
  begin
    Items := nil;
    N := 0;
    pd_layout_page_items(E.Layout, 0, nil, 0, N);
    SetLength(Items, N + 1);
    pd_layout_page_items(E.Layout, 0, @Items[0], N, N);
    Pic := -1;
    for I := 0 to N - 1 do
      if Items[I].kind = PD_DRAW_IMAGE then
        Pic := I;
    Check((Pic >= 0) and (Items[Pic].resource <> 0), 'the named picture is loaded');
    Check((Pic >= 0) and (Items[Pic].w = 60 * 3 * 65536 div 4), 'at its own size, 96 dpi');
    if Pic >= 0 then
    begin
      Bmp := TBitmap.Create;
      try
        E.RenderPage(0, Bmp, 1.0 * 96 / 72 / PD_SP_PER_PT);
        PX := Bmp.Canvas.Pixels[Round((Items[Pic].x + Items[Pic].w div 2) * 96 / 72 / 65536) + 1,
          Round((Items[Pic].y + Items[Pic].h div 2) * 96 / 72 / 65536) + 1];
        { the photo's middle is a warm yellow, not the frame's pale blue }
        Check((Red(PX) > 200) and (Green(PX) > 150) and (Blue(PX) < 100), 'and drawn: ' + IntToHex(PX, 6));
      finally
        Bmp.Free;
      end;
    end;
  end;


  { 8b. the Home tab's operations: character and paragraph formatting, lists, styles }
  Step('home');
  E.NewDocument;
  E.InsertText('Plain text here');
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 6 do
    E.ProcessKey(VK_RIGHT, []);
  for I := 1 to 4 do
    E.ProcessKey(VK_RIGHT, [ssShift]);
  Check(E.SelectedText = 'text', 'select "text": ' + E.SelectedText);
  E.SetFontSize(18);
  Check(E.CurrentCharProps.size = 18 * PD_SP_PER_PT, 'font size 18');
  E.StepFontSize(True);
  Check(E.CurrentCharProps.size = 20 * PD_SP_PER_PT, 'a step up: 20');
  E.StepFontSize(False);
  E.StepFontSize(False);
  Check(E.CurrentCharProps.size = 16 * PD_SP_PER_PT, 'two down: 16');
  Lst := TStringList.Create;
  try
    E.GetFontFamilies(Lst);
    Check(Lst.Count > 0, 'font families');
    if Lst.Count > 0 then
    begin
      E.SetFontFamily(Lst[Lst.Count - 1]);
      Check(string(E.CurrentCharProps.family) = Lst[Lst.Count - 1], 'family: ' + E.CurrentCharProps.family);
    end;
    E.GetParagraphStyles(Lst);
    Check(Lst.IndexOf('Heading 1') >= 0, 'paragraph styles: ' + Lst.CommaText);
  finally
    Lst.Free;
  end;
  E.ToggleStrike;
  Check(E.CurrentCharProps.strike <> 0, 'strike');
  E.ToggleSuperscript;
  Check(E.CurrentCharProps.shift = PD_SHIFT_SUPER, 'superscript');
  E.ToggleSubscript;
  Check(E.CurrentCharProps.shift = PD_SHIFT_SUB, 'subscript in its place');
  E.ToggleSubscript;
  Check(E.CurrentCharProps.shift = PD_SHIFT_NONE, 'and off');
  E.SetTextColor($C00000);
  Check(E.CurrentCharProps.color and $FFFFFF = $C00000, 'colour');
  E.SetHighlight($FFFF00);
  Check(E.CurrentCharProps.background and $FFFFFF = $FFFF00, 'highlight');
  E.SetHighlight(-1);
  Check(E.CurrentCharProps.background = 0, 'no highlight');
  E.ClearFormatting;
  Props := E.CurrentCharProps;
  Check((Props.size <> 16 * PD_SP_PER_PT) and (Props.strike = 0) and (Props.color and $FFFFFF <> $C00000),
    'formatting cleared');
  Check(E.DocumentText = 'Plain text here', 'the text untouched: ' + E.DocumentText);

  { nothing selected: bold for what is typed next, one undo with it }
  E.ProcessKey(VK_END, []);
  E.ToggleBold;
  Check(E.CurrentCharProps.weight = 700, 'bold waiting at the caret');
  Check(E.DocumentText = 'Plain text here', 'nothing changed yet');
  E.InsertText(' strong');
  N := 0;
  pd_doc_para_runs(E.Doc, E.CaretPos.block, nil, 0, N);
  SetLength(Runs, N);
  if N > 0 then
    pd_doc_para_runs(E.Doc, E.CaretPos.block, @Runs[0], N, N);
  if N > 0 then
    pd_doc_format_resolve(E.Doc, E.CaretPos.block, Runs[N - 1].format, Props);
  Check((N >= 2) and (Runs[N - 1].start = 15) and (Props.weight = 700), 'typed text is bold');
  E.InsertText('er');
  Check(E.CurrentCharProps.weight = 700, 'and typing goes on bold');
  E.Undo;
  E.Undo;
  Check(E.DocumentText = 'Plain text here', 'typed bold text undone: ' + E.DocumentText);
  E.ToggleItalic;
  E.ProcessKey(VK_HOME, []);
  Check(E.CurrentCharProps.italic = 0, 'formatting waiting at the caret goes when it moves');

  { the format painter: the look at the caret, onto the next selection }
  E.NewDocument;
  E.InsertText('Plain and plain');
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 5 do
    E.ProcessKey(VK_RIGHT, [ssShift]);
  E.SetFontSize(20);
  E.ToggleItalic;
  E.ProcessKey(VK_HOME, []);
  E.ProcessKey(VK_RIGHT, []);
  E.StartFormatPainter;
  Check(E.FormatPainterOn, 'the painter on');
  E.ProcessKey(VK_END, []);
  for I := 1 to 5 do
    E.ProcessKey(VK_LEFT, [ssShift]);
  E.ApplyFormatPainter;
  Props := E.CurrentCharProps;
  Check((Props.size = 20 * PD_SP_PER_PT) and (Props.italic <> 0), 'the selection painted');
  Check(not E.FormatPainterOn, 'and the painter off after one');
  Check(E.PropsAt(PdPos(E.CaretPos.block, 7)).size <> 20 * PD_SP_PER_PT, 'the text between untouched');
  E.StartFormatPainter(True);
  E.ProcessKey(VK_HOME, []);
  E.ProcessKey(VK_RIGHT, [ssShift]);
  E.ApplyFormatPainter;
  Check(E.FormatPainterOn, 'a sticky painter stays on');
  E.StopFormatPainter;
  Check(not E.FormatPainterOn, 'until stopped');
  E.NewDocument;
  E.InsertText('Plain text here');

  { paragraphs }
  E.SetAlignment(PD_ALIGN_CENTER);
  Check(E.CurrentParaProps.align = PD_ALIGN_CENTER, 'centred');
  E.ChangeIndent(True);
  E.ChangeIndent(True);
  Check(E.CurrentParaProps.indent_left = 72 * PD_SP_PER_PT, 'indent twice: an inch');
  E.ChangeIndent(False);
  Pp := E.CurrentParaProps;
  Check((Pp.indent_left = 36 * PD_SP_PER_PT) and (Pp.align = PD_ALIGN_CENTER), 'back half an inch, still centred');
  E.SetLineSpacing(1500);
  E.SetParaSpacing(6, 12);
  Pp := E.CurrentParaProps;
  Check((Pp.line_spacing = 1500) and (Pp.space_before = 6 * PD_SP_PER_PT) and (Pp.space_after = 12 * PD_SP_PER_PT),
    'spacing');
  E.Undo;
  Check(E.CurrentParaProps.space_after <> 12 * PD_SP_PER_PT, 'spacing undone');

  { lists }
  E.ProcessKey(VK_END, []);
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Second');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Third');
  E.ProcessKey(VK_HOME, [ssCtrl]);
  E.ProcessKey(VK_END, [ssCtrl, ssShift]);
  E.ToggleList(PD_NUM_DECIMAL);
  Check(E.CurrentListFormat = PD_NUM_DECIMAL, 'numbered');
  pd_doc_list_label(E.Doc, E.CaretPos.block, @Lbl[0], 64);
  Check(string(Lbl) = '3.', 'the third is 3.: ' + Lbl);
  E.ProcessKey(VK_HOME, []);
  E.ChangeIndent(True);
  pd_doc_list_label(E.Doc, E.CaretPos.block, @Lbl[0], 64);
  Check(string(Lbl) = 'a.', 'indented in a list: a level down, a.: ' + Lbl);
  E.ChangeIndent(False);
  E.ProcessKey(VK_HOME, [ssCtrl]);
  E.ProcessKey(VK_END, [ssCtrl, ssShift]);
  E.ToggleList(PD_NUM_DECIMAL);
  Check(E.CurrentListFormat = -1, 'numbering off again');
  E.ToggleList(PD_NUM_BULLET);
  Check(E.CurrentListFormat = PD_NUM_BULLET, 'bullets');
  E.ToggleList(PD_NUM_DECIMAL);
  Check(E.CurrentListFormat = PD_NUM_DECIMAL, 'bullets to numbers');

  { styles, and the toolbar is told }
  Counter := TCounter.Create;
  try
    E.OnSelectionChange := @Counter.Hit;
    E.ProcessKey(VK_HOME, [ssCtrl]);
    E.SetParagraphStyle('Heading 1');
    Check(E.CurrentStyleName = 'Heading 1', 'style: ' + E.CurrentStyleName);
    E.Invalidate;
    for I := 1 to 20 do
    begin
      Application.ProcessMessages;
      Sleep(10);
    end;
    Check(Counter.N > 0, 'the selection-change event fired');
    E.OnSelectionChange := nil;
  finally
    Counter.Free;
  end;

  { 8c. the Insert tab's operations }
  Step('insert');
  E.NewDocument;
  E.InsertText('Before after');
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 7 do
    E.ProcessKey(VK_RIGHT, []);
  E.InsertTable(2, 3);
  Check(CountKind(E.Doc, PD_BLOCK_TABLE) = 1, 'a table');
  pd_doc_block_info(E.Doc, E.CaretPos.block, Info);
  pd_doc_block_info(E.Doc, Info.parent, Tb);
  Check(Tb.kind = PD_BLOCK_CELL, 'the caret in its first cell');
  pd_doc_block_info(E.Doc, Tb.parent, Rw);
  Check((Rw.kind = PD_BLOCK_ROW) and (Rw.child_count = 3) and (Rw.index = 0), 'three cells a row');
  pd_doc_block_info(E.Doc, Rw.parent, Tb);
  Check((Tb.kind = PD_BLOCK_TABLE) and (Tb.child_count = 2), 'two rows');
  E.InsertText('cell');
  Check(Pos('cell', E.DocumentText) > 0, 'typing in the cell');
  E.Undo;
  E.Undo;
  Check((CountKind(E.Doc, PD_BLOCK_TABLE) = 0) and (E.DocumentText = 'Before after'),
    'one undo takes the table away, the paragraph whole again: ' + E.DocumentText);

  Pages := E.PageCount;
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 7 do
    E.ProcessKey(VK_RIGHT, []);
  E.InsertBreak(PD_BREAK_PAGE);
  pd_doc_block_info(E.Doc, E.CaretPos.block, Info);
  Check(E.PageCount = Pages + 1, Format('a page break: another page (%d -> %d, breaks %d, caret in %d at %d, "%s")',
    [Pages, E.PageCount, CountKind(E.Doc, PD_BLOCK_BREAK), E.CaretPos.block, Info.index,
    E.ParaText(E.CaretPos.block)]));
  E.Undo;
  Check(E.PageCount = Pages, 'undone');
  E.InsertBreak(PD_BREAK_RULE);
  Check(CountKind(E.Doc, PD_BLOCK_BREAK) = 1, 'a horizontal rule');
  E.Undo;

  E.ProcessKey(VK_END, []);
  E.InsertText(' ');
  E.InsertLink('https://example.org/', 'Example');
  Check(Pos('Example', E.DocumentText) > 0, 'link text: ' + E.DocumentText);
  C := E.CaretPos;
  FillChar(Obj, SizeOf(Obj), 0);
  Check((pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3), Obj) = PD_OK) and (Obj.kind = PD_INLINE_LINK) and
    (Obj.source_len = 0), 'the link''s end mark before the caret');
  Check((pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3 - 7 - 3), Obj) = PD_OK) and
    (Obj.kind = PD_INLINE_LINK) and (Copy(Obj.source, 1, Obj.source_len) = 'https://example.org/'),
    'and its start mark with the address');
  Check(E.PropsAt(PdPos(C.block, C.offset - 5)).underline = PD_UNDERLINE_SINGLE, 'link text underlined');
  { a link over a selection }
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 6 do
    E.ProcessKey(VK_RIGHT, [ssShift]);
  E.InsertLink('https://parade.example/', '');
  Check(Copy(E.DocumentText, 4, 6) = 'Before', 'the selection kept as the link''s text: ' + E.DocumentText);
  Check((pd_doc_inline_at(E.Doc, PdPos(C.block, 0), Obj) = PD_OK) and (Obj.kind = PD_INLINE_LINK),
    'its start before it');

  E.ProcessKey(VK_END, [ssCtrl]);
  Check(E.InsertPicture('tests/data/photo.jpg'), 'a picture from a file');
  C := E.CaretPos;
  Check((pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3), Obj) = PD_OK) and (Obj.kind = PD_INLINE_IMAGE) and
    (Obj.resource <> 0) and (Obj.width = 60 * 3 * 65536 div 4), 'at its own size');
  Check(not E.InsertPicture('tests/data/nothing-here.png'), 'no file: no picture');

  E.InsertEquation('x^2+y^2=z^2', False);
  C := E.CaretPos;
  Check((pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3), Obj) = PD_OK) and
    (Obj.kind = PD_INLINE_EQUATION) and (Copy(Obj.source, 1, Obj.source_len) = 'x^2+y^2=z^2'), 'an equation');
  Check(E.PropsAt(PdPos(C.block, C.offset - 3)).color <> $FF0563C1, Format('the equation is not in the link''s colour (%x, picture %x)',
    [E.PropsAt(PdPos(C.block, C.offset - 3)).color, E.PropsAt(PdPos(C.block, C.offset - 6)).color]));
  E.InsertText(' and');
  E.InsertEquation('\int_0^1 f(x)\,dx', True);
  pd_doc_block_info(E.Doc, E.CaretPos.block, Info);
  Check((Info.role = PD_ROLE_EQUATION) and (E.CaretPos.offset = 3), 'a display equation on a line of its own');

  E.ProcessKey(VK_END, [ssCtrl]);
  E.InsertNote('A footnote.');
  C := E.CaretPos;
  Check((pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3), Obj) = PD_OK) and
    (Obj.kind = PD_INLINE_FOOTNOTE) and (Obj.target <> 0), 'a footnote mark');
  if Obj.target <> 0 then
    Check(E.ParaText(pd_doc_child(E.Doc, Obj.target, 0)) = 'A footnote.', 'its note');
  E.InsertField(PD_FIELD_PAGE);
  C := E.CaretPos;
  Check((pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3), Obj) = PD_OK) and (Obj.kind = PD_INLINE_FIELD),
    'a page number field');
  E.InsertTable(2, 2);

  { all of it through a Word file and back }
  E.SaveToFile(Dir + 'edit_insert.docx');
  E.LoadFromFile(Dir + 'edit_insert.docx');
  Check(CountKind(E.Doc, PD_BLOCK_TABLE) = 1, 'the table comes back from .docx');
  Check((Pos('Example', E.DocumentText) > 0) and (Pos('Before', E.DocumentText) > 0), 'and the links'' text');
  C := PdPos(pd_doc_next_paragraph(E.Doc, 0), 0);
  T := E.ParaText(C.block);
  I := Pos('x^', T);   { not there: the equation is one U+FFFC; find it as the object after the picture }
  N := 0;
  for I := 1 to Length(T) - 2 do
    if (Copy(T, I, 3) = #$EF#$BF#$BC) and (pd_doc_inline_at(E.Doc, PdPos(C.block, I - 1), Obj) = PD_OK) and
       (Obj.kind = PD_INLINE_EQUATION) then
    begin
      N := 1;
      Props := E.PropsAt(PdPos(C.block, I - 1));
      Check(Props.color <> $FF0563C1, Format('the equation after a link is not the link''s colour (%x, u%d, fmt %d; picture %x)',
        [Props.color, Props.underline, I - 1, E.PropsAt(PdPos(C.block, I - 4)).color]));
      Break;
    end;
  Check(N = 1, 'the equation comes back from .docx');
  SavePage(E, 0, Dir + 'edit_insert.png', 1.0 * 96 / 72 / PD_SP_PER_PT);

  { 8d. the Layout tab: page setup, sections, header and footer }
  Step('layout');
  E.NewDocument;
  for I := 1 to 6 do
    E.InsertText('The first section has its own page settings, header and footer. ');
  E.InsertText('First section.');
  Sp := E.CurrentSectionProps;
  Check(Sp.page_width < Sp.page_height, 'portrait to start');
  E.SetOrientation(True);
  Sp := E.CurrentSectionProps;
  Check(Sp.page_width > Sp.page_height, 'landscape');
  E.SetPageSize(595.28, 841.89);
  Sp := E.CurrentSectionProps;
  Check((Abs(Sp.page_width - Round(841.89 * PD_SP_PER_PT)) < 2) and (Sp.page_height < Sp.page_width), 'A4, still turned');
  E.SetOrientation(False);
  E.SetMargins(36, 36, 54, 54);
  Sp := E.CurrentSectionProps;
  Check((Sp.margin_top = 36 * PD_SP_PER_PT) and (Sp.margin_left = 54 * PD_SP_PER_PT), 'margins');
  E.SetColumns(2);
  Check(E.CurrentSectionProps.columns = 2, 'two columns');
  E.Undo;
  Check(E.CurrentSectionProps.columns <= 1, 'undone');
  E.SetHeaderFooter(False, 'My report', PD_ALIGN_LEFT);
  E.SetHeaderFooter(True, 'Page {page} of {pages}');
  Check(E.HeaderFooterText(False) = 'My report', 'header: ' + E.HeaderFooterText(False));
  Check(E.HeaderFooterText(True) = 'Page {page} of {pages}', 'footer with fields: ' + E.HeaderFooterText(True));
  SavePage(E, 0, Dir + 'edit_layout.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  Pages := E.PageCount;
  E.ProcessKey(VK_END, []);
  E.InsertSectionBreak(False);
  Check(pd_doc_story_count(E.Doc) >= 0, 'section break');
  Check(E.PageCount = Pages + 1, 'a section on a new page');
  E.InsertText('Second section.');
  E.SetOrientation(True);
  Sp := E.CurrentSectionProps;
  Check((Sp.page_width > Sp.page_height) and (Sp.margin_left = 54 * PD_SP_PER_PT) and (Sp.footer <> 0),
    'the new section turned, the rest as before');
  E.ProcessKey(VK_HOME, [ssCtrl]);
  Sp := E.CurrentSectionProps;
  Check(Sp.page_width < Sp.page_height, 'the first section still portrait');
  E.SaveToFile(Dir + 'edit_layout.docx');
  E.LoadFromFile(Dir + 'edit_layout.docx');
  Check(E.PageCount = 2, 'two pages back from .docx');
  Check(E.HeaderFooterText(True) = 'Page {page} of {pages}', 'the footer back from .docx: ' + E.HeaderFooterText(True));

  { 8e. the Table tab }
  Step('table');
  E.NewDocument;
  E.InsertTable(2, 2);
  Check(E.InTable, 'in a table');
  E.InsertText('a');
  E.TableInsertColumn(True);
  E.InsertText('b');
  Check(E.CellAt(E.CaretPos, Cell, Row, Tbl), 'in a cell');
  Check(ChildCountOf(E.Doc, Row) = 3, 'a column to the right: three cells');
  Tp := E.CurrentTableProps;
  Check((Tp.ncols = 3) and (Abs(Tp.col_width[0] - Tp.col_width[2]) <= Tp.col_width[0] div 10 + 2),
    Format('three columns, about as wide (%d %d %d)', [Tp.col_width[0], Tp.col_width[1], Tp.col_width[2]]));
  E.TableInsertRow(True);
  Check(ChildCountOf(E.Doc, Tbl) = 3, 'a row below: three rows');
  Check(ChildCountOf(E.Doc, pd_doc_child(E.Doc, Tbl, 1)) = 3, 'of three cells');
  E.TableDeleteRow;
  Check(ChildCountOf(E.Doc, Tbl) = 2, 'deleted again');
  { merge the first row's first two cells: a + (empty) }
  E.ProcessKey(VK_HOME, [ssCtrl]);
  E.TableMergeRight;
  Cp := E.CurrentCellProps;
  Check((Cp.col_span = 2) and (ChildCountOf(E.Doc, pd_doc_child(E.Doc, Tbl, 0)) = 2), 'merged across');
  E.TableSplitCell;
  Check((E.CurrentCellProps.col_span = 1) and (ChildCountOf(E.Doc, pd_doc_child(E.Doc, Tbl, 0)) = 3), 'split again');
  E.TableMergeDown;
  E.CellAt(PdPos(pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_child(E.Doc, Tbl, 1), 0), 0), 0), Cell, Row, Tbl);
  pd_doc_cell_props(E.Doc, Cell, Cp);
  Check(Cp.merge_up = 1, 'merged down: the cell below continues it');
  E.TableSplitCell;
  pd_doc_cell_props(E.Doc, Cell, Cp);
  Check(Cp.merge_up = 0, 'split again');
  E.SetCellShading($D9E2F3);
  Check(E.CurrentCellProps.background and $FFFFFF = $D9E2F3, 'shading');
  E.SetTableBorders(1.5);
  Check(E.CurrentTableProps.border = Round(1.5 * PD_SP_PER_PT), 'borders');
  E.SetHeaderRow(True);
  Check(E.CurrentTableProps.header_rows = 1, 'header row');
  E.DistributeColumns;
  Tp := E.CurrentTableProps;
  Check((Tp.col_width[0] = Tp.col_width[1]) and (Tp.col_width[1] = Tp.col_width[2]), 'columns even');
  E.TableDeleteColumn;
  Check(ChildCountOf(E.Doc, pd_doc_child(E.Doc, Tbl, 0)) = 2, 'a column deleted');
  Check(E.CurrentTableProps.ncols = 2, 'and its width');
  SavePage(E, 0, Dir + 'edit_table.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  E.TableDelete;
  Check(not E.InTable and (CountKind(E.Doc, PD_BLOCK_TABLE) = 0), 'the table deleted, the caret out of it');
  E.Undo;
  Check(CountKind(E.Doc, PD_BLOCK_TABLE) = 1, 'and back');

  { 8f. the References tab: a table of contents, captions, cross-references }
  Step('references');
  E.NewDocument;
  E.InsertText('Intro');
  E.SetParagraphStyle('Heading 1');
  E.ProcessKey(VK_RETURN, []);
  E.SetParagraphStyle('Normal');
  E.InsertText('Some text.');
  E.InsertCaption('Figure', 'A first picture');
  Check(Pos('Figure ', E.ParaText(E.CaretPos.block)) = 1, 'a caption: ' + E.ParaText(E.CaretPos.block));
  E.ProcessKey(VK_RETURN, []);
  E.SetParagraphStyle('Normal');
  E.InsertBreak(PD_BREAK_PAGE);
  E.InsertText('Methods');
  E.SetParagraphStyle('Heading 1');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Detail');
  E.SetParagraphStyle('Heading 2');
  E.ProcessKey(VK_RETURN, []);
  E.SetParagraphStyle('Normal');
  E.InsertCaption('Figure', 'A second one');
  E.InsertCaption('Table', 'Numbers');
  Refs := E.ReferenceTargets;
  Check(Length(Refs) = 6, Format('six targets: %d', [Length(Refs)]));
  if Length(Refs) = 6 then
  begin
    Check(not Refs[0].IsCaption and (Refs[0].Text = 'Intro') and (Refs[0].Level = 1), 'a heading: ' + Refs[0].Text);
    Check(Refs[1].IsCaption and (Refs[1].Seq = 'Figure') and (Refs[1].Number = 1) and
      (Refs[1].Text = 'Figure 1: A first picture'), 'Figure 1: ' + Refs[1].Text);
    Check(Refs[4].IsCaption and (Refs[4].Number = 2), 'Figure 2: ' + Refs[4].Text);
    Check(Refs[5].IsCaption and (Refs[5].Seq = 'Table') and (Refs[5].Number = 1), 'Table 1 on its own count');
    E.ProcessKey(VK_END, [ssCtrl]);
    E.ProcessKey(VK_RETURN, []);
    E.SetParagraphStyle('Normal');
    E.InsertText('See ');
    E.InsertCrossReference(Refs[4], prfLabel);
    E.InsertText(' on page ');
    E.InsertCrossReference(Refs[4], prfPage);
    E.InsertText(', and ');
    E.InsertCrossReference(Refs[2], prfText);
    Check(Pos('See Figure ', E.ParaText(E.CaretPos.block)) = 1, 'a reference: ' + E.ParaText(E.CaretPos.block));
    Check(Pos(', and Methods', E.ParaText(E.CaretPos.block)) > 0, 'a heading''s text');
  end;
  E.ProcessKey(VK_HOME, [ssCtrl]);
  E.InsertTableOfContents;
  Check(CountStyle(E, 'TOC Heading') = 1, 'a table of contents with its title');
  Check(CountStyle(E, 'TOC 1') = 2, Format('two first-level entries: %d', [CountStyle(E, 'TOC 1')]));
  Check(CountStyle(E, 'TOC 2') = 1, 'one second-level');
  Check((Pos('Contents', E.DocumentText) = 1), 'the table first: ' + Copy(E.DocumentText, 1, 40));
  SavePage(E, 0, Dir + 'edit_toc.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  { a heading more, and the table updated }
  E.ProcessKey(VK_END, [ssCtrl]);
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Results');
  E.SetParagraphStyle('Heading 1');
  Check(E.UpdateTableOfContents, 'updated');
  Check(CountStyle(E, 'TOC 1') = 3, Format('three first-level entries now: %d', [CountStyle(E, 'TOC 1')]));
  Check(CountStyle(E, 'TOC Heading') = 1, 'still one table');
  E.Undo;
  Check(CountStyle(E, 'TOC 1') = 2, 'the update undone in one');
  E.InsertBookmark('results');
  { through Word and back: the table is found again by its styles }
  E.SaveToFile(Dir + 'edit_refs.docx');
  E.LoadFromFile(Dir + 'edit_refs.docx');
  Check(CountStyle(E, 'TOC 1') = 2, 'the table of contents back from .docx');
  Check(E.UpdateTableOfContents and (CountStyle(E, 'TOC 1') = 3), 'and updatable there');

  { 8g. the View tab: zoom to fit, formatting marks, headings }
  Step('view');
  Z := E.PageWidthZoom;
  Check((Z > 0.3) and (Z < 4), Format('a page-width zoom: %g', [Z]));
  Check(E.WholePageZoom <= Z + 1e-9, 'a whole page is no wider than the page''s width');
  E.Zoom := Z;
  Check(Abs(E.Zoom - Z) < 1e-9, 'zoomed');
  Heads := TStringList.Create;
  try
    E.GetHeadings(Heads);
    Check((Heads.Count = 4) and (Trim(Heads[2]) = 'Detail') and (Heads[2][1] = ' '), 'the headings, indented by level: ' +
      Heads.CommaText);
    if Heads.Count = 4 then
    begin
      E.GoToPos(PdPos(pd_block_id(PtrUInt(Heads.Objects[3])), 0));
      Check(Copy(E.ParaText(E.CaretPos.block), 1, 7) = 'Results', Format('going to a heading (%d, caret in %d: "%s")',
        [PtrUInt(Heads.Objects[3]), E.CaretPos.block, E.ParaText(E.CaretPos.block)]));
    end;
  finally
    Heads.Free;
  end;
  E.Zoom := 1;
  E.ShowMarks := False;
  Bmp := TBitmap.Create;
  try
    E.RenderPage(0, Bmp, 1.0 * 96 / 72 / PD_SP_PER_PT);
    I := DarkPixels(Bmp);
  finally
    Bmp.Free;
  end;
  E.ShowMarks := True;
  Check(E.ShowMarks, 'formatting marks on');

  { 8h. the system's fonts: listed, loaded when used }
  Step('system fonts');
  T0 := GetTickCount64;
  Sys := ParadeSystemFaces;
  WriteLn(StdErr, Format('  %d system faces in %d ms', [Length(Sys), GetTickCount64 - T0]));
  Check(Length(Sys) > 10, 'the system''s fonts are found');
  Check(Length(ParadeSystemFaces) = Length(Sys), 'and kept');
  if Length(Sys) > 0 then
  begin
    { a file's own tables say what fontconfig says }
    K := -1;
    for I := 0 to High(Sys) do
      if (LowerCase(ExtractFileExt(Sys[I].FileName)) = '.ttf') and (Sys[I].Index = 0) then
      begin
        K := I;
        Break;
      end;
    if K >= 0 then
    begin
      One := ParadeReadFontFile(Sys[K].FileName);
      Check((Length(One) = 1) and (One[0].Family = Sys[K].Family) and (One[0].Italic = Sys[K].Italic) and
        (Abs(One[0].Weight - Sys[K].Weight) <= 100), Format('the file''s own name: "%s" for "%s" (%s)',
        [IfThen(Length(One) > 0, One[0].Family, '-'), Sys[K].Family, Sys[K].FileName]));
    end;
    Scanned := ParadeScanFontDirs(['/usr/share/fonts/truetype']);
    Check(Length(Scanned) > 0, Format('the folders read where fontconfig is not: %d faces', [Length(Scanned)]));
  end;
  E.NewDocument;
  Lst := TStringList.Create;
  try
    E.GetFontFamilies(Lst);
    Before := Lst.Count;
    T0 := GetTickCount64;
    Added := E.AddSystemFonts;
    E.GetFontFamilies(Lst);
    WriteLn(StdErr, Format('  %d faces registered in %d ms; %d families listed, %d before', [Added,
      GetTickCount64 - T0, Lst.Count, Before]));
    Check(Lst.Count > Before + 5, 'the font list has the system''s families');
    { one the editor did not have, used: loaded then, and drawn }
    Fam := '';
    for I := 0 to High(Sys) do
      if (Pos('Mono', Sys[I].Family) = 0) and (LowerCase(ExtractFileExt(Sys[I].FileName)) = '.ttf') and
         not Sys[I].Italic and (Sys[I].Weight = 400) and (Pos(LowerCase(Sys[I].Family), 'liberation serif liberation sans') = 0) then
      begin
        Fam := Sys[I].Family;
        Break;
      end;
    if Fam <> '' then
    begin
      E.InsertText('In a system font.');
      E.SelectAll;
      E.SetFontFamily(Fam);
      Check(string(E.CurrentCharProps.family) = Fam, 'the text in ' + Fam);
      Bmp := TBitmap.Create;
      try
        E.RenderPage(0, Bmp, 1.0 * 96 / 72 / PD_SP_PER_PT);
        Check(DarkPixels(Bmp) > 100, 'drawn in it');
      finally
        Bmp.Free;
      end;
    end;
    { a family no font has falls back to the editor's own, not to some system face }
    E.SelectAll;
    E.SetFontFamily('No Such Family Anywhere');
    Bmp := TBitmap.Create;
    try
      E.RenderPage(0, Bmp, 1.0 * 96 / 72 / PD_SP_PER_PT);
      Check(DarkPixels(Bmp) > 100, 'an unknown family still drawn');
    finally
      Bmp.Free;
    end;
  finally
    Lst.Free;
  end;

  { 9. review: tracked changes and comments }
  Step('review');
  E.NewDocument;
  E.InsertText('The quick fox');
  E.Author := 'Ann';
  E.TrackChanges := True;
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 10 do
    E.ProcessKey(VK_RIGHT, []);
  E.InsertText('brown ');
  Check(E.DocumentText = 'The quick brown fox', 'tracked typing: ' + E.DocumentText);
  Check((pd_layout_page_markup(E.Layout, 0, nil, 0, N) = PD_OK) and (N = 1), 'an insertion on the page');
  E.InsertText('XY');
  E.ProcessKey(VK_BACK, []);
  E.ProcessKey(VK_BACK, []);
  Check(E.DocumentText = 'The quick brown fox', 'own insertion really goes: ' + E.DocumentText);
  E.ProcessKey(VK_HOME, []);
  for I := 1 to 4 do
    E.ProcessKey(VK_DELETE, []);
  Check(E.DocumentText = 'The quick brown fox', 'deleted text stays, marked: ' + E.DocumentText);
  Check((pd_layout_page_markup(E.Layout, 0, nil, 0, N) = PD_OK) and (N = 2), 'a deletion and an insertion');
  { the caret steps over the hidden deletion }
  E.ProcessKey(VK_HOME, []);
  E.ProcessKey(VK_RIGHT, []);
  Check(E.CaretPos.offset = 5, 'right skips the deletion: ' + IntToStr(E.CaretPos.offset));
  E.ProcessKey(VK_LEFT, []);
  E.ProcessKey(VK_BACK, []);
  Check(E.DocumentText = 'The quick brown fox', 'backspace over a deletion leaves it');
  E.TrackChanges := False;
  { a comment on "fox" }
  E.ProcessKey(VK_END, []);
  E.ProcessKey(VK_LEFT, [ssShift]);
  E.ProcessKey(VK_LEFT, [ssShift]);
  E.ProcessKey(VK_LEFT, [ssShift]);
  Check(E.SelectedText = 'fox', 'select fox: ' + E.SelectedText);
  Check(E.AddComment('Which fox?') = 1, 'comment added');
  Check(E.ReplyToComment(1, 'The red one.') = 2, 'reply added');
  Check(E.CommentAt(PdPos(E.CaretPos.block, 17)) = 1, 'comment at a position');
  Check((pd_layout_page_markup(E.Layout, 0, nil, 0, N) = PD_OK) and (N = 3), 'two changes and a comment');
  E.SaveToFile(Dir + 'edit_review.docx');
  E.LoadFromFile(Dir + 'edit_review.docx');
  Check(E.DocumentText = 'The quick brown fox', 'review docx round trip: ' + E.DocumentText);
  Check((pd_doc_revision_count(E.Doc) = 2) and (pd_doc_comment_count(E.Doc) = 2), 'changes and comments come back');
  { next change from the start: the deletion; accept it, and the insertion is selected next }
  E.ProcessKey(VK_HOME, [ssCtrl]);
  Check(E.NextChange(1) and (E.SelectedText = 'The '), 'next change: ' + E.SelectedText);
  E.AcceptChange;
  Check(E.DocumentText = 'quick brown fox', 'accepted deletion: ' + E.DocumentText);
  Check(E.SelectedText = 'brown ', 'then the next change: ' + E.SelectedText);
  E.RejectChange;
  Check(E.DocumentText = 'quick fox', 'rejected insertion: ' + E.DocumentText);
  E.Undo;
  E.Undo;
  Check(E.DocumentText = 'The quick brown fox', 'undo review: ' + E.DocumentText);
  E.AcceptAllChanges;
  Check(E.DocumentText = 'quick brown fox', 'accept all: ' + E.DocumentText);
  E.DeleteComment(1);
  Check(pd_doc_comment_get(E.Doc, 2, Cm) <> PD_OK, 'comment and reply deleted');

  { a real reviewed document, with the balloons in view }
  if FileExists(ParamStr(2)) then
  begin
    Step('reviewed document');
    E.LoadFromFile(ParamStr(2));
    Form.SetBounds(0, 0, 1260, 1000);
    for I := 1 to 20 do
    begin
      Application.ProcessMessages;
      Sleep(20);
    end;
    E.Invalidate;
    E.Update;
    Application.ProcessMessages;
    Check((pd_layout_page_markup(E.Layout, 0, nil, 0, N) = PD_OK) and (N > 5), 'markup on the first page');
    ExecuteProcess('/usr/bin/import', ['-window', 'root', Dir + 'edit_review.png']);
    E.MarkupMode := PD_MARKUP_INLINE;
    E.Update;
    Application.ProcessMessages;
    ExecuteProcess('/usr/bin/import', ['-window', 'root', Dir + 'edit_review_inline.png']);
    E.MarkupMode := PD_MARKUP_BALLOONS;
  end;

  WriteLn(Checks, ' checks, ', Failures, ' failures');
  E.Free;
  Form.Free;
  except
    on X: Exception do
      Fail(X);
  end;
  if Failures > 0 then
    Halt(1);
end.
