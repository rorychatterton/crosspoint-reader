#pragma once

#include <cstdint>
#include <string_view>

#include "OpdsFeedCache.h"

// SD-side of the OPDS feed cache (see OpdsFeedCache.h for the layout and the
// decision rules). Index lines are streamed through a 64-byte window: the
// numeric prefix is parsed in place and the URL tail is skipped or copied, so
// no function here holds a whole line or the URL in RAM.
class OpdsFeedCacheStore {
 public:
  // Fresh-entry lookup by key; no heap allocation. `now` is 0 when the clock
  // is unsynced (opds_feed_cache::normaliseNow).
  static bool lookup(uint64_t hash, uint32_t now, uint32_t sessionId, opds_feed_cache::Entry& out);

  // Moves the spooled feed at `spoolPath` into the cache under `entry.hash`,
  // rewrites index.txt with `entry` as most recent, and removes evicted pages.
  // On false the spool is left where it was.
  static bool commit(const char* spoolPath, const opds_feed_cache::Entry& entry, std::string_view url);

  // Removes files in the cache dir that the index does not list.
  static void sweepOrphans();
};
