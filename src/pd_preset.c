/*
 * Office's preset shapes (DrawingML prstGeom): their guides worked out for a size and adjustments, their paths
 * as moves, lines and cubic curves (an arc as the curves that make it), their handles where they are, and a
 * handle dragged to a point as the adjustments that put it there.  The shapes themselves are ECMA-376's
 * presetShapeDefinitions.xml, in pd_presets.inc (tools/presets.py).
 */
#include "parade_convert.h"
#include "pd_preset.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"    /* a few shapes are long: every compiler in use takes them */
#endif
#include "pd_presets.inc"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define PV_MAXG 256

typedef struct {
    char name[PV_MAXG][24];
    double val[PV_MAXG];
    int n;
    double w, h;
} pv_env;

static const char* pv_def(const char* name) {
    int lo = 0, hi = (int)(sizeof(pd_presets) / sizeof(pd_presets[0])) - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, pd_presets[mid].name);

        if (c == 0) {
            return pd_presets[mid].def;
        }

        if (c < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    return NULL;
}

int pd_preset_known(const char* name) {
    return name && pv_def(name) != NULL;
}

PD_API int pd_preset_count(void) {
    return (int)(sizeof(pd_presets) / sizeof(pd_presets[0]));
}

PD_API const char* pd_preset_name(int i) {
    return i >= 0 && i < pd_preset_count() ? pd_presets[i].name : NULL;
}

/* a guide's name, a built-in (w, h, hc, ss, wd2, cd4, ...), or a number */
static double pv_arg(const pv_env* E, const char* a) {
    double w = E->w, h = E->h, ss = w < h ? w : h, ls = w > h ? w : h;
    int i;

    if (!strcmp(a, "3cd4")) return 16200000;   /* names that begin with a digit, before the numbers */
    if (!strcmp(a, "3cd8")) return 8100000;
    if (!strcmp(a, "5cd8")) return 13500000;
    if (!strcmp(a, "7cd8")) return 18900000;

    if ((a[0] >= '0' && a[0] <= '9') || a[0] == '-' || a[0] == '+' || a[0] == '.') {
        return atof(a);
    }

    for (i = E->n - 1; i >= 0; i--) {
        if (!strcmp(E->name[i], a)) {
            return E->val[i];
        }
    }

    if (!strcmp(a, "w") || !strcmp(a, "r")) return w;
    if (!strcmp(a, "h") || !strcmp(a, "b")) return h;
    if (!strcmp(a, "l") || !strcmp(a, "t")) return 0;
    if (!strcmp(a, "hc")) return w / 2;
    if (!strcmp(a, "vc")) return h / 2;
    if (!strcmp(a, "ss")) return ss;
    if (!strcmp(a, "ls")) return ls;
    if (!strncmp(a, "ssd", 3)) return ss / atof(a + 3);
    if (!strncmp(a, "wd", 2)) return w / atof(a + 2);
    if (!strncmp(a, "hd", 2)) return h / atof(a + 2);
    if (!strcmp(a, "cd2")) return 10800000;
    if (!strcmp(a, "cd4")) return 5400000;
    if (!strcmp(a, "cd8")) return 2700000;
    return 0;
}

#define PV_RAD(a) ((a) / 60000.0 * 3.14159265358979323846 / 180)

