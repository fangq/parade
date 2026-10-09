#!/usr/bin/env python3
"""Generate the C and Pascal halves of the ABI check from one table.

Each program prints "struct size" and "struct.field offset" lines; the two
outputs must be identical (make pascal-abi).
"""
STRUCTS = {
    "pd_font_metrics": "units_per_em ascender descender line_gap x_height cap_height num_glyphs has_kerning",
    "pd_outline_sink": "move_to line_to quad_to cubic_to close",
    "pd_glyph_image": "width height left top",
    "pd_style": "font size space_stretch space_shrink kerning color user hyph text_case letter_space hidden",
    "pd_params": "mode align width indent line_penalty adj_demerits double_hyphen_demerits final_hyphen_demerits "
                 "hyphen_penalty ex_hyphen_penalty tex_badness rag_stretch baseline_skip line_spacing hysteresis "
                 "freeze_offset direction looseness protrusion expansion full_lines line_grid",
    "pd_break_info": "lines demerits overfull underfull reused_breakpoints frozen_lines height",
    "pd_line": "text_start text_end x baseline width ascent descent ratio badness hyphenated overfull underfull",
    "pd_glyph": "glyph cluster x y advance style kind user scale",
    "pd_pos": "block offset",
    "pd_range": "start end",
    "pd_block_info": "kind id parent child_count index role level list_level style list break_kind text_length "
                     "revision",
    "pd_char_props": "mask family size weight italic color background underline strike shift letter_space kerning "
                     "lang small_caps link_target caps hidden position revision family_ea family_cs size_cs weight_cs "
                     "italic_cs",
    "pd_revision": "kind author date",
    "pd_comment": "author date text text_len parent resolved range",
    "pd_para_props": "mask align indent_left indent_right indent_first space_before space_after line_spacing "
                     "keep_with_next keep_lines widows orphans page_break_before hyphenate break_mode next_style "
                     "border_color border_width shading direction ntabs tabs tab_interval contextual border_sides border_space "
                     "snap_grid",
    "pd_tab_stop": "position align leader",
    "pd_list_level": "format start text indent hanging restart_after label_family label_size label_weight "
                     "label_italic label_color",
    "pd_run": "start end format",
    "pd_inline": "kind resource width height depth field target level name source source_len user title title_len "
                 "alt alt_len",
    "pd_para_attrs": "quote_depth task loose lang cont div_class",
    "pd_font_info": "alt generic pitch panose charset",
    "pd_float_props": "placement wrap width width_fraction span_columns gap sequence offset_x offset_y offset_from",
    "pd_section_props": "page_width page_height margin_top margin_bottom margin_left margin_right header_distance "
                        "footer_distance columns column_gap first_page_number page_number_format title_page "
                        "facing_pages header header_first header_even footer footer_first footer_even continuous page_breaking "
                        "footnote_skip add_spacing line_numbers line_number_start line_number_distance "
                        "line_number_restart mirror_margins gutter page_valign line_pitch",
    "pd_table_props": "width align header_rows cell_padding border border_color ncols col_width indent width_pct "
                      "border_sides cell_padding_v",
    "pd_cell_props": "col_span valign background merge_up min_height border_set border_on border_width border_color "
                     "edge_width",
    "pd_change": "kind block style revision",
    "pd_layout_info": "pages paragraphs_broken paragraphs_reused float_pages overfull variants",
    "pd_page_info": "width height section number label float_page first last",
    "pd_draw": "kind x y w h glyph font size color resource block offset region text scale points npoints path_flags "
               "line_width fill clip_x clip_y clip_w clip_h",
    "pd_markup_item": "kind id range x y top bottom color",
    "pd_pdf_options": "compress outlines title author",
}
PASCAL_NAME = {"end": "finish", "label": "label_"}

c = ['#include <stdio.h>', '#include <stddef.h>', '#include "parade_layout.h"', 'int main(void) {']
p = ['program abi_test;', '{$mode objfpc}{$H+}', 'uses parade;', 'var']
for s in STRUCTS:
    p.append(f'  v_{s}: {s};')
p.append('begin')
for s, fields in STRUCTS.items():
    c.append(f'    printf("{s} %zu\\n", sizeof({s}));')
    p.append(f"  WriteLn('{s} ', SizeOf({s}));")
    for f in fields.split():
        c.append(f'    printf("{s}.{f} %zu\\n", offsetof({s}, {f}));')
        pf = PASCAL_NAME.get(f, f)
        p.append(f"  WriteLn('{s}.{f} ', PtrUInt(@v_{s}.{pf}) - PtrUInt(@v_{s}));")
c += ['    return 0;', '}']
p.append('end.')
open('pascal/tests/abi_c.c', 'w').write('\n'.join(c) + '\n')
open('pascal/tests/abi_test.pas', 'w').write('\n'.join(p) + '\n')
print('generated pascal/tests/abi_c.c and pascal/tests/abi_test.pas')
