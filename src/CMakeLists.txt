add_library(mde_common INTERFACE)
target_include_directories(mde_common INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(mde_common INTERFACE cxx_std_20)

add_library(mde_itch INTERFACE)
target_link_libraries(mde_itch INTERFACE mde_common)
