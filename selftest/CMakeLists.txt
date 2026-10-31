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
