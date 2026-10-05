/*
 * Parade text segmentation (Unicode 15.1)
 *
 *  - UAX #14 line breaking: rules LB2-LB31 evaluated per boundary, with
 *    LB9/LB10 combining-mark absorption, SP* look-behind (LB8, LB14,
 *    LB15a, LB16, LB17), quotation (LB15a/b), Brahmic (LB28a), regional
 *    indicators (LB30a) and emoji modifiers (LB30b)
 *  - UAX #29 grapheme clusters: GB3-GB999 including InCB (GB9c) and emoji
 *    ZWJ sequences (GB11)
 * Both are checked against the official LineBreakTest.txt and
 * GraphemeBreakTest.txt (make conformance).
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_unidata.h"

uint32_t pd_uni_mirror(uint32_t cp) {
    int32_t lo = 0, hi = pd_uni_nmirrors;

    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;

        if (pd_uni_mirrors[mid][0] < cp) {
            lo = mid + 1;
        } else if (pd_uni_mirrors[mid][0] > cp) {
            hi = mid;
        } else {
            return pd_uni_mirrors[mid][1];
        }
    }

    return cp;
}

int pd_uni_bracket(uint32_t cp, uint32_t* pair) {
    int32_t lo = 0, hi = pd_uni_nbrackets;

    while (lo < hi) {
        int32_t mid = (lo + hi) / 2;

        if (pd_uni_brackets[mid][0] < cp) {
            lo = mid + 1;
        } else if (pd_uni_brackets[mid][0] > cp) {
            hi = mid;
        } else {
            *pair = pd_uni_brackets[mid][1];
            return (int)pd_uni_brackets[mid][2];
        }
    }

    *pair = cp;
    return 0;
}

/* decode UTF-8 into code points with their byte offsets; invalid bytes become U+FFFD */
int32_t pd_text_decode(const char* utf8, size_t len, uint32_t** cps, uint32_t** offs) {
    const unsigned char* s = (const unsigned char*)utf8;
    size_t i = 0;
    int32_t n = 0;

    *cps = (uint32_t*)malloc((len + 1) * sizeof(uint32_t));
    *offs = (uint32_t*)malloc((len + 1) * sizeof(uint32_t));

    if (!*cps || !*offs) {
        free(*cps);
        free(*offs);
        *cps = *offs = NULL;
        return -1;
    }

    while (i < len) {
        unsigned c = s[i];
        uint32_t cp = 0xFFFD;
        int k, extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;

        (*offs)[n] = (uint32_t)i;

        if (extra == 0) {
            cp = c;
            i++;
        } else if (extra > 0 && i + (size_t)extra < len) {
            cp = c & (0x3F >> extra);

            for (k = 1; k <= extra; k++) {
                if ((s[i + k] & 0xC0) != 0x80) {
                    break;
                }

                cp = (cp << 6) | (s[i + k] & 0x3F);
            }

            if (k <= extra) {
                cp = 0xFFFD;
                i++;
            } else {
                i += (size_t)extra + 1;
            }
        } else {
            i++;
        }

        (*cps)[n++] = cp;
    }

    (*offs)[n] = (uint32_t)len;
    return n;
}

/* ------------------------------------------------------------------ */
/* UAX #14                                                            */
/* ------------------------------------------------------------------ */

#define IS(c, set) ((1ULL << (c)) & (set))
#define B(c) (1ULL << LB_##c)

/* does "NU (NU|SY|IS)*" end at code point k (a base position) */
static int numeric_run_ends_at(const uint8_t* eff, const int32_t* base, int32_t k) {
    while (k >= 0 && IS(eff[k], B(NU) | B(SY) | B(IS))) {
        if (eff[k] == LB_NU) {
            return 1;
        }

        k = k > 0 ? base[k - 1] : -1;
    }

    return 0;
}

