add_executable(TailnetCryptoTest
  TailnetCryptoTest.cpp
  ${REPO_ROOT}/lib/WireGuardLwip/src/crypto.c
  ${REPO_ROOT}/lib/WireGuardLwip/src/crypto/refc/chacha20.c
  ${REPO_ROOT}/lib/WireGuardLwip/src/crypto/refc/chacha20poly1305.c
  ${REPO_ROOT}/lib/WireGuardLwip/src/crypto/refc/poly1305-donna.c
)
target_include_directories(TailnetCryptoTest PRIVATE ${REPO_ROOT}/lib/WireGuardLwip/src)
target_link_libraries(TailnetCryptoTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TailnetCryptoTest)
