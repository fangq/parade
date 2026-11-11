/*
 * Parade PDF reader: colour spaces, functions, shadings and pictures
 *
 * Everything ends in RGB: device CMYK by Poppler's sixteen-corner
 * approximation of a press profile (no ICC: an ICCBased space is the device
 * space of its component count), Lab by its white point, Indexed,
 * Separation and DeviceN through their bases and tint functions. Functions
 * of all four types; PostScript calculator functions compiled to a small
 * stack machine. Pictures decode to 8-bit RGBA (a soft mask or colour key
 * folded into the alpha) or to a stencil, and are kept in a cache that
 * keeps the most recently drawn within a memory budget.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "pd_internal.h"
#include "pd_pdfr_dl.h"

#define IMG_BUDGET ((size_t)384 << 20)

pimage* pimage_level(pdoc* d, pimage* im, double px_w, double px_h);

static double clamp01(double v) {
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* ------------------------------------------------------------------ */
/* functions                                                          */
/* ------------------------------------------------------------------ */

enum {
    PS_NUM = 1, PS_ABS, PS_ADD, PS_ATAN, PS_CEIL, PS_COS, PS_CVI, PS_CVR, PS_DIV, PS_EXP, PS_FLOOR, PS_IDIV, PS_LN,
    PS_LOG, PS_MOD, PS_MUL, PS_NEG, PS_ROUND, PS_SIN, PS_SQRT, PS_SUB, PS_TRUNC, PS_AND, PS_OR, PS_NOT, PS_XOR,
    PS_BITSHIFT, PS_EQ, PS_GE, PS_GT, PS_LE, PS_LT, PS_NE, PS_TRUE, PS_FALSE, PS_COPY, PS_DUP, PS_EXCH, PS_INDEX,
    PS_POP, PS_ROLL, PS_JZ, PS_JMP
};

static const struct {
    const char* name;
    int op;
} ps_ops[] = {
    {"abs", PS_ABS}, {"add", PS_ADD}, {"atan", PS_ATAN}, {"ceiling", PS_CEIL}, {"cos", PS_COS}, {"cvi", PS_CVI},
    {"cvr", PS_CVR}, {"div", PS_DIV}, {"exp", PS_EXP}, {"floor", PS_FLOOR}, {"idiv", PS_IDIV}, {"ln", PS_LN},
    {"log", PS_LOG}, {"mod", PS_MOD}, {"mul", PS_MUL}, {"neg", PS_NEG}, {"round", PS_ROUND}, {"sin", PS_SIN},
    {"sqrt", PS_SQRT}, {"sub", PS_SUB}, {"truncate", PS_TRUNC}, {"and", PS_AND}, {"or", PS_OR}, {"not", PS_NOT},
    {"xor", PS_XOR}, {"bitshift", PS_BITSHIFT}, {"eq", PS_EQ}, {"ge", PS_GE}, {"gt", PS_GT}, {"le", PS_LE},
    {"lt", PS_LT}, {"ne", PS_NE}, {"true", PS_TRUE}, {"false", PS_FALSE}, {"copy", PS_COPY}, {"dup", PS_DUP},
    {"exch", PS_EXCH}, {"index", PS_INDEX}, {"pop", PS_POP}, {"roll", PS_ROLL}
};

static void ps_emit(pbuf* b, double op, double arg) {
    double v[2] = { op, arg };

    qb_put(b, v, sizeof(v));
}

/* a { } block compiled; L is just past its "{" */
static int ps_block(plex* L, pbuf* b, int depth) {
    if (depth > 32) {
        return -1;
    }

    for (;;) {
        pv v = pl_read(L);

        if (v.t == PV_ERR) {
            return -1;
        }

        if (v.t == PV_INT || v.t == PV_REAL) {
            ps_emit(b, PS_NUM, v.t == PV_INT ? (double)v.u.i : v.u.r);
        } else if (v.t == PV_BOOL) {
            ps_emit(b, v.u.i ? PS_TRUE : PS_FALSE, 0);
        } else if (v.t == PV_OP) {
            size_t k;

            if (strcmp(v.u.s, "}") == 0) {
                return 0;
            }

            if (strcmp(v.u.s, "{") == 0) {
                /* a block: if or ifelse follows (with a second block) */
                pbuf a, c;
                pv nx;
                size_t save;

                memset(&a, 0, sizeof(a));
                memset(&c, 0, sizeof(c));

                if (ps_block(L, &a, depth + 1)) {
                    qb_free(&a);
                    return -1;
                }

                save = L->i;
                nx = pl_read(L);

                if (nx.t == PV_OP && strcmp(nx.u.s, "{") == 0) {
                    pv kw;

                    if (ps_block(L, &c, depth + 1)) {
                        qb_free(&a);
                        qb_free(&c);
                        return -1;
                    }

                    kw = pl_read(L);

                    if (kw.t == PV_OP && strcmp(kw.u.s, "ifelse") == 0) {
                        /* JZ over a and its JMP; a; JMP over c; c */
                        ps_emit(b, PS_JZ, (double)(a.n / (2 * sizeof(double)) + 1));
                        qb_put(b, a.p, a.n);
                        ps_emit(b, PS_JMP, (double)(c.n / (2 * sizeof(double))));
                        qb_put(b, c.p, c.n);
                    }
                } else {
                    L->i = save;
                    nx = pl_read(L);

                    if (nx.t == PV_OP && strcmp(nx.u.s, "if") == 0) {
                        ps_emit(b, PS_JZ, (double)(a.n / (2 * sizeof(double))));
                        qb_put(b, a.p, a.n);
                    }
                }

                qb_free(&a);
                qb_free(&c);
                continue;
            }

            for (k = 0; k < sizeof(ps_ops) / sizeof(ps_ops[0]); k++) {
                if (strcmp(ps_ops[k].name, v.u.s) == 0) {
                    ps_emit(b, ps_ops[k].op, 0);
                    break;
                }
            }
        }
    }
}

static int ps_run(const pfunc* f, double* st, int n) {
    int pc, np = f->nprog;

    for (pc = 0; pc < np; pc++) {
        int op = (int)f->prog[2 * pc];
        double arg = f->prog[2 * pc + 1], a, b;

        if (n >= 96) {
            return n;
        }

#define NEED(k) if (n < (k)) return n
        switch (op) {
            case PS_NUM: st[n++] = arg; break;
            case PS_TRUE: st[n++] = 1; break;
            case PS_FALSE: st[n++] = 0; break;
            case PS_ABS: NEED(1); st[n - 1] = fabs(st[n - 1]); break;
            case PS_NEG: NEED(1); st[n - 1] = -st[n - 1]; break;
            case PS_CEIL: NEED(1); st[n - 1] = ceil(st[n - 1]); break;
            case PS_FLOOR: NEED(1); st[n - 1] = floor(st[n - 1]); break;
            case PS_ROUND: NEED(1); st[n - 1] = floor(st[n - 1] + 0.5); break;
            case PS_TRUNC: case PS_CVI: NEED(1); st[n - 1] = trunc(st[n - 1]); break;
            case PS_CVR: break;
            case PS_SQRT: NEED(1); st[n - 1] = st[n - 1] > 0 ? sqrt(st[n - 1]) : 0; break;
            case PS_SIN: NEED(1); st[n - 1] = sin(st[n - 1] * M_PI / 180); break;
            case PS_COS: NEED(1); st[n - 1] = cos(st[n - 1] * M_PI / 180); break;
            case PS_LN: NEED(1); st[n - 1] = st[n - 1] > 0 ? log(st[n - 1]) : -1e30; break;
            case PS_LOG: NEED(1); st[n - 1] = st[n - 1] > 0 ? log10(st[n - 1]) : -1e30; break;
            case PS_NOT: NEED(1); st[n - 1] = st[n - 1] == 0 || st[n - 1] == 1 ? !st[n - 1] : (double)~(int64_t)st[n - 1]; break;
            case PS_DUP: NEED(1); st[n] = st[n - 1]; n++; break;
            case PS_POP: NEED(1); n--; break;
            case PS_EXCH: NEED(2); a = st[n - 1]; st[n - 1] = st[n - 2]; st[n - 2] = a; break;
            case PS_ADD: case PS_SUB: case PS_MUL: case PS_DIV: case PS_IDIV: case PS_MOD: case PS_EXP:
            case PS_ATAN: case PS_AND: case PS_OR: case PS_XOR: case PS_BITSHIFT: case PS_EQ: case PS_GE:
            case PS_GT: case PS_LE: case PS_LT: case PS_NE:
                NEED(2);
                a = st[n - 2];
                b = st[n - 1];
                n--;

                switch (op) {
                    case PS_ADD: a += b; break;
                    case PS_SUB: a -= b; break;
                    case PS_MUL: a *= b; break;
                    case PS_DIV: a = b ? a / b : 0; break;
                    case PS_IDIV: a = (int64_t)b ? (double)((int64_t)a / (int64_t)b) : 0; break;
                    case PS_MOD: a = (int64_t)b ? (double)((int64_t)a % (int64_t)b) : 0; break;
                    case PS_EXP: a = pow(a, b); break;
                    case PS_ATAN: a = atan2(a, b) * 180 / M_PI; a = a < 0 ? a + 360 : a; break;
                    case PS_AND: a = (double)((int64_t)a & (int64_t)b); break;
                    case PS_OR: a = (double)((int64_t)a | (int64_t)b); break;
                    case PS_XOR: a = (double)((int64_t)a ^ (int64_t)b); break;
                    case PS_BITSHIFT: a = b >= 0 ? (double)((int64_t)a << (int)b) : (double)((int64_t)a >> (int)-b); break;
                    case PS_EQ: a = a == b; break;
                    case PS_GE: a = a >= b; break;
                    case PS_GT: a = a > b; break;
                    case PS_LE: a = a <= b; break;
                    case PS_LT: a = a < b; break;
                    case PS_NE: a = a != b; break;
                    default: break;
                }

                st[n - 1] = a;
                break;

            case PS_COPY: {
                int k, c;

                NEED(1);
                c = (int)st[--n];

                if (c < 0 || c > n || n + c > 96) {
                    return n;
                }

                for (k = 0; k < c; k++) {
                    st[n + k] = st[n - c + k];
                }

                n += c;
                break;
            }

            case PS_INDEX: {
                int k;

                NEED(1);
                k = (int)st[n - 1];

                if (k < 0 || k >= n - 1) {
                    return n;
                }

                st[n - 1] = st[n - 2 - k];
                break;
            }

            case PS_ROLL: {
                int j, c, k;

                NEED(2);
                j = (int)st[--n];
                c = (int)st[--n];

                if (c <= 0 || c > n) {
                    break;
                }

                j = ((j % c) + c) % c;

                for (k = 0; k < j; k++) {
                    double t = st[n - 1];
                    int q;

                    for (q = n - 1; q > n - c; q--) {
                        st[q] = st[q - 1];
                    }

                    st[n - c] = t;
                }

                break;
            }

            case PS_JZ:
                NEED(1);

                if (st[--n] == 0) {
                    pc += (int)arg;
                }

                break;

            case PS_JMP:
                pc += (int)arg;
                break;

            default:
                break;
        }
    }

    return n;
}