/*
 * LB25 replaced by the tailoring of UAX #14 Example 7, which keeps whole
 * numbers such as "$(12.35)" together (and is what LineBreakTest uses):
 *   (PR | PO) × (OP | HY)? NU
 *   (OP | HY) × NU
 *   NU × (NU | SY | IS)
 *   NU (NU | SY | IS)* × (NU | SY | IS | CL | CP)
 *   NU (NU | SY | IS)* (CL | CP)? × (PO | PR)
 */
static int number_rule(const uint8_t* eff, const int32_t* base, int32_t n, int32_t i, int A, int Bc) {
    int32_t pb = base[i - 1], k = i + 1;

    while (k < n && base[k] == i) {     /* the character after i, past its marks */
        k++;
    }

    if (IS(A, B(PR) | B(PO)) && (Bc == LB_NU || (IS(Bc, B(OP) | B(HY)) && k < n && eff[k] == LB_NU))) {
        return 1;
    }

    if (IS(A, B(OP) | B(HY)) && Bc == LB_NU) {
        return 1;
    }

    if (IS(Bc, B(NU) | B(SY) | B(IS) | B(CL) | B(CP)) && numeric_run_ends_at(eff, base, pb)) {
        return 1;
    }

    if (IS(Bc, B(PO) | B(PR))) {
        int32_t q = pb;

        if (IS(eff[q], B(CL) | B(CP))) {
            q = q > 0 ? base[q - 1] : -1;
        }

        if (q >= 0 && numeric_run_ends_at(eff, base, q)) {
            return 1;
        }
    }

    return 0;
}

/*
 * brk[i] for the boundary before code point i (1 <= i < n):
 * 0 no break, 1 break allowed, 2 mandatory break. brk[0] = 0 (LB2) and
 * brk[n] = 2 (LB3).
 */
