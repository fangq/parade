{ Parade demo: a small word processor on TParadeEdit }
program paradedemo;

{$mode objfpc}{$H+}

uses
  {$IFDEF UNIX}cthreads,{$ENDIF}
  Interfaces, Forms, Controls, StdCtrls, ExtCtrls, ComCtrls, Dialogs, SysUtils, Classes, parade, paradeedit;

type
  TMainForm = class(TForm)
  private
    FEdit: TParadeEdit;
    FBar: TPanel;
    FStatus: TStatusBar;
    FStyle: TComboBox;
    function AddButton(const ACaption: string; Handler: TNotifyEvent): TButton;
    procedure DoNew(Sender: TObject);
    procedure DoOpen(Sender: TObject);
    procedure DoSave(Sender: TObject);
    procedure DoPDF(Sender: TObject);
    procedure DoBold(Sender: TObject);
    procedure DoItalic(Sender: TObject);
    procedure DoUnderline(Sender: TObject);
    procedure DoUndo(Sender: TObject);
    procedure DoRedo(Sender: TObject);
    procedure DoZoomIn(Sender: TObject);
    procedure DoZoomOut(Sender: TObject);
    procedure DoStyle(Sender: TObject);
    procedure EditChanged(Sender: TObject);
  public
    constructor CreateMain;
  end;

constructor TMainForm.CreateMain;
const
  Styles: array[0..7] of string = ('Normal', 'Title', 'Heading 1', 'Heading 2', 'Heading 3', 'Quote', 'Code', 'Caption');
var
  I: Integer;
begin
  inherited CreateNew(nil);
  Caption := 'Parade';
  SetBounds(100, 80, 1000, 760);
  FBar := TPanel.Create(Self);
  FBar.Parent := Self;
  FBar.Align := alTop;
  FBar.Height := 34;
  FBar.BevelOuter := bvNone;
  FBar.ChildSizing.ControlsPerLine := 100;
  FBar.ChildSizing.Layout := cclLeftToRightThenTopToBottom;
  FBar.ChildSizing.HorizontalSpacing := 4;
  FBar.BorderSpacing.Around := 4;
  AddButton('New', @DoNew);
  AddButton('Open', @DoOpen);
  AddButton('Save', @DoSave);
  AddButton('PDF', @DoPDF);
  AddButton('B', @DoBold);
  AddButton('I', @DoItalic);
  AddButton('U', @DoUnderline);
  AddButton('Undo', @DoUndo);
  AddButton('Redo', @DoRedo);
  FStyle := TComboBox.Create(Self);
  FStyle.Parent := FBar;
  FStyle.Style := csDropDownList;
  for I := 0 to High(Styles) do
    FStyle.Items.Add(Styles[I]);
  FStyle.ItemIndex := 0;
  FStyle.OnSelect := @DoStyle;
  AddButton('+', @DoZoomIn);
  AddButton('-', @DoZoomOut);
  FStatus := TStatusBar.Create(Self);
  FStatus.Parent := Self;
  FStatus.SimplePanel := True;
  FEdit := TParadeEdit.Create(Self);
  FEdit.Parent := Self;
  FEdit.Align := alClient;
  FEdit.AddDefaultFonts;
  FEdit.OnChange := @EditChanged;
  if (ParamCount > 0) and FileExists(ParamStr(1)) then
    FEdit.LoadFromFile(ParamStr(1));
  EditChanged(nil);
  ActiveControl := FEdit;
end;

function TMainForm.AddButton(const ACaption: string; Handler: TNotifyEvent): TButton;
begin
  Result := TButton.Create(Self);
  Result.Parent := FBar;
  Result.Caption := ACaption;
  Result.AutoSize := True;
  Result.OnClick := Handler;
  Result.TabStop := False;
end;

procedure TMainForm.EditChanged(Sender: TObject);
begin
  FStatus.SimpleText := Format('%d pages   zoom %d%%   %s', [FEdit.PageCount, Round(FEdit.Zoom * 100),
    FEdit.FileName]);
end;

procedure TMainForm.DoNew(Sender: TObject);
begin
  FEdit.NewDocument;
  EditChanged(nil);
end;

procedure TMainForm.DoOpen(Sender: TObject);
var
  D: TOpenDialog;
begin
  D := TOpenDialog.Create(Self);
  try
    D.Filter := 'All documents|*.pdoc;*.jdoc;*.docx;*.rtf;*.html;*.htm;*.md;*.txt|Parade documents|*.pdoc;*.jdoc|' +
      'Word|*.docx|Rich Text|*.rtf|HTML|*.html;*.htm|Markdown|*.md|Text|*.txt|All files|*';
    if D.Execute then
      FEdit.LoadFromFile(D.FileName);
  finally
    D.Free;
  end;
  EditChanged(nil);
end;

procedure TMainForm.DoSave(Sender: TObject);
var
  D: TSaveDialog;
begin
  D := TSaveDialog.Create(Self);
  try
    D.Filter := 'Parade document|*.pdoc|Parade document, JSON text|*.jdoc|Word|*.docx|Rich Text|*.rtf|HTML|*.html|Markdown|*.md|' +
      'LaTeX|*.tex|Text|*.txt';
    D.DefaultExt := 'pdoc';
    D.FileName := FEdit.FileName;
    if D.Execute then
      FEdit.SaveToFile(D.FileName);
  finally
    D.Free;
  end;
  EditChanged(nil);
end;

procedure TMainForm.DoPDF(Sender: TObject);
var
  D: TSaveDialog;
begin
  D := TSaveDialog.Create(Self);
  try
    D.Filter := 'PDF|*.pdf';
    D.DefaultExt := 'pdf';
    if D.Execute then
      FEdit.ExportPDF(D.FileName);
  finally
    D.Free;
  end;
end;

procedure TMainForm.DoBold(Sender: TObject);
begin
  FEdit.ToggleBold;
  FEdit.SetFocus;
end;

procedure TMainForm.DoItalic(Sender: TObject);
begin
  FEdit.ToggleItalic;
  FEdit.SetFocus;
end;

procedure TMainForm.DoUnderline(Sender: TObject);
begin
  FEdit.ToggleUnderline;
  FEdit.SetFocus;
end;

procedure TMainForm.DoUndo(Sender: TObject);
begin
  FEdit.Undo;
  FEdit.SetFocus;
end;

procedure TMainForm.DoRedo(Sender: TObject);
begin
  FEdit.Redo;
  FEdit.SetFocus;
end;

procedure TMainForm.DoZoomIn(Sender: TObject);
begin
  FEdit.Zoom := FEdit.Zoom * 1.25;
  EditChanged(nil);
end;

procedure TMainForm.DoZoomOut(Sender: TObject);
begin
  FEdit.Zoom := FEdit.Zoom / 1.25;
  EditChanged(nil);
end;

procedure TMainForm.DoStyle(Sender: TObject);
begin
  FEdit.SetParagraphStyle(FStyle.Text);
  FEdit.SetFocus;
end;

var
  MainForm: TMainForm;
begin
  Application.Initialize;
  MainForm := TMainForm.CreateMain;
  Application.ShowMainForm := True;
  MainForm.Show;
  Application.Run;
  MainForm.Free;
end.
