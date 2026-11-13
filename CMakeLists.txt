cmake_minimum_required(VERSION 3.23)
# stest is either the top-level project or a subproject of a consumer's test repo:
#   include(<stest>/cmake/StestToolchain.cmake)   # before project()
#   project(my_sil C CXX)
#   add_subdirectory(<stest> stest)
#   stest_add_firmware(...)
# As a subproject the compilers are already chosen, so the consumer must have included the
# toolchain file before its own project() (checked here, before this file includes it).
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  get_property(_stest_toolchain GLOBAL PROPERTY STEST_TOOLCHAIN_INCLUDED)
  if(NOT _stest_toolchain)
    message(FATAL_ERROR "stest: include(${CMAKE_CURRENT_LIST_DIR}/cmake/StestToolchain.cmake) "
            "before project() in your top-level CMakeLists.txt (it selects the upstream clang "
            "the MMIO pass and firmware need).")
  endif()
else()
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/StestToolchain.cmake)
endif()
project(stest LANGUAGES C CXX)

# stest's source root, for its own CMake files and for stest_add_firmware() called from a
# consumer's scope.
get_filename_component(_stest_root "${CMAKE_CURRENT_LIST_DIR}" REALPATH)
set(STEST_ROOT_DIR "${_stest_root}" CACHE INTERNAL "stest source root")
# Self-tests, unit tests and the speed-gate spike: built and registered with CTest only when
# stest is the top-level project, unless asked for.
option(STEST_BUILD_TESTS "Build and register stest's own tests (spike/, selftest/, unit tests)"
       ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(STEST_BUILD_TESTS)
  enable_testing()
endif()
include(${CMAKE_CURRENT_LIST_DIR}/cmake/stest_add_firmware.cmake)  # + StestFetchST
add_subdirectory(compiler/mmio_pass)
if(STEST_BUILD_TESTS)
  add_subdirectory(spike)
endif()
add_subdirectory(runtime)
add_subdirectory(chips)
if(STEST_BUILD_TESTS)
  add_subdirectory(selftest)
endif()
