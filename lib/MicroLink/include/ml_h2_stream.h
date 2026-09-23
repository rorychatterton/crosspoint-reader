#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*ml_h2_data_cb_t)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    uint8_t header[9];
    uint8_t header_used;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
    uint32_t remaining;
    uint32_t data_remaining;
    uint8_t padding_remaining;
    bool padding_read;
    bool end_stream;
    bool error;
} ml_h2_stream_t;

void ml_h2_stream_init(ml_h2_stream_t *stream);
bool ml_h2_stream_feed(ml_h2_stream_t *stream, const uint8_t *data, size_t len,
                       uint32_t wanted_stream_id, ml_h2_data_cb_t callback, void *ctx);

#ifdef __cplusplus
}
#endif
