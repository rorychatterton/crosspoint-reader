#include "ml_startup_policy.h"

bool ml_has_target(uint32_t priority_peer_ip, const char *priority_peer_name) {
    return priority_peer_ip != 0 || (priority_peer_name && priority_peer_name[0]);
}

uint32_t ml_peer_wait_retrigger_ms(uint32_t max_wait_ms) {
    const uint32_t min_ms = 1500;
    const uint32_t max_ms = 5000;
    uint32_t interval = max_wait_ms / 3;
    interval -= interval % ML_PEER_WAIT_POLL_MS;
    if (interval < min_ms) return min_ms;
    if (interval > max_ms) return max_ms;
    return interval;
}

uint32_t ml_peer_wait_budget_ms(uint32_t max_wait_ms, bool confirm_pending, bool already_extended) {
    if (!confirm_pending || already_extended) return max_wait_ms;
    return max_wait_ms + ML_PEER_WAIT_CONFIRM_GRACE_MS;
}
