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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"
#include "pd_json.h"

/* v * m / d, to the nearest: what is read back converts to the same again */
#define SCALE(v, m, d) ((int64_t)(v) * (m) >= 0 ? ((int64_t)(v) * (m) + (d) / 2) / (d) : \
                        ((int64_t)(v) * (m) - (d) / 2) / (d))
#define TW(sp) ((int)SCALE(sp, 20, 65536))          /* sp -> twips */
#define EMU(sp) ((long long)SCALE(sp, 12700, 65536)) /* sp -> EMU */
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
    "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\" "
    "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
    "xmlns:wpg=\"http://schemas.microsoft.com/office/word/2010/wordprocessingGroup\" "
    "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\" "
    "xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\" "
    "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"";
static const char* XML_DECL = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";

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
    int hf_footer[8];
    int nhf;
    int hyph_auto;              /* the document hyphenates: settings.xml autoHyphenation */
    pd_block_id pending[8];     /* picture floats to anchor in the next paragraph */
    int npending;
    int page_break;             /* a page break opens the next paragraph */
    pd_block_id reft[1024];     /* paragraphs referred to that have no bookmark: given _RefPd<id> */
    int nreft;
    int even_odd;               /* some section has even-page headers */
    int mirror;                 /* some section mirrors its margins */
    int nbookmarks;
    pd_buf* hf_rels[8];
    struct {
        uint32_t off;
        int end;                /* 0: the range starts, 1: it ends */
        pd_comment_id id;
    } cb[64];                   /* comment range ends in the paragraph being written, in order */
    int ncb, cbi;
    int nrev;                   /* w:ins / w:del ids given out */
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

static const char* dx_u_name(int32_t u) {
    static const char* names[] = { "none", "single", "double", "thick", "dotted", "dash", "wave", "words" };

    return u >= 0 && u <= PD_UNDERLINE_WORDS ? names[u] : "single";
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

    if (!c->caps != !b->caps) {
        pb_puts(o, c->caps ? "<w:caps/>" : "<w:caps w:val=\"0\"/>");
    }

    if (!c->small_caps != !b->small_caps) {
        pb_puts(o, c->small_caps ? "<w:smallCaps/>" : "<w:smallCaps w:val=\"0\"/>");
    }

    if (c->strike && !b->strike) {
        pb_puts(o, "<w:strike/>");
    }

    if (!c->hidden != !b->hidden) {
        pb_puts(o, c->hidden ? "<w:vanish/>" : "<w:vanish w:val=\"0\"/>");
    }

    if (c->color != b->color) {
        pb_printf(o, "<w:color w:val=\"%06X\"/>", (unsigned)(c->color & 0xFFFFFF));
    }

    if (c->letter_space != b->letter_space) {
        pb_printf(o, "<w:spacing w:val=\"%d\"/>", TW(c->letter_space));
    }

    if (!c->kerning != !b->kerning) {
        pb_printf(o, "<w:kern w:val=\"%d\"/>", c->kerning ? 2 : 0);
    }

    if (c->position != b->position) {
        pb_printf(o, "<w:position w:val=\"%d\"/>", (int)SCALE(c->position, 2, 65536));
    }

    if (c->size != b->size) {
        pb_printf(o, "<w:sz w:val=\"%d\"/>", (int)SCALE(c->size, 2, 65536));
    }

    if (c->underline != b->underline) {
        pb_printf(o, "<w:u w:val=\"%s\"/>", dx_u_name(c->underline));
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
static void dx_text_as(pd_buf* o, const char* s, size_t n, const char* tag) {
    size_t i, j = 0;

    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '\t' || s[i] == '\n') {
            if (i > j) {
                pb_printf(o, "<%s xml:space=\"preserve\">", tag);
                xesc(o, s + j, i - j);
                pb_printf(o, "</%s>", tag);
            }

            if (i < n) {
                pb_puts(o, s[i] == '\t' ? "<w:tab/>" : "<w:br/>");
            }

            j = i + 1;
        }
    }
}

static void dx_text(pd_buf* o, const char* s, size_t n) {
    dx_text_as(o, s, n, "w:t");
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
             strstr(mime, "png") ? "png" : strstr(mime, "gif") ? "gif" : strstr(mime, "emf") ? "emf" :
             strstr(mime, "wmf") ? "wmf" : "jpeg");
    pb_printf(&x->rels, "<Relationship Id=\"rIdm%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
              "relationships/image\" Target=\"media/%s\"/>", x->nmedia + 1, x->media[x->nmedia].name);
    *rid_name = x->media[x->nmedia].name;
    return x->nmedia++;
}

static void dx_block(dxo* x, pd_block_id id);
static void dx_para(dxo* x, pd_block_id p, const char* extra_ppr);
static void dx_sid(const pd_doc* d, pd_style_id sid, char* out, size_t cap);
static void dx_ppr_head(pd_buf* o, const pd_para_props* pp, uint32_t m);
static void dx_ppr_tail(pd_buf* o, const pd_para_props* pp, uint32_t m, int hyph_auto, const pd_para_props* base);

/* a picture as a run: inline, or anchored where a float is (fp) -- beside the text on the side the float
   wraps on, or across the column */
static void dx_drawing_group(dxo* x, const void* data, size_t len, long long cx, long long cy);
static int dx_ref_name(const pd_doc* d, pd_block_id para, char* out, size_t cap);

static void dx_picture(dxo* x, const pd_inline* ob, const pd_float_props* fp) {
    pd_buf* o = x->o;
    const char* name = "";
    const char* mime = "";
    const void* data = NULL;
    size_t len = 0;
    int group = pd_doc_resource(x->d, ob->resource, &mime, &data, &len) == PD_OK &&
                strcmp(mime, "application/vnd.parade.drawing+json") == 0;
    pd_res_id pic = ob->resource;
    int m;

    if (group) {    /* a metafile played into a drawing: the metafile itself goes back */
        pj_doc* jd = pj_parse(data, len, 0, NULL);
        pd_res_id src = jd ? (pd_res_id)pj_int_or(pj_get(pj_root(jd), "src"), 0) : 0;

        pj_free(jd);

        if (src) {
            group = 0;
            pic = src;
        }
    }

    m = group ? 0 : dx_media(x, pic, &name);
    pd_sp iw, ih;
    long long cx, cy;
    size_t mark;

    if (m < 0) {
        return;
    }

    pd_doc_image_display_size(x->d, ob, &iw, &ih);
    cx = EMU(iw);
    cy = EMU(ih);
    x->docpr++;
    mark = o->n;

    if (!fp) {
        pb_printf(o, "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                  "<wp:extent cx=\"%lld\" cy=\"%lld\"/>", cx, cy);
    } else {
        long long gap = EMU(fp->gap);
        int off = (fp->placement & PD_PLACE_OFFSET) && fp->wrap != PD_WRAP_NONE;

        pb_printf(o, "<w:r><w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"%lld\" distR=\"%lld\" "
                  "simplePos=\"0\" relativeHeight=\"%d\" behindDoc=\"0\" locked=\"0\" layoutInCell=\"1\" "
                  "allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\">",
                  gap, gap, x->docpr);

        if (off) {
            pb_printf(o, "<wp:posOffset>%lld</wp:posOffset>", EMU(fp->offset_x));
        } else {
            pb_printf(o, "<wp:align>%s</wp:align>", fp->wrap == PD_WRAP_LEFT ? "left" : fp->wrap == PD_WRAP_RIGHT ?
                      "right" : "center");
        }

        /* at an offset, which side the text is on says which side the picture is */
        pb_printf(o, "</wp:positionH><wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset>"
                  "</wp:positionV><wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>%s",
                  cx, cy, fp->wrap == PD_WRAP_NONE ? "<wp:wrapTopAndBottom/>" : !off ?
                  "<wp:wrapSquare wrapText=\"bothSides\"/>" : fp->wrap == PD_WRAP_LEFT ?
                  "<wp:wrapSquare wrapText=\"right\"/>" : "<wp:wrapSquare wrapText=\"left\"/>");
    }

    if (group) {    /* Word keeps a group in markup it alone reads, and says so: after the run's opening */
        static const char mc[] = "<mc:AlternateContent><mc:Choice Requires=\"wpg\">";
        size_t at = mark + strlen("<w:r>"), k = strlen(mc), tail = o->n - at;

        pb_put(o, mc, k);   /* room, then the drawing moved up behind it */

        if (!o->err) {
            memmove(o->p + at + k, o->p + at, tail);
            memcpy(o->p + at, mc, k);
        }
    }

    pb_printf(o, "<wp:docPr id=\"%d\" name=\"Picture %d\"", x->docpr, x->docpr);

    if (ob->alt_len > 0) {
        pb_puts(o, " descr=\"");
        xesc(o, ob->alt, (size_t)ob->alt_len);
        pb_putc(o, '"');
    }

    if (group) {
        pb_printf(o, "/>%s<a:graphic><a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingGroup\">",
                  fp ? "<wp:cNvGraphicFramePr/>" : "");
        dx_drawing_group(x, data, len, cx, cy);
        pb_printf(o, "</a:graphicData></a:graphic>%s</w:drawing></mc:Choice></mc:AlternateContent></w:r>",
                  fp ? "</wp:anchor>" : "</wp:inline>");
        return;
    }

    pb_printf(o, "/>%s<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
              "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
              "<pic:blipFill><a:blip r:embed=\"rIdm%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
              "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
              "</a:graphic>%s</w:drawing></w:r>", fp ? "<wp:cNvGraphicFramePr/>" : "", x->docpr, name, m + 1, cx, cy,
              fp ? "</wp:anchor>" : "</wp:inline>");
}

static long long jnum(const pj_node* o, const char* k) {
    return (long long)pj_int_or(pj_get(o, k), 0);
}

/* a colour of a drawing as DrawingML: a fill or nothing */
static void dx_fill(pd_buf* o, uint32_t c) {
    if (c) {
        pb_printf(o, "<a:solidFill><a:srgbClr val=\"%06X\"/></a:solidFill>", (unsigned)(c & 0xFFFFFF));
    } else {
        pb_puts(o, "<a:noFill/>");
    }
}

/* A drawing resource (a Word canvas or group read in) as a Word group: its
   pictures, shapes and text boxes in the group's own coordinates, the
   drawing's (sp, as EMU), shown at cx x cy. */
static void dx_drawing_group(dxo* x, const void* data, size_t len, long long cx, long long cy) {
    pd_buf* o = x->o;
    pj_doc* doc = pj_parse(data, len, 0, NULL);
    const pj_node* root = doc ? pj_root(doc) : NULL, *items = root ? pj_get(root, "items") : NULL, *it;
    int id = 1;

    pb_printf(o, "<wpg:wgp><wpg:cNvGrpSpPr/><wpg:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/>"
              "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"%lld\" cy=\"%lld\"/></a:xfrm></wpg:grpSpPr>", cx, cy,
              root ? EMU(jnum(root, "w")) : cx, root ? EMU(jnum(root, "h")) : cy);

    for (it = items ? items->child : NULL; it; it = it->next) {
        long long ix = EMU(jnum(it, "x")), iy = EMU(jnum(it, "y")), iw = EMU(jnum(it, "w")), ih = EMU(jnum(it, "h"));
        char xfrm[200];

        snprintf(xfrm, sizeof(xfrm), "<a:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>", ix,
                 iy, iw, ih);
        id++;

        if (pj_get(it, "img")) {
            const char* name;
            pd_res_id cr = (pd_res_id)jnum(it, "img");
            const char* cm = "";
            const void* cd = NULL;
            size_t cn = 0;
            int m;

            if (pd_doc_resource(x->d, cr, &cm, &cd, &cn) == PD_OK && strcmp(cm, PD_DRAWING_MIME) == 0) {
                pj_doc* jd = pj_parse(cd, cn, 0, NULL);    /* a metafile in the group: the metafile itself */

                cr = jd ? (pd_res_id)pj_int_or(pj_get(pj_root(jd), "src"), 0) : 0;
                pj_free(jd);
            }

            m = cr ? dx_media(x, cr, &name) : -1;

            if (m >= 0) {
                pb_printf(o, "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
                          "<pic:blipFill><a:blip r:embed=\"rIdm%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                          "<pic:spPr>%s<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>", id, name, m + 1,
                          xfrm);
            }
        } else if (pj_get(it, "shape")) {
            const pj_node* sh = pj_get(it, "shape");
            char geom[16] = "rect";

            if (sh->type == PJ_STR && sh->len < sizeof(geom)) {
                memcpy(geom, sh->s, sh->len);
                geom[sh->len] = '\0';
            }

            pb_printf(o, "<wps:wsp><wps:cNvPr id=\"%d\" name=\"Shape %d\"/><wps:cNvSpPr/><wps:spPr>%s"
                      "<a:prstGeom prst=\"%s\"><a:avLst/></a:prstGeom>", id, id, xfrm, geom);
            dx_fill(o, (uint32_t)jnum(it, "fill"));
            pb_printf(o, "<a:ln w=\"%lld\">", EMU(jnum(it, "lw")));
            dx_fill(o, (uint32_t)jnum(it, "line"));
            pb_puts(o, "</a:ln></wps:spPr><wps:bodyPr/></wps:wsp>");
        } else if (pj_get(it, "path")) {    /* a path: a shape of custom geometry in its own box */
            const pj_node* pts = pj_get(it, "path"), *q;
            long long bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
            int first = 1, k, start = 1;

            for (q = pts->child, k = 0; q && q->next; q = q->next->next, k++) {
                long long px = pj_int_or(q, 0), py = pj_int_or(q->next, 0);

                if (px == INT32_MIN) {
                    continue;
                }

                px = EMU(px);
                py = EMU(py);

                if (first || px < bx0) bx0 = px;
                if (first || py < by0) by0 = py;
                if (first || px > bx1) bx1 = px;
                if (first || py > by1) by1 = py;
                first = 0;
            }

            if (first) {
                continue;
            }

            pb_printf(o, "<wps:wsp><wps:cNvPr id=\"%d\" name=\"Freeform %d\"/><wps:cNvSpPr/><wps:spPr><a:xfrm><a:off x=\"%lld\" "
                      "y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm><a:custGeom><a:pathLst><a:path w=\"%lld\" "
                      "h=\"%lld\">", id, id, bx0, by0, bx1 - bx0, by1 - by0, bx1 - bx0 > 0 ? bx1 - bx0 : 1,
                      by1 - by0 > 0 ? by1 - by0 : 1);

            for (q = pts->child; q && q->next; q = q->next->next) {
                long long px = pj_int_or(q, 0), py = pj_int_or(q->next, 0);

                if (px == INT32_MIN) {
                    if (!start && jnum(it, "closed")) {
                        pb_puts(o, "<a:close/>");
                    }

                    start = 1;
                    continue;
                }

                pb_printf(o, "<a:%s><a:pt x=\"%lld\" y=\"%lld\"/></a:%s>", start ? "moveTo" : "lnTo", EMU(px) - bx0,
                          EMU(py) - by0, start ? "moveTo" : "lnTo");
                start = 0;
            }

            if (!start && jnum(it, "closed")) {
                pb_puts(o, "<a:close/>");
            }

            pb_puts(o, "</a:path></a:pathLst></a:custGeom>");
            dx_fill(o, (uint32_t)jnum(it, "fill"));
            pb_printf(o, "<a:ln w=\"%lld\">", EMU(jnum(it, "lw")));
            dx_fill(o, (uint32_t)jnum(it, "line"));
            pb_puts(o, "</a:ln></wps:spPr><wps:bodyPr/></wps:wsp>");
        } else if (pj_get(it, "label")) {   /* a line of text at its baseline: a text box around it */
            const pj_node* t = pj_get(it, "label"), *f = pj_get(it, "f");
            long long sz = EMU(jnum(it, "sz")), lx = EMU(jnum(it, "x")), ly = EMU(jnum(it, "y")) - sz;
            long long lw2 = sz * (long long)(t && t->type == PJ_STR ? t->len : 1) * 6 / 10 + sz;
            int ha = (int)jnum(it, "ha");

            if (!t || t->type != PJ_STR) {
                continue;
            }

            lx -= ha == 1 ? lw2 / 2 : ha == 2 ? lw2 : 0;
            pb_printf(o, "<wps:wsp><wps:cNvPr id=\"%d\" name=\"Label %d\"/><wps:cNvSpPr txBox=\"1\"/><wps:spPr><a:xfrm>"
                      "<a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/>"
                      "</a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr><wps:txbx><w:txbxContent><w:p><w:pPr>"
                      "<w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/><w:jc w:val=\"%s\"/></w:pPr><w:r><w:rPr>",
                      id, id, lx, ly, lw2, sz * 13 / 10, ha == 1 ? "center" : ha == 2 ? "right" : "left");

            if (f && f->type == PJ_STR) {
                pb_puts(o, "<w:rFonts w:ascii=\"");
                xesc(o, f->s, f->len);
                pb_puts(o, "\" w:hAnsi=\"");
                xesc(o, f->s, f->len);
                pb_puts(o, "\"/>");
            }

            pb_puts(o, jnum(it, "w") >= 600 ? "<w:b/>" : "");
            pb_puts(o, jnum(it, "i") ? "<w:i/>" : "");

            if ((uint32_t)jnum(it, "c") & 0xFFFFFF) {
                pb_printf(o, "<w:color w:val=\"%06X\"/>", (unsigned)(jnum(it, "c") & 0xFFFFFF));
            }

            pb_printf(o, "<w:sz w:val=\"%d\"/></w:rPr><w:t xml:space=\"preserve\">", (int)SCALE(jnum(it, "sz"), 2, 65536));
            xesc(o, t->s, t->len);
            pb_puts(o, "</w:t></w:r></w:p></w:txbxContent></wps:txbx><wps:bodyPr lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\" "
                    "anchor=\"b\"/></wps:wsp>");
        } else if (pj_get(it, "text")) {
            const pj_node* p, *r, *ins = pj_get(it, "ins"), *an = pj_get(it, "anchor");

            pb_printf(o, "<wps:wsp><wps:cNvPr id=\"%d\" name=\"Text Box %d\"/><wps:cNvSpPr txBox=\"1\"/><wps:spPr>%s"
                      "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr>"
                      "<wps:txbx><w:txbxContent>", id, id, xfrm);

            for (p = pj_get(it, "text")->child; p; p = p->next) {
                int a = (int)jnum(p, "a");

                pb_printf(o, "<w:p><w:pPr><w:spacing w:after=\"0\"/><w:jc w:val=\"%s\"/></w:pPr>", a == PD_ALIGN_CENTER ?
                          "center" : a == PD_ALIGN_RIGHT ? "right" : a == PD_ALIGN_JUSTIFY ? "both" : "left");

                for (r = pj_get(p, "runs") ? pj_get(p, "runs")->child : NULL; r; r = r->next) {
                    const pj_node* t = pj_get(r, "t"), *f = pj_get(r, "f");

                    if (!t || t->type != PJ_STR) {
                        continue;
                    }

                    pb_puts(o, "<w:r><w:rPr>");

                    if (f && f->type == PJ_STR) {
                        pb_puts(o, "<w:rFonts w:ascii=\"");
                        xesc(o, f->s, f->len);
                        pb_puts(o, "\" w:hAnsi=\"");
                        xesc(o, f->s, f->len);
                        pb_puts(o, "\"/>");
                    }

                    pb_puts(o, jnum(r, "w") >= 600 ? "<w:b/>" : "");
                    pb_puts(o, jnum(r, "i") ? "<w:i/>" : "");

                    if ((uint32_t)jnum(r, "c") & 0xFFFFFF) {
                        pb_printf(o, "<w:color w:val=\"%06X\"/>", (unsigned)(jnum(r, "c") & 0xFFFFFF));
                    }

                    if (jnum(r, "sz") > 0) {
                        pb_printf(o, "<w:sz w:val=\"%d\"/>", (int)SCALE(jnum(r, "sz"), 2, 65536));
                    }

                    if (jnum(r, "u")) {
                        pb_printf(o, "<w:u w:val=\"%s\"/>", dx_u_name((int32_t)jnum(r, "u")));
                    }

                    if (jnum(r, "s")) {
                        pb_puts(o, jnum(r, "s") == PD_SHIFT_SUPER ? "<w:vertAlign w:val=\"superscript\"/>" :
                                "<w:vertAlign w:val=\"subscript\"/>");
                    }

                    pb_puts(o, "</w:rPr><w:t xml:space=\"preserve\">");
                    xesc(o, t->s, t->len);
                    pb_puts(o, "</w:t></w:r>");
                }

                pb_puts(o, "</w:p>");
            }

            pb_printf(o, "</w:txbxContent></wps:txbx><wps:bodyPr lIns=\"%lld\" tIns=\"%lld\" rIns=\"%lld\" bIns=\"%lld\" "
                      "anchor=\"%s\"/></wps:wsp>", EMU(pj_int_or(pj_at(ins, 0), 0)), EMU(pj_int_or(pj_at(ins, 1), 0)),
                      EMU(pj_int_or(pj_at(ins, 2), 0)), EMU(pj_int_or(pj_at(ins, 3), 0)),
                      an && an->type == PJ_STR && an->len >= 3 && !memcmp(an->s, "ctr", 3) ? "ctr" :
                      an && an->type == PJ_STR && an->len >= 1 && an->s[0] == 'b' ? "b" : "t");
        }
    }

    pb_puts(o, "</wpg:wgp>");
    pj_free(doc);
}

/* a float that is only a picture: Word anchors it in the paragraph that follows */
static int dx_float_picture(const dxo* x, pd_block_id fl, pd_inline* ob) {
    pd_block_info fi, pi;
    const char* t;
    uint32_t n;
    pd_pos at;

    if (pd_doc_block_info(x->d, fl, &fi) != PD_OK || fi.child_count != 1 ||
            pd_doc_block_info(x->d, pd_doc_child(x->d, fl, 0), &pi) != PD_OK || pi.kind != PD_BLOCK_PARAGRAPH ||
            pd_doc_para_text(x->d, pi.id, &t, &n) != PD_OK || n != 3 || memcmp(t, "\xEF\xBF\xBC", 3) != 0) {
        return 0;
    }

    at.block = pi.id;
    at.offset = 0;
    return pd_doc_inline_at(x->d, at, ob) == PD_OK && ob->kind == PD_INLINE_IMAGE;
}

