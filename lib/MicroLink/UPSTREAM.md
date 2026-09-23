# MicroLink vendoring notes (rebase map)

- Upstream repo: https://github.com/CamM2325/microlink
- Upstream commit: `216da33` (`216da3300f0493b0860247d43f7af5ce29df63a5`, branch `main`)
- Vendored from: `components/microlink` -> `lib/MicroLink`;
  `components/microlink/components/wireguard_lwip` -> `lib/WireGuardLwip`
- Target: ESP32-C3 (single-core RISC-V, no PSRAM, ~380KB RAM),
  Arduino-ESP32 core 3.3.x (prebuilt ESP-IDF 5.5).

## Deleted (not copied)

MicroLink sources for features disabled on this target (cellular modem,
WiFi/cellular net-switching, config web UI, zero-copy WG):

- `src/ml_cellular.c`, `src/ml_at_socket.c`, `src/ml_net_switch.c`,
  `src/ml_config_httpd.c`, `src/ml_config_html.h`, `src/ml_zerocopy.c`
- `include/ml_net_switch.h` (not included by any kept file)
- Build-system files: `CMakeLists.txt`, `Kconfig` (replaced by
  `library.json` + `include/ml_config_defaults.h`)

Kept headers of deleted modules (they carry the disabled-feature stubs or
are included unconditionally): `include/ml_config_httpd.h` (inline no-op
stubs when `CONFIG_ML_ENABLE_CONFIG_HTTPD` undefined),
`include/ml_at_socket.h` (included unconditionally by
`microlink_internal.h`; provides `ml_at_socket_is_ready()`/`_is_at_fd()`
false-stubs when cellular disabled), `include/ml_cellular.h` (include in
`microlink.c` is `#ifdef CONFIG_ML_ENABLE_CELLULAR`-guarded).

WireGuardLwip: copied only `src/` (sources the upstream CMake built:
`wireguard.c`, `wireguardif.c`, `crypto.c`, `wireguard-platform-esp32.c`,
`crypto/refc/*.c`) plus `LICENSE`. Excluded `src/crypto/cortex/` (ARM
assembly), `example/`, `CMakeLists.txt`.

## Added files

- `lib/MicroLink/include/ml_config_defaults.h`: `#ifndef`-guarded
  defaults for all `CONFIG_ML_*` symbols the kept sources use
  (`MAX_PEERS=4`, `NVS_MAX_PEERS=16`, `H2_BUFFER_SIZE_KB=16`,
  `JSON_BUFFER_SIZE_KB=16`, `PRIORITY_PEER_IP=""`, `DEVICE_NAME=""`).
  Feature enables (`ENABLE_CELLULAR`, `ENABLE_NET_SWITCH`,
  `ENABLE_CONFIG_HTTPD`, `ZERO_COPY_WG`) deliberately not defined
  (absent = disabled). `CONFIG_ML_CONFIG_MAX_ALLOWED_PEERS` needs no
  default (only used inside the `ENABLE_CONFIG_HTTPD` block).
