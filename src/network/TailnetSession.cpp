#include "TailnetSession.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "TailscaleStore.h"

TailnetSession& TailnetSession::getInstance() {
  static TailnetSession instance;
  return instance;
}

const char* TailnetSession::lastErrorCode() const {
  switch (lastError) {
    case Error::NONE:
      return "TS-OK";
    case Error::NOT_COMPILED:
      return "TS-E01";
    case Error::NO_AUTH_KEY:
      return "TS-E02";
    case Error::LOW_MEMORY:
      return "TS-E03";
    case Error::INIT_FAILED:
      return "TS-E04";
    case Error::START_FAILED:
      return "TS-E05";
    case Error::BRINGUP_TIMEOUT:
      return "TS-E06";
    case Error::INVALID_URL:
      return "TS-E07";
    case Error::HOST_NOT_FOUND:
      return "TS-E08";
    case Error::HOST_NOT_TAILNET:
      return "TS-E09";
    case Error::PEER_TIMEOUT:
      return "TS-E10";
    case Error::CLIENT_ERROR:
      return "TS-E11";
    case Error::RESOLVER_NOT_FOUND:
      return "TS-E12";
  }
  return "TS-E00";
}

const char* TailnetSession::lastErrorMessage() const {
  if (!errorDetail.empty()) return errorDetail.c_str();
  switch (lastError) {
    case Error::NONE:
      return "No error";
    case Error::NOT_COMPILED:
      return "Tailscale is not included in this firmware";
    case Error::NO_AUTH_KEY:
      return "No Tailscale auth key configured";
    case Error::LOW_MEMORY:
      return "Not enough free memory for Tailscale";
    case Error::INIT_FAILED:
      return "Tailscale initialization failed";
    case Error::START_FAILED:
      return "Tailscale client failed to start";
    case Error::BRINGUP_TIMEOUT:
      return "Tailscale login timed out";
    case Error::INVALID_URL:
      return "The OPDS server URL has no host";
    case Error::HOST_NOT_FOUND:
      return "Tailnet host was not found";
    case Error::HOST_NOT_TAILNET:
      return "Server DNS is not a tailnet address";
    case Error::PEER_TIMEOUT:
      return "Peer handshake timed out; peer may be offline or unreachable";
    case Error::CLIENT_ERROR:
      return "Tailscale client entered an error state";
    case Error::RESOLVER_NOT_FOUND:
      return "Tailnet DNS server is not visible to this device";
  }
  return "Unknown Tailscale error";
}

void TailnetSession::setError(const Error error) {
  lastError = error;
  errorDetail.clear();
}

void TailnetSession::writeErrorLog(const char* detail) const {
  String contents;
  contents.reserve(384);
  contents += "format=8\nfirmware=" CROSSPOINT_VERSION "\nfeature=routed-target-v8\ncode=";
  contents += lastErrorCode();
  contents += "\nmessage=";
  contents += lastErrorMessage();
  contents += "\ndetail=";
  contents += detail ? detail : "";
  contents += '\n';
  if (!Storage.writeFile("/tailscale-error.log", contents)) {
    LOG_ERR("TSN", "Could not write /tailscale-error.log");
  }
}

void TailnetSession::writeStartupLog() const {
  if (Storage.exists("/tailscale-error.log")) Storage.remove("/tailscale-error.log");
  const String marker = "format=8\nfirmware=" CROSSPOINT_VERSION
                        "\nfeature=routed-target-v8\nstatus=tailnet startup began; no failure recorded yet\n";
  if (!Storage.writeFile("/tailscale-error.log", marker)) {
    LOG_ERR("TSN", "Could not initialize /tailscale-error.log");
  }
}

#ifdef CROSSPOINT_TAILNET

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <lwip/dns.h>
#include <lwip/inet.h>
#include <lwip/tcpip.h>
#include <microlink.h>

#include <algorithm>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* TAG = "TSN";

// Epoch seconds when the clock has been set (SNTP or RTC), else 0: the cache
// timestamps must not record a 1970 boot time as a real age.
uint32_t epochNow() {
  const time_t now = time(nullptr);
  return now >= 1700000000 ? static_cast<uint32_t>(now) : 0;
}

struct AllocationFailureSnapshot {
  size_t requested = 0;
  uint32_t caps = 0;
  uint32_t count = 0;
  const char* functionName = nullptr;
};

AllocationFailureSnapshot allocationFailure;
portMUX_TYPE allocationFailureMux = portMUX_INITIALIZER_UNLOCKED;
bool allocationHookInstalled = false;

void recordAllocationFailure(size_t requested, uint32_t caps, const char* functionName) {
  portENTER_CRITICAL(&allocationFailureMux);
  allocationFailure.requested = requested;
  allocationFailure.caps = caps;
  allocationFailure.functionName = functionName;
  ++allocationFailure.count;
  portEXIT_CRITICAL(&allocationFailureMux);
}

