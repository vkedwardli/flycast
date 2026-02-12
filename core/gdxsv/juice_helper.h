#pragma once

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JUICE_STUN_TID_SIZE 12

typedef struct {
    struct sockaddr_storage addr;
    socklen_t len;
} juice_mapped_addr_t;

int juice_stun_write_binding_request(void *buf, size_t size, const uint8_t tid[JUICE_STUN_TID_SIZE]);

int juice_stun_parse_binding_response(void *buf, size_t size, 
                                      const uint8_t expected_tid[JUICE_STUN_TID_SIZE], 
                                      juice_mapped_addr_t *out);

#ifdef __cplusplus
}
#endif
