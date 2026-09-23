#include "TailnetSelfTest.h"

#if defined(CROSSPOINT_TAILNET_SELFTEST) && defined(CROSSPOINT_TAILNET)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderDocumentId.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_rom_md5.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <microlink.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "TailscaleStore.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"
#include "network/TailnetSession.h"
#include "SilentRestart.h"
#include "activities/browser/OpdsBookBrowserActivity.h"

// Build-flag defaults. Every one of them can be overridden per run from
// /.crosspoint/selftest.json (see loadConfig) without reflashing.
#ifndef CROSSPOINT_SELFTEST_TARGET
// Default: the lab tailnet node used for hardware runs.
// Override with -DCROSSPOINT_SELFTEST_TARGET=\"...\".
#define CROSSPOINT_SELFTEST_TARGET "100.93.9.14"
#endif
#ifndef CROSSPOINT_SELFTEST_FB_BYTES
#define CROSSPOINT_SELFTEST_FB_BYTES 48000
#endif
#ifndef CROSSPOINT_SELFTEST_BALLAST
#define CROSSPOINT_SELFTEST_BALLAST 0  // 0 = leave the heap as the boot left it
#endif

namespace {
constexpr char TAG[] = "SELFTEST";
constexpr char kConfigPath[] = "/.crosspoint/selftest.json";
constexpr char kSpool[] = "/.crosspoint/opds_feed.xml";
constexpr char kDownloadPath[] = "/.crosspoint/selftest_dl.bin";
// Throwaway KOSync document so the sync mode's writes never touch a real record.
constexpr char kSyntheticName[] = "crosspoint-selftest.epub";
// Light heap poisoning (CONFIG_HEAP_POISONING_LIGHT) prefixes every block with
// {uint32 canary, uint32 alloc_size}; heap_caps_walk reports the block from
// that header, not from the user pointer.
constexpr uint32_t kPoisonCanary = 0xABBA1234u;
constexpr size_t kPoisonHeadBytes = 8;

enum class Mode : uint8_t { FEED, DOWNLOAD, SYNC };

const char* modeName(const Mode m) {
  switch (m) {
    case Mode::DOWNLOAD:
      return "download";
    case Mode::SYNC:
      return "sync";
    default:
      return "feed";
  }
}

struct Config {
  uint32_t fbBytes = CROSSPOINT_SELFTEST_FB_BYTES;
  uint32_t ballastFree = CROSSPOINT_SELFTEST_BALLAST;  // bytes to leave free; 0 = no ballast
  uint32_t cycles = 1;
#ifdef CROSSPOINT_SELFTEST_SYNC
  Mode mode = Mode::SYNC;
#else
  Mode mode = Mode::FEED;
#endif
  uint32_t downloadSize = 0;  // expected byte count; 0 = don't check
  uint32_t interCycleDelayMs = 0;
  bool dumpSurvivors = false;  // dump region blocks even when the reclaim succeeds
  bool setTargetAfterRelease = false;
  // Corrupt the cached target peer key before each window so a warm start
  // fails its handshake and the warm-fail-then-cold path is exercised.
  bool poisonNetmapCache = false;
  const char* src = "flags";
  std::string downloadUrl;
  std::string downloadMd5;  // expected hex digest; empty = don't check
  std::string target = CROSSPOINT_SELFTEST_TARGET;
  // >= 0: after a passing feed cycle, write the browser resume sidecar with
  // this OpdsBookBrowserActivity::ResumeMode and reboot into the real UI.
  int resumeMode = -1;
};

// The modeled display framebuffer: one contiguous block held between windows,
// lent to the heap for each tunnel window, re-malloc'd after teardown. The
// ballast is never freed (it models the UI's other resident memory).
struct Model {
  void* fb = nullptr;
  uint32_t fbBytes = 0;
};

unsigned freeNow() { return static_cast<unsigned>(esp_get_free_heap_size()); }
unsigned largestNow() { return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); }

void loadConfig(Config& cfg) {
  JsonDocument doc;  // transient; parsed before the model exists, so nothing of it sits in the region
  if (!PersistableStoreBase::readDocFromFile(kConfigPath, doc)) return;
  cfg.src = "json";
  if (!doc["fbBytes"].isNull()) cfg.fbBytes = doc["fbBytes"].as<uint32_t>();
  if (!doc["ballastFree"].isNull()) cfg.ballastFree = doc["ballastFree"].as<uint32_t>();
  if (!doc["cycles"].isNull()) cfg.cycles = std::max<uint32_t>(1, doc["cycles"].as<uint32_t>());
  if (!doc["downloadSize"].isNull()) cfg.downloadSize = doc["downloadSize"].as<uint32_t>();
  if (!doc["interCycleDelayMs"].isNull()) cfg.interCycleDelayMs = doc["interCycleDelayMs"].as<uint32_t>();
  if (!doc["dumpSurvivors"].isNull()) cfg.dumpSurvivors = doc["dumpSurvivors"].as<bool>();
  if (!doc["setTargetAfterRelease"].isNull()) cfg.setTargetAfterRelease = doc["setTargetAfterRelease"].as<bool>();
  if (!doc["poisonNetmapCache"].isNull()) cfg.poisonNetmapCache = doc["poisonNetmapCache"].as<bool>();
  const char* mode = doc["mode"] | "";
  if (strcmp(mode, "download") == 0) {
    cfg.mode = Mode::DOWNLOAD;
  } else if (strcmp(mode, "sync") == 0) {
    cfg.mode = Mode::SYNC;
  } else if (strcmp(mode, "feed") == 0) {
    cfg.mode = Mode::FEED;
  } else if (strcmp(mode, "resume") == 0) {
    cfg.mode = Mode::FEED;  // one spooled fetch, then hand the spool to the real browser
    cfg.cycles = 1;
    if (cfg.resumeMode < 0) cfg.resumeMode = 1;
  } else if (mode[0]) {
    LOG_ERR(TAG, "SELFTEST config: unknown mode '%s'; keeping %s", mode, modeName(cfg.mode));
  }
  if (!doc["resumeMode"].isNull()) cfg.resumeMode = doc["resumeMode"].as<int>();
  cfg.downloadUrl = doc["downloadUrl"] | "";
  cfg.downloadMd5 = doc["downloadMd5"] | "";
  const char* target = doc["target"] | "";
  if (target[0]) cfg.target = target;
}

