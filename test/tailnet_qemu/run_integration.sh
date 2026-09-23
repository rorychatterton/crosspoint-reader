#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/../.." && pwd)"
work_dir="$repo_dir/.cache/tailnet-qemu-results"
fixture_pid=""
requested=("$@")

mkdir -p "$work_dir"

cleanup() {
  if [[ -n "$fixture_pid" ]]; then
    kill "$fixture_pid" 2>/dev/null || true
    wait "$fixture_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

should_run() {
  local candidate="$1"
  if [[ "${#requested[@]}" -eq 0 ]]; then
    return 0
  fi
  local item
  for item in "${requested[@]}"; do
    if [[ "$item" == "$candidate" ]]; then
      return 0
    fi
  done
  return 1
}

qemu_release="esp-develop-9.2.2-20260417"
qemu_archive="qemu-riscv32-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz"
qemu_sha256="547f03e04701a92cbb699f7f7d015adc1f5b5ef93cbb94c0dd9b7107e2d84e77"
qemu_cache="$repo_dir/.cache/esp-qemu-download"
qemu_raw="$qemu_cache/qemu/bin/qemu-system-riscv32"
qemu_bin="$qemu_cache/qemu/bin/qemu-system-riscv32.nix"

mkdir -p "$qemu_cache"
if [[ ! -x "$qemu_raw" ]]; then
  archive_path="$qemu_cache/$qemu_archive"
  curl --fail --location --output "$archive_path" \
    "https://github.com/espressif/qemu/releases/download/$qemu_release/$qemu_archive"
  actual_sha256="$(sha256sum "$archive_path" | cut -d' ' -f1)"
  if [[ "$actual_sha256" != "$qemu_sha256" ]]; then
    echo "QEMU archive checksum mismatch: got $actual_sha256" >&2
    exit 1
  fi
  tar -xJf "$archive_path" -C "$qemu_cache"
fi

if [[ ! -x "$qemu_bin" ]]; then
  cp "$qemu_raw" "$qemu_bin"
  dynamic_linker="$(nix develop "$repo_dir/nix" --command bash -lc 'cat "$NIX_CC/nix-support/dynamic-linker"')"
  nix shell nixpkgs#patchelf --command patchelf --set-interpreter "$dynamic_linker" "$qemu_bin"
fi

nix shell nixpkgs#go --command env CGO_ENABLED=0 go -C "$script_dir/control_fixture" test ./...
nix shell nixpkgs#go --command env CGO_ENABLED=0 go -C "$script_dir/control_fixture" build \
  -o "$work_dir/control-fixture" .

# (Re)start the fixture with optional VAR=VALUE impairment settings (see
# control_fixture/impair.go). The log is appended so control_mark line counts
# stay valid across a restart within one invocation.
start_fixture() {
  cleanup
  fixture_pid=""
  env "$@" "$work_dir/control-fixture" >>"$work_dir/control.log" 2>&1 &
  fixture_pid=$!
  local attempt
  for attempt in $(seq 1 50); do
    if (exec 3<>/dev/tcp/127.0.0.1/18080) 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$fixture_pid" 2>/dev/null; then
      break
    fi
    sleep 0.2
  done
  echo "Fixture did not start (env: $*)" >&2
  tail -20 "$work_dir/control.log" >&2
  exit 1
}

: >"$work_dir/control.log"
start_fixture

run_qemu() {
  local environment="$1"
  local seconds="$2"
  local flash="$work_dir/$environment.bin"
  local log="$work_dir/$environment.log"
  local host_log="$work_dir/$environment-qemu.log"

  # PlatformIO only seeds sdkconfig.<env> from sdkconfig.defaults when the
  # former is missing; drop a stale one so edits to the defaults take effect.
  if [[ -f "$script_dir/sdkconfig.$environment" &&
        "$script_dir/sdkconfig.defaults" -nt "$script_dir/sdkconfig.$environment" ]]; then
    rm -f "$script_dir/sdkconfig.$environment"
  fi
  nix develop "$repo_dir/nix" --command pio run -d "$script_dir" -e "$environment" \
    >"$work_dir/$environment-build.log"
  # Compose the flash image from the parts rather than firmware.factory.bin:
  # that file comes from an esptool post-action which SCons skips when every
  # artifact is retrieved from build_cache_dir (a fresh .pio with a warm
  # cache). Offsets are the C3 defaults the post-action itself uses.
  local build="$script_dir/.pio/build/$environment"
  truncate -s 0 "$flash"
  truncate -s 4M "$flash"
  dd if="$build/bootloader.bin" of="$flash" bs=4096 seek=0 conv=notrunc status=none
  dd if="$build/partitions.bin" of="$flash" bs=4096 seek=8 conv=notrunc status=none
  dd if="$build/firmware.bin" of="$flash" bs=4096 seek=16 conv=notrunc status=none
  rm -f "$log"
  # The guestfwd cmd: form runs one forwarder process per guest connection.
  # The chardev form (-tcp:host:port) opens a single host connection at QEMU
  # start and multiplexes every guest connection onto it, which breaks any
  # scenario that opens the control plane or DERP a second time.
  local forward_control="$work_dir/control-fixture -pipe 127.0.0.1:18080"
  local forward_derp="$work_dir/control-fixture -pipe 127.0.0.1:18443"
  timeout --signal=INT "$seconds" nix develop "$repo_dir/nix" --command "$qemu_bin" \
    -M esp32c3 \
    -drive "file=$flash,if=mtd,format=raw" \
    -global driver=timer.esp32c3.timg,property=wdt_disable,value=true \
    -nic "user,model=open_eth,guestfwd=tcp:10.0.2.100:80-cmd:$forward_control,guestfwd=tcp:10.0.2.100:18443-cmd:$forward_derp" \
    -display none -monitor none -serial "file:$log" >"$host_log" 2>&1 || {
      status=$?
      if [[ "$status" -ne 124 && "$status" -ne 130 ]]; then
        cat "$host_log" >&2
        tail -80 "$log" >&2
        return "$status"
      fi
    }
  require_no_crash "$log"
}

# A guest panic fails the scenario before any stage assertion runs. lwIP's
# CONFIG_LWIP_CHECK_THREAD_SAFETY asserts and ESP_ERROR_CHECK aborts both
# reboot the guest (CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT), which can leave a
# later boot's PASS lines in the same log.
require_no_crash() {
  local log="$1"
  local pattern
  for pattern in "assert failed" "Guru Meditation" "abort()" "Backtrace"; do
    if grep -a -q -F "$pattern" "$log"; then
      echo "Guest crashed: '$pattern' in $log" >&2
      grep -a -n -F -m1 -B8 -A24 "$pattern" "$log" >&2
      exit 1
    fi
  done
}

# control.log accumulates across scenarios in one invocation; these scope a
# count to the lines the fixture wrote after a mark.
control_mark() {
  wc -l <"$work_dir/control.log"
}

control_count_since() {
  local mark="$1"
  local pattern="$2"
  tail -n +"$((mark + 1))" "$work_dir/control.log" | grep -c "$pattern" || true
}

# Require a fixture log line written after a mark, so a scenario cannot pass
# on a request an earlier scenario in the same invocation made.
require_control_since() {
  local mark="$1"
  local pattern="$2"
  if [[ "$(control_count_since "$mark" "$pattern")" -lt 1 ]]; then
    echo "Fixture did not log '$pattern' during this scenario" >&2
    tail -n +"$((mark + 1))" "$work_dir/control.log" | tail -40 >&2
    exit 1
  fi
}

log_field() {
  local pattern="$1"
  local field="$2"
  local log="$3"
  grep -a -m1 "$pattern" "$log" | sed -n "s/.*$field=\([0-9]*\).*/\1/p"
}

require_log() {
  local pattern="$1"
  local log="$2"
  if ! grep -q "$pattern" "$log"; then
    echo "Missing expected log pattern: $pattern" >&2
    tail -120 "$log" >&2
    exit 1
  fi
}

if should_run qemu_eager; then
  run_qemu qemu_eager 35
  require_log "QEMU_MODE eager-v5-regression" "$work_dir/qemu_eager.log"
  require_log "QEMU_HEAP target=4200" "$work_dir/qemu_eager.log"
  require_log "TCP connect failed: 113" "$work_dir/qemu_eager.log"
  echo "eager regression: $(grep -m1 'TCP connect failed: 113' "$work_dir/qemu_eager.log" | tr -d '\r')"
fi

# Stages after the peer map: DERP TLS + upgrade against the fixture's local
# relay, coordinator CONNECTED, the relayed WireGuard handshake with the
# fixture's wireguard-go peer, and an HTTP GET through the tunnel over a
# plain lwIP socket. $2 is the control.log mark taken before the run and $3
# the fixture peer that must have served the request.
require_data_plane() {
  local log="$1"
  local mark="$2"
  local peer="$3"
  require_log "TLS connected to DERP" "$log"
  require_log "HTTP 101 Switching Protocols received" "$log"
  require_log "QEMU_PASS stage=derp-connected" "$log"
  require_log "WG handshake triggered (DERP)" "$log"
  require_log "HANDSHAKE COMPLETE" "$log"
  require_log "QEMU_INFO stage=peer-handshake result=ESP_OK" "$log"
  require_log "QEMU_PASS stage=tunnel-http" "$log"
  require_log "body=tailnet-ok $peer calibre.lab.qemu.test" "$log"
  require_control_since "$mark" "DERP_FIXTURE upgrade"
  require_control_since "$mark" "WG_FIXTURE reader_configured"
  require_control_since "$mark" "WG_FIXTURE http peer=$peer host=calibre.lab.qemu.test"
}

# The split-DNS scenarios also resolve the routed name through lab-dns
# (UDP through the tunnel) before fetching through lab-gw.
require_tunnel_dns() {
  local log="$1"
  local mark="$2"
  require_log "QEMU_PASS stage=tunnel-dns ip=100.70.0.42" "$log"
  require_log "QEMU_PASS stage=tunnel-http ip=100.70.0.42" "$log"
  require_control_since "$mark" "WG_FIXTURE dns peer=lab-dns q=calibre.lab.qemu.test"
}

if should_run qemu_v6; then
  v6_mark="$(control_mark)"
  run_qemu qemu_v6 30
  require_log "Noise handshake complete" "$work_dir/qemu_v6.log"
  require_log "RegisterResponse drained incrementally" "$work_dir/qemu_v6.log"
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6.log"
  require_log "full map not required" "$work_dir/qemu_v6.log"
  require_log "Closed control socket after targeted bootstrap" "$work_dir/qemu_v6.log"
  require_log "CONTROL_FIXTURE register_ok" "$work_dir/control.log"
  require_log "CONTROL_FIXTURE map_prefix_sent scenario=crosspoint-qemu-early" "$work_dir/control.log"
  require_data_plane "$work_dir/qemu_v6.log" "$v6_mark" api-gateway
  require_log "QEMU_PASS stage=tunnel-http ip=100.64.0.42" "$work_dir/qemu_v6.log"
  echo "early target: $(grep -m1 'QEMU_PASS stage=target-map' "$work_dir/qemu_v6.log" | tr -d '\r')"
  echo "derp: $(grep -m1 'QEMU_PASS stage=derp-connected' "$work_dir/qemu_v6.log" | tr -d '\r')"
  echo "peer: $(grep -m1 'QEMU_INFO stage=peer-handshake' "$work_dir/qemu_v6.log" | tr -d '\r')"
  echo "tunnel: $(grep -m1 'QEMU_PASS stage=tunnel-http' "$work_dir/qemu_v6.log" | tr -d '\r')"
fi

if should_run qemu_v6_full; then
  full_mark="$(control_mark)"
  run_qemu qemu_v6_full 30
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6_full.log"
  require_log "Target selected after" "$work_dir/qemu_v6_full.log"
  require_log "CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-full" "$work_dir/control.log"
  require_data_plane "$work_dir/qemu_v6_full.log" "$full_mark" api-gateway
  require_log "QEMU_PASS stage=tunnel-http ip=100.64.0.42" "$work_dir/qemu_v6_full.log"
  echo "full map: $(grep -m1 'QEMU_PASS stage=target-map' "$work_dir/qemu_v6_full.log" | tr -d '\r')"
  echo "derp: $(grep -m1 'QEMU_PASS stage=derp-connected' "$work_dir/qemu_v6_full.log" | tr -d '\r')"
  echo "tunnel: $(grep -m1 'QEMU_PASS stage=tunnel-http' "$work_dir/qemu_v6_full.log" | tr -d '\r')"
fi

if should_run qemu_v6_routed; then
  routed_mark="$(control_mark)"
  run_qemu qemu_v6_routed 30
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6_routed.log"
  require_log "Streamed target peer 'lab-gw.integration.test.ts.net'" "$work_dir/qemu_v6_routed.log"
  require_log "Route 100.70.0.42/32 via peer lab-gw" "$work_dir/qemu_v6_routed.log"
  require_log "CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-routed" "$work_dir/control.log"
  require_data_plane "$work_dir/qemu_v6_routed.log" "$routed_mark" lab-gw
  require_log "WG handshake triggered (DERP) to lab-gw" "$work_dir/qemu_v6_routed.log"
  require_log "QEMU_PASS stage=tunnel-http ip=100.70.0.42" "$work_dir/qemu_v6_routed.log"
  echo "routed target: $(grep -m1 'QEMU_PASS stage=target-map' "$work_dir/qemu_v6_routed.log" | tr -d '\r')"
  echo "route: $(grep -m1 'Route 100.70.0.42/32' "$work_dir/qemu_v6_routed.log" | tr -d '\r')"
  echo "tunnel: $(grep -m1 'QEMU_PASS stage=tunnel-http' "$work_dir/qemu_v6_routed.log" | tr -d '\r')"
fi

if should_run qemu_v6_resolver; then
  resolver_mark="$(control_mark)"
  run_qemu qemu_v6_resolver 30
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6_resolver.log"
  require_log "Streamed target peer 'lab-gw.integration.test.ts.net'" "$work_dir/qemu_v6_resolver.log"
  require_log "Streamed resolver peer 'lab-dns.integration.test.ts.net'" "$work_dir/qemu_v6_resolver.log"
  require_log "Route 100.70.0.42/32 via peer lab-gw" "$work_dir/qemu_v6_resolver.log"
  require_log "CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-resolver" "$work_dir/control.log"
  require_data_plane "$work_dir/qemu_v6_resolver.log" "$resolver_mark" lab-gw
  require_log "QEMU_PASS stage=secondary-peer arrived=1 handshake=ESP_OK peers=2" "$work_dir/qemu_v6_resolver.log"
  require_tunnel_dns "$work_dir/qemu_v6_resolver.log" "$resolver_mark"
  echo "resolver: $(grep -m1 'QEMU_PASS stage=secondary-peer' "$work_dir/qemu_v6_resolver.log" | tr -d '\r')"
  echo "dns: $(grep -m1 'QEMU_PASS stage=tunnel-dns' "$work_dir/qemu_v6_resolver.log" | tr -d '\r')"
  echo "tunnel: $(grep -m1 'QEMU_PASS stage=tunnel-http' "$work_dir/qemu_v6_resolver.log" | tr -d '\r')"
fi

if should_run qemu_v6_resolver_late; then
  # lab-dns is the last peer, after lab-gw: the filter must keep streaming
  # past the target until the secondary arrives, so the bytes it consumed
  # must cover the whole map.
  late_mark="$(control_mark)"
  run_qemu qemu_v6_resolver_late 30
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6_resolver_late.log"
  require_log "Streamed target peer 'lab-gw.integration.test.ts.net'" "$work_dir/qemu_v6_resolver_late.log"
  require_log "Streamed resolver peer 'lab-dns.integration.test.ts.net'" "$work_dir/qemu_v6_resolver_late.log"
  require_log "Route 100.70.0.42/32 via peer lab-gw" "$work_dir/qemu_v6_resolver_late.log"
  require_log "CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-resolver-late" "$work_dir/control.log"
  map_bytes="$(grep -a -m1 'Target selected after' "$work_dir/qemu_v6_resolver_late.log" |
    sed -n 's/.* and \([0-9]*\) map bytes.*/\1/p')"
  response_bytes="$(log_field 'map_ok scenario=crosspoint-qemu-resolver-late' response_bytes "$work_dir/control.log")"
  if [[ -z "$map_bytes" || -z "$response_bytes" || "$map_bytes" -lt "$response_bytes" ]]; then
    echo "Filter stopped before the late resolver: consumed ${map_bytes:-?} of ${response_bytes:-?} map bytes" >&2
    exit 1
  fi
  require_data_plane "$work_dir/qemu_v6_resolver_late.log" "$late_mark" lab-gw
  require_log "QEMU_PASS stage=secondary-peer arrived=1 handshake=ESP_OK peers=2" "$work_dir/qemu_v6_resolver_late.log"
  require_tunnel_dns "$work_dir/qemu_v6_resolver_late.log" "$late_mark"
  echo "late resolver: $(grep -m1 'Target selected after' "$work_dir/qemu_v6_resolver_late.log" | tr -d '\r')"
  echo "late resolver: $(grep -m1 'QEMU_PASS stage=secondary-peer' "$work_dir/qemu_v6_resolver_late.log" | tr -d '\r')"
  echo "dns: $(grep -m1 'QEMU_PASS stage=tunnel-dns' "$work_dir/qemu_v6_resolver_late.log" | tr -d '\r')"
  echo "tunnel: $(grep -m1 'QEMU_PASS stage=tunnel-http' "$work_dir/qemu_v6_resolver_late.log" | tr -d '\r')"
