# Tailnet features: offline test plan

Scope: OPDS browse and download through the Tailscale tunnel, and KOReader
progress sync through the tunnel, on the ESP32-C3 reader. "Offline" means
without a person holding the device: a headless self-test image flashed and
captured over USB from the Mac (chattstudio), plus host-side checks from the
dev VM (100.93.9.14, on the same tailnet).

## 1. Status (2026-09-18)

Working on the reader (firmware flashed 16:15):

- Browse root feed and sub-feeds through the tunnel. Each page frees the
  48 KB framebuffer, brings the tunnel up (~6 s), fetches the feed to
  `/.crosspoint/opds_feed.xml`, tears the tunnel down, reclaims the
  framebuffer, parses the spool. If reclaim fails, it reboots silently into
  the browser and renders from the spool; the `/.crosspoint/opds_resume.txt`
  sidecar carries path and history.
- Download through the tunnel: a 9 MB EPUB in 294 s (~30 KB/s, bound by
  `CONFIG_LWIP_TCP_WND_DEFAULT=5760` over a ~190 ms relay round trip), with a
  clean reclaim afterwards.

Built, not yet flashed (working tree):

- Bring-up refused on a fragmented heap now reboots once into the browser
  with the fetch pending, instead of an error page.
- KOReader sync uses the same release/tunnel/teardown/reclaim model
  (`KOReaderSyncActivity::overTailnet`), with heap logging per phase and a
  bounded reclaim retry. The previous "OOM" was the tunnel's 80 KB floor being
  checked with the framebuffer held (~71 KB free).
- Downloader verifies the SD close, the byte count against Content-Length,
  and the on-disk size, and logs throughput at INFO.
- Tunnel target host string is set before the framebuffer release in all
  paths. The string outlives teardown, and inside the region it blocks
  reclaim.
- Tunnel task stacks trimmed 8 KB (net_io/derp_tx/wg_mgr 6->4 KB, coord
  8->6 KB). Device watermarks at the old sizes were 3.5-4.6 KB min-free.
  Validate on the self-test before trusting: `STACK_HWM` min-free must stay
  above 1 KB on every task.

Open questions the plan must answer:

1. Is the downloaded 9 MB book byte-exact? One download on the device ended
   with "won't index due to corruption". Both encryption layers (TLS inside
   WireGuard) reject altered bytes, so silent corruption is implausible;
   truncation via an unchecked SD close, or a low-heap section build that
   shows the same "Failed to index" popup, are the live hypotheses.
2. Does sync work end to end on the new model, including the second tunnel
   bring-up in smart mode?
3. What are the ~6 small blocks (24-108 bytes gross) that survive teardown
   inside the freed framebuffer region, and can they be removed so the
   no-reboot path becomes the norm?

## 1a. Additions on 2026-09-19 (built, not flashed)

- Self-test (`src/TailnetSelfTest.cpp`): SD config `/.crosspoint/selftest.json`,
  multi-cycle per boot with `BOOT_SUMMARY`, poison-header-aware survivor dump
  with task owners in `[env:selftest_owner]`, download mode with on-device
  MD5, sync mode in `[env:selftest_sync]`, and resume mode (`"mode":"resume"`,
  `resumeMode` 0/1/2) that writes the browser sidecar and reboots into the real
  browser; the resumed boot hands off to the UI when the RTC attempt counter
  is set. `TailnetSession::isStopDeferred()` added.
- Driver: `scripts/hw/selftest_run.py` with `--parse-only` and `--selftest`
  (55 assertions against real logs in `scripts/hw/fixtures/`), plus
  `scripts/hw/mac/{chunkflash.sh,capture.py}` and a README runbook. Run it
  with `~/.platformio/penv/bin/python3`; the VM has no system python3.
- Browser: public `writeResumeSidecar`, log lines `Resume from spool`,
  `Feed applied`, `Rendered browse`; `CMD:REBOOT` serial command in main.cpp.
- Settings authenticate screen ported to the tunnel memory model.
- Sync client: missing JSON fields no longer become the string "null";
  `reserveBaseUrlOverride` keeps the override's storage out of the released
  region. Downloader: close/size/Content-Length verification.
- Host unit tests: `test/koreader_sync_decision`, `test/http_framing` (39
  tests). Run with the cmake flow in `test/README`; the chapter parser suite
  needs system `expat.h`, absent on this VM.
