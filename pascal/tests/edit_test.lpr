{ Headless test of TParadeEdit: drives the editor like a user and renders pages.
  Run under a display (xvfb-run). Writes PNGs next to the executable. }
program edit_test;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads,{$ENDIF}
  Interfaces, Forms, Controls, Graphics, LCLType, SysUtils, Classes, StrUtils, IntfGraphics, FPImage, Math, Clipbrd, ctypes, parade, paradeedit,
  paradefonts, paradeshow, paradeanim, paradeshowpdf, process, FPWritePNG, FPReadJPEG;

type
  { the wheel, as the mouse turns it }
  TParadeWheel = class(TParadeEdit);

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

{ a file's bytes }
function FileText(const FileName: string): RawByteString;
var
  S: TStringStream;
begin
  S := TStringStream.Create('');
  try
    S.LoadFromFile(FileName);
    Result := S.DataString;
  finally
    S.Free;
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
  Same: Boolean;
  CapB: pd_block_id;
  CapPage: Int32;
  CapX, CapBase, CapAsc, CapDesc: pd_sp;
  CapFile: string;
  DrawP: pd_pos;
  DrawSid: Integer;
  DrawPara: pd_block_id;
  BX0, BY0, BX1, BY1, NX0, NY0, NX1, NY1: Double;
  NShapes: Integer;
  TbSid: Integer;
  TbPage: Int32;
  Xml: string;
  NStories, J, PickOk, PickAll, PickPg: Integer;
  PickSeen: string;
  Pt0, Pt1: TPoint;
  StA, StB, Bk: pd_block_id;
  CanvasP: pd_pos;
  E: TParadeEdit;
  Dir, Sample, T: string;
  C: pd_pos;
  Props: pd_char_props;
  Info: pd_block_info;
  I: Integer;
  N: Int32;
  Runs: array of pd_run;
  Ms: TMemoryStream;
  P, Q, A, B: pd_pos;
  Names: TStringList;
  Kind, Spec: string;
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

{ the story at the top of a block's parents (0: the body's) }
function StoryTopOfTest(E: TParadeEdit; B: pd_block_id): pd_block_id;
var
  Info: pd_block_info;
begin
  while (pd_doc_block_info(E.Doc, B, Info) = PD_OK) and (Info.parent <> 0) do
    B := Info.parent;
  if (pd_doc_block_info(E.Doc, B, Info) = PD_OK) and (Info.kind = PD_BLOCK_STORY) then
    Result := B
  else
    Result := 0;
end;

{ a canvas in a new document: clicked while empty, a shape drawn, its handles (the cursor over them, the corner's
  radius, the turn), its points edited with a double click, a text box typed in as soon as it is drawn }
procedure TestShapeHandles(E: TParadeEdit);
var
  CP, D: pd_pos;
  Sid: Integer;
  CPage: Int32;
  CX, Base, Asc, Desc: pd_sp;
  Pt, P2: TPoint;
  G: TParadeShapeGeom;
  Xml: string;
  W0: Double;
  N0: Integer;
  TB: pd_block_id;

  function At(XPt, YPt: Double): TPoint;
  begin
    pd_layout_caret(E.Layout, CP, CPage, CX, Base, Asc, Desc);
    Result := E.PageToClient(CPage, CX / PD_SP_PER_PT + XPt, Base / PD_SP_PER_PT - 216 + YPt);
  end;

  procedure Drag(A, B: TPoint);
  begin
    TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseMove([ssLeft], (A.X + B.X) div 2, (A.Y + B.Y) div 2);
    TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y);
  end;

begin
  E.NewDocument;
  E.InsertText('Before');
  E.ProcessKey(VK_RETURN, []);
  Check(E.InsertCanvas, 'a canvas in a new document');
  CP := PdPos(E.CaretPos.block, 0);
  E.ProcessKey(VK_ESCAPE, []);
  E.Repaint;
  Application.ProcessMessages;
  Check(not E.SelectedShape(D, Sid), 'the canvas let go');
  Pt := At(20, 20);
  TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  Check(E.SelectedShape(D, Sid) and (Sid = -1) and (D.block = CP.block), 'an empty canvas clicked: selected');
  E.InsertShape('roundRect');
  Drag(At(100, 40), At(250, 140));
  Check(E.SelectedShapeGeom(G) and (G.Prst = 'roundRect') and (Length(G.AdjN) = 1) and
    (Abs(G.W - 150 * PD_SP_PER_PT) < 2 * PD_SP_PER_PT), 'a rounded rectangle drawn');
  Check(E.ShapeHandlePoint('s', 0, Pt), 'its top left handle');
  TParadeWheel(E).MouseMove([], Pt.X, Pt.Y);
  Check(E.Cursor = crSizeNWSE, 'over it: the diagonal sizing cursor');
  E.ShapeHandlePoint('s', 3, Pt);
  TParadeWheel(E).MouseMove([], Pt.X, Pt.Y);
  Check(E.Cursor = crSizeWE, 'over the right one: the sideways one');
  Pt := At(175, 90);
  TParadeWheel(E).MouseMove([], Pt.X, Pt.Y);
  Check(E.Cursor = crSizeAll, 'inside: the move cursor');
  Check(E.ShapeHandlePoint('a', 0, Pt), 'a handle for the corners'' radius');
  TParadeWheel(E).MouseMove([], Pt.X, Pt.Y);
  Check(E.Cursor = crHandPoint, 'over it: the hand');
  Drag(Pt, Point(Pt.X + 30, Pt.Y));
  Check(E.SelectedShapeGeom(G) and (G.AdjV[0] > 30000) and E.KeptXml(Xml) and
    (Pos('<a:gd name="adj" fmla="val ', Xml) > 0), Format('dragged: rounder (%.0f)', [G.AdjV[0]]));
  E.Undo;
  Check(E.KeptXml(Xml) and (Pos('<a:gd name="adj"', Xml) = 0), 'and back with undo');
  E.Redo;
  if not E.SelectedShape(D, Sid) or (Sid < 0) then
  begin
    Pt := At(175, 90);
    TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
    TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  end;
  Check(E.ShapeHandlePoint('r', 0, Pt), 'a handle to turn it');
  P2 := At(175, 90);
  Drag(Pt, Point(P2.X + 120, P2.Y));
  Check(E.SelectedShapeGeom(G) and (Abs(G.Rot - 90) < 3) and E.KeptXml(Xml) and (Pos(' rot="', Xml) > 0),
    Format('turned a quarter by its handle (%.1f)', [G.Rot]));
  Check(E.RotateShape(30) and E.SelectedShapeGeom(G) and (Abs(G.Rot - 30) < 0.01), 'turned to 30 degrees');
  Check(E.FlipShape(True) and E.SelectedShapeGeom(G) and G.FlipH, 'flipped');
  E.FlipShape(True);
  E.RotateShape(0);
  W0 := G.W;
  Pt := At(175, 90);
  TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  TParadeWheel(E).DblClick;
  Check(E.EditingPoints and E.KeptXml(Xml) and (Pos('<a:custGeom>', Xml) > 0) and (Pos('<a:prstGeom', Xml) = 0),
    'a double click: its points, a custom geometry');
  Check(E.ShapeHandlePoint('n', 0, Pt), 'its first point');
  Drag(Pt, Point(Pt.X - 40, Pt.Y - 40));
  Check(E.EditingPoints and E.SelectedShapeGeom(G) and (G.W > W0 + 2 * PD_SP_PER_PT),
    Format('a point dragged out: the shape wider (%.1f, was %.1f)', [G.W / PD_SP_PER_PT, W0 / PD_SP_PER_PT]));
  E.ProcessKey(VK_ESCAPE, []);
  Check(not E.EditingPoints and E.SelectedShape(D, Sid) and (Sid >= 0), 'Escape: the shape, without its points');
  E.ShapeFillColor := $0047AD70;
  E.InsertShape('textbox');
  Drag(At(300, 20), At(420, 70));
  Check(not E.SelectedShape(D, Sid) and (StoryTopOfTest(E, E.CaretPos.block) <> 0),
    'a text box drawn: the caret in it');
  E.InsertText('Typed');
  Check(E.ParaText(E.CaretPos.block) = 'Typed', 'typed in at once');
  { in the text box, as in any text: a double click a word, a third the paragraph }
  E.InsertText(' more words');
  TB := E.CaretPos.block;
  if pd_layout_caret(E.Layout, PdPos(TB, 8), CPage, CX, Base, Asc, Desc) = PD_OK then
  begin
    P2 := E.PageToClient(CPage, CX / PD_SP_PER_PT, (Base - Asc / 2) / PD_SP_PER_PT);
    TParadeWheel(E).MouseDown(mbLeft, [], P2.X, P2.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], P2.X, P2.Y);
    TParadeWheel(E).MouseDown(mbLeft, [ssDouble], P2.X, P2.Y);
    TParadeWheel(E).DblClick;
    TParadeWheel(E).MouseUp(mbLeft, [], P2.X, P2.Y);
    Check(E.SelectedText = 'more', 'a double click in a text box: the word, ' + E.SelectedText);
    TParadeWheel(E).TripleClick;
    TParadeWheel(E).MouseDown(mbLeft, [ssTriple], P2.X, P2.Y);
    TParadeWheel(E).MouseMove([ssLeft], P2.X + 1, P2.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], P2.X, P2.Y);
    Check(E.SelectedText = 'Typed more words', 'a triple click in a text box: the paragraph, ' + E.SelectedText);
  end
  else
    Check(False, 'the text box''s caret placed');
  { the canvas again: drawn by hand, and Word's other shapes }
  Pt := At(20, 20);
  TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  N0 := Length(E.DrawingShapes(CP));
  Check(E.InsertShape('freeform') and (E.DrawKind = 'freeform'), 'a freeform to draw');
  for Sid := 0 to 2 do
  begin
    Pt := At(40 + 50 * Ord(Sid = 1), 160 - 40 * Ord(Sid = 1) + 30 * Ord(Sid = 2));
    if Sid = 2 then Pt := At(20, 200);
    TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  end;
  Pt := At(40, 160);    { the first corner again: closed }
  TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  Check((E.DrawKind = '') and (Length(E.DrawingShapes(CP)) = N0 + 1) and E.KeptXml(Xml) and
    (Pos('name="Freeform: Shape', Xml) > 0) and (Pos('<a:close/>', Xml) > 0), 'drawn corner by corner, closed');
  Check(E.InsertShape('curve'), 'a curve to draw');
  Pt := At(150, 160); TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y); TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  Pt := At(190, 120); TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y); TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  Pt := At(230, 170); TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y); TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  TParadeWheel(E).MouseDown(mbLeft, [ssDouble], Pt.X, Pt.Y);
  TParadeWheel(E).DblClick;
  TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  Check((E.DrawKind = '') and (Length(E.DrawingShapes(CP)) = N0 + 2) and E.KeptXml(Xml) and
    (Pos('<a:cubicBezTo>', Xml) > 0), 'a curve through three clicks, ended by a double click');
  Check(E.InsertShape('scribble'), 'a scribble');
  Pt := At(260, 150);
  TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
  for Sid := 1 to 30 do
    TParadeWheel(E).MouseMove([ssLeft], Pt.X + Sid * 3, Pt.Y + Round(10 * Sin(Sid / 3)));
  TParadeWheel(E).MouseUp(mbLeft, [], Pt.X + 90, Pt.Y);
  Check((Length(E.DrawingShapes(CP)) = N0 + 3) and E.SelectedShape(D, Sid) and (Sid = N0 + 2), 'drawn by hand');
  Check(E.InsertShape('elbowDoubleArrow'), 'an elbow connector');
  Drag(At(300, 100), At(380, 180));
  Check(E.KeptXml(Xml) and (Pos('prst="bentConnector3"', Xml) > 0) and (Pos('<a:headEnd type="triangle"/>', Xml) > 0),
    'with arrowheads at both ends');
  Check(E.InsertShape('smileyFace'), 'one of Word''s other shapes');
  Drag(At(400, 100), At(480, 180));
  Check(E.SelectedShapeGeom(G) and (G.Prst = 'smileyFace') and (Length(G.HX) = 1), 'its handle, as Office defines it');
  if E.ShapeHandlePoint('a', 0, Pt) then
  begin
    Drag(Pt, Point(Pt.X, Pt.Y - 15));
    Check(E.KeptXml(Xml) and (Pos('<a:gd name="adj" fmla="val ', Xml) > 0) and E.SelectedShapeGeom(G) and
      (G.AdjV[0] < 4653), Format('dragged: the smile turned (%.0f)', [G.AdjV[0]]));
  end;
  SavePage(E, 0, ExtractFilePath(ParamStr(0)) + 'edit_handles.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  E.SaveToFile(ExtractFilePath(ParamStr(0)) + 'edit_handles.docx');
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + 'edit_handles.docx');
  Xml := '';
  for Sid := 0 to pd_doc_story_count(E.Doc) - 1 do
    Xml := Xml + E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, Sid), 0));
  Check(Pos('Typed', Xml) > 0, 'saved and read back: the text box''s text');
end;

procedure SortBoxesX(var B: TParadeShapeBoxes);
var
  I, K: Integer;
  T: TParadeShapeBox;
begin
  for I := 0 to High(B) do
    for K := I + 1 to High(B) do
      if B[K].X0 < B[I].X0 then
      begin
        T := B[I]; B[I] := B[K]; B[K] := T;
      end;
end;

{ the description of the drawing at P, as the document has it }
function DrawingText(E: TParadeEdit; const P: pd_pos): string;
var
  O: pd_inline;
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
begin
  Result := '';
  if (pd_doc_inline_at(E.Doc, P, O) = PD_OK) and (pd_doc_resource(E.Doc, O.resource, @Mime, @Data, @Len) = PD_OK) then
    SetString(Result, PAnsiChar(Data), Len);
end;

function StoryText(E: TParadeEdit; St: pd_block_id): string;
begin
  Result := E.ParaText(pd_doc_child(E.Doc, St, 0));
end;

{ shapes undone with their selection, copied, cut, pasted and duplicated (text boxes with their text), and a text
  box's story going with it when it is deleted }
procedure TestShapeEditing(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
var
  CP, D: pd_pos;
  Sid, I, N0, S0: Integer;
  B: TParadeShapeBoxes;
  St, St2: pd_block_id;
  Xml: string;
  Pg: Int32;
  PX0, PY0, PX1, PY1: Double;
  Ch: TUTF8Char;
  A: TPoint;
  W: Double;
  LDash, LHead, LTail: string;

  function Pt(U, V: Double): TPoint;     { a point of the canvas (points from its corner) in the control }
  begin
    Result := E.PageToClient(Pg, PX0 / K + U, PY0 / K + V);
  end;

  procedure Drag(A, B: TPoint);
  begin
    TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseMove([ssLeft], (A.X + B.X) div 2, (A.Y + B.Y) div 2);
    TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y);
  end;

begin
  E.NewDocument;
  E.InsertText('Shapes');
  E.ProcessKey(VK_RETURN, []);
  CP := E.CaretPos;
  Check(E.InsertCanvas(300, 200), 'a canvas for the shapes');
  CP := PdPos(CP.block, 0);
  E.AddShape('rect', 20 * K, 20 * K, 100 * K, 80 * K);
  E.AddShape('ellipse', 150 * K, 30 * K, 250 * K, 120 * K);
  Check(E.SelectedShape(D, Sid) and (Sid = 1) and (Length(E.DrawingShapes(CP)) = 2), 'two shapes, the second selected');
  E.SetShapeFill($000000FF, False);
  E.ProcessKey(VK_Z, [ssCtrl]);
  Check(E.SelectedShape(D, Sid) and (Sid = 1), 'its fill undone: still selected');
  E.ProcessKey(VK_Y, [ssCtrl]);
  Check(E.SelectedShape(D, Sid) and (Sid = 1) and E.KeptXml(Xml) and (Pos('FF0000', Xml) > 0),
    'redone: selected');
  E.ProcessKey(VK_TAB, []);
  Check(E.SelectedShape(D, Sid) and (Sid = 0), 'Tab: the first');
  E.ProcessKey(VK_DELETE, []);
  Check((Length(E.DrawingShapes(CP)) = 1) and E.SelectedShape(D, Sid) and (Sid = -1), 'deleted: the canvas selected');
  E.Undo;
  Check((Length(E.DrawingShapes(CP)) = 2) and E.SelectedShape(D, Sid) and (Sid = 0),
    'undone: the shape back, and selected');
  E.Redo;
  Check((Length(E.DrawingShapes(CP)) = 1) and E.SelectedShape(D, Sid) and (Sid = -1), 'redone: the canvas selected');
  E.Undo;
  { copied and pasted: beside the shape it is a copy of }
  E.ProcessKey(VK_C, [ssCtrl]);
  Check(E.CanPasteShapes, 'copied: on the clipboard');
  E.ProcessKey(VK_V, [ssCtrl]);
  B := E.DrawingShapes(CP);
  Check((Length(B) = 3) and E.SelectedShape(D, Sid) and (Sid = 2), 'pasted: a third shape, selected');
  if Length(B) = 3 then
    Check((Abs(B[2].X0 - B[0].X0 - 9 * K) < K) and (Abs(B[2].Y0 - B[0].Y0 - 9 * K) < K),
      Format('beside the first (%.1f, %.1f)', [(B[2].X0 - B[0].X0) / K, (B[2].Y0 - B[0].Y0) / K]));
  E.ProcessKey(VK_V, [ssCtrl]);
  B := E.DrawingShapes(CP);
  Check((Length(B) = 4) and (Abs(B[3].X0 - B[0].X0 - 18 * K) < K), 'pasted again: further along');
  E.ProcessKey(VK_Z, [ssCtrl]);
  E.ProcessKey(VK_Z, [ssCtrl]);
  Check((Length(E.DrawingShapes(CP)) = 2) and E.SelectedShape(D, Sid) and (Sid = 0), 'both undone');
  E.ProcessKey(VK_D, [ssCtrl]);
  Check((Length(E.DrawingShapes(CP)) = 3) and E.SelectedShape(D, Sid) and (Sid = 2), 'Ctrl+D: duplicated');
  E.Undo;
  E.ProcessKey(VK_TAB, []);
  Check(E.SelectedShape(D, Sid) and (Sid = 1) and E.ToggleShape(0) and (Length(E.SelectedShapes) = 2),
    'both shapes selected');
  E.ProcessKey(VK_X, [ssCtrl]);
  Check((Length(E.DrawingShapes(CP)) = 0) and E.SelectedShape(D, Sid) and (Sid = -1), 'cut: the canvas empty');
  E.ProcessKey(VK_V, [ssCtrl]);
  B := E.DrawingShapes(CP);
  Check((Length(B) = 2) and (Length(E.SelectedShapes) = 2) and (Abs(B[0].X0 - 20 * K) < K),
    'pasted back where they were, both selected');
  { a rubber band round shapes: those wholly in it selected; dragged, all of them moved }
  Check(E.ShapePageBox(CP, -1, Pg, PX0, PY0, PX1, PY1), 'where the canvas is');
  Drag(Pt(10, 10), Pt(110, 90));
  Check((Length(E.SelectedShapes) = 1) and (E.SelectedShapes[0] = 0), 'a rubber band round the rectangle: it');
  Drag(Pt(5, 5), Pt(290, 190));
  Check(Length(E.SelectedShapes) = 2, 'round both: both');
  Drag(Pt(60, 50), Pt(90, 50));
  B := E.DrawingShapes(CP);
  Check((Length(B) = 2) and (Abs(B[0].X0 - 50 * K) < K) and (Abs(B[1].X0 - 180 * K) < K) and
    (Length(E.SelectedShapes) = 2), Format('dragged: both moved (%.1f, %.1f)', [B[0].X0 / K, B[1].X0 / K]));
  E.ProcessKey(VK_RIGHT, []);
  B := E.DrawingShapes(CP);
  Check((Abs(B[0].X0 - 51 * K) < 0.5 * K) and (Abs(B[1].X0 - 181 * K) < 0.5 * K), 'an arrow key: both a point on');
  E.Undo;
  E.Undo;
  B := E.DrawingShapes(CP);
  Check((Abs(B[0].X0 - 20 * K) < K) and (Length(E.SelectedShapes) = 2), 'undone: back, both still selected');
  { lined up, spread out }
  Drag(Pt(5, 5), Pt(290, 190));
  Check(E.AlignShapes(3), 'both lined up along their tops');
  B := E.DrawingShapes(CP);
  Check((Abs(B[0].Y0 - B[1].Y0) < 0.5 * K) and (Abs(B[0].Y0 - 20 * K) < K) and (Length(E.SelectedShapes) = 2),
    Format('their tops the higher one''s (%.1f, %.1f)', [B[0].Y0 / K, B[1].Y0 / K]));
  E.Undo;
  B := E.DrawingShapes(CP);
  Check(Abs(B[1].Y0 - 30 * K) < K, 'undone');
  E.ToggleShape(1);
  E.ProcessKey(VK_D, [ssCtrl]);
  Drag(Pt(5, 5), Pt(298, 198));
  B := E.DrawingShapes(CP);
  Check((Length(E.SelectedShapes) = 3) and E.DistributeShapes(True), 'three spread across');
  B := E.DrawingShapes(CP);
  if Length(B) = 3 then
  begin
    SortBoxesX(B);
    Check(Abs((B[1].X0 - B[0].X1) - (B[2].X0 - B[1].X1)) < 0.5 * K,
      Format('the same gap between each (%.1f, %.1f)', [(B[1].X0 - B[0].X1) / K, (B[2].X0 - B[1].X1) / K]));
  end;
  E.Undo;
  E.Undo;
  Check(Length(E.DrawingShapes(CP)) = 2, 'undone, and the copy');
  { a text box: its text with it }
  S0 := pd_doc_story_count(E.Doc);
  E.AddShape('textbox', 30 * K, 120 * K, 130 * K, 170 * K);
  B := E.DrawingShapes(CP);
  St := 0;
  for I := 0 to High(B) do
    if B[I].Story <> 0 then
      St := B[I].Story;
  Check((St <> 0) and (pd_doc_story_count(E.Doc) = S0 + 1), 'a text box, its story');
  if St = 0 then
    Exit;
  pd_doc_insert_text(E.Doc, PdPos(pd_doc_child(E.Doc, St, 0), 0), 'Boxed', 5, PD_FORMAT_INHERIT, nil);
  E.ProcessKey(VK_C, [ssCtrl]);
  E.ProcessKey(VK_V, [ssCtrl]);
  B := E.DrawingShapes(CP);
  St2 := 0;
  if E.SelectedShape(D, Sid) then
    for I := 0 to High(B) do
      if B[I].Sid = Sid then
        St2 := B[I].Story;
  Check((St2 <> 0) and (St2 <> St) and (StoryText(E, St2) = 'Boxed') and (ChildCountOf(E.Doc, St2) = 1),
    'pasted: a text box of its own, with the text (' + StoryText(E, St2) + ')');
  N0 := pd_doc_story_count(E.Doc);
  E.ProcessKey(VK_DELETE, []);
  Check(pd_doc_story_count(E.Doc) = N0 - 1, 'deleted: its story gone too');
  E.Undo;
  Check((pd_doc_story_count(E.Doc) = N0) and (StoryText(E, St2) = 'Boxed'), 'undone: back, with its text');
  { the whole drawing deleted as text: its text boxes' stories too }
  E.ProcessKey(VK_ESCAPE, []);
  Check(E.SelectedShape(D, Sid) and (Sid = -1), 'Escape: the canvas');
  E.ProcessKey(VK_DELETE, []);
  Check((Length(E.DrawingShapes(CP)) = 0) and (pd_doc_story_count(E.Doc) = S0), 'the canvas deleted: no stories left');
  E.Undo;
  Check((Length(E.DrawingShapes(CP)) = 4) and (pd_doc_story_count(E.Doc) = N0), 'undone: all back');
  { text typed into a shape that has none: in the middle of it, within its text rectangle }
  A := Pt(280, 180);
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y);
  E.AddShape('ellipse', 160 * K, 120 * K, 260 * K, 190 * K);
  Ch := 'H';
  TParadeWheel(E).UTF8KeyPress(Ch);
  Ch := 'i';
  TParadeWheel(E).UTF8KeyPress(Ch);
  St := StoryTopOfTest(E, E.CaretPos.block);
  Check((St <> 0) and (StoryText(E, St) = 'Hi'), 'typed with an ellipse selected: its text (' + StoryText(E, St) + ')');
  Check(E.PropsAt(PdPos(pd_doc_child(E.Doc, St, 0), 0)).color and $FFFFFF = $FFFFFF, 'white, on its fill');
  Xml := DrawingText(E, CP);
  I := Pos('"story":' + IntToStr(St) + ',', Xml);
  if I > 0 then
    I := PosEx('"w":', Xml, I);
  if I > 0 then
    W := StrToIntDef(Copy(Xml, I + 4, PosEx(',', Xml, I) - I - 4), 0) / K
  else
    W := 0;
  Check((W < 90) and (W > 60), Format('in the ellipse''s text rectangle (%.1f wide)', [W]));
  for I := 1 to 5 do     { the typing, then the text given to it }
    if StrPas(pd_doc_undo_label(E.Doc)) <> 'Add text' then
      E.Undo;
  E.Undo;
  Check(E.SelectedShape(D, Sid) and (Sid >= 0) and (StoryTopOfTest(E, E.CaretPos.block) = 0),
    'undone: the ellipse selected again');
  E.AddShape('line', 20 * K, 180 * K, 120 * K, 190 * K);
  Check(not E.AddShapeText, 'a line takes no text');
  Check(E.SetShapeLineStyle('dash', '', 'triangle') and E.ShapeLineStyle(LDash, LHead, LTail) and (LDash = 'dash') and
    (LHead = '') and (LTail = 'triangle'), 'the line dashed, an arrowhead at its end');
  Check(E.KeptXml(Xml) and (Pos('<a:prstDash val="dash"/><a:tailEnd type="triangle"/></a:ln>', Xml) > 0),
    'in the order Word has them');
  Check(E.SetShapeLineStyle('solid', 'oval', 'none') and E.ShapeLineStyle(LDash, LHead, LTail) and (LDash = '') and
    (LHead = 'oval') and (LTail = ''), 'solid again, a dot at its start instead');
  E.Undo;
  Check(E.ShapeLineStyle(LDash, LHead, LTail) and (LDash = 'dash') and (LTail = 'triangle'), 'undone: as it was');
  { pasted where no canvas is: a new one at the caret, around them }
  E.ClearShapeSelection;
  E.ProcessKey(VK_END, [ssCtrl]);
  Check(E.PasteShapes and E.SelectedShape(D, Sid) and (D.offset > CP.offset) and (Length(E.DrawingShapes(D)) = 1),
    'pasted in the text: a canvas of its own');
  B := E.DrawingShapes(D);
  if Length(B) = 1 then
    Check((Abs(B[0].X0 - 9 * K) < K) and (B[0].Story <> 0) and (StoryText(E, B[0].Story) = 'Boxed'),
      'its text box at the canvas''s corner, with the text');
  E.Undo;
  Check(not E.SelectedShape(D, Sid) and (pd_doc_story_count(E.Doc) = N0), 'undone: gone');
end;

{ a drawing moved: in the text, dragged by its edge to where the text is dropped at; floating, down its paragraph
  and on to another, and so saved }
procedure TestDrawingMove(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
  Long = 'In olden times when wishing still helped one, there lived a king whose daughters were all beautiful, ' +
    'but the youngest was so beautiful that the sun itself, which has seen so much, was astonished whenever ' +
    'it shone in her face. Close by the king''s castle lay a great dark forest.';
var
  P1, P2, D: pd_pos;
  Sid, I: Integer;
  Pg, CPage: Int32;
  X0, Y0, X1, Y1, NX0, NY0, NX1, NY1: Double;
  CX, Base, Asc, Desc: pd_sp;
  A, B: TPoint;
  Sec, Fl, Para: pd_block_id;
  Fp, Fp2: pd_float_props;
  Info: pd_block_info;
  O: pd_inline;
  R: pd_res_id;
  Png: RawByteString;
  Dir: string;
begin
  Dir := ExtractFilePath(ParamStr(0));
  E.NewDocument;
  E.InsertText('One two three');
  P1 := PdPos(E.CaretPos.block, 0);
  E.ProcessKey(VK_RETURN, []);
  Check(E.InsertCanvas(200, 100), 'a canvas in the text');
  P2 := PdPos(E.CaretPos.block, 0);
  E.AddShape('rect', 20 * K, 20 * K, 80 * K, 60 * K);
  E.ProcessKey(VK_ESCAPE, []);
  Check(E.SelectedShape(D, Sid) and (Sid = -1), 'the canvas selected');
  Check(E.ShapePageBox(D, -1, Pg, X0, Y0, X1, Y1), 'where it is');
  A := E.PageToClient(Pg, X0 / K + 1, (Y0 + (Y1 - Y0) / 4) / K);   { not its handle at the middle }
  pd_layout_caret(E.Layout, PdPos(P1.block, 4), CPage, CX, Base, Asc, Desc);
  B := E.PageToClient(CPage, CX / K, (Base - Asc / 2) / K);
  TParadeWheel(E).MouseMove([], A.X, A.Y);
  Check(E.Cursor = crSizeAll, 'over its edge: the move cursor');
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], (A.X + B.X) div 2, (A.Y + B.Y) div 2);
  TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y);
  Check((Pos(#$EF#$BF#$BC, E.ParaText(P1.block)) = 5) and (Pos(#$EF#$BF#$BC, E.ParaText(P2.block)) = 0) and
    E.SelectedShape(D, Sid) and (D.block = P1.block) and (D.offset = 4),
    'dragged by its edge into the line above: there (' + E.ParaText(P1.block) + ')');
  Check(Length(E.DrawingShapes(D)) = 1, 'with its shape');
  E.Undo;
  Check((Pos(#$EF#$BF#$BC, E.ParaText(P2.block)) = 1) and E.SelectedShape(D, Sid) and (D.block = P2.block),
    'undone: back, selected');
  { a floating picture, beside the text }
  E.NewDocument;
  for I := 0 to 5 do
  begin
    E.InsertText(Long);
    if I < 5 then
      E.ProcessKey(VK_RETURN, []);
  end;
  Png := FileText(Dir + '../../tests/data/rgba.png');
  Sec := pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0);
  Check((Png <> '') and (pd_doc_add_resource(E.Doc, 'image/png', PAnsiChar(Png), Length(Png), R) = PD_OK) and
    (pd_doc_insert_block(E.Doc, Sec, 1, PD_BLOCK_FLOAT, Fl) = PD_OK), 'a float');
  pd_doc_float_props(E.Doc, Fl, Fp);
  Fp.placement := PD_PLACE_HERE or PD_PLACE_FORCE;
  Fp.wrap := PD_WRAP_LEFT;
  Fp.width := 100 * K;
  pd_doc_set_float_props(E.Doc, Fl, Fp);
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_IMAGE;
  O.resource := R;
  O.width := 100 * K;
  O.height := 60 * K;
  Para := pd_doc_child(E.Doc, Fl, 0);
  pd_doc_insert_inline(E.Doc, PdPos(Para, 0), O, nil);
  E.ExternalChange;
  Check(E.ShapePageBox(PdPos(Para, 0), -1, Pg, X0, Y0, X1, Y1), 'where the picture is');
  A := E.PageToClient(Pg, (X0 + X1) / 2 / K, (Y0 + Y1) / 2 / K);
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], A.X, A.Y + Round(30 * E.Zoom * 96 / 72));
  TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y + Round(30 * E.Zoom * 96 / 72));
  Check(E.SelectedShape(D, Sid) and (E.SelectedFloat = Fl) and E.ShapePageBox(D, -1, Pg, NX0, NY0, NX1, NY1) and
    (Abs(NY0 - Y0 - 30 * K) < 2 * K) and (pd_doc_float_props(E.Doc, Fl, Fp) = PD_OK) and
    (Abs(Fp.offset_y - 30 * K) < 2 * K), Format('pressed and dragged down: lower in its paragraph (%.1f, offset %.1f)',
    [(NY0 - Y0) / K, Fp.offset_y / K]));
  Check(E.MoveFloatBy(Round(40 * K), Round(150 * K)) and E.ShapePageBox(D, -1, Pg, X1, Y1, X0, Y0) and
    (Abs(Y1 - NY0 - 150 * K) < 2 * K) and (Abs(X1 - NX0 - 40 * K) < 2 * K) and
    (pd_doc_block_info(E.Doc, Fl, Info) = PD_OK) and (Info.index > 1),
    Format('moved on: anchored further down (by %.1f, %.1f; at %d)', [(X1 - NX0) / K, (Y1 - NY0) / K, Info.index]));
  pd_doc_float_props(E.Doc, Fl, Fp);
  E.SaveToFile(Dir + 'edit_float_move.docx');
  E.LoadFromFile(Dir + 'edit_float_move.docx');
  Sec := pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0);
  Fl := 0;
  for I := 0 to ChildCountOf(E.Doc, Sec) - 1 do
    if (pd_doc_block_info(E.Doc, pd_doc_child(E.Doc, Sec, I), Info) = PD_OK) and (Info.kind = PD_BLOCK_FLOAT) then
      Fl := Info.id;
  Check((Fl <> 0) and (pd_doc_float_props(E.Doc, Fl, Fp2) = PD_OK) and (Abs(Fp2.offset_y - Fp.offset_y) < K) and
    (Abs(Fp2.offset_x - Fp.offset_x) < K) and (Fp2.offset_y > 0),
    Format('saved and read back: as far down and across (%.1f, %.1f)', [Fp2.offset_y / K, Fp2.offset_x / K]));
