add_executable(TailnetH2StreamTest
  TailnetH2StreamTest.cpp
  ${REPO_ROOT}/lib/MicroLink/src/ml_h2_stream.c
  ${REPO_ROOT}/lib/MicroLink/src/ml_map_filter.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)
target_include_directories(TailnetH2StreamTest PRIVATE
  ${REPO_ROOT}/lib/MicroLink/include
  ${REPO_ROOT}/lib/JsonParser
)
target_link_libraries(TailnetH2StreamTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TailnetH2StreamTest)
