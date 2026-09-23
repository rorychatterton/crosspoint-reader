#pragma once

#include <cstdint>
#include <string>

/**
 * On-demand Tailscale session for endpoints flagged with OpdsServer::useTailnet.
 *
 * Lifetime model: brought up lazily by the first tailnet fetch inside a network
 * activity, kept up for the whole browsing session, and destroyed by the
 * activity's exit teardown (which already reboots to defragment the heap).
 * The tunnel is never up outside these windows, so idle battery cost is zero.
 *
 * Callers must have WiFi connected first (the usual WifiSelectionActivity
 * flow). Traffic to 100.64.0.0/10 routes through the WireGuard netif
 * automatically once the session is up; MagicDNS names must be resolved via
 * resolveHost() because nothing hooks the system DNS.
 *
 * Compiled against the vendored MicroLink fork (lib/MicroLink). When the
 * CROSSPOINT_TAILNET build flag is absent, every method compiles to a cheap
 * failure so callers need no ifdefs.
 */
class TailnetSession {
 public:
  enum class Status : uint8_t {
    IDLE = 0,     // No session
    STARTING,     // microlink init/start issued
    REGISTERING,  // Talking to the control plane
    CONNECTED,    // Tunnel usable
    FAILED,       // Bring-up failed (see log); teardown() done
  };

  // Progress callback: plain fn pointer + context per repo std::function policy.
  // Invoked from the caller's own task inside ensureUp()'s poll loop.
  using ProgressCallback = void (*)(void* ctx, Status status);

  static TailnetSession& getInstance();

  /**
   * Bring the tailnet session up (no-op if already connected).
   * Blocks up to timeoutMs; reports coarse progress through cb.
   * Fails fast when no auth key is configured or free heap is below the
   * bring-up floor. On failure the session is fully torn down.
   */
  bool ensureUp(ProgressCallback cb = nullptr, void* ctx = nullptr, uint32_t timeoutMs = 75000);

  /**
   * Non-blocking bring-up for activities that must keep servicing their loop
   * (e.g. the web server) while registration runs: startBringUp() kicks the
   * session off (or returns false on the same fast-fail conditions as
   * ensureUp), then call poll() each loop tick until it reports CONNECTED or
   * FAILED. poll() tears the session down itself on timeout.
   */
  bool startBringUp(uint32_t timeoutMs = 60000);
  Status poll();

  bool isUp() const;

  /** True once microlink_stop() timed out in teardown(): the client tasks are
   * still alive, so the handle cannot be reused before the post-session
   * reboot and every later bring-up fails with TS-E11. Lets the self-test
   * report a deferred stop directly instead of inferring it. */
  bool isStopDeferred() const { return stopDeferred; }

  /** Stable, short diagnostic for the last failed tailnet operation. */
  const char* lastErrorCode() const;
  const char* lastErrorMessage() const;
  bool setTargetUrl(const std::string& url);

  /** True if a session was brought up at any point since boot. Activities use
   * this to force the restart-style teardown even on touch boards. */
  bool wasActive() const { return everActive; }

  /**
   * Resolve a tailnet host to a VPN IP (host byte order).
   * Accepts a literal 100.x.y.z IP, a MagicDNS short name, or a FQDN.
   * Returns 0 when unknown (peer not in the netmap yet).
   */
  uint32_t resolveHost(const char* host);

  /** Wait until the WireGuard tunnel to the peer is established. */
  bool waitForPeer(uint32_t vpnIp, uint32_t timeoutMs = 20000);

  /**
   * Make a URL reachable through the tunnel, waiting for the WG session to
   * the peer first. MagicDNS names and literal 100.x IPs get the host
   * rewritten to the VPN IP; custom domains that ordinary DNS maps to a
   * CGNAT address (e.g. calibre.lab.wayvz.io -> 100.x.y.z) are returned
   * unchanged, preserving the Host header and TLS SNI for name-routed
   * reverse proxies. Returns an empty string when the host cannot be
   * resolved to a tailnet address or the tunnel does not come up.
   */
  std::string rewriteUrlForTailnet(const std::string& url);

  /** Stop and free the session. Safe to call when not up. */
  void teardown();

  /** True while the current session was brought up from the cached netmap
   * (control plane skipped). */
  bool isWarmStart() const { return warmActive; }

