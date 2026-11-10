unit paradeanim;

{ A slide's transition and animations as something to edit: read from the <pd:tail> XML a slide keeps
  (TParadeEdit.SlideTiming, pd_pptx_slide_timing), changed, and written back in the same form (SetSlideTiming).

  The main sequence is read as a list of effects, each starting on a click, with the one before it or after it
  (PowerPoint's Start), with its own delay and duration. An effect keeps its behaviours as they were read (so a
  preset this unit cannot make still plays and is written as it came), scaled when its duration changes; a new
  one, or one whose preset is changed, gets behaviours made here (Appear, Fade, Fly In/Out, Wipe, Zoom, Spin,
  Grow/Shrink, Disappear). Written again, the sequence's time containers are made from that list: a group a
  click, a part of it each "after previous", the ids numbered in order. What else the timing has (triggered
  sequences, media) and what follows it is kept; the build list keeps the entries for shapes still animated.

  The transition is kept as it was read (mc:AlternateContent and PowerPoint 2010's kinds with it) until it is
  changed; then it is a p:transition of its kind, direction, speed and advance. }

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, Math, StrUtils, DOM, XMLRead;

type
  TAnimStart = (asClick, asWith, asAfter);

  TAnimEffect = record
    Cls: string;                { entr, exit, emph, path }
    Preset, Subtype: Integer;
    Shape: Int32;               { the shape it animates (spid): its id on the slide }
    ParaSt, ParaEnd: Integer;   { the paragraphs of its text it animates, -1: the whole shape }
    Start: TAnimStart;
    Delay: Integer;             { ms after it starts }
    Dur: Integer;               { ms, one play of it }
    Attrs: string;              { its time node's other attributes, as read (' accel="..." repeatCount="..."') }
    GrpId: string;
    Pre, Post: string;          { its time node's other children (endSync, iterate; subTnLst), as read }
    Behs: string;               { its behaviours' XML, made for BehDur }
    BehDur: Integer;
  end;
  TAnimEffects = array of TAnimEffect;

  TAnimTrans = record
    Kind: string;               { '' none; fade, push, wipe ... (the p: element's name), or p14:vortex as read }
    KindAttrs: string;          { its attributes besides its direction, as read (' thruBlk="1"') }
    Dir: string;                { l r u d, lu ru ld rd, horz vert, in out, 'horz in' (split: orient and dir) }
    Speed: string;              { slow, med, fast }
    AdvClick: Boolean;          { a click goes on }
    AdvTime: Integer;           { ms after which it goes on by itself, -1 never }
    Raw: string;                { as read; written while not Changed }
    Changed: Boolean;
  end;

  TAnimPreset = record
    Cls: string;
    Preset, Subtype: Integer;
    Name: string;
  end;

  TParadeAnim = class
  private
    FNs: string;                { the root's attributes (namespaces) }
    FAfter: string;             { what the tail holds besides its transition and timing }
    FOthers: string;            { the root time node's children besides the main sequence }
    FBld: TStringList;          { the build list's entries; Objects: their spid }
  public
    Effects: TAnimEffects;
    Trans: TAnimTrans;
    constructor Create;
    destructor Destroy; override;
    { from SlideTiming's XML ('' none); False (and nothing) when it is not XML }
    function Load(const Xml: string): Boolean;
    { SetSlideTiming's XML; '' when there is neither a transition nor an animation }
    function Save: string;
    { a new effect (an AnimPresets entry) on a shape, at At (-1 the end); its index }
    function Add(Shape: Int32; const Cls: string; Preset, Subtype: Integer; Start: TAnimStart; Dur: Integer;
      At: Integer = -1): Integer;
    procedure Delete(I: Integer);
    procedure Move(I, ToI: Integer);
    { an effect made another (an AnimPresets entry): new behaviours, its target and timing kept }
    procedure SetPreset(I: Integer; const Cls: string; Preset, Subtype: Integer);
    { the transition set (Kind '' none), Changed }
    procedure SetTransition(const Kind, Dir, Speed: string; AdvClick: Boolean; AdvTime: Integer);
    { the click each effect plays on (1 the first click, 0 before any: on the slide's start) }
    function ClickOf(I: Integer): Integer;
  end;

{ the effects this unit makes, for an Add menu }
function AnimPresets: specialize TArray<TAnimPreset>;
{ an effect's name: "Fade", "Fly In", "Entrance 37" ... }
function AnimPresetName(const Cls: string; Preset: Integer): string;
{ a direction's name for an effect's subtype ('' none) and the subtypes it can have ('' none) }
function AnimSubtypeName(Subtype: Integer): string;
function AnimSubtypes(const Cls: string; Preset: Integer): string;
{ the transitions it writes (Kind), each's name, and the directions one can have (comma separated, '' none) }
function AnimTransKinds: string;
function AnimTransName(const Kind: string): string;
function AnimTransDirs(const Kind: string): string;
function AnimDirName(const Dir: string): string;
{ behaviours for an effect: its target, Dur ms }
function AnimMakeBehs(const Cls: string; Preset, Subtype: Integer; Shape: Int32; ParaSt, ParaEnd, Dur: Integer): string;

implementation

const
  DefaultNs = ' xmlns:pd="urn:parade" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" ' +
    'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" ' +
    'xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main"';

{ ---- XML ---- }

function Esc(const S: string): string;
begin
  Result := StringReplace(S, '&', '&amp;', [rfReplaceAll]);
  Result := StringReplace(Result, '<', '&lt;', [rfReplaceAll]);
  Result := StringReplace(Result, '>', '&gt;', [rfReplaceAll]);
  Result := StringReplace(Result, '"', '&quot;', [rfReplaceAll]);
end;

function U8(const S: DOMString): string;
begin
  Result := UTF8Encode(S);
end;

{ a node as XML: elements, their attributes and text, nothing else }
function NodeXml(N: TDOMNode): string;
var
  I: Integer;
  C: TDOMNode;
begin
  Result := '';
  if N = nil then
    Exit;
  case N.NodeType of
    ELEMENT_NODE:
      begin
        Result := '<' + U8(N.NodeName);
        if N.Attributes <> nil then
          for I := 0 to N.Attributes.Length - 1 do
            Result := Result + ' ' + U8(N.Attributes[I].NodeName) + '="' + Esc(U8(N.Attributes[I].NodeValue)) + '"';
        if N.FirstChild = nil then
          Result := Result + '/>'
        else
        begin
          Result := Result + '>';
          C := N.FirstChild;
          while C <> nil do
          begin
            Result := Result + NodeXml(C);
            C := C.NextSibling;
          end;
          Result := Result + '</' + U8(N.NodeName) + '>';
        end;
      end;
    TEXT_NODE, CDATA_SECTION_NODE:
      Result := Esc(U8(N.NodeValue));
  end;
end;

function Kid(N: TDOMNode; const AName: string): TDOMNode;
begin
  Result := nil;
  if N = nil then
    Exit;
  Result := N.FirstChild;
  while (Result <> nil) and not ((Result.NodeType = ELEMENT_NODE) and (U8(Result.NodeName) = AName)) do
    Result := Result.NextSibling;
end;

function Att(N: TDOMNode; const AName: string): string;
begin
  Result := '';
  if (N <> nil) and (N is TDOMElement) then
    Result := U8(TDOMElement(N).GetAttribute(DOMString(AName)));
end;

function HasAtt(N: TDOMNode; const AName: string): Boolean;
begin
  Result := (N <> nil) and (N is TDOMElement) and TDOMElement(N).HasAttribute(DOMString(AName));
end;

function IntAtt(N: TDOMNode; const AName: string; ADefault: Integer): Integer;
begin
  Result := StrToIntDef(Att(N, AName), ADefault);
end;

{ the first descendant element named AName }
function Find(N: TDOMNode; const AName: string): TDOMNode;
var
  C: TDOMNode;
begin
  Result := nil;
  if N = nil then
    Exit;
  C := N.FirstChild;
  while C <> nil do
  begin
    if C.NodeType = ELEMENT_NODE then
    begin
      if U8(C.NodeName) = AName then
        Exit(C);
      Result := Find(C, AName);
      if Result <> nil then
        Exit;
    end;
    C := C.NextSibling;
  end;
end;

{ a time node's start: its first condition's numeric delay (0 none, -1 indefinite) }
function CondDelay(CTn: TDOMNode): Integer;
var
  C: TDOMNode;
begin
  Result := 0;
  C := Kid(Kid(CTn, 'p:stCondLst'), 'p:cond');
  if C = nil then
    Exit;
  if Att(C, 'delay') = 'indefinite' then
    Exit(-1);
  Result := IntAtt(C, 'delay', 0);
end;

{ the latest end of the time nodes under N: delay + dur, both numeric }
function BehEnd(N: TDOMNode): Integer;
var
  C: TDOMNode;
  D: Integer;
begin
  Result := 0;
  C := N.FirstChild;
  while C <> nil do
  begin
    if C.NodeType = ELEMENT_NODE then
    begin
      if U8(C.NodeName) = 'p:cTn' then
      begin
        D := IntAtt(C, 'dur', 0);
        Result := Max(Result, Max(0, CondDelay(C)) + D);
      end;
      Result := Max(Result, BehEnd(C));
    end;
    C := C.NextSibling;
  end;
end;

{ every ' dur="N"' and '<p:cond delay="N"' in S (numbers: not 'indefinite') times F }
function ScaleTimes(const S: string; F: Double): string;

  function Scale(const In_, Key: string): string;
  var
    P, Q, From: Integer;
    V: Int64;
  begin
    Result := '';
    From := 1;
    P := Pos(Key, In_);
    while P > 0 do
    begin
      Q := P + Length(Key);
      while (Q <= Length(In_)) and (In_[Q] in ['0'..'9']) do
        Inc(Q);
      if (Q > P + Length(Key)) and (Q <= Length(In_)) and (In_[Q] = '"') then
      begin
        V := StrToInt64(Copy(In_, P + Length(Key), Q - P - Length(Key)));
        Result := Result + Copy(In_, From, P + Length(Key) - From) + IntToStr(Max(Int64(1), Round(V * F)));
        From := Q;
      end;
      P := PosEx(Key, In_, Q);
    end;
    Result := Result + Copy(In_, From, MaxInt);
  end;

begin
  Result := Scale(Scale(S, ' dur="'), '<p:cond delay="');
end;

{ S's time nodes numbered from Id on, and what names one of them (<p:tn val="N"/>) with them }
function Renumber(const S: string; var Id: Integer): string;
const
  Key = '<p:cTn id="';
  Ref = '<p:tn val="';
var
  Old, New_: array of Integer;
  P, Q, From, N, K, V: Integer;
begin
  Result := '';
  Old := nil;
  New_ := nil;
  From := 1;
  N := 0;
  P := Pos(Key, S);
  while P > 0 do
  begin
    Q := PosEx('"', S, P + Length(Key));
    if Q = 0 then
      Break;
    SetLength(Old, N + 1);
    SetLength(New_, N + 1);
    Old[N] := StrToIntDef(Copy(S, P + Length(Key), Q - P - Length(Key)), -1);
    New_[N] := Id;
    Result := Result + Copy(S, From, P + Length(Key) - From) + IntToStr(Id);
    Inc(Id);
    Inc(N);
    From := Q;
    P := PosEx(Key, S, Q);
  end;
  Result := Result + Copy(S, From, MaxInt);
  if N = 0 then
    Exit;
  P := Pos(Ref, Result);   { references among them }
  while P > 0 do
  begin
    Q := PosEx('"', Result, P + Length(Ref));
    if Q = 0 then
      Break;
    V := StrToIntDef(Copy(Result, P + Length(Ref), Q - P - Length(Ref)), -1);
    for K := 0 to N - 1 do
      if Old[K] = V then
      begin
        Result := Copy(Result, 1, P + Length(Ref) - 1) + IntToStr(New_[K]) + Copy(Result, Q, MaxInt);
        Q := P + Length(Ref) + Length(IntToStr(New_[K]));
        Break;
      end;
    P := PosEx(Ref, Result, Q);
  end;
end;

{ ---- presets ---- }

function AnimPresets: specialize TArray<TAnimPreset>;

  procedure Add(const C: string; P, S: Integer; const N: string);
  begin
    SetLength(Result, Length(Result) + 1);
    Result[High(Result)].Cls := C;
    Result[High(Result)].Preset := P;
    Result[High(Result)].Subtype := S;
    Result[High(Result)].Name := N;
  end;

begin
  Result := nil;
  Add('entr', 1, 0, 'Appear');
  Add('entr', 10, 0, 'Fade');
  Add('entr', 2, 4, 'Fly In');
  Add('entr', 22, 8, 'Wipe');
  Add('entr', 53, 16, 'Zoom');
  Add('emph', 8, 0, 'Spin');
  Add('emph', 6, 0, 'Grow/Shrink');
  Add('emph', 32, 0, 'Teeter');
  Add('exit', 1, 0, 'Disappear');
  Add('exit', 10, 0, 'Fade');
  Add('exit', 2, 4, 'Fly Out');
  Add('exit', 22, 8, 'Wipe');
  Add('exit', 53, 16, 'Zoom');
end;

function AnimPresetName(const Cls: string; Preset: Integer): string;
begin
  Result := '';
  if Cls = 'entr' then
    case Preset of
      1: Result := 'Appear';
      2: Result := 'Fly In';
      3: Result := 'Blinds';
      4: Result := 'Box';
      5: Result := 'Checkerboard';
      6: Result := 'Circle';
      8: Result := 'Diamond';
      9: Result := 'Dissolve In';
      10: Result := 'Fade';
      12: Result := 'Peek In';
      13: Result := 'Plus';
      14: Result := 'Random Bars';
      16: Result := 'Split';
      18: Result := 'Strips';
      20: Result := 'Wedge';
      21: Result := 'Wheel';
      22: Result := 'Wipe';
      53: Result := 'Zoom';
    end
  else if Cls = 'exit' then
    case Preset of
      1: Result := 'Disappear';
      2: Result := 'Fly Out';
      10: Result := 'Fade';
      22: Result := 'Wipe';
      53: Result := 'Zoom';
    end
  else if Cls = 'emph' then
    case Preset of
      6: Result := 'Grow/Shrink';
      8: Result := 'Spin';
      9: Result := 'Transparency';
      32: Result := 'Teeter';
    end
  else if Cls = 'path' then
    Result := 'Motion Path';
  if Result = '' then
    if Cls = 'entr' then
      Result := 'Entrance ' + IntToStr(Preset)
    else if Cls = 'exit' then
      Result := 'Exit ' + IntToStr(Preset)
    else if Cls = 'emph' then
      Result := 'Emphasis ' + IntToStr(Preset)
    else
      Result := 'Effect ' + IntToStr(Preset);
end;

function AnimSubtypeName(Subtype: Integer): string;
begin
  case Subtype of
    1: Result := 'From Top';
    2: Result := 'From Right';
    4: Result := 'From Bottom';
    8: Result := 'From Left';
  else
    Result := '';
  end;
end;

function AnimSubtypes(const Cls: string; Preset: Integer): string;
begin
  if ((Cls = 'entr') or (Cls = 'exit')) and ((Preset = 2) or (Preset = 22)) then
    Result := '4,8,2,1'
  else
    Result := '';
end;

function AnimTransKinds: string;
begin
  Result := 'fade,push,wipe,split,cover,pull,zoom,circle,diamond,plus,wedge,wheel,randomBar,blinds,checker,comb,' +
    'strips,dissolve,newsflash,cut,random';
end;

function AnimTransName(const Kind: string): string;
begin
  case Kind of
    '': Result := 'None';
    'fade': Result := 'Fade';
    'push': Result := 'Push';
    'wipe': Result := 'Wipe';
    'split': Result := 'Split';
    'cover': Result := 'Cover';
    'pull': Result := 'Uncover';
    'zoom': Result := 'Zoom';
    'circle': Result := 'Circle';
    'diamond': Result := 'Diamond';
    'plus': Result := 'Plus';
    'wedge': Result := 'Wedge';
    'wheel': Result := 'Wheel';
    'randomBar': Result := 'Random Bars';
    'blinds': Result := 'Blinds';
    'checker': Result := 'Checkerboard';
    'comb': Result := 'Comb';
    'strips': Result := 'Strips';
    'dissolve': Result := 'Dissolve';
    'newsflash': Result := 'Newsflash';
    'cut': Result := 'Cut';
    'random': Result := 'Random';
  else
    Result := Kind + ' (as read)';
  end;
end;

function AnimTransDirs(const Kind: string): string;
begin
  case Kind of
    'push', 'wipe': Result := 'l,r,u,d';
    'cover', 'pull': Result := 'l,r,u,d,lu,ru,ld,rd';
    'strips': Result := 'lu,ru,ld,rd';
    'split': Result := 'horz out,horz in,vert out,vert in';
    'zoom': Result := 'in,out';
    'randomBar', 'blinds', 'checker', 'comb': Result := 'horz,vert';
  else
    Result := '';
  end;
end;

function AnimDirName(const Dir: string): string;
begin
  case Dir of
    'l': Result := 'From Right';     { PowerPoint's dir is where it goes: l, the new slide from the right }
    'r': Result := 'From Left';
    'u': Result := 'From Bottom';
    'd': Result := 'From Top';
    'lu': Result := 'From Bottom-Right';
    'ru': Result := 'From Bottom-Left';
    'ld': Result := 'From Top-Right';
    'rd': Result := 'From Top-Left';
    'horz': Result := 'Horizontal';
    'vert': Result := 'Vertical';
    'in': Result := 'In';
    'out': Result := 'Out';
    'horz out': Result := 'Horizontal Out';
    'horz in': Result := 'Horizontal In';
    'vert out': Result := 'Vertical Out';
    'vert in': Result := 'Vertical In';
  else
    Result := Dir;
  end;
end;

function AnimMakeBehs(const Cls: string; Preset, Subtype: Integer; Shape: Int32; ParaSt, ParaEnd, Dur: Integer): string;
var
  Tgt, D: string;
  Exit_: Boolean;

  function Bhvr(const CTnAttrs, Attr: string): string;
  begin
    Result := '<p:cBhvr><p:cTn id="0"' + CTnAttrs + '/>' + Tgt;
    if Attr <> '' then
      Result := Result + '<p:attrNameLst><p:attrName>' + Attr + '</p:attrName></p:attrNameLst>';
    Result := Result + '</p:cBhvr>';
  end;

  { a turn by By (60000ths of a degree), At ms into the effect for Len ms }
  function Rot(By, At, Len: Integer): string;
  begin
    Result := '<p:animRot by="' + IntToStr(By) + '"><p:cBhvr><p:cTn id="0" dur="' + IntToStr(Max(1, Len)) +
      '" fill="hold"><p:stCondLst><p:cond delay="' + IntToStr(At) + '"/></p:stCondLst></p:cTn>' + Tgt +
      '<p:attrNameLst><p:attrName>r</p:attrName></p:attrNameLst></p:cBhvr></p:animRot>';
  end;

  function SetVis(const Val: string; At: Integer): string;
  begin
    Result := '<p:set><p:cBhvr><p:cTn id="0" dur="1" fill="hold"><p:stCondLst><p:cond delay="' + IntToStr(At) +
      '"/></p:stCondLst></p:cTn>' + Tgt + '<p:attrNameLst><p:attrName>style.visibility</p:attrName>' +
      '</p:attrNameLst></p:cBhvr><p:to><p:strVal val="' + Val + '"/></p:to></p:set>';
  end;

  function Anim(const Attr, V0, V1: string): string;
  begin
    Result := '<p:anim calcmode="lin" valueType="num">' + Bhvr(' dur="' + D + '" fill="hold"', Attr) +
      '<p:tavLst><p:tav tm="0"><p:val><p:strVal val="' + V0 + '"/></p:val></p:tav><p:tav tm="100000"><p:val>' +
      '<p:strVal val="' + V1 + '"/></p:val></p:tav></p:tavLst></p:anim>';
  end;

  function Filter(const F: string): string;
  begin
    Result := '<p:animEffect transition="' + IfThen(Exit_, 'out', 'in') + '" filter="' + F + '">' +
      Bhvr(' dur="' + D + '"', '') + '</p:animEffect>';
  end;

  { where a fly in comes from (a fly out goes to): x and y }
  procedure Off(out X, Y: string);
  begin
    X := '#ppt_x';
    Y := '#ppt_y';
    case Subtype of
      1: Y := '0-#ppt_h/2';
      2: X := '1+#ppt_w/2';
      8: X := '0-#ppt_w/2';
    else
      Y := '1+#ppt_h/2';
    end;
  end;

var
  X, Y, W: string;
begin
  Dur := Max(1, Dur);
  D := IntToStr(Dur);
  Exit_ := Cls = 'exit';
  if ParaSt >= 0 then
    Tgt := '<p:tgtEl><p:spTgt spid="' + IntToStr(Shape) + '"><p:txEl><p:pRg st="' + IntToStr(ParaSt) + '" end="' +
      IntToStr(Max(ParaSt, ParaEnd)) + '"/></p:txEl></p:spTgt></p:tgtEl>'
  else
    Tgt := '<p:tgtEl><p:spTgt spid="' + IntToStr(Shape) + '"/></p:tgtEl>';
  case Subtype of
    1: W := 'wipe(down)';
    2: W := 'wipe(left)';
    4: W := 'wipe(up)';
  else
    W := 'wipe(right)';
  end;
  Result := '';
  if Cls = 'emph' then
  begin
    if Preset = 6 then
      Result := '<p:animScale>' + Bhvr(' dur="' + D + '" fill="hold"', '') + '<p:by x="150000" y="150000"/></p:animScale>'
    else if Preset = 32 then
      { Teeter, as PowerPoint writes it: 2 degrees one way, 4 back, 4, 4 back, 2 -- where it was; its times a tenth
        and fifths of the effect's }
      Result := Rot(120000, 0, Dur div 10) + Rot(-240000, Dur div 5, Dur div 5) + Rot(240000, Dur * 2 div 5, Dur div 5) +
        Rot(-240000, Dur * 3 div 5, Dur div 5) + Rot(120000, Dur * 4 div 5, Dur div 5)
    else
      Result := '<p:animRot by="21600000">' + Bhvr(' dur="' + D + '" fill="hold"', 'r') + '</p:animRot>';
    Exit;
  end;
  if not Exit_ then
    Result := SetVis('visible', 0);
  Off(X, Y);
  case Preset of
    2:
      if Exit_ then
        Result := Result + Anim('ppt_x', '#ppt_x', X) + Anim('ppt_y', '#ppt_y', Y)
      else
        Result := Result + Anim('ppt_x', X, '#ppt_x') + Anim('ppt_y', Y, '#ppt_y');
    10: Result := Result + Filter('fade');
    22: Result := Result + Filter(W);
    53:
      if Exit_ then
        Result := Result + Anim('ppt_w', '#ppt_w', '0') + Anim('ppt_h', '#ppt_h', '0') + Filter('fade')
      else
        Result := Result + Anim('ppt_w', '0', '#ppt_w') + Anim('ppt_h', '0', '#ppt_h') + Filter('fade');
  end;
  if Exit_ then
    Result := Result + SetVis('hidden', IfThen(Preset = 1, 0, Dur - 1));
end;

{ ---- TParadeAnim ---- }

constructor TParadeAnim.Create;
begin
  inherited Create;
  FBld := TStringList.Create;
  Trans.Speed := 'fast';
  Trans.AdvClick := True;
  Trans.AdvTime := -1;
end;

destructor TParadeAnim.Destroy;
begin
  FBld.Free;
  inherited Destroy;
end;

function TParadeAnim.Load(const Xml: string): Boolean;
var
  Doc: TXMLDocument;
  St: TStringStream;
  Root, C, T, K, Main, Grp, Sub, Eff, ET, X: TDOMNode;
  I, NG, NS, EI: Integer;
  N, NT: string;
  E: TAnimEffect;

  { a group of the main sequence that starts by itself (with the slide, or after the one before) }
  function GroupAuto(G: TDOMNode): Boolean;
  var
    Cd: TDOMNode;
  begin
    Result := False;
    Cd := Kid(Kid(G, 'p:stCondLst'), 'p:cond');
    while Cd <> nil do
    begin
      if (Cd.NodeType = ELEMENT_NODE) and ((Att(Cd, 'evt') <> '') or (Att(Cd, 'delay') <> 'indefinite')) then
        Exit(True);
      Cd := Cd.NextSibling;
    end;
  end;

  procedure ReadTrans(Tr: TDOMNode);
  var
    Kc: TDOMNode;
    J: Integer;
    An: string;
  begin
    Trans.Speed := Att(Tr, 'spd');
    if Trans.Speed = '' then
      Trans.Speed := 'fast';
    Trans.AdvClick := Att(Tr, 'advClick') <> '0';
    Trans.AdvTime := IntAtt(Tr, 'advTm', -1);
    Kc := Tr.FirstChild;
    while (Kc <> nil) and ((Kc.NodeType <> ELEMENT_NODE) or (U8(Kc.NodeName) = 'p:sndAc') or
      (U8(Kc.NodeName) = 'p:extLst')) do
      Kc := Kc.NextSibling;
    if Kc = nil then
      Exit;
    Trans.Kind := U8(Kc.NodeName);
    if AnsiStartsStr('p:', Trans.Kind) then
      Trans.Kind := Copy(Trans.Kind, 3, MaxInt);
    Trans.Dir := Att(Kc, 'dir');
    if Trans.Kind = 'split' then
      Trans.Dir := IfThen(Att(Kc, 'orient') = 'vert', 'vert', 'horz') + ' ' + IfThen(Trans.Dir = 'in', 'in', 'out');
    if Kc.Attributes <> nil then
      for J := 0 to Kc.Attributes.Length - 1 do
      begin
        An := U8(Kc.Attributes[J].NodeName);
        if (An <> 'dir') and not ((Trans.Kind = 'split') and (An = 'orient')) then
          Trans.KindAttrs := Trans.KindAttrs + ' ' + An + '="' + Esc(U8(Kc.Attributes[J].NodeValue)) + '"';
      end;
  end;

begin
  Result := False;
  Effects := nil;
  FBld.Clear;
  FNs := '';
  FAfter := '';
  FOthers := '';
  Trans.Kind := '';
  Trans.KindAttrs := '';
  Trans.Dir := '';
  Trans.Speed := 'fast';
  Trans.AdvClick := True;
  Trans.AdvTime := -1;
  Trans.Raw := '';
  Trans.Changed := False;
  if Trim(Xml) = '' then
    Exit(True);
  Doc := nil;
  St := TStringStream.Create(Xml);
  try
    try
      ReadXMLFile(Doc, St);
    except
      Exit;
    end;
    Root := Doc.DocumentElement;
    if (Root = nil) or (U8(Root.NodeName) <> 'pd:tail') then
      Exit;
    if Root.Attributes <> nil then
      for I := 0 to Root.Attributes.Length - 1 do
        FNs := FNs + ' ' + U8(Root.Attributes[I].NodeName) + '="' + Esc(U8(Root.Attributes[I].NodeValue)) + '"';
    C := Root.FirstChild;
    while C <> nil do
    begin
      N := U8(C.NodeName);
      if C.NodeType <> ELEMENT_NODE then
      else if N = 'p:transition' then
      begin
        Trans.Raw := NodeXml(C);
        ReadTrans(C);
      end
      else if (N = 'mc:AlternateContent') and (Find(C, 'p:transition') <> nil) then
      begin
        Trans.Raw := NodeXml(C);
        T := Kid(Kid(C, 'mc:Fallback'), 'p:transition');     { the plain one, if it has it }
        if T = nil then
          T := Find(C, 'p:transition');
        ReadTrans(T);
      end
      else if N = 'p:timing' then
      begin
        Main := nil;
        T := Kid(Kid(Kid(Kid(C, 'p:tnLst'), 'p:par'), 'p:cTn'), 'p:childTnLst');
        K := T.FirstChild;
        while K <> nil do
        begin
          if (K.NodeType = ELEMENT_NODE) and (Main = nil) and (U8(K.NodeName) = 'p:seq') and
             (Att(Kid(K, 'p:cTn'), 'nodeType') = 'mainSeq') then
            Main := K
          else if K.NodeType = ELEMENT_NODE then
            FOthers := FOthers + NodeXml(K);
          K := K.NextSibling;
        end;
        NG := 0;
        Grp := Kid(Kid(Kid(Main, 'p:cTn'), 'p:childTnLst'), 'p:par');
        while Grp <> nil do
        begin
          if (Grp.NodeType = ELEMENT_NODE) and (U8(Grp.NodeName) = 'p:par') then
          begin
            NS := 0;
            Sub := Kid(Kid(Kid(Grp, 'p:cTn'), 'p:childTnLst'), 'p:par');
            while Sub <> nil do
            begin
              if (Sub.NodeType = ELEMENT_NODE) and (U8(Sub.NodeName) = 'p:par') then
              begin
                Eff := Kid(Kid(Kid(Sub, 'p:cTn'), 'p:childTnLst'), 'p:par');
                EI := 0;
                while Eff <> nil do
                begin
                  ET := Kid(Eff, 'p:cTn');
                  if (Eff.NodeType = ELEMENT_NODE) and (U8(Eff.NodeName) = 'p:par') and HasAtt(ET, 'presetClass') then
                  begin
                    FillChar(E, SizeOf(E), 0);
                    E.Cls := Att(ET, 'presetClass');
                    E.Preset := IntAtt(ET, 'presetID', 0);
                    E.Subtype := IntAtt(ET, 'presetSubtype', 0);
                    E.GrpId := Att(ET, 'grpId');
                    NT := Att(ET, 'nodeType');
                    if NT = 'clickEffect' then
                      E.Start := asClick
                    else if NT = 'withEffect' then
                      E.Start := asWith
                    else if NT = 'afterEffect' then
                      E.Start := asAfter
                    else if (NS = 0) and (EI = 0) and not GroupAuto(Kid(Grp, 'p:cTn')) then
                      E.Start := asClick
                    else if (NS > 0) and (EI = 0) then
                      E.Start := asAfter
                    else
                      E.Start := asWith;
                    E.Delay := Max(0, CondDelay(ET));
                    if ET.Attributes <> nil then
                      for I := 0 to ET.Attributes.Length - 1 do
                      begin
                        N := U8(ET.Attributes[I].NodeName);
                        if (N <> 'id') and (N <> 'presetID') and (N <> 'presetClass') and (N <> 'presetSubtype') and
                           (N <> 'fill') and (N <> 'grpId') and (N <> 'nodeType') then
                          E.Attrs := E.Attrs + ' ' + N + '="' + Esc(U8(ET.Attributes[I].NodeValue)) + '"';
                      end;
                    X := ET.FirstChild;
                    while X <> nil do
                    begin
                      N := U8(X.NodeName);
                      if X.NodeType <> ELEMENT_NODE then
                      else if N = 'p:childTnLst' then
                      begin
                        K := X.FirstChild;
                        while K <> nil do
                        begin
                          E.Behs := E.Behs + NodeXml(K);
                          K := K.NextSibling;
                        end;
                        E.BehDur := BehEnd(X);
                      end
                      else if N = 'p:subTnLst' then
                        E.Post := E.Post + NodeXml(X)
                      else if N <> 'p:stCondLst' then
                        E.Pre := E.Pre + NodeXml(X);
                      X := X.NextSibling;
                    end;
                    E.Dur := E.BehDur;
                    K := Find(ET, 'p:spTgt');
                    E.Shape := IntAtt(K, 'spid', 0);
                    K := Find(K, 'p:pRg');
                    E.ParaSt := IntAtt(K, 'st', -1);
                    E.ParaEnd := IntAtt(K, 'end', E.ParaSt);
                    SetLength(Effects, Length(Effects) + 1);
                    Effects[High(Effects)] := E;
                    Inc(EI);
                  end;
                  Eff := Eff.NextSibling;
                end;
                Inc(NS);
              end;
              Sub := Sub.NextSibling;
            end;
            Inc(NG);
          end;
          Grp := Grp.NextSibling;
        end;
        K := Kid(C, 'p:bldLst');
        if K <> nil then
        begin
          K := K.FirstChild;
          while K <> nil do
          begin
            if K.NodeType = ELEMENT_NODE then
              FBld.AddObject(NodeXml(K), TObject(PtrInt(IntAtt(K, 'spid', 0))));
            K := K.NextSibling;
          end;
        end;
      end
      else
        FAfter := FAfter + NodeXml(C);
      C := C.NextSibling;
    end;
    Result := True;
  finally
    Doc.Free;
    St.Free;
  end;
end;

function TParadeAnim.ClickOf(I: Integer): Integer;
var
  K: Integer;
begin
  Result := 0;
  for K := 0 to Min(I, High(Effects)) do
    if Effects[K].Start = asClick then
      Inc(Result);
end;

function EffLen(const E: TAnimEffect): Integer;
var
  P: Integer;
  R: Double;
  S: string;
begin
  R := 1;
  P := Pos(' repeatCount="', E.Attrs);
  if P > 0 then
  begin
    S := Copy(E.Attrs, P + 14, MaxInt);
    R := Max(1, StrToIntDef(Copy(S, 1, Pos('"', S) - 1), 1000) / 1000);
  end;
  if Pos(' autoRev="1"', E.Attrs) > 0 then
    R := R * 2;
  Result := E.Delay + Round(Max(0, E.Dur) * R);
end;

function TParadeAnim.Save: string;
var
  Tm, Tr, Bld, Kd: string;
  I, J, G, Id, SubT, SubEnd: Integer;
  Used: Boolean;
  E: TAnimEffect;
  Behs: string;

  procedure CloseSub;
  begin
    Tm := Tm + '</p:childTnLst></p:cTn></p:par>';
  end;

  procedure OpenSub(T: Integer);
  begin
    Tm := Tm + '<p:par><p:cTn id="' + IntToStr(Id) + '" fill="hold"><p:stCondLst><p:cond delay="' + IntToStr(T) +
      '"/></p:stCondLst><p:childTnLst>';
    Inc(Id);
  end;

begin
  { the transition }
  if not Trans.Changed then
    Tr := Trans.Raw
  else if (Trans.Kind = '') and Trans.AdvClick and (Trans.AdvTime < 0) then
    Tr := ''
  else
  begin
    Tr := '<p:transition spd="' + IfThen(Trans.Speed = '', 'fast', Trans.Speed) + '"' +
      IfThen(not Trans.AdvClick, ' advClick="0"') + IfThen(Trans.AdvTime >= 0, ' advTm="' + IntToStr(Trans.AdvTime) + '"') + '>';
    if Trans.Kind <> '' then
    begin
      Kd := IfThen(Pos(':', Trans.Kind) > 0, Trans.Kind, 'p:' + Trans.Kind);
      Tr := Tr + '<' + Kd + Trans.KindAttrs;
      if Trans.Kind = 'split' then
        Tr := Tr + ' orient="' + IfThen(AnsiStartsStr('vert', Trans.Dir), 'vert', 'horz') + '" dir="' +
          IfThen(AnsiEndsStr('in', Trans.Dir), 'in', 'out') + '"'
      else if Trans.Dir <> '' then
        Tr := Tr + ' dir="' + Trans.Dir + '"';
      Tr := Tr + '/>';
    end;
    Tr := Tr + '</p:transition>';
    if Pos(':', Trans.Kind) > 0 then   { PowerPoint 2010's (p14:vortex): for those who know it, a fade for others }
      Tr := '<mc:AlternateContent' + IfThen(Pos('xmlns:mc=', FNs) = 0,
        ' xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"') + '><mc:Choice Requires="' +
        Copy(Trans.Kind, 1, Pos(':', Trans.Kind) - 1) + '">' + Tr + '</mc:Choice><mc:Fallback>' +
        StringReplace(Tr, '<' + Trans.Kind + Trans.KindAttrs + IfThen(Trans.Dir <> '', ' dir="' + Trans.Dir + '"') +
        '/>', '<p:fade/>', []) + '</mc:Fallback></mc:AlternateContent>';
  end;
  { the timing }
  Tm := '';
  if (Length(Effects) > 0) or (FOthers <> '') then
  begin
    Tm := '<p:timing><p:tnLst><p:par><p:cTn id="1" dur="indefinite" restart="never" nodeType="tmRoot"><p:childTnLst>';
    Id := 2;
    if Length(Effects) > 0 then
    begin
      Tm := Tm + '<p:seq concurrent="1" nextAc="seek"><p:cTn id="2" dur="indefinite" nodeType="mainSeq"><p:childTnLst>';
      Id := 3;
      SubT := 0;
      SubEnd := 0;
      G := 0;
      for I := 0 to High(Effects) do
      begin
        E := Effects[I];
        if (I = 0) or (E.Start = asClick) then
        begin
          if I > 0 then
          begin
            CloseSub;
            CloseSub;
          end;
          Tm := Tm + '<p:par><p:cTn id="' + IntToStr(Id) + '" fill="hold"><p:stCondLst><p:cond delay="indefinite"/>';
          if E.Start <> asClick then  { the first not on a click: on the slide's start }
            Tm := Tm + '<p:cond evt="onBegin" delay="0"><p:tn val="2"/></p:cond>';
          Tm := Tm + '</p:stCondLst><p:childTnLst>';
          Inc(Id);
          Inc(G);
          SubT := 0;
          SubEnd := 0;
          OpenSub(0);
        end
        else if E.Start = asAfter then
        begin
          CloseSub;
          SubT := SubT + SubEnd;
          SubEnd := 0;
          OpenSub(SubT);
        end;
        SubEnd := Max(SubEnd, EffLen(E));
        Tm := Tm + '<p:par><p:cTn id="' + IntToStr(Id) + '" presetID="' + IntToStr(E.Preset) + '" presetClass="' + E.Cls +
          '" presetSubtype="' + IntToStr(E.Subtype) + '"' + E.Attrs + ' fill="hold"' +
          IfThen(E.GrpId <> '', ' grpId="' + E.GrpId + '"') + ' nodeType="' +
          IfThen(E.Start = asClick, 'clickEffect', IfThen(E.Start = asWith, 'withEffect', 'afterEffect')) +
          '"><p:stCondLst><p:cond delay="' + IntToStr(E.Delay) + '"/></p:stCondLst>' + E.Pre + '<p:childTnLst>';
        Inc(Id);
        Behs := E.Behs;
        if (E.BehDur > 1) and (E.Dur > 0) and (E.Dur <> E.BehDur) then
          Behs := ScaleTimes(Behs, E.Dur / E.BehDur);
        Tm := Tm + Renumber(Behs, Id) + '</p:childTnLst>' + Renumber(E.Post, Id) + '</p:cTn></p:par>';
      end;
      CloseSub;
      CloseSub;
      Tm := Tm + '</p:childTnLst></p:cTn><p:prevCondLst><p:cond evt="onPrev" delay="0"><p:tgtEl><p:sldTgt/></p:tgtEl>' +
        '</p:cond></p:prevCondLst><p:nextCondLst><p:cond evt="onNext" delay="0"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond>' +
        '</p:nextCondLst></p:seq>';
    end;
    Tm := Tm + Renumber(FOthers, Id) + '</p:childTnLst></p:cTn></p:par></p:tnLst>';
    Bld := '';
    for I := 0 to FBld.Count - 1 do
    begin
      Used := False;
      for J := 0 to High(Effects) do
        Used := Used or (Effects[J].Shape = PtrInt(FBld.Objects[I]));
      if Used or ((FOthers <> '') and (Pos('spid="' + IntToStr(PtrInt(FBld.Objects[I])) + '"', FOthers) > 0)) then
        Bld := Bld + FBld[I];
    end;
    if Bld <> '' then
      Tm := Tm + '<p:bldLst>' + Bld + '</p:bldLst>';
    Tm := Tm + '</p:timing>';
  end;
  if (Tr = '') and (Tm = '') and (FAfter = '') then
    Exit('');
  Result := '<pd:tail' + IfThen(FNs = '', DefaultNs, FNs) + '>' + Tr + Tm + FAfter + '</pd:tail>';
end;

function TParadeAnim.Add(Shape: Int32; const Cls: string; Preset, Subtype: Integer; Start: TAnimStart; Dur: Integer;
  At: Integer): Integer;
var
  E: TAnimEffect;
  I: Integer;
begin
  FillChar(E, SizeOf(E), 0);
  E.Cls := Cls;
  E.Preset := Preset;
  E.Subtype := Subtype;
  E.Shape := Shape;
  E.ParaSt := -1;
  E.ParaEnd := -1;
  E.Start := Start;
  E.Dur := IfThen(Preset = 1, 1, Max(1, Dur));
  E.GrpId := '0';
  E.Behs := AnimMakeBehs(Cls, Preset, Subtype, Shape, -1, -1, E.Dur);
  E.BehDur := E.Dur;
  if (At < 0) or (At > Length(Effects)) then
    At := Length(Effects);
  SetLength(Effects, Length(Effects) + 1);
  for I := High(Effects) downto At + 1 do
    Effects[I] := Effects[I - 1];
  Effects[At] := E;
  Result := At;
end;

procedure TParadeAnim.Delete(I: Integer);
var
  K: Integer;
begin
  if (I < 0) or (I > High(Effects)) then
    Exit;
  for K := I to High(Effects) - 1 do
    Effects[K] := Effects[K + 1];
  SetLength(Effects, Length(Effects) - 1);
end;

procedure TParadeAnim.Move(I, ToI: Integer);
var
  E: TAnimEffect;
  K: Integer;
begin
  if (I < 0) or (I > High(Effects)) or (ToI < 0) or (ToI > High(Effects)) or (I = ToI) then
    Exit;
  E := Effects[I];
  if ToI > I then
    for K := I to ToI - 1 do
      Effects[K] := Effects[K + 1]
  else
    for K := I downto ToI + 1 do
      Effects[K] := Effects[K - 1];
  Effects[ToI] := E;
end;

procedure TParadeAnim.SetPreset(I: Integer; const Cls: string; Preset, Subtype: Integer);
var
  E: TAnimEffect;
begin
  if (I < 0) or (I > High(Effects)) then
    Exit;
  E := Effects[I];
  if E.Dur <= 1 then
    E.Dur := 500;
  if Preset = 1 then
    E.Dur := 1;
  if (Cls = 'emph') and (Preset = 32) and ((E.Cls <> Cls) or (E.Preset <> Preset)) then
    E.Dur := 1000;    { (a Teeter's second, as PowerPoint's) }
  E.Cls := Cls;
  E.Preset := Preset;
  E.Subtype := Subtype;
  E.Behs := AnimMakeBehs(Cls, Preset, Subtype, E.Shape, E.ParaSt, E.ParaEnd, E.Dur);
  E.BehDur := E.Dur;
  E.Pre := '';
  E.Post := '';
  Effects[I] := E;
end;

procedure TParadeAnim.SetTransition(const Kind, Dir, Speed: string; AdvClick: Boolean; AdvTime: Integer);
begin
  if Kind <> Trans.Kind then
    Trans.KindAttrs := '';
  Trans.Kind := Kind;
  Trans.Dir := Dir;
  Trans.Speed := Speed;
  Trans.AdvClick := AdvClick;
  Trans.AdvTime := AdvTime;
  Trans.Changed := True;
end;

end.
