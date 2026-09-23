#include "ml_map_filter.h"

#include <StreamingJsonParser.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
uint32_t parseIp(const char* value) {
  unsigned a, b, c, d;
  if (std::sscanf(value, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
    return 0;
  }
  return (a << 24) | (b << 16) | (c << 8) | d;
}

bool parseEndpoint(const char* value, ml_map_filter_endpoint_t& endpoint) {
  unsigned a, b, c, d, port;
  if (std::sscanf(value, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) != 5 || a > 255 || b > 255 ||
      c > 255 || d > 255 || port > 65535) {
    return false;
  }
  endpoint.ip = (a << 24) | (b << 16) | (c << 8) | d;
  endpoint.port = static_cast<uint16_t>(port);
  return true;
}

bool parseCidr(const char* value, uint32_t& ip, uint8_t& bits) {
  unsigned a, b, c, d, prefix;
  if (std::sscanf(value, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &prefix) != 5 || a > 255 || b > 255 || c > 255 ||
      d > 255 || prefix > 32) {
    return false;
  }
  ip = (a << 24) | (b << 16) | (c << 8) | d;
  bits = static_cast<uint8_t>(prefix);
  return true;
}

bool prefixContains(uint32_t prefixIp, uint8_t bits, uint32_t ip) {
  const uint32_t mask = bits == 0 ? 0U : (0xFFFFFFFFU << (32 - bits));
  return (prefixIp & mask) == (ip & mask);
}

void copyText(char* dest, size_t capacity, const char* value, size_t len) {
  if (capacity == 0) return;
  if (len >= capacity) len = capacity - 1;
  std::memcpy(dest, value, len);
  dest[len] = '\0';
}

bool sameHost(const char* candidate, const char* target) {
  if (!target || !target[0]) return false;
  size_t candidateLen = std::strlen(candidate);
  if (candidateLen > 0 && candidate[candidateLen - 1] == '.') --candidateLen;
  const char* dot = static_cast<const char*>(std::memchr(candidate, '.', candidateLen));
  const size_t shortLen = dot ? static_cast<size_t>(dot - candidate) : candidateLen;
  const size_t targetLen = std::strlen(target);
  const bool shortTarget = std::memchr(target, '.', targetLen) == nullptr;
  const size_t compareLen = shortTarget ? shortLen : candidateLen;
  if (targetLen != compareLen) return false;
  for (size_t i = 0; i < compareLen; ++i) {
    char a = candidate[i];
    char b = target[i];
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (a != b) return false;
  }
  return true;
}
}  // namespace

struct ml_map_filter_s {
  ml_map_filter_s(uint32_t wanted, const char* wantedHost, uint32_t secondary, const JsonCallbacks& callbacks)
      : parser(callbacks), targetIp(wanted), secondaryIp(secondary) {
    if (wantedHost) copyText(targetHost, sizeof(targetHost), wantedHost, std::strlen(wantedHost));
  }

  StreamingJsonParser parser;
  uint32_t targetIp;
  uint32_t secondaryIp;
  char targetHost[64]{};
  ml_map_filter_result_t result{};
  ml_map_filter_peer_t candidate{};
  char key[32]{};
  char arrayKey[32]{};
  uint8_t depth = 0;
  uint8_t nodeDepth = 0;
  uint8_t peersArrayDepth = 0;
  uint8_t peerDepth = 0;
  bool inNode = false;
  bool inPeers = false;
  bool inPeer = false;
  bool started = false;
  /* First bytes of the stream, held until the 4-byte length prefix can be
   * told apart from a bare JSON object. */
  uint8_t head[4]{};
  uint8_t prefixBytes = 0;
};

