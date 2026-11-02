add_executable(mde_tests
  test_common.cpp
  test_itch_codec.cpp)
target_link_libraries(mde_tests PRIVATE mde_itch mde_warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mde_tests DISCOVERY_MODE PRE_TEST)
