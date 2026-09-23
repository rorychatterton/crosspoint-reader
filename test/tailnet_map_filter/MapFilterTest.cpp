// Host-side tests for the targeted Tailscale MapResponse filter
// (lib/MicroLink/src/ml_map_filter.cpp).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ml_map_filter.h"

namespace {

uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d) { return (a << 24) | (b << 16) | (c << 8) | d; }

const uint32_t kSelfIp = ip(100, 64, 0, 1);
const uint32_t kTargetIp = ip(100, 64, 0, 42);
const uint32_t kResolverIp = ip(100, 64, 0, 53);

// A Node object as the coordination server emits it: IPv6 first, HomeDERP as
// a number.
const char* const kNode =
    R"("Node":{"ID":1,"Name":"reader.tail.example.","Key":"nodekey:00","Addresses":["fd7a:115c:a1e0::1/128","100.64.0.1/32"],"AllowedIPs":["fd7a:115c:a1e0::1/128","100.64.0.1/32"],"HomeDERP":7,"Hostinfo":{"OS":"esp32","Hostname":"reader"}})";

struct PeerSpec {
  std::string name = "peer.tail.example.";
  std::string nodeKey = "nodekey:aa";
  std::string discoKey = "discokey:bb";
  std::vector<std::string> addresses;
  std::vector<std::string> allowedIps;
  std::vector<std::string> endpoints;
  std::string derp = R"("HomeDERP":9)";
  bool allowedIpsFirst = false;
  std::string extra;  // raw JSON members appended before the closing brace
};

std::string stringArray(const char* key, const std::vector<std::string>& items) {
  std::string out = std::string("\"") + key + "\":[";
  for (size_t i = 0; i < items.size(); ++i) {
    if (i) out += ',';
    out += '"' + items[i] + '"';
  }
  return out + ']';
}

std::string peerJson(const PeerSpec& spec) {
  std::string out = "{\"Name\":\"" + spec.name + "\",\"Key\":\"" + spec.nodeKey + "\",\"DiscoKey\":\"" +
                    spec.discoKey + "\",";
  const std::string addresses = stringArray("Addresses", spec.addresses);
  const std::string allowed = stringArray("AllowedIPs", spec.allowedIps);
  if (spec.allowedIpsFirst) {
    out += allowed + ',' + addresses;
  } else {
    out += addresses + ',' + allowed;
  }
  out += ',' + stringArray("Endpoints", spec.endpoints) + ',' + spec.derp;
  if (!spec.extra.empty()) out += ',' + spec.extra;
  return out + '}';
}

PeerSpec targetPeer() {
  PeerSpec spec;
  spec.name = "target.tail.example.";
  spec.nodeKey = "nodekey:1111";
  spec.discoKey = "discokey:2222";
  spec.addresses = {"fd7a:115c:a1e0::2a/128", "100.64.0.42/32"};
  spec.allowedIps = {"fd7a:115c:a1e0::2a/128", "100.64.0.42/32"};
  spec.endpoints = {"192.0.2.42:41641", "[2001:db8::2a]:41641", "198.51.100.42:3478"};
  return spec;
}

PeerSpec resolverPeer() {
  PeerSpec spec;
  spec.name = "resolver.tail.example.";
  spec.nodeKey = "nodekey:5353";
  spec.discoKey = "discokey:5454";
  spec.addresses = {"100.64.0.53/32"};
  spec.allowedIps = {"100.64.0.53/32"};
  spec.endpoints = {"192.0.2.53:41641"};
  spec.derp = R"("HomeDERP":3)";
  return spec;
}

PeerSpec otherPeer(unsigned index) {
  PeerSpec spec;
  spec.name = "other-" + std::to_string(index) + ".tail.example.";
  spec.nodeKey = "nodekey:" + std::to_string(index);
  spec.discoKey = "discokey:" + std::to_string(index);
  spec.addresses = {"100.64.1." + std::to_string(index) + "/32"};
  spec.allowedIps = {"100.64.1." + std::to_string(index) + "/32"};
  spec.endpoints = {"192.0.2." + std::to_string(index) + ":41641"};
  return spec;
}

