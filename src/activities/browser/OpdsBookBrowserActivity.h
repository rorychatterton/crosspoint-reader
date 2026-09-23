#pragma once
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "activities/CatalogActivity.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public CatalogActivity {
 public:
  // What a boot resumed via rebootIntoBrowse() does: show the fetch error,
  // parse the spooled feed, or fetch `path` on the fresh heap. The sidecar
  // (/.crosspoint/opds_resume.txt) holds the mode, the path, then the history.
  enum class ResumeMode : uint8_t { FETCH_FAILED = 0, SPOOLED = 1, FETCH_PENDING = 2 };
  static bool writeResumeSidecar(ResumeMode mode, const std::string& path, const std::vector<std::string>& history);

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;

  // Headless driver hooks (main.cpp serial CMD:OPDS_OPEN / CMD:OPDS_BACK):
  // the browser on screen, if any, and the same actions a Confirm on `row` or
  // a Back press would take while browsing. Return false when not browsing.
  static OpdsBookBrowserActivity* instance() { return activeInstance; }
  bool injectOpenRow(int row);
  bool injectBack();

 private:
  static OpdsBookBrowserActivity* activeInstance;

  std::vector<OpdsEntry> entries;
  // Row buffer, built whenever entries changes (fetchFeed()/releaseEntries())
  // so buildBrowsingScreen() reuses it on every repaint instead of rebuilding
  // a ListItem vector per render.
  std::vector<freeink::ui::ListItem> rowItems;
  void rebuildRowItems();
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  // Synthetic pager rows fetchFeed() bracketed the entries with; their footer
  // hint is Fetch (a server round-trip), not Open.
  bool prevRowPresent = false;
  bool nextRowPresent = false;
  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  int listCount() const override { return state == State::BROWSING ? static_cast<int>(entries.size()) : 0; }
  bool hasSearch() const override { return !searchTemplate.empty(); }
  void activateIndex(int index) override;
  void buildScreen(UiScreen& screen) override;
  void drawFooter() override;
  void buildBrowsingScreen(UiScreen& screen);
  // Fetches currentPath from the server (WiFi must be up).
  void startBrowse() override;
  void downloadFinished(bool) override { openCurrentPath(); }
  // Shows currentPath: from the feed cache when fresh, else via WiFi (and the
  // tunnel) and a fetch.
  void openCurrentPath();
  // For tailnet-flagged servers: bring the Tailscale session up (first call
  // blocks 15-25s for registration) and rewrite the URL's host to the peer's
  // VPN IP. Returns false after switching to the ERROR state.
  bool prepareTailnetUrl(std::string& url);
  void fetchFeed(const std::string& path);
  void releaseEntries();
  void navigateToEntry(const OpdsEntry& entry);
  void onBackButton() override;
  void downloadBook(const OpdsEntry& book);
  void performSearch(const std::string& query) override;
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
};
