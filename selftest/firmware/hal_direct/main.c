/* hal_direct: built with MODELS GPIO=hal. Configures PA5 through the HAL, then pokes
 * GPIOA->BSRR directly twice (set, then reset): each write still reaches the GPIO model,
 * but stest warns once that firmware bypassed the HAL. Afterwards PA5 toggles every
 * 10 ms through HAL_GPIO_TogglePin. Uses the blinky CubeMX scaffolding (MSI clock). */
#include "main.h"

volatile uint32_t toggle_count = 0;

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

  gi.Pin = GPIO_PIN_5;
  gi.Mode = GPIO_MODE_OUTPUT_PP;
  gi.Pull = GPIO_NOPULL;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &gi);

  GPIOA->BSRR = GPIO_PIN_5;          /* direct access: warns (once) */
  GPIOA->BSRR = GPIO_PIN_5 << 16u;   /* same register: no second warning */

  while (1)
  {
    HAL_Delay(9);
    HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
    toggle_count++;
  }
}
