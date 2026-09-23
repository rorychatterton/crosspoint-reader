#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ML_MAP_FILTER_MAX_ENDPOINTS 8

typedef struct {
    uint32_t ip;
    uint16_t port;
} ml_map_filter_endpoint_t;

typedef struct {
    uint32_t vpn_ip;
    uint16_t derp_region;
    char hostname[64];
    char node_key[80];
    char disco_key[80];
    ml_map_filter_endpoint_t endpoints[ML_MAP_FILTER_MAX_ENDPOINTS];
    uint8_t endpoint_count;
    /* Prefix from the peer's AllowedIPs that contains the target IP when the
     * target is reached through an advertised route (subnet router, VIP
     * service) rather than the peer's own address. route_bits == 0 when the
     * peer matched on its own address or name. */
    uint32_t route_ip;
    uint8_t route_bits;
    bool found;
} ml_map_filter_peer_t;

typedef struct {
    uint32_t self_ip;
    uint16_t self_derp_region;
    ml_map_filter_peer_t peer;
    /* Optional resolver peer requested through secondary_ip. */
    ml_map_filter_peer_t secondary;
} ml_map_filter_result_t;

typedef struct ml_map_filter_s ml_map_filter_t;

/* target_ip/target_name select the peer the session is for; secondary_ip
 * (0 = none) additionally selects the peer owning that address. The stream
 * is complete when the target and, if requested, the secondary are found. */
ml_map_filter_t *ml_map_filter_create(uint32_t target_ip, const char *target_name, uint32_t secondary_ip);
bool ml_map_filter_feed(ml_map_filter_t *filter, const uint8_t *data, size_t len);
bool ml_map_filter_finish(ml_map_filter_t *filter, ml_map_filter_result_t *result);
void ml_map_filter_destroy(ml_map_filter_t *filter);
size_t ml_map_filter_allocation_size(void);

#ifdef __cplusplus
}
#endif
