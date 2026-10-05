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
    pd_buf endnotes;            /* endnotes.xml body */
    int nlinks, nnotes, nendnotes;
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
                int endnote = ob->level == 1;
                int32_t k, id = endnote ? ++x->nendnotes : ++x->nnotes;
                pd_block_id spara = x->para;
                pd_char_props sbase = x->base;
                int slink = x->in_link;

                if (endnote) {
                    pb_printf(o, "<w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:endnoteReference "
                              "w:id=\"%d\"/></w:r>", (int)id);
                } else {
                    pb_printf(o, "<w:r><w:rPr><w:rStyle w:val=\"FootnoteReference\"/></w:rPr><w:footnoteReference "
                              "w:id=\"%d\"/></w:r>", (int)id);
                }

                /* the body goes to footnotes.xml or endnotes.xml */
                x->o = endnote ? &x->endnotes : &x->notes;
                x->in_note = endnote ? 2 : 1;
                x->in_link = 0;
                pb_printf(x->o, endnote ? "<w:endnote w:id=\"%d\">" : "<w:footnote w:id=\"%d\">", (int)id);

                if (pd_doc_block_info(x->d, ob->target, &si) == PD_OK) {
                    for (k = 0; k < si.child_count; k++) {
                        dx_block(x, pd_doc_child(x->d, ob->target, k));
                    }
                }

                pb_puts(x->o, endnote ? "</w:endnote>" : "</w:footnote>");
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

    memset(&dp, 0, sizeof(dp));

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

    if ((dp.mask & PD_PP_TABS) && dp.ntabs > 0) {     /* after numPr, before jc, as the schema orders them */
        static const char* al[] = { "left", "center", "right", "decimal" };
        static const char* ld[] = { "none", "dot", "hyphen", "underscore" };
        int32_t k;

        pb_puts(o, "<w:tabs>");

        for (k = 0; k < dp.ntabs && k < PD_MAX_TABS; k++) {
            pb_printf(o, "<w:tab w:val=\"%s\" w:leader=\"%s\" w:pos=\"%d\"/>", al[dp.tabs[k].align & 3],
                      ld[dp.tabs[k].leader & 3], TW(dp.tabs[k].position));
        }

        pb_puts(o, "</w:tabs>");
    }

    if (pp.align == PD_ALIGN_CENTER || pp.align == PD_ALIGN_RIGHT || (pp.align == PD_ALIGN_JUSTIFY && !sid && !num)) {
        pb_printf(o, "<w:jc w:val=\"%s\"/>", pp.align == PD_ALIGN_CENTER ? "center" : pp.align == PD_ALIGN_RIGHT ? "right" :
                  "both");
    }

    if (extra_ppr) {
        pb_puts(o, extra_ppr);
    }

    pb_puts(o, "</w:pPr>");

    if (x->in_note == 2 && bi.index == 0) {     /* the note's number opens its first paragraph */
        pb_puts(o, "<w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:endnoteRef/></w:r>"
                "<w:r><w:t xml:space=\"preserve\"> </w:t></w:r>");
    } else if (x->in_note && bi.index == 0) {
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

/* whether the cell of a row starting at grid column col continues the one above */
static int dx_merges_below(const dxo* x, pd_block_id row, int32_t col) {
    pd_block_info ri;
    int32_t c, at = 0;

    pd_doc_block_info(x->d, row, &ri);

    for (c = 0; c < ri.child_count && at <= col; c++) {
        pd_cell_props cp;

        pd_doc_cell_props(x->d, pd_doc_child(x->d, row, c), &cp);

        if (at == col) {
            return cp.merge_up;
        }

        at += cp.col_span;
    }

    return 0;
}

static void dx_table(dxo* x, pd_block_id t, pd_sp width) {
    pd_block_info ti, ri;
    pd_table_props tp;
    int32_t r, c, k, ncols = 0, col;
    pd_buf* o = x->o;
    int colw, fixed = 0;

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

    for (c = 0; c < ncols && c < tp.ncols; c++) {
        fixed |= tp.col_width[c] > 0;
    }

    pb_puts(o, "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/>");

    if (tp.width > 0) {
        pb_printf(o, "<w:tblW w:w=\"%d\" w:type=\"dxa\"/>", TW(tp.width));
    } else {
        pb_puts(o, "<w:tblW w:w=\"0\" w:type=\"auto\"/>");
    }

    if (tp.align == PD_ALIGN_CENTER || tp.align == PD_ALIGN_RIGHT) {
        pb_puts(o, tp.align == PD_ALIGN_CENTER ? "<w:jc w:val=\"center\"/>" : "<w:jc w:val=\"right\"/>");
    }

    /* said outright when Parade sizes the columns: the grid below is then
       only a starting point, and the importer reads it as one */
    if (!fixed) {
        pb_puts(o, "<w:tblLayout w:type=\"autofit\"/>");
    }

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
        pb_printf(o, "<w:gridCol w:w=\"%d\"/>", c < tp.ncols && tp.col_width[c] > 0 ? TW(tp.col_width[c]) : colw);
    }

    pb_puts(o, "</w:tblGrid>");

    for (r = 0; r < ti.child_count; r++) {
        pd_block_id row = pd_doc_child(x->d, t, r);

        pd_doc_block_info(x->d, row, &ri);
        pb_puts(o, "<w:tr>");
        col = 0;

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

            if (cp.merge_up) {
                pb_puts(o, "<w:vMerge/>");
            } else if (r + 1 < ti.child_count && dx_merges_below(x, pd_doc_child(x->d, t, r + 1), col)) {
                pb_puts(o, "<w:vMerge w:val=\"restart\"/>");
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
            col += cp.col_span;
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
            "<Override PartName=\"/word/endnotes.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.endnotes+xml\"/>"
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
    pb_printf(&part, "<w:endnotes %s><w:endnote w:type=\"separator\" w:id=\"-1\"><w:p><w:r><w:separator/></w:r></w:p>"
              "</w:endnote><w:endnote w:type=\"continuationSeparator\" w:id=\"0\"><w:p><w:r><w:continuationSeparator/>"
              "</w:r></w:p></w:endnote>", W_NS);
    pb_put(&part, x->endnotes.p, x->endnotes.n);
    pb_puts(&part, "</w:endnotes>");
    zip_add(&z, "word/endnotes.xml", part.p, part.n);

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
            "<Relationship Id=\"rIdEn\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "endnotes\" Target=\"endnotes.xml\"/>"
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
    i = doc.err || part.err || x->rels.err || x->notes.err || x->endnotes.err;
    pb_free(&part);
    pb_free(&doc);
    pb_free(&x->rels);
    pb_free(&x->notes);
    pb_free(&x->endnotes);
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

/* what a w:pPr and a w:rPr say, as masked overrides. An exact or at-least line
   height is kept as a length until the font size is known (line_abs). */
typedef struct {
    pd_para_props pp;
    pd_char_props cp;
    pd_sp line_abs;
} dprops;

typedef struct {
    char id[64];
    char name[64];
    char based_on[64];
    int type;                   /* 1 paragraph, 2 character, 0 table/numbering (not applied) */
    int outline;                /* -1 none */
    int num_id, ilvl;           /* numbering set by the style itself */
    dprops pr;
} dstyle_x;

typedef struct {
    int num_id, abstract_id;
    int start[9];               /* w:startOverride per level, -1 none */
    pd_list_id list;            /* its Parade list, once a paragraph uses it */
} dnum;

typedef struct {
    int id;
    int kind[9];                /* 1 bullet, 2 numbered */
    pd_list_level lv[9];
    int has_ind[9];
    pd_list_id list;            /* shared by every w:num without overrides, as Word counts them */
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
    char* notes_xml;            /* footnotes.xml */
    size_t notes_len;
    dnote* notes;
    int nnotes;
    char* en_xml;               /* endnotes.xml */
    size_t en_len;
    dnote* en;
    int nen;
    int depth;                  /* footnote recursion */
    dprops defaults;            /* w:docDefaults and settings.xml */
    int even_odd;               /* settings.xml: even pages have headers of their own */
    char hf_rid[16][64];        /* header/footer parts already read, and their stories */
    pd_block_id hf_story[16];
    int nhf;
    pd_block_id hf_last[6];     /* the previous section's: header default/first/even, footer the same */
    char def_pstyle[64];        /* the paragraph style of a paragraph that names none */
    char theme_major[64], theme_minor[64];  /* the theme's heading and body fonts */
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

/* twentieths of a point (twips) to sp */
static pd_sp twips(int v) {
    return (pd_sp)((int64_t)v * 65536 / 20);
}

/* one child of a w:rPr into cp; a character style name goes to rstyle */
static void rpr_elem(const dxi* X, const pd_markup* m, const char* t, pd_char_props* cp, char* rstyle,
                     size_t rcap) {
    char v[300];

    if (strcmp(t, "b") == 0) {
        cp->mask |= PD_CP_WEIGHT;
        cp->weight = attr_on(m) ? 700 : 400;
    } else if (strcmp(t, "i") == 0) {
        cp->mask |= PD_CP_ITALIC;
        cp->italic = attr_on(m);
    } else if (strcmp(t, "u") == 0) {
        cp->mask |= PD_CP_UNDERLINE;
        cp->underline = attr_on(m) ? (mu_attr(m, "w:val", v, sizeof(v)) && strcmp(v, "double") == 0 ? 2 : 1) : 0;
    } else if (strcmp(t, "strike") == 0 || strcmp(t, "dstrike") == 0) {
        cp->mask |= PD_CP_STRIKE;
        cp->strike = attr_on(m);
    } else if (strcmp(t, "vertAlign") == 0 && mu_attr(m, "w:val", v, sizeof(v))) {
        cp->mask |= PD_CP_SHIFT;
        cp->shift = strcmp(v, "superscript") == 0 ? PD_SHIFT_SUPER : strcmp(v, "subscript") == 0 ? PD_SHIFT_SUB : 0;
    } else if (strcmp(t, "color") == 0 && mu_attr(m, "w:val", v, sizeof(v)) && strcmp(v, "auto") != 0) {
        cp->mask |= PD_CP_COLOR;
        cp->color = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
    } else if (strcmp(t, "shd") == 0 && mu_attr(m, "w:fill", v, sizeof(v)) && strcmp(v, "auto") != 0) {
        cp->mask |= PD_CP_BACKGROUND;
        cp->background = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
    } else if (strcmp(t, "highlight") == 0 && mu_attr(m, "w:val", v, sizeof(v))) {
        cp->mask |= PD_CP_BACKGROUND;
        cp->background = strcmp(v, "none") == 0 ? 0 : strcmp(v, "yellow") == 0 ? 0xFFFFFF00u : strcmp(v, "green") == 0 ?
                         0xFF00FF00u : strcmp(v, "cyan") == 0 ? 0xFF00FFFFu : 0xFFFFFF00u;
    } else if (strcmp(t, "rFonts") == 0) {
        /* a named font, else the theme's: minorHAnsi is the body font, majorHAnsi the headings' */
        if (mu_attr(m, "w:ascii", v, sizeof(v)) || mu_attr(m, "w:hAnsi", v, sizeof(v))) {
            cp->mask |= PD_CP_FAMILY;
            snprintf(cp->family, sizeof(cp->family), "%.63s", v);
        } else if ((mu_attr(m, "w:asciiTheme", v, sizeof(v)) || mu_attr(m, "w:hAnsiTheme", v, sizeof(v))) &&
                   (strncmp(v, "major", 5) == 0 ? X->theme_major : X->theme_minor)[0]) {
            cp->mask |= PD_CP_FAMILY;
            snprintf(cp->family, sizeof(cp->family), "%s", strncmp(v, "major", 5) == 0 ? X->theme_major :
                     X->theme_minor);
        }
    } else if (strcmp(t, "sz") == 0) {
        int hp = attr_int(m, "w:val", 0);       /* half-points */

        if (hp > 0) {
            cp->mask |= PD_CP_SIZE;
            cp->size = (pd_sp)((int64_t)hp * 65536 / 2);
        }
    } else if (strcmp(t, "smallCaps") == 0) {
        cp->mask |= PD_CP_SMALLCAPS;
        cp->small_caps = attr_on(m);
    } else if (strcmp(t, "rStyle") == 0 && rstyle) {
        mu_attr(m, "w:val", rstyle, rcap);
    }
}

/* one child of a w:pPr into pr */
static void ppr_elem(const pd_markup* m, const char* t, dprops* pr) {
    pd_para_props* pp = &pr->pp;
    char v[64];

    if (strcmp(t, "jc") == 0 && mu_attr(m, "w:val", v, sizeof(v))) {
        pp->mask |= PD_PP_ALIGN;
        pp->align = strcmp(v, "center") == 0 ? PD_ALIGN_CENTER : strcmp(v, "right") == 0 || strcmp(v, "end") == 0 ?
                    PD_ALIGN_RIGHT : strcmp(v, "both") == 0 || strcmp(v, "distribute") == 0 ? PD_ALIGN_JUSTIFY :
                    PD_ALIGN_LEFT;
    } else if (strcmp(t, "spacing") == 0) {
        if (mu_attr(m, "w:before", v, sizeof(v))) {
            pp->mask |= PD_PP_SPACE_BEFORE;
            pp->space_before = twips(atoi(v));
        }

        if (mu_attr(m, "w:after", v, sizeof(v))) {
            pp->mask |= PD_PP_SPACE_AFTER;
            pp->space_after = twips(atoi(v));
        }

        if (mu_attr(m, "w:line", v, sizeof(v)) && atoi(v) > 0) {
            int line = atoi(v);
            char rule[16] = "auto";

            mu_attr(m, "w:lineRule", rule, sizeof(rule));
            pp->mask |= PD_PP_LINE_SPACING;

            if (strcmp(rule, "auto") == 0) {    /* in 240ths of a line */
                pp->line_spacing = line * 1000 / 240;
                pr->line_abs = 0;
            } else {                            /* exact or at least, in twips */
                pp->line_spacing = 1000;
                pr->line_abs = twips(line);
            }
        }
    } else if (strcmp(t, "ind") == 0) {
        if (mu_attr(m, "w:left", v, sizeof(v)) || mu_attr(m, "w:start", v, sizeof(v))) {
            pp->mask |= PD_PP_INDENT_LEFT;
            pp->indent_left = twips(atoi(v));
        }

        if (mu_attr(m, "w:right", v, sizeof(v)) || mu_attr(m, "w:end", v, sizeof(v))) {
            pp->mask |= PD_PP_INDENT_RIGHT;
            pp->indent_right = twips(atoi(v));
        }

        if (mu_attr(m, "w:hanging", v, sizeof(v))) {
            pp->mask |= PD_PP_INDENT_FIRST;
            pp->indent_first = -twips(atoi(v));
        } else if (mu_attr(m, "w:firstLine", v, sizeof(v))) {
            pp->mask |= PD_PP_INDENT_FIRST;
            pp->indent_first = twips(atoi(v));
        }
    } else if (strcmp(t, "keepNext") == 0) {
        pp->mask |= PD_PP_KEEP_NEXT;
        pp->keep_with_next = attr_on(m);
    } else if (strcmp(t, "keepLines") == 0) {
        pp->mask |= PD_PP_KEEP_LINES;
        pp->keep_lines = attr_on(m);
    } else if (strcmp(t, "pageBreakBefore") == 0) {
        pp->mask |= PD_PP_BREAK_BEFORE;
        pp->page_break_before = attr_on(m);
    } else if (strcmp(t, "suppressAutoHyphens") == 0) {
        pp->mask |= PD_PP_HYPHENATE;
        pp->hyphenate = !attr_on(m);
    } else if (strcmp(t, "tab") == 0 && pp->ntabs < PD_MAX_TABS && mu_attr(m, "w:val", v, sizeof(v))) {
        /* a stop, or the clearing of one the style set (align -1, merged away by pr_over) */
        pd_tab_stop* ts = &pp->tabs[pp->ntabs];
        char ld[32] = "none";

        if (strcmp(v, "bar") == 0 || strcmp(v, "num") == 0) {
            return;     /* a vertical bar, a list's own stop: not stops text goes to */
        }

        pp->mask |= PD_PP_TABS;
        ts->position = twips(attr_int(m, "w:pos", 0));
        ts->align = strcmp(v, "clear") == 0 ? -1 : strcmp(v, "center") == 0 ? PD_TAB_CENTER : strcmp(v, "right") == 0 ||
                    strcmp(v, "end") == 0 ? PD_TAB_RIGHT : strcmp(v, "decimal") == 0 ? PD_TAB_DECIMAL : PD_TAB_LEFT;
        mu_attr(m, "w:leader", ld, sizeof(ld));
        ts->leader = strcmp(ld, "dot") == 0 || strcmp(ld, "middleDot") == 0 ? PD_LEADER_DOT :
                     strcmp(ld, "hyphen") == 0 ? PD_LEADER_HYPHEN : strcmp(ld, "underscore") == 0 ||
                     strcmp(ld, "heavy") == 0 ? PD_LEADER_UNDERSCORE : PD_LEADER_NONE;
        pp->ntabs++;
    }
}

/* s over d: the fields s sets replace those of d */
static void pr_over(dprops* d, const dprops* s) {
    const pd_para_props* sp = &s->pp;
    const pd_char_props* sc = &s->cp;
    pd_para_props* dp = &d->pp;
    pd_char_props* dc = &d->cp;

    if (sp->mask & PD_PP_ALIGN) dp->align = sp->align;
    if (sp->mask & PD_PP_INDENT_LEFT) dp->indent_left = sp->indent_left;
    if (sp->mask & PD_PP_INDENT_RIGHT) dp->indent_right = sp->indent_right;
    if (sp->mask & PD_PP_INDENT_FIRST) dp->indent_first = sp->indent_first;
    if (sp->mask & PD_PP_SPACE_BEFORE) dp->space_before = sp->space_before;
    if (sp->mask & PD_PP_SPACE_AFTER) dp->space_after = sp->space_after;
    if (sp->mask & PD_PP_KEEP_NEXT) dp->keep_with_next = sp->keep_with_next;
    if (sp->mask & PD_PP_KEEP_LINES) dp->keep_lines = sp->keep_lines;
    if (sp->mask & PD_PP_BREAK_BEFORE) dp->page_break_before = sp->page_break_before;
    if (sp->mask & PD_PP_HYPHENATE) dp->hyphenate = sp->hyphenate;

    if (sp->mask & PD_PP_LINE_SPACING) {
        dp->line_spacing = sp->line_spacing;
        d->line_abs = s->line_abs;
    }

    if (sp->mask & PD_PP_TABS) {    /* Word's stops add up along the chain, and a clear takes one away */
        int32_t i, k, j;

        if (!(dp->mask & PD_PP_TABS)) {
            dp->ntabs = 0;
        }

        if (sp->tab_interval > 0) {
            dp->tab_interval = sp->tab_interval;
        }

        for (i = 0; i < sp->ntabs; i++) {
            for (k = 0; k < dp->ntabs; k++) {
                if (abs(dp->tabs[k].position - sp->tabs[i].position) <= 65536 / 20) {
                    for (j = k; j + 1 < dp->ntabs; j++) {
                        dp->tabs[j] = dp->tabs[j + 1];
                    }

                    dp->ntabs--;
                    break;
                }
            }

            if (sp->tabs[i].align >= 0 && dp->ntabs < PD_MAX_TABS) {
                for (k = dp->ntabs; k > 0 && dp->tabs[k - 1].position > sp->tabs[i].position; k--) {
                    dp->tabs[k] = dp->tabs[k - 1];
                }

                dp->tabs[k] = sp->tabs[i];
                dp->ntabs++;
            }
        }
    }

    dp->mask |= sp->mask;

    if (sc->mask & PD_CP_FAMILY) memcpy(dc->family, sc->family, sizeof(dc->family));
    if (sc->mask & PD_CP_SIZE) dc->size = sc->size;
    if (sc->mask & PD_CP_WEIGHT) dc->weight = sc->weight;
    if (sc->mask & PD_CP_ITALIC) dc->italic = sc->italic;
    if (sc->mask & PD_CP_COLOR) dc->color = sc->color;
    if (sc->mask & PD_CP_BACKGROUND) dc->background = sc->background;
    if (sc->mask & PD_CP_UNDERLINE) dc->underline = sc->underline;
    if (sc->mask & PD_CP_STRIKE) dc->strike = sc->strike;
    if (sc->mask & PD_CP_SHIFT) dc->shift = sc->shift;
    if (sc->mask & PD_CP_SMALLCAPS) dc->small_caps = sc->small_caps;

    dc->mask |= sc->mask;
}

static const dstyle_x* find_style(const dxi* X, const char* id);

/* a style with everything it is based on, the farthest ancestor first */
static void style_chain(const dxi* X, const char* id, dprops* out, int depth) {
    const dstyle_x* st = depth < 10 ? find_style(X, id) : NULL;

    if (st) {
        style_chain(X, st->based_on, out, depth + 1);
        pr_over(out, &st->pr);
    }
}

/* An exact line height as Parade's multiple of the font's own line height,
   taken as 1.15 em -- near enough for the faces documents use (Times 1.15,
   Arial 1.15, Calibri 1.22). */
static void line_finish(dprops* pr, pd_sp size) {
    if (pr->line_abs > 0 && size > 0) {
        pr->pp.line_spacing = (int32_t)((int64_t)pr->line_abs * 100000 / ((int64_t)size * 115));
    }
}

static void read_styles(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    int cap = 0, cur = -1, in_ppr = 0, in_rpr = 0, skip = 0;
    dprops* tgt = NULL;         /* the docDefaults or the style being read */

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);
        int open = m.type == MT_OPEN || m.type == MT_EMPTY;

        if (skip) {
            skip += m.type == MT_OPEN ? 1 : m.type == MT_CLOSE ? -1 : 0;
            continue;
        }

        if (open && (strcmp(t, "tblStylePr") == 0 || strcmp(t, "tblPr") == 0 || strcmp(t, "trPr") == 0 ||
                     strcmp(t, "tcPr") == 0 || (in_ppr && strcmp(t, "rPr") == 0))) {
            skip = m.type == MT_OPEN;   /* table conditions, and the paragraph mark's own run */
            continue;
        }

        if (m.type == MT_OPEN && strcmp(t, "docDefaults") == 0) {
            tgt = &X->defaults;
        } else if (m.type == MT_CLOSE && strcmp(t, "docDefaults") == 0) {
            tgt = NULL;
        } else if (m.type == MT_OPEN && strcmp(t, "style") == 0) {
            dstyle_x s;
            char v[32];

            memset(&s, 0, sizeof(s));
            s.outline = -1;
            s.num_id = -1;
            mu_attr(&m, "w:styleId", s.id, sizeof(s.id));
            s.type = !mu_attr(&m, "w:type", v, sizeof(v)) || strcmp(v, "paragraph") == 0 ? 1 :
                     strcmp(v, "character") == 0 ? 2 : 0;

            if (s.type == 1 && mu_attr(&m, "w:default", v, sizeof(v)) && (strcmp(v, "1") == 0 ||
                    strcmp(v, "true") == 0)) {
                snprintf(X->def_pstyle, sizeof(X->def_pstyle), "%s", s.id);
            }

            if (!pd_grow((void**)&X->styles, &cap, (int64_t)X->nstyles + 1, sizeof(dstyle_x))) {
                cur = X->nstyles;
                X->styles[X->nstyles++] = s;
                tgt = s.type ? &X->styles[cur].pr : NULL;
            }
        } else if (m.type == MT_CLOSE && strcmp(t, "style") == 0) {
            cur = -1;
            tgt = NULL;
        } else if (strcmp(t, "pPr") == 0) {
            in_ppr = m.type == MT_OPEN;
        } else if (strcmp(t, "rPr") == 0) {
            in_rpr = m.type == MT_OPEN;
        } else if (open && tgt && in_ppr) {
            ppr_elem(&m, t, tgt);

            if (cur >= 0 && strcmp(t, "outlineLvl") == 0) {
                X->styles[cur].outline = attr_int(&m, "w:val", -1);
            } else if (cur >= 0 && strcmp(t, "numId") == 0) {
                X->styles[cur].num_id = attr_int(&m, "w:val", -1);
            } else if (cur >= 0 && strcmp(t, "ilvl") == 0) {
                X->styles[cur].ilvl = attr_int(&m, "w:val", 0);
            }
        } else if (open && tgt && in_rpr) {
            rpr_elem(X, &m, t, &tgt->cp, NULL, 0);
        } else if (cur >= 0 && open) {
            if (strcmp(t, "name") == 0) {
                mu_attr(&m, "w:val", X->styles[cur].name, sizeof(X->styles[0].name));
            } else if (strcmp(t, "basedOn") == 0) {
                mu_attr(&m, "w:val", X->styles[cur].based_on, sizeof(X->styles[0].based_on));
            }
        }
    }
}

/* the theme's font scheme: the Latin face of the major (headings) and minor (body) fonts */
static void read_theme(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    char* which = NULL;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);

        if (m.type == MT_OPEN && strcmp(t, "majorFont") == 0) {
            which = X->theme_major;
        } else if (m.type == MT_OPEN && strcmp(t, "minorFont") == 0) {
            which = X->theme_minor;
        } else if (m.type == MT_CLOSE && (strcmp(t, "majorFont") == 0 || strcmp(t, "minorFont") == 0)) {
            which = NULL;
        } else if (which && !which[0] && (m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(t, "latin") == 0) {
            mu_attr(&m, "typeface", which, 64);
        }
    }
}

/* document-wide settings that are paragraph properties in Parade */
static void read_settings(dxi* X, const char* xml, size_t n) {
    pd_markup m;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "autoHyphenation") == 0) {
            X->defaults.pp.hyphenate = attr_on(&m);
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "evenAndOddHeaders") == 0) {
            X->even_odd = attr_on(&m);
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "defaultTabStop") == 0 &&
                   attr_int(&m, "w:val", 0) > 0) {
            X->defaults.pp.mask |= PD_PP_TABS;
            X->defaults.pp.tab_interval = twips(attr_int(&m, "w:val", 0));
        }
    }
}