std::string peersJson(const std::vector<PeerSpec>& peers) {
  std::string out = "\"Peers\":[";
  for (size_t i = 0; i < peers.size(); ++i) {
    if (i) out += ',';
    out += peerJson(peers[i]);
  }
  return out + ']';
}

// Full MapResponse: Node, then Peers, then trailing sections the filter must skip.
std::string mapJson(const std::vector<PeerSpec>& peers, bool peersFirst = false) {
  std::string out = "{";
  if (peersFirst) {
    out += peersJson(peers) + "," + kNode;
  } else {
    out += std::string(kNode) + "," + peersJson(peers);
  }
  out += R"(,"DERPMap":{"Regions":{"7":{"RegionID":7,"Nodes":[{"Name":"7a","HostName":"derp7.example","DERPPort":443}]}}},"DNSConfig":{"Nameservers":["100.100.100.100"]},"ControlTime":"2026-09-16T00:00:00Z"})";
  return out;
}

std::string withLengthPrefix(const std::string& json, uint32_t length) {
  std::string out;
  out.push_back(static_cast<char>(length & 0xff));
  out.push_back(static_cast<char>((length >> 8) & 0xff));
  out.push_back(static_cast<char>((length >> 16) & 0xff));
  out.push_back(static_cast<char>((length >> 24) & 0xff));
  return out + json;
}

struct FeedResult {
  bool fed = true;
  bool finished = false;
  ml_map_filter_result_t result{};
};

FeedResult feedAll(const std::string& stream, uint32_t targetIp, const char* targetName, uint32_t secondaryIp,
        size_t chunk = 0) {
  FeedResult out;
  ml_map_filter_t* filter = ml_map_filter_create(targetIp, targetName, secondaryIp);
  if (!filter) {
    out.fed = false;
    return out;
  }
  if (chunk == 0) chunk = stream.size() ? stream.size() : 1;
  for (size_t offset = 0; offset < stream.size(); offset += chunk) {
    const size_t len = std::min(chunk, stream.size() - offset);
    if (!ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(stream.data() + offset), len)) out.fed = false;
  }
  out.finished = ml_map_filter_finish(filter, &out.result);
  ml_map_filter_destroy(filter);
  return out;
}

void expectSamePeer(const ml_map_filter_peer_t& a, const ml_map_filter_peer_t& b) {
  EXPECT_EQ(a.found, b.found);
  EXPECT_EQ(a.vpn_ip, b.vpn_ip);
  EXPECT_EQ(a.derp_region, b.derp_region);
  EXPECT_STREQ(a.hostname, b.hostname);
  EXPECT_STREQ(a.node_key, b.node_key);
  EXPECT_STREQ(a.disco_key, b.disco_key);
  EXPECT_EQ(a.route_ip, b.route_ip);
  EXPECT_EQ(a.route_bits, b.route_bits);
  ASSERT_EQ(a.endpoint_count, b.endpoint_count);
  for (unsigned i = 0; i < a.endpoint_count; ++i) {
    EXPECT_EQ(a.endpoints[i].ip, b.endpoints[i].ip);
    EXPECT_EQ(a.endpoints[i].port, b.endpoints[i].port);
  }
}

void expectSameResult(const ml_map_filter_result_t& a, const ml_map_filter_result_t& b) {
  EXPECT_EQ(a.self_ip, b.self_ip);
  EXPECT_EQ(a.self_derp_region, b.self_derp_region);
  expectSamePeer(a.peer, b.peer);
  expectSamePeer(a.secondary, b.secondary);
}

