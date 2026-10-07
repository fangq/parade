{ The fonts installed on the system, for a font list: each face's family,
  weight, italic and file, found once and kept. Nothing is loaded here --
  TParadeEdit.AddSystemFonts registers them and loads a face the first time
  text uses it.

  On Linux and the BSDs fontconfig answers (fc-list); where it does not
  (Windows, macOS, or no fontconfig) the platform's font folders are read,
  the names coming from each file's own tables (TrueType/OpenType, and the
  faces of a collection, .ttc). }

unit paradefonts;

{$mode objfpc}{$H+}

interface

uses
  Classes, SysUtils;

type
  TParadeSystemFace = record
    Family: string;
    FileName: string;
    Index: Integer;            { the face in a collection (.ttc), 0 otherwise }
    Weight: Integer;           { 100..900 }
    Italic: Boolean;
  end;
  TParadeSystemFaces = array of TParadeSystemFace;

{ every scalable face installed, sorted by family; found on the first call, the same list after }
function ParadeSystemFaces: TParadeSystemFaces;
{ the faces in the given folders (and their subfolders), from the files' own tables: what is used where
  fontconfig is not }
function ParadeScanFontDirs(const Dirs: array of string): TParadeSystemFaces;
{ a font file's faces, from its own tables (none when it is not a TrueType/OpenType font or collection) }
function ParadeReadFontFile(const FileName: string): TParadeSystemFaces;

implementation

{$IFDEF UNIX}
uses
  Process;
{$ENDIF}

var
  Cached: TParadeSystemFaces;
  CachedDone: Boolean = False;

{ ---------- a font file's own tables ---------- }

function U16(const B: TBytes; P: Int64): Integer; inline;
begin
  if (P < 0) or (P + 1 >= Length(B)) then Exit(0);
  Result := B[P] shl 8 or B[P + 1];
end;

function U32(const B: TBytes; P: Int64): Int64; inline;
begin
  if (P < 0) or (P + 3 >= Length(B)) then Exit(0);
  Result := Int64(B[P]) shl 24 or B[P + 1] shl 16 or B[P + 2] shl 8 or B[P + 3];
end;

{ a UTF-16BE run as UTF-8 }
function Utf16BE(const B: TBytes; P, Len: Int64): string;
var
  W: UnicodeString;
  I: Integer;
begin
  SetLength(W, Len div 2);
  for I := 1 to Length(W) do
    W[I] := WideChar(U16(B, P + (I - 1) * 2));
  Result := UTF8Encode(W);
end;

{ one face's family, weight and italic, from the table directory at Base }
function ReadFace(const B: TBytes; Base: Int64; out F: TParadeSystemFace): Boolean;
var
  N, I, Count, StrOff, Pid, Eid, Lid, Nid, Len, Off, Best, Score: Integer;
  Tag: string;
  NameAt, Os2At, HeadAt, Rec: Int64;
  Fam1, Fam16: string;
begin
  Result := False;
  F := Default(TParadeSystemFace);
  N := U16(B, Base + 4);
  if (N <= 0) or (N > 200) then
    Exit;
  NameAt := -1;
  Os2At := -1;
  HeadAt := -1;
  for I := 0 to N - 1 do
  begin
    Rec := Base + 12 + I * 16;
    if Rec + 16 > Length(B) then
      Exit;
    Tag := Chr(B[Rec]) + Chr(B[Rec + 1]) + Chr(B[Rec + 2]) + Chr(B[Rec + 3]);
    if Tag = 'name' then NameAt := U32(B, Rec + 8)
    else if Tag = 'OS/2' then Os2At := U32(B, Rec + 8)
    else if Tag = 'head' then HeadAt := U32(B, Rec + 8);
  end;
  if (NameAt < 0) or (NameAt + 6 > Length(B)) then
    Exit;
  { the family: the Windows English name (1, or the typographic 16 when 1 is missing), else the Mac one }
  Count := U16(B, NameAt + 2);
  StrOff := U16(B, NameAt + 4);
  Fam1 := '';
  Fam16 := '';
  Best := MaxInt;
  for I := 0 to Count - 1 do
  begin
    Rec := NameAt + 6 + I * 12;
    if Rec + 12 > Length(B) then
      Break;
    Pid := U16(B, Rec);
    Eid := U16(B, Rec + 2);
    Lid := U16(B, Rec + 4);
    Nid := U16(B, Rec + 6);
    Len := U16(B, Rec + 8);
    Off := U16(B, Rec + 10);
    if (Nid <> 1) and (Nid <> 16) then
      Continue;
    if NameAt + StrOff + Off + Len > Length(B) then
      Continue;
    if (Pid = 3) and ((Eid = 1) or (Eid = 0)) then
      Score := Ord(Lid <> $409)
    else if (Pid = 1) and (Eid = 0) then
      Score := 2
    else if Pid = 0 then
      Score := 1
    else
      Continue;
    if (Nid = 1) and (Score <= Best) then
    begin
      Best := Score;
      if Pid = 1 then
        SetString(Fam1, PAnsiChar(@B[NameAt + StrOff + Off]), Len)
      else
        Fam1 := Utf16BE(B, NameAt + StrOff + Off, Len);
    end
    else if (Nid = 16) and (Fam16 = '') and (Pid <> 1) then
      Fam16 := Utf16BE(B, NameAt + StrOff + Off, Len);
  end;
  F.Family := Trim(Fam1);
  if F.Family = '' then
    F.Family := Trim(Fam16);
  if F.Family = '' then
    Exit;
  F.Weight := 400;
  if (Os2At >= 0) and (Os2At + 64 <= Length(B)) then
  begin
    F.Weight := U16(B, Os2At + 4);
    F.Italic := (U16(B, Os2At + 62) and 1) <> 0;
  end
  else if (HeadAt >= 0) and (HeadAt + 46 <= Length(B)) then
  begin
    F.Italic := (U16(B, HeadAt + 44) and 2) <> 0;
    if (U16(B, HeadAt + 44) and 1) <> 0 then
      F.Weight := 700;
  end;
  if (F.Weight < 1) or (F.Weight > 1000) then
    F.Weight := 400;
  Result := True;
end;

function ParadeReadFontFile(const FileName: string): TParadeSystemFaces;
var
  S: TFileStream;
  B: TBytes;
  Sig: Int64;
  I, N: Integer;
  F: TParadeSystemFace;
begin
  Result := nil;
  try
    S := TFileStream.Create(FileName, fmOpenRead or fmShareDenyNone);
    try
      { the tables needed sit near the start: enough of the file for them, never all of a 20 MB CJK font }
      SetLength(B, S.Size);
      if Length(B) > 4 shl 20 then
        SetLength(B, 4 shl 20);
      if Length(B) < 12 then
        Exit;
      S.ReadBuffer(B[0], Length(B));
    finally
      S.Free;
    end;
  except
    Exit;
  end;
  Sig := U32(B, 0);
  if Sig = $74746366 then     { 'ttcf': a collection, its faces' directories listed }
  begin
    N := U32(B, 8);
    if (N <= 0) or (N > 64) then
      Exit;
    for I := 0 to N - 1 do
      if ReadFace(B, U32(B, 12 + I * 4), F) then
      begin
        F.FileName := FileName;
        F.Index := I;
        SetLength(Result, Length(Result) + 1);
        Result[High(Result)] := F;
      end;
  end
  else if (Sig = $00010000) or (Sig = $4F54544F) or (Sig = $74727565) then   { TrueType, 'OTTO', 'true' }
  begin
    if ReadFace(B, 0, F) then
    begin
      F.FileName := FileName;
      SetLength(Result, 1);
      Result[0] := F;
    end;
  end;
end;

function ParadeScanFontDirs(const Dirs: array of string): TParadeSystemFaces;
var
  L: TParadeSystemFaces;
  N: Integer;

  procedure Scan(const Dir: string; Depth: Integer);
  var
    R: TSearchRec;
    Ext: string;
    Got: TParadeSystemFaces;
    I: Integer;
  begin
    if (Depth > 6) or (FindFirst(IncludeTrailingPathDelimiter(Dir) + '*', faAnyFile, R) <> 0) then
      Exit;
    try
      repeat
        if (R.Name = '.') or (R.Name = '..') then
          Continue;
        if (R.Attr and faDirectory) <> 0 then
          Scan(IncludeTrailingPathDelimiter(Dir) + R.Name, Depth + 1)
        else
        begin
          Ext := LowerCase(ExtractFileExt(R.Name));
          if (Ext = '.ttf') or (Ext = '.otf') or (Ext = '.ttc') or (Ext = '.otc') then
          begin
            Got := ParadeReadFontFile(IncludeTrailingPathDelimiter(Dir) + R.Name);
            for I := 0 to High(Got) do
            begin
              if N = Length(L) then
                SetLength(L, N * 2 + 64);
              L[N] := Got[I];
              Inc(N);
            end;
          end;
        end;
      until FindNext(R) <> 0;
    finally
      FindClose(R);
    end;
  end;

var
  I: Integer;
begin
  L := nil;
  N := 0;
  for I := 0 to High(Dirs) do
    if (Dirs[I] <> '') and DirectoryExists(Dirs[I]) then
      Scan(Dirs[I], 0);
  SetLength(L, N);
  Result := L;
end;

{ ---------- fontconfig ---------- }

{$IFDEF UNIX}
{ fontconfig's weight scale (0..215) to CSS's (100..900) }
function FcWeight(W: Integer): Integer;
begin
  if W <= 0 then Result := 100
  else if W <= 40 then Result := 200
  else if W <= 50 then Result := 300
  else if W <= 75 then Result := 350
  else if W <= 80 then Result := 400
  else if W <= 100 then Result := 500
  else if W <= 180 then Result := 600
  else if W <= 200 then Result := 700
  else if W <= 205 then Result := 800
  else Result := 900;
end;

{ the families of a fontconfig list ("DejaVu Sans,DejaVu Sans Condensed"): the first, or for a face
  narrower or wider than normal the last, which names it ("DejaVu Sans Condensed") }
function PickFamily(const S: string; Normal: Boolean): string;
var
  Parts: TStringList;
begin
  Parts := TStringList.Create;
  try
    Parts.StrictDelimiter := True;
    Parts.Delimiter := ',';
    Parts.DelimitedText := StringReplace(S, '\,', #1, [rfReplaceAll]);
    if Parts.Count = 0 then
      Exit('');
    if Normal then
      Result := Parts[0]
    else
      Result := Parts[Parts.Count - 1];
    Result := Trim(StringReplace(Result, #1, ',', [rfReplaceAll]));
  finally
    Parts.Free;
  end;
end;

function FromFontconfig(out Faces: TParadeSystemFaces): Boolean;
var
  Output: string;
  Lines, Cols: TStringList;
  I, N: Integer;
  F: TParadeSystemFace;
  Ext: string;
begin
  Faces := nil;
  Result := False;
  try
    if not RunCommand('fc-list', [':scalable=true', '--format',
      '%{family}' + #9 + '%{weight}' + #9 + '%{slant}' + #9 + '%{width}' + #9 + '%{index}' + #9 + '%{file}\n'],
      Output, [poNoConsole]) then
      Exit;
  except
    Exit;
  end;
  Lines := TStringList.Create;
  Cols := TStringList.Create;
  try
    Lines.Text := Output;
    Cols.StrictDelimiter := True;
    Cols.Delimiter := #9;
    SetLength(Faces, Lines.Count);
    N := 0;
    for I := 0 to Lines.Count - 1 do
    begin
      Cols.DelimitedText := Lines[I];
      if Cols.Count < 6 then
        Continue;
      Ext := LowerCase(ExtractFileExt(Cols[5]));
      if (Ext <> '.ttf') and (Ext <> '.otf') and (Ext <> '.ttc') and (Ext <> '.otc') then
        Continue;     { Type 1, bitmap and the like: not what Parade reads }
      F := Default(TParadeSystemFace);
      F.Family := PickFamily(Cols[0], StrToIntDef(Cols[3], 100) = 100);
      if F.Family = '' then
        Continue;
      F.Weight := FcWeight(StrToIntDef(Cols[1], 80));
      F.Italic := StrToIntDef(Cols[2], 0) <> 0;
      F.Index := StrToIntDef(Cols[4], 0);
      F.FileName := Cols[5];
      Faces[N] := F;
      Inc(N);
    end;
    SetLength(Faces, N);
    Result := N > 0;
  finally
    Cols.Free;
    Lines.Free;
  end;
end;
{$ENDIF}

{ the folders fonts are installed in, where fontconfig is not asked }
function FontDirs: TStringArray;
var
  Home: string;
begin
  Home := GetUserDir;
  {$IFDEF WINDOWS}
  Result := [IncludeTrailingPathDelimiter(GetEnvironmentVariable('WINDIR')) + 'Fonts',
    IncludeTrailingPathDelimiter(GetEnvironmentVariable('LOCALAPPDATA')) + 'Microsoft\Windows\Fonts'];
  {$ELSE}
  {$IFDEF DARWIN}
  Result := ['/System/Library/Fonts', '/Library/Fonts', Home + 'Library/Fonts'];
  {$ELSE}
  Result := ['/usr/share/fonts', '/usr/local/share/fonts', Home + '.local/share/fonts', Home + '.fonts'];
  {$ENDIF}
  {$ENDIF}
end;

{ by family, then weight, then upright first: through a sorted index, the faces being records }
procedure SortFaces(var F: TParadeSystemFaces);
var
  I: Integer;
  L: TStringList;
  Sorted: TParadeSystemFaces;
begin
  L := TStringList.Create;
  try
    for I := 0 to High(F) do
      L.AddObject(Format('%s'#1'%.4d'#1'%d'#1'%.6d', [LowerCase(F[I].Family), F[I].Weight, Ord(F[I].Italic), I]),
        TObject(PtrInt(I)));
    L.Sort;
    SetLength(Sorted, Length(F));
    for I := 0 to L.Count - 1 do
      Sorted[I] := F[PtrInt(L.Objects[I])];
    F := Sorted;
  finally
    L.Free;
  end;
end;

function ParadeSystemFaces: TParadeSystemFaces;
begin
  if not CachedDone then
  begin
    CachedDone := True;
    Cached := nil;
    {$IFDEF UNIX}{$IFNDEF DARWIN}
    if not FromFontconfig(Cached) then
    {$ENDIF}{$ENDIF}
      Cached := ParadeScanFontDirs(FontDirs);
    SortFaces(Cached);
  end;
  Result := Cached;
end;

end.
