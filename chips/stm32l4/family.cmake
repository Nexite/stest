# STM32L4 family settings read by stest_add_firmware (cmake/stest_add_firmware.cmake).
#
# HAL shims (MODELS <TYPE>=hal): for each TYPE in STEST_STM32L4_HAL_SHIMS, the ST HAL file
# STEST_STM32L4_HAL_SHIM_<TYPE>_REPLACES is dropped from the firmware's HAL sources and
# STEST_STM32L4_HAL_SHIM_<TYPE>_SOURCE (compiled the same way, with the MMIO pass) is added.
# The shim calls the stest_<type>_hal_* C API (runtime/include/stest/hal_api.h). Every other
# type is register mode only.
set(STEST_STM32L4_HAL_SHIMS GPIO)
set(STEST_STM32L4_HAL_SHIM_GPIO_REPLACES stm32l4xx_hal_gpio.c)
set(STEST_STM32L4_HAL_SHIM_GPIO_SOURCE ${CMAKE_CURRENT_LIST_DIR}/hal/stm32l4xx_hal_gpio_shim.c)