void expectTargetPeer(const ml_map_filter_peer_t& peer) {
  EXPECT_TRUE(peer.found);
  EXPECT_EQ(peer.vpn_ip, kTargetIp);
  EXPECT_STREQ(peer.hostname, "target.tail.example.");
  EXPECT_STREQ(peer.node_key, "nodekey:1111");
  EXPECT_STREQ(peer.disco_key, "discokey:2222");
  EXPECT_EQ(peer.derp_region, 9);
  ASSERT_EQ(peer.endpoint_count, 2);  // the IPv6 endpoint is skipped
  EXPECT_EQ(peer.endpoints[0].ip, ip(192, 0, 2, 42));
  EXPECT_EQ(peer.endpoints[0].port, 41641);
  EXPECT_EQ(peer.endpoints[1].ip, ip(198, 51, 100, 42));
  EXPECT_EQ(peer.endpoints[1].port, 3478);
}

}  // namespace

// ---------------------------------------------------------------- selection

TEST(MapFilter, SelectsTargetByAddress) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), targetPeer(), otherPeer(2)}), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.fed);
  ASSERT_TRUE(r.finished);
  EXPECT_EQ(r.result.self_ip, kSelfIp);
  EXPECT_EQ(r.result.self_derp_region, 7);
  expectTargetPeer(r.result.peer);
  // Matched on its own address: no route even though AllowedIPs carries the
  // peer's own /32, as every real MapResponse does.
  EXPECT_EQ(r.result.peer.route_bits, 0);
  EXPECT_EQ(r.result.peer.route_ip, 0u);
  EXPECT_FALSE(r.result.secondary.found);
}

TEST(MapFilter, SelectsTargetByFqdn) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), targetPeer()}), 0, "target.tail.example", 0);
  ASSERT_TRUE(r.fed);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
  EXPECT_EQ(r.result.peer.route_bits, 0);
}

TEST(MapFilter, SelectsTargetByFqdnCaseInsensitiveWithTrailingDot) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), targetPeer()}), 0, "Target.Tail.EXAMPLE", 0);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
}

TEST(MapFilter, SelectsTargetByShortName) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), targetPeer()}), 0, "TARGET", 0);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
  EXPECT_EQ(r.result.peer.route_bits, 0);
}

TEST(MapFilter, ShortNameMatchesPeerWithoutDomain) {
  PeerSpec bare = targetPeer();
  bare.name = "target";
  const FeedResult r = feedAll(mapJson({bare}), 0, "target", 0);
  ASSERT_TRUE(r.finished);
  EXPECT_STREQ(r.result.peer.hostname, "target");
}

TEST(MapFilter, NameDoesNotMatchPrefixOrOtherDomain) {
  PeerSpec longer = targetPeer();
  longer.name = "targeted.tail.example.";
  PeerSpec otherDomain = targetPeer();
  otherDomain.name = "target.other.example.";
  const FeedResult r = feedAll(mapJson({longer, otherDomain}), 0, "target.tail.example", 0);
  ASSERT_TRUE(r.fed);
  EXPECT_FALSE(r.finished);
  EXPECT_FALSE(r.result.peer.found);
  EXPECT_EQ(r.result.self_ip, kSelfIp);
}

TEST(MapFilter, ReportsMissingTargetButKeepsSelf) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), otherPeer(2)}), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.fed);
  EXPECT_FALSE(r.finished);
  EXPECT_FALSE(r.result.peer.found);
  EXPECT_EQ(r.result.self_ip, kSelfIp);
  EXPECT_EQ(r.result.self_derp_region, 7);
}

// --------------------------------------------------------------- AllowedIPs

TEST(MapFilter, SelectsRouterWhenTargetOnlyInAllowedIpsSlash32) {
  PeerSpec service = otherPeer(9);
  service.name = "svc-host.tail.example.";
  service.allowedIps = {"100.64.1.9/32", "100.100.100.100/32"};
  const uint32_t vip = ip(100, 100, 100, 100);
  const FeedResult r = feedAll(mapJson({otherPeer(1), service, otherPeer(2)}), vip, nullptr, 0);
  ASSERT_TRUE(r.fed);
  ASSERT_TRUE(r.finished);
  EXPECT_STREQ(r.result.peer.hostname, "svc-host.tail.example.");
  EXPECT_EQ(r.result.peer.vpn_ip, ip(100, 64, 1, 9));
  EXPECT_EQ(r.result.peer.route_ip, vip);
  EXPECT_EQ(r.result.peer.route_bits, 32);
}

