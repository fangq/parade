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
  paradefonts;

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
procedure FillRingsImg(Img: TLazIntfImage; const Rings: TRings; Col: UInt32);
const
  SUB = 4;
var
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
        Pix^.R := (Pix^.R * (255 - A) + R * A) div 255;
        Pix^.G := (Pix^.G * (255 - A) + G * A) div 255;
        Pix^.B := (Pix^.B * (255 - A) + B * A) div 255;
        Pix^.A := 255;
      end;
      Inc(Pix);
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
  if E = '.txt' then Exit(PD_CONV_TEXT);
  if (E = '.pdoc') or (E = '.jdoc') then Exit(PD_CONV_JDATA);
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
  inherited Create(AOwner);
  FHybridDefault := True;
  ControlStyle := ControlStyle + [csOpaque] - [csSetCaption];
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
  Relayout;
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
  if FReadOnly then
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
  Sig := SysUtils.Format('%d:%d %d:%d %d %d %d %d', [CaretPos.block, CaretPos.offset, AnchorPos.block,
    AnchorPos.offset, pd_doc_revision(FDoc), FPending.mask, Ord(PendingHere), Ord(FPainter)]);
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
begin
  if FReadOnly then
    Exit;
  if Assigned(FOnUndo) then
  begin
    if FOnUndo(Self, False) then
    begin
      pd_doc_marker_set(FDoc, FAnchor, CaretPos);
      Changed;
    end;
    Exit;
  end;
  if pd_doc_undo(FDoc) = PD_OK then
  begin
    pd_doc_marker_set(FDoc, FAnchor, CaretPos);
    Changed;
  end;
end;

procedure TParadeEdit.Redo;
begin
  if FReadOnly then
    Exit;
  if Assigned(FOnUndo) then
  begin
    if FOnUndo(Self, True) then
    begin
      pd_doc_marker_set(FDoc, FAnchor, CaretPos);
      Changed;
    end;
    Exit;
  end;
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
  if FReadOnly then
    Exit;
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
  if FReadOnly then
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
                pd_doc_delete(FDoc, PdRange(P, CaretPos), nil);
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
                pd_doc_delete(FDoc, PdRange(CaretPos, P), nil);
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
  I: Integer;
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
  if FPainter and (Button = mbLeft) and HasSelection then
    ApplyFormatPainter;     { the selection just made takes the copied look }
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

procedure TParadeEdit.PaintPage(Img: TLazIntfImage; Page, OX, OY: Integer; PxScale: Double; DrawCaret: Boolean;
  OnScreen: Boolean);
var
  Bands: array of TSelBand;
  NB, K: Integer;
  Rings: TRings;
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
            if fill <> 0 then
              FillRingsImg(Img, Rings, fill);
            if (line_width > 0) and (color <> 0) then
              for K := 0 to High(Rings) do
                StrokePolylineImg(Img, Rings[K], (path_flags and PD_PATH_CLOSED) <> 0, line_width * PxScale,
                  color);
          end;
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

  if FShowMarks then
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