- Abortive-close experiment behind `-DCROSSPOINT_TCP_ABORTIVE_CLOSE` in
  `SecureClient::stop()` and `ml_derp_disconnect()`. The `SecureClient` side
  lives in the freeink-sdk submodule and is kept as
  `docs/patches/freeink-sdk-tcp-abortive-close.patch`; apply it from the
  submodule root with `git apply ../docs/patches/freeink-sdk-tcp-abortive-close.patch`.
  Inert until `CONFIG_LWIP_SO_LINGER=y` is added to the base
  `custom_sdkconfig`; a TCP pcb is ~180 bytes, larger than the observed
  24-108 byte survivors, so treat this as a hypothesis test only.

- QEMU (`test/tailnet_qemu`): new `qemu_v6_bulk` (1 MiB byte-exact through
  the tunnel, MD5 checked on both ends, heap drift 16-20 bytes) and
  `qemu_v6_bulk_lossy` (2% drop, 1% dup, 1% reorder injected at the relay;
  35 drops, 4 dups, 2 reorders observed; still byte-exact). All 13 scenarios
  pass with the trimmed stacks. Stack marks under mbedTLS after the trim:
  coord 2544/6144, derp_tx 1260/4096, net_io 708/4096, wg_mgr 956/4096
  min-free; usage is unchanged from before the trim and does not move with
  transfer size. net_io's 708 bytes is under the rubric's 1 KB warning; the
  device runs wolfSSL and used ~1 KB less on net_io, so the first self-test
  run's `STACK_HWM` line decides whether net_io goes back to 5 KB.

## 1b. Navigation latency work (2026-09-21, built, not flashed)

Per-page tunnel cost measured at ~7.4 s: ~5.0 s control plane (registration
and peer map), ~0.7 s DERP TLS, ~1.2 s two sequential WireGuard handshakes,
~0.5 s teardown. Implemented:

- Feed cache on SD (`lib/OpdsFeedCache`, browser activity). Every fetched
  page is kept under `/.crosspoint/opds_cache/` (16 entries, 512 KB, LRU);
  Back, forward revisits and the post-download reload are served from it
  with no WiFi or tunnel. Hit rule: fetched within 24 h (when the clock is
  SNTP-synced) or in the same browse session (RTC session id, survives the
  silent reboots). The ERROR-screen retry bypasses the cache. Log lines:
  `Feed served from cache path= bytes= age_s=` and `Feed cached path= bytes=`.
  Serial `CMD:OPDS_OPEN <row>` / `CMD:OPDS_BACK` drive it headlessly.
- Warm start (MicroLink fork + `TailscaleStore` + `TailnetSession`). After
  a clean cold session the self IP, relay region and the two peers (with
  their route) are cached; later sessions skip the control plane, start all
  data-plane tasks at once, and initiate both WireGuard handshakes together.
  Stale cache (peer key rotated, node expired) fails the 4 s handshake
  budget, logs `Warm start failed (...); cold start`, clears the cache and
  cold-starts in the same call. Expected bring-up ~2.5 s.
  `[TIMING] control dns= tcp= noise= h2= reg= map= derp_dns= derp_tcp=
  derp_tls= derp_proto= wait= warm= total= ms` is logged once per session.
  Self-test option `poisonNetmapCache` exercises the fallback; the driver
  records warm/cold per iteration and separate medians.
- Not done, by design: keeping the tunnel resident across pages, which needs
  the `persist` self-test experiment first, and keeping the WireGuard session
  in RAM across relay reconnects, a deeper fork change worth about 1 s more.

Verification on the device: `feed_cycles5` should show cycle 1 `Cold start`,
`Netmap cached`, then `Warm start from cached netmap` with `Tailnet up after`
well under 3000 ms; a run with `poisonNetmapCache: true` should show one
`Warm start failed` followed by a cold `Tailnet up after` in the same cycle;
`resume` mode plus `CMD:OPDS_OPEN 0` then `CMD:OPDS_BACK` should show
`Feed served from cache` with no `Starting tailnet session` in between.

## 2. Forensics first: the book already on the SD card

Do this before opening OPDS again; a re-download of the same title deletes
the file (`HttpDownloader::downloadToFile` removes an existing path).

1. Get the file off the device: pull the microSD (byte-exact) or use the File
   Transfer web server: `curl -o dev.epub "http://<device-ip>/download?path=/<Author - Title>.epub"`.
2. Local checks: `ls -l dev.epub`, `md5 dev.epub`, `unzip -t dev.epub`
   (per-entry CRC-32 is the integrity proof), `unzip -lv dev.epub`.
