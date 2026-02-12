#include "juice_helper.h"
#include <string.h>

#include "deps/libjuice/src/stun.h"

int juice_stun_write_binding_request(void *buf, size_t size, const uint8_t tid[JUICE_STUN_TID_SIZE]) {
    return stun_write_header(buf, size, STUN_CLASS_REQUEST, STUN_METHOD_BINDING, tid);
}

int juice_stun_parse_binding_response(void *buf, size_t size, 
                                      const uint8_t expected_tid[JUICE_STUN_TID_SIZE], 
                                      juice_mapped_addr_t *out) {
    stun_message_t msg;
    if (stun_read(buf, size, &msg) < 0) {
        return -1;
    }

    if (msg.msg_class != STUN_CLASS_RESP_SUCCESS || msg.msg_method != STUN_METHOD_BINDING) {
        return -1;
    }

    if (memcmp(msg.transaction_id, expected_tid, JUICE_STUN_TID_SIZE) != 0) {
        return -1;
    }

    if (out) {
        memcpy(&out->addr, &msg.mapped.addr, sizeof(struct sockaddr_storage));
        out->len = msg.mapped.len;
    }

    return 0;
}