bool connectWifiHeadless() {
  WIFI_STORE.loadFromFile();
  WiFi.mode(WIFI_STA);
  const size_t count = WIFI_STORE.getCredentialCount();
  LOG_ERR(TAG, "SELFTEST wifi: %u saved credentials", (unsigned)count);
  for (size_t i = 0; i < count; i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred) continue;
    LOG_ERR(TAG, "SELFTEST wifi: trying %s", cred->ssid.c_str());
    WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    const unsigned long start = millis();
    while (millis() - start < 15000) {
      if (WiFi.status() == WL_CONNECTED) {
        LOG_ERR(TAG, "SELFTEST wifi: connected %s ip=%s", cred->ssid.c_str(), WiFi.localIP().toString().c_str());
        return true;
      }
      delay(200);
    }
    LOG_ERR(TAG, "SELFTEST wifi: %s failed", cred->ssid.c_str());
    WiFi.disconnect();
  }
  return false;
}

void allocateModel(const Config& cfg, Model& model) {
  // Model the display framebuffer that the real UI holds resident: a separate
  // ~48 KB block, freed to the heap right before each window (mimicking
  // OpdsBookBrowserActivity's releaseFrameBufferToHeap) and re-malloc'd after
  // (reacquireFrameBufferFromHeap). This exercises the exact "free 48 KB ->
  // concurrent DERP+OPDS TLS -> reclaim 48 KB contiguous" mechanic.
  model.fbBytes = cfg.fbBytes;
  if (cfg.fbBytes > 0) {
    model.fb = heap_caps_malloc(cfg.fbBytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    LOG_ERR(TAG, "SELFTEST framebuffer model=%u (%s) free_now=%u", (unsigned)cfg.fbBytes, model.fb ? "ok" : "FAILED",
            freeNow());
  }
  // Ballast models the UI's other resident memory (activity stack, fonts,
  // WiFi) so the test runs at the heap the real OPDS browse sees at fetch
  // entry, framebuffer still held, rather than the ~120 KB of a bare boot.
  // Measured after the framebuffer model above.
  if (cfg.ballastFree > 0) {
    const size_t target_free = cfg.ballastFree;
    const size_t have = esp_get_free_heap_size();
    if (have > target_free) {
      // Allocated in 8 KB pieces: a single large block is often unavailable
      // in the fragmented heap, and pieces also model scattered UI residents.
      const size_t reserve = have - target_free;
      size_t got = 0;
      while (got < reserve) {
        const size_t piece = std::min<size_t>(8192, reserve - got);
        if (!heap_caps_malloc(piece, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL)) break;
        got += piece;
      }
      LOG_ERR(TAG, "SELFTEST ballast reserve=%u got=%u free_now=%u (%s)", (unsigned)reserve, (unsigned)got, freeNow(),
              got == reserve ? "ok" : "PARTIAL");
    }
  }
}

// ---------------------------------------------------------------------------
// Owner tracking ([env:selftest_owner], CONFIG_HEAP_TASK_TRACKING=y). The heap
// stores the allocating task's handle as the first word of every block's user
// area (after the poison head). Handles are compared, never dereferenced: a
// survivor's owner may already be a deleted task.
#if CONFIG_HEAP_TASK_TRACKING
struct KnownTask {
  const char* name;
  TaskHandle_t handle;
};
// Boot-time residents first, then the tunnel's tasks (exist only while it is up).
KnownTask knownTasks[] = {{"tcpip_thread", nullptr}, {"wifi", nullptr},       {"loopTask", nullptr},
                          {"sys_evt", nullptr},      {"ml_coord", nullptr},   {"ml_derp_tx", nullptr},
                          {"ml_net_io", nullptr},    {"ml_udp_rx", nullptr},  {"ml_wg_mgr", nullptr}};
constexpr unsigned kBootTaskCount = 4;
constexpr unsigned kKnownTaskCount = sizeof(knownTasks) / sizeof(knownTasks[0]);

void captureTaskHandles(const unsigned from, const unsigned to, const char* label) {
  char line[200];
  int n = snprintf(line, sizeof(line), "SELFTEST task handles %s", label);
  for (unsigned i = from; i < to && n < (int)sizeof(line); i++) {
    knownTasks[i].handle = xTaskGetHandle(knownTasks[i].name);
    n += snprintf(line + n, sizeof(line) - n, " %s=%p", knownTasks[i].name, (void*)knownTasks[i].handle);
  }
  LOG_ERR(TAG, "%s", line);
}

const char* ownerName(const TaskHandle_t h) {
  if (!h) return "none";
  for (unsigned i = 0; i < kKnownTaskCount; i++)
    if (knownTasks[i].handle == h) return knownTasks[i].name;
  return "?";
}
#endif

// ---------------------------------------------------------------------------
// Region snapshot: (offset,size) of every used block inside [lo,hi). Static so
// the walker callback never allocates while the heap is locked.
constexpr unsigned kSnapMax = 400;
struct RegionSnap {
  uintptr_t lo, hi;
  unsigned n;
  struct {
    uint32_t off, size;
  } b[kSnapMax];
};
RegionSnap snapUp, snapFetch, snapDown;
void snapRegion(RegionSnap& s, uintptr_t lo, uintptr_t hi) {
  s.lo = lo;
  s.hi = hi;
  s.n = 0;
  heap_caps_walk(
      MALLOC_CAP_8BIT,
      [](walker_heap_into_t, walker_block_info_t blk, void* ud) {
        auto* w = static_cast<RegionSnap*>(ud);
        const uintptr_t a = reinterpret_cast<uintptr_t>(blk.ptr);
        if (blk.used && a >= w->lo && a < w->hi) {
          if (w->n < kSnapMax) w->b[w->n] = {static_cast<uint32_t>(a - w->lo), static_cast<uint32_t>(blk.size)};
          w->n++;
        }
        return true;
      },
      &s);
}
bool snapHas(const RegionSnap& s, uint32_t off, uint32_t size) {
  for (unsigned i = 0; i < s.n && i < kSnapMax; i++)
    if (s.b[i].off == off && s.b[i].size == size) return true;
  return false;
}

// Survivors = used blocks in the region after teardown. Tag each with the
// phase it first appeared in (U=tunnel bring-up, F=fetch, else teardown) and
// dump its user bytes so strings are recognisable. size= is the walker's gross
// block size (poison head/tail included); user= is the allocation's own size
// read from the poison head.
void dumpSurvivors(const uintptr_t lo, const uintptr_t hi) {
  LOG_ERR(TAG, "SELFTEST survivors=%u in region %p..%p (blocks up=%u fetch=%u)", snapDown.n, (void*)lo, (void*)hi,
          snapUp.n, snapFetch.n);
  for (unsigned i = 0; i < snapDown.n && i < kSnapMax; i++) {
    const uint32_t off = snapDown.b[i].off;
    const uint32_t size = snapDown.b[i].size;
    const char* phase = snapHas(snapUp, off, size) ? "U" : (snapHas(snapFetch, off, size) ? "F" : "T");
    const uint8_t* blk = reinterpret_cast<const uint8_t*>(lo + off);
    // memcpy: the RISC-V core traps on misaligned word loads and the walker
    // gives no alignment promise.
    uint32_t canary = 0, allocSize = 0;
    if (size >= kPoisonHeadBytes) {
      memcpy(&canary, blk, sizeof(canary));
      memcpy(&allocSize, blk + 4, sizeof(allocSize));
    }
    const bool poisoned = canary == kPoisonCanary;
    size_t skip = poisoned ? kPoisonHeadBytes : 0;
    uint32_t user = poisoned ? allocSize : 0;
#if CONFIG_HEAP_TASK_TRACKING
    TaskHandle_t owner = nullptr;
    if (poisoned && size >= kPoisonHeadBytes + sizeof(owner)) {
      memcpy(&owner, blk + kPoisonHeadBytes, sizeof(owner));
      skip += sizeof(owner);
      user = user >= sizeof(owner) ? user - (uint32_t)sizeof(owner) : 0;
    }
#endif
    char hex[3 * 32 + 1];
    char txt[33];
    const uint8_t* p = blk + skip;
    const size_t avail = size > skip ? size - skip : 0;
    const unsigned n = avail < 32 ? (unsigned)avail : 32;
    for (unsigned k = 0; k < n; k++) {
      snprintf(hex + 3 * k, 4, "%02x ", p[k]);
      txt[k] = (p[k] >= 32 && p[k] < 127) ? static_cast<char>(p[k]) : '.';
    }
    hex[3 * n] = 0;
    txt[n] = 0;
#if CONFIG_HEAP_TASK_TRACKING
    LOG_ERR(TAG, "SELFTEST survivor #%u %s off=%u size=%u user=%u task=%p(%s) |%s| %s", i + 1, phase, off, size, user,
            (void*)owner, ownerName(owner), txt, hex);
#else
    LOG_ERR(TAG, "SELFTEST survivor #%u %s off=%u size=%u user=%u |%s| %s", i + 1, phase, off, size, user, txt, hex);
#endif
  }
}

// ---------------------------------------------------------------------------
// One tunnel window: release the framebuffer model, bring the tunnel up,
// rewrite the URL; the caller runs its network op; closeWindow tears down,
// snapshots the region and reclaims the framebuffer.
struct Window {
  uintptr_t lo = 0, hi = 0;  // the freed framebuffer region (0,0 when the model is not held)
  bool released = false;
  bool up = false;     // ensureUp succeeded
  bool ready = false;  // and the URL rewrote
  std::string url;     // rewritten URL; freed by closeWindow before teardown
};

// Returns false after logging a RESULT=FAIL line for the stage that refused;
// the caller must still closeWindow() so a released framebuffer is reclaimed.
bool openWindow(const Config& cfg, Model& model, const std::string& url, Window& w) {
  // Target set BEFORE the release by default: the session keeps the host
  // string past teardown, and it must not sit inside the framebuffer's
  // region. setTargetAfterRelease=true sets it after the release instead, to
  // reproduce that fault.
  if (!cfg.setTargetAfterRelease) TAILNET.setTargetUrl(url);
  if (model.fb) {
    // Free the modeled framebuffer to the heap for the tunnel + fetch, exactly
    // as the UI does. From here the panel would show its last painted frame.
    w.lo = reinterpret_cast<uintptr_t>(model.fb);
    w.hi = w.lo + model.fbBytes;
    free(model.fb);
    model.fb = nullptr;
    w.released = true;
    LOG_ERR(TAG, "SELFTEST fb released free=%u largest=%u", freeNow(), largestNow());
  }
  if (cfg.setTargetAfterRelease) TAILNET.setTargetUrl(url);
  if (cfg.poisonNetmapCache && TAILNET.poisonNetmapCache()) LOG_ERR(TAG, "SELFTEST netmap cache poisoned");
  if (!TAILNET.ensureUp()) {
    LOG_ERR(TAG, "SELFTEST RESULT=FAIL stage=ensureUp code=%s msg=%s", TAILNET.lastErrorCode(),
            TAILNET.lastErrorMessage());
    return false;
  }
  w.up = true;
  w.url = TAILNET.rewriteUrlForTailnet(url);
  LOG_ERR(TAG, "SELFTEST tunnel_url=%s free=%u largest=%u", w.url.c_str(), freeNow(), largestNow());
#if CONFIG_HEAP_TASK_TRACKING
  captureTaskHandles(kBootTaskCount, kKnownTaskCount, "tunnel");
#endif
  snapRegion(snapUp, w.lo, w.hi);
  if (w.url.empty()) {
    LOG_ERR(TAG, "SELFTEST RESULT=FAIL stage=rewrite code=%s msg=%s", TAILNET.lastErrorCode(),
            TAILNET.lastErrorMessage());
    return false;
  }
  w.ready = true;
  return true;
}

struct CloseResult {
  bool reclaimed = true;  // trivially true when nothing was released
  bool deferred = false;
};

CloseResult closeWindow(const Config& cfg, Model& model, Window& w) {
  CloseResult r;
  std::string().swap(w.url);  // allocated inside the window; must not outlive it
  snapRegion(snapFetch, w.lo, w.hi);
  // The tunnel + a 48 KB framebuffer exceed the heap, so tear the tunnel
  // down BEFORE reclaiming the framebuffer (the UI does the same: fetch,
  // drop the tunnel, restore the display, parse the spool, render).
  if (!TAILNET.isStopDeferred()) TAILNET.teardown();
  LOG_ERR(TAG, "SELFTEST post-teardown free=%u largest=%u", freeNow(), largestNow());
  r.deferred = TAILNET.isStopDeferred();
  if (r.deferred) LOG_ERR(TAG, "SELFTEST stop deferred=1");
  snapRegion(snapDown, w.lo, w.hi);
  if (w.released) {
    // Retry with short delays: frees queued on the tcpip/WiFi tasks may
    // land after teardown returns.
    for (int attempt = 0; attempt < 10 && !model.fb; attempt++) {
      if (attempt) delay(200);
      model.fb = heap_caps_malloc(model.fbBytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
      LOG_ERR(TAG, "SELFTEST fb reclaim try=%d %s free=%u largest=%u", attempt, model.fb ? "OK" : "fail", freeNow(),
              largestNow());
    }
    r.reclaimed = (model.fb != nullptr);
    LOG_ERR(TAG, "SELFTEST fb reclaim=%s (%u bytes) free=%u largest=%u", r.reclaimed ? "OK" : "FAIL",
            (unsigned)model.fbBytes, freeNow(), largestNow());
    if (!r.reclaimed || cfg.dumpSurvivors) dumpSurvivors(w.lo, w.hi);
  }
  return r;
}

const OpdsServer* firstTailnetServer() {
  for (const auto& s : OPDS_STORE.getServers())
    if (s.useTailnet) return &s;
  return nullptr;
}

// Host part of scheme://host[:port]/..., lower-cased into out. False if none.
bool urlHost(const std::string& url, char* out, const size_t outLen) {
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) return false;
  const size_t start = scheme + 3;
  size_t end = url.find_first_of(":/?", start);
  if (end == std::string::npos) end = url.size();
  if (end <= start || end - start >= outLen) return false;
  for (size_t i = start; i < end; i++) out[i - start] = static_cast<char>(tolower(url[i]));
  out[end - start] = 0;
  return true;
}

// ---------------------------------------------------------------------------
// Feed mode: the real OPDS fetch over the tunnel (first tailnet-enabled OPDS
// server, full prepareTailnetUrl flow, HTTPS GET spooled to SD).
bool runFeedCycle(const Config& cfg, Model& model) {
  const OpdsServer* server = firstTailnetServer();
  if (!server) {
    LOG_ERR(TAG, "SELFTEST RESULT=FAIL stage=no-tailnet-opds-server");
    return false;
  }
  std::string url = server->url;
  if (url.find("/opds") == std::string::npos) {
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/opds";
  }
  LOG_ERR(TAG, "SELFTEST opds url=%s free=%u", url.c_str(), freeNow());
  // The feed is spooled to an SD temp file (mirrors the UI): the heap is too
  // tight to reserve a RAM body with the framebuffer held, and nothing from
  // the fetch may survive in the framebuffer's freed region. Opened BEFORE
  // the release so its bookkeeping sits outside that region.
  HalFile spool;
  const bool spoolOpen = Storage.openFileForWrite(TAG, kSpool, spool);
  LOG_ERR(TAG, "SELFTEST spool open=%s free=%u", spoolOpen ? "ok" : "FAILED", freeNow());
  size_t bodyBytes = 0;
  bool ok = false;
  Window w;
  if (openWindow(cfg, model, url, w)) {
    const unsigned long tf = millis();
    ok = spoolOpen && HttpDownloader::fetchUrl(
                          w.url,
                          [&spool, &bodyBytes](const uint8_t* data, size_t len) {
                            bodyBytes += len;
                            return spool.write(data, len) == len;
                          },
                          server->username, server->password);
    LOG_ERR(TAG, "SELFTEST fetch=%s bytes=%u after=%lums free=%u largest=%u", ok ? "OK" : "FAIL", (unsigned)bodyBytes,
            millis() - tf, freeNow(), largestNow());
  }
  spool.close();
  const CloseResult cr = closeWindow(cfg, model, w);
  if (!w.ready) {
    Storage.remove(kSpool);
    return false;  // RESULT=FAIL already logged by openWindow
  }
  // Read the spool back post-reclaim (the UI parses it here), then drop it.
  size_t readBack = 0;
  if (Storage.openFileForRead(TAG, kSpool, spool)) {
    uint8_t chunk[256];
    int n;
    while ((n = spool.read(chunk, sizeof(chunk))) > 0) readBack += (size_t)n;
    spool.close();
  }
  if (cfg.resumeMode < 0) Storage.remove(kSpool);  // resume mode hands the spool to the browser
  LOG_ERR(TAG, "SELFTEST spool readback=%u free=%u largest=%u", (unsigned)readBack, freeNow(), largestNow());
  const bool pass = ok && cr.reclaimed && readBack == bodyBytes;
  LOG_ERR(TAG, "SELFTEST RESULT=%s", pass ? "PASS" : "FAIL");
  return pass;
}

// ---------------------------------------------------------------------------
// Download mode: HttpDownloader::downloadToFile through the tunnel, then a
// byte-exact check (size + MD5) of what landed on SD, after the reclaim.
struct DlProgress {
  unsigned long start = 0;
  unsigned long lastLog = 0;
  size_t minFree = SIZE_MAX;
};

// MD5 of a file in 4 KB chunks. The buffer is a transient heap block (4 KB is
// far over the 256-byte stack budget); it is freed before the next window.
bool md5File(const char* path, size_t& bytes, char* hexOut /* 33 bytes */) {
  bytes = 0;
  hexOut[0] = 0;
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  auto buf = makeUniqueNoThrow<uint8_t[]>(4096);
  if (!buf) {
    f.close();
    LOG_ERR(TAG, "SELFTEST md5: OOM for 4 KB read buffer");
    return false;
  }
  md5_context_t ctx;
  esp_rom_md5_init(&ctx);
  int n;
  while ((n = f.read(buf.get(), 4096)) > 0) {
    esp_rom_md5_update(&ctx, buf.get(), static_cast<uint32_t>(n));
    bytes += static_cast<size_t>(n);
  }
  f.close();
  uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
  esp_rom_md5_final(digest, &ctx);
  for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; i++) snprintf(hexOut + 2 * i, 3, "%02x", digest[i]);
  return true;
}