3. Server reference from the VM (needs the OPDS password):
   `curl -u rory -sI "<acquisition href>"` and record Content-Length; a
   missing length means a close-delimited body the client cannot verify. Then
   `curl -u rory -o ref.epub "<href>"`, `md5 ref.epub`, `cmp -l dev.epub ref.epub | head`.
   Calibre embeds current metadata into `/get/` downloads, so compare against
   a fresh download, not the library file.
4. Decide: same size and md5 and `unzip -t` clean means intact, go to 5.
   Shorter than Content-Length means truncation (SD close/sync or framing).
   Same size, different md5: 512-byte-aligned diffs point at the SD write
   path; 2048-aligned at the TLS client buffer; scattered bytes would
   contradict the encryption argument and needs escalation.
5. If intact, open the book with serial attached and grep the log.
   Corruption: `[ZIP] Not a valid zip file header`, `[ZIP] Decompressed size
   mismatch`, `[ZIP] Decompression failed`, `[SCT] Failed to stream item
   contents`. Heap: `[SCT] OOM:`, `[SCT] Insufficient heap to hydrate CSS`,
   `[EBP] Insufficient heap`. Cross-check by uploading the verified copy under
   a new name via the web UI and opening that.

Note: the indexer never checks ZIP CRCs, so "indexed OK" is not proof of
integrity either; `unzip -t` is.

## 3. Host-side sync checks from the VM (no device needed)

Headers match `KOReaderSyncClient::applyAuthHeaders`. `-k` mirrors the
client's `setInsecure()`.

```bash
BASE=https://calibre.lab.wayvz.io
USER=rory; PASS='<plaintext>'
KEY=$(printf '%s' "$PASS" | md5sum | cut -d' ' -f1)
H=(-H "Accept: application/vnd.koreader.v1+json" -H "x-auth-user: $USER" -H "x-auth-key: $KEY" -u "$USER:$PASS")

curl -sk -o /dev/null -w '%{http_code}\n' "${H[@]}" $BASE/users/auth          # must be 2xx
curl -sk -o /dev/null -w '%{http_code}\n' "${H[@]}" $BASE/kosync/users/auth   # only if the first is 404

HASH=$(printf '%s' "My Book.epub" | md5sum | cut -d' ' -f1)   # Filename match method
curl -sk "${H[@]}" $BASE/syncs/progress/$HASH; echo
curl -sk -o /dev/null -w '%{http_code}\n' -X PUT "${H[@]}" -H 'Content-Type: application/json' \
  -d "{\"document\":\"$HASH\",\"progress\":\"/body/DocFragment[3]/body/p[7]/text().12\",\"percentage\":0.123,\"device\":\"CrossPoint\",\"device_id\":\"crosspoint-reader\"}" \
  $BASE/syncs/progress
curl -sk "${H[@]}" $BASE/syncs/progress/$HASH; echo
```

Caveat: the client maps a 404 to NOT_FOUND. If the server mounts KOSync under
a prefix, every GET looks like "no remote progress", smart mode uploads, and
the PUT fails. Confirm the base path before any device run. Use a throwaway
hash (for example md5 of `crosspoint-selftest.epub`) for destructive PUTs.

Binary match method (the smart-mode alternate probe) reference:

```python
import hashlib, sys, os
p = sys.argv[1]; n = os.path.getsize(p); m = hashlib.md5()
with open(p, 'rb') as f:
    for i in range(-1, 11):
        off = 0 if i < 0 else 1024 << (2 * i)
        if off >= n: continue
        f.seek(off); m.update(f.read(1024))
print(m.hexdigest())
```

## 4. Self-test extensions (headless, on the device)

All in `src/TailnetSelfTest.cpp` under `CROSSPOINT_TAILNET_SELFTEST`. Keep
`SELFTEST RESULT=` as the first token of the verdict line.

### 4.1 Config and multi-cycle

- Read `/.crosspoint/selftest.json` via `PersistableStoreBase::readDocFromFile`
  with keys `fbBytes`, `ballastFree`, `cycles`, `mode` (`feed|download|sync|resume`),
  `downloadUrl`, `interCycleDelayMs`, `resumeMode`. Build flags stay as
  defaults. Log `SELFTEST config fb= ballast= cycles= mode= src=`.
- Split into `loadConfig`, `connectWifiHeadless`, `allocateModel` (once per
  boot), `runCycle`, `logBootSummary` (`SELFTEST BOOT_SUMMARY cycles= pass=
  fail= first_fail= deferred_stop=`). A reclaim failure does not end the boot:
  continue with the model unheld, which is the fragmented state the UI reaches.
