/*
 * Parade markup tokenizer: tags, text and entities, for HTML (tolerant:
 * unquoted attributes, raw script/style, stray '<') and XML (DOCX parts)
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

static const struct {
    const char* name;
    uint32_t cp;
} entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "trade", 0x2122 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "hellip", 0x2026 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D },
    { "sbquo", 0x201A }, { "bdquo", 0x201E }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "bull", 0x2022 },
    { "middot", 0xB7 }, { "times", 0xD7 }, { "divide", 0xF7 }, { "deg", 0xB0 }, { "plusmn", 0xB1 },
    { "micro", 0xB5 }, { "para", 0xB6 }, { "sect", 0xA7 }, { "euro", 0x20AC }, { "pound", 0xA3 },
    { "yen", 0xA5 }, { "cent", 0xA2 }, { "shy", 0xAD }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
    { "thinsp", 0x2009 }, { "zwj", 0x200D }, { "zwnj", 0x200C }, { "dagger", 0x2020 }, { "Dagger", 0x2021 },
    { "larr", 0x2190 }, { "rarr", 0x2192 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "harr", 0x2194 },
    { "le", 0x2264 }, { "ge", 0x2265 }, { "ne", 0x2260 }, { "asymp", 0x2248 }, { "infin", 0x221E },
    { "sum", 0x2211 }, { "prod", 0x220F }, { "minus", 0x2212 }, { "radic", 0x221A }, { "alpha", 0x3B1 },
    { "beta", 0x3B2 }, { "gamma", 0x3B3 }, { "delta", 0x3B4 }, { "epsilon", 0x3B5 }, { "lambda", 0x3BB },
    { "mu", 0x3BC }, { "pi", 0x3C0 }, { "sigma", 0x3C3 }, { "omega", 0x3C9 }, { "Delta", 0x394 },
    { "Omega", 0x3A9 }, { "iexcl", 0xA1 }, { "iquest", 0xBF }, { "frac12", 0xBD }, { "frac14", 0xBC },
    { "frac34", 0xBE }, { "sup2", 0xB2 }, { "sup3", 0xB3 }, { "ordm", 0xBA }, { "ordf", 0xAA },
    { "Agrave", 0xC0 }, { "Aacute", 0xC1 }, { "Auml", 0xC4 }, { "Ccedil", 0xC7 }, { "Egrave", 0xC8 },
    { "Eacute", 0xC9 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "szlig", 0xDF }, { "agrave", 0xE0 },
    { "aacute", 0xE1 }, { "acirc", 0xE2 }, { "auml", 0xE4 }, { "aring", 0xE5 }, { "ccedil", 0xE7 },
    { "egrave", 0xE8 }, { "eacute", 0xE9 }, { "ecirc", 0xEA }, { "euml", 0xEB }, { "iacute", 0xED },
    { "ntilde", 0xF1 }, { "oacute", 0xF3 }, { "ocirc", 0xF4 }, { "ouml", 0xF6 }, { "uacute", 0xFA },
    { "uuml", 0xFC }, { "oslash", 0xF8 }, { "Oslash", 0xD8 }, { "aelig", 0xE6 }, { "AElig", 0xC6 }
};

static void put_cp(pd_buf* b, uint32_t c) {
    char u[4];

    if (c == 0 || c > 0x10FFFF || (c >= 0xD800 && c < 0xE000) || c == 0xFFFC) {
        c = 0xFFFD;
    }

    if (c < 0x80) {
        u[0] = (char)c;
        pb_put(b, u, 1);
    } else if (c < 0x800) {
        u[0] = (char)(0xC0 | (c >> 6));
        u[1] = (char)(0x80 | (c & 0x3F));
        pb_put(b, u, 2);
    } else if (c < 0x10000) {
        u[0] = (char)(0xE0 | (c >> 12));
        u[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        u[2] = (char)(0x80 | (c & 0x3F));
        pb_put(b, u, 3);
    } else {
        u[0] = (char)(0xF0 | (c >> 18));
        u[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        u[3] = (char)(0x80 | (c & 0x3F));
        pb_put(b, u, 4);
    }
}

void mu_decode(const char* s, size_t n, pd_buf* out) {
    size_t i = 0;

    while (i < n) {
        size_t j = i;

        while (j < n && s[j] != '&') {
            j++;
        }

        pb_put(out, s + i, j - i);

        if (j >= n) {
            break;
        }

        /* &...; */
        {
            size_t e = j + 1;
            uint32_t cp = 0;
            int ok = 0;

            while (e < n && e - j < 32 && (isalnum((unsigned char)s[e]) || s[e] == '#')) {
                e++;
            }

            if (j + 1 < n && s[j + 1] == '#') {
                const char* p = s + j + 2;
                char* endp;
                char num[16];
                size_t k = (size_t)(s + e - p);

                if (k > 0 && k < sizeof(num)) {
                    memcpy(num, p, k);
                    num[k] = '\0';
                    cp = (uint32_t)strtoul(num[0] == 'x' || num[0] == 'X' ? num + 1 : num, &endp,
                                           num[0] == 'x' || num[0] == 'X' ? 16 : 10);
                    ok = *endp == '\0' && k > (size_t)(num[0] == 'x' || num[0] == 'X');
                }
            } else {
                size_t k;

                for (k = 0; k < sizeof(entities) / sizeof(entities[0]); k++) {
                    size_t ln = strlen(entities[k].name);

                    if (ln == e - j - 1 && memcmp(entities[k].name, s + j + 1, ln) == 0) {
                        cp = entities[k].cp;
                        ok = 1;
                        break;
                    }
                }
            }

            if (ok) {
                put_cp(out, cp);
                i = e < n && s[e] == ';' ? e + 1 : e;
            } else {
                pb_putc(out, '&');
                i = j + 1;
            }
        }
    }
}