/* a guide's formula worked out */
static double pv_fmla(const pv_env* E, const char* f) {
    char op[8] = "", a[3][40];
    double x, y, z;
    int n;

    a[0][0] = a[1][0] = a[2][0] = '\0';
    n = sscanf(f, "%7s %39s %39s %39s", op, a[0], a[1], a[2]);
    x = n > 1 ? pv_arg(E, a[0]) : 0;
    y = n > 2 ? pv_arg(E, a[1]) : 0;
    z = n > 3 ? pv_arg(E, a[2]) : 0;

    if (!strcmp(op, "*/")) return z != 0 ? x * y / z : 0;
    if (!strcmp(op, "+-")) return x + y - z;
    if (!strcmp(op, "+/")) return z != 0 ? (x + y) / z : 0;
    if (!strcmp(op, "?:")) return x > 0 ? y : z;
    if (!strcmp(op, "abs")) return fabs(x);
    if (!strcmp(op, "at2")) return atan2(y, x) * 180 / 3.14159265358979323846 * 60000;
    if (!strcmp(op, "cat2")) return x * cos(atan2(z, y));
    if (!strcmp(op, "sat2")) return x * sin(atan2(z, y));
    if (!strcmp(op, "cos")) return x * cos(PV_RAD(y));
    if (!strcmp(op, "sin")) return x * sin(PV_RAD(y));
    if (!strcmp(op, "tan")) return x * tan(PV_RAD(y));
    if (!strcmp(op, "max")) return x > y ? x : y;
    if (!strcmp(op, "min")) return x < y ? x : y;
    if (!strcmp(op, "mod")) return sqrt(x * x + y * y + z * z);
    if (!strcmp(op, "pin")) return y < x ? x : y > z ? z : y;
    if (!strcmp(op, "sqrt")) return x > 0 ? sqrt(x) : 0;
    if (!strcmp(op, "val")) return x;
    return 0;
}

static void pv_set(pv_env* E, const char* name, double v) {
    if (E->n < PV_MAXG) {
        snprintf(E->name[E->n], sizeof(E->name[0]), "%s", name);
        E->val[E->n++] = v;
    }
}

/* the next statement of a definition into S (at most N - 1 bytes); the place after it */
static const char* pv_next(const char* d, char* s, size_t n) {
    size_t k = 0;

    while (*d && *d != ';') {
        if (k + 1 < n) {
            s[k++] = *d;
        }

        d++;
    }

    s[k] = '\0';
    return *d == ';' ? d + 1 : d;
}

/* the adjustments given: "adj1=5000 adj2=-200" (or with commas) -- its value for NAME into V */
static int pv_given(const char* adj, const char* name, double* v) {
    size_t n = strlen(name);
    const char* p = adj;

    while (p && *p) {
        while (*p == ' ' || *p == ',' || *p == ';') {
            p++;
        }

        if (!strncmp(p, name, n) && p[n] == '=') {
            *v = atof(p + n + 1);
            return 1;
        }

        while (*p && *p != ' ' && *p != ',' && *p != ';') {
            p++;
        }
    }

    return 0;
}

/* the guides for a size and adjustments (and a handle's adjustment overridden: OVN = OVV) */
static const char* pv_env_make(pv_env* E, const char* def, double w, double h, const char* adj, const char* ovn,
                               double ovv) {
    char s[256], nm[40];
    const char* d = def, *rest;

    E->n = 0;
    E->w = w;
    E->h = h;

    while (*d) {
        const char* at = d;

        d = pv_next(d, s, sizeof(s));

        if ((s[0] == 'a' || s[0] == 'g') && s[1] == ' ' && sscanf(s + 2, "%39s", nm) == 1) {
            double v;

            rest = s + 2 + strlen(nm);

            if (s[0] == 'a' && ovn && !strcmp(nm, ovn)) {
                v = ovv;
            } else if (!(s[0] == 'a' && pv_given(adj, nm, &v))) {
                v = pv_fmla(E, rest + 1);
            }

            pv_set(E, nm, v);
        } else {
            return at;     /* the handles and paths, after the guides */
        }
    }

    return d;
}

/* ---- paths ---- */

typedef struct {
    char op;            /* m, l, c (two controls and the end), z */
    double p[6];
} pv_cmd;

typedef struct {
    pv_cmd* c;
    int n, cap;
    int npath;
    struct {
        int start, n;
        char fill[16];
        int stroke;
    } path[32];
} pv_out;

static void pv_put(pv_out* O, char op, const double* p, int np) {
    if (O->n == O->cap) {
        int cap = O->cap ? O->cap * 2 : 64;
        pv_cmd* c = (pv_cmd*)realloc(O->c, sizeof(pv_cmd) * cap);

        if (!c) {
            return;
        }

        O->c = c;
        O->cap = cap;
    }

    O->c[O->n].op = op;
    memset(O->c[O->n].p, 0, sizeof(O->c[O->n].p));
    memcpy(O->c[O->n].p, p, sizeof(double) * np);
    O->n++;
    O->path[O->npath - 1].n++;
}

