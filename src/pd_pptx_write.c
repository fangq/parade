/*
 * PowerPoint (.pptx) written: each page that is a canvas (a section whose first block is a drawing in front of the
 * text from the page's corner, as the .pptx reader and an editor's slides make them) a slide of the canvas's shapes,
 * from the DrawingML the drawing keeps -- Word's wps/wpg/pic made PowerPoint's sp/grpSp/cxnSp/pic, the text boxes'
 * text from their stories as they are now -- or, when it keeps none, from what the drawing draws. A section that is
 * no canvas is a slide of one text box holding its text. A document read from a .pptx goes back on the masters and
 * layouts it came with (what the reader kept of them), the shapes they draw left to them; another gets one master,
 * one blank layout and the document's theme. The slides are as big as the first page. No timestamps: the same
 * document gives the same bytes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_conv.h"
#include "pd_json.h"
#include "pd_xdoc.h"

#define EMU(sp) ((long long)(sp) * 12700 / 65536 + (((long long)(sp) * 12700 % 65536) * 2 >= 65536 ? 1 : 0))
#define MAX_MEDIA 256

static const char NS[] = "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                         "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"";
static const char DECL[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
static const char REL[] = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
static const char TABLE_STYLES[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<a:tblStyleLst "
                                   "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                                   "def=\"{5C22544A-7EE6-4342-B048-85BDC9FD1C3A}\"/>";
static const char* const SLOT[12] = { "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3", "accent4",
                                      "accent5", "accent6", "hlink", "folHlink"
                                    };

typedef struct {
    pd_res_id res;
    char name[32];              /* in ppt/media */
} pmedia;

typedef struct {
    const pd_doc* d;
    pd_buf* o;                  /* the slide being written */
    pd_buf rels;                /* its relationships */
    int nrel;
    pd_res_id srel_res[MAX_MEDIA];  /* its pictures: their relationship ids, rId(srel_id) */
    int srel_id[MAX_MEDIA], nsrel;
    pmedia media[MAX_MEDIA];
    int nmedia;
    int slide_no, next_id, nfld;
    long long sw, sh;
    /* the kept XML being written */
    const xdoc* x;
    int* ids;                   /* each cNvPr's new id (by node), 0 none */
    const pj_node* krels;       /* the drawing's pictures: relationship id -> resource */
    pd_block_id st[256];        /* its text boxes' stories in the order of their w:txbxContent */
    int nst, next_st;
    char link[16];              /* the hyperlink the text is in: its relationship id, "" none */
    int kept;                   /* the masters and layouts the presentation came with are written (else one blank) */
    const unsigned char* kdata; /* they, as the reader kept them (PD_PPTX_KEEP_MIME) */
    char clr_ovr[256];          /* the slide being written: its own colour map's attributes ("" the master's) */
    size_t klen;
    int own_bg, has_rec;        /* the slide being written: it has a background of its own; what the reader saw */
    long rec_bg;                /* the background colour it read (with has_rec) */
    const char* pfx;            /* before the names of the parts written beside kept ones ("pd"), else "" */
    pd_numbers nb;              /* the numbers fields and notes show */
    struct {
        pd_res_id chart, data;  /* the chart's part, its workbook (0 none) */
        char rid[64], link[512];    /* the workbook's relationship id in the part, a linked one's file */
    } charts[64];
    int ncharts;
} pw;

/* ---- small things ---- */

static void esc(pd_buf* o, const char* s, size_t n) {
    size_t i, j = 0;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char* r = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : NULL;

        if (r || (c < 32 && c != '\t' && c != '\n')) {
            pb_put(o, s + j, i - j);
            pb_puts(o, r ? r : "");
            j = i + 1;
        }
    }

    pb_put(o, s + j, n - j);
}

static void escs(pd_buf* o, const char* s) {
    esc(o, s, strlen(s));
}

/* a picture's resource as a part of ppt/media (copied in the first time), -1 when it is none Office shows */
static int media_of(pw* P, pd_res_id res) {
    const char* mime, *ext;
    const void* data;
    size_t len;
    int i;

    if (pd_doc_resource(P->d, res, &mime, &data, &len) == PD_OK && !strcmp(mime, PD_DRAWING_MIME)) {
        pj_doc* jd = pj_parse(data, len, 0, NULL);  /* a metafile played into a drawing: the metafile */

        res = jd ? (pd_res_id)pj_int_or(pj_get(pj_root(jd), "src"), 0) : 0;
        pj_free(jd);
    }

    for (i = 0; i < P->nmedia; i++) {
        if (P->media[i].res == res) {
            return i;
        }
    }

    if (!res || P->nmedia >= MAX_MEDIA || pd_doc_resource(P->d, res, &mime, &data, &len) != PD_OK) {
        return -1;
    }

    ext = strstr(mime, "png") ? "png" : strstr(mime, "jpeg") || strstr(mime, "jpg") ? "jpeg" : strstr(mime, "gif") ?
          "gif" : strstr(mime, "emf") ? "emf" : strstr(mime, "wmf") ? "wmf" : strstr(mime, "bmp") ? "bmp" :
          strstr(mime, "tif") ? "tiff" : NULL;

    if (!ext) {
        return -1;
    }

    P->media[P->nmedia].res = res;
    snprintf(P->media[P->nmedia].name, sizeof(P->media[0].name), "%simage%d.%s", P->pfx, P->nmedia + 1,
             ext);
    return P->nmedia++;
}

/* the slide's relationship to a picture: its id (rIdN), 0 when there is no picture */
static int pic_rel(pw* P, pd_res_id res) {
    int m = media_of(P, res), i;

    if (m < 0) {
        return 0;
    }

    for (i = 0; i < P->nsrel; i++) {
        if (P->srel_res[i] == P->media[m].res) {
            return P->srel_id[i];
        }
    }

    if (P->nsrel >= MAX_MEDIA) {
        return 0;
    }

    P->nrel++;
    pb_printf(&P->rels, "<Relationship Id=\"rId%d\" Type=\"%simage\" Target=\"../media/%s\"/>", P->nrel, REL,
              P->media[m].name);
    P->srel_res[P->nsrel] = P->media[m].res;
    P->srel_id[P->nsrel++] = P->nrel;
    return P->nrel;
}

static void put_colour(pd_buf* o, uint32_t c, uint32_t ref) {
    int32_t slot, mod, off;
    unsigned a = c >> 24;

    if (ref && pd_theme_color_parts(ref, &slot, &mod, &off) && slot >= 0 && slot < 12) {
        pb_printf(o, "<a:schemeClr val=\"%s\"", SLOT[slot]);

        if (mod == 100000 && off == 0) {
            pb_puts(o, "/>");
            return;
        }

        pb_putc(o, '>');

        if (mod != 100000) {
            pb_printf(o, "<a:lumMod val=\"%d\"/>", (int)mod);
        }

        if (off) {
            pb_printf(o, "<a:lumOff val=\"%d\"/>", (int)off);
        }

        pb_puts(o, "</a:schemeClr>");
        return;
    }

    pb_printf(o, "<a:srgbClr val=\"%06X\"", (unsigned)(c & 0xFFFFFF));

    if (a && a < 255) {
        pb_printf(o, "><a:alpha val=\"%d\"/></a:srgbClr>", (int)(a * 100000 / 255));
    } else {
        pb_puts(o, "/>");
    }
}

/* a fill from an ARGB colour of a drawing: none when it is transparent */
static void put_fill(pd_buf* o, uint32_t c) {
    if (c >> 24) {
        pb_puts(o, "<a:solidFill>");
        put_colour(o, c, 0);
        pb_puts(o, "</a:solidFill>");
    } else {
        pb_puts(o, "<a:noFill/>");
    }
}

/* ---- what the reader kept of the package ---- */

#define PD_PPTX_KEEP_MIME "application/vnd.parade.pptx-kept-parts"

typedef struct {
    const char* name, *ctype, *src, *extra;
    const unsigned char* data;
    size_t len;
} pkept;

/* the next entry of the kept parts at *pos (pd_pptx.c's pkeep_entry); 0 at their end or where they are broken */
static int kept_next(const pw* P, size_t* pos, pkept* e) {
    const unsigned char* p = P->kdata;
    size_t n = P->klen, i = *pos, len;
    const char* f[4];
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
    e->extra = f[3];
    e->data = p + i;
    e->len = len;
    *pos = i + len;
    return 1;
}

/* a kept entry by its source ("map", "pres"), 0 none */
static int kept_find(const pw* P, const char* src, pkept* e) {
    size_t pos = 0;

    while (kept_next(P, &pos, e)) {
        if (!strcmp(e->src, src)) {
            return 1;
        }
    }

    return 0;
}

/* the line of the reader's map for a key (a section's block id as text, or "theme"): its fields after the key, tab
   separated, into f (up to nf, each up to 256 bytes); how many, 0 when there is no such line */
static int kept_line(const pw* P, const char* key, char f[][256], int nf) {
    pkept e;
    const char* s, *end;
    size_t kl = strlen(key);

    if (!kept_find(P, "map", &e)) {
        return 0;
    }

    for (s = (const char*)e.data, end = s + e.len; s < end;) {
        const char* nl = memchr(s, '\n', (size_t)(end - s));
        const char* le = nl ? nl : end;

        if ((size_t)(le - s) > kl && !memcmp(s, key, kl) && s[kl] == '\t') {
            const char* q = s + kl + 1;
            int n = 0;

            while (n < nf && q <= le) {
                const char* t = memchr(q, '\t', (size_t)(le - q));
                const char* fe = t ? t : le;

                snprintf(f[n++], 256, "%.*s", (int)(fe - q), q);
                q = fe + 1;

                if (!t) {
                    break;
                }
            }

            return n;
        }

        s = le + 1;
    }

    return 0;
}

/* a slide's kept tail (its transition and timing as read) by its section's key: the root's namespaces it needs
   (only those NS lacks) into ns, the XML into *xml and *xn; 0 when it has none */
static int kept_tail(const pw* P, const char* key, pd_buf* ns, const char** xml, size_t* xn) {
    size_t pos = 0;
    pkept e;

    while (kept_next(P, &pos, &e)) {
        if (!strcmp(e.src, "tail") && !strcmp(e.extra, key)) {
            const char* p = (const char*)e.data, *nl = memchr(p, '\n', e.len), *q;

            if (!nl) {
                return 0;
            }

            for (q = p; q < nl;) {  /* each ' name="value"' of the line: kept unless NS declares it already */
                const char* a = q + 1, *eq = memchr(a, '=', (size_t)(nl - a)), *v1, *v2;

                if (!eq || eq + 1 >= nl) {
                    break;
                }

                v1 = eq + 1;
                v2 = memchr(v1 + 1, *v1, (size_t)(nl - v1 - 1));

                if (!v2) {
                    break;
                }

                {
                    char name[64];

                    snprintf(name, sizeof(name), "%.*s=", (int)(eq - a), a);

                    if (!strstr(NS, name)) {
                        pb_put(ns, q, (size_t)(v2 + 1 - q));
                    }
                }

                q = v2 + 1;
            }

            *xml = nl + 1;
            *xn = e.len - (size_t)(nl + 1 - p);

            while (*xn && (*xml)[*xn - 1] == '\0') {
                (*xn)--;
            }

            return 1;
        }
    }

    return 0;
}

/* the first line of the map after the theme's: the layout the first page was on, into out ("" none) */
static void kept_first_layout(const pw* P, char* out, size_t cap) {
    pkept e;
    const char* s, *end;

    out[0] = '\0';

    if (!kept_find(P, "map", &e)) {
        return;
    }

    for (s = (const char*)e.data, end = s + e.len; s < end;) {
        const char* nl = memchr(s, '\n', (size_t)(end - s));
        const char* le = nl ? nl : end, *t1 = memchr(s, '\t', (size_t)(le - s));
        const char* t2 = t1 ? memchr(t1 + 1, '\t', (size_t)(le - t1 - 1)) : NULL;

        if (t1 && t2 && strncmp(s, "theme\t", 6) && t2 > t1 + 1) {
            snprintf(out, cap, "%.*s", (int)(t2 - t1 - 1), t1 + 1);
            return;
        }

        s = le + 1;
    }
}


static void zadd(void* z, const char* name, const pd_buf* b, int* err);

/* ---- speaker notes ---- */

/* the notes story of slide (section) sec, 0 none */
static pd_block_id slide_notes(const pd_doc* d, pd_block_id sec) {
    pd_section_props sp;

    return pd_doc_section_props(d, sec, &sp) == PD_OK ? sp.notes : 0;
}

/* whether any slide of d has notes */
static int any_notes(const pd_doc* d, int nslides) {
    int s;

    for (s = 0; s < nslides; s++) {
        if (slide_notes(d, pd_doc_child(d, pd_doc_root(d), s))) {
            return 1;
        }
    }

    return 0;
}

