{ Headless test of TParadeEdit: drives the editor like a user and renders pages.
  Run under a display (xvfb-run). Writes PNGs next to the executable. }
program edit_test;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads,{$ENDIF}
  Interfaces, Forms, Controls, Graphics, LCLType, SysUtils, Classes, parade, paradeedit;

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

procedure SavePage(E: TParadeEdit; Page: Integer; const FileName: string; Scale: Double);
var
  Bmp: TBitmap;
  Png: TPortableNetworkGraphic;
begin
  Bmp := TBitmap.Create;
  Png := TPortableNetworkGraphic.Create;
  try
    E.RenderPage(Page, Bmp, Scale);
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
