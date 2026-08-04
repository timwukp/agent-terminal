/* test_ring.c — byte ring: wraparound, growth, ceiling, consume. */
#include "runner.h"

#include "common/ring.h"

TEST(basic_write_read) {
    ring r;
    ring_init(&r, 16, 0);
    ASSERT_TRUE(ring_write(&r, "hello", 5));
    char out[8] = {0};
    ASSERT_EQ_INT(ring_read(&r, out, sizeof out), 5);
    ASSERT_EQ_MEM(out, "hello", 5);
    ASSERT_EQ_INT(ring_len(&r), 0);
    ring_free(&r);
}

TEST(wraparound) {
    ring r;
    ring_init(&r, 16, 16); /* fixed size forces wrap */
    char out[16];
    for (int i = 0; i < 100; i++) {
        ASSERT_TRUE(ring_write(&r, "0123456789", 10));
        ASSERT_EQ_INT(ring_read(&r, out, 10), 10);
        ASSERT_EQ_MEM(out, "0123456789", 10);
    }
    ring_free(&r);
}

TEST(growth_preserves_order) {
    ring r;
    ring_init(&r, 16, 0);
    /* Shift head off zero, then force growth across the wrap point. */
    ring_write(&r, "abcdefgh", 8);
    char tmp[4];
    ring_read(&r, tmp, 4); /* head=4 */
    char big[100];
    for (int i = 0; i < 100; i++) big[i] = (char)('A' + i % 26);
    ASSERT_TRUE(ring_write(&r, big, 100));
    char out[104];
    ASSERT_EQ_INT(ring_read(&r, out, sizeof out), 104);
    ASSERT_EQ_MEM(out, "efgh", 4);
    ASSERT_EQ_MEM(out + 4, big, 100);
    ring_free(&r);
}

TEST(ceiling_enforced) {
    ring r;
    ring_init(&r, 16, 64);
    uint8_t data[65];
    ASSERT_TRUE(!ring_write(&r, data, 65));
    ASSERT_TRUE(ring_write(&r, data, 64));
    ASSERT_TRUE(!ring_write(&r, data, 1)); /* full */
    ring_consume(&r, 1);
    ASSERT_TRUE(ring_write(&r, data, 1));
    ring_free(&r);
}

TEST(consume_overrun_is_safe) {
    ring r;
    ring_init(&r, 16, 0);
    ring_write(&r, "abc", 3);
    ring_consume(&r, 1000);
    ASSERT_EQ_INT(ring_len(&r), 0);
    ring_free(&r);
}

TEST(peek_does_not_consume) {
    ring r;
    ring_init(&r, 16, 0);
    ring_write(&r, "xyz", 3);
    char out[3];
    ASSERT_EQ_INT(ring_peek(&r, out, 3), 3);
    ASSERT_EQ_INT(ring_len(&r), 3);
    ASSERT_EQ_MEM(out, "xyz", 3);
    ring_free(&r);
}

int main(void) {
    RUN(basic_write_read);
    RUN(wraparound);
    RUN(growth_preserves_order);
    RUN(ceiling_enforced);
    RUN(consume_overrun_is_safe);
    RUN(peek_does_not_consume);
    TEST_MAIN_END();
}