static pfunc* fn_parse(pdoc* d, parena** A, pv v, int depth);

static int nums(pdoc* d, pv arr, double* out, int max) {
    int i, n;

    arr = pd_resolve(d, arr);

    if (arr.t != PV_ARRAY) {
        return 0;
    }

    n = arr.n < max ? arr.n : max;

    for (i = 0; i < n; i++) {
        if (!pd_num(d, arr.u.a[i], &out[i])) {
            out[i] = 0;
        }
    }

    return n;
}

static pfunc* fn_parse(pdoc* d, parena** A, pv v, int depth) {
    pfunc* f;
    int type;

    v = pd_resolve(d, v);

    if ((v.t != PV_DICT && v.t != PV_STREAM) || depth > 8) {
        return NULL;
    }

    f = (pfunc*)pa_alloc(A, sizeof(pfunc));

    if (!f) {
        return NULL;
    }

    type = (int)pd_getint(d, v, "FunctionType", -1);
    f->type = type;
    f->m = nums(d, pd_get(d, v, "Domain"), f->domain, 8) / 2;
    f->n = nums(d, pd_get(d, v, "Range"), f->range, 32) / 2;

    if (f->m < 1) {
        f->m = 1;
        f->domain[0] = 0;
        f->domain[1] = 1;
    }

    if (type == 0) {
        size_t len, total = 1, i;
        unsigned char* data;
        int k;

        if (v.t != PV_STREAM || f->n < 1) {
            return NULL;
        }

        if (nums(d, pd_get(d, v, "Size"), (double[4]){0}, 4) < f->m) {
            return NULL;
        }

        {
            double sz[4] = { 0 };

            nums(d, pd_get(d, v, "Size"), sz, 4);

            for (k = 0; k < f->m && k < 4; k++) {
                f->size[k] = (int)sz[k];

                if (f->size[k] < 1 || f->size[k] > 65536) {
                    return NULL;
                }

                total *= (size_t)f->size[k];
            }
        }

        if (f->m > 4 || total > ((size_t)1 << 24) / (size_t)f->n) {
            return NULL;
        }

        f->bps = (int)pd_getint(d, v, "BitsPerSample", 8);

        if (nums(d, pd_get(d, v, "Encode"), f->encode, 8) < 2 * f->m) {
            for (k = 0; k < f->m; k++) {
                f->encode[2 * k] = 0;
                f->encode[2 * k + 1] = f->size[k] - 1;
            }
        }

        if (nums(d, pd_get(d, v, "Decode"), f->decode, 32) < 2 * f->n) {
            memcpy(f->decode, f->range, sizeof(double) * 2 * (size_t)f->n);
        }

        data = pd_stream_data(d, v, &len, NULL, NULL);

        if (!data) {
            return NULL;
        }

        f->samples = (double*)pa_alloc(A, sizeof(double) * total * (size_t)f->n);

        if (!f->samples) {
            free(data);
            return NULL;
        }

        {
            uint64_t bit = 0, maxv = f->bps >= 32 ? 0xFFFFFFFFu : ((uint64_t)1 << f->bps) - 1;

            for (i = 0; i < total * (size_t)f->n; i++) {
                uint64_t s = 0;
                int b;

                for (b = 0; b < f->bps; b++, bit++) {
                    size_t byte = (size_t)(bit / 8);

                    s = (s << 1) | (byte < len ? (data[byte] >> (7 - bit % 8)) & 1 : 0);
                }

                f->samples[i] = (double)s / (double)maxv;
            }
        }

        free(data);
        return f;
    }

    if (type == 2) {
        int k;

        f->e = pd_getnum(d, v, "N", 1);

        if (nums(d, pd_get(d, v, "C0"), f->c0, 16) == 0) {
            f->c0[0] = 0;
            f->n = f->n ? f->n : 1;
        }

        k = nums(d, pd_get(d, v, "C1"), f->c1, 16);

        if (k == 0) {
            f->c1[0] = 1;
            k = 1;
        }

        if (f->n < k) {
            f->n = k;
        }

        return f;
    }

    if (type == 3) {
        pv fns = pd_get(d, v, "Functions");
        int k;

        if (fns.t != PV_ARRAY || fns.n < 1 || fns.n > 16) {
            return NULL;
        }

        f->k = fns.n;
        f->fns = (pfunc**)pa_alloc(A, sizeof(pfunc*) * (size_t)fns.n);

        for (k = 0; k < fns.n; k++) {
            f->fns[k] = fn_parse(d, A, fns.u.a[k], depth + 1);

            if (!f->fns[k]) {
                return NULL;
            }

            if (f->fns[k]->n > f->n) {
                f->n = f->fns[k]->n;
            }
        }

        nums(d, pd_get(d, v, "Bounds"), f->bounds, 16);

        if (nums(d, pd_get(d, v, "Encode"), f->enc3, 32) < 2 * f->k) {
            for (k = 0; k < f->k; k++) {
                f->enc3[2 * k] = 0;
                f->enc3[2 * k + 1] = 1;
            }
        }

        return f;
    }

    if (type == 4 && v.t == PV_STREAM) {
        size_t len;
        unsigned char* data = pd_stream_data(d, v, &len, NULL, NULL);
        plex L;
        pbuf b;
        pv first;

        if (!data) {
            return NULL;
        }

        memset(&L, 0, sizeof(L));
        memset(&b, 0, sizeof(b));
        L.p = data;
        L.n = len;
        L.d = d;
        L.content = 1;
        first = pl_read(&L);

        if (first.t == PV_OP && strcmp(first.u.s, "{") == 0 && ps_block(&L, &b, 0) == 0) {
            f->nprog = (int)(b.n / (2 * sizeof(double)));
            f->prog = (double*)pa_alloc(A, b.n + 16);

            if (f->prog && b.p) {
                memcpy(f->prog, b.p, b.n);
            }
        }

        qb_free(&b);
        free(data);

        if (f->n < 1) {
            f->n = 1;
        }

        return f;
    }

    return NULL;
}

