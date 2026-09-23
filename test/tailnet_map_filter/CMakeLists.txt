add_executable(TailnetMapFilterTest
  TailnetMapFilterTest.cpp
  ${REPO_ROOT}/lib/MicroLink/src/ml_map_filter.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)
target_include_directories(TailnetMapFilterTest PRIVATE
  ${REPO_ROOT}/lib/MicroLink/include
  ${REPO_ROOT}/lib/JsonParser
)
target_link_libraries(TailnetMapFilterTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TailnetMapFilterTest)

add_executable(MapFilterTest
  MapFilterTest.cpp
  ${REPO_ROOT}/lib/MicroLink/src/ml_map_filter.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)
target_include_directories(MapFilterTest PRIVATE
  ${REPO_ROOT}/lib/MicroLink/include
  ${REPO_ROOT}/lib/JsonParser
)
target_link_libraries(MapFilterTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MapFilterTest)
