/*
 * Parade DOCX (Office Open XML WordprocessingML)
 *
 * Export writes a deterministic package (fixed zip timestamps, stable
 * part order): document, styles, numbering, footnotes, settings, headers
 * and footers, media. Import reads the same parts from Word, LibreOffice,
 * Google Docs or pandoc output: paragraphs with styles and numbering,
 * runs with direct formatting, hyperlinks (also as fields), footnotes,
 * inline pictures, tables (grid spans, shading, header rows) and section
 * properties. The zip reader is bounds-checked for untrusted input.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

#define TW(sp) ((int)((int64_t)(sp) * 20 / 65536))         /* sp -> twips */
#define EMU(sp) ((long long)((int64_t)(sp) * 12700 / 65536)) /* sp -> EMU */
#define ZIP_MAX_ENTRY ((size_t)256 << 20)

/* ------------------------------------------------------------------ */
/* zip                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char name[128];
    uint32_t crc, csize, usize, offset;
    uint16_t method;
} zentry;

typedef struct {
    pd_buf* o;
    zentry e[512];
    int n;
} zipw;

static void le16(pd_buf* o, unsigned v) {
    char b[2] = { (char)(v & 255), (char)(v >> 8) };
    pb_put(o, b, 2);
}

static void le32(pd_buf* o, uint32_t v) {
    char b[4] = { (char)(v & 255), (char)((v >> 8) & 255), (char)((v >> 16) & 255), (char)(v >> 24) };
    pb_put(o, b, 4);
}

static int zip_add(zipw* z, const char* name, const void* data, size_t len) {
    zentry* e;
    unsigned char* comp = NULL;
    size_t clen = 0;
    int deflated = 0;

    if (z->n >= 512 || strlen(name) >= sizeof(z->e[0].name)) {
        return -1;
    }

    e = &z->e[z->n++];
    memset(e, 0, sizeof(*e));
    strcpy(e->name, name);
    e->crc = pd_crc32(data, len);
    e->usize = (uint32_t)len;
    e->offset = (uint32_t)z->o->n;

    if (len > 64 && pd_deflate(data, len, 0, &comp, &clen) == 0 && clen < len) {
        deflated = 1;
    }

    e->method = deflated ? 8 : 0;
    e->csize = (uint32_t)(deflated ? clen : len);
    le32(z->o, 0x04034b50);
    le16(z->o, 20);
    le16(z->o, 0x0800);                 /* UTF-8 names */
    le16(z->o, e->method);
    le16(z->o, 0);                      /* 00:00:00 */
    le16(z->o, 0x21);                   /* 1980-01-01: the same bytes every time */
    le32(z->o, e->crc);
    le32(z->o, e->csize);
    le32(z->o, e->usize);
    le16(z->o, (unsigned)strlen(name));
    le16(z->o, 0);
    pb_puts(z->o, name);
    pb_put(z->o, deflated ? (const void*)comp : data, e->csize);
    free(comp);
    return 0;
}

static void zip_finish(zipw* z) {
    uint32_t cd = (uint32_t)z->o->n, size;
    int i;

    for (i = 0; i < z->n; i++) {
        const zentry* e = &z->e[i];

        le32(z->o, 0x02014b50);
        le16(z->o, 20);
        le16(z->o, 20);
        le16(z->o, 0x0800);
        le16(z->o, e->method);
        le16(z->o, 0);
        le16(z->o, 0x21);
        le32(z->o, e->crc);
        le32(z->o, e->csize);
        le32(z->o, e->usize);
        le16(z->o, (unsigned)strlen(e->name));
        le16(z->o, 0);
        le16(z->o, 0);
        le16(z->o, 0);
        le16(z->o, 0);
        le32(z->o, 0);
        le32(z->o, e->offset);
        pb_puts(z->o, e->name);
    }

    size = (uint32_t)z->o->n - cd;
    le32(z->o, 0x06054b50);
    le16(z->o, 0);
    le16(z->o, 0);
    le16(z->o, (unsigned)z->n);
    le16(z->o, (unsigned)z->n);
    le32(z->o, size);
    le32(z->o, cd);
    le16(z->o, 0);
}

