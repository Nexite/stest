# STM32L4 peripheral models (uninstrumented C++, linked into every L4 firmware exe).
add_library(stest_chip_stm32l4 STATIC
  models/rcc.cpp models/pwr.cpp models/flash.cpp models/gpio.cpp)
target_include_directories(stest_chip_stm32l4 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(stest_chip_stm32l4 PUBLIC stest_runtime)

add_executable(stm32l4_model_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_models.cpp)
target_link_libraries(stm32l4_model_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_models COMMAND stm32l4_model_tests)

add_executable(stm32l4_rcc_clock_tests ${PROJECT_SOURCE_DIR}/runtime/tests/main.cpp tests/test_rcc_clocks.cpp)
target_link_libraries(stm32l4_rcc_clock_tests PRIVATE stest_chip_stm32l4 doctest::doctest)
add_test(NAME chip_stm32l4_rcc_clocks COMMAND stm32l4_rcc_clock_tests)

# Generator output for the L496 part: the weak-handler line format and table size.
add_test(NAME chip_gen_stm32l496rg
  COMMAND ${CMAKE_COMMAND} -DPYTHON=${Python3_EXECUTABLE}
          -DGEN=${PROJECT_SOURCE_DIR}/tools/gen_chip.py
          -DTOML=${CMAKE_CURRENT_SOURCE_DIR}/parts/stm32l496rg.toml
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/gen_check_stm32l496rg.cpp
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_gen.cmake)
set_tests_properties(chip_stm32l4_models chip_stm32l4_rcc_clocks chip_gen_stm32l496rg PROPERTIES TIMEOUT 30)
