add_executable(mde_bench mde_bench.cpp)
target_link_libraries(mde_bench PRIVATE mde_book mde_synth mde_moldudp64 mde_pipeline mde_sim mde_itch mde_warnings)
# Recorded in the report so results can be tied to the exact flags.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
target_compile_definitions(mde_bench PRIVATE
  MDE_BENCH_FLAGS="${CMAKE_BUILD_TYPE}: ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}}")