static uint32_t rd16(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

typedef struct {
    const unsigned char* s;
    size_t n;
    zentry* e;
    int ne;
} zipr;

static int zip_open(zipr* z, const unsigned char* s, size_t n) {
    size_t i, eocd = 0, cd, cdn, pos;
    int k, found = 0;

    memset(z, 0, sizeof(*z));
    z->s = s;
    z->n = n;

    if (n < 22) {
        return -1;
    }

    for (i = n - 22; ; i--) {   /* the end record, behind at most a 64K comment */
        if (rd32(s + i) == 0x06054b50) {
            eocd = i;
            found = 1;
            break;
        }

        if (i == 0 || n - i > 65557) {
            break;
        }
    }

    if (!found) {
        return -1;
    }

    cdn = rd16(s + eocd + 10);
    cd = rd32(s + eocd + 16);

    if (cdn == 0 || cdn > 10000 || cd >= n) {
        return -1;
    }

    if ((z->e = (zentry*)calloc(cdn, sizeof(zentry))) == NULL) {
        return -1;
    }

    for (k = 0, pos = cd; k < (int)cdn; k++) {
        size_t nl, xl, cl;
        zentry* e = &z->e[z->ne];

        if (pos + 46 > n || rd32(s + pos) != 0x02014b50) {
            break;
        }

        nl = rd16(s + pos + 28);
        xl = rd16(s + pos + 30);
        cl = rd16(s + pos + 32);

        if (pos + 46 + nl > n) {
            break;
        }

        e->method = (uint16_t)rd16(s + pos + 10);
        e->crc = rd32(s + pos + 16);
        e->csize = rd32(s + pos + 20);
        e->usize = rd32(s + pos + 24);
        e->offset = rd32(s + pos + 42);

        if (nl < sizeof(e->name)) {
            memcpy(e->name, s + pos + 46, nl);
            e->name[nl] = '\0';
            z->ne++;
        }

        pos += 46 + nl + xl + cl;
    }

    if (z->ne == 0) {
        free(z->e);
        z->e = NULL;
        return -1;
    }

    return 0;
}

/* an entry's bytes (malloc'ed, NUL-terminated), or NULL */
static unsigned char* zip_read(const zipr* z, const char* name, size_t* len) {
    int i;

    for (i = 0; i < z->ne; i++) {
        const zentry* e = &z->e[i];
        size_t lh = e->offset, data;
        unsigned char* out = NULL;
        size_t olen = 0;

        if (strcmp(e->name, name) != 0) {
            continue;
        }

        if (lh + 30 > z->n || rd32(z->s + lh) != 0x04034b50) {
            return NULL;
        }

        data = lh + 30 + rd16(z->s + lh + 26) + rd16(z->s + lh + 28);

        if (data > z->n || e->csize > z->n - data || e->usize > ZIP_MAX_ENTRY) {
            return NULL;
        }

        if (e->method == 0) {
            if ((out = (unsigned char*)malloc((size_t)e->csize + 1)) == NULL) {
                return NULL;
            }

            memcpy(out, z->s + data, e->csize);
            olen = e->csize;
        } else if (e->method == 8) {
            unsigned char* raw = NULL;

            if (pd_inflate(z->s + data, e->csize, 0, (size_t)e->usize + 1, &raw, &olen) != 0) {
                free(raw);
                return NULL;
            }

            if ((out = (unsigned char*)realloc(raw, olen + 1)) == NULL) {
                free(raw);
                return NULL;
            }
        } else {
            return NULL;
        }

        out[olen] = '\0';
        *len = olen;
        return out;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* export                                                             */
/* ------------------------------------------------------------------ */

static const char* W_NS =
    "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
    "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
    "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
    "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
    "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"";

typedef struct {
    pd_res_id res;
    char name[32];
    char mime[64];
} dmedia;

typedef struct {
    const pd_doc* d;
    pd_buf* o;                  /* the part being written */
    pd_numbers nb;
    pd_block_id para;
    pd_char_props base;
    int in_link, in_note;
    pd_buf rels;                /* document.xml.rels entries beyond the fixed ones */
    pd_buf notes;               /* footnotes.xml body */
    int nlinks, nnotes;
    dmedia media[256];
    int nmedia, docpr;
    pd_list_id listmap[64];     /* Parade list -> numId order */
    int nlistmap;
    int listkind[64];
    pd_block_id hf[8];          /* header/footer stories written as parts */
    int nhf;
    pd_buf* hf_rels[8];
} dxo;

static void xesc(pd_buf* o, const char* s, size_t n) {
    size_t i, j = 0;

    for (i = 0; i < n; i++) {
        const char* r = s[i] == '&' ? "&amp;" : s[i] == '<' ? "&lt;" : s[i] == '>' ? "&gt;" : s[i] == '"' ? "&quot;" :
                        NULL;

        if ((unsigned char)s[i] < 32 && s[i] != '\t' && s[i] != '\n') {
            pb_put(o, s + j, i - j);
            j = i + 1;
            continue;
        }

        if (r) {
            pb_put(o, s + j, i - j);
            pb_puts(o, r);
            j = i + 1;
        }
    }

    pb_put(o, s + j, n - j);
}

static void dx_rpr(dxo* x, const pd_char_props* c, const pd_char_props* b, const char* rstyle) {
    pd_buf* o = x->o;
    size_t mark;
    int mono = pd_conv_is_mono(c) && !pd_conv_is_mono(b);

    pb_puts(o, "<w:rPr>");
    mark = o->n;

    if (rstyle) {
        pb_printf(o, "<w:rStyle w:val=\"%s\"/>", rstyle);
    }

    if (mono) {
        pb_puts(o, "<w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/>");
    } else if (strcmp(c->family, b->family) && c->family[0]) {
        pb_puts(o, "<w:rFonts w:ascii=\"");
        xesc(o, c->family, strlen(c->family));
        pb_puts(o, "\" w:hAnsi=\"");
        xesc(o, c->family, strlen(c->family));
        pb_puts(o, "\"/>");
    }

    if (c->weight >= 600 && b->weight < 600) {
        pb_puts(o, "<w:b/>");
    } else if (c->weight < 600 && b->weight >= 600) {
        pb_puts(o, "<w:b w:val=\"0\"/>");
    }

    if (c->italic && !b->italic) {
        pb_puts(o, "<w:i/>");
    }

    if (c->strike && !b->strike) {
        pb_puts(o, "<w:strike/>");
    }

    if (c->color != b->color) {
        pb_printf(o, "<w:color w:val=\"%06X\"/>", (unsigned)(c->color & 0xFFFFFF));
    }

    if (c->size != b->size) {
        pb_printf(o, "<w:sz w:val=\"%d\"/>", (int)((int64_t)c->size * 2 / 65536));
    }

    if (c->underline && !b->underline) {
        pb_puts(o, c->underline == 2 ? "<w:u w:val=\"double\"/>" : "<w:u w:val=\"single\"/>");
    }

    if (c->background && c->background != b->background) {
        pb_printf(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>", (unsigned)(c->background & 0xFFFFFF));
    }

    if (c->shift == PD_SHIFT_SUPER) {
        pb_puts(o, "<w:vertAlign w:val=\"superscript\"/>");
    } else if (c->shift == PD_SHIFT_SUB) {
        pb_puts(o, "<w:vertAlign w:val=\"subscript\"/>");
    }

    if (o->n == mark) {     /* nothing: no rPr at all */
        o->n -= 7;
    } else {
        pb_puts(o, "</w:rPr>");
    }
}

/* run text: tabs and line breaks are their own elements */
static void dx_text(pd_buf* o, const char* s, size_t n) {
    size_t i, j = 0;

    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '\t' || s[i] == '\n') {
            if (i > j) {
                pb_puts(o, "<w:t xml:space=\"preserve\">");
                xesc(o, s + j, i - j);
                pb_puts(o, "</w:t>");
            }

            if (i < n) {
                pb_puts(o, s[i] == '\t' ? "<w:tab/>" : "<w:br/>");
            }

            j = i + 1;
        }
    }
}

static int dx_media(dxo* x, pd_res_id res, const char** rid_name) {
    int i;
    const char* mime;
    const void* data;
    size_t len;

    for (i = 0; i < x->nmedia; i++) {
        if (x->media[i].res == res) {
            *rid_name = x->media[i].name;
            return i;
        }
    }

    if (x->nmedia >= 256 || pd_doc_resource(x->d, res, &mime, &data, &len) != PD_OK) {
        return -1;
    }

    x->media[x->nmedia].res = res;
    snprintf(x->media[x->nmedia].mime, sizeof(x->media[0].mime), "%s", mime);
    snprintf(x->media[x->nmedia].name, sizeof(x->media[0].name), "image%d.%s", x->nmedia + 1,
             strstr(mime, "png") ? "png" : strstr(mime, "gif") ? "gif" : "jpeg");
    pb_printf(&x->rels, "<Relationship Id=\"rIdm%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
              "relationships/image\" Target=\"media/%s\"/>", x->nmedia + 1, x->media[x->nmedia].name);
    *rid_name = x->media[x->nmedia].name;
    return x->nmedia++;
}

static void dx_block(dxo* x, pd_block_id id);
static void dx_para(dxo* x, pd_block_id p, const char* extra_ppr);

static int dx_span(void* user, const pd_span* sp) {
    dxo* x = (dxo*)user;
    pd_buf* o = x->o;

    if (sp->is_object) {
        const pd_inline* ob = &sp->obj;

        switch (ob->kind) {
            case PD_INLINE_IMAGE: {
                const char* name;
                int m = dx_media(x, ob->resource, &name);

                if (m >= 0) {
                    long long cx = EMU(ob->width), cy = EMU(ob->height);

                    x->docpr++;
                    pb_printf(o, "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                              "<wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:docPr id=\"%d\" name=\"Picture %d\"/>"
                              "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                              "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
                              "<pic:blipFill><a:blip r:embed=\"rIdm%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                              "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
                              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
                              "</a:graphic></wp:inline></w:drawing></w:r>", cx, cy, x->docpr, x->docpr, x->docpr, name, m + 1,
                              cx, cy);
                }

                break;
            }

            case PD_INLINE_EQUATION:
                pb_puts(o, "<w:r><w:rPr><w:i/></w:rPr>");

                if (ob->source) {
                    dx_text(o, ob->source, (size_t)ob->source_len);
                }

                pb_puts(o, "</w:r>");
                break;

            case PD_INLINE_FIELD: {
                char v[32] = "";
                const char* instr = NULL;

                switch (ob->field) {
                    case PD_FIELD_SEQ:
                        snprintf(v, sizeof(v), "%d", (int)pd_numbers_at(&x->nb, x->para, sp->offset));
                        instr = "SEQ";
                        break;

                    case PD_FIELD_REF_NUMBER:
                        snprintf(v, sizeof(v), "%d", (int)pd_numbers_ref(&x->nb, ob->target));
                        break;

                    case PD_FIELD_PAGE:
                        strcpy(v, "1");
                        instr = "PAGE";
                        break;

                    case PD_FIELD_PAGES:
                        strcpy(v, "1");
                        instr = "NUMPAGES";
                        break;

                    case PD_FIELD_SECTION_PAGE:
                        strcpy(v, "1");
                        instr = "SECTIONPAGES";
                        break;

                    case PD_FIELD_HEADING:
                        strcpy(v, " ");
                        instr = "STYLEREF";
                        break;
                }

                if (instr && ob->field == PD_FIELD_SEQ) {
                    pb_printf(o, "<w:fldSimple w:instr=\" SEQ %s \\* ARABIC \">", ob->name[0] ? ob->name : "Figure");
                } else if (instr && ob->field == PD_FIELD_HEADING) {
                    pb_printf(o, "<w:fldSimple w:instr=\" STYLEREF &quot;Heading %d&quot; \">", ob->level > 0 ? (int)ob->level :
                              1);
                } else if (instr) {
                    pb_printf(o, "<w:fldSimple w:instr=\" %s \">", instr);
                }

                if (v[0]) {
                    pb_puts(o, "<w:r>");
                    dx_text(o, v, strlen(v));
                    pb_puts(o, "</w:r>");
                }

                if (instr) {
                    pb_puts(o, "</w:fldSimple>");
                }

                break;
            }

            case PD_INLINE_FOOTNOTE: {
                pd_buf* saved = x->o;
                pd_block_info si;
                int32_t k, id = ++x->nnotes;
                pd_block_id spara = x->para;
                pd_char_props sbase = x->base;
                int slink = x->in_link;

                pb_printf(o, "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteReference w:id=\"%d\"/>"
                          "</w:r>", (int)id);
                /* the body goes to footnotes.xml */
                x->o = &x->notes;
                x->in_note = 1;
                x->in_link = 0;
                pb_printf(x->o, "<w:footnote w:id=\"%d\">", (int)id);

                if (pd_doc_block_info(x->d, ob->target, &si) == PD_OK) {
                    for (k = 0; k < si.child_count; k++) {
                        dx_block(x, pd_doc_child(x->d, ob->target, k));
                    }
                }

                pb_puts(x->o, "</w:footnote>");
                x->in_note = 0;
                x->o = saved;
                x->para = spara;
                x->base = sbase;
                x->in_link = slink;
                break;
            }

            case PD_INLINE_LINK:
                if (x->in_link) {
                    pb_puts(o, "</w:hyperlink>");
                    x->in_link = 0;
                }

                if (ob->source && ob->source_len > 0 && !x->in_note) {
                    x->nlinks++;
                    pb_printf(&x->rels, "<Relationship Id=\"rIdl%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/"
                              "2006/relationships/hyperlink\" Target=\"", x->nlinks);
                    xesc(&x->rels, ob->source, (size_t)ob->source_len);
                    pb_puts(&x->rels, "\" TargetMode=\"External\"/>");
                    pb_printf(o, "<w:hyperlink r:id=\"rIdl%d\">", x->nlinks);
                    x->in_link = 1;
                }

                break;

            case PD_INLINE_TAB:
                pb_puts(o, "<w:r><w:tab/></w:r>");
                break;
        }

        return o->err;
    }

    pb_puts(o, "<w:r>");
    dx_rpr(x, &sp->cp, &x->base, x->in_link ? "Hyperlink" : NULL);
    dx_text(o, sp->text, sp->len);
    pb_puts(o, "</w:r>");
    return o->err;
}

static const char* dx_style_id(const pd_block_info* bi) {
    static const char* h[] = { "Heading1", "Heading1", "Heading2", "Heading3", "Heading4", "Heading5", "Heading6" };

    switch (bi->role) {
        case PD_ROLE_TITLE:
            return "Title";

        case PD_ROLE_HEADING:
            return h[bi->level >= 1 && bi->level <= 6 ? bi->level : 1];

        case PD_ROLE_QUOTE:
            return "Quote";

        case PD_ROLE_CODE:
            return "SourceCode";

        case PD_ROLE_CAPTION:
            return "Caption";
    }

    return NULL;
}

static int dx_num_id(dxo* x, pd_block_id p, int32_t* level) {
    pd_block_info bi;
    int i, kind;

    if ((kind = pd_conv_list_kind(x->d, p, level)) == 0 || pd_doc_block_info(x->d, p, &bi) != PD_OK) {
        return 0;
    }

    for (i = 0; i < x->nlistmap; i++) {
        if (x->listmap[i] == bi.list) {
            return i + 1;
        }
    }

    if (x->nlistmap >= 64) {
        return 0;
    }

    x->listmap[x->nlistmap] = bi.list;
    x->listkind[x->nlistmap] = kind;
    return ++x->nlistmap;
}

static void dx_para(dxo* x, pd_block_id p, const char* extra_ppr) {
    pd_block_info bi;
    pd_para_props pp, dp;
    const char* sid;
    int32_t level = 0;
    int num;
    pd_buf* o = x->o;

    pd_doc_block_info(x->d, p, &bi);
    pd_doc_style_resolve(x->d, bi.style, &pp, NULL);

    if (pd_doc_para_props(x->d, p, &dp) == PD_OK && (dp.mask & PD_PP_ALIGN)) {
        pp.align = dp.align;
    }

    sid = x->in_note && bi.role == PD_ROLE_BODY ? "FootnoteText" : dx_style_id(&bi);
    num = x->in_note ? 0 : dx_num_id(x, p, &level);
    pb_puts(o, "<w:p><w:pPr>");

    if (sid) {
        pb_printf(o, "<w:pStyle w:val=\"%s\"/>", sid);
    } else if (num) {
        pb_puts(o, "<w:pStyle w:val=\"ListParagraph\"/>");
    }

    if (num) {
        pb_printf(o, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>", (int)level, num);
    }

    if (pp.align == PD_ALIGN_CENTER || pp.align == PD_ALIGN_RIGHT || (pp.align == PD_ALIGN_JUSTIFY && !sid && !num)) {
        pb_printf(o, "<w:jc w:val=\"%s\"/>", pp.align == PD_ALIGN_CENTER ? "center" : pp.align == PD_ALIGN_RIGHT ? "right" :
                  "both");
    }

    if (extra_ppr) {
        pb_puts(o, extra_ppr);
    }

    pb_puts(o, "</w:pPr>");

    if (x->in_note && bi.index == 0) {  /* the note's number opens its first paragraph */
        pb_puts(o, "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteRef/></w:r>"
                "<w:r><w:t xml:space=\"preserve\"> </w:t></w:r>");
    }

    x->para = p;
    x->in_link = 0;
    pd_conv_base_props(x->d, p, &x->base);
    pd_conv_spans(x->d, p, dx_span, x);

    if (x->in_link) {
        pb_puts(x->o, "</w:hyperlink>");
        x->in_link = 0;
    }

    pb_puts(x->o, "</w:p>");
}

static void dx_table(dxo* x, pd_block_id t, pd_sp width) {
    pd_block_info ti, ri;
    pd_table_props tp;
    int32_t r, c, k, ncols = 0;
    pd_buf* o = x->o;
    int colw;

    pd_doc_block_info(x->d, t, &ti);
    pd_doc_table_props(x->d, t, &tp);

    for (r = 0; r < ti.child_count; r++) {
        int32_t w = 0;

        pd_doc_block_info(x->d, pd_doc_child(x->d, t, r), &ri);

        for (c = 0; c < ri.child_count; c++) {
            pd_cell_props cp;

            pd_doc_cell_props(x->d, pd_doc_child(x->d, pd_doc_child(x->d, t, r), c), &cp);
            w += cp.col_span;
        }

        ncols = w > ncols ? w : ncols;
    }

    if (ncols == 0) {
        return;
    }

    colw = TW(width) / ncols;
    pb_puts(o, "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"0\" w:type=\"auto\"/>");

    if (tp.border) {
        int sz = (int)((int64_t)tp.border * 8 / 65536);     /* eighths of a point */

        sz = sz < 2 ? 2 : sz;
        pb_puts(o, "<w:tblBorders>");
        pb_printf(o, "<w:top w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"auto\"/>", sz);
        pb_printf(o, "<w:left w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"auto\"/>", sz);
        pb_printf(o, "<w:bottom w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"auto\"/>", sz);
        pb_printf(o, "<w:right w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"auto\"/>", sz);
        pb_printf(o, "<w:insideH w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"auto\"/>", sz);
        pb_printf(o, "<w:insideV w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"auto\"/>", sz);
        pb_puts(o, "</w:tblBorders>");
    }

    pb_puts(o, "<w:tblLook w:val=\"04A0\"/></w:tblPr><w:tblGrid>");

    for (c = 0; c < ncols; c++) {
        pb_printf(o, "<w:gridCol w:w=\"%d\"/>", colw);
    }

    pb_puts(o, "</w:tblGrid>");

    for (r = 0; r < ti.child_count; r++) {
        pd_block_id row = pd_doc_child(x->d, t, r);

        pd_doc_block_info(x->d, row, &ri);
        pb_puts(o, "<w:tr>");

        if (r < tp.header_rows) {
            pb_puts(o, "<w:trPr><w:tblHeader/></w:trPr>");
        }

        for (c = 0; c < ri.child_count; c++) {
            pd_block_id cell = pd_doc_child(x->d, row, c);
            pd_cell_props cp;
            pd_block_info ci;
            int last_para = 0;

            pd_doc_cell_props(x->d, cell, &cp);
            pd_doc_block_info(x->d, cell, &ci);
            pb_printf(o, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/>", colw * cp.col_span);

            if (cp.col_span > 1) {
                pb_printf(o, "<w:gridSpan w:val=\"%d\"/>", (int)cp.col_span);
            }

            if (cp.background) {
                pb_printf(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>", (unsigned)(cp.background & 0xFFFFFF));
            }

            if (cp.valign) {
                pb_puts(o, cp.valign == 1 ? "<w:vAlign w:val=\"center\"/>" : "<w:vAlign w:val=\"bottom\"/>");
            }

            pb_puts(o, "</w:tcPr>");

            for (k = 0; k < ci.child_count; k++) {
                pd_block_info ki;
                pd_block_id kid = pd_doc_child(x->d, cell, k);

                pd_doc_block_info(x->d, kid, &ki);

                if (ki.kind == PD_BLOCK_TABLE) {
                    dx_table(x, kid, colw * cp.col_span * 65536 / 20);
                    last_para = 0;
                } else {
                    dx_block(x, kid);
                    last_para = 1;
                }
            }

            if (!last_para) {   /* a cell ends with a paragraph */
                pb_puts(o, "<w:p/>");
            }

            pb_puts(o, "</w:tc>");
        }

        pb_puts(o, "</w:tr>");
    }

    pb_puts(o, "</w:tbl>");
}

static pd_sp dx_text_width(const dxo* x) {
    pd_section_props sp;

    pd_doc_section_props(x->d, pd_doc_child(x->d, pd_doc_root(x->d), 0), &sp);
    return sp.page_width - sp.margin_left - sp.margin_right;
}

static void dx_block(dxo* x, pd_block_id id) {
    pd_block_info bi;
    int32_t i;

    if (pd_doc_block_info(x->d, id, &bi) != PD_OK) {
        return;
    }

    switch (bi.kind) {
        case PD_BLOCK_PARAGRAPH:
            dx_para(x, id, NULL);
            break;

        case PD_BLOCK_TABLE:
            dx_table(x, id, dx_text_width(x));
            break;

        case PD_BLOCK_BREAK:
            pb_printf(x->o, "<w:p><w:r><w:br w:type=\"%s\"/></w:r></w:p>", bi.break_kind == PD_BREAK_COLUMN ? "column" :
                      "page");
            break;

        default:
            for (i = 0; i < bi.child_count; i++) {
                dx_block(x, pd_doc_child(x->d, id, i));
            }
    }
}

/* a header or footer part for a story; returns its index in hf (part names header<i>.xml / footer<i>.xml) */
static int dx_story_part(dxo* x, pd_block_id story) {
    int i;

    for (i = 0; i < x->nhf; i++) {
        if (x->hf[i] == story) {
            return i;
        }
    }

    if (x->nhf >= 8) {
        return -1;
    }

    x->hf[x->nhf] = story;
    return x->nhf++;
}

static void dx_sectpr(dxo* x, const pd_section_props* sp, pd_buf* o) {
    int h = sp->header ? dx_story_part(x, sp->header) : -1, f = sp->footer ? dx_story_part(x, sp->footer) : -1;

    pb_puts(o, "<w:sectPr>");

    if (h >= 0) {
        pb_printf(o, "<w:headerReference w:type=\"default\" r:id=\"rIdh%d\"/>", h + 1);
    }

    if (f >= 0) {
        pb_printf(o, "<w:footerReference w:type=\"default\" r:id=\"rIdh%d\"/>", f + 1);
    }

    if (sp->continuous) {
        pb_puts(o, "<w:type w:val=\"continuous\"/>");
    }

    pb_printf(o, "<w:pgSz w:w=\"%d\" w:h=\"%d\"/>", TW(sp->page_width), TW(sp->page_height));
    pb_printf(o, "<w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" w:header=\"%d\" w:footer=\"%d\" "
              "w:gutter=\"0\"/>", TW(sp->margin_top), TW(sp->margin_right), TW(sp->margin_bottom), TW(sp->margin_left),
              TW(sp->header_distance), TW(sp->footer_distance));

    if (sp->columns > 1) {
        pb_printf(o, "<w:cols w:num=\"%d\" w:space=\"%d\"/>", (int)sp->columns, TW(sp->column_gap));
    }

    if (sp->first_page_number > 1) {
        pb_printf(o, "<w:pgNumType w:start=\"%d\"/>", (int)sp->first_page_number);
    }

    pb_puts(o, "</w:sectPr>");
}

static const char* XML_DECL = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";

static void dx_styles(pd_buf* o) {
    int i;

    pb_puts(o, XML_DECL);
    pb_printf(o, "<w:styles %s>", W_NS);
    pb_puts(o, "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Times New Roman\" w:hAnsi=\"Times New Roman\" "
            "w:eastAsia=\"Times New Roman\" w:cs=\"Times New Roman\"/><w:sz w:val=\"22\"/><w:lang w:val=\"en-US\"/>"
            "</w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after=\"120\"/></w:pPr></w:pPrDefault>"
            "</w:docDefaults>");
    pb_puts(o, "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
            "<w:qFormat/></w:style>");
    pb_puts(o, "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/>"
            "<w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:jc w:val=\"center\"/><w:spacing w:after=\"240\"/></w:pPr>"
            "<w:rPr><w:sz w:val=\"48\"/></w:rPr></w:style>");

    for (i = 1; i <= 6; i++) {
        static const int sz[] = { 0, 36, 30, 26, 24, 22, 22 };

        pb_printf(o, "<w:style w:type=\"paragraph\" w:styleId=\"Heading%d\"><w:name w:val=\"heading %d\"/>"
                  "<w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/>"
                  "<w:spacing w:before=\"240\" w:after=\"120\"/><w:outlineLvl w:val=\"%d\"/></w:pPr>"
                  "<w:rPr><w:b/><w:sz w:val=\"%d\"/></w:rPr></w:style>", i, i, i - 1, sz[i]);
    }

    pb_puts(o, "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/><w:basedOn w:val=\"Normal\"/>"
            "<w:qFormat/><w:pPr><w:ind w:left=\"720\" w:right=\"720\"/></w:pPr><w:rPr><w:i/></w:rPr></w:style>");
    pb_puts(o, "<w:style w:type=\"paragraph\" w:styleId=\"SourceCode\"><w:name w:val=\"Source Code\"/>"
            "<w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:rPr><w:rFonts w:ascii=\"Courier New\" "
            "w:hAnsi=\"Courier New\" w:cs=\"Courier New\"/><w:sz w:val=\"20\"/></w:rPr></w:style>");
    pb_puts(o, "<w:style w:type=\"paragraph\" w:styleId=\"Caption\"><w:name w:val=\"caption\"/>"
            "<w:basedOn w:val=\"Normal\"/><w:qFormat/><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:rPr><w:i/>"
            "<w:sz w:val=\"20\"/></w:rPr></w:style>");
    pb_puts(o, "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/>"
            "<w:basedOn w:val=\"Normal\"/><w:qFormat/><w:pPr><w:spacing w:after=\"0\"/><w:ind w:left=\"720\"/></w:pPr>"
            "</w:style>");
    pb_puts(o, "<w:style w:type=\"paragraph\" w:styleId=\"FootnoteText\"><w:name w:val=\"footnote text\"/>"
            "<w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:rPr><w:sz w:val=\"18\"/></w:rPr>"
            "</w:style>");
    pb_puts(o, "<w:style w:type=\"character\" w:styleId=\"FootnoteReference\"><w:name w:val=\"footnote reference\"/>"
            "<w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr></w:style>");
    pb_puts(o, "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/>"
            "<w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>");
    pb_puts(o, "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:tblPr>"
            "<w:tblCellMar><w:left w:w=\"108\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar>"
            "</w:tblPr></w:style>");
    pb_puts(o, "</w:styles>");
}

static void dx_numbering(dxo* x, pd_buf* o) {
    int a, lv, i;
    static const char* bullets[] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };

    pb_puts(o, XML_DECL);
    pb_printf(o, "<w:numbering %s>", W_NS);

    for (a = 0; a < 2; a++) {
        pb_printf(o, "<w:abstractNum w:abstractNumId=\"%d\"><w:multiLevelType w:val=\"hybridMultilevel\"/>", a);

        for (lv = 0; lv < 9; lv++) {
            pb_printf(o, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%s\"/>", lv, a == 0 ? "bullet" :
                      lv % 3 == 0 ? "decimal" : lv % 3 == 1 ? "lowerLetter" : "lowerRoman");

            if (a == 0) {
                pb_printf(o, "<w:lvlText w:val=\"%s\"/>", bullets[lv % 3]);
            } else {
                pb_printf(o, "<w:lvlText w:val=\"%%%d.\"/>", lv + 1);
            }

            pb_printf(o, "<w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>",
                      720 + 360 * lv);
        }

        pb_puts(o, "</w:abstractNum>");
    }

    for (i = 0; i < x->nlistmap; i++) {     /* one numbering instance per list: each restarts at 1 */
        pb_printf(o, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/>", i + 1, x->listkind[i] == 1 ? 0 : 1);

        if (x->listkind[i] == 2) {
            pb_puts(o, "<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"1\"/></w:lvlOverride>");
        }

        pb_puts(o, "</w:num>");
    }

    pb_puts(o, "</w:numbering>");
}

pd_status pd_docx_export(const pd_doc* d, pd_buf* out) {
    dxo* x = (dxo*)calloc(1, sizeof(dxo));
    pd_buf doc, part;
    zipw z;
    pd_block_info ri;
    int32_t s, i;

    if (!x) {
        return PD_ERR_NOMEM;
    }

    memset(&doc, 0, sizeof(doc));
    memset(&part, 0, sizeof(part));
    memset(&z, 0, sizeof(z));
    x->d = d;
    pd_numbers_init(&x->nb, d);

    /* document.xml; sections end in the pPr of their last paragraph, the last one in the body */
    x->o = &doc;
    pb_puts(&doc, XML_DECL);
    pb_printf(&doc, "<w:document %s><w:body>", W_NS);
    pd_doc_block_info(d, pd_doc_root(d), &ri);

    for (s = 0; s < ri.child_count; s++) {
        pd_block_id sec = pd_doc_child(d, pd_doc_root(d), s);
        pd_section_props sp;
        pd_block_info si;

        pd_doc_section_props(d, sec, &sp);
        pd_doc_block_info(d, sec, &si);

        for (i = 0; i < si.child_count; i++) {
            pd_block_id k = pd_doc_child(d, sec, i);
            pd_block_info ki;

            pd_doc_block_info(d, k, &ki);

            if (s + 1 < ri.child_count && i + 1 == si.child_count) {    /* the section's sectPr rides here */
                pd_section_props next;
                pd_buf sect;

                memset(&sect, 0, sizeof(sect));
                pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), s + 1), &next);
                dx_sectpr(x, &sp, &sect);

                if (next.continuous) {  /* in Word the next section says how it starts */
                }

                if (ki.kind == PD_BLOCK_PARAGRAPH) {
                    dx_para(x, k, sect.p);
                } else {
                    dx_block(x, k);
                    pb_printf(&doc, "<w:p><w:pPr>%s</w:pPr></w:p>", sect.p ? sect.p : "");
                }

                pb_free(&sect);
            } else {
                dx_block(x, k);
            }
        }

        if (s + 1 == ri.child_count) {
            dx_sectpr(x, &sp, &doc);
        }
    }

    pb_puts(&doc, "</w:body></w:document>");

    /* the package */
    z.o = out;
    pb_puts(&part, XML_DECL);
    pb_puts(&part, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Default Extension=\"png\" ContentType=\"image/png\"/>"
            "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
            "<Default Extension=\"gif\" ContentType=\"image/gif\"/>"
            "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.document.main+xml\"/>"
            "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.styles+xml\"/>"
            "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.numbering+xml\"/>"
            "<Override PartName=\"/word/footnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.footnotes+xml\"/>"
            "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.settings+xml\"/>");

    for (i = 0; i < x->nhf; i++) {
        pb_printf(&part, "<Override PartName=\"/word/hf%d.xml\" ContentType=\"application/vnd.openxmlformats-"
                  "officedocument.wordprocessingml.header+xml\"/>", (int)i + 1);
    }

    pb_puts(&part, "</Types>");
    zip_add(&z, "[Content_Types].xml", part.p, part.n);
    part.n = 0;
    pb_puts(&part, XML_DECL);
    pb_puts(&part, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "officeDocument\" Target=\"word/document.xml\"/></Relationships>");
    zip_add(&z, "_rels/.rels", part.p, part.n);
    zip_add(&z, "word/document.xml", doc.p, doc.n);

    part.n = 0;
    dx_styles(&part);
    zip_add(&z, "word/styles.xml", part.p, part.n);
    part.n = 0;
    dx_numbering(x, &part);
    zip_add(&z, "word/numbering.xml", part.p, part.n);

    part.n = 0;
    pb_puts(&part, XML_DECL);
    pb_printf(&part, "<w:footnotes %s><w:footnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p>"
              "</w:footnote><w:footnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/>"
              "</w:r></w:p></w:footnote>", W_NS);
    pb_put(&part, x->notes.p, x->notes.n);
    pb_puts(&part, "</w:footnotes>");
    zip_add(&z, "word/footnotes.xml", part.p, part.n);

    part.n = 0;
    pb_puts(&part, XML_DECL);
    pb_printf(&part, "<w:settings %s><w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>"
              "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" "
              "w:val=\"15\"/></w:compat></w:settings>", W_NS);
    zip_add(&z, "word/settings.xml", part.p, part.n);

    /* headers and footers: their own parts (with their own relationships for links) */
    for (i = 0; i < x->nhf; i++) {
        pd_block_info hi;
        int32_t k;
        char name[32];
        pd_buf hrels = x->rels;
        int keep_links = x->nlinks;

        part.n = 0;
        pb_puts(&part, XML_DECL);
        pb_printf(&part, "<w:hdr %s>", W_NS);
        x->o = &part;
        memset(&x->rels, 0, sizeof(x->rels));
        pd_doc_block_info(d, x->hf[i], &hi);

        for (k = 0; k < hi.child_count; k++) {
            dx_block(x, pd_doc_child(d, x->hf[i], k));
        }

        pb_puts(&part, "</w:hdr>");
        snprintf(name, sizeof(name), "word/hf%d.xml", (int)i + 1);
        zip_add(&z, name, part.p, part.n);

        if (x->rels.n) {
            pd_buf r2;

            memset(&r2, 0, sizeof(r2));
            pb_puts(&r2, XML_DECL);
            pb_puts(&r2, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
            pb_put(&r2, x->rels.p, x->rels.n);
            pb_puts(&r2, "</Relationships>");
            snprintf(name, sizeof(name), "word/_rels/hf%d.xml.rels", (int)i + 1);
            zip_add(&z, name, r2.p, r2.n);
            pb_free(&r2);
        }

        pb_free(&x->rels);
        x->rels = hrels;
        (void)keep_links;
    }

    part.n = 0;
    pb_puts(&part, XML_DECL);
    pb_puts(&part, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" "
            "Target=\"styles.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "numbering\" Target=\"numbering.xml\"/>"
            "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "footnotes\" Target=\"footnotes.xml\"/>"
            "<Relationship Id=\"rId4\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "settings\" Target=\"settings.xml\"/>");

    for (i = 0; i < x->nhf; i++) {
        int is_footer = 0, s2;

        for (s2 = 0; s2 < ri.child_count; s2++) {
            pd_section_props sp;

            pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), s2), &sp);
            is_footer |= sp.footer == x->hf[i];
        }

        pb_printf(&part, "<Relationship Id=\"rIdh%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                  "relationships/%s\" Target=\"hf%d.xml\"/>", (int)i + 1, is_footer ? "footer" : "header", (int)i + 1);
    }

    pb_put(&part, x->rels.p, x->rels.n);
    pb_puts(&part, "</Relationships>");
    zip_add(&z, "word/_rels/document.xml.rels", part.p, part.n);

    for (i = 0; i < x->nmedia; i++) {
        const char* mime;
        const void* data;
        size_t len;
        char name[64];

        if (pd_doc_resource(d, x->media[i].res, &mime, &data, &len) == PD_OK) {
            snprintf(name, sizeof(name), "word/media/%s", x->media[i].name);
            zip_add(&z, name, data, len);
        }
    }

    zip_finish(&z);
    i = doc.err || part.err || x->rels.err || x->notes.err;
    pb_free(&part);
    pb_free(&doc);
    pb_free(&x->rels);
    pb_free(&x->notes);
    pd_numbers_free(&x->nb);
    free(x);
    return out->err || i ? PD_ERR_NOMEM : PD_OK;
}

