/* Poll fast-forward regression firmware. Counts ITERS iterations of a loop in a
 * NON-volatile local while re-reading a constant volatile (never set), measuring the
 * elapsed virtual time with SysTick (free-running, 4 MHz MSI core clock, down counter).
 * Result: elapsed_ticks (start VAL - end VAL), done = 1.
 * With poll skip disabled this is ITERS x access cost; with the default threshold each
 * 64 iterations jump to the next clock event / quantum end (time-dilated). */
#include <stdint.h>
#include "stm32l4xx.h"

#define ITERS 1000u

volatile uint32_t never_set = 0;
volatile uint32_t elapsed_ticks = 0;
volatile uint32_t done = 0;

int main(void)
{
  SysTick->LOAD = 0xFFFFFFu;
  SysTick->VAL = 0u;
  SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_ENABLE_Msk;

  uint32_t start = SysTick->VAL;
  uint32_t n = ITERS;
  while (--n && !never_set) {
  }
  uint32_t end = SysTick->VAL;

  elapsed_ticks = (start - end) & 0xFFFFFFu; /* 24-bit down counter */
  done = 1u;
  for (;;) {
    __WFI();
  }
}
