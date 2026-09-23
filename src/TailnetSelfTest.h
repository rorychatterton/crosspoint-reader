#pragma once
// Headless tailnet self-test. Built only when CROSSPOINT_TAILNET_SELFTEST is
// defined (the [env:selftest*] profiles). Runs on boot, before the UI, so the
// full tailnet path can be exercised over USB serial with no user interaction.
//
// Modes (build flag defaults, overridable per run from /.crosspoint/selftest.json):
//   feed      OPDS feed fetch through the tunnel, spooled to SD (the original test)
//   download  HttpDownloader::downloadToFile + size/MD5 check of the SD file
//   sync      KOReader progress round trip in two tunnel windows (-DCROSSPOINT_SELFTEST_SYNC)
// Every mode runs N cycles per boot, logs SELFTEST RESULT= per cycle and
// SELFTEST BOOT_SUMMARY once, then reboots. See docs/tailnet-offline-test-plan.md s4.
void runTailnetSelfTest();
