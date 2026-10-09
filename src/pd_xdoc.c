/*
 * A small DOM over the markup tokenizer: the elements of a part as a tree (local names, their attributes and
 * where they are in the text), for the readers of Office packages' parts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pd_xdoc.h"

void xd_free(xdoc* d) {
    free(d->s);
    free(d->v);
    memset(d, 0, sizeof(*d));
}

/* parsed from owned text (taken); 0 when it cannot be */
int xd_parse(xdoc* d, char* s, size_t n) {
    pd_markup m;
    int stack[256], depth = 0, last[256];
    size_t at;

    memset(d, 0, sizeof(*d));
    d->s = s;
    d->n = n;

    if (!s) {
        return 0;
    }

    mu_init(&m, s, n, 0);

    for (;;) {
        at = m.pos;

        if (mu_next(&m) == MT_END) {
            break;
        }

        if (m.type == MT_OPEN || m.type == MT_EMPTY) {
            xnode* x;
            int k;

            if (d->nv >= d->cap) {
                int nc = d->cap ? d->cap * 2 : 256;
                xnode* t = (xnode*)realloc(d->v, sizeof(xnode) * (size_t)nc);

                if (!t) {
                    return 0;
                }

                d->v = t;
                d->cap = nc;
            }

            k = d->nv++;
            x = &d->v[k];
            memset(x, 0, sizeof(*x));
            snprintf(x->name, sizeof(x->name), "%s", mu_local(m.name));
            x->a = at;
            x->ia = x->ib = x->b = m.pos;
            x->attrs = m.attrs;
            x->alen = m.alen;
            x->parent = depth ? stack[depth - 1] : -1;
            x->kid = x->next = -1;

            if (depth) {    /* the last child of its parent: this one after it */
                int p = stack[depth - 1];

                if (last[depth - 1] < 0) {
                    d->v[p].kid = k;
                } else {
                    d->v[last[depth - 1]].next = k;
                }

                last[depth - 1] = k;
            }

            if (m.type == MT_OPEN && depth < 255) {
                stack[depth] = k;
                last[depth] = -1;
                depth++;
            }
        } else if (m.type == MT_CLOSE && depth) {
            xnode* x = &d->v[stack[--depth]];

            x->ib = at;
            x->b = m.pos;
        }
    }

    return d->nv > 0;
}

int xd_kid(const xdoc* d, int n, const char* name) {
    int k;

    if (n < 0) {
        return -1;
    }

    for (k = d->v[n].kid; k >= 0; k = d->v[k].next) {
        if (!strcmp(d->v[k].name, name)) {
            return k;
        }
    }

    return -1;
}

/* a descendant by a path of local names ("spPr/xfrm") */
int xd_path(const xdoc* d, int n, const char* path) {
    char part[48];
    const char* p = path;

    while (n >= 0 && *p) {
        size_t k = 0;

        while (*p && *p != '/' && k < sizeof(part) - 1) {
            part[k++] = *p++;
        }

        part[k] = '\0';

        if (*p == '/') {
            p++;
        }

        n = xd_kid(d, n, part);
    }

    return n;
}

/* the first element of a name anywhere in the document */
int xd_find(const xdoc* d, const char* name) {
    int k;

    for (k = 0; k < d->nv; k++) {
        if (!strcmp(d->v[k].name, name)) {
            return k;
        }
    }

    return -1;
}

int xd_attr(const xdoc* d, int n, const char* name, char* buf, size_t cap) {
    pd_markup m;

    buf[0] = '\0';

    if (n < 0) {
        return 0;
    }

    memset(&m, 0, sizeof(m));
    m.attrs = d->v[n].attrs;
    m.alen = d->v[n].alen;
    return mu_attr(&m, name, buf, cap);
}

long long xd_int(const xdoc* d, int n, const char* name, long long def) {
    char v[40];

    return xd_attr(d, n, name, v, sizeof(v)) && v[0] ? atoll(v) : def;
}

/* an element's whole text, or its inside */
void xd_raw(pd_buf* o, const xdoc* d, int n) {
    pb_put(o, d->s + d->v[n].a, d->v[n].b - d->v[n].a);
}

void xd_inner(pd_buf* o, const xdoc* d, int n) {
    pb_put(o, d->s + d->v[n].ia, d->v[n].ib - d->v[n].ia);
}