/* a point of an ellipse of radii WR, HR at an angle as DrawingML means it (seen from its centre) */
static double pv_param(double wr, double hr, double ang) {
    return atan2(wr * sin(ang), hr * cos(ang));
}

static void pv_paths(const pv_env* E, const char* d, pv_out* O) {
    char s[256], f[16];
    double pw = 0, ph = 0, sx = 1, sy = 1, cx = 0, cy = 0, x0 = 0, y0 = 0;
    char a[6][40];

    while (*d) {
        int n;

        d = pv_next(d, s, sizeof(s));
        a[0][0] = '\0';
        n = sscanf(s + 2, "%39s %39s %39s %39s %39s %39s", a[0], a[1], a[2], a[3], a[4], a[5]);

        if (s[0] == 'p' && s[1] == ' ' && O->npath < 32) {
            int st = 1;

            f[0] = '\0';
            sscanf(s + 2, "%lf %lf %15s %d", &pw, &ph, f, &st);
            sx = pw > 0 ? E->w / pw : 1;
            sy = ph > 0 ? E->h / ph : 1;
            O->npath++;
            O->path[O->npath - 1].start = O->n;
            O->path[O->npath - 1].n = 0;
            snprintf(O->path[O->npath - 1].fill, sizeof(O->path[0].fill), "%s", f[0] ? f : "norm");
            O->path[O->npath - 1].stroke = st;
        } else if (O->npath == 0) {
            continue;   /* a handle */
        } else if (s[0] == 'm' && n >= 2) {
            double p[2] = { pv_arg(E, a[0]) * sx, pv_arg(E, a[1]) * sy };

            pv_put(O, 'm', p, 2);
            cx = x0 = p[0];
            cy = y0 = p[1];
        } else if (s[0] == 'l' && n >= 2) {
            double p[2] = { pv_arg(E, a[0]) * sx, pv_arg(E, a[1]) * sy };

            pv_put(O, 'l', p, 2);
            cx = p[0];
            cy = p[1];
        } else if (s[0] == 'q' && n >= 4) {     /* as a cubic */
            double qx = pv_arg(E, a[0]) * sx, qy = pv_arg(E, a[1]) * sy, ex = pv_arg(E, a[2]) * sx,
                   ey = pv_arg(E, a[3]) * sy;
            double p[6] = { cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), ex + 2.0 / 3 * (qx - ex),
                            ey + 2.0 / 3 * (qy - ey), ex, ey };

            pv_put(O, 'c', p, 6);
            cx = ex;
            cy = ey;
        } else if (s[0] == 'c' && n >= 6) {
            double p[6];
            int k;

            for (k = 0; k < 6; k++) {
                p[k] = pv_arg(E, a[k]) * (k % 2 ? sy : sx);
            }

            pv_put(O, 'c', p, 6);
            cx = p[4];
            cy = p[5];
        } else if (s[0] == 'A' && n >= 4) {
            /* an arc from where the path is: as curves of at most a quarter turn each */
            double wr = pv_arg(E, a[0]) * sx, hr = pv_arg(E, a[1]) * sy, st = PV_RAD(pv_arg(E, a[2])),
                   sw = PV_RAD(pv_arg(E, a[3]));
            double t0 = pv_param(wr, hr, st), ox = cx - wr * cos(t0), oy = cy - hr * sin(t0);
            int k, segs = (int)ceil(fabs(sw) / (3.14159265358979323846 / 2) - 1e-9);

            if (segs < 1) {
                segs = 1;
            }

            for (k = 0; k < segs; k++) {
                double a0 = st + sw * k / segs, a1 = st + sw * (k + 1) / segs;
                double u0 = pv_param(wr, hr, a0), u1 = pv_param(wr, hr, a1), kk, p[6];

                while (sw > 0 && u1 < u0) u1 += 2 * 3.14159265358979323846;
                while (sw < 0 && u1 > u0) u1 -= 2 * 3.14159265358979323846;
                kk = 4.0 / 3 * tan((u1 - u0) / 4);
                p[0] = ox + wr * cos(u0) - kk * wr * sin(u0);
                p[1] = oy + hr * sin(u0) + kk * hr * cos(u0);
                p[4] = ox + wr * cos(u1);
                p[5] = oy + hr * sin(u1);
                p[2] = p[4] + kk * wr * sin(u1);
                p[3] = p[5] - kk * hr * cos(u1);
                pv_put(O, 'c', p, 6);
            }

            cx = ox + wr * cos(pv_param(wr, hr, st + sw));
            cy = oy + hr * sin(pv_param(wr, hr, st + sw));
        } else if (s[0] == 'z') {
            double p[2] = { x0, y0 };

            pv_put(O, 'z', p, 0);
            cx = x0;
            cy = y0;
        }
    }
}

