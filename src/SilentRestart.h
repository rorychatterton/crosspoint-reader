#pragma once

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session. The live frontlight
// state rides along in the same flag so the reboot is invisible: the light
// comes back exactly as it was, regardless of the Restore Light on Wake
// preference.

// paint=false when the framebuffer has been released to the heap (nothing can
// draw the popup).
void silentRestart(bool paint = true);            // home screen
void silentRestartToReader(bool paint = true);    // currently-open EPUB (APP_STATE.openEpubPath)
void silentRestartToSettings(bool paint = true);  // settings screen

// Reboots immediately after an activity releases exclusive raw storage. The
// RTC target ensures setup() lands on Home instead of resuming a reader.
void restartToHomeAfterStorageHandoff();

// Reboot into a clean heap and resume the tailnet OPDS browse for the given
// server index. The concurrent DERP + HTTPS data path needs a large
// contiguous block only a fresh boot provides. tailnetRebootAttemptCount()
// is the loop guard (0 on a normal boot; >0 while resuming from such a reboot).
// paint=false when the framebuffer is released (nothing can draw the popup).
void silentRestartToOpds(uint32_t serverIndex, bool paint = true);
uint32_t tailnetRebootAttemptCount();
void clearTailnetRebootAttempt();

// OPDS feed cache session id: set when the browser is entered from Home and
// preserved (RTC_NOINIT) across silentRestartToOpds(), so pages spooled
// before a heap reboot still count as same-session hits after it.
uint32_t opdsBrowseSessionId();
void setOpdsBrowseSessionId(uint32_t id);