/* a bullet as Word stores it -- often a character of the Symbol or Wingdings
   font, in the private use area -- as the character it shows */
static void bullet_text(const char* v, char* out, size_t cap) {
    const unsigned char* u = (const unsigned char*)v;
    uint32_t c = 0;

    if (u[0] < 0x80) {
        c = u[0];
    } else if ((u[0] & 0xE0) == 0xC0 && u[1]) {
        c = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
    } else if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) {
        c = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
    }

    if (c == 0xF0B7 || c == 0xF06C || c == 0 || c == 0xF0FC) {
        snprintf(out, cap, "\xE2\x80\xA2");     /* bullet */
    } else if (c == 'o' || c == 0xF06F) {
        snprintf(out, cap, "\xE2\x97\xA6");     /* white bullet: Courier New's "o" */
    } else if (c == 0xF0A7 || c == 0xF06E) {
        snprintf(out, cap, "\xE2\x96\xAA");     /* small square */
    } else if (c == 0xF0D8) {
        snprintf(out, cap, "\xE2\x9E\xA2");     /* arrowhead */
    } else if (c >= 0xF000 && c < 0xF100) {
        snprintf(out, cap, "\xE2\x80\xA2");
    } else {
        snprintf(out, cap, "%s", v);
    }
}

static void read_numbering(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    int cur_abs = -1, cur_lvl = -1, cur_num = -1, ovr_lvl = -1, i;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);
        pd_list_level* L = cur_abs >= 0 && cur_lvl >= 0 && cur_lvl < 9 ? &X->abss[cur_abs].lv[cur_lvl] : NULL;
        char v[64] = "";

        if (m.type == MT_CLOSE) {
            if (strcmp(t, "lvl") == 0) {
                cur_lvl = -1;
            } else if (strcmp(t, "abstractNum") == 0) {
                cur_abs = -1;
            } else if (strcmp(t, "num") == 0) {
                cur_num = -1;
            }

            continue;
        }

        if (m.type != MT_OPEN && m.type != MT_EMPTY) {
            continue;
        }

        if (strcmp(t, "abstractNum") == 0 && X->nabs < 64) {
            cur_abs = X->nabs++;
            memset(&X->abss[cur_abs], 0, sizeof(dabs));
            X->abss[cur_abs].id = attr_int(&m, "w:abstractNumId", -1);

            for (i = 0; i < 9; i++) {   /* what a level says nothing about */
                X->abss[cur_abs].lv[i].format = PD_NUM_DECIMAL;
                X->abss[cur_abs].lv[i].start = 1;
                snprintf(X->abss[cur_abs].lv[i].text, sizeof(X->abss[0].lv[0].text), "%%%d.", i + 1);
                X->abss[cur_abs].lv[i].indent = PD_PT(36) * (i + 1);
                X->abss[cur_abs].lv[i].hanging = PD_PT(18);
            }

            cur_num = -1;
        } else if (strcmp(t, "lvl") == 0 && cur_abs >= 0 && cur_num < 0) {
            cur_lvl = attr_int(&m, "w:ilvl", 0);
        } else if (L && strcmp(t, "start") == 0) {
            L->start = attr_int(&m, "w:val", 1);
        } else if (L && strcmp(t, "numFmt") == 0) {
            mu_attr(&m, "w:val", v, sizeof(v));
            X->abss[cur_abs].kind[cur_lvl] = strcmp(v, "bullet") == 0 || strcmp(v, "none") == 0 ? 1 : 2;
            L->format = strcmp(v, "bullet") == 0 ? PD_NUM_BULLET : strcmp(v, "none") == 0 ? PD_NUM_NONE :
                        strcmp(v, "lowerLetter") == 0 ? PD_NUM_LOWER_ALPHA : strcmp(v, "upperLetter") == 0 ?
                        PD_NUM_UPPER_ALPHA : strcmp(v, "lowerRoman") == 0 ? PD_NUM_LOWER_ROMAN :
                        strcmp(v, "upperRoman") == 0 ? PD_NUM_UPPER_ROMAN : PD_NUM_DECIMAL;
        } else if (L && strcmp(t, "lvlText") == 0) {
            if (mu_attr(&m, "w:val", v, sizeof(v)) || m.type == MT_EMPTY) {
                if (L->format == PD_NUM_BULLET) {
                    bullet_text(v, L->text, sizeof(L->text));
                } else {
                    snprintf(L->text, sizeof(L->text), "%s", v);  /* Word's %1.%2 is Parade's */
                }
            }
        } else if (L && strcmp(t, "ind") == 0) {
            if (mu_attr(&m, "w:left", v, sizeof(v)) || mu_attr(&m, "w:start", v, sizeof(v))) {
                L->indent = twips(atoi(v));
                X->abss[cur_abs].has_ind[cur_lvl] = 1;
            }

            if (mu_attr(&m, "w:hanging", v, sizeof(v))) {
                L->hanging = twips(atoi(v));
            } else if (mu_attr(&m, "w:firstLine", v, sizeof(v))) {
                L->hanging = 0;
            }
        } else if (strcmp(t, "num") == 0 && X->nnums < 256) {
            cur_num = X->nnums++;
            memset(&X->nums[cur_num], 0, sizeof(dnum));
            X->nums[cur_num].num_id = attr_int(&m, "w:numId", -1);
            X->nums[cur_num].abstract_id = -1;

            for (i = 0; i < 9; i++) {
                X->nums[cur_num].start[i] = -1;
            }

            cur_abs = -1;
        } else if (strcmp(t, "abstractNumId") == 0 && cur_num >= 0) {
            X->nums[cur_num].abstract_id = attr_int(&m, "w:val", -1);
        } else if (strcmp(t, "lvlOverride") == 0 && cur_num >= 0) {
            ovr_lvl = attr_int(&m, "w:ilvl", 0);
        } else if (strcmp(t, "startOverride") == 0 && cur_num >= 0 && ovr_lvl >= 0 && ovr_lvl < 9) {
            X->nums[cur_num].start[ovr_lvl] = attr_int(&m, "w:val", 1);
        }
    }
}