- `lib/MicroLink/library.json`, `lib/WireGuardLwip/library.json`:
  PlatformIO manifests (LDF dependency MicroLink -> WireGuardLwip,
  `-DWIREGUARD_CRYPTO_REFC=1` kept from upstream CMake although this
  fork's `crypto.h` hard-includes refc and never tests the macro).

## Functional changes vs upstream (file + what/why)

- `include/microlink_internal.h`: include `ml_config_defaults.h`
  immediately after `sdkconfig.h` so real sdkconfig values win.
- `include/microlink_internal.h`: `ML_TASK_COORD_CORE` and
  `ML_TASK_WG_MGR_CORE` 1 -> `tskNO_AFFINITY` (`#ifndef`-wrapped); core-1
  pinning asserts on single-core C3. net_io/derp_tx stay pinned to core 0.
- `include/microlink_internal.h`: `ML_PEER_UPDATE_QUEUE_DEPTH` 400 -> 8
  (`#ifndef`), a RAM cut for small tailnets.
- `include/microlink_internal.h`: `ML_MAX_DERP_REGIONS` 32 -> 4,
  `ML_MAX_DERP_NODES` 4 -> 1 (`#ifndef`), a RAM cut. All node/region array
  access is bounds-checked against these at parse time (ml_coord.c) and
  loops on `node_count` (ml_derp.c); ml_stun.c only reads `nodes[0]`.
- `include/microlink_internal.h`: new `ML_H2_FRAME_BUF_SIZE`
  (`#ifndef`, default 16KB) replacing two hard-coded 65536 Noise-frame
  scratch buffers.
- `src/ml_coord.c`: both `ml_psram_malloc(65536)` / `noise_recv(..., 65536)`
  sites now use `ML_H2_FRAME_BUF_SIZE`. `noise_recv()` rejects frames
  larger than the buffer (no overflow; oversized control frames fail).
- `src/ml_coord.c` (`do_h2_preface`): `conn_window_delta` computed as
  signed `int32_t`. Upstream's `uint32_t ML_H2_BUFFER_SIZE - 65535`
  underflows for buffers < 64KB (upstream Kconfig floor was 64KB) and
  would send a bogus H2 WINDOW_UPDATE.
- `src/ml_coord.c` (MapResponse recv loop): mid-response WINDOW_UPDATE
  threshold 32768 -> `min(32768, ML_H2_BUFFER_SIZE/2)` so replenishment
  still fires when the stream window is < 64KB (otherwise the server can
  stall until timeout on responses larger than the window).
- `src/ml_noise.c`: ported ChaCha20-Poly1305 AEAD from
  `mbedtls_chachapoly_*` to wireguard-lwip's refc
  `chacha20poly1305_encrypt/_decrypt`. Arduino-ESP32's prebuilt mbedTLS
  has `MBEDTLS_CHACHAPOLY_C`/`CHACHA20_C`/`POLY1305_C` disabled, so the
  mbedTLS calls fail to link. Nonce equivalence: Tailscale ts2021 needs 4 zero bytes +
  8-byte big-endian counter; refc builds 4 zero bytes + 8-byte
  little-endian value, so the counter is passed through
  `__builtin_bswap64()` (LE64(bswap64(x)) == BE64(x); same trick as
  MicroLink v1). Local wrappers renamed `noise_aead_encrypt/_decrypt` to
  avoid colliding with the refc global symbol names. blake2s/chachapoly
  includes now use the `crypto/refc/` path prefix (resolved via
  WireGuardLwip's exported `src` include dir).
- `src/ml_udp.c`: public-API UDP RX task unpinned. Upstream pins it to
  literal core 1, which asserts on C3.
- Log noise (battery/serial): per-packet & periodic `ESP_LOGI` demoted to
  `ESP_LOGD` in `src/ml_net_io.c` (UDP RX), `src/ml_wg_mgr.c` (WG UDP TX,
  DISCO PING/PONG/RX, WG RX, wireguardif_periodic / disco_periodic_probes
  tick timing), `src/ml_derp.c` (DERP RecvPacket/SendPacket per-frame,
  5s HEARTBEAT, previously `ESP_LOGW`, 10s status). State transitions and
  errors remain at INFO/WARN/ERROR.

### WireGuardLwip changes

- `src/wireguard-platform.h`: `WIREGUARD_MAX_PEERS` 16 -> 4, now
  `#ifndef`-wrapped. Must match `CONFIG_ML_MAX_PEERS`; any override must
  be a global `-D` seen by both libraries (struct sizes diverge
  otherwise).
- `src/wireguard-platform-esp32.c`: 5-second `[TAI64N]` raw `printf`
  guarded behind `#ifdef ML_DEBUG_TAI64N`.
- `src/wireguardif.c`: per-packet `[WG_RX] type=...` raw `printf` (fired
  for every WG DATA packet, in lwIP context) routed through the existing
  `WG_DEBUG` macro (compiled out unless `WG_DEBUG_LOGGING` set to 1).
  Handshake-event printfs left as upstream.

## Behavioral limits accepted for this target

- Noise transport frames larger than `ML_H2_FRAME_BUF_SIZE` (16KB) are
  rejected; H2/MapResponse data larger than `ML_H2_BUFFER_SIZE` (16KB) is
  truncated. Fine for small tailnets (<= 4 peers); raise both via `-D`
  for larger ones.
- `ML_MAX_DERP_NODES=1`: if a region's only stored node is STUN-only,
  DERP connect falls back to the compile-time default host
  (`derp9e.tailscale.com`). Normal Tailscale DERP maps list a usable
  node first.

## Post-vendoring additions (integration layer)

- `microlink.c` / `microlink.h`: added `microlink_set_ctrl_host()` (public
  Headscale/control-plane override; upstream only reads this from the deleted
  config-httpd NVS path) and `microlink_wait_peer_ready()` (public wrapper
  around `ml_wg_mgr_trigger_handshake` + `ml_wg_mgr_send_cmm` +
  `ml_wg_mgr_peer_is_up`, mirroring `microlink_tcp_connect()`'s wait loop, so
  Arduino/lwIP TCP clients can wait for the tunnel before connecting).
- Deleted `src/x25519.{c,h}` (byte-identical duplicate of WireGuardLwip's
  `crypto/refc/x25519.{c,h}` apart from a warning pragma); the four includers
  now use `crypto/refc/x25519.h`. Fixes multiple-definition link errors:
  PlatformIO links both libs' archives, unlike the upstream IDF component
  build where only one copy was pulled.

## Diagnostics and test hooks

- `include/microlink_internal.h`: `ML_DERP_REGION`, `ML_DERP_HOST`,
  `ML_DERP_PORT` are now `#ifndef`-wrapped so `test/tailnet_qemu` can point
  the relay at a local DERP server via `-D`. Values unchanged.
- `src/ml_derp.c`: the connect/reconnect retry loops log `ml->last_error`
  alongside "DERP connect attempt N failed" (previously the reason recorded
  by `ml_derp_connect()` was invisible in the serial log). The DNS-resolve,
  TCP-connect and HTTP-upgrade-rejected failure paths now also record a
  reason in `last_error` so `microlink_get_last_error()` and the reader's
  `/tailscale-error.log` name the failing step.
- Routed targets (subnet routes, Tailscale VIP services). The targeted map
  filter matched a peer only on its `Name` or `Addresses`, so a target that
  a peer merely advertises (its `AllowedIPs`/`PrimaryRoutes`, e.g. a
  Kubernetes-operator proxy serving `100.70.198.230/32`) was reported as
  "target peer absent". Changes:
  - `include/ml_map_filter.h`, `src/ml_map_filter.cpp`: peers also match
    when an `AllowedIPs` prefix contains the target IP; the matched prefix
    is returned as `route_ip`/`route_bits`.
  - `include/microlink_internal.h`, `src/ml_coord.c`: `ml_peer_update_t`
    and `ml_peer_t` carry `route_ip`/`route_bits`.
  - `src/ml_wg_mgr.c`: `find_peer_by_ip()` and the priority-peer check also
    match addresses inside the route, so `microlink_wait_peer_ready()`,
    handshake triggers and CallMeMaybe work for the routed address; on add,
    the route is installed as a second allowed prefix on the WG peer.
  - `WireGuardLwip/src/wireguardif.{h,c}`: new `wireguardif_add_allowed_ip()`
    (wraps the static `peer_add_ip()`; `WIREGUARD_MAX_SRC_IPS` is 2, enough
    for the peer's own /32 plus one route).
  Persisted in the NVS peer cache (`route_ip`/`route_bits`, see warm start
  below) so a warm start reinstalls it; the targeted map re-sends it on
  every cold start. Covered by `test/tailnet_qemu` scenario `qemu_v6_routed`.
- Secondary (resolver) peer. `microlink_config_t.secondary_peer_ip` selects
  one more peer from the same targeted map fetch, by its own VPN address.
  The reader uses it for a DNS resolver inside the tailnet so split-DNS
  hostnames (`*.lab.wayvz.io`, known only to `lab-dns`) resolve through the
  tunnel: phase 1 brings up the resolver alone and asks it, phase 2 rebuilds
  the session around the answer with the resolver as secondary so the HTTP
  client's own lookup also works. Two sessions rather than a re-fetch,
  because the map workspace and the DERP/WG data plane cannot coexist on
  the C3. Changes: `include/microlink.h` (config field),
  `include/ml_map_filter.h`/`src/ml_map_filter.cpp` (`secondary` result,
  stream complete only when both are found; a map without the resolver
  still yields the target), `src/ml_coord.c` (`queue_selected_peer()`
  factored out and called for both; "Streamed resolver peer" log).
  Covered by `test/tailnet_qemu` scenario `qemu_v6_resolver`.
- lwIP core locking. The Arduino-ESP32 lwIP is built with
  `CONFIG_LWIP_CHECK_THREAD_SAFETY=y`, so `netif_*`, `udp_*` and
  `sys_timeout` calls from the wg_mgr task assert ("Required to lock TCPIP
  core functionality!", first seen on the X3 in `netif_set_up`). ESP-IDF
  builds (and the QEMU harness before this fix) have the check off, which is
  why upstream never hit it. `WireGuardLwip/src/lwip_compat.h` adds
  `wg_lwip_lock()`/`wg_lwip_unlock()` (no-op inside the tcpip thread or when
  the lock is already held) and every such call site in `wireguardif.c`,
  `src/ml_wg_mgr.c` (interface init/add/up, raw output PCB, teardown) and
  `src/ml_udp.c` is wrapped. `test/tailnet_qemu/sdkconfig.defaults` now
  enables the check so the harness matches the device.
- Map filter fixes found by the host suite `test/tailnet_map_filter`
  (`MapFilterTest`, 37 cases): a target matched on its own address no
  longer carries its /32 from AllowedIPs in `route_ip`/`route_bits` (the
  header promises 0 there, and wg_mgr would otherwise add a redundant
  allowed prefix); and `ml_map_filter_feed()` now buffers the 4-byte length
  prefix instead of sniffing for '{', so a response whose length is 123 mod
  256 (low byte 0x7B) is not mistaken for unprefixed JSON.
- `WireGuardLwip/src/wireguardif.c` `wireguardif_process_data_message()`:
  decrypted packets were handed to `ip_input()` on the WireGuard manager
  task. Under `CONFIG_LWIP_CHECK_THREAD_SAFETY` (the device lwIP) the first
  decrypted packet after a handshake aborts in `ip4_input`. It now calls
  `netif->input` (`tcpip_input`, which posts to the TCP/IP mailbox) and
  frees the pbuf on failure. Found by the QEMU harness once the fixture
  gained real WireGuard peers (`control_fixture/wgpeer.go`: wireguard-go
  devices bound to the fixture's DERP relay, with a gVisor netstack serving
  HTTP and DNS), reproduced by reverting the fix.
- DERP TLS backend: wolfSSL on device, mbedTLS in the QEMU harness. The
  device DERP client used mbedTLS, which statically allocates a 16 KB inbound
  record buffer in mbedtls_ssl_setup(); mid tailnet session the C3 has only
  ~11 KB contiguous, so setup failed (MBEDTLS_ERR_SSL_ALLOC_FAILED). The
  intended fix (CONFIG_MBEDTLS_DYNAMIC_BUFFER) is impossible on the device:
  Kconfig makes it depend on !MBEDTLS_SSL_PROTO_DTLS and the prebuilt Arduino
  mbedTLS is compiled with DTLS on. Rather than rebuild the prebuilt libs,
  `src/ml_derp.c` now uses wolfSSL, the TLS stack the firmware already links
  for OPDS HTTPS. wolfSSL grows its record buffers on demand (no fixed 16 KB
  block), so the peak contiguous allocation is a few KB. It is driven as a
  raw client over MicroLink's socket layer via wolfSSL I/O callbacks
  (ml_read_sock/ml_write_sock), VERIFY_NONE (the relay is authenticated at
  the DERP protocol layer), SNI set, X25519 key share pinned, and a 2 KB max
  fragment requested. Selected by FREEINK_NET_WOLFSSL. The `#else` branch
  keeps the original mbedTLS path for the QEMU harness, whose emulated
  ESP32-C3 RNG cannot seed wolfSSL's DRBG (wc_InitRng returns -199 even with
  ample heap) though it seeds mbedTLS; the harness therefore still exercises
  the DERP protocol, HTTP upgrade, WireGuard-over-DERP and the tunnel data
  path, and the wolfSSL handshake is validated on hardware where the TRNG
  works. Read/write call sites are unified behind derp_ssl_read/derp_ssl_write
  (>0 bytes / 0 want-io / -1 closed), so only the includes, the DERP struct
  fields, the callbacks, the setup and the teardown differ by backend.
- Task stacks right-sized for the C3 (`include/ml_memory_budget.h`). The four
  MicroLink task stacks were 8/14/12/8 KB (42 KB of heap), the largest single
  consumer during a session; with them the DERP TLS handshake had only ~16 KB
  left and failed with MEMORY_E even under wolfSSL. Measured high-water marks
  after a full QEMU session (new `microlink_log_stack_watermarks()`, also
  called by the reader at teardown at ERROR level so the device reports its
  own marks): coord 3.6 KB, derp_tx 2.8 KB, net_io 3.4 KB, wg_mgr 3.1 KB
  used. Now 6/4/4/4 KB (18 KB; coord/derp_tx/net_io/wg_mgr), 1.2-1.7x
  margin over measured peak, freeing 24 KB at the handshake. derp_tx was
  measured under mbedTLS and is conservative for the device's wolfSSL +
  WOLFSSL_SMALL_STACK.
  QEMU marks after the later trim to 4/4/6/4 KB (net_io/derp_tx/coord/
  wg_mgr), from `test/tailnet_qemu` (mbedTLS DERP, OpenETH, indicative
  only): every data-plane scenario, including the restart scenario's two
  lifecycles and the 1 MiB `qemu_v6_bulk`/`qemu_v6_bulk_lossy` transfers,
  reports `STACK_HWM min-free/size bytes: coord=2548/6144 derp_tx=1260/4096
  net_io=708/4096 wg_mgr=956/4096` (restart: 2544/1264/712/960). The bulk
  transfer moves no mark: net_io and wg_mgr peak during the handshake, not
  under sustained traffic. net_io keeps 17% and wg_mgr 23% of their stacks
  free here; the device's wolfSSL path is the one to read before trimming
  further. Restart heap drift 12 bytes; bulk-transfer heap drift 16-20 bytes.
- Real-relay certificate chain (found on device once wolfSSL DERP had heap to
  reach the handshake): derp*.tailscale.com serves a Let's Encrypt chain
  (P-256 leaf, P-384 intermediates, all signed ecdsa-with-SHA384) plus
  Tailscale's self-signed Ed25519 "derpkey<hex>" meta-certificate that
  advertises the relay's DERP key (derpserver.Server.MetaCert()). wolfSSL
  parses every chain cert even under VERIFY_NONE, so the missing SHA-384 OID
  aborted on the leaf (ASN_UNKNOWN_OID_E, -148) and Ed25519 would have
  aborted on the meta-cert next. Fixed with -DWOLFSSL_SHA384 -DWOLFSSL_SHA512
  -DHAVE_ED25519 in the reader's wolfSSL flags (compiled from source, so they
  take effect; additive, OPDS HTTPS unaffected). Note this means the original
  mbedTLS DERP client could never have connected to a real relay from this
  build: ESP-IDF mbedTLS has no Ed25519 X.509 support, so the meta-cert alone
  would fail the handshake. The QEMU harness (mbedTLS backend) therefore
  cannot serve the meta-cert without breaking; its fixture keeps a plain
  ECDSA cert, and the real-relay chain remains a device-only check.
- Relay region on the targeted path (`include/microlink_internal.h`,
  `src/ml_derp.c`). The targeted map filter never parses the DERPMap, so
  ml_derp_connect() had no relay hostnames and always used the compile-time
  ML_DERP_HOST (derp9e, Dallas). DERP only delivers to peers connected to
  the same region, so from a Sydney-homed reader every WireGuard handshake
  init to the Sydney-homed peers was dropped at Dallas (seen on device as
  TS-E10 with the session otherwise fully up). Node.HomeDERP is already
  parsed into derp_home_region; the connect path now derives the relay by
  Tailscale's official naming, ML_DERP_REGION_HOST_FMT "derp%u.tailscale.com",
  when no DERPMap is known and the region is, falling back to ML_DERP_HOST
  only when the region is unknown. ML_DERP_USE_REGION_HOST=0 (set by the QEMU
  harness) keeps ML_DERP_HOST, e.g. a local relay. Assumption: the target
  peers are homed in the reader's own region (true for a home-network book
  server); a split-region peer would need a second DERP connection. The
  chosen relay and region are printed in the ERROR-level pre-handshake line.
- PreferredDERP reporting (`src/ml_coord.c`, `include/microlink.h`). MicroLink
  hardcoded PreferredDERP=ML_DERP_REGION (9, Dallas) in the register, the
  MapRequest and the endpoint update, so control recorded HomeDERP=9 for the
  reader and reported it to every peer. magicsock sets a peer's DERP reply
  address from that HomeDERP (wgengine/magicsock/endpoint.go updateFromNode),
  not from the region a packet arrived on, so even relaying through the right
  region left every handshake reply stranded on Dallas. Now:
  preferred_derp_region() reports the target peer's home once the map is
  parsed, else microlink_config_t.preferred_derp_region (persisted by the
  reader from a prior session via microlink_get_derp_region()), else the
  default; the relay itself is chosen by the target's home region
  (target_derp_region); and on the targeted path, when the learned region
  differs from what was reported at registration, do_send_endpoint_update()
  (now allowed with no STUN endpoints) pushes the new PreferredDERP on the
  still-open control connection and reads the OmitPeers acknowledgement
  before the bootstrap socket closes, so peers reply via the right relay on
  the very first run. Not exercised by the QEMU harness (its fixture peers'
  HomeDERP equals the default, so no update fires; the fixture also does not
  honour OmitPeers yet).
- DERP relay node hostname (the bug that blocked every on-device handshake).
  `derp<region>.tailscale.com` (e.g. derp5.tailscale.com) resolves and serves
  the DERP protocol, but is not one of a region's mesh nodes. The real nodes
  are `derp<region><letter>` (Sydney/region 5: derp5e/5f/5g; Dallas/9: 9d/9e/9f).
  Connecting to the bare name put the reader on a relay outside the region
  mesh, so peers could not reach it ("derp-5 does not know about peer",
  handshakes dropped) even though DERP TLS and the protocol handshake
  succeeded. Fix (v19): ml_derp_connect probes the region node names
  derp<region><letter> (ML_DERP_REGION_HOST_FMT = "derp%u%c.tailscale.com",
  letters tried "efgdhijabc") and connects to the first that DNS-resolves,
  which finds a real mesh node regardless of which letters a region has and
  never uses the dead bare alias. ML_DERP_USE_REGION_HOST=0 (the QEMU harness)
  keeps the compile-time local relay. A fuller option, parsing
  DERPMap.Regions[home].Nodes[].HostName from the map, is deferred because the
  targeted path skips the full map for C3 memory/speed; the probe removes the
  fragility at no added cost. Confirmed on hardware via the self-test harness
  (v18, single-letter): WireGuard handshakes to the dev VM, lab-dns and
  lab-gw all complete over derp5e.
- Bring-up timing summary. `CONFIG_LOG_MAXIMUM_LEVEL=1` compiles the
  per-phase `[TIMING]` `ESP_LOGI` lines out of the device build, so the
  coord and DERP tasks now record each phase in `ml->timing`
  (`ml_bringup_timing_t`, `include/microlink_internal.h`) and the coord task
  prints one `ESP_LOGE` line when the session reaches CONNECTED:
  `[TIMING] control dns= tcp= noise= h2= reg= map= derp_dns= derp_tcp=
  derp_tls= derp_proto= wait= warm= total= ms`. `derp_dns` includes the
  region node-letter probe; `wait` is the coord task's wait for WireGuard
  init, the selected peers and DERP admission; control phases are 0 on a
  warm start. `scripts/hw/selftest_run.py` parses it (`COORD_TIMING`).
- Warm start (`microlink_config_t.warm_start`, `warm_self_ip`,
  `warm_derp_region`). Every session registers the same NVS node identity
  and the targeted path closes the control socket before DERP/WireGuard
  start, so the data plane needs nothing live from control. The ~5 s
  control phase (control DNS, TCP, Noise, HTTP/2, Register, targeted
  MapRequest) only produces the reader's VPN IP, the relay region and the two peer
  records, all of which the previous cold session left behind: the reader
  persists the first two (`TailscaleStore` `selfIp`/`derpRegion`, with
  `netmapCachedAt`/`netmapColdAt`/`netmapValid`) and the NVS peer table
  (`src/ml_peer_nvs.c`) already held the peers; it now also stores the
  advertised route and moved to blob key `tbl2` (97-byte entries; a
  pre-route `tbl` blob is erased on init). Mechanism:
  - `microlink_init()` skips the 16.4 KB control workspace reservation
    (`ML_TARGET_BOOTSTRAP_RESERVED_BYTES`) and forces `enable_stun=false`;
    `microlink_start()` creates all four tasks at once.
  - `ml_coord_task` enters `COORD_WARM` instead of `COORD_DNS_RESOLVE`:
    `do_warm_start()` sets `vpn_ip`, `derp_home_region` and
    `target_derp_region` from the cached values and queues `ML_PEER_ADD`
    updates built from the NVS entries covering `priority_peer_ip` and
    `secondary_peer_ip` (`queue_cached_peer()`; the resolver is optional,
    the target is not), then falls into the shared `COORD_DATA_PLANE`
    state (also the cold path's tail after `COORD_FETCH_PEERS`): set
    `ML_EVT_COORD_REGISTERED`, request DERP, wait for WG init + peers +
    DERP admission (`ML_WARM_READY_TIMEOUT_MS` 8 s instead of 15 s) and
    report CONNECTED. wg_mgr installs the queued peers into wireguardif
    exactly as it does for map-streamed ones, so they get a real
    `wg_peer_index` (the NVS pre-load alone never did). The coord task is
    kept for the DERP keepalive; dropping it in warm mode is a later
    optimisation.
  - `microlink_peer_cache_contains()` is the reader's precondition (target
    and resolver must be cached); `microlink_wait_peers_ready()` initiates
    every handshake before waiting so the two relayed round trips overlap
    (`microlink_wait_peer_ready()` is now the count-1 wrapper);
    `microlink_peer_is_up()` tells which peer answered.
  - Staleness fallback lives in the reader (`TailnetSession`): warm only
    when the store cache is valid, the cold start is at most 7 days old
    (or the clock was unset when cached), and the peers are in NVS; a
    warm session must answer a WireGuard handshake within 4 s
    (`WARM_PEER_READY_MS`) instead of the 20 s TS-E10 budget. A cache
    miss ends the session in `ML_STATE_ERROR` ("warm start: ... not in
    NVS cache"), a stale key or region shows as no handshake response;
    either way the reader logs `[TSN] Warm start failed (...); cold
    start`, tears down, clears the store cache and runs the cold path in
    the same `ensureUp()` call. A cold session that reached CONNECTED
    without a TS-E error re-caches at teardown; any TS-E error at
    teardown clears the cache. `microlink_peer_cache_poison()` flips one
    key byte for the self-test's `poisonNetmapCache` scenario.
- Self-test harness (`src/TailnetSelfTest.{h,cpp}`, `[env:selftest]`,
  CROSSPOINT_TAILNET_SELFTEST). Runs the full tailnet bring-up + WireGuard
  handshake to a list of target IPs on boot, logs RESULT= per target at ERROR
  level, and reboots to retry. Lets the whole path be driven and iterated over
  USB serial with no device UI interaction. Not built into normal firmware.

## Teardown-leak fixes

Upstream assumes a single session per boot (stop is followed by reboot);
touch-capable CrossPoint boards tear down in place, and each up/down cycle
leaked ~35-40KB. Changes (all vendored-fork only):

- `include/microlink_internal.h`: new task-exit event bits
  `ML_EVT_{NET_IO,DERP_TX,COORD,WG_MGR}_EXITED` (BIT9-12); new
  `ml_derp_conn_t.tls_inited` flag (mbedTLS context lifecycle).
- `src/microlink.c` (`microlink_stop`): blind `vTaskDelay(3000)` replaced by
  `xEventGroupWaitBits` on the exit bits of the tasks that were actually
  created (each task sets its bit immediately before `vTaskDelete(NULL)`),
  3s bounded fallback plus 50ms grace for idle-task TCB/stack reclamation.
  Sockets are now closed only after confirmed task exit, which also removes
  the risk of closing an fd still inside net_io's `select()`. Added a
  guarded close of `ml->derp.sockfd` (normally already closed/-1 via the
  DERP task's own `ml_derp_disconnect`).
- `src/microlink.c` (`microlink_start`): clears all session event bits
  (including a stale `ML_EVT_SHUTDOWN_REQUEST` left set by a previous
  `microlink_stop`; without this, stop()+start() on the same context made
  every task exit immediately). On task-creation failure, now calls
  `microlink_stop(ml)` to roll back already-created tasks/sockets instead of
  returning with them running.
- `src/microlink.c` (`microlink_destroy`): drains `derp_tx_queue`
  (`ml_derp_tx_item_t.data`), `disco_rx_queue`/`wg_rx_queue`/`stun_rx_queue`
  (`ml_rx_packet_t.data`) and `peer_update_queue` (heap `ml_peer_update_t*`)
  with zero-timeout receives, freeing payloads, before `vQueueDelete`.
  `coord_cmd_queue` carries enums by value, nothing to free. Restores
  `cJSON_InitHooks(NULL)`: init installs global PSRAM-backed hooks, and
  nothing else in `src/` or `lib/` uses cJSON (the app uses ArduinoJson), so
  restoring is safe. Also restored on the two `microlink_init` failure paths
  that free the context directly.
- `src/ml_wg_mgr.c` (task teardown): after `wireguardif_shutdown` +
  `netif_remove`, the `struct wireguard_device` allocated by
  `wireguardif_init()` via `mem_calloc` (~3.5KB at 4 peers; upstream never
  freed it) is zeroed with `crypto_zero` (holds the WG private key and
  per-peer session keys; non-elidable memset) and released with `mem_free`,
  before `free(netif)`. Added `lwip/mem.h` + `crypto.h` includes.
- `src/ml_wg_mgr.c` (task teardown): `s_wg_output_pcb` (file-static raw UDP
  output PCB holding port 51820, previously created once and never removed)
  is now `udp_remove`d; the static is NULLed *before* the remove so a late
  `wg_udp_output_cb` from the TCPIP thread returns ERR_CONN instead of using
  a freed PCB. The netif insertion, `udp_new`, `netif_remove` and the new
  `udp_remove` all run under `wg_lwip_lock()` from the wg_mgr task (see the
  lwIP core locking note above). The netif-down + 100ms drain before
  `netif_remove` stays as a second line of defence.
- `src/ml_wg_mgr.c` (task start): also resets `disco_probe_start_idx` next
  to the existing `pending_probes` memset, which upstream already had.
- `src/ml_derp.c`: the DERP I/O task now calls `ml_derp_disconnect(ml)` on
  its shutdown exit path (previously only on reconnect), freeing the
  mbedTLS ssl/config/ctr_drbg/entropy contexts (~20KB) and closing the
  socket. `ml_derp_disconnect` frees the TLS contexts based on the new
  `tls_inited` flag rather than `sockfd >= 0`, making it idempotent and
  covering failed-connect residue (connect failure paths close the socket
  but leave contexts allocated). `ml_derp_connect` frees leftover contexts
  from a previous failed attempt before re-running `mbedtls_*_init()`
  (re-init on a live context orphans its heap buffers; this also leaked on
  every failed connect retry, independent of teardown).
- `src/ml_coord.c`: the four long-poll `static uint64_t last_*_ms` timers
  (STUN re-probe, DERP NotePreferred keepalive, key-expiry check, H2 ping)
  moved to `ml_coord_task` locals so a restarted session doesn't inherit
  stale timestamps. Task exit now frees/resets the file-static Noise
  handshake state (`s_server_extra_data` heap buffer,
  `s_has_node_key_challenge`).
- `src/ml_net_io.c`, `src/ml_derp.c`, `src/ml_coord.c`, `src/ml_wg_mgr.c`:
  each task sets its `ML_EVT_*_EXITED` bit immediately before
  `vTaskDelete(NULL)` (both exit sites in coord and wg_mgr).

Also checked:
- wireguardif timers: in this fork's magicsock mode `wireguardif_init` never
  arms `sys_timeout(wireguardif_tmr)` (commented out; `wireguardif_periodic`
  is driven from the wg_mgr task), so `wireguardif_shutdown`'s
  `sys_untimeout` is a no-op safety and no timer can fire after the device
  is freed. `WireGuardLwip/src/wireguardif.c` left unchanged.
- Fresh init()+start() after stop()+destroy(): context, event group and
  queues are recreated; remaining file-statics are either reset at task
  start/exit (above) or benign.

Residual single-session state accepted (harmless with monotonic time /
overwritten per probe):
- `ml_wg_mgr.c` `add_peer()` function-statics `last_burst_ms`/`burst_count`
  (ping-burst throttle; self-corrects within 1s).
- `ml_stun.c` `txid_v4/v6[_valid]` statics (overwritten by the next probe;
  a stale-matched late response is impossible across sessions; sockets
  differ).
- `ml_udp.c` `last_trigger_ms` (10s handshake-retrigger throttle; app-owned
  public UDP sockets are outside microlink_stop's scope).
- If a task misses the 3s exit window, stop proceeds after a warning; a
  wedged DERP task could then race the guarded socket close (same exposure
  as the old blind delay, now logged).
- In-flight TCPIP-thread output through the WG netif during teardown is
  mitigated (netif down + 100ms + remove-before-free + PCB NULL-before-
  remove) and the remove calls themselves hold the tcpip core lock; output
  already queued inside the tcpip thread is not lock-proven.