static int pv_eval(const char* name, double w, double h, const char* adj, const char* ovn, double ovv, pv_env* E,
                   pv_out* O, const char** after) {
    const char* def = pv_def(name), *d;

    if (!def) {
        return 0;
    }

    d = pv_env_make(E, def, w, h, adj, ovn, ovv);

    if (after) {
        *after = d;
    }

    if (O) {
        memset(O, 0, sizeof(*O));
        pv_paths(E, d, O);
    }

    return 1;
}

int pd_preset_flatten(const char* name, double w, double h, const char* adj, pd_preset_flat* F) {
    pv_env* E = (pv_env*)malloc(sizeof(pv_env));
    pv_out O;
    int i, k, q;

    memset(F, 0, sizeof(*F));

    if (!E || !pv_eval(name, w, h, adj, NULL, 0, E, &O, NULL)) {
        free(E);
        return 0;
    }

    for (i = 0; i < O.npath && i < PD_PRESET_MAXPATH; i++) {
        double cx = 0, cy = 0, sx = 0, sy = 0;
        int ring = 0;

        F->path[i].start = F->n;
        F->path[i].closed = 0;
        snprintf(F->path[i].fill, sizeof(F->path[i].fill), "%s", O.path[i].fill);
        F->path[i].stroke = O.path[i].stroke;

        for (k = O.path[i].start; k < O.path[i].start + O.path[i].n; k++) {
            const pv_cmd* c = &O.c[k];

#define PUT(X, Y) do { if (F->n < PD_PRESET_MAXPT) { F->xy[2 * F->n] = (X); F->xy[2 * F->n + 1] = (Y); F->n++; } } while (0)
            if (c->op == 'm') {
                if (ring) {
                    PUT(NAN, NAN);
                }

                ring = 1;
                sx = cx = c->p[0];
                sy = cy = c->p[1];
                PUT(cx, cy);
            } else if (c->op == 'l') {
                cx = c->p[0];
                cy = c->p[1];
                PUT(cx, cy);
            } else if (c->op == 'c') {
                for (q = 1; q <= 12; q++) {
                    double t = q / 12.0, u = 1 - t;

                    PUT(u * u * u * cx + 3 * u * u * t * c->p[0] + 3 * u * t * t * c->p[2] + t * t * t * c->p[4],
                        u * u * u * cy + 3 * u * u * t * c->p[1] + 3 * u * t * t * c->p[3] + t * t * t * c->p[5]);
                }

                cx = c->p[4];
                cy = c->p[5];
            } else if (c->op == 'z') {
                F->path[i].closed = 1;
                cx = sx;
                cy = sy;
            }
#undef PUT
        }

        F->path[i].n = F->n - F->path[i].start;
        F->npath = i + 1;
    }

    free(O.c);
    free(E);
    return F->npath > 0;
}

int pd_preset_text_rect(const char* name, double w, double h, const char* adj, double* l, double* t, double* r,
                        double* b) {
    const char* def = pv_def(name), *d;
    pv_env* E;
    char s[256], a[4][40];
    int ok = 0;

    if (!def || !(E = (pv_env*)malloc(sizeof(pv_env)))) {
        return 0;
    }

    d = pv_env_make(E, def, w, h, adj, NULL, 0);

    while (*d && !ok) {
        d = pv_next(d, s, sizeof(s));

        if (s[0] == 'r' && s[1] == ' ' && sscanf(s + 2, "%39s %39s %39s %39s", a[0], a[1], a[2], a[3]) == 4) {
            *l = pv_arg(E, a[0]);
            *t = pv_arg(E, a[1]);
            *r = pv_arg(E, a[2]);
            *b = pv_arg(E, a[3]);
            ok = *r > *l && *b > *t;
        }
    }

    free(E);
    return ok;
}


