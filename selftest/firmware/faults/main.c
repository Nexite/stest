/* Fault-injection self-test firmware (one executable per variant, -DFAULT=<variant>):
 *   SPIN        for(;;){} with no volatile access after 10 ms (wall-clock timeout)
 *   CRASH       null-pointer store at 10 ms (host SIGSEGV/SIGBUS)
 *   UNMODELLED  write to USART1 (0x40013800, not modelled) at 10 ms (strict abort)
 *
 * Time base: SysTick at 1 kHz from the reset clock (MSI 4 MHz); the firmware sleeps
 * in WFI until the 10th tick (virtual t = 10 ms + startup offset). Before that, if the
 * environment sets STEST_FAULT_CHATTER=N, it prints N long lines to stderr (used to
 * test that the host drains node stderr continuously).
 */
#include <stdio.h>
#include <stdlib.h>
#include "stm32l4xx.h"

#define SPIN 1
#define CRASH 2
#define UNMODELLED 3
#ifndef FAULT
#error "define FAULT=SPIN|CRASH|UNMODELLED"
#endif

static volatile uint32_t ticks;

void SysTick_Handler(void) { ticks++; }

int main(void)
{
  const char *chatter = getenv("STEST_FAULT_CHATTER");
  if (chatter) {
    long n = strtol(chatter, NULL, 10);
    for (long i = 0; i < n; i++)
      fprintf(stderr, "chatter %ld ......................................................................\n", i);
  }

  SysTick_Config(SystemCoreClock / 1000u);
  while (ticks < 10u)
    __WFI();

#if FAULT == SPIN
  for (;;) {
  }
#elif FAULT == CRASH
  uint32_t *volatile p = NULL;
  *p = 1u; /* plain (non-volatile) store through a null pointer: host crash */
#elif FAULT == UNMODELLED
  *(volatile uint32_t *)0x40013800u = 1u; /* USART1->CR1: no USART model */
#else
#error "unknown FAULT"
#endif
  for (;;) {
    __WFI();
  }
}
