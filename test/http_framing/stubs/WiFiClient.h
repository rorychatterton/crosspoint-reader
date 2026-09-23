#pragma once
// Scripted fake transport. SecureHttpClient uses WiFiClient for plain-http
// URLs, so a test loads a canned response into the script, issues a GET on an
// http:// URL, and inspects how the client framed the body.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "Client.h"

struct FakeSocketScript {
  std::string response;      // bytes the "server" sends, in order
  bool closeAfter = true;    // peer closes once the response is drained
  std::string lastRequest;   // captured request bytes (for assertions)
  int connectCalls = 0;
};

inline FakeSocketScript& fakeSocket() {
  static FakeSocketScript script;
  return script;
}

class WiFiClient : public Client {
 public:
  int connect(IPAddress, uint16_t) override { return openFromScript(); }
  int connect(const char*, uint16_t) override { return openFromScript(); }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t size) override {
    if (!_open) return 0;
    fakeSocket().lastRequest.append(reinterpret_cast<const char*>(buf), size);
    return size;
  }
  int available() override { return _open ? static_cast<int>(_rx.size() - _pos) : 0; }
  int read() override {
    if (!_open || _pos >= _rx.size()) return -1;
    return static_cast<uint8_t>(_rx[_pos++]);
  }
  int read(uint8_t* buf, size_t size) override {
    if (!_open || _pos >= _rx.size()) return -1;
    const size_t n = std::min(size, _rx.size() - _pos);
    for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(_rx[_pos + i]);
    _pos += n;
    return static_cast<int>(n);
  }
  int peek() override { return (_open && _pos < _rx.size()) ? static_cast<uint8_t>(_rx[_pos]) : -1; }
  void flush() override {}
  void stop() override { _open = false; }
  uint8_t connected() override {
    if (!_open) return 0;
    if (_pos < _rx.size()) return 1;
    return fakeSocket().closeAfter ? 0 : 1;
  }
  operator bool() override { return connected() != 0; }
  void setConnectionTimeout(uint32_t) {}

 private:
  int openFromScript() {
    ++fakeSocket().connectCalls;
    _rx = fakeSocket().response;
    _pos = 0;
    _open = true;
    return 1;
  }
  std::string _rx;
  size_t _pos = 0;
  bool _open = false;
};
