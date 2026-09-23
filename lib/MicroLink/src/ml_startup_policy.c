#include "ml_startup_policy.h"

bool ml_has_target(uint32_t priority_peer_ip, const char *priority_peer_name) {
    return priority_peer_ip != 0 || (priority_peer_name && priority_peer_name[0]);
}
