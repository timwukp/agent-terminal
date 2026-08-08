/* layout.h — binary split tree mapping panes to rectangles.
 *
 * A fixed node array (2*MAX_PANES-1 nodes suffices for any full binary tree
 * with MAX_PANES leaves), no allocation, correct reflow on resize — which is
 * what tmux is underneath its named presets. Leaves hold a pane index;
 * internal nodes hold a split. The field is named `stacked`, not vertical/
 * horizontal: tmux's split-window -h makes a *vertical divider* with panes
 * side by side, and every implementation gets the axis words backwards once.
 * stacked = true → children are one above the other. */
#ifndef AT_LAYOUT_H
#define AT_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

/* The leaf capacity. session.h defines MAX_PANES_PER_SESSION from this, not
 * the other way around, so this header stays free of the session type (the
 * session embeds a layout by value). */
#define LAYOUT_MAX_LEAVES 6
#define LAYOUT_NODES (2 * LAYOUT_MAX_LEAVES - 1)
#define PANE_MIN_COLS 20
#define PANE_MIN_ROWS 3

typedef struct {
    bool in_use;
    bool leaf;
    /* leaf */
    int8_t pane_idx;        /* index into session->panes */
    /* internal */
    bool stacked;           /* true: top/bottom; false: side-by-side */
    int8_t child[2];        /* node indices */
    /* both: the node's rectangle, derived by layout_reflow */
    uint16_t x, y, cols, rows;
} layout_node;

typedef struct {
    layout_node nodes[LAYOUT_NODES];
    int8_t root;            /* -1 = empty */
} layout;

void layout_init(layout *lt, int8_t pane_idx); /* single full-view leaf */

/* Split the leaf holding `pane_idx` into two; the new leaf gets
 * `new_pane_idx` and the lower/right half. Returns false (tree unchanged)
 * if the resulting rectangles would violate PANE_MIN_* at the current
 * geometry, or if no node is free. */
bool layout_split(layout *lt, uint16_t view_cols, uint16_t view_rows,
                  int8_t pane_idx, int8_t new_pane_idx, bool stacked);

/* Remove the leaf holding `pane_idx`; its sibling subtree absorbs the
 * parent's rectangle. Returns false if it is the last leaf. */
bool layout_close(layout *lt, uint16_t view_cols, uint16_t view_rows,
                  int8_t pane_idx);

/* Recompute every rectangle for the given view. Never destroys a pane: on a
 * shrink the tree cannot satisfy, rectangles clamp to >= 1x1 and still
 * render — destroying a pane on resize is data loss. */
void layout_reflow(layout *lt, uint16_t view_cols, uint16_t view_rows);

/* Rectangle of the leaf holding `pane_idx`; false if absent. */
bool layout_pane_rect(const layout *lt, int8_t pane_idx, uint16_t *x,
                      uint16_t *y, uint16_t *cols, uint16_t *rows);

int layout_leaf_count(const layout *lt);

#endif
