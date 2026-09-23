#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "lib/OpdsFeedCache/OpdsFeedCache.h"

using namespace opds_feed_cache;

namespace {

Entry makeEntry(const uint64_t hash, const uint32_t bytes, const uint32_t fetchedAt = 0,
                const uint32_t sessionId = 0) {
  Entry e;
  e.hash = hash;
  e.bytes = bytes;
  e.fetchedAt = fetchedAt;
  e.sessionId = sessionId;
  return e;
}

}  // namespace

// --- Hashing -----------------------------------------------------------------

TEST(OpdsFeedCache, Fnv1a64MatchesReferenceVectors) {
  // Published FNV-1a 64 test vectors.
  EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ull);
  EXPECT_EQ(fnv1a64("a"), 0xaf63dc4c8601ec8cull);
  EXPECT_EQ(fnv1a64("foobar"), 0x85944171f73967e8ull);
}

TEST(OpdsFeedCache, FeedKeyIsStableAndAccountScoped) {
  const uint64_t k1 = feedKey("http://host/opds", "alice");
  EXPECT_EQ(k1, feedKey("http://host/opds", "alice"));
  EXPECT_NE(k1, feedKey("http://host/opds", "bob"));
  EXPECT_NE(k1, feedKey("http://host/opds/", "alice"));
  EXPECT_NE(k1, feedKey("http://host/opds", ""));
  // The separator keeps url/username boundaries distinct.
  EXPECT_NE(feedKey("ab", "c"), feedKey("a", "bc"));
  // Pinned so a future refactor cannot silently invalidate on-card indexes.
  EXPECT_EQ(feedKey("http://host/opds", "alice"), fnv1a64("alice", fnv1a64("\n", fnv1a64("http://host/opds"))));
}

TEST(OpdsFeedCache, FileNameRoundTrip) {
  char name[FILE_NAME_LEN + 1];
  formatFileName(name, sizeof(name), 0x0123456789abcdefull);
  EXPECT_STREQ(name, "0123456789abcdef.xml");
  uint64_t hash = 0;
  ASSERT_TRUE(parseFileName(name, hash));
  EXPECT_EQ(hash, 0x0123456789abcdefull);
  EXPECT_FALSE(parseFileName("index.txt", hash));
  EXPECT_FALSE(parseFileName("0123456789abcdef.tmp", hash));
  EXPECT_FALSE(parseFileName("0123456789abcdeg.xml", hash));
  EXPECT_FALSE(parseFileName("123456789abcdef.xml", hash));

  char path[PATH_CAP];
  formatPath(path, sizeof(path), 0xffffffffffffffffull);
  EXPECT_STREQ(path, "/.crosspoint/opds_cache/ffffffffffffffff.xml");
  EXPECT_LT(strlen(path), PATH_CAP);
}

// --- Index lines -------------------------------------------------------------

TEST(OpdsFeedCache, IndexLineRoundTrip) {
  const Entry in = makeEntry(0x00ab00cd00ef0012ull, 123456, 1758400000u, 0xdeadbeefu);
  const std::string url = "http://host/opds/catalog?page=2&q=a%20b";
  char prefix[LINE_PREFIX_CAP];
  const size_t n = formatIndexPrefix(prefix, sizeof(prefix), in);
  ASSERT_GT(n, 0u);
  EXPECT_STREQ(prefix, "00ab00cd00ef0012 123456 1758400000 3735928559 ");
  const std::string line = std::string(prefix) + url;

  Entry out;
  const size_t urlAt = parseIndexLine(line.c_str(), out);
  ASSERT_GT(urlAt, 0u);
  EXPECT_EQ(out.hash, in.hash);
  EXPECT_EQ(out.bytes, in.bytes);
  EXPECT_EQ(out.fetchedAt, in.fetchedAt);
  EXPECT_EQ(out.sessionId, in.sessionId);
  EXPECT_EQ(line.substr(urlAt), url);
}

TEST(OpdsFeedCache, IndexLineParsesTruncatedWindowAndRejectsGarbage) {
  // The store parses only a 64-byte window; the URL may be cut anywhere.
  Entry out;
  EXPECT_GT(parseIndexLine("0000000000000001 10 0 7 http://ho", out), 0u);
  EXPECT_EQ(out.hash, 1u);
  EXPECT_EQ(out.bytes, 10u);
  EXPECT_EQ(out.fetchedAt, 0u);
  EXPECT_EQ(out.sessionId, 7u);
  // Empty URL still parses.
  EXPECT_GT(parseIndexLine("0000000000000002 5 1 2 ", out), 0u);
  EXPECT_EQ(out.hash, 2u);
  EXPECT_EQ(parseIndexLine("", out), 0u);
  EXPECT_EQ(parseIndexLine("garbage line", out), 0u);
  EXPECT_EQ(parseIndexLine("0000000000000001 10", out), 0u);
}

