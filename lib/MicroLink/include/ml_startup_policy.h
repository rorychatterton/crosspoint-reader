#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ml_has_target(uint32_t priority_peer_ip, const char *priority_peer_name);

#ifdef __cplusplus
}
#endif