/* ---- the API: a preset as JSON, a handle dragged ---- */

typedef struct {
    char* buf;
    size_t cap, len;
} pv_str;

static void pv_printf(pv_str* S, const char* fmt, ...) {
    va_list ap;
    int n;
    char tmp[512];

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n > 0) {
        if (S->len + (size_t)n < S->cap) {
            memcpy(S->buf + S->len, tmp, (size_t)n);
        }

        S->len += (size_t)n;
    }
}

/* the handles of a definition (D: after the guides): each worked out as pos, its adjustments and their ranges */
typedef struct {
    int polar;                  /* 0: x/y, 1: polar */
    char g1[40], g2[40];        /* x (or r) and y (or angle) adjustments; "" for none */
    double min1, max1, min2, max2, x, y;
} pv_handle;

static int pv_handles(const pv_env* E, const char* d, pv_handle* H, int max) {
    char s[256], a[8][40];
    int n = 0;

    while (*d && n < max) {
        int k;

        d = pv_next(d, s, sizeof(s));

        if (s[0] != 'h') {
            continue;
        }

        memset(&H[n], 0, sizeof(H[n]));

        if (!strncmp(s, "hxy ", 4) && sscanf(s + 4, "%39s %39s %39s %39s %39s %39s %39s %39s", a[0], a[1], a[2], a[3],
                                              a[4], a[5], a[6], a[7]) == 8) {
            snprintf(H[n].g1, sizeof(H[n].g1), "%s", a[0]);
            snprintf(H[n].g2, sizeof(H[n].g2), "%s", a[3]);
            k = 1;
        } else if ((!strncmp(s, "hx ", 3) || !strncmp(s, "hy ", 3)) &&
                   sscanf(s + 3, "%39s %39s %39s %39s %39s", a[0], a[1], a[2], a[6], a[7]) == 5) {
            snprintf(s[1] == 'x' ? H[n].g1 : H[n].g2, sizeof(H[n].g1), "%s", a[0]);

            if (s[1] == 'y') {
                memcpy(a[4], a[1], sizeof(a[4]));
                memcpy(a[5], a[2], sizeof(a[5]));
                strcpy(a[1], "-");
                strcpy(a[2], "-");
            } else {
                strcpy(a[4], "-");
                strcpy(a[5], "-");
            }

            k = 1;
        } else if (!strncmp(s, "hp ", 3) && sscanf(s + 3, "%39s %39s %39s %39s %39s %39s %39s %39s", a[0], a[1], a[2],
                                                   a[3], a[4], a[5], a[6], a[7]) == 8) {
            H[n].polar = 1;
            snprintf(H[n].g1, sizeof(H[n].g1), "%s", strcmp(a[0], "-") ? a[0] : "");
            snprintf(H[n].g2, sizeof(H[n].g2), "%s", strcmp(a[3], "-") ? a[3] : "");
            k = 1;
        } else {
            k = 0;
        }

        if (k) {
            H[n].min1 = strcmp(a[1], "-") ? pv_arg(E, a[1]) : 0;
            H[n].max1 = strcmp(a[2], "-") ? pv_arg(E, a[2]) : 0;
            H[n].min2 = strcmp(a[4], "-") ? pv_arg(E, a[4]) : 0;
            H[n].max2 = strcmp(a[5], "-") ? pv_arg(E, a[5]) : 0;
            H[n].x = pv_arg(E, a[6]);
            H[n].y = pv_arg(E, a[7]);
            n++;
        }
    }

    return n;
}

static void pv_json_str(pv_str* S, const char* s) {
    pv_printf(S, "\"%s\"", s);    /* names of guides and fills: letters and digits */
}

