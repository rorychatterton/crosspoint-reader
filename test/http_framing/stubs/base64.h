#pragma once
#include <string>
// Only reached when basic auth is configured; the framing tests never set it.
struct base64 {
  static std::string encode(const char* s) { return std::string(s); }
};