int pd_linebreaks(const uint32_t* cp, int32_t n, uint8_t* brk) {
    uint8_t* raw = (uint8_t*)malloc((size_t)n + 1);
    uint8_t* eff = (uint8_t*)malloc((size_t)n + 1);
    int32_t* base = (int32_t*)malloc(((size_t)n + 1) * sizeof(int32_t));
    int32_t i;

    if (!raw || !eff || !base) {
        free(raw);
        free(eff);
        free(base);
        return -1;
    }

    /* LB9/LB10: combining marks take their base's class */
    for (i = 0; i < n; i++) {
        int c = pd_uni_lb(cp[i]);

        raw[i] = (uint8_t)c;
        base[i] = i;

        if (c == LB_CM || c == LB_ZWJ) {
            if (i > 0 && !IS(eff[i - 1], B(BK) | B(CR) | B(LF) | B(NL) | B(SP) | B(ZW))) {
                eff[i] = eff[i - 1];
                base[i] = base[i - 1];
                continue;
            }

            c = LB_AL;
        }

        eff[i] = (uint8_t)c;
    }

    brk[0] = 0;

    for (i = 1; i < n; i++) {
        int a = raw[i - 1], b = raw[i], A = eff[i - 1], Bc = eff[i], r = -1;
        int32_t j, pb = base[i - 1];
        int last_nonsp;     /* effective class of the last non-space before the boundary */
        int32_t lj;

        for (lj = i - 1; lj >= 0 && raw[lj] == LB_SP; lj--) {
        }

        last_nonsp = lj >= 0 ? eff[lj] : -1;

        if (a == LB_BK) {                               /* LB4 */
            r = 2;
        } else if (a == LB_CR && b == LB_LF) {          /* LB5 */
            r = 0;
        } else if (a == LB_CR || a == LB_LF || a == LB_NL) {
            r = 2;
        } else if (IS(b, B(BK) | B(CR) | B(LF) | B(NL))) {     /* LB6 */
            r = 0;
        } else if (b == LB_SP || b == LB_ZW) {          /* LB7 */
            r = 0;
        } else if (lj >= 0 && raw[lj] == LB_ZW) {       /* LB8: ZW SP* ÷ */
            r = 1;
        } else if (a == LB_ZWJ) {                       /* LB8a */
            r = 0;
        } else if ((b == LB_CM || b == LB_ZWJ) && base[i] != i) {   /* LB9 */
            r = 0;
        } else if (A == LB_WJ || Bc == LB_WJ) {         /* LB11 */
            r = 0;
        } else if (A == LB_GL) {                        /* LB12 */
            r = 0;
        } else if (Bc == LB_GL && !IS(A, B(SP) | B(BA) | B(HY))) {     /* LB12a */
            r = 0;
        } else if (Bc == LB_EX || (IS(Bc, B(CL) | B(CP) | B(IS) | B(SY)) && A != LB_NU)) {
            r = 0;                                      /* LB13, tailored for numbers (UAX #14 Example 7) */
        } else if (last_nonsp == LB_OP) {               /* LB14: OP SP* × */
            r = 0;
        } else if (lj >= 0 && eff[lj] == LB_QU && pd_uni_pi(cp[base[lj]]) &&
                   (base[lj] == 0 || IS(eff[base[lj] - 1], B(BK) | B(CR) | B(LF) | B(NL) | B(OP) | B(QU) | B(GL) | B(SP) |
                                        B(ZW)))) {
            r = 0;                                      /* LB15a */
        } else if (Bc == LB_QU && pd_uni_pf(cp[i])) {   /* LB15b: look past the quote's combining marks */
            int32_t k = i + 1;

            while (k < n && base[k] == i) {
                k++;
            }

            if (k >= n || IS(eff[k], B(SP) | B(GL) | B(WJ) | B(CL) | B(QU) | B(CP) | B(EX) | B(IS) | B(SY) | B(BK) |
                             B(CR) | B(LF) | B(NL) | B(ZW))) {
                r = 0;
            }
        }

        if (r < 0) {
            if ((last_nonsp == LB_CL || last_nonsp == LB_CP) && Bc == LB_NS) {    /* LB16 */
                r = 0;
            } else if (last_nonsp == LB_B2 && Bc == LB_B2) {   /* LB17 */
                r = 0;
            } else if (a == LB_SP) {                    /* LB18 */
                r = 1;
            } else if (Bc == LB_QU || A == LB_QU) {     /* LB19 */
                r = 0;
            } else if (Bc == LB_CB || A == LB_CB) {     /* LB20 */
                r = 1;
            } else if (IS(Bc, B(BA) | B(HY) | B(NS)) || A == LB_BB) {    /* LB21 */
                r = 0;
            } else if (IS(A, B(HY) | B(BA)) && pb > 0 && eff[pb - 1] == LB_HL) {     /* LB21a */
                r = 0;
            } else if (A == LB_SY && Bc == LB_HL) {     /* LB21b */
                r = 0;
            } else if (Bc == LB_IN) {                   /* LB22 */
                r = 0;
            } else if ((IS(A, B(AL) | B(HL)) && Bc == LB_NU) || (A == LB_NU && IS(Bc, B(AL) | B(HL)))) {    /* LB23 */
                r = 0;
            } else if ((A == LB_PR && IS(Bc, B(ID) | B(EB) | B(EM))) || (IS(A, B(ID) | B(EB) | B(EM)) && Bc == LB_PO)) {
                r = 0;                                  /* LB23a */
            } else if ((IS(A, B(PR) | B(PO)) && IS(Bc, B(AL) | B(HL))) || (IS(A, B(AL) | B(HL)) && IS(Bc, B(PR) | B(PO)))) {
                r = 0;                                  /* LB24 */
            } else if (number_rule(eff, base, n, i, A, Bc)) {
                r = 0;                                  /* LB25 as the regex-number tailoring */
            } else if ((A == LB_JL && IS(Bc, B(JL) | B(JV) | B(H2) | B(H3))) || (IS(A, B(JV) | B(H2)) &&
                       IS(Bc, B(JV) | B(JT))) || (IS(A, B(JT) | B(H3)) && Bc == LB_JT)) {
                r = 0;                                  /* LB26 */
            } else if ((IS(A, B(JL) | B(JV) | B(JT) | B(H2) | B(H3)) && Bc == LB_PO) || (A == LB_PR &&
                       IS(Bc, B(JL) | B(JV) | B(JT) | B(H2) | B(H3)))) {
                r = 0;                                  /* LB27 */
            } else if (IS(A, B(AL) | B(HL)) && IS(Bc, B(AL) | B(HL))) {   /* LB28 */
                r = 0;
            }
        }

        if (r < 0) {    /* LB28a: Brahmic orthographic syllables; ◌ U+25CC counts as AK */
            int akp = A == LB_AK || A == LB_AS || cp[pb] == 0x25CC;
            int akc = Bc == LB_AK || Bc == LB_AS || cp[i] == 0x25CC;
            int32_t k = i + 1;

            while (k < n && base[k] == i) {
                k++;
            }

            if (A == LB_AP && akc) {
                r = 0;
            } else if (akp && (Bc == LB_VF || Bc == LB_VI)) {
                r = 0;
            } else if (A == LB_VI && pb > 0 && (eff[pb - 1] == LB_AK || eff[pb - 1] == LB_AS || cp[base[pb - 1]] == 0x25CC) &&
                       (Bc == LB_AK || cp[i] == 0x25CC)) {
                r = 0;
            } else if (akp && akc && k < n && eff[k] == LB_VF) {
                r = 0;
            }
        }

        if (r < 0) {
            if (A == LB_IS && IS(Bc, B(AL) | B(HL))) {  /* LB29 */
                r = 0;
            } else if ((IS(A, B(AL) | B(HL) | B(NU)) && Bc == LB_OP && !pd_uni_lb_eawide(cp[i])) ||
                       (A == LB_CP && !pd_uni_lb_eawide(cp[pb]) && IS(Bc, B(AL) | B(HL) | B(NU)))) {
                r = 0;                                  /* LB30 */
            } else if (A == LB_RI && Bc == LB_RI) {     /* LB30a: pairs of regional indicators */
                int32_t cnt = 0;

                for (j = i - 1; j >= 0; j--) {
                    if (base[j] != j) {
                        continue;   /* absorbed marks do not count */
                    }

                    if (eff[j] != LB_RI) {
                        break;
                    }

                    cnt++;
                }

                r = (cnt % 2) ? 0 : 1;
            } else if ((A == LB_EB && Bc == LB_EM) || (pd_uni_extpict(cp[pb]) && pd_uni_unassigned(cp[pb]) && Bc == LB_EM)) {
                r = 0;                                  /* LB30b */
            } else {
                r = 1;                                  /* LB31 */
            }
        }

        brk[i] = (uint8_t)r;
    }

    brk[n] = 2;     /* LB3 */
    free(raw);
    free(eff);
    free(base);
    return 0;
}

