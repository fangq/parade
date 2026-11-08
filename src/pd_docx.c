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
#include "pd_preset.h"
#include "parade_layout.h"

/* parts kept whole with the document, to write again */
#define CHART_MIME "application/vnd.openxmlformats-officedocument.drawingml.chart+xml"
#define THEME_MIME "application/vnd.openxmlformats-officedocument.theme+xml"
#define XLSX_MIME "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"

/* v * m / d, to the nearest: what is read back converts to the same again */
#define SCALE(v, m, d) ((int64_t)(v) * (m) >= 0 ? ((int64_t)(v) * (m) + (d) / 2) / (d) : \
                        ((int64_t)(v) * (m) - (d) / 2) / (d))
#define TW(sp) ((int)SCALE(sp, 20, 65536))          /* sp -> twips */

/* a file name's extension, in any case */
static int ext_is(const char* ext, const char* want) {
    for (; *ext && *want; ext++, want++) {
        if (tolower((unsigned char)*ext) != *want) {
            return 0;
        }
    }

    return !*ext && !*want;
}
#define EMU(sp) ((long long)SCALE(sp, 12700, 65536)) /* sp -> EMU */
#define ZIP_MAX_ENTRY ((size_t)256 << 20)

/* ------------------------------------------------------------------ */
/* zip                                                                */
/* ------------------------------------------------------------------ */

static const char* num_fmt_name(int32_t f);
static int32_t num_fmt_of(const char* v);

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
    "xmlns:wpc=\"http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas\" "
    "xmlns:wp14=\"http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing\" "
    "xmlns:w15=\"http://schemas.microsoft.com/office/word/2012/wordml\" "
    "xmlns:a14=\"http://schemas.microsoft.com/office/drawing/2010/main\" "
    "xmlns:a15=\"http://schemas.microsoft.com/office/drawing/2012/main\" "
    "xmlns:a16=\"http://schemas.microsoft.com/office/drawing/2014/main\" "
    "xmlns:v=\"urn:schemas-microsoft-com:vml\" "
    "xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
    "xmlns:w10=\"urn:schemas-microsoft-com:office:word\" "
    "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\"";

/* the prefixes declared above, which a drawing's own XML, kept as it came, may use */
static int dx_prefix_declared(const char* p, size_t n) {
    static const char* const known[] = { "w", "r", "wp", "a", "pic", "mc", "wpg", "wps", "w14", "wpc", "wp14", "w15",
                                         "a14", "a15", "a16", "v", "o", "w10", "m", "xml"
                                       };
    size_t i;

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strlen(known[i]) == n && !memcmp(known[i], p, n)) {
            return 1;
        }
    }

    return 0;
}
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
    int nctl, nctl_ids;         /* content controls open in the paragraph; ids given */
    int nruby;                  /* phonetic guides open in the paragraph */
    struct {                    /* charts written: their parts, the workbook each has */
        pd_res_id chart, data;
        char rid[64];
        char link[300];
    } charts[64];
    int ncharts;
    pd_buf rels;                /* document.xml.rels entries beyond the fixed ones */
    pd_buf notes;               /* footnotes.xml body */
    pd_buf endnotes;            /* endnotes.xml body */
    int nlinks, nnotes, nendnotes;
    dmedia media[256];
    int nmedia, docpr;
    pd_list_id listmap[2048];   /* Parade list -> numId order */
    int nlistmap;
    int listkind[2048];
    pd_block_id hf[8];          /* header/footer stories written as parts */
    int hf_footer[8];
    int nhf;
    int hyph_auto;              /* the document hyphenates: settings.xml autoHyphenation */
    const pd_float_props* frame;    /* the frame whose paragraphs are being written (their w:framePr) */
    int move_range;             /* the id of the move's range being written */
    pd_block_id pending[8];     /* picture floats to anchor in the next paragraph */
    int npending;
    pd_block_id tblp;           /* the float the table being written floats as (w:tblpPr), 0 none */
    uint32_t page_color;        /* a section's page colour: the document's w:background */
    int page_break;             /* a page break opens the next paragraph */
    pd_block_id reft[1024];     /* paragraphs referred to that have no bookmark: given _RefPd<id> */
    int nreft;
    int even_odd;               /* some section has even-page headers */
    int mirror;                 /* some section mirrors its margins */
    int nbookmarks;
    pd_buf* hf_rels[8];
    struct dxcb {
        uint32_t off;
        int end;                /* 0: the range starts, 1: it ends */
        pd_comment_id id;
    } cb[64];                   /* comment range ends in the paragraph being written, in order */
    int ncb, cbi;
    int nrev;                   /* w:ins / w:del ids given out */
    const pd_char_props* obj_cp;    /* the run an inline picture is in, while it is written */
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

/* a theme colour reference as Word's attributes: w:themeColor (w:themeFill with fill), its tint or shade */
void pd_conv_theme_attr(pd_buf* o, uint32_t ref, int fill) {
    static const char* const names[PD_THEME_COLORS] = { "text1", "background1", "text2", "background2", "accent1",
                                                        "accent2", "accent3", "accent4", "accent5", "accent6",
                                                        "hyperlink", "followedHyperlink"
                                                      };
    int32_t slot, lm, lo;
    double mul, add;

    if (!pd_theme_color_parts(ref, &slot, &lm, &lo)) {
        return;
    }

    mul = lm / 100000.0;
    add = lo / 100000.0;
    pb_printf(o, fill ? " w:themeFill=\"%s\"" : " w:themeColor=\"%s\"", names[slot]);

    if (add > 0.0005) {     /* a tint (toward white), shaded after when the two do not add up to one */
        double sh = mul + add > 0.9995 ? 1 : mul + add, t = mul / (sh > 0 ? sh : 1);

        pb_printf(o, fill ? " w:themeFillTint=\"%02X\"" : " w:themeTint=\"%02X\"", (unsigned)(t * 255 + 0.5));

        if (sh < 1) {
            pb_printf(o, fill ? " w:themeFillShade=\"%02X\"" : " w:themeShade=\"%02X\"", (unsigned)(sh * 255 + 0.5));
        }
    } else if (mul < 0.9995) {
        pb_printf(o, fill ? " w:themeFillShade=\"%02X\"" : " w:themeShade=\"%02X\"",
                  (unsigned)((mul < 0 ? 0 : mul > 1 ? 1 : mul) * 255 + 0.5));
    }
}

#define dx_theme_attr pd_conv_theme_attr

static void dx_color(pd_buf* o, uint32_t rgb, uint32_t ref) {
    pb_printf(o, "<w:color w:val=\"%06X\"", (unsigned)(rgb & 0xFFFFFF));
    dx_theme_attr(o, ref, 0);
    pb_puts(o, "/>");
}

static void dx_shd(pd_buf* o, uint32_t rgb, uint32_t ref) {
    pb_printf(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"", (unsigned)(rgb & 0xFFFFFF));
    dx_theme_attr(o, ref, 1);
    pb_puts(o, "/>");
}

/* Word's sixteen highlight colours (ST_HighlightColor) */
static const struct {
    const char* name;
    uint32_t rgb;
} dx_highlights[16] = {
    { "yellow", 0xFFFF00 }, { "green", 0x00FF00 }, { "cyan", 0x00FFFF }, { "magenta", 0xFF00FF },
    { "blue", 0x0000FF }, { "red", 0xFF0000 }, { "darkBlue", 0x000080 }, { "darkCyan", 0x008080 },
    { "darkGreen", 0x008000 }, { "darkMagenta", 0x800080 }, { "darkRed", 0x800000 }, { "darkYellow", 0x808000 },
    { "darkGray", 0x808080 }, { "lightGray", 0xC0C0C0 }, { "black", 0x000000 }, { "white", 0xFFFFFF }
};

/* the highlight a run's background is, NULL when it is shading: one of the sixteen, not linked to the theme */
static const char* dx_highlight_name(uint32_t rgb, uint32_t ref) {
    int i;

    for (i = 0; i < 16 && rgb && !ref; i++) {
        if (dx_highlights[i].rgb == (rgb & 0xFFFFFF)) {
            return dx_highlights[i].name;
        }
    }

    return NULL;
}

/* the theme's fonts a family is linked to, as w:rFonts attributes */
static void dx_font_theme_attr(pd_buf* o, int32_t ft) {
    int t = PD_FONT_THEME_TEXT(ft), e = PD_FONT_THEME_EA(ft), c = PD_FONT_THEME_CS(ft);

    if (t) {
        pb_printf(o, " w:asciiTheme=\"%sHAnsi\" w:hAnsiTheme=\"%sHAnsi\"", t == 1 ? "major" : "minor",
                  t == 1 ? "major" : "minor");
    }

    if (e) {
        pb_printf(o, " w:eastAsiaTheme=\"%sEastAsia\"", e == 1 ? "major" : "minor");
    }

    if (c) {
        pb_printf(o, " w:cstheme=\"%sBidi\"", c == 1 ? "major" : "minor");
    }
}

static void dx_rpr_set(pd_buf* o, const pd_char_props* c);

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
    } else if ((strcmp(c->family, b->family) && c->family[0]) || strcmp(c->family_ea, b->family_ea) ||
               strcmp(c->family_cs, b->family_cs) || c->font_theme != b->font_theme) {
        int lat = (strcmp(c->family, b->family) || PD_FONT_THEME_TEXT(c->font_theme) !=
                   PD_FONT_THEME_TEXT(b->font_theme)) && c->family[0];

        pb_puts(o, "<w:rFonts");

        if (lat) {
            pb_puts(o, " w:ascii=\"");
            xesc(o, c->family, strlen(c->family));
            pb_puts(o, "\" w:hAnsi=\"");
            xesc(o, c->family, strlen(c->family));
            pb_putc(o, '"');
        }

        if (strcmp(c->family_ea, b->family_ea) && c->family_ea[0]) {
            pb_puts(o, " w:eastAsia=\"");
            xesc(o, c->family_ea, strlen(c->family_ea));
            pb_putc(o, '"');
        }

        if (strcmp(c->family_cs, b->family_cs) && c->family_cs[0]) {
            pb_puts(o, " w:cs=\"");
            xesc(o, c->family_cs, strlen(c->family_cs));
            pb_putc(o, '"');
        }

        dx_font_theme_attr(o, PD_FONT_THEME(lat ? PD_FONT_THEME_TEXT(c->font_theme) : 0,
                                            c->family_ea[0] ? PD_FONT_THEME_EA(c->font_theme) : 0,
                                            c->family_cs[0] ? PD_FONT_THEME_CS(c->font_theme) : 0));
        pb_puts(o, "/>");
    }

    if (c->weight >= 600 && b->weight < 600) {
        pb_puts(o, "<w:b/>");
    } else if (c->weight < 600 && b->weight >= 600) {
        pb_puts(o, "<w:b w:val=\"0\"/>");
    }

    if (c->weight_cs != b->weight_cs && c->weight_cs) {
        pb_puts(o, c->weight_cs >= 600 ? "<w:bCs/>" : "<w:bCs w:val=\"0\"/>");
    }

    if (c->italic && !b->italic) {
        pb_puts(o, "<w:i/>");
    }

    if (c->italic_cs != b->italic_cs && c->italic_cs >= 0) {
        pb_puts(o, c->italic_cs ? "<w:iCs/>" : "<w:iCs w:val=\"0\"/>");
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

    if (c->color != b->color || c->color_theme != b->color_theme) {
        dx_color(o, c->color, c->color_theme);
    }

    if (c->letter_space != b->letter_space) {
        pb_printf(o, "<w:spacing w:val=\"%d\"/>", TW(c->letter_space));
    }

    if ((c->hscale > 0 ? c->hscale : 1000) != (b->hscale > 0 ? b->hscale : 1000)) {
        pb_printf(o, "<w:w w:val=\"%d\"/>", (int)((c->hscale > 0 ? c->hscale : 1000) + 5) / 10);
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

    if (c->size_cs != b->size_cs && c->size_cs > 0) {
        pb_printf(o, "<w:szCs w:val=\"%d\"/>", (int)SCALE(c->size_cs, 2, 65536));
    }

    if (c->background && (c->background != b->background || c->background_theme != b->background_theme) &&
            dx_highlight_name(c->background, c->background_theme)) {
        pb_printf(o, "<w:highlight w:val=\"%s\"/>", dx_highlight_name(c->background, c->background_theme));
    }

    if (c->underline != b->underline || (rstyle && !strcmp(rstyle, "Hyperlink") && !c->underline)) {
        /* a link not underlined says so: the Hyperlink style it is written in underlines */
        pb_printf(o, "<w:u w:val=\"%s\"/>", dx_u_name(c->underline));
    }

    if (c->background && (c->background != b->background || c->background_theme != b->background_theme) &&
            !dx_highlight_name(c->background, c->background_theme)) {
        dx_shd(o, c->background, c->background_theme);
    }

    if (c->shift == PD_SHIFT_SUPER) {
        pb_puts(o, "<w:vertAlign w:val=\"superscript\"/>");
    } else if (c->shift == PD_SHIFT_SUB) {
        pb_puts(o, "<w:vertAlign w:val=\"subscript\"/>");
    }

    if (strcmp(c->lang, b->lang) && c->lang[0]) {
        pb_puts(o, "<w:lang w:val=\"");
        xesc(o, c->lang, strlen(c->lang));
        pb_puts(o, "\"/>");
    }

    {
        /* a formatting change: what the run had */
        pd_revision rv;

        if (c->revision && pd_doc_revision_get(x->d, c->revision, &rv) == PD_OK && rv.kind == PD_REV_FORMAT) {
            pb_printf(o, "<w:rPrChange w:id=\"%d\" w:author=\"", ++x->nrev);
            xesc(o, rv.author, strlen(rv.author));

            if (rv.date[0]) {
                pb_puts(o, "\" w:date=\"");
                xesc(o, rv.date, strlen(rv.date));
            }

            pb_puts(o, "\"><w:rPr>");
            dx_rpr_set(o, &rv.old);
            pb_puts(o, "</w:rPr></w:rPrChange>");
        }
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

/* a field's instruction as w:instrText runs, a field nested in it (U+FFF9 instruction U+FFFA result U+FFFB) as
   a field of its own there */
static void dx_field_code(dxo* x, const pd_char_props* cp, const char* s, size_t n) {
    static const char* const fc[3] = { "begin", "separate", "end" };
    pd_buf* o = x->o;
    size_t i = 0, j;
    int result = 0, mark;

    while (i < n) {
        for (j = i; j + 2 < n && !(s[j] == '\xEF' && s[j + 1] == '\xBF' && s[j + 2] >= '\xB9' && s[j + 2] <= '\xBB');
                j++) {
        }

        j = j + 2 < n ? j : n;

        if (j > i) {
            pb_puts(o, "<w:r>");
            dx_rpr(x, cp, &x->base, NULL);
            pb_puts(o, result ? "<w:t xml:space=\"preserve\">" : "<w:instrText xml:space=\"preserve\">");
            xesc(o, s + i, j - i);
            pb_puts(o, result ? "</w:t></w:r>" : "</w:instrText></w:r>");
        }

        if (j < n) {
            mark = (unsigned char)s[j + 2] - 0xB9;
            result = mark == 1;
            pb_puts(o, "<w:r>");
            dx_rpr(x, cp, &x->base, NULL);
            pb_printf(o, "<w:fldChar w:fldCharType=\"%s\"/></w:r>", fc[mark]);
        }

        i = j + 3;
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
             strstr(mime, "png") ? "png" : strstr(mime, "gif") ? "gif" : strstr(mime, "emf") ? "emf" :
             strstr(mime, "wmf") ? "wmf" : strstr(mime, "bmp") ? "bmp" : strstr(mime, "tiff") ? "tiff" :
             strstr(mime, "webp") ? "webp" : "jpeg");
    pb_printf(&x->rels, "<Relationship Id=\"rIdm%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
              "relationships/image\" Target=\"media/%s\"/>", x->nmedia + 1, x->media[x->nmedia].name);
    *rid_name = x->media[x->nmedia].name;
    return x->nmedia++;
}

static void dx_block(dxo* x, pd_block_id id);
static void dx_para(dxo* x, pd_block_id p, const char* extra_ppr);
static void dx_sid(const pd_doc* d, pd_style_id sid, char* out, size_t cap);
static void dx_rules(pd_buf* o, const char* tag, int set, int on, pd_sp w, const pd_sp* ew, uint32_t c, uint32_t ref,
                     int table);
static void dx_ppr_head(pd_buf* o, const pd_para_props* pp, uint32_t m);
static void dx_ppr_tail(pd_buf* o, const pd_para_props* pp, uint32_t m, int hyph_auto, const pd_para_props* base);

/* a picture as a run: inline, or anchored where a float is (fp) -- beside the text on the side the float
   wraps on, or across the column */
static void dx_drawing_group(dxo* x, const void* data, size_t len, long long cx, long long cy);
static int dx_kept_fallback(dxo* x, const void* data, size_t len);
static void dx_vml_fallback(dxo* x, const void* data, size_t len, long long cx, long long cy,
                            const pd_float_props* fp);
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
    char crop[96] = "";

    int chart = 0, wpc = 0;

    if (ob->crop[0] || ob->crop[1] || ob->crop[2] || ob->crop[3]) {    /* cut off at its sides */
        snprintf(crop, sizeof(crop), "<a:srcRect l=\"%d\" t=\"%d\" r=\"%d\" b=\"%d\"/>", (int)ob->crop[0],
                 (int)ob->crop[1], (int)ob->crop[2], (int)ob->crop[3]);
    }

    if (group) {    /* a metafile played into a drawing: the metafile itself goes back; a chart: the chart */
        pj_doc* jd = pj_parse(data, len, 0, NULL);
        const pj_node* jr = jd ? pj_root(jd) : NULL, *n;
        pd_res_id src = jr ? (pd_res_id)pj_int_or(pj_get(jr, "src"), 0) : 0;
        pd_res_id cres = jr ? (pd_res_id)pj_int_or(pj_get(jr, "chart"), 0) : 0;

        n = jr ? pj_get(jr, "kind") : NULL;     /* a canvas kept as it came goes back as a canvas */
        wpc = n && n->type == PJ_STR && n->len == 3 && !memcmp(n->s, "wpc", 3) && pj_get(jr, "xml");

        if (cres && x->ncharts < 64) {
            chart = ++x->ncharts;
            x->charts[chart - 1].chart = cres;
            x->charts[chart - 1].data = (pd_res_id)pj_int_or(pj_get(jr, "data"), 0);
            n = pj_get(jr, "dataRid");
            snprintf(x->charts[chart - 1].rid, sizeof(x->charts[0].rid), "%.*s", n && n->type == PJ_STR ? (int)n->len : 0,
                     n && n->type == PJ_STR ? n->s : "");
            n = pj_get(jr, "dataLink");
            snprintf(x->charts[chart - 1].link, sizeof(x->charts[0].link), "%.*s",
                     n && n->type == PJ_STR ? (int)n->len : 0, n && n->type == PJ_STR ? n->s : "");
            pb_printf(&x->rels, "<Relationship Id=\"rIdch%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/"
                      "2006/relationships/chart\" Target=\"charts/chart%d.xml\"/>", chart, chart);
            group = 0;
        }

        pj_free(jd);

        if (src && !chart) {
            group = 0;
            pic = src;
        }
    }

    m = group || chart ? 0 : dx_media(x, pic, &name);
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
    pb_puts(o, "<w:r>");

    if (x->obj_cp) {    /* the picture's run as formatted: its size sets its line's */
        dx_rpr(x, x->obj_cp, &x->base, NULL);
    }

    mark = o->n;    /* where the run's content starts: after its properties */

    if (!fp) {
        pb_printf(o, "<w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                  "<wp:extent cx=\"%lld\" cy=\"%lld\"/>", cx, cy);
    } else {
        long long gap = EMU(fp->gap);
        int off = (fp->placement & PD_PLACE_OFFSET) && fp->wrap != PD_WRAP_NONE;

        pb_printf(o, "<w:drawing><wp:anchor distT=\"0\" distB=\"0\" distL=\"%lld\" distR=\"%lld\" "
                  "simplePos=\"0\" relativeHeight=\"%d\" behindDoc=\"%d\" locked=\"0\" layoutInCell=\"1\" "
                  "allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\">",
                  gap, gap, x->docpr, fp->wrap == PD_WRAP_BEHIND);

        if (off) {
            pb_printf(o, "<wp:posOffset>%lld</wp:posOffset>", EMU(fp->offset_x));
        } else {
            pb_printf(o, "<wp:align>%s</wp:align>", fp->wrap == PD_WRAP_LEFT ? "left" : fp->wrap == PD_WRAP_RIGHT ?
                      "right" : "center");
        }

        /* at an offset, which side the text is on says which side the picture is */
        pb_printf(o, "</wp:positionH><wp:positionV relativeFrom=\"%s\"><wp:posOffset>%lld</wp:posOffset>"
                  "</wp:positionV><wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>%s",
                  fp->offset_from == PD_FROM_PAGE ? "page" : fp->offset_from == PD_FROM_MARGIN ? "margin" : "paragraph",
                  EMU(fp->offset_y), cx, cy, fp->wrap >= PD_WRAP_FRONT ? "<wp:wrapNone/>" :
                  fp->wrap == PD_WRAP_NONE ? "<wp:wrapTopAndBottom/>" : !off ?
                  "<wp:wrapSquare wrapText=\"bothSides\"/>" : fp->wrap == PD_WRAP_LEFT ?
                  "<wp:wrapSquare wrapText=\"right\"/>" : "<wp:wrapSquare wrapText=\"left\"/>");
    }

    if (group) {    /* Word keeps a group in markup it alone reads, and says so: after the run's opening */
        static const char mc_g[] = "<mc:AlternateContent><mc:Choice Requires=\"wpg\">";
        static const char mc_c[] = "<mc:AlternateContent><mc:Choice Requires=\"wpc\">";
        const char* mc = wpc ? mc_c : mc_g;
        size_t at = mark, k = strlen(mc), tail = o->n - at;


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

    if (chart) {
        pb_printf(o, "/>%s<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/chart\">"
                  "<c:chart xmlns:c=\"http://schemas.openxmlformats.org/drawingml/2006/chart\" r:id=\"rIdch%d\"/>"
                  "</a:graphicData></a:graphic>%s</w:drawing></w:r>", fp ? "" : "<wp:cNvGraphicFramePr/>", chart,
                  fp ? "</wp:anchor>" : "</wp:inline>");
        return;
    }

    if (group) {
        pb_printf(o, "/>%s<a:graphic><a:graphicData uri=\"http://schemas.microsoft.com/office/word/2010/%s\">",
                  fp ? "<wp:cNvGraphicFramePr/>" : "", wpc ? "wordprocessingCanvas" : "wordprocessingGroup");
        dx_drawing_group(x, data, len, cx, cy);
        pb_printf(o, "</a:graphicData></a:graphic>%s</w:drawing></mc:Choice>", fp ? "</wp:anchor>" : "</wp:inline>");
        if (!dx_kept_fallback(x, data, len) && wpc) {   /* the VML beside it, for readers without DrawingML: kept */
            dx_vml_fallback(x, data, len, cx, cy, fp);  /* as it came, or made from what the canvas draws */
        }

        pb_puts(o, "</mc:AlternateContent></w:r>");
        return;
    }

    pb_printf(o, "/>%s<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
              "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
              "<pic:blipFill><a:blip r:embed=\"rIdm%d\"/>%s<a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
              "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData>"
              "</a:graphic>%s</w:drawing></w:r>", fp ? "<wp:cNvGraphicFramePr/>" : "", x->docpr, name, m + 1, crop, cx, cy,
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
/* a picture in a text box's line */
static void dx_txbx_picture(dxo* x, const pj_node* r) {
    pd_buf* o = x->o;
    pd_res_id cr = (pd_res_id)jnum(r, "img");
    long long cx = EMU(jnum(r, "w")), cy = EMU(jnum(r, "h"));
    const char* name, *cm = "";
    const void* cd = NULL;
    size_t cn = 0;
    int m;

    if (pd_doc_resource(x->d, cr, &cm, &cd, &cn) == PD_OK && strcmp(cm, PD_DRAWING_MIME) == 0) {
        pj_doc* jd = pj_parse(cd, cn, 0, NULL);     /* a metafile: the metafile itself */

        cr = jd ? (pd_res_id)pj_int_or(pj_get(pj_root(jd), "src"), 0) : 0;
        pj_free(jd);
    }

    m = cr ? dx_media(x, cr, &name) : -1;

    if (m < 0 || cx <= 0 || cy <= 0) {
        return;
    }

    x->docpr++;
    pb_printf(o, "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%lld\" "
              "cy=\"%lld\"/><wp:docPr id=\"%d\" name=\"Picture %d\"/><wp:cNvGraphicFramePr/><a:graphic><a:graphicData "
              "uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" "
              "name=\"%s\"/><pic:cNvPicPr/></pic:nvPicPr><pic:blipFill><a:blip r:embed=\"rIdm%d\"/><a:stretch><a:fillRect/>"
              "</a:stretch></pic:blipFill><pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>"
              "</wp:inline></w:drawing></w:r>", cx, cy, x->docpr, x->docpr, x->docpr, name, m + 1, cx, cy);
}

static void dx_txbx_table(dxo* x, const pj_node* tbl, int depth);

/* a text box's paragraphs (and tables), as the drawing has them */
static void dx_txbx_paras(dxo* x, const pj_node* paras, int depth) {
    pd_buf* o = x->o;
    const pj_node* p, *r;

    for (p = paras ? paras->child : NULL; p; p = p->next) {
        int a = (int)jnum(p, "a");

        if (pj_get(p, "table")) {
            if (depth < 4) {
                dx_txbx_table(x, pj_get(p, "table"), depth + 1);
            }

            continue;
        }

        pb_printf(o, "<w:p><w:pPr><w:spacing w:after=\"0\"/><w:jc w:val=\"%s\"/></w:pPr>", a == PD_ALIGN_CENTER ?
                  "center" : a == PD_ALIGN_RIGHT ? "right" : a == PD_ALIGN_JUSTIFY ? "both" : "left");

        for (r = pj_get(p, "runs") ? pj_get(p, "runs")->child : NULL; r; r = r->next) {
            const pj_node* t = pj_get(r, "t"), *f = pj_get(r, "f");

            if (pj_get(r, "img")) {
                dx_txbx_picture(x, r);
                continue;
            }

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

}

/* a text box's table: its grid, its rows of cells, their spans, shading and rules */
static void dx_txbx_table(dxo* x, const pj_node* tbl, int depth) {
    pd_buf* o = x->o;
    const pj_node* q, *row, *cell;
    static const char* const side[] = { "top", "left", "bottom", "right", "insideH", "insideV" };
    long long rule = jnum(tbl, "rules");
    int jc = (int)jnum(tbl, "jc"), sz = (int)SCALE(rule, 8, 65536), k;

    pb_printf(o, "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/>%s<w:tblBorders>", jc == PD_ALIGN_CENTER ?
              "<w:jc w:val=\"center\"/>" : jc == PD_ALIGN_RIGHT ? "<w:jc w:val=\"right\"/>" : "");

    for (k = 0; k < 6; k++) {
        pb_printf(o, rule > 0 ? "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"auto\"/>" :
                  "<w:%s w:val=\"nil\"/>", side[k], sz);
    }

    pb_puts(o, "</w:tblBorders><w:tblLayout w:type=\"fixed\"/></w:tblPr><w:tblGrid>");

    for (q = pj_get(tbl, "cols") ? pj_get(tbl, "cols")->child : NULL; q; q = q->next) {
        pb_printf(o, "<w:gridCol w:w=\"%d\"/>", (int)pj_int_or(q, 0));
    }

    pb_puts(o, "</w:tblGrid>");

    for (row = pj_get(tbl, "rows") ? pj_get(tbl, "rows")->child : NULL; row; row = row->next) {
        pb_puts(o, "<w:tr>");

        for (cell = row->child; cell; cell = cell->next) {
            int span = (int)jnum(cell, "span");
            uint32_t bg = (uint32_t)jnum(cell, "bg");

            pb_puts(o, "<w:tc><w:tcPr>");

            if (span > 1) {
                pb_printf(o, "<w:gridSpan w:val=\"%d\"/>", span);
            }

            if (bg) {
                pb_printf(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06X\"/>", (unsigned)(bg & 0xFFFFFF));
            }

            pb_puts(o, "</w:tcPr>");
            dx_txbx_paras(x, pj_get(cell, "paras"), depth);

            if (!pj_get(cell, "paras") || !pj_get(cell, "paras")->child) {
                pb_puts(o, "<w:p/>");   /* a cell ends in a paragraph */
            }

            pb_puts(o, "</w:tc>");
        }

        pb_puts(o, "</w:tr>");
    }

    pb_puts(o, "</w:tbl><w:p/>");   /* and so does a text box: after its table */
}

static void dx_flush_anchors(dxo* x);

/* A story's blocks as the inside of a w:txbxContent, written in the middle of a run: the paragraph under way is
   left as it was found. An empty story is one empty paragraph, as Word wants one. */
static void dx_story_content(dxo* x, pd_block_id story) {
    pd_block_id para = x->para;
    pd_char_props base = x->base;
    int in_link = x->in_link, in_note = x->in_note, nctl = x->nctl, nruby = x->nruby, page_break = x->page_break;
    int ncb = x->ncb, cbi = x->cbi, npending = x->npending, k;
    pd_block_id pending[8];
    struct dxcb cb[64];
    pd_block_info bi;

    memcpy(cb, x->cb, sizeof(cb));
    memcpy(pending, x->pending, sizeof(pending));
    x->in_link = x->in_note = x->nctl = x->nruby = x->page_break = 0;
    x->ncb = x->cbi = 0;
    x->npending = 0;

    if (pd_doc_block_info(x->d, story, &bi) == PD_OK && bi.child_count > 0) {
        for (k = 0; k < bi.child_count; k++) {
            dx_block(x, pd_doc_child(x->d, story, k));
        }

        dx_flush_anchors(x);
    } else {
        pb_puts(x->o, "<w:p/>");
    }

    x->para = para;
    x->base = base;
    x->in_link = in_link;
    x->in_note = in_note;
    x->nctl = nctl;
    x->nruby = nruby;
    x->page_break = page_break;
    x->ncb = ncb;
    x->cbi = cbi;
    memcpy(x->cb, cb, sizeof(cb));
    memcpy(x->pending, pending, sizeof(pending));
    x->npending = npending;
}

/* A drawing's own XML, kept as it came from a .docx, written back as it was: its references to pictures (r:embed,
   r:link, r:id) renamed to this package's, the rest byte for byte. */
static void dx_kept_xml(dxo* x, const char* s, size_t n, const pj_node* rels, const pj_node* styles,
                        const pd_block_id* stories, int nstories) {
    static const char* const styled[] = { "w:pStyle w:val=\"", "w:rStyle w:val=\"", "w:tblStyle w:val=\"" };
    pd_buf* o = x->o;
    size_t i = 0, from = 0, j;
    int next_story = 0;

    while (i + 3 < n) {
        /* a text box's content: its story's, as edited, in the order the boxes come */
        if (s[i] == '<' && next_story < nstories && i + 16 < n && !memcmp(s + i, "<w:txbxContent", 14) &&
                (s[i + 14] == '>' || s[i + 14] == ' ')) {
            const char* gt = memchr(s + i, '>', n - i);
            size_t at, depth = 1;

            for (at = gt ? (size_t)(gt + 1 - s) : n; at + 15 < n && depth > 0; at++) {     /* its closing tag */
                if (!memcmp(s + at, "<w:txbxContent", 14) && (s[at + 14] == '>' || s[at + 14] == ' ')) {
                    depth++;
                } else if (!memcmp(s + at, "</w:txbxContent>", 16) && --depth == 0) {
                    break;
                }
            }

            if (gt && depth == 0) {
                pb_put(o, s + from, (size_t)(gt + 1 - (s + from)));
                dx_story_content(x, stories[next_story++]);
                from = at;
                i = at;
                continue;
            }
        }

        for (j = 0; j < sizeof(styled) / sizeof(styled[0]); j++) {     /* a style, by the id written for it */
            size_t sl = strlen(styled[j]);

            if (i + sl < n && !memcmp(s + i, styled[j], sl)) {
                const char* v0 = s + i + sl, *v1 = memchr(v0, '"', n - (size_t)(v0 - s));
                const pj_node* st;
                char id[64], out[80];

                if (v1 && (size_t)(v1 - v0) < sizeof(id)) {
                    memcpy(id, v0, (size_t)(v1 - v0));
                    id[v1 - v0] = '\0';
                    st = styles ? pj_get(styles, id) : NULL;

                    if (st) {
                        dx_sid(x->d, (pd_style_id)pj_int_or(st, 0), out, sizeof(out));
                        pb_put(o, s + from, (size_t)(v0 - (s + from)));
                        pb_puts(o, out);
                        from = (size_t)(v1 - s);
                    }
                }

                i += sl;
                break;
            }
        }

        if (((s[i] == 'r' && s[i + 1] == ':' && (!strncmp(s + i + 2, "embed=\"", 7) || !strncmp(s + i + 2, "link=\"", 6) ||
                                                   !strncmp(s + i + 2, "id=\"", 4))) ||
                (s[i] == 'o' && s[i + 1] == ':' && !strncmp(s + i + 2, "relid=\"", 7))) &&
                (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t' || s[i - 1] == '\n')) {
            const char* q = memchr(s + i, '"', n - i);
            const char* e = q ? memchr(q + 1, '"', n - (size_t)(q + 1 - s)) : NULL;
            const pj_node* r;
            char rid[64];
            const char* name;
            int m = -1;

            if (q && e && (size_t)(e - q - 1) < sizeof(rid)) {
                memcpy(rid, q + 1, (size_t)(e - q - 1));
                rid[e - q - 1] = '\0';
                r = rels ? pj_get(rels, rid) : NULL;
                m = r ? dx_media(x, (pd_res_id)pj_int_or(r, 0), &name) : -1;
            }

            if (m >= 0) {
                pb_put(o, s + from, (size_t)(q + 1 - (s + from)));
                pb_printf(o, "rIdm%d", m + 1);
                from = (size_t)(e - s);
                i = from;
                continue;
            }
        }

        i++;
    }

    pb_put(o, s + from, n - from);
}

/* the stories of a drawing's text boxes, in the order its items have them (the order of their w:txbxContent) */
static int dx_drawing_stories(const pj_node* root, pd_block_id* out, int cap) {
    const pj_node* it, *items = root ? pj_get(root, "items") : NULL;
    int n = 0;

    for (it = items ? items->child : NULL; it && n < cap; it = it->next) {
        if (pj_get(it, "story")) {
            out[n++] = (pd_block_id)pj_int_or(pj_get(it, "story"), 0);
        }
    }

    return n;
}

/* a kept drawing's mc:Fallback, its references renamed as the drawing's are; nothing when there is none */
static int dx_kept_fallback(dxo* x, const void* data, size_t len) {
    pj_doc* doc = pj_parse(data, len, 0, NULL);
    const pj_node* root = doc ? pj_root(doc) : NULL, *fb = root ? pj_get(root, "fallback") : NULL;
    int done = 0;

    if (fb && fb->type == PJ_STR && fb->len > 0 && pj_get(root, "xml")) {
        pd_block_id st[128];
        int nst = dx_drawing_stories(root, st, 128);

        dx_kept_xml(x, fb->s, fb->len, pj_get(root, "rels"), pj_get(root, "styles"), st, nst);
        done = 1;
    }

    pj_free(doc);
    return done;
}

/* sp as the hundredths of a point a VML fallback's group counts in */
static long long vml_u(long long sp) {
    return sp * 100 / 65536;
}

/* a VML shape's fill and stroke from a drawing's colours (0: none) */
static void vml_paint(pd_buf* o, uint32_t fill, uint32_t line, long long lw) {
    if (fill & 0xFF000000u) {
        pb_printf(o, " fillcolor=\"#%06X\"", (unsigned)(fill & 0xFFFFFF));
    } else {
        pb_puts(o, " filled=\"f\"");
    }

    if (line & 0xFF000000u) {
        pb_printf(o, " strokecolor=\"#%06X\" strokeweight=\"%.2fpt\"", (unsigned)(line & 0xFFFFFF),
                  (double)lw / 65536);
    } else {
        pb_puts(o, " stroked=\"f\"");
    }
}

/* the alpha of a VML shape's fill, when it is not opaque */
static void vml_opacity(pd_buf* o, uint32_t fill) {
    unsigned a = fill >> 24;

    if (a && a < 255) {
        pb_printf(o, "<v:fill opacity=\"%.3f\"/>", a / 255.0);
    }
}

/* A canvas's mc:Fallback made from what it draws, for the readers that show a Word canvas only as VML
   (LibreOffice): one v:group counting in hundredths of a point, its boxes, ellipses and paths, its
   pictures and its text boxes with their stories, where the drawing has them. */
static void dx_vml_fallback(dxo* x, const void* data, size_t len, long long cx, long long cy,
                            const pd_float_props* fp) {
    pd_buf* o = x->o;
    pj_doc* doc = pj_parse(data, len, 0, NULL);
    const pj_node* root = doc ? pj_root(doc) : NULL, *items = root ? pj_get(root, "items") : NULL, *it;
    long long W = root ? vml_u(jnum(root, "w")) : 0, H = root ? vml_u(jnum(root, "h")) : 0;

    if (W <= 0 || H <= 0) {
        pj_free(doc);
        return;
    }

    pb_printf(o, "<mc:Fallback><w:pict><v:group editas=\"canvas\" coordorigin=\"0,0\" coordsize=\"%lld,%lld\" "
              "style=\"", W, H);

    if (fp) {   /* a float: beside the text as the drawing is */
        int off = (fp->placement & PD_PLACE_OFFSET) && fp->wrap != PD_WRAP_NONE;

        pb_puts(o, "position:absolute;");

        if (off) {
            pb_printf(o, "margin-left:%.2fpt;", (double)fp->offset_x / 65536);
        } else {
            pb_printf(o, "mso-position-horizontal:%s;", fp->wrap == PD_WRAP_LEFT ? "left" : fp->wrap == PD_WRAP_RIGHT ?
                      "right" : "center");
        }

        if (fp->offset_y) {
            pb_printf(o, "margin-top:%.2fpt;", (double)fp->offset_y / 65536);
        }

        pb_printf(o, "width:%.2fpt;height:%.2fpt;z-index:%d;mso-position-horizontal-relative:text;"
                  "mso-position-vertical-relative:%s\">", cx / 12700.0, cy / 12700.0,
                  fp->wrap == PD_WRAP_BEHIND ? -x->docpr : x->docpr, fp->offset_from == PD_FROM_PAGE ? "page" :
                  fp->offset_from == PD_FROM_MARGIN ? "margin" : "paragraph");
        pb_puts(o, fp->wrap >= PD_WRAP_FRONT ? "" : fp->wrap == PD_WRAP_NONE ? "<w10:wrap type=\"topAndBottom\"/>" :
                "<w10:wrap type=\"square\"/>");
    } else {
        pb_printf(o, "width:%.2fpt;height:%.2fpt;mso-position-horizontal-relative:char;"
                  "mso-position-vertical-relative:line\">", cx / 12700.0, cy / 12700.0);
    }

    for (it = items ? items->child : NULL; it; it = it->next) {
        long long ix = vml_u(jnum(it, "x")), iy = vml_u(jnum(it, "y")), iw = vml_u(jnum(it, "w")),
                  ih = vml_u(jnum(it, "h"));
        uint32_t fill = (uint32_t)jnum(it, "fill"), line = (uint32_t)jnum(it, "line");
        char box[160];

        snprintf(box, sizeof(box), "position:absolute;left:%lld;top:%lld;width:%lld;height:%lld", ix, iy, iw, ih);

        if (pj_get(it, "img")) {
            const char* name;
            pd_res_id cr = (pd_res_id)jnum(it, "img");
            const char* cm = "";
            const void* cd = NULL;
            size_t cn = 0;
            int m;

            if (pd_doc_resource(x->d, cr, &cm, &cd, &cn) == PD_OK && strcmp(cm, PD_DRAWING_MIME) == 0) {
                pj_doc* jd = pj_parse(cd, cn, 0, NULL);    /* a metafile in the canvas: the metafile itself */

                cr = jd ? (pd_res_id)pj_int_or(pj_get(pj_root(jd), "src"), 0) : 0;
                pj_free(jd);
            }

            m = cr ? dx_media(x, cr, &name) : -1;

            if (m >= 0) {
                pb_printf(o, "<v:rect style=\"%s\" filled=\"f\" stroked=\"f\"><v:imagedata r:id=\"rIdm%d\" o:title=\"\"/>"
                          "</v:rect>", box, m + 1);
            }
        } else if (pj_get(it, "shape")) {
            const pj_node* sh = pj_get(it, "shape");
            int ell = sh->type == PJ_STR && sh->len == 7 && !memcmp(sh->s, "ellipse", 7);
            int rr = sh->type == PJ_STR && sh->len == 9 && !memcmp(sh->s, "roundRect", 9);

            pb_printf(o, "<v:%s style=\"%s\"%s", ell ? "oval" : rr ? "roundrect" : "rect", box,
                      rr ? " arcsize=\"10923f\"" : "");
            vml_paint(o, fill, line, jnum(it, "lw"));
            pb_putc(o, '>');
            vml_opacity(o, fill);
            pb_printf(o, "</v:%s>", ell ? "oval" : rr ? "roundrect" : "rect");
        } else if (pj_get(it, "path")) {    /* in the group's own units: a shape the size of the canvas */
            const pj_node* q;
            int start = 1, closed = (int)jnum(it, "closed"), any = 0;

            pb_printf(o, "<v:shape style=\"position:absolute;left:0;top:0;width:%lld;height:%lld\" coordsize=\"%lld,%lld\" "
                      "path=\"", W, H, W, H);

            for (q = pj_get(it, "path")->child; q && q->next; q = q->next->next) {
                long long px = pj_int_or(q, 0), py = pj_int_or(q->next, 0);

                if (px == INT32_MIN) {
                    pb_puts(o, !start && closed ? "x" : "");
                    start = 1;
                    continue;
                }

                /* as Word writes it, the points of a run of lines after one l, all by commas */
                pb_printf(o, "%s%lld,%lld", start ? "m" : any == 1 ? "l" : ",", vml_u(px), vml_u(py));
                any = start ? 1 : 2;
                start = 0;
            }

            pb_printf(o, "%se\"", !start && closed ? "x" : "");
            vml_paint(o, closed ? fill : 0, line, jnum(it, "lw"));
            pb_putc(o, '>');
            vml_opacity(o, closed ? fill : 0);
            pb_puts(o, "</v:shape>");
        } else if (pj_get(it, "story") || pj_get(it, "text")) {
            const pj_node* ins = pj_get(it, "ins"), *an = pj_get(it, "anchor");
            const char* va = an && an->type == PJ_STR && an->len >= 3 && !memcmp(an->s, "ctr", 3) ? "middle" :
                              an && an->type == PJ_STR && an->len >= 1 && an->s[0] == 'b' ? "bottom" : "top";

            pb_printf(o, "<v:rect style=\"%s;v-text-anchor:%s\" filled=\"f\" stroked=\"f\"><v:textbox inset=\"%.2fpt,%.2fpt,"
                      "%.2fpt,%.2fpt\"><w:txbxContent>", box, va, (double)pj_int_or(pj_at(ins, 0), 0) / 65536,
                      (double)pj_int_or(pj_at(ins, 1), 0) / 65536, (double)pj_int_or(pj_at(ins, 2), 0) / 65536,
                      (double)pj_int_or(pj_at(ins, 3), 0) / 65536);

            if (pj_get(it, "story")) {
                dx_story_content(x, (pd_block_id)pj_int_or(pj_get(it, "story"), 0));
            } else {
                dx_txbx_paras(x, pj_get(it, "text"), 0);
            }

            pb_puts(o, "</w:txbxContent></v:textbox></v:rect>");
        }
    }

    pb_puts(o, "</v:group></w:pict></mc:Fallback>");
    pj_free(doc);
}

static void dx_drawing_group(dxo* x, const void* data, size_t len, long long cx, long long cy) {
    pd_buf* o = x->o;
    pj_doc* doc = pj_parse(data, len, 0, NULL);
    const pj_node* root = doc ? pj_root(doc) : NULL, *items = root ? pj_get(root, "items") : NULL, *it;
    const pj_node* xml = root ? pj_get(root, "xml") : NULL;
    int id = 1;

    if (xml && xml->type == PJ_STR && xml->len > 0) {
        pd_block_id st[128];
        int nst = dx_drawing_stories(root, st, 128);

        dx_kept_xml(x, xml->s, xml->len, pj_get(root, "rels"), pj_get(root, "styles"), st, nst);
        pj_free(doc);
        return;
    }

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
        } else if (pj_get(it, "text") || pj_get(it, "story")) {
            const pj_node* ins = pj_get(it, "ins"), *an = pj_get(it, "anchor");

            pb_printf(o, "<wps:wsp><wps:cNvPr id=\"%d\" name=\"Text Box %d\"/><wps:cNvSpPr txBox=\"1\"/><wps:spPr>%s"
                      "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr>"
                      "<wps:txbx><w:txbxContent>", id, id, xfrm);

            if (pj_get(it, "story")) {  /* the text box's story, as edited */
                dx_story_content(x, (pd_block_id)pj_int_or(pj_get(it, "story"), 0));
            } else {
                dx_txbx_paras(x, pj_get(it, "text"), 0);
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
static int first_rev(void* user, const pd_span* sp) {
    *(pd_rev_id*)user = sp->cp.revision;
    return 1;
}

/* the tracked change a float's picture is in (a deleted one: in w:del) */
static pd_rev_id dx_float_rev(const dxo* x, pd_block_id fl) {
    pd_rev_id rev = 0;
    pd_block_info fi;

    if (pd_doc_block_info(x->d, fl, &fi) == PD_OK && fi.child_count == 1) {
        pd_conv_spans_all(x->d, pd_doc_child(x->d, fl, 0), first_rev, &rev);
    }

    return rev;
}

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
              "distL=\"%lld\" distR=\"%lld\" simplePos=\"0\" relativeHeight=\"%d\" behindDoc=\"%d\" locked=\"0\" "
              "layoutInCell=\"1\" allowOverlap=\"1\"><wp:simplePos x=\"0\" y=\"0\"/><wp:positionH relativeFrom=\"column\">",
              gap, gap, x->docpr, fp->wrap == PD_WRAP_BEHIND);

    if (off) {
        pb_printf(o, "<wp:posOffset>%lld</wp:posOffset>", EMU(fp->offset_x));
    } else {
        pb_printf(o, "<wp:align>%s</wp:align>", fp->wrap == PD_WRAP_LEFT ? "left" : fp->wrap == PD_WRAP_RIGHT ? "right" :
                  "center");
    }

    pb_printf(o, "</wp:positionH><wp:positionV relativeFrom=\"%s\"><wp:posOffset>%lld</wp:posOffset></wp:positionV>"
              "<wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:effectExtent l=\"0\" t=\"0\" r=\"0\" b=\"0\"/>%s"
              "<wp:docPr id=\"%d\" name=\"Text Box %d\"/><wp:cNvGraphicFramePr/><a:graphic><a:graphicData "
              "uri=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\"><wps:wsp><wps:cNvSpPr txBox=\"1\"/>"
              "<wps:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm><a:prstGeom prst=\"rect\">"
              "<a:avLst/></a:prstGeom><a:noFill/><a:ln><a:noFill/></a:ln></wps:spPr><wps:txbx><w:txbxContent>",
              fp->offset_from == PD_FROM_PAGE ? "page" : fp->offset_from == PD_FROM_MARGIN ? "margin" : "paragraph",
              EMU(fp->offset_y), cx, cy,
              fp->wrap >= PD_WRAP_FRONT ? "<wp:wrapNone/>" :
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
            pd_revision rv;
            pd_rev_id rev = dx_float_rev(x, pend[i]);
            int tracked = rev && pd_doc_revision_get(x->d, rev, &rv) == PD_OK;

            if (tracked) {      /* inserted or deleted with the text around it */
                pb_printf(x->o, "<w:%s w:id=\"%d\" w:author=\"", rv.kind == PD_REV_DELETE ? "del" : "ins", ++x->nrev);
                xesc(x->o, rv.author, strlen(rv.author));

                if (rv.date[0]) {
                    pb_puts(x->o, "\" w:date=\"");
                    xesc(x->o, rv.date, strlen(rv.date));
                }

                pb_puts(x->o, "\">");
            }

            dx_picture(x, &ob, &fp);

            if (tracked) {
                pb_puts(x->o, rv.kind == PD_REV_DELETE ? "</w:del>" : "</w:ins>");
            }
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
/* the w:ins or w:del a tracked change's runs go in, opened: its kind (PD_REV_*), 0 when it is none */
/* a tracked insertion or deletion opened round runs: its kind, 8 more for a move (moveFrom, moveTo, in a range
   of the move's name), 0 none (a formatting change is in the run's rPr) */
static int dx_rev_open(dxo* x, pd_rev_id rev) {
    pd_buf* o = x->o;
    pd_revision rv;
    const char* el;
    int k;

    if (!rev || pd_doc_revision_get(x->d, rev, &rv) != PD_OK || rv.kind == PD_REV_FORMAT) {
        return 0;
    }

    el = rv.move ? (rv.kind == PD_REV_DELETE ? "moveFrom" : "moveTo") : rv.kind == PD_REV_DELETE ? "del" : "ins";

    for (k = rv.move ? 0 : 1; k < 2; k++) {     /* the move's range, then the change */
        pb_printf(o, k ? "<w:%s w:id=\"%d\" w:author=\"" : "<w:%sRangeStart w:id=\"%d\" w:author=\"", el, ++x->nrev);
        xesc(o, rv.author, strlen(rv.author));

        if (rv.date[0]) {
            pb_puts(o, "\" w:date=\"");
            xesc(o, rv.date, strlen(rv.date));
        }

        if (k) {
            pb_puts(o, "\">");
        } else {
            pb_printf(o, "\" w:name=\"move%d\"/>", (int)rv.move);
            x->move_range = x->nrev;
        }
    }

    return rv.move ? rv.kind + 8 : rv.kind;
}

static void dx_rev_close(dxo* x, int kind) {
    if (kind > 8) {
        pb_printf(x->o, kind == PD_REV_DELETE + 8 ? "</w:moveFrom><w:moveFromRangeEnd w:id=\"%d\"/>" :
                  "</w:moveTo><w:moveToRangeEnd w:id=\"%d\"/>", x->move_range);
    } else if (kind) {
        pb_puts(x->o, kind == PD_REV_DELETE ? "</w:del>" : "</w:ins>");
    }
}

static void dx_run(dxo* x, const pd_span* sp, const char* t, size_t n) {
    pd_buf* o = x->o;
    int kind = dx_rev_open(x, sp->cp.revision);

    pb_puts(o, "<w:r>");
    dx_rpr(x, &sp->cp, &x->base, x->in_link ? "Hyperlink" : NULL);
    dx_text_as(o, t, n, kind == PD_REV_DELETE ? "w:delText" : "w:t");
    pb_puts(o, "</w:r>");
    dx_rev_close(x, kind);
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

/* a string member of a control's JSON as an attribute value: name="value", nothing when absent */
static void dx_jattr(pd_buf* o, const pj_node* root, const char* key, const char* attr) {
    const pj_node* n = pj_get(root, key);

    if (n && n->type == PJ_STR) {
        pb_printf(o, " %s=\"", attr);
        xesc(o, n->s, n->len);
        pb_putc(o, '"');
    }
}

/* an element with one value from the control's JSON: <name w:val="..."/> */
static void dx_jval(pd_buf* o, const pj_node* root, const char* key, const char* elem) {
    const pj_node* n = pj_get(root, key);

    if (n && n->type == PJ_STR) {
        pb_printf(o, "<%s w:val=\"", elem);
        xesc(o, n->s, n->len);
        pb_puts(o, "\"/>");
    }
}

/* a content control's start: w:sdt, its properties, the content opened */
static void dx_control_start(dxo* x, const pd_inline* ob) {
    pd_buf* o = x->o;
    pj_doc* doc = ob->source_len > 0 ? pj_parse(ob->source, (size_t)ob->source_len, 0, NULL) : NULL;
    const pj_node* r = doc ? pj_root(doc) : NULL, *n;
    const char* kind = ob->name;

    if (r && r->type != PJ_OBJ) {
        r = NULL;
    }

    pb_puts(o, "<w:sdt><w:sdtPr>");

    if (r) {
        dx_jval(o, r, "title", "w:alias");
        dx_jval(o, r, "tag", "w:tag");
        pb_printf(o, "<w:id w:val=\"%d\"/>", 0x4C000000 + ++x->nctl_ids);
        dx_jval(o, r, "lock", "w:lock");

        if ((n = pj_get(r, "prompt")) != NULL && n->type == PJ_STR) {
            pb_puts(o, "<w:placeholder><w:docPart w:val=\"");
            xesc(o, n->s, n->len);
            pb_puts(o, "\"/></w:placeholder>");
        }

        if (pj_int_or(pj_get(r, "placeholder"), 0)) {
            pb_puts(o, "<w:showingPlcHdr/>");
        }
    }

    if (!strcmp(kind, "checkbox")) {
        const pj_node* on = r ? pj_get(r, "on") : NULL, *off = r ? pj_get(r, "off") : NULL;

        pb_printf(o, "<w14:checkbox><w14:checked w14:val=\"%d\"/>", r && pj_int_or(pj_get(r, "checked"), 0) ? 1 : 0);
        pb_puts(o, "<w14:checkedState w14:val=\"");
        xesc(o, on && on->type == PJ_STR ? on->s : "2612", on && on->type == PJ_STR ? on->len : 4);
        pb_putc(o, '"');

        if (r) {
            dx_jattr(o, r, "onfont", "w14:font");
        }

        pb_puts(o, "/><w14:uncheckedState w14:val=\"");
        xesc(o, off && off->type == PJ_STR ? off->s : "2610", off && off->type == PJ_STR ? off->len : 4);
        pb_putc(o, '"');

        if (r) {
            dx_jattr(o, r, "offfont", "w14:font");
        }

        pb_puts(o, "/></w14:checkbox>");
    } else if (!strcmp(kind, "dropdown") || !strcmp(kind, "combobox")) {
        const pj_node* items = r ? pj_get(r, "items") : NULL, *it;

        pb_puts(o, kind[0] == 'd' ? "<w:dropDownList" : "<w:comboBox");

        if (r) {
            dx_jattr(o, r, "value", "w:lastValue");
        }

        pb_putc(o, '>');

        for (it = items && items->type == PJ_ARR ? items->child : NULL; it; it = it->next) {
            const pj_node* dt = pj_at(it, 0), *vl = pj_at(it, 1);

            if (!dt || dt->type != PJ_STR) {
                continue;
            }

            pb_puts(o, "<w:listItem w:displayText=\"");
            xesc(o, dt->s, dt->len);
            pb_puts(o, "\" w:value=\"");
            xesc(o, vl && vl->type == PJ_STR ? vl->s : dt->s, vl && vl->type == PJ_STR ? vl->len : dt->len);
            pb_puts(o, "\"/>");
        }

        pb_puts(o, kind[0] == 'd' ? "</w:dropDownList>" : "</w:comboBox>");
    } else if (!strcmp(kind, "date")) {
        pb_puts(o, "<w:date");

        if (r) {
            dx_jattr(o, r, "date", "w:fullDate");
        }

        pb_putc(o, '>');

        if (r) {
            dx_jval(o, r, "format", "w:dateFormat");
            dx_jval(o, r, "lid", "w:lid");
        }

        pb_puts(o, "<w:storeMappedDataAs w:val=\"dateTime\"/><w:calendar w:val=\"gregorian\"/></w:date>");
    } else if (!strcmp(kind, "text")) {
        pb_puts(o, r && pj_int_or(pj_get(r, "multiline"), 0) ? "<w:text w:multiLine=\"1\"/>" : "<w:text/>");
    } else if (!strcmp(kind, "picture") || !strcmp(kind, "group") || !strcmp(kind, "citation") ||
               !strcmp(kind, "bibliography") || !strcmp(kind, "equation")) {
        pb_printf(o, "<w:%s/>", kind);
    } else if (!strcmp(kind, "docpart")) {
        pb_puts(o, "<w:docPartObj>");

        if (r) {
            dx_jval(o, r, "gallery", "w:docPartGallery");
        }

        pb_puts(o, "<w:docPartUnique/></w:docPartObj>");
    }

    pb_puts(o, "</w:sdtPr><w:sdtContent>");
    x->nctl++;
    pj_free(doc);
}

/* the innermost control's end, or every one still open (at a paragraph's end) */
static void dx_control_end(dxo* x, int all) {
    while (x->nctl > 0) {
        if (x->in_link) {   /* a link opened inside it ends with it */
            pb_puts(x->o, "</w:hyperlink>");
            x->in_link = 0;
        }

        pb_puts(x->o, "</w:sdtContent></w:sdt>");
        x->nctl--;

        if (!all) {
            break;
        }
    }
}

static int dx_span(void* user, const pd_span* sp) {
    dxo* x = (dxo*)user;
    pd_buf* o = x->o;

    dx_marks(x, sp->offset);

    if (sp->is_object) {
        const pd_inline* ob = &sp->obj;

        switch (ob->kind) {
            case PD_INLINE_IMAGE: {     /* inserted or deleted with the text round it: in its w:ins or w:del */
                int kind = dx_rev_open(x, sp->cp.revision);

                x->obj_cp = &sp->cp;
                dx_picture(x, ob, NULL);
                x->obj_cp = NULL;
                dx_rev_close(x, kind);
                break;
            }

            case PD_INLINE_EQUATION:     /* as Word's own math, a display one on its line */
                if (ob->source && ob->source_len > 0) {
                    pd_block_info pi;

                    pd_doc_block_info(x->d, x->para, &pi);
                    pd_latex_to_omml(ob->source, (size_t)ob->source_len, pi.role == PD_ROLE_EQUATION, o);
                }

                break;

            case PD_INLINE_FIELD: {
                char v[96] = "", ins[300];
                const char* instr = NULL;

                switch (ob->field) {
                    case PD_FIELD_KEPT:     /* Word's field around its result: its start, or (no source) its end */
                        if (ob->source_len > 0) {
                            pb_puts(o, "<w:r>");
                            dx_rpr(x, &sp->cp, &x->base, NULL);
                            pb_puts(o, "<w:fldChar w:fldCharType=\"begin\"/></w:r>");
                            dx_field_code(x, &sp->cp, ob->source, (size_t)ob->source_len);
                            pb_puts(o, "<w:r>");
                            dx_rpr(x, &sp->cp, &x->base, NULL);
                            pb_puts(o, "<w:fldChar w:fldCharType=\"separate\"/></w:r>");
                        } else {
                            pb_puts(o, "<w:r>");
                            dx_rpr(x, &sp->cp, &x->base, NULL);
                            pb_puts(o, "<w:fldChar w:fldCharType=\"end\"/></w:r>");
                        }

                        break;

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

                    case PD_FIELD_DATE:     /* its own instruction, and its date now */
                        snprintf(ins, sizeof(ins), "%.*s", ob->source_len > 0 ? (int)(ob->source_len < 299 ?
                                 ob->source_len : 299) : 0, ob->source ? ob->source : "");
                        instr = ins[strspn(ins, " ")] ? ins : "DATE";
                        pd_date_field(x->d, ob->source, ob->source_len, v, sizeof(v));
                        v[0] = v[0] ? v[0] : ' ';
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

                if (instr && sp->cp.revision) {
                    /* in a tracked change: the field's runs (begin, code, result, end) in its w:ins or w:del, which
                       a w:fldSimple cannot be inside -- a deleted field's code and result as deleted text */
                    int kind = dx_rev_open(x, sp->cp.revision);
                    const char* it = kind == PD_REV_DELETE ? "w:delInstrText" : "w:instrText";
                    char code[320];

                    if (ob->field == PD_FIELD_SEQ) {
                        snprintf(code, sizeof(code), " SEQ %.40s \\* ARABIC ", ob->name[0] ? ob->name : "Figure");
                    } else if (ob->field == PD_FIELD_HEADING) {
                        snprintf(code, sizeof(code), " STYLEREF \"Heading %d\" ", ob->level > 0 ? (int)ob->level : 1);
                    } else {
                        snprintf(code, sizeof(code), " %s ", instr);
                    }

                    /* each run in the text's own size and face: the field takes its format from the first */
                    pb_puts(o, "<w:r>");
                    dx_rpr(x, &sp->cp, &x->base, NULL);
                    pb_printf(o, "<w:fldChar w:fldCharType=\"begin\"/></w:r><w:r>");
                    dx_rpr(x, &sp->cp, &x->base, NULL);
                    pb_printf(o, "<%s xml:space=\"preserve\">", it);
                    xesc(o, code, strlen(code));
                    pb_printf(o, "</%s></w:r><w:r>", it);
                    dx_rpr(x, &sp->cp, &x->base, NULL);
                    pb_puts(o, "<w:fldChar w:fldCharType=\"separate\"/></w:r><w:r>");
                    dx_rpr(x, &sp->cp, &x->base, NULL);
                    dx_text_as(o, v, strlen(v), kind == PD_REV_DELETE ? "w:delText" : "w:t");
                    pb_puts(o, "</w:r><w:r>");
                    dx_rpr(x, &sp->cp, &x->base, NULL);
                    pb_puts(o, "<w:fldChar w:fldCharType=\"end\"/></w:r>");
                    dx_rev_close(x, kind);
                    break;
                }

                if (instr && ob->field == PD_FIELD_SEQ) {
                    pb_printf(o, "<w:fldSimple w:instr=\" SEQ %s \\* ARABIC \">", ob->name[0] ? ob->name : "Figure");
                } else if (instr && ob->field == PD_FIELD_HEADING) {
                    pb_printf(o, "<w:fldSimple w:instr=\" STYLEREF &quot;Heading %d&quot; \">", ob->level > 0 ? (int)ob->level :
                              1);
                } else if (instr) {
                    pb_puts(o, "<w:fldSimple w:instr=\" ");
                    xesc(o, instr + strspn(instr, " "), strlen(instr + strspn(instr, " ")));
                    pb_puts(o, " \">");
                }

                if (v[0]) {
                    pb_puts(o, "<w:r>");
                    dx_rpr(x, &sp->cp, &x->base, NULL);
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
                int slink = x->in_link, sncb = x->ncb, scbi = x->cbi, sctl = x->nctl;
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
                x->nctl = sctl;

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

            case PD_INLINE_RUBY:
                if (x->in_link) {   /* inside a run: no link around it */
                    pb_puts(o, "</w:hyperlink>");
                    x->in_link = 0;
                }

                if (ob->source && ob->source_len > 0) {
                    int base = (int)SCALE(sp->cp.size, 2, 65536), hps = ob->height > 0 ? (int)SCALE(ob->height, 2, 65536) :
                               base / 2, raise = ob->depth > 0 ? (int)SCALE(ob->depth, 2, 65536) : base;
                    const char* lid = !strncmp(sp->cp.lang, "zh", 2) || !strncmp(sp->cp.lang, "ko", 2) ||
                                      !strncmp(sp->cp.lang, "ja", 2) ? sp->cp.lang : "ja-JP";

                    pb_printf(o, "<w:r><w:ruby><w:rubyPr><w:rubyAlign w:val=\"center\"/><w:hps w:val=\"%d\"/>"
                              "<w:hpsRaise w:val=\"%d\"/><w:hpsBaseText w:val=\"%d\"/><w:lid w:val=\"", hps, raise, base);
                    xesc(o, lid, strlen(lid));
                    pb_printf(o, "\"/></w:rubyPr><w:rt><w:r><w:rPr><w:sz w:val=\"%d\"/></w:rPr>", hps);
                    dx_text(o, ob->source, (size_t)ob->source_len);
                    pb_puts(o, "</w:r></w:rt><w:rubyBase>");
                    x->nruby++;
                } else if (x->nruby > 0) {
                    pb_puts(o, "</w:rubyBase></w:ruby></w:r>");
                    x->nruby--;
                }

                break;

            case PD_INLINE_CONTROL:
                if (x->in_link) {   /* w:sdt and w:hyperlink nest: the link ends here */
                    pb_puts(o, "</w:hyperlink>");
                    x->in_link = 0;
                }

                if (ob->name[0]) {
                    dx_control_start(x, ob);
                } else {
                    dx_control_end(x, 0);
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

    if (x->nlistmap >= 2048) {
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

    dx_ppr_head(o, &dp, dp.mask & ~(PD_PP_WIDOWS | PD_PP_ORPHANS));

    if (x->frame) {     /* a frame's (a drop cap's) paragraph */
        const pd_float_props* f = x->frame;

        pb_puts(o, "<w:framePr");

        if (f->frame == PD_FRAME_DROP || f->frame == PD_FRAME_MARGIN) {
            pb_printf(o, " w:dropCap=\"%s\" w:lines=\"%d\"", f->frame == PD_FRAME_DROP ? "drop" : "margin",
                      (int)(f->frame_lines > 0 ? f->frame_lines : 1));
        } else {
            pb_printf(o, " w:w=\"%d\"", TW(f->width));

            if (f->placement & PD_PLACE_OFFSET) {
                pb_printf(o, " w:x=\"%d\"", TW(f->offset_x));
            } else if (f->wrap == PD_WRAP_RIGHT) {
                pb_puts(o, " w:xAlign=\"right\"");
            }

            if (f->offset_y || f->offset_from != PD_FROM_PARAGRAPH) {
                pb_printf(o, " w:y=\"%d\" w:vAnchor=\"%s\"", TW(f->offset_y), f->offset_from == PD_FROM_PAGE ? "page" :
                          f->offset_from == PD_FROM_MARGIN ? "margin" : "text");
            }
        }

        if (f->gap > 0) {
            pb_printf(o, " w:hSpace=\"%d\"", TW(f->gap));
        }

        pb_printf(o, " w:wrap=\"%s\" w:hAnchor=\"text\"%s/>", f->wrap == PD_WRAP_NONE ? "notBeside" : "around",
                  f->frame == PD_FRAME_BOX ? "" : " w:vAnchor=\"text\"");
    }

    dx_ppr_head(o, &dp, dp.mask & (PD_PP_WIDOWS | PD_PP_ORPHANS));

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
    x->nctl = 0;
    x->nruby = 0;
    pd_conv_base_props(x->d, p, &x->base);
    dx_comment_bounds(x, p);
    pd_conv_spans_all(x->d, p, dx_span, x);

    if (x->in_link) {
        pb_puts(x->o, "</w:hyperlink>");
        x->in_link = 0;
    }

    while (x->nruby > 0) {      /* a guide the paragraph does not end: ended with it */
        pb_puts(x->o, "</w:rubyBase></w:ruby></w:r>");
        x->nruby--;
    }

    dx_control_end(x, 1);

    dx_marks(x, UINT32_MAX);
    x->ncb = x->cbi = 0;

    pb_puts(x->o, "</w:p>");
}

/* whether the cell of a row starting at grid column col continues the one above */
static int dx_merges_below(const dxo* x, pd_block_id row, int32_t col) {
    pd_block_info ri;
    pd_row_props rp;
    int32_t c, at = 0;

    pd_doc_block_info(x->d, row, &ri);

    if (pd_doc_row_props(x->d, row, &rp) == PD_OK) {
        at = rp.grid_before;
    }

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
    pd_row_props rp;
    pd_float_props fp;
    pd_block_id fl;
    int32_t r, c, k, ncols = 0, col;
    pd_buf* o = x->o;
    int colw, fixed = 0;

    pd_doc_block_info(x->d, t, &ti);
    pd_doc_table_props(x->d, t, &tp);
    fl = x->tblp;
    x->tblp = 0;

    for (r = 0; r < ti.child_count; r++) {
        int32_t w = 0;

        pd_doc_block_info(x->d, pd_doc_child(x->d, t, r), &ri);

        if (pd_doc_row_props(x->d, pd_doc_child(x->d, t, r), &rp) == PD_OK) {
            w = rp.grid_before + rp.grid_after;
        }

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

    if (tp.style) {     /* the table's style, the document's: its rules those the table does not give itself */
        char sid[64];

        dx_sid(x->d, tp.style, sid, sizeof(sid));
        pb_printf(o, "<w:tbl><w:tblPr><w:tblStyle w:val=\"%s\"/>", sid);
    } else {
        pb_puts(o, "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/>");
    }

    if (fl && pd_doc_float_props(x->d, fl, &fp) == PD_OK) {    /* floating, beside the text */
        int gap = TW(fp.gap);

        pb_printf(o, "<w:tblpPr w:leftFromText=\"%d\" w:rightFromText=\"%d\" w:vertAnchor=\"%s\" "
                  "w:horzAnchor=\"%s\"", fp.wrap == PD_WRAP_LEFT ? 0 : gap, fp.wrap == PD_WRAP_RIGHT ? 0 : gap,
                  fp.offset_from == PD_FROM_PAGE ? "page" : fp.offset_from == PD_FROM_MARGIN ? "margin" : "text",
                  (fp.placement & PD_PLACE_OFFSET) ? "text" : "margin");

        if (fp.placement & PD_PLACE_OFFSET) {
            pb_printf(o, " w:tblpX=\"%d\"", TW(fp.offset_x));
        } else {
            pb_printf(o, " w:tblpXSpec=\"%s\"", fp.wrap == PD_WRAP_RIGHT ? "right" : fp.wrap == PD_WRAP_LEFT ? "left" :
                      "center");
        }

        pb_printf(o, " w:tblpY=\"%d\"/>", TW(fp.offset_y));
    }

    if (tp.width_pct > 0) {
        pb_printf(o, "<w:tblW w:w=\"%d\" w:type=\"pct\"/>", (int)tp.width_pct * 5);     /* fiftieths of a percent */
    } else if (tp.width > 0) {
        pb_printf(o, "<w:tblW w:w=\"%d\" w:type=\"dxa\"/>", TW(tp.width));
    } else {
        pb_puts(o, "<w:tblW w:w=\"0\" w:type=\"auto\"/>");
    }

    if (tp.direction == PD_DIR_RTL) {
        pb_puts(o, "<w:bidiVisual/>");
    }

    if (tp.align == PD_ALIGN_CENTER || tp.align == PD_ALIGN_RIGHT) {
        pb_puts(o, tp.align == PD_ALIGN_CENTER ? "<w:jc w:val=\"center\"/>" : "<w:jc w:val=\"right\"/>");
    }

    if (tp.cell_spacing > 0) {
        pb_printf(o, "<w:tblCellSpacing w:w=\"%d\" w:type=\"dxa\"/>", TW(tp.cell_spacing));
    }

    if (tp.align != PD_ALIGN_CENTER && tp.align != PD_ALIGN_RIGHT && tp.indent) {
        pb_printf(o, "<w:tblInd w:w=\"%d\" w:type=\"dxa\"/>", TW(tp.indent));
    }

    if (tp.style) {
        int own = tp.border ? (tp.border_sides ? tp.border_sides : 63) : 0;

        dx_rules(o, "tblBorders", tp.border_given & 63, own & tp.border_given, tp.border, NULL, tp.border_color,
                 tp.border_theme, 1);
    } else if (tp.border) {
        static const char* edge[] = { "top", "left", "bottom", "right", "insideH", "insideV" };
        static const int bit[] = { PD_TBORDER_TOP, PD_TBORDER_LEFT, PD_TBORDER_BOTTOM, PD_TBORDER_RIGHT,
                                   PD_TBORDER_INSIDE_H, PD_TBORDER_INSIDE_V
                                 };
        int sz = (int)SCALE(tp.border, 8, 65536), sides = tp.border_sides ? tp.border_sides : 63;     /* eighths of a point */

        sz = sz < 2 ? 2 : sz;
        pb_puts(o, "<w:tblBorders>");

        for (k = 0; k < 6; k++) {
            if (sides & bit[k]) {
                pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"%06X\"", edge[k], sz,
                          (unsigned)(tp.border_color & 0xFFFFFF));
                dx_theme_attr(o, tp.border_theme, 0);
                pb_puts(o, "/>");
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

    if (tp.style) {
        int lk = tp.look, v = ((lk & PD_TLOOK_FIRST_ROW) ? 0x20 : 0) | ((lk & PD_TLOOK_LAST_ROW) ? 0x40 : 0) |
                              ((lk & PD_TLOOK_FIRST_COL) ? 0x80 : 0) | ((lk & PD_TLOOK_LAST_COL) ? 0x100 : 0) |
                              ((lk & PD_TLOOK_NO_HBAND) ? 0x200 : 0) | ((lk & PD_TLOOK_NO_VBAND) ? 0x400 : 0);

        pb_printf(o, "<w:tblLook w:val=\"%04X\" w:firstRow=\"%d\" w:lastRow=\"%d\" w:firstColumn=\"%d\" "
                  "w:lastColumn=\"%d\" w:noHBand=\"%d\" w:noVBand=\"%d\"/></w:tblPr><w:tblGrid>", v,
                  !!(lk & PD_TLOOK_FIRST_ROW), !!(lk & PD_TLOOK_LAST_ROW), !!(lk & PD_TLOOK_FIRST_COL),
                  !!(lk & PD_TLOOK_LAST_COL), !!(lk & PD_TLOOK_NO_HBAND), !!(lk & PD_TLOOK_NO_VBAND));
    } else {
        pb_puts(o, "<w:tblLook w:val=\"04A0\"/></w:tblPr><w:tblGrid>");
    }

    for (c = 0; c < ncols; c++) {
        pb_printf(o, "<w:gridCol w:w=\"%d\"/>", c < tp.ncols && tp.col_width[c] > 0 ? TW(tp.col_width[c]) : colw);
    }

    pb_puts(o, "</w:tblGrid>");

    for (r = 0; r < ti.child_count; r++) {
        pd_block_id row = pd_doc_child(x->d, t, r);

        pd_doc_block_info(x->d, row, &ri);
        pd_doc_row_props(x->d, row, &rp);
        pb_puts(o, "<w:tr>");
        col = rp.grid_before;

        {   /* the row's height: the tallest a cell asks for */
            pd_sp minh = 0;

            for (c = 0; c < ri.child_count; c++) {
                pd_cell_props cp;

                pd_doc_cell_props(x->d, pd_doc_child(x->d, row, c), &cp);
                minh = cp.min_height > minh ? cp.min_height : minh;
            }

            if (rp.height_exact) {
                minh = rp.height;
            }

            if (minh > 0 || r < tp.header_rows || rp.grid_before || rp.grid_after || rp.split == PD_ROWSPLIT_NEVER) {
                pb_puts(o, "<w:trPr>");

                if (rp.grid_before) {
                    pb_printf(o, "<w:gridBefore w:val=\"%d\"/>", (int)rp.grid_before);
                }

                if (rp.grid_after) {
                    pb_printf(o, "<w:gridAfter w:val=\"%d\"/>", (int)rp.grid_after);
                }

                if (rp.grid_before && rp.width_before) {
                    pb_printf(o, "<w:wBefore w:w=\"%d\" w:type=\"dxa\"/>", TW(rp.width_before));
                }

                if (rp.grid_after && rp.width_after) {
                    pb_printf(o, "<w:wAfter w:w=\"%d\" w:type=\"dxa\"/>", TW(rp.width_after));
                }

                if (rp.split == PD_ROWSPLIT_NEVER) {
                    pb_puts(o, "<w:cantSplit/>");
                }

                if (minh > 0) {
                    pb_printf(o, "<w:trHeight w:val=\"%d\"%s/>", TW(minh), rp.height_exact ? " w:hRule=\"exact\"" : "");
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
            if (cp.width > 0) {
                pb_printf(o, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/>", TW(cp.width));
            } else if (cp.width_pct > 0) {
                pb_printf(o, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"pct\"/>", (int)cp.width_pct * 5);
            } else {    /* the grid says */
                pb_puts(o, "<w:tc><w:tcPr><w:tcW w:w=\"0\" w:type=\"auto\"/>");
            }

            if (cp.col_span > 1) {
                pb_printf(o, "<w:gridSpan w:val=\"%d\"/>", (int)cp.col_span);
            }

            if (cp.merge_up) {
                pb_puts(o, "<w:vMerge/>");
            } else if (r + 1 < ti.child_count && dx_merges_below(x, pd_doc_child(x->d, t, r + 1), col)) {
                pb_puts(o, "<w:vMerge w:val=\"restart\"/>");
            }

            if (cp.border_set || cp.diag_width[0] || cp.diag_width[1]) {
                static const char* edge[] = { "top", "left", "bottom", "right" };
                static const int bit[] = { PD_BORDER_TOP, PD_BORDER_LEFT, PD_BORDER_BOTTOM, PD_BORDER_RIGHT };
                static const int slot[] = { 0, 3, 2, 1 };  /* edge_width's order: top, right, bottom, left */
                int sz, e;

                pb_puts(o, "<w:tcBorders>");

                for (e = 0; e < 4; e++) {
                    sz = (int)SCALE(cp.edge_width[slot[e]] > 0 ? cp.edge_width[slot[e]] : cp.border_width, 8, 65536);
                    sz = sz < 2 ? 2 : sz;

                    if (cp.border_on & bit[e]) {
                        pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"%06X\"", edge[e], sz,
                                  (unsigned)(cp.border_color & 0xFFFFFF));
                        dx_theme_attr(o, cp.border_theme, 0);
                        pb_puts(o, "/>");
                    } else if (cp.border_set & bit[e]) {
                        pb_printf(o, "<w:%s w:val=\"nil\"/>", edge[e]);
                    }
                }

                for (e = 0; e < 2; e++) {   /* corner to corner, after the edges (insideH and insideV between) */
                    if (cp.diag_width[e] > 0) {
                        sz = (int)SCALE(cp.diag_width[e], 8, 65536);
                        pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"%06X\"/>",
                                  e ? "tr2bl" : "tl2br", sz < 2 ? 2 : sz, (unsigned)(cp.diag_color[e] & 0xFFFFFF));
                    }
                }

                pb_puts(o, "</w:tcBorders>");
            }

            if (cp.background) {
                dx_shd(o, cp.background, cp.background_theme);
            }

            if (cp.text_dir) {
                pb_puts(o, cp.text_dir == PD_CELLTEXT_BTLR ? "<w:textDirection w:val=\"btLr\"/>" :
                        "<w:textDirection w:val=\"tbRl\"/>");
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
            pd_float_props fp;
            pd_block_info ki;

            (void)ob;

            if (pd_doc_float_props(x->d, id, &fp) == PD_OK && fp.frame && !x->frame) {
                x->frame = &fp;     /* a Word frame: its paragraphs with their framePr, in the flow */

                for (i = 0; i < bi.child_count; i++) {
                    dx_block(x, pd_doc_child(x->d, id, i));
                }

                x->frame = NULL;
                break;
            }

            if (bi.child_count == 1 && pd_doc_block_info(x->d, pd_doc_child(x->d, id, 0), &ki) == PD_OK &&
                    ki.kind == PD_BLOCK_TABLE) {     /* a table alone: a floating table */
                dx_flush_anchors(x);
                x->tblp = id;
                dx_table(x, ki.id, dx_text_width(x));
                break;
            }

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

/* Parade's line styles as Word's border styles */
static const char* line_style_name(int32_t style) {
    static const char* names[] = { "nil", "single", "double", "dotted", "dashed", "thick", "thinThickSmallGap",
                                   "thickThinSmallGap", "triple", "dotDash"
                                 };

    return style >= 0 && style <= PD_LINE_DOT_DASH ? names[style] : "single";
}

/* one side of a border as Word's element NAME (space in points) */
static void dx_border_line(pd_buf* o, const char* name, const pd_border_line* l) {
    int sz = (int)SCALE(l->width, 8, 65536);

    if (l->style == PD_LINE_NONE) {
        return;
    }

    pb_printf(o, "<w:%s w:val=\"%s\" w:sz=\"%d\" w:space=\"%d\" w:color=\"%06X\"/>", name,
              line_style_name(l->style), sz < 2 ? 2 : sz, (int)((l->space + 32768) / 65536),
              (unsigned)(l->color & 0xFFFFFF));
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

    for (k = 0; k < 2; k++) {   /* how its notes are numbered, when not as usual */
        static const char* const rs[3] = { "continuous", "eachSect", "eachPage" };

        if (sp->note_format[k] || sp->note_start[k] > 1 || sp->note_restart[k]) {
            pb_printf(o, "<w:%s>", k ? "endnotePr" : "footnotePr");

            if (sp->note_format[k]) {
                pb_printf(o, "<w:numFmt w:val=\"%s\"/>", num_fmt_name(sp->note_format[k]));
            }

            if (sp->note_start[k] > 1) {
                pb_printf(o, "<w:numStart w:val=\"%d\"/>", (int)sp->note_start[k]);
            }

            if (sp->note_restart[k] > 0 && sp->note_restart[k] <= 2) {
                pb_printf(o, "<w:numRestart w:val=\"%s\"/>", rs[sp->note_restart[k]]);
            }

            pb_printf(o, "</w:%s>", k ? "endnotePr" : "footnotePr");
        }
    }

    if (sp->continuous || sp->start != PD_BREAK_PAGE) {
        pb_printf(o, "<w:type w:val=\"%s\"/>", sp->continuous ? "continuous" : sp->start == PD_BREAK_ODD_PAGE ?
                  "oddPage" : sp->start == PD_BREAK_EVEN_PAGE ? "evenPage" : "nextColumn");
    }

    pb_printf(o, "<w:pgSz w:w=\"%d\" w:h=\"%d\"/>", TW(sp->page_width), TW(sp->page_height));
    pb_printf(o, "<w:pgMar w:top=\"%d\" w:right=\"%d\" w:bottom=\"%d\" w:left=\"%d\" w:header=\"%d\" w:footer=\"%d\" "
              "w:gutter=\"%d\"/>", TW(sp->margin_top), TW(sp->margin_right), TW(sp->margin_bottom), TW(sp->margin_left),
              TW(sp->header_distance), TW(sp->footer_distance), TW(sp->gutter));

    x->mirror |= sp->mirror_margins > 0;

    if (sp->page_border[0].style || sp->page_border[1].style || sp->page_border[2].style || sp->page_border[3].style) {
        static const char* side[4] = { "top", "left", "bottom", "right" };
        static const int at[4] = { 0, 3, 2, 1 };    /* the schema's order of Parade's top, right, bottom, left */
        int f = sp->page_border_flags;

        pb_printf(o, "<w:pgBorders w:offsetFrom=\"%s\"%s%s>", (f & PD_PGBORDER_FROM_TEXT) ? "text" : "page",
                  (f & PD_PGBORDER_FIRST) ? " w:display=\"firstPage\"" : (f & PD_PGBORDER_NOT_FIRST) ?
                  " w:display=\"notFirstPage\"" : "", (f & PD_PGBORDER_BEHIND) ? " w:zOrder=\"back\"" : "");

        for (k = 0; k < 4; k++) {
            dx_border_line(o, side[k], &sp->page_border[at[k]]);
        }

        pb_puts(o, "</w:pgBorders>");
    }

    if (sp->line_numbers > 0) {     /* Word's start is one less than the first number */
        pb_printf(o, "<w:lnNumType w:countBy=\"%d\" w:start=\"%d\" w:distance=\"%d\" w:restart=\"%s\"/>",
                  (int)sp->line_numbers, (int)(sp->line_number_start > 1 ? sp->line_number_start - 1 : 0),
                  TW(sp->line_number_distance > 0 ? sp->line_number_distance : PD_PT(18)),
                  sp->line_number_restart == PD_LINENUM_SECTION ? "newSection" :
                  sp->line_number_restart == PD_LINENUM_CONTINUOUS ? "continuous" : "newPage");
    }

    if (sp->first_page_number > 0 || sp->page_number_format != PD_NUM_DECIMAL) {


        pb_puts(o, "<w:pgNumType");

        if (sp->page_number_format != PD_NUM_DECIMAL && sp->page_number_format > PD_NUM_BULLET &&
                sp->page_number_format <= PD_NUM_LAST && sp->page_number_format != PD_NUM_NONE) {
            pb_printf(o, " w:fmt=\"%s\"", num_fmt_name(sp->page_number_format));
        }

        if (sp->first_page_number > 0) {
            pb_printf(o, " w:start=\"%d\"", (int)sp->first_page_number);
        }

        pb_puts(o, "/>");
    }

    if (sp->columns > 1 && sp->ncol_widths > 0) {   /* columns of their own widths */
        pb_printf(o, "<w:cols w:num=\"%d\" w:space=\"%d\"%s w:equalWidth=\"0\">", (int)sp->columns,
                  TW(sp->column_gap), sp->column_rule ? " w:sep=\"1\"" : "");

        for (k = 0; k < sp->ncol_widths; k++) {
            pb_printf(o, "<w:col w:w=\"%d\" w:space=\"%d\"/>", TW(sp->col_width[k]), TW(sp->col_space[k]));
        }

        pb_puts(o, "</w:cols>");
    } else if (sp->columns > 1) {
        pb_printf(o, "<w:cols w:num=\"%d\" w:space=\"%d\"%s/>", (int)sp->columns, TW(sp->column_gap),
                  sp->column_rule ? " w:sep=\"1\"" : "");
    } else {    /* the gap too, as Word always writes it: read again, it would be the default */
        pb_printf(o, "<w:cols w:space=\"%d\"/>", TW(sp->column_gap));
    }

    if (sp->page_valign) {
        pb_puts(o, sp->page_valign == 1 ? "<w:vAlign w:val=\"center\"/>" : "<w:vAlign w:val=\"bottom\"/>");
    }

    if (sp->text_flow) {
        pb_puts(o, sp->text_flow == PD_CELLTEXT_BTLR ? "<w:textDirection w:val=\"btLr\"/>" :
                "<w:textDirection w:val=\"tbRl\"/>");
    }

    if (sp->title_page) {
        pb_puts(o, "<w:titlePg/>");
    }

    if (sp->direction == PD_DIR_RTL) {
        pb_puts(o, "<w:bidi/>");
    }

    if (sp->line_pitch > 0) {
        pb_printf(o, "<w:docGrid w:type=\"lines\" w:linePitch=\"%d\"/>", TW(sp->line_pitch));
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
    } else if (n && !strcmp(n, "Table Grid")) {
        snprintf(out, cap, "TableGrid");        /* the document's own in place of the writer's */
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
/* w:rFonts: the text's family (NULL: not said) in the Latin slots, East Asian and complex-script ones (NULL or
   "": the text's, when it is said) */
static void dx_fonts3(pd_buf* o, const char* family, const char* ea, const char* cs, int32_t ft) {
    const char* f = family && !strcmp(family, "monospace") ? "Courier New" : family;

    cs = cs && cs[0] ? cs : f;
    ea = ea && ea[0] ? ea : NULL;
    pb_puts(o, "<w:rFonts");

    if (f) {
        pb_puts(o, " w:ascii=\"");
        xesc(o, f, strlen(f));
        pb_puts(o, "\" w:hAnsi=\"");
        xesc(o, f, strlen(f));
        pb_putc(o, '"');
    }

    if (ea) {
        pb_puts(o, " w:eastAsia=\"");
        xesc(o, ea, strlen(ea));
        pb_putc(o, '"');
    }

    if (cs) {
        pb_puts(o, " w:cs=\"");
        xesc(o, cs, strlen(cs));
        pb_putc(o, '"');
    }

    dx_font_theme_attr(o, PD_FONT_THEME(f ? PD_FONT_THEME_TEXT(ft) : 0, ea ? PD_FONT_THEME_EA(ft) : 0,
                                        cs ? PD_FONT_THEME_CS(ft) : 0));
    pb_puts(o, "/>");
}

static void dx_fonts(pd_buf* o, const char* family) {
    dx_fonts3(o, family, NULL, NULL, 0);
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
    int lines = 0, side;

    for (side = 0; side < 6; side++) {
        lines |= pp->border_line[side].style != PD_LINE_NONE;
    }

    if ((m & PD_PP_BORDER) && lines) {  /* each edge its own line */
        static const char* names[] = { "top", "left", "bottom", "right", "between", "bar" };
        static const int at[] = { 0, 3, 2, 1, PD_BORDER_LINE_BETWEEN, PD_BORDER_LINE_BAR };

        pb_puts(o, "<w:pBdr>");

        for (side = 0; side < 6; side++) {
            if (pp->border_line[at[side]].style != PD_LINE_NONE) {
                dx_border_line(o, names[side], &pp->border_line[at[side]]);
            } else if (base && base->border_color && base->border_width > 0 && side < 5) {
                pb_printf(o, "<w:%s w:val=\"nil\"/>", names[side]);
            }
        }

        pb_puts(o, "</w:pBdr>");
    } else if ((m & PD_PP_BORDER) && pp->border_color && pp->border_width > 0) {
        int sz = (int)SCALE(pp->border_width, 8, 65536);
        int has = pp->border_sides ? pp->border_sides : PD_BORDER_TOP | PD_BORDER_RIGHT | PD_BORDER_BOTTOM | PD_BORDER_LEFT;
        static const char* names[] = { "top", "left", "bottom", "right", "between" };
        static const int bits[] = { PD_BORDER_TOP, PD_BORDER_LEFT, PD_BORDER_BOTTOM, PD_BORDER_RIGHT, PD_BORDER_BETWEEN };

        sz = sz < 2 ? 2 : sz;
        pb_puts(o, "<w:pBdr>");

        for (side = 0; side < 5; side++) {
            if (has & bits[side]) {
                pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"%d\" w:color=\"%06X\"", names[side], sz,
                          side == 4 ? 0 : (int)SCALE(pp->border_space, 1, 65536), (unsigned)(pp->border_color & 0xFFFFFF));
                dx_theme_attr(o, pp->border_theme, 0);
                pb_puts(o, "/>");
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
        dx_shd(o, pp->shading, pp->shading_theme);
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
            pb_printf(o, "<w:tab w:val=\"%s\" w:leader=\"%s\" w:pos=\"%d\"/>", pp->tabs[k].align == PD_TAB_BAR ? "bar" :
                      al[pp->tabs[k].align & 3],
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

        if ((m & PD_PP_LINE_SPACING) && (pp->line_rule == PD_LINE_AT_LEAST || pp->line_rule == PD_LINE_EXACT)) {
            pb_printf(o, " w:line=\"%d\" w:lineRule=\"%s\"", TW(pp->line_height),
                      pp->line_rule == PD_LINE_EXACT ? "exact" : "atLeast");
        } else if (m & PD_PP_LINE_SPACING) {    /* Parade's multiple of the font's line height is Word's auto rule */
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

    if ((m & PD_PP_SNAP_GRID) && !pp->snap_grid) {
        pb_puts(o, "<w:snapToGrid w:val=\"0\"/>");
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

    if (((m & PD_CP_FAMILY) && c->family[0]) || ((m & PD_CP_FAMILY_EA) && c->family_ea[0]) ||
            ((m & PD_CP_FAMILY_CS) && c->family_cs[0])) {
        dx_fonts3(o, (m & PD_CP_FAMILY) && c->family[0] ? c->family : NULL, m & PD_CP_FAMILY_EA ? c->family_ea : NULL,
                  m & PD_CP_FAMILY_CS ? c->family_cs : NULL, c->font_theme);
    }

    if (m & PD_CP_WEIGHT) {
        pb_puts(o, c->weight >= 600 ? "<w:b/>" : "<w:b w:val=\"0\"/>");
    }

    if ((m & PD_CP_WEIGHT_CS) && c->weight_cs) {
        pb_puts(o, c->weight_cs >= 600 ? "<w:bCs/>" : "<w:bCs w:val=\"0\"/>");
    }

    if (m & PD_CP_ITALIC) {
        pb_puts(o, c->italic ? "<w:i/>" : "<w:i w:val=\"0\"/>");
    }

    if ((m & PD_CP_ITALIC_CS) && c->italic_cs >= 0) {
        pb_puts(o, c->italic_cs ? "<w:iCs/>" : "<w:iCs w:val=\"0\"/>");
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
        dx_color(o, c->color, c->color_theme);
    }

    if (m & PD_CP_LETTERSPACE) {
        pb_printf(o, "<w:spacing w:val=\"%d\"/>", TW(c->letter_space));
    }

    if ((m & PD_CP_HSCALE) && c->hscale > 0) {
        pb_printf(o, "<w:w w:val=\"%d\"/>", (int)(c->hscale + 5) / 10);
    }

    if (m & PD_CP_KERNING) {
        pb_printf(o, "<w:kern w:val=\"%d\"/>", c->kerning ? 2 : 0);
    }

    if (m & PD_CP_POSITION) {
        pb_printf(o, "<w:position w:val=\"%d\"/>", (int)SCALE(c->position, 2, 65536));
    }

    if (m & PD_CP_SIZE) {
        pb_printf(o, "<w:sz w:val=\"%d\"/>", (int)SCALE(c->size, 2, 65536));
    }

    if ((m & PD_CP_SIZE_CS) && c->size_cs > 0) {
        pb_printf(o, "<w:szCs w:val=\"%d\"/>", (int)SCALE(c->size_cs, 2, 65536));
    } else if (m & PD_CP_SIZE) {
        pb_printf(o, "<w:szCs w:val=\"%d\"/>", (int)SCALE(c->size, 2, 65536));  /* the same, as Word writes it */
    }

    if ((m & PD_CP_BACKGROUND) && dx_highlight_name(c->background, c->background_theme)) {
        pb_printf(o, "<w:highlight w:val=\"%s\"/>", dx_highlight_name(c->background, c->background_theme));
    }

    if (m & PD_CP_UNDERLINE) {
        pb_printf(o, "<w:u w:val=\"%s\"/>", dx_u_name(c->underline));
    }

    if ((m & PD_CP_BACKGROUND) && c->background && !dx_highlight_name(c->background, c->background_theme)) {
        dx_shd(o, c->background, c->background_theme);
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

/* a style's own properties with what their links to the theme come to now */
static void dx_theme_props(const pd_doc* d, pd_para_props* pp, pd_char_props* cp) {
    pd_theme t;
    int k;

    pd_doc_theme(d, &t);
    pp->border_color = pd_theme_color_resolve(&t, pp->border_theme, pp->border_color);
    pp->shading = pd_theme_color_resolve(&t, pp->shading_theme, pp->shading);
    cp->color = pd_theme_color_resolve(&t, cp->color_theme, cp->color);
    cp->background = pd_theme_color_resolve(&t, cp->background_theme, cp->background);

    for (k = 0; k < 3; k++) {
        int w = (cp->font_theme >> (2 * k)) & 3;
        const char* f = !w ? NULL : k == 0 ? (w == 1 ? t.major : t.minor) : k == 1 ? (w == 1 ? t.major_ea : t.minor_ea) :
                        (w == 1 ? t.major_cs : t.minor_cs);

        if (f && f[0]) {
            snprintf(k == 0 ? cp->family : k == 1 ? cp->family_ea : cp->family_cs, 64, "%s", f);
        }
    }
}

static int dx_theme_of_xml(const char* xml, size_t n, pd_theme* out);

/* DrawingML's names of the theme's colours, in PD_THEME_* order */
static const char* const dx_clr_names[PD_THEME_COLORS] = { "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3",
                                                           "accent4", "accent5", "accent6", "hlink", "folHlink"
                                                         };

static void dx_clr_scheme(pd_buf* o, const char* pre, const pd_theme* t) {
    int i;

    pb_printf(o, "<%sclrScheme name=\"", pre);
    xesc(o, t->name, strlen(t->name));
    pb_puts(o, "\">");

    for (i = 0; i < PD_THEME_COLORS; i++) {
        pb_printf(o, "<%s%s><%ssrgbClr val=\"%06X\"/></%s%s>", pre, dx_clr_names[i], pre,
                  (unsigned)(t->color[i] & 0xFFFFFF), pre, dx_clr_names[i]);
    }

    pb_printf(o, "</%sclrScheme>", pre);
}

static void dx_font_set(pd_buf* o, const char* tag, const char* lat, const char* ea, const char* cs) {
    pb_printf(o, "<a:%s><a:latin typeface=\"", tag);
    xesc(o, lat, strlen(lat));
    pb_puts(o, "\"/><a:ea typeface=\"");
    xesc(o, ea, strlen(ea));
    pb_puts(o, "\"/><a:cs typeface=\"");
    xesc(o, cs, strlen(cs));
    pb_printf(o, "\"/></a:%s>", tag);
}

/* a theme part of the document's theme, Office's formatting scheme with it */
static void dx_theme_new(pd_buf* o, const pd_theme* t) {
    pb_puts(o, XML_DECL);
    pb_puts(o, "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"");
    xesc(o, t->name, strlen(t->name));
    pb_puts(o, "\"><a:themeElements>");
    dx_clr_scheme(o, "a:", t);
    pb_puts(o, "<a:fontScheme name=\"");
    xesc(o, t->name, strlen(t->name));
    pb_puts(o, "\">");
    dx_font_set(o, "majorFont", t->major, t->major_ea, t->major_cs);
    dx_font_set(o, "minorFont", t->minor, t->minor_ea, t->minor_cs);
    pb_puts(o, "</a:fontScheme><a:fmtScheme name=\"Office\"><a:fillStyleLst>"
            "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
            "<a:gradFill rotWithShape=\"1\"><a:gsLst>"
            "<a:gs pos=\"0\"><a:schemeClr val=\"phClr\"><a:lumMod val=\"110000\"/><a:satMod val=\"105000\"/>"
            "<a:tint val=\"67000\"/></a:schemeClr></a:gs>"
            "<a:gs pos=\"50000\"><a:schemeClr val=\"phClr\"><a:lumMod val=\"105000\"/><a:satMod val=\"103000\"/>"
            "<a:tint val=\"73000\"/></a:schemeClr></a:gs>"
            "<a:gs pos=\"100000\"><a:schemeClr val=\"phClr\"><a:lumMod val=\"105000\"/><a:satMod val=\"109000\"/>"
            "<a:tint val=\"81000\"/></a:schemeClr></a:gs></a:gsLst><a:lin ang=\"5400000\" scaled=\"0\"/></a:gradFill>"
            "<a:gradFill rotWithShape=\"1\"><a:gsLst>"
            "<a:gs pos=\"0\"><a:schemeClr val=\"phClr\"><a:satMod val=\"103000\"/><a:lumMod val=\"102000\"/>"
            "<a:tint val=\"94000\"/></a:schemeClr></a:gs>"
            "<a:gs pos=\"50000\"><a:schemeClr val=\"phClr\"><a:satMod val=\"110000\"/><a:lumMod val=\"100000\"/>"
            "<a:shade val=\"100000\"/></a:schemeClr></a:gs>"
            "<a:gs pos=\"100000\"><a:schemeClr val=\"phClr\"><a:lumMod val=\"99000\"/><a:satMod val=\"120000\"/>"
            "<a:shade val=\"78000\"/></a:schemeClr></a:gs></a:gsLst><a:lin ang=\"5400000\" scaled=\"0\"/></a:gradFill>"
            "</a:fillStyleLst><a:lnStyleLst>"
            "<a:ln w=\"6350\" cap=\"flat\" cmpd=\"sng\" algn=\"ctr\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
            "<a:prstDash val=\"solid\"/><a:miter lim=\"800000\"/></a:ln>"
            "<a:ln w=\"12700\" cap=\"flat\" cmpd=\"sng\" algn=\"ctr\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
            "<a:prstDash val=\"solid\"/><a:miter lim=\"800000\"/></a:ln>"
            "<a:ln w=\"19050\" cap=\"flat\" cmpd=\"sng\" algn=\"ctr\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
            "<a:prstDash val=\"solid\"/><a:miter lim=\"800000\"/></a:ln>"
            "</a:lnStyleLst><a:effectStyleLst>"
            "<a:effectStyle><a:effectLst/></a:effectStyle><a:effectStyle><a:effectLst/></a:effectStyle>"
            "<a:effectStyle><a:effectLst><a:outerShdw blurRad=\"57150\" dist=\"19050\" dir=\"5400000\" algn=\"ctr\" "
            "rotWithShape=\"0\"><a:srgbClr val=\"000000\"><a:alpha val=\"63000\"/></a:srgbClr></a:outerShdw>"
            "</a:effectLst></a:effectStyle></a:effectStyleLst><a:bgFillStyleLst>"
            "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
            "<a:solidFill><a:schemeClr val=\"phClr\"><a:tint val=\"95000\"/><a:satMod val=\"170000\"/></a:schemeClr>"
            "</a:solidFill>"
            "<a:gradFill rotWithShape=\"1\"><a:gsLst>"
            "<a:gs pos=\"0\"><a:schemeClr val=\"phClr\"><a:tint val=\"93000\"/><a:satMod val=\"150000\"/>"
            "<a:shade val=\"98000\"/><a:lumMod val=\"102000\"/></a:schemeClr></a:gs>"
            "<a:gs pos=\"50000\"><a:schemeClr val=\"phClr\"><a:tint val=\"98000\"/><a:satMod val=\"130000\"/>"
            "<a:shade val=\"90000\"/><a:lumMod val=\"103000\"/></a:schemeClr></a:gs>"
            "<a:gs pos=\"100000\"><a:schemeClr val=\"phClr\"><a:shade val=\"63000\"/><a:satMod val=\"120000\"/>"
            "</a:schemeClr></a:gs></a:gsLst><a:lin ang=\"5400000\" scaled=\"0\"/></a:gradFill>"
            "</a:bgFillStyleLst></a:fmtScheme></a:themeElements><a:objectDefaults/><a:extraClrSchemeLst/></a:theme>");
}

/* where a tag (prefixed or not) starts in s[from, to), or to */
static size_t dx_tag_at(const char* s, size_t from, size_t to, const char* local, int closing) {
    size_t i, n = strlen(local);

    for (i = from; i + n + 2 < to; i++) {
        size_t j = i + 1;

        if (s[i] != '<' || (closing ? s[j++] != '/' : s[j] == '/')) {
            continue;
        }

        while (j < to && s[j] != ':' && s[j] != '>' && s[j] != ' ' && s[j] != '/' && j - i < 24) {
            j++;
        }

        j = j < to && s[j] == ':' ? j + 1 : i + 1 + (closing ? 1 : 0);

        if (j + n < to && !strncmp(s + j, local, n) && (s[j + n] == '>' || s[j + n] == ' ' || s[j + n] == '/')) {
            return i;
        }
    }

    return to;
}

/* the kept theme part with the document's colours and fonts in it, where they changed; else the part as it was */
static void dx_theme_patch(pd_buf* o, const char* s, size_t n, const pd_theme* t) {
    pd_theme was;
    size_t a, b, at = 0, k;
    char pre[24] = "";

    if (!dx_theme_of_xml(s, n, &was) || (!memcmp(was.color, t->color, sizeof(t->color)) && !strcmp(was.major, t->major) &&
                                        !strcmp(was.minor, t->minor) && !strcmp(was.major_ea, t->major_ea) &&
                                        !strcmp(was.minor_ea, t->minor_ea) && !strcmp(was.major_cs, t->major_cs) &&
                                        !strcmp(was.minor_cs, t->minor_cs))) {
        pb_put(o, s, n);
        return;
    }

    /* the colour scheme made again, in the part's own prefix */
    a = dx_tag_at(s, 0, n, "clrScheme", 0);
    b = a < n ? dx_tag_at(s, a, n, "clrScheme", 1) : n;

    if (a < n && b < n) {
        for (k = a + 1; k < n && s[k] != ':' && s[k] != ' ' && s[k] != '>' && k - a < sizeof(pre); k++) {
        }

        if (s[k] == ':') {
            snprintf(pre, sizeof(pre), "%.*s", (int)(k - a), s + a + 1);
        }

        pb_put(o, s, a);
        dx_clr_scheme(o, pre, t);
        at = b;

        while (at < n && s[at] != '>') {
            at++;
        }

        at = at < n ? at + 1 : n;
    }

    /* each font set's latin, ea and cs typefaces */
    for (k = 0; k < 2; k++) {
        size_t fa = dx_tag_at(s, at, n, k ? "minorFont" : "majorFont", 0), fb;
        static const char* const slot[3] = { "latin", "ea", "cs" };
        int q;

        if (fa >= n) {
            continue;
        }

        fb = dx_tag_at(s, fa, n, k ? "minorFont" : "majorFont", 1);

        for (q = 0; q < 3; q++) {
            const char* v = q == 0 ? (k ? t->minor : t->major) : q == 1 ? (k ? t->minor_ea : t->major_ea) :
                            (k ? t->minor_cs : t->major_cs);
            size_t e = dx_tag_at(s, fa, fb, slot[q], 0), tf, te;

            if (e >= fb) {
                continue;
            }

            for (tf = e; tf + 10 < fb && strncmp(s + tf, "typeface=\"", 10) && s[tf] != '>'; tf++) {
            }

            if (tf + 10 >= fb || s[tf] == '>') {
                continue;
            }

            tf += 10;

            for (te = tf; te < fb && s[te] != '"'; te++) {
            }

            if (tf < at) {
                continue;
            }

            pb_put(o, s + at, tf - at);
            xesc(o, v, strlen(v));
            at = te;
        }

    }

    pb_put(o, s + at, n - at);
}

/* the document's theme as a theme part: the one it came with (its colours and fonts as they are now) when that is
   whole, else one made of pd_doc_theme with Office's formats -- for a package that needs one (pptx) */
static int dx_kept(const pd_doc* d, const char* type, const void** data, size_t* len);
static const char* dw_memmem(const char* h, size_t n, const char* needle, size_t nn);

void pd_docx_theme_part(const pd_doc* d, pd_buf* o) {
    const void* theme = NULL;
    size_t len = 0;
    pd_theme th;

    pd_doc_theme(d, &th);

    if (dx_kept(d, THEME_MIME, &theme, &len) && dw_memmem((const char*)theme, len, "fillStyleLst", 12) &&
            dw_memmem((const char*)theme, len, "bgFillStyleLst", 14)) {
        dx_theme_patch(o, (const char*)theme, len, &th);
    } else {
        dx_theme_new(o, &th);
    }
}

/* the rules of a cell (w:tcBorders) or a table (w:tblBorders, with inside): the edges said, ruled or nil */
static void dx_rules(pd_buf* o, const char* tag, int set, int on, pd_sp w, const pd_sp* ew, uint32_t c, uint32_t ref,
                     int table) {
    static const char* const edge[] = { "top", "left", "bottom", "right", "insideH", "insideV" };
    static const int bit[] = { PD_TBORDER_TOP, PD_TBORDER_LEFT, PD_TBORDER_BOTTOM, PD_TBORDER_RIGHT,
                               PD_TBORDER_INSIDE_H, PD_TBORDER_INSIDE_V
                             };
    static const int slot[] = { 0, 3, 2, 1 };  /* edge_width's order: top, right, bottom, left */
    int k;

    if (!set) {
        return;
    }

    pb_printf(o, "<w:%s>", tag);

    for (k = 0; k < (table ? 6 : 4); k++) {
        if (!(set & bit[k])) {
            continue;
        }

        if (on & bit[k]) {
            pd_sp ww = ew && k < 4 && ew[slot[k]] > 0 ? ew[slot[k]] : w;
            int sz = (int)SCALE(ww, 8, 65536);

            pb_printf(o, "<w:%s w:val=\"single\" w:sz=\"%d\" w:space=\"0\" w:color=\"%06X\"", edge[k], sz < 2 ? 2 : sz,
                      (unsigned)(c & 0xFFFFFF));
            dx_theme_attr(o, ref, 0);
            pb_puts(o, "/>");
        } else {
            pb_printf(o, "<w:%s w:val=\"nil\"/>", edge[k]);
        }
    }

    pb_printf(o, "</w:%s>", tag);
}

/* a part's paragraph, run and cell properties */
static void dx_tpart_body(dxo* x, pd_buf* o, pd_table_style_part* p) {
    dx_theme_props(x->d, &p->para, &p->chr);

    if (p->para.mask) {
        pb_puts(o, "<w:pPr>");
        dx_ppr_head(o, &p->para, p->para.mask);
        dx_ppr_tail(o, &p->para, p->para.mask, x->hyph_auto, NULL);
        pb_puts(o, "</w:pPr>");
    }

    if (p->chr.mask) {
        pb_puts(o, "<w:rPr>");
        dx_rpr_set(o, &p->chr);
        pb_puts(o, "</w:rPr>");
    }
}

static void dx_tpart_cell(dxo* x, pd_buf* o, const pd_table_style_part* p) {
    pd_theme t;

    if (!p->border_set && !p->has_shading) {
        return;
    }

    pd_doc_theme(x->d, &t);
    pb_puts(o, "<w:tcPr>");
    dx_rules(o, "tcBorders", p->border_set, p->border_on, p->border_width, p->edge_width,
             pd_theme_color_resolve(&t, p->border_theme, p->border_color), p->border_theme, 0);

    if (p->has_shading && p->shading) {
        dx_shd(o, pd_theme_color_resolve(&t, p->shading_theme, p->shading), p->shading_theme);
    } else if (p->has_shading) {
        pb_puts(o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"auto\"/>");
    }

    pb_puts(o, "</w:tcPr>");
}

/* a table style of the document's: the whole table's properties, then each condition it has */
static void dx_table_style(dxo* x, pd_buf* o, pd_style_id sid, const char* name, pd_style_id parent) {
    static const char* const names[PD_TPART_COUNT] = { "wholeTable", "band1Vert", "band2Vert", "band1Horz",
                                                      "band2Horz", "firstCol", "lastCol", "firstRow", "lastRow",
                                                      "neCell", "nwCell", "seCell", "swCell"
                                                    };
    pd_table_style* ts = (pd_table_style*)malloc(sizeof(pd_table_style));
    char id[64], pid[64];
    pd_theme t;
    int k;

    if (!ts || pd_doc_table_style_info(x->d, sid, NULL, ts) != PD_OK) {
        free(ts);
        return;
    }

    pd_doc_theme(x->d, &t);
    dx_sid(x->d, sid, id, sizeof(id));
    pb_printf(o, "<w:style w:type=\"table\" w:styleId=\"%s\"><w:name w:val=\"", id);
    xesc(o, name, strlen(name));
    pb_puts(o, "\"/>");

    if (parent) {
        dx_sid(x->d, parent, pid, sizeof(pid));
        pb_printf(o, "<w:basedOn w:val=\"%s\"/>", pid);
    }

    pb_puts(o, "<w:uiPriority w:val=\"59\"/>");
    dx_tpart_body(x, o, &ts->part[PD_TPART_WHOLE]);
    pb_puts(o, "<w:tblPr>");

    if (ts->row_band > 0) {
        pb_printf(o, "<w:tblStyleRowBandSize w:val=\"%d\"/>", (int)ts->row_band);
    }

    if (ts->col_band > 0) {
        pb_printf(o, "<w:tblStyleColBandSize w:val=\"%d\"/>", (int)ts->col_band);
    }

    dx_rules(o, "tblBorders", ts->border_set, ts->border_on, ts->border_width, NULL,
             pd_theme_color_resolve(&t, ts->border_theme, ts->border_color), ts->border_theme, 1);

    if (ts->cell_padding >= 0 || ts->cell_padding_v >= 0) {
        pb_puts(o, "<w:tblCellMar>");

        if (ts->cell_padding_v >= 0) {
            pb_printf(o, "<w:top w:w=\"%d\" w:type=\"dxa\"/>", TW(ts->cell_padding_v));
        }

        if (ts->cell_padding >= 0) {
            pb_printf(o, "<w:left w:w=\"%d\" w:type=\"dxa\"/>", TW(ts->cell_padding));
        }

        if (ts->cell_padding_v >= 0) {
            pb_printf(o, "<w:bottom w:w=\"%d\" w:type=\"dxa\"/>", TW(ts->cell_padding_v));
        }

        if (ts->cell_padding >= 0) {
            pb_printf(o, "<w:right w:w=\"%d\" w:type=\"dxa\"/>", TW(ts->cell_padding));
        }

        pb_puts(o, "</w:tblCellMar>");
    }

    pb_puts(o, "</w:tblPr>");
    dx_tpart_cell(x, o, &ts->part[PD_TPART_WHOLE]);

    for (k = 1; k < PD_TPART_COUNT; k++) {
        if (!ts->part[k].given) {
            continue;
        }

        pb_printf(o, "<w:tblStylePr w:type=\"%s\">", names[k]);
        dx_tpart_body(x, o, &ts->part[k]);
        pb_puts(o, "<w:tblPr/>");
        dx_tpart_cell(x, o, &ts->part[k]);
        pb_puts(o, "</w:tblStylePr>");
    }

    pb_puts(o, "</w:style>");
    free(ts);
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

        if (kind == PD_STYLE_TABLE) {
            dx_table_style(x, o, sid, name, parent);
            continue;
        }

        dx_theme_props(d, &pp, &cp);
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
    if (!pd_doc_style_find(d, "Table Grid")) {  /* the writer's own, unless the document has a table style of the name */
        pb_puts(o, "<w:style w:type=\"table\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/><w:tblPr>"
                "<w:tblCellMar><w:left w:w=\"108\" w:type=\"dxa\"/><w:right w:w=\"108\" w:type=\"dxa\"/>"
                "</w:tblCellMar></w:tblPr></w:style>");
    }
    pb_puts(o, "</w:styles>");
}


/* one abstract numbering per list, from its own levels; one instance of it
   per list, so each counts on its own */
static void dx_numbering(dxo* x, pd_buf* o) {
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
            int f = L.format >= PD_NUM_BULLET && L.format <= PD_NUM_LAST ? L.format : PD_NUM_DECIMAL;

            if (k >= nlv) {     /* levels the list lacks: like its deepest, further in */
                L.indent += PD_PT(18) * (k - (nlv > 0 ? nlv - 1 : 0));

                if (f != PD_NUM_BULLET && f != PD_NUM_NONE) {
                    snprintf(L.text, sizeof(L.text), "%%%d.", k + 1);
                }
            }

            pb_printf(o, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"%d\"/><w:numFmt w:val=\"%s\"/>", k,
                      (int)(L.start >= 0 ? L.start : 1), num_fmt_name(f));

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

/* the latest resource of a type: a part kept whole (the theme) */
static int dx_kept(const pd_doc* d, const char* type, const void** data, size_t* len) {
    const char* mime;
    const void* p;
    size_t n;
    pd_res_id r;
    int found = 0;

    for (r = 1; pd_doc_resource(d, r, &mime, &p, &n) == PD_OK; r++) {
        if (strcmp(mime, type) == 0) {
            *data = p;
            *len = n;
            found = 1;
        }
    }

    return found;
}

/* a font the document carries, written embedded: its resource, part number and obfuscation key */
typedef struct {
    pd_res_id res;
    unsigned char key[16];
} dembed;

/* a carried font's resource type: its family, weight and italic (0 when it is no such font) */
static int embed_of(const char* mime, char* family, size_t cap, int* bold, int* italic) {
    const char* f = strstr(mime, "family=\""), *e, *w, *it;

    if (strncmp(mime, "font/ttf;", 9) != 0 || !f) {
        return 0;
    }

    f += 8;

    if ((e = strchr(f, '"')) == NULL || (size_t)(e - f) >= cap) {
        return 0;
    }

    memcpy(family, f, (size_t)(e - f));
    family[e - f] = '\0';
    w = strstr(e, "weight=");
    it = strstr(e, "italic=");
    *bold = w && atoi(w + 7) >= 600;
    *italic = it && atoi(it + 7) != 0;
    return 1;
}

/* a font's embed elements (and their relationships, the fonts to write in emb) for the fonts the document
   carries of a family, in Word's order: regular, bold, italic, bold italic */
static void dx_font_embeds(const pd_doc* d, const char* fam, size_t flen, pd_buf* o, pd_buf* rels, dembed* emb,
                           int* nemb) {
    static const char* el[4] = { "embedRegular", "embedBold", "embedItalic", "embedBoldItalic" };
    static const int pos[16] = { 35, 33, 31, 29, 27, 25, 22, 20, 17, 15, 12, 10, 7, 5, 3, 1 };
    int v;

    for (v = 0; v < 4; v++) {
        const char* mime;
        const void* data;
        size_t n;
        pd_res_id r;

        for (r = 1; pd_doc_resource(d, r, &mime, &data, &n) == PD_OK; r++) {
            char family[64], guid[39] = "{00000000-0000-0000-0000-000000000000}";
            int bold, italic, i;
            uint32_t h;

            if (!embed_of(mime, family, sizeof(family), &bold, &italic) || strlen(family) != flen ||
                    memcmp(family, fam, flen) != 0 || bold + 2 * italic != v || n <= 32 || *nemb >= 64) {
                continue;
            }

            emb[*nemb].res = r;
            h = pd_crc32(data, n);     /* a key from the font itself: the same file, the same bytes */

            for (i = 0; i < 16; i++) {
                h = h * 1103515245u + 12345u + (uint32_t)i;
                emb[*nemb].key[i] = (unsigned char)(h >> 24);
                guid[pos[i]] = "0123456789ABCDEF"[emb[*nemb].key[i] >> 4];
                guid[pos[i] + 1] = "0123456789ABCDEF"[emb[*nemb].key[i] & 15];
            }

            ++*nemb;
            pb_printf(o, "<w:%s r:id=\"rIdF%d\" w:fontKey=\"%s\"/>", el[v], *nemb, guid);
            pb_printf(rels, "<Relationship Id=\"rIdF%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                      "relationships/font\" Target=\"fonts/font%d.odttf\"/>", *nemb, *nemb);
            break;
        }
    }
}

/* the document's font table (PD_FONT_TABLE_MIME) as word/fontTable.xml, with the fonts it carries embedded
   (their relationships into rels, which to write into emb); 0 when it has none */
static int dx_font_table(const pd_doc* d, pd_buf* o, pd_buf* rels, dembed* emb, int* nemb) {
    static const char* generic[] = { "auto", "roman", "swiss", "modern", "script", "decorative" };
    static const char* pitch[] = { "default", "fixed", "variable" };
    const char* table = NULL, *mime, *p, *end;
    const void* data;
    size_t len = 0, n;
    pd_res_id r;

    for (r = 1; pd_doc_resource(d, r, &mime, &data, &n) == PD_OK; r++) {
        if (strcmp(mime, PD_FONT_TABLE_MIME) == 0) {
            table = (const char*)data;      /* the latest */
            len = n;
        }
    }

    if (!table) {
        return 0;
    }

    pb_puts(o, XML_DECL);
    pb_printf(o, "<w:fonts %s>", W_NS);

    for (p = table, end = table + len; p < end;) {
        const char* eol = (const char*)memchr(p, '\n', (size_t)(end - p)), *f[6];
        size_t fl[6];
        int k;

        eol = eol ? eol : end;

        for (k = 0; k < 6; k++) {
            const char* tab = p < eol ? (const char*)memchr(p, '\t', (size_t)(eol - p)) : NULL;

            f[k] = p;
            fl[k] = (size_t)((tab ? tab : eol) - p);
            p = tab ? tab + 1 : eol;
        }

        p = eol + 1;

        if (!fl[0]) {
            continue;
        }

        pb_puts(o, "<w:font w:name=\"");
        xesc(o, f[0], fl[0]);
        pb_puts(o, "\">");

        if (fl[1]) {
            pb_puts(o, "<w:altName w:val=\"");
            xesc(o, f[1], fl[1]);
            pb_puts(o, "\"/>");
        }

        if (fl[4] == 20) {
            pb_printf(o, "<w:panose1 w:val=\"%.20s\"/>", f[4]);
        }

        if (fl[5] == 2) {
            pb_printf(o, "<w:charset w:val=\"%.2s\"/>", f[5]);
        }

        k = fl[2] ? f[2][0] - '0' : 0;
        pb_printf(o, "<w:family w:val=\"%s\"/>", generic[k >= 0 && k <= 5 ? k : 0]);
        k = fl[3] ? f[3][0] - '0' : 0;
        pb_printf(o, "<w:pitch w:val=\"%s\"/>", pitch[k >= 0 && k <= 2 ? k : 0]);
        dx_font_embeds(d, f[0], fl[0], o, rels, emb, nemb);
        pb_puts(o, "</w:font>");
    }

    pb_puts(o, "</w:fonts>");
    return 1;
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


/* ------------------------------------------------------------------ */
/* parts kept as they came                                            */
/* ------------------------------------------------------------------ */

/* What Parade does not model goes back as it came: the package's extended and custom properties and thumbnail,
   custom XML data, the glossary (building blocks), web settings, and settings.xml itself, merged on writing with
   what Parade sets.  Kept in one resource of the document, entries of
   name NUL content-type NUL source NUL relationship-type NUL length(4, little endian) bytes,
   source "pkg" (from _rels/.rels), "doc" (from document.xml's), "" (a part those parts reach). */
#define KEEP_MIME "application/vnd.parade.docx-kept-parts"
#define KEEP_MAX 400

typedef struct {
    pd_buf b;
    int n;
    char names[KEEP_MAX][128];
} dkeep;

/* a relationship target from the part at base resolved to a part name ("word/x.xml" + "../customXml/a.xml") */
static void keep_resolve(const char* base, const char* target, char* out, size_t cap) {
    char tmp[256];
    size_t n;
    const char* t = target;

    if (*t == '/') {
        snprintf(out, cap, "%s", t + 1);
        return;
    }

    snprintf(tmp, sizeof(tmp), "%s", base);
    n = strlen(tmp);

    while (n > 0 && tmp[n - 1] != '/') {    /* the base's folder */
        n--;
    }

    tmp[n] = '\0';

    while (strncmp(t, "../", 3) == 0) {
        t += 3;

        if (n > 0) {
            n--;

            while (n > 0 && tmp[n - 1] != '/') {
                n--;
            }

            tmp[n] = '\0';
        }
    }

    while (strncmp(t, "./", 2) == 0) {
        t += 2;
    }

    snprintf(out, cap, "%s%s", tmp, t);
}

/* the relationships part of a part ("word/document.xml" -> "word/_rels/document.xml.rels") */
static void keep_rels_name(const char* part, char* out, size_t cap) {
    const char* sl = strrchr(part, '/');

    if (sl) {
        snprintf(out, cap, "%.*s_rels/%s.rels", (int)(sl - part + 1), part, sl + 1);
    } else {
        snprintf(out, cap, "_rels/%s.rels", part);
    }
}

static int keep_ext_eq(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
    }

    return !*a && !*b;
}

/* a part's content type from [Content_Types].xml: its override, else its extension's default */
static void keep_ctype(const char* ct, size_t ctn, const char* name, char* out, size_t cap) {
    pd_markup m;
    char v[256];
    const char* ext = strrchr(name, '.');

    out[0] = '\0';
    mu_init(&m, ct, ctn, 0);

    while (mu_next(&m) != MT_END) {
        if (m.type != MT_OPEN && m.type != MT_EMPTY) {
            continue;
        }

        if (strcmp(mu_local(m.name), "Override") == 0 && mu_attr(&m, "PartName", v, sizeof(v)) &&
                strcmp(v[0] == '/' ? v + 1 : v, name) == 0) {
            mu_attr(&m, "ContentType", out, cap);
            return;
        }

        if (ext && !out[0] && strcmp(mu_local(m.name), "Default") == 0 && mu_attr(&m, "Extension", v, sizeof(v)) &&
                keep_ext_eq(v, ext + 1)) {
            mu_attr(&m, "ContentType", out, cap);
        }
    }
}

static int keep_has(const dkeep* k, const char* name) {
    int i;

    for (i = 0; i < k->n; i++) {
        if (strcmp(k->names[i], name) == 0) {
            return 1;
        }
    }

    return 0;
}

static void keep_entry(dkeep* k, const char* name, const char* ctype, const char* src, const char* rtype,
                       const void* data, size_t len) {
    unsigned char l4[4];

    if (k->n >= KEEP_MAX || strlen(name) >= sizeof(k->names[0]) || keep_has(k, name)) {
        return;
    }

    snprintf(k->names[k->n++], sizeof(k->names[0]), "%s", name);
    pb_put(&k->b, name, strlen(name) + 1);
    pb_put(&k->b, ctype, strlen(ctype) + 1);
    pb_put(&k->b, src, strlen(src) + 1);
    pb_put(&k->b, rtype, strlen(rtype) + 1);
    l4[0] = (unsigned char)(len & 255);
    l4[1] = (unsigned char)((len >> 8) & 255);
    l4[2] = (unsigned char)((len >> 16) & 255);
    l4[3] = (unsigned char)((len >> 24) & 255);
    pb_put(&k->b, l4, 4);
    pb_put(&k->b, data, len);
}

/* a part kept, with its relationships and the parts they reach inside the package */
static void keep_part(dkeep* k, const zipr* z, const char* ct, size_t ctn, const char* name, const char* src,
                      const char* rtype, int depth) {
    unsigned char* data;
    size_t len;
    char ctype[256], rn[300];

    if (depth > 4 || keep_has(k, name) || (data = zip_read(z, name, &len)) == NULL) {
        return;
    }

    keep_ctype(ct, ctn, name, ctype, sizeof(ctype));
    keep_entry(k, name, ctype, src, rtype, data, len);
    free(data);
    keep_rels_name(name, rn, sizeof(rn));

    if ((data = zip_read(z, rn, &len)) != NULL) {
        pd_markup m;
        char t[256], mode[32], full[300];

        keep_entry(k, rn, "application/vnd.openxmlformats-package.relationships+xml", "", "", data, len);
        mu_init(&m, (const char*)data, len, 0);

        while (mu_next(&m) != MT_END) {
            if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "Relationship") == 0 &&
                    mu_attr(&m, "Target", t, sizeof(t)) &&
                    !(mu_attr(&m, "TargetMode", mode, sizeof(mode)) && strcmp(mode, "External") == 0)) {
                keep_resolve(name, t, full, sizeof(full));
                keep_part(k, z, ct, ctn, full, "", "", depth + 1);
            }
        }

        free(data);
    }
}

/* the relationship types whose parts are kept, from the package (pkg) or the main document (doc) */
static int keep_type(const char* type, int doc) {
    static const char* pkg[] = { "/extended-properties", "/custom-properties", "/custom-properties" };
    static const char* dt[] = { "/customXml", "/glossaryDocument", "/webSettings" };
    const char** l = doc ? dt : pkg;
    size_t tl = strlen(type), i;

    for (i = 0; i < 3; i++) {
        size_t ln = strlen(l[i]);

        if (tl >= ln && strcmp(type + tl - ln, l[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

/* what of a package is kept as it came, made a resource of d */
static void keep_read(const zipr* z, pd_doc* d) {
    dkeep* k = (dkeep*)calloc(1, sizeof(dkeep));
    unsigned char* ct, *rels;
    size_t ctn = 0, rn = 0;
    int doc;

    if (!k) {
        return;
    }

    ct = zip_read(z, "[Content_Types].xml", &ctn);

    for (doc = 0; doc < 2; doc++) {
        pd_markup m;

        if ((rels = zip_read(z, doc ? "word/_rels/document.xml.rels" : "_rels/.rels", &rn)) == NULL) {
            continue;
        }

        mu_init(&m, (const char*)rels, rn, 0);

        while (mu_next(&m) != MT_END) {
            char type[256], t[256], mode[32], full[300];

            if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "Relationship") == 0 &&
                    mu_attr(&m, "Type", type, sizeof(type)) && mu_attr(&m, "Target", t, sizeof(t)) &&
                    keep_type(type, doc) && !(mu_attr(&m, "TargetMode", mode, sizeof(mode)) &&
                                              strcmp(mode, "External") == 0)) {
                keep_resolve(doc ? "word/document.xml" : "", t, full, sizeof(full));
                keep_part(k, z, ct ? (const char*)ct : "", ctn, full, doc ? "doc" : "pkg", type, 0);
            }
        }

        free(rels);
    }

    {   /* settings.xml, to be merged; its relationships only when they all point out of the package */
        unsigned char* s = zip_read(z, "word/settings.xml", &rn);

        if (s) {
            unsigned char* sr = zip_read(z, "word/_rels/settings.xml.rels", &ctn);

            keep_entry(k, "word/settings.xml", "", "settings", "", s, rn);

            if (sr) {
                pd_markup m;
                int all_ext = 1;
                char mode[32];

                mu_init(&m, (const char*)sr, ctn, 0);

                while (mu_next(&m) != MT_END) {
                    if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "Relationship") == 0 &&
                            !(mu_attr(&m, "TargetMode", mode, sizeof(mode)) && strcmp(mode, "External") == 0)) {
                        all_ext = 0;
                    }
                }

                if (all_ext) {
                    keep_entry(k, "word/_rels/settings.xml.rels", "", "settings-rels", "", sr, ctn);
                }

                free(sr);
            }

            free(s);
        }
    }

    if (k->n > 0 && !k->b.err) {
        pd_res_id r;

        pd_doc_add_resource(d, KEEP_MIME, k->b.p, k->b.n, &r);
    }

    free(ct);
    pb_free(&k->b);
    free(k);
}

typedef struct {
    const char* name, *ctype, *src, *rtype;
    const unsigned char* data;
    size_t len;
} dkept;

/* the next entry of a kept-parts resource at *pos; 0 at its end or where it is broken */
static int keep_next(const unsigned char* p, size_t n, size_t* pos, dkept* e) {
    const char* f[4];
    size_t i = *pos, len;
    int k;

    for (k = 0; k < 4; k++) {
        const unsigned char* z = i < n ? (const unsigned char*)memchr(p + i, 0, n - i) : NULL;

        if (!z) {
            return 0;
        }

        f[k] = (const char*)p + i;
        i = (size_t)(z - p) + 1;
    }

    if (i + 4 > n) {
        return 0;
    }

    len = (size_t)p[i] | ((size_t)p[i + 1] << 8) | ((size_t)p[i + 2] << 16) | ((size_t)p[i + 3] << 24);
    i += 4;

    if (len > n - i) {
        return 0;
    }

    e->name = f[0];
    e->ctype = f[1];
    e->src = f[2];
    e->rtype = f[3];
    e->data = p + i;
    e->len = len;
    *pos = i + len;
    return 1;
}

/* relationships to the kept parts of a source ("pkg": the package's, "doc": document.xml's) */
static void dx_keep_rels(pd_buf* o, const void* kdata, size_t klen, const char* src) {
    size_t kp = 0;
    dkept e;
    int i = 0;

    while (klen && keep_next((const unsigned char*)kdata, klen, &kp, &e)) {
        if (strcmp(e.src, src) != 0) {
            continue;
        }

        pb_printf(o, "<Relationship Id=\"rIdK%d\" Type=\"", ++i);
        xesc(o, e.rtype, strlen(e.rtype));
        pb_puts(o, "\" Target=\"");

        if (strcmp(src, "doc") == 0) {     /* from word/ */
            if (strncmp(e.name, "word/", 5) == 0) {
                xesc(o, e.name + 5, strlen(e.name + 5));
            } else {
                pb_puts(o, "../");
                xesc(o, e.name, strlen(e.name));
            }
        } else {
            xesc(o, e.name, strlen(e.name));
        }

        pb_puts(o, "\"/>");
    }
}

/* the top-level children of a settings part: where each starts and ends, and its name */
typedef struct {
    size_t a, b;
    char name[64];
} dsel;

static int settings_children(const char* s, size_t n, dsel* out, int cap, size_t* body_a, size_t* body_b) {
    pd_markup m;
    int depth = 0, cnt = 0;
    size_t before = 0;

    *body_a = *body_b = 0;
    mu_init(&m, s, n, 0);

    for (;;) {
        int t;

        before = m.pos;
        t = mu_next(&m);

        if (t == MT_END) {
            break;
        }

        if (t == MT_OPEN || t == MT_EMPTY) {
            if (depth == 0) {
                if (strcmp(mu_local(m.name), "settings") == 0 && t == MT_OPEN) {
                    *body_a = m.pos;
                    depth = 1;
                }

                continue;
            }

            if (depth == 1 && cnt < cap) {
                out[cnt].a = before;
                snprintf(out[cnt].name, sizeof(out[cnt].name), "%s", m.name);
                out[cnt].b = m.pos;

                if (t == MT_EMPTY) {
                    cnt++;
                }
            }

            if (t == MT_OPEN) {
                depth++;
            }
        } else if (t == MT_CLOSE) {
            depth--;

            if (depth == 1 && cnt < cap) {
                out[cnt++].b = m.pos;
            } else if (depth == 0) {
                *body_b = before;
                break;
            }
        }
    }

    return cnt;
}

/* where a settings element goes, in CT_Settings's order (extensions: after what precedes them) */
static int settings_rank(const char* qname) {
    static const char* order[] = {
        "writeProtection", "view", "zoom", "removePersonalInformation", "removeDateAndTime",
        "doNotDisplayPageBoundaries", "displayBackgroundShape", "printPostScriptOverText",
        "printFractionalCharacterWidth", "printFormsData", "embedTrueTypeFonts", "embedSystemFonts",
        "saveSubsetFonts", "saveFormsData", "mirrorMargins", "alignBordersAndEdges", "bordersDoNotSurroundHeader",
        "bordersDoNotSurroundFooter", "gutterAtTop", "hideSpellingErrors", "hideGrammaticalErrors",
        "activeWritingStyle", "proofState", "formsDesign", "attachedTemplate", "linkStyles",
        "stylePaneFormatFilter", "stylePaneSortMethod", "documentType", "mailMerge", "revisionView",
        "trackRevisions", "doNotTrackMoves", "doNotTrackFormatting", "documentProtection", "autoFormatOverride",
        "styleLockTheme", "styleLockQFSet", "defaultTabStop", "autoHyphenation", "consecutiveHyphenLimit",
        "hyphenationZone", "doNotHyphenateCaps", "showEnvelope", "summaryLength", "clickAndTypeStyle",
        "defaultTableStyle", "evenAndOddHeaders", "bookFoldRevPrinting", "bookFoldPrinting",
        "bookFoldPrintingSheets", "drawingGridHorizontalSpacing", "drawingGridVerticalSpacing",
        "displayHorizontalDrawingGridEvery", "displayVerticalDrawingGridEvery",
        "doNotUseMarginsForDrawingGridOrigin", "drawingGridHorizontalOrigin", "drawingGridVerticalOrigin",
        "doNotShadeFormData", "noPunctuationKerning", "characterSpacingControl", "printTwoOnOne",
        "strictFirstAndLastChars", "noLineBreaksAfter", "noLineBreaksBefore", "savePreviewPicture",
        "doNotValidateAgainstSchema", "saveInvalidXml", "ignoreMixedContent", "alwaysShowPlaceholderText",
        "doNotDemarcateInvalidXml", "saveXmlDataOnly", "useXSLTWhenSaving", "saveThroughXslt", "showXMLTags",
        "alwaysMergeEmptyNamespace", "updateFields", "hdrShapeDefaults", "footnotePr", "endnotePr", "compat",
        "docVars", "rsids", "mathPr", "attachedSchema", "themeFontLang", "clrSchemeMapping",
        "doNotIncludeSubdocsInStats", "doNotAutoCompressPictures", "forceUpgrade", "captions",
        "readModeInkLockDown", "smartTagType", "schemaLibrary", "shapeDefaults", "doNotEmbedSmartTags",
        "decimalSymbol", "listSeparator"
    };
    size_t i;

    if (strncmp(qname, "w:", 2) != 0 && strncmp(qname, "m:", 2) != 0 && strncmp(qname, "sl:", 3) != 0 &&
            strncmp(qname, "o:", 2) != 0) {
        return -1;  /* w14:, w15:, ...: an extension */
    }

    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if (strcmp(mu_local(qname), order[i]) == 0) {
            return (int)i * 2 + 2;
        }
    }

    return -1;
}

/* settings Parade writes from what it models: these of the kept part give way to Parade's, or to their
   absence; the rest of the kept part stays, compatibility settings included */
static int settings_parade_owns(const char* qname) {
    static const char* own[] = { "mirrorMargins", "defaultTabStop", "autoHyphenation", "evenAndOddHeaders",
                                 "footnotePr", "endnotePr", "displayBackgroundShape"
                               };
    size_t i;

    for (i = 0; i < sizeof(own) / sizeof(own[0]); i++) {
        if (strncmp(qname, "w:", 2) == 0 && strcmp(qname + 2, own[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

/* Parade's settings part (gen) merged into the kept one: gen's elements, and the kept ones it does not own or
   have, in the schema's order; the result in out.  The kept root element (its namespaces) is kept.  0 when
   either cannot be read, out untouched */
static int settings_merge(const char* gen, size_t gn, const char* kept, size_t kn, int keep_rels, pd_buf* out) {
    dsel g[160], k[320];
    int ng, nk, i, j, rank, prev, *kr;
    size_t ga, gb, ka, kb;
    pd_buf o;

    ng = settings_children(gen, gn, g, 160, &ga, &gb);
    nk = settings_children(kept, kn, k, 320, &ka, &kb);

    if (ka == 0 || kb == 0 || ga == 0 || gb == 0 || (kr = (int*)calloc((size_t)nk + 1, sizeof(int))) == NULL) {
        return 0;
    }

    for (i = 0, prev = 0; i < nk; i++) {    /* an extension sits just after what it follows */
        rank = settings_rank(k[i].name);
        kr[i] = rank < 0 ? prev + 1 : rank;
        prev = rank < 0 ? prev : rank;
    }

    memset(&o, 0, sizeof(o));
    pb_put(&o, kept, ka);   /* the declaration and the kept root, with its namespaces */

    for (rank = 0; rank <= 2 * 100 + 2; rank++) {
        for (j = 0; j < ng; j++) {
            int r = settings_rank(g[j].name);

            if ((r < 0 ? 2 * 100 + 2 : r) == rank) {
                int dup = 0;

                if (strcmp(g[j].name, "w:compat") == 0) {   /* the document's own compatibility wins */
                    for (i = 0; i < nk && !dup; i++) {
                        dup = strcmp(k[i].name, "w:compat") == 0;
                    }
                }

                if (!dup) {
                    pb_put(&o, gen + g[j].a, g[j].b - g[j].a);
                }
            }
        }

        for (i = 0; i < nk; i++) {
            int theirs = 0;

            if (kr[i] != rank || settings_parade_owns(k[i].name)) {
                continue;
            }

            for (j = 0; j < ng && !theirs; j++) {
                theirs = strcmp(g[j].name, k[i].name) == 0 && strcmp(k[i].name, "w:compat") != 0;
            }

            if (!keep_rels) {   /* what names a relationship that is not kept (a template in the package) */
                pd_markup m;

                mu_init(&m, kept + k[i].a, k[i].b - k[i].a, 0);

                if (mu_next(&m) == MT_OPEN || m.type == MT_EMPTY) {
                    char v[8];

                    theirs |= mu_attr(&m, "r:id", v, sizeof(v));
                }
            }

            if (!theirs) {
                pb_put(&o, kept + k[i].a, k[i].b - k[i].a);
            }
        }
    }

    pb_put(&o, kept + kb, kn - kb);
    free(kr);

    if (o.err) {
        pb_free(&o);
        return 0;
    }

    out->n = 0;
    pb_put(out, o.p, o.n);
    pb_free(&o);
    return 1;
}

pd_status pd_docx_export(const pd_doc* d, pd_buf* out) {
    return pd_docx_export_as(d, out, 0);
}

/* a .docx, or (tmpl) a Word template, .dotx: the same package, its main part another type */
pd_status pd_docx_export_as(const pd_doc* d, pd_buf* out, int tmpl) {
    dxo* x = (dxo*)calloc(1, sizeof(dxo));
    pd_buf doc, part, cxml, cext, fonts, frels;
    dembed emb[64];                 /* the fonts the document carries, written embedded */
    int nemb = 0;
    int has_fonts, has_theme;
    pd_buf themeb = { 0 };
    const void* theme = NULL;
    size_t theme_len = 0;
    const void* kdata = NULL;     /* the parts kept as they came (KEEP_MIME) */
    size_t klen = 0;
    int has_keep = 0, keep_srels = 0;
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
    memset(&fonts, 0, sizeof(fonts));
    memset(&frels, 0, sizeof(frels));
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
    pb_printf(&doc, "<w:document %s>", W_NS);

    {   /* the pages' colour (Word's is the document's): the first section's */
        pd_section_props fsp;

        if (pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &fsp) == PD_OK && fsp.page_color) {
            pb_printf(&doc, "<w:background w:color=\"%06X\"/>", (unsigned)(fsp.page_color & 0xFFFFFF));
            x->page_color = fsp.page_color;
        }
    }

    pb_puts(&doc, "<w:body>");
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
    has_fonts = dx_font_table(d, &fonts, &frels, emb, &nemb);
    has_theme = dx_kept(d, THEME_MIME, &theme, &theme_len);

    {   /* the theme as the document has it now: the part it came with, its colours and fonts changed where they
           were; a document of Parade's own with a theme other than Office's gets a part made */
        pd_theme th, office;

        pd_doc_theme(d, &th);
        pd_theme_init(&office);

        if (has_theme) {
            dx_theme_patch(&themeb, (const char*)theme, theme_len, &th);
        } else if (memcmp(&th, &office, sizeof(th)) != 0) {
            dx_theme_new(&themeb, &th);
            has_theme = 1;
        }

        theme = themeb.p;
        theme_len = themeb.n;
    }

    /* the package */
    has_keep = dx_kept(d, KEEP_MIME, &kdata, &klen);
    z.o = out;
    pb_puts(&part, XML_DECL);
    pb_puts(&part, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Default Extension=\"png\" ContentType=\"image/png\"/>"
            "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
            "<Default Extension=\"gif\" ContentType=\"image/gif\"/>"
            "<Default Extension=\"bmp\" ContentType=\"image/bmp\"/>"
            "<Default Extension=\"tiff\" ContentType=\"image/tiff\"/>"
            "<Default Extension=\"webp\" ContentType=\"image/webp\"/>"
            "<Default Extension=\"emf\" ContentType=\"image/x-emf\"/>"
            "<Default Extension=\"wmf\" ContentType=\"image/x-wmf\"/>"
            "<Default Extension=\"odttf\" ContentType=\"application/vnd.openxmlformats-officedocument.obfuscatedFont\"/>"
            "<Default Extension=\"xlsx\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet\"/>"
            "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "wordprocessingml.");
    pb_puts(&part, tmpl ? "template.main+xml\"/>" : "document.main+xml\"/>");
    pb_puts(&part,
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

    for (i = 0; i < x->ncharts; i++) {
        pb_printf(&part, "<Override PartName=\"/word/charts/chart%d.xml\" ContentType=\"application/vnd."
                  "openxmlformats-officedocument.drawingml.chart+xml\"/>", (int)i + 1);
    }

    if (has_theme) {
        pb_puts(&part, "<Override PartName=\"/word/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-"
                "officedocument.theme+xml\"/>");
    }

    if (has_fonts) {
        pb_puts(&part, "<Override PartName=\"/word/fontTable.xml\" ContentType=\"application/vnd.openxmlformats-"
                "officedocument.wordprocessingml.fontTable+xml\"/>");
    }

    pb_puts(&part, "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package."
            "core-properties+xml\"/>");

    if (ncomments) {
        pb_puts(&part, "<Override PartName=\"/word/comments.xml\" ContentType=\"application/vnd.openxmlformats-"
                "officedocument.wordprocessingml.comments+xml\"/><Override PartName=\"/word/commentsExtended.xml\" "
                "ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.commentsExtended+xml\"/>");
    }

    if (has_keep) {     /* the parts kept as they came */
        size_t kp = 0;
        dkept e;

        while (keep_next((const unsigned char*)kdata, klen, &kp, &e)) {
            if (e.ctype[0] && e.src[0] != 's' && !strstr(e.name, ".rels")) {
                pb_puts(&part, "<Override PartName=\"/");
                xesc(&part, e.name, strlen(e.name));
                pb_puts(&part, "\" ContentType=\"");
                xesc(&part, e.ctype, strlen(e.ctype));
                pb_puts(&part, "\"/>");
            }
        }
    }

    pb_puts(&part, "</Types>");
    zip_add(&z, "[Content_Types].xml", part.p, part.n);
    part.n = 0;
    pb_puts(&part, XML_DECL);
    pb_puts(&part, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
            "officeDocument\" Target=\"word/document.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
            "core-properties\" Target=\"docProps/core.xml\"/>");
    dx_keep_rels(&part, kdata, has_keep ? klen : 0, "pkg");
    pb_puts(&part, "</Relationships>");
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

        if (x->page_color) {    /* the pages' colour shown */
            pb_puts(&part, "<w:displayBackgroundShape/>");
        }

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
    if (has_keep) {     /* merged into the settings the document came with */
        size_t kp = 0;
        dkept e, ks;
        int found = 0;

        memset(&ks, 0, sizeof(ks));

        while (keep_next((const unsigned char*)kdata, klen, &kp, &e)) {
            if (strcmp(e.src, "settings") == 0) {
                ks = e;
                found = 1;
            } else if (strcmp(e.src, "settings-rels") == 0) {
                keep_srels = 1;
            }
        }

        if (found) {
            pd_buf mg;

            memset(&mg, 0, sizeof(mg));

            if (settings_merge(part.p, part.n, (const char*)ks.data, ks.len, keep_srels, &mg)) {
                part.n = 0;
                pb_put(&part, mg.p, mg.n);
            } else {
                keep_srels = 0;
            }

            pb_free(&mg);
        } else {
            keep_srels = 0;
        }
    }

    zip_add(&z, "word/settings.xml", part.p, part.n);

    if (has_fonts) {
        zip_add(&z, "word/fontTable.xml", fonts.p, fonts.n);

        if (nemb > 0) {     /* the fonts it carries: obfuscated as Word does, and found through its relationships */
            part.n = 0;
            pb_puts(&part, XML_DECL);
            pb_puts(&part, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
            pb_put(&part, frels.p, frels.n);
            pb_puts(&part, "</Relationships>");
            zip_add(&z, "word/_rels/fontTable.xml.rels", part.p, part.n);

            for (i = 0; i < nemb; i++) {
                const char* mime;
                const void* data;
                size_t n;
                char name[48];
                int k;

                if (pd_doc_resource(d, emb[i].res, &mime, &data, &n) != PD_OK || n <= 32) {
                    continue;
                }

                part.n = 0;
                pb_put(&part, data, n);

                for (k = 0; k < 16 && !part.err; k++) {
                    part.p[k] ^= (char)emb[i].key[k];
                    part.p[k + 16] ^= (char)emb[i].key[k];
                }

                snprintf(name, sizeof(name), "word/fonts/font%d.odttf", (int)i + 1);
                zip_add(&z, name, part.p, part.n);
            }
        }
    }

    if (has_theme) {
        zip_add(&z, "word/theme/theme1.xml", theme, theme_len);
    }

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

    if (has_theme) {
        pb_puts(&part, "<Relationship Id=\"rIdTh\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                "relationships/theme\" Target=\"theme/theme1.xml\"/>");
    }

    if (has_fonts) {
        pb_puts(&part, "<Relationship Id=\"rIdFt\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/"
                "relationships/fontTable\" Target=\"fontTable.xml\"/>");
    }

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
    dx_keep_rels(&part, kdata, has_keep ? klen : 0, "doc");
    pb_puts(&part, "</Relationships>");
    zip_add(&z, "word/_rels/document.xml.rels", part.p, part.n);

    if (has_keep) {
        size_t kp = 0;
        dkept e;

        while (keep_next((const unsigned char*)kdata, klen, &kp, &e)) {
            if (strcmp(e.src, "settings") != 0 && (strcmp(e.src, "settings-rels") != 0 || keep_srels)) {
                zip_add(&z, e.name, e.data, e.len);
            }
        }
    }

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

    for (i = 0; i < x->ncharts; i++) {  /* the charts' parts, and the workbooks they come from */
        const char* mime;
        const void* data;
        size_t len;
        char name[64];

        if (pd_doc_resource(d, x->charts[i].chart, &mime, &data, &len) != PD_OK) {
            continue;
        }

        snprintf(name, sizeof(name), "word/charts/chart%d.xml", (int)i + 1);
        zip_add(&z, name, data, len);

        if (x->charts[i].rid[0] && (x->charts[i].data || x->charts[i].link[0])) {
            part.n = 0;
            pb_puts(&part, XML_DECL);
            pb_puts(&part, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                    "<Relationship Id=\"");
            xesc(&part, x->charts[i].rid, strlen(x->charts[i].rid));

            if (x->charts[i].data) {
                pb_printf(&part, "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/package\" "
                          "Target=\"../embeddings/Microsoft_Excel_Worksheet%d.xlsx\"/>", (int)i + 1);
            } else {
                pb_puts(&part, "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/oleObject\" "
                        "Target=\"");
                xesc(&part, x->charts[i].link, strlen(x->charts[i].link));
                pb_puts(&part, "\" TargetMode=\"External\"/>");
            }

            pb_puts(&part, "</Relationships>");
            snprintf(name, sizeof(name), "word/charts/_rels/chart%d.xml.rels", (int)i + 1);
            zip_add(&z, name, part.p, part.n);

            if (x->charts[i].data && pd_doc_resource(d, x->charts[i].data, &mime, &data, &len) == PD_OK) {
                snprintf(name, sizeof(name), "word/embeddings/Microsoft_Excel_Worksheet%d.xlsx", (int)i + 1);
                zip_add(&z, name, data, len);
            }
        }
    }

    zip_finish(&z);
    i = doc.err || part.err || x->rels.err || x->notes.err || x->endnotes.err || cxml.err || cext.err;
    pb_free(&cxml);
    pb_free(&fonts);
    pb_free(&frels);
    pb_free(&themeb);
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

/* what a w:pPr and a w:rPr say, as masked overrides */
typedef struct {
    pd_para_props pp;
    pd_char_props cp;
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
    pd_sp w;                    /* the widest */
    pd_sp ew[6];                /* each edge's, by bit */
    uint32_t c;
    uint32_t ref;               /* c's theme colour, 0 none */
} dedges;

/* one part of a table style: the whole table, or a condition of it */
typedef struct {
    int given;
    dprops pr;                  /* its w:pPr and w:rPr, for the text of the cells */
    uint32_t shd;               /* cell shading, with has_shd */
    uint32_t shd_ref;           /* its theme colour, 0 none */
    int has_shd;
    dedges cb;                  /* w:tcBorders */
} dtpart;

enum { TP_WHOLE, TP_FIRST_ROW, TP_LAST_ROW, TP_FIRST_COL, TP_LAST_COL, TP_BAND1V, TP_BAND2V, TP_BAND1H, TP_BAND2H,
       TP_NE, TP_NW, TP_SE, TP_SW, TP_N
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
    dnum* nums;                 /* numbering.xml's w:num, grown as read */
    int nnums, cnums;
    dabs* abss;                 /* and its w:abstractNum */
    int nabs, cabs;
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
    int32_t note_fmt[2], note_start[2], note_restart[2];    /* settings.xml: footnotePr, endnotePr */
    char hf_rid[16][64];        /* header/footer parts already read, and their stories */
    pd_block_id hf_story[16];
    int nhf;
    pd_block_id hf_last[6];     /* the previous section's: header default/first/even, footer the same */
    char def_pstyle[64];        /* the paragraph style of a paragraph that names none */
    char theme_major[64], theme_minor[64];  /* the theme's heading and body fonts */
    char theme_major_ea[64], theme_minor_ea[64], theme_major_cs[64], theme_minor_cs[64];  /* other scripts' */
    char theme_name[64];
    char builtin[16][24];       /* Parade's own styles made what the document's Word styles of them say */
    int nbuiltin;
    uint32_t theme_clr[12];     /* the theme's colours: dk1 lt1 dk2 lt2 accent1..6 hlink folHlink */
    pd_sp margin_left;          /* the section's, for pictures placed from the page's edge */
    pd_sp page_w, page_h, margin_right, margin_top, margin_bottom;  /* ... and for VML shapes placed in it */
    uint32_t page_color;        /* w:background: every page's colour */
    struct {                    /* bookmarks read: where they are */
        char name[32];
        pd_block_id para;
        int wid;                /* its w:id, which its bookmarkEnd names */
        pd_marker_id m0, m1;    /* where its text starts and ends (REF fields show it) */
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
        int pending;            /* 1 start, 2 end: marked before a paragraph began, so at its start */
    }* cm;
    int ncm, capcm;
    pd_rev_id float_rev;        /* the tracked change a float being read sits in: its content's */
    const pj_node* rebuild;     /* a drawing made again from its edited XML: its description as it was (pictures,
                                   styles, text boxes, fallback), in place of the package it was read from */
    int rebuild_story;          /* the next of its text boxes' stories */
    const int* rebuild_map;     /* the k-th text box's story, an index into the old ones (-1: a new one); NULL: in order */
    int rebuild_nmap;
} dxi;

/* ------------------------------------------------------------------ */
/* theme colours                                                      */
/* ------------------------------------------------------------------ */

enum { TC_DK1, TC_LT1, TC_DK2, TC_LT2, TC_ACCENT1, TC_HLINK = 10, TC_FOLHLINK, TC_N };

/* Office's default theme, for a document without one */
static void theme_defaults(uint32_t* c) {
    static const uint32_t d[TC_N] = { 0x000000, 0xFFFFFF, 0x44546A, 0xE7E6E6, 0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000,
                                      0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72
                                    };
    int i;

    for (i = 0; i < TC_N; i++) {
        c[i] = 0xFF000000u | d[i];
    }
}

/* a theme colour's slot by its name: DrawingML's (dk1, tx1, accent1, hlink) or WordprocessingML's (dark1, text1,
   accent1, hyperlink); text and background as Word maps them (text1 dark1, background1 light1); -1 if none */
int pd_conv_theme_slot(const char* n) {
    static const struct {
        const char* n;
        int slot;
    } t[] = { { "dk1", TC_DK1 }, { "lt1", TC_LT1 }, { "dk2", TC_DK2 }, { "lt2", TC_LT2 }, { "tx1", TC_DK1 },
        { "bg1", TC_LT1 }, { "tx2", TC_DK2 }, { "bg2", TC_LT2 }, { "dark1", TC_DK1 }, { "light1", TC_LT1 },
        { "dark2", TC_DK2 }, { "light2", TC_LT2 }, { "text1", TC_DK1 }, { "background1", TC_LT1 },
        { "text2", TC_DK2 }, { "background2", TC_LT2 }, { "hlink", TC_HLINK }, { "hyperlink", TC_HLINK },
        { "folHlink", TC_FOLHLINK }, { "followedHyperlink", TC_FOLHLINK }
    };
    size_t i;

    if (strncmp(n, "accent", 6) == 0 && n[6] >= '1' && n[6] <= '6' && !n[7]) {
        return TC_ACCENT1 + n[6] - '1';
    }

    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (strcmp(n, t[i].n) == 0) {
            return t[i].slot;
        }
    }

    return -1;
}

static void rgb_hsl(uint32_t c, double* h, double* s, double* l) {
    double r = ((c >> 16) & 255) / 255.0, g = ((c >> 8) & 255) / 255.0, b = (c & 255) / 255.0;
    double mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    double d = mx - mn;

    *l = (mx + mn) / 2;
    *h = *s = 0;

    if (d > 1e-9) {
        *s = *l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
        *h = mx == r ? (g - b) / d + (g < b ? 6 : 0) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        *h /= 6;
    }
}

static double hue_part(double p, double q, double t) {
    t = t < 0 ? t + 1 : t > 1 ? t - 1 : t;
    return t < 1.0 / 6 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2.0 / 3 ? p + (q - p) * (2.0 / 3 - t) * 6 : p;
}

static uint32_t hsl_rgb(uint32_t alpha, double h, double s, double l) {
    double r = l, g = l, b = l, q, p;

    l = l < 0 ? 0 : l > 1 ? 1 : l;
    r = g = b = l;

    if (s > 1e-9) {
        q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        p = 2 * l - q;
        r = hue_part(p, q, h + 1.0 / 3);
        g = hue_part(p, q, h);
        b = hue_part(p, q, h - 1.0 / 3);
    }

    return alpha | (uint32_t)(r * 255 + 0.5) << 16 | (uint32_t)(g * 255 + 0.5) << 8 | (uint32_t)(b * 255 + 0.5);
}

/* a colour's luminance scaled by mul and moved by add (DrawingML's lumMod and lumOff; Word's tints and shades) */
static uint32_t lum_adjust(uint32_t c, double mul, double add) {
    double h, s, l;

    rgb_hsl(c, &h, &s, &l);
    return hsl_rgb(c & 0xFF000000u, h, s, l * mul + add);
}

/* a w: colour: the theme colour named by themeattr when there is one (tinted toward white, or shaded toward
   black, by the hex 00-FF in tintattr and shadeattr), else the hex in valattr; 0 when neither, or auto */
static int wcolor_ref(const dxi* X, const pd_markup* m, const char* valattr, const char* themeattr,
                      const char* tintattr, const char* shadeattr, uint32_t* out, uint32_t* ref) {
    char v[64];
    int slot = -1;

    if (ref) {
        *ref = 0;
    }

    if (themeattr && mu_attr(m, themeattr, v, sizeof(v))) {
        slot = pd_conv_theme_slot(v);
    }

    if (slot >= 0) {
        double mul = 1, add = 0;

        if (tintattr && mu_attr(m, tintattr, v, sizeof(v))) {
            mul = (double)strtoul(v, NULL, 16) / 255.0;
            add = 1 - mul;
        }

        if (shadeattr && mu_attr(m, shadeattr, v, sizeof(v))) {
            double sh = (double)strtoul(v, NULL, 16) / 255.0;

            mul *= sh;
            add *= sh;
        }

        *out = mul == 1 && add == 0 ? X->theme_clr[slot] : lum_adjust(X->theme_clr[slot], mul, add);

        if (ref) {  /* linked to the theme: what it comes to follows the document's */
            *ref = pd_theme_color(slot, (int32_t)(mul * 100000 + 0.5), (int32_t)(add * 100000 + 0.5));
        }

        return 1;
    }

    if (mu_attr(m, valattr, v, sizeof(v)) && strcmp(v, "auto") != 0) {
        *out = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
        return 1;
    }

    return 0;
}

static int wcolor(const dxi* X, const pd_markup* m, const char* valattr, const char* themeattr, const char* tintattr,
                  const char* shadeattr, uint32_t* out) {
    return wcolor_ref(X, m, valattr, themeattr, tintattr, shadeattr, out, NULL);
}

/* a w:shd as the colour it shows: the pattern (w:val: clear, solid, pctN, stripes...) of its foreground
   (w:color; auto is black) over its fill (w:fill; auto is none, white under a pattern); 0 when it shows nothing */
static int shd_color_ref(const dxi* X, const pd_markup* m, uint32_t* out, uint32_t* ref) {
    char v[32] = "clear";
    uint32_t fill = 0, fg = 0xFF000000u, base, fref = 0;
    int has_fill = wcolor_ref(X, m, "w:fill", "w:themeFill", "w:themeFillTint", "w:themeFillShade", &fill, &fref);
    double f = 0;
    uint32_t r, g, b;

    wcolor(X, m, "w:color", "w:themeColor", "w:themeTint", "w:themeShade", &fg);
    mu_attr(m, "w:val", v, sizeof(v));

    if (!strcmp(v, "solid")) {
        f = 1;
    } else if (!strncmp(v, "pct", 3)) {
        f = atoi(v + 3) / 100.0;
    } else if (strcmp(v, "clear") != 0 && strcmp(v, "nil") != 0 && strcmp(v, "none") != 0) {
        f = !strncmp(v, "thin", 4) ? 0.25 : 0.5;   /* stripes, crosses: what they come to, seen from afar */
    }

    f = f < 0 ? 0 : f > 1 ? 1 : f;

    if (ref) {  /* a plain fill keeps its link to the theme; a pattern is a colour of its own */
        *ref = f == 0 && has_fill ? fref : 0;
    }

    if (f == 0) {
        *out = has_fill ? fill : 0;
        return has_fill;
    }

    base = has_fill ? fill : 0xFFFFFFFFu;
    r = (uint32_t)(((base >> 16) & 255) * (1 - f) + ((fg >> 16) & 255) * f + 0.5);
    g = (uint32_t)(((base >> 8) & 255) * (1 - f) + ((fg >> 8) & 255) * f + 0.5);
    b = (uint32_t)((base & 255) * (1 - f) + (fg & 255) * f + 0.5);
    *out = 0xFF000000u | r << 16 | g << 8 | b;
    return 1;
}

static int shd_color(const dxi* X, const pd_markup* m, uint32_t* out) {
    return shd_color_ref(X, m, out, NULL);
}

/* a DrawingML colour modifier (a child of srgbClr, schemeClr, sysClr, prstClr) applied to c */
uint32_t pd_conv_clr_modify(uint32_t c, const char* t, const pd_markup* m) {
    char v[32];
    double f;

    if (!mu_attr(m, "val", v, sizeof(v))) {
        return c;
    }

    f = atof(v) / 100000.0;

    if (!strcmp(t, "lumMod")) {
        return lum_adjust(c, f, 0);
    } else if (!strcmp(t, "lumOff")) {
        return lum_adjust(c, 1, f);
    } else if (!strcmp(t, "alpha")) {   /* how opaque: the colour's alpha byte (1 at the least: 0 says "opaque") */
        int a = (int)(f * 255 + 0.5);

        a = a < 1 ? 1 : a > 255 ? 255 : a;
        return (c & 0xFFFFFFu) | (uint32_t)a << 24;
    } else if (!strcmp(t, "tint") || !strcmp(t, "shade")) {    /* toward white, toward black */
        uint32_t r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;

        if (t[0] == 't') {
            r = (uint32_t)(255 - (255 - r) * f + 0.5);
            g = (uint32_t)(255 - (255 - g) * f + 0.5);
            b = (uint32_t)(255 - (255 - b) * f + 0.5);
        } else {
            r = (uint32_t)(r * f + 0.5);
            g = (uint32_t)(g * f + 0.5);
            b = (uint32_t)(b * f + 0.5);
        }

        return (c & 0xFF000000u) | r << 16 | g << 8 | b;
    }

    return c;
}

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
    uint32_t c, ref;

    if (strcmp(t, "b") == 0) {
        cp->mask |= PD_CP_WEIGHT;
        cp->weight = attr_on(m) ? 700 : 400;
    } else if (strcmp(t, "i") == 0) {
        cp->mask |= PD_CP_ITALIC;
        cp->italic = attr_on(m);
    } else if (strcmp(t, "bCs") == 0) {
        /* the complex scripts' own, when it is not what the rPr says for the text (b with bCs: both bold) */
        if (!(cp->mask & PD_CP_WEIGHT) || cp->weight != (attr_on(m) ? 700 : 400)) {
            cp->mask |= PD_CP_WEIGHT_CS;
            cp->weight_cs = attr_on(m) ? 700 : 400;
        }
    } else if (strcmp(t, "iCs") == 0) {
        if (!(cp->mask & PD_CP_ITALIC) || cp->italic != attr_on(m)) {
            cp->mask |= PD_CP_ITALIC_CS;
            cp->italic_cs = attr_on(m);
        }
    } else if (strcmp(t, "szCs") == 0) {
        int hp = attr_int(m, "w:val", 0);
        pd_sp z = (pd_sp)((int64_t)hp * 65536 / 2);

        if (hp > 0 && (!(cp->mask & PD_CP_SIZE) || cp->size != z)) {
            cp->mask |= PD_CP_SIZE_CS;
            cp->size_cs = z;
        }
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
    } else if (strcmp(t, "color") == 0 && wcolor_ref(X, m, "w:val", "w:themeColor", "w:themeTint", "w:themeShade", &c,
               &ref)) {
        cp->mask |= PD_CP_COLOR;
        cp->color = c;
        cp->color_theme = ref;
    } else if (strcmp(t, "shd") == 0 && shd_color_ref(X, m, &c, &ref)) {
        cp->mask |= PD_CP_BACKGROUND;
        cp->background = c;
        cp->background_theme = ref;
    } else if (strcmp(t, "highlight") == 0 && mu_attr(m, "w:val", v, sizeof(v))) {
        cp->mask |= PD_CP_BACKGROUND;
        cp->background_theme = 0;
        cp->background = strcmp(v, "none") == 0 ? 0 : 0xFFFFFF00u;

        for (c = 0; c < 16; c++) {
            if (strcmp(v, dx_highlights[c].name) == 0) {
                cp->background = 0xFF000000u | dx_highlights[c].rgb;
            }
        }
    } else if (strcmp(t, "rFonts") == 0) {
        /* a named font, else the theme's: minorHAnsi is the body font, majorHAnsi the headings' */
        /* Word takes the theme's font over a name given beside it (the name is what it was when saved) */
        if ((mu_attr(m, "w:asciiTheme", v, sizeof(v)) || mu_attr(m, "w:hAnsiTheme", v, sizeof(v))) &&
                (strncmp(v, "major", 5) == 0 ? X->theme_major : X->theme_minor)[0]) {
            cp->mask |= PD_CP_FAMILY;
            snprintf(cp->family, sizeof(cp->family), "%s", strncmp(v, "major", 5) == 0 ? X->theme_major :
                     X->theme_minor);
            cp->font_theme = (cp->font_theme & ~3) | (strncmp(v, "major", 5) == 0 ? PD_FONT_THEME_MAJOR :
                             PD_FONT_THEME_MINOR);
        } else if (mu_attr(m, "w:ascii", v, sizeof(v)) || mu_attr(m, "w:hAnsi", v, sizeof(v))) {
            cp->mask |= PD_CP_FAMILY;
            snprintf(cp->family, sizeof(cp->family), "%.63s", v);
            cp->font_theme &= ~3;
        }

        /* the East Asian and complex-script slots: the theme's for those scripts, else named */
        {
            static const char* const named[2] = { "w:eastAsia", "w:cs" }, *const themed[2] = { "w:eastAsiaTheme",
                                                                                             "w:cstheme"
                                                                                           };
            int k;

            for (k = 0; k < 2; k++) {
                int th = 0, sh = k ? 4 : 2;

                v[0] = '\0';

                if (mu_attr(m, themed[k], v, sizeof(v))) {
                    th = strncmp(v, "major", 5) == 0 ? PD_FONT_THEME_MAJOR : PD_FONT_THEME_MINOR;
                    snprintf(v, sizeof(v), "%s", k ? (th == PD_FONT_THEME_MAJOR ? X->theme_major_cs : X->theme_minor_cs) :
                             (th == PD_FONT_THEME_MAJOR ? X->theme_major_ea : X->theme_minor_ea));
                    th = v[0] ? th : 0;
                }

                if (!v[0] && !mu_attr(m, named[k], v, sizeof(v))) {
                    v[0] = '\0';
                }

                if (v[0] && !((cp->mask & PD_CP_FAMILY) && !strcmp(v, cp->family))) {   /* the same face: nothing apart */
                    cp->mask |= k ? PD_CP_FAMILY_CS : PD_CP_FAMILY_EA;
                    snprintf(k ? cp->family_cs : cp->family_ea, 64, "%.63s", v);
                    cp->font_theme = (cp->font_theme & ~(3 << sh)) | th << sh;
                }
            }
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
    } else if (strcmp(t, "w") == 0) {               /* widened or narrowed, in per cent */
        int pc = attr_int(m, "w:val", 100);

        cp->mask |= PD_CP_HSCALE;
        cp->hscale = pc < 1 ? 1000 : pc > 600 ? 6000 : pc * 10;
    } else if (strcmp(t, "position") == 0) {        /* raised or lowered, in half-points */
        cp->mask |= PD_CP_POSITION;
        cp->position = (pd_sp)((int64_t)attr_int(m, "w:val", 0) * 65536 / 2);
    } else if (strcmp(t, "kern") == 0) {            /* kerned from a size up: Word's own default is not to */
        cp->mask |= PD_CP_KERNING;
        cp->kerning = attr_int(m, "w:val", 0) > 0;
    } else if (strcmp(t, "lang") == 0 && mu_attr(m, "w:val", v, sizeof(v))) {
        cp->mask |= PD_CP_LANG;
        snprintf(cp->lang, sizeof(cp->lang), "%.15s", v);
    } else if (strcmp(t, "rStyle") == 0 && rstyle) {
        mu_attr(m, "w:val", rstyle, rcap);
    }
}

/* Word's border styles as Parade's line styles: the art ones (apples, ...) and others as single */
static int32_t line_style_of(const char* v) {
    static const struct {
        const char* name;
        int32_t style;
    } map[] = {
        { "nil", PD_LINE_NONE }, { "none", PD_LINE_NONE }, { "double", PD_LINE_DOUBLE }, { "dotted", PD_LINE_DOTTED },
        { "dashed", PD_LINE_DASHED }, { "dashSmallGap", PD_LINE_DASHED }, { "thick", PD_LINE_THICK },
        { "dotDash", PD_LINE_DOT_DASH }, { "dotDotDash", PD_LINE_DOT_DASH }, { "triple", PD_LINE_TRIPLE },
        { "thinThickSmallGap", PD_LINE_THIN_THICK }, { "thinThickMediumGap", PD_LINE_THIN_THICK },
        { "thinThickLargeGap", PD_LINE_THIN_THICK }, { "thickThinSmallGap", PD_LINE_THICK_THIN },
        { "thickThinMediumGap", PD_LINE_THICK_THIN }, { "thickThinLargeGap", PD_LINE_THICK_THIN },
        { "thinThickThinSmallGap", PD_LINE_TRIPLE }, { "thinThickThinMediumGap", PD_LINE_TRIPLE },
        { "thinThickThinLargeGap", PD_LINE_TRIPLE }, { "doubleWave", PD_LINE_DOUBLE },
        { "threeDEmboss", PD_LINE_THICK }, { "threeDEngrave", PD_LINE_THICK }
    };
    size_t k;

    for (k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
        if (!strcmp(v, map[k].name)) {
            return map[k].style;
        }
    }

    return PD_LINE_SINGLE;
}

/* one side of a w:pBdr or w:pgBorders (space in points; on a page from its edge or the text) as a border line */
static void border_line(const dxi* X, const pd_markup* m, pd_border_line* l, int page) {
    char v[40];
    uint32_t ref = 0;

    memset(l, 0, sizeof(*l));

    if (!mu_attr(m, "w:val", v, sizeof(v)) || (l->style = line_style_of(v)) == PD_LINE_NONE) {
        return;
    }

    l->width = (pd_sp)((int64_t)attr_int(m, "w:sz", 4) * 65536 / 8);
    l->width = l->width > 0 ? l->width > PD_PT(100) ? PD_PT(100) : l->width : PD_PT(0.25);
    l->space = (pd_sp)attr_int(m, "w:space", 0) * 65536;
    l->space = l->space < 0 ? 0 : l->space > PD_PT(page ? 1000 : 31) ? PD_PT(page ? 1000 : 31) : l->space;
    l->color = 0xFF000000u;

    if (mu_attr(m, "w:color", v, sizeof(v)) && strcmp(v, "auto") != 0) {
        wcolor_ref(X, m, "w:color", "w:themeColor", "w:themeTint", "w:themeShade", &l->color, &ref);
        l->color = l->color ? l->color : 0xFF000000u;
    }
}

/* one child of a w:pPr into pr */
static void ppr_elem(const dxi* X, const pd_markup* m, const char* t, dprops* pr) {
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
                pp->line_rule = PD_LINE_MULTIPLE;
                pp->line_height = 0;
            } else {                            /* exact or at least, in twips */
                pp->line_spacing = 1000;
                pp->line_rule = strcmp(rule, "exact") == 0 ? PD_LINE_EXACT : PD_LINE_AT_LEAST;
                pp->line_height = twips(line);
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
    } else if (strcmp(t, "snapToGrid") == 0) {
        pp->mask |= PD_PP_SNAP_GRID;
        pp->snap_grid = attr_on(m);
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
        memset(pp->border_line, 0, sizeof(pp->border_line));
    } else if (strcmp(t, "bar") == 0) {     /* a line in the left margin */
        pp->mask |= PD_PP_BORDER;
        border_line(X, m, &pp->border_line[PD_BORDER_LINE_BAR], 0);
    } else if (strcmp(t, "top") == 0 || strcmp(t, "bottom") == 0 || strcmp(t, "left") == 0 ||
               strcmp(t, "right") == 0 || strcmp(t, "start") == 0 || strcmp(t, "end") == 0 ||
               strcmp(t, "between") == 0) {
        /* an edge of w:pBdr (no other w:pPr child has these names) */
        int side = t[0] == 't' ? PD_BORDER_TOP : t[0] == 'b' && t[1] == 'o' ? PD_BORDER_BOTTOM : t[0] == 'b' ?
                   PD_BORDER_BETWEEN : t[0] == 'r' || t[0] == 'e' ? PD_BORDER_RIGHT : PD_BORDER_LEFT;
        int slot = side == PD_BORDER_TOP ? 0 : side == PD_BORDER_RIGHT ? 1 : side == PD_BORDER_BOTTOM ? 2 :
                   side == PD_BORDER_LEFT ? 3 : PD_BORDER_LINE_BETWEEN;

        border_line(X, m, &pp->border_line[slot], 0);     /* each edge its own; the summary below */

        if (mu_attr(m, "w:val", v, sizeof(v)) && strcmp(v, "none") != 0 && strcmp(v, "nil") != 0) {
            pd_sp bw = (pd_sp)((int64_t)attr_int(m, "w:sz", 4) * 65536 / 8);

            pp->mask |= PD_PP_BORDER;
            pp->border_sides |= side;
            pp->border_width = bw > pp->border_width ? bw : pp->border_width;
            pp->border_width = pp->border_width > 0 ? pp->border_width : PD_PT(0.25);

            if (!pp->border_color || side != PD_BORDER_BETWEEN) {
                if (!wcolor_ref(X, m, "w:color", "w:themeColor", "w:themeTint", "w:themeShade", &pp->border_color,
                                &pp->border_theme)) {
                    pp->border_color = 0xFF000000u;
                }
            }

            if (side != PD_BORDER_BETWEEN && attr_int(m, "w:space", 0) > 0) {   /* in points */
                pd_sp sp = PD_PT(attr_int(m, "w:space", 0));

                pp->border_space = sp > pp->border_space ? sp : pp->border_space;
            }
        }
    } else if (strcmp(t, "shd") == 0) {
        pp->mask |= PD_PP_SHADING;
        if (!shd_color_ref(X, m, &pp->shading, &pp->shading_theme)) {
            pp->shading = 0;
        }
    } else if (strcmp(t, "suppressAutoHyphens") == 0) {
        pp->mask |= PD_PP_HYPHENATE;
        pp->hyphenate = !attr_on(m);
    } else if (strcmp(t, "tab") == 0 && pp->ntabs < PD_MAX_TABS && mu_attr(m, "w:val", v, sizeof(v))) {
        /* a stop, or the clearing of one the style set (align -1, merged away by pr_over) */
        pd_tab_stop* ts = &pp->tabs[pp->ntabs];
        char ld[32] = "none";

        if (strcmp(v, "num") == 0) {
            return;     /* a list's own stop: not one text goes to */
        }

        pp->mask |= PD_PP_TABS;
        ts->position = twips(attr_int(m, "w:pos", 0));
        ts->align = strcmp(v, "clear") == 0 ? -1 : strcmp(v, "center") == 0 ? PD_TAB_CENTER : strcmp(v, "right") == 0 ||
                    strcmp(v, "end") == 0 ? PD_TAB_RIGHT : strcmp(v, "decimal") == 0 ? PD_TAB_DECIMAL :
                    strcmp(v, "bar") == 0 ? PD_TAB_BAR : PD_TAB_LEFT;
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
    if (sp->mask & PD_PP_SNAP_GRID) dp->snap_grid = sp->snap_grid;
    if (sp->mask & PD_PP_DIRECTION) dp->direction = sp->direction;
    if (sp->mask & PD_PP_SHADING) dp->shading = sp->shading;
    if (sp->mask & PD_PP_SHADING) dp->shading_theme = sp->shading_theme;

    if (sp->mask & PD_PP_BORDER) {
        dp->border_theme = sp->border_theme;
        dp->border_color = sp->border_color;
        dp->border_width = sp->border_width;
        dp->border_sides = sp->border_sides;
        dp->border_space = sp->border_space;
        memcpy(dp->border_line, sp->border_line, sizeof(dp->border_line));
    }

    if (sp->mask & PD_PP_LINE_SPACING) {
        dp->line_spacing = sp->line_spacing;
        dp->line_rule = sp->line_rule;
        dp->line_height = sp->line_height;
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
    if (sc->mask & PD_CP_FAMILY) dc->font_theme = (dc->font_theme & ~3) | (sc->font_theme & 3);
    if (sc->mask & PD_CP_FAMILY_EA) dc->font_theme = (dc->font_theme & ~(3 << 2)) | (sc->font_theme & (3 << 2));
    if (sc->mask & PD_CP_FAMILY_CS) dc->font_theme = (dc->font_theme & ~(3 << 4)) | (sc->font_theme & (3 << 4));
    if (sc->mask & PD_CP_COLOR) dc->color_theme = sc->color_theme;
    if (sc->mask & PD_CP_BACKGROUND) dc->background_theme = sc->background_theme;
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
    if (sc->mask & PD_CP_HSCALE) dc->hscale = sc->hscale;
    if (sc->mask & PD_CP_POSITION) dc->position = sc->position;
    if (sc->mask & PD_CP_KERNING) dc->kerning = sc->kerning;
    if (sc->mask & PD_CP_LANG) memcpy(dc->lang, sc->lang, sizeof(dc->lang));
    if (sc->mask & PD_CP_FAMILY_EA) memcpy(dc->family_ea, sc->family_ea, sizeof(dc->family_ea));
    if (sc->mask & PD_CP_FAMILY_CS) memcpy(dc->family_cs, sc->family_cs, sizeof(dc->family_cs));
    if (sc->mask & PD_CP_SIZE_CS) dc->size_cs = sc->size_cs;
    if (sc->mask & PD_CP_WEIGHT_CS) dc->weight_cs = sc->weight_cs;
    if (sc->mask & PD_CP_ITALIC_CS) dc->italic_cs = sc->italic_cs;

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
            ppr_elem(X, &m, t, tgt);

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
static void edge_elem(const dxi* X, const pd_markup* m, const char* t, dedges* e) {
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

        int k;

        e->on |= bit;
        e->w = bw > e->w ? bw : e->w;
        e->w = e->w > 0 ? e->w : PD_PT(0.25);   /* w:sz 0: the thinnest, 2/8 pt */

        for (k = 0; k < 6 && !(bit & (1 << k)); k++) {
        }

        if (k < 6) {
            e->ew[k] = bw > 0 ? bw : PD_PT(0.25);
        }

        wcolor_ref(X, m, "w:color", "w:themeColor", "w:themeTint", "w:themeShade", &e->c, &e->ref);
    } else {
        e->on &= ~bit;
    }
}

/* b's edges over a's: those b says replace a's */
static void edges_over(dedges* a, const dedges* b) {
    int k;

    a->on = (a->on & ~b->set) | (b->on & b->set);
    a->set |= b->set;

    for (k = 0; k < 6; k++) {
        if ((b->set & (1 << k)) && b->ew[k]) {
            a->ew[k] = b->ew[k];
        }
    }

    if (b->on) {
        a->w = b->w;
        a->ref = b->c ? b->ref : a->ref;
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
        a->shd_ref = b->shd_ref;
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
                                                   "band1Vert", "band2Vert", "band1Horz", "band2Horz", "neCell",
                                                   "nwCell", "seCell", "swCell"
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
            ppr_elem(X, &m, t, &tp->pr);
        } else if (in_rpr && !in_ppr) {
            rpr_elem(X, &m, t, &tp->pr.cp, NULL, 0);
        } else if (in_tb && part == TP_WHOLE) {
            edge_elem(X, &m, t, &ts->tb);
        } else if (in_cb) {
            edge_elem(X, &m, t, &tp->cb);
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
            if (!shd_color_ref(X, &m, &tp->shd, &tp->shd_ref)) {
                tp->shd = 0;
            }
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

/* the theme's colour scheme, and its font scheme: the Latin face of the major (headings) and minor (body) fonts */
static void read_theme(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    char* which = NULL;
    int slot = -1, in_scheme = 0;
    char v[32];

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);

        if (strcmp(t, "theme") == 0 && m.type == MT_OPEN) {
            mu_attr(&m, "name", X->theme_name, sizeof(X->theme_name));
        } else if (strcmp(t, "clrScheme") == 0) {
            in_scheme = m.type == MT_OPEN;
        } else if (in_scheme && m.type == MT_OPEN && pd_conv_theme_slot(t) >= 0 && (t[0] == 'd' || t[0] == 'l' ||
                   t[0] == 'a' || t[0] == 'h' || t[0] == 'f')) {
            slot = pd_conv_theme_slot(t);   /* dk1 ... folHlink, each holding one colour */
        } else if (in_scheme && m.type == MT_CLOSE && pd_conv_theme_slot(t) >= 0) {
            slot = -1;
        } else if (slot >= 0 && strcmp(t, "srgbClr") == 0 && mu_attr(&m, "val", v, sizeof(v))) {
            X->theme_clr[slot] = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
        } else if (slot >= 0 && strcmp(t, "sysClr") == 0 && mu_attr(&m, "lastClr", v, sizeof(v))) {
            X->theme_clr[slot] = 0xFF000000u | (uint32_t)strtoul(v, NULL, 16);
        } else if (m.type == MT_OPEN && strcmp(t, "majorFont") == 0) {
            which = X->theme_major;
        } else if (m.type == MT_OPEN && strcmp(t, "minorFont") == 0) {
            which = X->theme_minor;
        } else if (m.type == MT_CLOSE && (strcmp(t, "majorFont") == 0 || strcmp(t, "minorFont") == 0)) {
            which = NULL;
        } else if (which && !which[0] && (m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(t, "latin") == 0) {
            mu_attr(&m, "typeface", which, 64);
        } else if (which && (m.type == MT_OPEN || m.type == MT_EMPTY) && (strcmp(t, "ea") == 0 || strcmp(t, "cs") == 0)) {
            int major = which == X->theme_major;

            mu_attr(&m, "typeface", t[0] == 'e' ? (major ? X->theme_major_ea : X->theme_minor_ea) :
                    (major ? X->theme_major_cs : X->theme_minor_cs), 64);
        }
    }
}

static void dx_theme_from(const dxi* X, pd_theme* th);

/* a theme part's colours and fonts */
static int dx_theme_of_xml(const char* xml, size_t n, pd_theme* out) {
    dxi* X = (dxi*)calloc(1, sizeof(dxi));

    if (!X) {
        return 0;
    }

    theme_defaults(X->theme_clr);
    read_theme(X, xml, n);
    dx_theme_from(X, out);
    free(X);
    return 1;
}

/* the theme read made the document's, for what is linked to it */
static void dx_theme_set(const dxi* X, pd_doc* d) {
    pd_theme th;

    dx_theme_from(X, &th);
    pd_doc_set_theme(d, &th);
}

static void dx_theme_from(const dxi* X, pd_theme* out) {
    pd_theme th;
    int i;

    pd_theme_init(&th);

    if (X->theme_name[0]) {
        snprintf(th.name, sizeof(th.name), "%s", X->theme_name);
    }

    for (i = 0; i < PD_THEME_COLORS; i++) {
        th.color[i] = X->theme_clr[i];
    }

    snprintf(th.major, sizeof(th.major), "%s", X->theme_major);
    snprintf(th.minor, sizeof(th.minor), "%s", X->theme_minor);
    snprintf(th.major_ea, sizeof(th.major_ea), "%s", X->theme_major_ea);
    snprintf(th.minor_ea, sizeof(th.minor_ea), "%s", X->theme_minor_ea);
    snprintf(th.major_cs, sizeof(th.major_cs), "%s", X->theme_major_cs);
    snprintf(th.minor_cs, sizeof(th.minor_cs), "%s", X->theme_minor_cs);
    *out = th;
}

/* document-wide settings that are paragraph properties in Parade */
/* a child of a footnotePr or endnotePr: how the notes are numbered */
static void notepr_elem(const pd_markup* m, const char* t, int32_t* fmt, int32_t* start, int32_t* restart) {
    char v[32];

    if (!mu_attr(m, "w:val", v, sizeof(v))) {
        return;
    }

    if (strcmp(t, "numFmt") == 0) {
        *fmt = num_fmt_of(v);
        *fmt = *fmt == PD_NUM_NONE ? PD_NUM_DECIMAL : *fmt;
    } else if (strcmp(t, "numStart") == 0) {
        *start = atoi(v) > 0 ? atoi(v) : 0;
    } else if (strcmp(t, "numRestart") == 0) {
        *restart = strcmp(v, "eachSect") == 0 ? PD_NOTES_SECTION : strcmp(v, "eachPage") == 0 ? PD_NOTES_PAGE :
                   PD_NOTES_CONTINUOUS;
    }
}

static void read_settings(dxi* X, const char* xml, size_t n) {
    pd_markup m;
    int k = -1;

    mu_init(&m, xml, n, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);

        if (m.type == MT_OPEN && (strcmp(t, "footnotePr") == 0 || strcmp(t, "endnotePr") == 0)) {
            k = t[0] == 'e';
        } else if (m.type == MT_CLOSE && (strcmp(t, "footnotePr") == 0 || strcmp(t, "endnotePr") == 0)) {
            k = -1;
        } else if (k >= 0 && (m.type == MT_OPEN || m.type == MT_EMPTY)) {
            notepr_elem(&m, t, &X->note_fmt[k], &X->note_start[k], &X->note_restart[k]);
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && strcmp(mu_local(m.name), "autoHyphenation") == 0) {
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

/* Adobe's Symbol encoding from 0x20, as Unicode (0: no character of its own) */
static const uint16_t symbol_font[224] = {
    0x0020, 0x0021, 0x2200, 0x0023, 0x2203, 0x0025, 0x0026, 0x220B, 0x0028, 0x0029, 0x2217, 0x002B, 0x002C, 0x2212,
    0x002E, 0x002F, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B,
    0x003C, 0x003D, 0x003E, 0x003F, 0x2245, 0x0391, 0x0392, 0x03A7, 0x0394, 0x0395, 0x03A6, 0x0393, 0x0397, 0x0399,
    0x03D1, 0x039A, 0x039B, 0x039C, 0x039D, 0x039F, 0x03A0, 0x0398, 0x03A1, 0x03A3, 0x03A4, 0x03A5, 0x03C2, 0x03A9,
    0x039E, 0x03A8, 0x0396, 0x005B, 0x2234, 0x005D, 0x22A5, 0x005F, 0x203E, 0x03B1, 0x03B2, 0x03C7, 0x03B4, 0x03B5,
    0x03C6, 0x03B3, 0x03B7, 0x03B9, 0x03D5, 0x03BA, 0x03BB, 0x03BC, 0x03BD, 0x03BF, 0x03C0, 0x03B8, 0x03C1, 0x03C3,
    0x03C4, 0x03C5, 0x03D6, 0x03C9, 0x03BE, 0x03C8, 0x03B6, 0x007B, 0x007C, 0x007D, 0x223C, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x20AC, 0x03D2, 0x2032, 0x2264, 0x2044, 0x221E, 0x0192, 0x2663, 0x2666, 0x2665, 0x2660, 0x2194, 0x2190, 0x2191,
    0x2192, 0x2193, 0x00B0, 0x00B1, 0x2033, 0x2265, 0x00D7, 0x221D, 0x2202, 0x2022, 0x00F7, 0x2260, 0x2261, 0x2248,
    0x2026, 0x23D0, 0x23AF, 0x21B5, 0x2135, 0x2111, 0x211C, 0x2118, 0x2297, 0x2295, 0x2205, 0x2229, 0x222A, 0x2283,
    0x2287, 0x2284, 0x2282, 0x2286, 0x2208, 0x2209, 0x2220, 0x2207, 0x00AE, 0x00A9, 0x2122, 0x220F, 0x221A, 0x22C5,
    0x00AC, 0x2227, 0x2228, 0x21D4, 0x21D0, 0x21D1, 0x21D2, 0x21D3, 0x25CA, 0x27E8, 0x00AE, 0x00A9, 0x2122, 0x2211,
    0x239B, 0x239C, 0x239D, 0x23A1, 0x23A2, 0x23A3, 0x23A7, 0x23A8, 0x23A9, 0x23AA, 0, 0x27E9, 0x222B, 0x2320,
    0x23AE, 0x2321, 0x239E, 0x239F, 0x23A0, 0x23A4, 0x23A5, 0x23A6, 0x23AB, 0x23AC, 0x23AD, 0
};

/* a w:sym's character as Unicode where its font has a standard mapping (Symbol's, in the private use area
   or not); 0 when it has none (Wingdings, Webdings: the code stays, drawn in that font) */
static uint32_t sym_unicode(const char* font, uint32_t c) {
    if (c >= 0xF000 && c < 0xF100) {
        c -= 0xF000;
    }

    if (!same_ci(font, "Symbol") || c < 0x20 || c > 0xFF) {
        return 0;
    }

    return symbol_font[c - 0x20];
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

/* Word's w:numFmt as Parade's: the formats with a counterpart; others count in decimal */
static int32_t num_fmt_of(const char* v) {
    static const struct {
        const char* w;
        int32_t f;
    } map[] = {
        { "bullet", PD_NUM_BULLET }, { "none", PD_NUM_NONE }, { "lowerLetter", PD_NUM_LOWER_ALPHA },
        { "upperLetter", PD_NUM_UPPER_ALPHA }, { "lowerRoman", PD_NUM_LOWER_ROMAN },
        { "upperRoman", PD_NUM_UPPER_ROMAN }, { "decimalZero", PD_NUM_DECIMAL_ZERO }, { "ordinal", PD_NUM_ORDINAL },
        { "decimalEnclosedCircle", PD_NUM_CIRCLED }, { "decimalEnclosedCircleChinese", PD_NUM_CIRCLED },
        { "decimalFullWidth", PD_NUM_FULLWIDTH }, { "decimalFullWidth2", PD_NUM_FULLWIDTH },
        { "chineseCounting", PD_NUM_CJK_COUNTING }, { "chineseCountingThousand", PD_NUM_CJK_COUNTING },
        { "japaneseCounting", PD_NUM_CJK_COUNTING }, { "taiwaneseCounting", PD_NUM_CJK_COUNTING },
        { "taiwaneseCountingThousand", PD_NUM_CJK_COUNTING }, { "ideographTraditional", PD_NUM_CJK_TRADITIONAL },
        { "cardinalText", PD_NUM_CARDINAL_TEXT }, { "ordinalText", PD_NUM_ORDINAL_TEXT }, { "chicago", PD_NUM_SYMBOL }
    };
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(v, map[i].w) == 0) {
            return map[i].f;
        }
    }

    return PD_NUM_DECIMAL;
}

/* Parade's number format as Word's w:numFmt */
static const char* num_fmt_name(int32_t f) {
    static const char* fmts[PD_NUM_LAST + 1] = { "bullet", "decimal", "lowerLetter", "upperLetter", "lowerRoman",
                                                 "upperRoman", "none", "decimalZero", "ordinal",
                                                 "decimalEnclosedCircle", "decimalFullWidth", "chineseCounting",
                                                 "ideographTraditional", "cardinalText", "ordinalText",
                                                 "chicago"
                                               };

    return f >= 0 && f <= PD_NUM_LAST ? fmts[f] : "decimal";
}

/* room for want items of size sz in *p, which holds *cap; 0 when memory runs out */
static int grow(void** p, int* cap, int want, size_t sz) {
    void* q;
    int nc;

    if (want <= *cap) {
        return 1;
    }

    if (want > (1 << 20)) {
        return 0;
    }

    nc = *cap < 16 ? 16 : *cap * 2;
    nc = nc < want ? want : nc;

    if ((q = realloc(*p, (size_t)nc * sz)) == NULL) {
        return 0;
    }

    *p = q;
    *cap = nc;
    return 1;
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

        if (strcmp(t, "abstractNum") == 0 && grow((void**)&X->abss, &X->cabs, X->nabs + 1, sizeof(dabs))) {
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
            L->format = num_fmt_of(v);
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
        } else if (strcmp(t, "num") == 0 && grow((void**)&X->nums, &X->cnums, X->nnums + 1, sizeof(dnum))) {
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
    int rtl;                    /* w:bidiVisual: its columns from the right */
    pd_sp spacing;              /* w:tblCellSpacing */
    pd_block_id fl;             /* w:tblpPr: the float it is in, its props (width set at its end), else 0 */
    pd_float_props fp;
    pd_style_id sid;            /* its style as one of the document's: the cells and their text take it there */
    pd_sp row_h;                /* w:trHeight of the row under way */
    int cspan;                  /* the grid columns the cell under way takes */
    dtpart cell;                /* what the style does to the cell under way */
} dtab;

typedef struct {
    dxi* X;
    /* paragraph */
    int in_p, started, in_ppr, in_rpr, in_tcpr, in_trpr, in_sect;
    int in_notepr;              /* in a sectPr's footnotePr (1) or endnotePr (2) */
    struct {
        int on, drop, lines, wrap, has_x, has_y, xalign, hpage, vfrom;
        long w, h, x, y, hspace;
    } frm;                      /* the paragraph's w:framePr: a frame, a drop cap */
    pd_block_id frame_fl;       /* the float the paragraph is read into, for its frame */
    char pstyle[64];
    int num_id, ilvl, outline, sect_here;
    char hf_ref[6][64];         /* the section's header/footer r:ids: default, first, even; footers after */
    dprops ppr;                 /* the paragraph's own w:pPr */
    pd_char_props pcp;          /* character properties of the paragraph style, defaults included */
    pd_char_props tcp;          /* what the Parade style it was given already says, resolved */
    pd_section_props sp;
    /* run */
    pd_char_props rcp;          /* the run's own w:rPr */
    char rstyle[64];
    pd_char_props mcp;          /* the paragraph mark's w:rPr (in w:pPr) */
    char mstyle[64];
    int in_mrpr;
    int in_t, in_instr, in_drawing;
    /* fields and links */
    struct {
        char instr[512];
        size_t n;
        char res[256];          /* its result, while it is in its parent's instruction */
        size_t nres;
        int state;              /* 1 instruction, 2 result */
        int inner;              /* nested in its parent's instruction */
        int suppress;           /* its result not shown: Parade computes it */
        int kept, link;         /* a PD_FIELD_KEPT start, a link opened, to end with it */
    } fs[8];                    /* the fields open, the innermost last */
    int nfs, nfs_over;          /* how many, and those past the eighth (ignored) */
    /* tracked changes */
    int in_rprchg;              /* in a run's w:rPrChange: its old w:rPr */
    pd_char_props old_cp;
    char chg_author[64], chg_date[32];
    pd_rev_id fmt_rev;          /* the run's formatting change */
    char move_name[32][40];     /* the moves' names (moveFromRangeStart, moveToRangeStart), numbered from 1 */
    int nmove, move_cur;
    int link_depth[16], nlinks;
    /* drawings */
    char blip[64];
    int32_t crop[4];            /* the picture's a:srcRect: l, t, r, b in 100000ths */
    long long cx, cy;
    int anchor, wrap, posh_align, in_posh, in_offset, in_align;   /* a floating drawing */
    int posh_page, posh_has_off;    /* its offset is from the page's edge; it has one */
    int behind;                 /* behindDoc: under the text, when the text takes no notice of it */
    int in_posv, in_voffset, posv_para;     /* its offset down: from its paragraph (or line), not the page */
    long long posv_off;
    pd_res_id drawing_res;      /* a group or canvas made into a drawing resource */
    int in_vml;                 /* inside w:object or w:pict: a VML picture */
    char vml_style[400];        /* its shape's style (size, position), for one with text */
    char art[128], art_font[64];    /* WordArt: v:textpath's string and font */
    uint32_t vml_fill;          /* the shape's fillcolor (0: none said) */
    int vml_opacity;            /* v:fill's opacity, per-mille */
    int vml_wrap;               /* w10:wrap: -1 not said, else a pd_wrap (LEFT for square, NONE topAndBottom) */
    int math_para;              /* the paragraph opens with display math (m:oMathPara) */
    const char* tbx;            /* a floating text box's content, in the part being read */
    size_t tbn;
    long long posh_off, dist;
    struct {
        pd_res_id res;
        pd_sp w, h, gap, off_x, off_y;
        int wrap, has_off, off_from;
        const char* tbx;        /* a text box: its w:txbxContent, read into the float */
        size_t tbn;
        char* own;              /* tbx made here (WordArt's text as a paragraph), freed once read */
        int32_t crop[4];        /* a picture's crop */
        pd_rev_id rev;          /* inserted or deleted with its anchor */
    } pend_fl[8];               /* floats anchored in a paragraph already under way: after it */
    int npend_fl;
    /* tables */
    dtab tabs[8];               /* each table being read, the innermost last */
    int ntab;
    dedges cell_edges;          /* the cell's own w:tcBorders */
    int pend_row, row_header, pend_cell, span, cell_merge, cell_valign;
    pd_row_props row_pr;        /* the row's w:trPr, for the row when its first cell makes it */
    pd_sp cell_w;               /* the cell's w:tcW: twips made sp, or (cell_wpct) per-mille */
    int32_t cell_wpct;
    int32_t cell_dir;           /* its w:textDirection, PD_CELLTEXT_* */
    int in_pgb;                 /* in the section's w:pgBorders */
    int in_cols;                /* in its w:cols of unequal columns */
    pd_sp cell_diag[2];         /* its w:tl2br and w:tr2bl rules' widths, and colours */
    uint32_t cell_diag_c[2];
    uint32_t cell_bg, cell_bg_ref;
    int in_tblpr, in_grid, in_borders, in_mar, in_cb;
    pd_sp t_width, grid[PD_TABLE_MAX_COLS];
    int t_jc, ngrid, t_autofit;
    int skip;                   /* depth inside an ignored element */
    int note;                   /* parsing a footnote body */
    int after_ref;              /* just after the note's own number: drop the space that follows it */
    pd_rev_id rev;              /* inside w:ins or w:del: the tracked change the text is */
    int in_ruby, ruby_started;  /* a w:ruby: its guide's text, size and raise, read before its base */
    char ruby_text[256];
    pd_sp ruby_size, ruby_raise;
    unsigned sdt_inline;        /* the w:sdt being read, a bit each level: 1 a control in a paragraph's text */
    int sdt_depth;
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
    DW_DIFF(PD_CP_HSCALE, hscale)
    DW_DIFF(PD_CP_POSITION, position)
    DW_DIFF(PD_CP_KERNING, kerning)
    DW_DIFF(PD_CP_SIZE_CS, size_cs)
    DW_DIFF(PD_CP_WEIGHT_CS, weight_cs)
    DW_DIFF(PD_CP_ITALIC_CS, italic_cs)
#undef DW_DIFF

    /* a colour's link to the theme goes with it, and makes it different from the style's when the link is */
    if ((f->mask & PD_CP_COLOR) && (cp.mask & PD_CP_COLOR || f->color_theme != t->color_theme)) {
        cp.mask |= PD_CP_COLOR;
        cp.color = f->color;
        cp.color_theme = f->color_theme;
    }

    if ((f->mask & PD_CP_BACKGROUND) && (cp.mask & PD_CP_BACKGROUND || f->background_theme != t->background_theme)) {
        cp.mask |= PD_CP_BACKGROUND;
        cp.background = f->background;
        cp.background_theme = f->background_theme;
    }

    if ((f->mask & PD_CP_FAMILY) && PD_FONT_THEME_TEXT(f->font_theme) != PD_FONT_THEME_TEXT(t->font_theme)) {
        cp.mask |= PD_CP_FAMILY;
        memcpy(cp.family, f->family, sizeof(cp.family));
    }

    if ((f->mask & PD_CP_LANG) && strcmp(f->lang, t->lang)) {
        cp.mask |= PD_CP_LANG;
        memcpy(cp.lang, f->lang, sizeof(cp.lang));
    }

    if ((f->mask & PD_CP_FAMILY_EA) && !same_ci(f->family_ea, t->family_ea)) {
        cp.mask |= PD_CP_FAMILY_EA;
        memcpy(cp.family_ea, f->family_ea, sizeof(cp.family_ea));
    }

    if ((f->mask & PD_CP_FAMILY_CS) && !same_ci(f->family_cs, t->family_cs)) {
        cp.mask |= PD_CP_FAMILY_CS;
        memcpy(cp.family_cs, f->family_cs, sizeof(cp.family_cs));
    }

    cp.font_theme = f->font_theme;      /* kept for the families given (pd_doc_format masks it) */

    if (w->rev || w->X->float_rev || w->fmt_rev) {
        cp.mask |= PD_CP_REVISION;
        cp.revision = w->rev ? w->rev : w->X->float_rev ? w->X->float_rev : w->fmt_rev;
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

/* A table's w:tblpPr, looked for ahead of the reader (it comes in w:tblPr, after the table has begun): where Word
   floats it, as a float's props (its width left for the table's end); 0 if it does not float */
static int dw_tblp(dw* w, const pd_markup* at, pd_float_props* fp) {
    pd_markup q = *at;
    char v[32];
    int depth = 0;

    while (mu_next(&q) != MT_END) {
        const char* t = mu_local(q.name);

        if (q.type == MT_CLOSE && --depth < 0) {
            return 0;
        }

        if (q.type == MT_OPEN) {
            depth++;
        }

        if (!strcmp(t, "tr") || !strcmp(t, "tblGrid") || (q.type == MT_CLOSE && !strcmp(t, "tblPr"))) {
            return 0;
        }

        if ((q.type == MT_OPEN || q.type == MT_EMPTY) && depth <= 2 && !strcmp(t, "tblpPr")) {
            pd_sp x = twips(attr_int(&q, "w:tblpX", 0)), y = twips(attr_int(&q, "w:tblpY", 0));
            pd_sp lgap = twips(attr_int(&q, "w:leftFromText", 0)), rgap = twips(attr_int(&q, "w:rightFromText", 0));
            pd_sp textw = w->sp.page_width - w->X->margin_left - w->sp.margin_right;
            char spec[16] = "";

            fp->placement = PD_PLACE_HERE | PD_PLACE_FORCE;
            mu_attr(&q, "w:tblpXSpec", spec, sizeof(spec));

            if (mu_attr(&q, "w:horzAnchor", v, sizeof(v)) && !strcmp(v, "page")) {
                x -= w->X->margin_left;     /* from the page's edge: the column's is the margin's */
            }

            if (!strcmp(spec, "right") || !strcmp(spec, "outside")) {
                fp->wrap = PD_WRAP_RIGHT;
            } else if (!strcmp(spec, "center")) {
                fp->wrap = PD_WRAP_NONE;
            } else if (spec[0]) {
                fp->wrap = PD_WRAP_LEFT;
            } else {    /* at an offset: beside the text on the side it is nearer */
                fp->wrap = x > textw / 3 ? PD_WRAP_RIGHT : PD_WRAP_LEFT;
                fp->placement |= PD_PLACE_OFFSET;
                fp->offset_x = x;
            }

            fp->gap = fp->wrap == PD_WRAP_RIGHT ? lgap : rgap;
            fp->gap = fp->gap > 0 ? fp->gap : 0;
            fp->offset_y = y;
            fp->offset_from = !mu_attr(&q, "w:vertAnchor", v, sizeof(v)) || !strcmp(v, "margin") ? PD_FROM_MARGIN :
                              !strcmp(v, "page") ? PD_FROM_PAGE : PD_FROM_PARAGRAPH;
            strcpy(fp->sequence, "Table");
            return 1;
        }
    }

    return 0;
}

/* the row whose first cell is coming, with what its w:trPr said */
static void dw_row_begin(dw* w) {
    pd_bld* b = w->X->b;

    bld_row_begin(b, w->row_header);
    w->pend_row = 0;

    if (b->ntables >= 1 && b->ntables <= 8 && b->table[b->ntables - 1] && (w->row_pr.grid_before ||
            w->row_pr.grid_after || w->row_pr.width_before || w->row_pr.width_after || w->row_pr.height_exact || w->row_pr.split)) {
        pd_doc_set_row_props(b->d, b->row[b->ntables - 1], &w->row_pr);
    }
}

static void dw_begin_cell(dw* w) {
    pd_bld* b = w->X->b;

    if (w->pend_row) {
        dw_row_begin(w);
    }

    if (w->pend_cell) {
        uint32_t bg = w->cell_bg, bg_ref = w->cell_bg_ref;
        dedges e;

        memset(&e, 0, sizeof(e));

        if (w->ntab >= 1 && w->ntab <= 8) {
            w->tabs[w->ntab - 1].cspan = w->span > 0 ? w->span : 1;
        }

        if (w->ntab >= 1 && w->ntab <= 8) {     /* what the table's style does to this cell (a style of the document's
                                                   own does it itself, where the cell is laid out) */
            dtpart* c = &w->tabs[w->ntab - 1].cell;

            dw_cell_style(w, &w->tabs[w->ntab - 1]);

            if (!bg && c->has_shd && !w->tabs[w->ntab - 1].sid) {
                bg = c->shd;
                bg_ref = c->shd_ref;
            }

            if (!w->tabs[w->ntab - 1].sid) {
                e = c->cb;
            }
        }

        edges_over(&e, &w->cell_edges);
        bld_cell_begin(b, w->span, bg);
        w->pend_cell = 0;

        if ((e.set & 15) || bg_ref || (w->ntab >= 1 && w->ntab <= 8 && w->tabs[w->ntab - 1].row_h > 0)) {
            pd_cell_props cp;

            if (pd_doc_cell_props(b->d, bld_container(b), &cp) == PD_OK) {
                cp.border_set = e.set & 15;     /* PD_TBORDER_* outer bits are PD_BORDER_* */
                cp.border_on = e.on & e.set & 15;
                cp.border_width = e.w;
                cp.border_color = e.c ? e.c : 0xFF000000u;
                cp.border_theme = e.c ? e.ref : 0;
                cp.background_theme = bg ? bg_ref : 0;

                {   /* each edge's own, where it is not the widest (PD_TBORDER_ bits 0-3 are PD_BORDER_'s) */
                    int k2;

                    for (k2 = 0; k2 < 4; k2++) {
                        cp.edge_width[k2] = (e.on & (1 << k2)) && e.ew[k2] && e.ew[k2] != e.w ? e.ew[k2] : 0;
                    }
                }
                cp.min_height = w->ntab >= 1 && w->ntab <= 8 ? w->tabs[w->ntab - 1].row_h : 0;
                pd_doc_set_cell_props(b->d, bld_container(b), &cp);
            }
        }

        if (w->cell_merge) {
            bld_cell_merge_up(b);
        }

        if (w->cell_valign || w->cell_w || w->cell_wpct || w->cell_dir || w->cell_diag[0] || w->cell_diag[1]) {
            pd_cell_props cp;

            if (pd_doc_cell_props(b->d, bld_container(b), &cp) == PD_OK) {
                int dk;

                for (dk = 0; dk < 2; dk++) {
                    cp.diag_width[dk] = w->cell_diag[dk];
                    cp.diag_color[dk] = w->cell_diag[dk] ? w->cell_diag_c[dk] : 0;
                }

                cp.text_dir = w->cell_dir;
                cp.width = w->cell_w;
                cp.width_pct = w->cell_w ? 0 : w->cell_wpct;
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

/* a part of a Word table style as Parade's */
static void dx_tpart(const dxi* X, const dtpart* a, pd_table_style_part* p) {
    dprops pr = a->pr;
    int k;

    (void)X;

    memset(p, 0, sizeof(*p));

    if (!a->given) {
        return;
    }

    p->given = 1;
    p->para = pr.pp;
    p->para.mask &= ~PD_PP_NEXT_STYLE;
    p->chr = pr.cp;
    p->has_shading = a->has_shd;
    p->shading = a->shd;
    p->shading_theme = a->shd_ref;
    p->border_set = a->cb.set & 15;
    p->border_on = a->cb.on & a->cb.set & 15;
    p->border_width = a->cb.w;
    p->border_color = a->cb.c ? a->cb.c : 0xFF000000u;
    p->border_theme = a->cb.c ? a->cb.ref : 0;

    for (k = 0; k < 4; k++) {
        p->edge_width[k] = (p->border_on & (1 << k)) && a->cb.ew[k] && a->cb.ew[k] != a->cb.w ? a->cb.ew[k] : 0;
    }
}

/* Word's table style WID as one of the document's, with those it is based on, on first use: its id, 0 if none */
static pd_style_id dx_table_style_define(dxi* X, pd_bld* b, const char* wid, int depth) {
    static const int map[TP_N] = { PD_TPART_WHOLE, PD_TPART_FIRST_ROW, PD_TPART_LAST_ROW, PD_TPART_FIRST_COL,
                                   PD_TPART_LAST_COL, PD_TPART_BAND1_V, PD_TPART_BAND2_V, PD_TPART_BAND1_H,
                                   PD_TPART_BAND2_H, PD_TPART_NE, PD_TPART_NW, PD_TPART_SE, PD_TPART_SW
                                 };
    const dtstyle* st = NULL;
    const dstyle_x* sx;
    pd_table_style* ts;
    pd_style_id sid, parent = 0;
    char name[64];
    int i, k;

    for (i = 0; i < X->ntstyles && !st; i++) {
        st = !strcmp(X->tstyles[i].id, wid) ? &X->tstyles[i] : NULL;
    }

    if (!st || depth > 10) {
        return 0;
    }

    sx = find_style(X, wid);
    snprintf(name, sizeof(name), "%s", sx && sx->name[0] ? sx->name : wid);
    sid = pd_doc_style_find(b->d, name);

    if (sid && pd_doc_table_style_info(b->d, sid, NULL, NULL) != PD_OK) {    /* a paragraph style of the same name */
        snprintf(name, sizeof(name), "%.50s (table)", sx && sx->name[0] ? sx->name : wid);
        sid = pd_doc_style_find(b->d, name);
    }

    if (sid) {
        return pd_doc_table_style_info(b->d, sid, NULL, NULL) == PD_OK ? sid : 0;
    }

    if (st->based_on[0] && strcmp(st->based_on, wid) != 0) {
        parent = dx_table_style_define(X, b, st->based_on, depth + 1);
    }

    if ((ts = (pd_table_style*)malloc(sizeof(pd_table_style))) == NULL) {
        return 0;
    }

    pd_table_style_init(ts);

    for (k = 0; k < TP_N; k++) {
        dx_tpart(X, &st->part[k], &ts->part[map[k]]);
    }

    ts->part[PD_TPART_WHOLE].given = 1;
    ts->row_band = st->row_band > 0 ? st->row_band : 0;
    ts->col_band = st->col_band > 0 ? st->col_band : 0;
    ts->cell_padding = st->mar[3];
    ts->cell_padding_v = st->mar[0];
    ts->border_set = st->tb.set & 63;
    ts->border_on = st->tb.on & st->tb.set & 63;
    ts->border_width = st->tb.w;
    ts->border_color = st->tb.c ? st->tb.c : 0xFF000000u;
    ts->border_theme = st->tb.c ? st->tb.ref : 0;

    if (pd_doc_table_style_define(b->d, name, parent, ts, &sid) != PD_OK) {
        sid = 0;
    }

    free(ts);
    return sid;
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
        T->sid = 0;

        for (k = 0; found && k < w->X->ntstyles; k++) {   /* the style itself, the document's: what the table shows of
                                                             it is worked out where it is laid out */
            if (T->style[0] ? !strcmp(w->X->tstyles[k].id, T->style) : w->X->tstyles[k].is_default) {
                T->sid = dx_table_style_define(w->X, b, w->X->tstyles[k].id, 0);
                break;
            }
        }

        if (T->sid) {
            int lk = T->look;

            e = T->tb;
            tp.style = T->sid;
            tp.look = ((lk & 0x20) ? PD_TLOOK_FIRST_ROW : 0) | ((lk & 0x40) ? PD_TLOOK_LAST_ROW : 0) |
                      ((lk & 0x80) ? PD_TLOOK_FIRST_COL : 0) | ((lk & 0x100) ? PD_TLOOK_LAST_COL : 0) |
                      ((lk & 0x200) ? PD_TLOOK_NO_HBAND : 0) | ((lk & 0x400) ? PD_TLOOK_NO_VBAND : 0);
            tp.border_given = e.set & 63;
            tp.border = e.on ? e.w : 0;
            tp.border_sides = e.on == 63 ? 0 : e.on;
            tp.border_color = e.c ? e.c : 0xFF000000u;
            tp.border_theme = e.c ? e.ref : 0;
        } else if (!e.set && !found && T->style[0]) {
            tp.border = PD_PT(0.5);     /* a style the document lacks: Word's own grid, w:sz 4 */
            tp.border_sides = 0;
        } else {
            tp.border = e.on ? e.w : 0;
            tp.border_sides = e.on == 63 ? 0 : e.on ? e.on : 0;
            tp.border_color = e.c ? e.c : 0xFF000000u;
            tp.border_theme = e.c ? e.ref : 0;
        }

        /* cell margins: Word's 5.4pt at the sides and none above and below unless said */
        tp.cell_padding = T->mar[3] >= 0 ? T->mar[3] : T->st.mar[3] >= 0 ? T->st.mar[3] : twips(108);
        tp.cell_padding_v = T->mar[0] >= 0 ? T->mar[0] : T->st.mar[0] >= 0 ? T->st.mar[0] : 0;
        tp.indent = T->has_ind ? T->ind : T->st.has_ind ? T->st.ind : 0;
        tp.direction = T->rtl ? PD_DIR_RTL : PD_DIR_AUTO;
        tp.cell_spacing = T->spacing;
        tp.width_pct = (int32_t)T->width_pct;

        if (tp.width_pct > 0) {
            tp.width = 0;
        }
    }

    if (w->ntab >= 1 && w->ntab <= 8 && w->tabs[w->ntab - 1].fl) {  /* floating: its float as wide as it */
        dtab* T = &w->tabs[w->ntab - 1];
        pd_sp tw = 0;

        for (k = 0; k < w->ngrid; k++) {
            tw += w->grid[k];
        }

        T->fp.width = T->width_pct > 0 ? 0 : w->t_width > tw ? w->t_width : tw;
        T->fp.width_fraction = T->width_pct > 0 ? (int32_t)T->width_pct : 1000;
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
/* Word's paragraph style WID as one of the document's, made from its chain on first use: its id, 0 if it is none */
/* as: one of Parade's own styles the Word style is (Heading 1, Title, Quote, ...), defined as it says the first time */
static pd_style_id custom_style_define(dxi* X, pd_bld* b, const char* wid, const char* as) {
    const dstyle_x* st = find_style(X, wid);
    const char* name = as ? as : st && st->name[0] ? st->name : wid;
    pd_style_id normal = pd_doc_style_find(b->d, "Normal"), sid = pd_doc_style_find(b->d, name);
    int k, fresh = !sid;

    if (!st || st->type != 1 || !name[0] || strcmp(name, "Normal") == 0) {
        return sid;
    }

    for (k = 0; as && k < X->nbuiltin && strcmp(X->builtin[k], as) != 0; k++) {
    }

    if (as && k == X->nbuiltin && k < 16) {     /* Parade's own, not yet what the document says */
        snprintf(X->builtin[X->nbuiltin++], sizeof(X->builtin[0]), "%s", as);
        fresh = 1;
    }

    if (fresh) {
        dprops sty;
        pd_para_props rp;
        pd_char_props rc;

        memset(&sty, 0, sizeof(sty));
        style_chain(X, wid, &sty, 0);
        pd_doc_style_resolve(b->d, normal, &rp, &rc);
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
        if ((sty.pp.mask & PD_PP_LINE_SPACING) && (sty.pp.line_rule != rp.line_rule ||
                sty.pp.line_height != rp.line_height)) {
            /* a length where Normal has none (or another): said, whatever the multiple */
        } else {
            SAME_P(PD_PP_LINE_SPACING, line_spacing)
        }

        SAME_P(PD_PP_KEEP_NEXT, keep_with_next)
        SAME_P(PD_PP_KEEP_LINES, keep_lines)
        SAME_P(PD_PP_BREAK_BEFORE, page_break_before)
        SAME_P(PD_PP_HYPHENATE, hyphenate)
        SAME_P(PD_PP_CONTEXTUAL, contextual)
        SAME_P(PD_PP_SNAP_GRID, snap_grid)
        if (sty.pp.shading_theme == rp.shading_theme) {
            SAME_P(PD_PP_SHADING, shading)
        }
        SAME_C(PD_CP_SIZE, size)
        SAME_C(PD_CP_WEIGHT, weight)
        SAME_C(PD_CP_ITALIC, italic)
        if (sty.cp.color_theme == rc.color_theme) {     /* a different link to the theme is another colour */
            SAME_C(PD_CP_COLOR, color)
        }

        if (sty.cp.background_theme == rc.background_theme) {
            SAME_C(PD_CP_BACKGROUND, background)
        }
        SAME_C(PD_CP_UNDERLINE, underline)
        SAME_C(PD_CP_STRIKE, strike)
        SAME_C(PD_CP_SHIFT, shift)
        SAME_C(PD_CP_SMALLCAPS, small_caps)
        SAME_C(PD_CP_CAPS, caps)
        SAME_C(PD_CP_HIDDEN, hidden)
        SAME_C(PD_CP_LETTERSPACE, letter_space)
        SAME_C(PD_CP_HSCALE, hscale)
        SAME_C(PD_CP_POSITION, position)
        SAME_C(PD_CP_KERNING, kerning)
        SAME_C(PD_CP_SIZE_CS, size_cs)
        SAME_C(PD_CP_WEIGHT_CS, weight_cs)
        SAME_C(PD_CP_ITALIC_CS, italic_cs)
#undef SAME_P
#undef SAME_C

        if ((sty.cp.mask & PD_CP_FAMILY_EA) && same_ci(sty.cp.family_ea, rc.family_ea)) {
            sty.cp.mask &= ~PD_CP_FAMILY_EA;
        }

        if ((sty.cp.mask & PD_CP_FAMILY_CS) && same_ci(sty.cp.family_cs, rc.family_cs)) {
            sty.cp.mask &= ~PD_CP_FAMILY_CS;
        }

        if ((sty.cp.mask & PD_CP_FAMILY) && same_ci(sty.cp.family, rc.family) &&
                PD_FONT_THEME_TEXT(sty.cp.font_theme) == PD_FONT_THEME_TEXT(rc.font_theme)) {
            sty.cp.mask &= ~PD_CP_FAMILY;
        }

        if ((sty.cp.mask & PD_CP_LANG) && same_ci(sty.cp.lang, rc.lang)) {
            sty.cp.mask &= ~PD_CP_LANG;
        }

        if ((sty.pp.mask & PD_PP_TABS) && sty.pp.ntabs == rp.ntabs && sty.pp.tab_interval == rp.tab_interval &&
                memcmp(sty.pp.tabs, rp.tabs, (size_t)sty.pp.ntabs * sizeof(pd_tab_stop)) == 0) {
            sty.pp.mask &= ~PD_PP_TABS;
        }

        if ((sty.pp.mask & PD_PP_BORDER) && sty.pp.border_color == rp.border_color && sty.pp.border_theme == rp.border_theme &&
                sty.pp.border_width == rp.border_width && sty.pp.border_sides == rp.border_sides &&
                sty.pp.border_space == rp.border_space &&
                memcmp(sty.pp.border_line, rp.border_line, sizeof(rp.border_line)) == 0) {
            sty.pp.mask &= ~PD_PP_BORDER;
        }

        if (as && sid) {    /* Parade's own keeps the style that comes after it */
            pd_para_props own;

            if (pd_doc_style_info(b->d, sid, NULL, NULL, &own, NULL) == PD_OK && (own.mask & PD_PP_NEXT_STYLE)) {
                sty.pp.mask |= PD_PP_NEXT_STYLE;
                sty.pp.next_style = own.next_style;
            }
        }

        if (pd_doc_style_define(b->d, name, PD_STYLE_PARAGRAPH, normal, &sty.pp, &sty.cp, &sid) != PD_OK) {
            return 0;
        }
    }

    return sid;
}

static void dw_custom_style(dw* w, pd_bld* b) {
    const dstyle_x* st = find_style(w->X, w->pstyle);
    const char* name = st && st->name[0] ? st->name : w->pstyle;

    if (!st || st->type != 1 || !name[0] || strcmp(name, "Normal") == 0 || !custom_style_define(w->X, b, w->pstyle, NULL)) {
        return;
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

    if (w->frm.on && !w->frame_fl && !w->note && w->X->depth == 0 && w->ntab == 0) {
        w->frame_fl = bld_float_begin(b);   /* a frame: the paragraph is a float's, placed by dw_frame_end */
    }

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

    /* Parade's own styles for Word's: what Word's of the very name says made theirs (a template's headings) */
    if (strcmp(low, "title") == 0) {
        custom_style_define(w->X, b, w->pstyle, "Title");
        bld_para_style(b, "Title", PD_ROLE_TITLE, 0);
    } else if ((strncmp(low, "heading", 7) == 0 && isdigit((unsigned char)low[strlen(low) - 1])) ||
               (outline >= 0 && outline < 6)) {
        int lvl = outline >= 0 && outline < 6 ? outline + 1 : low[strlen(low) - 1] - '0';
        char sname[16];

        lvl = lvl < 1 ? 1 : lvl > 6 ? 6 : lvl;
        snprintf(sname, sizeof(sname), "Heading %d", lvl);

        if (!strncmp(low, "heading ", 8) && low[8] - '0' == lvl && !low[9]) {
            custom_style_define(w->X, b, w->pstyle, sname);
        }

        bld_para_style(b, sname, PD_ROLE_HEADING, lvl);
    } else if (strstr(low, "quote") || strcmp(low, "block text") == 0) {
        if (!strcmp(low, "quote")) {
            custom_style_define(w->X, b, w->pstyle, "Quote");
        }

        bld_para_style(b, "Quote", PD_ROLE_QUOTE, 0);
    } else if (strstr(low, "source") || strstr(low, "code") || strstr(low, "preformatted") ||
               strcmp(low, "plain text") == 0) {
        bld_para_style(b, "Code", PD_ROLE_CODE, 0);
    } else if (strcmp(low, "caption") == 0) {
        custom_style_define(w->X, b, w->pstyle, "Caption");
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

        if (w->ntab >= 1 && w->ntab <= 8 && w->tabs[w->ntab - 1].sid && w->tabs[w->ntab - 1].cell.given) {
            pd_table_style_part tpart;  /* what Parade lays the paragraph out with: its style's and its table style's */

            dx_tpart(w->X, &w->tabs[w->ntab - 1].cell, &tpart);
            pd_doc_style_resolve_with(b->d, sid, &tpart, &rp, &w->tcp);
        } else {
            pd_doc_style_resolve(b->d, sid, &rp, &w->tcp);
        }
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

        if ((f->mask & PD_PP_LINE_SPACING) && (f->line_spacing != rp.line_spacing || f->line_rule != rp.line_rule ||
                                               f->line_height != rp.line_height)) {
            b->pp.mask |= PD_PP_LINE_SPACING;
            b->pp.line_spacing = f->line_spacing;
            b->pp.line_rule = f->line_rule;
            b->pp.line_height = f->line_height;
        }

        DW_DIFF(PD_PP_KEEP_NEXT, keep_with_next)
        DW_DIFF(PD_PP_KEEP_LINES, keep_lines)
        DW_DIFF(PD_PP_BREAK_BEFORE, page_break_before)
        DW_DIFF(PD_PP_HYPHENATE, hyphenate)
        DW_DIFF(PD_PP_CONTEXTUAL, contextual)
        DW_DIFF(PD_PP_SNAP_GRID, snap_grid)
        DW_DIFF(PD_PP_DIRECTION, direction)
        DW_DIFF(PD_PP_SHADING, shading)
#undef DW_DIFF

        if ((f->mask & PD_PP_SHADING) && (b->pp.mask & PD_PP_SHADING || f->shading_theme != rp.shading_theme)) {
            b->pp.mask |= PD_PP_SHADING;    /* its link to the theme with it */
            b->pp.shading = f->shading;
            b->pp.shading_theme = f->shading_theme;
        }

        if ((f->mask & PD_PP_BORDER) && (f->border_color != rp.border_color || f->border_width != rp.border_width ||
                                         f->border_sides != rp.border_sides || f->border_space != rp.border_space ||
                                         f->border_theme != rp.border_theme ||
                                         memcmp(f->border_line, rp.border_line, sizeof(rp.border_line)) != 0)) {
            b->pp.mask |= PD_PP_BORDER;
            b->pp.border_theme = f->border_theme;
            b->pp.border_color = f->border_color;
            b->pp.border_width = f->border_width;
            b->pp.border_sides = f->border_sides;
            b->pp.border_space = f->border_space;
            memcpy(b->pp.border_line, f->border_line, sizeof(b->pp.border_line));
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
            struct dcmt* c = &w->X->cm[ci];

            if (c->pending && b->para) {
                pd_pos at;

                at.block = b->para;
                at.offset = 0;

                if ((c->pending & 1) && !c->m0) {
                    pd_doc_marker_new(b->d, at, PD_GRAVITY_LEFT, &c->m0);
                }

                if ((c->pending & 2) && !c->m1) {
                    pd_doc_marker_new(b->d, at, PD_GRAVITY_LEFT, &c->m1);
                }

                c->pending = 0;
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

    if (!w->in_p || !w->started) {     /* the paragraph has not begun (beginning it here would put a float
                                          read before its text after it): at its start, when it does */
        if (what == 0 && !c->m0) {
            c->pending |= 1;
        } else if (what == 1 || (what == 2 && !c->m1 && !(c->pending & 2))) {
            c->pending |= 2 | (c->m0 ? 0 : 1);
        }

        return;
    }

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

static int dw_fld_hidden(const dw* w);

static void dw_text(dw* w, const char* s, size_t n) {
    if (w->after_ref) {
        while (n > 0 && *s == ' ') {
            s++;
            n--;
        }

        w->after_ref = n == 0;
    }

    if (dw_fld_hidden(w) || n == 0) {
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
/* the change a float goes with when it is a deletion (its content then deleted too, gone where deletions are
   hidden); an insertion leaves its content as it is */
static pd_rev_id deleted_rev(const dxi* X, pd_rev_id rev) {
    pd_revision rv;

    return rev && pd_doc_revision_get(X->b->d, rev, &rv) == PD_OK && rv.kind == PD_REV_DELETE ? rev : 0;
}

static long long vml_length(const char* style, const char* prop);
static void dw_floats(dw* w);

/* a property of a VML style ("mso-position-horizontal:center"), its value into out; 0 when it has none */
static int vml_prop(const char* style, const char* prop, char* out, size_t cap) {
    const char* p = style;
    size_t n = strlen(prop), k = 0;

    while ((p = strstr(p, prop)) != NULL) {
        if ((p == style || p[-1] == ';' || p[-1] == ' ') && p[n] == ':') {
            for (p += n + 1; *p == ' ' || *p == '"' || *p == '\''; p++) {
            }

            while (*p && *p != ';' && *p != '"' && *p != '\'' && k + 1 < cap) {
                out[k++] = *p++;
            }

            out[k] = '\0';
            return 1;
        }

        p += n;
    }

    return 0;
}

/* a VML colour: #rrggbb, #rgb or a name (with Word's " [n]" index after it), as 0xFFRRGGBB; black if unknown */
static uint32_t vml_color(const char* v) {
    static const struct {
        const char* n;
        uint32_t c;
    } names[] = {
        { "black", 0x000000 }, { "white", 0xFFFFFF }, { "silver", 0xC0C0C0 }, { "gray", 0x808080 },
        { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 },
        { "navy", 0x000080 }, { "maroon", 0x800000 }, { "purple", 0x800080 }, { "teal", 0x008080 },
        { "olive", 0x808000 }, { "lime", 0x00FF00 }, { "aqua", 0x00FFFF }, { "fuchsia", 0xFF00FF }
    };
    size_t i;

    if (v[0] == '#' && strlen(v) >= 7) {
        return 0xFF000000u | (uint32_t)strtoul(v + 1, NULL, 16);
    }

    if (v[0] == '#' && strlen(v) >= 4) {
        uint32_t c = (uint32_t)strtoul(v + 1, NULL, 16);

        return 0xFF000000u | ((c >> 8) & 15) * 0x110000u | ((c >> 4) & 15) * 0x1100u | (c & 15) * 0x11u;
    }

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strncmp(v, names[i].n, strlen(names[i].n))) {
            return 0xFF000000u | names[i].c;
        }
    }

    return 0xFF000000u;
}

/* A VML shape with text -- a text box (v:textbox) or WordArt (v:textpath, a header's watermark) -- as a
   float like a DrawingML text box's: as big as its style says, where it puts it when it is absolutely
   positioned (from the margin, the page or the text, centred where it says), the text round it as w10:wrap
   says, else over it (under it, at a negative z-index); in the line when it is not positioned. WordArt's
   string becomes a paragraph of its font and fill (made lighter by the fill's opacity), as big as fits. */
static void dw_vml_float(dw* w) {
    dxi* X = w->X;
    const char* st = w->vml_style;
    char v[64];
    long long cx = vml_length(st, "width"), cy = vml_length(st, "height");
    int k, absolute = vml_prop(st, "position", v, sizeof(v)) && !strcmp(v, "absolute");
    pd_sp colw = X->page_w - X->margin_left - X->margin_right;

    if (cx <= 0 || cy <= 0 || w->npend_fl >= 8) {
        return;
    }

    k = w->npend_fl++;
    memset(&w->pend_fl[k], 0, sizeof(w->pend_fl[0]));
    w->pend_fl[k].w = (pd_sp)(cx * 65536 / 12700);
    w->pend_fl[k].h = (pd_sp)(cy * 65536 / 12700);
    w->pend_fl[k].rev = deleted_rev(X, w->rev);

    if (!absolute) {
        w->pend_fl[k].wrap = PD_WRAP_NONE;
    } else {
        char ha[16] = "", hr[16] = "", va[16] = "", vr[16] = "";
        pd_sp ox = (pd_sp)((vml_length(st, "margin-left") + vml_length(st, "left")) * 65536 / 12700);
        pd_sp oy = (pd_sp)((vml_length(st, "margin-top") + vml_length(st, "top")) * 65536 / 12700);

        vml_prop(st, "mso-position-horizontal", ha, sizeof(ha));
        vml_prop(st, "mso-position-horizontal-relative", hr, sizeof(hr));
        vml_prop(st, "mso-position-vertical", va, sizeof(va));
        vml_prop(st, "mso-position-vertical-relative", vr, sizeof(vr));

        if (w->vml_wrap >= 0) {
            w->pend_fl[k].wrap = w->vml_wrap == PD_WRAP_LEFT && !strcmp(ha, "right") ? PD_WRAP_RIGHT : w->vml_wrap;
        } else {
            w->pend_fl[k].wrap = vml_prop(st, "z-index", v, sizeof(v)) && atol(v) < 0 ? PD_WRAP_BEHIND : PD_WRAP_FRONT;
        }

        if (!strcmp(hr, "page")) {
            ox -= X->margin_left;
        }

        if (!strcmp(ha, "center") && colw > 0) {
            ox = (colw - w->pend_fl[k].w) / 2;
        } else if (!strcmp(ha, "right") && colw > 0) {
            ox = colw - w->pend_fl[k].w;
        }

        w->pend_fl[k].has_off = 1;
        w->pend_fl[k].off_x = ox;
        w->pend_fl[k].off_from = !strcmp(vr, "page") ? PD_FROM_PAGE : !strcmp(vr, "margin") ? PD_FROM_MARGIN :
                                 PD_FROM_PARAGRAPH;

        if (!strcmp(va, "center") && X->page_h > 0) {
            oy = ((w->pend_fl[k].off_from == PD_FROM_PAGE ? X->page_h : X->page_h - X->margin_top - X->margin_bottom) -
                  w->pend_fl[k].h) / 2;
            w->pend_fl[k].off_from = w->pend_fl[k].off_from == PD_FROM_PAGE ? PD_FROM_PAGE : PD_FROM_MARGIN;
        }

        w->pend_fl[k].off_y = oy;
    }

    if (w->tbx) {
        w->pend_fl[k].tbx = w->tbx;
        w->pend_fl[k].tbn = w->tbn;
    } else {    /* WordArt: its string as a paragraph */
        pd_buf o;
        uint32_t c = w->vml_fill ? w->vml_fill : 0xFF000000u;
        int op = w->vml_opacity < 0 ? 0 : w->vml_opacity > 1000 ? 1000 : w->vml_opacity, r, g, bl;
        size_t i, chars = 0;
        long long hp;

        for (i = 0; w->art[i]; i++) {
            chars += ((unsigned char)w->art[i] & 0xC0) != 0x80;
        }

        /* as tall as the shape, or as wide, a character 0.6 em */
        hp = cy * 2 / 12700;
        hp = chars && cx * 2 * 10 / (12700 * 6 * (long long)chars) < hp ? cx * 2 * 10 / (12700 * 6 * (long long)chars) :
             hp;
        hp = hp < 2 ? 2 : hp > 3276 ? 3276 : hp;
        r = ((int)((c >> 16) & 255) * op + 255 * (1000 - op) + 500) / 1000;
        g = ((int)((c >> 8) & 255) * op + 255 * (1000 - op) + 500) / 1000;
        bl = ((int)(c & 255) * op + 255 * (1000 - op) + 500) / 1000;
        memset(&o, 0, sizeof(o));
        pb_puts(&o, "<w:p><w:pPr><w:spacing w:before=\"0\" w:after=\"0\"/><w:jc w:val=\"center\"/></w:pPr><w:r><w:rPr>");

        if (w->art_font[0]) {
            pb_puts(&o, "<w:rFonts w:ascii=\"");
            xesc(&o, w->art_font, strlen(w->art_font));
            pb_puts(&o, "\" w:hAnsi=\"");
            xesc(&o, w->art_font, strlen(w->art_font));
            pb_puts(&o, "\"/>");
        }

        pb_printf(&o, "<w:color w:val=\"%02X%02X%02X\"/><w:sz w:val=\"%d\"/></w:rPr><w:t xml:space=\"preserve\">", r, g,
                  bl, (int)hp);
        xesc(&o, w->art, strlen(w->art));
        pb_puts(&o, "</w:t></w:r></w:p>");

        if (o.p) {
            w->pend_fl[k].own = o.p;
            w->pend_fl[k].tbx = o.p;
            w->pend_fl[k].tbn = o.n;
        } else {
            w->npend_fl--;
            return;
        }
    }

    if (!w->started) {
        dw_floats(w);
    }
}

/* A frame's paragraph read: the float it is in placed as its framePr says -- a drop cap beside the lines of
   text it spans (in the margin left of them for "margin"), a frame where its x and y put it, the text round it
   unless it says not -- and as wide as it says, else as its text (each character taken as 0.6 em, a drop cap's
   capital as 0.8). */
static void dw_frame_end(dw* w) {
    pd_bld* b = w->X->b;
    pd_float_props fp;
    pd_block_id fl = w->frame_fl;

    bld_float_end(b);
    w->frame_fl = 0;

    if (fl && pd_doc_float_props(b->d, fl, &fp) == PD_OK) {
        pd_block_id p = pd_doc_child(b->d, fl, 0);
        const char* t = NULL;
        uint32_t n = 0, i, chars = 0;
        pd_sp size = PD_PT(11), width;
        pd_run r[1];
        int32_t nr = 0;

        if (p && pd_doc_para_text(b->d, p, &t, &n) == PD_OK) {
            pd_char_props cp;

            for (i = 0; i < n; i++) {
                chars += ((unsigned char)t[i] & 0xC0) != 0x80;
            }

            if (pd_doc_para_runs(b->d, p, r, 1, &nr) == PD_OK && nr > 0 &&
                    pd_doc_format_resolve(b->d, p, r[0].format, &cp) == PD_OK && cp.size > 0) {
                size = cp.size;
            }
        }

        width = w->frm.w > 0 ? twips((int)w->frm.w) : (pd_sp)((int64_t)size * (w->frm.drop ? 8 : 6) / 10 *
                (chars ? chars : 1));
        fp.frame = w->frm.drop ? w->frm.drop : PD_FRAME_BOX;
        fp.frame_lines = w->frm.drop ? (w->frm.lines < 1 ? 1 : w->frm.lines > 10 ? 10 : w->frm.lines) : 0;
        fp.width = width;
        fp.gap = twips((int)w->frm.hspace);
        fp.placement = PD_PLACE_HERE | PD_PLACE_FORCE;
        fp.wrap = w->frm.wrap == PD_WRAP_NONE ? PD_WRAP_NONE : w->frm.xalign == 2 ? PD_WRAP_RIGHT : PD_WRAP_LEFT;

        if (w->frm.drop == PD_FRAME_MARGIN) {  /* in the margin: the text keeps its width */
            fp.placement |= PD_PLACE_OFFSET;
            fp.offset_x = -width - fp.gap;
        } else if (!w->frm.drop && w->frm.has_x && fp.wrap != PD_WRAP_NONE) {
            fp.placement |= PD_PLACE_OFFSET;
            fp.offset_x = twips((int)w->frm.x) - (w->frm.hpage ? w->X->margin_left : 0);
        }

        if (!w->frm.drop && w->frm.has_y) {
            fp.offset_y = twips((int)w->frm.y);
            fp.offset_from = w->frm.vfrom;
        }

        pd_doc_set_float_props(b->d, fl, &fp);
    }
}

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

            fp.offset_y = w->pend_fl[k].off_y;     /* down the paragraph it is anchored in, or the page */
            fp.offset_from = w->pend_fl[k].off_from;

            pd_doc_set_float_props(b->d, fl, &fp);
        }

        if (w->pend_fl[k].tbx) {    /* a text box: what it holds, read here, in the float */
            pd_char_props keep = b->cp;
            pd_rev_id outer = w->X->float_rev;

            w->X->depth++;
            w->X->float_rev = w->pend_fl[k].rev ? w->pend_fl[k].rev : outer;
            dw_parse(w->X, w->pend_fl[k].tbx, w->pend_fl[k].tbn, 1);
            w->X->float_rev = outer;
            w->X->depth--;
            free(w->pend_fl[k].own);
            w->pend_fl[k].own = NULL;
            bld_end_para(b);
            b->cp = keep;
            bld_float_end(b);
            continue;
        }

        memset(&o, 0, sizeof(o));
        o.kind = PD_INLINE_IMAGE;
        o.resource = w->pend_fl[k].res;
        memcpy(o.crop, w->pend_fl[k].crop, sizeof(o.crop));
        o.width = w->pend_fl[k].w;
        o.height = w->pend_fl[k].h;
        bld_begin_para(b);

        if (w->pend_fl[k].rev) {    /* a picture inserted or deleted */
            pd_char_props keep = b->cp, cp;

            memset(&cp, 0, sizeof(cp));
            cp.mask = PD_CP_REVISION;
            cp.revision = w->pend_fl[k].rev;
            bld_set_format(b, &cp);
            bld_inline(b, &o);
            bld_set_format(b, &keep);
        } else {
            bld_inline(b, &o);
        }

        bld_float_end(b);
    }

    w->npend_fl = 0;
}

/* a picture part the relationship id names, as a resource of the document; 0 if none */
static pd_res_id dw_resource(dxi* X, const char* rid) {
    const char* target;

    if (X->rebuild) {   /* made again: the pictures it had, by the ids its XML names them by */
        const pj_node* r = pj_get(pj_get(X->rebuild, "rels"), rid);

        return r ? (pd_res_id)pj_int_or(r, 0) : 0;
    }

    target = rel_target(X, rid, NULL);
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
        const unsigned char* u = (const unsigned char*)data;
        /* its type from its first bytes, else from its name (in any case: image36.GIF) */
        const char* mime = meta == 1 ? "image/x-emf" : meta == 2 ? "image/x-wmf" :
                           len >= 8 && !memcmp(u, "\x89PNG\r\n\x1a\n", 8) ? "image/png" :
                           len >= 6 && (!memcmp(u, "GIF87a", 6) || !memcmp(u, "GIF89a", 6)) ? "image/gif" :
                           len >= 3 && u[0] == 0xFF && u[1] == 0xD8 && u[2] == 0xFF ? "image/jpeg" :
                           len >= 2 && u[0] == 'B' && u[1] == 'M' ? "image/bmp" :
                           len >= 4 && (!memcmp(u, "II*\0", 4) || !memcmp(u, "MM\0*", 4)) ? "image/tiff" :
                           len >= 12 && !memcmp(u, "RIFF", 4) && !memcmp(u + 8, "WEBP", 4) ? "image/webp" :
                           ext && ext_is(ext, ".png") ? "image/png" : ext && ext_is(ext, ".gif") ? "image/gif" :
                           ext && (ext_is(ext, ".jpg") || ext_is(ext, ".jpeg")) ? "image/jpeg" :
                           ext && ext_is(ext, ".bmp") ? "image/bmp" :
                           ext && (ext_is(ext, ".tif") || ext_is(ext, ".tiff")) ? "image/tiff" : "application/octet-stream";

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

/* a DrawingML scheme colour by name, from the document's theme */
static uint32_t scheme_color(const dxi* X, const char* name) {
    int slot = pd_conv_theme_slot(name);

    return slot >= 0 ? X->theme_clr[slot] : 0xFF000000u;
}

typedef struct {                /* child coordinates to the drawing's: (a x + b y + ox, c x + d y + oy) */
    double ox, oy, sx, sy;      /* sx, sy: how much a unit along each axis becomes (sizes of what cannot turn) */
    double a, b, c, d;
} dxform;

static void fr_pt(const dxform* f, double x, double y, double* ox, double* oy) {
    *ox = f->a * x + f->b * y + f->ox;
    *oy = f->c * x + f->d * y + f->oy;
}

/* g after f: what maps by g, then by f */
static dxform fr_then(const dxform* f, const dxform* g) {
    dxform r;

    r.a = f->a * g->a + f->b * g->c;
    r.b = f->a * g->b + f->b * g->d;
    r.c = f->c * g->a + f->d * g->c;
    r.d = f->c * g->b + f->d * g->d;
    r.ox = f->a * g->ox + f->b * g->oy + f->ox;
    r.oy = f->c * g->ox + f->d * g->oy + f->oy;
    r.sx = sqrt(r.a * r.a + r.c * r.c);
    r.sy = sqrt(r.b * r.b + r.d * r.d);
    return r;
}

/* a box's own turn and flips (and a 3-D camera's projection, lin: its 2x2), about its centre (cx, cy), as a map */
static dxform fr_box(double cx, double cy, int rot, int fliph, int flipv, const double* lin) {
    double t = rot / 60000.0 * 3.14159265358979 / 180, ct = cos(t), st = sin(t);
    double fa = fliph ? -1 : 1, fd = flipv ? -1 : 1;
    double l0 = lin ? lin[0] : 1, l1 = lin ? lin[1] : 0, l2 = lin ? lin[3] : 0, l3 = lin ? lin[4] : 1;
    dxform r;

    /* flipped first, then projected, then turned: R * L * F */
    double m0 = l0 * fa, m1 = l1 * fd, m2 = l2 * fa, m3 = l3 * fd;

    r.a = ct * m0 - st * m2;
    r.b = ct * m1 - st * m3;
    r.c = st * m0 + ct * m2;
    r.d = st * m1 + ct * m3;
    r.ox = cx - (r.a * cx + r.b * cy);
    r.oy = cy - (r.c * cx + r.d * cy);
    r.sx = sqrt(r.a * r.a + r.c * r.c);
    r.sy = sqrt(r.b * r.b + r.d * r.d);
    return r;
}

/* A 3-D camera (scene3d) seen in parallel: its rotation (3x3, row by row: x, y on the page, then depth toward the
   viewer), from the preset (Office's values, as LibreOffice reads them) and an explicit rot over it; fr_box takes
   the part in the shape's plane. 0 when there is none, or it is head-on. */
static int camera_lin(const char* prst, int has_rot, double lat, double lon, double rev, double* lin) {
    static const struct {
        const char* n;
        double lat, lon, rev;
    } p[] = {
        { "isometricBottomDown", 2124000, 18882000, 17988000 }, { "isometricBottomUp", 2124000, 2718000, 3612000 },
        { "isometricLeftDown", 2100000, 2700000, 0 }, { "isometricLeftUp", 19500000, 2700000, 0 },
        { "isometricOffAxis1Left", 1080000, 3840000, 0 }, { "isometricOffAxis1Right", 1080000, 20040000, 0 },
        { "isometricOffAxis1Top", 18078000, 18390000, 3456000 }, { "isometricOffAxis2Left", 1080000, 1560000, 0 },
        { "isometricOffAxis2Right", 1080000, 17760000, 0 }, { "isometricOffAxis2Top", 18078000, 3210000, 18144000 },
        { "isometricOffAxis3Bottom", 3522000, 18390000, 18144000 }, { "isometricOffAxis3Left", 20520000, 3840000, 0 },
        { "isometricOffAxis3Right", 20520000, 20040000, 0 }, { "isometricOffAxis4Bottom", 3522000, 3210000, 3456000 },
        { "isometricOffAxis4Left", 20520000, 1560000, 0 }, { "isometricOffAxis4Right", 20520000, 17760000, 0 },
        { "isometricRightDown", 19500000, 18900000, 0 }, { "isometricRightUp", 2100000, 18900000, 0 },
        { "isometricTopDown", 19476000, 2718000, 17988000 }, { "isometricTopUp", 19476000, 18882000, 3612000 }
    };
    size_t i;
    double la = 0, lo = 0, re = 0, d2r = 3.14159265358979 / 180 / 60000, X[9], Y[9], Z[9], XY[9], M[9];
    int found = 0, k, j, q;

    for (i = 0; prst && i < sizeof(p) / sizeof(p[0]); i++) {
        if (!strcmp(prst, p[i].n)) {
            la = p[i].lat;
            lo = p[i].lon;
            re = p[i].rev;
            found = 1;
        }
    }

    if (has_rot) {
        la = lat;
        lo = lon;
        re = rev;
        found = 1;
    }

    if (!found || (la == 0 && lo == 0 && re == 0)) {
        return 0;
    }

    la *= d2r;
    lo *= d2r;
    re *= d2r;
    /* about y by lon, then x by lat, then z by rev: Z * X * Y */
    memset(X, 0, sizeof(X));
    memset(Y, 0, sizeof(Y));
    memset(Z, 0, sizeof(Z));
    X[0] = 1;
    X[4] = cos(la); X[8] = cos(la); X[5] = sin(la); X[7] = -sin(la);
    Y[4] = 1;
    Y[0] = cos(lo); Y[8] = cos(lo); Y[2] = -sin(lo); Y[6] = sin(lo);
    Z[8] = 1;
    Z[0] = cos(re); Z[4] = cos(re); Z[1] = sin(re); Z[3] = -sin(re);

    for (k = 0; k < 3; k++) {
        for (j = 0; j < 3; j++) {
            XY[3 * k + j] = 0;

            for (q = 0; q < 3; q++) {
                XY[3 * k + j] += X[3 * k + q] * Y[3 * q + j];
            }
        }
    }

    for (k = 0; k < 3; k++) {
        for (j = 0; j < 3; j++) {
            M[3 * k + j] = 0;

            for (q = 0; q < 3; q++) {
                M[3 * k + j] += Z[3 * k + q] * XY[3 * q + j];
            }
        }
    }

    for (k = 0; k < 9; k++) {
        lin[k] = M[k];
    }

    return 1;
}

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
    } else if ((!strcmp(t, "ext") || !strcmp(t, "chExt")) && mu_attr(m, "cx", v, sizeof(v))) {
        /* (an a:ext of an extension list, a:ext uri="...", is no size) */
        long long* d = !strcmp(t, "ext") ? f->ext : f->chext;

        d[0] = atoll(v);
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

/* An extruded shape (sp3d extrusionH) seen through its camera, drawn as a prism: the outline (px, py, in the
   group's child space, np points, closed) as the front face, the same moved back by the depth, and a side for each
   edge, every face as a path, far ones first, each shaded by how it faces a light from the upper left and the
   front. The front keeps the shape's fill and outline; the sides and back take the extrusion colour. */
static void dw_extruded(pd_buf* o, int* first, const dxform* FR, const double* M, double cx, double cy, int rot,
                        int fliph, int flipv, const double* px, const double* py, int np, double depth, uint32_t fill,
                        uint32_t side, uint32_t line, double lwd) {
    double t = rot / 60000.0 * 3.14159265358979 / 180, ct = cos(t), st = sin(t), fa = fliph ? -1 : 1,
           fd = flipv ? -1 : 1, area = 0, L[3] = { -0.35, -0.55, 1 }, ll, front_nl;
    double fx[64], fy[64], bx[64], by[64], fz[64];
    struct {
        int i;          /* -1 front, -2 back, else the side from point i */
        double z, nl;
    } face[66], tmp;
    int n = 0, i, j, k;

    if (np < 3 || np > 64) {
        return;
    }

    ll = sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);

    for (i = 0; i < 3; i++) {
        L[i] /= ll;
    }

    for (i = 0; i < np; i++) {      /* each point, at the front and at the back */
        double u = (px[i] - cx) * fa, v = (py[i] - cy) * fd, X, Y, rx, ry;

        for (k = 0; k < 2; k++) {
            double z = k ? -depth : 0;

            X = M[0] * u + M[1] * v + M[2] * z;
            Y = M[3] * u + M[4] * v + M[5] * z;
            rx = ct * X - st * Y + cx;
            ry = st * X + ct * Y + cy;
            fr_pt(FR, rx, ry, k ? &bx[i] : &fx[i], k ? &by[i] : &fy[i]);
        }

        fz[i] = M[6] * u + M[7] * v;
        area += (px[i] - cx) * fa * ((py[(i + 1) % np] - cy) * fd) - ((px[(i + 1) % np] - cx) * fa) * ((py[i] - cy) * fd);
    }

    front_nl = M[2] * L[0] + M[5] * L[1] + M[8] * L[2];
    face[n].i = -1;
    face[n].z = 0;
    face[n++].nl = front_nl;
    face[n].i = -2;
    face[n].z = -depth * M[8];
    face[n++].nl = -front_nl;

    for (i = 0; i < np; i++) {      /* the sides: their outward normals turned by the camera */
        int i2 = (i + 1) % np;
        double ex = (px[i2] - px[i]) * fa, ey = (py[i2] - py[i]) * fd, nx = ey, ny = -ex, nn, cnx, cny, cnz;

        if (area < 0) {
            nx = -nx;
            ny = -ny;
        }

        nn = sqrt(nx * nx + ny * ny);

        if (nn <= 0) {
            continue;
        }

        nx /= nn;
        ny /= nn;
        cnx = M[0] * nx + M[1] * ny;
        cny = M[3] * nx + M[4] * ny;
        cnz = M[6] * nx + M[7] * ny;
        face[n].i = i;
        face[n].z = (fz[i] + fz[i2]) / 2 - depth * M[8] / 2 + cnz * 1e-3;
        face[n++].nl = cnx * L[0] + cny * L[1] + cnz * L[2];
    }

    for (i = 1; i < n; i++) {       /* far to near: the nearer drawn over */
        for (j = i; j > 0 && face[j - 1].z > face[j].z; j--) {
            tmp = face[j];
            face[j] = face[j - 1];
            face[j - 1] = tmp;
        }
    }

    for (i = 0; i < n; i++) {
        uint32_t base = face[i].i == -1 ? fill : side, c;
        double shade = (0.45 + 0.55 * (face[i].nl > 0 ? face[i].nl : 0)) / (0.45 + 0.55 * (front_nl > 0 ? front_nl : 0));
        int r, g, b;

        if (!base) {
            continue;
        }

        if (face[i].i == -1) {
            shade = 1;
        }

        shade = shade > 1.25 ? 1.25 : shade;
        r = (int)(((base >> 16) & 255) * shade);
        g = (int)(((base >> 8) & 255) * shade);
        b = (int)((base & 255) * shade);
        c = (base & 0xFF000000u) | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 |
            (uint32_t)(b > 255 ? 255 : b);

        if (!*first) {
            pb_putc(o, ',');
        }

        *first = 0;
        pb_puts(o, "{\"path\":[");

        if (face[i].i < 0) {
            for (k = 0; k < np; k++) {
                pb_printf(o, "%s%d,%d", k ? "," : "", (int)emu_sp(face[i].i == -1 ? fx[k] : bx[k]),
                          (int)emu_sp(face[i].i == -1 ? fy[k] : by[k]));
            }
        } else {
            int a = face[i].i, b2 = (a + 1) % np;

            pb_printf(o, "%d,%d,%d,%d,%d,%d,%d,%d", (int)emu_sp(fx[a]), (int)emu_sp(fy[a]), (int)emu_sp(fx[b2]),
                      (int)emu_sp(fy[b2]), (int)emu_sp(bx[b2]), (int)emu_sp(by[b2]), (int)emu_sp(bx[a]),
                      (int)emu_sp(by[a]));
        }

        /* the sides and the back outlined in their own colour, a hairline: neighbouring faces of a curved side
           otherwise show the background through the anti-aliased seam between them */
        pb_printf(o, "],\"closed\":1,\"fill\":%u,\"line\":%u,\"lw\":%d}", (unsigned)c,
                  (unsigned)(face[i].i == -1 ? line : c), (int)emu_sp(face[i].i == -1 ? lwd : 6350));
    }
}

/* DrawingML shape guides: a name's value -- one of the shape's own guides, or a built-in (w, h, hc, ss, wd2, ...)
   from its width and height -- or a number written out. */
typedef struct {
    char name[48][40];
    double val[48];
    int n;
    double w, h;
} dw_guides;

static double dw_guide_arg(const dw_guides* G, const char* a) {
    double w = G->w, h = G->h, ss = w < h ? w : h, ls = w > h ? w : h;
    int i;

    if ((a[0] >= '0' && a[0] <= '9') || a[0] == '-' || a[0] == '+' || a[0] == '.') {
        return atof(a);
    }

    for (i = G->n - 1; i >= 0; i--) {
        if (!strcmp(G->name[i], a)) {
            return G->val[i];
        }
    }

    if (!strcmp(a, "w") || !strcmp(a, "r")) return w;
    if (!strcmp(a, "h") || !strcmp(a, "b")) return h;
    if (!strcmp(a, "l") || !strcmp(a, "t")) return 0;
    if (!strcmp(a, "hc")) return w / 2;
    if (!strcmp(a, "vc")) return h / 2;
    if (!strcmp(a, "ss")) return ss;
    if (!strcmp(a, "ls")) return ls;
    if (!strncmp(a, "wd", 2)) return w / atof(a + 2);
    if (!strncmp(a, "hd", 2)) return h / atof(a + 2);
    if (!strncmp(a, "ssd", 3)) return ss / atof(a + 3);
    if (!strcmp(a, "cd2")) return 10800000;
    if (!strcmp(a, "cd4")) return 5400000;
    if (!strcmp(a, "cd8")) return 2700000;
    if (!strcmp(a, "3cd4")) return 16200000;
    if (!strcmp(a, "3cd8")) return 8100000;
    if (!strcmp(a, "5cd8")) return 13500000;
    if (!strcmp(a, "7cd8")) return 18900000;
    return 0;
}

/* a guide's formula (multiply-divide, add-subtract, "val n", min, max, pin, ...) evaluated, and kept under its
   name */
static double dw_guide_add(dw_guides* G, const char* name, const char* fmla) {
    char op[8] = "", a[3][40];
    double x, y, z, r = 0;
    int n;

    a[0][0] = a[1][0] = a[2][0] = '\0';
    n = sscanf(fmla, "%7s %39s %39s %39s", op, a[0], a[1], a[2]);
    x = n > 1 ? dw_guide_arg(G, a[0]) : 0;
    y = n > 2 ? dw_guide_arg(G, a[1]) : 0;
    z = n > 3 ? dw_guide_arg(G, a[2]) : 0;

    if (!strcmp(op, "*/")) r = z != 0 ? x * y / z : 0;
    else if (!strcmp(op, "+-")) r = x + y - z;
    else if (!strcmp(op, "+/")) r = z != 0 ? (x + y) / z : 0;
    else if (!strcmp(op, "val")) r = x;
    else if (!strcmp(op, "abs")) r = fabs(x);
    else if (!strcmp(op, "sqrt")) r = x > 0 ? sqrt(x) : 0;
    else if (!strcmp(op, "min")) r = x < y ? x : y;
    else if (!strcmp(op, "max")) r = x > y ? x : y;
    else if (!strcmp(op, "pin")) r = y < x ? x : y > z ? z : y;
    else if (!strcmp(op, "?:")) r = x > 0 ? y : z;
    else if (!strcmp(op, "mod")) r = sqrt(x * x + y * y + z * z);

    if (G->n < 48) {
        snprintf(G->name[G->n], sizeof(G->name[0]), "%s", name);
        G->val[G->n++] = r;
    } else {    /* the oldest go: a freeform's guides are used near where they are made */
        memmove(G->name[0], G->name[1], sizeof(G->name[0]) * 47);
        memmove(&G->val[0], &G->val[1], sizeof(G->val[0]) * 47);
        snprintf(G->name[47], sizeof(G->name[0]), "%s", name);
        G->val[47] = r;
    }

    return r;
}

/* A DrawingML colour element (srgbClr, schemeClr, sysClr, prstClr) as ARGB; 0 if T is none of them. */
static int dw_color(const dxi* X, const pd_markup* g, const char* t, uint32_t* out) {
    static const struct {
        const char* n;
        uint32_t c;
    } prst[] = {
        { "white", 0xFFFFFF }, { "black", 0x000000 }, { "red", 0xFF0000 }, { "green", 0x008000 }, { "blue", 0x0000FF },
        { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "orange", 0xFFA500 },
        { "ltGray", 0xD3D3D3 }, { "dkGray", 0xA9A9A9 }, { "silver", 0xC0C0C0 }, { "navy", 0x000080 }
    };
    char v[64];
    size_t i;

    if (!strcmp(t, "srgbClr") || !strcmp(t, "sysClr")) {
        *out = mu_attr(g, !strcmp(t, "sysClr") ? "lastClr" : "val", v, sizeof(v)) ?
               0xFF000000u | (uint32_t)strtoul(v, NULL, 16) : 0xFF000000u;
        return 1;
    }

    if (!strcmp(t, "schemeClr")) {
        *out = mu_attr(g, "val", v, sizeof(v)) ? scheme_color(X, v) : 0xFF000000u;
        return 1;
    }

    if (!strcmp(t, "prstClr")) {
        *out = 0xFF000000u;

        for (i = 0; mu_attr(g, "val", v, sizeof(v)) && i < sizeof(prst) / sizeof(prst[0]); i++) {
            if (!strcmp(v, prst[i].n)) {
                *out = 0xFF000000u | prst[i].c;
            }
        }

        return 1;
    }

    return 0;
}

/* a drawing made again: the story of its next text box, as the description it had names them in order */
static pd_block_id dw_rebuild_story(dxi* X) {
    const pj_node* it, *items = pj_get(X->rebuild, "items");
    int k = 0, want = X->rebuild_story++;

    if (X->rebuild_map) {   /* the text boxes as the editor marked them: moved ones keep their own text */
        want = want < X->rebuild_nmap ? X->rebuild_map[want] : -1;
    }

    for (it = items ? items->child : NULL; it && want >= 0; it = it->next) {
        if (pj_get(it, "story") && k++ == want) {
            return (pd_block_id)pj_int_or(pj_get(it, "story"), 0);
        }
    }

    return 0;
}

/* a preset drawn from Office's definitions (pd_preset.c): every one but those drawn here as they always were */
static int dw_generic_preset(const char* p) {
    return pd_preset_known(p) && strcmp(p, "rect") && strcmp(p, "ellipse") && strcmp(p, "roundRect") &&
           strcmp(p, "line") && !strstr(p, "Connector");
}

/* a colour as a preset's path shades it: darker or lighter for its sides (a cube's, a can's top) */
static uint32_t dw_shade(uint32_t c, const char* mode) {
    double k = !strcmp(mode, "darken") ? -0.4 : !strcmp(mode, "darkenLess") ? -0.2 : !strcmp(mode, "lighten") ? 0.4 :
               !strcmp(mode, "lightenLess") ? 0.2 : 0;
    uint32_t out = c & 0xFF000000u;
    int sh;

    for (sh = 0; sh <= 16; sh += 8) {
        double v = (c >> sh) & 255;

        v = k < 0 ? v * (1 + k) : v + (255 - v) * k;
        out |= (uint32_t)(v + 0.5) << sh;
    }

    return out;
}

/* Office's preset shapes that are polygons: their corners in a box of 1000 by 1000 (the adjustments at their
   defaults), drawn as a custom geometry is */
static const struct {
    const char* n;
    int np;
    short xy[24];
} dw_presets[] = {
    { "triangle", 3, { 500, 0, 1000, 1000, 0, 1000 } },
    { "rtTriangle", 3, { 0, 0, 1000, 1000, 0, 1000 } },
    { "diamond", 4, { 500, 0, 1000, 500, 500, 1000, 0, 500 } },
    { "flowChartDecision", 4, { 500, 0, 1000, 500, 500, 1000, 0, 500 } },
    { "parallelogram", 4, { 250, 0, 1000, 0, 750, 1000, 0, 1000 } },
    { "flowChartInputOutput", 4, { 200, 0, 1000, 0, 800, 1000, 0, 1000 } },
    { "trapezoid", 4, { 250, 0, 750, 0, 1000, 1000, 0, 1000 } },
    { "pentagon", 5, { 500, 0, 1000, 382, 809, 1000, 191, 1000, 0, 382 } },
    { "hexagon", 6, { 250, 0, 750, 0, 1000, 500, 750, 1000, 250, 1000, 0, 500 } },
    { "octagon", 8, { 293, 0, 707, 0, 1000, 293, 1000, 707, 707, 1000, 293, 1000, 0, 707, 0, 293 } },
    { "rightArrow", 7, { 0, 250, 500, 250, 500, 0, 1000, 500, 500, 1000, 500, 750, 0, 750 } },
    { "leftArrow", 7, { 1000, 250, 500, 250, 500, 0, 0, 500, 500, 1000, 500, 750, 1000, 750 } },
    { "upArrow", 7, { 250, 1000, 250, 500, 0, 500, 500, 0, 1000, 500, 750, 500, 750, 1000 } },
    { "downArrow", 7, { 250, 0, 250, 500, 0, 500, 500, 1000, 1000, 500, 750, 500, 750, 0 } },
    { "leftRightArrow", 10, { 0, 500, 250, 0, 250, 250, 750, 250, 750, 0, 1000, 500, 750, 1000, 750, 750, 250, 750, 250, 1000 } },
    { "homePlate", 5, { 0, 0, 750, 0, 1000, 500, 750, 1000, 0, 1000 } },
    { "chevron", 6, { 0, 0, 750, 0, 1000, 500, 750, 1000, 0, 1000, 250, 500 } },
    { "star5", 10, { 500, 0, 612, 345, 976, 345, 682, 559, 794, 905, 500, 691, 206, 905, 318, 559, 24, 345, 388, 345 } },
    { "plus", 12, { 250, 0, 750, 0, 750, 250, 1000, 250, 1000, 750, 750, 750, 750, 1000, 250, 1000, 250, 750, 0, 750, 0, 250, 250, 250 } },
};

/* N bytes of H holding the first of NN bytes of NEEDLE, or NULL */
static const char* dw_memmem(const char* h, size_t n, const char* needle, size_t nn) {
    size_t i;

    for (i = 0; nn && i + nn <= n; i++) {
        if (h[i] == needle[0] && !memcmp(h + i, needle, nn)) {
            return h + i;
        }
    }

    return NULL;
}

/* Whether markup RAW0..RAW1 can be written back as it is: every prefix one a written document declares, and every
   reference (r:embed, r:id, r:link, o:relid) to a picture -- one read already (REL_ID), or, with LOAD, read now and
   added. */
static int dw_xml_writable(dxi* X, const char* raw0, const char* raw1, char (*rel_id)[24], pd_res_id* rel_res, int* nrel,
                           int load) {
    const char* q;
    int ok = raw1 > raw0, k;

    for (q = raw0; ok && q < raw1; q++) {
        const char* e, *c;

        if (*q != '<' || q + 1 >= raw1 || q[1] == '/' || q[1] == '?' || q[1] == '!') {
            continue;
        }

        for (e = q + 1; e < raw1 && *e != '>'; e++) {
            if (*e == '"') {    /* past a value, which may hold anything */
                const char* ce = memchr(e + 1, '"', (size_t)(raw1 - e - 1));

                e = ce ? ce : raw1 - 1;
            }
        }

        for (c = q + 1; ok && c < e; c++) {
            const char* b2;

            if (*c == '"') {
                const char* ce = memchr(c + 1, '"', (size_t)(e - c - 1));

                c = ce ? ce : e;
                continue;
            }

            if (*c != ':') {
                continue;
            }

            for (b2 = c; b2 > q + 1 && (isalnum((unsigned char)b2[-1]) || b2[-1] == '_' || b2[-1] == '-'); b2--) {
            }

            if (c - b2 == 5 && !memcmp(b2, "xmlns", 5)) {
                continue;
            }

            if (!dx_prefix_declared(b2, (size_t)(c - b2))) {
                ok = 0;
            } else if ((c - b2 == 1 && *b2 == 'r') || (c - b2 == 1 && *b2 == 'o' && !strncmp(c + 1, "relid=", 6))) {
                const char* qv = memchr(c, '"', (size_t)(e - c));
                const char* qe = qv ? memchr(qv + 1, '"', (size_t)(e - qv - 1)) : NULL;
                int found = 0;

                if (!qv || !qe || (size_t)(qe - qv - 1) >= sizeof(rel_id[0])) {
                    ok = 0;
                    continue;
                }

                for (k = 0; k < *nrel; k++) {
                    found |= strlen(rel_id[k]) == (size_t)(qe - qv - 1) && !memcmp(rel_id[k], qv + 1, (size_t)(qe - qv - 1));
                }

                if (!found && load && *nrel < 64) {     /* a picture the fallback alone shows */
                    char id[24];
                    pd_res_id r;

                    memcpy(id, qv + 1, (size_t)(qe - qv - 1));
                    id[qe - qv - 1] = '\0';
                    r = dw_resource(X, id);

                    if (r) {
                        snprintf(rel_id[*nrel], sizeof(rel_id[0]), "%s", id);
                        rel_res[(*nrel)++] = r;
                        found = 1;
                    }
                }

                ok = found;
            }
        }

        q = e;
    }

    return ok;
}

/* The group's own XML (RAW0 to RAW1) into its drawing, and the VML Word keeps beside it for readers without
   DrawingML (FB0 to FB1, its mc:Fallback, or NULL), with the pictures they name by relationship id -- when every
   reference is to a picture and every prefix one a written document declares, so that a .docx gets them back as
   they came rather than the drawing made from them. Otherwise nothing, and the drawing is written. */
static void dw_keep_xml(dxi* X, pd_buf* o, const char* raw0, const char* raw1, const char* fb0, const char* fb1,
                        int canvas, int rel_ok, char (*rel_id)[24], pd_res_id* rel_res, int nrel) {
    static const char* const renumbered[] = { "<w:numPr", "<w:commentRangeStart", "<w:commentReference",
                                              "<w:footnoteReference", "<w:endnoteReference"
                                            };
    static const char* const styled[] = { "w:pStyle w:val=\"", "w:rStyle w:val=\"", "w:tblStyle w:val=\"" };
    size_t rawn = (size_t)(raw1 - raw0), j;
    int ok = rel_ok && rawn > 0 && rawn < 16u * 1024 * 1024, fb_ok = fb0 && fb1 > fb0, k, first = 1;

    /* what the writer numbers afresh (lists, comments, notes) and so cannot keep pointing at: made, not kept */
    for (j = 0; ok && j < sizeof(renumbered) / sizeof(renumbered[0]); j++) {
        ok = dw_memmem(raw0, rawn, renumbered[j], strlen(renumbered[j])) == NULL;
    }

    ok = ok && dw_xml_writable(X, raw0, raw1, rel_id, rel_res, &nrel, 0);

    if (!ok) {
        return;
    }

    for (j = 0; fb_ok && j < sizeof(renumbered) / sizeof(renumbered[0]); j++) {
        fb_ok = dw_memmem(fb0, (size_t)(fb1 - fb0), renumbered[j], strlen(renumbered[j])) == NULL;
    }

    fb_ok = fb_ok && dw_xml_writable(X, fb0, fb1, rel_id, rel_res, &nrel, 1);
    pb_printf(o, ",\"kind\":\"%s\",\"theme\":[", canvas ? "wpc" : "wgp");

    for (k = 0; k < 12; k++) {  /* the theme's colours: the drawing made again from its XML has them */
        pb_printf(o, "%s%u", k ? "," : "", (unsigned)X->theme_clr[k]);
    }

    pb_puts(o, "],\"rels\":{");

    for (k = 0; k < nrel; k++) {
        if (k) {
            pb_putc(o, ',');
        }

        json_str(o, rel_id[k], strlen(rel_id[k]));
        pb_printf(o, ":%d", (int)rel_res[k]);
    }

    /* the styles its text names, by Word's id, as the document's: the writer names them its own way */
    pb_puts(o, "},\"styles\":{");

    if (X->rebuild) {   /* made again: the styles it had; not its fallback, the VML of the drawing as it was */
        const pj_node* st = pj_get(X->rebuild, "styles"), *c;

        for (c = st ? st->child : NULL; c; c = c->next) {
            if (!first) {
                pb_putc(o, ',');
            }

            first = 0;
            json_str(o, c->key, c->keylen);
            pb_printf(o, ":%d", (int)pj_int_or(c, 0));
        }

        pb_puts(o, "},\"xml\":");
        json_str(o, raw0, rawn);

        return;
    }

    for (j = 0; j < sizeof(styled) / sizeof(styled[0]); j++) {
        int part;

        for (part = 0; part < (fb_ok ? 2 : 1); part++) {
            const char* p = part ? fb0 : raw0, *end = part ? fb1 : raw1;
            size_t sl = strlen(styled[j]);

            while ((p = dw_memmem(p, (size_t)(end - p), styled[j], sl)) != NULL) {
                const char* v0 = p + sl, *v1 = memchr(v0, '"', (size_t)(end - v0));
                char id[64];
                const dstyle_x* st;
                pd_style_id sid;

                p = v0;

                if (!v1 || (size_t)(v1 - v0) >= sizeof(id)) {
                    continue;
                }

                memcpy(id, v0, (size_t)(v1 - v0));
                id[v1 - v0] = '\0';
                st = find_style(X, id);
                sid = pd_doc_style_find(X->b->d, st && st->name[0] ? st->name : id);

                if (!sid && st && st->name[0] && islower((unsigned char)st->name[0])) {
                    /* Word's built-in names are lower case (caption, heading 2): the document's own, capitalised */
                    char cap[64];

                    snprintf(cap, sizeof(cap), "%s", st->name);
                    cap[0] = (char)toupper((unsigned char)cap[0]);
                    sid = pd_doc_style_find(X->b->d, cap);
                }

                if (!sid && st && st->type == 1) {  /* used in the drawing alone: made now, as the body would */
                    sid = custom_style_define(X, X->b, id, NULL);
                } else if (!sid && st && st->type == 2) {   /* a character style: its properties, under its name */
                    dprops cs;

                    memset(&cs, 0, sizeof(cs));
                    style_chain(X, id, &cs, 0);

                    if (pd_doc_style_define(X->b->d, st->name[0] ? st->name : id, PD_STYLE_CHARACTER, 0, NULL, &cs.cp,
                                            &sid) != PD_OK) {
                        sid = 0;
                    }
                }

                if (sid) {
                    if (!first) {
                        pb_putc(o, ',');
                    }

                    first = 0;
                    json_str(o, id, strlen(id));
                    pb_printf(o, ":%d", (int)sid);
                }
            }
        }
    }

    pb_puts(o, "},\"xml\":");
    json_str(o, raw0, rawn);

    if (fb_ok) {
        pb_puts(o, ",\"fallback\":");
        json_str(o, fb0, (size_t)(fb1 - fb0));
    }
}

/* a preset dash (prstDash) as its lengths, dash, gap, ..., in 100000ths of the line's width (ECMA's patterns);
   how many, 0 for solid */
static int dw_dash_preset(const char* v, int* out) {
    static const struct {
        const char* n;
        int k, d[6];
    } t[] = {
        { "dash", 2, { 4, 3 } }, { "dashDot", 4, { 4, 3, 1, 3 } }, { "dot", 2, { 1, 3 } }, { "lgDash", 2, { 8, 3 } },
        { "lgDashDot", 4, { 8, 3, 1, 3 } }, { "lgDashDotDot", 6, { 8, 3, 1, 3, 1, 3 } }, { "sysDash", 2, { 3, 1 } },
        { "sysDashDot", 4, { 3, 1, 1, 1 } }, { "sysDashDotDot", 6, { 3, 1, 1, 1, 1, 1 } }, { "sysDot", 2, { 1, 1 } }
    };
    int i, k;

    for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
        if (!strcmp(v, t[i].n)) {
            for (k = 0; k < t[i].k; k++) {
                out[k] = t[i].d[k] * 100000;
            }

            return t[i].k;
        }
    }

    return 0;
}

/* an arrowhead's type (headEnd, tailEnd) as the drawing resource numbers it: 1 triangle, 2 stealth, 3 diamond,
   4 oval, 5 an open arrow; 0 none */
static int dw_arrow_type(const char* v) {
    return !strcmp(v, "triangle") ? 1 : !strcmp(v, "stealth") ? 2 : !strcmp(v, "diamond") ? 3 : !strcmp(v, "oval") ?
           4 : !strcmp(v, "arrow") ? 5 : 0;
}

/* A custom geometry as read: each path's commands in its own space (m, l, c, z: arcs and quadratic curves made
   cubics), with its own size, fill and outline, so each is drawn as it says */
#define DW_CG_MAXCMD 2048
#define DW_CG_MAXPATH 16
typedef struct {
    pd_preset_cmd c[DW_CG_MAXCMD];
    int nc;
    struct {
        int start, n;
        double w, h;        /* its coordinate space (0: the shape's own) */
        char fill[16];      /* norm, none, darken, lighten, ... */
        int stroke;
    } path[DW_CG_MAXPATH];
    int np;
    double cx, cy, sx, sy;  /* where the path is, where its ring began */
    double pt[6];           /* a command's points so far */
    int npt;
    char cmd;
} dw_cust;

static void dw_cust_put(dw_cust* C, char op, const double* p, int np) {
    if (C->np > 0 && C->nc < DW_CG_MAXCMD) {
        C->c[C->nc].op = op;
        memcpy(C->c[C->nc].p, p, (size_t)np * sizeof(double));
        C->nc++;
        C->path[C->np - 1].n++;
    }
}

/* path I of a custom geometry as points in its own space, curves as 12 chords each, NAN pairs between its rings;
   how many (at most cap) */
static int dw_cust_flat(const dw_cust* C, int i, double* xy, int cap, int* closed) {
    double cx = 0, cy = 0, sx = 0, sy = 0;
    int k, q, n = 0, ring = 0;

    *closed = 0;

#define CG_PUT(X, Y) do { if (n < cap) { xy[2 * n] = (X); xy[2 * n + 1] = (Y); n++; } } while (0)
    for (k = C->path[i].start; k < C->path[i].start + C->path[i].n; k++) {
        const double* p = C->c[k].p;

        if (C->c[k].op == 'm') {
            if (ring) {
                CG_PUT(NAN, NAN);
            }

            ring = 1;
            sx = cx = p[0];
            sy = cy = p[1];
            CG_PUT(cx, cy);
        } else if (C->c[k].op == 'l') {
            if (!ring) {
                ring = 1;
                CG_PUT(cx, cy);
            }

            cx = p[0];
            cy = p[1];
            CG_PUT(cx, cy);
        } else if (C->c[k].op == 'c') {
            if (!ring) {
                ring = 1;
                CG_PUT(cx, cy);
            }

            for (q = 1; q <= 12; q++) {
                double t = q / 12.0, u = 1 - t;

                CG_PUT(u * u * u * cx + 3 * u * u * t * p[0] + 3 * u * t * t * p[2] + t * t * t * p[4],
                       u * u * u * cy + 3 * u * u * t * p[1] + 3 * u * t * t * p[3] + t * t * t * p[5]);
            }

            cx = p[4];
            cy = p[5];
        } else if (C->c[k].op == 'z') {
            *closed = 1;
            cx = sx;
            cy = sy;
        }
    }
#undef CG_PUT

    return n;
}

/* Path commands (m, l, c, z) as a drawing resource's curve, ",\"cv\":[...]": the points mapped by sm from the box
   at (bx0, by0), scaled gx, gy; a PD_PATH_BREAK pair between rings and a PD_PATH_CUBIC pair before each cubic's
   three points */
static void dw_put_cv(pd_buf* o, const pd_preset_cmd* c, int n, const dxform* sm, double bx0, double by0, double gx,
                      double gy) {
    double cx = 0, cy = 0, sx = 0, sy = 0, qx, qy;
    int k, q, ring = 0, first = 1;

#define CV_PT(X, Y) do { fr_pt(sm, bx0 + (X) * gx, by0 + (Y) * gy, &qx, &qy); \
                         pb_printf(o, "%s%d,%d", first ? "" : ",", (int)emu_sp(qx), (int)emu_sp(qy)); first = 0; } while (0)
    pb_puts(o, ",\"cv\":[");

    for (k = 0; k < n; k++) {
        if (c[k].op == 'm') {
            if (ring) {
                pb_printf(o, "%s%d,%d", first ? "" : ",", (int)INT32_MIN, (int)INT32_MIN);
                first = 0;
            }

            ring = 1;
            sx = cx = c[k].p[0];
            sy = cy = c[k].p[1];
            CV_PT(cx, cy);
        } else if (c[k].op == 'l' || c[k].op == 'c') {
            if (!ring) {    /* drawing on from a closed ring: a new one from where that began */
                if (!first) {
                    pb_printf(o, ",%d,%d", (int)INT32_MIN, (int)INT32_MIN);
                }

                ring = 1;
                CV_PT(cx, cy);
            }

            if (c[k].op == 'c') {
                pb_printf(o, ",%d,0", (int)INT32_MIN + 1);

                for (q = 0; q < 3; q++) {
                    CV_PT(c[k].p[2 * q], c[k].p[2 * q + 1]);
                }

                cx = c[k].p[4];
                cy = c[k].p[5];
            } else {
                cx = c[k].p[0];
                cy = c[k].p[1];
                CV_PT(cx, cy);
            }
        } else if (c[k].op == 'z') {
            cx = sx;
            cy = sy;
        }
    }
#undef CV_PT

    pb_putc(o, ']');
}

/* whether path commands have a curve among them */
static int dw_has_curve(const pd_preset_cmd* c, int n) {
    int k;

    for (k = 0; k < n; k++) {
        if (c[k].op == 'c') {
            return 1;
        }
    }

    return 0;
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
    int arrow_w[2] = { 2, 2 }, arrow_len[2] = { 2, 2 };  /* head, tail: 1 sm, 2 med, 3 lg */
    int dash[8], ndash = 0, lcap = 0, ljoin = 0, lcmpd = 0;   /* the outline's dashes, ends, corners, lines */
    long long ext_h = 0, adj1 = -1;                     /* extrusion depth; a preset's first adjustment */
    char prst[32] = "";                                 /* the shape's preset, for one whose corners its adjustments move */
    uint32_t ext_clr = 0, patt_fg = 0, patt_bg = 0, cv_bg = 0, cv_line = 0;
    int in_extclr = 0, in_patt = 0, patt_part = 0, patt_pct = 50, in_cvbg = 0, in_cvwhole = 0;
    long long cv_lw = 9525;
    char conn[24] = "";                                 /* a connector's preset: its path, not a straight line */
    uint32_t gfill[17], gfill_new = 0, *gclr = NULL;     /* each group's fill, for a shape's grpFill */
    int g_in_ln = 0;
    dw_guides gds;
    const char* raw0;                                   /* the group's own XML, from its opening tag */
    char rel_id[64][24];                                /* the pictures it names, by relationship id */
    pd_res_id rel_res[64];
    int nrel = 0, rel_ok = 1;
    const char* tx_a = NULL, *tx_b = NULL;              /* a text box's w:txbxContent: its inside, read into a story */
    int tx_depth = 0;
    int sid = -1;                                       /* the shape being read: its place among the group's shapes */
    double cg_xy[4096];             /* a polygon's points (its own space), NAN pairs between rings */
    long long cg_w = 0, cg_h = 0;
    int cg_n = 0, cg_closed = 0, k2;
    char blip[64] = "", geom[32] = "rect", v[300], anchor[8] = "t";
    int crop[4] = { 0, 0, 0, 0 };   /* a picture's srcRect: its sides cut off, in 100000ths */
    dw_cust* cg = (dw_cust*)calloc(1, sizeof(dw_cust));     /* a custom geometry's paths */
    uint32_t fill2 = 0, shd_clr = 0;    /* a gradient's last stop; a shadow's colour */
    int grad_kind = 0, grad_ang = 0, in_grad = 0, in_shd = 0, has_shd = 0;
    int ngst = 0, gst_pos[PD_MAX_STOPS], grad_sc = 0, grad_path = 1, grad_rws = 1, gft[4] = { 50000, 50000, 50000, 50000 };
    uint32_t gst_clr[PD_MAX_STOPS];     /* a gradient's stops, as listed; scaled; its path; turned with the shape */
    unsigned char patt_bits[8];         /* a pattern fill's bitmap, if it is one Office has (have_patt) */
    int in_bfill = 0, bf_tile = 0, bf_rect[4] = { 0, 0, 0, 0 }, bf_tl[6] = { 0, 0, 100000, 100000, 0, 0 };
    char bf_blip[64] = "";              /* a shape's picture fill: stretched into its fillRect, or tiled */
    int have_patt = 0;
    double shd_dist = 0, shd_dir = 0;
    double shd_blur = 0, shd_k[4] = { 100000, 100000, 0, 0 };     /* a shadow's blur; its sx, sy, kx, ky */
    int shd_algn = 7, in_ish = 0, has_ish = 0, in_glow = 0, soft = 0, has_refl = 0;   /* (algn: tl..br 0..8) */
    double ish_blur = 0, ish_dist = 0, ish_dir = 0, glow_rad = 0, refl[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t ish_clr = 0, glow_clr = 0;     /* inner shadow, glow; reflection: blur, stA, endA, stPos, endPos, dist */
    char gprst[32] = "";            /* a picture's geometry, when it is not a rectangle (it is cut to that shape) */
    uint32_t fill = 0, line = 0, sfill = 0, sline = 0, *cur_clr = NULL;
    char cam_prst[48] = "";         /* the shape's 3-D camera (scene3d), seen in parallel */
    int cam_has_rot = 0, in_camera = 0;
    double cam_lat = 0, cam_lon = 0, cam_rev = 0;
    long long lw = 9525, ins[4] = { 91440, 45720, 91440, 45720 };
    int tx_rot = 0, tx_upright = 0, tx_vert = 0;   /* its text's bodyPr: turned further, kept upright, vertical, */
    int tx_actr = 0;                                /* its lines' block centred across */
    pd_char_props base, pbase, rcp;     /* the text boxes' Normal, the paragraph's style's, the run's own */
    pd_buf text;
    int jc = PD_ALIGN_LEFT, def_jc = PD_ALIGN_LEFT;
    int tb_depth = 0, tb_pr = 0, tb_tcpr = 0, tb_head = 0, tb_row0 = 1, tb_cell0 = 1, tb_jc = PD_ALIGN_LEFT;
    int tb_span = 1, tb_rule = 0, tb_grid = 0;
    int in_inl = 0;                 /* a picture in a text box's paragraph: its size and image */
    long long inl_cx = 0, inl_cy = 0;
    char inl_blip[64] = "";   /* a text box's table: one level of it, its cells' paragraphs among the text's */
    uint32_t tb_bg = 0;

    memset(&xf, 0, sizeof(xf));
    memset(gfill, 0, sizeof(gfill));

    /* where the group's opening tag began: its XML is kept whole, for a .docx written back as it was */
    raw0 = m->s + (m->pos > 0 ? m->pos - 1 : 0);    /* the last character of the opening tag, its '>' */

    while (raw0 > m->s && *raw0 != '<') {
        raw0--;
    }
    memset(&gds, 0, sizeof(gds));
    memset(&o, 0, sizeof(o));
    memset(&text, 0, sizeof(text));
    fr[0].ox = fr[0].oy = 0;
    fr[0].sx = fr[0].sy = 1;
    fr[0].a = fr[0].d = 1;
    fr[0].b = fr[0].c = 0;

    /* the text boxes' default characters: Normal's, which is the document's default paragraph style by now */
    pd_doc_style_resolve(X->b->d, pd_doc_style_find(X->b->d, "Normal"), NULL, &base);
    pbase = base;

    {   /* the default paragraph style's alignment: a text box's paragraph without its own has it (justified,
           in many a document) */
        dprops dp;

        memset(&dp, 0, sizeof(dp));
        style_chain(X, X->def_pstyle, &dp, 0);

        if (dp.pp.mask & PD_PP_ALIGN) {
            def_jc = dp.pp.align;
        }
    }

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
                    pd_char_props c = pbase;
                    dprops a, b;

                    memset(&a, 0, sizeof(a));
                    memset(&b, 0, sizeof(b));
                    a.cp = pbase;
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

        if (in_txbx && (in_inl || (!strcmp(t, "drawing") && g.type == MT_OPEN))) {     /* not the group's own shapes */
            if (!strcmp(t, "drawing")) {
                if (g.type == MT_OPEN && !in_inl++) {
                    inl_cx = inl_cy = 0;
                    inl_blip[0] = '\0';
                } else if (g.type == MT_CLOSE && !--in_inl && inl_blip[0] && inl_cx > 0 && inl_cy > 0) {
                    pd_res_id r = dw_resource(X, inl_blip);

                    if (r && nrel < 64) {   /* by its id: the XML kept names it so */
                        snprintf(rel_id[nrel], sizeof(rel_id[0]), "%.23s", inl_blip);
                        rel_res[nrel++] = r;
                    } else if (r) {
                        rel_ok = 0;
                    }

                    if (r) {
                        if (!para_open) {
                            pb_printf(&text, "%s{\"a\":%d,\"runs\":[", nopara ? "" : ",", jc);
                            para_open = 1;
                            nopara = 0;
                        } else if (text.n && text.p[text.n - 1] != '[') {
                            pb_putc(&text, ',');
                        }

                        pb_printf(&text, "{\"img\":%d,\"w\":%d,\"h\":%d}", (int)r, (int)emu_sp((double)inl_cx),
                                  (int)emu_sp((double)inl_cy));
                    }
                }
            } else if (!strcmp(t, "extent") && open && !inl_cx) {
                inl_cx = mu_attr(&g, "cx", v, sizeof(v)) ? atoll(v) : 0;
                inl_cy = mu_attr(&g, "cy", v, sizeof(v)) ? atoll(v) : 0;
            } else if (!strcmp(t, "blip") && open && !inl_blip[0]) {
                mu_attr(&g, "r:embed", inl_blip, sizeof(inl_blip));
            }

            continue;
        }

        /* groups: their xfrm maps their children's coordinates into their parent's */
        if (!strcmp(t, "grpSpPr")) {
            if (g.type == MT_OPEN) {
                in_grpsppr = 1;
                memset(&xf, 0, sizeof(xf));
                gfill_new = gfill[nfr - 1];     /* a group with no fill of its own passes its parent's on */
                gclr = NULL;
                g_in_ln = 0;
            } else if (g.type == MT_CLOSE) {
                double sx = xf.chext[0] > 0 ? (double)xf.ext[0] / xf.chext[0] : 1;
                double sy = xf.chext[1] > 0 ? (double)xf.ext[1] / xf.chext[1] : 1;

                in_grpsppr = 0;

                if (root_frame) {   /* the root group: its child space onto the drawing's extent */
                    sx = xf.chext[0] > 0 ? (double)w->cx / xf.chext[0] : 1;
                    sy = xf.chext[1] > 0 ? (double)w->cy / xf.chext[1] : 1;
                    fr[0].sx = sx;
                    fr[0].sy = sy;
                    fr[0].a = sx;
                    fr[0].d = sy;
                    fr[0].b = fr[0].c = 0;
                    fr[0].ox = -xf.choff[0] * sx;
                    fr[0].oy = -xf.choff[1] * sy;
                    root_frame = 0;
                    gfill[0] = gfill_new;
                } else if (nfr < 17) {
                    /* a nested group: off + (child - chOff) * ext / chExt, then turned and flipped about its box's
                       centre, in its parent */
                    dxform gmap, box, both;

                    memset(&gmap, 0, sizeof(gmap));
                    gmap.a = sx;
                    gmap.d = sy;
                    gmap.ox = xf.off[0] - xf.choff[0] * sx;
                    gmap.oy = xf.off[1] - xf.choff[1] * sy;
                    box = fr_box(xf.off[0] + xf.ext[0] / 2.0, xf.off[1] + xf.ext[1] / 2.0, xf.rot, xf.fliph, xf.flipv,
                                 NULL);
                    both = fr_then(&box, &gmap);
                    fr[nfr] = fr_then(&fr[nfr - 1], &both);
                    gfill[nfr] = gfill_new;
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
            uint32_t c;

            if (!strcmp(t, "ln")) {
                g_in_ln = g.type == MT_OPEN;
            } else if (g_in_ln) {
                /* the group's outline: not a fill */
            } else if (open && !strcmp(t, "noFill")) {
                gfill_new = 0;
            } else if (open && dw_color(X, &g, t, &c)) {
                if (gclr == NULL) {     /* the first colour: a solid fill's, a gradient's first stop */
                    gfill_new = c;
                    gclr = g.type == MT_OPEN ? &gfill_new : NULL;
                }
            } else if (gclr && open && strcmp(t, "gs")) {
                *gclr = pd_conv_clr_modify(*gclr, t, &g);
            } else if (!open && (!strcmp(t, "srgbClr") || !strcmp(t, "schemeClr") || !strcmp(t, "sysClr") ||
                                 !strcmp(t, "prstClr"))) {
                gclr = gclr == &gfill_new ? (uint32_t*)1 : gclr;   /* done with its modifiers */
            }

            if (open) {
                xfrm_attr(&g, t, &xf);
            }

            continue;
        }

        /* a picture or a shape: read to its end, then written */
        if ((!strcmp(t, "pic") || !strcmp(t, "wsp")) && g.type == MT_OPEN) {
            kind = !strcmp(t, "pic") ? 1 : 2;
            sid++;      /* the n-th wsp or pic of the group's XML, in order */
            memset(&xf, 0, sizeof(xf));
            cam_prst[0] = '\0';
            cam_has_rot = in_camera = 0;
            blip[0] = '\0';
            memset(crop, 0, sizeof(crop));
            fill2 = shd_clr = 0;
            grad_kind = grad_ang = in_grad = in_shd = has_shd = 0;
            shd_blur = 0;
            shd_algn = 7;
            in_ish = has_ish = in_glow = soft = has_refl = 0;
            glow_rad = 0;
            ngst = grad_sc = have_patt = 0;
            in_bfill = bf_tile = 0;
            bf_blip[0] = '\0';
            memset(bf_rect, 0, sizeof(bf_rect));
            grad_path = grad_rws = 1;
            gft[0] = gft[1] = gft[2] = gft[3] = 50000;
            gprst[0] = '\0';
            strcpy(geom, "rect");
            have_fill = have_line = style_fill = style_line = 0;
            head_arrow = tail_arrow = 0;
            tx_a = tx_b = NULL;
            tx_depth = 0;
            arrow_w[0] = arrow_w[1] = arrow_len[0] = arrow_len[1] = 2;
            ndash = lcap = ljoin = lcmpd = 0;
            ext_h = 0;
            ext_clr = 0;
            adj1 = -1;
            prst[0] = '\0';
            gds.n = 0;
            conn[0] = '\0';
            cg_n = cg_closed = 0;
            cg_w = cg_h = 0;

            if (cg) {
                cg->np = cg->nc = 0;
            }
            fill = line = sfill = sline = 0;
            tx_rot = tx_upright = tx_vert = tx_actr = 0;
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
            /* the shape's own map: its box turned, flipped and seen through its camera, in its group's */
            double lin[9], bx0 = (double)xf.off[0], by0 = (double)xf.off[1], bw = (double)xf.ext[0], bh = (double)xf.ext[1];
            int cam = camera_lin(cam_prst[0] ? cam_prst : NULL, cam_has_rot, cam_lat, cam_lon, cam_rev, lin);
            dxform box = fr_box(bx0 + bw / 2, by0 + bh / 2, xf.rot, xf.fliph, xf.flipv, cam ? lin : NULL);
            dxform sm = fr_then(FR, &box);
            int straight = fabs(sm.b) < 1e-9 && fabs(sm.c) < 1e-9 && sm.a > 0 && sm.d > 0;
            double x, y, cw, ch, mx, my;

            {   /* the shape, for a selection: which it is, its box as drawn, and its group's scale (EMU a unit) */
                double cx4[4] = { bx0, bx0 + bw, bx0 + bw, bx0 }, cy4[4] = { by0, by0, by0 + bh, by0 + bh };
                double qx, qy, x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
                int c4;

                for (c4 = 0; c4 < 4; c4++) {
                    fr_pt(&sm, cx4[c4], cy4[c4], &qx, &qy);
                    x0 = qx < x0 ? qx : x0;
                    y0 = qy < y0 ? qy : y0;
                    x1 = qx > x1 ? qx : x1;
                    y1 = qy > y1 ? qy : y1;
                }

                ITEM_SEP();
                pb_printf(&o, "{\"sid\":%d,\"box\":[%d,%d,%d,%d],\"fs\":[%d,%d]}", sid, (int)emu_sp(x0), (int)emu_sp(y0),
                          (int)emu_sp(x1), (int)emu_sp(y1), (int)(FR->sx * 1000000), (int)(FR->sy * 1000000));
            }

            /* where an unturnable box (a picture, a text box) goes: its centre mapped, its size scaled */
            fr_pt(&sm, bx0 + bw / 2, by0 + bh / 2, &mx, &my);
            cw = FR->sx * bw;
            ch = FR->sy * bh;
            x = mx - cw / 2;
            y = my - ch / 2;

            if (tx_a && kind == 2 && prst[0] && bw > 0 && bh > 0) {
                /* text in a shape: in Office's text rectangle of it (an ellipse's, a callout's body) */
                char adjs[512];
                size_t an = 0;
                double tl, tt, tr, tb;
                int q;

                adjs[0] = '\0';

                for (q = 0; q < gds.n && an < sizeof(adjs) - 64; q++) {
                    if (!strncmp(gds.name[q], "adj", 3)) {
                        an += (size_t)snprintf(adjs + an, sizeof(adjs) - an, "%s%s=%.0f", an ? " " : "", gds.name[q],
                                               gds.val[q]);
                    }
                }

                if (pd_preset_text_rect(prst, bw, bh, adjs, &tl, &tt, &tr, &tb)) {
                    x += FR->sx * (xf.fliph ? bw - tr : tl);
                    y += FR->sy * (xf.flipv ? bh - tb : tt);
                    cw = FR->sx * (tr - tl);
                    ch = FR->sy * (tb - tt);
                }
            }

            if (para_open) {
                pb_puts(&text, "]}");
                para_open = 0;
            }

            if (kind == 1 && blip[0]) {
                pd_res_id r = dw_resource(X, blip);

                    int seen = 0, q;

                    for (q = 0; q < nrel; q++) {    /* a picture used twice: its id once */
                        seen |= !strcmp(rel_id[q], blip);
                    }

                    if (r && !seen && nrel < 64) {   /* by its id: the XML kept names it so */
                        snprintf(rel_id[nrel], sizeof(rel_id[0]), "%.23s", blip);
                        rel_res[nrel++] = r;
                    } else if (r && !seen) {
                        rel_ok = 0;
                    }

                if (r) {
                    ITEM_SEP();
                    pb_printf(&o, "{\"img\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d", (int)r, (int)emu_sp(x), (int)emu_sp(y),
                              (int)emu_sp(cw), (int)emu_sp(ch));

                    if (crop[0] || crop[1] || crop[2] || crop[3]) {
                        pb_printf(&o, ",\"crop\":[%d,%d,%d,%d]", crop[0], crop[1], crop[2], crop[3]);
                    }

                    if (xf.rot || xf.fliph || xf.flipv) {   /* turned, flipped about its middle */
                        pb_printf(&o, ",\"rot\":%d,\"fh\":%d,\"fv\":%d", xf.rot, xf.fliph, xf.flipv);
                    }

                    if (gprst[0] && bw > 0 && bh > 0) {     /* cut to its shape (a photo in a circle) */
                        pd_preset_flat* cf = (pd_preset_flat*)malloc(sizeof(pd_preset_flat));

                        if (cf && pd_preset_flatten(gprst, bw, bh, "", cf) && cf->path[0].n >= 3) {
                            int cq;

                            pb_puts(&o, ",\"clip\":[");

                            for (cq = 0; cq < cf->path[0].n; cq++) {
                                const double* cxy = &cf->xy[2 * (cf->path[0].start + cq)];
                                double qx, qy;

                                if (isnan(cxy[0])) {
                                    break;
                                }

                                fr_pt(&sm, bx0 + cxy[0], by0 + cxy[1], &qx, &qy);
                                pb_printf(&o, "%s%d,%d", cq ? "," : "", (int)emu_sp(qx), (int)emu_sp(qy));
                            }

                            pb_putc(&o, ']');
                        }

                        free(cf);
                    }

                    pb_putc(&o, '}');
                }
            } else if (kind == 2) {
                uint32_t f = have_fill ? fill : style_fill ? sfill : 0, l = have_line ? line : style_line ? sline : 0;
                char fx[640] = "";  /* the fill's gradient and shadow, for the items that fill */
                char lx[160], ax[64];   /* the outline's dashes, ends, corners and lines; its arrowheads */

                if (grad_kind && fill2 && have_fill) {
                    size_t fl;
                    int q;

                    snprintf(fx, sizeof(fx), ",\"gk\":%d,\"f2\":%u,\"ga\":%d", grad_kind, (unsigned)fill2,
                             grad_ang + (grad_rws ? xf.rot : 0));

                    if (ngst >= 2) {    /* every stop, where it is, with its own alpha */
                        fl = strlen(fx);
                        fl += (size_t)snprintf(fx + fl, sizeof(fx) - fl, ",\"gsc\":%d,\"gp\":%d,\"gft\":[%d,%d,%d,%d],\"gs\":[",
                                               grad_sc, grad_path, gft[0], gft[1], gft[2], gft[3]);

                        for (q = 0; q < ngst && fl < sizeof(fx); q++) {
                            fl += (size_t)snprintf(fx + fl, sizeof(fx) - fl, "%s%d,%u", q ? "," : "", gst_pos[q],
                                                   (unsigned)gst_clr[q]);
                        }

                        if (fl < sizeof(fx)) {
                            snprintf(fx + fl, sizeof(fx) - fl, "]");
                        }
                    }
                }

                {   /* the outline's style, for the items it outlines; arrowheads for the open ones */
                    size_t ll = 0;
                    int q;

                    lx[0] = ax[0] = '\0';

                    if (ndash) {
                        ll += (size_t)snprintf(lx + ll, sizeof(lx) - ll, ",\"dash\":[");

                        for (q = 0; q < ndash; q++) {
                            ll += (size_t)snprintf(lx + ll, sizeof(lx) - ll, "%s%d", q ? "," : "", dash[q]);
                        }

                        ll += (size_t)snprintf(lx + ll, sizeof(lx) - ll, "]");
                    }

                    if (lcap) {
                        ll += (size_t)snprintf(lx + ll, sizeof(lx) - ll, ",\"cap\":%d", lcap);
                    }

                    if (ljoin) {
                        ll += (size_t)snprintf(lx + ll, sizeof(lx) - ll, ",\"join\":%d", ljoin);
                    }

                    if (lcmpd) {
                        snprintf(lx + ll, sizeof(lx) - ll, ",\"cmpd\":%d", lcmpd);
                    }

                    if (head_arrow || tail_arrow) {
                        snprintf(ax, sizeof(ax), ",\"he\":[%d,%d,%d],\"te\":[%d,%d,%d]", head_arrow, arrow_w[0],
                                 arrow_len[0], tail_arrow, arrow_w[1], arrow_len[1]);
                    }
                }

                if (bf_blip[0] && have_fill && fill == 0x01FFFFFFu) {   /* a picture fill: the picture, cut to the shape */
                    pd_res_id r = dw_resource(X, bf_blip);
                    size_t fl = strlen(fx);
                    int seen = 0, q;

                    for (q = 0; q < nrel; q++) {
                        seen |= !strcmp(rel_id[q], bf_blip);
                    }

                    if (r && !seen && nrel < 64) {  /* by its id: the XML kept names it so */
                        snprintf(rel_id[nrel], sizeof(rel_id[0]), "%.23s", bf_blip);
                        rel_res[nrel++] = r;
                    } else if (r && !seen) {
                        rel_ok = 0;
                    }

                    if (r && bf_tile) {
                        snprintf(fx + fl, sizeof(fx) - fl, ",\"bimg\":%d,\"btile\":[%d,%d,%d,%d,%d,%d],\"bcrop\":[%d,%d,%d,%d]",
                                 (int)r, (int)emu_sp(bf_tl[0] * FR->sx), (int)emu_sp(bf_tl[1] * FR->sy), bf_tl[2], bf_tl[3],
                                 bf_tl[4], bf_tl[5], crop[0], crop[1], crop[2], crop[3]);
                    } else if (r) {
                        snprintf(fx + fl, sizeof(fx) - fl, ",\"bimg\":%d,\"bfr\":[%d,%d,%d,%d],\"bcrop\":[%d,%d,%d,%d]",
                                 (int)r, bf_rect[0], bf_rect[1], bf_rect[2], bf_rect[3], crop[0], crop[1], crop[2], crop[3]);
                    }
                }

                if (have_patt && have_fill && !grad_kind) {     /* the pattern itself, its colours mixed in fill */
                    size_t fl = strlen(fx);

                    snprintf(fx + fl, sizeof(fx) - fl, ",\"patt\":[%d,%d,%d,%d,%d,%d,%d,%d],\"pfg\":%u,\"pbg\":%u",
                             patt_bits[0], patt_bits[1], patt_bits[2], patt_bits[3], patt_bits[4], patt_bits[5],
                             patt_bits[6], patt_bits[7], (unsigned)patt_fg, (unsigned)patt_bg);
                }

                if (has_shd && shd_dist > 0) {
                    size_t fl = strlen(fx);

                    snprintf(fx + fl, sizeof(fx) - fl, ",\"shd\":[%d,%d,%u]",
                             (int)emu_sp(shd_dist * cos(shd_dir * 3.14159265358979 / 180) * FR->sx),
                             (int)emu_sp(shd_dist * sin(shd_dir * 3.14159265358979 / 180) * FR->sy), (unsigned)shd_clr);
                }

                {   /* effects: a blurred, scaled, skewed shadow; an inner one; a glow; a soft edge; a reflection */
                    size_t fl = strlen(fx);
                    double fs = (FR->sx + FR->sy) / 2;

                    if (has_shd && (shd_blur > 0 || shd_k[0] != 100000 || shd_k[1] != 100000 || shd_k[2] || shd_k[3])) {
                        fl += (size_t)snprintf(fx + fl, sizeof(fx) - fl, ",\"osh\":[%d,%d,%d,%u,%d,%d,%d,%d,%d]",
                                               (int)emu_sp(shd_blur * fs),
                                               (int)emu_sp(shd_dist * cos(shd_dir * 3.14159265358979 / 180) * FR->sx),
                                               (int)emu_sp(shd_dist * sin(shd_dir * 3.14159265358979 / 180) * FR->sy),
                                               (unsigned)shd_clr, (int)shd_k[0], (int)shd_k[1], (int)shd_k[2], (int)shd_k[3],
                                               shd_algn);
                    }

                    if (has_ish && fl < sizeof(fx)) {
                        fl += (size_t)snprintf(fx + fl, sizeof(fx) - fl, ",\"ish\":[%d,%d,%d,%u]", (int)emu_sp(ish_blur * fs),
                                               (int)emu_sp(ish_dist * cos(ish_dir * 3.14159265358979 / 180) * FR->sx),
                                               (int)emu_sp(ish_dist * sin(ish_dir * 3.14159265358979 / 180) * FR->sy),
                                               (unsigned)ish_clr);
                    }

                    if (glow_rad > 0 && fl < sizeof(fx)) {
                        fl += (size_t)snprintf(fx + fl, sizeof(fx) - fl, ",\"glow\":[%d,%u]", (int)emu_sp(glow_rad * fs),
                                               (unsigned)glow_clr);
                    }

                    if (soft > 0 && fl < sizeof(fx)) {
                        fl += (size_t)snprintf(fx + fl, sizeof(fx) - fl, ",\"soft\":%d", (int)emu_sp(soft * fs));
                    }

                    if (has_refl && refl[1] > 0 && fl < sizeof(fx)) {
                        snprintf(fx + fl, sizeof(fx) - fl, ",\"refl\":[%d,%d,%d,%d,%d,%d]", (int)emu_sp(refl[0] * fs),
                                 (int)refl[1], (int)refl[2], (int)refl[3], (int)refl[4], (int)emu_sp(refl[5] * fs));
                    }
                }
                int isline = !strcmp(geom, "line"), k;
                double lwd = lw;    /* a line as wide as it says: a group's scale is not its own (Office does not scale it) */
                pd_preset_flat* pf = NULL;

                if (prst[0] && dw_generic_preset(prst) && bw > 0) {
                    /* Office's own definition, at the shape's size with the adjustments it gives */
                    char adjs[512];
                    size_t an = 0;
                    int q;

                    adjs[0] = '\0';

                    for (q = 0; q < gds.n && an < sizeof(adjs) - 64; q++) {
                        if (!strncmp(gds.name[q], "adj", 3)) {
                            an += (size_t)snprintf(adjs + an, sizeof(adjs) - an, "%s%s=%.0f", an ? " " : "",
                                                   gds.name[q], gds.val[q]);
                        }
                    }

                    pf = (pd_preset_flat*)malloc(sizeof(pd_preset_flat));

                    if (pf && pd_preset_flatten(prst, bw, bh, adjs, pf)) {
                        /* its first path the prism's front face, if it is extruded */
                        strcpy(geom, "cust");
                        cg_w = (long long)bw;
                        cg_h = (long long)(bh > 0 ? bh : 1);
                        cg_n = 0;

                        for (q = 0; q < pf->path[0].n && cg_n < 2047; q++, cg_n++) {
                            cg_xy[2 * cg_n] = pf->xy[2 * (pf->path[0].start + q)];
                            cg_xy[2 * cg_n + 1] = pf->xy[2 * (pf->path[0].start + q) + 1];
                        }

                        cg_closed = pf->path[0].closed;
                    } else {
                        free(pf);
                        pf = NULL;
                    }
                }

                if (cam && ext_h > 0 && (f || l) && !isline) {
                    /* extruded and seen through a camera: a prism, its outline in the box as the front face */
                    double px[64], py[64], gx = cg_w > 0 ? bw / cg_w : 1, gy = cg_h > 0 ? bh / cg_h : 1;
                    double fs = (FR->sx + FR->sy) / 2;
                    int np = 0;

                    if (!strcmp(geom, "cust") && cg_n >= 3) {
                        int n0 = 0, step;

                        while (n0 < cg_n && !isnan(cg_xy[2 * n0])) {
                            n0++;   /* the first ring */
                        }

                        step = (n0 + 63) / 64;

                        for (k = 0; k < n0 && np < 64; k += step) {
                            px[np] = bx0 + cg_xy[2 * k] * gx;
                            py[np++] = by0 + cg_xy[2 * k + 1] * gy;
                        }
                    } else if (!strcmp(geom, "ellipse")) {
                        for (k = 0; k < 48; k++) {
                            px[np] = bx0 + bw / 2 + bw / 2 * cos(k * 6.2831853 / 48);
                            py[np++] = by0 + bh / 2 + bh / 2 * sin(k * 6.2831853 / 48);
                        }
                    } else if (!strcmp(geom, "roundRect")) {
                        double r = (bw < bh ? bw : bh) * (adj1 >= 0 ? adj1 : 16667) / 100000.0;
                        static const double cxs[4] = { 1, 1, 0, 0 }, cys[4] = { 0, 1, 1, 0 };
                        int c, q;

                        for (c = 0; c < 4; c++) {   /* each corner a quarter circle, clockwise from the top right */
                            double ccx = bx0 + (cxs[c] ? bw - r : r), ccy = by0 + (cys[c] ? bh - r : r);

                            for (q = 0; q <= 6; q++) {
                                double a = (c * 90 - 90 + q * 15) * 3.14159265358979 / 180;

                                px[np] = ccx + r * cos(a);
                                py[np++] = ccy + r * sin(a);
                            }
                        }
                    } else {
                        px[0] = bx0; py[0] = by0;
                        px[1] = bx0 + bw; py[1] = by0;
                        px[2] = bx0 + bw; py[2] = by0 + bh;
                        px[3] = bx0; py[3] = by0 + bh;
                        np = 4;
                    }

                    dw_extruded(&o, &first_item, FR, lin, bx0 + bw / 2, by0 + bh / 2, xf.rot, xf.fliph, xf.flipv, px, py,
                                np, fs > 0 ? ext_h / fs : (double)ext_h, f, ext_clr ? ext_clr : f, l, lwd);
                    f = l = 0;
                }

                if (pf && (f || l)) {   /* each of the preset's paths: filled (or shaded) and outlined as it says */
                    int i;

                    for (i = 0; i < pf->npath; i++) {
                        uint32_t pfl = !f || !strcmp(pf->path[i].fill, "none") ? 0 : dw_shade(f, pf->path[i].fill);
                        uint32_t pln = pf->path[i].stroke ? l : 0;
                        double qx, qy;

                        if ((!pfl && !pln) || pf->path[i].n < 2) {
                            continue;
                        }

                        ITEM_SEP();
                        pb_puts(&o, "{\"path\":[");

                        for (k = 0; k < pf->path[i].n; k++) {
                            const double* xy = &pf->xy[2 * (pf->path[i].start + k)];

                            if (isnan(xy[0])) {
                                pb_printf(&o, "%s%d,%d", k ? "," : "", (int)INT32_MIN, (int)INT32_MIN);
                            } else {
                                fr_pt(&sm, bx0 + xy[0], by0 + xy[1], &qx, &qy);
                                pb_printf(&o, "%s%d,%d", k ? "," : "", (int)emu_sp(qx), (int)emu_sp(qy));
                            }
                        }

                        pb_printf(&o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d%s%s%s", pf->path[i].closed && pfl,
                                  (unsigned)pfl, (unsigned)pln, (int)emu_sp(lwd),
                                  pfl && !strcmp(pf->path[i].fill, "norm") ? fx : "", pln ? lx : "",
                                  pln && !pf->path[i].closed ? ax : "");

                        if (!cam && dw_has_curve(pf->c + pf->path[i].cstart, pf->path[i].cn)) {
                            dw_put_cv(&o, pf->c + pf->path[i].cstart, pf->path[i].cn, &sm, bx0, by0, 1, 1);
                        }

                        pb_putc(&o, '}');
                    }

                    f = l = 0;
                }

                free(pf);
                pf = NULL;

                if (!strcmp(geom, "cust") && cg && cg->np > 0 && (f || l)) {
                    /* custom geometry: each path in the box, in its own space, filled and outlined as it says */
                    int i;

                    for (i = 0; i < cg->np; i++) {
                        double gx = cg->path[i].w > 0 ? bw / cg->path[i].w : 1, gy = cg->path[i].h > 0 ?
                                    bh / cg->path[i].h : 1, qx, qy;
                        uint32_t pfl = !f || !strcmp(cg->path[i].fill, "none") ? 0 : dw_shade(f, cg->path[i].fill);
                        uint32_t pln = cg->path[i].stroke ? l : 0;
                        int np, closed;

                        np = dw_cust_flat(cg, i, cg_xy, 2048, &closed);

                        if ((!pfl && !pln) || np < 2) {
                            continue;
                        }

                        ITEM_SEP();
                        pb_puts(&o, "{\"path\":[");

                        for (k = 0; k < np; k++) {
                            if (isnan(cg_xy[2 * k])) {
                                pb_printf(&o, "%s%d,%d", k ? "," : "", (int)INT32_MIN, (int)INT32_MIN);
                            } else {
                                fr_pt(&sm, bx0 + cg_xy[2 * k] * gx, by0 + cg_xy[2 * k + 1] * gy, &qx, &qy);
                                pb_printf(&o, "%s%d,%d", k ? "," : "", (int)emu_sp(qx), (int)emu_sp(qy));
                            }
                        }

                        pb_printf(&o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d%s%s%s", closed, (unsigned)pfl,
                                  (unsigned)pln, (int)emu_sp(lwd), pfl && !strcmp(cg->path[i].fill, "norm") ? fx : "",
                                  pln ? lx : "", pln && !closed ? ax : "");

                        if (!cam && dw_has_curve(cg->c + cg->path[i].start, cg->path[i].n)) {  /* its curves as such */
                            dw_put_cv(&o, cg->c + cg->path[i].start, cg->path[i].n, &sm, bx0, by0, gx, gy);
                        }

                        pb_putc(&o, '}');
                    }

                    f = l = 0;
                }

                if (!strcmp(geom, "cust") && cg_n >= 2 && (f || l)) {     /* a polygon preset: its corners in the box */
                    double gx = cg_w > 0 ? bw / cg_w : 1, gy = cg_h > 0 ? bh / cg_h : 1, qx, qy;

                    ITEM_SEP();
                    pb_puts(&o, "{\"path\":[");

                    for (k = 0; k < cg_n; k++) {
                        if (isnan(cg_xy[2 * k])) {
                            pb_printf(&o, "%s%d,%d", k ? "," : "", (int)INT32_MIN, (int)INT32_MIN);
                        } else {
                            fr_pt(&sm, bx0 + cg_xy[2 * k] * gx, by0 + cg_xy[2 * k + 1] * gy, &qx, &qy);
                            pb_printf(&o, "%s%d,%d", k ? "," : "", (int)emu_sp(qx), (int)emu_sp(qy));
                        }
                    }

                    pb_printf(&o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d%s%s}", cg_closed, (unsigned)f, (unsigned)l,
                              (int)emu_sp(lwd), f && cg_closed ? fx : "", l ? lx : "");
                    f = l = 0;
                }

                if ((f || l) && (!straight || isline || conn[0] || !strcmp(geom, "roundRect"))) {
                    /* a turned or projected box or ellipse, a line or a connector: as a path through the shape's map */
                    double px[64], py[64];
                    int np = 0;

                    if (isline && conn[0]) {    /* a connector's own path, corner to corner through its bends */
                        double a = (adj1 >= 0 ? adj1 : 50000) / 100000.0, x2 = bw * a;

                        if (strstr(conn, "curved")) {
                            double c[2][8];
                            int seg, q, nseg = strstr(conn, "2") ? 1 : 2;

                            if (nseg == 1) {
                                double c1[8] = { 0, 0, bw / 2, 0, bw, bh / 2, bw, bh };

                                memcpy(c[0], c1, sizeof(c1));
                            } else {
                                double c1[8] = { 0, 0, x2 / 2, 0, x2, bh / 4, x2, bh / 2 };
                                double c2[8] = { x2, bh / 2, x2, bh * 3 / 4, (bw + x2) / 2, bh, bw, bh };

                                memcpy(c[0], c1, sizeof(c1));
                                memcpy(c[1], c2, sizeof(c2));
                            }

                            for (seg = 0; seg < nseg; seg++) {
                                for (q = seg ? 1 : 0; q <= 16; q++) {
                                    double u = q / 16.0, w0 = (1 - u) * (1 - u) * (1 - u), w1 = 3 * u * (1 - u) * (1 - u),
                                           w2 = 3 * u * u * (1 - u), w3 = u * u * u;

                                    px[np] = bx0 + w0 * c[seg][0] + w1 * c[seg][2] + w2 * c[seg][4] + w3 * c[seg][6];
                                    py[np++] = by0 + w0 * c[seg][1] + w1 * c[seg][3] + w2 * c[seg][5] + w3 * c[seg][7];
                                }
                            }
                        } else if (strstr(conn, "bentConnector2")) {
                            px[0] = bx0; py[0] = by0;
                            px[1] = bx0 + bw; py[1] = by0;
                            px[2] = bx0 + bw; py[2] = by0 + bh;
                            np = 3;
                        } else {    /* bent, three segments (and the four- and five-segment ones as near as that) */
                            px[0] = bx0; py[0] = by0;
                            px[1] = bx0 + x2; py[1] = by0;
                            px[2] = bx0 + x2; py[2] = by0 + bh;
                            px[3] = bx0 + bw; py[3] = by0 + bh;
                            np = 4;
                        }
                    } else if (isline) {   /* corner to corner; the flips are in the map */
                        px[0] = bx0;
                        py[0] = by0;
                        px[1] = bx0 + bw;
                        py[1] = by0 + bh;
                        np = 2;
                    } else if (!strcmp(geom, "ellipse")) {
                        for (k = 0; k < 48; k++) {
                            px[k] = bx0 + bw / 2 + bw / 2 * cos(k * 6.2831853 / 48);
                            py[k] = by0 + bh / 2 + bh / 2 * sin(k * 6.2831853 / 48);
                        }

                        np = 48;
                    } else if (!strcmp(geom, "roundRect")) {
                        double r = (bw < bh ? bw : bh) * (adj1 >= 0 ? adj1 : 16667) / 100000.0;
                        static const double cxs[4] = { 1, 1, 0, 0 }, cys[4] = { 0, 1, 1, 0 };
                        int c, q;

                        for (c = 0; c < 4; c++) {
                            double ccx = bx0 + (cxs[c] ? bw - r : r), ccy = by0 + (cys[c] ? bh - r : r);

                            for (q = 0; q <= 6; q++) {
                                double an = (c * 90 - 90 + q * 15) * 3.14159265358979 / 180;

                                px[np] = ccx + r * cos(an);
                                py[np++] = ccy + r * sin(an);
                            }
                        }
                    } else {
                        px[0] = bx0; py[0] = by0;
                        px[1] = bx0 + bw; py[1] = by0;
                        px[2] = bx0 + bw; py[2] = by0 + bh;
                        px[3] = bx0; py[3] = by0 + bh;
                        np = 4;
                    }

                    for (k = 0; k < np; k++) {
                        double rx, ry;

                        fr_pt(&sm, px[k], py[k], &rx, &ry);
                        px[k] = rx;
                        py[k] = ry;
                    }

                    ITEM_SEP();
                    pb_puts(&o, "{\"path\":[");

                    for (k = 0; k < np; k++) {
                        pb_printf(&o, "%s%d,%d", k ? "," : "", (int)emu_sp(px[k]), (int)emu_sp(py[k]));
                    }

                    pb_printf(&o, "],\"closed\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d%s%s%s}", isline ? 0 : 1,
                              (unsigned)(isline ? 0 : f), (unsigned)l, (int)emu_sp(lwd), isline ? "" : fx, l ? lx : "",
                              isline && l ? ax : "");
                    f = l = 0;  /* drawn */
                }

                if (f || l) {   /* a box or an ellipse square to the page */
                    double x0, y0, x1, y1;

                    fr_pt(&sm, bx0, by0, &x0, &y0);
                    fr_pt(&sm, bx0 + bw, by0 + bh, &x1, &y1);
                    ITEM_SEP();
                    pb_printf(&o, "{\"shape\":\"%s\",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d%s%s}",
                              geom, (int)emu_sp(x0), (int)emu_sp(y0), (int)emu_sp(x1 - x0), (int)emu_sp(y1 - y0), (unsigned)f,
                              (unsigned)l, (int)emu_sp(lwd), f ? fx : "", l ? lx : "");
                }

                if (tx_a && tx_b > tx_a && X->depth < 3) {
                    /* the text box's content as a story of the document, read as the body is -- tables, pictures,
                       styles, fields -- and edited in place; the drawing says where it goes. Made again from
                       edited XML: the story it had, as edited */
                    pd_char_props keep = X->b->cp;
                    pd_block_id story = X->rebuild ? dw_rebuild_story(X) : 0;
                    int fresh = !story;

                    if (fresh) {    /* read now: a text box new to the drawing, or the drawing read from DOCX */
                        story = bld_story_begin(X->b);
                    }

                    if (story && fresh) {
                        pd_block_id para;

                        X->depth++;
                        dw_parse(X, tx_a, (size_t)(tx_b - tx_a), 1);
                        X->depth--;
                        bld_end_para(X->b);
                        bld_story_end(X->b);

                        if (pd_doc_child(X->b->d, story, 0) == 0) {     /* empty: a paragraph to type in */
                            pd_doc_insert_block(X->b->d, story, -1, PD_BLOCK_PARAGRAPH, &para);
                        }
                    }

                    if (story) {
                        /* turned as its shape is (and as its bodyPr says), unless it is to stay upright; text
                           flipped upside down turned round */
                        long long trot = (tx_upright ? 0 : (long long)xf.rot + (xf.flipv ? 10800000 : 0)) + tx_rot +
                                         tx_vert;
                        double tx = x, ty = y, tw = cw, th = ch;

                        if (tx_vert) {  /* vertical: laid out across the box's height */
                            tx = x + cw / 2 - ch / 2;
                            ty = y + ch / 2 - cw / 2;
                            tw = ch;
                            th = cw;
                        }

                        trot %= 21600000;
                        ITEM_SEP();
                        pb_printf(&o, "{\"story\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"ins\":[%d,%d,%d,%d],"
                                  "\"anchor\":\"%s\"", (int)story, (int)emu_sp(tx), (int)emu_sp(ty), (int)emu_sp(tw),
                                  (int)emu_sp(th), (int)emu_sp((double)ins[0]), (int)emu_sp((double)ins[1]),
                                  (int)emu_sp((double)ins[2]), (int)emu_sp((double)ins[3]), anchor);

                        if (trot) {
                            pb_printf(&o, ",\"rot\":%lld", trot < 0 ? trot + 21600000 : trot);
                        }

                        if (tx_actr) {
                            pb_puts(&o, ",\"actr\":1");
                        }

                        pb_putc(&o, '}');
                        nopara = 1;     /* not also drawn from the copy read above */
                    }

                    X->b->cp = keep;
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

        /* the canvas's own background and frame, ahead of its shapes */
        if (canvas && !kind && (!strcmp(t, "bg") || !strcmp(t, "whole"))) {
            if (t[0] == 'b') {
                in_cvbg = g.type == MT_OPEN;
            } else {
                in_cvwhole = g.type == MT_OPEN;
            }

            if (g.type == MT_CLOSE && ((t[0] == 'b' && cv_bg) || (t[0] == 'w' && cv_line))) {
                ITEM_SEP();
                pb_printf(&o, "{\"shape\":\"rect\",\"x\":0,\"y\":0,\"w\":%d,\"h\":%d,\"fill\":%u,\"line\":%u,\"lw\":%d}",
                          (int)emu_sp((double)w->cx), (int)emu_sp((double)w->cy), (unsigned)(t[0] == 'b' ? cv_bg : 0),
                          (unsigned)(t[0] == 'w' ? cv_line : 0), (int)emu_sp((double)cv_lw));
            }

            continue;
        }

        if (!kind && (in_cvbg || in_cvwhole)) {
            uint32_t c;

            if (in_cvwhole && !strcmp(t, "ln") && open) {
                cv_lw = mu_attr(&g, "w", v, sizeof(v)) ? atoll(v) : 9525;
            } else if (open && dw_color(X, &g, t, &c)) {
                if (in_cvbg) {
                    cv_bg = c;
                } else {
                    cv_line = c;
                }
            }

            continue;
        }

        if (!kind) {
            continue;
        }

        /* extrusion, its colour, and pattern fills: read before the colours below take them for the fill */
        if (in_sppr && !strcmp(t, "sp3d")) {
            if (open) {
                ext_h = mu_attr(&g, "extrusionH", v, sizeof(v)) ? atoll(v) : 0;
            }

            continue;
        } else if (in_sppr && !strcmp(t, "bevelT") && open) {
            /* a top bevel raises the face by its height: counted with the depth (its rounding is not drawn) */
            if (ext_h > 0 && mu_attr(&g, "h", v, sizeof(v))) {
                ext_h += atoll(v);
            }

            continue;
        } else if (in_sppr && !strcmp(t, "extrusionClr")) {
            in_extclr = g.type == MT_OPEN;
            continue;
        } else if (in_extclr) {
            if (open && dw_color(X, &g, t, &ext_clr)) {
                cur_clr = g.type == MT_OPEN ? &ext_clr : NULL;
            } else if (cur_clr && open) {
                *cur_clr = pd_conv_clr_modify(*cur_clr, t, &g);
            }

            continue;
        } else if (in_sppr && !in_ln && !strcmp(t, "pattFill")) {
            if (open) {     /* a pattern, seen from afar: its colours mixed in the proportion it covers */
                in_patt = 1;
                patt_fg = 0xFF000000u;
                patt_bg = 0xFFFFFFFFu;
                patt_pct = 50;

                have_patt = mu_attr(&g, "prst", v, sizeof(v)) && pd_pattern_bits(v, patt_bits);

                if (mu_attr(&g, "prst", v, sizeof(v)) && !strncmp(v, "pct", 3)) {
                    patt_pct = atoi(v + 3);
                } else if (mu_attr(&g, "prst", v, sizeof(v)) && (!strncmp(v, "lt", 2) || !strncmp(v, "narrow", 6))) {
                    patt_pct = 25;
                } else if (mu_attr(&g, "prst", v, sizeof(v)) && !strncmp(v, "dk", 2)) {
                    patt_pct = 75;
                }
            }

            if (g.type != MT_OPEN) {
                double f = patt_pct / 100.0;

                in_patt = 0;
                have_fill = 1;
                fill = 0xFF000000u | (uint32_t)((((patt_fg >> 16) & 255) * f + ((patt_bg >> 16) & 255) * (1 - f)) + 0.5) << 16 |
                       (uint32_t)((((patt_fg >> 8) & 255) * f + ((patt_bg >> 8) & 255) * (1 - f)) + 0.5) << 8 |
                       (uint32_t)(((patt_fg & 255) * f + (patt_bg & 255) * (1 - f)) + 0.5);
            }

            continue;
        } else if (in_patt) {
            if (!strcmp(t, "fgClr") || !strcmp(t, "bgClr")) {
                patt_part = g.type == MT_OPEN ? (t[0] == 'f' ? 1 : 2) : 0;
            } else if (patt_part && open && dw_color(X, &g, t, patt_part == 1 ? &patt_fg : &patt_bg)) {
                cur_clr = g.type == MT_OPEN ? (patt_part == 1 ? &patt_fg : &patt_bg) : NULL;
            } else if (cur_clr && open) {
                *cur_clr = pd_conv_clr_modify(*cur_clr, t, &g);
            }

            continue;
        } else if (in_sppr && !strcmp(t, "gd") && open && mu_attr(&g, "fmla", v, sizeof(v))) {
            char gn[40] = "";
            double r;

            mu_attr(&g, "name", gn, sizeof(gn));
            gds.w = (double)xf.ext[0];
            gds.h = (double)xf.ext[1];
            r = dw_guide_add(&gds, gn, v);

            if (adj1 < 0 && !strncmp(gn, "adj", 3)) {
                adj1 = (long long)r;
            }

            continue;
        } else if (in_sppr && !in_ln && !strcmp(t, "grpFill") && open) {
            have_fill = 1;      /* the enclosing group's */
            fill = gfill[nfr - 1];
            continue;
        }

        if (in_sppr && !in_ln && !strcmp(t, "gradFill")) {     /* a gradient: its stops, in order */
            in_grad = g.type == MT_OPEN;

            if (open && !have_fill) {
                grad_kind = 1;
                grad_ang = 0;
                ngst = 0;
                grad_rws = !(mu_attr(&g, "rotWithShape", v, sizeof(v)) && (!strcmp(v, "0") || !strcmp(v, "false")));
            }

            if (g.type != MT_OPEN && grad_kind && ngst > 0 && !have_fill) {
                int q, r;

                for (q = 1; q < ngst; q++) {    /* by position, those at one place kept in their order */
                    for (r = q; r > 0 && gst_pos[r - 1] > gst_pos[r]; r--) {
                        int tp = gst_pos[r];
                        uint32_t tc = gst_clr[r];

                        gst_pos[r] = gst_pos[r - 1];
                        gst_clr[r] = gst_clr[r - 1];
                        gst_pos[r - 1] = tp;
                        gst_clr[r - 1] = tc;
                    }
                }

                fill = gst_clr[0];      /* a host drawing two colours: the first and the last */
                fill2 = ngst > 1 ? gst_clr[ngst - 1] : 0;
                have_fill = 1;
                grad_kind = ngst > 1 ? grad_kind : 0;
            }
        } else if (in_grad && !strcmp(t, "lin") && open) {
            grad_ang = mu_attr(&g, "ang", v, sizeof(v)) ? atoi(v) : 0;
            grad_sc = mu_attr(&g, "scaled", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));
        } else if (in_grad && !strcmp(t, "path") && open) {
            grad_kind = 2;
            grad_path = mu_attr(&g, "path", v, sizeof(v)) ? (!strcmp(v, "rect") ? 2 : !strcmp(v, "shape") ? 3 : 1) : 1;
        } else if (in_grad && !strcmp(t, "fillToRect") && open) {
            static const char* side[4] = { "l", "t", "r", "b" };
            int q;

            for (q = 0; q < 4; q++) {
                gft[q] = mu_attr(&g, side[q], v, sizeof(v)) ? atoi(v) : 0;
            }
        } else if (in_grad && grad_kind && !strcmp(t, "gs") && g.type == MT_OPEN && ngst < PD_MAX_STOPS) {
            gst_pos[ngst] = mu_attr(&g, "pos", v, sizeof(v)) ? atoi(v) : 0;
            gst_clr[ngst] = 0xFF000000u;
        } else if (in_sppr && !strcmp(t, "outerShdw")) {    /* a shadow cast down and across */
            in_shd = g.type == MT_OPEN;

            if (open) {
                shd_dist = mu_attr(&g, "dist", v, sizeof(v)) ? atof(v) : 38100;
                shd_dir = mu_attr(&g, "dir", v, sizeof(v)) ? atof(v) / 60000.0 : 45;
                shd_clr = 0x66000000u;
                has_shd = 1;
                shd_blur = mu_attr(&g, "blurRad", v, sizeof(v)) ? atof(v) : 0;
                shd_k[0] = mu_attr(&g, "sx", v, sizeof(v)) ? atof(v) : 100000;
                shd_k[1] = mu_attr(&g, "sy", v, sizeof(v)) ? atof(v) : 100000;
                shd_k[2] = mu_attr(&g, "kx", v, sizeof(v)) ? atof(v) : 0;
                shd_k[3] = mu_attr(&g, "ky", v, sizeof(v)) ? atof(v) : 0;
                shd_algn = 7;

                if (mu_attr(&g, "algn", v, sizeof(v))) {
                    static const char* al[9] = { "tl", "t", "tr", "l", "ctr", "r", "bl", "b", "br" };
                    int q;

                    for (q = 0; q < 9; q++) {
                        shd_algn = !strcmp(v, al[q]) ? q : shd_algn;
                    }
                }
            }
        } else if (in_sppr && !strcmp(t, "innerShdw")) {    /* a shadow inside, cast from the edge */
            in_ish = g.type == MT_OPEN;

            if (open) {
                ish_blur = mu_attr(&g, "blurRad", v, sizeof(v)) ? atof(v) : 0;
                ish_dist = mu_attr(&g, "dist", v, sizeof(v)) ? atof(v) : 0;
                ish_dir = mu_attr(&g, "dir", v, sizeof(v)) ? atof(v) / 60000.0 : 0;
                ish_clr = 0xFF000000u;
                has_ish = 1;
            }
        } else if (in_sppr && !strcmp(t, "glow")) {
            in_glow = g.type == MT_OPEN;

            if (open) {
                glow_rad = mu_attr(&g, "rad", v, sizeof(v)) ? atof(v) : 0;
                glow_clr = 0xFF000000u;
            }
        } else if (in_sppr && !strcmp(t, "softEdge") && open) {
            soft = mu_attr(&g, "rad", v, sizeof(v)) ? atoi(v) : 0;
        } else if (in_sppr && !strcmp(t, "reflection") && open) {
            static const char* ra[6] = { "blurRad", "stA", "endA", "stPos", "endPos", "dist" };
            static const double rd[6] = { 0, 100000, 0, 0, 100000, 0 };
            int q;

            for (q = 0; q < 6; q++) {
                refl[q] = mu_attr(&g, ra[q], v, sizeof(v)) ? atof(v) : rd[q];
            }

            has_refl = 1;
        } else if (kind == 1 && in_sppr && !strcmp(t, "prstGeom") && open && mu_attr(&g, "prst", v, sizeof(v)) &&
                   strcmp(v, "rect")) {
            snprintf(gprst, sizeof(gprst), "%.31s", v);
        }

        if (!strcmp(t, "spPr")) {
            in_sppr = g.type == MT_OPEN;
        } else if (!strcmp(t, "ln") && in_sppr) {
            in_ln = g.type == MT_OPEN;

            if (open) {     /* its colour, if it says one (else the style's, lnRef); its width, ends and lines */
                lw = mu_attr(&g, "w", v, sizeof(v)) ? atoll(v) : 9525;
                lcap = mu_attr(&g, "cap", v, sizeof(v)) ? (!strcmp(v, "rnd") ? 1 : !strcmp(v, "sq") ? 2 : 0) : 0;
                lcmpd = mu_attr(&g, "cmpd", v, sizeof(v)) ? (!strcmp(v, "dbl") ? 1 : !strcmp(v, "thickThin") ? 2 :
                        !strcmp(v, "thinThick") ? 3 : !strcmp(v, "tri") ? 4 : 0) : 0;
            }
        } else if (!strcmp(t, "txbxContent")) {
            in_txbx = g.type == MT_OPEN;

            if (g.type == MT_OPEN && tx_depth++ == 0) {
                tx_a = g.s + g.pos;     /* just after the opening tag */
            } else if (g.type == MT_CLOSE && tx_depth > 0 && --tx_depth == 0) {
                tx_b = g.s + g.pos;

                while (tx_b > tx_a && *tx_b != '<') {   /* back to the closing tag's start */
                    tx_b--;
                }
            }
        } else if (!strcmp(t, "fillRef")) {
            in_fillref = g.type == MT_OPEN && attr_int(&g, "idx", 0) > 0;
        } else if (!strcmp(t, "lnRef")) {
            in_lnref = g.type == MT_OPEN && attr_int(&g, "idx", 0) > 0;
        } else if (!strcmp(t, "gs")) {
            in_gs = g.type == MT_OPEN ? in_gs + 1 : 0;
        } else if (in_txbx && (!strcmp(t, "tbl") || !strcmp(t, "tblGrid") || !strcmp(t, "gridCol") ||
                               !strcmp(t, "tr") || !strcmp(t, "tc") || !strcmp(t, "tcPr") || !strcmp(t, "tblPr") ||
                               !strcmp(t, "gridSpan") || (tb_depth == 1 && (tb_pr || tb_tcpr)))) {
            if (!strcmp(t, "tbl")) {
                if (g.type == MT_OPEN && ++tb_depth == 1) {
                    if (para_open) {
                        pb_puts(&text, "]}");
                        para_open = 0;
                    }

                    pb_printf(&text, "%s{\"table\":{", nopara ? "" : ",");
                    nopara = 0;
                    tb_row0 = 1;
                    tb_rule = 0;
                    tb_grid = 0;
                    tb_jc = PD_ALIGN_LEFT;
                } else if (g.type == MT_CLOSE && tb_depth > 0 && --tb_depth == 0) {
                    pb_puts(&text, tb_grid ? "]}}" : "\"cols\":[],\"rows\":[]}}");
                }
            } else if (tb_depth != 1) {
                /* a table in the table: its cells' text flows into the outer cell */
            } else if (!strcmp(t, "tblPr")) {
                tb_pr = g.type == MT_OPEN;
            } else if (tb_pr) {
                if (!strcmp(t, "jc") && mu_attr(&g, "w:val", v, sizeof(v))) {
                    tb_jc = !strcmp(v, "center") ? PD_ALIGN_CENTER : !strcmp(v, "right") || !strcmp(v, "end") ?
                            PD_ALIGN_RIGHT : PD_ALIGN_LEFT;
                } else if ((!strcmp(t, "top") || !strcmp(t, "insideH")) && open && mu_attr(&g, "w:val", v, sizeof(v)) &&
                           strcmp(v, "nil") && strcmp(v, "none")) {
                    int sz = attr_int(&g, "w:sz", 2);     /* eighths of a point, a hairline at the least */

                    sz = (sz < 2 ? 2 : sz) * 65536 / 8;
                    tb_rule = sz > tb_rule ? sz : tb_rule;
                }
            } else if (!strcmp(t, "tblGrid")) {
                if (g.type == MT_OPEN && !tb_grid) {
                    tb_grid = 1;
                    pb_printf(&text, "\"rules\":%d,\"jc\":%d,\"cols\":[", tb_rule, tb_jc);
                    tb_cell0 = 1;
                } else if (g.type == MT_CLOSE && tb_grid == 1) {
                    tb_grid = 2;
                    pb_puts(&text, "],\"rows\":[");
                }
            } else if (tb_grid != 2 && strcmp(t, "gridCol")) {
                /* rows before the grid: not a table to draw */
            } else if (!strcmp(t, "gridCol") && open && tb_grid == 1) {
                pb_printf(&text, "%s%d", tb_cell0 ? "" : ",", attr_int(&g, "w:w", 0));
                tb_cell0 = 0;
            } else if (!strcmp(t, "tr")) {
                if (g.type == MT_OPEN) {
                    pb_puts(&text, tb_row0 ? "[" : ",[");
                    tb_row0 = 0;
                    tb_cell0 = 1;
                } else if (g.type == MT_CLOSE) {
                    pb_putc(&text, ']');
                }
            } else if (!strcmp(t, "tc")) {
                if (g.type == MT_OPEN) {
                    tb_span = 1;
                    tb_bg = 0;
                    tb_head = 0;
                } else if (g.type == MT_CLOSE) {
                    if (para_open) {
                        pb_puts(&text, "]}");
                        para_open = 0;
                    }

                    if (!tb_head) {
                        pb_printf(&text, "%s{\"span\":%d,\"bg\":%u,\"paras\":[", tb_cell0 ? "" : ",", tb_span,
                                  (unsigned)tb_bg);
                        tb_cell0 = 0;
                    }

                    pb_puts(&text, "]}");
                    nopara = 0;
                    tb_head = 1;
                }
            } else if (!strcmp(t, "tcPr")) {
                tb_tcpr = g.type == MT_OPEN;

                if (g.type == MT_CLOSE && !tb_head) {   /* the cell's head, its paragraphs to follow */
                    pb_printf(&text, "%s{\"span\":%d,\"bg\":%u,\"paras\":[", tb_cell0 ? "" : ",", tb_span,
                              (unsigned)tb_bg);
                    tb_cell0 = 0;
                    tb_head = 1;
                    nopara = 1;
                }
            } else if (tb_tcpr && !strcmp(t, "gridSpan") && open) {
                tb_span = attr_int(&g, "w:val", 1);
            } else if (tb_tcpr && !strcmp(t, "shd") && open) {
                shd_color(X, &g, &tb_bg);
            }
        } else if (!open) {
            if (!strcmp(t, "camera")) {
                in_camera = 0;
            } else if (!strcmp(t, "p") && para_open) {
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
        } else if (in_sppr && !strcmp(t, "camera")) {
            if (!mu_attr(&g, "prst", cam_prst, sizeof(cam_prst))) {
                cam_prst[0] = '\0';
            }

            in_camera = g.type == MT_OPEN;
        } else if (in_camera && !strcmp(t, "rot")) {
            cam_has_rot = 1;
            cam_lat = mu_attr(&g, "lat", v, sizeof(v)) ? atof(v) : 0;
            cam_lon = mu_attr(&g, "lon", v, sizeof(v)) ? atof(v) : 0;
            cam_rev = mu_attr(&g, "rev", v, sizeof(v)) ? atof(v) : 0;
        } else if (in_sppr && !strcmp(t, "custGeom")) {
            strcpy(geom, "cust");

            if (g.type == MT_CLOSE && cg && cg->np > 0) {   /* its first path, as an extruded freeform's front */
                cg_n = dw_cust_flat(cg, 0, cg_xy, 2048, &cg_closed);
                cg_w = (long long)cg->path[0].w;
                cg_h = (long long)cg->path[0].h;
            }
        } else if (in_sppr && cg && !in_grad && !strcmp(t, "path") && !strcmp(geom, "cust")) {
            /* a path: its own coordinate space, fill and outline */
            if (cg->np < DW_CG_MAXPATH) {
                cg->np++;
                cg->path[cg->np - 1].start = cg->nc;
                cg->path[cg->np - 1].n = 0;
                cg->path[cg->np - 1].w = mu_attr(&g, "w", v, sizeof(v)) ? atof(v) : 0;
                cg->path[cg->np - 1].h = mu_attr(&g, "h", v, sizeof(v)) ? atof(v) : 0;
                snprintf(cg->path[cg->np - 1].fill, sizeof(cg->path[0].fill), "%.15s",
                         mu_attr(&g, "fill", v, sizeof(v)) ? v : "norm");
                cg->path[cg->np - 1].stroke = !(mu_attr(&g, "stroke", v, sizeof(v)) && (!strcmp(v, "0") ||
                                                !strcmp(v, "false")));
                cg->cx = cg->cy = cg->sx = cg->sy = 0;
                cg->npt = 0;
            }
        } else if (in_sppr && cg && !strcmp(geom, "cust") && (!strcmp(t, "moveTo") || !strcmp(t, "lnTo") ||
                   !strcmp(t, "cubicBezTo") || !strcmp(t, "quadBezTo"))) {
            cg->cmd = t[0];
            cg->npt = 0;
        } else if (in_sppr && cg && !strcmp(geom, "cust") && !strcmp(t, "arcTo")) {
            /* an arc from where the path is, on the ellipse whose stAng point that is */
            double p[24];
            double wr = mu_attr(&g, "wR", v, sizeof(v)) ? dw_guide_arg(&gds, v) : 0;
            double hr = mu_attr(&g, "hR", v, sizeof(v)) ? dw_guide_arg(&gds, v) : 0;
            double st = mu_attr(&g, "stAng", v, sizeof(v)) ? dw_guide_arg(&gds, v) : 0;
            double sw = mu_attr(&g, "swAng", v, sizeof(v)) ? dw_guide_arg(&gds, v) : 0;
            int segs = pd_preset_arc(cg->cx, cg->cy, wr, hr, st, sw, p), q;

            for (q = 0; q < segs; q++) {
                dw_cust_put(cg, 'c', p + 6 * q, 6);
            }

            cg->cx = p[6 * segs - 2];
            cg->cy = p[6 * segs - 1];
        } else if (in_sppr && cg && !strcmp(geom, "cust") && !strcmp(t, "close") && open) {
            dw_cust_put(cg, 'z', &cg->sx, 0);
            cg->cx = cg->sx;
            cg->cy = cg->sy;
        } else if (in_sppr && cg && !strcmp(geom, "cust") && !strcmp(t, "pt") && cg->npt < 3) {
            double* pt = &cg->pt[2 * cg->npt];

            pt[0] = mu_attr(&g, "x", v, sizeof(v)) ? dw_guide_arg(&gds, v) : 0;
            pt[1] = mu_attr(&g, "y", v, sizeof(v)) ? dw_guide_arg(&gds, v) : 0;
            cg->npt++;

            if (cg->cmd == 'm' || cg->cmd == 'l') {
                dw_cust_put(cg, cg->cmd, cg->pt, 2);
                cg->cx = cg->pt[0];
                cg->cy = cg->pt[1];

                if (cg->cmd == 'm') {
                    cg->sx = cg->cx;
                    cg->sy = cg->cy;
                }

                cg->npt = 0;
            } else if (cg->cmd == 'c' && cg->npt == 3) {
                dw_cust_put(cg, 'c', cg->pt, 6);
                cg->cx = cg->pt[4];
                cg->cy = cg->pt[5];
                cg->npt = 0;
            } else if (cg->cmd == 'q' && cg->npt == 2) {    /* as a cubic */
                double c3[6] = { cg->cx + 2.0 / 3 * (cg->pt[0] - cg->cx), cg->cy + 2.0 / 3 * (cg->pt[1] - cg->cy),
                                 cg->pt[2] + 2.0 / 3 * (cg->pt[0] - cg->pt[2]), cg->pt[3] + 2.0 / 3 * (cg->pt[1] - cg->pt[3]),
                                 cg->pt[2], cg->pt[3]
                               };

                dw_cust_put(cg, 'c', c3, 6);
                cg->cx = cg->pt[2];
                cg->cy = cg->pt[3];
                cg->npt = 0;
            }
        } else if (in_sppr && !strcmp(t, "prstGeom") && mu_attr(&g, "prst", v, sizeof(v))) {
            snprintf(prst, sizeof(prst), "%.31s", v);
            snprintf(geom, sizeof(geom), "%s", !strcmp(v, "ellipse") ? "ellipse" : !strcmp(v, "line") ||
                     strstr(v, "Connector") ? "line" : !strcmp(v, "roundRect") ? "roundRect" : "rect");
            snprintf(conn, sizeof(conn), "%.23s", strstr(v, "Connector") && strcmp(v, "straightConnector1") ? v : "");

            if (!strcmp(v, "flowChartTerminator") || !strcmp(v, "flowChartAlternateProcess")) {
                strcpy(geom, "roundRect");
            }

            for (k2 = 0; k2 < (int)(sizeof(dw_presets) / sizeof(dw_presets[0])); k2++) {
                if (!strcmp(v, dw_presets[k2].n)) {     /* a polygon: as a custom geometry of its corners */
                    int q;

                    strcpy(geom, "cust");
                    cg_w = cg_h = 1000;
                    cg_n = 0;

                    for (q = 0; q < dw_presets[k2].np; q++, cg_n++) {
                        cg_xy[2 * cg_n] = dw_presets[k2].xy[2 * q];
                        cg_xy[2 * cg_n + 1] = dw_presets[k2].xy[2 * q + 1];
                    }

                    cg_closed = 1;
                    break;
                }
            }
        } else if (!strcmp(t, "srgbClr") || !strcmp(t, "schemeClr") || !strcmp(t, "sysClr")) {
            uint32_t c = !strcmp(t, "schemeClr") ? (mu_attr(&g, "val", v, sizeof(v)) ? scheme_color(X, v) :
                                                    0xFF000000u) :
                         mu_attr(&g, !strcmp(t, "sysClr") ? "lastClr" : "val", v, sizeof(v)) ?
                         0xFF000000u | (uint32_t)strtoul(v, NULL, 16) : 0xFF000000u;

            cur_clr = NULL;

            if (in_shd) {
                shd_clr = (c & 0xFFFFFFu) | 0x66000000u;    /* see-through unless its alpha says */
                cur_clr = &shd_clr;
            } else if (in_ish) {
                ish_clr = c;
                cur_clr = &ish_clr;
            } else if (in_glow) {
                glow_clr = c;
                cur_clr = &glow_clr;
            } else if (in_ln) {
                line = c;
                have_line = 1;
                cur_clr = &line;
            } else if (in_sppr && in_grad && in_gs && grad_kind && !have_fill) {
                if (ngst < PD_MAX_STOPS) {  /* a stop's colour, its modifiers to follow */
                    gst_clr[ngst] = c;
                    cur_clr = &gst_clr[ngst++];
                }
            } else if (in_sppr && in_grad && in_gs && have_fill) {
                fill2 = c;      /* a later stop: the last one, in the end */
                cur_clr = &fill2;
            } else if (in_sppr && (!in_gs || in_gs == 1) && !have_fill) {
                fill = c;   /* a gradient: its first stop */
                have_fill = 1;
                cur_clr = &fill;
            } else if (in_fillref) {
                sfill = c;
                style_fill = 1;
                cur_clr = &sfill;
            } else if (in_lnref) {
                sline = c;
                style_line = 1;
                cur_clr = &sline;
            }

            if (g.type != MT_OPEN) {
                cur_clr = NULL;     /* no modifiers inside */
            }
        } else if (cur_clr && (!strcmp(t, "lumMod") || !strcmp(t, "lumOff") || !strcmp(t, "tint") ||
                               !strcmp(t, "shade") || !strcmp(t, "alpha"))) {
            *cur_clr = pd_conv_clr_modify(*cur_clr, t, &g);
        } else if (in_ln && !strcmp(t, "prstDash") && mu_attr(&g, "val", v, sizeof(v))) {
            ndash = dw_dash_preset(v, dash);
        } else if (in_ln && !strcmp(t, "custDash")) {
            ndash = 0;
        } else if (in_ln && !strcmp(t, "ds") && ndash < 8) {    /* a dash of a pattern of its own and its gap */
            dash[ndash++] = mu_attr(&g, "d", v, sizeof(v)) ? atoi(v) : 100000;
            dash[ndash++] = mu_attr(&g, "sp", v, sizeof(v)) ? atoi(v) : 100000;
        } else if (in_ln && (!strcmp(t, "round") || !strcmp(t, "bevel") || !strcmp(t, "miter"))) {
            ljoin = t[0] == 'r' ? 0 : t[0] == 'b' ? 1 : 2;
        } else if (in_ln && (!strcmp(t, "headEnd") || !strcmp(t, "tailEnd"))) {
            int arrow = mu_attr(&g, "type", v, sizeof(v)) ? dw_arrow_type(v) : 0, e = t[0] == 'h' ? 0 : 1;

            if (t[0] == 'h') {
                head_arrow = arrow;
            } else {
                tail_arrow = arrow;
            }

            if (mu_attr(&g, "w", v, sizeof(v))) {
                arrow_w[e] = !strcmp(v, "sm") ? 1 : !strcmp(v, "lg") ? 3 : 2;
            }

            if (mu_attr(&g, "len", v, sizeof(v))) {
                arrow_len[e] = !strcmp(v, "sm") ? 1 : !strcmp(v, "lg") ? 3 : 2;
            }
        } else if (in_ln && !strcmp(t, "noFill")) {
            have_line = 1;  /* no line */
            line = 0;
        } else if (in_sppr && !in_ln && !strcmp(t, "noFill")) {
            have_fill = 1;
            fill = 0;
        } else if (kind == 2 && in_sppr && !in_ln && !strcmp(t, "blipFill")) {
            in_bfill = g.type == MT_OPEN;

            if (open && !have_fill) {   /* filled with a picture: a clear fill the layout puts it in */
                have_fill = 1;
                fill = 0x01FFFFFFu;
            }
        } else if (in_bfill && !strcmp(t, "blip") && open) {
            mu_attr(&g, "r:embed", bf_blip, sizeof(bf_blip));
        } else if (in_bfill && !strcmp(t, "fillRect") && open) {
            static const char* sides[4] = { "l", "t", "r", "b" };
            int q;

            for (q = 0; q < 4; q++) {
                bf_rect[q] = mu_attr(&g, sides[q], v, sizeof(v)) ? atoi(v) : 0;
            }
        } else if (in_bfill && !strcmp(t, "tile") && open) {
            bf_tile = 1;
            bf_tl[0] = mu_attr(&g, "tx", v, sizeof(v)) ? atoi(v) : 0;
            bf_tl[1] = mu_attr(&g, "ty", v, sizeof(v)) ? atoi(v) : 0;
            bf_tl[2] = mu_attr(&g, "sx", v, sizeof(v)) ? atoi(v) : 100000;
            bf_tl[3] = mu_attr(&g, "sy", v, sizeof(v)) ? atoi(v) : 100000;
            bf_tl[4] = mu_attr(&g, "flip", v, sizeof(v)) ? (!strcmp(v, "x") ? 1 : !strcmp(v, "y") ? 2 : !strcmp(v, "xy") ? 3 :
                       0) : 0;
            bf_tl[5] = 0;   /* tl t tr l ctr r bl b br: across 0 1 2, down 0 3 6 */

            if (mu_attr(&g, "algn", v, sizeof(v))) {
                static const char* al[9] = { "tl", "t", "tr", "l", "ctr", "r", "bl", "b", "br" };
                int q;

                for (q = 0; q < 9; q++) {
                    bf_tl[5] = !strcmp(v, al[q]) ? q : bf_tl[5];
                }
            }
        } else if ((kind == 1 || in_bfill) && !strcmp(t, "srcRect")) {    /* cropped: by how much of each side */
            static const char* sides[4] = { "l", "t", "r", "b" };
            int q;

            for (q = 0; q < 4; q++) {
                crop[q] = mu_attr(&g, sides[q], v, sizeof(v)) ? atoi(v) : 0;
            }
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

            tx_actr = mu_attr(&g, "anchorCtr", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));
            tx_rot = mu_attr(&g, "rot", v, sizeof(v)) ? atoi(v) : 0;
            tx_upright = mu_attr(&g, "upright", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));
            tx_vert = !mu_attr(&g, "vert", v, sizeof(v)) || !strcmp(v, "horz") ? 0 : !strcmp(v, "vert270") ?
                      16200000 : 5400000;   /* (vertical text: the box turned a quarter, its text across it) */
        } else if (in_txbx) {
            if (!strcmp(t, "p") && tb_depth == 1 && tb_grid == 2 && g.type == MT_OPEN && !tb_head) {   /* a cell without tcPr */
                pb_printf(&text, "%s{\"span\":%d,\"bg\":%u,\"paras\":[", tb_cell0 ? "" : ",", tb_span,
                          (unsigned)tb_bg);
                tb_cell0 = 0;
                tb_head = 1;
                nopara = 1;
            }

            if (!strcmp(t, "p")) {
                jc = def_jc;
                pbase = base;
                memset(&rcp, 0, sizeof(rcp));
            } else if (!strcmp(t, "pStyle") && mu_attr(&g, "w:val", v, sizeof(v))) {
                dprops a;

                memset(&a, 0, sizeof(a));
                a.cp = base;
                style_chain(X, v, &a, 0);
                pbase = a.cp;

                if (a.pp.mask & PD_PP_ALIGN) {
                    jc = a.pp.align;
                }
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

    pb_puts(&o, "]");
    m->pos += g.pos;
    {   /* the mc:Fallback after the mc:Choice this group is in: the VML of the same drawing */
        const char* end = m->s + m->n, *cur = m->s + m->pos;
        const char* ch = dw_memmem(cur, (size_t)(end - cur), "</mc:Choice>", 12), *fb0 = NULL, *fb1 = NULL;
        const char* ac = dw_memmem(cur, (size_t)(end - cur), "</mc:AlternateContent>", 22);

        if (ch && ac && ch < ac) {
            fb0 = ch + 12;

            while (fb0 < ac && isspace((unsigned char)*fb0)) {
                fb0++;
            }

            fb1 = dw_memmem(fb0, (size_t)(ac - fb0), "</mc:Fallback>", 14);

            if (fb1 && !strncmp(fb0, "<mc:Fallback", 12)) {
                fb1 += 14;
            } else {
                fb0 = fb1 = NULL;
            }
        }

        dw_keep_xml(X, &o, raw0, m->s + m->pos, fb0, fb1, canvas, rel_ok, rel_id, rel_res, nrel);
    }
    pb_putc(&o, '}');

    if (!o.err && pd_doc_add_resource(X->b->d, "application/vnd.parade.drawing+json", o.p, o.n, &w->drawing_res) != PD_OK) {
        w->drawing_res = 0;
    }

    pb_free(&o);
    pb_free(&text);
    free(cg);
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

    if (!w->drawing_res) {
        memcpy(o.crop, w->crop, sizeof(o.crop));
    }

    if (!w->anchor) {
        dw_begin_para(w);
        dw_apply_run(w);    /* its run's format, not the one before it */
        bld_inline(X->b, &o);
    } else if (w->npend_fl < 8) {
        int k = w->npend_fl++;

        memset(&w->pend_fl[k], 0, sizeof(w->pend_fl[0]));
        w->pend_fl[k].res = o.resource;
        memcpy(w->pend_fl[k].crop, o.crop, sizeof(o.crop));
        w->pend_fl[k].w = o.width;
        w->pend_fl[k].h = o.height;
        w->pend_fl[k].gap = (pd_sp)(w->dist * 65536 / 12700);
        w->pend_fl[k].wrap = w->wrap;
        w->pend_fl[k].rev = deleted_rev(X, w->rev);
        w->pend_fl[k].has_off = w->posh_has_off && w->posh_align < 0;
        w->pend_fl[k].off_x = (pd_sp)(w->posh_off * 65536 / 12700) - (w->posh_page ? X->margin_left : 0);
        w->pend_fl[k].off_y = (pd_sp)(w->posv_off * 65536 / 12700);
        w->pend_fl[k].off_from = w->posv_para >= 2 ? w->posv_para - 1 : 0;   /* from the page, its margin */

        if (w->posv_para <= 1 && (w->started || !w->posv_para)) {
            w->pend_fl[k].off_y = 0;    /* from a paragraph only when it is the one after it */
        }

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
           PD_FIELD_REF_PAGE + 1 : strcmp(word, "SEQ") == 0 ? PD_FIELD_SEQ + 1 : strcmp(word, "DATE") == 0 ||
           strcmp(word, "TIME") == 0 || strcmp(word, "CREATEDATE") == 0 || strcmp(word, "SAVEDATE") == 0 ||
           strcmp(word, "PRINTDATE") == 0 ? PD_FIELD_DATE + 1 : 0;
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
    } else if (o.field == PD_FIELD_DATE || o.field == PD_FIELD_KEPT) {   /* the instruction, for its picture */
        o.source = instr;
        o.source_len = (int32_t)strlen(instr);
    } else if (o.field == PD_FIELD_HEADING) {
        const char* h = strstr(instr, "eading ");

        o.level = h ? atoi(h + 7) : 1;
        o.level = o.level < 1 ? 1 : o.level > 6 ? 6 : o.level;
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

/* the end of a kept field (its result read) */
static void dw_kept_end(dw* w) {
    pd_inline o;

    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_FIELD;
    o.field = PD_FIELD_KEPT;
    dw_begin_para(w);
    dw_apply_run(w);
    bld_inline(w->X->b, &o);
}

/* whether text read now is not shown: in a field's instruction, or a result Parade computes */
static int dw_fld_hidden(const dw* w) {
    int k;

    for (k = 0; k < w->nfs && k < 8; k++) {
        if (w->fs[k].state == 1 || w->fs[k].suppress) {
            return 1;
        }
    }

    return 0;
}

static void dw_fld_instr(dw* w, const char* s, size_t n) {
    if (w->nfs > 0 && !w->nfs_over && w->fs[w->nfs - 1].state == 1 && w->fs[w->nfs - 1].n + n < sizeof(w->fs[0].instr)) {
        memcpy(w->fs[w->nfs - 1].instr + w->fs[w->nfs - 1].n, s, n);
        w->fs[w->nfs - 1].n += n;
        w->fs[w->nfs - 1].instr[w->fs[w->nfs - 1].n] = '\0';
    }
}

static void dw_fld_begin(dw* w) {
    if (w->nfs >= 8 || w->nfs_over) {
        w->nfs_over++;
        return;
    }

    memset(&w->fs[w->nfs], 0, sizeof(w->fs[0]));
    w->fs[w->nfs].state = 1;
    w->fs[w->nfs].inner = w->nfs > 0 && w->fs[w->nfs - 1].state == 1;
    w->nfs++;
}

/* the marks of a field nested in another's instruction: U+FFF9 instruction U+FFFA result U+FFFB */
#define FLD_IN  "\xEF\xBF\xB9"
#define FLD_SEP "\xEF\xBF\xBA"
#define FLD_OUT "\xEF\xBF\xBB"

/* an instruction's text with each field nested in it as its result; 0 when one of those depends on the page
   (its result then is only Word's last one) */
static int fld_flat(const char* in, char* out, size_t cap) {
    size_t o = 0;
    int ok = 1;

    while (*in && o + 1 < cap) {
        if (!strncmp(in, FLD_IN, 3)) {
            const char* us = strstr(in, FLD_SEP), *e = us ? strstr(us, FLD_OUT) : NULL;
            char ins[256];

            if (!us || !e) {
                break;
            }

            snprintf(ins, sizeof(ins), "%.*s", (int)(us - in - 3 < 255 ? us - in - 3 : 255), in + 3);
            ok &= !page_field(ins) || page_field(ins) == PD_FIELD_DATE + 1;

            for (us += 3; us < e && o + 1 < cap; us++) {
                out[o++] = *us;
            }

            in = e + 3;
        } else {
            out[o++] = *in++;
        }
    }

    out[o] = '\0';
    return ok;
}

/* the next operand of an IF: a quoted string or a word; NULL at the end */
static const char* fld_token(const char* p, char* out, size_t cap) {
    size_t k = 0;

    while (*p == ' ') {
        p++;
    }

    if (!*p) {
        return NULL;
    }

    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) {
            if (k + 1 < cap) {
                out[k++] = *p;
            }
        }

        p += *p == '"';
    } else if (strchr("=<>", *p)) {
        while (*p && strchr("=<>", *p) && k + 1 < cap) {
            out[k++] = *p++;
        }
    } else {
        while (*p && *p != ' ' && !strchr("=<>\"", *p) && k + 1 < cap) {
            out[k++] = *p++;
        }
    }

    out[k] = '\0';
    return p;
}

/* = arithmetic: + - * / and brackets over numbers */
static double fld_expr(const char** p, int* ok);

static double fld_atom(const char** p, int* ok) {
    double v;
    char* e;

    while (**p == ' ') {
        (*p)++;
    }

    if (**p == '(') {
        (*p)++;
        v = fld_expr(p, ok);

        while (**p == ' ') {
            (*p)++;
        }

        *ok &= **p == ')';
        *p += **p == ')';
        return v;
    }

    if (**p == '-') {
        (*p)++;
        return -fld_atom(p, ok);
    }

    v = strtod(*p, &e);
    *ok &= e != *p;
    *p = e;
    return v;
}

static double fld_term(const char** p, int* ok) {
    double v = fld_atom(p, ok);

    for (;;) {
        while (**p == ' ') {
            (*p)++;
        }

        if (**p == '*') {
            (*p)++;
            v *= fld_atom(p, ok);
        } else if (**p == '/') {
            double d;

            (*p)++;
            d = fld_atom(p, ok);
            *ok &= d != 0;
            v = d != 0 ? v / d : 0;
        } else {
            return v;
        }
    }
}

static double fld_expr(const char** p, int* ok) {
    double v = fld_term(p, ok);

    for (;;) {
        while (**p == ' ') {
            (*p)++;
        }

        if (**p == '+') {
            (*p)++;
            v += fld_term(p, ok);
        } else if (**p == '-') {
            (*p)++;
            v -= fld_term(p, ok);
        } else {
            return v;
        }
    }
}

/* What Parade works out of a field when it is read, as Word would show it: the document's properties (TITLE,
   AUTHOR, SUBJECT, KEYWORDS, COMMENTS, LASTSAVEDBY, DOCPROPERTY), IF comparing its operands (numbers, else
   text), = arithmetic. 0 for anything else, or an operand that depends on the page. */
static int fld_eval(dw* w, const char* instr, char* out, size_t cap) {
    static const struct {
        const char* field;
        const char* key;
    } props[] = {
        { "TITLE", "title" }, { "AUTHOR", "author" }, { "SUBJECT", "subject" }, { "KEYWORDS", "keywords" },
        { "COMMENTS", "description" }, { "LASTSAVEDBY", "lastModifiedBy" }
    };
    char flat[512], word[32], a[128];
    const char* p;
    size_t k = 0, i, yl = 0;
    const char* y = pd_doc_metadata(w->X->b->d, &yl);

    if (!fld_flat(instr, flat, sizeof(flat))) {
        return 0;
    }

    for (p = flat; *p == ' '; p++) {
    }

    while (k + 1 < sizeof(word) && p[k] && p[k] != ' ' && p[k] != '\\') {
        word[k] = (char)toupper((unsigned char)p[k]);
        k++;
    }

    word[k] = '\0';
    p += k;

    if (!strcmp(word, "DOCPROPERTY")) {
        if (!fld_token(p, a, sizeof(a))) {
            return 0;
        }

        for (i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
            if (same_ci(a, props[i].field) || same_ci(a, props[i].key) || (same_ci(a, "Author") && i == 1)) {
                snprintf(word, sizeof(word), "%s", props[i].field);
            }
        }
    }

    for (i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        if (!strcmp(word, props[i].field)) {
            if (!y || !yaml_value(y, yl, props[i].key, out, cap)) {
                out[0] = '\0';
            }

            return 1;
        }
    }

    if (!strcmp(word, "IF")) {
        char l[128], op[8], r[128], yes[256] = "", no[256] = "";
        char* e1, *e2;
        double x1, x2;
        int c, res;

        if (!(p = fld_token(p, l, sizeof(l))) || !(p = fld_token(p, op, sizeof(op))) ||
                !(p = fld_token(p, r, sizeof(r)))) {
            return 0;
        }

        if ((p = fld_token(p, yes, sizeof(yes))) != NULL) {
            fld_token(p, no, sizeof(no));
        }

        x1 = strtod(l, &e1);
        x2 = strtod(r, &e2);
        c = *l && *r && !*e1 && !*e2 ? (x1 > x2) - (x1 < x2) : strcmp(l, r);
        res = !strcmp(op, "=") ? c == 0 : !strcmp(op, "<>") ? c != 0 : !strcmp(op, "<") ? c < 0 : !strcmp(op, ">") ?
              c > 0 : !strcmp(op, "<=") ? c <= 0 : !strcmp(op, ">=") ? c >= 0 : -1;

        if (res < 0) {
            return 0;
        }

        snprintf(out, cap, "%s", res ? yes : no);
        return 1;
    }

    if (word[0] == '=') {
        const char* q = flat + strspn(flat, " ") + 1, *e;
        int ok = 1;
        double v = fld_expr(&q, &ok);

        e = strstr(q, "\\#");     /* a number picture after it: not followed */
        ok &= !*(q + strspn(q, " ")) || (e && e == q + strspn(q, " "));

        if (!ok) {
            return 0;
        }

        snprintf(out, cap, v == (double)(long long)v && fabs(v) < 1e15 ? "%.0f" : "%g", v);
        return 1;
    }

    return 0;
}

/* The result of the innermost field begins: what it shows. Parade's own fields (page numbers, references to
   pages, counters, dates, running headings) are computed and Word's result left out; a link is a link over
   its result; the rest are kept -- a PD_FIELD_KEPT around the result, Word's or the one worked out here when
   Parade can (fld_eval) -- and a reference is also a link to what it names. */
static void dw_fld_separate(dw* w) {
    char* ins;
    const char* h, *q, *e, *s;
    char val[256];
    int kind;

    if (w->nfs_over || w->nfs <= 0 || w->fs[w->nfs - 1].state != 1) {
        return;
    }

    w->fs[w->nfs - 1].state = 2;

    if (w->fs[w->nfs - 1].inner || dw_fld_hidden(w)) {
        return;     /* in another's instruction, or in a result not shown */
    }

    ins = w->fs[w->nfs - 1].instr;
    s = ins + strspn(ins, " ");
    h = strstr(ins, "HYPERLINK");
    q = h ? strchr(h, '"') : NULL;
    e = q ? strchr(q + 1, '"') : NULL;

    if ((kind = page_field(ins)) != 0 && !strstr(ins, FLD_IN)) {
        dw_field(w, kind, ins);
        w->fs[w->nfs - 1].suppress = 1;
    } else if (!strncmp(s, "STYLEREF", 8) && strstr(s, "eading ") && !strstr(ins, FLD_IN)) {
        dw_field(w, PD_FIELD_HEADING + 1, ins);     /* a running head */
        w->fs[w->nfs - 1].suppress = 1;
    } else if (h && q && e && e > q + 1 && !strstr(ins, FLD_IN)) {
        dw_link(w, q + 1, (size_t)(e - q - 1));
        w->fs[w->nfs - 1].link = 1;
    } else if (s[0]) {
        dw_field(w, PD_FIELD_KEPT + 1, ins);
        w->fs[w->nfs - 1].kept = 1;

        if (fld_eval(w, ins, val, sizeof(val))) {
            dw_text(w, val, strlen(val));
            w->fs[w->nfs - 1].suppress = 1;
        } else if (!strncmp(s, "REF ", 4) || !strncmp(s, "NOTEREF ", 8)) {
            char arg[64];   /* a reference: Word's text kept, a link to the place it names */

            arg[0] = '#';
            field_arg(ins, arg + 1, sizeof(arg) - 1);

            if (arg[1]) {
                dw_link(w, arg, strlen(arg));
                w->fs[w->nfs - 1].link = 1;
            }
        }
    }
}

static void dw_fld_end(dw* w) {
    int k;

    if (w->nfs_over) {
        w->nfs_over--;
        return;
    }

    if (w->nfs <= 0) {
        return;
    }

    if (w->fs[w->nfs - 1].state == 1) {     /* no result part at all */
        dw_fld_separate(w);
    }

    k = --w->nfs;

    if (w->fs[k].inner && k > 0) {  /* into its parent's instruction, between FLD_IN and FLD_OUT */
        char enc[800];
        int n = snprintf(enc, sizeof(enc), FLD_IN "%s" FLD_SEP "%s" FLD_OUT, w->fs[k].instr, w->fs[k].res);

        if (n > 0 && w->fs[k - 1].n + (size_t)n < sizeof(w->fs[0].instr)) {
            memcpy(w->fs[k - 1].instr + w->fs[k - 1].n, enc, (size_t)n + 1);
            w->fs[k - 1].n += (size_t)n;
        }

        return;
    }

    if (w->fs[k].link) {
        dw_link(w, "", 0);
    }

    if (w->fs[k].kept) {
        dw_kept_end(w);
    }
}

static pd_pos at_pos(pd_block_id b, uint32_t off) {
    pd_pos q;

    q.block = b;
    q.offset = off;
    return q;
}

/* A REF field shows its bookmark's text, not the text Word last put there: the result of each kept REF (the text
   between its start and its end, inside the link \h makes) is the bookmarked text of that paragraph, its
   objects left out, in the result's own format. A bookmark not found, empty, or holding a value only layout
   knows (a field, a note's number) leaves Word's. */
static void dx_ref_results(dxi* X, pd_doc* d) {
    pd_block_id p;
    int k;

    for (p = pd_doc_next_paragraph(d, 0); p; p = pd_doc_next_paragraph(d, p)) {
        const char* t;
        uint32_t n, i;

        if (pd_doc_para_text(d, p, &t, &n) != PD_OK) {
            continue;
        }

        for (i = 0; i + 3 <= n; i++) {
            pd_inline o, e;
            char name[64], want[512];
            uint32_t a, z, j, wn = 0;
            const char* s;
            pd_pos m0, m1;
            pd_range r;

            if (memcmp(t + i, "\xEF\xBF\xBC", 3) || pd_doc_inline_at(d, at_pos(p, i), &o) != PD_OK ||
                    o.kind != PD_INLINE_FIELD || o.field != PD_FIELD_KEPT || o.source_len <= 4) {
                continue;
            }

            snprintf(want, sizeof(want), "%.*s", o.source_len, o.source);
            s = want + strspn(want, " ");

            if (strncmp(s, "REF ", 4) || strstr(want, FLD_IN)) {
                continue;
            }

            field_arg(want, name, sizeof(name));

            for (k = 0; k < X->nbm && strcmp(X->bm[k].name, name); k++) {
            }

            if (k == X->nbm || !X->bm[k].m0 || pd_doc_marker_get(d, X->bm[k].m0, &m0) != PD_OK) {
                continue;
            }

            /* the result: after the start (and a link opening it), up to the end (and the link closing it) */
            a = i + 3;

            if (a + 3 <= n && !memcmp(t + a, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at_pos(p, a), &e) == PD_OK &&
                    e.kind == PD_INLINE_LINK) {
                a += 3;
            }

            for (z = a; z + 3 <= n; z++) {
                if (!memcmp(t + z, "\xEF\xBF\xBC", 3) && pd_doc_inline_at(d, at_pos(p, z), &e) == PD_OK &&
                        ((e.kind == PD_INLINE_FIELD && e.field == PD_FIELD_KEPT) || e.kind == PD_INLINE_LINK)) {
                    break;
                }
            }

            if (z + 3 > n) {
                continue;
            }

            {
                /* the bookmarked text: to its end, or its paragraph's end */
                const char* bt;
                uint32_t bn, end;

                if (pd_doc_para_text(d, m0.block, &bt, &bn) != PD_OK) {
                    continue;
                }

                end = X->bm[k].m1 && pd_doc_marker_get(d, X->bm[k].m1, &m1) == PD_OK && m1.block == m0.block ? m1.offset :
                      bn;

                for (j = m0.offset; j < end && j < bn && wn + 1 < sizeof(want); j++) {
                    pd_inline bo;

                    if (j + 3 <= bn && !memcmp(bt + j, "\xEF\xBF\xBC", 3)) {
                        if (pd_doc_inline_at(d, at_pos(m0.block, j), &bo) == PD_OK && (bo.kind == PD_INLINE_FIELD ||
                                bo.kind == PD_INLINE_EQUATION || bo.kind == PD_INLINE_FOOTNOTE)) {
                            wn = 0;     /* a value only layout knows (a caption's number): Word's text stays */
                            break;
                        }

                        j += 2;
                    } else {
                        want[wn++] = bt[j];
                    }
                }

                want[wn] = '\0';
            }

            if (!wn || (z - a == wn && !memcmp(t + a, want, wn))) {
                continue;
            }

            r.start = at_pos(p, a);
            r.end = at_pos(p, z);

            if (z > a) {    /* the new text in the old's format */
                pd_format_id f = 0;
                int32_t nr = 0, q;
                pd_run* all;

                pd_doc_para_runs(d, p, NULL, 0, &nr);
                all = (pd_run*)malloc(((size_t)nr + 1) * sizeof(pd_run));

                if (all && pd_doc_para_runs(d, p, all, nr, &nr) == PD_OK) {
                    for (q = 0; q < nr; q++) {
                        if (all[q].start <= a && a < all[q].end) {
                            f = all[q].format;
                        }
                    }
                }

                free(all);

                if (pd_doc_delete(d, r, NULL) == PD_OK) {
                    pd_doc_insert_text(d, r.start, want, wn, f, NULL);
                }
            } else {
                pd_doc_insert_text(d, r.start, want, wn, PD_FORMAT_INHERIT, NULL);
            }

            if (pd_doc_para_text(d, p, &t, &n) != PD_OK) {
                break;
            }

            i = a + wn - 1;
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
    pd_buf tab;                 /* the table as the model keeps it (PD_FONT_TABLE_MIME) */
    char alt[128] = "", panose[24] = "", charset[8] = "";
    int generic = 0, pitch = 0;

    if ((xml = (char*)zip_read(&X->z, "word/fontTable.xml", &len)) == NULL) {
        return;
    }

    memset(&tab, 0, sizeof(tab));

    swapped = part_rels_begin(X, "word/fontTable.xml", &saved, &saved_n);
    mu_init(&m, xml, len, 0);

    while (mu_next(&m) != MT_END) {
        const char* t = mu_local(m.name);
        char rid[64], key[64];

        if ((m.type == MT_OPEN || m.type == MT_EMPTY) && !strcmp(t, "font")) {
            if (!mu_attr(&m, "w:name", family, sizeof(family))) {
                family[0] = '\0';
            }

            alt[0] = panose[0] = charset[0] = '\0';
            generic = pitch = 0;
        }

        if ((m.type == MT_CLOSE || m.type == MT_EMPTY) && !strcmp(t, "font") && family[0] &&
                !strpbrk(family, "\t\n") && !strpbrk(alt, "\t\n")) {
            pb_printf(&tab, "%s\t%s\t%d\t%d\t%s\t%s\n", family, alt, generic, pitch, strlen(panose) == 20 ? panose : "",
                      charset);
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && family[0] && !strcmp(t, "charset")) {
            unsigned long c = mu_attr(&m, "w:val", charset, sizeof(charset)) ? strtoul(charset, NULL, 16) : 256;

            if (c < 256) {
                snprintf(charset, sizeof(charset), "%02lX", c);
            } else {
                charset[0] = '\0';
            }
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && family[0] && !strcmp(t, "altName")) {
            mu_attr(&m, "w:val", alt, sizeof(alt));
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && family[0] && !strcmp(t, "panose1")) {
            char* q;

            if (!mu_attr(&m, "w:val", panose, sizeof(panose)) || strlen(panose) != 20) {
                panose[0] = '\0';
            }

            for (q = panose; *q; q++) {
                *q = *q >= 'a' && *q <= 'f' ? (char)(*q - 32) : *q;

                if (!((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'F'))) {
                    panose[0] = '\0';
                    break;
                }
            }
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && family[0] && !strcmp(t, "family")) {
            char v[32] = "";

            mu_attr(&m, "w:val", v, sizeof(v));
            generic = !strcmp(v, "roman") ? 1 : !strcmp(v, "swiss") ? 2 : !strcmp(v, "modern") ? 3 :
                      !strcmp(v, "script") ? 4 : !strcmp(v, "decorative") ? 5 : 0;
        } else if ((m.type == MT_OPEN || m.type == MT_EMPTY) && family[0] && !strcmp(t, "pitch")) {
            char v[32] = "";

            mu_attr(&m, "w:val", v, sizeof(v));
            pitch = !strcmp(v, "fixed") ? 1 : !strcmp(v, "variable") ? 2 : 0;
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

    if (tab.n && !tab.err) {
        pd_res_id res;

        pd_doc_add_resource(X->b->d, PD_FONT_TABLE_MIME, tab.p, tab.n, &res);
    }

    pb_free(&tab);
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



/* a part's path in the package from a target relative to the folder of the part that names it */
static void part_path(const char* from_dir, const char* target, char* out, size_t cap) {
    char dir[256];
    size_t n;

    if (target[0] == '/') {
        snprintf(out, cap, "%s", target + 1);
        return;
    }

    snprintf(dir, sizeof(dir), "%s", from_dir);

    while (strncmp(target, "../", 3) == 0) {  /* up a folder for each ../ */
        n = strlen(dir);

        if (n > 0 && dir[n - 1] == '/') {
            dir[--n] = '\0';
        }

        while (n > 0 && dir[n - 1] != '/') {
            dir[--n] = '\0';
        }

        target += 3;
    }

    snprintf(out, cap, "%s%s", dir, target);
}

/* the element of xml that opens with tag (an empty one, or to its closing tag) cut out */
static size_t cut_element(char* xml, size_t n, const char* tag) {
    char open[48], close[48];
    char* a, *e;
    size_t k;

    snprintf(open, sizeof(open), "<%s", tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    if ((a = strstr(xml, open)) == NULL || (a[strlen(open)] != ' ' && a[strlen(open)] != '>' && a[strlen(open)] != '/')) {
        return n;
    }

    e = strchr(a, '>');

    if (!e) {
        return n;
    }

    if (e[-1] != '/') {     /* not empty: to its end */
        char* c = strstr(e, close);

        if (!c) {
            return n;
        }

        e = c + strlen(close) - 1;
    }

    k = (size_t)(e + 1 - a);
    memmove(a, e + 1, n - (size_t)(e + 1 - xml) + 1);
    return n - k;
}

/* A chart (c:chart r:id in a drawing): drawn into a drawing resource, the part and its embedded workbook kept with
   it so that it goes back as a chart. */
static void dw_chart(dw* w, const char* rid) {
    dxi* X = w->X;
    const char* target = rel_target(X, rid, NULL);
    char path[300], rels[340], data_rid[64] = "", ext_target[300] = "", v[300];
    char* xml, *rx;
    size_t len = 0, rlen = 0;
    pd_res_id chart_res = 0, data_res = 0;
    pd_buf o, items;
    pd_sp W = emu_sp((double)w->cx), H = emu_sp((double)w->cy);
    const char* slash;

    if (!target || W <= 0 || H <= 0) {
        return;
    }

    part_path("word/", target, path, sizeof(path));

    if ((xml = (char*)zip_read(&X->z, path, &len)) == NULL) {
        return;
    }

    slash = strrchr(path, '/');
    snprintf(rels, sizeof(rels), "%.*s_rels/%s.rels", slash ? (int)(slash - path + 1) : 0, path, slash ? slash + 1 : path);

    {   /* the workbook its values come from: embedded (kept), or a file it links to (named) */
        const char* ed = strstr(xml, "externalData");
        pd_markup m;

        if (ed && (ed = strstr(ed, "r:id=\"")) != NULL) {
            snprintf(data_rid, sizeof(data_rid), "%.*s", (int)strcspn(ed + 6, "\""), ed + 6);
        }

        if (data_rid[0] && (rx = (char*)zip_read(&X->z, rels, &rlen)) != NULL) {
            mu_init(&m, rx, rlen, 0);

            while (mu_next(&m) != MT_END) {
                if ((m.type == MT_OPEN || m.type == MT_EMPTY) && !strcmp(mu_local(m.name), "Relationship") &&
                        mu_attr(&m, "Id", v, sizeof(v)) && !strcmp(v, data_rid) && mu_attr(&m, "Target", v, sizeof(v))) {
                    char mode[32] = "";

                    mu_attr(&m, "TargetMode", mode, sizeof(mode));

                    if (!strcmp(mode, "External")) {
                        snprintf(ext_target, sizeof(ext_target), "%s", v);
                    } else {
                        char dp[300], dir[300];
                        unsigned char* bytes;
                        size_t bn = 0;

                        snprintf(dir, sizeof(dir), "%.*s", slash ? (int)(slash - path + 1) : 0, path);
                        part_path(dir, v, dp, sizeof(dp));

                        if ((bytes = zip_read(&X->z, dp, &bn)) != NULL) {
                            pd_doc_add_resource(X->b->d, XLSX_MIME, bytes, bn, &data_res);
                            free(bytes);
                        }
                    }
                }
            }

            free(rx);
        }
    }

    /* what is kept names no other part: its drawing over the chart and an unkept workbook go */
    len = cut_element(xml, len, "c:userShapes");

    if (!data_res && !ext_target[0]) {
        len = cut_element(xml, len, "c:externalData");
    }

    pd_doc_add_resource(X->b->d, CHART_MIME, xml, len, &chart_res);
    memset(&o, 0, sizeof(o));
    memset(&items, 0, sizeof(items));

    if (!pd_chart_items(xml, len, W, H, X->theme_clr, X->theme_minor[0] ? X->theme_minor : "Calibri", &items)) {
        pb_printf(&items, "{\"shape\":\"rect\",\"x\":0,\"y\":0,\"w\":%d,\"h\":%d,\"fill\":0,\"line\":%u,\"lw\":%d}", (int)W,
                  (int)H, 0xFFBFBFBFu, (int)PD_PT(0.75));
    }

    pb_printf(&o, "{\"w\":%d,\"h\":%d,\"chart\":%u", (int)W, (int)H, (unsigned)chart_res);

    if (data_res) {
        pb_printf(&o, ",\"data\":%u", (unsigned)data_res);
    }

    if (data_rid[0] && (data_res || ext_target[0])) {
        pb_puts(&o, ",\"dataRid\":");
        json_str(&o, data_rid, strlen(data_rid));
    }

    if (ext_target[0]) {
        pb_puts(&o, ",\"dataLink\":");
        json_str(&o, ext_target, strlen(ext_target));
    }

    pb_puts(&o, ",\"items\":[");
    pb_put(&o, items.p, items.n);
    pb_puts(&o, "]}");

    if (chart_res && !o.err && pd_doc_add_resource(X->b->d, "application/vnd.parade.drawing+json", o.p, o.n,
            &w->drawing_res) != PD_OK) {
        w->drawing_res = 0;
    }

    pb_free(&o);
    pb_free(&items);
    free(xml);
}

/* an attribute in either of the namespaces a check box's are written in */
static int attr_w14(const pd_markup* m, const char* name, char* v, size_t cap) {
    char q[48];

    snprintf(q, sizeof(q), "w14:%s", name);

    if (mu_attr(m, q, v, cap)) {
        return 1;
    }

    snprintf(q, sizeof(q), "w:%s", name);
    return mu_attr(m, q, v, cap);
}

/* a JSON member: ,"key":"value" */
static void json_member(pd_buf* o, const char* key, const char* v) {
    pb_printf(o, "%s\"%s\":", o->n > 1 ? "," : "", key);
    json_str(o, v, strlen(v));
}

/* A w:sdtPr: the control's kind into kind (dropdown, checkbox, ...), the rest as the JSON object
   pd_doc_control_at describes into o. m is at its opening tag; it is left after the closing one. */
static void dw_sdt_props(pd_markup* m, char* kind, size_t kcap, pd_buf* o) {
    pd_markup g;
    int depth = 1, nitems = 0, in_rpr = 0;
    char v[300], v2[300];

    snprintf(kind, kcap, "richtext");
    pb_putc(o, '{');
    mu_init(&g, m->s + m->pos, m->n - m->pos, 0);

    while (depth > 0 && mu_next(&g) != MT_END) {
        const char* t = mu_local(g.name);
        int open = g.type == MT_OPEN || g.type == MT_EMPTY;

        if (g.type == MT_OPEN) {
            depth++;
        } else if (g.type == MT_CLOSE && --depth == 0) {
            break;
        }

        if (!strcmp(t, "rPr")) {
            in_rpr = g.type == MT_OPEN;     /* the format new content takes: not the control's own */
            continue;
        }

        if (!open || in_rpr) {
            continue;
        }

        if (!strcmp(t, "alias") && mu_attr(&g, "w:val", v, sizeof(v))) {
            json_member(o, "title", v);
        } else if (!strcmp(t, "tag") && mu_attr(&g, "w:val", v, sizeof(v))) {
            json_member(o, "tag", v);
        } else if (!strcmp(t, "lock") && mu_attr(&g, "w:val", v, sizeof(v))) {
            json_member(o, "lock", v);
        } else if (!strcmp(t, "showingPlcHdr") && attr_on(&g)) {
            pb_printf(o, "%s\"placeholder\":1", o->n > 1 ? "," : "");
        } else if (!strcmp(t, "docPart") && mu_attr(&g, "w:val", v, sizeof(v))) {
            json_member(o, "prompt", v);    /* the glossary entry its placeholder text is */
        } else if (!strcmp(t, "checkbox")) {
            snprintf(kind, kcap, "checkbox");
        } else if (!strcmp(t, "checked")) {
            pb_printf(o, "%s\"checked\":%d", o->n > 1 ? "," : "", attr_w14(&g, "val", v, sizeof(v)) ?
                      (!strcmp(v, "1") || !strcmp(v, "true")) : 1);
        } else if ((!strcmp(t, "checkedState") || !strcmp(t, "uncheckedState")) && attr_w14(&g, "val", v, sizeof(v))) {
            json_member(o, t[0] == 'c' ? "on" : "off", v);

            if (attr_w14(&g, "font", v2, sizeof(v2))) {
                json_member(o, t[0] == 'c' ? "onfont" : "offfont", v2);
            }
        } else if (!strcmp(t, "dropDownList") || !strcmp(t, "comboBox")) {
            snprintf(kind, kcap, "%s", t[0] == 'd' ? "dropdown" : "combobox");

            if (mu_attr(&g, "w:lastValue", v, sizeof(v))) {
                json_member(o, "value", v);
            }
        } else if (!strcmp(t, "listItem")) {
            if (!mu_attr(&g, "w:displayText", v, sizeof(v))) {
                v[0] = '\0';
            }

            if (!mu_attr(&g, "w:value", v2, sizeof(v2))) {
                snprintf(v2, sizeof(v2), "%s", v);
            }

            pb_printf(o, nitems++ ? ",[" : "%s\"items\":[[", o->n > 1 ? "," : "");
            json_str(o, v[0] ? v : v2, strlen(v[0] ? v : v2));
            pb_putc(o, ',');
            json_str(o, v2, strlen(v2));
            pb_putc(o, ']');
        } else if (!strcmp(t, "date")) {
            snprintf(kind, kcap, "date");

            if (mu_attr(&g, "w:fullDate", v, sizeof(v))) {
                json_member(o, "date", v);
            }
        } else if (!strcmp(t, "dateFormat") && mu_attr(&g, "w:val", v, sizeof(v))) {
            json_member(o, "format", v);
        } else if (!strcmp(t, "lid") && mu_attr(&g, "w:val", v, sizeof(v))) {
            json_member(o, "lid", v);
        } else if (!strcmp(t, "text")) {
            snprintf(kind, kcap, "text");

            if (mu_attr(&g, "w:multiLine", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"))) {
                pb_printf(o, "%s\"multiline\":1", o->n > 1 ? "," : "");
            }
        } else if (!strcmp(t, "picture") || !strcmp(t, "group") || !strcmp(t, "citation") ||
                   !strcmp(t, "bibliography") || !strcmp(t, "equation")) {
            snprintf(kind, kcap, "%s", t);
        } else if (!strcmp(t, "docPartObj") || !strcmp(t, "docPartList")) {
            snprintf(kind, kcap, "docpart");
        } else if (!strcmp(t, "docPartGallery") && mu_attr(&g, "w:val", v, sizeof(v))) {
            json_member(o, "gallery", v);
        }

        if (nitems && strcmp(t, "listItem") != 0) {
            pb_putc(o, ']');    /* the list's items end with the first thing that is not one */
            nitems = 0;
        }
    }

    if (nitems) {
        pb_putc(o, ']');
    }

    pb_putc(o, '}');
    m->pos += g.pos;
}

/* a content control's start (kind and JSON) or, kind NULL, its end, where the paragraph is */
static void dw_control(dw* w, const char* kind, const pd_buf* spec) {
    pd_inline o;

    dw_begin_para(w);
    memset(&o, 0, sizeof(o));
    o.kind = PD_INLINE_CONTROL;

    if (kind) {
        snprintf(o.name, sizeof(o.name), "%s", kind);
        o.source = spec->p;
        o.source_len = (int32_t)spec->n;
    }

    bld_inline(w->X->b, &o);
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

        if (m.type == MT_TEXT && w->in_voffset) {
            char tv[32];
            size_t tn = m.tlen < sizeof(tv) - 1 ? m.tlen : sizeof(tv) - 1;

            memcpy(tv, m.text, tn);
            tv[tn] = '\0';
            w->posv_off = atoll(tv);
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
            if (w->in_t && w->nfs > 0 && w->fs[w->nfs - 1].inner && w->fs[w->nfs - 1].state == 2) {
                pd_buf txt;     /* a nested field's result: in its parent's instruction */

                memset(&txt, 0, sizeof(txt));
                mu_decode(m.text, m.tlen, &txt);

                if (w->fs[w->nfs - 1].nres + txt.n < sizeof(w->fs[0].res)) {
                    memcpy(w->fs[w->nfs - 1].res + w->fs[w->nfs - 1].nres, txt.p ? txt.p : "", txt.n);
                    w->fs[w->nfs - 1].nres += txt.n;
                    w->fs[w->nfs - 1].res[w->fs[w->nfs - 1].nres] = '\0';
                }

                pb_free(&txt);
            } else if (w->in_t && !dw_fld_hidden(w)) {
                pd_buf txt;

                memset(&txt, 0, sizeof(txt));
                mu_decode(m.text, m.tlen, &txt);
                dw_text(w, txt.p ? txt.p : "", txt.n);
                pb_free(&txt);
            } else if (w->in_instr && w->nfs > 0 && w->fs[w->nfs - 1].state == 1) {
                pd_buf txt;

                memset(&txt, 0, sizeof(txt));
                mu_decode(m.text, m.tlen, &txt);
                dw_fld_instr(w, txt.p ? txt.p : "", txt.n);
                pb_free(&txt);
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
                    pd_char_props keep_rcp = w->rcp;
                    char keep_rstyle[sizeof(w->rstyle)];

                    /* outside any run: the paragraph's own format, not what the run before it had */
                    memcpy(keep_rstyle, w->rstyle, sizeof(keep_rstyle));
                    memset(&w->rcp, 0, sizeof(w->rcp));
                    w->rstyle[0] = 0;
                    dw_begin_para(w);
                    dw_apply_run(w);
                    bld_inline(X->b, &o);
                    w->rcp = keep_rcp;
                    memcpy(w->rstyle, keep_rstyle, sizeof(keep_rstyle));
                }

                free(tex);
            }

            m.pos += g.pos;
            continue;
        }

        if (m.type == MT_OPEN && (w->in_drawing || w->in_vml) && !w->tbx && strcmp(t, "txbxContent") == 0 &&
                X->depth < 3) {
            /* a text box (floating or in the line, DrawingML's or VML's): its paragraphs become a float's, read
               once the anchoring paragraph ends */
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

        if (strcmp(t, "ruby") == 0 && w->in_p) {
            if (m.type == MT_OPEN) {
                w->in_ruby = 1;
                w->ruby_started = 0;
                w->ruby_text[0] = '\0';
                w->ruby_size = w->ruby_raise = 0;
            } else if (m.type == MT_CLOSE && w->in_ruby) {
                if (w->ruby_started) {
                    pd_inline o;

                    memset(&o, 0, sizeof(o));
                    o.kind = PD_INLINE_RUBY;    /* no source: where it ends */
                    bld_inline(X->b, &o);
                }

                w->in_ruby = 0;
            }

            continue;
        }

        if (w->in_ruby && (m.type == MT_OPEN || m.type == MT_EMPTY) && (strcmp(t, "hps") == 0 ||
                strcmp(t, "hpsRaise") == 0)) {
            int hp = attr_int(&m, "w:val", 0);

            if (hp > 0 && hp < 2000) {
                *(t[3] == 'R' ? &w->ruby_raise : &w->ruby_size) = (pd_sp)((int64_t)hp * 65536 / 2);
            }

            continue;
        }

        if (w->in_ruby && m.type == MT_OPEN && strcmp(t, "rt") == 0) {    /* the guide's text, kept aside */
            pd_markup g;
            int depth = 1, in_t = 0;

            mu_init(&g, m.s + m.pos, m.n - m.pos, 0);

            while (depth > 0 && mu_next(&g) != MT_END) {
                if (g.type == MT_OPEN) {
                    depth++;
                    in_t = !strcmp(mu_local(g.name), "t");
                } else if (g.type == MT_CLOSE) {
                    depth--;
                    in_t = 0;
                } else if (g.type == MT_TEXT && in_t) {
                    pd_buf txt;

                    memset(&txt, 0, sizeof(txt));
                    mu_decode(g.text, g.tlen, &txt);

                    if (txt.p) {
                        size_t k = strlen(w->ruby_text);

                        snprintf(w->ruby_text + k, sizeof(w->ruby_text) - k, "%.*s", (int)txt.n, txt.p);
                    }

                    pb_free(&txt);
                }
            }

            m.pos += g.pos;
            continue;
        }

        if (w->in_ruby && m.type == MT_OPEN && strcmp(t, "rubyBase") == 0 && w->ruby_text[0]) {
            pd_inline o;

            dw_begin_para(w);
            memset(&o, 0, sizeof(o));
            o.kind = PD_INLINE_RUBY;
            o.source = w->ruby_text;
            o.source_len = (int32_t)strlen(w->ruby_text);
            o.height = w->ruby_size;
            o.depth = w->ruby_raise;
            bld_inline(X->b, &o);
            w->ruby_started = 1;
            continue;
        }

        if (w->in_ruby && strcmp(t, "rubyPr") == 0) {
            continue;
        }

        if (m.type == MT_OPEN && strcmp(t, "sdt") == 0) {
            if (w->sdt_depth < 30) {
                w->sdt_inline &= ~(1u << w->sdt_depth);
            }

            w->sdt_depth++;
            continue;
        }

        if (m.type == MT_OPEN && strcmp(t, "sdtPr") == 0 && w->sdt_depth > 0) {
            char kind[32];
            pd_buf spec;

            memset(&spec, 0, sizeof(spec));
            dw_sdt_props(&m, kind, sizeof(kind), &spec);

            /* in a paragraph's text: a control, from here to the w:sdt's end; around paragraphs, rows or cells,
               only its content is kept */
            if (w->in_p && !w->in_drawing && w->sdt_depth <= 30 && !spec.err) {
                w->sdt_inline |= 1u << (w->sdt_depth - 1);
                dw_control(w, kind, &spec);
            }

            pb_free(&spec);
            continue;
        }

        if (m.type == MT_CLOSE && strcmp(t, "sdt") == 0 && w->sdt_depth > 0) {
            w->sdt_depth--;

            if (w->sdt_depth < 30 && (w->sdt_inline & (1u << w->sdt_depth)) && w->in_p) {
                dw_control(w, NULL, NULL);
            }

            continue;
        }

        if (open && strcmp(t, "sdtEndPr") == 0) {
            if (m.type == MT_OPEN) {
                w->skip = 1;
            }

            continue;
        }

        if (open && strcmp(t, "rPrChange") == 0 && w->in_rpr && !w->in_mrpr && !w->in_rprchg) {
            /* a run's formatting changed: what it had, in the w:rPr inside */
            w->in_rprchg = m.type == MT_OPEN;
            memset(&w->old_cp, 0, sizeof(w->old_cp));
            w->chg_author[0] = w->chg_date[0] = '\0';
            mu_attr(&m, "w:author", w->chg_author, sizeof(w->chg_author));
            mu_attr(&m, "w:date", w->chg_date, sizeof(w->chg_date));
            continue;
        }

        if (w->in_rprchg) {
            if (m.type == MT_CLOSE && strcmp(t, "rPrChange") == 0) {
                pd_revision rv;

                memset(&rv, 0, sizeof(rv));
                rv.kind = PD_REV_FORMAT;
                memcpy(rv.author, w->chg_author, sizeof(rv.author));
                memcpy(rv.date, w->chg_date, sizeof(rv.date));
                rv.old = w->old_cp;
                w->in_rprchg = 0;

                if (pd_doc_revision_add(X->b->d, &rv, &w->fmt_rev) != PD_OK) {
                    w->fmt_rev = 0;
                }
            } else if (open && strcmp(t, "rPr") != 0) {
                rpr_elem(X, &m, t, &w->old_cp, NULL, 0);
            }

            continue;
        }

        if (open && (!strcmp(t, "moveFromRangeStart") || !strcmp(t, "moveToRangeStart"))) {
            int k;      /* a move's two ends share its name: their revisions its number */

            if (!mu_attr(&m, "w:name", v, sizeof(v))) {
                v[0] = '\0';
            }

            for (k = 0; k < w->nmove && strcmp(w->move_name[k], v); k++) {
            }

            if (k == w->nmove && k < 32) {
                snprintf(w->move_name[w->nmove++], sizeof(w->move_name[0]), "%.39s", v);
            }

            w->move_cur = k + 1;
            continue;
        }

        if (open) {
            /* elements whose content is ignored */
            if (strcmp(t, "txbxContent") == 0 || strcmp(t, "Fallback") == 0 ||
                    strcmp(t, "rPrChange") == 0 ||
                    strcmp(t, "pPrChange") == 0) {
                if (m.type == MT_OPEN) {
                    w->skip = 1;
                }

                continue;
            }

            if (w->in_ppr && strcmp(t, "rPr") == 0) {  /* the paragraph mark's: an empty paragraph's size */
                w->in_mrpr = m.type == MT_OPEN;
                continue;
            }

            if (w->in_mrpr) {
                if (strcmp(t, "ins") && strcmp(t, "del") && strcmp(t, "moveTo") && strcmp(t, "moveFrom")) {
                    rpr_elem(X, &m, t, &w->mcp, w->mstyle, sizeof(w->mstyle));
                }

                continue;
            }

            if ((!strcmp(t, "ins") || !strcmp(t, "del") || !strcmp(t, "moveTo") || !strcmp(t, "moveFrom")) &&
                    m.type == MT_OPEN && !w->in_rpr) {
                pd_revision rv;     /* a tracked change: the runs inside are an insertion or a deletion */

                memset(&rv, 0, sizeof(rv));
                rv.kind = t[0] == 'd' || !strcmp(t, "moveFrom") ? PD_REV_DELETE : PD_REV_INSERT;
                rv.move = t[0] == 'm' ? (w->move_cur ? w->move_cur : 33) : 0;
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
                memset(&w->frm, 0, sizeof(w->frm));
                memset(&w->mcp, 0, sizeof(w->mcp));
                w->mstyle[0] = '\0';
                w->in_mrpr = 0;
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
                memcpy(w->sp.note_format, X->note_fmt, sizeof(X->note_fmt));
                memcpy(w->sp.note_start, X->note_start, sizeof(X->note_start));
                memcpy(w->sp.note_restart, X->note_restart, sizeof(X->note_restart));
                w->in_notepr = 0;
                w->sp.page_color = X->page_color;
                w->sp.first_page_number = 0;    /* numbered on from the section before, unless w:pgNumType says */
                w->in_pgb = 0;
                memset(w->hf_ref, 0, sizeof(w->hf_ref));

                if (w->in_ppr) {
                    w->sect_here = 1;
                }
            } else if (w->in_sect) {
                if (strcmp(t, "footnotePr") == 0 || strcmp(t, "endnotePr") == 0) {
                    w->in_notepr = m.type == MT_OPEN ? 1 + (t[0] == 'e') : 0;
                } else if (w->in_notepr) {
                    notepr_elem(&m, t, &w->sp.note_format[w->in_notepr - 1], &w->sp.note_start[w->in_notepr - 1],
                                &w->sp.note_restart[w->in_notepr - 1]);
                } else if (strcmp(t, "pgSz") == 0) {
                    w->sp.page_width = twips(attr_int(&m, "w:w", 11906));   /* (a poster's: past 2^31 / 65536) */
                    w->sp.page_height = twips(attr_int(&m, "w:h", 16838));
                } else if (strcmp(t, "pgMar") == 0) {
                    w->sp.margin_top = twips(abs(attr_int(&m, "w:top", 1440)));
                    w->sp.margin_bottom = twips(abs(attr_int(&m, "w:bottom", 1440)));
                    w->sp.margin_left = twips(attr_int(&m, "w:left", 1440));
                    w->sp.gutter = twips(attr_int(&m, "w:gutter", 0));
                    w->sp.gutter = w->sp.gutter < 0 ? 0 : w->sp.gutter;
                    w->X->margin_left = w->sp.margin_left;  /* for the pictures of its headers, read next */
                    w->sp.margin_right = twips(attr_int(&m, "w:right", 1440));
                    w->X->margin_right = w->sp.margin_right;
                    w->X->margin_top = w->sp.margin_top;
                    w->X->margin_bottom = w->sp.margin_bottom;
                    w->X->page_w = w->sp.page_width;
                    w->X->page_h = w->sp.page_height;
                    w->sp.header_distance = twips(attr_int(&m, "w:header", 720));
                    w->sp.footer_distance = twips(attr_int(&m, "w:footer", 720));
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
                } else if (strcmp(t, "bidi") == 0) {    /* right to left: its columns from the right */
                    w->sp.direction = attr_on(&m) ? PD_DIR_RTL : PD_DIR_AUTO;
                } else if (strcmp(t, "docGrid") == 0) {     /* a grid of lines: not the default one, which is none */
                    char ty[24] = "default";

                    mu_attr(&m, "w:type", ty, sizeof(ty));
                    w->sp.line_pitch = strcmp(ty, "default") != 0 && attr_int(&m, "w:linePitch", 0) > 0 ?
                                       twips(attr_int(&m, "w:linePitch", 0)) : 0;
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
                        int32_t f = num_fmt_of(v);

                        w->sp.page_number_format = f == PD_NUM_BULLET || f == PD_NUM_NONE ? PD_NUM_DECIMAL : f;
                    }
                } else if (strcmp(t, "cols") == 0) {
                    int nc = attr_int(&m, "w:num", 1);

                    w->sp.columns = nc < 1 ? 1 : nc > 16 ? 16 : nc;
                    w->sp.column_gap = attr_int(&m, "w:space", 720) * 65536 / 20;
                    w->sp.column_rule = attr_int(&m, "w:sep", 0) != 0 || (mu_attr(&m, "w:sep", v, sizeof(v)) &&
                                        (!strcmp(v, "true") || !strcmp(v, "on")));
                    w->sp.ncol_widths = 0;
                    w->in_cols = m.type == MT_OPEN && mu_attr(&m, "w:equalWidth", v, sizeof(v)) &&
                                 (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off"));
                } else if (strcmp(t, "col") == 0 && w->in_cols && w->sp.ncol_widths < PD_SECTION_MAX_COLS) {
                    pd_sp cw = twips(attr_int(&m, "w:w", 0)), cs = twips(attr_int(&m, "w:space", 0));

                    w->sp.col_width[w->sp.ncol_widths] = cw > 0 ? cw : 0;
                    w->sp.col_space[w->sp.ncol_widths++] = cs > 0 ? cs : 0;
                } else if (strcmp(t, "type") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->sp.continuous = strcmp(v, "continuous") == 0;
                    w->sp.start = !strcmp(v, "oddPage") ? PD_BREAK_ODD_PAGE : !strcmp(v, "evenPage") ?
                                  PD_BREAK_EVEN_PAGE : !strcmp(v, "nextColumn") ? PD_BREAK_COLUMN : PD_BREAK_PAGE;
                } else if (strcmp(t, "textDirection") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->sp.text_flow = !strcmp(v, "btLr") ? PD_CELLTEXT_BTLR : !strcmp(v, "tbRl") ||
                                      !strcmp(v, "tbRlV") || !strcmp(v, "rl") || !strcmp(v, "rlV") ? PD_CELLTEXT_TBRL :
                                      PD_CELLTEXT_LRTB;
                } else if (strcmp(t, "pgBorders") == 0) {
                    w->in_pgb = m.type == MT_OPEN;
                    w->sp.page_border_flags = 0;

                    if (mu_attr(&m, "w:offsetFrom", v, sizeof(v)) && !strcmp(v, "text")) {
                        w->sp.page_border_flags |= PD_PGBORDER_FROM_TEXT;
                    }

                    if (mu_attr(&m, "w:display", v, sizeof(v))) {
                        w->sp.page_border_flags |= !strcmp(v, "firstPage") ? PD_PGBORDER_FIRST :
                                                   !strcmp(v, "notFirstPage") ? PD_PGBORDER_NOT_FIRST : 0;
                    }

                    if (mu_attr(&m, "w:zOrder", v, sizeof(v)) && !strcmp(v, "back")) {
                        w->sp.page_border_flags |= PD_PGBORDER_BEHIND;
                    }
                } else if (w->in_pgb) {
                    int side = !strcmp(t, "top") ? 0 : !strcmp(t, "right") ? 1 : !strcmp(t, "bottom") ? 2 :
                               !strcmp(t, "left") ? 3 : -1;

                    if (side >= 0) {
                        border_line(w->X, &m, &w->sp.page_border[side], 1);
                    }
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
                } else if (strcmp(t, "framePr") == 0) {     /* a frame: its paragraph read into a float */
                    memset(&w->frm, 0, sizeof(w->frm));
                    w->frm.on = 1;
                    w->frm.drop = mu_attr(&m, "w:dropCap", v, sizeof(v)) ? (!strcmp(v, "drop") ? PD_FRAME_DROP :
                                  !strcmp(v, "margin") ? PD_FRAME_MARGIN : 0) : 0;
                    w->frm.lines = attr_int(&m, "w:lines", 1);
                    w->frm.w = attr_int(&m, "w:w", 0);
                    w->frm.h = attr_int(&m, "w:h", 0);
                    w->frm.hspace = attr_int(&m, "w:hSpace", 0);
                    w->frm.has_x = mu_attr(&m, "w:x", v, sizeof(v));
                    w->frm.x = atol(w->frm.has_x ? v : "0");
                    w->frm.has_y = mu_attr(&m, "w:y", v, sizeof(v));
                    w->frm.y = atol(w->frm.has_y ? v : "0");
                    w->frm.xalign = mu_attr(&m, "w:xAlign", v, sizeof(v)) ? (!strcmp(v, "right") || !strcmp(v, "outside") ?
                                    2 : !strcmp(v, "center") ? 1 : 0) : 0;
                    w->frm.hpage = mu_attr(&m, "w:hAnchor", v, sizeof(v)) && !strcmp(v, "page");
                    w->frm.vfrom = mu_attr(&m, "w:vAnchor", v, sizeof(v)) ? (!strcmp(v, "page") ? PD_FROM_PAGE :
                                   !strcmp(v, "margin") ? PD_FROM_MARGIN : PD_FROM_PARAGRAPH) : PD_FROM_MARGIN;
                    w->frm.wrap = mu_attr(&m, "w:wrap", v, sizeof(v)) && (!strcmp(v, "notBeside") || !strcmp(v, "none")) ?
                                  PD_WRAP_NONE : -1;
                } else {
                    ppr_elem(w->X, &m, t, &w->ppr);
                }
            } else if (strcmp(t, "r") == 0) {
                memset(&w->rcp, 0, sizeof(w->rcp));
                w->rstyle[0] = '\0';
                w->fmt_rev = 0;
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
                    X->bm[X->nbm].wid = attr_int(&m, "w:id", -1);
                    X->bm[X->nbm].m0 = X->bm[X->nbm].m1 = 0;
                    pd_doc_marker_new(X->b->d, bld_pos(X->b), PD_GRAVITY_LEFT, &X->bm[X->nbm].m0);
                    X->bm[X->nbm++].para = X->b->para;
                }
            } else if (strcmp(t, "bookmarkEnd") == 0 && w->in_p && w->started) {
                int id = attr_int(&m, "w:id", -1), k;

                for (k = X->nbm - 1; k >= 0 && X->bm[k].wid != id; k--) {
                }

                if (k >= 0 && !X->bm[k].m1) {
                    pd_doc_marker_new(X->b->d, bld_pos(X->b), PD_GRAVITY_LEFT, &X->bm[k].m1);
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
                w->in_posv = w->in_voffset = w->posv_para = 0;
                w->posv_off = 0;
                w->drawing_res = 0;
                w->tbx = NULL;
                w->tbn = 0;
                w->dist = 0;
                memset(w->crop, 0, sizeof(w->crop));
            } else if ((strcmp(t, "object") == 0 || strcmp(t, "pict") == 0) && m.type == MT_OPEN && !w->in_drawing) {
                /* VML: an OLE object's preview picture (an equation, a chart), or an old-style picture */
                w->in_vml = 1;
                w->blip[0] = '\0';
                w->cx = w->cy = 0;
                w->vml_style[0] = w->art[0] = w->art_font[0] = '\0';
                w->vml_fill = 0;
                w->vml_opacity = 1000;
                w->vml_wrap = -1;
                w->tbx = NULL;
                w->tbn = 0;
            } else if (w->in_vml && (strcmp(t, "shape") == 0 || strcmp(t, "rect") == 0 || strcmp(t, "roundrect") == 0 ||
                                     strcmp(t, "oval") == 0) && !w->vml_style[0] &&
                       mu_attr(&m, "style", w->vml_style, sizeof(w->vml_style))) {
                w->cx = w->cx ? w->cx : vml_length(w->vml_style, "width");
                w->cy = w->cy ? w->cy : vml_length(w->vml_style, "height");

                if (mu_attr(&m, "fillcolor", v, sizeof(v))) {
                    w->vml_fill = vml_color(v);
                }
            } else if (w->in_vml && strcmp(t, "textpath") == 0 && mu_attr(&m, "string", w->art, sizeof(w->art))) {
                if (mu_attr(&m, "style", v, sizeof(v))) {
                    vml_prop(v, "font-family", w->art_font, sizeof(w->art_font));
                }
            } else if (w->in_vml && strcmp(t, "fill") == 0 && mu_attr(&m, "opacity", v, sizeof(v))) {
                w->vml_opacity = strchr(v, 'f') ? (int)(atof(v) * 1000 / 65536) : (int)(atof(v) * 1000);
            } else if (w->in_vml && strcmp(t, "wrap") == 0 && mu_attr(&m, "type", v, sizeof(v))) {
                w->vml_wrap = !strcmp(v, "topAndBottom") ? PD_WRAP_NONE : !strcmp(v, "none") ? -1 : PD_WRAP_LEFT;
            } else if (w->in_vml && strcmp(t, "imagedata") == 0 && !w->blip[0]) {
                mu_attr(&m, "r:id", w->blip, sizeof(w->blip));
            } else if (w->in_drawing && strcmp(t, "chart") == 0 && !w->drawing_res && mu_attr(&m, "r:id", v, sizeof(v))) {
                dw_chart(w, v);
            } else if (w->in_drawing && m.type == MT_OPEN && (strcmp(t, "wpc") == 0 || strcmp(t, "wgp") == 0)) {
                dw_drawing_group(w, &m, strcmp(t, "wpc") == 0);   /* reads the group to its end */
            } else if (w->in_drawing && strcmp(t, "anchor") == 0) {
                w->anchor = 1;
                w->behind = mu_attr(&m, "behindDoc", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"));
                w->dist = atoll(mu_attr(&m, "distL", v, sizeof(v)) ? v : "0");

                if (mu_attr(&m, "distR", v, sizeof(v)) && atoll(v) > w->dist) {
                    w->dist = atoll(v);
                }
            } else if (w->in_drawing && w->anchor && strcmp(t, "wrapNone") == 0) {
                w->wrap = w->behind ? PD_WRAP_BEHIND : PD_WRAP_FRONT;   /* over or under the text */
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
            } else if (w->in_drawing && strcmp(t, "positionV") == 0) {
                w->in_posv = m.type == MT_OPEN;
                w->posv_para = !mu_attr(&m, "relativeFrom", v, sizeof(v)) ? 0 : !strcmp(v, "paragraph") ||
                               !strcmp(v, "line") ? 1 : !strcmp(v, "page") ? 2 : !strcmp(v, "margin") ||
                               !strcmp(v, "topMargin") ? 3 : 0;
            } else if (w->in_posv && strcmp(t, "posOffset") == 0) {
                w->in_voffset = m.type == MT_OPEN;
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
            } else if (w->in_drawing && strcmp(t, "srcRect") == 0) {   /* the picture cropped */
                static const char* const side[4] = { "l", "t", "r", "b" };
                int k;

                for (k = 0; k < 4; k++) {
                    int v4 = attr_int(&m, side[k], 0);

                    w->crop[k] = v4 < -100000 ? -100000 : v4 > 99999 ? 99999 : v4;
                }
            } else if (strcmp(t, "hyperlink") == 0) {
                int ext = 0;
                const char* url = mu_attr(&m, "r:id", v, sizeof(v)) ? rel_target(X, v, &ext) : NULL;

                if (m.type == MT_OPEN && w->nlinks < 16 && w->nfs > 0 && w->fs[w->nfs - 1].link) {
                    w->link_depth[w->nlinks++] = 0;     /* in a reference's result, which links already */
                } else if (url && w->nlinks < 16 && m.type == MT_OPEN) {
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
                char ins[512];

                if (!mu_attr(&m, "w:instr", ins, sizeof(ins))) {
                    ins[0] = '\0';
                }

                dw_fld_begin(w);    /* a field whose instruction is said at once, its result its content */
                dw_fld_instr(w, ins, strlen(ins));
                dw_fld_separate(w);

                if (m.type == MT_EMPTY) {
                    dw_fld_end(w);
                }
            } else if (strcmp(t, "fldChar") == 0 && mu_attr(&m, "w:fldCharType", v, sizeof(v))) {
                if (strcmp(v, "begin") == 0) {
                    dw_fld_begin(w);
                } else if (strcmp(v, "separate") == 0) {
                    dw_fld_separate(w);
                } else if (strcmp(v, "end") == 0) {
                    dw_fld_end(w);
                }
            } else if (strcmp(t, "sym") == 0 && mu_attr(&m, "w:char", v, sizeof(v))) {
                uint32_t c = (uint32_t)strtoul(v, NULL, 16), sc;
                char font[64] = "", u[4];
                size_t un = 0;

                mu_attr(&m, "w:font", font, sizeof(font));
                sc = sym_unicode(font, c);

                if (sc) {
                    c = sc;
                } else if (c >= 0xF000 && c < 0xF100 && !font[0]) {
                    c -= 0xF000;    /* no font named: the character it stands in for */
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

                if (!sc && font[0]) {   /* Wingdings and the like: its code in that font, for a font that has it */
                    pd_char_props keep = w->rcp;

                    w->rcp.mask |= PD_CP_FAMILY;
                    snprintf(w->rcp.family, sizeof(w->rcp.family), "%s", font);
                    w->rcp.font_theme &= ~3;
                    dw_text(w, u, un);
                    w->rcp = keep;
                } else {
                    dw_text(w, u, un);
                }
            } else if (strcmp(t, "footnoteRef") == 0 || strcmp(t, "endnoteRef") == 0) {
                w->after_ref = 1;
            } else if (strcmp(t, "tbl") == 0) {
                if (w->started) {
                    bld_end_para(X->b);
                    w->started = 0;
                }

                dw_begin_cell(w);

                {   /* floating (w:tblpPr): in a float of its own, where one may be */
                    pd_float_props fp;
                    pd_block_info ci;
                    pd_block_id fl = 0;

                    memset(&fp, 0, sizeof(fp));
                    fp.width_fraction = 1000;

                    if (w->ntab < 8 && pd_doc_block_info(X->b->d, bld_container(X->b), &ci) == PD_OK &&
                            (ci.kind == PD_BLOCK_SECTION || ci.kind == PD_BLOCK_STORY) && dw_tblp(w, &m, &fp)) {
                        fl = bld_float_begin(X->b);
                    }

                    bld_table_begin(X->b);

                    if (w->ntab < 8) {
                        memset(&w->tabs[w->ntab], 0, sizeof(w->tabs[0]));
                        w->tabs[w->ntab].fl = fl;
                        w->tabs[w->ntab].fp = fp;
                    }
                }

                if (w->ntab < 8) {
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
                    edge_elem(w->X, &m, t, &w->tabs[w->ntab - 1].tb);
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
            } else if (w->in_tblpr && w->ntab >= 1 && w->ntab <= 8 && strcmp(t, "bidiVisual") == 0) {
                w->tabs[w->ntab - 1].rtl = attr_on(&m);
            } else if (w->in_tblpr && w->ntab >= 1 && w->ntab <= 8 && strcmp(t, "tblCellSpacing") == 0) {
                if (!mu_attr(&m, "w:type", v, sizeof(v)) || strcmp(v, "dxa") == 0) {
                    pd_sp sp = twips(attr_int(&m, "w:w", 0));

                    w->tabs[w->ntab - 1].spacing = sp > 0 && sp < PD_PT(1000) ? sp : 0;
                }
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
                memset(&w->row_pr, 0, sizeof(w->row_pr));
                w->row_pr.split = PD_ROWSPLIT_ANY;  /* Word breaks a row where the page ends, unless told not to */

                if (w->ntab >= 1 && w->ntab <= 8) {
                    w->tabs[w->ntab - 1].row++;
                    w->tabs[w->ntab - 1].col = 0;
                    w->tabs[w->ntab - 1].row_h = 0;
                }
            } else if (strcmp(t, "trPr") == 0) {
                w->in_trpr = m.type == MT_OPEN;
            } else if (w->in_trpr && strcmp(t, "cantSplit") == 0) {
                w->row_pr.split = attr_on(&m) ? PD_ROWSPLIT_NEVER : PD_ROWSPLIT_ANY;
            } else if (w->in_trpr && (strcmp(t, "gridBefore") == 0 || strcmp(t, "gridAfter") == 0)) {
                int g = attr_int(&m, "w:val", 0);

                g = g < 0 ? 0 : g >= PD_TABLE_MAX_GRID ? PD_TABLE_MAX_GRID - 1 : g;
                *(t[4] == 'B' ? &w->row_pr.grid_before : &w->row_pr.grid_after) = g;
            } else if (w->in_trpr && (strcmp(t, "wBefore") == 0 || strcmp(t, "wAfter") == 0)) {
                if (mu_attr(&m, "w:type", v, sizeof(v)) == 0 || strcmp(v, "dxa") == 0) {
                    pd_sp ww = twips(attr_int(&m, "w:w", 0));

                    *(t[1] == 'B' ? &w->row_pr.width_before : &w->row_pr.width_after) = ww > 0 ? ww : 0;
                }
            } else if (w->in_trpr && strcmp(t, "tblHeader") == 0) {
                w->row_header = attr_on(&m);
            } else if (w->in_trpr && strcmp(t, "trHeight") == 0 && w->ntab >= 1 && w->ntab <= 8) {
                pd_sp hh = twips(attr_int(&m, "w:val", 0));

                hh = hh > 0 ? hh : 0;

                if (!mu_attr(&m, "w:hRule", v, sizeof(v)) || strcmp(v, "atLeast") == 0) {
                    w->tabs[w->ntab - 1].row_h = hh;
                } else if (strcmp(v, "exact") == 0 && hh > 0) {   /* "auto": as tall as its content */
                    w->row_pr.height = hh;
                    w->row_pr.height_exact = 1;
                }
            } else if (strcmp(t, "tc") == 0) {
                if (w->pend_row) {
                    dw_row_begin(w);
                }

                w->pend_cell = 1;
                w->span = 1;
                w->cell_bg = w->cell_bg_ref = 0;
                w->cell_merge = 0;
                w->cell_valign = 0;
                w->cell_w = 0;
                w->cell_wpct = 0;
                w->cell_dir = 0;
                w->cell_diag[0] = w->cell_diag[1] = 0;
                memset(&w->cell_edges, 0, sizeof(w->cell_edges));
            } else if (strcmp(t, "tcPr") == 0) {
                w->in_tcpr = m.type == MT_OPEN;
            } else if (w->in_tcpr) {
                if (strcmp(t, "gridSpan") == 0) {
                    w->span = attr_int(&m, "w:val", 1);
                } else if (strcmp(t, "vMerge") == 0) {   /* no value: continues the cell above */
                    w->cell_merge = !(mu_attr(&m, "w:val", v, sizeof(v)) && strcmp(v, "restart") == 0);
                } else if (strcmp(t, "textDirection") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->cell_dir = !strcmp(v, "btLr") ? PD_CELLTEXT_BTLR : !strcmp(v, "tbRl") || !strcmp(v, "tbRlV") ||
                                  !strcmp(v, "rl") || !strcmp(v, "rlV") ? PD_CELLTEXT_TBRL : PD_CELLTEXT_LRTB;
                } else if (strcmp(t, "tcW") == 0) {
                    if (!mu_attr(&m, "w:type", v, sizeof(v)) || strcmp(v, "dxa") == 0) {
                        w->cell_w = twips(attr_int(&m, "w:w", 0));
                        w->cell_w = w->cell_w > 0 ? w->cell_w : 0;
                    } else if (strcmp(v, "pct") == 0 && mu_attr(&m, "w:w", v, sizeof(v))) {
                        double pct = strchr(v, '%') ? atof(v) * 10 : atof(v) / 5;

                        w->cell_wpct = (int32_t)(pct < 0 ? 0 : pct > 1000 ? 1000 : pct);
                    }
                } else if (strcmp(t, "vAlign") == 0 && mu_attr(&m, "w:val", v, sizeof(v))) {
                    w->cell_valign = strcmp(v, "center") == 0 ? 1 : strcmp(v, "bottom") == 0 ? 2 : 0;
                } else if (strcmp(t, "shd") == 0 && shd_color_ref(w->X, &m, &w->cell_bg, &w->cell_bg_ref)) {
                } else if (strcmp(t, "tcBorders") == 0) {
                    w->in_cb = m.type == MT_OPEN;
                } else if (w->in_cb && (!strcmp(t, "tl2br") || !strcmp(t, "tr2bl"))) {     /* corner to corner */
                    int e = t[1] == 'r';
                    uint32_t ref = 0;

                    w->cell_diag[e] = 0;
                    w->cell_diag_c[e] = 0xFF000000u;

                    if (mu_attr(&m, "w:val", v, sizeof(v)) && strcmp(v, "nil") != 0 && strcmp(v, "none") != 0) {
                        w->cell_diag[e] = (pd_sp)((int64_t)attr_int(&m, "w:sz", 4) * 65536 / 8);
                        w->cell_diag[e] = w->cell_diag[e] > 0 ? w->cell_diag[e] : PD_PT(0.25);
                        wcolor_ref(w->X, &m, "w:color", "w:themeColor", "w:themeTint", "w:themeShade",
                                   &w->cell_diag_c[e], &ref);
                        w->cell_diag_c[e] = w->cell_diag_c[e] ? w->cell_diag_c[e] : 0xFF000000u;
                    }
                } else if (w->in_cb) {
                    edge_elem(w->X, &m, t, &w->cell_edges);
                }
            }

            continue;
        }

        /* closing tags */
        if (strcmp(t, "p") == 0 && w->in_p) {
            dw_begin_para(w);   /* an empty paragraph is still one */

            if (w->mcp.mask || w->mstyle[0]) {  /* sized by its mark */
                pd_char_props keep = w->rcp;
                char ks[64];

                memcpy(ks, w->rstyle, sizeof(ks));
                w->rcp = w->mcp;
                memcpy(w->rstyle, w->mstyle, sizeof(ks));
                dw_apply_run(w);
                bld_mark_format(X->b);
                w->rcp = keep;
                memcpy(w->rstyle, ks, sizeof(ks));
            }

            bld_end_para(X->b);

            if (w->frame_fl) {
                dw_frame_end(w);
            }

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
        } else if (strcmp(t, "footnotePr") == 0 || strcmp(t, "endnotePr") == 0) {
            w->in_notepr = 0;
        } else if (strcmp(t, "pgBorders") == 0) {
            w->in_pgb = 0;
        } else if (strcmp(t, "cols") == 0) {
            w->in_cols = 0;
        } else if (strcmp(t, "sectPr") == 0) {
            w->in_sect = 0;
            w->in_pgb = w->in_cols = 0;

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
            w->in_mrpr = 0;
        } else if (w->in_mrpr) {
            /* inside the mark's properties */
        } else if (strcmp(t, "t") == 0 || strcmp(t, "delText") == 0) {
            w->in_t = 0;
        } else if (strcmp(t, "instrText") == 0 || strcmp(t, "delInstrText") == 0) {
            w->in_instr = 0;
        } else if ((!strcmp(t, "ins") || !strcmp(t, "del") || !strcmp(t, "moveTo") || !strcmp(t, "moveFrom")) &&
                   !w->in_rpr) {
            w->rev = 0;
        } else if (strcmp(t, "positionH") == 0) {
            w->in_posh = 0;
        } else if (strcmp(t, "positionV") == 0) {
            w->in_posv = 0;
            w->in_voffset = 0;
        } else if (strcmp(t, "align") == 0) {
            w->in_align = 0;
        } else if (strcmp(t, "posOffset") == 0) {
            w->in_offset = 0;
            w->in_voffset = 0;
        } else if ((strcmp(t, "object") == 0 || strcmp(t, "pict") == 0) && w->in_vml) {
            if (w->blip[0] && w->cx > 0 && w->cy > 0) {
                w->anchor = 0;      /* in the line, as Word shows an object */
                w->drawing_res = 0;
                dw_image(w);
            } else if (w->tbx || w->art[0]) {
                dw_vml_float(w);    /* a text box, WordArt */
            }

            w->tbx = NULL;
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
                w->pend_fl[k].rev = deleted_rev(X, w->rev);
                w->pend_fl[k].has_off = w->posh_has_off && w->posh_align < 0;
                w->pend_fl[k].off_x = (pd_sp)(w->posh_off * 65536 / 12700) - (w->posh_page ? X->margin_left : 0);
                w->pend_fl[k].off_y = (pd_sp)(w->posv_off * 65536 / 12700);
                w->pend_fl[k].off_from = w->posv_para >= 2 ? w->posv_para - 1 : 0;   /* from the page, its margin */

                if (w->posv_para <= 1 && (w->started || !w->posv_para)) {
                    w->pend_fl[k].off_y = 0;    /* from a paragraph only when it is the one after it */
                }
                w->pend_fl[k].tbx = w->tbx;
                w->pend_fl[k].tbn = w->tbn;

                if (!w->started) {
                    dw_floats(w);
                }
            } else if (w->in_drawing && w->tbx && !w->anchor && w->cx > 0 && !w->blip[0] && !w->drawing_res &&
                       w->npend_fl < 8) {
                /* a text box in the line: its own block where it is, as wide as it is */
                int k = w->npend_fl++;

                memset(&w->pend_fl[k], 0, sizeof(w->pend_fl[0]));
                w->pend_fl[k].w = (pd_sp)(w->cx * 65536 / 12700);
                w->pend_fl[k].h = (pd_sp)(w->cy * 65536 / 12700);
                w->pend_fl[k].wrap = PD_WRAP_NONE;
                w->pend_fl[k].rev = deleted_rev(X, w->rev);
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
            dw_fld_end(w);
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
                dw_row_begin(w);
            }
        } else if (strcmp(t, "tbl") == 0) {
            bld_table_end(X->b);

            if (w->ntab >= 1 && w->ntab <= 8 && w->tabs[w->ntab - 1].fl) {  /* floating: as wide as the table */
                pd_doc_set_float_props(X->b->d, w->tabs[w->ntab - 1].fl, &w->tabs[w->ntab - 1].fp);
                bld_float_end(X->b);
            }

            w->ntab -= w->ntab > 0;
        }
    }

    for (tk = 0; tk < w->npend_fl; tk++) {
        free(w->pend_fl[tk].own);
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
    return pd_docx_import_ex(d, s, n, 0);
}

/* every paragraph and table style of the document's made Parade's, whether used or not (a template's) */
static void dx_define_all_styles(dxi* X, pd_bld* b) {
    int i;

    for (i = 0; i < X->nstyles; i++) {
        const dstyle_x* st = &X->styles[i];
        char low[64], as[16] = "";
        size_t k;

        if (st->type != 1 || !strcmp(st->id, X->def_pstyle)) {
            continue;
        }

        for (k = 0; k + 1 < sizeof(low) && st->name[k]; k++) {
            low[k] = (char)tolower((unsigned char)st->name[k]);
        }

        low[k] = '\0';

        if (!strncmp(low, "heading ", 8) && low[8] >= '1' && low[8] <= '6' && !low[9]) {
            snprintf(as, sizeof(as), "Heading %c", low[8]);
        } else if (!strcmp(low, "title") || !strcmp(low, "quote") || !strcmp(low, "caption")) {
            snprintf(as, sizeof(as), "%c%.7s", toupper((unsigned char)low[0]), low + 1);
        } else if (!strncmp(low, "heading", 7) || strstr(low, "quote") || !strcmp(low, "block text") ||
                   strstr(low, "source") || strstr(low, "code") || strstr(low, "preformatted") ||
                   !strcmp(low, "plain text")) {
            continue;   /* read as one of Parade's own, by role */
        }

        custom_style_define(X, b, st->id, as[0] ? as : NULL);
    }

    for (i = 0; i < X->ntstyles; i++) {
        dx_table_style_define(X, b, X->tstyles[i].id, 0);
    }
}

pd_status pd_docx_import_ex(pd_doc* d, const unsigned char* s, size_t n, int all_styles) {
    dxi X;
    pd_bld b;
    char* xml, *theme_xml = NULL;
    size_t len = 0, theme_len = 0;
    pd_status st;

    memset(&X, 0, sizeof(X));
    theme_defaults(X.theme_clr);
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
        theme_xml = xml;    /* kept with the document, for what writes it again (charts take their colours there) */
        theme_len = len;
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
        free(X.nums);
        free(X.abss);
        free(X.notes_xml);
        free(X.notes);
        free(X.en_xml);
        free(X.en);
        free(theme_xml);
        return PD_ERR_FORMAT;
    }

    {   /* SmartArt: the shapes it is drawn with */
        size_t n2 = 0, nr = 0;
        char* rels = NULL, *x2 = pd_docx_smartart(s, n, xml, len, &n2, &rels, &nr);

        if (x2) {
            free(xml);
            xml = x2;
            len = n2;
        }

        if (rels) {     /* its pictures' */
            read_rels(&X, rels, nr);
            free(rels);
        }
    }

    {   /* the first section's left margin, for the pictures placed from the page's edge before its end is read */
        static const char key[] = "<w:pgMar ";
        size_t i, j;

        for (i = 0; i + sizeof(key) - 1 < len && memcmp(xml + i, key, sizeof(key) - 1); i++) {
        }

        for (j = i; j + 8 < len && xml[j] != '>' && memcmp(xml + j, " w:left=\"", 9); j++) {
        }

        if (j + 9 < len && xml[j] != '>') {
            X.margin_left = twips(atoi(xml + j + 9));
        }
    }

    bld_init(&b, d);
    X.b = &b;
    read_fonts(&X);

    keep_read(&X.z, d);

    if (theme_xml) {
        pd_res_id tr;

        pd_doc_add_resource(d, THEME_MIME, theme_xml, theme_len, &tr);
        free(theme_xml);
        theme_xml = NULL;
        dx_theme_set(&X, d);
    }

    if (all_styles) {
        dx_define_all_styles(&X, &b);
    }

    {   /* the pages' colour, before the body */
        pd_markup pm;

        mu_init(&pm, xml, len, 0);

        while (mu_next(&pm) != MT_END && strcmp(mu_local(pm.name), "body") != 0) {
            if ((pm.type == MT_OPEN || pm.type == MT_EMPTY) && !strcmp(mu_local(pm.name), "background")) {
                uint32_t ref = 0;

                wcolor_ref(&X, &pm, "w:color", "w:themeColor", "w:themeTint", "w:themeShade", &X.page_color, &ref);
                break;
            }
        }
    }

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

        dx_ref_results(&X, d);
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
    free(X.nums);
    free(X.abss);
    free(X.notes_xml);
    free(X.notes);
    free(X.en_xml);
    free(X.en);
    return st;
}

pd_status pd_docx_drawing_rebuild(pd_doc* doc, pd_res_id drawing, const char* xml, size_t len, pd_res_id* out) {
    const char* mime = NULL;
    const void* data = NULL;
    size_t dlen = 0;
    pj_doc* jd;
    const pj_node* root, *th;
    dxi* X;
    pd_bld b;
    dw w;
    pd_markup m;
    int k, canvas = -1, nmap = 0, *map = NULL;
    char* clean = NULL;
    size_t clen = 0;
    pd_status st = PD_ERR_FORMAT;

    if (!doc || !xml || !out) {
        return PD_ERR_ARG;
    }

    *out = 0;

    if (pd_doc_resource(doc, drawing, &mime, &data, &dlen) != PD_OK ||
            strcmp(mime, "application/vnd.parade.drawing+json") != 0 || (jd = pj_parse(data, dlen, 0, NULL)) == NULL) {
        return PD_ERR_ARG;
    }

    root = pj_root(jd);
    X = (dxi*)calloc(1, sizeof(dxi));

    if (!X || !root || !pj_get(root, "xml")) {
        free(X);
        pj_free(jd);
        return X ? PD_ERR_ARG : PD_ERR_NOMEM;
    }

    memset(&b, 0, sizeof(b));
    b.d = doc;
    X->b = &b;
    X->rebuild = root;
    theme_defaults(X->theme_clr);
    th = pj_get(root, "theme");

    for (k = 0; th && k < 12; k++) {
        if (pj_at(th, k)) {
            X->theme_clr[k] = (uint32_t)pj_int_or(pj_at(th, k), 0);
        }
    }

    {   /* <!--pd-story:N--> in a text box: the editor's mark of whose story it is; taken out of what is kept */
        static const char mk[] = "<!--pd-story:", tb[] = "<w:txbxContent";
        const char* p = xml, *e = xml + len, *q;
        int has = dw_memmem(xml, len, mk, sizeof(mk) - 1) != NULL;

        clean = (char*)malloc(len + 1);
        map = (int*)malloc(sizeof(int) * (len / 16 + 1));

        if (!clean || !map) {
            free(clean);
            free(map);
            free(X);
            pj_free(jd);
            return PD_ERR_NOMEM;
        }

        while (p < e) {
            if (*p == '<' && (size_t)(e - p) >= sizeof(tb) - 1 && !memcmp(p, tb, sizeof(tb) - 1) &&
                    (p[sizeof(tb) - 1] == '>' || p[sizeof(tb) - 1] == ' ' || p[sizeof(tb) - 1] == '/')) {
                const char* end = dw_memmem(p, (size_t)(e - p), "</w:txbxContent>", 16);
                const char* m2 = dw_memmem(p, (size_t)((end ? end : e) - p), mk, sizeof(mk) - 1);

                map[nmap++] = m2 ? atoi(m2 + sizeof(mk) - 1) : -1;
            }

            if (*p == '<' && (size_t)(e - p) >= sizeof(mk) - 1 && !memcmp(p, mk, sizeof(mk) - 1) &&
                    (q = dw_memmem(p, (size_t)(e - p), "-->", 3)) != NULL) {
                p = q + 3;
                continue;
            }

            clean[clen++] = *p++;
        }

        if (has) {
            X->rebuild_map = map;
            X->rebuild_nmap = nmap;
        }

        xml = clean;
        len = clen;
    }

    memset(&w, 0, sizeof(w));
    w.X = X;
    w.cx = (long long)(pj_int_or(pj_get(root, "w"), 0) * 12700LL / 65536);    /* the drawing's extent, in EMU */
    w.cy = (long long)(pj_int_or(pj_get(root, "h"), 0) * 12700LL / 65536);
    mu_init(&m, xml, len, 0);

    while (mu_next(&m) != MT_END) {     /* to the canvas's or group's own opening tag */
        const char* t = mu_local(m.name);

        if (m.type == MT_OPEN && (!strcmp(t, "wpc") || !strcmp(t, "wgp"))) {
            canvas = !strcmp(t, "wpc");
            break;
        }
    }

    if (canvas >= 0 && w.cx > 0 && w.cy > 0) {
        dw_drawing_group(&w, &m, canvas);
        *out = w.drawing_res;
        st = *out ? PD_OK : PD_ERR_FORMAT;
    }

    bld_end_para(&b);
    pb_free(&b.pend);
    free(clean);
    free(map);
    free(X);
    pj_free(jd);
    return st;
}

/* ------------------------------------------------------------------ */
/* the zip reader and writer, for the other OOXML packages (pd_pptx.c) */
/* ------------------------------------------------------------------ */

pd_status pd_theme_read(const void* data, size_t len, pd_theme* out) {
    static const char* const where[] = { "theme/theme/theme1.xml", "word/theme/theme1.xml", "ppt/theme/theme1.xml" };
    zipr z;
    size_t k, n = 0;
    char* xml = NULL;
    int ok = 0;

    if (!data || !out) {
        return PD_ERR_ARG;
    }

    memset(&z, 0, sizeof(z));

    if (zip_open(&z, (const unsigned char*)data, len)) {
        return PD_ERR_FORMAT;
    }

    for (k = 0; k < sizeof(where) / sizeof(where[0]) && !xml; k++) {
        xml = (char*)zip_read(&z, where[k], &n);
    }

    if (xml) {
        ok = dx_theme_of_xml(xml, n, out);
        free(xml);
    }

    free(z.e);
    return ok ? PD_OK : PD_ERR_FORMAT;
}

unsigned char* pd_zip_get(const unsigned char* zip, size_t n, const char* name, size_t* len) {
    zipr z;
    unsigned char* out;

    if (zip_open(&z, zip, n) != 0) {
        return NULL;
    }

    out = zip_read(&z, name, len);
    free(z.e);
    return out;
}

void* pd_zipw_new(pd_buf* o) {
    zipw* z = (zipw*)calloc(1, sizeof(zipw));

    if (z) {
        z->o = o;
    }

    return z;
}

int pd_zipw_add(void* z, const char* name, const void* data, size_t len) {
    return zip_add((zipw*)z, name, data, len);
}

void pd_zipw_finish(void* z) {
    zip_finish((zipw*)z);
    free(z);
}
