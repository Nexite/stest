/* exti: PC13 is an input with pull-down configured through the HAL for a rising-edge
 * interrupt (GPIO_MODE_IT_RISING, as a CubeMX user button). HAL_GPIO_EXTI_Callback toggles
 * the LED on PA5 and counts callbacks. Uses the blinky CubeMX scaffolding (MSI clock). */
#include "main.h"

volatile uint32_t exti_count = 0;

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_13)
  {
    exti_count++;
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
  }
}

void EXTI15_10_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_13);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

int main(void)
{
  GPIO_InitTypeDef gi = {0};
  HAL_Init();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  gi.Pin = GPIO_PIN_5;
  gi.Mode = GPIO_MODE_OUTPUT_PP;
  gi.Pull = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gi);

  gi.Pin = GPIO_PIN_13;
  gi.Mode = GPIO_MODE_IT_RISING;
  gi.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOC, &gi);

  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  while (1)
  {
    __WFI();
  }
}