/* ------------------------------------------------------------------ */
/* import                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char id[64];
    char target[256];
    int external;
} drel;

typedef struct {
    char id[64];
    char name[64];
    char based_on[64];
    int outline;                /* -1 none */
    int num_id, ilvl;           /* numbering set by the style itself */
} dstyle_x;

typedef struct {
    int num_id, abstract_id;
} dnum;

typedef struct {
    int id;
    int kind[9];
} dabs;

typedef struct {
    int id;
    size_t a, b;
} dnote;

typedef struct {
    pd_bld* b;
    zipr z;
    drel* rels;
    int nrels;
    dstyle_x* styles;
    int nstyles;
    dnum nums[256];
    int nnums;
    dabs abss[64];
    int nabs;
    char* notes_xml;
    size_t notes_len;
    dnote* notes;
    int nnotes;
    int depth;                  /* footnote recursion */
} dxi;

static const char* rel_target(const dxi* X, const char* id, int* external) {
    int i;

    for (i = 0; i < X->nrels; i++) {
        if (strcmp(X->rels[i].id, id) == 0) {
            if (external) {
                *external = X->rels[i].external;
            }

            return X->rels[i].target;
        }
    }

    return NULL;
}

static int attr_int(const pd_markup* m, const char* name, int dflt) {
    char v[32];
    return mu_attr(m, name, v, sizeof(v)) ? atoi(v) : dflt;
}

