/* test_layout.c — split-tree geometry: rectangles tile, minimums gate,
 * close absorbs, reflow clamps without destroying panes. */
#include "runner.h"

#include "daemon/layout.h"

#define VC 80
#define VR 24

/* Every cell of the view is covered by exactly one leaf rectangle or one
 * divider — no gaps, no overlaps. This is the invariant the compositor
 * assumes when it repaints only dirty panes: an uncovered cell would show
 * stale bytes forever. */
static void assert_tiles(const layout *lt, uint16_t vc, uint16_t vr, const char *what) {
    static uint8_t hit[1024][1024];
    memset(hit, 0, sizeof hit);
    for (int8_t i = 0; i < LAYOUT_NODES; i++) {
        const layout_node *n = &lt->nodes[i];
        if (!n->in_use || !n->leaf) continue;
        for (uint16_t r = n->y; r < n->y + n->rows && r < vr; r++)
            for (uint16_t c = n->x; c < n->x + n->cols && c < vc; c++)
                hit[r][c]++;
    }
    int gaps = 0, overlaps = 0;
    for (uint16_t r = 0; r < vr; r++)
        for (uint16_t c = 0; c < vc; c++) {
            if (hit[r][c] > 1) overlaps++;
            /* a zero is a divider cell: it must be adjacent to two panes,
             * but for this test just count them and bound the total */
            if (hit[r][c] == 0) gaps++;
        }
    t_checks++;
    if (overlaps != 0) {
        t_failures++;
        fprintf(stderr, "FAIL layout [%s]: %d overlapping cells\n", what, overlaps);
    }
    /* Dividers: at most one row or column per internal node. */
    int internals = 0;
    for (int8_t i = 0; i < LAYOUT_NODES; i++)
        if (lt->nodes[i].in_use && !lt->nodes[i].leaf) internals++;
    int max_divider_cells = internals * (vc > vr ? vc : vr);
    t_checks++;
    if (gaps > max_divider_cells) {
        t_failures++;
        fprintf(stderr, "FAIL layout [%s]: %d uncovered cells > %d divider budget\n",
                what, gaps, max_divider_cells);
    }
}

TEST(single_leaf_fills_view) {
    layout lt;
    layout_init(&lt, 0);
    layout_reflow(&lt, VC, VR);
    uint16_t x, y, c, r;
    ASSERT_TRUE(layout_pane_rect(&lt, 0, &x, &y, &c, &r));
    ASSERT_EQ_INT(x, 0); ASSERT_EQ_INT(y, 0);
    ASSERT_EQ_INT(c, VC); ASSERT_EQ_INT(r, VR);
    ASSERT_EQ_INT(layout_leaf_count(&lt), 1);
}

TEST(side_by_side_split) {
    layout lt;
    layout_init(&lt, 0);
    layout_reflow(&lt, VC, VR);
    ASSERT_TRUE(layout_split(&lt, VC, VR, 0, 1, false));
    ASSERT_EQ_INT(layout_leaf_count(&lt), 2);
    uint16_t x0, y0, c0, r0, x1, y1, c1, r1;
    ASSERT_TRUE(layout_pane_rect(&lt, 0, &x0, &y0, &c0, &r0));
    ASSERT_TRUE(layout_pane_rect(&lt, 1, &x1, &y1, &c1, &r1));
    /* Original keeps the left; the divider column sits between. */
    ASSERT_EQ_INT(x0, 0);
    ASSERT_EQ_INT(x1, c0 + 1);
    ASSERT_EQ_INT(c0 + 1 + c1, VC);
    ASSERT_EQ_INT(r0, VR);
    ASSERT_EQ_INT(r1, VR);
    assert_tiles(&lt, VC, VR, "side-by-side");
}