PD_API size_t pd_preset_json(const char* name, double w, double h, const char* adj, char* buf, size_t cap) {
    pv_env* E;
    pv_out O;
    pv_handle H[16];
    pv_str S = { buf, cap, 0 };
    const char* def = name ? pv_def(name) : NULL, *d, *after = NULL;
    char s[256], nm[40];
    int i, k, nh, first;

    if (!def) {
        if (buf && cap) {
            buf[0] = '\0';
        }

        return 0;
    }

    E = (pv_env*)malloc(sizeof(pv_env));

    if (!E) {
        return 0;
    }

    pv_eval(name, w, h, adj, NULL, 0, E, &O, &after);
    pv_printf(&S, "{\"adj\":[");
    first = 1;

    for (d = def; *d;) {    /* each adjustment: its name, its value, its default */
        d = pv_next(d, s, sizeof(s));

        if (s[0] == 'a' && s[1] == ' ' && sscanf(s + 2, "%39s", nm) == 1) {
            pv_env* D = (pv_env*)malloc(sizeof(pv_env));
            double dv = 0;

            if (D) {
                D->n = 0;
                D->w = w;
                D->h = h;
                dv = pv_fmla(D, s + 3 + strlen(nm));
                free(D);
            }

            pv_printf(&S, "%s[", first ? "" : ",");
            pv_json_str(&S, nm);
            pv_printf(&S, ",%.6g,%.6g]", pv_arg(E, nm), dv);
            first = 0;
        }
    }

    pv_printf(&S, "],\"handles\":[");
    nh = pv_handles(E, after, H, 16);

    for (i = 0; i < nh; i++) {
        pv_printf(&S, "%s{\"polar\":%d,\"g1\":", i ? "," : "", H[i].polar);
        pv_json_str(&S, H[i].g1);
        pv_printf(&S, ",\"g2\":");
        pv_json_str(&S, H[i].g2);
        pv_printf(&S, ",\"x\":%.6g,\"y\":%.6g}", H[i].x, H[i].y);
    }

    pv_printf(&S, "],\"paths\":[");

    for (i = 0; i < O.npath; i++) {
        pv_printf(&S, "%s{\"fill\":", i ? "," : "");
        pv_json_str(&S, O.path[i].fill);
        pv_printf(&S, ",\"stroke\":%d,\"cmds\":[", O.path[i].stroke);

        for (k = O.path[i].start; k < O.path[i].start + O.path[i].n; k++) {
            const pv_cmd* c = &O.c[k];

            if (k > O.path[i].start) {
                pv_printf(&S, ",");
            }

            if (c->op == 'z') {
                pv_printf(&S, "[\"z\"]");
            } else if (c->op == 'c') {
                pv_printf(&S, "[\"c\",%.6g,%.6g,%.6g,%.6g,%.6g,%.6g]", c->p[0], c->p[1], c->p[2], c->p[3], c->p[4],
                          c->p[5]);
            } else {
                pv_printf(&S, "[\"%c\",%.6g,%.6g]", c->op, c->p[0], c->p[1]);
            }
        }

        pv_printf(&S, "]}");
    }

    pv_printf(&S, "],\"sites\":[");
    first = 1;

    for (d = after ? after : def; *d;) {    /* where a connector's ends go: each site and its direction */
        char a[3][40];

        d = pv_next(d, s, sizeof(s));

        if (s[0] == 'x' && s[1] == ' ' && sscanf(s + 2, "%39s %39s %39s", a[0], a[1], a[2]) == 3) {
            pv_printf(&S, "%s[%.6g,%.6g,%.6g]", first ? "" : ",", pv_arg(E, a[1]), pv_arg(E, a[2]), pv_arg(E, a[0]));
            first = 0;
        }
    }

    pv_printf(&S, "]}");

    if (buf && cap) {
        buf[S.len < cap ? S.len : cap - 1] = '\0';
    }

    free(O.c);
    free(E);
    return S.len;
}

/* where handle K is with adjustment G at V (the others as ADJ gives them) */
static int pv_handle_at(const char* name, double w, double h, const char* adj, int k, const char* g, double v,
                        pv_env* E, double* x, double* y) {
    const char* after = NULL;
    pv_handle H[16];
    int nh;

    if (!pv_eval(name, w, h, adj, g, v, E, NULL, &after)) {
        return 0;
    }

    nh = pv_handles(E, after, H, 16);

    if (k >= nh) {
        return 0;
    }

    *x = H[k].x;
    *y = H[k].y;
    return 1;
}