/* slide n's notes slide (ppt/notesSlides/notesSlideN.xml and its relationships): its slide's picture and its body,
   a paragraph for each of the story's */
static void put_notes_slide(pw* P, void* z, int n, pd_block_id story, int* err) {
    const pd_doc* d = P->d;
    pd_buf b;
    pd_block_info bi;
    char name[96];
    int32_t k;

    memset(&b, 0, sizeof(b));
    pb_puts(&b, DECL);
    pb_printf(&b, "<p:notes %s><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/>"
              "</p:nvGrpSpPr><p:grpSpPr/><p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image Placeholder 1\"/><p:cNvSpPr>"
              "<a:spLocks noGrp=\"1\" noRot=\"1\" noChangeAspect=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"sldImg\"/></p:nvPr>"
              "</p:nvSpPr><p:spPr/></p:sp><p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Notes Placeholder 2\"/><p:cNvSpPr>"
              "<a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/>"
              "<p:txBody><a:bodyPr/><a:lstStyle/>", NS);

    if (pd_doc_block_info(d, story, &bi) == PD_OK) {
        for (k = 0; k < bi.child_count; k++) {
            const char* t;
            uint32_t len = 0, i, st = 0;
            pd_block_id para = pd_doc_child(d, story, k);

            pb_puts(&b, "<a:p>");

            if (pd_doc_para_text(d, para, &t, &len) == PD_OK) {
                for (i = 0; i <= len; i++) {    /* runs between its line breaks; objects left out */
                    if (i == len || t[i] == '\n' || (i + 2 < len && (unsigned char)t[i] == 0xEF &&
                                                     (unsigned char)t[i + 1] == 0xBF && (unsigned char)t[i + 2] == 0xBC)) {
                        if (i > st) {
                            char* seg = (char*)malloc(i - st + 1);

                            if (seg) {
                                memcpy(seg, t + st, i - st);
                                seg[i - st] = '\0';
                                pb_puts(&b, "<a:r><a:rPr lang=\"en-US\" dirty=\"0\"/><a:t>");
                                escs(&b, seg);
                                pb_puts(&b, "</a:t></a:r>");
                                free(seg);
                            }
                        }

                        if (i < len && t[i] == '\n') {
                            pb_puts(&b, "<a:br><a:rPr lang=\"en-US\"/></a:br>");
                        }

                        st = i < len && t[i] != '\n' ? i + 3 : i + 1;
                        i = i < len && t[i] != '\n' ? i + 2 : i;
                    }
                }
            }

            pb_puts(&b, "</a:p>");
        }
    }

    pb_puts(&b, "</p:txBody></p:sp></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>");
    snprintf(name, sizeof(name), "ppt/notesSlides/notesSlide%d.xml", n);
    zadd(z, name, &b, err);
    b.n = 0;
    pb_puts(&b, DECL);
    pb_printf(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"%snotesMaster\" Target=\"../notesMasters/notesMaster1.xml\"/>"
              "<Relationship Id=\"rId2\" Type=\"%sslide\" Target=\"../slides/slide%d.xml\"/></Relationships>", REL, REL, n);
    snprintf(name, sizeof(name), "ppt/notesSlides/_rels/notesSlide%d.xml.rels", n);
    zadd(z, name, &b, err);
    pb_free(&b);
}

/* the notes master the notes slides are on (its picture of the slide above, the notes below), and its theme */
static void put_notes_master(pw* P, void* z, int* err) {
    pd_buf b;

    memset(&b, 0, sizeof(b));
    pb_puts(&b, DECL);
    pb_printf(&b, "<p:notesMaster %s><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg>"
              "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>"
              "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image Placeholder 1\"/><p:cNvSpPr><a:spLocks noGrp=\"1\" "
              "noRot=\"1\" noChangeAspect=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"sldImg\" idx=\"2\"/></p:nvPr></p:nvSpPr>"
              "<p:spPr><a:xfrm><a:off x=\"1143000\" y=\"685800\"/><a:ext cx=\"4572000\" cy=\"3429000\"/></a:xfrm>"
              "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr></p:sp><p:sp><p:nvSpPr><p:cNvPr "
              "id=\"3\" name=\"Notes Placeholder 2\"/><p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph "
              "type=\"body\" sz=\"quarter\" idx=\"3\"/></p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"685800\" "
              "y=\"4400550\"/><a:ext cx=\"5486400\" cy=\"3600450\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/>"
              "</a:prstGeom></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p>"
              "</p:txBody></p:sp></p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" "
              "accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" "
              "accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:notesStyle><a:lvl1pPr marL=\"0\" algn=\"l\" "
              "defTabSz=\"914400\" rtl=\"0\" eaLnBrk=\"1\" latinLnBrk=\"0\" hangingPunct=\"1\"><a:defRPr sz=\"1200\" "
              "kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mn-lt\"/>"
              "<a:ea typeface=\"+mn-ea\"/><a:cs typeface=\"+mn-cs\"/></a:defRPr></a:lvl1pPr></p:notesStyle>"
              "</p:notesMaster>", NS);
    zadd(z, "ppt/notesMasters/notesMaster1.xml", &b, err);
    b.n = 0;
    pb_puts(&b, DECL);
    pb_printf(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"%stheme\" Target=\"../theme/pdNotesTheme1.xml\"/></Relationships>", REL);
    zadd(z, "ppt/notesMasters/_rels/notesMaster1.xml.rels", &b, err);
    b.n = 0;
    pd_docx_theme_part(P->d, &b);
    zadd(z, "ppt/theme/pdNotesTheme1.xml", &b, err);
    pb_free(&b);
}

/* ---- text ---- */

static const char* face_of(const char* f) {
    return !strcmp(f, "serif") ? "Times New Roman" : !strcmp(f, "sans-serif") ? "Arial" : !strcmp(f, "monospace") ?
           "Courier New" : f;
}

static void put_rpr(pw* P, const pd_char_props* cp, const char* el) {
    pd_buf* o = P->o;
    long sz = (long)((cp->size * 100LL + 32768) / 65536);
    static const char* const u[] = { "none", "sng", "dbl", "heavy", "dotted", "dash", "wavy", "words" };
    int ft = cp->font_theme;

    sz = sz < 100 ? 100 : sz > 400000 ? 400000 : sz;
    pb_printf(o, "<a:%s", el);

    if (cp->lang[0]) {
        pb_puts(o, " lang=\"");
        escs(o, cp->lang);
        pb_putc(o, '"');
    }

    pb_printf(o, " sz=\"%ld\"", sz);
    pb_puts(o, cp->weight >= 600 ? " b=\"1\"" : "");
    pb_puts(o, cp->italic ? " i=\"1\"" : "");

    if (cp->underline > 0 && cp->underline < 8) {
        pb_printf(o, " u=\"%s\"", u[cp->underline]);
    }

    pb_puts(o, cp->strike ? " strike=\"sngStrike\"" : "");
    pb_puts(o, cp->small_caps ? " cap=\"small\"" : cp->caps ? " cap=\"all\"" : "");

    if (cp->letter_space) {
        pb_printf(o, " spc=\"%ld\"", (long)((cp->letter_space * 100LL) / 65536));
    }

    if (cp->shift == PD_SHIFT_SUPER) {
        pb_puts(o, " baseline=\"30000\"");
    } else if (cp->shift == PD_SHIFT_SUB) {
        pb_puts(o, " baseline=\"-25000\"");
    } else if (cp->position && cp->size > 0) {
        pb_printf(o, " baseline=\"%ld\"", (long)(cp->position * 100000LL / cp->size));
    }

    pb_puts(o, " dirty=\"0\">");

    if ((cp->color >> 24) || cp->color_theme) {
        pb_puts(o, "<a:solidFill>");
        put_colour(o, cp->color, cp->color_theme);
        pb_puts(o, "</a:solidFill>");
    }

    if (cp->background >> 24) {
        pb_puts(o, "<a:highlight>");
        put_colour(o, cp->background | 0xFF000000u, 0);
        pb_puts(o, "</a:highlight>");
    }

    if (PD_FONT_THEME_TEXT(ft) || cp->family[0]) {
        pb_puts(o, "<a:latin typeface=\"");
        escs(o, PD_FONT_THEME_TEXT(ft) == PD_FONT_THEME_MAJOR ? "+mj-lt" : PD_FONT_THEME_TEXT(ft) ==
             PD_FONT_THEME_MINOR ? "+mn-lt" : face_of(cp->family));
        pb_puts(o, "\"/>");
    }

    if (PD_FONT_THEME_EA(ft) || cp->family_ea[0]) {
        pb_puts(o, "<a:ea typeface=\"");
        escs(o, PD_FONT_THEME_EA(ft) == PD_FONT_THEME_MAJOR ? "+mj-ea" : PD_FONT_THEME_EA(ft) ==
             PD_FONT_THEME_MINOR ? "+mn-ea" : face_of(cp->family_ea));
        pb_puts(o, "\"/>");
    }

    if (PD_FONT_THEME_CS(ft) || cp->family_cs[0]) {
        pb_puts(o, "<a:cs typeface=\"");
        escs(o, PD_FONT_THEME_CS(ft) == PD_FONT_THEME_MAJOR ? "+mj-cs" : PD_FONT_THEME_CS(ft) ==
             PD_FONT_THEME_MINOR ? "+mn-cs" : face_of(cp->family_cs));
        pb_puts(o, "\"/>");
    }

    if (P->link[0] && strcmp(el, "endParaRPr")) {
        pb_printf(o, "<a:hlinkClick r:id=\"%s\"/>", P->link);
    }

    pb_printf(o, "</a:%s>", el);
}

typedef struct {
    pw* P;
    pd_block_id para;
    uint32_t skip;              /* the bytes of a bullet written as the paragraph's: left out of its text */
    int any;
    pd_char_props last;         /* the last text's: the paragraph mark's */
} runs_ctx;

static int put_span(void* user, const pd_span* s) {
    runs_ctx* R = (runs_ctx*)user;
    pw* P = R->P;
    pd_buf* o = P->o;

    if (s->cp.hidden || s->offset + s->len <= R->skip) {
        return 0;
    }

    R->last = s->cp;

    if (s->is_object) {
        const pd_inline* ob = &s->obj;

        if (ob->kind == PD_INLINE_TAB) {
            pb_puts(o, "<a:r>");
            put_rpr(P, &s->cp, "rPr");
            pb_puts(o, "<a:t>\t</a:t></a:r>");
            R->any = 1;
        } else if (ob->kind == PD_INLINE_LINK) {
            P->link[0] = '\0';

            if (ob->source && ob->source_len > 0) {     /* to the next one with no address */
                P->nrel++;
                pb_printf(&P->rels, "<Relationship Id=\"rId%d\" Type=\"%shyperlink\" Target=\"", P->nrel, REL);
                esc(&P->rels, ob->source, (size_t)ob->source_len);
                pb_puts(&P->rels, "\" TargetMode=\"External\"/>");
                snprintf(P->link, sizeof(P->link), "rId%d", P->nrel);
            }
        } else if (ob->kind == PD_INLINE_FIELD && (ob->field == PD_FIELD_PAGE || ob->field == PD_FIELD_SECTION_PAGE)) {
            pb_printf(o, "<a:fld id=\"{B6F15528-21DE-4FAA-801E-%012X}\" type=\"slidenum\">", (unsigned)++P->nfld);
            put_rpr(P, &s->cp, "rPr");
            pb_printf(o, "<a:t>%d</a:t></a:fld>", P->slide_no);
            R->any = 1;
        } else if ((ob->kind == PD_INLINE_FIELD && (ob->field == PD_FIELD_SEQ || ob->field == PD_FIELD_REF_NUMBER)) ||
                   ob->kind == PD_INLINE_FOOTNOTE) {    /* a number: as it is now, as text */
            int32_t v = ob->kind == PD_INLINE_FIELD && ob->field == PD_FIELD_REF_NUMBER ? pd_numbers_ref(&P->nb,
                        ob->target) : pd_numbers_at(&P->nb, R->para, s->offset);
            pd_char_props cp = s->cp;

            if (ob->kind == PD_INLINE_FOOTNOTE) {
                cp.shift = PD_SHIFT_SUPER;
            }

            if (v) {
                pb_puts(o, "<a:r>");
                put_rpr(P, &cp, "rPr");
                pb_printf(o, "<a:t>%d</a:t></a:r>", (int)v);
                R->any = 1;
            }
        }

        return 0;
    }

    {   /* text: a line break its own element */
        const char* t = s->text;
        uint32_t n = s->len, i, j = 0;

        if (s->offset < R->skip) {
            t += R->skip - s->offset;
            n -= R->skip - s->offset;
        }

        for (i = 0; i <= n; i++) {
            if (i == n || t[i] == '\n' || t[i] == '\v' || t[i] == '\r') {
                if (i > j) {
                    pb_puts(o, "<a:r>");
                    put_rpr(P, &s->cp, "rPr");
                    pb_puts(o, "<a:t>");
                    esc(o, t + j, i - j);
                    pb_puts(o, "</a:t></a:r>");
                }

                if (i < n) {
                    pb_puts(o, "<a:br>");
                    put_rpr(P, &s->cp, "rPr");
                    pb_puts(o, "</a:br>");
                }

                j = i + 1;
                R->any = 1;
            }
        }
    }

    return 0;
}

