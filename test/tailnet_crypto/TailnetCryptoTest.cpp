#include <gtest/gtest.h>

#include <array>
#include <cstring>

extern "C" {
#include "crypto/refc/chacha20poly1305.h"
}

TEST(TailnetCrypto, AuthenticatedDecryptSupportsSameInputAndOutputBuffer) {
  constexpr char plaintext[] = "fragmented tailscale map payload";
  std::array<uint8_t, sizeof(plaintext) - 1 + 16> buffer{};
  std::array<uint8_t, 32> key{};
  for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(i * 7 + 3);

  chacha20poly1305_encrypt(buffer.data(), reinterpret_cast<const uint8_t*>(plaintext), sizeof(plaintext) - 1,
                           nullptr, 0, 42, key.data());
  ASSERT_TRUE(chacha20poly1305_decrypt(buffer.data(), buffer.data(), buffer.size(), nullptr, 0, 42, key.data()));
  EXPECT_EQ(std::memcmp(buffer.data(), plaintext, sizeof(plaintext) - 1), 0);
}

TEST(TailnetCrypto, InPlaceDecryptRejectsModifiedCiphertext) {
  std::array<uint8_t, 32> buffer{};
  std::array<uint8_t, 32> key{};
  const std::array<uint8_t, 16> plaintext{};
  chacha20poly1305_encrypt(buffer.data(), plaintext.data(), plaintext.size(), nullptr, 0, 7, key.data());
  buffer[3] ^= 0x80;
  EXPECT_FALSE(chacha20poly1305_decrypt(buffer.data(), buffer.data(), buffer.size(), nullptr, 0, 7, key.data()));
}
