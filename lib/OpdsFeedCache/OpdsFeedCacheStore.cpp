#include "OpdsFeedCacheStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

using namespace opds_feed_cache;

namespace {

// Chunked byte reader: every HalFile::read takes the storage mutex, so bytes
// are pulled 64 at a time and handed out one by one.
class ByteReader {
 public:
  explicit ByteReader(HalFile& f) : file(f) {}
  int next() {
    if (pos >= len) {
      const int n = file.read(buf, sizeof(buf));
      if (n <= 0) return -1;
      len = n;
      pos = 0;
    }
    return buf[pos++];
  }
  void unread() { pos--; }  // only valid right after a successful next()

 private:
  HalFile& file;
  uint8_t buf[64];
  int len = 0;
  int pos = 0;
};

enum class LineHead { END_OF_FILE, COMPLETE, MORE };

// Reads up to cap-1 chars of the next line into `out` (NUL-terminated).
// MORE means the line continues past the window: skip or copy the rest.
LineHead readLineHead(ByteReader& reader, char* out, const size_t cap, size_t& outLen) {
  outLen = 0;
  int c;
  while ((c = reader.next()) >= 0) {
    if (c == '\n') {
      out[outLen] = 0;
      return LineHead::COMPLETE;
    }
    if (outLen + 1 >= cap) {
      reader.unread();
      out[outLen] = 0;
      return LineHead::MORE;
    }
    out[outLen++] = static_cast<char>(c);
  }
  out[outLen] = 0;
  return outLen > 0 ? LineHead::COMPLETE : LineHead::END_OF_FILE;
}

void skipRestOfLine(ByteReader& reader) {
  int c;
  while ((c = reader.next()) >= 0 && c != '\n') {
  }
}

// Copies the rest of the line (without its newline) to `out`.
bool copyRestOfLine(ByteReader& reader, HalFile& out) {
  uint8_t stage[64];
  size_t n = 0;
  int c;
  while ((c = reader.next()) >= 0 && c != '\n') {
    stage[n++] = static_cast<uint8_t>(c);
    if (n == sizeof(stage)) {
      if (out.write(stage, n) != n) return false;
      n = 0;
    }
  }
  return n == 0 || out.write(stage, n) == n;
}

// Reads the numeric prefixes of index.txt into `index` (oldest first).
void loadIndex(Index& index) {
  index.count = 0;
  if (!Storage.exists(INDEX_PATH)) return;
  HalFile in;
  if (!Storage.openFileForRead("OPDS", INDEX_PATH, in)) return;
  ByteReader reader(in);
  char head[LINE_PREFIX_CAP];
  size_t len = 0;
  for (;;) {
    const LineHead st = readLineHead(reader, head, sizeof(head), len);
    if (st == LineHead::END_OF_FILE) break;
    if (st == LineHead::MORE) skipRestOfLine(reader);
    Entry e;
    if (parseIndexLine(head, e) > 0) index.append(e);
  }
}

// Streams the old index into index.tmp keeping only lines still in `index`
// (the fresh entry's line is rewritten from `url`), then swaps it in.
bool rewriteIndex(const Index& index, const Entry& fresh, const std::string_view url) {
  HalFile out;
  if (!Storage.openFileForWrite("OPDS", INDEX_TMP_PATH, out)) return false;
  bool written[MAX_ENTRIES + 1] = {};
  bool ok = true;
  char prefix[LINE_PREFIX_CAP];

  if (Storage.exists(INDEX_PATH)) {
    HalFile in;
    if (Storage.openFileForRead("OPDS", INDEX_PATH, in)) {
      ByteReader reader(in);
      char head[LINE_PREFIX_CAP];
      size_t len = 0;
      for (;;) {
        const LineHead st = readLineHead(reader, head, sizeof(head), len);
        if (st == LineHead::END_OF_FILE) break;
        Entry e;
        const size_t urlAt = parseIndexLine(head, e);
        const int slot = urlAt > 0 ? index.find(e.hash) : -1;
        const bool keep = slot >= 0 && e.hash != fresh.hash && !written[slot];
        if (!keep) {
          if (st == LineHead::MORE) skipRestOfLine(reader);
          continue;
        }
        written[slot] = true;
        const size_t pn = formatIndexPrefix(prefix, sizeof(prefix), index.entries[slot]);
        const size_t tail = len - urlAt;
        ok = ok && pn > 0 && out.write(prefix, pn) == pn && out.write(head + urlAt, tail) == tail;
        if (st == LineHead::MORE) ok = ok && copyRestOfLine(reader, out);
        ok = ok && out.write("\n", 1) == 1;
      }
    }
  }

  const size_t pn = formatIndexPrefix(prefix, sizeof(prefix), fresh);
  ok = ok && pn > 0 && out.write(prefix, pn) == pn && out.write(url.data(), url.size()) == url.size() &&
       out.write("\n", 1) == 1;
  ok = out.close() && ok;
  if (!ok) {
    Storage.remove(INDEX_TMP_PATH);
    return false;
  }
  if (Storage.exists(INDEX_PATH) && !Storage.remove(INDEX_PATH)) return false;
  return Storage.rename(INDEX_TMP_PATH, INDEX_PATH);
}

struct CommitScratch {
  Index index;
  uint64_t evicted[MAX_ENTRIES];
};

}  // namespace

