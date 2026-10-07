{ The Parade relay as a program, in place of tools/parade_relay.py (same
  arguments, same log file, same tokens; SQLite only):

    parade_relay serve --db relay.sqlite --secret-file relay.secret [--host 127.0.0.1] [--port 8765]
                       [--compact-every 500 | --no-compact]
    parade_relay compact --db relay.sqlite --doc proposal
    parade_relay token --secret-file relay.secret --user ann [--doc proposal] [--role editor] [--days 30]
    parade_relay secret > relay.secret }
program parade_relay;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads, BaseUnix,{$ENDIF}
  Classes, SysUtils,
  parade,         { before paraderelay: libparade.a goes to the linker ahead of libyrs.a, which it needs }
  paraderelay;

type
  TLogger = class
    procedure Log(Sender: TObject; const Msg: string);
  end;

var
  Opts: TStringList;
  Cmd: string;
  Quit: Boolean = False;

procedure TLogger.Log(Sender: TObject; const Msg: string);
begin
  WriteLn(StdErr, Msg);
  Flush(StdErr);
end;

procedure Usage;
begin
  WriteLn(StdErr, 'usage: parade_relay serve|compact|token|secret [--option value ...] (see the source)');
  Halt(2);
end;

function Opt(const Name, Default: string; Required: Boolean = False): string;
var
  I: Integer;
begin
  I := Opts.IndexOfName(Name);
  if I >= 0 then
    Exit(Opts.ValueFromIndex[I]);
  if Required then
  begin
    WriteLn(StdErr, 'parade_relay ', Cmd, ': --', Name, ' is needed');
    Halt(2);
  end;
  Result := Default;
end;

{$IFDEF UNIX}
procedure OnSignal(Sig: cint); cdecl;
begin
  Quit := True;
end;
{$ENDIF}

var
  I: Integer;
  R: TParadeRelay;
  L: TLogger;
  Role: string;
  Before: string;
  St: TParadeRelayStore;
begin
  if ParamCount < 1 then
    Usage;
  Cmd := ParamStr(1);
  Opts := TStringList.Create;
  I := 2;
  while I <= ParamCount do
  begin
    if Copy(ParamStr(I), 1, 2) <> '--' then
      Usage;
    if ParamStr(I) = '--no-compact' then
      Opts.Values['no-compact'] := '1'
    else
    begin
      if I = ParamCount then
        Usage;
      Opts.Values[Copy(ParamStr(I), 3, MaxInt)] := ParamStr(I + 1);
      Inc(I);
    end;
    Inc(I);
  end;
  try
    if Cmd = 'secret' then
      WriteLn(ParadeNewSecret)
    else if Cmd = 'token' then
    begin
      Role := Opt('role', 'editor');
      if (Role <> 'viewer') and (Role <> 'commenter') and (Role <> 'editor') then
        Usage;
      WriteLn(ParadeMakeToken(ParadeReadSecret(Opt('secret-file', '', True)), Opt('user', '', True), Opt('doc', '*'), Role,
        StrToFloat(Opt('days', '30'), DefaultFormatSettings)));
    end
    else if Cmd = 'compact' then
    begin
      St := TParadeRelayStore.Create(Opt('db', 'relay.sqlite'));
      try
        Before := St.Info(Opt('doc', '', True));
        if ParadeCompact(St, Opt('doc', '')) then
          WriteLn(Opt('doc', ''), ': ', Before, ' -> ', St.Info(Opt('doc', '')))
        else
          WriteLn(Opt('doc', ''), ': ', Before, ' -> unchanged');
      finally
        St.Free;
      end;
    end
    else if Cmd = 'serve' then
    begin
      L := TLogger.Create;
      R := TParadeRelay.Create(nil);
      try
        R.OnLog := @L.Log;
        R.DbFile := Opt('db', 'relay.sqlite');
        R.Secret := ParadeReadSecret(Opt('secret-file', '', True));
        R.Host := Opt('host', '127.0.0.1');
        R.Port := StrToInt(Opt('port', '8765'));
        R.Compact := Opt('no-compact', '') = '';
        R.CompactEvery := StrToInt(Opt('compact-every', '500'));
        if R.CompactEvery < 2 then
          R.CompactEvery := 2;
        if not R.Start then
        begin
          WriteLn(StdErr, 'parade_relay: ', R.LastError);
          Halt(1);
        end;
        {$IFDEF UNIX}
        FpSignal(SIGINT, @OnSignal);
        FpSignal(SIGTERM, @OnSignal);
        {$ENDIF}
        while not Quit do
          Sleep(100);
        R.Stop;
      finally
        R.Free;
        L.Free;
      end;
    end
    else
      Usage;
  except
    on E: Exception do
    begin
      WriteLn(StdErr, 'parade_relay: ', E.Message);
      Halt(1);
    end;
  end;
end.