/* the notes of footnotes.xml or endnotes.xml: where each one's body is */
static void read_notes(const char* xml, size_t len, const char* tag, dnote** notes, int* nnotes) {
    pd_markup m;
    int cap = 0, id = 0, depth = 0;
    size_t start = 0;

    mu_init(&m, xml, len, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);

        if (m.type == MT_OPEN && strcmp(t, tag) == 0 && depth == 0) {
            id = attr_int(&m, "w:id", -999);
            start = m.pos;
            depth = 1;
        } else if (m.type == MT_OPEN && depth > 0) {
            depth++;
        } else if (m.type == MT_CLOSE && depth > 0) {
            if (--depth == 0 && strcmp(t, tag) == 0) {
                dnote nt;

                nt.id = id;
                nt.a = start;
                nt.b = m.pos - strlen(m.name) - 3;

                if (!pd_grow((void**)notes, &cap, (int64_t)*nnotes + 1, sizeof(dnote))) {
                    (*notes)[(*nnotes)++] = nt;
                }
            }
        }
    }
}

/* The Parade list a w:num stands for, made the first time it is used. A
   num that overrides no start shares its abstract definition's list, so
   two of them count on from each other the way Word counts them. */
static pd_list_id list_of(dxi* X, int num_id, const pd_list_level** lv) {
    int i, k, j, own = 0;
    dnum* nm = NULL;
    dabs* ab = NULL;

    *lv = NULL;

    for (i = 0; i < X->nnums && !nm; i++) {
        if (X->nums[i].num_id == num_id) {
            nm = &X->nums[i];
        }
    }

    for (k = 0; nm && k < X->nabs && !ab; k++) {
        if (X->abss[k].id == nm->abstract_id) {
            ab = &X->abss[k];
        }
    }

    if (!ab) {
        return 0;
    }

    *lv = ab->lv;

    for (j = 0; j < 9; j++) {
        own |= nm->start[j] >= 0;
    }

    if (own ? !nm->list : !ab->list) {
        pd_list_level lv9[9];
        pd_list_id id = 0;

        memcpy(lv9, ab->lv, sizeof(lv9));

        for (j = 0; j < 9; j++) {
            if (nm->start[j] >= 0) {
                lv9[j].start = nm->start[j];
            }
        }

        pd_doc_list_define(X->b->d, 9, lv9, &id);
        *(own ? &nm->list : &ab->list) = id;
    }

    return own ? nm->list : ab->list;
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
    dxi* X;
    /* paragraph */
    int in_p, started, in_ppr, in_rpr, in_tcpr, in_trpr, in_sect;
    char pstyle[64];
    int num_id, ilvl, outline, sect_here;
    char hf_ref[6][64];         /* the section's header/footer r:ids: default, first, even; footers after */
    int fld_kind;               /* a page-number field being read: PD_FIELD_* + 1, 0 none */
    dprops ppr;                 /* the paragraph's own w:pPr */
    pd_char_props pcp;          /* character properties of the paragraph style, defaults included */
    pd_char_props tcp;          /* what the Parade style it was given already says, resolved */
    pd_section_props sp;
    /* run */
    pd_char_props rcp;          /* the run's own w:rPr */
    char rstyle[64];
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
    int anchor, wrap, posh_align, in_posh, in_offset, in_align;   /* a floating drawing */
    long long posh_off, dist;
    struct {
        pd_res_id res;
        pd_sp w, h, gap;
        int wrap;
    } pend_fl[8];               /* floats anchored in a paragraph already under way: after it */
    int npend_fl;
    /* tables */
    int pend_row, row_header, pend_cell, span, cell_merge, cell_valign;
    uint32_t cell_bg;
    int in_tblpr, in_grid, in_borders;
    pd_sp t_width, grid[PD_TABLE_MAX_COLS];
    int t_jc, t_border_seen, t_border_any, ngrid, t_autofit;
    pd_sp t_border;
    uint32_t t_border_color;
    int skip;                   /* depth inside an ignored element */
    int note;                   /* parsing a footnote body */
    int after_ref;              /* just after the note's own number: drop the space that follows it */
} dw;