static pd_sp dx_text_width(const dxo* x);
static void dx_flush_anchors(dxo* x);

/* a rough height for a text box Word sizes to its text: pictures at their height, text at a line per 80 characters */
static pd_sp dx_block_height(const dxo* x, pd_block_id b, pd_sp width) {
    pd_block_info bi;
    pd_sp h = 0;
    int32_t i;

    if (pd_doc_block_info(x->d, b, &bi) != PD_OK) {
        return 0;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        const char* t;
        uint32_t n, k;
        pd_sp pic = 0;

        pd_doc_para_text(x->d, b, &t, &n);

        for (k = 0; k + 3 <= n; k++) {
            pd_inline o;
            pd_pos at;

            at.block = b;
            at.offset = k;

            if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(x->d, at, &o) == PD_OK && o.kind == PD_INLINE_IMAGE) {
                pd_sp iw, ih;

                pd_doc_image_display_size(x->d, &o, &iw, &ih);
                pic = ih > pic ? ih : pic;
            }
        }

        return pic + PD_PT(14) * (1 + (pd_sp)(n * PD_PT(5) / (width > PD_PT(36) ? width : PD_PT(36))));
    }

    for (i = 0; i < bi.child_count; i++) {
        h += dx_block_height(x, pd_doc_child(x->d, b, i), width);
    }

    return h;
}

/* A float of more than a picture -- a figure with its caption, a box of
   text -- as an anchored Word text box holding its blocks, beside the text
   or across the column as the float is, grown by Word to what it holds. */
static void dx_textbox(dxo* x, pd_block_id fl, const pd_float_props* fp) {
    pd_buf* o = x->o;
    pd_block_info fi;
    pd_sp w = fp->width > 0 ? fp->width : (pd_sp)((int64_t)dx_text_width(x) * (fp->width_fraction > 0 ?
              fp->width_fraction : 1000) / 1000);
    long long cx = EMU(w), cy = EMU(dx_block_height(x, fl, w)), gap = EMU(fp->gap);
    int off = (fp->placement & PD_PLACE_OFFSET) && fp->wrap != PD_WRAP_NONE;
    int32_t i;

    if (pd_doc_block_info(x->d, fl, &fi) != PD_OK) {
        return;
    }

    x->docpr++;
    pb_printf(o, "<w:r><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:drawing><wp:anchor distT=\"0\" distB=\"0\" "
              "distL=\"%lld\" distR=\"%lld\" simplePos=\"0\" relativeHeight=\"%d\" behindDoc=\"0\" locked=\"0\" "
              "layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\">",
              gap, gap, x->docpr);

    if (off) {
        pb_printf(o, "<wp:posOffset>%lld</wp:posOffset>", EMU(fp->offset_x));
    } else {
        pb_printf(o, "<wp:align>%s</wp:align>", fp->wrap == PD_WRAP_LEFT ? "left" : fp->wrap == PD_WRAP_RIGHT ? "right" :
                  "center");
    }

    pb_printf(o, "</wp:positionH><wp:positionV relativeFrom=\"paragraph\"><wp:posOffset>0</wp:posOffset></wp:positionV>"
              "<wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>%s"
              "<wp:docPr id=\"%d\" name=\"Text Box %d\"/><wp:cNvGraphicFramePr/><a:graphic><a:graphicData "
              "uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp><wps:cNvSpPr txBox=\"1\"/>"
              "<wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm><a:prstGeom prst=\"rect\">"
              "<a:avLst/></a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr><wps:txbx><w:txbxContent>", cx, cy,
              fp->wrap == PD_WRAP_NONE ? "<wp:wrapTopAndBottom/>" : !off ? "<wp:wrapSquare wrapText=\"bothSides\"/>" :
              fp->wrap == PD_WRAP_LEFT ? "<wp:wrapSquare wrapText=\"right\"/>" : "<wp:wrapSquare wrapText=\"left\"/>",
              x->docpr, x->docpr, cx, cy);

    for (i = 0; i < fi.child_count; i++) {
        dx_block(x, pd_doc_child(x->d, fl, i));
    }

    dx_flush_anchors(x);
    pb_puts(o, "</w:txbxContent></wps:txbx><wps:bodyPr rot=\"0\" vert=\"horz\" wrap=\"square\" lIns=\"0\" tIns=\"0\" "
            "rIns=\"0\" bIns=\"0\" anchor=\"t\"><a:spAutoFit/></wps:bodyPr></wps:wsp></a:graphicData></a:graphic>"
            "</wp:anchor></w:drawing></mc:Choice></mc:AlternateContent></w:r>");
}

/* the anchored pictures waiting for a paragraph */
static void dx_textbox(dxo* x, pd_block_id fl, const pd_float_props* fp);

static void dx_anchors(dxo* x) {
    int i, n = x->npending;
    pd_block_id pend[8];

    /* taken off the list first: a text box's own paragraphs anchor nothing of these */
    memcpy(pend, x->pending, sizeof(pend));
    x->npending = 0;

    for (i = 0; i < n; i++) {
        pd_inline ob;
        pd_float_props fp;

        if (dx_float_picture(x, pend[i], &ob) && pd_doc_float_props(x->d, pend[i], &fp) == PD_OK) {
            dx_picture(x, &ob, &fp);
        } else if (pd_doc_float_props(x->d, pend[i], &fp) == PD_OK) {
            dx_textbox(x, pend[i], &fp);
        }
    }
}

/* ... and when none comes, in a paragraph of their own */
static void dx_flush_anchors(dxo* x) {
    if (x->npending > 0) {
        pb_puts(x->o, "<w:p>");
        dx_anchors(x);
        pb_puts(x->o, "</w:p>");
    }
}

/* the comment range ends up to an offset of the paragraph being written */
static void dx_marks(dxo* x, uint32_t upto) {
    while (x->cbi < x->ncb && x->cb[x->cbi].off <= upto) {
        int id = (int)x->cb[x->cbi].id;

        if (x->cb[x->cbi].end) {
            pb_printf(x->o, "<w:commentRangeEnd w:id=\"%d\"/><w:r><w:rPr><w:rStyle w:val=\"CommentReference\"/></w:rPr>"
                      "<w:commentReference w:id=\"%d\"/></w:r>", id, id);
        } else {
            pb_printf(x->o, "<w:commentRangeStart w:id=\"%d\"/>", id);
        }

        x->cbi++;
    }
}

/* where the comments' ranges start and end in a paragraph */
static void dx_comment_bounds(dxo* x, pd_block_id p) {
    int32_t i, j;

    x->ncb = x->cbi = 0;

    for (i = 1; i <= pd_doc_comment_count(x->d); i++) {
        pd_comment c;
        int e;

        if (pd_doc_comment_get(x->d, (pd_comment_id)i, &c) != PD_OK) {
            continue;
        }

        for (e = 0; e < 2; e++) {
            const pd_pos* q = e ? &c.range.end : &c.range.start;

            if (q->block == p && x->ncb < 64) {
                for (j = x->ncb; j > 0 && (x->cb[j - 1].off > q->offset || (x->cb[j - 1].off == q->offset &&
                                           x->cb[j - 1].end > e)); j--) {
                    x->cb[j] = x->cb[j - 1];
                }

                x->cb[j].off = q->offset;
                x->cb[j].end = e;
                x->cb[j].id = (pd_comment_id)i;
                x->ncb++;
            }
        }
    }
}

/* one run of text, inside w:ins or w:del when it is a tracked change */
static void dx_run(dxo* x, const pd_span* sp, const char* t, size_t n) {
    pd_buf* o = x->o;
    pd_revision rv;
    int kind = 0;

    if (sp->cp.revision && pd_doc_revision_get(x->d, sp->cp.revision, &rv) == PD_OK) {
        kind = rv.kind;
        pb_printf(o, "<w:%s w:id=\"%d\" w:author=\"", kind == PD_REV_DELETE ? "del" : "ins", ++x->nrev);
        xesc(o, rv.author, strlen(rv.author));

        if (rv.date[0]) {
            pb_puts(o, "\" w:date=\"");
            xesc(o, rv.date, strlen(rv.date));
        }

        pb_puts(o, "\">");
    }

    pb_puts(o, "<w:r>");
    dx_rpr(x, &sp->cp, &x->base, x->in_link ? "Hyperlink" : NULL);
    dx_text_as(o, t, n, kind == PD_REV_DELETE ? "w:delText" : "w:t");
    pb_puts(o, "</w:r>");

    if (kind) {
        pb_puts(o, kind == PD_REV_DELETE ? "</w:del>" : "</w:ins>");
    }
}

/* comments.xml (and commentsExtended.xml, for replies and resolved ones); 0 when there are none */
static int dx_comments(const pd_doc* d, pd_buf* o, pd_buf* ex) {
    int32_t i, n = 0;

    pb_puts(o, XML_DECL);
    pb_printf(o, "<w:comments %s>", W_NS);
    pb_puts(ex, XML_DECL);
    pb_puts(ex, "<w15:commentsEx xmlns:w15=\"http://schemas.microsoft.com/office/word/2012/wordml\">");

    for (i = 1; i <= pd_doc_comment_count(d); i++) {
        pd_comment c;
        uint32_t k, j = 0;
        char ini[8];
        int ni = 0, word = 1;

        if (pd_doc_comment_get(d, (pd_comment_id)i, &c) != PD_OK) {
            continue;
        }

        for (k = 0; c.author[k] && ni < 4; k++) {    /* initials: the first letters of the words */
            if (c.author[k] == ' ') {
                word = 1;
            } else if (word && (unsigned char)c.author[k] < 128) {
                ini[ni++] = c.author[k];
                word = 0;
            } else {
                word = 0;
            }
        }

        ini[ni] = '\0';
        n++;
        pb_printf(o, "<w:comment w:id=\"%d\" w:author=\"", (int)i);
        xesc(o, c.author, strlen(c.author));

        if (c.date[0]) {
            pb_puts(o, "\" w:date=\"");
            xesc(o, c.date, strlen(c.date));
        }

        pb_printf(o, "\" w:initials=\"%s\">", ini);

        for (k = 0; k <= c.text_len; k++) {
            if (k == c.text_len || c.text[k] == '\n') {
                if (k == c.text_len) {
                    pb_printf(o, "<w:p w14:paraId=\"%08X\">", 0x10000000u + (unsigned)i);
                } else {
                    pb_puts(o, "<w:p>");
                }

                pb_puts(o, "<w:pPr><w:pStyle w:val=\"CommentText\"/></w:pPr>");

                if (j == 0) {
                    pb_puts(o, "<w:r><w:rPr><w:rStyle w:val=\"CommentReference\"/></w:rPr><w:annotationRef/></w:r>");
                }

                if (k > j) {
                    pb_puts(o, "<w:r>");
                    dx_text(o, c.text + j, k - j);
                    pb_puts(o, "</w:r>");
                }

                pb_puts(o, "</w:p>");
                j = k + 1;
            }
        }

        pb_puts(o, "</w:comment>");
        pb_printf(ex, "<w15:commentEx w15:paraId=\"%08X\"", 0x10000000u + (unsigned)i);

        if (c.parent) {
            pb_printf(ex, " w15:paraIdParent=\"%08X\"", 0x10000000u + (unsigned)c.parent);
        }

        pb_printf(ex, " w15:done=\"%d\"/>", c.resolved ? 1 : 0);
    }

    pb_puts(o, "</w:comments>");
    pb_puts(ex, "</w15:commentsEx>");
    return n;
}

static int dx_span(void* user, const pd_span* sp) {
    dxo* x = (dxo*)user;
    pd_buf* o = x->o;

    dx_marks(x, sp->offset);

    if (sp->is_object) {
        const pd_inline* ob = &sp->obj;

        switch (ob->kind) {
            case PD_INLINE_IMAGE:
                dx_picture(x, ob, NULL);
                break;

            case PD_INLINE_EQUATION:     /* as Word's own math, a display one on its line */
                if (ob->source && ob->source_len > 0) {
                    pd_block_info pi;

                    pd_doc_block_info(x->d, x->para, &pi);
                    pd_latex_to_omml(ob->source, (size_t)ob->source_len, pi.role == PD_ROLE_EQUATION, o);
                }

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

                    case PD_FIELD_REF_PAGE:     /* the page of a place: its bookmark's, or one made for it */
                        if (ob->target) {
                            char bm[48];

                            dx_ref_name(x->d, ob->target, bm, sizeof(bm));
                            pb_puts(o, "<w:fldSimple w:instr=\" PAGEREF ");
                            xesc(o, bm, strlen(bm));
                            pb_puts(o, " \\h \"><w:r><w:t>1</w:t></w:r></w:fldSimple>");
                        }

                        break;

                    case PD_FIELD_DATE:
                        strcpy(v, " ");
                        instr = "DATE";
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
                int slink = x->in_link, sncb = x->ncb, scbi = x->cbi;
                void* scb = malloc(sizeof(x->cb));

                if (scb) {
                    memcpy(scb, x->cb, sizeof(x->cb));
                }

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

                if (scb) {
                    memcpy(x->cb, scb, sizeof(x->cb));
                    free(scb);
                }

                x->ncb = sncb;
                x->cbi = scbi;
                break;
            }

            case PD_INLINE_LINK:
                if (x->in_link) {
                    pb_puts(o, "</w:hyperlink>");
                    x->in_link = 0;
                }

                if (ob->source && ob->source_len > 1 && ob->source[0] == '#') {     /* to a bookmark of the document */
                    pb_puts(o, "<w:hyperlink w:anchor=\"");
                    xesc(o, ob->source + 1, (size_t)ob->source_len - 1);
                    pb_puts(o, "\" w:history=\"1\">");
                    x->in_link = 1;
                } else if (ob->source && ob->source_len > 0 && !x->in_note) {
                    x->nlinks++;
                    pb_printf(&x->rels, "<Relationship Id=\"rIdl%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/"
                              "2006/relationships/hyperlink\" Target=\"", x->nlinks);
                    xesc(&x->rels, ob->source, (size_t)ob->source_len);
                    pb_puts(&x->rels, "\" TargetMode=\"External\"/>");
                    pb_printf(o, "<w:hyperlink r:id=\"rIdl%d\">", x->nlinks);
                    x->in_link = 1;
                }

                break;

            case PD_INLINE_BOOKMARK:    /* a named point: start and end together */
                if (ob->name[0]) {
                    int id = x->nbookmarks++;

                    pb_printf(o, "<w:bookmarkStart w:id=\"%d\" w:name=\"", id);
                    xesc(o, ob->name, strlen(ob->name));
                    pb_printf(o, "\"/><w:bookmarkEnd w:id=\"%d\"/>", id);
                }

                break;

            case PD_INLINE_TAB:
                pb_puts(o, "<w:r><w:tab/></w:r>");
                break;
        }

        return o->err;
    }

    {   /* split where comment ranges start or end */
        uint32_t cur = sp->offset, end = sp->offset + sp->len;

        while (cur < end) {
            uint32_t stop = end;

            dx_marks(x, cur);

            if (x->cbi < x->ncb && x->cb[x->cbi].off < end) {
                stop = x->cb[x->cbi].off;
            }

            dx_run(x, sp, sp->text + (cur - sp->offset), stop - cur);
            cur = stop;
        }
    }

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

/* the bookmark a reference to a paragraph names: the paragraph's own first one, or _RefPd<id>, which the
   paragraph is given when it is written */
static int dx_ref_name(const pd_doc* d, pd_block_id para, char* out, size_t cap) {
    const char* t;
    uint32_t n, k;

    if (pd_doc_para_text(d, para, &t, &n) == PD_OK) {
        for (k = 0; k + 3 <= n; k++) {
            pd_inline o;
            pd_pos at;

            at.block = para;
            at.offset = k;

            if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at, &o) == PD_OK &&
                    o.kind == PD_INLINE_BOOKMARK && o.name[0]) {
                snprintf(out, cap, "%s", o.name);
                return 1;
            }
        }
    }

    snprintf(out, cap, "_RefPd%u", (unsigned)para);
    return 0;
}

/* whether a paragraph is the target of a reference that needs a bookmark made for it */
static int dx_ref_target(const dxo* x, pd_block_id para) {
    int i;

    for (i = 0; i < x->nreft; i++) {
        if (x->reft[i] == para) {
            return 1;
        }
    }

    return 0;
}

