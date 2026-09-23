#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_rom_md5.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/tcpip.h"
#include "microlink.h"
#include "nvs_flash.h"

namespace {
constexpr EventBits_t kGotIp = BIT0;
constexpr EventBits_t kGotPeer = BIT1;
constexpr EventBits_t kConnected = BIT2;
constexpr EventBits_t kGotSecondary = BIT3;
constexpr EventBits_t kSessionBits = kGotPeer | kConnected | kGotSecondary;

/* lab-dns (100.64.0.53): the tailnet DNS resolver node in the resolver-shaped
 * maps, selected as a secondary peer in the split-DNS scenarios. */
constexpr uint32_t kResolverIp = 0x64400035U;
#if defined(ML_QEMU_RESOLVER_TEST) || defined(ML_QEMU_RESOLVER_LATE_TEST) || defined(ML_QEMU_RESOLVER_ABSENT_TEST)
constexpr uint32_t kSecondaryIp = kResolverIp;
#else
constexpr uint32_t kSecondaryIp = 0;
#endif
#if defined(ML_QEMU_ROUTED_TARGET_TEST) || defined(ML_QEMU_RESOLVER_TEST) || defined(ML_QEMU_RESOLVER_LATE_TEST) || \
    defined(ML_QEMU_RESOLVER_ABSENT_TEST) || defined(ML_QEMU_RESTART_TEST)
/* 100.70.0.42 is advertised by lab-gw (100.64.0.77) in AllowedIPs only: the
 * shape of a subnet router or Tailscale VIP service. The name is deliberately
 * not a MagicDNS peer name, as with a split-DNS hostname on hardware. */
constexpr uint32_t kTargetIp = 0x6446002aU;
constexpr uint32_t kExpectedPeerIp = 0x6440004dU;
constexpr char kTargetName[] = "calibre.lab.qemu.test";
#else
constexpr uint32_t kTargetIp = 0x6440002aU;
constexpr uint32_t kExpectedPeerIp = kTargetIp;
constexpr char kTargetName[] = "api-gateway.integration.test.ts.net";
#endif
/* Slirp reserves 10.0.2.2 for its gateway. QEMU guestfwd exposes the host
 * fixture at this otherwise-unused address inside the guest network. */
constexpr char kControlHost[] = "10.0.2.100";
constexpr char kAuthKey[] = "qemu-integration-authkey";
#if defined(ML_QEMU_FULL_MAP_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-full";
#elif defined(ML_QEMU_STALLED_MAP_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-stalled";
#elif defined(ML_QEMU_ROUTED_TARGET_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-routed";
#elif defined(ML_QEMU_RESOLVER_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-resolver";
#elif defined(ML_QEMU_RESOLVER_LATE_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-resolver-late";
#elif defined(ML_QEMU_RESOLVER_ABSENT_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-resolver-absent";
#elif defined(ML_QEMU_RESTART_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-restart";
#elif defined(ML_QEMU_MISSING_TARGET_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-missing";
#elif defined(ML_QEMU_MALFORMED_MAP_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-malformed";
#elif defined(ML_QEMU_BULK_LOSSY_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-bulk-lossy";
#elif defined(ML_QEMU_BULK_TEST)
constexpr char kDeviceName[] = "crosspoint-qemu-bulk";
#else
constexpr char kDeviceName[] = "crosspoint-qemu-early";
#endif

/* Public half of control_fixture's deterministic MachinePrivate key. */
constexpr uint8_t kControlPublicKey[32] = {
    0x8f, 0x40, 0xc5, 0xad, 0xb6, 0x8f, 0x25, 0x62, 0x4a, 0xe5, 0xb2, 0x14, 0xea, 0x76, 0x7a, 0x6e,
    0xc9, 0x4d, 0x82, 0x9d, 0x3d, 0x7b, 0x5e, 0x1a, 0xd1, 0xba, 0x6f, 0x3e, 0x21, 0x38, 0x28, 0x5f,
};

EventGroupHandle_t events;
void* ballast[64];
size_t ballastCount;
/* Peers the callback watches for. The restart scenario retargets these
 * between its two sessions. */
uint32_t expectedPeerIp = kExpectedPeerIp;
uint32_t secondaryIp = kSecondaryIp;

unsigned freeHeap() { return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)); }
unsigned largestBlock() { return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); }

