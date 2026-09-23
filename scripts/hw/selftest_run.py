#!/usr/bin/env python3
"""Headless tailnet self-test driver (docs/tailnet-offline-test-plan.md, section 5).

Runs on the Linux VM and drives the Mac (chattstudio) over ssh:

  preflight -> stage -> flash -> capture -> loop -> restore -> outputs

The serial log is streamed back with ``tail -F`` and parsed live by
``LogParser``; each self-test iteration becomes one JSONL/CSV record and the
run is graded against the section 7 rubric. Stdlib only.

Offline use (no hardware):

  python3 scripts/hw/selftest_run.py --parse-only scripts/hw/fixtures/stv.log
  python3 scripts/hw/selftest_run.py --selftest

Exit codes: 0 pass, 1 rubric regression, 2 infrastructure, 3 crash.
"""

import argparse
import csv
import hashlib
import json
import os
import queue
import re
import shlex
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

EXIT_PASS, EXIT_RUBRIC, EXIT_INFRA, EXIT_CRASH = 0, 1, 2, 3

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
MAC_HELPERS = ("chunkflash.sh", "capture.py")
REMOTE_DIR = "hw"  # relative to $HOME on the Mac (a quoted ~ path would not expand)
REMOTE_LOG = REMOTE_DIR + "/run.log"
REMOTE_SEND = REMOTE_DIR + "/send.txt"
REMOTE_PY = "~/fwvenv/bin/python"
COREDUMP_OFF, COREDUMP_LEN = 0xFF0000, 0x10000

ALLOWED_RESET = {"USB_UART_CHIP_RESET", "USB_JTAG_CHIP_RESET", "RTC_SW_CPU_RST", "SW_RESET"}

# Section 6 matrix; used to label records, sanity-check the SELFTEST config
# line and pick rubric variants. The device reads its own config from
# /.crosspoint/selftest.json (or build flags); the driver cannot set it.
SCENARIOS = {
    "feed_baseline": dict(fb=48000, ballast=36000, cycles=1, mode="feed"),
    "feed_nofb": dict(fb=0, ballast=84000, cycles=1, mode="feed"),
    "feed_loose": dict(fb=48000, ballast=60000, cycles=1, mode="feed"),
    "feed_tight": dict(fb=48000, ballast=28000, cycles=1, mode="feed"),
    "feed_cycles5": dict(fb=48000, ballast=36000, cycles=5, mode="feed"),
    "feed_cycles5_wait": dict(fb=48000, ballast=36000, cycles=5, mode="feed", wait_s=130),
    "download_9mb": dict(fb=48000, ballast=36000, cycles=1, mode="download"),
    "sync_roundtrip": dict(fb=48000, ballast=None, cycles=1, mode="sync"),
    "resume_spooled": dict(fb=48000, ballast=36000, cycles=1, mode="resume", resume_mode=1),
    "resume_pending": dict(fb=48000, ballast=36000, cycles=1, mode="resume", resume_mode=2),
    "owner_tracking": dict(fb=48000, ballast=36000, cycles=3, mode="feed"),
}
GATE_SCENARIOS = ("feed_baseline", "download_9mb", "sync_roundtrip")

SYNC_FORBIDDEN = (
    "Insufficient allocatable heap for TLS handshake",
    "Not enough heap for tailnet session",
    "Tailnet teardown deferred to reboot",
    "did not stop cleanly",
    "TS-E",
    "Guru Meditation",
    "abort()",
)

# ---------------------------------------------------------------------------
# Regex table (plan section 5 plus the SELFTEST lines from section 4)
# ---------------------------------------------------------------------------
RX = {
    "BOOT": re.compile(r"^ESP-ROM:esp32c3"),
    "RST": re.compile(r"^rst:0x[0-9a-f]+ \((\w+)\),boot:"),
    "READER_MS": re.compile(r"^\[\s*(\d+)\] \[(?:ERR|INF|DBG)\]"),
    "ML_MS": re.compile(r"^[EWIDV] \((\d+)\) \w+:"),
    "TSN_START": re.compile(r"\[TSN\] Starting tailnet session \(free heap (\d+)\)"),
    "TSN_CACHED": re.compile(r"\[TSN\] Using cached tailnet address (\S+)"),
    "TSN_UP": re.compile(r"\[TSN\] Tailnet up after (\d+) ms \(free heap (\d+)\)"),
    "TSN_TEARDOWN": re.compile(r"\[TSN\] Tearing down tailnet session"),
    # Warm start (cached netmap) and the one-line bring-up timing summary.
    "WARM_START": re.compile(r"\[TSN\] Warm start from cached netmap age_s=(\d+) self=(\S+) region=(\d+)"),
    "WARM_FAIL": re.compile(r"\[TSN\] Warm start failed \((.+?)\); cold start"),
    "COLD_START": re.compile(r"\[TSN\] Cold start \((.+?)\)"),
    "NETMAP_CACHED": re.compile(r"\[TSN\] Netmap cached self=(\S+) region=(\d+) peers=(\d+)"),
    "POISONED": re.compile(r"SELFTEST netmap cache poisoned"),
    "COORD_TIMING": re.compile(r"\[TIMING\] control ((?:\w+=\d+ ?)+)ms"),
    "NO_HEAP": re.compile(r"\[TSN\] Not enough heap for tailnet session \(free=(\d+), maxAlloc=(\d+)\)"),
    "DERP_HS": re.compile(r"ml_derp: DERP TLS heap before handshake: free=(\d+) largest=(\d+) relay=(\S+) region=(\d+)"),
    "TLS_OK": re.compile(r"\[SecureClient\] handshake ok \(auto\): (\S+) / (\S+) in (\d+) ms"),
    "TLS_ANY": re.compile(r"\[SecureClient\] (.*)$"),
    "FETCH": re.compile(r"SELFTEST fetch=(OK|FAIL) bytes=(\d+) after=(\d+)ms free=(\d+) largest=(\d+)"),
    "HWM": re.compile(
        r"STACK_HWM min-free/size bytes: coord=(\d+)/(\d+) derp_tx=(\d+)/(\d+) net_io=(\d+)/(\d+) wg_mgr=(\d+)/(\d+)"
    ),
    "STOP_TO": re.compile(r"microlink: Stop timed out: tasks still running"),
    "DEFERRED": re.compile(r"\[TSN\] Tailnet teardown deferred to reboot"),
    "RECL": re.compile(r"SELFTEST fb reclaim=(OK|FAIL) \((\d+) bytes\) free=(\d+) largest=(\d+)"),
    "RECL_TRY": re.compile(r"SELFTEST fb reclaim try=(\d+) (\w+) free=(\d+) largest=(\d+)"),
    "SURV_HDR": re.compile(
        r"SELFTEST survivors=(\d+) in region(?: (0x[0-9a-f]+)\.\.(0x[0-9a-f]+))?(?: \(blocks up=(\d+) fetch=(\d+)\))?"
    ),
    # Greedy text group: the |txt| dump may itself contain '|' (0x7c is printable).
    "SURV_NEW": re.compile(
        r"SELFTEST survivor #(\d+) ([UFT]) off=(\d+) size=(\d+)(?: user=(\d+))?(?: task=(\S+))? \|(.*)\| ([0-9a-f ]+)$"
    ),
    "SURV_OLD": re.compile(r"SELFTEST survivor #(\d+) off=(\d+) size=(\d+) first=([0-9a-f]+)"),
    "RESULT": re.compile(r"SELFTEST RESULT=(PASS|FAIL)(?:\s+(.*))?$"),
    "PANIC": re.compile(r"^Guru Meditation Error|^abort\(\) was called|^assert failed|Task watchdog got triggered"),
    "CONFIG": re.compile(r"SELFTEST config fb=(\d+) ballast=(\d+) cycles=(\d+) mode=(\w+) src=(\w+)"),
    "CYCLE": re.compile(r"SELFTEST cycle=(\d+)/(\d+) begin free=(\d+) largest=(\d+)"),
    "BOOT_SUMMARY": re.compile(
        r"SELFTEST BOOT_SUMMARY cycles=(\d+) pass=(\d+) fail=(\d+) first_fail=(-?\d+) deferred_stop=([01])"
    ),
    # content_length= is optional: section 4.2 lists it; the firmware may omit it.
    "DL": re.compile(
        r"SELFTEST dl rep=(\d+) result=(-?\d+) bytes=(\d+) expect=(\d+)(?: content_length=(\d+))? secs=(\d+) kbps=(\d+) "
        r"file_md5=(\w+) expect_md5=(\w+) size_ok=([01]) file_ok=([01]) minfree=(\d+) largest=(\d+)"
    ),
    "BEGIN": re.compile(r"SELFTEST begin target=(\S+)"),
    "WIFI_TRY": re.compile(r"SELFTEST wifi: trying (.+)$"),
    "WIFI_FAILED": re.compile(r"SELFTEST wifi: (.+) failed$"),
    "WIFI_OK": re.compile(r"SELFTEST wifi: connected (.+) ip=(\S+)"),
    "FB_MODEL": re.compile(r"SELFTEST framebuffer model=(\d+) \((\w+)\) free_now=(\d+)"),
    "BALLAST": re.compile(r"SELFTEST ballast reserve=(\d+) got=(\d+) free_now=(\d+) \((\w+)\)"),
    "OPDS_URL": re.compile(r"SELFTEST opds url=(\S+) free=(\d+)"),
    "SPOOL_OPEN": re.compile(r"SELFTEST spool open=(\w+) free=(\d+)"),
    "FB_REL": re.compile(r"SELFTEST fb released free=(\d+) largest=(\d+)"),
    "TUNNEL_URL": re.compile(r"SELFTEST tunnel_url=(\S+) free=(\d+) largest=(\d+)"),
    "POST_TD": re.compile(r"SELFTEST post-teardown free=(\d+) largest=(\d+)"),
    "SPOOL_RB": re.compile(r"SELFTEST spool readback=(\d+) free=(\d+) largest=(\d+)"),
    "DONE": re.compile(r"SELFTEST done; rebooting"),
    "RESUME_HOP": re.compile(r"SELFTEST resume hop=(\d+) mode=(\d+) idx=(\d+)"),
    "RESUME_SPOOL": re.compile(r"Resume from spool: mode=(\d+) path=(\S*) history=(\d+)"),
    "FEED_APPLIED": re.compile(r"Feed applied: entries=(\d+) truncated=(\d+)"),
    "RENDERED": re.compile(r"Rendered browse: rows=(\d+) selected=(\d+)"),
    "SILENT_RESTART": re.compile(r"Silent restart \(target=(\w+)"),
    "HTTP_FETCH": re.compile(r"\[HTTP\] Fetching: (\S+)"),
    "HTTP_DL_START": re.compile(r"\[HTTP\] Downloading: (\S+) -> (.+)$"),
    "HTTP_DL_DONE": re.compile(r"\[HTTP\] Downloaded (\d+) bytes(?: \(content-length (\d+)\) in (\d+) ms, (\d+) KB/s)?"),
    "SYNC_GET": re.compile(r"Get progress response: (-?\d+)"),
    "SYNC_PUT": re.compile(r"Update progress response: (-?\d+)"),
    "SYNC_REL": re.compile(r"tailnet window released free=(\d+) largest=(\d+)"),
    "START_VER": re.compile(r"Starting CrossPoint version (\S+)"),
    "ENTER_ACT": re.compile(r"Entering activity: (\w+)"),
    "TS_CODE": re.compile(r"\b(TS-E\d\d)\b"),
    "MEPC": re.compile(r"\bMEPC\s*:\s*(0x[0-9a-fA-F]+)"),
    "RA": re.compile(r"\bRA\s*:\s*(0x[0-9a-fA-F]+)"),
    "BACKTRACE": re.compile(r"^Backtrace:\s*(.*)$"),
    "FB_NOT_RECL": re.compile(r"Framebuffer not reclaimable"),
}
RESTORE_ACTIVITY = re.compile(r"Entering activity: (Home|Boot|EpubReader)\b")
STALL_MIN_MS = 45000  # "fetch FAIL with 0 bytes at ~60 s"
PANIC_TEXT_LINES = 60