/* w:val="0"/"false"/"off" turns a toggle off; absent means on */
static int attr_on(const pd_markup* m) {
    char v[16];
    return !mu_attr(m, "w:val", v, sizeof(v)) || !(strcmp(v, "0") == 0 || strcmp(v, "false") == 0 ||
            strcmp(v, "off") == 0 || strcmp(v, "none") == 0);
}

static void read_rels(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    int cap = 0;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "Relationship") == 0) {
            drel r;
            char mode[32];

            memset(&r, 0, sizeof(r));
            mu_attr(&m, "Id", r.id, sizeof(r.id));
            mu_attr(&m, "Target", r.target, sizeof(r.target));
            r.external = mu_attr(&m, "TargetMode", mode, sizeof(mode)) && strcmp(mode, "External") == 0;

            if (!pd_grow((void**)&X->rels, &cap, (int64_t)X->nrels + 1, sizeof(drel))) {
                X->rels[X->nrels++] = r;
            }
        }
    }
}

static void read_styles(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    int cap = 0, cur = -1;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);

        if (m.type == MT_OPEN && strcmp(t, "style") == 0) {
            dstyle_x s;

            memset(&s, 0, sizeof(s));
            s.outline = -1;
            s.num_id = -1;
            mu_attr(&m, "w:styleId", s.id, sizeof(s.id));

            if (!pd_grow((void**)&X->styles, &cap, (int64_t)X->nstyles + 1, sizeof(dstyle_x))) {
                cur = X->nstyles;
                X->styles[X->nstyles++] = s;
            }
        } else if (m.type == MT_CLOSE && strcmp(t, "style") == 0) {
            cur = -1;
        } else if (cur >= 0 && (m.type == MT_OPEN || m.type == MT_EMPTY)) {
            if (strcmp(t, "name") == 0) {
                mu_attr(&m, "w:val", X->styles[cur].name, sizeof(X->styles[0].name));
            } else if (strcmp(t, "basedOn") == 0) {
                mu_attr(&m, "w:val", X->styles[cur].based_on, sizeof(X->styles[0].based_on));
            } else if (strcmp(t, "outlineLvl") == 0) {
                X->styles[cur].outline = attr_int(&m, "w:val", -1);
            } else if (strcmp(t, "numId") == 0) {
                X->styles[cur].num_id = attr_int(&m, "w:val", -1);
            } else if (strcmp(t, "ilvl") == 0) {
                X->styles[cur].ilvl = attr_int(&m, "w:val", 0);
            }
        }
    }
}

