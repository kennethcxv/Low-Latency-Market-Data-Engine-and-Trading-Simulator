add_executable(mde mde/main.cpp)
target_link_libraries(mde PRIVATE mde_book mde_synth mde_net mde_pipeline mde_itch mde_warnings)