/* a bullet the text begins with, and a tab after it, hung to the left of the text (as the reader writes
   PowerPoint's bullets): its length in bytes, 0 none */
static uint32_t text_bullet(const char* t, uint32_t n, const pd_para_props* pp) {
    uint32_t k = 1;
    unsigned char c = n ? (unsigned char)t[0] : 0;

    if (pp->indent_first >= 0 || n < 2 || c == ' ' || c == '\t' || (c < 128 && (c < 33 || (c >= '0' && c <= '9') ||
            ((c | 32) >= 'a' && (c | 32) <= 'z')))) {
        return 0;
    }

    while (k < n && ((unsigned char)t[k] & 0xC0) == 0x80) {
        k++;
    }

    return k < n && t[k] == '\t' && k <= 4 && !(t[0] == '\xEF' && t[1] == '\xBF' && t[2] == '\xBC') ? k : 0;
}

static const char* autonum(int32_t fmt, const char* text) {
    int paren = strchr(text, ')') != NULL, both = paren && strchr(text, '(') != NULL;

    switch (fmt) {
        case PD_NUM_LOWER_ALPHA:
            return both ? "alphaLcParenBoth" : paren ? "alphaLcParenR" : "alphaLcPeriod";

        case PD_NUM_UPPER_ALPHA:
            return both ? "alphaUcParenBoth" : paren ? "alphaUcParenR" : "alphaUcPeriod";

        case PD_NUM_LOWER_ROMAN:
            return both ? "romanLcParenBoth" : paren ? "romanLcParenR" : "romanLcPeriod";

        case PD_NUM_UPPER_ROMAN:
            return both ? "romanUcParenBoth" : paren ? "romanUcParenR" : "romanUcPeriod";

        case PD_NUM_CIRCLED:
            return "circleNumDbPlain";

        default:
            return both ? "arabicParenBoth" : paren ? "arabicParenR" : strchr(text, '.') ? "arabicPeriod" :
                   "arabicPlain";
    }
}

static void put_para(pw* P, pd_block_id p) {
    pd_buf* o = P->o;
    pd_para_props pp;
    pd_block_info bi;
    pd_char_props base;
    const char* t = "";
    uint32_t n = 0, bul = 0;
    int32_t level = 0;
    int kind = pd_conv_list_kind(P->d, p, &level);
    long long marl, ind;
    runs_ctx R;
    pd_list_level lv[9];
    int32_t nl = 0;

    pd_doc_para_resolve(P->d, p, &pp);
    pd_doc_block_info(P->d, p, &bi);
    pd_doc_para_text(P->d, p, &t, &n);
    marl = EMU(pp.indent_left);
    ind = EMU(pp.indent_first);

    if (kind && pd_doc_list_info(P->d, bi.list, &nl, lv) == PD_OK && level < nl) {
        pd_para_props own;

        pd_doc_para_props(P->d, p, &own);

        if (!(own.mask & PD_PP_INDENT_LEFT)) {     /* the list level's indents */
            marl = EMU(lv[level].indent);
            ind = -EMU(lv[level].hanging);
        }
    } else {
        kind = 0;
        bul = text_bullet(t, n, &pp);
    }

    pb_puts(o, "<a:p><a:pPr");

    /* the indents always said: a placeholder's text would take its master's */
    pb_printf(o, " marL=\"%lld\"", marl > 51206400 ? 51206400 : marl < 0 ? 0 : marl);

    if (pp.indent_right > 0) {
        pb_printf(o, " marR=\"%lld\"", EMU(pp.indent_right));
    }

    if (kind && level > 0) {
        pb_printf(o, " lvl=\"%d\"", (int)(level > 8 ? 8 : level));
    }

    pb_printf(o, " indent=\"%lld\"", ind < -51206400 ? -51206400 : ind > 51206400 ? 51206400 : ind);

    pb_printf(o, " algn=\"%s\"", pp.align == PD_ALIGN_CENTER ? "ctr" : pp.align == PD_ALIGN_RIGHT ? "r" :
              pp.align == PD_ALIGN_JUSTIFY ? "just" : "l");
    pb_puts(o, pd_doc_para_rtl(P->d, p) == 1 ? " rtl=\"1\">" : ">");

    if (pp.line_spacing > 0 && pp.line_spacing != 1000) {
        pb_printf(o, "<a:lnSpc><a:spcPct val=\"%d\"/></a:lnSpc>", (int)pp.line_spacing * 100);
    }

    if (pp.space_before > 0) {
        pb_printf(o, "<a:spcBef><a:spcPts val=\"%ld\"/></a:spcBef>", (long)(pp.space_before * 100LL / 65536));
    }

    pb_printf(o, "<a:spcAft><a:spcPts val=\"%ld\"/></a:spcAft>", (long)(pp.space_after > 0 ?
              pp.space_after * 100LL / 65536 : 0));

    if (bul) {
        pb_puts(o, "<a:buFontTx/><a:buChar char=\"");
        esc(o, t, bul);
        pb_puts(o, "\"/>");
        bul++;  /* and its tab */
    } else if (kind == 1) {
        char label[32] = "";

        pd_doc_list_label(P->d, p, label, sizeof(label));
        pb_puts(o, "<a:buFontTx/><a:buChar char=\"");
        escs(o, label[0] ? label : "\xE2\x80\xA2");
        pb_puts(o, "\"/>");
    } else if (kind == 2) {
        pb_printf(o, "<a:buFontTx/><a:buAutoNum type=\"%s\"", autonum(lv[level].format, lv[level].text));
        pb_printf(o, lv[level].start > 1 ? " startAt=\"%d\"/>" : "/>", (int)lv[level].start);
    } else {
        pb_puts(o, "<a:buNone/>");
    }

    pb_puts(o, "</a:pPr>");
    memset(&R, 0, sizeof(R));
    R.P = P;
    R.para = p;
    R.skip = bul;
    P->link[0] = '\0';
    pd_conv_spans(P->d, p, put_span, &R);
    P->link[0] = '\0';
    pd_conv_base_props(P->d, p, &base);

    if (R.any) {
        base = R.last;
    } else if (n == 0) {   /* an empty line as tall as its mark */
        pd_run r;
        int32_t nr = 0;

        if (pd_doc_para_runs(P->d, p, &r, 1, &nr) == PD_OK && nr > 0) {
            pd_doc_format_resolve(P->d, p, r.format, &base);
        }
    }

    put_rpr(P, &base, "endParaRPr");
    pb_puts(o, "</a:p>");
}

static void put_blocks(pw* P, pd_block_id parent, int* nparas);

/* a block's text as paragraphs; a table's cells one after another (a text body has no tables) */
static void put_block(pw* P, pd_block_id b, int* nparas) {
    pd_block_info bi;

    if (pd_doc_block_info(P->d, b, &bi) != PD_OK) {
        return;
    }

    if (bi.kind == PD_BLOCK_PARAGRAPH) {
        put_para(P, b);
        ++*nparas;
    } else if (bi.kind != PD_BLOCK_BREAK) {
        put_blocks(P, b, nparas);
    }
}

static void put_blocks(pw* P, pd_block_id parent, int* nparas) {
    pd_block_info bi;
    int32_t k;

    if (pd_doc_block_info(P->d, parent, &bi) != PD_OK) {
        return;
    }

    for (k = 0; k < bi.child_count; k++) {
        put_block(P, pd_doc_child(P->d, parent, k), nparas);
    }
}

/* a text body's paragraphs from a story (an empty one: one empty paragraph, as a body must have) */
static void put_story(pw* P, pd_block_id story) {
    int np = 0;

    if (story) {
        put_blocks(P, story, &np);
    }

    if (!np) {
        pb_puts(P->o, "<a:p><a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p>");
    }
}

/* the one table a story holds (and empty paragraphs around it), 0 when it holds anything else */
static pd_block_id story_table(const pd_doc* d, pd_block_id story) {
    pd_block_info bi, ci;
    pd_block_id tbl = 0;
    int32_t k;

    if (!story || pd_doc_block_info(d, story, &bi) != PD_OK) {
        return 0;
    }

    for (k = 0; k < bi.child_count; k++) {
        pd_block_id c = pd_doc_child(d, story, k);

        if (pd_doc_block_info(d, c, &ci) != PD_OK) {
            return 0;
        }

        if (ci.kind == PD_BLOCK_TABLE && !tbl) {
            tbl = c;
        } else if (ci.kind != PD_BLOCK_PARAGRAPH || ci.text_length > 0) {
            return 0;
        }
    }

    return tbl;
}

/* ---- tables ---- */

#define TROWS 128
#define TCOLS PD_TABLE_MAX_COLS

static void put_line(pd_buf* o, const char* el, long long w, uint32_t c) {
    if (w > 0 && (c >> 24)) {
        pb_printf(o, "<a:%s w=\"%lld\"><a:solidFill>", el, w);
        put_colour(o, c, 0);
        pb_printf(o, "</a:solidFill></a:%s>", el);
    } else {
        pb_printf(o, "<a:%s w=\"0\"><a:noFill/></a:%s>", el, el);
    }
}

/* a table as PowerPoint's (a:tbl in a graphic frame) in the box at x, y of cx by cy: its grid, its cells' text,
   spans and merges, fills and rules */
static void put_table(pw* P, pd_block_id tbl, const char* name, long long x, long long y, long long cx, long long cy) {
    pd_buf* o = P->o;
    pd_table_props tp;
    pd_block_info ti, ri;
    pd_block_id grid[TROWS][TCOLS];
    long long colw[TCOLS], total = 0;
    int nrows = 0, ncols = 0, r, c, k;

    pd_doc_table_resolve(P->d, tbl, &tp);
    pd_doc_block_info(P->d, tbl, &ti);
    memset(grid, 0, sizeof(grid));

    for (r = 0; r < ti.child_count && r < TROWS; r++) {     /* every cell where it is in the grid */
        pd_block_id row = pd_doc_child(P->d, tbl, r);

        pd_doc_block_info(P->d, row, &ri);
        c = 0;

        for (k = 0; k < ri.child_count && c < TCOLS; k++) {
            pd_block_id cell = pd_doc_child(P->d, row, k);
            pd_cell_props cp;
            int s;

            pd_doc_cell_props(P->d, cell, &cp);

            for (s = 0; s < (cp.col_span > 1 ? cp.col_span : 1) && c < TCOLS; s++) {
                grid[r][c++] = s ? (pd_block_id)0xFFFFFFFFu : cell;   /* a spanned column: covered */
            }
        }

        ncols = c > ncols ? c : ncols;
        nrows = r + 1;
    }

    if (!nrows || !ncols) {
        return;
    }

    for (c = 0; c < ncols; c++) {
        colw[c] = c < tp.ncols && tp.col_width[c] > 0 ? EMU(tp.col_width[c]) : 0;
        total += colw[c];
    }

    for (c = 0; c < ncols; c++) {   /* columns of no width of their own: what is left of the frame, shared */
        if (!colw[c]) {
            int nz = 0, q;

            for (q = 0; q < ncols; q++) {
                nz += !colw[q];
            }

            colw[c] = cx > total ? (cx - total) / nz : 914400;
        }
    }

    pb_printf(o, "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"%d\" name=\"", P->next_id++);
    escs(o, name);
    pb_printf(o, "\"/><p:cNvGraphicFramePr><a:graphicFrameLocks noGrp=\"1\"/></p:cNvGraphicFramePr><p:nvPr/>"
              "</p:nvGraphicFramePr><p:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></p:xfrm>"
              "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\"><a:tbl>"
              "<a:tblPr%s/><a:tblGrid>", x, y, cx, cy, tp.header_rows > 0 ? " firstRow=\"1\"" : "");

    for (c = 0; c < ncols; c++) {
        pb_printf(o, "<a:gridCol w=\"%lld\"/>", colw[c]);
    }

    pb_puts(o, "</a:tblGrid>");

    for (r = 0; r < nrows; r++) {
        pd_sp h = 0;

        for (c = 0; c < ncols; c++) {   /* the row as tall as its tallest cell asks */
            pd_cell_props cp;

            if (grid[r][c] && grid[r][c] != 0xFFFFFFFFu && pd_doc_cell_props(P->d, grid[r][c], &cp) == PD_OK &&
                    cp.min_height > h) {
                h = cp.min_height;
            }
        }

        pb_printf(o, "<a:tr h=\"%lld\">", h > 0 ? EMU(h) : 370840);

        for (c = 0; c < ncols; c++) {
            pd_block_id cell = grid[r][c];
            pd_cell_props cp;
            int span = 1, down = 1;
            long long pad = EMU(tp.cell_padding), padv = EMU(tp.cell_padding_v >= 0 ? tp.cell_padding_v :
                            tp.cell_padding);
            long long bw = EMU(tp.border);

            if (cell == 0xFFFFFFFFu || !cell) {     /* covered by a span, or a row short of cells */
                pb_puts(o, cell ? "<a:tc hMerge=\"1\">" : "<a:tc>");
                pb_puts(o, "<a:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></a:txBody>"
                        "<a:tcPr/></a:tc>");
                continue;
            }

            pd_doc_cell_resolve(P->d, cell, &cp);

            while (c + span < ncols && grid[r][c + span] == 0xFFFFFFFFu) {
                span++;
            }

            while (r + down < nrows && grid[r + down][c] && grid[r + down][c] != 0xFFFFFFFFu) {
                pd_cell_props below;

                if (pd_doc_cell_props(P->d, grid[r + down][c], &below) != PD_OK || !below.merge_up) {
                    break;
                }

                down++;
            }

            pb_puts(o, cp.merge_up ? "<a:tc vMerge=\"1\"" : "<a:tc");
            pb_printf(o, span > 1 ? " gridSpan=\"%d\"" : "", span);
            pb_printf(o, down > 1 ? " rowSpan=\"%d\">" : ">", down);
            pb_puts(o, "<a:txBody><a:bodyPr/><a:lstStyle/>");
            put_story(P, cell);
            pb_printf(o, "</a:txBody><a:tcPr marL=\"%lld\" marR=\"%lld\" marT=\"%lld\" marB=\"%lld\"%s>", pad, pad,
                      padv, padv, cp.valign == 1 ? " anchor=\"ctr\"" : cp.valign == 2 ? " anchor=\"b\"" : "");

            {   /* rules: the table's grid, or the cell's own */
                static const char* const el[4] = { "lnL", "lnR", "lnT", "lnB" };
                static const int side[4] = { PD_BORDER_LEFT, PD_BORDER_RIGHT, PD_BORDER_TOP, PD_BORDER_BOTTOM };
                static const int edge[4] = { 3, 1, 0, 2 };
                int q;

                for (q = 0; q < 4; q++) {
                    if (cp.border_set & side[q]) {
                        put_line(o, el[q], (cp.border_on & side[q]) ? EMU(cp.edge_width[edge[q]] ?
                                 cp.edge_width[edge[q]] : cp.border_width) : 0, cp.border_color);
                    } else if (bw > 0) {
                        put_line(o, el[q], bw, tp.border_color);
                    } else {
                        put_line(o, el[q], 0, 0);
                    }
                }
            }

            if (cp.background >> 24) {
                pb_puts(o, "<a:solidFill>");
                put_colour(o, cp.background, cp.background_theme);
                pb_puts(o, "</a:solidFill>");
            }

            pb_puts(o, "</a:tcPr></a:tc>");
        }

        pb_puts(o, "</a:tr>");
    }

    pb_puts(o, "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>");
}

