#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "ml_map_filter.h"

namespace {
uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) { return (a << 24) | (b << 16) | (c << 8) | d; }

std::string peer(unsigned index) {
  char text[512];
  std::snprintf(
      text, sizeof(text),
      R"({"Name":"peer-%u.tail.example.","Key":"nodekey:%064u","DiscoKey":"discokey:%064u","Addresses":["100.64.%u.%u/32"],"HomeDERP":%u,"Endpoints":["192.0.2.%u:41641"]})",
      index, index, index, index / 250, index % 250 + 1, index % 20 + 1, index % 250 + 1);
  return text;
}

std::string mapWithPeers(unsigned count) {
  std::string json = R"({"Node":{"Addresses":["100.64.0.1/32"],"HomeDERP":7},"Peers":[)";
  for (unsigned i = 0; i < count; ++i) {
    if (i) json += ',';
    json += peer(i);
  }
  json += R"(],"DERPMap":{"Regions":{}}})";
  return json;
}
}  // namespace

TEST(TailnetMapFilter, SelectsTargetAndSelfFromLargeBytewiseMap) {
  const std::string json = mapWithPeers(400);
  const uint32_t target = ip(100, 64, 0, 151);
  ml_map_filter_t* filter = ml_map_filter_create(target, nullptr, 0);
  ASSERT_NE(filter, nullptr);
  for (char byte : json) ASSERT_TRUE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(&byte), 1));
  ml_map_filter_result_t result{};
  ASSERT_TRUE(ml_map_filter_finish(filter, &result));
  EXPECT_EQ(result.self_ip, ip(100, 64, 0, 1));
  EXPECT_EQ(result.self_derp_region, 7);
  EXPECT_EQ(result.peer.vpn_ip, target);
  EXPECT_STREQ(result.peer.hostname, "peer-150.tail.example.");
  EXPECT_EQ(result.peer.derp_region, 11);
  ASSERT_EQ(result.peer.endpoint_count, 1);
  EXPECT_EQ(result.peer.endpoints[0].port, 41641);
  EXPECT_LT(ml_map_filter_allocation_size(), 2048u);
  ml_map_filter_destroy(filter);
}

TEST(TailnetMapFilter, ReportsMissingTarget) {
  const std::string json = mapWithPeers(3);
  ml_map_filter_t* filter = ml_map_filter_create(ip(100, 100, 100, 100), nullptr, 0);
  ASSERT_NE(filter, nullptr);
  ASSERT_TRUE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(json.data()), json.size()));
  ml_map_filter_result_t result{};
  EXPECT_FALSE(ml_map_filter_finish(filter, &result));
  EXPECT_EQ(result.self_ip, ip(100, 64, 0, 1));
  EXPECT_FALSE(result.peer.found);
  ml_map_filter_destroy(filter);
}

TEST(TailnetMapFilter, ReturnsTargetBeforeTrailingPeersArrive) {
  const std::string prefix =
      R"({"Node":{"Addresses":["100.64.0.1/32"],"HomeDERP":7},"Peers":[{"Name":"target.tail.example.","Key":"nodekey:aa","DiscoKey":"discokey:bb","Addresses":["100.64.0.42/32"],"HomeDERP":9,"Endpoints":["192.0.2.42:41641"]},{"Name":"unfinished)";
  ml_map_filter_t* filter = ml_map_filter_create(ip(100, 64, 0, 42), nullptr, 0);
  ASSERT_NE(filter, nullptr);
  ASSERT_TRUE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(prefix.data()), prefix.size()));
  ml_map_filter_result_t result{};
  ASSERT_TRUE(ml_map_filter_finish(filter, &result));
  EXPECT_EQ(result.self_ip, ip(100, 64, 0, 1));
  EXPECT_STREQ(result.peer.hostname, "target.tail.example.");
  EXPECT_EQ(result.peer.derp_region, 9);
  ml_map_filter_destroy(filter);
}

TEST(TailnetMapFilter, SelectsMagicDnsShortNameWithoutPreResolvedIp) {
  const std::string json = mapWithPeers(20);
  ml_map_filter_t* filter = ml_map_filter_create(0, "PEER-12", 0);
  ASSERT_NE(filter, nullptr);
  ASSERT_TRUE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(json.data()), json.size()));
  ml_map_filter_result_t result{};
  ASSERT_TRUE(ml_map_filter_finish(filter, &result));
  EXPECT_STREQ(result.peer.hostname, "peer-12.tail.example.");
  EXPECT_EQ(result.peer.vpn_ip, ip(100, 64, 0, 13));
  ml_map_filter_destroy(filter);
}

TEST(TailnetMapFilter, AcceptsLegacyDerpStringsAndCapsEndpoints) {
  std::string json =
      R"({"Node":{"Addresses":["100.64.0.1/32"],"DERP":"127.3.3.40:4"},"Peers":[{"Name":"target","Key":"nodekey:aa","DiscoKey":"discokey:bb","Addresses":["100.64.0.2/32"],"DERP":"127.3.3.40:9","Endpoints":[)";
  for (unsigned i = 0; i < 20; ++i) {
    if (i) json += ',';
    json += "\"192.0.2." + std::to_string(i + 1) + ":41641\"";
  }
  json += "]}]}";
  ml_map_filter_t* filter = ml_map_filter_create(ip(100, 64, 0, 2), nullptr, 0);
  ASSERT_NE(filter, nullptr);
  ASSERT_TRUE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(json.data()), json.size()));
  ml_map_filter_result_t result{};
  ASSERT_TRUE(ml_map_filter_finish(filter, &result));
  EXPECT_EQ(result.self_derp_region, 4);
  EXPECT_EQ(result.peer.derp_region, 9);
  EXPECT_EQ(result.peer.endpoint_count, ML_MAP_FILTER_MAX_ENDPOINTS);
  ml_map_filter_destroy(filter);
}

TEST(TailnetMapFilter, SkipsBinaryLengthPrefixEvenWhenItContainsOpeningBrace) {
  const std::string json = mapWithPeers(2);
  std::string framed("\0\x7b\0\x20", 4);
  framed += json;
  ml_map_filter_t* filter = ml_map_filter_create(ip(100, 64, 0, 2), nullptr, 0);
  ASSERT_NE(filter, nullptr);
  for (char byte : framed) ASSERT_TRUE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(&byte), 1));
  ml_map_filter_result_t result{};
  EXPECT_TRUE(ml_map_filter_finish(filter, &result));
  EXPECT_STREQ(result.peer.hostname, "peer-1.tail.example.");
  ml_map_filter_destroy(filter);
}