TEST(MapFilter, SelectsRouterWhenTargetInsideWiderRoute) {
  PeerSpec router = otherPeer(8);
  router.name = "router.tail.example.";
  router.allowedIps = {"100.64.1.8/32", "10.10.12.0/23", "10.10.10.0/23", "10.10.11.0/24"};
  const FeedResult r = feedAll(mapJson({otherPeer(1), router}), ip(10, 10, 11, 7), nullptr, 0);
  ASSERT_TRUE(r.finished);
  EXPECT_STREQ(r.result.peer.hostname, "router.tail.example.");
  EXPECT_EQ(r.result.peer.vpn_ip, ip(100, 64, 1, 8));
  // First covering prefix wins; 10.10.12.0/23 does not cover 10.10.11.7.
  EXPECT_EQ(r.result.peer.route_ip, ip(10, 10, 10, 0));
  EXPECT_EQ(r.result.peer.route_bits, 23);
}

TEST(MapFilter, IgnoresIpv6AndDefaultRouteInAllowedIps) {
  PeerSpec exitNode = otherPeer(4);
  exitNode.name = "exit.tail.example.";
  exitNode.allowedIps = {"fd7a:115c:a1e0::4/128", "100.64.1.4/32", "0.0.0.0/0", "::/0"};
  const FeedResult r = feedAll(mapJson({exitNode}), ip(10, 20, 30, 40), nullptr, 0);
  ASSERT_TRUE(r.fed);
  EXPECT_FALSE(r.finished);
  EXPECT_FALSE(r.result.peer.found);
}

TEST(MapFilter, ExitNodeBeforeRouterDoesNotStealTarget) {
  PeerSpec exitNode = otherPeer(4);
  exitNode.name = "exit.tail.example.";
  exitNode.allowedIps = {"0.0.0.0/0", "::/0", "100.64.1.4/32"};
  PeerSpec router = otherPeer(5);
  router.name = "router.tail.example.";
  router.allowedIps = {"100.64.1.5/32", "10.20.30.0/24"};
  const FeedResult r = feedAll(mapJson({exitNode, router}), ip(10, 20, 30, 40), nullptr, 0);
  ASSERT_TRUE(r.finished);
  EXPECT_STREQ(r.result.peer.hostname, "router.tail.example.");
  EXPECT_EQ(r.result.peer.route_ip, ip(10, 20, 30, 0));
  EXPECT_EQ(r.result.peer.route_bits, 24);
}

TEST(MapFilter, AllowedIpsBeforeAddressesRouteMatch) {
  PeerSpec router = otherPeer(6);
  router.name = "router.tail.example.";
  router.allowedIps = {"100.64.1.6/32", "172.16.0.0/12"};
  router.allowedIpsFirst = true;
  const FeedResult r = feedAll(mapJson({router}), ip(172, 20, 1, 2), nullptr, 0);
  ASSERT_TRUE(r.finished);
  EXPECT_EQ(r.result.peer.vpn_ip, ip(100, 64, 1, 6));
  EXPECT_EQ(r.result.peer.route_ip, ip(172, 16, 0, 0));
  EXPECT_EQ(r.result.peer.route_bits, 12);
}

