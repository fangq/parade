{ ParadeShowPdf: a presentation's slide show written as a PDF that plays it, for a viewer with no Office.

  The show (ParadeShow) is played through, headless, a click at a time.  Each still state -- the slide as a click
  leaves it -- is a page that waits for the next click (a key or a click goes on, in any viewer).  While an effect
  plays, its frames, Fps a second, are pages of their own that a viewer in full-screen (presentation) mode shows each
  for its time (/Dur) and goes on from by itself: Acrobat, Okular, Evince.  A slide's transition is the page's
  /Trans, the nearest PDF has.  The document opens full screen.

  The pages are pictures of the frames.  To keep it small, a page is drawn from a picture shared with the pages
  before it and the parts that have changed since, each a picture of its own drawn once and used by every page after
  it, until they are more than a whole frame is (then a whole frame again). }

unit ParadeShowPdf;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Math, StrUtils, Types, zstream, IntfGraphics, paradeedit, paradeshow, parade;

type
  TShowPdfOptions = record
    Width: Integer;         { the frames' width, px (their height the slide's shape) }
    Fps: Integer;           { frames a second while an effect plays }
    FullScreen: Boolean;    { the document opens in full-screen mode }
  end;

function ShowPdfDefaults: TShowPdfOptions;

{ AEdit's slide show to FileName; False, Err why, when it could not be }
function ParadeShowToPdf(AEdit: TParadeEdit; const FileName: string; const Opt: TShowPdfOptions;
  out Err: string): Boolean;

{ for a test: the pages the last export wrote, and how many waited for a click }
var
  ShowPdfPages, ShowPdfStill: Integer;

implementation

type
  TPatch = record
    Obj: Integer;       { the picture's object }
    R: TRect;           { where it is in the frame, px }
  end;

  TPdfPage = record
    Patches: array of TPatch;   { drawn in order: the first a whole frame }
    Dur: Double;                { seconds shown before the next by itself; 0 waits for a click }
    Trans: string;              { its /Trans dictionary, '' none }
  end;

  TPdfOut = class
  private
    FS: TFileStream;
    FOffsets: array of Int64;
  public
    constructor Create(const FileName: string);
    destructor Destroy; override;
    function NewObj: Integer;
    procedure BeginObj(N: Integer);
    procedure Put(const S: string);
    procedure PutBytes(const B: array of Byte; N: Integer);
    procedure Finish(Root: Integer);
  end;

function ShowPdfDefaults: TShowPdfOptions;
begin
  Result.Width := 1600;
  Result.Fps := 20;
  Result.FullScreen := True;
end;

{ ---- TPdfOut ---- }

constructor TPdfOut.Create(const FileName: string);
begin
  inherited Create;
  FS := TFileStream.Create(FileName, fmCreate);
  Put('%PDF-1.5'#10'%'#$E2#$E3#$CF#$D3#10);
end;

destructor TPdfOut.Destroy;
begin
  FS.Free;
  inherited Destroy;
end;

function TPdfOut.NewObj: Integer;
begin
  SetLength(FOffsets, Length(FOffsets) + 1);
  FOffsets[High(FOffsets)] := -1;
  Result := Length(FOffsets);    { (objects from 1) }
end;

procedure TPdfOut.BeginObj(N: Integer);
begin
  FOffsets[N - 1] := FS.Position;
  Put(IntToStr(N) + ' 0 obj'#10);
end;

procedure TPdfOut.Put(const S: string);
begin
  if S <> '' then
    FS.WriteBuffer(S[1], Length(S));
end;

procedure TPdfOut.PutBytes(const B: array of Byte; N: Integer);
begin
  if N > 0 then
    FS.WriteBuffer(B[0], N);
end;

procedure TPdfOut.Finish(Root: Integer);
var
  X: Int64;
  I: Integer;
begin
  X := FS.Position;
  Put('xref'#10'0 ' + IntToStr(Length(FOffsets) + 1) + #10'0000000000 65535 f '#10);
  for I := 0 to High(FOffsets) do
    Put(Format('%.10d 00000 n '#10, [Max(0, FOffsets[I])]));
  Put('trailer'#10'<< /Size ' + IntToStr(Length(FOffsets) + 1) + ' /Root ' + IntToStr(Root) + ' 0 R >>'#10 +
    'startxref'#10 + IntToStr(X) + #10'%%EOF'#10);
end;

{ ---- the export ---- }

function Num(V: Double): string;
begin
  Result := FloatToStrF(V, ffFixed, 0, 3, DefaultFormatSettings);
  Result := StringReplace(Result, DefaultFormatSettings.DecimalSeparator, '.', []);
  if Pos('.', Result) > 0 then
  begin
    while Result[Length(Result)] = '0' do
      Delete(Result, Length(Result), 1);
    if Result[Length(Result)] = '.' then
      Delete(Result, Length(Result), 1);
  end;
end;

{ a slide's transition as PDF has it (PDF 1.5's): the nearest kind, its direction, its time }
function TransDict(const T: TShowTrans): string;
var
  S, Di: string;
begin
  Result := '';
  if not T.Present or (T.Kind = '') then
    Exit;
  Di := '';
  case T.Dir of      { which way it moves: PDF's angle, counterclockwise from left to right }
    'r': Di := '0';
    'u': Di := '90';
    'l': Di := '180';
    'd': Di := '270';
    'ru', 'ur': Di := '45';
    'lu', 'ul': Di := '135';
    'ld', 'dl': Di := '225';
    'rd', 'dr': Di := '315';
  end;
  case T.Kind of
    'push': S := '/Push';
    'wipe': S := '/Wipe';
    'cover': S := '/Cover';
    'pull': S := '/Uncover';
    'split': S := '/Split';
    'blinds', 'randomBar', 'checker': S := '/Blinds';
    'dissolve': S := '/Dissolve';
    'zoom', 'box': S := '/Box';
    'cut': S := '/R';
  else
    S := '/Fade';
  end;
  Result := '<< /Type /Trans /S ' + S + ' /D ' + Num(Max(1, T.Dur) / 1000);
  if (Di <> '') and ((S = '/Push') or (S = '/Wipe') or (S = '/Cover') or (S = '/Uncover')) then
    Result := Result + ' /Di ' + Di;
  if S = '/Split' then
    Result := Result + IfThen(Pos('horz', T.Orient + T.Dir) > 0, ' /Dm /H', ' /Dm /V') +
      IfThen(Pos('in', T.Dir) > 0, ' /M /I', ' /M /O');
  if S = '/Blinds' then
    Result := Result + IfThen(Pos('vert', T.Dir) > 0, ' /Dm /V', ' /Dm /H');
  if S = '/Box' then
    Result := Result + IfThen(Pos('out', T.Dir) > 0, ' /M /O', ' /M /I');
  Result := Result + ' >>';
end;

function ParadeShowToPdf(AEdit: TParadeEdit; const FileName: string; const Opt: TShowPdfOptions;
  out Err: string): Boolean;
var
  Show: TParadeShow;
  Pdf: TPdfOut;
  Pages: array of TPdfPage;
  Stack: array of TPatch;         { what the page drawn now is drawn from }
  StackArea: Int64;
  W, H, Pg, Pos_, I, J, K, Root, PagesObj, Fps: Integer;
  WS, HS: pd_sp;
  PW, PH, Kp, Dt: Double;
  Now, Guard: Int64;
  T: TShowTiming;
  Content: string;
  PageObjs: array of Integer;

  { the frame's part R as a picture: RGB, deflated, each row first told from the one above (PNG's Up) }
  function Picture(const R: TRect): Integer;
  var
    Raw: TMemoryStream;
    Z: TCompressionStream;
    Packed_: TMemoryStream;
    Row, Prev: array of Byte;
    X, Y: Integer;
    P: PByte;
    B: array of Byte;
  begin
    Raw := TMemoryStream.Create;
    Packed_ := TMemoryStream.Create;
    try
      SetLength(Row, 1 + R.Width * 3);
      SetLength(Prev, R.Width * 3);
      FillChar(Prev[0], Length(Prev), 0);
      Z := TCompressionStream.Create(clDefault, Packed_);
      try
        for Y := R.Top to R.Bottom - 1 do
        begin
          P := Show.Frame.GetDataLineStart(Y);
          Row[0] := 2;     { Up }
          for X := 0 to R.Width - 1 do
          begin   { (B G R, the fourth byte not used) }
            Row[1 + X * 3] := Byte(P[(R.Left + X) * 4 + 2] - Prev[X * 3]);
            Row[2 + X * 3] := Byte(P[(R.Left + X) * 4 + 1] - Prev[X * 3 + 1]);
            Row[3 + X * 3] := Byte(P[(R.Left + X) * 4] - Prev[X * 3 + 2]);
            Prev[X * 3] := P[(R.Left + X) * 4 + 2];
            Prev[X * 3 + 1] := P[(R.Left + X) * 4 + 1];
            Prev[X * 3 + 2] := P[(R.Left + X) * 4];
          end;
          Z.WriteBuffer(Row[0], Length(Row));
        end;
      finally
        Z.Free;
      end;
      Result := Pdf.NewObj;
      Pdf.BeginObj(Result);
      Pdf.Put(Format('<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB ' +
        '/BitsPerComponent 8 /Filter /FlateDecode /DecodeParms << /Predictor 15 /Colors 3 /BitsPerComponent 8 ' +
        '/Columns %d >> /Length %d >>'#10'stream'#10, [R.Width, R.Height, R.Width, Packed_.Size]));
      SetLength(B, Packed_.Size);
      Packed_.Position := 0;
      if Packed_.Size > 0 then
        Packed_.ReadBuffer(B[0], Packed_.Size);
      Pdf.PutBytes(B, Length(B));
      Pdf.Put(#10'endstream'#10'endobj'#10);
    finally
      Packed_.Free;
      Raw.Free;
    end;
  end;

  { the frame as it is now, whole: what the pages from here on are drawn from }
  procedure Whole;
  begin
    SetLength(Stack, 1);
    Stack[0].R := Rect(0, 0, W, H);
    Stack[0].Obj := Picture(Stack[0].R);
    StackArea := 0;
    Show.TakeChanged;
  end;

  { a page of the frame as it is now: what has changed since the page before, on what that was drawn from }
  procedure AddPage(Dur: Double; const Trans: string; ForceNew: Boolean);
  var
    Parts: TRectArray;
    R: TRect;
    A: Int64;
    N: Integer;
  begin
    Parts := Show.TakeChanged;
    if (Length(Parts) = 0) and not ForceNew and (Length(Pages) > 0) then
    begin   { nothing changed: the page before shown longer (or, still, waiting) }
      if Dur <= 0 then
        Pages[High(Pages)].Dur := 0
      else if Pages[High(Pages)].Dur > 0 then
        Pages[High(Pages)].Dur := Pages[High(Pages)].Dur + Dur;
      Exit;
    end;
    A := 0;
    for R in Parts do
      A := A + Int64(R.Width) * R.Height;
    if StackArea + A > Int64(W) * H then
    begin   { (more than a frame drawn over the one drawn from: a whole frame again) }
      SetLength(Stack, 1);
      Stack[0].R := Rect(0, 0, W, H);
      Stack[0].Obj := Picture(Stack[0].R);
      StackArea := 0;
    end
    else
      for R in Parts do
      begin
        N := Length(Stack);
        SetLength(Stack, N + 1);
        Stack[N].R := R;
        Stack[N].Obj := Picture(R);
        StackArea := StackArea + Int64(R.Width) * R.Height;
      end;
    SetLength(Pages, Length(Pages) + 1);
    Pages[High(Pages)].Patches := Copy(Stack, 0, Length(Stack));
    Pages[High(Pages)].Dur := Dur;
    Pages[High(Pages)].Trans := Trans;
  end;

  { the steps playing: a frame each Dt, until it is still }
  procedure Play;
  begin
    Guard := 0;
    while (Show.Phase = spPlay) and (Guard < 100000) do
    begin
      Now := Now + Max(1, Round(Dt * 1000));
      Show.Tick(Now);
      if Show.Phase = spPlay then
        AddPage(Dt, '', False);
      Inc(Guard);
    end;
  end;

begin
  Result := False;
  Err := '';
  ShowPdfPages := 0;
  ShowPdfStill := 0;
  if AEdit.SlideCount = 0 then
  begin
    Err := 'no slides';
    Exit;
  end;
  Pg := AEdit.SlidePage(0);
  if (Pg < 0) or not AEdit.PageSizeSp(Pg, WS, HS) or (WS <= 0) or (HS <= 0) then
  begin
    Err := 'the slides have no size';
    Exit;
  end;
  W := Max(64, Opt.Width);
  H := Max(16, Round(W * HS / WS));
  PW := WS / PD_SP_PER_PT;
  PH := HS / PD_SP_PER_PT;
  Kp := PW / W;
  Fps := EnsureRange(Opt.Fps, 1, 60);
  Dt := 1 / Fps;
  Pages := nil;
  Show := TParadeShow.Create(AEdit, W, H, 0);
  try
    if Show.SlideCount = 0 then
    begin
      Err := 'every slide is hidden';
      Exit;
    end;
    try
      Pdf := TPdfOut.Create(FileName);
    except
      on E: Exception do
      begin
        Err := E.Message;
        Exit;
      end;
    end;
    try
      Now := 1000;
      Show.Start(Now);
      Pos_ := 0;
      while (Show.Phase <> spEnd) and (Pos_ < Show.SlideCount) do
      begin
        T := Show.TimingOf(Pos_);
        if Show.Phase = spTrans then
          Show.Next(Now);     { (the transition is the page's own, PDF's) }
        { the slide as it begins }
        Whole;
        SetLength(Pages, Length(Pages) + 1);
        Pages[High(Pages)].Patches := Copy(Stack, 0, 1);
        Pages[High(Pages)].Dur := Dt;
        Pages[High(Pages)].Trans := '';
        if T <> nil then
          Pages[High(Pages)].Trans := TransDict(T.Trans);
        Play;
        AddPage(0, '', False);  { still: waits for the click }
        { each click: its frames, then still }
        while (Show.Phase = spIdle) and (Show.StepsDone < Show.StepCount) do
        begin
          Now := Now + 1000;
          Show.Next(Now);
          if Show.Phase = spPlay then
            AddPage(Dt, '', False)
          else
            AddPage(0, '', False);
          Play;
          AddPage(0, '', False);
        end;
        if (T <> nil) and (T.Trans.AdvTime >= 0) then
          Pages[High(Pages)].Dur := Max(0.001, T.Trans.AdvTime / 1000);   { (on by itself) }
        Now := Now + 1000;
        Show.Next(Now);
        Inc(Pos_);
      end;
      { the pages, then what holds them }
      PagesObj := Pdf.NewObj;
      SetLength(PageObjs, Length(Pages));
      for I := 0 to High(Pages) do
      begin
        Content := '';
        for J := 0 to High(Pages[I].Patches) do
          with Pages[I].Patches[J] do
            Content := Content + 'q ' + Num(R.Width * Kp) + ' 0 0 ' + Num(R.Height * Kp) + ' ' + Num(R.Left * Kp) + ' ' +
              Num(PH - R.Bottom * Kp) + ' cm /I' + IntToStr(Obj) + ' Do Q'#10;
        K := Pdf.NewObj;
        Pdf.BeginObj(K);
        Pdf.Put('<< /Length ' + IntToStr(Length(Content)) + ' >>'#10'stream'#10 + Content + 'endstream'#10'endobj'#10);
        PageObjs[I] := Pdf.NewObj;
        Pdf.BeginObj(PageObjs[I]);
        Content := '<< /Type /Page /Parent ' + IntToStr(PagesObj) + ' 0 R /MediaBox [0 0 ' + Num(PW) + ' ' + Num(PH) +
          '] /Contents ' + IntToStr(K) + ' 0 R /Resources << /XObject <<';
        for J := 0 to High(Pages[I].Patches) do
          Content := Content + ' /I' + IntToStr(Pages[I].Patches[J].Obj) + ' ' + IntToStr(Pages[I].Patches[J].Obj) + ' 0 R';
        Content := Content + ' >> >>';
        if Pages[I].Dur > 0 then
          Content := Content + ' /Dur ' + Num(Max(0.001, Pages[I].Dur))
        else
          Inc(ShowPdfStill);
        if Pages[I].Trans <> '' then
          Content := Content + ' /Trans ' + Pages[I].Trans;
        Pdf.Put(Content + ' >>'#10'endobj'#10);
      end;
      Pdf.BeginObj(PagesObj);
      Content := '<< /Type /Pages /Count ' + IntToStr(Length(Pages)) + ' /Kids [';
      for I := 0 to High(PageObjs) do
        Content := Content + IntToStr(PageObjs[I]) + ' 0 R ';
      Pdf.Put(Content + '] >>'#10'endobj'#10);
      Root := Pdf.NewObj;
      Pdf.BeginObj(Root);
      Pdf.Put('<< /Type /Catalog /Pages ' + IntToStr(PagesObj) + ' 0 R' + IfThen(Opt.FullScreen, ' /PageMode /FullScreen',
        '') + ' >>'#10'endobj'#10);
      Pdf.Finish(Root);
      ShowPdfPages := Length(Pages);
      Result := True;
    finally
      Pdf.Free;
    end;
  finally
    Show.Free;
  end;
end;

end.
