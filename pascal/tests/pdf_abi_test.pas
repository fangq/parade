{ the PDF reader's records as Pascal lays them out (pdf_abi_c.c prints the same from C) }
program pdf_abi_test;
{$mode objfpc}{$H+}
uses paradepdf;
var
  C: pd_pdfr_char;
  A: pd_pdfr_annot;
  F: pd_pdfr_field;
  S: pd_pdfr_span;
  B: pd_pdfr_bookmark;
begin
  WriteLn('char ', SizeOf(C), ' ', PtrUInt(@C.x0) - PtrUInt(@C), ' ', PtrUInt(@C.size) - PtrUInt(@C), ' ',
    PtrUInt(@C.run) - PtrUInt(@C));
  WriteLn('annot ', SizeOf(A), ' ', PtrUInt(@A.x0) - PtrUInt(@A), ' ', PtrUInt(@A.color) - PtrUInt(@A), ' ',
    PtrUInt(@A.width) - PtrUInt(@A), ' ', PtrUInt(@A.field) - PtrUInt(@A), ' ', PtrUInt(@A.contents) - PtrUInt(@A),
    ' ', PtrUInt(@A.uri) - PtrUInt(@A));
  WriteLn('field ', SizeOf(F), ' ', PtrUInt(@F.value) - PtrUInt(@F), ' ', PtrUInt(@F.page) - PtrUInt(@F), ' ',
    PtrUInt(@F.x0) - PtrUInt(@F), ' ', PtrUInt(@F.readonly) - PtrUInt(@F), ' ', PtrUInt(@F.maxlen) - PtrUInt(@F),
    ' ', PtrUInt(@F.font_size) - PtrUInt(@F));
  WriteLn('span ', SizeOf(S), ' ', PtrUInt(@S.x0) - PtrUInt(@S), ' ', PtrUInt(@S.size) - PtrUInt(@S), ' ',
    PtrUInt(@S.lines) - PtrUInt(@S), ' ', PtrUInt(@S.align) - PtrUInt(@S));
  WriteLn('bookmark ', SizeOf(B), ' ', PtrUInt(@B.page) - PtrUInt(@B), ' ', PtrUInt(@B.top) - PtrUInt(@B), ' ',
    PtrUInt(@B.title) - PtrUInt(@B), ' ', PtrUInt(@B.uri) - PtrUInt(@B));
end.