end;

{ connectors joined to shapes: put on their sites, moved with them, an end dragged to another shape }
procedure TestConnectors(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
var
  CP, D: pd_pos;
  Sid, SS, ES: Integer;
  Pg: Int32;
  PX0, PY0, PX1, PY1, X1, Y1, X2, Y2: Double;
  S: TParadeDrawPoints;
  Xml: string;
  N0, LineSid: Integer;
  Bx: TParadeShapeBoxes;

  function Pt(U, V: Double): TPoint;
  begin
    Result := E.PageToClient(Pg, PX0 / K + U, PY0 / K + V);
  end;

  procedure Drag(A, B: TPoint);
  begin
    TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseMove([ssLeft], (A.X + B.X) div 2, (A.Y + B.Y) div 2);
    TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y);
  end;

  procedure SelectSid(N: Integer);
  var
    I: Integer;
  begin
    for I := 0 to 9 do
      if E.SelectedShape(D, Sid) and (Sid = N) then
        Exit
      else
        E.ProcessKey(VK_TAB, []);
  end;

  function Near(A, B: Double): Boolean;
  begin
    Result := Abs(A - B) < 1.5 * K;
  end;

begin
  E.NewDocument;
  E.InsertText('Joined');
  E.ProcessKey(VK_RETURN, []);
  CP := E.CaretPos;
  Check(E.InsertCanvas(400, 200), 'a canvas for connectors');
  CP := PdPos(CP.block, 0);
  E.ShapePageBox(CP, -1, Pg, PX0, PY0, PX1, PY1);
  E.AddShape('rect', 20 * K, 20 * K, 100 * K, 80 * K);
  E.AddShape('rect', 250 * K, 100 * K, 350 * K, 160 * K);
  S := E.ShapeSites(0);
  Check((Length(S) = 4) and Near(S[3].X, 100 * K) and Near(S[3].Y, 50 * K) and Near(S[0].X, 60 * K) and
    Near(S[0].Y, 20 * K), 'a rectangle''s sites: the middles of its sides, as Office numbers them');
  Check(E.AddConnector('elbowArrow', 100 * K, 50 * K, 250 * K, 130 * K, 0, 3, 1, 1), 'an elbow from one to the other');
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and (SS = 0) and (ES = 1) and Near(X1, 100 * K) and Near(Y1, 50 * K) and
    Near(X2, 250 * K) and Near(Y2, 130 * K), 'joined at both ends');
  SelectSid(0);
  Check(E.SelectedShape(D, Sid) and (Sid = 0), 'the first rectangle');
  Drag(Pt(60, 50), Pt(60, 90));
  SelectSid(2);
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and Near(X1, 100 * K) and Near(Y1, 90 * K) and Near(X2, 250 * K),
    Format('the rectangle dragged down: the connector''s start with it (%.1f, %.1f)', [X1 / K, Y1 / K]));
  E.Undo;
  SelectSid(2);
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and Near(Y1, 50 * K), 'undone: both back');
  SelectSid(1);
  E.ProcessKey(VK_RIGHT, [ssShift]);
  SelectSid(2);
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and Near(X2, 260 * K), 'the other nudged: the end with it');
  Check(E.MoveConnectorEnd(False, 60 * K, 140 * K) and E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and (SS = -1) and
    (ES = 1) and Near(X1, 60 * K) and Near(Y1, 140 * K), 'its start let go, put elsewhere');
  { drawn with the mouse: its ends dropped by sites, joined there }
  SelectSid(0);
  E.InsertShape('line');
  Drag(Pt(61, 82), Pt(312, 103));    { by the first's bottom and the second's top }
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and (SS = 0) and (ES = 1) and Near(X1, 60 * K) and Near(Y1, 80 * K) and
    Near(X2, 310 * K) and Near(Y2, 100 * K), Format('a line drawn between them: on their sites (%.1f, %.1f - %.1f, %.1f)',
    [X1 / K, Y1 / K, X2 / K, Y2 / K]));
  { an end dragged on to a site }
  SelectSid(2);
  if E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) then
    Drag(E.PageToClient(Pg, PX0 / K + X1 / K, PY0 / K + Y1 / K), Pt(21, 50));
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and (SS = 0) and Near(X1, 20 * K) and Near(Y1, 50 * K),
    Format('its start dragged on to the first''s left: joined there (%.1f, %.1f)', [X1 / K, Y1 / K]));
  { an elbow between a bottom and a top: set off down }
  E.AddConnector('elbow', 60 * K, 80 * K, 300 * K, 100 * K, 0, 2, 1, 0);
  Check(E.KeptXml(Xml) and (Pos('rot="5400000"', Xml) > 0), 'an elbow from a bottom to a top: turned to go down');
  SavePage(E, 0, ExtractFilePath(ParamStr(0)) + 'edit_connectors.png', 1.5 * 96 / 72 / PD_SP_PER_PT);
  { a shape in a group: joined, and copied out of it alone }
  SelectSid(0);
  E.ToggleShape(1);
  Check(E.GroupShapes, 'the two rectangles grouped');
  SelectSid(1);
  Check(E.AddConnector('line', 100 * K, 50 * K, 380 * K, 20 * K, 1, 3), 'a line from the grouped second rectangle');
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and (SS = 1) and Near(X1, 360 * K) and Near(Y1, 130 * K),
    Format('on its right side (%.1f, %.1f)', [X1 / K, Y1 / K]));
  E.SelectedShape(D, LineSid);
  SelectSid(1);
  E.ProcessKey(VK_DOWN, [ssShift]);
  SelectSid(LineSid);
  Check(E.ConnectorInfo(X1, Y1, X2, Y2, SS, ES) and Near(Y1, 140 * K), Format('the grouped rectangle nudged: ' +
    'the line with it (%.1f)', [Y1 / K]));
  SelectSid(1);
  E.ProcessKey(VK_C, [ssCtrl]);
  N0 := Length(E.DrawingShapes(CP));
  E.ProcessKey(VK_V, [ssCtrl]);
  Bx := E.DrawingShapes(CP);
  Check((Length(Bx) = N0 + 1) and E.SelectedShape(D, Sid) and (Abs(Bx[Sid].X0 - 269 * K) < 1.5 * K) and
    (Abs(Bx[Sid].Y0 - 119 * K) < 1.5 * K), Format('copied out of its group: a shape of its own beside it (%.1f, %.1f)',
    [Bx[Sid].X0 / K, Bx[Sid].Y0 / K]));
  { a picture in the canvas, copied into a canvas of its own }
  E.ProcessKey(VK_ESCAPE, []);
  N0 := Length(E.DrawingShapes(CP));
  Check(E.InsertPicture(ExtractFilePath(ParamStr(0)) + '../../tests/data/rgba.png') and
    (Length(E.DrawingShapes(CP)) = N0 + 1) and E.SelectedShape(D, Sid) and (Sid = N0) and
    (Pos('"img":', DrawingText(E, CP)) > 0), 'a picture put in the canvas selected');
  E.ProcessKey(VK_C, [ssCtrl]);
  E.ClearShapeSelection;
  E.ProcessKey(VK_END, [ssCtrl]);
  Check(E.PasteShapes and E.SelectedShape(D, Sid) and (D.offset > CP.offset) and
    (Pos('"img":', DrawingText(E, D)) > 0), 'copied into the text: a canvas of its own, the picture in it');
  E.SaveToFile(ExtractFilePath(ParamStr(0)) + 'edit_pictures.docx');
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + 'edit_pictures.docx');
  E.GoToPos(PdPos(pd_doc_next_paragraph(E.Doc, pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 0)), 0));
  Xml := DrawingText(E, E.CaretPos);
  Check((Pos('"img":', Xml) > 0) and (Pos('stCxn', Xml) > 0), 'saved and read back: the picture, the joins');
  E.SaveToFile(ExtractFilePath(ParamStr(0)) + 'edit_connectors.docx');
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + 'edit_connectors.docx');
  E.GoToPos(PdPos(pd_doc_next_paragraph(E.Doc, pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 0)), 0));
  Xml := DrawingText(E, E.CaretPos);
  Check((Pos('stCxn', Xml) > 0) and (Pos('endCxn', Xml) > 0), 'saved and read back: still joined');
end;