/* ---- the kept DrawingML ---- */

/* an element's qualified name (its prefix in pre, "" none), from its raw start tag */
static void qname_of(const xdoc* d, int n, char* q, size_t cap, char* pre, size_t pcap) {
    const char* s = d->s + d->v[n].a + 1;
    size_t k = 0;
    const char* colon;

    while (s[k] && s[k] != ' ' && s[k] != '\t' && s[k] != '\n' && s[k] != '\r' && s[k] != '/' && s[k] != '>' &&
            k + 1 < cap) {
        q[k] = s[k];
        k++;
    }

    q[k] = '\0';
    colon = strchr(q, ':');
    snprintf(pre, pcap, "%.*s", colon ? (int)(colon - q) : 0, q);
}

/* an element's attributes as they are, but those of a namespace a slide does not declare, and its references to
   pictures made the slide's (dropped when the picture is not there) */
static void put_attrs(pw* P, const char* a, size_t n) {
    pd_buf* o = P->o;
    size_t i = 0;

    while (i < n) {
        size_t n0, n1, v0, v1;
        char qc;

        while (i < n && (a[i] == ' ' || a[i] == '\t' || a[i] == '\n' || a[i] == '\r')) {
            i++;
        }

        for (n0 = i; i < n && a[i] != '=' && a[i] != ' ' && a[i] != '/' && a[i] != '>'; i++) {
        }

        n1 = i;

        while (i < n && a[i] != '"' && a[i] != '\'') {
            if (a[i] == '/' || a[i] == '>') {
                return;
            }

            i++;
        }

        if (i >= n || n1 == n0) {
            return;
        }

        qc = a[i++];

        for (v0 = i; i < n && a[i] != qc; i++) {
        }

        v1 = i++;

        {
            const char* colon = memchr(a + n0, ':', n1 - n0);
            size_t pl = colon ? (size_t)(colon - (a + n0)) : 0;

            if (colon && pl == 1 && a[n0] == 'r') {     /* a picture's relationship */
                const pj_node* r;
                char rid[64];
                int id = 0;

                if (v1 - v0 < sizeof(rid)) {
                    memcpy(rid, a + v0, v1 - v0);
                    rid[v1 - v0] = '\0';
                    r = P->krels ? pj_get(P->krels, rid) : NULL;
                    id = r ? pic_rel(P, (pd_res_id)pj_int_or(r, 0)) : 0;
                }

                if (id) {
                    pb_putc(o, ' ');
                    pb_put(o, a + n0, n1 - n0);
                    pb_printf(o, "=\"rId%d\"", id);
                }
            } else if (!colon || (pl == 3 && !memcmp(a + n0, "xml", 3))) {
                if (!(n1 - n0 > 5 && !memcmp(a + n0, "xmlns", 5))) {
                    pb_putc(o, ' ');
                    pb_put(o, a + n0, n1 - n0);
                    pb_puts(o, "=\"");
                    pb_put(o, a + v0, v1 - v0);
                    pb_putc(o, '"');
                }
            }
        }
    }
}

/* an element of DrawingML (a:) copied, named el when that is given; what is in other namespaces (Word's own,
   extensions) left out */
static void put_copy(pw* P, int n, const char* el) {
    const xdoc* d = P->x;
    pd_buf* o = P->o;
    char q[64], pre[16];
    int k;
    size_t at;

    qname_of(d, n, q, sizeof(q), pre, sizeof(pre));

    if (!el && (strcmp(pre, "a") || !strcmp(d->v[n].name, "extLst"))) {
        return;
    }

    el = el ? el : q;
    pb_printf(o, "<%s", el);
    put_attrs(P, d->v[n].attrs, d->v[n].alen);

    if (d->v[n].ib <= d->v[n].ia) {
        pb_puts(o, "/>");
        return;
    }

    pb_putc(o, '>');
    at = d->v[n].ia;

    for (k = d->v[n].kid; k >= 0; k = d->v[k].next) {
        pb_put(o, d->s + at, d->v[k].a - at);   /* the text before it */
        put_copy(P, k, NULL);
        at = d->v[k].b;
    }

    pb_put(o, d->s + at, d->v[n].ib - at);
    pb_printf(o, "</%s>", el);
}