TEST(MapFilter, AllowedIpsBeforeAddressesOwnAddressMatch) {
  PeerSpec target = targetPeer();
  target.allowedIpsFirst = true;
  const FeedResult r = feedAll(mapJson({otherPeer(1), target}), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
  EXPECT_EQ(r.result.peer.route_bits, 0);
  EXPECT_EQ(r.result.peer.route_ip, 0u);
}

TEST(MapFilter, RoutableIpsInHostinfoDoNotSelectPeer) {
  PeerSpec peer = otherPeer(3);
  peer.extra = R"("Hostinfo":{"Hostname":"target","Services":[{"Proto":"tcp","Port":22}],"RoutableIPs":["10.99.0.0/24"],"NetInfo":{"PreferredDERP":2}})";
  const FeedResult r = feedAll(mapJson({peer}), ip(10, 99, 0, 5), "target", 0);
  ASSERT_TRUE(r.fed);
  EXPECT_FALSE(r.finished);
  EXPECT_FALSE(r.result.peer.found);
}

TEST(MapFilter, FirstMatchingPeerWins) {
  PeerSpec first = otherPeer(1);
  first.allowedIps = {"100.64.1.1/32", "10.30.0.0/16"};
  PeerSpec second = otherPeer(2);
  second.allowedIps = {"100.64.1.2/32", "10.30.5.0/24"};
  const FeedResult r = feedAll(mapJson({first, second}), ip(10, 30, 5, 9), nullptr, 0);
  ASSERT_TRUE(r.finished);
  EXPECT_STREQ(r.result.peer.hostname, "other-1.tail.example.");
  EXPECT_EQ(r.result.peer.route_bits, 16);
}

TEST(MapFilter, FirstMatchingPeerWinsAcrossNameAndAddress) {
  PeerSpec byName = otherPeer(1);
  byName.name = "target.tail.example.";
  const FeedResult r = feedAll(mapJson({byName, targetPeer()}), kTargetIp, "target", 0);
  ASSERT_TRUE(r.finished);
  EXPECT_EQ(r.result.peer.vpn_ip, ip(100, 64, 1, 1));
}

// ------------------------------------------------------------ Node parsing

TEST(MapFilter, PeersBeforeNode) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), targetPeer()}, true), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.fed);
  ASSERT_TRUE(r.finished);
  EXPECT_EQ(r.result.self_ip, kSelfIp);
  EXPECT_EQ(r.result.self_derp_region, 7);
  expectTargetPeer(r.result.peer);
}

TEST(MapFilter, ParsesLegacyDerpStringOnNodeAndPeer) {
  PeerSpec target = targetPeer();
  target.derp = R"("DERP":"127.3.3.40:11")";
  std::string json =
      R"({"Node":{"Name":"reader.tail.example.","Addresses":["100.64.0.1/32"],"DERP":"127.3.3.40:4"},)" +
      peersJson({target}) + "}";
  const FeedResult r = feedAll(json, kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.finished);
  EXPECT_EQ(r.result.self_ip, kSelfIp);
  EXPECT_EQ(r.result.self_derp_region, 4);
  EXPECT_EQ(r.result.peer.derp_region, 11);
}

TEST(MapFilter, MissingDerpLeavesRegionZero) {
  PeerSpec target = targetPeer();
  target.derp = R"("Online":true)";
  std::string json = R"({"Node":{"Addresses":["100.64.0.1/32"]},)" + peersJson({target}) + "}";
  const FeedResult r = feedAll(json, kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.finished);
  EXPECT_EQ(r.result.self_derp_region, 0);
  EXPECT_EQ(r.result.peer.derp_region, 0);
}

TEST(MapFilter, NodeAddressesDoNotLeakIntoPeerOrViceVersa) {
  // Node lists the target address in AllowedIPs (it never would, but the
  // filter must only consult peers).
  std::string json =
      R"({"Node":{"Name":"reader.tail.example.","Addresses":["100.64.0.1/32"],"AllowedIPs":["100.64.0.42/32"],"HomeDERP":7},)" +
      peersJson({otherPeer(1)}) + "}";
  const FeedResult r = feedAll(json, kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.fed);
  EXPECT_FALSE(r.finished);
  EXPECT_FALSE(r.result.peer.found);
  EXPECT_EQ(r.result.self_ip, kSelfIp);
}