void onIp(void*, esp_event_base_t, int32_t eventId, void* eventData) {
  if (eventId != IP_EVENT_ETH_GOT_IP) return;
  auto* event = static_cast<ip_event_got_ip_t*>(eventData);
  ESP_LOGI("qemu_tailnet", "QEMU_NET ip=" IPSTR, IP2STR(&event->ip_info.ip));
  xEventGroupSetBits(events, kGotIp);
}

void startOpenEth() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_config_t netifConfig = ESP_NETIF_DEFAULT_ETH();
  esp_netif_t* netif = esp_netif_new(&netifConfig);
  if (!netif) abort();

  eth_mac_config_t macConfig = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phyConfig = ETH_PHY_DEFAULT_CONFIG();
  phyConfig.phy_addr = 1;
  phyConfig.reset_gpio_num = -1;
  phyConfig.autonego_timeout_ms = 100;
  esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&macConfig);
  esp_eth_phy_t* phy = esp_eth_phy_new_dp83848(&phyConfig);
  if (!mac || !phy) abort();
  esp_eth_config_t ethConfig = ETH_DEFAULT_CONFIG(mac, phy);
  esp_eth_handle_t eth = nullptr;
  ESP_ERROR_CHECK(esp_eth_driver_install(&ethConfig, &eth));
  ESP_ERROR_CHECK(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, onIp, nullptr));
  ESP_ERROR_CHECK(esp_eth_start(eth));
}