static int same_ci(const char* a, const char* b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }

    return tolower((unsigned char)*a) == tolower((unsigned char)*b);
}

/* the run's format: what the document says about its characters, less what
   the paragraph's Parade style already says */
static void dw_apply_run(dw* w) {
    dprops full;
    pd_char_props cp;
    const pd_char_props* f = &full.cp;
    const pd_char_props* t = &w->tcp;

    memset(&full, 0, sizeof(full));
    full.cp = w->pcp;
    style_chain(w->X, w->rstyle, &full, 0);

    if (!(full.cp.mask & PD_CP_FAMILY) && !(w->rcp.mask & PD_CP_FAMILY) && w->rstyle[0] &&
            (strstr(w->rstyle, "Code") || strstr(w->rstyle, "Verbatim"))) {
        full.cp.mask |= PD_CP_FAMILY;   /* a code character style that names no font */
        strcpy(full.cp.family, "monospace");
    }

    {
        dprops run;

        memset(&run, 0, sizeof(run));
        run.cp = w->rcp;
        pr_over(&full, &run);
    }

    memset(&cp, 0, sizeof(cp));

    if ((f->mask & PD_CP_FAMILY) && !same_ci(f->family, t->family)) {
        cp.mask |= PD_CP_FAMILY;
        memcpy(cp.family, f->family, sizeof(cp.family));
    }

#define DW_DIFF(bit, field) \
    if ((f->mask & (bit)) && f->field != t->field) { cp.mask |= (bit); cp.field = f->field; }
    DW_DIFF(PD_CP_SIZE, size)
    DW_DIFF(PD_CP_WEIGHT, weight)
    DW_DIFF(PD_CP_ITALIC, italic)
    DW_DIFF(PD_CP_COLOR, color)
    DW_DIFF(PD_CP_BACKGROUND, background)
    DW_DIFF(PD_CP_UNDERLINE, underline)
    DW_DIFF(PD_CP_STRIKE, strike)
    DW_DIFF(PD_CP_SHIFT, shift)
    DW_DIFF(PD_CP_SMALLCAPS, small_caps)
#undef DW_DIFF

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

        if (w->cell_merge) {
            bld_cell_merge_up(b);
        }

        if (w->cell_valign) {
            pd_cell_props cp;

            if (pd_doc_cell_props(b->d, bld_container(b), &cp) == PD_OK) {
                cp.valign = w->cell_valign;
                pd_doc_set_cell_props(b->d, bld_container(b), &cp);
            }
        }
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

/* what w:tblPr and w:tblGrid said, into the table being built */
static void dw_table_props(dw* w) {
    pd_bld* b = w->X->b;
    pd_table_props tp;
    int k;

    if (b->ntables < 1 || b->ntables > 8 || pd_doc_table_props(b->d, b->table[b->ntables - 1], &tp) != PD_OK) {
        return;
    }

    if (w->t_width > 0) {
        tp.width = w->t_width;
    }

    if (w->t_jc >= 0) {
        tp.align = w->t_jc;
    }

    if (w->t_border_seen) {     /* every edge "none" is a table without rules */
        tp.border = w->t_border_any ? (w->t_border > 0 ? w->t_border : tp.border) : 0;

        if (w->t_border_color) {
            tp.border_color = w->t_border_color;
        }
    }

    if (w->ngrid > 0 && !w->t_autofit) {    /* Word lays a table out on its grid */
        tp.ncols = w->ngrid;

        for (k = 0; k < w->ngrid; k++) {
            tp.col_width[k] = w->grid[k];
        }
    }

    pd_doc_set_table_props(b->d, b->table[b->ntables - 1], &tp);
}

static void dw_begin_para(dw* w) {
    pd_bld* b = w->X->b;
    const char* name = "";
    char low[64];
    int hop, outline = w->outline, num_id = w->num_id, ilvl = w->ilvl, list_para = 0;
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
        const pd_list_level* lv = NULL;

        ilvl = ilvl < 0 ? 0 : ilvl > 8 ? 8 : ilvl;
        bld_list(b, list_kind_of(w->X, num_id, ilvl), ilvl);
        b->list_id = list_of(w->X, num_id, &lv);

        /* Word measures a level's indent from the margin, and the paragraph's
           own w:ind replaces it; Parade adds the level's to the paragraph's */
        if (b->list_id && lv) {
            b->pp.mask |= PD_PP_INDENT_LEFT | PD_PP_INDENT_FIRST;
            b->pp.indent_left = (w->ppr.pp.mask & PD_PP_INDENT_LEFT) ? w->ppr.pp.indent_left - lv[ilvl].indent : 0;
            b->pp.indent_first = 0;
            list_para = 1;
        }
    }

    /* what the document says about the paragraph -- defaults, the style and
       what it is based on, its own w:pPr -- and, of that, what the Parade
       style it now has does not already say */
    {
        dprops full = w->X->defaults;
        pd_para_props rp;
        pd_style_id sid = pd_doc_style_find(b->d, b->pstyle[0] ? b->pstyle : "Normal");
        const pd_para_props* f = &full.pp;

        style_chain(w->X, w->pstyle[0] ? w->pstyle : w->X->def_pstyle, &full, 0);
        pr_over(&full, &w->ppr);
        pd_doc_style_resolve(b->d, sid, &rp, &w->tcp);
        line_finish(&full, (full.cp.mask & PD_CP_SIZE) ? full.cp.size : w->tcp.size);
        w->pcp = full.cp;

#define DW_DIFF(bit, field) \
        if ((f->mask & (bit)) && f->field != rp.field) { b->pp.mask |= (bit); b->pp.field = f->field; }
        DW_DIFF(PD_PP_ALIGN, align)
        if (!list_para) {   /* a list item's are settled above */
            DW_DIFF(PD_PP_INDENT_LEFT, indent_left)
            DW_DIFF(PD_PP_INDENT_FIRST, indent_first)
        }

        DW_DIFF(PD_PP_INDENT_RIGHT, indent_right)
        DW_DIFF(PD_PP_SPACE_BEFORE, space_before)
        DW_DIFF(PD_PP_SPACE_AFTER, space_after)
        DW_DIFF(PD_PP_LINE_SPACING, line_spacing)
        DW_DIFF(PD_PP_KEEP_NEXT, keep_with_next)
        DW_DIFF(PD_PP_KEEP_LINES, keep_lines)
        DW_DIFF(PD_PP_BREAK_BEFORE, page_break_before)
        DW_DIFF(PD_PP_HYPHENATE, hyphenate)
#undef DW_DIFF

        if ((f->mask & PD_PP_TABS) && (f->ntabs != rp.ntabs || f->tab_interval != rp.tab_interval ||
                                       memcmp(f->tabs, rp.tabs, (size_t)f->ntabs * sizeof(pd_tab_stop)) != 0)) {
            b->pp.mask |= PD_PP_TABS;
            b->pp.ntabs = f->ntabs;
            memcpy(b->pp.tabs, f->tabs, sizeof(b->pp.tabs));
            b->pp.tab_interval = f->tab_interval;
        }
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

static void dw_footnote(dw* w, int id, int endnote) {
    dxi* X = w->X;
    const dnote* notes = endnote ? X->en : X->notes;
    const char* xml = endnote ? X->en_xml : X->notes_xml;
    int i, n = endnote ? X->nen : X->nnotes;

    for (i = 0; i < n; i++) {
        if (notes[i].id == id && X->depth < 3) {
            pd_char_props keep = X->b->cp;

            dw_begin_para(w);
            bld_note_begin(X->b, endnote);
            X->depth++;
            dw_parse(X, xml + notes[i].a, notes[i].b - notes[i].a, 1);
            X->depth--;
            bld_footnote_end(X->b);
            X->b->cp = keep;
            return;
        }
    }
}

/* Floating drawings, as Parade floats: beside the text when Word wraps text
   round them (square, tight, through), on the side Word put them; across
   the column when it puts text above and below or none at all. Pinned at
   the anchor, which is where Word positions them from. */
static void dw_floats(dw* w) {
    pd_bld* b = w->X->b;
    int k;

    dw_begin_cell(w);

    for (k = 0; k < w->npend_fl; k++) {
        pd_block_id fl = bld_float_begin(b);
        pd_float_props fp;
        pd_inline o;

        if (fl && pd_doc_float_props(b->d, fl, &fp) == PD_OK) {
            fp.placement = PD_PLACE_HERE | PD_PLACE_FORCE;
            fp.wrap = w->pend_fl[k].wrap;
            fp.width = w->pend_fl[k].w;
            fp.gap = w->pend_fl[k].gap > 0 ? w->pend_fl[k].gap : PD_PT(9);
            pd_doc_set_float_props(b->d, fl, &fp);
        }

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_IMAGE;
        o.resource = w->pend_fl[k].res;
        o.width = w->pend_fl[k].w;
        o.height = w->pend_fl[k].h;
        bld_begin_para(b);
        bld_inline(b, &o);
        bld_float_end(b);
    }

    w->npend_fl = 0;
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
            if (!w->anchor) {
                dw_begin_para(w);
                bld_inline(X->b, &o);
            } else if (w->npend_fl < 8) {
                int k = w->npend_fl++;

                w->pend_fl[k].res = o.resource;
                w->pend_fl[k].w = o.width;
                w->pend_fl[k].h = o.height;
                w->pend_fl[k].gap = (pd_sp)(w->dist * 65536 / 12700);
                w->pend_fl[k].wrap = w->wrap;

                if (!w->started) {  /* anchored before any text: the float goes first */
                    dw_floats(w);
                }
            }
        }

        free(data);
    }
}

