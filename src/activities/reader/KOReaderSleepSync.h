#pragma once
#include <string>

#include "CrossPointPosition.h"

class GfxRenderer;

/**
 * Pushes the open book's position to the KOSync server on the way into deep
 * sleep (Sync Progress on Sleep). The reader captures its position while it
 * still owns the book; main pushes it once the sleep screen is on the panel.
 */
namespace kosync_sleep {

// No-op unless the setting is on and credentials are set.
void capture(const std::string& epubPath, const CrossPointPosition& position);

// True when a captured position differs from the last one handled.
bool pending();

// Uploads the captured position over the connected WiFi. Lends the
// framebuffer, so the panel must already hold the sleep screen and nothing
// may draw afterwards.
void push(GfxRenderer& renderer);

}  // namespace kosync_sleep