void constrainHeapTo(size_t targetBytes) {
  while (heap_caps_get_free_size(MALLOC_CAP_8BIT) > targetBytes + 8192 &&
         ballastCount < sizeof(ballast) / sizeof(ballast[0])) {
    ballast[ballastCount] = heap_caps_malloc(8192, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!ballast[ballastCount]) break;
    memset(ballast[ballastCount], 0xa5, 8192);
    ++ballastCount;
  }
  const size_t remaining = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (remaining > targetBytes + 512 && ballastCount < sizeof(ballast) / sizeof(ballast[0])) {
    const size_t finalSize = remaining - targetBytes;
    ballast[ballastCount] = heap_caps_malloc(finalSize, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (ballast[ballastCount]) {
      memset(ballast[ballastCount], 0x5a, finalSize);
      ++ballastCount;
    }
  }
  ESP_LOGI("qemu_tailnet", "QEMU_HEAP target=%u free=%u largest=%u ballast_blocks=%u",
           static_cast<unsigned>(targetBytes), freeHeap(), largestBlock(), static_cast<unsigned>(ballastCount));
}

void onState(microlink_t* ml, microlink_state_t state, void*) {
  ESP_LOGI("qemu_tailnet", "QEMU_STATE value=%d name=%s free=%u largest=%u reason=%s", static_cast<int>(state),
           microlink_state_name(state), freeHeap(), largestBlock(), microlink_get_last_error(ml));
  if (state == ML_STATE_CONNECTED) xEventGroupSetBits(events, kConnected);
}

void onPeer(microlink_t*, const microlink_peer_info_t* peer, void*) {
  char ip[16];
  microlink_ip_to_str(peer->vpn_ip, ip);
  ESP_LOGI("qemu_tailnet", "QEMU_PEER name=%s ip=%s", peer->hostname, ip);
  if (peer->vpn_ip == expectedPeerIp) xEventGroupSetBits(events, kGotPeer);
  if (secondaryIp != 0 && peer->vpn_ip == secondaryIp) xEventGroupSetBits(events, kGotSecondary);
}

/* Bound the TCP connect: lwIP's blocking connect() retries the SYN for
 * about a minute, longer than any scenario budget. */
bool connectWithTimeout(int sock, const sockaddr_in& dest, int timeoutMs) {
  const int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
  int rc = connect(sock, reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
  if (rc != 0 && errno != EINPROGRESS) return false;
  if (rc != 0) {
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(sock, &writable);
    timeval timeout = {.tv_sec = timeoutMs / 1000, .tv_usec = (timeoutMs % 1000) * 1000};
    rc = select(sock + 1, nullptr, &writable, nullptr, &timeout);
    if (rc <= 0) {
      if (rc == 0) errno = ETIMEDOUT;
      return false;
    }
    int soError = 0;
    socklen_t len = sizeof(soError);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &soError, &len);
    if (soError != 0) {
      errno = soError;
      return false;
    }
  }
  fcntl(sock, F_SETFL, flags);
  return true;
}

/* GET / from the fixture peer at ip:80 over a plain lwIP BSD socket, the way
 * the reader's HTTP client reaches a tailnet host: routed by lwIP onto the
 * WireGuard netif, encrypted, relayed by DERP, and back. */
bool tunnelHttpGet(uint32_t ip) {
  char ipStr[16];
  microlink_ip_to_str(ip, ipStr);
  const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-http step=socket ip=%s errno=%d", ipStr, errno);
    return false;
  }
  sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(80);
  dest.sin_addr.s_addr = htonl(ip);
  if (!connectWithTimeout(sock, dest, 8000)) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-http step=connect ip=%s errno=%d", ipStr, errno);
    close(sock);
    return false;
  }
  timeval timeout = {.tv_sec = 8, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  static const char kRequest[] = "GET / HTTP/1.0\r\nHost: calibre.lab.qemu.test\r\n\r\n";
  if (send(sock, kRequest, sizeof(kRequest) - 1, 0) != static_cast<int>(sizeof(kRequest) - 1)) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-http step=send ip=%s errno=%d", ipStr, errno);
    close(sock);
    return false;
  }
  char reply[512];
  size_t total = 0;
  int lastErrno = 0;
  while (total < sizeof(reply) - 1) {
    const int n = recv(sock, reply + total, sizeof(reply) - 1 - total, 0);
    if (n <= 0) {
      if (n < 0) lastErrno = errno;
      break;
    }
    total += static_cast<size_t>(n);
  }
  reply[total] = '\0';
  close(sock);
  char* body = strstr(reply, "tailnet-ok");
  if (!body) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-http step=body ip=%s bytes=%u errno=%d head=%.60s", ipStr,
             static_cast<unsigned>(total), lastErrno, reply);
    return false;
  }
  body[strcspn(body, "\r\n")] = '\0';
  ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=tunnel-http ip=%s bytes=%u body=%s free=%u largest=%u", ipStr,
           static_cast<unsigned>(total), body, freeHeap(), largestBlock());
  return true;
}

#if defined(ML_QEMU_BULK_TEST)
#ifndef ML_QEMU_BULK_BYTES
#define ML_QEMU_BULK_BYTES (1024u * 1024u)
#endif
/* Same xorshift32 stream the fixture's GET /bulk serves
 * (control_fixture/wgpeer.go, bulkSeed): little-endian words, one byte at a
 * time, so the body is verified as it streams and never buffered. */
constexpr uint32_t kBulkSeed = 0x2545f491U;
constexpr size_t kBulkChunk = 1024;

struct BulkStream {
  uint32_t state = kBulkSeed;
  uint32_t word = 0;
  unsigned have = 0;
  uint8_t next() {
    if (have == 0) {
      uint32_t x = state;
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      state = x;
      word = x;
      have = 4;
    }
    const uint8_t out = static_cast<uint8_t>(word);
    word >>= 8;
    --have;
    return out;
  }
};

/* One recv-sized chunk, in .bss rather than on the 8 KB main task stack. */
uint8_t bulkChunk[kBulkChunk];

