/* stest HAL shim for STM32L4 GPIO (stest_add_firmware(... MODELS GPIO=hal)).
 *
 * Replaces ST's stm32l4xx_hal_gpio.c at link time (family.cmake): it defines the same
 * public symbols with the same signatures (HAL_GPIO_Init/DeInit/ReadPin/WritePin/
 * TogglePin/LockPin/EXTI_IRQHandler and the weak HAL_GPIO_EXTI_Callback) and behaves the
 * same, but never dereferences a GPIO_TypeDef: GPIO register work goes to the GPIO model
 * through the stest_gpio_hal_* C API (stest/hal_api.h), which keeps the register block
 * consistent and accounts the same register accesses as ST's code (identical event logs).
 *
 * This file is compiled as part of the firmware's HAL archive, with the MMIO pass, like
 * the ST file it replaces: SYSCFG and EXTI stay in register mode, so EXTI line setup,
 * __HAL_RCC_SYSCFG_CLK_ENABLE and the EXTI pending-flag handling below go through their
 * register models exactly as in ST's driver (same accesses, same strictness, same time).
 */
#include "stm32l4xx_hal.h"
#include "stest/hal_api.h"

#ifdef HAL_GPIO_MODULE_ENABLED

/* Port letter of a GPIOx pointer, from its address (no access). Not a GPIO port: '?' (the
 * C API aborts). */
static char stest_port(const GPIO_TypeDef *GPIOx)
{
  uintptr_t off = (uintptr_t)GPIOx - (uintptr_t)GPIOA_BASE;
  if ((uintptr_t)GPIOx < (uintptr_t)GPIOA_BASE || (off % 0x400u) != 0u || off / 0x400u > 25u)
  {
    return '?';
  }
  return (char)('A' + off / 0x400u);
}

void HAL_GPIO_Init(GPIO_TypeDef *GPIOx, GPIO_InitTypeDef *GPIO_Init)
{
  const char port = stest_port(GPIOx);
  uint32_t position = 0x00u;
  uint32_t iocurrent;
  uint32_t temp;

  assert_param(IS_GPIO_ALL_INSTANCE(GPIOx));
  assert_param(IS_GPIO_PIN(GPIO_Init->Pin));
  assert_param(IS_GPIO_MODE(GPIO_Init->Mode));

  while (((GPIO_Init->Pin) >> position) != 0x00u)
  {
    iocurrent = (GPIO_Init->Pin) & (1uL << position);
    if (iocurrent != 0x00u)
    {
      /* Speed, output type, pull, alternate function, direction (GPIO model). */
      stest_gpio_hal_init(port, (uint16_t)iocurrent, GPIO_Init->Mode, GPIO_Init->Pull,
                          GPIO_Init->Speed, GPIO_Init->Alternate);

      /* EXTI line of this pin (SYSCFG/EXTI registers, register mode). */
      if ((GPIO_Init->Mode & EXTI_MODE) != 0x00u)
      {
        __HAL_RCC_SYSCFG_CLK_ENABLE();

        temp = SYSCFG->EXTICR[position >> 2u];
        temp &= ~(0x0FuL << (4u * (position & 0x03u)));
        temp |= ((uint32_t)(port - 'A') << (4u * (position & 0x03u)));
        SYSCFG->EXTICR[position >> 2u] = temp;

        temp = EXTI->RTSR1;
        temp &= ~(iocurrent);
        if ((GPIO_Init->Mode & TRIGGER_RISING) != 0x00u)
        {
          temp |= iocurrent;
        }
        EXTI->RTSR1 = temp;

        temp = EXTI->FTSR1;
        temp &= ~(iocurrent);
        if ((GPIO_Init->Mode & TRIGGER_FALLING) != 0x00u)
        {
          temp |= iocurrent;
        }
        EXTI->FTSR1 = temp;

        temp = EXTI->EMR1;
        temp &= ~(iocurrent);
        if ((GPIO_Init->Mode & EXTI_EVT) != 0x00u)
        {
          temp |= iocurrent;
        }
        EXTI->EMR1 = temp;

        temp = EXTI->IMR1;
        temp &= ~(iocurrent);
        if ((GPIO_Init->Mode & EXTI_IT) != 0x00u)
        {
          temp |= iocurrent;
        }
        EXTI->IMR1 = temp;
      }
    }
    position++;
  }
}

