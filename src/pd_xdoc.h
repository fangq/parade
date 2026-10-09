/* a small DOM over the markup tokenizer (pd_xdoc.c): for the readers of Office packages' parts */
#ifndef PD_XDOC_H
#define PD_XDOC_H

#include <stddef.h>

#include "pd_conv.h"

typedef struct {
    char name[48];              /* local name */
    size_t a, b;                /* the element: its '<' to after its end */
    size_t ia, ib;              /* its inside */
    const char* attrs;
    size_t alen;
    int parent, kid, next;
} xnode;

typedef struct {
    char* s;                    /* owned */
    size_t n;
    xnode* v;
    int nv, cap;
} xdoc;

void xd_free(xdoc* d);
/* parsed from owned text (taken); 0 when it cannot be */
int xd_parse(xdoc* d, char* s, size_t n);
/* a child by local name, -1 none */
int xd_kid(const xdoc* d, int n, const char* name);
/* a descendant by a path of local names ("spPr/xfrm") */
int xd_path(const xdoc* d, int n, const char* path);
/* the first element of a name anywhere in the document */
int xd_find(const xdoc* d, const char* name);
int xd_attr(const xdoc* d, int n, const char* name, char* buf, size_t cap);
long long xd_int(const xdoc* d, int n, const char* name, long long def);
/* an element's whole text, or its inside */
void xd_raw(pd_buf* o, const xdoc* d, int n);
void xd_inner(pd_buf* o, const xdoc* d, int n);

#endif