/* GET /bulk?bytes=N through the tunnel over a plain lwIP socket, verifying
 * the body byte for byte against the regenerated stream and MD5ing it with
 * the ROM routines. The fixture logs the same digest for the run script to
 * compare. Memory: the chunk above, the generator and the MD5 context. */
bool tunnelBulkGet(uint32_t ip, size_t bytes) {
  char ipStr[16];
  microlink_ip_to_str(ip, ipStr);
  /* The previous stage's socket releases its netconn asynchronously; settle
   * so the before/after heap samples bracket only this transfer. */
  vTaskDelay(pdMS_TO_TICKS(1000));
  const unsigned freeBefore = freeHeap();
  const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-bulk step=socket ip=%s errno=%d", ipStr, errno);
    return false;
  }
  sockaddr_in dest = {};
  dest.sin_family = AF_INET;
  dest.sin_port = htons(80);
  dest.sin_addr.s_addr = htonl(ip);
  if (!connectWithTimeout(sock, dest, 8000)) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-bulk step=connect ip=%s errno=%d", ipStr, errno);
    close(sock);
    return false;
  }
  timeval timeout = {.tv_sec = 15, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  char request[96];
  const int requestLen = snprintf(request, sizeof(request),
                                  "GET /bulk?bytes=%u HTTP/1.0\r\nHost: calibre.lab.qemu.test\r\n\r\n",
                                  static_cast<unsigned>(bytes));
  const int64_t started = esp_timer_get_time();
  if (send(sock, request, requestLen, 0) != requestLen) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-bulk step=send ip=%s errno=%d", ipStr, errno);
    close(sock);
    return false;
  }

  /* Header: accumulate in the chunk until the blank line; the fixture's
   * headers are well under 1 KB. Whatever follows is the start of the body. */
  size_t headerLen = 0;
  size_t bodyStart = 0;
  while (true) {
    if (headerLen >= sizeof(bulkChunk) - 1) {
      ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-bulk step=header ip=%s bytes=%u", ipStr,
               static_cast<unsigned>(headerLen));
      close(sock);
      return false;
    }
    const int n = recv(sock, bulkChunk + headerLen, sizeof(bulkChunk) - 1 - headerLen, 0);
    if (n <= 0) {
      ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-bulk step=header-recv ip=%s n=%d errno=%d", ipStr, n, errno);
      close(sock);
      return false;
    }
    headerLen += static_cast<size_t>(n);
    bulkChunk[headerLen] = '\0';
    const char* end = strstr(reinterpret_cast<const char*>(bulkChunk), "\r\n\r\n");
    if (end) {
      bodyStart = static_cast<size_t>(end - reinterpret_cast<const char*>(bulkChunk)) + 4;
      break;
    }
  }
  const char* header = reinterpret_cast<const char*>(bulkChunk);
  const char* lengthField = strstr(header, "Content-Length: ");
  const unsigned long contentLength = lengthField ? strtoul(lengthField + 16, nullptr, 10) : 0;
  if (strncmp(header, "HTTP/1.", 7) != 0 || strncmp(header + 8, " 200", 4) != 0 || contentLength != bytes) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-bulk step=status ip=%s content_length=%lu head=%.40s", ipStr,
             contentLength, header);
    close(sock);
    return false;
  }

  BulkStream stream;
  md5_context_t md5;
  esp_rom_md5_init(&md5);
  size_t received = 0;
  size_t mismatches = 0;
  size_t firstMismatch = 0;
  int lastErrno = 0;
  size_t chunkLen = headerLen - bodyStart;
  size_t chunkOffset = bodyStart;
  while (true) {
    for (size_t i = 0; i < chunkLen; ++i) {
      const uint8_t expected = stream.next();
      if (bulkChunk[chunkOffset + i] != expected) {
        if (mismatches == 0) firstMismatch = received + i;
        ++mismatches;
      }
    }
    if (chunkLen) esp_rom_md5_update(&md5, bulkChunk + chunkOffset, static_cast<uint32_t>(chunkLen));
    received += chunkLen;
    if (received >= bytes) break;
    const int n = recv(sock, bulkChunk, sizeof(bulkChunk), 0);
    if (n <= 0) {
      if (n < 0) lastErrno = errno;
      break;
    }
    chunkLen = static_cast<size_t>(n);
    chunkOffset = 0;
  }
  const int64_t elapsedUs = esp_timer_get_time() - started;
  close(sock);
  uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
  esp_rom_md5_final(digest, &md5);
  char digestHex[2 * ESP_ROM_MD5_DIGEST_LEN + 1];
  for (size_t i = 0; i < ESP_ROM_MD5_DIGEST_LEN; ++i) snprintf(digestHex + 2 * i, 3, "%02x", digest[i]);
  const unsigned elapsedMs = static_cast<unsigned>(elapsedUs / 1000);
  if (received != bytes || mismatches != 0) {
    ESP_LOGE("qemu_tailnet",
             "QEMU_FAIL stage=tunnel-bulk step=body ip=%s received=%u expected=%u mismatches=%u first_mismatch=%u "
             "errno=%d ms=%u md5=%s",
             ipStr, static_cast<unsigned>(received), static_cast<unsigned>(bytes), static_cast<unsigned>(mismatches),
             static_cast<unsigned>(firstMismatch), lastErrno, elapsedMs, digestHex);
    return false;
  }
  /* Let the socket's netconn and the wg_mgr/net_io queues drain before the
   * heap sample the run script compares with free_before. */
  vTaskDelay(pdMS_TO_TICKS(1000));
  const unsigned kbps = elapsedMs ? static_cast<unsigned>((static_cast<uint64_t>(bytes) * 8) / elapsedMs) : 0;
  ESP_LOGI("qemu_tailnet",
           "QEMU_PASS stage=tunnel-bulk ip=%s bytes=%u ms=%u kbps=%u md5=%s free_before=%u free=%u largest=%u", ipStr,
           static_cast<unsigned>(bytes), elapsedMs, kbps, digestHex, freeBefore, freeHeap(), largestBlock());
  return true;
}
#endif

