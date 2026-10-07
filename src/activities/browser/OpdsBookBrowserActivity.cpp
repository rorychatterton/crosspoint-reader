#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <OpdsFeedCacheStore.h>
#include <OpdsStream.h>
#include <esp_system.h>

#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/RenderLock.h"
#include "components/CatalogScreens.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TailnetSession.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/PluginHttp.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long TAILNET_PROGRESS_LOG_MS = 5000;

// Tailnet feed spool and the resume sidecar that carries the browse state
// (fetch result, current path, history) across a reboot into the browser.
constexpr char OPDS_FEED_SPOOL[] = "/.crosspoint/opds_feed.xml";
constexpr char OPDS_RESUME_FILE[] = "/.crosspoint/opds_resume.txt";
}  // namespace

OpdsBookBrowserActivity* OpdsBookBrowserActivity::activeInstance = nullptr;

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server)
    : CatalogActivity("OpdsBookBrowser", renderer, mappedInput), server(std::move(server)) {}

bool OpdsBookBrowserActivity::injectOpenRow(const int row) {
  if (state != State::BROWSING) return false;
  if (row < 0 || row >= static_cast<int>(entries.size())) return false;
  nav.selected = row;
  activateIndex(row);
  return true;
}

bool OpdsBookBrowserActivity::injectBack() {
  if (state != State::BROWSING) return false;
  onBackButton();
  return true;
}

void OpdsBookBrowserActivity::onEnter() {
  CatalogActivity::onEnter();
  activeInstance = this;

  // A fresh session id marks every page fetched from this entry as
  // same-session for the cache; a boot resumed via the sidecar keeps the id
  // it rebooted with (RTC_NOINIT) so pages spooled before the reboot still hit.
  if (!Storage.exists(OPDS_RESUME_FILE)) setOpdsBrowseSessionId(millis() ^ esp_random());
  OpdsFeedCacheStore::sweepOrphans();

  state = State::CHECK_WIFI;
  statusMessage = tr(STR_CHECKING_WIFI);

  // Booted from rebootIntoBrowse(): render the spooled feed; WiFi (and the
  // tunnel) are only brought up again on the next navigation.
  if (resumeFromSpool()) return;
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  activeInstance = nullptr;
  releaseEntries();
  navigationHistory.clear();
  // Stop the WG netif before CatalogActivity drops WiFi; its silent reboot
  // then clears any remaining tunnel state (see TailnetSession lifetime model).
  if (TAILNET.wasActive()) TAILNET.teardown();
  CatalogActivity::onExit();
}

void OpdsBookBrowserActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  const auto& entry = entries[index];
  entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str());
  if (!buildStatusScreen(screen, /*boldError=*/false, /*showDownloadTotal=*/true)) buildBrowsingScreen(screen);
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  if (entries.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  syncListViewport(screen, props);
  screen.list(props);
}

void OpdsBookBrowserActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING: {
      LOG_DBG("OPDS", "Rendered browse: rows=%u selected=%d", static_cast<unsigned>(rowItems.size()),
              static_cast<int>(nav.selected));
      // Feeds open; books and the pager rows fetch, matching the plugin catalog.
      const char* confirmLabel = tr(STR_OPEN);
      if (!entries.empty()) {
        const bool onPager = (prevRowPresent && nav.selected == 0) ||
                             (nextRowPresent && nav.selected == static_cast<int>(entries.size()) - 1);
        if (onPager || entries[nav.selected].type == OpdsEntryType::BOOK) confirmLabel = tr(STR_FETCH);
      }
      const char* searchLabel = (!searchTemplate.empty() && nav.selected == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case State::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    fail(StrId::STR_NO_SERVER_URL);
    return;
  }

  std::string url = UrlUtils::buildUrl(server.url, path);

  OpdsParser parser;
  if (server.useTailnet) {
    // DERP TLS plus the OPDS HTTPS session need a contiguous block the
    // fragmented UI heap cannot supply, so the 48 KB framebuffer is freed to
    // the heap for the fetch under a RenderLock (the panel keeps the painted
    // status frame). ~35 KB free with the framebuffer held leaves no room for
    // a RAM body, and anything left in the freed region would split the 48 KB
    // block, so the feed is spooled to SD and parsed after the reclaim. The
    // tunnel and framebuffer together exceed the heap, so the tunnel is torn
    // down before the reclaim and re-established per fetch (the cached DNS
    // answer keeps later bring-ups single-phase). Network-stack leftovers
    // (TIME_WAIT sockets, a timed-out stop) can still split the region; then
    // only a reboot restores the display, and rebootIntoBrowse() carries the
    // spool and browse state across it.
    state = State::LOADING;
    statusMessage = TAILNET.isUp() ? tr(STR_LOADING) : tr(STR_TAILNET_CONNECTING);
    requestUpdateAndWait();  // paint the status while the framebuffer still exists

    // Opened before the release so its bookkeeping sits outside the region.
    HalFile spool;
    if (!Storage.openFileForWrite("OPDS", OPDS_FEED_SPOOL, spool)) {
      fail(StrId::STR_FETCH_FEED_FAILED);
      return;
    }

    // Set the target before the release: the session keeps the host string
    // past teardown, and it must not sit inside the framebuffer's region.
    if (!TAILNET.isUp()) TAILNET.setTargetUrl(url);

    bool ok = false;
    size_t spooled = 0;
    {
      RenderLock lock;
      TailnetSession::prepareCallerTask();
      renderer.releaseFrameBufferToHeap();
      if (prepareTailnetUrl(url)) {  // ensureUp + rewrite (sets errorMessage on fail)
        LOG_DBG("OPDS", "Fetching: %s", url.c_str());
        ok = HttpDownloader::fetchUrl(
            url,
            [&spool, &spooled](const uint8_t* data, size_t len) {
              spooled += len;
              return spool.write(data, len) == len;
            },
            server.username, server.password);
        if (!ok) {
          state = State::ERROR;
          errorMessage = tr(STR_FETCH_FEED_FAILED);
        }
      } else if (TAILNET.needsReboot() || tailnetRebootAttemptCount() == 0) {
        // Bring-up refused, typically because earlier fetch cycles (or a
        // failed warm start) left the heap too fragmented for the tunnel.
        // Retry once from a fresh boot; a second refusal on a clean heap is
        // reported as the error it is. A failed warm start always retries:
        // it cleared the netmap cache, so the next boot starts cold.
        spool.close();
        TAILNET.teardown();
        rebootIntoBrowse(ResumeMode::FETCH_PENDING);
      }
      spool.close();       // closed before reopen/remove below
      TAILNET.teardown();  // drop the tunnel before reclaiming the framebuffer
      // Still under the RenderLock: nothing may try to draw a null framebuffer.
      if (!renderer.reacquireFrameBufferFromHeap()) {
        rebootIntoBrowse(ok ? ResumeMode::SPOOLED : ResumeMode::FETCH_FAILED);
      }
    }  // RenderLock released
    if (!ok) {
      Storage.remove(OPDS_FEED_SPOOL);
      requestUpdate();
      return;
    }
    // Framebuffer restored and tunnel down: file the page in the cache (keyed
    // by the pre-rewrite URL) and parse it from there.
    char cachePath[opds_feed_cache::PATH_CAP];
    const bool cached = fileSpoolInCache(UrlUtils::buildUrl(server.url, path), static_cast<uint32_t>(spooled),
                                         cachePath, sizeof(cachePath));
    const bool parsed = parseSpool(parser, cached ? cachePath : OPDS_FEED_SPOOL);
    if (!cached) Storage.remove(OPDS_FEED_SPOOL);
    if (!parsed) {
      fail(StrId::STR_FETCH_FEED_FAILED);
      return;
    }
  } else {
    LOG_DBG("OPDS", "Fetching: %s", url.c_str());
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      fail(StrId::STR_FETCH_FEED_FAILED);
      return;
    }
  }

  applyParsedFeed(std::move(parser));
}

// Feeds the spooled/cached page at `path` through the parser. The file is
// left in place (cache pages are reused; a plain spool is removed by the
// caller). Returns false only when the file cannot be opened.
bool OpdsBookBrowserActivity::parseSpool(OpdsParser& parser, const char* path) {
  HalFile spool;
  if (!Storage.openFileForRead("OPDS", path, spool)) return false;
  uint8_t chunk[256];
  int n;
  while ((n = spool.read(chunk, sizeof(chunk))) > 0) parser.write(chunk, static_cast<size_t>(n));
  parser.flush();
  return true;
}

bool OpdsBookBrowserActivity::fileSpoolInCache(const std::string& feedUrl, const uint32_t bytes, char* cachePath,
                                               const size_t cap) {
  opds_feed_cache::Entry entry;
  entry.hash = opds_feed_cache::feedKey(feedUrl, server.username);
  entry.bytes = bytes;
  entry.fetchedAt = opds_feed_cache::normaliseNow(static_cast<int64_t>(time(nullptr)));
  entry.sessionId = opdsBrowseSessionId();
  opds_feed_cache::formatPath(cachePath, cap, entry.hash);
  if (!OpdsFeedCacheStore::commit(OPDS_FEED_SPOOL, entry, feedUrl)) return false;
  LOG_INF("OPDS", "Feed cached path=%s bytes=%u", currentPath.c_str(), static_cast<unsigned>(bytes));
  return true;
}

