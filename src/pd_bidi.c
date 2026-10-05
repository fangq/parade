/*
 * Parade bidirectional text: the Unicode Bidirectional Algorithm (UAX #9, 15.1)
 *
 *  P2/P3  paragraph level (first strong character, isolates skipped)
 *  X1-X8  explicit embeddings, overrides and isolates (stack depth 125)
 *  X9     removal of embedding/override controls and BN
 *  X10    isolating run sequences with sos/eos
 *  W1-W7, N0 (bracket pairs, BD16), N1-N2, I1-I2 per sequence
 *  L1     whitespace reset at line ends (per line), L2 reordering
 * Checked against BidiCharacterTest.txt (make conformance).
 */

#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_unidata.h"

#define MAX_DEPTH 125
#define REMOVED 0xFF

static int is_isolate_initiator(int t) {
    return t == BC_LRI || t == BC_RLI || t == BC_FSI;
}

static int is_removed_class(int t) {
    return t == BC_RLE || t == BC_LRE || t == BC_RLO || t == BC_LRO || t == BC_PDF || t == BC_BN;
}

/* index of the PDI matching the isolate initiator at i, or n */
static int32_t matching_pdi(const uint8_t* cls, int32_t n, int32_t i) {
    int32_t depth = 1, k;

    for (k = i + 1; k < n; k++) {
        if (is_isolate_initiator(cls[k])) {
            depth++;
        } else if (cls[k] == BC_PDI && --depth == 0) {
            return k;
        } else if (cls[k] == BC_B) {
            return n;
        }
    }

    return n;
}

/* P2/P3: 1 for RTL, 0 for LTR, -1 when no strong character (in [from, to)) */
static int first_strong(const uint8_t* cls, int32_t n, int32_t from, int32_t to) {
    int32_t k;

    for (k = from; k < to; k++) {
        int t = cls[k];

        if (t == BC_L) {
            return 0;
        }

        if (t == BC_R || t == BC_AL) {
            return 1;
        }

        if (is_isolate_initiator(t)) {
            k = matching_pdi(cls, n, k);
        } else if (t == BC_B) {
            break;
        }
    }

    return -1;
}

typedef struct {
    int32_t* idx;               /* positions of the sequence's characters */
    int32_t n;
    int level;
    int sos, eos;               /* BC_L or BC_R */
} seqrun;

static int strong_of(int t) {
    return (t == BC_EN || t == BC_AN || t == BC_AL) ? BC_R : t;
}

static int is_ni(int t) {
    return t == BC_B || t == BC_S || t == BC_WS || t == BC_ON || is_isolate_initiator(t) || t == BC_PDI;
}

/* canonical equivalents for bracket matching (U+2329/232A ~ U+3008/3009) */
static uint32_t canon_bracket(uint32_t c) {
    return c == 0x2329 ? 0x3008 : c == 0x232A ? 0x3009 : c;
}

