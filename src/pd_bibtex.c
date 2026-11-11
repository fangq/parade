/*
 * Parade BibTeX: a .bib file's entries set out as a thebibliography list
 *
 * What BibTeX and a style (.bst) do, done the plain way: the entries a
 * document cites, in the order it cites them or sorted by author, year and
 * title, each written out as LaTeX (authors, title, where it appeared,
 * volume, number, pages, date, DOI) for the LaTeX importer to read as it
 * reads a thebibliography list it finds in a file. Each item is
 * \bibitem[Author(Year)]{key}, natbib's form, so \citet and \citeauthor
 * have the authors' names.
 *
 * Read: @string macros (and the months), values in braces or quotes or bare
 * numbers joined with #, crossref (missing fields from the entry named),
 * biblatex's names (journaltitle, location, date). @comment and @preamble
 * are passed over.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pd_conv.h"

#define BIB_MAX_ENTRIES 100000
#define BIB_MAX_NAMES 256

/* (C99 has neither strncasecmp nor strndup) */
static int bib_ncase(const char* a, const char* b, size_t n) {
    size_t k;

    for (k = 0; k < n; k++) {
        int x = tolower((unsigned char)a[k]), y = tolower((unsigned char)b[k]);

        if (x != y || !x) {
            return x - y;
        }
    }

    return 0;
}

static char* bib_dup(const char* s, size_t n) {
    char* r = (char*)malloc(n + 1);

    if (r) {
        memcpy(r, s, n);
        r[n] = '\0';
    }

    return r;
}

typedef struct {
    char name[32];
    char* val;
} bfield;

typedef struct {
    char type[24];
    char key[80];
    bfield* f;
    int nf, capf;
    char* sortkey;
    int picked;
} bentry;

typedef struct {
    char name[48];
    char* val;
} bstring;

typedef struct {
    const char* s;
    size_t n, i;
    bentry* e;
    int ne, cape;
    bstring* str;
    int nstr, capstr;
} bparse;

/* ------------------------------------------------------------------ */
/* reading                                                            */
/* ------------------------------------------------------------------ */

static void ws(bparse* P) {
    while (P->i < P->n && isspace((unsigned char)P->s[P->i])) {
        P->i++;
    }
}

static int ident_char(char c) {
    return isalnum((unsigned char)c) || (c && strchr("_-:./+'!$&*?@", c) != NULL) || (unsigned char)c >= 0x80;
}

/* a name (entry type, field, macro): lowered into buf */
static size_t ident(bparse* P, char* buf, size_t cap) {
    size_t k = 0;

    while (P->i < P->n && ident_char(P->s[P->i]) && P->s[P->i] != '@') {
        if (k + 1 < cap) {
            buf[k++] = (char)tolower((unsigned char)P->s[P->i]);
        }

        P->i++;
    }

    buf[k] = '\0';
    return k;
}

/* past a balanced group opened at s[i] ('{' or '('): the index after its close */
static size_t skip_group(const char* s, size_t n, size_t i) {
    char open = s[i], close = open == '(' ? ')' : '}';
    int d = 0;

    for (; i < n; i++) {
        if (s[i] == open) {
            d++;
        } else if (s[i] == close && --d == 0) {
            return i + 1;
        } else if (open == '(' && s[i] == '{') {
            i = skip_group(s, n, i) - 1;
        }
    }

    return n;
}

static const char* const MONTHS[12] = {
    "January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
    "November", "December"
};

static const char* macro_value(const bparse* P, const char* name) {
    static const char* const ab[12] = {
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"
    };
    int k;

    for (k = P->nstr - 1; k >= 0; k--) {
        if (!strcmp(P->str[k].name, name)) {
            return P->str[k].val;
        }
    }

    for (k = 0; k < 12; k++) {
        if (!strcmp(ab[k], name)) {
            return MONTHS[k];
        }
    }

    return "";
}

