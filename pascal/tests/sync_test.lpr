{ Two editors sharing a document through the relay (tools/parade_relay.py),
  driven headless: share and join, typing on each side and at once, the
  others' carets, undo of one's own edits, the relay going away and
  coming back with edits made meanwhile, edits kept on disk across a quit,
  and joining from a compacted log. Run under a display (xvfb-run);
  arguments: the path of parade_relay.py, then of pd_compact. }
program sync_test;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads,{$ENDIF}
  Interfaces, Forms, Controls, Graphics, LCLType, SysUtils, Classes, Process, FileUtil, fphttpclient, fpjson, jsonparser,
  parade, paradeedit, paradesync;

type
  TFunc = function: Boolean;

var
  Failures: Integer = 0;
  Checks: Integer = 0;
  Relay: TProcess;
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

function Run(const Args: array of string): string;
begin
  if not RunCommand('python3', Args, Result, [poStderrToOutPut]) then
    raise Exception.Create('python3 failed: ' + Result);
  Result := Trim(Result);
end;

procedure StartRelay;
begin
  Relay := TProcess.Create(nil);
  Relay.Executable := 'python3';
  Relay.Parameters.Add(RelayPy);
  Relay.Parameters.Add('serve');
  Relay.Parameters.Add('--db');
  Relay.Parameters.Add(DbFile);
  Relay.Parameters.Add('--secret-file');
  Relay.Parameters.Add(SecretFile);
  Relay.Parameters.Add('--port');
  Relay.Parameters.Add(IntToStr(Port));
  Relay.Parameters.Add('--compactor');
  Relay.Parameters.Add(Compactor);
  Relay.Parameters.Add('--compact-every');
  Relay.Parameters.Add('5');
  Relay.Options := [poNoConsole];
  Relay.Execute;
  Sleep(1500);
end;

procedure StopRelay;
begin
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
  TokA, TokB, TokV: string;
  I: Integer;
  C: pd_pos;
  J: TJSONObject;
  F: string;

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
    RelayPy := ExpandFileName(ParamStr(1));
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
      Text := Run([RelayPy, 'secret']);
      SaveToFile(SecretFile);
    finally
      Free;
    end;
    Port := 18000 + Random(20000);
    Url := 'http://127.0.0.1:' + IntToStr(Port);
    TokA := Run([RelayPy, 'token', '--secret-file', SecretFile, '--user', 'ann', '--doc', 'proposal']);
    TokB := Run([RelayPy, 'token', '--secret-file', SecretFile, '--user', 'bob', '--doc', 'proposal']);
    TokV := Run([RelayPy, 'token', '--secret-file', SecretFile, '--user', 'vic', '--doc', 'proposal', '--role', 'viewer']);
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

    { 1. Ann shares her document; Bob joins and gets it }
    Step('share and join');
    A.InsertText('Hello from Ann.');
    Check(SA.Start(Url, 'proposal', TokA, 'Ann', True), 'share: ' + SA.LastError);
    Check(WaitFor(@ASynced, 15), 'shared: the relay has it');
    Check(not SB.Start(Url, 'proposal', TokB, 'Bob', True), 'sharing a document the relay has is refused');
    Check(SB.Start(Url, 'proposal', TokB, 'Bob', False), 'join: ' + SB.LastError);
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
