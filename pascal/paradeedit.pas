{ TParadeEdit - a rich text editor control for Lazarus built on Parade

  The control owns a Parade document and its page layout and shows the
  pages one under another. Glyphs are rasterized by Parade itself (exact
  area coverage, integer arithmetic) and cached, so what is drawn is what
  the layout computed, on every widgetset.

  Caret and selection anchor are document markers, so they follow every
  edit, undo and redo. All editing goes through document operations;
  consecutive typing is one undo step. }

unit paradeedit;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Controls, Graphics, LCLType, LCLIntf, ExtCtrls, StdCtrls, Forms, Clipbrd,
  IntfGraphics, GraphType, FPImage, LazFileUtils, ctypes, parade;

type
  TParadeFontEntry = record
    Family: string;
    Weight, Italic: Integer;
    Font: Ppd_font;
  end;

  TGlyphBmp = record
    W, H, Left, Top: Integer;
    Alpha: array of Byte;
    KFont: Pointer;               { what it is the bitmap of: the cache's key }
    KGlyph: UInt32;
    KPx: pd_sp;
    KSub: Integer;
  end;
  PGlyphBmp = ^TGlyphBmp;

  { TParadeEdit }

  TParadeEdit = class(TCustomControl)
  private
    FDoc: Ppd_doc;
    FLayout: Ppd_layout;
    FFonts: array of TParadeFontEntry;
    FMathFont: Ppd_font;        { equations are typeset with it }
    FCaret, FAnchor: pd_marker_id;
    FDesiredX: Double;
    FHasDesiredX: Boolean;
    FZoom: Double;
    FScrollY: Integer;
    FScrollBar: TScrollBar;
    FBlink: TTimer;
    FCaretOn: Boolean;
    FGlyphs: array of PGlyphBmp;   { open-addressing hash table by font, glyph, size and subpixel position }
    FGlyphCount: Integer;
    FPics: array of TLazIntfImage; { by resource id - 1: the picture, decoded once }
    FPicSized: array of TLazIntfImage; { ... and at the size it was last drawn }
    FPicDoc: Ppd_doc;              { the document they are of }
    FOrder: array of Int32;        { block id -> reading-order index, -1 = not in the main flow }
    FOrderRev: UInt64;
    FDragging: Boolean;
    FFileName: string;
    FOnChange: TNotifyEvent;
    FModified: Boolean;
    FPageGap: Integer;
    FBack: TBitmap;                { the pages as last drawn, on the display's side: a paint copies from it }
    FBackImg: TLazIntfImage;       { ... and the same pixels here, to find what a redraw changes }
    FBackSig: string;              { what they were drawn for: size, zoom, scroll, layout, selection }
    FLayoutEpoch: Integer;         { counts layout updates }
    function GetPageCount: Integer;
    procedure SetZoom(AValue: Double);
    procedure BlinkTimer(Sender: TObject);
    procedure ScrollBarChange(Sender: TObject);
    procedure ClearGlyphCache;
    procedure ClearPictures;
    function GetPicture(Res: pd_res_id; W, H: Integer): TLazIntfImage;
    function GetGlyphBmp(AFont: Ppd_font; GlyphId: UInt32; PxPerEm: pd_sp; Sub: Integer): PGlyphBmp;
    function PxPerSp: Double;
    function PageTop(Page: Integer): Integer;
    function PageLeft(Page: Integer): Integer;
    function TotalHeight: Integer;
    procedure UpdateScrollBar;
    procedure Relayout;
    procedure Changed;
    procedure BuildOrder;
    function Compare(const A, B: pd_pos): Integer;
    function GetCaretPos: pd_pos;
    function GetAnchorPos: pd_pos;
    function HasSelection: Boolean;
    function SelStart: pd_pos;
    function SelEnd: pd_pos;
    procedure SetCaret(const P: pd_pos; Extend: Boolean);
    function ParaText(Block: pd_block_id): string;
    function NextPos(const P: pd_pos): pd_pos;
    function PrevPos(const P: pd_pos): pd_pos;
    function FirstPara: pd_block_id;
    function LastPos: pd_pos;
    procedure MoveVertical(Dir: Integer; Extend: Boolean);
    procedure MoveLineEdge(ToEnd: Boolean; Extend: Boolean);
    function DeleteSelection: Boolean;
    procedure EnsureCaretVisible;
    function PointToPos(X, Y: Integer; out P: pd_pos): Boolean;
    procedure ToggleCharProp(Mask: UInt32);
    function PropsAt(const P: pd_pos): pd_char_props;
    function FormatAt(const P: pd_pos): pd_format_id;
    procedure PaintPage(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double; DrawCaret: Boolean);
    function BackSignature: string;
    procedure RebuildBack;
    function CaretRect(out R: TRect): Boolean;
  protected
    procedure Paint; override;
    procedure Resize; override;
    procedure KeyDown(var Key: Word; Shift: TShiftState); override;
    procedure UTF8KeyPress(var UTF8Key: TUTF8Char); override;
    procedure MouseDown(Button: TMouseButton; Shift: TShiftState; X, Y: Integer); override;
    procedure MouseMove(Shift: TShiftState; X, Y: Integer); override;
    procedure MouseUp(Button: TMouseButton; Shift: TShiftState; X, Y: Integer); override;
    function DoMouseWheel(Shift: TShiftState; WheelDelta: Integer; MousePos: TPoint): Boolean; override;
    procedure DblClick; override;
    procedure DoEnter; override;
    procedure DoExit; override;
  public
    constructor Create(AOwner: TComponent); override;
    destructor Destroy; override;

    { fonts: the resolver picks the closest registered face for a family }
    procedure AddFont(const Family, FileName: string; Weight: Integer = 400; Italic: Boolean = False);
    procedure AddDefaultFonts;
    { an OpenType math font (Latin Modern Math, STIX Two Math) for equations }
    procedure SetMathFont(const FileName: string);

    procedure NewDocument;
    procedure LoadFromFile(const FileName: string);
    procedure SaveToFile(const FileName: string);
    { the whole of a stream as a document in a format (PD_CONV_*, PD_CONV_JDATA, -1 = detect);
      FileName is only remembered, for a host that holds the bytes itself }
    procedure LoadFromStream(Stream: TStream; Format: Int32; const FileName: string = '');
    { the document in a format (PD_CONV_*; PD_CONV_JDATA is the native document, binary
      JData when Binary); leaves Modified alone }
    procedure SaveToStream(Stream: TStream; Format: Int32; Binary: Boolean = False);
    procedure ExportPDF(const FileName: string);

    { editing, also usable for automation }
    procedure InsertText(const S: string);
    procedure ProcessKey(Key: Word; Shift: TShiftState);
    procedure ClickAt(Page: Integer; XPt, YPt: Double; Extend: Boolean = False);
    procedure SelectAll;
    procedure Undo;
    procedure Redo;
    procedure ToggleBold;
    procedure ToggleItalic;
    procedure ToggleUnderline;
    procedure SetParagraphStyle(const StyleName: string);
    function SelectedText: string;
    procedure CopyToClipboard;
    procedure CutToClipboard;
    procedure PasteFromClipboard;
    { insert data of a format (PD_CONV_*) at the caret, replacing the selection; one undo step }
    procedure PasteData(Data: Pointer; Len: Integer; Format: Int32);
    { the selection in a format (PD_CONV_*) }
    function ExportSelection(Format: Int32): string;
    function DocumentText: string;

    { draw a page into a bitmap at a scale, without the caret (tests, previews, printing) }
    procedure RenderPage(Page: Integer; Bmp: TBitmap; Scale: Double);

    property Doc: Ppd_doc read FDoc;
    property Layout: Ppd_layout read FLayout;
    property CaretPos: pd_pos read GetCaretPos;
    property AnchorPos: pd_pos read GetAnchorPos;
    property PageCount: Integer read GetPageCount;
    property Modified: Boolean read FModified write FModified;
    property FileName: string read FFileName;
  published
    property Align;
    property Anchors;
    property Zoom: Double read FZoom write SetZoom;
    property OnChange: TNotifyEvent read FOnChange write FOnChange;
    property TabStop default True;
  end;

implementation

{ ---------------- pixels ---------------- }

function Floor0(V: Double): Integer; inline;
begin
  Result := Trunc(V);
  if V < Result then
    Dec(Result);
end;

type
  TPixel = packed record
    B, G, R, A: Byte;
  end;
  PPixel = ^TPixel;

{ a picture onto the page at X, Y, over what is there by its alpha }
{ a picture in the page's pixel layout onto the page: opaque runs copied, the rest blended }
procedure BlendPicture(Img, Pic: TLazIntfImage; X, Y: Integer);
var
  PX, PY, A, X0, X1: Integer;
  Src, Dst: PPixel;
begin
  X0 := 0;
  if X < 0 then X0 := -X;
  X1 := Pic.Width;
  if X + X1 > Img.Width then X1 := Img.Width - X;
  if X1 <= X0 then
    Exit;
  for PY := 0 to Pic.Height - 1 do
  begin
    if (Y + PY < 0) or (Y + PY >= Img.Height) then
      Continue;
    Src := PPixel(Pic.GetDataLineStart(PY));
    Inc(Src, X0);
    Dst := PPixel(Img.GetDataLineStart(Y + PY));
    Inc(Dst, X + X0);
    for PX := X0 to X1 - 1 do
    begin
      A := Src^.A;
      if A = 255 then
        Dst^ := Src^
      else if A > 0 then
      begin
        Dst^.R := (Dst^.R * (255 - A) + Src^.R * A) div 255;
        Dst^.G := (Dst^.G * (255 - A) + Src^.G * A) div 255;
        Dst^.B := (Dst^.B * (255 - A) + Src^.B * A) div 255;
        Dst^.A := 255;
      end;
      Inc(Src);
      Inc(Dst);
    end;
  end;
end;

procedure FillRectImg(Img: TLazIntfImage; X0, Y0, X1, Y1: Integer; Col: UInt32; Alpha: Integer);
var
  X, Y: Integer;
  P: PPixel;
  R, G, B: Integer;
begin
  if X0 < 0 then X0 := 0;
  if Y0 < 0 then Y0 := 0;
  if X1 > Img.Width then X1 := Img.Width;
  if Y1 > Img.Height then Y1 := Img.Height;
  R := (Col shr 16) and $FF;
  G := (Col shr 8) and $FF;
  B := Col and $FF;
  if Alpha >= 255 then
  begin   { opaque: the pixel itself, row by row }
    if X1 > X0 then
      for Y := Y0 to Y1 - 1 do
      begin
        P := PPixel(Img.GetDataLineStart(Y));
        Inc(P, X0);
        FillDWord(P^, X1 - X0, UInt32(B) or (UInt32(G) shl 8) or (UInt32(R) shl 16) or $FF000000);
      end;
    Exit;
  end;
  for Y := Y0 to Y1 - 1 do
  begin
    P := PPixel(Img.GetDataLineStart(Y));
    Inc(P, X0);
    for X := X0 to X1 - 1 do
    begin
      P^.R := (P^.R * (255 - Alpha) + R * Alpha) div 255;
      P^.G := (P^.G * (255 - Alpha) + G * Alpha) div 255;
      P^.B := (P^.B * (255 - Alpha) + B * Alpha) div 255;
      P^.A := 255;    { opaque: a zero alpha byte would make the bitmap transparent }
      Inc(P);
    end;
  end;
end;

procedure BlendGlyph(Img: TLazIntfImage; G: PGlyphBmp; X, Y: Integer; Col: UInt32);
var
  Row, Cl, PX, PY, A: Integer;
  Line, Pix: PPixel;
  CR, CG, CB: Integer;
begin
  CR := (Col shr 16) and $FF;
  CG := (Col shr 8) and $FF;
  CB := Col and $FF;
  for Row := 0 to G^.H - 1 do
  begin
    PY := Y - G^.Top + Row;
    if (PY < 0) or (PY >= Img.Height) then
      Continue;
    Line := PPixel(Img.GetDataLineStart(PY));
    for Cl := 0 to G^.W - 1 do
    begin
      PX := X + G^.Left + Cl;
      A := G^.Alpha[Row * G^.W + Cl];
      if (A = 0) or (PX < 0) or (PX >= Img.Width) then
        Continue;
      Pix := Line;
      Inc(Pix, PX);
      Pix^.R := (Pix^.R * (255 - A) + CR * A) div 255;
      Pix^.G := (Pix^.G * (255 - A) + CG * A) div 255;
      Pix^.B := (Pix^.B * (255 - A) + CB * A) div 255;
      Pix^.A := 255;
    end;
  end;
end;

function NewImage(W, H: Integer; Col: UInt32): TLazIntfImage;
var
  Desc: TRawImageDescription;
begin
  Result := TLazIntfImage.Create(0, 0);
  Desc.Init_BPP32_B8G8R8A8_BIO_TTB(W, H);
  Result.DataDescription := Desc;
  FillRectImg(Result, 0, 0, W, H, Col, 255);
end;

function TColorToRGB(C: TColor): UInt32;
var
  V: LongInt;
begin
  V := ColorToRGB(C);
  Result := (UInt32(V and $FF) shl 16) or (UInt32((V shr 8) and $FF) shl 8) or UInt32((V shr 16) and $FF);
end;



const
  SCREEN_PPI = 96.0;

function ResolveFont(user: Pointer; family: PAnsiChar; weight, italic: Int32): Ppd_font; cdecl;
var
  E: TParadeEdit;
  I, Best, Score, BestScore, Cls: Integer;
  Fam: string;
begin
  E := TParadeEdit(user);
  Result := nil;
  Fam := LowerCase(StrPas(family));
  Best := -1;
  BestScore := MaxInt;
  Cls := pd_font_family_class(family);
  for I := 0 to High(E.FFonts) do
  begin
    { family mismatch costs most, less when it is the same kind of face (a sans
      for Arial, a mono for Courier New); then italic; then weight distance }
    Score := Abs(E.FFonts[I].Weight - weight) + 1000 * Ord((E.FFonts[I].Italic <> 0) <> (italic <> 0));
    if (Fam <> '') and (LowerCase(E.FFonts[I].Family) <> Fam) then
      Inc(Score, 100000 - 50000 * Ord(pd_font_family_class(PAnsiChar(E.FFonts[I].Family)) = Cls))
    else if (Fam = '') and (I > 0) and (LowerCase(E.FFonts[I].Family) <> LowerCase(E.FFonts[0].Family)) then
      Inc(Score, 100000);
    if Score < BestScore then
    begin
      BestScore := Score;
      Best := I;
    end;
  end;
  if Best >= 0 then
    Result := E.FFonts[Best].Font;
end;

function WriteToStream(user: Pointer; data: Pointer; len: csize_t): cint; cdecl;
begin
  TStream(user).WriteBuffer(data^, len);
  Result := 0;
end;

{ a picture by its address, for pd_doc_load_images: a path relative to the document's folder (user
  points at it, a string) or absolute, or a file:// URL; nothing is fetched from the network }
function FetchFile(user: Pointer; address: PAnsiChar; write: pd_writer; sink: Pointer): cint; cdecl;
var
  Path: string;
  Ms: TMemoryStream;
begin
  Result := 1;
  Path := StrPas(address);
  if Pos('file://', Path) = 1 then
    Delete(Path, 1, 7)
  else if Pos('://', Path) > 0 then
    Exit;
  if (Path = '') or (Pos('data:', Path) = 1) then
    Exit;
  if not FilenameIsAbsolute(Path) then
    Path := PString(user)^ + Path;
  if not FileExists(Path) then
    Exit;
  Ms := TMemoryStream.Create;
  try
    try
      Ms.LoadFromFile(Path);
      if Ms.Size > 0 then
        Result := write(sink, Ms.Memory, Ms.Size);
    except
      Result := 1;
    end;
  finally
    Ms.Free;
  end;
end;

{ converter format of a file name: PD_CONV_*, PD_CONV_JDATA for .pdoc/.bpdoc, -1 unknown }
function FormatOfFile(const FileName: string): Int32;
var
  E: string;
begin
  E := LowerCase(ExtractFileExt(FileName));
  if (E = '.html') or (E = '.htm') then Exit(PD_CONV_HTML);
  if (E = '.md') or (E = '.markdown') then Exit(PD_CONV_MARKDOWN);
  if E = '.tex' then Exit(PD_CONV_LATEX);
  if E = '.rtf' then Exit(PD_CONV_RTF);
  if E = '.docx' then Exit(PD_CONV_DOCX);
  if E = '.txt' then Exit(PD_CONV_TEXT);
  if (E = '.pdoc') or (E = '.bpdoc') then Exit(PD_CONV_JDATA);
  Result := -1;
end;

var
  CF_Parade, CF_Html, CF_Rtf: TClipboardFormat;

procedure RegisterFormats;
begin
  if CF_Parade <> 0 then
    Exit;
  CF_Parade := RegisterClipboardFormat('application/x-parade');
  {$IFDEF WINDOWS}
  CF_Html := RegisterClipboardFormat('HTML Format');
  CF_Rtf := RegisterClipboardFormat('Rich Text Format');
  {$ELSE}
  CF_Html := RegisterClipboardFormat('text/html');
  CF_Rtf := RegisterClipboardFormat('text/rtf');
  {$ENDIF}
end;

{$IFDEF WINDOWS}
{ Windows wants HTML on the clipboard as a fragment behind a header of byte offsets }
function WrapCFHtml(const Html: string): string;
const
  Head = 'Version:0.9'#13#10'StartHTML:%.10d'#13#10'EndHTML:%.10d'#13#10'StartFragment:%.10d'#13#10 +
    'EndFragment:%.10d'#13#10;
  Pre = '<html><body>'#13#10'<!--StartFragment-->';
  Post = '<!--EndFragment-->'#13#10'</body></html>';
var
  Body: string;
  A, B, H, SF, EF: Integer;
begin
  Body := Html;
  A := Pos('<body>', Body);
  B := Pos('</body>', Body);
  if (A > 0) and (B > A) then
    Body := Copy(Body, A + 6, B - A - 6);
  H := Length(Format(Head, [0, 0, 0, 0]));
  SF := H + Length(Pre);
  EF := SF + Length(Body);
  Result := Format(Head, [H, EF + Length(Post), SF, EF]) + Pre + Body + Post;
end;
{$ENDIF}

{ clipboard text of a format; browsers may give HTML as UTF-16 }
function ReadClip(Fmt: TClipboardFormat; out S: string): Boolean;
var
  Ms: TMemoryStream;
  W: UnicodeString;
begin
  Result := False;
  S := '';
  if (Fmt = 0) or not Clipboard.HasFormat(Fmt) then
    Exit;
  Ms := TMemoryStream.Create;
  try
    if not Clipboard.GetFormat(Fmt, Ms) or (Ms.Size = 0) then
      Exit;
    if (Ms.Size >= 2) and (PByte(Ms.Memory)[0] = $FF) and (PByte(Ms.Memory)[1] = $FE) then
    begin
      SetLength(W, (Ms.Size - 2) div 2);
      if Length(W) > 0 then
        Move(PByte(Ms.Memory)[2], W[1], Length(W) * 2);
      S := UTF8Encode(W);
    end
    else
      SetString(S, PAnsiChar(Ms.Memory), Ms.Size);
    while (S <> '') and (S[Length(S)] = #0) do
      SetLength(S, Length(S) - 1);
    Result := S <> '';
  finally
    Ms.Free;
  end;
end;

procedure AddClip(Fmt: TClipboardFormat; const S: string);
var
  Ss: TStringStream;
begin
  if (Fmt = 0) or (S = '') then
    Exit;
  Ss := TStringStream.Create(S);
  try
    Clipboard.AddFormat(Fmt, Ss);
  finally
    Ss.Free;
  end;
end;

{ TParadeEdit }

constructor TParadeEdit.Create(AOwner: TComponent);
begin
  inherited Create(AOwner);
  ControlStyle := ControlStyle + [csOpaque] - [csSetCaption];
  TabStop := True;
  Color := $00E0E0E0;
  Cursor := crIBeam;       { a text editor's: the scroll bar keeps its own }
  FZoom := 1.0;
  FPageGap := 16;
  FScrollBar := TScrollBar.Create(Self);
  FScrollBar.Kind := sbVertical;
  FScrollBar.Align := alRight;
  FScrollBar.Parent := Self;
  FScrollBar.OnChange := @ScrollBarChange;
  FBlink := TTimer.Create(Self);
  FBlink.Interval := 530;
  FBlink.OnTimer := @BlinkTimer;
  FBlink.Enabled := False;
  NewDocument;
end;

destructor TParadeEdit.Destroy;
var
  I: Integer;
begin
  if FLayout <> nil then
    pd_layout_free(FLayout);
  if FDoc <> nil then
    pd_doc_free(FDoc);
  ClearGlyphCache;
  ClearPictures;
  FBack.Free;
  FBackImg.Free;
  for I := 0 to High(FFonts) do
    pd_font_free(FFonts[I].Font);
  if FMathFont <> nil then
    pd_font_free(FMathFont);
  inherited Destroy;
end;

procedure TParadeEdit.AddFont(const Family, FileName: string; Weight: Integer; Italic: Boolean);
var
  F: Ppd_font;
begin
  ParadeCheck(pd_font_load_file(PAnsiChar(FileName), 0, F), 'font ' + FileName);
  SetLength(FFonts, Length(FFonts) + 1);
  FFonts[High(FFonts)].Family := Family;
  FFonts[High(FFonts)].Weight := Weight;
  FFonts[High(FFonts)].Italic := Ord(Italic);
  FFonts[High(FFonts)].Font := F;
  if FLayout <> nil then
  begin
    pd_layout_invalidate(FLayout);
    Relayout;
  end;
end;

procedure TParadeEdit.AddDefaultFonts;
const
  Dir = '/usr/share/fonts/truetype/';
begin
  if FileExists(Dir + 'liberation/LiberationSerif-Regular.ttf') then
  begin
    AddFont('Liberation Serif', Dir + 'liberation/LiberationSerif-Regular.ttf');
    AddFont('Liberation Serif', Dir + 'liberation/LiberationSerif-Bold.ttf', 700);
    AddFont('Liberation Serif', Dir + 'liberation/LiberationSerif-Italic.ttf', 400, True);
    AddFont('Liberation Serif', Dir + 'liberation/LiberationSerif-BoldItalic.ttf', 700, True);
  end;
  if FileExists(Dir + 'dejavu/DejaVuSansMono.ttf') then
    AddFont('monospace', Dir + 'dejavu/DejaVuSansMono.ttf');
  SetMathFont('/usr/share/texmf/fonts/opentype/public/lm-math/latinmodern-math.otf');
end;

procedure TParadeEdit.SetMathFont(const FileName: string);
var
  F: Ppd_font;
begin
  if not FileExists(FileName) or (pd_font_load_file(PAnsiChar(FileName), 0, F) <> PD_OK) then
    Exit;
  if FMathFont <> nil then
    pd_font_free(FMathFont);
  FMathFont := F;
  if FDoc <> nil then
  begin
    pd_doc_set_math_font(FDoc, FMathFont);
    Relayout;
  end;
end;

procedure TParadeEdit.NewDocument;
var
  D: Ppd_doc;
begin
  ParadeCheck(pd_doc_new(D), 'new document');
  if FLayout <> nil then
    pd_layout_free(FLayout);
  if FDoc <> nil then
    pd_doc_free(FDoc);
  FDoc := D;
  pd_doc_set_font_resolver(FDoc, @ResolveFont, Self);
  pd_doc_set_math_font(FDoc, FMathFont);
  ParadeCheck(pd_layout_new(FDoc, FLayout), 'layout');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_RIGHT, FCaret), 'caret');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_LEFT, FAnchor), 'anchor');
  FScrollY := 0;
  FFileName := '';
  FModified := False;
  FOrderRev := High(UInt64);
  Relayout;
end;

procedure TParadeEdit.LoadFromFile(const FileName: string);
var
  Ms: TMemoryStream;
begin
  Ms := TMemoryStream.Create;
  try
    Ms.LoadFromFile(FileName);
    LoadFromStream(Ms, FormatOfFile(FileName), FileName);
  finally
    Ms.Free;
  end;
end;

procedure TParadeEdit.LoadFromStream(Stream: TStream; Format: Int32; const FileName: string);
var
  Ms: TMemoryStream;
  D: Ppd_doc;
  Base: string;
begin
  Ms := TMemoryStream.Create;
  try
    Ms.CopyFrom(Stream, Stream.Size - Stream.Position);
    if Format < 0 then
      Format := pd_conv_detect(Ms.Memory, Ms.Size);
    if Format = PD_CONV_JDATA then
      ParadeCheck(pd_doc_load(Ms.Memory, Ms.Size, PD_JDATA_AUTO, D), 'load ' + FileName)
    else
      ParadeCheck(pd_doc_import(Ms.Memory, Ms.Size, Format, D), 'import ' + FileName);
  finally
    Ms.Free;
  end;
  if FLayout <> nil then
    pd_layout_free(FLayout);
  if FDoc <> nil then
    pd_doc_free(FDoc);
  FDoc := D;
  { pictures the document only names (Markdown, HTML), from beside the file }
  Base := ExtractFilePath(ExpandFileName(FileName));
  if FileName = '' then
    Base := IncludeTrailingPathDelimiter(GetCurrentDir);
  pd_doc_load_images(FDoc, @FetchFile, @Base);
  pd_doc_set_font_resolver(FDoc, @ResolveFont, Self);
  pd_doc_set_math_font(FDoc, FMathFont);
  ParadeCheck(pd_layout_new(FDoc, FLayout), 'layout');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_RIGHT, FCaret), 'caret');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_LEFT, FAnchor), 'anchor');
  FScrollY := 0;
  FFileName := FileName;
  FModified := False;
  FOrderRev := High(UInt64);
  Relayout;
