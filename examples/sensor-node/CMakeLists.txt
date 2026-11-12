# sensor-node: two STM32L496RG boards (sensor, display) built for stest.
#
# The pattern for your own repo: stest as a git submodule (STEST_DIR), its toolchain file
# included before project() so firmware is compiled with upstream clang, then stest added as
# a subdirectory, which provides stest_add_firmware().
cmake_minimum_required(VERSION 3.23)

set(STEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "stest source root (e.g. a submodule)")
include(${STEST_DIR}/cmake/StestToolchain.cmake)   # before project(): selects upstream clang
project(sensor_node C CXX)
add_subdirectory(${STEST_DIR} stest)

# Each board is a CubeMX project: Core/Src and Core/Inc (with stm32l4xx_hal_conf.h). stest
# compiles these sources and ST's HAL, unchanged, into one host executable per board.
foreach(board sensor display)
  set(_core ${CMAKE_CURRENT_SOURCE_DIR}/firmware/${board}/Core)
  stest_add_firmware(${board}
    CHIP STM32L496RG
    SOURCES ${_core}/Src/main.c ${_core}/Src/stm32l4xx_it.c ${_core}/Src/stm32l4xx_hal_msp.c
    INCLUDE_DIRS ${_core}/Inc)
endforeach()