fi

if should_run qemu_v6_resolver_absent; then
  # secondary_peer_ip requested but no lab-dns anywhere in the map: the
  # session must still come up for the target alone.
  absent_mark="$(control_mark)"
  run_qemu qemu_v6_resolver_absent 30
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6_resolver_absent.log"
  require_log "Streamed target peer 'lab-gw.integration.test.ts.net'" "$work_dir/qemu_v6_resolver_absent.log"
  require_log "Resolver peer 100.64.0.53 absent from map; continuing with the target only" "$work_dir/qemu_v6_resolver_absent.log"
  require_log "Route 100.70.0.42/32 via peer lab-gw" "$work_dir/qemu_v6_resolver_absent.log"
  require_log "CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-resolver-absent" "$work_dir/control.log"
  if grep -a -q "Streamed resolver peer" "$work_dir/qemu_v6_resolver_absent.log"; then
    echo "A resolver peer was selected from a map that has none" >&2
    exit 1
  fi
  require_data_plane "$work_dir/qemu_v6_resolver_absent.log" "$absent_mark" lab-gw
  require_log "QEMU_PASS stage=resolver-absent target=1 peers=1" "$work_dir/qemu_v6_resolver_absent.log"
  require_log "QEMU_PASS stage=tunnel-http ip=100.70.0.42" "$work_dir/qemu_v6_resolver_absent.log"
  echo "absent resolver: $(grep -m1 'QEMU_PASS stage=resolver-absent' "$work_dir/qemu_v6_resolver_absent.log" | tr -d '\r')"
  echo "tunnel: $(grep -m1 'QEMU_PASS stage=tunnel-http' "$work_dir/qemu_v6_resolver_absent.log" | tr -d '\r')"
