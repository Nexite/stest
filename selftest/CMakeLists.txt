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
# faults_unmodelled, faults_ro_write, faults_clkoff, faults_printf, faults_abort_output, faults_partial_end,
# faults_abort_events). Uses no HAL, so the HAL archive is a single file; the HAL config
# header is borrowed from blinky.
stest_st_family_dirs(stm32l4 _st_hal _st_dev)
foreach(variant SPIN CRASH UNMODELLED RO_WRITE CLKOFF PRINTF ABORT_OUTPUT PARTIAL_END ABORT_EVENTS)
  string(TOLOWER ${variant} v)
  stest_add_firmware(faults_${v}
    CHIP STM32L496RG
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/faults/main.c
    INCLUDE_DIRS ${_bl}/Inc
    DEFINES FAULT=${variant}
    HAL_SOURCES ${_st_hal}/Src/stm32l4xx_hal.c)
endforeach()

# Standalone strict abort must still print the aborting quantum's output (printf lines, ITM,
# partial fragments) as EVT lines before exiting 70.
add_test(NAME selftest_standalone_abort_output
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:faults_abort_output>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_abort_output.cmake)
add_test(NAME selftest_standalone_partial_end
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:faults_partial_end>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_partial_end.cmake)
set_tests_properties(selftest_standalone_abort_output selftest_standalone_partial_end PROPERTIES TIMEOUT 30)

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

# SPI: CubeMX-style SPI1 master (mode 0, 8-bit, /16, soft NSS, CS on PA4) transferring
# {A5 01 02 03} every 10 ms. Variants: spi (HAL_SPI_TransmitReceive), spi_tx
# (HAL_SPI_Transmit). SPI HAL module enabled in spi/Inc; MSP/handlers from blinky.
set(_spi ${CMAKE_CURRENT_SOURCE_DIR}/firmware/spi)
foreach(variant TXRX TX)
  string(TOLOWER ${variant} v)
  if(variant STREQUAL "TXRX")
    set(_exe spi)
  else()
    set(_exe spi_${v})
  endif()
  stest_add_firmware(${_exe}
    CHIP STM32L496RG
    SOURCES ${_spi}/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
    INCLUDE_DIRS ${_spi}/Inc ${_bl}/Inc
    DEFINES SPI_VARIANT=SPI_${variant})
endforeach()

# CAN: CubeMX-style CAN2 node at 500 kbit/s (CAN1 clock for the shared filters; 16-bit list +
# 32-bit mask banks; FIFO0 interrupt; id 0x100 + NODE_ID every 10 ms). One exe per node id
# (can_node1, can_node2) plus can_badrate (node 1 with Prescaler 20: 250 kbit/s), can_err (node 1
# with the error notifications and CAN2_SCE interrupt) and can_abom (can_err with AutoBusOff).
# CAN HAL module enabled in can/Inc; handlers from blinky.
set(_can ${CMAKE_CURRENT_SOURCE_DIR}/firmware/can)
foreach(variant node1 node2 badrate err abom)
  if(variant STREQUAL "node2")
    set(_defs NODE_ID=2)
  elseif(variant STREQUAL "badrate")
    set(_defs NODE_ID=1 CAN_PRESCALER=20)
  elseif(variant STREQUAL "err")
    set(_defs NODE_ID=1 CAN_ERR_NOTIFY=1)
  elseif(variant STREQUAL "abom")
    set(_defs NODE_ID=1 CAN_ERR_NOTIFY=1 CAN_ABOM=1)
  else()
    set(_defs NODE_ID=1)
  endif()
  stest_add_firmware(can_${variant}
    CHIP STM32L496RG
    SOURCES ${_can}/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
    INCLUDE_DIRS ${_can}/Inc ${_bl}/Inc
    DEFINES ${_defs})
endforeach()

# ITM/DWT: CMSIS ITM_SendChar logging every 100 ms + DWT CYCCNT across HAL_Delay(9) (blinky
# scaffolding for clocks/handlers/MSP).
stest_add_firmware(itm
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/itm/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_bl}/Inc)
# itm_mixed: the same firmware logging through printf/stdio and ITM interleaved (Node.output).
stest_add_firmware(itm_mixed
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/itm/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_bl}/Inc
  DEFINES ITM_MIXED=1)

# HAL mode (MODELS GPIO=hal): stest's GPIO shim replaces ST's stm32l4xx_hal_gpio.c. blinky and
# exti rebuilt unchanged in hal mode (their event logs must match register mode), plus
# hal_direct, which also pokes GPIOA->BSRR directly (one warning, run continues).
stest_add_firmware(blinky_gpio_hal
  CHIP STM32L496RG
  SOURCES ${_bl}/Src/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
          ${_bl}/Src/vars.c ${_bl}/Src/vars2.c
  INCLUDE_DIRS ${_bl}/Inc
  MODELS GPIO=hal)