int pfn_eval(const pfunc* f, const double* in, double* out) {
    double x[4];
    int k;

    if (!f) {
        return 0;
    }

    for (k = 0; k < f->m && k < 4; k++) {
        double lo = f->domain[2 * k], hi = f->domain[2 * k + 1];

        x[k] = in[k] < lo ? lo : in[k] > hi ? hi : in[k];
    }

    switch (f->type) {
        case 0: {
            /* linear between samples in the first input, nearest in the rest */
            int idx[4], j;
            double frac = 0;
            size_t base = 0, stride = (size_t)f->n;

            for (k = 0; k < f->m; k++) {
                double lo = f->domain[2 * k], hi = f->domain[2 * k + 1];
                double e = f->encode[2 * k] + (hi > lo ? (x[k] - lo) / (hi - lo) : 0) * (f->encode[2 * k + 1] -
                           f->encode[2 * k]);

                if (e < 0) {
                    e = 0;
                }

                if (e > f->size[k] - 1) {
                    e = f->size[k] - 1;
                }

                idx[k] = (int)floor(e);

                if (k == 0) {
                    frac = e - idx[0];
                } else {
                    idx[k] = (int)floor(e + 0.5);
                }

                if (idx[k] >= f->size[k]) {
                    idx[k] = f->size[k] - 1;
                }
            }

            for (k = f->m - 1; k >= 0; k--) {
                size_t mul = 1;

                for (j = 0; j < k; j++) {
                    mul *= (size_t)f->size[j];
                }

                base += (size_t)idx[k] * mul * stride;
            }

            for (j = 0; j < f->n; j++) {
                double s0 = f->samples[base + (size_t)j], s1 = s0;

                if (idx[0] + 1 < f->size[0]) {
                    s1 = f->samples[base + stride + (size_t)j];
                }

                out[j] = f->decode[2 * j] + (s0 + (s1 - s0) * frac) * (f->decode[2 * j + 1] - f->decode[2 * j]);
            }

            break;
        }

        case 2: {
            double t = x[0], p = (f->e == 1) ? t : pow(t < 0 ? 0 : t, f->e);

            for (k = 0; k < f->n; k++) {
                out[k] = f->c0[k] + p * (f->c1[k] - f->c0[k]);
            }

            break;
        }

        case 3: {
            double t = x[0], lo = f->domain[0], hi = f->domain[1], a, b;
            int i = 0;
            double y;

            while (i < f->k - 1 && t >= f->bounds[i]) {
                i++;
            }

            a = i == 0 ? lo : f->bounds[i - 1];
            b = i == f->k - 1 ? hi : f->bounds[i];
            y = f->enc3[2 * i] + (b > a ? (t - a) / (b - a) : 0) * (f->enc3[2 * i + 1] - f->enc3[2 * i]);
            pfn_eval(f->fns[i], &y, out);
            break;
        }

        case 4: {
            double st[100];
            int n, j;

            for (k = 0; k < f->m; k++) {
                st[k] = x[k];
            }

            n = ps_run(f, st, f->m);

            for (j = 0; j < f->n; j++) {
                int at = n - f->n + j;

                out[j] = at >= 0 ? st[at] : 0;
            }

            break;
        }

        default:
            return 0;
    }

    for (k = 0; k < f->n; k++) {
        if (f->range[2 * k + 1] > f->range[2 * k]) {
            out[k] = out[k] < f->range[2 * k] ? f->range[2 * k] : out[k] > f->range[2 * k + 1] ? f->range[2 * k + 1] : out[k];
        }
    }

    return f->n;
}

/* ------------------------------------------------------------------ */
/* colour spaces                                                      */
/* ------------------------------------------------------------------ */

static void cmyk_rgb(double c, double m, double y, double k, double* rgb) {
    double c1 = 1 - c, m1 = 1 - m, y1 = 1 - y, k1 = 1 - k, x, r, g, b;

    x = c1 * m1 * y1 * k1;
    r = g = b = x;
    x = c1 * m1 * y1 * k;
    r += 0.1373 * x;
    g += 0.1216 * x;
    b += 0.1255 * x;
    x = c1 * m1 * y * k1;
    r += x;
    g += 0.9490 * x;
    x = c1 * m1 * y * k;
    r += 0.1098 * x;
    g += 0.1020 * x;
    x = c1 * m * y1 * k1;
    r += 0.9255 * x;
    b += 0.5490 * x;
    x = c1 * m * y1 * k;
    r += 0.1412 * x;
    x = c1 * m * y * k1;
    r += 0.9294 * x;
    g += 0.1098 * x;
    b += 0.1412 * x;
    x = c1 * m * y * k;
    r += 0.1333 * x;
    x = c * m1 * y1 * k1;
    g += 0.6784 * x;
    b += 0.9373 * x;
    x = c * m1 * y1 * k;
    g += 0.0588 * x;
    b += 0.1412 * x;
    x = c * m1 * y * k1;
    g += 0.6510 * x;
    b += 0.3137 * x;
    x = c * m1 * y * k;
    g += 0.0745 * x;
    x = c * m * y1 * k1;
    r += 0.1804 * x;
    g += 0.1922 * x;
    b += 0.5725 * x;
    x = c * m * y1 * k;
    b += 0.0078 * x;
    x = c * m * y * k1;
    r += 0.2118 * x;
    g += 0.2119 * x;
    b += 0.2235 * x;
    rgb[0] = clamp01(r);
    rgb[1] = clamp01(g);
    rgb[2] = clamp01(b);
}

static double lab_f(double t) {
    return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29);
}

static double srgb_gamma(double v) {
    v = clamp01(v);
    return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
}

void pcs_to_rgb(const pcspace* cs, const double* c, double* rgb) {
    if (!cs) {
        rgb[0] = rgb[1] = rgb[2] = clamp01(c[0]);
        return;
    }

    switch (cs->kind) {
        case CS_GRAY:
            rgb[0] = rgb[1] = rgb[2] = clamp01(c[0]);
            break;

        case CS_RGB:
            rgb[0] = clamp01(c[0]);
            rgb[1] = clamp01(c[1]);
            rgb[2] = clamp01(c[2]);
            break;

        case CS_CMYK:
            cmyk_rgb(clamp01(c[0]), clamp01(c[1]), clamp01(c[2]), clamp01(c[3]), rgb);
            break;

        case CS_LAB: {
            double L = c[0], a = c[1], b = c[2], M = (L + 16) / 116, X, Y, Z, r, g, bb;

            X = cs->white[0] * lab_f(M + a / 500);
            Y = cs->white[1] * lab_f(M);
            Z = cs->white[2] * lab_f(M - b / 200);
            r = 3.2406 * X - 1.5372 * Y - 0.4986 * Z;
            g = -0.9689 * X + 1.8758 * Y + 0.0415 * Z;
            bb = 0.0557 * X - 0.2040 * Y + 1.0570 * Z;
            rgb[0] = srgb_gamma(r);
            rgb[1] = srgb_gamma(g);
            rgb[2] = srgb_gamma(bb);
            break;
        }

        case CS_INDEXED: {
            int i = (int)floor(c[0] + 0.5), k, n = cs->base ? cs->base->n : 3;
            double bc[32];

            i = i < 0 ? 0 : i > cs->hival ? cs->hival : i;

            for (k = 0; k < n && k < 32; k++) {
                bc[k] = cs->lookup ? cs->lookup[i * n + k] / 255.0 : 0;
            }

            if (cs->base && cs->base->kind == CS_LAB) {     /* a Lab base's ranges */
                bc[0] *= 100;
                bc[1] = cs->base->range[0] + bc[1] * (cs->base->range[1] - cs->base->range[0]);
                bc[2] = cs->base->range[2] + bc[2] * (cs->base->range[3] - cs->base->range[2]);
            }

            pcs_to_rgb(cs->base, bc, rgb);
            break;
        }

        case CS_SEP: {
            double out[32];
            int k;

            if (cs->none) {
                rgb[0] = rgb[1] = rgb[2] = 1;
                break;
            }

            if (cs->tint && pfn_eval(cs->tint, c, out) > 0) {
                pcs_to_rgb(cs->base, out, rgb);
            } else {    /* no tint transform: the tint as grey ink */
                double t = 0;

                for (k = 0; k < cs->n; k++) {
                    t = c[k] > t ? c[k] : t;
                }

                rgb[0] = rgb[1] = rgb[2] = 1 - clamp01(t);
            }

            break;
        }

        default:
            rgb[0] = rgb[1] = rgb[2] = 0;
    }
}

static pcspace* cs_new(parena** A, int kind, int n) {
    pcspace* cs = (pcspace*)pa_alloc(A, sizeof(pcspace));

    if (cs) {
        cs->kind = kind;
        cs->n = n;
    }

    return cs;
}

pcspace* pcs_parse(pdoc* d, parena** A, pv v, pv res, int depth);

