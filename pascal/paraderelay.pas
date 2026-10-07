{ The Parade relay in Pascal: what tools/parade_relay.py does, inside a
  program -- an editor can host the documents it shares, no Python and no
  database server needed.

  The log is an SQLite file through SQLdb (the same tables as the Python
  relay's, so either can serve a file the other wrote), HTTP is
  fphttpserver with a thread per request, tokens are the same HS256 JWTs.
  Compaction merges the log in-process with pd_sync_merge (Parade built
  with SYNC=yrs, as paradesync needs anyway).

    R := TParadeRelay.Create(nil);
    R.DbFile := 'relay.sqlite';
    R.Secret := ParadeReadSecret('relay.secret');   (or ParadeNewSecret, kept)
    R.Port := 8765;
    if not R.Start then ... R.LastError ...
    Token := ParadeMakeToken(R.Secret, 'bob', 'proposal', 'editor', 30);

  HTTP API: see tools/parade_relay.py. SQLdb loads the SQLite library at run
  time (libsqlite3.so.0, sqlite3.dll, libsqlite3.dylib). PARADE_RELAY_TRACE
  set in the environment logs every request through OnLog. }

unit paraderelay;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils, SyncObjs, DateUtils, ctypes, fpjson, jsonparser, base64, sqldb, sqlite3conn, db,
  fphttpserver, httpdefs, parade;   { parade: libparade.a, which has pd_sync_merge when built with SYNC=yrs }

{$LINKLIB yrs}
{$LINKLIB pthread}
{$LINKLIB dl}
{$LINKLIB gcc_s}

type
  TParadeRelayThread = class;
  TBytesArray = array of TBytes;

  { one document's log in an SQLite file; every call takes the lock (one connection, any thread) }
  TParadeRelayStore = class
  private
    FConn: TSQLite3Connection;
    FTrans: TSQLTransaction;
    FLock: TCriticalSection;
    function Query(const Sql: string): TSQLQuery;
    function LastLocked(const Doc: string): Int64;
  public
    constructor Create(const FileName: string);
    destructor Destroy; override;
    function Append(const Doc, Client, Author: string; const Data: RawByteString): Int64;
    { the frames after Seq ([seq u64][len u32][bytes], big-endian), up to about LimitBytes;
      Last: the newest number in them (Seq when none) }
    function After(const Doc: string; Seq: Int64; LimitBytes: Int64; out Last: Int64): RawByteString;
    function Last(const Doc: string): Int64;
    { the snapshot and the updates after it; Upto: the number the last has }
    procedure CompactionInput(const Doc: string; out Parts: TBytesArray; out Upto: Int64);
    procedure SaveSnapshot(const Doc: string; Upto: Int64; const Data: RawByteString);
    function Info(const Doc: string): string;
  end;

  TParadeRelayServer = class(TFPHttpServer)
  public
    property Address;
  end;

  TParadeRelayLog = procedure(Sender: TObject; const Msg: string) of object;

  TParadeRelay = class(TComponent)
  private
    FStore: TParadeRelayStore;
    FServer: TParadeRelayServer;
    FThread: TParadeRelayThread;
    FDbFile, FHost, FLastError: string;
    FSecret: RawByteString;
    FPort: Word;
    FCompact: Boolean;
    FCompactEvery: Integer;
    FLock: TCriticalSection;
    FWaiters: TList;            { long polls waiting: PWaiter }
    FSince: TStringList;        { doc -> updates since compaction last started (Objects) }
    FCompacting: TStringList;
    FCompactions: Integer;      { compaction threads running }
    FPresence: TStringList;     { doc#0client -> TPresence }
    FListening: TEvent;
    FStopping: Boolean;
    FOnLog: TParadeRelayLog;
    procedure Request(Sender: TObject; var ARequest: TFPHTTPConnectionRequest;
      var AResponse: TFPHTTPConnectionResponse);
    procedure Idle(Sender: TObject);
    procedure Notify(const Doc: string);
    procedure Log(const Msg: string);
    procedure CompactLater(const Doc: string);
    function GetActive: Boolean;
  public
    constructor Create(AOwner: TComponent); override;
    destructor Destroy; override;
    { opens the log and listens; False and LastError when it cannot }
    function Start: Boolean;
    procedure Stop;
    { the document's snapshot and the updates after it made one snapshot; False: nothing to do }
    function CompactNow(const Doc: string): Boolean;
    property Active: Boolean read GetActive;
    property DbFile: string read FDbFile write FDbFile;
    { the address to listen on: 127.0.0.1 (this machine only, the default) or 0.0.0.0 (everyone) }
    property Host: string read FHost write FHost;
    property Port: Word read FPort write FPort;
    property Secret: RawByteString read FSecret write FSecret;
    property Compact: Boolean read FCompact write FCompact;
    property CompactEvery: Integer read FCompactEvery write FCompactEvery;
    property LastError: string read FLastError;
    property Store: TParadeRelayStore read FStore;
    { called from the server's threads }
    property OnLog: TParadeRelayLog read FOnLog write FOnLog;
  end;

  TParadeRelayThread = class(TThread)
  private
    FRelay: TParadeRelay;
    FError: string;
  protected
    procedure Execute; override;
  public
    constructor Create(ARelay: TParadeRelay);
  end;

function ParadeSHA256(const Data: RawByteString): RawByteString;
function ParadeHMACSHA256(const Key, Data: RawByteString): RawByteString;
function ParadeBase64Url(const Data: RawByteString): string;
function ParadeBase64UrlDecode(const S: string): RawByteString;
{ a signed token: Doc '*' for every document, Role viewer | commenter | editor }
function ParadeMakeToken(const Secret: RawByteString; const User, Doc, Role: string; Days: Double): string;
{ the token's claims (free them) when it is signed with Secret and not expired; nil and Why otherwise }
function ParadeCheckToken(const Secret: RawByteString; const Token: string; out Why: string): TJSONObject;
{ the document's snapshot and the updates after it merged into one snapshot (pd_sync_merge), which
  replaces them; False: nothing to merge, or the log does not merge (then it is left as it was) }
function ParadeCompact(Store: TParadeRelayStore; const Doc: string): Boolean;
{ a new random signing key, as text }
function ParadeNewSecret: string;
{ a key from a file (surrounding white space dropped); raises when it is shorter than 32 bytes }
function ParadeReadSecret(const FileName: string): RawByteString;

implementation


const
  MAX_UPDATE = 16 shl 20;
  MAX_BATCH = 4 shl 20;
  PRESENCE_TTL = 30.0;
  PD_OK = 0;

function pd_sync_merge(updates: PPointer; lens: Pcsize_t; n: csize_t; out data: Pointer; out len: csize_t): cint;
  cdecl; external;
procedure pd_sync_free_data(data: Pointer); cdecl; external;

type
  PWaiter = ^TWaiter;
  TWaiter = record
    Doc: string;
    Event: TEvent;
  end;

  TPresence = class
    At: Double;
    Json: string;
  end;

  TCompactThread = class(TThread)
  private
    FRelay: TParadeRelay;
    FDoc: string;
  protected
    procedure Execute; override;
  public
    constructor Create(ARelay: TParadeRelay; const ADoc: string);
  end;

{ ---------- SHA-256, HMAC, base64url, JWT ---------- }

const
  K256: array[0..63] of Cardinal = (
    $428a2f98, $71374491, $b5c0fbcf, $e9b5dba5, $3956c25b, $59f111f1, $923f82a4, $ab1c5ed5,
    $d807aa98, $12835b01, $243185be, $550c7dc3, $72be5d74, $80deb1fe, $9bdc06a7, $c19bf174,
    $e49b69c1, $efbe4786, $0fc19dc6, $240ca1cc, $2de92c6f, $4a7484aa, $5cb0a9dc, $76f988da,
    $983e5152, $a831c66d, $b00327c8, $bf597fc7, $c6e00bf3, $d5a79147, $06ca6351, $14292967,
    $27b70a85, $2e1b2138, $4d2c6dfc, $53380d13, $650a7354, $766a0abb, $81c2c92e, $92722c85,
    $a2bfe8a1, $a81a664b, $c24b8b70, $c76c51a3, $d192e819, $d6990624, $f40e3585, $106aa070,
    $19a4c116, $1e376c08, $2748774c, $34b0bcb5, $391c0cb3, $4ed8aa4a, $5b9cca4f, $682e6ff3,
    $748f82ee, $78a5636f, $84c87814, $8cc70208, $90befffa, $a4506ceb, $bef9a3f7, $c67178f2);

{ binary strings are handled as bytes throughout: a program whose default code page is UTF-8 (any LCL
  program) would otherwise re-encode bytes from $80 up wherever two strings meet }
function Bytes(const S: RawByteString): TBytes;
begin
  SetLength(Result, Length(S));
  if S <> '' then
    Move(S[1], Result[0], Length(S));
end;

function Str(const B: TBytes): RawByteString;
begin
  Result := '';
  SetLength(Result, Length(B));
  if Length(B) > 0 then
    Move(B[0], Result[1], Length(B));
end;

{$PUSH}{$R-}{$Q-}    { the hash adds modulo 2^32: a build with range and overflow checks on would stop it }
function SHA256Bytes(const Data: TBytes): TBytes;
var
  H: array[0..7] of Cardinal;
  W: array[0..63] of Cardinal;
  M: TBytes;
  A, B, C, D, E, F, G, HH, T1, T2, S0, S1: Cardinal;
  I, J, Blk, N: Integer;
  Bits: QWord;
begin
  H[0] := $6a09e667; H[1] := $bb67ae85; H[2] := $3c6ef372; H[3] := $a54ff53a;
  H[4] := $510e527f; H[5] := $9b05688c; H[6] := $1f83d9ab; H[7] := $5be0cd19;
  Bits := QWord(Length(Data)) * 8;
  N := Length(Data) + 1;
  while N mod 64 <> 56 do
    Inc(N);
  SetLength(M, N + 8);
  FillChar(M[0], Length(M), 0);
  if Length(Data) > 0 then
    Move(Data[0], M[0], Length(Data));
  M[Length(Data)] := $80;
  for I := 0 to 7 do
    M[N + I] := (Bits shr ((7 - I) * 8)) and $FF;
  for Blk := 0 to Length(M) div 64 - 1 do
  begin
    for I := 0 to 15 do
    begin
      J := Blk * 64 + I * 4;
      W[I] := Cardinal(M[J]) shl 24 or Cardinal(M[J + 1]) shl 16 or Cardinal(M[J + 2]) shl 8 or M[J + 3];
    end;
    for I := 16 to 63 do
    begin
      S0 := RorDWord(W[I - 15], 7) xor RorDWord(W[I - 15], 18) xor (W[I - 15] shr 3);
      S1 := RorDWord(W[I - 2], 17) xor RorDWord(W[I - 2], 19) xor (W[I - 2] shr 10);
      W[I] := W[I - 16] + S0 + W[I - 7] + S1;
    end;
    A := H[0]; B := H[1]; C := H[2]; D := H[3]; E := H[4]; F := H[5]; G := H[6]; HH := H[7];
    for I := 0 to 63 do
    begin
      S1 := RorDWord(E, 6) xor RorDWord(E, 11) xor RorDWord(E, 25);
      T1 := HH + S1 + ((E and F) xor ((not E) and G)) + K256[I] + W[I];
      S0 := RorDWord(A, 2) xor RorDWord(A, 13) xor RorDWord(A, 22);
      T2 := S0 + ((A and B) xor (A and C) xor (B and C));
      HH := G; G := F; F := E; E := D + T1; D := C; C := B; B := A; A := T1 + T2;
    end;
    H[0] += A; H[1] += B; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += HH;
  end;
  SetLength(Result, 32);
  for I := 0 to 7 do
  begin
    Result[I * 4] := H[I] shr 24;
    Result[I * 4 + 1] := (H[I] shr 16) and $FF;
    Result[I * 4 + 2] := (H[I] shr 8) and $FF;
    Result[I * 4 + 3] := H[I] and $FF;
  end;
end;
{$POP}

function ParadeSHA256(const Data: RawByteString): RawByteString;
begin
  Result := Str(SHA256Bytes(Bytes(Data)));
end;

function ParadeHMACSHA256(const Key, Data: RawByteString): RawByteString;
var
  K, Inner, Outer, D: TBytes;
  I: Integer;
begin
  K := Bytes(Key);
  if Length(K) > 64 then
    K := SHA256Bytes(K);
  I := Length(K);
  SetLength(K, 64);
  if I < 64 then
    FillChar(K[I], 64 - I, 0);
  D := Bytes(Data);
  SetLength(Inner, 64 + Length(D));
  for I := 0 to 63 do
    Inner[I] := K[I] xor $36;
  if Length(D) > 0 then
    Move(D[0], Inner[64], Length(D));
  D := SHA256Bytes(Inner);
  SetLength(Outer, 64 + 32);
  for I := 0 to 63 do
    Outer[I] := K[I] xor $5C;
  Move(D[0], Outer[64], 32);
  Result := Str(SHA256Bytes(Outer));
end;

function ParadeBase64Url(const Data: RawByteString): string;
var
  I: Integer;
begin
  Result := EncodeStringBase64(Data);
  while (Result <> '') and (Result[Length(Result)] = '=') do
    Delete(Result, Length(Result), 1);
  for I := 1 to Length(Result) do
    case Result[I] of
      '+': Result[I] := '-';
      '/': Result[I] := '_';
    end;
end;

function ParadeBase64UrlDecode(const S: string): RawByteString;
var
  T: string;
  I: Integer;
begin
  T := S;
  for I := 1 to Length(T) do
    case T[I] of
      '-': T[I] := '+';
      '_': T[I] := '/';
      'A'..'Z', 'a'..'z', '0'..'9': ;
    else
      Exit('');
    end;
  while Length(T) mod 4 <> 0 do
    T := T + '=';
  Result := DecodeStringBase64(T, True);
end;

function NowUnix: Double;
begin
  Result := (LocalTimeToUniversal(Now) - UnixDateDelta) * 86400.0;
end;

function ParadeMakeToken(const Secret: RawByteString; const User, Doc, Role: string; Days: Double): string;
var
  P: TJSONObject;
  Head: string;
begin
  P := TJSONObject.Create(['sub', User, 'doc', Doc, 'role', Role, 'exp', Int64(Trunc(NowUnix + Days * 86400))]);
  try
    Head := ParadeBase64Url('{"alg":"HS256","typ":"JWT"}') + '.' + ParadeBase64Url(P.AsJSON);
  finally
    P.Free;
  end;
  Result := Head + '.' + ParadeBase64Url(ParadeHMACSHA256(Secret, Head));
end;

function SameBytes(const A, B: RawByteString): Boolean;
var
  I: Integer;
  D: Byte;
begin
  if Length(A) <> Length(B) then
    Exit(False);
  D := 0;
  for I := 1 to Length(A) do
    D := D or (Byte(A[I]) xor Byte(B[I]));    { the same time whatever differs }
  Result := D = 0;
end;

function ParadeCheckToken(const Secret: RawByteString; const Token: string; out Why: string): TJSONObject;
var
  P1, P2: Integer;
  Head: TJSONData;
  Claims: TJSONData;
begin
  Result := nil;
  Why := 'bad token';
  P1 := Pos('.', Token);
  P2 := Pos('.', Token, P1 + 1);
  if (P1 = 0) or (P2 = 0) or (Pos('.', Token, P2 + 1) <> 0) then
    Exit;
  if not SameBytes(ParadeBase64UrlDecode(Copy(Token, P2 + 1, MaxInt)),
    ParadeHMACSHA256(Secret, Copy(Token, 1, P2 - 1))) then
  begin
    Why := 'bad token: signature';
    Exit;
  end;
  Head := nil;
  Claims := nil;
  try
    try
      Head := GetJSON(ParadeBase64UrlDecode(Copy(Token, 1, P1 - 1)));
      Claims := GetJSON(ParadeBase64UrlDecode(Copy(Token, P1 + 1, P2 - P1 - 1)));
    except
      Exit;
    end;
    if not (Head is TJSONObject) or (TJSONObject(Head).Get('alg', '') <> 'HS256') or not (Claims is TJSONObject) then
      Exit;
    with TJSONObject(Claims) do
      if (Find('exp') = nil) or (Find('sub') = nil) or (Find('role') = nil) then
      begin
        Why := 'bad token: exp, sub and role needed';
        Exit;
      end
      else if Elements['exp'].AsFloat <= NowUnix then
      begin
        Why := 'bad token: expired';
        Exit;
      end;
    Result := TJSONObject(Claims);
    Claims := nil;
    Why := '';
  finally
    Head.Free;
    Claims.Free;
  end;
end;

function ParadeNewSecret: string;
var
  B: RawByteString;
  F: TFileStream;
  I: Integer;
begin
  SetLength(B, 48);
  try
    F := TFileStream.Create('/dev/urandom', fmOpenRead or fmShareDenyNone);
    try
      F.ReadBuffer(B[1], 48);
    finally
      F.Free;
    end;
  except
    Randomize;
    for I := 1 to 48 do
      B[I] := Chr(Random(256));
  end;
  Result := ParadeBase64Url(B);
end;

function ParadeReadSecret(const FileName: string): RawByteString;
begin
  with TStringList.Create do
  try
    LoadFromFile(FileName);
    Result := Trim(Text);
  finally
    Free;
  end;
  if Length(Result) < 32 then
    raise Exception.Create('the secret is too short (want 32 bytes or more)');
end;

{ ---------- the store ---------- }

function ToBytes(const S: RawByteString): TBytes;
begin
  Result := Bytes(S);
end;

function FieldBytes(F: TField): TBytes;
begin
  Result := F.AsBytes;
end;

{ one frame of a read, [seq u64][length u32][bytes] big-endian, onto the stream }
procedure Frame(Dest: TStream; Seq: Int64; const Data: TBytes);
var
  H: array[0..11] of Byte;
  I: Integer;
  N: Cardinal;
begin
  for I := 0 to 7 do
    H[I] := (QWord(Seq) shr ((7 - I) * 8)) and $FF;
  N := Length(Data);
  for I := 0 to 3 do
    H[8 + I] := (N shr ((3 - I) * 8)) and $FF;
  Dest.WriteBuffer(H, 12);
  if N > 0 then
    Dest.WriteBuffer(Data[0], N);
end;

constructor TParadeRelayStore.Create(const FileName: string);
begin
  FLock := TCriticalSection.Create;
  FConn := TSQLite3Connection.Create(nil);
  FTrans := TSQLTransaction.Create(nil);
  FTrans.Options := [stoUseImplicit];     { autocommit; the one transaction that matters is explicit }
  FConn.Transaction := FTrans;
  FTrans.DataBase := FConn;
  FConn.DatabaseName := FileName;
  FConn.AlwaysUseBigint := True;
  FConn.Open;
  with Query('pragma journal_mode=wal') do
  try
    Open;
  finally
    Free;
  end;
  FConn.ExecuteDirect('pragma synchronous=full');
  FConn.ExecuteDirect('create table if not exists doc_update (doc text not null, seq integer not null,' +
    ' client text, author text, data blob not null, at real not null, primary key (doc, seq))');
  FConn.ExecuteDirect('create table if not exists doc_snapshot (doc text primary key, upto integer not null,' +
    ' data blob not null, at real not null)');
end;

destructor TParadeRelayStore.Destroy;
begin
  if FConn <> nil then
    FConn.Close;
  FTrans.Free;
  FConn.Free;
  FLock.Free;
  inherited Destroy;
end;

function TParadeRelayStore.Query(const Sql: string): TSQLQuery;
begin
  Result := TSQLQuery.Create(nil);
  Result.DataBase := FConn;
  Result.Transaction := FTrans;
  Result.SQL.Text := Sql;
end;

function TParadeRelayStore.LastLocked(const Doc: string): Int64;
begin
  with Query('select max(coalesce((select max(seq) from doc_update where doc = :d), 0),' +
    ' coalesce((select upto from doc_snapshot where doc = :d), 0)) as n') do
  try
    ParamByName('d').AsString := Doc;
    Open;
    Result := Fields[0].AsLargeInt;
  finally
    Free;
  end;
end;

function TParadeRelayStore.Append(const Doc, Client, Author: string; const Data: RawByteString): Int64;
begin
  FLock.Enter;
  try
    Result := LastLocked(Doc) + 1;
    with Query('insert into doc_update values (:d, :s, :c, :a, :data, :at)') do
    try
      ParamByName('d').AsString := Doc;
      ParamByName('s').AsLargeInt := Result;
      ParamByName('c').AsString := Client;
      ParamByName('a').AsString := Author;
      ParamByName('data').AsBlob := ToBytes(Data);
      ParamByName('at').AsFloat := NowUnix;
      ExecSQL;
    finally
      Free;
    end;
  finally
    FLock.Leave;
  end;
end;

function TParadeRelayStore.After(const Doc: string; Seq: Int64; LimitBytes: Int64; out Last: Int64): RawByteString;
var
  Newest, Size: Int64;
  D: TBytes;
  Dest: TMemoryStream;
begin
  Newest := Seq;
  Size := 0;
  Dest := TMemoryStream.Create;
  FLock.Enter;
  try
    with Query('select upto, data from doc_snapshot where doc = :d') do
    try
      ParamByName('d').AsString := Doc;
      Open;
      if not EOF and (Seq < Fields[0].AsLargeInt) then
      begin   { from before the snapshot: the snapshot, then what follows it }
        Seq := Fields[0].AsLargeInt;
        D := FieldBytes(Fields[1]);
        Frame(Dest, Seq, D);
        Size := Length(D);
        Newest := Seq;
      end;
    finally
      Free;
    end;
    with Query('select seq, data from doc_update where doc = :d and seq > :s order by seq') do
    try
      ParamByName('d').AsString := Doc;
      ParamByName('s').AsLargeInt := Seq;
      Open;
      while not EOF do
      begin
        D := FieldBytes(Fields[1]);
        if (Dest.Size > 0) and (Size + Length(D) > LimitBytes) then
          Break;
        Newest := Fields[0].AsLargeInt;
        Frame(Dest, Newest, D);
        Inc(Size, Length(D));
        Next;
      end;
    finally
      Free;
    end;
    Result := '';
    SetLength(Result, Dest.Size);
    if Dest.Size > 0 then
      Move(Dest.Memory^, Result[1], Dest.Size);
  finally
    FLock.Leave;
    Dest.Free;
  end;
  Last := Newest;
end;

function TParadeRelayStore.Last(const Doc: string): Int64;
begin
  FLock.Enter;
  try
    Result := LastLocked(Doc);
  finally
    FLock.Leave;
  end;
end;

procedure TParadeRelayStore.CompactionInput(const Doc: string; out Parts: TBytesArray; out Upto: Int64);

  procedure Add(const B: TBytes);
  begin
    SetLength(Parts, Length(Parts) + 1);
    Parts[High(Parts)] := B;
  end;

begin
  Parts := nil;
  Upto := 0;
  FLock.Enter;
  try
    with Query('select upto, data from doc_snapshot where doc = :d') do
    try
      ParamByName('d').AsString := Doc;
      Open;
      if not EOF then
      begin
        Upto := Fields[0].AsLargeInt;
        Add(FieldBytes(Fields[1]));
      end;
    finally
      Free;
    end;
    with Query('select seq, data from doc_update where doc = :d and seq > :s order by seq') do
    try
      ParamByName('d').AsString := Doc;
      ParamByName('s').AsLargeInt := Upto;
      Open;
      while not EOF do
      begin
        Upto := Fields[0].AsLargeInt;
        Add(FieldBytes(Fields[1]));
        Next;
      end;
    finally
      Free;
    end;
  finally
    FLock.Leave;
  end;
end;

procedure TParadeRelayStore.SaveSnapshot(const Doc: string; Upto: Int64; const Data: RawByteString);
begin
  FLock.Enter;
  try
    FConn.ExecuteDirect('begin immediate');
    try
      with Query('insert or replace into doc_snapshot values (:d, :u, :data, :at)') do
      try
        ParamByName('d').AsString := Doc;
        ParamByName('u').AsLargeInt := Upto;
        ParamByName('data').AsBlob := ToBytes(Data);
        ParamByName('at').AsFloat := NowUnix;
        ExecSQL;
      finally
        Free;
      end;
      with Query('delete from doc_update where doc = :d and seq <= :u') do
      try
        ParamByName('d').AsString := Doc;
        ParamByName('u').AsLargeInt := Upto;
        ExecSQL;
      finally
        Free;
      end;
      FConn.ExecuteDirect('commit');
    except
      FConn.ExecuteDirect('rollback');
      raise;
    end;
  finally
    FLock.Leave;
  end;
end;

function TParadeRelayStore.Info(const Doc: string): string;
var
  Base, N: Int64;
begin
  FLock.Enter;
  try
    with Query('select coalesce((select upto from doc_snapshot where doc = :d), 0) as n') do
    try
      ParamByName('d').AsString := Doc;
      Open;
      Base := Fields[0].AsLargeInt;
    finally
      Free;
    end;
    with Query('select count(*) as n from doc_update where doc = :d and seq > :s') do
    try
      ParamByName('d').AsString := Doc;
      ParamByName('s').AsLargeInt := Base;
      Open;
      N := Fields[0].AsLargeInt;
    finally
      Free;
    end;
    Result := Format('{"last": %d, "snapshot": %d, "updates": %d}', [LastLocked(Doc), Base, N]);
  finally
    FLock.Leave;
  end;
end;

{ ---------- the server ---------- }

constructor TParadeRelayThread.Create(ARelay: TParadeRelay);
begin
  FRelay := ARelay;
  inherited Create(False);
end;

procedure TParadeRelayThread.Execute;
begin
  try
    FRelay.FServer.Active := True;    { returns when stopped }
  except
    on E: Exception do
      FError := E.Message;
  end;
  FRelay.FListening.SetEvent;         { a Start waiting is told either way }
end;

constructor TCompactThread.Create(ARelay: TParadeRelay; const ADoc: string);
begin
  FRelay := ARelay;
  FDoc := ADoc;
  FreeOnTerminate := True;
  inherited Create(False);
end;

procedure TCompactThread.Execute;
var
  I: Integer;
begin
  try
    try
      if not FRelay.CompactNow(FDoc) then
        FRelay.Log('compaction of ' + FDoc + ': nothing done (the log does not merge?)');
    except
      on E: Exception do
        FRelay.Log('compaction of ' + FDoc + ' failed: ' + E.Message);
    end;
  finally
    FRelay.FLock.Enter;
    try
      I := FRelay.FCompacting.IndexOf(FDoc);
      if I >= 0 then
        FRelay.FCompacting.Delete(I);
      Dec(FRelay.FCompactions);
    finally
      FRelay.FLock.Leave;
    end;
  end;
end;

constructor TParadeRelay.Create(AOwner: TComponent);
begin
  inherited Create(AOwner);
  FHost := '127.0.0.1';
  FPort := 8765;
  FDbFile := 'relay.sqlite';
  FCompact := True;
  FCompactEvery := 500;
  FLock := TCriticalSection.Create;
  FWaiters := TList.Create;
  FSince := TStringList.Create;
  FSince.Sorted := True;
  FCompacting := TStringList.Create;
  FPresence := TStringList.Create;
  FPresence.Sorted := True;
  FPresence.OwnsObjects := True;
  FListening := TEvent.Create(nil, True, False, '');
end;

destructor TParadeRelay.Destroy;
begin
  Stop;
  FListening.Free;
  FPresence.Free;
  FCompacting.Free;
  FSince.Free;
  FWaiters.Free;
  FLock.Free;
  inherited Destroy;
end;

function TParadeRelay.GetActive: Boolean;
begin
  Result := FServer <> nil;
end;

procedure TParadeRelay.Log(const Msg: string);
begin
  if Assigned(FOnLog) then
    FOnLog(Self, Msg);
end;

function TParadeRelay.Start: Boolean;
begin
  Result := False;
  Stop;
  FLastError := '';
  if Length(FSecret) < 32 then
  begin
    FLastError := 'the secret is too short (want 32 bytes or more)';
    Exit;
  end;
  try
    FStore := TParadeRelayStore.Create(FDbFile);
  except
    on E: Exception do
    begin
      FLastError := 'the log: ' + E.Message;
      FreeAndNil(FStore);
      Exit;
    end;
  end;
  FStopping := False;
  FServer := TParadeRelayServer.Create(nil);
  FServer.Address := FHost;
  FServer.Port := FPort;
  FServer.Threaded := True;
  FServer.QueueSize := 64;
  FServer.AcceptIdleTimeout := 200;     { how often the accept loop looks whether it is to stop }
  FServer.OnAcceptIdle := @Idle;
  FServer.OnRequest := @Request;
  FListening.ResetEvent;
  FThread := TParadeRelayThread.Create(Self);
  FListening.WaitFor(10000);
  if FThread.Finished or (FThread.FError <> '') then
  begin
    FThread.WaitFor;
    FLastError := FThread.FError;
    if FLastError = '' then
      FLastError := 'the server did not start';
    FreeAndNil(FThread);
    FreeAndNil(FServer);
    FreeAndNil(FStore);
    Exit;
  end;
  Log(Format('relay on http://%s:%d, log in %s', [FHost, FPort, FDbFile]));
  Result := True;
end;

procedure TParadeRelay.Idle(Sender: TObject);
begin
  FListening.SetEvent;    { bound and accepting }
end;

procedure TParadeRelay.Stop;
var
  I: Integer;
  Busy: Boolean;
begin
  if FServer = nil then
    Exit;
  FLock.Enter;
  try
    FStopping := True;
    for I := 0 to FWaiters.Count - 1 do
      PWaiter(FWaiters[I])^.Event.SetEvent;     { the long polls return now }
  finally
    FLock.Leave;
  end;
  FServer.Active := False;
  FThread.WaitFor;
  FreeAndNil(FThread);
  FreeAndNil(FServer);    { waits for the requests under way }
  repeat
    FLock.Enter;
    Busy := FCompactions > 0;
    FLock.Leave;
    if Busy then
      Sleep(20);
  until not Busy;
  FreeAndNil(FStore);
  FPresence.Clear;
  FSince.Clear;
end;

procedure TParadeRelay.Notify(const Doc: string);
var
  I: Integer;
begin
  FLock.Enter;
  try
    for I := 0 to FWaiters.Count - 1 do
      if PWaiter(FWaiters[I])^.Doc = Doc then
        PWaiter(FWaiters[I])^.Event.SetEvent;
  finally
    FLock.Leave;
  end;
end;

function TParadeRelay.CompactNow(const Doc: string): Boolean;
begin
  Result := (FStore <> nil) and ParadeCompact(FStore, Doc);
end;

function ParadeCompact(Store: TParadeRelayStore; const Doc: string): Boolean;
var
  Parts: TBytesArray;
  Upto: Int64;
  P: array of Pointer;
  L: array of csize_t;
  I: Integer;
  Data: Pointer;
  Len: csize_t;
  Merged: RawByteString;
begin
  Result := False;
  Store.CompactionInput(Doc, Parts, Upto);
  if Length(Parts) < 2 then
    Exit;
  SetLength(P, Length(Parts));
  SetLength(L, Length(Parts));
  for I := 0 to High(Parts) do
  begin
    if Length(Parts[I]) > 0 then
      P[I] := @Parts[I][0]
    else
      P[I] := nil;
    L[I] := Length(Parts[I]);
  end;
  if pd_sync_merge(@P[0], @L[0], Length(Parts), Data, Len) <> PD_OK then
    Exit;
  Merged := '';
  SetLength(Merged, Len);
  if Len > 0 then
    Move(Data^, Merged[1], Len);
  pd_sync_free_data(Data);
  Store.SaveSnapshot(Doc, Upto, Merged);
  Result := True;
end;

procedure TParadeRelay.CompactLater(const Doc: string);
var
  I, N: Integer;
begin
  FLock.Enter;
  try
    if FStopping then
      Exit;
    I := FSince.IndexOf(Doc);
    if I < 0 then
      I := FSince.Add(Doc);
    N := PtrInt(FSince.Objects[I]) + 1;
    if (N >= FCompactEvery) and (FCompacting.IndexOf(Doc) < 0) then
    begin
      N := 0;
      FCompacting.Add(Doc);
      Inc(FCompactions);
      TCompactThread.Create(Self, Doc);
    end;
    FSince.Objects[I] := TObject(PtrInt(N));
  finally
    FLock.Leave;
  end;
end;

procedure Reply(AResponse: TFPHTTPConnectionResponse; Code: Integer; const Text: RawByteString;
  const ContentType: string = 'text/plain; charset=utf-8');
begin
  AResponse.Code := Code;
  case Code of
    200: AResponse.CodeText := 'OK';
    400: AResponse.CodeText := 'Bad Request';
    401: AResponse.CodeText := 'Unauthorized';
    403: AResponse.CodeText := 'Forbidden';
    404: AResponse.CodeText := 'Not Found';
    405: AResponse.CodeText := 'Method Not Allowed';
    413: AResponse.CodeText := 'Payload Too Large';
  else
    AResponse.CodeText := 'Error';
  end;
  AResponse.ContentType := ContentType;
  { as bytes: Content would go through a TStrings and gain a line end }
  AResponse.ContentStream := TMemoryStream.Create;
  if Text <> '' then
    AResponse.ContentStream.WriteBuffer(Text[1], Length(Text));
  AResponse.ContentStream.Position := 0;
  AResponse.FreeContentStream := True;
  AResponse.ContentLength := Length(Text);
end;

type
  EReply = class(Exception)
    Code: Integer;
  end;

procedure Refuse(Code: Integer; const Why: string);
var
  E: EReply;
begin
  E := EReply.Create(Why);
  E.Code := Code;
  raise E;
end;

function HtmlText(const S: string): string;
begin
  Result := StringReplace(StringReplace(StringReplace(S, '&', '&amp;', [rfReplaceAll]), '<', '&lt;', [rfReplaceAll]),
    '>', '&gt;', [rfReplaceAll]);
  Result := StringReplace(Result, '"', '&quot;', [rfReplaceAll]);
end;

{ what a browser opening an invitation link shows }
function InvitePage(const Doc: string): string;
begin
  Result := '<!doctype html><html><head><meta charset="utf-8"><title>Parade: ' + HtmlText(Doc) +
    '</title></head><body style="font-family:sans-serif;max-width:40em;margin:3em auto;line-height:1.5">' +
    '<h1>&ldquo;' + HtmlText(Doc) + '&rdquo;</h1>' +
    '<p>This link is an invitation to edit a shared document with Parade.</p>' +
    '<p>To join, open LED, choose <b>File &gt; Join Shared Document...</b> and paste the whole link, ' +
    'with the part after the <code>#</code>: that part is the key, so keep the link to yourself.</p>' +
    '</body></html>';
end;

procedure TParadeRelay.Request(Sender: TObject; var ARequest: TFPHTTPConnectionRequest;
  var AResponse: TFPHTTPConnectionResponse);
var
  Path, Doc, Act, Why, H, Body, Client: string;
  Claims: TJSONObject;
  P, I: Integer;
  After, Last: Int64;
  Wait, Deadline, T: Double;
  Waiter: TWaiter;
  Data: RawByteString;
  J: TJSONData;
  Dest: TJSONArray;
  Pr: TPresence;
  Key, Prefix: string;
  Keep: TJSONObject;

  procedure Authorize(Write: Boolean);
  var
    D, Role: string;
  begin
    H := ARequest.Authorization;
    if Copy(H, 1, 7) <> 'Bearer ' then
      Refuse(401, 'token needed');
    Claims := ParadeCheckToken(FSecret, Copy(H, 8, MaxInt), Why);
    if Claims = nil then
      Refuse(401, Why);
    D := Claims.Get('doc', '*');
    if (D <> '*') and (D <> Doc) then
      Refuse(403, 'not this document');
    Role := Claims.Get('role', '');
    if (Role <> 'viewer') and (Role <> 'commenter') and (Role <> 'editor') then
      Refuse(403, 'unknown role');
    if Write and (Role = 'viewer') then
      Refuse(403, 'read only');
  end;

begin
  Claims := nil;
  if GetEnvironmentVariable('PARADE_RELAY_TRACE') <> '' then
    Log('> ' + ARequest.Method + ' ' + ARequest.URI);
  AResponse.SetCustomHeader('Connection', 'close');
  try
    try
      Path := ARequest.PathInfo;
      if Path = '' then
        Path := ARequest.URI;
      P := Pos('?', Path);
      if P > 0 then
        Path := Copy(Path, 1, P - 1);
      if Path = '/health' then
      begin
        Reply(AResponse, 200, 'ok');
        Exit;
      end;
      if Copy(Path, 1, 3) <> '/d/' then
        Refuse(404, 'not found');
      Delete(Path, 1, 3);
      P := Pos('/', Path);
      if (P = 0) and (ARequest.Method = 'GET') then
      begin   { an invitation link opened in a browser }
        Reply(AResponse, 200, InvitePage(HTTPDecode(Path)), 'text/html; charset=utf-8');
        Exit;
      end;
      if P = 0 then
        Refuse(404, 'not found');
      Doc := HTTPDecode(Copy(Path, 1, P - 1));
      Act := Copy(Path, P, MaxInt);
      if (Doc = '') or (Length(Doc) > 200) or (Pos('/', Doc) > 0) or (Pos('\', Doc) > 0) or (Pos(#0, Doc) > 0) then
        Refuse(400, 'bad document name');

      if (Act = '/updates') and (ARequest.Method = 'POST') then
      begin
        Authorize(True);
        Data := ARequest.Content;
        SetCodePage(Data, CP_NONE, False);
        if (Data = '') or (Length(Data) > MAX_UPDATE) then
          Refuse(400, 'empty or too large');
        Client := Copy(ARequest.GetCustomHeader('X-Client'), 1, 64);
        Last := FStore.Append(Doc, Client, Copy(Claims.Get('sub', ''), 1, 200), Data);
        Notify(Doc);
        if FCompact then
          CompactLater(Doc);
        Reply(AResponse, 200, Format('{"seq": %d}', [Last]), 'application/json');
      end
      else if (Act = '/updates') and (ARequest.Method = 'GET') then
      begin
        Authorize(False);
        After := StrToInt64Def(ARequest.QueryFields.Values['after'], -1);
        Wait := StrToFloatDef(ARequest.QueryFields.Values['wait'], 0, DefaultFormatSettings);
        if ARequest.QueryFields.Values['after'] = '' then
          After := 0;
        if (After < 0) or (Wait < 0) then
          Refuse(400, 'bad after or wait');
        if Wait > 55 then
          Wait := 55;
        Data := FStore.After(Doc, After, MAX_BATCH, Last);
        if (Data = '') and (Wait > 0) then
        begin
          Waiter.Doc := Doc;
          Waiter.Event := TEvent.Create(nil, True, False, '');
          FLock.Enter;
          FWaiters.Add(@Waiter);
          FLock.Leave;
          try
            Deadline := NowUnix + Wait;
            repeat
              { looked again after registering: an update in between is not missed }
              Data := FStore.After(Doc, After, MAX_BATCH, Last);
              T := Deadline - NowUnix;
              if (Data <> '') or (T <= 0) or FStopping then
                Break;
              Waiter.Event.WaitFor(Round(T * 1000) + 1);
              Waiter.Event.ResetEvent;
            until False;
          finally
            FLock.Enter;
            FWaiters.Remove(@Waiter);
            FLock.Leave;
            Waiter.Event.Free;
          end;
        end;
        AResponse.SetCustomHeader('X-Last-Seq', IntToStr(Last));
        Reply(AResponse, 200, Data, 'application/octet-stream');
      end
      else if (Act = '/presence') and (ARequest.Method = 'POST') then
      begin
        Authorize(False);
        Body := ARequest.Content;
        if Trim(Body) = '' then
          Body := '{}';
        try
          J := GetJSON(Body);
        except
          J := nil;
        end;
        if not (J is TJSONObject) then
        begin
          J.Free;
          Refuse(400, 'bad presence');
        end;
        Dest := TJSONArray.Create;
        try
          Client := Copy(TJSONObject(J).Get('client', ''), 1, 64);
          Prefix := Doc + #0;
          T := NowUnix;
          FLock.Enter;
          try
            if Client <> '' then
            begin
              Keep := TJSONObject.Create;
              for Key in ['client', 'name', 'color', 'key', 'offset', 'anchor_key', 'anchor_offset'] do
                if TJSONObject(J).Find(Key) <> nil then
                  Keep.Add(Key, TJSONObject(J).Elements[Key].Clone);
              Keep.Strings['client'] := Client;
              Keep.Add('user', Claims.Get('sub', ''));
              I := FPresence.IndexOf(Prefix + Client);
              if I < 0 then
                I := FPresence.AddObject(Prefix + Client, TPresence.Create);
              Pr := TPresence(FPresence.Objects[I]);
              Pr.At := T;
              Pr.Json := Keep.AsJSON;
              Keep.Free;
            end;
            for I := FPresence.Count - 1 downto 0 do
              if T - TPresence(FPresence.Objects[I]).At > PRESENCE_TTL then
                FPresence.Delete(I);
            for I := 0 to FPresence.Count - 1 do
              if (Copy(FPresence[I], 1, Length(Prefix)) = Prefix) and (FPresence[I] <> Prefix + Client) then
                Dest.Add(GetJSON(TPresence(FPresence.Objects[I]).Json));
          finally
            FLock.Leave;
          end;
          Reply(AResponse, 200, Dest.AsJSON, 'application/json');
        finally
          Dest.Free;
          J.Free;
        end;
      end
      else if (Act = '/info') and (ARequest.Method = 'GET') then
      begin
        Authorize(False);
        Reply(AResponse, 200, FStore.Info(Doc), 'application/json');
      end
      else
        Refuse(404, 'not found');
    except
      on E: EReply do
        Reply(AResponse, E.Code, E.Message);
      on E: Exception do
      begin
        Log('request failed: ' + E.Message);
        Reply(AResponse, 500, 'relay error');
      end;
    end;
  finally
    Claims.Free;
    if GetEnvironmentVariable('PARADE_RELAY_TRACE') <> '' then
      Log(Format('< %s %s %d %d', [ARequest.Method, ARequest.URI, AResponse.Code, AResponse.ContentLength]));
  end;
end;

end.