fi

if should_run qemu_v6_restart; then
  # The reader's split-DNS flow: a session to the resolver alone, then
  # microlink_stop()/microlink_destroy(), then a fresh session for the target
  # with the resolver as secondary. Teardown runs netif/udp/timer calls that
  # must hold the lwIP core lock (require_no_crash), and the two bring-ups
  # must land on the same heap.
  restart_mark="$(control_mark)"
  run_qemu qemu_v6_restart 35
  require_log "QEMU_PASS stage=derp-connected phase=1" "$work_dir/qemu_v6_restart.log"
  require_log "microlink: Stopped" "$work_dir/qemu_v6_restart.log"
  require_log "microlink: Destroyed" "$work_dir/qemu_v6_restart.log"
  require_log "QEMU_INFO stage=session-stopped" "$work_dir/qemu_v6_restart.log"
  require_log "Streamed target peer 'lab-gw.integration.test.ts.net'" "$work_dir/qemu_v6_restart.log"
  require_log "Streamed resolver peer 'lab-dns.integration.test.ts.net'" "$work_dir/qemu_v6_restart.log"
  require_log "Route 100.70.0.42/32 via peer lab-gw" "$work_dir/qemu_v6_restart.log"
  require_log "QEMU_PASS stage=restart" "$work_dir/qemu_v6_restart.log"
  # The second session must carry traffic to both peers.
  require_log "QEMU_INFO stage=peer-handshake result=ESP_OK resolver=ESP_OK" "$work_dir/qemu_v6_restart.log"
  require_tunnel_dns "$work_dir/qemu_v6_restart.log" "$restart_mark"
  require_log "body=tailnet-ok lab-gw calibre.lab.qemu.test" "$work_dir/qemu_v6_restart.log"
  require_control_since "$restart_mark" "WG_FIXTURE http peer=lab-gw host=calibre.lab.qemu.test"
  connected="$(grep -a -c 'QEMU_STATE value=4 name=connected' "$work_dir/qemu_v6_restart.log" || true)"
  if [[ "$connected" -ne 2 ]]; then
    echo "Expected two CONNECTED transitions, saw $connected" >&2
    exit 1
  fi
  # Count only the reader's relay connections. Each follows its own map fetch;
  # the fixture's WireGuard peers also upgrade to DERP at startup and, when
  # this scenario runs first in an invocation, those land inside the scenario
  # window and would inflate a count taken from restart_mark.
  first_map="$(awk -v m="$restart_mark" \
    'NR>m && /CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-restart/ {print NR; exit}' \
    "$work_dir/control.log")"
  upgrades="$(control_count_since "${first_map:-$restart_mark}" 'DERP_FIXTURE upgrade')"
  if [[ "$upgrades" -ne 2 ]]; then
    echo "Expected two DERP upgrades for the restart scenario, saw $upgrades" >&2
    exit 1
  fi
  free_first="$(log_field 'QEMU_PASS stage=derp-connected phase=1' free "$work_dir/qemu_v6_restart.log")"
  free_second="$(log_field 'QEMU_PASS stage=restart' free "$work_dir/qemu_v6_restart.log")"
  if [[ -z "$free_first" || -z "$free_second" ]]; then
    echo "Could not read free heap from both bring-ups" >&2
    exit 1
  fi
  heap_delta=$((free_first > free_second ? free_first - free_second : free_second - free_first))
  if [[ "$heap_delta" -gt 4096 ]]; then
    echo "Heap drift across restart: first=$free_first second=$free_second delta=$heap_delta (limit 4096)" >&2
    exit 1
  fi
  echo "restart phase 1: $(grep -m1 'QEMU_PASS stage=derp-connected phase=1' "$work_dir/qemu_v6_restart.log" | tr -d '\r')"
  echo "restart stop: $(grep -m1 'QEMU_INFO stage=session-stopped' "$work_dir/qemu_v6_restart.log" | tr -d '\r')"
  echo "restart phase 2: $(grep -m1 'QEMU_PASS stage=restart' "$work_dir/qemu_v6_restart.log" | tr -d '\r') heap_delta=$heap_delta derp_upgrades=$upgrades"
  echo "restart dns: $(grep -m1 'QEMU_PASS stage=tunnel-dns' "$work_dir/qemu_v6_restart.log" | tr -d '\r')"
  echo "restart tunnel: $(grep -m1 'QEMU_PASS stage=tunnel-http' "$work_dir/qemu_v6_restart.log" | tr -d '\r')"