bool OpdsBookBrowserActivity::serveFromCache() {
  const uint64_t key = opds_feed_cache::feedKey(UrlUtils::buildUrl(server.url, currentPath), server.username);
  const uint32_t now = opds_feed_cache::normaliseNow(static_cast<int64_t>(time(nullptr)));
  opds_feed_cache::Entry entry;
  if (!OpdsFeedCacheStore::lookup(key, now, opdsBrowseSessionId(), entry)) return false;
  char cachePath[opds_feed_cache::PATH_CAP];
  opds_feed_cache::formatPath(cachePath, sizeof(cachePath), key);
  if (!Storage.exists(cachePath)) return false;  // indexed but swept/missing: fetch instead
  OpdsParser parser;
  if (!parseSpool(parser, cachePath)) return false;
  LOG_INF("OPDS", "Feed served from cache path=%s bytes=%u age_s=%u", currentPath.c_str(),
          static_cast<unsigned>(entry.bytes), static_cast<unsigned>(opds_feed_cache::ageSeconds(entry, now)));
  applyParsedFeed(std::move(parser));
  return true;
}

bool OpdsBookBrowserActivity::writeResumeSidecar(const ResumeMode mode, const std::string& path,
                                                 const std::vector<std::string>& history) {
  HalFile f;
  if (!Storage.openFileForWrite("OPDS", OPDS_RESUME_FILE, f)) return false;
  const char modeLine[3] = {static_cast<char>('0' + static_cast<uint8_t>(mode)), '\n', 0};
  f.write(modeLine, 2);
  f.write(path.data(), path.size());
  f.write("\n", 1);
  for (const auto& p : history) {
    f.write(p.data(), p.size());
    f.write("\n", 1);
  }
  return f.close();
}

void OpdsBookBrowserActivity::rebootIntoBrowse(const ResumeMode mode) {
  if (!writeResumeSidecar(mode, currentPath, navigationHistory)) {
    LOG_ERR("OPDS", "Resume sidecar write failed; the resumed boot will start at the root feed");
  }
  uint32_t index = 0;
  const auto& servers = OPDS_STORE.getServers();
  for (size_t i = 0; i < servers.size(); i++) {
    if (servers[i].url == server.url && servers[i].name == server.name) {
      index = static_cast<uint32_t>(i);
      break;
    }
  }
  LOG_ERR("OPDS", "Rebooting into the browse (mode=%u)", static_cast<unsigned>(mode));
  silentRestartToOpds(index, /*paint=*/false);
  for (;;) delay(1000);  // ESP.restart() does not return
}

bool OpdsBookBrowserActivity::resumeFromSpool() {
  if (!Storage.exists(OPDS_RESUME_FILE)) {
    if (Storage.exists(OPDS_FEED_SPOOL)) Storage.remove(OPDS_FEED_SPOOL);  // stale
    return false;
  }
  const String sidecar = Storage.readFile(OPDS_RESUME_FILE);
  Storage.remove(OPDS_RESUME_FILE);
  ResumeMode mode = ResumeMode::FETCH_FAILED;
  bool first = true;
  bool havePath = false;
  size_t start = 0;
  while (start < static_cast<size_t>(sidecar.length())) {
    int end = sidecar.indexOf('\n', static_cast<unsigned>(start));
    if (end < 0) end = sidecar.length();
    const std::string line(sidecar.c_str() + start, static_cast<size_t>(end) - start);
    start = static_cast<size_t>(end) + 1;
    if (first) {
      if (line == "1") mode = ResumeMode::SPOOLED;
      if (line == "2") mode = ResumeMode::FETCH_PENDING;
      first = false;
    } else if (!havePath) {
      currentPath = line;
      havePath = true;
    } else {
      navigationHistory.push_back(line);
    }
  }
  LOG_INF("OPDS", "Resume from spool: mode=%u path=%s history=%u", static_cast<unsigned>(mode), currentPath.c_str(),
          static_cast<unsigned>(navigationHistory.size()));
  switch (mode) {
    case ResumeMode::SPOOLED: {
      // File the spool in the cache first so later back navigation to this
      // page is served from the card, then parse it from there.
      uint32_t bytes = 0;
      {
        HalFile spool;
        if (Storage.openFileForRead("OPDS", OPDS_FEED_SPOOL, spool)) bytes = static_cast<uint32_t>(spool.fileSize());
      }
      char cachePath[opds_feed_cache::PATH_CAP];
      const bool cached =
          fileSpoolInCache(UrlUtils::buildUrl(server.url, currentPath), bytes, cachePath, sizeof(cachePath));
      OpdsParser parser;
      const bool parsed = parseSpool(parser, cached ? cachePath : OPDS_FEED_SPOOL);
      if (!cached) Storage.remove(OPDS_FEED_SPOOL);
      if (parsed) {
        applyParsedFeed(std::move(parser));
      } else {
        fail(StrId::STR_FETCH_FEED_FAILED);
      }
      return true;
    }
    case ResumeMode::FETCH_PENDING:
      if (Storage.exists(OPDS_FEED_SPOOL)) Storage.remove(OPDS_FEED_SPOOL);
      openCurrentPath();  // no WiFi detour for a page already on the card
      return true;
    case ResumeMode::FETCH_FAILED:
    default:
      if (Storage.exists(OPDS_FEED_SPOOL)) Storage.remove(OPDS_FEED_SPOOL);
      fail(StrId::STR_FETCH_FEED_FAILED);
      return true;
  }
}

