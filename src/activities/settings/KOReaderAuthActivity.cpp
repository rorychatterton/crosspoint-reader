#include "KOReaderAuthActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/TailnetSession.h"

void KOReaderAuthActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = tr(STR_WIFI_CONN_FAILED);
    }
    requestUpdate();
    return;
  }

  WiFi.setSleep(false);
  LOG_DBG("KOAuth", "WiFi sleep disabled for authentication");

  {
    RenderLock lock(*this);
    state = AUTHENTICATING;
    statusMessage = mode == Mode::SIGN_UP ? tr(STR_CREATING_ACCOUNT) : tr(STR_AUTHENTICATING);
  }
  // Over the tailnet the framebuffer is lent to the heap for the whole request
  // (overTailnet), so the status must already be on the panel before the
  // window opens: the panel holds that frame until the reclaim repaints.
  if (KOREADER_STORE.getUseTailnet()) {
    requestUpdateAndWait();
  } else {
    requestUpdate();
  }

  performAuthentication();
}

KOReaderSyncClient::Error KOReaderAuthActivity::authOp() const {
  return mode == Mode::SIGN_UP ? KOReaderSyncClient::createUser() : KOReaderSyncClient::authenticate();
}

void KOReaderAuthActivity::performAuthentication() {
  KOReaderSyncClient::Error result = KOReaderSyncClient::NETWORK_ERROR;
  if (KOREADER_STORE.getUseTailnet()) {
    if (!overTailnet(result)) return;  // tunnel failed; error shown
  } else {
    result = authOp();
  }

  {
    RenderLock lock(*this);
    if (result == KOReaderSyncClient::OK) {
      state = SUCCESS;
      statusMessage = mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS);
    } else {
      state = FAILED;
      errorMessage =
          result == KOReaderSyncClient::USER_EXISTS ? tr(STR_USERNAME_TAKEN) : KOReaderSyncClient::errorString(result);
    }
  }
  requestUpdate();
}

bool KOReaderAuthActivity::overTailnet(KOReaderSyncClient::Error& result) {
  // Same memory model as KOReaderSyncActivity::overTailnet: ~71 KB free with
  // the 48 KB framebuffer held, ensureUp refuses below MIN_TAILNET_FREE_HEAP,
  // and tunnel plus auth TLS need ~45 KB more. So the framebuffer is lent to
  // the heap for the window (the panel keeps the "Authenticating..." frame),
  // authOp must not render, and the tunnel is torn down before the reclaim.
  // A failed reclaim reboots to Home unpainted; the settings screen already
  // saved the credentials. The target is set before the release because the
  // session keeps the host string past teardown, outside the released region.
  std::string baseUrl = KOREADER_STORE.getBaseUrl();
  TAILNET.setTargetUrl(baseUrl);
  KOReaderSyncClient::reserveBaseUrlOverride(baseUrl.size() + 32);  // storage outlives the window
  bool up = false;
  {
    RenderLock lock;
    renderer.releaseFrameBufferToHeap();
    LOG_INF("KOAuth", "tailnet window released free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (TAILNET.ensureUp() && !(baseUrl = TAILNET.rewriteUrlForTailnet(baseUrl)).empty()) {
      up = true;
      LOG_INF("KOAuth", "tailnet window up free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      KOReaderSyncClient::setBaseUrlOverride(baseUrl);
      result = authOp();
      LOG_INF("KOAuth", "tailnet window done free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    TAILNET.teardown();
    LOG_INF("KOAuth", "tailnet window torn down free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    // Frees queued on the tcpip/WiFi tasks can land after teardown returns.
    bool reclaimed = false;
    for (int attempt = 0; attempt < 10 && !reclaimed; attempt++) {
      if (attempt) delay(200);
      reclaimed = renderer.reacquireFrameBufferFromHeap();
    }
    LOG_INF("KOAuth", "tailnet window reclaim=%s free=%u largest=%u", reclaimed ? "OK" : "FAIL", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    if (!reclaimed) {
      LOG_ERR("KOAuth", "Framebuffer not reclaimable after tailnet auth; rebooting to home");
      silentRestart(/*paint=*/false);
      for (;;) delay(1000);  // ESP.restart() does not return
    }
  }
  if (!up) {
    {
      RenderLock lock(*this);
      state = FAILED;
      errorMessage = std::string(TAILNET.lastErrorCode()) + ": " + TAILNET.lastErrorMessage();
    }
    requestUpdate();
  }
  return up;
}

void KOReaderAuthActivity::onEnter() {
  Activity::onEnter();

  // Check if already connected
  if (WiFi.status() == WL_CONNECTED) {
    onWifiSelectionComplete(true);
    return;
  }

  // Launch WiFi selection
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void KOReaderAuthActivity::onExit() {
  Activity::onExit();
  KOReaderSyncClient::clearBaseUrlOverride();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    if (TAILNET.wasActive()) TAILNET.teardown();
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void KOReaderAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 mode == Mode::SIGN_UP ? tr(STR_SIGN_UP) : tr(STR_KOREADER_AUTH));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == AUTHENTICATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, statusMessage.c_str());
  } else if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top,
                              mode == Mode::SIGN_UP ? tr(STR_ACCOUNT_CREATED) : tr(STR_AUTH_SUCCESS), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_SYNC_READY));
  } else if (state == FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, mode == Mode::SIGN_UP ? tr(STR_SIGNUP_FAILED) : tr(STR_AUTH_FAILED),
                              true, EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(renderer, Rect{16, top + height + 10, pageWidth - 32, height * 4}, UI_10_FONT_ID,
                                     errorMessage.c_str(), 4);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void KOReaderAuthActivity::loop() {
  if (state == SUCCESS || state == FAILED) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}