- Add `bool TailnetSession::isStopDeferred() const` so a deferred stop is
  reported directly rather than inferred from the next TS-E11.
- Survivor dump: `CONFIG_HEAP_POISONING_LIGHT=y` means the walker reports the
  8-byte poison header (`abba1234`, then `alloc_size`). Skip 8 bytes for the
  hexdump and print the user size. Add `[env:selftest_owner]` with
  `CONFIG_HEAP_TASK_TRACKING=y` and print the owning task handle compared
  against handles captured at boot (`tcpip_thread`, `wifi`, `loopTask`).

### 4.2 Download mode

- Flags or config: `downloadUrl`, expected size, expected MD5, reps.
- Reference hosting, most isolated first: plain HTTP on the VM
  (`python3 -m http.server 8080 --bind 100.93.9.14` serving a 9 MB
  `/dev/urandom` file); HTTPS on the VM with a self-signed cert (the client
  accepts anything); Calibre itself with the OPDS store's credentials.
- Per rep: `HttpDownloader::downloadToFile` to `/.crosspoint/selftest_dl.bin`,
  progress line every 10 s with KB/s and heap; after teardown and reclaim,
  re-read the file with `esp_rom_md5_*` and log
  `SELFTEST dl rep= result= bytes= expect= content_length= secs= kbps=
  file_md5= expect_md5= size_ok= file_ok= minfree= largest=`.
- Optional: walk the ZIP central directory and CRC each entry. This needs the
  CRC field added to `FileStatSlim`, which `ZipFile.cpp` currently skips.

### 4.3 Sync mode (`[env:selftest_sync]`)

- Preconditions logged: credentials present, `useTailnet`, auth key.
- Book: `APP_STATE.openEpubPath`, else first recent book, else a flag.
  Compute primary and alternate hashes before the release. Use a synthetic
  third hash for writes so the real record is only read.
- Window A: bring-up, `getProgress(primary)`, `getProgress(alt)`,
  `getProgress(synthetic)` baseline, teardown, reclaim with retries.
- Window B: second bring-up, which is what catches a deferred stop, then
  `updateProgress(synthetic, pct=0.10+0.01*i)`, `getProgress(synthetic)`,
  assert percentage, xpath, device, timestamp; teardown; reclaim.
- Verdict `SELFTEST RESULT=PASS iters=N` only if both windows come up, no
  LOW_MEMORY or NETWORK_ERROR, round trip asserted, both reclaims OK.
- Must be absent from the capture: `Insufficient allocatable heap for TLS
  handshake`, `Not enough heap for tailnet session`, `Tailnet teardown
  deferred to reboot`, `did not stop cleanly`, `TS-E`, `Guru Meditation`,
  `abort()`. Must be present: `Tailnet up after`, `Get progress response: 2`,
  `Update progress response: 2`.
- Matrix: ballast 71000 (the reader's real free heap with the framebuffer
  held), 60000, 48000; iterations 5 then 20.
- `ProgressMapper` needs the display, which the self-test runs before
  initialising; it is covered by the manual checklist and unit tests.

### 4.4 Reboot-into-browse mode (`resume`)

- Hop 1 (self-test): normal spooled fetch and teardown, write the sidecar via
  an extracted `OpdsBookBrowserActivity::writeResumeSidecar`, log
  `SELFTEST resume hop=1 mode= idx=`, call `silentRestartToOpds(idx, false)`.
- Hop 2: `runTailnetSelfTest` returns immediately when
  `tailnetRebootAttemptCount() > 0`, so `setup()` initialises the display and
  routes to the real browser. Evidence lines to add: `Resume from spool:
  mode= path= history=`, `Feed applied: entries= truncated=`, `Rendered
  browse: rows= selected=`. Pixel proof: send `CMD:SCREENSHOT` over serial
  and check the ink fraction of the 48000-byte frame.
- `resumeMode=1` proves parse and render with no network; `resumeMode=2`
  proves the full pending-fetch path including WiFi auto-connect;
  `resumeMode=0` proves the error screen.
- Add `CMD:REBOOT` next to `CMD:SCREENSHOT` in `main.cpp` (dev builds only)
  so the driver can start the next iteration.

## 5. Driver: `scripts/hw/selftest_run.py`

One stdlib Python script on the VM that drives the Mac over ssh:

1. Preflight: port listed, `~/fwvenv` has esptool and pyserial, no stray
   `capture.py`.