bool runDownloadCycle(const Config& cfg, Model& model, const unsigned rep) {
  if (cfg.downloadUrl.empty()) {
    LOG_ERR(TAG, "SELFTEST RESULT=FAIL stage=config msg=downloadUrl missing (selftest.json)");
    return false;
  }
  // Reuse the OPDS store's tailnet credentials when the download is served by
  // that host (Calibre); the plain http.server / self-signed variants need none.
  std::string authUser, authPass;
  const OpdsServer* server = firstTailnetServer();
  char hostA[80], hostB[80];
  if (server && urlHost(server->url, hostA, sizeof(hostA)) && urlHost(cfg.downloadUrl, hostB, sizeof(hostB)) &&
      strcmp(hostA, hostB) == 0) {
    authUser = server->username;
    authPass = server->password;
  }
  const std::string dest = kDownloadPath;  // built before the release: outside the region
  LOG_ERR(TAG, "SELFTEST dl url=%s auth=%d expect=%u expect_md5=%s free=%u", cfg.downloadUrl.c_str(), !authUser.empty(),
          (unsigned)cfg.downloadSize, cfg.downloadMd5.empty() ? "-" : cfg.downloadMd5.c_str(), freeNow());
  if (Storage.exists(kDownloadPath)) Storage.remove(kDownloadPath);

  DlProgress p;
  int result = HttpDownloader::HTTP_ERROR;
  unsigned secs = 0;
  size_t downloaded = 0;
  Window w;
  if (openWindow(cfg, model, cfg.downloadUrl, w)) {
    p.start = p.lastLog = millis();
    p.minFree = esp_get_free_heap_size();
    // One reference captured: fits std::function's local storage, no heap.
    result = HttpDownloader::downloadToFile(
        w.url, dest,
        [&p](size_t done, size_t total) {
          const size_t f = esp_get_free_heap_size();
          if (f < p.minFree) p.minFree = f;
          const unsigned long now = millis();
          if (now - p.lastLog >= 10000) {
            p.lastLog = now;
            const unsigned s = (now - p.start) / 1000;
            LOG_ERR(TAG, "SELFTEST dl progress=%u/%u secs=%u kbps=%u free=%u largest=%u", (unsigned)done,
                    (unsigned)total, s, s ? (unsigned)(done / 1024 / s) : 0, (unsigned)f, largestNow());
          }
        },
        nullptr, authUser, authPass);
    secs = (millis() - p.start) / 1000;
    LOG_ERR(TAG, "SELFTEST dl done result=%d secs=%u minfree=%u free=%u largest=%u", result, secs,
            (unsigned)p.minFree, freeNow(), largestNow());
  }
  const CloseResult cr = closeWindow(cfg, model, w);
  if (!w.ready) return false;  // RESULT=FAIL already logged by openWindow

  char md5[33];
  const bool haveFile = md5File(kDownloadPath, downloaded, md5);
  if (!haveFile) snprintf(md5, sizeof(md5), "-");
  const bool sizeOk = haveFile && (cfg.downloadSize == 0 || downloaded == cfg.downloadSize);
  const bool fileOk = haveFile && (cfg.downloadMd5.empty() || strcasecmp(md5, cfg.downloadMd5.c_str()) == 0);
  LOG_ERR(TAG,
          "SELFTEST dl rep=%u result=%d bytes=%u expect=%u secs=%u kbps=%u file_md5=%s expect_md5=%s size_ok=%d "
          "file_ok=%d minfree=%u largest=%u",
          rep, result, (unsigned)downloaded, (unsigned)cfg.downloadSize, secs,
          secs ? (unsigned)(downloaded / 1024 / secs) : 0, md5, cfg.downloadMd5.empty() ? "-" : cfg.downloadMd5.c_str(),
          sizeOk, fileOk, (unsigned)p.minFree, largestNow());
  Storage.remove(kDownloadPath);
  const bool pass = result == HttpDownloader::OK && sizeOk && fileOk && cr.reclaimed;
  LOG_ERR(TAG, "SELFTEST RESULT=%s mode=download rep=%u", pass ? "PASS" : "FAIL", rep);
  return pass;
}

