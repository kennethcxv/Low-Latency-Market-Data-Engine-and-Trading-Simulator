cmake_minimum_required(VERSION 3.22)
project(market_data_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MDE_BUILD_TESTS "Build unit and integration tests" ON)
option(MDE_BUILD_FUZZ  "Build libFuzzer targets (upstream clang only)" OFF)
option(MDE_WERROR      "Treat warnings as errors" OFF)
set(MDE_SANITIZE "" CACHE STRING "Comma-separated -fsanitize= list, e.g. address,undefined or thread")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Sanitizers apply to everything, including GoogleTest, so instrumented and uninstrumented code
# never share containers (required for TSan correctness, and avoids ASan container false alarms).
if(MDE_SANITIZE)
  add_compile_options(-fsanitize=${MDE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${MDE_SANITIZE})
endif()

if(APPLE)
  # Apple's ld warns when a static library is reached through two dependency paths; harmless.
  add_link_options(LINKER:-no_warn_duplicate_libraries)
endif()

add_library(mde_warnings INTERFACE)
target_compile_options(mde_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wnon-virtual-dtor -Wold-style-cast
  $<$<BOOL:${MDE_WERROR}>:-Werror>)

add_subdirectory(src)
add_subdirectory(apps)

if(MDE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MDE_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
