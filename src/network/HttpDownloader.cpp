#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <ResumableFetch.h>

#include <functional>
#include <string>

#include "WifiPowerSaveGuard.h"

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }

namespace {
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. -DCROSSPOINT_HTTP_TIMEOUT_MS=<ms> overrides it for soak
// builds (self-test).
#ifndef CROSSPOINT_HTTP_TIMEOUT_MS
#define CROSSPOINT_HTTP_TIMEOUT_MS 60000
#endif
constexpr int HTTP_TIMEOUT_MS = CROSSPOINT_HTTP_TIMEOUT_MS;

// All HTTP(S) fetches go through wolfSSL (the firmware's only TLS stack: it
// speaks TLS 1.3 and reads large bodies reliably). Plain-http URLs still use a
// WiFiClient here, so this is safe for non-TLS targets too. A body cut short
// mid-transfer resumes with a Range request (see ResumableFetch.h).
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password,
                                           const std::vector<HttpDownloader::Header>& headers,
                                           const freeink::FetchSink& sink, const bool* cancelFlag = nullptr,
                                           size_t* bytesOut = nullptr, const bool downgradeRedirectsToHttp = false) {
  WifiPowerSaveGuard psGuard;
  freeink::FetchOptions options;
  options.redirectToHttp = downgradeRedirectsToHttp;
  const freeink::FetchResult result = freeink::fetchResumable(
      url, options,
      [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.setInsecure();
        // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
        // append a second User-Agent header, which strict servers reject (aiohttp
        // answers 400 "Duplicate 'User-Agent' header found").
        http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
        // Credentials and caller headers stay with the starting origin; a
        // redirect elsewhere (or to plain http) gets neither.
        if (sameOrigin) {
          if (!username.empty() && !password.empty()) http.setBasicAuth(username, password);
          for (const auto& h : headers) http.addHeader(h.first, h.second);
        }
        LOG_DBG("HTTP", "wolfSSL GET: %s (heap %u, max block %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
      },
      sink, [cancelFlag] { return cancelFlag && *cancelFlag; });
  if (bytesOut) *bytesOut = result.bytes;

  if (result.aborted) return HttpDownloader::ABORTED;
  if (result.stopped) return HttpDownloader::FILE_ERROR;
  if (result.status == 401 || result.status == 403) {
    LOG_ERR("HTTP", "wolfSSL request unauthorized: status %d: %s", result.status, url.c_str());
    return HttpDownloader::UNAUTHORIZED;
  }
  if (result.status < 200 || result.status >= 300) {
    LOG_ERR("HTTP", "wolfSSL request failed: status %d: %s", result.status, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (!result.complete) {
    LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", result.bytes, result.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

}  // namespace

HttpDownloader::TransferStats HttpDownloader::lastDownloadStats;

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  return fetchUrl(
      url, [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; }, username,
      password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  freeink::FetchSink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, {}, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers,
                                                             bool downgradeRedirectsToHttp) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());
  lastDownloadStats = {};

  // Stage in <dest>.part: a failed or cancelled download never replaces an
  // existing copy, and a partial file never sits under the real name.
  const std::string partPath = destPath + ".part";
  Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  freeink::FetchSink sink;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the server restarted the body from byte 0.
  sink.rewind = [&file, &partPath] {
    file.close();
    return Storage.openFileForWrite("HTTP", partPath.c_str(), file);
  };
  sink.progress = progress;

  const unsigned long startMs = millis();
  size_t downloaded = 0;
  const DownloadError result =
      runGetSecure(url, username, password, headers, sink, cancelFlag, &downloaded, downgradeRedirectsToHttp);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove. A failed rewind leaves no open handle.
  // close() does the final SD flush and directory-entry write, so its failure
  // means a truncated file.
  bool closed = true;
  if (file.isOpen()) closed = file.close();

  if (result != OK) {
    Storage.remove(partPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }
  if (!closed) {
    LOG_ERR("HTTP", "close/sync failed after %zu bytes", downloaded);
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  // Verify what actually landed on SD against what was received.
  size_t onDisk = 0;
  {
    HalFile check;
    if (Storage.openFileForRead("HTTP", partPath.c_str(), check)) onDisk = check.fileSize();
  }
  if (onDisk != downloaded) {
    LOG_ERR("HTTP", "SD size mismatch: %zu on disk, %zu received", onDisk, downloaded);
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  if (!Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "Failed to move download into place: %s", destPath.c_str());
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  const unsigned long ms = millis() - startMs;
  LOG_INF("HTTP", "Downloaded %zu bytes in %lu ms, %u KB/s", downloaded, ms,
          ms > 0 ? static_cast<unsigned>(downloaded / ms) : 0u);
  lastDownloadStats = {downloaded, static_cast<uint32_t>(ms)};
  return OK;
}