end;

procedure TParadeEdit.SaveToFile(const FileName: string);
var
  Fs: TFileStream;
  Fmt: Int32;
begin
  if LowerCase(ExtractFileExt(FileName)) = '.pdf' then
  begin
    ExportPDF(FileName);
    Exit;
  end;
  Fmt := FormatOfFile(FileName);
  if Fmt < 0 then
    Fmt := PD_CONV_JDATA;
  Fs := TFileStream.Create(FileName, fmCreate);
  try
    SaveToStream(Fs, Fmt, LowerCase(ExtractFileExt(FileName)) = '.bpdoc');
  finally
    Fs.Free;
  end;
  { only the native format is the document's own file }
  if Fmt = PD_CONV_JDATA then
  begin
    FFileName := FileName;
    FModified := False;
  end;
end;

procedure TParadeEdit.SaveToStream(Stream: TStream; Format: Int32; Binary: Boolean);
begin
  if Format = PD_CONV_JDATA then
  begin
    if Binary then
      ParadeCheck(pd_doc_save(FDoc, PD_JDATA_BINARY, @WriteToStream, Stream), 'save')
    else
      ParadeCheck(pd_doc_save(FDoc, PD_JDATA_TEXT, @WriteToStream, Stream), 'save');
  end
  else
    ParadeCheck(pd_doc_export(FDoc, Format, @WriteToStream, Stream), 'export');
end;

procedure TParadeEdit.ExportPDF(const FileName: string);
var
  Fs: TFileStream;
  Opt: pd_pdf_options;
begin
  pd_pdf_options_init(Opt);
  Fs := TFileStream.Create(FileName, fmCreate);
  try
    ParadeCheck(pd_layout_write_pdf(FLayout, @Opt, @WriteToStream, Fs), 'PDF ' + FileName);
  finally
    Fs.Free;
  end;
end;

{ ---------------- geometry ---------------- }

function TParadeEdit.PxPerSp: Double;
begin
  Result := FZoom * SCREEN_PPI / 72.0 / PD_SP_PER_PT;
end;

function TParadeEdit.GetPageCount: Integer;
begin
  Result := pd_layout_page_count(FLayout);
end;

function TParadeEdit.PageTop(Page: Integer): Integer;
var
  I: Integer;
  Info: pd_page_info;
begin
  Result := FPageGap - FScrollY;
  for I := 0 to Page - 1 do
  begin
    pd_layout_page_info(FLayout, I, Info);
    Inc(Result, Round(Info.height * PxPerSp) + FPageGap);
  end;
end;

function TParadeEdit.PageLeft(Page: Integer): Integer;
var
  Info: pd_page_info;
begin
  pd_layout_page_info(FLayout, Page, Info);
  Result := (ClientWidth - FScrollBar.Width - Round(Info.width * PxPerSp)) div 2;
  if Result < FPageGap then
    Result := FPageGap;
end;

function TParadeEdit.TotalHeight: Integer;
var
  I: Integer;
  Info: pd_page_info;
begin
  Result := FPageGap;
  for I := 0 to PageCount - 1 do
  begin
    pd_layout_page_info(FLayout, I, Info);
    Inc(Result, Round(Info.height * PxPerSp) + FPageGap);
  end;
end;

procedure TParadeEdit.UpdateScrollBar;
var
  Max: Integer;
begin
  Max := TotalHeight - ClientHeight;
  if Max < 0 then
    Max := 0;
  if FScrollY > Max then
    FScrollY := Max;
  FScrollBar.Max := Max + ClientHeight;
  FScrollBar.PageSize := ClientHeight;
  FScrollBar.LargeChange := ClientHeight * 9 div 10;
  FScrollBar.SmallChange := 40;
  FScrollBar.Position := FScrollY;
end;

procedure TParadeEdit.ScrollBarChange(Sender: TObject);
begin
  FScrollY := FScrollBar.Position;
  Invalidate;
end;

procedure TParadeEdit.SetZoom(AValue: Double);
begin
  if AValue < 0.25 then
    AValue := 0.25;
  if AValue > 6 then
    AValue := 6;
  if AValue = FZoom then
    Exit;
  FZoom := AValue;
  ClearGlyphCache;
  UpdateScrollBar;
  Invalidate;
end;

procedure TParadeEdit.Relayout;
begin
  { without fonts there is nothing to lay out yet: show no pages rather than fail }
  if Length(FFonts) > 0 then
    ParadeCheck(pd_layout_update(FLayout, nil), 'layout update');
  Inc(FLayoutEpoch);
  UpdateScrollBar;
  Invalidate;
end;

procedure TParadeEdit.Changed;
begin
  FModified := True;
  FHasDesiredX := False;
  Relayout;
  EnsureCaretVisible;
  FCaretOn := True;
  if Assigned(FOnChange) then
    FOnChange(Self);
end;

procedure TParadeEdit.Resize;
begin
  inherited Resize;
  UpdateScrollBar;
end;

{ ---------------- glyphs ---------------- }

procedure TParadeEdit.ClearGlyphCache;
var
  I: Integer;
begin
  for I := 0 to High(FGlyphs) do
    if FGlyphs[I] <> nil then
      Dispose(FGlyphs[I]);
  FGlyphs := nil;
  FGlyphCount := 0;
end;

procedure TParadeEdit.ClearPictures;
var
  I: Integer;
begin
  for I := 0 to High(FPics) do
  begin
    FPics[I].Free;
    FPicSized[I].Free;
  end;
  FPics := nil;
  FPicSized := nil;
  FPicDoc := nil;
end;

{ a resource of the document as a picture W x H pixels, nil if it is not one the LCL reads }
function TParadeEdit.GetPicture(Res: pd_res_id; W, H: Integer): TLazIntfImage;
var
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
  Ms: TMemoryStream;
  Pic: TPicture;
  Src: TLazIntfImage;
  X, Y, I: Integer;
  Px: PPixel;
  C: TFPColor;
begin
  Result := nil;
  if (Res = 0) or (W <= 0) or (H <= 0) or (W > 8000) or (H > 8000) then
    Exit;
  if FPicDoc <> FDoc then
  begin
    ClearPictures;
    FPicDoc := FDoc;
  end;
  I := Integer(Res) - 1;
  if I >= Length(FPics) then
  begin
    SetLength(FPics, I + 1);
    SetLength(FPicSized, I + 1);
  end;
  if FPics[I] = nil then
  begin
    if pd_doc_resource(FDoc, Res, @Mime, @Data, @Len) <> PD_OK then
      Exit;
    Ms := TMemoryStream.Create;
    Pic := TPicture.Create;
    try
      try
        Ms.WriteBuffer(Data^, Len);
        Ms.Position := 0;
        Pic.LoadFromStream(Ms);
        if Pic.Graphic is TRasterImage then
          FPics[I] := TRasterImage(Pic.Graphic).CreateIntfImage;
      except
        FPics[I] := nil;   { not a picture the LCL can read: the placeholder stays }
      end;
    finally
      Pic.Free;
      Ms.Free;
    end;
    if FPics[I] = nil then
      Exit;
  end;
  { scaled once per size, nearest pixel: the page is redrawn far more often than it is zoomed }
  if (FPicSized[I] = nil) or (FPicSized[I].Width <> W) or (FPicSized[I].Height <> H) then
  begin
    FPicSized[I].Free;
    Src := FPics[I];
    FPicSized[I] := NewImage(W, H, 0);     { the page's own pixel layout: drawn by copying bytes }
    for Y := 0 to H - 1 do
    begin
      Px := PPixel(FPicSized[I].GetDataLineStart(Y));
      for X := 0 to W - 1 do
      begin
        C := Src.Colors[X * Src.Width div W, Y * Src.Height div H];
        Px^.R := C.red shr 8;
        Px^.G := C.green shr 8;
        Px^.B := C.blue shr 8;
        Px^.A := C.alpha shr 8;
        Inc(Px);
      end;
    end;
  end;
  Result := FPicSized[I];
end;

function GlyphHash(AFont: Pointer; GlyphId: UInt32; PxPerEm: pd_sp; Sub: Integer): UInt32; inline;
begin
  Result := (UInt32(PtrUInt(AFont) shr 4) * 2654435761) xor (GlyphId * 40503) xor (UInt32(PxPerEm) * 2246822519) xor
            UInt32(Sub);
  Result := Result xor (Result shr 15);
end;

function TParadeEdit.GetGlyphBmp(AFont: Ppd_font; GlyphId: UInt32; PxPerEm: pd_sp; Sub: Integer): PGlyphBmp;
var
  Info: pd_glyph_image;
  I, Mask: Integer;
  Old: array of PGlyphBmp;
  G: PGlyphBmp;
begin
  if Length(FGlyphs) > 0 then
  begin
    Mask := High(FGlyphs);
    I := GlyphHash(AFont, GlyphId, PxPerEm, Sub) and Mask;
    while FGlyphs[I] <> nil do
    begin
      G := FGlyphs[I];
      if (G^.KFont = AFont) and (G^.KGlyph = GlyphId) and (G^.KPx = PxPerEm) and (G^.KSub = Sub) then
        Exit(G);
      I := (I + 1) and Mask;
    end;
  end;
  if (FGlyphCount + 1) * 10 > Length(FGlyphs) * 7 then
  begin   { grow, and put every bitmap in its new place }
    Old := FGlyphs;
    FGlyphs := nil;
    if Length(Old) = 0 then
      SetLength(FGlyphs, 1024)
    else
      SetLength(FGlyphs, Length(Old) * 2);
    Mask := High(FGlyphs);
    for G in Old do
      if G <> nil then
      begin
        I := GlyphHash(G^.KFont, G^.KGlyph, G^.KPx, G^.KSub) and Mask;
        while FGlyphs[I] <> nil do
          I := (I + 1) and Mask;
        FGlyphs[I] := G;
      end;
  end;
  New(Result);
  Result^.KFont := AFont;
  Result^.KGlyph := GlyphId;
  Result^.KPx := PxPerEm;
  Result^.KSub := Sub;
  Result^.W := 0;
  Result^.H := 0;
  if pd_font_glyph_render(AFont, GlyphId, PxPerEm, Sub, nil, 0, Info) = PD_OK then
  begin
    SetLength(Result^.Alpha, Info.width * Info.height);
    if (Length(Result^.Alpha) = 0) or
       (pd_font_glyph_render(AFont, GlyphId, PxPerEm, Sub, @Result^.Alpha[0], Length(Result^.Alpha), Info) = PD_OK) then
    begin
      Result^.W := Info.width;
      Result^.H := Info.height;
      Result^.Left := Info.left;
      Result^.Top := Info.top;
    end;
  end;
  Mask := High(FGlyphs);
  I := GlyphHash(AFont, GlyphId, PxPerEm, Sub) and Mask;
  while FGlyphs[I] <> nil do
    I := (I + 1) and Mask;
  FGlyphs[I] := Result;
  Inc(FGlyphCount);
end;

{ ---------------- reading order, positions ---------------- }

function TParadeEdit.FirstPara: pd_block_id;
var
  B: pd_block_id;
  Info: pd_block_info;
begin
  B := pd_doc_root(FDoc);
  while (B <> 0) and (pd_doc_block_info(FDoc, B, Info) = PD_OK) and (Info.kind <> PD_BLOCK_PARAGRAPH) do
    B := pd_doc_child(FDoc, B, 0);
  Result := B;
end;

procedure TParadeEdit.BuildOrder;
var
  B: pd_block_id;
  N: Int32;
begin
  if FOrderRev = pd_doc_revision(FDoc) then
    Exit;
  FOrderRev := pd_doc_revision(FDoc);
  SetLength(FOrder, 0);
  N := 0;
  B := FirstPara;
  while B <> 0 do
  begin
    if B >= Length(FOrder) then
      SetLength(FOrder, B * 2 + 16);
    FOrder[B] := N;
    Inc(N);
    B := pd_doc_next_paragraph(FDoc, B);
  end;
end;

function TParadeEdit.Compare(const A, B: pd_pos): Integer;
var
  OA, OB: Int32;
begin
  BuildOrder;
  if A.block < Length(FOrder) then OA := FOrder[A.block] else OA := -1;
  if B.block < Length(FOrder) then OB := FOrder[B.block] else OB := -1;
  if OA <> OB then
    Exit(OA - OB);
  if A.offset < B.offset then Exit(-1);
  if A.offset > B.offset then Exit(1);
  Result := 0;
end;

function TParadeEdit.GetCaretPos: pd_pos;
begin
  pd_doc_marker_get(FDoc, FCaret, Result);
end;

function TParadeEdit.GetAnchorPos: pd_pos;
begin
  pd_doc_marker_get(FDoc, FAnchor, Result);
end;

function TParadeEdit.HasSelection: Boolean;
var
  C, A: pd_pos;
begin
  C := CaretPos;
  A := AnchorPos;
  Result := (C.block <> A.block) or (C.offset <> A.offset);
end;

function TParadeEdit.SelStart: pd_pos;
begin
  if Compare(CaretPos, AnchorPos) <= 0 then Result := CaretPos else Result := AnchorPos;
end;

function TParadeEdit.SelEnd: pd_pos;
begin
  if Compare(CaretPos, AnchorPos) <= 0 then Result := AnchorPos else Result := CaretPos;
end;

procedure TParadeEdit.SetCaret(const P: pd_pos; Extend: Boolean);
begin
  pd_doc_marker_set(FDoc, FCaret, P);
  if not Extend then
    pd_doc_marker_set(FDoc, FAnchor, P);
  pd_doc_seal_undo(FDoc);
  FCaretOn := True;
  EnsureCaretVisible;
  Invalidate;
end;

function TParadeEdit.ParaText(Block: pd_block_id): string;
var
  T: PAnsiChar;
  L: UInt32;
begin
  Result := '';
  if pd_doc_para_text(FDoc, Block, T, L) = PD_OK then
    SetString(Result, T, L);
end;

function TParadeEdit.NextPos(const P: pd_pos): pd_pos;
var
  S: string;
  N: pd_block_id;
begin
  Result := P;
  S := ParaText(P.block);
  if P.offset < UInt32(Length(S)) then
    Result.offset := pd_text_next_grapheme(PAnsiChar(S), Length(S), P.offset)   { a whole grapheme cluster }
  else
  begin
    N := pd_doc_next_paragraph(FDoc, P.block);
    if N <> 0 then
      Result := PdPos(N, 0);
  end;
end;

function TParadeEdit.PrevPos(const P: pd_pos): pd_pos;
var
  S: string;
  N: pd_block_id;
begin
  Result := P;
  if P.offset > 0 then
  begin
    S := ParaText(P.block);
    Result.offset := pd_text_prev_grapheme(PAnsiChar(S), Length(S), P.offset);
  end
  else
  begin
    N := pd_doc_prev_paragraph(FDoc, P.block);
    if N <> 0 then
      Result := PdPos(N, Length(ParaText(N)));
  end;
end;

function TParadeEdit.LastPos: pd_pos;
var
  B, N: pd_block_id;
begin
  B := FirstPara;
  N := B;
  while N <> 0 do
  begin
    B := N;
    N := pd_doc_next_paragraph(FDoc, B);
  end;
  Result := PdPos(B, Length(ParaText(B)));
end;

function TParadeEdit.PointToPos(X, Y: Integer; out P: pd_pos): Boolean;
var
  Page: Integer;
  Info: pd_page_info;
  PTop, H: Integer;
begin
  Result := False;
  for Page := 0 to PageCount - 1 do
  begin
    pd_layout_page_info(FLayout, Page, Info);
    PTop := PageTop(Page);
    H := Round(Info.height * PxPerSp);
    if (Y < PTop + H + FPageGap div 2) or (Page = PageCount - 1) then
    begin
      Result := pd_layout_hit_test(FLayout, Page, Round((X - PageLeft(Page)) / PxPerSp),
        Round((Y - PTop) / PxPerSp), P) = PD_OK;
      Exit;
    end;
  end;
end;

procedure TParadeEdit.ClickAt(Page: Integer; XPt, YPt: Double; Extend: Boolean);
var
  P: pd_pos;
begin
  if pd_layout_hit_test(FLayout, Page, PT(XPt), PT(YPt), P) = PD_OK then
    SetCaret(P, Extend);
end;

procedure TParadeEdit.EnsureCaretVisible;
var
  Page: Int32;
  X, Base, Asc, Desc: pd_sp;
  Y0, Y1: Integer;
begin
  if pd_layout_caret(FLayout, CaretPos, Page, X, Base, Asc, Desc) <> PD_OK then
    Exit;
  Y0 := PageTop(Page) + Round((Base - Asc) * PxPerSp);
  Y1 := PageTop(Page) + Round((Base + Desc) * PxPerSp);
  if Y0 < 0 then
    FScrollY := FScrollY + Y0 - 20
  else if Y1 > ClientHeight then
    FScrollY := FScrollY + Y1 - ClientHeight + 20
  else
    Exit;
  if FScrollY < 0 then
    FScrollY := 0;
  UpdateScrollBar;
  Invalidate;
end;

procedure TParadeEdit.MoveVertical(Dir: Integer; Extend: Boolean);
var
  Page: Int32;
  X, Base, Asc, Desc: pd_sp;
  P: pd_pos;
  Info: pd_page_info;
  Target: pd_sp;
begin
  if pd_layout_caret(FLayout, CaretPos, Page, X, Base, Asc, Desc) <> PD_OK then
    Exit;
  if not FHasDesiredX then
  begin
    FDesiredX := X;
    FHasDesiredX := True;
  end;
  Target := Base + Dir * ((Asc + Desc) * 6 div 5);
  pd_layout_page_info(FLayout, Page, Info);
  { step into the next/previous page when the line leaves this one }
  if pd_layout_hit_test(FLayout, Page, Round(FDesiredX), Target, P) = PD_OK then
  begin
    if (P.block = CaretPos.block) and (P.offset = CaretPos.offset) and (Page + Dir >= 0) and
       (Page + Dir < PageCount) then
    begin
      if Dir > 0 then
        pd_layout_hit_test(FLayout, Page + 1, Round(FDesiredX), 0, P)
      else
      begin
        pd_layout_page_info(FLayout, Page - 1, Info);
        pd_layout_hit_test(FLayout, Page - 1, Round(FDesiredX), Info.height, P);
      end;
    end;
    pd_doc_marker_set(FDoc, FCaret, P);
    if not Extend then
      pd_doc_marker_set(FDoc, FAnchor, P);
    pd_doc_seal_undo(FDoc);
    EnsureCaretVisible;
    Invalidate;
  end;
end;

procedure TParadeEdit.MoveLineEdge(ToEnd: Boolean; Extend: Boolean);
var
  Page: Int32;
  X, Base, Asc, Desc: pd_sp;
  P: pd_pos;
  Info: pd_page_info;
begin
  if pd_layout_caret(FLayout, CaretPos, Page, X, Base, Asc, Desc) <> PD_OK then
    Exit;
  pd_layout_page_info(FLayout, Page, Info);
  if ToEnd then X := Info.width else X := 0;
  if pd_layout_hit_test(FLayout, Page, X, Base - Asc div 2, P) = PD_OK then
    SetCaret(P, Extend);
end;

{ ---------------- editing ---------------- }

function TParadeEdit.DeleteSelection: Boolean;
var
  A, B, Cur, NextB: pd_pos;
  After: pd_pos;
  BI, BJ: pd_block_info;
  Blocks: array of pd_block_id;
  I: Integer;
begin
  Result := HasSelection;
  if not Result then
    Exit;
  A := SelStart;
  B := SelEnd;
  pd_doc_block_info(FDoc, A.block, BI);
  pd_doc_block_info(FDoc, B.block, BJ);
  if (A.block = B.block) or (BI.parent = BJ.parent) then
  begin
    pd_doc_delete(FDoc, PdRange(A, B), @After);
  end
  else
  begin
    { across containers: clear the parts, keep the structure }
    pd_doc_begin_group(FDoc, 'Delete');
    SetLength(Blocks, 0);
    Cur := A;
    while Cur.block <> 0 do
    begin
      SetLength(Blocks, Length(Blocks) + 1);
      Blocks[High(Blocks)] := Cur.block;
      if Cur.block = B.block then
        Break;
      NextB.block := pd_doc_next_paragraph(FDoc, Cur.block);
      Cur.block := NextB.block;
    end;
    for I := High(Blocks) downto 0 do
      if Blocks[I] = A.block then
        pd_doc_delete(FDoc, PdRange(A, PdPos(A.block, Length(ParaText(A.block)))), nil)
      else if Blocks[I] = B.block then
        pd_doc_delete(FDoc, PdRange(PdPos(B.block, 0), B), nil)
      else
        pd_doc_delete(FDoc, PdRange(PdPos(Blocks[I], 0), PdPos(Blocks[I], Length(ParaText(Blocks[I])))), nil);
    pd_doc_end_group(FDoc);
    After := A;
  end;
  pd_doc_marker_set(FDoc, FCaret, After);
  pd_doc_marker_set(FDoc, FAnchor, After);
end;

function TParadeEdit.FormatAt(const P: pd_pos): pd_format_id;
var
  Runs: array of pd_run;
  N, I: Int32;
begin
  Result := PD_FORMAT_INHERIT;
  pd_doc_para_runs(FDoc, P.block, nil, 0, N);
  if N = 0 then
    Exit;
  SetLength(Runs, N);
  pd_doc_para_runs(FDoc, P.block, @Runs[0], N, N);
  for I := 0 to N - 1 do
    if (P.offset >= Runs[I].start) and (P.offset < Runs[I].finish) then
      Exit(Runs[I].format);
end;

procedure TParadeEdit.InsertText(const S: string);
var
  Lines: TStringList;
  I: Integer;
  After: pd_pos;
  Grouped: Boolean;
  Fmt: pd_format_id;
begin
  if S = '' then
    Exit;
  Grouped := HasSelection or (Pos(#10, S) > 0);
  { text typed over a selection takes the format of the selection's first character }
  Fmt := PD_FORMAT_INHERIT;
  if HasSelection then
    Fmt := FormatAt(SelStart);
  if Grouped then
    pd_doc_begin_group(FDoc, 'Insert');
  DeleteSelection;
  if Pos(#10, S) = 0 then
    pd_doc_insert_text(FDoc, CaretPos, PAnsiChar(S), Length(S), Fmt, @After)
  else
  begin
    { pasted lines become paragraphs }
    Lines := TStringList.Create;
    try
      Lines.Text := StringReplace(S, #13, '', [rfReplaceAll]);
      After := CaretPos;
      for I := 0 to Lines.Count - 1 do
      begin
        if I > 0 then
          pd_doc_split(FDoc, After, @After);
        if Lines[I] <> '' then
          pd_doc_insert_text(FDoc, After, PAnsiChar(Lines[I]), Length(Lines[I]), PD_FORMAT_INHERIT, @After);
      end;
    finally
      Lines.Free;
    end;
  end;
  if Grouped then
    pd_doc_end_group(FDoc);
  pd_doc_marker_set(FDoc, FCaret, After);
  pd_doc_marker_set(FDoc, FAnchor, After);
  Changed;
end;

function TParadeEdit.PropsAt(const P: pd_pos): pd_char_props;
var
  Runs: array of pd_run;
  N, I: Int32;
  F: pd_format_id;
begin
  FillChar(Result, SizeOf(Result), 0);
  F := 0;
  pd_doc_para_runs(FDoc, P.block, nil, 0, N);
  if N > 0 then
  begin
    SetLength(Runs, N);
    pd_doc_para_runs(FDoc, P.block, @Runs[0], N, N);
    F := Runs[N - 1].format;
    for I := 0 to N - 1 do
      if (P.offset >= Runs[I].start) and (P.offset < Runs[I].finish) then
      begin
        F := Runs[I].format;
        Break;
      end;
  end;
  pd_doc_format_resolve(FDoc, P.block, F, Result);
end;

procedure TParadeEdit.ToggleCharProp(Mask: UInt32);
var
  Cur, Props: pd_char_props;
begin
  if not HasSelection then
    Exit;
  Cur := PropsAt(SelStart);   { the new state is the opposite of the selection start's }
  FillChar(Props, SizeOf(Props), 0);
  Props.mask := Mask;
  if Mask = PD_CP_WEIGHT then
  begin
    if Cur.weight >= 600 then Props.weight := 400 else Props.weight := 700;
  end
  else if Mask = PD_CP_ITALIC then
    Props.italic := Ord(Cur.italic = 0)
  else if Mask = PD_CP_UNDERLINE then
    Props.underline := Ord(Cur.underline = 0);
  pd_doc_set_char_props(FDoc, PdRange(SelStart, SelEnd), Props);
  Changed;
end;

procedure TParadeEdit.ToggleBold;
begin
  ToggleCharProp(PD_CP_WEIGHT);
end;

procedure TParadeEdit.ToggleItalic;
begin
  ToggleCharProp(PD_CP_ITALIC);
end;

procedure TParadeEdit.ToggleUnderline;
begin
  ToggleCharProp(PD_CP_UNDERLINE);
end;

procedure TParadeEdit.SetParagraphStyle(const StyleName: string);
var
  St: pd_style_id;
  B: pd_block_id;
  Level: Integer;
begin
  St := pd_doc_style_find(FDoc, PAnsiChar(StyleName));
  if St = 0 then
    Exit;
  pd_doc_begin_group(FDoc, 'Paragraph style');
  B := SelStart.block;
  while B <> 0 do
  begin
    pd_doc_set_para_style(FDoc, B, St);
    if Copy(StyleName, 1, 8) = 'Heading ' then
    begin
      Level := StrToIntDef(Copy(StyleName, 9, 1), 1);
      pd_doc_set_role(FDoc, B, PD_ROLE_HEADING, Level);
    end
    else
      pd_doc_set_role(FDoc, B, PD_ROLE_BODY, 0);
    if B = SelEnd.block then
      Break;
    B := pd_doc_next_paragraph(FDoc, B);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.Undo;
begin
  if pd_doc_undo(FDoc) = PD_OK then
  begin
    pd_doc_marker_set(FDoc, FAnchor, CaretPos);
    Changed;
  end;
end;

procedure TParadeEdit.Redo;
begin
  if pd_doc_redo(FDoc) = PD_OK then
  begin
    pd_doc_marker_set(FDoc, FAnchor, CaretPos);
    Changed;
  end;
end;

procedure TParadeEdit.SelectAll;
begin
  pd_doc_marker_set(FDoc, FAnchor, PdPos(FirstPara, 0));
  pd_doc_marker_set(FDoc, FCaret, LastPos);
  Invalidate;
end;

function TParadeEdit.SelectedText: string;
var
  A, B: pd_pos;
  Blk: pd_block_id;
  S: string;
  Lo, Hi: UInt32;
begin
  Result := '';
  if not HasSelection then
    Exit;
  A := SelStart;
  B := SelEnd;
  Blk := A.block;
  while Blk <> 0 do
  begin
    S := ParaText(Blk);
    if Blk = A.block then Lo := A.offset else Lo := 0;
    if Blk = B.block then Hi := B.offset else Hi := Length(S);
    { inline objects (U+FFFC) do not travel as text }
    Result := Result + StringReplace(Copy(S, Lo + 1, Hi - Lo), #$EF#$BF#$BC, '', [rfReplaceAll]);
    if Blk = B.block then
      Break;
    Result := Result + LineEnding;
    Blk := pd_doc_next_paragraph(FDoc, Blk);
  end;
end;

function TParadeEdit.DocumentText: string;
var
  Blk: pd_block_id;
begin
  Result := '';
  Blk := FirstPara;
  while Blk <> 0 do
  begin
    Result := Result + ParaText(Blk);
    Blk := pd_doc_next_paragraph(FDoc, Blk);
    if Blk <> 0 then
      Result := Result + #10;
  end;
end;

function TParadeEdit.ExportSelection(Format: Int32): string;
var
  Ss: TStringStream;
begin
  Result := '';
  if not HasSelection then
    Exit;
  Ss := TStringStream.Create('');
  try
    if pd_doc_export_range(FDoc, PdRange(SelStart, SelEnd), Format, @WriteToStream, Ss) = PD_OK then
      Result := Ss.DataString;
  finally
    Ss.Free;
  end;
end;

{ the selection as plain text, HTML, RTF and Parade's own format, for any application to take }
procedure TParadeEdit.CopyToClipboard;
var
  Native, Html, Rtf: string;
begin
  if not HasSelection then
    Exit;
  RegisterFormats;
  Native := ExportSelection(PD_CONV_JDATA);
  Html := ExportSelection(PD_CONV_HTML);
  Rtf := ExportSelection(PD_CONV_RTF);
  Clipboard.Open;
  try
    Clipboard.AsText := SelectedText;   { clears the clipboard: first }
    AddClip(CF_Parade, Native);
    {$IFDEF WINDOWS}
    AddClip(CF_Html, WrapCFHtml(Html));
    {$ELSE}
    AddClip(CF_Html, Html);
    {$ENDIF}
    AddClip(CF_Rtf, Rtf);
  finally
    Clipboard.Close;
  end;
end;

procedure TParadeEdit.CutToClipboard;
begin
  if HasSelection then
  begin
    CopyToClipboard;
    DeleteSelection;
    Changed;
  end;
end;

procedure TParadeEdit.PasteData(Data: Pointer; Len: Integer; Format: Int32);
var
  After: pd_pos;
begin
  if Len <= 0 then
    Exit;
  pd_doc_begin_group(FDoc, 'Paste');
  DeleteSelection;
  if pd_doc_paste(FDoc, CaretPos, Data, Len, Format, @After) = PD_OK then
  begin
    pd_doc_marker_set(FDoc, FCaret, After);
    pd_doc_marker_set(FDoc, FAnchor, After);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

{ the richest format on the clipboard: Parade's own, then HTML, RTF, plain text }
procedure TParadeEdit.PasteFromClipboard;
var
  S: string;
begin
  RegisterFormats;
  if ReadClip(CF_Parade, S) then
    PasteData(PAnsiChar(S), Length(S), PD_CONV_JDATA)
  else if ReadClip(CF_Html, S) then
    PasteData(PAnsiChar(S), Length(S), PD_CONV_HTML)
  else if ReadClip(CF_Rtf, S) then
    PasteData(PAnsiChar(S), Length(S), PD_CONV_RTF)
  else
    InsertText(Clipboard.AsText);
end;

procedure TParadeEdit.ProcessKey(Key: Word; Shift: TShiftState);
var
  Ext: Boolean;
  P, After: pd_pos;
  BI, BJ: pd_block_info;
begin
  Ext := ssShift in Shift;
  if ssCtrl in Shift then
  begin
    case Key of
      VK_Z: Undo;
      VK_Y: Redo;
      VK_A: SelectAll;
      VK_B: ToggleBold;
      VK_I: ToggleItalic;
      VK_U: ToggleUnderline;
      VK_C: CopyToClipboard;
      VK_X: CutToClipboard;
      VK_V: PasteFromClipboard;
      VK_HOME: SetCaret(PdPos(FirstPara, 0), Ext);
      VK_END: SetCaret(LastPos, Ext);
      VK_ADD, VK_OEM_PLUS: Zoom := Zoom * 1.25;
      VK_SUBTRACT, VK_OEM_MINUS: Zoom := Zoom / 1.25;
    end;
    Exit;
  end;
  case Key of
    VK_LEFT:
      if HasSelection and not Ext then SetCaret(SelStart, False) else SetCaret(PrevPos(CaretPos), Ext);
    VK_RIGHT:
      if HasSelection and not Ext then SetCaret(SelEnd, False) else SetCaret(NextPos(CaretPos), Ext);
    VK_UP: MoveVertical(-1, Ext);
    VK_DOWN: MoveVertical(1, Ext);
    VK_HOME: MoveLineEdge(False, Ext);
    VK_END: MoveLineEdge(True, Ext);
    VK_PRIOR: begin FScrollY := FScrollY - ClientHeight; if FScrollY < 0 then FScrollY := 0; UpdateScrollBar; Invalidate; end;
    VK_NEXT: begin FScrollY := FScrollY + ClientHeight; UpdateScrollBar; Invalidate; end;
    VK_RETURN:
      begin
        pd_doc_begin_group(FDoc, 'New paragraph');
        DeleteSelection;
        if ssShift in Shift then
          pd_doc_insert_text(FDoc, CaretPos, #10, 1, PD_FORMAT_INHERIT, @After)
        else
          pd_doc_split(FDoc, CaretPos, @After);
        pd_doc_end_group(FDoc);
        pd_doc_marker_set(FDoc, FCaret, After);
        pd_doc_marker_set(FDoc, FAnchor, After);
        Changed;
      end;
    VK_BACK, VK_DELETE:
      begin
        if not DeleteSelection then
        begin
          if Key = VK_BACK then
          begin
            P := PrevPos(CaretPos);
            if Compare(P, CaretPos) < 0 then
            begin
              pd_doc_block_info(FDoc, P.block, BI);
              pd_doc_block_info(FDoc, CaretPos.block, BJ);
              if (P.block = CaretPos.block) or (BI.parent = BJ.parent) then
                pd_doc_delete(FDoc, PdRange(P, CaretPos), nil);
            end;
          end
          else
          begin
            P := NextPos(CaretPos);
            if Compare(CaretPos, P) < 0 then
            begin
              pd_doc_block_info(FDoc, P.block, BI);
              pd_doc_block_info(FDoc, CaretPos.block, BJ);
              if (P.block = CaretPos.block) or (BI.parent = BJ.parent) then
                pd_doc_delete(FDoc, PdRange(CaretPos, P), nil);
            end;
          end;
        end;
        pd_doc_marker_set(FDoc, FAnchor, CaretPos);
        Changed;
      end;
    VK_TAB: InsertText(#9);
  end;
end;

procedure TParadeEdit.KeyDown(var Key: Word; Shift: TShiftState);
begin
  inherited KeyDown(Key, Shift);
  if Key in [VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_RETURN, VK_BACK,
             VK_DELETE, VK_TAB] then
  begin
    ProcessKey(Key, Shift);
    Key := 0;
  end
  else if (ssCtrl in Shift) and (Key in [VK_Z, VK_Y, VK_A, VK_B, VK_I, VK_U, VK_C, VK_X, VK_V, VK_ADD,
          VK_SUBTRACT, VK_OEM_PLUS, VK_OEM_MINUS]) then
  begin
    ProcessKey(Key, Shift);
    Key := 0;
  end;
end;

procedure TParadeEdit.UTF8KeyPress(var UTF8Key: TUTF8Char);
begin
  inherited UTF8KeyPress(UTF8Key);
  if (Length(UTF8Key) > 0) and (Ord(UTF8Key[1]) >= 32) and (UTF8Key <> #127) then
    InsertText(UTF8Key);
  UTF8Key := '';
end;

{ ---------------- mouse ---------------- }

procedure TParadeEdit.MouseDown(Button: TMouseButton; Shift: TShiftState; X, Y: Integer);
var
  P: pd_pos;
begin
  inherited MouseDown(Button, Shift, X, Y);
  SetFocus;
  if (Button = mbLeft) and PointToPos(X, Y, P) then
  begin
    FHasDesiredX := False;
    SetCaret(P, ssShift in Shift);
    FDragging := True;
  end;
end;

procedure TParadeEdit.MouseMove(Shift: TShiftState; X, Y: Integer);
var
  P: pd_pos;
begin
  inherited MouseMove(Shift, X, Y);
  if FDragging and PointToPos(X, Y, P) then
  begin
    pd_doc_marker_set(FDoc, FCaret, P);
    Invalidate;
  end;
end;

procedure TParadeEdit.MouseUp(Button: TMouseButton; Shift: TShiftState; X, Y: Integer);
begin
  inherited MouseUp(Button, Shift, X, Y);
  FDragging := False;
end;

procedure TParadeEdit.DblClick;
var
  S: string;
  A, B: UInt32;

  function IsWord(I: UInt32): Boolean;
  begin
    Result := (I < UInt32(Length(S))) and ((S[I + 1] in ['0'..'9', 'A'..'Z', 'a'..'z', '_']) or (Ord(S[I + 1]) >= $80));
  end;

begin
  inherited DblClick;
  FDragging := False;
  S := ParaText(CaretPos.block);
  A := CaretPos.offset;
  B := A;
  while (A > 0) and IsWord(A - 1) do
    Dec(A);
  while IsWord(B) do
    Inc(B);
  pd_doc_marker_set(FDoc, FAnchor, PdPos(CaretPos.block, A));
  pd_doc_marker_set(FDoc, FCaret, PdPos(CaretPos.block, B));
  Invalidate;
end;

function TParadeEdit.DoMouseWheel(Shift: TShiftState; WheelDelta: Integer; MousePos: TPoint): Boolean;
begin
  Result := True;
  if ssCtrl in Shift then
  begin
    if WheelDelta > 0 then Zoom := Zoom * 1.1 else Zoom := Zoom / 1.1;
    Exit;
  end;
  FScrollY := FScrollY - WheelDelta;
  if FScrollY < 0 then
    FScrollY := 0;
  UpdateScrollBar;
  Invalidate;
end;

procedure TParadeEdit.DoEnter;
begin
  inherited DoEnter;
  FBlink.Enabled := True;
  FCaretOn := True;
  Invalidate;
end;

procedure TParadeEdit.DoExit;
begin
  inherited DoExit;
  FBlink.Enabled := False;
  FCaretOn := False;
  Invalidate;
end;

procedure TParadeEdit.BlinkTimer(Sender: TObject);
var
  R: TRect;
begin
  FCaretOn := not FCaretOn;
  { only the caret's few pixels: the pages under it come back from the back buffer }
  if HandleAllocated and CaretRect(R) then
    InvalidateRect(Handle, @R, False)
  else
    Invalidate;
end;

{ ---------------- painting ---------------- }

type
  TSelBand = record
    Block: pd_block_id;
    Y, X0, X1, LineEnd: pd_sp;
    Off: Int64;
    LastSel: Boolean;
  end;

procedure TParadeEdit.PaintPage(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double; DrawCaret: Boolean);
var
  Bands: array of TSelBand;
  NB, K: Integer;
  Info: pd_page_info;
  Items: array of pd_draw;
  N, I, PW, PH, IX, IY, Sub: Integer;
  PX: Double;
  Sel: Boolean;
  A, B, Q: pd_pos;
  G: PGlyphBmp;
  Pic: TLazIntfImage;
  CPage: Int32;
  CX, CBase, CAsc, CDesc: pd_sp;
begin
  { PxScale, not Scale: pd_draw has a field named scale, and the WITH below
    would read that one -- 65536, font expansion -- in place of this }
  pd_layout_page_info(FLayout, Page, Info);
  PW := Round(Info.width * PxScale);
  PH := Round(Info.height * PxScale);
  FillRectImg(Img, OX - 1, OY - 1, OX + PW + 1, OY + PH + 1, $00909090, 255);   { frame }
  FillRectImg(Img, OX, OY, OX + PW, OY + PH, $00FFFFFF, 255);
  if pd_layout_page_items(FLayout, Page, nil, 0, N) <> PD_OK then
    Exit;
  SetLength(Items, N + 1);
  pd_layout_page_items(FLayout, Page, @Items[0], N, N);

  { selection behind the text: one band per line, the line's full height, from
    the first selected character to the last -- the spaces between them too --
    and on to the end of the line's text where the selection goes on past it }
  Sel := HasSelection;
  if Sel then
  begin
    A := SelStart;
    B := SelEnd;
    NB := 0;
    SetLength(Bands, 0);
    for I := 0 to N - 1 do
      if ((Items[I].kind = PD_DRAW_GLYPH) or (Items[I].kind = PD_DRAW_IMAGE)) and (Items[I].region = 0) then
      begin
        { the band of this line: the last one, or a new one }
        K := NB - 1;
        while (K >= 0) and ((Bands[K].Block <> Items[I].block) or (Bands[K].Y <> Items[I].y)) do
          Dec(K);
        if K < 0 then
        begin
          if NB >= Length(Bands) then
            SetLength(Bands, NB * 2 + 16);
          K := NB;
          Inc(NB);
          Bands[K].Block := Items[I].block;
          Bands[K].Y := Items[I].y;
          Bands[K].X0 := High(pd_sp);
          Bands[K].X1 := Low(pd_sp);
          Bands[K].LineEnd := Low(pd_sp);
          Bands[K].Off := -1;
          Bands[K].LastSel := False;
        end;
        if Items[I].x + Items[I].w > Bands[K].LineEnd then
          Bands[K].LineEnd := Items[I].x + Items[I].w;
        Q := PdPos(Items[I].block, Items[I].offset);
        Bands[K].LastSel := (Compare(Q, A) >= 0) and (Compare(Q, B) < 0);
        if Bands[K].LastSel then
        begin
          if Items[I].x < Bands[K].X0 then
          begin
            Bands[K].X0 := Items[I].x;
            Bands[K].Off := Items[I].offset;
          end;
          if Items[I].x + Items[I].w > Bands[K].X1 then
            Bands[K].X1 := Items[I].x + Items[I].w;
        end;
      end;
    for K := 0 to NB - 1 do
      with Bands[K] do
        if (Off >= 0) and (X1 > X0) then
        begin
          { the line's height where it is, as the caret has it }
          if pd_layout_caret(FLayout, PdPos(Block, Off), CPage, CX, CBase, CAsc, CDesc) <> PD_OK then
          begin
            CBase := Y;
            CAsc := 0;
            CDesc := 0;
          end;
          if LastSel then   { its last character is selected: the selection goes on, through the line's end }
            X1 := LineEnd + Round(4 / PxScale);
          FillRectImg(Img, OX + Floor0(X0 * PxScale), OY + Round((Y - CAsc) * PxScale), OX + Round(X1 * PxScale),
            OY + Round((Y + CDesc) * PxScale), $003390FF, 80);
        end;
  end;

  for I := 0 to N - 1 do
    with Items[I] do
      case kind of
        PD_DRAW_RULE:
          FillRectImg(Img, OX + Round(x * PxScale), OY + Round(y * PxScale), OX + Round((x + w) * PxScale) + 1,
            OY + Round((y + h) * PxScale) + 1, color and $FFFFFF, 255);
        PD_DRAW_IMAGE:
          begin
            IX := OX + Round(x * PxScale);
            IY := OY + Round(y * PxScale);
            Pic := GetPicture(resource, OX + Round((x + w) * PxScale) - IX, OY + Round((y + h) * PxScale) - IY);
            if Pic <> nil then
              BlendPicture(Img, Pic, IX, IY)
            else
            begin   { not loaded, or not a picture: a frame in its place }
              FillRectImg(Img, IX, IY, OX + Round((x + w) * PxScale), OY + Round((y + h) * PxScale), $004A90D9, 255);
              FillRectImg(Img, IX + 1, IY + 1, OX + Round((x + w) * PxScale) - 1, OY + Round((y + h) * PxScale) - 1,
                $00DFE9F5, 255);
            end;
          end;
        PD_DRAW_BOX:
          FillRectImg(Img, OX + Round(x * PxScale), OY + Round(y * PxScale), OX + Round((x + w) * PxScale),
            OY + Round((y + h) * PxScale), $00FBE3C0, 255);
        PD_DRAW_GLYPH:
          if font <> nil then
          begin
            PX := OX + x * PxScale;
            IX := Floor0(PX);
            Sub := Round((PX - IX) * 4) * 64;   { quarter-pixel positions keep the cache small }
            if Sub >= 256 then
            begin
              Inc(IX);
              Sub := 0;
            end;
            IY := OY + Round(y * PxScale);
            G := GetGlyphBmp(font, glyph, Round(size * PxScale * PD_SP_PER_PT), Sub);
            if G^.W > 0 then
              BlendGlyph(Img, G, IX, IY, color and $FFFFFF);
          end;
      end;

  if DrawCaret and (pd_layout_caret(FLayout, CaretPos, CPage, CX, CBase, CAsc, CDesc) = PD_OK) and (CPage = Page) then
    FillRectImg(Img, OX + Round(CX * PxScale), OY + Round((CBase - CAsc) * PxScale), OX + Round(CX * PxScale) + 2,
      OY + Round((CBase + CDesc) * PxScale), $00000000, 255);
end;

{ the caret in client pixels, if it is on a page in view }
function TParadeEdit.CaretRect(out R: TRect): Boolean;
var
  CPage: Int32;
  CX, CBase, CAsc, CDesc: pd_sp;
  OX, OY: Integer;
begin
  Result := (FLayout <> nil) and (PageCount > 0) and
            (pd_layout_caret(FLayout, CaretPos, CPage, CX, CBase, CAsc, CDesc) = PD_OK) and (CPage < PageCount);
  if not Result then
    Exit;
  OX := PageLeft(CPage);
  OY := PageTop(CPage);
  R := Rect(OX + Round(CX * PxPerSp), OY + Round((CBase - CAsc) * PxPerSp), OX + Round(CX * PxPerSp) + 2,
    OY + Round((CBase + CDesc) * PxPerSp));
  Result := (R.Bottom > 0) and (R.Top < ClientHeight);
end;

{ everything the drawn pages depend on, but the caret }
function TParadeEdit.BackSignature: string;
var
  A, B: pd_pos;
begin
  Result := Format('%d %d %g %d %d %p %d', [ClientWidth, ClientHeight, FZoom, FScrollY, FLayoutEpoch, Pointer(FDoc),
    Int64(pd_doc_revision(FDoc))]);
  if HasSelection then
  begin
    A := SelStart;
    B := SelEnd;
    Result := Result + Format(' %d:%d-%d:%d', [A.block, A.offset, B.block, B.offset]);
  end;
end;

{ Draw the pages in view again and send the display only what changed: the
   rows and columns that differ from the last drawing. Over a remote X
   connection the whole window would be megabytes for every keystroke. }
procedure TParadeEdit.RebuildBack;
var
  Img, Part: TLazIntfImage;
  W, H, Page, PTop, Y, X, X0, X1, Y0, Y1, RowBytes: Integer;
  Info: pd_page_info;
  P, Q: PByte;
  Bmp: TBitmap;
  Full: Boolean;
begin
  W := ClientWidth - FScrollBar.Width;
  H := ClientHeight;
  if (W <= 0) or (H <= 0) then
    Exit;
  Img := NewImage(W, H, TColorToRGB(Color));
  for Page := 0 to PageCount - 1 do
  begin
    PTop := PageTop(Page);
    pd_layout_page_info(FLayout, Page, Info);
    if PTop + Round(Info.height * PxPerSp) < 0 then
      Continue;
    if PTop > H then
      Break;
    PaintPage(Img, Page, PageLeft(Page), PTop, PxPerSp, False);
  end;

  Full := (FBack = nil) or (FBackImg = nil) or (FBackImg.Width <> W) or (FBackImg.Height <> H);
  RowBytes := W * 4;
  X0 := W;
  X1 := -1;
  Y0 := H;
  Y1 := -1;
  if Full then
  begin
    X0 := 0; Y0 := 0; X1 := W - 1; Y1 := H - 1;
  end
  else
    for Y := 0 to H - 1 do
    begin
      P := Img.GetDataLineStart(Y);
      Q := FBackImg.GetDataLineStart(Y);
      if CompareMem(P, Q, RowBytes) then
        Continue;
      if Y < Y0 then Y0 := Y;
      Y1 := Y;
      X := 0;
      while (X < X0) and (PUInt32(P)[X] = PUInt32(Q)[X]) do
        Inc(X);
      if X < X0 then X0 := X;
      X := W - 1;
      while (X > X1) and (PUInt32(P)[X] = PUInt32(Q)[X]) do
        Dec(X);
      if X > X1 then X1 := X;
    end;

  if Full then
  begin   { the display's own kind of bitmap, which takes drawing; one loaded from an image may not }
    FBack.Free;
    FBack := TBitmap.Create;
    FBack.PixelFormat := pf24bit;
    FBack.SetSize(W, H);
  end;
  if Y1 >= Y0 then
  begin   { just the changed rectangle (all of it, the first time) }
    Part := NewImage(X1 - X0 + 1, Y1 - Y0 + 1, 0);
    Bmp := TBitmap.Create;
    try
      for Y := Y0 to Y1 do
        Move(PUInt32(Img.GetDataLineStart(Y))[X0], Part.GetDataLineStart(Y - Y0)^, (X1 - X0 + 1) * 4);
      Bmp.LoadFromIntfImage(Part);
      FBack.Canvas.Draw(X0, Y0, Bmp);
    finally
      Bmp.Free;
      Part.Free;
    end;
  end;
  FBackImg.Free;
  FBackImg := Img;
end;

procedure TParadeEdit.Paint;
var
  Sig: string;
  R: TRect;
begin
  if (ClientWidth - FScrollBar.Width <= 0) or (ClientHeight <= 0) then
    Exit;
  Sig := BackSignature;
  if (Sig <> FBackSig) or (FBack = nil) then
  begin
    RebuildBack;
    FBackSig := Sig;
  end;
  if FBack = nil then
    Exit;
  R := Canvas.ClipRect;
  if R.Right > FBack.Width then R.Right := FBack.Width;
  if R.Bottom > FBack.Height then R.Bottom := FBack.Height;
  if (R.Right > R.Left) and (R.Bottom > R.Top) then
    Canvas.CopyRect(R, FBack.Canvas, R);     { on the display's side: no pixels cross the connection }
  if Focused and FCaretOn and CaretRect(R) then
  begin
    Canvas.Brush.Style := bsSolid;
    Canvas.Brush.Color := clBlack;
    Canvas.FillRect(R);
  end;
end;

procedure TParadeEdit.RenderPage(Page: Integer; Bmp: TBitmap; Scale: Double);
var
  Img: TLazIntfImage;
  Info: pd_page_info;
  OldZoom: Double;
begin
  pd_layout_page_info(FLayout, Page, Info);
  OldZoom := FZoom;
  Img := NewImage(Round(Info.width * Scale) + 2, Round(Info.height * Scale) + 2, $00FFFFFF);
  try
    PaintPage(Img, Page, 1, 1, Scale, False);
    Bmp.LoadFromIntfImage(Img);
  finally
    Img.Free;
  end;
  FZoom := OldZoom;
end;

end.