/* a field's value: its parts joined by # */
static char* value(bparse* P) {
    pd_buf o;

    memset(&o, 0, sizeof(o));

    for (;;) {
        ws(P);

        if (P->i >= P->n) {
            break;
        }

        if (P->s[P->i] == '{') {
            size_t e = skip_group(P->s, P->n, P->i);
            size_t b = e > P->i + 1 && e <= P->n && P->s[e - 1] == '}' ? e - 1 : e;

            pb_put(&o, P->s + P->i + 1, b - P->i - 1);
            P->i = e;
        } else if (P->s[P->i] == '"') {
            size_t j = P->i + 1;
            int d = 0;

            while (j < P->n && (P->s[j] != '"' || d > 0)) {
                if (P->s[j] == '{') {
                    d++;
                } else if (P->s[j] == '}' && d > 0) {
                    d--;
                } else if (P->s[j] == '\\' && j + 1 < P->n) {
                    j++;
                }

                j++;
            }

            pb_put(&o, P->s + P->i + 1, (j < P->n ? j : P->n) - P->i - 1);
            P->i = j < P->n ? j + 1 : P->n;
        } else if (isdigit((unsigned char)P->s[P->i])) {
            size_t j = P->i;

            while (j < P->n && isdigit((unsigned char)P->s[j])) {
                j++;
            }

            pb_put(&o, P->s + P->i, j - P->i);
            P->i = j;
        } else if (ident_char(P->s[P->i])) {
            char name[48];

            ident(P, name, sizeof(name));
            pb_puts(&o, macro_value(P, name));
        } else {
            break;
        }

        ws(P);

        if (P->i < P->n && P->s[P->i] == '#') {
            P->i++;
            continue;
        }

        break;
    }

    pb_putc(&o, '\0');

    if (o.err) {
        pb_free(&o);
        return NULL;
    }

    return o.p;
}

static void add_field(bentry* e, const char* name, char* val) {
    int k;

    for (k = 0; k < e->nf; k++) {
        if (!strcmp(e->f[k].name, name)) {     /* the first one counts, as BibTeX has it */
            free(val);
            return;
        }
    }

    if (e->nf == e->capf) {
        int nc = e->capf ? e->capf * 2 : 16;
        bfield* q = (bfield*)realloc(e->f, sizeof(bfield) * (size_t)nc);

        if (!q) {
            free(val);
            return;
        }

        e->f = q;
        e->capf = nc;
    }

    strncpy(e->f[e->nf].name, name, sizeof(e->f[0].name) - 1);
    e->f[e->nf].name[sizeof(e->f[0].name) - 1] = '\0';
    e->f[e->nf].val = val;
    e->nf++;
}

static void free_entry(bentry* e) {
    int k;

    for (k = 0; k < e->nf; k++) {
        free(e->f[k].val);
    }

    free(e->f);
    free(e->sortkey);
}

/* an entry's body after its '{' or '(': the key, the fields to close */
static void entry_body(bparse* P, bentry* e, char close) {
    size_t a;

    ws(P);
    a = P->i;

    while (P->i < P->n && P->s[P->i] != ',' && P->s[P->i] != close && !isspace((unsigned char)P->s[P->i])) {
        P->i++;
    }

    if (P->i - a < sizeof(e->key)) {
        memcpy(e->key, P->s + a, P->i - a);
        e->key[P->i - a] = '\0';
    }

    while (P->i < P->n) {
        char name[32];

        ws(P);

        if (P->i < P->n && P->s[P->i] == ',') {
            P->i++;
            continue;
        }

        if (P->i >= P->n || P->s[P->i] == close) {
            P->i = P->i < P->n ? P->i + 1 : P->n;
            return;
        }

        if (!ident(P, name, sizeof(name))) {
            P->i++;     /* (something out of place: passed over) */
            continue;
        }

        ws(P);

        if (P->i < P->n && P->s[P->i] == '=') {
            char* v;

            P->i++;
            v = value(P);

            if (v) {
                add_field(e, name, v);
            }
        }
    }
}

