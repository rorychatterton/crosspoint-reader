#include "ml_h2_stream.h"

#include <string.h>

void ml_h2_stream_init(ml_h2_stream_t *stream) {
    memset(stream, 0, sizeof(*stream));
}

static bool begin_frame(ml_h2_stream_t *stream) {
    stream->remaining = ((uint32_t)stream->header[0] << 16) |
                        ((uint32_t)stream->header[1] << 8) | stream->header[2];
    stream->type = stream->header[3];
    stream->flags = stream->header[4];
    stream->stream_id = ((uint32_t)(stream->header[5] & 0x7f) << 24) |
                        ((uint32_t)stream->header[6] << 16) |
                        ((uint32_t)stream->header[7] << 8) | stream->header[8];
    stream->data_remaining = stream->remaining;
    stream->padding_remaining = 0;
    stream->padding_read = (stream->flags & 0x08) == 0;
    if (!stream->padding_read && stream->remaining == 0) return false;
    return true;
}

bool ml_h2_stream_feed(ml_h2_stream_t *stream, const uint8_t *data, size_t len,
                       uint32_t wanted_stream_id, ml_h2_data_cb_t callback, void *ctx) {
    if (!stream || (!data && len != 0) || stream->error) return false;
    size_t pos = 0;
    while (pos < len) {
        if (stream->header_used < sizeof(stream->header)) {
            const size_t need = sizeof(stream->header) - stream->header_used;
            const size_t take = len - pos < need ? len - pos : need;
            memcpy(stream->header + stream->header_used, data + pos, take);
            stream->header_used += take;
            pos += take;
            if (stream->header_used < sizeof(stream->header)) continue;
            if (!begin_frame(stream)) {
                stream->error = true;
                return false;
            }
            if (stream->remaining == 0) {
                if ((stream->type == 0 || stream->type == 1) && stream->stream_id == wanted_stream_id &&
                    (stream->flags & 0x01)) stream->end_stream = true;
                stream->header_used = 0;
            }
            continue;
        }

        if (!stream->padding_read) {
            stream->padding_remaining = data[pos++];
            --stream->remaining;
            if (stream->padding_remaining > stream->remaining) {
                stream->error = true;
                return false;
            }
            stream->data_remaining = stream->remaining - stream->padding_remaining;
            stream->padding_read = true;
            continue;
        }

        size_t take = len - pos;
        if (take > stream->remaining) take = stream->remaining;
        if (stream->type == 0 && stream->stream_id == wanted_stream_id && stream->data_remaining > 0) {
            size_t emit = take;
            if (emit > stream->data_remaining) emit = stream->data_remaining;
            if (emit > 0 && callback && !callback(ctx, data + pos, emit)) {
                stream->error = true;
                return false;
            }
            stream->data_remaining -= emit;
        }
        stream->remaining -= take;
        pos += take;
        if (stream->remaining == 0) {
            if ((stream->type == 0 || stream->type == 1) && stream->stream_id == wanted_stream_id &&
                (stream->flags & 0x01)) stream->end_stream = true;
            stream->header_used = 0;
        }
    }
    return true;
}