void OpdsBookBrowserActivity::applyParsedFeed(OpdsParser&& parser) {
  if (!parser) {
    fail(StrId::STR_PARSE_FEED_FAILED);
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  const bool feedTruncated = parser.truncated();
  // Reset selection before swapping in a potentially shorter feed.
  nav.reset();
  entries = std::move(parser).getEntries();

  entries.reserve(entries.size() + (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1));
  prevRowPresent = !prevUrl.empty();
  nextRowPresent = !nextUrl.empty();
  if (prevRowPresent) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (nextRowPresent) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }
  if (feedTruncated) {
    LOG_INF("OPDS", "Feed truncated to fit memory");
  }

  state = entries.empty() ? State::ERROR : State::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  rebuildRowItems();
  LOG_INF("OPDS", "Feed applied: entries=%u truncated=%d", static_cast<unsigned>(entries.size()),
          feedTruncated ? 1 : 0);
  requestUpdate();
  // A feed landed: the next tunnel refusal may again retry via a fresh boot.
  clearTailnetRebootAttempt();
}

// Derives rowItems from entries. Called whenever entries changes
// (fetchFeed()/releaseEntries()) so buildBrowsingScreen() reuses the cached
// rows on every repaint instead of rebuilding them per render.
void OpdsBookBrowserActivity::rebuildRowItems() {
  rowItems.clear();
  rowItems.reserve(entries.size());
  for (const auto& entry : entries) {
    fui::ListItem item;
    item.label = entry.title.c_str();
    if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) item.subtitle = entry.author.c_str();
    if (entry.type == OpdsEntryType::NAVIGATION) item.value = ">";
    item.actionValue = static_cast<int16_t>(rowItems.size());
    rowItems.push_back(item);
  }
}

void OpdsBookBrowserActivity::releaseEntries() {
  // The app's interaction table holds row indices (and hit rects) for the old
  // entries; stop routing touches against it until the next render.
  closeRouting();
  std::vector<OpdsEntry>().swap(entries);
  std::vector<fui::ListItem>().swap(rowItems);
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  releaseEntries();
  openCurrentPath();
}

void OpdsBookBrowserActivity::onBackButton() {
  if (state == State::CHECK_WIFI || navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    releaseEntries();
    openCurrentPath();
  }
}