static void parse(bparse* P) {
    while (P->i < P->n) {
        char type[24], close;

        while (P->i < P->n && P->s[P->i] != '@') {
            P->i++;
        }

        if (P->i >= P->n) {
            return;
        }

        P->i++;
        ws(P);
        ident(P, type, sizeof(type));
        ws(P);

        if (P->i >= P->n || (P->s[P->i] != '{' && P->s[P->i] != '(')) {
            continue;
        }

        close = P->s[P->i] == '(' ? ')' : '}';

        if (!strcmp(type, "comment") || !strcmp(type, "preamble")) {
            P->i = skip_group(P->s, P->n, P->i);
            continue;
        }

        P->i++;

        if (!strcmp(type, "string")) {
            char name[48];
            char* v;

            ws(P);
            ident(P, name, sizeof(name));
            ws(P);

            if (P->i < P->n && P->s[P->i] == '=') {
                P->i++;
                v = value(P);

                if (v) {
                    if (P->nstr == P->capstr) {
                        int nc = P->capstr ? P->capstr * 2 : 16;
                        bstring* q = (bstring*)realloc(P->str, sizeof(bstring) * (size_t)nc);

                        if (!q) {
                            free(v);
                            return;
                        }

                        P->str = q;
                        P->capstr = nc;
                    }

                    strcpy(P->str[P->nstr].name, name);
                    P->str[P->nstr].val = v;
                    P->nstr++;
                }
            }

            while (P->i < P->n && P->s[P->i] != close && P->s[P->i] != '@') {
                P->i++;
            }

            continue;
        }

        if (P->ne >= BIB_MAX_ENTRIES) {
            return;
        }

        if (P->ne == P->cape) {
            int nc = P->cape ? P->cape * 2 : 64;
            bentry* q = (bentry*)realloc(P->e, sizeof(bentry) * (size_t)nc);

            if (!q) {
                return;
            }

            P->e = q;
            P->cape = nc;
        }

        memset(&P->e[P->ne], 0, sizeof(bentry));
        strcpy(P->e[P->ne].type, type);
        entry_body(P, &P->e[P->ne], close);

        if (P->e[P->ne].key[0]) {
            P->ne++;
        } else {
            free_entry(&P->e[P->ne]);
        }
    }
}

static bentry* find_entry(const bparse* P, const char* key, size_t kn) {
    int k;

    for (k = 0; k < P->ne; k++) {
        if (strlen(P->e[k].key) == kn && !strncmp(P->e[k].key, key, kn)) {
            return &P->e[k];
        }
    }

    /* BibTeX's keys are matched without regard to case */
    for (k = 0; k < P->ne; k++) {
        if (strlen(P->e[k].key) == kn && !bib_ncase(P->e[k].key, key, kn)) {
            return &P->e[k];
        }
    }

    return NULL;
}

static const char* get1(const bentry* e, const char* name) {
    int k;

    for (k = 0; k < e->nf; k++) {
        if (!strcmp(e->f[k].name, name)) {
            return e->f[k].val[0] ? e->f[k].val : NULL;
        }
    }

    return NULL;
}

/* a field by its BibTeX name or biblatex's */
static const char* get(const bentry* e, const char* name) {
    const char* v = get1(e, name);

    if (v) {
        return v;
    }

    if (!strcmp(name, "journal")) {
        return get1(e, "journaltitle");
    }

    if (!strcmp(name, "address")) {
        return get1(e, "location");
    }

    if (!strcmp(name, "school")) {
        return get1(e, "institution");
    }

    return NULL;
}

/* the year: the field, or a biblatex date's first four digits */
static void year_of(const bentry* e, char* buf, size_t cap) {
    const char* y = get1(e, "year");

    buf[0] = '\0';

    if (y) {
        snprintf(buf, cap, "%s", y);
        return;
    }

    y = get1(e, "date");

    if (y && strlen(y) >= 4 && isdigit((unsigned char)y[0])) {
        snprintf(buf, cap, "%.4s", y);
    }
}