/* ------------------------------------------------------------------ */
/* UAX #29 grapheme clusters                                          */
/* ------------------------------------------------------------------ */

/* is there a grapheme boundary between cp[i-1] and cp[i] */
int pd_grapheme_boundary(const uint32_t* cp, int32_t n, int32_t i) {
    int a, b;
    int32_t j;

    if (i <= 0 || i >= n) {
        return 1;   /* GB1, GB2 */
    }

    a = pd_uni_gcb(cp[i - 1]);
    b = pd_uni_gcb(cp[i]);

    if (a == GCB_CR && b == GCB_LF) {   /* GB3 */
        return 0;
    }

    if (a == GCB_CONTROL || a == GCB_CR || a == GCB_LF || b == GCB_CONTROL || b == GCB_CR || b == GCB_LF) {
        return 1;   /* GB4, GB5 */
    }

    if ((a == GCB_L && (b == GCB_L || b == GCB_V || b == GCB_LV || b == GCB_LVT)) ||
            ((a == GCB_LV || a == GCB_V) && (b == GCB_V || b == GCB_T)) || ((a == GCB_LVT || a == GCB_T) && b == GCB_T)) {
        return 0;   /* GB6-GB8 */
    }

    if (b == GCB_EXTEND || b == GCB_ZWJ || b == GCB_SPACINGMARK || a == GCB_PREPEND) {
        return 0;   /* GB9, GB9a, GB9b */
    }

    /* GB9c: Consonant [Extend Linker]* Linker [Extend Linker]* × Consonant */
    if (pd_uni_incb(cp[i]) == INCB_CONSONANT) {
        int linker = 0;

        for (j = i - 1; j >= 0; j--) {
            int k = pd_uni_incb(cp[j]);

            if (k == INCB_LINKER) {
                linker = 1;
            } else if (k != INCB_EXTEND) {
                break;
            }
        }

        if (linker && j >= 0 && pd_uni_incb(cp[j]) == INCB_CONSONANT) {
            return 0;
        }
    }

    /* GB11: ExtPict Extend* ZWJ × ExtPict */
    if (a == GCB_ZWJ && pd_uni_extpict(cp[i])) {
        for (j = i - 2; j >= 0 && pd_uni_gcb(cp[j]) == GCB_EXTEND; j--) {
        }

        if (j >= 0 && pd_uni_extpict(cp[j])) {
            return 0;
        }
    }

    /* GB12, GB13: regional indicators pair up */
    if (a == GCB_RI && b == GCB_RI) {
        int32_t cnt = 0;

        for (j = i - 1; j >= 0 && pd_uni_gcb(cp[j]) == GCB_RI; j--) {
            cnt++;
        }

        return cnt % 2 == 0;
    }

    return 1;   /* GB999 */
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

static size_t grapheme_step(const char* utf8, size_t len, size_t off, int dir) {
    uint32_t* cps, *offs;
    int32_t n, i, k;
    size_t r = off;

    if (!utf8 || len == 0) {
        return 0;
    }

    off = off > len ? len : off;
    n = pd_text_decode(utf8, len, &cps, &offs);

    if (n < 0) {
        return off;
    }

    for (i = 0; i < n && offs[i] < off; i++) {
    }

    /* i: first code point at or after off */
    if (dir > 0) {
        for (k = i + 1; k <= n && !pd_grapheme_boundary(cps, n, k); k++) {
        }

        r = i >= n ? len : offs[k > n ? n : k];
    } else {
        for (k = i - 1; k > 0 && !pd_grapheme_boundary(cps, n, k); k--) {
        }

        r = i <= 0 ? 0 : offs[k < 0 ? 0 : k];
    }

    free(cps);
    free(offs);
    return r;
}

size_t pd_text_next_grapheme(const char* utf8, size_t len, size_t off) {
    return grapheme_step(utf8, len, off, 1);
}

size_t pd_text_prev_grapheme(const char* utf8, size_t len, size_t off) {
    return grapheme_step(utf8, len, off, -1);
}

pd_status pd_text_line_breaks(const char* utf8, size_t len, uint8_t* out) {
    uint32_t* cps, *offs;
    uint8_t* brk;
    int32_t n, i;

    if (!out || (!utf8 && len)) {
        return PD_ERR_ARG;
    }

    memset(out, 0, len + 1);
    n = pd_text_decode(utf8 ? utf8 : "", len, &cps, &offs);

    if (n < 0) {
        return PD_ERR_NOMEM;
    }

    brk = (uint8_t*)malloc((size_t)n + 1);

    if (!brk || pd_linebreaks(cps, n, brk)) {
        free(brk);
        free(cps);
        free(offs);
        return PD_ERR_NOMEM;
    }

    for (i = 1; i <= n; i++) {
        out[offs[i]] = brk[i];
    }

    free(brk);
    free(cps);
    free(offs);
    return PD_OK;
}
