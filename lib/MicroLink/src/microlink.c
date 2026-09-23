/**
 * @file microlink.c
 * @brief MicroLink v2 - Public API and Task Orchestration
 *
 * Creates all FreeRTOS tasks, queues, and event groups.
 * Provides the public API that the application calls.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "microlink_internal.h"
#include "nvs.h"
#include "nvs_flash.h"

#ifdef CONFIG_ML_ENABLE_CELLULAR
#include "ml_cellular.h"
#endif

static const char* TAG = "microlink";

static void* coord_workspace_alloc(size_t size, void* ctx) {
  (void)ctx;
  return ml_psram_malloc(size);
}

static void coord_workspace_free(void* ptr, void* ctx) {
  (void)ctx;
  free(ptr);
}

/* NVS keys */
#define NVS_NAMESPACE "microlink"
#define NVS_KEY_MACHINE_PRI "machine_pri"
#define NVS_KEY_MACHINE_PUB "machine_pub"
#define NVS_KEY_WG_PRI "wg_private"
#define NVS_KEY_WG_PUB "wg_public"
#define NVS_KEY_DISCO_PRI "disco_pri"
#define NVS_KEY_DISCO_PUB "disco_pub"

/* X25519 from x25519.h */
#include "crypto/refc/x25519.h"

/* ============================================================================
 * Key Management (loaded once at init, read-only after)
 * ========================================================================== */

static void generate_keypair(uint8_t* private_key, uint8_t* public_key) {
  esp_fill_random(private_key, 32);
  private_key[0] &= 248;
  private_key[31] &= 127;
  private_key[31] |= 64;
  x25519_base(public_key, private_key, 1);
}

static esp_err_t load_or_generate_keys(microlink_t* ml) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "NVS open failed (%d), generating ephemeral keys", err);
    generate_keypair(ml->machine_private_key, ml->machine_public_key);
    generate_keypair(ml->wg_private_key, ml->wg_public_key);
    generate_keypair(ml->disco_private_key, ml->disco_public_key);
    return ESP_OK;
  }

  size_t key_len = 32;
  bool need_save = false;

  /* Machine key */
  if (nvs_get_blob(nvs, NVS_KEY_MACHINE_PRI, ml->machine_private_key, &key_len) != ESP_OK) {
    generate_keypair(ml->machine_private_key, ml->machine_public_key);
    need_save = true;
    ESP_LOGI(TAG, "Generated new machine key");
  } else {
    key_len = 32;
    nvs_get_blob(nvs, NVS_KEY_MACHINE_PUB, ml->machine_public_key, &key_len);
  }

  /* WireGuard key */
  key_len = 32;
  if (nvs_get_blob(nvs, NVS_KEY_WG_PRI, ml->wg_private_key, &key_len) != ESP_OK) {
    generate_keypair(ml->wg_private_key, ml->wg_public_key);
    need_save = true;
    ESP_LOGI(TAG, "Generated new WireGuard key");
  } else {
    key_len = 32;
    nvs_get_blob(nvs, NVS_KEY_WG_PUB, ml->wg_public_key, &key_len);
  }

  /* DISCO key */
  key_len = 32;
  if (nvs_get_blob(nvs, NVS_KEY_DISCO_PRI, ml->disco_private_key, &key_len) != ESP_OK) {
    generate_keypair(ml->disco_private_key, ml->disco_public_key);
    need_save = true;
    ESP_LOGI(TAG, "Generated new DISCO key");
  } else {
    key_len = 32;
    nvs_get_blob(nvs, NVS_KEY_DISCO_PUB, ml->disco_public_key, &key_len);
  }

  if (need_save) {
    nvs_set_blob(nvs, NVS_KEY_MACHINE_PRI, ml->machine_private_key, 32);
    nvs_set_blob(nvs, NVS_KEY_MACHINE_PUB, ml->machine_public_key, 32);
    nvs_set_blob(nvs, NVS_KEY_WG_PRI, ml->wg_private_key, 32);
    nvs_set_blob(nvs, NVS_KEY_WG_PUB, ml->wg_public_key, 32);
    nvs_set_blob(nvs, NVS_KEY_DISCO_PRI, ml->disco_private_key, 32);
    nvs_set_blob(nvs, NVS_KEY_DISCO_PUB, ml->disco_public_key, 32);
    nvs_commit(nvs);
    ESP_LOGI(TAG, "Keys saved to NVS");
  } else {
    ESP_LOGI(TAG, "Keys loaded from NVS");
  }

  nvs_close(nvs);
  return ESP_OK;
}

/* ============================================================================
 * cJSON PSRAM Hooks
 * ========================================================================== */

static void* cjson_psram_malloc(size_t size) { return ml_psram_malloc(size); }

/* ============================================================================
 * Factory Reset
 * ========================================================================== */

esp_err_t microlink_factory_reset(void) {
  esp_err_t err;

  /* Erase key namespace */
  nvs_handle_t nvs;
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
  if (err == ESP_OK) {
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Factory reset: keys erased");
  }

  /* Erase cached peers */
  ml_peer_nvs_init();
  ml_peer_nvs_clear();
  ml_peer_nvs_deinit();

  ESP_LOGI(TAG, "Factory reset complete");
  return ESP_OK;
}

/* ============================================================================
 * Public API
 * ========================================================================== */

