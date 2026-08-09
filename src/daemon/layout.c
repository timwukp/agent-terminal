#include "layout.h"

#include <string.h>

static int8_t node_alloc(layout *lt) {
    for (int i = 0; i < LAYOUT_NODES; i++)
        if (!lt->nodes[i].in_use) return (int8_t)i;
    return -1;
}

static int8_t find_leaf(const layout *lt, int8_t pane_idx) {
    for (int i = 0; i < LAYOUT_NODES; i++)
        if (lt->nodes[i].in_use && lt->nodes[i].leaf &&
            lt->nodes[i].pane_idx == pane_idx)
            return (int8_t)i;
    return -1;
}

static int8_t find_parent(const layout *lt, int8_t node) {
    for (int i = 0; i < LAYOUT_NODES; i++) {
        const layout_node *n = &lt->nodes[i];
        if (n->in_use && !n->leaf && (n->child[0] == node || n->child[1] == node))
            return (int8_t)i;
    }
    return -1;
}

void layout_init(layout *lt, int8_t pane_idx) {
    memset(lt, 0, sizeof *lt);
    lt->root = 0;
    lt->nodes[0] = (layout_node){.in_use = true, .leaf = true, .pane_idx = pane_idx};
}

/* Divide a parent rectangle between its two children. The dividing line
 * costs one row/column; child 0 keeps the extra cell on odd sizes so a
 * repeated split drifts predictably. Clamp to >= 1 rather than refuse:
 * reflow must never destroy panes (layout_split is where minimums gate). */
static void divide(const layout_node *parent, layout_node *a, layout_node *b) {
    if (parent->stacked) {
        uint16_t usable = parent->rows > 1 ? (uint16_t)(parent->rows - 1) : 1;
        uint16_t top = (uint16_t)((usable + 1) / 2);
        uint16_t bot = usable > top ? (uint16_t)(usable - top) : 1;
        a->x = parent->x; a->y = parent->y;
        a->cols = parent->cols; a->rows = top ? top : 1;
        b->x = parent->x;
        b->y = (uint16_t)(parent->y + a->rows + 1); /* +1: divider row */
        b->cols = parent->cols; b->rows = bot ? bot : 1;
    } else {
        uint16_t usable = parent->cols > 1 ? (uint16_t)(parent->cols - 1) : 1;
        uint16_t left = (uint16_t)((usable + 1) / 2);
        uint16_t right = usable > left ? (uint16_t)(usable - left) : 1;
        a->x = parent->x; a->y = parent->y;
        a->cols = left ? left : 1; a->rows = parent->rows;
        b->x = (uint16_t)(parent->x + a->cols + 1); /* +1: divider column */
        b->y = parent->y;
        b->cols = right ? right : 1; b->rows = parent->rows;
    }
}

static void reflow_node(layout *lt, int8_t idx) {
    layout_node *n = &lt->nodes[idx];
    if (n->leaf) return;
    layout_node *a = &lt->nodes[n->child[0]];
    layout_node *b = &lt->nodes[n->child[1]];
    divide(n, a, b);
    reflow_node(lt, n->child[0]);
    reflow_node(lt, n->child[1]);
}

void layout_reflow(layout *lt, uint16_t view_cols, uint16_t view_rows) {
    if (lt->root < 0) return;
    layout_node *r = &lt->nodes[lt->root];
    r->x = 0; r->y = 0;
    r->cols = view_cols ? view_cols : 1;
    r->rows = view_rows ? view_rows : 1;
    reflow_node(lt, lt->root);
}

bool layout_split(layout *lt, uint16_t view_cols, uint16_t view_rows,
                  int8_t pane_idx, int8_t new_pane_idx, bool stacked) {
    int8_t leaf = find_leaf(lt, pane_idx);
    if (leaf < 0) return false;

    /* Minimums are checked against CURRENT geometry, before mutating: the
     * refusal must leave the tree untouched. */
    layout_node cur = lt->nodes[leaf];
    if (stacked) {
        if (cur.rows < (uint16_t)(2 * PANE_MIN_ROWS + 1)) return false;
    } else {
        if (cur.cols < (uint16_t)(2 * PANE_MIN_COLS + 1)) return false;
    }

    int8_t a = node_alloc(lt);
    if (a < 0) return false;
    lt->nodes[a].in_use = true; /* reserve before second alloc */
    int8_t b = node_alloc(lt);
    if (b < 0) { lt->nodes[a].in_use = false; return false; }

    /* The leaf becomes the internal node (so the parent's child link stays
     * valid); its pane moves to child a, the new pane to child b. */
    lt->nodes[a] = (layout_node){.in_use = true, .leaf = true, .pane_idx = pane_idx};
    lt->nodes[b] = (layout_node){.in_use = true, .leaf = true, .pane_idx = new_pane_idx};
    layout_node *n = &lt->nodes[leaf];
    n->leaf = false;
    n->pane_idx = -1;
    n->stacked = stacked;
    n->child[0] = a;
    n->child[1] = b;

    layout_reflow(lt, view_cols, view_rows);
    return true;
}

bool layout_close(layout *lt, uint16_t view_cols, uint16_t view_rows,
                  int8_t pane_idx) {
    int8_t leaf = find_leaf(lt, pane_idx);
    if (leaf < 0) return false;
    int8_t parent = find_parent(lt, leaf);
    if (parent < 0) return false; /* last leaf: the session dies instead */

    layout_node *pn = &lt->nodes[parent];
    int8_t sibling = pn->child[0] == leaf ? pn->child[1] : pn->child[0];

    /* The sibling subtree absorbs the parent's rectangle: copy the sibling
     * node over the parent (child links come along), free both old slots. */
    int8_t grand_children[2] = {lt->nodes[sibling].child[0], lt->nodes[sibling].child[1]};
    bool sib_leaf = lt->nodes[sibling].leaf;
    lt->nodes[parent] = lt->nodes[sibling];
    lt->nodes[parent].in_use = true;
    (void)grand_children; (void)sib_leaf; /* links copied verbatim above */
    lt->nodes[leaf].in_use = false;
    lt->nodes[sibling].in_use = false;

    layout_reflow(lt, view_cols, view_rows);
    return true;
}

bool layout_pane_rect(const layout *lt, int8_t pane_idx, uint16_t *x,
                      uint16_t *y, uint16_t *cols, uint16_t *rows) {
    int8_t leaf = find_leaf(lt, pane_idx);
    if (leaf < 0) return false;
    const layout_node *n = &lt->nodes[leaf];
    if (x) *x = n->x;
    if (y) *y = n->y;
    if (cols) *cols = n->cols;
    if (rows) *rows = n->rows;
    return true;
}

int layout_leaf_count(const layout *lt) {
    int n = 0;
    for (int i = 0; i < LAYOUT_NODES; i++)
        if (lt->nodes[i].in_use && lt->nodes[i].leaf) n++;
    return n;
}