static void resolve_sequence(const uint32_t* cp, const uint8_t* orig, uint8_t* t, uint8_t* lev, const seqrun* s) {
    int32_t i, k, n = s->n;
    const int32_t* ix = s->idx;
    int e = (s->level & 1) ? BC_R : BC_L;

    /* W1: NSM takes the previous type (ON after an isolate initiator or PDI) */
    for (i = 0; i < n; i++) {
        if (t[ix[i]] == BC_NSM) {
            if (i == 0) {
                t[ix[i]] = (uint8_t)s->sos;
            } else {
                int pt = t[ix[i - 1]];
                t[ix[i]] = (uint8_t)((is_isolate_initiator(pt) || pt == BC_PDI) ? BC_ON : pt);
            }
        }
    }

    /* W2: EN after AL becomes AN; W3: AL becomes R */
    {
        int last = s->sos;

        for (i = 0; i < n; i++) {
            int x = t[ix[i]];

            if (x == BC_L || x == BC_R || x == BC_AL) {
                last = x;
            } else if (x == BC_EN && last == BC_AL) {
                t[ix[i]] = BC_AN;
            }
        }

        for (i = 0; i < n; i++) {
            if (t[ix[i]] == BC_AL) {
                t[ix[i]] = BC_R;
            }
        }
    }

    /* W4: single separators between numbers */
    for (i = 1; i + 1 < n; i++) {
        int a = t[ix[i - 1]], x = t[ix[i]], b = t[ix[i + 1]];

        if (x == BC_ES && a == BC_EN && b == BC_EN) {
            t[ix[i]] = BC_EN;
        } else if (x == BC_CS && a == BC_EN && b == BC_EN) {
            t[ix[i]] = BC_EN;
        } else if (x == BC_CS && a == BC_AN && b == BC_AN) {
            t[ix[i]] = BC_AN;
        }
    }

    /* W5: ET sequences adjacent to EN become EN */
    for (i = 0; i < n; i++) {
        if (t[ix[i]] == BC_ET) {
            int32_t j = i;

            while (j < n && t[ix[j]] == BC_ET) {
                j++;
            }

            if ((i > 0 && t[ix[i - 1]] == BC_EN) || (j < n && t[ix[j]] == BC_EN)) {
                for (k = i; k < j; k++) {
                    t[ix[k]] = BC_EN;
                }
            }

            i = j - 1;
        }
    }

    /* W6: remaining separators and terminators become ON */
    for (i = 0; i < n; i++) {
        int x = t[ix[i]];

        if (x == BC_ES || x == BC_ET || x == BC_CS) {
            t[ix[i]] = BC_ON;
        }
    }

    /* W7: EN after L (or sos L) becomes L */
    {
        int last = s->sos;

        for (i = 0; i < n; i++) {
            int x = t[ix[i]];

            if (x == BC_L || x == BC_R) {
                last = x;
            } else if (x == BC_EN && last == BC_L) {
                t[ix[i]] = BC_L;
            }
        }
    }

    /* N0: bracket pairs (BD16) */
    {
        int32_t stack_pos[63], pairs[512][2], npairs = 0, sp = 0;
        uint32_t stack_cp[63];

        for (i = 0; i < n; i++) {
            uint32_t pair, c = cp[ix[i]];
            int bt;

            if (t[ix[i]] != BC_ON) {
                continue;
            }

            bt = pd_uni_bracket(c, &pair);

            if (bt == 1) {
                if (sp == 63) {
                    break;  /* BD16: stop processing */
                }

                stack_cp[sp] = canon_bracket(pair);
                stack_pos[sp++] = i;
            } else if (bt == 2) {
                int32_t q;

                for (q = sp - 1; q >= 0; q--) {
                    if (stack_cp[q] == canon_bracket(c)) {
                        if (npairs < 512) {
                            pairs[npairs][0] = stack_pos[q];
                            pairs[npairs][1] = i;
                            npairs++;
                        }

                        sp = q;
                        break;
                    }
                }
            }
        }

        /* in order of opening positions */
        for (i = 1; i < npairs; i++) {
            int32_t a0 = pairs[i][0], a1 = pairs[i][1];

            for (k = i - 1; k >= 0 && pairs[k][0] > a0; k--) {
                pairs[k + 1][0] = pairs[k][0];
                pairs[k + 1][1] = pairs[k][1];
            }

            pairs[k + 1][0] = a0;
            pairs[k + 1][1] = a1;
        }

        for (k = 0; k < npairs; k++) {
            int32_t o = pairs[k][0], c = pairs[k][1], j;
            int found_e = 0, found_o = 0, newt = -1;

            for (j = o + 1; j < c; j++) {
                int x = strong_of(t[ix[j]]);

                if (x == e) {
                    found_e = 1;
                } else if (x == BC_L || x == BC_R) {
                    found_o = 1;
                }
            }

            if (found_e) {
                newt = e;
            } else if (found_o) {
                int ctx = s->sos;

                for (j = o - 1; j >= 0; j--) {
                    int x = strong_of(t[ix[j]]);

                    if (x == BC_L || x == BC_R) {
                        ctx = x;
                        break;
                    }
                }

                newt = (ctx != e) ? ctx : e;
            }

            if (newt >= 0) {
                t[ix[o]] = t[ix[c]] = (uint8_t)newt;

                /* NSMs that followed a bracket take its new type */
                for (j = o + 1; j < n && orig[ix[j]] == BC_NSM; j++) {
                    t[ix[j]] = (uint8_t)newt;
                }

                for (j = c + 1; j < n && orig[ix[j]] == BC_NSM; j++) {
                    t[ix[j]] = (uint8_t)newt;
                }
            }
        }
    }

    /* N1, N2: neutrals between strong types */
    for (i = 0; i < n; i++) {
        if (is_ni(t[ix[i]])) {
            int32_t j = i;
            int before, after;

            while (j < n && is_ni(t[ix[j]])) {
                j++;
            }

            before = i > 0 ? strong_of(t[ix[i - 1]]) : s->sos;
            after = j < n ? strong_of(t[ix[j]]) : s->eos;

            for (k = i; k < j; k++) {
                t[ix[k]] = (uint8_t)(before == after ? before : e);
            }

            i = j - 1;
        }
    }

    /* I1, I2: implicit levels */
    for (i = 0; i < n; i++) {
        int x = t[ix[i]], l = lev[ix[i]];

        if ((l & 1) == 0) {
            lev[ix[i]] = (uint8_t)(x == BC_R ? l + 1 : (x == BC_AN || x == BC_EN) ? l + 2 : l);
        } else if (x == BC_L || x == BC_EN || x == BC_AN) {
            lev[ix[i]] = (uint8_t)(l + 1);
        }
    }
}

