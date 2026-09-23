#include <gtest/gtest.h>

#include <string>

#include "ProgressResponseParser.h"

using koreader_sync::parseProgressResponse;

namespace {

constexpr const char* kFullRecord =
    R"({"document":"abc","progress":"/body/DocFragment[3]/body/p[7]/text().0","percentage":0.4231,)"
    R"("device":"CrossPoint","device_id":"DEADBEEF","timestamp":1758200000,)"
    R"("position":{"pctQ":423100,"spine":3,"page":7,"pages":12,"para":42,"xpath":"/body/DocFragment[3]/body/p[7]"}})";

KOReaderProgress dirtyProgress() {
  // Pre-populated so tests catch fields the parser forgets to overwrite.
  KOReaderProgress p;
  p.document = "stale";
  p.progress = "stale";
  p.percentage = 0.99f;
  p.device = "stale";
  p.deviceId = "stale";
  p.timestamp = 7;
  KOReaderRichPosition pos;
  pos.spineIndex = 99;
  p.position = pos;
  return p;
}

}  // namespace

TEST(ProgressResponseParser, FullRecordOnCrossPointServer) {
  KOReaderProgress out = dirtyProgress();
  ASSERT_TRUE(parseProgressResponse(kFullRecord, /*crossPointServer=*/true, out));

  EXPECT_EQ(out.document, "stale");  // caller-owned; the body's document field is ignored
  EXPECT_EQ(out.progress, "/body/DocFragment[3]/body/p[7]/text().0");
  EXPECT_FLOAT_EQ(out.percentage, 0.4231f);
  EXPECT_EQ(out.device, "CrossPoint");
  EXPECT_EQ(out.deviceId, "DEADBEEF");
  EXPECT_EQ(out.timestamp, 1758200000);

  ASSERT_TRUE(out.position.has_value());
  EXPECT_EQ(out.position->pctQ, 423100u);
  EXPECT_EQ(out.position->spineIndex, 3);
  EXPECT_EQ(out.position->pageNumber, 7);
  EXPECT_EQ(out.position->totalPages, 12);
  ASSERT_TRUE(out.position->paragraphIndex.has_value());
  EXPECT_EQ(*out.position->paragraphIndex, 42);
  EXPECT_EQ(out.position->xpath, "/body/DocFragment[3]/body/p[7]");
}

TEST(ProgressResponseParser, EmptyObjectParsesAsZeroProgress) {
  KOReaderProgress out = dirtyProgress();
  ASSERT_TRUE(parseProgressResponse("{}", /*crossPointServer=*/true, out));
  EXPECT_EQ(out.progress, "");
  EXPECT_FLOAT_EQ(out.percentage, 0.0f);
  EXPECT_EQ(out.device, "");
  EXPECT_EQ(out.deviceId, "");
  EXPECT_EQ(out.timestamp, 0);
  EXPECT_FALSE(out.position.has_value());
}

TEST(ProgressResponseParser, PositionIgnoredOnStockKOSyncServer) {
  KOReaderProgress out = dirtyProgress();
  ASSERT_TRUE(parseProgressResponse(kFullRecord, /*crossPointServer=*/false, out));
  EXPECT_FLOAT_EQ(out.percentage, 0.4231f);
  EXPECT_EQ(out.progress, "/body/DocFragment[3]/body/p[7]/text().0");
  EXPECT_FALSE(out.position.has_value());  // stale position cleared, body's not trusted
}

TEST(ProgressResponseParser, PositionDefaultsClampPagesAndOmitParagraph) {
  KOReaderProgress out;
  ASSERT_TRUE(parseProgressResponse(R"({"percentage":0.1,"position":{"spine":2,"page":0,"pages":0,"para":0}})",
                                    /*crossPointServer=*/true, out));
  ASSERT_TRUE(out.position.has_value());
  EXPECT_EQ(out.position->spineIndex, 2);
  EXPECT_EQ(out.position->pageNumber, 0);
  EXPECT_EQ(out.position->totalPages, 1);  // 0 pages clamps to 1
  EXPECT_FALSE(out.position->paragraphIndex.has_value());
  EXPECT_EQ(out.position->xpath, "");
}

TEST(ProgressResponseParser, NonObjectPositionIsIgnored) {
  KOReaderProgress out;
  ASSERT_TRUE(parseProgressResponse(R"({"percentage":0.1,"position":"not-an-object"})",
                                    /*crossPointServer=*/true, out));
  EXPECT_FALSE(out.position.has_value());
}

TEST(ProgressResponseParser, MalformedJsonFailsWithReason) {
  KOReaderProgress out = dirtyProgress();
  const char* reason = nullptr;
  EXPECT_FALSE(parseProgressResponse(R"({"percentage":0.4,)", /*crossPointServer=*/true, out, &reason));
  ASSERT_NE(reason, nullptr);
  EXPECT_STRNE(reason, "");
}

TEST(ProgressResponseParser, EmptyBodyFails) {
  KOReaderProgress out;
  EXPECT_FALSE(parseProgressResponse("", /*crossPointServer=*/false, out));
}