/* Point lwIP's resolver at the lab-dns peer and resolve the routed name
 * through the tunnel (UDP over the WireGuard netif). */
bool tunnelDnsResolve(uint32_t* resolved) {
  ip_addr_t server;
  IP_ADDR4(&server, 100, 64, 0, 53);
  LOCK_TCPIP_CORE();
  dns_setserver(0, &server);
  dns_clear_cache();
  UNLOCK_TCPIP_CORE();
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const int rc = getaddrinfo("calibre.lab.qemu.test", "80", &hints, &result);
  if (rc != 0 || !result || !result->ai_addr) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-dns rc=%d errno=%d", rc, errno);
    if (result) freeaddrinfo(result);
    return false;
  }
  const uint32_t ip = ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
  freeaddrinfo(result);
  char ipStr[16];
  microlink_ip_to_str(ip, ipStr);
  if (ip != kTargetIp) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=tunnel-dns ip=%s", ipStr);
    return false;
  }
  ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=tunnel-dns ip=%s free=%u largest=%u", ipStr, freeHeap(), largestBlock());
  *resolved = ip;
  return true;
}

/* init + start for one session. Returns nullptr after logging QEMU_FAIL. */
microlink_t* startSession(uint32_t targetIp, const char* targetName, uint32_t secondary) {
  microlink_config_t config = {};
  config.auth_key = kAuthKey;
  config.device_name = kDeviceName;
  config.enable_derp = true;
  config.enable_stun = false;
  config.enable_disco = false;
  config.max_peers = 4;
  config.priority_peer_ip = targetIp;
  config.priority_peer_name = targetName;
  config.secondary_peer_ip = secondary;
  config.ctrl_server_public_key = kControlPublicKey;

  microlink_t* ml = microlink_init(&config);
  if (!ml) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=init free=%u largest=%u", freeHeap(), largestBlock());
    return nullptr;
  }
  microlink_set_state_callback(ml, onState, nullptr);
  microlink_set_peer_callback(ml, onPeer, nullptr);
  ESP_ERROR_CHECK(microlink_set_ctrl_host(ml, kControlHost));
  const esp_err_t startResult = microlink_start(ml);
  if (startResult != ESP_OK) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=start error=%s free=%u largest=%u reason=%s",
             esp_err_to_name(startResult), freeHeap(), largestBlock(), microlink_get_last_error(ml));
    return nullptr;
  }
  return ml;
}

