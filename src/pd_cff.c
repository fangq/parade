/*
 * Parade CFF outlines: a Type 2 charstring interpreter
 *
 * Reads the CFF table of an OpenType font (name-keyed or CID-keyed with
 * FDArray/FDSelect), runs a glyph's charstring with local and global
 * subroutines and emits move/line/cubic segments in 26.6 font units.
 * Arithmetic is 16.16 fixed point, so outlines are identical everywhere.
 * Hints are skipped (stems are counted only to size hintmask data).
 * Every read is bounds-checked; malformed charstrings stop, never crash.
 */

#include <string.h>
#include "pd_internal.h"

#define MAX_STACK 48
#define MAX_SUBR_DEPTH 10

enum {
    OP_MOVE = 0,
    OP_LINE = 1,
    OP_QUAD = 2,
    OP_CUBIC = 3
};

typedef void (*emit_fn)(void* user, int op, const int32_t* xs, const int32_t* ys, int n);

typedef struct {
    uint32_t off;               /* absolute offset of the data area start - 1 rule applied */
    uint32_t count, offsize, data;
} cindex;

static uint32_t B(const pd_font* f, uint64_t o) {
    return o < f->len ? f->data[o] : 0;
}

static uint32_t BN(const pd_font* f, uint64_t o, uint32_t n) {
    uint32_t v = 0, k;

    for (k = 0; k < n; k++) {
        v = (v << 8) | B(f, o + k);
    }

    return v;
}

/* an INDEX at off; returns the offset just past it */
static uint32_t read_index(const pd_font* f, uint32_t off, cindex* x) {
    memset(x, 0, sizeof(*x));
    x->off = off;
    x->count = BN(f, off, 2);

    if (x->count == 0) {
        return off + 2;
    }

    x->offsize = B(f, off + 2);

    if (x->offsize < 1 || x->offsize > 4) {
        x->count = 0;
        return off + 2;
    }

    x->data = off + 3 + (x->count + 1) * x->offsize - 1;   /* offsets are 1-based */
    return x->data + BN(f, (uint64_t)off + 3 + (uint64_t)x->count * x->offsize, x->offsize);
}

static int index_item(const pd_font* f, const cindex* x, uint32_t i, uint32_t* start, uint32_t* len) {
    uint32_t a, b;

    if (i >= x->count) {
        return -1;
    }

    a = BN(f, (uint64_t)x->off + 3 + (uint64_t)i * x->offsize, x->offsize);
    b = BN(f, (uint64_t)x->off + 3 + (uint64_t)(i + 1) * x->offsize, x->offsize);

    if (b < a || (uint64_t)x->data + b > f->len) {
        return -1;
    }

    *start = x->data + a;
    *len = b - a;
    return 0;
}

/* DICT: operands as integers (reals are read and truncated); find one operator */
static int dict_get(const pd_font* f, uint32_t start, uint32_t len, int op, int32_t* vals, int nwant) {
    uint32_t p = start, end = start + len;
    int32_t stack[48];
    int n = 0;

    while (p < end) {
        uint32_t b0 = B(f, p);

        if (b0 <= 21) {     /* operator */
            int o = (int)b0;

            p++;

            if (b0 == 12) {
                o = 1200 + (int)B(f, p);
                p++;
            }

            if (o == op) {
                int k;

                for (k = 0; k < nwant && k < n; k++) {
                    vals[k] = stack[k];
                }

                return n;
            }

            n = 0;
        } else {
            int32_t v = 0;

            if (b0 == 28) {
                v = (int16_t)BN(f, p + 1, 2);
                p += 3;
            } else if (b0 == 29) {
                v = (int32_t)BN(f, p + 1, 4);
                p += 5;
            } else if (b0 == 30) {      /* real: skip nibbles to 0xf */
                p++;

                while (p < end) {
                    uint32_t nb = B(f, p++);

                    if ((nb & 0x0f) == 0x0f || (nb >> 4) == 0x0f) {
                        break;
                    }
                }
            } else if (b0 >= 32 && b0 <= 246) {
                v = (int32_t)b0 - 139;
                p++;
            } else if (b0 >= 247 && b0 <= 250) {
                v = ((int32_t)b0 - 247) * 256 + (int32_t)B(f, p + 1) + 108;
                p += 2;
            } else if (b0 >= 251 && b0 <= 254) {
                v = -((int32_t)b0 - 251) * 256 - (int32_t)B(f, p + 1) - 108;
                p += 2;
            } else {
                p++;
            }

            if (n < 48) {
                stack[n++] = v;
            }
        }
    }

    return -1;
}

