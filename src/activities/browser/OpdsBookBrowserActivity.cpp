#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <OpdsFeedCacheStore.h>
#include <OpdsStream.h>
#include <WiFi.h>
#include <esp_system.h>

#include <ctime>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/RenderLock.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/icons/search32.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/TailnetSession.h"
#include "util/BookCacheUtils.h"
#include "util/OpdsFilename.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SEARCH = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

// Tailnet feed spool and the resume sidecar that carries the browse state
// (fetch result, current path, history) across a reboot into the browser.
constexpr char OPDS_FEED_SPOOL[] = "/.crosspoint/opds_feed.xml";
constexpr char OPDS_RESUME_FILE[] = "/.crosspoint/opds_resume.txt";

}  // namespace

OpdsBookBrowserActivity* OpdsBookBrowserActivity::activeInstance = nullptr;

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server)
    : Activity("OpdsBookBrowser", renderer, mappedInput),
      UiAppHost(renderer),
      buttonNavigator(),
      server(std::move(server)) {}

bool OpdsBookBrowserActivity::injectOpenRow(const int row) {
  if (state != BrowserState::BROWSING) return false;
  if (row < 0 || row >= static_cast<int>(entries.size())) return false;
  selectorIndex = row;
  activateSelected();
  return true;
}

bool OpdsBookBrowserActivity::injectBack() {
  if (state != BrowserState::BROWSING) return false;
  navigateBack();
  return true;
}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();
  activeInstance = this;

  // A fresh session id marks every page fetched from this entry as
  // same-session for the cache; a boot resumed via the sidecar keeps the id
  // it rebooted with (RTC_NOINIT) so pages spooled before the reboot still hit.
  if (!Storage.exists(OPDS_RESUME_FILE)) setOpdsBrowseSessionId(millis() ^ esp_random());
  OpdsFeedCacheStore::sweepOrphans();

  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  listNav.reset();
  resetUi();
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.on(ACTION_SEARCH, &OpdsBookBrowserActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &OpdsBookBrowserActivity::onCancelEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
  requestUpdate();

  // Booted from rebootIntoBrowse(): render the spooled feed; WiFi (and the
  // tunnel) are only brought up again on the next navigation.
  if (resumeFromSpool()) return;
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  activeInstance = nullptr;
  Activity::onExit();
  entries.clear();
  navigationHistory.clear();

  // Stop the WG netif before dropping WiFi; the silent reboot below then
  // clears any remaining tunnel state (see TailnetSession lifetime model).
  if (TAILNET.wasActive()) {
    TAILNET.teardown();
  }

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::activateSelected() {
  if (entries.empty() || selectorIndex < 0 || selectorIndex >= static_cast<int>(entries.size())) return;
  const auto& entry = entries[selectorIndex];
  entry.type == OpdsEntryType::BOOK ? downloadBook(entry) : navigateToEntry(entry);
}

void OpdsBookBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->entries.size())) return;
  self->selectorIndex = event.value;
  // The tapped row leaves the screen either way (new feed or download view);
  // a lingering tap flash would gray an unrelated row on the next list.
  self->app.clearTapFlash();
  self->activateSelected();
}

void OpdsBookBrowserActivity::onSearchEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  self->launchSearch();
}

void OpdsBookBrowserActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (state == BrowserState::ERROR) {
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdate();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) return;

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == 0) launchSearch();
    }

    // Touch goes through the FreeInkApp: render() registered every tap target
    // (rows, header search button); route the snapshot and let the registered
    // handlers dispatch.
    const auto route = routeTouch(mappedInput);
    if (route.routed) {
      // No pressed-state repaint: the render it triggers would drop a slow
      // tap's release inside the uiReady window (tap-to-activate needed two
      // taps), and it costs a second e-ink refresh per tap.
      if (app.invalidated()) requestUpdate();
      if (route) return;  // dispatched to onRowEvent/onSearchEvent
      if (state != BrowserState::BROWSING) return;
    }

    if (!entries.empty()) {
      // Swipes scroll the viewport; the selection stays put (it may scroll
      // off-screen) and button navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? listNav.visibleRows : -listNav.visibleRows;
        if (listNav.scrollBy(delta, static_cast<int>(entries.size()))) requestUpdate();
        return;
      }

      const auto moveSelection = [this](const int index) {
        selectorIndex = index;
        listNav.selected = index;
        listNav.follow(static_cast<int>(entries.size()));
        requestUpdate();
      };
      buttonNavigator.onNextRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, entries.size())); });
      buttonNavigator.onPreviousRelease(
          [this, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, entries.size())); });
      buttonNavigator.onNextContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, entries.size(), listNav.visibleRows));
      });
      buttonNavigator.onPreviousContinuous([this, &moveSelection] {
        moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, entries.size(), listNav.visibleRows));
      });
    }
  }
}

bool OpdsBookBrowserActivity::preventAutoSleep() {
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

void OpdsBookBrowserActivity::rootScreen(UiScreen& screen, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

// Shared chrome for every state: reserve the firmware's button-hint band and
// draw the themed header (padding, centering, and rule come from the theme).
void OpdsBookBrowserActivity::screenHeader(UiScreen& screen, const bool withSearch) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getInstance().getMetrics().buttonHintsHeight));
  // Same top offset as every GUI.drawHeader caller, so the band lines up with
  // the rest of the firmware's screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().topPadding));
  fui::HeaderProps header;
  header.title = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  header.borderEdges = fui::EdgeBottom;
  if (withSearch && !searchTemplate.empty()) {
    header.trailingIcon = fui::bitmapFromIcon(icon_search_32);
    header.trailingAction = ACTION_SEARCH;
    // Optically align the icon with the title glyphs: text hangs low in its
    // line cell by the font's internal leading; drop the button to match.
    const int titleFontId = uiScaleSpec().titleFontId;
    header.actionOffsetY =
        static_cast<int16_t>((renderer.getLineHeight(titleFontId) - renderer.getTextHeight(titleFontId)) / 2);
  }
  screen.header(header);
  // Same breathing room between header and content as the legacy screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiScreen& screen) {
  screenHeader(screen, true);

  if (entries.empty()) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Transient per-render: sized once via reserve, points into `entries`
  // strings, freed on scope exit.
  // rowItems is built whenever entries changes (see rebuildRowItems(), called
  // from fetchFeed()/releaseEntries()) and reused here on every repaint.
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(rowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  listNav.selected = selectorIndex;
  props.partialTrailingRow = true;
  screen.syncListViewport(listNav, props, static_cast<int>(entries.size()));
  screen.list(props);
}