pcspace* pcs_parse(pdoc* d, parena** A, pv v, pv res, int depth) {
    v = pd_resolve(d, v);

    if (depth > 8) {
        return cs_new(A, CS_RGB, 3);
    }

    if (v.t == PV_NAME) {
        const char* s = v.u.s;

        if (strcmp(s, "DeviceGray") == 0 || strcmp(s, "G") == 0 || strcmp(s, "CalGray") == 0) {
            return cs_new(A, CS_GRAY, 1);
        }

        if (strcmp(s, "DeviceRGB") == 0 || strcmp(s, "RGB") == 0 || strcmp(s, "CalRGB") == 0) {
            return cs_new(A, CS_RGB, 3);
        }

        if (strcmp(s, "DeviceCMYK") == 0 || strcmp(s, "CMYK") == 0) {
            return cs_new(A, CS_CMYK, 4);
        }

        if (strcmp(s, "Pattern") == 0) {
            return cs_new(A, CS_PATTERN, 1);
        }

        if (strcmp(s, "I") == 0) {
            return cs_new(A, CS_INDEXED, 1);
        }

        /* a resource name */
        {
            pv named = pd_get(d, pd_get(d, res, "ColorSpace"), s);

            if (named.t != PV_NULL) {
                return pcs_parse(d, A, named, res, depth + 1);
            }
        }

        return cs_new(A, CS_RGB, 3);
    }

    if (v.t == PV_ARRAY && v.n >= 1) {
        pv k = pd_index(d, v, 0);
        const char* s = k.t == PV_NAME ? k.u.s : "";

        if (strcmp(s, "ICCBased") == 0) {
            pv st = pd_index(d, v, 1);
            int64_t n = pd_getint(d, st, "N", 3);
            pv alt = pd_get(d, st, "Alternate");

            if (n == 1) {
                return cs_new(A, CS_GRAY, 1);
            }

            if (n == 4) {
                return cs_new(A, CS_CMYK, 4);
            }

            if (n == 3) {
                if (alt.t == PV_NAME && strcmp(alt.u.s, "Lab") == 0) {
                    pcspace* cs = cs_new(A, CS_LAB, 3);

                    cs->white[0] = 0.9505;
                    cs->white[1] = 1;
                    cs->white[2] = 1.089;
                    cs->range[0] = cs->range[2] = -100;
                    cs->range[1] = cs->range[3] = 100;
                    return cs;
                }

                return cs_new(A, CS_RGB, 3);
            }

            return alt.t != PV_NULL ? pcs_parse(d, A, alt, res, depth + 1) : cs_new(A, CS_RGB, 3);
        }

        if (strcmp(s, "CalGray") == 0 || strcmp(s, "DeviceGray") == 0) {
            return cs_new(A, CS_GRAY, 1);
        }

        if (strcmp(s, "CalRGB") == 0 || strcmp(s, "DeviceRGB") == 0) {
            return cs_new(A, CS_RGB, 3);
        }

        if (strcmp(s, "DeviceCMYK") == 0) {
            return cs_new(A, CS_CMYK, 4);
        }

        if (strcmp(s, "Lab") == 0) {
            pcspace* cs = cs_new(A, CS_LAB, 3);
            pv dict = pd_index(d, v, 1);

            cs->white[0] = 0.9505;
            cs->white[1] = 1;
            cs->white[2] = 1.089;
            nums(d, pd_get(d, dict, "WhitePoint"), cs->white, 3);
            cs->range[0] = cs->range[2] = -100;
            cs->range[1] = cs->range[3] = 100;
            nums(d, pd_get(d, dict, "Range"), cs->range, 4);
            return cs;
        }

        if (strcmp(s, "Indexed") == 0 || strcmp(s, "I") == 0) {
            pcspace* cs = cs_new(A, CS_INDEXED, 1);
            pv lk = pd_index(d, v, 3);
            int nb;
            size_t need;

            cs->base = pcs_parse(d, A, pd_index(d, v, 1), res, depth + 1);
            cs->hival = (int)pd_getint(d, pv_null(), "", 0);

            {
                pv h = pd_index(d, v, 2);

                cs->hival = h.t == PV_INT ? (int)h.u.i : 255;
            }

            cs->hival = cs->hival < 0 ? 0 : cs->hival > 255 ? 255 : cs->hival;
            nb = cs->base ? cs->base->n : 3;
            need = (size_t)(cs->hival + 1) * (size_t)nb;
            cs->lookup = (unsigned char*)pa_alloc(A, need + 1);

            if (cs->lookup) {
                if (lk.t == PV_STRING) {
                    memcpy(cs->lookup, lk.u.s, (size_t)lk.n < need ? (size_t)lk.n : need);
                } else if (lk.t == PV_STREAM) {
                    size_t len;
                    unsigned char* data = pd_stream_data(d, lk, &len, NULL, NULL);

                    if (data) {
                        memcpy(cs->lookup, data, len < need ? len : need);
                        free(data);
                    }
                }
            }

            return cs;
        }

        if (strcmp(s, "Separation") == 0 || strcmp(s, "DeviceN") == 0) {
            pcspace* cs = cs_new(A, CS_SEP, 1);
            int sep = s[0] == 'S';
            pv names = pd_index(d, v, 1);

            if (!sep) {
                cs->n = names.t == PV_ARRAY ? (names.n < 32 ? names.n : 32) : 1;
            }

            if (sep && names.t == PV_NAME && strcmp(names.u.s, "None") == 0) {
                cs->none = 1;
            }

            if (sep && names.t == PV_NAME && strcmp(names.u.s, "All") == 0) {
                /* all inks: a tint is black */
            }

            cs->base = pcs_parse(d, A, pd_index(d, v, 2), res, depth + 1);
            cs->tint = fn_parse(d, A, pd_index(d, v, 3), 0);
            return cs;
        }

        if (strcmp(s, "Pattern") == 0) {
            pcspace* cs = cs_new(A, CS_PATTERN, 1);

            if (v.n >= 2) {
                cs->base = pcs_parse(d, A, pd_index(d, v, 1), res, depth + 1);
                cs->n = cs->base ? cs->base->n : 1;
            }

            return cs;
        }
    }

    return cs_new(A, CS_RGB, 3);
}

/* ------------------------------------------------------------------ */
/* shadings                                                           */
/* ------------------------------------------------------------------ */

static uint32_t pack_rgb(const double* rgb) {
    return ((uint32_t)(clamp01(rgb[0]) * 255 + 0.5) << 16) | ((uint32_t)(clamp01(rgb[1]) * 255 + 0.5) << 8) |
           (uint32_t)(clamp01(rgb[2]) * 255 + 0.5);
}

static void shade_color(const pshade* s, const double* in, int nin, double* rgb) {
    double out[32];
    int k;

    if (s->nfn == 1) {
        if (pfn_eval(s->fn[0], in, out) <= 0) {
            out[0] = 0;
        }
    } else if (s->nfn > 1) {
        for (k = 0; k < s->nfn && k < 32; k++) {
            double o[32];

            out[k] = pfn_eval(s->fn[k], in, o) > 0 ? o[0] : 0;
        }
    } else {
        for (k = 0; k < nin && k < 32; k++) {
            out[k] = in[k];
        }
    }

    pcs_to_rgb(s->cs, out, rgb);
}

typedef struct {
    const unsigned char* p;
    size_t n;
    uint64_t bit;
} bitrd;

static uint32_t bits_read(bitrd* b, int k) {
    uint32_t v = 0;
    int i;

    for (i = 0; i < k; i++, b->bit++) {
        size_t byte = (size_t)(b->bit / 8);

        v = (v << 1) | (byte < b->n ? (b->p[byte] >> (7 - b->bit % 8)) & 1 : 0);
    }

    return v;
}

static double bits_val(bitrd* b, int k, double lo, double hi) {
    double maxv = k >= 32 ? 4294967295.0 : (double)(((uint64_t)1 << k) - 1);

    return lo + bits_read(b, k) / maxv * (hi - lo);
}

typedef struct {
    double x, y, c[32];
} mvert;

static void tri_add(pbuf* b, const pshade* s, const mvert* v0, const mvert* v1, const mvert* v2, int ncomp) {
    const mvert* vs[3] = { v0, v1, v2 };
    int i;

    for (i = 0; i < 3; i++) {
        double rgb[3];
        float f[5];

        shade_color(s, vs[i]->c, ncomp, rgb);
        f[0] = (float)vs[i]->x;
        f[1] = (float)vs[i]->y;
        f[2] = (float)rgb[0];
        f[3] = (float)rgb[1];
        f[4] = (float)rgb[2];
        qb_put(b, f, sizeof(f));
    }
}

