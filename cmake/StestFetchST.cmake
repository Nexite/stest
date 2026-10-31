# ST sources used to build firmware: STM32L4 HAL driver, CMSIS device L4, CMSIS-Core.
#
# Each can be pointed at an existing checkout via a cache variable; if empty, the
# source archive of a pinned commit is fetched with FetchContent (SHA-256 checked).
# The commits are the ones the reference firmware project pins as submodules.
#
#   STEST_STM32L4_HAL_DIR           stm32l4xx_hal_driver checkout (Inc/, Src/)
#   STEST_STM32L4_CMSIS_DEVICE_DIR  cmsis_device_l4 checkout (Include/, Source/Templates/)
#   STEST_CMSIS_CORE_DIR            CMSIS-Core: a dir containing core_cm4.h, or a
#                                   checkout whose Include/ contains it
#
# Functions:
#   stest_st_family_dirs(<family> <hal_dir_var> <device_dir_var>)   (family: stm32l4)
#   stest_cmsis_core_include(<include_dir_var>)
include_guard(GLOBAL)
include(FetchContent)

set(STEST_STM32L4_HAL_DIR "" CACHE PATH "stm32l4xx_hal_driver checkout (fetched if empty)")
set(STEST_STM32L4_CMSIS_DEVICE_DIR "" CACHE PATH "cmsis_device_l4 checkout (fetched if empty)")
set(STEST_CMSIS_CORE_DIR "" CACHE PATH "CMSIS-Core dir with core_cm4.h, or checkout with Include/ (fetched if empty)")

# repo | commit | sha256 of https://github.com/STMicroelectronics/<repo>/archive/<commit>.tar.gz
set(_STEST_ST_PINS
  "stm32l4xx_hal_driver|047a8610c7dc0dcfdb4cd63b86cd3a2cbd3e5899|1dc2a7d8c9a1fd4b3d7c0d8fb7fe108b36ba8873b2d20ae2e0371f69cb833c06"
  "cmsis_device_l4|dc04e41afbd7c204cd09f2fa5dd29658efbaff6c|d6421a4bb6ec22f0294d840d441054265c36f415b82fc1dd7fe1a3187fa8f2e4"
  "cmsis_core|1a2f783395dd72778a98ebe7771f36ca1c30a35e|0dd7b9316c8ff157479d8a7ee21c2734f7ef543ffbd6bf680fdba631d2381853")

# _stest_st_source(<repo> <out_var>): source dir of the pinned archive (fetched once).
function(_stest_st_source repo out_var)
  foreach(pin IN LISTS _STEST_ST_PINS)
    string(REPLACE "|" ";" pin "${pin}")
    list(GET pin 0 r)
    if(r STREQUAL repo)
      list(GET pin 1 commit)
      list(GET pin 2 sha)
    endif()
  endforeach()
  if(NOT commit)
    message(FATAL_ERROR "stest: no pinned commit for ${repo}")
  endif()
  set(dep "stest_st_${repo}")
  FetchContent_Declare(${dep}
    URL "https://github.com/STMicroelectronics/${repo}/archive/${commit}.tar.gz"
    URL_HASH SHA256=${sha}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _stest_no_cmake)  # never add_subdirectory() ST sources
  FetchContent_MakeAvailable(${dep})
  FetchContent_GetProperties(${dep} SOURCE_DIR src)
  set(${out_var} "${src}" PARENT_SCOPE)
endfunction()

function(stest_st_family_dirs family hal_var device_var)
  if(NOT family STREQUAL "stm32l4")
    message(FATAL_ERROR "stest: no ST sources known for family '${family}'")
  endif()
  if(STEST_STM32L4_HAL_DIR)
    set(hal "${STEST_STM32L4_HAL_DIR}")
  else()
    _stest_st_source(stm32l4xx_hal_driver hal)
  endif()
  if(STEST_STM32L4_CMSIS_DEVICE_DIR)
    set(dev "${STEST_STM32L4_CMSIS_DEVICE_DIR}")
  else()
    _stest_st_source(cmsis_device_l4 dev)
  endif()
  if(NOT EXISTS "${hal}/Inc/stm32l4xx_hal.h")
    message(FATAL_ERROR "stest: ${hal} is not an stm32l4xx_hal_driver checkout")
  endif()
  if(NOT EXISTS "${dev}/Include/stm32l4xx.h")
    message(FATAL_ERROR "stest: ${dev} is not a cmsis_device_l4 checkout")
  endif()
  set(${hal_var} "${hal}" PARENT_SCOPE)
  set(${device_var} "${dev}" PARENT_SCOPE)
endfunction()

function(stest_cmsis_core_include out_var)
  if(STEST_CMSIS_CORE_DIR)
    set(dir "${STEST_CMSIS_CORE_DIR}")
  else()
    _stest_st_source(cmsis_core dir)
  endif()
  if(EXISTS "${dir}/core_cm4.h")
    set(inc "${dir}")
  elseif(EXISTS "${dir}/Include/core_cm4.h")
    set(inc "${dir}/Include")
  else()
    message(FATAL_ERROR "stest: no core_cm4.h in ${dir} or ${dir}/Include")
  endif()
  # Stage the CMSIS-Core headers WITHOUT the toolchain headers (cmsis_compiler.h,
  # cmsis_gcc.h, ...): core_cm4.h does `#include "cmsis_compiler.h"`, and a quoted
  # include searches the includer's own directory before any -I path, so the host
  # shim in chips/cmsis_host would otherwise be bypassed.
  string(MD5 key "${inc}")
  set(stage "${CMAKE_BINARY_DIR}/_stest/cmsis_core_${key}")
  file(GLOB hdrs "${inc}/*.h")
  list(FILTER hdrs EXCLUDE REGEX "/cmsis_(compiler|gcc|clang|armcc|armclang|armclang_ltm|iccarm|tiarmclang)\\.h$")
  file(COPY ${hdrs} DESTINATION "${stage}")
  set(${out_var} "${stage}" PARENT_SCOPE)
endfunction()
