{ Two editors sharing a document through the relay (tools/parade_relay.py),
  driven headless: share and join, typing on each side and at once, the
  others' carets, undo of one's own edits, the relay going away and
  coming back with edits made meanwhile, edits kept on disk across a quit,
  and joining from a compacted log. Run under a display (xvfb-run);
  arguments: the relay -- parade_relay.py and pd_compact, the Pascal
  relay (pascal/relay/parade_relay.lpr) alone, or "inproc": a TParadeRelay
  in this program, as an editor hosting the document runs it. }
program sync_test;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads,{$ENDIF}
  Interfaces, Forms, Controls, Graphics, LCLType, SysUtils, Classes, Process, FileUtil, fphttpclient, fpjson, jsonparser,
  parade, paradeedit, paradesync, paraderelay, paradetextsync;

type
  TFunc = function: Boolean;

  TLogger = class
    procedure Log(Sender: TObject; const Msg: string);
  end;

procedure TLogger.Log(Sender: TObject; const Msg: string);
begin
  WriteLn(StdErr, 'relay: ', Msg);
end;

var
  Failures: Integer = 0;
  Checks: Integer = 0;
  Relay: TProcess;
  InProc: TParadeRelay;
  Logger: TLogger;
  RelayPy, Compactor, Dir, SecretFile, DbFile, Url, OutboxDir: string;
  Port: Integer;

procedure Step(const S: string);
begin
  WriteLn(StdErr, '- ', S);
  Flush(StdErr);
end;

procedure Check(Cond: Boolean; const What: string);
begin
  Inc(Checks);
  if not Cond then
  begin
    Inc(Failures);
    WriteLn('CHECK failed: ', What);
  end;
end;

function IsPython: Boolean;
begin
  Result := ExtractFileExt(RelayPy) = '.py';
end;