def utcnow():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def extract_ms(line):
    m = RX["READER_MS"].match(line)
    if m:
        return int(m.group(1))
    m = RX["ML_MS"].match(line)
    if m:
        return int(m.group(1))
    return None


def kv_pairs(text):
    out = {}
    for k, v in re.findall(r"(\w+)=(\S+)", text or ""):
        out[k] = v.rstrip(";,")
    return out


def new_record():
    return dict(
        idx=None, boot=None, cycle=None, cycle_total=None, opened_by=None, selftest=False, kind=None,
        result=None, stage=None, ts_code=None, msg=None,
        wifi_ms=None, wifi_ssid=None, fb_bytes=None, fb_ok=None, ballast_reserve=None, ballast_got=None,
        free_with_fb=None, config_mode=None, config_src=None, config_cycles=None,
        free_at_release=None, largest_at_release=None,
        cached_address=False, tunnel_start_free=None, derp_hs_free=None, derp_hs_largest=None, derp_relay=None,
        tunnel_up_ms=None, tunnel_up_free=None, tunnel_up_count=0, tunnel_ups=[],
        warm_start=False, warm_age_s=None, warm_self=None, warm_region=None, warm_fail=None, cold_start=None,
        netmap_cached=False, poisoned=False, coord_timing={}, coord_timing_warm=None, coord_total_ms=None,
        coord_control_ms=None,
        tls_ms=None, tls_version=None, tls_suite=None, tls_err=None,
        fetch_ok=None, fetch_bytes=None, fetch_ms=None, fetch_free=None, fetch_largest=None, fetch_url=None,
        hwm={}, stop_timeout=False, deferred=False, post_free=None, post_largest=None,
        reclaim_ok=None, reclaim_tries=None, survivors=None, survivors_bytes=None, survivors_phases="",
        survivors_dumps=[], survivors_up=None, survivors_fetch=None, spool_readback=None,
        dl=[], dl_bytes=None, dl_content_length=None, dl_ms=None, dl_kbps=None, dl_path=None,
        dl_size_ok=None, dl_file_ok=None,
        sync_get=[], sync_put=[], sync_released_free=None,
        resume_hop=None, resume_mode=None, resume_spool=None, feed_applied=None, rendered=None,
        forbidden_hits=[], flags=[], panic_text=None, mepc=None, ra=None, backtrace=None, coredump_path=None,
        expected_reboot=False, boots_spanned=0,
        ms_open=None, ms_close=None, t_open=None, t_close=None, line_open=None, line_close=None,
    )


CSV_COLUMNS = [
    "idx", "boot", "cycle", "cycle_total", "opened_by", "selftest", "kind", "result", "stage", "ts_code",
    "wifi_ms", "fb_bytes", "ballast_got", "free_with_fb", "free_at_release", "largest_at_release",
    "cached_address", "tunnel_start_free", "derp_hs_free", "derp_hs_largest", "tunnel_up_ms", "tunnel_up_free",
    "tunnel_up_count", "warm_start", "warm_age_s", "warm_fail", "cold_start", "netmap_cached", "poisoned",
    "coord_total_ms", "coord_control_ms", "coord_timing",
    "tls_ms", "tls_version", "tls_suite", "fetch_ok", "fetch_bytes", "fetch_ms", "fetch_free",
    "fetch_largest", "hwm_coord", "hwm_derp_tx", "hwm_net_io", "hwm_wg_mgr", "stop_timeout", "deferred",
    "post_free", "post_largest", "reclaim_ok", "reclaim_tries", "survivors", "survivors_bytes", "survivors_phases",
    "survivors_dumps", "spool_readback", "dl_bytes", "dl_content_length", "dl_ms", "dl_kbps", "dl_size_ok",
    "dl_file_ok", "sync_get", "sync_put", "resume_hop", "resume_spool", "feed_applied", "rendered", "flags",
    "panic_text", "mepc", "ra", "coredump_path", "ms_open", "ms_close", "t_open", "t_close", "line_open",
    "line_close",
]