/* a point of a tensor patch: 16 control points, u and v in 0..1 */
static void tensor_pt(const double (*p)[2], double u, double v, double* x, double* y) {
    double bu[4], bv[4];
    int i, j;

    bu[0] = (1 - u) * (1 - u) * (1 - u);
    bu[1] = 3 * u * (1 - u) * (1 - u);
    bu[2] = 3 * u * u * (1 - u);
    bu[3] = u * u * u;
    bv[0] = (1 - v) * (1 - v) * (1 - v);
    bv[1] = 3 * v * (1 - v) * (1 - v);
    bv[2] = 3 * v * v * (1 - v);
    bv[3] = v * v * v;
    *x = *y = 0;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            *x += p[i * 4 + j][0] * bu[i] * bv[j];
            *y += p[i * 4 + j][1] * bu[i] * bv[j];
        }
    }
}

static void patch_tris(pbuf* b, const pshade* s, const double (*p)[2], const double (*c)[32], int ncomp) {
    enum { N = 8 };
    mvert grid[N + 1][N + 1];
    int i, j, k;

    for (i = 0; i <= N; i++) {
        for (j = 0; j <= N; j++) {
            double u = (double)i / N, v = (double)j / N;

            tensor_pt(p, u, v, &grid[i][j].x, &grid[i][j].y);

            /* colours: c[0] at (0,0), c[1] at (0,1), c[2] at (1,1), c[3] at (1,0) */
            for (k = 0; k < ncomp; k++) {
                grid[i][j].c[k] = (1 - u) * (1 - v) * c[0][k] + (1 - u) * v * c[1][k] + u * v * c[2][k] +
                                  u * (1 - v) * c[3][k];
            }
        }
    }

    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            tri_add(b, s, &grid[i][j], &grid[i + 1][j], &grid[i + 1][j + 1], ncomp);
            tri_add(b, s, &grid[i][j], &grid[i + 1][j + 1], &grid[i][j + 1], ncomp);
        }
    }
}

static void mesh_parse(pdoc* d, pshade* s, pv v, int ncomp) {
    size_t len;
    unsigned char* data = pd_stream_data(d, v, &len, NULL, NULL);
    int bpc = (int)pd_getint(d, v, "BitsPerCoordinate", 16), bpcomp = (int)pd_getint(d, v, "BitsPerComponent", 8);
    int bpf = (int)pd_getint(d, v, "BitsPerFlag", 8), k;
    double dec[64];
    int nd = nums(d, pd_get(d, v, "Decode"), dec, 64);
    bitrd br;
    pbuf b;
    int nc = s->nfn ? 1 : ncomp;

    if (!data) {
        return;
    }

    if (nd < 4 + 2 * nc) {
        free(data);
        return;
    }

    if (bpc < 1 || bpc > 32 || bpcomp < 1 || bpcomp > 16) {
        free(data);
        return;
    }

    memset(&b, 0, sizeof(b));
    br.p = data;
    br.n = len;
    br.bit = 0;

    if (s->type == 4 || s->type == 5) {
        mvert* row = NULL, *prev = NULL;
        int vpr = (int)pd_getint(d, v, "VerticesPerRow", 2);
        mvert va, vb, vc;
        int have = 0;

        if (s->type == 5) {
            if (vpr < 2 || vpr > 100000) {
                free(data);
                return;
            }

            row = (mvert*)malloc(sizeof(mvert) * (size_t)vpr);
            prev = (mvert*)malloc(sizeof(mvert) * (size_t)vpr);
        }

        while (br.bit / 8 < len) {
            if (s->type == 5) {
                int i;

                if (!row || !prev) {
                    break;
                }

                for (i = 0; i < vpr && br.bit / 8 < len; i++) {
                    row[i].x = bits_val(&br, bpc, dec[0], dec[1]);
                    row[i].y = bits_val(&br, bpc, dec[2], dec[3]);

                    for (k = 0; k < nc; k++) {
                        row[i].c[k] = bits_val(&br, bpcomp, dec[4 + 2 * k], dec[5 + 2 * k]);
                    }
                }

                if (i < vpr) {
                    break;
                }

                if (have) {
                    for (i = 0; i + 1 < vpr; i++) {
                        tri_add(&b, s, &prev[i], &prev[i + 1], &row[i], nc);
                        tri_add(&b, s, &prev[i + 1], &row[i + 1], &row[i], nc);
                    }
                }

                memcpy(prev, row, sizeof(mvert) * (size_t)vpr);
                have = 1;
                br.bit = br.bit;
                continue;
            }

            {
                int flag = (int)bits_read(&br, bpf);
                mvert nv;

                nv.x = bits_val(&br, bpc, dec[0], dec[1]);
                nv.y = bits_val(&br, bpc, dec[2], dec[3]);

                for (k = 0; k < nc; k++) {
                    nv.c[k] = bits_val(&br, bpcomp, dec[4 + 2 * k], dec[5 + 2 * k]);
                }

                br.bit = (br.bit + 7) / 8 * 8;  /* each vertex starts on a byte */

                if (flag == 0 || have < 2) {
                    if (flag == 0 && have >= 3) {
                        have = 0;
                    }

                    if (have == 0) {
                        va = nv;
                    } else if (have == 1) {
                        vb = nv;
                    } else {
                        vc = nv;
                    }

                    have++;

                    if (have == 3) {
                        tri_add(&b, s, &va, &vb, &vc, nc);
                    }
                } else if (flag == 1) {
                    va = vb;
                    vb = vc;
                    vc = nv;
                    tri_add(&b, s, &va, &vb, &vc, nc);
                } else {
                    vb = vc;
                    vc = nv;
                    tri_add(&b, s, &va, &vb, &vc, nc);
                }
            }
        }

        free(row);
        free(prev);
    } else {    /* 6 and 7: patches */
        double p[16][2], c[4][32], pp[16][2], pc[4][32];
        int have = 0;
        int tensor = s->type == 7;
        /* where each of the 12 (or 16) points of the stream goes in the 4 x 4 grid (row: u, column: v) */
        static const int order12[12] = { 0, 1, 2, 3, 7, 11, 15, 14, 13, 12, 8, 4 };
        static const int order16[16] = { 0, 1, 2, 3, 7, 11, 15, 14, 13, 12, 8, 4, 5, 6, 10, 9 };

        memset(pp, 0, sizeof(pp));
        memset(pc, 0, sizeof(pc));

        while (br.bit / 8 + 1 < len) {
            int flag = (int)bits_read(&br, bpf), ncol = 4, start = 0, i;
            double q[16][2];

            if (flag != 0 && !have) {
                break;
            }

            if (flag != 0) {
                ncol = 2;
                start = 4;
            }

            for (i = start; i < (tensor ? 16 : 12); i++) {
                q[i][0] = bits_val(&br, bpc, dec[0], dec[1]);
                q[i][1] = bits_val(&br, bpc, dec[2], dec[3]);
            }

            for (i = 4 - ncol; i < 4; i++) {
                for (k = 0; k < nc; k++) {
                    c[i][k] = bits_val(&br, bpcomp, dec[4 + 2 * k], dec[5 + 2 * k]);
                }
            }

            br.bit = (br.bit + 7) / 8 * 8;

            if (flag != 0) {
                /* the shared edge of the last patch: its points and colours */
                double e[4][2], ec[2][32];
                int pi[4];

                if (flag == 1) {
                    pi[0] = 3; pi[1] = 7; pi[2] = 11; pi[3] = 15;
                    memcpy(ec[0], pc[1], sizeof(ec[0]));
                    memcpy(ec[1], pc[2], sizeof(ec[1]));
                } else if (flag == 2) {
                    pi[0] = 15; pi[1] = 14; pi[2] = 13; pi[3] = 12;
                    memcpy(ec[0], pc[2], sizeof(ec[0]));
                    memcpy(ec[1], pc[3], sizeof(ec[1]));
                } else {
                    pi[0] = 12; pi[1] = 8; pi[2] = 4; pi[3] = 0;
                    memcpy(ec[0], pc[3], sizeof(ec[0]));
                    memcpy(ec[1], pc[0], sizeof(ec[1]));
                }

                for (i = 0; i < 4; i++) {
                    e[i][0] = pp[pi[i]][0];
                    e[i][1] = pp[pi[i]][1];
                }

                for (i = 0; i < 4; i++) {
                    q[i][0] = e[i][0];
                    q[i][1] = e[i][1];
                }

                memcpy(c[0], ec[0], sizeof(c[0]));
                memcpy(c[1], ec[1], sizeof(c[1]));
            }

            for (i = 0; i < (tensor ? 16 : 12); i++) {
                int at = tensor ? order16[i] : order12[i];

                p[at][0] = q[i][0];
                p[at][1] = q[i][1];
            }

            if (!tensor) {  /* the Coons patch's inner points from its edges */
                int a, bb;

                for (a = 1; a <= 2; a++) {
                    for (bb = 1; bb <= 2; bb++) {
                        int idx = a * 4 + bb;
                        int r = a, col = bb, j;

                        (void)r;
                        (void)col;

                        for (j = 0; j < 2; j++) {
                            double v1 = (-4 * p[(a == 1 ? 0 : 3) * 4 + (bb == 1 ? 0 : 3)][j]
                                         + 6 * (p[(a == 1 ? 0 : 3) * 4 + bb][j] + p[a * 4 + (bb == 1 ? 0 : 3)][j])
                                         - 2 * (p[(a == 1 ? 0 : 3) * 4 + (bb == 1 ? 3 : 0)][j] +
                                                p[(a == 1 ? 3 : 0) * 4 + (bb == 1 ? 0 : 3)][j])
                                         + 3 * (p[(a == 1 ? 3 : 0) * 4 + bb][j] + p[a * 4 + (bb == 1 ? 3 : 0)][j])
                                         - p[(a == 1 ? 3 : 0) * 4 + (bb == 1 ? 3 : 0)][j]) / 9.0;

                            p[idx][j] = v1;
                        }
                    }
                }
            }

            /* the colours, corners of the grid: stream order is (0,0) (0,3) (3,3) (3,0) of the u-v grid */
            patch_tris(&b, s, (const double (*)[2])p, (const double (*)[32])c, nc);
            memcpy(pp, p, sizeof(pp));
            memcpy(pc, c, sizeof(pc));
            have = 1;
        }
    }

    free(data);
    s->tri = (float*)b.p;
    s->ntri = (int32_t)(b.n / (sizeof(float) * 15));
}