void OpdsBookBrowserActivity::buildDownloadScreen(UiScreen& screen) {
  screenHeader(screen, false);

  // Centered block: status line, book title, progress bar, cancel button.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 2 + barH + btnH + gap * 3);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void OpdsBookBrowserActivity::buildStatusScreen(UiScreen& screen) {
  screenHeader(screen, false);

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    centered.maxLines = 4;
    const int16_t errorH =
        fui::measureWrappedText(screen.target(), errorMessage.c_str(), centered, screen.body().width).height;
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 2 : 1) + errorH + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(errorH, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  // CHECK_WIFI / LOADING (and the brief child-activity handoff states).
  screen.centeredText(statusMessage.c_str(), centered);
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::BROWSING: {
      LOG_DBG("OPDS", "Rendered browse: rows=%u selected=%d", static_cast<unsigned>(rowItems.size()), selectorIndex);
      const char* confirmLabel =
          (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
      const char* searchLabel = (!searchTemplate.empty() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderUi();
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
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
    state = BrowserState::LOADING;
    statusMessage = TAILNET.isUp() ? tr(STR_LOADING) : tr(STR_TAILNET_CONNECTING);
    requestUpdateAndWait();  // paint the status while the framebuffer still exists

    // Opened before the release so its bookkeeping sits outside the region.
    HalFile spool;
    if (!Storage.openFileForWrite("OPDS", OPDS_FEED_SPOOL, spool)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }

    // Set the target before the release: the session keeps the host string
    // past teardown, and it must not sit inside the framebuffer's region.
    if (!TAILNET.isUp()) TAILNET.setTargetUrl(url);

    bool ok = false;
    size_t spooled = 0;
    {
      RenderLock lock;
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
          state = BrowserState::ERROR;
          errorMessage = tr(STR_FETCH_FEED_FAILED);
        }
      } else if (tailnetRebootAttemptCount() == 0) {
        // Bring-up refused, typically because earlier fetch cycles left the
        // heap too fragmented for the tunnel. Retry once from a fresh boot;
        // a second refusal on a clean heap is reported as the error it is.
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
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  } else {
    LOG_DBG("OPDS", "Fetching: %s", url.c_str());
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
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
        state = BrowserState::ERROR;
        errorMessage = tr(STR_FETCH_FEED_FAILED);
        requestUpdate();
      }
      return true;
    }
    case ResumeMode::FETCH_PENDING:
      if (Storage.exists(OPDS_FEED_SPOOL)) Storage.remove(OPDS_FEED_SPOOL);
      if (serveFromCache()) return true;  // no WiFi detour for a page already on the card
      checkAndConnectWifi();              // fetches currentPath on this fresh heap
      return true;
    case ResumeMode::FETCH_FAILED:
    default:
      if (Storage.exists(OPDS_FEED_SPOOL)) Storage.remove(OPDS_FEED_SPOOL);
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return true;
  }
}