// ---------------------------------------------------------------------------
// Sync mode: two tunnel windows per cycle. A reads the real book's
// primary/alternate records and the synthetic baseline; B (the second
// bring-up, which is what catches a deferred stop) writes the synthetic
// record and reads it back.
bool isNetErr(const KOReaderSyncClient::Error e) {
  return e == KOReaderSyncClient::LOW_MEMORY || e == KOReaderSyncClient::NETWORK_ERROR ||
         e == KOReaderSyncClient::TAILNET_ERROR;
}

std::string hashFor(const std::string& path, const DocumentMatchMethod m) {
  return m == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                            : KOReaderDocumentId::calculate(path);
}

bool runSyncCycle(const Config& cfg, Model& model, const unsigned iter) {
  const bool creds = KOREADER_STORE.hasCredentials();
  std::string baseUrl = KOREADER_STORE.getBaseUrl();
  LOG_ERR(TAG, "SELFTEST sync creds=%d useTailnet=%d authKey=%d base=%s free=%u", creds,
          KOREADER_STORE.getUseTailnet(), TAILSCALE_STORE.hasAuthKey(), baseUrl.c_str(), freeNow());
  if (!creds) {
    LOG_ERR(TAG, "SELFTEST RESULT=FAIL stage=sync-credentials");
    return false;
  }
  // Book: the open EPUB, else the most recent, else a build flag. Both hashes
  // are computed here, before the release, because they read the EPUB from SD.
  std::string book = APP_STATE.openEpubPath;
  if (book.empty() && RECENT_BOOKS.getCount() > 0) book = RECENT_BOOKS.getBooks().front().path;
#ifdef CROSSPOINT_SELFTEST_EPUB
  if (book.empty()) book = CROSSPOINT_SELFTEST_EPUB;
#endif
  std::string primary, alt;
  if (!book.empty()) {
    const DocumentMatchMethod pm = KOREADER_STORE.getMatchMethod();
    primary = hashFor(book, pm);
    alt = hashFor(book, pm == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME);
  }
  const std::string synthetic = KOReaderDocumentId::calculateFromFilename(kSyntheticName);
  LOG_ERR(TAG, "SELFTEST sync book=%s primary=%s alt=%s synthetic=%s free=%u", book.empty() ? "-" : book.c_str(),
          primary.empty() ? "-" : primary.c_str(), alt.empty() ? "-" : alt.c_str(), synthetic.c_str(), freeNow());
  // The client keeps the base-URL override in a static std::string and
  // clearBaseUrlOverride() only clear()s it (capacity kept). Reserve that slot
  // here, outside the region, so the rewritten URL copied into it inside the
  // window does not become a survivor. (The UI's overTailnet has the same
  // hazard; this only keeps the self-test honest about the tunnel's own blocks.)
  KOReaderSyncClient::setBaseUrlOverride(std::string(baseUrl.size() + 64, ' '));
  KOReaderSyncClient::clearBaseUrlOverride();

  char xpath[64];
  snprintf(xpath, sizeof(xpath), "/body/DocFragment[3]/body/p[7]/text().%u", iter);
  const float pct = 0.10f + 0.01f * static_cast<float>(iter);
  bool netErr = false;

  // Window A: reads only.
  Window a;
  if (openWindow(cfg, model, baseUrl, a)) {
    KOReaderSyncClient::setBaseUrlOverride(a.url);
    auto prog = makeUniqueNoThrow<KOReaderProgress>();  // transient, freed before teardown
    if (!prog) {
      LOG_ERR(TAG, "SELFTEST sync A: OOM for progress record");
      netErr = true;
    } else {
      if (!primary.empty()) {
        const auto e = KOReaderSyncClient::getProgress(primary, *prog);
        LOG_ERR(TAG, "SELFTEST sync A primary=%d http=%d pct=%.4f", e, KOReaderSyncClient::lastHttpCode,
                e == KOReaderSyncClient::OK ? prog->percentage : -1.0f);
        netErr |= isNetErr(e);
        *prog = KOReaderProgress{};
      }
      if (!alt.empty()) {
        const auto e = KOReaderSyncClient::getProgress(alt, *prog);
        LOG_ERR(TAG, "SELFTEST sync A alt=%d http=%d pct=%.4f", e, KOReaderSyncClient::lastHttpCode,
                e == KOReaderSyncClient::OK ? prog->percentage : -1.0f);
        netErr |= isNetErr(e);
        *prog = KOReaderProgress{};
      }
      const auto e = KOReaderSyncClient::getProgress(synthetic, *prog);
      LOG_ERR(TAG, "SELFTEST sync A synthetic=%d http=%d pct=%.4f free=%u largest=%u", e,
              KOReaderSyncClient::lastHttpCode, e == KOReaderSyncClient::OK ? prog->percentage : -1.0f, freeNow(),
              largestNow());
      netErr |= isNetErr(e);
    }
    KOReaderSyncClient::clearBaseUrlOverride();
  }
  const CloseResult ca = closeWindow(cfg, model, a);

  // Window B: write the synthetic record, read it back, assert.
  Window b;
  CloseResult cb;
  bool rt = false;
  if (ca.deferred) {
    LOG_ERR(TAG, "SELFTEST sync B skipped: stop deferred after window A");
  } else {
    if (openWindow(cfg, model, baseUrl, b)) {
      KOReaderSyncClient::setBaseUrlOverride(b.url);
      auto up = makeUniqueNoThrow<KOReaderProgress>();
      auto got = makeUniqueNoThrow<KOReaderProgress>();
      if (!up || !got) {
        LOG_ERR(TAG, "SELFTEST sync B: OOM for progress records");
        netErr = true;
      } else {
        up->document = synthetic;
        up->progress = xpath;
        up->percentage = pct;
        const auto eu = KOReaderSyncClient::updateProgress(*up);
        const int httpU = KOReaderSyncClient::lastHttpCode;
        LOG_ERR(TAG, "SELFTEST sync B update=%d http=%d pct=%.4f free=%u largest=%u", eu, httpU, pct, freeNow(),
                largestNow());
        const auto eg = KOReaderSyncClient::getProgress(synthetic, *got);
        const bool okG = eg == KOReaderSyncClient::OK;
        const bool pctOk = okG && fabsf(got->percentage - pct) < 0.0005f;
        const bool xpathOk = okG && got->progress == xpath;
        // "CrossPoint" is KOReaderSyncClient's DEVICE_NAME (file-local there).
        const bool deviceOk = okG && got->device == "CrossPoint";
        const bool tsOk = okG && got->timestamp > 0;
        rt = eu == KOReaderSyncClient::OK && pctOk && xpathOk && deviceOk && tsOk;
        LOG_ERR(TAG, "SELFTEST sync B get=%d http=%d pct=%.4f pct_ok=%d xpath_ok=%d device_ok=%d ts=%ld rt=%d", eg,
                KOReaderSyncClient::lastHttpCode, okG ? got->percentage : -1.0f, pctOk, xpathOk, deviceOk,
                okG ? (long)got->timestamp : 0L, rt);
        netErr |= isNetErr(eu) || isNetErr(eg);
      }
      KOReaderSyncClient::clearBaseUrlOverride();
    }
    cb = closeWindow(cfg, model, b);
  }
  const bool pass = a.ready && b.ready && rt && !netErr && ca.reclaimed && cb.reclaimed && !ca.deferred && !cb.deferred;
  LOG_ERR(TAG, "SELFTEST RESULT=%s mode=sync iters=%u upA=%d upB=%d rt=%d reclaimA=%d reclaimB=%d neterr=%d deferred=%d",
          pass ? "PASS" : "FAIL", iter + 1, a.up, b.up, rt, ca.reclaimed, cb.reclaimed, netErr,
          ca.deferred || cb.deferred);
  return pass;
}