static void dx_para(dxo* x, pd_block_id p, const char* extra_ppr) {
    pd_block_info bi;
    pd_para_props dp;
    const char* sid = NULL;
    char id[64];
    int32_t level = 0, clevel;
    int num;
    pd_buf* o = x->o;
    pd_style_id normal = pd_doc_style_find(x->d, "Normal");

    pd_doc_block_info(x->d, p, &bi);
    memset(&dp, 0, sizeof(dp));
    pd_doc_para_props(x->d, p, &dp);

    /* its style: its own, else the one its role has */
    if (x->in_note && bi.role == PD_ROLE_BODY && (!bi.style || bi.style == normal)) {
        sid = "FootnoteText";
    } else if (bi.style && bi.style != normal) {
        dx_sid(x->d, bi.style, id, sizeof(id));
        sid = id;
    } else if (dx_style_id(&bi)) {
        char nm[16];
        pd_style_id rs;

        snprintf(nm, sizeof(nm), "Heading %d", (int)(bi.level >= 1 && bi.level <= 6 ? bi.level : 1));
        rs = pd_doc_style_find(x->d, bi.role == PD_ROLE_HEADING ? nm : bi.role == PD_ROLE_TITLE ? "Title" :
                               bi.role == PD_ROLE_QUOTE ? "Quote" : bi.role == PD_ROLE_CODE ? "Code" : "Caption");

        if (rs) {
            dx_sid(x->d, rs, id, sizeof(id));
            sid = id;
        }
    }

    num = x->in_note ? 0 : dx_num_id(x, p, &level);
    pb_puts(o, "<w:p><w:pPr>");

    if (sid) {
        pb_printf(o, "<w:pStyle w:val=\"%s\"/>", sid);
    }

    dx_ppr_head(o, &dp, dp.mask);

    if (num) {
        pd_list_level lv[9];
        int32_t nlv = 0;

        pb_printf(o, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>", (int)level, num);

        /* Parade adds the paragraph's indent to the level's; Word's w:ind
           replaces the level's, measured from the margin -- and none keeps it */
        if (pd_doc_list_info(x->d, bi.list, &nlv, lv) == PD_OK && level >= 0 && level < nlv && level < 9) {
            if ((dp.mask & PD_PP_INDENT_LEFT) && dp.indent_left != 0) {
                dp.indent_left += lv[level].indent;
            } else {
                dp.mask &= ~PD_PP_INDENT_LEFT;
            }

            if ((dp.mask & PD_PP_INDENT_FIRST) && dp.indent_first == 0) {
                dp.mask &= ~PD_PP_INDENT_FIRST;
            }
        }
    }

    /* a later block of a list item: under its text */
    if (!x->in_note && pd_conv_item_level(x->d, p, &clevel) && !(dp.mask & PD_PP_INDENT_LEFT)) {
        dp.mask |= PD_PP_INDENT_LEFT;
        dp.indent_left = PD_PT(18) * (clevel + 1);
    }

    {
        pd_para_props sp;   /* the style's own, for the stops it has and the paragraph not */

        pd_doc_style_resolve(x->d, bi.style, &sp, NULL);
        dx_ppr_tail(o, &dp, dp.mask, x->hyph_auto, &sp);
    }

    if (extra_ppr) {
        pb_puts(o, extra_ppr);
    }

    pb_puts(o, "</w:pPr>");
    dx_anchors(x);

    if (x->page_break) {
        pb_puts(o, "<w:r><w:br w:type=\"page\"/></w:r>");
        x->page_break = 0;
    }

    if (dx_ref_target(x, p)) {  /* a place references point at */
        int bid = x->nbookmarks++;

        pb_printf(o, "<w:bookmarkStart w:id=\"%d\" w:name=\"_RefPd%u\"/><w:bookmarkEnd w:id=\"%d\"/>", bid, (unsigned)p, bid);
    }

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
    dx_comment_bounds(x, p);
    pd_conv_spans_all(x->d, p, dx_span, x);

    if (x->in_link) {
        pb_puts(x->o, "</w:hyperlink>");
        x->in_link = 0;
    }

    dx_marks(x, UINT32_MAX);
    x->ncb = x->cbi = 0;

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

    if (tp.width_pct > 0) {
        pb_printf(o, "<w:tblW w:w=\"%d\" w:type=\"pct\"/>", (int)tp.width_pct * 5);     /* fiftieths of a percent */
    } else if (tp.width > 0) {
        pb_printf(o, "<w:tblW w:w=\"%d\" w:type=\"dxa\"/>", TW(tp.width));
    } else {
        pb_puts(o, "<w:tblW w:w=\"0\" w:type=\"auto\"/>");
    }

    if (tp.align == PD_ALIGN_CENTER || tp.align == PD_ALIGN_RIGHT) {
        pb_puts(o, tp.align == PD_ALIGN_CENTER ? "<w:jc w:val=\"center\"/>" : "<w:jc w:val=\"right\"/>");
    } else if (tp.indent) {
        pb_printf(o, "<w:tblInd w:w=\"%d\" w:type=\"dxa\"/>", TW(tp.indent));
    }

    if (tp.border) {
        static const char* edge[] = { "top", "left", "bottom", "right", "insideH", "insideV" };
        static const int bit[] = { PD_TBORDER_TOP, PD_TBORDER_LEFT, PD_TBORDER_BOTTOM, PD_TBORDER_RIGHT,
                                   PD_TBORDER_INSIDE_H, PD_TBORDER_INSIDE_V
                                 };
        int sz = (int)SCALE(tp.border, 8, 65536), sides = tp.border_sides ? tp.border_sides : 63;     /* eighths of a point */

        sz = sz < 2 ? 2 : sz;
        pb_puts(o, "<w:tblBorders>");

        for (k = 0; k < 6; k++) {
            if (sides & bit[k]) {
                pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"%06X\"/>", edge[k], sz,
                          (unsigned)(tp.border_color & 0xFFFFFF));
            } else {
                pb_printf(o, "<w:%s w:val=\"nil\"/>", edge[k]);
            }
        }

        pb_puts(o, "</w:tblBorders>");
    }

    /* said outright when Parade sizes the columns: the grid below is then
       only a starting point, and the importer reads it as one */
    if (!fixed) {
        pb_puts(o, "<w:tblLayout w:type=\"autofit\"/>");
    }

    {
        int pv = TW(tp.cell_padding_v >= 0 ? tp.cell_padding_v : tp.cell_padding), ph = TW(tp.cell_padding);

        pb_printf(o, "<w:tblCellMar><w:top w:w=\"%d\" w:type=\"dxa\"/><w:left w:w=\"%d\" w:type=\"dxa\"/>"
                  "<w:bottom w:w=\"%d\" w:type=\"dxa\"/><w:right w:w=\"%d\" w:type=\"dxa\"/></w:tblCellMar>", pv, ph, pv, ph);
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

        {   /* the row's height: the tallest a cell asks for */
            pd_sp minh = 0;

            for (c = 0; c < ri.child_count; c++) {
                pd_cell_props cp;

                pd_doc_cell_props(x->d, pd_doc_child(x->d, row, c), &cp);
                minh = cp.min_height > minh ? cp.min_height : minh;
            }

            if (minh > 0 || r < tp.header_rows) {
                pb_puts(o, "<w:trPr>");

                if (minh > 0) {
                    pb_printf(o, "<w:trHeight w:val=\"%d\"/>", TW(minh));
                }

                if (r < tp.header_rows) {
                    pb_puts(o, "<w:tblHeader/>");
                }

                pb_puts(o, "</w:trPr>");
            }
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

            if (cp.merge_up) {
                pb_puts(o, "<w:vMerge/>");
            } else if (r + 1 < ti.child_count && dx_merges_below(x, pd_doc_child(x->d, t, r + 1), col)) {
                pb_puts(o, "<w:vMerge w:val=\"restart\"/>");
            }

            if (cp.border_set) {
                static const char* edge[] = { "top", "left", "bottom", "right" };
                static const int bit[] = { PD_BORDER_TOP, PD_BORDER_LEFT, PD_BORDER_BOTTOM, PD_BORDER_RIGHT };
                int sz = (int)SCALE(cp.border_width, 8, 65536), e;

                sz = sz < 2 ? 2 : sz;
                pb_puts(o, "<w:tcBorders>");

                for (e = 0; e < 4; e++) {
                    if (cp.border_on & bit[e]) {
                        pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"%06X\"/>", edge[e], sz,
                                  (unsigned)(cp.border_color & 0xFFFFFF));
                    } else if (cp.border_set & bit[e]) {
                        pb_printf(o, "<w:%s w:val=\"nil\"/>", edge[e]);
                    }
                }

                pb_puts(o, "</w:tcBorders>");
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

            if (x->npending > 0) {  /* picture floats at the cell's end: a paragraph to anchor in */
                dx_flush_anchors(x);
                last_para = 1;
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
            dx_flush_anchors(x);
            dx_table(x, id, dx_text_width(x));
            break;

        case PD_BLOCK_FLOAT: {
            pd_inline ob;

            (void)ob;

            if (x->npending < 8) {
                x->pending[x->npending++] = id;     /* anchored in the paragraph after it: a picture or a text box */
                break;
            }

            for (i = 0; i < bi.child_count; i++) {
                dx_block(x, pd_doc_child(x->d, id, i));
            }

            break;
        }

        case PD_BLOCK_BREAK:
            dx_flush_anchors(x);

            if (bi.break_kind == PD_BREAK_RULE) {   /* an empty paragraph ruled underneath */
                pb_puts(x->o, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\" w:sz=\"6\" w:space=\"1\" "
                        "w:color=\"808080\"/></w:pBdr></w:pPr></w:p>");
                break;
            }

            /* in the paragraph that follows, as Word has it: a paragraph of
               its own would leave an empty line at the top of the new page */
            if (bi.break_kind == PD_BREAK_PAGE) {
                pd_block_info ni;

                if (pd_doc_block_info(x->d, pd_doc_child(x->d, bi.parent, bi.index + 1), &ni) == PD_OK &&
                        ni.kind == PD_BLOCK_PARAGRAPH) {
                    x->page_break = 1;
                    break;
                }
            }

            pb_printf(x->o, "<w:p><w:r><w:br w:type=\"%s\"/></w:r></w:p>", bi.break_kind == PD_BREAK_COLUMN ? "column" :
                      "page");
            break;

        default:
            for (i = 0; i < bi.child_count; i++) {
                dx_block(x, pd_doc_child(x->d, id, i));
            }
    }
}

/* a header or footer part for a story; returns its index in hf (part names hf<i>.xml) */
static int dx_story_part(dxo* x, pd_block_id story, int footer) {
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
    x->hf_footer[x->nhf] = footer;
    return x->nhf++;
}

static void dx_sectpr(dxo* x, const pd_section_props* sp, pd_buf* o) {
    static const char* types[3] = { "default", "first", "even" };
    pd_block_id hs[3], fs[3];
    int k, part;

    hs[0] = sp->header;
    hs[1] = sp->title_page ? sp->header_first : 0;
    hs[2] = sp->facing_pages ? sp->header_even : 0;
    fs[0] = sp->footer;
    fs[1] = sp->title_page ? sp->footer_first : 0;
    fs[2] = sp->facing_pages ? sp->footer_even : 0;
    x->even_odd |= sp->facing_pages && (hs[2] || fs[2]);
    pb_puts(o, "<w:sectPr>");

    for (k = 0; k < 3; k++) {   /* headers, then footers, as the schema has them */
        if (hs[k] && (part = dx_story_part(x, hs[k], 0)) >= 0) {
            pb_printf(o, "<w:headerReference w:type=\"%s\" r:id=\"rIdh%d\"/>", types[k], part + 1);
        }
    }

    for (k = 0; k < 3; k++) {
        if (fs[k] && (part = dx_story_part(x, fs[k], 1)) >= 0) {
            pb_printf(o, "<w:footerReference w:type=\"%s\" r:id=\"rIdh%d\"/>", types[k], part + 1);
        }
    }

    if (sp->continuous) {
        pb_puts(o, "<w:type w:val=\"continuous\"/>");
    }

    pb_printf(o, "<w:pgSz w:w=\"%d\" w:h=\"%d\"/>", TW(sp->page_width), TW(sp->page_height));
    pb_printf(o, "<w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" w:header=\"%d\" w:footer=\"%d\" "
              "w:gutter=\"%d\"/>", TW(sp->margin_top), TW(sp->margin_right), TW(sp->margin_bottom), TW(sp->margin_left),
              TW(sp->header_distance), TW(sp->footer_distance), TW(sp->gutter));

    x->mirror |= sp->mirror_margins > 0;

    if (sp->line_numbers > 0) {     /* Word's start is one less than the first number */
        pb_printf(o, "<w:lnNumType w:countBy=\"%d\" w:start=\"%d\" w:distance=\"%d\" w:restart=\"%s\"/>",
                  (int)sp->line_numbers, (int)(sp->line_number_start > 1 ? sp->line_number_start - 1 : 0),
                  TW(sp->line_number_distance > 0 ? sp->line_number_distance : PD_PT(18)),
                  sp->line_number_restart == PD_LINENUM_SECTION ? "newSection" :
                  sp->line_number_restart == PD_LINENUM_CONTINUOUS ? "continuous" : "newPage");
    }

    if (sp->first_page_number > 0 || sp->page_number_format != PD_NUM_DECIMAL) {
        static const char* fmts[] = { "decimal", "decimal", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman",
                                      "decimal"
                                    };

        pb_puts(o, "<w:pgNumType");

        if (sp->page_number_format != PD_NUM_DECIMAL && sp->page_number_format >= 0 && sp->page_number_format <= 6) {
            pb_printf(o, " w:fmt=\"%s\"", fmts[sp->page_number_format]);
        }

        if (sp->first_page_number > 0) {
            pb_printf(o, " w:start=\"%d\"", (int)sp->first_page_number);
        }

        pb_puts(o, "/>");
    }

    if (sp->columns > 1) {
        pb_printf(o, "<w:cols w:num=\"%d\" w:space=\"%d\"/>", (int)sp->columns, TW(sp->column_gap));
    }

    if (sp->page_valign) {
        pb_puts(o, sp->page_valign == 1 ? "<w:vAlign w:val=\"center\"/>" : "<w:vAlign w:val=\"bottom\"/>");
    }

    if (sp->title_page) {
        pb_puts(o, "<w:titlePg/>");
    }

    pb_puts(o, "</w:sectPr>");
}


static int same_ci(const char* a, const char* b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }

    return tolower((unsigned char)*a) == tolower((unsigned char)*b);
}

/* the styleId a Parade style is written under: its name's letters and digits */
static void dx_sid(const pd_doc* d, pd_style_id sid, char* out, size_t cap) {
    const char* n = sid ? pd_doc_style_name(d, sid) : NULL;
    size_t k = 0;

    for (; n && *n && k + 1 < cap; n++) {
        if (isalnum((unsigned char)*n)) {
            out[k++] = *n;
        }
    }

    out[k] = '\0';

    if (n && same_ci(pd_doc_style_name(d, sid), "footnote text")) {
        snprintf(out, cap, "FootnoteText");     /* Word's, read in: written in place of the writer's own */
    } else if (!out[0] || !strcmp(out, "FootnoteText") || !strcmp(out, "FootnoteReference") ||
               !strcmp(out, "Hyperlink") || !strcmp(out, "TableGrid")) {
        snprintf(out, cap, "PStyle%u", (unsigned)sid);  /* one of the writer's own, or no name to make one of */
    }
}

/* the w:name Word knows a built-in style by: heading 1 .. heading 9, caption */
static void dx_style_name(pd_buf* o, const char* n) {
    if (!strncmp(n, "Heading ", 8) && n[8] >= '1' && n[8] <= '9' && !n[9]) {
        pb_printf(o, "heading %c", n[8]);
    } else if (!strcmp(n, "Caption")) {
        pb_puts(o, "caption");
    } else {
        xesc(o, n, strlen(n));
    }
}

/* a family for Word: the generic monospace one as a face it has */
static void dx_fonts(pd_buf* o, const char* family) {
    const char* f = !strcmp(family, "monospace") ? "Courier New" : family;

    pb_puts(o, "<w:rFonts w:ascii=\"");
    xesc(o, f, strlen(f));
    pb_puts(o, "\" w:hAnsi=\"");
    xesc(o, f, strlen(f));
    pb_puts(o, "\" w:cs=\"");
    xesc(o, f, strlen(f));
    pb_puts(o, "\"/>");
}

/* the paragraph properties before w:numPr in a w:pPr, of those mask sets */
static void dx_ppr_head(pd_buf* o, const pd_para_props* pp, uint32_t m) {
    if (m & PD_PP_KEEP_NEXT) {
        pb_puts(o, pp->keep_with_next ? "<w:keepNext/>" : "<w:keepNext w:val=\"0\"/>");
    }

    if (m & PD_PP_KEEP_LINES) {
        pb_puts(o, pp->keep_lines ? "<w:keepLines/>" : "<w:keepLines w:val=\"0\"/>");
    }

    if (m & PD_PP_BREAK_BEFORE) {
        pb_puts(o, pp->page_break_before ? "<w:pageBreakBefore/>" : "<w:pageBreakBefore w:val=\"0\"/>");
    }

    if (m & (PD_PP_WIDOWS | PD_PP_ORPHANS)) {
        pb_puts(o, pp->widows > 1 || pp->orphans > 1 ? "<w:widowControl/>" : "<w:widowControl w:val=\"0\"/>");
    }
}

/* ... and those after it, in the schema's order; hyph_auto: the document hyphenates (settings.xml) */
static void dx_ppr_tail(pd_buf* o, const pd_para_props* pp, uint32_t m, int hyph_auto, const pd_para_props* base) {
    if ((m & PD_PP_BORDER) && pp->border_color && pp->border_width > 0) {
        int sz = (int)SCALE(pp->border_width, 8, 65536), side;
        int has = pp->border_sides ? pp->border_sides : PD_BORDER_TOP | PD_BORDER_RIGHT | PD_BORDER_BOTTOM | PD_BORDER_LEFT;
        static const char* names[] = { "top", "left", "bottom", "right", "between" };
        static const int bits[] = { PD_BORDER_TOP, PD_BORDER_LEFT, PD_BORDER_BOTTOM, PD_BORDER_RIGHT, PD_BORDER_BETWEEN };

        sz = sz < 2 ? 2 : sz;
        pb_puts(o, "<w:pBdr>");

        for (side = 0; side < 5; side++) {
            if (has & bits[side]) {
                pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"%d\" w:color=\"%06X\"/>", names[side], sz,
                          side == 4 ? 0 : (int)SCALE(pp->border_space, 1, 65536), (unsigned)(pp->border_color & 0xFFFFFF));
            } else if (base && base->border_color && base->border_width > 0) {
                pb_printf(o, "<w:%s w:val=\"nil\"/>", names[side]);   /* the style's edge taken away */
            }
        }

        pb_puts(o, "</w:pBdr>");
    } else if ((m & PD_PP_BORDER) && base && base->border_color && base->border_width > 0) {
        pb_puts(o, "<w:pBdr><w:top w:val=\"nil\"/><w:left w:val=\"nil\"/><w:bottom w:val=\"nil\"/>"
                "<w:right w:val=\"nil\"/><w:between w:val=\"nil\"/></w:pBdr>");
    }

    if ((m & PD_PP_SHADING) && pp->shading) {
        pb_printf(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>", (unsigned)(pp->shading & 0xFFFFFF));
    } else if ((m & PD_PP_SHADING) && base && base->shading) {
        pb_puts(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"auto\"/>");
    }

    if ((m & PD_PP_TABS) && (pp->ntabs > 0 || (base && base->ntabs > 0))) {
        static const char* al[] = { "left", "center", "right", "decimal" };
        static const char* ld[] = { "none", "dot", "hyphen", "underscore" };
        int32_t k;

        pb_puts(o, "<w:tabs>");

        /* Word adds a paragraph's stops to its style's: those of the style's it has not are cleared */
        for (k = 0; base && k < base->ntabs && k < PD_MAX_TABS; k++) {
            int32_t j;

            for (j = 0; j < pp->ntabs && TW(pp->tabs[j].position) != TW(base->tabs[k].position); j++) {
            }

            if (j == pp->ntabs) {
                pb_printf(o, "<w:tab w:val=\"clear\" w:pos=\"%d\"/>", TW(base->tabs[k].position));
            }
        }

        for (k = 0; k < pp->ntabs && k < PD_MAX_TABS; k++) {
            pb_printf(o, "<w:tab w:val=\"%s\" w:leader=\"%s\" w:pos=\"%d\"/>", al[pp->tabs[k].align & 3],
                      ld[pp->tabs[k].leader & 3], TW(pp->tabs[k].position));
        }

        pb_puts(o, "</w:tabs>");
    }

    if ((m & PD_PP_HYPHENATE) && (!pp->hyphenate) != (!hyph_auto)) {
        pb_puts(o, pp->hyphenate ? "<w:suppressAutoHyphens w:val=\"0\"/>" : "<w:suppressAutoHyphens/>");
    }

    if (m & PD_PP_DIRECTION) {
        pb_puts(o, pp->direction == PD_DIR_RTL ? "<w:bidi/>" : "<w:bidi w:val=\"0\"/>");
    }

    if (m & (PD_PP_SPACE_BEFORE | PD_PP_SPACE_AFTER | PD_PP_LINE_SPACING)) {
        pb_puts(o, "<w:spacing");

        if (m & PD_PP_SPACE_BEFORE) {
            pb_printf(o, " w:before=\"%d\"", TW(pp->space_before));
        }

        if (m & PD_PP_SPACE_AFTER) {
            pb_printf(o, " w:after=\"%d\"", TW(pp->space_after));
        }

        if (m & PD_PP_LINE_SPACING) {   /* Parade's multiple of the font's line height is Word's auto rule */
            pb_printf(o, " w:line=\"%d\" w:lineRule=\"auto\"", (int)SCALE(pp->line_spacing, 240, 1000));
        }

        pb_puts(o, "/>");
    }

    if (m & (PD_PP_INDENT_LEFT | PD_PP_INDENT_RIGHT | PD_PP_INDENT_FIRST)) {
        pb_puts(o, "<w:ind");

        if (m & PD_PP_INDENT_LEFT) {
            pb_printf(o, " w:left=\"%d\"", TW(pp->indent_left));
        }

        if (m & PD_PP_INDENT_RIGHT) {
            pb_printf(o, " w:right=\"%d\"", TW(pp->indent_right));
        }

        if (m & PD_PP_INDENT_FIRST) {
            pb_printf(o, pp->indent_first < 0 ? " w:hanging=\"%d\"" : " w:firstLine=\"%d\"",
                      TW(pp->indent_first < 0 ? -pp->indent_first : pp->indent_first));
        }

        pb_puts(o, "/>");
    }

    if (m & PD_PP_CONTEXTUAL) {
        pb_puts(o, pp->contextual ? "<w:contextualSpacing/>" : "<w:contextualSpacing w:val=\"0\"/>");
    }

    if (m & PD_PP_ALIGN) {
        pb_printf(o, "<w:jc w:val=\"%s\"/>", pp->align == PD_ALIGN_JUSTIFY ? "both" : pp->align == PD_ALIGN_CENTER ?
                  "center" : pp->align == PD_ALIGN_RIGHT ? "right" : "left");
    }
}

/* the character properties a style sets, in the schema's order */
static void dx_rpr_set(pd_buf* o, const pd_char_props* c) {
    uint32_t m = c->mask;

    if ((m & PD_CP_FAMILY) && c->family[0]) {
        dx_fonts(o, c->family);
    }

    if (m & PD_CP_WEIGHT) {
        pb_puts(o, c->weight >= 600 ? "<w:b/>" : "<w:b w:val=\"0\"/>");
    }

    if (m & PD_CP_ITALIC) {
        pb_puts(o, c->italic ? "<w:i/>" : "<w:i w:val=\"0\"/>");
    }

    if (m & PD_CP_CAPS) {
        pb_puts(o, c->caps ? "<w:caps/>" : "<w:caps w:val=\"0\"/>");
    }

    if (m & PD_CP_SMALLCAPS) {
        pb_puts(o, c->small_caps ? "<w:smallCaps/>" : "<w:smallCaps w:val=\"0\"/>");
    }

    if (m & PD_CP_STRIKE) {
        pb_puts(o, c->strike ? "<w:strike/>" : "<w:strike w:val=\"0\"/>");
    }

    if (m & PD_CP_HIDDEN) {
        pb_puts(o, c->hidden ? "<w:vanish/>" : "<w:vanish w:val=\"0\"/>");
    }

    if (m & PD_CP_COLOR) {
        pb_printf(o, "<w:color w:val=\"%06X\"/>", (unsigned)(c->color & 0xFFFFFF));
    }

    if (m & PD_CP_LETTERSPACE) {
        pb_printf(o, "<w:spacing w:val=\"%d\"/>", TW(c->letter_space));
    }

    if (m & PD_CP_KERNING) {
        pb_printf(o, "<w:kern w:val=\"%d\"/>", c->kerning ? 2 : 0);
    }

    if (m & PD_CP_POSITION) {
        pb_printf(o, "<w:position w:val=\"%d\"/>", (int)SCALE(c->position, 2, 65536));
    }

    if (m & PD_CP_SIZE) {
        pb_printf(o, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", (int)SCALE(c->size, 2, 65536),
                  (int)SCALE(c->size, 2, 65536));
    }

    if (m & PD_CP_UNDERLINE) {
        pb_printf(o, "<w:u w:val=\"%s\"/>", dx_u_name(c->underline));
    }

    if ((m & PD_CP_BACKGROUND) && c->background) {
        pb_printf(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>", (unsigned)(c->background & 0xFFFFFF));
    }

    if (m & PD_CP_SHIFT) {
        pb_puts(o, c->shift == PD_SHIFT_SUPER ? "<w:vertAlign w:val=\"superscript\"/>" : c->shift == PD_SHIFT_SUB ?
                "<w:vertAlign w:val=\"subscript\"/>" : "<w:vertAlign w:val=\"baseline\"/>");
    }

    if ((m & PD_CP_LANG) && c->lang[0]) {
        pb_puts(o, "<w:lang w:val=\"");
        xesc(o, c->lang, strlen(c->lang));
        pb_puts(o, "\"/>");
    }
}

/* the document's own styles. docDefaults are Parade's defaults, so each style says only what it sets,
   based on its parent, and Word works out what Parade does; then the few the writer uses itself */
static void dx_styles(dxo* x, pd_buf* o) {
    const pd_doc* d = x->d;
    pd_para_props dp;
    pd_char_props dc;
    int32_t i, n = pd_doc_style_count(d);
    pd_style_id normal = pd_doc_style_find(d, "Normal");

    pd_doc_style_resolve(d, 0, &dp, &dc);
    pb_puts(o, XML_DECL);
    pb_printf(o, "<w:styles %s>", W_NS);
    pb_puts(o, "<w:docDefaults><w:rPrDefault><w:rPr>");
    dx_fonts(o, "Times New Roman");     /* what a family-less Parade document is set in: a serif */
    pb_printf(o, "<w:kern w:val=\"%d\"/><w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/><w:lang w:val=\"%s\"/>"
              "</w:rPr></w:rPrDefault>", dc.kerning ? 2 : 0, (int)SCALE(dc.size, 2, 65536), (int)SCALE(dc.size, 2, 65536),
              dc.lang[0] ? dc.lang : "en-US");
    pb_puts(o, "<w:pPrDefault><w:pPr>");
    dx_ppr_head(o, &dp, PD_PP_WIDOWS);
    dx_ppr_tail(o, &dp, PD_PP_SPACE_BEFORE | PD_PP_SPACE_AFTER | PD_PP_LINE_SPACING, x->hyph_auto, NULL);
    pb_puts(o, "</w:pPr></w:pPrDefault></w:docDefaults>");

    for (i = 0; i < n; i++) {
        pd_style_id sid = pd_doc_style_at(d, i), parent = 0;
        int32_t kind = 0;
        pd_para_props pp;
        pd_char_props cp;
        const char* name = pd_doc_style_name(d, sid);
        char id[64], pid[64];

        if (!name || pd_doc_style_info(d, sid, &kind, &parent, &pp, &cp) != PD_OK) {
            continue;
        }

        dx_sid(d, sid, id, sizeof(id));
        pb_printf(o, "<w:style w:type=\"%s\"%s w:styleId=\"%s\"><w:name w:val=\"", kind == PD_STYLE_CHARACTER ?
                  "character" : "paragraph", sid == normal ? " w:default=\"1\"" : "", id);
        dx_style_name(o, name);
        pb_puts(o, "\"/>");

        if (parent) {
            dx_sid(d, parent, pid, sizeof(pid));
            pb_printf(o, "<w:basedOn w:val=\"%s\"/>", pid);
        }

        if ((pp.mask & PD_PP_NEXT_STYLE) && pp.next_style && kind == PD_STYLE_PARAGRAPH) {
            dx_sid(d, pp.next_style, pid, sizeof(pid));
            pb_printf(o, "<w:next w:val=\"%s\"/>", pid);
        }

        pb_puts(o, "<w:qFormat/>");

        if (kind == PD_STYLE_PARAGRAPH && (pp.mask || (!strncmp(name, "Heading ", 8) && name[8] >= '1' &&
                                           name[8] <= '9'))) {
            pb_puts(o, "<w:pPr>");
            dx_ppr_head(o, &pp, pp.mask);
            {
                pd_para_props bp;

                pd_doc_style_resolve(d, parent, &bp, NULL);
                dx_ppr_tail(o, &pp, pp.mask, x->hyph_auto, parent ? &bp : NULL);
            }

            if (!strncmp(name, "Heading ", 8) && name[8] >= '1' && name[8] <= '9' && !name[9]) {
                pb_printf(o, "<w:outlineLvl w:val=\"%c\"/>", name[8] - 1);
            }

            pb_puts(o, "</w:pPr>");
        }

        if (cp.mask) {
            pb_puts(o, "<w:rPr>");
            dx_rpr_set(o, &cp);
            pb_puts(o, "</w:rPr>");
        }

        pb_puts(o, "</w:style>");
    }

    for (i = 0; i < n && !same_ci(pd_doc_style_name(d, pd_doc_style_at(d, i)), "footnote text"); i++) {
    }

    if (i == n) {
        pb_puts(o, "<w:style w:type=\"paragraph\" w:styleId=\"FootnoteText\"><w:name w:val=\"footnote text\"/>"
                "<w:basedOn w:val=\"Normal\"/><w:pPr><w:spacing w:after=\"0\"/></w:pPr><w:rPr><w:sz w:val=\"18\"/>"
                "</w:rPr></w:style>");
    }

    pb_puts(o, "<w:style w:type=\"character\" w:styleId=\"FootnoteReference\"><w:name w:val=\"footnote reference\"/>"
            "<w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr></w:style>");
    pb_puts(o, "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/>"
            "<w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>");
    pb_puts(o, "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:tblPr>"
            "<w:tblCellMar><w:left w:w=\"108\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/></w:tblCellMar>"
            "</w:tblPr></w:style>");
    pb_puts(o, "</w:styles>");
}

/* one abstract numbering per list, from its own levels; one instance of it
   per list, so each counts on its own */
static void dx_numbering(dxo* x, pd_buf* o) {
    static const char* fmts[] = { "bullet", "decimal", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman",
                                  "none"
                                };
    pd_list_level lv[9];
    int32_t nlv;
    int i, k;

    pb_puts(o, XML_DECL);
    pb_printf(o, "<w:numbering %s>", W_NS);

    for (i = 0; i < x->nlistmap; i++) {
        memset(lv, 0, sizeof(lv));

        if (pd_doc_list_info(x->d, x->listmap[i], &nlv, lv) != PD_OK) {
            nlv = 0;
        }

        pb_printf(o, "<w:abstractNum w:abstractNumId=\"%d\"><w:multiLevelType w:val=\"hybridMultilevel\"/>", i);

        for (k = 0; k < 9; k++) {
            pd_list_level L = lv[k < nlv ? k : nlv > 0 ? nlv - 1 : 0];
            int f = L.format >= PD_NUM_BULLET && L.format <= PD_NUM_NONE ? L.format : PD_NUM_DECIMAL;

            if (k >= nlv) {     /* levels the list lacks: like its deepest, further in */
                L.indent += PD_PT(18) * (k - (nlv > 0 ? nlv - 1 : 0));

                if (f != PD_NUM_BULLET && f != PD_NUM_NONE) {
                    snprintf(L.text, sizeof(L.text), "%%%d.", k + 1);
                }
            }

            pb_printf(o, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"%d\"/><w:numFmt w:val=\"%s\"/>", k,
                      (int)(L.start >= 0 ? L.start : 1), fmts[f]);

            if (L.restart_after) {
                pb_printf(o, "<w:lvlRestart w:val=\"%d\"/>", L.restart_after < 0 ? 0 : (int)L.restart_after);
            }

            pb_puts(o, "<w:lvlText w:val=\"");
            xesc(o, L.text, strlen(L.text));
            pb_printf(o, "\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%d\" w:hanging=\"%d\"/></w:pPr>",
                      TW(L.indent), TW(L.hanging));

            if (L.label_family[0] || L.label_size || L.label_weight || L.label_italic || L.label_color) {
                pd_char_props lc;

                memset(&lc, 0, sizeof(lc));
                lc.mask = (L.label_family[0] ? PD_CP_FAMILY : 0) | (L.label_size ? PD_CP_SIZE : 0) |
                          (L.label_weight ? PD_CP_WEIGHT : 0) | (L.label_italic ? PD_CP_ITALIC : 0) |
                          (L.label_color ? PD_CP_COLOR : 0);
                snprintf(lc.family, sizeof(lc.family), "%s", L.label_family);
                lc.size = L.label_size;
                lc.weight = L.label_weight;
                lc.italic = L.label_italic > 0;
                lc.color = L.label_color;
                pb_puts(o, "<w:rPr>");
                dx_rpr_set(o, &lc);
                pb_puts(o, "</w:rPr>");
            }

            pb_puts(o, "</w:lvl>");
        }

        pb_puts(o, "</w:abstractNum>");
    }

    for (i = 0; i < x->nlistmap; i++) {
        pb_printf(o, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/></w:num>", i + 1, i);
    }

    pb_puts(o, "</w:numbering>");
}

/* the core properties Word shows (title, author, ...) from the document's metadata, YAML lines of key: value */
static const struct {
    const char* key;            /* the metadata's */
    const char* el;             /* core.xml's */
} CORE_PROPS[] = {
    { "title", "dc:title" }, { "author", "dc:creator" }, { "subject", "dc:subject" }, { "keywords", "cp:keywords" },
    { "description", "dc:description" }, { "lastModifiedBy", "cp:lastModifiedBy" }, { "created", "dcterms:created" },
    { "modified", "dcterms:modified" }
};

/* the value of a top-level key in YAML text: the rest of its line, quotes taken off */
static int yaml_value(const char* y, size_t n, const char* key, char* out, size_t cap) {
    size_t i = 0, kl = strlen(key);

    while (i < n) {
        size_t e = i;

        while (e < n && y[e] != '\n') {
            e++;
        }

        if (e - i > kl && !strncmp(y + i, key, kl) && y[i + kl] == ':') {
            size_t a = i + kl + 1, b = e, k;

            while (a < b && (y[a] == ' ' || y[a] == '\t')) {
                a++;
            }

            while (b > a && (y[b - 1] == ' ' || y[b - 1] == '\r')) {
                b--;
            }

            if (b - a >= 2 && (y[a] == '"' || y[a] == '\'') && y[b - 1] == y[a]) {
                int dq = y[a] == '"';   /* in double quotes, \" and \\ stand for themselves */

                a++;
                b--;

                for (k = 0; a < b && k + 1 < cap; a++) {
                    if (dq && y[a] == '\\' && a + 1 < b) {
                        a++;
                    }

                    out[k++] = y[a];
                }

                out[k] = '\0';
                return k > 0;
            }

            k = b - a < cap - 1 ? b - a : cap - 1;
            memcpy(out, y + a, k);
            out[k] = '\0';
            return k > 0;
        }

        i = e + 1;
    }

    return 0;
}

static void dx_core(const pd_doc* d, pd_buf* o) {
    size_t n = 0, k;
    const char* y = pd_doc_metadata(d, &n);
    char v[1024];

    pb_puts(o, XML_DECL);
    pb_puts(o, "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">");

    for (k = 0; y && k < sizeof(CORE_PROPS) / sizeof(CORE_PROPS[0]); k++) {
        if (yaml_value(y, n, CORE_PROPS[k].key, v, sizeof(v))) {
            int date = CORE_PROPS[k].el[0] == 'd' && CORE_PROPS[k].el[2] == 't';

            pb_printf(o, "<%s%s>", CORE_PROPS[k].el, date ? " xsi:type=\"dcterms:W3CDTF\"" : "");
            xesc(o, v, strlen(v));
            pb_printf(o, "</%s>", CORE_PROPS[k].el);
        }
    }

    pb_puts(o, "</cp:coreProperties>");
}

pd_status pd_docx_export(const pd_doc* d, pd_buf* out) {
    dxo* x = (dxo*)calloc(1, sizeof(dxo));
    pd_buf doc, part, cxml, cext;
    zipw z;
    pd_block_info ri;
    int32_t s, i, ncomments;

    if (!x) {
        return PD_ERR_NOMEM;
    }

    memset(&doc, 0, sizeof(doc));
    memset(&part, 0, sizeof(part));
    memset(&cxml, 0, sizeof(cxml));
    memset(&cext, 0, sizeof(cext));
    memset(&z, 0, sizeof(z));
    x->d = d;
    pd_numbers_init(&x->nb, d);

    {   /* the paragraphs page references point at that have no bookmark of their own */
        pd_block_id p;

        for (p = pd_doc_next_paragraph(d, 0); p; p = pd_doc_next_paragraph(d, p)) {
            const char* t;
            uint32_t n, k;

            pd_doc_para_text(d, p, &t, &n);

            for (k = 0; k + 3 <= n; k++) {
                pd_inline o;
                pd_pos at;
                char bm[48];

                at.block = p;
                at.offset = k;

                if (!memcmp(t + k, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at, &o) == PD_OK && o.kind == PD_INLINE_FIELD &&
                        o.field == PD_FIELD_REF_PAGE && o.target && !dx_ref_name(d, o.target, bm, sizeof(bm)) &&
                        !dx_ref_target(x, o.target) && x->nreft < 1024) {
                    x->reft[x->nreft++] = o.target;
                }
            }
        }
    }

    {   /* the document hyphenates when its Normal does */
        pd_para_props np;

        pd_doc_style_resolve(d, pd_doc_style_find(d, "Normal"), &np, NULL);
        x->hyph_auto = np.hyphenate;
    }

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
                    pb_printf(&doc, "<w:p><w:pPr>%s</w:pPr>", sect.p ? sect.p : "");
                    dx_anchors(x);
                    pb_puts(&doc, "</w:p>");
                }

                pb_free(&sect);
            } else {
                dx_block(x, k);
            }
        }

        if (s + 1 == ri.child_count) {
            dx_flush_anchors(x);
            dx_sectpr(x, &sp, &doc);
        }
    }

    pb_puts(&doc, "</w:body></w:document>");
    ncomments = dx_comments(d, &cxml, &cext);

    /* the package */
    z.o = out;
    pb_puts(&part, XML_DECL);
    pb_puts(&part, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Default Extension=\"png\" ContentType=\"image/png\"/>"
            "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
            "<Default Extension=\"gif\" ContentType=\"image/gif\"/>"
            "<Default Extension=\"emf\" ContentType=\"image/x-emf\"/>"
            "<Default Extension=\"wmf\" ContentType=\"image/x-wmf\"/>"
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
                  "officedocument.wordprocessingml.%s+xml\"/>", (int)i + 1, x->hf_footer[i] ? "footer" : "header");
    }

    pb_puts(&part, "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package."
            "core-properties+xml\"/>");

    if (ncomments) {
        pb_puts(&part, "<Override PartName=\"/word/comments.xml\" ContentType=\"application/vnd.openxmlformats-"
                "officedocument.wordprocessingml.comments+xml\"/><Override PartName=\"/word/commentsExtended.xml\" "
                "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.commentsExtended+xml\"/>");
    }

    pb_puts(&part, "</Types>");
    zip_add(&z, "[Content_Types].xml", part.p, part.n);
    part.n = 0;
    pb_puts(&part, XML_DECL);
    pb_puts(&part, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "officeDocument\" Target=\"word/document.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
            "core-properties\" Target=\"docProps/core.xml\"/></Relationships>");
    zip_add(&z, "_rels/.rels", part.p, part.n);
    part.n = 0;
    dx_core(d, &part);
    zip_add(&z, "docProps/core.xml", part.p, part.n);
    zip_add(&z, "word/document.xml", doc.p, doc.n);

    part.n = 0;
    dx_styles(x, &part);
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

    if (ncomments) {
        zip_add(&z, "word/comments.xml", cxml.p, cxml.n);
        zip_add(&z, "word/commentsExtended.xml", cext.p, cext.n);
    }

    part.n = 0;
    pb_puts(&part, XML_DECL);
    pb_printf(&part, "<w:settings %s>", W_NS);

    {   /* in the schema's order: hyphenation, the default tab, even pages, notes, compatibility */
        pd_para_props np;

        pd_doc_style_resolve(d, pd_doc_style_find(d, "Normal"), &np, NULL);

        if (x->mirror) {
            pb_puts(&part, "<w:mirrorMargins/>");
        }

        pb_printf(&part, "<w:defaultTabStop w:val=\"%d\"/>", TW(np.tab_interval > 0 ? np.tab_interval : PD_PT(36)));

        if (x->hyph_auto) {
            pb_puts(&part, "<w:autoHyphenation/>");
        }
    }

    if (x->even_odd) {
        pb_puts(&part, "<w:evenAndOddHeaders/>");
    }

    pb_puts(&part, "<w:footnotePr><w:footnote w:id=\"-1\"/><w:footnote w:id=\"0\"/></w:footnotePr>"
            "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" "
            "w:val=\"15\"/></w:compat></w:settings>");
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
        pb_printf(&part, x->hf_footer[i] ? "<w:ftr %s>" : "<w:hdr %s>", W_NS);
        x->o = &part;
        memset(&x->rels, 0, sizeof(x->rels));
        pd_doc_block_info(d, x->hf[i], &hi);

        for (k = 0; k < hi.child_count; k++) {
            dx_block(x, pd_doc_child(d, x->hf[i], k));
        }

        pb_puts(&part, x->hf_footer[i] ? "</w:ftr>" : "</w:hdr>");
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

    if (ncomments) {
        pb_puts(&part, "<Relationship Id=\"rIdCm\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                "relationships/comments\" Target=\"comments.xml\"/><Relationship Id=\"rIdCx\" Type=\"http://schemas."
                "microsoft.com/office/2011/relationships/commentsExtended\" Target=\"commentsExtended.xml\"/>");
    }

    for (i = 0; i < x->nhf; i++) {
        pb_printf(&part, "<Relationship Id=\"rIdh%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                  "relationships/%s\" Target=\"hf%d.xml\"/>", (int)i + 1, x->hf_footer[i] ? "footer" : "header",
                  (int)i + 1);
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
    i = doc.err || part.err || x->rels.err || x->notes.err || x->endnotes.err || cxml.err || cext.err;
    pb_free(&cxml);
    pb_free(&cext);
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

/* rules on the edges of a table or a cell: PD_TBORDER_* bits (top, right,
   bottom, left, inside H, inside V) said, and of those ruled */
typedef struct {
    int set, on;
    pd_sp w;
    uint32_t c;
} dedges;

/* one part of a table style: the whole table, or a condition of it */
typedef struct {
    int given;
    dprops pr;                  /* its w:pPr and w:rPr, for the text of the cells */
    uint32_t shd;               /* cell shading, with has_shd */
    int has_shd;
    dedges cb;                  /* w:tcBorders */
} dtpart;

enum { TP_WHOLE, TP_FIRST_ROW, TP_LAST_ROW, TP_FIRST_COL, TP_LAST_COL, TP_BAND1V, TP_BAND2V, TP_BAND1H, TP_BAND2H,
       TP_N
     };

typedef struct {                /* a table style */
    char id[64], based_on[64];
    int is_default;
    dtpart part[TP_N];
    dedges tb;                  /* w:tblBorders */
    pd_sp mar[4];               /* w:tblCellMar top, right, bottom, left; -1 unsaid */
    pd_sp ind;
    int has_ind;
    int row_band, col_band;
} dtstyle;

typedef struct {
    pd_bld* b;
    zipr z;
    drel* rels;
    int nrels;
    dstyle_x* styles;
    int nstyles;
    dtstyle* tstyles;
    int ntstyles;
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
    int mirror;                 /* settings.xml: margins mirror on even pages */
    char hf_rid[16][64];        /* header/footer parts already read, and their stories */
    pd_block_id hf_story[16];
    int nhf;
    pd_block_id hf_last[6];     /* the previous section's: header default/first/even, footer the same */
    char def_pstyle[64];        /* the paragraph style of a paragraph that names none */
    char theme_major[64], theme_minor[64];  /* the theme's heading and body fonts */
    pd_sp margin_left;          /* the section's, for pictures placed from the page's edge */
    struct {                    /* bookmarks read: where they are */
        char name[32];
        pd_block_id para;
    } bm[4096];
    int nbm;
    struct {                    /* fields that point at a bookmark, put right once all are read */
        char name[32];
        pd_block_id para;
        uint32_t off;
        int field;
    } refs[2048];
    int nrefs;
    struct dcmt {               /* comments' ranges in the text, by w:id */
        int wid;
        pd_marker_id m0, m1;
        int pending;            /* started between paragraphs: at the next one's start */
    }* cm;
    int ncm, capcm;
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
        cp->underline = !attr_on(m) ? 0 : !mu_attr(m, "w:val", v, sizeof(v)) ? PD_UNDERLINE_SINGLE :
                        strcmp(v, "none") == 0 ? 0 : strcmp(v, "double") == 0 || strcmp(v, "wavyDouble") == 0 ?
                        PD_UNDERLINE_DOUBLE : strcmp(v, "thick") == 0 ? PD_UNDERLINE_THICK : strcmp(v, "words") == 0 ?
                        PD_UNDERLINE_WORDS : strncmp(v, "wav", 3) == 0 ? PD_UNDERLINE_WAVY : strncmp(v, "dotted", 6) == 0 ?
                        PD_UNDERLINE_DOTTED : strncmp(v, "dash", 4) == 0 || strncmp(v, "dot", 3) == 0 ?
                        PD_UNDERLINE_DASHED : PD_UNDERLINE_SINGLE;
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
    } else if (strcmp(t, "caps") == 0) {
        cp->mask |= PD_CP_CAPS;
        cp->caps = attr_on(m);
    } else if (strcmp(t, "vanish") == 0) {
        cp->mask |= PD_CP_HIDDEN;
        cp->hidden = attr_on(m);
    } else if (strcmp(t, "spacing") == 0) {         /* between letters, in twips */
        cp->mask |= PD_CP_LETTERSPACE;
        cp->letter_space = twips(attr_int(m, "w:val", 0));
    } else if (strcmp(t, "position") == 0) {        /* raised or lowered, in half-points */
        cp->mask |= PD_CP_POSITION;
        cp->position = (pd_sp)((int64_t)attr_int(m, "w:val", 0) * 65536 / 2);
    } else if (strcmp(t, "kern") == 0) {            /* kerned from a size up: Word's own default is not to */
        cp->mask |= PD_CP_KERNING;
        cp->kerning = attr_int(m, "w:val", 0) > 0;
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
    } else if (strcmp(t, "bidi") == 0) {  /* a right-to-left paragraph */
        pp->mask |= PD_PP_DIRECTION;
        pp->direction = attr_on(m) ? PD_DIR_RTL : PD_DIR_LTR;
    } else if (strcmp(t, "contextualSpacing") == 0) {
        pp->mask |= PD_PP_CONTEXTUAL;
        pp->contextual = attr_on(m);
    } else if (strcmp(t, "keepNext") == 0) {
        pp->mask |= PD_PP_KEEP_NEXT;
        pp->keep_with_next = attr_on(m);
    } else if (strcmp(t, "keepLines") == 0) {
        pp->mask |= PD_PP_KEEP_LINES;
        pp->keep_lines = attr_on(m);
    } else if (strcmp(t, "pageBreakBefore") == 0) {
        pp->mask |= PD_PP_BREAK_BEFORE;
        pp->page_break_before = attr_on(m);
    } else if (strcmp(t, "pBdr") == 0 && m->type == MT_OPEN) {
        pp->mask |= PD_PP_BORDER;   /* the edges that follow say what it has */
        pp->border_color = 0;
        pp->border_width = 0;
        pp->border_sides = 0;
        pp->border_space = 0;
    } else if (strcmp(t, "top") == 0 || strcmp(t, "bottom") == 0 || strcmp(t, "left") == 0 ||
               strcmp(t, "right") == 0 || strcmp(t, "start") == 0 || strcmp(t, "end") == 0 ||
               strcmp(t, "between") == 0) {
        /* an edge of w:pBdr (no other w:pPr child has these names) */
        int side = t[0] == 't' ? PD_BORDER_TOP : t[0] == 'b' && t[1] == 'o' ? PD_BORDER_BOTTOM : t[0] == 'b' ?
                   PD_BORDER_BETWEEN : t[0] == 'r' || t[0] == 'e' ? PD_BORDER_RIGHT : PD_BORDER_LEFT;

        if (mu_attr(m, "w:val", v, sizeof(v)) && strcmp(v, "none") != 0 && strcmp(v, "nil") != 0) {
            pd_sp bw = (pd_sp)((int64_t)attr_int(m, "w:sz", 4) * 65536 / 8);

            pp->mask |= PD_PP_BORDER;
            pp->border_sides |= side;
            pp->border_width = bw > pp->border_width ? bw : pp->border_width;
            pp->border_width = pp->border_width > 0 ? pp->border_width : PD_PT(0.25);

            if (!pp->border_color || side != PD_BORDER_BETWEEN) {
                pp->border_color = mu_attr(m, "w:color", v, sizeof(v)) && strcmp(v, "auto") != 0 ?
                                   0xFF000000u | (uint32_t)strtoul(v, NULL, 16) : 0xFF000000u;
            }

            if (side != PD_BORDER_BETWEEN && attr_int(m, "w:space", 0) > 0) {   /* in points */
                pd_sp sp = PD_PT(attr_int(m, "w:space", 0));

                pp->border_space = sp > pp->border_space ? sp : pp->border_space;
            }
        }
    } else if (strcmp(t, "shd") == 0) {
        pp->mask |= PD_PP_SHADING;
        pp->shading = mu_attr(m, "w:fill", v, sizeof(v)) && strcmp(v, "auto") != 0 ?
                      0xFF000000u | (uint32_t)strtoul(v, NULL, 16) : 0;
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
    if (sp->mask & PD_PP_CONTEXTUAL) dp->contextual = sp->contextual;
    if (sp->mask & PD_PP_DIRECTION) dp->direction = sp->direction;
    if (sp->mask & PD_PP_SHADING) dp->shading = sp->shading;

    if (sp->mask & PD_PP_BORDER) {
        dp->border_color = sp->border_color;
        dp->border_width = sp->border_width;
        dp->border_sides = sp->border_sides;
        dp->border_space = sp->border_space;
    }

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
    if (sc->mask & PD_CP_CAPS) dc->caps = sc->caps;
    if (sc->mask & PD_CP_HIDDEN) dc->hidden = sc->hidden;
    if (sc->mask & PD_CP_LETTERSPACE) dc->letter_space = sc->letter_space;
    if (sc->mask & PD_CP_POSITION) dc->position = sc->position;
    if (sc->mask & PD_CP_KERNING) dc->kerning = sc->kerning;

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

/* the edges of a w:tblBorders or w:tcBorders: one child into e */
static void edge_elem(const pd_markup* m, const char* t, dedges* e) {
    char v[32];
    int bit = !strcmp(t, "top") ? PD_TBORDER_TOP : !strcmp(t, "bottom") ? PD_TBORDER_BOTTOM :
              !strcmp(t, "left") || !strcmp(t, "start") ? PD_TBORDER_LEFT : !strcmp(t, "right") ||
              !strcmp(t, "end") ? PD_TBORDER_RIGHT : !strcmp(t, "insideH") ? PD_TBORDER_INSIDE_H :
              !strcmp(t, "insideV") ? PD_TBORDER_INSIDE_V : 0;

    if (!bit) {
        return;
    }

    e->set |= bit;

    if (mu_attr(m, "w:val", v, sizeof(v)) && strcmp(v, "nil") != 0 && strcmp(v, "none") != 0) {
        pd_sp bw = (pd_sp)((int64_t)attr_int(m, "w:sz", 4) * 65536 / 8);

        e->on |= bit;
        e->w = bw > e->w ? bw : e->w;
        e->w = e->w > 0 ? e->w : PD_PT(0.25);   /* w:sz 0: the thinnest, 2/8 pt */

        if (mu_attr(m, "w:color", v, sizeof(v)) && strcmp(v, "auto") != 0) {
            e->c = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
        }
    } else {
        e->on &= ~bit;
    }
}

/* b's edges over a's: those b says replace a's */
static void edges_over(dedges* a, const dedges* b) {
    a->on = (a->on & ~b->set) | (b->on & b->set);
    a->set |= b->set;

    if (b->on) {
        a->w = b->w;
        a->c = b->c ? b->c : a->c;
    }
}

static void part_over(dtpart* a, const dtpart* b) {
    if (!b->given) {
        return;
    }

    a->given = 1;
    pr_over(&a->pr, &b->pr);

    if (b->has_shd) {
        a->has_shd = 1;
        a->shd = b->shd;
    }

    edges_over(&a->cb, &b->cb);
}

/* the table styles of styles.xml: borders, cell margins and the parts
   (whole table, header row, banding...) with what they do to the cells */
static void read_table_styles(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    int cap = 0, cur = -1, part = TP_WHOLE, in_ppr = 0, in_rpr = 0, in_tblpr = 0, in_tcpr = 0, in_tb = 0, in_cb = 0;
    int in_mar = 0, k;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);
        int open = m.type == MT_OPEN || m.type == MT_EMPTY;
        dtstyle* ts = cur >= 0 ? &X->tstyles[cur] : NULL;
        dtpart* tp = ts ? &ts->part[part] : NULL;
        char v[64];

        if (m.type == MT_OPEN && strcmp(t, "style") == 0) {
            if (mu_attr(&m, "w:type", v, sizeof(v)) && strcmp(v, "table") == 0 &&
                    !pd_grow((void**)&X->tstyles, &cap, (int64_t)X->ntstyles + 1, sizeof(dtstyle))) {
                cur = X->ntstyles++;
                ts = &X->tstyles[cur];
                memset(ts, 0, sizeof(*ts));
                mu_attr(&m, "w:styleId", ts->id, sizeof(ts->id));
                ts->is_default = mu_attr(&m, "w:default", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));

                for (k = 0; k < 4; k++) {
                    ts->mar[k] = -1;
                }

                ts->part[TP_WHOLE].given = 1;
                part = TP_WHOLE;
            }

            continue;
        }

        if (m.type == MT_CLOSE && strcmp(t, "style") == 0) {
            cur = -1;
            continue;
        }

        if (!ts) {
            continue;
        }

        if (strcmp(t, "tblStylePr") == 0) {
            if (m.type == MT_OPEN) {
                static const char* names[TP_N] = { "wholeTable", "firstRow", "lastRow", "firstCol", "lastCol",
                                                   "band1Vert", "band2Vert", "band1Horz", "band2Horz"
                                                 };

                mu_attr(&m, "w:type", v, sizeof(v));

                for (part = TP_N - 1; part > 0 && strcmp(names[part], v) != 0; part--) {
                }

                ts->part[part].given = 1;
            } else if (m.type == MT_CLOSE) {
                part = TP_WHOLE;
            }
        } else if (strcmp(t, "pPr") == 0) {
            in_ppr = m.type == MT_OPEN;
        } else if (strcmp(t, "rPr") == 0) {
            in_rpr = m.type == MT_OPEN;
        } else if (strcmp(t, "tblPr") == 0) {
            in_tblpr = m.type == MT_OPEN;
        } else if (strcmp(t, "tcPr") == 0) {
            in_tcpr = m.type == MT_OPEN;
        } else if (strcmp(t, "tblBorders") == 0) {
            in_tb = m.type == MT_OPEN;
        } else if (strcmp(t, "tcBorders") == 0) {
            in_cb = m.type == MT_OPEN;
        } else if (strcmp(t, "tblCellMar") == 0) {
            in_mar = m.type == MT_OPEN;
        } else if (!open) {
            continue;
        } else if (in_ppr && !in_rpr) {
            ppr_elem(&m, t, &tp->pr);
        } else if (in_rpr && !in_ppr) {
            rpr_elem(X, &m, t, &tp->pr.cp, NULL, 0);
        } else if (in_tb && part == TP_WHOLE) {
            edge_elem(&m, t, &ts->tb);
        } else if (in_cb) {
            edge_elem(&m, t, &tp->cb);
        } else if (in_mar && part == TP_WHOLE) {
            int side = !strcmp(t, "top") ? 0 : !strcmp(t, "right") || !strcmp(t, "end") ? 1 : !strcmp(t, "bottom") ? 2 :
                       !strcmp(t, "left") || !strcmp(t, "start") ? 3 : -1;

            if (side >= 0) {
                ts->mar[side] = twips(attr_int(&m, "w:w", 0));
            }
        } else if (in_tblpr && part == TP_WHOLE && strcmp(t, "tblInd") == 0) {
            ts->ind = twips(attr_int(&m, "w:w", 0));
            ts->has_ind = 1;
        } else if (in_tblpr && strcmp(t, "tblStyleRowBandSize") == 0) {
            ts->row_band = attr_int(&m, "w:val", 1);
        } else if (in_tblpr && strcmp(t, "tblStyleColBandSize") == 0) {
            ts->col_band = attr_int(&m, "w:val", 1);
        } else if (in_tcpr && strcmp(t, "shd") == 0) {
            tp->has_shd = 1;
            tp->shd = mu_attr(&m, "w:fill", v, sizeof(v)) && strcmp(v, "auto") != 0 ?
                      0xFF000000u | (uint32_t)strtoul(v, NULL, 16) : 0;
        } else if (strcmp(t, "basedOn") == 0) {
            mu_attr(&m, "w:val", ts->based_on, sizeof(ts->based_on));
        }
    }
}

