#pragma once

// Header-only parser for the GET /syncs/progress/:document response body, so
// the host gtest suite can cover it without the HTTP/credential layers.

#include <ArduinoJson.h>

#include <cstdint>
#include <string>
#include <utility>

#include "KOReaderSyncClient.h"

namespace koreader_sync {

// Absent or non-string fields read as "" (JsonVariant::as<std::string>() would
// serialize a missing key to the literal "null").
inline std::string stringOr(JsonVariantConst v) {
  const char* s = v.as<const char*>();
  return s ? std::string(s) : std::string();
}

// Fills `out` from a 2xx response body. Returns false on malformed JSON and,
// when `error` is given, points it at ArduinoJson's static reason string.
// `out.document` is not in the body; the caller sets it. An empty object `{}`
// (the reference server's "no progress yet" answer) parses as zero progress.
// The rich `position` object is only honoured for crosspoint-sync servers;
// stock KOSync servers never send it and a stray one must not be trusted.
inline bool parseProgressResponse(const char* json, bool crossPointServer, KOReaderProgress& out,
                                  const char** error = nullptr) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    if (error) *error = err.c_str();
    return false;
  }

  out.progress = stringOr(doc["progress"]);
  out.percentage = doc["percentage"].as<float>();
  out.device = stringOr(doc["device"]);
  out.deviceId = stringOr(doc["device_id"]);
  out.timestamp = doc["timestamp"].as<int64_t>();

  out.position.reset();
  if (crossPointServer) {
    const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
    if (!pos.isNull()) {
      KOReaderRichPosition rich;
      rich.pctQ = pos["pctQ"].as<uint32_t>();
      rich.spineIndex = pos["spine"].as<uint16_t>();
      rich.pageNumber = pos["page"].as<uint16_t>();
      const uint16_t pages = pos["pages"].as<uint16_t>();
      rich.totalPages = pages > 0 ? pages : 1;
      const uint16_t para = pos["para"].as<uint16_t>();
      if (para > 0) rich.paragraphIndex = para;
      rich.xpath = stringOr(pos["xpath"]);
      out.position = std::move(rich);
    }
  }
  return true;
}

}  // namespace koreader_sync
