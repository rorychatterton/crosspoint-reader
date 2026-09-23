#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "ml_h2_stream.h"
#include "ml_map_filter.h"
#include "ml_memory_budget.h"

namespace {
std::vector<uint8_t> frame(uint8_t type, uint8_t flags, uint32_t stream, const std::string& body) {
  const uint32_t len = body.size();
  std::vector<uint8_t> out = {static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 8),
                              static_cast<uint8_t>(len), type, flags,
                              static_cast<uint8_t>(stream >> 24), static_cast<uint8_t>(stream >> 16),
                              static_cast<uint8_t>(stream >> 8), static_cast<uint8_t>(stream)};
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

bool append(void* ctx, const uint8_t* data, size_t len) {
  static_cast<std::string*>(ctx)->append(reinterpret_cast<const char*>(data), len);
  return true;
}

bool filterData(void* ctx, const uint8_t* data, size_t len) {
  return ml_map_filter_feed(static_cast<ml_map_filter_t*>(ctx), data, len);
}

struct ByteSummary {
  size_t length = 0;
  uint32_t sum = 0;
};

bool summarise(void* ctx, const uint8_t* data, size_t len) {
  auto& summary = *static_cast<ByteSummary*>(ctx);
  summary.length += len;
  for (size_t i = 0; i < len; ++i) summary.sum += data[i];
  return true;
}
}  // namespace

TEST(TailnetH2Stream, ReassemblesBytewiseHeaderAndPayload) {
  const auto bytes = frame(0, 1, 3, "{\"Peers\":[]}");
  ml_h2_stream_t stream;
  ml_h2_stream_init(&stream);
  std::string received;
  for (uint8_t byte : bytes) ASSERT_TRUE(ml_h2_stream_feed(&stream, &byte, 1, 3, append, &received));
  EXPECT_EQ(received, "{\"Peers\":[]}");
  EXPECT_TRUE(stream.end_stream);
}

TEST(TailnetH2Stream, SkipsOtherFramesAndStreams) {
  auto bytes = frame(1, 0, 3, "headers");
  const auto wrong = frame(0, 0, 5, "wrong");
  const auto first = frame(0, 0, 3, "abc");
  const auto last = frame(0, 1, 3, "def");
  bytes.insert(bytes.end(), wrong.begin(), wrong.end());
  bytes.insert(bytes.end(), first.begin(), first.end());
  bytes.insert(bytes.end(), last.begin(), last.end());
  ml_h2_stream_t stream;
  ml_h2_stream_init(&stream);
  std::string received;
  ASSERT_TRUE(ml_h2_stream_feed(&stream, bytes.data(), bytes.size(), 3, append, &received));
  EXPECT_EQ(received, "abcdef");
  EXPECT_TRUE(stream.end_stream);
}

TEST(TailnetH2Stream, RecognizesHeadersOnlyEndStream) {
  const auto bytes = frame(1, 1, 1, "compressed-headers");
  ml_h2_stream_t stream;
  ml_h2_stream_init(&stream);
  std::string received;
  ASSERT_TRUE(ml_h2_stream_feed(&stream, bytes.data(), bytes.size(), 1, append, &received));
  EXPECT_TRUE(stream.end_stream);
  EXPECT_TRUE(received.empty());
}

TEST(TailnetH2Stream, HandlesPaddedData) {
  std::string payload;
  payload.push_back(2);
  payload += "json";
  payload += "xx";
  const auto bytes = frame(0, 0x09, 3, payload);
  ml_h2_stream_t stream;
  ml_h2_stream_init(&stream);
  std::string received;
  ASSERT_TRUE(ml_h2_stream_feed(&stream, bytes.data(), bytes.size(), 3, append, &received));
  EXPECT_EQ(received, "json");
  EXPECT_TRUE(stream.end_stream);
}

TEST(TailnetH2Stream, RejectsImpossiblePadding) {
  std::string payload;
  payload.push_back(8);
  payload += "x";
  const auto bytes = frame(0, 0x08, 3, payload);
  ml_h2_stream_t stream;
  ml_h2_stream_init(&stream);
  EXPECT_FALSE(ml_h2_stream_feed(&stream, bytes.data(), bytes.size(), 3, append, nullptr));
  EXPECT_TRUE(stream.error);
}

TEST(TailnetH2Stream, FiltersLargeMapAcrossTransportAndH2Boundaries) {
  std::string map = R"({"Node":{"Addresses":["100.64.0.1/32"],"HomeDERP":3},"Peers":[)";
  for (unsigned i = 1; i <= 400; ++i) {
    if (i > 1) map += ',';
    map += "{\"Name\":\"peer-" + std::to_string(i) +
           "\",\"Key\":\"nodekey:aa\",\"DiscoKey\":\"discokey:bb\",\"Addresses\":[\"100.64.1." +
           std::to_string(i % 250 + 1) + "/32\"],\"HomeDERP\":8}";
  }
  map += "]}";

  std::vector<uint8_t> encoded;
  for (size_t offset = 0; offset < map.size(); offset += 4096) {
    const size_t length = std::min<size_t>(4096, map.size() - offset);
    const bool last = offset + length == map.size();
    auto part = frame(0, last ? 1 : 0, 3, map.substr(offset, length));
    encoded.insert(encoded.end(), part.begin(), part.end());
  }

  ml_map_filter_t* filter = ml_map_filter_create((100u << 24) | (64u << 16) | (1u << 8) | 201u, nullptr, 0);
  ASSERT_NE(filter, nullptr);
  ml_h2_stream_t stream;
  ml_h2_stream_init(&stream);
  for (size_t offset = 0; offset < encoded.size(); offset += 37) {
    const size_t length = std::min<size_t>(37, encoded.size() - offset);
    ASSERT_TRUE(ml_h2_stream_feed(&stream, encoded.data() + offset, length, 3, filterData, filter));
  }
  ml_map_filter_result_t result{};
  ASSERT_TRUE(ml_map_filter_finish(filter, &result));
  EXPECT_TRUE(stream.end_stream);
  EXPECT_STREQ(result.peer.hostname, "peer-200");
  ml_map_filter_destroy(filter);
}

TEST(TailnetH2Stream, DrainsMaximumHttp2DataFrameAcrossIrregularReads) {
  std::string body(16384, '\0');
  uint32_t expectedSum = 0;
  for (size_t i = 0; i < body.size(); ++i) {
    body[i] = static_cast<char>((i * 37u) & 0xffu);
    expectedSum += static_cast<uint8_t>(body[i]);
  }
  const auto encoded = frame(0, 1, 1, body);
  ASSERT_EQ(encoded.size(), 16393u);
  ASSERT_LE(encoded.size(), ML_TARGET_MAP_PLAINTEXT_CAPACITY);

  ml_h2_stream_t stream;
  ml_h2_stream_init(&stream);
  ByteSummary received;
  const size_t chunks[] = {1, 2, 7, 31, 257, 4093};
  size_t offset = 0;
  size_t chunkIndex = 0;
  while (offset < encoded.size()) {
    const size_t chunkCount = sizeof(chunks) / sizeof(chunks[0]);
    const size_t length = std::min(chunks[chunkIndex++ % chunkCount], encoded.size() - offset);
    ASSERT_TRUE(ml_h2_stream_feed(&stream, encoded.data() + offset, length, 1, summarise, &received));
    offset += length;
  }
  EXPECT_TRUE(stream.end_stream);
  EXPECT_EQ(received.length, body.size());
  EXPECT_EQ(received.sum, expectedSum);
}
