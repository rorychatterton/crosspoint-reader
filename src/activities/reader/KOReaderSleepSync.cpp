#include "KOReaderSleepSync.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <memory>

#include "KOReaderCredentialStore.h"
#include "KOReaderProgressPayload.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "SyncDecision.h"
#include "activities/RenderLock.h"
#include "network/TailnetSession.h"

namespace kosync_sleep {
namespace {
// Key of the last position handled, so a sleep without reading stays offline.
constexpr char LAST_PUSH_FILE[] = "/.crosspoint/kosync_sleep.txt";
// Outcome of the last attempt, for diagnosis without a serial console.
constexpr char RESULT_LOG_FILE[] = "/kosync-sleep.log";
constexpr uint32_t TAILNET_TIMEOUT_MS = 30000;

std::string capturedPath;
CrossPointPosition capturedPosition{};
bool captured = false;

// Spine and page identify the position before the Epub is mapped.
std::string positionKey() {
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "\n%d %d", capturedPosition.spineIndex, capturedPosition.pageNumber);
  return capturedPath + suffix;
}

enum class Outcome { Pushed, RemoteAhead, Failed };

// Reads the record first so a stale position never overwrites one another
// device synced further on.
Outcome upload(const KOReaderProgress& payload, KOReaderSyncClient::Error& error) {
  KOReaderProgress remote{};
  error = KOReaderSyncClient::getProgress(payload.document, remote);
  const auto lookup = error == KOReaderSyncClient::OK          ? koreader_sync::Lookup::Found
                      : error == KOReaderSyncClient::NOT_FOUND ? koreader_sync::Lookup::NotFound
                                                               : koreader_sync::Lookup::Failed;
  switch (koreader_sync::decideSleepPush(lookup, KOReaderSyncClient::isOwnRecord(remote), payload.percentage,
                                         remote.percentage)) {
    case koreader_sync::SleepPush::LookupFailed:
      return Outcome::Failed;
    case koreader_sync::SleepPush::RemoteAhead:
      LOG_INF("KOSleep", "%s is ahead (%.4f > %.4f); not pushing", remote.device.c_str(), remote.percentage,
              payload.percentage);
      return Outcome::RemoteAhead;
    case koreader_sync::SleepPush::Upload:
      break;
  }
  error = KOReaderSyncClient::updateProgress(payload);
  return error == KOReaderSyncClient::OK ? Outcome::Pushed : Outcome::Failed;
}

void writeResult(const char* outcome, const char* detail, const float percentage) {
  char line[160];
  snprintf(line, sizeof(line), "firmware=" CROSSPOINT_VERSION "\nresult=%s\ndetail=%s\npercentage=%.4f\n", outcome,
           detail, percentage);
  Storage.writeFile(RESULT_LOG_FILE, line);
}
}  // namespace

void capture(const std::string& epubPath, const CrossPointPosition& position) {
  if (!KOREADER_STORE.getSyncOnSleep() || !KOREADER_STORE.hasCredentials()) return;
  capturedPath = epubPath;
  capturedPosition = position;
  captured = true;
}

bool pending() {
  if (!captured) return false;
  std::string last;
  Storage.readFileToString("KOSleep", LAST_PUSH_FILE, 512, last);
  return last != positionKey();
}

void push(GfxRenderer& renderer) {
  // Nothing draws again before deep sleep; the lock keeps the render task off
  // the framebuffer while it is lent.
  RenderLock lock;

  // The reader built this book's cache, so loading only reads it.
  auto epub = std::make_shared<Epub>(capturedPath, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(false, true)) {
    LOG_ERR("KOSleep", "Could not load %s", capturedPath.c_str());
    writeResult("failed", "epub load", 0.0f);
    return;
  }
  SavedProgressPosition local;
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    local = ProgressMapper::toSavedProgress(epub, capturedPosition);
  }
  const std::string hash = koreaderDocumentHash(capturedPath, KOREADER_STORE.getMatchMethod());
  if (hash.empty()) {
    LOG_ERR("KOSleep", "Could not hash %s", capturedPath.c_str());
    writeResult("failed", "document hash", local.percentage);
    return;
  }
  const KOReaderProgress payload =
      buildKOReaderProgressPayload(hash, local, capturedPosition, capturedPath, epub.get());
  epub.reset();

  WiFi.setSleep(false);  // modem sleep stalls the short HTTPS exchange
  KOReaderSyncClient::Error error = KOReaderSyncClient::OK;
  Outcome outcome = Outcome::Failed;
  const char* detail = "";
  if (KOREADER_STORE.getUseTailnet()) {
    // Same window as KOReaderSyncActivity::overTailnet, without the reclaim
    // that one needs to keep drawing.
    std::string baseUrl = KOREADER_STORE.getBaseUrl();
    TAILNET.setTargetUrl(baseUrl);
    KOReaderSyncClient::reserveBaseUrlOverride(baseUrl.size() + 32);
    TailnetSession::prepareCallerTask();
    renderer.releaseFrameBufferToHeap();
    if (TAILNET.ensureUp(nullptr, nullptr, TAILNET_TIMEOUT_MS) &&
        !(baseUrl = TAILNET.rewriteUrlForTailnet(baseUrl)).empty()) {
      KOReaderSyncClient::setBaseUrlOverride(baseUrl);
      outcome = upload(payload, error);
    } else {
      error = KOReaderSyncClient::TAILNET_ERROR;
      detail = TAILNET.lastErrorCode();
    }
    TAILNET.teardown();
    KOReaderSyncClient::clearBaseUrlOverride();
    renderer.reacquireFrameBufferFromHeap();
  } else {
    outcome = upload(payload, error);
  }

  if (outcome == Outcome::Failed) {
    LOG_ERR("KOSleep", "Push failed: %s %s", KOReaderSyncClient::errorString(error), detail);
    writeResult("failed", *detail ? detail : KOReaderSyncClient::errorString(error), local.percentage);
    return;
  }
  LOG_INF("KOSleep", "%s at %.4f", outcome == Outcome::Pushed ? "Pushed" : "Remote ahead", local.percentage);
  writeResult(outcome == Outcome::Pushed ? "pushed" : "remote ahead", "", local.percentage);
  Storage.writeFile(LAST_PUSH_FILE, positionKey().c_str());
}

}  // namespace kosync_sleep