void prepareAllocationDiagnostics() {
  portENTER_CRITICAL(&allocationFailureMux);
  allocationFailure.requested = 0;
  allocationFailure.caps = 0;
  allocationFailure.count = 0;
  allocationFailure.functionName = nullptr;
  portEXIT_CRITICAL(&allocationFailureMux);
  if (!allocationHookInstalled) {
    allocationHookInstalled = heap_caps_register_failed_alloc_callback(recordAllocationFailure) == ESP_OK;
  }
}

void appendAllocationFailure(char* output, size_t capacity) {
  if (!output || capacity == 0) return;
  AllocationFailureSnapshot snapshot;
  portENTER_CRITICAL(&allocationFailureMux);
  snapshot = allocationFailure;
  portEXIT_CRITICAL(&allocationFailureMux);
  if (snapshot.count == 0) return;
  const size_t used = strnlen(output, capacity);
  if (used >= capacity) return;
  snprintf(output + used, capacity - used, " alloc_fail=%u caps=0x%x fn=%.24s count=%u",
           static_cast<unsigned>(snapshot.requested), static_cast<unsigned>(snapshot.caps),
           snapshot.functionName ? snapshot.functionName : "unknown", static_cast<unsigned>(snapshot.count));
}

// Relaxed keepalive cadence: this is a download session, not a realtime link,
// and every probe is radio time.
constexpr uint32_t DISCO_HEARTBEAT_MS = 10000;
constexpr uint32_t STUN_INTERVAL_MS = 60000;

// 100.64.0.0/10, in host byte order
bool isCgnatIp(const uint32_t ip) { return (ip & 0xFFC00000u) == 0x64400000u; }

TailnetSession::Status statusFromState(const microlink_state_t state) {
  switch (state) {
    case ML_STATE_CONNECTING:
      return TailnetSession::Status::STARTING;
    case ML_STATE_REGISTERING:
      return TailnetSession::Status::REGISTERING;
    case ML_STATE_CONNECTED:
      return TailnetSession::Status::CONNECTED;
    default:
      return TailnetSession::Status::STARTING;
  }
}

}  // namespace