pshade* psh_parse(pdoc* d, parena** A, pv v, pv res);

pshade* psh_parse(pdoc* d, parena** A, pv v, pv res) {
    pshade* s;
    pv fn;
    int i;
    double bg[32];

    v = pd_resolve(d, v);

    if (v.t != PV_DICT && v.t != PV_STREAM) {
        return NULL;
    }

    s = (pshade*)pa_alloc(A, sizeof(pshade));

    if (!s) {
        return NULL;
    }

    s->type = (int)pd_getint(d, v, "ShadingType", 0);
    s->cs = pcs_parse(d, A, pd_get(d, v, "ColorSpace"), res, 0);
    s->has_bbox = pd_rect(d, pd_get(d, v, "BBox"), s->bbox);
    s->domain[0] = 0;
    s->domain[1] = 1;
    nums(d, pd_get(d, v, "Domain"), s->domain, 2);
    nums(d, pd_get(d, v, "Coords"), s->coords, 6);

    {
        pv ex = pd_get(d, v, "Extend");

        if (ex.t == PV_ARRAY && ex.n >= 2) {
            pv a = pd_index(d, ex, 0), b = pd_index(d, ex, 1);

            s->extend[0] = a.t == PV_BOOL && a.u.i;
            s->extend[1] = b.t == PV_BOOL && b.u.i;
        }
    }

    fn = pd_get(d, v, "Function");

    if (fn.t == PV_ARRAY) {
        for (i = 0; i < fn.n && i < 8; i++) {
            s->fn[i] = fn_parse(d, A, fn.u.a[i], 0);
        }

        s->nfn = fn.n < 8 ? fn.n : 8;
    } else if (fn.t != PV_NULL) {
        s->fn[0] = fn_parse(d, A, fn, 0);
        s->nfn = s->fn[0] ? 1 : 0;
    }

    if (nums(d, pd_get(d, v, "Background"), bg, 32) >= (s->cs ? s->cs->n : 1)) {
        double rgb[3];

        pcs_to_rgb(s->cs, bg, rgb);
        s->background = pack_rgb(rgb);
        s->has_background = 1;
    }

    if (s->type == 1) {
        s->fdomain[0] = 0;
        s->fdomain[1] = 1;
        s->fdomain[2] = 0;
        s->fdomain[3] = 1;
        nums(d, pd_get(d, v, "Domain"), s->fdomain, 4);
        s->matrix[0] = s->matrix[3] = 1;
        pd_matrix(d, pd_get(d, v, "Matrix"), s->matrix);
    } else if (s->type == 2 || s->type == 3 || ((s->type >= 4 && s->type <= 7) && s->nfn)) {
        double t0 = s->domain[0], t1 = s->domain[1];

        if (s->type >= 4) {     /* a mesh's t runs through its Decode's range for the colour */
            double dec[8];

            if (nums(d, pd_get(d, v, "Decode"), dec, 8) >= 6) {
                t0 = dec[4];
                t1 = dec[5];
            }
        }

        for (i = 0; i < 256; i++) {
            double t = t0 + (t1 - t0) * i / 255.0, rgb[3];

            shade_color(s, &t, 1, rgb);
            s->lut[i] = pack_rgb(rgb);
        }

        s->has_lut = 1;
    }

    if (s->type >= 4 && s->type <= 7 && v.t == PV_STREAM) {
        mesh_parse(d, s, v, s->cs ? s->cs->n : 1);
    }

    return s;
}

/* a type 1 shading's colour at a point of its domain */
uint32_t psh_fn_color(const pshade* s, double x, double y) {
    double in[2] = { x, y }, rgb[3];

    shade_color(s, in, 2, rgb);
    return pack_rgb(rgb);
}

/* ------------------------------------------------------------------ */
/* pictures                                                           */
/* ------------------------------------------------------------------ */

void pimage_free(pimage* im) {
    while (im) {
        pimage* h = im->half;

        free(im->px);
        free(im);
        im = h;
    }
}

/* samples of bpc bits to 0..255 (through Decode), one row at a time */
static void unpack_row(const unsigned char* src, size_t avail, int32_t w, int nc, int bpc, const double* dec,
                       double* out) {
    int64_t i, total = (int64_t)w * nc;
    double maxv = bpc == 16 ? 65535.0 : (double)((1 << bpc) - 1);

    for (i = 0; i < total; i++) {
        uint32_t s = 0;

        if (bpc == 8) {
            s = (size_t)i < avail ? src[i] : 0;
        } else if (bpc == 16) {
            s = (size_t)(2 * i + 1) < avail ? ((uint32_t)src[2 * i] << 8) | src[2 * i + 1] : 0;
        } else {
            int64_t bit = i * bpc;
            size_t byte = (size_t)(bit / 8);

            s = byte < avail ? (uint32_t)(src[byte] >> (8 - bpc - bit % 8)) & ((1u << bpc) - 1) : 0;
        }

        out[i] = dec[2 * (i % nc)] + s / maxv * (dec[2 * (i % nc) + 1] - dec[2 * (i % nc)]);
    }
}

static pimage* img_new(int32_t w, int32_t h, int stencil) {
    pimage* im;

    if (w <= 0 || h <= 0 || (int64_t)w * h > ((int64_t)1 << 28)) {
        return NULL;
    }

    im = (pimage*)calloc(1, sizeof(pimage));

    if (!im) {
        return NULL;
    }

    im->w = w;
    im->h = h;
    im->stencil = stencil;
    im->bytes = (size_t)w * (size_t)h * (stencil ? 1 : 4);
    im->px = (unsigned char*)malloc(im->bytes);

    if (!im->px) {
        free(im);
        return NULL;
    }

    return im;
}

static const char* key_alt(pdoc* d, pv dict, const char* a, const char* b, pv* out) {
    *out = pd_get(d, dict, a);

    if (out->t == PV_NULL && b) {
        *out = pd_get(d, dict, b);
    }

    return NULL;
}

/* the pixels of a mask or soft mask image (gray), w x h, resampled */
static unsigned char* mask_pixels(pdoc* d, pv sm, int32_t w, int32_t h, int stencil_mask) {
    pimage* m = pdr_image_decode(d, sm, NULL, 0, pv_null());
    unsigned char* out;
    int32_t x, y;

    if (!m) {
        return NULL;
    }

    out = (unsigned char*)malloc((size_t)w * (size_t)h);

    if (out) {
        for (y = 0; y < h; y++) {
            int32_t sy = (int32_t)((int64_t)y * m->h / h);

            for (x = 0; x < w; x++) {
                int32_t sx = (int32_t)((int64_t)x * m->w / w);
                const unsigned char* p = m->px + ((size_t)sy * (size_t)m->w + (size_t)sx) * (m->stencil ? 1 : 4);

                /* a stencil's painted samples hide the picture there; a soft mask's grey is the alpha */
                out[(size_t)y * (size_t)w + (size_t)x] = m->stencil ? (unsigned char)(stencil_mask ? 255 - p[0] : p[0]) :
                                                         p[0];
            }
        }
    }

    pimage_free(m);
    return out;
}

