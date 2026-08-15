#include "proto.h"

#include <string.h>

void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}
void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)(v >> 24);
}
void put_u64(uint8_t *p, uint64_t v) {
    put_u32(p, (uint32_t)(v & 0xffffffffu));
    put_u32(p + 4, (uint32_t)(v >> 32));
}
uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | (uint64_t)get_u32(p + 4) << 32;
}

bool proto_write_frame(ring *out, uint8_t type, const void *payload, size_t len) {
    if (len > PROTO_MAX_PAYLOAD) return false;
    uint8_t hdr[PROTO_HDR_SIZE];
    put_u32(hdr, (uint32_t)len);
    hdr[4] = type;
    /* Two writes must both fit; check combined size against the ceiling
     * first so a failure never leaves a half-written frame. */
    if (out->max_cap && out->len + PROTO_HDR_SIZE + len > out->max_cap) return false;
    ring_write(out, hdr, PROTO_HDR_SIZE);
    ring_write(out, payload, len);
    return true;
}

int proto_read_frame(ring *in, uint8_t *type, uint8_t *scratch, size_t *len) {
    uint8_t hdr[PROTO_HDR_SIZE];
    if (ring_peek(in, hdr, PROTO_HDR_SIZE) < PROTO_HDR_SIZE) return 0;
    uint32_t plen = get_u32(hdr);
    if (plen > PROTO_MAX_PAYLOAD) return -1;
    if (ring_len(in) < PROTO_HDR_SIZE + plen) return 0;
    ring_consume(in, PROTO_HDR_SIZE);
    ring_read(in, scratch, plen);
    *type = hdr[4];
    *len = plen;
    return 1;
}

bool proto_err_text(const uint8_t *payload, size_t len, uint16_t *code,
                    const char **msg, uint16_t *msg_len) {
    if (code) *code = len >= 2 ? get_u16(payload) : 0;
    if (msg) *msg = NULL;
    if (msg_len) *msg_len = 0;
    if (len < 4) return false;
    uint16_t declared = get_u16(payload + 2);
    /* 4u + declared cannot overflow: both operands promote to unsigned int and
     * declared is at most 65535. Comparing against the frame's OWN length is
     * the whole point — a declared length that does not fit is a lie, and the
     * bytes past the frame are not ours to read. */
    if (declared == 0 || 4u + declared > len) return false;
    if (msg) *msg = (const char *)payload + 4;
    if (msg_len) *msg_len = declared;
    return true;
}