/* the value of adjustment G (between LO and HI) that brings handle K nearest U, V */
static double pv_search(const char* name, double w, double h, const char* adj, int k, const char* g, double lo,
                        double hi, double u, double v, pv_env* E) {
    double best = lo, bd = 1e300, x, y, step, a, b;
    int i, it;

    if (hi < lo) {
        double t = lo;

        lo = hi;
        hi = t;
    }

    for (i = 0; i <= 64; i++) {
        double t = lo + (hi - lo) * i / 64;

        if (pv_handle_at(name, w, h, adj, k, g, t, E, &x, &y) && (x - u) * (x - u) + (y - v) * (y - v) < bd) {
            bd = (x - u) * (x - u) + (y - v) * (y - v);
            best = t;
        }
    }

    step = (hi - lo) / 64;
    a = best - step > lo ? best - step : lo;
    b = best + step < hi ? best + step : hi;

    for (it = 0; it < 40; it++) {   /* nearer, between the samples either side */
        double m1 = a + (b - a) / 3, m2 = b - (b - a) / 3, d1 = 1e300, d2 = 1e300;

        if (pv_handle_at(name, w, h, adj, k, g, m1, E, &x, &y)) d1 = (x - u) * (x - u) + (y - v) * (y - v);
        if (pv_handle_at(name, w, h, adj, k, g, m2, E, &x, &y)) d2 = (x - u) * (x - u) + (y - v) * (y - v);

        if (d1 < d2) {
            b = m2;
        } else {
            a = m1;
        }
    }

    return (a + b) / 2;
}

PD_API size_t pd_preset_drag(const char* name, double w, double h, const char* adj, int handle, double u, double v,
                             char* buf, size_t cap) {
    pv_env* E;
    pv_handle H[16];
    pv_str S = { buf, cap, 0 };
    const char* after = NULL, *def = name ? pv_def(name) : NULL, *d;
    char cur[512], s[256], nm[40];
    int nh, round, first = 1;

    if (buf && cap) {
        buf[0] = '\0';
    }

    E = def ? (pv_env*)malloc(sizeof(pv_env)) : NULL;

    if (!E) {
        return 0;
    }

    snprintf(cur, sizeof(cur), "%s", adj ? adj : "");

    for (round = 0; round < 3; round++) {    /* each of the handle's adjustments in turn, a few times over */
        int which;

        pv_eval(name, w, h, cur, NULL, 0, E, NULL, &after);
        nh = pv_handles(E, after, H, 16);

        if (handle < 0 || handle >= nh) {
            free(E);
            return 0;
        }

        for (which = 0; which < 2; which++) {
            const char* g = which ? H[handle].g2 : H[handle].g1;
            double lo = which ? H[handle].min2 : H[handle].min1, hi = which ? H[handle].max2 : H[handle].max1, nv;
            char next[512];
            size_t n = 0;

            if (!g[0]) {
                continue;
            }

            nv = pv_search(name, w, h, cur, handle, g, lo, hi, u, v, E);
            pv_eval(name, w, h, cur, g, nv, E, NULL, NULL);
            next[0] = '\0';

            for (d = def; *d;) {    /* every adjustment as it is now */
                d = pv_next(d, s, sizeof(s));

                if (s[0] == 'a' && s[1] == ' ' && sscanf(s + 2, "%39s", nm) == 1 && n < sizeof(next) - 64) {
                    n += (size_t)snprintf(next + n, sizeof(next) - n, "%s%s=%.0f", n ? " " : "", nm, pv_arg(E, nm));
                }
            }

            snprintf(cur, sizeof(cur), "%s", next);
        }

        if (!H[handle].g1[0] || !H[handle].g2[0]) {
            break;     /* one adjustment: once is enough */
        }
    }

    pv_printf(&S, "%s", cur);
    (void)first;

    if (buf && cap) {
        buf[S.len < cap ? S.len : cap - 1] = '\0';
    }

    free(E);
    return S.len;
}