// --- Freshness ---------------------------------------------------------------

TEST(OpdsFeedCache, HitRuleFreshWithinADay) {
  const uint32_t now = 1758400000u;
  EXPECT_TRUE(isFresh(makeEntry(1, 1, now - 60, 111), now, 999));
  EXPECT_TRUE(isFresh(makeEntry(1, 1, now - MAX_AGE_S, 111), now, 999));  // boundary inclusive
  EXPECT_TRUE(isFresh(makeEntry(1, 1, now, 111), now, 999));
  EXPECT_EQ(ageSeconds(makeEntry(1, 1, now - 60, 111), now), 60u);
}

TEST(OpdsFeedCache, HitRuleExpiredAfterADay) {
  const uint32_t now = 1758400000u;
  EXPECT_FALSE(isFresh(makeEntry(1, 1, now - MAX_AGE_S - 1, 111), now, 999));
  // A timestamp from the future is not trusted either.
  EXPECT_FALSE(isFresh(makeEntry(1, 1, now + 10, 111), now, 999));
  EXPECT_EQ(ageSeconds(makeEntry(1, 1, now + 10, 111), now), 0u);
}

TEST(OpdsFeedCache, HitRuleSameSessionIgnoresClock) {
  const uint32_t now = 1758400000u;
  EXPECT_TRUE(isFresh(makeEntry(1, 1, now - MAX_AGE_S * 10, 42), now, 42));  // stale but same session
  EXPECT_TRUE(isFresh(makeEntry(1, 1, 0, 42), 0, 42));                       // no clock on either side
  EXPECT_TRUE(isFresh(makeEntry(1, 1, 0, 42), now, 42));
}

TEST(OpdsFeedCache, HitRuleUnsyncedFetchNeedsSameSession) {
  const uint32_t now = 1758400000u;
  EXPECT_FALSE(isFresh(makeEntry(1, 1, 0, 42), now, 43));  // fetchedAt=0, other session
  EXPECT_FALSE(isFresh(makeEntry(1, 1, 0, 42), 0, 43));
  EXPECT_FALSE(isFresh(makeEntry(1, 1, now - 60, 42), 0, 43));  // synced then, unsynced now
}

TEST(OpdsFeedCache, NormaliseNowDropsUnsyncedClock) {
  EXPECT_EQ(normaliseNow(0), 0u);
  EXPECT_EQ(normaliseNow(86400 * 365), 0u);  // 1971: boot-epoch clock
  EXPECT_EQ(normaliseNow(1699999999), 0u);
  EXPECT_EQ(normaliseNow(1700000000), 1700000000u);
  EXPECT_EQ(normaliseNow(1758400000), 1758400000u);
}

// --- Index / LRU -------------------------------------------------------------

TEST(OpdsFeedCache, InsertAppendsMostRecentLastAndReplacesSameHash) {
  Index index;
  uint64_t evicted[MAX_ENTRIES];
  EXPECT_EQ(insert(index, makeEntry(1, 10), evicted, MAX_ENTRIES), 0u);
  EXPECT_EQ(insert(index, makeEntry(2, 20), evicted, MAX_ENTRIES), 0u);
  EXPECT_EQ(insert(index, makeEntry(3, 30), evicted, MAX_ENTRIES), 0u);
  ASSERT_EQ(index.count, 3u);
  EXPECT_EQ(index.entries[2].hash, 3u);

  // Refetching page 1 moves it to the most-recent slot with the new values.
  EXPECT_EQ(insert(index, makeEntry(1, 11, 5, 6), evicted, MAX_ENTRIES), 0u);
  ASSERT_EQ(index.count, 3u);
  EXPECT_EQ(index.entries[0].hash, 2u);
  EXPECT_EQ(index.entries[1].hash, 3u);
  EXPECT_EQ(index.entries[2].hash, 1u);
  EXPECT_EQ(index.entries[2].bytes, 11u);
  EXPECT_EQ(index.entries[2].fetchedAt, 5u);
  EXPECT_EQ(index.entries[2].sessionId, 6u);
  EXPECT_EQ(index.totalBytes(), 61u);
  EXPECT_EQ(index.find(3), 1);
  EXPECT_EQ(index.find(99), -1);
}