def add_flag(rec, flag):
    if flag not in rec["flags"]:
        rec["flags"].append(flag)


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------
class LogParser:
    """State machine keyed on boot boundaries. Unknown lines are ignored."""

    def __init__(self, scenario=None):
        self.scenario = scenario
        self.records = []
        self.boots = []
        self.cur = None
        self.boot = None
        self.events = []
        self.lines = 0
        self.results_seen = 0
        self.panic_left = 0
        self.restore = {"version": None, "activities": []}
        self.crashes = 0
        self._pending_cached = False
        # Warm/cold decision lines precede "Starting tailnet session"; outside
        # a self-test cycle they are held until that line opens the record.
        self._pending_start = {}

    # -- boot / iteration bookkeeping -------------------------------------
    def _new_boot(self):
        self.boot = dict(
            index=len(self.boots), reset_reason=None, reset_ok=None, begin_ms=None, target=None,
            wifi_tries=[], wifi_failed=[], wifi_ssid=None, wifi_ms=None, config=None, fb_bytes=None,
            fb_ok=None, free_with_fb=None, ballast_reserve=None, ballast_got=None, summary=None,
            expected_reboot=False, line=self.lines, t=utcnow(),
        )
        self.boots.append(self.boot)
        self.events.append(("boot", self.boot))

    def _ensure_boot(self):
        if self.boot is None:
            self._new_boot()

    def _open(self, ms, opened_by, selftest):
        self._ensure_boot()
        r = new_record()
        r["boot"] = self.boot["index"]
        r["opened_by"] = opened_by
        r["selftest"] = selftest
        r["ms_open"] = ms
        r["t_open"] = utcnow()
        r["line_open"] = self.lines
        b = self.boot
        r["wifi_ms"] = b["wifi_ms"]
        r["wifi_ssid"] = b["wifi_ssid"]
        r["fb_bytes"] = b["fb_bytes"]
        r["fb_ok"] = b["fb_ok"]
        r["free_with_fb"] = b["free_with_fb"]
        r["ballast_reserve"] = b["ballast_reserve"]
        r["ballast_got"] = b["ballast_got"]
        if b["config"]:
            r["config_mode"] = b["config"]["mode"]
            r["config_src"] = b["config"]["src"]
            r["config_cycles"] = b["config"]["cycles"]
            r["kind"] = b["config"]["mode"]
            if r["fb_bytes"] is None:
                r["fb_bytes"] = b["config"]["fb"]
        if selftest and r["cycle"] is None:
            r["cycle"], r["cycle_total"] = 1, (b["config"]["cycles"] if b["config"] else 1)
        self.cur = r
        self.events.append(("iter_open", r))
        return r

    def _ensure_iter(self, ms, opened_by="lazy", selftest=True):
        if self.cur is None:
            self._open(ms, opened_by, selftest)
        return self.cur

    def _close(self, ms, result=None):
        r = self.cur
        if r is None:
            return None
        if result is not None or r["result"] is None:
            r["result"] = result or ("OK" if not r["flags"] else "FLAGGED")
        r["ms_close"] = ms
        r["t_close"] = utcnow()
        r["line_close"] = self.lines
        r["idx"] = len(self.records)
        # Derived classification (plan section 5).
        if r["reclaim_ok"] is False:
            add_flag(r, "reclaim_fail_survivors" if (r["survivors"] or 0) > 0 else "reclaim_fail_nosurvivors")
        if r["tunnel_up_ms"] is not None and r["fetch_ok"] is False and r["tls_ms"] is None:
            add_flag(r, "tls_connect_fail")
        if r["survivors_dumps"]:
            r["survivors_bytes"] = sum(d["size"] for d in r["survivors_dumps"])
            r["survivors_phases"] = "".join(d.get("phase") or "?" for d in r["survivors_dumps"])
        if r["ts_code"] == "TS-E03":
            add_flag(r, "not_enough_heap")
        if r["warm_fail"]:
            add_flag(r, "warm_fail")
        if r["selftest"] and r["fetch_ok"] is not None:
            r["kind"] = r["kind"] or ("download" if r["dl"] else "feed")
        self.records.append(r)
        self.cur = None
        self.events.append(("iter_close", r))
        return r

    def finalize(self):
        if self.cur is not None:
            self._close(None, "CRASH" if "crash" in self.cur["flags"] else "INCOMPLETE")

    # -- main entry --------------------------------------------------------
    def feed(self, raw):
        line = raw.rstrip("\r\n")
        self.lines += 1
        ms = extract_ms(line)
        cur = self.cur
        boot = self.boot

        if self.panic_left > 0 and cur is not None and not RX["BOOT"].match(line):
            self.panic_left -= 1
            if len(cur["panic_text"] or "") < 4096:
                cur["panic_text"] = (cur["panic_text"] or "") + line + "\n"
            m = RX["MEPC"].search(line)
            if m:
                cur["mepc"] = m.group(1)
            m = RX["RA"].search(line)
            if m and cur["ra"] is None:
                cur["ra"] = m.group(1)
            m = RX["BACKTRACE"].match(line)
            if m:
                cur["backtrace"] = m.group(1)

        if RX["BOOT"].match(line):
            if cur is not None:
                if "crash" in cur["flags"]:
                    self._close(ms, "CRASH")
                elif cur["expected_reboot"] or (boot and boot["expected_reboot"]):
                    cur["boots_spanned"] += 1
                    cur["expected_reboot"] = False
                else:
                    add_flag(cur, "unexpected_reset")
                    self._close(ms, "RESET")
            self.panic_left = 0
            self._new_boot()
            return

        m = RX["RST"].match(line)
        if m:
            self._ensure_boot()
            reason = m.group(1)
            self.boot["reset_reason"] = reason
            ok = reason in ALLOWED_RESET or (self.boot["index"] == 0 and reason == "POWERON_RESET")
            self.boot["reset_ok"] = ok
            if not ok:
                self.events.append(("bad_reset", self.boot))
            return

        if RX["PANIC"].search(line):
            r = self._ensure_iter(ms, "panic", selftest=cur["selftest"] if cur else False)
            add_flag(r, "crash")
            r["panic_text"] = (r["panic_text"] or "") + line + "\n"
            self.panic_left = PANIC_TEXT_LINES
            self.crashes += 1
            self.events.append(("crash", r))
            return

        # Restore evidence (default image) and sync forbidden-string tracking.
        m = RX["START_VER"].search(line)
        if m:
            self.restore["version"] = m.group(1)
            self.events.append(("restore_version", m.group(1)))
        m = RX["ENTER_ACT"].search(line)
        if m:
            self.restore["activities"].append(m.group(1))
            self.events.append(("restore_activity", m.group(1)))
        if cur is not None:
            for s in SYNC_FORBIDDEN:
                if s in line:
                    cur["forbidden_hits"].append(line[:160])
                    break

        # --- boot-level SELFTEST context -----------------------------------
        m = RX["BEGIN"].search(line)
        if m:
            self._ensure_boot()
            if cur is not None:
                self._close(ms, "INCOMPLETE")
            self.boot["begin_ms"] = ms
            self.boot["target"] = m.group(1)
            return
        m = RX["CONFIG"].search(line)
        if m:
            self._ensure_boot()
            self.boot["config"] = dict(fb=int(m.group(1)), ballast=int(m.group(2)), cycles=int(m.group(3)),
                                       mode=m.group(4), src=m.group(5))
            self.events.append(("config", self.boot["config"]))
            return
        m = RX["WIFI_TRY"].search(line)
        if m:
            self._ensure_boot()
            self.boot["wifi_tries"].append(m.group(1))
            if self.boot["begin_ms"] is None:
                self.boot["begin_ms"] = ms
            return
        m = RX["WIFI_FAILED"].search(line)
        if m:
            self._ensure_boot()
            self.boot["wifi_failed"].append(m.group(1))
            return
        m = RX["WIFI_OK"].search(line)
        if m:
            self._ensure_boot()
            self.boot["wifi_ssid"] = m.group(1)
            if ms is not None and self.boot["begin_ms"] is not None:
                self.boot["wifi_ms"] = ms - self.boot["begin_ms"]
            return
        m = RX["FB_MODEL"].search(line)
        if m:
            self._ensure_boot()
            self.boot["fb_bytes"] = int(m.group(1))
            self.boot["fb_ok"] = m.group(2) == "ok"
            return
        m = RX["BALLAST"].search(line)
        if m:
            self._ensure_boot()
            self.boot["ballast_reserve"] = int(m.group(1))
            self.boot["ballast_got"] = int(m.group(2))
            self.boot["free_with_fb"] = int(m.group(3))
            if cur is not None:
                cur["ballast_reserve"], cur["ballast_got"], cur["free_with_fb"] = (
                    self.boot["ballast_reserve"], self.boot["ballast_got"], self.boot["free_with_fb"])
            return
        m = RX["CYCLE"].search(line)
        if m:
            if cur is not None:
                self._close(ms, "INCOMPLETE")
            r = self._open(ms, "cycle", True)
            r["cycle"], r["cycle_total"] = int(m.group(1)), int(m.group(2))
            r["free_with_fb"] = int(m.group(3))
            return
        m = RX["BOOT_SUMMARY"].search(line)
        if m:
            self._ensure_boot()
            self.boot["summary"] = dict(cycles=int(m.group(1)), passed=int(m.group(2)), failed=int(m.group(3)),
                                        first_fail=int(m.group(4)), deferred_stop=m.group(5) == "1")
            if cur is not None:
                if self.boot["summary"]["deferred_stop"]:
                    cur["deferred"] = True
                    add_flag(cur, "deferred_stop")
                self._close(ms, "INCOMPLETE")
            elif self.boot["summary"]["deferred_stop"] and self.records:
                last = self.records[-1]
                if last["boot"] == self.boot["index"] and not last["deferred"]:
                    last["deferred"] = True
                    add_flag(last, "deferred_stop")
            self.events.append(("boot_summary", self.boot["summary"]))
            return
        if RX["DONE"].search(line):
            self._ensure_boot()
            self.boot["expected_reboot"] = True
            if cur is not None:
                self._close(ms, "INCOMPLETE")
            return

        # --- verdict -----------------------------------------------------------
        m = RX["RESULT"].search(line)
        if m:
            r = self._ensure_iter(ms, "result")
            kv = kv_pairs(m.group(2))
            r["stage"] = kv.get("stage")
            r["ts_code"] = kv.get("code") or r["ts_code"]
            if m.group(2):
                mm = re.search(r"msg=(.*)$", m.group(2))
                if mm:
                    r["msg"] = mm.group(1)
            if r["stage"] == "wifi":
                add_flag(r, "wifi_fail")
                self.boot["expected_reboot"] = True
                self.events.append(("wifi_fail", r))
            self.results_seen += 1
            self.events.append(("result", r))
            self._close(ms, m.group(1))
            return

        # --- per-iteration SELFTEST lines ---------------------------------------
        for key in ("OPDS_URL", "SPOOL_OPEN"):
            if RX[key].search(line):
                self._ensure_iter(ms)
                return
        m = RX["FB_REL"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["free_at_release"], r["largest_at_release"] = int(m.group(1)), int(m.group(2))
            return
        m = RX["TUNNEL_URL"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["fetch_url"] = m.group(1)
            return
        m = RX["FETCH"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["fetch_ok"] = m.group(1) == "OK"
            r["fetch_bytes"], r["fetch_ms"] = int(m.group(2)), int(m.group(3))
            r["fetch_free"], r["fetch_largest"] = int(m.group(4)), int(m.group(5))
            if not r["fetch_ok"] and r["fetch_bytes"] == 0 and r["fetch_ms"] >= STALL_MIN_MS and r["tls_ms"] is not None:
                add_flag(r, "stall_after_tls")
            return
        m = RX["POST_TD"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["post_free"], r["post_largest"] = int(m.group(1)), int(m.group(2))
            return
        m = RX["RECL_TRY"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["reclaim_tries"] = int(m.group(1)) + 1
            return
        m = RX["RECL"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["reclaim_ok"] = m.group(1) == "OK"
            if r["reclaim_tries"] is None:
                r["reclaim_tries"] = 1
            if r["fb_bytes"] is None:
                r["fb_bytes"] = int(m.group(2))
            r["post_free"], r["post_largest"] = int(m.group(3)), int(m.group(4))
            return
        m = RX["SURV_HDR"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["survivors"] = int(m.group(1))
            if m.group(4):
                r["survivors_up"], r["survivors_fetch"] = int(m.group(4)), int(m.group(5))
            return
        m = RX["SURV_NEW"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["survivors_dumps"].append(dict(
                n=int(m.group(1)), phase=m.group(2), off=int(m.group(3)), size=int(m.group(4)),
                user=int(m.group(5)) if m.group(5) else None, task=m.group(6), txt=m.group(7),
                hex=m.group(8).strip()))
            return
        m = RX["SURV_OLD"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["survivors_dumps"].append(dict(n=int(m.group(1)), phase=None, off=int(m.group(2)),
                                             size=int(m.group(3)), user=None, task=None, txt=None,
                                             hex=m.group(4)))
            return
        m = RX["SPOOL_RB"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["spool_readback"] = int(m.group(1))
            return
        m = RX["DL"].search(line)
        if m:
            r = self._ensure_iter(ms)
            d = dict(rep=int(m.group(1)), result=int(m.group(2)), bytes=int(m.group(3)), expect=int(m.group(4)),
                     content_length=int(m.group(5)) if m.group(5) else None, secs=int(m.group(6)),
                     kbps=int(m.group(7)), file_md5=m.group(8), expect_md5=m.group(9),
                     size_ok=m.group(10) == "1", file_ok=m.group(11) == "1", minfree=int(m.group(12)),
                     largest=int(m.group(13)))
            r["dl"].append(d)
            r["kind"] = "download"
            r["dl_bytes"], r["dl_ms"], r["dl_kbps"] = d["bytes"], d["secs"] * 1000, d["kbps"]
            r["dl_content_length"] = d["content_length"] or r["dl_content_length"]
            r["dl_size_ok"] = all(x["size_ok"] for x in r["dl"])
            r["dl_file_ok"] = all(x["file_ok"] for x in r["dl"])
            if not (d["size_ok"] and d["file_ok"] and d["result"] == 0):
                add_flag(r, "dl_bad")
            return
        m = RX["RESUME_HOP"].search(line)
        if m:
            r = self._ensure_iter(ms)
            r["kind"] = "resume"
            r["resume_hop"], r["resume_mode"] = int(m.group(1)), int(m.group(2))
            r["expected_reboot"] = True
            return
        m = RX["RESUME_SPOOL"].search(line)
        if m:
            r = self._ensure_iter(ms, "resume")
            r["kind"] = "resume"
            r["resume_spool"] = dict(mode=int(m.group(1)), path=m.group(2), history=int(m.group(3)))
            return
        m = RX["FEED_APPLIED"].search(line)
        if m:
            r = self._ensure_iter(ms, "resume")
            r["feed_applied"] = dict(entries=int(m.group(1)), truncated=int(m.group(2)))
            return
        m = RX["RENDERED"].search(line)
        if m:
            r = self._ensure_iter(ms, "resume")
            r["rendered"] = dict(rows=int(m.group(1)), selected=int(m.group(2)))
            if r["kind"] == "resume":
                self.results_seen += 1
                self.events.append(("result", r))
                self._close(ms, "PASS" if r["resume_spool"] and r["feed_applied"] else "FAIL")
            return
        m = RX["SILENT_RESTART"].search(line)
        if m:
            if cur is not None:
                cur["expected_reboot"] = True
            elif boot is not None:
                boot["expected_reboot"] = True
            return

        # --- tunnel / TLS / fetch lines (self-test and normal firmware) ----------
        m = RX["NO_HEAP"].search(line)
        if m:
            if cur is None:
                r = self._open(ms, "no_heap", False)
                add_flag(r, "not_enough_heap")
                r["ts_code"] = "TS-E03"
                r["tunnel_start_free"] = int(m.group(1))
                self.events.append(("no_heap", r))
                self._close(ms, "FLAGGED")
            else:
                add_flag(cur, "not_enough_heap")
                cur["ts_code"] = cur["ts_code"] or "TS-E03"
                self.events.append(("no_heap", cur))
            return
        m = RX["TSN_CACHED"].search(line)
        if m:
            if cur is not None:
                cur["cached_address"] = True
            else:
                self._pending_cached = True
            return
        m = RX["WARM_START"].search(line)
        if m:
            fields = dict(warm_start=True, warm_age_s=int(m.group(1)), warm_self=m.group(2), warm_region=int(m.group(3)))
            (cur if cur is not None else self._pending_start).update(fields)
            return
        m = RX["COLD_START"].search(line)
        if m:
            (cur if cur is not None else self._pending_start).update(cold_start=m.group(1))
            return
        m = RX["WARM_FAIL"].search(line)
        if m:
            # The cold retry re-logs "Starting tailnet session" inside the same
            # record; keep the first tunnel_start_free rather than reopening.
            (cur if cur is not None else self._pending_start).update(warm_fail=m.group(1))
            return
        m = RX["NETMAP_CACHED"].search(line)
        if m and cur is not None:
            cur["netmap_cached"] = True
            return
        if RX["POISONED"].search(line):
            if cur is not None:
                cur["poisoned"] = True
            else:
                self._pending_start["poisoned"] = True
            return
        m = RX["COORD_TIMING"].search(line)
        if m and cur is not None:
            timing = {k: int(v) for k, v in kv_pairs(m.group(1)).items()}
            # A warm attempt that later falls back leaves two lines; keep the
            # warm one aside so the cold one does not erase it.
            if timing.get("warm") == 1:
                cur["coord_timing_warm"] = timing
            cur["coord_timing"] = timing
            cur["coord_total_ms"] = timing.get("total")
            cur["coord_control_ms"] = sum(timing.get(k, 0) for k in ("dns", "tcp", "noise", "h2", "reg", "map"))
            return
        m = RX["TSN_START"].search(line)
        if m:
            if cur is not None and not cur["selftest"] and cur["warm_fail"] is None:
                self._close(ms, "INCOMPLETE")
            r = self._ensure_iter(ms, "tsn", selftest=False)
            if getattr(self, "_pending_cached", False):
                r["cached_address"] = True
                self._pending_cached = False
            if self._pending_start:
                r.update(self._pending_start)
                self._pending_start = {}
            if r["tunnel_start_free"] is None:
                r["tunnel_start_free"] = int(m.group(1))
            return
        m = RX["DERP_HS"].search(line)
        if m and cur is not None:
            if cur["derp_hs_free"] is None:
                cur["derp_hs_free"], cur["derp_hs_largest"], cur["derp_relay"] = int(m.group(1)), int(m.group(2)), m.group(3)
            return
        m = RX["TSN_UP"].search(line)
        if m:
            r = self._ensure_iter(ms, "tsn", selftest=False)
            r["tunnel_ups"].append(int(m.group(1)))
            r["tunnel_up_count"] = len(r["tunnel_ups"])
            if r["tunnel_up_ms"] is None:
                r["tunnel_up_ms"], r["tunnel_up_free"] = int(m.group(1)), int(m.group(2))
            return
        m = RX["TLS_OK"].search(line)
        if m:
            if cur is not None and cur["tls_ms"] is None:
                cur["tls_version"], cur["tls_suite"], cur["tls_ms"] = m.group(1), m.group(2), int(m.group(3))
            return
        m = RX["TLS_ANY"].search(line)
        if m and cur is not None and "handshake ok" not in line:
            cur["tls_err"] = m.group(1)[:160]
            return
        m = RX["HTTP_FETCH"].search(line)
        if m and cur is not None:
            cur["fetch_url"] = cur["fetch_url"] or m.group(1)
            cur["kind"] = cur["kind"] or "feed"
            return
        m = RX["HTTP_DL_START"].search(line)
        if m and cur is not None:
            cur["kind"] = "download"
            cur["dl_path"] = m.group(2)
            cur["fetch_url"] = cur["fetch_url"] or m.group(1)
            return
        m = RX["HTTP_DL_DONE"].search(line)
        if m and cur is not None:
            cur["kind"] = "download"
            cur["dl_bytes"] = int(m.group(1))
            if m.group(2):
                cur["dl_content_length"], cur["dl_ms"], cur["dl_kbps"] = int(m.group(2)), int(m.group(3)), int(m.group(4))
                if cur["dl_content_length"] != cur["dl_bytes"]:
                    add_flag(cur, "dl_bad")
            return
        if RX["STOP_TO"].search(line):
            if cur is not None:
                cur["stop_timeout"] = True
            return
        if RX["DEFERRED"].search(line):
            if cur is not None:
                cur["deferred"] = True
                add_flag(cur, "deferred_stop")
            return
        m = RX["HWM"].search(line)
        if m:
            if cur is not None:
                cur["hwm"] = {
                    "coord": [int(m.group(1)), int(m.group(2))],
                    "derp_tx": [int(m.group(3)), int(m.group(4))],
                    "net_io": [int(m.group(5)), int(m.group(6))],
                    "wg_mgr": [int(m.group(7)), int(m.group(8))],
                }
                if not cur["selftest"]:
                    self._close(ms)
            return
        m = RX["SYNC_GET"].search(line)
        if m and cur is not None:
            cur["sync_get"].append(int(m.group(1)))
            cur["kind"] = cur["kind"] or "sync"
            return
        m = RX["SYNC_PUT"].search(line)
        if m and cur is not None:
            cur["sync_put"].append(int(m.group(1)))
            cur["kind"] = cur["kind"] or "sync"
            return
        m = RX["SYNC_REL"].search(line)
        if m and cur is not None:
            cur["sync_released_free"] = int(m.group(1))
            return
        if RX["FB_NOT_RECL"].search(line) and cur is not None:
            add_flag(cur, "fb_not_reclaimable")
            return
        m = RX["TS_CODE"].search(line)
        if m and cur is not None and cur["ts_code"] is None:
            cur["ts_code"] = m.group(1)


# ---------------------------------------------------------------------------
# Statistics, rubric, outputs
# ---------------------------------------------------------------------------
def pct(values, p):
    vals = sorted(v for v in values if v is not None)
    if not vals:
        return None
    k = max(0, int(-(-p * len(vals) // 100)) - 1)  # ceil(p/100*n) - 1
    return vals[min(k, len(vals) - 1)]


def med(values):
    vals = [v for v in values if v is not None]
    return statistics.median(vals) if vals else None


def rate(records, flag):
    if not records:
        return None
    return sum(1 for r in records if flag in r["flags"]) / len(records)


def load_baseline(path):
    if not path:
        return None
    with open(path) as f:
        data = json.load(f)
    return data.get("baseline_candidate", data)


def hwm_min(records):
    out = {}
    for r in records:
        for task, (mn, size) in (r.get("hwm") or {}).items():
            if task not in out or mn < out[task]["min"]:
                out[task] = {"min": mn, "size": size}
    return out


def grade(records, boots, scenario, baseline, restore_ok, n_required=10):
    """Section 7 rubric. Returns (rules, overall)."""
    rules = []
    st = [r for r in records if r["selftest"] and r["result"] != "INCOMPLETE"]
    n = len(st)
    sc = SCENARIOS.get(scenario, {}) if scenario else {}
    mode = sc.get("mode")
    small_n = n < n_required

    def add(rule, verdict, detail):
        rules.append({"rule": rule, "verdict": verdict, "detail": detail})

    def rate_verdict(bad):
        return "FAIL" if (bad and not small_n) else ("WARN" if bad else "PASS")

    # Always a regression.
    crashes = [r["idx"] for r in records if "crash" in r["flags"]]
    resets = [r["idx"] for r in records if "unexpected_reset" in r["flags"]]
    bad_boots = [b["index"] for b in boots if b["reset_ok"] is False]
    if crashes or resets or bad_boots:
        add("crash_or_reset", "FAIL",
            "crash in %s; unexpected_reset in %s; bad reset reason on boots %s" % (crashes, resets,
                [(b["index"], b["reset_reason"]) for b in boots if b["reset_ok"] is False]))
    else:
        add("crash_or_reset", "PASS", "no crash, no unexpected reset, reset reasons allowed")

    hm = hwm_min(records)
    if hm:
        worst = min(v["min"] for v in hm.values())
        detail = " ".join("%s=%d/%d" % (t, v["min"], v["size"]) for t, v in sorted(hm.items()))
        if worst < 512:
            add("stack_hwm_min", "FAIL", "min-free under 512 B: " + detail)
        elif worst < 1024:
            add("stack_hwm_min", "WARN", "min-free under 1024 B: " + detail)
        else:
            add("stack_hwm_min", "PASS", detail)
        if baseline and baseline.get("hwm_min"):
            drops = {t: baseline["hwm_min"][t]["min"] - v["min"] for t, v in hm.items() if t in baseline["hwm_min"]}
            bad = {t: d for t, d in drops.items() if d > 512}
            add("stack_hwm_vs_baseline", "WARN" if bad else "PASS",
                "drop vs baseline > 512 B: %s" % bad if bad else "drops vs baseline: %s" % drops)
        else:
            add("stack_hwm_vs_baseline", "NO_BASELINE", "no baseline hwm_min")
    else:
        add("stack_hwm_min", "NA", "no STACK_HWM lines")

    mism = [r["idx"] for r in records if r["spool_readback"] is not None and r["fetch_bytes"] is not None
            and r["spool_readback"] != r["fetch_bytes"]]
    add("spool_readback", "FAIL" if mism else ("PASS" if any(r["spool_readback"] is not None for r in records) else "NA"),
        "readback != fetch bytes in %s" % mism if mism else "readback equals fetch bytes")

    c1 = [r for r in st if (r["cycle"] or 1) == 1]
    c1_noheap = [r["idx"] for r in c1 if "not_enough_heap" in r["flags"]]
    if scenario == "feed_tight":
        add("not_enough_heap_cycle1", "PASS" if c1_noheap and len(c1_noheap) == len(c1) else "FAIL",
            "feed_tight expects TS-E03 on every cycle 1: %d/%d" % (len(c1_noheap), len(c1)))
    else:
        add("not_enough_heap_cycle1", "FAIL" if c1_noheap else "PASS",
            "not_enough_heap on cycle 1 of a fresh boot in %s" % c1_noheap if c1_noheap else "none on cycle 1")

    up_cached = [r["tunnel_up_ms"] for r in records if r["cached_address"] and r["tunnel_up_ms"] is not None]
    p95_up = pct(up_cached, 95)
    if p95_up is None:
        add("tunnel_up_p95", "NA", "no tunnel-up with cached address")
    else:
        add("tunnel_up_p95", "FAIL" if p95_up > 30000 else "PASS", "p95=%d ms (limit 30000, n=%d)" % (p95_up, len(up_cached)))

    if mode in (None, "feed"):
        fm = [r["fetch_ms"] for r in st if r["fetch_ms"] is not None and r["fetch_ok"]]
        p95_f = pct(fm, 95)
        if p95_f is None:
            add("fetch_p95", "NA", "no successful self-test fetches")
        else:
            add("fetch_p95", "FAIL" if p95_f > 15000 else "PASS", "p95=%d ms (limit 15000, n=%d)" % (p95_f, len(fm)))

    kbps = [r["dl_kbps"] for r in records if r["dl_kbps"] is not None]
    if kbps:
        m_k = med(kbps)
        if baseline and baseline.get("dl_kbps_median"):
            add("download_throughput", "FAIL" if m_k < 0.5 * baseline["dl_kbps_median"] else "PASS",
                "median %s KB/s vs baseline %s" % (m_k, baseline["dl_kbps_median"]))
        else:
            add("download_throughput", "NO_BASELINE", "median %s KB/s" % m_k)
        bad = [r["idx"] for r in records if "dl_bad" in r["flags"]]
        add("download_integrity", "FAIL" if bad else "PASS",
            "size/md5/content-length mismatch in %s" % bad if bad else "all downloads byte-exact")
    elif mode == "download":
        add("download_integrity", "FAIL", "download scenario produced no dl records")

    if restore_ok is None:
        add("restore_ok", "NA", "restore not run")
    else:
        add("restore_ok", "PASS" if restore_ok else "FAIL", "restore_ok=%s" % restore_ok)

    # Flaky but acceptable, with ceilings.
    stalls = [i for i, r in enumerate(st) if "stall_after_tls" in r["flags"]]
    r_st = rate(st, "stall_after_tls")
    twice = any(b == a + 1 for a, b in zip(stalls, stalls[1:]))
    next_bad = any(i + 1 < len(st) and st[i + 1]["result"] != "PASS" for i in stalls)
    bad = bool(stalls) and ((r_st or 0) > 0.10 or twice or next_bad)
    add("stall_after_tls", rate_verdict(bad) if n else "NA",
        "rate=%s twice_in_a_row=%s next_not_pass=%s" % (r_st, twice, next_bad))

    defs = [i for i, r in enumerate(st) if "deferred_stop" in r["flags"]]
    r_d = rate(st, "deferred_stop")
    not_after_stall = [i for i in defs if not (i > 0 and "stall_after_tls" in st[i - 1]["flags"])]
    bad = bool(defs) and ((r_d or 0) > 0.05 or bool(not_after_stall))
    add("deferred_stop", rate_verdict(bad) if n else "NA",
        "rate=%s not_following_a_stall=%s" % (r_d, not_after_stall))

    rf = [r for r in st if "reclaim_fail_survivors" in r["flags"]]
    r_rf = rate(st, "reclaim_fail_survivors")
    ceil_bad = [r["idx"] for r in rf if (r["survivors"] or 0) > 8 or (r["survivors_bytes"] or 0) > 768
                or "F" in (r["survivors_phases"] or "")]
    if ceil_bad:
        add("reclaim_fail_ceilings", "FAIL", "survivors>8, >768 B gross or F-phase survivor in %s" % ceil_bad)
    else:
        add("reclaim_fail_ceilings", "PASS" if n else "NA", "%d reclaim failures with survivors within ceilings" % len(rf))
    if baseline and baseline.get("reclaim_fail_survivors_rate") is not None:
        bad = (r_rf or 0) > baseline["reclaim_fail_survivors_rate"] + 0.15
        add("reclaim_fail_rate", rate_verdict(bad) if n else "NA",
            "rate=%s baseline=%s (+0.15 allowed)" % (r_rf, baseline["reclaim_fail_survivors_rate"]))
    else:
        add("reclaim_fail_rate", "NO_BASELINE", "rate=%s" % r_rf)

    if scenario and scenario.startswith("feed_cycles"):
        ff = [b["summary"]["first_fail"] for b in boots if b["summary"] and b["summary"]["first_fail"] > 0]
        now = min(ff) if ff else None
        if baseline and baseline.get("first_fail") is not None:
            base = baseline["first_fail"]
            bad = now is not None and (base < 0 or now < base - 1)
            add("first_fail_shift", "FAIL" if bad else "PASS", "first_fail=%s baseline=%s" % (now, base))
        else:
            add("first_fail_shift", "NO_BASELINE", "first_fail=%s" % now)

    # Gates.
    passed = sum(1 for r in st if r["result"] == "PASS")
    if scenario in GATE_SCENARIOS:
        pr = passed / n if n else 0
        add("gate_pass_rate", rate_verdict(pr < 0.80) if n else "FAIL", "PASS %d/%d = %.0f%% (need 80%%)" % (passed, n, pr * 100))
    elif n:
        add("gate_pass_rate", "NA", "PASS %d/%d (gate applies to %s)" % (passed, n, ", ".join(GATE_SCENARIOS)))
    if scenario and scenario.startswith("resume"):
        res = [r for r in records if r["kind"] == "resume"]
        bad = [r["idx"] for r in res if not (r["resume_spool"] and r["feed_applied"] and r["rendered"])]
        add("resume_evidence", "FAIL" if (bad or not res) else "PASS",
            "hop-2 evidence missing in %s" % bad if bad else "%d resume iterations with full evidence" % len(res))
    if scenario == "sync_roundtrip":
        bad = []
        for r in st:
            ok = (r["tunnel_up_count"] >= 2 and any(200 <= c < 300 for c in r["sync_get"])
                  and any(200 <= c < 300 for c in r["sync_put"]) and not r["forbidden_hits"])
            if not ok:
                bad.append(r["idx"])
        add("sync_evidence", "FAIL" if (bad or not st) else "PASS",
            "missing both windows / 2xx get+put / forbidden text in %s" % bad if bad else "all iterations complete")

    verdicts = {r["verdict"] for r in rules}
    overall = "FAIL" if "FAIL" in verdicts else ("WARN" if "WARN" in verdicts else "PASS")
    if small_n and n:
        rules.append({"rule": "sample_size", "verdict": "WARN", "detail": "n=%d < %d; rate rules downgraded to WARN" % (n, n_required)})
        if overall == "PASS":
            overall = "WARN"
    return rules, overall


def build_summary(parser, scenario, baseline, restore_ok, extra=None):
    recs = parser.records
    st = [r for r in recs if r["selftest"] and r["result"] != "INCOMPLETE"]
    rules, overall = grade(recs, parser.boots, scenario, baseline, restore_ok)
    hm = hwm_min(recs)
    counts = {}
    for r in recs:
        counts[r["result"]] = counts.get(r["result"], 0) + 1
    flag_rates = {}
    for f in ("unexpected_reset", "crash", "stall_after_tls", "tls_connect_fail", "deferred_stop", "not_enough_heap",
              "reclaim_fail_survivors", "reclaim_fail_nosurvivors", "wifi_fail", "dl_bad", "fb_not_reclaimable",
              "warm_fail"):
        flag_rates[f] = {"count": sum(1 for r in recs if f in r["flags"]), "rate": rate(st, f) if st else rate(recs, f)}
    up = [r["tunnel_up_ms"] for r in recs]
    fm = [r["fetch_ms"] for r in recs]
    rt = [r["reclaim_tries"] for r in recs]
    kb = [r["dl_kbps"] for r in recs]
    first_fails = [b["summary"]["first_fail"] for b in parser.boots if b["summary"]]
    warm = [r for r in recs if r["warm_start"]]
    warm_ok = [r for r in warm if r["warm_fail"] is None]
    coord_keys = sorted({k for r in recs for k in (r.get("coord_timing") or {})})
    coord_timing = {}
    for k in coord_keys:
        vals = [(r.get("coord_timing") or {}).get(k) for r in recs]
        coord_timing[k] = {"median": med(vals), "p95": pct(vals, 95), "n": sum(1 for v in vals if v is not None)}
    up_warm = [r["tunnel_up_ms"] for r in warm_ok]
    # After a warm failure the first "Tailnet up" is the discarded warm
    # session; the cold bring-up that served the page is the last one.
    up_cold = [r["tunnel_ups"][-1] if (r["warm_fail"] and len(r["tunnel_ups"]) > 1) else r["tunnel_up_ms"]
               for r in recs if not r["warm_start"] or r["warm_fail"] is not None]
    summary = {
        "scenario": scenario,
        "generated": utcnow(),
        "lines": parser.lines,
        "boots": len(parser.boots),
        "reset_reasons": [b["reset_reason"] for b in parser.boots],
        "records": len(recs),
        "selftest_records": len(st),
        "results_seen": parser.results_seen,
        "counts": counts,
        "flag_rates": flag_rates,
        "tunnel_up_ms": {"median": med(up), "p95": pct(up, 95), "n": sum(1 for v in up if v is not None)},
        "tunnel_up_ms_warm": {"median": med(up_warm), "p95": pct(up_warm, 95),
                              "n": sum(1 for v in up_warm if v is not None)},
        "tunnel_up_ms_cold": {"median": med(up_cold), "p95": pct(up_cold, 95),
                              "n": sum(1 for v in up_cold if v is not None)},
        "warm_starts": {"attempted": len(warm), "succeeded": len(warm_ok), "failed": len(warm) - len(warm_ok),
                        "fail_reasons": sorted({r["warm_fail"] for r in warm if r["warm_fail"]}),
                        "cold_reasons": sorted({r["cold_start"] for r in recs if r["cold_start"]}),
                        "netmap_cached": sum(1 for r in recs if r["netmap_cached"]),
                        "poisoned": sum(1 for r in recs if r["poisoned"])},
        "coord_timing_ms": coord_timing,
        "fetch_ms": {"median": med(fm), "p95": pct(fm, 95), "n": sum(1 for v in fm if v is not None)},
        "reclaim_tries": {"median": med(rt), "p95": pct(rt, 95), "n": sum(1 for v in rt if v is not None)},
        "dl_kbps": {"median": med(kb), "p95": pct(kb, 95), "n": sum(1 for v in kb if v is not None)},
        "hwm_min": hm,
        "boot_summaries": [b["summary"] for b in parser.boots if b["summary"]],
        "restore_ok": restore_ok,
        "restore_evidence": parser.restore,
        "rubric": rules,
        "overall": overall,
        "baseline_used": baseline is not None,
        "baseline_candidate": {
            "scenario": scenario,
            "n": len(st),
            "reclaim_fail_survivors_rate": rate(st, "reclaim_fail_survivors") if st else None,
            "hwm_min": hm,
            "dl_kbps_median": med(kb),
            "first_fail": min(first_fails) if first_fails else None,
            "tunnel_up_ms_p95": pct(up, 95),
            "fetch_ms_p95": pct(fm, 95),
        },
    }
    if extra:
        summary.update(extra)
    return summary


def write_outputs(out_dir, parser, summary):
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "iterations.jsonl"), "w") as f:
        for r in parser.records:
            f.write(json.dumps(r, sort_keys=True) + "\n")
    with open(os.path.join(out_dir, "iterations.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(CSV_COLUMNS)
        for r in parser.records:
            row = []
            for c in CSV_COLUMNS:
                if c.startswith("hwm_"):
                    v = (r.get("hwm") or {}).get(c[4:])
                    row.append("%d/%d" % tuple(v) if v else "")
                    continue
                v = r.get(c)
                if isinstance(v, (list, dict)):
                    v = ";".join(v) if c == "flags" else json.dumps(v)
                elif isinstance(v, str) and c == "panic_text":
                    v = v[:400].replace("\n", " | ")
                row.append("" if v is None else v)
            w.writerow(row)
    with open(os.path.join(out_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2, sort_keys=True)
    with open(os.path.join(out_dir, "boots.json"), "w") as f:
        json.dump(parser.boots, f, indent=2, sort_keys=True)


def print_summary(summary):
    print("\n=== summary ===")
    print("scenario=%s boots=%d records=%d selftest=%d results=%d counts=%s" % (
        summary["scenario"], summary["boots"], summary["records"], summary["selftest_records"],
        summary["results_seen"], summary["counts"]))
    print("reset_reasons=%s" % summary["reset_reasons"])
    print("tunnel_up_ms=%s fetch_ms=%s reclaim_tries=%s dl_kbps=%s" % (
        summary["tunnel_up_ms"], summary["fetch_ms"], summary["reclaim_tries"], summary["dl_kbps"]))
    if summary["warm_starts"]["attempted"] or summary["warm_starts"]["netmap_cached"]:
        print("warm_starts=%s tunnel_up_ms_warm=%s tunnel_up_ms_cold=%s" % (
            summary["warm_starts"], summary["tunnel_up_ms_warm"], summary["tunnel_up_ms_cold"]))
    if summary["coord_timing_ms"]:
        print("coord_timing_ms(median)=%s" % {k: v["median"] for k, v in summary["coord_timing_ms"].items()})
    print("hwm_min=%s" % {k: v["min"] for k, v in summary["hwm_min"].items()})
    print("flags=%s" % {k: v["count"] for k, v in summary["flag_rates"].items() if v["count"]})
    if summary.get("restore_ok") is not None:
        print("restore_ok=%s evidence=%s" % (summary["restore_ok"], summary["restore_evidence"]))
    print("rubric:")
    for r in summary["rubric"]:
        print("  %-24s %-12s %s" % (r["rule"], r["verdict"], r["detail"]))
    print("overall=%s" % summary["overall"])


def exit_code_for(summary, infra_error=False):
    if summary["flag_rates"]["crash"]["count"]:
        return EXIT_CRASH
    if infra_error:
        return EXIT_INFRA
    if summary["overall"] == "FAIL":
        return EXIT_RUBRIC
    return EXIT_PASS


# ---------------------------------------------------------------------------
# Mac transport
# ---------------------------------------------------------------------------
class InfraError(Exception):
    pass


class Mac:
    SSH_OPTS = ["-o", "BatchMode=yes", "-o", "ConnectTimeout=20", "-o", "ServerAliveInterval=15"]

    def __init__(self, host):
        self.host = host

    def ssh(self, cmd, timeout=120, check=False):
        p = subprocess.run(["ssh"] + self.SSH_OPTS + [self.host, cmd], capture_output=True, text=True, timeout=timeout)
        if check and p.returncode != 0:
            raise InfraError("ssh %r failed (%d): %s" % (cmd, p.returncode, (p.stderr or p.stdout).strip()[-400:]))
        return p

    def popen(self, cmd):
        return subprocess.Popen(["ssh"] + self.SSH_OPTS + [self.host, cmd], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)

    def scp_to(self, local, remote):
        p = subprocess.run(["scp"] + self.SSH_OPTS + ["-q", local, "%s:%s" % (self.host, remote)],
                           capture_output=True, text=True, timeout=600)
        if p.returncode != 0:
            raise InfraError("scp %s -> %s failed: %s" % (local, remote, p.stderr.strip()[-300:]))

    def scp_from(self, remote, local):
        p = subprocess.run(["scp"] + self.SSH_OPTS + ["-q", "%s:%s" % (self.host, remote), local],
                           capture_output=True, text=True, timeout=600)
        if p.returncode != 0:
            raise InfraError("scp %s -> %s failed: %s" % (remote, local, p.stderr.strip()[-300:]))

    def remote_sha256(self, remote):
        p = self.ssh("shasum -a 256 %s 2>/dev/null | cut -d' ' -f1" % shlex.quote(remote))
        return p.stdout.strip() or None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def log(msg):
    print("[%s] %s" % (datetime.now().strftime("%H:%M:%S"), msg), flush=True)


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------
class Runner:
    def __init__(self, args):
        self.a = args
        self.mac = Mac(args.host)
        self.parser = LogParser(args.scenario)
        self.tail = None
        self.reader = None
        self.q = queue.Queue()
        self.raw = None
        self.infra_error = None
        self.restore_ok = None
        self.coredump_path = None
        self.timings = {}
        self.image_sha = None

    # -- phases -------------------------------------------------------------
    def preflight(self):
        a = self.a
        log("preflight: host=%s port=%s" % (a.host, a.port))
        p = self.mac.ssh("ls %s" % shlex.quote(a.port))
        if p.returncode != 0:
            raise InfraError("port %s not listed on %s: %s" % (a.port, a.host, (p.stderr or p.stdout).strip()))
        p = self.mac.ssh("%s -c 'import esptool, serial; print(esptool.__version__, serial.VERSION)'" % REMOTE_PY)
        if p.returncode != 0:
            raise InfraError("~/fwvenv lacks esptool/pyserial: %s" % (p.stderr or p.stdout).strip()[-300:])
        log("preflight: esptool/pyserial %s" % p.stdout.strip())
        p = self.mac.ssh("pgrep -fl capture.py || true")
        procs = [l for l in p.stdout.splitlines() if "capture.py" in l]
        ours = [l for l in procs if "hw/capture.py" in l]
        others = [l for l in procs if l not in ours]
        if others:
            raise InfraError("stray capture.py on the Mac (not ours), stop it first: %s" % others)
        if ours:
            log("preflight: killing leftover hw/capture.py: %s" % ours)
            self.mac.ssh("pkill -f hw/capture.py || true")
            time.sleep(1)
        self.mac.ssh("mkdir -p %s" % REMOTE_DIR, check=True)

    def stage_file(self, local, remote_name):
        if not os.path.isfile(local):
            raise InfraError("missing local file %s" % local)
        remote = "%s/%s" % (REMOTE_DIR, remote_name)
        sha = sha256_file(local)
        if self.mac.remote_sha256(remote) == sha:
            log("stage: %s already on Mac (sha256 match)" % remote_name)
        else:
            log("stage: scp %s -> %s (%d bytes)" % (local, remote, os.path.getsize(local)))
            self.mac.scp_to(local, remote)
            if self.mac.remote_sha256(remote) != sha:
                raise InfraError("sha256 mismatch after scp of %s" % remote_name)
        return remote, sha

    def stage(self, image):
        if "factory" not in os.path.basename(image):
            log("WARNING: %s does not look like a factory image; chunkflash writes from offset 0" % image)
        for h in MAC_HELPERS:
            self.stage_file(os.path.join(HERE, "mac", h), h)
        self.mac.ssh("chmod +x %s/chunkflash.sh %s/capture.py" % (REMOTE_DIR, REMOTE_DIR), check=True)
        return self.stage_file(image, os.path.basename(image))

    def flash(self, remote_image, sha, allow_skip):
        if allow_skip:
            last = self.mac.ssh("cat %s/last_flashed.sha256 2>/dev/null" % REMOTE_DIR).stdout.strip()
            if last == sha:
                log("flash: skipped, sha256 matches last flashed image")
                return
        log("flash: %s (chunked)" % remote_image)
        t0 = time.time()
        p = self.mac.popen("bash %s/chunkflash.sh %s %s" % (REMOTE_DIR, shlex.quote(remote_image), shlex.quote(self.a.port)))
        out = []
        try:
            for raw in p.stdout:
                line = raw.decode("utf-8", "replace").rstrip()
                out.append(line)
                log("  flash| " + line)
            p.wait(timeout=1200)
        except subprocess.TimeoutExpired:
            p.kill()
            raise InfraError("flash timed out")
        self.timings["flash_s"] = round(time.time() - t0, 1)
        if not any(l.strip() == "FLASH COMPLETE" for l in out):
            raise InfraError("flash did not report FLASH COMPLETE (rc=%s)" % p.returncode)
        self.mac.ssh("printf '%s' %s > %s/last_flashed.sha256" % ("%s", shlex.quote(sha), REMOTE_DIR))

    def capture_start(self):
        self.mac.ssh("cd %s && mv -f run.log run.prev.log 2>/dev/null; rm -f send.txt; : > run.log" % REMOTE_DIR, check=True)
        cmd = ("cd %s && nohup %s ./capture.py %s run.log --send-file send.txt > capture.out 2>&1 < /dev/null & echo $!"
               % (REMOTE_DIR, REMOTE_PY, shlex.quote(self.a.port)))
        p = self.mac.ssh(cmd, check=True)
        log("capture: started capture.py pid %s" % p.stdout.strip())
        self.q = queue.Queue()
        self.tail = self.mac.popen("tail -n +1 -F %s" % REMOTE_LOG)
        self.reader = threading.Thread(target=self._pump, daemon=True)
        self.reader.start()

    def _pump(self):
        try:
            for raw in self.tail.stdout:
                self.q.put(raw.decode("utf-8", "replace"))
        finally:
            self.q.put(None)

    def capture_stop(self):
        if self.tail:
            try:
                self.tail.kill()
            except Exception:
                pass
            self.tail = None
        self.mac.ssh("pkill -f hw/capture.py || true")
        time.sleep(1)

    def repulse(self):
        log("silence: re-pulsing RTS via SIGUSR1")
        self.mac.ssh("pkill -USR1 -f hw/capture.py || true")

    def send_serial(self, text):
        """Queue bytes for capture.py to write to the port (CMD:REBOOT etc.)."""
        self.mac.ssh("printf '%%s\\n' %s > %s" % (shlex.quote(text), REMOTE_SEND), check=True)

    def open_raw(self, name="run.log"):
        os.makedirs(self.a.out, exist_ok=True)
        self.raw = open(os.path.join(self.a.out, name), "a")

    def _drain_events(self, on_event):
        while self.parser.events:
            ev, payload = self.parser.events.pop(0)
            on_event(ev, payload)

    def loop(self):
        a = self.a
        need = a.iterations
        t_start = time.time()
        last_line = time.time()
        iter_start = None
        repulsed = False
        wifi_fail_streak = 0
        state = {"done": 0, "crash": False, "stop": None}

        def on_event(ev, payload):
            nonlocal iter_start, wifi_fail_streak
            if ev == "iter_open":
                iter_start = time.time()
                log("iteration open (boot %s cycle %s by %s)" % (payload["boot"], payload["cycle"], payload["opened_by"]))
            elif ev == "iter_close":
                r = payload
                if r["selftest"] and r["result"] != "INCOMPLETE":
                    state["done"] += 1
                iter_start = None
                log("iteration #%d closed: %s flags=%s up=%s tls=%s fetch=%s/%s reclaim=%s surv=%s" % (
                    r["idx"], r["result"], r["flags"], r["tunnel_up_ms"], r["tls_ms"], r["fetch_ok"],
                    r["fetch_bytes"], r["reclaim_ok"], r["survivors"]))
                if "wifi_fail" in r["flags"]:
                    wifi_fail_streak += 1
                    if wifi_fail_streak >= 2:
                        state["stop"] = ("infra", "WiFi failed twice in a row")
                else:
                    wifi_fail_streak = 0
                if state["done"] >= need:
                    state["stop"] = ("done", "%d iterations" % state["done"])
            elif ev == "crash":
                state["crash"] = True
                log("CRASH detected: %s" % (payload["panic_text"] or "").splitlines()[0])
                if not a.continue_on_crash:
                    state["stop"] = ("crash", "panic")
            elif ev == "boot":
                log("boot #%d" % payload["index"])
            elif ev == "bad_reset":
                log("reset reason %s is not in the allowed set" % payload["reset_reason"])
            elif ev == "config":
                sc = SCENARIOS.get(a.scenario)
                if sc and any(sc.get(k) is not None and sc[k] != payload[k] for k in ("fb", "ballast", "cycles", "mode")):
                    log("WARNING: device config %s does not match scenario %s %s" % (payload, a.scenario, sc))

        while state["stop"] is None:
            try:
                item = self.q.get(timeout=1.0)
            except queue.Empty:
                item = False
            now = time.time()
            if item is None:
                state["stop"] = ("infra", "tail -F stream ended")
                break
            if item:
                last_line = now
                repulsed = False
                self.raw.write(item)
                self.raw.flush()
                self.parser.feed(item)
                self._drain_events(on_event)
                continue
            if now - last_line > a.silence_timeout:
                if not repulsed:
                    self.repulse()
                    repulsed = True
                    last_line = now
                else:
                    if self.parser.cur is not None:
                        add_flag(self.parser.cur, "silence_timeout")
                        self.parser._close(None, "TIMEOUT")
                        self._drain_events(on_event)
                        state["stop"] = ("rubric", "silence %ds with an iteration open" % a.silence_timeout)
                    else:
                        state["stop"] = ("infra", "silence %ds, no iteration open" % a.silence_timeout)
            if iter_start and now - iter_start > a.iteration_timeout:
                add_flag(self.parser.cur, "iteration_timeout")
                self.parser._close(None, "TIMEOUT")
                self._drain_events(on_event)
                state["stop"] = state["stop"] or ("rubric", "iteration timeout %ds" % a.iteration_timeout)
            if now - t_start > a.total_timeout:
                state["stop"] = ("infra", "total timeout %ds" % a.total_timeout)
        self.parser.finalize()
        self._drain_events(on_event)
        self.timings["loop_s"] = round(time.time() - t_start, 1)
        log("loop stopped: %s (%s)" % state["stop"])
        return state

    def read_coredump(self):
        log("coredump: reading partition 0x%X (%d bytes)" % (COREDUMP_OFF, COREDUMP_LEN))
        cmd = ("%s -m esptool --chip esp32c3 --port %s --baud 460800 --before usb-reset --after no-reset "
               "read-flash 0x%X 0x%X %s/coredump.bin" % (REMOTE_PY, shlex.quote(self.a.port), COREDUMP_OFF,
                                                         COREDUMP_LEN, REMOTE_DIR))
        p = self.mac.ssh(cmd, timeout=300)
        if p.returncode != 0:
            log("coredump: read failed: %s" % (p.stderr or p.stdout).strip()[-300:])
            return None
        local = os.path.join(self.a.out, "coredump.bin")
        self.mac.scp_from("%s/coredump.bin" % REMOTE_DIR, local)
        self.coredump_path = local
        log("coredump: saved %s" % local)
        for r in self.parser.records:
            if "crash" in r["flags"]:
                r["coredump_path"] = local
        return local

    def symbolise(self):
        elf = self.a.elf
        a2l = self.a.addr2line
        for r in self.parser.records:
            if "crash" not in r["flags"]:
                continue
            addrs = [x for x in (r["mepc"], r["ra"]) if x]
            if r["backtrace"]:
                addrs += [t.split(":")[0] for t in r["backtrace"].split() if t.startswith("0x")]
            if not addrs:
                print("record %d: crash without MEPC/RA; inspect coredump: espcoredump.py info_corefile -t raw -c %s %s"
                      % (r["idx"], self.coredump_path, elf))
                continue
            cmd = "%s -pfiaC -e %s %s" % (a2l, elf, " ".join(addrs))
            print("symbolise record %d:\n  %s" % (r["idx"], cmd))
            if os.path.exists(elf) and os.path.exists(a2l):
                p = subprocess.run(shlex.split(cmd), capture_output=True, text=True)
                r["symbols"] = p.stdout.strip()
                print("  " + p.stdout.strip().replace("\n", "\n  "))
            if self.coredump_path:
                print("  espcoredump.py info_corefile -t raw -c %s %s" % (self.coredump_path, elf))

    def restore(self):
        a = self.a
        log("restore: flashing default image %s" % a.default_image)
        t0 = time.time()
        remote, sha = self.stage(a.default_image)
        self.flash(remote, sha, allow_skip=False)
        self.mac.ssh("%s -m esptool --chip esp32c3 --port %s --before default-reset --after hard-reset chip-id >/dev/null 2>&1 || true"
                     % (REMOTE_PY, shlex.quote(a.port)), timeout=120)
        rp = LogParser(None)
        self.capture_start()
        self.open_raw("restore.log")
        deadline = time.time() + 60
        repulsed = False
        ok = False
        while time.time() < deadline + (30 if repulsed else 0):
            try:
                item = self.q.get(timeout=1.0)
            except queue.Empty:
                item = None
            if item:
                self.raw.write(item)
                self.raw.flush()
                rp.feed(item)
                if rp.restore["version"] and any(RESTORE_ACTIVITY.search("Entering activity: " + x) for x in rp.restore["activities"]):
                    ok = True
                    break
            elif time.time() > deadline and not repulsed:
                self.repulse()
                repulsed = True
        self.capture_stop()
        self.restore_ok = ok
        self.timings["restore_s"] = round(time.time() - t0, 1)
        log("restore: ok=%s version=%s activities=%s" % (ok, rp.restore["version"], rp.restore["activities"][:4]))
        return ok

    # -- orchestration --------------------------------------------------------
    def run(self):
        a = self.a
        os.makedirs(a.out, exist_ok=True)
        baseline = load_baseline(a.baseline)
        state = {"crash": False, "stop": ("none", "")}
        try:
            self.preflight()
            if a.restore_only:
                self.restore()
            else:
                remote, self.image_sha = self.stage(a.image)
                self.flash(remote, self.image_sha, allow_skip=a.skip_flash_if_same)
                self.capture_start()
                self.open_raw()
                try:
                    state = self.loop()
                finally:
                    self.capture_stop()
                if state["crash"] or self.parser.crashes:
                    try:
                        self.read_coredump()
                    except InfraError as e:
                        log("coredump: %s" % e)
                    self.symbolise()
                if state["stop"][0] == "infra":
                    self.infra_error = state["stop"][1]
                if not a.no_restore:
                    try:
                        self.restore()
                    except InfraError as e:
                        log("restore: %s" % e)
                        self.restore_ok = False
        except InfraError as e:
            log("INFRA ERROR: %s" % e)
            self.infra_error = str(e)
        except KeyboardInterrupt:
            log("interrupted; stopping capture")
            self.infra_error = "interrupted"
            self.capture_stop()
        summary = build_summary(self.parser, a.scenario, baseline, self.restore_ok, extra={
            "image": a.image, "image_sha256": self.image_sha, "host": a.host, "port": a.port,
            "timings": self.timings, "stop_reason": list(state["stop"]), "infra_error": self.infra_error,
            "coredump": self.coredump_path, "out": a.out,
        })
        code = exit_code_for(summary, infra_error=bool(self.infra_error))
        summary["exit_code"] = code
        write_outputs(a.out, self.parser, summary)
        print_summary(summary)
        log("outputs in %s (exit %d)" % (a.out, code))
        return code


# ---------------------------------------------------------------------------
# Offline modes
# ---------------------------------------------------------------------------
def parse_file(path, scenario=None):
    p = LogParser(scenario)
    with open(path, "rb") as f:
        for raw in f:
            p.feed(raw.decode("utf-8", "replace"))
    p.finalize()
    return p


def parse_only(args):
    p = parse_file(args.parse_only, args.scenario)
    baseline = load_baseline(args.baseline)
    summary = build_summary(p, args.scenario, baseline, None, extra={"source": args.parse_only})
    code = exit_code_for(summary)
    summary["exit_code"] = code
    write_outputs(args.out, p, summary)
    print_summary(summary)
    log("outputs in %s (exit %d)" % (args.out, code))
    return code


def selftest():
    """Parser self-check against scripts/hw/fixtures; expectations verified by hand."""
    fx = os.path.join(HERE, "fixtures")
    failures = []

    def check(name, cond, detail=""):
        if not cond:
            failures.append("%s %s" % (name, detail))
        print("  %s %s %s" % ("ok  " if cond else "FAIL", name, detail))

    print("fixture stv.log")
    p = parse_file(os.path.join(fx, "stv.log"), "feed_baseline")
    r0 = p.records[0]
    check("boots=2", len(p.boots) == 2, len(p.boots))
    check("reset reasons allowed", all(b["reset_ok"] for b in p.boots), [b["reset_reason"] for b in p.boots])
    check("records=2", len(p.records) == 2, len(p.records))
    check("iter0 result FAIL", r0["result"] == "FAIL", r0["result"])
    check("iter0 wifi_ms=33381 (connected 33878 - begin 497)", r0["wifi_ms"] == 33381, r0["wifi_ms"])
    check("iter0 fb/ballast", r0["fb_bytes"] == 48000 and r0["ballast_got"] == 65004 and r0["free_with_fb"] == 31852,
          (r0["fb_bytes"], r0["ballast_got"], r0["free_with_fb"]))
    check("iter0 release free/largest", (r0["free_at_release"], r0["largest_at_release"]) == (80860, 49140))
    check("iter0 tunnel start/derp/up", (r0["tunnel_start_free"], r0["derp_hs_free"], r0["tunnel_up_ms"], r0["tunnel_up_free"])
          == (56328, 26500, 6376, 32684), (r0["tunnel_start_free"], r0["derp_hs_free"], r0["tunnel_up_ms"]))
    check("iter0 cached address", r0["cached_address"] is True)
    check("iter0 tls", (r0["tls_ms"], r0["tls_version"], r0["tls_suite"]) == (608, "TLSv1.3", "TLS_AES_128_GCM_SHA256"))
    check("iter0 fetch OK 5253 in 993", (r0["fetch_ok"], r0["fetch_bytes"], r0["fetch_ms"]) == (True, 5253, 993))
    check("iter0 hwm", r0["hwm"] == {"coord": [4652, 8192], "derp_tx": [3520, 6144], "net_io": [3860, 6144], "wg_mgr": [3692, 6144]}, r0["hwm"])
    check("iter0 reclaim FAIL after 10 tries", (r0["reclaim_ok"], r0["reclaim_tries"]) == (False, 10), (r0["reclaim_ok"], r0["reclaim_tries"]))
    check("iter0 survivors=6, 424 bytes", (r0["survivors"], len(r0["survivors_dumps"]), r0["survivors_bytes"]) == (6, 6, 424),
          (r0["survivors"], len(r0["survivors_dumps"]), r0["survivors_bytes"]))
    check("iter0 old survivor format first=abba1234", all(d["hex"] == "abba1234" and d["phase"] is None for d in r0["survivors_dumps"]))
    check("iter0 spool readback 5253", r0["spool_readback"] == 5253)
    check("iter0 flags == [reclaim_fail_survivors]", r0["flags"] == ["reclaim_fail_survivors"], r0["flags"])
    r1 = p.records[1]
    check("iter1 INCOMPLETE (log ends mid-run), not a reset", r1["result"] == "INCOMPLETE" and "unexpected_reset" not in r1["flags"], (r1["result"], r1["flags"]))
    check("iter1 tunnel up 5900", r1["tunnel_up_ms"] == 5900, r1["tunnel_up_ms"])
    rules, overall = grade(p.records, p.boots, "feed_baseline", None, None)
    v = {r["rule"]: r["verdict"] for r in rules}
    check("rubric crash_or_reset PASS", v["crash_or_reset"] == "PASS", v["crash_or_reset"])
    check("rubric reclaim_fail_rate NO_BASELINE", v["reclaim_fail_rate"] == "NO_BASELINE", v["reclaim_fail_rate"])
    check("rubric reclaim ceilings PASS (6 <= 8, 424 <= 768)", v["reclaim_fail_ceilings"] == "PASS", v["reclaim_fail_ceilings"])
    check("rubric gate WARN (0/1 pass, n<10)", v["gate_pass_rate"] == "WARN", v["gate_pass_rate"])

    print("fixture dev.log")
    p = parse_file(os.path.join(fx, "dev.log"), None)
    check("boots=1", len(p.boots) == 1, len(p.boots))
    check("records=2 tunnel windows", len(p.records) == 2, len(p.records))
    w0, w1 = p.records[0], p.records[1]
    check("window0 feed /opds, up 5999, tls 543", (w0["kind"], w0["fetch_url"], w0["tunnel_up_ms"], w0["tls_ms"])
          == ("feed", "https://calibre.lab.wayvz.io/opds", 5999, 543), (w0["kind"], w0["fetch_url"], w0["tunnel_up_ms"], w0["tls_ms"]))
    check("window0 closes on STACK_HWM", w0["hwm"].get("coord") == [4652, 8192], w0["hwm"])
    check("window1 download 8992638 bytes", (w1["kind"], w1["dl_bytes"]) == ("download", 8992638), (w1["kind"], w1["dl_bytes"]))
    check("window1 tunnel up 5999, tls 533", (w1["tunnel_up_ms"], w1["tls_ms"]) == (5999, 533))
    check("window1 hwm net_io 3748", w1["hwm"].get("net_io") == [3748, 6144], w1["hwm"])
    check("no not_enough_heap in real dev.log", not any("not_enough_heap" in r["flags"] for r in p.records))
    check("no flags at all", all(r["flags"] == [] for r in p.records), [r["flags"] for r in p.records])
    check("restore evidence version", p.restore["version"] == "1.6.0-dev-develop-b7bceb56", p.restore["version"])
    check("restore evidence activities", p.restore["activities"][:3] == ["Boot", "EpubReader", "Home"], p.restore["activities"][:3])

    print("fixture synthetic_newfmt.log")
    p = parse_file(os.path.join(fx, "synthetic_newfmt.log"), "download_9mb")
    check("boots=3", len(p.boots) == 3, len(p.boots))
    check("records=3", len(p.records) == 3, len(p.records))
    c1, c2, c3 = p.records
    check("config parsed", p.boots[0]["config"] == dict(fb=48000, ballast=36000, cycles=2, mode="download", src="json"), p.boots[0]["config"])
    check("cycle1 not_enough_heap + TS-E03 + stage", (c1["cycle"], c1["cycle_total"], c1["ts_code"], c1["stage"], c1["result"])
          == (1, 2, "TS-E03", "ensureUp", "FAIL") and "not_enough_heap" in c1["flags"], (c1["cycle"], c1["ts_code"], c1["stage"], c1["flags"]))
    check("cycle1 msg", c1["msg"] == "Not enough memory for the tailnet session", c1["msg"])
    check("cycle2 dl record", c2["dl"] and c2["dl"][0]["kbps"] == 32 and c2["dl_size_ok"] and c2["dl_file_ok"] and "dl_bad" not in c2["flags"], c2["dl"])
    check("cycle2 http download line", (c2["dl_bytes"], c2["dl_content_length"], c2["dl_kbps"]) == (9437184, 9437184, 32))
    check("cycle2 survivors new format", [(d["phase"], d["user"], d["task"]) for d in c2["survivors_dumps"]]
          == [("U", 96, "tcpip_thread"), ("T", 16, "loopTask")], c2["survivors_dumps"])
    check("cycle2 survivors bytes/phases", (c2["survivors_bytes"], c2["survivors_phases"], c2["survivors_up"]) == (128, "UT", 1))
    check("cycle2 hwm derp_tx 920/4096", c2["hwm"].get("derp_tx") == [920, 4096])
    check("cycle2 reclaim tries 2", c2["reclaim_tries"] == 2, c2["reclaim_tries"])
    check("boot summary", p.boots[0]["summary"] == dict(cycles=2, passed=0, failed=2, first_fail=1, deferred_stop=False), p.boots[0]["summary"])
    check("boot0 -> boot1 reboot expected (no unexpected_reset)", "unexpected_reset" not in c2["flags"], c2["flags"])
    check("crash record", c3["result"] == "CRASH" and "crash" in c3["flags"] and c3["mepc"] == "0x4200a1c4" and c3["ra"] == "0x4200a0f8",
          (c3["result"], c3["flags"], c3["mepc"], c3["ra"]))
    check("crash reset reason flagged", p.boots[2]["reset_ok"] is False and p.boots[2]["reset_reason"] == "RTC_SW_SYS_RST")
    rules, overall = grade(p.records, p.boots, "download_9mb", None, None)
    v = {r["rule"]: r["verdict"] for r in rules}
    check("rubric crash_or_reset FAIL", v["crash_or_reset"] == "FAIL")
    check("rubric hwm WARN (<1024 on derp_tx)", v["stack_hwm_min"] == "WARN", v["stack_hwm_min"])
    check("rubric not_enough_heap_cycle1 FAIL", v["not_enough_heap_cycle1"] == "FAIL")
    check("rubric download_integrity PASS", v["download_integrity"] == "PASS")
    check("rubric download_throughput NO_BASELINE", v["download_throughput"] == "NO_BASELINE")
    s = build_summary(p, "download_9mb", None, None)
    check("exit code 3 on crash", exit_code_for(s) == EXIT_CRASH)
    base = {"reclaim_fail_survivors_rate": 0.0, "hwm_min": {"derp_tx": {"min": 3520, "size": 6144}}, "dl_kbps_median": 100}
    rules, _ = grade(p.records, p.boots, "download_9mb", base, True)
    v = {r["rule"]: r["verdict"] for r in rules}
    check("baseline: throughput 32 < 50 FAIL", v["download_throughput"] == "FAIL")
    check("baseline: hwm drop > 512 WARN", v["stack_hwm_vs_baseline"] == "WARN")
    check("baseline: reclaim rate 0.5 > 0+0.15 WARN (n<10)", v["reclaim_fail_rate"] == "WARN", v["reclaim_fail_rate"])
    check("restore_ok True PASS", v["restore_ok"] == "PASS")

    print("\n%d checks failed" % len(failures) if failures else "\nall checks passed")
    return 1 if failures else 0


# ---------------------------------------------------------------------------
def default_addr2line():
    for base in (os.path.join(REPO, ".cache", "platformio", "packages"), os.path.expanduser("~/.platformio/packages")):
        c = os.path.join(base, "toolchain-riscv32-esp", "bin", "riscv32-esp-elf-addr2line")
        if os.path.exists(c):
            return c
    return "riscv32-esp-elf-addr2line"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default=os.path.join(REPO, ".pio", "build", "selftest", "firmware.factory.bin"))
    ap.add_argument("--default-image", default=os.path.join(REPO, ".pio", "build", "default", "firmware.factory.bin"))
    ap.add_argument("--host", default="rorychatterton@chattstudio")
    ap.add_argument("--port", default="/dev/cu.usbmodem8401")
    ap.add_argument("--scenario", choices=sorted(SCENARIOS), default=None)
    ap.add_argument("--iterations", type=int, default=10)
    ap.add_argument("--iteration-timeout", type=int, default=360)
    ap.add_argument("--total-timeout", type=int, default=3600)
    ap.add_argument("--silence-timeout", type=int, default=120)
    ap.add_argument("--baseline", default=None, help="baseline JSON (a summary.json or its baseline_candidate)")
    ap.add_argument("--out", default=None, help="output directory (default scripts/hw/runs/<ts>_<scenario>)")
    ap.add_argument("--skip-flash-if-same", action="store_true")
    ap.add_argument("--no-restore", action="store_true")
    ap.add_argument("--restore-only", action="store_true")
    ap.add_argument("--continue-on-crash", action="store_true")
    ap.add_argument("--parse-only", metavar="LOGFILE", help="parse an existing log, no hardware")
    ap.add_argument("--selftest", action="store_true", help="run the parser self-check on scripts/hw/fixtures")
    ap.add_argument("--elf", default=os.path.join(REPO, ".pio", "build", "selftest", "firmware.elf"))
    ap.add_argument("--addr2line", default=default_addr2line())
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    if args.out is None:
        tag = "parse_" + os.path.splitext(os.path.basename(args.parse_only))[0] if args.parse_only else (args.scenario or "run")
        args.out = os.path.join(HERE, "runs", "%s_%s" % (ts, tag))
    if args.parse_only:
        return parse_only(args)
    if args.scenario is None:
        log("note: no --scenario; rubric runs in generic mode (no gate, no scenario checks)")
    return Runner(args).run()


if __name__ == "__main__":
    sys.exit(main())