/* the fields of the entry it crossrefs that it has not */
static void inherit(bparse* P) {
    int k, j;

    for (k = 0; k < P->ne; k++) {
        const char* x = get1(&P->e[k], "crossref");
        bentry* p;

        if (!x || !(p = find_entry(P, x, strlen(x))) || p == &P->e[k]) {
            continue;
        }

        for (j = 0; j < p->nf; j++) {
            const char* name = p->f[j].name;
            char* v;

            if (!strcmp(name, "title") && !get1(&P->e[k], "booktitle")) {
                name = "booktitle";
            }

            if (get1(&P->e[k], name)) {
                continue;
            }

            v = bib_dup(p->f[j].val, strlen(p->f[j].val));

            if (v) {
                add_field(&P->e[k], name, v);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* names                                                              */
/* ------------------------------------------------------------------ */

/* the names of a list ("A and B and C"): their ranges */
static int split_names(const char* s, size_t (*r)[2], int cap) {
    size_t n = strlen(s), i = 0, a = 0;
    int d = 0, k = 0;

    while (i <= n && k < cap) {
        if (i < n && s[i] == '{') {
            d++;
        } else if (i < n && s[i] == '}' && d > 0) {
            d--;
        }

        if (i == n || (d == 0 && i > 0 && isspace((unsigned char)s[i - 1]) && i + 4 <= n &&
                       !bib_ncase(s + i, "and", 3) && isspace((unsigned char)s[i + 3]))) {
            size_t b = i;

            while (a < b && isspace((unsigned char)s[a])) {
                a++;
            }

            while (b > a && isspace((unsigned char)s[b - 1])) {
                b--;
            }

            if (b > a) {
                r[k][0] = a;
                r[k][1] = b;
                k++;
            }

            if (i == n) {
                break;
            }

            i += 3;
            a = i;
            continue;
        }

        i++;
    }

    return k;
}

/* the words of s[a,b): at blanks and ~ outside braces */
static int words(const char* s, size_t a, size_t b, size_t (*w)[2], int cap) {
    int k = 0, d = 0;
    size_t i = a, st = a;

    for (; i <= b; i++) {
        int brk = i == b || (d == 0 && (isspace((unsigned char)s[i]) || s[i] == '~'));

        if (i < b && s[i] == '{') {
            d++;
        } else if (i < b && s[i] == '}' && d > 0) {
            d--;
        }

        if (brk) {
            if (i > st && k < cap) {
                w[k][0] = st;
                w[k][1] = i;
                k++;
            }

            st = i + 1;
        }
    }

    return k;
}

typedef struct {
    size_t fa, fb, la, lb, ja, jb;  /* first, von + last, jr: ranges in the name */
} bname;

static void trim(const char* s, size_t* a, size_t* b) {
    while (*a < *b && isspace((unsigned char)s[*a])) {
        (*a)++;
    }

    while (*b > *a && isspace((unsigned char)s[*b - 1])) {
        (*b)--;
    }
}

static void name_parts(const char* s, size_t a, size_t b, bname* nm) {
    size_t c[2];
    int nc = 0, d = 0;
    size_t i;

    memset(nm, 0, sizeof(*nm));

    for (i = a; i < b; i++) {
        if (s[i] == '{') {
            d++;
        } else if (s[i] == '}' && d > 0) {
            d--;
        } else if (s[i] == ',' && d == 0 && nc < 2) {
            c[nc++] = i;
        }
    }

    if (nc == 0) {      /* First von Last */
        size_t w[64][2];
        int k = words(s, a, b, w, 64), v;

        if (k == 0) {
            return;
        }

        nm->la = w[k - 1][0];
        nm->lb = w[k - 1][1];

        for (v = 1; v < k - 1; v++) {
            if (islower((unsigned char)s[w[v][0]])) {
                break;
            }
        }

        if (v < k - 1) {
            nm->la = w[v][0];
        }

        if (v > 0 && k > 1) {
            nm->fa = w[0][0];
            nm->fb = w[(v < k - 1 ? v : k - 1) - 1][1];
        }
    } else {            /* von Last, First   or   von Last, Jr, First */
        nm->la = a;
        nm->lb = c[0];
        nm->fa = c[nc - 1] + 1;
        nm->fb = b;

        if (nc == 2) {
            nm->ja = c[0] + 1;
            nm->jb = c[1];
        }
    }

    trim(s, &nm->la, &nm->lb);
    trim(s, &nm->fa, &nm->fb);
    trim(s, &nm->ja, &nm->jb);
}

/* the first letter of a name's piece, as LaTeX: "M", "{\"O}", "\'E" */
static void initial(pd_buf* o, const char* s, size_t a, size_t b) {
    if (a >= b) {
        return;
    }

    if (s[a] == '{') {
        size_t e = a + 1;
        int d = 1;

        while (e < b && d > 0) {
            d += s[e] == '{' ? 1 : s[e] == '}' ? -1 : 0;
            e++;
        }

        if (a + 1 < b && s[a + 1] == '\\') {
            pb_put(o, s + a, e - a);       /* {\"O}: the whole letter */
        } else {
            initial(o, s, a + 1, e > a + 1 && s[e - 1] == '}' ? e - 1 : e);
        }

        return;
    }

    if (s[a] == '\\' && a + 1 < b) {
        size_t e = a + 1;

        if (isalpha((unsigned char)s[e])) {
            while (e < b && isalpha((unsigned char)s[e])) {
                e++;
            }
        } else {
            e++;
        }

        if (e < b && s[e] == '{') {
            while (e < b && s[e] != '}') {
                e++;
            }

            e = e < b ? e + 1 : b;
        } else if (e < b) {
            e++;
        }

        pb_put(o, s + a, e - a);
        return;
    }

    {
        unsigned char c = (unsigned char)s[a];
        size_t l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;

        pb_put(o, s + a, a + l <= b ? l : b - a);
    }
}

/* given names as initials: "Mohammad Ali" "M. A.", "Jean-Pierre" "J.-P." */
static void initials(pd_buf* o, const char* s, size_t a, size_t b) {
    size_t w[32][2];
    int k = words(s, a, b, w, 32), j;

    for (j = 0; j < k; j++) {
        size_t p = w[j][0];

        if (j) {
            pb_putc(o, ' ');
        }

        while (p < w[j][1]) {
            size_t q = p;
            int d = 0;

            while (q < w[j][1] && (s[q] != '-' || d > 0)) {
                d += s[q] == '{' ? 1 : s[q] == '}' ? -1 : 0;
                q++;
            }

            if (q == p) {
                /* (an empty piece: nothing) */
            } else if (q - p <= 2 && s[q - 1] == '.') {
                pb_put(o, s + p, q - p);       /* already "J." */
            } else {
                initial(o, s, p, q);
                pb_putc(o, '.');
            }

            if (q < w[j][1]) {
                pb_putc(o, '-');
            }

            p = q + 1;
        }
    }
}

static void name_out(pd_buf* o, const char* s, const bname* nm, int abbrev) {
    if (nm->fb > nm->fa) {
        if (abbrev) {
            initials(o, s, nm->fa, nm->fb);
        } else {
            pb_put(o, s + nm->fa, nm->fb - nm->fa);
        }

        pb_putc(o, '~');
    }

    pb_put(o, s + nm->la, nm->lb - nm->la);

    if (nm->jb > nm->ja) {
        pb_puts(o, ", ");
        pb_put(o, s + nm->ja, nm->jb - nm->ja);
    }
}

static int is_others(const char* s, size_t a, size_t b) {
    return b - a == 6 && !strncmp(s + a, "others", 6);
}

/* a list of names: "A", "A and B", "A, B, and C", "A, B, et al." */
static void names_out(pd_buf* o, const char* s, int abbrev) {
    size_t r[BIB_MAX_NAMES][2];
    int k = split_names(s, r, BIB_MAX_NAMES), j;

    for (j = 0; j < k; j++) {
        bname nm;

        if (is_others(s, r[j][0], r[j][1])) {
            pb_puts(o, j ? (j > 1 ? ", et~al." : " et~al.") : "et~al.");
            return;
        }

        if (j) {
            pb_puts(o, k == 2 ? " and " : j == k - 1 ? ", and " : ", ");
        }

        name_parts(s, r[j][0], r[j][1], &nm);
        name_out(o, s, &nm, abbrev);
    }
}

/* the short form a citation names: "Smith", "Smith and Jones", "Smith et al." */
static void short_names(pd_buf* o, const char* s) {
    size_t r[3][2];
    int k = split_names(s, r, 3), j;

    for (j = 0; j < k && j < 2; j++) {
        bname nm;

        if (is_others(s, r[j][0], r[j][1]) || (k > 2)) {
            if (j == 0) {
                name_parts(s, r[0][0], r[0][1], &nm);
                pb_put(o, s + nm.la, nm.lb - nm.la);
            }

            pb_puts(o, " et~al.");
            return;
        }

        if (j) {
            pb_puts(o, " and ");
        }

        name_parts(s, r[j][0], r[j][1], &nm);
        pb_put(o, s + nm.la, nm.lb - nm.la);
    }
}

/* ------------------------------------------------------------------ */
/* the entry set out                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    pd_buf* o;
    int open;       /* a block started: the next piece after ", " */
} blk;

static int ends_sentence(const pd_buf* o) {
    size_t n = o->n;

    while (n > 0 && (o->p[n - 1] == '}' || o->p[n - 1] == ' ')) {
        n--;
    }

    return n > 0 && strchr(".?!", o->p[n - 1]) != NULL;
}

static void piece(blk* B, const char* pre, const char* text, const char* post) {
    if (!text) {
        return;
    }

    if (B->open) {
        pb_puts(B->o, ", ");
    }

    pb_puts(B->o, pre);
    pb_puts(B->o, text);
    pb_puts(B->o, post);
    B->open = 1;
}

static void block_end(blk* B) {
    if (B->open) {
        if (!ends_sentence(B->o)) {
            pb_putc(B->o, '.');
        }

        pb_putc(B->o, ' ');
    }

    B->open = 0;
}

/* pages: "13-18" with an en dash */
static void pages_out(pd_buf* o, const char* p) {
    for (; *p; p++) {
        if (*p == '-') {
            while (p[1] == '-') {
                p++;
            }

            pb_puts(o, "--");
        } else {
            pb_putc(o, *p);
        }
    }
}

/* a URL's characters a LaTeX group would take for its own, escaped */
static void url_out(pd_buf* o, const char* u) {
    for (; *u; u++) {
        if (strchr("%#{}", *u)) {
            pb_putc(o, '\\');
        }

        pb_putc(o, *u);
    }
}

static void date_out(blk* B, const bentry* e) {
    char y[16];
    const char* m = get1(e, "month");
    pd_buf t;

    year_of(e, y, sizeof(y));

    if (!y[0]) {
        return;
    }

    memset(&t, 0, sizeof(t));

    if (m) {
        int k = atoi(m);

        pb_puts(&t, k >= 1 && k <= 12 ? MONTHS[k - 1] : m);
        pb_putc(&t, ' ');
    }

    pb_puts(&t, y);
    pb_putc(&t, '\0');

    if (!t.err) {
        piece(B, "", t.p, "");
    }

    pb_free(&t);
}

static int type_is(const bentry* e, const char* list) {
    const char* p = list;
    size_t n = strlen(e->type);

    if (n == 0) {
        return 0;   /* (an entry with no type: none of them; strstr would find "" everywhere) */
    }

    while ((p = strstr(p, e->type)) != NULL) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) {
            return 1;
        }

        p += n;
    }

    return 0;
}

static void entry_out(pd_buf* o, const bentry* e, int abbrev) {
    const char* au = get(e, "author");
    const char* ed = get(e, "editor");
    const char* title = get(e, "title");
    const char* doi = get(e, "doi");
    const char* url = get(e, "url");
    char y[16];
    blk B;

    B.o = o;
    B.open = 0;
    year_of(e, y, sizeof(y));

    /* \bibitem[Smith et al.(2020)]{key} */
    pb_puts(o, "\\bibitem[");

    if (au || ed) {
        short_names(o, au ? au : ed);
    } else {
        pb_puts(o, e->key);
    }

    pb_printf(o, "(%s)]{%s} ", y, e->key);

    /* who */
    if (au) {
        names_out(o, au, abbrev);
        B.open = 1;
        block_end(&B);
    } else if (ed && !type_is(e, "incollection inproceedings conference")) {
        names_out(o, ed, abbrev);
        {
            size_t r[2][2];

            pb_puts(o, split_names(ed, r, 2) > 1 ? ", editors" : ", editor");
        }
        B.open = 1;
        block_end(&B);
    }

    if (type_is(e, "article")) {
        const char* j = get(e, "journal");
        const char* v = get(e, "volume");
        const char* nu = get(e, "number");
        const char* pg = get(e, "pages");

        piece(&B, "", title, "");
        block_end(&B);
        piece(&B, "\\emph{", j, "}");

        if (v) {
            if (B.open) {
                pb_puts(o, ", ");
            }

            pb_puts(o, v);

            if (nu) {
                pb_printf(o, "(%s)", nu);
            }

            if (pg) {
                pb_putc(o, ':');
                pages_out(o, pg);
            }

            B.open = 1;
        } else if (pg) {
            if (B.open) {
                pb_puts(o, ", ");
            }

            pages_out(o, pg);
            B.open = 1;
        }

        date_out(&B, e);
        block_end(&B);
    } else if (type_is(e, "book booklet manual proceedings collection mvbook periodical")) {
        piece(&B, "\\emph{", title, "}");
        piece(&B, "", get(e, "edition"), " edition");
        block_end(&B);
        piece(&B, "", get(e, "series"), "");
        piece(&B, "volume ", get(e, "volume"), "");
        piece(&B, "", get(e, "publisher") ? get(e, "publisher") : get(e, "organization"), "");
        piece(&B, "", get(e, "address"), "");
        date_out(&B, e);
        block_end(&B);
    } else if (type_is(e, "inproceedings conference incollection inbook")) {
        const char* bt = get(e, "booktitle");

        if (type_is(e, "inbook") && !bt) {
            piece(&B, "\\emph{", title, "}");
            piece(&B, "chapter ", get(e, "chapter"), "");
        } else {
            piece(&B, "", title, "");
        }

        block_end(&B);

        if (bt) {
            pb_puts(o, "In ");

            if (ed && au) {
                names_out(o, ed, abbrev);
                {
                    size_t r[2][2];

                    pb_puts(o, split_names(ed, r, 2) > 1 ? ", editors, " : ", editor, ");
                }
            }

            pb_puts(o, "\\emph{");
            pb_puts(o, bt);
            pb_putc(o, '}');
            B.open = 1;
        }

        piece(&B, "volume ", get(e, "volume"), "");
        if (get(e, "pages")) {
            if (B.open) {
                pb_puts(o, ", ");
            }

            pb_puts(o, strchr(get(e, "pages"), '-') ? "pages " : "page ");
            pages_out(o, get(e, "pages"));
            B.open = 1;
        }

        piece(&B, "", get(e, "address"), "");
        piece(&B, "", get(e, "publisher") ? get(e, "publisher") : get(e, "organization"), "");
        date_out(&B, e);
        block_end(&B);
    } else if (type_is(e, "phdthesis mastersthesis thesis")) {
        const char* kind = get1(e, "type");

        piece(&B, "\\emph{", title, "}");
        block_end(&B);
        piece(&B, "", kind ? kind : !strcmp(e->type, "mastersthesis") ? "Master's thesis" : "PhD thesis", "");
        piece(&B, "", get(e, "school"), "");
        piece(&B, "", get(e, "address"), "");
        date_out(&B, e);
        block_end(&B);
    } else if (type_is(e, "techreport report")) {
        const char* kind = get1(e, "type");
        const char* nu = get(e, "number");

        piece(&B, "", title, "");
        block_end(&B);

        if (B.open) {
            pb_puts(o, ", ");
        }

        pb_puts(o, kind ? kind : "Technical Report");

        if (nu) {
            pb_printf(o, " %s", nu);
        }

        B.open = 1;
        piece(&B, "", get(e, "institution"), "");
        piece(&B, "", get(e, "address"), "");
        date_out(&B, e);
        block_end(&B);
    } else {        /* misc, online, unpublished, software, dataset, ... */
        piece(&B, "", title, "");
        block_end(&B);
        piece(&B, "", get(e, "howpublished"), "");
        piece(&B, "", get(e, "publisher") ? get(e, "publisher") : get(e, "organization"), "");
        date_out(&B, e);
        block_end(&B);
    }

    if (get(e, "note")) {
        piece(&B, "", get(e, "note"), "");
        block_end(&B);
    }

    if (doi) {
        const char* d = doi;

        if (!bib_ncase(d, "https://doi.org/", 16)) {
            d += 16;
        } else if (!bib_ncase(d, "http://dx.doi.org/", 18)) {
            d += 18;
        }

        pb_puts(o, "doi:\\href{https://doi.org/");
        url_out(o, d);
        pb_puts(o, "}{\\nolinkurl{");
        url_out(o, d);
        pb_puts(o, "}} ");
    } else if (url && (type_is(e, "misc online electronic www software dataset manual unpublished") ||
                       !(get(e, "journal") || get(e, "booktitle") || get(e, "publisher")))) {
        pb_puts(o, "\\url{");
        url_out(o, url);
        pb_puts(o, "} ");
    }

    pb_putc(o, '\n');
}

/* ------------------------------------------------------------------ */
/* sorting                                                            */
/* ------------------------------------------------------------------ */

/* a field's letters and digits, lowered: what entries are ordered by */
static void purify(pd_buf* o, const char* s) {
    for (; s && *s; s++) {
        if (*s == '\\') {
            s++;

            while (*s && isalpha((unsigned char)*s) && isalpha((unsigned char)s[1])) {
                s++;    /* a command's name: \ss, \o (its last letter kept) */
            }

            if (!*s) {
                break;
            }

            if (!isalpha((unsigned char)*s)) {
                continue;
            }
        }

        if (isalnum((unsigned char)*s) || (unsigned char)*s >= 0x80) {
            pb_putc(o, (char)tolower((unsigned char)*s));
        } else if (*s == ' ' || *s == ',') {
            pb_putc(o, ' ');
        }
    }
}

static void make_sortkey(bentry* e) {
    const char* au = get(e, "author") ? get(e, "author") : get(e, "editor");
    pd_buf o;
    char y[16];

    memset(&o, 0, sizeof(o));

    if (au) {
        size_t r[BIB_MAX_NAMES][2];
        int k = split_names(au, r, BIB_MAX_NAMES), j;

        for (j = 0; j < k; j++) {
            bname nm;
            char* part;

            name_parts(au, r[j][0], r[j][1], &nm);
            part = bib_dup(au + nm.la, nm.lb - nm.la);
            purify(&o, part);
            free(part);
            pb_putc(&o, ' ');
            part = bib_dup(au + nm.fa, nm.fb - nm.fa);
            purify(&o, part);
            free(part);
            pb_puts(&o, "  ");
        }
    } else {
        purify(&o, e->key);
    }

    year_of(e, y, sizeof(y));
    pb_printf(&o, "\x01%s\x01", y);
    purify(&o, get(e, "title"));
    pb_putc(&o, '\0');
    e->sortkey = o.err ? NULL : o.p;

    if (o.err) {
        pb_free(&o);
    }
}

static int by_sortkey(const void* a, const void* b) {
    const bentry* x = *(const bentry* const*)a;
    const bentry* y = *(const bentry* const*)b;

    return strcmp(x->sortkey ? x->sortkey : "", y->sortkey ? y->sortkey : "");
}

/* ------------------------------------------------------------------ */
/* the list                                                           */
/* ------------------------------------------------------------------ */

char* pd_bibtex_thebibliography(const char* bib, size_t n, const char* const* keys, int nkeys, int all,
                                int sorted, int abbrev, size_t* len) {
    bparse P;
    bentry** pick;
    int np = 0, k;
    pd_buf o;

    memset(&P, 0, sizeof(P));
    memset(&o, 0, sizeof(o));
    P.s = bib;
    P.n = n;
    parse(&P);
    inherit(&P);

    pick = (bentry**)calloc((size_t)P.ne + 1, sizeof(bentry*));

    if (!pick) {
        goto done;
    }

    /* the cited, in the order cited (each once), then with \nocite{*} the rest in the file's order */
    for (k = 0; k < nkeys; k++) {
        bentry* e = find_entry(&P, keys[k], strlen(keys[k]));

        if (!e) {
            continue;
        }

        if (!e->picked) {
            e->picked = 1;
            pick[np++] = e;
        }
    }

    for (k = 0; all && k < P.ne; k++) {
        if (!P.e[k].picked) {
            P.e[k].picked = 1;
            pick[np++] = &P.e[k];
        }
    }

    if (sorted) {
        for (k = 0; k < np; k++) {
            make_sortkey(pick[k]);
        }

        qsort(pick, (size_t)np, sizeof(pick[0]), by_sortkey);
    }

    pb_puts(&o, "{99}\n");

    for (k = 0; k < np; k++) {
        entry_out(&o, pick[k], abbrev);
    }

done:
    free(pick);

    for (k = 0; k < P.ne; k++) {
        free_entry(&P.e[k]);
    }

    free(P.e);

    for (k = 0; k < P.nstr; k++) {
        free(P.str[k].val);
    }

    free(P.str);
    pb_putc(&o, '\0');

    if (o.err) {
        pb_free(&o);
        return NULL;
    }

    *len = o.n - 1;
    return o.p;
}