fi

# The bulk stage after the early-map data plane: 1 MiB GET /bulk from
# api-gateway through the tunnel, verified byte for byte on the guest against
# the regenerated stream, MD5 on both sides compared here, and free heap
# after the transfer held to within 4096 bytes of free heap before it. $2 is
# the control.log mark taken before the run.
require_tunnel_bulk() {
  local log="$1"
  local mark="$2"
  local bytes=1048576
  require_log "QEMU_PASS stage=tunnel-bulk ip=100.64.0.42 bytes=$bytes " "$log"
  if grep -a -q "QEMU_FAIL stage=tunnel-bulk" "$log"; then
    echo "Bulk transfer failed: $(grep -a -m1 'QEMU_FAIL stage=tunnel-bulk' "$log" | tr -d '\r')" >&2
    exit 1
  fi
  require_control_since "$mark" "WG_FIXTURE bulk peer=api-gateway bytes=$bytes "
  local guest_md5 fixture_md5
  guest_md5="$(grep -a -m1 'QEMU_PASS stage=tunnel-bulk' "$log" | sed -n 's/.*md5=\([0-9a-f]*\).*/\1/p')"
  fixture_md5="$(tail -n +"$((mark + 1))" "$work_dir/control.log" |
    grep -m1 "WG_FIXTURE bulk peer=api-gateway bytes=$bytes " | sed -n 's/.*md5=\([0-9a-f]*\).*/\1/p')"
  if [[ -z "$guest_md5" || -z "$fixture_md5" || "$guest_md5" != "$fixture_md5" ]]; then
    echo "Bulk MD5 mismatch: guest=${guest_md5:-?} fixture=${fixture_md5:-?}" >&2
    exit 1
  fi
  local free_before free_after
  free_before="$(log_field 'QEMU_PASS stage=tunnel-bulk' free_before "$log")"
  free_after="$(grep -a -m1 'QEMU_PASS stage=tunnel-bulk' "$log" | sed -n 's/.* free=\([0-9]*\).*/\1/p')"
  if [[ -z "$free_before" || -z "$free_after" ]]; then
    echo "Could not read free heap around the bulk transfer" >&2
    exit 1
  fi
  local bulk_delta=$((free_before > free_after ? free_before - free_after : free_after - free_before))
  if [[ "$bulk_delta" -gt 4096 ]]; then
    echo "Heap drift across bulk transfer: before=$free_before after=$free_after delta=$bulk_delta (limit 4096)" >&2
    exit 1
  fi
  echo "bulk: $(grep -m1 'QEMU_PASS stage=tunnel-bulk' "$log" | tr -d '\r') heap_delta=$bulk_delta"
}

