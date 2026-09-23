add_executable(OpdsFeedCacheTest
  OpdsFeedCacheTest.cpp
)

target_link_libraries(OpdsFeedCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsFeedCacheTest)
