# Out-of-tree consumer of stest (selftest_out_of_tree_ok): the supported pattern for a
# user's test repo. stest is referenced by path (STEST_DIR, as a git submodule would be).
cmake_minimum_required(VERSION 3.23)
set(STEST_DIR "" CACHE PATH "stest source root")
include(${STEST_DIR}/cmake/StestToolchain.cmake)   # before project(): selects upstream clang
project(my_sil C CXX)
add_subdirectory(${STEST_DIR} stest)

set(_bl ${STEST_DIR}/selftest/firmware/blinky/Core)
stest_add_firmware(blinky
  CHIP STM32L496RG
  SOURCES ${_bl}/Src/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
          ${_bl}/Src/vars.c ${_bl}/Src/vars2.c
  INCLUDE_DIRS ${_bl}/Inc)

enable_testing()
add_test(NAME consumer_smoke COMMAND blinky --run-ms 600)
set_tests_properties(consumer_smoke PROPERTIES TIMEOUT 30)