if should_run qemu_v6_bulk; then
  bulk_mark="$(control_mark)"
  run_qemu qemu_v6_bulk 150
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6_bulk.log"
  require_log "CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-bulk " "$work_dir/control.log"
  require_data_plane "$work_dir/qemu_v6_bulk.log" "$bulk_mark" api-gateway
  require_tunnel_bulk "$work_dir/qemu_v6_bulk.log" "$bulk_mark"
fi

if should_run qemu_v6_bulk_lossy; then
  # Same firmware and checks under a relay that drops 2%, duplicates 1% and
  # reorders 1% of WireGuard frames. The transfer must still be byte-exact,
  # and the fixture's counters must show every impairment engaged.
  start_fixture FIXTURE_DROP_PCT=2 FIXTURE_DUP_PCT=1 FIXTURE_REORDER_PCT=1
  lossy_mark="$(control_mark)"
  run_qemu qemu_v6_bulk_lossy 240
  require_log "QEMU_PASS stage=target-map" "$work_dir/qemu_v6_bulk_lossy.log"
  require_log "CONTROL_FIXTURE map_ok scenario=crosspoint-qemu-bulk-lossy" "$work_dir/control.log"
  require_data_plane "$work_dir/qemu_v6_bulk_lossy.log" "$lossy_mark" api-gateway
  require_tunnel_bulk "$work_dir/qemu_v6_bulk_lossy.log" "$lossy_mark"
  require_control_since "$lossy_mark" "WG_FIXTURE impairment_stats peer=api-gateway"
  stats="$(tail -n +"$((lossy_mark + 1))" "$work_dir/control.log" |
    grep -m1 'WG_FIXTURE impairment_stats peer=api-gateway')"
  for counter in dropped duplicated reordered; do
    value="$(sed -n "s/.*$counter=\([0-9]*\).*/\1/p" <<<"$stats")"
    if [[ -z "$value" || "$value" -eq 0 ]]; then
      echo "Impairment did not engage ($counter=${value:-?}): $stats" >&2
      exit 1
    fi
  done
  echo "lossy: $(sed 's/.*WG_FIXTURE/WG_FIXTURE/' <<<"$stats")"
  start_fixture
