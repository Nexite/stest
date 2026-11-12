/* WFI regression firmware: a naive sleep loop whose body touches a register on every wake-up.
 *
 *   for (;;) { __WFI(); GPIOA->ODR ^= 1; }
 *
 * SysTick interrupts every 750 us (LOAD 2999 at the 4 MHz MSI reset clock); its handler counts
 * ticks. Each wake-up toggles PA0 (an output), so every return from WFI is an event in the log.
 * WFI returns only when an exception is pending or was taken (ARMv7-M), never because a quantum
 * ended, so the log must be identical for any quantum size (selftest_wfiloop_quantum_determinism:
 * 1000, 88 and 20 us). */
#include <stdint.h>
#include "stm32l4xx.h"

volatile uint32_t ticks = 0;
volatile uint32_t wakes = 0;

void SysTick_Handler(void)
{
  ++ticks;
}

int main(void)
{
  RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
  (void)RCC->AHB2ENR;
  GPIOA->MODER = (GPIOA->MODER & ~GPIO_MODER_MODE0_Msk) | GPIO_MODER_MODE0_0; /* PA0 output */

  SysTick->LOAD = 2999u;
  SysTick->VAL = 0u;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;

  for (;;) {
    __WFI();
    GPIOA->ODR ^= 1u;
    ++wakes;
  }
}