/* a table style with what it is based on folded in (the farthest first) */
static void table_style_resolve(const dxi* X, const char* id, dtstyle* out, int depth) {
    const dtstyle* ts = NULL;
    int i, k;

    for (i = 0; i < X->ntstyles; i++) {
        if ((id && id[0]) ? strcmp(X->tstyles[i].id, id) == 0 : X->tstyles[i].is_default) {
            ts = &X->tstyles[i];
            break;
        }
    }

    if (!ts || depth > 10) {
        return;
    }

    if (ts->based_on[0]) {
        table_style_resolve(X, ts->based_on, out, depth + 1);
    }

    for (k = 0; k < TP_N; k++) {
        part_over(&out->part[k], &ts->part[k]);
    }

    edges_over(&out->tb, &ts->tb);

    for (k = 0; k < 4; k++) {
        out->mar[k] = ts->mar[k] >= 0 ? ts->mar[k] : out->mar[k];
    }

    if (ts->has_ind) {
        out->ind = ts->ind;
        out->has_ind = 1;
    }

    out->row_band = ts->row_band > 0 ? ts->row_band : out->row_band;
    out->col_band = ts->col_band > 0 ? ts->col_band : out->col_band;
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
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "mirrorMargins") == 0) {
            X->mirror = attr_on(&m);
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
    int cur_abs = -1, cur_lvl = -1, cur_num = -1, ovr_lvl = -1, i, in_rpr = 0;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);
        pd_list_level* L = cur_abs >= 0 && cur_lvl >= 0 && cur_lvl < 9 ? &X->abss[cur_abs].lv[cur_lvl] : NULL;
        char v[64] = "";

        if (strcmp(t, "rPr") == 0) {
            in_rpr = m.type == MT_OPEN;
            continue;
        }

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
        } else if (L && in_rpr) {     /* the label's own font */
            pd_char_props lc;

            memset(&lc, 0, sizeof(lc));
            rpr_elem(X, &m, t, &lc, NULL, 0);

            if ((lc.mask & PD_CP_FAMILY) && L->format != PD_NUM_BULLET) {   /* a bullet's symbol font: read as Unicode */
                snprintf(L->label_family, sizeof(L->label_family), "%.31s", lc.family);
            }

            if (lc.mask & PD_CP_SIZE) {
                L->label_size = lc.size;
            }

            if (lc.mask & PD_CP_WEIGHT) {
                L->label_weight = lc.weight;
            }

            if (lc.mask & PD_CP_ITALIC) {
                L->label_italic = lc.italic ? 1 : -1;
            }

            if (lc.mask & PD_CP_COLOR) {
                L->label_color = lc.color;
            }
        } else if (L && strcmp(t, "lvlRestart") == 0) {
            L->restart_after = attr_int(&m, "w:val", 0) <= 0 ? -1 : attr_int(&m, "w:val", 0);
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

typedef struct {               /* a table being read */
    char style[64];
    dtstyle st;                 /* its style, resolved */
    int look;                   /* w:tblLook: which of the style's parts apply */
    int row, col;               /* the row under way, the grid column of the cell under way */
    dedges tb;                  /* its own w:tblBorders */
    pd_sp mar[4];               /* its own w:tblCellMar, -1 unsaid */
    pd_sp ind, width_pct;
    int has_ind;
    pd_sp row_h;                /* w:trHeight of the row under way */
    int cspan;                  /* the grid columns the cell under way takes */
    dtpart cell;                /* what the style does to the cell under way */
} dtab;

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
    int posh_page, posh_has_off;    /* its offset is from the page's edge; it has one */
    pd_res_id drawing_res;      /* a group or canvas made into a drawing resource */
    int in_vml;                 /* inside w:object or w:pict: a VML picture */
    int math_para;              /* the paragraph opens with display math (m:oMathPara) */
    const char* tbx;            /* a floating text box's content, in the part being read */
    size_t tbn;
    long long posh_off, dist;
    struct {
        pd_res_id res;
        pd_sp w, h, gap, off_x;
        int wrap, has_off;
        const char* tbx;        /* a text box: its w:txbxContent, read into the float */
        size_t tbn;
    } pend_fl[8];               /* floats anchored in a paragraph already under way: after it */
    int npend_fl;
    /* tables */
    dtab tabs[8];               /* each table being read, the innermost last */
    int ntab;
    dedges cell_edges;          /* the cell's own w:tcBorders */
    int pend_row, row_header, pend_cell, span, cell_merge, cell_valign;
    uint32_t cell_bg;
    int in_tblpr, in_grid, in_borders, in_mar, in_cb;
    pd_sp t_width, grid[PD_TABLE_MAX_COLS];
    int t_jc, ngrid, t_autofit;
    int skip;                   /* depth inside an ignored element */
    int note;                   /* parsing a footnote body */
    int after_ref;              /* just after the note's own number: drop the space that follows it */
    pd_rev_id rev;              /* inside w:ins or w:del: the tracked change the text is */
} dw;

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
    DW_DIFF(PD_CP_CAPS, caps)
    DW_DIFF(PD_CP_HIDDEN, hidden)
    DW_DIFF(PD_CP_LETTERSPACE, letter_space)
    DW_DIFF(PD_CP_POSITION, position)
    DW_DIFF(PD_CP_KERNING, kerning)
