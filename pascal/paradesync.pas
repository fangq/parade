{ Parade collaboration for TParadeEdit: a document shared through a relay
  (tools/parade_relay.py).

  Needs Parade built with SYNC=yrs: link the libparade.a that has pd_sync,
  and libyrs.a (-Fl<their directory>).

  Each editor's document is bound to a pd_sync. What it writes goes into an
  outbox that one thread sends to the relay in order, retrying until it is
  taken, so nothing typed while the network is down is lost; another thread
  reads the relay's log after the last update it has, waiting on a long poll
  for new ones, and the main thread merges what arrives into the document.
  Carets go both ways as presence, and the others' are drawn in the editor
  with their names. Undo in the editor undoes this editor's own edits only.

  With OutboxDir set, the outbox is also kept on disk (one file per server,
  document and user, rewritten whole and renamed into place), so what was
  typed offline outlives the editor: joining the same document later merges
  it back in and sends it. }

unit paradesync;

{$mode objfpc}{$H+}
{$PACKRECORDS C}

interface

uses
  Classes, SysUtils, SyncObjs, ExtCtrls, ctypes, fphttpclient, ssockets, sockets, fpjson, jsonparser, base64, parade,
  paradeedit;

{$LINKLIB yrs}
{$LINKLIB pthread}
{$LINKLIB dl}
{$LINKLIB gcc_s}

type
  Ppd_sync = Pointer;
  pd_sync_send_fn = procedure(user: Pointer; update: Pointer; len: csize_t); cdecl;

function pd_sync_new(doc: Ppd_doc; client: UInt64; out sync: Ppd_sync): pd_status; cdecl; external;
procedure pd_sync_free(sync: Ppd_sync); cdecl; external;
function pd_sync_publish(sync: Ppd_sync): pd_status; cdecl; external;
procedure pd_sync_set_sender(sync: Ppd_sync; fn: pd_sync_send_fn; user: Pointer); cdecl; external;
function pd_sync_receive(sync: Ppd_sync; update: Pointer; len: csize_t): pd_status; cdecl; external;
function pd_sync_undo(sync: Ppd_sync): pd_status; cdecl; external;
function pd_sync_redo(sync: Ppd_sync): pd_status; cdecl; external;
function pd_sync_pos_share(sync: Ppd_sync; pos: pd_pos; key: PAnsiChar; cap: csize_t; out offset: UInt32): pd_status;
  cdecl; external;
function pd_sync_pos_local(sync: Ppd_sync; key: PAnsiChar; offset: UInt32; out pos: pd_pos): pd_status; cdecl; external;
function pd_sync_dump(sync: Ppd_sync; from_shared: Int32): PAnsiChar; cdecl; external;
procedure pd_sync_free_data(data: Pointer); cdecl; external;

