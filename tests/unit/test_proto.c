/* test_proto.c — frame codec: round-trips, partial reads, violations. */
#include "runner.h"

#include "common/proto.h"
#include "common/ring.h"

static uint8_t scratch[PROTO_MAX_PAYLOAD];

TEST(roundtrip_basic) {
    ring r;
    ring_init(&r, 64, 0);
    const char *msg = "hello world";
    ASSERT_TRUE(proto_write_frame(&r, MSG_STDIN_DATA, msg, strlen(msg)));
    uint8_t type = 0;
    size_t len = 0;
    ASSERT_EQ_INT(proto_read_frame(&r, &type, scratch, &len), 1);
    ASSERT_EQ_INT(type, MSG_STDIN_DATA);
    ASSERT_EQ_INT(len, strlen(msg));
    ASSERT_EQ_MEM(scratch, msg, len);
    ASSERT_EQ_INT(ring_len(&r), 0);
    ring_free(&r);
}

TEST(roundtrip_empty_payload) {
    ring r;
    ring_init(&r, 16, 0);
    ASSERT_TRUE(proto_write_frame(&r, MSG_DETACH, NULL, 0));
    uint8_t type = 0;
    size_t len = 99;
    ASSERT_EQ_INT(proto_read_frame(&r, &type, scratch, &len), 1);
    ASSERT_EQ_INT(type, MSG_DETACH);
    ASSERT_EQ_INT(len, 0);
    ring_free(&r);
}

TEST(partial_frame_needs_more) {
    ring r;
    ring_init(&r, 64, 0);
    uint8_t hdr[5] = {10, 0, 0, 0, MSG_OUTPUT}; /* claims 10 bytes, deliver 3 */
    ring_write(&r, hdr, 5);
    ring_write(&r, "abc", 3);
    uint8_t type;
    size_t len;
    ASSERT_EQ_INT(proto_read_frame(&r, &type, scratch, &len), 0);
    /* nothing consumed while incomplete */
    ASSERT_EQ_INT(ring_len(&r), 8);
    ring_write(&r, "defghij", 7);
    ASSERT_EQ_INT(proto_read_frame(&r, &type, scratch, &len), 1);
    ASSERT_EQ_INT(len, 10);
    ASSERT_EQ_MEM(scratch, "abcdefghij", 10);
    ring_free(&r);
}

TEST(oversized_frame_rejected) {
    ring r;
    ring_init(&r, 64, 0);
    uint8_t hdr[5];
    put_u32(hdr, PROTO_MAX_PAYLOAD + 1);
    hdr[4] = MSG_OUTPUT;
    ring_write(&r, hdr, 5);
    uint8_t type;
    size_t len;
    ASSERT_EQ_INT(proto_read_frame(&r, &type, scratch, &len), -1);
    ring_free(&r);
}

TEST(write_respects_ceiling) {
    ring r;
    ring_init(&r, 16, 32); /* tiny ceiling */
    uint8_t big[64] = {0};
    ASSERT_TRUE(!proto_write_frame(&r, MSG_OUTPUT, big, sizeof big));
    ASSERT_EQ_INT(ring_len(&r), 0); /* failure leaves ring untouched */
    ring_free(&r);
}

TEST(many_frames_stream) {
    ring r;
    ring_init(&r, 16, 0);
    for (int i = 0; i < 1000; i++) {
        uint8_t payload[8];
        put_u64(payload, (uint64_t)i);
        ASSERT_TRUE(proto_write_frame(&r, MSG_PING, payload, 8));
    }
    for (int i = 0; i < 1000; i++) {
        uint8_t type;
        size_t len;
        ASSERT_EQ_INT(proto_read_frame(&r, &type, scratch, &len), 1);
        ASSERT_EQ_INT(type, MSG_PING);
        ASSERT_EQ_INT((long long)get_u64(scratch), i);
    }
    ring_free(&r);
}

TEST(scalar_helpers) {
    uint8_t b[8];
    put_u16(b, 0xBEEF);
    ASSERT_EQ_INT(get_u16(b), 0xBEEF);
    ASSERT_EQ_INT(b[0], 0xEF); /* little-endian on the wire */
    put_u32(b, 0xDEADBEEF);
    ASSERT_EQ_INT((long long)get_u32(b), (long long)0xDEADBEEFu);
    put_u64(b, 0x0123456789ABCDEFull);
    ASSERT_EQ_INT((long long)get_u64(b), (long long)0x0123456789ABCDEFull);
}

int main(void) {
    RUN(roundtrip_basic);
    RUN(roundtrip_empty_payload);
    RUN(partial_frame_needs_more);
    RUN(oversized_frame_rejected);
    RUN(write_respects_ceiling);
    RUN(many_frames_stream);
    RUN(scalar_helpers);
    TEST_MAIN_END();
}
