#pragma once

// Pure decision helpers for KOReader smart sync. Header-only and free of
// Arduino/network includes so the host gtest suite (test/koreader_sync_decision)
// can exercise exactly the branches KOReaderSyncActivity runs on-device.

#include <cmath>
#include <cstdint>

namespace koreader_sync {

// Outcome of one remote progress lookup. Skipped: no lookup was made (the
// alternate document id is unavailable or equal to the primary one).
enum class Lookup : uint8_t { Found, NotFound, Failed, Skipped };

// True when smart mode can upload in the same tunnel window as the fetch: no
// record exists under either document id, so the result is "upload" without
// mapping anything through the Epub. A found record needs the mapped-position
// comparison, and a failed lookup keeps the separate decision step.
inline bool uploadInFetchWindow(bool smart, Lookup primary, Lookup alternate) {
  if (!smart || primary != Lookup::NotFound) return false;
  return alternate == Lookup::NotFound || alternate == Lookup::Skipped;
}

enum class SmartAction {
  AlreadySynced,  // |local - remote| <= eps: nothing to do
  UploadLocal,    // local is ahead: push it to the server
  ApplyRemote     // remote is ahead: jump the reader to it
};

// Smart-mode decision. `local` and `remote` are 0..1 progress fractions;
// `eps` is the "same position" tolerance (0.001 == 0.1 percentage points).
//
// A non-finite remote percentage (NaN/inf) cannot be compared, so it is
// treated as an untrustworthy record and the local position wins. Nothing
// the sync client parses today produces one (ArduinoJson yields 0 for a
// missing or non-numeric field); this only pins the behaviour.
inline SmartAction decideSmart(float local, float remote, float eps = 0.001f) {
  if (!std::isfinite(remote)) return SmartAction::UploadLocal;
  const float delta = local - remote;
  if (std::fabs(delta) <= eps) return SmartAction::AlreadySynced;
  return delta > 0 ? SmartAction::UploadLocal : SmartAction::ApplyRemote;
}

// Alternate document-hash merge. Smart mode probes the other matching method
// (filename vs. binary) so a stale local upload never clobbers progress a
// KOReader device synced under it. Returns true when the alternate record
// should replace the primary one.
//
//   - alternate lookup failed (not OK)          -> keep primary
//   - primary NOT_FOUND, alternate OK            -> take alternate
//   - primary errored (neither OK nor NOT_FOUND) -> take alternate: it is the
//     only successful lookup (the primary percentage is still the 0 from
//     construction, so comparing against it would be meaningless)
//   - both OK                                    -> take alternate only if it
//     is strictly further ahead (ties keep the configured method)
inline bool preferAlternate(bool primaryOk, bool primaryNotFound, float primaryPct, bool altOk, float altPct) {
  if (!altOk) return false;
  if (primaryNotFound || !primaryOk) return true;
  return altPct > primaryPct;
}

}  // namespace koreader_sync
