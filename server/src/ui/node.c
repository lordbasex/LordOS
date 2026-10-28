/* node.c — tree of native nodes that React creates and modifies. */
#include "ui.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

Ui ui;

void style_defaults(Style *s) {
    memset(s, 0, sizeof *s);
    s->align_items = ALIGN_STRETCH;
    s->object_fit = FIT_COVER;
    s->opacity = 1;
    s->color = 0xff000000;
    s->font_size = 14;
    s->font_weight = 400;
    s->grad_angle = 180;
    s->text_align = TEXT_START;
}

Node *node_new(int type) {
    int id = 1;
    while (id < ui.cap && ui.nodes[id]) id++;
    if (id >= ui.cap) {
        int cap = ui.cap ? ui.cap * 2 : 1024;
        Node **n = realloc(ui.nodes, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        memset(n + ui.cap, 0, (size_t)(cap - ui.cap) * sizeof *n);
        ui.nodes = n;
        ui.cap = cap;
    }
    Node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->id = id;
    n->type = type;
    style_defaults(&n->s);
    ui.nodes[id] = n;
    ui.dirty = 1;
    return n;
}

Node *node_get(int id) { return id > 0 && id < ui.cap ? ui.nodes[id] : NULL; }

int node_count(void) {
    int n = 0;
    for (int i = 0; i < ui.cap; i++) n += ui.nodes[i] != NULL;
    return n;
}

float box_area(Box b) { return b.x1 > b.x0 && b.y1 > b.y0 ? (b.x1 - b.x0) * (b.y1 - b.y0) : 0; }
static int dbox_meets(Box a, Box b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
static Box dbox_join(Box a, Box b) { return (Box){ fminf(a.x0, b.x0), fminf(a.y0, b.y0), fmaxf(a.x1, b.x1), fmaxf(a.y1, b.y1) }; }

/* Adds b to the list: it swallows every rectangle it overlaps (and grows by them, so the list never
 * holds two that overlap); past DAMAGE_MAX the two whose union adds the least area are joined. */
void dlist_add(DamageList *l, Box b) {
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    for (int again = 1; again;) {
        again = 0;
        for (int i = 0; i < l->n; i++) {
            if (!dbox_meets(l->r[i], b)) continue;
            b = dbox_join(b, l->r[i]);
            l->r[i--] = l->r[--l->n];
            again = 1;
        }
    }
    if (l->n == DAMAGE_MAX) {
        int bi = 0, bj = 1;
        float best = 1e30f;
        for (int i = 0; i < l->n; i++)
            for (int j = i + 1; j < l->n; j++) {
                float w = box_area(dbox_join(l->r[i], l->r[j])) - box_area(l->r[i]) - box_area(l->r[j]);
                if (w < best) { best = w; bi = i; bj = j; }
            }
        Box u = dbox_join(l->r[bi], l->r[bj]);
        l->r[bj] = l->r[--l->n];
        l->r[bi] = l->r[--l->n];
        dlist_add(l, u);                /* the union may overlap a third one */
    }
    l->r[l->n++] = b;
}

void damage_add(Box b) {
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    Box *d = &ui.damage;
    /* ui.damage set or cleared directly (a full repaint, the end of a frame): the list starts from it */
    if (d->x0 != ui.dlast.x0 || d->y0 != ui.dlast.y0 || d->x1 != ui.dlast.x1 || d->y1 != ui.dlast.y1) {
        ui.dlist.n = 0;
        dlist_add(&ui.dlist, *d);
    }
    if (d->x1 <= d->x0 || d->y1 <= d->y0) *d = b;
    else {
        if (b.x0 < d->x0) d->x0 = b.x0;
        if (b.y0 < d->y0) d->y0 = b.y0;
        if (b.x1 > d->x1) d->x1 = b.x1;
        if (b.y1 > d->y1) d->y1 = b.y1;
    }
    dlist_add(&ui.dlist, b);
    ui.dlast = *d;
}

/* A node that leaves the tree stops being visible: its previous area must be repainted. */
void damage_subtree(Node *n) {
    damage_add(n->prev_box);
    n->prev_box = (Box){0, 0, 0, 0};
    for (Node *c = n->first; c; c = c->next) damage_subtree(c);
}

static void invalidate_measure(Node *n) {
    for (; n; n = n->parent) n->measure_valid = n->layout_ok = 0;
    ui.dirty = 1;
}

/* Only its insets changed (grab.c): its parent places it again, its own subtree is kept. */
void node_moved(Node *n) {
    for (Node *p = n->parent; p; p = p->parent) p->layout_ok = 0;
    ui.dirty = 1;
}

void node_invalidate(Node *n) {
    n->paint_dirty = 1;
    n->gen++;
    if (n->s.ext) n->s.ext->r.ok = 0;
    if (n->type == NODE_RAW && n->parent) n->parent->paint_dirty = 1;
    invalidate_measure(n);
}

void node_restyled(Node *n, const Style *before, const StyleExt *ext_before) {
    /* the same value (e.g. React confirms the position left by a drag in C): nothing to repaint */
    int ext_changed = n->s.ext && (!before->ext || memcmp(ext_before, n->s.ext, STYLE_EXT_VALUES));
    if (memcmp(before, &n->s, sizeof *before) || ext_changed) {
        /* only the insets of an absolutely positioned node changed (a dot sliding along the Monitor's
         * map, the map image while panning): its size and content are the same, so it's only placed
         * again, like a window being dragged (no remeasuring of every ancestor, no repaint of its
         * content; the old and new boxes are damaged by the paint pass) */
        Style moved = *before;
        memcpy(moved.pos, n->s.pos, sizeof moved.pos);
        if (!ext_changed && n->s.absolute && before->absolute && !memcmp(&moved, &n->s, sizeof moved)) node_moved(n);
        else node_invalidate(n);
    }
}

void node_detach(Node *c) {
    Node *p = c->parent;
    if (!p) return;
    damage_subtree(c);
    if (c->prev) c->prev->next = c->next; else p->first = c->next;
    if (c->next) c->next->prev = c->prev; else p->last = c->prev;
    c->parent = c->prev = c->next = NULL;
    invalidate_measure(p);
}

void node_append(Node *p, Node *c) {
    node_detach(c);
    c->parent = p;
    c->prev = p->last;
    if (p->last) p->last->next = c; else p->first = c;
    p->last = c;
    invalidate_measure(p);
}

void node_insert_before(Node *p, Node *c, Node *before) {
    if (!before || before->parent != p) { node_append(p, c); return; }
    node_detach(c);
    c->parent = p;
    c->next = before;
    c->prev = before->prev;
    if (before->prev) before->prev->next = c; else p->first = c;
    before->prev = c;
    invalidate_measure(p);
}

/* Destroys the node and the children still hanging from it. */
void node_destroy(Node *n) {
    if (!n) return;
    node_detach(n);
    while (n->first) node_destroy(n->first);
    if (ui.root == n) ui.root = NULL;
    ui.nodes[n->id] = NULL;
    image_release(n->img);
    free(n->text);
    free(n->tf);
    free(n->s.ext);
    free(n);
    ui.dirty = 1;
}

void node_set_text(Node *n, const char *s) {
    free(n->text);
    n->text = strdup(s ? s : "");
    node_invalidate(n);
}

/* Concatenates the text of the descendant RAW nodes (<Text>hello {name}</Text>). */
static int gather(const Node *n, char *buf, int size, int len) {
    for (const Node *c = n->first; c; c = c->next) {
        if (c->type == NODE_RAW && c->text) {
            int l = (int)strlen(c->text);
            if (len + l >= size) l = size - 1 - len;
            if (l > 0) { memcpy(buf + len, c->text, (size_t)l); len += l; }
        } else if (c->type == NODE_TEXT) {
            len = gather(c, buf, size, len);
        }
    }
    buf[len] = 0;
    return len;
}

int node_gather_text(const Node *n, char *buf, int size) {
    buf[0] = 0;
    return gather(n, buf, size, 0);
}

/* Children in paint order: tree order, stable by zIndex. */
int node_children_ordered(Node *n, Node **out, int max) {
    int k = 0, sorted = 1;
    for (Node *c = n->first; c && k < max; c = c->next) {
        out[k++] = c;
        if (c->s.z_index != 0) sorted = 0;
    }
    if (!sorted)
        for (int i = 1; i < k; i++) {
            Node *v = out[i];
            int j = i - 1;
            while (j >= 0 && out[j]->s.z_index > v->s.z_index) { out[j + 1] = out[j]; j--; }
            out[j + 1] = v;
        }
    return k;
}

/* Deletes all nodes (theme change). The whole screen is left to repaint. */
void ui_reset(void) {
    for (int i = 0; i < ui.cap; i++)
        if (ui.nodes[i]) { free(ui.nodes[i]->text); free(ui.nodes[i]->tf); free(ui.nodes[i]->s.ext); free(ui.nodes[i]); ui.nodes[i] = NULL; }
    ui.root = NULL;
    ui.damage = (Box){0, 0, ui.width, ui.height};
    ui.dirty = 1;
}