static void read_numbering(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    int cur_abs = -1, cur_lvl = -1, cur_num = -1;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);

        if (m.type != MT_OPEN && m.type != MT_EMPTY) {
            continue;
        }

        if (strcmp(t, "abstractNum") == 0 && X->nabs < 64) {
            cur_abs = X->nabs++;
            memset(&X->abss[cur_abs], 0, sizeof(dabs));
            X->abss[cur_abs].id = attr_int(&m, "w:abstractNumId", -1);
            cur_num = -1;
        } else if (strcmp(t, "lvl") == 0) {
            cur_lvl = attr_int(&m, "w:ilvl", 0);
        } else if (strcmp(t, "numFmt") == 0 && cur_abs >= 0 && cur_lvl >= 0 && cur_lvl < 9) {
            char v[32] = "";

            mu_attr(&m, "w:val", v, sizeof(v));
            X->abss[cur_abs].kind[cur_lvl] = strcmp(v, "bullet") == 0 || strcmp(v, "none") == 0 ? 1 : 2;
        } else if (strcmp(t, "num") == 0 && X->nnums < 256) {
            cur_num = X->nnums++;
            X->nums[cur_num].num_id = attr_int(&m, "w:numId", -1);
            X->nums[cur_num].abstract_id = -1;
            cur_abs = -1;
        } else if (strcmp(t, "abstractNumId") == 0 && cur_num >= 0) {
            X->nums[cur_num].abstract_id = attr_int(&m, "w:val", -1);
        }
    }
}