typedef struct {
    const pd_font* f;
    cindex gsubrs, lsubrs;
    int32_t gbias, lbias;
    int32_t st[MAX_STACK];      /* 16.16 */
    int n;
    int32_t x, y;               /* 16.16 */
    int nstems, width_done, open, done;
    int32_t hint_skip;
    emit_fn emit;
    void* user;
    int err;
} interp;

static int32_t bias(uint32_t count) {
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

static void out(interp* I, int op, int32_t* xs, int32_t* ys, int npts) {
    int32_t ox[3], oy[3];
    int k;

    for (k = 0; k < npts; k++) {    /* 16.16 -> 26.6 */
        ox[k] = xs[k] >> 10;
        oy[k] = ys[k] >> 10;
    }

    I->emit(I->user, op, ox, oy, npts);
}

static void moveto(interp* I, int32_t dx, int32_t dy) {
    I->x += dx;
    I->y += dy;
    out(I, OP_MOVE, &I->x, &I->y, 1);
    I->open = 1;
}

static void lineto(interp* I, int32_t dx, int32_t dy) {
    I->x += dx;
    I->y += dy;
    out(I, OP_LINE, &I->x, &I->y, 1);
}

static void curveto(interp* I, int32_t dxa, int32_t dya, int32_t dxb, int32_t dyb, int32_t dxc, int32_t dyc) {
    int32_t xs[3], ys[3];

    xs[0] = I->x + dxa;
    ys[0] = I->y + dya;
    xs[1] = xs[0] + dxb;
    ys[1] = ys[0] + dyb;
    xs[2] = xs[1] + dxc;
    ys[2] = ys[1] + dyc;
    I->x = xs[2];
    I->y = ys[2];
    out(I, OP_CUBIC, xs, ys, 3);
}

/* the optional width before the first stem/move operator */
static int args_after_width(interp* I, int expect_even, int nargs_needed) {
    if (!I->width_done) {
        I->width_done = 1;

        if ((expect_even && (I->n % 2) == 1) || (!expect_even && I->n > nargs_needed)) {
            memmove(I->st, I->st + 1, (size_t)(I->n - 1) * sizeof(int32_t));
            I->n--;
        }
    }

    return 0;
}

static int run(interp* I, uint32_t start, uint32_t len, int depth) {
    const pd_font* f = I->f;
    uint32_t p = start, end = start + len;

    if (depth > MAX_SUBR_DEPTH) {
        return -1;
    }

    while (p < end && !I->done) {
        uint32_t b0 = B(f, p);
        int i;

        if (b0 >= 32 || b0 == 28 || b0 == 255) {     /* operands */
            int32_t v;

            if (b0 == 28) {
                v = (int32_t)(int16_t)BN(f, p + 1, 2) * 65536;
                p += 3;
            } else if (b0 == 255) {
                v = (int32_t)BN(f, p + 1, 4);
                p += 5;
            } else if (b0 <= 246) {
                v = ((int32_t)b0 - 139) * 65536;
                p++;
            } else if (b0 <= 250) {
                v = (((int32_t)b0 - 247) * 256 + (int32_t)B(f, p + 1) + 108) * 65536;
                p += 2;
            } else {
                v = (-((int32_t)b0 - 251) * 256 - (int32_t)B(f, p + 1) - 108) * 65536;
                p += 2;
            }

            if (I->n >= MAX_STACK) {
                return -1;
            }

            I->st[I->n++] = v;
            continue;
        }

        p++;

        switch (b0) {
            case 1:     /* hstem */
            case 3:     /* vstem */
            case 18:    /* hstemhm */
            case 23:    /* vstemhm */
                args_after_width(I, 1, 0);
                I->nstems += I->n / 2;
                I->n = 0;
                break;

            case 19:    /* hintmask */
            case 20:    /* cntrmask */
                args_after_width(I, 1, 0);
                I->nstems += I->n / 2;     /* implicit vstem */
                I->n = 0;
                p += (uint32_t)(I->nstems + 7) / 8;
                break;

            case 21:    /* rmoveto */
                args_after_width(I, 0, 2);

                if (I->n < 2) {
                    return -1;
                }

                moveto(I, I->st[0], I->st[1]);
                I->n = 0;
                break;

            case 22:    /* hmoveto */
            case 4:     /* vmoveto */
                args_after_width(I, 0, 1);

                if (I->n < 1) {
                    return -1;
                }

                moveto(I, b0 == 22 ? I->st[0] : 0, b0 == 22 ? 0 : I->st[0]);
                I->n = 0;
                break;

            case 5:     /* rlineto */
                for (i = 0; i + 1 < I->n; i += 2) {
                    lineto(I, I->st[i], I->st[i + 1]);
                }

                I->n = 0;
                break;

            case 6:     /* hlineto */
            case 7: {   /* vlineto */
                int horiz = b0 == 6;

                for (i = 0; i < I->n; i++, horiz = !horiz) {
                    lineto(I, horiz ? I->st[i] : 0, horiz ? 0 : I->st[i]);
                }

                I->n = 0;
                break;
            }

            case 8:     /* rrcurveto */
                for (i = 0; i + 5 < I->n; i += 6) {
                    curveto(I, I->st[i], I->st[i + 1], I->st[i + 2], I->st[i + 3], I->st[i + 4], I->st[i + 5]);
                }

                I->n = 0;
                break;

            case 24:    /* rcurveline */
                for (i = 0; i + 5 < I->n - 2; i += 6) {
                    curveto(I, I->st[i], I->st[i + 1], I->st[i + 2], I->st[i + 3], I->st[i + 4], I->st[i + 5]);
                }

                if (i + 1 < I->n) {
                    lineto(I, I->st[i], I->st[i + 1]);
                }

                I->n = 0;
                break;

            case 25:    /* rlinecurve */
                for (i = 0; i + 1 < I->n - 6; i += 2) {
                    lineto(I, I->st[i], I->st[i + 1]);
                }

                if (i + 5 < I->n) {
                    curveto(I, I->st[i], I->st[i + 1], I->st[i + 2], I->st[i + 3], I->st[i + 4], I->st[i + 5]);
                }

                I->n = 0;
                break;

            case 26:    /* vvcurveto */
                i = 0;

                if (I->n % 2) {
                    i = 1;

                    if (I->n >= 5) {
                        curveto(I, I->st[0], I->st[1], I->st[2], I->st[3], 0, I->st[4]);
                        i = 5;
                    }
                }

                for (; i + 3 < I->n; i += 4) {
                    curveto(I, 0, I->st[i], I->st[i + 1], I->st[i + 2], 0, I->st[i + 3]);
                }

                I->n = 0;
                break;

            case 27:    /* hhcurveto */
                i = 0;

                if (I->n % 2) {
                    i = 1;

                    if (I->n >= 5) {
                        curveto(I, I->st[1], I->st[0], I->st[2], I->st[3], I->st[4], 0);
                        i = 5;
                    }
                }

                for (; i + 3 < I->n; i += 4) {
                    curveto(I, I->st[i], 0, I->st[i + 1], I->st[i + 2], I->st[i + 3], 0);
                }

                I->n = 0;
                break;

            case 30:    /* vhcurveto */
            case 31: {  /* hvcurveto */
                int horiz = b0 == 31;

                for (i = 0; i + 3 < I->n; i += 4, horiz = !horiz) {
                    int last = i + 4 >= I->n - 1, extra = (last && I->n - i == 5) ? I->st[i + 4] : 0;

                    if (horiz) {
                        curveto(I, I->st[i], 0, I->st[i + 1], I->st[i + 2], extra, I->st[i + 3]);
                    } else {
                        curveto(I, 0, I->st[i], I->st[i + 1], I->st[i + 2], I->st[i + 3], extra);
                    }
                }

                I->n = 0;
                break;
            }

            case 10:    /* callsubr */
            case 29: {  /* callgsubr */
                const cindex* x = b0 == 10 ? &I->lsubrs : &I->gsubrs;
                int32_t idx;
                uint32_t s0, l0;

                if (I->n < 1) {
                    return -1;
                }

                idx = (I->st[--I->n] >> 16) + (b0 == 10 ? I->lbias : I->gbias);

                if (idx < 0 || index_item(f, x, (uint32_t)idx, &s0, &l0) || run(I, s0, l0, depth + 1)) {
                    return -1;
                }

                break;
            }

            case 11:    /* return */
                return 0;

            case 14:    /* endchar */
                args_after_width(I, 0, 0);
                I->done = 1;
                I->n = 0;
                break;

            case 12: {  /* escape: flex family; other operators are dropped */
                uint32_t b1 = B(f, p++);
                int32_t* a = I->st;

                if (b1 == 35 && I->n >= 13) {           /* flex */
                    curveto(I, a[0], a[1], a[2], a[3], a[4], a[5]);
                    curveto(I, a[6], a[7], a[8], a[9], a[10], a[11]);
                } else if (b1 == 34 && I->n >= 7) {     /* hflex */
                    int32_t y0 = I->y;

                    curveto(I, a[0], 0, a[1], a[2], a[3], 0);
                    curveto(I, a[4], 0, a[5], y0 - I->y, a[6], 0);
                } else if (b1 == 36 && I->n >= 9) {     /* hflex1: ends back at the starting height */
                    int32_t y0 = I->y;

                    curveto(I, a[0], a[1], a[2], a[3], a[4], 0);
                    curveto(I, a[5], 0, a[6], a[7], a[8], y0 - I->y - a[7]);
                } else if (b1 == 37 && I->n >= 11) {    /* flex1: the last coordinate is x or y by dominance */
                    int32_t dx = a[0] + a[2] + a[4] + a[6] + a[8], dy = a[1] + a[3] + a[5] + a[7] + a[9];
                    int32_t ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;

                    curveto(I, a[0], a[1], a[2], a[3], a[4], a[5]);

                    if (ax > ay) {
                        curveto(I, a[6], a[7], a[8], a[9], a[10], -dy);
                    } else {
                        curveto(I, a[6], a[7], a[8], a[9], -dx, a[10]);
                    }
                }

                I->n = 0;
                break;
            }

            default:    /* reserved or unsupported: clear the stack */
                I->n = 0;
        }

        if (I->err) {
            return -1;
        }
    }

    return 0;
}

int pd_cff_glyph(const pd_font* f, uint32_t g, emit_fn emit, void* user) {
    uint32_t cff = f->cff, hdr, p, td, tdlen, cs, cslen;
    cindex names, tops, strings, chars;
    int32_t v[2];
    interp I;

    if (!cff) {
        return -1;
    }

    memset(&I, 0, sizeof(I));
    I.f = f;
    I.emit = emit;
    I.user = user;
    hdr = B(f, cff + 2);
    p = read_index(f, cff + hdr, &names);
    p = read_index(f, p, &tops);
    p = read_index(f, p, &strings);
    read_index(f, p, &I.gsubrs);
    I.gbias = bias(I.gsubrs.count);

    if (index_item(f, &tops, 0, &td, &tdlen) || dict_get(f, td, tdlen, 17, v, 1) < 1) {
        return -1;
    }

    read_index(f, cff + (uint32_t)v[0], &chars);

    if (index_item(f, &chars, g, &cs, &cslen)) {
        return -1;
    }

    /* the private dict (and so the local subrs) of this glyph */
    {
        uint32_t pd = 0, pdlen = 0;
        int32_t fdv[1];

        if (dict_get(f, td, tdlen, 1236, fdv, 1) >= 1) {   /* FDArray: CID-keyed */
            cindex fda;
            int32_t sel[1];
            uint32_t fd = 0, fds, fdo, fdl;

            read_index(f, cff + (uint32_t)fdv[0], &fda);

            if (dict_get(f, td, tdlen, 1237, sel, 1) >= 1) {
                fds = cff + (uint32_t)sel[0];

                if (B(f, fds) == 0) {
                    fd = B(f, fds + 1 + g);
                } else if (B(f, fds) == 3) {
                    uint32_t nr = BN(f, fds + 1, 2), k;

                    for (k = 0; k < nr; k++) {
                        uint32_t first = BN(f, fds + 3 + 3 * k, 2), next = BN(f, fds + 3 + 3 * (k + 1), 2);

                        if (g >= first && g < next) {
                            fd = B(f, fds + 3 + 3 * k + 2);
                            break;
                        }
                    }
                }
            }

            if (index_item(f, &fda, fd, &fdo, &fdl) == 0 && dict_get(f, fdo, fdl, 18, v, 2) >= 2) {
                pdlen = (uint32_t)v[0];
                pd = cff + (uint32_t)v[1];
            }
        } else if (dict_get(f, td, tdlen, 18, v, 2) >= 2) {
            pdlen = (uint32_t)v[0];
            pd = cff + (uint32_t)v[1];
        }

        if (pd && dict_get(f, pd, pdlen, 19, v, 1) >= 1) {
            read_index(f, pd + (uint32_t)v[0], &I.lsubrs);
            I.lbias = bias(I.lsubrs.count);
        }
    }

    return run(&I, cs, cslen, 0);
}