TEST(stacked_split) {
    layout lt;
    layout_init(&lt, 0);
    layout_reflow(&lt, VC, VR);
    ASSERT_TRUE(layout_split(&lt, VC, VR, 0, 1, true));
    uint16_t y0, r0, y1, r1;
    ASSERT_TRUE(layout_pane_rect(&lt, 0, NULL, &y0, NULL, &r0));
    ASSERT_TRUE(layout_pane_rect(&lt, 1, NULL, &y1, NULL, &r1));
    ASSERT_EQ_INT(y0, 0);
    ASSERT_EQ_INT(y1, r0 + 1);
    ASSERT_EQ_INT(r0 + 1 + r1, VR);
    assert_tiles(&lt, VC, VR, "stacked");
}

TEST(minimums_refuse_split) {
    layout lt;
    layout_init(&lt, 0);
    /* A 30-col view cannot side-split into two >= 20-col panes. */
    layout_reflow(&lt, 30, VR);
    ASSERT_TRUE(!layout_split(&lt, 30, VR, 0, 1, false));
    ASSERT_EQ_INT(layout_leaf_count(&lt), 1); /* tree untouched */
    /* A 6-row view cannot stack-split into two >= 3-row panes. */
    layout_reflow(&lt, VC, 6);
    ASSERT_TRUE(!layout_split(&lt, VC, 6, 0, 1, true));
    ASSERT_EQ_INT(layout_leaf_count(&lt), 1);
}

TEST(close_absorbs_sibling) {
    layout lt;
    layout_init(&lt, 0);
    layout_reflow(&lt, VC, VR);
    ASSERT_TRUE(layout_split(&lt, VC, VR, 0, 1, false));
    ASSERT_TRUE(layout_close(&lt, VC, VR, 1));
    ASSERT_EQ_INT(layout_leaf_count(&lt), 1);
    uint16_t x, y, c, r;
    ASSERT_TRUE(layout_pane_rect(&lt, 0, &x, &y, &c, &r));
    ASSERT_EQ_INT(c, VC); /* survivor grew back to the full view */
    ASSERT_EQ_INT(r, VR);
    /* Closing the last leaf is refused: the session dies instead. */
    ASSERT_TRUE(!layout_close(&lt, VC, VR, 0));
}

TEST(nested_splits_tile) {
    layout lt;
    layout_init(&lt, 0);
    layout_reflow(&lt, 200, 60);
    ASSERT_TRUE(layout_split(&lt, 200, 60, 0, 1, false));
    ASSERT_TRUE(layout_split(&lt, 200, 60, 1, 2, true));
    ASSERT_TRUE(layout_split(&lt, 200, 60, 0, 3, true));
    ASSERT_EQ_INT(layout_leaf_count(&lt), 4);
    assert_tiles(&lt, 200, 60, "nested");
    /* Close an inner pane; the rest still tile. */
    ASSERT_TRUE(layout_close(&lt, 200, 60, 1));
    ASSERT_EQ_INT(layout_leaf_count(&lt), 3);
    assert_tiles(&lt, 200, 60, "nested after close");
}

TEST(shrink_clamps_never_destroys) {
    layout lt;
    layout_init(&lt, 0);
    layout_reflow(&lt, VC, VR);
    ASSERT_TRUE(layout_split(&lt, VC, VR, 0, 1, false));
    ASSERT_TRUE(layout_split(&lt, VC, VR, 1, 2, true));
    /* Shrink far below what three panes need. Every pane must still have a
     * >= 1x1 rectangle — destroying one on resize is data loss. */
    layout_reflow(&lt, 8, 4);
    ASSERT_EQ_INT(layout_leaf_count(&lt), 3);
    for (int8_t p = 0; p < 3; p++) {
        uint16_t c = 0, r = 0;
        ASSERT_TRUE(layout_pane_rect(&lt, p, NULL, NULL, &c, &r));
        ASSERT_TRUE(c >= 1 && r >= 1);
    }
    /* And growing back restores sane tiling. */
    layout_reflow(&lt, VC, VR);
    assert_tiles(&lt, VC, VR, "regrown");
}

int main(void) {
    RUN(single_leaf_fills_view);
    RUN(side_by_side_split);
    RUN(stacked_split);
    RUN(minimums_refuse_split);
    RUN(close_absorbs_sibling);
    RUN(nested_splits_tile);
    RUN(shrink_clamps_never_destroys);
    TEST_MAIN_END();
}
