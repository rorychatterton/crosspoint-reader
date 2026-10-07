#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ml_has_target(uint32_t priority_peer_ip, const char *priority_peer_name);

/* microlink_wait_peers_ready() pacing. A tailscaled peer configures
 * wireguard-go peers lazily: it drops the first initiation from an idle peer
 * and initiates itself, so a short wait needs re-initiations inside its
 * budget, and a handshake answered as responder needs one more relayed round
 * trip (the initiator's first packet) before the session can send. */
#define ML_PEER_WAIT_POLL_MS 500
#define ML_PEER_WAIT_CONFIRM_GRACE_MS 3000

/* Interval between re-initiations: a third of the budget on the poll grid,
 * at least 1.5 s (a relayed round trip) and at most WireGuard's 5 s
 * REKEY_TIMEOUT. */
uint32_t ml_peer_wait_retrigger_ms(uint32_t max_wait_ms);

/* Budget after the deadline is reached: extended once by the grace period
 * while an answered handshake waits for confirmation, else unchanged. */
uint32_t ml_peer_wait_budget_ms(uint32_t max_wait_ms, bool confirm_pending, bool already_extended);

#ifdef __cplusplus
}
#endif
