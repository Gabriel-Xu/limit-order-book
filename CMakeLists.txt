cmake_minimum_required(VERSION 3.25)
project(limit_order_book LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(LOB_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(LOB_NATIVE "Optimize for this machine's CPU (-march=native)" ON)
option(LOB_BUILD_TESTS "Build the test suite" ON)
option(LOB_BUILD_BENCH "Build the benchmark" ON)

# Header-only order book library.
add_library(lob INTERFACE)
target_include_directories(lob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# Warnings and codegen flags applied to our own targets only, not GoogleTest.
add_library(lob_options INTERFACE)
target_compile_options(lob_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
if(LOB_NATIVE)
  target_compile_options(lob_options INTERFACE -march=native)
endif()
if(LOB_SANITIZE)
  target_compile_options(lob_options INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(lob_options INTERFACE -fsanitize=address,undefined)
endif()

if(LOB_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/releases/download/v1.18.0/googletest-1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    # Stamp extracted files with the extraction time, not the archive's, so a
    # version bump always rebuilds GoogleTest instead of linking stale objects.
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SYSTEM)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(lob_tests
    tests/order_book_test.cpp
    tests/differential_test.cpp
    tests/ladder_book_test.cpp)
  target_include_directories(lob_tests PRIVATE tests)
  target_link_libraries(lob_tests PRIVATE lob lob_options GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(lob_tests)
endif()

if(LOB_BUILD_BENCH)
  add_executable(lob_bench bench/bench.cpp)
  target_link_libraries(lob_bench PRIVATE lob lob_options)
endif()