/*
 * Resolve embedding levels for one paragraph. dir: -1 auto, 0 LTR, 1 RTL.
 * levels[i] = REMOVED (0xFF) for characters removed by X9. Returns the
 * paragraph level, or -1 on allocation failure.
 */
int pd_bidi_levels(const uint32_t* cp, int32_t n, int dir, uint8_t* levels) {
    uint8_t* orig, *t;
    int para, i;
    struct {
        uint8_t level, override, isolate;
    } stack[MAX_DEPTH + 2];
    int sp = 0, ovf_isolate = 0, ovf_embed = 0, valid_isolate = 0;
    seqrun* runs = NULL;
    int32_t* buf = NULL, *bufbase = NULL;

    orig = (uint8_t*)malloc((size_t)n + 1);
    t = (uint8_t*)malloc((size_t)n + 1);

    if (!orig || !t) {
        free(orig);
        free(t);
        return -1;
    }

    for (i = 0; i < n; i++) {
        orig[i] = t[i] = (uint8_t)pd_uni_bidi(cp[i]);
    }

    para = dir >= 0 ? dir : first_strong(orig, n, 0, n);
    para = para < 0 ? 0 : para;

    /* X1-X8 */
    stack[0].level = (uint8_t)para;
    stack[0].override = 0;
    stack[0].isolate = 0;
    sp = 1;

    for (i = 0; i < n; i++) {
        int x = orig[i];

        if (x == BC_RLE || x == BC_LRE || x == BC_RLO || x == BC_LRO) {
            int rtl = x == BC_RLE || x == BC_RLO;
            int nl = rtl ? ((stack[sp - 1].level + 1) | 1) : ((stack[sp - 1].level + 2) & ~1);

            levels[i] = stack[sp - 1].level;

            if (nl <= MAX_DEPTH && ovf_isolate == 0 && ovf_embed == 0) {
                stack[sp].level = (uint8_t)nl;
                stack[sp].override = (uint8_t)(x == BC_RLO ? 2 : x == BC_LRO ? 1 : 0);  /* 0 none, 1 L, 2 R */
                stack[sp].isolate = 0;
                sp++;
            } else if (ovf_isolate == 0) {
                ovf_embed++;
            }
        } else if (is_isolate_initiator(x)) {
            int rtl = x == BC_RLI;
            int nl;

            levels[i] = stack[sp - 1].level;

            if (stack[sp - 1].override) {
                t[i] = stack[sp - 1].override == 1 ? BC_L : BC_R;
            }

            if (x == BC_FSI) {
                rtl = first_strong(orig, n, i + 1, matching_pdi(orig, n, i)) == 1;
            }

            nl = rtl ? ((stack[sp - 1].level + 1) | 1) : ((stack[sp - 1].level + 2) & ~1);

            if (nl <= MAX_DEPTH && ovf_isolate == 0 && ovf_embed == 0) {
                valid_isolate++;
                stack[sp].level = (uint8_t)nl;
                stack[sp].override = 0;
                stack[sp].isolate = 1;
                sp++;
            } else {
                ovf_isolate++;
            }
        } else if (x == BC_PDI) {
            if (ovf_isolate > 0) {
                ovf_isolate--;
            } else if (valid_isolate > 0) {
                ovf_embed = 0;

                while (sp > 1 && !stack[sp - 1].isolate) {
                    sp--;
                }

                if (sp > 1) {
                    sp--;
                }

                valid_isolate--;
            }

            levels[i] = stack[sp - 1].level;

            if (stack[sp - 1].override) {
                t[i] = stack[sp - 1].override == 1 ? BC_L : BC_R;
            }
        } else if (x == BC_PDF) {
            levels[i] = stack[sp - 1].level;

            if (ovf_isolate > 0) {
                /* nothing */
            } else if (ovf_embed > 0) {
                ovf_embed--;
            } else if (!stack[sp - 1].isolate && sp >= 2) {
                sp--;
            }
        } else if (x == BC_B) {
            levels[i] = (uint8_t)para;
        } else {
            levels[i] = stack[sp - 1].level;

            if (x != BC_BN && stack[sp - 1].override) {
                t[i] = stack[sp - 1].override == 1 ? BC_L : BC_R;
            }
        }
    }

    /* X9 */
    for (i = 0; i < n; i++) {
        if (is_removed_class(orig[i])) {
            levels[i] = REMOVED;
        }
    }

    /* X10: level runs, chained into isolating run sequences */
    buf = bufbase = (int32_t*)malloc(((size_t)n + 1) * sizeof(int32_t));
    runs = (seqrun*)calloc((size_t)n + 1, sizeof(seqrun));

    if (!buf || !runs) {
        free(orig);
        free(t);
        free(buf);
        free(runs);
        return -1;
    }

    {
        int32_t nruns = 0, k, used = 0;
        int32_t* run_of = (int32_t*)malloc(((size_t)n + 1) * sizeof(int32_t));
        int32_t* rstart, *rend;

        rstart = (int32_t*)malloc(((size_t)n + 1) * sizeof(int32_t));
        rend = (int32_t*)malloc(((size_t)n + 1) * sizeof(int32_t));

        if (!run_of || !rstart || !rend) {
            free(run_of);
            free(rstart);
            free(rend);
            free(orig);
            free(t);
            free(buf);
            free(runs);
            return -1;
        }

        /* level runs over the characters that remain */
        for (i = 0; i < n; i++) {
            if (levels[i] == REMOVED) {
                run_of[i] = -1;
                continue;
            }

            if (nruns > 0 && levels[rend[nruns - 1]] == levels[i]) {
                rend[nruns - 1] = i;
            } else {
                rstart[nruns] = i;
                rend[nruns] = i;
                nruns++;
            }

            run_of[i] = nruns - 1;
        }

        {
            uint8_t* chained = (uint8_t*)calloc((size_t)nruns + 1, 1);

            for (k = 0; k < nruns; k++) {
                int32_t r = k, cnt = 0, first_c, last_c, prevc, nextc, q;
                seqrun* s;
                int lv;

                if (chained[k]) {
                    continue;
                }

                /* a sequence starts at a run that does not begin with a matched PDI */
                s = &runs[used++];
                s->idx = buf;

                for (;;) {
                    int32_t j, endc = rend[r];

                    chained[r] = 1;

                    for (j = rstart[r]; j <= endc; j++) {
                        if (levels[j] != REMOVED) {
                            buf[cnt++] = j;
                        }
                    }

                    if (is_isolate_initiator(orig[endc])) {
                        int32_t m = matching_pdi(orig, n, endc);

                        if (m < n && run_of[m] >= 0 && !chained[run_of[m]]) {
                            r = run_of[m];
                            continue;
                        }
                    }

                    break;
                }

                s->n = cnt;
                buf += cnt;
                first_c = s->idx[0];
                last_c = s->idx[cnt - 1];
                lv = levels[first_c];
                s->level = lv;

                for (prevc = first_c - 1; prevc >= 0 && levels[prevc] == REMOVED; prevc--) {
                }

                for (nextc = last_c + 1; nextc < n && levels[nextc] == REMOVED; nextc++) {
                }

                /* sos: against the character before; eos: against the one after, or the
                   paragraph when the sequence ends with an (unmatched) isolate initiator */
                q = prevc >= 0 ? levels[prevc] : para;
                s->sos = ((lv > q ? lv : q) & 1) ? BC_R : BC_L;
                q = (nextc < n && !is_isolate_initiator(orig[last_c])) ? levels[nextc] : para;
                s->eos = ((lv > q ? lv : q) & 1) ? BC_R : BC_L;
            }

            /* runs that begin with a matched PDI were placed in their initiator's sequence;
               resolve each sequence */
            for (k = 0; k < used; k++) {
                resolve_sequence(cp, orig, t, levels, &runs[k]);
            }

            free(chained);
        }

        free(run_of);
        free(rstart);
        free(rend);
    }

    free(runs);
    free(bufbase);
    free(orig);
    free(t);
    return para;
}