static void read_notes(dxi* X) {
    pd_markup m;
    int cap = 0, id = 0, depth = 0;
    size_t start = 0;

    mu_init(&m, X->notes_xml, X->notes_len, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);

        if (m.type == MT_OPEN && strcmp(t, "footnote") == 0 && depth == 0) {
            id = attr_int(&m, "w:id", -999);
            start = m.pos;
            depth = 1;
        } else if (m.type == MT_OPEN && depth > 0) {
            depth++;
        } else if (m.type == MT_CLOSE && depth > 0) {
            if (--depth == 0 && strcmp(t, "footnote") == 0) {
                dnote nt;

                nt.id = id;
                nt.a = start;
                nt.b = m.pos - strlen(m.name) - 3;

                if (!pd_grow((void**)&X->notes, &cap, (int64_t)X->nnotes + 1, sizeof(dnote))) {
                    X->notes[X->nnotes++] = nt;
                }
            }
        }
    }
}

static int list_kind_of(const dxi* X, int num_id, int ilvl) {
    int i, k;

    for (i = 0; i < X->nnums; i++) {
        if (X->nums[i].num_id == num_id) {
            for (k = 0; k < X->nabs; k++) {
                if (X->abss[k].id == X->nums[i].abstract_id) {
                    int kind = X->abss[k].kind[ilvl >= 0 && ilvl < 9 ? ilvl : 0];
                    return kind ? kind : 1;
                }
            }
        }
    }

    return num_id > 0 ? 1 : 0;
}

typedef struct {
    int b, i, u, strike, shift, mono;
    uint32_t color, fill;
} drun;

typedef struct {
    dxi* X;
    /* paragraph */
    int in_p, started, in_ppr, in_rpr, in_tcpr, in_trpr, in_sect;
    char pstyle[64];
    int num_id, ilvl, jc, outline, sect_here;
    pd_section_props sp;
    /* run */
    drun run;
    int in_t, in_instr, in_drawing;
    /* fields and links */
    int fld;                    /* 0 none, 1 instruction, 2 result */
    char instr[512];
    size_t ninstr;
    int fld_link, link_depth[16], nlinks;
    int simple_link;
    /* drawings */
    char blip[64];
    long long cx, cy;
    /* tables */
    int pend_row, row_header, pend_cell, span;
    uint32_t cell_bg;
    int skip;                   /* depth inside an ignored element */
    int note;                   /* parsing a footnote body */
    int after_ref;              /* just after the note's own number: drop the space that follows it */
} dw;

static void dw_apply_run(dw* w) {
    pd_char_props cp;

    memset(&cp, 0, sizeof(cp));

    if (w->run.b) {
        cp.mask |= PD_CP_WEIGHT;
        cp.weight = 700;
    }

    if (w->run.i) {
        cp.mask |= PD_CP_ITALIC;
        cp.italic = 1;
    }

    if (w->run.u) {
        cp.mask |= PD_CP_UNDERLINE;
        cp.underline = w->run.u;
    }

    if (w->run.strike) {
        cp.mask |= PD_CP_STRIKE;
        cp.strike = 1;
    }

    if (w->run.shift) {
        cp.mask |= PD_CP_SHIFT;
        cp.shift = w->run.shift;
    }

    if (w->run.mono) {
        cp.mask |= PD_CP_FAMILY;
        strcpy(cp.family, "monospace");
    }

    if (w->run.color) {
        cp.mask |= PD_CP_COLOR;
        cp.color = w->run.color;
    }

    if (w->run.fill) {
        cp.mask |= PD_CP_BACKGROUND;
        cp.background = w->run.fill;
    }

    bld_set_format(w->X->b, &cp);
}

static void dw_begin_cell(dw* w) {
    pd_bld* b = w->X->b;

    if (w->pend_row) {
        bld_row_begin(b, w->row_header);
        w->pend_row = 0;
    }

    if (w->pend_cell) {
        bld_cell_begin(b, w->span, w->cell_bg);
        w->pend_cell = 0;
    }
}

static const dstyle_x* find_style(const dxi* X, const char* id) {
    int i;

    for (i = 0; id[0] && i < X->nstyles; i++) {
        if (strcmp(X->styles[i].id, id) == 0) {
            return &X->styles[i];
        }
    }

    return NULL;
}

/* a role-carrying style name along the basedOn chain ("My Heading" based on "heading 2") */
static int known_name(const char* n) {
    char low[64];
    size_t k;

    for (k = 0; k + 1 < sizeof(low) && n[k]; k++) {
        low[k] = (char)tolower((unsigned char)n[k]);
    }

    low[k] = '\0';
    return strncmp(low, "heading", 7) == 0 || strcmp(low, "title") == 0 || strstr(low, "quote") ||
           strstr(low, "source") || strstr(low, "preformatted") || strcmp(low, "caption") == 0;
}

static void dw_begin_para(dw* w) {
    pd_bld* b = w->X->b;
    const char* name = "";
    char low[64];
    int hop, outline = w->outline, num_id = w->num_id, ilvl = w->ilvl;
    size_t k;
    const dstyle_x* st = find_style(w->X, w->pstyle);

    if (w->started) {
        return;
    }

    dw_begin_cell(w);

    for (hop = 0; st && hop < 10; hop++, st = find_style(w->X, st->based_on)) {
        if (hop == 0 || (!known_name(name) && known_name(st->name))) {
            name = st->name;    /* the style's own name, or a known one it is based on */
        }

        outline = outline >= 0 ? outline : st->outline;

        if (num_id <= 0 && st->num_id > 0) {
            num_id = st->num_id;
            ilvl = st->ilvl;
        }
    }

    if (!name[0]) {
        name = w->pstyle;
    }

    for (k = 0; k + 1 < sizeof(low) && name[k]; k++) {
        low[k] = (char)tolower((unsigned char)name[k]);
    }

    low[k] = '\0';

    if (strcmp(low, "title") == 0) {
        bld_para_style(b, "Title", PD_ROLE_TITLE, 0);
    } else if ((strncmp(low, "heading", 7) == 0 && isdigit((unsigned char)low[strlen(low) - 1])) ||
               (outline >= 0 && outline < 6)) {
        int lvl = outline >= 0 && outline < 6 ? outline + 1 : low[strlen(low) - 1] - '0';
        char sname[16];

        lvl = lvl < 1 ? 1 : lvl > 6 ? 6 : lvl;
        snprintf(sname, sizeof(sname), "Heading %d", lvl);
        bld_para_style(b, sname, PD_ROLE_HEADING, lvl);
    } else if (strstr(low, "quote") || strcmp(low, "block text") == 0) {
        bld_para_style(b, "Quote", PD_ROLE_QUOTE, 0);
    } else if (strstr(low, "source") || strstr(low, "code") || strstr(low, "preformatted") ||
               strcmp(low, "plain text") == 0) {
        bld_para_style(b, "Code", PD_ROLE_CODE, 0);
    } else if (strcmp(low, "caption") == 0) {
        bld_para_style(b, "Caption", PD_ROLE_CAPTION, 0);
    }

    if (num_id > 0) {
        bld_list(b, list_kind_of(w->X, num_id, ilvl), ilvl < 0 ? 0 : ilvl > 8 ? 8 : ilvl);
    }

    if (w->jc >= 0) {
        b->pp.mask |= PD_PP_ALIGN;
        b->pp.align = w->jc;
    }

    bld_begin_para(b);
    w->started = 1;
}

static void dw_text(dw* w, const char* s, size_t n) {
    if (w->after_ref) {
        while (n > 0 && *s == ' ') {
            s++;
            n--;
        }

        w->after_ref = n == 0;
    }

    if (w->fld == 1 || n == 0) {
        return;
    }

    dw_begin_para(w);
    dw_apply_run(w);
    bld_text(w->X->b, s, n);
}

static void dw_link(dw* w, const char* url, size_t n) {
    pd_inline o;

    dw_begin_para(w);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_LINK;
    o.source = url;
    o.source_len = (int32_t)n;
    bld_inline(w->X->b, &o);
}

static void dw_parse(dxi* X, const char* xml, size_t n, int note);

static void dw_footnote(dw* w, int id) {
    dxi* X = w->X;
    int i;

    for (i = 0; i < X->nnotes; i++) {
        if (X->notes[i].id == id && X->depth < 3) {
            pd_char_props keep = X->b->cp;

            dw_begin_para(w);
            bld_footnote_begin(X->b);
            X->depth++;
            dw_parse(X, X->notes_xml + X->notes[i].a, X->notes[i].b - X->notes[i].a, 1);
            X->depth--;
            bld_footnote_end(X->b);
            X->b->cp = keep;
            return;
        }
    }
}

static void dw_image(dw* w) {
    dxi* X = w->X;
    const char* target = rel_target(X, w->blip, NULL);
    char path[300];
    unsigned char* data;
    size_t len = 0;

    if (!target || w->cx <= 0 || w->cy <= 0) {
        return;
    }

    if (target[0] == '/') {
        snprintf(path, sizeof(path), "%s", target + 1);
    } else if (strncmp(target, "../", 3) == 0) {
        snprintf(path, sizeof(path), "%s", target + 3);
    } else {
        snprintf(path, sizeof(path), "word/%s", target);
    }

    if ((data = zip_read(&X->z, path, &len)) != NULL) {
        const char* ext = strrchr(path, '.');
        const char* mime = ext && (strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0) ? "image/png" :
                           ext && (strcmp(ext, ".gif") == 0) ? "image/gif" : ext && (strcmp(ext, ".jpg") == 0 ||
                                   strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".JPG") == 0) ? "image/jpeg" : "application/octet-stream";
        pd_inline o;

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_IMAGE;
        o.width = (pd_sp)(w->cx * 65536 / 12700);
        o.height = (pd_sp)(w->cy * 65536 / 12700);

        if (o.width > 0 && o.height > 0 && pd_doc_add_resource(X->b->d, mime, data, len, &o.resource) == PD_OK) {
            dw_begin_para(w);
            bld_inline(X->b, &o);
        }

        free(data);
    }
}

