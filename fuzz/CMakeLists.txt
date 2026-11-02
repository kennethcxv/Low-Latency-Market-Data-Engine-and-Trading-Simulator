# Built only with upstream clang; Apple clang does not ship the libFuzzer runtime.
foreach(target fuzz_itch)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE mde_itch mde_warnings)
  target_compile_options(${target} PRIVATE -fsanitize=fuzzer)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endforeach()
