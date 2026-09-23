add_executable(TailnetStateTest
  TailnetStateTest.cpp
  ${REPO_ROOT}/lib/MicroLink/src/ml_state.c
  ${REPO_ROOT}/lib/MicroLink/src/ml_startup_policy.c
  ${REPO_ROOT}/lib/MicroLink/src/ml_workspace.c
)

target_include_directories(TailnetStateTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/MicroLink/include
)

target_link_libraries(TailnetStateTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TailnetStateTest)