static void dw_parse(dxi* X, const char* xml, size_t n, int note) {
    pd_markup m;
    dw* w = (dw*)calloc(1, sizeof(dw));
    char v[300];

    if (!w) {
        return;
    }

    w->X = X;
    w->note = note;
    w->jc = -1;
    w->outline = -1;
    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);
        int open = m.type == MT_OPEN || m.type == MT_EMPTY;

        if (w->skip) {
            if (m.type == MT_OPEN) {
                w->skip++;
            } else if (m.type == MT_CLOSE) {
                w->skip--;
            }

            continue;
        }

        if (m.type == MT_TEXT) {
            if (w->in_t && w->fld != 1) {
                pd_buf txt;

                memset(&txt, 0, sizeof(txt));
                mu_decode(m.text, m.tlen, &txt);
                dw_text(w, txt.p ? txt.p : "", txt.n);
                pb_free(&txt);
            } else if (w->in_instr && w->ninstr + m.tlen < sizeof(w->instr)) {
                memcpy(w->instr + w->ninstr, m.text, m.tlen);
                w->ninstr += m.tlen;
                w->instr[w->ninstr] = '\0';
            }

            continue;
        }

        if (open) {
            /* elements whose content is ignored */
            if (strcmp(t, "del") == 0 || strcmp(t, "txbxContent") == 0 || strcmp(t, "Fallback") == 0 ||
                    strcmp(t, "pict") == 0 || strcmp(t, "object") == 0 || strcmp(t, "moveFrom") == 0 ||
                    (w->in_ppr && strcmp(t, "rPr") == 0) || strcmp(t, "rPrChange") == 0 ||
                    strcmp(t, "pPrChange") == 0) {
                if (m.type == MT_OPEN) {
                    w->skip = 1;
                }

                continue;
            }

            if (strcmp(t, "p") == 0) {
                dw_begin_cell(w);
                w->in_p = 1;
                w->started = 0;
                w->pstyle[0] = '\0';
                w->num_id = 0;
                w->ilvl = 0;
                w->jc = -1;
                w->outline = -1;
                w->sect_here = 0;

                if (m.type == MT_EMPTY) {   /* <w:p/>: an empty paragraph */
                    dw_begin_para(w);
                    bld_end_para(X->b);
                    w->in_p = 0;
                }
            } else if (strcmp(t, "pPr") == 0) {
                w->in_ppr = m.type == MT_OPEN;
            } else if (strcmp(t, "sectPr") == 0) {
                w->in_sect = m.type == MT_OPEN;
                pd_section_props_init(&w->sp);

                if (w->in_ppr) {
                    w->sect_here = 1;
                }
            } else if (w->in_sect) {
                if (strcmp(t, "pgSz") == 0) {
                    w->sp.page_width = attr_int(&m, "w:w", 11906) * 65536 / 20;
                    w->sp.page_height = attr_int(&m, "w:h", 16838) * 65536 / 20;
                } else if (strcmp(t, "pgMar") == 0) {
                    w->sp.margin_top = abs(attr_int(&m, "w:top", 1440)) * 65536 / 20;
                    w->sp.margin_bottom = abs(attr_int(&m, "w:bottom", 1440)) * 65536 / 20;
                    w->sp.margin_left = attr_int(&m, "w:left", 1440) * 65536 / 20;
                    w->sp.margin_right = attr_int(&m, "w:right", 1440) * 65536 / 20;
                } else if (strcmp(t, "cols") == 0) {
                    int nc = attr_int(&m, "w:num", 1);

                    w->sp.columns = nc < 1 ? 1 : nc > 16 ? 16 : nc;
                    w->sp.column_gap = attr_int(&m, "w:space", 720) * 65536 / 20;
                } else if (strcmp(t, "type") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->sp.continuous = strcmp(v, "continuous") == 0;
                }
            } else if (w->in_ppr) {
                if (strcmp(t, "pStyle") == 0) {
                    mu_attr(&m, "w:val", w->pstyle, sizeof(w->pstyle));
                } else if (strcmp(t, "numId") == 0) {
                    w->num_id = attr_int(&m, "w:val", 0);
                } else if (strcmp(t, "ilvl") == 0) {
                    w->ilvl = attr_int(&m, "w:val", 0);
                    w->ilvl = w->ilvl < 0 ? 0 : w->ilvl > 8 ? 8 : w->ilvl;
                } else if (strcmp(t, "jc") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->jc = strcmp(v, "center") == 0 ? PD_ALIGN_CENTER : strcmp(v, "right") == 0 || strcmp(v, "end") == 0 ?
                            PD_ALIGN_RIGHT : strcmp(v, "both") == 0 || strcmp(v, "distribute") == 0 ? PD_ALIGN_JUSTIFY :
                            PD_ALIGN_LEFT;
                } else if (strcmp(t, "outlineLvl") == 0) {
                    w->outline = attr_int(&m, "w:val", -1);
                }
            } else if (strcmp(t, "r") == 0) {
                memset(&w->run, 0, sizeof(w->run));
            } else if (strcmp(t, "rPr") == 0) {
                w->in_rpr = m.type == MT_OPEN;
            } else if (w->in_rpr) {
                if (strcmp(t, "b") == 0) {
                    w->run.b = attr_on(&m);
                } else if (strcmp(t, "i") == 0) {
                    w->run.i = attr_on(&m);
                } else if (strcmp(t, "u") == 0) {
                    w->run.u = attr_on(&m) ? (mu_attr(&m, "w:val", v, sizeof(v)) && strcmp(v, "double") == 0 ? 2 : 1) : 0;
                } else if (strcmp(t, "strike") == 0 || strcmp(t, "dstrike") == 0) {
                    w->run.strike = attr_on(&m);
                } else if (strcmp(t, "vertAlign") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->run.shift = strcmp(v, "superscript") == 0 ? PD_SHIFT_SUPER : strcmp(v, "subscript") == 0 ?
                                   PD_SHIFT_SUB : 0;
                } else if (strcmp(t, "color") == 0 && mu_attr(&m, "w:val", v, sizeof(v)) && strcmp(v, "auto") != 0) {
                    w->run.color = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
                } else if (strcmp(t, "shd") == 0 && mu_attr(&m, "w:fill", v, sizeof(v)) && strcmp(v, "auto") != 0) {
                    w->run.fill = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
                } else if (strcmp(t, "highlight") == 0 && mu_attr(&m, "w:val", v, sizeof(v)) && strcmp(v, "none") != 0) {
                    w->run.fill = strcmp(v, "yellow") == 0 ? 0xFFFFFF00u : strcmp(v, "green") == 0 ? 0xFF00FF00u :
                                  strcmp(v, "cyan") == 0 ? 0xFF00FFFFu : 0xFFFFFF00u;
                } else if (strcmp(t, "rFonts") == 0 && (mu_attr(&m, "w:ascii", v, sizeof(v)) ||
                                                        mu_attr(&m, "w:hAnsi", v, sizeof(v)))) {
                    w->run.mono = strstr(v, "Courier") || strstr(v, "Mono") || strstr(v, "Consolas") ||
                                  strstr(v, "Menlo");
                } else if (strcmp(t, "rStyle") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    if (strstr(v, "Code") || strstr(v, "Verbatim")) {
                        w->run.mono = 1;
                    }
                }
            } else if (strcmp(t, "t") == 0) {
                w->in_t = m.type == MT_OPEN;
            } else if (strcmp(t, "instrText") == 0) {
                w->in_instr = m.type == MT_OPEN;
            } else if (strcmp(t, "tab") == 0 && w->in_p && !w->in_ppr) {
                dw_text(w, "\t", 1);
            } else if ((strcmp(t, "br") == 0 || strcmp(t, "cr") == 0) && w->in_p) {
                if (mu_attr(&m, "w:type", v, sizeof(v)) && strcmp(v, "page") == 0) {
                    if (w->started) {
                        bld_end_para(X->b);
                        w->started = 0;
                    }

                    if (!w->note) {
                        bld_break(X->b, PD_BREAK_PAGE);
                    }
                } else {
                    dw_text(w, "\n", 1);
                }
            } else if (strcmp(t, "noBreakHyphen") == 0) {
                dw_text(w, "\xE2\x80\x91", 3);
            } else if (strcmp(t, "softHyphen") == 0) {
                dw_text(w, "\xC2\xAD", 2);
            } else if (strcmp(t, "footnoteReference") == 0 && !w->note) {
                dw_footnote(w, attr_int(&m, "w:id", -999));
            } else if (strcmp(t, "drawing") == 0) {
                w->in_drawing = m.type == MT_OPEN;
                w->blip[0] = '\0';
                w->cx = w->cy = 0;
            } else if (w->in_drawing && strcmp(t, "extent") == 0) {
                if (mu_attr(&m, "cx", v, sizeof(v))) {
                    w->cx = atoll(v);
                }

                if (mu_attr(&m, "cy", v, sizeof(v))) {
                    w->cy = atoll(v);
                }
            } else if (w->in_drawing && strcmp(t, "blip") == 0) {
                mu_attr(&m, "r:embed", w->blip, sizeof(w->blip));
            } else if (strcmp(t, "hyperlink") == 0) {
                int ext = 0;
                const char* url = mu_attr(&m, "r:id", v, sizeof(v)) ? rel_target(X, v, &ext) : NULL;

                if (url && w->nlinks < 16 && m.type == MT_OPEN) {
                    dw_link(w, url, strlen(url));
                    w->link_depth[w->nlinks++] = 1;
                } else if (m.type == MT_OPEN && w->nlinks < 16) {
                    w->link_depth[w->nlinks++] = 0;     /* internal anchor: no link */
                }
            } else if (strcmp(t, "fldSimple") == 0) {
                w->simple_link = 0;

                if (mu_attr(&m, "w:instr", v, sizeof(v))) {
                    const char* h = strstr(v, "HYPERLINK");
                    const char* q = h ? strchr(h, '"') : NULL, *e = q ? strchr(q + 1, '"') : NULL;

                    if (q && e && e > q + 1 && m.type == MT_OPEN) {
                        dw_link(w, q + 1, (size_t)(e - q - 1));
                        w->simple_link = 1;
                    }
                }
            } else if (strcmp(t, "fldChar") == 0 && mu_attr(&m, "w:fldCharType", v, sizeof(v))) {
                if (strcmp(v, "begin") == 0) {
                    w->fld = 1;
                    w->ninstr = 0;
                    w->instr[0] = '\0';
                } else if (strcmp(v, "separate") == 0) {
                    const char* h = strstr(w->instr, "HYPERLINK");
                    const char* q = h ? strchr(h, '"') : NULL, *e = q ? strchr(q + 1, '"') : NULL;

                    w->fld = 2;

                    if (q && e && e > q + 1) {
                        dw_link(w, q + 1, (size_t)(e - q - 1));
                        w->fld_link = 1;
                    }
                } else if (strcmp(v, "end") == 0) {
                    if (w->fld_link) {
                        dw_link(w, "", 0);
                        w->fld_link = 0;
                    }

                    w->fld = 0;
                }
            } else if (strcmp(t, "sym") == 0 && mu_attr(&m, "w:char", v, sizeof(v))) {
                uint32_t c = (uint32_t)strtoul(v, NULL, 16);
                char u[4];
                size_t un = 0;

                if (c >= 0xF000 && c < 0xF100) {
                    c -= 0xF000;    /* symbol fonts map into the private use area */
                }

                if (c >= 0x20 && c < 0x80) {
                    u[un++] = (char)c;
                } else if (c >= 0x80 && c < 0x800) {
                    u[un++] = (char)(0xC0 | (c >> 6));
                    u[un++] = (char)(0x80 | (c & 0x3F));
                } else if (c >= 0x800 && c < 0x10000 && !(c >= 0xD800 && c < 0xE000)) {
                    u[un++] = (char)(0xE0 | (c >> 12));
                    u[un++] = (char)(0x80 | ((c >> 6) & 0x3F));
                    u[un++] = (char)(0x80 | (c & 0x3F));
                }

                dw_text(w, u, un);
            } else if (strcmp(t, "footnoteRef") == 0) {
                w->after_ref = 1;
            } else if (strcmp(t, "tbl") == 0) {
                if (w->started) {
                    bld_end_para(X->b);
                    w->started = 0;
                }

                dw_begin_cell(w);
                bld_table_begin(X->b);
            } else if (strcmp(t, "tr") == 0) {
                w->pend_row = 1;
                w->row_header = 0;
            } else if (strcmp(t, "trPr") == 0) {
                w->in_trpr = m.type == MT_OPEN;
            } else if (w->in_trpr && strcmp(t, "tblHeader") == 0) {
                w->row_header = attr_on(&m);
            } else if (strcmp(t, "tc") == 0) {
                if (w->pend_row) {
                    bld_row_begin(X->b, w->row_header);
                    w->pend_row = 0;
                }

                w->pend_cell = 1;
                w->span = 1;
                w->cell_bg = 0;
            } else if (strcmp(t, "tcPr") == 0) {
                w->in_tcpr = m.type == MT_OPEN;
            } else if (w->in_tcpr) {
                if (strcmp(t, "gridSpan") == 0) {
                    w->span = attr_int(&m, "w:val", 1);
                } else if (strcmp(t, "shd") == 0 && mu_attr(&m, "w:fill", v, sizeof(v)) && strcmp(v, "auto") != 0) {
                    w->cell_bg = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
                }
            }

            continue;
        }

        /* closing tags */
        if (strcmp(t, "p") == 0 && w->in_p) {
            dw_begin_para(w);   /* an empty paragraph is still one */
            bld_end_para(X->b);
            w->in_p = 0;
            w->started = 0;

            if (w->sect_here && !w->note) {     /* the section that ends with this paragraph */
                pd_block_id sec = bld_container(X->b);

                pd_doc_set_section_props(X->b->d, sec, &w->sp);
                bld_section(X->b, NULL);
                w->sect_here = 0;
            }
        } else if (strcmp(t, "pPr") == 0) {
            w->in_ppr = 0;
        } else if (strcmp(t, "sectPr") == 0) {
            w->in_sect = 0;

            if (!w->sect_here && !w->note && !w->in_p) {    /* the body's last section */
                pd_block_info bi;
                pd_block_id sec = X->b->st[0].id;

                if (pd_doc_block_info(X->b->d, sec, &bi) == PD_OK && bi.kind == PD_BLOCK_SECTION) {
                    pd_doc_set_section_props(X->b->d, sec, &w->sp);
                }
            }
        } else if (strcmp(t, "rPr") == 0) {
            w->in_rpr = 0;
        } else if (strcmp(t, "t") == 0) {
            w->in_t = 0;
        } else if (strcmp(t, "instrText") == 0) {
            w->in_instr = 0;
        } else if (strcmp(t, "drawing") == 0) {
            if (w->in_drawing && w->blip[0]) {
                dw_image(w);
            }

            w->in_drawing = 0;
        } else if (strcmp(t, "hyperlink") == 0) {
            if (w->nlinks > 0 && w->link_depth[--w->nlinks]) {
                dw_link(w, "", 0);
            }
        } else if (strcmp(t, "fldSimple") == 0) {
            if (w->simple_link) {
                dw_link(w, "", 0);
                w->simple_link = 0;
            }
        } else if (strcmp(t, "trPr") == 0) {
            w->in_trpr = 0;
        } else if (strcmp(t, "tcPr") == 0) {
            w->in_tcpr = 0;
        } else if (strcmp(t, "tc") == 0) {
            dw_begin_cell(w);
            bld_end_para(X->b);
            bld_cell_end(X->b);
        } else if (strcmp(t, "tr") == 0) {
            if (w->pend_row) {
                bld_row_begin(X->b, w->row_header);
                w->pend_row = 0;
            }
        } else if (strcmp(t, "tbl") == 0) {
            bld_table_end(X->b);
        }
    }

    free(w);
}