// --------------------------------------------------------------- endpoints

TEST(MapFilter, CapsEndpointsAtMax) {
  PeerSpec target = targetPeer();
  target.endpoints.clear();
  for (unsigned i = 1; i <= ML_MAP_FILTER_MAX_ENDPOINTS + 4; ++i) {
    if (i == 3) target.endpoints.push_back("[2001:db8::3]:41641");
    target.endpoints.push_back("192.0.2." + std::to_string(i) + ":" + std::to_string(40000 + i));
  }
  const FeedResult r = feedAll(mapJson({target}), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.finished);
  ASSERT_EQ(r.result.peer.endpoint_count, ML_MAP_FILTER_MAX_ENDPOINTS);
  for (unsigned i = 0; i < ML_MAP_FILTER_MAX_ENDPOINTS; ++i) {
    EXPECT_EQ(r.result.peer.endpoints[i].ip, ip(192, 0, 2, i + 1)) << i;
    EXPECT_EQ(r.result.peer.endpoints[i].port, 40001 + i) << i;
  }
}

TEST(MapFilter, RejectsMalformedEndpoints) {
  PeerSpec target = targetPeer();
  target.endpoints = {"192.0.2.1", "192.0.2.1:70000", "300.0.2.1:41641", "192.0.2.7:41641"};
  const FeedResult r = feedAll(mapJson({target}), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.finished);
  ASSERT_EQ(r.result.peer.endpoint_count, 1);
  EXPECT_EQ(r.result.peer.endpoints[0].ip, ip(192, 0, 2, 7));
}

// --------------------------------------------------------------- secondary

TEST(MapFilter, SecondaryBeforeTarget) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), resolverPeer(), otherPeer(2), targetPeer()}), kTargetIp, nullptr,
                    kResolverIp);
  ASSERT_TRUE(r.fed);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
  EXPECT_TRUE(r.result.secondary.found);
  EXPECT_EQ(r.result.secondary.vpn_ip, kResolverIp);
  EXPECT_STREQ(r.result.secondary.hostname, "resolver.tail.example.");
  EXPECT_STREQ(r.result.secondary.node_key, "nodekey:5353");
  EXPECT_EQ(r.result.secondary.derp_region, 3);
  ASSERT_EQ(r.result.secondary.endpoint_count, 1);
  EXPECT_EQ(r.result.secondary.endpoints[0].ip, ip(192, 0, 2, 53));
  EXPECT_EQ(r.result.secondary.route_bits, 0);
}

TEST(MapFilter, SecondaryAfterTarget) {
  const FeedResult r = feedAll(mapJson({targetPeer(), otherPeer(1), resolverPeer()}), kTargetIp, nullptr, kResolverIp);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
  EXPECT_TRUE(r.result.secondary.found);
  EXPECT_EQ(r.result.secondary.vpn_ip, kResolverIp);
}

TEST(MapFilter, SecondaryAbsentFinishFalseButTargetFound) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), targetPeer()}), kTargetIp, nullptr, kResolverIp);
  ASSERT_TRUE(r.fed);
  EXPECT_FALSE(r.finished);
  EXPECT_EQ(r.result.self_ip, kSelfIp);
  expectTargetPeer(r.result.peer);
  EXPECT_FALSE(r.result.secondary.found);
}

TEST(MapFilter, SecondaryEqualToTargetFindsBoth) {
  const FeedResult r = feedAll(mapJson({otherPeer(1), targetPeer()}), kTargetIp, nullptr, kTargetIp);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
  EXPECT_TRUE(r.result.secondary.found);
  expectSamePeer(r.result.peer, r.result.secondary);
}

