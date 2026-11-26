/* handsmark.c — the nodes an agent's screenshot paints black (see handsmark.h) */
#include "handsmark.h"

static int mark(Node *n, int *marked, int max, int *k) {
    if (n->protect) return 0;                       /* already the protected fill: nothing to undo later */
    if (*k >= max) return -1;
    n->protect = 1;
    marked[(*k)++] = n->id;
    return 0;
}

static int walk(Node *n, int depth, int *marked, int max, int *k) {
    if (!n) return 0;
    if (depth > 4096) return -1;                    /* deeper than any real tree: refused */
    if (n->hands_deny) return mark(n, marked, max, k);
    for (Node *c = n->first; c; c = c->next) if (walk(c, depth + 1, marked, max, k)) return -1;
    return 0;
}

void hands_unmark_nodes(Node *(*get)(int id), const int *marked, int n) {
    for (int i = 0; i < n; i++) { Node *x = get(marked[i]); if (x) x->protect = 0; }
}

int hands_mark_nodes(Node *root, Node *(*get)(int id), const int *black, int nblack, int *marked, int max, int *n) {
    int k = 0, bad = 0;
    for (Node *l = root ? root->first : NULL; l && !bad; l = l->next) {
        if (l->s.z_index >= HANDS_SYSTEM_Z) {
            for (Node *c = l->first; c && !bad; c = c->next) bad = mark(c, marked, max, &k);
        } else bad = walk(l, 0, marked, max, &k);
    }
    for (int i = 0; i < nblack && !bad; i++) {
        Node *b = get(black[i]);
        bad = b ? mark(b, marked, max, &k) : -1;
    }
    if (bad) { hands_unmark_nodes(get, marked, k); *n = 0; return -1; }
    *n = k;
    return 0;
}
