# STM32L4 peripheral models (uninstrumented C++, linked into every L4 firmware exe).
add_library(stest_chip_stm32l4 STATIC
  models/rcc.cpp models/pwr.cpp models/flash.cpp models/gpio.cpp
  models/syscfg.cpp models/exti.cpp models/tim.cpp models/adc.cpp models/dac.cpp models/spi.cpp)
target_include_directories(stest_chip_stm32l4 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(stest_chip_stm32l4 PUBLIC stest_runtime)

add_executable(stm32l4_model_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_models.cpp)
target_link_libraries(stm32l4_model_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_models COMMAND stm32l4_model_tests)

add_executable(stm32l4_rcc_clock_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_rcc_clocks.cpp)
target_link_libraries(stm32l4_rcc_clock_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_rcc_clocks COMMAND stm32l4_rcc_clock_tests)

add_executable(stm32l4_exti_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_exti.cpp)
target_link_libraries(stm32l4_exti_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_exti COMMAND stm32l4_exti_tests)

add_executable(stm32l4_tim_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_tim.cpp)
target_link_libraries(stm32l4_tim_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_tim COMMAND stm32l4_tim_tests)

add_executable(stm32l4_adc_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_adc.cpp)
target_link_libraries(stm32l4_adc_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_adc COMMAND stm32l4_adc_tests)

add_executable(stm32l4_spi_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_spi.cpp)
target_link_libraries(stm32l4_spi_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_spi COMMAND stm32l4_spi_tests)

add_executable(stm32l4_gpio_hal_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_gpio_hal.cpp)
target_link_libraries(stm32l4_gpio_hal_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_gpio_hal COMMAND stm32l4_gpio_hal_tests)

# Generator output for the L496 part: the weak-handler line format and table size.
add_test(NAME chip_gen_stm32l496rg
  COMMAND ${CMAKE_COMMAND} -DPYTHON=${Python3_EXECUTABLE}
          -DGEN=${PROJECT_SOURCE_DIR}/tools/gen_chip.py
          -DTOML=${CMAKE_CURRENT_SOURCE_DIR}/parts/stm32l496rg.toml
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/gen_check_stm32l496rg.cpp
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_gen.cmake)
set_tests_properties(chip_stm32l4_models chip_stm32l4_rcc_clocks chip_stm32l4_exti chip_stm32l4_tim chip_stm32l4_adc chip_stm32l4_spi chip_stm32l4_gpio_hal
  chip_gen_stm32l496rg PROPERTIES TIMEOUT 30)