#if defined(ML_QEMU_RESTART_TEST)
/* The reader's split-DNS flow (TailnetSession): phase 1 brings up a session
 * to the resolver alone, tears it down with microlink_stop() and
 * microlink_destroy(), and phase 2 starts a fresh session for the real
 * target with the resolver as the secondary peer. Teardown must run its
 * netif/udp/timer calls under the lwIP core lock, and the heap after the
 * second bring-up is compared with the first by the test runner. */
void runRestartScenario() {
  const unsigned baselineFree = freeHeap();
  expectedPeerIp = kResolverIp;
  secondaryIp = 0;
  microlink_t* ml = startSession(kResolverIp, nullptr, 0);
  if (!ml) return;
  EventBits_t got = xEventGroupWaitBits(events, kGotPeer | kConnected, pdFALSE, pdTRUE, pdMS_TO_TICKS(25000));
  if ((got & (kGotPeer | kConnected)) != (kGotPeer | kConnected)) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=derp-connected phase=1 bits=0x%x state=%s free=%u largest=%u reason=%s",
             static_cast<unsigned>(got), microlink_state_name(microlink_get_state(ml)), freeHeap(), largestBlock(),
             microlink_get_last_error(ml));
    return;
  }
  /* Let the DERP and WG manager tasks finish their post-connect work so
   * both heap samples are taken at the same point of a session. */
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=derp-connected phase=1 free=%u largest=%u peers=%d", freeHeap(),
           largestBlock(), microlink_get_peer_count(ml));

  const esp_err_t stopErr = microlink_stop(ml);
  if (stopErr != ESP_OK) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=session-stopped error=%s reason=%s", esp_err_to_name(stopErr),
             microlink_get_last_error(ml));
    return;
  }
  microlink_destroy(ml);
  ml = nullptr;
  ESP_LOGI("qemu_tailnet", "QEMU_INFO stage=session-stopped free=%u largest=%u baseline=%u", freeHeap(),
           largestBlock(), baselineFree);

  xEventGroupClearBits(events, kSessionBits);
  expectedPeerIp = kExpectedPeerIp;
  secondaryIp = kResolverIp;
  ml = startSession(kTargetIp, kTargetName, kResolverIp);
  if (!ml) return;
  got = xEventGroupWaitBits(events, kGotPeer | kConnected, pdFALSE, pdTRUE, pdMS_TO_TICKS(25000));
  if ((got & (kGotPeer | kConnected)) != (kGotPeer | kConnected)) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=restart bits=0x%x state=%s free=%u largest=%u reason=%s",
             static_cast<unsigned>(got), microlink_state_name(microlink_get_state(ml)), freeHeap(), largestBlock(),
             microlink_get_last_error(ml));
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=restart free=%u largest=%u peers=%d resolver_arrived=%d", freeHeap(),
           largestBlock(), microlink_get_peer_count(ml), (xEventGroupGetBits(events) & kGotSecondary) ? 1 : 0);

  /* The second session must carry traffic: handshake with both peers, then
   * resolve through lab-dns and fetch through lab-gw. */
  const esp_err_t peerErr = microlink_wait_peer_ready(ml, kTargetIp, 10000);
  const esp_err_t resolverErr = microlink_wait_peer_ready(ml, kResolverIp, 10000);
  ESP_LOGI("qemu_tailnet", "QEMU_INFO stage=peer-handshake result=%s resolver=%s free=%u largest=%u reason=%s",
           esp_err_to_name(peerErr), esp_err_to_name(resolverErr), freeHeap(), largestBlock(),
           microlink_get_last_error(ml));
  if (peerErr == ESP_OK && resolverErr == ESP_OK) {
    uint32_t resolved = 0;
    if (tunnelDnsResolve(&resolved)) tunnelHttpGet(resolved);
  }
  /* Marks after two full lifecycles (stop/restart), the worst case for the
   * right-sized ML_TASK_*_STACK_SIZE. */
  microlink_log_stack_watermarks(ml);
}
#endif
}  // namespace