/* the field a Word instruction stands for, as PD_FIELD_* + 1; 0 for one Parade
   does not compute (its result text stays as text) */
static int page_field(const char* instr) {
    char word[32];
    int k = 0;

    while (*instr == ' ') {
        instr++;
    }

    while (k + 1 < (int)sizeof(word) && instr[k] && instr[k] != ' ' && instr[k] != '\\') {
        word[k] = instr[k];
        k++;
    }

    word[k] = '\0';
    return strcmp(word, "PAGE") == 0 ? PD_FIELD_PAGE + 1 : strcmp(word, "NUMPAGES") == 0 ? PD_FIELD_PAGES + 1 :
           strcmp(word, "SECTIONPAGES") == 0 ? PD_FIELD_SECTION_PAGE + 1 : 0;
}

static void dw_field(dw* w, int kind) {
    pd_inline o;

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = kind - 1;
    dw_begin_para(w);
    dw_apply_run(w);
    bld_inline(w->X->b, &o);
}

/* the story of a header or footer part, read once however many sections use it */
static pd_block_id hf_story(dxi* X, const char* rid) {
    const char* target = rel_target(X, rid, NULL);
    char path[300];
    char* xml;
    size_t len = 0;
    pd_block_id story = 0;
    int i;

    for (i = 0; i < X->nhf; i++) {
        if (strcmp(X->hf_rid[i], rid) == 0) {
            return X->hf_story[i];
        }
    }

    if (!target || X->nhf >= 16 || X->depth >= 3) {
        return 0;
    }

    snprintf(path, sizeof(path), "%s%s", target[0] == '/' ? "" : "word/", target[0] == '/' ? target + 1 : target);

    if ((xml = (char*)zip_read(&X->z, path, &len)) != NULL) {
        story = bld_story_begin(X->b);
        X->depth++;
        dw_parse(X, xml, len, 1);
        X->depth--;
        bld_story_end(X->b);
        free(xml);
    }

    snprintf(X->hf_rid[X->nhf], sizeof(X->hf_rid[0]), "%s", rid);
    X->hf_story[X->nhf++] = story;
    return story;
}

