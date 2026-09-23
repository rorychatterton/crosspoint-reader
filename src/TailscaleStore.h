#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <string>

/**
 * Singleton store for Tailscale (MicroLink) tailnet configuration on the SD
 * card. The auth key is XOR-obfuscated with the device's unique hardware MAC
 * address and base64-encoded before writing to JSON (not cryptographically
 * secure, but prevents casual reading and ties it to the specific device).
 * Use a reusable, tag-scoped key with a minimal ACL. The node identity keys
 * MicroLink derives during registration live in ESP NVS, not on SD.
 */
class TailscaleStore : public PersistableStore<TailscaleStore> {
 private:
  std::string authKey;      // Plaintext in memory; obfuscated with hardware key on disk
  std::string deviceName;   // Tailnet hostname; empty = MicroLink auto-generates from MAC
  std::string controlHost;  // Coordination server; empty = standard Tailscale control plane
  std::string dnsServer;    // Tailnet DNS resolver (100.x.y.z) for split-DNS hostnames; empty = none
  // Last split-DNS answer, so later sessions skip the resolver-only phase.
  std::string dnsCacheHost;
  std::string dnsCacheIp;
  // DERP region the last session relayed through (the book server's home
  // region). Reported to control as PreferredDERP from the first request of
  // the next session so peers reply via a relay we are connected to.
  uint16_t derpRegion = 0;
  // Control-plane state cached by the last cold session so the next one can
  // skip control entirely (warm start): our VPN IP, with derpRegion above.
  // netmapCachedAt/netmapColdAt are epoch seconds, 0 when the clock was unset;
  // the peers themselves live in MicroLink's NVS table.
  std::string selfIp;
  bool netmapValid = false;
  uint32_t netmapCachedAt = 0;
  uint32_t netmapColdAt = 0;

  // A compile-time key (personal builds, via gitignored platformio.local.ini)
  // seeds the in-memory default so it works even with no tailscale.json on
  // the SD card; fromJson() re-applies it when a config file exists but
  // carries no key of its own.
  TailscaleStore() {
#ifdef CROSSPOINT_TAILNET_AUTHKEY
    authKey = CROSSPOINT_TAILNET_AUTHKEY;
#endif
  }
  ~TailscaleStore() = default;

  friend class PersistableStore<TailscaleStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/tailscale.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // A tailnet session can only be brought up once an auth key is configured
  // (needed for first registration; later sessions reuse NVS node keys, but
  // MicroLink still requires the key to be present for re-registration on
  // key expiry).
  bool hasAuthKey() const { return !authKey.empty(); }

  void setAuthKey(const std::string& key);
  const std::string& getAuthKey() const { return authKey; }

  void setDeviceName(const std::string& name);
  const std::string& getDeviceName() const { return deviceName; }

  void setControlHost(const std::string& host);
  const std::string& getControlHost() const { return controlHost; }

  void setDnsServer(const std::string& server);
  const std::string& getDnsServer() const { return dnsServer; }

  // Cached split-DNS answer for host, or empty when none/other host.
  std::string getDnsCacheIp(const std::string& host) const { return host == dnsCacheHost ? dnsCacheIp : ""; }
  void setDnsCache(const std::string& host, const std::string& ip);
  void clearDnsCache();

  uint16_t getDerpRegion() const { return derpRegion; }
  void setDerpRegion(uint16_t region);

  // Warm-start cache. Validity of the values themselves (CGNAT self IP,
  // non-zero region, age) is judged by TailnetSession.
  bool hasNetmapCache() const { return netmapValid; }
  const std::string& getSelfIp() const { return selfIp; }
  uint32_t getNetmapCachedAt() const { return netmapCachedAt; }
  uint32_t getNetmapColdAt() const { return netmapColdAt; }
  void setNetmapCache(const std::string& ip, uint16_t region, uint32_t cachedAt, uint32_t coldAt);
  void clearNetmapCache();
};

#define TAILSCALE_STORE TailscaleStore::getInstance()
