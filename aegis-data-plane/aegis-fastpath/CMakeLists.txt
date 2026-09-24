cmake_minimum_required(VERSION 3.16)
project(aegis_fastpath CXX)

# Aegis Fastpath builds with CMake + pkg-config rather than the Aegis Engine
# Bazel workspace: DPDK is not in the Bazel Central Registry and ships a
# libdpdk.pc, so pkg-config is the supported way to consume it.
#
# The limiter core (bucket, flow table, parser, config) has no DPDK dependency,
# so tests and the micro-benchmark build anywhere, including macOS. The
# aegis-fastpath binary is only built when libdpdk is found (Linux).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(AEGIS_FASTPATH_BUILD_TESTS "Build unit tests (fetches googletest)" ON)
option(AEGIS_FASTPATH_NATIVE "Compile with -march=native" ON)

add_library(fastpath_core STATIC src/fastpath_config.cc)
target_include_directories(fastpath_core PUBLIC src)
target_compile_options(fastpath_core PUBLIC -Wall -Wextra)
if(AEGIS_FASTPATH_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native HAS_MARCH_NATIVE)
  if(HAS_MARCH_NATIVE)
    target_compile_options(fastpath_core PUBLIC -march=native)
  endif()
endif()

# ── DPDK application ──────────────────────────────────────────────────────────
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(DPDK IMPORTED_TARGET libdpdk)
endif()
if(DPDK_FOUND)
  message(STATUS "DPDK ${DPDK_VERSION} found: building aegis-fastpath")
  add_executable(aegis-fastpath src/fastpath_main.cc)
  target_link_libraries(aegis-fastpath PRIVATE fastpath_core PkgConfig::DPDK)
else()
  message(STATUS "libdpdk not found: skipping aegis-fastpath (tests and bench still build)")
endif()

# ── Micro-benchmark (no NIC needed) ───────────────────────────────────────────
add_executable(limiter_bench bench/limiter_bench.cc)
target_link_libraries(limiter_bench PRIVATE fastpath_core)

# ── Unit tests ────────────────────────────────────────────────────────────────
if(AEGIS_FASTPATH_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(fastpath_tests
    tests/packet_token_bucket_test.cc
    tests/flow_table_test.cc
    tests/packet_parse_test.cc
    tests/source_limiter_test.cc
    tests/fastpath_config_test.cc)
  target_link_libraries(fastpath_tests PRIVATE fastpath_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(fastpath_tests)
endif()