bool TailnetSession::startBringUp(const uint32_t timeoutMs) {
  setError(Error::NONE);
  if (stopDeferred) {
    setError(Error::CLIENT_ERROR);
    errorDetail = "The previous Tailscale session did not stop cleanly; restart the reader";
    LOG_ERR(TAG, "%s: %s", lastErrorCode(), errorDetail.c_str());
    writeErrorLog("stop_deferred=1");
    return false;
  }
  // Phase 2 of a split-DNS session keeps phase 1's entries in the SD log.
  if (!phaseTwo) writeStartupLog();
  prepareAllocationDiagnostics();
  auto* handle = static_cast<microlink_t*>(ml);
  if (handle && microlink_is_connected(handle)) {
    return true;
  }

  if (!TAILSCALE_STORE.hasAuthKey()) {
    setError(Error::NO_AUTH_KEY);
    LOG_ERR(TAG, "No Tailscale auth key configured");
    writeErrorLog("Auth key missing before startup");
    return false;
  }

  if (ESP.getFreeHeap() < MIN_TAILNET_FREE_HEAP || ESP.getMaxAllocHeap() < MIN_TAILNET_MAX_ALLOC) {
    setError(Error::LOW_MEMORY);
    LOG_ERR(TAG, "Not enough heap for tailnet session (free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    char detail[96];
    snprintf(detail, sizeof(detail), "free_heap=%u max_alloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    writeErrorLog(detail);
    return false;
  }

  if (!handle) {
    microlink_config_t config = {};
    config.auth_key = TAILSCALE_STORE.getAuthKey().c_str();
    config.device_name = TAILSCALE_STORE.getDeviceName().empty() ? microlink_default_device_name()
                                                                 : TAILSCALE_STORE.getDeviceName().c_str();
    config.enable_derp = true;
    // No control connection exists in a warm session to report STUN
    // endpoints to; the relayed path is all a download session needs.
    config.enable_stun = !warmActive;
    config.enable_disco = true;
    if (warmActive) {
      config.warm_start = true;
      config.warm_self_ip = microlink_parse_ip(TAILSCALE_STORE.getSelfIp().c_str());
      config.warm_derp_region = TAILSCALE_STORE.getDerpRegion();
    }
    config.max_peers = 4;  // Matches CONFIG_ML_MAX_PEERS / WIREGUARD_MAX_PEERS
    if (resolvePending) {
      // Phase 1 of a split-DNS lookup: only the resolver peer is needed.
      config.priority_peer_ip = resolverIp;
      config.priority_peer_name = nullptr;
    } else {
      config.priority_peer_ip = targetPeerIp;
      config.priority_peer_name = targetPeerHost.c_str();
      // Keep the resolver reachable so the HTTP client's own lookup of the
      // hostname also goes through the tunnel.
      config.secondary_peer_ip = targetPeerIp != 0 ? resolverIp : 0;
    }
    // Region learned by an earlier session (or phase 1 of this one), so
    // control records the right HomeDERP before the first handshake.
    config.preferred_derp_region = TAILSCALE_STORE.getDerpRegion();
    config.disco_heartbeat_ms = DISCO_HEARTBEAT_MS;
    config.stun_interval_ms = STUN_INTERVAL_MS;

    handle = microlink_init(&config);
    if (!handle) {
      setError(Error::INIT_FAILED);
      LOG_ERR(TAG, "microlink_init failed");
      char detail[160];
      snprintf(detail, sizeof(detail), "free_heap=%u max_alloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      appendAllocationFailure(detail, sizeof(detail));
      writeErrorLog(detail);
      return false;
    }
    ml = handle;

    if (!TAILSCALE_STORE.getControlHost().empty()) {
      microlink_set_ctrl_host(handle, TAILSCALE_STORE.getControlHost().c_str());
    }
  }

  LOG_INF(TAG, "Starting tailnet session (free heap %u)", ESP.getFreeHeap());
  const esp_err_t startErr = microlink_start(handle);
  if (startErr != ESP_OK) {
    setError(Error::START_FAILED);
    LOG_ERR(TAG, "%s: microlink_start failed: %s (0x%x)", lastErrorCode(), esp_err_to_name(startErr),
            static_cast<unsigned>(startErr));
    teardown();
    return false;
  }
  everActive = true;
  bringUpStartedAt = millis();
  bringUpTimeoutMs = timeoutMs;
  return true;
}

bool TailnetSession::setTargetUrl(const std::string& url) {
  // MicroLink keeps a pointer to targetPeerHost's buffer for the life of the
  // client; never change it while a handle exists (including a deferred stop).
  if (ml != nullptr) {
    LOG_ERR(TAG, "Ignoring new target URL while a Tailscale client exists");
    return false;
  }
  phaseTwo = false;
  const size_t schemeEnd = url.find("://");
  const size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  const size_t hostEnd = url.find_first_of(":/", hostStart);
  const std::string host =
      url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
  if (host.empty()) return false;
  targetPeerHost = host;
  targetPeerIp = microlink_parse_ip(host.c_str());
  if (targetPeerIp == 0) {
    IPAddress dnsIp;
    if (WiFi.hostByName(host.c_str(), dnsIp) == 1) {
      targetPeerIp = (static_cast<uint32_t>(dnsIp[0]) << 24) | (static_cast<uint32_t>(dnsIp[1]) << 16) |
                     (static_cast<uint32_t>(dnsIp[2]) << 8) | static_cast<uint32_t>(dnsIp[3]);
    } else {
      LOG_INF(TAG, "Wi-Fi DNS could not resolve %s", host.c_str());
    }
  }
  if (!isCgnatIp(targetPeerIp)) targetPeerIp = 0;

  // Split-DNS names (known only to a resolver inside the tailnet) need the
  // tunnel to resolve. With a resolver configured, either reuse the address
  // cached from an earlier session or plan a first phase that brings up the
  // resolver peer alone and asks it through the tunnel.
  resolverIp = 0;
  resolvePending = false;
  usingCachedIp = false;
  if (targetPeerIp == 0 && microlink_parse_ip(host.c_str()) == 0 && !TAILSCALE_STORE.getDnsServer().empty()) {
    resolverIp = microlink_parse_ip(TAILSCALE_STORE.getDnsServer().c_str());
    if (!isCgnatIp(resolverIp)) {
      LOG_ERR(TAG, "Tailnet DNS server '%s' is not a tailnet address; ignoring", TAILSCALE_STORE.getDnsServer().c_str());
      resolverIp = 0;
    } else {
      const std::string cached = TAILSCALE_STORE.getDnsCacheIp(host);
      const uint32_t cachedIp = cached.empty() ? 0 : microlink_parse_ip(cached.c_str());
      if (isCgnatIp(cachedIp)) {
        targetPeerIp = cachedIp;
        usingCachedIp = true;
        LOG_INF(TAG, "Using cached tailnet address %s for %s", cached.c_str(), host.c_str());
      } else {
        resolvePending = true;
        LOG_INF(TAG, "%s will be resolved through the tailnet via %s", host.c_str(),
                TAILSCALE_STORE.getDnsServer().c_str());
      }
    }
  }
  return true;
}

TailnetSession::Status TailnetSession::poll() {
  auto* handle = static_cast<microlink_t*>(ml);
  if (!handle) return Status::FAILED;
  if (microlink_is_connected(handle)) return Status::CONNECTED;
  const microlink_state_t state = microlink_get_state(handle);
  // Control answered the targeted map without the peer. Retrying cannot
  // change that answer, so fail now instead of spending the bring-up budget
  // on reconnect backoff.
  const char* absentError = microlink_get_last_error(handle);
  if (absentError && strncmp(absentError, "target peer absent", 18) == 0) {
    if (resolvePending) {
      // Phase 1 targets the resolver itself, so the missing peer is the DNS
      // server, not the OPDS host.
      setError(Error::RESOLVER_NOT_FOUND);
      errorDetail = "Tailnet DNS server " + TAILSCALE_STORE.getDnsServer() +
                    " is not in this device's netmap (check the ACL lets the reader reach it): " + absentError;
    } else {
      setError(Error::HOST_NOT_FOUND);
      errorDetail = "Tailnet peer for " + targetPeerHost + " is not in the netmap: " + absentError;
      if (targetPeerIp == 0) {
        errorDetail +=
            " (the host is not a MagicDNS peer name and did not resolve over Wi-Fi; put the server's tailnet IP "
            "in the OPDS URL, or configure a tailnet DNS server)";
      }
      if (usingCachedIp) {
        TAILSCALE_STORE.clearDnsCache();
        errorDetail += " (cached address dropped; retry to resolve it again)";
      }
    }
    LOG_ERR(TAG, "%s: %s", lastErrorCode(), errorDetail.c_str());
    char detail[240];
    snprintf(detail, sizeof(detail),
             "phase=%s split_dns_phase=%d microlink_state=%d target_host=%s target_ip_known=%d free_heap=%u "
             "max_alloc=%u",
             microlink_state_name(state), resolvePending ? 1 : (phaseTwo ? 2 : 0), static_cast<int>(state),
             targetPeerHost.c_str(), targetPeerIp != 0 ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    writeErrorLog(detail);
    teardown();
    return Status::FAILED;
  }
  if (microlink_state_is_terminal_error(state)) {
    setError(Error::CLIENT_ERROR);
    errorDetail = std::string("Tailscale failed while ") + microlink_state_name(state);
    const char* internalError = microlink_get_last_error(handle);
    if (internalError && strcmp(internalError, "none recorded") != 0) {
      errorDetail += ": ";
      errorDetail += internalError;
    }
    char detail[240];
    snprintf(detail, sizeof(detail), "microlink_state=%d free_heap=%u max_alloc=%u", static_cast<int>(state),
             ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    appendAllocationFailure(detail, sizeof(detail));
    writeErrorLog(detail);
    teardown();
    return Status::FAILED;
  }
  if (millis() - bringUpStartedAt >= bringUpTimeoutMs) {
    setError(Error::BRINGUP_TIMEOUT);
    errorDetail = std::string("Timed out while ") + microlink_state_name(state);
    const char* internalError = microlink_get_last_error(handle);
    if (internalError && strcmp(internalError, "none recorded") != 0) {
      errorDetail += ": ";
      errorDetail += internalError;
    }
    LOG_ERR(TAG, "%s: Tailnet startup timed out after %u ms in phase '%s' (MicroLink state=%d)", lastErrorCode(),
            bringUpTimeoutMs, microlink_state_name(state), static_cast<int>(state));
    char detail[240];
    snprintf(detail, sizeof(detail), "timeout_ms=%u phase=%s microlink_state=%d free_heap=%u max_alloc=%u",
             bringUpTimeoutMs, microlink_state_name(state), static_cast<int>(state), ESP.getFreeHeap(),
             ESP.getMaxAllocHeap());
    appendAllocationFailure(detail, sizeof(detail));
    writeErrorLog(detail);
    teardown();
    return Status::FAILED;
  }
  return statusFromState(microlink_get_state(handle));
}

bool TailnetSession::ensureUp(const ProgressCallback cb, void* ctx, const uint32_t timeoutMs) {
  if (isUp()) return true;
  if (!bringUpWarmOrCold(cb, ctx, timeoutMs)) return false;
  if (!resolvePending) return true;

  // Phase 1 done: the resolver peer is up. Resolve the hostname through the
  // tunnel, then rebuild the session around the real target. The second
  // bring-up costs a few seconds, but it keeps the map fetch and the data
  // plane from ever needing memory at the same time, which the C3 cannot
  // afford. Later sessions skip phase 1 through the cached answer.
  const bool resolved = resolveThroughTunnel();
  teardown();
  resolvePending = false;
  if (!resolved) {
    if (cb) cb(ctx, Status::FAILED);
    return false;
  }
  if (ml != nullptr) {
    setError(Error::CLIENT_ERROR);
    errorDetail = "Tailscale client could not be restarted after the DNS lookup";
    LOG_ERR(TAG, "%s: %s", lastErrorCode(), errorDetail.c_str());
    writeErrorLog("split_dns_phase=1 stop_deferred=1");
    if (cb) cb(ctx, Status::FAILED);
    return false;
  }
  phaseTwo = true;
  const bool up = bringUpWarmOrCold(cb, ctx, timeoutMs);
  phaseTwo = false;
  return up;
}

// Warm start when the previous cold session's netmap is cached and still
// plausible; on any warm failure fall back to the cold path within the same
// call, so callers never see the difference beyond the elapsed time.
bool TailnetSession::bringUpWarmOrCold(const ProgressCallback cb, void* ctx, const uint32_t timeoutMs) {
  const unsigned long started = millis();
  const char* reason = nullptr;
  uint32_t ageSeconds = 0;
  if (warmEligible(reason, ageSeconds)) {
    warmActive = true;
    LOG_INF(TAG, "Warm start from cached netmap age_s=%u self=%s region=%u", ageSeconds,
            TAILSCALE_STORE.getSelfIp().c_str(), TAILSCALE_STORE.getDerpRegion());
    const char* failure = nullptr;
    if (!bringUpAndWait(cb, ctx, std::min<uint32_t>(timeoutMs, WARM_BRINGUP_TIMEOUT_MS))) {
      failure = warmFailureReason();
    } else if (!warmVerifyPeers()) {
      failure = "no handshake response";
    }
    if (!failure) return true;
    LOG_ERR(TAG, "Warm start failed (%s); cold start", failure);
    // teardown() clears the cache when lastError is set; a handshake miss
    // leaves it NONE, so clear explicitly either way.
    teardown();
    TAILSCALE_STORE.clearNetmapCache();
    warmActive = false;
    warmFailed = true;
    if (ml != nullptr) {
      setError(Error::CLIENT_ERROR);
      errorDetail = "Tailscale client could not be restarted after the warm start failure";
      LOG_ERR(TAG, "%s: %s", lastErrorCode(), errorDetail.c_str());
      writeErrorLog("warm_start_failed=1 stop_deferred=1");
      if (cb) cb(ctx, Status::FAILED);
      return false;
    }
  } else {
    LOG_INF(TAG, "Cold start (%s)", reason);
  }
  const unsigned long elapsed = millis() - started;
  const uint32_t remaining = elapsed >= timeoutMs ? 1000u : std::max<uint32_t>(timeoutMs - elapsed, 1000u);
  return bringUpAndWait(cb, ctx, remaining);
}

bool TailnetSession::warmEligible(const char*& reason, uint32_t& ageSeconds) const {
  ageSeconds = 0;
  if (warmFailed) {
    reason = "previous warm failure";
    return false;
  }
  if (!TAILSCALE_STORE.hasNetmapCache()) {
    reason = "no cache";
    return false;
  }
  if (!isCgnatIp(microlink_parse_ip(TAILSCALE_STORE.getSelfIp().c_str())) || TAILSCALE_STORE.getDerpRegion() == 0) {
    reason = "poisoned";
    return false;
  }
  const uint32_t now = epochNow();
  const uint32_t cachedAt = TAILSCALE_STORE.getNetmapCachedAt();
  const uint32_t coldAt = TAILSCALE_STORE.getNetmapColdAt();
  // Age is only knowable when both ends were stamped with a real clock.
  if (cachedAt != 0 && now != 0) {
    if (now > coldAt && now - coldAt > NETMAP_MAX_AGE_S) {
      reason = "expired";
      return false;
    }
    ageSeconds = now > cachedAt ? now - cachedAt : 0;
  }
  // Phase 1 of a split-DNS session needs only the resolver; otherwise the
  // target by address (the NVS table has no usable names) plus the resolver
  // when one is configured.
  const uint32_t needed = resolvePending ? resolverIp : targetPeerIp;
  if (needed == 0) {
    reason = "target address unknown";
    return false;
  }
  if (!microlink_peer_cache_contains(needed)) {
    reason = resolvePending ? "resolver peer not cached" : "target peer not cached";
    return false;
  }
  if (!resolvePending && resolverIp != 0 && !microlink_peer_cache_contains(resolverIp)) {
    reason = "resolver peer not cached";
    return false;
  }
  return true;
}

// The session is up in the tunnel sense; prove the cache with a handshake.
// Both peers are initiated at once; only the primary one must answer (the
// resolver is a convenience the URL rewrite can do without).
bool TailnetSession::warmVerifyPeers() {
  auto* handle = static_cast<microlink_t*>(ml);
  if (!handle) return false;
  uint32_t ips[2];
  int count = 0;
  const uint32_t primary = resolvePending ? resolverIp : targetPeerIp;
  ips[count++] = primary;
  if (!resolvePending && resolverIp != 0) ips[count++] = resolverIp;
  const esp_err_t err = microlink_wait_peers_ready(handle, ips, count, WARM_PEER_READY_MS);
  if (err == ESP_OK) return true;
  if (!microlink_peer_is_up(handle, primary)) return false;
  LOG_INF(TAG, "Warm start: resolver peer did not answer within %u ms; continuing with the target", WARM_PEER_READY_MS);
  return true;
}

const char* TailnetSession::warmFailureReason() const {
  switch (lastError) {
    case Error::BRINGUP_TIMEOUT:
      return "timeout";
    case Error::CLIENT_ERROR:
      if (errorDetail.find("warm start") != std::string::npos) return "cached peer missing";
      if (errorDetail.find("readiness") != std::string::npos) return "no DERP admission";
      return "client error";
    case Error::START_FAILED:
    case Error::INIT_FAILED:
      return "start failed";
    default:
      return lastErrorCode();
  }
}

bool TailnetSession::poisonNetmapCache() {
  const uint32_t needed = resolvePending ? resolverIp : targetPeerIp;
  return needed != 0 && microlink_peer_cache_poison(needed);
}

// Called from teardown() of a cold session that reached CONNECTED without a
// TS-E error: what the next session needs to skip the control plane.
void TailnetSession::cacheNetmap() {
  auto* handle = static_cast<microlink_t*>(ml);
  if (!handle) return;
  const uint32_t selfIp = microlink_get_vpn_ip(handle);
  const uint16_t region = microlink_get_derp_region(handle);
  if (!isCgnatIp(selfIp) || region == 0) return;
  char ipStr[16];
  microlink_ip_to_str(selfIp, ipStr);
  const uint32_t now = epochNow();
  TAILSCALE_STORE.setNetmapCache(ipStr, region, now, now);
  LOG_INF(TAG, "Netmap cached self=%s region=%u peers=%u", ipStr, region,
          static_cast<unsigned>(microlink_get_peer_count(handle)));
}

bool TailnetSession::bringUpAndWait(const ProgressCallback cb, void* ctx, const uint32_t timeoutMs) {
  if (cb) cb(ctx, Status::STARTING);
  if (!startBringUp(timeoutMs)) {
    if (cb) cb(ctx, Status::FAILED);
    return false;
  }

  // Poll from the caller's task rather than registering microlink's state
  // callback: that callback fires on the coord task, and progress must reach
  // the UI on the activity's own task.
  Status lastReported = Status::STARTING;
  for (;;) {
    const Status now = poll();
    if (now == Status::CONNECTED) {
      sessionConnected = true;
      LOG_INF(TAG, "Tailnet up after %lu ms (free heap %u)%s", millis() - bringUpStartedAt, ESP.getFreeHeap(),
              warmActive ? " warm" : "");
      if (cb) cb(ctx, Status::CONNECTED);
      return true;
    }
    if (now == Status::FAILED) {
      if (cb) cb(ctx, Status::FAILED);
      return false;
    }
    if (cb && now != lastReported) {
      lastReported = now;
      cb(ctx, now);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

bool TailnetSession::isUp() const {
  auto* handle = static_cast<microlink_t*>(ml);
  return !stopDeferred && handle && microlink_is_connected(handle);
}

uint32_t TailnetSession::resolveHost(const char* host) {
  auto* handle = static_cast<microlink_t*>(ml);
  if (!handle || !host || host[0] == '\0') return 0;

  // Literal IP wins (also covers non-CGNAT IPs the caller typed directly)
  const uint32_t literal = microlink_parse_ip(host);
  if (literal != 0) return literal;

  return microlink_resolve(handle, host);
}

bool TailnetSession::waitForPeer(const uint32_t vpnIp, const uint32_t timeoutMs) {
  auto* handle = static_cast<microlink_t*>(ml);
  if (!handle) return false;
  const uint32_t budget = warmActive ? std::min(timeoutMs, WARM_PEER_READY_MS) : timeoutMs;
  return microlink_wait_peer_ready(handle, vpnIp, budget) == ESP_OK;
}

std::string TailnetSession::rewriteUrlForTailnet(const std::string& url) {
  setError(Error::NONE);
  // Host spans from after "://" to the first ':' or '/' (OPDS URLs carry
  // credentials in separate fields, so no userinfo handling needed).
  const size_t schemeEnd = url.find("://");
  const size_t hostStart = schemeEnd == std::string::npos ? 0 : schemeEnd + 3;
  const size_t hostEnd = url.find_first_of(":/", hostStart);
  const std::string host =
      url.substr(hostStart, hostEnd == std::string::npos ? std::string::npos : hostEnd - hostStart);
  if (host.empty()) {
    setError(Error::INVALID_URL);
    LOG_ERR(TAG, "%s: OPDS URL has no host: %s", lastErrorCode(), url.c_str());
    return std::string();
  }

  uint32_t ip = resolveHost(host.c_str());
  bool rewriteHost = true;
  auto* handle = static_cast<microlink_t*>(ml);
  const uint32_t peerBudget = warmActive ? WARM_PEER_READY_MS : PEER_READY_MS;

  if (ip == 0 && targetPeerIp != 0 && resolverIp != 0 && host == targetPeerHost) {
    // Resolved through the tailnet, in this session or a cached earlier one.
    // Keep the hostname in the URL for Host/SNI and point lwIP at the
    // resolver so the HTTP client's own lookup also goes through the tunnel.
    // Both handshakes are relayed round trips with nothing in common, so
    // they are initiated together rather than one after the other.
    ip = targetPeerIp;
    const uint32_t both[2] = {resolverIp, ip};
    microlink_wait_peers_ready(handle, both, 2, peerBudget);
    if (microlink_peer_is_up(handle, resolverIp)) {
      rewriteHost = false;
      applyDnsServer(resolverIp);
    } else {
      // Without the resolver the client cannot look the name up, so fall
      // back to the address (loses Host/SNI for name-routed proxies, but a
      // plain server still works). Leave the Wi-Fi DNS in place.
      LOG_ERR(TAG, "Tailnet DNS server peer did not come up; using %s's address in the URL instead", host.c_str());
      restoreDnsServer();
    }
  }

  if (ip == 0) {
    // Not a literal IP or MagicDNS peer name, so try ordinary DNS. Custom
    // domains (split-horizon or public records pointing at a tailnet
    // address) resolve here. When DNS answers with a CGNAT IP, keep the
    // hostname in the URL: lwIP routes 100.64.0.0/10 through the WG netif by
    // netmask, and preserving the name keeps the Host header and TLS SNI
    // correct for name-routed reverse proxies.
    IPAddress dnsIp;
    if (WiFi.hostByName(host.c_str(), dnsIp) == 1) {
      const uint32_t candidate = (static_cast<uint32_t>(dnsIp[0]) << 24) | (static_cast<uint32_t>(dnsIp[1]) << 16) |
                                 (static_cast<uint32_t>(dnsIp[2]) << 8) | static_cast<uint32_t>(dnsIp[3]);
      if (isCgnatIp(candidate)) {
        ip = candidate;
        rewriteHost = false;
      } else {
        setError(Error::HOST_NOT_TAILNET);
        LOG_ERR(TAG, "%s: %s resolves outside CGNAT/tailnet range (%s)", lastErrorCode(), host.c_str(),
                dnsIp.toString().c_str());
        return std::string();
      }
    }
  }

  if (ip == 0) {
    setError(Error::HOST_NOT_FOUND);
    LOG_ERR(TAG, "%s: host '%s' is absent from the netmap and ordinary DNS", lastErrorCode(), host.c_str());
    writeErrorLog(host.c_str());
    return std::string();
  }

  // Establish the WG session before handing the URL to an HTTP client; a
  // plain socket connect without one fails with EHOSTUNREACH. Returns
  // immediately when the tunnel is already up.
  const esp_err_t peerErr = microlink_wait_peer_ready(handle, ip, peerBudget);
  if (peerErr != ESP_OK) {
    setError(Error::PEER_TIMEOUT);
    if (usingCachedIp) {
      // The cached address may point at a node that no longer serves the
      // name; resolve again next time.
      TAILSCALE_STORE.clearDnsCache();
    }
    char ipStr[16];
    microlink_ip_to_str(ip, ipStr);
    LOG_ERR(TAG,
            "%s: WG handshake to %s (%s) failed: %s (0x%x). Device registration succeeded; check "
            "peer online state and UDP/DERP reachability",
            lastErrorCode(), host.c_str(), ipStr, esp_err_to_name(peerErr), static_cast<unsigned>(peerErr));
    const char* internalError = microlink_get_last_error(handle);
    char detail[256];
    snprintf(detail, sizeof(detail),
             "host=%s vpn_ip=%s esp_error=%s (0x%x) free_heap=%u max_alloc=%u microlink_error=%s", host.c_str(), ipStr,
             esp_err_to_name(peerErr), static_cast<unsigned>(peerErr), ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
             internalError);
    writeErrorLog(detail);
    return std::string();
  }

  if (!rewriteHost) return url;

  char ipStr[16];
  microlink_ip_to_str(ip, ipStr);

  std::string rewritten = url.substr(0, hostStart);
  rewritten += ipStr;
  if (hostEnd != std::string::npos) rewritten += url.substr(hostEnd);
  return rewritten;
}

bool TailnetSession::resolveThroughTunnel() {
  auto* handle = static_cast<microlink_t*>(ml);
  char resolverStr[16];
  microlink_ip_to_str(resolverIp, resolverStr);
  if (!waitForPeer(resolverIp, PEER_READY_MS)) {
    setError(Error::PEER_TIMEOUT);
    errorDetail = std::string("No WireGuard handshake response from the tailnet DNS server ") + resolverStr +
                  " (relayed via DERP; the peer may be offline, on another DERP region, or ACL-blocked)";
    LOG_ERR(TAG, "%s: %s", lastErrorCode(), errorDetail.c_str());
    char detail[224];
    snprintf(detail, sizeof(detail), "phase=resolver-peer resolver=%s free_heap=%u max_alloc=%u microlink_error=%s",
             resolverStr, ESP.getFreeHeap(), ESP.getMaxAllocHeap(), microlink_get_last_error(handle));
    writeErrorLog(detail);
    return false;
  }

  applyDnsServer(resolverIp);
  IPAddress dnsIp;
  const unsigned long started = millis();
  const int rc = WiFi.hostByName(targetPeerHost.c_str(), dnsIp);
  const uint32_t ip = rc == 1 ? (static_cast<uint32_t>(dnsIp[0]) << 24) | (static_cast<uint32_t>(dnsIp[1]) << 16) |
                                    (static_cast<uint32_t>(dnsIp[2]) << 8) | static_cast<uint32_t>(dnsIp[3])
                              : 0;
  if (!isCgnatIp(ip)) {
    setError(rc == 1 ? Error::HOST_NOT_TAILNET : Error::HOST_NOT_FOUND);
    errorDetail = std::string("Tailnet DNS server ") + resolverStr;
    if (rc == 1) {
      errorDetail += " maps " + targetPeerHost + " outside the tailnet (" + dnsIp.toString().c_str() + ")";
    } else {
      errorDetail += " has no address for " + targetPeerHost;
    }
    LOG_ERR(TAG, "%s: %s", lastErrorCode(), errorDetail.c_str());
    char detail[224];
    snprintf(detail, sizeof(detail), "phase=resolve host=%s resolver=%s dns_result=%d elapsed_ms=%lu free_heap=%u",
             targetPeerHost.c_str(), resolverStr, rc, millis() - started, ESP.getFreeHeap());
    writeErrorLog(detail);
    return false;
  }

  targetPeerIp = ip;
  char ipStr[16];
  microlink_ip_to_str(ip, ipStr);
  TAILSCALE_STORE.setDnsCache(targetPeerHost, ipStr);
  LOG_INF(TAG, "%s resolved to %s through the tailnet in %lu ms", targetPeerHost.c_str(), ipStr, millis() - started);
  return true;
}

// lwIP has one global resolver list; swap the main entry for the tailnet
// resolver while a session is up and put the Wi-Fi network's back afterwards.
namespace {
uint32_t readDns(esp_netif_t* sta, const esp_netif_dns_type_t type) {
  esp_netif_dns_info_t current = {};
  if (esp_netif_get_dns_info(sta, type, &current) != ESP_OK || current.ip.type != ESP_IPADDR_TYPE_V4) return 0;
  return ntohl(current.ip.u_addr.ip4.addr);
}

void writeDns(esp_netif_t* sta, const esp_netif_dns_type_t type, const uint32_t ip) {
  esp_netif_dns_info_t dns = {};
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = htonl(ip);
  if (esp_netif_set_dns_info(sta, type, &dns) != ESP_OK) LOG_ERR(TAG, "Could not set DNS server");
}

// Answers cached from the Wi-Fi resolver (including negative ones) must not
// satisfy lookups meant for the tailnet resolver, and vice versa.
void flushDnsCache() {
  LOCK_TCPIP_CORE();
  dns_clear_cache();
  UNLOCK_TCPIP_CORE();
}
}  // namespace

void TailnetSession::applyDnsServer(const uint32_t ip) {
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) return;
  if (!dnsOverridden) {
    savedDnsIp = readDns(sta, ESP_NETIF_DNS_MAIN);
    savedBackupDnsIp = readDns(sta, ESP_NETIF_DNS_BACKUP);
    dnsOverridden = true;
  }
  // Both slots: lwIP falls through to the backup resolver on timeout, and the
  // Wi-Fi one must not get a second chance at a tailnet-only name.
  writeDns(sta, ESP_NETIF_DNS_MAIN, ip);
  writeDns(sta, ESP_NETIF_DNS_BACKUP, ip);
  flushDnsCache();
}

void TailnetSession::restoreDnsServer() {
  if (!dnsOverridden) return;
  esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta) {
    if (savedDnsIp != 0) writeDns(sta, ESP_NETIF_DNS_MAIN, savedDnsIp);
    writeDns(sta, ESP_NETIF_DNS_BACKUP, savedBackupDnsIp);
    flushDnsCache();
  }
  dnsOverridden = false;
}

void TailnetSession::teardown() {
  restoreDnsServer();
  auto* handle = static_cast<microlink_t*>(ml);
  if (!handle) return;
  LOG_INF(TAG, "Tearing down tailnet session");
  microlink_log_stack_watermarks(handle);
  // Persist the relay region this session used (the target's home region)
  // so the next session, and phase 2 of this one, report it from the start.
  const uint16_t learnedRegion = microlink_get_derp_region(handle);
  if (learnedRegion != 0) TAILSCALE_STORE.setDerpRegion(learnedRegion);
  // A cold session that came up cleanly is what the next warm start reuses;
  // any TS-E failure means the cached state can no longer be trusted.
  if (lastError != Error::NONE) {
    TAILSCALE_STORE.clearNetmapCache();
  } else if (!warmActive && sessionConnected) {
    cacheNetmap();
  }
  warmActive = false;
  sessionConnected = false;
  const esp_err_t stopErr = microlink_stop(handle);
  if (stopErr != ESP_OK) {
    LOG_ERR(TAG, "Tailnet teardown deferred to reboot: %s (0x%x)", esp_err_to_name(stopErr),
            static_cast<unsigned>(stopErr));
    stopDeferred = true;
    return;
  }
  microlink_destroy(handle);
  ml = nullptr;
}

#else  // !CROSSPOINT_TAILNET: stubs so callers need no ifdefs

bool TailnetSession::ensureUp(const ProgressCallback cb, void* ctx, uint32_t) {
  setError(Error::NOT_COMPILED);
  LOG_ERR("TSN", "Tailnet support not compiled in (CROSSPOINT_TAILNET)");
  if (cb) cb(ctx, Status::FAILED);
  return false;
}

bool TailnetSession::isUp() const { return false; }

bool TailnetSession::setTargetUrl(const std::string&) { return false; }

uint32_t TailnetSession::resolveHost(const char*) { return 0; }

bool TailnetSession::waitForPeer(uint32_t, uint32_t) { return false; }

std::string TailnetSession::rewriteUrlForTailnet(const std::string&) { return std::string(); }

void TailnetSession::teardown() {}

bool TailnetSession::poisonNetmapCache() { return false; }

#endif  // CROSSPOINT_TAILNET
