// Out-of-line SecureClient definitions so SecureHttpClient.h links on the host.
// The framing tests use plain http:// URLs, so the TLS path is never entered.
#include "SecureClient.h"

namespace freeink {

SecureClient::~SecureClient() = default;
void SecureClient::setCACert(const char* rootCA) { _rootCA = rootCA; }
void SecureClient::setInsecure() { _insecure = true; }
int SecureClient::connect(IPAddress, uint16_t) { return 0; }
int SecureClient::connect(const char*, uint16_t) { return 0; }
size_t SecureClient::write(uint8_t) { return 0; }
size_t SecureClient::write(const uint8_t*, size_t) { return 0; }
int SecureClient::available() { return 0; }
int SecureClient::read() { return -1; }
int SecureClient::read(uint8_t*, size_t) { return -1; }
int SecureClient::peek() { return -1; }
void SecureClient::flush() {}
void SecureClient::stop() { _connected = false; }
uint8_t SecureClient::connected() { return 0; }
bool SecureClient::tls13Available() { return false; }

}  // namespace freeink