#undef DW_DIFF

    if (w->rev) {
        cp.mask |= PD_CP_REVISION;
        cp.revision = w->rev;
    }

    bld_set_format(w->X->b, &cp);
}

/* The parts of a table's style that apply to its cell under way -- the
   whole table, the banding of rows and columns, the first column, the
   header row, in that order -- as the tblLook allows. */
static void dw_cell_style(dw* w, dtab* T) {
    const dtstyle* st = &T->st;
    int look = T->look, fr = (look & 0x20) != 0, fc = (look & 0x80) != 0;
    int rb = st->row_band > 0 ? st->row_band : 1, cb = st->col_band > 0 ? st->col_band : 1;
    int r = T->row - fr, c = T->col - fc;

    memset(&T->cell, 0, sizeof(T->cell));
    T->cspan = w->span > 0 ? w->span : 1;
    part_over(&T->cell, &st->part[TP_WHOLE]);

    if (!(look & 0x400) && c >= 0) {
        part_over(&T->cell, &st->part[(c / cb) % 2 ? TP_BAND2V : TP_BAND1V]);
    }

    if (!(look & 0x200) && r >= 0) {
        part_over(&T->cell, &st->part[(r / rb) % 2 ? TP_BAND2H : TP_BAND1H]);
    }

    if (fc && T->col == 0) {
        part_over(&T->cell, &st->part[TP_FIRST_COL]);
    }

    if (fr && T->row == 0) {
        part_over(&T->cell, &st->part[TP_FIRST_ROW]);
    }

    T->cell.given = 1;
}

