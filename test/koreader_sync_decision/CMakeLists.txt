# ArduinoJson (header-only) for the progress-response parser test. Prefer the
# copy PlatformIO already fetched for the firmware build; fall back to
# FetchContent (CI has no .pio/). Keep the tag in step with platformio.ini.
set(ARDUINOJSON_LOCAL_SRC "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src")
if(EXISTS "${ARDUINOJSON_LOCAL_SRC}/ArduinoJson.h")
  add_library(arduinojson_host INTERFACE)
  target_include_directories(arduinojson_host INTERFACE "${ARDUINOJSON_LOCAL_SRC}")
else()
  FetchContent_Declare(
    arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
  )
  FetchContent_MakeAvailable(arduinojson)
  add_library(arduinojson_host INTERFACE)
  target_link_libraries(arduinojson_host INTERFACE ArduinoJson)
endif()

add_executable(KOReaderSyncDecisionTest
  KOReaderSyncDecisionTest.cpp
  ProgressResponseParserTest.cpp
)

target_include_directories(KOReaderSyncDecisionTest PRIVATE
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(KOReaderSyncDecisionTest PRIVATE
  crosspoint_test_common
  arduinojson_host
  GTest::gtest_main
)

gtest_discover_tests(KOReaderSyncDecisionTest)
