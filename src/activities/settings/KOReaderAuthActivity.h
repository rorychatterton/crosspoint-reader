#pragma once

#include <functional>

#include "KOReaderSyncClient.h"
#include "activities/Activity.h"

/**
 * Activity for testing KOReader credentials, or — in sign-up mode — creating a
 * new account on the sync server with the entered username/password.
 * Connects to WiFi, then authenticates or registers.
 */
class KOReaderAuthActivity final : public Activity {
 public:
  enum class Mode { AUTHENTICATE, SIGN_UP };

  explicit KOReaderAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Mode mode = Mode::AUTHENTICATE)
      : Activity("KOReaderAuth", renderer, mappedInput), mode(mode) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == CONNECTING || state == AUTHENTICATING; }

 private:
  enum State { WIFI_SELECTION, CONNECTING, AUTHENTICATING, SUCCESS, FAILED };

  Mode mode = Mode::AUTHENTICATE;
  State state = WIFI_SELECTION;
  std::string statusMessage;
  std::string errorMessage;

  void onWifiSelectionComplete(bool success);
  void performAuthentication();
  // The authenticate / create-user request itself. Must not render: it runs
  // inside the tailnet window with the framebuffer lent to the heap.
  KOReaderSyncClient::Error authOp() const;
  // Bring the tunnel up around authOp() (see the .cpp for the memory model).
  // Returns false when the tunnel failed; the FAILED state is already shown.
  bool overTailnet(KOReaderSyncClient::Error& result);
};
