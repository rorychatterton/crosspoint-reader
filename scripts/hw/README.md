# scripts/hw: headless tailnet self-test driver

Implements section 5 of `docs/tailnet-offline-test-plan.md`. One stdlib
Python script on the VM (`selftest_run.py`) drives the Mac (`chattstudio`)
over ssh: stages the image and helpers, flashes the reader in 128 KB chunks,
captures serial into `~/hw/run.log`, streams it back with `tail -F`, parses
it live, restores the default image and grades the run against the section 7
rubric.

```
scripts/hw/
  selftest_run.py        driver + parser + rubric (runs on the VM)
  mac/chunkflash.sh      chunked esptool flash (runs on the Mac, staged to ~/hw/)
  mac/capture.py         serial capture with RTS pulse (runs on the Mac, staged to ~/hw/)
  fixtures/              trimmed device logs used by --selftest / --parse-only
  baselines/             one JSON per scenario, produced from a good run (see below)
  runs/                  outputs, one directory per run (gitignore locally if noisy)
```

## Prerequisites

VM:

- Python 3.8+ on `PATH` as `python3`; stdlib only. On the NixOS VM there is no
  system python: use `~/.platformio/penv/bin/python3` (PlatformIO's venv) or
  `nix shell nixpkgs#python3`.
- Passwordless ssh to the Mac: `ssh rorychatterton@chattstudio true` must work
  without a prompt (`BatchMode=yes` is used).
- Built images: `pio run -e selftest` and `pio run -e default` produce
  `.pio/build/<env>/firmware.factory.bin`. The driver flashes factory images
  (bootloader + partitions + app from offset 0); passing `firmware.bin`
  would brick the boot, so it warns when the name lacks `factory`.
- For crash symbolisation: `.pio/build/selftest/firmware.elf` and
  `.cache/platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line`
  (or `~/.platformio/packages/...`; both are probed).

Mac (`chattstudio`):

- Reader plugged in and listed as `/dev/cu.usbmodem8401` (`--port` to override).
- `~/fwvenv` with `esptool` and `pyserial` (`~/fwvenv/bin/python -c 'import esptool, serial'`).
- `~/hw/` is created by the driver; the helpers and image are copied there
  (skipped when the sha256 already matches).
- No other process holding the port. Preflight kills a leftover
  `~/hw/capture.py` from an aborted run; any other `capture.py` aborts the run.

The device reads its own scenario configuration from
`/.crosspoint/selftest.json` on the SD card (`fbBytes`, `ballastFree`,
`cycles`, `mode`, `downloadUrl`, `interCycleDelayMs`, `resumeMode`) or falls
back to the build flags in `platformio.ini` (`[env:selftest]`). The driver
cannot write the SD card; `--scenario` only labels the run, picks the rubric
variant and warns when the device's `SELFTEST config` line disagrees with the
section 6 matrix.

## Runbook (plan section 9, steps 1-5)

1. Plug in and confirm the port:
   `ssh rorychatterton@chattstudio 'ls /dev/cu.usbmodem*'`.
2. Section 2 forensics on the book already on the SD card (before opening
   OPDS again; a re-download deletes the file).
3. Section 3 host-side sync checks from the VM; fix the KOSync base path if the
   server mounts it under a prefix.
4. Flash `selftest` and run `feed_baseline` x10, then store the baseline:

   ```bash
   pio run -e selftest && pio run -e default
   python3 scripts/hw/selftest_run.py --scenario feed_baseline --iterations 10
   # outputs: scripts/hw/runs/<ts>_feed_baseline/{run.log,iterations.jsonl,iterations.csv,summary.json,boots.json,restore.log}
   cp scripts/hw/runs/<ts>_feed_baseline/summary.json scripts/hw/baselines/feed_baseline.json
   ```

   Check `hwm_min` in the summary against the trimmed task stacks: every task
   must stay above 1024 B min-free (the rubric fails under 512, warns under
   1024).
5. Run the rest of the matrix, each against its baseline once one exists:

   ```bash
   python3 scripts/hw/selftest_run.py --scenario download_9mb  --iterations 10 --baseline scripts/hw/baselines/download_9mb.json
   python3 scripts/hw/selftest_run.py --scenario sync_roundtrip --iterations 10
   python3 scripts/hw/selftest_run.py --scenario resume_spooled --iterations 10
   python3 scripts/hw/selftest_run.py --scenario resume_pending --iterations 10
   python3 scripts/hw/selftest_run.py --scenario owner_tracking --iterations 10 --image .pio/build/selftest_owner/firmware.factory.bin
   ```

   Re-running the same image: add `--skip-flash-if-same` (compares the
   sha256 against `~/hw/last_flashed.sha256` on the Mac). To put the reader
   back on the default build without a test run: `--restore-only`.

Steps 6-8 of section 9 are manual (default image, serial attached).

### What a run does

| phase | action | failure |
|---|---|---|
| preflight | port listed, `~/fwvenv` imports esptool+pyserial, no stray `capture.py`, `mkdir ~/hw` | exit 2 |
| stage | scp image + `mac/chunkflash.sh` + `mac/capture.py` to `~/hw/` (sha256 skip) | exit 2 |
| flash | `bash ~/hw/chunkflash.sh <image> <port>`; requires the literal `FLASH COMPLETE` | exit 2 |
| capture | rotate `run.log`, `nohup capture.py <port> run.log --send-file send.txt`, then `ssh tail -n +1 -F ~/hw/run.log` parsed live | exit 2 if the stream ends |
| loop | until `--iterations` closed iterations, a crash (unless `--continue-on-crash`), `--iteration-timeout` (360 s), `--total-timeout` (3600 s) or `--silence-timeout` (120 s: one RTS re-pulse via `SIGUSR1`, then fail) | see exit codes |
| crash forensics | stop capture, `esptool read-flash 0xFF0000 0x10000` (the `coredump` partition), scp to the run dir, print/run `riscv32-esp-elf-addr2line -pfiaC -e firmware.elf <MEPC> <RA> <backtrace>` | exit 3 |
| restore | flash `--default-image`, `esptool --before default-reset --after hard-reset chip-id`, capture 60 s (one re-pulse, +30 s), require `Starting CrossPoint version` and `Entering activity: (Home|Boot|EpubReader)`; `restore_ok` in summary | rubric FAIL |

Exit codes: 0 pass, 1 rubric regression (or a timeout with an iteration
open), 2 infrastructure (port, ssh, flash, stream ended, WiFi failed twice in
a row, total timeout), 3 crash.

### Outputs

- `iterations.jsonl` and `iterations.csv`: one record per iteration, the
  plan's per-iteration record: result/stage/TS code, WiFi ms, fb and
  ballast, free and largest at release, tunnel start heap, DERP handshake
  heap, tunnel-up ms, TLS ms/suite, fetch ok/bytes/ms, `STACK_HWM` per task,
  stop timeout, deferred, post-teardown heap, reclaim ok/tries, survivors
  count/bytes/phases/dumps, spool readback, download and sync and resume
  fields, flags, panic text, coredump path. Records opened by the normal
  firmware (a tunnel
  window without SELFTEST lines, as in `fixtures/dev.log`) have
  `selftest=false` and close on the `STACK_HWM` line of the teardown.
- `summary.json`: counts, flag counts and rates, median/p95 of
  `tunnel_up_ms`, `fetch_ms`, `reclaim_tries`, `dl_kbps`, `hwm_min` per task,
  boot summaries, `restore_ok`, the rubric (one verdict per rule) and
  `baseline_candidate`.
- `boots.json`: per-boot context (reset reason, config, WiFi, BOOT_SUMMARY).
- `run.log`, `restore.log`: raw serial as streamed.

Flags (section 5): `unexpected_reset` (a boot while an iteration is open and
no expected reboot was announced), `crash` (Guru Meditation / abort / assert /
task watchdog), `stall_after_tls` (TLS ok, then `fetch=FAIL bytes=0` after
45 s or more), `tls_connect_fail` (tunnel up, fetch FAIL, no handshake ok),
`not_enough_heap` (`[TSN] Not enough heap` or `code=TS-E03`),
`deferred_stop`, `reclaim_fail_survivors` / `reclaim_fail_nosurvivors`,
`wifi_fail`, `dl_bad` (size/md5/content-length mismatch),
`fb_not_reclaimable`, `silence_timeout`, `iteration_timeout`. Reset reasons
outside `USB_UART_CHIP_RESET`, `USB_JTAG_CHIP_RESET`, `RTC_SW_CPU_RST`,
`SW_RESET` (and `POWERON_RESET` on the first boot only) fail
`crash_or_reset`.

Rubric verdicts are `PASS`, `WARN`, `FAIL`, `NO_BASELINE` (a rule that needs
a baseline, none given: never a failure), `NA` (no data for the rule). The
section 7 rubric assumes N >= 10; with fewer iterations the rate and gate
rules downgrade `FAIL` to `WARN` and a `sample_size` warning is added. Hard
rules (crash, reset, HWM under 512, spool mismatch, `not_enough_heap` on
cycle 1, restore) apply at any N.

## Offline: testing the parser

```bash
python3 scripts/hw/selftest_run.py --selftest
python3 scripts/hw/selftest_run.py --parse-only scripts/hw/fixtures/stv.log --scenario feed_baseline
python3 scripts/hw/selftest_run.py --parse-only <any run.log> [--scenario X] [--baseline Y] [--out DIR]
```

`--selftest` parses the fixtures and asserts values checked by hand against
the log text:

- `fixtures/stv.log` (self-test run, 2 boots, log ends mid second run): boot 1
  is `RESULT=FAIL` with `fb reclaim=FAIL` after 10 tries, `survivors=6`
  (104+104+36+108+24+48 = 424 bytes, old `first=abba1234` format), tunnel up
  6376 ms, TLS 608 ms, `fetch=OK bytes=5253`, spool readback 5253, HWM
  coord=4652 derp_tx=3520 net_io=3860 wg_mgr=3692; boot 2 is `INCOMPLETE`
  (not an unexpected reset).
- `fixtures/dev.log` (default firmware: boot, root feed browse, one 9 MB
  download): two tunnel windows, the second with `Downloaded 8992638 bytes`,
  no flags, restore evidence `Starting CrossPoint version 1.6.0-dev-develop-b7bceb56`
  and activities Boot, EpubReader, Home. The Mac's `dev.log` contains no
  `Not enough heap` refusal (its only `[ERR]` line is `Could not set DNS
  server`), so that case lives in the synthetic fixture.
- `fixtures/synthetic_newfmt.log` (hand-built from the firmware format
  strings, exercises the section 4 lines): `SELFTEST config`, `cycle=1/2`
  refused with `Not enough heap` + `RESULT=FAIL stage=ensureUp code=TS-E03`,
  `cycle=2/2` with an `[HTTP] Downloaded ... (content-length ...)` line, a
  `SELFTEST dl rep=` line, two new-format survivors (`U`/`T`, `user=`,
  `task=`), `BOOT_SUMMARY`, an expected reboot, then a Guru Meditation with
  MEPC/RA and a `RTC_SW_SYS_RST` reboot (crash record, exit code 3).

## Adding a scenario

1. Add a row to `SCENARIOS` in `selftest_run.py` (fb, ballast, cycles, mode;
   optional `wait_s`, `resume_mode`). The name becomes a `--scenario` choice.
2. If it needs its own rubric variant, extend `grade()`: gates go in
   `GATE_SCENARIOS`, scenario-specific evidence rules follow the
   `resume_*` / `sync_roundtrip` examples.
3. Put the matching `selftest.json` on the SD card (or build flags) so the
   device's `SELFTEST config` line agrees; the driver warns on mismatch.
4. New firmware log lines: add a regex to `RX` and a handler in
   `LogParser.feed`; add a line to `fixtures/synthetic_newfmt.log` and an
   assertion in `selftest()`.

## Adding a baseline

A baseline is the `baseline_candidate` block of a good run's `summary.json`
(`n`, `reclaim_fail_survivors_rate`, `hwm_min` per task, `dl_kbps_median`,
`first_fail`, `tunnel_up_ms_p95`, `fetch_ms_p95`). Either file shape is
accepted by `--baseline`:

```bash
cp scripts/hw/runs/<ts>_feed_baseline/summary.json scripts/hw/baselines/feed_baseline.json
python3 scripts/hw/selftest_run.py --scenario feed_baseline --baseline scripts/hw/baselines/feed_baseline.json
```

Rules that use it: `stack_hwm_vs_baseline` (warn if a task drops more than
512 B below), `reclaim_fail_rate` (baseline + 15 points), `download_throughput`
(under half of baseline fails), `first_fail_shift` (`feed_cycles*`: first
refused cycle must not move earlier than baseline - 1).

## Mac helpers

`mac/chunkflash.sh <image> [port]`: the chunked flash recipe that works on
this Mac, parameterised. 128 KB chunks via `dd`, each written with `esptool --before usb-reset --after
no-reset write-flash --flash-size keep <off>`, verified by `Hash of data
verified`, up to 5 tries, then `chip-id --after hard-reset`. Any sustained
esptool write over ~3 s drops the USB link on this Mac; do not collapse it
into one write.

`mac/capture.py <port> <log> [--send-file <path>]`: opens the port at 115200,
pulses RTS (`DTR=0, RTS=1, 0.2 s, RTS=0, DTR=1`) so the firmware boots with
the host attached, appends bytes to the log, reopens on any error. If
`--send-file` exists its bytes are written to the port and the file is
deleted (the driver's `Runner.send_serial("CMD:REBOOT")` uses this).
`SIGUSR1` re-pulses RTS.
