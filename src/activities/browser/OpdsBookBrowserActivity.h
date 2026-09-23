#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity, private UiAppHost {
 public:
  // What a boot resumed via rebootIntoBrowse() does: show the fetch error,
  // parse the spooled feed, or fetch `path` on the fresh heap. The sidecar
  // (/.crosspoint/opds_resume.txt) holds the mode, the path, then the history.
  enum class ResumeMode : uint8_t { FETCH_FAILED = 0, SPOOLED = 1, FETCH_PENDING = 2 };
  static bool writeResumeSidecar(ResumeMode mode, const std::string& path, const std::vector<std::string>& history);

  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Headless driver hooks (main.cpp serial CMD:OPDS_OPEN / CMD:OPDS_BACK):
  // the browser on screen, if any, and the same actions a Confirm on `row` or
  // a Back press would take while browsing. Return false when not browsing.
  static OpdsBookBrowserActivity* instance() { return activeInstance; }
  bool injectOpenRow(int row);
  bool injectBack();

 private:
  static OpdsBookBrowserActivity* activeInstance;

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  // Viewport memory (top/visibleRows) for the browsing list; `selected` is
  // mirrored from selectorIndex at build/move time.
  freeink::ui::ListNav listNav;
  // Read by HttpDownloader between chunks; set by the Cancel button handler or
  // a Back press, both pumped from the download's progress callback.
  bool cancelDownload = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeAfterCancel = false;

  // Single screen fn dispatching on `state`: every state shares the themed
  // header and gets built through FreeInkUI.
  static void rootScreen(UiScreen& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSearchEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiScreen& screen, bool withSearch);
  void buildBrowsingScreen(UiScreen& screen);
  void buildDownloadScreen(UiScreen& screen);
  void buildStatusScreen(UiScreen& screen);
  void activateSelected();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  // For tailnet-flagged servers: bring the Tailscale session up (first call
  // blocks 15-25s for registration) and rewrite the URL's host to the peer's
  // VPN IP. Returns false after switching to the ERROR state.
  bool prepareTailnetUrl(std::string& url);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void launchSearch();
  void performSearch(const std::string& query);
  // Tailnet fetches spool the feed to SD (see fetchFeed); these parse it,
  // apply it, or carry it across the reboot taken when the framebuffer
  // cannot be reclaimed afterwards.
  bool parseSpool(OpdsParser& parser, const char* path);
  void applyParsedFeed(OpdsParser&& parser);
  [[noreturn]] void rebootIntoBrowse(ResumeMode mode);
  bool resumeFromSpool();
  // On-SD feed cache (lib/OpdsFeedCache): pages keyed by fetch URL and
  // account. serveFromCache() renders currentPath from the card when a fresh
  // copy exists (no network, no tunnel); fileSpoolInCache() moves a freshly
  // spooled page into the cache and yields its path there.
  bool serveFromCache();
  bool fileSpoolInCache(const std::string& feedUrl, uint32_t bytes, char* cachePath, size_t cap);
  bool preventAutoSleep() override;
};