/* a section's headers and footers into its props: the ones it names, else
   the previous section's, as Word carries them over */
static void dw_section_hf(dw* w) {
    dxi* X = w->X;
    pd_block_id* f[6] = { &w->sp.header, &w->sp.header_first, &w->sp.header_even,
                          &w->sp.footer, &w->sp.footer_first, &w->sp.footer_even
                        };
    int k;

    for (k = 0; k < 6; k++) {
        *f[k] = w->hf_ref[k][0] ? hf_story(X, w->hf_ref[k]) : X->hf_last[k];
        X->hf_last[k] = *f[k];
    }

    w->sp.facing_pages = X->even_odd && (w->sp.header_even || w->sp.footer_even);
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

        if (m.type == MT_TEXT && (w->in_align || w->in_offset)) {
            char tv[32];
            size_t tn = m.tlen < sizeof(tv) - 1 ? m.tlen : sizeof(tv) - 1;

            memcpy(tv, m.text, tn);
            tv[tn] = '\0';

            if (w->in_align) {
                w->posh_align = strstr(tv, "right") || strstr(tv, "outside") ? PD_WRAP_RIGHT :
                                strstr(tv, "center") ? PD_WRAP_NONE : PD_WRAP_LEFT;
            } else {
                w->posh_off = atoll(tv);
            }

            continue;
        }

        if (m.type == MT_TEXT) {
            if (w->in_t && w->fld != 1 && !w->fld_kind) {
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
                memset(&w->ppr, 0, sizeof(w->ppr));
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
                memset(w->hf_ref, 0, sizeof(w->hf_ref));

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
                    w->sp.header_distance = attr_int(&m, "w:header", 720) * 65536 / 20;
                    w->sp.footer_distance = attr_int(&m, "w:footer", 720) * 65536 / 20;
                } else if (strcmp(t, "headerReference") == 0 || strcmp(t, "footerReference") == 0) {
                    char ty[16] = "default";
                    int k = (t[0] == 'f' ? 3 : 0);

                    mu_attr(&m, "w:type", ty, sizeof(ty));
                    k += strcmp(ty, "first") == 0 ? 1 : strcmp(ty, "even") == 0 ? 2 : 0;
                    mu_attr(&m, "r:id", w->hf_ref[k], sizeof(w->hf_ref[0]));
                } else if (strcmp(t, "titlePg") == 0) {
                    w->sp.title_page = attr_on(&m);
                } else if (strcmp(t, "pgNumType") == 0) {
                    if (mu_attr(&m, "w:start", v, sizeof(v))) {
                        w->sp.first_page_number = atoi(v);
                    }

                    if (mu_attr(&m, "w:fmt", v, sizeof(v))) {
                        w->sp.page_number_format = strcmp(v, "lowerRoman") == 0 ? PD_NUM_LOWER_ROMAN :
                                                   strcmp(v, "upperRoman") == 0 ? PD_NUM_UPPER_ROMAN :
                                                   strcmp(v, "lowerLetter") == 0 ? PD_NUM_LOWER_ALPHA :
                                                   strcmp(v, "upperLetter") == 0 ? PD_NUM_UPPER_ALPHA : PD_NUM_DECIMAL;
                    }
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
                } else if (strcmp(t, "outlineLvl") == 0) {
                    w->outline = attr_int(&m, "w:val", -1);
                } else {
                    ppr_elem(&m, t, &w->ppr);
                }
            } else if (strcmp(t, "r") == 0) {
                memset(&w->rcp, 0, sizeof(w->rcp));
                w->rstyle[0] = '\0';
            } else if (strcmp(t, "rPr") == 0) {
                w->in_rpr = m.type == MT_OPEN;
            } else if (w->in_rpr) {
                rpr_elem(X, &m, t, &w->rcp, w->rstyle, sizeof(w->rstyle));
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
                dw_footnote(w, attr_int(&m, "w:id", -999), 0);
            } else if (strcmp(t, "endnoteReference") == 0 && !w->note) {
                dw_footnote(w, attr_int(&m, "w:id", -999), 1);
            } else if (strcmp(t, "drawing") == 0) {
                w->in_drawing = m.type == MT_OPEN;
                w->blip[0] = '\0';
                w->cx = w->cy = 0;
                w->anchor = 0;
                w->wrap = PD_WRAP_NONE;
                w->posh_align = -1;
                w->posh_off = 0;
                w->dist = 0;
            } else if (w->in_drawing && strcmp(t, "anchor") == 0) {
                w->anchor = 1;
                w->dist = atoll(mu_attr(&m, "distL", v, sizeof(v)) ? v : "0");

                if (mu_attr(&m, "distR", v, sizeof(v)) && atoll(v) > w->dist) {
                    w->dist = atoll(v);
                }
            } else if (w->in_drawing && w->anchor && (strcmp(t, "wrapSquare") == 0 || strcmp(t, "wrapTight") == 0 ||
                       strcmp(t, "wrapThrough") == 0)) {
                /* text on the left only puts the drawing on the right, and the other way round */
                if (mu_attr(&m, "wrapText", v, sizeof(v)) && strcmp(v, "left") == 0) {
                    w->wrap = -PD_WRAP_RIGHT;
                } else if (mu_attr(&m, "wrapText", v, sizeof(v)) && strcmp(v, "right") == 0) {
                    w->wrap = -PD_WRAP_LEFT;
                } else {
                    w->wrap = -9;       /* by its position, once that is read */
                }
            } else if (w->in_drawing && strcmp(t, "positionH") == 0) {
                w->in_posh = m.type == MT_OPEN;
            } else if (w->in_posh && strcmp(t, "align") == 0) {
                w->in_align = m.type == MT_OPEN;
            } else if (w->in_posh && strcmp(t, "posOffset") == 0) {
                w->in_offset = m.type == MT_OPEN;
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
                int kind;

                w->simple_link = 0;

                if (mu_attr(&m, "w:instr", v, sizeof(v)) && (kind = page_field(v)) != 0) {
                    dw_field(w, kind);  /* computed here; Word's last result is not kept */

                    if (m.type == MT_OPEN) {
                        w->skip = 1;
                    }
                } else if (mu_attr(&m, "w:instr", v, sizeof(v))) {
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

                    if ((w->fld_kind = page_field(w->instr)) != 0) {
                        dw_field(w, w->fld_kind);   /* and its cached result is dropped */
                    }

                    if (q && e && e > q + 1) {
                        dw_link(w, q + 1, (size_t)(e - q - 1));
                        w->fld_link = 1;
                    }
                } else if (strcmp(v, "end") == 0) {
                    if (w->fld == 1 && page_field(w->instr)) {     /* no result part at all */
                        dw_field(w, page_field(w->instr));
                    }

                    w->fld_kind = 0;

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
            } else if (strcmp(t, "footnoteRef") == 0 || strcmp(t, "endnoteRef") == 0) {
                w->after_ref = 1;
            } else if (strcmp(t, "tbl") == 0) {
                if (w->started) {
                    bld_end_para(X->b);
                    w->started = 0;
                }

                dw_begin_cell(w);
                bld_table_begin(X->b);
                w->t_width = 0;
                w->t_jc = -1;
                w->t_border_seen = w->t_border_any = 0;
                w->t_border = 0;
                w->t_border_color = 0;
                w->ngrid = 0;
                w->t_autofit = 0;
            } else if (strcmp(t, "tblPr") == 0) {
                w->in_tblpr = m.type == MT_OPEN;
            } else if (strcmp(t, "tblGrid") == 0) {
                w->in_grid = m.type == MT_OPEN;
            } else if (w->in_grid && strcmp(t, "gridCol") == 0 && w->ngrid < PD_TABLE_MAX_COLS) {
                w->grid[w->ngrid++] = twips(attr_int(&m, "w:w", 0));
            } else if (w->in_tblpr && strcmp(t, "tblBorders") == 0) {
                w->in_borders = m.type == MT_OPEN;
            } else if (w->in_tblpr && w->in_borders) {
                w->t_border_seen = 1;

                if (attr_on(&m) && !(mu_attr(&m, "w:val", v, sizeof(v)) && strcmp(v, "nil") == 0)) {
                    pd_sp bw = (pd_sp)((int64_t)attr_int(&m, "w:sz", 4) * 65536 / 8);    /* eighths of a point */

                    w->t_border_any = 1;
                    w->t_border = bw > w->t_border ? bw : w->t_border;

                    if (mu_attr(&m, "w:color", v, sizeof(v)) && strcmp(v, "auto") != 0) {
                        w->t_border_color = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
                    }
                }
            } else if (w->in_tblpr && strcmp(t, "tblLayout") == 0) {
                /* written only by an exporter that left the widths to the reader (Parade's own) */
                w->t_autofit = mu_attr(&m, "w:type", v, sizeof(v)) && strcmp(v, "autofit") == 0;
            } else if (w->in_tblpr && strcmp(t, "tblW") == 0) {
                if (mu_attr(&m, "w:type", v, sizeof(v)) && strcmp(v, "dxa") == 0) {
                    w->t_width = twips(attr_int(&m, "w:w", 0));
                }
            } else if (w->in_tblpr && strcmp(t, "jc") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                w->t_jc = strcmp(v, "center") == 0 ? PD_ALIGN_CENTER : strcmp(v, "right") == 0 ||
                          strcmp(v, "end") == 0 ? PD_ALIGN_RIGHT : PD_ALIGN_LEFT;
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
                w->cell_merge = 0;
                w->cell_valign = 0;
            } else if (strcmp(t, "tcPr") == 0) {
                w->in_tcpr = m.type == MT_OPEN;
            } else if (w->in_tcpr) {
                if (strcmp(t, "gridSpan") == 0) {
                    w->span = attr_int(&m, "w:val", 1);
                } else if (strcmp(t, "vMerge") == 0) {   /* no value: continues the cell above */
                    w->cell_merge = !(mu_attr(&m, "w:val", v, sizeof(v)) && strcmp(v, "restart") == 0);
                } else if (strcmp(t, "vAlign") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->cell_valign = strcmp(v, "center") == 0 ? 1 : strcmp(v, "bottom") == 0 ? 2 : 0;
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

            if (w->npend_fl) {
                dw_floats(w);
            }
            w->in_p = 0;
            w->started = 0;

            if (w->sect_here && !w->note) {     /* the section that ends with this paragraph */
                pd_block_id sec = bld_container(X->b);

                dw_section_hf(w);
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
                    dw_section_hf(w);
                    pd_doc_set_section_props(X->b->d, sec, &w->sp);
                }
            }
        } else if (strcmp(t, "rPr") == 0) {
            w->in_rpr = 0;
        } else if (strcmp(t, "t") == 0) {
            w->in_t = 0;
        } else if (strcmp(t, "instrText") == 0) {
            w->in_instr = 0;
        } else if (strcmp(t, "positionH") == 0) {
            w->in_posh = 0;
        } else if (strcmp(t, "align") == 0) {
            w->in_align = 0;
        } else if (strcmp(t, "posOffset") == 0) {
            w->in_offset = 0;
        } else if (strcmp(t, "drawing") == 0) {
            if (w->wrap < 0) {      /* wrapped: which side it is on */
                w->wrap = w->wrap == -9 ? (w->posh_align >= 0 ? w->posh_align :
                                           w->posh_off > 2286000 ? PD_WRAP_RIGHT : PD_WRAP_LEFT) : -w->wrap;
            }

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
        } else if (strcmp(t, "tblPr") == 0 || strcmp(t, "tblGrid") == 0) {
            w->in_tblpr = w->in_grid = w->in_borders = 0;
            dw_table_props(w);
        } else if (strcmp(t, "tblBorders") == 0) {
            w->in_borders = 0;
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

    /* Word hyphenates only when the document asks it to */
    X.defaults.pp.mask |= PD_PP_HYPHENATE;
    X.defaults.pp.hyphenate = 0;

    if ((xml = (char*)zip_read(&X.z, "word/settings.xml", &len)) != NULL) {
        read_settings(&X, xml, len);
        free(xml);
    }

    if ((xml = (char*)zip_read(&X.z, "word/theme/theme1.xml", &len)) != NULL) {
        read_theme(&X, xml, len);
        free(xml);
    }

    if ((xml = (char*)zip_read(&X.z, "word/styles.xml", &len)) != NULL) {
        read_styles(&X, xml, len);
        free(xml);
    }

    /* Parade's Normal becomes the document's: its defaults and default
       paragraph style, which every other style is built on */
    {
        dprops base = X.defaults;
        pd_style_id normal = pd_doc_style_find(d, "Normal");
        int32_t kind = PD_STYLE_PARAGRAPH;
        pd_style_id parent = 0;
        dprops cur;

        memset(&cur, 0, sizeof(cur));
        style_chain(&X, X.def_pstyle, &base, 0);

        if (normal && pd_doc_style_info(d, normal, &kind, &parent, &cur.pp, &cur.cp) == PD_OK) {
            pd_para_props rp;
            pd_char_props rc;

            pd_doc_style_resolve(d, normal, &rp, &rc);
            line_finish(&base, (base.cp.mask & PD_CP_SIZE) ? base.cp.size : rc.size);
            pr_over(&cur, &base);
            pd_doc_style_define(d, "Normal", (pd_style_kind)kind, parent, &cur.pp, &cur.cp, NULL);
        }
    }

    if ((xml = (char*)zip_read(&X.z, "word/numbering.xml", &len)) != NULL) {
        read_numbering(&X, xml, len);
        free(xml);
    }

    if ((X.notes_xml = (char*)zip_read(&X.z, "word/footnotes.xml", &X.notes_len)) != NULL) {
        read_notes(X.notes_xml, X.notes_len, "footnote", &X.notes, &X.nnotes);
    }

    if ((X.en_xml = (char*)zip_read(&X.z, "word/endnotes.xml", &X.en_len)) != NULL) {
        read_notes(X.en_xml, X.en_len, "endnote", &X.en, &X.nen);
    }

    if ((xml = (char*)zip_read(&X.z, "word/document.xml", &len)) == NULL) {
        free(X.z.e);
        free(X.rels);
        free(X.styles);
        free(X.notes_xml);
        free(X.notes);
        free(X.en_xml);
        free(X.en);
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
    free(X.en_xml);
    free(X.en);
    return st;
}
