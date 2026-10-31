# stest self-test firmware: real CubeMX-style projects built with stest_add_firmware.
set(_bl ${CMAKE_CURRENT_SOURCE_DIR}/firmware/blinky/Core)
stest_add_firmware(blinky
  CHIP STM32L496RG
  SOURCES ${_bl}/Src/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_bl}/Inc)

add_test(NAME selftest_blinky_standalone
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:blinky>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_blinky.cmake)

# Same event log for different quantum sizes (poll fast-forward included).
add_test(NAME selftest_blinky_quantum_determinism
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:blinky>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_blinky_determinism.cmake)

# Speed budget: <= 0.4 s wall per virtual second (5 s virtual in <= 2.0 s).
add_test(NAME selftest_blinky_speed
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_speed.py
          $<TARGET_FILE:blinky> 5000 2.0)

# Fault-injection firmware: one executable per variant (faults_spin, faults_crash,
# faults_unmodelled). Uses no HAL, so the HAL archive is a single file; the HAL config
# header is borrowed from blinky.
stest_st_family_dirs(stm32l4 _st_hal _st_dev)
foreach(variant SPIN CRASH UNMODELLED)
  string(TOLOWER ${variant} v)
  stest_add_firmware(faults_${v}
    CHIP STM32L496RG
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/faults/main.c
    INCLUDE_DIRS ${_bl}/Inc
    DEFINES FAULT=${variant}
    HAL_SOURCES ${_st_hal}/Src/stm32l4xx_hal.c)
endforeach()

# Python harness tests (python/tests) against the self-test executables. Uses uv with a
# per-build-dir environment so native and container builds never share a venv.
find_program(STEST_UV uv)
if(STEST_UV)
  add_test(NAME python_tests
    COMMAND ${STEST_UV} run --project ${PROJECT_SOURCE_DIR} pytest ${PROJECT_SOURCE_DIR}/python/tests -q
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(python_tests PROPERTIES
    ENVIRONMENT "STEST_SELFTEST_BIN=${CMAKE_CURRENT_BINARY_DIR};UV_PROJECT_ENVIRONMENT=${CMAKE_BINARY_DIR}/py-venv"
    TIMEOUT 600)
else()
  message(WARNING "stest: uv not found; the python_tests CTest is not registered")
endif()
