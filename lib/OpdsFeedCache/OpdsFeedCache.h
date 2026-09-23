#pragma once

// Decision core of the on-SD OPDS feed cache: key hashing, index line
// parse/serialise, freshness rule, and LRU eviction over a fixed entry array.
// No Arduino or SD dependencies (host-testable); the SD I/O lives in
// OpdsFeedCacheStore.
//
// Layout on the card:
//   /.crosspoint/opds_cache/<fnv1a64 hex>.xml   one file per feed page
//   /.crosspoint/opds_cache/index.txt           one line per page, oldest first:
//     <hash> <bytes> <fetchedAt> <sessionId> <url>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace opds_feed_cache {

constexpr char CACHE_DIR[] = "/.crosspoint/opds_cache";
constexpr char INDEX_PATH[] = "/.crosspoint/opds_cache/index.txt";
constexpr char INDEX_TMP_PATH[] = "/.crosspoint/opds_cache/index.tmp";

constexpr size_t MAX_ENTRIES = 16;
constexpr uint32_t MAX_TOTAL_BYTES = 512u * 1024u;
constexpr uint32_t MAX_AGE_S = 24u * 60u * 60u;
// time(nullptr) below this is an unsynced clock (no SNTP yet); stored as 0.
constexpr uint32_t MIN_VALID_EPOCH = 1700000000u;

// "<16 hex>.xml" plus the directory prefix.
constexpr size_t FILE_NAME_LEN = 16 + 4;
constexpr size_t PATH_CAP = sizeof(CACHE_DIR) + 1 + FILE_NAME_LEN + 1;
// Numeric prefix of an index line: hash(16) + 3 x uint32(<=10) + 4 spaces.
constexpr size_t LINE_PREFIX_CAP = 64;

struct Entry {
  uint64_t hash = 0;
  uint32_t bytes = 0;
  uint32_t fetchedAt = 0;  // unix seconds, 0 when the clock was unsynced
  uint32_t sessionId = 0;
};

// One spare slot so a fresh entry can be appended before eviction runs.
struct Index {
  Entry entries[MAX_ENTRIES + 1];
  size_t count = 0;

  int find(const uint64_t hash) const {
    for (size_t i = 0; i < count; i++) {
      if (entries[i].hash == hash) return static_cast<int>(i);
    }
    return -1;
  }

  uint32_t totalBytes() const {
    uint32_t total = 0;
    for (size_t i = 0; i < count; i++) total += entries[i].bytes;
    return total;
  }

  void removeAt(const size_t at) {
    for (size_t i = at + 1; i < count; i++) entries[i - 1] = entries[i];
    count--;
  }

  // Append as most recent; an existing line for the same hash is dropped, and
  // the oldest is dropped when the spare slot is already in use.
  void append(const Entry& e) {
    const int existing = find(e.hash);
    if (existing >= 0) removeAt(static_cast<size_t>(existing));
    if (count == MAX_ENTRIES + 1) removeAt(0);
    entries[count++] = e;
  }
};

