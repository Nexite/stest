# stest self-test firmware: real CubeMX-style projects built with stest_add_firmware.
set(_bl ${CMAKE_CURRENT_SOURCE_DIR}/firmware/blinky/Core)
stest_add_firmware(blinky
  CHIP STM32L496RG
  SOURCES ${_bl}/Src/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
          ${_bl}/Src/vars.c ${_bl}/Src/vars2.c
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
# faults_unmodelled, faults_ro_write, faults_clkoff). Uses no HAL, so the HAL archive is a single file; the HAL config
# header is borrowed from blinky.
stest_st_family_dirs(stm32l4 _st_hal _st_dev)
foreach(variant SPIN CRASH UNMODELLED RO_WRITE CLKOFF)
  string(TOLOWER ${variant} v)
  stest_add_firmware(faults_${v}
    CHIP STM32L496RG
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/faults/main.c
    INCLUDE_DIRS ${_bl}/Inc
    DEFINES FAULT=${variant}
    HAL_SOURCES ${_st_hal}/Src/stm32l4xx_hal.c)
endforeach()

# EXTI: HAL_GPIO_EXTI_Callback driven by PC13 rising edges (blinky's CubeMX scaffolding).
stest_add_firmware(exti
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/exti/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_bl}/Inc)

# TIM: CubeMX-style TIM7 20 kHz time base driving a software PWM on PA6 (TIM module enabled
# in its own HAL config; handlers/MSP from blinky).
stest_add_firmware(tim
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/tim/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/firmware/tim/Inc ${_bl}/Inc)

# ADC: CubeMX-style ADC1 channel 16 (PB1) single conversions every 1 ms. Variants (one exe
# each): adc (synchronous HCLK/4), adc_async (asynchronous SYSCLK), adc_async_noclk
# (asynchronous, no RCC ADCSEL: aborts), adc_scan (two ranks: aborts). ADC + DAC HAL modules
# enabled in adc/Inc; MSP/handlers from blinky.
set(_adc ${CMAKE_CURRENT_SOURCE_DIR}/firmware/adc)
foreach(variant SYNC ASYNC ASYNC_NOCLK SCAN)
  string(TOLOWER ${variant} v)
  if(variant STREQUAL "SYNC")
    set(_exe adc)
  else()
    set(_exe adc_${v})
  endif()
  stest_add_firmware(${_exe}
    CHIP STM32L496RG
    SOURCES ${_adc}/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
    INCLUDE_DIRS ${_adc}/Inc ${_bl}/Inc
    DEFINES ADC_VARIANT=ADC_${variant})
endforeach()

# DAC: CubeMX-style DAC1 channel 1 (PA4) set to 2048 and started.
stest_add_firmware(dac
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/dac/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_adc}/Inc ${_bl}/Inc)

# Poll fast-forward regression firmware (counting loop over a constant volatile read).
stest_add_firmware(pollcount
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/pollcount/main.c
  INCLUDE_DIRS ${_bl}/Inc
  HAL_SOURCES ${_st_hal}/Src/stm32l4xx_hal.c)

# Configure-time checks of stest_add_firmware (compiler identity, CMSIS include dirs).
foreach(case bad_include bad_compiler)
  add_test(NAME selftest_configure_${case}
    COMMAND ${CMAKE_COMMAND} -DCASE=${case} -DSTEST_ROOT=${PROJECT_SOURCE_DIR}
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cmake_checks_${case}
            -DSTEST_CLANG=${STEST_CLANG} -DSTEST_LLVM_ROOT=${STEST_LLVM_ROOT}
            -DLLVM_DIR=${LLVM_DIR}
            -DCMAKE_BIN=${CMAKE_COMMAND} -DGEN=${CMAKE_GENERATOR}
            -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DSTEST_CLANGXX=${STEST_CLANGXX}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_configure_fails.cmake)
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
    TIMEOUT 300)
else()
  message(WARNING "stest: uv not found; the python_tests CTest is not registered")
endif()

# Every CTest that runs a firmware/node executable gets a 30 s timeout.
set_tests_properties(selftest_blinky_standalone selftest_blinky_quantum_determinism
  selftest_blinky_speed PROPERTIES TIMEOUT 30)