pd_status pd_docx_import(pd_doc* d, const unsigned char* s, size_t n) {
    dxi X;
    pd_bld b;
    char* xml;
    size_t len = 0;
    pd_status st;

    memset(&X, 0, sizeof(X));

    if (zip_open(&X.z, s, n) != 0) {
        return PD_ERR_FORMAT;
    }

    if ((xml = (char*)zip_read(&X.z, "word/_rels/document.xml.rels", &len)) != NULL) {
        read_rels(&X, xml, len);
        free(xml);
    }

    if ((xml = (char*)zip_read(&X.z, "word/styles.xml", &len)) != NULL) {
        read_styles(&X, xml, len);
        free(xml);
    }

    if ((xml = (char*)zip_read(&X.z, "word/numbering.xml", &len)) != NULL) {
        read_numbering(&X, xml, len);
        free(xml);
    }

    if ((X.notes_xml = (char*)zip_read(&X.z, "word/footnotes.xml", &X.notes_len)) != NULL) {
        read_notes(&X);
    }

    if ((xml = (char*)zip_read(&X.z, "word/document.xml", &len)) == NULL) {
        free(X.z.e);
        free(X.rels);
        free(X.styles);
        free(X.notes_xml);
        free(X.notes);
        return PD_ERR_FORMAT;
    }

    bld_init(&b, d);
    X.b = &b;
    dw_parse(&X, xml, len, 0);

    while (b.ntables > 0) {
        bld_table_end(&b);
    }

    st = bld_finish(&b);

    /* a trailing empty section (from a sectPr in the last paragraph) goes */
    {
        pd_block_info ri, si, pi;
        pd_block_id last;

        if (pd_doc_block_info(d, pd_doc_root(d), &ri) == PD_OK && ri.child_count > 1 &&
                pd_doc_block_info(d, (last = pd_doc_child(d, pd_doc_root(d), ri.child_count - 1)), &si) == PD_OK &&
                si.child_count == 1 && pd_doc_block_info(d, pd_doc_child(d, last, 0), &pi) == PD_OK &&
                pi.kind == PD_BLOCK_PARAGRAPH && pi.text_length == 0) {
            pd_doc_remove_block(d, last);
            pd_doc_clear_undo(d);
        }
    }

    free(xml);
    free(X.z.e);
    free(X.rels);
    free(X.styles);
    free(X.notes_xml);
    free(X.notes);
    return st;
}