namespace {
void onKey(void* ctx, const char* key, size_t len) {
  auto& f = *static_cast<ml_map_filter_t*>(ctx);
  copyText(f.key, sizeof(f.key), key, len);
}

void onString(void* ctx, const char* value, size_t len) {
  auto& f = *static_cast<ml_map_filter_t*>(ctx);
  if (f.inNode) {
    if (std::strcmp(f.arrayKey, "Addresses") == 0 && f.result.self_ip == 0) f.result.self_ip = parseIp(value);
    if (std::strcmp(f.key, "DERP") == 0) {
      const char* colon = std::strrchr(value, ':');
      if (colon) f.result.self_derp_region = static_cast<uint16_t>(std::atoi(colon + 1));
    }
  }
  if (!f.inPeer) return;
  if (std::strcmp(f.key, "Name") == 0) copyText(f.candidate.hostname, sizeof(f.candidate.hostname), value, len);
  if (std::strcmp(f.key, "Key") == 0) copyText(f.candidate.node_key, sizeof(f.candidate.node_key), value, len);
  if (std::strcmp(f.key, "DiscoKey") == 0) copyText(f.candidate.disco_key, sizeof(f.candidate.disco_key), value, len);
  if (std::strcmp(f.arrayKey, "Addresses") == 0 && f.candidate.vpn_ip == 0) f.candidate.vpn_ip = parseIp(value);
  // Subnet routes and VIP services appear only in AllowedIPs, never in
  // Addresses. Remember the first prefix that covers the target.
  if (std::strcmp(f.arrayKey, "AllowedIPs") == 0 && f.targetIp != 0 && f.candidate.route_bits == 0) {
    uint32_t routeIp;
    uint8_t routeBits;
    if (parseCidr(value, routeIp, routeBits) && prefixContains(routeIp, routeBits, f.targetIp)) {
      f.candidate.route_ip = routeIp;
      f.candidate.route_bits = routeBits;
    }
  }
  if (std::strcmp(f.arrayKey, "Endpoints") == 0 && f.candidate.endpoint_count < ML_MAP_FILTER_MAX_ENDPOINTS) {
    auto& endpoint = f.candidate.endpoints[f.candidate.endpoint_count];
    if (parseEndpoint(value, endpoint)) ++f.candidate.endpoint_count;
  }
  if (std::strcmp(f.key, "DERP") == 0) {
    const char* colon = std::strrchr(value, ':');
    if (colon) f.candidate.derp_region = static_cast<uint16_t>(std::atoi(colon + 1));
  }
}

void onNumber(void* ctx, const char* value, size_t) {
  auto& f = *static_cast<ml_map_filter_t*>(ctx);
  if (std::strcmp(f.key, "HomeDERP") != 0) return;
  const uint16_t region = static_cast<uint16_t>(std::atoi(value));
  if (f.inPeer) f.candidate.derp_region = region;
  else if (f.inNode) f.result.self_derp_region = region;
}

void onObjectStart(void* ctx) {
  auto& f = *static_cast<ml_map_filter_t*>(ctx);
  ++f.depth;
  if (f.depth == 2 && std::strcmp(f.key, "Node") == 0) {
    f.inNode = true;
    f.nodeDepth = f.depth;
  } else if (f.inPeers && f.depth == f.peersArrayDepth + 1) {
    f.inPeer = true;
    f.peerDepth = f.depth;
    f.candidate = {};
  }
  f.key[0] = '\0';
}

void onObjectEnd(void* ctx) {
  auto& f = *static_cast<ml_map_filter_t*>(ctx);
  if (f.inPeer && f.depth == f.peerDepth) {
    const bool ownAddress = f.targetIp != 0 && f.candidate.vpn_ip == f.targetIp;
    if (!f.result.peer.found &&
        (ownAddress || f.candidate.route_bits != 0 || sameHost(f.candidate.hostname, f.targetHost))) {
      f.result.peer = f.candidate;
      f.result.peer.found = true;
      // AllowedIPs always repeats the peer's own address as a /32; that is not
      // a route.
      if (ownAddress) {
        f.result.peer.route_ip = 0;
        f.result.peer.route_bits = 0;
      }
    }
    if (f.secondaryIp != 0 && !f.result.secondary.found && f.candidate.vpn_ip == f.secondaryIp) {
      f.result.secondary = f.candidate;
      f.result.secondary.found = true;
      f.result.secondary.route_ip = 0;
      f.result.secondary.route_bits = 0;
    }
    f.inPeer = false;
  }
  if (f.inNode && f.depth == f.nodeDepth) f.inNode = false;
  if (f.depth > 0) --f.depth;
  f.key[0] = '\0';
}

void onArrayStart(void* ctx) {
  auto& f = *static_cast<ml_map_filter_t*>(ctx);
  ++f.depth;
  copyText(f.arrayKey, sizeof(f.arrayKey), f.key, std::strlen(f.key));
  if (f.depth == 2 && std::strcmp(f.key, "Peers") == 0) {
    f.inPeers = true;
    f.peersArrayDepth = f.depth;
  }
  f.key[0] = '\0';
}

void onArrayEnd(void* ctx) {
  auto& f = *static_cast<ml_map_filter_t*>(ctx);
  if (f.inPeers && f.depth == f.peersArrayDepth) f.inPeers = false;
  if (f.depth > 0) --f.depth;
  f.arrayKey[0] = '\0';
  f.key[0] = '\0';
}

JsonCallbacks callbacks(void* ctx) {
  return {ctx, onKey, onString, onNumber, nullptr, nullptr, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd};
}
}  // namespace

extern "C" ml_map_filter_t *ml_map_filter_create(uint32_t target_ip, const char *target_name, uint32_t secondary_ip) {
  void* memory = std::malloc(sizeof(ml_map_filter_t));
  if (!memory) return nullptr;
  const JsonCallbacks cb = callbacks(memory);
  return new (memory) ml_map_filter_t(target_ip, target_name, secondary_ip, cb);
}

extern "C" bool ml_map_filter_feed(ml_map_filter_t *filter, const uint8_t *data, size_t len) {
  if (!filter || (!data && len != 0)) return false;
  size_t offset = 0;
  while (offset < len && !filter->started) {
    const uint8_t byte = data[offset++];
    if (filter->prefixBytes < 4) {
      filter->head[filter->prefixBytes++] = byte;
      if (filter->prefixBytes < 4) continue;
      // A bare JSON object never contains NUL bytes, whereas a length prefix
      // below 16 MiB always ends with one, even when its low byte is 0x7B.
      const uint8_t* head = filter->head;
      if (head[0] == '{' && head[1] != 0 && head[2] != 0 && head[3] != 0) {
        filter->started = true;
        filter->parser.feed(reinterpret_cast<const char*>(head), 4);
      }
      continue;
    }
    if (byte == '{') {
      filter->started = true;
      filter->parser.feed(reinterpret_cast<const char*>(&byte), 1);
    }
  }
  if (offset < len) filter->parser.feed(reinterpret_cast<const char*>(data + offset), len - offset);
  return !filter->parser.hasError();
}

extern "C" bool ml_map_filter_finish(ml_map_filter_t *filter, ml_map_filter_result_t *result) {
  if (!filter || !result || !filter->started || filter->parser.hasError()) return false;
  *result = filter->result;
  return result->self_ip != 0 && result->peer.found && (filter->secondaryIp == 0 || result->secondary.found);
}

extern "C" void ml_map_filter_destroy(ml_map_filter_t *filter) {
  if (!filter) return;
  filter->~ml_map_filter_s();
  std::free(filter);
}

extern "C" size_t ml_map_filter_allocation_size(void) {
  return sizeof(ml_map_filter_t);
}