void HAL_GPIO_DeInit(GPIO_TypeDef *GPIOx, uint32_t GPIO_Pin)
{
  const char port = stest_port(GPIOx);
  uint32_t position = 0x00u;
  uint32_t iocurrent;
  uint32_t tmp;

  assert_param(IS_GPIO_ALL_INSTANCE(GPIOx));
  assert_param(IS_GPIO_PIN(GPIO_Pin));

  while ((GPIO_Pin >> position) != 0x00u)
  {
    iocurrent = (GPIO_Pin) & (1uL << position);
    if (iocurrent != 0x00u)
    {
      /* Release the EXTI line if this port owns it (SYSCFG/EXTI registers). */
      tmp = SYSCFG->EXTICR[position >> 2u];
      tmp &= (0x0FuL << (4u * (position & 0x03u)));
      if (tmp == ((uint32_t)(port - 'A') << (4u * (position & 0x03u))))
      {
        EXTI->IMR1 &= ~(iocurrent);
        EXTI->EMR1 &= ~(iocurrent);
        EXTI->FTSR1 &= ~(iocurrent);
        EXTI->RTSR1 &= ~(iocurrent);
        tmp = 0x0FuL << (4u * (position & 0x03u));
        SYSCFG->EXTICR[position >> 2u] &= ~tmp;
      }

      /* Analog mode, AF0, low speed, push-pull, no pull (GPIO model). */
      stest_gpio_hal_deinit(port, (uint16_t)iocurrent);
    }
    position++;
  }
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  assert_param(IS_GPIO_PIN(GPIO_Pin));
  return stest_gpio_hal_read(stest_port(GPIOx), GPIO_Pin) != 0 ? GPIO_PIN_SET : GPIO_PIN_RESET;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
  assert_param(IS_GPIO_PIN(GPIO_Pin));
  assert_param(IS_GPIO_PIN_ACTION(PinState));
  stest_gpio_hal_write(stest_port(GPIOx), GPIO_Pin, PinState != GPIO_PIN_RESET);
}

void HAL_GPIO_TogglePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  assert_param(IS_GPIO_PIN(GPIO_Pin));
  stest_gpio_hal_toggle(stest_port(GPIOx), GPIO_Pin);
}

HAL_StatusTypeDef HAL_GPIO_LockPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
  const char port = stest_port(GPIOx);
  __IO uint32_t tmp = GPIO_LCKR_LCKK;

  assert_param(IS_GPIO_LOCK_INSTANCE(GPIOx));
  assert_param(IS_GPIO_PIN(GPIO_Pin));

  /* LCKR key sequence: LCKK=1 + pins, LCKK=0 + pins, LCKK=1 + pins, then read LCKR (needed
   * to complete the sequence) and read it again to confirm LCKK is set. */
  tmp |= GPIO_Pin;
  stest_gpio_hal_lckr_write(port, tmp);
  stest_gpio_hal_lckr_write(port, GPIO_Pin);
  stest_gpio_hal_lckr_write(port, tmp);
  tmp = stest_gpio_hal_lckr_read(port);
  if ((stest_gpio_hal_lckr_read(port) & GPIO_LCKR_LCKK) != 0x00u)
  {
    return HAL_OK;
  }
  return HAL_ERROR;
}

void HAL_GPIO_EXTI_IRQHandler(uint16_t GPIO_Pin)
{
  if (__HAL_GPIO_EXTI_GET_IT(GPIO_Pin) != 0x00u)
  {
    __HAL_GPIO_EXTI_CLEAR_IT(GPIO_Pin);
    HAL_GPIO_EXTI_Callback(GPIO_Pin);
  }
}

__weak void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  UNUSED(GPIO_Pin);
}

#endif /* HAL_GPIO_MODULE_ENABLED */
