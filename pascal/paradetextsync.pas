{ A plain text shared through the relay (paradesync's session, pd_tsync's
  CRDT): for an editor of lines -- a code editor, a memo.

  TParadeTextTarget does the work: it keeps a copy of the editor's lines,
  and when told the editor changed (Changed, from the editor's own change
  notification) it finds what differs on the next tick -- lines the editor
  did not touch share their strings with the copy, so comparing them costs
  nothing -- and sends that. What the others did comes back as one range
  replaced, mapped from the shared text's positions (UTF-16 code units) to
  lines and byte columns. A subclass binds a real editor: its lines, a
  range replaced without its own undo, the caret, and the others' carets.

    S := TParadeSync.CreateFor(Owner, TMyEditorTarget.Create(MyEditor));
    S.Start(Server, Doc, Token, Name, Publish);

  Undo (with the session on) is Undo(False): this side's own edits only. }

unit paradetextsync;

{$mode objfpc}{$H+}
{$PACKRECORDS C}

interface

uses
  Classes, SysUtils, Math, ctypes, parade, paradesync;

type
  Ppd_tsync = Pointer;
  pd_tsync_send_fn = procedure(user: Pointer; update: Pointer; len: csize_t); cdecl;
  pd_tsync_change_fn = procedure(user: Pointer; at, removed: UInt32; inserted: PAnsiChar; len: csize_t); cdecl;

function pd_tsync_new(client: UInt64; out t: Ppd_tsync): pd_status; cdecl; external;
procedure pd_tsync_free(t: Ppd_tsync); cdecl; external;
procedure pd_tsync_set_sender(t: Ppd_tsync; fn: pd_tsync_send_fn; user: Pointer); cdecl; external;
procedure pd_tsync_set_listener(t: Ppd_tsync; fn: pd_tsync_change_fn; user: Pointer); cdecl; external;
function pd_tsync_publish(t: Ppd_tsync; utf8: PAnsiChar; len: csize_t): pd_status; cdecl; external;
function pd_tsync_receive(t: Ppd_tsync; update: Pointer; len: csize_t): pd_status; cdecl; external;
function pd_tsync_insert(t: Ppd_tsync; at: UInt32; utf8: PAnsiChar; len: csize_t): pd_status; cdecl; external;
function pd_tsync_delete(t: Ppd_tsync; at, count: UInt32): pd_status; cdecl; external;
function pd_tsync_undo(t: Ppd_tsync): pd_status; cdecl; external;
function pd_tsync_redo(t: Ppd_tsync): pd_status; cdecl; external;
function pd_tsync_can_undo(t: Ppd_tsync): Int32; cdecl; external;
function pd_tsync_can_redo(t: Ppd_tsync): Int32; cdecl; external;
procedure pd_tsync_seal(t: Ppd_tsync); cdecl; external;
procedure pd_tsync_clear_undo(t: Ppd_tsync); cdecl; external;
function pd_tsync_text(t: Ppd_tsync): PAnsiChar; cdecl; external;
function pd_tsync_length(t: Ppd_tsync): UInt32; cdecl; external;
procedure pd_tsync_free_data(data: Pointer); cdecl; external;
function pd_tsync_units(utf8: PAnsiChar; len: csize_t): UInt32; cdecl; external;

type
  { another's caret in the editor's terms: lines from 0, byte columns from 0 }
  TParadeTextCaret = record
    Line, Col, AnchorLine, AnchorCol: Integer;
    Color: UInt32;
    Name: string;
  end;

  TParadeTextTarget = class(TParadeSyncTarget)
  private
    FSync: Ppd_tsync;
    FShadow: TStringList;       { the editor's lines as the shared text last had them }
    FStart: array of UInt32;    { the code unit each shadow line starts at, valid below FStartValid }
    FStartValid: Integer;
    FDirty: Boolean;
    FFrom: Integer;             { the first line the editor may have changed since the last look }
    FApplying: Integer;         { applying the others' change: the editor's notification is ours }
    FQuiet: Boolean;            { catching up on joining: the whole text is set once, at the end }
    procedure FindLocal;
    procedure Apply(At, Removed: UInt32; const Inserted: string);
    function LineStart(L: Integer): UInt32;
    procedure Locate(U: UInt32; out L, Col: Integer);
    function PosOf(L, Col: Integer): UInt32;
    procedure Reshadow;
  protected
    { the editor: how many lines it has (at least one, maybe empty), and each }
    function LineCount: Integer; virtual; abstract;
    function GetLine(I: Integer): string; virtual; abstract;
    { its whole text replaced (a joiner's, once caught up) }
    procedure SetText(const S: string); virtual; abstract;
    { the text from (Line1, Col1) to (Line2, Col2) replaced by S (which may hold #10), not as an edit to
      undo; lines from 0, byte columns from 0 }
    procedure ReplaceRange(Line1, Col1, Line2, Col2: Integer; const S: string); virtual; abstract;
    { where its caret is, and the selection's other end }
    procedure GetCaret(out Line, Col, AnchorLine, AnchorCol: Integer); virtual; abstract;
    procedure ShowCarets(const Carets: array of TParadeTextCaret); virtual; abstract;
    procedure SetEditorReadOnly(AValue: Boolean); virtual; abstract;
  public
    constructor Create;
    destructor Destroy; override;
    { the editor's text changed, from FromLine on (0 when not known): what changed is sent on the next tick }
    procedure Changed(FromLine: Integer = 0);
    { while it is True the editor's change notifications are this target's own (the others' edits) }
    function Applying: Boolean;
    { this side's own edits undone (or done again, Redo); False when there is none }
    function Undo(Redo: Boolean = False): Boolean;
    function CanUndo(Redo: Boolean = False): Boolean;
    { the undo step under way ends (the caret moved elsewhere) }
    procedure Seal;
    { the shared text now (for tests) }
    function SharedText: string;
    function Open(Publish: Boolean; out Why: string): Boolean; override;
    procedure Close; override;
    procedure Receive(Update: Pointer; Len: csize_t); override;
    procedure Joined; override;
    function Caret(out Key: string; out Offset: UInt32; out AnchorKey: string; out AnchorOffset: UInt32): Boolean;
      override;
    procedure ShowPeers(const APeers: array of TParadePeer); override;
    procedure SetReadOnly(AValue: Boolean); override;
    function Dump(FromShared: Boolean): string; override;
    procedure Poll; override;
  end;

  { a text in a TStrings, with no editor: for tests, and for a program sharing a text it edits itself }
  TParadeStringsTarget = class(TParadeTextTarget)
  private
    FLines: TStrings;
    FCaretLine, FCaretCol: Integer;
    FReadOnly: Boolean;
    FCarets: array of TParadeTextCaret;
  protected
    function LineCount: Integer; override;
    function GetLine(I: Integer): string; override;
    procedure SetText(const S: string); override;
    procedure ReplaceRange(Line1, Col1, Line2, Col2: Integer; const S: string); override;
    procedure GetCaret(out Line, Col, AnchorLine, AnchorCol: Integer); override;
    procedure ShowCarets(const Carets: array of TParadeTextCaret); override;
    procedure SetEditorReadOnly(AValue: Boolean); override;
  public
    { the lines are the caller's; the target changes them with the others' edits }
    constructor Create(ALines: TStrings);
    { an edit made through the target (an editor would make it and call Changed) }
    procedure Edit(Line1, Col1, Line2, Col2: Integer; const S: string);
    procedure SetCaretAt(Line, Col: Integer);
    function PeerCarets: Integer;
    function PeerCaret(I: Integer): TParadeTextCaret;
    property ReadOnly: Boolean read FReadOnly;
    property Lines: TStrings read FLines;
  end;

{ what the relay's document is, read from what it holds: 'text' (a plain text with something in it),
  'rich' (updates, none of them a plain text's), '' (nothing yet, or the relay could not be read) }
function ParadeRelayKind(const Server, Doc, Token: string): string;

{ a text's lines, #10 between them }
function ParadeJoinLines(L: TStrings; From, Upto: Integer): string;
{ a text cut at its #10s into L (cleared first; one empty line for an empty text) -- by hand: DelimitedText
  would read quotes }
procedure ParadeSplitLines(const S: string; L: TStrings);

implementation

uses
  fphttpclient;

function Units(const S: string): UInt32; inline;
begin
  if S = '' then
    Result := 0
  else
    Result := pd_tsync_units(PAnsiChar(S), Length(S));
end;

function ParadeJoinLines(L: TStrings; From, Upto: Integer): string;
var
  I: Integer;
begin
  Result := '';
  for I := From to Upto - 1 do
  begin
    if I > From then
      Result := Result + #10;
    Result := Result + L[I];
  end;
end;

procedure ParadeSplitLines(const S: string; L: TStrings);
var
  I, From: Integer;
begin
  L.BeginUpdate;
  try
    L.Clear;
    From := 1;
    for I := 1 to Length(S) do
      if S[I] = #10 then
      begin
        L.Add(Copy(S, From, I - From));
        From := I + 1;
      end;
    L.Add(Copy(S, From, MaxInt));
  finally
    L.EndUpdate;
  end;
end;

{ the byte column a run of code units reaches in a line (a position inside a character goes past it) }
function ColOfUnits(const S: string; U: UInt32): Integer;
var
  I: Integer;
  C: Byte;
  N: UInt32;
begin
  I := 1;
  N := 0;
  while (I <= Length(S)) and (N < U) do
  begin
    C := Byte(S[I]);
    if C >= $F0 then
    begin
      Inc(N, 2);
      Inc(I, 4);
    end
    else if C >= $E0 then
    begin
      Inc(N);
      Inc(I, 3);
    end
    else if C >= $C0 then
    begin
      Inc(N);
      Inc(I, 2);
    end
    else
    begin
      Inc(N);
      Inc(I);
    end;
  end;
  if I > Length(S) + 1 then
    I := Length(S) + 1;
  Result := I - 1;
end;

procedure OnTextSend(user: Pointer; update: Pointer; len: csize_t); cdecl;
begin
  TParadeSyncTarget(user).Emit(update, len);
end;

procedure OnTextChange(user: Pointer; at, removed: UInt32; inserted: PAnsiChar; len: csize_t); cdecl;
var
  S: string;
begin
  S := '';
  SetString(S, inserted, len);
  TParadeTextTarget(user).Apply(at, removed, S);
end;

{ TParadeTextTarget }

constructor TParadeTextTarget.Create;
begin
  inherited Create;
  FShadow := TStringList.Create;
end;

destructor TParadeTextTarget.Destroy;
begin
  Close;
  FShadow.Free;
  inherited Destroy;
end;

procedure TParadeTextTarget.Reshadow;
var
  I: Integer;
begin
  FShadow.Clear;
  for I := 0 to LineCount - 1 do
    FShadow.Add(GetLine(I));
  if FShadow.Count = 0 then
    FShadow.Add('');
  FStartValid := 0;
  FDirty := False;
end;

function TParadeTextTarget.LineStart(L: Integer): UInt32;
var
  I: Integer;
begin
  if Length(FStart) < FShadow.Count + 1 then
    SetLength(FStart, FShadow.Count + 1);
  if FStartValid = 0 then
  begin
    FStart[0] := 0;
    FStartValid := 1;
  end;
  for I := FStartValid to L do
    FStart[I] := FStart[I - 1] + Units(FShadow[I - 1]) + 1;
  if L >= FStartValid then
    FStartValid := L + 1;
  Result := FStart[L];
end;

procedure TParadeTextTarget.Locate(U: UInt32; out L, Col: Integer);
var
  Lo, Hi, M: Integer;
begin
  LineStart(FShadow.Count - 1);
  Lo := 0;
  Hi := FShadow.Count - 1;
  while Lo < Hi do
  begin   { the last line starting at or before U }
    M := (Lo + Hi + 1) div 2;
    if FStart[M] <= U then
      Lo := M
    else
      Hi := M - 1;
  end;
  L := Lo;
  Col := ColOfUnits(FShadow[L], U - FStart[L]);
end;

function TParadeTextTarget.PosOf(L, Col: Integer): UInt32;
begin
  if L < 0 then
    L := 0;
  if L >= FShadow.Count then
    L := FShadow.Count - 1;
  Result := LineStart(L) + Units(Copy(FShadow[L], 1, Col));
end;

procedure TParadeTextTarget.Changed(FromLine: Integer);
begin
  if FApplying > 0 then
    Exit;
  if not FDirty or (FromLine < FFrom) then
    FFrom := FromLine;
  if FFrom < 0 then
    FFrom := 0;
  FDirty := True;
end;

function TParadeTextTarget.Applying: Boolean;
begin
  Result := FApplying > 0;
end;

{ what the editor changed since the last look, sent: the lines that differ found from both ends, then the
  characters that differ inside them, as one range replaced }
procedure TParadeTextTarget.FindLocal;
var
  NOld, NNew, I, J, A, BOld, BNew, P, Q, K: Integer;
  Old, New, Ins: string;
  At, Removed: UInt32;
  Cur: TStringList;

  function NewLine(K: Integer): string;
  begin
    if LineCount = 0 then
      Result := ''
    else
      Result := GetLine(K);
  end;

  function IsCont(C: Char): Boolean; inline;
  begin
    Result := (Byte(C) and $C0) = $80;
  end;

begin
  FDirty := False;
  if FSync = nil then
    Exit;
  NOld := FShadow.Count;
  NNew := LineCount;
  if NNew = 0 then
    NNew := 1;
  I := Min(FFrom, Min(NOld, NNew));
  while (I < NOld) and (I < NNew) and (FShadow[I] = NewLine(I)) do
    Inc(I);
  if (I = NOld) and (I = NNew) then
    Exit;     { nothing changed after all }
  J := 0;
  while (J < NOld - I) and (J < NNew - I) and (FShadow[NOld - 1 - J] = NewLine(NNew - 1 - J)) do
    Inc(J);
  { the block: from the line before the first changed one to the line after the last, where there are
    such; the same start on both sides, and the same lines after it }
  A := Max(0, I - 1);
  BOld := Min(NOld, NOld - J + 1);
  BNew := Min(NNew, NNew - J + 1);
  Old := ParadeJoinLines(FShadow, A, BOld);
  Cur := TStringList.Create;
  try
    for K := A to BNew - 1 do
      Cur.Add(NewLine(K));
    New := ParadeJoinLines(Cur, 0, Cur.Count);
    { the characters that differ }
    P := 0;
    while (P < Length(Old)) and (P < Length(New)) and (Old[P + 1] = New[P + 1]) do
      Inc(P);
    while (P > 0) and (((P < Length(Old)) and IsCont(Old[P + 1])) or ((P < Length(New)) and IsCont(New[P + 1]))) do
      Dec(P);
    Q := 0;
    while (Q < Length(Old) - P) and (Q < Length(New) - P) and (Old[Length(Old) - Q] = New[Length(New) - Q]) do
      Inc(Q);
    while (Q > 0) and (IsCont(Old[Length(Old) - Q + 1]) or IsCont(New[Length(New) - Q + 1])) do
      Dec(Q);
    At := LineStart(A) + Units(Copy(Old, 1, P));
    Removed := Units(Copy(Old, P + 1, Length(Old) - Q - P));
    Ins := Copy(New, P + 1, Length(New) - Q - P);
    if Removed > 0 then
      pd_tsync_delete(FSync, At, Removed);
    if Ins <> '' then
      pd_tsync_insert(FSync, At, PAnsiChar(Ins), Length(Ins));
    { the copy as the editor has it }
    for K := A to BOld - 1 do
      FShadow.Delete(A);
    for K := Cur.Count - 1 downto 0 do
      FShadow.Insert(A, Cur[K]);
    FStartValid := Min(FStartValid, A + 1);
  finally
    Cur.Free;
  end;
end;

{ the others' change, at code units of the text as the editor has it: into the editor and the copy }
procedure TParadeTextTarget.Apply(At, Removed: UInt32; const Inserted: string);
var
  L1, C1, L2, C2, K: Integer;
  S: string;
  Parts: TStringList;
begin
  if FQuiet then
    Exit;     { catching up: the whole text comes at the end }
  if FDirty then
    FindLocal;
  Locate(At, L1, C1);
  Locate(At + Removed, L2, C2);
  Inc(FApplying);
  try
    ReplaceRange(L1, C1, L2, C2, Inserted);
  finally
    Dec(FApplying);
  end;
  { the copy: the lines L1..L2 made again round what went in }
  S := Copy(FShadow[L1], 1, C1) + Inserted + Copy(FShadow[L2], C2 + 1, MaxInt);
  Parts := TStringList.Create;
  try
    ParadeSplitLines(S, Parts);
    for K := L1 to L2 do
      FShadow.Delete(L1);
    for K := Parts.Count - 1 downto 0 do
      FShadow.Insert(L1, Parts[K]);
  finally
    Parts.Free;
  end;
  FStartValid := Min(FStartValid, L1 + 1);
  { lines the editor would have built differently (it trims, say): found on the next look and sent }
  for K := L1 to Min(L1 + 1, FShadow.Count - 1) do
    if (K < LineCount) and (GetLine(K) <> FShadow[K]) then
      Changed(L1);
end;

function TParadeTextTarget.Open(Publish: Boolean; out Why: string): Boolean;
var
  S: string;
begin
  Why := '';
  if pd_tsync_new(0, FSync) <> PD_OK then
  begin
    Why := 'pd_tsync_new failed';
    FSync := nil;
    Exit(False);
  end;
  pd_tsync_set_sender(FSync, @OnTextSend, Self);
  pd_tsync_set_listener(FSync, @OnTextChange, Self);
  if Publish then
  begin
    Reshadow;
    S := ParadeJoinLines(FShadow, 0, FShadow.Count);
    pd_tsync_publish(FSync, PAnsiChar(S), Length(S));
    FQuiet := False;
  end
  else
    FQuiet := True;     { what the relay has comes first; the editor gets it whole once caught up }
  Result := True;
end;

procedure TParadeTextTarget.Joined;
var
  P: PAnsiChar;
  S: string;
begin
  if FSync = nil then
    Exit;
  P := pd_tsync_text(FSync);
  S := P;
  pd_tsync_free_data(P);
  Inc(FApplying);
  try
    SetText(S);
  finally
    Dec(FApplying);
  end;
  Reshadow;
  pd_tsync_clear_undo(FSync);    { what the others wrote, and what an earlier session left, is not to undo }
  FQuiet := False;
end;

procedure TParadeTextTarget.Close;
begin
  if FSync <> nil then
  begin
    pd_tsync_free(FSync);
    FSync := nil;
  end;
  FDirty := False;
  FQuiet := False;
end;

procedure TParadeTextTarget.Receive(Update: Pointer; Len: csize_t);
begin
  if FSync = nil then
    Exit;
  if FDirty and not FQuiet then
    FindLocal;     { this side's edits into the shared text first: the update lands on them }
  pd_tsync_receive(FSync, Update, Len);
end;

procedure TParadeTextTarget.Poll;
begin
  if FDirty and not FQuiet then
    FindLocal;
end;

function TParadeTextTarget.Undo(Redo: Boolean): Boolean;
begin
  Result := False;
  if FSync = nil then
    Exit;
  if FDirty then
    FindLocal;
  if Redo then
    Result := pd_tsync_redo(FSync) = PD_OK
  else
    Result := pd_tsync_undo(FSync) = PD_OK;
end;

function TParadeTextTarget.CanUndo(Redo: Boolean): Boolean;
begin
  if FSync = nil then
    Exit(False);
  if Redo then
    Result := pd_tsync_can_redo(FSync) > 0
  else
    Result := (pd_tsync_can_undo(FSync) > 0) or FDirty;
end;

procedure TParadeTextTarget.Seal;
begin
  if FSync = nil then
    Exit;
  if FDirty then
    FindLocal;
  pd_tsync_seal(FSync);
end;

function TParadeTextTarget.SharedText: string;
var
  P: PAnsiChar;
begin
  Result := '';
  if FSync = nil then
    Exit;
  P := pd_tsync_text(FSync);
  Result := P;
  pd_tsync_free_data(P);
end;

function TParadeTextTarget.Caret(out Key: string; out Offset: UInt32; out AnchorKey: string;
  out AnchorOffset: UInt32): Boolean;
var
  L, C, AL, AC: Integer;
begin
  Key := '';
  AnchorKey := '';
  Offset := 0;
  AnchorOffset := 0;
  if (FSync = nil) or FQuiet then
    Exit(False);
  if FDirty then
    FindLocal;
  GetCaret(L, C, AL, AC);
  Offset := PosOf(L, C);
  AnchorOffset := PosOf(AL, AC);
  Result := True;
end;

procedure TParadeTextTarget.ShowPeers(const APeers: array of TParadePeer);
var
  C: array of TParadeTextCaret;
  I: Integer;
begin
  C := nil;
  SetLength(C, Length(APeers));
  for I := 0 to High(APeers) do
  begin
    Locate(Min(APeers[I].Offset, LineStart(FShadow.Count - 1) + Units(FShadow[FShadow.Count - 1])), C[I].Line,
      C[I].Col);
    Locate(Min(APeers[I].AnchorOffset, LineStart(FShadow.Count - 1) + Units(FShadow[FShadow.Count - 1])),
      C[I].AnchorLine, C[I].AnchorCol);
    C[I].Color := APeers[I].Color;
    C[I].Name := APeers[I].Name;
  end;
  ShowCarets(C);
end;

procedure TParadeTextTarget.SetReadOnly(AValue: Boolean);
begin
  SetEditorReadOnly(AValue);
end;

function TParadeTextTarget.Dump(FromShared: Boolean): string;
begin
  if FromShared then
    Result := SharedText
  else
    Result := ParadeJoinLines(FShadow, 0, FShadow.Count);
end;

function ParadeRelayKind(const Server, Doc, Token: string): string;
var
  H: TFPHTTPClient;
  Body: RawByteString;
  T: Ppd_tsync;
  I, K: Integer;
  Len: Cardinal;
  Base: string;
begin
  Result := '';
  Base := Server;
  while (Base <> '') and (Base[Length(Base)] = '/') do
    Delete(Base, Length(Base), 1);
  H := TFPHTTPClient.Create(nil);
  try
    H.ConnectTimeout := 5000;
    H.IOTimeout := 30000;
    if Token <> '' then
      H.AddHeader('Authorization', 'Bearer ' + Token);
    try
      Body := H.Get(Base + '/d/' + ParadeUrlEncode(Doc) + '/updates?after=0');
    except
      Exit;
    end;
  finally
    H.Free;
  end;
  if Body = '' then
    Exit;
  { the updates into a plain text: a text's put text in it, a rich document's do not }
  if pd_tsync_new(0, T) <> PD_OK then
    Exit;
  try
    I := 1;
    while I + 11 <= Length(Body) do
    begin
      Len := 0;
      for K := 8 to 11 do
        Len := (Len shl 8) or Byte(Body[I + K]);
      if I + 12 + Int64(Len) - 1 > Length(Body) then
        Break;
      pd_tsync_receive(T, @Body[I + 12], Len);
      Inc(I, 12 + Len);
    end;
    if pd_tsync_length(T) > 0 then
      Result := 'text'
    else
      Result := 'rich';
  finally
    pd_tsync_free(T);
  end;
end;

{ TParadeStringsTarget }

constructor TParadeStringsTarget.Create(ALines: TStrings);
begin
  inherited Create;
  FLines := ALines;
end;

function TParadeStringsTarget.LineCount: Integer;
begin
  Result := FLines.Count;
end;

function TParadeStringsTarget.GetLine(I: Integer): string;
begin
  Result := FLines[I];
end;

procedure TParadeStringsTarget.SetText(const S: string);
var
  L: TStringList;
begin
  L := TStringList.Create;
  try
    ParadeSplitLines(S, L);
    FLines.Assign(L);
  finally
    L.Free;
  end;
end;

procedure TParadeStringsTarget.ReplaceRange(Line1, Col1, Line2, Col2: Integer; const S: string);
var
  T: string;
  L: TStringList;
  K: Integer;
begin
  if FLines.Count = 0 then
    FLines.Add('');
  T := Copy(FLines[Line1], 1, Col1) + S + Copy(FLines[Line2], Col2 + 1, MaxInt);
  L := TStringList.Create;
  try
    ParadeSplitLines(T, L);
    for K := Line1 to Line2 do
      FLines.Delete(Line1);
    for K := L.Count - 1 downto 0 do
      FLines.Insert(Line1, L[K]);
  finally
    L.Free;
  end;
end;

procedure TParadeStringsTarget.Edit(Line1, Col1, Line2, Col2: Integer; const S: string);
begin
  ReplaceRange(Line1, Col1, Line2, Col2, S);
  Changed(Line1);
end;

procedure TParadeStringsTarget.SetCaretAt(Line, Col: Integer);
begin
  FCaretLine := Line;
  FCaretCol := Col;
end;

procedure TParadeStringsTarget.GetCaret(out Line, Col, AnchorLine, AnchorCol: Integer);
begin
  Line := FCaretLine;
  Col := FCaretCol;
  AnchorLine := Line;
  AnchorCol := Col;
end;

procedure TParadeStringsTarget.ShowCarets(const Carets: array of TParadeTextCaret);
var
  I: Integer;
begin
  SetLength(FCarets, Length(Carets));
  for I := 0 to High(Carets) do
    FCarets[I] := Carets[I];
end;

procedure TParadeStringsTarget.SetEditorReadOnly(AValue: Boolean);
begin
  FReadOnly := AValue;
end;

function TParadeStringsTarget.PeerCarets: Integer;
begin
  Result := Length(FCarets);
end;

function TParadeStringsTarget.PeerCaret(I: Integer): TParadeTextCaret;
begin
  Result := FCarets[I];
end;

end.
