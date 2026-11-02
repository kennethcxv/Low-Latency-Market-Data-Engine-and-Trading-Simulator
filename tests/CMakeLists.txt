add_executable(mde_tests
  test_common.cpp
  test_itch_codec.cpp
  test_itch_golden.cpp
  test_itch_malformed.cpp
  test_reference_market.cpp
  test_book_builder.cpp
  test_synth.cpp)
target_link_libraries(mde_tests PRIVATE mde_book mde_synth mde_itch mde_warnings GTest::gtest_main)

include(GoogleTest)
# GCC's ASan stack-use-after-return instrumentation made the property tests ~300x slower
# (measured: 119 ms -> 36 s for one test with GCC 14). Clang keeps the check.
set(test_env "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND MDE_SANITIZE MATCHES "address")
  set(test_env "ASAN_OPTIONS=detect_stack_use_after_return=0")
endif()
gtest_discover_tests(mde_tests DISCOVERY_MODE PRE_TEST PROPERTIES ENVIRONMENT "${test_env}")