type
  TParadeSyncState = (
    pssOff,                     { not connected to anything }
    pssOffline,                 { the relay cannot be reached: edits wait in the outbox }
    pssPending,                 { connected, edits not yet taken by the relay }
    pssSynced                   { connected, everything sent }
  );

  TParadePeer = record
    Client, Name, User: string;
    Color: UInt32;
    Key, AnchorKey: string;
    Offset, AnchorOffset: UInt32;
  end;

  TParadeSync = class;

  { What a session shares and with what: a rich document in a TParadeEdit (pd_sync), or a plain text
    in an editor of text (paradetextsync). The session -- the relay, the outbox, presence -- is the
    same for both; a target makes the shared state, merges what arrives into its editor, and says
    where its caret is. }
  TParadeSyncTarget = class
  protected
    FOwner: TParadeSync;
  public
    { an update this side made, for the relay (what a target's CRDT calls back with) }
    procedure Emit(Update: Pointer; Len: csize_t);
    { the shared state made: from what the editor has for a sharer (Publish, sent at once), empty for
      a joiner (what the relay has arrives through Receive). False and Why when it cannot be. }
    function Open(Publish: Boolean; out Why: string): Boolean; virtual; abstract;
    procedure Close; virtual; abstract;
    { an update from the relay, in its order }
    procedure Receive(Update: Pointer; Len: csize_t); virtual; abstract;
    { after a batch of them merged in }
    procedure Received; virtual;
    { a joiner caught up: everything the relay had, and the edits an earlier session left unsent }
    procedure Joined; virtual;
    { where this side's caret and the selection's other end are, as the others can find them }
    function Caret(out Key: string; out Offset: UInt32; out AnchorKey: string; out AnchorOffset: UInt32): Boolean;
      virtual; abstract;
    { the others, where they are (their positions as their Caret said) }
    procedure ShowPeers(const APeers: array of TParadePeer); virtual; abstract;
    procedure SetReadOnly(AValue: Boolean); virtual; abstract;
    { a canonical text of what is shared, for comparing replicas }
    function Dump(FromShared: Boolean): string; virtual;
    { every tick of the session's timer, before what arrived is merged: a target that finds its
      editor's edits itself sends them now }
    procedure Poll; virtual;
    property Owner: TParadeSync read FOwner;
  end;

  { An HTTP client whose request another thread can cut short.  Terminate alone
    is looked at only between reads, so a long poll would wait out the relay
    (25 seconds, or the read timeout when the relay is gone); Abort also shuts
    the connection down, which returns a read under way at once. }
  TParadeHttp = class(TFPHTTPClient)
  private
    FSockLock: TCriticalSection;
    FHandler: TSocketHandler;   { the connection being made: this thread's alone }
    FHandle: THandle;
    FLive: Boolean;             { FHandle is a connection open now }
    FAborted: Boolean;
  protected
    function GetSocketHandler(const UseSSL: Boolean): TSocketHandler; override;
    procedure ConnectToServer(const AHost: String; APort: Integer; UseSSL: Boolean = False); override;
    procedure DisconnectFromServer; override;
  public
    constructor Create(AOwner: TComponent); override;
    destructor Destroy; override;
    { from any thread: the request under way ends now, and those after it do not start }
    procedure Abort;
  end;

  TParadeSyncThread = class(TThread)
  protected
    FOwner: TParadeSync;
    FHttp: TParadeHttp;
  public
    constructor Create(AOwner: TParadeSync);
    destructor Destroy; override;
    procedure Abort;
  end;

  { TParadeSync }

  TParadeSync = class(TComponent)
  private
    FTarget: TParadeSyncTarget;
    FOpen: Boolean;             { the target's shared state exists }
    FServer, FDocName, FToken, FUserName, FClient: string;
    FColor: UInt32;
    FLock: TCriticalSection;
    FWake: TEvent;              { the sender: something to send }
    FOutbox: TStringList;       { updates not yet taken by the relay, oldest first }
    FOutboxDir, FOutboxFile: string;
    FOutboxDirty: Boolean;      { the outbox changed since it was last written }
    FInbox: TStringList;        { updates read from the relay, for the main thread }
    FLastSeq: Int64;
    FOnline: Boolean;
    FSender, FPoller: TParadeSyncThread;
    FTimer: TTimer;
    FState: TParadeSyncState;
    FOnStateChange: TNotifyEvent;
    FPresenceOut, FPresenceSent, FPeersJson, FPeersShown: string;
    FPeers: array of TParadePeer;
    FLastError: string;
    FSent, FReceived: Int64;
    procedure Tick(Sender: TObject);
    procedure SaveOutbox;
    function LoadOutbox: TStringList;
    procedure QueueUpdate(const B: RawByteString);
    function GetSync: Ppd_sync;
    procedure SetState(AState: TParadeSyncState);
    function Url(const Path: string): string;
    procedure Authorize(Http: TFPHTTPClient);
    procedure ApplyFrames(const Body: RawByteString; out Last: Int64);
    procedure UpdatePresence;
    procedure ShowPeers;
    function GetPeerCount: Integer;
    function GetPeer(I: Integer): TParadePeer;
  public
    { a session for a rich document in AEdit }
    constructor Create(AOwner: TComponent; AEdit: TParadeEdit); reintroduce;
    { a session for another target (a plain text: paradetextsync), which it owns from now on }
    constructor CreateFor(AOwner: TComponent; ATarget: TParadeSyncTarget);
    destructor Destroy; override;
    { Share the editor's document as Doc on Server (which must not have it yet: Publish), or open
      the shared Doc in the editor in place of what it shows (not Publish). False with LastError
      when the relay says no. }
    function Start(const Server, Doc, Token, UserName: string; Publish: Boolean): Boolean;
    { Leave: the document stays in the editor as it is. Edits not yet sent are lost. }
    procedure Stop;
    { a canonical text of the document, for comparing replicas }
    function Dump(FromShared: Boolean = False): string;
    property State: TParadeSyncState read FState;
    property LastError: string read FLastError;
    property OnStateChange: TNotifyEvent read FOnStateChange write FOnStateChange;
    property PeerCount: Integer read GetPeerCount;
    property Peers[I: Integer]: TParadePeer read GetPeer;
    { the rich document's pd_sync (nil for another target) }
    property Sync: Ppd_sync read GetSync;
    property Target: TParadeSyncTarget read FTarget;
    property UpdatesSent: Int64 read FSent;
    property UpdatesReceived: Int64 read FReceived;
    property Color: UInt32 read FColor;
    { where unsent updates are kept between sessions ('': in memory only); set before Start }
    property OutboxDir: string read FOutboxDir write FOutboxDir;
    { the file of the current session ('': none) }
    property OutboxFile: string read FOutboxFile;
  end;

function ParadeSyncStateName(S: TParadeSyncState): string;
{ the role a relay token gives ('viewer', 'commenter', 'editor'; '' when it cannot be read) }
function ParadeTokenRole(const Token: string): string;
{ one link that carries a relay, a document and a token, to send to whoever is to join:
  http://host:8765/d/<document>#t=<token> -- the token after the #, which a browser opening
  the link keeps to itself (the relay answers the link with a page saying how to join) }
function ParadeInviteLink(const Server, Doc, Token: string; const Kind: string = ''): string;
{ the parts of such a link; False when it is not one }
function ParadeParseInvite(const Link: string; out Server, Doc, Token: string): Boolean;
{ a document name as it goes into a URL path (UTF-8, %XX for all but A-Z a-z 0-9 - . _ ~) }
function ParadeUrlEncode(const S: string): string;
function ParadeUrlDecode(const S: string): string;
{ what an invitation link says the document is ('text': a plain text; '': a rich document) }
function ParadeInviteKind(const Link: string): string;

implementation

type
  TSenderThread = class(TParadeSyncThread)
  protected
    procedure Execute; override;
  end;

  TPollerThread = class(TParadeSyncThread)
  protected
    procedure Execute; override;
  end;

const
  PALETTE: array[0..7] of UInt32 = ($1565C0, $C2185B, $2E7D32, $E65100, $6A1B9A, $00838F, $8D6E00, $5D4037);
  LONG_POLL = 25;               { seconds a read waits on the relay for something new }

function ParadeSyncStateName(S: TParadeSyncState): string;
begin
  case S of
    pssOff: Result := 'not shared';
    pssOffline: Result := 'offline';
    pssPending: Result := 'sending';
  else
    Result := 'synced';
  end;
end;

function ParadeUrlEncode(const S: string): string;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(S) do
    if S[I] in ['A'..'Z', 'a'..'z', '0'..'9', '-', '.', '_', '~'] then
      Result := Result + S[I]
    else
      Result := Result + '%' + IntToHex(Ord(S[I]), 2);
end;

function ParadeUrlDecode(const S: string): string;
var
  I, V: Integer;
begin
  Result := '';
  I := 1;
  while I <= Length(S) do
  begin
    if (S[I] = '%') and (I + 2 <= Length(S)) and TryStrToInt('$' + Copy(S, I + 1, 2), V) then
    begin
      Result := Result + Chr(V);
      Inc(I, 3);
    end
    else
    begin
      Result := Result + S[I];
      Inc(I);
    end;
  end;
end;

function ParadeInviteLink(const Server, Doc, Token: string; const Kind: string): string;
begin
  Result := Server;
  while (Result <> '') and (Result[Length(Result)] = '/') do
    Delete(Result, Length(Result), 1);
  Result := Result + '/d/' + ParadeUrlEncode(Doc) + '#t=' + Token;
  if Kind <> '' then
    Result := Result + '&k=' + ParadeUrlEncode(Kind);    { what to open it in: a page, or a text editor }
end;

function ParadeInviteKind(const Link: string): string;
var
  P, E: Integer;
begin
  Result := '';
  P := Pos('&k=', Link);
  if P = 0 then
    Exit;
  E := P + 3;
  while (E <= Length(Link)) and (Link[E] <> '&') do
    Inc(E);
  Result := ParadeUrlDecode(Copy(Link, P + 3, E - P - 3));
end;

function ParadeParseInvite(const Link: string; out Server, Doc, Token: string): Boolean;
var
  L, Base: string;
  P, Q, I: Integer;
begin
  Result := False;
  Server := '';
  Doc := '';
  Token := '';
  L := Trim(Link);
  P := Pos('#t=', L);
  if P = 0 then
    Exit;
  Token := Copy(L, P + 3, MaxInt);
  if Pos('&', Token) > 0 then
    Token := Copy(Token, 1, Pos('&', Token) - 1);   { what follows says more about the link (its kind) }
  Base := Copy(L, 1, P - 1);
  Q := 0;     { the last /d/: a relay may sit under a path of its own }
  for I := Length(Base) - 2 downto 1 do
    if Copy(Base, I, 3) = '/d/' then
    begin
      Q := I;
      Break;
    end;
  if Q = 0 then
    Exit;
  Server := Copy(Base, 1, Q - 1);
  Doc := ParadeUrlDecode(Copy(Base, Q + 3, MaxInt));
  Result := ((Pos('http://', LowerCase(Server)) = 1) or (Pos('https://', LowerCase(Server)) = 1)) and (Doc <> '') and
    (Pos('/', Doc) = 0) and (Token <> '');
end;

function ParadeTokenRole(const Token: string): string;
var
  P1, P2: Integer;
  B: string;
  D: TJSONData;
begin
  Result := '';
  P1 := Pos('.', Token);
  if P1 = 0 then
    Exit;
  P2 := Pos('.', Token, P1 + 1);
  if P2 = 0 then
    Exit;
  B := StringReplace(StringReplace(Copy(Token, P1 + 1, P2 - P1 - 1), '-', '+', [rfReplaceAll]), '_', '/', [rfReplaceAll]);
  while Length(B) mod 4 <> 0 do
    B := B + '=';
  try
    D := GetJSON(DecodeStringBase64(B));
    try
      if D is TJSONObject then
        Result := TJSONObject(D).Get('role', '');
    finally
      D.Free;
    end;
  except
    Result := '';
  end;
end;

type
  { a rich document in a TParadeEdit, shared through pd_sync }
  TParadeRichTarget = class(TParadeSyncTarget)
  private
    FEdit: TParadeEdit;
    FSync: Ppd_sync;
    function EditUndo(Sender: TObject; Redo: Boolean): Boolean;
    procedure EditReplacing(Sender: TObject);
  public
    constructor Create(AEdit: TParadeEdit);
    function Open(Publish: Boolean; out Why: string): Boolean; override;
    procedure Close; override;
    procedure Receive(Update: Pointer; Len: csize_t); override;
    procedure Received; override;
    procedure Joined; override;
    function Caret(out Key: string; out Offset: UInt32; out AnchorKey: string; out AnchorOffset: UInt32): Boolean;
      override;
    procedure ShowPeers(const APeers: array of TParadePeer); override;
    procedure SetReadOnly(AValue: Boolean); override;
    function Dump(FromShared: Boolean): string; override;
  end;

{ TParadeSyncTarget }

procedure TParadeSyncTarget.Emit(Update: Pointer; Len: csize_t);
var
  B: RawByteString;
begin
  B := '';
  SetLength(B, Len);
  if Len > 0 then
    Move(Update^, B[1], Len);
  FOwner.QueueUpdate(B);
end;

procedure TParadeSyncTarget.Received;
begin
end;

procedure TParadeSyncTarget.Joined;
begin
end;

function TParadeSyncTarget.Dump(FromShared: Boolean): string;
begin
  Result := '';
end;

procedure TParadeSyncTarget.Poll;
begin
end;

{ C calls this from inside an edit: the update joins the outbox }
procedure OnSend(user: Pointer; update: Pointer; len: csize_t); cdecl;
begin
  TParadeSyncTarget(user).Emit(update, len);
end;

{ TParadeRichTarget }

constructor TParadeRichTarget.Create(AEdit: TParadeEdit);
begin
  inherited Create;
  FEdit := AEdit;
end;

function TParadeRichTarget.Open(Publish: Boolean; out Why: string): Boolean;
begin
  Why := '';
  if not Publish then
    FEdit.NewDocument;      { the shared document, in place of what the editor shows }
  if pd_sync_new(FEdit.Doc, 0, FSync) <> PD_OK then
  begin
    Why := 'pd_sync_new failed';
    FSync := nil;
    Exit(False);
  end;
  pd_sync_set_sender(FSync, @OnSend, Self);
  if Publish then
    pd_sync_publish(FSync);
  FEdit.OnUndo := @EditUndo;
  FEdit.OnReplacing := @EditReplacing;
  Result := True;
end;

procedure TParadeRichTarget.Close;
begin
  if FSync <> nil then
  begin
    pd_sync_free(FSync);
    FSync := nil;
  end;
  if FEdit <> nil then
  begin
    FEdit.OnUndo := nil;
    FEdit.OnReplacing := nil;
    FEdit.ReadOnly := False;
    FEdit.SetRemoteCarets([]);
  end;
end;

procedure TParadeRichTarget.Receive(Update: Pointer; Len: csize_t);
begin
  if FSync <> nil then
    pd_sync_receive(FSync, Update, Len);
end;

procedure TParadeRichTarget.Received;
begin
  FEdit.ExternalChange;
end;

procedure TParadeRichTarget.Joined;
begin
  FEdit.ExternalChange;
  FEdit.Modified := False;
end;

procedure TParadeRichTarget.EditReplacing(Sender: TObject);
begin
  FOwner.Stop;     { another document in the editor: this one is no longer shown }
end;

function TParadeRichTarget.EditUndo(Sender: TObject; Redo: Boolean): Boolean;
begin
  if FSync = nil then
    Exit(False);
  if Redo then
    Result := pd_sync_redo(FSync) = PD_OK
  else
    Result := pd_sync_undo(FSync) = PD_OK;
end;

procedure TParadeRichTarget.SetReadOnly(AValue: Boolean);
begin
  FEdit.ReadOnly := AValue;
end;

function TParadeRichTarget.Dump(FromShared: Boolean): string;
var
  P: PAnsiChar;
begin
  Result := '';
  if FSync = nil then
    Exit;
  P := pd_sync_dump(FSync, Ord(FromShared));
  if P <> nil then
  begin
    Result := P;
    pd_sync_free_data(P);
  end;
end;

function TParadeRichTarget.Caret(out Key: string; out Offset: UInt32; out AnchorKey: string;
  out AnchorOffset: UInt32): Boolean;
var
  K, AK: array[0..63] of AnsiChar;
  Off, AOff: UInt32;
begin
  Result := False;
  Key := '';
  AnchorKey := '';
  Offset := 0;
  AnchorOffset := 0;
  if (FSync = nil) or (pd_sync_pos_share(FSync, FEdit.CaretPos, @K[0], SizeOf(K), Off) <> PD_OK) then
    Exit;
  if pd_sync_pos_share(FSync, FEdit.AnchorPos, @AK[0], SizeOf(AK), AOff) <> PD_OK then
  begin
    AK := K;
    AOff := Off;
  end;
  Key := K;
  Offset := Off;
  AnchorKey := AK;
  AnchorOffset := AOff;
  Result := True;
end;

procedure TParadeRichTarget.ShowPeers(const APeers: array of TParadePeer);
var
  C: array of TParadeRemoteCaret;
  I, N: Integer;
  P, Q: pd_pos;
begin
  if FSync = nil then
    Exit;
  SetLength(C, Length(APeers));
  N := 0;
  for I := 0 to High(APeers) do
    if pd_sync_pos_local(FSync, PAnsiChar(APeers[I].Key), APeers[I].Offset, P) = PD_OK then
    begin
      if pd_sync_pos_local(FSync, PAnsiChar(APeers[I].AnchorKey), APeers[I].AnchorOffset, Q) <> PD_OK then
        Q := P;
      C[N].Pos := P;
      C[N].Anchor := Q;
      C[N].Color := APeers[I].Color;
      C[N].Name := APeers[I].Name;
      Inc(N);
    end;
  SetLength(C, N);
  FEdit.SetRemoteCarets(C);
end;

{ TParadeHttp }

constructor TParadeHttp.Create(AOwner: TComponent);
begin
  inherited Create(AOwner);
  FSockLock := TCriticalSection.Create;
end;

destructor TParadeHttp.Destroy;
begin
  inherited Destroy;
  FSockLock.Free;
end;

function TParadeHttp.GetSocketHandler(const UseSSL: Boolean): TSocketHandler;
begin
  Result := inherited GetSocketHandler(UseSSL);
  FHandler := Result;
end;

procedure TParadeHttp.ConnectToServer(const AHost: String; APort: Integer; UseSSL: Boolean);
begin
  if FAborted then
    raise ESocketError.Create('aborted');
  FHandler := nil;
  inherited ConnectToServer(AHost, APort, UseSSL);
  FSockLock.Enter;
  try
    if (FHandler <> nil) and (FHandler.Socket <> nil) then
    begin
      FHandle := FHandler.Socket.Handle;
      FLive := True;
      if FAborted then      { aborted while it connected }
        fpshutdown(FHandle, 2);
    end;
  finally
    FSockLock.Leave;
  end;
end;

procedure TParadeHttp.DisconnectFromServer;
begin
  FSockLock.Enter;
  try
    FLive := False;     { not shut down by Abort once it may be closed and its number reused }
  finally
    FSockLock.Leave;
  end;
  inherited DisconnectFromServer;
end;

procedure TParadeHttp.Abort;
begin
  FSockLock.Enter;
  try
    FAborted := True;
    Terminate;
    if FLive then
      fpshutdown(FHandle, 2);   { both ways: the read under way returns }
  finally
    FSockLock.Leave;
  end;
end;

{ TParadeSyncThread }

constructor TParadeSyncThread.Create(AOwner: TParadeSync);
begin
  FOwner := AOwner;
  FHttp := TParadeHttp.Create(nil);
  FHttp.ConnectTimeout := 5000;
  FHttp.IOTimeout := (LONG_POLL + 15) * 1000;
  FHttp.KeepConnection := False;   { a connection kept to a relay that went away would fail every request after }
  inherited Create(False);
end;

destructor TParadeSyncThread.Destroy;
begin
  FHttp.Free;
  inherited Destroy;
end;

procedure TParadeSyncThread.Abort;
begin
  Terminate;
  FHttp.Abort;          { a long poll under way returns now }
end;

{ the outbox, oldest first, to the relay; presence alongside }
procedure TSenderThread.Execute;
var
  Item, Body: RawByteString;
  Have: Boolean;
  Req, Resp: TStringStream;
  Backoff: Integer;
  LastPresence: QWord;
begin
  Backoff := 0;
  LastPresence := 0;
  while not Terminated do
  begin
    FOwner.SaveOutbox;      { on disk before it is sent, and again once the relay took it }
    FOwner.FLock.Enter;
    try
      Have := FOwner.FOutbox.Count > 0;
      if Have then
        Item := FOwner.FOutbox[0];
      Body := FOwner.FPresenceOut;
    finally
      FOwner.FLock.Leave;
    end;
    try
      if Have then
      begin
        Req := TStringStream.Create(Item);
        Resp := TStringStream.Create('');
        try
          FHttp.RequestHeaders.Clear;
          FOwner.Authorize(FHttp);
          FHttp.AddHeader('X-Client', FOwner.FClient);
          FHttp.AddHeader('Content-Type', 'application/octet-stream');
          FHttp.RequestBody := Req;
          FHttp.HTTPMethod('POST', FOwner.Url('/updates'), Resp, [200]);
        finally
          FHttp.RequestBody := nil;
          Req.Free;
          Resp.Free;
        end;
        FOwner.FLock.Enter;
        try
          if (FOwner.FOutbox.Count > 0) and (FOwner.FOutbox[0] = Item) then
          begin
            FOwner.FOutbox.Delete(0);
            FOwner.FOutboxDirty := True;
          end;
          Inc(FOwner.FSent);
          FOwner.FOnline := True;
        finally
          FOwner.FLock.Leave;
        end;
        Backoff := 0;
        Continue;
      end;
      if (Body <> '') and ((Body <> FOwner.FPresenceSent) or (GetTickCount64 - LastPresence > 10000)) then
      begin
        Req := TStringStream.Create(Body);
        Resp := TStringStream.Create('');
        try
          FHttp.RequestHeaders.Clear;
          FOwner.Authorize(FHttp);
          FHttp.AddHeader('Content-Type', 'application/json');
          FHttp.RequestBody := Req;
          FHttp.HTTPMethod('POST', FOwner.Url('/presence'), Resp, [200]);
          FOwner.FLock.Enter;
          try
            FOwner.FPeersJson := Resp.DataString;
            FOwner.FPresenceSent := Body;
          finally
            FOwner.FLock.Leave;
          end;
          LastPresence := GetTickCount64;
        finally
          FHttp.RequestBody := nil;
          Req.Free;
          Resp.Free;
        end;
      end;
      FOwner.FWake.WaitFor(1000);
      FOwner.FWake.ResetEvent;
    except
      on E: Exception do
      begin
        if Terminated then
          Break;
        if Have and (FHttp.ResponseStatusCode = 403) then
        begin   { not let write (a viewer's token): dropped, not retried }
          FOwner.FLock.Enter;
          try
            if (FOwner.FOutbox.Count > 0) and (FOwner.FOutbox[0] = Item) then
            begin
              FOwner.FOutbox.Delete(0);
              FOwner.FOutboxDirty := True;
            end;
            FOwner.FLastError := 'read only: ' + E.Message;
          finally
            FOwner.FLock.Leave;
          end;
          Continue;
        end;
        FOwner.FLock.Enter;
        try
          FOwner.FOnline := False;
          FOwner.FLastError := E.Message;
        finally
          FOwner.FLock.Leave;
        end;
        { waits longer each time, up to half a minute, and tries again: the outbox keeps everything }
        if Backoff < 6 then
          Inc(Backoff);
        FOwner.FWake.WaitFor(250 shl Backoff);
        FOwner.FWake.ResetEvent;
      end;
    end;
  end;
end;

{ the relay's log, read on from the last update this replica has }
procedure TPollerThread.Execute;
var
  Resp: TStringStream;
  Backoff: Integer;
  Last: Int64;
  Body: RawByteString;
  Idle: TEvent;
begin
  Backoff := 0;
  Idle := TEvent.Create(nil, True, False, '');
  try
    while not Terminated do
      try
        Resp := TStringStream.Create('');
        try
          FHttp.RequestHeaders.Clear;
          FOwner.Authorize(FHttp);
          FHttp.HTTPMethod('GET', FOwner.Url('/updates?after=' + IntToStr(FOwner.FLastSeq) + '&wait=' +
            IntToStr(LONG_POLL)), Resp, [200]);
          Body := Resp.DataString;
        finally
          Resp.Free;
        end;
        Last := FOwner.FLastSeq;
        if Body <> '' then
        begin
          FOwner.FLock.Enter;
          try
            FOwner.FInbox.Add(Body);
          finally
            FOwner.FLock.Leave;
          end;
          { the newest sequence number: the last frame's }
          FOwner.ApplyFrames(Body, Last);
        end;
        FOwner.FLastSeq := Last;
        FOwner.FOnline := True;
        Backoff := 0;
      except
        on E: Exception do
        begin
          if Terminated then
            Break;
          FOwner.FOnline := False;
          FOwner.FLastError := E.Message;
          if Backoff < 6 then
            Inc(Backoff);
          Idle.WaitFor(250 shl Backoff);
        end;
      end;
  finally
    Idle.Free;
  end;
end;

{ TParadeSync }

constructor TParadeSync.Create(AOwner: TComponent; AEdit: TParadeEdit);
begin
  CreateFor(AOwner, TParadeRichTarget.Create(AEdit));
end;

constructor TParadeSync.CreateFor(AOwner: TComponent; ATarget: TParadeSyncTarget);
begin
  inherited Create(AOwner);
  FTarget := ATarget;
  FTarget.FOwner := Self;
  FLock := TCriticalSection.Create;
  FWake := TEvent.Create(nil, True, False, '');
  FOutbox := TStringList.Create;
  FInbox := TStringList.Create;
  FTimer := TTimer.Create(Self);
  FTimer.Interval := 40;
  FTimer.Enabled := False;
  FTimer.OnTimer := @Tick;
end;

destructor TParadeSync.Destroy;
begin
  Stop;
  FTarget.Free;
  FOutbox.Free;
  FInbox.Free;
  FWake.Free;
  FLock.Free;
  inherited Destroy;
end;

{ an update for the relay: into the outbox, the sender woken }
procedure TParadeSync.QueueUpdate(const B: RawByteString);
begin
  FLock.Enter;
  try
    FOutbox.Add(B);
    FOutboxDirty := True;
  finally
    FLock.Leave;
  end;
  FWake.SetEvent;
end;

function TParadeSync.GetSync: Ppd_sync;
begin
  if FTarget is TParadeRichTarget then
    Result := TParadeRichTarget(FTarget).FSync
  else
    Result := nil;
end;

function TParadeSync.Url(const Path: string): string;
var
  S: string;
begin
  S := FServer;
  while (S <> '') and (S[Length(S)] = '/') do
    Delete(S, Length(S), 1);
  Result := S + '/d/' + ParadeUrlEncode(FDocName) + Path;
end;

procedure TParadeSync.Authorize(Http: TFPHTTPClient);
begin
  if FToken <> '' then
    Http.AddHeader('Authorization', 'Bearer ' + FToken);
end;

{ the frames of a read ([seq u64][length u32][bytes], big-endian) into the document when this is the
   main thread (Last: the newest number); from the poller, only Last is wanted }
procedure TParadeSync.ApplyFrames(const Body: RawByteString; out Last: Int64);
var
  I, N: Integer;
  Seq: QWord;
  Len: Cardinal;
  K: Integer;
begin
  Last := FLastSeq;
  I := 1;
  N := Length(Body);
  while I + 11 <= N do
  begin
    Seq := 0;
    for K := 0 to 7 do
      Seq := (Seq shl 8) or Byte(Body[I + K]);
    Len := 0;
    for K := 8 to 11 do
      Len := (Len shl 8) or Byte(Body[I + K]);
    if I + 12 + Int64(Len) - 1 > N then
      Break;
    if (MainThreadID = GetCurrentThreadId) and FOpen then
    begin
      FTarget.Receive(@Body[I + 12], Len);
      Inc(FReceived);
    end;
    Last := Seq;
    Inc(I, 12 + Len);
  end;
end;

const
  OUTBOX_MAGIC = 'PDOUTBOX1'#10;

{ the outbox to its file, if it changed: written whole beside it, then renamed over it, so a crash
  leaves the old file or the new one; no file when it is empty. Called by the sender thread, and by
  Stop once that is gone. }
procedure TParadeSync.SaveOutbox;
var
  Data, Tmp: RawByteString;
  Empty: Boolean;
  F: TFileStream;
  I: Integer;
  N: Cardinal;
begin
  if FOutboxFile = '' then
    Exit;
  FLock.Enter;
  try
    if not FOutboxDirty then
      Exit;
    FOutboxDirty := False;
    Empty := FOutbox.Count = 0;
    Data := OUTBOX_MAGIC;
    for I := 0 to FOutbox.Count - 1 do
    begin
      N := Length(FOutbox[I]);
      Data := Data + Chr(N shr 24) + Chr((N shr 16) and $FF) + Chr((N shr 8) and $FF) + Chr(N and $FF) + FOutbox[I];
    end;
  finally
    FLock.Leave;
  end;
  try
    if Empty then
    begin
      if FileExists(FOutboxFile) and not DeleteFile(FOutboxFile) then
        raise EInOutError.Create('cannot delete ' + FOutboxFile);
      Exit;
    end;
    ForceDirectories(ExtractFileDir(FOutboxFile));
    Tmp := FOutboxFile + '.tmp';
    F := TFileStream.Create(Tmp, fmCreate);
    try
      F.WriteBuffer(Data[1], Length(Data));
      FileFlush(F.Handle);
    finally
      F.Free;
    end;
    if not RenameFile(Tmp, FOutboxFile) then
      raise EInOutError.Create('cannot write ' + FOutboxFile);
  except
    on E: Exception do
    begin
      FLock.Enter;
      try
        FOutboxDirty := True;     { tried again next time }
        FLastError := 'outbox: ' + E.Message;
      finally
        FLock.Leave;
      end;
    end;
  end;
end;

{ the updates a previous session left unsent (nil: none, or not a readable outbox) }
function TParadeSync.LoadOutbox: TStringList;
var
  Data: RawByteString;
  F: TFileStream;
  I: Integer;
  N: Cardinal;
begin
  Result := nil;
  if (FOutboxFile = '') or not FileExists(FOutboxFile) then
    Exit;
  try
    F := TFileStream.Create(FOutboxFile, fmOpenRead or fmShareDenyWrite);
    try
      SetLength(Data, F.Size);
      if Length(Data) > 0 then
        F.ReadBuffer(Data[1], Length(Data));
    finally
      F.Free;
    end;
  except
    Exit;
  end;
  if Copy(Data, 1, Length(OUTBOX_MAGIC)) <> OUTBOX_MAGIC then
    Exit;
  Result := TStringList.Create;
  I := Length(OUTBOX_MAGIC) + 1;
  while I + 3 <= Length(Data) do
  begin
    N := Cardinal(Byte(Data[I])) shl 24 or Cardinal(Byte(Data[I + 1])) shl 16 or Cardinal(Byte(Data[I + 2])) shl 8 or
      Byte(Data[I + 3]);
    if I + 4 + Int64(N) - 1 > Length(Data) then
      Break;      { cut short: the whole records before it are kept }
    Result.Add(Copy(Data, I + 4, N));
    Inc(I, 4 + N);
  end;
end;

function OutboxName(const Server, Doc, User: string): string;
var
  I: Integer;
begin
  Result := Server + '_' + Doc + '_' + User;
  for I := 1 to Length(Result) do
    if not (Result[I] in ['A'..'Z', 'a'..'z', '0'..'9', '-', '.']) then
      Result[I] := '_';
  Result := Result + '.outbox';
end;

function TParadeSync.Start(const Server, Doc, Token, UserName: string; Publish: Boolean): Boolean;
var
  Http: TFPHTTPClient;
  Resp: TStringStream;
  Body, Chunk: RawByteString;
  Last: Int64;
  H: Cardinal;
  I: Integer;
  Saved: TStringList;
begin
  Result := False;
  Stop;
  FServer := Server;
  FDocName := Doc;
  FToken := Token;
  FUserName := UserName;
  FClient := IntToHex(Random($7FFFFFFF), 8) + IntToHex(Random($7FFFFFFF), 8);
  H := 5381;
  for I := 1 to Length(UserName) do
    H := H * 33 + Ord(UserName[I]);
  FColor := PALETTE[H mod 8];
  FLastError := '';
  { what the relay has of it already, all of it (a read returns a few megabytes at most) }
  Body := '';
  FLastSeq := 0;
  Http := TFPHTTPClient.Create(nil);
  Resp := TStringStream.Create('');
  try
    try
      Http.ConnectTimeout := 5000;
      Http.IOTimeout := 30000;
      Authorize(Http);
      repeat
        FreeAndNil(Resp);
        Resp := TStringStream.Create('');
        Http.HTTPMethod('GET', Url('/updates?after=' + IntToStr(FLastSeq)), Resp, [200]);
        Chunk := Resp.DataString;
        Body := Body + Chunk;
        ApplyFrames(Chunk, Last);     { not open yet: only the numbers }
        FLastSeq := Last;
      until (Chunk = '') or Publish;
    except
      on E: Exception do
      begin
        FLastError := E.Message;
        Exit;
      end;
    end;
  finally
    Resp.Free;
    Http.Free;
  end;
  if Publish and (Body <> '') then
  begin
    FLastError := 'the relay has "' + Doc + '" already: join it instead';
    Exit;
  end;
  FLastSeq := 0;
  FOutboxDirty := False;
  FOutboxFile := '';
  if (FOutboxDir <> '') and (ParadeTokenRole(Token) <> 'viewer') then
    FOutboxFile := IncludeTrailingPathDelimiter(FOutboxDir) + OutboxName(Server, Doc, UserName);
  if Publish and (FOutboxFile <> '') then
    DeleteFile(FOutboxFile);    { left from an earlier document of this name: not this one }
  if not FTarget.Open(Publish, FLastError) then
    Exit;
  FOpen := True;
  if not Publish then
  begin
    ApplyFrames(Body, Last);
    FLastSeq := Last;
    { what an earlier session typed and could not send: into the document, and on to the relay }
    Saved := LoadOutbox;
    if Saved <> nil then
    try
      for I := 0 to Saved.Count - 1 do
        if Saved[I] <> '' then
          FTarget.Receive(@Saved[I][1], Length(Saved[I]));
      FLock.Enter;
      try
        for I := 0 to Saved.Count - 1 do
          FOutbox.Insert(I, Saved[I]);    { before anything the merge itself wrote }
        FOutboxDirty := True;
      finally
        FLock.Leave;
      end;
    finally
      Saved.Free;
    end;
    FTarget.Joined;
  end;
  FTarget.SetReadOnly(ParadeTokenRole(Token) = 'viewer');    { the relay would refuse its edits anyway }
  FOnline := True;
  FSender := TSenderThread.Create(Self);
  FPoller := TPollerThread.Create(Self);
  FTimer.Enabled := True;
  SetState(pssPending);
  Result := True;
end;

procedure TParadeSync.Stop;
begin
  FTimer.Enabled := False;
  if FSender <> nil then
  begin
    FSender.Abort;
    FWake.SetEvent;
    FPoller.Abort;
    FSender.WaitFor;
    FPoller.WaitFor;
    FreeAndNil(FSender);
    FreeAndNil(FPoller);
  end;
  SaveOutbox;     { what is still unsent stays on disk for the next session; none: no file }
  FOutboxFile := '';
  if FOpen then
  begin
    FOpen := False;
    FTarget.Close;
  end;
  FOutbox.Clear;
  FInbox.Clear;
  SetLength(FPeers, 0);
  FPeersJson := '';
  FPeersShown := '';
  FPresenceOut := '';
  FPresenceSent := '';
  SetState(pssOff);
end;

procedure TParadeSync.SetState(AState: TParadeSyncState);
begin
  if AState = FState then
    Exit;
  FState := AState;
  if Assigned(FOnStateChange) then
    FOnStateChange(Self);
end;

function TParadeSync.Dump(FromShared: Boolean): string;
begin
  if FOpen then
    Result := FTarget.Dump(FromShared)
  else
    Result := '';
end;

procedure TParadeSync.UpdatePresence;
var
  Key, AKey: string;
  Off, AOff: UInt32;
  J: TJSONObject;
  S: string;
begin
  if not FTarget.Caret(Key, Off, AKey, AOff) then
    Exit;
  J := TJSONObject.Create(['client', FClient, 'name', FUserName, 'color', Int64(FColor), 'key', Key,
    'offset', Int64(Off), 'anchor_key', AKey, 'anchor_offset', Int64(AOff)]);
  try
    S := J.AsJSON;
  finally
    J.Free;
  end;
  FLock.Enter;
  try
    if S <> FPresenceOut then
    begin
      FPresenceOut := S;
      FWake.SetEvent;
    end;
  finally
    FLock.Leave;
  end;
end;

procedure TParadeSync.ShowPeers;
var
  Js: string;
  D: TJSONData;
  A: TJSONArray;
  O: TJSONObject;
  I: Integer;
begin
  FLock.Enter;
  try
    Js := FPeersJson;
  finally
    FLock.Leave;
  end;
  if Js = '' then
    Exit;
  { the positions may mean something new now even when the list is the same (the text moved) }
  try
    D := GetJSON(Js);
  except
    Exit;
  end;
  try
    if not (D is TJSONArray) then
      Exit;
    A := TJSONArray(D);
    SetLength(FPeers, A.Count);
    for I := 0 to A.Count - 1 do
      if A[I] is TJSONObject then
      begin
        O := TJSONObject(A[I]);
        FPeers[I].Client := O.Get('client', '');
        FPeers[I].Name := O.Get('name', '');
        FPeers[I].User := O.Get('user', '');
        FPeers[I].Color := UInt32(O.Get('color', Int64(0)));
        FPeers[I].Key := O.Get('key', '');
        FPeers[I].Offset := UInt32(O.Get('offset', Int64(0)));
        FPeers[I].AnchorKey := O.Get('anchor_key', '');
        FPeers[I].AnchorOffset := UInt32(O.Get('anchor_offset', Int64(0)));
      end;
    FTarget.ShowPeers(FPeers);
  finally
    D.Free;
  end;
end;

procedure TParadeSync.Tick(Sender: TObject);
var
  L: TStringList;
  I: Integer;
  Last: Int64;
  Online, Pending, Applied: Boolean;
  PeersNow: string;
begin
  if not FOpen then
    Exit;
  FTarget.Poll;     { the editor's own edits out first: what arrives is merged on top of them }
  Applied := False;
  { what was read, merged in }
  L := TStringList.Create;
  try
    FLock.Enter;
    try
      L.Assign(FInbox);
      FInbox.Clear;
      Online := FOnline;
      Pending := FOutbox.Count > 0;
      PeersNow := FPeersJson;
    finally
      FLock.Leave;
    end;
    for I := 0 to L.Count - 1 do
      ApplyFrames(L[I], Last);
    Applied := L.Count > 0;
    if Applied then
      FTarget.Received;
  finally
    FreeAndNil(L);
  end;
  UpdatePresence;
  if (PeersNow <> FPeersShown) or ((PeersNow <> '') and Applied) then   { the text moved, or they did }
  begin
    FPeersShown := PeersNow;
    ShowPeers;
  end;
  if not Online then
    SetState(pssOffline)
  else if Pending then
    SetState(pssPending)
  else
    SetState(pssSynced);
end;

function TParadeSync.GetPeerCount: Integer;
begin
  Result := Length(FPeers);
end;

function TParadeSync.GetPeer(I: Integer): TParadePeer;
begin
  Result := FPeers[I];
end;

initialization
  Randomize;
end.