static void dw_begin_cell(dw* w) {
    pd_bld* b = w->X->b;

    if (w->pend_row) {
        bld_row_begin(b, w->row_header);
        w->pend_row = 0;
    }

    if (w->pend_cell) {
        uint32_t bg = w->cell_bg;
        dedges e;

        memset(&e, 0, sizeof(e));

        if (w->ntab >= 1 && w->ntab <= 8) {     /* what the table's style does to this cell */
            dtpart* c = &w->tabs[w->ntab - 1].cell;

            dw_cell_style(w, &w->tabs[w->ntab - 1]);

            if (!bg && c->has_shd) {
                bg = c->shd;
            }

            e = c->cb;
        }

        edges_over(&e, &w->cell_edges);
        bld_cell_begin(b, w->span, bg);
        w->pend_cell = 0;

        if ((e.set & 15) || (w->ntab >= 1 && w->ntab <= 8 && w->tabs[w->ntab - 1].row_h > 0)) {
            pd_cell_props cp;

            if (pd_doc_cell_props(b->d, bld_container(b), &cp) == PD_OK) {
                cp.border_set = e.set & 15;     /* PD_TBORDER_* outer bits are PD_BORDER_* */
                cp.border_on = e.on & e.set & 15;
                cp.border_width = e.w;
                cp.border_color = e.c ? e.c : 0xFF000000u;
                cp.min_height = w->ntab >= 1 && w->ntab <= 8 ? w->tabs[w->ntab - 1].row_h : 0;
                pd_doc_set_cell_props(b->d, bld_container(b), &cp);
            }
        }

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

    if (w->ntab >= 1 && w->ntab <= 8) {     /* its style, and what it says itself over that */
        dtab* T = &w->tabs[w->ntab - 1];
        dedges e;
        int found = 0;

        memset(&T->st, 0, sizeof(T->st));

        for (k = 0; k < 4; k++) {
            T->st.mar[k] = -1;
        }

        for (k = 0; k < w->X->ntstyles && !found; k++) {
            found = T->style[0] ? strcmp(w->X->tstyles[k].id, T->style) == 0 : w->X->tstyles[k].is_default;
        }

        table_style_resolve(w->X, T->style, &T->st, 0);
        e = T->st.tb;
        edges_over(&e, &T->tb);

        if (!e.set && !found && T->style[0]) {
            tp.border = PD_PT(0.5);     /* a style the document lacks: Word's own grid, w:sz 4 */
            tp.border_sides = 0;
        } else {
            tp.border = e.on ? e.w : 0;
            tp.border_sides = e.on == 63 ? 0 : e.on ? e.on : 0;
            tp.border_color = e.c ? e.c : 0xFF000000u;
        }

        /* cell margins: Word's 5.4pt at the sides and none above and below unless said */
        tp.cell_padding = T->mar[3] >= 0 ? T->mar[3] : T->st.mar[3] >= 0 ? T->st.mar[3] : twips(108);
        tp.cell_padding_v = T->mar[0] >= 0 ? T->mar[0] : T->st.mar[0] >= 0 ? T->st.mar[0] : 0;
        tp.indent = T->has_ind ? T->ind : T->st.has_ind ? T->st.ind : 0;
        tp.width_pct = (int32_t)T->width_pct;

        if (tp.width_pct > 0) {
            tp.width = 0;
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

/* A paragraph style of the document's own, made a Parade style of the same
   name, based on Normal, holding what the style and those it is based on
   say: the paragraph then carries only what it sets itself, and keeps its
   style's name (and contextual spacing knows its neighbours' styles). */
static void dw_custom_style(dw* w, pd_bld* b) {
    const dstyle_x* st = find_style(w->X, w->pstyle);
    const char* name = st && st->name[0] ? st->name : w->pstyle;
    pd_style_id normal = pd_doc_style_find(b->d, "Normal"), sid = pd_doc_style_find(b->d, name);

    if (!st || st->type != 1 || !name[0] || strcmp(name, "Normal") == 0) {
        return;
    }

    if (!sid) {
        dprops sty;
        pd_para_props rp;
        pd_char_props rc;

        memset(&sty, 0, sizeof(sty));
        style_chain(w->X, w->pstyle, &sty, 0);
        pd_doc_style_resolve(b->d, normal, &rp, &rc);
        line_finish(&sty, (sty.cp.mask & PD_CP_SIZE) ? sty.cp.size : rc.size);
        sty.pp.mask &= ~PD_PP_NEXT_STYLE;

        /* only what differs from Normal: the same style read again says the same */
#define SAME_P(bit, f) if ((sty.pp.mask & (bit)) && sty.pp.f == rp.f) { sty.pp.mask &= ~(bit); }
#define SAME_C(bit, f) if ((sty.cp.mask & (bit)) && sty.cp.f == rc.f) { sty.cp.mask &= ~(bit); }
        SAME_P(PD_PP_ALIGN, align)
        SAME_P(PD_PP_INDENT_LEFT, indent_left)
        SAME_P(PD_PP_INDENT_RIGHT, indent_right)
        SAME_P(PD_PP_INDENT_FIRST, indent_first)
        SAME_P(PD_PP_SPACE_BEFORE, space_before)
        SAME_P(PD_PP_SPACE_AFTER, space_after)
        SAME_P(PD_PP_LINE_SPACING, line_spacing)
        SAME_P(PD_PP_KEEP_NEXT, keep_with_next)
        SAME_P(PD_PP_KEEP_LINES, keep_lines)
        SAME_P(PD_PP_BREAK_BEFORE, page_break_before)
        SAME_P(PD_PP_HYPHENATE, hyphenate)
        SAME_P(PD_PP_CONTEXTUAL, contextual)
        SAME_P(PD_PP_SHADING, shading)
        SAME_C(PD_CP_SIZE, size)
        SAME_C(PD_CP_WEIGHT, weight)
        SAME_C(PD_CP_ITALIC, italic)
        SAME_C(PD_CP_COLOR, color)
        SAME_C(PD_CP_BACKGROUND, background)
        SAME_C(PD_CP_UNDERLINE, underline)
        SAME_C(PD_CP_STRIKE, strike)
        SAME_C(PD_CP_SHIFT, shift)
        SAME_C(PD_CP_SMALLCAPS, small_caps)
        SAME_C(PD_CP_CAPS, caps)
        SAME_C(PD_CP_HIDDEN, hidden)
        SAME_C(PD_CP_LETTERSPACE, letter_space)
        SAME_C(PD_CP_POSITION, position)
        SAME_C(PD_CP_KERNING, kerning)
#undef SAME_P
#undef SAME_C

        if ((sty.cp.mask & PD_CP_FAMILY) && same_ci(sty.cp.family, rc.family)) {
            sty.cp.mask &= ~PD_CP_FAMILY;
        }

        if ((sty.cp.mask & PD_CP_LANG) && same_ci(sty.cp.lang, rc.lang)) {
            sty.cp.mask &= ~PD_CP_LANG;
        }

        if ((sty.pp.mask & PD_PP_TABS) && sty.pp.ntabs == rp.ntabs && sty.pp.tab_interval == rp.tab_interval &&
                memcmp(sty.pp.tabs, rp.tabs, (size_t)sty.pp.ntabs * sizeof(pd_tab_stop)) == 0) {
            sty.pp.mask &= ~PD_PP_TABS;
        }

        if ((sty.pp.mask & PD_PP_BORDER) && sty.pp.border_color == rp.border_color &&
                sty.pp.border_width == rp.border_width && sty.pp.border_sides == rp.border_sides &&
                sty.pp.border_space == rp.border_space) {
            sty.pp.mask &= ~PD_PP_BORDER;
        }

        if (pd_doc_style_define(b->d, name, PD_STYLE_PARAGRAPH, normal, &sty.pp, &sty.cp, &sid) != PD_OK) {
            return;
        }
    }

    bld_para_style(b, name, PD_ROLE_BODY, 0);
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
    } else if (w->pstyle[0] && strcmp(w->pstyle, w->X->def_pstyle) != 0) {
        dw_custom_style(w, b);  /* one of the document's own */
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

        {   /* in a table cell, what the table's style says: over Normal's, under any other style's */
            const dtpart* tc = w->ntab >= 1 && w->ntab <= 8 && w->tabs[w->ntab - 1].cell.given ?
                               &w->tabs[w->ntab - 1].cell : NULL;
            int plain = !w->pstyle[0] || strcmp(w->pstyle, w->X->def_pstyle) == 0;

            if (tc && !plain) {
                pr_over(&full, &tc->pr);
            }

            style_chain(w->X, w->pstyle[0] ? w->pstyle : w->X->def_pstyle, &full, 0);

            if (tc && plain) {
                pr_over(&full, &tc->pr);
            }
        }

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
        DW_DIFF(PD_PP_CONTEXTUAL, contextual)
        DW_DIFF(PD_PP_DIRECTION, direction)
        DW_DIFF(PD_PP_SHADING, shading)
#undef DW_DIFF

        if ((f->mask & PD_PP_BORDER) && (f->border_color != rp.border_color || f->border_width != rp.border_width ||
                                         f->border_sides != rp.border_sides || f->border_space != rp.border_space)) {
            b->pp.mask |= PD_PP_BORDER;
            b->pp.border_color = f->border_color;
            b->pp.border_width = f->border_width;
            b->pp.border_sides = f->border_sides;
            b->pp.border_space = f->border_space;
        }

        if ((f->mask & PD_PP_TABS) && (f->ntabs != rp.ntabs || f->tab_interval != rp.tab_interval ||
                                       memcmp(f->tabs, rp.tabs, (size_t)f->ntabs * sizeof(pd_tab_stop)) != 0)) {
            b->pp.mask |= PD_PP_TABS;
            b->pp.ntabs = f->ntabs;
            memcpy(b->pp.tabs, f->tabs, sizeof(b->pp.tabs));
            b->pp.tab_interval = f->tab_interval;
        }
    }

    if (w->math_para) {     /* display math, on a line of its own */
        b->role = PD_ROLE_EQUATION;
        b->level = 0;
    }

    bld_begin_para(b);
    w->started = 1;

    {   /* comment ranges that started between paragraphs start here */
        int ci;

        for (ci = 0; ci < w->X->ncm; ci++) {
            if (w->X->cm[ci].pending && b->para) {
                pd_pos at;

                at.block = b->para;
                at.offset = 0;
                pd_doc_marker_new(b->d, at, PD_GRAVITY_LEFT, &w->X->cm[ci].m0);
                w->X->cm[ci].pending = 0;
            }
        }
    }
}

/* where a comment's range starts (0), ends (1) or its reference mark is (2) */
static void dw_comment_mark(dw* w, int wid, int what) {
    dxi* X = w->X;
    struct dcmt* c = NULL;
    pd_pos at;
    int k;

    for (k = 0; k < X->ncm; k++) {
        if (X->cm[k].wid == wid) {
            c = &X->cm[k];
        }
    }

    if (!c) {
        if (X->ncm >= 4096 || pd_grow((void**)&X->cm, &X->capcm, (int64_t)X->ncm + 1, sizeof(*X->cm))) {
            return;
        }

        c = &X->cm[X->ncm++];
        memset(c, 0, sizeof(*c));
        c->wid = wid;
    }

    if (!w->in_p) {
        c->pending = what == 0 && !c->m0;
        return;
    }

    dw_begin_para(w);
    at = bld_pos(X->b);

    if (!at.block) {
        return;
    }

    if (what == 0 && !c->m0) {
        pd_doc_marker_new(X->b->d, at, PD_GRAVITY_LEFT, &c->m0);
    } else if (what == 1 || (what == 2 && !c->m1)) {
        if (c->m1) {
            pd_doc_marker_free(X->b->d, c->m1);
        }

        pd_doc_marker_new(X->b->d, at, PD_GRAVITY_LEFT, &c->m1);

        if (!c->m0) {   /* only a reference: a point */
            pd_doc_marker_new(X->b->d, at, PD_GRAVITY_LEFT, &c->m0);
        }
    }
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

/* A part's own relationships (word/_rels/<name>.rels) in place of the
   document's while it is read: the pictures and links of a header, a
   footer or the notes are named there. Returns whether it has its own;
   *saved keeps the document's to put back. */
static int part_rels_begin(dxi* X, const char* part, drel** saved, int* saved_n) {
    const char* slash = strrchr(part, '/');
    char path[320];
    char* xml;
    size_t len = 0;

    snprintf(path, sizeof(path), "%.*s_rels/%s.rels", slash ? (int)(slash - part + 1) : 0, part, slash ? slash + 1 : part);

    if ((xml = (char*)zip_read(&X->z, path, &len)) == NULL) {
        return 0;
    }

    *saved = X->rels;
    *saved_n = X->nrels;
    X->rels = NULL;
    X->nrels = 0;
    read_rels(X, xml, len);
    free(xml);
    return 1;
}

static void part_rels_end(dxi* X, int swapped, drel* saved, int saved_n) {
    if (swapped) {
        free(X->rels);
        X->rels = saved;
        X->nrels = saved_n;
    }
}

static void dw_footnote(dw* w, int id, int endnote) {
    dxi* X = w->X;
    const dnote* notes = endnote ? X->en : X->notes;
    const char* xml = endnote ? X->en_xml : X->notes_xml;
    int i, n = endnote ? X->nen : X->nnotes;

    for (i = 0; i < n; i++) {
        if (notes[i].id == id && X->depth < 3) {
            pd_char_props keep = X->b->cp;
            int saved_n = 0, swapped;
            drel* saved = NULL;

            dw_begin_para(w);
            bld_note_begin(X->b, endnote);
            swapped = part_rels_begin(X, endnote ? "word/endnotes.xml" : "word/footnotes.xml", &saved, &saved_n);
            X->depth++;
            dw_parse(X, xml + notes[i].a, notes[i].b - notes[i].a, 1);
            X->depth--;
            part_rels_end(X, swapped, saved, saved_n);
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

            if (w->pend_fl[k].has_off && fp.wrap != PD_WRAP_NONE) {    /* where Word has it across the column */
                fp.placement |= PD_PLACE_OFFSET;
                fp.offset_x = w->pend_fl[k].off_x;
            }

            pd_doc_set_float_props(b->d, fl, &fp);
        }

        if (w->pend_fl[k].tbx) {    /* a text box: what it holds, read here, in the float */
            pd_char_props keep = b->cp;

            w->X->depth++;
            dw_parse(w->X, w->pend_fl[k].tbx, w->pend_fl[k].tbn, 1);
            w->X->depth--;
            bld_end_para(b);
            b->cp = keep;
            bld_float_end(b);
            continue;
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

/* a picture part the relationship id names, as a resource of the document; 0 if none */
static pd_res_id dw_resource(dxi* X, const char* rid) {
    const char* target = rel_target(X, rid, NULL);
    char path[300];
    unsigned char* data;
    size_t len = 0;
    pd_res_id res = 0;

    if (!target) {
        return 0;
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
        int meta = pd_metafile_kind(data, len);
        const char* mime = meta == 1 ? "image/x-emf" : meta == 2 ? "image/x-wmf" :
                           ext && (strcmp(ext, ".png") == 0 || strcmp(ext, ".PNG") == 0) ? "image/png" :
                           ext && (strcmp(ext, ".gif") == 0) ? "image/gif" : ext && (strcmp(ext, ".jpg") == 0 ||
                                   strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".JPG") == 0) ? "image/jpeg" : "application/octet-stream";

        if (pd_doc_add_resource(X->b->d, mime, data, len, &res) != PD_OK) {
            res = 0;
        }

        if (res && meta) {  /* a metafile: drawn from its records, the original kept for writing back */
            pd_res_id dr = pd_metafile_drawing(X->b->d, data, len, res);

            res = dr ? dr : res;
        }

        free(data);
    }

    return res;
}

/* ------------------------------------------------------------------ */
/* drawing canvases and groups                                        */
/* ------------------------------------------------------------------ */

/* Word's theme colours by name, as Office's default theme has them */
static uint32_t scheme_color(const char* name) {
    static const struct {
        const char* n;
        uint32_t c;
    } t[] = { { "accent1", 0x4472C4 }, { "accent2", 0xED7D31 }, { "accent3", 0xA5A5A5 }, { "accent4", 0xFFC000 },
        { "accent5", 0x5B9BD5 }, { "accent6", 0x70AD47 }, { "tx1", 0x000000 }, { "dk1", 0x000000 }, { "bg1", 0xFFFFFF },
        { "lt1", 0xFFFFFF }, { "tx2", 0x44546A }, { "dk2", 0x44546A }, { "bg2", 0xE7E6E6 }, { "lt2", 0xE7E6E6 },
        { "hlink", 0x0563C1 }
    };
    size_t i;

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (strcmp(name, t[i].n) == 0) {
            return 0xFF000000u | t[i].c;
        }
    }

    return 0xFF000000u;
}

typedef struct {                /* child coordinates to the drawing's: x * s + o */
    double ox, oy, sx, sy;
} dxform;

typedef struct {                /* the xfrm being read: offset, extent, and a group's child space */
    long long off[2], ext[2], choff[2], chext[2];
    int rot, fliph, flipv;
} dxfrm_in;

static void xfrm_attr(const pd_markup* m, const char* t, dxfrm_in* f) {
    char v[32];

    if (!strcmp(t, "xfrm")) {
        f->rot = mu_attr(m, "rot", v, sizeof(v)) ? atoi(v) : 0;
        f->fliph = mu_attr(m, "flipH", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));
        f->flipv = mu_attr(m, "flipV", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));
    } else if (!strcmp(t, "off") || !strcmp(t, "chOff")) {
        long long* d = !strcmp(t, "off") ? f->off : f->choff;

        d[0] = mu_attr(m, "x", v, sizeof(v)) ? atoll(v) : 0;
        d[1] = mu_attr(m, "y", v, sizeof(v)) ? atoll(v) : 0;
    } else if (!strcmp(t, "ext") || !strcmp(t, "chExt")) {
        long long* d = !strcmp(t, "ext") ? f->ext : f->chext;

        d[0] = mu_attr(m, "cx", v, sizeof(v)) ? atoll(v) : 0;
        d[1] = mu_attr(m, "cy", v, sizeof(v)) ? atoll(v) : 0;
    }
}

static pd_sp emu_sp(double e) {
    return (pd_sp)(e * 65536.0 / 12700.0);
}

/* a JSON string */
static void json_str(pd_buf* o, const char* s, size_t n) {
    size_t i;

    pb_putc(o, '"');

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '"' || c == '\\') {
            pb_putc(o, '\\');
            pb_putc(o, (char)c);
        } else if (c < 32) {
            pb_printf(o, "\\u%04x", c);
        } else {
            pb_putc(o, (char)c);
        }
    }

    pb_putc(o, '"');
}

/* A Word drawing canvas (wpc) or group (wpg) as one picture: its pictures,
   shapes and text boxes where the group's coordinates put them, written as
   a drawing resource (application/vnd.parade.drawing+json) the layout draws.
   m is at the group's opening tag; it is left after the closing one. */
static void dw_drawing_group(dw* w, pd_markup* m, int canvas) {
    dxi* X = w->X;
    pd_markup g;
    pd_buf o;
    dxform fr[17];
    dxfrm_in xf;
    int nfr = 1, depth = 1, in_grpsppr = 0, root_frame = !canvas, kind = 0, in_sppr = 0, in_ln = 0, in_txbx = 0;
    int in_rpr = 0, in_body_t = 0, para_open = 0, first_item = 1, have_fill = 0, have_line = 0, style_fill = 0;
    int style_line = 0, in_fillref = 0, in_lnref = 0, in_gs = 0, nopara = 1, head_arrow = 0, tail_arrow = 0;
    double cg_xy[4096], cg_pt[6];   /* a custom geometry's points (its own space), NAN pairs between rings */
    long long cg_w = 0, cg_h = 0;
    int cg_n = 0, cg_npt = 0, cg_closed = 0, cg_new_ring = 1, k2;
    char cg_cmd = 'm';
    char blip[64] = "", geom[32] = "rect", v[300], anchor[8] = "t";
    uint32_t fill = 0, line = 0, sfill = 0, sline = 0;
    long long lw = 9525, ins[4] = { 91440, 45720, 91440, 45720 };
    pd_char_props base, rcp;
    pd_buf text;
    int jc = PD_ALIGN_LEFT;

    memset(&xf, 0, sizeof(xf));
    memset(&o, 0, sizeof(o));
    memset(&text, 0, sizeof(text));
    fr[0].ox = fr[0].oy = 0;
    fr[0].sx = fr[0].sy = 1;

    /* the text boxes' default characters: Normal's, which is the document's default paragraph style by now */
    pd_doc_style_resolve(X->b->d, pd_doc_style_find(X->b->d, "Normal"), NULL, &base);

    pb_printf(&o, "{\"w\":%d,\"h\":%d,\"items\":[", (int)emu_sp((double)w->cx), (int)emu_sp((double)w->cy));
    mu_init(&g, m->s + m->pos, m->n - m->pos, 0);

#define FR (&fr[nfr - 1])
#define ITEM_SEP() do { if (!first_item) pb_putc(&o, ','); first_item = 0; } while (0)
    while (depth > 0 && mu_next(&g) != MT_END) {
        const char* t = mu_local(g.name);
        int open = g.type == MT_OPEN || g.type == MT_EMPTY;

        if (g.type == MT_OPEN) {
            depth++;
        } else if (g.type == MT_CLOSE) {
            depth--;
        }

        if (depth == 0) {
            break;
        }

        if (g.type == MT_TEXT) {
            if (in_body_t && in_txbx) {
                pd_buf dec;
                const char* u;
                size_t un;

                memset(&dec, 0, sizeof(dec));
                mu_decode(g.text, g.tlen, &dec);
                u = dec.p ? dec.p : "";
                un = dec.n;

                if (un) {
                    pd_char_props c = base;
                    dprops a, b;

                    memset(&a, 0, sizeof(a));
                    memset(&b, 0, sizeof(b));
                    a.cp = base;
                    b.cp = rcp;
                    pr_over(&a, &b);
                    c = a.cp;

                    if (!para_open) {
                        pb_printf(&text, "%s{\"a\":%d,\"runs\":[", nopara ? "" : ",", jc);
                        para_open = 1;
                        nopara = 0;
                    } else if (text.n && text.p[text.n - 1] != '[') {
                        pb_putc(&text, ',');
                    }

                    pb_puts(&text, "{\"t\":");
                    json_str(&text, u, un);
                    pb_printf(&text, ",\"sz\":%d,\"w\":%d,\"i\":%d,\"c\":%u,\"s\":%d,\"u\":%d", (int)c.size, (int)c.weight,
                              (int)c.italic, (unsigned)c.color, (int)c.shift, (int)c.underline);

                    if ((c.mask & PD_CP_FAMILY) && c.family[0]) {
                        pb_puts(&text, ",\"f\":");
                        json_str(&text, c.family, strlen(c.family));
                    }

                    pb_putc(&text, '}');
                }

                pb_free(&dec);
            }

            continue;
        }

        /* groups: their xfrm maps their children's coordinates into their parent's */
        if (!strcmp(t, "grpSpPr")) {
            if (g.type == MT_OPEN) {
                in_grpsppr = 1;
                memset(&xf, 0, sizeof(xf));
            } else if (g.type == MT_CLOSE) {
                double sx = xf.chext[0] > 0 ? (double)xf.ext[0] / xf.chext[0] : 1;
                double sy = xf.chext[1] > 0 ? (double)xf.ext[1] / xf.chext[1] : 1;

                in_grpsppr = 0;

                if (root_frame) {   /* the root group: its child space onto the drawing's extent */
                    sx = xf.chext[0] > 0 ? (double)w->cx / xf.chext[0] : 1;
                    sy = xf.chext[1] > 0 ? (double)w->cy / xf.chext[1] : 1;
                    fr[0].sx = sx;
                    fr[0].sy = sy;
                    fr[0].ox = -xf.choff[0] * sx;
                    fr[0].oy = -xf.choff[1] * sy;
                    root_frame = 0;
                } else if (nfr < 17) {  /* a nested group: off + (child - chOff) * ext / chExt, in its parent */
                    dxform* p = &fr[nfr - 1];

                    fr[nfr].sx = p->sx * sx;
                    fr[nfr].sy = p->sy * sy;
                    fr[nfr].ox = p->ox + p->sx * (xf.off[0] - xf.choff[0] * sx);
                    fr[nfr].oy = p->oy + p->sy * (xf.off[1] - xf.choff[1] * sy);
                    nfr++;
                }
            }

            continue;
        }

        if ((!strcmp(t, "grpSp") || !strcmp(t, "wgp")) && g.type == MT_CLOSE && nfr > 1) {   /* a canvas's groups are wgp */
            nfr--;
            continue;
        }

        if (in_grpsppr) {
            if (open) {
                xfrm_attr(&g, t, &xf);
            }

            continue;
        }

        /* a picture or a shape: read to its end, then written */
        if ((!strcmp(t, "pic") || !strcmp(t, "wsp")) && g.type == MT_OPEN) {
            kind = !strcmp(t, "pic") ? 1 : 2;
            memset(&xf, 0, sizeof(xf));
            blip[0] = '\0';
            strcpy(geom, "rect");
            have_fill = have_line = style_fill = style_line = 0;
            head_arrow = tail_arrow = 0;
            cg_n = cg_npt = cg_closed = 0;
            cg_w = cg_h = 0;
            cg_new_ring = 1;
            fill = line = sfill = sline = 0;
            lw = 9525;
            ins[0] = ins[2] = 91440;
            ins[1] = ins[3] = 45720;
            strcpy(anchor, "t");
            text.n = 0;
            nopara = 1;
            para_open = 0;
            continue;
        }

        if ((!strcmp(t, "pic") || !strcmp(t, "wsp")) && g.type == MT_CLOSE && kind) {
            double x = FR->ox + FR->sx * xf.off[0], y = FR->oy + FR->sy * xf.off[1];
            double cw = FR->sx * xf.ext[0], ch = FR->sy * xf.ext[1];

            if (para_open) {
                pb_puts(&text, "]}");
                para_open = 0;
            }

            if (kind == 1 && blip[0]) {
                pd_res_id r = dw_resource(X, blip);

                if (r) {
                    ITEM_SEP();
                    pb_printf(&o, "{\"img\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", (int)r, (int)emu_sp(x), (int)emu_sp(y),
                              (int)emu_sp(cw), (int)emu_sp(ch));
                }
            } else if (kind == 2) {
                uint32_t f = have_fill ? fill : style_fill ? sfill : 0, l = have_line ? line : style_line ? sline : 0;
                int turned = xf.rot % 21600000 != 0, isline = !strcmp(geom, "line");

                if (!strcmp(geom, "cust") && cg_n >= 2 && (f || l)) {     /* custom geometry: its path in the box */
                    double gx = cg_w > 0 ? cw / cg_w : 1, gy = cg_h > 0 ? ch / cg_h : 1;
                    int k;

                    ITEM_SEP();
                    pb_puts(&o, "{\"path\":[");

                    for (k = 0; k < cg_n; k++) {
                        if (isnan(cg_xy[2 * k])) {
                            pb_printf(&o, "%s%d,%d", k ? "," : "", (int)INT32_MIN, (int)INT32_MIN);
                        } else {
                            pb_printf(&o, "%s%d,%d", k ? "," : "", (int)emu_sp(x + cg_xy[2 * k] * gx),
                                      (int)emu_sp(y + cg_xy[2 * k + 1] * gy));
                        }
                    }

                    pb_printf(&o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d}", cg_closed, (unsigned)f, (unsigned)l,
                              (int)emu_sp((double)lw));
                    f = l = 0;
                }

                if ((f || l) && (turned || isline)) {
                    /* a turned box or ellipse, or a line in any direction: as a path, turned about its centre */
                    double cxm = x + cw / 2, cym = y + ch / 2, a = xf.rot / 60000.0 * 3.14159265358979 / 180;
                    double ca = cos(a), sa = sin(a), px[64], py[64];
                    int np = 0, k;

                    if (isline) {   /* corner to corner; the flips say which */
                        px[0] = xf.fliph ? x + cw : x;
                        py[0] = xf.flipv ? y + ch : y;
                        px[1] = xf.fliph ? x : x + cw;
                        py[1] = xf.flipv ? y : y + ch;
                        np = 2;
                    } else if (!strcmp(geom, "ellipse")) {
                        for (k = 0; k < 48; k++) {
                            px[k] = cxm + cw / 2 * cos(k * 6.2831853 / 48);
                            py[k] = cym + ch / 2 * sin(k * 6.2831853 / 48);
                        }

                        np = 48;
                    } else {
                        px[0] = x; py[0] = y;
                        px[1] = x + cw; py[1] = y;
                        px[2] = x + cw; py[2] = y + ch;
                        px[3] = x; py[3] = y + ch;
                        np = 4;
                    }

                    ITEM_SEP();
                    pb_puts(&o, "{\"path\":[");

                    for (k = 0; k < np; k++) {
                        double rx = cxm + (px[k] - cxm) * ca - (py[k] - cym) * sa;
                        double ry = cym + (px[k] - cxm) * sa + (py[k] - cym) * ca;

                        px[k] = rx;
                        py[k] = ry;
                        pb_printf(&o, "%s%d,%d", k ? "," : "", (int)emu_sp(rx), (int)emu_sp(ry));
                    }

                    pb_printf(&o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d}", isline ? 0 : 1,
                              (unsigned)(isline ? 0 : f), (unsigned)l, (int)emu_sp((double)lw));

                    /* arrowheads: a filled triangle at the end the line says */
                    for (k = 0; isline && l && k < 2; k++) {
                        if ((k == 0 && head_arrow) || (k == 1 && tail_arrow)) {
                            double tx = k ? px[1] : px[0], ty = k ? py[1] : py[0];
                            double fx = k ? px[0] : px[1], fy = k ? py[0] : py[1];
                            double dx = tx - fx, dy = ty - fy, len = sqrt(dx * dx + dy * dy), s3 = lw * 3 > 50800 ? lw * 3 : 50800;

                            if (len > 0) {
                                dx /= len;
                                dy /= len;
                                ITEM_SEP();
                                pb_printf(&o, "{\"path\":[%d,%d,%d,%d,%d,%d],\"closed\":1,\"fill\":%u,\"line\":0,\"lw\":0}",
                                          (int)emu_sp(tx), (int)emu_sp(ty), (int)emu_sp(tx - dx * s3 * 2 - dy * s3),
                                          (int)emu_sp(ty - dy * s3 * 2 + dx * s3), (int)emu_sp(tx - dx * s3 * 2 + dy * s3),
                                          (int)emu_sp(ty - dy * s3 * 2 - dx * s3), (unsigned)l);
                            }
                        }
                    }

                    f = l = 0;  /* drawn */
                }

                if (f || l) {
                    ITEM_SEP();
                    pb_printf(&o, "{\"shape\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d}", geom,
                              (int)emu_sp(x), (int)emu_sp(y), (int)emu_sp(cw), (int)emu_sp(ch), (unsigned)f, (unsigned)l,
                              (int)emu_sp((double)lw));
                }

                if (!nopara) {
                    ITEM_SEP();
                    pb_puts(&o, "{\"text\":[");
                    pb_put(&o, text.p, text.n);
                    pb_printf(&o, "],\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"ins\":[%d,%d,%d,%d],\"anchor\":\"%s\"}",
                              (int)emu_sp(x), (int)emu_sp(y), (int)emu_sp(cw), (int)emu_sp(ch), (int)emu_sp((double)ins[0]),
                              (int)emu_sp((double)ins[1]), (int)emu_sp((double)ins[2]), (int)emu_sp((double)ins[3]), anchor);
                }
            }

            kind = 0;
            continue;
        }

        if (!kind) {
            continue;
        }

        if (!strcmp(t, "spPr")) {
            in_sppr = g.type == MT_OPEN;
        } else if (!strcmp(t, "ln") && in_sppr) {
            in_ln = g.type == MT_OPEN;

            if (open) {
                have_line = 1;      /* said here: no colour inside means none */
                line = 0;
                lw = mu_attr(&g, "w", v, sizeof(v)) ? atoll(v) : 9525;
            }
        } else if (!strcmp(t, "txbxContent")) {
            in_txbx = g.type == MT_OPEN;
        } else if (!strcmp(t, "fillRef")) {
            in_fillref = g.type == MT_OPEN && attr_int(&g, "idx", 0) > 0;
        } else if (!strcmp(t, "lnRef")) {
            in_lnref = g.type == MT_OPEN && attr_int(&g, "idx", 0) > 0;
        } else if (!strcmp(t, "gs")) {
            in_gs = g.type == MT_OPEN ? in_gs + 1 : 0;
        } else if (!open) {
            if (!strcmp(t, "p") && para_open) {
                pb_puts(&text, "]}");
                para_open = 0;
            } else if (!strcmp(t, "rPr")) {
                in_rpr = 0;
            } else if (!strcmp(t, "t")) {
                in_body_t = 0;
            }

            continue;
        } else if (in_sppr && !in_ln && (!strcmp(t, "xfrm") || !strcmp(t, "off") || !strcmp(t, "ext"))) {
            xfrm_attr(&g, t, &xf);
        } else if (in_sppr && !strcmp(t, "custGeom")) {
            strcpy(geom, "cust");
        } else if (in_sppr && !strcmp(t, "path") && !strcmp(geom, "cust")) {   /* its coordinate space */
            cg_w = mu_attr(&g, "w", v, sizeof(v)) ? atoll(v) : 0;
            cg_h = mu_attr(&g, "h", v, sizeof(v)) ? atoll(v) : 0;
            cg_new_ring = 1;
        } else if (in_sppr && !strcmp(geom, "cust") && (!strcmp(t, "moveTo") || !strcmp(t, "lnTo") ||
                   !strcmp(t, "cubicBezTo") || !strcmp(t, "quadBezTo"))) {
            cg_new_ring |= !strcmp(t, "moveTo");
            cg_cmd = t[0];
            cg_npt = 0;
        } else if (in_sppr && !strcmp(geom, "cust") && !strcmp(t, "close")) {
            cg_closed = 1;
        } else if (in_sppr && !strcmp(geom, "cust") && !strcmp(t, "pt") && cg_n + 4 < 2048) {
            double px = mu_attr(&g, "x", v, sizeof(v)) ? atof(v) : 0, py = mu_attr(&g, "y", v, sizeof(v)) ? atof(v) : 0;

            cg_pt[2 * cg_npt] = px;
            cg_pt[2 * cg_npt + 1] = py;
            cg_npt++;

            /* a point, or the end of a curve: a curve as its end point and its control points, straightened */
            if ((cg_cmd == 'c' && cg_npt == 3) || (cg_cmd == 'q' && cg_npt == 2) || cg_cmd == 'm' || cg_cmd == 'l') {
                if (cg_new_ring && cg_n > 0) {
                    cg_xy[2 * cg_n] = NAN;
                    cg_xy[2 * cg_n + 1] = NAN;
                    cg_n++;
                }

                cg_new_ring = 0;

                for (k2 = 0; k2 < cg_npt; k2++) {
                    cg_xy[2 * cg_n] = cg_pt[2 * k2];
                    cg_xy[2 * cg_n + 1] = cg_pt[2 * k2 + 1];
                    cg_n++;
                }

                cg_npt = 0;
            }
        } else if (in_sppr && !strcmp(t, "prstGeom") && mu_attr(&g, "prst", v, sizeof(v))) {
            snprintf(geom, sizeof(geom), "%s", !strcmp(v, "ellipse") ? "ellipse" : !strcmp(v, "line") ||
                     strstr(v, "Connector") ? "line" : "rect");
        } else if (!strcmp(t, "srgbClr") || !strcmp(t, "schemeClr")) {
            uint32_t c = !strcmp(t, "srgbClr") ? (mu_attr(&g, "val", v, sizeof(v)) ?
                         0xFF000000u | (uint32_t)strtoul(v, NULL, 16) : 0xFF000000u) :
                         (mu_attr(&g, "val", v, sizeof(v)) ? scheme_color(v) : 0xFF000000u);

            if (in_ln) {
                line = c;
            } else if (in_sppr && (!in_gs || in_gs == 1) && !have_fill) {
                fill = c;   /* a gradient: its first stop */
                have_fill = 1;
            } else if (in_fillref) {
                sfill = c;
                style_fill = 1;
            } else if (in_lnref) {
                sline = c;
                style_line = 1;
            }
        } else if (in_ln && (!strcmp(t, "headEnd") || !strcmp(t, "tailEnd"))) {
            int arrow = mu_attr(&g, "type", v, sizeof(v)) && strcmp(v, "none") != 0;

            if (t[0] == 'h') {
                head_arrow = arrow;
            } else {
                tail_arrow = arrow;
            }
        } else if (in_sppr && !in_ln && !strcmp(t, "noFill")) {
            have_fill = 1;
            fill = 0;
        } else if (kind == 1 && !strcmp(t, "blip")) {
            mu_attr(&g, "r:embed", blip, sizeof(blip));
        } else if (!strcmp(t, "bodyPr")) {
            static const char* in[4] = { "lIns", "tIns", "rIns", "bIns" };
            int k;

            for (k = 0; k < 4; k++) {
                if (mu_attr(&g, in[k], v, sizeof(v))) {
                    ins[k] = atoll(v);
                }
            }

            if (mu_attr(&g, "anchor", v, sizeof(v))) {
                snprintf(anchor, sizeof(anchor), "%.3s", v);
            }
        } else if (in_txbx) {
            if (!strcmp(t, "p")) {
                jc = PD_ALIGN_LEFT;
                memset(&rcp, 0, sizeof(rcp));
            } else if (!strcmp(t, "jc") && mu_attr(&g, "w:val", v, sizeof(v))) {
                jc = !strcmp(v, "center") ? PD_ALIGN_CENTER : !strcmp(v, "right") || !strcmp(v, "end") ? PD_ALIGN_RIGHT :
                     !strcmp(v, "both") ? PD_ALIGN_JUSTIFY : PD_ALIGN_LEFT;
            } else if (!strcmp(t, "r")) {
                memset(&rcp, 0, sizeof(rcp));
            } else if (!strcmp(t, "rPr")) {
                in_rpr = g.type == MT_OPEN;
            } else if (in_rpr) {
                rpr_elem(X, &g, t, &rcp, NULL, 0);
            } else if (!strcmp(t, "t")) {
                in_body_t = g.type == MT_OPEN;
            }
        }
    }