fi

if should_run qemu_v6_stalled; then
  run_qemu qemu_v6_stalled 10
  require_log "QEMU_PASS stage=stalled-map-diagnostic" "$work_dir/qemu_v6_stalled.log"
  require_log "CONTROL_FIXTURE map_prefix_sent scenario=crosspoint-qemu-stalled" "$work_dir/control.log"
  echo "stalled map: $(grep -m1 'QEMU_PASS stage=stalled-map-diagnostic' "$work_dir/qemu_v6_stalled.log" | tr -d '\r')"
fi

if should_run qemu_v6_missing; then
  run_qemu qemu_v6_missing 8
  require_log "QEMU_PASS stage=missing-target-error" "$work_dir/qemu_v6_missing.log"
  require_log "target peer absent" "$work_dir/qemu_v6_missing.log"
  echo "missing target: $(grep -m1 'QEMU_PASS stage=missing-target-error' "$work_dir/qemu_v6_missing.log" | tr -d '\r')"
fi

if should_run qemu_v6_malformed; then
  run_qemu qemu_v6_malformed 8
  require_log "QEMU_PASS stage=malformed-map-error" "$work_dir/qemu_v6_malformed.log"
  require_log "Malformed HTTP/2 or JSON" "$work_dir/qemu_v6_malformed.log"
  require_log "CONTROL_FIXTURE map_malformed scenario=crosspoint-qemu-malformed" "$work_dir/control.log"
  echo "malformed map: $(grep -m1 'QEMU_PASS stage=malformed-map-error' "$work_dir/qemu_v6_malformed.log" | tr -d '\r')"
fi