pimage* pdr_image_decode(pdoc* d, pv dict, const unsigned char* raw, size_t rawlen, pv res) {
    pv v;
    int32_t w, h;
    int bpc, nc, stencil, filt = 0, inverted = 0, k;
    pcspace* cs = NULL;
    parena* A = NULL;
    unsigned char* data = NULL, *jpx = NULL;
    size_t len = 0;
    double dec[64];
    pimage* im = NULL;
    pv parms = pv_null();

    dict = pd_resolve(d, dict);
    key_alt(d, dict, "Width", "W", &v);
    w = v.t == PV_INT ? (int32_t)v.u.i : v.t == PV_REAL ? (int32_t)v.u.r : 0;
    key_alt(d, dict, "Height", "H", &v);
    h = v.t == PV_INT ? (int32_t)v.u.i : v.t == PV_REAL ? (int32_t)v.u.r : 0;
    key_alt(d, dict, "ImageMask", "IM", &v);
    stencil = v.t == PV_BOOL && v.u.i;
    key_alt(d, dict, "BitsPerComponent", "BPC", &v);
    bpc = v.t == PV_INT ? (int)v.u.i : stencil ? 1 : 8;

    if (raw) {      /* an inline image: its data given */
        pv f, p;

        key_alt(d, dict, "Filter", "F", &f);
        key_alt(d, dict, "DecodeParms", "DP", &p);
        data = f.t != PV_NULL ? pd_decode(d, raw, rawlen, f, p, &len, &filt, &parms) : NULL;

        if (!data) {
            data = (unsigned char*)malloc(rawlen + 1);

            if (data) {
                memcpy(data, raw, rawlen);
            }

            len = rawlen;
        }
    } else {
        data = pd_stream_data(d, dict, &len, &filt, &parms);
    }

    if (!data) {
        return NULL;
    }

    if (filt == PF_DCT) {
        int32_t jw, jh, jc;
        unsigned char* px = pd_jpeg_decode(data, len, &jw, &jh, &jc, &inverted);

        free(data);

        if (!px) {
            return NULL;
        }

        data = px;
        len = (size_t)jw * (size_t)jh * (size_t)jc;
        w = jw;
        h = jh;
        bpc = 8;

        if (stencil) {
            nc = 1;
        }
    } else if (filt == PF_CCITT) {
        int64_t kk = pd_getint(d, parms, "K", 0), cols = pd_getint(d, parms, "Columns", 1728);
        int64_t rows = pd_getint(d, parms, "Rows", h);
        pv b1 = pd_get(d, parms, "BlackIs1"), ba = pd_get(d, parms, "EncodedByteAlign");
        size_t ol;
        unsigned char* px = pd_ccitt_decode(data, len, (int32_t)kk, (int32_t)cols, (int32_t)rows,
                                            b1.t == PV_BOOL && b1.u.i, ba.t == PV_BOOL && ba.u.i, &ol);

        free(data);

        if (!px) {
            return NULL;
        }

        data = px;
        len = ol;
        bpc = 1;
    } else if (filt == PF_JBIG2) {
        pv g = pd_get(d, parms, "JBIG2Globals");
        size_t gn = 0, ol = 0;
        unsigned char* gd = g.t == PV_STREAM ? pd_stream_data(d, g, &gn, NULL, NULL) : NULL;
        unsigned char* px = pd_jbig2_decode(data, len, gd, gn, w, h, &ol);

        free(gd);

        if (px) {
            free(data);
            data = px;
            len = ol;
            bpc = 1;
        } else {
            jpx = data;     /* not decoded: a grey picture of its size stands in */
            data = NULL;
        }
    } else if (filt == PF_JPX) {
        jpx = data;     /* not decoded: a grey picture of its size stands in */
        data = NULL;
    }

    /* the colour space */
    key_alt(d, dict, "ColorSpace", "CS", &v);

    if (!stencil) {
        cs = v.t != PV_NULL ? pcs_parse(d, &A, v, res, 0) : NULL;

        if (!cs) {
            /* a JPEG's own components say */
            int32_t comps = filt == PF_DCT && w > 0 && h > 0 ? (int32_t)(len / ((size_t)w * (size_t)h)) : 1;

            cs = (pcspace*)pa_alloc(&A, sizeof(pcspace));

            if (cs) {
                cs->kind = comps == 4 ? CS_CMYK : comps == 3 ? CS_RGB : CS_GRAY;
                cs->n = comps;
            }
        }

        nc = cs ? cs->n : 1;
    } else {
        nc = 1;
    }

    if (bpc != 1 && bpc != 2 && bpc != 4 && bpc != 8 && bpc != 16) {
        bpc = 8;
    }

    for (k = 0; k < nc; k++) {
        dec[2 * k] = 0;
        dec[2 * k + 1] = cs && cs->kind == CS_INDEXED ? (double)((1 << bpc) - 1) : 1;

        if (cs && cs->kind == CS_LAB) {
            if (k == 0) {
                dec[1] = 100;
            } else {
                dec[2 * k] = cs->range[2 * (k - 1)];
                dec[2 * k + 1] = cs->range[2 * (k - 1) + 1];
            }
        }
    }

    key_alt(d, dict, "Decode", "D", &v);

    if (v.t == PV_ARRAY) {
        nums(d, v, dec, 2 * nc);
    }

    if (filt == PF_DCT && inverted && nc == 4) {
        /* the decoder undid Adobe's inversion; a Decode of [1 0 ...] already expects it inverted */
    }

    im = img_new(w, h, stencil);

    if (!im) {
        free(data);
        free(jpx);
        pa_free(A);
        return NULL;
    }

    key_alt(d, dict, "Interpolate", "I", &v);
    im->interpolate = v.t == PV_BOOL && v.u.i;

    if (jpx) {
        memset(im->px, stencil ? 0 : 200, im->bytes);

        if (!stencil) {
            int64_t i;

            for (i = 0; i < (int64_t)w * h; i++) {
                im->px[4 * i + 3] = 255;
            }
        }

        free(jpx);
        pa_free(A);
        return im;
    }

    {
        size_t row = ((size_t)w * (size_t)nc * (size_t)bpc + 7) / 8;
        double* vals = (double*)malloc(sizeof(double) * (size_t)w * (size_t)nc);
        int32_t x, y;
        int simple = !stencil && cs && (cs->kind == CS_GRAY || cs->kind == CS_RGB) && bpc == 8 &&
                     dec[0] == 0 && dec[1] == 1;
        uint32_t* lut = NULL;

        if (!vals) {
            pimage_free(im);
            free(data);
            pa_free(A);
            return NULL;
        }

        for (k = 1; k < nc && simple; k++) {
            if (dec[2 * k] != 0 || dec[2 * k + 1] != 1) {
                simple = 0;
            }
        }

        /* a single component of few bits through a table */
        if (!stencil && cs && nc == 1 && bpc <= 8 && !simple) {
            lut = (uint32_t*)malloc(sizeof(uint32_t) * 256);

            if (lut) {
                int s;

                for (s = 0; s < (1 << bpc); s++) {
                    double c = dec[0] + s / (double)((1 << bpc) - 1) * (dec[1] - dec[0]), rgb[3];

                    pcs_to_rgb(cs, &c, rgb);
                    lut[s] = pack_rgb(rgb);
                }
            }
        }

        for (y = 0; y < h; y++) {
            size_t off = (size_t)y * row;
            const unsigned char* src = off < len ? data + off : NULL;
            size_t avail = src ? len - off : 0;
            unsigned char* o = im->px + (size_t)y * (size_t)w * (stencil ? 1 : 4);

            if (stencil) {
                for (x = 0; x < w; x++) {
                    int bit = 0;

                    if (src && (size_t)(x / 8) < avail) {
                        bit = (src[x / 8] >> (7 - x % 8)) & 1;
                    }

                    /* sample 0 paints, unless Decode is [1 0] */
                    o[x] = (unsigned char)((bit ^ (dec[0] > dec[1] ? 1 : 0)) ? 0 : 255);
                }

                continue;
            }

            if (simple) {
                for (x = 0; x < w; x++) {
                    if (nc == 1) {
                        unsigned char g = src && (size_t)x < avail ? src[x] : 0;

                        o[4 * x] = o[4 * x + 1] = o[4 * x + 2] = g;
                    } else {
                        for (k = 0; k < 3; k++) {
                            o[4 * x + k] = src && (size_t)(3 * x + k) < avail ? src[3 * x + k] : 0;
                        }
                    }

                    o[4 * x + 3] = 255;
                }

                continue;
            }

            if (lut) {
                for (x = 0; x < w; x++) {
                    uint32_t s = 0, c;

                    if (src) {
                        int64_t bit = (int64_t)x * bpc;
                        size_t byte = (size_t)(bit / 8);

                        s = byte < avail ? (uint32_t)(src[byte] >> (8 - bpc - bit % 8)) & ((1u << bpc) - 1) : 0;
                    }

                    c = lut[s];
                    o[4 * x] = (unsigned char)(c >> 16);
                    o[4 * x + 1] = (unsigned char)(c >> 8);
                    o[4 * x + 2] = (unsigned char)c;
                    o[4 * x + 3] = 255;
                }

                continue;
            }

            unpack_row(src ? src : (const unsigned char*)"", avail, w, nc, bpc, dec, vals);

            for (x = 0; x < w; x++) {
                double rgb[3];

                pcs_to_rgb(cs, vals + (size_t)x * (size_t)nc, rgb);
                o[4 * x] = (unsigned char)(rgb[0] * 255 + 0.5);
                o[4 * x + 1] = (unsigned char)(rgb[1] * 255 + 0.5);
                o[4 * x + 2] = (unsigned char)(rgb[2] * 255 + 0.5);
                o[4 * x + 3] = 255;
            }
        }

        free(lut);

        /* a colour key mask */
        if (!stencil) {
            pv mk = pd_get(d, dict, "Mask");

            if (mk.t == PV_ARRAY && mk.n >= 2 * nc && bpc <= 16) {
                double rng[64];
                int nr = nums(d, mk, rng, 64);
                uint32_t maxv = (1u << bpc) - 1;

                (void)maxv;

                for (y = 0; y < h && nr >= 2 * nc; y++) {
                    size_t off = (size_t)y * row;
                    const unsigned char* src = off < len ? data + off : NULL;
                    size_t avail = src ? len - off : 0;

                    for (x = 0; x < w && src; x++) {
                        int inside = 1;

                        for (k = 0; k < nc; k++) {
                            int64_t i = (int64_t)x * nc + k, bit = i * bpc;
                            size_t byte = (size_t)(bit / 8);
                            uint32_t s = bpc == 8 ? (byte < avail ? src[byte] : 0) :
                                         bpc == 16 ? (byte + 1 < avail ? ((uint32_t)src[byte] << 8) | src[byte + 1] : 0) :
                                         (byte < avail ? (uint32_t)(src[byte] >> (8 - bpc - bit % 8)) & ((1u << bpc) - 1) : 0);

                            if (s < rng[2 * k] || s > rng[2 * k + 1]) {
                                inside = 0;
                            }
                        }

                        if (inside) {
                            im->px[((size_t)y * (size_t)w + (size_t)x) * 4 + 3] = 0;
                        }
                    }
                }
            } else if (mk.t == PV_STREAM) {     /* a stencil hiding the picture */
                unsigned char* m = mask_pixels(d, mk, w, h, 1);

                if (m) {
                    int64_t i;

                    for (i = 0; i < (int64_t)w * h; i++) {
                        im->px[4 * i + 3] = m[i];
                    }

                    free(m);
                }
            }

            {
                pv sm = pd_get(d, dict, "SMask");

                if (sm.t == PV_STREAM) {
                    unsigned char* m = mask_pixels(d, sm, w, h, 0);

                    if (m) {
                        int64_t i;

                        for (i = 0; i < (int64_t)w * h; i++) {
                            im->px[4 * i + 3] = (unsigned char)(im->px[4 * i + 3] * m[i] / 255);
                        }

                        free(m);
                    }
                }
            }
        }

        free(vals);
    }

    free(data);
    pa_free(A);
    return im;
}