TEST(OpdsFeedCache, EvictsOldestByCount) {
  Index index;
  uint64_t evicted[MAX_ENTRIES];
  for (uint64_t h = 1; h <= MAX_ENTRIES; h++) {
    EXPECT_EQ(insert(index, makeEntry(h, 100), evicted, MAX_ENTRIES), 0u);
  }
  ASSERT_EQ(index.count, MAX_ENTRIES);

  EXPECT_EQ(insert(index, makeEntry(100, 100), evicted, MAX_ENTRIES), 1u);
  EXPECT_EQ(evicted[0], 1u);
  ASSERT_EQ(index.count, MAX_ENTRIES);
  EXPECT_EQ(index.entries[0].hash, 2u);
  EXPECT_EQ(index.entries[MAX_ENTRIES - 1].hash, 100u);

  EXPECT_EQ(insert(index, makeEntry(101, 100), evicted, MAX_ENTRIES), 1u);
  EXPECT_EQ(evicted[0], 2u);
  EXPECT_EQ(index.find(1), -1);
  EXPECT_EQ(index.find(2), -1);
  EXPECT_EQ(index.count, MAX_ENTRIES);
}

TEST(OpdsFeedCache, EvictsOldestByBytes) {
  Index index;
  uint64_t evicted[MAX_ENTRIES];
  // Four 128 KB pages fill the 512 KB budget exactly.
  const uint32_t page = MAX_TOTAL_BYTES / 4;
  for (uint64_t h = 1; h <= 4; h++) {
    EXPECT_EQ(insert(index, makeEntry(h, page), evicted, MAX_ENTRIES), 0u);
  }
  EXPECT_EQ(index.totalBytes(), MAX_TOTAL_BYTES);

  // One more byte over budget drops the oldest page.
  EXPECT_EQ(insert(index, makeEntry(5, 1), evicted, MAX_ENTRIES), 1u);
  EXPECT_EQ(evicted[0], 1u);
  EXPECT_EQ(index.count, 4u);
  EXPECT_LE(index.totalBytes(), MAX_TOTAL_BYTES);

  // A large page evicts as many oldest entries as it takes: 3 x 128 KB + 384 KB
  // + 1 byte needs three of the four older pages gone.
  EXPECT_EQ(insert(index, makeEntry(6, page * 3), evicted, MAX_ENTRIES), 3u);
  EXPECT_EQ(evicted[0], 2u);
  EXPECT_EQ(evicted[1], 3u);
  EXPECT_EQ(evicted[2], 4u);
  ASSERT_EQ(index.count, 2u);
  EXPECT_EQ(index.entries[0].hash, 5u);
  EXPECT_EQ(index.entries[1].hash, 6u);
  EXPECT_LE(index.totalBytes(), MAX_TOTAL_BYTES);
}

TEST(OpdsFeedCache, OversizedFreshEntryEvictsEverythingElseButStays) {
  Index index;
  uint64_t evicted[MAX_ENTRIES];
  insert(index, makeEntry(1, 100), evicted, MAX_ENTRIES);
  insert(index, makeEntry(2, 100), evicted, MAX_ENTRIES);
  EXPECT_EQ(insert(index, makeEntry(3, MAX_TOTAL_BYTES + 1), evicted, MAX_ENTRIES), 2u);
  EXPECT_EQ(evicted[0], 1u);
  EXPECT_EQ(evicted[1], 2u);
  ASSERT_EQ(index.count, 1u);
  EXPECT_EQ(index.entries[0].hash, 3u);
}

TEST(OpdsFeedCache, AppendDropsOldestWhenSpareSlotIsTaken) {
  // A corrupt index with more lines than MAX_ENTRIES cannot grow past the
  // spare slot; the oldest lines fall off.
  Index index;
  for (uint64_t h = 1; h <= MAX_ENTRIES + 3; h++) index.append(makeEntry(h, 1));
  EXPECT_EQ(index.count, MAX_ENTRIES + 1);
  EXPECT_EQ(index.entries[0].hash, 3u);
  EXPECT_EQ(index.entries[MAX_ENTRIES].hash, MAX_ENTRIES + 3);
}