void OpdsBookBrowserActivity::applyParsedFeed(OpdsParser&& parser) {
  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  const bool feedTruncated = parser.truncated();
  // Reset the selection before the swap: the render task reads
  // entries[selectorIndex] under only an empty() guard, and the new feed can
  // be shorter than the old selection.
  selectorIndex = 0;
  listNav.reset();
  entries = std::move(parser).getEntries();

  entries.reserve(entries.size() + (prevUrl.empty() ? 0 : 1) + (nextUrl.empty() ? 0 : 1));
  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }
  if (feedTruncated) {
    LOG_INF("OPDS", "Feed truncated to fit memory");
  }

  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
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
  selectorIndex = 0;
  if (serveFromCache()) return;  // before any WiFi/tunnel work, and without a Loading repaint
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  checkAndConnectWifi();  // reconnects first after a resumed boot, then fetches currentPath
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    releaseEntries();
    selectorIndex = 0;
    if (serveFromCache()) return;
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    checkAndConnectWifi();
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  cancelDownload = false;
  goHomeAfterCancel = false;
  requestUpdate(true);

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

  // Rebuildable SD-font caches can hold tens of KB the TLS session needs for
  // a multi-MB book; release them up front (they repopulate on demand) and
  // refuse to start below the floor — a doomed transfer otherwise dies
  // mid-stream with MEMORY_E, or abort()s on an interior allocation.
  if (auto* fcm = renderer.getFontCacheManager()) {
    fcm->releaseSdFontCaches();
  }
  HttpDownloader::DownloadError result = HttpDownloader::ABORTED;
  if (server.useTailnet) {
    // Same memory model as fetchFeed(): the panel keeps the "Downloading"
    // frame, the framebuffer is lent to the heap for the tunnel + TLS, and no
    // progress can be drawn until the transfer ends. The file lands on SD
    // whichever way the display comes back (reclaim, or a reboot into the
    // browse with the feed fetch pending).
    requestUpdateAndWait();
    if (!TAILNET.isUp()) TAILNET.setTargetUrl(downloadUrl);  // before the release (see fetchFeed)
    bool started = false;
    {
      RenderLock lock;
      renderer.releaseFrameBufferToHeap();
      if (prepareTailnetUrl(downloadUrl)) {
        started = true;
        LOG_DBG("OPDS", "Downloading (tailnet): %s", downloadUrl.c_str());
        result = HttpDownloader::downloadToFile(
            downloadUrl, filename,
            [this](const size_t downloaded, const size_t total) {
              downloadProgress = downloaded;
              downloadTotal = total;
              // No display during the transfer, so progress goes to the log.
              static unsigned long lastLogMs = 0;
              if (millis() - lastLogMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
                lastLogMs = millis();
                LOG_INF("OPDS", "Download progress: %u / %u bytes", (unsigned)downloaded, (unsigned)total);
              }
              // Pump input so Back or the home gesture can abort mid-transfer.
              mappedInput.update(true);
              if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
              if (mappedInput.wasHomeGesture()) {
                cancelDownload = true;
                goHomeAfterCancel = true;
              }
            },
            &cancelDownload, server.username, server.password);
        if (result == HttpDownloader::OK) {
          clearBookCache(filename);
          library::markLibraryIndexDirty();
        }
      } else if (tailnetRebootAttemptCount() == 0) {
        TAILNET.teardown();
        rebootIntoBrowse(ResumeMode::FETCH_PENDING);  // heap too fragmented: retry from a fresh boot
      }
      TAILNET.teardown();
      if (!renderer.reacquireFrameBufferFromHeap()) rebootIntoBrowse(ResumeMode::FETCH_PENDING);
    }
    if (!started) {  // prepareTailnetUrl() set the error
      requestUpdate();
      return;
    }
  } else {
    LOG_DBG("OPDS", "Download heap: %u free, %u max block", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
        ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC) {
      LOG_ERR("OPDS", "Low heap for download (%u free, %u max block)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }

    int lastRenderedPercent = -1;
    unsigned long lastProgressUpdateMs = 0;
    result = HttpDownloader::downloadToFile(
        downloadUrl, filename,
        [this, &lastRenderedPercent, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
          downloadProgress = downloaded;
          downloadTotal = total;
          // The activity loop is blocked for the whole download; pump input here
          // so the Cancel button or a Back press can abort mid-transfer.
          mappedInput.update(true);
          if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelDownload = true;
          // Home cancels immediately; other configured actions are deferred to
          // the next main-loop pass by the transfer input pump.
          if (mappedInput.wasHomeGesture()) {
            cancelDownload = true;
            goHomeAfterCancel = true;
          }
          routeTouch(mappedInput);
          const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
          const unsigned long now = millis();
          if (percent >= 100 || lastRenderedPercent < 0 ||
              percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
              now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
            lastRenderedPercent = percent;
            lastProgressUpdateMs = now;
            requestUpdate(true);
          }
        },
        &cancelDownload, server.username, server.password);
  }

  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    library::markLibraryIndexDirty();
    if (serveFromCache()) return;  // the released catalog, back from the card
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    fetchFeed(currentPath);
    return;
  } else if (result == HttpDownloader::ABORTED) {
    // The partial file is already removed. Reload the released catalog unless
    // the cancel came from the home gesture.
    LOG_INF("OPDS", "Download cancelled");
    if (goHomeAfterCancel) {
      onGoHome();
      return;
    }
    if (serveFromCache()) return;
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    fetchFeed(currentPath);
    return;
  } else {
    LOG_ERR("OPDS", "Download failed: %d", static_cast<int>(result));
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);
  currentPath = url;

  releaseEntries();
  selectorIndex = 0;
  if (serveFromCache()) return;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  checkAndConnectWifi();
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
    state = BrowserState::ERROR;
    errorMessage = std::string(TAILNET.lastErrorCode()) + ": " + TAILNET.lastErrorMessage();
    return false;
  }

  std::string rewritten = TAILNET.rewriteUrlForTailnet(url);
  if (rewritten.empty()) {
    state = BrowserState::ERROR;
    errorMessage = std::string(TAILNET.lastErrorCode()) + ": " + TAILNET.lastErrorMessage();
    return false;
  }
  url = std::move(rewritten);
  return true;
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