void mu_init(pd_markup* m, const char* s, size_t n, int html) {
    memset(m, 0, sizeof(*m));
    m->s = s;
    m->n = n;
    m->html = html;
}

const char* mu_local(const char* name) {
    const char* c = strchr(name, ':');
    return c ? c + 1 : name;
}

static int ci_eq(const char* a, const char* b, size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }

    return 1;
}

static int name_char(char c) {
    return isalnum((unsigned char)c) || c == ':' || c == '-' || c == '_' || c == '.';
}

/* skip raw text up to </name> (script, style) */
static void skip_raw(pd_markup* m, const char* name) {
    size_t ln = strlen(name);

    while (m->pos < m->n) {
        if (m->s[m->pos] == '<' && m->pos + 1 + ln < m->n && m->s[m->pos + 1] == '/') {
            size_t k;

            for (k = 0; k < ln && tolower((unsigned char)m->s[m->pos + 2 + k]) == name[k]; k++) {
            }

            if (k == ln) {
                return;
            }
        }

        m->pos++;
    }
}

int mu_next(pd_markup* m) {
    const char* s = m->s;
    size_t n = m->n, i;

    m->type = MT_END;

    if (m->pos >= n) {
        return MT_END;
    }

    i = m->pos;

    if (s[i] != '<' || i + 1 >= n || !(name_char(s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!' || s[i + 1] == '?')) {
        size_t j = i + 1;

        while (j < n && !(s[j] == '<' && j + 1 < n && (name_char(s[j + 1]) || s[j + 1] == '/' || s[j + 1] == '!' ||
                          s[j + 1] == '?'))) {
            j++;
        }

        m->type = MT_TEXT;
        m->text = s + i;
        m->tlen = j - i;
        m->pos = j;
        return MT_TEXT;
    }

    if (s[i + 1] == '!' || s[i + 1] == '?') {     /* comments, doctype, CDATA, processing instructions */
        if (i + 3 < n && s[i + 2] == '-' && s[i + 3] == '-') {
            const char* e = NULL;
            size_t k;

            for (k = i + 4; k + 2 < n; k++) {
                if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') {
                    e = s + k;
                    break;
                }
            }

            m->pos = e ? (size_t)(e - s) + 3 : n;
        } else if (i + 8 < n && memcmp(s + i, "<![CDATA[", 9) == 0) {
            size_t k = i + 9;

            while (k + 2 < n && !(s[k] == ']' && s[k + 1] == ']' && s[k + 2] == '>')) {
                k++;
            }

            m->type = MT_TEXT;
            m->text = s + i + 9;
            m->tlen = k - (i + 9);
            m->pos = k + 3 < n ? k + 3 : n;
            return MT_TEXT;
        } else {
            while (i < n && s[i] != '>') {
                i++;
            }

            m->pos = i + 1;
        }

        return mu_next(m);
    }

    {
        int closing = s[i + 1] == '/';
        size_t a = i + 1 + closing, k = 0, e;
        char q = 0;

        while (a < n && name_char(s[a]) && k + 1 < sizeof(m->name)) {
            m->name[k++] = m->html ? (char)tolower((unsigned char)s[a]) : s[a];
            a++;
        }

        m->name[k] = '\0';

        for (e = a; e < n; e++) {   /* the end of the tag, outside quotes */
            if (q) {
                q = s[e] == q ? 0 : q;
            } else if (s[e] == '"' || s[e] == '\'') {
                q = s[e];
            } else if (s[e] == '>') {
                break;
            }
        }

        m->attrs = s + a;
        m->alen = e - a;
        m->pos = e < n ? e + 1 : n;

        if (closing) {
            m->type = MT_CLOSE;
        } else if (m->alen > 0 && s[e - 1] == '/') {
            m->type = MT_EMPTY;
            m->alen--;
        } else {
            m->type = MT_OPEN;
        }

        if (m->html && m->type == MT_OPEN && (strcmp(m->name, "script") == 0 || strcmp(m->name, "style") == 0 ||
                                              strcmp(m->name, "title") == 0)) {
            skip_raw(m, m->name);
        }

        return m->type;
    }
}

int mu_attr(const pd_markup* m, const char* name, char* buf, size_t cap) {
    const char* a = m->attrs;
    size_t n = m->alen, i = 0, ln = strlen(name);

    while (i < n) {
        size_t ks, ke, vs, ve;

        while (i < n && !name_char(a[i])) {
            i++;
        }

        ks = i;

        while (i < n && name_char(a[i])) {
            i++;
        }

        ke = i;

        while (i < n && isspace((unsigned char)a[i])) {
            i++;
        }

        vs = ve = i;

        if (i < n && a[i] == '=') {
            i++;

            while (i < n && isspace((unsigned char)a[i])) {
                i++;
            }

            if (i < n && (a[i] == '"' || a[i] == '\'')) {
                char q = a[i++];

                vs = i;

                while (i < n && a[i] != q) {
                    i++;
                }

                ve = i;
                i += i < n;
            } else {
                vs = i;

                while (i < n && !isspace((unsigned char)a[i])) {
                    i++;
                }

                ve = i;
            }
        }

        if (ke - ks == ln && (m->html ? ci_eq(a + ks, name, ln) : memcmp(a + ks, name, ln) == 0)) {
            pd_buf v;

            memset(&v, 0, sizeof(v));
            mu_decode(a + vs, ve - vs, &v);

            if (cap > 0) {
                size_t c = v.n < cap - 1 ? v.n : cap - 1;

                if (c) {
                    memcpy(buf, v.p, c);
                }

                buf[c] = '\0';
            }

            pb_free(&v);
            return 1;
        }

        if (ke == ks) {
            i++;
        }
    }

    return 0;
}