bool runCycle(const Config& cfg, Model& model, const unsigned idx) {
  switch (cfg.mode) {
    case Mode::DOWNLOAD:
      return runDownloadCycle(cfg, model, idx + 1);
    case Mode::SYNC:
      return runSyncCycle(cfg, model, idx);
    default:
      return runFeedCycle(cfg, model);
  }
}

void logBootSummary(const Config& cfg, const unsigned pass, const unsigned fail, const int firstFail,
                    const bool deferred) {
  LOG_ERR(TAG, "SELFTEST BOOT_SUMMARY cycles=%u pass=%u fail=%u first_fail=%d deferred_stop=%d", (unsigned)cfg.cycles,
          pass, fail, firstFail, deferred);
}

void delaySeconds(const uint32_t ms) {
  // 1 s slices so the idle task (and its watchdog) keep running during long
  // inter-cycle waits (feed_cycles5_wait uses 130 s = 2 x TCP MSL).
  for (uint32_t left = ms; left > 0;) {
    const uint32_t step = std::min<uint32_t>(1000, left);
    delay(step);
    left -= step;
  }
}
}  // namespace

void runTailnetSelfTest() {
  // Hop 2 of a resume-mode run: the RTC target is the OPDS browser, so let
  // setup() bring the display up and route into the real activity.
  if (tailnetRebootAttemptCount() > 0) {
    LOG_ERR(TAG, "SELFTEST resume hop=2 attempt=%u: handing off to the UI", (unsigned)tailnetRebootAttemptCount());
    return;
  }
  Config cfg;
  loadConfig(cfg);
  LOG_ERR(TAG, "========== SELFTEST begin target=%s ==========", cfg.target.c_str());
  LOG_ERR(TAG, "SELFTEST config fb=%u ballast=%u cycles=%u mode=%s src=%s", (unsigned)cfg.fbBytes,
          (unsigned)cfg.ballastFree, (unsigned)cfg.cycles, modeName(cfg.mode), cfg.src);
  LOG_ERR(TAG, "SELFTEST config-extra url=%s size=%u md5=%s delay=%u dump=%d target_after_release=%d poison=%d",
          cfg.downloadUrl.empty() ? "-" : cfg.downloadUrl.c_str(), (unsigned)cfg.downloadSize,
          cfg.downloadMd5.empty() ? "-" : cfg.downloadMd5.c_str(), (unsigned)cfg.interCycleDelayMs, cfg.dumpSurvivors,
          cfg.setTargetAfterRelease, cfg.poisonNetmapCache);
  // A crashed download rep leaves its file behind; never let it accumulate.
  if (Storage.exists(kDownloadPath)) {
    Storage.remove(kDownloadPath);
    LOG_ERR(TAG, "SELFTEST removed stale %s", kDownloadPath);
  }
  if (!connectWifiHeadless()) {
    LOG_ERR(TAG, "SELFTEST RESULT=FAIL stage=wifi; rebooting in 15s");
    delaySeconds(15000);
    esp_restart();
  }
#if CONFIG_HEAP_TASK_TRACKING
  captureTaskHandles(0, kBootTaskCount, "boot");
#endif
  Model model;
  allocateModel(cfg, model);

  unsigned pass = 0, fail = 0;
  int firstFail = 0;  // 1-based cycle of the first FAIL; 0 = none
  bool deferred = false;
  for (unsigned i = 0; i < cfg.cycles; i++) {
    LOG_ERR(TAG, "SELFTEST cycle=%u/%u begin free=%u largest=%u", i + 1, (unsigned)cfg.cycles, freeNow(),
            largestNow());
    // A failed cycle (refused bring-up, failed reclaim, ...) does not end the
    // boot: the next cycle runs in whatever state this one left, which is the
    // fragmented state the UI reaches.
    if (runCycle(cfg, model, i)) {
      pass++;
    } else {
      fail++;
      if (!firstFail) firstFail = static_cast<int>(i + 1);
    }
    if (TAILNET.isStopDeferred()) {
      // The client tasks are still alive; the handle cannot be reused until
      // the reboot, so the remaining cycles are skipped (neither pass nor fail).
      deferred = true;
      LOG_ERR(TAG, "SELFTEST stop deferred; skipping %u remaining cycles", (unsigned)(cfg.cycles - i - 1));
      break;
    }
    if (cfg.interCycleDelayMs && i + 1 < cfg.cycles) {
      LOG_ERR(TAG, "SELFTEST inter-cycle delay=%u ms free=%u largest=%u", (unsigned)cfg.interCycleDelayMs, freeNow(),
              largestNow());
      delaySeconds(cfg.interCycleDelayMs);
    }
  }
  logBootSummary(cfg, pass, fail, firstFail, deferred);
  if (!TAILNET.isStopDeferred()) TAILNET.teardown();
  if (cfg.resumeMode >= 0) {
    // Hop 1: carry the spool (mode 1) or a pending/failed fetch (2/0) into the
    // real browser exactly as rebootIntoBrowse() does, without painting.
    const auto mode = static_cast<OpdsBookBrowserActivity::ResumeMode>(cfg.resumeMode);
    if (mode != OpdsBookBrowserActivity::ResumeMode::SPOOLED && Storage.exists(kSpool)) Storage.remove(kSpool);
    const bool sidecar = OpdsBookBrowserActivity::writeResumeSidecar(mode, "", {});
    uint32_t index = 0;
    const auto& servers = OPDS_STORE.getServers();
    for (size_t i = 0; i < servers.size(); i++) {
      if (servers[i].useTailnet) {
        index = static_cast<uint32_t>(i);
        break;
      }
    }
    LOG_ERR(TAG, "SELFTEST resume hop=1 mode=%u idx=%u sidecar=%d spool=%d", (unsigned)cfg.resumeMode,
            (unsigned)index, sidecar ? 1 : 0, Storage.exists(kSpool) ? 1 : 0);
    delaySeconds(500);
    silentRestartToOpds(index, /*paint=*/false);
    for (;;) delay(1000);
  }
  LOG_ERR(TAG, "SELFTEST done; rebooting in 10s");
  delaySeconds(10000);
  esp_restart();
}

#else
void runTailnetSelfTest() {}
#endif
