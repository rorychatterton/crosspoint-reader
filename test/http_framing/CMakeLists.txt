# freeink::SecureHttpClient (header-only) over a scripted transport. The
# stubs directory stands in for Arduino.h / Client.h / WiFiClient.h / base64.h;
# the real SecureClient.h is used and its out-of-line members are stubbed.
add_executable(HttpFramingTest
  HttpFramingTest.cpp
  SecureClientStubs.cpp
)

target_include_directories(HttpFramingTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/include
)

target_link_libraries(HttpFramingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HttpFramingTest)