extern "C" void app_main() {
  events = xEventGroupCreate();
  if (!events) abort();
  esp_err_t nvsResult = nvs_flash_init();
  if (nvsResult == ESP_ERR_NVS_NO_FREE_PAGES || nvsResult == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvsResult = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvsResult);

#ifdef ML_EAGER_DATA_PLANE_TEST
  ESP_LOGI("qemu_tailnet", "QEMU_MODE eager-v5-regression");
#else
  ESP_LOGI("qemu_tailnet", "QEMU_MODE staged-v6");
#endif
  startOpenEth();
  if (!(xEventGroupWaitBits(events, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000)) & kGotIp)) {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=dhcp");
    return;
  }

  constrainHeapTo(ML_QEMU_HEAP_TARGET_BYTES);
#if defined(ML_QEMU_RESTART_TEST)
  runRestartScenario();
#else
  microlink_t* ml = startSession(kTargetIp, kTargetName, kSecondaryIp);
  if (!ml) return;

#ifdef ML_QEMU_POST_START_HEAP_TARGET_BYTES
  /* The OpenETH harness retains less driver state than the X3 Wi-Fi path.
   * Recreate the device's post-task free/max-allocation snapshot before the
   * coordinator's 100 ms retry delay expires. */
  constrainHeapTo(ML_QEMU_POST_START_HEAP_TARGET_BYTES);
#endif

#if defined(ML_QEMU_STALLED_MAP_TEST)
  vTaskDelay(pdMS_TO_TICKS(5000));
  const char* reason = microlink_get_last_error(ml);
  if (strstr(reason, "target map waiting: records=") && strstr(reason, "data=")) {
    ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=stalled-map-diagnostic reason=%s", reason);
  } else {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=stalled-map-diagnostic reason=%s", reason);
  }
#elif defined(ML_QEMU_MISSING_TARGET_TEST) || defined(ML_QEMU_MALFORMED_MAP_TEST)
  vTaskDelay(pdMS_TO_TICKS(3000));
  const char* reason = microlink_get_last_error(ml);
#ifdef ML_QEMU_MISSING_TARGET_TEST
  constexpr char kExpectedError[] = "target peer absent";
  constexpr char kErrorStage[] = "missing-target-error";
#else
  constexpr char kExpectedError[] = "Malformed HTTP/2 or JSON";
  constexpr char kErrorStage[] = "malformed-map-error";
#endif
  if (strstr(reason, kExpectedError)) {
    ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=%s reason=%s", kErrorStage, reason);
  } else {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=%s reason=%s", kErrorStage, reason);
  }
