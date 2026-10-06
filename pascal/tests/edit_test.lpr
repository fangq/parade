{ Headless test of TParadeEdit: drives the editor like a user and renders pages.
  Run under a display (xvfb-run). Writes PNGs next to the executable. }
program edit_test;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads,{$ENDIF}
  Interfaces, Forms, Controls, Graphics, LCLType, SysUtils, Classes, IntfGraphics, FPImage, parade, paradeedit;

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
