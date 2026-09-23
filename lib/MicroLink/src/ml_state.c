#include "microlink.h"

const char *microlink_state_name(microlink_state_t state) {
    switch (state) {
        case ML_STATE_IDLE: return "idle";
        case ML_STATE_WIFI_WAIT: return "waiting for Wi-Fi";
        case ML_STATE_CONNECTING: return "connecting to control plane";
        case ML_STATE_REGISTERING: return "registering/fetching peer map";
        case ML_STATE_CONNECTED: return "connected";
        case ML_STATE_RECONNECTING: return "reconnecting to control plane";
        case ML_STATE_ERROR: return "client error";
        default: return "invalid state";
    }
}

bool microlink_state_is_terminal_error(microlink_state_t state) {
    return state == ML_STATE_ERROR;
}