TEST(MapFilter, SecondaryNotSelectedByRoute) {
  // The resolver must own the address; a covering route is not enough.
  PeerSpec router = otherPeer(2);
  router.allowedIps = {"100.64.1.2/32", "100.64.0.0/16"};
  const FeedResult r = feedAll(mapJson({router, targetPeer()}), kTargetIp, nullptr, kResolverIp);
  EXPECT_FALSE(r.finished);
  EXPECT_FALSE(r.result.secondary.found);
  // ... but the router is the first peer whose AllowedIPs covers the target.
  EXPECT_TRUE(r.result.peer.found);
  EXPECT_EQ(r.result.peer.vpn_ip, ip(100, 64, 1, 2));
}

TEST(MapFilter, SecondaryRouteFieldsAreClearedEvenWhenItAdvertisesCoveringRoute) {
  PeerSpec resolver = resolverPeer();
  resolver.allowedIps = {"100.64.0.53/32", "100.64.0.0/16"};
  const FeedResult r = feedAll(mapJson({targetPeer(), resolver}), kTargetIp, nullptr, kResolverIp);
  ASSERT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
  EXPECT_EQ(r.result.peer.route_bits, 0);
  EXPECT_TRUE(r.result.secondary.found);
  EXPECT_EQ(r.result.secondary.vpn_ip, kResolverIp);
  EXPECT_EQ(r.result.secondary.route_bits, 0);
  EXPECT_EQ(r.result.secondary.route_ip, 0u);
}

// -------------------------------------------------------------- streaming

TEST(MapFilter, BytewiseAndChunkedFeedsAgree) {
  const std::string json = mapJson({otherPeer(1), resolverPeer(), otherPeer(2), targetPeer(), otherPeer(3)});
  const FeedResult whole = feedAll(json, kTargetIp, "target", kResolverIp);
  const FeedResult bytewise = feedAll(json, kTargetIp, "target", kResolverIp, 1);
  const FeedResult chunked = feedAll(json, kTargetIp, "target", kResolverIp, 37);
  const FeedResult large = feedAll(json, kTargetIp, "target", kResolverIp, 4096);
  ASSERT_TRUE(whole.fed);
  ASSERT_TRUE(whole.finished);
  for (const FeedResult* r : {&bytewise, &chunked, &large}) {
    EXPECT_TRUE(r->fed);
    EXPECT_TRUE(r->finished);
    expectSameResult(whole.result, r->result);
  }
}

TEST(MapFilter, TargetIsReadyBeforeStreamEnds) {
  const std::string json = mapJson({targetPeer(), otherPeer(1)});
  const size_t cut = json.find("other-1");
  ASSERT_NE(cut, std::string::npos);
  const FeedResult r = feedAll(json.substr(0, cut), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.fed);
  EXPECT_TRUE(r.finished);
  expectTargetPeer(r.result.peer);
}

TEST(MapFilter, MalformedJsonFailsFeedAndFinish) {
  const std::string json = R"({"Node":{"Addresses":["100.64.0.1/32"]},"Peers":nope})";
  ml_map_filter_t* filter = ml_map_filter_create(kTargetIp, nullptr, 0);
  ASSERT_NE(filter, nullptr);
  EXPECT_FALSE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(json.data()), json.size()));
  // The error is sticky.
  const std::string more = "{}";
  EXPECT_FALSE(ml_map_filter_feed(filter, reinterpret_cast<const uint8_t*>(more.data()), more.size()));
  ml_map_filter_result_t result{};
  EXPECT_FALSE(ml_map_filter_finish(filter, &result));
  ml_map_filter_destroy(filter);
}

TEST(MapFilter, FinishWithoutDataIsFalse) {
  ml_map_filter_t* filter = ml_map_filter_create(kTargetIp, nullptr, 0);
  ASSERT_NE(filter, nullptr);
  EXPECT_TRUE(ml_map_filter_feed(filter, nullptr, 0));
  ml_map_filter_result_t result{};
  EXPECT_FALSE(ml_map_filter_finish(filter, &result));
  EXPECT_FALSE(ml_map_filter_feed(filter, nullptr, 3));
  ml_map_filter_destroy(filter);
  EXPECT_FALSE(ml_map_filter_feed(nullptr, nullptr, 0));
  EXPECT_FALSE(ml_map_filter_finish(nullptr, &result));
  EXPECT_GT(ml_map_filter_allocation_size(), sizeof(ml_map_filter_result_t));
  ml_map_filter_destroy(nullptr);
}

