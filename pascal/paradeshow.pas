{ Parade - a slide show of a presentation's pages: its transitions and animations played.

  A presentation read from PowerPoint keeps each slide's transition and timing as PowerPoint wrote them
  (pd_pptx_slide_timing).  The timing is a tree of time containers; its main sequence is a list of groups, one a
  click, each a list of effects at offsets from the click, each effect a list of behaviours on a shape (or on some
  of its paragraphs): set it visible, reveal it through a filter, move it along keyframes written as formulas of the
  shape's own place and size, scale it, turn it, take it along a path.  Those behaviours are what is played here,
  so the presets need no table of their own: Fly In is its two keyframe animations, Spin its rotation.

  Every drawn item says which shape it is for (pd_draw.shape), and a text box's glyphs which paragraph, so a slide is
  cut into runs of its display list -- the shapes and paragraphs that are animated each a run of their own, what is
  between them a run -- drawn once, each over black and over white, which gives each its exact coverage.  A frame is
  the runs laid over one another, each as its shape's state at that moment has it.

  TParadeShow is the show without a window: what is shown, given the time (in milliseconds, from any clock), so a
  test steps it as it likes.  TParadeShowForm is the window: full screen, the keys and the mouse of a slide show.

  Played: transitions (fade, push, wipe, cover, pull, split, the shapes, bars, blinds, checkers, dissolve, zoom, and
  PowerPoint 2010's as the nearest of those); effects' visibility, opacity, filters, position and size keyframes,
  scale, rotation and motion paths, with their delays, durations, acceleration, auto-reverse and repeats; automatic
  advance.  Not played: colour and font changes (the shape stays as it is), sounds, triggers, media. }

unit ParadeShow;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Math, Graphics, Controls, Forms, ExtCtrls, LCLType, LCLIntf, IntfGraphics, GraphType, FPImage,
  Types, DOM, XMLRead, parade, paradeedit;

type
  TShowBehKind = (sbSet, sbEffect, sbAnim, sbScale, sbRot, sbMotion, sbOther);

  TShowKey = record
    Tm: Double;                 { 0..1 }
    Val, Fmla: string;
  end;

  { one behaviour of an effect: what it changes of its target, when (ms from the effect's start) and how }
  TShowBeh = record
    Kind: TShowBehKind;
    Target: Integer;            { the slide's target (TShowTiming.Targets) }
    Start, Dur: Double;
    Accel, Decel: Double;       { 0..1 of the duration }
    AutoRev: Boolean;
    Repeats: Double;
    Attr: string;               { ppt_x, ppt_y, ppt_w, ppt_h, r, style.rotation, style.opacity, style.visibility }
    ToVal: string;              { sbSet: the value set }
    Keys: array of TShowKey;    { sbAnim }
    Discrete: Boolean;
    Filter: string;             { sbEffect: wipe(up), fade, circle(in) ... }
    TransIn: Boolean;
    SFromX, SFromY, SToX, SToY, SByX, SByY: Double;   { sbScale, as factors }
    HasSFrom, HasSTo, HasSBy: Boolean;
    RotBy, RotFrom, RotTo: Double;                    { sbRot, degrees }
    HasRotFrom, HasRotTo: Boolean;
    Path: array of Double;      { sbMotion: x, y pairs, the slide's width and height 1, a polyline }
  end;

  TShowEffect = record
    Cls: string;                { entr, exit, emph, path }
    Start, Len: Double;         { ms from the step's start; how long }
    Behs: array of TShowBeh;
  end;

  { a click's effects (Steps[0]: those that play by themselves as the slide begins) }
  TShowStep = record
    Effects: array of TShowEffect;
    Len: Double;
  end;

  { what an effect works on: a shape (its DrawingML id), or paragraphs of its text (ParaA..ParaB, -1 the whole) }
  TShowTarget = record
    Shape: Int32;
    ParaA, ParaB: Integer;
  end;

  TShowTrans = record
    Present: Boolean;
    Kind, Dir: string;          { fade push wipe cover pull split ... ; l r u d, in out, horz vert, lu ... }
    Orient: string;
    ThruBlack: Boolean;
    Dur: Double;                { ms }
    AdvClick: Boolean;          { a click goes on (else only the time) }
    AdvTime: Integer;           { ms after its animations to go on by itself, -1 none }
  end;

  { a slide's transition and timing, read from what PowerPoint wrote }
  TShowTiming = class
  public
    Targets: array of TShowTarget;
    Steps: array of TShowStep;
    Trans: TShowTrans;
    constructor Create;
    function TargetOf(Shape: Int32; A, B: Integer): Integer;
    { from pd_pptx_slide_timing's XML; nothing in it (or '') gives no transition and no steps but the empty first }
    procedure Parse(const Xml: string);
    function StepCount: Integer;
  end;

  { a target's state at a moment: whether and how it shows }
  TShowState = record
    Visible: Boolean;
    Opacity: Double;
    CX, CY, W, H: Double;       { its middle and size, the slide's width and height 1 }
    Rot: Double;                { degrees, clockwise }
    SX, SY: Double;
    MX, MY: Double;             { along a motion path }
    Filter: string;
    FilterP: Double;            { how far it has come in (0..1) }
  end;

  { a stretch of the slide's display list drawn as one picture, premultiplied, where it is on the slide }
  TShowRun = record
    RTarget: Integer;           { -1: what is not animated }
    RLo, RHi: Integer;          { its items }
    RX, RY, RW, RH: Integer;
    RPx: array of UInt32;       { B G R A, premultiplied }
  end;

  TShowPhase = (spTrans, spPlay, spIdle, spEnd);

  TParadeShow = class
  private
    FEdit: TParadeEdit;
    FW, FH: Integer;                    { the frame }
    FSW, FSH, FOX, FOY: Integer;        { the slide in it }
    FSlides: array of Integer;          { the slides shown (hidden ones left out) }
    FTimings: array of TShowTiming;
    FPos: Integer;                      { the slide shown: an index into FSlides; Length: the end }
    FDone: Integer;                     { its steps played }
    FPhase: TShowPhase;
    FStart: Int64;                      { when the phase began }
    FIdleSince: Int64;
    { the slide prepared: its runs, its targets' places }
    FPrepared: Integer;
    FRuns: array of TShowRun;
    FBase: array of UInt32;             { the paper and the first run, when it is not animated }
    FBoxes: array of TRect;             { each target's box on the slide, px }
    FFirstEntr: array of Boolean;       { its first effect brings it in: hidden until then }
    FLo, FHi: Integer;                  { RenderSlide's filter: the items it draws }
    FItems: TParadeDrawItems;
    FParaOf: array of Integer;          { each item's paragraph in its text box, -1 none }
    FFrom, FTo, FCur: array of UInt32;  { a transition's slides, and the slide as it is now }
    FFrame: TLazIntfImage;
    FChanged: Boolean;
    FFinished: Boolean;
    function KeepItem(const Item: pd_draw; Index: Integer): Boolean;
    function TargetFor(T: TShowTiming; Index: Integer): Integer;
    procedure Prepare(APos: Integer);
    procedure RenderPlain(APos: Integer; var Px: array of UInt32);
    procedure Compose(APos, Done: Integer; PlayT: Double; var Px: array of UInt32);
    procedure StateAt(APos, Done: Integer; PlayT: Double; out S: array of TShowState);
    procedure Transition(const T: TShowTrans; P: Double; var Px: array of UInt32);
    procedure ToFrame(const Px: array of UInt32);
    procedure Enter(APos: Integer; Now: Int64; WithTrans: Boolean);
    procedure BeginSteps(Now: Int64);
    function Timing(APos: Integer): TShowTiming;
    function GetSlideRect: TRect;
  public
    { the show of AEdit's slides from slide FromSlide (its index among all, hidden or not), in a W by H frame }
    constructor Create(AEdit: TParadeEdit; W, H: Integer; FromSlide: Integer = 0);
    destructor Destroy; override;
    procedure Resize(W, H: Integer);
    { begins it: the first slide, through its transition }
    procedure Start(Now: Int64);
    { what the time has made of it; True when the frame changed }
    function Tick(Now: Int64): Boolean;
    { a click (or the key for it): the step playing ends, else the next plays, else the next slide }
    procedure Next(Now: Int64);
    { back a step, or to the slide before as it ends }
    procedure Prev(Now: Int64);
    { a slide by its index among all, as it begins }
    procedure GoToSlide(SlideIndex: Integer; Now: Int64);
    function SlideCount: Integer;       { the slides shown }
    function CurrentSlide: Integer;     { its index among all, -1 at the end }
    function StepsDone: Integer;
    function StepCount: Integer;        { the current slide's, the automatic one too }
    property Phase: TShowPhase read FPhase;
    property Finished: Boolean read FFinished;   { Next past the end }
    property Frame: TLazIntfImage read FFrame;
    property SlideRect: TRect read GetSlideRect;
    { for a test: the slide's timing, and a target's box on the slide as it is drawn (px, in the slide) }
    function TimingOf(ShowIndex: Integer): TShowTiming;
    function TargetBox(Shape: Int32; Para: Integer = -1): TRect;
  end;

  TParadeShowForm = class(TForm)
  private
    FShow: TParadeShow;
    FEdit: TParadeEdit;
    FFromSlide: Integer;
    FTimer: TTimer;
    FBmp: TBitmap;
    FBlank: TColor;
    FBlanked: Boolean;
    FTyped: string;
    FLastMove: Int64;
    FOnEnd: TNotifyEvent;
    procedure TimerTick(Sender: TObject);
    procedure Refresh;
  protected
    procedure KeyDown(var Key: Word; Shift: TShiftState); override;
    procedure KeyPress(var Key: Char); override;
    procedure MouseDown(Button: TMouseButton; Shift: TShiftState; X, Y: Integer); override;
    procedure MouseMove(Shift: TShiftState; X, Y: Integer); override;
    function DoMouseWheel(Shift: TShiftState; WheelDelta: Integer; MousePos: TPoint): Boolean; override;
    procedure Paint; override;
    procedure Resize; override;
    procedure DoClose(var CloseAction: TCloseAction); override;
  public
    constructor CreateShow(AOwner: TComponent; AEdit: TParadeEdit; FromSlide: Integer);
    destructor Destroy; override;
    property Engine: TParadeShow read FShow;
    { when it ends: the slide it was on is Show.CurrentSlide }
    property OnEnd: TNotifyEvent read FOnEnd write FOnEnd;
  end;

{ a slide show of AEdit's slides from FromSlide, full screen; it frees itself when it ends }
function ParadeSlideShow(AEdit: TParadeEdit; FromSlide: Integer; AOnEnd: TNotifyEvent = nil): TParadeShowForm;

{ the formulas of PowerPoint's keyframes: #ppt_x, #ppt_y, #ppt_w, #ppt_h, $ and pi, + - * / ^, sin, cos, ... }
function ShowEval(const Expr: string; X, Y, W, H, Dollar: Double; out V: Double): Boolean;

implementation

const
  BLACK_PX = $FF000000;

var
  Dot: TFormatSettings;

{ ---- the formulas ---- }

type
  TEvalCtx = record
    S: string;
    P: Integer;
    X, Y, W, H, D: Double;
    Ok: Boolean;
  end;

function EvExpr(var C: TEvalCtx): Double; forward;

procedure EvSpace(var C: TEvalCtx);
begin
  while (C.P <= Length(C.S)) and (C.S[C.P] in [' ', #9]) do
    Inc(C.P);
end;

function EvPrimary(var C: TEvalCtx): Double;
var
  A, I: Integer;
  Name: string;
  Args: array of Double;
begin
  Result := 0;
  EvSpace(C);
  if C.P > Length(C.S) then
  begin
    C.Ok := False;
    Exit;
  end;
  case C.S[C.P] of
    '0'..'9', '.':
      begin
        A := C.P;
        while (C.P <= Length(C.S)) and (C.S[C.P] in ['0'..'9', '.']) do
          Inc(C.P);
        if (C.P <= Length(C.S)) and (C.S[C.P] in ['e', 'E']) and (C.P < Length(C.S)) and
           (C.S[C.P + 1] in ['0'..'9', '-', '+']) then
        begin
          Inc(C.P, 2);
          while (C.P <= Length(C.S)) and (C.S[C.P] in ['0'..'9']) do
            Inc(C.P);
        end;
        if not TryStrToFloat(Copy(C.S, A, C.P - A), Result, Dot) then
          C.Ok := False;
      end;
    '(':
      begin
        Inc(C.P);
        Result := EvExpr(C);
        EvSpace(C);
        if (C.P <= Length(C.S)) and (C.S[C.P] = ')') then
          Inc(C.P)
        else
          C.Ok := False;
      end;
    '$':
      begin
        Inc(C.P);
        Result := C.D;
      end;
    '#', 'a'..'z', 'A'..'Z', '_':
      begin
        A := C.P;
        Inc(C.P);
        while (C.P <= Length(C.S)) and (C.S[C.P] in ['a'..'z', 'A'..'Z', '0'..'9', '_']) do
          Inc(C.P);
        Name := LowerCase(Copy(C.S, A, C.P - A));
        if Name = '#ppt_x' then Result := C.X
        else if Name = '#ppt_y' then Result := C.Y
        else if Name = '#ppt_w' then Result := C.W
        else if Name = '#ppt_h' then Result := C.H
        else if Name = 'pi' then Result := Pi
        else if Name = 'e' then Result := Exp(1)
        else
        begin   { a function of its arguments }
          EvSpace(C);
          Args := nil;
          if (C.P <= Length(C.S)) and (C.S[C.P] = '(') then
          begin
            Inc(C.P);
            repeat
              SetLength(Args, Length(Args) + 1);
              Args[High(Args)] := EvExpr(C);
              EvSpace(C);
              I := Ord((C.P <= Length(C.S)) and (C.S[C.P] = ','));
              Inc(C.P, I);
            until I = 0;
            if (C.P <= Length(C.S)) and (C.S[C.P] = ')') then
              Inc(C.P)
            else
              C.Ok := False;
          end;
          if Length(Args) = 0 then
          begin
            C.Ok := False;
            Exit;
          end;
          if Name = 'abs' then Result := Abs(Args[0])
          else if Name = 'sin' then Result := Sin(Args[0])
          else if Name = 'cos' then Result := Cos(Args[0])
          else if Name = 'tan' then Result := Tan(Args[0])
          else if Name = 'asin' then Result := ArcSin(EnsureRange(Args[0], -1, 1))
          else if Name = 'acos' then Result := ArcCos(EnsureRange(Args[0], -1, 1))
          else if Name = 'atan' then Result := ArcTan(Args[0])
          else if Name = 'sqrt' then Result := Sqrt(Max(0, Args[0]))
          else if Name = 'ln' then begin if Args[0] > 0 then Result := Ln(Args[0]) else C.Ok := False; end
          else if Name = 'exp' then Result := Exp(Min(Args[0], 700))
          else if Name = 'floor' then Result := Floor(Args[0])
          else if Name = 'ceil' then Result := Ceil(Args[0])
          else if Name = 'deg' then Result := RadToDeg(Args[0])
          else if Name = 'rad' then Result := DegToRad(Args[0])
          else if Name = 'rand' then Result := Random * Args[0]
          else if Name = 'max' then
          begin
            Result := Args[0];
            for I := 1 to High(Args) do
              Result := Max(Result, Args[I]);
          end
          else if Name = 'min' then
          begin
            Result := Args[0];
            for I := 1 to High(Args) do
              Result := Min(Result, Args[I]);
          end
          else
            C.Ok := False;
        end;
      end;
  else
    C.Ok := False;
  end;
end;

function EvUnary(var C: TEvalCtx): Double;
begin
  EvSpace(C);
  if (C.P <= Length(C.S)) and (C.S[C.P] = '-') then
  begin
    Inc(C.P);
    Result := -EvUnary(C);
  end
  else if (C.P <= Length(C.S)) and (C.S[C.P] = '+') then
  begin
    Inc(C.P);
    Result := EvUnary(C);
  end
  else
    Result := EvPrimary(C);
end;

function EvPower(var C: TEvalCtx): Double;
var
  B: Double;
begin
  Result := EvUnary(C);
  EvSpace(C);
  if (C.P <= Length(C.S)) and (C.S[C.P] = '^') then
  begin
    Inc(C.P);
    B := EvPower(C);
    if (Result < 0) and (Frac(B) <> 0) then
      C.Ok := False
    else
      Result := Power(Result, B);
  end;
end;

function EvTerm(var C: TEvalCtx): Double;
var
  B: Double;
  Op: Char;
begin
  Result := EvPower(C);
  EvSpace(C);
  while C.Ok and (C.P <= Length(C.S)) and (C.S[C.P] in ['*', '/']) do
  begin
    Op := C.S[C.P];
    Inc(C.P);
    B := EvPower(C);
    if Op = '*' then
      Result := Result * B
    else if B <> 0 then
      Result := Result / B
    else
      C.Ok := False;
    EvSpace(C);
  end;
end;

function EvExpr(var C: TEvalCtx): Double;
var
  Op: Char;
begin
  Result := EvTerm(C);
  EvSpace(C);
  while C.Ok and (C.P <= Length(C.S)) and (C.S[C.P] in ['+', '-']) do
  begin
    Op := C.S[C.P];
    Inc(C.P);
    if Op = '+' then
      Result := Result + EvTerm(C)
    else
      Result := Result - EvTerm(C);
    EvSpace(C);
  end;
end;

function ShowEval(const Expr: string; X, Y, W, H, Dollar: Double; out V: Double): Boolean;
var
  C: TEvalCtx;
begin
  C.S := Expr;
  C.P := 1;
  C.X := X;
  C.Y := Y;
  C.W := W;
  C.H := H;
  C.D := Dollar;
  C.Ok := Trim(Expr) <> '';
  V := 0;
  if not C.Ok then
    Exit(False);
  V := EvExpr(C);
  EvSpace(C);
  Result := C.Ok and (C.P > Length(C.S)) and not IsNan(V) and not IsInfinite(V);
end;

{ ---- the timing ---- }

function Kid(N: TDOMNode; const AName: string): TDOMElement;
var
  C: TDOMNode;
begin
  Result := nil;
  if N = nil then
    Exit;
  C := N.FirstChild;
  while C <> nil do
  begin
    if (C.NodeType = ELEMENT_NODE) and (string(C.NodeName) = AName) then
      Exit(TDOMElement(C));
    C := C.NextSibling;
  end;
end;

function Att(N: TDOMNode; const AName: string): string;
begin
  if (N <> nil) and (N.NodeType = ELEMENT_NODE) then
    Result := string(TDOMElement(N).GetAttribute(DOMString(AName)))
  else
    Result := '';
end;

function NumAtt(N: TDOMNode; const AName: string; ADefault: Double): Double;
begin
  if not TryStrToFloat(Att(N, AName), Result, Dot) then
    Result := ADefault;
end;

function LocalName(const S: string): string;
var
  I: Integer;
begin
  I := Pos(':', S);
  if I > 0 then
    Result := Copy(S, I + 1, MaxInt)
  else
    Result := S;
end;

{ a time node's start: its condition's delay (ms); "indefinite" (a click) and events as 0 }
function CondDelay(CTn: TDOMNode): Double;
var
  L, C: TDOMNode;
  V: Double;
begin
  Result := 0;
  L := Kid(CTn, 'p:stCondLst');
  if L = nil then
    Exit;
  C := L.FirstChild;
  while C <> nil do
  begin
    if (C.NodeType = ELEMENT_NODE) and (C.NodeName = 'p:cond') and TryStrToFloat(Att(C, 'delay'), V, Dot) then
      Exit(V);
    C := C.NextSibling;
  end;
end;

{ a value element's value: p:strVal, p:fltVal, p:intVal, p:boolVal }
function ValOf(N: TDOMNode): string;
var
  C: TDOMNode;
begin
  Result := '';
  if N = nil then
    Exit;
  C := N.FirstChild;
  while C <> nil do
  begin
    if C.NodeType = ELEMENT_NODE then
      Exit(Att(C, 'val'));
    C := C.NextSibling;
  end;
end;

constructor TShowTiming.Create;
begin
  inherited Create;
  SetLength(Steps, 1);
  Trans.AdvClick := True;
  Trans.AdvTime := -1;
end;

function TShowTiming.StepCount: Integer;
begin
  Result := Length(Steps);
end;

function TShowTiming.TargetOf(Shape: Int32; A, B: Integer): Integer;
var
  I: Integer;
begin
  for I := 0 to High(Targets) do
    if (Targets[I].Shape = Shape) and (Targets[I].ParaA = A) and (Targets[I].ParaB = B) then
      Exit(I);
  SetLength(Targets, Length(Targets) + 1);
  Result := High(Targets);
  Targets[Result].Shape := Shape;
  Targets[Result].ParaA := A;
  Targets[Result].ParaB := B;
end;

{ a motion path ("M 0 0 L 0.25 0.1 C ... E"): a polyline, its curves in 16 chords }
function PathPoints(const S: string): TDoubleDynArray;
var
  Tok: TStringList;
  I, K: Integer;
  Cmd: Char;
  X, Y, X1, Y1, X2, Y2, X3, Y3, CX0, CY0, T: Double;

  procedure Add(AX, AY: Double);
  begin
    SetLength(Result, Length(Result) + 2);
    Result[High(Result) - 1] := AX;
    Result[High(Result)] := AY;
  end;

  function Num(out V: Double): Boolean;
  begin
    Result := (I < Tok.Count) and TryStrToFloat(Tok[I], V, Dot);
    if Result then
      Inc(I);
  end;

begin
  Result := nil;
  Tok := TStringList.Create;
  try
    I := 1;
    while I <= Length(S) do
    begin   { letters and numbers, each a token }
      if S[I] in ['A'..'Z', 'a'..'z'] then
      begin
        Tok.Add(S[I]);
        Inc(I);
      end
      else if S[I] in ['0'..'9', '.', '-', '+'] then
      begin
        K := I;
        Inc(I);
        while (I <= Length(S)) and (S[I] in ['0'..'9', '.', 'e']) do
          Inc(I);
        Tok.Add(Copy(S, K, I - K));
      end
      else
        Inc(I);
    end;
    I := 0;
    X := 0;
    Y := 0;
    Cmd := 'M';
    while I < Tok.Count do
    begin
      if (Length(Tok[I]) = 1) and (Tok[I][1] in ['A'..'Z', 'a'..'z']) then
      begin
        Cmd := UpCase(Tok[I][1]);
        Inc(I);
        if Cmd in ['Z', 'E'] then
        begin
          if (Cmd = 'Z') and (Length(Result) >= 2) then
            Add(Result[0], Result[1]);
          Continue;
        end;
      end;
      case Cmd of
        'M', 'L':
          if Num(X1) and Num(Y1) then
          begin
            X := X1;
            Y := Y1;
            Add(X, Y);
          end
          else
            Inc(I);
        'C':
          if Num(X1) and Num(Y1) and Num(X2) and Num(Y2) and Num(X3) and Num(Y3) then
          begin
            CX0 := X;
            CY0 := Y;
            if Length(Result) = 0 then
              Add(X, Y);
            for K := 1 to 16 do
            begin
              T := K / 16;
              Add(Power(1 - T, 3) * CX0 + 3 * Sqr(1 - T) * T * X1 + 3 * (1 - T) * Sqr(T) * X2 + Power(T, 3) * X3,
                Power(1 - T, 3) * CY0 + 3 * Sqr(1 - T) * T * Y1 + 3 * (1 - T) * Sqr(T) * Y2 + Power(T, 3) * Y3);
            end;
            X := X3;
            Y := Y3;
          end
          else
            Inc(I);
      else
        Inc(I);
      end;
    end;
  finally
    Tok.Free;
  end;
end;

procedure TShowTiming.Parse(const Xml: string);
var
  Doc: TXMLDocument;
  St: TStringStream;
  Root, Main, C, Kids: TDOMNode;
  L: TDOMNodeList;
  I, Best: Integer;
  K, Spd: string;
  V: Double;
  Known: Boolean;

  function KnownKind(const S: string): Boolean;
  begin
    Result := (S = 'fade') or (S = 'push') or (S = 'wipe') or (S = 'cover') or (S = 'pull') or (S = 'split') or
      (S = 'cut') or (S = 'circle') or (S = 'diamond') or (S = 'plus') or (S = 'wedge') or (S = 'wheel') or
      (S = 'zoom') or (S = 'dissolve') or (S = 'randomBar') or (S = 'blinds') or (S = 'checker') or (S = 'comb') or
      (S = 'strips') or (S = 'newsflash') or (S = 'random');
  end;

  { an effect's behaviour, from its element (p:set, p:anim, ...) }
  procedure AddBeh(var E: TShowEffect; B: TDOMNode);
  var
    Bh: TShowBeh;
    CB, BT, Tg, Sp, Tx, Rg, Tl, Tv, X: TDOMNode;
    N: string;
    J: Integer;
  begin
    FillChar(Bh, SizeOf(Bh), 0);
    Bh.Keys := nil;
    Bh.Path := nil;
    N := string(B.NodeName);
    CB := Kid(B, 'p:cBhvr');
    if CB = nil then
      Exit;
    BT := Kid(CB, 'p:cTn');
    Bh.Start := CondDelay(BT);
    Bh.Dur := NumAtt(BT, 'dur', 0);
    Bh.Accel := NumAtt(BT, 'accel', 0) / 100000;
    Bh.Decel := NumAtt(BT, 'decel', 0) / 100000;
    Bh.AutoRev := Att(BT, 'autoRev') = '1';
    Bh.Repeats := NumAtt(BT, 'repeatCount', 1000) / 1000;
    if Bh.Repeats <= 0 then
      Bh.Repeats := 1;
    Tg := Kid(CB, 'p:tgtEl');
    Sp := Kid(Tg, 'p:spTgt');
    if Sp = nil then
      Bh.Target := -1
    else
    begin
      Tx := Kid(Sp, 'p:txEl');
      Rg := Kid(Tx, 'p:pRg');
      if Rg <> nil then
        Bh.Target := TargetOf(StrToIntDef(Att(Sp, 'spid'), 0), StrToIntDef(Att(Rg, 'st'), 0),
          StrToIntDef(Att(Rg, 'end'), StrToIntDef(Att(Rg, 'st'), 0)))
      else
        Bh.Target := TargetOf(StrToIntDef(Att(Sp, 'spid'), 0), -1, -1);
    end;
    X := Kid(Kid(CB, 'p:attrNameLst'), 'p:attrName');
    if X <> nil then
      Bh.Attr := string(X.TextContent);
    if N = 'p:set' then
    begin
      Bh.Kind := sbSet;
      Bh.ToVal := ValOf(Kid(B, 'p:to'));
    end
    else if N = 'p:animEffect' then
    begin
      Bh.Kind := sbEffect;
      Bh.Filter := Att(B, 'filter');
      Bh.TransIn := Att(B, 'transition') <> 'out';
    end
    else if N = 'p:anim' then
    begin
      Bh.Kind := sbAnim;
      Bh.Discrete := Att(B, 'calcmode') = 'discrete';
      Tl := Kid(B, 'p:tavLst');
      if Tl <> nil then
      begin
        Tv := Tl.FirstChild;
        while Tv <> nil do
        begin
          if (Tv.NodeType = ELEMENT_NODE) and (Tv.NodeName = 'p:tav') then
          begin
            J := Length(Bh.Keys);
            SetLength(Bh.Keys, J + 1);
            Bh.Keys[J].Tm := NumAtt(Tv, 'tm', 0) / 100000;
            Bh.Keys[J].Val := ValOf(Kid(Tv, 'p:val'));
            Bh.Keys[J].Fmla := Att(Tv, 'fmla');
          end;
          Tv := Tv.NextSibling;
        end;
      end
      else if (Att(B, 'from') <> '') or (Att(B, 'to') <> '') then
      begin
        SetLength(Bh.Keys, 2);
        Bh.Keys[0].Tm := 0;
        Bh.Keys[0].Val := Att(B, 'from');
        Bh.Keys[1].Tm := 1;
        Bh.Keys[1].Val := Att(B, 'to');
      end;
    end
    else if N = 'p:animScale' then
    begin
      Bh.Kind := sbScale;
      X := Kid(B, 'p:from');
      Bh.HasSFrom := X <> nil;
      Bh.SFromX := NumAtt(X, 'x', 100000) / 100000;
      Bh.SFromY := NumAtt(X, 'y', 100000) / 100000;
      X := Kid(B, 'p:to');
      Bh.HasSTo := X <> nil;
      Bh.SToX := NumAtt(X, 'x', 100000) / 100000;
      Bh.SToY := NumAtt(X, 'y', 100000) / 100000;
      X := Kid(B, 'p:by');
      Bh.HasSBy := X <> nil;
      Bh.SByX := NumAtt(X, 'x', 100000) / 100000;
      Bh.SByY := NumAtt(X, 'y', 100000) / 100000;
    end
    else if N = 'p:animRot' then
    begin
      Bh.Kind := sbRot;
      Bh.RotBy := NumAtt(B, 'by', 0) / 60000;
      Bh.HasRotFrom := Att(B, 'from') <> '';
      Bh.RotFrom := NumAtt(B, 'from', 0) / 60000;
      Bh.HasRotTo := Att(B, 'to') <> '';
      Bh.RotTo := NumAtt(B, 'to', 0) / 60000;
    end
    else if N = 'p:animMotion' then
    begin
      Bh.Kind := sbMotion;
      Bh.Path := PathPoints(Att(B, 'path'));
    end
    else
      Bh.Kind := sbOther;
    J := Length(E.Behs);
    SetLength(E.Behs, J + 1);
    E.Behs[J] := Bh;
  end;

  { an effect (a time node with a preset class) at T ms from its step's start }
  procedure AddEffect(var S: TShowStep; CTn: TDOMNode; T: Double);
  var
    E: TShowEffect;
    B: TDOMNode;
    J: Integer;
    End_: Double;
  begin
    E.Cls := Att(CTn, 'presetClass');
    E.Start := T;
    E.Len := 0;
    E.Behs := nil;
    B := Kid(CTn, 'p:childTnLst');
    if B <> nil then
      B := B.FirstChild;
    while B <> nil do
    begin
      if B.NodeType = ELEMENT_NODE then
        AddBeh(E, B);
      B := B.NextSibling;
    end;
    for J := 0 to High(E.Behs) do
      with E.Behs[J] do
      begin
        End_ := Start + Dur * Repeats * (1 + Ord(AutoRev));
        if End_ > E.Len then
          E.Len := End_;
      end;
    J := Length(S.Effects);
    SetLength(S.Effects, J + 1);
    S.Effects[J] := E;
    if T + E.Len > S.Len then
      S.Len := T + E.Len;
  end;

  { the time containers under List, each at its delay from Base: effects, or containers of them }
  procedure Walk(var S: TShowStep; List: TDOMNode; Base: Double);
  var
    E, CT: TDOMNode;
  begin
    if List = nil then
      Exit;
    E := List.FirstChild;
    while E <> nil do
    begin
      if (E.NodeType = ELEMENT_NODE) and ((E.NodeName = 'p:par') or (E.NodeName = 'p:seq')) then
      begin
        CT := Kid(E, 'p:cTn');
        if CT <> nil then
          if Att(CT, 'presetClass') <> '' then
            AddEffect(S, CT, Base + CondDelay(CT))
          else
            Walk(S, Kid(CT, 'p:childTnLst'), Base + CondDelay(CT));
      end;
      E := E.NextSibling;
    end;
  end;

  { a group of the main sequence: a click's, or one that goes on by itself (onBegin, or a delay of its own) }
  procedure AddGroup(G: TDOMNode);
  var
    CT, L, Cn: TDOMNode;
    Auto: Boolean;
    D, V: Double;
    Last: Integer;
  begin
    CT := Kid(G, 'p:cTn');
    if CT = nil then
      Exit;
    Auto := False;
    D := 0;
    L := Kid(CT, 'p:stCondLst');
    if L <> nil then
    begin
      Auto := True;
      Cn := L.FirstChild;
      while Cn <> nil do
      begin
        if (Cn.NodeType = ELEMENT_NODE) and (Cn.NodeName = 'p:cond') then
          if Att(Cn, 'evt') = 'onBegin' then
          begin
            Auto := True;
            D := NumAtt(Cn, 'delay', 0);
            Break;
          end
          else if Att(Cn, 'delay') = 'indefinite' then
            Auto := False
          else if TryStrToFloat(Att(Cn, 'delay'), V, Dot) then
            D := V;
        Cn := Cn.NextSibling;
      end;
    end;
    if not Auto then
    begin
      SetLength(Steps, Length(Steps) + 1);
      Last := High(Steps);
      Steps[Last].Effects := nil;
      Steps[Last].Len := 0;
      Walk(Steps[Last], Kid(CT, 'p:childTnLst'), D);
    end
    else
    begin   { after the step before has played }
      Last := High(Steps);
      Walk(Steps[Last], Kid(CT, 'p:childTnLst'), Steps[Last].Len + D);
    end;
  end;

begin
  Targets := nil;
  SetLength(Steps, 1);
  Steps[0].Effects := nil;
  Steps[0].Len := 0;
  FillChar(Trans, SizeOf(Trans), 0);
  Trans.AdvClick := True;
  Trans.AdvTime := -1;
  if Trim(Xml) = '' then
    Exit;
  Doc := nil;
  St := TStringStream.Create(Xml);
  try
    try
      ReadXMLFile(Doc, St);
    except
      Doc := nil;
    end;
    if Doc = nil then
      Exit;
    Root := Doc.DocumentElement;
    { the transition: PowerPoint 2010's in an mc:Choice, its older equal in the mc:Fallback }
    L := TDOMElement(Root).GetElementsByTagName('p:transition');
    try
      Best := -1;
      for I := 0 to L.Count - 1 do
      begin
        C := L[I];
        Trans.Present := True;
        if Att(C, 'p14:dur') <> '' then
          Trans.Dur := NumAtt(C, 'p14:dur', 0);
        if Att(C, 'advClick') = '0' then
          Trans.AdvClick := False;
        if Att(C, 'advTm') <> '' then
          Trans.AdvTime := Round(NumAtt(C, 'advTm', -1));
        Spd := Att(C, 'spd');
        if (Trans.Dur = 0) and (Spd <> '') then
          if Spd = 'slow' then
            Trans.Dur := 1000
          else if Spd = 'med' then
            Trans.Dur := 750
          else
            Trans.Dur := 500;
        Kids := C.FirstChild;
        while Kids <> nil do
        begin
          if (Kids.NodeType = ELEMENT_NODE) and (Kids.NodeName <> 'p:sndAc') and (Kids.NodeName <> 'p:extLst') then
          begin
            K := LocalName(string(Kids.NodeName));
            Known := KnownKind(K);
            if (Best < 0) or (Known and (Best = 0)) then
            begin   { the first kind, or the first that is played as it is }
              Trans.Kind := K;
              Trans.Dir := Att(Kids, 'dir');
              Trans.Orient := Att(Kids, 'orient');
              Trans.ThruBlack := Att(Kids, 'thruBlk') = '1';
              Best := Ord(Known);
            end;
            Break;
          end;
          Kids := Kids.NextSibling;
        end;
      end;
      if Trans.Present and (Trans.Kind = '') then
        Trans.Present := False      { a transition element with no effect: the slide comes as it is }
      else if Trans.Present and (Trans.Dur <= 0) then
        Trans.Dur := 500;
      if Trans.Kind = 'cut' then
        Trans.Dur := IfThen(Trans.ThruBlack, 300, 0);
    finally
      L.Free;
    end;
    { the main sequence's groups }
    Main := nil;
    L := TDOMElement(Root).GetElementsByTagName('p:cTn');
    try
      for I := 0 to L.Count - 1 do
        if Att(L[I], 'nodeType') = 'mainSeq' then
        begin
          Main := L[I];
          Break;
        end;
    finally
      L.Free;
    end;
    if Main <> nil then
    begin
      C := Kid(Main, 'p:childTnLst');
      if C <> nil then
        C := C.FirstChild;
      while C <> nil do
      begin
        if (C.NodeType = ELEMENT_NODE) and (C.NodeName = 'p:par') then
          AddGroup(C);
        C := C.NextSibling;
      end;
    end;
  finally
    Doc.Free;
    St.Free;
  end;
end;

{ ---- pixels ---- }

function Premul(Px: UInt32; M: Integer): UInt32; inline;
var
  A, R, G, B: UInt32;
begin
  if M >= 255 then
    Exit(Px);
  B := (Px and $FF) * UInt32(M) div 255;
  G := ((Px shr 8) and $FF) * UInt32(M) div 255;
  R := ((Px shr 16) and $FF) * UInt32(M) div 255;
  A := (Px shr 24) * UInt32(M) div 255;
  Result := B or (G shl 8) or (R shl 16) or (A shl 24);
end;

{ premultiplied source over the destination }
function Over(Dst, Src: UInt32): UInt32; inline;
var
  IA: UInt32;
begin
  IA := 255 - (Src shr 24);
  if IA = 0 then
    Exit(Src);
  if IA = 255 then
    Exit(Dst);
  Result := ((Src and $FF) + (Dst and $FF) * IA div 255) or
    ((((Src shr 8) and $FF) + ((Dst shr 8) and $FF) * IA div 255) shl 8) or
    ((((Src shr 16) and $FF) + ((Dst shr 16) and $FF) * IA div 255) shl 16) or
    (((Src shr 24) + (Dst shr 24) * IA div 255) shl 24);
end;

function Mix(A, B: UInt32; T: Double): UInt32; inline;
var
  K, IK: UInt32;
begin
  K := EnsureRange(Round(T * 256), 0, 256);
  IK := 256 - K;
  Result := (((A and $FF) * IK + (B and $FF) * K) shr 8) or
    (((((A shr 8) and $FF) * IK + ((B shr 8) and $FF) * K) shr 8) shl 8) or
    (((((A shr 16) and $FF) * IK + ((B shr 16) and $FF) * K) shr 8) shl 16) or $FF000000;
end;

{ a run's pixel at a point between them: bilinear, premultiplied; transparent outside }
function Sample(const R: TShowRun; X, Y: Double): UInt32;
var
  X0, Y0, I, J: Integer;
  FX, FY: Double;
  P: array[0..3] of UInt32;
  C, Ch: Integer;
  V: Double;
begin
  X := X - R.RX - 0.5;
  Y := Y - R.RY - 0.5;
  X0 := Floor(X);
  Y0 := Floor(Y);
  if (X0 < -1) or (Y0 < -1) or (X0 >= R.RW) or (Y0 >= R.RH) then
    Exit(0);
  FX := X - X0;
  FY := Y - Y0;
  for J := 0 to 1 do
    for I := 0 to 1 do
      if (X0 + I >= 0) and (Y0 + J >= 0) and (X0 + I < R.RW) and (Y0 + J < R.RH) then
        P[J * 2 + I] := R.RPx[(Y0 + J) * R.RW + X0 + I]
      else
        P[J * 2 + I] := 0;
  Result := 0;
  for C := 0 to 3 do
  begin
    Ch := C * 8;
    V := ((P[0] shr Ch) and $FF) * (1 - FX) * (1 - FY) + ((P[1] shr Ch) and $FF) * FX * (1 - FY) +
      ((P[2] shr Ch) and $FF) * (1 - FX) * FY + ((P[3] shr Ch) and $FF) * FX * FY;
    Result := Result or (UInt32(EnsureRange(Round(V), 0, 255)) shl Ch);
  end;
end;

function Hash01(X, Y: Integer): Double; inline;
var
  H: UInt32;
begin
  H := UInt32(X) * 374761393 + UInt32(Y) * 668265263;
  H := (H xor (H shr 13)) * 1274126177;
  Result := ((H xor (H shr 16)) and $FFFF) / 65536;
end;

{ how much of a point a filter shows, P of the way in: XN, YN where it is in the target's box (0..1 each way) }
function FilterMask(const F: string; P, XN, YN: Double; PX, PY: Integer): Double;
var
  Name, Arg: string;
  I: Integer;
  D, A: Double;
begin
  I := Pos('(', F);
  if I > 0 then
  begin
    Name := Copy(F, 1, I - 1);
    Arg := Copy(F, I + 1, Length(F) - I - 1);
  end
  else
  begin
    Name := F;
    Arg := '';
  end;
  Result := 1;
  if (Name = '') or (Name = 'fade') or (Name = 'image') then
    Exit(P);
  if Name = 'wipe' then
  begin
    if Arg = 'up' then D := 1 - YN
    else if Arg = 'down' then D := YN
    else if Arg = 'left' then D := 1 - XN
    else D := XN;
    Exit(Ord(D <= P));
  end;
  if Name = 'barn' then
  begin
    if Pos('Vertical', Arg) > 0 then D := Abs(XN - 0.5) * 2 else D := Abs(YN - 0.5) * 2;
    if Pos('in', Arg) = 1 then Exit(Ord(D >= 1 - P)) else Exit(Ord(D <= P));
  end;
  if Name = 'blinds' then
  begin
    if Arg = 'vertical' then D := Frac(XN * 6) else D := Frac(YN * 6);
    Exit(Ord(D <= P));
  end;
  if (Name = 'box') or (Name = 'circle') or (Name = 'diamond') or (Name = 'plus') then
  begin
    if Name = 'box' then D := Max(Abs(XN - 0.5), Abs(YN - 0.5)) * 2
    else if Name = 'circle' then D := Sqrt(Sqr(XN - 0.5) + Sqr(YN - 0.5)) * Sqrt(2)
    else if Name = 'diamond' then D := Abs(XN - 0.5) + Abs(YN - 0.5)
    else D := Min(Abs(XN - 0.5), Abs(YN - 0.5)) * 2;
    if Arg = 'in' then Exit(Ord(D >= 1 - P)) else Exit(Ord(D <= P));
  end;
  if Name = 'checkerboard' then
  begin
    if Arg = 'down' then
      D := Frac(YN * 4 + 0.5 * (Floor(XN * 4) mod 2))
    else
      D := Frac(XN * 4 + 0.5 * (Floor(YN * 4) mod 2));
    Exit(Ord(D <= P));
  end;
  if Name = 'dissolve' then
    Exit(Ord(Hash01(PX div 4, PY div 4) < P));
  if Name = 'randombar' then
  begin
    if Arg = 'vertical' then Exit(Ord(Hash01(PX div 2, 0) < P)) else Exit(Ord(Hash01(0, PY div 2) < P));
  end;
  if Name = 'strips' then
  begin
    if Arg = 'downLeft' then D := (1 - XN + YN) / 2
    else if Arg = 'upLeft' then D := (2 - XN - YN) / 2
    else if Arg = 'upRight' then D := (XN + 1 - YN) / 2
    else D := (XN + YN) / 2;
    Exit(Ord(D <= P));
  end;
  if (Name = 'wedge') or (Name = 'wheel') then
  begin
    A := ArcTan2(XN - 0.5, 0.5 - YN);     { from the top, clockwise }
    if A < 0 then A := A + 2 * Pi;
    if Name = 'wedge' then
      Exit(Ord(Abs(A - Pi) >= Pi * (1 - P)))
    else
    begin
      I := Max(1, StrToIntDef(Arg, 1));
      Exit(Ord(Frac(A / (2 * Pi) * I) <= P));
    end;
  end;
  Result := P;      { the rest: a fade }
end;

{ ---- the show ---- }

constructor TParadeShow.Create(AEdit: TParadeEdit; W, H: Integer; FromSlide: Integer);
var
  I, Len: Int32;
  Buf: string;
  T: TShowTiming;
begin
  inherited Create;
  FEdit := AEdit;
  FFrame := TLazIntfImage.Create(0, 0);
  for I := 0 to AEdit.SlideCount - 1 do
    if not AEdit.SlideHidden(I) or (I = FromSlide) then
    begin
      SetLength(FSlides, Length(FSlides) + 1);
      FSlides[High(FSlides)] := I;
      T := TShowTiming.Create;
      Len := 0;
      if pd_pptx_slide_timing(AEdit.Doc, AEdit.SlideSection(I), nil, 0, Len) = PD_OK then
      begin
        SetLength(Buf, Len);
        if (Len > 0) and (pd_pptx_slide_timing(AEdit.Doc, AEdit.SlideSection(I), PAnsiChar(Buf), Len, Len) = PD_OK) then
          T.Parse(Buf);
      end;
      SetLength(FTimings, Length(FTimings) + 1);
      FTimings[High(FTimings)] := T;
    end;
  FPos := 0;
  for I := 0 to High(FSlides) do
    if FSlides[I] = FromSlide then
      FPos := I;
  FPrepared := -1;
  FPhase := spIdle;
  Resize(W, H);
end;

destructor TParadeShow.Destroy;
var
  I: Integer;
begin
  for I := 0 to High(FTimings) do
    FTimings[I].Free;
  FFrame.Free;
  inherited Destroy;
end;

function TParadeShow.Timing(APos: Integer): TShowTiming;
begin
  if (APos >= 0) and (APos <= High(FTimings)) then
    Result := FTimings[APos]
  else
    Result := nil;
end;

function TParadeShow.TimingOf(ShowIndex: Integer): TShowTiming;
begin
  Result := Timing(ShowIndex);
end;

function TParadeShow.GetSlideRect: TRect;
begin
  Result := Rect(FOX, FOY, FOX + FSW, FOY + FSH);
end;

procedure TParadeShow.Resize(W, H: Integer);
var
  Desc: TRawImageDescription;
  WS, HS: pd_sp;
  Pg: Integer;
  K: Double;
begin
  FW := Max(16, W);
  FH := Max(16, H);
  Desc.Init_BPP32_B8G8R8A8_BIO_TTB(FW, FH);
  FFrame.DataDescription := Desc;
  FSW := FW;
  FSH := FH;
  Pg := -1;
  if Length(FSlides) > 0 then
    Pg := FEdit.SlidePage(FSlides[0]);
  if (Pg >= 0) and FEdit.PageSizeSp(Pg, WS, HS) and (WS > 0) and (HS > 0) then
  begin   { as large as it goes, the slide's shape kept }
    K := Min(FW / WS, FH / HS);
    FSW := Max(1, Round(WS * K));
    FSH := Max(1, Round(HS * K));
  end;
  FOX := (FW - FSW) div 2;
  FOY := (FH - FSH) div 2;
  SetLength(FFrom, FSW * FSH);
  SetLength(FTo, FSW * FSH);
  SetLength(FCur, FSW * FSH);
  FPrepared := -1;
  FChanged := True;
end;

function TParadeShow.SlideCount: Integer;
begin
  Result := Length(FSlides);
end;

function TParadeShow.CurrentSlide: Integer;
begin
  if (FPos >= 0) and (FPos < Length(FSlides)) then
    Result := FSlides[FPos]
  else
    Result := -1;
end;

function TParadeShow.StepsDone: Integer;
begin
  Result := FDone;
end;

function TParadeShow.StepCount: Integer;
begin
  if Timing(FPos) <> nil then
    Result := Timing(FPos).StepCount
  else
    Result := 0;
end;

function TParadeShow.KeepItem(const Item: pd_draw; Index: Integer): Boolean;
begin
  Result := (Index >= FLo) and (Index <= FHi);
end;

{ the target an item of the slide is drawn for: its paragraph's, else its shape's; -1 none }
function TParadeShow.TargetFor(T: TShowTiming; Index: Integer): Integer;
var
  I, Whole: Integer;
  Sh: Int32;
begin
  Result := -1;
  Sh := FItems[Index].shape;
  if (T = nil) or (Sh = 0) then
    Exit;
  Whole := -1;
  for I := 0 to High(T.Targets) do
    if T.Targets[I].Shape = Sh then
      if T.Targets[I].ParaA < 0 then
        Whole := I
      else if (FParaOf[Index] >= T.Targets[I].ParaA) and (FParaOf[Index] <= T.Targets[I].ParaB) then
        Exit(I);
  Result := Whole;
end;

{ the slide's runs drawn: the first (when it is not animated) on the paper, the others each over black and over
  white, so that where they cover, and how much, is known exactly }
procedure TParadeShow.Prepare(APos: Integer);
var
  T: TShowTiming;
  Pg, I, J, K, L, X, Y, X0, Y0, X1, Y1, A: Integer;
  Cur: Integer;
  Info: pd_block_info;
  WS, HS: pd_sp;
  Scale: Double;
  ImgW, ImgB: TLazIntfImage;
  Desc: TRawImageDescription;
  PW, PB: PUInt32;
  Runs: array of TShowRun;
  W1, B1: UInt32;
  DW, DB: Integer;
  Seen: array of Boolean;
begin
  if FPrepared = APos then
    Exit;
  FPrepared := APos;
  FRuns := nil;
  T := Timing(APos);
  Pg := -1;
  if (APos >= 0) and (APos < Length(FSlides)) then
    Pg := FEdit.SlidePage(FSlides[APos]);
  SetLength(FBase, FSW * FSH);
  for I := 0 to High(FBase) do
    FBase[I] := $FFFFFFFF;
  if T <> nil then
  begin
    SetLength(FBoxes, Length(T.Targets));
    SetLength(FFirstEntr, Length(T.Targets));
    for I := 0 to High(FBoxes) do
      FBoxes[I] := Rect(FSW, FSH, 0, 0);
  end;
  if (Pg < 0) or not FEdit.PageSizeSp(Pg, WS, HS) or (WS <= 0) then
    Exit;
  Scale := FSW / WS;
  FItems := FEdit.PageDrawItems(Pg);
  SetLength(FParaOf, Length(FItems));
  for I := 0 to High(FItems) do
  begin
    FParaOf[I] := -1;
    if (FItems[I].region = 5) and (FItems[I].block <> 0) and (pd_doc_block_info(FEdit.Doc, FItems[I].block, Info) = PD_OK) then
      FParaOf[I] := Info.index;
  end;
  { the runs: the display list cut where the target it is drawn for changes }
  Runs := nil;
  Cur := -2;
  for I := 0 to High(FItems) do
  begin
    L := TargetFor(T, I);
    if L <> Cur then
    begin
      SetLength(Runs, Length(Runs) + 1);
      Runs[High(Runs)].RTarget := L;
      Runs[High(Runs)].RLo := I;
      Cur := L;
    end;
    Runs[High(Runs)].RHi := I;
  end;
  ImgW := TLazIntfImage.Create(0, 0);
  ImgB := TLazIntfImage.Create(0, 0);
  try
    Desc.Init_BPP32_B8G8R8A8_BIO_TTB(FSW, FSH);
    ImgW.DataDescription := Desc;
    ImgB.DataDescription := Desc;
    { the paper, with the first run when it stays as it is }
    FLo := 0;
    FHi := -1;
    K := 0;
    if (Length(Runs) > 0) and (Runs[0].RTarget < 0) then
    begin
      FHi := Runs[0].RHi;
      K := 1;
    end;
    FEdit.RenderSlide(ImgW, Pg, Scale, clWhite, @KeepItem);
    for Y := 0 to FSH - 1 do
      Move(ImgW.GetDataLineStart(Y)^, FBase[Y * FSW], FSW * 4);
    for J := K to High(Runs) do
    begin
      FLo := Runs[J].RLo;
      FHi := Runs[J].RHi;
      FEdit.RenderSlide(ImgW, Pg, Scale, clWhite, @KeepItem);
      FEdit.RenderSlide(ImgB, Pg, Scale, clBlack, @KeepItem);
      X0 := FSW;
      Y0 := FSH;
      X1 := -1;
      Y1 := -1;
      for Y := 0 to FSH - 1 do
      begin
        PW := PUInt32(ImgW.GetDataLineStart(Y));
        PB := PUInt32(ImgB.GetDataLineStart(Y));
        for X := 0 to FSW - 1 do
        begin
          if ((PW[X] and $FFFFFF) <> $FFFFFF) or ((PB[X] and $FFFFFF) <> 0) then   { something drawn there }
          begin
            if X < X0 then X0 := X;
            if X > X1 then X1 := X;
            if Y < Y0 then Y0 := Y;
            if Y > Y1 then Y1 := Y;
          end;
        end;
      end;
      if X1 < X0 then
      begin   { nothing drawn }
        Runs[J].RW := 0;
        Runs[J].RH := 0;
        Continue;
      end;
      Runs[J].RX := X0;
      Runs[J].RY := Y0;
      Runs[J].RW := X1 - X0 + 1;
      Runs[J].RH := Y1 - Y0 + 1;
      SetLength(Runs[J].RPx, Runs[J].RW * Runs[J].RH);
      for Y := Y0 to Y1 do
      begin
        PW := PUInt32(ImgW.GetDataLineStart(Y));
        PB := PUInt32(ImgB.GetDataLineStart(Y));
        for X := X0 to X1 do
        begin
          W1 := PW[X];
          B1 := PB[X];
          { over white W = C + (1 - a) 255, over black B = C: a from the difference, C (premultiplied) is B }
          DW := ((W1 and $FF) + ((W1 shr 8) and $FF) + ((W1 shr 16) and $FF));
          DB := ((B1 and $FF) + ((B1 shr 8) and $FF) + ((B1 shr 16) and $FF));
          A := EnsureRange(255 - (DW - DB) div 3, 0, 255);
          if A = 0 then
            Runs[J].RPx[(Y - Y0) * Runs[J].RW + X - X0] := 0
          else
            Runs[J].RPx[(Y - Y0) * Runs[J].RW + X - X0] := (Min(B1 and $FF, A)) or (Min((B1 shr 8) and $FF, A) shl 8) or
              (Min((B1 shr 16) and $FF, A) shl 16) or (UInt32(A) shl 24);
        end;
      end;
      if Runs[J].RTarget >= 0 then
        with FBoxes[Runs[J].RTarget] do
        begin
          Left := Min(Left, X0);
          Top := Min(Top, Y0);
          Right := Max(Right, X1 + 1);
          Bottom := Max(Bottom, Y1 + 1);
        end;
    end;
  finally
    ImgW.Free;
    ImgB.Free;
  end;
  FRuns := nil;
  for J := K to High(Runs) do
    if Runs[J].RW > 0 then
    begin
      SetLength(FRuns, Length(FRuns) + 1);
      FRuns[High(FRuns)] := Runs[J];
    end;
  { which targets an entrance brings in: hidden until it does }
  if T <> nil then
  begin
    SetLength(Seen, Length(T.Targets));
    for I := 0 to High(T.Steps) do
      for J := 0 to High(T.Steps[I].Effects) do
        with T.Steps[I].Effects[J] do
          for K := 0 to High(Behs) do
            if (Behs[K].Target >= 0) and not Seen[Behs[K].Target] then
            begin
              Seen[Behs[K].Target] := True;
              FFirstEntr[Behs[K].Target] := Cls = 'entr';
            end;
    for I := 0 to High(FBoxes) do
      if FBoxes[I].Right <= FBoxes[I].Left then
        FBoxes[I] := Rect(0, 0, 0, 0);
  end;
end;

function TParadeShow.TargetBox(Shape: Int32; Para: Integer): TRect;
var
  T: TShowTiming;
  I: Integer;
begin
  Result := Rect(0, 0, 0, 0);
  Prepare(FPos);
  T := Timing(FPos);
  if T <> nil then
    for I := 0 to High(T.Targets) do
      if (T.Targets[I].Shape = Shape) and ((Para < 0) and (T.Targets[I].ParaA < 0) or
         (Para >= 0) and (T.Targets[I].ParaA <= Para) and (T.Targets[I].ParaB >= Para)) and (I <= High(FBoxes)) then
        Exit(FBoxes[I]);
end;

{ progress of a behaviour Local ms after the effect began: 0..1, its easing, repeats and reversal; False before it }
function BehProgress(const B: TShowBeh; Local: Double; out U: Double): Boolean;
var
  Cycle, Total, Ph, A, D, R: Double;
begin
  U := 0;
  Local := Local - B.Start;
  if Local < 0 then
    Exit(False);
  Result := True;
  if B.Dur <= 0 then
  begin
    U := 1;
    Exit;
  end;
  Cycle := B.Dur * (1 + Ord(B.AutoRev));
  Total := Cycle * B.Repeats;
  if Local >= Total then
  begin
    if B.AutoRev then U := 0 else U := 1;
    Exit;
  end;
  Ph := Local - Floor(Local / Cycle) * Cycle;
  U := Ph / B.Dur;
  if U > 1 then
    U := 2 - U;
  A := B.Accel;
  D := B.Decel;
  if A + D > 1 then
  begin
    A := A / (A + D);
    D := 1 - A;
  end;
  if A + D > 0 then
  begin   { SMIL's acceleration and deceleration }
    R := 1 / (1 - A / 2 - D / 2);
    if U < A then
      U := R * U * U / (2 * A)
    else if U <= 1 - D then
      U := R * (U - A / 2)
    else
      U := R * (U - A / 2 - Sqr(U - (1 - D)) / (2 * D));
  end;
  U := EnsureRange(U, 0, 1);
end;

{ a keyframe animation's value U of the way, its formulas of the target's place and size as they were (BX..BH) }
function AnimValue(const B: TShowBeh; U, BX, BY, BW, BH: Double; out V: Double): Boolean;
var
  I: Integer;
  V0, V1, T: Double;
begin
  Result := False;
  V := 0;
  if Length(B.Keys) = 0 then
    Exit;
  I := 0;
  while (I < High(B.Keys)) and (B.Keys[I + 1].Tm <= U) do
    Inc(I);
  if not ShowEval(B.Keys[I].Val, BX, BY, BW, BH, 0, V0) then
    Exit;
  if (I = High(B.Keys)) or B.Discrete then
    V := V0
  else
  begin
    if not ShowEval(B.Keys[I + 1].Val, BX, BY, BW, BH, 0, V1) then
      Exit;
    T := 0;
    if B.Keys[I + 1].Tm > B.Keys[I].Tm then
      T := (U - B.Keys[I].Tm) / (B.Keys[I + 1].Tm - B.Keys[I].Tm);
    V := V0 + (V1 - V0) * EnsureRange(T, 0, 1);
  end;
  if (B.Keys[I].Fmla <> '') and not ShowEval(B.Keys[I].Fmla, BX, BY, BW, BH, V, V) then
    Exit;
  Result := True;
end;

{ a motion path's point U of its length along }
procedure PathAt(const P: array of Double; U: Double; out X, Y: Double);
var
  I, N: Integer;
  Len, Acc, Seg, Want: Double;
begin
  X := 0;
  Y := 0;
  N := Length(P) div 2;
  if N = 0 then
    Exit;
  X := P[0];
  Y := P[1];
  Len := 0;
  for I := 1 to N - 1 do
    Len := Len + Hypot(P[2 * I] - P[2 * I - 2], P[2 * I + 1] - P[2 * I - 1]);
  if Len <= 0 then
    Exit;
  Want := EnsureRange(U, 0, 1) * Len;
  Acc := 0;
  for I := 1 to N - 1 do
  begin
    Seg := Hypot(P[2 * I] - P[2 * I - 2], P[2 * I + 1] - P[2 * I - 1]);
    if (Acc + Seg >= Want) and (Seg > 0) then
    begin
      X := P[2 * I - 2] + (P[2 * I] - P[2 * I - 2]) * (Want - Acc) / Seg;
      Y := P[2 * I - 1] + (P[2 * I + 1] - P[2 * I - 1]) * (Want - Acc) / Seg;
      Exit;
    end;
    Acc := Acc + Seg;
  end;
  X := P[2 * N - 2];
  Y := P[2 * N - 1];
end;

{ each target's state with Done steps played and the next PlayT ms into its own (PlayT < 0: not begun) }
procedure TParadeShow.StateAt(APos, Done: Integer; PlayT: Double; out S: array of TShowState);
var
  T: TShowTiming;
  I, J, K: Integer;
  U, V, BX, BY, BW, BH, MX, MY: Double;
  Local: Double;
begin
  T := Timing(APos);
  for I := 0 to High(S) do
  begin
    S[I].Visible := (I > High(FFirstEntr)) or not FFirstEntr[I];
    S[I].Opacity := 1;
    if I <= High(FBoxes) then
    begin
      S[I].CX := (FBoxes[I].Left + FBoxes[I].Right) / 2 / FSW;
      S[I].CY := (FBoxes[I].Top + FBoxes[I].Bottom) / 2 / FSH;
      S[I].W := (FBoxes[I].Right - FBoxes[I].Left) / FSW;
      S[I].H := (FBoxes[I].Bottom - FBoxes[I].Top) / FSH;
    end;
    S[I].Rot := 0;
    S[I].SX := 1;
    S[I].SY := 1;
    S[I].MX := 0;
    S[I].MY := 0;
    S[I].Filter := '';
    S[I].FilterP := 1;
  end;
  if T = nil then
    Exit;
  for I := 0 to Min(Done, High(T.Steps)) do
  begin
    if (I = Done) and (PlayT < 0) then
      Break;
    for J := 0 to High(T.Steps[I].Effects) do
      with T.Steps[I].Effects[J] do
      begin
        if I < Done then
          Local := 1e12
        else
          Local := PlayT - Start;
        if Local < 0 then
          Continue;
        for K := 0 to High(Behs) do
          if (Behs[K].Target >= 0) and (Behs[K].Target <= High(S)) and BehProgress(Behs[K], Local, U) then
            with Behs[K], S[Target] do
            begin
              BX := (FBoxes[Target].Left + FBoxes[Target].Right) / 2 / FSW;
              BY := (FBoxes[Target].Top + FBoxes[Target].Bottom) / 2 / FSH;
              BW := (FBoxes[Target].Right - FBoxes[Target].Left) / FSW;
              BH := (FBoxes[Target].Bottom - FBoxes[Target].Top) / FSH;
              case Kind of
                sbSet:
                  if Attr = 'style.visibility' then
                    Visible := ToVal = 'visible'
                  else if (Attr = 'style.opacity') and ShowEval(ToVal, BX, BY, BW, BH, 0, V) then
                    Opacity := EnsureRange(V, 0, 1);
                sbEffect:
                  if TransIn then
                  begin
                    Visible := True;
                    if U >= 1 then Filter := '' else
                    begin
                      Filter := Behs[K].Filter;
                      FilterP := U;
                    end;
                  end
                  else if U >= 1 then
                  begin
                    Filter := '';
                    Visible := False;
                  end
                  else
                  begin
                    Filter := Behs[K].Filter;
                    FilterP := 1 - U;
                  end;
                sbAnim:
                  if AnimValue(Behs[K], U, BX, BY, BW, BH, V) then
                    if Attr = 'ppt_x' then CX := V
                    else if Attr = 'ppt_y' then CY := V
                    else if Attr = 'ppt_w' then W := V
                    else if Attr = 'ppt_h' then H := V
                    else if (Attr = 'r') or (Attr = 'style.rotation') then Rot := V
                    else if Attr = 'style.opacity' then Opacity := EnsureRange(V, 0, 1);
                sbScale:
                  if HasSBy then
                  begin
                    SX := SX * (1 + (SByX - 1) * U);
                    SY := SY * (1 + (SByY - 1) * U);
                  end
                  else if HasSTo then
                  begin
                    if HasSFrom then
                    begin
                      SX := SFromX + (SToX - SFromX) * U;
                      SY := SFromY + (SToY - SFromY) * U;
                    end
                    else
                    begin
                      SX := SX + (SToX - SX) * U;
                      SY := SY + (SToY - SY) * U;
                    end;
                  end;
                sbRot:
                  if HasRotTo then
                  begin
                    if HasRotFrom then Rot := RotFrom + (RotTo - RotFrom) * U else Rot := Rot + (RotTo - Rot) * U;
                  end
                  else
                    Rot := Rot + RotBy * U;
                sbMotion:
                  begin
                    PathAt(Path, U, MX, MY);
                    S[Target].MX := MX;
                    S[Target].MY := MY;
                  end;
              end;
            end;
      end;
  end;
end;

procedure TParadeShow.RenderPlain(APos: Integer; var Px: array of UInt32);
begin
  Compose(APos, MaxInt div 2, -1, Px);
end;

{ the slide with Done steps played and PlayT ms into the next: its runs over the paper, each as its target is }
procedure TParadeShow.Compose(APos, Done: Integer; PlayT: Double; var Px: array of UInt32);
var
  T: TShowTiming;
  S: array of TShowState;
  R, X, Y, X0, Y0, X1, Y1, I: Integer;
  St: TShowState;
  M: Integer;
  BCX, BCY, CCX, CCY, FX, FY, Cs, Sn, DX, DY, SX, SY, XN, YN, BL, BT, BWd, BHt, Msk, Off: Double;
  Ident: Boolean;
  IX, IY: Integer;
  Src: UInt32;
  Bx: TRect;
  Name: string;
begin
  Prepare(APos);
  T := Timing(APos);
  if T <> nil then
    Done := Min(Done, T.StepCount)
  else
    Done := 0;
  Move(FBase[0], Px[0], Length(FBase) * 4);
  S := nil;
  if T <> nil then
  begin
    SetLength(S, Length(T.Targets));
    StateAt(APos, Done, PlayT, S);
  end;
  for R := 0 to High(FRuns) do
    with FRuns[R] do
    begin
      if RTarget >= 0 then
        St := S[RTarget]
      else
      begin
        St.Visible := True;
        St.Opacity := 1;
        St.Filter := '';
        St.FilterP := 1;
      end;
      if not St.Visible or (St.Opacity <= 0.002) then
        Continue;
      M := Round(255 * St.Opacity);
      if RTarget >= 0 then
      begin
        Bx := FBoxes[RTarget];
        BCX := (Bx.Left + Bx.Right) / 2;
        BCY := (Bx.Top + Bx.Bottom) / 2;
        BL := Bx.Left;
        BT := Bx.Top;
        BWd := Max(1, Bx.Right - Bx.Left);
        BHt := Max(1, Bx.Bottom - Bx.Top);
        CCX := (St.CX + St.MX) * FSW;
        CCY := (St.CY + St.MY) * FSH;
        FX := St.SX;
        FY := St.SY;
        if (St.W > 0) and (BWd > 1) then FX := FX * St.W * FSW / BWd;
        if (St.H > 0) and (BHt > 1) then FY := FY * St.H * FSH / BHt;
        if Abs(FX) < 1e-4 then FX := 1e-4;
        if Abs(FY) < 1e-4 then FY := 1e-4;
        Cs := Cos(DegToRad(St.Rot));
        Sn := Sin(DegToRad(St.Rot));
      end
      else
      begin
        BCX := 0; BCY := 0; CCX := 0; CCY := 0; FX := 1; FY := 1; Cs := 1; Sn := 0;
        BL := RX; BT := RY; BWd := Max(1, RW); BHt := Max(1, RH);
      end;
      Name := St.Filter;
      I := Pos('(', Name);
      if I > 0 then
        Name := Copy(Name, 1, I - 1);
      if (Name = '') or (Name = 'fade') then
      begin   { all of it: a fade is its opacity }
        if Name = 'fade' then
          M := Round(M * St.FilterP);
        Name := '';
      end;
      Off := 0;
      if Name = 'slide' then
        Off := 1 - St.FilterP;   { comes in from an edge, within its box }
      Ident := (Abs(CCX - BCX) < 0.5) and (Abs(CCY - BCY) < 0.5) and (Abs(FX - 1) < 1e-3) and (Abs(FY - 1) < 1e-3) and
        (Abs(Sn) < 1e-4) and (Cs > 0) and (Name = '');
      if Ident then
      begin   { where it was drawn: a pixel for a pixel }
        for Y := 0 to RH - 1 do
          for X := 0 to RW - 1 do
          begin
            Src := RPx[Y * RW + X];
            if Src = 0 then
              Continue;
            IX := RX + X;
            IY := RY + Y;
            Px[IY * FSW + IX] := Over(Px[IY * FSW + IX], Premul(Src, M));
          end;
        Continue;
      end;
      { moved, scaled or turned: the box it covers now, each pixel of that from where it was }
      X0 := FSW;
      Y0 := FSH;
      X1 := -1;
      Y1 := -1;
      for I := 0 to 3 do
      begin
        DX := ((RX + (I and 1) * RW) - BCX) * FX;
        DY := ((RY + (I shr 1) * RH) - BCY) * FY;
        SX := CCX + DX * Cs - DY * Sn;
        SY := CCY + DX * Sn + DY * Cs;
        X0 := Min(X0, Floor(SX) - 1);
        Y0 := Min(Y0, Floor(SY) - 1);
        X1 := Max(X1, Ceil(SX) + 1);
        Y1 := Max(Y1, Ceil(SY) + 1);
      end;
      X0 := Max(0, X0);
      Y0 := Max(0, Y0);
      X1 := Min(FSW - 1, X1);
      Y1 := Min(FSH - 1, Y1);
      for Y := Y0 to Y1 do
        for X := X0 to X1 do
        begin
          DX := X + 0.5 - CCX;
          DY := Y + 0.5 - CCY;
          SX := (DX * Cs + DY * Sn) / FX + BCX;     { turned back, then unscaled }
          SY := (-DX * Sn + DY * Cs) / FY + BCY;
          if Off <> 0 then
          begin   { a slide filter: its content shifted in, cut to its box }
            if (SX < BL) or (SX >= BL + BWd) or (SY < BT) or (SY >= BT + BHt) then
              Continue;
            if Pos('Bottom', St.Filter) > 0 then SY := SY + Off * BHt
            else if Pos('Top', St.Filter) > 0 then SY := SY - Off * BHt
            else if Pos('Left', St.Filter) > 0 then SX := SX - Off * BWd
            else SX := SX + Off * BWd;
          end;
          Src := Sample(FRuns[R], SX, SY);
          if Src = 0 then
            Continue;
          if (Name <> '') and (Name <> 'slide') then
          begin
            XN := (SX - BL) / BWd;
            YN := (SY - BT) / BHt;
            Msk := FilterMask(St.Filter, St.FilterP, XN, YN, X, Y);
            if Msk <= 0 then
              Continue;
            Src := Premul(Src, Round(255 * Msk));
          end;
          Px[Y * FSW + X] := Over(Px[Y * FSW + X], Premul(Src, M));
        end;
    end;
end;

{ a transition P of the way from FFrom to FTo, into Px }
procedure TParadeShow.Transition(const T: TShowTrans; P: Double; var Px: array of UInt32);
var
  X, Y, SX, SY, I: Integer;
  K, XN, YN, D, A: Double;
  Dir, Kind: string;
  Show: Boolean;

  function At(const Img: array of UInt32; AX, AY: Integer): UInt32; inline;
  begin
    if (AX >= 0) and (AY >= 0) and (AX < FSW) and (AY < FSH) then
      Result := Img[AY * FSW + AX]
    else
      Result := BLACK_PX;
  end;

begin
  P := EnsureRange(P, 0, 1);
  Kind := T.Kind;
  Dir := T.Dir;
  { PowerPoint 2010's: as the nearest of the older }
  if (Kind = 'pan') or (Kind = 'conveyor') or (Kind = 'ferris') or (Kind = 'gallery') or (Kind = 'flythrough') then
    Kind := 'push'
  else if (Kind = 'doors') or (Kind = 'window') then
  begin
    Kind := 'split';
    if Dir = '' then Dir := 'out';
  end
  else if (Kind = 'reveal') or (Kind = 'flash') then
    Kind := 'fade'
  else if (Kind = 'vortex') or (Kind = 'shred') or (Kind = 'glitter') or (Kind = 'honeycomb') or (Kind = 'ripple') then
    Kind := 'dissolve';
  if (Kind = 'fade') and T.ThruBlack then
  begin
    for I := 0 to High(Px) do
      if P < 0.5 then
        Px[I] := Mix(FFrom[I], BLACK_PX, P * 2)
      else
        Px[I] := Mix(BLACK_PX, FTo[I], P * 2 - 1);
    Exit;
  end;
  if (Kind = 'cut') or (Kind = '') then
  begin
    for I := 0 to High(Px) do
      if T.ThruBlack and (P < 1) then
        Px[I] := BLACK_PX
      else
        Px[I] := FTo[I];
    Exit;
  end;
  if (Kind = 'push') or (Kind = 'cover') or (Kind = 'pull') then
  begin
    { which way it goes: l (to the left) is the default, the new from the right }
    SX := 0;
    SY := 0;
    if Dir = 'r' then SX := 1
    else if Dir = 'u' then SY := -1
    else if Dir = 'd' then SY := 1
    else if Dir = 'lu' then begin SX := -1; SY := -1; end
    else if Dir = 'ru' then begin SX := 1; SY := -1; end
    else if Dir = 'ld' then begin SX := -1; SY := 1; end
    else if Dir = 'rd' then begin SX := 1; SY := 1; end
    else SX := -1;
    for Y := 0 to FSH - 1 do
      for X := 0 to FSW - 1 do
      begin
        if Kind = 'push' then
        begin   { both move: the old out, the new in behind it }
          I := Round(SX * P * FSW);
          D := Round(SY * P * FSH);
          if (X - I >= 0) and (X - I < FSW) and (Y - Round(D) >= 0) and (Y - Round(D) < FSH) then
            Px[Y * FSW + X] := At(FFrom, X - I, Y - Round(D))
          else    { the new one a slide behind it, coming the same way }
            Px[Y * FSW + X] := At(FTo, X - I + SX * FSW, Y - Round(D) + SY * FSH);
        end
        else if Kind = 'cover' then
        begin   { the new over the old }
          I := Round(SX * (P - 1) * FSW);
          D := Round(SY * (P - 1) * FSH);
          if (X - I >= 0) and (X - I < FSW) and (Y - Round(D) >= 0) and (Y - Round(D) < FSH) then
            Px[Y * FSW + X] := At(FTo, X - I, Y - Round(D))
          else
            Px[Y * FSW + X] := FFrom[Y * FSW + X];
        end
        else
        begin   { pull: the old away, the new under it }
          I := Round(SX * P * FSW);
          D := Round(SY * P * FSH);
          if (X - I >= 0) and (X - I < FSW) and (Y - Round(D) >= 0) and (Y - Round(D) < FSH) then
            Px[Y * FSW + X] := At(FFrom, X - I, Y - Round(D))
          else
            Px[Y * FSW + X] := FTo[Y * FSW + X];
        end;
      end;
    Exit;
  end;
  if Kind = 'zoom' then
  begin   { the new grows from the middle (in), or the old shrinks away (out) }
    for Y := 0 to FSH - 1 do
      for X := 0 to FSW - 1 do
      begin
        if Dir = 'out' then
        begin
          K := Max(1e-3, 1 - P);
          SX := Round(FSW / 2 + (X - FSW / 2) / K);
          SY := Round(FSH / 2 + (Y - FSH / 2) / K);
          if (SX >= 0) and (SY >= 0) and (SX < FSW) and (SY < FSH) then
            Px[Y * FSW + X] := Mix(FTo[Y * FSW + X], FFrom[SY * FSW + SX], 1 - P)
          else
            Px[Y * FSW + X] := FTo[Y * FSW + X];
        end
        else
        begin
          K := Max(1e-3, P);
          SX := Round(FSW / 2 + (X - FSW / 2) / K);
          SY := Round(FSH / 2 + (Y - FSH / 2) / K);
          if (SX >= 0) and (SY >= 0) and (SX < FSW) and (SY < FSH) then
            Px[Y * FSW + X] := Mix(FFrom[Y * FSW + X], FTo[SY * FSW + SX], P)
          else
            Px[Y * FSW + X] := FFrom[Y * FSW + X];
        end;
      end;
    Exit;
  end;
  for Y := 0 to FSH - 1 do
    for X := 0 to FSW - 1 do
    begin
      XN := (X + 0.5) / FSW;
      YN := (Y + 0.5) / FSH;
      Show := False;
      if Kind = 'wipe' then
      begin
        if Dir = 'r' then D := XN
        else if Dir = 'u' then D := 1 - YN
        else if Dir = 'd' then D := YN
        else D := 1 - XN;
        { a soft edge, a tenth of the way }
        K := EnsureRange((P * 1.1 - D) / 0.1, 0, 1);
        Px[Y * FSW + X] := Mix(FFrom[Y * FSW + X], FTo[Y * FSW + X], K);
        Continue;
      end
      else if Kind = 'split' then
      begin
        if T.Orient = 'vert' then D := Abs(XN - 0.5) * 2 else D := Abs(YN - 0.5) * 2;
        if Dir = 'in' then Show := D >= 1 - P else Show := D <= P;
      end
      else if (Kind = 'circle') or (Kind = 'diamond') or (Kind = 'plus') then
      begin
        if Kind = 'circle' then D := Sqrt(Sqr(XN - 0.5) + Sqr(YN - 0.5)) * Sqrt(2)
        else if Kind = 'diamond' then D := Abs(XN - 0.5) + Abs(YN - 0.5)
        else D := Min(Abs(XN - 0.5), Abs(YN - 0.5)) * 2;
        Show := D <= P;
      end
      else if (Kind = 'wedge') or (Kind = 'wheel') then
      begin
        A := ArcTan2(XN - 0.5, 0.5 - YN);
        if A < 0 then A := A + 2 * Pi;
        if Kind = 'wedge' then
          Show := Abs(A - Pi) >= Pi * (1 - P)
        else
          Show := Frac(A / (2 * Pi) * Max(1, StrToIntDef(T.Dir, 4))) <= P;
      end
      else if Kind = 'blinds' then
      begin
        if Dir = 'vert' then D := Frac(XN * 6) else D := Frac(YN * 6);
        Show := D <= P;
      end
      else if Kind = 'checker' then
      begin
        if Dir = 'vert' then D := Frac(YN * 8 + 0.5 * (Floor(XN * 8) mod 2)) else D := Frac(XN * 8 + 0.5 * (Floor(YN * 8) mod 2));
        Show := D <= P;
      end
      else if Kind = 'comb' then
      begin
        if Dir = 'vert' then
        begin
          if Floor(XN * 10) mod 2 = 0 then D := YN else D := 1 - YN;
        end
        else if Floor(YN * 10) mod 2 = 0 then D := XN else D := 1 - XN;
        Show := D <= P;
      end
      else if Kind = 'strips' then
      begin
        if Dir = 'lu' then D := (2 - XN - YN) / 2
        else if Dir = 'ru' then D := (XN + 1 - YN) / 2
        else if Dir = 'ld' then D := (1 - XN + YN) / 2
        else D := (XN + YN) / 2;
        Show := D <= P;
      end
      else if Kind = 'randomBar' then
      begin
        if Dir = 'vert' then Show := Hash01(X div 3, 0) < P else Show := Hash01(0, Y div 3) < P;
      end
      else if (Kind = 'dissolve') or (Kind = 'newsflash') then
        Show := Hash01(X div 4, Y div 4) < P
      else
      begin   { fade, and what is not known }
        Px[Y * FSW + X] := Mix(FFrom[Y * FSW + X], FTo[Y * FSW + X], P);
        Continue;
      end;
      if Show then
        Px[Y * FSW + X] := FTo[Y * FSW + X]
      else
        Px[Y * FSW + X] := FFrom[Y * FSW + X];
    end;
end;

procedure TParadeShow.ToFrame(const Px: array of UInt32);
var
  Y: Integer;
  P: PUInt32;
begin
  for Y := 0 to FH - 1 do
  begin
    P := PUInt32(FFrame.GetDataLineStart(Y));
    FillDWord(P^, FW, BLACK_PX);
    if (Y >= FOY) and (Y < FOY + FSH) and (Length(Px) >= FSW * FSH) then
      Move(Px[(Y - FOY) * FSW], P[FOX], FSW * 4);
  end;
  FChanged := True;
end;

procedure TParadeShow.BeginSteps(Now: Int64);
var
  T: TShowTiming;
begin
  T := Timing(FPos);
  FDone := 0;
  if (T <> nil) and (Length(T.Steps) > 0) and (Length(T.Steps[0].Effects) > 0) then
  begin   { what plays by itself as the slide begins }
    FPhase := spPlay;
    FStart := Now;
  end
  else
  begin
    FDone := 1;
    FPhase := spIdle;
    FIdleSince := Now;
  end;
  Compose(FPos, FDone, IfThen(FPhase = spPlay, 0, -1), FCur);
  ToFrame(FCur);
end;

procedure TParadeShow.Enter(APos: Integer; Now: Int64; WithTrans: Boolean);
var
  T: TShowTiming;
begin
  if APos >= Length(FSlides) then
  begin   { the end: black }
    FPos := Length(FSlides);
    FPhase := spEnd;
    SetLength(FCur, FSW * FSH);
    FillDWord(FCur[0], Length(FCur), BLACK_PX);
    ToFrame(FCur);
    Exit;
  end;
  T := Timing(APos);
  FPos := APos;
  if WithTrans and (T <> nil) and T.Trans.Present and (T.Trans.Dur > 0) then
  begin
    Move(FCur[0], FFrom[0], Length(FCur) * 4);
    Compose(APos, 0, -1, FTo);
    FPhase := spTrans;
    FStart := Now;
    FDone := 0;
    Transition(T.Trans, 0, FCur);
    ToFrame(FCur);
  end
  else
    BeginSteps(Now);
end;

procedure TParadeShow.Start(Now: Int64);
begin
  FFinished := False;
  SetLength(FCur, FSW * FSH);
  if Length(FCur) > 0 then
    FillDWord(FCur[0], Length(FCur), BLACK_PX);
  Enter(FPos, Now, True);
end;

function TParadeShow.Tick(Now: Int64): Boolean;
var
  T: TShowTiming;
  E: Double;
begin
  T := Timing(FPos);
  case FPhase of
    spTrans:
      begin
        E := (Now - FStart) / Max(1.0, T.Trans.Dur);
        if E >= 1 then
          BeginSteps(Now)
        else
        begin
          Transition(T.Trans, E, FCur);
          ToFrame(FCur);
        end;
      end;
    spPlay:
      begin
        E := Now - FStart;
        if (T = nil) or (FDone > High(T.Steps)) or (E >= T.Steps[FDone].Len) then
        begin
          Inc(FDone);
          FPhase := spIdle;
          FIdleSince := Now;
          Compose(FPos, FDone, -1, FCur);
        end
        else
          Compose(FPos, FDone, E, FCur);
        ToFrame(FCur);
      end;
    spIdle:
      if (T <> nil) and (T.Trans.AdvTime >= 0) and (FDone >= T.StepCount) and (Now - FIdleSince >= T.Trans.AdvTime) then
        Enter(FPos + 1, Now, True);
  end;
  Result := FChanged;
  FChanged := False;
end;

procedure TParadeShow.Next(Now: Int64);
var
  T: TShowTiming;
begin
  T := Timing(FPos);
  case FPhase of
    spEnd:
      FFinished := True;
    spTrans:
      BeginSteps(Now);
    spPlay:
      begin   { what plays, played out }
        Inc(FDone);
        FPhase := spIdle;
        FIdleSince := Now;
        Compose(FPos, FDone, -1, FCur);
        ToFrame(FCur);
      end;
    spIdle:
      if (T <> nil) and (FDone < T.StepCount) then
      begin
        FPhase := spPlay;
        FStart := Now;
        Compose(FPos, FDone, 0, FCur);
        ToFrame(FCur);
      end
      else
        Enter(FPos + 1, Now, True);
  end;
end;

procedure TParadeShow.Prev(Now: Int64);
var
  T: TShowTiming;
begin
  if FPhase = spEnd then
  begin
    FPos := Length(FSlides) - 1;
    T := Timing(FPos);
    if T <> nil then
      FDone := T.StepCount
    else
      FDone := 1;
    FPhase := spIdle;
    FIdleSince := Now;
    Compose(FPos, FDone, -1, FCur);
    ToFrame(FCur);
    Exit;
  end;
  if FPhase = spTrans then
    FDone := 0;
  if (FPhase = spPlay) or (FDone > 1) then
  begin   { a step back: the slide as it was before the last click }
    if FPhase <> spPlay then
      Dec(FDone);
    FPhase := spIdle;
    FIdleSince := Now;
    Compose(FPos, FDone, -1, FCur);
    ToFrame(FCur);
  end
  else if FPos > 0 then
  begin   { the slide before, as it ends }
    Dec(FPos);
    T := Timing(FPos);
    if T <> nil then
      FDone := T.StepCount
    else
      FDone := 1;
    FPhase := spIdle;
    FIdleSince := Now;
    Compose(FPos, FDone, -1, FCur);
    ToFrame(FCur);
  end;
end;

procedure TParadeShow.GoToSlide(SlideIndex: Integer; Now: Int64);
var
  I: Integer;
begin
  for I := 0 to High(FSlides) do
    if FSlides[I] = SlideIndex then
    begin
      Enter(I, Now, False);
      Exit;
    end;
end;

{ ---- the window ---- }

constructor TParadeShowForm.CreateShow(AOwner: TComponent; AEdit: TParadeEdit; FromSlide: Integer);
begin
  inherited CreateNew(AOwner);
  FEdit := AEdit;
  FFromSlide := FromSlide;
  Caption := 'Slide show';
  BorderStyle := bsNone;
  Color := clBlack;
  KeyPreview := True;
  FBmp := TBitmap.Create;
  FBlank := clBlack;
  FTimer := TTimer.Create(Self);
  FTimer.Interval := 15;
  FTimer.OnTimer := @TimerTick;
  FTimer.Enabled := False;
  FLastMove := GetTickCount64;
end;

destructor TParadeShowForm.Destroy;
begin
  FTimer.Enabled := False;
  FShow.Free;
  FBmp.Free;
  inherited Destroy;
end;

procedure TParadeShowForm.Resize;
begin
  inherited Resize;
  if (ClientWidth < 16) or (ClientHeight < 16) then
    Exit;
  if FShow = nil then
  begin
    FShow := TParadeShow.Create(FEdit, ClientWidth, ClientHeight, FFromSlide);
    FShow.Start(GetTickCount64);
    FTimer.Enabled := True;
  end
  else if (ClientWidth <> FShow.Frame.Width) or (ClientHeight <> FShow.Frame.Height) then
  begin
    FShow.Resize(ClientWidth, ClientHeight);
    FShow.GoToSlide(FShow.CurrentSlide, GetTickCount64);
  end;
  Refresh;
end;

procedure TParadeShowForm.Refresh;
begin
  if FShow <> nil then
    FBmp.LoadFromIntfImage(FShow.Frame);
  Invalidate;
end;

procedure TParadeShowForm.TimerTick(Sender: TObject);
begin
  if FShow = nil then
    Exit;
  if FShow.Tick(GetTickCount64) then
    Refresh;
  if (Cursor <> crNone) and (GetTickCount64 - FLastMove > 2500) then
    Cursor := crNone;
end;

procedure TParadeShowForm.Paint;
var
  S: string;
begin
  if FBlanked then
  begin
    Canvas.Brush.Color := FBlank;
    Canvas.FillRect(ClientRect);
    Exit;
  end;
  if FBmp.Width > 0 then
    Canvas.Draw(0, 0, FBmp)
  else
  begin
    Canvas.Brush.Color := clBlack;
    Canvas.FillRect(ClientRect);
  end;
  if (FShow <> nil) and (FShow.Phase = spEnd) then
  begin
    S := 'End of slide show, click to exit.';
    Canvas.Brush.Style := bsClear;
    Canvas.Font.Color := clWhite;
    Canvas.Font.Size := 12;
    Canvas.TextOut(20, 20, S);
    Canvas.Brush.Style := bsSolid;
  end;
end;

procedure TParadeShowForm.KeyDown(var Key: Word; Shift: TShiftState);
var
  Now: Int64;
  N: Integer;
begin
  Now := GetTickCount64;
  if FShow = nil then
    Exit;
  if FBlanked and not (Key in [VK_SHIFT, VK_CONTROL, VK_MENU]) then
  begin   { a blank screen: any key back to the slide }
    FBlanked := False;
    Invalidate;
    if Key in [Ord('B'), Ord('W'), VK_OEM_PERIOD, VK_OEM_COMMA] then
    begin
      Key := 0;
      Exit;
    end;
  end;
  case Key of
    VK_ESCAPE, VK_OEM_MINUS, VK_SUBTRACT:
      Close;
    VK_RIGHT, VK_DOWN, VK_SPACE, VK_NEXT, Ord('N'):
      FShow.Next(Now);
    VK_RETURN:
      if (FTyped <> '') and TryStrToInt(FTyped, N) then
        FShow.GoToSlide(N - 1, Now)
      else
        FShow.Next(Now);
    VK_LEFT, VK_UP, VK_BACK, VK_PRIOR, Ord('P'):
      FShow.Prev(Now);
    VK_HOME:
      if FShow.SlideCount > 0 then
        FShow.GoToSlide(FShow.FSlides[0], Now);
    VK_END:
      if FShow.SlideCount > 0 then
        FShow.GoToSlide(FShow.FSlides[High(FShow.FSlides)], Now);
    Ord('B'), VK_OEM_PERIOD:
      begin
        FBlank := clBlack;
        FBlanked := True;
        Invalidate;
      end;
    Ord('W'), VK_OEM_COMMA:
      begin
        FBlank := clWhite;
        FBlanked := True;
        Invalidate;
      end;
  else
    Exit;
  end;
  if not (Key in [Ord('0')..Ord('9'), VK_NUMPAD0..VK_NUMPAD9]) then
    FTyped := '';
  Key := 0;
  if FShow.Finished then
    Close
  else
    Refresh;
end;

procedure TParadeShowForm.KeyPress(var Key: Char);
begin
  if Key in ['0'..'9'] then
  begin   { a slide's number, then Enter: that slide }
    FTyped := FTyped + Key;
    Key := #0;
  end;
  inherited KeyPress(Key);
end;

procedure TParadeShowForm.MouseDown(Button: TMouseButton; Shift: TShiftState; X, Y: Integer);
begin
  inherited MouseDown(Button, Shift, X, Y);
  if FShow = nil then
    Exit;
  if FBlanked then
  begin
    FBlanked := False;
    Invalidate;
    Exit;
  end;
  if Button = mbLeft then
    FShow.Next(GetTickCount64)
  else if Button = mbRight then
    FShow.Prev(GetTickCount64);
  if FShow.Finished then
    Close
  else
    Refresh;
end;

procedure TParadeShowForm.MouseMove(Shift: TShiftState; X, Y: Integer);
begin
  inherited MouseMove(Shift, X, Y);
  FLastMove := GetTickCount64;
  if Cursor <> crDefault then
    Cursor := crDefault;
end;

function TParadeShowForm.DoMouseWheel(Shift: TShiftState; WheelDelta: Integer; MousePos: TPoint): Boolean;
begin
  Result := True;
  if FShow = nil then
    Exit;
  if WheelDelta < 0 then
    FShow.Next(GetTickCount64)
  else
    FShow.Prev(GetTickCount64);
  if FShow.Finished then
    Close
  else
    Refresh;
end;

procedure TParadeShowForm.DoClose(var CloseAction: TCloseAction);
begin
  FTimer.Enabled := False;
  CloseAction := caFree;
  if Assigned(FOnEnd) then
    FOnEnd(Self);
  inherited DoClose(CloseAction);
end;

function ParadeSlideShow(AEdit: TParadeEdit; FromSlide: Integer; AOnEnd: TNotifyEvent): TParadeShowForm;
begin
  Result := TParadeShowForm.CreateShow(Application, AEdit, FromSlide);
  Result.OnEnd := AOnEnd;
  Result.BoundsRect := Screen.MonitorFromWindow(Application.MainFormHandle).BoundsRect;
  Result.Show;
  Result.WindowState := wsFullScreen;
end;

initialization
  Dot := DefaultFormatSettings;
  Dot.DecimalSeparator := '.';
end.
