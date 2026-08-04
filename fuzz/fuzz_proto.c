/* fuzz_proto.c — frame decoder must never over-read or hang on garbage. */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "common/proto.h"
#include "common/ring.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ring r;
    ring_init(&r, 64, 0);
    ring_write(&r, data, size);

    static uint8_t scratch[PROTO_MAX_PAYLOAD];
    uint8_t type;
    size_t len;
    /* Drain until "need more" or violation; must terminate. */
    for (int guard = 0; guard < 100000; guard++) {
        int rc = proto_read_frame(&r, &type, scratch, &len);
        if (rc <= 0) break;
    }
    ring_free(&r);
    return 0;
}
