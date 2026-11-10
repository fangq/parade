{ ParadeShowPdf: a presentation's slide show written as a PDF that plays it, for a viewer with no Office.

  The show (ParadeShow) is played through, headless, a click at a time.  Each still state -- the slide as a click
  leaves it -- is a page that waits for the next click (a key or a click goes on, in any viewer).  While an effect
  plays, its frames, Fps a second, are pages of their own that a viewer in full-screen (presentation) mode shows each
  for its time (/Dur) and goes on from by itself: Acrobat, Okular, Evince.  A slide's transition is the page's
  /Trans, the nearest PDF has.  The document opens full screen.

  The pages are drawn as the slides are, not as pictures of them: text, shapes and pictures as Parade's PDF has them
  (pd_layout_write_pdf_show).  A slide is cut, as the show cuts it, into runs of its display list, each drawn for one
  animated thing (or for none); each run is written once, a form, and each page draws the runs as the show has them at
  that moment: moved, scaled, turned (a matrix), see-through (an opacity) and cut by its filter (a wipe's rectangle, a
  wheel's spokes, a circle ...).  So a frame costs a few hundred bytes, and a slide that nothing animates is as it is
  in the slides' PDF.  Of the filters only dissolve, a scatter of pixels, has no outline: it fades in. }

unit ParadeShowPdf;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Math, StrUtils, Types, ctypes, paradeedit, paradeshow, parade;

type
  TShowPdfOptions = record
    Width: Integer;         { the show's frame, px, the animated things are measured in (their boxes, a px in it) }
    Fps: Integer;           { frames a second while an effect plays }
    FullScreen: Boolean;    { the document opens in full-screen mode }
    JpegQuality: Integer;   { 0: the slides' pictures as they are; 1..100: photographs as JPEGs of this quality }
    Script: Boolean;        { a script that plays the frames, for a viewer that runs one but keeps no /Dur: Chrome }
  end;

function ShowPdfDefaults: TShowPdfOptions;

{ AEdit's slide show to FileName; False, Err why, when it could not be }
function ParadeShowToPdf(AEdit: TParadeEdit; const FileName: string; const Opt: TShowPdfOptions;
  out Err: string): Boolean;

{ the document's script that plays the frames: Durs, a page's seconds, 0 for one that waits; for a test }
function ShowPdfScript(const Durs: array of Double): string;

{ a place's filter as the clip that cuts it, in the slide's px: Dest the frame's own (else as the run is drawn, before
  its matrix); '' none; for a test }
function ShowFilterClip(const Pl: TShowPlace; SlideW, SlideH: Integer; out Dest: Boolean): string;

{ for a test: the pages the last export wrote, and how many waited for a click }
var
  ShowPdfPages, ShowPdfStill: Integer;

implementation

var
  Dot: TFormatSettings;

function ShowPdfDefaults: TShowPdfOptions;
begin
  Result.Width := 1600;
  Result.Fps := 20;
  Result.FullScreen := True;
  Result.JpegQuality := 85;
  Result.Script := True;
end;

{ Chrome (PDFium) runs a document's JavaScript but shows every page until a key goes on; so the script, a tick every
  20 ms, goes on from a frame once it has been shown its time, and a click (a key) plays the effect through.  In
  full-screen mode Acrobat keeps /Dur itself: the script there does nothing, or the frames would go twice as fast }
function ShowPdfScript(const Durs: array of Double): string;
var
  I: Integer;
  D: string;
begin
  D := '';
  for I := 0 to High(Durs) do
  begin
    if I > 0 then
      D := D + ',';
    D := D + IntToStr(Round(1000 * Max(0, Durs[I])));
  end;
  { a run of frames timed from its first: a frame drawn late, the next ones skipped, so an effect takes its time;
    a page turned to by a key (not by the script, Chrome a tick behind) times a run from there }
  Result := 'var pdD=[' + D + '];var pdDoc=this;var pdLast=-1;var pdBase=0;var pdT0=0;var pdSet=0;' +
    'function pdTick(){try{if(typeof app.fs!="undefined"&&app.fs.isFullScreen)return;}catch(e){}' +
    'var p=pdDoc.pageNum;var n=new Date().getTime();' +
    'if(p!=pdLast){if(n-pdSet<300&&p<pdLast&&p>=pdBase)return;pdLast=p;pdBase=p;pdT0=n;return;}' +
    'var q=pdBase;var a=0;while(q<pdD.length&&pdD[q]>0&&q+1<pdDoc.numPages&&a+pdD[q]<=n-pdT0){a+=pdD[q];q++;}' +
    'if(q>p){pdDoc.pageNum=q;pdLast=q;pdSet=n;}}' +
    'app.setInterval("pdTick()",20);';
end;

{ S as a PDF string's text: \, ( and ) escaped }
function PdfText(const S: string): string;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(S) do
    if S[I] in ['\', '(', ')'] then
      Result := Result + '\' + S[I]
    else
      Result := Result + S[I];
end;

{ V with up to D decimals, '.' the point, no zeros after it }
function Num(V: Double; D: Integer = 3): string;
begin
  if Abs(V) < 0.5 * Power(10, -D) then
    Exit('0');
  Result := FloatToStrF(V, ffFixed, 0, D, Dot);
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

{ ParadeShow's, for random bars: the same columns and rows }
function Hash01(X, Y: Integer): Double; inline;
var
  H: UInt32;
begin
  H := UInt32(X) * 374761393 + UInt32(Y) * 668265263;
  H := (H xor (H shr 13)) * 1274126177;
  Result := ((H xor (H shr 16)) and $FFFF) / 65536;
end;

const
  E_ = 0.05;     { how far a cut reaches past the box where it does not cut: the edges' smoothing }

function ShowFilterClip(const Pl: TShowPlace; SlideW, SlideH: Integer; out Dest: Boolean): string;
var
  Name, Arg, S: string;
  I, K, N, Row: Integer;
  P, H, Off, A0, A1, A, R: Double;
  EvenOdd: Boolean;

  function PX(XN: Double): string;
  begin
    Result := Num(Pl.BoxX + XN * Pl.BoxW);
  end;

  function PY(YN: Double): string;
  begin
    Result := Num(Pl.BoxY + YN * Pl.BoxH);
  end;

  { a rectangle of the box, in its 0..1, as a path of its own }
  procedure Rc(X0, Y0, X1, Y1: Double);
  begin
    if (X1 <= X0) or (Y1 <= Y0) then
      Exit;
    S := S + PX(X0) + ' ' + PY(Y0) + ' m ' + PX(X1) + ' ' + PY(Y0) + ' l ' + PX(X1) + ' ' + PY(Y1) + ' l ' + PX(X0) +
      ' ' + PY(Y1) + ' l h ';
  end;

  procedure Poly(const Pts: array of Double);
  var
    J: Integer;
  begin
    if Length(Pts) < 6 then
      Exit;
    S := S + PX(Pts[0]) + ' ' + PY(Pts[1]) + ' m ';
    J := 2;
    while J < High(Pts) do
    begin
      S := S + PX(Pts[J]) + ' ' + PY(Pts[J + 1]) + ' l ';
      Inc(J, 2);
    end;
    S := S + 'h ';
  end;

  { the box's ellipse about its middle, radius Rd of its width and height: four curves }
  procedure Ell(Rd: Double);
  const
    K_ = 0.5522847498;
  begin
    if Rd <= 0 then
      Exit;
    S := S + PX(0.5 + Rd) + ' ' + PY(0.5) + ' m ' +
      PX(0.5 + Rd) + ' ' + PY(0.5 + Rd * K_) + ' ' + PX(0.5 + Rd * K_) + ' ' + PY(0.5 + Rd) + ' ' + PX(0.5) + ' ' + PY(0.5 + Rd) + ' c ' +
      PX(0.5 - Rd * K_) + ' ' + PY(0.5 + Rd) + ' ' + PX(0.5 - Rd) + ' ' + PY(0.5 + Rd * K_) + ' ' + PX(0.5 - Rd) + ' ' + PY(0.5) + ' c ' +
      PX(0.5 - Rd) + ' ' + PY(0.5 - Rd * K_) + ' ' + PX(0.5 - Rd * K_) + ' ' + PY(0.5 - Rd) + ' ' + PX(0.5) + ' ' + PY(0.5 - Rd) + ' c ' +
      PX(0.5 + Rd * K_) + ' ' + PY(0.5 - Rd) + ' ' + PX(0.5 + Rd) + ' ' + PY(0.5 - Rd * K_) + ' ' + PX(0.5 + Rd) + ' ' + PY(0.5) + ' c h ';
  end;

  { the box's sector from angle Fr to To (radians from the top, clockwise), past its corners }
  procedure Sector(Fr, To_: Double);
  var
    J, Steps: Integer;
    B: Double;
  begin
    if To_ <= Fr then
      Exit;
    Steps := Max(1, Ceil((To_ - Fr) / DegToRad(3)));
    S := S + PX(0.5) + ' ' + PY(0.5) + ' m ';
    for J := 0 to Steps do
    begin
      B := Fr + (To_ - Fr) * J / Steps;
      S := S + PX(0.5 + Sin(B)) + ' ' + PY(0.5 - Cos(B)) + ' l ';
    end;
    S := S + 'h ';
  end;

  { what of the box (reaching E_ past it) has Ax XN + Ay YN <= C }
  procedure HalfPlane(Ax, Ay, C: Double);
  var
    Q, O: array of Double;
    J, M: Integer;
    X0, Y0, X1, Y1, V0, V1, T: Double;
  begin
    Q := [-E_, -E_, 1 + E_, -E_, 1 + E_, 1 + E_, -E_, 1 + E_];
    O := nil;
    M := Length(Q) div 2;
    for J := 0 to M - 1 do
    begin
      X0 := Q[2 * J]; Y0 := Q[2 * J + 1];
      X1 := Q[2 * ((J + 1) mod M)]; Y1 := Q[2 * ((J + 1) mod M) + 1];
      V0 := Ax * X0 + Ay * Y0 - C;
      V1 := Ax * X1 + Ay * Y1 - C;
      if V0 <= 0 then
        O := Concat(O, [X0, Y0]);
      if (V0 <= 0) <> (V1 <= 0) then
      begin
        T := V0 / (V0 - V1);
        O := Concat(O, [X0 + (X1 - X0) * T, Y0 + (Y1 - Y0) * T]);
      end;
    end;
    Poly(O);
  end;

  { the frame's columns (Vert) or rows, two px each, a random bar shows: rectangles of the slide }
  procedure Bars(Vert: Boolean);
  var
    J, Lim, Start_: Integer;
    On_: Boolean;
  begin
    if Vert then Lim := (SlideW + 1) div 2 else Lim := (SlideH + 1) div 2;
    Start_ := -1;
    for J := 0 to Lim do
    begin
      if J < Lim then
      begin
        if Vert then On_ := Hash01(J, 0) < P else On_ := Hash01(0, J) < P;
      end
      else
        On_ := False;
      if On_ and (Start_ < 0) then
        Start_ := J
      else if not On_ and (Start_ >= 0) then
      begin
        if Vert then
          S := S + IntToStr(2 * Start_) + ' 0 ' + IntToStr(2 * (J - Start_)) + ' ' + IntToStr(SlideH) + ' re '
        else
          S := S + '0 ' + IntToStr(2 * Start_) + ' ' + IntToStr(SlideW) + ' ' + IntToStr(2 * (J - Start_)) + ' re ';
        Start_ := -1;
      end;
    end;
  end;

begin
  Result := '';
  Dest := False;
  I := Pos('(', Pl.Filter);
  if I > 0 then
  begin
    Name := Copy(Pl.Filter, 1, I - 1);
    Arg := Copy(Pl.Filter, I + 1, Length(Pl.Filter) - I - 1);
  end
  else
  begin
    Name := Pl.Filter;
    Arg := '';
  end;
  P := EnsureRange(Pl.FilterP, 0, 1);
  S := '';
  EvenOdd := False;
  if Name = 'slide' then
    Rc(0, 0, 1, 1)
  else if Name = 'wipe' then
  begin
    if Arg = 'up' then Rc(-E_, 1 - P, 1 + E_, 1 + E_)
    else if Arg = 'down' then Rc(-E_, -E_, 1 + E_, P)
    else if Arg = 'left' then Rc(1 - P, -E_, 1 + E_, 1 + E_)
    else Rc(-E_, -E_, P, 1 + E_);
  end
  else if Name = 'barn' then
  begin
    H := P / 2;
    if Pos('in', Arg) = 1 then H := (1 - P) / 2;
    if Pos('in', Arg) = 1 then
    begin   { from the sides in: what is beyond H of the middle }
      if Pos('Vertical', Arg) > 0 then
      begin
        Rc(-E_, -E_, 0.5 - H, 1 + E_);
        Rc(0.5 + H, -E_, 1 + E_, 1 + E_);
      end
      else
      begin
        Rc(-E_, -E_, 1 + E_, 0.5 - H);
        Rc(-E_, 0.5 + H, 1 + E_, 1 + E_);
      end;
    end
    else if Pos('Vertical', Arg) > 0 then
      Rc(0.5 - H, -E_, 0.5 + H, 1 + E_)
    else
      Rc(-E_, 0.5 - H, 1 + E_, 0.5 + H);
  end
  else if Name = 'blinds' then
  begin
    for K := 0 to 5 do
      if Arg = 'vertical' then
        Rc(IfThen(K = 0, -E_, K / 6), -E_, (K + P) / 6, 1 + E_)
      else
        Rc(-E_, IfThen(K = 0, -E_, K / 6), 1 + E_, (K + P) / 6);
  end
  else if (Name = 'box') or (Name = 'circle') or (Name = 'diamond') or (Name = 'plus') then
  begin
    if Arg = 'in' then
    begin   { what is beyond 1 - P of the way out: all of it, the shape within cut away }
      R := 1 - P;
      EvenOdd := Name <> 'plus';
      if Name <> 'plus' then
        Rc(-E_, -E_, 1 + E_, 1 + E_);
    end
    else
      R := P;
    if Name = 'box' then
      Rc(0.5 - R / 2, 0.5 - R / 2, 0.5 + R / 2, 0.5 + R / 2)
    else if Name = 'circle' then
      Ell(R / Sqrt(2))
    else if Name = 'diamond' then
    begin
      if R > 0 then
        Poly([0.5, 0.5 - R, 0.5 + R, 0.5, 0.5, 0.5 + R, 0.5 - R, 0.5]);
    end
    else if Arg = 'in' then
    begin   { both ways beyond R/2 of the middle: the corners }
      H := R / 2;
      Rc(-E_, -E_, 0.5 - H, 0.5 - H);
      Rc(0.5 + H, -E_, 1 + E_, 0.5 - H);
      Rc(-E_, 0.5 + H, 0.5 - H, 1 + E_);
      Rc(0.5 + H, 0.5 + H, 1 + E_, 1 + E_);
    end
    else
    begin   { either way within R/2: a cross }
      Rc(0.5 - R / 2, -E_, 0.5 + R / 2, 1 + E_);
      Rc(-E_, 0.5 - R / 2, 1 + E_, 0.5 + R / 2);
    end;
  end
  else if Name = 'checkerboard' then
  begin
    for Row := 0 to 3 do
    begin
      Off := 0.5 * (Row mod 2);
      for K := 0 to 4 do
      begin
        A0 := Max(IfThen(Row = 0, -E_, 0), (K - Off) / 4);
        A1 := Min(1 + E_, (K - Off + P) / 4);
        if A1 <= A0 then
          Continue;
        if Arg = 'down' then
          Rc(IfThen(Row = 0, -E_, Row / 4), A0, IfThen(Row = 3, 1 + E_, (Row + 1) / 4), A1)
        else
          Rc(A0, IfThen(Row = 0, -E_, Row / 4), A1, IfThen(Row = 3, 1 + E_, (Row + 1) / 4));
      end;
    end;
  end
  else if Name = 'randombar' then
  begin
    Dest := True;
    Bars(Arg = 'vertical');
    if S = '' then
      S := '0 0 0 0 re ';
  end
  else if Name = 'strips' then
  begin
    if Arg = 'downLeft' then HalfPlane(-1, 1, 2 * P - 1)
    else if Arg = 'upLeft' then HalfPlane(-1, -1, 2 * P - 2)
    else if Arg = 'upRight' then HalfPlane(1, -1, 2 * P - 1)
    else HalfPlane(1, 1, 2 * P);
  end
  else if Name = 'wedge' then
    Sector(-Pi * P, Pi * P)
  else if Name = 'wheel' then
  begin
    N := Max(1, StrToIntDef(Arg, 1));
    for K := 0 to N - 1 do
    begin
      A := 2 * Pi / N;
      Sector(K * A, (K + P) * A);
    end;
  end
  else
    Exit('');       { a fade, or what has no outline (dissolve): its opacity }
  if S = '' then
    S := Num(Pl.BoxX) + ' ' + Num(Pl.BoxY) + ' 0 0 re ';    { (nothing of it shows yet) }
  Result := S + IfThen(EvenOdd, 'W* n', 'W n');
end;

{ ---- the export ---- }

type
  TPartKey = record
    Page, Lo, Hi, Group: Integer;
  end;

  TOutPage = record
    Page: Integer;
    Content: AnsiString;
    Extra: AnsiString;
    Dur: Double;            { seconds shown before the next by itself; 0 waits for a click }
    Trans: string;
  end;

function WriteCb(user: Pointer; data: Pointer; len: csize_t): cint; cdecl;
begin
  try
    if len > 0 then
      TStream(user).WriteBuffer(data^, len);
    Result := 0;
  except
    Result := -1;
  end;
end;

function ParadeShowToPdf(AEdit: TParadeEdit; const FileName: string; const Opt: TShowPdfOptions;
  out Err: string): Boolean;
var
  Show: TParadeShow;
  Parts: array of TPartKey;
  Pages: array of TOutPage;
  W, H, Pg, Pos_, I, Fps: Integer;
  WS, HS: pd_sp;
  PW, PH, Kp, Dt: Double;
  Now, Guard: Int64;
  T: TShowTiming;
  CParts: array of pd_pdf_part;
  CPages: array of pd_pdf_show_page;
  CShow: pd_pdf_show;
  POpt: pd_pdf_options;
  Catalog: AnsiString;
  Durs: array of Double;
  Fs: TFileStream;
  St: pd_status;

  function PartOf(APage, ALo, AHi, AGroup: Integer): Integer;
  var
    J: Integer;
  begin
    for J := 0 to High(Parts) do
      if (Parts[J].Page = APage) and (Parts[J].Lo = ALo) and (Parts[J].Hi = AHi) and (Parts[J].Group = AGroup) then
        Exit(J);
    Result := Length(Parts);
    SetLength(Parts, Result + 1);
    Parts[Result].Page := APage;
    Parts[Result].Lo := ALo;
    Parts[Result].Hi := AHi;
    Parts[Result].Group := AGroup;
  end;

  { the frame the show drew last, as the page draws it: its runs as forms, each placed as the show has it }
  function SceneContent(out APage: Integer): AnsiString;
  var
    Sc: TShowScene;
    J: Integer;
    Clip: string;
    Dest, Plain: Boolean;
    ToPt, ToPx: string;
  begin
    Sc := Show.Scene;
    APage := Sc.Page;
    Result := '';
    if Sc.Page < 0 then
      Exit;
    { the slide's px (y down) to the page's points, and back }
    ToPt := Num(Kp, 6) + ' 0 0 ' + Num(-Kp, 6) + ' 0 ' + Num(PH, 4) + ' cm ';
    ToPx := Num(1 / Kp, 6) + ' 0 0 ' + Num(-1 / Kp, 6) + ' 0 ' + Num(PH / Kp, 4) + ' cm ';
    if Sc.BaseHi >= 0 then
      Result := Result + '/P' + IntToStr(PartOf(Sc.Page, 0, Sc.BaseHi, 0)) + ' Do'#10;
    for J := 0 to High(Sc.Places) do
      with Sc.Places[J] do
      begin
        Clip := ShowFilterClip(Sc.Places[J], Show.SlideW, Show.SlideH, Dest);
        Plain := (Clip = '') and (Opacity >= 0.999) and (M[0] = 1) and (M[1] = 0) and (M[2] = 0) and (M[3] = 1) and
          (M[4] = 0) and (M[5] = 0);
        if Plain then
        begin
          Result := Result + '/P' + IntToStr(PartOf(Sc.Page, Lo, Hi, 0)) + ' Do'#10;
          Continue;
        end;
        if Round(255 * Opacity) <= 0 then
          Continue;
        Result := Result + 'q ';
        if Round(255 * Opacity) < 255 then
          Result := Result + '/GO' + IntToStr(Round(255 * Opacity)) + ' gs ';
        Result := Result + ToPt;
        if Dest and (Clip <> '') then
          Result := Result + Clip + ' ';
        Result := Result + Num(M[0], 6) + ' ' + Num(M[1], 6) + ' ' + Num(M[2], 6) + ' ' + Num(M[3], 6) + ' ' +
          Num(M[4], 3) + ' ' + Num(M[5], 3) + ' cm ';
        if not Dest and (Clip <> '') then
          Result := Result + Clip + ' ';
        if (ShiftX <> 0) or (ShiftY <> 0) then
          Result := Result + '1 0 0 1 ' + Num(ShiftX) + ' ' + Num(ShiftY) + ' cm ';
        { a group only while a shape fades as one (its parts not each seen through the other); a viewer draws each
          group aside, at the page's size, a frame several times slower }
        Result := Result + ToPx + '/P' + IntToStr(PartOf(Sc.Page, Lo, Hi, Ord((Target >= 0) and
          (Round(255 * Opacity) < 255)))) + ' Do Q'#10;
      end;
  end;

  { a page of the frame as it is now; the same as the page before: that one shown longer (or, still, waiting) }
  procedure AddPage(Dur: Double; const Trans: string; ForceNew: Boolean);
  var
    C: AnsiString;
    APage: Integer;
  begin
    C := SceneContent(APage);
    if APage < 0 then
      Exit;
    if not ForceNew and (Length(Pages) > 0) and (Pages[High(Pages)].Content = C) then
    begin
      if Dur <= 0 then
        Pages[High(Pages)].Dur := 0
      else if Pages[High(Pages)].Dur > 0 then
        Pages[High(Pages)].Dur := Pages[High(Pages)].Dur + Dur;
      Exit;
    end;
    SetLength(Pages, Length(Pages) + 1);
    Pages[High(Pages)].Page := APage;
    Pages[High(Pages)].Content := C;
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
  if (Pg < 0) or not AEdit.PageSizeSp(Pg, WS, HS) or (WS <= 0) or (HS <= 0) or (AEdit.Layout = nil) then
  begin
    Err := 'the slides have no size';
    Exit;
  end;
  W := Max(64, Opt.Width);
  H := Max(16, Round(W * HS / WS));
  PW := WS / PD_SP_PER_PT;
  PH := HS / PD_SP_PER_PT;
  Fps := EnsureRange(Opt.Fps, 1, 60);
  Dt := 1 / Fps;
  Pages := nil;
  Parts := nil;
  Show := TParadeShow.Create(AEdit, W, H, 0);
  try
    if Show.SlideCount = 0 then
    begin
      Err := 'every slide is hidden';
      Exit;
    end;
    Kp := PW / Max(1, Show.SlideW);
    Now := 1000;
    Show.Start(Now);
    Pos_ := 0;
    while (Show.Phase <> spEnd) and (Pos_ < Show.SlideCount) do
    begin
      T := Show.TimingOf(Pos_);
      if Show.Phase = spTrans then
        Show.Next(Now);     { (the transition is the page's own, PDF's) }
      { the slide as it begins }
      AddPage(Dt, IfThen(T <> nil, TransDict(T.Trans), ''), True);
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
      if (T <> nil) and (T.Trans.AdvTime >= 0) and (Length(Pages) > 0) then
        Pages[High(Pages)].Dur := Max(0.001, T.Trans.AdvTime / 1000);   { (on by itself) }
      Now := Now + 1000;
      Show.Next(Now);
      Inc(Pos_);
    end;
  finally
    Show.Free;
  end;
  if Length(Pages) = 0 then
  begin
    Err := 'nothing to show';
    Exit;
  end;
  { the pages to Parade's PDF writer: the parts, then the pages that draw them }
  SetLength(CParts, Length(Parts));
  for I := 0 to High(Parts) do
  begin
    CParts[I].page := Parts[I].Page;
    CParts[I].first := Parts[I].Lo;
    CParts[I].last := Parts[I].Hi;
    CParts[I].group := Parts[I].Group;
  end;
  SetLength(CPages, Length(Pages));
  for I := 0 to High(Pages) do
  begin
    Pages[I].Extra := '';
    if Pages[I].Dur > 0 then
      Pages[I].Extra := '/Dur ' + Num(Max(0.001, Pages[I].Dur))
    else
      Inc(ShowPdfStill);
    if Pages[I].Trans <> '' then
      Pages[I].Extra := Pages[I].Extra + ' /Trans ' + Pages[I].Trans;
    CPages[I].page := Pages[I].Page;
    CPages[I].content := PAnsiChar(Pages[I].Content);
    CPages[I].extra := PAnsiChar(Pages[I].Extra);
  end;
  Catalog := IfThen(Opt.FullScreen, '/PageMode /FullScreen', '');
  if Opt.Script then
  begin
    SetLength(Durs, Length(Pages));
    for I := 0 to High(Pages) do
      Durs[I] := Pages[I].Dur;
    Catalog := Catalog + ' /OpenAction << /S /JavaScript /JS (' + PdfText(ShowPdfScript(Durs)) + ') >>';
  end;
  FillChar(CShow, SizeOf(CShow), 0);
  if Length(CParts) > 0 then
    CShow.parts := @CParts[0];
  CShow.nparts := Length(CParts);
  CShow.pages := @CPages[0];
  CShow.npages := Length(CPages);
  CShow.catalog := PAnsiChar(Catalog);
  pd_pdf_options_init(POpt);
  POpt.jpeg_quality := EnsureRange(Opt.JpegQuality, 0, 100);
  try
    Fs := TFileStream.Create(FileName, fmCreate);
  except
    on X: Exception do
    begin
      Err := X.Message;
      Exit;
    end;
  end;
  try
    St := pd_layout_write_pdf_show(AEdit.Layout, @POpt, @CShow, @WriteCb, Fs);
  finally
    Fs.Free;
  end;
  if St <> PD_OK then
  begin
    Err := 'the PDF could not be written (' + IntToStr(Ord(St)) + ')';
    Exit;
  end;
  ShowPdfPages := Length(Pages);
  Result := True;
end;

initialization
  Dot := DefaultFormatSettings;
  Dot.DecimalSeparator := '.';
  Dot.ThousandSeparator := #0;

end.