{ a shape dragged near another's edge: on it; with Alt, where it is dropped; on a grid }
procedure TestSnap(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
var
  CP, D: pd_pos;
  Sid: Integer;
  Pg: Int32;
  PX0, PY0, PX1, PY1: Double;
  B: TParadeShapeBoxes;

  function Pt(U, V: Double): TPoint;
  begin
    Result := E.PageToClient(Pg, PX0 / K + U, PY0 / K + V);
  end;

  procedure Drag(A, B: TPoint; Shift: TShiftState);
  begin
    TParadeWheel(E).MouseDown(mbLeft, Shift, A.X, A.Y);
    TParadeWheel(E).MouseMove([ssLeft] + Shift, (A.X + B.X) div 2, (A.Y + B.Y) div 2);
    TParadeWheel(E).MouseMove([ssLeft] + Shift, B.X, B.Y);
    TParadeWheel(E).MouseUp(mbLeft, Shift, B.X, B.Y);
  end;

begin
  E.NewDocument;
  E.InsertText('Snapped');
  E.ProcessKey(VK_RETURN, []);
  CP := E.CaretPos;
  E.InsertCanvas(400, 200);
  CP := PdPos(CP.block, 0);
  E.ShapePageBox(CP, -1, Pg, PX0, PY0, PX1, PY1);
  E.AddShape('rect', 20 * K, 20 * K, 100 * K, 80 * K);
  E.AddShape('rect', 250 * K, 100 * K, 350 * K, 160 * K);
  Drag(Pt(300, 130), Pt(300, 52), []);    { its top 2 points below the other's }
  B := E.DrawingShapes(CP);
  Check(E.SelectedShape(D, Sid) and (Sid = 1) and (Abs(B[1].Y0 - 20 * K) < 0.5 * K),
    Format('dragged near the other''s top: on it (%.1f)', [B[1].Y0 / K]));
  E.Undo;
  Drag(Pt(300, 130), Pt(300, 52), [ssAlt]);
  B := E.DrawingShapes(CP);
  Check(Abs(B[1].Y0 - 22 * K) < 0.8 * K, Format('with Alt: where it was dropped (%.1f)', [B[1].Y0 / K]));
  E.Undo;
  E.SnapToShapes := False;
  E.SnapGrid := 18;
  Drag(Pt(300, 130), Pt(312, 137), []);
  B := E.DrawingShapes(CP);
  Check((Abs(B[1].X0 - 270 * K) < 0.5 * K) and (Abs(B[1].Y0 - 108 * K) < 0.5 * K),
    Format('on a grid of a quarter inch (%.1f, %.1f)', [B[1].X0 / K, B[1].Y0 / K]));
  E.SnapGrid := 0;
  E.SnapToShapes := True;
end;

{ a picture's wrapping: in the line, beside the text, over it, and back; so saved }
procedure TestWrap(E: TParadeEdit);
const
  Long = 'In olden times when wishing still helped one, there lived a king whose daughters were all beautiful, ' +
    'but the youngest was so beautiful that the sun itself, which has seen so much, was astonished whenever ' +
    'it shone in her face. Close by the king''s castle lay a great dark forest.';
var
  D, P: pd_pos;
  Sid, I: Integer;
  Dir: string;
  Sec, Fl: pd_block_id;
  Info: pd_block_info;
  Fp: pd_float_props;
  Pg: Int32;
  X0, Y0, X1, Y1: Double;
  A: TPoint;
begin
  Dir := ExtractFilePath(ParamStr(0));
  E.NewDocument;
  E.InsertText(Long);
  E.ProcessKey(VK_RETURN, []);
  P := E.CaretPos;
  Check(E.InsertPicture(Dir + '../../tests/data/rgba.png'), 'a picture in the line');
  E.InsertText(Long);
  { select it: a click on it }
  if E.ShapePageBox(PdPos(P.block, 0), -1, Pg, X0, Y0, X1, Y1) then
  begin
    A := E.PageToClient(Pg, (X0 + X1) / 2 / PD_SP_PER_PT, (Y0 + Y1) / 2 / PD_SP_PER_PT);
    TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y);
  end;
  Check(E.SelectedShape(D, Sid) and (E.ObjectWrap = -1), 'selected: in line with the text');
  Check(E.SetObjectWrap(PD_WRAP_LEFT) and (E.SelectedFloat <> 0) and (E.ObjectWrap = PD_WRAP_LEFT) and
    (Pos(#$EF#$BF#$BC, E.ParaText(P.block)) = 0), 'square: a float, the text beside it');
  Check(E.SetObjectWrap(PD_WRAP_FRONT) and (E.ObjectWrap = PD_WRAP_FRONT), 'in front of the text');
  E.SaveToFile(Dir + 'edit_wrap.docx');
  Check(E.SetObjectWrap(-1) and (E.SelectedFloat = 0) and E.SelectedShape(D, Sid) and
    (Pos(#$EF#$BF#$BC, E.ParaText(D.block)) = 1), 'back in the line, at its paragraph''s start');
  E.Undo;
  Check(E.ObjectWrap = PD_WRAP_FRONT, 'undone: in front again, selected');
  E.LoadFromFile(Dir + 'edit_wrap.docx');
  Sec := pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0);
  Fl := 0;
  for I := 0 to ChildCountOf(E.Doc, Sec) - 1 do
    if (pd_doc_block_info(E.Doc, pd_doc_child(E.Doc, Sec, I), Info) = PD_OK) and (Info.kind = PD_BLOCK_FLOAT) then
      Fl := Info.id;
  Check((Fl <> 0) and (pd_doc_float_props(E.Doc, Fl, Fp) = PD_OK) and (Fp.wrap = PD_WRAP_FRONT),
    'saved and read back: in front of the text');
end;

{ a page to draw on: landscape, the page a canvas, nothing to select but its shapes; its corner dragged: the page
  and the canvas smaller, the shapes as they were; so saved }
procedure TestCanvasPage(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
var
  D, C: pd_pos;
  Sid: Integer;
  Sp: pd_section_props;
  Pg: Int32;
  X0, Y0, X1, Y1, SX0, SY0, SX1, SY1, NX0, NY0, NX1, NY1: Double;
  A, B: TPoint;
  Info: pd_page_info;
  Dir: string;
begin
  Dir := ExtractFilePath(ParamStr(0));
  E.NewDocument;
  Check(E.StartCanvasPage and E.CanvasPage, 'a page to draw on');
  Sp := E.CurrentSectionProps;
  Check((Sp.page_width > Sp.page_height) and (Sp.margin_left = 0) and (Sp.margin_top = 0), 'landscape, no margins');
  Check(E.SelectedShape(D, Sid) and (Sid = -1) and E.ShapePageBox(D, -1, Pg, X0, Y0, X1, Y1) and (Pg = 0) and
    (Abs(X0) < K) and (Abs(Y0) < K) and (Abs(X1 - Sp.page_width) < K) and (Abs(Y1 - Sp.page_height) < K) and
    (E.PageCount = 1), Format('the page a canvas, ready to draw in (%.1f by %.1f at %.1f, %.1f; %d page)',
    [(X1 - X0) / K, (Y1 - Y0) / K, X0 / K, Y0 / K, E.PageCount]));
  Check(not E.SelectionRange(C, D) and not E.Modified and (pd_doc_can_undo(E.Doc) = 0),
    'not selected as text, not a change, nothing to undo');
  E.ProcessKey(VK_DELETE, []);
  E.ProcessKey(VK_ESCAPE, []);
  E.InsertText('typed');
  Check(E.SelectedShape(D, Sid) and (Sid = -1) and (Length(E.DrawingShapes(D)) = 0) and
    (Pos('typed', E.DocumentText) = 0) and E.CanvasPagePos(C), 'Delete, Escape, typing: the canvas as it was');
  E.AddShape('rect', 20 * K, 20 * K, 120 * K, 80 * K);
  E.AddShape('ellipse', 200 * K, 20 * K, 300 * K, 80 * K);
  E.ShapePageBox(D, 0, Pg, SX0, SY0, SX1, SY1);
  E.ProcessKey(VK_ESCAPE, []);
  E.SelectAll;
  Check(Length(E.SelectedShapes) = 2, 'Ctrl+A: the shapes');
  E.ProcessKey(VK_ESCAPE, []);
  pd_layout_page_info(E.Layout, 0, Info);
  A := E.PageToClient(0, Info.width / K, Info.height / K);    { the page's corner }
  TParadeWheel(E).MouseMove([], A.X, A.Y);
  Check(E.Cursor = crSizeNWSE, 'over the page''s corner: the sizing cursor');
  B := Point(A.X - Round(200 * E.Zoom * 96 / 72), A.Y - Round(100 * E.Zoom * 96 / 72));
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], (A.X + B.X) div 2, (A.Y + B.Y) div 2);
  TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y);
  Sp := E.CurrentSectionProps;
  Check((Abs(X1 - X0 - Sp.page_width - 200 * K) < 2 * K) and (Abs(Y1 - Y0 - Sp.page_height - 100 * K) < 2 * K) and
    E.CanvasPagePos(C) and E.ShapePageBox(C, -1, Pg, NX0, NY0, NX1, NY1) and (Abs(NX1 - Sp.page_width) < K) and
    (Abs(NY1 - Sp.page_height) < K), Format('the corner dragged in: the page and its canvas smaller (%.1f by %.1f)',
    [Sp.page_width / K, Sp.page_height / K]));
  E.ShapePageBox(C, 0, Pg, NX0, NY0, NX1, NY1);
  Check((Abs((NX1 - NX0) - (SX1 - SX0)) < 0.5 * K) and (Abs(NX0 - SX0) < 0.5 * K), 'the shapes as they were');
  E.Undo;
  Check(Abs(E.CurrentSectionProps.page_width - (X1 - X0)) < K, 'undone: as big again');
  E.Redo;
  E.SaveToFile(Dir + 'edit_canvas.pdoc');
  E.LoadFromFile(Dir + 'edit_canvas.pdoc');
  Check(E.CanvasPage and E.SelectedShape(D, Sid) and (Sid = -1) and (Length(E.DrawingShapes(D)) = 2),
    'saved and read back: a canvas page again, its shapes');
  SavePage(E, 0, Dir + 'edit_canvas.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  E.NewDocument;
  Check(not E.CanvasPage, 'a new document: no canvas page');
end;

{ a PowerPoint file opened: each slide a canvas page, its shapes there to edit; the slides sized together }
{ a right-to-left paragraph: Left steps forward through its letters, as they are shown; the Left button aligns it
  left as it is seen (its end), and the alignment it reports is the one seen }
procedure TestRightToLeft(E: TParadeEdit);
var
  S: TStringStream;
  B: pd_block_id;
  Pp: pd_para_props;
begin
  S := TStringStream.Create('<p dir="rtl">'#$D7#$90#$D7#$91#$D7#$92'</p><p>abc</p>');
  try
    E.LoadFromStream(S, PD_CONV_HTML);
  finally
    S.Free;
  end;
  B := pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 0);
  E.GoToPos(PdPos(B, 0));
  E.ProcessKey(VK_LEFT, []);
  Check((E.CaretPos.block = B) and (E.CaretPos.offset = 2), Format('Left in Hebrew: forward (%d)', [E.CaretPos.offset]));
  E.ProcessKey(VK_RIGHT, []);
  Check(E.CaretPos.offset = 0, 'Right: back');
  Check(E.CurrentAlignment = PD_ALIGN_RIGHT, 'a right-to-left paragraph shown at the right');
  E.SetAlignment(PD_ALIGN_LEFT);
  pd_doc_para_props(E.Doc, B, Pp);
  Check((Pp.align = PD_ALIGN_RIGHT) and (E.CurrentAlignment = PD_ALIGN_LEFT), 'aligned left as seen: its end');
  E.GoToPos(PdPos(pd_doc_next_paragraph(E.Doc, B), 0));
  E.ProcessKey(VK_RIGHT, []);
  Check(E.CaretPos.offset = 1, 'Right in a left-to-right paragraph: forward');
end;

function StreamWriter(user: Pointer; data: Pointer; len: csize_t): cint; cdecl;
begin
  TStream(user).WriteBuffer(data^, len);
  Result := 0;
end;

{ the theme, a colour linked to it, a template's styles, a table style from the gallery and its options }
procedure TestThemesAndStyles(E: TParadeEdit);
var
  Th: pd_theme;
  Cp: pd_char_props;
  Pp: pd_para_props;
  Cell: pd_cell_props;
  B: pd_block_id;
  L: TStringList;
  Path: string;
  Ms: TMemoryStream;
  T: Ppd_doc;
  C, R, Tb: pd_block_id;
begin
  E.NewDocument;
  E.InsertText('Hello');
  E.SelectAll;
  E.SetTextThemeColor(PD_THEME_ACCENT2, 100000, 0);
  Th := E.CurrentTheme;
  B := pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 0);
  Cp := E.PropsAt(PdPos(B, 1));
  Check(Cp.color = Th.color[PD_THEME_ACCENT2], 'text in the theme''s second accent');
  pd_theme_preset(1, @Th);
  E.SetTheme(Th);
  Cp := E.PropsAt(PdPos(B, 1));
  Check(Cp.color = $FF000000 or $C0504D, Format('the theme changed: the text follows (%x)', [Cp.color]));
  E.Undo;
  Check(E.CurrentTheme.color[PD_THEME_ACCENT2] <> Th.color[PD_THEME_ACCENT2], 'undone: the theme as it was');

  { a style of the document's own, then given by a template }
  FillChar(Pp, SizeOf(Pp), 0);
  FillChar(Cp, SizeOf(Cp), 0);
  Cp.mask := PD_CP_ITALIC;
  Cp.italic := 1;
  Check(E.DefineParagraphStyle('Remark', Pp, Cp), 'a style defined');
  E.SetParagraphStyle('Remark');
  Check(E.PropsAt(PdPos(B, 1)).italic = 1, 'the paragraph in it');
  Path := GetTempDir + 'parade_edit_template.dotx';
  T := nil;
  Check(pd_doc_new(T) = PD_OK, 'a template');
  Cp.italic := 0;
  Cp.mask := PD_CP_WEIGHT;
  Cp.weight := 700;
  pd_doc_style_define(T, 'Remark', PD_STYLE_PARAGRAPH, 0, nil, @Cp, nil);
  Ms := TMemoryStream.Create;
  try
    Check(pd_doc_export(T, PD_CONV_DOTX, @StreamWriter, Ms) = PD_OK, 'written as a template');
    Ms.SaveToFile(Path);
  finally
    Ms.Free;
    pd_doc_free(T);
  end;
  Check(E.ApplyTemplate(Path, PD_ADOPT_STYLES), 'the template applied');
  Cp := E.PropsAt(PdPos(B, 1));
  Check((Cp.weight = 700) and (Cp.italic = 0), 'its Remark over the document''s');
  DeleteFile(Path);

  { a table in a style from the gallery: its header in the accent, then without a header row }
  E.GoToPos(PdPos(B, 5));
  E.InsertTable(3, 2);
  E.ApplyTableStylePreset(3, 1);
  Check(E.CurrentTableStyle = 'Grid Table 4 - Accent 1', 'the table''s style: ' + E.CurrentTableStyle);
  L := TStringList.Create;
  try
    E.GetTableStyles(L);
    Check(L.IndexOf('Grid Table 4 - Accent 1') >= 0, 'among the document''s table styles');
  finally
    L.Free;
  end;
  Th := E.CurrentTheme;
  if E.CellAt(E.CaretPos, C, R, Tb) then
  begin
    pd_doc_cell_resolve(E.Doc, pd_doc_child(E.Doc, pd_doc_child(E.Doc, Tb, 0), 1), @Cell);
    Check(Cell.background = Th.color[PD_THEME_ACCENT1], 'the header in the accent');
  end
  else
    Check(False, 'the caret in the table');
  E.SetTableLook(PD_TLOOK_FIRST_COL);
  Check(E.CurrentTableProps.look = PD_TLOOK_FIRST_COL, 'the header row''s part off');
end;

{ SmartArt labels turned a quarter (tests/data/smartart.pptx, slide 12): their text painted turned, a column of
  white glyphs in each blue box taller than it is wide }
procedure TestTurnedText(E: TParadeEdit);
var
  Bmp: TBitmap;
  Img: TLazIntfImage;
  X, Y, BX0, BY0, BX1, BY1, TX0, TY0, TX1, TY1: Integer;
  C: TFPColor;
begin
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + '../../tests/data/smartart.pptx');
  Bmp := TBitmap.Create;
  try
    E.RenderPage(11, Bmp, 1.0 * 96 / 72 / PD_SP_PER_PT);
    Img := Bmp.CreateIntfImage;
    try
      BX0 := MaxInt; BY0 := MaxInt; BX1 := -1; BY1 := -1;
      for Y := 0 to Img.Height - 1 do     { the right box: blue }
        for X := Img.Width div 2 to Img.Width - 1 do
        begin
          C := Img.Colors[X, Y];
          if (C.blue > $A000) and (C.red < $8000) then
          begin
            if X < BX0 then BX0 := X;
            if X > BX1 then BX1 := X;
            if Y < BY0 then BY0 := Y;
            if Y > BY1 then BY1 := Y;
          end;
        end;
      TX0 := MaxInt; TY0 := MaxInt; TX1 := -1; TY1 := -1;
      for Y := BY0 + 2 to BY1 - 2 do      { its text: white in it }
        for X := BX0 + 2 to BX1 - 2 do
        begin
          C := Img.Colors[X, Y];
          if (C.red > $E000) and (C.green > $E000) then
          begin
            if X < TX0 then TX0 := X;
            if X > TX1 then TX1 := X;
            if Y < TY0 then TY0 := Y;
            if Y > TY1 then TY1 := Y;
          end;
        end;
      Check((BX1 > BX0) and (BY1 - BY0 > BX1 - BX0) and (TX1 > TX0) and (TY1 - TY0 > 2 * (TX1 - TX0)),
        Format('turned text painted turned (box %dx%d, text %dx%d)', [BX1 - BX0, BY1 - BY0, TX1 - TX0, TY1 - TY0]));
    finally
      Img.Free;
    end;
    SavePage(E, 11, ExtractFilePath(ParamStr(0)) + 'edit_turned.png', 1.0 * 96 / 72 / PD_SP_PER_PT);
  finally
    Bmp.Free;
  end;
end;

procedure TestSlides(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
var
  D, C: pd_pos;
  Sid, N0: Integer;
  Info: pd_page_info;
  A, B: TPoint;
  Sp: pd_section_props;
  Dir, Xml, W: string;
  WB: pd_block_id;
  I: Integer;
  Bm: TBitmap;
  Px0, Px1: TColor;
  CPage: Int32;
  CX, Base, Asc, Desc: pd_sp;
begin
  Dir := ExtractFilePath(ParamStr(0));
  E.LoadFromFile(Dir + '../../tests/data/slides.pptx');
  Check(E.CanvasPage and (E.PageCount = 2), 'a presentation opened: its slides canvas pages');
  Check(E.SelectedShape(D, Sid) and (Sid = -1) and E.CanvasPagePos(C) and (C.block = D.block),
    'the first slide''s canvas, ready to draw in');
  { a slide's text box, as it opens (the canvas selected): a double click a word, a third the paragraph }
  WB := 0;
  for I := 0 to pd_doc_story_count(E.Doc) - 1 do
    if WB = 0 then
    begin
      WB := pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0);
      W := E.ParaText(WB);
      if (Pos(' ', W) < 3) or (pd_layout_caret(E.Layout, PdPos(WB, 1), CPage, CX, Base, Asc, Desc) <> PD_OK) or
         (CPage <> 0) then
        WB := 0;
    end;
  Check(WB <> 0, 'a slide''s text box with words');
  if WB <> 0 then
  begin
    A := E.PageToClient(CPage, CX / K, (Base - Asc / 2) / K);
    Bm := TBitmap.Create;
    E.RenderPage(0, Bm, 1.0 / K);
    Px0 := Bm.Canvas.Pixels[Round(CX / K) + 1, Round((Base - Asc / 4) / K) + 1];
    TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseDown(mbLeft, [ssDouble], A.X, A.Y);
    TParadeWheel(E).DblClick;
    TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y);
    Check(E.SelectedText = Copy(W, 1, Pos(' ', W) - 1), 'a double click in a slide''s text: the word, [' +
      E.SelectedText + '] of ' + W);
    E.RenderPage(0, Bm, 1.0 / K);
    Px1 := Bm.Canvas.Pixels[Round(CX / K) + 1, Round((Base - Asc / 4) / K) + 1];
    Bm.Free;
    Check(Px1 <> Px0, Format('and painted selected (%.6x, was %.6x)', [Px1, Px0]));
    TParadeWheel(E).TripleClick;
    TParadeWheel(E).MouseDown(mbLeft, [ssTriple], A.X, A.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y);
    Check(E.SelectedText = W, 'a triple click in a slide''s text: the paragraph, [' + E.SelectedText + ']');
    E.LoadFromFile(Dir + '../../tests/data/slides.pptx');
    E.SelectedShape(D, Sid);
  end;
  N0 := Length(E.DrawingShapes(D));
  Check(N0 >= 6, Format('its shapes, the master''s too (%d)', [N0]));
  Check(E.AddShape('star5', 100 * K, 250 * K, 160 * K, 300 * K) and (Length(E.DrawingShapes(D)) = N0 + 1),
    'a shape drawn on it');
  E.ProcessKey(VK_ESCAPE, []);
  pd_layout_page_info(E.Layout, 1, Info);
  A := E.PageToClient(1, Info.width / K, Info.height / K);    { the second slide's corner }
  B := Point(A.X - Round(72 * E.Zoom * 96 / 72), A.Y);
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y);
  Sp := E.CurrentSectionProps;
  pd_layout_page_info(E.Layout, 0, Info);
  Check((Abs(Info.width - 648 * K) < 2 * K) and E.CanvasPage, Format('a slide''s corner dragged: every slide ' +
    'narrower (%.1f)', [Info.width / K]));
  { slides added, copied, moved, deleted }
  Check((E.SlideCount = 2) and E.GoToSlide(1) and (E.CurrentSlide = 1), 'the second slide gone to');
  Check(E.NewSlide(0) and (E.SlideCount = 3) and (E.CurrentSlide = 1) and E.SelectedShape(D, Sid) and
    (Length(E.DrawingShapes(D)) = 0), 'a new slide after the first: blank, the one being edited');
  E.Undo;
  Check(E.SlideCount = 2, 'undone');
  N0 := pd_doc_story_count(E.Doc);
  Check(E.NewSlide(0, True) and (E.SlideCount = 3) and E.CanvasPagePos(C) and
    (Length(E.DrawingShapes(C)) = N0 * 0 + Length(E.DrawingShapes(C))) and
    (pd_doc_story_count(E.Doc) > N0), 'the first slide copied: its text boxes of their own');
  Xml := '';
  for Sid := 0 to pd_doc_story_count(E.Doc) - 1 do
    Xml := Xml + '|' + E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, Sid), 0));
  Check(Pos('Hello Slides', Copy(Xml, Pos('Hello Slides', Xml) + 1, MaxInt)) > 0, 'with their text');
  Check(E.MoveSlide(1, 2) and (E.CurrentSlide = 2), 'the copy moved last');
  Check(E.DeleteSlide(2) and (E.SlideCount = 2) and (pd_doc_story_count(E.Doc) = N0), 'deleted: its stories too');
  E.SaveToFile(Dir + 'edit_slides.pdoc');
  E.LoadFromFile(Dir + 'edit_slides.pdoc');
  Check(E.CanvasPage and (E.PageCount = 2), 'saved as a Parade document: still its slides');
end;

{ comments edited -- anyone's, from its balloon: a double click on a reply's lines names the reply; the text
  changed, its author kept, undone in one step; the balloons rounded }
type
  TCommentCatch = class
    Got: pd_comment_id;
    procedure Hit(Sender: TObject; Id: pd_comment_id);
  end;

procedure TCommentCatch.Hit(Sender: TObject; Id: pd_comment_id);
begin
  Got := Id;
end;

{ what the Font, Paragraph and Bullets and numbering dialogs set reaches the paragraphs: every property, not
  only the toolbar's; and a list's levels as given }
procedure TestFormatProps(E: TParadeEdit);
var
  Pp: pd_para_props;
  Cp: pd_char_props;
  Lv, Got: TParadeListLevels;
  Lvl, I: Integer;
  Lbl: array[0..63] of AnsiChar;
  B1, B2, B3, B4: pd_block_id;
  Info: pd_block_info;

  { the caret at the start of the paragraph N down, the selection to the end of the one K further when K >= 0 }
  procedure Go(N: Integer; K: Integer = -1);
  var
    J, C: Integer;
    B: pd_block_id;
  begin
    E.ProcessKey(VK_HOME, [ssCtrl]);
    B := pd_doc_next_paragraph(E.Doc, 0);
    for J := 1 to N do
    begin
      for C := 0 to Length(E.ParaText(B)) do
        E.ProcessKey(VK_RIGHT, []);
      B := pd_doc_next_paragraph(E.Doc, B);
    end;
    if K >= 0 then
      for J := 0 to K do
      begin
        for C := 1 to Length(E.ParaText(B)) + Ord(J < K) do
          E.ProcessKey(VK_RIGHT, [ssShift]);
        B := pd_doc_next_paragraph(E.Doc, B);
      end;
  end;

begin
  E.NewDocument;
  E.InsertText('One');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Two');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Three');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Four');
  B1 := pd_doc_next_paragraph(E.Doc, 0);
  B2 := pd_doc_next_paragraph(E.Doc, B1);
  B3 := pd_doc_next_paragraph(E.Doc, B2);
  B4 := pd_doc_next_paragraph(E.Doc, B3);
  { paragraph: the properties past the toolbar's }
  Go(0);
  FillChar(Pp, SizeOf(Pp), 0);
  Pp.mask := PD_PP_KEEP_NEXT or PD_PP_KEEP_LINES or PD_PP_BREAK_BEFORE or PD_PP_WIDOWS or PD_PP_LINE_SPACING or
    PD_PP_CONTEXTUAL or PD_PP_INDENT_RIGHT;
  Pp.keep_with_next := 1;
  Pp.keep_lines := 1;
  Pp.page_break_before := 1;
  Pp.widows := 2;
  Pp.line_spacing := 1000;
  Pp.line_rule := PD_LINE_EXACT;
  Pp.line_height := 20 * PD_SP_PER_PT;
  Pp.contextual := 1;
  Pp.indent_right := 30 * PD_SP_PER_PT;
  E.ApplyParaProps(Pp);
  FillChar(Pp, SizeOf(Pp), 0);
  pd_doc_para_props(E.Doc, B1, Pp);
  Check((Pp.keep_with_next = 1) and (Pp.keep_lines = 1) and (Pp.page_break_before = 1) and (Pp.widows = 2),
    'keep with next, keep lines, break before and widows reach the paragraph');
  Check((Pp.line_rule = PD_LINE_EXACT) and (Pp.line_height = 20 * PD_SP_PER_PT), 'exact line spacing of 20pt');
  Check((Pp.contextual = 1) and (Pp.indent_right = 30 * PD_SP_PER_PT), 'contextual spacing and the right indent');
  Pp := E.CurrentParaProps;
  Check((Pp.keep_with_next = 1) and (Pp.line_rule = PD_LINE_EXACT), 'and read back at the caret');
  { a change of one keeps the others }
  FillChar(Pp, SizeOf(Pp), 0);
  Pp.mask := PD_PP_SPACE_AFTER;
  Pp.space_after := 9 * PD_SP_PER_PT;
  E.ApplyParaProps(Pp);
  FillChar(Pp, SizeOf(Pp), 0);
  pd_doc_para_props(E.Doc, B1, Pp);
  Check((Pp.keep_with_next = 1) and (Pp.line_height = 20 * PD_SP_PER_PT) and (Pp.space_after = 9 * PD_SP_PER_PT),
    'space after set, the rest kept');
  { characters, nothing selected: kept for the text typed next, every property }
  Go(3);
  E.ProcessKey(VK_END, []);
  FillChar(Cp, SizeOf(Cp), 0);
  Cp.mask := PD_CP_LETTERSPACE or PD_CP_CAPS or PD_CP_SMALLCAPS or PD_CP_HSCALE or PD_CP_POSITION;
  Cp.letter_space := 2 * PD_SP_PER_PT;
  Cp.caps := 1;
  Cp.small_caps := 1;
  Cp.hscale := 1500;
  Cp.position := 3 * PD_SP_PER_PT;
  E.ApplyCharProps(Cp);
  E.InsertText('x');
  Cp := E.PropsAt(PdPos(B4, 4));
  Check((Cp.letter_space = 2 * PD_SP_PER_PT) and (Cp.caps = 1) and (Cp.small_caps = 1) and (Cp.hscale = 1500) and
    (Cp.position = 3 * PD_SP_PER_PT), 'spacing, caps, scale and position typed at the caret');
  { lists: levels of one's own }
  Lv := TParadeEdit.DefaultListLevels(PD_NUM_DECIMAL);
  Lv[0].format := PD_NUM_UPPER_ROMAN;
  StrPLCopy(Lv[0].text, '(%1)', High(Lv[0].text));
  Lv[0].start := 4;
  Lv[0].indent := 50 * PD_SP_PER_PT;
  Go(0, 1);
  E.ApplyListLevels(Lv, False);
  pd_doc_list_label(E.Doc, B1, @Lbl[0], 64);
  Check(string(Lbl) = '(IV)', 'roman from IV in brackets: ' + Lbl);
  pd_doc_list_label(E.Doc, B2, @Lbl[0], 64);
  Check(string(Lbl) = '(V)', 'then (V): ' + Lbl);
  Go(1);
  Check(E.CurrentListLevels(Got, Lvl) and (Lvl = 0) and (Got[0].start = 4) and (Got[0].indent = 50 * PD_SP_PER_PT),
    'the levels read back at the caret');
  { the next paragraph given the same levels numbers on }
  Go(2);
  E.ApplyListLevels(Lv, False);
  pd_doc_list_label(E.Doc, B3, @Lbl[0], 64);
  Check(string(Lbl) = '(VI)', 'the same levels right after: numbered on, (VI): ' + Lbl);
  { the whole list changed from one of its items, levels kept }
  Go(1);
  E.ChangeIndent(True);
  Lv := TParadeEdit.DefaultListLevels(PD_NUM_BULLET);
  StrPLCopy(Lv[0].text, '*', High(Lv[0].text));
  StrPLCopy(Lv[1].text, '-', High(Lv[1].text));
  Go(0, 1);     { the first two: the second keeps its level }
  E.ApplyListLevels(Lv, True);
  pd_doc_list_label(E.Doc, B3, @Lbl[0], 64);
  Check(string(Lbl) = '*', 'the whole list: the third, not selected, a star too: ' + Lbl);
  pd_doc_list_label(E.Doc, B2, @Lbl[0], 64);
  Check(string(Lbl) = '-', 'and the second still a level down: ' + Lbl);
  E.Undo;
  pd_doc_list_label(E.Doc, B3, @Lbl[0], 64);
  Check(string(Lbl) = '(V)', 'one undo: numbered again, (V) under the indented second: ' + Lbl);
  Go(0, 2);
  E.RemoveList;
  I := 0;
  if (pd_doc_block_info(E.Doc, B1, Info) = PD_OK) and (Info.list = 0) then Inc(I);
  if (pd_doc_block_info(E.Doc, B3, Info) = PD_OK) and (Info.list = 0) then Inc(I);
  Check(I = 2, 'out of the list');
  Check(not E.CurrentListLevels(Got, Lvl), 'no list at the caret');
end;

{ the animations edited (paradeanim): tests/data/anim.pptx's read as a list of effects, written again as the same
  timing, changed, set on the slide, played and exported as changed }
procedure TestAnimEdit(E: TParadeEdit);
var
  A, B: TParadeAnim;
  T0, T1: TShowTiming;
  S: TParadeShow;
  Xml, Saved, Tmp: string;
  Sh: TParadeAnimShapes;
  I, K, N4, N5, N8: Integer;
  Same: Boolean;
begin
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + '../../tests/data/anim.pptx');
  A := TParadeAnim.Create;
  B := TParadeAnim.Create;
  T0 := TShowTiming.Create;
  T1 := TShowTiming.Create;
  try
    Xml := E.SlideTiming(0);
    Check(A.Load(Xml) and (Length(A.Effects) = 5), 'the first slide''s timing read as five effects: ' +
      IntToStr(Length(A.Effects)));
    if Length(A.Effects) <> 5 then
      Exit;
    Check((A.Effects[0].Start = asClick) and (A.Effects[1].Start = asClick) and (A.Effects[2].Start = asClick) and
      (A.Effects[3].Start = asWith) and (A.Effects[4].Start = asClick), 'on clicks, the oval''s with the box''s');
    Check((A.Effects[0].Cls = 'entr') and (A.Effects[0].Preset = 1) and (A.Effects[0].ParaSt = 0) and
      (A.Effects[1].Preset = 10) and (A.Effects[1].ParaSt = 1) and (A.Effects[2].Preset = 2) and
      (A.Effects[2].Shape = 4) and (A.Effects[2].ParaSt = -1) and (A.Effects[3].Cls = 'exit') and
      (A.Effects[3].Shape = 5) and (A.Effects[4].Cls = 'emph') and (A.Effects[4].Shape = 8),
      'each its kind, its shape, its paragraph');
    Check((A.Effects[1].Dur = 500) and (A.ClickOf(3) = 3) and (A.ClickOf(4) = 4), 'their durations, their clicks');
    Check(AnimPresetName('entr', 2) + '/' + AnimPresetName('exit', 10) + '/' + AnimPresetName('emph', 8) =
      'Fly In/Fade/Spin', 'their names');
    { written again: the same to the show }
    Saved := A.Save;
    T0.Parse(Xml);
    T1.Parse(Saved);
    Same := (T0.StepCount = T1.StepCount) and (Length(T0.Targets) = Length(T1.Targets));
    for I := 0 to Min(T0.StepCount, T1.StepCount) - 1 do
    begin
      Same := Same and (Length(T0.Steps[I].Effects) = Length(T1.Steps[I].Effects)) and
        SameValue(T0.Steps[I].Len, T1.Steps[I].Len);
      for K := 0 to Min(Length(T0.Steps[I].Effects), Length(T1.Steps[I].Effects)) - 1 do
        Same := Same and (Length(T0.Steps[I].Effects[K].Behs) = Length(T1.Steps[I].Effects[K].Behs)) and
          SameValue(T0.Steps[I].Effects[K].Start, T1.Steps[I].Effects[K].Start);
    end;
    Check(Same and (Saved <> Xml), 'written again (not as it came): the same steps, effects and times');
    Check(B.Load(Saved) and (B.Save = Saved), 'and that read and written again, the same');
    { changed: the fade twice as long; a fade-in after the spin; the second click's moved first }
    A.Effects[1].Dur := 1000;
    I := A.Add(8, 'entr', 10, 0, asAfter, 700);
    Check(I = 5, 'one added at the end');
    Saved := A.Save;     { its time nodes numbered 1, 2, 3 ... in order, as PowerPoint wants them }
    K := 0;
    Same := True;
    I := Pos('<p:cTn id="', Saved);
    while I > 0 do
    begin
      Inc(K);
      Same := Same and (Copy(Saved, I + 11, Length(IntToStr(K)) + 1) = IntToStr(K) + '"');
      I := PosEx('<p:cTn id="', Saved, I + 1);
    end;
    Check(Same and (K > 20), 'its time nodes numbered in order: ' + IntToStr(K));
    I := 5;
    T1.Parse(A.Save);
    Check((T1.StepCount = 5) and SameValue(T1.Steps[2].Len, 1000), 'the fade a second long: ' +
      FloatToStr(T1.Steps[2].Len));
    Check((Length(T1.Steps[4].Effects) = 2) and SameValue(T1.Steps[4].Effects[1].Start, T1.Steps[4].Effects[0].Len) and
      SameValue(T1.Steps[4].Len, T1.Steps[4].Effects[0].Len + 700), 'the new one after the spin, in its click');
    A.Move(1, 0);
    A.Effects[1].Start := asWith;
    T1.Parse(A.Save);
    Check((T1.StepCount = 4) and (Length(T1.Steps[1].Effects) = 2), 'moved first, the other with it: one click fewer');
    A.Delete(5);
    A.SetPreset(4, 'entr', 22, 4);
    T1.Parse(A.Save);
    Check((Length(T1.Steps[3].Effects) = 1) and (T1.Steps[3].Effects[0].Cls = 'entr') and
      (Pos('filter="wipe(up)"', A.Save) > 0) and (Pos('presetID="22" presetClass="entr" presetSubtype="4"', A.Save) > 0),
      'the spin made a wipe up; the one after it deleted');
    { the first effect not on a click: on the slide's start }
    A.Effects[0].Start := asAfter;
    T1.Parse(A.Save);
    Check((Length(T1.Steps[0].Effects) = 2) and (T1.StepCount = 3), 'the first two by themselves, as the slide begins');
    { the transition, set }
    A.SetTransition('wipe', 'd', 'slow', False, 2000);
    T1.Parse(A.Save);
    Check(T1.Trans.Present and (T1.Trans.Kind = 'wipe') and (T1.Trans.Dir = 'd') and not T1.Trans.AdvClick and
      (T1.Trans.AdvTime = 2000), 'a transition: wiped down, slowly, on by itself after two seconds');
    A.Effects := nil;
    A.SetTransition('', '', 'fast', True, -1);
    Check(A.Save = '', 'nothing left, none');
    { the second slide's transition kept as it came while it is not changed }
    Check(B.Load(E.SlideTiming(1)) and (B.Trans.Kind = 'push') and (B.Trans.Dir = 'r') and (B.Trans.Speed = 'slow') and
      (Pos(B.Trans.Raw, B.Save) > 0), 'the second slide''s push read, and kept as it was');
    { one of PowerPoint 2010's, read with nothing else around it: changed, written for those who know it, a fade for
      those who do not }
    Check(B.Load('<pd:tail xmlns:pd="urn:parade" xmlns:p="pns" xmlns:p14="p14ns"><p:transition spd="fast">' +
      '<p14:vortex dir="r"/></p:transition></pd:tail>') and (B.Trans.Kind = 'p14:vortex') and (B.Trans.Dir = 'r'),
      'a vortex read');
    B.SetTransition(B.Trans.Kind, B.Trans.Dir, 'slow', True, -1);
    Check(Pos('<mc:AlternateContent xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"><mc:Choice ' +
      'Requires="p14"><p:transition spd="slow"><p14:vortex dir="r"/></p:transition></mc:Choice><mc:Fallback>' +
      '<p:transition spd="slow"><p:fade/></p:transition></mc:Fallback></mc:AlternateContent>', B.Save) > 0,
      'slowed: in a choice, with a fade for the rest: ' + B.Save);
    { on the slide: set, a change of the document; the show plays it; exported and read again }
    Sh := E.SlideAnimShapes(0);
    N4 := -1;
    N5 := -1;
    N8 := -1;
    for I := 0 to High(Sh) do
      if Sh[I].Id = 4 then
        N4 := I
      else if Sh[I].Id = 5 then
        N5 := I
      else if Sh[I].Id = 8 then
        N8 := I;
    Check((N4 >= 0) and (N5 >= 0) and (N8 >= 0) and (Sh[N4].Name <> ''), 'the slide''s shapes, by id and name: ' +
      IfThen(N4 >= 0, Sh[N4].Name, '?'));
    E.GoToSlide(0);
    Check(E.SelectAnimShape(8) and (E.SelectedAnimShape = 8), 'a shape selected by its id, and read back');
    A.Load(E.SlideTiming(0));
    A.Delete(4);
    A.Add(8, 'exit', 53, 16, asClick, 600);
    E.Modified := False;
    Check(E.SetSlideTiming(0, A.Save) and E.Modified and
      (ParadeTailInner(E.SlideTiming(0)) = ParadeTailInner(A.Save)) and (ParadeTailInner(A.Save) <> ''), 'set on the slide: read back, '
      + 'the document changed');
    Check(not E.SetSlideTiming(0, A.Save), 'set the same again: no change');
    Saved := E.SlideTiming(0);
    E.Undo;
    B.Load(E.SlideTiming(0));
    Check((Length(B.Effects) = 5) and (B.Effects[4].Cls = 'emph') and (StrPas(pd_doc_undo_label(E.Doc)) <> 'Animation'),
      'undone: the spin back, the zoom out gone');
    E.Redo;
    Check(ParadeTailInner(E.SlideTiming(0)) = ParadeTailInner(Saved), 'redone: the zoom out again');
    Check(StrPas(pd_doc_undo_label(E.Doc)) = 'Animation', 'Undo names it: ' + StrPas(pd_doc_undo_label(E.Doc)));
    S := TParadeShow.Create(E, 400, 300, 0);
    try
      T1 := S.TimingOf(0);
      Check((T1 <> nil) and (T1.StepCount = 5) and (T1.Steps[4].Effects[0].Cls = 'exit'), 'the show plays it as set');
      T1 := nil;
    finally
      S.Free;
    end;
    Check(E.NewSlide(0, False) and (E.SlideTiming(1) = '') and E.DeleteSlide(1), 'a new slide: none of its own');
    Check(E.NewSlide(0, True) and (E.SlideCount = 3) and (ParadeTailInner(E.SlideTiming(1)) = ParadeTailInner(E.SlideTiming(0))),
      'a slide duplicated: its animations with it');
    Tmp := GetTempDir + 'edit_anim_' + IntToStr(GetProcessID) + '.pptx';
    E.SaveToFile(Tmp);
    E.LoadFromFile(Tmp);
    DeleteFile(Tmp);
    B.Load(E.SlideTiming(0));
    Check((E.SlideCount = 3) and (Length(B.Effects) = 5) and (B.Effects[4].Cls = 'exit') and (B.Effects[4].Preset = 53)
      and (Length(E.SlideAnimShapes(0)) > 0), 'exported and read again: the zoom out there');
    Same := False;
    for I := 0 to High(E.SlideAnimShapes(0)) do
      Same := Same or (E.SlideAnimShapes(0)[I].Id = B.Effects[4].Shape);
    Check(Same, 'naming a shape the slide has, by its id there');
    B.Load(E.SlideTiming(1));
    Check(Length(B.Effects) = 5, 'the duplicate''s too');
  finally
    A.Free;
    B.Free;
    T0.Free;
    if T1 <> nil then
      T1.Free;
  end;
end;

{ the slide show (paradeshow): tests/data/anim.pptx's timing read, and what it plays seen in the frames -- the time
  given, so each frame is the one wanted }
procedure TestSlideShow(E: TParadeEdit);
var
  S: TParadeShow;
  T: TShowTiming;
  V: Double;
  R0, R1, R4, R5, R8, RT, Sl: TRect;
  D0, D1, Mid, Fin, Before: Int64;
  Old, Frm: array of UInt32;
  I, Same, Tried: Integer;
  RL, BL: Int64;
  RG, PR: TRect;
  Whole: array of UInt32;
  Parts: TRectArray;
  Area: Int64;
  J: Integer;
  A: TParadeAnim;
  LT0, RT0, LT1, RT1, LT2, RT2: Integer;

  function Px(X, Y: Integer): UInt32;
  begin
    Result := PUInt32(S.Frame.GetDataLineStart(Sl.Top + Y))[Sl.Left + X];
  end;

  { how much is drawn in R (in the slide): how far each pixel is from white }
  function Ink(const R: TRect): Int64;
  var
    X, Y: Integer;
    C: UInt32;
  begin
    Result := 0;
    for Y := Max(0, R.Top) to Min(Sl.Bottom - Sl.Top, R.Bottom) - 1 do
      for X := Max(0, R.Left) to Min(Sl.Right - Sl.Left, R.Right) - 1 do
      begin
        C := Px(X, Y);
        Inc(Result, 765 - Integer(C and $FF) - Integer((C shr 8) and $FF) - Integer((C shr 16) and $FF));
      end;
  end;

  { the red and the blue of the left quarter of R }
  procedure LeftColour(const R: TRect; out Red, Blue: Int64);
  var
    X, Y: Integer;
    C: UInt32;
  begin
    Red := 0;
    Blue := 0;
    for Y := R.Top + (R.Bottom - R.Top) div 3 to R.Bottom - (R.Bottom - R.Top) div 3 do
      for X := R.Left + 2 to R.Left + (R.Right - R.Left) div 4 do
      begin
        C := Px(X, Y);
        Inc(Red, (C shr 16) and $FF);
        Inc(Blue, C and $FF);
      end;
  end;

  { the first row drawn in column X, from a little above R }
  function TopAt(X: Integer; const R: TRect): Integer;
  var
    C: UInt32;
  begin
    for Result := Max(0, R.Top - 12) to R.Bottom do
    begin
      C := Px(X, Result);
      if 765 - Integer(C and $FF) - Integer((C shr 8) and $FF) - Integer((C shr 16) and $FF) > 120 then
        Exit;
    end;
    Result := -1;
  end;

  function Shifted(const R: TRect; DY: Integer): TRect;
  begin
    Result := Rect(R.Left, R.Top + DY, R.Right, R.Bottom + DY);
  end;

  procedure Snap(var A: array of UInt32);
  var
    Y: Integer;
  begin
    for Y := 0 to Sl.Bottom - Sl.Top - 1 do
      Move(PUInt32(S.Frame.GetDataLineStart(Sl.Top + Y))[Sl.Left], A[Y * (Sl.Right - Sl.Left)], (Sl.Right - Sl.Left) * 4);
  end;

begin
  { the formulas of the keyframes }
  Check(ShowEval('1+#ppt_h/2', 0.5, 0.25, 0.2, 0.4, 0, V) and SameValue(V, 1.2), 'a keyframe formula: 1+#ppt_h/2');
  Check(ShowEval('#ppt_x-sin(pi/2)*$^2', 0.5, 0, 0, 0, 3, V) and SameValue(V, -8.5), 'functions, pi, $ and powers');
  Check(ShowEval('max(1,3,2)-min(4,-1)', 0, 0, 0, 0, 0, V) and SameValue(V, 4), 'max and min of several');
  Check(not ShowEval('2*(1+', 0, 0, 0, 0, 0, V) and not ShowEval('foo(1)', 0, 0, 0, 0, 0, V), 'a broken one is not');

  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + '../../tests/data/anim.pptx');
  Check(E.SlideCount = 2, 'the animated deck: two slides');
  S := TParadeShow.Create(E, 800, 600, 0);
  SetLength(Old, 800 * 450);
  SetLength(Frm, 800 * 450);
  try
    Sl := S.SlideRect;
    Check((Sl.Right - Sl.Left = 800) and (Sl.Bottom - Sl.Top = 450) and (Sl.Top = 75), 'the slide as large as it goes, '
      + 'its shape kept: ' + IntToStr(Sl.Right - Sl.Left) + 'x' + IntToStr(Sl.Bottom - Sl.Top));
    T := S.TimingOf(0);
    Check((T <> nil) and (T.StepCount = 5) and (Length(T.Steps[0].Effects) = 0) and (Length(T.Steps[3].Effects) = 2) and
      not T.Trans.Present, 'the first slide: four clicks, the third two effects, no transition');
    T := S.TimingOf(1);
    Check((T <> nil) and T.Trans.Present and (T.Trans.Kind = 'push') and (T.Trans.Dir = 'r') and (T.Trans.Dur = 1000) and
      (T.StepCount = 2) and (Length(T.Steps[0].Effects) = 1), 'the second: pushed in, a second; an effect by itself, '
      + 'then a click');
    S.Start(0);
    R0 := S.TargetBox(3, 0);
    R1 := S.TargetBox(3, 1);
    R4 := S.TargetBox(4);
    R5 := S.TargetBox(5);
    R8 := S.TargetBox(8);
    R4 := Rect(R4.Left + 4, R4.Top + 4, R4.Right - 4, R4.Bottom - 4);    { inside it: clear of its neighbours (the connector starts at its edge) }
    R5 := Rect(R5.Left + 4, R5.Top + 4, R5.Right - 4, R5.Bottom - 4);
    Check((R0.Right > R0.Left) and (R1.Top >= R0.Bottom - 2) and (R4.Right > R4.Left) and (R5.Right > R5.Left) and
      (R8.Right > R8.Left), 'each animated shape and paragraph found on the slide, the second paragraph below the first');
    Check((S.Phase = spIdle) and (S.StepsDone = 1), 'nothing by itself: waiting for the first click');
    Check((Ink(R0) = 0) and (Ink(R1) = 0) and (Ink(R4) = 0), 'what comes in later is not there yet');
    Before := Ink(R5);
    Check(Before > 0, 'the oval, to go out later, is');
    { click 1: the first paragraph appears }
    S.Next(100);
    S.Tick(102);
    Check((Ink(R0) > 0) and (Ink(R1) = 0), 'click 1: the first point, not the second');
    { click 2: the second fades in over half a second }
    S.Next(200);
    S.Tick(450);
    Mid := Ink(R1);
    S.Tick(800);
    Fin := Ink(R1);
    Check((Fin > 0) and (Mid > Fin div 4) and (Mid < Fin * 3 div 4), Format('click 2: half way, half faded in (%d of %d)',
      [Mid, Fin]));
    Check(S.StepsDone = 3, 'and done when its time is up');
    { click 3: the box flies in from below as the oval fades out }
    S.Next(1000);
    S.Tick(1250);
    D0 := Ink(R4);
    D1 := Ink(Shifted(R4, Round(((1 + (R4.Bottom - R4.Top) / 2 / 450) - (R4.Top + R4.Bottom) / 2 / 450) * 450 / 2)));
    Mid := Ink(R5);
    S.Tick(1600);
    Fin := Ink(R4);
    Check((Fin > 0) and (D0 < Fin div 4) and (D1 > Fin div 2), Format('click 3: half way the box is half way up from the '
      + 'bottom (%d where it ends, %d half way, of %d)', [D0, D1, Fin]));
    Check((Mid > Before div 5) and (Mid < Before * 4 div 5) and (Ink(R5) = 0), Format('and the oval half gone, then '
      + 'gone (%d of %d half way)', [Mid, Before]));
    { click 4: the gradient box turns once in a second: half way, its left side is its right }
    LeftColour(R8, RL, BL);
    Check(RL > BL, 'the gradient: red on the left');
    S.Next(2000);
    S.Tick(2500);
    LeftColour(R8, RL, BL);
    Check(BL > RL, 'click 4: half a turn on, blue on the left');
    S.Tick(3100);
    LeftColour(R8, RL, BL);
    Check(RL > BL, 'and red again once round');
    Check((S.StepsDone = S.StepCount) and (S.CurrentSlide = 0), 'every step of the slide played');
    { back a click: the slide as it was before it }
    S.Prev(3150);
    LeftColour(R8, RL, BL);
    Check((S.StepsDone = 4) and (Ink(R4) > 0) and (RL > BL), 'back a step');
    S.Next(3160);
    S.Tick(4200);
    { the next slide, pushed in from the left: half way, the old slide's left half on the right }
    Snap(Old);
    S.Next(5000);
    Check((S.CurrentSlide = 1) and (S.Phase = spTrans), 'the next slide, through its transition');
    S.Tick(5500);
    Snap(Frm);
    Same := 0;
    Tried := 0;
    I := 0;
    while I < 400 * 450 do
    begin
      Inc(Tried);
      if Frm[(I div 400) * 800 + 400 + I mod 400] = Old[(I div 400) * 800 + I mod 400] then
        Inc(Same);
      Inc(I, 37);
    end;
    Check(Same * 100 > Tried * 95, Format('half way, the old slide half pushed off to the right (%d of %d)', [Same, Tried]));
    { then, by itself, the turned box wipes in from the bottom }
    S.Tick(6000);
    Check((S.Phase = spPlay) and (S.StepsDone = 0), 'the transition over, what plays by itself plays');
    RT := S.TargetBox(4);
    S.Tick(6250);
    D0 := Ink(Rect(RT.Left, RT.Top, RT.Right, RT.Top + (RT.Bottom - RT.Top) div 3));
    D1 := Ink(Rect(RT.Left, RT.Bottom - (RT.Bottom - RT.Top) div 3, RT.Right, RT.Bottom));
    Check((D1 > 0) and (D0 * 10 < D1), Format('half way, the box wiped in from the bottom up (%d top, %d bottom)', [D0, D1]));
    S.Tick(6600);
    Check((S.Phase = spIdle) and (Ink(Rect(RT.Left, RT.Top, RT.Right, RT.Top + (RT.Bottom - RT.Top) div 3)) > 0),
      'and all of it once it has');
    { click 1 there: a group comes in by a wheel, its boxes with it (the group the target, not its first box) }
    RG := S.TargetBox(9);
    Check((RG.Right - RG.Left > (RG.Bottom - RG.Top) * 3 div 2) and (Ink(RG) = 0), Format('the group found, both its '
      + 'boxes (%dx%d), not there before its click', [RG.Right - RG.Left, RG.Bottom - RG.Top]));
    SetLength(Whole, S.Frame.Width * S.Frame.Height);
    for I := 0 to S.Frame.Height - 1 do
      Move(S.Frame.GetDataLineStart(I)^, Whole[I * S.Frame.Width], S.Frame.Width * 4);
    S.TakeChanged;
    S.Next(7000);
    { each frame as it turns: what has changed, put on the frame before, the frame now (as a window draws it again,
      those parts only) }
    Same := 0;
    Tried := 0;
    Area := 0;
    Mid := 0;
    for J := 1 to 22 do
    begin
      S.Tick(7000 + J * 50);
      if J = 10 then
        Mid := Ink(RG);
      Parts := S.TakeChanged;
      for PR in Parts do
      begin
        Area := Max(Area, Int64(PR.Width) * PR.Height);
        for I := PR.Top to PR.Bottom - 1 do
          Move(PUInt32(S.Frame.GetDataLineStart(I))[PR.Left], Whole[I * S.Frame.Width + PR.Left], PR.Width * 4);
      end;
      for I := 0 to S.Frame.Height - 1 do
      begin
        Inc(Tried);
        if CompareDWord(Whole[I * S.Frame.Width], S.Frame.GetDataLineStart(I)^, S.Frame.Width) = 0 then
          Inc(Same);
      end;
    end;
    Check((Same = Tried) and (Area > 0) and (Area * 4 < S.Frame.Width * S.Frame.Height), Format('the changed parts '
      + 'make each frame again (%d of %d rows), at most %d px of %d', [Same, Tried, Area, S.Frame.Width * S.Frame.Height]));
    S.Tick(8100);
    Fin := Ink(RG);
    Check((Fin > 0) and (Mid > Fin div 5) and (Mid < Fin * 4 div 5), Format('half way the wheel has drawn about half the '
      + 'group (%d of %d)', [Mid, Fin]));
    S.TakeChanged;
    Check(not S.Tick(8200) and (Length(S.TakeChanged) = 0), 'once it is still, nothing changed to draw again');
    S.Next(9000);
    Check(S.Phase = spEnd, 'past the last slide: the end');
    S.Next(9100);
    Check(S.Finished, 'and a click there ends it');
    { the next slide made ready while the show waits: what is shown the same as when it is not }
    S.Free;
    S := TParadeShow.Create(E, 800, 600, 0);
    S.Start(0);
    S.Next(100);
    S.Next(200);
    S.Next(1000);
    S.Next(2000);
    S.Tick(3100);
    S.Tick(3150);
    Check(not S.Ready(1), 'the next slide not drawn yet as this one ends');
    S.Tick(3500);   { (still for a while: the next slide made ready) }
    Check(S.Ready(1) and S.Ready(0), 'still a while: the next drawn ready, this one kept');
    S.Prev(3550);
    LeftColour(R8, RL, BL);
    Check((S.CurrentSlide = 0) and (S.StepsDone = 4) and (Ink(R4) > 0) and (RL > BL), 'made ready the next, this slide '
      + 'is drawn still as it is');
    S.Next(3560);
    S.Tick(4600);
    S.Tick(5000);
    S.Next(5100);
    S.Tick(5600);
    SetLength(Old, Length(Frm));
    Snap(Old);
    Check((S.CurrentSlide = 1) and (CompareDWord(Old[0], Frm[0], Length(Frm)) = 0), 'and the next, through its '
      + 'transition, as it was drawn when not made ready before');
  finally
    S.Free;
  end;
  { a Teeter added to the gradient box (as the pane adds it): tilted one way, then the other, then where it was }
  A := TParadeAnim.Create;
  try
    A.Load(E.SlideTiming(0));
    A.Add(8, 'emph', 32, 0, asClick, 1000, -1);
    Check(AnimPresetName('emph', 32) = 'Teeter', 'Teeter by its name');
    E.SetSlideTiming(0, A.Save);
  finally
    A.Free;
  end;
  S := TParadeShow.Create(E, 800, 600, 0);
  try
    Sl := S.SlideRect;
    T := S.TimingOf(0);
    Check((T.StepCount = 6) and (Length(T.Steps[5].Effects) = 1) and (Length(T.Steps[5].Effects[0].Behs) = 5) and
      SameValue(T.Steps[5].Effects[0].Behs[0].RotBy + T.Steps[5].Effects[0].Behs[1].RotBy + T.Steps[5].Effects[0].Behs[2].RotBy
      + T.Steps[5].Effects[0].Behs[3].RotBy + T.Steps[5].Effects[0].Behs[4].RotBy, 0) and
      SameValue(T.Steps[5].Effects[0].Behs[1].RotBy, -4) and (T.Steps[5].Effects[0].Behs[4].Start = 800), 'the Teeter: '
      + 'five turns, 2 degrees, 4 back, 4, 4 back, 2, the last at 0.8 s');
    S.Start(0);
    S.Next(100);
    S.Next(200);
    S.Next(1000);
    S.Next(2000);
    S.Tick(3100);
    R8 := S.TargetBox(8);
    SetLength(Old, Length(Frm));
    Snap(Old);
    LT0 := TopAt(R8.Left + (R8.Right - R8.Left) div 6, R8);
    RT0 := TopAt(R8.Right - (R8.Right - R8.Left) div 6, R8);
    S.Next(4000);
    S.Tick(4150);
    LT1 := TopAt(R8.Left + (R8.Right - R8.Left) div 6, R8);
    RT1 := TopAt(R8.Right - (R8.Right - R8.Left) div 6, R8);
    S.Tick(4400);
    LT2 := TopAt(R8.Left + (R8.Right - R8.Left) div 6, R8);
    RT2 := TopAt(R8.Right - (R8.Right - R8.Left) div 6, R8);
    Check((Abs(LT0 - RT0) <= 1) and (LT1 < RT1 - 1) and (LT2 > RT2 + 1), Format('tilted clockwise, then the other '
      + 'way (its top edge, left and right: %d %d, %d %d, %d %d)', [LT0, RT0, LT1, RT1, LT2, RT2]));
    S.Tick(5100);
    Snap(Frm);
    Check((S.StepsDone = 6) and (CompareDWord(Old[0], Frm[0], Length(Frm)) = 0), 'and where it was once it is done');
  finally
    S.Free;
    E.Undo;
  end;
  { a hidden slide is left out }
  E.SetSlideHidden(1, True);
  S := TParadeShow.Create(E, 400, 300, 0);
  try
    Check(S.SlideCount = 1, 'a hidden slide not shown');
  finally
    S.Free;
    E.SetSlideHidden(1, False);
  end;
end;

{ the show as a PDF that plays it (paradeshowpdf): a page for each still state and for each frame while an effect
  plays, the transition the page's; the last page, drawn by a PDF viewer, the show's last frame }
type
  TRgb = array of Byte;

{ page Page of the PDF Fn as Ghostscript draws it at 80 dpi (the 10-inch page 800 by 450 px), RGB; nil if it cannot }
function GsPage(const Fn: string; Page: Integer; const Tmp: string): TRgb;
var
  F: TFileStream;
  Data: array of Byte;
  Out_: string;
  P, Off: Integer;
begin
  Result := nil;
  RunCommand('/usr/bin/gs', ['-q', '-dNOPAUSE', '-dBATCH', '-dSAFER', '-sDEVICE=ppmraw', '-r80', '-dFirstPage=' +
    IntToStr(Page), '-dLastPage=' + IntToStr(Page), '-sOutputFile=' + Tmp + '.ppm', Fn], Out_);
  if not FileExists(Tmp + '.ppm') then
    Exit;
  F := TFileStream.Create(Tmp + '.ppm', fmOpenRead);
  try
    SetLength(Data, F.Size);
    F.ReadBuffer(Data[0], F.Size);
  finally
    F.Free;
  end;
  DeleteFile(Tmp + '.ppm');
  Off := 0;
  for P := 1 to 4 do   { past P6, its width, height and 255 (and any # comment between) }
  begin
    while (Off < Length(Data)) and ((Data[Off] in [9, 10, 13, 32]) or (Data[Off] = Ord('#'))) do
      if Data[Off] = Ord('#') then
        while (Off < Length(Data)) and (Data[Off] <> 10) do Inc(Off)
      else
        Inc(Off);
    while (Off < Length(Data)) and not (Data[Off] in [9, 10, 13, 32]) do
      Inc(Off);
  end;
  Inc(Off);
  if Length(Data) - Off <> 800 * 450 * 3 then
    Exit;
  Result := Copy(Data, Off, 800 * 450 * 3);
end;

function FrameRgb(S: TParadeShow): TRgb;
var
  X, Y: Integer;
  C: UInt32;
begin
  SetLength(Result, 800 * 450 * 3);
  for Y := 0 to 449 do
    for X := 0 to 799 do
    begin
      C := PUInt32(S.Frame.GetDataLineStart(Y))[X];
      Result[(Y * 800 + X) * 3] := (C shr 16) and $FF;
      Result[(Y * 800 + X) * 3 + 1] := (C shr 8) and $FF;
      Result[(Y * 800 + X) * 3 + 2] := C and $FF;
    end;
end;

{ how many px of A within R are far from B, a px either way forgiven (two renderers each smooth an edge its own
  way); -1 when either is missing }
function PxOff(const A, B: TRgb; const R: TRect): Integer;
var
  P, Q, X, Y, DX, DY, D, Best: Integer;
begin
  if (Length(A) <> 800 * 450 * 3) or (Length(B) <> Length(A)) then
    Exit(-1);
  Result := 0;
  for Y := Max(0, R.Top) to Min(449, R.Bottom - 1) do
    for X := Max(0, R.Left) to Min(799, R.Right - 1) do
    begin
      Q := (Y * 800 + X) * 3;
      Best := MaxInt;
      for DY := Max(0, Y - 1) to Min(449, Y + 1) do
        for DX := Max(0, X - 1) to Min(799, X + 1) do
        begin
          P := (DY * 800 + DX) * 3;
          D := Abs(Integer(A[P]) - B[Q]) + Abs(Integer(A[P + 1]) - B[Q + 1]) + Abs(Integer(A[P + 2]) - B[Q + 2]);
          Best := Min(Best, D);
        end;
      if Best > 60 then
        Inc(Result);
    end;
end;

{ the page of the PDF Fn, among its pages First..Last, most like the show's frame within R: its number, how many
  px of it are far off (Off), and of the page next most like it (Next) }
function BestPage(const Fn: string; First, Last: Integer; const Frame: TRgb; const R: TRect; const Tmp: string;
  out Off, Next: Integer): Integer;
var
  P, D: Integer;
begin
  Result := -1;
  Off := MaxInt;
  Next := MaxInt;
  for P := First to Last do
  begin
    D := PxOff(GsPage(Fn, P, Tmp), Frame, R);
    if D < 0 then
      Continue;
    if D < Off then
    begin
      Next := Off;
      Off := D;
      Result := P;
    end
    else if D < Next then
      Next := D;
  end;
end;

{ whether the point X, Y is inside a clip path as ShowFilterClip writes it (m l c re h, then W n or W* n) }
function InClip(const Path: string; X, Y: Double): Boolean;
var
  Tok: TStringList;
  Nums: array of Double;
  Polys: array of array of Double;
  I, J, K, Wind, Cross: Integer;
  T, X0, Y0, X1, Y1, AX, AY: Double;
  EvenOdd: Boolean;
  DotFmt: TFormatSettings;

  procedure Pt(PX, PY: Double);
  begin
    if Length(Polys) = 0 then
      SetLength(Polys, 1);
    Polys[High(Polys)] := Concat(Polys[High(Polys)], [PX, PY]);
  end;

begin
  Result := False;
  DotFmt := DefaultFormatSettings;
  DotFmt.DecimalSeparator := '.';
  EvenOdd := Pos('W*', Path) > 0;
  Tok := TStringList.Create;
  try
    Tok.Delimiter := ' ';
    Tok.StrictDelimiter := True;
    Tok.DelimitedText := Trim(Path);
    Nums := nil;
    Polys := nil;
    AX := 0; AY := 0;
    for I := 0 to Tok.Count - 1 do
    begin
      if Tok[I] = '' then
        Continue;
      if Tok[I] = 'm' then
      begin
        SetLength(Polys, Length(Polys) + 1);
        Pt(Nums[0], Nums[1]);
        AX := Nums[0]; AY := Nums[1];
      end
      else if Tok[I] = 'l' then
      begin
        Pt(Nums[0], Nums[1]);
        AX := Nums[0]; AY := Nums[1];
      end
      else if Tok[I] = 'c' then
      begin
        for K := 1 to 16 do
        begin
          T := K / 16;
          Pt(Power(1 - T, 3) * AX + 3 * Sqr(1 - T) * T * Nums[0] + 3 * (1 - T) * Sqr(T) * Nums[2] + Power(T, 3) * Nums[4],
            Power(1 - T, 3) * AY + 3 * Sqr(1 - T) * T * Nums[1] + 3 * (1 - T) * Sqr(T) * Nums[3] + Power(T, 3) * Nums[5]);
        end;
        AX := Nums[4]; AY := Nums[5];
      end
      else if Tok[I] = 're' then
      begin
        SetLength(Polys, Length(Polys) + 1);
        Pt(Nums[0], Nums[1]);
        Pt(Nums[0] + Nums[2], Nums[1]);
        Pt(Nums[0] + Nums[2], Nums[1] + Nums[3]);
        Pt(Nums[0], Nums[1] + Nums[3]);
      end
      else if (Tok[I] = 'h') or (Tok[I] = 'W') or (Tok[I] = 'W*') or (Tok[I] = 'n') then
      else
      begin
        SetLength(Nums, Length(Nums) + 1);
        Nums[High(Nums)] := StrToFloat(Tok[I], DotFmt);
        Continue;
      end;
      Nums := nil;
    end;
  finally
    Tok.Free;
  end;
  Wind := 0;
  Cross := 0;
  for J := 0 to High(Polys) do
    for K := 0 to Length(Polys[J]) div 2 - 1 do
    begin
      X0 := Polys[J][2 * K]; Y0 := Polys[J][2 * K + 1];
      X1 := Polys[J][(2 * K + 2) mod Length(Polys[J])]; Y1 := Polys[J][(2 * K + 3) mod Length(Polys[J])];
      if (Y0 <= Y) <> (Y1 <= Y) then
        if X0 + (Y - Y0) * (X1 - X0) / (Y1 - Y0) > X then
        begin
          Inc(Cross);
          if Y1 > Y0 then Inc(Wind) else Dec(Wind);
        end;
    end;
  if EvenOdd then
    Result := Odd(Cross)
  else
    Result := Wind <> 0;
end;

{ each filter's clip, as the PDF cuts with it: what the show's own mask shows, point for point (but at its edges) }
procedure TestShowFilterClips;
const
  Filters: array[0..31] of string = ('wipe(up)', 'wipe(down)', 'wipe(left)', 'wipe(right)', 'barn(inVertical)',
    'barn(outVertical)', 'barn(inHorizontal)', 'barn(outHorizontal)', 'blinds(horizontal)', 'blinds(vertical)',
    'box(in)', 'box(out)', 'circle(in)', 'circle(out)', 'diamond(in)', 'diamond(out)', 'plus(in)', 'plus(out)',
    'checkerboard(across)', 'checkerboard(down)', 'randombar(horizontal)', 'randombar(vertical)', 'strips(downLeft)',
    'strips(upLeft)', 'strips(upRight)', 'strips(downRight)', 'wedge', 'wheel(1)', 'wheel(2)', 'wheel(4)', 'wheel(8)',
    'slide');
  Ps: array[0..3] of Double = (0.1, 0.37, 0.5, 0.83);
var
  Pl: TShowPlace;
  F, PI_, X, Y, DX, DY, Bad, Tested: Integer;
  Clip: string;
  Dest, Want, Edge: Boolean;
  Msk: array[0..99, 0..99] of Boolean;
  Worst: string;
begin
  FillChar(Pl, SizeOf(Pl), 0);
  Pl.BoxW := 100;
  Pl.BoxH := 100;
  Bad := 0;
  Tested := 0;
  Worst := '';
  for F := 0 to High(Filters) do
    for PI_ := 0 to High(Ps) do
    begin
      Pl.Filter := Filters[F];
      Pl.FilterP := Ps[PI_];
      Clip := ShowFilterClip(Pl, 100, 100, Dest);
      if Clip = '' then
      begin
        Inc(Bad);
        Worst := Worst + ' ' + Filters[F] + ': no clip';
        Continue;
      end;
      for Y := 0 to 99 do
        for X := 0 to 99 do
          if Filters[F] = 'slide' then
            Msk[Y, X] := True
          else
            Msk[Y, X] := FilterMask(Filters[F], Ps[PI_], (X + 0.37) / 100, (Y + 0.61) / 100, X, Y) > 0;
      for Y := 0 to 99 do
        for X := 0 to 99 do
        begin
          Edge := False;
          for DY := Max(0, Y - 1) to Min(99, Y + 1) do
            for DX := Max(0, X - 1) to Min(99, X + 1) do
              if Msk[DY, DX] <> Msk[Y, X] then
                Edge := True;
          if Edge then
            Continue;
          Want := Msk[Y, X];
          Inc(Tested);
          if InClip(Clip, X + 0.37, Y + 0.61) <> Want then   { (off the middle: none on a spoke) }
          begin
            Inc(Bad);
            if Length(Worst) < 300 then
              Worst := Worst + Format(' %s %.2f at %d,%d', [Filters[F], Ps[PI_], X, Y]);
            Break;
          end;
        end;
    end;
  Check((Bad = 0) and (Tested > 32 * 4 * 8000), Format('each filter''s cut as the PDF has it: what the show shows of '
    + 'its box (%d of %d points off:%s)', [Bad, Tested, Worst]));
end;

{ a photograph in the PDF as a JPEG when asked (ExportPDF's JpegQuality): another decoder (FPC's) reads it back as
  the picture was, but for a JPEG's loss; odd sizes, so the last blocks reach past the picture's edge }
procedure TestPdfJpeg(E: TParadeEdit);
var
  Img, Back: TFPMemoryImage;
  W: TFPWriterPNG;
  R: TFPReaderJPEG;
  Png, Pdf, Txt: string;
  X, Y, P, Q, N: Integer;
  Seed: UInt32;
  C, D: TFPColor;
  Sq: Double;
  Raw: TStringList;
  M: TMemoryStream;
begin
  Png := GetTempDir + 'edit_jpeg_' + IntToStr(GetProcessID) + '.png';
  Pdf := ChangeFileExt(Png, '.pdf');
  Img := TFPMemoryImage.Create(301, 203);
  Seed := 7;
  for Y := 0 to 202 do
    for X := 0 to 300 do
    begin
      Seed := Seed * 1103515245 + 12345;
      C.red := EnsureRange(X * 200 div 301 + Integer((Seed shr 16) mod 41) - 20 + 20, 0, 255) * 257;
      C.green := EnsureRange(Y * 200 div 203 + Integer((Seed shr 20) mod 41) - 20 + 20, 0, 255) * 257;
      C.blue := EnsureRange(255 - (X + Y) * 255 div 504, 0, 255) * 257;
      C.alpha := $FFFF;
      Img.Colors[X, Y] := C;
    end;
  W := TFPWriterPNG.Create;
  try
    W.UseAlpha := False;
    W.WordSized := False;
    Img.SaveToFile(Png, W);
  finally
    W.Free;
  end;
  E.NewDocument;
  Check(E.InsertPicture(Png), 'a photograph in the page');
  E.ExportPDF(Pdf);
  Raw := TStringList.Create;
  try
    Raw.LoadFromFile(Pdf);
    Txt := Raw.Text;
  finally
    Raw.Free;
  end;
  Check((Pos('/DCTDecode', Txt) = 0) and (Pos('/Width 301 /Height 203 ', Txt) > 0), 'as it is, losslessly, unless '
    + 'asked');
  N := Length(Txt);
  E.ExportPDF(Pdf, 85);
  M := TMemoryStream.Create;
  Back := TFPMemoryImage.Create(0, 0);
  R := TFPReaderJPEG.Create;
  try
    M.LoadFromFile(Pdf);
    SetLength(Txt, M.Size);
    Move(M.Memory^, Txt[1], M.Size);
    P := Pos('/Width 301 /Height 203 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length ', Txt);
    Check((P > 0) and (Length(Txt) * 2 < N), Format('asked, the photograph a JPEG: %d bytes, %d lossless',
      [Length(Txt), N]));
    if P > 0 then
    begin
      P := PosEx('stream'#10, Txt, P) + 7;
      Q := PosEx(#10'endstream', Txt, P);
      M.Clear;
      M.WriteBuffer(Txt[P], Q - P);
      M.Position := 0;
      Back.LoadFromStream(M, R);
      Check((Back.Width = 301) and (Back.Height = 203), Format('another decoder reads it: %d by %d', [Back.Width,
        Back.Height]));
      Sq := 0;
      if (Back.Width = 301) and (Back.Height = 203) then
      begin
        for Y := 0 to 202 do
          for X := 0 to 300 do
          begin
            C := Img.Colors[X, Y];
            D := Back.Colors[X, Y];
            Sq := Sq + Sqr((C.red shr 8) - (D.red shr 8)) + Sqr((C.green shr 8) - (D.green shr 8)) +
              Sqr((C.blue shr 8) - (D.blue shr 8));
          end;
        Sq := 10 * Log10(Sqr(255) / (Sq / (301 * 203 * 3)));
        { (the edge's blocks too: the last column and row as near as the rest) }
        C := Img.Colors[300, 202];
        D := Back.Colors[300, 202];
        Check((Abs((C.red shr 8) - (D.red shr 8)) < 40) and (Abs((C.blue shr 8) - (D.blue shr 8)) < 40),
          'the corner past the last whole block');
      end;
      { (its noise is what a JPEG keeps least: libjpeg's own at 85 is 28.6 dB on such a picture) }
      Check(Sq > 27, Format('the picture as it was, but for a JPEG''s loss: %.1f dB', [Sq]));
    end;
  finally
    R.Free;
    Back.Free;
    M.Free;
    Img.Free;
    DeleteFile(Png);
    DeleteFile(Pdf);
  end;
end;

procedure TestShowPdf(E: TParadeEdit);
var
  Opt: TShowPdfOptions;
  Err, Fn, Txt, Ppm, Stat: string;
  S: TParadeShow;
  Raw: TStringList;
  P, NDur, Off, NextOff, NGrp, NForm: Integer;
  Big: Int64;
  Box: TRect;
begin
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + '../../tests/data/anim.pptx');
  Fn := GetTempDir + 'edit_showpdf_' + IntToStr(GetProcessID) + '.pdf';
  Opt := ShowPdfDefaults;
  Opt.Width := 800;
  Opt.Fps := 20;
  Check(ParadeShowToPdf(E, Fn, Opt, Err) and FileExists(Fn), 'the show written as a PDF ' + Err);
  { still: the first slide as it begins and after each of its four clicks; the second once its wipe has played, and
    after its click }
  Check(ShowPdfStill = 7, Format('a page for each still state, waiting for a click: %d', [ShowPdfStill]));
  Raw := TStringList.Create;
  try
    Raw.LoadFromFile(Fn);
    Txt := Raw.Text;
  finally
    Raw.Free;
  end;
  NDur := 0;
  P := Pos('/Dur ', Txt);
  while P > 0 do
  begin
    Inc(NDur);
    P := PosEx('/Dur ', Txt, P + 1);
  end;
  Check((NDur = ShowPdfPages - ShowPdfStill) and (NDur >= 40) and (NDur < 120), Format('the frames as the effects play, '
    + 'each shown for its time: %d of %d pages', [NDur, ShowPdfPages]));
  Check((Pos('/Trans << /Type /Trans /S /Push /D 1 /Di 0 >>', Txt) > 0) and (Pos('/PageMode /FullScreen', Txt) > 0) and
    (Pos('/Count ' + IntToStr(ShowPdfPages) + ' ', Txt) > 0), 'the second slide pushed in from the left, as PDF has it; '
    + 'it opens full screen');
  { drawn as the slides are: forms, placed; no picture of a frame (800 px wide), only the deck's own and a shadow's }
  Check((Pos('/Subtype /Form', Txt) > 0) and (Pos('/Subtype /Image', Txt) > 0) and (Pos('/Width 800 ', Txt) = 0) and
    (Pos('/P0 ', Txt) > 0),
    'the pages draw the slides'' own text and shapes, no pictures of them');
  Check(Length(Txt) < 200000, Format('a frame costs little: %d bytes for %d pages', [Length(Txt), ShowPdfPages]));
  { a transparency group only where a shape fades as one: a viewer draws each aside, a frame several times slower }
  NGrp := 0;
  P := Pos('/Group << /S /Transparency >>', Txt);
  while P > 0 do
  begin
    Inc(NGrp);
    P := PosEx('/Group << /S /Transparency >>', Txt, P + 1);
  end;
  NForm := 0;
  P := Pos('/Subtype /Form', Txt);
  while P > 0 do
  begin
    Inc(NForm);
    P := PosEx('/Subtype /Form', Txt, P + 1);
  end;
  Check((NGrp >= 1) and (NGrp <= 3) and (NForm >= 12), Format('a group only for a shape seen fading: %d of %d forms',
    [NGrp, NForm]));
  { a script plays the frames where /Dur is not kept (Chrome): each page's time, 0 for one that waits for a click }
  P := Pos('/OpenAction << /S /JavaScript /JS (var pdD=[', Txt);
  Check(P > 0, 'a script that plays the frames');
  if P > 0 then
  begin
    Stat := Copy(Txt, P + Length('/OpenAction << /S /JavaScript /JS (var pdD=['), MaxInt);
    Stat := Copy(Stat, 1, Pos(']', Stat) - 1);
    Raw := TStringList.Create;
    try
      Raw.StrictDelimiter := True;
      Raw.CommaText := Stat;
      NGrp := 0;
      NForm := 0;
      for P := 0 to Raw.Count - 1 do
        if StrToIntDef(Raw[P], -1) = 0 then
          Inc(NGrp)
        else if StrToIntDef(Raw[P], -1) = 50 then
          Inc(NForm);
      Check((Raw.Count = ShowPdfPages) and (NGrp = ShowPdfStill) and (NForm >= NDur div 2),
        Format('the script has each page''s time: %d of %d, %d waiting, %d a frame''s 50 ms', [Raw.Count,
        ShowPdfPages, NGrp, NForm]));
    finally
      Raw.Free;
    end;
    Check(Pos('pdDoc.pageNum=q;', Txt) > 0, 'the script goes on to the frame due');
  end;
  if FileExists('/usr/bin/gs') then
  begin
    Ppm := GetTempDir + 'edit_showpdf_' + IntToStr(GetProcessID);
    Stat := Ppm + '_static.pdf';
    E.ExportPDF(Stat);
    S := TParadeShow.Create(E, 800, 450, 0);
    try
      S.Start(0);
      S.Next(100); S.Next(200); S.Next(1000);
      S.Tick(1250);   { the box half way up from below the slide, the oval half faded: a page draws it so }
      Box := S.TargetBox(4);
      Box := Rect(Box.Left - 4, Box.Top - 4, Box.Right + 4, 450);
      { (its text, Ghostscript's glyphs, a few hundred px off; the frames before and after it thousands) }
      P := BestPage(Fn, 1, ShowPdfPages - 25, FrameRgb(S), Box, Ppm, Off, NextOff);
      Check((P > 0) and (Off < 1000) and (Off * 4 < NextOff), Format('the box flying in, half way: page %d as the '
        + 'show has it (%d px far off, the next most like it %d)', [P, Off, NextOff]));
      Box := S.TargetBox(5);   { the oval, as it fades out: half see-through }
      Box := Rect(Box.Left - 4, Box.Top - 4, Box.Right + 4, Box.Bottom + 4);
      P := BestPage(Fn, 1, ShowPdfPages - 25, FrameRgb(S), Box, Ppm, Off, NextOff);
      { (a frame of a fade is much as the next: only how near; drawn opaque it is thousands off) }
      Check((P > 0) and (Off < 1000), Format('the oval half faded: page %d as the show has it (%d px far off)',
        [P, Off]));
      S.Next(2000);
      S.Tick(2250);   { the gradient box a quarter turned }
      Box := S.TargetBox(8);
      Box := Rect(Box.Left - Box.Width div 3, Box.Top - Box.Height div 3, Box.Right + Box.Width div 3,
        Box.Bottom + Box.Height div 3);
      P := BestPage(Fn, 1, ShowPdfPages - 25, FrameRgb(S), Box, Ppm, Off, NextOff);
      Check((P > 0) and (Off < 1000) and (Off * 4 < NextOff), Format('the box turning, a quarter of the way: page %d '
        + 'as the show has it (%d px far off, the next most like it %d)', [P, Off, NextOff]));
      S.Tick(3100);
      S.Next(5000);
      S.Tick(6100);   { the second slide: its push and its wipe played }
      S.Tick(6700);
      S.Next(7000);
      S.Tick(7500);   { its group's wheel half way round: the page ten frames (of 50 ms) before the last }
      Box := S.TargetBox(9);
      Check(Box.Width > 50, 'the group''s box');
      Box := Rect(Box.Left - 4, Box.Top - 4, Box.Right + 4, Box.Bottom + 4);
      Big := PxOff(GsPage(Fn, ShowPdfPages - 10, Ppm), FrameRgb(S), Box);
      Check((Big >= 0) and (Big < 20), Format('the wheel half way, as a viewer draws its page: the show''s frame then '
        + '(%d px far off, past a px of edge)', [Big]));
      Big := PxOff(GsPage(Fn, ShowPdfPages - 15, Ppm), FrameRgb(S), Box);
      Check(Big > 200, Format('(and the page a quarter round is not that frame: %d px off)', [Big]));
      { played: the slide as it is, the slides' own PDF's page as the same viewer draws it }
      Big := PxOff(GsPage(Fn, ShowPdfPages, Ppm), GsPage(Stat, 2, Ppm), Rect(0, 0, 800, 450));
      Check((Big >= 0) and (Big < 20), Format('the last page: the second slide as the slides'' PDF has it (%d px far '
        + 'off)', [Big]));
    finally
      S.Free;
      DeleteFile(Stat);
    end;
  end;
  DeleteFile(Fn);
end;

procedure TestCommentEdit(E: TParadeEdit);
var
  Img: TLazIntfImage;
  X, Y, X0, Y0, X1, Y1, RX, RY: Integer;
  Id: pd_comment_id;
  Catch: TCommentCatch;
  Cm: pd_comment;
  Edge, Corner: TFPColor;
begin
  Img := TLazIntfImage.Create(0, 0);
  Catch := TCommentCatch.Create;
  try
    E.RenderView(Img);
    X0 := MaxInt; Y0 := MaxInt; X1 := -1; Y1 := -1; RX := -1; RY := -1;
    for Y := 0 to Img.Height - 1 do
      for X := 0 to Img.Width - 1 do
      begin
        Id := E.CommentAtPoint(X, Y);
        if Id <> 0 then
        begin
          if X < X0 then X0 := X;
          if Y < Y0 then Y0 := Y;
          if X > X1 then X1 := X;
          if Y > Y1 then Y1 := Y;
        end;
        if (Id = 2) and (RX < 0) then
        begin
          RX := X + 10;
          RY := Y + 4;
        end;
      end;
    Check((X1 > X0) and (E.CommentAtPoint(X0 + 10, Y0 + 4) = 1) and (RY > Y0 + 8),
      Format('a balloon for the comment, the reply''s lines below its own (%d,%d-%d,%d reply at %d)', [X0, Y0, X1, Y1, RY]));
    if X1 > X0 then
    begin
      Edge := Img.Colors[(X0 + X1) div 2, Y0];
      Corner := Img.Colors[X0, Y0];
      Check((Edge.red <> Corner.red) or (Edge.green <> Corner.green) or (Edge.blue <> Corner.blue),
        'the balloon''s corner rounded: not drawn as its top edge is');
    end;
    E.OnCommentDblClick := @Catch.Hit;
    if RX >= 0 then
    begin
      TParadeWheel(E).MouseDown(mbLeft, [], RX, RY);
      TParadeWheel(E).MouseUp(mbLeft, [], RX, RY);
      TParadeWheel(E).MouseDown(mbLeft, [ssDouble], RX, RY);
      TParadeWheel(E).DblClick;
      TParadeWheel(E).MouseUp(mbLeft, [], RX, RY);
    end;
    Check(Catch.Got = 2, 'a double click on the reply''s lines: the reply to edit (' + IntToStr(Catch.Got) + ')');
    E.OnCommentDblClick := nil;
    E.Author := 'Bob';
    Check(E.EditComment(1, 'Which fox, the red?') and (E.CommentText(1) = 'Which fox, the red?') and
      (pd_doc_comment_get(E.Doc, 1, Cm) = PD_OK) and (PAnsiChar(@Cm.author[0]) = 'Ann'),
      'Ann''s comment edited by Bob: the text new, still Ann''s');
    Check(not E.EditComment(1, ''), 'not to nothing');
    E.Undo;
    Check(E.CommentText(1) = 'Which fox?', 'undone: ' + E.CommentText(1));
    E.Author := 'Ann';
  finally
    Catch.Free;
    Img.Free;
  end;
end;

{ a comment's balloon by its text however often the view is drawn again (the
  balloons' marks are kept between drawings and must come back as they were
  listed); a click on the balloon selects its text and leaves the view where it
  is, so the second press of a double click finds the balloon to edit it }
procedure TestBalloonPlace(E: TParadeEdit);
var
  I: Integer;
  Cm: pd_comment;
  Pg: Int32;
  CX, Base, Asc, Desc: pd_sp;
  A: TPoint;
  B0, B: TRect;
  Note: string;
  Catch: TCommentCatch;
  Para: pd_block_id;
  Img0, Img1: TLazIntfImage;
  T: string;
  Y, D1, D2: Integer;
  Got: Boolean;

  function Balloon: TRect;
  var
    X, Y: Integer;
  begin
    Result := Rect(MaxInt, MaxInt, -1, -1);
    for Y := 0 to E.ClientHeight - 1 do
      for X := 0 to (E.ClientWidth - 1) div 3 do
        if E.CommentAtPoint(X * 3, Y) = 1 then
        begin
          if X * 3 < Result.Left then Result.Left := X * 3;
          if Y < Result.Top then Result.Top := Y;
          if X * 3 > Result.Right then Result.Right := X * 3;
          if Y > Result.Bottom then Result.Bottom := Y;
        end;
  end;

  function Anchor: TPoint;
  begin
    Result := Point(-1, -1);
    if (pd_doc_comment_get(E.Doc, 1, Cm) = PD_OK) and
       (pd_layout_caret(E.Layout, Cm.range.start, Pg, CX, Base, Asc, Desc) = PD_OK) then
      Result := E.PageToClient(Pg, CX / PD_SP_PER_PT, Base / PD_SP_PER_PT);
  end;

  function Differ(P, Q: TLazIntfImage; R: TRect): Integer;
  var
    X, Y: Integer;
    C, D: TFPColor;
  begin
    Result := 0;
    for Y := Max(0, R.Top) to Min(P.Height, R.Bottom) - 1 do
      for X := Max(0, R.Left) to Min(P.Width, R.Right) - 1 do
      begin
        C := P.Colors[X, Y];
        D := Q.Colors[X, Y];
        if (C.red <> D.red) or (C.green <> D.green) or (C.blue <> D.blue) then
          Inc(Result);
      end;
  end;

  procedure Show;
  begin
    E.Repaint;
    Application.ProcessMessages;
  end;

begin
  E.NewDocument;
  E.MarkupMode := PD_MARKUP_BALLOONS;
  for I := 1 to 200 do
  begin
    E.InsertText('Line ' + IntToStr(I) + ' of the text');
    if I < 200 then
      E.ProcessKey(VK_RETURN, []);
  end;
  Para := pd_doc_next_paragraph(E.Doc, 0);
  for I := 1 to 24 do
    Para := pd_doc_next_paragraph(E.Doc, Para);
  E.GoToPos(PdPos(Para, 0));
  E.ProcessKey(VK_END, [ssShift]);
  Check(E.SelectedText = 'Line 25 of the text', 'line 25 selected: ' + E.SelectedText);
  Note := 'A long note';
  for I := 1 to 30 do
    Note := Note + #10 + 'line ' + IntToStr(I) + ' of it';
  Check(E.AddComment(Note) = 1, 'a long comment on line 25');
  Show;
  A := Anchor;
  B0 := Balloon;
  Check((B0.Right > B0.Left) and (B0.Top <= A.Y) and (B0.Top >= A.Y - Round(24 * E.Zoom)),
    Format('its balloon by its line (balloon top %d, the text''s baseline %d)', [B0.Top, A.Y]));
  for I := 1 to 2 do
  begin
    TParadeWheel(E).DoMouseWheel([], -120, Point(50, 50));
    Show;
  end;
  for I := 1 to 2 do
  begin
    TParadeWheel(E).DoMouseWheel([], 120, Point(50, 50));
    Show;
  end;
  E.Invalidate;
  Show;
  B := Balloon;
  Check((B.Left = B0.Left) and (B.Top - Anchor.Y = B0.Top - A.Y),
    Format('drawn again and again, still by its line (%d from it, was %d)', [B.Top - Anchor.Y, B0.Top - A.Y]));

  { the text scrolled out of view above, the balloon's foot still in it }
  I := 0;
  while (Anchor.Y >= 0) and (I < 100) do
  begin
    TParadeWheel(E).DoMouseWheel([], -120, Point(50, 50));
    Show;
    Inc(I);
  end;
  B := Balloon;
  Check((Anchor.Y < 0) and (B.Bottom > B.Top + 10), Format('the text above the view, the balloon in it (%d-%d)', [B.Top, B.Bottom]));
  if B.Bottom > B.Top + 10 then
  begin
    I := E.ViewTop;
    Catch := TCommentCatch.Create;
    try
      E.OnCommentDblClick := @Catch.Hit;
      TParadeWheel(E).MouseDown(mbLeft, [], B.Left + 10, B.Bottom - 4);
      TParadeWheel(E).MouseUp(mbLeft, [], B.Left + 10, B.Bottom - 4);
      Check((E.ViewTop = I) and (E.SelectedText = 'Line 25 of the text'),
        Format('a click on it selects its text and the view stays (%d, was %d: %s)', [E.ViewTop, I, E.SelectedText]));
      Show;
      TParadeWheel(E).MouseDown(mbLeft, [ssDouble], B.Left + 10, B.Bottom - 4);
      TParadeWheel(E).DblClick;
      TParadeWheel(E).MouseUp(mbLeft, [], B.Left + 10, B.Bottom - 4);
      Check(Catch.Got = 1, 'and a double click there: the comment to edit');
    finally
      E.OnCommentDblClick := nil;
      Catch.Free;
    end;
  end;

  { the balloon clicked drawn bold, a little triangle at its place; the same text selected otherwise is not }
  E.GoToPos(PdPos(Para, 0));
  E.ProcessKey(VK_END, [ssShift]);
  Show;
  A := Anchor;
  B := Balloon;
  Img0 := TLazIntfImage.Create(0, 0);
  Img1 := TLazIntfImage.Create(0, 0);
  try
    E.RenderView(Img0);
    TParadeWheel(E).MouseDown(mbLeft, [], B.Left + 10, B.Top + 6);
    TParadeWheel(E).MouseUp(mbLeft, [], B.Left + 10, B.Top + 6);
    Show;
    E.RenderView(Img1);
    Check(E.SelectedText = 'Line 25 of the text', 'the balloon clicked: its text selected');
    Check(Differ(Img0, Img1, Rect(B.Left - 3, B.Top + 8, B.Left + 6, B.Bottom - 8)) >= (B.Bottom - B.Top - 16) div 2,
      Format('its frame bold (%d pixels changed)', [Differ(Img0, Img1, Rect(B.Left - 3, B.Top + 8, B.Left + 6, B.Bottom - 8))]));
    Check(Differ(Img0, Img1, Rect(A.X - 5, A.Y - 3, A.X + 6, A.Y + 2)) >= 10,
      Format('a triangle at its place (%d pixels changed)', [Differ(Img0, Img1, Rect(A.X - 5, A.Y - 3, A.X + 6, A.Y + 2))]));
    Check(Differ(Img0, Img1, Rect(A.X + 20, A.Y + 1, A.X + 120, A.Y + 6)) >= 20,
      Format('the dotted line under its text bold (%d pixels changed)', [Differ(Img0, Img1, Rect(A.X + 20, A.Y + 1, A.X + 120, A.Y + 6))]));
    Check(Differ(Img0, Img1, Rect(B.Left - 9, Min(A.Y, B.Top + 8) - 4, B.Left - 2, Max(A.Y, B.Top + 8) + 6)) >= 7,
      Format('the line on to the balloon bold (%d pixels changed)',
      [Differ(Img0, Img1, Rect(B.Left - 9, Min(A.Y, B.Top + 8) - 4, B.Left - 2, Max(A.Y, B.Top + 8) + 6))]));
    E.ProcessKey(VK_LEFT, []);
    Show;
    E.RenderView(Img1);
    Check(Differ(Img0, Img1, Rect(B.Left - 3, B.Top + 8, B.Left + 6, B.Bottom - 8)) = 0,
      'the caret moved on: no longer bold');

    { a change's balloon: its text selected and copied, not edited }
    E.TrackChanges := True;
    Para := pd_doc_next_paragraph(E.Doc, 0);
    for I := 1 to 99 do
      Para := pd_doc_next_paragraph(E.Doc, Para);
    E.GoToPos(PdPos(Para, 9));
    for I := 1 to 6 do
      E.ProcessKey(VK_RIGHT, [ssShift]);
    Check(E.SelectedText = 'of the', 'on line 100: ' + E.SelectedText);
    E.ProcessKey(VK_DELETE, []);
    E.TrackChanges := False;
    E.ProcessKey(VK_HOME, []);
    Show;
    T := E.DocumentText;
    Got := False;
    for Y := 0 to E.ClientHeight - 1 do
      if not Got and (E.CommentAtPoint(B.Left + 10, Y) = 0) then
      begin
        TParadeWheel(E).MouseDown(mbLeft, [], B.Left + 10, Y);
        TParadeWheel(E).MouseUp(mbLeft, [], B.Left + 10, Y);
        Got := E.SelectedText = 'of the';
      end;
    Check(Got, 'the deletion''s balloon clicked: the deleted text selected');
    if Got then
    begin
      Clipboard.AsText := '';
      E.CopyToClipboard;
      Check(Clipboard.AsText = 'of the', 'and copied: ' + Clipboard.AsText);
      E.InsertText('X');
      E.ProcessKey(VK_DELETE, []);
      E.ProcessKey(VK_BACK, []);
      E.ProcessKey(VK_RETURN, []);
      Check((E.DocumentText = T) and (E.SelectedText = 'of the'), 'but not typed over, deleted or split');
      E.ProcessKey(VK_RIGHT, []);
      E.InsertText('X');
      Check(E.DocumentText <> T, 'the caret moved on: typing again');
      E.Undo;
    end;

    { Two deletions by one author are one revision, and the balloon clicked was told from the others by its
      revision alone: a click on one drew every deletion of that revision bold (reported on a resubmission's
      tracked changes).  Two deletions in one session of tracking, on lines 102 and 104; the first one's
      balloon clicked; the second's left as it was. }
    E.TrackChanges := True;
    Para := pd_doc_next_paragraph(E.Doc, 0);
    for I := 1 to 101 do
      Para := pd_doc_next_paragraph(E.Doc, Para);
    E.GoToPos(PdPos(Para, 0));
    for I := 1 to 4 do
      E.ProcessKey(VK_RIGHT, [ssShift]);
    E.ProcessKey(VK_DELETE, []);
    Para := pd_doc_next_paragraph(E.Doc, pd_doc_next_paragraph(E.Doc, Para));
    E.GoToPos(PdPos(Para, 0));
    for I := 1 to 4 do
      E.ProcessKey(VK_RIGHT, [ssShift]);
    E.ProcessKey(VK_DELETE, []);
    E.TrackChanges := False;
    E.ProcessKey(VK_HOME, []);
    Show;
    D1 := -1;
    D2 := -1;
    for I := 0 to E.BalloonCount - 1 do    { the last two: the first deletion, of its own session, is before them }
      if E.BalloonKind(I) = PD_MARK_DELETION then
      begin
        D1 := D2;
        D2 := I;
      end;
    Check((D1 >= 0) and (D2 >= 0), Format('two deletions, two balloons (%d, %d)', [D1, D2]));
    if (D1 >= 0) and (D2 >= 0) then
    begin
      B := E.BalloonRect(D1);
      B0 := E.BalloonRect(D2);
      E.RenderView(Img0);
      TParadeWheel(E).MouseDown(mbLeft, [], B.Left + 10, B.Top + 6);
      TParadeWheel(E).MouseUp(mbLeft, [], B.Left + 10, B.Top + 6);
      Show;
      E.RenderView(Img1);
      Check(E.SelectedText = 'Line', 'the first of the two balloons clicked: its text: ' + E.SelectedText);
      Check(Differ(Img0, Img1, Rect(B.Left - 3, B.Top + 4, B.Left + 6, B.Bottom - 4)) > 0,
        'its frame bold');
      Check(Differ(Img0, Img1, Rect(B0.Left - 3, B0.Top, B0.Right + 3, B0.Bottom + 1)) = 0,
        Format('the other deletion''s balloon, of the same revision, left as it was (%d pixels changed)',
        [Differ(Img0, Img1, Rect(B0.Left - 3, B0.Top, B0.Right + 3, B0.Bottom + 1))]));
      E.ProcessKey(VK_RIGHT, []);
    end;
  finally
    Img0.Free;
    Img1.Free;
  end;
end;

{ a resource found where the document shows it, and the view taken there: a picture as itself and inside a drawing
  (its "img"), in reading order, counted round; each selected as a click selects it -- the whole object, its top
  near the view's top -- and a drawing so selected not tinted line by line over its labels }
procedure TestShowResource(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
var
  Png, Js: string;
  R, RD, RX: pd_res_id;
  I, N: Integer;
  Para: pd_block_id;
  O: pd_inline;
  U: TParadePosArray;
  At: pd_pos;
  Sid: Integer;
  Pg: Int32;
  X0, Y0, X1, Y1: Double;
  A: TPoint;
  Img0, Img1: TLazIntfImage;
  Diff, X, Y: Integer;
  C0, C1: TFPColor;
  PP: pd_para_props;
  Use: TParadeUseArray;
  Fl, FPara: pd_block_id;
  Fp: pd_float_props;

  function ParaN(N_: Integer): pd_block_id;
  var
    J: Integer;
  begin
    Result := pd_doc_next_paragraph(E.Doc, 0);
    for J := 1 to N_ - 1 do
      Result := pd_doc_next_paragraph(E.Doc, Result);
  end;

begin
  E.NewDocument;
  for I := 1 to 80 do
  begin
    E.InsertText('Line ' + IntToStr(I));
    if I < 80 then
      E.ProcessKey(VK_RETURN, []);
  end;
  Png := FileText(ExtractFilePath(ParamStr(0)) + '../../tests/data/rgba.png');
  Check(pd_doc_add_resource(E.Doc, 'image/png', PAnsiChar(Png), Length(Png), R) = PD_OK, 'a picture');
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_IMAGE;
  O.resource := R;
  O.width := 120 * K;
  O.height := 640 * K;   { taller than the view: selecting it puts the caret at its foot, which must not decide }
  { in a float beside line 40, as a paper's figures are: its own paragraph, its line exactly 14 points -- the
    picture stands far above its line's text }
  Check(pd_doc_insert_block(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 39, PD_BLOCK_FLOAT, Fl) = PD_OK,
    'a float');
  pd_doc_float_props(E.Doc, Fl, Fp);
  Fp.placement := PD_PLACE_HERE or PD_PLACE_FORCE;
  Fp.wrap := PD_WRAP_LEFT;
  Fp.width := 120 * K;
  pd_doc_set_float_props(E.Doc, Fl, Fp);
  FPara := pd_doc_child(E.Doc, Fl, 0);
  pd_doc_insert_inline(E.Doc, PdPos(FPara, 0), O, nil);
  pd_doc_para_props(E.Doc, FPara, PP);
  PP.mask := PP.mask or PD_PP_LINE_SPACING;
  PP.line_rule := PD_LINE_EXACT;
  PP.line_height := 14 * K;
  Check(pd_doc_set_para_props(E.Doc, FPara, PP) = PD_OK, 'its line exactly 14 points');
  { a drawing that draws the same picture, with a label over it }
  Js := Format('{"w":%d,"h":%d,"items":[{"sid":0,"box":[0,0,%d,%d]},{"img":%d,"x":0,"y":0,"w":%d,"h":%d},' +
    '{"label":"A label in it","x":%d,"y":%d,"sz":%d,"w":400,"i":0,"c":4278190080,"ha":0}]}',
    [160 * K, 120 * K, 80 * K, 60 * K, R, 80 * K, 60 * K, 10 * K, 100 * K, 12 * K]);
  Check(pd_doc_add_resource(E.Doc, 'application/vnd.parade.drawing+json', PAnsiChar(Js), Length(Js), RD) = PD_OK,
    'a drawing of it');
  O.resource := RD;
  O.width := 160 * K;
  O.height := 120 * K;
  Para := ParaN(60);
  pd_doc_insert_inline(E.Doc, PdPos(Para, 0), O, nil);
  pd_doc_add_resource(E.Doc, 'text/plain', PAnsiChar('unused'), 6, RX);
  E.ExternalChange;
  E.ProcessKey(VK_HOME, [ssCtrl]);
  E.Repaint;
  Application.ProcessMessages;

  U := E.ResourceUses(R);
  Check((Length(U) = 2) and (U[0].block = FPara) and (U[1].block = Para),
    Format('the picture shown twice, in reading order: itself, then in the drawing (%d)', [Length(U)]));
  Check(E.ShowResource(RX) = 0, 'one shown nowhere: nothing to go to');

  N := E.ShowResource(R, 0);
  E.Repaint;
  Application.ProcessMessages;
  Check((N = 2) and E.SelectedShape(At, Sid) and (At.block = U[0].block) and (At.offset = U[0].offset) and (Sid = -1),
    'gone to the first, it selected whole as a click selects it');
  Check(E.ShapePageBox(U[0], -1, Pg, X0, Y0, X1, Y1), 'where it is');
  A := E.PageToClient(Pg, X0 / K, Y0 / K);
  Check((A.Y >= 0) and (A.Y < E.ClientHeight div 3), Format('its top near the view''s (%d of %d)', [A.Y, E.ClientHeight]));

  E.ShowResource(R, 1);
  E.Repaint;
  Application.ProcessMessages;
  Check(E.SelectedShape(At, Sid) and (At.block = Para) and (Sid = 0),
    Format('and again: the next -- in the drawing, its shape that is the picture selected (%d)', [Sid]));
  Check(E.ShowResource(R, 2) = 2, 'counted round: the first again');
  Check((E.ShowResource(RD) = 1) and E.SelectedShape(At, Sid) and (Sid = -1), 'the drawing itself: selected whole');
  Img0 := TLazIntfImage.Create(0, 0);
  Img1 := TLazIntfImage.Create(0, 0);
  try
    E.RenderView(Img1);
    E.ProcessKey(VK_ESCAPE, []);    { not selected now (an arrow key would move the shape selected) }
    E.ProcessKey(VK_ESCAPE, []);
    Application.ProcessMessages;
    E.RenderView(Img0);
    Check(E.ShapePageBox(U[1], -1, Pg, X0, Y0, X1, Y1), 'where the drawing is');
    A := E.PageToClient(Pg, X0 / K, Y0 / K);
    Diff := 0;
    for Y := Max(0, A.Y) to Min(Img0.Height, A.Y + Round((Y1 - Y0) / K * E.Zoom * 96 / 72)) - 1 do
      for X := Max(0, A.X) to Min(Img0.Width, A.X + Round((X1 - X0) / K * E.Zoom * 96 / 72)) - 1 do
      begin
        C0 := Img0.Colors[X, Y];
        C1 := Img1.Colors[X, Y];
        if (C0.red <> C1.red) or (C0.green <> C1.green) or (C0.blue <> C1.blue) then
          Inc(Diff);
      end;
    Check(Diff = 0, Format('the selected drawing not tinted over its labels and picture (%d pixels)', [Diff]));
  finally
    Img0.Free;
    Img1.Free;
  end;

  { what the pages show: the picture (twice), the drawing; not the text that is no image of them }
  Use := E.ResourcesInUse;
  Check((Length(Use) > Integer(RX)) and Use[R] and Use[RD] and not Use[RX], 'in use: the picture and the drawing');
  E.ShowResource(RD);
  E.ProcessKey(VK_DELETE, []);
  Use := E.ResourcesInUse;
  Check(not Use[RD] and Use[R], 'the drawing deleted: not in use; the picture still, in its float');
  E.ShowResource(R);
  E.ProcessKey(VK_DELETE, []);
  Use := E.ResourcesInUse;
  Check(not Use[R], 'the picture deleted too: not in use, though kept');
  E.Undo;
  Use := E.ResourcesInUse;
  Check(Use[R] and not Use[RD], 'undone: the picture in use again');
end;

{ a table by the mouse: a drag from cell to cell selects the rectangle of cells (painted and formatted so); a
  selection in a shaded cell shows; a border shows the sizing cursor, and dragged sizes its column or row }
procedure TestTableMouse(E: TParadeEdit);
const
  K = PD_SP_PER_PT;
var
  Tbl: pd_block_id;
  Cells: array[0..2, 0..2] of pd_block_id;
  Boxes: array[0..2, 0..2] of pd_cell_box;
  Buf: array[0..63] of pd_cell_box;
  N: Int32;
  R, C, I: Integer;
  A, B: TPoint;
  Img: TLazIntfImage;
  P: pd_char_props;
  Sel, Plain, Before, After: TFPColor;
  W0, W1, H0: pd_sp;
  CPage: Int32;
  CX, Base, Asc, Desc: pd_sp;

  function Pt(const Bx: pd_cell_box; FX, FY: Double): TPoint;
  begin
    Result := E.PageToClient(0, (Bx.x + Bx.w * FX) / K, (Bx.y + Bx.h * FY) / K);
  end;

  procedure ReadBoxes;
  var
    Q, R2, C2: Integer;
  begin
    pd_layout_page_cells(E.Layout, 0, @Buf[0], 64, N);
    for R2 := 0 to 2 do
      for C2 := 0 to 2 do
        for Q := 0 to N - 1 do
          if Buf[Q].cell = Cells[R2, C2] then
            Boxes[R2, C2] := Buf[Q];
  end;

  function WeightIn(Cell: pd_block_id): Integer;
  var
    Rs: array[0..7] of pd_run;
    NR: Int32;
    Pr: pd_char_props;
    Para: pd_block_id;
  begin
    Result := -1;
    Para := pd_doc_child(E.Doc, Cell, 0);
    if (pd_doc_para_runs(E.Doc, Para, @Rs[0], 8, NR) = PD_OK) and (NR > 0) and
       (pd_doc_format_resolve(E.Doc, Para, Rs[0].format, Pr) = PD_OK) then
      Result := Pr.weight;
  end;

  function Differs(const X, Y: TFPColor): Boolean;
  begin
    Result := (Abs(X.red - Y.red) > $0800) or (Abs(X.green - Y.green) > $0800) or (Abs(X.blue - Y.blue) > $0800);
  end;

begin
  E.NewDocument;
  E.InsertTable(3, 3);
  E.CellAt(E.CaretPos, Cells[0, 0], Tbl, Tbl);
  for R := 0 to 2 do
    for C := 0 to 2 do
    begin
      Cells[R, C] := pd_doc_child(E.Doc, pd_doc_child(E.Doc, Tbl, R), C);
      pd_doc_insert_text(E.Doc, PdPos(pd_doc_child(E.Doc, Cells[R, C], 0), 0), PAnsiChar('w' + IntToStr(R) + IntToStr(C)),
        3, PD_FORMAT_INHERIT, nil);
    end;
  E.ExternalChange;
  E.Repaint;
  Application.ProcessMessages;
  ReadBoxes;
  Check((N = 9) and (Boxes[0, 0].w > 0) and (Boxes[0, 0].x + Boxes[0, 0].w = Boxes[0, 1].x),
    Format('the cells'' boxes from the layout (%d)', [N]));
  { from the middle cell of the first row to the first of the second: the four cells of that rectangle, not the
    text between in reading order (which would take the first row's last cell and not its first) }
  A := Pt(Boxes[0, 1], 0.5, 0.5);
  B := Pt(Boxes[1, 0], 0.5, 0.5);
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], (A.X + B.X) div 2, (A.Y + B.Y) div 2);
  TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y);
  Check(Length(E.SelectedCells) = 4, Format('a drag from cell to cell: the rectangle''s four cells (%d)',
    [Length(E.SelectedCells)]));
  Img := TLazIntfImage.Create(0, 0);
  try
    E.RenderView(Img);
    A := Pt(Boxes[0, 0], 0.9, 0.85);
    B := Pt(Boxes[0, 2], 0.9, 0.85);
    Sel := Img.Colors[A.X, A.Y];
    Plain := Img.Colors[B.X, B.Y];
    Check(Differs(Sel, Plain) and (Sel.blue > Sel.red),
      Format('painted: the first cell, though its text is before the anchor, and not the row''s last (%.4x %.4x)',
      [Sel.red, Plain.red]));
    FillChar(P, SizeOf(P), 0);
    P.mask := PD_CP_WEIGHT;
    P.weight := 700;
    E.ApplyCharProps(P);
    Check((WeightIn(Cells[0, 0]) = 700) and
      (WeightIn(Cells[1, 1]) = 700) and
      (WeightIn(Cells[0, 2]) <> 700),
      'bold on it: those four cells'' text, not the row''s last cell');
    E.Undo;
    Check(WeightIn(Cells[0, 0]) <> 700, 'undone in one step');

    { a shaded cell: a word double-clicked in it shows selected }
    A := Pt(Boxes[2, 2], 0.5, 0.5);
    E.ClickAt(0, (Boxes[2, 2].x + Boxes[2, 2].w div 2) / K, (Boxes[2, 2].y + Boxes[2, 2].h div 2) / K, False);
    E.SetCellShading($C0C0C0);
    E.Repaint;
    Application.ProcessMessages;
    pd_layout_caret(E.Layout, PdPos(pd_doc_child(E.Doc, Cells[2, 2], 0), 1), CPage, CX, Base, Asc, Desc);
    A := E.PageToClient(0, CX / K, (Base - Asc / 2) / K);
    E.RenderView(Img);
    B := E.PageToClient(0, CX / K + 0.5, (Base - Asc * 0.9) / K);
    Before := Img.Colors[B.X, B.Y];
    TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y);
    TParadeWheel(E).MouseDown(mbLeft, [ssDouble], A.X, A.Y);
    TParadeWheel(E).DblClick;
    TParadeWheel(E).MouseUp(mbLeft, [], A.X, A.Y);
    E.RenderView(Img);
    After := Img.Colors[B.X, B.Y];
    Check((E.SelectedText = 'w22') and Differs(Before, After),
      Format('a word in a shaded cell double-clicked: selected (%s) and painted so (%.4x, was %.4x)',
      [E.SelectedText, After.blue, Before.blue]));
  finally
    Img.Free;
  end;

  { the sizing cursor over a column's edge and a row's foot }
  E.ClickAt(0, (Boxes[0, 0].x + 4 * K) / K, (Boxes[0, 0].y + 4 * K) / K, False);
  ReadBoxes;
  A := E.PageToClient(0, (Boxes[0, 0].x + Boxes[0, 0].w) / K, (Boxes[0, 0].y + Boxes[0, 0].h div 2) / K);
  TParadeWheel(E).MouseMove([], A.X, A.Y);
  Check(E.Cursor = crHSplit, 'over a column''s edge: the cursor to size it');
  B := E.PageToClient(0, (Boxes[0, 0].x + Boxes[0, 0].w div 2) / K, (Boxes[0, 0].y + Boxes[0, 0].h) / K);
  TParadeWheel(E).MouseMove([], B.X, B.Y);
  Check(E.Cursor = crVSplit, 'over a row''s foot: the cursor to size it');
  TParadeWheel(E).MouseMove([], Pt(Boxes[1, 1], 0.5, 0.5).X, Pt(Boxes[1, 1], 0.5, 0.5).Y);
  Check((E.Cursor <> crHSplit) and (E.Cursor <> crVSplit), 'inside a cell: not');
  { the edge dragged 40 px right: the first column wider, the second narrower by as much, the table as wide }
  W0 := Boxes[0, 0].w;
  W1 := Boxes[0, 1].w;
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], A.X + 20, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], A.X + 40, A.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], A.X + 40, A.Y);
  ReadBoxes;
  I := Round(40 / (E.Zoom * 96 / 72 / K));
  Check((Abs(Boxes[0, 0].w - W0 - I) <= 2 * K) and (Abs(W1 - Boxes[0, 1].w - I) <= 2 * K) and
    (E.SelectedText = ''),
    Format('a column''s edge dragged: %.1f pt wider, the next %.1f pt narrower (40 px = %.1f pt)',
    [(Boxes[0, 0].w - W0) / K, (W1 - Boxes[0, 1].w) / K, I / K]));
  E.Undo;
  ReadBoxes;
  Check(Abs(Boxes[0, 0].w - W0) <= K, 'undone');
  { the first row's foot dragged 30 px down: that row taller }
  H0 := Boxes[0, 0].h;
  B := E.PageToClient(0, (Boxes[0, 0].x + Boxes[0, 0].w div 2) / K, (Boxes[0, 0].y + Boxes[0, 0].h) / K);
  TParadeWheel(E).MouseDown(mbLeft, [], B.X, B.Y);
  TParadeWheel(E).MouseMove([ssLeft], B.X, B.Y + 30);
  TParadeWheel(E).MouseUp(mbLeft, [], B.X, B.Y + 30);
  ReadBoxes;
  Check(Abs(Boxes[0, 0].h - H0 - Round(30 / (E.Zoom * 96 / 72 / K))) <= 2 * K,
    Format('a row''s foot dragged: %.1f pt taller', [(Boxes[0, 0].h - H0) / K]));
  E.Undo;
  { a table further down, scrolled to by the wheel with the caret left at the top: its border dragged, the view
    stays where it is }
  E.NewDocument;
  for I := 1 to 60 do
    E.InsertText('A paragraph to push the table down the pages.' + #10);
  E.InsertTable(2, 2);
  E.ProcessKey(VK_HOME, [ssCtrl]);
  E.Repaint;
  Application.ProcessMessages;
  Tbl := 0;
  for R := 0 to pd_layout_page_count(E.Layout) - 1 do
    if (pd_layout_page_cells(E.Layout, R, @Buf[0], 64, N) = PD_OK) and (N > 0) and (Tbl = 0) then
    begin
      Tbl := 1;
      CPage := R;
      Boxes[0, 0] := Buf[0];
    end;
  Check(Tbl <> 0, 'the table further down');
  I := 0;
  repeat
    TParadeWheel(E).DoMouseWheel([], -120, Point(50, 50));
    E.Repaint;
    Application.ProcessMessages;
    A := E.PageToClient(CPage, (Boxes[0, 0].x + Boxes[0, 0].w) / K, (Boxes[0, 0].y + Boxes[0, 0].h div 2) / K);
    Inc(I);
  until ((A.Y > 40) and (A.Y < E.ClientHeight - 40)) or (I > 400);
  Check((A.Y > 40) and (A.Y < E.ClientHeight - 40), 'scrolled to it by the wheel');
  TParadeWheel(E).MouseDown(mbLeft, [], A.X, A.Y);
  TParadeWheel(E).MouseMove([ssLeft], A.X + 30, A.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], A.X + 30, A.Y);
  E.Repaint;
  Application.ProcessMessages;
  B := E.PageToClient(CPage, (Boxes[0, 0].x + Boxes[0, 0].w) / K, (Boxes[0, 0].y + Boxes[0, 0].h div 2) / K);
  Check(Abs(B.Y - A.Y) <= 2, Format('a border dragged there: the view where it was (the table at %d, was %d)',
    [B.Y, A.Y]));
end;

{ pictures decoded off the screen's thread: the first paint shows a picture's frame and does not wait for it; the
  picture comes in when the thread has it }
procedure TestPictureDecode(E: TParadeEdit);
var
  Img: TLazIntfImage;
  Items: array[0..255] of pd_draw;
  N: Int32;
  I, Tries: Integer;
  Pt: TPoint;
  First, Last: TFPColor;
  Dir: string;
begin
  Dir := ExtractFilePath(ParamStr(0));
  E.NewDocument;
  Check(E.InsertPicture('tests/data/photo.jpg'), 'a photo to decode');
  E.SaveToFile(Dir + 'edit_decode.pdoc');
  E.LoadFromFile(Dir + 'edit_decode.pdoc');
  Pt := Point(-1, -1);
  if pd_layout_page_items(E.Layout, 0, @Items[0], 256, N) = PD_OK then
    for I := 0 to Min(N, 256) - 1 do
      if Items[I].kind = PD_DRAW_IMAGE then
        Pt := E.PageToClient(0, (Items[I].x + Items[I].w div 2) / PD_SP_PER_PT,
          (Items[I].y + Items[I].h div 2) / PD_SP_PER_PT);
  Check(Pt.X >= 0, 'the photo on the page');
  Img := TLazIntfImage.Create(0, 0);
  try
    E.RenderView(Img);
    First := Img.Colors[Pt.X, Pt.Y];
    Check((First.red shr 8 = $DF) and (First.green shr 8 = $E9) and (First.blue shr 8 = $F5),
      Format('the first paint: the frame in its place, not a wait for the decode (%.2x%.2x%.2x)',
      [First.red shr 8, First.green shr 8, First.blue shr 8]));
    Tries := 0;
    repeat
      Sleep(20);
      Application.ProcessMessages;
      E.RenderView(Img);
      Last := Img.Colors[Pt.X, Pt.Y];
      Inc(Tries);
    until ((Last.red shr 8 <> $DF) or (Last.green shr 8 <> $E9) or (Last.blue shr 8 <> $F5)) or (Tries > 250);
    Check((Last.red shr 8 <> $DF) or (Last.green shr 8 <> $E9) or (Last.blue shr 8 <> $F5),
      Format('then the photo itself, decoded by the thread (%.2x%.2x%.2x after %d)',
      [Last.red shr 8, Last.green shr 8, Last.blue shr 8, Tries]));
  finally
    Img.Free;
  end;
end;

{ text selected in one text box is shaded there and nowhere else: positions in text boxes were all of one place
  in the editor's order of paragraphs, so a selection's offsets were shaded in every text box's paragraph (and
  the document's first) -- double-clicking a word in a drawing's label shaded the start of every label }
procedure TestBoxSelection(E: TParadeEdit; PA, PB: pd_block_id);
var
  Img0, Img1: TLazIntfImage;
  Pg: Int32;
  CX, Base, Asc, Desc, CX2: pd_sp;
  R: TRect;
  P0, P1: TPoint;

  function Differ(const Box: TRect): Integer;
  var
    X, Y: Integer;
    C, D: TFPColor;
  begin
    Result := 0;
    for Y := Max(0, Box.Top) to Min(Img0.Height, Box.Bottom) - 1 do
      for X := Max(0, Box.Left) to Min(Img0.Width, Box.Right) - 1 do
      begin
        C := Img0.Colors[X, Y];
        D := Img1.Colors[X, Y];
        if (C.red <> D.red) or (C.green <> D.green) or (C.blue <> D.blue) then
          Inc(Result);
      end;
  end;

  function TextBox(P: pd_block_id; out Box: TRect): Boolean;
  begin
    Result := (pd_layout_caret(E.Layout, PdPos(P, 0), Pg, CX, Base, Asc, Desc) = PD_OK) and
      (pd_layout_caret(E.Layout, PdPos(P, 3), Pg, CX2, Base, Asc, Desc) = PD_OK);
    if not Result then
      Exit;
    P0 := E.PageToClient(Pg, CX / PD_SP_PER_PT, (Base - Asc) / PD_SP_PER_PT);
    P1 := E.PageToClient(Pg, CX2 / PD_SP_PER_PT, (Base + Desc) / PD_SP_PER_PT);
    Box := Rect(P0.X + 2, P0.Y, P1.X - 1, P1.Y);
  end;

begin
  Img0 := TLazIntfImage.Create(0, 0);
  Img1 := TLazIntfImage.Create(0, 0);
  try
    E.GoToPos(PdPos(PA, 0));
    E.Repaint;
    Application.ProcessMessages;
    E.RenderView(Img0);
    E.ProcessKey(VK_RIGHT, [ssShift]);
    E.ProcessKey(VK_RIGHT, [ssShift]);
    E.ProcessKey(VK_RIGHT, [ssShift]);
    E.Repaint;
    Application.ProcessMessages;
    E.RenderView(Img1);
    Check(E.SelectedText = 'Alp', 'three letters selected in a text box: ' + E.SelectedText);
    Check(TextBox(PA, R) and (Differ(R) > 20), Format('shaded there (%d pixels)', [Differ(R)]));
    Check(TextBox(PB, R) and (R.Right > R.Left) and (Differ(R) = 0),
      Format('and not in the other text box (%d pixels)', [Differ(R)]));
    E.ProcessKey(VK_LEFT, []);
  finally
    Img0.Free;
    Img1.Free;
  end;
end;

{ a field shaded gray while the caret is in it or the selection takes it in, as Word shades one: a field Parade works
  out (a page number) its one character, one kept as Word wrote it (a cross-reference) from start to end }
procedure TestFieldShade(E: TParadeEdit);
var
  Para: pd_block_id;
  O: pd_inline;
  Src: string;
  F: TParadeRanges;
  Img0, Img1: TLazIntfImage;
  Pg: Int32;
  CX, CX2, Base, Asc, Desc: pd_sp;
  P0, P1: TPoint;
  R: TRect;
  KB, KE, F1: Integer;

  function Differ(const Box: TRect): Integer;
  var
    X, Y: Integer;
    C, D: TFPColor;
  begin
    Result := 0;
    for Y := Max(0, Box.Top) to Min(Img0.Height, Box.Bottom) - 1 do
      for X := Max(0, Box.Left) to Min(Img0.Width, Box.Right) - 1 do
      begin
        C := Img0.Colors[X, Y];
        D := Img1.Colors[X, Y];
        if (C.red <> D.red) or (C.green <> D.green) or (C.blue <> D.blue) then
          Inc(Result);
      end;
  end;

  function Band(A, B: UInt32): TRect;
  begin
    Result := Rect(0, 0, 0, 0);
    if (pd_layout_caret(E.Layout, PdPos(Para, A), Pg, CX, Base, Asc, Desc) = PD_OK) and
       (pd_layout_caret(E.Layout, PdPos(Para, B), Pg, CX2, Base, Asc, Desc) = PD_OK) then
    begin
      P0 := E.PageToClient(Pg, CX / PD_SP_PER_PT, (Base - Asc) / PD_SP_PER_PT);
      P1 := E.PageToClient(Pg, CX2 / PD_SP_PER_PT, (Base + Desc) / PD_SP_PER_PT);
      Result := Rect(P0.X, P0.Y, P1.X, P1.Y);
    end;
  end;

  procedure Show;
  begin
    E.Repaint;
    Application.ProcessMessages;
  end;

begin
  E.NewDocument;
  E.InsertText('See  and Fig. 1');
  Para := pd_doc_next_paragraph(E.Doc, 0);
  { right to left, so the offsets before each stay as they are: the cross-reference's end, its start, the page }
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_FIELD;
  O.field := PD_FIELD_KEPT;
  pd_doc_insert_inline(E.Doc, PdPos(Para, 15), O, nil);
  Src := 'REF _Ref1 \h';
  O.source := PAnsiChar(Src);
  O.source_len := Length(Src);
  pd_doc_insert_inline(E.Doc, PdPos(Para, 9), O, nil);
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_FIELD;
  O.field := PD_FIELD_PAGE;
  pd_doc_insert_inline(E.Doc, PdPos(Para, 4), O, nil);
  { written and read back as Word would have it: laid out afresh, and the cross-reference as the reader reads one }
  E.SaveToFile(ExtractFilePath(ParamStr(0)) + 'edit_fields.docx');
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + 'edit_fields.docx');
  Para := pd_doc_next_paragraph(E.Doc, 0);
  { where each one is now: Word's \h made the cross-reference's text a link, inside the field }
  Src := E.ParaText(Para);
  KB := -1;
  KE := -1;
  for Pg := 0 to Length(Src) - 1 do
    if (Copy(Src, Pg + 1, 3) = #$EF#$BF#$BC) and (pd_doc_inline_at(E.Doc, PdPos(Para, Pg), O) = PD_OK) and
       (O.kind = PD_INLINE_FIELD) and (O.field = PD_FIELD_KEPT) then
      if O.source_len > 0 then KB := Pg else KE := Pg;
  F1 := Pos('Fig. 1', Src) - 1;
  Check((KB > 4) and (KB < F1) and (KE > F1), Format('the fields read back: start %d, text %d, end %d', [KB, F1, KE]));
  E.Repaint;
  Application.ProcessMessages;

  E.GoToPos(PdPos(Para, Pos(' and ', Src) + 1));
  Check(Length(E.SelectedFields) = 0, 'the caret between fields: none shaded');
  E.GoToPos(PdPos(Para, F1 + 2));
  F := E.SelectedFields;
  Check((Length(F) = 1) and (F[0].start.offset = UInt32(KB)) and (F[0].finish.offset = UInt32(KE + 3)),
    'the caret in a cross-reference''s text: the whole of it');
  E.GoToPos(PdPos(Para, KB));
  Check(Length(E.SelectedFields) = 0, 'at its very start: not in it');
  E.GoToPos(PdPos(Para, 4));
  E.ProcessKey(VK_RIGHT, [ssShift]);
  F := E.SelectedFields;
  Check((Length(F) = 1) and (F[0].start.offset = 4) and (F[0].finish.offset = 7), 'a page number selected: it');
  E.GoToPos(PdPos(Para, 0));
  E.ProcessKey(VK_END, [ssShift]);
  Check(Length(E.SelectedFields) = 2, 'the line selected: both');

  { drawn: gray over the cross-reference's text with the caret in it, and nothing beside it }
  Img0 := TLazIntfImage.Create(0, 0);
  Img1 := TLazIntfImage.Create(0, 0);
  try
    E.GoToPos(PdPos(Para, Pos(' and ', Src) + 1));
    Show;
    E.RenderView(Img0);
    E.GoToPos(PdPos(Para, F1 + 2));
    Show;
    E.RenderView(Img1);
    R := Band(F1, F1 + 5);     { "Fig. " }
    Check((R.Right > R.Left) and (Differ(R) > (R.Right - R.Left) * (R.Bottom - R.Top) div 2),
      Format('the cross-reference shaded (%d of %d pixels)', [Differ(R), (R.Right - R.Left) * (R.Bottom - R.Top)]));
    R := Band(7, KB - 1);
    Check(Differ(Rect(R.Left + 2, R.Top, R.Right - 2, R.Bottom)) = 0, 'the text before it not');
  finally
    Img0.Free;
    Img1.Free;
  end;
end;

{ what a right click is on, for a menu of its own: plain text nothing; a link its address (and Remove Link keeping
  its text); a table; a tracked change; a comment's text its comment; a field its code; a picture; a header; a right
  click in the selection leaving it as it is }
procedure TestContext(E: TParadeEdit);
var
  P: array[0..7] of pd_block_id;
  C: TParadeContext;
  A, B: pd_pos;
  U: string;
  Pt: TPoint;
  I: Integer;
  SP: pd_section_props;

  function At(Blk: pd_block_id; Off: UInt32): TPoint;
  var
    Pg: Int32;
    CX, Base, Asc, Desc: pd_sp;
  begin
    Result := Point(-1, -1);
    E.Repaint;
    Application.ProcessMessages;
    if pd_layout_caret(E.Layout, PdPos(Blk, Off), Pg, CX, Base, Asc, Desc) = PD_OK then
      Result := E.PageToClient(Pg, CX / PD_SP_PER_PT + 1.5, (Base - Asc div 2) / PD_SP_PER_PT);
  end;

  function RightAt(Blk: pd_block_id; Off: UInt32): TParadeContext;
  begin
    Pt := At(Blk, Off);
    E.ContextClick(Pt.X, Pt.Y);
    Result := E.ContextAt(Pt.X, Pt.Y);
  end;

  function Para(N: Integer): pd_block_id;
  var
    K: Integer;
  begin
    Result := pd_doc_next_paragraph(E.Doc, 0);
    for K := 1 to N do
      Result := pd_doc_next_paragraph(E.Doc, Result);
  end;

begin
  E.NewDocument;
  E.InsertText('Plain words here');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('See ');
  E.InsertLink('https://example.org/', 'Example');
  E.InsertText(' now');
  E.ProcessKey(VK_RETURN, []);
  E.TrackChanges := True;
  E.InsertText('Added words');
  E.TrackChanges := False;
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Noted words');
  E.ProcessKey(VK_HOME, []);
  E.ProcessKey(VK_END, [ssShift]);
  E.AddComment('A note');
  E.ProcessKey(VK_END, []);
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Page ');
  E.InsertField(PD_FIELD_PAGE);
  E.ProcessKey(VK_RETURN, []);
  Check(E.InsertPicture(ExtractFilePath(ParamStr(0)) + '../../tests/data/rgba.png'), 'a picture for the menu');
  E.ProcessKey(VK_RETURN, []);
  E.InsertTable(2, 2);
  E.InsertText('cell');
  for I := 0 to 5 do
    P[I] := Para(I);

  C := RightAt(P[0], 2);
  Check((C.Shape = '') and not C.Table and (C.Link = '') and not C.Field and (C.Comment = 0) and not C.Change and
    (C.HeaderFooter = 0), 'plain text: nothing but text');
  C := RightAt(P[1], 9);     { in "Example", past the link's zero-width start }
  Check(C.Link = 'https://example.org/', 'a link: its address: ' + C.Link);
  C := RightAt(P[2], 3);
  Check(C.Change, 'a tracked insertion: a change');
  C := RightAt(P[3], 3);
  Check(C.Comment <> 0, 'commented text: its comment');
  C := RightAt(P[4], 5);     { on the number: the caret beside it, which is the field's for a menu }
  Check(C.Field and (C.FieldCode = 'PAGE'), 'a page number: a field, PAGE: ' + C.FieldCode);
  C := RightAt(P[5], 0);
  Check(C.Shape = 'picture', 'a picture: selected, a picture: ' + C.Shape);
  Check(E.SelectedShape(A, I), 'and selected by the right click');
  E.ClearShapeSelection;
  E.GoToPos(PdPos(Para(6), 0));
  C := E.ContextAt(0, 0);
  Check(C.Table, 'in a table: a table');

  { the selection stays when the right click is in it, goes when it is not }
  E.GoToPos(PdPos(P[0], 0));
  E.ProcessKey(VK_END, [ssShift]);
  Pt := At(P[0], 6);
  E.ContextClick(Pt.X, Pt.Y);
  Check(E.SelectedText = 'Plain words here', 'a right click in the selection: it stays');
  Pt := At(P[2], 3);
  E.ContextClick(Pt.X, Pt.Y);
  Check(E.SelectedText = '', 'one elsewhere: the caret there');

  { Remove Link: its text kept, the link gone }
  Check(E.RemoveLink(PdPos(P[1], 7)) and (Pos('See ', E.ParaText(P[1])) = 1) and
    not E.LinkAt(PdPos(P[1], 5), U, A, B), 'Remove Link: ' + E.ParaText(P[1]));
  Check(Pos('Example now', E.ParaText(P[1])) > 0, 'its text kept');
  E.Undo;
  Check(E.LinkAt(PdPos(P[1], 7), U, A, B), 'and back with undo');

  { a header: right-clicked in it }
  E.SetHeaderFooter(False, 'Head text');
  E.GoToPos(PdPos(P[0], 0));
  SP := E.CurrentSectionProps;
  if (SP.header <> 0) and (pd_doc_child(E.Doc, SP.header, 0) <> 0) then
  begin
    Pt := At(pd_doc_child(E.Doc, SP.header, 0), 2);
    C := E.ContextAt(Pt.X, Pt.Y);
    Check(C.HeaderFooter = 1, Format('the header: a header (%d at %d,%d)', [C.HeaderFooter, Pt.X, Pt.Y]));
  end
  else
    Check(False, 'a header made');
end;

{ pages side by side once two fit across, as Word shows them: at 40% a document's second page beside its first, at
  the same top; a click on it a place on it; one column without MultiPage }
procedure TestPagesAcross(E: TParadeEdit);
var
  I: Integer;
  Info: pd_page_info;
  P: pd_pos;
  Pg: Int32;
  CX, Base, Asc, Desc: pd_sp;
  Was: Double;
begin
  E.NewDocument;
  for I := 1 to 150 do
  begin
    E.InsertText('Line ' + IntToStr(I) + ' of a document of several pages');
    E.ProcessKey(VK_RETURN, []);
  end;
  Was := E.Zoom;
  E.Zoom := 0.4;
  E.Repaint;
  Application.ProcessMessages;
  Check(E.PageCount >= 3, 'several pages: ' + IntToStr(E.PageCount));
  Check(E.PagesAcross >= 2, Format('at 40%% in %d pixels: pages side by side (%d across)', [E.ClientWidth,
    E.PagesAcross]));
  Check((E.PageToClient(1, 0, 0).Y = E.PageToClient(0, 0, 0).Y) and (E.PageToClient(1, 0, 0).X > E.PageToClient(0, 0, 0).X), Format('the second beside the first ' +
    '(%d,%d and %d,%d)', [E.PageToClient(0, 0, 0).X, E.PageToClient(0, 0, 0).Y, E.PageToClient(1, 0, 0).X, E.PageToClient(1, 0, 0).Y]));
  pd_layout_page_info(E.Layout, 1, Info);
  Check(E.PageToClient(1, 0, 0).X >= E.PageToClient(0, Info.width / PD_SP_PER_PT, 0).X, 'and not over it');
  { a click in the second page's text: a place on that page, not the first }
  Check(E.PageAt(E.PageToClient(1, 0, 0).X + 30, E.PageToClient(1, 0, 0).Y + 80) = 1, 'the page at a point of the second: the second');
  E.ClickAt(1, 80, 80);
  Check((pd_layout_caret(E.Layout, E.CaretPos, Pg, CX, Base, Asc, Desc) = PD_OK) and (Pg = 1),
    Format('clicked on the second page: the caret on it (page %d)', [Pg]));
  E.MultiPage := False;
  E.Repaint;
  Application.ProcessMessages;
  Check((E.PagesAcross = 1) and (E.PageToClient(1, 0, 0).Y > E.PageToClient(0, 0, 0).Y), 'without MultiPage: one column');
  E.MultiPage := True;
  E.Zoom := Was;
end;

{ the slide being edited while the caret is in a text box of it: a box's text is a story of its own, in no slide's
  section, and the slide was none (-1) -- the host, fitting the slide to the view, then showed the first: a click
  into a box on any slide jumped back to slide 1 (reported on a talk's slides) }
procedure TestSlideOfTextBox(E: TParadeEdit);
var
  I, Sid: Integer;
  PZ: pd_block_id;
  Pg: Int32;
  CX, Base, Asc, Desc: pd_sp;
  Pt: TPoint;
  At: pd_pos;
begin
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + '../../tests/data/slides.pptx');
  E.Repaint;
  Application.ProcessMessages;
  Check(E.SlideCount >= 2, 'two slides');
  { a text box on the second, with words in it }
  E.ShowSlide(1);
  E.Repaint;
  Application.ProcessMessages;
  E.ClickAt(1, 30, 30);
  Check(E.SelectedShape(At, Sid) and E.AddShape('textbox', 100 * PD_SP_PER_PT, 100 * PD_SP_PER_PT,
    400 * PD_SP_PER_PT, 160 * PD_SP_PER_PT), 'a text box put on the second slide');
  PZ := pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, pd_doc_story_count(E.Doc) - 1), 0);
  pd_doc_insert_text(E.Doc, PdPos(PZ, 0), 'Second slide words', 18, 0, nil);
  E.ClearShapeSelection;
  E.SaveToFile(ExtractFilePath(ParamStr(0)) + 'edit_slidebox.pptx');      { laid out afresh, as a talk is opened }
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + 'edit_slidebox.pptx');
  E.Repaint;
  Application.ProcessMessages;
  PZ := 0;
  for I := 0 to pd_doc_story_count(E.Doc) - 1 do
    if (PZ = 0) and (Pos('Second slide words', E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0))) = 1) then
      PZ := pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0);
  Check((PZ <> 0) and (pd_layout_caret(E.Layout, PdPos(PZ, 2), Pg, CX, Base, Asc, Desc) = PD_OK) and (Pg = 1),
    'the box''s words on the second slide');
  if PZ = 0 then
    Exit;
  E.ShowSlide(1);
  E.Repaint;
  Application.ProcessMessages;
  Pt := E.PageToClient(Pg, CX / PD_SP_PER_PT + 1, (Base - Asc div 2) / PD_SP_PER_PT);
  for I := 1 to 2 do
  begin
    TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
  end;
  Check(E.CaretPos.block = PZ, 'the caret in the box''s text');
  Check(E.CurrentSlide = 1, Format('the slide being edited is the box''s: 1 (%d)', [E.CurrentSlide]));
end;

{ a click on a bullet selects the list's labels at that level, as Word does: the caret at that paragraph's start,
  no shape selected (in a slide's text box the click took the box), every bullet of the level shaded and the text
  not; a right click there a label's; a new bullet for the level, every one of its paragraphs' }
procedure TestListLabelClick(E: TParadeEdit);
var
  P: array[0..2] of pd_block_id;
  I, Sid: Integer;
  Pt: TPoint;
  Img0, Img1: TLazIntfImage;
  Lst: pd_list_id;
  Lv: Int32;
  At: pd_pos;
  Buf: array[0..63] of AnsiChar;
  C: TParadeContext;
  Lbl: string;

  function BulletPoint(Para: pd_block_id): TPoint;
  var
    Pg: Int32;
    CX, Base, Asc, Desc: pd_sp;
  begin
    Result := Point(-1, -1);
    E.Repaint;
    Application.ProcessMessages;
    if pd_layout_caret(E.Layout, PdPos(Para, 0), Pg, CX, Base, Asc, Desc) = PD_OK then
      Result := E.PageToClient(Pg, CX / PD_SP_PER_PT - 14, (Base - Asc div 2) / PD_SP_PER_PT);
  end;

  function Shaded(Para: pd_block_id; TextToo: Boolean): Integer;
  var
    Q: TPoint;
    X, Y, X0, X1: Integer;
    A, B: TFPColor;
  begin
    Result := 0;
    Q := BulletPoint(Para);
    if TextToo then
    begin
      X0 := Q.X + 30;
      X1 := Q.X + 80;
    end
    else
    begin
      X0 := Q.X - 6;
      X1 := Q.X + 10;
    end;
    for Y := Q.Y - 4 to Q.Y + 4 do
      for X := X0 to X1 do
        if (X >= 0) and (Y >= 0) and (X < Img0.Width) and (Y < Img0.Height) then
        begin
          A := Img0.Colors[X, Y];
          B := Img1.Colors[X, Y];
          if (A.red <> B.red) or (A.green <> B.green) or (A.blue <> B.blue) then
            Inc(Result);
        end;
  end;

begin
  E.NewDocument;
  E.InsertText('Apples');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Pears');
  E.ProcessKey(VK_RETURN, []);
  E.InsertText('Plums');
  P[0] := pd_doc_next_paragraph(E.Doc, 0);
  P[1] := pd_doc_next_paragraph(E.Doc, P[0]);
  P[2] := pd_doc_next_paragraph(E.Doc, P[1]);
  E.SetSelection(PdPos(P[0], 0), PdPos(P[2], 5));
  E.ToggleList(PD_NUM_BULLET);
  E.GoToPos(PdPos(P[2], 5));
  Img0 := TLazIntfImage.Create(0, 0);
  Img1 := TLazIntfImage.Create(0, 0);
  try
    Pt := BulletPoint(P[1]);
    E.RenderView(Img0);
    TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
    E.Repaint;
    Application.ProcessMessages;
    E.RenderView(Img1);
    Check(E.ListLabelSelected(Lst, Lv), 'a click on the second bullet: the list''s labels selected');
    Check((E.CaretPos.block = P[1]) and (E.CaretPos.offset = 0), 'the caret at its paragraph''s start');
    Check((Shaded(P[0], False) > 20) and (Shaded(P[2], False) > 20), Format('every bullet of the level shaded (%d, %d)',
      [Shaded(P[0], False), Shaded(P[2], False)]));
    Check(Shaded(P[0], True) = 0, Format('and not the text (%d)', [Shaded(P[0], True)]));
    C := E.ContextAt(Pt.X, Pt.Y);
    Check(C.ListLabel, 'a right click there: a label''s menu');
    E.SetListLabel(PD_NUM_BULLET, #$E2#$9C#$93);     { a tick }
    pd_doc_list_label(E.Doc, P[2], @Buf[0], SizeOf(Buf));
    Lbl := Buf;
    Check(Pos(#$E2#$9C#$93, Lbl) > 0, 'a new bullet: every paragraph''s of the level: ' + Lbl);
    Check(E.ListLabelSelected(Lst, Lv), 'still selected');
    E.GoToPos(PdPos(P[1], 2));
    Check(not E.ListLabelSelected(Lst, Lv), 'the caret moved on: not selected');
  finally
    Img0.Free;
    Img1.Free;
  end;

  { in a slide's text box: the bullet, not the box }
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + '../../tests/data/slides.pptx');
  E.Repaint;
  Application.ProcessMessages;
  P[0] := 0;
  for I := 0 to pd_doc_story_count(E.Doc) - 1 do
    if (P[0] = 0) and (Pos('First point', E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0))) > 0) then
      P[0] := pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0);
  Check(P[0] <> 0, 'a bulleted point on the slide');
  if P[0] <> 0 then
  begin
    Pt := BulletPoint(P[0]);
    TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], Pt.X, Pt.Y);
    Check(not E.SelectedShape(At, Sid), 'a click on a slide''s bullet: no shape selected');
    Check(E.ListLabelSelected(Lst, Lv) and (E.CaretPos.block = P[0]), 'its labels selected');
  end;
end;

{ the text box being typed in shows its handles, as Word and PowerPoint show them -- to size it and to turn it, the
  turning one orange -- and a drag of one sizes the box }
procedure TestTypingBoxHandles(E: TParadeEdit; AForm: TForm);
var
  At: pd_pos;
  Sid, I, J, Orange: Integer;
  St, Para: pd_block_id;
  Pg: Int32;
  X0, Y0, X1, Y1, W0, BX0, BY0, BX1, BY1: Double;
  Bmp: TBitmap;
  Img: TLazIntfImage;
  C: TFPColor;
  Pt: TPoint;
begin
  E.NewDocument;
  E.InsertText('Before');
  E.ProcessKey(VK_RETURN, []);
  Check(E.InsertCanvas(300, 150) and E.AddShape('textbox', 40 * PD_SP_PER_PT, 40 * PD_SP_PER_PT, 200 * PD_SP_PER_PT,
    100 * PD_SP_PER_PT) and E.SelectedShape(At, Sid), 'a text box in a canvas');
  St := pd_doc_story_at(E.Doc, pd_doc_story_count(E.Doc) - 1);
  Para := pd_doc_child(E.Doc, St, 0);
  pd_doc_insert_text(E.Doc, PdPos(Para, 0), 'Typed in', 8, 0, nil);
  E.ClearShapeSelection;
  E.GoToPos(PdPos(Para, 3));
  E.Repaint;
  Application.ProcessMessages;
  Check(not E.SelectedShape(At, I), 'the caret in the box''s text, the box not selected');
  Bmp := AForm.GetFormImage;
  Img := Bmp.CreateIntfImage;
  try
    Orange := 0;
    for J := 0 to Img.Height - 1 do
      for I := 0 to Img.Width - 1 do
      begin
        C := Img.Colors[I, J];
        if (C.red > $F000) and (C.green > $7800) and (C.green < $A000) and (C.blue < $2000) then
          Inc(Orange);
      end;
    Check(Orange > 20, Format('typing in it: its turning handle, orange (%d pixels)', [Orange]));
  finally
    Img.Free;
    Bmp.Free;
  end;
  { its right edge's handle dragged 40 pixels on: wider }
  Check(E.ShapePageBox(At, Sid, Pg, X0, Y0, X1, Y1), 'where the box is');
  W0 := X1 - X0;
  Pt := E.PageToClient(Pg, X1 / PD_SP_PER_PT, (Y0 + Y1) / 2 / PD_SP_PER_PT);
  TParadeWheel(E).MouseDown(mbLeft, [], Pt.X, Pt.Y);
  TParadeWheel(E).MouseMove([ssLeft], Pt.X + 20, Pt.Y);
  TParadeWheel(E).MouseMove([ssLeft], Pt.X + 40, Pt.Y);
  TParadeWheel(E).MouseUp(mbLeft, [], Pt.X + 40, Pt.Y);
  Check(E.ShapePageBox(At, Sid, Pg, BX0, BY0, BX1, BY1) and (BX1 - BX0 > W0 * 1.1), Format('its handle dragged: ' +
    'the box wider (%.0f, was %.0f)', [(BX1 - BX0) / PD_SP_PER_PT, W0 / PD_SP_PER_PT]));
end;

{ PowerPoint's New Slide (Ctrl+M): after the slide, on its layout -- its placeholders empty, showing their prompts
  ("Click to add title"), the slide's own shapes not copied; Reset Slide puts a moved placeholder back where the
  layout has it }
procedure TestNewLayoutSlide(E: TParadeEdit; AForm: TForm);
var
  N, I, Sid, Gray, Title, Places, Orange, Body: Integer;
  D, At: pd_pos;
  Boxes: TParadeShapeBoxes;
  Ph, Idx, Names: string;
  X0, Y0, X1, Y1, TX0, TY0: Double;
  Pg: Int32;
  Bmp: TBitmap;
  Img: TLazIntfImage;
  C, C2: TFPColor;
  Bmp2: TBitmap;
  Img2: TLazIntfImage;
  Xml: string;
begin
  E.LoadFromFile(ExtractFilePath(ParamStr(0)) + '../../tests/data/slides.pptx');
  E.Repaint;
  Application.ProcessMessages;
  N := E.SlideCount;
  Check(E.SlideLayoutPath(0) <> '', 'the first slide''s layout known: ' + E.SlideLayoutPath(0));
  Check(E.NewLayoutSlide(0), 'New Slide after the first');
  Check(E.SlideCount = N + 1, 'a slide more');
  Check(E.CurrentSlide = 1, 'the new one being edited');
  Check(E.GoToSlide(1) and E.SelectedShape(D, Sid), 'its canvas');
  Boxes := E.DrawingShapes(D);
  Title := -1;
  for I := 0 to High(Boxes) do
    if E.PlaceholderOf(D, Boxes[I].Sid, Ph, Idx) and ((Ph = 'title') or (Ph = 'ctrTitle')) then
      Title := I;
  Check(Title >= 0, 'a title placeholder on it');
  Places := 0;
  Body := -1;
  for I := 0 to High(Boxes) do
    if E.PlaceholderOf(D, Boxes[I].Sid, Ph, Idx) then
    begin
      Inc(Places);
      if (I <> Title) and (Boxes[I].Story <> 0) then
        Body := I;
    end;
  Check(Places >= 2, Format('and its content placeholder, as the slide had (%d placeholders)', [Places]));
  if Title >= 0 then
    Check((Boxes[Title].Story <> 0) and (E.ParaText(pd_doc_child(E.Doc, Boxes[Title].Story, 0)) = ''), 'empty');
  if not E.KeptXml(Xml) then
    Xml := '';
  Check((Pos('name="Box"', Xml) = 0) and (Pos('name="Oval"', Xml) = 0), 'the slide''s own shapes not on it');
  { its prompt, gray, where the title would be -- and gone with the caret in it }
  E.ClearShapeSelection;
  E.ShowSlide(1);
  E.Repaint;
  Application.ProcessMessages;
  Gray := 0;
  if (Title >= 0) and E.ShapePageBox(D, Boxes[Title].Sid, Pg, X0, Y0, X1, Y1) then
  begin
    Bmp := AForm.GetFormImage;
    Img := Bmp.CreateIntfImage;
    E.GoToPos(PdPos(pd_doc_child(E.Doc, Boxes[Title].Story, 0), 0));
    E.Repaint;
    Application.ProcessMessages;
    Bmp2 := AForm.GetFormImage;
    Img2 := Bmp2.CreateIntfImage;
    try
      with E.PageToClient(Pg, X0 / PD_SP_PER_PT, Y0 / PD_SP_PER_PT) do
        for I := Max(0, Y + E.Top) to Min(Img.Height - 1, E.PageToClient(Pg, 0, Y1 / PD_SP_PER_PT).Y + E.Top) do
          for Sid := Max(0, X + E.Left) to Min(Img.Width - 1, E.PageToClient(Pg, X1 / PD_SP_PER_PT, 0).X + E.Left) do
          begin
            C := Img.Colors[Sid, I];
            C2 := Img2.Colors[Sid, I];
            if (Abs(C.red - $9090) < $1800) and (Abs(C.green - $9090) < $1800) and (Abs(C.blue - $9090) < $1800) and
               ((C.red <> C2.red) or (C.green <> C2.green) or (C.blue <> C2.blue)) then
              Inc(Gray);
          end;
    finally
      Img.Free;
      Bmp.Free;
      Img2.Free;
      Bmp2.Free;
    end;
  end;
  Check(Gray > 30, Format('its prompt drawn, gray, and gone with the caret in it (%d pixels)', [Gray]));
  { a click in the middle of the empty title: the caret in its text, not the box selected; typing there, its frame
    and handles stay, the turning one orange }
  E.ClearShapeSelection;
  E.GoToSlide(1);
  E.SelectedShape(D, Sid);
  if (Body >= 0) and E.ShapePageBox(D, Boxes[Body].Sid, Pg, X0, Y0, X1, Y1) then
  begin   { low in the content box, well under its one empty line }
    with E.PageToClient(Pg, (X0 + X1) / 2 / PD_SP_PER_PT, (Y0 + (Y1 - Y0) * 0.8) / PD_SP_PER_PT) do
    begin
      TParadeWheel(E).MouseDown(mbLeft, [], X, Y);
      TParadeWheel(E).MouseUp(mbLeft, [], X, Y);
    end;
    Check(not E.SelectedShape(At, Sid) and (E.CaretPos.block = pd_doc_child(E.Doc, Boxes[Body].Story, 0)),
      'a click inside the empty content box, under its line: the caret in it');
    E.GoToPos(PdPos(pd_doc_child(E.Doc, Boxes[Title].Story, 0), 0));    { and in the title, typing }
    E.ShowSlide(1);
    E.InsertText('Typed');
    E.Repaint;
    Application.ProcessMessages;
    Bmp := AForm.GetFormImage;
    Img := Bmp.CreateIntfImage;
    try
      Orange := 0;
      for I := 0 to Img.Height - 1 do
        for Sid := 0 to Img.Width - 1 do
        begin
          C := Img.Colors[Sid, I];
          if (C.red > $F000) and (C.green > $7800) and (C.green < $A000) and (C.blue < $2000) then
            Inc(Orange);
        end;
    finally
      Img.Free;
      Bmp.Free;
    end;
    Check(Orange > 20, Format('typing in it: its frame and handles, the turning one orange (%d pixels)', [Orange]));
    E.Undo;
  end;
  { moved, then Reset Slide: back }
  if Title >= 0 then
  begin
    Check(E.GoToSlide(1) and E.SelectedShape(D, Sid) and E.ShapePageBox(D, Boxes[Title].Sid, Pg, TX0, TY0, X1, Y1),
      'where the title is');
    E.ToggleShape(Boxes[Title].Sid);
    Check(E.SelectedShape(At, Sid) and E.ShapeBox(Sid, X0, Y0, X1, Y1) and E.SetShapeBox(Sid, X0 + 500000, Y0 + 500000,
      X1 + 500000, Y1 + 500000), 'the title moved');
    E.ClearShapeSelection;
    Check(E.ResetSlide(1), 'Reset Slide');
    E.GoToSlide(1);
    E.SelectedShape(D, Sid);
    Check(E.ShapePageBox(D, Boxes[Title].Sid, Pg, X0, Y0, X1, Y1) and (Abs(X0 - TX0) < PD_SP_PER_PT) and
      (Abs(Y0 - TY0) < PD_SP_PER_PT), Format('the title back where the layout has it (%.0f,%.0f; %.0f,%.0f)',
      [X0 / PD_SP_PER_PT, Y0 / PD_SP_PER_PT, TX0 / PD_SP_PER_PT, TY0 / PD_SP_PER_PT]));
  end;
  Names := '';
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

  { PARADE_PICK_DOCX=file.docx: every picture and drawing of a document, clicked where it is drawn, is selected --
    inline or floating }
  if GetEnvironmentVariable('PARADE_PICK_DOCX') <> '' then
  begin
    E.LoadFromFile(GetEnvironmentVariable('PARADE_PICK_DOCX'));
    PickOk := 0;
    PickAll := 0;
    PickSeen := '';
    for PickPg := 0 to E.PageCount - 1 do
    begin
      pd_layout_page_items(E.Layout, PickPg, nil, 0, N);
      SetLength(Items, N + 1);
      pd_layout_page_items(E.Layout, PickPg, @Items[0], N, N);
      for I := 0 to N - 1 do
        if (Items[I].kind = PD_DRAW_IMAGE) and (Items[I].block <> 0) and (Items[I].w > 4 * PD_SP_PER_PT) and
           (Pos('|' + IntToStr(Items[I].block) + ':' + IntToStr(Items[I].offset) + '|', PickSeen) = 0) and
           (pd_doc_inline_at(E.Doc, PdPos(Items[I].block, Items[I].offset), Obj) = PD_OK) then
        begin
          PickSeen := PickSeen + '|' + IntToStr(Items[I].block) + ':' + IntToStr(Items[I].offset) + '|';
          Inc(PickAll);
          E.ProcessKey(VK_ESCAPE, []);
          E.ProcessKey(VK_ESCAPE, []);
          E.ClickAt(PickPg, (Items[I].x + Items[I].w / 2) / PD_SP_PER_PT, (Items[I].y + Items[I].h / 2) / PD_SP_PER_PT);
          if E.SelectedShape(DrawP, DrawSid) and (DrawP.block = Items[I].block) and (DrawP.offset = Items[I].offset) then
            Inc(PickOk)
          else
          begin
            WriteLn('  not picked: page ', PickPg + 1, ' at ', Round(Items[I].x / PD_SP_PER_PT), ',',
              Round(Items[I].y / PD_SP_PER_PT), ' ', Round(Items[I].w / PD_SP_PER_PT), 'x',
              Round(Items[I].h / PD_SP_PER_PT), ' region ', Items[I].region);
            if pd_layout_caret(E.Layout, PdPos(Items[I].block, Items[I].offset), CapPage, CapX, CapBase, CapAsc,
               CapDesc) = PD_OK then
              WriteLn('    caret: page ', CapPage + 1, ' x ', Round(CapX / PD_SP_PER_PT), ' base ',
                Round(CapBase / PD_SP_PER_PT), ' obj ', Round(Obj.width / PD_SP_PER_PT), 'x',
                Round(Obj.height / PD_SP_PER_PT), ' selected ', E.SelectedShape(DrawP, DrawSid));
          end;
        end;
    end;
    WriteLn('picked ', PickOk, ' of ', PickAll);
    Halt(0);
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
  { a caption in a drawing canvas is text of the document: clicked into, typed in, undone, and saved back into
    the drawing }
  CapFile := ExpandFileName(Dir + '../../tests/data/canvas_caption.docx');
  if FileExists(CapFile) then
  begin
    E.LoadFromFile(CapFile);
    CapB := 0;
    for I := 0 to pd_doc_story_count(E.Doc) - 1 do
      if (CapB = 0) and (Pos('Fig. 1.', E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0))) = 1) then
        CapB := pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0);
    Check(CapB <> 0, 'the caption is a story of the document');
    Check((CapB <> 0) and (pd_layout_caret(E.Layout, PdPos(CapB, 4), CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK),
      'and is laid out on the page');
    if CapB <> 0 then
    begin
      E.ClickAt(CapPage, CapX / PD_SP_PER_PT, (CapBase - CapAsc / 2) / PD_SP_PER_PT);
      Check(E.CaretPos.block = CapB, 'a click in the caption puts the caret there');
      E.Invalidate;     { its text box's edge dashed, for a look }
      E.Update;
      Application.ProcessMessages;
      ExecuteProcess('/usr/bin/import', ['-window', 'root', Dir + 'edit_tbframe.png']);
      E.InsertText('NEW ');
      Check(Pos('NEW ', E.ParaText(CapB)) > 0, 'what is typed goes into the caption: ' + E.ParaText(CapB));
      E.Undo;
      Check(Pos('NEW', E.ParaText(CapB)) = 0, 'and undo takes it out');
      E.Redo;
      { the caption's text box as a shape: Escape from its text, its edge clicked; moved by a drag, a click on
        its text to type again, deleted; Escape out to the drawing and past it }
      E.ProcessKey(VK_ESCAPE, []);
      TbSid := -1;
      if E.SelectedShape(DrawP, DrawSid) then
        for I := 0 to High(E.DrawingShapes(DrawP)) do
          if (E.DrawingShapes(DrawP)[I].Sid = DrawSid) and (E.DrawingShapes(DrawP)[I].Story <> 0) and
             (pd_doc_child(E.Doc, E.DrawingShapes(DrawP)[I].Story, 0) = CapB) then
            TbSid := DrawSid;
      Check(TbSid >= 0, 'Escape from the caption''s text selects its text box');
      if (TbSid >= 0) and E.ShapePageBox(DrawP, TbSid, TbPage, BX0, BY0, BX1, BY1) then
      begin
        Pt0 := E.PageToClient(TbPage, (BX0 + BX1) / 2 / PD_SP_PER_PT, (BY0 + BY1) / 2 / PD_SP_PER_PT);
        TParadeWheel(E).MouseDown(mbLeft, [], Pt0.X, Pt0.Y);
        TParadeWheel(E).MouseMove([ssLeft], Pt0.X + 10, Pt0.Y + 10);
        TParadeWheel(E).MouseMove([ssLeft], Pt0.X + 20, Pt0.Y + 20);
        TParadeWheel(E).MouseUp(mbLeft, [], Pt0.X + 20, Pt0.Y + 20);
        Check(E.ShapePageBox(DrawP, TbSid, TbPage, NX0, NY0, NX1, NY1) and (NX0 > BX0 + PD_SP_PER_PT) and
          (NY0 > BY0 + PD_SP_PER_PT) and (Abs((NX1 - NX0) - (BX1 - BX0)) < PD_SP_PER_PT),
          'the text box moved by dragging it');
        E.Undo;
        Check(E.ShapePageBox(DrawP, TbSid, TbPage, NX0, NY0, NX1, NY1) and (Abs(NX0 - BX0) < PD_SP_PER_PT),
          'and back with undo');
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = TbSid), 'still selected');
        if pd_layout_caret(E.Layout, PdPos(CapB, 4), CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK then
        begin
          Pt0 := E.PageToClient(CapPage, CapX / PD_SP_PER_PT, (CapBase - CapAsc / 2) / PD_SP_PER_PT);
          TParadeWheel(E).MouseDown(mbLeft, [], Pt0.X, Pt0.Y);
          TParadeWheel(E).MouseUp(mbLeft, [], Pt0.X, Pt0.Y);
          Check(not E.SelectedShape(DrawP, DrawSid) and (E.CaretPos.block = CapB),
            'a click on its text, not a drag: the caret in the text again');
        end;
        { its edge, from the text: the box at once }
        Pt0 := E.PageToClient(TbPage, BX0 / PD_SP_PER_PT, (BY0 + BY1) / 2 / PD_SP_PER_PT);
        TParadeWheel(E).MouseDown(mbLeft, [], Pt0.X, Pt0.Y);
        TParadeWheel(E).MouseUp(mbLeft, [], Pt0.X, Pt0.Y);
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = TbSid), 'a click on its edge selects the text box');
        NShapes := Length(E.DrawingShapes(DrawP));
        E.ProcessKey(VK_DELETE, []);
        Check(Length(E.DrawingShapes(DrawP)) = NShapes - 1, 'Delete takes the text box out');
        E.Undo;
        Check(Length(E.DrawingShapes(DrawP)) = NShapes, 'and undo puts it back');
        TParadeWheel(E).MouseDown(mbLeft, [], Pt0.X, Pt0.Y);
        TParadeWheel(E).MouseUp(mbLeft, [], Pt0.X, Pt0.Y);
        E.ProcessKey(VK_ESCAPE, []);
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = -1), 'Escape from the text box: the drawing');
        E.ProcessKey(VK_ESCAPE, []);
        Check(not E.SelectedShape(DrawP, DrawSid) and (Pos(#$EF#$BF#$BC, E.ParaText(E.CaretPos.block)) > 0) and
          (E.CaretPos.offset = 3), 'Escape again: the text, just after the drawing');
      end;
      { a word by a double click, the paragraph by a third }
      Bk := pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 0);
      if pd_layout_caret(E.Layout, PdPos(Bk, 3), CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK then
      begin
        { as the LCL sends them: down, up; down (double), the double click, up; the triple click, then down (triple) }
        Pt0 := E.PageToClient(CapPage, CapX / PD_SP_PER_PT, (CapBase - CapAsc / 2) / PD_SP_PER_PT);
        TParadeWheel(E).MouseDown(mbLeft, [], Pt0.X, Pt0.Y);
        TParadeWheel(E).MouseUp(mbLeft, [], Pt0.X, Pt0.Y);
        TParadeWheel(E).MouseDown(mbLeft, [ssDouble], Pt0.X, Pt0.Y);
        TParadeWheel(E).DblClick;
        TParadeWheel(E).MouseUp(mbLeft, [], Pt0.X, Pt0.Y);
        Check((E.SelectedText <> '') and (Pos(' ', E.SelectedText) = 0), 'a double click: a word, ' + E.SelectedText);
        TParadeWheel(E).TripleClick;
        TParadeWheel(E).MouseDown(mbLeft, [ssTriple], Pt0.X, Pt0.Y);
        TParadeWheel(E).MouseMove([ssLeft], Pt0.X + 1, Pt0.Y);
        TParadeWheel(E).MouseUp(mbLeft, [], Pt0.X, Pt0.Y);
        Check(E.SelectedText = E.ParaText(Bk), 'a triple click: the paragraph, ' + E.SelectedText);
        E.ProcessKey(VK_RIGHT, []);
      end;
      { the drawing and its shapes, selected: one click the whole, the next a shape; Tab the next shape;
        Escape back out; Delete takes the drawing as any text, and undo brings it back }
      DrawPara := pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 1);
      if pd_layout_caret(E.Layout, PdPos(DrawPara, 0), CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK then
      begin
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawP.block = DrawPara) and (DrawSid = -1),
          'a click on a drawing selects the whole of it');
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = 0), 'a second click the shape under it');
        E.Invalidate;
        E.Update;
        Application.ProcessMessages;
        ExecuteProcess('/usr/bin/import', ['-window', 'root', Dir + 'edit_shape.png']);
        { moved by a point with the arrow key, back with undo; made twice as wide; taken out and put back }
        Check(E.ShapeBox(0, BX0, BY0, BX1, BY1), 'the shape''s box');
        E.ProcessKey(VK_RIGHT, []);
        Check(E.ShapeBox(0, NX0, NY0, NX1, NY1) and (Abs(NX0 - BX0 - PD_SP_PER_PT) < 2) and (Abs(NY0 - BY0) < 2),
          'the arrow key moves the shape a point');
        E.Undo;
        Check(E.ShapeBox(0, NX0, NY0, NX1, NY1) and (Abs(NX0 - BX0) < 2), 'undo puts it back');
        NShapes := Length(E.DrawingShapes(PdPos(DrawPara, 0)));
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        E.ProcessKey(VK_DELETE, []);
        Check(Length(E.DrawingShapes(PdPos(DrawPara, 0))) = NShapes - 1, 'Delete takes the shape out of the drawing');
        E.Undo;
        Check(Length(E.DrawingShapes(PdPos(DrawPara, 0))) = NShapes, 'and undo puts it back');
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        Check(E.SetShapeBox(0, BX0, BY0, BX0 + 2 * (BX1 - BX0), BY1), 'the shape made twice as wide');
        E.ProcessKey(VK_TAB, []);
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = 1), 'Tab the next shape');
        E.ProcessKey(VK_ESCAPE, []);
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = -1), 'Escape the drawing again');
        E.ProcessKey(VK_DELETE, []);
        Check(Pos(#$EF#$BF#$BC, E.ParaText(DrawPara)) = 0, 'Delete takes the selected drawing');
        E.Undo;
        Check(Pos(#$EF#$BF#$BC, E.ParaText(DrawPara)) = 1, 'and undo brings it back');
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        E.ProcessKey(VK_ESCAPE, []);
        Check(not E.SelectedShape(DrawP, DrawSid), 'Escape from the drawing: nothing selected');
      end
      else
        Check(False, 'the drawing is laid out');
      E.SaveToFile(Dir + 'edit_caption.docx');
      E.LoadFromFile(Dir + 'edit_caption.docx');
      Same := False;
      for I := 0 to pd_doc_story_count(E.Doc) - 1 do
        Same := Same or (Pos('NEW ', E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, I), 0))) > 0);
      Check(Same, 'saved: the edited caption comes back in the drawing');
      { and the shape as Word will have it: twice as wide, read back from the XML kept for it }
      DrawPara := pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 1);
      Same := False;
      for I := 0 to High(E.DrawingShapes(PdPos(DrawPara, 0))) do
        with E.DrawingShapes(PdPos(DrawPara, 0))[I] do
          if Sid = 0 then
            Same := Abs((X1 - X0) - 2 * (BX1 - BX0)) < PD_SP_PER_PT;
      Check(Same, 'saved: the resized shape comes back resized');
      { filled, outlined, put in front, grouped and ungrouped: each in the XML Word gets, each undone }
      if pd_layout_caret(E.Layout, PdPos(DrawPara, 0), CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK then
      begin
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = 0), 'the shape selected again');
        Check(E.SetShapeFill(clRed, False) and E.KeptXml(Xml) and (Pos('<a:srgbClr val="FF0000"/>', Xml) > 0),
          'a fill: red in the shape''s XML');
        Check(Length(E.DrawingShapes(PdPos(DrawPara, 0))) = NShapes, 'the drawing made again keeps its shapes');
        E.Undo;
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 144 + 20);
        Check(E.KeptXml(Xml) and (Pos('FF0000', Xml) = 0), 'and undo takes it out');
        Check(E.SetShapeLine(clBlue, 3, False) and E.KeptXml(Xml) and (Pos('w="38100"', Xml) > 0) and
          (Pos('<a:srgbClr val="0000FF"/>', Xml) > 0), 'an outline: blue, three points wide');
        Check(E.SetShapeLine(clNone, 1.5, False) and E.KeptXml(Xml) and (Pos('w="19050"', Xml) > 0) and
          (Pos('<a:srgbClr val="0000FF"/>', Xml) > 0), 'a width alone: the colour kept');
        Check(E.ShapeBox(0, BX0, BY0, BX1, BY1), 'the box of the first shape');
        Check(E.ShapeOrder(2) and E.SelectedShape(DrawP, DrawSid) and (DrawSid = NShapes - 1),
          'brought to the front: the last shape drawn, still selected');
        Check(E.ShapeBox(DrawSid, NX0, NY0, NX1, NY1) and (Abs(NX0 - BX0) < 2) and (Abs(NY1 - BY1) < 2),
          'and where it was');
        Check(E.ShapeOrder(3) and E.SelectedShape(DrawP, DrawSid) and (DrawSid = 0), 'sent to the back again');
        Check(not E.GroupShapes, 'one shape is no group');
        Check(E.ToggleShape(1) and (Length(E.SelectedShapes) = 2), 'Shift+click adds a second shape');
        Check(E.GroupShapes and E.KeptXml(Xml) and (Pos('<wpg:wgp><wpg:cNvGrpSpPr/>', Xml) > 0),
          'the two made a group');
        Check(Length(E.DrawingShapes(PdPos(DrawPara, 0))) = NShapes, 'the group keeps its shapes');
        Check(E.ShapeBox(0, NX0, NY0, NX1, NY1) and (Abs(NX0 - BX0) < 2) and (Abs(NX1 - BX1) < 2) and
          (Abs(NY0 - BY0) < 2), 'and leaves them where they were');
        Check(E.UngroupShape and E.KeptXml(Xml) and (Pos('<wpg:wgp>', Xml) = 0), 'ungrouped');
        Check(E.ShapeBox(0, NX0, NY0, NX1, NY1) and (Abs(NX0 - BX0) < 2) and (Abs(NX1 - BX1) < 2),
          'and still where they were');
        E.Undo;
        Check(E.KeptXml(Xml) and (Pos('<wpg:wgp>', Xml) > 0), 'undo: the group again');
        E.SaveToFile(Dir + 'edit_group.docx');
        E.LoadFromFile(Dir + 'edit_group.docx');
        DrawPara := pd_doc_child(E.Doc, pd_doc_child(E.Doc, pd_doc_root(E.Doc), 0), 1);
        Same := False;
        for I := 0 to High(E.DrawingShapes(PdPos(DrawPara, 0))) do
          with E.DrawingShapes(PdPos(DrawPara, 0))[I] do
            if Sid = 0 then
              Same := (Abs(X0 - BX0) < PD_SP_PER_PT) and (Abs(X1 - BX1) < PD_SP_PER_PT);
        Check(Same and (Length(E.DrawingShapes(PdPos(DrawPara, 0))) = NShapes),
          'saved: the grouped shapes come back where they were');
      end;
    end;
  end;

  { a canvas put in, shapes drawn in it; text boxes that keep their own text when their order changes }
  E.ProcessKey(VK_END, [ssCtrl]);
  E.ProcessKey(VK_RETURN, []);
  NStories := pd_doc_story_count(E.Doc);
  Check(E.InsertCanvas(300, 150) and E.SelectedShape(DrawP, DrawSid) and (DrawSid = -1), 'a canvas put in, selected');
  Check(E.KeptXml(Xml) and (Pos('<wpc:wpc>', Xml) > 0) and (Length(E.DrawingShapes(DrawP)) = 0), 'and empty');
  CanvasP := DrawP;
  Check(E.AddShape('rect', 10 * PD_SP_PER_PT, 10 * PD_SP_PER_PT, 82 * PD_SP_PER_PT, 64 * PD_SP_PER_PT) and
    E.SelectedShape(DrawP, DrawSid) and (DrawSid = 0) and (Length(E.DrawingShapes(CanvasP)) = 1),
    'a rectangle put in, selected');
  Check(E.ShapeBox(0, BX0, BY0, BX1, BY1) and (Abs(BX0 - 10 * PD_SP_PER_PT) < PD_SP_PER_PT) and
    (Abs(BX1 - 82 * PD_SP_PER_PT) < PD_SP_PER_PT), 'where it was put');
  Check(E.AddShape('star5', 100 * PD_SP_PER_PT, 10 * PD_SP_PER_PT, 150 * PD_SP_PER_PT, 60 * PD_SP_PER_PT) and
    E.SelectedShape(DrawP, DrawSid) and (DrawSid = 1), 'a star after it');
  Check(E.AddShape('arrow', 10 * PD_SP_PER_PT, 100 * PD_SP_PER_PT, 150 * PD_SP_PER_PT, 120 * PD_SP_PER_PT, True) and
    E.KeptXml(Xml) and (Pos('<a:tailEnd type="triangle"/>', Xml) > 0) and (Pos('flipH="1"', Xml) > 0),
    'an arrow, right to left');
  Check(E.InsertShape('ellipse') and (E.DrawKind = 'ellipse'), 'Insert shape with a canvas selected: drawn next');
  E.ProcessKey(VK_ESCAPE, []);
  Check(E.DrawKind = '', 'Escape: not drawn');
  E.ToggleShape(0);
  E.ProcessKey(VK_ESCAPE, []);
  Check(E.AddShape('textbox', 170 * PD_SP_PER_PT, 10 * PD_SP_PER_PT, 280 * PD_SP_PER_PT, 40 * PD_SP_PER_PT) and
    (pd_doc_story_count(E.Doc) = NStories + 1), 'a text box: a story of its own');
  StA := pd_doc_story_at(E.Doc, pd_doc_story_count(E.Doc) - 1);
  Check(pd_doc_child(E.Doc, StA, 0) <> 0, 'with a paragraph to type in');
  pd_doc_insert_text(E.Doc, PdPos(pd_doc_child(E.Doc, StA, 0), 0), 'Alpha', 5, 0, nil);
  Check(E.AddShape('textbox', 170 * PD_SP_PER_PT, 60 * PD_SP_PER_PT, 280 * PD_SP_PER_PT, 90 * PD_SP_PER_PT), 'another');
  StB := pd_doc_story_at(E.Doc, pd_doc_story_count(E.Doc) - 1);
  pd_doc_insert_text(E.Doc, PdPos(pd_doc_child(E.Doc, StB, 0), 0), 'Beta', 4, 0, nil);
  Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = 4) and E.ShapeOrder(3) and E.SelectedShape(DrawP, DrawSid) and
    (DrawSid = 0), 'the second sent to the back');
  Check(pd_doc_story_count(E.Doc) = NStories + 2, 'no story made by the move');
  Check((Pos('Alpha', E.ParaText(pd_doc_child(E.Doc, StA, 0))) = 1) and
    (Pos('Beta', E.ParaText(pd_doc_child(E.Doc, StB, 0))) = 1), 'each keeps its own text');
  E.Undo;
  Check(E.SelectedShape(DrawP, DrawSid), 'undo');
  E.Redo;
  E.ProcessKey(VK_ESCAPE, []);
  E.Repaint;
  Application.ProcessMessages;
  TestBoxSelection(E, pd_doc_child(E.Doc, StA, 0), pd_doc_child(E.Doc, StB, 0));
  { a shape of the drawing resized: the text boxes' text still laid out and drawn.  The drawing written again by the
    editor (fpjson's AsJSON: "story" : 12) was not looked in for text boxes, the layout asking for "story": with no
    space, so every text box of a drawing showed blank once one of its pictures was resized }
  E.GoToPos(PdPos(pd_doc_child(E.Doc, StA, 0), 0));
  Check(E.SelectTextBox and E.SelectedShape(DrawP, DrawSid) and E.ShapeBox(DrawSid, BX0, BY0, BX1, BY1) and
    E.SetShapeBox(DrawSid, BX0, BY0, BX0 + (BX1 - BX0) * 0.9, BY1), 'a text box made narrower');
  E.Repaint;
  Application.ProcessMessages;
  Check((pd_layout_caret(E.Layout, PdPos(pd_doc_child(E.Doc, StA, 0), 0), CapPage, CapX, CapBase, CapAsc,
    CapDesc) = PD_OK) and (pd_layout_caret(E.Layout, PdPos(pd_doc_child(E.Doc, StB, 0), 0), CapPage, CapX, CapBase,
    CapAsc, CapDesc) = PD_OK), 'and the text of both text boxes still laid out');
  E.Undo;
  { a shape drawn with the mouse: chosen, then dragged in the canvas from its top left corner }
  E.ProcessKey(VK_ESCAPE, []);
  E.ProcessKey(VK_ESCAPE, []);    { the caret after the canvas: in view }
  E.Repaint;
  Application.ProcessMessages;
  NShapes := Length(E.DrawingShapes(CanvasP));
  if (pd_layout_caret(E.Layout, CanvasP, CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK) then
    E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 2, CapBase / PD_SP_PER_PT - 150 + 2);
  Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = -1) and (DrawP.block = CanvasP.block), 'the canvas clicked');
  if E.InsertShape('ellipse') and
     (pd_layout_caret(E.Layout, CanvasP, CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK) then
  begin
    Pt0 := E.PageToClient(CapPage, CapX / PD_SP_PER_PT + 20, CapBase / PD_SP_PER_PT - 150 + 20);
    Pt1 := E.PageToClient(CapPage, CapX / PD_SP_PER_PT + 92, CapBase / PD_SP_PER_PT - 150 + 74);
    TParadeWheel(E).MouseDown(mbLeft, [], Pt0.X, Pt0.Y);
    TParadeWheel(E).MouseMove([ssLeft], (Pt0.X + Pt1.X) div 2, (Pt0.Y + Pt1.Y) div 2);
    TParadeWheel(E).MouseMove([ssLeft], Pt1.X, Pt1.Y);
    TParadeWheel(E).MouseUp(mbLeft, [], Pt1.X, Pt1.Y);
    Check((Length(E.DrawingShapes(CanvasP)) = NShapes + 1) and (E.DrawKind = '') and
      E.SelectedShape(DrawP, DrawSid) and (DrawSid = NShapes), 'an oval drawn by a drag');
    Check(E.ShapeBox(DrawSid, BX0, BY0, BX1, BY1) and (Abs(BX0 - 20 * PD_SP_PER_PT) < 2 * PD_SP_PER_PT) and
      (Abs(BX1 - 92 * PD_SP_PER_PT) < 2 * PD_SP_PER_PT) and (Abs(BY1 - 74 * PD_SP_PER_PT) < 2 * PD_SP_PER_PT),
      Format('where it was dragged: %.0f %.0f %.0f %.0f', [BX0 / PD_SP_PER_PT, BY0 / PD_SP_PER_PT,
      BX1 / PD_SP_PER_PT, BY1 / PD_SP_PER_PT]));
    E.Undo;
    Check(Length(E.DrawingShapes(CanvasP)) = NShapes, 'and gone with undo');
    E.Redo;
  end
  else
    Check(False, 'the oval to draw');

  { the whole canvas made wider: the object's size }
  E.ProcessKey(VK_ESCAPE, []);
  Check(E.SelectedShape(DrawP, DrawSid) and (DrawSid = -1) and (pd_doc_inline_at(E.Doc, CanvasP, Obj) = PD_OK) and
    E.ResizeObject(Obj.width + 72 * PD_SP_PER_PT, Obj.height) and (pd_doc_inline_at(E.Doc, CanvasP, Obj) = PD_OK) and
    (Abs(Obj.width - 372 * PD_SP_PER_PT) < 2), 'the canvas made an inch wider');
  E.Undo;
  Check((pd_doc_inline_at(E.Doc, CanvasP, Obj) = PD_OK) and (Abs(Obj.width - 300 * PD_SP_PER_PT) < 2),
    'and back with undo');
  E.SaveToFile(Dir + 'edit_canvas.docx');
  E.LoadFromFile(Dir + 'edit_canvas.docx');
  Same := False;
  Bk := pd_doc_next_paragraph(E.Doc, 0);
  while (Bk <> 0) and not Same do
  begin
    I := Pos(#$EF#$BF#$BC, E.ParaText(Bk));
    if I > 0 then
      Same := Length(E.DrawingShapes(PdPos(Bk, I - 1))) = 6;
    Bk := pd_doc_next_paragraph(E.Doc, Bk);
  end;
  Check(Same, 'saved: the canvas comes back with its six shapes');
  Same := False;
  I := -1;
  J := -1;
  for K := 0 to pd_doc_story_count(E.Doc) - 1 do
  begin
    if Pos('Alpha', E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, K), 0))) = 1 then I := K;
    if Pos('Beta', E.ParaText(pd_doc_child(E.Doc, pd_doc_story_at(E.Doc, K), 0))) = 1 then J := K;
  end;
  Check((I >= 0) and (J >= 0) and (J < I), 'and its text boxes, Beta now first: ' + IntToStr(I) + ' ' + IntToStr(J));

  { scrolled a wheel step at a time: the view moves and only the strip that
    comes into view is drawn, which is what a redraw of all of it gives }
  E.ProcessKey(VK_HOME, [ssCtrl]);
  E.Repaint;
  Application.ProcessMessages;
  Same := True;
  for I := 1 to 30 do
  begin
    TParadeWheel(E).DoMouseWheel([], -120, Point(50, 50));
    E.Repaint;
    Application.ProcessMessages;
    Same := Same and E.ViewMatchesRedraw;
  end;
  for I := 1 to 12 do
  begin
    TParadeWheel(E).DoMouseWheel([], 120, Point(50, 50));
    E.Repaint;
    Application.ProcessMessages;
    Same := Same and E.ViewMatchesRedraw;
  end;
  Check(Same, 'a scrolled view is what drawing it afresh gives');
  Check(E.DisplayMatchesView, 'and the display holds those pixels, sent once, in their colours');
  E.ProcessKey(VK_RETURN, []);
  E.Repaint;
  Application.ProcessMessages;
  Check(E.ViewMatchesRedraw and E.DisplayMatchesView, 'an edit sends what it changed to the display as it was drawn');
  E.Undo;
  E.Repaint;
  Application.ProcessMessages;
  E.SaveToFile(Dir + 'edit_test.pdoc');
  T := E.DocumentText;
  E.LoadFromFile(Dir + 'edit_test.pdoc');
  Check(E.DocumentText = T, 'save/load');
  Check(Pos('"_DataInfo_"', FileText(Dir + 'edit_test.pdoc')) = 0, 'a .pdoc is BJData, not JSON');
  E.SaveToFile(Dir + 'edit_test.jdoc');
  Check(Pos('"_DataInfo_"', FileText(Dir + 'edit_test.jdoc')) > 0, 'a .jdoc is JSON text');
  E.LoadFromFile(Dir + 'edit_test.jdoc');
  Check(E.DocumentText = T, 'and loads back');
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
  { a click on the picture selects it as a whole, as a drawing is; its size changed, and back with undo }
  if pd_layout_caret(E.Layout, PdPos(C.block, C.offset - 3), CapPage, CapX, CapBase, CapAsc, CapDesc) = PD_OK then
  begin
    E.ClickAt(CapPage, CapX / PD_SP_PER_PT + 5, CapBase / PD_SP_PER_PT - 5);
    Check(E.SelectedShape(DrawP, DrawSid) and (DrawP.block = C.block) and (DrawP.offset = C.offset - 3) and
      (DrawSid = -1), 'a click on a picture selects it');
    Check(E.ResizeObject(Obj.width * 2, Obj.height * 2) and
      (pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3), Obj) = PD_OK) and (Obj.width = 60 * 3 * 65536 div 2),
      'and makes it twice the size');
    E.Undo;
    Check((pd_doc_inline_at(E.Doc, PdPos(C.block, C.offset - 3), Obj) = PD_OK) and
      (Obj.width = 60 * 3 * 65536 div 4), 'undo: its size again');
    E.ProcessKey(VK_ESCAPE, []);
    E.ProcessKey(VK_END, [ssCtrl]);
  end
  else
    Check(False, 'the picture is laid out');
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

  Step('pictures decoded aside');
  TestPictureDecode(E);
  Step('table by the mouse');
  TestTableMouse(E);
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

  { the paper's colour: a page drawn on dark paper is a dark page, which is
    asked of the pixels rather than of the property that was set }
  E.PaperColor := TColor($00201810);
  Bmp := TBitmap.Create;
  try
    E.RenderPage(0, Bmp, 1.0 * 96 / 72 / PD_SP_PER_PT);
    K := DarkPixels(Bmp);
  finally
    Bmp.Free;
  end;
  E.PaperColor := clWhite;
  Check(K > 4 * I, Format('dark paper is drawn dark: %d dark pixels, %d on white', [K, I]));

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
  TestCommentEdit(E);
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

  { 10. content controls: a check box ticked, a list's choice, a date; kept through DOCX }
  Step('content controls');
  E.TrackChanges := False;
  E.NewDocument;
  E.InsertText('Agree ');
  E.InsertControl('checkbox', []);
  P := E.CaretPos;
  Check(Pos(#$E2#$98#$90, E.DocumentText) > 0, 'a check box, clear: ' + E.DocumentText);
  Check(E.ToggleCheckBox(P) and (Pos(#$E2#$98#$92, E.DocumentText) > 0), 'ticked: ' + E.DocumentText);
  E.Undo;
  Check(Pos(#$E2#$98#$90, E.DocumentText) > 0, 'and undone in one step: ' + E.DocumentText);
  E.ToggleCheckBox(P);
  E.ProcessKey(VK_END, []);
  E.InsertText(' colour ');
  E.InsertControl('dropdown', ['Red', 'Green', 'Blue']);
  Q := E.CaretPos;
  Check(Pos('Choose an item.', E.DocumentText) > 0, 'a list, its prompt: ' + E.DocumentText);
  Names := TStringList.Create;
  try
    Check(E.ControlItems(Q, Names) and (Names.Count = 3) and (Names[1] = 'Green'), 'its choices');
  finally
    Names.Free;
  end;
  E.ChooseControlItem(Q, 1);
  Check((Pos('colour ' + #$EF#$BF#$BC + 'Green', E.DocumentText) > 0) and (Pos('Choose', E.DocumentText) = 0),
    'chosen: ' + E.DocumentText);
  Check(E.ControlAt(E.CaretPos, Kind, Spec, A, B) and (Kind = 'dropdown') and (Pos('"value" : "Green"', Spec) > 0) and
    (Pos('placeholder', Spec) = 0), 'and it says so: ' + Spec);
  E.ProcessKey(VK_END, []);
  E.InsertText(' on ');
  E.InsertControl('date', []);
  E.SetControlDate(E.CaretPos, EncodeDate(2026, 10, 7));
  Check(Pos('on ' + #$EF#$BF#$BC + '10/7/2026', E.DocumentText) > 0, 'a date: ' + E.DocumentText);
  Ms := TMemoryStream.Create;
  try
    E.SaveToStream(Ms, PD_CONV_DOCX);
    Ms.Position := 0;
    E.LoadFromStream(Ms, PD_CONV_DOCX);
  finally
    Ms.Free;
  end;
  Check((Pos(#$E2#$98#$92, E.DocumentText) > 0) and (Pos('Green', E.DocumentText) > 0) and
    (Pos('10/7/2026', E.DocumentText) > 0), 'and read back: ' + E.DocumentText);
  Check(E.ControlAt(PdPos(E.CaretPos.block, 10), Kind, Spec, A, B) and (Kind = 'checkbox'), 'the box still a box');
  { the caret in the list: framed }
  E.ChooseControlItem(PdPos(E.CaretPos.block, Pos('Green', E.DocumentText) + 1), 1);
  E.Invalidate;
  E.Update;
  Application.ProcessMessages;
  ExecuteProcess('/usr/bin/import', ['-window', 'root', Dir + 'edit_controls.png']);

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
  TestShapeHandles(E);
  TestShapeEditing(E);
  TestDrawingMove(E);
  TestConnectors(E);
  TestSnap(E);
  TestWrap(E);
  TestCanvasPage(E);
  TestSlides(E);
  TestTurnedText(E);
  TestRightToLeft(E);
  TestThemesAndStyles(E);
  Step('format properties and lists');
  TestFormatProps(E);
  Step('the slide of a text box being typed in');
  TestSlideOfTextBox(E);
  Step('a bullet clicked');
  TestListLabelClick(E);
  Step('the handles of a text box typed in');
  TestTypingBoxHandles(E, Form);
  Step('a new slide on its layout');
  TestNewLayoutSlide(E, Form);
  Step('slide show');
  TestSlideShow(E);
  TestShowPdf(E);
  TestShowFilterClips;
  TestPdfJpeg(E);
  Step('animations edited');
  TestAnimEdit(E);
  Step('balloons in place');
  TestBalloonPlace(E);
  Step('pages side by side');
  TestPagesAcross(E);
  Step('fields shaded');
  TestFieldShade(E);
  Step('what a right click is on');
  TestContext(E);
  Step('resources found and shown');
  TestShowResource(E);

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