/* a shape's non-visual properties: a fresh id, its name and description */
static void put_cnvpr(pw* P, int cnv, const char* fallback) {
    const xdoc* d = P->x;
    pd_buf* o = P->o;
    char v[512];
    int id = cnv >= 0 && P->ids[cnv] ? P->ids[cnv] : P->next_id++;
    static const char* const keep[] = { "descr", "title" };
    int k;

    pb_printf(o, "<p:cNvPr id=\"%d\" name=\"", id);

    if (cnv >= 0 && xd_attr(d, cnv, "name", v, sizeof(v)) && v[0]) {
        escs(o, v);
    } else {
        pb_printf(o, "%s %d", fallback, id);
    }

    pb_putc(o, '"');

    for (k = 0; cnv >= 0 && k < 2; k++) {
        if (xd_attr(d, cnv, keep[k], v, sizeof(v))) {
            pb_printf(o, " %s=\"", keep[k]);
            escs(o, v);
            pb_putc(o, '"');
        }
    }

    if (cnv >= 0 && xd_attr(d, cnv, "hidden", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true"))) {
        pb_puts(o, " hidden=\"1\"");
    }

    pb_puts(o, "/>");
}

/* a w:txbxContent's text as it is in the XML (when the drawing has no story for it): its paragraphs' runs */
static void put_wml_text(pw* P, int tc) {
    const xdoc* d = P->x;
    pd_buf* o = P->o;
    int k, np = 0;

    for (k = tc + 1; k < d->nv && d->v[k].a < d->v[tc].b; k++) {
        int r, t;
        char jc[16] = "";

        if (strcmp(d->v[k].name, "p")) {
            continue;
        }

        xd_attr(d, xd_path(d, k, "pPr/jc"), "val", jc, sizeof(jc));
        pb_printf(o, "<a:p><a:pPr algn=\"%s\"/>", !strcmp(jc, "center") ? "ctr" : !strcmp(jc, "right") ||
                  !strcmp(jc, "end") ? "r" : !strcmp(jc, "both") ? "just" : "l");

        for (r = d->v[k].kid; r >= 0; r = d->v[r].next) {
            for (t = strcmp(d->v[r].name, "r") ? -1 : d->v[r].kid; t >= 0; t = d->v[t].next) {
                pd_buf txt;

                if (!strcmp(d->v[t].name, "t")) {
                    memset(&txt, 0, sizeof(txt));
                    mu_decode(d->s + d->v[t].ia, d->v[t].ib - d->v[t].ia, &txt);
                    pb_puts(o, "<a:r><a:rPr lang=\"en-US\" dirty=\"0\"/><a:t>");
                    esc(o, txt.p ? txt.p : "", txt.n);
                    pb_puts(o, "</a:t></a:r>");
                    pb_free(&txt);
                } else if (!strcmp(d->v[t].name, "tab")) {
                    pb_puts(o, "<a:r><a:rPr lang=\"en-US\" dirty=\"0\"/><a:t>\t</a:t></a:r>");
                } else if (!strcmp(d->v[t].name, "br")) {
                    pb_puts(o, "<a:br><a:rPr lang=\"en-US\" dirty=\"0\"/></a:br>");
                }
            }
        }

        pb_puts(o, "</a:p>");
        np++;
    }

    if (!np) {
        pb_puts(o, "<a:p><a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p>");
    }
}

/* the mark the reader leaves on a slide's shape (PD_PPTX_EXT in pd_pptx.c): its pd:sp element, -1 none */
static int shape_mark(const xdoc* d, int cnv) {
    int e = cnv >= 0 ? xd_path(d, cnv, "extLst/ext/sp") : -1;
    char v[64];

    return e >= 0 && xd_attr(d, e, "xmlns:pd", v, sizeof(v)) && !strcmp(v, "urn:parade:pptx") ? e : -1;
}

/* a shape's p:nvPr: the placeholder it is, when it was one -- in a presentation of its own masters any, else those
   PowerPoint finds by their type alone (a title, a subtitle, a body, the date, footer and slide number) */
static void put_nvpr(pw* P, int cnv) {
    static const char* const lone[] = { "title", "ctrTitle", "subTitle", "body", "dt", "ftr", "sldNum" };
    int e = shape_mark(P->x, cnv), k, ok = 0;
    char type[32] = "", idx[24] = "";

    if (e >= 0 && xd_attr(P->x, e, "ph", type, sizeof(type))) {
        xd_attr(P->x, e, "idx", idx, sizeof(idx));

        for (k = 0; k < 7; k++) {
            ok |= !strcmp(type, lone[k]);
        }

        if (ok || (P->kept && idx[0])) {
            pb_puts(P->o, "<p:nvPr><p:ph");

            if (type[0]) {
                pb_puts(P->o, " type=\"");
                escs(P->o, type);
                pb_putc(P->o, '"');
            }

            if (idx[0]) {
                pb_puts(P->o, " idx=\"");
                escs(P->o, idx);
                pb_putc(P->o, '"');
            }

            pb_puts(P->o, "/></p:nvPr>");
            return;
        }
    }

    pb_puts(P->o, "<p:nvPr/>");
}

static void put_shape(pw* P, int n);

static void put_kids(pw* P, int n) {
    int k;

    for (k = P->x->v[n].kid; k >= 0; k = P->x->v[k].next) {
        put_shape(P, k);
    }
}

/* a stCxn or endCxn: the shape it names by its new id; nothing when that is not on the slide */
static void put_cxn(pw* P, int k) {
    const xdoc* d = P->x;
    long long old = xd_int(d, k, "id", -1);
    int q;
    char v[32];

    for (q = 0; q < d->nv; q++) {
        if (!strcmp(d->v[q].name, "cNvPr") && P->ids[q] && xd_attr(d, q, "id", v, sizeof(v)) && atoll(v) == old) {
            pb_printf(P->o, "<a:%s id=\"%d\" idx=\"%lld\"/>", d->v[k].name, P->ids[q], xd_int(d, k, "idx", 0));
            return;
        }
    }
}

static void put_shape(pw* P, int n) {
    const xdoc* d = P->x;
    pd_buf* o = P->o;
    const char* el = d->v[n].name;

    if (!strcmp(el, "wsp")) {
        int cnv = xd_kid(d, n, "cNvPr"), cn = xd_kid(d, n, "cNvCnPr"), csp = xd_kid(d, n, "cNvSpPr");
        int spr = xd_kid(d, n, "spPr"), st = xd_kid(d, n, "style"), bp = xd_kid(d, n, "bodyPr");
        int tx = xd_kid(d, n, "txbx"), tc = tx >= 0 ? xd_kid(d, tx, "txbxContent") : -1;
        pd_block_id story = 0, tbl;
        char v[16];

        if (tc >= 0) {  /* its text: the next story, as dx_kept_xml gives them out */
            story = P->next_st < P->nst ? P->st[P->next_st++] : 0;
        }

        if (cn >= 0) {  /* a connector */
            int k;

            pb_puts(o, "<p:cxnSp><p:nvCxnSpPr>");
            put_cnvpr(P, cnv, "Connector");
            pb_puts(o, "<p:cNvCxnSpPr>");

            for (k = d->v[cn].kid; k >= 0; k = d->v[k].next) {
                if (!strcmp(d->v[k].name, "stCxn") || !strcmp(d->v[k].name, "endCxn")) {
                    put_cxn(P, k);
                }
            }

            pb_puts(o, "</p:cNvCxnSpPr><p:nvPr/></p:nvCxnSpPr>");

            if (spr >= 0) {
                put_copy(P, spr, "p:spPr");
            } else {
                pb_puts(o, "<p:spPr/>");
            }

            if (st >= 0) {
                put_copy(P, st, "p:style");
            }

            pb_puts(o, "</p:cxnSp>");
            return;
        }

        if ((tbl = story_table(P->d, story)) != 0) {    /* a table in a box: PowerPoint's table where the box is */
            int xf = spr >= 0 ? xd_kid(d, spr, "xfrm") : -1, off = xf >= 0 ? xd_kid(d, xf, "off") : -1;
            int ext = xf >= 0 ? xd_kid(d, xf, "ext") : -1;
            char name[256] = "Table";

            if (cnv >= 0) {
                xd_attr(d, cnv, "name", name, sizeof(name));
            }

            if (cnv >= 0 && P->ids[cnv]) {
                int keep = P->next_id;

                P->next_id = P->ids[cnv];
                put_table(P, tbl, name, xd_int(d, off, "x", 0), xd_int(d, off, "y", 0), xd_int(d, ext, "cx", 0),
                          xd_int(d, ext, "cy", 0));
                P->next_id = keep;
            } else {
                put_table(P, tbl, name, xd_int(d, off, "x", 0), xd_int(d, off, "y", 0), xd_int(d, ext, "cx", 0),
                          xd_int(d, ext, "cy", 0));
            }

            return;
        }

        pb_puts(o, "<p:sp><p:nvSpPr>");
        put_cnvpr(P, cnv, "Shape");
        pb_puts(o, csp >= 0 && xd_attr(d, csp, "txBox", v, sizeof(v)) && (!strcmp(v, "1") || !strcmp(v, "true")) ?
                "<p:cNvSpPr txBox=\"1\"/>" : "<p:cNvSpPr/>");
        put_nvpr(P, cnv);
        pb_puts(o, "</p:nvSpPr>");

        if (spr >= 0) {
            put_copy(P, spr, "p:spPr");
        } else {
            pb_puts(o, "<p:spPr/>");
        }

        if (st >= 0) {
            put_copy(P, st, "p:style");
        }

        if (tc >= 0) {
            pb_puts(o, "<p:txBody>");

            if (bp >= 0) {
                put_copy(P, bp, "a:bodyPr");
            } else {
                pb_puts(o, "<a:bodyPr/>");
            }

            pb_puts(o, "<a:lstStyle/>");

            if (story) {
                put_story(P, story);
            } else {
                put_wml_text(P, tc);
            }

            pb_puts(o, "</p:txBody>");
        }

        pb_puts(o, "</p:sp>");
    } else if (!strcmp(el, "pic")) {
        int cnv = xd_path(d, n, "nvPicPr/cNvPr"), bf = xd_kid(d, n, "blipFill"), spr = xd_kid(d, n, "spPr");

        if (bf < 0) {
            return;
        }

        pb_puts(o, "<p:pic><p:nvPicPr>");
        put_cnvpr(P, cnv, "Picture");
        pb_puts(o, "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr>");
        put_nvpr(P, cnv);
        pb_puts(o, "</p:nvPicPr>");
        put_copy(P, bf, "p:blipFill");

        if (spr >= 0) {
            put_copy(P, spr, "p:spPr");
        } else {
            pb_puts(o, "<p:spPr/>");
        }

        pb_puts(o, "</p:pic>");
    } else if (!strcmp(el, "wgp") || !strcmp(el, "grpSp")) {
        int gp = xd_kid(d, n, "grpSpPr");

        pb_puts(o, "<p:grpSp><p:nvGrpSpPr>");
        put_cnvpr(P, xd_kid(d, n, "cNvPr"), "Group");
        pb_puts(o, "<p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>");

        if (gp >= 0) {
            put_copy(P, gp, "p:grpSpPr");
        } else {
            pb_puts(o, "<p:grpSpPr/>");
        }

        put_kids(P, n);
        pb_puts(o, "</p:grpSp>");
    } else if (!strcmp(el, "AlternateContent")) {   /* what a reader without the choice reads */
        int fb = xd_kid(d, n, "Fallback");

        if (fb >= 0) {
            put_kids(P, fb);
        }
    }
}

/* a canvas's background as the slide's */
static void put_background(pw* P, int bg) {
    const xdoc* d = P->x;
    int k, any = 0;

    for (k = bg >= 0 ? d->v[bg].kid : -1; k >= 0; k = d->v[k].next) {
        const char* t = d->v[k].name;

        if (!strcmp(t, "solidFill") || !strcmp(t, "gradFill") || !strcmp(t, "blipFill") || !strcmp(t, "pattFill")) {
            if (!any) {
                pb_puts(P->o, "<p:bg><p:bgPr>");
            }

            put_copy(P, k, NULL);
            any = 1;
            break;
        }
    }

    if (any) {
        pb_puts(P->o, "<a:effectLst/></p:bgPr></p:bg>");
    }
}

/* ---- what a drawing draws (one that keeps no XML) ---- */

static long long jn(const pj_node* o, const char* k) {
    return (long long)pj_int_or(pj_get(o, k), 0);
}

static void put_xfrm(pd_buf* o, const pj_node* it, long long x, long long y, long long w, long long h) {
    pb_puts(o, "<a:xfrm");

    if (jn(it, "rot")) {
        pb_printf(o, " rot=\"%lld\"", jn(it, "rot"));
    }

    pb_puts(o, jn(it, "fh") ? " flipH=\"1\"" : "");
    pb_puts(o, jn(it, "fv") ? " flipV=\"1\"" : "");
    pb_printf(o, "><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>", x, y, w > 0 ? w : 0,
              h > 0 ? h : 0);
}

static void put_items(pw* P, const pj_node* items) {
    pd_buf* o = P->o;
    const pj_node* it;

    for (it = items ? items->child : NULL; it; it = it->next) {
        long long x = EMU(jn(it, "x")), y = EMU(jn(it, "y")), w = EMU(jn(it, "w")), h = EMU(jn(it, "h"));
        uint32_t fill = (uint32_t)jn(it, "fill"), line = (uint32_t)jn(it, "line");
        const pj_node* sh = pj_get(it, "shape"), *path = pj_get(it, "path");

        if (pj_get(it, "img")) {
            int rid = pic_rel(P, (pd_res_id)jn(it, "img"));
            const pj_node* crop = pj_get(it, "crop");

            if (!rid) {
                continue;
            }

            pb_printf(o, "<p:pic><p:nvPicPr><p:cNvPr id=\"%d\" name=\"Picture %d\"/><p:cNvPicPr/><p:nvPr/></p:nvPicPr>"
                      "<p:blipFill><a:blip r:embed=\"rId%d\"/>", P->next_id, P->next_id, rid);
            P->next_id++;

            if (crop && crop->n == 4) {
                pb_printf(o, "<a:srcRect l=\"%lld\" t=\"%lld\" r=\"%lld\" b=\"%lld\"/>", (long long)pj_int_or(pj_at(crop, 0), 0),
                          (long long)pj_int_or(pj_at(crop, 1), 0), (long long)pj_int_or(pj_at(crop, 2), 0),
                          (long long)pj_int_or(pj_at(crop, 3), 0));
            }

            pb_puts(o, "<a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>");
            put_xfrm(o, it, x, y, w, h);
            pb_puts(o, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>");
        } else if ((sh && sh->type == PJ_STR) || (path && path->type == PJ_ARR)) {
            pb_printf(o, "<p:sp><p:nvSpPr><p:cNvPr id=\"%d\" name=\"Shape %d\"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr>",
                      P->next_id, P->next_id);
            P->next_id++;

            if (sh && sh->type == PJ_STR) {
                put_xfrm(o, it, x, y, w, h);
                pb_puts(o, "<a:prstGeom prst=\"");
                esc(o, sh->s, sh->len);
                pb_puts(o, "\"><a:avLst/></a:prstGeom>");
            } else {    /* a path: its box, its points in it */
                long long x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                const pj_node* q;
                int first = 1, start = 1;

                for (q = path->child; q && q->next; q = q->next->next) {
                    long long px = pj_int_or(q, 0), py = pj_int_or(q->next, 0);

                    if (px == INT32_MIN) {
                        continue;
                    }

                    px = EMU(px);
                    py = EMU(py);
                    x0 = first || px < x0 ? px : x0;
                    y0 = first || py < y0 ? py : y0;
                    x1 = first || px > x1 ? px : x1;
                    y1 = first || py > y1 ? py : y1;
                    first = 0;
                }

                put_xfrm(o, it, x0, y0, x1 - x0, y1 - y0);
                pb_printf(o, "<a:custGeom><a:avLst/><a:gdLst/><a:ahLst/><a:cxnLst/><a:rect l=\"0\" t=\"0\" r=\"r\" "
                          "b=\"b\"/><a:pathLst><a:path w=\"%lld\" h=\"%lld\"%s>", x1 - x0, y1 - y0,
                          jn(it, "closed") ? "" : " fill=\"none\"");

                for (q = path->child; q && q->next; q = q->next->next) {
                    long long px = pj_int_or(q, 0), py = pj_int_or(q->next, 0);

                    if (px == INT32_MIN) {
                        pb_puts(o, !start && jn(it, "closed") ? "<a:close/>" : "");
                        start = 1;
                        continue;
                    }

                    pb_printf(o, "<a:%s><a:pt x=\"%lld\" y=\"%lld\"/></a:%s>", start ? "moveTo" : "lnTo", EMU(px) - x0,
                              EMU(py) - y0, start ? "moveTo" : "lnTo");
                    start = 0;
                }

                pb_printf(o, "%s</a:path></a:pathLst></a:custGeom>", !start && jn(it, "closed") ? "<a:close/>" : "");
            }

            put_fill(o, path && !jn(it, "closed") ? 0 : fill);

            if ((line >> 24) && jn(it, "lw") > 0) {
                pb_printf(o, "<a:ln w=\"%lld\">", EMU(jn(it, "lw")));
                put_fill(o, line);
                pb_puts(o, "</a:ln>");
            } else {
                pb_puts(o, "<a:ln><a:noFill/></a:ln>");
            }

            pb_puts(o, "</p:spPr></p:sp>");
        } else if (pj_get(it, "story")) {
            const pj_node* ins = pj_get(it, "ins"), *an = pj_get(it, "anchor");

            pb_printf(o, "<p:sp><p:nvSpPr><p:cNvPr id=\"%d\" name=\"Text Box %d\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/>"
                      "</p:nvSpPr><p:spPr>", P->next_id, P->next_id);
            P->next_id++;
            put_xfrm(o, it, x, y, w, h);
            pb_printf(o, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr><p:txBody><a:bodyPr "
                      "wrap=\"square\" lIns=\"%lld\" tIns=\"%lld\" rIns=\"%lld\" bIns=\"%lld\" anchor=\"%s\" rtlCol=\"0\">"
                      "<a:noAutofit/></a:bodyPr><a:lstStyle/>", EMU(pj_int_or(pj_at(ins, 0), 0)),
                      EMU(pj_int_or(pj_at(ins, 1), 0)), EMU(pj_int_or(pj_at(ins, 2), 0)), EMU(pj_int_or(pj_at(ins, 3), 0)),
                      an && an->type == PJ_STR && an->len >= 3 && !memcmp(an->s, "ctr", 3) ? "ctr" :
                      an && an->type == PJ_STR && an->len >= 1 && an->s[0] == 'b' ? "b" : "t");
            put_story(P, (pd_block_id)jn(it, "story"));
            pb_puts(o, "</p:txBody></p:sp>");
        }
    }
}

/* ---- slides ---- */

/* the drawing a section's page is (its first block a float in front of the text from the page's corner, holding
   one), 0 none */
static pd_res_id section_canvas(const pd_doc* d, pd_block_id sec) {
    pd_block_id fl = pd_doc_child(d, sec, 0);
    pd_block_info bi;
    pd_float_props fp;
    pd_inline ob;
    pd_pos p;
    const char* mime;
    const void* data;
    size_t len;

    if (!fl || pd_doc_block_info(d, fl, &bi) != PD_OK || bi.kind != PD_BLOCK_FLOAT ||
            pd_doc_float_props(d, fl, &fp) != PD_OK || fp.wrap != PD_WRAP_FRONT || fp.offset_from != PD_FROM_PAGE) {
        return 0;
    }

    p.block = pd_doc_child(d, fl, 0);
    p.offset = 0;

    if (!p.block || pd_doc_inline_at(d, p, &ob) != PD_OK || ob.kind != PD_INLINE_IMAGE ||
            pd_doc_resource(d, ob.resource, &mime, &data, &len) != PD_OK || strcmp(mime, PD_DRAWING_MIME)) {
        return 0;
    }

    return ob.resource;
}

/* a top-level shape of a canvas that its slide's layout or master draws, or the background the reader made a shape
   (put back as the slide's own background, or left to its layout's) */
static int from_master(const xdoc* d, int n) {
    int cnv = xd_kid(d, n, "cNvPr"), e;
    char v[16];

    if (cnv < 0) {
        cnv = xd_path(d, n, "nvPicPr/cNvPr");
    }

    if ((e = shape_mark(d, cnv)) < 0) {
        return 0;
    }

    return (xd_attr(d, e, "from", v, sizeof(v)) && strcmp(v, "slide")) || (xd_attr(d, e, "bg", v, sizeof(v)) &&
            !strcmp(v, "1"));
}

/* a slide on a layout of its own presentation: its background when it has one of its own (a picture or a gradient
   the reader made a shape, else the canvas's colour), or when its colour is not what was read (changed since) */
static void put_kept_background(pw* P, int wpc) {
    const xdoc* d = P->x;
    int k, bg = xd_kid(d, wpc, "bg"), fill;
    char v[16], hex[16] = "";

    for (k = d->v[wpc].kid; P->own_bg && k >= 0; k = d->v[k].next) {   /* its picture or gradient */
        int cnv = xd_kid(d, k, "cNvPr") >= 0 ? xd_kid(d, k, "cNvPr") : xd_path(d, k, "nvPicPr/cNvPr");
        int e = shape_mark(d, cnv), f = -1;

        if (e < 0 || !xd_attr(d, e, "bg", v, sizeof(v)) || !xd_attr(d, e, "from", v, sizeof(v)) || strcmp(v, "slide")) {
            continue;
        }

        if (!strcmp(d->v[k].name, "pic")) {
            f = xd_kid(d, k, "blipFill");
        } else {
            f = xd_path(d, k, "spPr/gradFill");
        }

        if (f >= 0) {
            pb_puts(P->o, "<p:bg><p:bgPr>");
            put_copy(P, f, !strcmp(d->v[f].name, "blipFill") ? "a:blipFill" : NULL);
            pb_puts(P->o, "<a:effectLst/></p:bgPr></p:bg>");
            return;
        }
    }

    fill = bg >= 0 ? xd_path(d, bg, "solidFill/srgbClr") : -1;
    xd_attr(d, fill, "val", hex, sizeof(hex));

    if (P->own_bg || (P->has_rec && fill >= 0 && strtol(hex, NULL, 16) != P->rec_bg)) {
        put_background(P, bg);
    }
}

/* a canvas's shapes on the slide */
static void put_canvas(pw* P, pd_res_id res) {
    const char* mime;
    const void* data;
    size_t len;
    pj_doc* jd;
    const pj_node* root, *xml, *it;

    if (pd_doc_resource(P->d, res, &mime, &data, &len) != PD_OK || !(jd = pj_parse(data, len, 0, NULL))) {
        return;
    }

    root = pj_root(jd);
    xml = pj_get(root, "xml");
    P->krels = pj_get(root, "rels");
    P->nst = P->next_st = 0;

    for (it = pj_get(root, "items") ? pj_get(root, "items")->child : NULL; it && P->nst < 256; it = it->next) {
        if (pj_get(it, "story")) {
            P->st[P->nst++] = (pd_block_id)jn(it, "story");
        }
    }

    if (xml && xml->type == PJ_STR && xml->len > 0) {
        xdoc x;
        char* s = (char*)malloc(xml->len + 1);
        int wpc, k, ncnv = 0;

        if (s) {
            memcpy(s, xml->s, xml->len);
            s[xml->len] = '\0';
        }

        if (s && xd_parse(&x, s, xml->len) && (P->ids = (int*)calloc((size_t)x.nv, sizeof(int))) != NULL) {
            P->x = &x;

            for (k = 0; k < x.nv; k++) {    /* the shapes' ids, in their order */
                if (!strcmp(x.v[k].name, "cNvPr")) {
                    P->ids[k] = P->next_id + ncnv++;
                }
            }

            P->next_id += ncnv;
            wpc = !strcmp(x.v[0].name, "wpc") ? 0 : xd_find(&x, "wpc");
            pb_puts(P->o, "<p:cSld>");

            if (!P->kept) {
                put_background(P, wpc >= 0 ? xd_kid(&x, wpc, "bg") : -1);
            } else if (wpc >= 0) {
                put_kept_background(P, wpc);
            }

            pb_puts(P->o, "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
                    "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/>"
                    "<a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>");

            for (k = wpc >= 0 ? x.v[wpc].kid : -1; k >= 0; k = x.v[k].next) {
                if (!P->kept || !from_master(&x, k)) {
                    put_shape(P, k);
                } else {    /* left to its layout: its text boxes' stories passed over */
                    int q;

                    for (q = k; q < x.nv && x.v[q].a < x.v[k].b; q++) {
                        P->next_st += !strcmp(x.v[q].name, "txbxContent");
                    }
                }
            }

            free(P->ids);
            P->ids = NULL;
            P->x = NULL;
            xd_free(&x);
            pj_free(jd);
            return;
        }

        if (s) {
            xd_free(&x);
        }
    }

    pb_puts(P->o, "<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/>"
            "<a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>");
    put_items(P, pj_get(root, "items"));
    pj_free(jd);
}

/* a chart drawing (one read from a chart part): its number in the package (its part kept the first time), 0 none */
static int chart_of(pw* P, const pj_node* root) {
    pd_res_id cres = (pd_res_id)jn(root, "chart");
    const pj_node* n;
    int i;

    if (!cres) {
        return 0;
    }

    for (i = 0; i < P->ncharts; i++) {
        if (P->charts[i].chart == cres) {
            return i + 1;
        }
    }

    if (P->ncharts >= 64) {
        return 0;
    }

    memset(&P->charts[P->ncharts], 0, sizeof(P->charts[0]));
    P->charts[P->ncharts].chart = cres;
    P->charts[P->ncharts].data = (pd_res_id)jn(root, "data");
    n = pj_get(root, "dataRid");
    snprintf(P->charts[P->ncharts].rid, sizeof(P->charts[0].rid), "%.*s", n && n->type == PJ_STR ? (int)n->len : 0,
             n && n->type == PJ_STR ? n->s : "");
    n = pj_get(root, "dataLink");
    snprintf(P->charts[P->ncharts].link, sizeof(P->charts[0].link), "%.*s", n && n->type == PJ_STR ? (int)n->len : 0,
             n && n->type == PJ_STR ? n->s : "");
    return ++P->ncharts;
}

/* what else a canvas page holds, in front of its canvas: charts and pictures placed from the page's corner, each
   where it is (a chart a graphic frame of its part, as PowerPoint has it) */
static void put_floats(pw* P, pd_block_id sec) {
    pd_block_info si, bi;
    int32_t k;

    pd_doc_block_info(P->d, sec, &si);

    for (k = 1; k < si.child_count; k++) {
        pd_block_id fl = pd_doc_child(P->d, sec, k);
        pd_float_props fp;
        pd_inline ob;
        pd_pos at;
        const char* mime;
        const void* data;
        size_t len;
        long long x, y, cx, cy;

        if (pd_doc_block_info(P->d, fl, &bi) != PD_OK || bi.kind != PD_BLOCK_FLOAT ||
                pd_doc_float_props(P->d, fl, &fp) != PD_OK || fp.offset_from != PD_FROM_PAGE) {
            continue;
        }

        at.block = pd_doc_child(P->d, fl, 0);
        at.offset = 0;

        if (!at.block || pd_doc_inline_at(P->d, at, &ob) != PD_OK || ob.kind != PD_INLINE_IMAGE ||
                pd_doc_resource(P->d, ob.resource, &mime, &data, &len) != PD_OK) {
            continue;
        }

        x = EMU(fp.offset_x);
        y = EMU(fp.offset_y);
        cx = EMU(ob.width);
        cy = EMU(ob.height);

        if (!strcmp(mime, PD_DRAWING_MIME)) {
            pj_doc* jd = pj_parse(data, len, 0, NULL);
            int no = jd ? chart_of(P, pj_root(jd)) : 0;

            pj_free(jd);

            if (!no) {
                continue;
            }

            P->nrel++;
            pb_printf(&P->rels, "<Relationship Id=\"rId%d\" Type=\"%schart\" Target=\"../charts/%schart%d.xml\"/>",
                      P->nrel, REL, P->pfx, no);
            pb_printf(P->o, "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"%d\" name=\"Chart %d\"/>"
                      "<p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"%lld\" y=\"%lld\"/>"
                      "<a:ext cx=\"%lld\" cy=\"%lld\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas."
                      "openxmlformats.org/drawingml/2006/chart\"><c:chart xmlns:c=\"http://schemas.openxmlformats.org/"
                      "drawingml/2006/chart\" r:id=\"rId%d\"/></a:graphicData></a:graphic></p:graphicFrame>", P->next_id,
                      P->next_id, x, y, cx, cy, P->nrel);
            P->next_id++;
        } else {
            int rid = pic_rel(P, ob.resource);

            if (!rid) {
                continue;
            }

            pb_printf(P->o, "<p:pic><p:nvPicPr><p:cNvPr id=\"%d\" name=\"Picture %d\"/><p:cNvPicPr/><p:nvPr/>"
                      "</p:nvPicPr><p:blipFill><a:blip r:embed=\"rId%d\"/><a:stretch><a:fillRect/></a:stretch>"
                      "</p:blipFill><p:spPr><a:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/>"
                      "</a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>", P->next_id,
                      P->next_id, rid, x, y, cx, cy);
            P->next_id++;
        }
    }
}

/* a section that is no canvas: one text box inside its margins with its text */
static void put_text_slide(pw* P, pd_block_id sec) {
    pd_section_props sp;
    long long x, y, w, h;
    int np = 0;

    pd_doc_section_props(P->d, sec, &sp);
    x = EMU(sp.margin_left);
    y = EMU(sp.margin_top);
    w = EMU(sp.page_width - sp.margin_left - sp.margin_right);
    h = EMU(sp.page_height - sp.margin_top - sp.margin_bottom);
    x = x < P->sw ? x : 0;
    y = y < P->sh ? y : 0;
    w = w > 0 && x + w <= P->sw ? w : P->sw - x;
    h = h > 0 && y + h <= P->sh ? h : P->sh - y;
    pb_puts(P->o, "<p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/>"
            "<a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>");
    pb_printf(P->o, "<p:sp><p:nvSpPr><p:cNvPr id=\"%d\" name=\"Text %d\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>"
              "<p:spPr><a:xfrm><a:off x=\"%lld\" y=\"%lld\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm><a:prstGeom "
              "prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/></p:spPr><p:txBody><a:bodyPr wrap=\"square\" lIns=\"0\" "
              "tIns=\"0\" rIns=\"0\" bIns=\"0\" rtlCol=\"0\"><a:noAutofit/></a:bodyPr><a:lstStyle/>", P->next_id,
              P->next_id, x, y, w, h);
    P->next_id++;
    put_blocks(P, sec, &np);

    if (!np) {
        pb_puts(P->o, "<a:p><a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p>");
    }

    pb_puts(P->o, "</p:txBody></p:sp>");
}

static void zadd(void* z, const char* name, const pd_buf* b, int* err) {
    if (b->err || pd_zipw_add(z, name, b->p ? b->p : "", b->n) != 0) {
        *err = 1;
    }
}

static void zadds(void* z, const char* name, const char* s, int* err) {
    if (pd_zipw_add(z, name, s, strlen(s)) != 0) {
        *err = 1;
    }
}

static void zadd(void* z, const char* name, const pd_buf* b, int* err);

/* an element of kept XML by its start ("<p:notesSz"): from it to its end, as it is, onto o; 0 when it is not there */
static int put_raw_element(pd_buf* o, const char* s, size_t n, const char* start, const char* close) {
    const char* a = NULL, *e;
    size_t sl = strlen(start), i;

    for (i = 0; i + sl < n && !a; i++) {
        if (!memcmp(s + i, start, sl) && (s[i + sl] == ' ' || s[i + sl] == '>' || s[i + sl] == '/')) {
            a = s + i;
        }
    }

    if (!a) {
        return 0;
    }

    e = memchr(a, '>', n - (size_t)(a - s));

    if (e && e[-1] != '/') {    /* not empty: to its closing tag */
        size_t cl = strlen(close);

        for (e = a; e + cl <= s + n && memcmp(e, close, cl); e++) {
        }

        e = e + cl <= s + n ? e + cl - 1 : NULL;
    }

    if (!e) {
        return 0;
    }

    pb_put(o, a, (size_t)(e + 1 - a));
    return 1;
}

/* A presentation on the masters and layouts it came with: those parts as they were kept (the first master's theme
   as the document's theme is now), the presentation naming its masters by their ids, its default text style and
   its notes size as they were. */
static void put_kept_package(pw* P, void* z, pd_buf* ct, long long sw, long long sh, int nslides, int* err) {
    pd_buf b, prels;
    pkept e, pres;
    size_t pos = 0;
    char f[1][256], theme[256] = "";
    int nm = 0, s, have_pres = kept_find(P, "pres", &pres), have_ts = kept_find(P, "tablestyles", &e);

    memset(&b, 0, sizeof(b));
    memset(&prels, 0, sizeof(prels));

    if (kept_line(P, "theme", f, 1) == 1) {
        snprintf(theme, sizeof(theme), "%s", f[0]);
    }

    while (kept_next(P, &pos, &e)) {
        size_t nl = strlen(e.name);

        if (!strcmp(e.src, "map") || !strcmp(e.src, "pres")) {
            continue;
        }

        if (theme[0] && !strcmp(e.name, theme)) {   /* the document's theme, as it is now */
            pd_docx_theme_part(P->d, &b);
            zadd(z, e.name, &b, err);
            pb_free(&b);
        } else if (pd_zipw_add(z, e.name, e.data, e.len)) {
            *err = 1;
        }

        if (e.ctype[0] && !(nl > 5 && !strcmp(e.name + nl - 5, ".rels"))) {
            pb_puts(ct, "<Override PartName=\"/");
            escs(ct, e.name);
            pb_puts(ct, "\" ContentType=\"");
            escs(ct, e.ctype);
            pb_puts(ct, "\"/>");
        }
    }

    if (!have_ts) {
        zadds(z, "ppt/tableStyles.xml", TABLE_STYLES, err);
        pb_puts(ct, "<Override PartName=\"/ppt/tableStyles.xml\" ContentType=\"application/vnd.openxmlformats-"
                "officedocument.presentationml.tableStyles+xml\"/>");
    }

    pb_puts(&b, DECL);
    pb_printf(&b, "<p:presentation %s saveSubsetFonts=\"1\"><p:sldMasterIdLst>", NS);
    pb_puts(&prels, DECL);
    pb_puts(&prels, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");

    for (pos = 0; kept_next(P, &pos, &e);) {
        if (strcmp(e.src, "master") || strncmp(e.name, "ppt/", 4)) {
            continue;
        }

        nm++;
        pb_printf(&b, "<p:sldMasterId id=\"%s\" r:id=\"rIdM%d\"/>", e.extra[0] ? e.extra : "2147483648", nm);
        pb_printf(&prels, "<Relationship Id=\"rIdM%d\" Type=\"%sslideMaster\" Target=\"", nm, REL);
        escs(&prels, e.name + 4);
        pb_puts(&prels, "\"/>");
    }

    pb_puts(&b, "</p:sldMasterIdLst>");

    if (any_notes(P->d, nslides)) {     /* the notes master */
        pb_puts(&b, "<p:notesMasterIdLst><p:notesMasterId r:id=\"rIdNM\"/></p:notesMasterIdLst>");
        pb_printf(&prels, "<Relationship Id=\"rIdNM\" Type=\"%snotesMaster\" Target=\"notesMasters/notesMaster1.xml\"/>",
                  REL);
    }

    pb_puts(&b, "<p:sldIdLst>");

    for (s = 0; s < nslides; s++) {
        pb_printf(&b, "<p:sldId id=\"%d\" r:id=\"rId%d\"/>", 256 + s, s + 2);
        pb_printf(&prels, "<Relationship Id=\"rId%d\" Type=\"%sslide\" Target=\"slides/slide%d.xml\"/>", s + 2, REL,
                  s + 1);
    }

    pb_printf(&b, "</p:sldIdLst><p:sldSz cx=\"%lld\" cy=\"%lld\"/>", sw, sh);

    if (!have_pres || !put_raw_element(&b, (const char*)pres.data, pres.len, "<p:notesSz", "</p:notesSz>")) {
        pb_puts(&b, "<p:notesSz cx=\"6858000\" cy=\"9144000\"/>");
    }

    if (!have_pres || !put_raw_element(&b, (const char*)pres.data, pres.len, "<p:defaultTextStyle",
                                       "</p:defaultTextStyle>")) {
        pb_puts(&b, "<p:defaultTextStyle><a:defPPr><a:defRPr lang=\"en-US\"/></a:defPPr></p:defaultTextStyle>");
    }

    pb_puts(&b, "</p:presentation>");
    pb_printf(&prels, "<Relationship Id=\"rIdP1\" Type=\"%spresProps\" Target=\"presProps.xml\"/>"
              "<Relationship Id=\"rIdP2\" Type=\"%sviewProps\" Target=\"viewProps.xml\"/>"
              "<Relationship Id=\"rIdP3\" Type=\"%stableStyles\" Target=\"tableStyles.xml\"/>", REL, REL, REL);

    if (theme[0] && !strncmp(theme, "ppt/", 4)) {
        pb_printf(&prels, "<Relationship Id=\"rIdP4\" Type=\"%stheme\" Target=\"", REL);
        escs(&prels, theme + 4);
        pb_puts(&prels, "\"/>");
    }

    pb_puts(&prels, "</Relationships>");
    zadd(z, "ppt/presentation.xml", &b, err);
    zadd(z, "ppt/_rels/presentation.xml.rels", &prels, err);
    pb_free(&b);
    pb_free(&prels);
}

pd_status pd_pptx_export(const pd_doc* d, pd_buf* out) {
    pw* P = (pw*)calloc(1, sizeof(pw));
    pd_block_info ri;
    pd_section_props sp0;
    pd_buf b, ct, prels;
    void* z;
    int s, nslides, err = 0, i;
    char name[200], layout[256], prev_layout[256] = "";   /* the slide before's layout */

    if (!P) {
        return PD_ERR_NOMEM;
    }

    P->d = d;
    P->pfx = "";
    pd_numbers_init(&P->nb, d);

    {   /* the masters and layouts it came with, when it came from a presentation */
        const char* mime;
        const void* data;
        size_t len;
        pd_res_id r;
        pkept e;

        for (r = 1; pd_doc_resource(d, r, &mime, &data, &len) == PD_OK; r++) {
            if (!strcmp(mime, PD_PPTX_KEEP_MIME)) {     /* the latest */
                P->kdata = (const unsigned char*)data;
                P->klen = len;
            }
        }

        P->kept = P->kdata && kept_find(P, "master", &e) && kept_find(P, "map", &e);
        P->pfx = P->kept ? "pd" : "";
    }
    pd_doc_block_info(d, pd_doc_root(d), &ri);
    nslides = ri.child_count;

    if (nslides < 1 || nslides > 400) {
        pd_numbers_free(&P->nb);
        free(P);
        return PD_ERR_FORMAT;
    }

    /* the slides as big as the first canvas's page (else the first page), in what PowerPoint allows */
    pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), 0), &sp0);

    for (s = 0; s < nslides; s++) {
        if (section_canvas(d, pd_doc_child(d, pd_doc_root(d), s))) {
            pd_doc_section_props(d, pd_doc_child(d, pd_doc_root(d), s), &sp0);
            break;
        }
    }

    P->sw = EMU(sp0.page_width);
    P->sh = EMU(sp0.page_height);
    P->sw = P->sw < 914400 ? 914400 : P->sw > 51206400 ? 51206400 : P->sw;
    P->sh = P->sh < 914400 ? 914400 : P->sh > 51206400 ? 51206400 : P->sh;

    z = pd_zipw_new(out);

    if (!z) {
        pd_numbers_free(&P->nb);
        free(P);
        return PD_ERR_NOMEM;
    }

    memset(&b, 0, sizeof(b));
    memset(&ct, 0, sizeof(ct));
    memset(&prels, 0, sizeof(prels));
    pb_puts(&ct, DECL);
    pb_puts(&ct, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Default Extension=\"png\" ContentType=\"image/png\"/>"
            "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
            "<Default Extension=\"gif\" ContentType=\"image/gif\"/>"
            "<Default Extension=\"emf\" ContentType=\"image/x-emf\"/>"
            "<Default Extension=\"wmf\" ContentType=\"image/x-wmf\"/>"
            "<Default Extension=\"bmp\" ContentType=\"image/bmp\"/>"
            "<Default Extension=\"tiff\" ContentType=\"image/tiff\"/>"
            "<Default Extension=\"xlsx\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml."
            "sheet\"/>"
            "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "presentationml.presentation.main+xml\"/>"
            "<Override PartName=\"/ppt/presProps.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "presentationml.presProps+xml\"/>"
            "<Override PartName=\"/ppt/viewProps.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "presentationml.viewProps+xml\"/>"
            "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-"
            "properties+xml\"/>"
            "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument."
            "extended-properties+xml\"/>");

    if (!P->kept) {
        pb_puts(&ct, "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd."
                "openxmlformats-officedocument.presentationml.slideMaster+xml\"/><Override PartName=\"/ppt/slideLayouts/"
                "slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml."
                "slideLayout+xml\"/><Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd."
                "openxmlformats-officedocument.theme+xml\"/><Override PartName=\"/ppt/tableStyles.xml\" ContentType="
                "\"application/vnd.openxmlformats-officedocument.presentationml.tableStyles+xml\"/>");
    }

    for (s = 0; s < nslides; s++) {
        pb_printf(&ct, "<Override PartName=\"/ppt/slides/slide%d.xml\" ContentType=\"application/vnd.openxmlformats-"
                  "officedocument.presentationml.slide+xml\"/>", s + 1);

        if (slide_notes(d, pd_doc_child(d, pd_doc_root(d), s))) {
            pb_printf(&ct, "<Override PartName=\"/ppt/notesSlides/notesSlide%d.xml\" ContentType=\"application/vnd."
                      "openxmlformats-officedocument.presentationml.notesSlide+xml\"/>", s + 1);
        }
    }

    if (any_notes(d, nslides)) {    /* the notes master, its theme */
        pb_puts(&ct, "<Override PartName=\"/ppt/notesMasters/notesMaster1.xml\" ContentType=\"application/vnd."
                "openxmlformats-officedocument.presentationml.notesMaster+xml\"/><Override PartName=\"/ppt/theme/"
                "pdNotesTheme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>");
        put_notes_master(P, z, &err);
    }

    /* the slides */
    for (s = 0; s < nslides && !err; s++) {
        pd_block_id sec = pd_doc_child(d, pd_doc_root(d), s);
        pd_res_id cv = section_canvas(d, sec);
        pd_buf slide, tns;     /* the slide; the namespaces its kept transition needs */
        pd_section_props hsp;
        const char* hidden_attr = pd_doc_section_props(d, sec, &hsp) == PD_OK && hsp.hidden ? " show=\"0\"" : "";
        const char* txml = NULL;
        size_t txn = 0;

        memset(&slide, 0, sizeof(slide));
        memset(&tns, 0, sizeof(tns));
        P->o = &slide;
        memset(&P->rels, 0, sizeof(P->rels));
        P->nrel = 1;
        P->nsrel = 0;
        P->slide_no = s + 1;
        P->next_id = 2;
        P->nfld = 0;
        P->clr_ovr[0] = '\0';
        P->own_bg = P->has_rec = 0;
        layout[0] = '\0';
        pb_puts(&slide, DECL);

        if (P->kept) {  /* the layout it was on, and what it had of its own */
            char f[5][256], key[16];

            snprintf(key, sizeof(key), "%u", (unsigned)sec);
            f[4][0] = '\0';

            if (!kept_tail(P, key, &tns, &txml, &txn)) {
                txn = 0;
            }

            if (kept_line(P, key, f, 5) >= 4 && f[0][0]) {
                snprintf(layout, sizeof(layout), "%s", f[0]);
                P->own_bg = atoi(f[1]);
                P->has_rec = 1;
                P->rec_bg = strtol(f[3], NULL, 16);
                snprintf(P->clr_ovr, sizeof(P->clr_ovr), "%s", f[4]);   /* its own colour map, if it had one */
                pb_printf(&slide, "<p:sld %s%s%s%s>", NS, tns.p ? tns.p : "", atoi(f[2]) ? " showMasterSp=\"0\"" : "",
                          hidden_attr);
            } else {
                pb_printf(&slide, "<p:sld %s%s%s>", NS, tns.p ? tns.p : "", hidden_attr);
            }

            if (!layout[0] && prev_layout[0]) {    /* a slide made since: on the layout of the one before it */
                snprintf(layout, sizeof(layout), "%s", prev_layout);
            } else if (!layout[0]) {    /* (made before them all: the first's) */
                kept_first_layout(P, layout, sizeof(layout));
            }

            snprintf(prev_layout, sizeof(prev_layout), "%s", layout);
        } else {
            pb_printf(&slide, "<p:sld %s%s>", NS, hidden_attr);
        }

        if (cv) {
            put_canvas(P, cv);
            put_floats(P, sec);
        } else {
            put_text_slide(P, sec);
        }

        if (P->clr_ovr[0]) {
            pb_printf(&slide, "</p:spTree></p:cSld><p:clrMapOvr><a:overrideClrMapping %s/></p:clrMapOvr>", P->clr_ovr);
        } else {
            pb_puts(&slide, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>");
        }

        if (txn) {      /* its transition and timing, as they were */
            pb_put(&slide, txml, txn);
        }

        pb_puts(&slide, "</p:sld>");
        pb_free(&tns);
        snprintf(name, sizeof(name), "ppt/slides/slide%d.xml", s + 1);
        zadd(z, name, &slide, &err);
        memset(&b, 0, sizeof(b));
        pb_puts(&b, DECL);
        pb_printf(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                  "<Relationship Id=\"rId1\" Type=\"%sslideLayout\" Target=\"../%s\"/>", REL,
                  P->kept && !strncmp(layout, "ppt/", 4) ? layout + 4 : "slideLayouts/slideLayout1.xml");
        pb_put(&b, P->rels.p ? P->rels.p : "", P->rels.n);

        if (slide_notes(d, sec)) {  /* its speaker notes */
            pb_printf(&b, "<Relationship Id=\"rIdNotes\" Type=\"%snotesSlide\" Target=\"../notesSlides/notesSlide%d.xml\"/>",
                      REL, s + 1);
            put_notes_slide(P, z, s + 1, slide_notes(d, sec), &err);
        }

        pb_puts(&b, "</Relationships>");
        snprintf(name, sizeof(name), "ppt/slides/_rels/slide%d.xml.rels", s + 1);
        zadd(z, name, &b, &err);
        pb_free(&b);
        pb_free(&P->rels);
        pb_free(&slide);
    }

    for (i = 0; i < P->ncharts && !err; i++) {     /* the charts' parts, and the workbooks they come from */
        const char* mime;
        const void* data;
        size_t len;

        if (pd_doc_resource(d, P->charts[i].chart, &mime, &data, &len) != PD_OK) {
            err = 1;
            break;
        }

        snprintf(name, sizeof(name), "ppt/charts/%schart%d.xml", P->pfx, i + 1);

        if (pd_zipw_add(z, name, data, len)) {
            err = 1;
        }

        if (P->charts[i].rid[0] && (P->charts[i].data || P->charts[i].link[0])) {
            pb_puts(&b, DECL);
            pb_puts(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                    "<Relationship Id=\"");
            escs(&b, P->charts[i].rid);

            if (P->charts[i].data) {
                pb_printf(&b, "\" Type=\"%spackage\" Target=\"../embeddings/%sMicrosoft_Excel_Worksheet%d.xlsx\"/>",
                          REL, P->pfx, i + 1);
            } else {
                pb_printf(&b, "\" Type=\"%soleObject\" Target=\"", REL);
                escs(&b, P->charts[i].link);
                pb_puts(&b, "\" TargetMode=\"External\"/>");
            }

            pb_puts(&b, "</Relationships>");
            snprintf(name, sizeof(name), "ppt/charts/_rels/%schart%d.xml.rels", P->pfx, i + 1);
            zadd(z, name, &b, &err);
            pb_free(&b);

            if (P->charts[i].data && pd_doc_resource(d, P->charts[i].data, &mime, &data, &len) == PD_OK) {
                snprintf(name, sizeof(name), "ppt/embeddings/%sMicrosoft_Excel_Worksheet%d.xlsx", P->pfx, i + 1);

                if (pd_zipw_add(z, name, data, len)) {
                    err = 1;
                }
            }
        }
    }

    for (i = 0; i < P->nmedia && !err; i++) {  /* the pictures */
        const char* mime;
        const void* data;
        size_t len;

        snprintf(name, sizeof(name), "ppt/media/%s", P->media[i].name);

        if (pd_doc_resource(d, P->media[i].res, &mime, &data, &len) != PD_OK || pd_zipw_add(z, name, data, len)) {
            err = 1;
        }
    }

    if (P->kept) {
        put_kept_package(P, z, &ct, P->sw, P->sh, nslides, &err);
    } else {   /* the presentation */
        pb_puts(&b, DECL);
        pb_printf(&b, "<p:presentation %s saveSubsetFonts=\"1\"><p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" "
                  "r:id=\"rId1\"/></p:sldMasterIdLst>%s<p:sldIdLst>", NS, any_notes(d, nslides) ?
                  "<p:notesMasterIdLst><p:notesMasterId r:id=\"rIdNM\"/></p:notesMasterIdLst>" : "");
        pb_puts(&prels, DECL);
        pb_printf(&prels, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                  "<Relationship Id=\"rId1\" Type=\"%sslideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>", REL);

        if (any_notes(d, nslides)) {
            pb_printf(&prels, "<Relationship Id=\"rIdNM\" Type=\"%snotesMaster\" Target=\"notesMasters/notesMaster1.xml\"/>",
                      REL);
        }

        for (s = 0; s < nslides; s++) {
            pb_printf(&b, "<p:sldId id=\"%d\" r:id=\"rId%d\"/>", 256 + s, s + 2);
            pb_printf(&prels, "<Relationship Id=\"rId%d\" Type=\"%sslide\" Target=\"slides/slide%d.xml\"/>", s + 2, REL,
                      s + 1);
        }

        pb_printf(&b, "</p:sldIdLst><p:sldSz cx=\"%lld\" cy=\"%lld\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
                  "<p:defaultTextStyle><a:defPPr><a:defRPr lang=\"en-US\"/></a:defPPr></p:defaultTextStyle>"
                  "</p:presentation>", P->sw, P->sh);
        pb_printf(&prels, "<Relationship Id=\"rId%d\" Type=\"%spresProps\" Target=\"presProps.xml\"/>"
                  "<Relationship Id=\"rId%d\" Type=\"%sviewProps\" Target=\"viewProps.xml\"/>"
                  "<Relationship Id=\"rId%d\" Type=\"%stheme\" Target=\"theme/theme1.xml\"/>"
                  "<Relationship Id=\"rId%d\" Type=\"%stableStyles\" Target=\"tableStyles.xml\"/></Relationships>",
                  nslides + 2, REL, nslides + 3, REL, nslides + 4, REL, nslides + 5, REL);
        zadd(z, "ppt/presentation.xml", &b, &err);
        zadd(z, "ppt/_rels/presentation.xml.rels", &prels, &err);
        pb_free(&b);
        pb_free(&prels);

        /* one master, one blank layout: what every slide's shapes are drawn over (nothing) */
        pb_puts(&b, DECL);
        pb_printf(&b, "<p:sldMaster %s><p:cSld><p:bg><p:bgRef idx=\"1001\"><a:schemeClr val=\"bg1\"/></p:bgRef></p:bg>"
                  "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr>"
                  "<a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" "
                  "cy=\"0\"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" "
                  "tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" "
                  "accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:sldLayoutIdLst>"
                  "<p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst><p:txStyles>"
                  "<p:titleStyle><a:lvl1pPr algn=\"l\"><a:defRPr sz=\"4400\" kern=\"1200\"><a:solidFill><a:schemeClr "
                  "val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mj-lt\"/><a:ea typeface=\"+mj-ea\"/><a:cs "
                  "typeface=\"+mj-cs\"/></a:defRPr></a:lvl1pPr></p:titleStyle>"
                  "<p:bodyStyle><a:lvl1pPr><a:defRPr sz=\"1800\" kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/>"
                  "</a:solidFill><a:latin typeface=\"+mn-lt\"/><a:ea typeface=\"+mn-ea\"/><a:cs typeface=\"+mn-cs\"/>"
                  "</a:defRPr></a:lvl1pPr></p:bodyStyle>"
                  "<p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\" kern=\"1200\"><a:solidFill><a:schemeClr val=\"tx1\"/>"
                  "</a:solidFill><a:latin typeface=\"+mn-lt\"/><a:ea typeface=\"+mn-ea\"/><a:cs typeface=\"+mn-cs\"/>"
                  "</a:defRPr></a:lvl1pPr></p:otherStyle></p:txStyles></p:sldMaster>", NS);
        zadd(z, "ppt/slideMasters/slideMaster1.xml", &b, &err);
        pb_free(&b);
        pb_puts(&b, DECL);
        pb_printf(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                  "<Relationship Id=\"rId1\" Type=\"%sslideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
                  "<Relationship Id=\"rId2\" Type=\"%stheme\" Target=\"../theme/theme1.xml\"/></Relationships>", REL, REL);
        zadd(z, "ppt/slideMasters/_rels/slideMaster1.xml.rels", &b, &err);
        pb_free(&b);
        pb_puts(&b, DECL);
        pb_printf(&b, "<p:sldLayout %s type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\"><p:spTree><p:nvGrpSpPr>"
                  "<p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" "
                  "y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm>"
                  "</p:grpSpPr></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>", NS);
        zadd(z, "ppt/slideLayouts/slideLayout1.xml", &b, &err);
        pb_free(&b);
        pb_puts(&b, DECL);
        pb_printf(&b, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                  "<Relationship Id=\"rId1\" Type=\"%sslideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/>"
                  "</Relationships>", REL);
        zadd(z, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", &b, &err);
        pb_free(&b);

        pd_docx_theme_part(d, &b);
        zadd(z, "ppt/theme/theme1.xml", &b, &err);
        pb_free(&b);
    }

    pb_puts(&b, DECL);
    pb_printf(&b, "<p:presentationPr %s/>", NS);
    zadd(z, "ppt/presProps.xml", &b, &err);
    pb_free(&b);
    pb_puts(&b, DECL);
    pb_printf(&b, "<p:viewPr %s><p:normalViewPr><p:restoredLeft sz=\"15620\"/><p:restoredTop sz=\"94660\"/>"
              "</p:normalViewPr><p:gridSpacing cx=\"76200\" cy=\"76200\"/></p:viewPr>", NS);
    zadd(z, "ppt/viewProps.xml", &b, &err);
    pb_free(&b);

    if (!P->kept) {
        zadds(z, "ppt/tableStyles.xml", TABLE_STYLES, &err);
    }

    /* the package's properties: no dates, so that the same document writes the same bytes */
    zadds(z, "docProps/core.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
          "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
          "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
          "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"/>", &err);
    pb_puts(&b, DECL);
    pb_printf(&b, "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
              "<Application>Parade</Application><Slides>%d</Slides></Properties>", nslides);
    zadd(z, "docProps/app.xml", &b, &err);
    pb_free(&b);
    pb_printf(&b, "%s<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
              "<Relationship Id=\"rId1\" Type=\"%sofficeDocument\" Target=\"ppt/presentation.xml\"/>"
              "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/"
              "core-properties\" Target=\"docProps/core.xml\"/><Relationship Id=\"rId3\" Type=\"%sextended-properties\" "
              "Target=\"docProps/app.xml\"/></Relationships>", DECL, REL, REL);
    zadd(z, "_rels/.rels", &b, &err);
    pb_free(&b);

    for (i = 0; i < P->ncharts; i++) {
        pb_printf(&ct, "<Override PartName=\"/ppt/charts/%schart%d.xml\" ContentType=\"application/vnd.openxmlformats-"
                  "officedocument.drawingml.chart+xml\"/>", P->pfx, i + 1);
    }

    pb_puts(&ct, "</Types>");
    zadd(z, "[Content_Types].xml", &ct, &err);
    pb_free(&ct);
    pd_zipw_finish(z);
    pd_numbers_free(&P->nb);
    free(P);
    return err || out->err ? (out->err ? PD_ERR_NOMEM : PD_ERR_FORMAT) : PD_OK;
}