/*
 * One line [from, to) of a resolved paragraph: L1 resets separators and
 * trailing whitespace to the paragraph level, L2 gives the visual order.
 * lev receives the line's levels (REMOVED kept), order the logical
 * indices (absolute) in visual order, removed characters left out.
 * Returns the number of entries in order.
 */
int32_t pd_bidi_line(const uint32_t* cp, const uint8_t* levels, int32_t from, int32_t to, int para, uint8_t* lev,
                     int32_t* order) {
    int32_t i, n = to - from, cnt = 0, j;
    int maxl = 0, minodd = 255, l;

    for (i = 0; i < n; i++) {
        lev[i] = levels[from + i];
    }

    /* L1 */
    for (i = 0; i < n; i++) {
        int x = pd_uni_bidi(cp[from + i]);

        if (x == BC_S || x == BC_B) {
            lev[i] = lev[i] == REMOVED ? REMOVED : (uint8_t)para;

            for (j = i - 1; j >= 0; j--) {
                int y = pd_uni_bidi(cp[from + j]);

                if (y == BC_WS || is_isolate_initiator(y) || y == BC_PDI || is_removed_class(y)) {
                    lev[j] = lev[j] == REMOVED ? REMOVED : (uint8_t)para;
                } else {
                    break;
                }
            }
        }
    }

    for (j = n - 1; j >= 0; j--) {
        int y = pd_uni_bidi(cp[from + j]);

        if (y == BC_WS || is_isolate_initiator(y) || y == BC_PDI || is_removed_class(y)) {
            lev[j] = lev[j] == REMOVED ? REMOVED : (uint8_t)para;
        } else {
            break;
        }
    }

    /* L2 */
    for (i = 0; i < n; i++) {
        if (lev[i] == REMOVED) {
            continue;
        }

        order[cnt++] = from + i;
        maxl = lev[i] > maxl ? lev[i] : maxl;

        if ((lev[i] & 1) && lev[i] < minodd) {
            minodd = lev[i];
        }
    }

    for (l = maxl; l >= minodd && l > 0; l--) {
        for (i = 0; i < cnt; i++) {
            if (lev[order[i] - from] >= l) {
                int32_t a = i, b;

                while (i < cnt && lev[order[i] - from] >= l) {
                    i++;
                }

                for (b = i - 1; a < b; a++, b--) {
                    int32_t tmp = order[a];

                    order[a] = order[b];
                    order[b] = tmp;
                }
            }
        }
    }

    return cnt;
}