2. Stage: scp the image and `scripts/hw/mac/{chunkflash.sh,capture.py}` to
   `~/hw/`; skip the flash when the sha256 matches the last flashed image.
3. Flash with the chunked script (44 x 128 KB, `--before usb-reset --after
   no-reset`, verified per chunk). Any sustained esptool write over ~3 s
   drops the USB link on this Mac; do not collapse the chunks into one write.
4. Capture: open the port, pulse RTS (`DTR=0, RTS=1, 0.2 s, RTS=0, DTR=1`),
   append to `~/hw/run.log`; stream it back with `tail -F` and parse live.
   The firmware drops serial output unless the host is attached at boot.
5. Loop until N `RESULT`/`BOOT_SUMMARY` lines, a crash, or a timeout
   (360 s per iteration, 120 s silence triggers one re-pulse then fails).
6. Restore: flash the default image, confirm `Starting CrossPoint` and
   `Entering activity: (Home|Boot)` within 60 s, record `restore_ok`.
7. On panic: read the coredump partition (`0xFF0000`, 64 KB) before anything
   else; symbolise MEPC/RA with `riscv32-esp-elf-addr2line -e
   .pio/build/selftest/firmware.elf`.

Per-iteration record (JSONL and CSV): result, stage, TS code, wifi ms, fb and
ballast figures, free/largest at release, tunnel start, DERP handshake heap,
tunnel-up ms, TLS ms and suite, fetch ok/bytes/ms, `STACK_HWM` per task, stop
timeout, deferred flag, post-teardown free/largest, reclaim ok/tries,
survivors (count, bytes, phases, dumps), spool readback, flags
(`stall_after_tls`, `tls_connect_fail`, `not_enough_heap`, `deferred_stop`,
`unexpected_reset`, `crash`), panic text and coredump path.

Regexes (reader lines are `[<ms>] [ERR|INF|DBG] [<TAG>] msg`):

```
BOOT      ^ESP-ROM:esp32c3
RST       ^rst:0x[0-9a-f]+ \((\w+)\),boot:
TSN_UP    \[TSN\] Tailnet up after (\d+) ms \(free heap (\d+)\)
NO_HEAP   \[TSN\] Not enough heap for tailnet session \(free=(\d+), maxAlloc=(\d+)\)
TLS_OK    \[SecureClient\] handshake ok \(auto\): (\S+) / (\S+) in (\d+) ms
FETCH     SELFTEST fetch=(OK|FAIL) bytes=(\d+) after=(\d+)ms free=(\d+) largest=(\d+)
HWM       STACK_HWM min-free/size bytes: coord=(\d+)/(\d+) derp_tx=(\d+)/(\d+) net_io=(\d+)/(\d+) wg_mgr=(\d+)/(\d+)
STOP_TO   microlink: Stop timed out: tasks still running
DEFERRED  \[TSN\] Tailnet teardown deferred to reboot
RECL      SELFTEST fb reclaim=(OK|FAIL) \((\d+) bytes\) free=(\d+) largest=(\d+)
SURV_HDR  SELFTEST survivors=(\d+) in region
RESULT    SELFTEST RESULT=(PASS|FAIL)
PANIC     ^Guru Meditation Error|^abort\(\) was called|^assert failed|Task watchdog got triggered
```

Classification: a new `BOOT` while an iteration is open is `unexpected_reset`;
reset reasons other than `USB_UART_CHIP_RESET`, `USB_JTAG_CHIP_RESET`,
`RTC_SW_CPU_RST`, `SW_RESET` are regressions (watchdogs, brown-out).
`stall_after_tls` = TLS OK then fetch FAIL with 0 bytes at ~60 s.

Exit codes: 0 pass, 1 rubric regression, 2 infrastructure (port, flash, ssh,
WiFi twice in a row), 3 crash.

## 6. Test matrix

| scenario | fb | ballast free | cycles/boot | mode | purpose |
|---|---|---|---|---|---|
| feed_baseline | 48000 | 36000 | 1 | feed | the existing test; reclaim and survivor rate |
| feed_nofb | 0 | 84000 | 1 | feed | same heap without region tracking |
| feed_loose | 48000 | 60000 | 1 | feed | does headroom change the survivors |
| feed_tight | 48000 | 28000 | 1 | feed | expected TS-E03 refusal, no crash |
| feed_cycles5 | 48000 | 36000 | 5 | feed | fragmentation across cycles; first refused cycle |
| feed_cycles5_wait | 48000 | 36000 | 5 | feed, 130 s between | 2 x TCP MSL; if survivors vanish they are TIME_WAIT sockets |
| download_9mb | 48000 | 36000 | 1 | download | byte-exact soak, throughput, heap minimum |
| sync_roundtrip | 48000 | 71000 / 60000 / 48000 | 1 | sync | both windows, round trip, second bring-up |
| resume_spooled | 48000 | 36000 | 1 | resume 1 | render from spool after reboot |
| resume_pending | 48000 | 36000 | 1 | resume 2 | full pending-fetch path |
| owner_tracking | 48000 | 36000 | 3 | feed | survivor owners via task tracking |