// ----------------------------------------------------------- length prefix

TEST(MapFilter, RawJsonWithoutPrefix) {
  const std::string json = mapJson({targetPeer()});
  ASSERT_EQ(json[0], '{');
  for (size_t chunk : {size_t(0), size_t(1), size_t(3)}) {
    const FeedResult r = feedAll(json, kTargetIp, nullptr, 0, chunk);
    EXPECT_TRUE(r.fed) << chunk;
    EXPECT_TRUE(r.finished) << chunk;
    expectTargetPeer(r.result.peer);
  }
}

TEST(MapFilter, BinaryLengthPrefixIsSkipped) {
  const std::string json = mapJson({otherPeer(1), targetPeer()});
  const std::string framed = withLengthPrefix(json, static_cast<uint32_t>(json.size()));
  ASSERT_NE(framed[0], '{');
  for (size_t chunk : {size_t(0), size_t(1), size_t(2), size_t(5)}) {
    const FeedResult r = feedAll(framed, kTargetIp, nullptr, 0, chunk);
    EXPECT_TRUE(r.fed) << chunk;
    EXPECT_TRUE(r.finished) << chunk;
    expectTargetPeer(r.result.peer);
  }
}

TEST(MapFilter, LengthPrefixWithBraceInsideIsSkipped) {
  // 0x00007B00 = 31488 bytes: the second prefix byte is '{'.
  const std::string framed = withLengthPrefix(mapJson({targetPeer()}), 0x7B00);
  for (size_t chunk : {size_t(0), size_t(1)}) {
    const FeedResult r = feedAll(framed, kTargetIp, nullptr, 0, chunk);
    EXPECT_TRUE(r.fed) << chunk;
    EXPECT_TRUE(r.finished) << chunk;
  }
}

TEST(MapFilter, LengthPrefixStartingWithBraceByteIsSkipped) {
  // A MapResponse whose length is 123 mod 256 (here 0x17B = 379) starts with
  // 0x7B, the same byte as '{'. The following NUL bytes prove it is a prefix.
  const std::string json = mapJson({targetPeer()});
  const std::string framed = withLengthPrefix(json, 0x17B);
  ASSERT_EQ(framed[0], '{');
  for (size_t chunk : {size_t(0), size_t(1), size_t(2)}) {
    const FeedResult r = feedAll(framed, kTargetIp, nullptr, 0, chunk);
    EXPECT_TRUE(r.fed) << chunk;
    EXPECT_TRUE(r.finished) << chunk;
    expectTargetPeer(r.result.peer);
  }
}

// ---------------------------------------------------------------- hostname

TEST(MapFilter, TruncatesLongHostname) {
  const std::string label(80, 'x');
  PeerSpec target = targetPeer();
  target.name = label + ".tail.example.";
  const FeedResult r = feedAll(mapJson({target}), kTargetIp, nullptr, 0);
  ASSERT_TRUE(r.finished);
  EXPECT_EQ(std::strlen(r.result.peer.hostname), sizeof(r.result.peer.hostname) - 1);
  EXPECT_EQ(std::string(r.result.peer.hostname), target.name.substr(0, sizeof(r.result.peer.hostname) - 1));

  // Matching by the (equally truncated) long name still selects the peer.
  const FeedResult byName = feedAll(mapJson({otherPeer(1), target}), 0, label.c_str(), 0);
  EXPECT_TRUE(byName.finished);
  EXPECT_EQ(byName.result.peer.vpn_ip, kTargetIp);
}
