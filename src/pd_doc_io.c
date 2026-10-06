/*
 * Parade native format: the document as JData (JSON text) or BJData
 *
 * {
 *   "_DataInfo_": {"GeneratedBy", "FormatName", "FormatVersion", "LengthUnit"},
 *   "NextID": n,
 *   "Styles": [ {"Name","Kind","Parent","Para":{...},"Char":{...}} | null ],
 *   "Formats": [ {"Style": id, "Char": {...}} ],
 *   "Lists": [ {"Levels": [ {"Format","Start","Text","Indent","Hanging","RestartAfter","Label*"} ]} ],
 *   "Resources": [ {"_ByteStream_": {"_DataInfo_": {"MediaType","ByteLength"}, "Data": bytes}} ],
 *   "Revisions": [ {"Kind": "insert"|"delete", "Author", "Date"} ],     (tracked changes; formats' "Revision")
 *   "Comments": [ {"Author","Date","Text","Parent","Resolved","Start":[block,offset],"End":[block,offset]} ],
 *   "Document": {"_TreeNode_(root)": {"ID": 1}, "_TreeChildren_": [ ... ]},
 *   "Stories": [ {"_TreeNode_(story)": {...}, "_TreeChildren_": [...]} ]
 * }
 * Ids are positions: style i is Styles[i-1], format i is Formats[i-1].
 * Property objects carry only the fields that are set, so a key's
 * presence is its mask bit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_doc_internal.h"
#include "pd_json.h"

#define FORMAT_VERSION 1

static const char* const kind_names[] = { "root", "section", "paragraph", "float", "table", "row", "cell", "break",
                                          "story"
                                        };
static const char* const role_names[] = { "body", "title", "heading", "caption", "quote", "code", "equation",
                                          "figure", "raw", "term", "definition"
                                        };
static const char* const align_names[] = { "justify", "left", "right", "center" };
static const char* const inline_names[] = { "image", "equation", "field", "footnote", "link", "bookmark", "tab",
                                            "user", "raw"
                                          };
static const char* const field_names[] = { "page", "pages", "sectionpage", "refnumber", "refpage", "seq",
                                           "heading", "date"
                                         };
static const char* const num_names[] = { "bullet", "decimal", "loweralpha", "upperalpha", "lowerroman",
                                         "upperroman", "none"
                                       };
static const char* const wrap_names[] = { "none", "left", "right" };
static const char* const break_names[] = { "page", "column", "oddpage", "evenpage", "rule" };
static const char* const shift_names[] = { "none", "super", "sub" };
static const char* const mode_names[] = { "optimal", "greedy" };
static const char* const style_kind_names[] = { "paragraph", "character" };
static const char* const dir_names[] = { "auto", "ltr", "rtl" };

#define NAMES(t) (t), (int32_t)(sizeof(t) / sizeof((t)[0]))

static const char* name_of(const char* const* t, int32_t n, int32_t v) {
    return v >= 0 && v < n ? t[v] : t[0];
}

/* -1 if the node is not one of the names */
static int32_t enum_of(const pj_node* x, const char* const* t, int32_t n) {
    int32_t i;

    if (!x || x->type != PJ_STR) {
        return -1;
    }

    for (i = 0; i < n; i++) {
        if (strcmp(x->s, t[i]) == 0) {
            return i;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* save                                                               */
/* ------------------------------------------------------------------ */

static void put_int(pj_writer* w, const char* k, int64_t v) {
    pj_key(w, k);
    pj_int(w, v);
}

static void put_str(pj_writer* w, const char* k, const char* v) {
    pj_key(w, k);
    pj_cstr(w, v);
}

static void put_bool(pj_writer* w, const char* k, int v) {
    pj_key(w, k);
    pj_bool(w, v != 0);
}

static void save_pp(pj_writer* w, const pd_para_props* p) {
    uint32_t m = p->mask;

    pj_obj_begin(w);

    if (m & PD_PP_ALIGN) {
        put_str(w, "Align", name_of(NAMES(align_names), p->align));
    }

    if (m & PD_PP_INDENT_LEFT) {
        put_int(w, "IndentLeft", p->indent_left);
    }

    if (m & PD_PP_INDENT_RIGHT) {
        put_int(w, "IndentRight", p->indent_right);
    }

    if (m & PD_PP_INDENT_FIRST) {
        put_int(w, "IndentFirst", p->indent_first);
    }

    if (m & PD_PP_SPACE_BEFORE) {
        put_int(w, "SpaceBefore", p->space_before);
    }

    if (m & PD_PP_SPACE_AFTER) {
        put_int(w, "SpaceAfter", p->space_after);
    }

    if (m & PD_PP_LINE_SPACING) {
        put_int(w, "LineSpacing", p->line_spacing);
    }

    if (m & PD_PP_KEEP_NEXT) {
        put_bool(w, "KeepWithNext", p->keep_with_next);
    }

    if (m & PD_PP_KEEP_LINES) {
        put_bool(w, "KeepLines", p->keep_lines);
    }

    if (m & PD_PP_WIDOWS) {
        put_int(w, "Widows", p->widows);
    }

    if (m & PD_PP_ORPHANS) {
        put_int(w, "Orphans", p->orphans);
    }

    if (m & PD_PP_BREAK_BEFORE) {
        put_bool(w, "PageBreakBefore", p->page_break_before);
    }

    if (m & PD_PP_HYPHENATE) {
        put_bool(w, "Hyphenate", p->hyphenate);
    }

    if (m & PD_PP_BREAK_MODE) {
        put_str(w, "BreakMode", name_of(NAMES(mode_names), p->break_mode));
    }

    if (m & PD_PP_NEXT_STYLE) {
        put_int(w, "NextStyle", p->next_style);
    }

    if (m & PD_PP_BORDER) {
        put_int(w, "BorderColor", p->border_color);
        put_int(w, "BorderWidth", p->border_width);

        if (p->border_sides) {
            put_int(w, "BorderSides", p->border_sides);
        }

        if (p->border_space) {
            put_int(w, "BorderSpace", p->border_space);
        }
    }

    if (m & PD_PP_SHADING) {
        put_int(w, "Shading", p->shading);
    }

    if (m & PD_PP_DIRECTION) {
        put_str(w, "Direction", name_of(NAMES(dir_names), p->direction));
    }

    if (m & PD_PP_CONTEXTUAL) {
        put_bool(w, "Contextual", p->contextual);
    }

    if (m & PD_PP_TABS) {   /* [[position, align, leader], ...] */
        int32_t i;

        put_int(w, "TabInterval", p->tab_interval);
        pj_key(w, "Tabs");
        pj_arr_begin(w);

        for (i = 0; i < p->ntabs && i < PD_MAX_TABS; i++) {
            pj_arr_begin(w);
            pj_int(w, p->tabs[i].position);
            pj_int(w, p->tabs[i].align);
            pj_int(w, p->tabs[i].leader);
            pj_arr_end(w);
        }

        pj_arr_end(w);
    }

    pj_obj_end(w);
}

static void save_cp(pj_writer* w, const pd_char_props* c) {
    uint32_t m = c->mask;

    pj_obj_begin(w);

    if (m & PD_CP_FAMILY) {
        put_str(w, "Family", c->family);
    }

    if (m & PD_CP_SIZE) {
        put_int(w, "Size", c->size);
    }

    if (m & PD_CP_WEIGHT) {
        put_int(w, "Weight", c->weight);
    }

    if (m & PD_CP_ITALIC) {
        put_bool(w, "Italic", c->italic);
    }

    if (m & PD_CP_COLOR) {
        put_int(w, "Color", c->color);
    }

    if (m & PD_CP_BACKGROUND) {
        put_int(w, "Background", c->background);
    }

    if (m & PD_CP_UNDERLINE) {
        put_int(w, "Underline", c->underline);
    }

    if (m & PD_CP_STRIKE) {
        put_int(w, "Strike", c->strike);
    }

    if (m & PD_CP_SHIFT) {
        put_str(w, "Shift", name_of(NAMES(shift_names), c->shift));
    }

    if (m & PD_CP_LETTERSPACE) {
        put_int(w, "LetterSpace", c->letter_space);
    }

    if (m & PD_CP_KERNING) {
        put_bool(w, "Kerning", c->kerning);
    }

    if (m & PD_CP_LANG) {
        put_str(w, "Lang", c->lang);
    }

    if (m & PD_CP_SMALLCAPS) {
        put_bool(w, "SmallCaps", c->small_caps);
    }

    if (m & PD_CP_LINK) {
        put_int(w, "Link", c->link_target);
    }

    if (m & PD_CP_CAPS) {
        put_bool(w, "Caps", c->caps);
    }

    if (m & PD_CP_HIDDEN) {
        put_bool(w, "Hidden", c->hidden);
    }

    if (m & PD_CP_POSITION) {
        put_int(w, "Position", c->position);
    }

    if (m & PD_CP_REVISION) {
        put_int(w, "Revision", c->revision);
    }

    pj_obj_end(w);
}

/* saving: formats are written compacted, numbered by first use */
typedef struct {
    const pd_doc* d;
    uint32_t* fmap;     /* old format id -> saved id; 0 = default */
    pd_format_id* order;
    int32_t nused;
    uint32_t maxid;
} saver;

static uint32_t fmt_id(const saver* sv, pd_format_id f) {
    return f ? sv->fmap[f] : 0;
}

static void collect(saver* sv, const blk* b) {
    int32_t i;

    sv->maxid = b->id > sv->maxid ? b->id : sv->maxid;

    for (i = -1; i < b->st.nruns; i++) {
        pd_format_id f = i < 0 ? b->st.empty_format : b->st.runs[i].format;

        if (f && !sv->fmap[f]) {
            sv->order[sv->nused++] = f;
            sv->fmap[f] = (uint32_t)sv->nused;
        }
    }

    for (i = 0; i < b->nkids; i++) {
        collect(sv, sv->d->tab[b->kids[i]]);
    }
}

static void save_block(pj_writer* w, const saver* sv, const blk* b) {
    const pd_doc* d = sv->d;
    char tag[32];
    const bstate* s = &b->st;
    int32_t i;

    snprintf(tag, sizeof(tag), "_TreeNode_(%s)", name_of(NAMES(kind_names), b->kind));
    pj_obj_begin(w);
    pj_key(w, tag);
    pj_obj_begin(w);
    put_int(w, "ID", b->id);

    switch (b->kind) {
        case PD_BLOCK_PARAGRAPH:
            put_int(w, "Style", s->style);
            put_str(w, "Role", name_of(NAMES(role_names), s->role));
            put_int(w, "Level", s->level);

            if (s->list) {
                put_int(w, "List", s->list);
                put_int(w, "ListLevel", s->list_level);
            }

            if (s->pp.mask) {
                pj_key(w, "Para");
                save_pp(w, &s->pp);
            }

            if (s->empty_format) {
                put_int(w, "EmptyFormat", fmt_id(sv, s->empty_format));
            }

            if (s->at.quote_depth || s->at.task || s->at.loose || s->at.lang[0] || s->at.cont || s->at.div_class[0]) {
                pj_key(w, "Attrs");
                pj_obj_begin(w);

                if (s->at.quote_depth) {
                    put_int(w, "QuoteDepth", s->at.quote_depth);
                }

                if (s->at.task) {
                    put_int(w, "Task", s->at.task);
                }

                if (s->at.loose) {
                    put_bool(w, "Loose", s->at.loose);
                }

                if (s->at.lang[0]) {
                    put_str(w, "Language", s->at.lang);
                }

                if (s->at.cont) {
                    put_bool(w, "Continues", s->at.cont);
                }

                if (s->at.div_class[0]) {
                    put_str(w, "Div", s->at.div_class);
                }

                pj_obj_end(w);
            }

            pj_key(w, "Text");
            pj_str(w, s->text ? s->text : "", s->len);

            if (s->nruns) {
                int64_t* m = (int64_t*)malloc((size_t)s->nruns * 3 * sizeof(int64_t));

                if (!m) {
                    w->err = 1;
                    break;
                }

                for (i = 0; i < s->nruns; i++) {
                    m[3 * i] = s->runs[i].start;
                    m[3 * i + 1] = s->runs[i].end;
                    m[3 * i + 2] = fmt_id(sv, s->runs[i].format);
                }

                pj_key(w, "Runs");
                pj_int_matrix(w, m, s->nruns, 3);
                free(m);
            }

            if (s->ninl) {
                pj_key(w, "Inlines");
                pj_arr_begin(w);

                for (i = 0; i < s->ninl; i++) {
                    const pd_inline* o = &s->inl[i].obj;

                    pj_obj_begin(w);
                    put_int(w, "Offset", s->inl[i].offset);
                    put_str(w, "Kind", name_of(NAMES(inline_names), o->kind));
                    put_int(w, "Width", o->width);
                    put_int(w, "Height", o->height);
                    put_int(w, "Depth", o->depth);

                    if (o->kind == PD_INLINE_IMAGE) {
                        put_int(w, "Resource", o->resource);
                    }

                    if (o->kind == PD_INLINE_FIELD) {
                        put_str(w, "Field", name_of(NAMES(field_names), o->field));
                    }

                    if (o->target) {
                        put_int(w, "Target", o->target);
                    }

                    if (o->level) {
                        put_int(w, "Level", o->level);
                    }

                    if (o->name[0]) {
                        put_str(w, "Name", o->name);
                    }

                    if (s->inl[i].source && o->source_len) {
                        pj_key(w, "Source");
                        pj_str(w, o->source, (size_t)o->source_len);
                    }

                    if (o->title_len) {
                        pj_key(w, "Title");
                        pj_str(w, o->title, (size_t)o->title_len);
                    }

                    if (o->alt_len) {
                        pj_key(w, "Alt");
                        pj_str(w, o->alt, (size_t)o->alt_len);
                    }

                    if (o->user) {
                        put_int(w, "User", o->user);
                    }

                    pj_obj_end(w);
                }

                pj_arr_end(w);
            }

            break;

        case PD_BLOCK_SECTION: {
            const pd_section_props* p = &s->sp;

            pj_key(w, "Page");
            pj_obj_begin(w);
            put_int(w, "Width", p->page_width);
            put_int(w, "Height", p->page_height);
            put_int(w, "MarginTop", p->margin_top);
            put_int(w, "MarginBottom", p->margin_bottom);
            put_int(w, "MarginLeft", p->margin_left);
            put_int(w, "MarginRight", p->margin_right);
            put_int(w, "HeaderDistance", p->header_distance);
            put_int(w, "FooterDistance", p->footer_distance);
            put_int(w, "Columns", p->columns);
            put_int(w, "ColumnGap", p->column_gap);
            put_int(w, "FirstPageNumber", p->first_page_number);
            put_str(w, "PageNumberFormat", name_of(NAMES(num_names), p->page_number_format));
            put_bool(w, "TitlePage", p->title_page);
            put_bool(w, "FacingPages", p->facing_pages);
            put_int(w, "Header", p->header);
            put_int(w, "HeaderFirst", p->header_first);
            put_int(w, "HeaderEven", p->header_even);
            put_int(w, "Footer", p->footer);
            put_int(w, "FooterFirst", p->footer_first);
            put_int(w, "FooterEven", p->footer_even);

            if (p->continuous) {
                put_bool(w, "Continuous", p->continuous);
            }

            if (p->page_breaking) {
                put_str(w, "PageBreaking", p->page_breaking == PD_PAGES_OPTIMAL ? "optimal" : "greedy");
            }

            put_int(w, "FootnoteSkip", p->footnote_skip);

            if (p->add_spacing) {
                put_bool(w, "AddSpacing", p->add_spacing);
            }

            if (p->mirror_margins) {
                put_int(w, "MirrorMargins", p->mirror_margins);
            }

            if (p->gutter) {
                put_int(w, "Gutter", p->gutter);
            }

            if (p->page_valign) {
                put_int(w, "PageVAlign", p->page_valign);
            }

            if (p->line_numbers) {
                put_int(w, "LineNumbers", p->line_numbers);
                put_int(w, "LineNumberStart", p->line_number_start);
                put_int(w, "LineNumberDistance", p->line_number_distance);
                put_int(w, "LineNumberRestart", p->line_number_restart);
            }

            pj_obj_end(w);
            break;
        }

        case PD_BLOCK_TABLE: {
            const pd_table_props* p = &s->tp;

            pj_key(w, "Table");
            pj_obj_begin(w);
            put_int(w, "Width", p->width);
            put_str(w, "Align", name_of(NAMES(align_names), p->align));
            put_int(w, "HeaderRows", p->header_rows);
            put_int(w, "CellPadding", p->cell_padding);
            put_int(w, "Border", p->border);
            put_int(w, "BorderColor", (int64_t)p->border_color);

            if (p->indent) {
                put_int(w, "Indent", p->indent);
            }

            if (p->width_pct) {
                put_int(w, "WidthPerMille", p->width_pct);
            }

            if (p->border_sides) {
                put_int(w, "BorderSides", p->border_sides);
            }

            if (p->cell_padding_v >= 0) {
                put_int(w, "CellPaddingV", p->cell_padding_v);
            }

            if (p->ncols) {
                pj_key(w, "ColumnWidths");
                pj_arr_begin(w);

                for (i = 0; i < p->ncols; i++) {
                    pj_int(w, p->col_width[i]);
                }

                pj_arr_end(w);
            }

            pj_obj_end(w);
            break;
        }

        case PD_BLOCK_CELL: {
            const pd_cell_props* p = &s->cell;

            if (p->col_span != 1 || p->valign || p->background || p->merge_up || p->min_height || p->border_set) {
                pj_key(w, "Cell");
                pj_obj_begin(w);
                put_int(w, "ColumnSpan", p->col_span);
                put_int(w, "VerticalAlign", p->valign);
                put_int(w, "Background", (int64_t)p->background);

                if (p->merge_up) {
                    put_int(w, "MergeUp", p->merge_up);
                }

                if (p->min_height) {
                    put_int(w, "MinHeight", p->min_height);
                }

                if (p->border_set) {
                    put_int(w, "BorderSet", p->border_set);
                    put_int(w, "BorderOn", p->border_on);
                    put_int(w, "BorderWidth", p->border_width);
                    put_int(w, "BorderColor", (int64_t)p->border_color);
                }
                pj_obj_end(w);
            }

            break;
        }

        case PD_BLOCK_FLOAT: {
            const pd_float_props* p = &s->fp;

            pj_key(w, "Float");
            pj_obj_begin(w);
            put_int(w, "Placement", p->placement);
            put_str(w, "Wrap", name_of(NAMES(wrap_names), p->wrap));
            put_int(w, "Width", p->width);
            put_int(w, "WidthFraction", p->width_fraction);
            put_bool(w, "SpanColumns", p->span_columns);
            put_int(w, "Gap", p->gap);
            put_str(w, "Sequence", p->sequence);

            if (p->placement & PD_PLACE_OFFSET) {
                put_int(w, "OffsetX", p->offset_x);
            }

            pj_obj_end(w);
            break;
        }

        case PD_BLOCK_BREAK:
            put_str(w, "Break", name_of(NAMES(break_names), s->break_kind));
            break;
    }

    pj_obj_end(w);

    if (b->nkids) {
        pj_key(w, "_TreeChildren_");
        pj_arr_begin(w);

        for (i = 0; i < b->nkids; i++) {
            save_block(w, sv, d->tab[b->kids[i]]);
        }

        pj_arr_end(w);
    }

    pj_obj_end(w);
}

static void put_pos(pj_writer* w, const char* k, const pd_pos* p) {
    pj_key(w, k);
    pj_arr_begin(w);
    pj_int(w, p->block);
    pj_int(w, p->offset);
    pj_arr_end(w);
}

/* live comments only, renumbered in order; replies refer to the new numbers */
static void save_comments(pj_writer* w, const pd_doc* d) {
    int32_t i, j, n = 0;
    pd_comment q;

    for (i = 1; i <= pd_doc_comment_count(d); i++) {
        n += pd_doc_comment_get(d, (pd_comment_id)i, &q) == PD_OK;
    }

    if (!n) {
        return;
    }

    pj_key(w, "Comments");
    pj_arr_begin(w);

    for (i = 1; i <= pd_doc_comment_count(d); i++) {
        pd_comment c;

        if (pd_doc_comment_get(d, (pd_comment_id)i, &c) != PD_OK) {
            continue;
        }

        pj_obj_begin(w);
        put_str(w, "Author", c.author);

        if (c.date[0]) {
            put_str(w, "Date", c.date);
        }

        pj_key(w, "Text");
        pj_str(w, c.text, c.text_len);

        if (c.parent) {
            int32_t no = 0;

            for (j = 1; j <= (int32_t)c.parent; j++) {
                no += pd_doc_comment_get(d, (pd_comment_id)j, &q) == PD_OK;
            }

            put_int(w, "Parent", no);
        } else {
            put_pos(w, "Start", &c.range.start);
            put_pos(w, "End", &c.range.end);
        }

        if (c.resolved) {
            put_bool(w, "Resolved", 1);
        }

        pj_obj_end(w);
    }

    pj_arr_end(w);
}

pd_status pd_doc_save(const pd_doc* d, pd_jdata_format format, pd_writer fn, void* user) {
    pj_writer w;
    saver sv;
    int32_t i, k;
    blk* sr;
    pd_status st;
    uint32_t* rmap = NULL, nrev = 0;

    if (!d || !fn || (format != PD_JDATA_TEXT && format != PD_JDATA_BINARY)) {
        return PD_ERR_ARG;
    }

    /* only what the content uses is saved, so a save depends on content
       alone and not on the editing history that produced it */
    memset(&sv, 0, sizeof(sv));
    sv.d = d;
    sv.fmap = (uint32_t*)calloc((size_t)d->nformats + 1, sizeof(uint32_t));
    sv.order = (pd_format_id*)calloc((size_t)d->nformats + 1, sizeof(pd_format_id));

    if (!sv.fmap || !sv.order) {
        free(sv.fmap);
        free(sv.order);
        return PD_ERR_NOMEM;
    }

    sv.maxid = PD_STORYROOT_ID + 1;
    collect(&sv, d->tab[PD_ROOT_ID]);
    sr = d->tab[PD_STORYROOT_ID];

    for (i = 0; i < sr->nkids; i++) {
        collect(&sv, d->tab[sr->kids[i]]);
    }

    pj_init(&w, format == PD_JDATA_BINARY, fn, user);
    pj_obj_begin(&w);
    pj_key(&w, "_DataInfo_");
    pj_obj_begin(&w);
    put_str(&w, "GeneratedBy", "Parade 0.1.0");
    put_str(&w, "FormatName", "ParadeDocument");
    put_int(&w, "FormatVersion", FORMAT_VERSION);
    put_str(&w, "LengthUnit", "sp (1/65536 pt)");
    pj_obj_end(&w);
    put_int(&w, "NextID", (int64_t)sv.maxid + 1);

    pj_key(&w, "Styles");
    pj_arr_begin(&w);

    for (i = 0; i < d->nstyles; i++) {
        const dstyle* s = &d->styles[i];

        if (!s->alive) {
            pj_null(&w);
            continue;
        }

        pj_obj_begin(&w);
        put_str(&w, "Name", s->name);
        put_str(&w, "Kind", name_of(NAMES(style_kind_names), s->kind));
        put_int(&w, "Parent", s->parent);

        if (s->kind == PD_STYLE_PARAGRAPH) {
            pj_key(&w, "Para");
            save_pp(&w, &s->pp);
        }

        pj_key(&w, "Char");
        save_cp(&w, &s->cp);
        pj_obj_end(&w);
    }

    pj_arr_end(&w);

    /* revisions in use, numbered by first use like the formats */
    if (d->nrevs && (rmap = (uint32_t*)calloc((size_t)d->nrevs + 1, sizeof(uint32_t))) == NULL) {
        free(sv.fmap);
        free(sv.order);
        return PD_ERR_NOMEM;
    }

    for (i = 0; i < sv.nused; i++) {
        const dformat* f = &d->formats[sv.order[i] - 1];

        if ((f->cp.mask & PD_CP_REVISION) && f->cp.revision && (int32_t)f->cp.revision <= d->nrevs &&
                !rmap[f->cp.revision]) {
            rmap[f->cp.revision] = ++nrev;
        }
    }

    if (nrev) {
        pj_key(&w, "Revisions");
        pj_arr_begin(&w);

        for (k = 1; k <= (int32_t)nrev; k++) {
            int32_t j;

            for (j = 1; j <= d->nrevs && rmap[j] != (uint32_t)k; j++) {
            }

            pj_obj_begin(&w);
            put_str(&w, "Kind", d->revs[j - 1].kind == PD_REV_DELETE ? "delete" : "insert");
            put_str(&w, "Author", d->revs[j - 1].author);
            put_str(&w, "Date", d->revs[j - 1].date);
            pj_obj_end(&w);
        }

        pj_arr_end(&w);
    }

    pj_key(&w, "Formats");
    pj_arr_begin(&w);

    for (i = 0; i < sv.nused; i++) {
        const dformat* f = &d->formats[sv.order[i] - 1];
        pd_char_props cp = f->cp;

        if ((cp.mask & PD_CP_REVISION) && rmap) {
            cp.revision = (int32_t)cp.revision <= d->nrevs ? rmap[cp.revision] : 0;
        }

        if (!cp.revision) {
            cp.mask &= ~PD_CP_REVISION;
        }

        pj_obj_begin(&w);
        put_int(&w, "Style", f->style);
        pj_key(&w, "Char");
        save_cp(&w, &cp);
        pj_obj_end(&w);
    }

    pj_arr_end(&w);
    free(rmap);

    pj_key(&w, "Lists");
    pj_arr_begin(&w);

    for (i = 0; i < d->nlists; i++) {
        pj_obj_begin(&w);
        pj_key(&w, "Levels");
        pj_arr_begin(&w);

        for (k = 0; k < d->lists[i].n; k++) {
            const pd_list_level* L = &d->lists[i].lv[k];

            pj_obj_begin(&w);
            put_str(&w, "Format", name_of(NAMES(num_names), L->format));
            put_int(&w, "Start", L->start);
            put_str(&w, "Text", L->text);
            put_int(&w, "Indent", L->indent);
            put_int(&w, "Hanging", L->hanging);

            if (L->restart_after) {
                put_int(&w, "RestartAfter", L->restart_after);
            }

            if (L->label_family[0]) {
                put_str(&w, "LabelFamily", L->label_family);
            }

            if (L->label_size) {
                put_int(&w, "LabelSize", L->label_size);
            }

            if (L->label_weight) {
                put_int(&w, "LabelWeight", L->label_weight);
            }

            if (L->label_italic) {
                put_int(&w, "LabelItalic", L->label_italic);
            }

            if (L->label_color) {
                put_int(&w, "LabelColor", (int64_t)L->label_color);
            }

            pj_obj_end(&w);
        }

        pj_arr_end(&w);
        pj_obj_end(&w);
    }

    pj_arr_end(&w);

    pj_key(&w, "Resources");
    pj_arr_begin(&w);

    for (i = 0; i < d->nres; i++) {
        pj_obj_begin(&w);
        pj_key(&w, "_ByteStream_");
        pj_obj_begin(&w);
        pj_key(&w, "_DataInfo_");
        pj_obj_begin(&w);
        put_str(&w, "MediaType", d->res[i].mime);
        put_int(&w, "ByteLength", (int64_t)d->res[i].len);
        pj_obj_end(&w);
        pj_key(&w, "Data");
        pj_bytes(&w, d->res[i].data, d->res[i].len);
        pj_obj_end(&w);
        pj_obj_end(&w);
    }

    pj_arr_end(&w);

    if (d->meta_len) {
        pj_key(&w, "Metadata");
        pj_str(&w, d->meta, d->meta_len);
    }

    pj_key(&w, "Document");
    save_block(&w, &sv, d->tab[PD_ROOT_ID]);

    pj_key(&w, "Stories");
    pj_arr_begin(&w);

    for (i = 0; i < sr->nkids; i++) {
        save_block(&w, &sv, d->tab[sr->kids[i]]);
    }

    pj_arr_end(&w);
    save_comments(&w, d);
    pj_obj_end(&w);
    st = pj_finish(&w) ? PD_ERR_IO : PD_OK;
    free(sv.fmap);
    free(sv.order);
    return st;
}

/* ------------------------------------------------------------------ */
/* load                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_doc* d;
    int bad;
    pd_block_id* story_refs;    /* checked once every block exists */
    int32_t nrefs, caprefs;
} loader;

#ifdef PD_LOAD_DEBUG
    #define BAD_AT() fprintf(stderr, "load rejected at %s:%d\n", __FILE__, __LINE__)
#else
    #define BAD_AT() ((void)0)
#endif
#define REQUIRE(c) do { if (!(c)) { BAD_AT(); L->bad = 1; return; } } while (0)
#define REQUIRE0(c) do { if (!(c)) { BAD_AT(); L->bad = 1; return 0; } } while (0)

static int in_range(const pj_node* x, int64_t lo, int64_t hi) {
    return pj_is_int(x) && x->i >= lo && x->i <= hi;
}

static int64_t int_or(const pj_node* x, int64_t def, int64_t lo, int64_t hi, loader* L) {
    if (!x) {
        return def;
    }

    if (x->type == PJ_BOOL) {
        return x->i;
    }

    if (!in_range(x, lo, hi)) {
        L->bad = 1;
        return def;
    }

    return x->i;
}

/* a required integer: missing or out of range marks the load bad */
static int64_t req_int(const pj_node* x, int64_t lo, int64_t hi, loader* L) {
    if (!in_range(x, lo, hi)) {
        L->bad = 1;
        return lo;
    }

    return x->i;
}

static void copy_name(const pj_node* x, char* dst, size_t cap, loader* L) {
    if (!x) {
        return;
    }

    if (x->type != PJ_STR || x->len >= cap || !pd_doc_utf8_valid(x->s, x->len, 0)) {
        L->bad = 1;
        return;
    }

    memcpy(dst, x->s, x->len);
    dst[x->len] = '\0';
}

#define SP_MAX 0x7FFFFFFF
#define SP_MIN (-0x7FFFFFFF - 1)

static void load_pp(const pj_node* o, pd_para_props* p, loader* L) {
    const pj_node* x;

    memset(p, 0, sizeof(*p));

    if (!o) {
        return;
    }

    REQUIRE(o->type == PJ_OBJ);
#define F(key, bit, field, lo, hi) if ((x = pj_get(o, key))) { p->mask |= (bit); p->field = (int32_t)int_or(x, 0, lo, hi, L); }

    if ((x = pj_get(o, "Align"))) {
        p->mask |= PD_PP_ALIGN;
        p->align = enum_of(x, NAMES(align_names));
        REQUIRE(p->align >= 0);
    }

    F("IndentLeft", PD_PP_INDENT_LEFT, indent_left, SP_MIN, SP_MAX);
    F("IndentRight", PD_PP_INDENT_RIGHT, indent_right, SP_MIN, SP_MAX);
    F("IndentFirst", PD_PP_INDENT_FIRST, indent_first, SP_MIN, SP_MAX);
    F("SpaceBefore", PD_PP_SPACE_BEFORE, space_before, SP_MIN, SP_MAX);
    F("SpaceAfter", PD_PP_SPACE_AFTER, space_after, SP_MIN, SP_MAX);
    F("LineSpacing", PD_PP_LINE_SPACING, line_spacing, 0, 100000);
    F("KeepWithNext", PD_PP_KEEP_NEXT, keep_with_next, 0, 1);
    F("KeepLines", PD_PP_KEEP_LINES, keep_lines, 0, 1);
    F("Widows", PD_PP_WIDOWS, widows, 0, 1000);
    F("Orphans", PD_PP_ORPHANS, orphans, 0, 1000);
    F("PageBreakBefore", PD_PP_BREAK_BEFORE, page_break_before, 0, 1);
    F("Hyphenate", PD_PP_HYPHENATE, hyphenate, 0, 1);

    if ((x = pj_get(o, "BreakMode"))) {
        p->mask |= PD_PP_BREAK_MODE;
        p->break_mode = enum_of(x, NAMES(mode_names));
        REQUIRE(p->break_mode >= 0);
    }

    if ((x = pj_get(o, "NextStyle"))) {
        p->mask |= PD_PP_NEXT_STYLE;
        p->next_style = (pd_style_id)int_or(x, 0, 0, L->d->nstyles, L);
    }

    if ((x = pj_get(o, "BorderColor"))) {
        p->mask |= PD_PP_BORDER;
        p->border_color = (uint32_t)int_or(x, 0, 0, 0xFFFFFFFFLL, L);
        p->border_width = (pd_sp)int_or(pj_get(o, "BorderWidth"), 0, 0, SP_MAX, L);
        p->border_sides = (int32_t)int_or(pj_get(o, "BorderSides"), 0, 0, 31, L);
        p->border_space = (pd_sp)int_or(pj_get(o, "BorderSpace"), 0, 0, SP_MAX, L);
    }

    if ((x = pj_get(o, "Shading"))) {
        p->mask |= PD_PP_SHADING;
        p->shading = (uint32_t)int_or(x, 0, 0, 0xFFFFFFFFLL, L);
    }

    if ((x = pj_get(o, "Direction"))) {
        p->mask |= PD_PP_DIRECTION;
        p->direction = enum_of(x, NAMES(dir_names));
        REQUIRE(p->direction >= 0);
    }

    if ((x = pj_get(o, "Contextual"))) {
        p->mask |= PD_PP_CONTEXTUAL;
        p->contextual = (int32_t)int_or(x, 0, 0, 1, L);
    }

    if ((x = pj_get(o, "Tabs"))) {
        int32_t i;

        REQUIRE(x->type == PJ_ARR);
        p->mask |= PD_PP_TABS;
        p->tab_interval = (pd_sp)int_or(pj_get(o, "TabInterval"), 0, 0, SP_MAX, L);

        for (i = 0; i < x->n && i < PD_MAX_TABS; i++) {
            const pj_node* t = pj_at(x, i);

            REQUIRE(t && t->type == PJ_ARR && t->n == 3);
            p->tabs[i].position = (pd_sp)int_or(pj_at(t, 0), 0, SP_MIN, SP_MAX, L);
            p->tabs[i].align = (int32_t)int_or(pj_at(t, 1), 0, PD_TAB_LEFT, PD_TAB_DECIMAL, L);
            p->tabs[i].leader = (int32_t)int_or(pj_at(t, 2), 0, PD_LEADER_NONE, PD_LEADER_UNDERSCORE, L);
            p->ntabs = i + 1;
        }
    }

#undef F
}

static void load_cp(const pj_node* o, pd_char_props* c, loader* L) {
    const pj_node* x;

    memset(c, 0, sizeof(*c));

    if (!o) {
        return;
    }

    REQUIRE(o->type == PJ_OBJ);
#define F(key, bit, field, lo, hi) if ((x = pj_get(o, key))) { c->mask |= (bit); c->field = int_or(x, 0, lo, hi, L); }

    if ((x = pj_get(o, "Family"))) {
        c->mask |= PD_CP_FAMILY;
        copy_name(x, c->family, sizeof(c->family), L);
    }

    F("Size", PD_CP_SIZE, size, 1, SP_MAX);
    F("Weight", PD_CP_WEIGHT, weight, 1, 1000);
    F("Italic", PD_CP_ITALIC, italic, 0, 1);
    F("Color", PD_CP_COLOR, color, 0, 0xFFFFFFFFLL);
    F("Background", PD_CP_BACKGROUND, background, 0, 0xFFFFFFFFLL);
    F("Underline", PD_CP_UNDERLINE, underline, 0, PD_UNDERLINE_WORDS);
    F("Strike", PD_CP_STRIKE, strike, 0, 2);

    if ((x = pj_get(o, "Shift"))) {
        c->mask |= PD_CP_SHIFT;
        c->shift = enum_of(x, NAMES(shift_names));
        REQUIRE(c->shift >= 0);
    }

    F("LetterSpace", PD_CP_LETTERSPACE, letter_space, SP_MIN, SP_MAX);
    F("Kerning", PD_CP_KERNING, kerning, 0, 1);

    if ((x = pj_get(o, "Lang"))) {
        c->mask |= PD_CP_LANG;
        copy_name(x, c->lang, sizeof(c->lang), L);
    }

    F("SmallCaps", PD_CP_SMALLCAPS, small_caps, 0, 1);
    F("Link", PD_CP_LINK, link_target, 0, PD_MAX_BLOCKS);
    F("Caps", PD_CP_CAPS, caps, 0, 1);
    F("Hidden", PD_CP_HIDDEN, hidden, 0, 1);
    F("Position", PD_CP_POSITION, position, SP_MIN, SP_MAX);
    F("Revision", PD_CP_REVISION, revision, 1, 0xFFFFFF);
#undef F
    pd_doc_cp_normalize(c);
}

static void want_story(loader* L, pd_block_id id) {
    if (id && !pd_grow((void**)&L->story_refs, &L->caprefs, (int64_t)L->nrefs + 1, sizeof(pd_block_id))) {
        L->story_refs[L->nrefs++] = id;
    } else if (id) {
        L->bad = 1;
    }
}

static void load_paragraph(loader* L, const pj_node* o, bstate* s) {
    pd_doc* d = L->d;
    const pj_node* x, *runs, *inl;
    uint32_t pos = 0, nobj = 0, k;
    int32_t i;
    pd_list_id list;

    s->style = (pd_style_id)int_or(pj_get(o, "Style"), 0, 0, d->nstyles, L);
    REQUIRE(!s->style || (d->styles[s->style - 1].alive && d->styles[s->style - 1].kind == PD_STYLE_PARAGRAPH));
    s->role = (x = pj_get(o, "Role")) ? enum_of(x, NAMES(role_names)) : 0;
    REQUIRE(s->role >= 0);
    s->level = (int32_t)int_or(pj_get(o, "Level"), 0, 0, 6, L);
    REQUIRE(s->role == PD_ROLE_HEADING ? s->level >= 1 : s->level == 0);
    list = (pd_list_id)int_or(pj_get(o, "List"), 0, 0, d->nlists, L);
    s->list = list;
    s->list_level = (int32_t)int_or(pj_get(o, "ListLevel"), 0, 0, 8, L);
    REQUIRE(list ? s->list_level < d->lists[list - 1].n : s->list_level == 0);
    load_pp(pj_get(o, "Para"), &s->pp, L);
    REQUIRE(!(s->pp.mask & PD_PP_NEXT_STYLE) || !s->pp.next_style || d->styles[s->pp.next_style - 1].alive);
    s->empty_format = (pd_format_id)int_or(pj_get(o, "EmptyFormat"), 0, 0, d->nformats, L);

    if ((x = pj_get(o, "Attrs"))) {
        REQUIRE(x->type == PJ_OBJ);
        s->at.quote_depth = (int32_t)int_or(pj_get(x, "QuoteDepth"), 0, 0, 9, L);
        s->at.task = (int32_t)int_or(pj_get(x, "Task"), 0, 0, 2, L);
        s->at.loose = (int32_t)int_or(pj_get(x, "Loose"), 0, 0, 1, L);
        copy_name(pj_get(x, "Language"), s->at.lang, sizeof(s->at.lang), L);
        s->at.cont = (int32_t)int_or(pj_get(x, "Continues"), 0, 0, 1, L);
        copy_name(pj_get(x, "Div"), s->at.div_class, sizeof(s->at.div_class), L);
    }

    x = pj_get(o, "Text");
    REQUIRE(x && x->type == PJ_STR && x->len < 0x40000000 && pd_doc_utf8_valid(x->s, x->len, 1));

    if (x->len) {
        s->text = (char*)malloc(x->len + 1);
        REQUIRE(s->text);
        memcpy(s->text, x->s, x->len + 1);
        s->len = (uint32_t)x->len;
        s->cap = s->len + 1;
    }

    /* runs: contiguous, covering the text, on character boundaries */
    runs = pj_get(o, "Runs");
    REQUIRE((s->len == 0) == (runs == NULL || runs->n == 0));

    if (runs) {
        REQUIRE(runs->type == PJ_ARR && runs->n <= (int32_t)s->len);
        s->runs = (pd_run*)malloc((size_t)(runs->n ? runs->n : 1) * sizeof(pd_run));
        REQUIRE(s->runs);
        s->caprun = runs->n;

        for (x = runs->child; x; x = x->next) {
            pd_run r;

            REQUIRE(x->type == PJ_ARR && x->n == 3);
            r.start = (uint32_t)req_int(pj_at(x, 0), 0, s->len, L);
            r.end = (uint32_t)req_int(pj_at(x, 1), 0, s->len, L);
            r.format = (pd_format_id)req_int(pj_at(x, 2), 0, d->nformats, L);
            REQUIRE(!L->bad && r.start == pos && r.end > r.start);
            REQUIRE(r.end == s->len || (s->text[r.end] & 0xC0) != 0x80);
            s->runs[s->nruns++] = r;
            pos = r.end;
        }

        REQUIRE(pos == s->len);
    }

    /* every U+FFFC is an inline object and every inline object is a U+FFFC */
    for (k = 0; k + 2 < s->len; k++) {
        nobj += memcmp(s->text + k, "\xEF\xBF\xBC", 3) == 0;
    }

    inl = pj_get(o, "Inlines");
    REQUIRE((inl ? (uint32_t)inl->n : 0) == nobj);

    if (inl) {
        int64_t last = -1;

        REQUIRE(inl->type == PJ_ARR);
        s->inl = (dinline*)calloc((size_t)(inl->n ? inl->n : 1), sizeof(dinline));
        REQUIRE(s->inl);
        s->capinl = inl->n;

        for (x = inl->child, i = 0; x; x = x->next, i++) {
            dinline* q = &s->inl[s->ninl];
            const pj_node* y;

            REQUIRE(x->type == PJ_OBJ);
            s->ninl++;
            q->offset = (uint32_t)req_int(pj_get(x, "Offset"), 0, (int64_t)s->len - 3, L);
            REQUIRE(!L->bad && (int64_t)q->offset > last && memcmp(s->text + q->offset, "\xEF\xBF\xBC", 3) == 0);
            last = q->offset;
            q->obj.kind = enum_of(pj_get(x, "Kind"), NAMES(inline_names));
            REQUIRE(q->obj.kind >= 0);
            q->obj.width = (pd_sp)int_or(pj_get(x, "Width"), 0, 0, SP_MAX, L);
            q->obj.height = (pd_sp)int_or(pj_get(x, "Height"), 0, SP_MIN, SP_MAX, L);
            q->obj.depth = (pd_sp)int_or(pj_get(x, "Depth"), 0, SP_MIN, SP_MAX, L);
            q->obj.resource = (pd_res_id)int_or(pj_get(x, "Resource"), 0, 0, d->nres, L);
            REQUIRE(q->obj.kind != PD_INLINE_IMAGE || q->obj.resource >= 1 || pj_get(x, "Source"));

            if ((y = pj_get(x, "Field"))) {
                q->obj.field = enum_of(y, NAMES(field_names));
                REQUIRE(q->obj.field >= 0);
            }

            q->obj.target = (pd_block_id)int_or(pj_get(x, "Target"), 0, 0, PD_MAX_BLOCKS, L);

            if (q->obj.kind == PD_INLINE_FOOTNOTE) {
                REQUIRE(q->obj.target);
                want_story(L, q->obj.target);
            }

            q->obj.level = (int32_t)int_or(pj_get(x, "Level"), 0, 0, 9, L);
            copy_name(pj_get(x, "Name"), q->obj.name, sizeof(q->obj.name), L);
            q->obj.user = (int32_t)int_or(pj_get(x, "User"), 0, INT32_MIN, INT32_MAX, L);

            {
                const pj_node* sv2[3];
                int32_t* ln[3];
                size_t total = 3;
                int k2;
                char* p2;

                sv2[0] = pj_get(x, "Source");
                sv2[1] = pj_get(x, "Title");
                sv2[2] = pj_get(x, "Alt");
                ln[0] = &q->obj.source_len;
                ln[1] = &q->obj.title_len;
                ln[2] = &q->obj.alt_len;

                for (k2 = 0; k2 < 3; k2++) {
                    REQUIRE(!sv2[k2] || (sv2[k2]->type == PJ_STR && sv2[k2]->len < 0x10000000 &&
                                         pd_doc_utf8_valid(sv2[k2]->s, sv2[k2]->len, 0)));
                    total += sv2[k2] ? sv2[k2]->len : 0;
                }

                if (sv2[0] || sv2[1] || sv2[2]) {   /* one block: source\0title\0alt\0 */
                    q->source = p2 = (char*)malloc(total);
                    REQUIRE(q->source);

                    for (k2 = 0; k2 < 3; k2++) {
                        size_t n2 = sv2[k2] ? sv2[k2]->len : 0;

                        memcpy(p2, sv2[k2] ? sv2[k2]->s : "", n2);
                        p2[n2] = '\0';
                        p2 += n2 + 1;
                        *ln[k2] = (int32_t)n2;
                    }
                }
            }

            pd_inl_point(q);
        }
    }
}

static void load_section(loader* L, const pj_node* o, pd_section_props* p) {
    const pj_node* x = pj_get(o, "Page");

    pd_section_props_init(p);

    if (!x) {
        return;
    }

    REQUIRE(x->type == PJ_OBJ);
    p->page_width = (pd_sp)int_or(pj_get(x, "Width"), p->page_width, 1, SP_MAX, L);
    p->page_height = (pd_sp)int_or(pj_get(x, "Height"), p->page_height, 1, SP_MAX, L);
    p->margin_top = (pd_sp)int_or(pj_get(x, "MarginTop"), p->margin_top, 0, SP_MAX, L);
    p->margin_bottom = (pd_sp)int_or(pj_get(x, "MarginBottom"), p->margin_bottom, 0, SP_MAX, L);
    p->margin_left = (pd_sp)int_or(pj_get(x, "MarginLeft"), p->margin_left, 0, SP_MAX, L);
    p->margin_right = (pd_sp)int_or(pj_get(x, "MarginRight"), p->margin_right, 0, SP_MAX, L);
    p->header_distance = (pd_sp)int_or(pj_get(x, "HeaderDistance"), p->header_distance, 0, SP_MAX, L);
    p->footer_distance = (pd_sp)int_or(pj_get(x, "FooterDistance"), p->footer_distance, 0, SP_MAX, L);
    p->columns = (int32_t)int_or(pj_get(x, "Columns"), 1, 1, 16, L);
    p->column_gap = (pd_sp)int_or(pj_get(x, "ColumnGap"), p->column_gap, 0, SP_MAX, L);
    p->first_page_number = (int32_t)int_or(pj_get(x, "FirstPageNumber"), 1, 0, 1000000, L);
    p->page_number_format = pj_get(x, "PageNumberFormat") ? enum_of(pj_get(x, "PageNumberFormat"),
                            NAMES(num_names)) : PD_NUM_DECIMAL;
    REQUIRE(p->page_number_format >= 0);
    p->title_page = (int32_t)int_or(pj_get(x, "TitlePage"), 0, 0, 1, L);
    p->facing_pages = (int32_t)int_or(pj_get(x, "FacingPages"), 0, 0, 1, L);
    p->header = (pd_block_id)int_or(pj_get(x, "Header"), 0, 0, PD_MAX_BLOCKS, L);
    p->header_first = (pd_block_id)int_or(pj_get(x, "HeaderFirst"), 0, 0, PD_MAX_BLOCKS, L);
    p->header_even = (pd_block_id)int_or(pj_get(x, "HeaderEven"), 0, 0, PD_MAX_BLOCKS, L);
    p->footer = (pd_block_id)int_or(pj_get(x, "Footer"), 0, 0, PD_MAX_BLOCKS, L);
    p->footer_first = (pd_block_id)int_or(pj_get(x, "FooterFirst"), 0, 0, PD_MAX_BLOCKS, L);
    p->footer_even = (pd_block_id)int_or(pj_get(x, "FooterEven"), 0, 0, PD_MAX_BLOCKS, L);
    p->continuous = (int32_t)int_or(pj_get(x, "Continuous"), 0, 0, 1, L);
    p->footnote_skip = (pd_sp)int_or(pj_get(x, "FootnoteSkip"), p->footnote_skip, 0, SP_MAX, L);
    p->add_spacing = (int32_t)int_or(pj_get(x, "AddSpacing"), 0, 0, 1, L);
    p->line_numbers = (int32_t)int_or(pj_get(x, "LineNumbers"), 0, 0, 100, L);
    p->mirror_margins = (int32_t)int_or(pj_get(x, "MirrorMargins"), 0, -1, 1, L);
    p->gutter = (pd_sp)int_or(pj_get(x, "Gutter"), 0, 0, PD_PT(1000), L);
    p->page_valign = (int32_t)int_or(pj_get(x, "PageVAlign"), 0, 0, 2, L);
    p->line_number_start = (int32_t)int_or(pj_get(x, "LineNumberStart"), 0, 0, 1000000, L);
    p->line_number_distance = (pd_sp)int_or(pj_get(x, "LineNumberDistance"), 0, 0, PD_PT(1000), L);
    p->line_number_restart = (int32_t)int_or(pj_get(x, "LineNumberRestart"), 0, 0, 2, L);

    if (pj_get(x, "PageBreaking")) {
        static const char* const pb_names[] = { "greedy", "optimal" };

        p->page_breaking = enum_of(pj_get(x, "PageBreaking"), NAMES(pb_names));
        REQUIRE(p->page_breaking >= 0);
    }

    REQUIRE((int64_t)p->margin_left + p->margin_right < p->page_width &&
            (int64_t)p->margin_top + p->margin_bottom < p->page_height);
    want_story(L, p->header);
    want_story(L, p->header_first);
    want_story(L, p->header_even);
    want_story(L, p->footer);
    want_story(L, p->footer_first);
    want_story(L, p->footer_even);
}

static void load_table(loader* L, const pj_node* o, pd_table_props* p) {
    const pj_node* x = pj_get(o, "Table"), *c;

    if (!x) {
        return;
    }

    REQUIRE(x->type == PJ_OBJ);
    p->width = (pd_sp)int_or(pj_get(x, "Width"), 0, 0, SP_MAX, L);
    p->align = pj_get(x, "Align") ? enum_of(pj_get(x, "Align"), NAMES(align_names)) : PD_ALIGN_LEFT;
    REQUIRE(p->align >= 0);
    p->header_rows = (int32_t)int_or(pj_get(x, "HeaderRows"), 0, 0, 1000, L);
    p->cell_padding = (pd_sp)int_or(pj_get(x, "CellPadding"), p->cell_padding, 0, SP_MAX, L);
    p->border = (pd_sp)int_or(pj_get(x, "Border"), p->border, 0, SP_MAX, L);
    p->border_color = (uint32_t)int_or(pj_get(x, "BorderColor"), p->border_color, 0, 0xFFFFFFFFLL, L);
    p->indent = (pd_sp)int_or(pj_get(x, "Indent"), 0, -PD_PT(10000), PD_PT(10000), L);
    p->width_pct = (int32_t)int_or(pj_get(x, "WidthPerMille"), 0, 0, 1000, L);
    p->border_sides = (int32_t)int_or(pj_get(x, "BorderSides"), 0, 0, 63, L);
    p->cell_padding_v = (pd_sp)int_or(pj_get(x, "CellPaddingV"), -1, -1, SP_MAX, L);

    if ((c = pj_get(x, "ColumnWidths")) != NULL) {
        int32_t i;

        REQUIRE(c->type == PJ_ARR && c->n <= PD_TABLE_MAX_COLS);
        p->ncols = (int32_t)c->n;

        for (i = 0, c = c->child; c; c = c->next, i++) {
            p->col_width[i] = (pd_sp)int_or(c, 0, 0, SP_MAX, L);
        }
    }
}

static void load_cell(loader* L, const pj_node* o, pd_cell_props* p) {
    const pj_node* x = pj_get(o, "Cell");

    if (!x) {
        return;
    }

    REQUIRE(x->type == PJ_OBJ);
    p->col_span = (int32_t)int_or(pj_get(x, "ColumnSpan"), 1, 1, PD_TABLE_MAX_COLS, L);
    p->valign = (int32_t)int_or(pj_get(x, "VerticalAlign"), 0, 0, 2, L);
    p->background = (uint32_t)int_or(pj_get(x, "Background"), 0, 0, 0xFFFFFFFFLL, L);
    p->merge_up = (int32_t)int_or(pj_get(x, "MergeUp"), 0, 0, 1, L);
    p->min_height = (pd_sp)int_or(pj_get(x, "MinHeight"), 0, 0, SP_MAX, L);
    p->border_set = (int32_t)int_or(pj_get(x, "BorderSet"), 0, 0, 15, L);
    p->border_on = (int32_t)int_or(pj_get(x, "BorderOn"), 0, 0, 15, L) & p->border_set;
    p->border_width = (pd_sp)int_or(pj_get(x, "BorderWidth"), 0, 0, SP_MAX, L);
    p->border_color = (uint32_t)int_or(pj_get(x, "BorderColor"), 0, 0, 0xFFFFFFFFLL, L);
}

static void load_float(loader* L, const pj_node* o, pd_float_props* p) {
    const pj_node* x = pj_get(o, "Float");

    if (!x) {
        return;
    }

    REQUIRE(x->type == PJ_OBJ);
    p->placement = (uint32_t)int_or(pj_get(x, "Placement"), p->placement, 1, 63, L);
    REQUIRE(p->placement & 31);
    p->offset_x = (pd_sp)int_or(pj_get(x, "OffsetX"), 0, -PD_PT(10000), PD_PT(10000), L);
    p->wrap = pj_get(x, "Wrap") ? enum_of(pj_get(x, "Wrap"), NAMES(wrap_names)) : 0;
    REQUIRE(p->wrap >= 0);
    p->width = (pd_sp)int_or(pj_get(x, "Width"), 0, 0, SP_MAX, L);
    p->width_fraction = (int32_t)int_or(pj_get(x, "WidthFraction"), 1000, 0, 1000, L);
    p->span_columns = (int32_t)int_or(pj_get(x, "SpanColumns"), 0, 0, 1, L);
    p->gap = (pd_sp)int_or(pj_get(x, "Gap"), p->gap, 0, SP_MAX, L);
    memset(p->sequence, 0, sizeof(p->sequence));
    copy_name(pj_get(x, "Sequence"), p->sequence, sizeof(p->sequence), L);
}

/* one tree node; returns the block id or 0 */
static pd_block_id load_tree(loader* L, const pj_node* node, pd_block_id parent, int32_t parent_kind, int depth) {
    pd_doc* d = L->d;
    const pj_node* c, *data = NULL, *kids;
    int32_t kind = -1, i;
    int64_t id;
    blk* b;

    REQUIRE0(node && node->type == PJ_OBJ && depth < 64);

    for (c = node->child; c; c = c->next) {
        if (c->keylen > 12 && memcmp(c->key, "_TreeNode_(", 11) == 0 && c->key[c->keylen - 1] == ')') {
            REQUIRE0(!data);
            data = c;

            for (i = 0; i < (int32_t)(sizeof(kind_names) / sizeof(kind_names[0])); i++) {
                if (strlen(kind_names[i]) == c->keylen - 12 && memcmp(c->key + 11, kind_names[i], c->keylen - 12) == 0) {
                    kind = i;
                }
            }
        }
    }

    REQUIRE0(data && data->type == PJ_OBJ && kind >= 0);
    id = pj_int_or(pj_get(data, "ID"), -1);
    REQUIRE0(id >= 1 && (uint64_t)id < d->next_id && !d->tab[id] && (id == PD_ROOT_ID) == (kind == PD_BLOCK_ROOT) &&
             id != PD_STORYROOT_ID);
    REQUIRE0(kind == PD_BLOCK_ROOT ? parent == 0 : (parent_kind == -1 ? kind == PD_BLOCK_STORY :
             pd_doc_child_allowed(parent_kind, kind)));

    b = (blk*)calloc(1, sizeof(blk));
    REQUIRE0(b);
    b->kind = kind;
    b->id = (pd_block_id)id;
    b->parent = parent;
    pd_doc_bstate_init(&b->st, kind);
    d->tab[id] = b;

    if (kind == PD_BLOCK_PARAGRAPH) {
        load_paragraph(L, data, &b->st);
    } else if (kind == PD_BLOCK_SECTION) {
        load_section(L, data, &b->st.sp);
    } else if (kind == PD_BLOCK_FLOAT) {
        load_float(L, data, &b->st.fp);
    } else if (kind == PD_BLOCK_TABLE) {
        load_table(L, data, &b->st.tp);
    } else if (kind == PD_BLOCK_CELL) {
        load_cell(L, data, &b->st.cell);
    } else if (kind == PD_BLOCK_BREAK && pj_get(data, "Break")) {
        b->st.break_kind = enum_of(pj_get(data, "Break"), NAMES(break_names));
        REQUIRE0(b->st.break_kind >= 0);
    }

    if (L->bad) {
        return 0;
    }

    kids = pj_get(node, "_TreeChildren_");
    REQUIRE0(!kids || kids->type == PJ_ARR);
    /* containers are never empty; paragraphs and breaks have no children */
    REQUIRE0(kind == PD_BLOCK_PARAGRAPH || kind == PD_BLOCK_BREAK ? !kids || kids->n == 0 : kids && kids->n > 0);

    for (c = kids ? kids->child : NULL; c; c = c->next) {
        pd_block_id k = load_tree(L, c, b->id, kind, depth + 1);

        REQUIRE0(k && pd_doc_add_kid(b, k, -1) == 0);
    }

    return b->id;
}

static pd_doc* load_doc(const pj_node* r, loader* L) {
    pd_doc* d;
    const pj_node* x, *c;
    int64_t next;
    blk* sr;
    int32_t i;
    uint32_t k;

    if (!r || r->type != PJ_OBJ) {
        return NULL;
    }

    next = pj_int_or(pj_get(r, "NextID"), -1);
    x = pj_get(pj_get(r, "_DataInfo_"), "FormatVersion");

    if (next < 4 || next > PD_MAX_BLOCKS || (x && pj_int_or(x, 0) > FORMAT_VERSION)) {
        return NULL;
    }

    d = pd_doc_alloc();

    if (!d) {
        return NULL;
    }

    L->d = d;
    d->tab = (blk**)calloc((size_t)next, sizeof(blk*));

    if (!d->tab) {
        pd_doc_free(d);
        return NULL;
    }

    d->captab = d->next_id = (uint32_t)next;

    /* styles: positions are ids; parents and next styles are checked once all exist */
    x = pj_get(r, "Styles");

    if (x && x->type != PJ_ARR) {
        L->bad = 1;
    }

    for (c = x ? x->child : NULL; c && !L->bad; c = c->next) {
        dstyle s;

        memset(&s, 0, sizeof(s));

        if (c->type == PJ_OBJ) {
            copy_name(pj_get(c, "Name"), s.name, sizeof(s.name), L);
            s.kind = enum_of(pj_get(c, "Kind"), NAMES(style_kind_names));
            s.parent = (pd_style_id)pj_int_or(pj_get(c, "Parent"), 0);
            s.alive = 1;
            L->bad |= !s.name[0] || s.kind < 0 || pd_doc_style_find(d, s.name);
        } else if (c->type != PJ_NULL) {
            L->bad = 1;
        }

        if (!L->bad && pd_grow((void**)&d->styles, &d->capstyles, (int64_t)d->nstyles + 1, sizeof(dstyle))) {
            L->bad = 1;
        }

        if (!L->bad) {
            d->styles[d->nstyles++] = s;
        }
    }

    /* props need nstyles for NextStyle bounds */
    for (c = x ? x->child : NULL, i = 0; c && !L->bad; c = c->next, i++) {
        dstyle* s = &d->styles[i];
        pd_style_id a;
        int depth = 0;

        if (!s->alive) {
            continue;
        }

        load_pp(pj_get(c, "Para"), &s->pp, L);
        load_cp(pj_get(c, "Char"), &s->cp, L);
        pd_doc_pp_normalize(&s->pp);
        L->bad |= s->parent > (pd_style_id)d->nstyles;

        for (a = s->parent; !L->bad && a; a = d->styles[a - 1].parent) {
            L->bad |= a > (pd_style_id)d->nstyles || !d->styles[a - 1].alive || d->styles[a - 1].kind != s->kind ||
                      ++depth > d->nstyles;
        }

        L->bad |= (s->pp.mask & PD_PP_NEXT_STYLE) && s->pp.next_style && !d->styles[s->pp.next_style - 1].alive;
    }

    x = pj_get(r, "Revisions");

    for (c = x ? x->child : NULL; c && !L->bad; c = c->next) {
        pd_revision rv;
        pd_rev_id rid;
        const pj_node* kn = pj_get(c, "Kind");

        memset(&rv, 0, sizeof(rv));
        rv.kind = kn && kn->type == PJ_STR && kn->len == 6 && !memcmp(kn->s, "delete", 6) ? PD_REV_DELETE : PD_REV_INSERT;
        copy_name(pj_get(c, "Author"), rv.author, sizeof(rv.author), L);
        copy_name(pj_get(c, "Date"), rv.date, sizeof(rv.date), L);

        /* the same triple twice would fold into one id and shift the rest */
        if (!L->bad && (pd_doc_revision_add(d, &rv, &rid) != PD_OK || (int32_t)rid != d->nrevs)) {
            L->bad = 1;
        }
    }

    x = pj_get(r, "Formats");

    for (c = x ? x->child : NULL; c && !L->bad; c = c->next) {
        dformat f;

        memset(&f, 0, sizeof(f));
        f.style = (pd_style_id)int_or(pj_get(c, "Style"), 0, 0, d->nstyles, L);
        load_cp(pj_get(c, "Char"), &f.cp, L);
        L->bad |= (f.cp.mask & PD_CP_REVISION) && (int32_t)f.cp.revision > d->nrevs;
        L->bad |= c->type != PJ_OBJ || (f.style && d->styles[f.style - 1].alive &&
                                        d->styles[f.style - 1].kind != PD_STYLE_CHARACTER);

        if (!L->bad && pd_grow((void**)&d->formats, &d->capformats, (int64_t)d->nformats + 1, sizeof(dformat))) {
            L->bad = 1;
        }

        if (!L->bad) {      /* positions are ids, so keep them exactly as saved */
            d->formats[d->nformats++] = f;
        }
    }

    x = pj_get(r, "Lists");

    for (c = x ? x->child : NULL; c && !L->bad; c = c->next) {
        const pj_node* lv = pj_get(c, "Levels"), *y;
        dlist l;

        memset(&l, 0, sizeof(l));
        L->bad |= !lv || lv->type != PJ_ARR || lv->n < 1 || lv->n > 9;

        for (y = lv && !L->bad ? lv->child : NULL; y && !L->bad; y = y->next) {
            pd_list_level* v = &l.lv[l.n++];

            v->format = enum_of(pj_get(y, "Format"), NAMES(num_names));
            v->start = (int32_t)int_or(pj_get(y, "Start"), 1, -1000000, 1000000, L);
            copy_name(pj_get(y, "Text"), v->text, sizeof(v->text), L);
            v->indent = (pd_sp)int_or(pj_get(y, "Indent"), 0, SP_MIN, SP_MAX, L);
            v->hanging = (pd_sp)int_or(pj_get(y, "Hanging"), 0, SP_MIN, SP_MAX, L);
            v->restart_after = (int32_t)int_or(pj_get(y, "RestartAfter"), 0, -1, 9, L);

            if (pj_get(y, "LabelFamily")) {
                copy_name(pj_get(y, "LabelFamily"), v->label_family, sizeof(v->label_family), L);
            }

            v->label_size = (pd_sp)int_or(pj_get(y, "LabelSize"), 0, 0, PD_PT(1000), L);
            v->label_weight = (int32_t)int_or(pj_get(y, "LabelWeight"), 0, 0, 1000, L);
            v->label_italic = (int32_t)int_or(pj_get(y, "LabelItalic"), 0, -1, 1, L);
            v->label_color = (uint32_t)int_or(pj_get(y, "LabelColor"), 0, 0, 0xFFFFFFFFLL, L);
            L->bad |= v->format < 0;
        }

        if (!L->bad && pd_grow((void**)&d->lists, &d->caplists, (int64_t)d->nlists + 1, sizeof(dlist))) {
            L->bad = 1;
        }

        if (!L->bad) {
            d->lists[d->nlists++] = l;
        }
    }

    x = pj_get(r, "Resources");

    for (c = x ? x->child : NULL; c && !L->bad; c = c->next) {
        const pj_node* bs = pj_get(c, "_ByteStream_");
        unsigned char* data;
        size_t len;
        pd_res_id rid;
        char mime[64] = "application/octet-stream";

        copy_name(pj_get(pj_get(bs, "_DataInfo_"), "MediaType"), mime, sizeof(mime), L);

        if (L->bad || pj_stream_bytes(bs, &data, &len)) {
            L->bad = 1;
            break;
        }

        if (pd_doc_add_resource(d, mime, data, len, &rid) != PD_OK) {
            L->bad = 1;
        }

        free(data);
    }

    if ((x = pj_get(r, "Metadata")) != NULL) {
        if (x->type != PJ_STR || pd_doc_set_metadata(d, x->s, x->len) != PD_OK) {
            L->bad = 1;
        }
    }

    /* blocks: the main tree, then the hidden story container */
    if (!L->bad && load_tree(L, pj_get(r, "Document"), 0, 0, 0) != PD_ROOT_ID) {
        L->bad = 1;
    }

    if (!L->bad) {
        sr = (blk*)calloc(1, sizeof(blk));

        if (!sr) {
            L->bad = 1;
        } else {
            sr->kind = PD_BLOCK_ROOT;
            sr->id = PD_STORYROOT_ID;
            d->tab[PD_STORYROOT_ID] = sr;
            x = pj_get(r, "Stories");

            for (c = x ? x->child : NULL; c && !L->bad; c = c->next) {
                pd_block_id k2 = load_tree(L, c, PD_STORYROOT_ID, -1, 0);

                if (!k2 || pd_doc_add_kid(sr, k2, -1)) {
                    L->bad = 1;
                }
            }
        }
    }

    if (!L->bad && d->tab[PD_ROOT_ID]->nkids == 0) {
        L->bad = 1;
    }

    for (i = 0; !L->bad && i < L->nrefs; i++) {
        blk* s = L->story_refs[i] < d->captab ? d->tab[L->story_refs[i]] : NULL;

        L->bad |= !s || s->kind != PD_BLOCK_STORY;
    }

    if (L->bad) {
        pd_doc_free(d);
        return NULL;
    }

    for (k = 1; k < d->captab; k++) {
        if (d->tab[k]) {
            d->tab[k]->alive = 1;
        }
    }

    x = pj_get(r, "Comments");

    for (c = x ? x->child : NULL; c; c = c->next) {
        pd_comment cm;
        pd_comment_id id;
        const pj_node* t = pj_get(c, "Text"), *p0 = pj_get(c, "Start"), *p1 = pj_get(c, "End");

        memset(&cm, 0, sizeof(cm));
        copy_name(pj_get(c, "Author"), cm.author, sizeof(cm.author), L);
        copy_name(pj_get(c, "Date"), cm.date, sizeof(cm.date), L);
        cm.parent = (pd_comment_id)int_or(pj_get(c, "Parent"), 0, 0, d->ncomments, L);
        cm.resolved = (int32_t)int_or(pj_get(c, "Resolved"), 0, 0, 1, L);

        if (t && t->type == PJ_STR) {
            cm.text = t->s;
            cm.text_len = (uint32_t)t->len;
        }

        if (!cm.parent) {
            L->bad |= !p0 || !p1 || p0->type != PJ_ARR || p1->type != PJ_ARR || p0->n != 2 || p1->n != 2;

            if (!L->bad) {
                cm.range.start.block = (pd_block_id)int_or(p0->child, 0, 0, PD_MAX_BLOCKS, L);
                cm.range.start.offset = (uint32_t)int_or(p0->child->next, 0, 0, INT32_MAX, L);
                cm.range.end.block = (pd_block_id)int_or(p1->child, 0, 0, PD_MAX_BLOCKS, L);
                cm.range.end.offset = (uint32_t)int_or(p1->child->next, 0, 0, INT32_MAX, L);
            }
        }

        if (L->bad || pd_doc_comment_add_raw(d, &cm, &id) != PD_OK) {
            L->bad = 1;
            break;
        }
    }

    if (L->bad) {
        pd_doc_free(d);
        return NULL;
    }

    return d;
}

pd_status pd_doc_load(const void* data, size_t len, pd_jdata_format format, pd_doc** out) {
    const unsigned char* p = (const unsigned char*)data;
    pj_doc* j;
    loader L;
    int binary;

    if (!data || !out) {
        return PD_ERR_ARG;
    }

    *out = NULL;

    if (format == PD_JDATA_AUTO) {
        size_t i = 0;

        if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
            i = 3;
        }

        while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' || p[i] == '\r')) {
            i++;
        }

        /* JSON: '{' then whitespace or a quote; BJData: '{' then a size marker */
        binary = !(i + 1 < len && p[i] == '{' &&
                   (p[i + 1] == '"' || p[i + 1] == ' ' || p[i + 1] == '\n' || p[i + 1] == '\r' || p[i + 1] == '\t'));
    } else if (format == PD_JDATA_TEXT || format == PD_JDATA_BINARY) {
        binary = format == PD_JDATA_BINARY;
    } else {
        return PD_ERR_ARG;
    }

    j = pj_parse(data, len, binary, NULL);

    if (!j) {
        return PD_ERR_FORMAT;
    }

    memset(&L, 0, sizeof(L));
    *out = load_doc(pj_root(j), &L);
    free(L.story_refs);
    pj_free(j);
    return *out ? PD_OK : PD_ERR_FORMAT;
}