## 7. Pass/fail rubric (N >= 10 iterations)

Always a regression: any crash or unexpected reset; `STACK_HWM` min-free
under 512 bytes on any task (warn under 1024, and warn if any task drops more
than 512 bytes below baseline); spool readback not equal to fetch bytes;
`not_enough_heap` on cycle 1 of a fresh boot (except `feed_tight`, where its
absence is the regression); tunnel-up p95 over 30 s with a cached address;
fetch p95 over 15 s for a small feed; download throughput under half of
baseline; `restore_ok` false.

Flaky but acceptable, with ceilings: `stall_after_tls` at most 10% and never
twice in a row, and the next iteration must pass; `deferred_stop` at most 5%
and only following a stall; `reclaim_fail_survivors` at most baseline plus
15 points, with at most 8 survivors, at most 768 bytes gross, and no
survivor tagged F (fetch phase). `feed_cyclesN` first failure must not move
earlier than baseline minus one.

Gates: `feed_baseline`, `download_9mb`, `sync_roundtrip` at least 80% PASS
within the ceilings; `resume_*` 100% on the hop-2 evidence lines.

## 8. Code changes still to make

- `KOReaderAuthActivity` (Settings > Authenticate / Sign Up) still brings the
  tunnel up with the framebuffer held and will fail on this hardware. Port
  the `overTailnet` pattern; needs a `silentRestart(bool paint)` overload.
- Downloader: log which framing was used and treat a close-delimited body as
  an error for book downloads (books always carry a length).
- Self-test: everything in section 4. Driver: section 5.
- Optional: raise `CONFIG_LWIP_TCP_WND_DEFAULT` (currently 5760) a few MSS
  and measure with `download_9mb`; roughly halves the 294 s transfer.
- Optional diagnostics in the vendored tunnel: counters for `wg_rx_queue`
  overflow, `pbuf_alloc` failure, replay drops, logged at teardown next to
  `STACK_HWM`.
- Host unit tests (`test/`, gtest, `pio run -t unit-tests`): extract the
  smart-sync decision and alternate-hash merge into a header-only
  `SyncDecision.h`; test `parseProgressResponse`; test the downloader's
  framing completeness cases.
- QEMU (`test/tailnet_qemu`, mbedTLS, no WiFi or SD): add a bulk transfer
  stage with a deterministic body and loss/reorder injection in the fixture.
  This catches genuine tunnel corruption without a device; it cannot test the
  48 KB reclaim, wolfSSL peaks, or WiFi buffers.

## 9. Manual on-device checklist

1. Plug in; `ssh chattstudio 'ls /dev/cu.usbmodem*'` must list the port.
2. Section 2 forensics on the existing book before anything else.
3. Section 3 host checks; fix the sync base URL if the prefix is wrong.
4. Flash `selftest` and run `feed_baseline` x10; store the baseline. Check
   `STACK_HWM` against the trimmed sizes.
5. Run `download_9mb`, `sync_roundtrip`, `resume_spooled`, `resume_pending`,
   `owner_tracking`.
6. Flash `default` (working tree build). With serial attached: open a book,
   Menu > Sync Progress. Expect `tailnet window released free>=80000`,
   `Tailnet up after`, `Primary remote ... http=`, either `Upload complete`
   or the compare screen, `Silent restart (target=reader)`, reader back on
   the same page. Then: remote-ahead (PUT a higher percentage from the VM,
   sync, page moves), local-ahead (turn pages, sync, VM GET updated),
   already-synced (one bring-up only), Ask mode compare screen, wrong
   password, WiFi dropped mid-sync (TS-E error screen, Back returns).
7. One real UI download of the same 9 MB book; compare the
   `[HTTP] Downloaded ... content-length` line and the file md5 against the
   VM; open the book and capture the index log.
8. After every run: no `Framebuffer not reclaimable`, no crash-report screen
   on boot, `restore_ok` true.