// FNV-1a 64 over `data`, continuing from `hash` so several fields can be
// folded into one key.
inline uint64_t fnv1a64(const std::string_view data, uint64_t hash = 14695981039346656037ull) {
  for (const char c : data) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

// Cache key: the exact fetch URL and the account it was fetched with.
inline uint64_t feedKey(const std::string_view url, const std::string_view username) {
  uint64_t hash = fnv1a64(url);
  hash = fnv1a64("\n", hash);
  return fnv1a64(username, hash);
}

inline void formatFileName(char* out, const size_t cap, const uint64_t hash) {
  snprintf(out, cap, "%016llx.xml", static_cast<unsigned long long>(hash));
}

inline void formatPath(char* out, const size_t cap, const uint64_t hash) {
  snprintf(out, cap, "%s/%016llx.xml", CACHE_DIR, static_cast<unsigned long long>(hash));
}

// Recognises "<16 hex>.xml" and yields its hash.
inline bool parseFileName(const char* name, uint64_t& hash) {
  if (strlen(name) != FILE_NAME_LEN || strcmp(name + 16, ".xml") != 0) return false;
  uint64_t value = 0;
  for (int i = 0; i < 16; i++) {
    const char c = name[i];
    uint8_t nibble;
    if (c >= '0' && c <= '9')
      nibble = static_cast<uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      nibble = static_cast<uint8_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      nibble = static_cast<uint8_t>(c - 'A' + 10);
    else
      return false;
    value = (value << 4) | nibble;
  }
  hash = value;
  return true;
}

// Parses the numeric prefix of an index line. `line` may be cut anywhere
// inside the URL (or omit it); only the four leading fields are needed.
// Returns the offset of the URL (one past the fourth space) or 0 on a
// malformed line.
inline size_t parseIndexLine(const char* line, Entry& out) {
  unsigned long long hash = 0;
  unsigned long bytes = 0;
  unsigned long fetchedAt = 0;
  unsigned long sessionId = 0;
  int consumed = 0;
  if (sscanf(line, "%llx %lu %lu %lu %n", &hash, &bytes, &fetchedAt, &sessionId, &consumed) != 4 || consumed <= 0) {
    return 0;
  }
  out.hash = static_cast<uint64_t>(hash);
  out.bytes = static_cast<uint32_t>(bytes);
  out.fetchedAt = static_cast<uint32_t>(fetchedAt);
  out.sessionId = static_cast<uint32_t>(sessionId);
  return static_cast<size_t>(consumed);
}

// Writes the numeric prefix "<hash> <bytes> <fetchedAt> <sessionId> " (with
// the trailing space, so the URL and newline follow). Returns its length.
inline size_t formatIndexPrefix(char* out, const size_t cap, const Entry& e) {
  const int n = snprintf(out, cap, "%016llx %lu %lu %lu ", static_cast<unsigned long long>(e.hash),
                         static_cast<unsigned long>(e.bytes), static_cast<unsigned long>(e.fetchedAt),
                         static_cast<unsigned long>(e.sessionId));
  if (n <= 0 || static_cast<size_t>(n) >= cap) return 0;
  return static_cast<size_t>(n);
}

// `now` is 0 when the clock is unsynced (see normaliseNow).
inline bool isFresh(const Entry& e, const uint32_t now, const uint32_t sessionId) {
  if (e.sessionId == sessionId) return true;
  return e.fetchedAt > 0 && now >= e.fetchedAt && now - e.fetchedAt <= MAX_AGE_S;
}

// Age for the hit log; 0 when either side has no synced clock.
inline uint32_t ageSeconds(const Entry& e, const uint32_t now) {
  return (e.fetchedAt > 0 && now >= e.fetchedAt) ? now - e.fetchedAt : 0;
}

// Maps time(nullptr) to the stored timestamp: 0 unless SNTP has synced.
inline uint32_t normaliseNow(const int64_t epoch) {
  return epoch >= static_cast<int64_t>(MIN_VALID_EPOCH) ? static_cast<uint32_t>(epoch) : 0;
}

// Records `fresh` as most recent, then evicts oldest-first while the index is
// over MAX_ENTRIES or MAX_TOTAL_BYTES. The fresh entry itself is never
// evicted. Evicted hashes are written to `evicted` (up to `evictedCap`);
// returns how many were evicted.
inline size_t insert(Index& index, const Entry& fresh, uint64_t* evicted, const size_t evictedCap) {
  index.append(fresh);
  size_t evictedCount = 0;
  while (index.count > 1 && (index.count > MAX_ENTRIES || index.totalBytes() > MAX_TOTAL_BYTES)) {
    if (evictedCount < evictedCap) evicted[evictedCount] = index.entries[0].hash;
    evictedCount++;
    index.removeAt(0);
  }
  return evictedCount;
}

}  // namespace opds_feed_cache