microlink_t* microlink_init(const microlink_config_t* config) {
  if (!config || !config->auth_key) {
    ESP_LOGE(TAG, "Invalid config: auth_key required");
    return NULL;
  }

  /* Route cJSON to PSRAM */
  cJSON_Hooks hooks = {.malloc_fn = cjson_psram_malloc, .free_fn = free};
  cJSON_InitHooks(&hooks);

  /* Allocate context from PSRAM */
  microlink_t* ml = ml_psram_calloc(1, sizeof(microlink_t));
  if (!ml) {
    ESP_LOGE(TAG, "Failed to allocate context");
    return NULL;
  }

  /* Copy config */
  ml->config = *config;
  if (config->ctrl_server_public_key) {
    memcpy(ml->ctrl_server_public_key, config->ctrl_server_public_key, sizeof(ml->ctrl_server_public_key));
    ml->config.ctrl_server_public_key = ml->ctrl_server_public_key;
  }
  if (ml->config.max_peers == 0) ml->config.max_peers = ML_MAX_PEERS;
  if (ml->config.max_peers > ML_MAX_PEERS) ml->config.max_peers = ML_MAX_PEERS;
  ml->config.enable_derp = true; /* Always need DERP for relay */

  /* Warm start needs the target by address (the NVS table is keyed by it)
   * and our cached VPN IP; anything less is a cold start. STUN is skipped:
   * there is no control connection to report endpoints to. */
  if (ml->config.warm_start && (ml->config.priority_peer_ip == 0 || ml->config.warm_self_ip == 0)) {
    ESP_LOGE(TAG, "Warm start needs priority_peer_ip and warm_self_ip; starting cold");
    ml->config.warm_start = false;
  }
  if (ml->config.warm_start) ml->config.enable_stun = false;

  ml->state = ML_STATE_IDLE;
  ml->coord_sock = -1;
  ml->disco_sock4 = -1;
  ml->disco_sock6 = -1;
  ml->stun_sock = -1;
  ml->stun_sock6 = -1;
  ml->derp.sockfd = -1;

  /* Take the only large control-plane block before NVS, queues, sockets,
   * and task stacks can split the C3 heap. It is released as soon as the
   * initial peer map has been consumed, before DERP allocates TLS state.
   * A warm start never talks to control, so it skips the reservation
   * (ML_TARGET_BOOTSTRAP_RESERVED_BYTES less heap held during bring-up). */
  if (ml->config.warm_start) {
    ESP_LOGI(TAG, "Warm start: control workspace not reserved (free=%u largest=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  } else if (!ml_workspace_reserve(&ml->coord_workspace, ML_TARGET_MAP_FRAME_STORAGE_SIZE, coord_workspace_alloc,
                                   coord_workspace_free, NULL)) {
    ESP_LOGE(TAG, "Failed to reserve %u-byte control workspace (free=%u largest=%u)",
             (unsigned)ML_TARGET_MAP_FRAME_STORAGE_SIZE, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    cJSON_InitHooks(NULL);
    free(ml);
    return NULL;
  } else {
    ESP_LOGI(TAG, "Reserved %u-byte control workspace early (free=%u largest=%u)",
             (unsigned)ML_TARGET_MAP_FRAME_STORAGE_SIZE, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }

  /* Resolve timing (0 = use defaults from #defines) */
  ml->t_disco_heartbeat_ms = ml->config.disco_heartbeat_ms ? ml->config.disco_heartbeat_ms : ML_DISCO_HEARTBEAT_MS;
  ml->t_stun_interval_ms = ml->config.stun_interval_ms ? ml->config.stun_interval_ms : ML_STUN_RESTUN_INTERVAL_MS;
  ml->t_ctrl_watchdog_ms = ml->config.ctrl_watchdog_ms ? ml->config.ctrl_watchdog_ms : ML_CTRL_WATCHDOG_MS;

  /* Apply Kconfig priority peer if set and app didn't provide one */
  if (ml->config.priority_peer_ip == 0 && strlen(CONFIG_ML_PRIORITY_PEER_IP) > 0) {
    ml->config.priority_peer_ip = microlink_parse_ip(CONFIG_ML_PRIORITY_PEER_IP);
    if (ml->config.priority_peer_ip) {
      ESP_LOGI(TAG, "Priority peer from Kconfig: %s", CONFIG_ML_PRIORITY_PEER_IP);
    }
  }

  /* Load or generate persistent keys */
  if (load_or_generate_keys(ml) != ESP_OK) {
    ml_workspace_release(&ml->coord_workspace);
    cJSON_InitHooks(NULL);
    free(ml);
    return NULL;
  }

  /* Initialize peer NVS cache */
  ml_peer_nvs_init();

  /* Initialize HTTP config server (loads NVS peer allowlist + settings) */
  ml->config_httpd = ml_config_httpd_init();

  /* Override config with NVS-saved settings (web UI save → restart flow).
   * NVS settings take priority over Kconfig defaults.  Strings are copied
   * into ml->nvs_* buffers so the const char* pointers remain valid. */
  if (ml->config_httpd) {
    const char* nvs_auth = ml_config_get_auth_key(ml->config_httpd);
    if (nvs_auth) {
      strncpy(ml->nvs_auth_key, nvs_auth, sizeof(ml->nvs_auth_key) - 1);
      ml->config.auth_key = ml->nvs_auth_key;
      ESP_LOGI(TAG, "Auth key overridden from NVS (len=%d)", (int)strlen(nvs_auth));
    }
    /* Device name: full name takes priority, then prefix+MAC, then Kconfig */
    const char* nvs_full_name = ml_config_get_device_name_full(ml->config_httpd);
    if (nvs_full_name) {
      /* Full custom hostname (e.g. "sensor-tailscale") */
      strncpy(ml->nvs_device_name, nvs_full_name, sizeof(ml->nvs_device_name) - 1);
      ml->config.device_name = ml->nvs_device_name;
      ESP_LOGI(TAG, "Device name from NVS (full): %s", ml->nvs_device_name);
    } else {
      const char* nvs_prefix = ml_config_get_device_prefix(ml->config_httpd);
      if (nvs_prefix) {
        /* Device name = prefix + MAC suffix (e.g. "sensor-a1b2c3") */
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(ml->nvs_device_name, sizeof(ml->nvs_device_name), "%s-%02x%02x%02x", nvs_prefix, mac[3], mac[4],
                 mac[5]);
        ml->config.device_name = ml->nvs_device_name;
        ESP_LOGI(TAG, "Device name from NVS (prefix): %s", ml->nvs_device_name);
      }
    }

    /* v2 overrides */
    uint8_t nvs_max = ml_config_get_max_peers(ml->config_httpd);
    if (nvs_max > 0 && nvs_max <= ML_MAX_PEERS) {
      ml->config.max_peers = nvs_max;
      ESP_LOGI(TAG, "Max peers overridden from NVS: %d", nvs_max);
    }
    uint16_t nvs_hb = ml_config_get_disco_heartbeat_ms(ml->config_httpd);
    if (nvs_hb > 0 && nvs_hb <= 60000) {
      ml->config.disco_heartbeat_ms = nvs_hb;
      ml->t_disco_heartbeat_ms = nvs_hb;
      ESP_LOGI(TAG, "DISCO heartbeat overridden from NVS: %dms", nvs_hb);
    }
    uint32_t nvs_pip = ml_config_get_priority_peer_ip(ml->config_httpd);
    if (nvs_pip > 0) {
      ml->config.priority_peer_ip = nvs_pip;
      char pip_str[16];
      microlink_ip_to_str(nvs_pip, pip_str);
      ESP_LOGI(TAG, "Priority peer overridden from NVS: %s", pip_str);
    }
    const char* nvs_ctrl = ml_config_get_ctrl_host(ml->config_httpd);
    if (nvs_ctrl) {
      strncpy(ml->ctrl_host, nvs_ctrl, sizeof(ml->ctrl_host) - 1);
      ESP_LOGI(TAG, "Control plane overridden from NVS: %s", ml->ctrl_host);
    }
    ml->debug_flags = ml_config_get_debug_flags(ml->config_httpd);
    if (ml->debug_flags) {
      ESP_LOGI(TAG, "Debug flags from NVS: 0x%02x", ml->debug_flags);
    }
  }

  /* Create event group */
  ml->events = xEventGroupCreate();
  if (!ml->events) {
    ESP_LOGE(TAG, "Failed to create event group");
    ml_workspace_release(&ml->coord_workspace);
    cJSON_InitHooks(NULL);
    free(ml);
    return NULL;
  }

  /* Create queues */
  ml->derp_tx_queue = xQueueCreate(ML_DERP_TX_QUEUE_DEPTH, sizeof(ml_derp_tx_item_t));
  ml->disco_rx_queue = xQueueCreate(ML_DISCO_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
  ml->wg_rx_queue = xQueueCreate(ML_WG_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
  ml->stun_rx_queue = xQueueCreate(ML_STUN_RX_QUEUE_DEPTH, sizeof(ml_rx_packet_t));
  ml->coord_cmd_queue = xQueueCreate(ML_COORD_CMD_QUEUE_DEPTH, sizeof(ml_coord_cmd_t));
  ml->peer_update_queue = xQueueCreate(ML_PEER_UPDATE_QUEUE_DEPTH, sizeof(ml_peer_update_t*));

  if (!ml->derp_tx_queue || !ml->disco_rx_queue || !ml->wg_rx_queue || !ml->stun_rx_queue || !ml->coord_cmd_queue ||
      !ml->peer_update_queue) {
    ESP_LOGE(TAG, "Failed to create queues");
    microlink_destroy(ml);
    return NULL;
  }

  ESP_LOGI(TAG, "MicroLink v2 initialized (max_peers=%d)", ml->config.max_peers);
  return ml;
}

esp_err_t ml_start_data_plane_tasks(microlink_t* ml) {
  if (!ml) return ESP_ERR_INVALID_ARG;

  BaseType_t ret;
  if (!ml->net_io_task) {
    ret = xTaskCreatePinnedToCore(ml_net_io_task, "ml_net_io", ML_TASK_NET_IO_STACK, ml, ML_TASK_NET_IO_PRIO,
                                  &ml->net_io_task, ML_TASK_NET_IO_CORE);
    if (ret != pdPASS) {
      snprintf(ml->last_error, sizeof(ml->last_error), "net I/O task allocation failed: stack=%u free=%u largest=%u",
               (unsigned)ML_TASK_NET_IO_STACK, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      return ESP_ERR_NO_MEM;
    }
  }

  if (!ml->derp_tx_task) {
    ret = xTaskCreatePinnedToCore(ml_derp_tx_task, "ml_derp_tx", ML_TASK_DERP_TX_STACK, ml, ML_TASK_DERP_TX_PRIO,
                                  &ml->derp_tx_task, ML_TASK_DERP_TX_CORE);
    if (ret != pdPASS) {
      snprintf(ml->last_error, sizeof(ml->last_error), "DERP task allocation failed: stack=%u free=%u largest=%u",
               (unsigned)ML_TASK_DERP_TX_STACK, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      return ESP_ERR_NO_MEM;
    }
  }

  if (!ml->wg_mgr_task) {
    ret = xTaskCreatePinnedToCore(ml_wg_mgr_task, "ml_wg_mgr", ML_TASK_WG_MGR_STACK, ml, ML_TASK_WG_MGR_PRIO,
                                  &ml->wg_mgr_task, ML_TASK_WG_MGR_CORE);
    if (ret != pdPASS) {
      snprintf(ml->last_error, sizeof(ml->last_error), "WireGuard task allocation failed: stack=%u free=%u largest=%u",
               (unsigned)ML_TASK_WG_MGR_STACK, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      return ESP_ERR_NO_MEM;
    }
  }

  ESP_LOGI(TAG, "Data-plane tasks started (free=%u largest=%u)", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return ESP_OK;
}

esp_err_t microlink_start(microlink_t* ml) {
  if (!ml) return ESP_ERR_INVALID_ARG;
  if (ml->state != ML_STATE_IDLE) {
    ESP_LOGW(TAG, "Already started (state=%d)", ml->state);
    return ESP_ERR_INVALID_STATE;
  }

  ml->state = ML_STATE_WIFI_WAIT;
  memset(&ml->timing, 0, sizeof(ml->timing));
  ml->timing.start_ms = ml_get_time_ms();

  /* Clear session bits from any previous stop(): a restarted task must
   * not see a stale ML_EVT_SHUTDOWN_REQUEST (or stale exit/connected bits). */
  xEventGroupClearBits(ml->events, ML_EVT_WIFI_CONNECTED | ML_EVT_COORD_REGISTERED | ML_EVT_DERP_CONNECTED |
                                       ML_EVT_WG_READY | ML_EVT_PEERS_AVAILABLE | ML_EVT_STUN_COMPLETE |
                                       ML_EVT_SHUTDOWN_REQUEST | ML_EVT_DERP_RECONNECT | ML_EVT_DERP_CONNECT_REQ |
                                       ML_EVT_NET_IO_EXITED | ML_EVT_DERP_TX_EXITED | ML_EVT_COORD_EXITED |
                                       ML_EVT_WG_MGR_EXITED);

  /* Set WiFi TX power if configured */
  if (ml->config.wifi_tx_power_dbm > 0) {
    int8_t power_quarter_dbm = ml->config.wifi_tx_power_dbm * 4;
    esp_wifi_set_max_tx_power(power_quarter_dbm);
    ESP_LOGI(TAG, "WiFi TX power set to %d dBm", ml->config.wifi_tx_power_dbm);
  }

#ifdef CONFIG_ML_ZERO_COPY_WG
  /* Zero-copy mode: raw lwIP PCB replaces BSD socket for DISCO/WG UDP.
   * WG packets go directly to wireguardif_network_rx() from tcpip_thread.
   * DISCO packets go to SPSC ring buffer, STUN to existing queue. */
  if (ml_zerocopy_init(ml) != ESP_OK) {
    ESP_LOGE(TAG, "Zero-copy init failed, falling back to BSD socket");
    goto bsd_socket_fallback;
  }
  ESP_LOGI(TAG, "Zero-copy WG mode active (high-throughput)");
  goto skip_bsd_socket;

bsd_socket_fallback:
#endif
  /* Create DISCO/magicsock UDP socket (port 51820 = WireGuard standard)
   * This is the single socket for ALL direct UDP traffic: DISCO pings/pongs,
   * CallMeMaybe probes, and WireGuard encrypted data.
   * Matches v1 microlink_disco_init() and tailscale's pconn4. */
  ml->disco_sock4 = ml_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (ml->disco_sock4 >= 0) {
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(51820),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (ml_bind(ml->disco_sock4, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
      ESP_LOGW(TAG, "Failed to bind port 51820 (errno=%d), trying ephemeral", errno);
      bind_addr.sin_port = 0;
      ml_bind(ml->disco_sock4, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
    }

    /* Mark packets as DSCP 46 (Expedited Forwarding) → WMM AC_VO.
     * WiFi APs with WMM use shorter contention windows for voice-priority
     * traffic, reducing jitter on WireGuard/DISCO UDP packets. */
    int tos = 0xB8; /* DSCP 46 = EF, TOS byte = 46 << 2 = 184 */
    setsockopt(ml->disco_sock4, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    /* Set non-blocking for select() in net_io */
    int flags = ml_fcntl(ml->disco_sock4, F_GETFL, 0);
    ml_fcntl(ml->disco_sock4, F_SETFL, flags | O_NONBLOCK);

    /* Record the actual bound port (getsockname not wrapped; AT sockets use stored port) */
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(ml->disco_sock4, (struct sockaddr*)&local_addr, &addr_len);
    ml->disco_local_port = ntohs(local_addr.sin_port);
    ESP_LOGI(TAG, "DISCO/magicsock UDP socket bound to port %d", ml->disco_local_port);
  } else {
    ESP_LOGE(TAG, "Failed to create DISCO socket: errno=%d", errno);
  }
#ifdef CONFIG_ML_ZERO_COPY_WG
skip_bsd_socket:;
#endif

  /* Targeted CrossReader sessions need only the coordinator during control
   * bootstrap. Starting all four task stacks alongside the 16KB control
   * workspace left lwIP with too little heap to open the control socket.
   * A warm start has no bootstrap and no workspace: start everything now. */
  bool stage_data_plane =
      ml_has_target(ml->config.priority_peer_ip, ml->config.priority_peer_name) && !ml->config.warm_start;
#ifdef ML_EAGER_DATA_PLANE_TEST
  /* Emulator-only regression mode for the pre-v6 startup order. The target
   * remains configured so registration still uses the bounded map path. */
  stage_data_plane = false;
#endif
  BaseType_t ret = xTaskCreatePinnedToCore(ml_coord_task, "ml_coord", ML_TASK_COORD_STACK, ml, ML_TASK_COORD_PRIO,
                                           &ml->coord_task, ML_TASK_COORD_CORE);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create coord task");
    microlink_stop(ml);
    return ESP_FAIL;
  }

  if (!stage_data_plane && ml_start_data_plane_tasks(ml) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create data-plane tasks");
    microlink_stop(ml);
    return ESP_FAIL;
  }

  /* WiFi is expected to be connected before microlink_start() is called.
   * Signal the event so coord/wg_mgr tasks proceed immediately. */
  xEventGroupSetBits(ml->events, ML_EVT_WIFI_CONNECTED);

  /* Signal coord task to start connecting */
  ml_coord_cmd_t cmd = ML_CMD_CONNECT;
  xQueueSend(ml->coord_cmd_queue, &cmd, 0);

  /* Start HTTP config server (binds port 80, serves config page + REST API) */
  if (ml->config_httpd) {
    ml_config_httpd_start(ml->config_httpd, ml);
  }

  ESP_LOGI(TAG, "%s startup tasks ready (free=%u largest=%u)",
           ml->config.warm_start ? "Warm" : (stage_data_plane ? "Targeted" : "Full"),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  return ESP_OK;
}

esp_err_t microlink_rebind(microlink_t* ml) {
  if (!ml) return ESP_ERR_INVALID_ARG;
  if (ml->state == ML_STATE_IDLE) return ESP_ERR_INVALID_STATE;

  ESP_LOGI(TAG, "=== Rebinding to new network interface ===");

  /* Step 1: Invalidate socket FDs FIRST, then delay to let net_io_task's
   * select() cycle complete (50ms timeout). Only THEN close the old FDs.
   * Closing a socket while another thread has it in select() deadlocks
   * on lwIP's global socket lock. */
  int old_disco = ml->disco_sock4;
  int old_stun = ml->stun_sock;
  int old_stun6 = ml->stun_sock6;

  /* Invalidate: net_io_task will skip these on next iteration */
  ml->disco_sock4 = -1;
  ml->stun_sock = -1;
  ml->stun_sock6 = -1;

  /* Wait for net_io_task select() to cycle out (50ms timeout + margin) */
  vTaskDelay(pdMS_TO_TICKS(100));

  /* Now safe to close the old FDs */
  if (old_disco >= 0) ml_close_sock(old_disco);
  if (old_stun >= 0) ml_close_sock(old_stun);
  if (old_stun6 >= 0) ml_close_sock(old_stun6);
  ESP_LOGI(TAG, "Rebind: closed DISCO + STUN sockets");

  /* Step 2: Signal coord to reconnect. ML_CMD_FORCE_RECONNECT closes
   * the coord socket, resets Noise state, and re-enters the
   * STUN → DNS → TCP → Noise → Register → MapRequest flow.
   * Peers and WG state are preserved. */
  xEventGroupClearBits(ml->events, ML_EVT_COORD_REGISTERED);
  ml_coord_cmd_t cmd = ML_CMD_FORCE_RECONNECT;
  xQueueSend(ml->coord_cmd_queue, &cmd, pdMS_TO_TICKS(100));

  /* Step 3: Signal DERP to reconnect. ML_EVT_DERP_RECONNECT triggers
   * derp_tx_task to close TLS, then reconnect with full handshake.
   * Pending TX packets are drained but WG state is preserved. */
  xEventGroupClearBits(ml->events, ML_EVT_DERP_CONNECTED);
  xEventGroupSetBits(ml->events, ML_EVT_DERP_RECONNECT);
  ESP_LOGI(TAG, "Rebind: signaled coord + DERP to reconnect");

  /* Step 4: Brief delay for coord/DERP to process reconnect signals */
  vTaskDelay(pdMS_TO_TICKS(200));

  /* Step 5: Recreate DISCO UDP socket on the new interface */
#ifdef CONFIG_ML_ZERO_COPY_WG
  ml_zerocopy_deinit(ml);
  if (ml_zerocopy_init(ml) == ESP_OK) {
    ESP_LOGI(TAG, "Rebind: zero-copy DISCO PCB recreated");
    goto rebind_wg_update;
  }
  ESP_LOGW(TAG, "Rebind: zero-copy init failed, using BSD socket fallback");
#endif
  ml->disco_sock4 = ml_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (ml->disco_sock4 >= 0) {
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(51820),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (ml_bind(ml->disco_sock4, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
      bind_addr.sin_port = 0;
      ml_bind(ml->disco_sock4, (struct sockaddr*)&bind_addr, sizeof(bind_addr));
    }
    int tos = 0xB8;
    setsockopt(ml->disco_sock4, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    int flags = ml_fcntl(ml->disco_sock4, F_GETFL, 0);
    ml_fcntl(ml->disco_sock4, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(ml->disco_sock4, (struct sockaddr*)&local_addr, &addr_len);
    ml->disco_local_port = ntohs(local_addr.sin_port);
    ESP_LOGI(TAG, "Rebind: DISCO socket rebound to port %d", ml->disco_local_port);
  } else {
    ESP_LOGE(TAG, "Rebind: failed to create DISCO socket: errno=%d", errno);
  }

#ifdef CONFIG_ML_ZERO_COPY_WG
rebind_wg_update:
#endif
  /* Step 6: Update WG output mode for new transport */
  ml_wg_mgr_update_transport(ml);

  ESP_LOGI(TAG, "=== Rebind complete — waiting for coord+DERP reconnect ===");
  return ESP_OK;
}

esp_err_t microlink_stop(microlink_t* ml) {
  if (!ml) return ESP_ERR_INVALID_ARG;

  ESP_LOGI(TAG, "Stopping...");
  xEventGroupSetBits(ml->events, ML_EVT_SHUTDOWN_REQUEST);

  /* Wait for tasks to exit (they check ML_EVT_SHUTDOWN_REQUEST).
   * Tasks call vTaskDelete(NULL) to self-delete, so we must NOT call
   * vTaskDelete() on them again; that causes a crash in uxListRemove
   * because the task's list node is already invalid. Each task sets its
   * ML_EVT_*_EXITED bit right before self-deleting; wait for the tasks
   * that were actually created, with a 3s bounded fallback. */
  EventBits_t exit_bits = 0;
  if (ml->net_io_task) exit_bits |= ML_EVT_NET_IO_EXITED;
  if (ml->derp_tx_task) exit_bits |= ML_EVT_DERP_TX_EXITED;
  if (ml->coord_task) exit_bits |= ML_EVT_COORD_EXITED;
  if (ml->wg_mgr_task) exit_bits |= ML_EVT_WG_MGR_EXITED;
  if (exit_bits) {
    EventBits_t got = xEventGroupWaitBits(ml->events, exit_bits, pdFALSE, pdTRUE, pdMS_TO_TICKS(3000));
    if ((got & exit_bits) != exit_bits) {
      ESP_LOGE(TAG, "Stop timed out: tasks still running (got=0x%x want=0x%x)", (unsigned)got, (unsigned)exit_bits);
      /* Do not free queues, event groups, or ml while any task may
       * still use them. The caller can retry stop later or reboot. */
      return ESP_ERR_TIMEOUT;
    }
    /* Let vTaskDelete(NULL) complete and the idle task reclaim the
     * deleted tasks' TCBs/stacks before we free resources they used. */
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ml->net_io_task = NULL;
  ml->derp_tx_task = NULL;
  ml->coord_task = NULL;
  ml->wg_mgr_task = NULL;

  /* Stop HTTP config server */
  if (ml->config_httpd) {
    ml_config_httpd_stop(ml->config_httpd);
  }

  /* Clean up zero-copy PCB if active */
#ifdef CONFIG_ML_ZERO_COPY_WG
  ml_zerocopy_deinit(ml);
#endif

  /* Close sockets */
  if (ml->disco_sock4 >= 0) {
    ml_close_sock(ml->disco_sock4);
    ml->disco_sock4 = -1;
  }
  if (ml->stun_sock >= 0) {
    ml_close_sock(ml->stun_sock);
    ml->stun_sock = -1;
  }
  if (ml->stun_sock6 >= 0) {
    ml_close_sock(ml->stun_sock6);
    ml->stun_sock6 = -1;
  }
  /* DERP socket is normally closed by the DERP task's own
   * ml_derp_disconnect() on exit (sockfd then -1); this guard only
   * covers a task that never ran or was stuck past the exit timeout. */
  if (ml->derp.sockfd >= 0) {
    ml_close_sock(ml->derp.sockfd);
    ml->derp.sockfd = -1;
  }

  ml->state = ML_STATE_IDLE;
  ESP_LOGI(TAG, "Stopped");
  return ESP_OK;
}

void microlink_destroy(microlink_t* ml) {
  if (!ml) return;

  if (microlink_stop(ml) != ESP_OK) {
    ESP_LOGE(TAG, "Destroy deferred because tasks have not stopped");
    return;
  }

  /* Deinitialize peer NVS */
  ml_peer_nvs_deinit();

  /* Deinitialize HTTP config server */
  if (ml->config_httpd) {
    ml_config_httpd_deinit(ml->config_httpd);
    ml->config_httpd = NULL;
  }

  /* Drain queues whose items own heap buffers, then delete.
   * (coord_cmd_queue carries plain enums, nothing to free.) */
  ml_derp_tx_item_t tx_item;
  while (ml->derp_tx_queue && xQueueReceive(ml->derp_tx_queue, &tx_item, 0) == pdTRUE) {
    free(tx_item.data);
  }
  ml_rx_packet_t rx_pkt;
  while (ml->disco_rx_queue && xQueueReceive(ml->disco_rx_queue, &rx_pkt, 0) == pdTRUE) {
    free(rx_pkt.data);
  }
  while (ml->wg_rx_queue && xQueueReceive(ml->wg_rx_queue, &rx_pkt, 0) == pdTRUE) {
    free(rx_pkt.data);
  }
  while (ml->stun_rx_queue && xQueueReceive(ml->stun_rx_queue, &rx_pkt, 0) == pdTRUE) {
    free(rx_pkt.data);
  }
  ml_peer_update_t* pending_update;
  while (ml->peer_update_queue && xQueueReceive(ml->peer_update_queue, &pending_update, 0) == pdTRUE) {
    free(pending_update);
  }

  /* Delete queues */
  if (ml->derp_tx_queue) vQueueDelete(ml->derp_tx_queue);
  if (ml->disco_rx_queue) vQueueDelete(ml->disco_rx_queue);
  if (ml->wg_rx_queue) vQueueDelete(ml->wg_rx_queue);
  if (ml->stun_rx_queue) vQueueDelete(ml->stun_rx_queue);
  if (ml->coord_cmd_queue) vQueueDelete(ml->coord_cmd_queue);
  if (ml->peer_update_queue) vQueueDelete(ml->peer_update_queue);

  /* Delete event group */
  if (ml->events) vEventGroupDelete(ml->events);

  /* Clear keys from memory */
  memset(ml->machine_private_key, 0, 32);
  memset(ml->wg_private_key, 0, 32);
  memset(ml->disco_private_key, 0, 32);

  ml_workspace_release(&ml->coord_workspace);

  /* Restore global cJSON allocators (init installed PSRAM-backed hooks
   * that would otherwise divert every other cJSON user in the firmware) */
  cJSON_InitHooks(NULL);

  free(ml);
  ESP_LOGI(TAG, "Destroyed");
}

/* ============================================================================
 * State Queries
 * ========================================================================== */

microlink_state_t microlink_get_state(const microlink_t* ml) { return ml ? ml->state : ML_STATE_IDLE; }

bool microlink_is_connected(const microlink_t* ml) { return ml && ml->state == ML_STATE_CONNECTED; }

uint32_t microlink_get_vpn_ip(const microlink_t* ml) { return ml ? ml->vpn_ip : 0; }

int microlink_get_peer_count(const microlink_t* ml) { return ml ? ml->peer_count : 0; }

esp_err_t microlink_get_peer_info(const microlink_t* ml, int index, microlink_peer_info_t* info) {
  if (!ml || !info || index < 0 || index >= ml->peer_count) {
    return ESP_ERR_INVALID_ARG;
  }
  const ml_peer_t* p = &ml->peers[index];
  info->vpn_ip = p->vpn_ip;
  strncpy(info->hostname, p->hostname, sizeof(info->hostname) - 1);
  memcpy(info->public_key, p->public_key, 32);
  info->online = p->active;
  info->direct_path = p->has_direct_path;
  return ESP_OK;
}

/* ============================================================================
 * Send API
 * ========================================================================== */

esp_err_t microlink_send(microlink_t* ml, uint32_t dest_vpn_ip, const uint8_t* data, size_t len) {
  if (!ml || !data || len == 0 || len > 1400) return ESP_ERR_INVALID_ARG;
  if (ml->state != ML_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;

  /* Find peer by VPN IP */
  for (int i = 0; i < ml->peer_count; i++) {
    if (ml->peers[i].vpn_ip == dest_vpn_ip && ml->peers[i].active) {
      /* TODO: Route through WireGuard tunnel */
      /* For now, queue via DERP as fallback */
      return ml_derp_queue_send(ml, ml->peers[i].public_key, data, len);
    }
  }
  return ESP_ERR_NOT_FOUND;
}

/* ============================================================================
 * Callbacks
 * ========================================================================== */

void microlink_set_state_callback(microlink_t* ml, microlink_state_cb_t cb, void* user_data) {
  if (ml) {
    ml->state_cb = cb;
    ml->state_cb_data = user_data;
  }
}

void microlink_set_peer_callback(microlink_t* ml, microlink_peer_cb_t cb, void* user_data) {
  if (ml) {
    ml->peer_cb = cb;
    ml->peer_cb_data = user_data;
  }
}

void microlink_set_data_callback(microlink_t* ml, microlink_data_cb_t cb, void* user_data) {
  if (ml) {
    ml->data_cb = cb;
    ml->data_cb_data = user_data;
  }
}

/* ============================================================================
 * Utilities
 * ========================================================================== */

void microlink_ip_to_str(uint32_t ip, char* buf) {
  snprintf(buf, 16, "%lu.%lu.%lu.%lu", (unsigned long)((ip >> 24) & 0xFF), (unsigned long)((ip >> 16) & 0xFF),
           (unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF));
}

uint32_t microlink_parse_ip(const char* ip_str) {
  if (!ip_str) return 0;
  unsigned int a, b, c, d;
  if (sscanf(ip_str, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
  if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
  return (a << 24) | (b << 16) | (c << 8) | d;
}

const char* microlink_default_device_name(void) {
  static char name[48] = {0};
  if (name[0] == 0) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    /* Use ML_DEVICE_NAME as prefix if configured, otherwise "esp32" */
    const char* prefix = CONFIG_ML_DEVICE_NAME;
    if (!prefix || !prefix[0]) prefix = "esp32";
    snprintf(name, sizeof(name), "%s-%02x%02x%02x", prefix, mac[3], mac[4], mac[5]);
  }
  return name;
}

const char* microlink_imei_device_name(void) {
#ifdef CONFIG_ML_ENABLE_CELLULAR
  static char name[48] = {0}; /* prefix + "-" + 15-digit IMEI + null */
  const char* imei = ml_cellular_get_imei();
  if (imei && imei[0]) {
    const char* prefix = CONFIG_ML_DEVICE_NAME;
    if (!prefix || !prefix[0]) prefix = "esp32";
    snprintf(name, sizeof(name), "%s-%s", prefix, imei);
    return name;
  }
#endif
  return NULL;
}

uint64_t ml_get_time_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000ULL); }

/* ============================================================================
 * MagicDNS: resolve tailnet hostnames against peer list
 * ========================================================================== */

/* Case-insensitive string compare (limited to len bytes) */
static int strncasecmp_local(const char* a, const char* b, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char ca = a[i], cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return ca - cb;
    if (ca == '\0') return 0;
  }
  return 0;
}

uint32_t microlink_resolve(const microlink_t* ml, const char* hostname) {
  if (!ml || !hostname || hostname[0] == '\0') return 0;

  size_t query_len = strlen(hostname);

  for (int i = 0; i < ml->peer_count; i++) {
    const ml_peer_t* p = &ml->peers[i];
    if (!p->active || p->hostname[0] == '\0') continue;

    /* 1. Exact match (case-insensitive) */
    if (strncasecmp_local(p->hostname, hostname, sizeof(p->hostname)) == 0) {
      return p->vpn_ip;
    }

    /* 2. Prefix match: query "npc1" matches peer "npc1.tail12345.ts.net"
     * The query must match up to the first '.' in the peer hostname. */
    const char* dot = strchr(p->hostname, '.');
    if (dot) {
      size_t short_len = (size_t)(dot - p->hostname);
      if (query_len == short_len && strncasecmp_local(p->hostname, hostname, short_len) == 0) {
        return p->vpn_ip;
      }
    }
  }

  return 0; /* Not found */
}

/* --- CrossPoint fork additions (see UPSTREAM.md) --- */

const char* microlink_get_last_error(const microlink_t* ml) {
  return ml && ml->last_error[0] ? ml->last_error : "none recorded";
}

esp_err_t microlink_set_ctrl_host(microlink_t* ml, const char* host) {
  if (!ml) return ESP_ERR_INVALID_ARG;
  if (ml->state != ML_STATE_IDLE) {
    ESP_LOGE(TAG, "set_ctrl_host: must be called before microlink_start()");
    return ESP_ERR_INVALID_STATE;
  }
  if (!host || host[0] == '\0') {
    ml->ctrl_host[0] = '\0'; /* fall back to ML_CTRL_HOST default */
    return ESP_OK;
  }
  strncpy(ml->ctrl_host, host, sizeof(ml->ctrl_host) - 1);
  ml->ctrl_host[sizeof(ml->ctrl_host) - 1] = '\0';
  ESP_LOGI(TAG, "Control plane host set to %s", ml->ctrl_host);
  return ESP_OK;
}

uint16_t microlink_get_derp_region(const microlink_t* ml) {
  if (!ml) return 0;
  return ml->target_derp_region ? ml->target_derp_region : ml->derp_home_region;
}

void microlink_log_stack_watermarks(const microlink_t* ml) {
  if (!ml) return;
  ESP_LOGE(TAG, "STACK_HWM min-free/size bytes: coord=%u/%u derp_tx=%u/%u net_io=%u/%u wg_mgr=%u/%u",
           ml->coord_task ? (unsigned)uxTaskGetStackHighWaterMark(ml->coord_task) : 0u,
           (unsigned)ML_TASK_COORD_STACK_SIZE,
           ml->derp_tx_task ? (unsigned)uxTaskGetStackHighWaterMark(ml->derp_tx_task) : 0u,
           (unsigned)ML_TASK_DERP_TX_STACK_SIZE,
           ml->net_io_task ? (unsigned)uxTaskGetStackHighWaterMark(ml->net_io_task) : 0u,
           (unsigned)ML_TASK_NET_IO_STACK_SIZE,
           ml->wg_mgr_task ? (unsigned)uxTaskGetStackHighWaterMark(ml->wg_mgr_task) : 0u,
           (unsigned)ML_TASK_WG_MGR_STACK_SIZE);
}

esp_err_t microlink_wait_peers_ready(microlink_t* ml, const uint32_t* vpn_ips, int count, uint32_t timeout_ms) {
  if (!ml || !vpn_ips || count <= 0 || count > ML_MAX_PEERS) return ESP_ERR_INVALID_ARG;
  if (ml->state != ML_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;

  /* Same wake-up sequence microlink_tcp_connect() uses, exposed so callers
   * routing plain BSD/lwIP sockets (e.g. an HTTP client) through the WG
   * netif can wait for the tunnel instead of eating EHOSTUNREACH. All
   * initiations go out first so relayed round trips overlap. */
  for (int i = 0; i < count; i++) {
    if (vpn_ips[i] == 0) return ESP_ERR_INVALID_ARG;
    ml_wg_mgr_trigger_handshake(ml, vpn_ips[i]);
    ml_wg_mgr_send_cmm(ml, vpn_ips[i]);
  }

  uint32_t wait_ms = 0;
  const uint32_t max_wait = timeout_ms > 0 ? timeout_ms : 15000;
  for (;;) {
    bool all_up = true;
    for (int i = 0; i < count; i++) {
      if (!ml_wg_mgr_peer_is_up(ml, vpn_ips[i])) all_up = false;
    }
    if (all_up) return ESP_OK;
    if (wait_ms >= max_wait) return ESP_ERR_TIMEOUT;
    vTaskDelay(pdMS_TO_TICKS(500));
    wait_ms += 500;
    /* Re-trigger every 5s in case the first handshake initiation dropped
     * (trigger_handshake is a no-op for a peer whose session is up). */
    if ((wait_ms % 5000) == 0) {
      for (int i = 0; i < count; i++) {
        ml_wg_mgr_trigger_handshake(ml, vpn_ips[i]);
        ml_wg_mgr_send_cmm(ml, vpn_ips[i]);
      }
    }
  }
}

esp_err_t microlink_wait_peer_ready(microlink_t* ml, uint32_t vpn_ip, uint32_t timeout_ms) {
  return microlink_wait_peers_ready(ml, &vpn_ip, 1, timeout_ms);
}

bool microlink_peer_is_up(microlink_t* ml, uint32_t vpn_ip) {
  if (!ml || vpn_ip == 0 || ml->state != ML_STATE_CONNECTED) return false;
  return ml_wg_mgr_peer_is_up(ml, vpn_ip);
}

/* The NVS table is opened by microlink_init() and closed by microlink_destroy();
 * outside a session these open it for the query and close it again so nothing
 * stays resident. */
bool microlink_peer_cache_contains(uint32_t vpn_ip) {
  if (vpn_ip == 0) return false;
  const bool was_open = ml_peer_nvs_is_initialized();
  if (!was_open && ml_peer_nvs_init() != ESP_OK) return false;
  const bool found = ml_peer_nvs_find(vpn_ip, NULL);
  if (!was_open) ml_peer_nvs_deinit();
  return found;
}

bool microlink_peer_cache_poison(uint32_t vpn_ip) {
  if (vpn_ip == 0) return false;
  const bool was_open = ml_peer_nvs_is_initialized();
  if (!was_open && ml_peer_nvs_init() != ESP_OK) return false;
  const bool poisoned = ml_peer_nvs_poison(vpn_ip);
  if (!was_open) ml_peer_nvs_deinit();
  if (poisoned) ESP_LOGE(TAG, "Peer cache poisoned for %u.%u.%u.%u", (unsigned)(vpn_ip >> 24),
                         (unsigned)((vpn_ip >> 16) & 0xff), (unsigned)((vpn_ip >> 8) & 0xff), (unsigned)(vpn_ip & 0xff));
  return poisoned;
}