/* ---- the cache ---- */

typedef struct {
    const void* key;            /* the stream's pstream */
    pimage* im;
} icache;

pimage* pdr_image_fetch(pdoc* d, dimgref* r) {
    icache* c = (icache*)d->imgcache;
    const void* key;
    int32_t i;
    pimage* im;

    if (r->own) {
        return r->own;
    }

    if (r->stream.t != PV_STREAM) {
        return NULL;
    }

    key = r->stream.u.st;

    for (i = d->nimg - 1; i >= 0; i--) {
        if (c[i].key == key) {
            icache e = c[i];

            /* to the end: the most recently drawn */
            memmove(&c[i], &c[i + 1], sizeof(icache) * (size_t)(d->nimg - i - 1));
            c[d->nimg - 1] = e;
            return e.im;
        }
    }

    im = pdr_image_decode(d, r->stream, NULL, 0, r->res);

    /* a picture of tens of megapixels (a figure scanned or plotted at print resolution) kept at most 8: halved
       until it is, so a page of them fits the cache; zoomed past its size, it is drawn from what is kept */
    while (im && (int64_t)im->w * im->h > 8000000 && im->w >= 4 && im->h >= 4) {
        pimage* h = pimage_level(d, im, im->w / 2.0 - 2, im->h / 2.0 - 2);

        if (h == im || !h) {
            break;
        }

        im->half = NULL;    /* the half kept on its own, the full size let go */
        d->imgbytes -= h->bytes;
        pimage_free(im);
        im = h;
    }

    if (!im) {
        /* a picture that fails is remembered as an empty one, so it is not tried on every draw */
        im = img_new(1, 1, 0);

        if (im) {
            memset(im->px, 0, 4);
        }
    }

    if (!im) {
        return NULL;
    }

    /* room made: the least recently drawn go first */
    while (d->nimg > 1 && d->imgbytes + im->bytes > IMG_BUDGET) {
        pimage* h;

        d->imgbytes -= c[0].im->bytes;

        for (h = c[0].im->half; h; h = h->half) {
            d->imgbytes -= h->bytes;
        }

        pimage_free(c[0].im);
        memmove(&c[0], &c[1], sizeof(icache) * (size_t)(d->nimg - 1));
        d->nimg--;
    }

    if (d->nimg >= d->capimg) {
        int32_t nc = d->capimg ? d->capimg * 2 : 32;
        icache* q = (icache*)realloc(c, sizeof(icache) * (size_t)nc);

        if (!q) {
            pimage_free(im);
            return NULL;
        }

        d->imgcache = q;
        c = q;
        d->capimg = nc;
    }

    c[d->nimg].key = key;
    c[d->nimg].im = im;
    d->nimg++;
    d->imgbytes += im->bytes;
    return im;
}

void pd_image_cache_free(pdoc* d) {
    icache* c = (icache*)d->imgcache;
    int32_t i;

    for (i = 0; i < d->nimg; i++) {
        pimage_free(c[i].im);
    }

    free(c);
    d->imgcache = NULL;
    d->nimg = d->capimg = 0;
    d->imgbytes = 0;
}

/* the size a picture is drawn from: halved until it is no more than twice what it covers */
pimage* pimage_level(pdoc* d, pimage* im, double px_w, double px_h);

pimage* pimage_level(pdoc* d, pimage* im, double px_w, double px_h) {
    while (im && im->w > 2 * px_w + 2 && im->h > 2 * px_h + 2 && im->w >= 4 && im->h >= 4) {
        if (!im->half) {
            int32_t w2 = im->w / 2, h2 = im->h / 2, x, y, k, bpp = im->stencil ? 1 : 4;
            pimage* h = img_new(w2, h2, im->stencil);

            if (!h) {
                return im;
            }

            h->interpolate = im->interpolate;

            for (y = 0; y < h2; y++) {
                const unsigned char* a = im->px + (size_t)(2 * y) * (size_t)im->w * (size_t)bpp;
                const unsigned char* b = a + (size_t)im->w * (size_t)bpp;
                unsigned char* o = h->px + (size_t)y * (size_t)w2 * (size_t)bpp;

                for (x = 0; x < w2; x++) {
                    if (bpp == 1) {
                        o[x] = (unsigned char)((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) / 4);
                    } else {
                        /* averaged weighted by alpha, so clear pixels do not darken the edge */
                        int al = a[8 * x + 3] + a[8 * x + 7] + b[8 * x + 3] + b[8 * x + 7];

                        for (k = 0; k < 3; k++) {
                            int s = a[8 * x + k] * a[8 * x + 3] + a[8 * x + 4 + k] * a[8 * x + 7] +
                                    b[8 * x + k] * b[8 * x + 3] + b[8 * x + 4 + k] * b[8 * x + 7];

                            o[4 * x + k] = (unsigned char)(al ? (s + al / 2) / al : 0);
                        }

                        o[4 * x + 3] = (unsigned char)((al + 2) / 4);
                    }
                }
            }

            im->half = h;
            d->imgbytes += h->bytes;
        }

        im = im->half;
    }

    return im;
}