{ the relay's command line tool: Args after the script (Python) or as they are (Pascal) }
function Run(const Args: array of string): string;
var
  A: array of string;
  I: Integer;
begin
  if RelayPy = 'inproc' then
  begin   { secret, or token --secret-file F --user U --doc D [--role R] }
    if Args[0] = 'secret' then
      Exit(ParadeNewSecret);
    if Length(Args) > 7 then
      Result := Args[8]
    else
      Result := 'editor';
    Exit(ParadeMakeToken(ParadeReadSecret(Args[2]), Args[4], Args[6], Result, 1));
  end;
  if IsPython then
  begin
    SetLength(A, Length(Args) + 1);
    A[0] := RelayPy;
    for I := 0 to High(Args) do
      A[I + 1] := Args[I];
    if not RunCommand('python3', A, Result, [poStderrToOutPut]) then
      raise Exception.Create('python3 failed: ' + Result);
  end
  else if not RunCommand(RelayPy, Args, Result, [poStderrToOutPut]) then
    raise Exception.Create(RelayPy + ' failed: ' + Result);
  Result := Trim(Result);
end;

procedure StartRelay;
begin
  if RelayPy = 'inproc' then
  begin
    InProc := TParadeRelay.Create(nil);
    InProc.DbFile := DbFile;
    InProc.Secret := ParadeReadSecret(SecretFile);
    InProc.Port := Port;
    InProc.CompactEvery := 5;
    if Logger = nil then
      Logger := TLogger.Create;
    InProc.OnLog := @Logger.Log;
    if not InProc.Start then
      raise Exception.Create('relay: ' + InProc.LastError);
    Exit;
  end;
  Relay := TProcess.Create(nil);
  if IsPython then
  begin
    Relay.Executable := 'python3';
    Relay.Parameters.Add(RelayPy);
  end
  else
    Relay.Executable := RelayPy;
  Relay.Parameters.Add('serve');
  Relay.Parameters.Add('--db');
  Relay.Parameters.Add(DbFile);
  Relay.Parameters.Add('--secret-file');
  Relay.Parameters.Add(SecretFile);
  Relay.Parameters.Add('--port');
  Relay.Parameters.Add(IntToStr(Port));
  if IsPython then
  begin
    Relay.Parameters.Add('--compactor');
    Relay.Parameters.Add(Compactor);
  end;
  Relay.Parameters.Add('--compact-every');
  Relay.Parameters.Add('5');
  Relay.Options := [poNoConsole];
  Relay.Execute;
  Sleep(1500);
end;

procedure StopRelay;
begin
  FreeAndNil(InProc);
  if Relay <> nil then
  begin
    Relay.Terminate(0);
    Relay.WaitOnExit;
    FreeAndNil(Relay);
  end;
end;

{ pump the message loop until a condition holds or the time is up }
function WaitFor(Cond: TFunc; Seconds: Double): Boolean;
var
  T0: QWord;
begin
  T0 := GetTickCount64;
  repeat
    Application.ProcessMessages;
    if Cond() then
      Exit(True);
    Sleep(20);
  until GetTickCount64 - T0 > Round(Seconds * 1000);
  Result := Cond();
end;

var
  Form: TForm;
  A, B: TParadeEdit;
  SA, SB: TParadeSync;
  TokA, TokB, TokV, TokA2, TokB2: string;
  I: Integer;
  C: pd_pos;
  J: TJSONObject;
  F, Server, DocName, Token: string;
  T0, T1, T2: QWord;

function Same: Boolean;
begin
  Result := (SA.Dump = SB.Dump) and (SA.Dump <> '') and (A.DocumentText = B.DocumentText);
end;

function BothSynced: Boolean;
begin
  Result := (SA.State = pssSynced) and (SB.State = pssSynced) and Same;
end;

function SeesBob: Boolean;
begin
  Result := (SA.PeerCount = 1) and (SA.Peers[0].Name = 'Bob');
end;

var
  TA, TB: TParadeSync;
  LA, LB: TStringList;
  Wide: string;

function TextsSame: Boolean;
begin
  Result := (TA.State = pssSynced) and (TB.State = pssSynced) and (LA.Text = LB.Text);
end;

function BobCaretSeen: Boolean;
begin
  with TParadeStringsTarget(TA.Target) do
    Result := (PeerCarets = 1) and (PeerCaret(0).Line = 2) and (PeerCaret(0).Col = 4);
end;

function OutboxGone: Boolean;
begin
  Result := not FileExists(SA.OutboxFile) and (SA.State = pssSynced);
end;

{ what the relay says of the document's log }
function Info(const Token: string): TJSONObject;
var
  H: TFPHTTPClient;
begin
  H := TFPHTTPClient.Create(nil);
  try
    H.AddHeader('Authorization', 'Bearer ' + Token);
    Result := GetJSON(H.Get(Url + '/d/proposal/info')) as TJSONObject;
  finally
    H.Free;
  end;
end;

function ASynced: Boolean;
begin
  Result := SA.State = pssSynced;
end;

function AOffline: Boolean;
begin
  Result := SA.State = pssOffline;
end;

procedure Fail(E: Exception);
begin
  WriteLn(StdErr, 'exception: ', E.ClassName, ': ', E.Message);
  DumpExceptionBacktrace(StdErr);
  StopRelay;
  Halt(2);
end;

begin
  Application.Initialize;
  try
    RelayPy := ParamStr(1);
    if RelayPy <> 'inproc' then
      RelayPy := ExpandFileName(RelayPy);
    Compactor := ExpandFileName(ParamStr(2));
    Dir := ExtractFilePath(ParamStr(0));
    OutboxDir := Dir + 'sync_test.outbox';
    DeleteDirectory(OutboxDir, False);
    SecretFile := Dir + 'sync_test.secret';
    DbFile := Dir + 'sync_test.sqlite';
    DeleteFile(DbFile);
    DeleteFile(DbFile + '-wal');
    DeleteFile(DbFile + '-shm');
    with TStringList.Create do
    try
      Text := Run(['secret']);
      SaveToFile(SecretFile);
    finally
      Free;
    end;
    Port := 18000 + Random(20000);
    Url := 'http://127.0.0.1:' + IntToStr(Port);
    TokA := Run(['token', '--secret-file', SecretFile, '--user', 'ann', '--doc', 'proposal']);
    TokB := Run(['token', '--secret-file', SecretFile, '--user', 'bob', '--doc', 'proposal']);
    TokA2 := Run(['token', '--secret-file', SecretFile, '--user', 'ann', '--doc', 'hello.pas']);
    TokB2 := Run(['token', '--secret-file', SecretFile, '--user', 'bob', '--doc', 'hello.pas']);
    TokV := Run(['token', '--secret-file', SecretFile, '--user', 'vic', '--doc', 'proposal', '--role', 'viewer']);
    Step('relay');
    StartRelay;

    Form := TForm.CreateNew(nil);
    Form.SetBounds(0, 0, 1400, 900);
    A := TParadeEdit.Create(Form);
    A.Parent := Form;
    A.SetBounds(0, 0, 700, 900);
    B := TParadeEdit.Create(Form);
    B.Parent := Form;
    B.SetBounds(700, 0, 700, 900);
    A.AddDefaultFonts;
    B.AddDefaultFonts;
    Form.Show;
    Application.ProcessMessages;
    SA := TParadeSync.Create(Form, A);
    SB := TParadeSync.Create(Form, B);
    SA.OutboxDir := OutboxDir;

    { 0. invitation links }
    Step('invitation links');
    F := ParadeInviteLink(Url + '/', 'grant proposal (v2)', TokB);
    Check(F = Url + '/d/grant%20proposal%20%28v2%29#t=' + TokB, 'a link: ' + F);
    Check(ParadeParseInvite('  ' + F + #10, Server, DocName, Token) and (Server = Url) and
      (DocName = 'grant proposal (v2)') and (Token = TokB), 'and back: ' + Server + ' ' + DocName);
    Check(ParadeParseInvite('https://example.org/relay/d/a%2Fb/d/x#t=T', Server, DocName, Token) and
      (Server = 'https://example.org/relay/d/a%2Fb') and (DocName = 'x'), 'the last /d/ counts');
    Check(not ParadeParseInvite(Url, Server, DocName, Token), 'an address alone is not a link');
    Check(not ParadeParseInvite(Url + '/d/x#t=', Server, DocName, Token), 'nor one without a token');
    Check(not ParadeParseInvite('ftp://h/d/x#t=T', Server, DocName, Token), 'nor one not over http');

    { 1. Ann shares her document; Bob joins and gets it, from the link she sends }
    Step('share and join');
    A.InsertText('Hello from Ann.');
    Check(SA.Start(Url, 'proposal', TokA, 'Ann', True), 'share: ' + SA.LastError);
    Check(WaitFor(@ASynced, 15), 'shared: the relay has it');
    Check(not SB.Start(Url, 'proposal', TokB, 'Bob', True), 'sharing a document the relay has is refused');
    Check(ParadeParseInvite(ParadeInviteLink(Url, 'proposal', TokB), Server, DocName, Token), 'Ann''s link');
    Check(SB.Start(Server, DocName, Token, 'Bob', False), 'join: ' + SB.LastError);
    Check(WaitFor(@BothSynced, 15), 'joined: the same document');
    Check(B.DocumentText = 'Hello from Ann.', 'Bob sees Ann''s text: ' + B.DocumentText);

    { 2. typing on one side shows on the other }
    Step('typing');
    B.ProcessKey(VK_END, [ssCtrl]);
    B.InsertText(' And Bob.');
    Check(WaitFor(@BothSynced, 15), 'Bob''s typing reaches Ann');
    Check(A.DocumentText = 'Hello from Ann. And Bob.', 'Ann sees: ' + A.DocumentText);

    { 3. both at once }
    Step('concurrent');
    A.ProcessKey(VK_HOME, [ssCtrl]);
    A.InsertText('Ann: ');
    B.ProcessKey(VK_END, [ssCtrl]);
    B.ProcessKey(VK_RETURN, []);
    B.InsertText('Second paragraph by Bob.');
    Check(WaitFor(@BothSynced, 15), 'concurrent edits merge');
    Check(A.DocumentText = 'Ann: Hello from Ann. And Bob.'#10'Second paragraph by Bob.', 'merged: ' + A.DocumentText);

    { 4. Ann sees where Bob is }
    Step('presence');
    Check(WaitFor(@SeesBob, 15), 'Ann sees Bob''s caret');

    { 5. undo takes away one's own edits only }
    Step('own undo');
    A.ProcessKey(VK_Z, [ssCtrl]);
    Check(WaitFor(@BothSynced, 15), 'undo shared');
    Check(B.DocumentText = 'Hello from Ann. And Bob.'#10'Second paragraph by Bob.', 'Ann''s "Ann: " gone, Bob''s text kept: ' +
      B.DocumentText);

    { 6. the relay goes away; Ann keeps typing; it comes back and Bob gets everything }
    Step('offline');
    StopRelay;
    A.ProcessKey(VK_END, [ssCtrl]);
    for I := 1 to 3 do
      A.InsertText(' offline' + IntToStr(I));
    Check(WaitFor(@AOffline, 20), 'Ann is told she is offline');
    Check(FileExists(SA.OutboxFile), 'the unsent edits are on disk: ' + SA.OutboxFile);
    Step('back online');
    StartRelay;
    Check(WaitFor(@BothSynced, 60), 'back: what was typed offline arrives');
    Check(Pos(' offline1 offline2 offline3', B.DocumentText) > 0, 'Bob has the offline edits: ' + B.DocumentText);

    { 7. a viewer can read, and what it types never reaches the others }
    Step('viewer');
    SB.Stop;
    Check(SB.Start(Url, 'proposal', TokV, 'Vic', False), 'a viewer joins');
    Check(WaitFor(@Same, 15), 'the viewer has the document');
    Check(B.ReadOnly, 'a viewer''s editor is read-only');
    B.InsertText('Vandalism. ');
    WaitFor(@AOffline, 3);
    Check(Pos('Vandalism', B.DocumentText + A.DocumentText) = 0, 'a viewer cannot type');

    { 8. Ann types with the relay away and quits; started again, she joins and the edits go out }
    Step('outbox across a quit');
    SB.Stop;
    Check(SB.Start(Url, 'proposal', TokB, 'Bob', False), 'Bob back as an editor');
    Check(WaitFor(@BothSynced, 15), 'Bob back');
    StopRelay;
    A.ProcessKey(VK_END, [ssCtrl]);
    A.InsertText(' typed before quitting');
    Check(WaitFor(@AOffline, 20), 'offline again');
    F := SA.OutboxFile;
    SA.Free;            { the editor quits }
    Check(FileExists(F), 'what was not sent is kept: ' + F);
    A.NewDocument;
    StartRelay;
    SA := TParadeSync.Create(Form, A);
    SA.OutboxDir := OutboxDir;
    Check(SA.Start(Url, 'proposal', TokA, 'Ann', False), 'Ann joins again: ' + SA.LastError);
    Check(Pos('typed before quitting', A.DocumentText) > 0, 'her saved edits are back in her editor: ' + A.DocumentText);
    Check(WaitFor(@BothSynced, 60), 'and reach Bob');
    Check(Pos('typed before quitting', B.DocumentText) > 0, 'Bob has them: ' + B.DocumentText);
    Check(WaitFor(@OutboxGone, 15), 'sent: the file is gone');

    { 9. the log was compacted on the way, and a newcomer from the start still gets everything }
    Step('compaction');
    J := Info(TokA);
    try
      Check(J.Get('snapshot', 0) > 0, 'the relay compacted the log: ' + J.AsJSON);
    finally
      J.Free;
    end;
    SB.Stop;
    B.NewDocument;
    Check(SB.Start(Url, 'proposal', TokB, 'Bob', False), 'a fresh join');
    Check(WaitFor(@BothSynced, 15), 'from the snapshot: the same document');

    { 10. a plain text shared: a code editor's lines, not a rich document }
    Step('plain text');
    LA := TStringList.Create;
    LB := TStringList.Create;
    try
      LA.Text := 'program hello;' + LineEnding + 'begin' + LineEnding + 'end.';
      TA := TParadeSync.CreateFor(Form, TParadeStringsTarget.Create(LA));
      TB := TParadeSync.CreateFor(Form, TParadeStringsTarget.Create(LB));
      Check(TA.Start(Url, 'hello.pas', TokA2, 'Ann', True), 'share a text: ' + TA.LastError);
      Check(TB.Start(Url, 'hello.pas', TokB2, 'Bob', False), 'join it: ' + TB.LastError);
      Check(WaitFor(@TextsSame, 15), 'the joiner has the text: ' + LB.Text);
      Check(LB.Count = 3, 'three lines');
      { Bob types a line in; Ann sees it }
      TParadeStringsTarget(TB.Target).Edit(1, 5, 1, 5, #10 + '  WriteLn(''hi'');');
      Check(WaitFor(@TextsSame, 15), 'a line typed reaches the other: ' + LA.Text);
      Check((LA.Count = 4) and (LA[2] = '  WriteLn(''hi'');'), 'the line: ' + LA.Text);
      { both at once, in the same line }
      Wide := 'hello, ' + #$E4#$B8#$96#$E7#$95#$8C + ' ' + #$F0#$9F#$98#$80;
      TParadeStringsTarget(TA.Target).Edit(0, 8, 0, 13, 'world');
      TParadeStringsTarget(TB.Target).Edit(2, 11, 2, 13, Wide);
      Check(WaitFor(@TextsSame, 15), 'edits at once merge');
      Check((LA[0] = 'program world;') and (LB[2] = '  WriteLn(''' + Wide + ''');'),
        'both kept, wide characters too: ' + LA[0] + ' / ' + LB[2]);
      Check(TParadeTextTarget(TA.Target).SharedText = TParadeTextTarget(TB.Target).SharedText, 'the same shared text');
      Check(ParadeRelayKind(Url, 'hello.pas', TokA2) = 'text', 'the relay''s text is a text');
      Check(ParadeRelayKind(Url, 'proposal', TokA) = 'rich', 'and the rich document is not');
      Check(ParadeRelayKind(Url, 'nothing-here', TokA) = '', 'nothing yet: neither');
      F := ParadeInviteLink(Url, 'hello.pas', TokB2, 'text');
      Check((ParadeInviteKind(F) = 'text') and ParadeParseInvite(F, Server, DocName, Token) and (Token = TokB2) and
        (DocName = 'hello.pas'), 'a link says it is a text, its token whole: ' + F);
      Check(ParadeInviteKind(ParadeInviteLink(Url, 'proposal', TokB)) = '', 'a rich document''s link says nothing');
      { undo takes back one's own edit only }
      TParadeTextTarget(TA.Target).Seal;
      TParadeStringsTarget(TA.Target).Edit(3, 4, 3, 4, ' { the end }');
      Check(WaitFor(@TextsSame, 15), 'another edit');
      Check(TParadeTextTarget(TA.Target).Undo, 'undo');
      Check(WaitFor(@TextsSame, 15) and (LB[3] = 'end.') and (Pos(Wide, LB[2]) > 0),
        'Ann''s last edit gone from both, Bob''s kept: ' + LB[3]);
      { the others' carets, where they are }
      TParadeStringsTarget(TB.Target).SetCaretAt(2, 4);
      Check(WaitFor(@BobCaretSeen, 15), 'Bob''s caret where he left it');
      { leaving is at once, though a long poll is waiting on the relay }
      Sleep(300);
      T0 := GetTickCount64;
      TA.Stop;
      T1 := GetTickCount64;
      TB.Stop;
      T2 := GetTickCount64;
      WriteLn('  leaving took ', T1 - T0, ' and ', T2 - T1, ' ms');
      Check((T1 - T0 < 2000) and (T2 - T1 < 2000), 'leaving does not wait for the long poll');
    finally
      LA.Free;
      LB.Free;
    end;

    { a picture of the two editors, the other's caret drawn in each }
    C := A.CaretPos;
    Check(C.block <> 0, 'caret');
    ExecuteProcess('/usr/bin/import', ['-window', 'root', Dir + 'sync_test.png']);

    SA.Stop;
    SB.Stop;
    StopRelay;
    WriteLn(Checks, ' checks, ', Failures, ' failures');
    Form.Free;
  except
    on X: Exception do
      Fail(X);
  end;
  if Failures > 0 then
    Halt(1);
end.