  /**
   * Test hook: corrupt the cached public key of the peer the next session
   * needs (target, or the resolver in a split-DNS first phase) so a warm
   * start gets no handshake response and falls back to a cold start.
   * Returns false when nothing was cached to poison.
   */
  bool poisonNetmapCache();

  // Heap floor before attempting bring-up. The tunnel bring-up (~23 KB) and
  // the OPDS HTTPS fetch's wolfSSL session (~27 KB peak) run concurrently with
  // the DERP relay's wolfSSL session, so below this floor the session fails
  // fast (TS-E03) instead of starving mid-fetch and blocking the UI for the
  // HTTP timeout. A fresh boot leaves ~120 KB; the post-activity reboot
  // restores it.
  static constexpr uint32_t MIN_TAILNET_FREE_HEAP = 80000;
  static constexpr uint32_t MIN_TAILNET_MAX_ALLOC = 20000;

  // Warm start: the control phase (~5 s of a ~7 s page) is skipped when the
  // last cold session's self IP, relay region and peers are cached. Nothing
  // is verified against control, so the peer handshake gets a short budget;
  // a stale cache costs at most this before the cold path runs instead.
  static constexpr uint32_t WARM_PEER_READY_MS = 4000;
  static constexpr uint32_t WARM_BRINGUP_TIMEOUT_MS = 12000;
  static constexpr uint32_t PEER_READY_MS = 20000;
  static constexpr uint32_t NETMAP_MAX_AGE_S = 7 * 24 * 3600;

 private:
  TailnetSession() = default;

  enum class Error : uint8_t {
    NONE = 0,
    NOT_COMPILED,
    NO_AUTH_KEY,
    LOW_MEMORY,
    INIT_FAILED,
    START_FAILED,
    BRINGUP_TIMEOUT,
    INVALID_URL,
    HOST_NOT_FOUND,
    HOST_NOT_TAILNET,
    PEER_TIMEOUT,
    CLIENT_ERROR,
    RESOLVER_NOT_FOUND,
  };
  void setError(Error error);
  void writeErrorLog(const char* detail) const;
  void writeStartupLog() const;

  void* ml = nullptr;  // microlink_t*, opaque here to keep MicroLink headers out of ours
  Error lastError = Error::NONE;
  std::string errorDetail;
  bool everActive = false;
  // Non-blocking bring-up bookkeeping (startBringUp/poll)
  unsigned long bringUpStartedAt = 0;
  uint32_t bringUpTimeoutMs = 0;
  uint32_t targetPeerIp = 0;
  std::string targetPeerHost;
  // Split-DNS support: a resolver inside the tailnet (TailscaleStore
  // dnsServer). resolvePending marks the first phase of a session whose
  // hostname only that resolver knows: bring the resolver peer up alone,
  // ask it through the tunnel, then rebuild the session around the answer.
  uint32_t resolverIp = 0;
  bool resolvePending = false;
  bool usingCachedIp = false;
  bool dnsOverridden = false;
  uint32_t savedDnsIp = 0;
  uint32_t savedBackupDnsIp = 0;
  // microlink_stop() timed out: the client tasks are still alive, so the
  // handle must not be reused or destroyed until the post-session reboot.
  bool stopDeferred = false;
  // Second bring-up of a split-DNS session; keeps phase 1 in the SD log.
  bool phaseTwo = false;
  // Warm-start bookkeeping: warmActive marks a session built from the cache
  // (teardown must not re-cache it); warmFailed keeps the rest of this boot
  // cold after a warm failure; sessionConnected gates caching at teardown.
  bool warmActive = false;
  bool warmFailed = false;
  bool sessionConnected = false;
  bool bringUpAndWait(ProgressCallback cb, void* ctx, uint32_t timeoutMs);
  bool bringUpWarmOrCold(ProgressCallback cb, void* ctx, uint32_t timeoutMs);
  bool warmEligible(const char*& reason, uint32_t& ageSeconds) const;
  bool warmVerifyPeers();
  const char* warmFailureReason() const;
  void cacheNetmap();
  bool resolveThroughTunnel();
  void applyDnsServer(uint32_t ip);
  void restoreDnsServer();
};

#define TAILNET TailnetSession::getInstance()