#undef FR
#undef ITEM_SEP

    pb_puts(&o, "]}");
    m->pos += g.pos;

    if (!o.err && pd_doc_add_resource(X->b->d, "application/vnd.parade.drawing+json", o.p, o.n, &w->drawing_res) != PD_OK) {
        w->drawing_res = 0;
    }

    pb_free(&o);
    pb_free(&text);
}

/* a VML style's length (width:228.6pt;height:32.4pt) in EMU; 0 if absent */
static long long vml_length(const char* style, const char* prop) {
    const char* p = style;
    size_t n = strlen(prop);

    while ((p = strstr(p, prop)) != NULL) {
        if ((p == style || p[-1] == ';' || p[-1] == ' ') && p[n] == ':') {
            double v = atof(p + n + 1);
            const char* u = p + n + 1;

            while (*u && (isdigit((unsigned char)*u) || *u == '.' || *u == ' ' || *u == '-')) {
                u++;
            }

            return (long long)(v * (!strncmp(u, "in", 2) ? 914400 : !strncmp(u, "cm", 2) ? 360000 : !strncmp(u, "mm", 2) ?
                                    36000 : !strncmp(u, "px", 2) ? 9525 : 12700));
        }

        p += n;
    }

    return 0;
}

/* the drawing's picture -- or the drawing itself, a group or canvas made into one -- inline or floating */
static void dw_image(dw* w) {
    dxi* X = w->X;
    pd_inline o;

    if (w->cx <= 0 || w->cy <= 0) {
        return;
    }

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_IMAGE;
    o.width = (pd_sp)(w->cx * 65536 / 12700);
    o.height = (pd_sp)(w->cy * 65536 / 12700);
    o.resource = w->drawing_res ? w->drawing_res : dw_resource(X, w->blip);

    if (!o.resource) {
        return;
    }

    if (!w->anchor) {
        dw_begin_para(w);
        bld_inline(X->b, &o);
    } else if (w->npend_fl < 8) {
        int k = w->npend_fl++;

        memset(&w->pend_fl[k], 0, sizeof(w->pend_fl[0]));
        w->pend_fl[k].res = o.resource;
        w->pend_fl[k].w = o.width;
        w->pend_fl[k].h = o.height;
        w->pend_fl[k].gap = (pd_sp)(w->dist * 65536 / 12700);
        w->pend_fl[k].wrap = w->wrap;
        w->pend_fl[k].has_off = w->posh_has_off && w->posh_align < 0;
        w->pend_fl[k].off_x = (pd_sp)(w->posh_off * 65536 / 12700) - (w->posh_page ? X->margin_left : 0);

        if (!w->started) {  /* anchored before any text: the float goes first */
            dw_floats(w);
        }
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
           strcmp(word, "SECTIONPAGES") == 0 ? PD_FIELD_SECTION_PAGE + 1 : strcmp(word, "PAGEREF") == 0 ?
           PD_FIELD_REF_PAGE + 1 : strcmp(word, "SEQ") == 0 ? PD_FIELD_SEQ + 1 : strcmp(word, "DATE") == 0 ?
           PD_FIELD_DATE + 1 : 0;
}

/* the instruction's first argument: a bookmark (PAGEREF), a sequence (SEQ) */
static void field_arg(const char* instr, char* out, size_t cap) {
    const char* p = instr;
    size_t k = 0;

    while (*p == ' ') {
        p++;
    }

    while (*p && *p != ' ') {   /* the field's name */
        p++;
    }

    while (*p == ' ') {
        p++;
    }

    if (*p == '"') {
        p++;
    }

    while (*p && *p != ' ' && *p != '"' && *p != '\\' && k + 1 < cap) {
        out[k++] = *p++;
    }

    out[k] = '\0';
}

static void dw_field(dw* w, int kind, const char* instr) {
    pd_inline o;
    pd_bld* b = w->X->b;
    char arg[64];

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = kind - 1;
    field_arg(instr, arg, sizeof(arg));

    if (o.field == PD_FIELD_SEQ) {
        snprintf(o.name, sizeof(o.name), "%.31s", arg[0] ? arg : "Figure");
    }

    dw_begin_para(w);
    dw_apply_run(w);
    bld_inline(b, &o);

    if (o.field == PD_FIELD_REF_PAGE && arg[0] && w->X->nrefs < 2048) {
        pd_block_info bi;   /* its target once every bookmark is known: the field is the paragraph's last 3 bytes */

        if (b->para && pd_doc_block_info(b->d, b->para, &bi) == PD_OK && bi.text_length >= 3) {
            snprintf(w->X->refs[w->X->nrefs].name, sizeof(w->X->refs[0].name), "%.31s", arg);
            w->X->refs[w->X->nrefs].para = b->para;
            w->X->refs[w->X->nrefs].off = bi.text_length - 3;
            w->X->refs[w->X->nrefs].field = o.field;
            w->X->nrefs++;
        }
    }
}

/* the story of a header or footer part, read once however many sections use it */
/* The fonts a document carries (fontTable.xml's embedRegular and the
   rest): each de-obfuscated -- its first 32 bytes XORed with the key its
   GUID gives, as ECMA-376 has Word do -- and kept as a resource whose type
   names it: font/ttf; family="Name"; weight=700; italic=1. Hosts load them
   ahead of their own. */
static void read_fonts(dxi* X) {
    char* xml;
    size_t len = 0;
    pd_markup m;
    char family[64] = "";
    int saved_n = 0, swapped;
    drel* saved = NULL;

    if ((xml = (char*)zip_read(&X->z, "word/fontTable.xml", &len)) == NULL) {
        return;
    }

    swapped = part_rels_begin(X, "word/fontTable.xml", &saved, &saved_n);
    mu_init(&m, xml, len, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);
        char rid[64], key[64];

        if ((m.type == MT_OPEN || m.type == MT_EMPTY) && !strcmp(t, "font")) {
            if (!mu_attr(&m, "w:name", family, sizeof(family))) {
                family[0] = '\0';
            }
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && !strncmp(t, "embed", 5) && family[0] &&
                   mu_attr(&m, "r:id", rid, sizeof(rid)) && mu_attr(&m, "w:fontKey", key, sizeof(key)) && strlen(key) == 38) {
            static const int pos[16] = { 35, 33, 31, 29, 27, 25, 22, 20, 17, 15, 12, 10, 7, 5, 3, 1 };
            const char* target = rel_target(X, rid, NULL);
            char path[300];
            unsigned char* data, k[16];
            size_t n = 0;
            int i, bold = strstr(t, "Bold") != NULL, italic = strstr(t, "Italic") != NULL;

            if (!target) {
                continue;
            }

            for (i = 0; i < 16; i++) {
                char h[3] = { key[pos[i]], key[pos[i] + 1], 0 };

                k[i] = (unsigned char)strtoul(h, NULL, 16);
            }

            snprintf(path, sizeof(path), "%s%s", target[0] == '/' ? "" : "word/", target[0] == '/' ? target + 1 : target);

            if ((data = zip_read(&X->z, path, &n)) != NULL && n > 32) {
                char mime[160];
                pd_res_id res;

                for (i = 0; i < 16; i++) {
                    data[i] ^= k[i];
                    data[i + 16] ^= k[i];
                }

                snprintf(mime, sizeof(mime), "font/ttf; family=\"%s\"; weight=%d; italic=%d", family, bold ? 700 : 400, italic);
                pd_doc_add_resource(X->b->d, mime, data, n, &res);
            }

            free(data);
        }
    }

    part_rels_end(X, swapped, saved, saved_n);
    free(xml);
}

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
        int saved_n = 0, swapped;
        drel* saved = NULL;

        swapped = part_rels_begin(X, path, &saved, &saved_n);
        story = bld_story_begin(X->b);
        X->depth++;
        dw_parse(X, xml, len, 1);
        X->depth--;
        bld_story_end(X->b);
        part_rels_end(X, swapped, saved, saved_n);
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
    int tk;

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
                w->posh_has_off = 1;
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

        if (m.type == MT_OPEN && (strcmp(t, "oMathPara") == 0 || strcmp(t, "oMath") == 0) && !w->in_drawing) {
            /* an equation: Word's math as the LaTeX Parade typesets */
            pd_markup g;
            int depth = 1;
            size_t end = 0;
            char* tex;

            mu_init(&g, m.s + m.pos, m.n - m.pos, 0);

            while (depth > 0 && mu_next(&g) != MT_END) {
                if (g.type == MT_OPEN) {
                    depth++;
                } else if (g.type == MT_CLOSE && --depth == 0) {
                    break;
                }

                end = g.pos;
            }

            if ((tex = pd_omml_to_latex(m.s + m.pos, end, t)) != NULL) {
                pd_inline o;

                if (t[1] == 'M' && t[5] == 'P' && !w->started) {    /* oMathPara opening the paragraph: a display */
                    w->math_para = 1;
                }

                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_EQUATION;
                o.source = tex;
                o.source_len = (int32_t)strlen(tex);

                if (o.source_len > 0) {
                    dw_begin_para(w);
                    bld_inline(X->b, &o);
                }

                free(tex);
            }

            m.pos += g.pos;
            continue;
        }

        if (m.type == MT_OPEN && w->in_drawing && w->anchor && !w->tbx && strcmp(t, "txbxContent") == 0 &&
                X->depth < 3) {
            /* a floating text box: its paragraphs become a float's, read once the anchoring paragraph ends */
            pd_markup g;
            int depth = 1;
            size_t end = 0;

            mu_init(&g, m.s + m.pos, m.n - m.pos, 0);

            while (depth > 0 && mu_next(&g) != MT_END) {
                if (g.type == MT_OPEN) {
                    depth++;
                } else if (g.type == MT_CLOSE && --depth == 0) {
                    break;
                }

                end = g.pos;
            }

            w->tbx = m.s + m.pos;
            w->tbn = end;
            m.pos += g.pos;
            continue;
        }

        if (open) {
            /* elements whose content is ignored */
            if (strcmp(t, "txbxContent") == 0 || strcmp(t, "Fallback") == 0 ||
                    (w->in_ppr && strcmp(t, "rPr") == 0) || strcmp(t, "rPrChange") == 0 ||
                    strcmp(t, "pPrChange") == 0) {
                if (m.type == MT_OPEN) {
                    w->skip = 1;
                }

                continue;
            }

            if ((!strcmp(t, "ins") || !strcmp(t, "del") || !strcmp(t, "moveTo") || !strcmp(t, "moveFrom")) &&
                    m.type == MT_OPEN && !w->in_rpr) {
                pd_revision rv;     /* a tracked change: the runs inside are an insertion or a deletion */

                memset(&rv, 0, sizeof(rv));
                rv.kind = t[0] == 'd' || !strcmp(t, "moveFrom") ? PD_REV_DELETE : PD_REV_INSERT;
                mu_attr(&m, "w:author", rv.author, sizeof(rv.author));
                mu_attr(&m, "w:date", rv.date, sizeof(rv.date));

                if (pd_doc_revision_add(X->b->d, &rv, &w->rev) != PD_OK) {
                    w->rev = 0;
                }

                continue;
            }

            if ((!strcmp(t, "commentRangeStart") || !strcmp(t, "commentRangeEnd") || !strcmp(t, "commentReference")) &&
                    mu_attr(&m, "w:id", v, sizeof(v))) {
                dw_comment_mark(w, atoi(v), !strcmp(t, "commentRangeStart") ? 0 : !strcmp(t, "commentRangeEnd") ? 1 : 2);
                continue;
            }

            if (strcmp(t, "p") == 0) {
                dw_begin_cell(w);
                w->math_para = 0;
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
                w->sp.mirror_margins = X->mirror ? 1 : -1;  /* Word mirrors only when its settings say so */
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
                    w->sp.gutter = twips(attr_int(&m, "w:gutter", 0));
                    w->sp.gutter = w->sp.gutter < 0 ? 0 : w->sp.gutter;
                    w->X->margin_left = w->sp.margin_left;  /* for the pictures of its headers, read next */
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
                } else if (strcmp(t, "vAlign") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->sp.page_valign = !strcmp(v, "center") ? 1 : !strcmp(v, "bottom") ? 2 : 0;
                } else if (strcmp(t, "lnNumType") == 0) {
                    int by = attr_int(&m, "w:countBy", 0);
                    char rs[16] = "";

                    w->sp.line_numbers = by < 0 ? 0 : by > 100 ? 100 : by;
                    w->sp.line_number_start = attr_int(&m, "w:start", 0) + 1;   /* Word writes one less */
                    w->sp.line_number_start = w->sp.line_number_start < 1 ? 1 : w->sp.line_number_start;
                    w->sp.line_number_distance = twips(attr_int(&m, "w:distance", 0));
                    w->sp.line_number_distance = w->sp.line_number_distance < 0 ? 0 : w->sp.line_number_distance;
                    mu_attr(&m, "w:restart", rs, sizeof(rs));
                    w->sp.line_number_restart = strcmp(rs, "newSection") == 0 ? PD_LINENUM_SECTION :
                                                strcmp(rs, "continuous") == 0 ? PD_LINENUM_CONTINUOUS : PD_LINENUM_PAGE;
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
            } else if (strcmp(t, "t") == 0 || strcmp(t, "delText") == 0) {
                w->in_t = m.type == MT_OPEN;
            } else if (strcmp(t, "instrText") == 0 || strcmp(t, "delInstrText") == 0) {
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
            } else if (strcmp(t, "bookmarkStart") == 0 && w->in_p && mu_attr(&m, "w:name", v, sizeof(v)) && v[0] &&
                       strcmp(v, "_GoBack") != 0) {
                pd_inline o;    /* a named place (a heading's _Toc, a caption's _Ref: references point there) */

                memset(&o, 0, sizeof(o));
                o.kind = PD_INLINE_BOOKMARK;
                snprintf(o.name, sizeof(o.name), "%.31s", v);
                dw_begin_para(w);
                bld_inline(X->b, &o);

                if (X->nbm < 4096) {
                    snprintf(X->bm[X->nbm].name, sizeof(X->bm[0].name), "%.31s", v);
                    X->bm[X->nbm++].para = X->b->para;
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
                w->posh_page = w->posh_has_off = 0;
                w->drawing_res = 0;
                w->tbx = NULL;
                w->tbn = 0;
                w->dist = 0;
            } else if ((strcmp(t, "object") == 0 || strcmp(t, "pict") == 0) && m.type == MT_OPEN && !w->in_drawing) {
                /* VML: an OLE object's preview picture (an equation, a chart), or an old-style picture */
                w->in_vml = 1;
                w->blip[0] = '\0';
                w->cx = w->cy = 0;
            } else if (w->in_vml && strcmp(t, "shape") == 0 && w->cx == 0 && mu_attr(&m, "style", v, sizeof(v))) {
                w->cx = vml_length(v, "width");
                w->cy = vml_length(v, "height");
            } else if (w->in_vml && strcmp(t, "imagedata") == 0 && !w->blip[0]) {
                mu_attr(&m, "r:id", w->blip, sizeof(w->blip));
            } else if (w->in_drawing && m.type == MT_OPEN && (strcmp(t, "wpc") == 0 || strcmp(t, "wgp") == 0)) {
                dw_drawing_group(w, &m, strcmp(t, "wpc") == 0);   /* reads the group to its end */
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
                w->posh_page = mu_attr(&m, "relativeFrom", v, sizeof(v)) && (strcmp(v, "page") == 0 ||
                               strcmp(v, "leftMargin") == 0);
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
                } else if (m.type == MT_OPEN && w->nlinks < 16 && mu_attr(&m, "w:anchor", v + 1, sizeof(v) - 1) && v[1]) {
                    v[0] = '#';     /* a place in the document: its bookmark, as a fragment */
                    dw_link(w, v, strlen(v));
                    w->link_depth[w->nlinks++] = 1;
                } else if (m.type == MT_OPEN && w->nlinks < 16) {
                    w->link_depth[w->nlinks++] = 0;
                }
            } else if (strcmp(t, "fldSimple") == 0) {
                int kind;

                w->simple_link = 0;

                if (mu_attr(&m, "w:instr", v, sizeof(v)) && (kind = page_field(v)) != 0) {
                    dw_field(w, kind, v);   /* computed here; Word's last result is not kept */

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
                        dw_field(w, w->fld_kind, w->instr);     /* and its cached result is dropped */
                    }

                    if (q && e && e > q + 1) {
                        dw_link(w, q + 1, (size_t)(e - q - 1));
                        w->fld_link = 1;
                    } else if (!w->fld_kind && (!strncmp(w->instr + strspn(w->instr, " "), "REF ", 4) ||
                                                !strncmp(w->instr + strspn(w->instr, " "), "NOTEREF ", 8))) {
                        char arg[64];   /* a reference: Word's text kept, a link to the place it names */

                        arg[0] = '#';
                        field_arg(w->instr, arg + 1, sizeof(arg) - 1);

                        if (arg[1]) {
                            dw_link(w, arg, strlen(arg));
                            w->fld_link = 1;
                        }
                    }
                } else if (strcmp(v, "end") == 0) {
                    if (w->fld == 1 && page_field(w->instr)) {     /* no result part at all */
                        dw_field(w, page_field(w->instr), w->instr);
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

                if (w->ntab < 8) {
                    memset(&w->tabs[w->ntab], 0, sizeof(w->tabs[0]));
                    w->tabs[w->ntab].look = 0x04A0;     /* Word's when it says nothing: header row, first column */
                    w->tabs[w->ntab].row = -1;

                    for (tk = 0; tk < 4; tk++) {
                        w->tabs[w->ntab].mar[tk] = -1;
                    }
                }

                w->ntab++;
                w->t_width = 0;
                w->t_jc = -1;
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
                if (w->ntab >= 1 && w->ntab <= 8) {
                    edge_elem(&m, t, &w->tabs[w->ntab - 1].tb);
                }
            } else if (w->in_tblpr && w->ntab >= 1 && w->ntab <= 8 && strcmp(t, "tblStyle") == 0) {
                mu_attr(&m, "w:val", w->tabs[w->ntab - 1].style, sizeof(w->tabs[0].style));
            } else if (w->in_tblpr && w->ntab >= 1 && w->ntab <= 8 && strcmp(t, "tblLook") == 0) {
                int look = 0;

                if (mu_attr(&m, "w:val", v, sizeof(v))) {
                    look = (int)strtol(v, NULL, 16);
                }

                look |= attr_int(&m, "w:firstRow", (look >> 5) & 1) ? 0x20 : 0;
                look &= attr_int(&m, "w:firstRow", 1) ? ~0 : ~0x20;
                look = attr_int(&m, "w:firstColumn", (look >> 7) & 1) ? look | 0x80 : look & ~0x80;
                look = attr_int(&m, "w:lastRow", (look >> 6) & 1) ? look | 0x40 : look & ~0x40;
                look = attr_int(&m, "w:lastColumn", (look >> 8) & 1) ? look | 0x100 : look & ~0x100;
                look = attr_int(&m, "w:noHBand", (look >> 9) & 1) ? look | 0x200 : look & ~0x200;
                look = attr_int(&m, "w:noVBand", (look >> 10) & 1) ? look | 0x400 : look & ~0x400;
                w->tabs[w->ntab - 1].look = look;
            } else if (w->in_tblpr && w->ntab >= 1 && w->ntab <= 8 && strcmp(t, "tblInd") == 0) {
                w->tabs[w->ntab - 1].ind = twips(attr_int(&m, "w:w", 0));
                w->tabs[w->ntab - 1].has_ind = 1;
            } else if (w->in_tblpr && strcmp(t, "tblCellMar") == 0) {
                w->in_mar = m.type == MT_OPEN;
            } else if (w->in_tblpr && w->in_mar && w->ntab >= 1 && w->ntab <= 8) {
                int side = !strcmp(t, "top") ? 0 : !strcmp(t, "right") || !strcmp(t, "end") ? 1 :
                           !strcmp(t, "bottom") ? 2 : !strcmp(t, "left") || !strcmp(t, "start") ? 3 : -1;

                if (side >= 0) {
                    w->tabs[w->ntab - 1].mar[side] = twips(attr_int(&m, "w:w", 0));
                }
            } else if (w->in_tblpr && strcmp(t, "tblLayout") == 0) {
                /* written only by an exporter that left the widths to the reader (Parade's own) */
                w->t_autofit = mu_attr(&m, "w:type", v, sizeof(v)) && strcmp(v, "autofit") == 0;
            } else if (w->in_tblpr && strcmp(t, "tblW") == 0) {
                if (mu_attr(&m, "w:type", v, sizeof(v)) && strcmp(v, "dxa") == 0) {
                    w->t_width = twips(attr_int(&m, "w:w", 0));
                } else if (strcmp(v, "pct") == 0 && w->ntab >= 1 && w->ntab <= 8 && mu_attr(&m, "w:w", v, sizeof(v))) {
                    /* fiftieths of a percent, or a percentage written out */
                    double pct = strchr(v, '%') ? atof(v) * 10 : atof(v) / 5;

                    w->tabs[w->ntab - 1].width_pct = (pd_sp)(pct < 0 ? 0 : pct > 1000 ? 1000 : pct);
                }
            } else if (w->in_tblpr && strcmp(t, "jc") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                w->t_jc = strcmp(v, "center") == 0 ? PD_ALIGN_CENTER : strcmp(v, "right") == 0 ||
                          strcmp(v, "end") == 0 ? PD_ALIGN_RIGHT : PD_ALIGN_LEFT;
            } else if (strcmp(t, "tr") == 0) {
                w->pend_row = 1;
                w->row_header = 0;

                if (w->ntab >= 1 && w->ntab <= 8) {
                    w->tabs[w->ntab - 1].row++;
                    w->tabs[w->ntab - 1].col = 0;
                    w->tabs[w->ntab - 1].row_h = 0;
                }
            } else if (strcmp(t, "trPr") == 0) {
                w->in_trpr = m.type == MT_OPEN;
            } else if (w->in_trpr && strcmp(t, "tblHeader") == 0) {
                w->row_header = attr_on(&m);
            } else if (w->in_trpr && strcmp(t, "trHeight") == 0 && w->ntab >= 1 && w->ntab <= 8) {
                w->tabs[w->ntab - 1].row_h = twips(attr_int(&m, "w:val", 0));    /* at least, or exactly: at least */
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
                memset(&w->cell_edges, 0, sizeof(w->cell_edges));
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
                } else if (strcmp(t, "tcBorders") == 0) {
                    w->in_cb = m.type == MT_OPEN;
                } else if (w->in_cb) {
                    edge_elem(&m, t, &w->cell_edges);
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
        } else if (strcmp(t, "t") == 0 || strcmp(t, "delText") == 0) {
            w->in_t = 0;
        } else if (strcmp(t, "instrText") == 0 || strcmp(t, "delInstrText") == 0) {
            w->in_instr = 0;
        } else if ((!strcmp(t, "ins") || !strcmp(t, "del") || !strcmp(t, "moveTo") || !strcmp(t, "moveFrom")) &&
                   !w->in_rpr) {
            w->rev = 0;
        } else if (strcmp(t, "positionH") == 0) {
            w->in_posh = 0;
        } else if (strcmp(t, "align") == 0) {
            w->in_align = 0;
        } else if (strcmp(t, "posOffset") == 0) {
            w->in_offset = 0;
        } else if ((strcmp(t, "object") == 0 || strcmp(t, "pict") == 0) && w->in_vml) {
            if (w->blip[0] && w->cx > 0 && w->cy > 0) {
                w->anchor = 0;      /* in the line, as Word shows an object */
                w->drawing_res = 0;
                dw_image(w);
            }

            w->in_vml = 0;
            w->blip[0] = '\0';
        } else if (strcmp(t, "drawing") == 0) {
            if (w->wrap < 0) {      /* wrapped: which side it is on */
                w->wrap = w->wrap == -9 ? (w->posh_align >= 0 ? w->posh_align :
                                           w->posh_off > 2286000 ? PD_WRAP_RIGHT : PD_WRAP_LEFT) : -w->wrap;
            }

            if (w->in_drawing && w->tbx && w->anchor && w->cx > 0 && w->npend_fl < 8) {
                int k = w->npend_fl++;

                memset(&w->pend_fl[k], 0, sizeof(w->pend_fl[0]));
                w->pend_fl[k].w = (pd_sp)(w->cx * 65536 / 12700);
                w->pend_fl[k].h = (pd_sp)(w->cy * 65536 / 12700);
                w->pend_fl[k].gap = (pd_sp)(w->dist * 65536 / 12700);
                w->pend_fl[k].wrap = w->wrap;
                w->pend_fl[k].has_off = w->posh_has_off && w->posh_align < 0;
                w->pend_fl[k].off_x = (pd_sp)(w->posh_off * 65536 / 12700) - (w->posh_page ? X->margin_left : 0);
                w->pend_fl[k].tbx = w->tbx;
                w->pend_fl[k].tbn = w->tbn;

                if (!w->started) {
                    dw_floats(w);
                }
            } else if (w->in_drawing && (w->blip[0] || w->drawing_res)) {
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
            w->in_cb = 0;
        } else if (strcmp(t, "tcBorders") == 0) {
            w->in_cb = 0;
        } else if (strcmp(t, "tblCellMar") == 0) {
            w->in_mar = 0;
        } else if (strcmp(t, "tc") == 0) {
            dw_begin_cell(w);
            bld_end_para(X->b);
            bld_cell_end(X->b);

            if (w->ntab >= 1 && w->ntab <= 8) {
                w->tabs[w->ntab - 1].col += w->tabs[w->ntab - 1].cspan > 0 ? w->tabs[w->ntab - 1].cspan : 1;
                w->tabs[w->ntab - 1].cell.given = 0;
            }
        } else if (strcmp(t, "tr") == 0) {
            if (w->pend_row) {
                bld_row_begin(X->b, w->row_header);
                w->pend_row = 0;
            }
        } else if (strcmp(t, "tbl") == 0) {
            bld_table_end(X->b);
            w->ntab -= w->ntab > 0;
        }
    }

    free(w);
}

/* comments.xml: each comment's text, on the range its w:id marked in the text; commentsExtended.xml
   makes replies and resolved ones of them */
static void read_comments(dxi* X, pd_doc* d) {
    struct {
        unsigned para, parent;
        int done;
    }* ex = NULL;
    struct {
        unsigned para;
        pd_comment_id id;
    }* made = NULL;
    int nex = 0, capex = 0, nmade = 0, capmade = 0, k;
    size_t len = 0;
    char* xml;
    pd_markup m;
    char v[64];

    if (!X->ncm) {
        return;
    }

    if ((xml = (char*)zip_read(&X->z, "word/comments.xml", &len)) == NULL) {
        xml = (char*)calloc(1, 1);  /* none: the ranges' markers still go */
    }

    {
        char* xe;
        size_t elen = 0;

        if ((xe = (char*)zip_read(&X->z, "word/commentsExtended.xml", &elen)) != NULL) {
            mu_init(&m, xe, elen, 0);

            while (mu_next(&m) != MT_END) {
                if ((m.type == MT_OPEN || m.type == MT_EMPTY) && !strcmp(mu_local(m.name), "commentEx") &&
                        mu_attr(&m, "w15:paraId", v, sizeof(v)) && nex < 65536 &&
                        !pd_grow((void**)&ex, &capex, (int64_t)nex + 1, sizeof(*ex))) {
                    ex[nex].para = (unsigned)strtoul(v, NULL, 16);
                    ex[nex].parent = mu_attr(&m, "w15:paraIdParent", v, sizeof(v)) ? (unsigned)strtoul(v, NULL, 16) : 0;
                    ex[nex].done = mu_attr(&m, "w15:done", v, sizeof(v)) && atoi(v) != 0;
                    nex++;
                }
            }

            free(xe);
        }
    }

    mu_init(&m, xml, len, 0);

    while (mu_next(&m) != MT_END) {
        pd_comment c;
        pd_buf text;
        int depth = 1, in_t = 0, paras = 0, wid;
        unsigned last_para = 0;
        struct dcmt* at = NULL;

        if (m.type != MT_OPEN || strcmp(mu_local(m.name), "comment") != 0 || !mu_attr(&m, "w:id", v, sizeof(v))) {
            continue;
        }

        wid = atoi(v);
        memset(&c, 0, sizeof(c));
        memset(&text, 0, sizeof(text));
        mu_attr(&m, "w:author", c.author, sizeof(c.author));
        mu_attr(&m, "w:date", c.date, sizeof(c.date));

        while (depth > 0 && mu_next(&m) != MT_END) {
            const char* t = mu_local(m.name);

            if (m.type == MT_OPEN) {
                depth++;
            } else if (m.type == MT_CLOSE) {
                depth--;
            }

            if (m.type == MT_OPEN && !strcmp(t, "p")) {
                if (paras++) {
                    pb_putc(&text, '\n');
                }

                last_para = mu_attr(&m, "w14:paraId", v, sizeof(v)) ? (unsigned)strtoul(v, NULL, 16) : 0;
            } else if ((m.type == MT_OPEN || m.type == MT_CLOSE) && !strcmp(t, "t")) {
                in_t = m.type == MT_OPEN;
            } else if (m.type == MT_TEXT && in_t) {
                mu_decode(m.text, m.tlen, &text);
            } else if ((m.type == MT_EMPTY || m.type == MT_OPEN) && !strcmp(t, "tab")) {
                pb_putc(&text, '\t');
            }
        }

        for (k = 0; k < X->ncm; k++) {
            if (X->cm[k].wid == wid && X->cm[k].m0 && X->cm[k].m1) {
                at = &X->cm[k];
            }
        }

        for (k = 0; k < nex && last_para; k++) {
            if (ex[k].para == last_para) {
                int j;

                c.resolved = ex[k].done;

                for (j = 0; j < nmade && ex[k].parent; j++) {
                    if (made[j].para == ex[k].parent) {
                        c.parent = made[j].id;
                    }
                }
            }
        }

        c.text = text.p;
        c.text_len = (uint32_t)text.n;

        if (at && pd_doc_marker_get(d, at->m0, &c.range.start) == PD_OK &&
                pd_doc_marker_get(d, at->m1, &c.range.end) == PD_OK) {
            pd_comment_id id;

            if (pd_doc_comment_add(d, &c, &id) == PD_OK && last_para &&
                    !pd_grow((void**)&made, &capmade, (int64_t)nmade + 1, sizeof(*made))) {
                made[nmade].para = last_para;
                made[nmade].id = id;
                nmade++;
            }
        }

        pb_free(&text);
    }

    for (k = 0; k < X->ncm; k++) {
        pd_doc_marker_free(d, X->cm[k].m0);
        pd_doc_marker_free(d, X->cm[k].m1);
    }

    pd_doc_clear_undo(d);
    free(ex);
    free(made);
    free(xml);
}

pd_status pd_docx_import(pd_doc* d, const unsigned char* s, size_t n) {
    dxi X;
    pd_bld b;
    char* xml;
    size_t len = 0;
    pd_status st;

    memset(&X, 0, sizeof(X));
    X.margin_left = PD_PT(72);

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
    X.defaults.cp.mask |= PD_CP_KERNING;    /* nor kerns, unless w:kern says so */
    X.defaults.cp.kerning = 0;

    if ((xml = (char*)zip_read(&X.z, "docProps/core.xml", &len)) != NULL) {
        /* the document's properties, as the YAML a Markdown file's front matter is */
        pd_markup m;
        pd_buf y, val;
        int cur = -1;
        size_t k;

        memset(&y, 0, sizeof(y));
        memset(&val, 0, sizeof(val));
        mu_init(&m, xml, len, 0);

        while (mu_next(&m) != MT_END) {
            if (m.type == MT_OPEN) {
                cur = -1;

                for (k = 0; k < sizeof(CORE_PROPS) / sizeof(CORE_PROPS[0]); k++) {
                    if (!strcmp(mu_local(m.name), mu_local(CORE_PROPS[k].el))) {
                        cur = (int)k;
                        val.n = 0;
                    }
                }
            } else if (m.type == MT_TEXT && cur >= 0) {
                mu_decode(m.text, m.tlen, &val);
            } else if (m.type == MT_CLOSE && cur >= 0) {
                if (val.n) {
                    size_t q;

                    pb_printf(&y, "%s: \"", CORE_PROPS[cur].key);

                    for (q = 0; q < val.n; q++) {   /* one line, quotes escaped */
                        char c = val.p[q] == '\n' || val.p[q] == '\r' ? ' ' : val.p[q];

                        if (c == '"' || c == '\\') {
                            pb_putc(&y, '\\');
                        }

                        pb_putc(&y, c);
                    }

                    pb_puts(&y, "\"\n");
                }

                cur = -1;
            }
        }

        if (y.n) {
            pd_doc_set_metadata(d, y.p, y.n - 1);
        }

        pb_free(&y);
        pb_free(&val);
        free(xml);
    }

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
        read_table_styles(&X, xml, len);
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
        free(X.tstyles);
        free(X.notes_xml);
        free(X.notes);
        free(X.en_xml);
        free(X.en);
        return PD_ERR_FORMAT;
    }

    bld_init(&b, d);
    X.b = &b;
    read_fonts(&X);
    dw_parse(&X, xml, len, 0);

    while (b.ntables > 0) {
        bld_table_end(&b);
    }

    st = bld_finish(&b);

    /* fields pointing at bookmarks: at the bookmarks' paragraphs */
    {
        int i, k;

        for (i = 0; i < X.nrefs; i++) {
            for (k = 0; k < X.nbm && strcmp(X.bm[k].name, X.refs[i].name) != 0; k++) {
            }

            if (k < X.nbm) {
                pd_pos at;
                pd_range r;
                pd_inline o;

                at.block = X.refs[i].para;
                at.offset = X.refs[i].off;

                if (pd_doc_inline_at(d, at, &o) == PD_OK && o.kind == PD_INLINE_FIELD) {
                    char name[32];

                    snprintf(name, sizeof(name), "%s", o.name);
                    r.start = at;
                    r.end = at;
                    r.end.offset += 3;
                    o.target = X.bm[k].para;
                    o.source = NULL;
                    o.source_len = 0;
                    o.title = o.alt = NULL;
                    o.title_len = o.alt_len = 0;
                    snprintf(o.name, sizeof(o.name), "%s", name);

                    if (pd_doc_delete(d, r, NULL) == PD_OK) {
                        pd_doc_insert_inline(d, at, &o, NULL);
                    }
                }
            }
        }

        pd_doc_clear_undo(d);
    }

    read_comments(&X, d);
    free(X.cm);

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

    /* Word adds a paragraph's space after to the next one's before */
    {
        pd_block_info ri;
        pd_section_props sp;
        int32_t i;

        if (pd_doc_block_info(d, pd_doc_root(d), &ri) == PD_OK) {
            for (i = 0; i < ri.child_count; i++) {
                pd_block_id sec = pd_doc_child(d, pd_doc_root(d), i);

                if (pd_doc_section_props(d, sec, &sp) == PD_OK && !sp.add_spacing) {
                    sp.add_spacing = 1;
                    pd_doc_set_section_props(d, sec, &sp);
                }
            }

            pd_doc_clear_undo(d);
        }
    }

    free(xml);
    free(X.z.e);
    free(X.rels);
    free(X.styles);
    free(X.tstyles);
    free(X.notes_xml);
    free(X.notes);
    free(X.en_xml);
    free(X.en);
    return st;
}
