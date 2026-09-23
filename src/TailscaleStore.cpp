#include "TailscaleStore.h"

#include <Logging.h>
#include <ObfuscationUtils.h>

void TailscaleStore::toJson(JsonDocument& doc) const {
  doc["authKey_obf"] = obfuscation::obfuscateToBase64(authKey);
  doc["deviceName"] = deviceName;
  doc["controlHost"] = controlHost;
  doc["dnsServer"] = dnsServer;
  doc["dnsCacheHost"] = dnsCacheHost;
  doc["dnsCacheIp"] = dnsCacheIp;
  doc["derpRegion"] = derpRegion;
  doc["selfIp"] = selfIp;
  doc["netmapValid"] = netmapValid;
  doc["netmapCachedAt"] = netmapCachedAt;
  doc["netmapColdAt"] = netmapColdAt;
}

bool TailscaleStore::fromJson(JsonVariantConst doc) {
  const char* encoded = doc["authKey_obf"] | "";
  const char* plaintext = doc["authKey"] | "";
  if (encoded[0] != '\0') {
    bool ok = false;
    authKey = obfuscation::deobfuscateFromBase64(encoded, &ok);
    if (!ok) {
      // Obfuscation is keyed to the hardware MAC, so a config copied from
      // another device decodes to garbage; treat it as unset.
      LOG_ERR("TSS", "Failed to decode auth key (copied from another device?)");
      authKey.clear();
    }
  } else if (plaintext[0] != '\0') {
    // Seed path: a provisioning script can drop {"authKey":"tskey-..."} into
    // /.crosspoint/tailscale.json before first boot (obfuscation needs this
    // device's MAC, so it can't be pre-computed off-device). Adopt the key
    // and resave obfuscated, the same migration pattern as legacy plaintext
    // passwords in the other stores.
    authKey = plaintext;
    LOG_INF("TSS", "Adopting plaintext auth key seed; resaving obfuscated");
    requestResave();
  } else {
    authKey.clear();
  }

  deviceName = doc["deviceName"] | "";
  controlHost = doc["controlHost"] | "";
  dnsServer = doc["dnsServer"] | "";
  dnsCacheHost = doc["dnsCacheHost"] | "";
  dnsCacheIp = doc["dnsCacheIp"] | "";
  derpRegion = static_cast<uint16_t>(doc["derpRegion"] | 0);
  selfIp = doc["selfIp"] | "";
  netmapValid = doc["netmapValid"] | false;
  netmapCachedAt = doc["netmapCachedAt"] | 0u;
  netmapColdAt = doc["netmapColdAt"] | 0u;

#ifdef CROSSPOINT_TAILNET_AUTHKEY
  // Personal-build escape hatch: a key baked in via platformio.local.ini
  // (gitignored) covers first flash with a blank SD card. A key configured
  // at runtime always wins; the baked key is only the fallback.
  if (authKey.empty()) {
    authKey = CROSSPOINT_TAILNET_AUTHKEY;
    LOG_INF("TSS", "Using compile-time auth key fallback");
  }
#endif

  LOG_DBG("TSS", "Loaded tailscale config (authKey %s)", hasAuthKey() ? "set" : "not set");
  return true;
}

void TailscaleStore::setAuthKey(const std::string& key) {
  authKey = key;
  saveToFile();
}

void TailscaleStore::setDeviceName(const std::string& name) {
  deviceName = name;
  saveToFile();
}

void TailscaleStore::setControlHost(const std::string& host) {
  controlHost = host;
  saveToFile();
}

void TailscaleStore::setDnsServer(const std::string& server) {
  if (server == dnsServer) return;
  dnsServer = server;
  // A different resolver may answer differently.
  dnsCacheHost.clear();
  dnsCacheIp.clear();
  saveToFile();
}

void TailscaleStore::setDnsCache(const std::string& host, const std::string& ip) {
  dnsCacheHost = host;
  dnsCacheIp = ip;
  saveToFile();
}

void TailscaleStore::clearDnsCache() {
  if (dnsCacheHost.empty() && dnsCacheIp.empty()) return;
  dnsCacheHost.clear();
  dnsCacheIp.clear();
  saveToFile();
}

void TailscaleStore::setDerpRegion(const uint16_t region) {
  if (region == derpRegion) return;
  derpRegion = region;
  saveToFile();
}

void TailscaleStore::setNetmapCache(const std::string& ip, const uint16_t region, const uint32_t cachedAt,
                                    const uint32_t coldAt) {
  // Every cold session re-caches; with a 7-day staleness window, refreshing
  // unchanged values more than hourly is not worth the SD write.
  constexpr uint32_t REFRESH_S = 3600;
  const bool same = netmapValid && ip == selfIp && region == derpRegion;
  const bool fresh = cachedAt - netmapCachedAt < REFRESH_S && coldAt - netmapColdAt < REFRESH_S;
  if (same && fresh) return;
  selfIp = ip;
  derpRegion = region;
  netmapValid = true;
  netmapCachedAt = cachedAt;
  netmapColdAt = coldAt;
  saveToFile();
}

void TailscaleStore::clearNetmapCache() {
  if (!netmapValid && selfIp.empty() && netmapCachedAt == 0 && netmapColdAt == 0) return;
  selfIp.clear();
  netmapValid = false;
  netmapCachedAt = 0;
  netmapColdAt = 0;
  saveToFile();
}