bool OpdsFeedCacheStore::lookup(const uint64_t hash, const uint32_t now, const uint32_t sessionId, Entry& out) {
  if (!Storage.exists(INDEX_PATH)) return false;
  HalFile in;
  if (!Storage.openFileForRead("OPDS", INDEX_PATH, in)) return false;
  ByteReader reader(in);
  char head[LINE_PREFIX_CAP];
  size_t len = 0;
  bool found = false;
  for (;;) {
    const LineHead st = readLineHead(reader, head, sizeof(head), len);
    if (st == LineHead::END_OF_FILE) break;
    if (st == LineHead::MORE) skipRestOfLine(reader);
    Entry e;
    if (parseIndexLine(head, e) > 0 && e.hash == hash) {
      out = e;  // a later duplicate line wins (most recent)
      found = true;
    }
  }
  return found && isFresh(out, now, sessionId);
}

bool OpdsFeedCacheStore::commit(const char* spoolPath, const Entry& entry, const std::string_view url) {
  if (!Storage.ensureDirectoryExists(CACHE_DIR)) {
    LOG_ERR("OPDS", "Feed cache dir unavailable: %s", CACHE_DIR);
    return false;
  }
  char path[PATH_CAP];
  formatPath(path, sizeof(path), entry.hash);
  if (Storage.exists(path) && !Storage.remove(path)) {
    LOG_ERR("OPDS", "Feed cache replace failed: %s", path);
    return false;
  }
  if (!Storage.rename(spoolPath, path)) {
    LOG_ERR("OPDS", "Feed cache rename failed: %s", path);
    return false;
  }

  // From here the page lives at `path`; index trouble only costs the next hit
  // (an unindexed page is swept as an orphan on the next browser entry).
  auto scratch = makeUniqueNoThrow<CommitScratch>();
  if (!scratch) {
    LOG_ERR("OPDS", "OOM: feed cache index scratch");
    return true;
  }
  loadIndex(scratch->index);
  const size_t evictedCount = insert(scratch->index, entry, scratch->evicted, MAX_ENTRIES);
  if (!rewriteIndex(scratch->index, entry, url)) {
    LOG_ERR("OPDS", "Feed cache index rewrite failed");
    return true;
  }
  for (size_t i = 0; i < evictedCount && i < MAX_ENTRIES; i++) {
    formatPath(path, sizeof(path), scratch->evicted[i]);
    if (Storage.exists(path)) Storage.remove(path);
    LOG_DBG("OPDS", "Feed cache evicted %s", path);
  }
  return true;
}

void OpdsFeedCacheStore::sweepOrphans() {
  if (!Storage.exists(CACHE_DIR)) return;
  auto index = makeUniqueNoThrow<Index>();
  if (!index) return;
  loadIndex(*index);
  const auto names = Storage.listFiles(CACHE_DIR, 64);
  for (const auto& name : names) {
    if (name == "index.txt") continue;
    uint64_t hash = 0;
    if (parseFileName(name.c_str(), hash) && index->find(hash) >= 0) continue;
    char path[sizeof(CACHE_DIR) + 1 + 128];
    const int n = snprintf(path, sizeof(path), "%s/%s", CACHE_DIR, name.c_str());
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(path)) continue;
    Storage.remove(path);
    LOG_DBG("OPDS", "Feed cache orphan removed %s", path);
  }
}
