# stest self-test firmware: real CubeMX-style projects built with stest_add_firmware.
set(_bl ${CMAKE_CURRENT_SOURCE_DIR}/firmware/blinky/Core)
stest_add_firmware(blinky
  CHIP STM32L496RG
  SOURCES ${_bl}/Src/main.c ${_bl}/Src/stm32l4xx_it.c ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_bl}/Inc)

add_test(NAME selftest_blinky_standalone
  COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:blinky>
          -P ${CMAKE_CURRENT_SOURCE_DIR}/check_blinky.cmake)