stest_add_firmware(exti_gpio_hal
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/exti/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_bl}/Inc
  MODELS GPIO=hal)
stest_add_firmware(hal_direct_gpio_hal
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/hal_direct/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_bl}/Inc
  MODELS GPIO=hal)

# Poll fast-forward regression firmware (counting loop over a constant volatile read).
stest_add_firmware(pollcount
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/pollcount/main.c
  INCLUDE_DIRS ${_bl}/Inc
  HAL_SOURCES ${_st_hal}/Src/stm32l4xx_hal.c)

# Tight polling of PA0 driven by timestamped inbox messages (python/tests/test_protocol_v2.py).
stest_add_firmware(pollpin
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/pollpin/main.c
  INCLUDE_DIRS ${_bl}/Inc
  HAL_SOURCES ${_st_hal}/Src/stm32l4xx_hal.c)

# WFI regression firmware: naive `for (;;) { __WFI(); <toggle PA0>; }` with a SysTick IRQ.
# WFI must not wake at quantum ends: the log is the same for any quantum size.
stest_add_firmware(wfiloop
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/wfiloop/main.c
  INCLUDE_DIRS ${_bl}/Inc
  HAL_SOURCES ${_st_hal}/Src/stm32l4xx_hal.c)
add_test(NAME selftest_wfiloop_quantum_determinism
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:wfiloop> -DRUN_MS=20 "-DQUANTA=1000;88;20"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_blinky_determinism.cmake)
set_tests_properties(selftest_wfiloop_quantum_determinism PROPERTIES TIMEOUT 30)

# Configure-time checks of stest_add_firmware (compiler identity, CMSIS include dirs,
# MODELS= types/modes).
foreach(case bad_include bad_compiler models_unknown models_unsupported)
  add_test(NAME selftest_configure_${case}
    COMMAND ${CMAKE_COMMAND} -DCASE=${case} -DSTEST_ROOT=${PROJECT_SOURCE_DIR}
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cmake_checks_${case}
            -DSTEST_CLANG=${STEST_CLANG} -DSTEST_LLVM_ROOT=${STEST_LLVM_ROOT}
            -DLLVM_DIR=${LLVM_DIR}
            -DCMAKE_BIN=${CMAKE_COMMAND} -DGEN=${CMAKE_GENERATOR}
            -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DSTEST_CLANGXX=${STEST_CLANGXX}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_configure_fails.cmake)
  # A nested configure (and, for some cases, the ST source fetch) can be slow on a cold cache.
  set_tests_properties(selftest_configure_${case} PROPERTIES TIMEOUT 120)
endforeach()

# Python harness tests (python/tests) against the self-test executables. Uses uv with a
# per-build-dir environment so native and container builds never share a venv.
# Wall-clock speed budgets (@pytest.mark.perf) are load-sensitive: under `ctest -j8` they share
# the CPU with the other tests (test_can_speed, sync-bound at the 88 us CAN quantum, reached
# 0.97 of its budget alongside the nested configures), so they run alone in python_perf_tests
# (RUN_SERIAL) and python_tests skips them. python/tests/test_perf_isolation.py checks both.
find_program(STEST_UV uv)
if(STEST_UV)
  set(_py_env "STEST_SELFTEST_BIN=${CMAKE_CURRENT_BINARY_DIR};UV_PROJECT_ENVIRONMENT=${CMAKE_BINARY_DIR}/py-venv")
  add_test(NAME python_tests
    COMMAND ${STEST_UV} run --project ${PROJECT_SOURCE_DIR} pytest ${PROJECT_SOURCE_DIR}/python/tests -q
            -m "not perf"
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(python_tests PROPERTIES ENVIRONMENT "${_py_env}" TIMEOUT 300)
  add_test(NAME python_perf_tests
    COMMAND ${STEST_UV} run --project ${PROJECT_SOURCE_DIR} pytest ${PROJECT_SOURCE_DIR}/python/tests -q
            -m perf
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(python_perf_tests PROPERTIES ENVIRONMENT "${_py_env}" TIMEOUT 300 RUN_SERIAL TRUE)
else()
  message(WARNING "stest: uv not found; the python_tests CTest is not registered")
endif()

# Every CTest that runs a firmware/node executable gets a 30 s timeout.
set_tests_properties(selftest_blinky_standalone selftest_blinky_quantum_determinism
  selftest_blinky_speed PROPERTIES TIMEOUT 30)
# A wall-clock budget: run it alone (see python_perf_tests).
set_tests_properties(selftest_blinky_speed PROPERTIES RUN_SERIAL TRUE)