void OpdsBookBrowserActivity::startBrowse() {
  nav.reset();
  beginLoading();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::openCurrentPath() {
  nav.reset();
  // Before any WiFi/tunnel work, and without a Loading repaint. A resumed boot
  // has WiFi down, so a miss reconnects first and then fetches.
  if (serveFromCache()) return;
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  beginDownload(book.title);

  // Build full download URL relative to the current feed, not the root server URL
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  std::string downloadUrl = UrlUtils::buildUrl(feedUrl, book.href);
  // (Tailnet URLs are rewritten below, once the framebuffer has been released.)
  // opdsDownloadFolder is already a null-terminated char[64]; use it directly —
  // no std::string copy. exists()/mkdir() take const char*.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    // exists()-guard first: mkdir's return-on-existing is unconfirmed, and every
    // existing caller checks exists() before mkdir. On real failure, fall back
    // to SD root so the download is never lost.
    LOG_ERR("OPDS", "mkdir failed for %s, using SD root", folder);
    haveFolder = false;
  }

  // downloadToFile() needs a std::string, and titles are unbounded (a fixed
  // char[] would truncate). Cold path (a multi-second download follows), so one
  // reserve'd, in-place-appended owning string is the right call.
  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += opdsBookFilename(book.author, book.title, static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat));
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), filename.c_str());

  // The selected book data is now copied into the download URL, filename, and
  // status line. Reclaim the catalog while TLS owns its record buffers; reload
  // the current feed when the transfer finishes.
  releaseEntries();

  if (!server.useTailnet) {
    // downloadFile() (CatalogActivity) releases font caches and checks the TLS heap floor.
    const auto result = downloadFile(downloadUrl, filename, server.username, server.password);
    if (result == HttpDownloader::OK) {
      clearBookCache(filename);
      library::markLibraryIndexDirty();
    }
    finishDownload(result);
    return;
  }

  // Same memory model as fetchFeed(): the panel keeps the "Downloading" frame,
  // the framebuffer is lent to the heap for the tunnel + TLS, and no progress
  // can be drawn until the transfer ends. The file lands on SD whichever way
  // the display comes back (reclaim, or a reboot into the browse with the feed
  // fetch pending).
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  requestUpdateAndWait();
  if (!TAILNET.isUp()) TAILNET.setTargetUrl(downloadUrl);  // before the release (see fetchFeed)
  HttpDownloader::DownloadError result = HttpDownloader::ABORTED;
  bool started = false;
  {
    RenderLock lock;
    TailnetSession::prepareCallerTask();
    renderer.releaseFrameBufferToHeap();
    if (prepareTailnetUrl(downloadUrl)) {
      started = true;
      LOG_DBG("OPDS", "Downloading (tailnet): %s", downloadUrl.c_str());
      unsigned long lastLogMs = 0;
      result = HttpDownloader::downloadToFile(
          downloadUrl, filename,
          [this, &lastLogMs](const size_t downloaded, const size_t total) {
            downloadProgress = downloaded;
            downloadTotal = total;
            // No display during the transfer, so progress goes to the log.
            if (millis() - lastLogMs >= TAILNET_PROGRESS_LOG_MS) {
              lastLogMs = millis();
              LOG_INF("OPDS", "Download progress: %u / %u bytes", (unsigned)downloaded, (unsigned)total);
            }
            pollDownloadCancel();
          },
          &cancelDownload, server.username, server.password);
      // Before the reclaim: a failed reclaim reboots without returning here.
      if (result == HttpDownloader::OK) {
        clearBookCache(filename);
        library::markLibraryIndexDirty();
      }
    } else if (TAILNET.needsReboot() || tailnetRebootAttemptCount() == 0) {
      TAILNET.teardown();
      rebootIntoBrowse(ResumeMode::FETCH_PENDING);  // heap too fragmented: retry from a fresh boot (see fetchFeed)
    }
    TAILNET.teardown();
    if (!renderer.reacquireFrameBufferFromHeap()) rebootIntoBrowse(ResumeMode::FETCH_PENDING);
  }
  if (!started) {  // prepareTailnetUrl() set the error
    requestUpdate();
    return;
  }
  finishDownload(result);
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = State::BROWSING;
    requestUpdate();
    return;
  }

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), pluginhttp::urlEncodeQuery(query));

  navigationHistory.push_back(currentPath);
  currentPath = url;

  releaseEntries();
  openCurrentPath();
}

bool OpdsBookBrowserActivity::prepareTailnetUrl(std::string& url) {
  // Bring up the tunnel and rewrite the target host to its tailnet address.
  // Blocks this task like downloadBook() does; the caller has painted the
  // status screen and freed the framebuffer before entering here. Back/cancel
  // cannot interrupt the bring-up itself (bounded by TailnetSession's own
  // timeouts). Sets state/errorMessage on failure; the caller repaints.
  if (!TAILNET.isUp()) {
    TAILNET.setTargetUrl(url);
  }
  if (!TAILNET.ensureUp()) {
    state = State::ERROR;
    errorMessage = std::string(TAILNET.lastErrorCode()) + ": " + TAILNET.lastErrorMessage();
    return false;
  }

  std::string rewritten = TAILNET.rewriteUrlForTailnet(url);
  if (rewritten.empty()) {
    state = State::ERROR;
    errorMessage = std::string(TAILNET.lastErrorCode()) + ": " + TAILNET.lastErrorMessage();
    return false;
  }
  url = std::move(rewritten);
  return true;
}