#else
  const EventBits_t result = xEventGroupWaitBits(events, kGotPeer, pdFALSE, pdTRUE, pdMS_TO_TICKS(75000));
  if (result & kGotPeer) {
    ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=target-map free=%u largest=%u peers=%d", freeHeap(), largestBlock(),
             microlink_get_peer_count(ml));

    /* The coordinator only reports CONNECTED once WireGuard is initialised
     * and the DERP relay (control_fixture's local TLS server) is up. This is
     * the stage the device reaches right after the peer map. */
    const EventBits_t connected = xEventGroupWaitBits(events, kConnected, pdFALSE, pdTRUE, pdMS_TO_TICKS(25000));
    if (connected & kConnected) {
      ESP_LOGI("qemu_tailnet", "QEMU_PASS stage=derp-connected free=%u largest=%u", freeHeap(), largestBlock());
      /* Same wait the reader performs before its first HTTP request: the
       * DERP-relayed initiation, answered by the fixture's wireguard-go
       * peer, must produce a session. */
      const esp_err_t peerErr = microlink_wait_peer_ready(ml, kTargetIp, 10000);
      ESP_LOGI("qemu_tailnet", "QEMU_INFO stage=peer-handshake result=%s free=%u largest=%u reason=%s",
               esp_err_to_name(peerErr), freeHeap(), largestBlock(), microlink_get_last_error(ml));
      if (kSecondaryIp != 0) {
#if defined(ML_QEMU_RESOLVER_ABSENT_TEST)
        /* The map has no lab-dns. The session must still come up for the
         * target alone: exactly one peer, and the resolver stays unknown. */
        const esp_err_t resolverErr = microlink_wait_peer_ready(ml, kSecondaryIp, 1000);
        const int peers = microlink_get_peer_count(ml);
        const bool resolverArrived = (xEventGroupGetBits(events) & kGotSecondary) != 0;
        ESP_LOGI("qemu_tailnet", "%s stage=resolver-absent target=1 peers=%d resolver_arrived=%d resolver=%s",
                 peers == 1 && !resolverArrived ? "QEMU_PASS" : "QEMU_FAIL", peers, resolverArrived ? 1 : 0,
                 esp_err_to_name(resolverErr));
        /* No resolver, so fetch the routed address directly through lab-gw. */
        if (peerErr == ESP_OK) tunnelHttpGet(kTargetIp);
#else
        /* The resolver peer must have arrived from the same map fetch and
         * complete its own handshake; then the name resolves through it and
         * the routed address is fetched through lab-gw. */
        const EventBits_t secondary = xEventGroupWaitBits(events, kGotSecondary, pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
        const esp_err_t resolverErr = microlink_wait_peer_ready(ml, kSecondaryIp, 10000);
        ESP_LOGI("qemu_tailnet", "%s stage=secondary-peer arrived=%d handshake=%s peers=%d",
                 (secondary & kGotSecondary) && resolverErr == ESP_OK ? "QEMU_PASS" : "QEMU_FAIL",
                 (secondary & kGotSecondary) ? 1 : 0, esp_err_to_name(resolverErr), microlink_get_peer_count(ml));
        if (peerErr == ESP_OK && resolverErr == ESP_OK) {
          uint32_t resolved = 0;
          if (tunnelDnsResolve(&resolved)) tunnelHttpGet(resolved);
        }
#endif
      } else if (peerErr == ESP_OK) {
        if (tunnelHttpGet(kTargetIp)) {
#if defined(ML_QEMU_BULK_TEST)
          tunnelBulkGet(kTargetIp, ML_QEMU_BULK_BYTES);
#endif
        }
      }
    } else {
      ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=derp-connected state=%s free=%u largest=%u reason=%s",
               microlink_state_name(microlink_get_state(ml)), freeHeap(), largestBlock(), microlink_get_last_error(ml));
    }
  } else {
    ESP_LOGE("qemu_tailnet", "QEMU_FAIL stage=target-map state=%s free=%u largest=%u reason=%s",
             microlink_state_name(microlink_get_state(ml)), freeHeap(), largestBlock(), microlink_get_last_error(ml));
  }
#endif
#endif  // ML_QEMU_RESTART_TEST
  /* The test runner terminates QEMU after it observes QEMU_PASS or QEMU_FAIL.
   * Keep the embedded session intact so teardown timing cannot obscure the
   * control-plane result. */
#if !defined(ML_QEMU_RESTART_TEST)
  /* Stack high-water marks after a full session: the basis for right-sizing
   * ML_TASK_*_STACK_SIZE on the C3. (The restart scenario logs its own inside
   * runRestartScenario, where its phase-2 handle is in scope.) */
  microlink_log_stack_watermarks(ml);
#endif
  vTaskSuspend(nullptr);
}
