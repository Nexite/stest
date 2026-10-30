cmake_minimum_required(VERSION 3.20)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/StestToolchain.cmake)
project(stest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

enable_testing()
include(${CMAKE_CURRENT_LIST_DIR}/cmake/stest_add_firmware.cmake)  # + StestFetchST
add_subdirectory(compiler/mmio_pass)
add_subdirectory(spike)
add_subdirectory(runtime)
add_subdirectory(chips)
add_subdirectory(selftest)
