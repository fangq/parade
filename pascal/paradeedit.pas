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
  IntfGraphics, GraphType, FPImage, LazFileUtils, LazUTF8, Math, ctypes, Menus, ExtDlgs, fpjson, jsonparser, parade,
  paradefonts, StrUtils, base64;

type
  TParadeFontEntry = record
    Family: string;
    Weight, Italic: Integer;
    Font: Ppd_font;             { nil until the face is first used, for one registered from a file (AddFontFile) }
    FromDoc: Boolean;           { carried by the document (a DOCX's embedded font): dropped with it }
    FileName: string;           { where a face not loaded yet is }
    FaceIndex: Integer;         { its face in a collection }
    Lazy: Boolean;              { registered to be loaded when used: a system font }
    Failed: Boolean;            { its file would not load: passed over }
    Key: string;                { the family, lower case }
    Cls: Integer;               { pd_font_family_class of the family }
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

  TParadeBlockArray = array of pd_block_id;

  { a place a cross-reference can point at }
  TParadeRefTarget = record
    Block: pd_block_id;
    IsCaption: Boolean;        { a caption (with its number); else a heading }
    Seq: string;               { the caption's sequence: Figure, Table, Equation }
    Number: Integer;           { the caption's number, as the layout counts them }
    Level: Integer;            { the heading's level }
    Text: string;              { the paragraph's text, objects left out }
  end;
  TParadeRefTargets = array of TParadeRefTarget;
  TParadeRefWhat = (prfLabel, prfNumber, prfPage, prfText);

  { an undo or redo done elsewhere (a collaboration binding's own-edits undo); True when it did one }
  TParadeUndoEvent = function(Sender: TObject; Redo: Boolean): Boolean of object;

  { someone else's caret and selection in a shared document }
  TParadeRemoteCaret = record
    Pos, Anchor: pd_pos;
    Color: UInt32;              { $RRGGBB }
    Name: string;
  end;

  { a margin balloon as last drawn: where a click on it selects the text it is about }
  { a shape of a drawing, as its description says: its place among the drawing's shapes and its box, in the
    drawing's own units (sp) }
  TParadeShapeBox = record
    Sid: Integer;
    X0, Y0, X1, Y1: Double;
    Story: pd_block_id;      { a text box's: the story of its text; 0 for a shape without }
  end;
  TParadeShapeBoxes = array of TParadeShapeBox;
  TIntegerArray = array of Integer;
  { a point of a drawing (its units) }
  TParadeDrawPoint = record
    X, Y: Double;
  end;
  TParadeDrawPoints = array of TParadeDrawPoint;

  { a shape as Word places it: its box before it is turned, the turn and the flips, its geometry }
  TParadeShapeGeom = record
    Sid: Integer;
    CX, CY: Double;          { its centre in the drawing (drawing units) }
    W, H: Double;            { its box, not turned (drawing units) }
    EX, EY: Double;          { the same in its own units (EMU, its a:ext) }
    Rot: Double;             { the turn, degrees clockwise }
    FlipH, FlipV: Boolean;
    Prst: string;            { its preset geometry; 'cust' a custom one; '' neither }
    AdjN: array of string;   { the preset's adjustments, and their values }
    AdjV: array of Double;
    HX, HY: array of Double; { its handles (drawing units in its box, not turned) }
    HIdx: array of Integer;  { and which of the preset's handles each is }
    KX, KY: Double;          { drawing units an EMU of its own }
    Pic, TextBox: Boolean;
  end;

  { where a drawing is: its page, its top left there (sp), sp a unit of its own }
  TParadeDrawMap = record
    Pg: Int32;
    X, Y, KX, KY: Double;
  end;

  { a shape edit's place in the undo history: the selection to go back to when it is undone (or redone) }
  TParadeShapeStep = record
    Lbl: string;                { the step's label, as the history has it }
    On: Boolean;                { a drawing selected }
    At: pd_pos;
    Sid: Integer;
    More: array of Integer;     { the others selected with it }
    Valid: Boolean;             { a redo: the step was a shape edit }
  end;

  TBalloonHit = record
    R: TRect;
    Range: pd_range;
    Kind: Int32;                { PD_MARK_* }
    Id: UInt32;                 { revision or comment }
  end;

  { TParadeEdit }

  TParadeEdit = class(TCustomControl)
  private
    FDoc: Ppd_doc;
    FLayout: Ppd_layout;
    FFonts: array of TParadeFontEntry;
    FMathFont: Ppd_font;        { equations are typeset with it }
    FFallback: array of Ppd_font;   { for characters a run's font lacks: symbols, CJK }
    FFallbackDone: Boolean;
    FCaret, FAnchor: pd_marker_id;
    FDesiredX: Double;
    FHasDesiredX: Boolean;
    FZoom: Double;
    FScrollY: Integer;
    FScrollBar: TScrollBar;
    FScrollHost: TPanel;           { the scroll bar's own window, for its own cursor }
    FShapeOn: Boolean;             { a drawing selected, or a shape of it }
    FShapeAt: pd_pos;              { the drawing: its object in the text }
    FShapeSid: Integer;            { the shape (its sid), -1 the whole drawing }
    FShapeMore: array of Integer;  { more shapes of the drawing, added with Shift+click }
    FDrawKind: string;             { a shape to draw by dragging in the selected canvas; '' none }
    FDrawCursor: TCursor;          { the cursor before drawing began }
    FShapeDrag: Integer;           { a drag of the shape: -1 none, 0..7 a handle (corners and sides), 8 the shape,
                                     9 a shape drawn, 10 a turn, 11 an adjustment, 12 a point, 13 a rubber band,
                                     14 the whole drawing moved, 15 a connector's end, 16 the canvas page's corner }
    FShapeFrom: TPoint;            { where it began (client pixels) }
    FBandTo: TPoint;               { a rubber band: its other corner }
    FGuideX, FGuideY: array of Double;   { a drag snapped to these: lines shown across the canvas (its units) }
    FSnapShapes: Boolean;          { a drag snaps to the other shapes' edges and middles, and the canvas's }
    FCanvasPage: Boolean;          { the document is a page to draw on: its page a canvas, not an object to select }
    FCornerPage: Integer;          { the page whose corner is dragged }
    FSnapGrid: Double;             { and to a grid this many points apart (0: none) }
    FShapeOld, FShapeNew: array[0..3] of Double;   { its box before, and as the drag has it (drawing units) }
    FFrameKey: string;             { the text box the caret is in, as last looked up: story and revision }
    FFrameAt: pd_pos;              { its drawing (block 0: the caret is in none) }
    FFrameSid: Integer;            { and its shape }
    FFrameCursor: Boolean;         { the cursor is the move cursor, over that frame's edge }
    FFrameOldCursor: TCursor;
    FNodeOn: Boolean;              { the selected shape's points shown, to be dragged (Edit Points) }
    FDragIdx: Integer;             { a drag of an adjustment or a point: which }
    FDragGeom: TParadeShapeGeom;   { the shape as the drag found it }
    FDragMap: TParadeDrawMap;
    FRotFrom, FRotNew: Double;     { a turn: the press's angle from the centre (radians), the turn as dragged (degrees) }
    FAdjNew: string;               { the adjustments as a handle's drag leaves them ("adj1=... adj2=...") }
    FNodeX, FNodeY: Double;        { a point as dragged (its path's units) }
    FDragPW, FDragPH: Double;      { and that path's units: its width and height }
    FGeomKey: string;              { ShapeGeom's last answer, and for what }
    FGeomLast: TParadeShapeGeom;
    FGeomOk: Boolean;
    FShapeFillDef, FShapeLineDef: TColor;   { what a new shape is filled and outlined with }
    FDrawPts: array of Double;     { a freeform, a curve or a scribble being drawn: its points (drawing units) }
    FPolyMouse: TPoint;            { where the mouse is, for the line to it from the last point }
    FSelUndo, FSelRedo: array of TParadeShapeStep;   { the selections shape edits were made from and left }
    FStepDepth: Integer;           { inside a shape edit: the edits it is made of are not steps of their own }
    FBlink: TTimer;
    FCaretOn: Boolean;
    FGlyphs: array of PGlyphBmp;   { open-addressing hash table by font, glyph, size and subpixel position }
    FGlyphCount: Integer;
    FPics: array of TLazIntfImage; { by resource id - 1: the picture, decoded once }
    FPicSized: array of TLazIntfImage; { ... and at the size it was last drawn }
    FPicDoc: Ppd_doc;              { the document they are of }
    FPrefetch: TTimer;             { decoding the pictures while the reader is not doing anything }
    FPrefetchRes: pd_res_id;
    FOrder: array of Int32;        { block id -> reading-order index, -1 = not in the main flow }
    FOrderRev: UInt64;
    FDragging: Boolean;
    FFileName: string;
    FOnChange: TNotifyEvent;
    FOnSelectionChange: TNotifyEvent;
    FSelSig: string;               { caret, anchor and document revision when the toolbar was last told }
    FSelQueued: Boolean;
    FPending: pd_char_props;       { formatting chosen with nothing selected: for what is typed next, here }
    FPendingAt: pd_pos;
    FShowMarks: Boolean;
    FCursorShown: Boolean;
    FPainter, FPainterSticky: Boolean;   { the format painter is on: the next selection takes FPainterProps }
    FPainterProps: pd_char_props;         { the I-beam put on the window once it exists (see MouseEnter) }
    FModified: Boolean;
    FPageGap: Integer;
    FBack: TBitmap;                { the pages as last drawn, on the display's side: a paint copies from it }
    FBackImg: TLazIntfImage;       { ... and the same pixels here, to find what a redraw changes }
    FBackSig: string;              { what they were drawn for: size, zoom, layout, selection -- not the scroll }
    FBackScroll: Integer;          { ... and the scroll position they were drawn at }
    FLayoutEpoch: Integer;         { counts layout updates }
    FAuthor: string;               { who tracked changes and comments are by }
    FTrack: Boolean;
    FHasMarkup: Boolean;           { some page has changes or comments: the balloon column shows }
    FBalloons: array of TBalloonHit;
    FOnUndo: TParadeUndoEvent;
    FOnReplacing: TNotifyEvent;
    FHybridDefault: Boolean;
    FReadOnly: Boolean;
    FRemote: array of TParadeRemoteCaret;
    FRemoteRev: Integer;
    FControlMenu: TPopupMenu;      { a drop-down control's choices }
    FMenuAt: pd_pos;
    procedure UseFallbackFonts;
    function DecodePicture(Res: pd_res_id): Boolean;
    procedure PrefetchTick(Sender: TObject);
    procedure ControlMenuClick(Sender: TObject);
    function PastEnds(const P: pd_pos): pd_pos;
    function GetPageCount: Integer;
    function MarkupWidth: Integer;
    function HiddenAt(const P: pd_pos): Boolean;
    function RevisionAt(const P: pd_pos; out Rev: pd_revision): Boolean;
    function StripRevision(Fmt: pd_format_id): pd_format_id;
    procedure SetTrackChanges(AValue: Boolean);
    procedure SetMarkupMode(AValue: Integer);
    function GetMarkupMode: Integer;
    procedure SetHybridBreaking(AValue: Boolean);
    function GetHybridBreaking: Boolean;
    procedure SelectRange(const R: pd_range);
    function ChangeAt(const P: pd_pos; out R: pd_range): Boolean;
    function BalloonHeight(const Segs: array of string; W: Integer; PxScale: Double; Draw: TLazIntfImage;
      X, Y: Integer; Col: UInt32): Integer;
    procedure PaintMarkup(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double);
    procedure ResolveChange(Accept: Boolean);
    procedure SetAuthor(const AValue: string);
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
    procedure CheckSelection;
    procedure SelectionNotify(Data: PtrInt);
    function PendingHere: Boolean;
    function SelectedParagraphs: TParadeBlockArray;
    function ListFormatOf(Block: pd_block_id): Integer;
    procedure InsertObject(const Obj: pd_inline);
    function TextWidthAt(Block: pd_block_id): pd_sp;
    function BlockSlot(out AParent: pd_block_id; out AIndex: Integer): Boolean;
    function CellSpan(Cell: pd_block_id): Integer;
    function GridColumn(Row: pd_block_id; Index: Integer): Integer;
    function CellAtColumn(Row: pd_block_id; Col: Integer): Integer;
    procedure CaretToCell(Cell: pd_block_id);
    procedure ResizeColumns(Table: pd_block_id; At, Count: Integer);
    function ChildCount(Block: pd_block_id): Integer;
    procedure MoveCellContent(From, Into: pd_block_id);
    function SelectedCells: TParadeBlockArray;
    function PlainText(Block: pd_block_id): string;
    function StyleNameOf(Block: pd_block_id): string;
    function TocStyle(Level: Integer; Room: pd_sp): pd_style_id;
    function BuildToc(Container: pd_block_id; Index, MaxLevel: Integer): Integer;
    procedure SetShowMarks(AValue: Boolean);
    procedure PaintMarks(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double);
    function FormatAt(const P: pd_pos): pd_format_id;
    { OnScreen: the editor's view, with what only it shows (the frame of the control the caret is in) }
    procedure PaintPage(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double; DrawCaret: Boolean;
      OnScreen: Boolean = False);
    procedure UseDocumentFonts;
    function BackSignature: string;
    function ScrollBack(DY: Integer): Boolean;
    function DrawingPlace(const P: pd_pos; out Page: Int32; out X, Y, W, H: Double; out JW, JH: Double): Boolean;
    function DrawingAt(Page: Integer; SX, SY: pd_sp; out P: pd_pos): Boolean;
    function PickShape(const P: pd_pos; Page: Integer; SX, SY: pd_sp): Integer;
    function ClickPage(Page: Integer; SX, SY: pd_sp; Shift: TShiftState): Boolean;
    procedure PaintShapeSelection;
    { the drawing whose text box holds a story, and that text box's shape }
    function StoryDrawing(Story: pd_block_id; out D: pd_pos): Boolean;
    function StoryTopOf(B: pd_block_id): pd_block_id;
    function CaretTextBox(out D: pd_pos; out Sid: Integer): Boolean;
    { a text box's edge under a point of the page (sp), in the drawing at D: its shape, or -1 }
    function TextBoxEdgeAt(const D: pd_pos; SX, SY: pd_sp): Integer;
    procedure PaintTextBoxFrame;
    function ReplaceDrawing(const P: pd_pos; const Json: string; const Lbl: string): Boolean;
    function ReplaceDrawingRes(const P: pd_pos; R: pd_res_id; const Lbl: string): Boolean;
    procedure PushShapeStep(const Lbl: string; const At: pd_pos);
    function PutPageCanvas(Sec: pd_block_id; Res: pd_res_id): Boolean;
    { a drawing's object selected as text (delete, cut and copy take it) -- the canvas page's only as the caret }
    procedure SelectObject(const P: pd_pos);
    { the canvas page's canvas: its object's place }
    { the canvas page's canvas selected, to draw in, when nothing else is (and the caret is not in a text box) }
    procedure EnsureCanvas;
    { the canvas page's text is not to be typed in: the caret out of any text box }
    function TextLocked: Boolean;
    function DetectCanvasPage: Boolean;
    { the canvas page's corner, where a drag makes it (and its canvas) bigger or smaller: near (X, Y) }
    function OnPageCorner(X, Y: Integer): Boolean;
    procedure PaintPageCorner;
    { the selected shapes as the clipboard has them (JSON: their XML, their text boxes' stories); Whole: each
      shape with the group it is in }
    function ShapesPayload(out S: string): Boolean;
    function PutShapesPayload(const S: string; const Lbl: string): Boolean;
    function RemoveShapes(Whole: Boolean; const Lbl: string): Boolean;
    function StoryData(St: pd_block_id): string;
    procedure FillStory(St: pd_block_id; const Data: string);
    { the stories of the text boxes of the drawings between A and B }
    function StoriesInRange(const A, B: pd_pos): TParadeBlockArray;
    { stories taken out of the document (a text box's, its drawing gone), those in Keep left }
    procedure DropStories(const S, Keep: TParadeBlockArray);
    { text deleted, with the text boxes' stories of the drawings in it (not when changes are tracked: they stay) }
    function DeleteText(const R: pd_range; After: Ppd_pos): pd_status;
    procedure RestoreShapeStep(const S: TParadeShapeStep);
    procedure OnlyShape;
    function ApplyKeptXml(const Xml: string; const Lbl: string; NewSid: Integer): Boolean;
    function ShapeClientRect(const B: array of Double; out R: TRect): Boolean;
    function ShapeDragStart(X, Y: Integer): Boolean;
    procedure ShapeDragMove(X, Y: Integer; Shift: TShiftState);
    procedure SnapDrag(JW, JH, Thr: Double);
    function NudgeShape(DX, DY: Double): Boolean;
    { over the selected drawing where a press moves it: a picture anywhere, a canvas on its edge }
    function OnDrawingBody(X, Y: Integer): Boolean;
    { the selected shapes moved by DX, DY (the drawing's units): one step of undo }
    function MoveShapes(DX, DY: Double): Boolean;
    { the selected shapes given boxes (the drawing's units), in the order SelectedShapes has them: one step }
    function PlaceShapes(const Lbl: string; const Sids: TIntegerArray; const Boxes: array of Double): Boolean;
    function ShapeGeom(Sid: Integer; out G: TParadeShapeGeom): Boolean;
    function DrawMap(out M: TParadeDrawMap): Boolean;
    function MapToClient(const M: TParadeDrawMap; DX, DY: Double): TPoint;
    function GeomToClient(const M: TParadeDrawMap; const G: TParadeShapeGeom; U, V, Rot: Double): TPoint;
    procedure ClientToGeom(const M: TParadeDrawMap; const G: TParadeShapeGeom; PX, PY: Integer; out U, V: Double);
    function RotHandle(const M: TParadeDrawMap; const G: TParadeShapeGeom; Rot: Double; out ATop, P: TPoint): Boolean;
    function ShapeHandleAt(X, Y: Integer; out Idx: Integer): Integer;
    procedure PaintShapeExtras;
    procedure HoverCursor(C: TCursor);
    function EnterTextBox(Sid: Integer; const At: pd_pos; UseAt, AtEnd: Boolean): Boolean;
    function MoveShapePoint(N: Integer; NX, NY: Double): Boolean;
    function PutShapeXml(const Sh: string; const Lbl: string = 'Insert shape'): Boolean;
    function ClientToDrawing(X, Y: Integer; out DX, DY: Double): Boolean;
    procedure FinishDrawPath(Closed: Boolean);
    procedure PaintDrawPath;
    procedure PaintSites;
    { the site of a shape of the selected canvas nearest a point of the control, within a few pixels (not of the
      shape Skip): its shape, its index, where it is (the drawing's units) }
    function NearestSite(X, Y, Skip: Integer; out Sid, Site: Integer; out DX, DY: Double): Boolean;
    function SidId(const Xml: string; Sid: Integer): string;
    function ShapeHandleXY(N: Integer; out PX, PY: Double): Boolean;
    procedure DrawView(Img: TLazIntfImage; ATop, AHeight: Integer; PagesToo: Boolean);
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
    procedure TripleClick; override;
    procedure MouseEnter; override;
    procedure DoEnter; override;
    procedure DoExit; override;
  public
    constructor Create(AOwner: TComponent); override;
    destructor Destroy; override;

    { fonts: the resolver picks the closest registered face for a family }
    procedure AddFont(const Family, FileName: string; Weight: Integer = 400; Italic: Boolean = False);
    procedure AddDefaultFonts;
    { a face registered from its file and loaded only when text first uses it }
    procedure AddFontFile(const Family, FileName: string; Weight: Integer = 400; Italic: Boolean = False;
      FaceIndex: Integer = 0);
    { every font installed on the system (paradefonts), registered to be loaded when used -- for a font list;
      a family registered already keeps its own faces. How many faces were added. }
    function AddSystemFonts: Integer;
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
    { the caret in a drawing's text box: back to the text, just after the drawing (Escape). False when the
      caret is not in one }
    function LeaveDrawingText: Boolean;
    { the caret in a drawing's text box: that text box selected, as a shape to move, size or delete (Escape, as
      Word has it). False when the caret is not in one }
    function SelectTextBox: Boolean;
    { where a shape of the drawing at D is on its page (sp); Sid -1 the whole drawing }
    function ShapePageBox(const D: pd_pos; Sid: Integer; out Page: Int32; out X0, Y0, X1, Y1: Double): Boolean;
    { the drawing selected, or a shape of it: True with its object's place and the shape (-1: the whole drawing) }
    function SelectedShape(out At: pd_pos; out Sid: Integer): Boolean;
    procedure ClearShapeSelection;
    { the shapes of the drawing whose object is at P, from its description (empty when it is no drawing) }
    function DrawingShapes(const P: pd_pos): TParadeShapeBoxes;
    { the selected drawing's shape Sid: its box in the drawing's units (sp) }
    function ShapeBox(Sid: Integer; out X0, Y0, X1, Y1: Double): Boolean;
    { the selected drawing's shape Sid moved and sized to a box (the drawing's units): its pieces, and the shape
      in the XML kept for Word; one step of undo }
    function SetShapeBox(Sid: Integer; X0, Y0, X1, Y1: Double): Boolean;
    { the selected shape taken out of its drawing; one step of undo }
    function DeleteShape: Boolean;
    { the selected shape filled with a colour (None: not filled), outlined (None: no line; Width in points, 0:
      as it is; the colour clNone: as it is), moved in the order (0 forward, 1 backward, 2 to the front, 3 to the back); the selected shapes
      (the shape and those added with Shift+click) made a group; the group the selected shape is in undone.
      Each one step of undo, the drawing made again from its XML as Word will have it }
    function SetShapeFill(AColor: TColor; None: Boolean): Boolean;
    function SetShapeLine(AColor: TColor; WidthPt: Double; None: Boolean): Boolean;
    function ShapeOrder(Mode: Integer): Boolean;
    { the selected shapes lined up (Mode: 0 left, 1 centre, 2 right, 3 top, 4 middle, 5 bottom) along the box
      round them all -- one shape alone, along its canvas; spread out evenly (Horizontal: across, else down),
      three or more, the outermost where they are. One step of undo each }
    function AlignShapes(Mode: Integer): Boolean;
    function DistributeShapes(Horizontal: Boolean): Boolean;
    { the selected shapes' outline dashed (Dash: solid, dash, sysDash, sysDot, dashDot, lgDash, lgDashDot,
      lgDashDotDot, sysDashDot, sysDashDotDot) and given arrowheads at its start (Head) and end (Tail): none,
      triangle, stealth, diamond, oval, arrow; '' leaves one as it is. One step of undo }
    function SetShapeLineStyle(const Dash, Head, Tail: string): Boolean;
    { the selected shape's outline: its dash and its arrowheads, as Word has them ('' none given) }
    function ShapeLineStyle(out Dash, Head, Tail: string): Boolean;
    function GroupShapes: Boolean;
    function UngroupShape: Boolean;
    { the selected shapes (each with its group, when it is in one) on the clipboard, with their text boxes' text;
      cut: taken out too; pasted into the selected canvas (or a new one at the caret), a little apart from the
      ones they were copied from when those are still there; duplicated without the clipboard (Ctrl+D). Each one
      step of undo, the pasted shapes selected }
    function CopyShapes: Boolean;
    function CutShapes: Boolean;
    function PasteShapes: Boolean;
    function DuplicateShapes: Boolean;
    { a picture (a resource of the document, W by H sp) put into the selected canvas, in its middle and no bigger
      than it; selected, one step of undo (InsertPicture does this with a canvas selected) }
    function AddPictureShape(Res: pd_res_id; W, H: pd_sp): Boolean;
    { the selected shapes taken out of their drawing at once: one step of undo }
    function DeleteShapes: Boolean;
    { whether the clipboard has shapes copied from a drawing }
    function CanPasteShapes: Boolean;
    { the XML kept for Word of the selected drawing; False when it keeps none (made here, or not from a .docx) }
    function KeptXml(out Xml: string): Boolean;
    { a new canvas (a drawing to draw shapes in), WPt by HPt points (0: as wide as the text, three inches high),
      put in at the caret and selected }
    function InsertCanvas(WPt: Double = 0; HPt: Double = 0): Boolean;
    { a shape of a kind -- rect, roundRect, ellipse, triangle, diamond, pentagon, hexagon, rightArrow, leftArrow,
      upArrow, downArrow, star5, line, arrow, textbox -- added to the selected canvas: drawn with the mouse when one
      is selected (the next drag in it), else a new canvas with the shape in its middle }
    function InsertShape(const Kind: string): Boolean;
    { the shape put into the selected canvas at a box of the drawing's units (the line's ends at its corners,
      FlipH/FlipV: from the right, from the bottom); selected }
    function AddShape(const Kind: string; X0, Y0, X1, Y1: Double; FlipH: Boolean = False;
      FlipV: Boolean = False): Boolean;
    { a connector (line, arrow, elbow..., curved...: the kinds InsertShape draws) from (X1, Y1) to (X2, Y2) in the
      selected canvas (its units), its start joined to site StartSite of shape StartSid and its end to EndSite of
      EndSid (-1: not joined): joined, it stays on them when they move. Selected; one step of undo }
    function AddConnector(const Kind: string; X1, Y1, X2, Y2: Double; StartSid: Integer = -1; StartSite: Integer = -1;
      EndSid: Integer = -1; EndSite: Integer = -1): Boolean;
    { where connectors can be joined to a shape of the selected canvas (its units), as its sites are numbered }
    function ShapeSites(Sid: Integer): TParadeDrawPoints;
    { the selected connector's start (End_ False) or end moved to (X, Y), joined there to site Site of shape Sid
      (-1: not joined); one step of undo }
    function MoveConnectorEnd(End_: Boolean; X, Y: Double; Sid: Integer = -1; Site: Integer = -1): Boolean;
    { the selected connector's ends (the canvas's units), and what each is joined to (sids; -1 none) }
    function ConnectorInfo(out X1, Y1, X2, Y2: Double; out StartSid, EndSid: Integer): Boolean;
    { the selected shape turned to Deg degrees (clockwise), flipped (Horizontal, else vertically): one step of undo }
    function RotateShape(Deg: Double): Boolean;
    function FlipShape(Horizontal: Boolean): Boolean;
    { the selected shape as Word places it: its box before it is turned, the turn, its preset and adjustments }
    function SelectedShapeGeom(out G: TParadeShapeGeom): Boolean;
    { a preset's adjustment (adj, adj1, ...: a corner's radius, an arrow's head) set, in Word's units: one step of undo }
    function SetShapeAdjust(const AName: string; Value: Double): Boolean;
    { the adjustments a string names ("adj1=5000 adj2=200") set at once: one step of undo }
    function SetShapeAdjusts(const Adj: string): Boolean;
    { the selected shape's points shown to be dragged (Word's Edit Points): a preset made a custom geometry first.
      False for a picture, a text box, or a geometry of guides }
    function EditShapePoints: Boolean;
    property EditingPoints: Boolean read FNodeOn;
    { the selected shape given text to type (Word's Add Text: in its middle, white on a dark fill), the caret put
      in it; a shape with text already: the caret in that. False for a line, a picture; one step of undo }
    function AddShapeText: Boolean;
    { a handle of the selected shape in the control's pixels: Kind 's' a sizing one (0..7), 'r' the turning one,
      'a' an adjustment's, 'n' a point's (Edit Points) }
    function ShapeHandlePoint(Kind: Char; Index: Integer; out P: TPoint): Boolean;
    { a shape dragged (moved, sized, drawn) snaps to the other shapes' edges and middles and the canvas's, a guide
      shown (Alt held: not); and to a grid SnapGrid points apart (0: none) }
    property SnapToShapes: Boolean read FSnapShapes write FSnapShapes;
    property SnapGrid: Double read FSnapGrid write FSnapGrid;
    { what a new shape is filled and outlined with }
    property ShapeFillColor: TColor read FShapeFillDef write FShapeFillDef;
    property ShapeLineColor: TColor read FShapeLineDef write FShapeLineDef;
    { the selected shape filled and outlined at once: one step of undo }
    function SetShapeStyle(AFill, ALine: TColor): Boolean;
    { a freeform put into the selected canvas through points of the drawing's units (sp, pairs): straight lines or
      (Curve) a smooth curve through them; filled when Closed. Selected; one step of undo }
    function AddPathShape(const Pts: array of Double; Curve, Closed: Boolean): Boolean;
    { a point of a page (points from its top left) in the control's pixels, as the view is now }
    function PageToClient(Page: Integer; XPt, YPt: Double): TPoint;
    { the shape kind waiting for a drag in the canvas ('' none) }
    property DrawKind: string read FDrawKind;
    { the selected object (a picture or a drawing) made W by H (sp) -- a canvas given that much room, its shapes
      not stretched; one step of undo }
    function ResizeObject(W, H: pd_sp): Boolean;
    { the document made a page to draw on: landscape, no margins, the page a canvas (a drawing canvas in front of
      the text, as big as the page), ready to draw in; what is not to be undone, and not a change }
    function StartCanvasPage: Boolean;
    { the document is such a page: its page a canvas, the canvas not selected as an object, the page's corner
      dragged to size both }
    property CanvasPage: Boolean read FCanvasPage;
    { the canvas page's canvas: its object's place (the page the caret is on, of several) }
    function CanvasPagePos(out P: pd_pos): Boolean;
    { the object at P is a canvas page's canvas }
    function IsPageCanvas(const P: pd_pos): Boolean;

    { the canvas page (and its canvas) made W by H (sp); one step of undo }
    function ResizeCanvasPage(W, H: pd_sp): Boolean;
    { slides (a presentation: every page a canvas page): how many; the one being edited (-1 none); one shown and
      ready to draw in; a new one after another (-1: first), blank or (Copy) a copy of that one; one deleted (not
      the last left); one moved to another place. Each one step of undo }
    function SlideCount: Integer;
    function CurrentSlide: Integer;
    function GoToSlide(Index: Integer): Boolean;
    { a slide's page at the top of the view, the selection as it is }
    procedure ShowSlide(Index: Integer);
    function NewSlide(After: Integer; Copy: Boolean = False): Boolean;
    function DeleteSlide(Index: Integer): Boolean;
    function MoveSlide(Index, ToIndex: Integer): Boolean;
    { the selected drawing is a canvas (made here, or read from Word) shapes can go into }
    function CanvasSelected: Boolean;
    { the selected object, in the text, moved to P (as text is dragged); one step of undo }
    function MoveObjectTo(const P: pd_pos): Boolean;
    { the selected object, floating, moved by DX, DY on its page (sp): anchored in the paragraph it is moved by,
      that far down it and across the column; one step of undo }
    function MoveFloatBy(DX, DY: pd_sp): Boolean;
    { the float the selected object is in (0: it is in the text) }
    function SelectedFloat: pd_block_id;
    { how the text goes round the selected object (a picture or a drawing): -1 in line with the text, else PD_WRAP_*
      (NONE: above and below it; LEFT, RIGHT: beside it, it on that side; FRONT, BEHIND: over or under the text,
      which takes no notice); -2 when none is selected }
    function ObjectWrap: Integer;
    { the selected object put in line with the text (-1) or floating with the text round it so; where it is kept;
      one step of undo }
    function SetObjectWrap(Wrap: Integer): Boolean;
    { the shapes selected: the one, and those added (Shift+click), as sids }
    function SelectedShapes: TIntegerArray;
    { a shape of the selected drawing added to the selection, or taken out if it is in it (Shift+click) }
    function ToggleShape(Sid: Integer): Boolean;
    { the shapes of the selected drawing wholly inside a box of its units selected (a rubber band); Add: to those
      selected already. How many there are }
    function SelectShapesIn(X0, Y0, X1, Y1: Double; Add: Boolean = False): Integer;
    procedure SelectAll;
    procedure Undo;
    procedure Redo;
    procedure ToggleBold;
    procedure ToggleItalic;
    procedure ToggleUnderline;
    procedure SetParagraphStyle(const StyleName: string);

    { ---- character formatting: the selection, or with none what is typed next at the caret ---- }
    { the masked fields of Props over the selection }
    procedure ApplyCharProps(const Props: pd_char_props);
    { a paragraph's text (UTF-8, U+FFFC for each inline object) }
    function ParaText(Block: pd_block_id): string;
    { the formatting of the character at a position }
    function PropsAt(const P: pd_pos): pd_char_props;
    { the format painter: the formatting at the caret copied, for the next selection made with the mouse
      (Sticky: every selection until stopped -- Escape, or StopFormatPainter) }
    procedure StartFormatPainter(Sticky: Boolean = False);
    procedure StopFormatPainter;
    { the copied formatting put on the selection now; the painter off unless sticky }
    procedure ApplyFormatPainter;
    function FormatPainterOn: Boolean;
    { the formatting where the selection starts (with none: what typing at the caret gets) }
    function CurrentCharProps: pd_char_props;
    procedure SetFontFamily(const Family: string);
    procedure SetFontSize(Points: Double);
    { the next size up or down the usual list (8, 9, 10, 11, 12, 14, 16, 18, 20, 24, ...) }
    procedure StepFontSize(Up: Boolean);
    procedure ToggleStrike;
    procedure ToggleSuperscript;
    procedure ToggleSubscript;
    { $RRGGBB; -1: the style's colour }
    procedure SetTextColor(RGB: Integer);
    { $RRGGBB; -1: none }
    procedure SetHighlight(RGB: Integer);
    { direct character formatting removed, styles kept }
    procedure ClearFormatting;

    { ---- paragraph formatting: every paragraph the selection touches ---- }
    { the masked fields of Props set on each paragraph, its other direct properties kept }
    procedure ApplyParaProps(const Props: pd_para_props);
    { the paragraph at the caret: its style's properties with its own over them }
    function CurrentParaProps: pd_para_props;
    procedure SetAlignment(AAlign: Integer);    { PD_ALIGN_* }
    { half an inch more or less left indent; in a list, a level deeper or shallower }
    procedure ChangeIndent(Deeper: Boolean);
    procedure SetLineSpacing(PerMille: Integer);   { 1000: single }
    procedure SetParaSpacing(Before, After: Double);    { points; < 0: unchanged }
    { a bulleted (PD_NUM_BULLET) or numbered (PD_NUM_DECIMAL ...) list, or back to plain paragraphs
      when they all are that kind of list already }
    procedure ToggleList(AFormat: Integer);
    { PD_NUM_* of the list the caret's paragraph is in, -1 when none }
    function CurrentListFormat: Integer;
    { the paragraph styles, and the caret's }
    procedure GetParagraphStyles(List: TStrings);
    function CurrentStyleName: string;
    { the font families the editor has (AddFont), sorted }
    procedure GetFontFamilies(List: TStrings);

    { ---- the Insert tab: objects at the caret (over the selection), one undo each ---- }
    { a picture from a file (PNG, JPEG, GIF), at its own size or the text's width when wider; False when it
      cannot be read }
    function InsertPicture(const FileName: string): Boolean;
    { a link to URL around the selection, or Text (the URL when empty) inserted as a link }
    procedure InsertLink(const URL, AText: string);
    { a page (PD_BREAK_PAGE), column (PD_BREAK_COLUMN) break or a horizontal rule (PD_BREAK_RULE) at the caret }
    procedure InsertBreak(Kind: Integer);
    { a Rows x Cols table at the caret (the paragraph split there), the caret in its first cell }
    procedure InsertTable(Rows, Cols: Integer);
    { an equation from its LaTeX source, in the line or (Display) on a line of its own }
    procedure InsertEquation(const Source: string; Display: Boolean);
    { a footnote (or endnote) mark at the caret, the note holding Text }
    procedure InsertNote(const ANote: string; Endnote: Boolean = False);
    { a field: PD_FIELD_PAGE, PD_FIELD_PAGES or PD_FIELD_DATE }
    procedure InsertField(Kind: Integer);
    { a content control at the caret: Kind 'checkbox', 'dropdown' or 'combobox' (with Items), 'date', 'text';
      its prompt selected }
    procedure InsertControl(const Kind: string; const Items: array of string);

    { ---- content controls (a Word form's fields) ---- }
    { the innermost control around P: its kind, its JSON (pd_doc_control_at), where it starts and ends }
    function ControlAt(const P: pd_pos; out Kind, Spec: string; out AStart, AEnd: pd_pos): Boolean;
    { the control from AStart to AEnd now says Spec and holds Content (one undo) }
    procedure SetControl(const AStart, AEnd: pd_pos; const Spec, Content: string);
    { the check box around P ticked or cleared; False when there is none (or it is locked) }
    function ToggleCheckBox(const P: pd_pos): Boolean;
    { a drop-down's (or combo box's) choices, what each shows }
    function ControlItems(const P: pd_pos; Items: TStrings): Boolean;
    procedure ChooseControlItem(const P: pd_pos; Index: Integer);
    function ControlDate(const P: pd_pos; out ADate: TDateTime): Boolean;
    procedure SetControlDate(const P: pd_pos; ADate: TDateTime);
    { what a click on a control does: tick, drop the list down, a calendar; True when it did }
    function ClickControl(const P: pd_pos; X, Y: Integer): Boolean;

    { ---- the Layout tab: the caret's section (its pages) ---- }
    function CurrentSection: pd_block_id;
    function CurrentSectionProps: pd_section_props;
    { the section's page settings replaced by Props (one undo) }
    procedure ApplySectionProps(const Props: pd_section_props);
    procedure SetMargins(ATop, ABottom, ALeft, ARight: Double);    { points }
    procedure SetOrientation(Landscape: Boolean);
    { the paper, in points, turned to the section's orientation }
    procedure SetPageSize(AWidth, AHeight: Double);
    procedure SetColumns(Count: Integer);
    { a new section from the caret on (on a new page, or on the same one when Continuous), with the same
      page settings, so they can differ from here }
    procedure InsertSectionBreak(Continuous: Boolean);
    { the header's (or footer's) text: fields as {page}, {pages}, {date} }
    function HeaderFooterText(Footer: Boolean): string;
    { the header (or footer) of every page of the section: Text with {page}, {pages} and {date} becoming fields,
      aligned AAlign; '' removes it }
    procedure SetHeaderFooter(Footer: Boolean; const AText: string; AAlign: Integer = PD_ALIGN_CENTER);
    { the number the section's first page has (0: on from the section before) }
    procedure SetFirstPageNumber(N: Integer);
    { lines numbered in the margin, every Every lines (0: none) }
    procedure SetLineNumbers(Every: Integer);

    { ---- the Table tab: the table the caret is in ---- }
    { the cell, row and table around a position; False when it is not in a table }
    function CellAt(const P: pd_pos; out Cell, Row, Table: pd_block_id): Boolean;
    function InTable: Boolean;
    procedure TableInsertRow(Below: Boolean);
    procedure TableInsertColumn(Right: Boolean);
    procedure TableDeleteRow;
    procedure TableDeleteColumn;
    procedure TableDelete;
    { the cell joined with the one to its right (or below), their contents together }
    procedure TableMergeRight;
    procedure TableMergeDown;
    { a merged cell back into cells }
    procedure TableSplitCell;
    { $RRGGBB behind the selected cells (-1: none) }
    procedure SetCellShading(RGB: Integer);
    { the grid's rules, in points (0: none) }
    procedure SetTableBorders(Points: Double);
    { the first row repeated at the top of every page the table runs onto }
    procedure SetHeaderRow(Repeated: Boolean);
    { every column as wide as the others, the table as wide as it was }
    procedure DistributeColumns;
    function CurrentTableProps: pd_table_props;
    function CurrentCellProps: pd_cell_props;

    { ---- the References tab ---- }
    { a table of contents of the headings (levels 1 to MaxLevel) at the caret: their text, dot leaders and page
      numbers kept up to date; its paragraphs have the styles TOC Heading and TOC 1.. }
    procedure InsertTableOfContents(MaxLevel: Integer = 3);
    { the table of contents made again from the headings as they are now; False when there is none }
    function UpdateTableOfContents: Boolean;
    { a caption paragraph after the caret's: "Figure 3: ...", numbered on its own for each Seq name }
    procedure InsertCaption(const Seq, AText: string);
    { what a cross-reference can point at: every caption and heading, in document order }
    function ReferenceTargets: TParadeRefTargets;
    { a reference at the caret to a target's label and number ("Figure 2", prfLabel), number (prfNumber),
      page (prfPage) or text (prfText) }
    procedure InsertCrossReference(const Target: TParadeRefTarget; What: TParadeRefWhat);
    { a named place, for links (#name) and other programs' cross-references }
    procedure InsertBookmark(const AName: string);

    { ---- the View tab ---- }
    { the zoom at which a page fills the width of the view, or the whole page fits in it }
    function PageWidthZoom: Double;
    function WholePageZoom: Double;
    { the headings, in order, for a navigation list: Objects are their blocks, the strings indented by level }
    procedure GetHeadings(List: TStrings);
    { the caret to a position, scrolled into view }
    procedure GoToPos(const P: pd_pos);
    { the selection's two ends, in document order; False when there is none (only the caret) }
    function SelectionRange(out A, B: pd_pos): Boolean;
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

    { review: tracked changes and comments }
    procedure AcceptChange;             { the selection's changes, or the one at the caret; then the next }
    procedure RejectChange;
    procedure AcceptAllChanges;
    procedure RejectAllChanges;
    function NextChange(Dir: Integer = 1): Boolean;   { select the next (previous) change or comment }
    { a comment by Author on the selection (the word at the caret without one); 0 if none could be made }
    function AddComment(const AText: string): pd_comment_id;
    function ReplyToComment(Id: pd_comment_id; const AText: string): pd_comment_id;
    function CommentAt(const P: pd_pos): pd_comment_id;   { the innermost comment over a position, 0 if none }
    procedure DeleteComment(Id: pd_comment_id);
    { the document was changed from outside (a collaborator's edit): laid out and drawn again, the
      view left where it is }
    procedure ExternalChange;
    { the others' carets and selections, drawn in their colours with their names }
    procedure SetRemoteCarets(const Carets: array of TParadeRemoteCaret);
    procedure ResolveComment(Id: pd_comment_id; Resolved: Boolean = True);

    property Doc: Ppd_doc read FDoc;
    property Layout: Ppd_layout read FLayout;
    property CaretPos: pd_pos read GetCaretPos;
    property AnchorPos: pd_pos read GetAnchorPos;
    property PageCount: Integer read GetPageCount;
    property Modified: Boolean read FModified write FModified;
    property FileName: string read FFileName;
    { edits are recorded as tracked changes by Author }
    property TrackChanges: Boolean read FTrack write SetTrackChanges;
    property Author: string read FAuthor write SetAuthor;
    { PD_MARKUP_*: balloons, inline, final or original }
    property MarkupMode: Integer read GetMarkupMode write SetMarkupMode;
    { line breaking as one types: hybrid (lines away from the edit hold still) or optimal (every edited
      paragraph re-broken as if fresh); saved with the document }
    property HybridBreaking: Boolean read GetHybridBreaking write SetHybridBreaking;
    { for tests: the view as last painted (scrolled pixels and drawn strips)
      is what drawing all of it afresh gives, pixels and balloons alike }
    function ViewMatchesRedraw: Boolean;
    { what a new or imported document gets (a .pdoc or .jdoc keeps its own); default on }
    property HybridDefault: Boolean read FHybridDefault write FHybridDefault;
    { Undo/Redo go here when set (a shared document undoes one's own edits only) }
    property OnUndo: TParadeUndoEvent read FOnUndo write FOnUndo;
    { the document is about to be replaced (new, loaded): whatever holds it lets go }
    property OnReplacing: TNotifyEvent read FOnReplacing write FOnReplacing;
    { shown, selected and copied, not changed (a viewer of a shared document) }
    property ReadOnly: Boolean read FReadOnly write FReadOnly;
  published
    property Align;
    property Anchors;
    property Zoom: Double read FZoom write SetZoom;
    property OnChange: TNotifyEvent read FOnChange write FOnChange;
    { the caret moved, the selection changed or the document did: for a toolbar showing what is here }
    property OnSelectionChange: TNotifyEvent read FOnSelectionChange write FOnSelectionChange;
    { the control's size changed: for a zoom that follows the width }
    property OnResize;
    { formatting marks: a pilcrow at each paragraph's end }
    property ShowMarks: Boolean read FShowMarks write SetShowMarks;
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
{ a picture over the page's pixels at X, Y; only within the clip (a cropped picture's frame) when it has one }
procedure BlendPicture(Img, Pic: TLazIntfImage; X, Y: Integer; CL: Integer = Low(Integer); CT: Integer = Low(Integer);
  CR: Integer = High(Integer); CB: Integer = High(Integer));
var
  PX, PY, A, X0, X1: Integer;
  Src, Dst: PPixel;
begin
  X0 := 0;
  if X < 0 then X0 := -X;
  if X + X0 < CL then X0 := CL - X;
  X1 := Pic.Width;
  if X + X1 > Img.Width then X1 := Img.Width - X;
  if X + X1 > CR then X1 := CR - X;
  if X1 <= X0 then
    Exit;
  for PY := 0 to Pic.Height - 1 do
  begin
    if (Y + PY < 0) or (Y + PY >= Img.Height) or (Y + PY < CT) or (Y + PY >= CB) then
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

{ how opaque a colour 0xAARRGGBB is to draw: its alpha, 0 (a colour given without one) as opaque }
function RuleAlpha(Col: UInt32): Integer;
begin
  Result := Col shr 24;
  if Result = 0 then
    Result := 255;
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

type
  TPtD = record
    X, Y: Double;
  end;
  TPtDArray = array of TPtD;
  TRings = array of TPtDArray;

{ a polygon filled with a colour, non-zero winding, edges smoothed: four rows
  of samples to a pixel, and each crossing counted to its fraction of a pixel }
{ rings filled (non-zero winding), antialiased; a gradient (GKind 1 linear at GAng degrees clockwise from right, 2
  radial out from the middle) from Col to Col2 across their box }
procedure FillRingsImg(Img: TLazIntfImage; const Rings: TRings; Col: UInt32; Col2: UInt32 = 0; GKind: Integer = 0;
  GAng: Double = 0);
const
  SUB = 4;
var
  GCX, GCY, GUX, GUY, GLo, GHi, GR, GT, BX0, BY0, BX1, BY1: Double;
  CR, CG, CB: Integer;
  Op: Integer;
  N, I, J, K, PY, S, X0, X1, Cnt, Wind, PX, RI, Total: Integer;
  P: TPtDArray;
  MinY, MaxY, MinX, MaxX, Sy, Ax, Ay, Bx, By, Xa, Xb: Double;
  Xs: array of Double;
  Ds: array of Integer;
  Cov: array of Single;
  Pix: PPixel;
  R, G, B, A: Integer;
  T: Double;
  Td: Integer;
begin
  Total := 0;
  MinY := 1e30; MaxY := -1e30; MinX := 1e30; MaxX := -1e30;
  for RI := 0 to High(Rings) do
    for I := 0 to High(Rings[RI]) do
    begin
      Inc(Total);
      if Rings[RI][I].Y < MinY then MinY := Rings[RI][I].Y;
      if Rings[RI][I].Y > MaxY then MaxY := Rings[RI][I].Y;
      if Rings[RI][I].X < MinX then MinX := Rings[RI][I].X;
      if Rings[RI][I].X > MaxX then MaxX := Rings[RI][I].X;
    end;
  if Total < 3 then
    Exit;
  BX0 := MinX; BY0 := MinY; BX1 := MaxX; BY1 := MaxY;     { the gradient's box: the rings', not the part in view }
  GCX := (BX0 + BX1) / 2;
  GCY := (BY0 + BY1) / 2;
  GUX := Cos(GAng * Pi / 180);
  GUY := Sin(GAng * Pi / 180);
  GLo := Min(Min((BX0 - GCX) * GUX + (BY0 - GCY) * GUY, (BX1 - GCX) * GUX + (BY0 - GCY) * GUY),
    Min((BX0 - GCX) * GUX + (BY1 - GCY) * GUY, (BX1 - GCX) * GUX + (BY1 - GCY) * GUY));
  GHi := -GLo;
  GR := Max(1, Sqrt(Sqr(BX1 - BX0) + Sqr(BY1 - BY0)) / 2);
  if MinY < 0 then MinY := 0;
  if MaxY > Img.Height then MaxY := Img.Height;
  if MinX < 0 then MinX := 0;
  if MaxX > Img.Width then MaxX := Img.Width;
  if (MaxY <= MinY) or (MaxX <= MinX) then
    Exit;
  X0 := Trunc(MinX);
  X1 := Trunc(MaxX) + 1;
  if X1 > Img.Width then X1 := Img.Width;
  SetLength(Cov, X1 - X0 + 1);
  SetLength(Xs, Total);
  SetLength(Ds, Total);
  R := (Col shr 16) and $FF;
  G := (Col shr 8) and $FF;
  B := Col and $FF;
  { the colour's own alpha (0xAARRGGBB): 1..254 see-through; 0 and 255 opaque, 0 being a colour given without one }
  Op := 255;
  if ((Col shr 24) > 0) and ((Col shr 24) < 255) then
    Op := Col shr 24;
  for PY := Trunc(MinY) to Trunc(MaxY) do
  begin
    if (PY < 0) or (PY >= Img.Height) then
      Continue;
    FillChar(Cov[0], Length(Cov) * SizeOf(Single), 0);
    for S := 0 to SUB - 1 do
    begin
      Sy := PY + (S + 0.5) / SUB;
      Cnt := 0;
      for RI := 0 to High(Rings) do
      begin
        P := Rings[RI];
        N := Length(P);
        for I := 0 to N - 1 do
        begin
          J := (I + 1) mod N;    { every ring closed for filling }
          Ay := P[I].Y; By := P[J].Y;
          if (Ay = By) or ((Sy < Ay) = (Sy < By)) then
            Continue;
          Ax := P[I].X; Bx := P[J].X;
          Xs[Cnt] := Ax + (Sy - Ay) * (Bx - Ax) / (By - Ay);
          if By > Ay then Ds[Cnt] := 1 else Ds[Cnt] := -1;
          Inc(Cnt);
        end;
      end;
      { in order across, a crossing at a time }
      for I := 1 to Cnt - 1 do
      begin
        T := Xs[I];
        Td := Ds[I];
        K := I - 1;
        while (K >= 0) and (Xs[K] > T) do
        begin
          Xs[K + 1] := Xs[K];
          Ds[K + 1] := Ds[K];
          Dec(K);
        end;
        Xs[K + 1] := T;
        Ds[K + 1] := Td;
      end;
      Wind := 0;
      for I := 0 to Cnt - 2 do
      begin
        Inc(Wind, Ds[I]);
        if Wind = 0 then
          Continue;
        Xa := Xs[I] - X0;
        Xb := Xs[I + 1] - X0;
        if Xa < 0 then Xa := 0;
        if Xb > X1 - X0 then Xb := X1 - X0;
        if Xb <= Xa then
          Continue;
        if Trunc(Xa) = Trunc(Xb) then
          Cov[Trunc(Xa)] := Cov[Trunc(Xa)] + (Xb - Xa) / SUB
        else
        begin
          Cov[Trunc(Xa)] := Cov[Trunc(Xa)] + (Trunc(Xa) + 1 - Xa) / SUB;
          for K := Trunc(Xa) + 1 to Trunc(Xb) - 1 do
            Cov[K] := Cov[K] + 1 / SUB;
          if Trunc(Xb) < Length(Cov) then
            Cov[Trunc(Xb)] := Cov[Trunc(Xb)] + (Xb - Trunc(Xb)) / SUB;
        end;
      end;
    end;
    Pix := PPixel(Img.GetDataLineStart(PY));
    Inc(Pix, X0);
    for PX := 0 to X1 - X0 - 1 do
    begin
      A := Round(Cov[PX] * Op);
      if A > Op then A := Op;
      if A > 0 then
      begin
        CR := R; CG := G; CB := B;
        if GKind > 0 then
        begin   { how far along the gradient this pixel is }
          if GKind = 2 then
            GT := 1 - Sqrt(Sqr(X0 + PX + 0.5 - GCX) + Sqr(PY + 0.5 - GCY)) / GR
          else if GHi > GLo then
            GT := ((X0 + PX + 0.5 - GCX) * GUX + (PY + 0.5 - GCY) * GUY - GLo) / (GHi - GLo)
          else
            GT := 0;
          if GT < 0 then GT := 0;
          if GT > 1 then GT := 1;
          if GKind = 2 then
            GT := 1 - GT;     { the middle the first colour, the outside the second }
          CR := Round(R + (Integer((Col2 shr 16) and $FF) - R) * GT);
          CG := Round(G + (Integer((Col2 shr 8) and $FF) - G) * GT);
          CB := Round(B + (Integer(Col2 and $FF) - B) * GT);
        end;
        Pix^.R := (Pix^.R * (255 - A) + CR * A) div 255;
        Pix^.G := (Pix^.G * (255 - A) + CG * A) div 255;
        Pix^.B := (Pix^.B * (255 - A) + CB * A) div 255;
        Pix^.A := 255;
      end;
      Inc(Pix);
    end;
  end;
end;

type
  { a glyph's outline as rings on the image: scaled from font units (26.6, y up), turned, at the pen }
  TGlyphRings = record
    Rings: TRings;
    Sc, CosT, SinT, PX, PY, LX, LY: Double;
  end;
  PGlyphRings = ^TGlyphRings;

procedure GRPoint(G: PGlyphRings; FX, FY: Double);
var
  U, V: Double;
  R, K: Integer;
begin
  U := FX * G^.Sc;
  V := -FY * G^.Sc;
  R := High(G^.Rings);
  if R < 0 then
    Exit;
  K := Length(G^.Rings[R]);
  SetLength(G^.Rings[R], K + 1);
  G^.Rings[R][K].X := G^.PX + U * G^.CosT - V * G^.SinT;
  G^.Rings[R][K].Y := G^.PY + U * G^.SinT + V * G^.CosT;
  G^.LX := FX;
  G^.LY := FY;
end;

procedure GRMove(user: Pointer; x, y: Int32); cdecl;
var
  G: PGlyphRings;
begin
  G := PGlyphRings(user);
  SetLength(G^.Rings, Length(G^.Rings) + 1);
  GRPoint(G, x, y);
end;

procedure GRLine(user: Pointer; x, y: Int32); cdecl;
begin
  GRPoint(PGlyphRings(user), x, y);
end;

procedure GRQuad(user: Pointer; cx, cy, x, y: Int32); cdecl;
var
  G: PGlyphRings;
  X0, Y0, T: Double;
  I: Integer;
begin
  G := PGlyphRings(user);
  X0 := G^.LX;
  Y0 := G^.LY;
  for I := 1 to 8 do
  begin
    T := I / 8;
    GRPoint(G, (1 - T) * (1 - T) * X0 + 2 * (1 - T) * T * cx + T * T * x,
      (1 - T) * (1 - T) * Y0 + 2 * (1 - T) * T * cy + T * T * y);
  end;
end;

procedure GRCubic(user: Pointer; c1x, c1y, c2x, c2y, x, y: Int32); cdecl;
var
  G: PGlyphRings;
  X0, Y0, T, U: Double;
  I: Integer;
begin
  G := PGlyphRings(user);
  X0 := G^.LX;
  Y0 := G^.LY;
  for I := 1 to 10 do
  begin
    T := I / 10;
    U := 1 - T;
    GRPoint(G, U * U * U * X0 + 3 * U * U * T * c1x + 3 * U * T * T * c2x + T * T * T * x,
      U * U * U * Y0 + 3 * U * U * T * c1y + 3 * U * T * T * c2y + T * T * T * y);
  end;
end;

procedure GRClose(user: Pointer); cdecl;
begin
end;

{ a glyph turned (Angle degrees clockwise) about its pen at (PX, PY), SizePx pixels to the em: its outline filled }
procedure DrawGlyphTurned(Img: TLazIntfImage; AFont: Ppd_font; Glyph: UInt32; PX, PY, SizePx, Angle: Double;
  Col: UInt32);
var
  M: pd_font_metrics;
  G: TGlyphRings;
  S: pd_outline_sink;
begin
  if (pd_font_get_metrics(AFont, M) <> PD_OK) or (M.units_per_em <= 0) then
    Exit;
  G.Rings := nil;
  G.Sc := SizePx / M.units_per_em / 64;
  G.CosT := Cos(Angle * Pi / 180);
  G.SinT := Sin(Angle * Pi / 180);
  G.PX := PX;
  G.PY := PY;
  G.LX := 0;
  G.LY := 0;
  S.move_to := @GRMove;
  S.line_to := @GRLine;
  S.quad_to := @GRQuad;
  S.cubic_to := @GRCubic;
  S.close := @GRClose;
  if (pd_font_glyph_outline(AFont, Glyph, S, @G) = PD_OK) and (Length(G.Rings) > 0) then
    FillRingsImg(Img, G.Rings, (Col and $FFFFFF) or $FF000000);
end;

{ whether a point is inside a polygon (even-odd) }
function InPolygon(const P: TPtDArray; X, Y: Double): Boolean;
var
  I, J: Integer;
begin
  Result := False;
  J := High(P);
  for I := 0 to High(P) do
  begin
    if ((P[I].Y > Y) <> (P[J].Y > Y)) and (X < (P[J].X - P[I].X) * (Y - P[I].Y) / (P[J].Y - P[I].Y) + P[I].X) then
      Result := not Result;
    J := I;
  end;
end;

{ a picture (Pic, its own size W by H in pixels) over the page turned Rot degrees clockwise about (CX, CY), flipped;
  only within Crop (the rectangle as it is before it is turned, when CropOn) and inside Poly (when it has points) }
procedure BlendPictureEx(Img, Pic: TLazIntfImage; CX, CY, Rot: Double; FH, FV, CropOn: Boolean;
  const Crop: TRect; const Poly: TPtDArray);
var
  C, S, HX, HY, UX, UY, SXf, SYf: Double;
  X0, Y0, X1, Y1, X, Y, SX, SY, A: Integer;
  Src, Dst: PPixel;
begin
  C := Cos(Rot * Pi / 180);
  S := Sin(Rot * Pi / 180);
  HX := Pic.Width / 2;
  HY := Pic.Height / 2;
  X0 := Floor(CX - Abs(HX * C) - Abs(HY * S)) - 1;
  X1 := Ceil(CX + Abs(HX * C) + Abs(HY * S)) + 1;
  Y0 := Floor(CY - Abs(HX * S) - Abs(HY * C)) - 1;
  Y1 := Ceil(CY + Abs(HX * S) + Abs(HY * C)) + 1;
  if X0 < 0 then X0 := 0;
  if Y0 < 0 then Y0 := 0;
  if X1 > Img.Width then X1 := Img.Width;
  if Y1 > Img.Height then Y1 := Img.Height;
  for Y := Y0 to Y1 - 1 do
  begin
    Dst := PPixel(Img.GetDataLineStart(Y));
    Inc(Dst, X0);
    for X := X0 to X1 - 1 do
    begin
      { back to where it was before it was turned }
      UX := (X + 0.5 - CX) * C + (Y + 0.5 - CY) * S;
      UY := -(X + 0.5 - CX) * S + (Y + 0.5 - CY) * C;
      SXf := UX + HX;
      SYf := UY + HY;
      if FH then SXf := Pic.Width - SXf;
      if FV then SYf := Pic.Height - SYf;
      SX := Floor(SXf);
      SY := Floor(SYf);
      if (SX >= 0) and (SY >= 0) and (SX < Pic.Width) and (SY < Pic.Height) and
         (not CropOn or ((CX + UX >= Crop.Left) and (CX + UX < Crop.Right) and (CY + UY >= Crop.Top) and
         (CY + UY < Crop.Bottom))) and ((Length(Poly) < 3) or InPolygon(Poly, X + 0.5, Y + 0.5)) then
      begin
        Src := PPixel(Pic.GetDataLineStart(SY));
        Inc(Src, SX);
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
      end;
      Inc(Dst);
    end;
  end;
end;

procedure FillPolygonImg(Img: TLazIntfImage; const P: TPtDArray; Col: UInt32);
var
  R: TRings;
begin
  SetLength(R, 1);
  R[0] := P;
  FillRingsImg(Img, R, Col);
end;

{ a polyline stroked at a width: a filled quad along each segment }
procedure StrokePolylineImg(Img: TLazIntfImage; const P: TPtDArray; Closed: Boolean; W: Double; Col: UInt32);
var
  I, J, N, Last: Integer;
  Q: TPtDArray;
  Dx, Dy, L, Nx, Ny: Double;
begin
  N := Length(P);
  if N < 2 then
    Exit;
  if W < 0.75 then
    W := 0.75;     { a hairline still shows }
  SetLength(Q, 4);
  if Closed then Last := N - 1 else Last := N - 2;
  for I := 0 to Last do
  begin
    J := (I + 1) mod N;
    Dx := P[J].X - P[I].X;
    Dy := P[J].Y - P[I].Y;
    L := Sqrt(Dx * Dx + Dy * Dy);
    if L = 0 then
      Continue;
    Nx := -Dy / L * W / 2;
    Ny := Dx / L * W / 2;
    { a little past each end, so that the segments meet }
    Dx := Dx / L * W / 2;
    Dy := Dy / L * W / 2;
    Q[0].X := P[I].X + Nx - Dx; Q[0].Y := P[I].Y + Ny - Dy;
    Q[1].X := P[J].X + Nx + Dx; Q[1].Y := P[J].Y + Ny + Dy;
    Q[2].X := P[J].X - Nx + Dx; Q[2].Y := P[J].Y - Ny + Dy;
    Q[3].X := P[I].X - Nx - Dx; Q[3].Y := P[I].Y - Ny - Dy;
    FillPolygonImg(Img, Q, Col);
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
  Fam, Alts: string;
  Info: pd_font_info;
  F: Ppd_font;
  P: ^TParadeFontEntry;
begin
  E := TParadeEdit(user);
  Result := nil;
  Fam := LowerCase(StrPas(family));
  Cls := pd_doc_font_class(E.FDoc, family);
  { the names the document's font table says the family also goes by (ＭＳ 明朝 for MS Mincho): a face
    by one of them is the family itself }
  Alts := '';
  if (E.FDoc <> nil) and (family <> nil) and (pd_doc_font_info(E.FDoc, family, Info) = PD_OK) then
    Alts := ',' + LowerCase(StrPas(PAnsiChar(@Info.alt[0]))) + ',';
  repeat
    Best := -1;
    BestScore := MaxInt;
    for I := 0 to High(E.FFonts) do
    begin
      P := @E.FFonts[I];    { through a pointer, not "with": the fields would hide weight and italic }
      if P^.Failed then
        Continue;
      { family mismatch costs most, less when it is the same kind of face (a sans for Arial, a mono for
        Courier New) and less again for a face loaded already, not one of the system's many; then italic;
        then weight distance; a face not loaded yet loses a tie }
      Score := Abs(P^.Weight - weight) + 1000 * Ord((P^.Italic <> 0) <> (italic <> 0)) + Ord(P^.Lazy);
      if (Fam <> '') and (P^.Key <> Fam) and (Alts <> '') and (Pos(',' + P^.Key + ',', Alts) > 0) then
        Inc(Score, 10 + 20000 * Ord(P^.Lazy))
      else if (Fam <> '') and (P^.Key <> Fam) then
        Inc(Score, 100000 - 50000 * Ord(P^.Cls = Cls) + 20000 * Ord(P^.Lazy))
      else if (Fam = '') and (I > 0) and (P^.Key <> E.FFonts[0].Key) then
        Inc(Score, 100000 + 20000 * Ord(P^.Lazy));
      if Score < BestScore then
      begin
        BestScore := Score;
        Best := I;
      end;
    end;
    if Best < 0 then
      Exit;
    P := @E.FFonts[Best];
    if P^.Font = nil then
    begin   { first used: loaded now; a file that will not load is passed over from then on }
      if pd_font_load_file(PAnsiChar(P^.FileName), P^.FaceIndex, F) = PD_OK then
        P^.Font := F
      else
        P^.Failed := True;
    end;
    Result := P^.Font;
  until Result <> nil;
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

{ converter format of a file name: PD_CONV_*, PD_CONV_JDATA for .pdoc (BJData) and .jdoc (JSON), -1 unknown }
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
  if E = '.pptx' then Exit(PD_CONV_PPTX);
  if E = '.txt' then Exit(PD_CONV_TEXT);
  if (E = '.pdoc') or (E = '.jdoc') then Exit(PD_CONV_JDATA);
  Result := -1;
end;

var
  CF_Parade, CF_Html, CF_Rtf, CF_Shapes: TClipboardFormat;

procedure RegisterFormats;
begin
  if CF_Parade <> 0 then
    Exit;
  CF_Parade := RegisterClipboardFormat('application/x-parade');
  CF_Shapes := RegisterClipboardFormat('application/x-parade-shapes');
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

{ the I-beam set again each time the pointer comes in. Set only when the control is made, GTK2 applies it
  before the control's window exists and it is lost -- an arrow over the text -- and since LCL keeps the
  cursor it last set and skips setting the same one again, it has to be another one first. }
procedure TParadeEdit.MouseEnter;
begin
  inherited MouseEnter;
  SetTempCursor(crArrow);
  SetTempCursor(Cursor);
end;

constructor TParadeEdit.Create(AOwner: TComponent);
begin
  FSnapShapes := True;
  inherited Create(AOwner);
  FHybridDefault := True;
  FShapeDrag := -1;
  FShapeFillDef := $00C47244;    { Office's blue (4472C4), outlined darker (2F528F) }
  FShapeLineDef := $008F522F;
  ControlStyle := ControlStyle + [csOpaque, csTripleClicks] - [csSetCaption];
  TabStop := True;
  Color := $00E0E0E0;
  Cursor := crIBeam;       { a text editor's: the scroll bar has its own, below }
  FZoom := 1.0;
  FPageGap := 16;
  FAuthor := GetEnvironmentVariable('USER');
  if FAuthor = '' then
    FAuthor := GetEnvironmentVariable('USERNAME');
  if FAuthor = '' then
    FAuthor := 'Author';
  { The scroll bar in a panel of its own, as wide as it is.  A GTK scroll bar
    has no window of its own and shows the cursor of the window it is in --
    this control's I-beam, which is what the pointer showed over the bar.
    The panel is a window, with the arrow. }
  FScrollHost := TPanel.Create(Self);
  FScrollHost.BevelOuter := bvNone;
  FScrollHost.Caption := '';
  FScrollHost.Align := alRight;
  FScrollHost.AutoSize := True;
  FScrollHost.Cursor := crArrow;
  FScrollHost.Parent := Self;
  FScrollBar := TScrollBar.Create(Self);
  FScrollBar.Kind := sbVertical;
  FScrollBar.Align := alRight;
  FScrollBar.Cursor := crArrow;
  FScrollBar.Parent := FScrollHost;
  FScrollBar.OnChange := @ScrollBarChange;
  FBlink := TTimer.Create(Self);
  FBlink.Interval := 530;
  FBlink.OnTimer := @BlinkTimer;
  FBlink.Enabled := False;
  FPrefetch := TTimer.Create(Self);
  FPrefetch.Interval := 30;
  FPrefetch.OnTimer := @PrefetchTick;
  FPrefetch.Enabled := False;
  NewDocument;
end;

destructor TParadeEdit.Destroy;
var
  I: Integer;
begin
  Application.RemoveAsyncCalls(Self);
  if FLayout <> nil then
    pd_layout_free(FLayout);
  if FDoc <> nil then
    pd_doc_free(FDoc);
  ClearGlyphCache;
  ClearPictures;
  FBack.Free;
  FBackImg.Free;
  for I := 0 to High(FFonts) do
    if FFonts[I].Font <> nil then
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
  FFonts[High(FFonts)] := Default(TParadeFontEntry);
  FFonts[High(FFonts)].Family := Family;
  FFonts[High(FFonts)].Weight := Weight;
  FFonts[High(FFonts)].Italic := Ord(Italic);
  FFonts[High(FFonts)].Font := F;
  FFonts[High(FFonts)].FileName := FileName;
  FFonts[High(FFonts)].Key := LowerCase(Family);
  FFonts[High(FFonts)].Cls := pd_font_family_class(PAnsiChar(Family));
  if FLayout <> nil then
  begin
    pd_layout_invalidate(FLayout);
    Relayout;
  end;
end;

procedure TParadeEdit.AddFontFile(const Family, FileName: string; Weight: Integer; Italic: Boolean;
  FaceIndex: Integer);
begin
  SetLength(FFonts, Length(FFonts) + 1);
  FFonts[High(FFonts)] := Default(TParadeFontEntry);
  { by index, not "with": the fields would hide the parameters of the same names }
  FFonts[High(FFonts)].Family := Family;
  FFonts[High(FFonts)].Weight := Weight;
  FFonts[High(FFonts)].Italic := Ord(Italic);
  FFonts[High(FFonts)].FileName := FileName;
  FFonts[High(FFonts)].FaceIndex := FaceIndex;
  FFonts[High(FFonts)].Lazy := True;
  FFonts[High(FFonts)].Key := LowerCase(Family);
  FFonts[High(FFonts)].Cls := pd_font_family_class(PAnsiChar(Family));
end;

{ the fonts tried for a character the run's font does not have (a check box's, CJK, arrows), those of them the
  system has: loaded once, the first time a document needs them }
procedure TParadeEdit.UseFallbackFonts;
const
  Families: array[0..8] of string = ('dejavu sans', 'noto sans symbols2', 'noto sans symbols', 'noto sans cjk sc',
    'noto sans cjk jp', 'wenquanyi micro hei', 'droid sans fallback', 'freeserif', 'symbola');
var
  I, K: Integer;
  F: Ppd_font;
begin
  if not FFallbackDone then
  begin
    FFallbackDone := True;
    SetLength(FFallback, 0);
    for K := 0 to High(Families) do
      for I := 0 to High(FFonts) do
        if (FFonts[I].Key = Families[K]) and (FFonts[I].Weight = 400) and (FFonts[I].Italic = 0) and
          not FFonts[I].Failed and not FFonts[I].FromDoc then
        begin
          if FFonts[I].Font = nil then
          begin
            if pd_font_load_file(PAnsiChar(FFonts[I].FileName), FFonts[I].FaceIndex, F) = PD_OK then
              FFonts[I].Font := F
            else
              FFonts[I].Failed := True;
          end;
          if FFonts[I].Font <> nil then
          begin
            SetLength(FFallback, Length(FFallback) + 1);
            FFallback[High(FFallback)] := FFonts[I].Font;
          end;
          Break;
        end;
  end;
  if (FDoc <> nil) and (Length(FFallback) > 0) then
    pd_doc_set_fallback_fonts(FDoc, @FFallback[0], Length(FFallback));
end;

function TParadeEdit.AddSystemFonts: Integer;
var
  Faces: TParadeSystemFaces;
  Have: TStringList;
  I, N: Integer;
begin
  Result := 0;
  Faces := ParadeSystemFaces;
  Have := TStringList.Create;    { the families registered already keep their faces: no system ones beside them }
  try
    Have.Sorted := True;
    Have.Duplicates := dupIgnore;
    for I := 0 to High(FFonts) do
      Have.Add(FFonts[I].Key);
    N := Length(FFonts);
    SetLength(FFonts, N + Length(Faces));
    for I := 0 to High(Faces) do
      if Have.IndexOf(LowerCase(Faces[I].Family)) < 0 then
      begin
        FFonts[N] := Default(TParadeFontEntry);
        FFonts[N].Family := Faces[I].Family;
        FFonts[N].Weight := Faces[I].Weight;
        FFonts[N].Italic := Ord(Faces[I].Italic);
        FFonts[N].FileName := Faces[I].FileName;
        FFonts[N].FaceIndex := Faces[I].Index;
        FFonts[N].Lazy := True;
        FFonts[N].Key := LowerCase(Faces[I].Family);
        FFonts[N].Cls := pd_font_family_class(PAnsiChar(Faces[I].Family));
        Inc(N);
        Inc(Result);
      end;
    SetLength(FFonts, N);
  finally
    Have.Free;
  end;
  FFallbackDone := False;   { looked for again, among these }
  UseFallbackFonts;
end;

{ the fonts the document carries, ahead of the registered ones of the same family; the last document's go }
procedure TParadeEdit.UseDocumentFonts;
var
  I, K: Integer;
  R: pd_res_id;
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
  M, Fam: string;
  F: Ppd_font;
  Kept: array of TParadeFontEntry;
begin
  Kept := nil;
  for I := 0 to High(FFonts) do
    if FFonts[I].FromDoc then
    begin
      if FFonts[I].Font <> nil then
        pd_font_free(FFonts[I].Font);
    end
    else
    begin
      SetLength(Kept, Length(Kept) + 1);
      Kept[High(Kept)] := FFonts[I];
    end;
  FFonts := Kept;
  R := 1;
  while pd_doc_resource(FDoc, R, @Mime, @Data, @Len) = PD_OK do
  begin
    M := StrPas(Mime);
    K := Pos('family="', M);
    if (Copy(M, 1, 5) = 'font/') and (K > 0) and (pd_font_load_memory(Data, Len, 0, F) = PD_OK) then
    begin
      Fam := Copy(M, K + 8, MaxInt);
      Fam := Copy(Fam, 1, Pos('"', Fam) - 1);
      { after the registered ones: the first stays the default face for text that names none }
      SetLength(FFonts, Length(FFonts) + 1);
      FFonts[High(FFonts)] := Default(TParadeFontEntry);
      FFonts[High(FFonts)].Key := LowerCase(Fam);
      FFonts[High(FFonts)].Cls := pd_font_family_class(PAnsiChar(Fam));
      FFonts[High(FFonts)].Family := Fam;
      FFonts[High(FFonts)].Weight := 400 + 300 * Ord(Pos('weight=700', M) > 0);
      FFonts[High(FFonts)].Italic := Ord(Pos('italic=1', M) > 0);
      FFonts[High(FFonts)].Font := F;
      FFonts[High(FFonts)].FromDoc := True;
    end;
    Inc(R);
  end;
  ClearGlyphCache;
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
  if Assigned(FOnReplacing) then
    FOnReplacing(Self);
  ParadeCheck(pd_doc_new(D), 'new document');
  if FLayout <> nil then
    pd_layout_free(FLayout);
  if FDoc <> nil then
    pd_doc_free(FDoc);
  FDoc := D;
  pd_doc_set_stable_breaks(FDoc, Ord(FHybridDefault));
  pd_doc_set_font_resolver(FDoc, @ResolveFont, Self);
  pd_doc_set_math_font(FDoc, FMathFont);
  UseFallbackFonts;
  if FTrack then
    pd_doc_set_tracking(FDoc, PAnsiChar(FAuthor));
  ParadeCheck(pd_layout_new(FDoc, FLayout), 'layout');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_RIGHT, FCaret), 'caret');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_LEFT, FAnchor), 'anchor');
  FScrollY := 0;
  FFileName := '';
  FModified := False;
  FOrderRev := High(UInt64);
  FCanvasPage := False;
  FShapeOn := False;
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
  if Assigned(FOnReplacing) then
    FOnReplacing(Self);
  if FLayout <> nil then
    pd_layout_free(FLayout);
  if FDoc <> nil then
    pd_doc_free(FDoc);
  FDoc := D;
  if Format <> PD_CONV_JDATA then
    pd_doc_set_stable_breaks(FDoc, Ord(FHybridDefault));     { a native document says itself }
  UseDocumentFonts;
  { pictures the document only names (Markdown, HTML), from beside the file }
  Base := ExtractFilePath(ExpandFileName(FileName));
  if FileName = '' then
    Base := IncludeTrailingPathDelimiter(GetCurrentDir);
  pd_doc_load_images(FDoc, @FetchFile, @Base);
  pd_doc_set_font_resolver(FDoc, @ResolveFont, Self);
  pd_doc_set_math_font(FDoc, FMathFont);
  UseFallbackFonts;
  if FTrack then
    pd_doc_set_tracking(FDoc, PAnsiChar(FAuthor));
  ParadeCheck(pd_layout_new(FDoc, FLayout), 'layout');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_RIGHT, FCaret), 'caret');
  ParadeCheck(pd_doc_marker_new(FDoc, PdPos(FirstPara, 0), PD_GRAVITY_LEFT, FAnchor), 'anchor');
  FScrollY := 0;
  FFileName := FileName;
  FModified := False;
  FOrderRev := High(UInt64);
  FShapeOn := False;
  Relayout;
  FCanvasPage := DetectCanvasPage;    { a page to draw on, as it was made }
  EnsureCanvas;
  FPrefetchRes := 1;            { its pictures decoded in the pauses, so that paging to them does not wait }
  FPrefetch.Interval := 500;    { once the first page is up }
  FPrefetch.Enabled := True;
end;

{ one picture of the document decoded, the next one the next time; stopped when they all are }
procedure TParadeEdit.PrefetchTick(Sender: TObject);
var
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
  M: string;
begin
  FPrefetch.Interval := 30;
  while (FDoc <> nil) and (pd_doc_resource(FDoc, FPrefetchRes, @Mime, @Data, @Len) = PD_OK) do
  begin
    M := StrPas(Mime);
    Inc(FPrefetchRes);
    if (Pos('image/png', M) = 1) or (Pos('image/jpeg', M) = 1) or (Pos('image/gif', M) = 1) or
      (Pos('image/bmp', M) = 1) then
    begin
      if (Integer(FPrefetchRes) - 1 > Length(FPics)) or (FPics[FPrefetchRes - 2] = nil) then
      begin
        DecodePicture(FPrefetchRes - 1);
        Exit;   { one a tick: a key pressed meanwhile waits for one picture at most }
      end;
    end;
  end;
  FPrefetch.Enabled := False;
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
    SaveToStream(Fs, Fmt, LowerCase(ExtractFileExt(FileName)) <> '.jdoc');   { .pdoc and the rest binary }
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
  Fresh: Ppd_layout;
begin
  pd_pdf_options_init(Opt);
  { a layout of its own: the document as it lays out when opened, not the line breaks editing left }
  ParadeCheck(pd_layout_new(FDoc, Fresh), 'layout');
  try
    ParadeCheck(pd_layout_update(Fresh, nil), 'layout');
    Fs := TFileStream.Create(FileName, fmCreate);
    try
      ParadeCheck(pd_layout_write_pdf(Fresh, @Opt, @WriteToStream, Fs), 'PDF ' + FileName);
    finally
      Fs.Free;
    end;
  finally
    pd_layout_free(Fresh);
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
  Result := (ClientWidth - FScrollBar.Width - Round(Info.width * PxPerSp) - MarkupWidth) div 2;
  if Result < FPageGap then
    Result := FPageGap;
end;

{ the column right of the pages that holds the balloons, 0 when nothing needs one }
function TParadeEdit.MarkupWidth: Integer;
var
  Info: pd_page_info;
  Room: Integer;
begin
  Result := 0;
  if not FHasMarkup or (PageCount = 0) then
    Exit;
  { as wide as the zoom makes it, narrower when the window has less room beside the page }
  pd_layout_page_info(FLayout, 0, Info);
  Room := ClientWidth - FScrollBar.Width - Round(Info.width * PxPerSp) - 2 * FPageGap;
  Result := Round(250 * FZoom);
  if Room < Result then
    Result := Room;
  if Result < 150 then
    Result := 150;
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
  FScrollBar.PageSize := Math.Max(0, ClientHeight);
  FScrollBar.LargeChange := Math.Min(32767, Math.Max(1, ClientHeight * 9 div 10));   { 1..32767; 0 before it is shown }
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
var
  I: Integer;
  N: Int32;
begin
  { without fonts there is nothing to lay out yet: show no pages rather than fail }
  if Length(FFonts) > 0 then
    ParadeCheck(pd_layout_update(FLayout, nil), 'layout update');
  Inc(FLayoutEpoch);
  { a balloon column while any page has something to show in it }
  FHasMarkup := False;
  if (pd_doc_revision_count(FDoc) > 0) or (pd_doc_comment_count(FDoc) > 0) then
  begin
    I := 0;
    while (I < PageCount) and not FHasMarkup do
    begin
      FHasMarkup := (pd_layout_page_markup(FLayout, I, nil, 0, N) = PD_OK) and (N > 0);
      Inc(I);
    end;
  end;
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

{ Src (the page's pixel layout) at W x H: each pixel the mean of the source pixels it covers, alpha-weighted, when
  smaller; the nearest one when larger }
function ScalePicture(Src: TLazIntfImage; W, H: Integer): TLazIntfImage;
var
  SW, SH, X, Y, SX, SY, X0, X1, Y0, Y1, N: Integer;
  SR, SG, SB, SA: Int64;
  Px, Sp: PPixel;
  Rows: array of PPixel;
begin
  Result := NewImage(W, H, 0);
  SW := Src.Width;
  SH := Src.Height;
  if (SW <= 0) or (SH <= 0) then
    Exit;
  SetLength(Rows, SH);
  for Y := 0 to SH - 1 do
    Rows[Y] := PPixel(Src.GetDataLineStart(Y));
  for Y := 0 to H - 1 do
  begin
    Y0 := Int64(Y) * SH div H;
    Y1 := Int64(Y + 1) * SH div H;
    if Y1 <= Y0 then
      Y1 := Y0 + 1;
    Px := PPixel(Result.GetDataLineStart(Y));
    for X := 0 to W - 1 do
    begin
      X0 := Int64(X) * SW div W;
      X1 := Int64(X + 1) * SW div W;
      if X1 <= X0 then
        X1 := X0 + 1;
      if (X1 - X0 = 1) and (Y1 - Y0 = 1) then
        Px^ := (Rows[Y0] + X0)^
      else
      begin
        SR := 0;
        SG := 0;
        SB := 0;
        SA := 0;
        N := 0;
        for SY := Y0 to Y1 - 1 do
        begin
          Sp := Rows[SY] + X0;
          for SX := X0 to X1 - 1 do
          begin
            Inc(SR, Sp^.R * Sp^.A);
            Inc(SG, Sp^.G * Sp^.A);
            Inc(SB, Sp^.B * Sp^.A);
            Inc(SA, Sp^.A);
            Inc(N);
            Inc(Sp);
          end;
        end;
        if SA > 0 then
        begin
          Px^.R := SR div SA;
          Px^.G := SG div SA;
          Px^.B := SB div SA;
        end;
        Px^.A := SA div N;
      end;
      Inc(Px);
    end;
  end;
end;

{ a resource of the document decoded (once) as a picture, False if it is not one the LCL reads }
function TParadeEdit.DecodePicture(Res: pd_res_id): Boolean;
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
  Result := False;
  if Res = 0 then
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
        begin
          { once, into the page's own pixel layout: scaling then reads memory, not a pixel at a time
            through the image's colour accessor }
          Src := TRasterImage(Pic.Graphic).CreateIntfImage;
          try
            FPics[I] := NewImage(Src.Width, Src.Height, 0);
            if Src.DataDescription.Depth = 24 then
              { no alpha in the picture: opaque, which a copy into a layout with one would not say }
              for Y := 0 to Src.Height - 1 do
              begin
                Px := PPixel(FPics[I].GetDataLineStart(Y));
                for X := 0 to Src.Width - 1 do
                begin
                  C := Src.Colors[X, Y];
                  Px^.R := C.red shr 8;
                  Px^.G := C.green shr 8;
                  Px^.B := C.blue shr 8;
                  Px^.A := 255;
                  Inc(Px);
                end;
              end
            else
              FPics[I].CopyPixels(Src);
          finally
            Src.Free;
          end;
        end;
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
  Result := True;
end;

{ a resource of the document as a picture W x H pixels, nil if it is not one the LCL reads }
function TParadeEdit.GetPicture(Res: pd_res_id; W, H: Integer): TLazIntfImage;
var
  I: Integer;
begin
  Result := nil;
  if (Res = 0) or (W <= 0) or (H <= 0) or (W > 8000) or (H > 8000) or not DecodePicture(Res) then
    Exit;
  I := Integer(Res) - 1;
  { scaled once per size, the page being redrawn far more often than it is zoomed: smaller by the mean of the
    pixels each covers (a thin line in a photo stays a line, as other viewers show it), larger by the nearest }
  if (FPicSized[I] = nil) or (FPicSized[I].Width <> W) or (FPicSized[I].Height <> H) then
  begin
    FPicSized[I].Free;
    FPicSized[I] := ScalePicture(FPics[I], W, H);
  end;
  Result := FPicSized[I];
end;

{$PUSH}{$R-}{$Q-}   { the hash multiplies modulo 2^32: checked arithmetic would stop it }
function GlyphHash(AFont: Pointer; GlyphId: UInt32; PxPerEm: pd_sp; Sub: Integer): UInt32; inline;
begin
  Result := (UInt32(PtrUInt(AFont) shr 4) * 2654435761) xor (GlyphId * 40503) xor (UInt32(PxPerEm) * 2246822519) xor
            UInt32(Sub);
  Result := Result xor (Result shr 15);
end;
{$POP}

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
    I := Integer(GlyphHash(AFont, GlyphId, PxPerEm, Sub) and UInt32(Mask));
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
        I := Integer(GlyphHash(G^.KFont, G^.KGlyph, G^.KPx, G^.KSub) and UInt32(Mask));
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
  I := Integer(GlyphHash(AFont, GlyphId, PxPerEm, Sub) and UInt32(Mask));
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
  begin
    { over text the markup hides (deleted text out of the line) as if it were not there }
    while (Result.offset < UInt32(Length(S))) and HiddenAt(Result) do
      Result.offset := pd_text_next_grapheme(PAnsiChar(S), Length(S), Result.offset);
    if Result.offset < UInt32(Length(S)) then
      Result.offset := pd_text_next_grapheme(PAnsiChar(S), Length(S), Result.offset)   { a whole grapheme cluster }
  end
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
    while (Result.offset > 0) and HiddenAt(Result) do
      Result.offset := pd_text_prev_grapheme(PAnsiChar(S), Length(S), Result.offset);
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
  if not Extend and ClickPage(Page, PT(XPt), PT(YPt), []) then
    Exit;
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
  begin
    if ToEnd then
      P := PastEnds(P);
    SetCaret(P, Extend);
  end;
end;

{ past the ends of controls and links right after P, which take no room: where End and typing after them go }
function TParadeEdit.PastEnds(const P: pd_pos): pd_pos;
var
  S: string;
  O: pd_inline;
begin
  Result := P;
  S := ParaText(P.block);
  while (Result.offset + 3 <= UInt32(Length(S))) and (Copy(S, Result.offset + 1, 3) = #$EF#$BF#$BC) and
    (pd_doc_inline_at(FDoc, Result, O) = PD_OK) and
    (((O.kind = PD_INLINE_CONTROL) and (O.name[0] = #0)) or ((O.kind = PD_INLINE_LINK) and (O.source_len = 0))) do
    Inc(Result.offset, 3);
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
    DeleteText(PdRange(A, B), @After);
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
        DeleteText(PdRange(A, PdPos(A.block, Length(ParaText(A.block)))), nil)
      else if Blocks[I] = B.block then
        DeleteText(PdRange(PdPos(B.block, 0), B), nil)
      else
        DeleteText(PdRange(PdPos(Blocks[I], 0), PdPos(Blocks[I], Length(ParaText(Blocks[I])))), nil);
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
  if FReadOnly or TextLocked then
    Exit;
  if S = '' then
    Exit;
  Grouped := HasSelection or (Pos(#10, S) > 0) or PendingHere;     { the text and its formatting: one undo }
  { text typed over a selection takes the format of the selection's first character }
  Fmt := PD_FORMAT_INHERIT;
  if HasSelection then
    Fmt := StripRevision(FormatAt(SelStart));
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
  { formatting chosen with nothing selected, on what was just typed where it was chosen }
  if (FPending.mask <> 0) and (Pos(#10, S) = 0) and (After.block = FPendingAt.block) and
     (After.offset = FPendingAt.offset + UInt32(Length(S))) then
    pd_doc_set_char_props(FDoc, PdRange(FPendingAt, After), FPending);
  FPending.mask := 0;
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
  Cur := CurrentCharProps;   { the new state is the opposite of the selection start's }
  FillChar(Props, SizeOf(Props), 0);
  Props.mask := Mask;
  if Mask = PD_CP_WEIGHT then
  begin
    if Cur.weight >= 600 then Props.weight := 400 else Props.weight := 700;
  end
  else if Mask = PD_CP_ITALIC then
    Props.italic := Ord(Cur.italic = 0)
  else if Mask = PD_CP_UNDERLINE then
    Props.underline := Ord(Cur.underline = 0)
  else if Mask = PD_CP_STRIKE then
    Props.strike := Ord(Cur.strike = 0);
  ApplyCharProps(Props);
end;

{ the masked fields of B over A }
procedure MergeCharProps(var A: pd_char_props; const B: pd_char_props);
begin
  if B.mask and PD_CP_FAMILY <> 0 then A.family := B.family;
  if B.mask and PD_CP_SIZE <> 0 then A.size := B.size;
  if B.mask and PD_CP_WEIGHT <> 0 then A.weight := B.weight;
  if B.mask and PD_CP_ITALIC <> 0 then A.italic := B.italic;
  if B.mask and PD_CP_COLOR <> 0 then A.color := B.color;
  if B.mask and PD_CP_BACKGROUND <> 0 then A.background := B.background;
  if B.mask and PD_CP_UNDERLINE <> 0 then A.underline := B.underline;
  if B.mask and PD_CP_STRIKE <> 0 then A.strike := B.strike;
  if B.mask and PD_CP_SHIFT <> 0 then A.shift := B.shift;
  A.mask := A.mask or B.mask;
end;

function TParadeEdit.PendingHere: Boolean;
begin
  Result := (FPending.mask <> 0) and not HasSelection and (FPendingAt.block = CaretPos.block) and
    (FPendingAt.offset = CaretPos.offset);
end;

procedure TParadeEdit.StartFormatPainter(Sticky: Boolean);
begin
  FPainterProps := CurrentCharProps;
  { what a reader means by "the look": the font, its size and weight and slant, colours, lines, shift }
  FPainterProps.mask := PD_CP_FAMILY or PD_CP_SIZE or PD_CP_WEIGHT or PD_CP_ITALIC or PD_CP_COLOR or
    PD_CP_BACKGROUND or PD_CP_UNDERLINE or PD_CP_STRIKE or PD_CP_SHIFT;
  FPainter := True;
  FPainterSticky := Sticky;
  Cursor := crHandPoint;     { as long as it is on: a selection made now is painted }
  SetTempCursor(crIBeam);
  SetTempCursor(Cursor);
  FSelSig := '';
  FFrameKey := '';     { a text box of the document before is not one of this }
  CheckSelection;
end;

procedure TParadeEdit.StopFormatPainter;
begin
  if not FPainter then
    Exit;
  FPainter := False;
  Cursor := crIBeam;
  SetTempCursor(crArrow);
  SetTempCursor(Cursor);
  FSelSig := '';
  FFrameKey := '';     { a text box of the document before is not one of this }
  CheckSelection;
end;

procedure TParadeEdit.ApplyFormatPainter;
begin
  if not FPainter or not HasSelection then
    Exit;
  ApplyCharProps(FPainterProps);
  if not FPainterSticky then
    StopFormatPainter;
end;

function TParadeEdit.FormatPainterOn: Boolean;
begin
  Result := FPainter;
end;

procedure TParadeEdit.ApplyCharProps(const Props: pd_char_props);
begin
  if FReadOnly or (Props.mask = 0) then
    Exit;
  if HasSelection then
  begin
    pd_doc_set_char_props(FDoc, PdRange(SelStart, SelEnd), Props);
    FPending.mask := 0;
    Changed;
  end
  else
  begin   { nothing selected: kept for what is typed next, here }
    if not PendingHere then
    begin
      FillChar(FPending, SizeOf(FPending), 0);
      FPendingAt := CaretPos;
    end;
    MergeCharProps(FPending, Props);
    CheckSelection;
  end;
end;

function TParadeEdit.CurrentCharProps: pd_char_props;
var
  P: pd_pos;
begin
  P := SelStart;
  if (not HasSelection) and (P.offset > 0) then
    P.offset := P.offset - 1;     { typing at the caret continues the character before it }
  Result := PropsAt(P);
  if PendingHere then
    MergeCharProps(Result, FPending);
end;

procedure TParadeEdit.SetFontFamily(const Family: string);
var
  P: pd_char_props;
begin
  if Family = '' then
    Exit;
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_CP_FAMILY;
  StrPLCopy(P.family, Family, High(P.family));
  ApplyCharProps(P);
end;

procedure TParadeEdit.SetFontSize(Points: Double);
var
  P: pd_char_props;
begin
  if (Points < 1) or (Points > 1600) then
    Exit;
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_CP_SIZE;
  P.size := Round(Points * PD_SP_PER_PT);
  ApplyCharProps(P);
end;

const
  FONT_STEPS: array[0..15] of Double = (8, 9, 10, 10.5, 11, 12, 14, 16, 18, 20, 24, 28, 36, 48, 72, 96);

procedure TParadeEdit.StepFontSize(Up: Boolean);
var
  Cur: Double;
  I: Integer;
begin
  Cur := CurrentCharProps.size / PD_SP_PER_PT;
  if Up then
  begin
    for I := 0 to High(FONT_STEPS) do
      if FONT_STEPS[I] > Cur + 0.01 then
      begin
        SetFontSize(FONT_STEPS[I]);
        Exit;
      end;
    SetFontSize(Cur + 12);
  end
  else
  begin
    for I := High(FONT_STEPS) downto 0 do
      if FONT_STEPS[I] < Cur - 0.01 then
      begin
        SetFontSize(FONT_STEPS[I]);
        Exit;
      end;
    if Cur > 2 then
      SetFontSize(Cur - 1);
  end;
end;

procedure TParadeEdit.ToggleStrike;
begin
  ToggleCharProp(PD_CP_STRIKE);
end;

procedure TParadeEdit.ToggleSuperscript;
var
  P: pd_char_props;
begin
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_CP_SHIFT;
  if CurrentCharProps.shift <> PD_SHIFT_SUPER then
    P.shift := PD_SHIFT_SUPER;
  ApplyCharProps(P);
end;

procedure TParadeEdit.ToggleSubscript;
var
  P: pd_char_props;
begin
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_CP_SHIFT;
  if CurrentCharProps.shift <> PD_SHIFT_SUB then
    P.shift := PD_SHIFT_SUB;
  ApplyCharProps(P);
end;

procedure TParadeEdit.SetTextColor(RGB: Integer);
var
  P: pd_char_props;
begin
  if RGB < 0 then
  begin   { back to the style's colour }
    if HasSelection and not FReadOnly then
    begin
      pd_doc_clear_char_props(FDoc, PdRange(SelStart, SelEnd), PD_CP_COLOR);
      Changed;
    end;
    Exit;
  end;
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_CP_COLOR;
  P.color := $FF000000 or UInt32(RGB and $FFFFFF);
  ApplyCharProps(P);
end;

procedure TParadeEdit.SetHighlight(RGB: Integer);
var
  P: pd_char_props;
begin
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_CP_BACKGROUND;
  if RGB >= 0 then
    P.background := $FF000000 or UInt32(RGB and $FFFFFF);   { 0: none }
  ApplyCharProps(P);
end;

procedure TParadeEdit.ClearFormatting;
begin
  FPending.mask := 0;
  if FReadOnly or not HasSelection then
    Exit;
  pd_doc_clear_char_props(FDoc, PdRange(SelStart, SelEnd), $FFFFFFFF and not UInt32(PD_CP_REVISION));
  Changed;
end;

{ ---- paragraphs ---- }

function TParadeEdit.SelectedParagraphs: TParadeBlockArray;
var
  B: pd_block_id;
begin
  Result := nil;
  B := SelStart.block;
  while B <> 0 do
  begin
    SetLength(Result, Length(Result) + 1);
    Result[High(Result)] := B;
    if B = SelEnd.block then
      Break;
    B := pd_doc_next_paragraph(FDoc, B);
  end;
end;

{ the masked fields of B over A }
procedure MergeParaProps(var A: pd_para_props; const B: pd_para_props);
begin
  if B.mask and PD_PP_ALIGN <> 0 then A.align := B.align;
  if B.mask and PD_PP_INDENT_LEFT <> 0 then A.indent_left := B.indent_left;
  if B.mask and PD_PP_INDENT_RIGHT <> 0 then A.indent_right := B.indent_right;
  if B.mask and PD_PP_INDENT_FIRST <> 0 then A.indent_first := B.indent_first;
  if B.mask and PD_PP_SPACE_BEFORE <> 0 then A.space_before := B.space_before;
  if B.mask and PD_PP_SPACE_AFTER <> 0 then A.space_after := B.space_after;
  if B.mask and PD_PP_LINE_SPACING <> 0 then A.line_spacing := B.line_spacing;
  A.mask := A.mask or B.mask;
end;

procedure TParadeEdit.ApplyParaProps(const Props: pd_para_props);
var
  Paras: TParadeBlockArray;
  Cur: pd_para_props;
  I: Integer;
begin
  if FReadOnly or (Props.mask = 0) then
    Exit;
  Paras := SelectedParagraphs;
  pd_doc_begin_group(FDoc, 'Paragraph');
  for I := 0 to High(Paras) do
  begin
    FillChar(Cur, SizeOf(Cur), 0);
    pd_doc_para_props(FDoc, Paras[I], Cur);     { its own, kept but for what changes }
    MergeParaProps(Cur, Props);
    pd_doc_set_para_props(FDoc, Paras[I], Cur);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

function TParadeEdit.CurrentParaProps: pd_para_props;
var
  Info: pd_block_info;
  Own: pd_para_props;
begin
  FillChar(Result, SizeOf(Result), 0);
  Result.line_spacing := 1000;
  if pd_doc_block_info(FDoc, CaretPos.block, Info) <> PD_OK then
    Exit;
  if Info.style <> 0 then
    pd_doc_style_resolve(FDoc, Info.style, @Result, nil);
  FillChar(Own, SizeOf(Own), 0);
  pd_doc_para_props(FDoc, CaretPos.block, Own);
  MergeParaProps(Result, Own);
end;

procedure TParadeEdit.SetAlignment(AAlign: Integer);
var
  P: pd_para_props;
begin
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_PP_ALIGN;
  P.align := AAlign;
  ApplyParaProps(P);
end;

procedure TParadeEdit.ChangeIndent(Deeper: Boolean);
const
  STEP = 36 * PD_SP_PER_PT;   { half an inch }
var
  Paras: TParadeBlockArray;
  Info: pd_block_info;
  Own, Style: pd_para_props;
  I, Lvl: Integer;
  NewLeft: pd_sp;
begin
  if FReadOnly then
    Exit;
  Paras := SelectedParagraphs;
  pd_doc_begin_group(FDoc, 'Indent');
  for I := 0 to High(Paras) do
  begin
    if pd_doc_block_info(FDoc, Paras[I], Info) <> PD_OK then
      Continue;
    if Info.list <> 0 then
    begin   { in a list: a level deeper or shallower }
      Lvl := Info.list_level + 2 * Ord(Deeper) - 1;
      if Lvl < 0 then Lvl := 0;
      if Lvl > 8 then Lvl := 8;
      pd_doc_set_list(FDoc, Paras[I], Info.list, Lvl);
      Continue;
    end;
    FillChar(Style, SizeOf(Style), 0);
    if Info.style <> 0 then
      pd_doc_style_resolve(FDoc, Info.style, @Style, nil);
    FillChar(Own, SizeOf(Own), 0);
    pd_doc_para_props(FDoc, Paras[I], Own);
    if Own.mask and PD_PP_INDENT_LEFT <> 0 then
      NewLeft := Own.indent_left
    else
      NewLeft := Style.indent_left;
    if Deeper then
      NewLeft := (NewLeft div STEP + 1) * STEP
    else if NewLeft mod STEP <> 0 then
      NewLeft := (NewLeft div STEP) * STEP
    else
      NewLeft := NewLeft - STEP;
    if NewLeft < 0 then
      NewLeft := 0;
    Own.indent_left := NewLeft;
    Own.mask := Own.mask or PD_PP_INDENT_LEFT;
    pd_doc_set_para_props(FDoc, Paras[I], Own);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.SetLineSpacing(PerMille: Integer);
var
  P: pd_para_props;
begin
  FillChar(P, SizeOf(P), 0);
  P.mask := PD_PP_LINE_SPACING;
  P.line_spacing := PerMille;
  ApplyParaProps(P);
end;

procedure TParadeEdit.SetParaSpacing(Before, After: Double);
var
  P: pd_para_props;
begin
  FillChar(P, SizeOf(P), 0);
  if Before >= 0 then
  begin
    P.mask := P.mask or PD_PP_SPACE_BEFORE;
    P.space_before := Round(Before * PD_SP_PER_PT);
  end;
  if After >= 0 then
  begin
    P.mask := P.mask or PD_PP_SPACE_AFTER;
    P.space_after := Round(After * PD_SP_PER_PT);
  end;
  ApplyParaProps(P);
end;

function TParadeEdit.ListFormatOf(Block: pd_block_id): Integer;
var
  Info: pd_block_info;
  Levels: array[0..8] of pd_list_level;
  N: Int32;
begin
  Result := -1;
  if (pd_doc_block_info(FDoc, Block, Info) <> PD_OK) or (Info.list = 0) then
    Exit;
  N := 0;
  if (pd_doc_list_info(FDoc, Info.list, @N, @Levels[0]) = PD_OK) and (N > 0) then
    Result := Levels[0].format;
end;

function TParadeEdit.CurrentListFormat: Integer;
begin
  Result := ListFormatOf(CaretPos.block);
end;

procedure TParadeEdit.ToggleList(AFormat: Integer);
const
  BULLETS: array[0..2] of string = (#$E2#$80#$A2, #$E2#$97#$A6, #$E2#$96#$AA);   { bullet, white bullet, square }
var
  Paras: TParadeBlockArray;
  Info: pd_block_info;
  Levels: array[0..8] of pd_list_level;
  List: pd_list_id;
  I: Integer;
  AllThat: Boolean;
  Prev: pd_block_id;
begin
  if FReadOnly then
    Exit;
  Paras := SelectedParagraphs;
  if Paras = nil then
    Exit;
  AllThat := True;
  for I := 0 to High(Paras) do
    if ListFormatOf(Paras[I]) <> AFormat then
      AllThat := False;
  pd_doc_begin_group(FDoc, 'List');
  if AllThat then
    for I := 0 to High(Paras) do
      pd_doc_set_list(FDoc, Paras[I], 0, 0)     { plain paragraphs again }
  else
  begin
    { the list just above, when it is the same kind: one list, numbered on }
    List := 0;
    Prev := pd_doc_prev_paragraph(FDoc, Paras[0]);
    if (Prev <> 0) and (ListFormatOf(Prev) = AFormat) and (pd_doc_block_info(FDoc, Prev, Info) = PD_OK) then
      List := Info.list;
    if List = 0 then
    begin
      FillChar(Levels, SizeOf(Levels), 0);
      for I := 0 to 8 do
      begin
        Levels[I].format := AFormat;
        Levels[I].start := 1;
        Levels[I].indent := (I + 1) * 36 * PD_SP_PER_PT;
        Levels[I].hanging := 18 * PD_SP_PER_PT;
        if AFormat = PD_NUM_BULLET then
          StrPLCopy(Levels[I].text, BULLETS[I mod 3], High(Levels[I].text))
        else
        begin
          { 1. a. i. 1. a. i. ... down the levels of a numbered list, as word processors do }
          if AFormat = PD_NUM_DECIMAL then
            case I mod 3 of
              1: Levels[I].format := PD_NUM_LOWER_ALPHA;
              2: Levels[I].format := PD_NUM_LOWER_ROMAN;
            end;
          StrPLCopy(Levels[I].text, '%' + IntToStr(I + 1) + '.', High(Levels[I].text));
        end;
      end;
      pd_doc_list_define(FDoc, 9, @Levels[0], List);
    end;
    for I := 0 to High(Paras) do
      if ListFormatOf(Paras[I]) <> AFormat then
        pd_doc_set_list(FDoc, Paras[I], List, 0);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.GetParagraphStyles(List: TStrings);
var
  I: Integer;
  Kind: Int32;
  St: pd_style_id;
begin
  List.Clear;
  for I := 0 to pd_doc_style_count(FDoc) - 1 do
  begin
    St := pd_doc_style_at(FDoc, I);
    Kind := -1;
    if (pd_doc_style_info(FDoc, St, @Kind, nil, nil, nil) = PD_OK) and (Kind = PD_STYLE_PARAGRAPH) then
      List.Add(pd_doc_style_name(FDoc, St));
  end;
end;

function TParadeEdit.CurrentStyleName: string;
var
  Info: pd_block_info;
begin
  Result := '';
  if (pd_doc_block_info(FDoc, CaretPos.block, Info) = PD_OK) and (Info.style <> 0) then
    Result := pd_doc_style_name(FDoc, Info.style);
end;

procedure TParadeEdit.GetFontFamilies(List: TStrings);
var
  I: Integer;
  L: TStringList;
begin
  L := TStringList.Create;
  try
    L.Sorted := True;
    L.Duplicates := dupIgnore;
    L.CaseSensitive := False;
    for I := 0 to High(FFonts) do
      L.Add(FFonts[I].Family);
    List.Assign(L);
  finally
    L.Free;
  end;
end;

{ ---- the Insert tab ---- }

{ the object at the caret, over the selection; the caret after it }
procedure TParadeEdit.InsertObject(const Obj: pd_inline);
var
  After: pd_pos;
begin
  DeleteSelection;
  if pd_doc_insert_inline(FDoc, CaretPos, Obj, @After) = PD_OK then
  begin
    pd_doc_marker_set(FDoc, FCaret, After);
    pd_doc_marker_set(FDoc, FAnchor, After);
  end;
end;

{ the width the caret's paragraph has to fill: its section's page less the margins }
function TParadeEdit.TextWidthAt(Block: pd_block_id): pd_sp;
var
  Info: pd_block_info;
  Sp: pd_section_props;
  B: pd_block_id;
begin
  Result := 0;
  B := Block;
  while (B <> 0) and (pd_doc_block_info(FDoc, B, Info) = PD_OK) do
  begin
    if Info.kind = PD_BLOCK_SECTION then
    begin
      if pd_doc_section_props(FDoc, B, Sp) = PD_OK then
        Result := Sp.page_width - Sp.margin_left - Sp.margin_right;
      Exit;
    end;
    B := Info.parent;
  end;
end;

{ where a block goes at the caret, among the paragraph's siblings: the paragraph split at the caret
  (unless the caret is at its start), the block before the second part }
function TParadeEdit.BlockSlot(out AParent: pd_block_id; out AIndex: Integer): Boolean;
var
  Info: pd_block_info;
  After: pd_pos;
begin
  Result := False;
  DeleteSelection;
  if CaretPos.offset > 0 then
  begin
    if pd_doc_split(FDoc, CaretPos, @After) <> PD_OK then
      Exit;
    pd_doc_marker_set(FDoc, FCaret, After);
    pd_doc_marker_set(FDoc, FAnchor, After);
  end;
  if pd_doc_block_info(FDoc, CaretPos.block, Info) <> PD_OK then
    Exit;
  AParent := Info.parent;
  AIndex := Info.index;
  Result := True;
end;

function TParadeEdit.InsertPicture(const FileName: string): Boolean;
var
  Data: TMemoryStream;
  Mime, Alt: string;
  Res: pd_res_id;
  O: pd_inline;
  W, H, Room: pd_sp;
begin
  Result := False;
  if FReadOnly then
    Exit;
  case LowerCase(ExtractFileExt(FileName)) of
    '.png': Mime := 'image/png';
    '.jpg', '.jpeg': Mime := 'image/jpeg';
    '.gif': Mime := 'image/gif';
  else
    Exit;
  end;
  Data := TMemoryStream.Create;
  try
    try
      Data.LoadFromFile(FileName);
    except
      Exit;
    end;
    if (Data.Size = 0) or (pd_doc_add_resource(FDoc, PAnsiChar(Mime), Data.Memory, Data.Size, Res) <> PD_OK) then
      Exit;
  finally
    Data.Free;
  end;
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_IMAGE;
  O.resource := Res;
  Alt := ChangeFileExt(ExtractFileName(FileName), '');
  O.alt := PAnsiChar(Alt);
  O.alt_len := Length(Alt);
  { its own size, 96 dpi; no wider than the text }
  pd_doc_image_display_size(FDoc, O, W, H);
  EnsureCanvas;
  if FShapeOn and CanvasSelected then
  begin   { a canvas selected: a picture of the canvas, in its middle, no bigger than it }
    Result := AddPictureShape(Res, W, H);
    Exit;
  end;
  Room := TextWidthAt(CaretPos.block);
  if (Room > 0) and (W > Room) then
  begin
    H := Round(H * (Room / W));
    W := Room;
  end;
  O.width := W;
  O.height := H;
  pd_doc_begin_group(FDoc, 'Insert picture');
  InsertObject(O);
  pd_doc_end_group(FDoc);
  Changed;
  Result := True;
end;

procedure TParadeEdit.InsertLink(const URL, AText: string);
var
  O: pd_inline;
  A, B, After: pd_pos;
  T: string;
  P: pd_char_props;
begin
  if FReadOnly or (URL = '') then
    Exit;
  pd_doc_begin_group(FDoc, 'Insert link');
  if not HasSelection then
  begin
    T := AText;
    if T = '' then
      T := URL;
    A := CaretPos;
    pd_doc_insert_text(FDoc, A, PAnsiChar(T), Length(T), PD_FORMAT_INHERIT, @After);
    pd_doc_marker_set(FDoc, FAnchor, A);    { the inserted text selected: linked below }
    pd_doc_marker_set(FDoc, FCaret, After);
  end;
  A := SelStart;
  B := SelEnd;
  if A.block = B.block then
  begin
    { the end first: inserting it does not move where the start goes }
    FillChar(O, SizeOf(O), 0);
    O.kind := PD_INLINE_LINK;     { no address: where the link ends }
    pd_doc_insert_inline(FDoc, B, O, @After);
    O.source := PAnsiChar(URL);
    O.source_len := Length(URL);
    pd_doc_insert_inline(FDoc, A, O, nil);
    { the end moved by the start mark's three bytes }
    B.offset := B.offset + 3;
    After.offset := After.offset + 3;
    { shown as links are: blue, underlined }
    FillChar(P, SizeOf(P), 0);
    P.mask := PD_CP_COLOR or PD_CP_UNDERLINE;
    P.color := $FF0563C1;
    P.underline := PD_UNDERLINE_SINGLE;
    pd_doc_set_char_props(FDoc, PdRange(PdPos(A.block, A.offset + 3), B), P);
    pd_doc_marker_set(FDoc, FCaret, After);
    pd_doc_marker_set(FDoc, FAnchor, After);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.InsertBreak(Kind: Integer);
var
  Par, Br: pd_block_id;
  Index: Integer;
begin
  if FReadOnly then
    Exit;
  pd_doc_begin_group(FDoc, 'Insert break');
  if BlockSlot(Par, Index) and (pd_doc_insert_block(FDoc, Par, Index, PD_BLOCK_BREAK, Br) = PD_OK) then
    pd_doc_set_break(FDoc, Br, Kind);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.InsertTable(Rows, Cols: Integer);
var
  Par, T, Row, Cell, Para: pd_block_id;
  Index, R, C: Integer;
  Tp: pd_table_props;
  Room: pd_sp;
begin
  if FReadOnly or (Rows < 1) or (Cols < 1) then
    Exit;
  if Cols > PD_TABLE_MAX_COLS then
    Cols := PD_TABLE_MAX_COLS;
  pd_doc_begin_group(FDoc, 'Insert table');
  if BlockSlot(Par, Index) and (pd_doc_insert_block(FDoc, Par, Index, PD_BLOCK_TABLE, T) = PD_OK) then
  begin
    { a table starts as one row of one cell }
    for R := 0 to Rows - 1 do
    begin
      if R = 0 then
        Row := pd_doc_child(FDoc, T, 0)
      else if pd_doc_insert_block(FDoc, T, -1, PD_BLOCK_ROW, Row) <> PD_OK then
        Break;
      for C := 1 to Cols - 1 do
        pd_doc_insert_block(FDoc, Row, -1, PD_BLOCK_CELL, Cell);
    end;
    { ruled, across the text in equal columns }
    pd_table_props_init(Tp);
    pd_doc_table_props(FDoc, T, Tp);
    Tp.border := PD_SP_PER_PT div 2;
    Tp.border_color := $FF000000;
    Room := TextWidthAt(T);
    if Room > 0 then
    begin
      Tp.width := Room;
      Tp.ncols := Cols;
      for C := 0 to Cols - 1 do
        Tp.col_width[C] := Room div Cols;
    end;
    pd_doc_set_table_props(FDoc, T, Tp);
    Para := pd_doc_child(FDoc, pd_doc_child(FDoc, pd_doc_child(FDoc, T, 0), 0), 0);
    if Para <> 0 then
    begin
      pd_doc_marker_set(FDoc, FCaret, PdPos(Para, 0));
      pd_doc_marker_set(FDoc, FAnchor, PdPos(Para, 0));
    end;
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.InsertEquation(const Source: string; Display: Boolean);
var
  O: pd_inline;
  After: pd_pos;
begin
  if FReadOnly or (Source = '') then
    Exit;
  pd_doc_begin_group(FDoc, 'Insert equation');
  DeleteSelection;
  if Display then
  begin
    { a paragraph of its own: split before and after the caret as needed }
    if CaretPos.offset > 0 then
    begin
      pd_doc_split(FDoc, CaretPos, @After);
      pd_doc_marker_set(FDoc, FCaret, After);
    end;
    if ParaText(CaretPos.block) <> '' then
    begin
      pd_doc_split(FDoc, CaretPos, @After);
      pd_doc_marker_set(FDoc, FCaret, PdPos(pd_doc_prev_paragraph(FDoc, After.block), 0));
    end;
    pd_doc_marker_set(FDoc, FAnchor, CaretPos);
    pd_doc_set_role(FDoc, CaretPos.block, PD_ROLE_EQUATION, 0);
  end;
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_EQUATION;
  O.source := PAnsiChar(Source);
  O.source_len := Length(Source);
  { a first guess: the layout typesets it with the math font and takes its real size }
  O.width := Length(Source) * 5 * PD_SP_PER_PT;
  O.height := 8 * PD_SP_PER_PT;
  O.depth := 2 * PD_SP_PER_PT;
  InsertObject(O);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.InsertNote(const ANote: string; Endnote: Boolean);
var
  Story, Para: pd_block_id;
  O: pd_inline;
begin
  if FReadOnly then
    Exit;
  pd_doc_begin_group(FDoc, 'Insert note');
  if pd_doc_insert_block(FDoc, 0, -1, PD_BLOCK_STORY, Story) = PD_OK then
  begin
    Para := pd_doc_child(FDoc, Story, 0);    { a story starts with one paragraph }
    if (Para <> 0) and (ANote <> '') then
      pd_doc_insert_text(FDoc, PdPos(Para, 0), PAnsiChar(ANote), Length(ANote), PD_FORMAT_INHERIT, nil);
    FillChar(O, SizeOf(O), 0);
    O.kind := PD_INLINE_FOOTNOTE;
    O.target := Story;
    O.level := Ord(Endnote);
    InsertObject(O);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.InsertField(Kind: Integer);
var
  O: pd_inline;
begin
  if FReadOnly then
    Exit;
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_FIELD;
  O.field := Kind;
  pd_doc_begin_group(FDoc, 'Insert field');
  InsertObject(O);
  pd_doc_end_group(FDoc);
  Changed;
end;

{ ---- content controls ---- }

function TParadeEdit.ControlAt(const P: pd_pos; out Kind, Spec: string; out AStart, AEnd: pd_pos): Boolean;
var
  O: pd_inline;
begin
  Kind := '';
  Spec := '';
  Result := (FDoc <> nil) and (pd_doc_control_at(FDoc, P, AStart, AEnd) = PD_OK) and
    (pd_doc_inline_at(FDoc, AStart, O) = PD_OK);
  if Result then
  begin
    Kind := StrPas(PAnsiChar(@O.name[0]));
    if (O.source <> nil) and (O.source_len > 0) then
      SetString(Spec, O.source, O.source_len);
  end;
end;

function ControlJson(const Spec: string): TJSONObject;
var
  D: TJSONData;
begin
  Result := nil;
  if Spec <> '' then
    try
      D := GetJSON(Spec);
      if D is TJSONObject then
        Result := TJSONObject(D)
      else
        D.Free;
    except
      Result := nil;
    end;
  if Result = nil then
    Result := TJSONObject.Create;
end;

{ a check box's character: hex code point as Word writes it, UTF-8 }
function HexChar(const Hex, Default: string): string;
var
  C: LongInt;
begin
  C := StrToIntDef('$' + Hex, -1);
  if (C <= 0) or (C > $10FFFF) then
    C := StrToInt('$' + Default);
  Result := UnicodeToUTF8(C);
end;

procedure TParadeEdit.SetControl(const AStart, AEnd: pd_pos; const Spec, Content: string);
var
  O: pd_inline;
  Kind: array[0..31] of AnsiChar;
  C, After: pd_pos;
  Fmt: pd_format_id;
begin
  if FReadOnly or (pd_doc_inline_at(FDoc, AStart, O) <> PD_OK) then
    Exit;
  Move(O.name, Kind, SizeOf(Kind));
  C := PdPos(AStart.block, AStart.offset + 3);
  Fmt := PD_FORMAT_INHERIT;
  if AEnd.offset > C.offset then
    Fmt := StripRevision(FormatAt(C));
  pd_doc_begin_group(FDoc, 'Content control');
  if AEnd.offset > C.offset then
    pd_doc_delete(FDoc, PdRange(C, AEnd), nil);
  After := C;
  if Content <> '' then
    pd_doc_insert_text(FDoc, C, PAnsiChar(Content), Length(Content), Fmt, @After);
  { the start object, saying what the control holds now }
  pd_doc_delete(FDoc, PdRange(AStart, C), nil);
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_CONTROL;
  Move(Kind, O.name, SizeOf(Kind));
  O.source := PAnsiChar(Spec);
  O.source_len := Length(Spec);
  pd_doc_insert_inline(FDoc, AStart, O, nil);
  pd_doc_end_group(FDoc);
  pd_doc_marker_set(FDoc, FCaret, After);
  pd_doc_marker_set(FDoc, FAnchor, After);
  Changed;
end;

function TParadeEdit.ToggleCheckBox(const P: pd_pos): Boolean;
var
  Kind, Spec: string;
  A, B: pd_pos;
  J: TJSONObject;
  On: Boolean;
begin
  Result := ControlAt(P, Kind, Spec, A, B) and (Kind = 'checkbox') and not FReadOnly;
  if not Result then
    Exit;
  J := ControlJson(Spec);
  try
    if J.Get('lock', '') = 'contentLocked' then
      Exit(False);
    On := J.Get('checked', 0) = 0;
    J.Integers['checked'] := Ord(On);
    if J.IndexOfName('placeholder') >= 0 then
      J.Delete('placeholder');
    if On then
      SetControl(A, B, J.AsJSON, HexChar(J.Get('on', ''), '2612'))
    else
      SetControl(A, B, J.AsJSON, HexChar(J.Get('off', ''), '2610'));
  finally
    J.Free;
  end;
end;

function TParadeEdit.ControlItems(const P: pd_pos; Items: TStrings): Boolean;
var
  Kind, Spec: string;
  A, B: pd_pos;
  J: TJSONObject;
  L: TJSONArray;
  I: Integer;
begin
  Items.Clear;
  Result := ControlAt(P, Kind, Spec, A, B) and ((Kind = 'dropdown') or (Kind = 'combobox'));
  if not Result then
    Exit;
  J := ControlJson(Spec);
  try
    L := J.Get('items', TJSONArray(nil));
    if L <> nil then
      for I := 0 to L.Count - 1 do
        if (L.Items[I] is TJSONArray) and (TJSONArray(L.Items[I]).Count > 0) then
          Items.Add(TJSONArray(L.Items[I]).Strings[0]);
  finally
    J.Free;
  end;
end;

procedure TParadeEdit.ChooseControlItem(const P: pd_pos; Index: Integer);
var
  Kind, Spec: string;
  A, B: pd_pos;
  J: TJSONObject;
  L, It: TJSONArray;
begin
  if not ControlAt(P, Kind, Spec, A, B) then
    Exit;
  J := ControlJson(Spec);
  try
    L := J.Get('items', TJSONArray(nil));
    if (L = nil) or (Index < 0) or (Index >= L.Count) or not (L.Items[Index] is TJSONArray) then
      Exit;
    It := TJSONArray(L.Items[Index]);
    if It.Count > 1 then
      J.Strings['value'] := It.Strings[1]
    else
      J.Strings['value'] := It.Strings[0];
    if J.IndexOfName('placeholder') >= 0 then
      J.Delete('placeholder');
    SetControl(A, B, J.AsJSON, It.Strings[0]);
  finally
    J.Free;
  end;
end;

{ Word's date pictures (M/d/yyyy, MMMM d, yyyy, dddd) as FormatDateTime's: M month, m minute, H hour }
function WordDateFormat(const F: string): string;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(F) do
    case F[I] of
      'M': Result := Result + 'm';
      'm': Result := Result + 'n';
      'H': Result := Result + 'h';
      'y', 'd', 'h', 's': Result := Result + F[I];
      #39: Result := Result + '"';
    else
      if F[I] = ' ' then
        Result := Result + F[I]
      else
        Result := Result + '"' + F[I] + '"';  { a separator as it is, not the locale's (/ is its date one) }
    end;
  if Result = '' then
    Result := 'm/d/yyyy';
end;

procedure TParadeEdit.SetControlDate(const P: pd_pos; ADate: TDateTime);
var
  Kind, Spec: string;
  A, B: pd_pos;
  J: TJSONObject;
begin
  if not ControlAt(P, Kind, Spec, A, B) or (Kind <> 'date') then
    Exit;
  J := ControlJson(Spec);
  try
    J.Strings['date'] := FormatDateTime('yyyy"-"mm"-"dd"T00:00:00Z"', ADate);
    if J.IndexOfName('placeholder') >= 0 then
      J.Delete('placeholder');
    SetControl(A, B, J.AsJSON, FormatDateTime(WordDateFormat(J.Get('format', 'M/d/yyyy')), ADate));
  finally
    J.Free;
  end;
end;

function TParadeEdit.ControlDate(const P: pd_pos; out ADate: TDateTime): Boolean;
var
  Kind, Spec, S: string;
  A, B: pd_pos;
  J: TJSONObject;
begin
  ADate := Date;
  Result := ControlAt(P, Kind, Spec, A, B) and (Kind = 'date');
  if not Result then
    Exit;
  J := ControlJson(Spec);
  try
    S := J.Get('date', '');
    if Length(S) >= 10 then
      ADate := EncodeDate(StrToIntDef(Copy(S, 1, 4), 2000), StrToIntDef(Copy(S, 6, 2), 1), StrToIntDef(Copy(S, 9, 2), 1));
  finally
    J.Free;
  end;
end;

procedure TParadeEdit.InsertControl(const Kind: string; const Items: array of string);
var
  O: pd_inline;
  J: TJSONObject;
  L: TJSONArray;
  S, Content: string;
  I: Integer;
  P, After: pd_pos;
begin
  if FReadOnly then
    Exit;
  J := TJSONObject.Create;
  try
    Content := '';
    if Kind = 'checkbox' then
    begin
      J.Integers['checked'] := 0;
      J.Strings['on'] := '2612';
      J.Strings['off'] := '2610';
      Content := HexChar('2610', '2610');
    end
    else if (Kind = 'dropdown') or (Kind = 'combobox') then
    begin
      L := TJSONArray.Create;
      for I := 0 to High(Items) do
        L.Add(TJSONArray.Create([Items[I], Items[I]]));
      J.Add('items', L);
      J.Integers['placeholder'] := 1;
      Content := 'Choose an item.';
    end
    else if Kind = 'date' then
    begin
      J.Strings['format'] := 'M/d/yyyy';
      J.Integers['placeholder'] := 1;
      Content := 'Click to enter a date.';
    end
    else
    begin
      J.Integers['placeholder'] := 1;
      Content := 'Click here to enter text.';
    end;
    S := J.AsJSON;
  finally
    J.Free;
  end;
  pd_doc_begin_group(FDoc, 'Insert content control');
  DeleteSelection;
  P := CaretPos;
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_CONTROL;
  StrPLCopy(PAnsiChar(@O.name[0]), Kind, High(O.name));
  O.source := PAnsiChar(S);
  O.source_len := Length(S);
  pd_doc_insert_inline(FDoc, P, O, @After);
  pd_doc_insert_text(FDoc, After, PAnsiChar(Content), Length(Content), PD_FORMAT_INHERIT, @After);
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_CONTROL;      { no name: where it ends }
  pd_doc_insert_inline(FDoc, After, O, @After);
  pd_doc_end_group(FDoc);
  { the caret on its content, to type over (a text box) or click (the others) }
  pd_doc_marker_set(FDoc, FAnchor, PdPos(P.block, P.offset + 3));
  pd_doc_marker_set(FDoc, FCaret, PdPos(P.block, P.offset + 3 + UInt32(Length(Content))));
  Changed;
end;

procedure TParadeEdit.ControlMenuClick(Sender: TObject);
begin
  ChooseControlItem(FMenuAt, TMenuItem(Sender).Tag);
end;

{ a click on a check box ticks it; on a list, the list drops down; on a date, a calendar; True when it was one }
function TParadeEdit.ClickControl(const P: pd_pos; X, Y: Integer): Boolean;
var
  Kind, Spec: string;
  A, B: pd_pos;
  Items: TStringList;
  I: Integer;
  Mi: TMenuItem;
  D: TDateTime;
  Dlg: TCalendarDialog;
  Pt: TPoint;
begin
  Result := False;
  if FReadOnly or not ControlAt(P, Kind, Spec, A, B) then
    Exit;
  if Kind = 'checkbox' then
    Exit(ToggleCheckBox(P));
  if (Kind = 'dropdown') or (Kind = 'combobox') then
  begin
    Items := TStringList.Create;
    try
      if not ControlItems(P, Items) or (Items.Count = 0) then
        Exit;
      if FControlMenu = nil then
        FControlMenu := TPopupMenu.Create(Self);
      FControlMenu.Items.Clear;
      for I := 0 to Items.Count - 1 do
      begin
        Mi := TMenuItem.Create(FControlMenu);
        Mi.Caption := Items[I];
        Mi.Tag := I;
        Mi.OnClick := @ControlMenuClick;
        FControlMenu.Items.Add(Mi);
      end;
      FMenuAt := P;
      Pt := ClientToScreen(Point(X, Y));
      FControlMenu.PopUp(Pt.X, Pt.Y);
      Result := True;
    finally
      Items.Free;
    end;
  end
  else if Kind = 'date' then
  begin
    ControlDate(P, D);
    Dlg := TCalendarDialog.Create(nil);
    try
      Dlg.Date := D;
      if Dlg.Execute then
        SetControlDate(P, Dlg.Date);
      Result := True;
    finally
      Dlg.Free;
    end;
  end;
end;

{ ---- the Layout tab: sections ---- }

function TParadeEdit.CurrentSection: pd_block_id;
var
  Info: pd_block_info;
  B: pd_block_id;
begin
  Result := 0;
  B := CaretPos.block;
  while (B <> 0) and (pd_doc_block_info(FDoc, B, Info) = PD_OK) do
  begin
    if Info.kind = PD_BLOCK_SECTION then
      Exit(B);
    B := Info.parent;
  end;
end;

function TParadeEdit.CurrentSectionProps: pd_section_props;
begin
  pd_section_props_init(Result);
  if CurrentSection <> 0 then
    pd_doc_section_props(FDoc, CurrentSection, Result);
end;

procedure TParadeEdit.ApplySectionProps(const Props: pd_section_props);
var
  S: pd_block_id;
begin
  S := CurrentSection;
  if FReadOnly or (S = 0) then
    Exit;
  pd_doc_begin_group(FDoc, 'Page setup');
  pd_doc_set_section_props(FDoc, S, Props);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.SetMargins(ATop, ABottom, ALeft, ARight: Double);
var
  P: pd_section_props;
begin
  P := CurrentSectionProps;
  P.margin_top := Round(ATop * PD_SP_PER_PT);
  P.margin_bottom := Round(ABottom * PD_SP_PER_PT);
  P.margin_left := Round(ALeft * PD_SP_PER_PT);
  P.margin_right := Round(ARight * PD_SP_PER_PT);
  ApplySectionProps(P);
end;

procedure TParadeEdit.SetOrientation(Landscape: Boolean);
var
  P: pd_section_props;
  T: pd_sp;
begin
  P := CurrentSectionProps;
  if (P.page_width > P.page_height) <> Landscape then
  begin
    T := P.page_width;
    P.page_width := P.page_height;
    P.page_height := T;
    ApplySectionProps(P);
  end;
end;

procedure TParadeEdit.SetPageSize(AWidth, AHeight: Double);
var
  P: pd_section_props;
  W, H, T: pd_sp;
begin
  P := CurrentSectionProps;
  W := Round(AWidth * PD_SP_PER_PT);
  H := Round(AHeight * PD_SP_PER_PT);
  if (W > H) <> (P.page_width > P.page_height) then
  begin   { turned as the section is }
    T := W;
    W := H;
    H := T;
  end;
  P.page_width := W;
  P.page_height := H;
  ApplySectionProps(P);
end;

procedure TParadeEdit.SetColumns(Count: Integer);
var
  P: pd_section_props;
begin
  if (Count < 1) or (Count > 9) then
    Exit;
  P := CurrentSectionProps;
  P.columns := Count;
  if P.column_gap <= 0 then
    P.column_gap := 36 * PD_SP_PER_PT;
  ApplySectionProps(P);
end;

procedure TParadeEdit.InsertSectionBreak(Continuous: Boolean);
var
  Sec, NewSec, Par, First: pd_block_id;
  Info: pd_block_info;
  Index, I, N: Integer;
  P: pd_section_props;
begin
  Sec := CurrentSection;
  if FReadOnly or (Sec = 0) then
    Exit;
  P := CurrentSectionProps;
  pd_doc_begin_group(FDoc, 'Section break');
  { the caret's paragraph split; it and what follows it go to the new section }
  if BlockSlot(Par, Index) and (Par = Sec) and (pd_doc_block_info(FDoc, Sec, Info) = PD_OK) and
     (pd_doc_insert_block(FDoc, pd_doc_root(FDoc), Info.index + 1, PD_BLOCK_SECTION, NewSec) = PD_OK) then
  begin
    First := pd_doc_child(FDoc, NewSec, 0);    { a section starts with an empty paragraph }
    N := Info.child_count - Index;
    for I := 0 to N - 1 do
      pd_doc_move_block(FDoc, pd_doc_child(FDoc, Sec, Index), NewSec, -1);
    if (N > 0) and (First <> 0) then
      pd_doc_remove_block(FDoc, First);
    P.continuous := Ord(Continuous);
    P.first_page_number := 0;      { the page numbers go on }
    pd_doc_set_section_props(FDoc, NewSec, P);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

function TParadeEdit.HeaderFooterText(Footer: Boolean): string;
var
  P: pd_section_props;
  Story, B: pd_block_id;
  T: string;
  I: Integer;
  O: pd_inline;
begin
  Result := '';
  P := CurrentSectionProps;
  if Footer then Story := P.footer else Story := P.header;
  if Story = 0 then
    Exit;
  B := pd_doc_child(FDoc, Story, 0);
  if B = 0 then
    Exit;
  T := ParaText(B);
  I := 1;
  while I <= Length(T) do
  begin
    if (Copy(T, I, 3) = #$EF#$BF#$BC) and (pd_doc_inline_at(FDoc, PdPos(B, I - 1), O) = PD_OK) then
    begin
      if O.kind = PD_INLINE_FIELD then
        case O.field of
          PD_FIELD_PAGE: Result := Result + '{page}';
          PD_FIELD_PAGES: Result := Result + '{pages}';
          PD_FIELD_DATE: Result := Result + '{date}';
        end;
      Inc(I, 3);
    end
    else
    begin
      Result := Result + T[I];
      Inc(I);
    end;
  end;
end;

procedure TParadeEdit.SetHeaderFooter(Footer: Boolean; const AText: string; AAlign: Integer);
const
  FIELDS: array[0..2] of string = ('{page}', '{pages}', '{date}');
  KINDS: array[0..2] of Integer = (PD_FIELD_PAGE, PD_FIELD_PAGES, PD_FIELD_DATE);
var
  Sec, Story, B: pd_block_id;
  P: pd_section_props;
  At: pd_pos;
  Rest: string;
  I, K, Best, BestAt, Q: Integer;
  O: pd_inline;
  Pp: pd_para_props;
begin
  Sec := CurrentSection;
  if FReadOnly or (Sec = 0) then
    Exit;
  P := CurrentSectionProps;
  pd_doc_begin_group(FDoc, 'Header and footer');
  if AText = '' then
  begin
    if Footer then P.footer := 0 else P.header := 0;
  end
  else
  begin
    if pd_doc_insert_block(FDoc, 0, -1, PD_BLOCK_STORY, Story) = PD_OK then
    begin
      B := pd_doc_child(FDoc, Story, 0);
      At := PdPos(B, 0);
      Rest := AText;
      { the text, its {page}, {pages} and {date} as fields }
      while Rest <> '' do
      begin
        Best := -1;
        BestAt := MaxInt;
        for K := 0 to High(FIELDS) do
        begin
          Q := Pos(FIELDS[K], Rest);
          if (Q > 0) and (Q < BestAt) then
          begin
            Best := K;
            BestAt := Q;
          end;
        end;
        if Best < 0 then
          I := Length(Rest)
        else
          I := BestAt - 1;
        if I > 0 then
          pd_doc_insert_text(FDoc, At, PAnsiChar(Rest), I, PD_FORMAT_INHERIT, @At);
        if Best < 0 then
          Break;
        FillChar(O, SizeOf(O), 0);
        O.kind := PD_INLINE_FIELD;
        O.field := KINDS[Best];
        pd_doc_insert_inline(FDoc, At, O, @At);
        Delete(Rest, 1, I + Length(FIELDS[Best]));
      end;
      FillChar(Pp, SizeOf(Pp), 0);
      Pp.mask := PD_PP_ALIGN;
      Pp.align := AAlign;
      pd_doc_set_para_props(FDoc, B, Pp);
      if Footer then P.footer := Story else P.header := Story;
    end;
  end;
  pd_doc_set_section_props(FDoc, Sec, P);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.SetFirstPageNumber(N: Integer);
var
  P: pd_section_props;
begin
  P := CurrentSectionProps;
  P.first_page_number := N;
  ApplySectionProps(P);
end;

procedure TParadeEdit.SetLineNumbers(Every: Integer);
var
  P: pd_section_props;
begin
  P := CurrentSectionProps;
  P.line_numbers := Every;
  ApplySectionProps(P);
end;

{ ---- the Table tab ---- }

function TParadeEdit.CellAt(const P: pd_pos; out Cell, Row, Table: pd_block_id): Boolean;
var
  Info: pd_block_info;
  B: pd_block_id;
begin
  Result := False;
  Cell := 0;
  Row := 0;
  Table := 0;
  B := P.block;
  while (B <> 0) and (pd_doc_block_info(FDoc, B, Info) = PD_OK) do
  begin
    if Info.kind = PD_BLOCK_CELL then
    begin
      Cell := B;
      Row := Info.parent;
      if pd_doc_block_info(FDoc, Row, Info) <> PD_OK then
        Exit;
      Table := Info.parent;
      Exit(True);
    end;
    B := Info.parent;
  end;
end;

function TParadeEdit.InTable: Boolean;
var
  C, R, T: pd_block_id;
begin
  Result := CellAt(CaretPos, C, R, T);
end;

function TParadeEdit.CurrentTableProps: pd_table_props;
var
  C, R, T: pd_block_id;
begin
  pd_table_props_init(Result);
  if CellAt(CaretPos, C, R, T) then
    pd_doc_table_props(FDoc, T, Result);
end;

function TParadeEdit.CurrentCellProps: pd_cell_props;
var
  C, R, T: pd_block_id;
begin
  FillChar(Result, SizeOf(Result), 0);
  Result.col_span := 1;
  if CellAt(CaretPos, C, R, T) then
    pd_doc_cell_props(FDoc, C, Result);
end;

function TParadeEdit.CellSpan(Cell: pd_block_id): Integer;
var
  Cp: pd_cell_props;
begin
  Result := 1;
  if (pd_doc_cell_props(FDoc, Cell, Cp) = PD_OK) and (Cp.col_span > 1) then
    Result := Cp.col_span;
end;

{ the grid column a cell starts at: the spans of the cells before it }
function TParadeEdit.GridColumn(Row: pd_block_id; Index: Integer): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to Index - 1 do
    Inc(Result, CellSpan(pd_doc_child(FDoc, Row, I)));
end;

{ the index of the cell of a row covering a grid column (-1: the row ends before it) }
function TParadeEdit.CellAtColumn(Row: pd_block_id; Col: Integer): Integer;
var
  Info: pd_block_info;
  I, G: Integer;
begin
  Result := -1;
  if pd_doc_block_info(FDoc, Row, Info) <> PD_OK then
    Exit;
  G := 0;
  for I := 0 to Info.child_count - 1 do
  begin
    Inc(G, CellSpan(pd_doc_child(FDoc, Row, I)));
    if G > Col then
      Exit(I);
  end;
end;

{ the caret into a cell's first paragraph }
procedure TParadeEdit.CaretToCell(Cell: pd_block_id);
var
  B: pd_block_id;
  Info: pd_block_info;
begin
  B := Cell;
  while (B <> 0) and (pd_doc_block_info(FDoc, B, Info) = PD_OK) and (Info.kind <> PD_BLOCK_PARAGRAPH) do
    B := pd_doc_child(FDoc, B, 0);
  if B <> 0 then
  begin
    pd_doc_marker_set(FDoc, FCaret, PdPos(B, 0));
    pd_doc_marker_set(FDoc, FAnchor, PdPos(B, 0));
  end;
end;

{ column widths kept adding up to the table's width when one is added (At, Count 1) or removed (Count -1) }
procedure TParadeEdit.ResizeColumns(Table: pd_block_id; At, Count: Integer);
var
  Tp: pd_table_props;
  Total, Sum: Int64;
  I: Integer;
begin
  if (pd_doc_table_props(FDoc, Table, Tp) <> PD_OK) or (Tp.ncols <= 0) then
    Exit;
  Total := 0;
  for I := 0 to Tp.ncols - 1 do
    Inc(Total, Tp.col_width[I]);
  if Count > 0 then
  begin
    if Tp.ncols >= PD_TABLE_MAX_COLS then
      Exit;
    if At > Tp.ncols then At := Tp.ncols;
    for I := Tp.ncols downto At + 1 do
      Tp.col_width[I] := Tp.col_width[I - 1];
    Tp.col_width[At] := Total div Tp.ncols;    { as wide as the others are on average }
    Inc(Tp.ncols);
  end
  else
  begin
    if (At < 0) or (At >= Tp.ncols) then
      Exit;
    for I := At to Tp.ncols - 2 do
      Tp.col_width[I] := Tp.col_width[I + 1];
    Dec(Tp.ncols);
  end;
  Sum := 0;
  for I := 0 to Tp.ncols - 1 do
    Inc(Sum, Tp.col_width[I]);
  if (Sum > 0) and (Total > 0) then
    for I := 0 to Tp.ncols - 1 do
      Tp.col_width[I] := Tp.col_width[I] * Total div Sum;
  pd_doc_set_table_props(FDoc, Table, Tp);
end;

procedure TParadeEdit.TableInsertRow(Below: Boolean);
var
  C, R, T, NewRow, Cell: pd_block_id;
  Info: pd_block_info;
  I: Integer;
  Cp: pd_cell_props;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) or (pd_doc_block_info(FDoc, R, Info) <> PD_OK) then
    Exit;
  pd_doc_begin_group(FDoc, 'Insert row');
  if pd_doc_insert_block(FDoc, T, Info.index + Ord(Below), PD_BLOCK_ROW, NewRow) = PD_OK then
  begin
    { as many cells as this row, as wide }
    for I := 0 to Info.child_count - 1 do
    begin
      if I = 0 then
        Cell := pd_doc_child(FDoc, NewRow, 0)
      else
        pd_doc_insert_block(FDoc, NewRow, -1, PD_BLOCK_CELL, Cell);
      if CellSpan(pd_doc_child(FDoc, R, I)) > 1 then
      begin
        pd_doc_cell_props(FDoc, Cell, Cp);
        Cp.col_span := CellSpan(pd_doc_child(FDoc, R, I));
        pd_doc_set_cell_props(FDoc, Cell, Cp);
      end;
    end;
    pd_doc_block_info(FDoc, C, Info);
    CaretToCell(pd_doc_child(FDoc, NewRow, Info.index));
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.TableInsertColumn(Right: Boolean);
var
  C, R, T, Row, Cell: pd_block_id;
  Info, Ti: pd_block_info;
  Col, I, K, G, J: Integer;
  Cp: pd_cell_props;
  Done: Boolean;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) or (pd_doc_block_info(FDoc, C, Info) <> PD_OK) then
    Exit;
  Col := GridColumn(R, Info.index);
  if Right then
    Inc(Col, CellSpan(C));
  pd_doc_block_info(FDoc, T, Ti);
  pd_doc_begin_group(FDoc, 'Insert column');
  for I := 0 to Ti.child_count - 1 do
  begin
    Row := pd_doc_child(FDoc, T, I);
    pd_doc_block_info(FDoc, Row, Info);
    { a new cell where the grid column begins; inside a merged cell, the merge grows }
    G := 0;
    Done := False;
    for K := 0 to Info.child_count - 1 do
    begin
      if G = Col then
      begin
        pd_doc_insert_block(FDoc, Row, K, PD_BLOCK_CELL, Cell);
        Done := True;
        Break;
      end;
      J := CellSpan(pd_doc_child(FDoc, Row, K));
      if G + J > Col then
      begin
        pd_doc_cell_props(FDoc, pd_doc_child(FDoc, Row, K), Cp);
        Cp.col_span := J + 1;
        pd_doc_set_cell_props(FDoc, pd_doc_child(FDoc, Row, K), Cp);
        Done := True;
        Break;
      end;
      Inc(G, J);
    end;
    if not Done then
      pd_doc_insert_block(FDoc, Row, -1, PD_BLOCK_CELL, Cell);
  end;
  ResizeColumns(T, Col, 1);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.TableDeleteRow;
var
  C, R, T: pd_block_id;
  Info, Ri: pd_block_info;
  Next: pd_block_id;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) then
    Exit;
  pd_doc_block_info(FDoc, T, Info);
  if Info.child_count <= 1 then
  begin
    TableDelete;
    Exit;
  end;
  pd_doc_block_info(FDoc, R, Ri);
  pd_doc_begin_group(FDoc, 'Delete row');
  if Ri.index + 1 < Info.child_count then
    Next := pd_doc_child(FDoc, T, Ri.index + 1)
  else
    Next := pd_doc_child(FDoc, T, Ri.index - 1);
  CaretToCell(pd_doc_child(FDoc, Next, 0));
  pd_doc_remove_block(FDoc, R);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.TableDeleteColumn;
var
  C, R, T, Row, Cell, Keep: pd_block_id;
  Info, Ti: pd_block_info;
  Col, I, K: Integer;
  Cp: pd_cell_props;
  Empty: Boolean;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) or (pd_doc_block_info(FDoc, C, Info) <> PD_OK) then
    Exit;
  Col := GridColumn(R, Info.index);
  { the last column: the table goes }
  Empty := True;
  pd_doc_block_info(FDoc, T, Ti);
  for I := 0 to Ti.child_count - 1 do
  begin
    pd_doc_block_info(FDoc, pd_doc_child(FDoc, T, I), Info);
    if (Info.child_count > 1) or (CellSpan(pd_doc_child(FDoc, pd_doc_child(FDoc, T, I), 0)) > 1) then
      Empty := False;
  end;
  if Empty then
  begin
    TableDelete;
    Exit;
  end;
  pd_doc_begin_group(FDoc, 'Delete column');
  { the caret to a neighbour in this row }
  pd_doc_block_info(FDoc, C, Info);
  pd_doc_block_info(FDoc, R, Ti);
  if Info.index + 1 < Ti.child_count then
    Keep := pd_doc_child(FDoc, R, Info.index + 1)
  else if Info.index > 0 then
    Keep := pd_doc_child(FDoc, R, Info.index - 1)
  else
    Keep := 0;
  if Keep <> 0 then
    CaretToCell(Keep);
  pd_doc_block_info(FDoc, T, Ti);
  for I := 0 to Ti.child_count - 1 do
  begin
    Row := pd_doc_child(FDoc, T, I);
    K := CellAtColumn(Row, Col);
    if K < 0 then
      Continue;
    Cell := pd_doc_child(FDoc, Row, K);
    pd_doc_block_info(FDoc, Row, Info);
    if CellSpan(Cell) > 1 then
    begin   { a merged cell over it: narrower }
      pd_doc_cell_props(FDoc, Cell, Cp);
      Dec(Cp.col_span);
      pd_doc_set_cell_props(FDoc, Cell, Cp);
    end
    else if Info.child_count > 1 then
      pd_doc_remove_block(FDoc, Cell);
  end;
  ResizeColumns(T, Col, -1);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.TableDelete;
var
  C, R, T, Next: pd_block_id;
  Info: pd_block_info;
  After: pd_pos;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) or (pd_doc_block_info(FDoc, T, Info) <> PD_OK) then
    Exit;
  pd_doc_begin_group(FDoc, 'Delete table');
  { the caret to the paragraph after the table, or before it; one is made when there is neither }
  Next := 0;
  if Info.index + 1 < ChildCount(Info.parent) then
    Next := pd_doc_child(FDoc, Info.parent, Info.index + 1)
  else if Info.index > 0 then
    Next := pd_doc_child(FDoc, Info.parent, Info.index - 1);
  if Next = 0 then
    pd_doc_insert_block(FDoc, Info.parent, Info.index + 1, PD_BLOCK_PARAGRAPH, Next);
  pd_doc_remove_block(FDoc, T);
  CaretToCell(Next);
  After := CaretPos;
  pd_doc_marker_set(FDoc, FAnchor, After);
  pd_doc_end_group(FDoc);
  Changed;
end;

function TParadeEdit.ChildCount(Block: pd_block_id): Integer;
var
  Info: pd_block_info;
begin
  Result := 0;
  if pd_doc_block_info(FDoc, Block, Info) = PD_OK then
    Result := Info.child_count;
end;

{ what is in cell From put at the end of cell Into (an empty paragraph left behind in From) }
procedure TParadeEdit.MoveCellContent(From, Into: pd_block_id);
var
  Info: pd_block_info;
  B, Fresh: pd_block_id;
  I: Integer;
begin
  if pd_doc_block_info(FDoc, From, Info) <> PD_OK then
    Exit;
  { nothing to move when it holds one empty paragraph }
  if Info.child_count = 1 then
  begin
    B := pd_doc_child(FDoc, From, 0);
    if (pd_doc_block_info(FDoc, B, Info) = PD_OK) and (Info.kind = PD_BLOCK_PARAGRAPH) and (Info.text_length = 0) then
      Exit;
    pd_doc_block_info(FDoc, From, Info);
  end;
  pd_doc_insert_block(FDoc, From, 0, PD_BLOCK_PARAGRAPH, Fresh);
  for I := 1 to Info.child_count do
    pd_doc_move_block(FDoc, pd_doc_child(FDoc, From, 1), Into, -1);
end;

procedure TParadeEdit.TableMergeRight;
var
  C, R, T, Next: pd_block_id;
  Info, Ri: pd_block_info;
  Cp: pd_cell_props;
  Span: Integer;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) or (pd_doc_block_info(FDoc, C, Info) <> PD_OK) then
    Exit;
  pd_doc_block_info(FDoc, R, Ri);
  if Info.index + 1 >= Ri.child_count then
    Exit;
  Next := pd_doc_child(FDoc, R, Info.index + 1);
  Span := CellSpan(Next);
  pd_doc_begin_group(FDoc, 'Merge cells');
  MoveCellContent(Next, C);
  pd_doc_remove_block(FDoc, Next);
  pd_doc_cell_props(FDoc, C, Cp);
  if Cp.col_span < 1 then Cp.col_span := 1;
  Inc(Cp.col_span, Span);
  pd_doc_set_cell_props(FDoc, C, Cp);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.TableMergeDown;
var
  C, R, T, Below, BelowRow: pd_block_id;
  Info, Ri, Ti: pd_block_info;
  Cp: pd_cell_props;
  Col, K, I: Integer;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) or (pd_doc_block_info(FDoc, C, Info) <> PD_OK) then
    Exit;
  Col := GridColumn(R, Info.index);
  pd_doc_block_info(FDoc, R, Ri);
  pd_doc_block_info(FDoc, T, Ti);
  { the first row below not already continuing this cell }
  Below := 0;
  for I := Ri.index + 1 to Ti.child_count - 1 do
  begin
    BelowRow := pd_doc_child(FDoc, T, I);
    K := CellAtColumn(BelowRow, Col);
    if K < 0 then
      Exit;
    Below := pd_doc_child(FDoc, BelowRow, K);
    pd_doc_cell_props(FDoc, Below, Cp);
    if Cp.merge_up = 0 then
      Break;
    Below := 0;
  end;
  if Below = 0 then
    Exit;
  pd_doc_begin_group(FDoc, 'Merge cells');
  MoveCellContent(Below, C);
  pd_doc_cell_props(FDoc, Below, Cp);
  Cp.merge_up := 1;
  pd_doc_set_cell_props(FDoc, Below, Cp);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.TableSplitCell;
var
  C, R, T, Row, Cell: pd_block_id;
  Info, Ri, Ti: pd_block_info;
  Cp: pd_cell_props;
  I, K, Col: Integer;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) or (pd_doc_block_info(FDoc, C, Info) <> PD_OK) then
    Exit;
  pd_doc_begin_group(FDoc, 'Split cell');
  pd_doc_cell_props(FDoc, C, Cp);
  { across: cells again for the columns it spans }
  if Cp.col_span > 1 then
  begin
    for I := 2 to Cp.col_span do
      pd_doc_insert_block(FDoc, R, Info.index + 1, PD_BLOCK_CELL, Cell);
    Cp.col_span := 1;
    pd_doc_set_cell_props(FDoc, C, Cp);
  end;
  { down: the cells below that continue it are cells of their own }
  Col := GridColumn(R, Info.index);
  pd_doc_block_info(FDoc, R, Ri);
  pd_doc_block_info(FDoc, T, Ti);
  for I := Ri.index + 1 to Ti.child_count - 1 do
  begin
    Row := pd_doc_child(FDoc, T, I);
    K := CellAtColumn(Row, Col);
    if K < 0 then
      Break;
    Cell := pd_doc_child(FDoc, Row, K);
    pd_doc_cell_props(FDoc, Cell, Cp);
    if Cp.merge_up = 0 then
      Break;
    Cp.merge_up := 0;
    pd_doc_set_cell_props(FDoc, Cell, Cp);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

{ the cells between the selection's ends (a rectangle in the grid), or the caret's }
function TParadeEdit.SelectedCells: TParadeBlockArray;
var
  C1, R1, T1, C2, R2, T2, Row: pd_block_id;
  I1, I2: pd_block_info;
  RowA, RowB, ColA, ColB, I, K, G: Integer;
  Info: pd_block_info;
begin
  Result := nil;
  if not CellAt(SelStart, C1, R1, T1) then
    Exit;
  if not CellAt(SelEnd, C2, R2, T2) or (T2 <> T1) then
  begin
    SetLength(Result, 1);
    Result[0] := C1;
    Exit;
  end;
  pd_doc_block_info(FDoc, R1, I1);
  pd_doc_block_info(FDoc, R2, I2);
  RowA := Min(I1.index, I2.index);
  RowB := Max(I1.index, I2.index);
  pd_doc_block_info(FDoc, C1, I1);
  pd_doc_block_info(FDoc, C2, I2);
  ColA := Min(GridColumn(R1, I1.index), GridColumn(R2, I2.index));
  ColB := Max(GridColumn(R1, I1.index), GridColumn(R2, I2.index));
  for I := RowA to RowB do
  begin
    Row := pd_doc_child(FDoc, T1, I);
    pd_doc_block_info(FDoc, Row, Info);
    G := 0;
    for K := 0 to Info.child_count - 1 do
    begin
      if (G + CellSpan(pd_doc_child(FDoc, Row, K)) > ColA) and (G <= ColB) then
      begin
        SetLength(Result, Length(Result) + 1);
        Result[High(Result)] := pd_doc_child(FDoc, Row, K);
      end;
      Inc(G, CellSpan(pd_doc_child(FDoc, Row, K)));
    end;
  end;
end;

procedure TParadeEdit.SetCellShading(RGB: Integer);
var
  Cells: TParadeBlockArray;
  Cp: pd_cell_props;
  I: Integer;
begin
  if FReadOnly then
    Exit;
  Cells := SelectedCells;
  if Cells = nil then
    Exit;
  pd_doc_begin_group(FDoc, 'Shading');
  for I := 0 to High(Cells) do
  begin
    pd_doc_cell_props(FDoc, Cells[I], Cp);
    if RGB < 0 then
      Cp.background := 0
    else
      Cp.background := $FF000000 or UInt32(RGB and $FFFFFF);
    pd_doc_set_cell_props(FDoc, Cells[I], Cp);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.SetTableBorders(Points: Double);
var
  C, R, T: pd_block_id;
  Tp: pd_table_props;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) then
    Exit;
  pd_doc_table_props(FDoc, T, Tp);
  Tp.border := Round(Points * PD_SP_PER_PT);
  if Tp.border_color = 0 then
    Tp.border_color := $FF000000;
  Tp.border_sides := 0;
  pd_doc_begin_group(FDoc, 'Borders');
  pd_doc_set_table_props(FDoc, T, Tp);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.SetHeaderRow(Repeated: Boolean);
var
  C, R, T: pd_block_id;
  Tp: pd_table_props;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) then
    Exit;
  pd_doc_table_props(FDoc, T, Tp);
  Tp.header_rows := Ord(Repeated);
  pd_doc_begin_group(FDoc, 'Header row');
  pd_doc_set_table_props(FDoc, T, Tp);
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.DistributeColumns;
var
  C, R, T, Row: pd_block_id;
  Tp: pd_table_props;
  Info: pd_block_info;
  Total: Int64;
  I, N: Integer;
begin
  if FReadOnly or not CellAt(CaretPos, C, R, T) then
    Exit;
  pd_doc_table_props(FDoc, T, Tp);
  { the grid's columns: the widest row's }
  N := 0;
  pd_doc_block_info(FDoc, T, Info);
  for I := 0 to Info.child_count - 1 do
  begin
    Row := pd_doc_child(FDoc, T, I);
    N := Max(N, GridColumn(Row, ChildCount(Row)));
  end;
  if (N < 1) or (N > PD_TABLE_MAX_COLS) then
    Exit;
  Total := 0;
  for I := 0 to Tp.ncols - 1 do
    Inc(Total, Tp.col_width[I]);
  if Total <= 0 then
    Total := TextWidthAt(T);
  if Total <= 0 then
    Exit;
  Tp.ncols := N;
  for I := 0 to N - 1 do
    Tp.col_width[I] := Total div N;
  pd_doc_begin_group(FDoc, 'Distribute columns');
  pd_doc_set_table_props(FDoc, T, Tp);
  pd_doc_end_group(FDoc);
  Changed;
end;

{ ---- the References tab ---- }

{ a paragraph's text without its objects (U+FFFC) }
function TParadeEdit.PlainText(Block: pd_block_id): string;
begin
  Result := StringReplace(ParaText(Block), #$EF#$BF#$BC, '', [rfReplaceAll]);
end;

function TParadeEdit.StyleNameOf(Block: pd_block_id): string;
var
  Info: pd_block_info;
begin
  Result := '';
  if (pd_doc_block_info(FDoc, Block, Info) = PD_OK) and (Info.style <> 0) then
    Result := pd_doc_style_name(FDoc, Info.style);
end;

function IsTocStyle(const S: string): Boolean;
begin
  Result := (Copy(S, 1, 4) = 'TOC ');
end;

{ the style a level of the table of contents has, made when the document has none: indented by level, the page
  number at the right edge of the text after dot leaders }
function TParadeEdit.TocStyle(Level: Integer; Room: pd_sp): pd_style_id;
var
  SName: string;
  Pp: pd_para_props;
  Cp: pd_char_props;
  Id: UInt32;
begin
  if Level <= 0 then
    SName := 'TOC Heading'
  else
    SName := 'TOC ' + IntToStr(Level);
  Result := pd_doc_style_find(FDoc, PAnsiChar(SName));
  if Result <> 0 then
    Exit;
  FillChar(Pp, SizeOf(Pp), 0);
  FillChar(Cp, SizeOf(Cp), 0);
  if Level <= 0 then
  begin
    Pp.mask := PD_PP_SPACE_BEFORE or PD_PP_SPACE_AFTER;
    Pp.space_before := 12 * PD_SP_PER_PT;
    Pp.space_after := 6 * PD_SP_PER_PT;
    Cp.mask := PD_CP_WEIGHT or PD_CP_SIZE;
    Cp.weight := 700;
    Cp.size := 16 * PD_SP_PER_PT;
  end
  else
  begin
    Pp.mask := PD_PP_INDENT_LEFT or PD_PP_SPACE_AFTER or PD_PP_TABS;
    Pp.indent_left := (Level - 1) * 18 * PD_SP_PER_PT;
    Pp.space_after := 3 * PD_SP_PER_PT;
    Pp.ntabs := 1;
    Pp.tabs[0].position := Room;
    Pp.tabs[0].align := PD_TAB_RIGHT;
    Pp.tabs[0].leader := PD_LEADER_DOT;
  end;
  Id := 0;
  pd_doc_style_define(FDoc, PAnsiChar(SName), PD_STYLE_PARAGRAPH, pd_doc_style_find(FDoc, 'Normal'), @Pp, @Cp, @Id);
  Result := Id;
end;

{ the table of contents' paragraphs at a place among a container's blocks; how many there are }
function TParadeEdit.BuildToc(Container: pd_block_id; Index, MaxLevel: Integer): Integer;
var
  B, P: pd_block_id;
  Info: pd_block_info;
  Heads: TParadeBlockArray;
  Room: pd_sp;
  I: Integer;
  T: string;
  At: pd_pos;
  O: pd_inline;

  function NewPara(Style: pd_style_id; const S: string): pd_block_id;
  begin
    Result := 0;
    if pd_doc_insert_block(FDoc, Container, Index, PD_BLOCK_PARAGRAPH, Result) <> PD_OK then
      Exit;
    Inc(Index);
    pd_doc_set_para_style(FDoc, Result, Style);
    if S <> '' then
      pd_doc_insert_text(FDoc, PdPos(Result, 0), PAnsiChar(S), Length(S), PD_FORMAT_INHERIT, nil);
  end;

begin
  Result := 0;
  { the headings first: the table's own paragraphs are about to be among them }
  Heads := nil;
  B := pd_doc_next_paragraph(FDoc, 0);
  while B <> 0 do
  begin
    if (pd_doc_block_info(FDoc, B, Info) = PD_OK) and (Info.role = PD_ROLE_HEADING) and (Info.level >= 1) and
       (Info.level <= MaxLevel) and (PlainText(B) <> '') and not IsTocStyle(StyleNameOf(B)) then
    begin
      SetLength(Heads, Length(Heads) + 1);
      Heads[High(Heads)] := B;
    end;
    B := pd_doc_next_paragraph(FDoc, B);
  end;
  Room := TextWidthAt(Container);
  if Room <= 0 then
    Room := 468 * PD_SP_PER_PT;
  NewPara(TocStyle(0, Room), 'Contents');
  Inc(Result);
  if Heads = nil then
  begin
    NewPara(TocStyle(1, Room), 'No headings yet: give paragraphs a Heading style, then update the table.');
    Exit(Result + 1);
  end;
  for I := 0 to High(Heads) do
  begin
    pd_doc_block_info(FDoc, Heads[I], Info);
    T := PlainText(Heads[I]);
    P := NewPara(TocStyle(Info.level, Room), T);
    if P = 0 then
      Break;
    Inc(Result);
    At := PdPos(P, Length(T));
    pd_doc_insert_text(FDoc, At, #9, 1, PD_FORMAT_INHERIT, @At);    { to the right-aligned stop, after dots }
    FillChar(O, SizeOf(O), 0);
    O.kind := PD_INLINE_FIELD;
    O.field := PD_FIELD_REF_PAGE;
    O.target := Heads[I];
    pd_doc_insert_inline(FDoc, At, O, nil);
  end;
end;

procedure TParadeEdit.InsertTableOfContents(MaxLevel: Integer);
var
  Par: pd_block_id;
  Index, N: Integer;
begin
  if FReadOnly then
    Exit;
  pd_doc_begin_group(FDoc, 'Table of contents');
  if BlockSlot(Par, Index) then
  begin
    N := BuildToc(Par, Index, MaxLevel);
    { the caret after the table, where the paragraph it was in goes on }
    CaretToCell(pd_doc_child(FDoc, Par, Index + N));
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

function TParadeEdit.UpdateTableOfContents: Boolean;
var
  B, Par: pd_block_id;
  Info: pd_block_info;
  Index, Old, New, MaxLevel, I: Integer;
  S: string;
begin
  Result := False;
  if FReadOnly then
    Exit;
  { the first paragraph of a table of contents, and the run of them it starts }
  B := pd_doc_next_paragraph(FDoc, 0);
  while (B <> 0) and not IsTocStyle(StyleNameOf(B)) do
    B := pd_doc_next_paragraph(FDoc, B);
  if (B = 0) or (pd_doc_block_info(FDoc, B, Info) <> PD_OK) then
    Exit;
  Par := Info.parent;
  Index := Info.index;
  MaxLevel := 3;
  Old := 0;
  while (Index + Old < ChildCount(Par)) and IsTocStyle(StyleNameOf(pd_doc_child(FDoc, Par, Index + Old))) do
  begin
    S := StyleNameOf(pd_doc_child(FDoc, Par, Index + Old));
    MaxLevel := Max(MaxLevel, StrToIntDef(Copy(S, 5, 2), 0));
    Inc(Old);
  end;
  pd_doc_begin_group(FDoc, 'Update table of contents');
  { the new one in front of the old, then the old one out: the container is never empty }
  New := BuildToc(Par, Index, MaxLevel);
  for I := 1 to Old do
    pd_doc_remove_block(FDoc, pd_doc_child(FDoc, Par, Index + New));
  pd_doc_end_group(FDoc);
  Changed;
  Result := True;
end;

procedure TParadeEdit.InsertCaption(const Seq, AText: string);
var
  Info: pd_block_info;
  B: pd_block_id;
  At: pd_pos;
  O: pd_inline;
  S: string;
begin
  if FReadOnly or (Seq = '') or (pd_doc_block_info(FDoc, CaretPos.block, Info) <> PD_OK) then
    Exit;
  pd_doc_begin_group(FDoc, 'Caption');
  if pd_doc_insert_block(FDoc, Info.parent, Info.index + 1, PD_BLOCK_PARAGRAPH, B) = PD_OK then
  begin
    if pd_doc_style_find(FDoc, 'Caption') <> 0 then
      pd_doc_set_para_style(FDoc, B, pd_doc_style_find(FDoc, 'Caption'));
    pd_doc_set_role(FDoc, B, PD_ROLE_CAPTION, 0);
    S := Seq + ' ';
    pd_doc_insert_text(FDoc, PdPos(B, 0), PAnsiChar(S), Length(S), PD_FORMAT_INHERIT, @At);
    FillChar(O, SizeOf(O), 0);
    O.kind := PD_INLINE_FIELD;
    O.field := PD_FIELD_SEQ;
    StrPLCopy(O.name, Seq, High(O.name));
    pd_doc_insert_inline(FDoc, At, O, @At);
    if AText <> '' then
    begin
      S := ': ' + AText;
      pd_doc_insert_text(FDoc, At, PAnsiChar(S), Length(S), PD_FORMAT_INHERIT, @At);
    end;
    pd_doc_marker_set(FDoc, FCaret, At);
    pd_doc_marker_set(FDoc, FAnchor, At);
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

function TParadeEdit.ReferenceTargets: TParadeRefTargets;
var
  B: pd_block_id;
  Info: pd_block_info;
  T, S, SName: string;
  I, K: Integer;
  O: pd_inline;
  Seqs: TStringList;
  R: TParadeRefTarget;
begin
  Result := nil;
  Seqs := TStringList.Create;    { each sequence's count so far, as the layout numbers them }
  try
    B := pd_doc_next_paragraph(FDoc, 0);
    while B <> 0 do
    begin
      pd_doc_block_info(FDoc, B, Info);
      T := ParaText(B);
      R := Default(TParadeRefTarget);
      R.Block := B;
      S := '';
      I := 1;
      while I <= Length(T) do
        if Copy(T, I, 3) = #$EF#$BF#$BC then
        begin
          if (pd_doc_inline_at(FDoc, PdPos(B, I - 1), O) = PD_OK) and (O.kind = PD_INLINE_FIELD) and
             (O.field = PD_FIELD_SEQ) then
          begin
            SName := O.name;
            K := Seqs.IndexOf(SName);
            if K < 0 then
              K := Seqs.AddObject(SName, TObject(PtrInt(0)));
            Seqs.Objects[K] := TObject(PtrInt(Seqs.Objects[K]) + 1);
            if not R.IsCaption then
            begin
              R.IsCaption := True;
              R.Seq := SName;
              R.Number := PtrInt(Seqs.Objects[K]);
            end;
            S := S + IntToStr(PtrInt(Seqs.Objects[K]));
          end;
          Inc(I, 3);
        end
        else
        begin
          S := S + T[I];
          Inc(I);
        end;
      R.Text := S;
      if R.IsCaption or ((Info.role = PD_ROLE_HEADING) and (S <> '') and not IsTocStyle(StyleNameOf(B))) then
      begin
        R.Level := Info.level;
        SetLength(Result, Length(Result) + 1);
        Result[High(Result)] := R;
      end;
      B := pd_doc_next_paragraph(FDoc, B);
    end;
  finally
    Seqs.Free;
  end;
end;

procedure TParadeEdit.InsertCrossReference(const Target: TParadeRefTarget; What: TParadeRefWhat);
var
  O: pd_inline;
  S: string;
  After: pd_pos;
  K: Integer;
begin
  if FReadOnly or (Target.Block = 0) then
    Exit;
  pd_doc_begin_group(FDoc, 'Cross-reference');
  DeleteSelection;
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_FIELD;
  O.target := Target.Block;
  case What of
    prfLabel, prfNumber:
      if Target.IsCaption then
      begin
        if What = prfLabel then
        begin
          S := Target.Seq + ' ';
          pd_doc_insert_text(FDoc, CaretPos, PAnsiChar(S), Length(S), PD_FORMAT_INHERIT, @After);
          pd_doc_marker_set(FDoc, FCaret, After);
          pd_doc_marker_set(FDoc, FAnchor, After);
        end;
        O.field := PD_FIELD_REF_NUMBER;
        InsertObject(O);
      end
      else
        What := prfText;      { a heading has no number: its text }
    prfPage:
      begin
        O.field := PD_FIELD_REF_PAGE;
        InsertObject(O);
      end;
  end;
  if What = prfText then
  begin
    S := Target.Text;
    K := Pos(': ', S);
    if Target.IsCaption and (K > 0) then
      S := Copy(S, K + 2, MaxInt);    { the caption's own words, without "Figure 2: " }
    if S <> '' then
    begin
      pd_doc_insert_text(FDoc, CaretPos, PAnsiChar(S), Length(S), PD_FORMAT_INHERIT, @After);
      pd_doc_marker_set(FDoc, FCaret, After);
      pd_doc_marker_set(FDoc, FAnchor, After);
    end;
  end;
  pd_doc_end_group(FDoc);
  Changed;
end;

procedure TParadeEdit.InsertBookmark(const AName: string);
var
  O: pd_inline;
begin
  if FReadOnly or (AName = '') then
    Exit;
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_BOOKMARK;
  StrPLCopy(O.name, AName, High(O.name));
  pd_doc_begin_group(FDoc, 'Bookmark');
  InsertObject(O);
  pd_doc_end_group(FDoc);
  Changed;
end;

{ ---- the View tab ---- }

function TParadeEdit.PageWidthZoom: Double;
var
  Info: pd_page_info;
begin
  Result := FZoom;
  if (PageCount = 0) or (pd_layout_page_info(FLayout, 0, Info) <> PD_OK) or (Info.width <= 0) then
    Exit;
  Result := (ClientWidth - FScrollBar.Width - 2 * FPageGap - MarkupWidth) /
    (Info.width * SCREEN_PPI / 72.0 / PD_SP_PER_PT);
end;

function TParadeEdit.WholePageZoom: Double;
var
  Info: pd_page_info;
begin
  Result := PageWidthZoom;
  if (PageCount = 0) or (pd_layout_page_info(FLayout, 0, Info) <> PD_OK) or (Info.height <= 0) then
    Exit;
  Result := Min(Result, (ClientHeight - 2 * FPageGap) / (Info.height * SCREEN_PPI / 72.0 / PD_SP_PER_PT));
end;

procedure TParadeEdit.GetHeadings(List: TStrings);
var
  B: pd_block_id;
  Info: pd_block_info;
  T: string;
begin
  List.BeginUpdate;
  try
    List.Clear;
    B := pd_doc_next_paragraph(FDoc, 0);
    while B <> 0 do
    begin
      if (pd_doc_block_info(FDoc, B, Info) = PD_OK) and (Info.role = PD_ROLE_HEADING) and
         not IsTocStyle(StyleNameOf(B)) then
      begin
        T := PlainText(B);
        if T <> '' then
          List.AddObject(StringOfChar(' ', 3 * Max(0, Info.level - 1)) + T, TObject(PtrUInt(B)));
      end;
      B := pd_doc_next_paragraph(FDoc, B);
    end;
  finally
    List.EndUpdate;
  end;
end;

procedure TParadeEdit.GoToPos(const P: pd_pos);
var
  Page: Int32;
  X, Base, Asc, Desc: pd_sp;
begin
  FHasDesiredX := False;
  SetCaret(P, False);
  { the place near the top of the view, as following a link or a heading does, not at the edge }
  if pd_layout_caret(FLayout, CaretPos, Page, X, Base, Asc, Desc) = PD_OK then
  begin
    FScrollY := FScrollY + PageTop(Page) + Round((Base - Asc) * PxPerSp) - 3 * FPageGap;
    if FScrollY < 0 then
      FScrollY := 0;
    UpdateScrollBar;
    Invalidate;
  end;
end;

procedure TParadeEdit.SetShowMarks(AValue: Boolean);
begin
  if FShowMarks = AValue then
    Exit;
  FShowMarks := AValue;
  Invalidate;
end;

{ a pilcrow where each paragraph on the page ends }
procedure TParadeEdit.PaintMarks(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double);
var
  B: pd_block_id;
  CPage: Int32;
  CX, CBase, CAsc, CDesc: pd_sp;
  G: PGlyphBmp;
  Glyph: UInt32;
begin
  if Length(FFonts) = 0 then
    Exit;
  Glyph := pd_font_glyph_index(FFonts[0].Font, $B6);
  if Glyph = 0 then
    Exit;
  B := pd_doc_next_paragraph(FDoc, 0);
  while B <> 0 do
  begin
    if (pd_layout_caret(FLayout, PdPos(B, Length(ParaText(B))), CPage, CX, CBase, CAsc, CDesc) = PD_OK) and
       (CPage = Page) then
    begin
      { as tall as the text: a picture or a drawing on the line raises its ascent, not its descent }
      if CDesc > 0 then
        CAsc := Min(CAsc, 4 * CDesc);
      CAsc := Min(CAsc, 32 * PD_SP_PER_PT);
      G := GetGlyphBmp(FFonts[0].Font, Glyph, Round((CAsc + CDesc) * 0.85 * PxScale * PD_SP_PER_PT), 0);
      if G^.W > 0 then
        BlendGlyph(Img, G, OX + Round(CX * PxScale) + 2, OY + Round(CBase * PxScale), $9AA9C4);
    end;
    B := pd_doc_next_paragraph(FDoc, B);
  end;
end;

procedure TParadeEdit.CheckSelection;
var
  Sig: string;
begin
  if not Assigned(FOnSelectionChange) or FSelQueued then
    Exit;
  Sig := SysUtils.Format('%d:%d %d:%d %d %d %d %d %d %d %d', [CaretPos.block, CaretPos.offset, AnchorPos.block,
    AnchorPos.offset, pd_doc_revision(FDoc), FPending.mask, Ord(PendingHere), Ord(FPainter), Ord(FShapeOn),
    FShapeSid, Length(FShapeMore)]);
  if Sig = FSelSig then
    Exit;
  FSelSig := Sig;
  FSelQueued := True;
  Application.QueueAsyncCall(@SelectionNotify, 0);     { not from inside a paint }
end;

procedure TParadeEdit.SelectionNotify(Data: PtrInt);
begin
  FSelQueued := False;
  if Assigned(FOnSelectionChange) then
    FOnSelectionChange(Self);
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
  if FReadOnly then
    Exit;
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
var
  L: string;
  Done, Matched: Boolean;
  S: TParadeShapeStep;
begin
  if FReadOnly then
    Exit;
  L := StrPas(pd_doc_undo_label(FDoc));
  if pd_doc_can_redo(FDoc) = 0 then
    SetLength(FSelRedo, 0);       { an edit since: what was undone is gone }
  Matched := (Length(FSelUndo) > 0) and (FSelUndo[High(FSelUndo)].Lbl = L);
  if Assigned(FOnUndo) then
    Done := FOnUndo(Self, False)
  else
    Done := pd_doc_undo(FDoc) = PD_OK;
  if not Done then
    Exit;
  S.Lbl := L;                     { the selection the step left: back to it when it is redone }
  S.On := FShapeOn;
  S.At := FShapeAt;
  S.Sid := FShapeSid;
  S.More := Copy(FShapeMore);
  S.Valid := Matched;
  SetLength(FSelRedo, Length(FSelRedo) + 1);
  FSelRedo[High(FSelRedo)] := S;
  pd_doc_marker_set(FDoc, FAnchor, CaretPos);
  Changed;
  if Matched then
  begin   { a shape edit: what was selected when it was made }
    S := FSelUndo[High(FSelUndo)];
    SetLength(FSelUndo, Length(FSelUndo) - 1);
    RestoreShapeStep(S);
  end;
end;

procedure TParadeEdit.Redo;
var
  Done: Boolean;
  S, B: TParadeShapeStep;
begin
  if FReadOnly then
    Exit;
  if Assigned(FOnUndo) then
    Done := FOnUndo(Self, True)
  else
    Done := pd_doc_redo(FDoc) = PD_OK;
  if not Done then
    Exit;
  pd_doc_marker_set(FDoc, FAnchor, CaretPos);
  Changed;
  if Length(FSelRedo) = 0 then
    Exit;
  S := FSelRedo[High(FSelRedo)];
  SetLength(FSelRedo, Length(FSelRedo) - 1);
  if not S.Valid then
    Exit;
  B.Lbl := S.Lbl;                 { undone again: back to the selection it is redone from }
  B.On := FShapeOn;
  B.At := FShapeAt;
  B.Sid := FShapeSid;
  B.More := Copy(FShapeMore);
  B.Valid := True;
  SetLength(FSelUndo, Length(FSelUndo) + 1);
  FSelUndo[High(FSelUndo)] := B;
  RestoreShapeStep(S);
end;

procedure TParadeEdit.SelectAll;
begin
  if TextLocked then
  begin   { the canvas page: every shape on it }
    FShapeOn := False;
    EnsureCanvas;
    SelectShapesIn(-1e12, -1e12, 1e12, 1e12);
    Exit;
  end;
  pd_doc_marker_set(FDoc, FAnchor, PdPos(FirstPara, 0));
  pd_doc_marker_set(FDoc, FCaret, LastPos);
  Invalidate;
end;

function TParadeEdit.SelectionRange(out A, B: pd_pos): Boolean;
begin
  Result := HasSelection;
  if Result then
  begin
    A := SelStart;
    B := SelEnd;
  end;
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
  if FShapeOn and (FShapeSid >= 0) then
  begin
    CopyShapes;
    Exit;
  end;
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
  if FReadOnly then
    Exit;
  if TextLocked and not (FShapeOn and (FShapeSid >= 0)) then
    Exit;
  if FShapeOn and (FShapeSid >= 0) then
  begin
    CutShapes;
    Exit;
  end;
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
  if FReadOnly or TextLocked then
    Exit;
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
  if ReadClip(CF_Shapes, S) then
    PutShapesPayload(S, 'Paste')
  else if ReadClip(CF_Parade, S) then
    PasteData(PAnsiChar(S), Length(S), PD_CONV_JDATA)
  else if ReadClip(CF_Html, S) then
    PasteData(PAnsiChar(S), Length(S), PD_CONV_HTML)
  else if ReadClip(CF_Rtf, S) then
    PasteData(PAnsiChar(S), Length(S), PD_CONV_RTF)
  else
    InsertText(Clipboard.AsText);
end;

function TParadeEdit.StoryTopOf(B: pd_block_id): pd_block_id;
var
  Info: pd_block_info;
begin
  Result := B;
  while (pd_doc_block_info(FDoc, Result, Info) = PD_OK) and (Info.parent <> 0) do
    Result := Info.parent;
  if (pd_doc_block_info(FDoc, Result, Info) <> PD_OK) or (Info.kind <> PD_BLOCK_STORY) then
    Result := 0;
end;

function TParadeEdit.StoryDrawing(Story: pd_block_id; out D: pd_pos): Boolean;
var
  B: pd_block_id;
  T, Key: string;
  I, J: Integer;
  O: pd_inline;
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
  S: RawByteString;
begin
  Result := False;
  D := PdPos(0, 0);
  if Story = 0 then
    Exit;
  Key := '"story":' + IntToStr(Story);
  B := pd_doc_next_paragraph(FDoc, 0);
  while B <> 0 do
  begin
    T := ParaText(B);
    I := Pos(#$EF#$BF#$BC, T);
    while I > 0 do
    begin
      if (pd_doc_inline_at(FDoc, PdPos(B, I - 1), O) = PD_OK) and (O.kind = PD_INLINE_IMAGE) and
         (pd_doc_resource(FDoc, O.resource, @Mime, @Data, @Len) = PD_OK) and
         (StrComp(Mime, 'application/vnd.parade.drawing+json') = 0) then
      begin
        SetString(S, PAnsiChar(Data), Len);
        if (Pos(Key + ',', S) > 0) or (Pos(Key + '}', S) > 0) then
        begin
          D := PdPos(B, I - 1);
          Exit(True);
        end;
      end;
      J := Pos(#$EF#$BF#$BC, Copy(T, I + 3, MaxInt));    { the next object in the paragraph }
      if J > 0 then
        I := I + 2 + J
      else
        I := 0;
    end;
    B := pd_doc_next_paragraph(FDoc, B);
  end;
end;

function TParadeEdit.LeaveDrawingText: Boolean;
var
  D: pd_pos;
begin
  Result := StoryDrawing(StoryTopOf(CaretPos.block), D);
  if Result then
    SetCaret(PdPos(D.block, D.offset + 3), False);   { just after the drawing that holds it }
end;

{ the text box the caret is in: its drawing and its shape (-1 when the drawing names no shape for it) }
function TParadeEdit.CaretTextBox(out D: pd_pos; out Sid: Integer): Boolean;
var
  Story: pd_block_id;
  Boxes: TParadeShapeBoxes;
  I: Integer;
  Key: string;
begin
  Story := StoryTopOf(CaretPos.block);
  Key := IntToStr(Story) + ':' + IntToStr(pd_doc_revision(FDoc));
  if Key <> FFrameKey then
  begin   { looked up again only when the caret changes story or the document changes }
    FFrameKey := Key;
    FFrameSid := -1;
    if not StoryDrawing(Story, FFrameAt) then
      FFrameAt := PdPos(0, 0)
    else
    begin
      Boxes := DrawingShapes(FFrameAt);
      for I := 0 to High(Boxes) do
        if Boxes[I].Story = Story then
          FFrameSid := Boxes[I].Sid;
    end;
  end;
  D := FFrameAt;
  Sid := FFrameSid;
  Result := D.block <> 0;
end;

function TParadeEdit.SelectTextBox: Boolean;
var
  D: pd_pos;
  Sid: Integer;
begin
  Result := CaretTextBox(D, Sid);
  if not Result then
    Exit;
  SetLength(FShapeMore, 0);
  FShapeOn := True;
  FShapeAt := D;
  FShapeSid := Sid;
  if Sid < 0 then
    SelectObject(D)
  else
    SetCaret(D, False);
  Invalidate;
end;

function TParadeEdit.ShapePageBox(const D: pd_pos; Sid: Integer; out Page: Int32; out X0, Y0, X1, Y1: Double): Boolean;
var
  Boxes: TParadeShapeBoxes;
  X, Y, W, H, JW, JH: Double;
  I: Integer;
begin
  X0 := 0; Y0 := 0; X1 := 0; Y1 := 0;
  Result := DrawingPlace(D, Page, X, Y, W, H, JW, JH) and (JW > 0) and (JH > 0);
  if not Result then
    Exit;
  X0 := X; Y0 := Y; X1 := X + W; Y1 := Y + H;
  if Sid < 0 then
    Exit;
  Result := False;
  Boxes := DrawingShapes(D);
  for I := 0 to High(Boxes) do
    if Boxes[I].Sid = Sid then
    begin
      X0 := X + Boxes[I].X0 * W / JW;
      Y0 := Y + Boxes[I].Y0 * H / JH;
      X1 := X + Boxes[I].X1 * W / JW;
      Y1 := Y + Boxes[I].Y1 * H / JH;
      Result := True;
    end;
end;

function TParadeEdit.TextBoxEdgeAt(const D: pd_pos; SX, SY: pd_sp): Integer;
var
  Boxes: TParadeShapeBoxes;
  Pg: Int32;
  X, Y, W, H, JW, JH, M, Mi, L, T, R, B: Double;
  I: Integer;
begin
  Result := -1;
  if not DrawingPlace(D, Pg, X, Y, W, H, JW, JH) or (JW <= 0) or (JH <= 0) then
    Exit;
  M := 4 / PxPerSp;     { four pixels outside the edge, two inside it: a click at the text's start is the text's }
  Mi := 2 / PxPerSp;
  Boxes := DrawingShapes(D);
  for I := High(Boxes) downto 0 do    { the one drawn last first: on top }
    if Boxes[I].Story <> 0 then
    begin
      L := X + Boxes[I].X0 * W / JW;
      T := Y + Boxes[I].Y0 * H / JH;
      R := X + Boxes[I].X1 * W / JW;
      B := Y + Boxes[I].Y1 * H / JH;
      if (SX >= L - M) and (SX <= R + M) and (SY >= T - M) and (SY <= B + M) and
         ((SX <= L + Mi) or (SX >= R - Mi) or (SY <= T + Mi) or (SY >= B - Mi)) then
        Exit(Boxes[I].Sid);
    end;
end;

{ the text box the caret is in: its edge dashed, as Word shows the one being typed in -- where it is taken to be
  moved or sized }
procedure TParadeEdit.PaintTextBoxFrame;
var
  D: pd_pos;
  Sid, I: Integer;
  Pg: Int32;
  X, Y, W, H, JW, JH: Double;
  Boxes: TParadeShapeBoxes;
begin
  if FShapeOn or not CaretTextBox(D, Sid) or (Sid < 0) or not DrawingPlace(D, Pg, X, Y, W, H, JW, JH) or
     (JW <= 0) or (JH <= 0) then
    Exit;
  Boxes := DrawingShapes(D);
  for I := 0 to High(Boxes) do
    if Boxes[I].Sid = Sid then
    begin
      Canvas.Brush.Style := bsClear;
      Canvas.Pen.Color := $00D77800;
      Canvas.Pen.Width := 1;
      Canvas.Pen.Style := psDash;
      Canvas.Rectangle(PageLeft(Pg) + Round((X + Boxes[I].X0 * W / JW) * PxPerSp) - 1,
        PageTop(Pg) + Round((Y + Boxes[I].Y0 * H / JH) * PxPerSp) - 1,
        PageLeft(Pg) + Round((X + Boxes[I].X1 * W / JW) * PxPerSp) + 2,
        PageTop(Pg) + Round((Y + Boxes[I].Y1 * H / JH) * PxPerSp) + 2);
      Canvas.Pen.Style := psSolid;
    end;
end;

procedure TParadeEdit.ProcessKey(Key: Word; Shift: TShiftState);
var
  Ext: Boolean;
  P, After: pd_pos;
  BI, BJ: pd_block_info;
  Boxes: TParadeShapeBoxes;
  K, J: Integer;
begin
  if TextLocked and FShapeOn and (FShapeSid < 0) and not (ssCtrl in Shift) and (FDrawKind = '') and
     (Length(FDrawPts) = 0) then
    Exit;     { the canvas page itself: not moved off, deleted or typed over }
  if TextLocked and not FShapeOn and (Key in [VK_RETURN, VK_BACK, VK_DELETE, VK_TAB]) then
    Exit;
  if FShapeOn and (ssCtrl in Shift) then
    case Key of     { the shapes' own: the drawing stays selected }
      VK_Z: begin Undo; Exit; end;
      VK_Y: begin Redo; Exit; end;
      VK_C, VK_X, VK_D:
        if FShapeSid >= 0 then
        begin
          case Key of
            VK_C: CopyShapes;
            VK_X: CutShapes;
          else
            DuplicateShapes;
          end;
          Exit;
        end;
      VK_V:
        if CanPasteShapes and CanvasSelected then
        begin
          PasteShapes;
          Exit;
        end;
    end;
  if FShapeOn then
    case Key of
      VK_ESCAPE:    { a shape: back to its drawing; the drawing: back to the text }
        begin
          if FDrawKind <> '' then
          begin   { no shape drawn after all }
            FDrawKind := '';
            SetLength(FDrawPts, 0);
            Cursor := FDrawCursor;
            Invalidate;
            Exit;
          end;
          if FNodeOn then
          begin   { Edit Points done: the shape still selected }
            FNodeOn := False;
            Invalidate;
            Exit;
          end;
          SetLength(FShapeMore, 0);
          if FShapeSid >= 0 then
          begin
            FShapeSid := -1;
            SelectObject(FShapeAt);
          end
          else
          begin
            if FCanvasPage then
              Exit;   { the canvas page: still there to draw in }
            ClearShapeSelection;
            SetCaret(PdPos(FShapeAt.block, FShapeAt.offset + 3), False);
          end;
          Invalidate;
          Exit;
        end;
      VK_RETURN:    { a freeform or a curve: done where it is }
        if Length(FDrawPts) >= 4 then
        begin
          FinishDrawPath(False);
          Exit;
        end
        else
          ClearShapeSelection;
      VK_TAB:       { the next shape of the drawing (Shift: the one before) }
        begin
          Boxes := DrawingShapes(FShapeAt);
          if Length(Boxes) > 0 then
          begin
            K := -1;
            for J := 0 to High(Boxes) do
              if Boxes[J].Sid = FShapeSid then
                K := J;
            if ssShift in Shift then
              K := (K - 1 + Length(Boxes) + Ord(K < 0)) mod Length(Boxes)
            else
              K := (K + 1) mod Length(Boxes);
            FShapeSid := Boxes[K].Sid;
            SetLength(FShapeMore, 0);
            SetCaret(FShapeAt, False);
          end;
          Invalidate;
          Exit;
        end;
      VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN:   { a shape nudged: a point, ten with Shift }
        if FShapeSid >= 0 then
        begin
          K := 1 + 9 * Ord(ssShift in Shift);
          case Key of
            VK_LEFT: NudgeShape(-K, 0);
            VK_RIGHT: NudgeShape(K, 0);
            VK_UP: NudgeShape(0, -K);
          else
            NudgeShape(0, K);
          end;
          Exit;
        end
        else
          ClearShapeSelection;
      VK_DELETE, VK_BACK:
        if FShapeSid >= 0 then
        begin
          if Length(FShapeMore) > 0 then
            DeleteShapes
          else
            DeleteShape;
          Exit;
        end
        else
          ClearShapeSelection;
    else
      ClearShapeSelection;    { anything else is the text's: Delete takes a selected drawing as any text }
    end;
  if (Key = VK_ESCAPE) and (SelectTextBox or LeaveDrawingText) then
    Exit;
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
      if not FReadOnly then
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
      if not FReadOnly then
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
              begin
                DeleteText(PdRange(P, CaretPos), nil);
                if FTrack then      { the text stays, marked: the caret goes before it }
                  pd_doc_marker_set(FDoc, FCaret, P);
              end;
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
              begin
                DeleteText(PdRange(CaretPos, P), nil);
                if FTrack and not HiddenAt(CaretPos) then   { struck through in place: after it }
                  pd_doc_marker_set(FDoc, FCaret, P);
              end;
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
  if (Key = VK_ESCAPE) and FPainter then
  begin
    StopFormatPainter;
    Key := 0;
    Exit;
  end;
  if (Key = VK_ESCAPE) and not FShapeOn and (SelectTextBox or LeaveDrawingText) then
  begin
    Key := 0;
    Exit;
  end;
  if FShapeOn and ((Key in [VK_ESCAPE, VK_TAB]) or ((Key = VK_RETURN) and (Length(FDrawPts) >= 4)) or
     ((FShapeSid >= 0) and (Key in [VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, VK_DELETE, VK_BACK]))) then
  begin
    ProcessKey(Key, Shift);
    Key := 0;
    Exit;
  end;
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
  begin
    if FShapeOn and (FShapeSid >= 0) and (Length(FShapeMore) = 0) then
    begin   { a text box selected: typed at the end of its text; another shape: given text, typed in }
      if not EnterTextBox(FShapeSid, CaretPos, False, True) and not AddShapeText then
      begin
        UTF8Key := '';
        Exit;
      end;
    end
    else if (FShapeOn and (FShapeSid >= 0)) or TextLocked then
    begin
      UTF8Key := '';
      Exit;
    end;
    InsertText(UTF8Key);
  end;
  UTF8Key := '';
end;

{ ---------------- mouse ---------------- }

{ ---------------- drawings: selecting them and their shapes ---------------- }

{ the description of the drawing whose object is at P, parsed; nil when it is no drawing }
function DrawingJson(Doc: Ppd_doc; const P: pd_pos): TJSONObject;
var
  O: pd_inline;
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
  S: RawByteString;
  J: TJSONData;
begin
  Result := nil;
  if (P.block = 0) or (pd_doc_inline_at(Doc, P, O) <> PD_OK) or (O.kind <> PD_INLINE_IMAGE) or
     (pd_doc_resource(Doc, O.resource, @Mime, @Data, @Len) <> PD_OK) or
     (StrComp(Mime, 'application/vnd.parade.drawing+json') <> 0) then
    Exit;
  SetString(S, PAnsiChar(Data), Len);
  try
    J := GetJSON(S);
  except
    Exit;
  end;
  if J is TJSONObject then
    Result := TJSONObject(J)
  else
    J.Free;
end;

{ the stories of a drawing's text boxes, from its description }
function ResStories(Doc: Ppd_doc; R: pd_res_id): TParadeBlockArray;
var
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
  S: RawByteString;
  J: TJSONData;
  Items: TJSONData;
  I: Integer;
begin
  Result := nil;
  if (R = 0) or (pd_doc_resource(Doc, R, @Mime, @Data, @Len) <> PD_OK) or
     (StrComp(Mime, 'application/vnd.parade.drawing+json') <> 0) then
    Exit;
  SetString(S, PAnsiChar(Data), Len);
  if Pos('"story"', S) = 0 then
    Exit;
  try
    J := GetJSON(S);
  except
    Exit;
  end;
  try
    if not (J is TJSONObject) then
      Exit;
    Items := TJSONObject(J).Find('items');
    if not (Items is TJSONArray) then
      Exit;
    for I := 0 to Items.Count - 1 do
      if (Items.Items[I] is TJSONObject) and (TJSONObject(Items.Items[I]).Find('story') <> nil) then
      begin
        SetLength(Result, Length(Result) + 1);
        Result[High(Result)] := TJSONObject(Items.Items[I]).Integers['story'];
      end;
  finally
    J.Free;
  end;
end;

function TParadeEdit.DrawingShapes(const P: pd_pos): TParadeShapeBoxes;
var
  J: TJSONObject;
  Items: TJSONArray;
  It: TJSONObject;
  B: TJSONArray;
  I, N: Integer;
begin
  Result := nil;
  J := DrawingJson(FDoc, P);
  if J = nil then
    Exit;
  try
    Items := J.Find('items') as TJSONArray;
    N := 0;
    if Items <> nil then
      for I := 0 to Items.Count - 1 do
        if (Items[I] is TJSONObject) and (TJSONObject(Items[I]).Find('sid') <> nil) then
        begin
          It := TJSONObject(Items[I]);
          B := It.Find('box') as TJSONArray;
          if (B = nil) or (B.Count < 4) then
            Continue;
          SetLength(Result, N + 1);
          Result[N].Sid := It.Integers['sid'];
          Result[N].X0 := B[0].AsFloat;
          Result[N].Y0 := B[1].AsFloat;
          Result[N].X1 := B[2].AsFloat;
          Result[N].Y1 := B[3].AsFloat;
          Result[N].Story := 0;
          Inc(N);
        end
        else if (N > 0) and (Items[I] is TJSONObject) and (TJSONObject(Items[I]).Find('story') <> nil) and
          (Result[N - 1].Story = 0) then
          { a text box's text: after its shape's box, the shape's }
          Result[N - 1].Story := TJSONObject(Items[I]).Get('story', Int64(0));
  finally
    J.Free;
  end;
end;

{ where the drawing at P is on its page (sp), and the size its description draws it at (sp) }
function TParadeEdit.DrawingPlace(const P: pd_pos; out Page: Int32; out X, Y, W, H: Double; out JW, JH: Double): Boolean;
var
  O: pd_inline;
  J: TJSONObject;
  CX, Base, Asc, Desc, IW, IH: pd_sp;
begin
  Result := False;
  Page := 0; X := 0; Y := 0; W := 0; H := 0; JW := 0; JH := 0;
  if (P.block = 0) or (pd_doc_inline_at(FDoc, P, O) <> PD_OK) or (O.kind <> PD_INLINE_IMAGE) or
     (pd_layout_caret(FLayout, P, Page, CX, Base, Asc, Desc) <> PD_OK) then
    Exit;
  pd_doc_image_display_size(FDoc, O, IW, IH);
  X := CX;
  Y := Base - IH;     { on its line's baseline }
  W := IW;
  H := IH;
  J := DrawingJson(FDoc, P);
  if J <> nil then
    try
      JW := J.Get('w', 0.0);
      JH := J.Get('h', 0.0);
    finally
      J.Free;
    end
  else
  begin   { a picture: its own units its size }
    JW := W;
    JH := H;
  end;
  Result := (W > 0) and (H > 0);
end;

{ the drawing on the page under a point (sp): its object's place }
function TParadeEdit.DrawingAt(Page: Integer; SX, SY: pd_sp; out P: pd_pos): Boolean;
var
  Items: array of pd_draw;
  N, I: Int32;
  Q: pd_pos;
  Pg: Int32;
  X, Y, W, H, JW, JH: Double;
  Seen: string;
  Key: string;
  BX, BY, BW, BH: Double;
begin
  Result := False;
  BX := 0; BY := 0; BW := 0; BH := 0;
  P := PdPos(0, 0);
  if pd_layout_page_items(FLayout, Page, nil, 0, N) <> PD_OK then
    Exit;
  SetLength(Items, N + 1);
  pd_layout_page_items(FLayout, Page, @Items[0], N, N);
  Seen := '';
  for I := 0 to N - 1 do
  begin
    if Items[I].block = 0 then
      Continue;
    Key := '|' + IntToStr(Items[I].block) + ':' + IntToStr(Items[I].offset) + '|';
    if Pos(Key, Seen) > 0 then
      Continue;
    Seen := Seen + Key;
    Q := PdPos(Items[I].block, Items[I].offset);
    if DrawingPlace(Q, Pg, X, Y, W, H, JW, JH) and (Pg = Page) and (SX >= X) and (SX <= X + W) and
       (SY >= Y) and (SY <= Y + H) and (not Result or not ((X <= BX) and (Y <= BY) and (X + W >= BX + BW) and
       (Y + H >= BY + BH))) then
    begin   { the one drawn last over it, unless it holds the one before: a picture in a drawing's text box }
      P := Q;
      BX := X; BY := Y; BW := W; BH := H;
      Result := True;
    end;
  end;
  if Result or (pd_layout_hit_test(FLayout, Page, SX, SY, Q) <> PD_OK) then
    Exit;
  { a drawing that draws nothing (a canvas just put in) is no item of the page: the object the point is at, on
    either side of it }
  for I := 0 to 1 do
  begin
    if (I = 1) and (Q.offset < 3) then
      Break;
    if I = 1 then
      Q.offset := Q.offset - 3;
    if DrawingPlace(Q, Pg, X, Y, W, H, JW, JH) and (Pg = Page) and (SX >= X) and (SX <= X + W) and
       (SY >= Y) and (SY <= Y + H) then
    begin
      P := Q;
      Exit(True);
    end;
  end;
end;

{ the shape of the drawing at P under a point of the page (sp): the smallest box holding it; -1 for none }
function TParadeEdit.PickShape(const P: pd_pos; Page: Integer; SX, SY: pd_sp): Integer;
var
  Boxes: TParadeShapeBoxes;
  Pg: Int32;
  X, Y, W, H, JW, JH, LX, LY, Best, A: Double;
  I: Integer;
begin
  Result := -1;
  if not DrawingPlace(P, Pg, X, Y, W, H, JW, JH) or (JW <= 0) or (JH <= 0) then
    Exit;
  LX := (SX - X) * JW / W;      { into the drawing's own units }
  LY := (SY - Y) * JH / H;
  Boxes := DrawingShapes(P);
  Best := 1e300;
  for I := 0 to High(Boxes) do
    with Boxes[I] do
      if (LX >= X0) and (LX <= X1) and (LY >= Y0) and (LY <= Y1) then
      begin
        A := (X1 - X0) * (Y1 - Y0);
        if A < Best then
        begin
          Best := A;
          Result := Sid;
        end;
      end;
end;

{ A click on a page (sp): a drawing selected -- the whole of it first, a shape of it when it is selected
  already -- unless the click is in its text (a caption: the caret goes there). True when it was one. }
function TParadeEdit.ClickPage(Page: Integer; SX, SY: pd_sp; Shift: TShiftState): Boolean;
var
  D, HP: pd_pos;
  Info: pd_block_info;
  StoryTop: pd_block_id;
  CPage: Int32;
  CX, Base, Asc, Desc: pd_sp;
  Sid: Integer;
  InStory: Boolean;
begin
  Result := False;
  if not DrawingAt(Page, SX, SY, D) then
  begin
    if FShapeOn then
      ClearShapeSelection;
    Exit;
  end;
  { on a text box's edge: that text box, at once -- to move, size or delete; inside it, its text }
  Sid := TextBoxEdgeAt(D, SX, SY);
  if (Sid >= 0) and not (ssShift in Shift) then
  begin
    SetLength(FShapeMore, 0);
    FShapeOn := True;
    FShapeAt := D;
    FShapeSid := Sid;
    SetCaret(D, False);
    Invalidate;
    Exit(True);
  end;
  { on a line of its text: the text's, as any text is clicked -- unless what is clicked is in that text itself
    (a picture in a text box) }
  StoryTop := D.block;
  while (pd_doc_block_info(FDoc, StoryTop, Info) = PD_OK) and (Info.parent <> 0) do
    StoryTop := Info.parent;
  InStory := (pd_doc_block_info(FDoc, StoryTop, Info) = PD_OK) and (Info.kind = PD_BLOCK_STORY);
  if not InStory and (pd_layout_hit_test(FLayout, Page, SX, SY, HP) = PD_OK) then
  begin
    StoryTop := HP.block;
    while (pd_doc_block_info(FDoc, StoryTop, Info) = PD_OK) and (Info.parent <> 0) do
      StoryTop := Info.parent;
    if (pd_doc_block_info(FDoc, StoryTop, Info) = PD_OK) and (Info.kind = PD_BLOCK_STORY) and
       (pd_layout_caret(FLayout, HP, CPage, CX, Base, Asc, Desc) = PD_OK) and (CPage = Page) and
       (SY >= Base - Asc) and (SY <= Base + Desc) then
    begin
      if FShapeOn then
        ClearShapeSelection;
      Exit;
    end;
  end;
  if FShapeOn and (FShapeAt.block = D.block) and (FShapeAt.offset = D.offset) then
    Sid := PickShape(D, Page, SX, SY)
  else
    Sid := -1;
  if (ssShift in Shift) and FShapeOn and (FShapeSid >= 0) and (Sid >= 0) and (FShapeAt.block = D.block) and
     (FShapeAt.offset = D.offset) then
    Exit(ToggleShape(Sid));    { one more shape of the drawing, for a group }
  if (Sid <> FShapeSid) or (FShapeAt.block <> D.block) or (FShapeAt.offset <> D.offset) then
    FNodeOn := False;
  SetLength(FShapeMore, 0);
  FShapeOn := True;
  FShapeAt := D;
  FShapeSid := Sid;
  if Sid < 0 then   { the whole drawing: its object selected, so delete, cut and copy take it }
    SelectObject(D)
  else
    SetCaret(D, False);
  Invalidate;
  Result := True;
end;

function TParadeEdit.SelectedShape(out At: pd_pos; out Sid: Integer): Boolean;
begin
  Result := FShapeOn;
  At := FShapeAt;
  Sid := FShapeSid;
end;

procedure TParadeEdit.ClearShapeSelection;
begin
  if FDrawKind <> '' then
  begin
    FDrawKind := '';
    Cursor := FDrawCursor;
  end;
  FNodeOn := False;
  if not FShapeOn then
    Exit;
  FShapeOn := False;
  FShapeSid := -1;
  SetLength(FShapeMore, 0);
  Invalidate;
end;

function TParadeEdit.ToggleShape(Sid: Integer): Boolean;
var
  I, K: Integer;
begin
  Result := FShapeOn and (Sid >= 0);
  if not Result then
    Exit;
  if FShapeSid < 0 then
    FShapeSid := Sid
  else if Sid <> FShapeSid then
  begin
    K := -1;
    for I := 0 to High(FShapeMore) do
      if FShapeMore[I] = Sid then
        K := I;
    if K >= 0 then
    begin
      FShapeMore[K] := FShapeMore[High(FShapeMore)];
      SetLength(FShapeMore, Length(FShapeMore) - 1);
    end
    else
    begin
      SetLength(FShapeMore, Length(FShapeMore) + 1);
      FShapeMore[High(FShapeMore)] := Sid;
    end;
  end;
  SetCaret(FShapeAt, False);
  Invalidate;
end;

{ the selection's box and handles, over the drawn page (not into it: the view's pixels stay as they are) }
procedure TParadeEdit.PaintShapeSelection;
var
  M: TParadeDrawMap;
  CX1, CY1, CX2, CY2: Double;
  P0: TPoint;
  Pg: Int32;
  X, Y, W, H, JW, JH, BX0, BY0, BX1, BY1: Double;
  Boxes: TParadeShapeBoxes;
  I, L, T, R, B, HX, HY, K: Integer;
  Rc: TRect;
begin
  if not FShapeOn then
    Exit;
  if not DrawingPlace(FShapeAt, Pg, X, Y, W, H, JW, JH) then
  begin
    FShapeOn := False;    { gone: deleted, or no longer a drawing }
    Exit;
  end;
  BX0 := X; BY0 := Y; BX1 := X + W; BY1 := Y + H;
  if (FShapeSid >= 0) and (JW > 0) and (JH > 0) then
  begin
    Boxes := DrawingShapes(FShapeAt);
    for I := 0 to High(Boxes) do
      if Boxes[I].Sid = FShapeSid then
      begin
        BX0 := X + Boxes[I].X0 * W / JW;
        BY0 := Y + Boxes[I].Y0 * H / JH;
        BX1 := X + Boxes[I].X1 * W / JW;
        BY1 := Y + Boxes[I].Y1 * H / JH;
      end;
  end;
  if (Length(FShapeMore) > 0) and (JW > 0) and (JH > 0) then
  begin   { the shapes added to it: a box each, without handles }
    Canvas.Brush.Style := bsClear;
    Canvas.Pen.Color := $00D77800;
    Canvas.Pen.Width := 1;
    Canvas.Pen.Style := psDot;
    for I := 0 to High(Boxes) do
      for K := 0 to High(FShapeMore) do
        if Boxes[I].Sid = FShapeMore[K] then
          Canvas.Rectangle(PageLeft(Pg) + Round((X + Boxes[I].X0 * W / JW) * PxPerSp),
            PageTop(Pg) + Round((Y + Boxes[I].Y0 * H / JH) * PxPerSp),
            PageLeft(Pg) + Round((X + Boxes[I].X1 * W / JW) * PxPerSp) + 1,
            PageTop(Pg) + Round((Y + Boxes[I].Y1 * H / JH) * PxPerSp) + 1);
  end;
  if FNodeOn then
  begin   { Edit Points: the points, not the box }
    PaintShapeExtras;
    Exit;
  end;
  L := PageLeft(Pg) + Round(BX0 * PxPerSp);
  T := PageTop(Pg) + Round(BY0 * PxPerSp);
  R := PageLeft(Pg) + Round(BX1 * PxPerSp);
  B := PageTop(Pg) + Round(BY1 * PxPerSp);
  Canvas.Brush.Style := bsClear;
  Canvas.Pen.Color := $00D77800;
  Canvas.Pen.Width := 1;
  Canvas.Pen.Style := psSolid;
  if not (FCanvasPage and (FShapeSid < 0)) then     { (the canvas page's own canvas: no box, no handles) }
    Canvas.Rectangle(L, T, R + 1, B + 1);
  Canvas.Brush.Style := bsSolid;
  Canvas.Brush.Color := clWhite;
  K := 3;
  for I := 0 to 7 * Ord(not (FCanvasPage and (FShapeSid < 0))) - Ord(FCanvasPage and (FShapeSid < 0)) do
  begin
    case I of
      0: begin HX := L; HY := T; end;
      1: begin HX := (L + R) div 2; HY := T; end;
      2: begin HX := R; HY := T; end;
      3: begin HX := R; HY := (T + B) div 2; end;
      4: begin HX := R; HY := B; end;
      5: begin HX := (L + R) div 2; HY := B; end;
      6: begin HX := L; HY := B; end;
    else
      begin HX := L; HY := (T + B) div 2; end;
    end;
    Canvas.Rectangle(HX - K, HY - K, HX + K + 1, HY + K + 1);
  end;
  PaintShapeExtras;
  PaintDrawPath;
  if (FShapeDrag in [0..9]) and not ((FShapeDrag = 9) and (FDrawKind = 'scribble')) and ShapeClientRect(FShapeNew, Rc) then
  begin   { where the drag would put it }
    Canvas.Brush.Style := bsClear;
    Canvas.Pen.Style := psDash;
    Canvas.Rectangle(Min(Rc.Left, Rc.Right), Min(Rc.Top, Rc.Bottom), Max(Rc.Left, Rc.Right) + 1,
      Max(Rc.Top, Rc.Bottom) + 1);
    if FShapeDrag = 8 then
      for I := 0 to High(Boxes) do    { and the others selected with it }
        for K := 0 to High(FShapeMore) do
          if (Boxes[I].Sid = FShapeMore[K]) and ShapeClientRect([Boxes[I].X0 + FShapeNew[0] - FShapeOld[0],
             Boxes[I].Y0 + FShapeNew[1] - FShapeOld[1], Boxes[I].X1 + FShapeNew[0] - FShapeOld[0],
             Boxes[I].Y1 + FShapeNew[1] - FShapeOld[1]], Rc) then
            Canvas.Rectangle(Rc.Left, Rc.Top, Rc.Right + 1, Rc.Bottom + 1);
    Canvas.Pen.Style := psSolid;
  end;
  if (FShapeDrag in [0..9]) and (DrawingPlace(FShapeAt, Pg, X, Y, W, H, JW, JH)) and (JW > 0) and (JH > 0) then
  begin   { the lines a drag has snapped to, across the canvas }
    Canvas.Pen.Color := $004080FF;
    Canvas.Pen.Style := psDot;
    for I := 0 to High(FGuideX) do
      if ShapeClientRect([FGuideX[I], 0, FGuideX[I], JH], Rc) then
        Canvas.Line(Rc.Left, Rc.Top, Rc.Left, Rc.Bottom);
    for I := 0 to High(FGuideY) do
      if ShapeClientRect([0, FGuideY[I], JW, FGuideY[I]], Rc) then
        Canvas.Line(Rc.Left, Rc.Top, Rc.Right, Rc.Top);
    Canvas.Pen.Style := psSolid;
  end;
  if FShapeDrag = 15 then
  begin   { a connector's end dragged: from its other end to the mouse }
    if DrawMap(M) and ConnectorInfo(CX1, CY1, CX2, CY2, I, K) then
    begin
      Canvas.Pen.Color := $00D77800;
      Canvas.Pen.Style := psDash;
      if FDragIdx = 1 then
        P0 := MapToClient(M, CX1, CY1)
      else
        P0 := MapToClient(M, CX2, CY2);
      Canvas.Line(P0.X, P0.Y, FBandTo.X, FBandTo.Y);
      Canvas.Pen.Style := psSolid;
    end;
    PaintSites;
  end;
  if FShapeDrag = 14 then
  begin   { the drawing where it would be dropped }
    Canvas.Brush.Style := bsClear;
    Canvas.Pen.Color := $00D77800;
    Canvas.Pen.Style := psDash;
    Canvas.Rectangle(L + FBandTo.X - FShapeFrom.X, T + FBandTo.Y - FShapeFrom.Y, R + FBandTo.X - FShapeFrom.X + 1,
      B + FBandTo.Y - FShapeFrom.Y + 1);
    Canvas.Pen.Style := psSolid;
  end;
  if FShapeDrag = 13 then
  begin   { the rubber band }
    Canvas.Brush.Style := bsClear;
    Canvas.Pen.Color := $00D77800;
    Canvas.Pen.Style := psDot;
    Canvas.Rectangle(Min(FShapeFrom.X, FBandTo.X), Min(FShapeFrom.Y, FBandTo.Y), Max(FShapeFrom.X, FBandTo.X) + 1,
      Max(FShapeFrom.Y, FBandTo.Y) + 1);
    Canvas.Pen.Style := psSolid;
  end;
end;

type
  { an element of kept drawing XML: where it starts (its '<') and ends (after its closing tag), its name, its parent }
  TXmlEl = record
    A, B: Integer;
    Name: string;
    Parent: Integer;
  end;
  TXmlEls = array of TXmlEl;

{ every element of an XML fragment, in document order }
function XmlElements(const Xml: string): TXmlEls;
var
  I, J, N, Cur: Integer;
  Name: string;
  Stack: array of Integer;
  Depth: Integer;
begin
  Result := nil;
  N := 0;
  Depth := 0;
  SetLength(Stack, 64);
  I := 1;
  while I <= Length(Xml) do
  begin
    if Xml[I] <> '<' then
    begin
      Inc(I);
      Continue;
    end;
    if (I < Length(Xml)) and (Xml[I + 1] in ['?', '!']) then
    begin
      J := PosEx('>', Xml, I);
      if J = 0 then Break;
      I := J + 1;
      Continue;
    end;
    J := I + 1;
    while (J <= Length(Xml)) and (Xml[J] <> '>') do   { the tag's end, past quoted values }
    begin
      if Xml[J] = '"' then
      begin
        Inc(J);
        while (J <= Length(Xml)) and (Xml[J] <> '"') do
          Inc(J);
      end;
      Inc(J);
    end;
    if J > Length(Xml) then
      Break;
    if Xml[I + 1] = '/' then
    begin   { a closing tag: the element open longest ago that is still open ends here }
      if Depth > 0 then
      begin
        Dec(Depth);
        Result[Stack[Depth]].B := J + 1;
      end;
    end
    else
    begin
      Cur := I + 1;
      while (Cur <= J) and not (Xml[Cur] in [' ', '>', '/', #9, #10, #13]) do
        Inc(Cur);
      Name := Copy(Xml, I + 1, Cur - I - 1);
      SetLength(Result, N + 1);
      Result[N].A := I;
      Result[N].B := J + 1;
      Result[N].Name := Name;
      Result[N].Parent := -1;
      if Depth > 0 then
        Result[N].Parent := Stack[Depth - 1];
      if Xml[J - 1] <> '/' then     { not empty: open until its closing tag }
      begin
        if Depth >= Length(Stack) then
          SetLength(Stack, Depth * 2);
        Stack[Depth] := N;
        Inc(Depth);
      end;
      Inc(N);
    end;
    I := J + 1;
  end;
end;

{ a shape of a drawing (what a selection or the order is about): a shape, a picture, a group }
function IsShapeEl(const Name: string): Boolean;
begin
  Result := (Name = 'wps:wsp') or (Name = 'pic:pic') or (Name = 'wpg:grpSp') or (Name = 'wpg:wgp') or (Name = 'wpg:graphicFrame');
end;

{ the element of the n-th wps:wsp / pic:pic (a sid); -1 if none }
function SidElement(const Els: TXmlEls; Sid: Integer): Integer;
var
  I, K: Integer;
begin
  Result := -1;
  K := -1;
  for I := 0 to High(Els) do
    if (Els[I].Name = 'wps:wsp') or (Els[I].Name = 'pic:pic') then
    begin
      Inc(K);
      if K = Sid then
        Exit(I);
    end;
end;

{ the sid of the first wps:wsp / pic:pic at or after a place of the XML }
function SidAt(const Els: TXmlEls; At: Integer): Integer;
var
  I, K: Integer;
begin
  Result := -1;
  K := -1;
  for I := 0 to High(Els) do
    if (Els[I].Name = 'wps:wsp') or (Els[I].Name = 'pic:pic') then
    begin
      Inc(K);
      if Els[I].A >= At then
        Exit(K);
    end;
end;

{ the n-th shape element (wps:wsp or pic:pic) of kept drawing XML: where it starts and where it ends }
function KeptShapeSpan(const Xml: string; N: Integer; out A, B: Integer): Boolean;
var
  I, K, Depth: Integer;
  Tag: string;

  function IsTag(At: Integer; const T: string): Boolean;
  begin
    Result := (Copy(Xml, At, Length(T)) = T) and (At + Length(T) <= Length(Xml)) and
      (Xml[At + Length(T)] in ['>', ' ', '/', #9, #10, #13]);
  end;

begin
  Result := False;
  A := 0;
  B := 0;
  K := -1;
  I := 1;
  while I <= Length(Xml) do
  begin
    if (Xml[I] = '<') and (IsTag(I, '<wps:wsp') or IsTag(I, '<pic:pic')) then
    begin
      Inc(K);
      if K = N then
      begin
        Tag := Copy(Xml, I + 1, 7);     { wps:wsp or pic:pic }
        A := I;
        Depth := 0;
        while I <= Length(Xml) do
        begin
          if IsTag(I, '<' + Tag) then
            Inc(Depth)
          else if Copy(Xml, I, Length(Tag) + 3) = '</' + Tag + '>' then
          begin
            Dec(Depth);
            if Depth = 0 then
            begin
              B := I + Length(Tag) + 3;
              Exit(True);
            end;
          end;
          Inc(I);
        end;
        Exit;
      end;
    end;
    Inc(I);
  end;
end;

{ an attribute's value in a tag starting at At (to its '>'): its place and length; 0 when it has none }
function AttrSpan(const Xml: string; At: Integer; const Name: string; out VLen: Integer): Integer;
var
  E, P: Integer;
begin
  Result := 0;
  VLen := 0;
  E := At;
  while (E <= Length(Xml)) and (Xml[E] <> '>') do
    Inc(E);
  P := At;
  while P < E do
  begin
    if (Copy(Xml, P, Length(Name) + 3) = ' ' + Name + '="') then
    begin
      Result := P + Length(Name) + 3;
      while (Result + VLen <= E) and (Xml[Result + VLen] <> '"') do
        Inc(VLen);
      Exit;
    end;
    Inc(P);
  end;
end;

{ the kept XML's shape N moved by DX, DY and sized by KX, KY, in its own units (a:off, a:ext of its xfrm) }
procedure PatchKeptXml(var Xml: string; N: Integer; DX, DY, KX, KY: Double);
var
  A, B, O, E, P, L: Integer;
  V: Int64;

  procedure Put(At, Len: Integer; Value: Int64);
  begin
    Delete(Xml, At, Len);
    Insert(IntToStr(Value), Xml, At);
  end;

begin
  if not KeptShapeSpan(Xml, N, A, B) then
    Exit;
  O := Pos('<a:off ', Copy(Xml, A, B - A));
  if O = 0 then
    Exit;
  O := A + O - 1;
  E := Pos('<a:ext ', Copy(Xml, O, B - O));
  if E > 0 then
    E := O + E - 1;
  { the extent first: its place is after the offset's, and the offset's text may change length }
  if E > 0 then
  begin
    P := AttrSpan(Xml, E, 'cy', L);
    if P > 0 then
    begin
      V := StrToInt64Def(Copy(Xml, P, L), 0);
      Put(P, L, Round(V * KY));
    end;
    P := AttrSpan(Xml, E, 'cx', L);
    if P > 0 then
    begin
      V := StrToInt64Def(Copy(Xml, P, L), 0);
      Put(P, L, Round(V * KX));
    end;
  end;
  P := AttrSpan(Xml, O, 'y', L);
  if P > 0 then
  begin
    V := StrToInt64Def(Copy(Xml, P, L), 0);
    Put(P, L, V + Round(DY));
  end;
  P := AttrSpan(Xml, O, 'x', L);
  if P > 0 then
  begin
    V := StrToInt64Def(Copy(Xml, P, L), 0);
    Put(P, L, V + Round(DX));
  end;
end;

function TParadeEdit.ShapeBox(Sid: Integer; out X0, Y0, X1, Y1: Double): Boolean;
var
  Boxes: TParadeShapeBoxes;
  I: Integer;
begin
  Result := False;
  X0 := 0; Y0 := 0; X1 := 0; Y1 := 0;
  if not FShapeOn then
    Exit;
  Boxes := DrawingShapes(FShapeAt);
  for I := 0 to High(Boxes) do
    if Boxes[I].Sid = Sid then
    begin
      X0 := Boxes[I].X0; Y0 := Boxes[I].Y0; X1 := Boxes[I].X1; Y1 := Boxes[I].Y1;
      Exit(True);
    end;
end;

{ the drawing at P given a new description: the old object out and the new in, one step of undo }
{ an object's text (source, title, alt) copied out of the document into Keep, its pointers moved there: for an
  object taken out and put back }
procedure KeepInlineText(var O: pd_inline; out Keep: array of string);
begin
  SetString(Keep[0], O.source, O.source_len);
  SetString(Keep[1], O.title, O.title_len);
  SetString(Keep[2], O.alt, O.alt_len);
  O.source := PAnsiChar(Keep[0]);
  O.title := PAnsiChar(Keep[1]);
  O.alt := PAnsiChar(Keep[2]);
end;

function TParadeEdit.ReplaceDrawing(const P: pd_pos; const Json: string; const Lbl: string): Boolean;
var
  R: pd_res_id;
begin
  Result := not FReadOnly and
    (pd_doc_add_resource(FDoc, 'application/vnd.parade.drawing+json', PAnsiChar(Json), Length(Json), R) = PD_OK) and
    ReplaceDrawingRes(P, R, Lbl);
end;

{ the drawing at P with another description (a resource of the document): its object swapped, one step of undo }
function TParadeEdit.ReplaceDrawingRes(const P: pd_pos; R: pd_res_id; const Lbl: string): Boolean;
var
  O: pd_inline;
  Keep: array[0..2] of string;
  Old: TParadeBlockArray;
begin
  Result := False;
  if FReadOnly or (R = 0) or (pd_doc_inline_at(FDoc, P, O) <> PD_OK) then
    Exit;
  KeepInlineText(O, Keep);
  PushShapeStep(Lbl, P);
  Old := ResStories(FDoc, O.resource);
  O.resource := R;
  pd_doc_begin_group(FDoc, PAnsiChar(Lbl));
  pd_doc_delete(FDoc, PdRange(P, PdPos(P.block, P.offset + 3)), nil);
  Result := pd_doc_insert_inline(FDoc, P, O, nil) = PD_OK;
  if Result then    { a text box gone: its story too }
    DropStories(Old, ResStories(FDoc, R));
  pd_doc_end_group(FDoc);
  SetCaret(P, False);
  Changed;
end;

function TParadeEdit.StoriesInRange(const A, B: pd_pos): TParadeBlockArray;
var
  Blk: pd_block_id;
  T: string;
  I, K, Lo, Hi: Integer;
  O: pd_inline;
  S: TParadeBlockArray;
begin
  Result := nil;
  Blk := A.block;
  K := 0;
  while (Blk <> 0) and (K < 100000) do
  begin
    T := ParaText(Blk);
    Lo := 0;
    Hi := Length(T);
    if Blk = A.block then
      Lo := A.offset;
    if Blk = B.block then
      Hi := B.offset;
    I := PosEx(#$EF#$BF#$BC, T, Lo + 1);
    while (I > 0) and (I - 1 < Hi) do
    begin
      if (pd_doc_inline_at(FDoc, PdPos(Blk, I - 1), O) = PD_OK) and (O.kind = PD_INLINE_IMAGE) then
      begin
        S := ResStories(FDoc, O.resource);
        for K := 0 to High(S) do
        begin
          SetLength(Result, Length(Result) + 1);
          Result[High(Result)] := S[K];
        end;
      end;
      I := PosEx(#$EF#$BF#$BC, T, I + 3);
    end;
    if Blk = B.block then
      Break;
    Blk := pd_doc_next_paragraph(FDoc, Blk);
    Inc(K);
  end;
end;

procedure TParadeEdit.DropStories(const S, Keep: TParadeBlockArray);
var
  I, K: Integer;
  Kept: Boolean;
  Info: pd_block_info;
begin
  for I := 0 to High(S) do
  begin
    Kept := False;
    for K := 0 to High(Keep) do
      Kept := Kept or (Keep[K] = S[I]);
    if not Kept and (pd_doc_block_info(FDoc, S[I], Info) = PD_OK) and (Info.kind = PD_BLOCK_STORY) then
      pd_doc_remove_block(FDoc, S[I]);
  end;
end;

function TParadeEdit.DeleteText(const R: pd_range; After: Ppd_pos): pd_status;
var
  S: TParadeBlockArray;
  P: pd_pos;
begin
  S := nil;
  if not FTrack then
    S := StoriesInRange(R.start, R.finish);
  if Length(S) = 0 then
    Exit(pd_doc_delete(FDoc, R, After));
  pd_doc_begin_group(FDoc, 'Delete');
  Result := pd_doc_delete(FDoc, R, @P);
  if Result = PD_OK then
    DropStories(S, nil);
  pd_doc_end_group(FDoc);
  if After <> nil then
    After^ := P;
end;

{ a shape edit about to be made (labelled as the history will have it), from the drawing at At: the selection now,
  for undo to go back to. Not for the edits a larger one is made of (FStepDepth) }
procedure TParadeEdit.PushShapeStep(const Lbl: string; const At: pd_pos);
var
  S: TParadeShapeStep;
begin
  if FStepDepth > 0 then
    Exit;
  S.Lbl := Lbl;
  S.On := FShapeOn;
  S.At := At;
  S.Sid := FShapeSid;
  S.More := Copy(FShapeMore);
  S.Valid := True;
  if not FShapeOn then
    S.Sid := -1;
  if Length(FSelUndo) >= 256 then   { as far back as anyone undoes }
    Delete(FSelUndo, 0, 64);
  SetLength(FSelUndo, Length(FSelUndo) + 1);
  FSelUndo[High(FSelUndo)] := S;
  SetLength(FSelRedo, 0);
end;

{ the selection a shape edit was made from (undone) or left (redone), as far as the drawing still has it }
procedure TParadeEdit.RestoreShapeStep(const S: TParadeShapeStep);
var
  O: pd_inline;
  Boxes: TParadeShapeBoxes;
  I, K: Integer;
  Found: Boolean;
begin
  if not S.On then
  begin
    ClearShapeSelection;
    Exit;
  end;
  if (pd_doc_inline_at(FDoc, S.At, O) <> PD_OK) or (O.kind <> PD_INLINE_IMAGE) then
  begin   { a picture or a drawing no longer there }
    ClearShapeSelection;
    Exit;
  end;
  FShapeOn := True;
  FShapeAt := S.At;
  FShapeSid := -1;
  SetLength(FShapeMore, 0);
  FNodeOn := False;
  if S.Sid >= 0 then
  begin
    Boxes := DrawingShapes(S.At);
    Found := False;
    for I := 0 to High(Boxes) do
      Found := Found or (Boxes[I].Sid = S.Sid);
    if Found then
    begin
      FShapeSid := S.Sid;
      for K := 0 to High(S.More) do
        for I := 0 to High(Boxes) do
          if Boxes[I].Sid = S.More[K] then
          begin
            SetLength(FShapeMore, Length(FShapeMore) + 1);
            FShapeMore[High(FShapeMore)] := S.More[K];
          end;
    end;
  end;
  if FShapeSid < 0 then
    SelectObject(S.At)
  else
    SetCaret(S.At, False);
  Invalidate;
end;

{ ---------------- shapes edited in the XML kept for Word ---------------- }

{ each text box's content marked with whose story it is (its place among the drawing's), so that a text box moved
  in the XML keeps its text when the drawing is made again }
function MarkTextBoxes(const Xml: string): string;
var
  I, J, K: Integer;
begin
  Result := '';
  I := 1;
  K := 0;
  repeat
    J := PosEx('<w:txbxContent', Xml, I);
    if (J = 0) or (J + 14 > Length(Xml)) or not (Xml[J + 14] in ['>', ' ']) then
    begin
      if J > 0 then
      begin   { another element whose name begins so }
        Result := Result + Copy(Xml, I, J + 14 - I);
        I := J + 14;
        Continue;
      end;
      Break;
    end;
    J := PosEx('>', Xml, J);
    if J = 0 then
      Break;
    Result := Result + Copy(Xml, I, J + 1 - I) + '<!--pd-story:' + IntToStr(K) + '-->';
    Inc(K);
    I := J + 1;
  until False;
  Result := Result + Copy(Xml, I, MaxInt);
end;

function TParadeEdit.KeptXml(out Xml: string): Boolean;
var
  J: TJSONObject;
begin
  Result := False;
  Xml := '';
  if not FShapeOn then
    Exit;
  J := DrawingJson(FDoc, FShapeAt);
  if J = nil then
    Exit;
  try
    if (J.Find('xml') <> nil) and (J.Find('xml').JSONType = jtString) then
    begin
      Xml := MarkTextBoxes(J.Strings['xml']);
      Result := Xml <> '';
    end;
  finally
    J.Free;
  end;
end;

function RouteConnectors(var Xml: string): Boolean; forward;

{ the selected drawing made again from edited XML: its object swapped, the shape NewSid selected (-1: the drawing) }
function TParadeEdit.ApplyKeptXml(const Xml: string; const Lbl: string; NewSid: Integer): Boolean;
var
  O: pd_inline;
  R: pd_res_id;
  N: Integer;
  X: string;
begin
  X := Xml;
  RouteConnectors(X);   { the connectors on the shapes they join, wherever those are now }
  Result := False;
  if FReadOnly or not FShapeOn or (pd_doc_inline_at(FDoc, FShapeAt, O) <> PD_OK) then
    Exit;
  N := Length(FSelUndo);
  PushShapeStep(Lbl, FShapeAt);
  Inc(FStepDepth);
  pd_doc_begin_group(FDoc, PAnsiChar(Lbl));     { with the story a new text box is given }
  try
    Result := (pd_docx_drawing_rebuild(FDoc, O.resource, PAnsiChar(X), Length(X), R) = PD_OK) and
      ReplaceDrawingRes(FShapeAt, R, Lbl);
  finally
    pd_doc_end_group(FDoc);
    Dec(FStepDepth);
  end;
  if not Result and (FStepDepth = 0) then
    SetLength(FSelUndo, Min(N, Length(FSelUndo)));    { no step made }
  if Result then
  begin
    FShapeOn := True;
    FShapeSid := NewSid;
    SetLength(FShapeMore, 0);
    Invalidate;
  end;
end;

function HexRGB(C: TColor): string;
var
  RGB: LongInt;
begin
  RGB := ColorToRGB(C);
  Result := IntToHex(Red(RGB), 2) + IntToHex(Green(RGB), 2) + IntToHex(Blue(RGB), 2);
end;

{ the child of element E named N (its index); -1 if none }
function ChildNamed(const Els: TXmlEls; E: Integer; const N: string): Integer;
var
  I: Integer;
begin
  Result := -1;
  for I := E + 1 to High(Els) do
    if (Els[I].Parent = E) and (Els[I].Name = N) then
      Exit(I);
end;

{ the shape's properties element (wps:spPr, pic:spPr, wpg:grpSpPr) }
function PropsOf(const Els: TXmlEls; E: Integer): Integer;
var
  I: Integer;
begin
  Result := -1;
  for I := E + 1 to High(Els) do
    if (Els[I].Parent = E) and ((Els[I].Name = 'wps:spPr') or (Els[I].Name = 'pic:spPr') or
       (Els[I].Name = 'wpg:grpSpPr')) then
      Exit(I);
end;

{ where in element E (not empty) its children end: its closing tag's start }
function CloseOf(const Xml: string; const El: TXmlEl): Integer;
begin
  Result := El.B - 1;
  while (Result > El.A) and (Xml[Result] <> '<') do
    Dec(Result);
end;

{ a fill or a line put into a properties element: its old fill (of the kinds a fill is) replaced, or the new one
  put before what follows a fill (the line, the effects, the 3-D); an empty element opened for it }
function PutIntoProps(const Xml: string; const Els: TXmlEls; Pr: Integer; const Kinds: array of string;
  const Before: array of string; const NewEl: string): string;
var
  I, K, At: Integer;
begin
  if Copy(Xml, Els[Pr].B - 2, 2) = '/>' then     { <wps:spPr/>: opened }
    Exit(Copy(Xml, 1, Els[Pr].A - 1) + '<' + Els[Pr].Name + '>' + NewEl + '</' + Els[Pr].Name + '>' +
      Copy(Xml, Els[Pr].B, MaxInt));
  for I := Pr + 1 to High(Els) do
    if Els[I].Parent = Pr then
      for K := 0 to High(Kinds) do
        if Els[I].Name = Kinds[K] then
          Exit(Copy(Xml, 1, Els[I].A - 1) + NewEl + Copy(Xml, Els[I].B, MaxInt));
  At := CloseOf(Xml, Els[Pr]);
  for I := Pr + 1 to High(Els) do
    if Els[I].Parent = Pr then
      for K := 0 to High(Before) do
        if (Els[I].Name = Before[K]) and (Els[I].A < At) then
          At := Els[I].A;
  Result := Copy(Xml, 1, At - 1) + NewEl + Copy(Xml, At, MaxInt);
end;

{ the whole drawing selected, and it has one shape: that shape is what a shape edit is for }
procedure TParadeEdit.OnlyShape;
var
  Boxes: TParadeShapeBoxes;
begin
  if not FShapeOn or (FShapeSid >= 0) then
    Exit;
  Boxes := DrawingShapes(FShapeAt);
  if Length(Boxes) = 1 then
  begin
    FShapeSid := Boxes[0].Sid;
    SetCaret(FShapeAt, False);
  end;
end;

function TParadeEdit.SetShapeFill(AColor: TColor; None: Boolean): Boolean;
var
  Xml, F: string;
  Els: TXmlEls;
  E, Pr: Integer;
begin
  OnlyShape;
  Result := False;
  if not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  Pr := PropsOf(Els, E);
  if (E < 0) or (Pr < 0) then
    Exit;
  if None then
    F := '<a:noFill/>'
  else
    F := '<a:solidFill><a:srgbClr val="' + HexRGB(AColor) + '"/></a:solidFill>';
  Xml := PutIntoProps(Xml, Els, Pr, ['a:noFill', 'a:solidFill', 'a:gradFill', 'a:pattFill', 'a:blipFill', 'a:grpFill'],
    ['a:ln', 'a:effectLst', 'a:effectDag', 'a:scene3d', 'a:sp3d', 'a:extLst'], F);
  Result := ApplyKeptXml(Xml, 'Fill', FShapeSid);
end;

function TParadeEdit.SetShapeLine(AColor: TColor; WidthPt: Double; None: Boolean): Boolean;
var
  Xml, F, Ln, W: string;
  Els, LEls: TXmlEls;
  E, Pr, L, I, P, Len: Integer;
begin
  OnlyShape;
  Result := False;
  if not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  Pr := PropsOf(Els, E);
  if (E < 0) or (Pr < 0) then
    Exit;
  if None then
    F := '<a:noFill/>'
  else if AColor = clNone then
    F := ''
  else
    F := '<a:solidFill><a:srgbClr val="' + HexRGB(AColor) + '"/></a:solidFill>';
  L := ChildNamed(Els, Pr, 'a:ln');
  if L < 0 then
  begin
    W := '';
    if WidthPt > 0 then
      W := ' w="' + IntToStr(Round(WidthPt * 12700)) + '"';
    Xml := PutIntoProps(Xml, Els, Pr, [], ['a:effectLst', 'a:effectDag', 'a:scene3d', 'a:sp3d', 'a:extLst'],
      '<a:ln' + W + '>' + F + '</a:ln>');
  end
  else
  begin   { the line there: its width, and its fill (what colour it is) }
    Ln := Copy(Xml, Els[L].A, Els[L].B - Els[L].A);
    if Copy(Ln, Length(Ln) - 1, 2) = '/>' then
      Ln := Copy(Ln, 1, Length(Ln) - 2) + '></a:ln>';
    if WidthPt > 0 then
    begin
      P := AttrSpan(Ln, 1, 'w', Len);
      if P > 0 then
      begin
        Delete(Ln, P, Len);
        Insert(IntToStr(Round(WidthPt * 12700)), Ln, P);
      end
      else
        Insert(' w="' + IntToStr(Round(WidthPt * 12700)) + '"', Ln, 6);
    end;
    LEls := XmlElements(Ln);
    I := -1;
    for P := 1 to High(LEls) do
      if (LEls[P].Parent = 0) and ((LEls[P].Name = 'a:noFill') or (LEls[P].Name = 'a:solidFill') or
         (LEls[P].Name = 'a:gradFill') or (LEls[P].Name = 'a:pattFill')) then
      begin
        I := P;
        Break;
      end;
    if F = '' then
    else if I >= 0 then
      Ln := Copy(Ln, 1, LEls[I].A - 1) + F + Copy(Ln, LEls[I].B, MaxInt)
    else
      Ln := Copy(Ln, 1, PosEx('>', Ln, 1)) + F + Copy(Ln, PosEx('>', Ln, 1) + 1, MaxInt);
    Xml := Copy(Xml, 1, Els[L].A - 1) + Ln + Copy(Xml, Els[L].B, MaxInt);
  end;
  Result := ApplyKeptXml(Xml, 'Line', FShapeSid);
end;

{ an a:ln with a part of it replaced (Name: a:prstDash, a:headEnd, a:tailEnd; Part '' takes it out), its parts in
  the order DrawingML has them }
function LnWithPart(const Ln, Name, Part: string): string;
const
  Order: array[0..11] of string = ('a:noFill', 'a:solidFill', 'a:gradFill', 'a:pattFill', 'a:prstDash',
    'a:custDash', 'a:round', 'a:bevel', 'a:miter', 'a:headEnd', 'a:tailEnd', 'a:extLst');
var
  Els: TXmlEls;
  L, Rank, R, I, K: Integer;
  Open: string;
  Kids: array[0..11] of string;
begin
  L := Length(Ln);
  if Copy(Ln, L - 1, 2) = '/>' then
    Exit(LnWithPart(Copy(Ln, 1, L - 2) + '></a:ln>', Name, Part));
  Els := XmlElements(Ln);
  K := PosEx('>', Ln, 1);
  Open := Copy(Ln, 1, K);
  for I := 0 to 11 do
    Kids[I] := '';
  Rank := 0;
  for I := 0 to 11 do
    if Order[I] = Name then
      Rank := I;
  for I := 1 to High(Els) do
    if Els[I].Parent = 0 then
    begin
      R := -1;
      for K := 0 to 11 do
        if Order[K] = Els[I].Name then
          R := K;
      if (R >= 0) and not ((R = Rank) or ((Name = 'a:prstDash') and (R = 5))) then   { a custom dash goes too }
        Kids[R] := Copy(Ln, Els[I].A, Els[I].B - Els[I].A);
    end;
  Kids[Rank] := Part;
  Result := Open;
  for I := 0 to 11 do
    Result := Result + Kids[I];
  Result := Result + '</a:ln>';
end;

function TParadeEdit.SetShapeLineStyle(const Dash, Head, Tail: string): Boolean;
var
  Xml, Ln: string;
  Els: TXmlEls;
  Sids: TIntegerArray;
  I, E, Pr, L: Integer;
begin
  OnlyShape;
  Result := False;
  Sids := SelectedShapes;
  if FReadOnly or (Length(Sids) = 0) or not KeptXml(Xml) then
    Exit;
  for I := 0 to High(Sids) do
  begin   { each in the XML as it is now: the XML before it is no longer where it was }
    Els := XmlElements(Xml);
    E := SidElement(Els, Sids[I]);
    Pr := PropsOf(Els, E);
    if (E < 0) or (Pr < 0) then
      Continue;
    L := ChildNamed(Els, Pr, 'a:ln');
    if L < 0 then
    begin
      Xml := PutIntoProps(Xml, Els, Pr, [], ['a:effectLst', 'a:effectDag', 'a:scene3d', 'a:sp3d', 'a:extLst'],
        '<a:ln></a:ln>');
      Els := XmlElements(Xml);
      E := SidElement(Els, Sids[I]);
      Pr := PropsOf(Els, E);
      L := ChildNamed(Els, Pr, 'a:ln');
      if L < 0 then
        Continue;
    end;
    Ln := Copy(Xml, Els[L].A, Els[L].B - Els[L].A);
    if Dash <> '' then
      Ln := LnWithPart(Ln, 'a:prstDash', IfThen(Dash = 'solid', '', '<a:prstDash val="' + Dash + '"/>'));
    if Head <> '' then
      Ln := LnWithPart(Ln, 'a:headEnd', IfThen(Head = 'none', '', '<a:headEnd type="' + Head + '"/>'));
    if Tail <> '' then
      Ln := LnWithPart(Ln, 'a:tailEnd', IfThen(Tail = 'none', '', '<a:tailEnd type="' + Tail + '"/>'));
    Xml := Copy(Xml, 1, Els[L].A - 1) + Ln + Copy(Xml, Els[L].B, MaxInt);
  end;
  Result := ApplyKeptXml(Xml, 'Line style', Sids[0]);
  if Result then
  begin
    SetLength(FShapeMore, Length(Sids) - 1);
    for I := 1 to High(Sids) do
      FShapeMore[I - 1] := Sids[I];
  end;
end;

function TParadeEdit.ShapeLineStyle(out Dash, Head, Tail: string): Boolean;
var
  Xml, Ln: string;
  Els: TXmlEls;
  E, Pr, L, P, N: Integer;

  function Val(const Tag, Attr: string): string;
  begin
    Result := '';
    P := Pos('<' + Tag + ' ', Ln);
    if P > 0 then
    begin
      P := AttrSpan(Ln, P, Attr, N);
      if P > 0 then
        Result := Copy(Ln, P, N);
    end;
  end;

begin
  Dash := '';
  Head := '';
  Tail := '';
  Result := FShapeOn and (FShapeSid >= 0) and KeptXml(Xml);
  if not Result then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  Pr := PropsOf(Els, E);
  L := -1;
  if Pr >= 0 then
    L := ChildNamed(Els, Pr, 'a:ln');
  if L < 0 then
    Exit;
  Ln := Copy(Xml, Els[L].A, Els[L].B - Els[L].A);
  Dash := Val('a:prstDash', 'val');
  Head := Val('a:headEnd', 'type');
  Tail := Val('a:tailEnd', 'type');
  if Head = 'none' then Head := '';
  if Tail = 'none' then Tail := '';
end;

{ the shape's element among its parent's shapes, moved: 0 one forward, 1 one back, 2 to the front, 3 to the back }
function TParadeEdit.ShapeOrder(Mode: Integer): Boolean;
var
  Xml, Mine, Rest: string;
  Els: TXmlEls;
  E, I, K, N, At: Integer;
  Sibs: array of Integer;
begin
  OnlyShape;
  Result := False;
  if not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  if E < 0 then
    Exit;
  N := 0;
  K := -1;
  for I := 0 to High(Els) do
    if (Els[I].Parent = Els[E].Parent) and IsShapeEl(Els[I].Name) then
    begin
      SetLength(Sibs, N + 1);
      Sibs[N] := I;
      if I = E then
        K := N;
      Inc(N);
    end;
  if (K < 0) or ((Mode in [0, 2]) and (K = N - 1)) or ((Mode in [1, 3]) and (K = 0)) then
    Exit;
  Mine := Copy(Xml, Els[E].A, Els[E].B - Els[E].A);
  Rest := Copy(Xml, 1, Els[E].A - 1) + Copy(Xml, Els[E].B, MaxInt);     { the XML without it }
  case Mode of      { where it goes, in the XML without it }
    0: At := Els[Sibs[K + 1]].B - (Els[E].B - Els[E].A);
    1: At := Els[Sibs[K - 1]].A;
    2: At := Els[Sibs[N - 1]].B - (Els[E].B - Els[E].A);
  else
    At := Els[Sibs[0]].A;
  end;
  Insert(Mine, Rest, At);
  Result := ApplyKeptXml(Rest, 'Order', SidAt(XmlElements(Rest), At));
end;

{ an element's place and size (a:off, a:ext of the a:xfrm of its properties), in its parent's units }
function XfrmOf(const S: string; out X, Y, CX, CY: Int64): Boolean;
var
  O, E, P, L: Integer;
begin
  X := 0; Y := 0; CX := 0; CY := 0;
  O := Pos('<a:off ', S);
  E := Pos('<a:ext ', S);
  Result := (O > 0) and (E > 0);
  if not Result then
    Exit;
  P := AttrSpan(S, O, 'x', L); if P > 0 then X := StrToInt64Def(Copy(S, P, L), 0);
  P := AttrSpan(S, O, 'y', L); if P > 0 then Y := StrToInt64Def(Copy(S, P, L), 0);
  P := AttrSpan(S, E, 'cx', L); if P > 0 then CX := StrToInt64Def(Copy(S, P, L), 0);
  P := AttrSpan(S, E, 'cy', L); if P > 0 then CY := StrToInt64Def(Copy(S, P, L), 0);
end;

{ an element given a place and size (its first a:off, a:ext) }
procedure SetXfrm(var S: string; X, Y, CX, CY: Int64);
var
  O, E, P, L: Integer;
begin
  E := Pos('<a:ext ', S);
  if E > 0 then
  begin
    P := AttrSpan(S, E, 'cy', L); if P > 0 then begin Delete(S, P, L); Insert(IntToStr(CY), S, P); end;
    P := AttrSpan(S, E, 'cx', L); if P > 0 then begin Delete(S, P, L); Insert(IntToStr(CX), S, P); end;
  end;
  O := Pos('<a:off ', S);
  if O > 0 then
  begin
    P := AttrSpan(S, O, 'y', L); if P > 0 then begin Delete(S, P, L); Insert(IntToStr(Y), S, P); end;
    P := AttrSpan(S, O, 'x', L); if P > 0 then begin Delete(S, P, L); Insert(IntToStr(X), S, P); end;
  end;
end;

function TParadeEdit.SelectedShapes: TIntegerArray;
var
  I: Integer;
begin
  Result := nil;
  if not FShapeOn or (FShapeSid < 0) then
    Exit;
  SetLength(Result, 1 + Length(FShapeMore));
  Result[0] := FShapeSid;
  for I := 0 to High(FShapeMore) do
    Result[I + 1] := FShapeMore[I];
end;

{ the selected shapes (siblings: of one parent) made one group, its child space their own }
function TParadeEdit.GroupShapes: Boolean;
var
  Xml, G, GTag: string;
  Els: TXmlEls;
  Ids: array of Integer;
  Sids: TIntegerArray;
  I, J, T, Par: Integer;
  X, Y, CX, CY, X0, Y0, X1, Y1: Int64;
begin
  Result := False;
  Sids := SelectedShapes;
  if (Length(Sids) < 2) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  SetLength(Ids, Length(Sids));
  for I := 0 to High(Sids) do
  begin
    Ids[I] := SidElement(Els, Sids[I]);
    { a shape inside a group is that group's: the group is what is grouped }
    while (Ids[I] >= 0) and (Els[Ids[I]].Parent >= 0) and (Els[Els[Ids[I]].Parent].Name = 'wpg:grpSp') and
          (I > 0) and (Els[Ids[I]].Parent <> Els[Ids[0]].Parent) do
      Ids[I] := Els[Ids[I]].Parent;
    if Ids[I] < 0 then
      Exit;
  end;
  Par := Els[Ids[0]].Parent;
  for I := 1 to High(Ids) do
    if Els[Ids[I]].Parent <> Par then
      Exit;     { not siblings: no one group holds them }
  for I := 0 to High(Ids) do    { in document order }
    for J := I + 1 to High(Ids) do
      if Els[Ids[J]].A < Els[Ids[I]].A then
      begin
        T := Ids[I]; Ids[I] := Ids[J]; Ids[J] := T;
      end;
  X0 := High(Int64); Y0 := High(Int64); X1 := Low(Int64); Y1 := Low(Int64);
  G := '';
  for I := 0 to High(Ids) do
  begin
    if not XfrmOf(Copy(Xml, Els[Ids[I]].A, Els[Ids[I]].B - Els[Ids[I]].A), X, Y, CX, CY) then
      Exit;
    X0 := Min(X0, X); Y0 := Min(Y0, Y); X1 := Max(X1, X + CX); Y1 := Max(Y1, Y + CY);
    G := G + Copy(Xml, Els[Ids[I]].A, Els[Ids[I]].B - Els[Ids[I]].A);
  end;
  if (Par >= 0) and (Els[Par].Name = 'wpc:wpc') then
    GTag := 'wpg:wgp'    { a canvas's groups }
  else
    GTag := 'wpg:grpSp';
  G := '<' + GTag + '><wpg:cNvGrpSpPr/><wpg:grpSpPr><a:xfrm><a:off x="' + IntToStr(X0) + '" y="' + IntToStr(Y0) +
    '"/><a:ext cx="' + IntToStr(X1 - X0) + '" cy="' + IntToStr(Y1 - Y0) + '"/><a:chOff x="' + IntToStr(X0) +
    '" y="' + IntToStr(Y0) + '"/><a:chExt cx="' + IntToStr(X1 - X0) + '" cy="' + IntToStr(Y1 - Y0) +
    '"/></a:xfrm></wpg:grpSpPr>' + G + '</' + GTag + '>';
  for I := High(Ids) downto 1 do
    Delete(Xml, Els[Ids[I]].A, Els[Ids[I]].B - Els[Ids[I]].A);
  Delete(Xml, Els[Ids[0]].A, Els[Ids[0]].B - Els[Ids[0]].A);
  Insert(G, Xml, Els[Ids[0]].A);
  Result := ApplyKeptXml(Xml, 'Group', SidAt(XmlElements(Xml), Els[Ids[0]].A));
end;

{ the group the selected shape is in: its shapes put in its place, in its parent's units }
function TParadeEdit.UngroupShape: Boolean;
var
  Xml, Kids, K: string;
  Els: TXmlEls;
  E, G, Pr, I: Integer;
  GX, GY, GCX, GCY, OX, OY, OCX, OCY, X, Y, CX, CY: Int64;
  Xf: string;
  P, L: Integer;
  SX, SY: Double;
begin
  OnlyShape;
  Result := False;
  if not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  if E < 0 then
    Exit;
  G := Els[E].Parent;
  if (G < 0) or (Els[G].Parent < 0) or not ((Els[G].Name = 'wpg:grpSp') or (Els[G].Name = 'wpg:wgp')) then
    Exit;     { not in a group (the drawing's own top is not one to undo) }
  Pr := ChildNamed(Els, G, 'wpg:grpSpPr');
  if Pr < 0 then
    Exit;
  Xf := Copy(Xml, Els[Pr].A, Els[Pr].B - Els[Pr].A);
  if not XfrmOf(Xf, GX, GY, GCX, GCY) then
    Exit;
  OX := GX; OY := GY; OCX := GCX; OCY := GCY;     { the child space: chOff, chExt (the same when none) }
  P := Pos('<a:chOff ', Xf);
  if P > 0 then
  begin
    I := AttrSpan(Xf, P, 'x', L); if I > 0 then OX := StrToInt64Def(Copy(Xf, I, L), GX);
    I := AttrSpan(Xf, P, 'y', L); if I > 0 then OY := StrToInt64Def(Copy(Xf, I, L), GY);
  end;
  P := Pos('<a:chExt ', Xf);
  if P > 0 then
  begin
    I := AttrSpan(Xf, P, 'cx', L); if I > 0 then OCX := StrToInt64Def(Copy(Xf, I, L), GCX);
    I := AttrSpan(Xf, P, 'cy', L); if I > 0 then OCY := StrToInt64Def(Copy(Xf, I, L), GCY);
  end;
  SX := 1; SY := 1;
  if OCX > 0 then SX := GCX / OCX;
  if OCY > 0 then SY := GCY / OCY;
  Kids := '';
  for I := G + 1 to High(Els) do
    if (Els[I].Parent = G) and IsShapeEl(Els[I].Name) then
    begin
      K := Copy(Xml, Els[I].A, Els[I].B - Els[I].A);
      if XfrmOf(K, X, Y, CX, CY) then
        SetXfrm(K, GX + Round((X - OX) * SX), GY + Round((Y - OY) * SY), Round(CX * SX), Round(CY * SY));
      Kids := Kids + K;
    end;
  Delete(Xml, Els[G].A, Els[G].B - Els[G].A);
  Insert(Kids, Xml, Els[G].A);
  Result := ApplyKeptXml(Xml, 'Ungroup', SidAt(XmlElements(Xml), Els[G].A));
end;

{ ---------------- canvases and shapes put in ---------------- }

function TParadeEdit.PageToClient(Page: Integer; XPt, YPt: Double): TPoint;
begin
  Result := Point(PageLeft(Page) + Round(XPt * PD_SP_PER_PT * PxPerSp), PageTop(Page) + Round(YPt * PD_SP_PER_PT * PxPerSp));
end;

procedure TParadeEdit.SelectObject(const P: pd_pos);
begin
  if IsPageCanvas(P) then
    SetCaret(P, False)    { the page's canvas: never taken as text }
  else
    SelectRange(PdRange(P, PdPos(P.block, P.offset + 3)));
end;

{ a section that is a page to draw on: no margins, its first block a canvas in front of the text from the page's
  corner; its canvas's place }
function SectionCanvas(Doc: Ppd_doc; Sec: pd_block_id; out P: pd_pos): Boolean;
var
  Fl, Para: pd_block_id;
  Info: pd_block_info;
  Sp: pd_section_props;
  Fp: pd_float_props;
  O: pd_inline;
  J: TJSONObject;
begin
  Result := False;
  P := PdPos(0, 0);
  Fl := pd_doc_child(Doc, Sec, 0);
  if (pd_doc_section_props(Doc, Sec, Sp) <> PD_OK) or (Sp.margin_left <> 0) or (Sp.margin_top <> 0) or
     (pd_doc_block_info(Doc, Fl, Info) <> PD_OK) or (Info.kind <> PD_BLOCK_FLOAT) or
     (pd_doc_float_props(Doc, Fl, Fp) <> PD_OK) or (Fp.wrap <> PD_WRAP_FRONT) or
     (Fp.offset_from <> PD_FROM_PAGE) then
    Exit;
  Para := pd_doc_child(Doc, Fl, 0);
  if (pd_doc_inline_at(Doc, PdPos(Para, 0), O) <> PD_OK) or (O.kind <> PD_INLINE_IMAGE) then
    Exit;
  J := DrawingJson(Doc, PdPos(Para, 0));
  if J = nil then
    Exit;
  try
    Result := (J.Find('xml') <> nil) and (Pos('<wpc:wpc', J.Get('xml', '')) > 0);
  finally
    J.Free;
  end;
  if Result then
    P := PdPos(Para, 0);
end;

{ the section a block is in }
function SectionOf(Doc: Ppd_doc; B: pd_block_id): pd_block_id;
var
  Info: pd_block_info;
begin
  Result := 0;
  while (B <> 0) and (pd_doc_block_info(Doc, B, Info) = PD_OK) do
  begin
    if Info.kind = PD_BLOCK_SECTION then
      Exit(B);
    B := Info.parent;
  end;
end;

function TParadeEdit.CanvasPagePos(out P: pd_pos): Boolean;
var
  Sec: pd_block_id;
begin
  Result := False;
  P := PdPos(0, 0);
  if not FCanvasPage then
    Exit;
  Sec := SectionOf(FDoc, CaretPos.block);     { the page the caret is on; else the first }
  if (Sec = 0) or not SectionCanvas(FDoc, Sec, P) then
    Result := SectionCanvas(FDoc, pd_doc_child(FDoc, pd_doc_root(FDoc), 0), P)
  else
    Result := True;
end;

function TParadeEdit.IsPageCanvas(const P: pd_pos): Boolean;
var
  C: pd_pos;
begin
  Result := FCanvasPage and SectionCanvas(FDoc, SectionOf(FDoc, P.block), C) and (C.block = P.block) and
    (C.offset = P.offset);
end;

function TParadeEdit.DetectCanvasPage: Boolean;
var
  I, N: Integer;
  P: pd_pos;
begin
  N := ChildCount(pd_doc_root(FDoc));
  Result := N > 0;
  for I := 0 to N - 1 do    { every page one to draw on (a slide each) }
    Result := Result and SectionCanvas(FDoc, pd_doc_child(FDoc, pd_doc_root(FDoc), I), P);
end;

procedure TParadeEdit.EnsureCanvas;
var
  P: pd_pos;
begin
  if FShapeOn or (StoryTopOf(CaretPos.block) <> 0) or not CanvasPagePos(P) then
    Exit;
  FShapeOn := True;
  FShapeAt := P;
  FShapeSid := -1;
  SetLength(FShapeMore, 0);
  SetCaret(P, False);
  Invalidate;
end;

function TParadeEdit.TextLocked: Boolean;
begin
  Result := FCanvasPage and (StoryTopOf(CaretPos.block) = 0);
end;

function TParadeEdit.OnPageCorner(X, Y: Integer): Boolean;
var
  Info: pd_page_info;
  CX, CY, Pg: Integer;
begin
  Result := False;
  if not FCanvasPage or FReadOnly then
    Exit;
  for Pg := 0 to PageCount - 1 do
  begin
    pd_layout_page_info(FLayout, Pg, Info);
    CX := PageLeft(Pg) + Round(Info.width * PxPerSp);
    CY := PageTop(Pg) + Round(Info.height * PxPerSp);
    if (Abs(X - CX) <= 7) and (Abs(Y - CY) <= 7) then
    begin
      FCornerPage := Pg;
      Exit(True);
    end;
  end;
end;

{ the canvas page's corner: a handle to size it by; as dragged, the page it would make }
procedure TParadeEdit.PaintPageCorner;
var
  Info: pd_page_info;
  CX, CY, Pg: Integer;
begin
  if not FCanvasPage or FReadOnly then
    Exit;
  for Pg := 0 to PageCount - 1 do
  begin
    pd_layout_page_info(FLayout, Pg, Info);
    CX := PageLeft(Pg) + Round(Info.width * PxPerSp);
    CY := PageTop(Pg) + Round(Info.height * PxPerSp);
    if (CY < -10) or (CY - Round(Info.height * PxPerSp) > ClientHeight + 10) then
      Continue;   { not in view }
    Canvas.Pen.Color := $00D77800;
    Canvas.Pen.Style := psSolid;
    Canvas.Pen.Width := 1;
    Canvas.Brush.Style := bsSolid;
    Canvas.Brush.Color := clWhite;
    Canvas.Rectangle(CX - 5, CY - 5, CX + 1, CY + 1);
    Canvas.Line(CX - 3, CY - 1, CX - 1, CY - 3);
  end;
  if FShapeDrag = 16 then
  begin
    Canvas.Brush.Style := bsClear;
    Canvas.Pen.Style := psDash;
    Canvas.Rectangle(PageLeft(FCornerPage), PageTop(FCornerPage), Max(PageLeft(FCornerPage) + 20, FBandTo.X) + 1,
      Max(PageTop(FCornerPage) + 20, FBandTo.Y) + 1);
    Canvas.Pen.Style := psSolid;
  end;
end;

function TParadeEdit.ResizeCanvasPage(W, H: pd_sp): Boolean;
var
  P: pd_pos;
  O: pd_inline;
  Keep: array[0..2] of string;
  Info: pd_block_info;
  Sec: pd_block_id;
  Sp: pd_section_props;
  Fp: pd_float_props;
  J: TJSONObject;
  R: pd_res_id;
  S: string;
  I: Integer;
  Lbl: TParadeShapeStep;
begin
  Result := False;
  W := Max(W, Round(144 * PD_SP_PER_PT));     { two inches at least }
  H := Max(H, Round(144 * PD_SP_PER_PT));
  if FReadOnly or not CanvasPagePos(P) then
    Exit;
  PushShapeStep('Page size', P);
  pd_doc_begin_group(FDoc, 'Page size');
  try
    for I := 0 to ChildCount(pd_doc_root(FDoc)) - 1 do
    begin   { every page (each slide) as big, its canvas with it, its shapes as they are }
      Sec := pd_doc_child(FDoc, pd_doc_root(FDoc), I);
      if not SectionCanvas(FDoc, Sec, P) or (pd_doc_inline_at(FDoc, P, O) <> PD_OK) or (O.width <= 0) or
         (O.height <= 0) or (pd_doc_block_info(FDoc, P.block, Info) <> PD_OK) or
         (pd_doc_section_props(FDoc, Sec, Sp) <> PD_OK) or (pd_doc_float_props(FDoc, Info.parent, Fp) <> PD_OK) then
        Continue;
      KeepInlineText(O, Keep);
      J := DrawingJson(FDoc, P);
      if J = nil then
        Continue;
      try
        J.Integers['w'] := Max(1, Round(J.Get('w', 0.0) * W / O.width));
        J.Integers['h'] := Max(1, Round(J.Get('h', 0.0) * H / O.height));
        S := J.AsJSON;
      finally
        J.Free;
      end;
      if pd_doc_add_resource(FDoc, 'application/vnd.parade.drawing+json', PAnsiChar(S), Length(S), R) <> PD_OK then
        Continue;
      O.resource := R;
      O.width := W;
      O.height := H;
      Sp.page_width := W;
      Sp.page_height := H;
      pd_doc_set_section_props(FDoc, Sec, Sp);
      Fp.width := W;
      pd_doc_set_float_props(FDoc, Info.parent, Fp);
      pd_doc_delete(FDoc, PdRange(P, PdPos(P.block, P.offset + 3)), nil);
      Result := (pd_doc_insert_inline(FDoc, P, O, nil) = PD_OK) or Result;
    end;
  finally
    pd_doc_end_group(FDoc);
  end;
  Changed;
  FShapeOn := False;
  EnsureCanvas;
end;

{ a section made a page to draw on: a canvas (Res, or a new empty one) in front of the text from the page's corner,
  as big as the page, at its start }
function TParadeEdit.PutPageCanvas(Sec: pd_block_id; Res: pd_res_id): Boolean;
var
  P: pd_section_props;
  Fp: pd_float_props;
  Fl: pd_block_id;
  J: TJSONObject;
  S, Xml: string;
  R0: pd_res_id;
  O: pd_inline;
begin
  Result := False;
  if pd_doc_section_props(FDoc, Sec, P) <> PD_OK then
    Exit;
  if Res = 0 then
  begin
    Xml := '<wpc:wpc><wpc:bg/><wpc:whole/></wpc:wpc>';
    J := TJSONObject.Create(['w', Integer(P.page_width), 'h', Integer(P.page_height), 'items', TJSONArray.Create,
      'kind', 'wpc', 'rels', TJSONObject.Create, 'xml', Xml]);
    try
      S := J.AsJSON;
    finally
      J.Free;
    end;
    if (pd_doc_add_resource(FDoc, 'application/vnd.parade.drawing+json', PAnsiChar(S), Length(S), R0) <> PD_OK) or
       (pd_docx_drawing_rebuild(FDoc, R0, PAnsiChar(Xml), Length(Xml), Res) <> PD_OK) then
      Exit;
  end;
  if pd_doc_insert_block(FDoc, Sec, 0, PD_BLOCK_FLOAT, Fl) <> PD_OK then
    Exit;
  pd_doc_float_props(FDoc, Fl, Fp);
  Fp.placement := PD_PLACE_HERE or PD_PLACE_FORCE or PD_PLACE_OFFSET;
  Fp.wrap := PD_WRAP_FRONT;
  Fp.width := P.page_width;
  Fp.gap := 0;
  Fp.offset_x := 0;
  Fp.offset_y := 0;
  Fp.offset_from := PD_FROM_PAGE;
  pd_doc_set_float_props(FDoc, Fl, Fp);
  FillChar(O, SizeOf(O), 0);
  O.kind := PD_INLINE_IMAGE;
  O.resource := Res;
  O.width := P.page_width;
  O.height := P.page_height;
  Result := pd_doc_insert_inline(FDoc, PdPos(pd_doc_child(FDoc, Fl, 0), 0), O, nil) = PD_OK;
end;

function TParadeEdit.StartCanvasPage: Boolean;
begin
  Result := False;
  if FReadOnly then
    Exit;
  SetOrientation(True);
  SetMargins(0, 0, 0, 0);
  Result := PutPageCanvas(pd_doc_child(FDoc, pd_doc_root(FDoc), 0), 0);
  pd_doc_clear_undo(FDoc);
  SetLength(FSelUndo, 0);
  SetLength(FSelRedo, 0);
  Changed;
  FModified := False;
  FCanvasPage := Result;
  FShapeOn := False;
  EnsureCanvas;
end;

{ ---------------- slides: the canvas pages of a presentation ---------------- }

function TParadeEdit.SlideCount: Integer;
begin
  if FCanvasPage then
    Result := ChildCount(pd_doc_root(FDoc))
  else
    Result := 0;
end;

function TParadeEdit.CurrentSlide: Integer;
var
  Sec: pd_block_id;
  Info: pd_block_info;
begin
  Result := -1;
  if not FCanvasPage then
    Exit;
  if FShapeOn then
    Sec := SectionOf(FDoc, FShapeAt.block)
  else
    Sec := SectionOf(FDoc, CaretPos.block);
  if (Sec <> 0) and (pd_doc_block_info(FDoc, Sec, Info) = PD_OK) then
    Result := Info.index;
end;

function TParadeEdit.GoToSlide(Index: Integer): Boolean;
var
  P: pd_pos;
begin
  Result := False;
  if (Index < 0) or (Index >= SlideCount) or
     not SectionCanvas(FDoc, pd_doc_child(FDoc, pd_doc_root(FDoc), Index), P) then
    Exit;
  ClearShapeSelection;
  SetCaret(P, False);
  EnsureCanvas;
  ShowSlide(Index);
  Result := True;
end;

procedure TParadeEdit.ShowSlide(Index: Integer);
var
  P: pd_pos;
  Pg: Int32;
  X0, Y0, X1, Y1: Double;
begin
  if (Index < 0) or (Index >= SlideCount) or
     not SectionCanvas(FDoc, pd_doc_child(FDoc, pd_doc_root(FDoc), Index), P) or
     not ShapePageBox(P, -1, Pg, X0, Y0, X1, Y1) then
    Exit;
  FScrollY := Max(0, PageTop(Pg) + FScrollY - FPageGap div 2);     { its page at the top of the view }
  UpdateScrollBar;
  Invalidate;
end;

function TParadeEdit.NewSlide(After: Integer; Copy: Boolean): Boolean;
var
  Src, Sec: pd_block_id;
  Sp: pd_section_props;
  P, C: pd_pos;
  O: pd_inline;
  J: TJSONObject;
  Xml: string;
  R: pd_res_id;
  Old, New_: TParadeBlockArray;
  I: Integer;
begin
  Result := False;
  if FReadOnly or not FCanvasPage or (After < -1) or (After >= SlideCount) then
    Exit;
  Src := pd_doc_child(FDoc, pd_doc_root(FDoc), Max(0, After));
  if pd_doc_section_props(FDoc, Src, Sp) <> PD_OK then
    Exit;
  R := 0;
  Old := nil;
  if Copy and SectionCanvas(FDoc, Src, C) and (pd_doc_inline_at(FDoc, C, O) = PD_OK) then
  begin   { its canvas made again from its XML: the same shapes, text boxes of their own (their text copied below) }
    J := DrawingJson(FDoc, C);
    if J <> nil then
      try
        Xml := J.Get('xml', '');
      finally
        J.Free;
      end;
    { every text box marked new (story -1): stories of their own, not the slide's }
    Xml := StringReplace(MarkTextBoxes(Xml), '<!--pd-story:', '<!--pd-story:-1', [rfReplaceAll]);
    if (Xml = '') or (pd_docx_drawing_rebuild(FDoc, O.resource, PAnsiChar(Xml), Length(Xml), R) <> PD_OK) then
      R := 0;
    Old := ResStories(FDoc, O.resource);
  end;
  PushShapeStep(IfThen(Copy, 'Duplicate slide', 'New slide'), FShapeAt);
  pd_doc_begin_group(FDoc, PAnsiChar(IfThen(Copy, 'Duplicate slide', 'New slide')));
  try
    if pd_doc_insert_block(FDoc, pd_doc_root(FDoc), After + 1, PD_BLOCK_SECTION, Sec) <> PD_OK then
      Exit;
    pd_doc_set_section_props(FDoc, Sec, Sp);
    Result := PutPageCanvas(Sec, R);
    if Result and (R <> 0) then
    begin   { the copies' text boxes: the text the slide's have now }
      New_ := ResStories(FDoc, R);
      for I := 0 to Min(High(Old), High(New_)) do
        FillStory(New_[I], StoryData(Old[I]));
    end;
  finally
    pd_doc_end_group(FDoc);
  end;
  Changed;
  if Result then
    GoToSlide(After + 1);
  if SectionCanvas(FDoc, Sec, P) then
    FShapeAt := P;
end;

function TParadeEdit.DeleteSlide(Index: Integer): Boolean;
var
  Sec: pd_block_id;
  P: pd_pos;
begin
  Result := False;
  if FReadOnly or (SlideCount < 2) or (Index < 0) or (Index >= SlideCount) then
    Exit;
  Sec := pd_doc_child(FDoc, pd_doc_root(FDoc), Index);
  ClearShapeSelection;
  if SectionCanvas(FDoc, Sec, P) then
    PushShapeStep('Delete slide', P);
  SetCaret(PdPos(FirstPara, 0), False);
  pd_doc_begin_group(FDoc, 'Delete slide');
  try
    if SectionCanvas(FDoc, Sec, P) then
      DropStories(StoriesInRange(P, PdPos(P.block, P.offset + 3)), nil);
    Result := pd_doc_remove_block(FDoc, Sec) = PD_OK;
  finally
    pd_doc_end_group(FDoc);
  end;
  Changed;
  GoToSlide(Min(Index, SlideCount - 1));
end;

function TParadeEdit.MoveSlide(Index, ToIndex: Integer): Boolean;
var
  Sec: pd_block_id;
begin
  Result := False;
  if FReadOnly or (Index < 0) or (Index >= SlideCount) or (ToIndex < 0) or (ToIndex >= SlideCount) or
     (Index = ToIndex) then
    Exit;
  Sec := pd_doc_child(FDoc, pd_doc_root(FDoc), Index);
  PushShapeStep('Move slide', FShapeAt);
  pd_doc_begin_group(FDoc, 'Move slide');
  try
    Result := pd_doc_move_block(FDoc, Sec, pd_doc_root(FDoc), ToIndex) = PD_OK;
  finally
    pd_doc_end_group(FDoc);
  end;
  Changed;
  GoToSlide(ToIndex);
end;

function TParadeEdit.ResizeObject(W, H: pd_sp): Boolean;
var
  O: pd_inline;
  Keep: array[0..2] of string;
  P: pd_pos;
  J: TJSONObject;
  R: pd_res_id;
  S: string;
  Sp: pd_section_props;
begin
  Result := False;
  P := FShapeAt;
  if FReadOnly or not FShapeOn or (W <= 0) or (H <= 0) or (pd_doc_inline_at(FDoc, P, O) <> PD_OK) then
    Exit;
  KeepInlineText(O, Keep);
  R := 0;
  if CanvasSelected and (O.width > 0) and (O.height > 0) then
  begin   { a canvas: more room or less to draw in, its shapes as they are (not stretched with it); no wider
            than the text, no deeper than the page has room for }
    Sp := CurrentSectionProps;
    if TextWidthAt(P.block) > 0 then
      W := Min(W, TextWidthAt(P.block));
    if Sp.page_height > 0 then
      H := Max(PD_SP_PER_PT, Min(H, Sp.page_height - Sp.margin_top - Sp.margin_bottom - Round(24 * PD_SP_PER_PT)));
    J := DrawingJson(FDoc, P);
    if J <> nil then
      try
        J.Integers['w'] := Max(1, Round(J.Get('w', 0.0) * W / O.width));
        J.Integers['h'] := Max(1, Round(J.Get('h', 0.0) * H / O.height));
        S := J.AsJSON;
        if pd_doc_add_resource(FDoc, 'application/vnd.parade.drawing+json', PAnsiChar(S), Length(S), R) = PD_OK then
          O.resource := R;
      finally
        J.Free;
      end;
  end;
  O.width := W;
  O.height := H;
  PushShapeStep('Size', P);
  pd_doc_begin_group(FDoc, 'Size');
  pd_doc_delete(FDoc, PdRange(P, PdPos(P.block, P.offset + 3)), nil);
  Result := pd_doc_insert_inline(FDoc, P, O, nil) = PD_OK;
  pd_doc_end_group(FDoc);
  FShapeOn := True;
  FShapeAt := P;
  FShapeSid := -1;
  SelectObject(P);
  Changed;
end;

{ ---------------- a shape turned, adjusted, its points edited ---------------- }

type
  { a step of a custom geometry's path: m (move), l (line), c (a cubic: two controls and its end), q, z (close) }
  TPathCmd = record
    Cmd: Char;
    X, Y: array[0..2] of Double;
    N: Integer;
  end;
  TPathCmds = array of TPathCmd;
  TPathFlat = array of Double;

{ an attribute of the tag at At set: its value replaced, or put after the tag's name }
procedure SetAttr(var Xml: string; At: Integer; const Name, Value: string);
var
  P, L, E: Integer;
begin
  P := AttrSpan(Xml, At, Name, L);
  if P > 0 then
  begin
    Delete(Xml, P, L);
    Insert(Value, Xml, P);
    Exit;
  end;
  E := At + 1;
  while (E <= Length(Xml)) and not (Xml[E] in [' ', '>', '/', #9, #10, #13]) do
    Inc(E);
  Insert(' ' + Name + '="' + Value + '"', Xml, E);
end;

function AttrOf(const Xml: string; At: Integer; const Name: string): string;
var
  P, L: Integer;
begin
  P := AttrSpan(Xml, At, Name, L);
  if P > 0 then
    Result := Copy(Xml, P, L)
  else
    Result := '';
end;

{ Office's preset at a size (its own units) with adjustments ("adj1=5000 ..."), as pd_preset_json describes it:
  its adjustments, handles and paths; nil for none }
function PresetJson(const Prst: string; W, H: Double; const Adj: string): TJSONObject;
var
  N: csize_t;
  S: RawByteString;
  J: TJSONData;
begin
  Result := nil;
  if Prst = '' then
    Exit;
  N := pd_preset_json(PAnsiChar(Prst), W, H, PAnsiChar(Adj), nil, 0);
  if N = 0 then
    Exit;
  SetLength(S, N + 1);
  pd_preset_json(PAnsiChar(Prst), W, H, PAnsiChar(Adj), PAnsiChar(S), N + 1);
  SetLength(S, N);
  try
    J := GetJSON(S);
  except
    Exit;
  end;
  if J is TJSONObject then
    Result := TJSONObject(J)
  else
    J.Free;
end;

{ a shape's adjustments as the preset functions take them }
function AdjString(const G: TParadeShapeGeom): string;
var
  I: Integer;
begin
  Result := '';
  for I := 0 to High(G.AdjN) do
    Result := Result + IfThen(Result <> '', ' ', '') + G.AdjN[I] + '=' + IntToStr(Round(G.AdjV[I]));
end;

{ the adjustments of a shape's preset, at their defaults }
procedure PresetAdjs(var G: TParadeShapeGeom);
var
  J: TJSONObject;
  A: TJSONArray;
  I: Integer;
begin
  SetLength(G.AdjN, 0);
  SetLength(G.AdjV, 0);
  J := PresetJson(G.Prst, Max(G.EX, 1), Max(G.EY, 1), '');
  if J = nil then
    Exit;
  try
    A := J.Find('adj') as TJSONArray;
    if A <> nil then
    begin
      SetLength(G.AdjN, A.Count);
      SetLength(G.AdjV, A.Count);
      for I := 0 to A.Count - 1 do
      begin
        G.AdjN[I] := TJSONArray(A[I])[0].AsString;
        G.AdjV[I] := TJSONArray(A[I])[2].AsFloat;
      end;
    end;
  finally
    J.Free;
  end;
end;

{ the adjustments a string of them gives, into a shape's }
procedure TakeAdjs(var G: TParadeShapeGeom; const Adj: string);
var
  Parts: TStringArray;
  I, K, Q: Integer;
begin
  Parts := Adj.Split([' ', ',']);
  for I := 0 to High(Parts) do
  begin
    Q := Pos('=', Parts[I]);
    if Q > 0 then
      for K := 0 to High(G.AdjN) do
        if G.AdjN[K] = Copy(Parts[I], 1, Q - 1) then
          G.AdjV[K] := StrToFloatDef(Copy(Parts[I], Q + 1, MaxInt), G.AdjV[K]);
  end;
end;

{ where a shape's preset puts its handles, as its adjustments are }
procedure PresetHandles(var G: TParadeShapeGeom);
var
  J, Hd: TJSONObject;
  A: TJSONArray;
  I, N: Integer;
begin
  SetLength(G.HX, 0);
  SetLength(G.HY, 0);
  SetLength(G.HIdx, 0);
  J := PresetJson(G.Prst, G.EX, G.EY, AdjString(G));
  if J = nil then
    Exit;
  try
    A := J.Find('handles') as TJSONArray;
    N := 0;
    if A <> nil then
      for I := 0 to A.Count - 1 do
      begin
        Hd := TJSONObject(A[I]);
        if (Hd.Get('g1', '') = '') and (Hd.Get('g2', '') = '') then
          Continue;
        SetLength(G.HX, N + 1);
        SetLength(G.HY, N + 1);
        SetLength(G.HIdx, N + 1);
        G.HX[N] := Hd.Get('x', 0.0) * G.KX;
        G.HY[N] := Hd.Get('y', 0.0) * G.KY;
        G.HIdx[N] := I;
        Inc(N);
      end;
  finally
    J.Free;
  end;
end;

procedure AddCmd(var C: TPathCmds; Cmd: Char; const P: array of Double);
var
  K: Integer;
begin
  SetLength(C, Length(C) + 1);
  C[High(C)].Cmd := Cmd;
  C[High(C)].N := Length(P) div 2;
  for K := 0 to C[High(C)].N - 1 do
  begin
    C[High(C)].X[K] := P[2 * K];
    C[High(C)].Y[K] := P[2 * K + 1];
  end;
end;

{ a preset's outline as one path in its own units (EMU, 0..EX by 0..EY), with adjustments Adj: what a custom
  geometry made of it starts as (the paths it outlines, or all of them when it outlines none), and what a drag of a
  handle shows }
function PresetPath(const G: TParadeShapeGeom; const Adj: string): TPathCmds;
var
  J, Pa: TJSONObject;
  Ps, Cs, Cm: TJSONArray;
  I, K, Pass: Integer;
  Any: Boolean;
begin
  Result := nil;
  J := PresetJson(G.Prst, Max(G.EX, 1), Max(G.EY, 0), Adj);
  if J = nil then
    Exit;
  try
    Ps := J.Find('paths') as TJSONArray;
    if Ps = nil then
      Exit;
    Any := False;
    for I := 0 to Ps.Count - 1 do
      Any := Any or (TJSONObject(Ps[I]).Get('stroke', 1) = 1);
    for Pass := 0 to 0 do
      for I := 0 to Ps.Count - 1 do
      begin
        Pa := TJSONObject(Ps[I]);
        if Any and (Pa.Get('stroke', 1) <> 1) then
          Continue;
        Cs := Pa.Find('cmds') as TJSONArray;
        if Cs = nil then
          Continue;
        for K := 0 to Cs.Count - 1 do
        begin
          Cm := TJSONArray(Cs[K]);
          case Cm[0].AsString of
            'm': AddCmd(Result, 'm', [Cm[1].AsFloat, Cm[2].AsFloat]);
            'l': AddCmd(Result, 'l', [Cm[1].AsFloat, Cm[2].AsFloat]);
            'c': AddCmd(Result, 'c', [Cm[1].AsFloat, Cm[2].AsFloat, Cm[3].AsFloat, Cm[4].AsFloat, Cm[5].AsFloat,
                   Cm[6].AsFloat]);
            'z': AddCmd(Result, 'z', []);
          end;
        end;
      end;
  finally
    J.Free;
  end;
end;

{ a path's steps as DrawingML (what goes inside its a:path) }
function PathXml(const C: TPathCmds): string;
const
  Names: array[0..3] of string = ('a:moveTo', 'a:lnTo', 'a:cubicBezTo', 'a:quadBezTo');
var
  I, K, N: Integer;
begin
  Result := '';
  for I := 0 to High(C) do
  begin
    case C[I].Cmd of
      'm': N := 0;
      'l': N := 1;
      'c': N := 2;
      'q': N := 3;
    else
      begin
        Result := Result + '<a:close/>';
        Continue;
      end;
    end;
    Result := Result + '<' + Names[N] + '>';
    for K := 0 to C[I].N - 1 do
      Result := Result + '<a:pt x="' + IntToStr(Round(C[I].X[K])) + '" y="' + IntToStr(Round(C[I].Y[K])) + '"/>';
    Result := Result + '</' + Names[N] + '>';
  end;
end;

function CustGeomXml(const C: TPathCmds; PW, PH: Double): string;
begin
  Result := '<a:custGeom><a:avLst/><a:gdLst/><a:ahLst/><a:cxnLst/><a:rect l="l" t="t" r="r" b="b"/><a:pathLst>' +
    '<a:path w="' + IntToStr(Round(PW)) + '" h="' + IntToStr(Round(PH)) + '">' + PathXml(C) +
    '</a:path></a:pathLst></a:custGeom>';
end;

{ the first path of a shape's custom geometry (Pr its properties element): its element, steps and units; False when
  it has none, or one of guides or arcs this editor does not move }
function ParsePath(const Xml: string; const Els: TXmlEls; Pr: Integer; out PathEl: Integer; out C: TPathCmds;
  out PW, PH: Double): Boolean;
var
  Cg, Pl, I, K: Integer;
  Cmd: Char;
  X, Y: Double;
begin
  Result := False;
  C := nil;
  PathEl := -1;
  PW := 0;
  PH := 0;
  Cg := ChildNamed(Els, Pr, 'a:custGeom');
  if Cg < 0 then Exit;
  Pl := ChildNamed(Els, Cg, 'a:pathLst');
  if Pl < 0 then Exit;
  PathEl := ChildNamed(Els, Pl, 'a:path');
  if PathEl < 0 then Exit;
  PW := StrToFloatDef(AttrOf(Xml, Els[PathEl].A, 'w'), 0);
  PH := StrToFloatDef(AttrOf(Xml, Els[PathEl].A, 'h'), 0);
  for I := PathEl + 1 to High(Els) do
    if Els[I].Parent = PathEl then
    begin
      case Els[I].Name of
        'a:moveTo': Cmd := 'm';
        'a:lnTo': Cmd := 'l';
        'a:cubicBezTo': Cmd := 'c';
        'a:quadBezTo': Cmd := 'q';
        'a:close': Cmd := 'z';
      else
        Exit;     { an arc: not a point to drag }
      end;
      SetLength(C, Length(C) + 1);
      C[High(C)].Cmd := Cmd;
      C[High(C)].N := 0;
      for K := I + 1 to High(Els) do
        if (Els[K].Parent = I) and (Els[K].Name = 'a:pt') and (C[High(C)].N < 3) then
        begin
          X := StrToFloatDef(AttrOf(Xml, Els[K].A, 'x'), NaN);
          Y := StrToFloatDef(AttrOf(Xml, Els[K].A, 'y'), NaN);
          if IsNan(X) or IsNan(Y) then
            Exit;   { a guide's name: not a number to move }
          C[High(C)].X[C[High(C)].N] := X;
          C[High(C)].Y[C[High(C)].N] := Y;
          Inc(C[High(C)].N);
        end;
    end;
  Result := Length(C) > 0;
end;

{ whether point K of a step is where the path goes (an anchor), not a curve's control }
function IsAnchor(const S: TPathCmd; K: Integer): Boolean;
begin
  Result := (K = S.N - 1) and (S.Cmd in ['m', 'l', 'c', 'q']);
end;

{ the point of a path numbered N (counting every point of every step): its step and its place there }
function PathPoint(const C: TPathCmds; N: Integer; out Ci, Ki: Integer): Boolean;
var
  I: Integer;
begin
  Result := False;
  Ci := -1;
  Ki := -1;
  for I := 0 to High(C) do
  begin
    if N < C[I].N then
    begin
      Ci := I;
      Ki := N;
      Exit(True);
    end;
    Dec(N, C[I].N);
  end;
end;

{ a path flattened: its points in its units, a NaN pair between rings }
function FlattenPath(const C: TPathCmds): TPathFlat;
var
  I, K, N: Integer;
  X0, Y0, SX, SY, T, U: Double;

  procedure Put(X, Y: Double);
  begin
    SetLength(Result, Length(Result) + 2);
    Result[High(Result) - 1] := X;
    Result[High(Result)] := Y;
  end;

begin
  Result := nil;
  X0 := 0; Y0 := 0; SX := 0; SY := 0;
  for I := 0 to High(C) do
    case C[I].Cmd of
      'm':
        begin
          if Length(Result) > 0 then
            Put(NaN, NaN);
          X0 := C[I].X[0]; Y0 := C[I].Y[0]; SX := X0; SY := Y0;
          Put(X0, Y0);
        end;
      'l':
        begin
          X0 := C[I].X[0]; Y0 := C[I].Y[0];
          Put(X0, Y0);
        end;
      'c', 'q':
        begin
          N := 12;
          for K := 1 to N do
          begin
            T := K / N;
            U := 1 - T;
            if C[I].Cmd = 'c' then
              Put(U * U * U * X0 + 3 * U * U * T * C[I].X[0] + 3 * U * T * T * C[I].X[1] + T * T * T * C[I].X[2],
                U * U * U * Y0 + 3 * U * U * T * C[I].Y[0] + 3 * U * T * T * C[I].Y[1] + T * T * T * C[I].Y[2])
            else
              Put(U * U * X0 + 2 * U * T * C[I].X[0] + T * T * C[I].X[1], U * U * Y0 + 2 * U * T * C[I].Y[0] +
                T * T * C[I].Y[1]);
          end;
          X0 := C[I].X[C[I].N - 1];
          Y0 := C[I].Y[C[I].N - 1];
        end;
      'z':
        begin
          Put(SX, SY);
          X0 := SX; Y0 := SY;
        end;
    end;
end;

{ ---------------- shapes copied, cut, pasted ---------------- }

function TParadeEdit.StoryData(St: pd_block_id): string;
var
  A, B: pd_block_id;
  N: Integer;
  Ss: TStringStream;
  Info: pd_block_info;
begin
  Result := '';
  N := ChildCount(St);
  if N = 0 then
    Exit;
  A := pd_doc_child(FDoc, St, 0);
  B := pd_doc_child(FDoc, St, N - 1);
  if (pd_doc_block_info(FDoc, A, Info) <> PD_OK) or (Info.kind <> PD_BLOCK_PARAGRAPH) or
     (pd_doc_block_info(FDoc, B, Info) <> PD_OK) or (Info.kind <> PD_BLOCK_PARAGRAPH) then
    Exit;     { a table first or last: the text box's XML as it was read }
  Ss := TStringStream.Create('');
  try
    if pd_doc_export_range(FDoc, PdRange(PdPos(A, 0), PdPos(B, Length(ParaText(B)))), PD_CONV_JDATA,
       @WriteToStream, Ss) = PD_OK then
      Result := Ss.DataString;
  finally
    Ss.Free;
  end;
end;

{ a story's text replaced by text StoryData gave }
procedure TParadeEdit.FillStory(St: pd_block_id; const Data: string);
var
  A, B: pd_block_id;
  N: Integer;
  After: pd_pos;
begin
  N := ChildCount(St);
  if (Data = '') or (N = 0) then
    Exit;
  A := pd_doc_child(FDoc, St, 0);
  B := pd_doc_child(FDoc, St, N - 1);
  if Compare(PdPos(A, 0), PdPos(B, Length(ParaText(B)))) < 0 then
    pd_doc_delete(FDoc, PdRange(PdPos(A, 0), PdPos(B, Length(ParaText(B)))), nil);
  pd_doc_paste(FDoc, PdPos(pd_doc_child(FDoc, St, 0), 0), PAnsiChar(Data), Length(Data), PD_CONV_JDATA, @After);
end;

{ how many text boxes' contents the XML has before a place }
function TextBoxesBefore(const Xml: string; At: Integer): Integer;
var
  P: Integer;
begin
  Result := 0;
  P := Pos('<w:txbxContent', Xml);
  while (P > 0) and (P < At) do
  begin
    if (P + 14 <= Length(Xml)) and (Xml[P + 14] in ['>', ' ']) then
      Inc(Result);
    P := PosEx('<w:txbxContent', Xml, P + 14);
  end;
end;

{ a point of a shape's parent (a group's child space) in the drawing's top's units: through each group it is in,
  up to the canvas (or the group the drawing is); SX, SY multiplied by the groups' scales }
procedure MapUp(const Xml: string; const Els: TXmlEls; E: Integer; var X, Y, SX, SY: Double);
var
  P, Pr, Xf, Off, Ext, COff, CExt: Integer;
  OX, OY, CX, CY, CHX, CHY, CHW, CHH, KX, KY: Double;
begin
  P := Els[E].Parent;
  while (P >= 0) and (Els[P].Parent >= 0) do
  begin
    if (Els[P].Name = 'wpg:grpSp') or (Els[P].Name = 'wpg:wgp') then
    begin
      Pr := ChildNamed(Els, P, 'wpg:grpSpPr');
      Xf := -1;
      if Pr >= 0 then
        Xf := ChildNamed(Els, Pr, 'a:xfrm');
      if Xf >= 0 then
      begin
        Off := ChildNamed(Els, Xf, 'a:off');
        Ext := ChildNamed(Els, Xf, 'a:ext');
        COff := ChildNamed(Els, Xf, 'a:chOff');
        CExt := ChildNamed(Els, Xf, 'a:chExt');
        if (Off >= 0) and (Ext >= 0) then
        begin
          OX := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'x'), 0);
          OY := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'y'), 0);
          CX := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cx'), 0);
          CY := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cy'), 0);
          CHX := OX; CHY := OY; CHW := CX; CHH := CY;
          if COff >= 0 then
          begin
            CHX := StrToFloatDef(AttrOf(Xml, Els[COff].A, 'x'), OX);
            CHY := StrToFloatDef(AttrOf(Xml, Els[COff].A, 'y'), OY);
          end;
          if CExt >= 0 then
          begin
            CHW := StrToFloatDef(AttrOf(Xml, Els[CExt].A, 'cx'), CX);
            CHH := StrToFloatDef(AttrOf(Xml, Els[CExt].A, 'cy'), CY);
          end;
          if CHW > 0 then KX := CX / CHW else KX := 1;
          if CHH > 0 then KY := CY / CHH else KY := 1;
          X := OX + (X - CHX) * KX;
          Y := OY + (Y - CHY) * KY;
          SX := SX * KX;
          SY := SY * KY;
        end;
      end;
    end;
    P := Els[P].Parent;
  end;
end;

{ a shape's element with its box in the drawing's top's units, as it would be out of the groups it is in }
function ElAtTop(const Xml: string; const Els: TXmlEls; E: Integer): string;
var
  X, Y, CX, CY: Int64;
  FX, FY, SX, SY: Double;
begin
  Result := Copy(Xml, Els[E].A, Els[E].B - Els[E].A);
  if (Els[E].Parent < 0) or (Els[Els[E].Parent].Parent < 0) or not XfrmOf(Result, X, Y, CX, CY) then
    Exit;
  FX := X;
  FY := Y;
  SX := 1;
  SY := 1;
  MapUp(Xml, Els, E, FX, FY, SX, SY);
  if (SX <> 1) or (SY <> 1) or (FX <> X) or (FY <> Y) then
    SetXfrm(Result, Round(FX), Round(FY), Round(CX * SX), Round(CY * SY));
end;

{ the top elements of the drawing's XML the selected shapes are (Whole: the group a shape is in, the group's), in
  the XML's order }
function TopShapeEls(const Els: TXmlEls; const Sids: TIntegerArray; Whole: Boolean): TIntegerArray;
var
  I, K, E, T: Integer;
  Have: Boolean;
begin
  Result := nil;
  for I := 0 to High(Sids) do
  begin
    E := SidElement(Els, Sids[I]);
    if E < 0 then
      Continue;
    if Whole then
      while (Els[E].Parent >= 0) and (Els[Els[E].Parent].Parent >= 0) and
            ((Els[Els[E].Parent].Name = 'wpg:grpSp') or (Els[Els[E].Parent].Name = 'wpg:wgp')) do
        E := Els[E].Parent;     { up to the drawing's own top, the canvas or the group it is }
    Have := False;
    for K := 0 to High(Result) do
      Have := Have or (Result[K] = E);
    if not Have then
    begin
      SetLength(Result, Length(Result) + 1);
      Result[High(Result)] := E;
    end;
  end;
  for I := 0 to High(Result) do
    for K := I + 1 to High(Result) do
      if Els[Result[K]].A < Els[Result[I]].A then
      begin
        T := Result[I]; Result[I] := Result[K]; Result[K] := T;
      end;
end;

function TParadeEdit.ShapesPayload(out S: string): Boolean;
var
  J, Out: TJSONObject;
  Items: TJSONArray;
  Xml, Sh, Part: string;
  Els: TXmlEls;
  Tops: TIntegerArray;
  Stories: TParadeBlockArray;
  XA, SA: TJSONArray;
  PA, Rels: TJSONObject;
  I, K, P, L, N, Id, K0, NT: Integer;
  Ids: TStringList;
  Rid: string;
  Bytes: RawByteString;
  Mime: PAnsiChar;
  Data: Pointer;
  Len: csize_t;
begin
  Result := False;
  S := '';
  Rels := nil;
  if not FShapeOn or (FShapeSid < 0) then
    Exit;
  J := DrawingJson(FDoc, FShapeAt);
  if J = nil then
    Exit;
  try
    if (J.Find('xml') = nil) or (J.Find('xml').JSONType <> jtString) then
      Exit;
    Xml := J.Strings['xml'];
    Stories := nil;
    Items := J.Find('items') as TJSONArray;
    if Items <> nil then
      for I := 0 to Items.Count - 1 do
        if (Items[I] is TJSONObject) and (TJSONObject(Items[I]).Find('story') <> nil) then
        begin
          SetLength(Stories, Length(Stories) + 1);
          Stories[High(Stories)] := TJSONObject(Items[I]).Integers['story'];
        end;
    if J.Find('rels') is TJSONObject then
      Rels := TJSONObject(J.Find('rels').Clone);
  finally
    J.Free;
  end;
  Els := XmlElements(Xml);
  Tops := TopShapeEls(Els, SelectedShapes, False);
  for I := High(Tops) downto 0 do   { a shape whose group is selected too: with the group }
    for K := 0 to High(Tops) do
      if (K <> I) and (Els[Tops[I]].A > Els[Tops[K]].A) and (Els[Tops[I]].B <= Els[Tops[K]].B) then
      begin
        Delete(Tops, I, 1);
        Break;
      end;
  if Length(Tops) = 0 then
    Exit;
  Out := TJSONObject.Create;
  Ids := TStringList.Create;
  try
    XA := TJSONArray.Create;
    SA := TJSONArray.Create;
    PA := TJSONObject.Create;
    Out.Add('xml', XA);
    Out.Add('stories', SA);
    Out.Add('pics', PA);
    for I := 0 to High(Tops) do
    begin
      Part := ElAtTop(Xml, Els, Tops[I]);    { out of the groups it is in }
      K := Pos('r:embed="', Part);
      while K > 0 do
      begin   { a picture: its file with it, for wherever it is pasted }
        L := PosEx('"', Part, K + 9);
        Rid := Copy(Part, K + 9, L - K - 9);
        if (PA.Find(Rid) = nil) and (Rels <> nil) and (Rels.Find(Rid) <> nil) and
           (pd_doc_resource(FDoc, Rels.Integers[Rid], @Mime, @Data, @Len) = PD_OK) then
        begin
          SetString(Bytes, PAnsiChar(Data), Len);
          PA.Add(Rid, TJSONObject.Create(['mime', StrPas(Mime), 'data', EncodeStringBase64(Bytes)]));
        end;
        K := PosEx('r:embed="', Part, L);
      end;
      Sh := '';       { its ids made new ones where it is pasted (a connector's ends: the same new ones) }
      P := 1;
      K := Pos(' id="', Part);
      while K > 0 do
      begin
        L := PosEx('"', Part, K + 5);
        if L = 0 then
          Break;
        Id := Ids.IndexOf(Copy(Part, K + 5, L - K - 5));
        if Id < 0 then
          Id := Ids.Add(Copy(Part, K + 5, L - K - 5));
        Sh := Sh + Copy(Part, P, K + 5 - P) + '%ID' + IntToStr(Id) + '%';
        P := L;
        K := PosEx(' id="', Part, L);
      end;
      Sh := Sh + Copy(Part, P, MaxInt);
      XA.Add(Sh);
      K0 := TextBoxesBefore(Xml, Els[Tops[I]].A);
      NT := TextBoxesBefore(Part, Length(Part) + 1);
      for N := 0 to NT - 1 do
        if K0 + N <= High(Stories) then
          SA.Add(StoryData(Stories[K0 + N]))
        else
          SA.Add('');
    end;
    if XA.Count = 0 then
      Exit;
    S := Out.AsJSON;
    Result := True;
  finally
    Out.Free;
    Ids.Free;
    Rels.Free;
  end;
end;

function TParadeEdit.PutShapesPayload(const S: string; const Lbl: string): Boolean;
const
  Gap = 114300;   { an eighth of an inch (EMU): a copy beside the shape it is of }
  Margin = 114300;
var
  PA: TJSONObject;
  Rid: string;
  Bytes: RawByteString;
  R: pd_res_id;
  D: TJSONData;
  XA, SA: TJSONArray;
  Parts: array of string;
  Xml, Sh, First: string;
  I, K, N, K0, Base: Integer;
  X, Y, CX, CY, X0, Y0, X1, Y1, DX, DY: Int64;
  NewCanvas: Boolean;
  Stories: TParadeBlockArray;
  J: TJSONObject;
  Items: TJSONArray;
  O: pd_inline;
begin
  Result := False;
  if FReadOnly then
    Exit;
  try
    D := GetJSON(S);
  except
    Exit;
  end;
  try
    if not (D is TJSONObject) or not (TJSONObject(D).Find('xml') is TJSONArray) then
      Exit;
    XA := TJSONArray(TJSONObject(D).Find('xml'));
    SA := TJSONArray(TJSONObject(D).Find('stories'));
    SetLength(Parts, XA.Count);
    X0 := High(Int64); Y0 := High(Int64); X1 := Low(Int64); Y1 := Low(Int64);
    for I := 0 to XA.Count - 1 do
    begin
      Parts[I] := XA.Strings[I];
      if XfrmOf(Parts[I], X, Y, CX, CY) then
      begin
        X0 := Min(X0, X); Y0 := Min(Y0, Y); X1 := Max(X1, X + CX); Y1 := Max(Y1, Y + CY);
      end;
    end;
    if (Length(Parts) = 0) or (X1 < X0) then
      Exit;
    EnsureCanvas;
    PushShapeStep(Lbl, FShapeAt);
    if FShapeOn and (FShapeSid >= 0) then
      FShapeSid := -1;
    NewCanvas := not (FShapeOn and CanvasSelected);
    Inc(FStepDepth);
    pd_doc_begin_group(FDoc, PAnsiChar(Lbl));
    try
      DX := 0;
      DY := 0;
      if NewCanvas then
      begin   { a canvas around them, at the caret (after the drawing selected, not over it) }
        if FShapeOn then
          SetCaret(PdPos(FShapeAt.block, FShapeAt.offset + 3), False);
        ClearShapeSelection;
        if not InsertCanvas((X1 - X0 + 2 * Margin) / 12700, (Y1 - Y0 + 2 * Margin) / 12700) then
          Exit;
        DX := Margin - X0;
        DY := Margin - Y0;
      end
      else if KeptXml(Xml) then
      begin   { the shapes they are copies of still there: beside them }
        XfrmOf(Parts[0], X, Y, CX, CY);
        for K := 1 to 20 do
        begin
          First := '<a:off x="' + IntToStr(X + DX) + '" y="' + IntToStr(Y + DY) + '"/>';
          if Pos(First, Xml) = 0 then
            Break;
          DX := DX + Gap;
          DY := DY + Gap;
        end;
      end;
      PA := nil;
      if TJSONObject(D).Find('pics') is TJSONObject then
        PA := TJSONObject(TJSONObject(D).Find('pics'));
      if (PA <> nil) and (PA.Count > 0) then
      begin   { the pictures' files, the canvas's own now, by ids of its own }
        J := DrawingJson(FDoc, FShapeAt);
        if J = nil then
          Exit;
        try
          if not (J.Find('rels') is TJSONObject) then
            J.Add('rels', TJSONObject.Create);
          for I := 0 to PA.Count - 1 do
            if PA.Items[I] is TJSONObject then
            begin
              Bytes := DecodeStringBase64(TJSONObject(PA.Items[I]).Get('data', ''));
              if (Bytes = '') or (pd_doc_add_resource(FDoc, PAnsiChar(TJSONObject(PA.Items[I]).Get('mime', '')),
                 PAnsiChar(Bytes), Length(Bytes), R) <> PD_OK) then
                Continue;
              Rid := 'rIdPd' + IntToStr(R);
              TJSONObject(J.Find('rels')).Delete(Rid);
              TJSONObject(J.Find('rels')).Add(Rid, Integer(R));
              for K := 0 to High(Parts) do
                Parts[K] := StringReplace(Parts[K], 'r:embed="' + PA.Names[I] + '"', 'r:embed="' + Rid + '"',
                  [rfReplaceAll]);
            end;
          if not ReplaceDrawing(FShapeAt, J.AsJSON, Lbl) then
            Exit;
        finally
          J.Free;
        end;
      end;
      Sh := '';
      for I := 0 to High(Parts) do
      begin
        if (DX <> 0) or (DY <> 0) then
          if XfrmOf(Parts[I], X, Y, CX, CY) then
            SetXfrm(Parts[I], X + DX, Y + DY, CX, CY);
        Sh := Sh + Parts[I];
      end;
      if not KeptXml(Xml) then
        Exit;
      Base := TextBoxesBefore(Xml, MaxInt);   { the new text boxes come after those it has }
      Result := PutShapeXml(Sh, Lbl);
      if Result and (SA <> nil) and (SA.Count > 0) then
      begin   { the text boxes' text as it was copied (the XML has it as it was read) }
        J := DrawingJson(FDoc, FShapeAt);
        if J <> nil then
          try
            Stories := nil;
            Items := J.Find('items') as TJSONArray;
            if Items <> nil then
              for I := 0 to Items.Count - 1 do
                if (Items[I] is TJSONObject) and (TJSONObject(Items[I]).Find('story') <> nil) then
                begin
                  SetLength(Stories, Length(Stories) + 1);
                  Stories[High(Stories)] := TJSONObject(Items[I]).Integers['story'];
                end;
          finally
            J.Free;
          end;
        for N := 0 to SA.Count - 1 do
          if Base + N <= High(Stories) then
            FillStory(Stories[Base + N], SA.Strings[N]);
      end;
    finally
      pd_doc_end_group(FDoc);
      Dec(FStepDepth);
    end;
    if Result then
    begin
      Changed;
      Invalidate;
    end
    else if pd_doc_inline_at(FDoc, FShapeAt, O) <> PD_OK then
      ClearShapeSelection;
  finally
    D.Free;
  end;
end;

function TParadeEdit.RemoveShapes(Whole: Boolean; const Lbl: string): Boolean;
var
  Xml: string;
  Els: TXmlEls;
  Tops: TIntegerArray;
  I: Integer;
begin
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) then
    Exit;
  if not KeptXml(Xml) then
  begin
    if Length(FShapeMore) = 0 then
      Result := DeleteShape;
    Exit;
  end;
  Els := XmlElements(Xml);
  Tops := TopShapeEls(Els, SelectedShapes, Whole);
  if Length(Tops) = 0 then
    Exit;
  for I := High(Tops) downto 0 do
    Delete(Xml, Els[Tops[I]].A, Els[Tops[I]].B - Els[Tops[I]].A);
  Result := ApplyKeptXml(Xml, Lbl, -1);
end;

function TParadeEdit.CanPasteShapes: Boolean;
begin
  RegisterFormats;
  Result := Clipboard.HasFormat(CF_Shapes);
end;

function TParadeEdit.CopyShapes: Boolean;
var
  S: string;
begin
  Result := ShapesPayload(S);
  if not Result then
    Exit;
  RegisterFormats;
  Clipboard.Open;
  try
    Clipboard.Clear;
    AddClip(CF_Shapes, S);
  finally
    Clipboard.Close;
  end;
end;

function TParadeEdit.CutShapes: Boolean;
begin
  Result := not FReadOnly and CopyShapes and RemoveShapes(False, 'Cut');
end;

function TParadeEdit.PasteShapes: Boolean;
var
  S: string;
begin
  RegisterFormats;
  Result := ReadClip(CF_Shapes, S) and PutShapesPayload(S, 'Paste');
end;

function TParadeEdit.DuplicateShapes: Boolean;
var
  S: string;
begin
  Result := ShapesPayload(S) and PutShapesPayload(S, 'Duplicate');
end;

function TParadeEdit.DeleteShapes: Boolean;
begin
  if Length(FShapeMore) = 0 then
    Result := DeleteShape
  else
    Result := RemoveShapes(False, 'Delete shape');
end;

function TParadeEdit.InsertCanvas(WPt: Double; HPt: Double): Boolean;
var
  J: TJSONObject;
  S, Xml: string;
  R0, R: pd_res_id;
  O: pd_inline;
  W, H: pd_sp;
  P: pd_pos;
begin
  Result := False;
  if FReadOnly then
    Exit;
  if WPt > 0 then
    W := Round(WPt * PD_SP_PER_PT)
  else
    W := TextWidthAt(CaretPos.block);
  if W <= 0 then
    W := 6 * 72 * PD_SP_PER_PT;
  if HPt > 0 then
    H := Round(HPt * PD_SP_PER_PT)
  else
    H := 3 * 72 * PD_SP_PER_PT;
  Xml := '<wpc:wpc><wpc:bg/><wpc:whole/></wpc:wpc>';
  J := TJSONObject.Create(['w', Integer(W), 'h', Integer(H), 'items', TJSONArray.Create, 'kind', 'wpc',
    'rels', TJSONObject.Create, 'xml', Xml]);
  try
    S := J.AsJSON;
  finally
    J.Free;
  end;
  pd_doc_begin_group(FDoc, 'Insert canvas');
  try
    if (pd_doc_add_resource(FDoc, 'application/vnd.parade.drawing+json', PAnsiChar(S), Length(S), R0) <> PD_OK) or
       (pd_docx_drawing_rebuild(FDoc, R0, PAnsiChar(Xml), Length(Xml), R) <> PD_OK) then
      Exit;
    FillChar(O, SizeOf(O), 0);
    O.kind := PD_INLINE_IMAGE;
    O.resource := R;
    O.width := W;
    O.height := H;
    DeleteSelection;
    P := CaretPos;
    InsertObject(O);
  finally
    pd_doc_end_group(FDoc);
  end;
  Changed;
  FShapeOn := True;
  FShapeAt := P;
  FShapeSid := -1;
  SetLength(FShapeMore, 0);
  SelectObject(P);
  Invalidate;
  Result := True;
end;

{ the selected drawing is a canvas made here or read from Word, shapes can go into }
function TParadeEdit.CanvasSelected: Boolean;
var
  Xml: string;
begin
  Result := KeptXml(Xml) and (Pos('<wpc:wpc', Xml) > 0);
end;

function TParadeEdit.InsertShape(const Kind: string): Boolean;
var
  Pg: Int32;
  X, Y, W, H, JW, JH: Double;
begin
  Result := False;
  if FReadOnly or (Kind = '') then
    Exit;
  EnsureCanvas;
  if FShapeOn and (FShapeSid >= 0) then
    FShapeSid := -1;    { into the drawing the shape is in }
  if FShapeOn and CanvasSelected then
  begin   { the next drag in the canvas draws it (a freeform or a curve: its clicks) }
    HoverCursor(crDefault);
    if FDrawKind = '' then
      FDrawCursor := Cursor;
    FDrawKind := Kind;
    SetLength(FDrawPts, 0);
    Cursor := crCross;
    Exit(True);
  end;
  if FShapeOn then
  begin   { not into the picture or drawing selected: after it }
    SetCaret(PdPos(FShapeAt.block, FShapeAt.offset + 3), False);
    ClearShapeSelection;
  end;
  if not InsertCanvas or not DrawingPlace(FShapeAt, Pg, X, Y, W, H, JW, JH) then
    Exit;
  if (Kind = 'curve') or (Kind = 'freeform') or (Kind = 'scribble') then
    Exit(InsertShape(Kind));    { drawn by hand, in the new canvas }
  { an inch and a half by one, in the middle (a line: across) }
  if (Kind = 'line') or (Kind = 'arrow') or (Kind = 'doubleArrow') then
    Result := AddShape(Kind, JW / 2 - 54 * PD_SP_PER_PT, JH / 2, JW / 2 + 54 * PD_SP_PER_PT, JH / 2)
  else
    Result := AddShape(Kind, JW / 2 - 54 * PD_SP_PER_PT, JH / 2 - 36 * PD_SP_PER_PT, JW / 2 + 54 * PD_SP_PER_PT,
      JH / 2 + 36 * PD_SP_PER_PT);
  if Result and (Kind = 'textbox') then
    EnterTextBox(FShapeSid, CaretPos, False, False);    { typed in at once, as Word has a new text box }
end;

{ a shape's DrawingML, as Word writes one in a canvas (EMU) }
{ the kinds of shape that are lines: Word's connectors, with arrowheads at none, one or both ends }
function LineKind(const Kind: string): Boolean;
begin
  Result := (Kind = 'line') or (Kind = 'arrow') or (Kind = 'doubleArrow') or (Kind = 'elbow') or
    (Kind = 'elbowArrow') or (Kind = 'elbowDoubleArrow') or (Kind = 'curvedConnector') or (Kind = 'curvedArrow') or
    (Kind = 'curvedDoubleArrow');
end;

{ a shape's DrawingML, as Word writes one in a canvas (EMU); its id written %ID% }
function ShapeXml(const Kind: string; X, Y, CX, CY: Int64; FlipH, FlipV: Boolean; const Fill, Line: string): string;
const
  Body = '<wps:bodyPr rot="0" vert="horz" wrap="square" lIns="91440" tIns="45720" rIns="91440" bIns="45720" ' +
    'anchor="ctr" anchorCtr="0"><a:noAutofit/></wps:bodyPr>';
var
  Xf, Name, Geom, Ends: string;
begin
  Xf := '<a:xfrm';
  if FlipH then Xf := Xf + ' flipH="1"';
  if FlipV then Xf := Xf + ' flipV="1"';
  Xf := Xf + '><a:off x="' + IntToStr(X) + '" y="' + IntToStr(Y) + '"/><a:ext cx="' + IntToStr(CX) + '" cy="' +
    IntToStr(CY) + '"/></a:xfrm>';
  if LineKind(Kind) then
  begin
    if Pos('elbow', Kind) = 1 then
    begin
      Geom := 'bentConnector3';
      Name := 'Connector: Elbow ';
    end
    else if Pos('curved', Kind) = 1 then
    begin
      Geom := 'curvedConnector3';
      Name := 'Connector: Curved ';
    end
    else
    begin
      Geom := 'straightConnector1';
      Name := 'Straight Connector ';
    end;
    Ends := '';
    if Pos('DoubleArrow', Kind) > 0 then
      Ends := '<a:headEnd type="triangle"/><a:tailEnd type="triangle"/>'
    else if (Kind = 'arrow') or (Pos('Arrow', Kind) > 0) then
      Ends := '<a:tailEnd type="triangle"/>';
    Exit('<wps:wsp><wps:cNvPr id="%ID%" name="' + Name + '%ID%"/><wps:cNvCnPr/><wps:spPr>' + Xf +
      '<a:prstGeom prst="' + Geom + '"><a:avLst/></a:prstGeom><a:ln w="12700"><a:solidFill><a:srgbClr ' +
      'val="' + Fill + '"/></a:solidFill>' + Ends + '</a:ln></wps:spPr><wps:bodyPr/></wps:wsp>');
  end;
  if Kind = 'textbox' then
    Exit('<wps:wsp><wps:cNvPr id="%ID%" name="Text Box %ID%"/><wps:cNvSpPr txBox="1"/><wps:spPr>' + Xf +
      '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom><a:solidFill>' +
      '<a:srgbClr val="FFFFFF"/></a:solidFill><a:ln w="6350"><a:solidFill><a:srgbClr val="000000"/></a:solidFill>' +
      '</a:ln></wps:spPr><wps:txbx><w:txbxContent><w:p/></w:txbxContent></wps:txbx>' +
      StringReplace(Body, 'anchor="ctr"', 'anchor="t"', []) + '</wps:wsp>');
  Geom := Kind;
  case Kind of
    'rect': Name := 'Rectangle ';
    'roundRect': Name := 'Rectangle: Rounded Corners ';
    'ellipse': Name := 'Oval ';
    'triangle': Name := 'Isosceles Triangle ';
    'diamond': Name := 'Diamond ';
    'rightArrow': Name := 'Arrow: Right ';
  else
    Name := 'Shape ';
  end;
  Result := '<wps:wsp><wps:cNvPr id="%ID%" name="' + Name + '%ID%"/><wps:cNvSpPr/><wps:spPr>' + Xf +
    '<a:prstGeom prst="' + Geom + '"><a:avLst/></a:prstGeom><a:solidFill>' +
    '<a:srgbClr val="' + Fill + '"/></a:solidFill><a:ln w="12700"><a:solidFill><a:srgbClr val="' + Line +
    '"/></a:solidFill>' +
    '</a:ln></wps:spPr>' + Body + '</wps:wsp>';
end;

{ a freeform's DrawingML: its path (EMU from its box's corner, the box CX by CY at X, Y), filled when closed }
function PathShapeXml(const C: TPathCmds; X, Y, CX, CY: Int64; Closed: Boolean; const Fill, Line: string): string;
begin
  Result := '<wps:wsp><wps:cNvPr id="%ID%" name="Freeform: Shape %ID%"/><wps:cNvSpPr/><wps:spPr><a:xfrm><a:off x="' +
    IntToStr(X) + '" y="' + IntToStr(Y) + '"/><a:ext cx="' + IntToStr(CX) + '" cy="' + IntToStr(CY) + '"/></a:xfrm>' +
    CustGeomXml(C, Max(CX, 1), Max(CY, 1));
  if Closed then
    Result := Result + '<a:solidFill><a:srgbClr val="' + Fill + '"/></a:solidFill><a:ln w="12700"><a:solidFill>' +
      '<a:srgbClr val="' + Line + '"/></a:solidFill></a:ln>'
  else
    Result := Result + '<a:noFill/><a:ln w="12700"><a:solidFill><a:srgbClr val="' + Fill + '"/></a:solidFill></a:ln>';
  Result := Result + '</wps:spPr><wps:bodyPr/></wps:wsp>';
end;

{ a path's points kept fewer, none of those left out farther than Tol from the line kept (Douglas and Peucker) }
function SimplifyPts(const P: array of Double; Tol: Double): TPathFlat;
var
  N, I: Integer;
  Keep: array of Boolean;

  procedure Span(A, B: Integer);
  var
    K, Far: Integer;
    D, Best, DX, DY, L: Double;
  begin
    if B <= A + 1 then
      Exit;
    DX := P[2 * B] - P[2 * A];
    DY := P[2 * B + 1] - P[2 * A + 1];
    L := Sqrt(DX * DX + DY * DY);
    Best := -1;
    Far := -1;
    for K := A + 1 to B - 1 do
    begin
      if L > 0 then
        D := Abs(DY * (P[2 * K] - P[2 * A]) - DX * (P[2 * K + 1] - P[2 * A + 1])) / L
      else
        D := Sqrt(Sqr(P[2 * K] - P[2 * A]) + Sqr(P[2 * K + 1] - P[2 * A + 1]));
      if D > Best then
      begin
        Best := D;
        Far := K;
      end;
    end;
    if Best > Tol then
    begin
      Keep[Far] := True;
      Span(A, Far);
      Span(Far, B);
    end;
  end;

begin
  Result := nil;
  N := Length(P) div 2;
  if N = 0 then
    Exit;
  SetLength(Keep, N);
  Keep[0] := True;
  Keep[N - 1] := True;
  Span(0, N - 1);
  for I := 0 to N - 1 do
    if Keep[I] then
    begin
      SetLength(Result, Length(Result) + 2);
      Result[High(Result) - 1] := P[2 * I];
      Result[High(Result)] := P[2 * I + 1];
    end;
end;

{ a smooth curve through points (a Catmull-Rom spline as cubic curves), round to the first again when Closed }
function SplineCmds(const P: array of Double; Closed: Boolean): TPathCmds;
var
  N, I, Segs: Integer;

  function Pt(K: Integer; Y: Boolean): Double;
  begin
    if Closed then
      K := (K mod N + N) mod N
    else
      K := EnsureRange(K, 0, N - 1);
    Result := P[2 * K + Ord(Y)];
  end;

begin
  Result := nil;
  N := Length(P) div 2;
  if N < 2 then
    Exit;
  AddCmd(Result, 'm', [P[0], P[1]]);
  if Closed then Segs := N else Segs := N - 1;
  for I := 0 to Segs - 1 do
    AddCmd(Result, 'c', [Pt(I, False) + (Pt(I + 1, False) - Pt(I - 1, False)) / 6,
      Pt(I, True) + (Pt(I + 1, True) - Pt(I - 1, True)) / 6,
      Pt(I + 1, False) - (Pt(I + 2, False) - Pt(I, False)) / 6,
      Pt(I + 1, True) - (Pt(I + 2, True) - Pt(I, True)) / 6, Pt(I + 1, False), Pt(I + 1, True)]);
  if Closed then
    AddCmd(Result, 'z', []);
end;

{ ---------------- connectors that stay on the shapes they join ---------------- }

type
  { where a connector's end can go on a shape (its cxnLst): in the canvas (EMU), and the way out of the shape
    (degrees, clockwise from right) }
  TParadeSite = record
    X, Y, Ang: Double;
  end;
  TParadeSites = array of TParadeSite;

{ the drawing's top: the canvas (or the group the drawing is); -1 if none }
function TopOfDrawing(const Els: TXmlEls): Integer;
var
  I: Integer;
begin
  Result := -1;
  for I := 0 to High(Els) do
    if (Els[I].Name = 'wpc:wpc') or (Els[I].Name = 'wpg:wgp') then
      Exit(I);
end;

{ a shape's own id (its cNvPr's) }
function ElId(const Xml: string; const Els: TXmlEls; E: Integer): string;
var
  I: Integer;
begin
  Result := '';
  for I := E + 1 to High(Els) do
    if Els[I].A >= Els[E].B then
      Break
    else if (Els[I].Parent = E) and ((Els[I].Name = 'wps:cNvPr') or (Els[I].Name = 'pic:cNvPr') or
       (Els[I].Name = 'wpg:cNvPr')) then
      Exit(AttrOf(Xml, Els[I].A, 'id'));
end;

{ a shape of the canvas's top: where connectors can be joined to it (Office's sites for its preset; the middles of
  its sides for any other), in the canvas's EMU as it is turned and flipped }
function ElSites(const Xml: string; const Els: TXmlEls; E: Integer): TParadeSites;
var
  Pr, Xf, Off, Ext, Pg, Av, I, N: Integer;
  OX, OY, CX, CY, Rot, U, V, C, S: Double;
  FH, FV: Boolean;
  Prst, Adj, F: string;
  J: TJSONObject;
  Sites: TJSONArray;
  Loc: array of Double;
begin
  Result := nil;
  Pr := PropsOf(Els, E);
  if Pr < 0 then
    Exit;
  Xf := ChildNamed(Els, Pr, 'a:xfrm');
  if Xf < 0 then
    Exit;
  Off := ChildNamed(Els, Xf, 'a:off');
  Ext := ChildNamed(Els, Xf, 'a:ext');
  if (Off < 0) or (Ext < 0) then
    Exit;
  OX := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'x'), 0);
  OY := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'y'), 0);
  CX := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cx'), 0);
  CY := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cy'), 0);
  Rot := StrToFloatDef(AttrOf(Xml, Els[Xf].A, 'rot'), 0) / 60000;
  FH := AttrOf(Xml, Els[Xf].A, 'flipH') = '1';
  FV := AttrOf(Xml, Els[Xf].A, 'flipV') = '1';
  Loc := nil;
  Pg := ChildNamed(Els, Pr, 'a:prstGeom');
  if (Pg >= 0) and (CX > 0) and (CY > 0) then
  begin
    Prst := AttrOf(Xml, Els[Pg].A, 'prst');
    Adj := '';
    Av := ChildNamed(Els, Pg, 'a:avLst');
    if Av >= 0 then
      for I := Av + 1 to High(Els) do
        if (Els[I].Parent = Av) and (Els[I].Name = 'a:gd') then
        begin
          F := AttrOf(Xml, Els[I].A, 'fmla');
          if Copy(F, 1, 4) = 'val ' then
            Adj := Adj + IfThen(Adj <> '', ' ', '') + AttrOf(Xml, Els[I].A, 'name') + '=' + Copy(F, 5, MaxInt);
        end;
    J := PresetJson(Prst, CX, CY, Adj);
    if J <> nil then
      try
        Sites := J.Find('sites') as TJSONArray;
        if Sites <> nil then
          for I := 0 to Sites.Count - 1 do
            if (Sites[I] is TJSONArray) and (TJSONArray(Sites[I]).Count >= 3) then
            begin
              N := Length(Loc);
              SetLength(Loc, N + 3);
              Loc[N] := TJSONArray(Sites[I])[0].AsFloat;
              Loc[N + 1] := TJSONArray(Sites[I])[1].AsFloat;
              Loc[N + 2] := TJSONArray(Sites[I])[2].AsFloat / 60000;
            end;
      finally
        J.Free;
      end;
  end;
  if Length(Loc) = 0 then
  begin   { the middles of its sides: top, left, bottom, right, as a rectangle's }
    SetLength(Loc, 12);
    Loc[0] := CX / 2; Loc[1] := 0; Loc[2] := 270;
    Loc[3] := 0; Loc[4] := CY / 2; Loc[5] := 180;
    Loc[6] := CX / 2; Loc[7] := CY; Loc[8] := 90;
    Loc[9] := CX; Loc[10] := CY / 2; Loc[11] := 0;
  end;
  SetLength(Result, Length(Loc) div 3);
  C := Cos(Rot * Pi / 180);
  S := Sin(Rot * Pi / 180);
  for I := 0 to High(Result) do
  begin
    U := Loc[3 * I] - CX / 2;
    V := Loc[3 * I + 1] - CY / 2;
    Result[I].Ang := Loc[3 * I + 2];
    if FH then
    begin
      U := -U;
      Result[I].Ang := 180 - Result[I].Ang;
    end;
    if FV then
    begin
      V := -V;
      Result[I].Ang := -Result[I].Ang;
    end;
    Result[I].X := OX + CX / 2 + U * C - V * S;
    Result[I].Y := OY + CY / 2 + U * S + V * C;
    U := 1;
    V := 1;
    MapUp(Xml, Els, E, Result[I].X, Result[I].Y, U, V);    { in a group: in the canvas's units }
    Result[I].Ang := Result[I].Ang + Rot;
    Result[I].Ang := Result[I].Ang - 360 * Floor(Result[I].Ang / 360);
  end;
end;

{ a connector's ends (EMU of the canvas), from its box and flips }
function ConnectorEnds(const Xml: string; const Els: TXmlEls; E: Integer; out X1, Y1, X2, Y2: Double): Boolean;
var
  Pr, Xf, Off, Ext: Integer;
  OX, OY, CX, CY, Rot, MX, MY, DX, DY, C, S: Double;
begin
  Result := False;
  Pr := PropsOf(Els, E);
  Xf := -1;
  if Pr >= 0 then
    Xf := ChildNamed(Els, Pr, 'a:xfrm');
  if Xf < 0 then
    Exit;
  Off := ChildNamed(Els, Xf, 'a:off');
  Ext := ChildNamed(Els, Xf, 'a:ext');
  if (Off < 0) or (Ext < 0) then
    Exit;
  OX := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'x'), 0);
  OY := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'y'), 0);
  CX := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cx'), 0);
  CY := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cy'), 0);
  Rot := StrToFloatDef(AttrOf(Xml, Els[Xf].A, 'rot'), 0) / 60000;
  DX := CX / 2;
  DY := CY / 2;
  if AttrOf(Xml, Els[Xf].A, 'flipH') = '1' then DX := -DX;
  if AttrOf(Xml, Els[Xf].A, 'flipV') = '1' then DY := -DY;
  C := Cos(Rot * Pi / 180);
  S := Sin(Rot * Pi / 180);
  MX := OX + CX / 2;
  MY := OY + CY / 2;
  X1 := MX - (DX * C - DY * S);
  Y1 := MY - (DX * S + DY * C);
  X2 := MX + (DX * C - DY * S);
  Y2 := MY + (DX * S + DY * C);
  Result := True;
end;

{ a connector's box made to run from (X1, Y1) to (X2, Y2): its offset, extent and flips (an elbow or a curve between
  two ends that leave up or down: turned a quarter, to set off that way) }
function ConnectorXfrm(X1, Y1, X2, Y2: Double; Upright: Boolean): string;
var
  MX, MY, DX, DY, W, H: Double;
begin
  MX := (X1 + X2) / 2;
  MY := (Y1 + Y2) / 2;
  DX := X2 - X1;
  DY := Y2 - Y1;
  if Upright then
  begin   { turned 90 degrees: its own across is the canvas's down }
    W := Abs(DY);
    H := Abs(DX);
    Result := '<a:xfrm rot="5400000"' + IfThen(DY < 0, ' flipH="1"', '') + IfThen(DX > 0, ' flipV="1"', '');
  end
  else
  begin
    W := Abs(DX);
    H := Abs(DY);
    Result := '<a:xfrm' + IfThen(DX < 0, ' flipH="1"', '') + IfThen(DY < 0, ' flipV="1"', '');
  end;
  Result := Result + '><a:off x="' + IntToStr(Round(MX - W / 2)) + '" y="' + IntToStr(Round(MY - H / 2)) +
    '"/><a:ext cx="' + IntToStr(Max(1, Round(W))) + '" cy="' + IntToStr(Max(1, Round(H))) + '"/></a:xfrm>';
end;

{ the end of a connector joined (a:stCxn, its start; a:endCxn): the shape's id and the site's index; '' if not }
function CxnOf(const Xml: string; const Els: TXmlEls; E: Integer; const Name: string; out Idx: Integer): string;
var
  Nv, I: Integer;
begin
  Result := '';
  Idx := -1;
  Nv := ChildNamed(Els, E, 'wps:cNvCnPr');
  if Nv < 0 then
    Exit;
  I := ChildNamed(Els, Nv, Name);
  if I < 0 then
    Exit;
  Result := AttrOf(Xml, Els[I].A, 'id');
  Idx := StrToIntDef(AttrOf(Xml, Els[I].A, 'idx'), -1);
end;

{ the connectors of the canvas joined to shapes put where those shapes' sites are now; whether any moved }
function RouteConnectors(var Xml: string): Boolean;
var
  Els: TXmlEls;
  Root, I, K, E, Pr, Xf, SIdx, EIdx: Integer;
  SId, EId, Prst, NewXf: string;
  X1, Y1, X2, Y2: Double;
  Sites: TParadeSites;
  Done: array of Boolean;
  Upright: Boolean;
  SAng, EAng, AX, AY, BX, BY: Double;

  function Find(const Id: string): Integer;
  var
    Q: Integer;
  begin
    Result := -1;
    if Id = '' then
      Exit;
    for Q := 0 to High(Els) do
      if IsShapeEl(Els[Q].Name) and (ElId(Xml, Els, Q) = Id) then
        Exit(Q);
  end;

begin
  Result := False;
  if Pos('Cxn ', Xml) = 0 then
    Exit;
  Els := XmlElements(Xml);
  SetLength(Done, Length(Els));
  repeat
    Els := XmlElements(Xml);
    Root := TopOfDrawing(Els);
    E := -1;
    K := 0;
    for I := 0 to High(Els) do
      if (Root >= 0) and (Els[I].Name = 'wps:wsp') and (ChildNamed(Els, I, 'wps:cNvCnPr') >= 0) then
      begin
        if (K < Length(Done)) and not Done[K] then
        begin
          E := I;
          Done[K] := True;
          Break;
        end;
        Inc(K);
      end;
    if E < 0 then
      Break;
    SId := CxnOf(Xml, Els, E, 'a:stCxn', SIdx);
    EId := CxnOf(Xml, Els, E, 'a:endCxn', EIdx);
    if ((SId = '') and (EId = '')) or not ConnectorEnds(Xml, Els, E, X1, Y1, X2, Y2) then
      Continue;
    { a connector in a group: its group's units to the canvas's and back (X = AX * x + BX) }
    BX := 0; BY := 0; AX := 1; AY := 1;
    MapUp(Xml, Els, E, BX, BY, AX, AY);
    X1 := AX * X1 + BX; Y1 := AY * Y1 + BY;
    X2 := AX * X2 + BX; Y2 := AY * Y2 + BY;
    SAng := -1;
    EAng := -1;
    K := Find(SId);
    if K >= 0 then
    begin
      Sites := ElSites(Xml, Els, K);
      if (SIdx >= 0) and (SIdx <= High(Sites)) then
      begin
        X1 := Sites[SIdx].X; Y1 := Sites[SIdx].Y; SAng := Sites[SIdx].Ang;
      end;
    end;
    K := Find(EId);
    if K >= 0 then
    begin
      Sites := ElSites(Xml, Els, K);
      if (EIdx >= 0) and (EIdx <= High(Sites)) then
      begin
        X2 := Sites[EIdx].X; Y2 := Sites[EIdx].Y; EAng := Sites[EIdx].Ang;
      end;
    end;
    Pr := PropsOf(Els, E);
    Xf := ChildNamed(Els, Pr, 'a:xfrm');
    if Xf < 0 then
      Continue;
    Prst := '';
    if ChildNamed(Els, Pr, 'a:prstGeom') >= 0 then
      Prst := AttrOf(Xml, Els[ChildNamed(Els, Pr, 'a:prstGeom')].A, 'prst');
    Upright := (Pos('bent', Prst) = 1) or (Pos('curved', Prst) = 1);
    Upright := Upright and ((SAng < 0) or (Abs(Sin(SAng * Pi / 180)) > 0.7)) and
      ((EAng < 0) or (Abs(Sin(EAng * Pi / 180)) > 0.7)) and ((SAng >= 0) or (EAng >= 0));
    if (AX <= 0) or (AY <= 0) then
      Continue;
    NewXf := ConnectorXfrm((X1 - BX) / AX, (Y1 - BY) / AY, (X2 - BX) / AX, (Y2 - BY) / AY, Upright);
    if Copy(Xml, Els[Xf].A, Els[Xf].B - Els[Xf].A) <> NewXf then
    begin
      Xml := Copy(Xml, 1, Els[Xf].A - 1) + NewXf + Copy(Xml, Els[Xf].B, MaxInt);
      Result := True;
    end;
  until False;
end;

const
  EmuD = PD_SP_PER_PT / 12700;    { a canvas's units (sp) an EMU }

function TParadeEdit.SidId(const Xml: string; Sid: Integer): string;
var
  Els: TXmlEls;
  E: Integer;
begin
  Result := '';
  Els := XmlElements(Xml);
  E := SidElement(Els, Sid);
  if E >= 0 then
    Result := ElId(Xml, Els, E);
end;

function TParadeEdit.ShapeSites(Sid: Integer): TParadeDrawPoints;
var
  Xml: string;
  Els: TXmlEls;
  E, I: Integer;
  S: TParadeSites;
begin
  Result := nil;
  if not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, Sid);
  if (E < 0) or (ChildNamed(Els, E, 'wps:cNvCnPr') >= 0) then
    Exit;
  S := ElSites(Xml, Els, E);
  SetLength(Result, Length(S));
  for I := 0 to High(S) do
  begin
    Result[I].X := S[I].X * EmuD;
    Result[I].Y := S[I].Y * EmuD;
  end;
end;

function TParadeEdit.NearestSite(X, Y, Skip: Integer; out Sid, Site: Integer; out DX, DY: Double): Boolean;
var
  Xml: string;
  Els: TXmlEls;
  M: TParadeDrawMap;
  Root, E, I, K: Integer;
  S: TParadeSites;
  P: TPoint;
  Best, D: Double;
begin
  Result := False;
  Sid := -1;
  Site := -1;
  DX := 0;
  DY := 0;
  if not FShapeOn or not KeptXml(Xml) or not DrawMap(M) then
    Exit;
  Els := XmlElements(Xml);
  Root := TopOfDrawing(Els);
  Best := 10;     { pixels }
  K := -1;
  for E := 0 to High(Els) do
  begin
    if (Els[E].Name = 'wps:wsp') or (Els[E].Name = 'pic:pic') then
      Inc(K);
    if (Root < 0) or not ((Els[E].Name = 'wps:wsp') or (Els[E].Name = 'pic:pic')) or (K = Skip) or
       (ChildNamed(Els, E, 'wps:cNvCnPr') >= 0) then
      Continue;
    S := ElSites(Xml, Els, E);
    for I := 0 to High(S) do
    begin
      P := MapToClient(M, S[I].X * EmuD, S[I].Y * EmuD);
      D := Hypot(P.X - X, P.Y - Y);
      if D <= Best then
      begin
        Best := D;
        Sid := K;
        Site := I;
        DX := S[I].X * EmuD;
        DY := S[I].Y * EmuD;
        Result := True;
      end;
    end;
  end;
end;

function TParadeEdit.AddPictureShape(Res: pd_res_id; W, H: pd_sp): Boolean;
var
  J: TJSONObject;
  Pg: Int32;
  PX, PY, DW, DH, JW, JH, K: Double;
  Rid: string;
begin
  Result := False;
  if FReadOnly or not FShapeOn or not CanvasSelected or (W <= 0) or (H <= 0) or
     not DrawingPlace(FShapeAt, Pg, PX, PY, DW, DH, JW, JH) then
    Exit;
  K := Min(1, Min(JW * 0.9 / W, JH * 0.9 / H));
  Rid := 'rIdPd' + IntToStr(Res);
  PushShapeStep('Insert picture', FShapeAt);
  Inc(FStepDepth);
  pd_doc_begin_group(FDoc, 'Insert picture');
  try
    J := DrawingJson(FDoc, FShapeAt);     { the picture's file, the canvas's own by an id of its own }
    if J = nil then
      Exit;
    try
      if not (J.Find('rels') is TJSONObject) then
        J.Add('rels', TJSONObject.Create);
      TJSONObject(J.Find('rels')).Delete(Rid);
      TJSONObject(J.Find('rels')).Add(Rid, Integer(Res));
      if not ReplaceDrawing(FShapeAt, J.AsJSON, 'Insert picture') then
        Exit;
    finally
      J.Free;
    end;
    Result := PutShapeXml('<pic:pic><pic:nvPicPr><pic:cNvPr id="%ID%" name="Picture %ID%"/><pic:cNvPicPr/>' +
      '</pic:nvPicPr><pic:blipFill><a:blip r:embed="' + Rid + '"/><a:stretch><a:fillRect/></a:stretch>' +
      '</pic:blipFill><pic:spPr><a:xfrm><a:off x="' + IntToStr(Round((JW - W * K) / 2 / EmuD)) + '" y="' +
      IntToStr(Round((JH - H * K) / 2 / EmuD)) + '"/><a:ext cx="' + IntToStr(Round(W * K / EmuD)) + '" cy="' +
      IntToStr(Round(H * K / EmuD)) + '"/></a:xfrm><a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr>' +
      '</pic:pic>', 'Insert picture');
  finally
    pd_doc_end_group(FDoc);
    Dec(FStepDepth);
  end;
end;

function TParadeEdit.AddConnector(const Kind: string; X1, Y1, X2, Y2: Double; StartSid: Integer;
  StartSite: Integer; EndSid: Integer; EndSite: Integer): Boolean;
var
  Xml, Sh, Cx, Id: string;
begin
  Result := False;
  if not LineKind(Kind) or not FShapeOn or not KeptXml(Xml) then
    Exit;
  Cx := '';
  if (StartSid >= 0) and (StartSite >= 0) then
  begin
    Id := SidId(Xml, StartSid);
    if Id <> '' then
      Cx := Cx + '<a:stCxn id="' + Id + '" idx="' + IntToStr(StartSite) + '"/>';
  end;
  if (EndSid >= 0) and (EndSite >= 0) then
  begin
    Id := SidId(Xml, EndSid);
    if Id <> '' then
      Cx := Cx + '<a:endCxn id="' + Id + '" idx="' + IntToStr(EndSite) + '"/>';
  end;
  Sh := ShapeXml(Kind, Round(Min(X1, X2) / EmuD), Round(Min(Y1, Y2) / EmuD), Max(1, Round(Abs(X2 - X1) / EmuD)),
    Max(1, Round(Abs(Y2 - Y1) / EmuD)), X2 < X1, Y2 < Y1, HexRGB(FShapeFillDef), HexRGB(FShapeLineDef));
  if Cx <> '' then
    Sh := StringReplace(Sh, '<wps:cNvCnPr/>', '<wps:cNvCnPr>' + Cx + '</wps:cNvCnPr>', []);
  Result := PutShapeXml(Sh, 'Insert shape');
end;

function TParadeEdit.ConnectorInfo(out X1, Y1, X2, Y2: Double; out StartSid, EndSid: Integer): Boolean;
var
  Xml, Id: string;
  Els: TXmlEls;
  E, I, K, Idx: Integer;
  AX, AY, BX, BY: Double;
begin
  Result := False;
  X1 := 0; Y1 := 0; X2 := 0; Y2 := 0;
  StartSid := -1;
  EndSid := -1;
  if not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  if (E < 0) or (ChildNamed(Els, E, 'wps:cNvCnPr') < 0) or not ConnectorEnds(Xml, Els, E, X1, Y1, X2, Y2) then
    Exit;
  BX := 0; BY := 0; AX := 1; AY := 1;
  MapUp(Xml, Els, E, BX, BY, AX, AY);    { in a group: in the canvas's units }
  X1 := (AX * X1 + BX) * EmuD; Y1 := (AY * Y1 + BY) * EmuD;
  X2 := (AX * X2 + BX) * EmuD; Y2 := (AY * Y2 + BY) * EmuD;
  for K := 0 to 1 do
  begin
    if K = 0 then
      Id := CxnOf(Xml, Els, E, 'a:stCxn', Idx)
    else
      Id := CxnOf(Xml, Els, E, 'a:endCxn', Idx);
    if Id = '' then
      Continue;
    for I := 0 to 100000 do
    begin
      if SidElement(Els, I) < 0 then
        Break;
      if ElId(Xml, Els, SidElement(Els, I)) = Id then
      begin
        if K = 0 then StartSid := I else EndSid := I;
        Break;
      end;
    end;
  end;
  Result := True;
end;

function TParadeEdit.MoveConnectorEnd(End_: Boolean; X, Y: Double; Sid: Integer; Site: Integer): Boolean;
var
  Xml, Nv, Keep, Id: string;
  Els: TXmlEls;
  E, Pr, Xf, NvE, I, Idx: Integer;
  X1, Y1, X2, Y2, AX, AY, BX, BY: Double;
  SId_, EId_: string;
  SIdx, EIdx: Integer;
begin
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  NvE := -1;
  if E >= 0 then
    NvE := ChildNamed(Els, E, 'wps:cNvCnPr');
  if (NvE < 0) or not ConnectorEnds(Xml, Els, E, X1, Y1, X2, Y2) then
    Exit;
  SId_ := CxnOf(Xml, Els, E, 'a:stCxn', SIdx);
  EId_ := CxnOf(Xml, Els, E, 'a:endCxn', EIdx);
  Id := '';
  if (Sid >= 0) and (Site >= 0) then
    Id := SidId(Xml, Sid);
  BX := 0; BY := 0; AX := 1; AY := 1;
  MapUp(Xml, Els, E, BX, BY, AX, AY);    { the canvas's units into its group's }
  if End_ then
  begin
    X2 := (X / EmuD - BX) / AX; Y2 := (Y / EmuD - BY) / AY; EId_ := Id; EIdx := Site;
  end
  else
  begin
    X1 := (X / EmuD - BX) / AX; Y1 := (Y / EmuD - BY) / AY; SId_ := Id; SIdx := Site;
  end;
  { what the connector's own properties keep (its locks), and its ends' joins as they are now }
  Keep := '';
  for I := NvE + 1 to High(Els) do
    if (Els[I].Parent = NvE) and (Els[I].Name = 'a:cxnSpLocks') then
      Keep := Copy(Xml, Els[I].A, Els[I].B - Els[I].A);
  Nv := '<wps:cNvCnPr>' + Keep;
  if SId_ <> '' then
    Nv := Nv + '<a:stCxn id="' + SId_ + '" idx="' + IntToStr(SIdx) + '"/>';
  if EId_ <> '' then
    Nv := Nv + '<a:endCxn id="' + EId_ + '" idx="' + IntToStr(EIdx) + '"/>';
  Nv := Nv + '</wps:cNvCnPr>';
  Pr := PropsOf(Els, E);
  Xf := ChildNamed(Els, Pr, 'a:xfrm');
  if Xf < 0 then
    Exit;
  { the box first (it comes after the joins in the XML), then the joins }
  Xml := Copy(Xml, 1, Els[Xf].A - 1) + ConnectorXfrm(X1, Y1, X2, Y2, False) + Copy(Xml, Els[Xf].B, MaxInt);
  Xml := Copy(Xml, 1, Els[NvE].A - 1) + Nv + Copy(Xml, Els[NvE].B, MaxInt);
  Result := ApplyKeptXml(Xml, 'Connector', FShapeSid);
end;

function TParadeEdit.AddShape(const Kind: string; X0, Y0, X1, Y1: Double; FlipH: Boolean;
  FlipV: Boolean): Boolean;
const
  Emu = 12700 / 65536;    { sp to EMU }
begin
  Result := PutShapeXml(ShapeXml(Kind, Round(X0 * Emu), Round(Y0 * Emu), Max(1, Round((X1 - X0) * Emu)),
    Round((Y1 - Y0) * Emu), FlipH, FlipV, HexRGB(FShapeFillDef), HexRGB(FShapeLineDef)));
end;

{ a shape's DrawingML (its id %ID%; more shapes: %ID0%, %ID1%, ...) put last into the selected canvas, and
  selected (more shapes: each one at the canvas's top) }
function TParadeEdit.PutShapeXml(const Sh: string; const Lbl: string): Boolean;
var
  Xml, T: string;
  Els: TXmlEls;
  I, K, Id, P, L, At, Root, NR: Integer;
  More: array of Integer;
begin
  Result := False;
  if FShapeOn and (FShapeSid >= 0) then
    FShapeSid := -1;
  if not FShapeOn or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  Root := -1;
  for I := 0 to High(Els) do
    if Els[I].Name = 'wpc:wpc' then
    begin
      Root := I;
      Break;
    end;
  if Root < 0 then
    Exit;
  Id := 1;    { an id none of the drawing's shapes has }
  P := Pos('id="', Xml);
  while P > 0 do
  begin
    L := PosEx('"', Xml, P + 4);
    if L > 0 then
      Id := Max(Id, StrToIntDef(Copy(Xml, P + 4, L - P - 4), 0) + 1);
    P := PosEx('id="', Xml, P + 4);
  end;
  T := StringReplace(Sh, '%ID%', IntToStr(Id), [rfReplaceAll]);
  K := 0;
  while Pos('%ID' + IntToStr(K) + '%', T) > 0 do
  begin
    T := StringReplace(T, '%ID' + IntToStr(K) + '%', IntToStr(Id + 1 + K), [rfReplaceAll]);
    Inc(K);
  end;
  if Copy(Xml, Els[Root].B - 2, 2) = '/>' then
  begin   { <wpc:wpc/>: opened }
    At := Els[Root].A;
    Xml := Copy(Xml, 1, At - 1) + '<wpc:wpc>' + T + '</wpc:wpc>' + Copy(Xml, Els[Root].B, MaxInt);
    At := At + 9;
  end
  else
  begin   { last: drawn over the others }
    At := CloseOf(Xml, Els[Root]);
    Insert(T, Xml, At);
  end;
  Els := XmlElements(Xml);
  More := nil;
  NR := -1;
  for I := 0 to High(Els) do
    if Els[I].Name = 'wpc:wpc' then
    begin
      NR := I;
      Break;
    end;
  for I := 0 to High(Els) do     { the shapes put in: each one's first shape }
    if (Els[I].Parent = NR) and (Els[I].A >= At) and (Els[I].A < At + Length(T)) and IsShapeEl(Els[I].Name) then
    begin
      SetLength(More, Length(More) + 1);
      More[High(More)] := SidAt(Els, Els[I].A);
    end;
  Result := ApplyKeptXml(Xml, Lbl, SidAt(Els, At));
  if Result and (Length(More) > 1) then
  begin
    SetLength(FShapeMore, Length(More) - 1);
    for I := 1 to High(More) do
      FShapeMore[I - 1] := More[I];
  end;
end;

{ a freeform put into the selected canvas: a path of the drawing's units (sp), filled when closed }
function TParadeEdit.AddPathShape(const Pts: array of Double; Curve, Closed: Boolean): Boolean;
const
  Emu = 12700 / 65536;
var
  C: TPathCmds;
  I, K: Integer;
  X0, Y0, X1, Y1: Double;
  Rel: TPathFlat;
begin
  Result := False;
  if Length(Pts) < 4 then
    Exit;
  X0 := 1e300; Y0 := 1e300; X1 := -1e300; Y1 := -1e300;
  SetLength(Rel, Length(Pts));
  for I := 0 to Length(Pts) div 2 - 1 do
  begin
    X0 := Min(X0, Pts[2 * I]); X1 := Max(X1, Pts[2 * I]);
    Y0 := Min(Y0, Pts[2 * I + 1]); Y1 := Max(Y1, Pts[2 * I + 1]);
  end;
  if Curve then
  begin   { the curve's own reach: its controls may go past its points }
    C := SplineCmds(Pts, Closed);
    for I := 0 to High(C) do
      for K := 0 to C[I].N - 1 do
      begin
        X0 := Min(X0, C[I].X[K]); X1 := Max(X1, C[I].X[K]);
        Y0 := Min(Y0, C[I].Y[K]); Y1 := Max(Y1, C[I].Y[K]);
      end;
    for I := 0 to High(C) do
      for K := 0 to C[I].N - 1 do
      begin
        C[I].X[K] := (C[I].X[K] - X0) * Emu;
        C[I].Y[K] := (C[I].Y[K] - Y0) * Emu;
      end;
  end
  else
  begin
    for I := 0 to Length(Pts) div 2 - 1 do
      if I = 0 then
        AddCmd(C, 'm', [(Pts[0] - X0) * Emu, (Pts[1] - Y0) * Emu])
      else
        AddCmd(C, 'l', [(Pts[2 * I] - X0) * Emu, (Pts[2 * I + 1] - Y0) * Emu]);
    if Closed then
      AddCmd(C, 'z', []);
  end;
  Result := PutShapeXml(PathShapeXml(C, Round(X0 * Emu), Round(Y0 * Emu), Max(1, Round((X1 - X0) * Emu)),
    Max(1, Round((Y1 - Y0) * Emu)), Closed, HexRGB(FShapeFillDef), HexRGB(FShapeLineDef)));
end;

function TParadeEdit.SetShapeBox(Sid: Integer; X0, Y0, X1, Y1: Double): Boolean;
var
  Routed: string;
  J, It, Mk: TJSONObject;
  Items: TJSONArray;
  I, K, Mi, Me: Integer;
  OX0, OY0, OX1, OY1, KX, KY, FSX, FSY, C: Double;
  Pts, Box, Fs: TJSONArray;
  Xml: string;

  function Map(V, O0, K, N0: Double): Int64;
  begin
    Result := Round(N0 + (V - O0) * K);
  end;

begin
  Result := False;
  if not FShapeOn or not ShapeBox(Sid, OX0, OY0, OX1, OY1) or (OX1 - OX0 <= 0) or (OY1 - OY0 <= 0) or
     (X1 - X0 < 1) or (Y1 - Y0 < 1) then
    Exit;
  KX := (X1 - X0) / (OX1 - OX0);
  KY := (Y1 - Y0) / (OY1 - OY0);
  J := DrawingJson(FDoc, FShapeAt);
  if J = nil then
    Exit;
  try
    Items := J.Find('items') as TJSONArray;
    Mi := -1;
    Me := Items.Count;
    for I := 0 to Items.Count - 1 do
      if (Items[I] is TJSONObject) and (TJSONObject(Items[I]).Find('sid') <> nil) then
        if TJSONObject(Items[I]).Integers['sid'] = Sid then
          Mi := I
        else if (Mi >= 0) and (I > Mi) and (Me = Items.Count) then
          Me := I;
    if Mi < 0 then
      Exit;
    Mk := TJSONObject(Items[Mi]);
    { its pieces: every place and size, every point of a path, from the old box to the new }
    for I := Mi + 1 to Me - 1 do
      if Items[I] is TJSONObject then
      begin
        It := TJSONObject(Items[I]);
        if It.Find('x') <> nil then It.Integers['x'] := Map(It.Get('x', 0.0), OX0, KX, X0);
        if It.Find('y') <> nil then It.Integers['y'] := Map(It.Get('y', 0.0), OY0, KY, Y0);
        if It.Find('w') <> nil then It.Integers['w'] := Round(It.Get('w', 0.0) * KX);
        if It.Find('h') <> nil then It.Integers['h'] := Round(It.Get('h', 0.0) * KY);
        Pts := It.Find('path') as TJSONArray;
        if Pts <> nil then
        begin
          K := 0;
          while K + 1 < Pts.Count do
          begin
            if Pts[K].AsInt64 <> -2147483648 then   { not a break between rings }
            begin
              Pts.Items[K] := TJSONIntegerNumber.Create(Map(Pts[K].AsFloat, OX0, KX, X0));
              Pts.Items[K + 1] := TJSONIntegerNumber.Create(Map(Pts[K + 1].AsFloat, OY0, KY, Y0));
            end;
            Inc(K, 2);
          end;
        end;
      end;
    Box := Mk.Find('box') as TJSONArray;
    Box.Items[0] := TJSONIntegerNumber.Create(Round(X0));
    Box.Items[1] := TJSONIntegerNumber.Create(Round(Y0));
    Box.Items[2] := TJSONIntegerNumber.Create(Round(X1));
    Box.Items[3] := TJSONIntegerNumber.Create(Round(Y1));
    { the shape as Word has it: its offset by the move, its extent by the scale, in its group's units }
    if (J.Find('xml') <> nil) and (J.Find('xml').JSONType = jtString) then
    begin
      Fs := Mk.Find('fs') as TJSONArray;
      FSX := 1; FSY := 1;
      if (Fs <> nil) and (Fs.Count >= 2) and (Fs[0].AsFloat > 0) and (Fs[1].AsFloat > 0) then
      begin
        FSX := Fs[0].AsFloat / 1000000;
        FSY := Fs[1].AsFloat / 1000000;
      end;
      C := 12700 / PD_SP_PER_PT;   { EMU a sp }
      Xml := J.Strings['xml'];
      PatchKeptXml(Xml, Sid, (X0 - OX0) * C / FSX, (Y0 - OY0) * C / FSY, KX, KY);
      J.Strings['xml'] := Xml;
      Routed := Xml;
      if RouteConnectors(Routed) then
      begin   { connectors joined to it: the drawing made again from the XML, with them moved too }
        Result := ApplyKeptXml(MarkTextBoxes(Routed), 'Shape', Sid);
        Exit;
      end;
    end;
    Result := ReplaceDrawing(FShapeAt, J.AsJSON, 'Shape');
    FShapeOn := True;     { the same shape, still selected }
    FShapeSid := Sid;
  finally
    J.Free;
  end;
end;

function TParadeEdit.DeleteShape: Boolean;
var
  J, It: TJSONObject;
  Items: TJSONArray;
  I, Mi, Me, Sid, A, B: Integer;
  Xml: string;
begin
  Result := False;
  if not FShapeOn or (FShapeSid < 0) then
    Exit;
  Sid := FShapeSid;
  J := DrawingJson(FDoc, FShapeAt);
  if J = nil then
    Exit;
  try
    Items := J.Find('items') as TJSONArray;
    Mi := -1;
    Me := Items.Count;
    for I := 0 to Items.Count - 1 do
      if (Items[I] is TJSONObject) and (TJSONObject(Items[I]).Find('sid') <> nil) then
        if TJSONObject(Items[I]).Integers['sid'] = Sid then
          Mi := I
        else if (Mi >= 0) and (I > Mi) and (Me = Items.Count) then
          Me := I;
    if Mi < 0 then
      Exit;
    for I := Me - 1 downto Mi do
      Items.Delete(I);
    for I := 0 to Items.Count - 1 do     { the shapes after it are one place earlier in the XML now }
      if Items[I] is TJSONObject then
      begin
        It := TJSONObject(Items[I]);
        if (It.Find('sid') <> nil) and (It.Integers['sid'] > Sid) then
          It.Integers['sid'] := It.Integers['sid'] - 1;
      end;
    if (J.Find('xml') <> nil) and (J.Find('xml').JSONType = jtString) then
    begin
      Xml := J.Strings['xml'];
      if KeptShapeSpan(Xml, Sid, A, B) then
      begin
        Delete(Xml, A, B - A);
        J.Strings['xml'] := Xml;
      end;
    end;
    Result := ReplaceDrawing(FShapeAt, J.AsJSON, 'Delete shape');
    FShapeOn := True;     { the drawing, still selected }
    FShapeSid := -1;
    Invalidate;
  finally
    J.Free;
  end;
end;

function TParadeEdit.ShapeGeom(Sid: Integer; out G: TParadeShapeGeom): Boolean;
var
  Xml, V, Key: string;
  Els: TXmlEls;
  E, Pr, Xf, Ext, Pg, Av, I, K: Integer;
  J, It: TJSONObject;
  Items, B, Fs: TJSONArray;
  KX, KY: Double;
  Found: Boolean;
begin
  Result := False;
  G.Sid := Sid;
  G.CX := 0; G.CY := 0; G.W := 0; G.H := 0; G.EX := 0; G.EY := 0; G.Rot := 0;
  G.FlipH := False; G.FlipV := False; G.Prst := ''; G.Pic := False; G.TextBox := False;
  G.KX := 1; G.KY := 1;
  SetLength(G.AdjN, 0);
  SetLength(G.AdjV, 0);
  SetLength(G.HX, 0);
  SetLength(G.HY, 0);
  SetLength(G.HIdx, 0);
  if not FShapeOn or (Sid < 0) then
    Exit;
  Key := Format('%d:%d:%d:%d', [FShapeAt.block, FShapeAt.offset, Sid, pd_doc_revision(FDoc)]);
  if Key = FGeomKey then
  begin
    G := FGeomLast;
    Exit(FGeomOk);
  end;
  FGeomKey := Key;
  FGeomOk := False;
  FGeomLast := G;
  if not KeptXml(Xml) then
    Exit;
  { its centre (its box's, turned or not) and its group's scale, from the description }
  Found := False;
  KX := PD_SP_PER_PT / 12700;
  KY := KX;
  J := DrawingJson(FDoc, FShapeAt);
  if J = nil then
    Exit;
  try
    Items := J.Find('items') as TJSONArray;
    if Items <> nil then
      for I := 0 to Items.Count - 1 do
        if (Items[I] is TJSONObject) and (TJSONObject(Items[I]).Find('sid') <> nil) and
           (TJSONObject(Items[I]).Integers['sid'] = Sid) then
        begin
          It := TJSONObject(Items[I]);
          B := It.Find('box') as TJSONArray;
          if (B = nil) or (B.Count < 4) then
            Break;
          G.CX := (B[0].AsFloat + B[2].AsFloat) / 2;
          G.CY := (B[1].AsFloat + B[3].AsFloat) / 2;
          Fs := It.Find('fs') as TJSONArray;
          if (Fs <> nil) and (Fs.Count >= 2) and (Fs[0].AsFloat > 0) and (Fs[1].AsFloat > 0) then
          begin
            KX := KX * Fs[0].AsFloat / 1000000;
            KY := KY * Fs[1].AsFloat / 1000000;
          end;
          Found := True;
          Break;
        end;
  finally
    J.Free;
  end;
  if not Found then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, Sid);
  if E < 0 then
    Exit;
  Pr := PropsOf(Els, E);
  if Pr < 0 then
    Exit;
  Xf := ChildNamed(Els, Pr, 'a:xfrm');
  if Xf < 0 then
    Exit;
  Ext := ChildNamed(Els, Xf, 'a:ext');
  if Ext < 0 then
    Exit;
  G.EX := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cx'), 0);
  G.EY := StrToFloatDef(AttrOf(Xml, Els[Ext].A, 'cy'), 0);
  G.W := G.EX * KX;
  G.H := G.EY * KY;
  G.KX := KX;
  G.KY := KY;
  G.Rot := StrToFloatDef(AttrOf(Xml, Els[Xf].A, 'rot'), 0) / 60000;
  V := AttrOf(Xml, Els[Xf].A, 'flipH');
  G.FlipH := (V = '1') or (V = 'true');
  V := AttrOf(Xml, Els[Xf].A, 'flipV');
  G.FlipV := (V = '1') or (V = 'true');
  G.Pic := Els[E].Name = 'pic:pic';
  G.TextBox := Pos('<wps:txbx', Copy(Xml, Els[E].A, Els[E].B - Els[E].A)) > 0;
  Pg := ChildNamed(Els, Pr, 'a:prstGeom');
  if Pg >= 0 then
  begin
    G.Prst := AttrOf(Xml, Els[Pg].A, 'prst');
    PresetAdjs(G);
    Av := ChildNamed(Els, Pg, 'a:avLst');
    if Av >= 0 then
      for I := Av + 1 to High(Els) do
        if (Els[I].Parent = Av) and (Els[I].Name = 'a:gd') then
        begin
          V := AttrOf(Xml, Els[I].A, 'fmla');
          for K := 0 to High(G.AdjN) do
            if (G.AdjN[K] = AttrOf(Xml, Els[I].A, 'name')) and (Copy(V, 1, 4) = 'val ') then
              G.AdjV[K] := StrToFloatDef(Copy(V, 5, MaxInt), G.AdjV[K]);
        end;
    PresetHandles(G);
  end
  else if ChildNamed(Els, Pr, 'a:custGeom') >= 0 then
    G.Prst := 'cust';
  FGeomLast := G;
  FGeomOk := True;
  Result := True;
end;

function TParadeEdit.SelectedShapeGeom(out G: TParadeShapeGeom): Boolean;
begin
  OnlyShape;
  Result := FShapeOn and (FShapeSid >= 0) and ShapeGeom(FShapeSid, G);
end;

function TParadeEdit.DrawMap(out M: TParadeDrawMap): Boolean;
var
  W, H, JW, JH: Double;
begin
  Result := FShapeOn and DrawingPlace(FShapeAt, M.Pg, M.X, M.Y, W, H, JW, JH) and (JW > 0) and (JH > 0);
  if Result then
  begin
    M.KX := W / JW;
    M.KY := H / JH;
  end;
end;

function TParadeEdit.MapToClient(const M: TParadeDrawMap; DX, DY: Double): TPoint;
begin
  Result.X := PageLeft(M.Pg) + Round((M.X + DX * M.KX) * PxPerSp);
  Result.Y := PageTop(M.Pg) + Round((M.Y + DY * M.KY) * PxPerSp);
end;

{ a point of a shape's box before it is turned (U, V: drawing units from its top left) where the shape, flipped and
  turned by Rot, puts it in the control }
function TParadeEdit.GeomToClient(const M: TParadeDrawMap; const G: TParadeShapeGeom; U, V, Rot: Double): TPoint;
var
  X, Y, C, S: Double;
begin
  X := U - G.W / 2;
  Y := V - G.H / 2;
  if G.FlipH then X := -X;
  if G.FlipV then Y := -Y;
  C := Cos(Rot * Pi / 180);
  S := Sin(Rot * Pi / 180);
  Result := MapToClient(M, G.CX + X * C - Y * S, G.CY + X * S + Y * C);
end;

procedure TParadeEdit.ClientToGeom(const M: TParadeDrawMap; const G: TParadeShapeGeom; PX, PY: Integer;
  out U, V: Double);
var
  X, Y, C, S, X2, Y2: Double;
begin
  X := ((PX - PageLeft(M.Pg)) / PxPerSp - M.X) / M.KX - G.CX;
  Y := ((PY - PageTop(M.Pg)) / PxPerSp - M.Y) / M.KY - G.CY;
  C := Cos(G.Rot * Pi / 180);
  S := Sin(G.Rot * Pi / 180);
  X2 := X * C + Y * S;
  Y2 := -X * S + Y * C;
  if G.FlipH then X2 := -X2;
  if G.FlipV then Y2 := -Y2;
  U := X2 + G.W / 2;
  V := Y2 + G.H / 2;
end;

{ the turning handle: above the middle of the shape's top, as it is turned }
function TParadeEdit.RotHandle(const M: TParadeDrawMap; const G: TParadeShapeGeom; Rot: Double;
  out ATop, P: TPoint): Boolean;
var
  Up, S, C, DX, DY: Double;
begin
  Result := True;
  ATop := GeomToClient(M, G, G.W / 2, 0, Rot);
  if G.FlipV then Up := 1 else Up := -1;
  C := Cos(Rot * Pi / 180);
  S := Sin(Rot * Pi / 180);
  DX := -Up * S;
  DY := Up * C;
  P := Point(ATop.X + Round(DX * 22), ATop.Y + Round(DY * 22));
end;

{ what of the selected shape is under a point of the control: 0..7 a sizing handle, 8 the shape (to move), 10 the
  turning handle, 11 an adjustment's (Idx), 12 a point (Idx, Edit Points); -1 none }
function TParadeEdit.ShapeHandleAt(X, Y: Integer; out Idx: Integer): Integer;
var
  G: TParadeShapeGeom;
  M: TParadeDrawMap;
  B: array[0..3] of Double;
  R: TRect;
  I, HX, HY, N, K: Integer;
  T, P: TPoint;
  C: TPathCmds;
  PW, PH, U, V: Double;
  Xml: string;
  Els: TXmlEls;
  PathEl, E: Integer;
begin
  Result := -1;
  Idx := -1;
  if not FShapeOn or (FDrawKind <> '') then
    Exit;
  if (FShapeSid >= 0) and (Length(FShapeMore) = 0) and not FNodeOn and DrawMap(M) and
     ConnectorInfo(U, V, PW, PH, HX, HY) then
  begin   { a connector: its ends, to drag on to other shapes }
    P := MapToClient(M, U, V);
    if (Abs(X - P.X) <= 6) and (Abs(Y - P.Y) <= 6) then
    begin
      Idx := 0;
      Exit(15);
    end;
    P := MapToClient(M, PW, PH);
    if (Abs(X - P.X) <= 6) and (Abs(Y - P.Y) <= 6) then
    begin
      Idx := 1;
      Exit(15);
    end;
  end;
  if (FShapeSid >= 0) and (Length(FShapeMore) = 0) and ShapeGeom(FShapeSid, G) and DrawMap(M) then
  begin
    if FNodeOn then
    begin
      if KeptXml(Xml) then
      begin
        Els := XmlElements(Xml);
        E := SidElement(Els, FShapeSid);
        if (E >= 0) and ParsePath(Xml, Els, PropsOf(Els, E), PathEl, C, PW, PH) then
        begin
          N := 0;
          for I := 0 to High(C) do
            for K := 0 to C[I].N - 1 do
            begin
              if PW > 0 then U := C[I].X[K] * G.W / PW else U := 0;
              if PH > 0 then V := C[I].Y[K] * G.H / PH else V := 0;
              P := GeomToClient(M, G, U, V, G.Rot);
              if (Abs(X - P.X) <= 5) and (Abs(Y - P.Y) <= 5) then
              begin
                Idx := N;
                Exit(12);
              end;
              Inc(N);
            end;
        end;
      end;
      Exit;
    end;
    if RotHandle(M, G, G.Rot, T, P) and (Abs(X - P.X) <= 6) and (Abs(Y - P.Y) <= 6) then
      Exit(10);
    for I := 0 to High(G.HX) do
    begin
      P := GeomToClient(M, G, G.HX[I], G.HY[I], G.Rot);
      if (Abs(X - P.X) <= 5) and (Abs(Y - P.Y) <= 5) then
      begin
        Idx := I;
        Exit(11);
      end;
    end;
  end;
  if (FShapeSid < 0) and FCanvasPage then
    Exit;     { the canvas page: its corner, not the canvas's handles }
  if FShapeSid < 0 then
  begin
    if not DrawMap(M) then
      Exit;
    B[0] := 0; B[1] := 0; B[2] := M.KX; B[3] := M.KY;
    { the whole object: its box in its own units }
    B[2] := 0; B[3] := 0;
    if not DrawingPlace(FShapeAt, M.Pg, M.X, M.Y, U, V, B[2], B[3]) then
      Exit;
  end
  else if not ShapeBox(FShapeSid, B[0], B[1], B[2], B[3]) then
    Exit;
  if not ShapeClientRect(B, R) then
    Exit;
  for I := 0 to 7 do    { the handles, as PaintShapeSelection draws them }
  begin
    case I of
      0: begin HX := R.Left; HY := R.Top; end;
      1: begin HX := (R.Left + R.Right) div 2; HY := R.Top; end;
      2: begin HX := R.Right; HY := R.Top; end;
      3: begin HX := R.Right; HY := (R.Top + R.Bottom) div 2; end;
      4: begin HX := R.Right; HY := R.Bottom; end;
      5: begin HX := (R.Left + R.Right) div 2; HY := R.Bottom; end;
      6: begin HX := R.Left; HY := R.Bottom; end;
    else
      begin HX := R.Left; HY := (R.Top + R.Bottom) div 2; end;
    end;
    if (Abs(X - HX) <= 5) and (Abs(Y - HY) <= 5) then
      Exit(I);
  end;
  if (FShapeSid < 0) and OnDrawingBody(X, Y) then
    Result := 14
  else if (FShapeSid >= 0) and PtInRect(Rect(R.Left, R.Top, R.Right + 1, R.Bottom + 1), Point(X, Y)) then
    Result := 8
  else if (FShapeSid >= 0) and (Length(FShapeMore) > 0) then
    for I := 0 to High(FShapeMore) do     { the others selected with it: all of them moved }
      if ShapeBox(FShapeMore[I], B[0], B[1], B[2], B[3]) and ShapeClientRect(B, R) and
         PtInRect(Rect(R.Left, R.Top, R.Right + 1, R.Bottom + 1), Point(X, Y)) then
        Exit(8);
end;

function TParadeEdit.ShapeHandlePoint(Kind: Char; Index: Integer; out P: TPoint): Boolean;
var
  G: TParadeShapeGeom;
  M: TParadeDrawMap;
  T: TPoint;
  U, V, PW, PH: Double;
  B: array[0..3] of Double;
  R: TRect;
  Xml: string;
  Els: TXmlEls;
  E, PathEl, Ci, Ki: Integer;
  C: TPathCmds;
begin
  Result := False;
  P := Point(0, 0);
  if Kind = 's' then
  begin
    if (FShapeSid < 0) or not ShapeBox(FShapeSid, B[0], B[1], B[2], B[3]) or not ShapeClientRect(B, R) then
      Exit;
    case Index of
      0: P := Point(R.Left, R.Top);
      1: P := Point((R.Left + R.Right) div 2, R.Top);
      2: P := Point(R.Right, R.Top);
      3: P := Point(R.Right, (R.Top + R.Bottom) div 2);
      4: P := Point(R.Right, R.Bottom);
      5: P := Point((R.Left + R.Right) div 2, R.Bottom);
      6: P := Point(R.Left, R.Bottom);
    else
      P := Point(R.Left, (R.Top + R.Bottom) div 2);
    end;
    Exit(True);
  end;
  if (FShapeSid < 0) or not ShapeGeom(FShapeSid, G) or not DrawMap(M) then
    Exit;
  case Kind of
    'r': Result := RotHandle(M, G, G.Rot, T, P);
    'a':
      if (Index >= 0) and (Index <= High(G.HX)) then
      begin
        P := GeomToClient(M, G, G.HX[Index], G.HY[Index], G.Rot);
        Result := True;
      end;
    'n':
      if KeptXml(Xml) then
      begin
        Els := XmlElements(Xml);
        E := SidElement(Els, FShapeSid);
        if (E >= 0) and ParsePath(Xml, Els, PropsOf(Els, E), PathEl, C, PW, PH) and PathPoint(C, Index, Ci, Ki) then
        begin
          if PW > 0 then U := C[Ci].X[Ki] * G.W / PW else U := 0;
          if PH > 0 then V := C[Ci].Y[Ki] * G.H / PH else V := 0;
          P := GeomToClient(M, G, U, V, G.Rot);
          Result := True;
        end;
      end;
  end;
end;

{ over the selection's box: the turning handle, the adjustments' handles, a turned shape's outline, Edit Points'
  points, and what a drag of one of them would make }
procedure TParadeEdit.PaintShapeExtras;
var
  G, GA: TParadeShapeGeom;
  M: TParadeDrawMap;
  T, P, Q: TPoint;
  I, K, N, E, PathEl: Integer;
  U, V, PW, PH, Rot: Double;
  C: TPathCmds;
  Xml: string;
  Els: TXmlEls;
  Ci, Ki: Integer;

  procedure Outline(const Cmds: TPathCmds; const GG: TParadeShapeGeom; PathW, PathH, R: Double);
  var
    F: TPathFlat;
    J, NP: Integer;
    Pts: array of TPoint;
  begin
    F := FlattenPath(Cmds);
    SetLength(Pts, Length(F) div 2);
    NP := 0;
    J := 0;
    while J + 1 <= High(F) do
    begin
      if IsNan(F[J]) then
      begin
        if NP > 1 then Canvas.Polyline(Pts, 0, NP);
        NP := 0;
      end
      else
      begin
        if PathW > 0 then U := F[J] * GG.W / PathW else U := 0;
        if PathH > 0 then V := F[J + 1] * GG.H / PathH else V := 0;
        Pts[NP] := GeomToClient(M, GG, U, V, R);
        Inc(NP);
      end;
      Inc(J, 2);
    end;
    if NP > 1 then Canvas.Polyline(Pts, 0, NP);
  end;

  procedure Box(const GG: TParadeShapeGeom; R: Double);
  begin
    Canvas.Polyline([GeomToClient(M, GG, 0, 0, R), GeomToClient(M, GG, GG.W, 0, R), GeomToClient(M, GG, GG.W, GG.H, R),
      GeomToClient(M, GG, 0, GG.H, R), GeomToClient(M, GG, 0, 0, R)]);
  end;

begin
  if (FShapeSid < 0) or (Length(FShapeMore) > 0) or (FDrawKind <> '') or not ShapeGeom(FShapeSid, G) or
     not DrawMap(M) then
    Exit;
  Canvas.Brush.Style := bsClear;
  Canvas.Pen.Width := 1;
  Canvas.Pen.Color := $00D77800;
  if FNodeOn then
  begin
    if not KeptXml(Xml) then
      Exit;
    Els := XmlElements(Xml);
    E := SidElement(Els, FShapeSid);
    if (E < 0) or not ParsePath(Xml, Els, PropsOf(Els, E), PathEl, C, PW, PH) then
      Exit;
    if (FShapeDrag = 12) and PathPoint(C, FDragIdx, Ci, Ki) then
    begin   { where the drag would put the point }
      C[Ci].X[Ki] := FNodeX;
      C[Ci].Y[Ki] := FNodeY;
    end;
    Canvas.Pen.Style := psSolid;
    Outline(C, G, PW, PH, G.Rot);
    { a curve's controls, tied to their ends }
    Canvas.Pen.Style := psDot;
    for I := 0 to High(C) do
      if C[I].Cmd = 'c' then
      begin
        if I > 0 then
        begin
          P := GeomToClient(M, G, IfThen(PW > 0, C[I - 1].X[C[I - 1].N - 1] * G.W / PW, 0),
            IfThen(PH > 0, C[I - 1].Y[C[I - 1].N - 1] * G.H / PH, 0), G.Rot);
          Q := GeomToClient(M, G, IfThen(PW > 0, C[I].X[0] * G.W / PW, 0), IfThen(PH > 0, C[I].Y[0] * G.H / PH, 0),
            G.Rot);
          Canvas.Line(P, Q);
        end;
        P := GeomToClient(M, G, IfThen(PW > 0, C[I].X[1] * G.W / PW, 0), IfThen(PH > 0, C[I].Y[1] * G.H / PH, 0), G.Rot);
        Q := GeomToClient(M, G, IfThen(PW > 0, C[I].X[2] * G.W / PW, 0), IfThen(PH > 0, C[I].Y[2] * G.H / PH, 0), G.Rot);
        Canvas.Line(P, Q);
      end;
    Canvas.Pen.Style := psSolid;
    Canvas.Pen.Color := clBlack;
    N := 0;
    for I := 0 to High(C) do
      for K := 0 to C[I].N - 1 do
      begin
        P := GeomToClient(M, G, IfThen(PW > 0, C[I].X[K] * G.W / PW, 0), IfThen(PH > 0, C[I].Y[K] * G.H / PH, 0), G.Rot);
        Canvas.Brush.Style := bsSolid;
        if IsAnchor(C[I], K) then
        begin   { a point the path goes through: a square, filled black while it is dragged }
          if (FShapeDrag = 12) and (FDragIdx = N) then Canvas.Brush.Color := clBlack else Canvas.Brush.Color := clWhite;
          Canvas.Rectangle(P.X - 3, P.Y - 3, P.X + 4, P.Y + 4);
        end
        else
        begin
          Canvas.Brush.Color := clWhite;
          Canvas.Ellipse(P.X - 3, P.Y - 3, P.X + 4, P.Y + 4);
        end;
        Inc(N);
      end;
    Exit;
  end;
  if Abs(G.Rot) > 0.01 then
  begin   { turned: its own box, as it is turned }
    Canvas.Pen.Style := psDot;
    Box(G, G.Rot);
  end;
  Rot := G.Rot;
  if FShapeDrag = 10 then
  begin   { where the turn would put it }
    Rot := FRotNew;
    Canvas.Pen.Style := psDash;
    Box(G, Rot);
  end;
  if FShapeDrag = 11 then
  begin   { the outline the drag would give, and the handles where they would be }
    GA := G;
    GA.AdjV := Copy(G.AdjV);
    TakeAdjs(GA, FAdjNew);
    PresetHandles(GA);
    Canvas.Pen.Style := psDash;
    Outline(PresetPath(GA, FAdjNew), GA, Max(GA.EX, 1), Max(GA.EY, 0), G.Rot);
    G := GA;
  end;
  { the turning handle, tied to the top }
  if RotHandle(M, G, Rot, T, P) then
  begin
    Canvas.Pen.Style := psSolid;
    Canvas.Pen.Color := $00D77800;
    Canvas.Line(T, P);
    Canvas.Brush.Style := bsSolid;
    Canvas.Brush.Color := clWhite;
    Canvas.Ellipse(P.X - 5, P.Y - 5, P.X + 6, P.Y + 6);
    Canvas.Arc(P.X - 3, P.Y - 3, P.X + 4, P.Y + 4, P.X + 4, P.Y, P.X, P.Y - 4);
  end;
  { the adjustments: yellow diamonds }
  Canvas.Pen.Style := psSolid;
  Canvas.Pen.Color := clBlack;
  Canvas.Brush.Style := bsSolid;
  Canvas.Brush.Color := clYellow;
  for I := 0 to High(G.HX) do
  begin
    P := GeomToClient(M, G, G.HX[I], G.HY[I], G.Rot);
    Canvas.Polygon([Point(P.X, P.Y - 5), Point(P.X + 5, P.Y), Point(P.X, P.Y + 5), Point(P.X - 5, P.Y)]);
  end;
end;

{ the cursor a place under the mouse asks for (crDefault: the control's own) }
procedure TParadeEdit.HoverCursor(C: TCursor);
begin
  if C = crDefault then
  begin
    if FFrameCursor then
    begin
      Cursor := FFrameOldCursor;
      FFrameCursor := False;
    end;
    Exit;
  end;
  if not FFrameCursor then
  begin
    FFrameOldCursor := Cursor;
    FFrameCursor := True;
  end;
  if Cursor <> C then
    Cursor := C;
end;

function TParadeEdit.RotateShape(Deg: Double): Boolean;
var
  Xml: string;
  Els: TXmlEls;
  E, Pr, Xf: Integer;
  R: Int64;
begin
  OnlyShape;
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  Pr := PropsOf(Els, E);
  if (E < 0) or (Pr < 0) then
    Exit;
  Xf := ChildNamed(Els, Pr, 'a:xfrm');
  if Xf < 0 then
    Exit;
  R := Round(Deg * 60000) mod 21600000;
  if R < 0 then
    Inc(R, 21600000);
  SetAttr(Xml, Els[Xf].A, 'rot', IntToStr(R));
  Result := ApplyKeptXml(Xml, 'Rotate', FShapeSid);
end;

function TParadeEdit.FlipShape(Horizontal: Boolean): Boolean;
var
  Xml, N, V: string;
  Els: TXmlEls;
  E, Pr, Xf: Integer;
begin
  OnlyShape;
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  Pr := PropsOf(Els, E);
  if (E < 0) or (Pr < 0) then
    Exit;
  Xf := ChildNamed(Els, Pr, 'a:xfrm');
  if Xf < 0 then
    Exit;
  if Horizontal then N := 'flipH' else N := 'flipV';
  V := AttrOf(Xml, Els[Xf].A, N);
  if (V = '1') or (V = 'true') then
    SetAttr(Xml, Els[Xf].A, N, '0')
  else
    SetAttr(Xml, Els[Xf].A, N, '1');
  Result := ApplyKeptXml(Xml, 'Flip', FShapeSid);
end;

{ an adjustment of shape Sid's preset set in kept XML: its a:gd in the preset's a:avLst, replaced or added }
function PutAdjXml(var Xml: string; Sid: Integer; const AName: string; Value: Double): Boolean;
var
  Gd: string;
  Els: TXmlEls;
  E, Pr, Pg, Av, I, P: Integer;
  Done: Boolean;
begin
  Result := False;
  Els := XmlElements(Xml);
  E := SidElement(Els, Sid);
  if E < 0 then
    Exit;
  Pr := PropsOf(Els, E);
  if Pr < 0 then
    Exit;
  Pg := ChildNamed(Els, Pr, 'a:prstGeom');
  if Pg < 0 then
    Exit;
  Gd := '<a:gd name="' + AName + '" fmla="val ' + IntToStr(Round(Value)) + '"/>';
  Av := ChildNamed(Els, Pg, 'a:avLst');
  if Av < 0 then
  begin
    if Copy(Xml, Els[Pg].B - 2, 2) = '/>' then
      Xml := Copy(Xml, 1, Els[Pg].B - 3) + '><a:avLst>' + Gd + '</a:avLst></a:prstGeom>' + Copy(Xml, Els[Pg].B, MaxInt)
    else
    begin
      P := PosEx('>', Xml, Els[Pg].A);
      Insert('<a:avLst>' + Gd + '</a:avLst>', Xml, P + 1);
    end;
  end
  else if Copy(Xml, Els[Av].B - 2, 2) = '/>' then
    Xml := Copy(Xml, 1, Els[Av].A - 1) + '<a:avLst>' + Gd + '</a:avLst>' + Copy(Xml, Els[Av].B, MaxInt)
  else
  begin
    Done := False;
    for I := Av + 1 to High(Els) do
      if (Els[I].Parent = Av) and (Els[I].Name = 'a:gd') and (AttrOf(Xml, Els[I].A, 'name') = AName) then
      begin
        Xml := Copy(Xml, 1, Els[I].A - 1) + Gd + Copy(Xml, Els[I].B, MaxInt);
        Done := True;
        Break;
      end;
    if not Done then
      Insert(Gd, Xml, CloseOf(Xml, Els[Av]));
  end;
  Result := True;
end;

{ the adjustments a preset's handle I (of those the shape shows) dragged to U, V (drawing units in its box) gives }
function PresetDrag(const G: TParadeShapeGeom; I: Integer; U, V: Double): string;
var
  N: csize_t;
  A: string;
begin
  A := AdjString(G);
  Result := A;
  if (I < 0) or (I > High(G.HIdx)) or (G.KX <= 0) or (G.KY <= 0) then
    Exit;
  SetLength(Result, 1024);
  N := pd_preset_drag(PAnsiChar(G.Prst), G.EX, G.EY, PAnsiChar(A), G.HIdx[I], U / G.KX, V / G.KY, PAnsiChar(Result),
    Length(Result));
  if N = 0 then
    Result := A
  else
    SetLength(Result, Min(N, 1023));
end;

function TParadeEdit.SetShapeAdjust(const AName: string; Value: Double): Boolean;
var
  Xml: string;
begin
  OnlyShape;
  Result := not FReadOnly and FShapeOn and (FShapeSid >= 0) and KeptXml(Xml) and
    PutAdjXml(Xml, FShapeSid, AName, Value) and ApplyKeptXml(Xml, 'Adjust', FShapeSid);
end;

function TParadeEdit.SetShapeAdjusts(const Adj: string): Boolean;
var
  Xml: string;
  G: TParadeShapeGeom;
  Parts: TStringArray;
  I, K, Q: Integer;
  V: Double;
  Moved: Boolean;
begin
  OnlyShape;
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) or not ShapeGeom(FShapeSid, G) or not KeptXml(Xml) then
    Exit;
  Moved := False;
  Parts := Adj.Split([' ', ',']);
  for I := 0 to High(Parts) do
  begin
    Q := Pos('=', Parts[I]);
    if Q = 0 then
      Continue;
    V := StrToFloatDef(Copy(Parts[I], Q + 1, MaxInt), 0);
    for K := 0 to High(G.AdjN) do     { only what the drag changed: the others stay as Word has them, or unsaid }
      if (G.AdjN[K] = Copy(Parts[I], 1, Q - 1)) and (Round(G.AdjV[K]) <> Round(V)) then
        Moved := PutAdjXml(Xml, FShapeSid, G.AdjN[K], V) or Moved;
  end;
  Result := Moved and ApplyKeptXml(Xml, 'Adjust', FShapeSid);
end;

function TParadeEdit.EditShapePoints: Boolean;
var
  G: TParadeShapeGeom;
  Xml: string;
  Els: TXmlEls;
  E, Pr, Pg, PathEl: Integer;
  C: TPathCmds;
  PW, PH: Double;
begin
  OnlyShape;
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) or (Length(FShapeMore) > 0) or not ShapeGeom(FShapeSid, G) or
     G.Pic or G.TextBox or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  Pr := PropsOf(Els, E);
  if (E < 0) or (Pr < 0) then
    Exit;
  if G.Prst = 'cust' then
  begin
    if not ParsePath(Xml, Els, Pr, PathEl, C, PW, PH) then
      Exit;
  end
  else
  begin   { a preset: its outline as a custom geometry, as Word makes it for Edit Points }
    Pg := ChildNamed(Els, Pr, 'a:prstGeom');
    if Pg < 0 then
      Exit;
    Xml := Copy(Xml, 1, Els[Pg].A - 1) + CustGeomXml(PresetPath(G, AdjString(G)), Max(G.EX, 1), Max(G.EY, 1)) +
      Copy(Xml, Els[Pg].B, MaxInt);
    if not ApplyKeptXml(Xml, 'Edit points', FShapeSid) then
      Exit;
  end;
  FNodeOn := True;
  Invalidate;
  Result := True;
end;

{ point N of the selected shape's path dragged to NX, NY (its path's units): an end with the controls beside it, and
  any end at the same place (a closed path's first and last); the box made again to hold the path }
function TParadeEdit.MoveShapePoint(N: Integer; NX, NY: Double): Boolean;
var
  G: TParadeShapeGeom;
  Xml: string;
  Els: TXmlEls;
  E, Pr, PathEl, Xf, Off, Ext, Ci, Ki, I, K, A, B: Integer;
  C: TPathCmds;
  PW, PH, OX, OY, DX, DY, MinX, MinY, MaxX, MaxY, SX, SY, NEX, NEY, LX, LY, CR, SR, RX, RY, OffX, OffY: Double;

  procedure Shift(Ii, Kk: Integer);
  begin
    if (Ii >= 0) and (Ii <= High(C)) and (Kk >= 0) and (Kk < C[Ii].N) then
    begin
      C[Ii].X[Kk] := C[Ii].X[Kk] + DX;
      C[Ii].Y[Kk] := C[Ii].Y[Kk] + DY;
    end;
  end;

begin
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) or not ShapeGeom(FShapeSid, G) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  Pr := PropsOf(Els, E);
  if (E < 0) or (Pr < 0) or not ParsePath(Xml, Els, Pr, PathEl, C, PW, PH) or not PathPoint(C, N, Ci, Ki) then
    Exit;
  OX := C[Ci].X[Ki];
  OY := C[Ci].Y[Ki];
  DX := NX - OX;
  DY := NY - OY;
  if IsAnchor(C[Ci], Ki) then
  begin
    for I := 0 to High(C) do
      if (C[I].N > 0) and IsAnchor(C[I], C[I].N - 1) and (Abs(C[I].X[C[I].N - 1] - OX) < 0.5) and
         (Abs(C[I].Y[C[I].N - 1] - OY) < 0.5) then
      begin
        Shift(I, C[I].N - 1);
        if C[I].Cmd = 'c' then
          Shift(I, 1);            { the control coming into it }
        if (I < High(C)) and (C[I + 1].Cmd = 'c') then
          Shift(I + 1, 0);        { and the one going out }
      end;
  end
  else
  begin
    C[Ci].X[Ki] := NX;
    C[Ci].Y[Ki] := NY;
  end;
  { the box around the points again }
  MinX := 1e300; MinY := 1e300; MaxX := -1e300; MaxY := -1e300;
  for I := 0 to High(C) do
    for K := 0 to C[I].N - 1 do
    begin
      MinX := Min(MinX, C[I].X[K]); MaxX := Max(MaxX, C[I].X[K]);
      MinY := Min(MinY, C[I].Y[K]); MaxY := Max(MaxY, C[I].Y[K]);
    end;
  if MinX > MaxX then
    Exit;
  for I := 0 to High(C) do
    for K := 0 to C[I].N - 1 do
    begin
      C[I].X[K] := C[I].X[K] - MinX;
      C[I].Y[K] := C[I].Y[K] - MinY;
    end;
  if PW > 0 then SX := G.EX / PW else SX := 1;
  if PH > 0 then SY := G.EY / PH else SY := 1;
  NEX := (MaxX - MinX) * SX;
  NEY := (MaxY - MinY) * SY;
  { the centre moved, in the shape's own units, flipped and turned as it is, into its group's }
  LX := (MinX + MaxX) / 2 * SX - G.EX / 2;
  LY := (MinY + MaxY) / 2 * SY - G.EY / 2;
  if G.FlipH then LX := -LX;
  if G.FlipV then LY := -LY;
  CR := Cos(G.Rot * Pi / 180);
  SR := Sin(G.Rot * Pi / 180);
  RX := LX * CR - LY * SR;
  RY := LX * SR + LY * CR;
  { written from the end back, so that what is written does not move what is still to be }
  A := Els[PathEl].A;
  B := CloseOf(Xml, Els[PathEl]);
  if Copy(Xml, Els[PathEl].B - 2, 2) <> '/>' then
  begin
    I := PosEx('>', Xml, A);
    Xml := Copy(Xml, 1, I) + PathXml(C) + Copy(Xml, B, MaxInt);
  end;
  SetAttr(Xml, A, 'h', IntToStr(Max(1, Round(MaxY - MinY))));
  SetAttr(Xml, A, 'w', IntToStr(Max(1, Round(MaxX - MinX))));
  Xf := ChildNamed(Els, Pr, 'a:xfrm');
  if Xf < 0 then
    Exit;
  Off := ChildNamed(Els, Xf, 'a:off');
  Ext := ChildNamed(Els, Xf, 'a:ext');
  if (Off < 0) or (Ext < 0) then
    Exit;
  OffX := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'x'), 0);
  OffY := StrToFloatDef(AttrOf(Xml, Els[Off].A, 'y'), 0);
  SetAttr(Xml, Els[Ext].A, 'cy', IntToStr(Round(NEY)));
  SetAttr(Xml, Els[Ext].A, 'cx', IntToStr(Round(NEX)));
  SetAttr(Xml, Els[Off].A, 'y', IntToStr(Round(OffY + G.EY / 2 + RY - NEY / 2)));
  SetAttr(Xml, Els[Off].A, 'x', IntToStr(Round(OffX + G.EX / 2 + RX - NEX / 2)));
  Result := ApplyKeptXml(Xml, 'Edit points', FShapeSid);
end;

function TParadeEdit.ClientToDrawing(X, Y: Integer; out DX, DY: Double): Boolean;
var
  M: TParadeDrawMap;
begin
  Result := DrawMap(M);
  if not Result then
    Exit;
  DX := ((X - PageLeft(M.Pg)) / PxPerSp - M.X) / M.KX;
  DY := ((Y - PageTop(M.Pg)) / PxPerSp - M.Y) / M.KY;
end;

{ the freeform, curve or scribble drawn so far made a shape of the canvas (Closed: round to its start, filled) }
procedure TParadeEdit.FinishDrawPath(Closed: Boolean);
var
  K: string;
  Pts: TPathFlat;
begin
  K := FDrawKind;
  Pts := Copy(FDrawPts);
  SetLength(FDrawPts, 0);
  FDrawKind := '';
  Cursor := FDrawCursor;
  if K = 'scribble' then
    Pts := SimplifyPts(Pts, 0.6 * PD_SP_PER_PT);
  if Length(Pts) >= 4 then
    AddPathShape(Pts, K = 'curve', Closed);
  Invalidate;
end;

{ the freeform, curve or scribble being drawn, and the line on to the mouse }
procedure TParadeEdit.PaintSites;
var
  Xml: string;
  Els: TXmlEls;
  M: TParadeDrawMap;
  Root, E, I: Integer;
  S: TParadeSites;
  P: TPoint;
begin
  if not KeptXml(Xml) or not DrawMap(M) then
    Exit;
  Els := XmlElements(Xml);
  Root := TopOfDrawing(Els);
  Canvas.Pen.Color := $00D77800;
  Canvas.Pen.Style := psSolid;
  Canvas.Pen.Width := 1;
  Canvas.Brush.Style := bsSolid;
  Canvas.Brush.Color := clWhite;
  for E := 0 to High(Els) do
    if (Root >= 0) and ((Els[E].Name = 'wps:wsp') or (Els[E].Name = 'pic:pic')) and
       (ChildNamed(Els, E, 'wps:cNvCnPr') < 0) then
    begin
      S := ElSites(Xml, Els, E);
      for I := 0 to High(S) do
      begin
        P := MapToClient(M, S[I].X * EmuD, S[I].Y * EmuD);
        Canvas.Ellipse(P.X - 3, P.Y - 3, P.X + 4, P.Y + 4);
      end;
    end;
end;

procedure TParadeEdit.PaintDrawPath;
var
  M: TParadeDrawMap;
  F: TPathFlat;
  Pts: array of TPoint;
  I, N: Integer;
begin
  if LineKind(FDrawKind) then
    PaintSites;     { a connector to draw: where it can be joined }
  if (Length(FDrawPts) < 2) or not DrawMap(M) then
    Exit;
  if (FDrawKind = 'curve') and (Length(FDrawPts) >= 4) then
    F := FlattenPath(SplineCmds(FDrawPts, False))
  else
    F := Copy(FDrawPts);
  SetLength(Pts, Length(F) div 2 + 1);
  N := 0;
  for I := 0 to Length(F) div 2 - 1 do
    if not IsNan(F[2 * I]) then
    begin
      Pts[N] := MapToClient(M, F[2 * I], F[2 * I + 1]);
      Inc(N);
    end;
  if (FDrawKind = 'freeform') or (FDrawKind = 'curve') then
  begin
    Pts[N] := FPolyMouse;
    Inc(N);
  end;
  Canvas.Pen.Style := psSolid;
  Canvas.Pen.Color := FShapeFillDef;
  Canvas.Pen.Width := 2;
  if N > 1 then
    Canvas.Polyline(Pts, 0, N);
  Canvas.Pen.Width := 1;
end;

{ point N of the selected shape's path, in its units (which it keeps for a drag) }
function TParadeEdit.ShapeHandleXY(N: Integer; out PX, PY: Double): Boolean;
var
  Xml: string;
  Els: TXmlEls;
  E, PathEl, Ci, Ki: Integer;
  C: TPathCmds;
begin
  Result := False;
  PX := 0;
  PY := 0;
  if (FShapeSid < 0) or not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, FShapeSid);
  if (E < 0) or not ParsePath(Xml, Els, PropsOf(Els, E), PathEl, C, FDragPW, FDragPH) or
     not PathPoint(C, N, Ci, Ki) then
    Exit;
  PX := C[Ci].X[Ki];
  PY := C[Ci].Y[Ki];
  Result := True;
end;

function TParadeEdit.AddShapeText: Boolean;
var
  Xml, Sh, Fill: string;
  Els: TXmlEls;
  E, A, B, P, L, Sid: Integer;
  RGB: LongInt;
begin
  OnlyShape;
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) or (Length(FShapeMore) > 0) then
    Exit;
  Sid := FShapeSid;
  if EnterTextBox(Sid, CaretPos, False, True) then
    Exit(True);
  if not KeptXml(Xml) then
    Exit;
  Els := XmlElements(Xml);
  E := SidElement(Els, Sid);
  if (E < 0) or (Els[E].Name <> 'wps:wsp') then
    Exit;     { a picture }
  Sh := Copy(Xml, Els[E].A, Els[E].B - Els[E].A);
  if (Pos('<wps:cNvCnPr', Sh) > 0) or (Pos('prst="line"', Sh) > 0) or (Pos('Connector', Sh) > 0) or
     (Pos('<wps:txbx', Sh) > 0) then
    Exit;     { a line takes no text }
  B := Pos('<wps:bodyPr', Sh);
  if B > 0 then
  begin   { in the middle of it, as Word puts a shape's text }
    L := PosEx('>', Sh, B);
    if (L > 0) and (Pos(' anchor="', Copy(Sh, B, L - B)) = 0) then
      Insert(' anchor="ctr"', Sh, B + 11);
  end
  else
  begin
    B := Length(Sh) - Length('</wps:wsp>') + 1;
    Insert('<wps:bodyPr anchor="ctr"/>', Sh, B);
  end;
  Insert('<wps:txbx><w:txbxContent><w:p><w:pPr><w:jc w:val="center"/></w:pPr></w:p></w:txbxContent></wps:txbx>',
    Sh, B);
  A := Els[E].A;
  Delete(Xml, A, Els[E].B - A);
  Insert(Sh, Xml, A);
  if not ApplyKeptXml(Xml, 'Add text', Sid) then
    Exit;
  Result := EnterTextBox(Sid, CaretPos, False, True);
  { white letters, as Word's shapes have them, unless the fill is light }
  Fill := '';
  P := Pos('<a:solidFill><a:srgbClr val="', Sh);
  if (P > 0) and (P < Pos('<a:ln', Sh + '<a:ln')) then
    Fill := Copy(Sh, P + 29, 6);
  if Result and (Length(Fill) = 6) then
  begin
    RGB := StrToIntDef('$' + Fill, -1);
    if (RGB >= 0) and (0.299 * ((RGB shr 16) and 255) + 0.587 * ((RGB shr 8) and 255) + 0.114 * (RGB and 255) < 186) then
      SetTextColor($FFFFFF);
  end;
end;

function TParadeEdit.SetShapeStyle(AFill, ALine: TColor): Boolean;
begin
  OnlyShape;
  Result := False;
  if FReadOnly or not FShapeOn or (FShapeSid < 0) then
    Exit;
  PushShapeStep('Shape style', FShapeAt);
  Inc(FStepDepth);
  pd_doc_begin_group(FDoc, 'Shape style');
  try
    Result := SetShapeFill(AFill, False);
    Result := SetShapeLine(ALine, 0, False) and Result;
  finally
    pd_doc_end_group(FDoc);
    Dec(FStepDepth);
  end;
end;

{ the text box Sid of the selected drawing: the caret in its text -- at At when that is in it (UseAt), else at its
  start or (AtEnd) its end. False for a shape without text }
function TParadeEdit.EnterTextBox(Sid: Integer; const At: pd_pos; UseAt, AtEnd: Boolean): Boolean;
var
  Boxes: TParadeShapeBoxes;
  I: Integer;
  St, B: pd_block_id;
begin
  Result := False;
  if not FShapeOn then
    Exit;
  Boxes := DrawingShapes(FShapeAt);
  St := 0;
  for I := 0 to High(Boxes) do
    if Boxes[I].Sid = Sid then
      St := Boxes[I].Story;
  if St = 0 then
    Exit;
  if AtEnd then
    B := pd_doc_child(FDoc, St, Max(0, ChildCount(St) - 1))
  else
    B := pd_doc_child(FDoc, St, 0);
  if B = 0 then
    Exit;
  ClearShapeSelection;
  if UseAt and (StoryTopOf(At.block) = St) then
    SetCaret(At, False)
  else if AtEnd then
    SetCaret(PdPos(B, Length(ParaText(B))), False)
  else
    SetCaret(PdPos(B, 0), False);
  Result := True;
end;

{ a box of the selected drawing (its units) in client pixels }
function TParadeEdit.ShapeClientRect(const B: array of Double; out R: TRect): Boolean;
var
  Pg: Int32;
  X, Y, W, H, JW, JH: Double;
begin
  Result := FShapeOn and DrawingPlace(FShapeAt, Pg, X, Y, W, H, JW, JH) and (JW > 0) and (JH > 0);
  if not Result then
    Exit;
  R.Left := PageLeft(Pg) + Round((X + B[0] * W / JW) * PxPerSp);
  R.Top := PageTop(Pg) + Round((Y + B[1] * H / JH) * PxPerSp);
  R.Right := PageLeft(Pg) + Round((X + B[2] * W / JW) * PxPerSp);
  R.Bottom := PageTop(Pg) + Round((Y + B[3] * H / JH) * PxPerSp);
end;

{ a press on the selected shape: on a handle, a resize; inside it, a move. False when it is neither }
function TParadeEdit.ShapeDragStart(X, Y: Integer): Boolean;
var
  I, K: Integer;
  HP: TPoint;
  Pg: Int32;
  PX, PY, W, H, JW, JH: Double;
begin
  Result := False;
  if not FShapeOn then
    Exit;
  if FDrawKind <> '' then
  begin   { a shape drawn: from the press, in the canvas }
    if DrawingPlace(FShapeAt, Pg, PX, PY, W, H, JW, JH) and (W > 0) and (H > 0) and
       (X >= PageLeft(Pg) + Round(PX * PxPerSp)) and (X <= PageLeft(Pg) + Round((PX + W) * PxPerSp)) and
       (Y >= PageTop(Pg) + Round(PY * PxPerSp)) and (Y <= PageTop(Pg) + Round((PY + H) * PxPerSp)) then
    begin
      FShapeOld[0] := ((X - PageLeft(Pg)) / PxPerSp - PX) * JW / W;
      FShapeOld[1] := ((Y - PageTop(Pg)) / PxPerSp - PY) * JH / H;
      FShapeOld[2] := FShapeOld[0];
      FShapeOld[3] := FShapeOld[1];
      if (FDrawKind = 'freeform') or (FDrawKind = 'curve') then
      begin   { a corner a click: on the first again, the shape closed }
        if (Length(FDrawPts) >= 6) and (Abs(X - (PageLeft(Pg) + Round((PX + FDrawPts[0] * W / JW) * PxPerSp))) <= 6) and
           (Abs(Y - (PageTop(Pg) + Round((PY + FDrawPts[1] * H / JH) * PxPerSp))) <= 6) then
          FinishDrawPath(True)
        else
        begin
          SetLength(FDrawPts, Length(FDrawPts) + 2);
          FDrawPts[High(FDrawPts) - 1] := FShapeOld[0];
          FDrawPts[High(FDrawPts)] := FShapeOld[1];
          FPolyMouse := Point(X, Y);
          Invalidate;
        end;
        Exit(True);
      end;
      if FDrawKind = 'scribble' then
      begin
        SetLength(FDrawPts, 2);
        FDrawPts[0] := FShapeOld[0];
        FDrawPts[1] := FShapeOld[1];
      end;
      FShapeNew := FShapeOld;
      FShapeDrag := 9;
      FShapeFrom := Point(X, Y);
      Exit(True);
    end;
    if Length(FDrawPts) >= 4 then
      FinishDrawPath(False)    { a freeform pressed out of: as far as it went }
    else
    begin
      FDrawKind := '';    { pressed elsewhere: no shape }
      SetLength(FDrawPts, 0);
      Cursor := FDrawCursor;
    end;
    Exit;
  end;
  K := ShapeHandleAt(X, Y, I);
  if K = 15 then
  begin   { a connector's end }
    FShapeDrag := 15;
    FDragIdx := I;
    FShapeFrom := Point(X, Y);
    FBandTo := FShapeFrom;
    Exit(True);
  end;
  if K = 14 then
  begin   { the whole drawing: moved where it is dropped }
    FShapeDrag := 14;
    FShapeFrom := Point(X, Y);
    FBandTo := FShapeFrom;
    Exit(True);
  end;
  if K < 0 then
  begin
    if FNodeOn then
    begin   { pressed off the points: Edit Points done }
      FNodeOn := False;
      Invalidate;
    end;
    Exit;
  end;
  if K >= 10 then
  begin   { a turn, an adjustment, a point }
    if not ShapeGeom(FShapeSid, FDragGeom) or not DrawMap(FDragMap) then
      Exit;
    FDragIdx := I;
    if K = 10 then
    begin
      HP := GeomToClient(FDragMap, FDragGeom, FDragGeom.W / 2, FDragGeom.H / 2, FDragGeom.Rot);
      FRotFrom := ArcTan2(Y - HP.Y, X - HP.X);
      FRotNew := FDragGeom.Rot;
    end
    else if K = 11 then
      FAdjNew := AdjString(FDragGeom)
    else if not ShapeHandleXY(I, FNodeX, FNodeY) then
      Exit;
    FShapeDrag := K;
    FShapeFrom := Point(X, Y);
    Exit(True);
  end;
  if FShapeSid < 0 then
  begin   { the whole object: its box, resized by its handles }
    if not DrawingPlace(FShapeAt, Pg, PX, PY, W, H, JW, JH) then
      Exit;
    FShapeOld[0] := 0; FShapeOld[1] := 0; FShapeOld[2] := JW; FShapeOld[3] := JH;
  end
  else if not ShapeBox(FShapeSid, FShapeOld[0], FShapeOld[1], FShapeOld[2], FShapeOld[3]) then
    Exit;
  FShapeNew := FShapeOld;
  FShapeDrag := K;
  FShapeFrom := Point(X, Y);
  Result := True;
end;

{ the drag so far: the box it would give the shape (Shift: a resize keeps the proportions) }
procedure TParadeEdit.ShapeDragMove(X, Y: Integer; Shift: TShiftState);
var
  Pg: Int32;
  PX, PY, W, H, JW, JH, DX, DY, K: Double;
  Cp: TPoint;
begin
  if FShapeDrag in [13, 14, 15, 16] then
  begin
    FBandTo := Point(X, Y);
    Invalidate;
    Exit;
  end;
  if (FShapeDrag < 0) or not DrawingPlace(FShapeAt, Pg, PX, PY, W, H, JW, JH) or (W <= 0) or (H <= 0) then
    Exit;
  if (FShapeDrag = 9) and (FDrawKind = 'scribble') then
  begin   { the hand's line, a point at a time }
    SetLength(FDrawPts, Length(FDrawPts) + 2);
    FDrawPts[High(FDrawPts) - 1] := ((X - PageLeft(Pg)) / PxPerSp - PX) * JW / W;
    FDrawPts[High(FDrawPts)] := ((Y - PageTop(Pg)) / PxPerSp - PY) * JH / H;
    Invalidate;
    Exit;
  end;
  if FShapeDrag = 10 then
  begin   { turned by the angle the mouse has gone round the centre (Shift: by fifteen degrees) }
    Cp := GeomToClient(FDragMap, FDragGeom, FDragGeom.W / 2, FDragGeom.H / 2, FDragGeom.Rot);
    K := FDragGeom.Rot + (ArcTan2(Y - Cp.Y, X - Cp.X) - FRotFrom) * 180 / Pi;
    if ssShift in Shift then
      K := Round(K / 15) * 15;
    FRotNew := K - 360 * Floor(K / 360);
    Invalidate;
    Exit;
  end;
  if FShapeDrag = 11 then
  begin
    ClientToGeom(FDragMap, FDragGeom, X, Y, DX, DY);
    FAdjNew := PresetDrag(FDragGeom, FDragIdx, DX, DY);
    Invalidate;
    Exit;
  end;
  if FShapeDrag = 12 then
  begin   { into the path's units }
    ClientToGeom(FDragMap, FDragGeom, X, Y, DX, DY);
    if FDragGeom.W > 0 then FNodeX := DX * FDragPW / FDragGeom.W else FNodeX := 0;
    if FDragGeom.H > 0 then FNodeY := DY * FDragPH / FDragGeom.H else FNodeY := 0;
    Invalidate;
    Exit;
  end;
  DX := (X - FShapeFrom.X) / PxPerSp * JW / W;   { client pixels into the drawing's units }
  DY := (Y - FShapeFrom.Y) / PxPerSp * JH / H;
  FShapeNew := FShapeOld;
  case FShapeDrag of
    9: begin    { a shape being drawn: from where the press was }
         FShapeNew[2] := FShapeOld[0] + DX;
         FShapeNew[3] := FShapeOld[1] + DY;
       end;
    8: begin
         FShapeNew[0] := FShapeOld[0] + DX; FShapeNew[2] := FShapeOld[2] + DX;
         FShapeNew[1] := FShapeOld[1] + DY; FShapeNew[3] := FShapeOld[3] + DY;
       end;
  else
    begin
      if FShapeDrag in [0, 6, 7] then FShapeNew[0] := FShapeOld[0] + DX;
      if FShapeDrag in [2, 3, 4] then FShapeNew[2] := FShapeOld[2] + DX;
      if FShapeDrag in [0, 1, 2] then FShapeNew[1] := FShapeOld[1] + DY;
      if FShapeDrag in [4, 5, 6] then FShapeNew[3] := FShapeOld[3] + DY;
      if (ssShift in Shift) and (FShapeDrag in [0, 2, 4, 6]) and (FShapeOld[2] > FShapeOld[0]) and
         (FShapeOld[3] > FShapeOld[1]) then
      begin   { a corner: the proportions kept, by the larger change }
        K := Max((FShapeNew[2] - FShapeNew[0]) / (FShapeOld[2] - FShapeOld[0]),
                 (FShapeNew[3] - FShapeNew[1]) / (FShapeOld[3] - FShapeOld[1]));
        if FShapeDrag in [0, 6] then
          FShapeNew[0] := FShapeNew[2] - (FShapeOld[2] - FShapeOld[0]) * K
        else
          FShapeNew[2] := FShapeNew[0] + (FShapeOld[2] - FShapeOld[0]) * K;
        if FShapeDrag in [0, 2] then
          FShapeNew[1] := FShapeNew[3] - (FShapeOld[3] - FShapeOld[1]) * K
        else
          FShapeNew[3] := FShapeNew[1] + (FShapeOld[3] - FShapeOld[1]) * K;
      end;
    end;
  end;
  SetLength(FGuideX, 0);
  SetLength(FGuideY, 0);
  if not (ssAlt in Shift) and ((FShapeSid >= 0) or (FShapeDrag = 9)) and
     not ((ssShift in Shift) and (FShapeDrag in [0, 2, 4, 6])) then
    SnapDrag(JW, JH, 6 / PxPerSp * JW / W);    { six pixels }
  Invalidate;
end;

{ the drag's box (FShapeNew) snapped: its moving edges (a move: its edges and middle) to the nearest of the other
  shapes' edges and middles, the canvas's, or the grid, within Thr (the drawing's units); the lines snapped to kept
  to show }
procedure TParadeEdit.SnapDrag(JW, JH, Thr: Double);
var
  Boxes: TParadeShapeBoxes;
  CX, CY: array of Double;
  Sids: TIntegerArray;
  I, K: Integer;
  Skip: Boolean;
  G: Double;

  procedure Add(var A: array of Double; var N: Integer; V: Double);
  begin
    A[N] := V;
    Inc(N);
  end;

  { the shift putting one of the edges Es on a line, the nearest within Thr; the line }
  function Best(const Es: array of Double; const Cs: array of Double; NC: Integer; out Line: Double): Double;
  var
    A, B: Integer;
    D, BD, V: Double;
  begin
    Result := 0;
    Line := NaN;
    BD := Thr;
    for A := 0 to High(Es) do
    begin
      for B := 0 to NC - 1 do
      begin
        D := Cs[B] - Es[A];
        if Abs(D) < BD then
        begin
          BD := Abs(D);
          Result := D;
          Line := Cs[B];
        end;
      end;
      if IsNan(Line) and (G > 0) and (A = 0) then
      begin   { the grid, when no shape is near: its first edge (a move: its top left corner) }
        V := Round(Es[A] / G) * G;    { the nearest grid line, however far }
        Result := V - Es[A];
      end;
    end;
  end;

var
  NX, NY: Integer;
  D, L: Double;
begin
  SetLength(FGuideX, 0);
  SetLength(FGuideY, 0);
  G := FSnapGrid * PD_SP_PER_PT;
  if not FSnapShapes and (G <= 0) then
    Exit;
  Boxes := DrawingShapes(FShapeAt);
  Sids := SelectedShapes;
  SetLength(CX, 3 * Length(Boxes) + 3);
  SetLength(CY, 3 * Length(Boxes) + 3);
  NX := 0;
  NY := 0;
  if FSnapShapes then
  begin
    Add(CX, NX, 0); Add(CX, NX, JW / 2); Add(CX, NX, JW);
    Add(CY, NY, 0); Add(CY, NY, JH / 2); Add(CY, NY, JH);
    for I := 0 to High(Boxes) do
    begin
      Skip := FShapeDrag = 9;
      for K := 0 to High(Sids) do
        Skip := Skip or (Boxes[I].Sid = Sids[K]);
      if Skip and (FShapeDrag <> 9) then
        Continue;
      Add(CX, NX, Boxes[I].X0); Add(CX, NX, (Boxes[I].X0 + Boxes[I].X1) / 2); Add(CX, NX, Boxes[I].X1);
      Add(CY, NY, Boxes[I].Y0); Add(CY, NY, (Boxes[I].Y0 + Boxes[I].Y1) / 2); Add(CY, NY, Boxes[I].Y1);
    end;
  end;
  case FShapeDrag of
    8:
      begin   { moved: whichever of its edges or its middle is nearest a line }
        D := Best([FShapeNew[0], (FShapeNew[0] + FShapeNew[2]) / 2, FShapeNew[2]], CX, NX, L);
        FShapeNew[0] := FShapeNew[0] + D;
        FShapeNew[2] := FShapeNew[2] + D;
        if not IsNan(L) then FGuideX := [L];
        D := Best([FShapeNew[1], (FShapeNew[1] + FShapeNew[3]) / 2, FShapeNew[3]], CY, NY, L);
        FShapeNew[1] := FShapeNew[1] + D;
        FShapeNew[3] := FShapeNew[3] + D;
        if not IsNan(L) then FGuideY := [L];
      end;
    9:
      begin   { drawn: where it ends }
        D := Best([FShapeNew[2]], CX, NX, L);
        FShapeNew[2] := FShapeNew[2] + D;
        if not IsNan(L) then FGuideX := [L];
        D := Best([FShapeNew[3]], CY, NY, L);
        FShapeNew[3] := FShapeNew[3] + D;
        if not IsNan(L) then FGuideY := [L];
      end;
  else
    begin   { sized: the edges its handle moves }
      if FShapeDrag in [0, 6, 7] then K := 0 else if FShapeDrag in [2, 3, 4] then K := 2 else K := -1;
      if K >= 0 then
      begin
        D := Best([FShapeNew[K]], CX, NX, L);
        FShapeNew[K] := FShapeNew[K] + D;
        if not IsNan(L) then FGuideX := [L];
      end;
      if FShapeDrag in [0, 1, 2] then K := 1 else if FShapeDrag in [4, 5, 6] then K := 3 else K := -1;
      if K >= 0 then
      begin
        D := Best([FShapeNew[K]], CY, NY, L);
        FShapeNew[K] := FShapeNew[K] + D;
        if not IsNan(L) then FGuideY := [L];
      end;
    end;
  end;
end;

{ the selected shape moved by DX, DY points on the page }
function TParadeEdit.NudgeShape(DX, DY: Double): Boolean;
var
  Pg: Int32;
  PX, PY, W, H, JW, JH, X0, Y0, X1, Y1: Double;
begin
  Result := FShapeOn and (FShapeSid >= 0) and DrawingPlace(FShapeAt, Pg, PX, PY, W, H, JW, JH) and (W > 0) and
            (H > 0) and ShapeBox(FShapeSid, X0, Y0, X1, Y1);
  if not Result then
    Exit;
  DX := DX * PD_SP_PER_PT * JW / W;
  DY := DY * PD_SP_PER_PT * JH / H;
  if Length(FShapeMore) > 0 then
    Result := MoveShapes(DX, DY)
  else
    Result := SetShapeBox(FShapeSid, X0 + DX, Y0 + DY, X1 + DX, Y1 + DY);
end;

function TParadeEdit.SelectedFloat: pd_block_id;
var
  Info: pd_block_info;
begin
  Result := 0;
  if FShapeOn and (pd_doc_block_info(FDoc, FShapeAt.block, Info) = PD_OK) and
     (pd_doc_block_info(FDoc, Info.parent, Info) = PD_OK) and (Info.kind = PD_BLOCK_FLOAT) then
    Result := Info.id;
end;

function TParadeEdit.ObjectWrap: Integer;
var
  Fl: pd_block_id;
  Fp: pd_float_props;
begin
  Result := -2;
  if not FShapeOn then
    Exit;
  Fl := SelectedFloat;
  if Fl = 0 then
    Exit(-1);
  if pd_doc_float_props(FDoc, Fl, Fp) = PD_OK then
    Result := Fp.wrap;
end;

function TParadeEdit.SetObjectWrap(Wrap: Integer): Boolean;
var
  Fl, Sec, Anchor, B: pd_block_id;
  FI, BI, Info: pd_block_info;
  Fp: pd_float_props;
  Sp: pd_section_props;
  O: pd_inline;
  Keep: array[0..2] of string;
  At: pd_pos;
  Pg: Int32;
  X, Y, W, H, JW, JH, ColLeft: Double;
  Sid: Integer;
begin
  Result := False;
  if FReadOnly or not FShapeOn or (Wrap < -1) or (Wrap > PD_WRAP_BEHIND) or
     (pd_doc_inline_at(FDoc, FShapeAt, O) <> PD_OK) or (Wrap = ObjectWrap) then
    Exit;
  At := FShapeAt;
  Sid := FShapeSid;
  Fl := SelectedFloat;
  KeepInlineText(O, Keep);
  if not DrawingPlace(At, Pg, X, Y, W, H, JW, JH) then
    X := 0;
  PushShapeStep('Wrap text', At);
  pd_doc_begin_group(FDoc, 'Wrap text');
  try
    if (Fl <> 0) and (Wrap = -1) then
    begin   { into the text: at the start of the paragraph it was anchored in, the float gone }
      if pd_doc_block_info(FDoc, Fl, FI) <> PD_OK then
        Exit;
      Anchor := pd_doc_child(FDoc, FI.parent, FI.index + 1);
      if (Anchor = 0) or (pd_doc_block_info(FDoc, Anchor, Info) <> PD_OK) or (Info.kind <> PD_BLOCK_PARAGRAPH) then
        if pd_doc_insert_block(FDoc, FI.parent, FI.index + 1, PD_BLOCK_PARAGRAPH, Anchor) <> PD_OK then
          Exit;
      if pd_doc_insert_inline(FDoc, PdPos(Anchor, 0), O, nil) <> PD_OK then
        Exit;
      Result := pd_doc_remove_block(FDoc, Fl) = PD_OK;
      At := PdPos(Anchor, 0);
    end
    else if Fl = 0 then
    begin   { out of the line: a float before its paragraph, anchored there, across the column where it was }
      if (pd_doc_block_info(FDoc, At.block, BI) <> PD_OK) or
         (pd_doc_insert_block(FDoc, BI.parent, BI.index, PD_BLOCK_FLOAT, Fl) <> PD_OK) then
        Exit;
      pd_doc_delete(FDoc, PdRange(At, PdPos(At.block, At.offset + 3)), nil);
      B := pd_doc_child(FDoc, Fl, 0);
      if pd_doc_insert_inline(FDoc, PdPos(B, 0), O, nil) <> PD_OK then
        Exit;
      pd_doc_float_props(FDoc, Fl, Fp);
      Fp.placement := PD_PLACE_HERE or PD_PLACE_FORCE;
      Fp.width := O.width;
      Fp.gap := Round(9 * PD_SP_PER_PT);
      Fp.wrap := Wrap;
      ColLeft := 0;
      Sec := BI.parent;
      while (Sec <> 0) and (pd_doc_block_info(FDoc, Sec, Info) = PD_OK) and (Info.kind <> PD_BLOCK_SECTION) do
        Sec := Info.parent;
      if (Sec <> 0) and (pd_doc_section_props(FDoc, Sec, Sp) = PD_OK) then
        ColLeft := Sp.margin_left;
      if (Wrap <> PD_WRAP_NONE) and (X > 0) then
      begin
        Fp.placement := Fp.placement or PD_PLACE_OFFSET;
        Fp.offset_x := Round(X - ColLeft);
      end;
      Result := pd_doc_set_float_props(FDoc, Fl, Fp) = PD_OK;
      At := PdPos(B, 0);
    end
    else
    begin   { another way round the float }
      pd_doc_float_props(FDoc, Fl, Fp);
      Fp.wrap := Wrap;
      if Wrap in [PD_WRAP_LEFT, PD_WRAP_RIGHT] then
        Fp.placement := Fp.placement and not PD_PLACE_OFFSET;     { against that side }
      Result := pd_doc_set_float_props(FDoc, Fl, Fp) = PD_OK;
    end;
  finally
    pd_doc_end_group(FDoc);
  end;
  Changed;
  FShapeOn := True;
  FShapeAt := At;
  FShapeSid := Sid;
  if Sid < 0 then
    SelectObject(At)
  else
    SetCaret(At, False);
  Invalidate;
end;

function TParadeEdit.OnDrawingBody(X, Y: Integer): Boolean;
var
  Pg: Int32;
  PX, PY, W, H, JW, JH: Double;
  R: TRect;
begin
  Result := False;
  if not FShapeOn or (FShapeSid >= 0) or (FDrawKind <> '') or FReadOnly or FCanvasPage or
     not DrawingPlace(FShapeAt, Pg, PX, PY, W, H, JW, JH) then
    Exit;
  R := Rect(PageLeft(Pg) + Round(PX * PxPerSp), PageTop(Pg) + Round(PY * PxPerSp),
    PageLeft(Pg) + Round((PX + W) * PxPerSp), PageTop(Pg) + Round((PY + H) * PxPerSp));
  if not PtInRect(Rect(R.Left - 3, R.Top - 3, R.Right + 4, R.Bottom + 4), Point(X, Y)) then
    Exit;
  Result := not CanvasSelected or (X < R.Left + 5) or (X > R.Right - 5) or (Y < R.Top + 5) or (Y > R.Bottom - 5);
end;

function TParadeEdit.MoveObjectTo(const P: pd_pos): Boolean;
var
  O: pd_inline;
  Keep: array[0..2] of string;
  S: TParadeBlockArray;
  At, Q: pd_pos;
  I: Integer;
begin
  Result := False;
  At := FShapeAt;
  if FReadOnly or not FShapeOn or (SelectedFloat <> 0) or (pd_doc_inline_at(FDoc, At, O) <> PD_OK) then
    Exit;
  if (P.block = At.block) and (P.offset >= At.offset) and (P.offset <= At.offset + 3) then
    Exit;     { where it is }
  S := ResStories(FDoc, O.resource);
  for I := 0 to High(S) do
    if StoryTopOf(P.block) = S[I] then
      Exit;   { into its own text box }
  KeepInlineText(O, Keep);
  PushShapeStep('Move', At);
  Q := P;
  if (Q.block = At.block) and (Q.offset > At.offset) then
    Dec(Q.offset, 3);
  pd_doc_begin_group(FDoc, 'Move');
  pd_doc_delete(FDoc, PdRange(At, PdPos(At.block, At.offset + 3)), nil);
  Result := pd_doc_insert_inline(FDoc, Q, O, nil) = PD_OK;
  pd_doc_end_group(FDoc);
  Changed;
  if Result then
  begin
    FShapeOn := True;
    FShapeAt := Q;
    FShapeSid := -1;
    SetLength(FShapeMore, 0);
    SelectObject(Q);
  end;
  Invalidate;
end;

function TParadeEdit.MoveFloatBy(DX, DY: pd_sp): Boolean;
var
  Fl, Sec, T, A0, B: pd_block_id;
  FI, Info: pd_block_info;
  Fp: pd_float_props;
  Sp: pd_section_props;
  Pg, CPage: Int32;
  X, Y, W, H, JW, JH, NX, NY, D0: Double;
  CX, Base, Asc, Desc: pd_sp;
  I, Ti: Integer;

  function LineTop(Blk: pd_block_id; out APage: Int32): Double;
  begin
    if pd_layout_caret(FLayout, PdPos(Blk, 0), APage, CX, Base, Asc, Desc) = PD_OK then
      Result := Base - Asc
    else
    begin
      APage := -1;
      Result := 0;
    end;
  end;

begin
  Result := False;
  Fl := SelectedFloat;
  if FReadOnly or (Fl = 0) or (pd_doc_block_info(FDoc, Fl, FI) <> PD_OK) or
     (pd_doc_float_props(FDoc, Fl, Fp) <> PD_OK) or not DrawingPlace(FShapeAt, Pg, X, Y, W, H, JW, JH) then
    Exit;
  Sec := FI.parent;
  if (pd_doc_block_info(FDoc, Sec, Info) <> PD_OK) or (Info.kind <> PD_BLOCK_SECTION) or
     (pd_doc_section_props(FDoc, Sec, Sp) <> PD_OK) then
    Exit;
  NX := X + DX;
  NY := Y + DY;
  if Fp.offset_from <> PD_FROM_PARAGRAPH then
  begin   { placed from the page: as far again down it, anchored where it is }
    PushShapeStep('Move', FShapeAt);
    Fp.offset_y := Fp.offset_y + DY;
    if Fp.wrap <> PD_WRAP_NONE then
    begin
      Fp.placement := Fp.placement or PD_PLACE_OFFSET;
      Fp.offset_x := Round(NX - Sp.margin_left);
    end;
    pd_doc_begin_group(FDoc, 'Move');
    Result := pd_doc_set_float_props(FDoc, Fl, Fp) = PD_OK;
    pd_doc_end_group(FDoc);
    Changed;
    Invalidate;
    Exit;
  end;
  { how far above its anchor's first line the drawing is placed from (the gap between them) }
  A0 := pd_doc_child(FDoc, Sec, FI.index + 1);
  D0 := 0;
  if (A0 <> 0) and (pd_doc_block_info(FDoc, A0, Info) = PD_OK) and (Info.kind = PD_BLOCK_PARAGRAPH) then
  begin
    D0 := LineTop(A0, CPage) - (Y - Fp.offset_y);
    if CPage <> Pg then
      D0 := 0;
  end;
  { the paragraph it is moved by: the last of its page whose first line is above where it now starts }
  T := 0;
  Ti := -1;
  for I := 0 to ChildCount(Sec) - 1 do
  begin
    B := pd_doc_child(FDoc, Sec, I);
    if (pd_doc_block_info(FDoc, B, Info) <> PD_OK) or (Info.kind <> PD_BLOCK_PARAGRAPH) then
      Continue;
    if (LineTop(B, CPage) <= NY) and (CPage = Pg) then
    begin
      T := B;
      Ti := I;
    end
    else if (CPage = Pg) and (T = 0) then
    begin   { above the page's first: that one }
      T := B;
      Ti := I;
      Break;
    end
    else if CPage > Pg then
      Break;
  end;
  if T = 0 then
    Exit;
  PushShapeStep('Move', FShapeAt);
  pd_doc_begin_group(FDoc, 'Move');
  try
    if T <> A0 then
    begin
      if Ti > FI.index then
        Dec(Ti);    { counted without it }
      if pd_doc_move_block(FDoc, Fl, Sec, Ti) <> PD_OK then
        Exit;
    end;
    Fp.offset_y := Max(0, Round(NY - LineTop(T, CPage) + D0));
    if Fp.wrap <> PD_WRAP_NONE then
    begin
      Fp.placement := Fp.placement or PD_PLACE_OFFSET;
      Fp.offset_x := Round(NX - Sp.margin_left);
    end;
    Result := pd_doc_set_float_props(FDoc, Fl, Fp) = PD_OK;
    if Result then
    begin   { laid out again: as far down as it was dragged, what the paragraphs above did to that put right }
      Relayout;
      if DrawingPlace(FShapeAt, CPage, X, Y, W, H, JW, JH) and (CPage = Pg) and (Abs(Y - NY) > PD_SP_PER_PT) and
         (Fp.offset_y + Round(NY - Y) >= 0) then
      begin
        Fp.offset_y := Fp.offset_y + Round(NY - Y);
        pd_doc_set_float_props(FDoc, Fl, Fp);
      end;
    end;
  finally
    pd_doc_end_group(FDoc);
  end;
  Changed;
  SelectObject(FShapeAt);
  FShapeOn := True;
  FShapeSid := -1;
  Invalidate;
end;

function TParadeEdit.MoveShapes(DX, DY: Double): Boolean;
var
  Sids: TIntegerArray;
  I: Integer;
  X0, Y0, X1, Y1: Double;
begin
  Result := False;
  Sids := SelectedShapes;
  if FReadOnly or (Length(Sids) = 0) then
    Exit;
  PushShapeStep('Move shapes', FShapeAt);
  Inc(FStepDepth);
  pd_doc_begin_group(FDoc, 'Move shapes');
  try
    for I := 0 to High(Sids) do
      if ShapeBox(Sids[I], X0, Y0, X1, Y1) then
        Result := SetShapeBox(Sids[I], X0 + DX, Y0 + DY, X1 + DX, Y1 + DY) or Result;
  finally
    pd_doc_end_group(FDoc);
    Dec(FStepDepth);
  end;
  FShapeOn := True;     { the same shapes, still selected }
  FShapeSid := Sids[0];
  SetLength(FShapeMore, Length(Sids) - 1);
  for I := 1 to High(Sids) do
    FShapeMore[I - 1] := Sids[I];
  Invalidate;
end;

function TParadeEdit.PlaceShapes(const Lbl: string; const Sids: TIntegerArray; const Boxes: array of Double): Boolean;
var
  I: Integer;
begin
  Result := False;
  if FReadOnly or (Length(Sids) = 0) then
    Exit;
  PushShapeStep(Lbl, FShapeAt);
  Inc(FStepDepth);
  pd_doc_begin_group(FDoc, PAnsiChar(Lbl));
  try
    for I := 0 to High(Sids) do
      Result := SetShapeBox(Sids[I], Boxes[4 * I], Boxes[4 * I + 1], Boxes[4 * I + 2], Boxes[4 * I + 3]) or Result;
  finally
    pd_doc_end_group(FDoc);
    Dec(FStepDepth);
  end;
  FShapeOn := True;
  FShapeSid := Sids[0];
  SetLength(FShapeMore, Length(Sids) - 1);
  for I := 1 to High(Sids) do
    FShapeMore[I - 1] := Sids[I];
  Invalidate;
end;

function TParadeEdit.AlignShapes(Mode: Integer): Boolean;
var
  Sids: TIntegerArray;
  B: array of Double;
  I: Integer;
  X0, Y0, X1, Y1, D: Double;
  Pg: Int32;
  PX, PY, W, H: Double;
begin
  OnlyShape;
  Result := False;
  Sids := SelectedShapes;
  if Length(Sids) = 0 then
    Exit;
  SetLength(B, 4 * Length(Sids));
  X0 := 1e300; Y0 := 1e300; X1 := -1e300; Y1 := -1e300;
  for I := 0 to High(Sids) do
  begin
    if not ShapeBox(Sids[I], B[4 * I], B[4 * I + 1], B[4 * I + 2], B[4 * I + 3]) then
      Exit;
    X0 := Min(X0, B[4 * I]); Y0 := Min(Y0, B[4 * I + 1]);
    X1 := Max(X1, B[4 * I + 2]); Y1 := Max(Y1, B[4 * I + 3]);
  end;
  if (Length(Sids) = 1) and DrawingPlace(FShapeAt, Pg, PX, PY, W, H, X1, Y1) then
  begin   { one: along the canvas }
    X0 := 0;
    Y0 := 0;
  end;
  for I := 0 to High(Sids) do
  begin
    case Mode of
      0: D := X0 - B[4 * I];
      1: D := (X0 + X1) / 2 - (B[4 * I] + B[4 * I + 2]) / 2;
      2: D := X1 - B[4 * I + 2];
      3: D := Y0 - B[4 * I + 1];
      4: D := (Y0 + Y1) / 2 - (B[4 * I + 1] + B[4 * I + 3]) / 2;
    else
      D := Y1 - B[4 * I + 3];
    end;
    if Mode <= 2 then
    begin
      B[4 * I] := B[4 * I] + D;
      B[4 * I + 2] := B[4 * I + 2] + D;
    end
    else
    begin
      B[4 * I + 1] := B[4 * I + 1] + D;
      B[4 * I + 3] := B[4 * I + 3] + D;
    end;
  end;
  Result := PlaceShapes('Align', Sids, B);
end;

function TParadeEdit.DistributeShapes(Horizontal: Boolean): Boolean;
var
  Sids, Ord_: TIntegerArray;
  B: array of Double;
  I, K, T, A: Integer;
  Total, Gap, At: Double;
begin
  Result := False;
  Sids := SelectedShapes;
  if Length(Sids) < 3 then
    Exit;
  SetLength(B, 4 * Length(Sids));
  for I := 0 to High(Sids) do
    if not ShapeBox(Sids[I], B[4 * I], B[4 * I + 1], B[4 * I + 2], B[4 * I + 3]) then
      Exit;
  A := Ord(not Horizontal);    { 0: across (x), 1: down (y) }
  SetLength(Ord_, Length(Sids));
  for I := 0 to High(Sids) do
    Ord_[I] := I;
  for I := 0 to High(Ord_) do   { by where they start }
    for K := I + 1 to High(Ord_) do
      if B[4 * Ord_[K] + A] < B[4 * Ord_[I] + A] then
      begin
        T := Ord_[I]; Ord_[I] := Ord_[K]; Ord_[K] := T;
      end;
  Total := 0;
  for I := 0 to High(Ord_) do
    Total := Total + B[4 * Ord_[I] + A + 2] - B[4 * Ord_[I] + A];
  Gap := (B[4 * Ord_[High(Ord_)] + A + 2] - B[4 * Ord_[0] + A] - Total) / (Length(Ord_) - 1);
  At := B[4 * Ord_[0] + A];
  for I := 0 to High(Ord_) do
  begin   { each after the one before, the same gap between }
    K := Ord_[I];
    Total := B[4 * K + A + 2] - B[4 * K + A];
    B[4 * K + A] := At;
    B[4 * K + A + 2] := At + Total;
    At := At + Total + Gap;
  end;
  Result := PlaceShapes('Distribute', Sids, B);
end;

function TParadeEdit.SelectShapesIn(X0, Y0, X1, Y1: Double; Add: Boolean): Integer;
var
  Boxes: TParadeShapeBoxes;
  Sids: TIntegerArray;
  I, K: Integer;
  T: Double;
  Have: Boolean;
begin
  Result := 0;
  if not FShapeOn then
    Exit;
  if X1 < X0 then begin T := X0; X0 := X1; X1 := T; end;
  if Y1 < Y0 then begin T := Y0; Y0 := Y1; Y1 := T; end;
  Sids := nil;
  if Add then
    Sids := SelectedShapes;
  Boxes := DrawingShapes(FShapeAt);
  for I := 0 to High(Boxes) do
    if (Boxes[I].X0 >= X0) and (Boxes[I].Y0 >= Y0) and (Boxes[I].X1 <= X1) and (Boxes[I].Y1 <= Y1) then
    begin
      Have := False;
      for K := 0 to High(Sids) do
        Have := Have or (Sids[K] = Boxes[I].Sid);
      if not Have then
      begin
        SetLength(Sids, Length(Sids) + 1);
        Sids[High(Sids)] := Boxes[I].Sid;
      end;
    end;
  Result := Length(Sids);
  FNodeOn := False;
  if Result = 0 then
  begin
    FShapeSid := -1;
    SetLength(FShapeMore, 0);
    SelectObject(FShapeAt);
  end
  else
  begin
    FShapeSid := Sids[0];
    SetLength(FShapeMore, Result - 1);
    for I := 1 to High(Sids) do
      FShapeMore[I - 1] := Sids[I];
    SetCaret(FShapeAt, False);
  end;
  Invalidate;
end;

procedure TParadeEdit.MouseDown(Button: TMouseButton; Shift: TShiftState; X, Y: Integer);
var
  P: pd_pos;
  I, Page: Integer;
  Info: pd_page_info;
begin
  inherited MouseDown(Button, Shift, X, Y);
  SetFocus;
  if Button = mbLeft then
    for I := 0 to High(FBalloons) do
      if PtInRect(FBalloons[I].R, Point(X, Y)) then
      begin
        SelectRange(FBalloons[I].Range);
        Exit;
      end;
  if (Button = mbLeft) and OnPageCorner(X, Y) then
  begin   { the canvas page's corner: the page, and its canvas, sized }
    FShapeDrag := 16;
    FShapeFrom := Point(X, Y);
    FBandTo := FShapeFrom;
    Exit;
  end;
  if (Button = mbLeft) and (ssTriple in Shift) and not FShapeOn and PointToPos(X, Y, P) then
  begin   { the third press: the paragraph (the LCL calls TripleClick before this press, which would undo it) }
    FDragging := False;
    pd_doc_marker_set(FDoc, FAnchor, PdPos(P.block, 0));
    pd_doc_marker_set(FDoc, FCaret, PdPos(P.block, Length(ParaText(P.block))));
    FHasDesiredX := False;
    Invalidate;
    Exit;
  end;
  if (Button = mbLeft) and ShapeDragStart(X, Y) then
    Exit;     { the selected shape: moved, or resized by a handle }
  if (Button = mbLeft) and (not (ssShift in Shift) or (FShapeOn and (FShapeSid >= 0))) then
    for Page := 0 to PageCount - 1 do     { a drawing, or a shape of it: selected }
    begin
      pd_layout_page_info(FLayout, Page, Info);
      if (Y < PageTop(Page) + Round(Info.height * PxPerSp) + FPageGap div 2) or (Page = PageCount - 1) then
      begin
        if ClickPage(Page, Round((X - PageLeft(Page)) / PxPerSp), Round((Y - PageTop(Page)) / PxPerSp), Shift) then
        begin
          if FShapeOn and (FShapeSid < 0) and not (ssDouble in Shift) and OnDrawingBody(X, Y) then
          begin   { a picture, or a canvas by its edge: moved where it is dropped }
            FShapeDrag := 14;
            FShapeFrom := Point(X, Y);
            FBandTo := FShapeFrom;
          end
          else if FShapeOn and (FShapeSid < 0) and not (ssDouble in Shift) and CanvasSelected then
          begin   { the canvas's own ground: a drag there a rubber band, round the shapes to select }
            FShapeDrag := 13;
            FShapeFrom := Point(X, Y);
            FBandTo := FShapeFrom;
          end;
          Exit;
        end;
        Break;
      end;
    end;
  if (Button = mbLeft) and PointToPos(X, Y, P) then
  begin
    FHasDesiredX := False;
    SetCaret(P, ssShift in Shift);
    if (Shift * [ssShift, ssCtrl, ssDouble] = []) and ClickControl(P, X, Y) then
      Exit;     { a check box ticked, a list or a calendar shown: not the start of a drag }
    FDragging := True;
  end;
end;

procedure TParadeEdit.MouseMove(Shift: TShiftState; X, Y: Integer);
var
  P, D: pd_pos;
  Sid: Integer;
  Want: TCursor;
  Pg: Int32;
  PX, PY, W, H, JW, JH: Double;
  Edge: Boolean;
begin
  inherited MouseMove(Shift, X, Y);
  if FShapeDrag >= 0 then
  begin
    ShapeDragMove(X, Y, Shift);
    Exit;
  end;
  if ((FDrawKind = 'freeform') or (FDrawKind = 'curve')) and (Length(FDrawPts) > 0) then
  begin   { the line from the last corner to the mouse }
    FPolyMouse := Point(X, Y);
    Invalidate;
  end;
  { over the edge of the text box being typed in: the move cursor, where a press takes the box; over a handle of
    the selected shape, what a drag of it does }
  if FDrawKind = '' then
  begin
    Want := crDefault;
    Edge := not FDragging and not FShapeOn and CaretTextBox(D, Sid) and (Sid >= 0) and
      DrawingPlace(D, Pg, PX, PY, W, H, JW, JH) and
      (TextBoxEdgeAt(D, Round((X - PageLeft(Pg)) / PxPerSp), Round((Y - PageTop(Pg)) / PxPerSp)) = Sid);
    if Edge then
      Want := crSizeAll
    else if not FDragging and OnPageCorner(X, Y) then
      Want := crSizeNWSE
    else if not FDragging and FShapeOn then
      case ShapeHandleAt(X, Y, Sid) of
        0, 4: Want := crSizeNWSE;
        2, 6: Want := crSizeNESW;
        1, 5: Want := crSizeNS;
        3, 7: Want := crSizeWE;
        8, 14: Want := crSizeAll;
        10, 11, 15: Want := crHandPoint;
        12: Want := crCross;
      end;
    HoverCursor(Want);
  end;
  if FDragging and PointToPos(X, Y, P) then
  begin
    pd_doc_marker_set(FDoc, FCaret, P);
    Invalidate;
  end;
end;

procedure TParadeEdit.MouseUp(Button: TMouseButton; Shift: TShiftState; X, Y: Integer);
var
  DM: TParadeDrawMap;
  PS, PE: TPoint;
  SS, SI, ES, EI: Integer;
  SX, SY, EX, EY: Double;
  D, I: Integer;
  P: pd_pos;
  Boxes: TParadeShapeBoxes;
  Moved: Boolean;
  K: string;
  Pg: Int32;
  PX, PY, W, H, JW, JH: Double;
begin
  inherited MouseUp(Button, Shift, X, Y);
  FDragging := False;
  if FShapeDrag >= 0 then
  begin
    D := FShapeDrag;
    FShapeDrag := -1;
    SetLength(FGuideX, 0);
    SetLength(FGuideY, 0);
    Moved := (Abs(X - FShapeFrom.X) > 2) or (Abs(Y - FShapeFrom.Y) > 2);
    if D = 16 then
    begin   { the canvas page's corner dropped: the page and its canvas that big }
      if Moved then
        ResizeCanvasPage(Round((X - PageLeft(FCornerPage)) / PxPerSp), Round((Y - PageTop(FCornerPage)) / PxPerSp));
    end
    else if D = 15 then
    begin   { a connector's end: on the site it is dropped by, or just there }
      if Moved then
      begin
        if not NearestSite(X, Y, FShapeSid, SS, SI, SX, SY) then
        begin
          SS := -1;
          SI := -1;
          ClientToDrawing(X, Y, SX, SY);
        end;
        MoveConnectorEnd(FDragIdx = 1, SX, SY, SS, SI);
      end;
    end
    else if D = 14 then
    begin   { the whole drawing dropped: floating, where it is now; in the text, at the text there }
      if Moved and (SelectedFloat <> 0) then
        MoveFloatBy(Round((X - FShapeFrom.X) / PxPerSp), Round((Y - FShapeFrom.Y) / PxPerSp))
      else if Moved and PointToPos(X, Y, P) then
        MoveObjectTo(P);
    end
    else if D = 13 then
    begin   { the rubber band: the shapes wholly in it }
      if Moved and ClientToDrawing(FShapeFrom.X, FShapeFrom.Y, PX, PY) and ClientToDrawing(X, Y, W, H) then
        SelectShapesIn(PX, PY, W, H, ssShift in Shift);
    end
    else if (D = 9) and (FDrawKind = 'scribble') then
      FinishDrawPath(False)
    else if D = 9 then
    begin   { drawn: its box as dragged, or an inch by three quarters where a click was }
      K := FDrawKind;
      FDrawKind := '';
      Cursor := FDrawCursor;
      if Moved and LineKind(K) and DrawMap(DM) then
      begin   { a connector: its ends on the sites of shapes they are dropped by }
        PS := MapToClient(DM, FShapeNew[0], FShapeNew[1]);
        PE := Point(X, Y);
        if not NearestSite(PS.X, PS.Y, -1, SS, SI, SX, SY) then
        begin
          SX := FShapeNew[0]; SY := FShapeNew[1];
        end;
        if not NearestSite(PE.X, PE.Y, -1, ES, EI, EX, EY) then
        begin
          EX := FShapeNew[2]; EY := FShapeNew[3];
        end;
        AddConnector(K, SX, SY, EX, EY, SS, SI, ES, EI);
        Invalidate;
        Exit;
      end;
      if not Moved then
      begin
        FShapeNew[2] := FShapeNew[0] + PD_SP_PER_PT * 72;
        FShapeNew[3] := FShapeNew[1] + PD_SP_PER_PT * IfThen((K = 'line') or (K = 'arrow') or (K = 'doubleArrow'), 0, 54);
      end;
      if AddShape(K, Min(FShapeNew[0], FShapeNew[2]), Min(FShapeNew[1], FShapeNew[3]), Max(FShapeNew[0], FShapeNew[2]),
        Max(FShapeNew[1], FShapeNew[3]), FShapeNew[2] < FShapeNew[0], FShapeNew[3] < FShapeNew[1]) and (K = 'textbox') then
        EnterTextBox(FShapeSid, CaretPos, False, False);    { a text box drawn: typed in at once }
    end
    else if D = 10 then
    begin
      if Moved then
        RotateShape(FRotNew);
    end
    else if D = 11 then
    begin
      if Moved then
        SetShapeAdjusts(FAdjNew);
    end
    else if D = 12 then
    begin
      if Moved then
        MoveShapePoint(FDragIdx, FNodeX, FNodeY);
    end
    else if Moved and (FShapeSid < 0) then
    begin   { the whole object: its new size }
      if DrawingPlace(FShapeAt, Pg, PX, PY, W, H, JW, JH) and (JW > 0) and (JH > 0) then
        ResizeObject(Max(PD_SP_PER_PT, Round(Abs(FShapeNew[2] - FShapeNew[0]) * W / JW)),
          Max(PD_SP_PER_PT, Round(Abs(FShapeNew[3] - FShapeNew[1]) * H / JH)));
    end
    else if Moved and (D = 8) and (Length(FShapeMore) > 0) then
      MoveShapes(FShapeNew[0] - FShapeOld[0], FShapeNew[1] - FShapeOld[1])   { all selected, together }
    else if Moved then     { moved: the shape takes its box }
      SetShapeBox(FShapeSid, Min(FShapeNew[0], FShapeNew[2]), Min(FShapeNew[1], FShapeNew[3]),
        Max(FShapeNew[0], FShapeNew[2]), Max(FShapeNew[1], FShapeNew[3]))
    else if (D = 8) and (FShapeSid >= 0) and PointToPos(X, Y, P) and (StoryTopOf(P.block) <> 0) then
    begin   { a click, not a drag, on the selected text box's text: the caret there, to type }
      Boxes := DrawingShapes(FShapeAt);
      for I := 0 to High(Boxes) do
        if (Boxes[I].Sid = FShapeSid) and (Boxes[I].Story = StoryTopOf(P.block)) then
        begin
          ClearShapeSelection;
          SetCaret(P, False);
          Break;
        end;
    end;
    Invalidate;
    Exit;
  end;
  if FPainter and (Button = mbLeft) and HasSelection then
    ApplyFormatPainter;     { the selection just made takes the copied look }
end;

procedure TParadeEdit.DblClick;
var
  S: string;
  A, B: UInt32;
  Q: pd_pos;

  function IsWord(I: UInt32): Boolean;
  begin
    Result := (I < UInt32(Length(S))) and ((S[I + 1] in ['0'..'9', 'A'..'Z', 'a'..'z', '_']) or (Ord(S[I + 1]) >= $80));
  end;

begin
  inherited DblClick;
  FDragging := False;
  if ((FDrawKind = 'freeform') or (FDrawKind = 'curve')) and (Length(FDrawPts) >= 4) then
  begin   { the last corner: the double click's second press put it in twice }
    if (Length(FDrawPts) >= 6) and (Abs(FDrawPts[High(FDrawPts) - 1] - FDrawPts[High(FDrawPts) - 3]) < PD_SP_PER_PT) and
       (Abs(FDrawPts[High(FDrawPts)] - FDrawPts[High(FDrawPts) - 2]) < PD_SP_PER_PT) then
      SetLength(FDrawPts, Length(FDrawPts) - 2);
    FinishDrawPath(False);
    Exit;
  end;
  if FShapeOn and (FShapeSid >= 0) and (Length(FShapeMore) = 0) then
  begin   { a shape: a text box's text to type in, the points of any other }
    FShapeDrag := -1;
    with ScreenToClient(Mouse.CursorPos) do
      if not PointToPos(X, Y, Q) then
        Q := CaretPos;
    if not EnterTextBox(FShapeSid, Q, True, False) then
      EditShapePoints;
    Exit;
  end;
  if FShapeOn then
    Exit;
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

{ a third click: the paragraph, as a second is the word }
procedure TParadeEdit.TripleClick;
var
  B: pd_block_id;
begin
  inherited TripleClick;
  FDragging := False;
  if FShapeOn then
    Exit;
  B := CaretPos.block;
  pd_doc_marker_set(FDoc, FAnchor, PdPos(B, 0));
  pd_doc_marker_set(FDoc, FCaret, PdPos(B, Length(ParaText(B))));
  FHasDesiredX := False;
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

procedure TParadeEdit.PaintPage(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double; DrawCaret: Boolean;
  OnScreen: Boolean);
var
  Bands: array of TSelBand;
  NB, K: Integer;
  Rings: TRings;
  Poly: TPtDArray;
  Info: pd_page_info;
  Items: array of pd_draw;
  N, I, PW, PH, IX, IY, Sub: Integer;
  PX: Double;
  Q: pd_pos;
  G: PGlyphBmp;
  Pic: TLazIntfImage;
  CPage, CPage2: Int32;
  CX, CBase, CAsc, CDesc, CX2, CBase2: pd_sp;
  CI: Int32;
  Cm: pd_comment;
  CKind, CSpec: string;
  CA, CB: pd_pos;
  IX2, IY2: Integer;
  VTop, VBot, PY0, PY1: Double;
  Keep: Boolean;
  J: Integer;

  { a range behind the text: one band per line, the line's full height, from
    the first character in it to the last -- the spaces between them too --
    and for a selection on to the end of the line's text where it goes on past it }
  procedure Highlight(const A, B: pd_pos; Col: UInt32; Alpha: Integer; ThroughEnd: Boolean);
  var
    I, K: Integer;
  begin
    NB := 0;
    SetLength(Bands, 0);
    for I := 0 to N - 1 do
      if ((Items[I].kind = PD_DRAW_GLYPH) or (Items[I].kind = PD_DRAW_IMAGE)) and (Items[I].region in [0, 3, 4, 5]) then
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
          if LastSel and ThroughEnd then   { its last character is selected: the selection goes on, through the line's end }
            X1 := LineEnd + Round(4 / PxScale);
          FillRectImg(Img, OX + Floor0(X0 * PxScale), OY + Round((Y - CAsc) * PxScale), OX + Round(X1 * PxScale),
            OY + Round((Y + CDesc) * PxScale), Col, Alpha);
        end;
  end;

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

  { Only what reaches into the image: the strip a scroll brings into view is
    a tenth of a page, and every glyph, rule and path of the page went
    through the loops below for it.  Each item's own extent, with a few
    pixels to spare, so an item kept or dropped draws exactly as before. }
  VTop := (-OY - 4) / PxScale;
  VBot := (Img.Height - OY + 4) / PxScale;
  K := 0;
  for I := 0 to N - 1 do
  begin
    with Items[I] do
      case kind of
        PD_DRAW_GLYPH:
          Keep := (y - 1.5 * size < VBot) and (y + size > VTop);
        PD_DRAW_PATH:
          begin
            Keep := points = nil;
            if not Keep then
            begin
              PY0 := 1e30;
              PY1 := -1e30;
              for J := 0 to npoints - 1 do
                if points[2 * J] <> PD_PATH_BREAK then
                begin
                  if points[2 * J + 1] < PY0 then PY0 := points[2 * J + 1];
                  if points[2 * J + 1] > PY1 then PY1 := points[2 * J + 1];
                end;
              Keep := (PY0 - line_width < VBot) and (PY1 + line_width > VTop);
            end;
          end;
      else
        Keep := (y < VBot) and (y + h > VTop);
      end;
    if Keep then
    begin
      if K <> I then
        Items[K] := Items[I];
      Inc(K);
    end;
  end;
  N := K;

  { comments' ranges, lightly in their authors' colours; the selection over them }
  if pd_doc_comment_count(FDoc) > 0 then
    for CI := 1 to pd_doc_comment_count(FDoc) do
      if (pd_doc_comment_get(FDoc, CI, Cm) = PD_OK) and (Cm.parent = 0) and (Cm.resolved = 0) and
         (Compare(Cm.range.start, Cm.range.finish) < 0) then
        Highlight(Cm.range.start, Cm.range.finish, pd_doc_author_color(FDoc, Cm.author) and $FFFFFF, 40, False);
  if HasSelection then
    Highlight(SelStart, SelEnd, $003390FF, 80, True);
  for CI := 0 to High(FRemote) do   { the others' selections, faintly in their colours }
    with FRemote[CI] do
      if (Pos.block <> Anchor.block) or (Pos.offset <> Anchor.offset) then
        if Compare(Anchor, Pos) < 0 then
          Highlight(Anchor, Pos, Color, 50, False)
        else
          Highlight(Pos, Anchor, Color, 50, False);

  for I := 0 to N - 1 do
    with Items[I] do
      case kind of
        PD_DRAW_RULE:
          FillRectImg(Img, OX + Round(x * PxScale), OY + Round(y * PxScale), OX + Round((x + w) * PxScale) + 1,
            OY + Round((y + h) * PxScale) + 1, color and $FFFFFF, RuleAlpha(color));
        PD_DRAW_IMAGE:
          begin
            IX := OX + Round(x * PxScale);
            IY := OY + Round(y * PxScale);
            Pic := GetPicture(resource, OX + Round((x + w) * PxScale) - IX, OY + Round((y + h) * PxScale) - IY);
            if (Pic <> nil) and ((rotation <> 0) or (flip <> 0) or (clip_npoints >= 3)) then
            begin   { turned, flipped, cut to its shape: a pixel at a time }
              SetLength(Poly, Max(0, clip_npoints));
              for K := 0 to clip_npoints - 1 do
              begin
                Poly[K].X := OX + clip_points[2 * K] * PxScale;
                Poly[K].Y := OY + clip_points[2 * K + 1] * PxScale;
              end;
              if clip_w > 0 then
                BlendPictureEx(Img, Pic, OX + (clip_x + clip_w / 2) * PxScale, OY + (clip_y + clip_h / 2) * PxScale,
                  rotation / 60000, (flip and PD_FLIP_H) <> 0, (flip and PD_FLIP_V) <> 0, True,
                  Rect(OX + Round(clip_x * PxScale), OY + Round(clip_y * PxScale), OX + Round((clip_x + clip_w) * PxScale),
                  OY + Round((clip_y + clip_h) * PxScale)), Poly)
              else
                BlendPictureEx(Img, Pic, OX + (x + w / 2) * PxScale, OY + (y + h / 2) * PxScale, rotation / 60000,
                  (flip and PD_FLIP_H) <> 0, (flip and PD_FLIP_V) <> 0, False, Rect(0, 0, 0, 0), Poly);
            end
            else if (Pic <> nil) and (clip_w > 0) then   { cropped: only its frame }
              BlendPicture(Img, Pic, IX, IY, OX + Round(clip_x * PxScale), OY + Round(clip_y * PxScale),
                OX + Round((clip_x + clip_w) * PxScale), OY + Round((clip_y + clip_h) * PxScale))
            else if Pic <> nil then
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
        PD_DRAW_PATH:
          if (points <> nil) and (npoints >= 2) then
          begin
            { rings: split where the points say }
            SetLength(Rings, 1);
            Rings[0] := nil;
            for K := 0 to npoints - 1 do
              if points[2 * K] = PD_PATH_BREAK then
              begin
                SetLength(Rings, Length(Rings) + 1);
                Rings[High(Rings)] := nil;
              end
              else
              begin
                SetLength(Rings[High(Rings)], Length(Rings[High(Rings)]) + 1);
                Rings[High(Rings)][High(Rings[High(Rings)])].X := OX + points[2 * K] * PxScale;
                Rings[High(Rings)][High(Rings[High(Rings)])].Y := OY + points[2 * K + 1] * PxScale;
              end;
            if (fill <> 0) and (grad > 0) and (fill2 <> 0) then
              FillRingsImg(Img, Rings, fill, fill2, grad, grad_angle / 60000)
            else if fill <> 0 then
              FillRingsImg(Img, Rings, fill);
            if (line_width > 0) and (color <> 0) then
              for K := 0 to High(Rings) do
                StrokePolylineImg(Img, Rings[K], (path_flags and PD_PATH_CLOSED) <> 0, line_width * PxScale,
                  color);
          end;
        PD_DRAW_GLYPH:
          if (font <> nil) and (rotation <> 0) then     { turned with its text box's shape }
            DrawGlyphTurned(Img, font, glyph, OX + x * PxScale, OY + y * PxScale, size * PxScale, rotation / 60000,
              color)
          else if font <> nil then
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

  if FShowMarks and not FCanvasPage then     { (the canvas page's paragraph under the canvas: not shown) }
    PaintMarks(Img, Page, OX, OY, PxScale);

  { the others' carets, each with its name above it }
  for CI := 0 to High(FRemote) do
    with FRemote[CI] do
      if (pd_layout_caret(FLayout, Pos, CPage, CX, CBase, CAsc, CDesc) = PD_OK) and (CPage = Page) then
      begin
        IX := OX + Round(CX * PxScale);
        IY := OY + Round((CBase - CAsc) * PxScale);
        FillRectImg(Img, IX, IY, IX + 2, OY + Round((CBase + CDesc) * PxScale), Color, 255);
        if Name <> '' then
        begin
          K := BalloonHeight([Name, ''], Round(140 * FZoom), PxScale, nil, 0, 0, $00FFFFFF);
          K := K * 7 div 10;    { the label: smaller than a balloon's padding would make it }
          FillRectImg(Img, IX, IY - K, IX + Round((8 + 7 * Length(Name)) * FZoom), IY, Color, 255);
          BalloonHeight([Name, ''], Round(140 * FZoom), PxScale, Img, IX - Round(3 * FZoom), IY - K - Round(3 * FZoom),
            $00FFFFFF);
        end;
      end;

  { the content control the caret is in, framed as Word frames it: where a form's field is, and how far }
  if OnScreen and ControlAt(CaretPos, CKind, CSpec, CA, CB) and
    (pd_layout_caret(FLayout, PdPos(CA.block, CA.offset + 3), CPage, CX, CBase, CAsc, CDesc) = PD_OK) and
    (CPage = Page) and (pd_layout_caret(FLayout, CB, CPage2, CX2, CBase2, CAsc, CDesc) = PD_OK) and (CPage2 = Page) and
    (CBase2 = CBase) then
  begin
    IX := OX + Round(CX * PxScale) - 2;
    IX2 := OX + Round(CX2 * PxScale) + 2;
    IY := OY + Round((CBase - CAsc) * PxScale) - 1;
    IY2 := OY + Round((CBase + CDesc) * PxScale) + 1;
    FillRectImg(Img, IX, IY, IX2, IY + 1, $007DA7D9, 255);
    FillRectImg(Img, IX, IY2 - 1, IX2, IY2, $007DA7D9, 255);
    FillRectImg(Img, IX, IY, IX + 1, IY2, $007DA7D9, 255);
    FillRectImg(Img, IX2 - 1, IY, IX2, IY2, $007DA7D9, 255);
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
  Result := Format('%d %d %g %d %p %d %d %d', [ClientWidth, ClientHeight, FZoom, FLayoutEpoch, Pointer(FDoc),
    Int64(pd_doc_revision(FDoc)), FRemoteRev, Ord(FShowMarks)]);
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
  SetLength(FBalloons, 0);
  DrawView(Img, 0, H, True);

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

{ Scrolled by DY pixels and nothing else changed: the pixels already drawn
  move -- on the display's side as well as here, so none of them cross the
  connection again -- and only the strip that came into view is drawn and
  sent.  This is how a scroll stays cheap over a remote display (an RDP or
  X2Go session sends a moved area as a copy, a redrawn one as pixels) and
  on a large window, where drawing every page in view again was most of a
  step.  False when there is nothing to move: a jump of a window or more. }
{ The pages that reach into the band of the view from Top, Height pixels
  tall, drawn into Img (whose row 0 is the band's top), with their balloons
  when there is markup -- and the balloons of the page just above the band,
  whose column can run on past the page's foot into it.  PagesToo false: the
  balloons alone, for the list a click looks in. }
procedure TParadeEdit.DrawView(Img: TLazIntfImage; ATop, AHeight: Integer; PagesToo: Boolean);
var
  Page, PTop, Prev: Integer;
  Info: pd_page_info;
begin
  Prev := -1;
  for Page := 0 to PageCount - 1 do
  begin
    PTop := PageTop(Page);
    pd_layout_page_info(FLayout, Page, Info);
    if PTop + Round(Info.height * PxPerSp) + FPageGap < ATop then
    begin
      Prev := Page;
      Continue;
    end;
    if PTop > ATop + AHeight then
      Break;
    if FHasMarkup and (Prev >= 0) then
    begin
      PaintMarkup(Img, Prev, PageLeft(Prev), PageTop(Prev) - ATop, PxPerSp);
      Prev := -1;
    end;
    if PagesToo then
      PaintPage(Img, Page, PageLeft(Page), PTop - ATop, PxPerSp, False, True);
    if FHasMarkup then
      PaintMarkup(Img, Page, PageLeft(Page), PTop - ATop, PxPerSp);
  end;
end;

function TParadeEdit.ScrollBack(DY: Integer): Boolean;
var
  W, H, SY0, SH, Y: Integer;
  Strip, Dot: TLazIntfImage;
  Bmp: TBitmap;
begin
  Result := False;
  W := ClientWidth - FScrollBar.Width;
  H := ClientHeight;
  if (FBack = nil) or (FBackImg = nil) or (FBackImg.Width <> W) or (FBackImg.Height <> H) or (DY = 0) or
     (Abs(DY) >= H) then
    Exit;

  { what is still in view moves by DY: here, row by row in the right order, and there, as one copy }
  if DY > 0 then
  begin   { down the document: the rows move up }
    for Y := 0 to H - 1 - DY do
      Move(FBackImg.GetDataLineStart(Y + DY)^, FBackImg.GetDataLineStart(Y)^, W * 4);
    FBack.Canvas.CopyRect(Rect(0, 0, W, H - DY), FBack.Canvas, Rect(0, DY, W, H));
    SY0 := H - DY;
  end
  else
  begin
    for Y := H - 1 downto -DY do
      Move(FBackImg.GetDataLineStart(Y + DY)^, FBackImg.GetDataLineStart(Y)^, W * 4);
    FBack.Canvas.CopyRect(Rect(0, -DY, W, H), FBack.Canvas, Rect(0, 0, W, H + DY));
    SY0 := 0;
  end;
  SH := Abs(DY);

  { the strip that came into view, drawn alone }
  Strip := NewImage(W, SH, TColorToRGB(Color));
  try
    DrawView(Strip, SY0, SH, True);
    { the balloons to click, as a full drawing would list them: laid out again
      for the whole view onto nothing (a pixel), which measures their text
      but draws none of it }
    SetLength(FBalloons, 0);
    if FHasMarkup then
    begin
      Dot := NewImage(1, 1, 0);
      try
        DrawView(Dot, 0, H, False);
      finally
        Dot.Free;
      end;
    end;

    for Y := 0 to SH - 1 do
      Move(Strip.GetDataLineStart(Y)^, FBackImg.GetDataLineStart(SY0 + Y)^, W * 4);
    Bmp := TBitmap.Create;
    try
      Bmp.LoadFromIntfImage(Strip);
      FBack.Canvas.Draw(0, SY0, Bmp);
    finally
      Bmp.Free;
    end;
  finally
    Strip.Free;
  end;
  Result := True;
end;

function TParadeEdit.ViewMatchesRedraw: Boolean;
var
  W, H, Y: Integer;
  Img: TLazIntfImage;
  Kept: array of TBalloonHit;
  I: Integer;
begin
  Result := False;
  W := ClientWidth - FScrollBar.Width;
  H := ClientHeight;
  if (FBackImg = nil) or (FBackImg.Width <> W) or (FBackImg.Height <> H) then
    Exit;
  SetLength(Kept, Length(FBalloons));
  for I := 0 to High(FBalloons) do
    Kept[I] := FBalloons[I];
  SetLength(FBalloons, 0);
  Img := NewImage(W, H, TColorToRGB(Color));
  try
    DrawView(Img, 0, H, True);
    Result := Length(FBalloons) = Length(Kept);
    for I := 0 to High(Kept) do
      if Result and not (EqualRect(Kept[I].R, FBalloons[I].R) and (Kept[I].Id = FBalloons[I].Id)) then
        Result := False;
    for Y := 0 to H - 1 do
      if Result and not CompareMem(Img.GetDataLineStart(Y), FBackImg.GetDataLineStart(Y), W * 4) then
        Result := False;
  finally
    Img.Free;
    SetLength(FBalloons, Length(Kept));
    for I := 0 to High(Kept) do
      FBalloons[I] := Kept[I];
  end;
end;

procedure TParadeEdit.Paint;
var
  Sig: string;
  R: TRect;
begin
  CheckSelection;     { every move of the caret and every edit is followed by a paint }
  if not FCursorShown then
  begin   { the first paint: the window exists now, so the cursor takes }
    FCursorShown := True;
    SetTempCursor(crArrow);
    SetTempCursor(Cursor);
  end;
  if (ClientWidth - FScrollBar.Width <= 0) or (ClientHeight <= 0) then
    Exit;
  Sig := BackSignature;
  if (Sig = FBackSig) and (FBack <> nil) and (FScrollY <> FBackScroll) and ScrollBack(FScrollY - FBackScroll) then
    FBackScroll := FScrollY
  else if (Sig <> FBackSig) or (FBack = nil) or (FScrollY <> FBackScroll) then
  begin
    RebuildBack;
    FBackSig := Sig;
    FBackScroll := FScrollY;
  end;
  if FBack = nil then
    Exit;
  R := Canvas.ClipRect;
  if R.Right > FBack.Width then R.Right := FBack.Width;
  if R.Bottom > FBack.Height then R.Bottom := FBack.Height;
  if (R.Right > R.Left) and (R.Bottom > R.Top) then
    Canvas.CopyRect(R, FBack.Canvas, R);     { on the display's side: no pixels cross the connection }
  if Focused and FCaretOn and CaretRect(R) and not FShapeOn then
  begin
    Canvas.Brush.Style := bsSolid;
    Canvas.Brush.Color := clBlack;
    Canvas.FillRect(R);
  end;
  PaintTextBoxFrame;
  PaintShapeSelection;
  PaintPageCorner;
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


{ ---------------- collaboration hooks ---------------- }

procedure TParadeEdit.ExternalChange;
begin
  FHasDesiredX := False;
  Relayout;
  FModified := True;
  if Assigned(FOnChange) then
    FOnChange(Self);
end;

procedure TParadeEdit.SetRemoteCarets(const Carets: array of TParadeRemoteCaret);
var
  I: Integer;
begin
  SetLength(FRemote, Length(Carets));
  for I := 0 to High(Carets) do
    FRemote[I] := Carets[I];
  Inc(FRemoteRev);
  Invalidate;
end;

{ ---------------- review: tracked changes and comments ---------------- }

function TParadeEdit.RevisionAt(const P: pd_pos; out Rev: pd_revision): Boolean;
var
  F: pd_format_id;
  St: UInt32;
  Cp: pd_char_props;
begin
  Result := False;
  F := FormatAt(P);
  if (F = PD_FORMAT_INHERIT) or (pd_doc_format_info(FDoc, F, @St, @Cp) <> PD_OK) or
     ((Cp.mask and PD_CP_REVISION) = 0) then
    Exit;
  Result := pd_doc_revision_get(FDoc, Cp.revision, Rev) = PD_OK;
end;

{ the character at a position is tracked text the markup leaves out of the line }
function TParadeEdit.HiddenAt(const P: pd_pos): Boolean;
var
  Rv: pd_revision;
begin
  Result := False;
  if (pd_doc_revision_count(FDoc) = 0) or not RevisionAt(P, Rv) then
    Exit;
  case pd_doc_markup(FDoc) of
    PD_MARKUP_BALLOONS, PD_MARKUP_FINAL: Result := Rv.kind = PD_REV_DELETE;
    PD_MARKUP_ORIGINAL: Result := Rv.kind = PD_REV_INSERT;
  end;
end;

function TParadeEdit.StripRevision(Fmt: pd_format_id): pd_format_id;
var
  St: UInt32;
  Cp: pd_char_props;
begin
  Result := Fmt;
  if (Fmt = PD_FORMAT_INHERIT) or (pd_doc_format_info(FDoc, Fmt, @St, @Cp) <> PD_OK) or
     ((Cp.mask and PD_CP_REVISION) = 0) then
    Exit;
  Cp.mask := Cp.mask and not PD_CP_REVISION;
  Cp.revision := 0;
  Result := pd_doc_format(FDoc, St, @Cp);
end;

procedure TParadeEdit.SetTrackChanges(AValue: Boolean);
begin
  FTrack := AValue;
  if AValue then
    pd_doc_set_tracking(FDoc, PAnsiChar(FAuthor))
  else
    pd_doc_set_tracking(FDoc, nil);
end;

procedure TParadeEdit.SetAuthor(const AValue: string);
begin
  FAuthor := Copy(AValue, 1, 63);
  if FTrack then
    pd_doc_set_tracking(FDoc, PAnsiChar(FAuthor));
end;

function TParadeEdit.GetMarkupMode: Integer;
begin
  Result := pd_doc_markup(FDoc);
end;

procedure TParadeEdit.SetMarkupMode(AValue: Integer);
begin
  if AValue = pd_doc_markup(FDoc) then
    Exit;
  pd_doc_set_markup(FDoc, AValue);
  Relayout;
end;

function TParadeEdit.GetHybridBreaking: Boolean;
begin
  Result := pd_doc_stable_breaks(FDoc) <> 0;
end;

procedure TParadeEdit.SetHybridBreaking(AValue: Boolean);
begin
  pd_doc_set_stable_breaks(FDoc, Ord(AValue));    { paragraphs follow it as they are next edited }
end;

procedure TParadeEdit.SelectRange(const R: pd_range);
begin
  pd_doc_marker_set(FDoc, FAnchor, R.start);
  pd_doc_marker_set(FDoc, FCaret, R.finish);
  FHasDesiredX := False;
  EnsureCaretVisible;
  Invalidate;
end;

{ the change the position is in or at an end of }
function TParadeEdit.ChangeAt(const P: pd_pos; out R: pd_range): Boolean;
var
  From: pd_pos;
begin
  Result := False;
  From := PdPos(P.block, 0);
  while pd_doc_revision_find(FDoc, From, 1, R, nil) = PD_OK do
  begin
    if (R.start.block <> P.block) or (R.start.offset > P.offset) then
      Exit;
    if R.finish.offset >= P.offset then
      Exit(True);
    From := R.finish;
  end;
end;

procedure TParadeEdit.ResolveChange(Accept: Boolean);
var
  R: pd_range;
begin
  if FReadOnly then
    Exit;
  if HasSelection then
    R := PdRange(SelStart, SelEnd)
  else if not ChangeAt(CaretPos, R) then
  begin
    NextChange(1);      { nothing here: go to the next one first }
    Exit;
  end;
  if pd_doc_revision_resolve(FDoc, R, Ord(Accept)) <> PD_OK then
    Exit;
  pd_doc_marker_set(FDoc, FAnchor, CaretPos);
  Changed;
  NextChange(1);
end;

procedure TParadeEdit.AcceptChange;
begin
  ResolveChange(True);
end;

procedure TParadeEdit.RejectChange;
begin
  ResolveChange(False);
end;

procedure TParadeEdit.AcceptAllChanges;
begin
  if FReadOnly then
    Exit;
  if pd_doc_revision_resolve(FDoc, PdRange(PdPos(FirstPara, 0), LastPos), 1) = PD_OK then
    Changed;
end;

procedure TParadeEdit.RejectAllChanges;
begin
  if FReadOnly then
    Exit;
  if pd_doc_revision_resolve(FDoc, PdRange(PdPos(FirstPara, 0), LastPos), 0) = PD_OK then
    Changed;
end;

function TParadeEdit.NextChange(Dir: Integer): Boolean;
var
  From: pd_pos;
  R, Best: pd_range;
  C: pd_comment;
  I: Int32;
  Found: Boolean;
begin
  if Dir >= 0 then
    From := SelEnd
  else
    From := SelStart;
  Found := pd_doc_revision_find(FDoc, From, Dir, R, nil) = PD_OK;
  if Found then
    Best := R;
  { or a comment, when one comes first }
  for I := 1 to pd_doc_comment_count(FDoc) do
    if (pd_doc_comment_get(FDoc, I, C) = PD_OK) and (C.parent = 0) then
      if ((Dir >= 0) and (Compare(C.range.start, From) >= 0) and (not Found or (Compare(C.range.start, Best.start) < 0)) and
          not ((Compare(C.range.start, From) = 0) and HasSelection)) or
         ((Dir < 0) and (Compare(C.range.finish, From) <= 0) and (not Found or (Compare(C.range.start, Best.start) > 0)) and
          not ((Compare(C.range.finish, From) = 0) and HasSelection)) then
      begin
        Best := C.range;
        Found := True;
      end;
  Result := Found;
  if Found then
    SelectRange(Best);
end;

function TParadeEdit.AddComment(const AText: string): pd_comment_id;
var
  C: pd_comment;
  S: string;
  A, B: UInt32;
begin
  if FReadOnly then
    Exit(0);
  Result := 0;
  FillChar(C, SizeOf(C), 0);
  if HasSelection then
    C.range := PdRange(SelStart, SelEnd)
  else
  begin   { the word at the caret }
    S := ParaText(CaretPos.block);
    A := CaretPos.offset;
    B := A;
    while (A > 0) and ((S[A] in ['0'..'9', 'A'..'Z', 'a'..'z', '_']) or (Ord(S[A]) >= $80)) do
      Dec(A);
    while (B < UInt32(Length(S))) and ((S[B + 1] in ['0'..'9', 'A'..'Z', 'a'..'z', '_']) or (Ord(S[B + 1]) >= $80)) do
      Inc(B);
    C.range := PdRange(PdPos(CaretPos.block, A), PdPos(CaretPos.block, B));
  end;
  StrPLCopy(C.author, FAuthor, 63);
  StrPLCopy(C.date, FormatDateTime('yyyy-mm-dd"T"hh:nn:ss', Now), 31);
  C.text := PAnsiChar(AText);
  C.text_len := Length(AText);
  if pd_doc_comment_add(FDoc, C, Result) <> PD_OK then
    Result := 0
  else
    Changed;
end;

function TParadeEdit.ReplyToComment(Id: pd_comment_id; const AText: string): pd_comment_id;
var
  C: pd_comment;
begin
  if FReadOnly then
    Exit(0);
  Result := 0;
  FillChar(C, SizeOf(C), 0);
  StrPLCopy(C.author, FAuthor, 63);
  StrPLCopy(C.date, FormatDateTime('yyyy-mm-dd"T"hh:nn:ss', Now), 31);
  C.text := PAnsiChar(AText);
  C.text_len := Length(AText);
  C.parent := Id;
  if pd_doc_comment_add(FDoc, C, Result) <> PD_OK then
    Result := 0
  else
    Changed;
end;

function TParadeEdit.CommentAt(const P: pd_pos): pd_comment_id;
var
  I: Int32;
  C: pd_comment;
  Start: pd_pos;
begin
  Result := 0;
  Start := PdPos(0, 0);
  for I := 1 to pd_doc_comment_count(FDoc) do
    if (pd_doc_comment_get(FDoc, I, C) = PD_OK) and (C.parent = 0) and (Compare(C.range.start, P) <= 0) and
       (Compare(P, C.range.finish) <= 0) and ((Result = 0) or (Compare(C.range.start, Start) > 0)) then
    begin
      Result := I;
      Start := C.range.start;
    end;
end;

procedure TParadeEdit.DeleteComment(Id: pd_comment_id);
begin
  if FReadOnly then
    Exit;
  if pd_doc_comment_remove(FDoc, Id) = PD_OK then
    Changed;
end;

procedure TParadeEdit.ResolveComment(Id: pd_comment_id; Resolved: Boolean);
var
  C: pd_comment;
begin
  if FReadOnly then
    Exit;
  if pd_doc_comment_get(FDoc, Id, C) <> PD_OK then
    Exit;
  C.resolved := Ord(Resolved);
  if pd_doc_comment_set(FDoc, Id, C) = PD_OK then
    Changed;
end;

{ Lay out a balloon's text -- runs alternating: a heading in the author's
  colour, bold, then plain text -- in W pixels; draw it at X, Y when Draw is
  given. Returns its height in pixels, padding included. }
function TParadeEdit.BalloonHeight(const Segs: array of string; W: Integer; PxScale: Double; Draw: TLazIntfImage;
  X, Y: Integer; Col: UInt32): Integer;
var
  Para: Ppd_para;
  Prm: pd_params;
  SH, SB, St: pd_style;
  FH, FB: Ppd_font;
  NL, L, K, IX, IY, Sub, Pad: Integer;
  NG: Int32;
  Ln: pd_line;
  Gs: array of pd_glyph;
  G: PGlyphBmp;
  PX: Double;
begin
  Pad := Round(5 * FZoom);
  Result := 2 * Pad;
  FB := ResolveFont(Self, 'sans-serif', 400, 0);
  FH := ResolveFont(Self, 'sans-serif', 700, 0);
  if (FB = nil) or (W <= 2 * Pad + 8) or (pd_para_new(Para) <> PD_OK) then
    Exit;
  if FH = nil then
    FH := FB;
  try
    pd_style_init(SH, FH, PD_SP_PER_PT * 8);
    SH.color := Col;
    pd_style_init(SB, FB, PD_SP_PER_PT * 8);
    SB.color := $00303030;
    for K := 0 to High(Segs) do
      if Segs[K] <> '' then
        if K mod 2 = 0 then
          pd_para_add_text(Para, PAnsiChar(Segs[K]), Length(Segs[K]), SH)
        else
          pd_para_add_text(Para, PAnsiChar(Segs[K]), Length(Segs[K]), SB);
    pd_params_init(Prm);
    Prm.width := Round((W - 2 * Pad) / PxScale);
    Prm.align := PD_ALIGN_LEFT;
    Prm.mode := PD_BREAK_GREEDY;    { fill each line: an optimal ragged edge evens the lines out, short }
    if pd_para_break(Para, @Prm, nil) <> PD_OK then
      Exit;
    NL := pd_para_line_count(Para);
    if NL = 0 then
      Exit;
    pd_para_get_line(Para, NL - 1, Ln);
    Result := 2 * Pad + Round((Ln.baseline + Ln.descent) * PxScale);
    if Draw = nil then
      Exit;
    for L := 0 to NL - 1 do
    begin
      pd_para_get_glyphs(Para, L, nil, 0, NG);
      if NG <= 0 then
        Continue;
      SetLength(Gs, NG);
      pd_para_get_glyphs(Para, L, @Gs[0], NG, NG);
      for K := 0 to NG - 1 do
        if (Gs[K].kind = PD_KIND_GLYPH) and (pd_para_get_style(Para, Gs[K].style, St) = PD_OK) and (St.font <> nil) then
        begin
          PX := X + Pad + Gs[K].x * PxScale;
          IX := Floor0(PX);
          Sub := Round((PX - IX) * 4) * 64;
          if Sub >= 256 then
          begin
            Inc(IX);
            Sub := 0;
          end;
          IY := Y + Pad + Round(Gs[K].y * PxScale);
          G := GetGlyphBmp(St.font, Gs[K].glyph, Round(St.size * PxScale * PD_SP_PER_PT), Sub);
          if G^.W > 0 then
            BlendGlyph(Draw, G, IX, IY, St.color and $FFFFFF);
        end;
    end;
  finally
    pd_para_free(Para);
  end;
end;

{ A page's tracked changes and comments: a change bar in the left margin by
  each change, and in the column on the right a balloon for each deletion
  (when they are out of the text) and each comment with its replies, as near
  its line as the balloons above it allow, tied to its place by a line. }
procedure TParadeEdit.PaintMarkup(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double);
var
  Items: array of pd_markup_item;
  N, CI: Int32;
  I, J, PW, BX, BW, Y, NextY, AX, AY, H, BarX, Gap: Integer;
  Segs: array of string;
  S: string;
  C, R: pd_comment;
  Rv: pd_revision;
  Col: UInt32;
  Line: TPtDArray;
  Info0: pd_page_info;

  procedure Add(const T: string);
  begin
    SetLength(Segs, Length(Segs) + 1);
    Segs[High(Segs)] := T;
  end;

begin
  if (pd_layout_page_markup(FLayout, Page, nil, 0, N) <> PD_OK) or (N = 0) then
    Exit;
  SetLength(Items, N);
  if pd_layout_page_markup(FLayout, Page, @Items[0], N, N) <> PD_OK then
    Exit;
  pd_layout_page_info(FLayout, Page, Info0);
  PW := Round(Info0.width * PxScale);
  Gap := Round(12 * FZoom);
  BX := OX + PW + Gap;
  BW := MarkupWidth - 2 * Gap;
  BarX := OX + Round(PD_SP_PER_PT * 24 * PxScale);
  NextY := OY + Gap;
  for I := 0 to N - 1 do
    with Items[I] do
    begin
      Col := color and $FFFFFF;
      if kind <> PD_MARK_COMMENT then
        FillRectImg(Img, BarX, OY + Round(top * PxScale), BarX + 2, OY + Round(bottom * PxScale), Col, 255);
      if (kind = PD_MARK_INSERTION) or ((kind = PD_MARK_DELETION) and (pd_doc_markup(FDoc) <> PD_MARKUP_BALLOONS)) then
        Continue;
      SetLength(Segs, 0);
      if kind = PD_MARK_DELETION then
      begin
        S := Copy(ParaText(range.start.block), range.start.offset + 1, range.finish.offset - range.start.offset);
        S := StringReplace(S, #$EF#$BF#$BC, '', [rfReplaceAll]);
        if Length(S) > 400 then
          S := Copy(S, 1, 400) + '...';
        if pd_doc_revision_get(FDoc, id, Rv) = PD_OK then
          Add(PAnsiChar(@Rv.author[0]) + ' deleted: ')
        else
          Add('Deleted: ');
        Add(S);
      end
      else if pd_doc_comment_get(FDoc, id, C) = PD_OK then
      begin
        SetString(S, C.text, C.text_len);
        if C.resolved <> 0 then
        begin
          Add(PAnsiChar(@C.author[0]) + ' (resolved): ');
          Col := $00909090;
        end
        else
          Add(PAnsiChar(@C.author[0]) + ': ');
        Add(S);
        for CI := 1 to pd_doc_comment_count(FDoc) do
          if (pd_doc_comment_get(FDoc, CI, R) = PD_OK) and (R.parent = id) then
          begin
            SetString(S, R.text, R.text_len);
            Add(#10 + PAnsiChar(@R.author[0]) + ': ');
            Add(S);
          end;
      end
      else
        Continue;

      AX := OX + Round(x * PxScale);
      AY := OY + Round(y * PxScale);
      Y := AY - Round(12 * FZoom);
      if Y < NextY then
        Y := NextY;
      H := BalloonHeight(Segs, BW, PxScale, nil, 0, 0, Col);
      { the balloon: white, tinted, framed, with a bar of the colour down its left }
      FillRectImg(Img, BX, Y, BX + BW, Y + H, $00FFFFFF, 255);
      FillRectImg(Img, BX, Y, BX + BW, Y + H, Col, 28);
      FillRectImg(Img, BX, Y, BX + BW, Y + 1, Col, 255);
      FillRectImg(Img, BX, Y + H - 1, BX + BW, Y + H, Col, 255);
      FillRectImg(Img, BX + BW - 1, Y, BX + BW, Y + H, Col, 255);
      FillRectImg(Img, BX, Y, BX + 3, Y + H, Col, 255);
      BalloonHeight(Segs, BW, PxScale, Img, BX + 2, Y, Col);
      { its place: a dotted line under the line, on to the page's edge, then to the balloon }
      J := AX;
      while J < OX + PW do
      begin
        FillRectImg(Img, J, AY + 2, J + 2, AY + 3, Col, 130);
        Inc(J, 4);
      end;
      SetLength(Line, 2);
      Line[0].X := OX + PW;
      Line[0].Y := AY + 2.5;
      Line[1].X := BX;
      Line[1].Y := Y + Round(8 * FZoom);
      StrokePolylineImg(Img, Line, False, 1, Col);
      SetLength(FBalloons, Length(FBalloons) + 1);
      FBalloons[High(FBalloons)].R := Rect(BX, Y, BX + BW, Y + H);
      FBalloons[High(FBalloons)].Range := range;
      FBalloons[High(FBalloons)].Kind := kind;
      FBalloons[High(FBalloons)].Id := id;
      NextY := Y + H + Round(6 * FZoom);
    end;
end;

end.
