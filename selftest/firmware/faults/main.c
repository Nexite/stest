/* Fault-injection self-test firmware (one executable per variant, -DFAULT=<variant>):
 *   SPIN        for(;;){} with no volatile access after 10 ms (wall-clock timeout)
 *   CRASH       null-pointer store at 10 ms (host SIGSEGV/SIGBUS)
 *   UNMODELLED  write to USART1 (0x40013800, not modelled) at 10 ms (strict abort)
 *   RO_WRITE    write to GPIOA->IDR (read-only register) at 10 ms (strict abort)
 *   CLKOFF      write to GPIOB->ODR without enabling the GPIOB clock at 10 ms (strict abort)
 *   PRINTF      no fault: at 10 ms printf("hi\n") to stdout (after STEST_FAULT_STDOUT_LINES lines
 *               "line <i>", if set), then sleep; used to test stdout capture
 *
 * Every variant except CLKOFF enables the GPIOA clock first (GPIO access with its RCC
 * enable bit clear is itself a strict abort).
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
#define RO_WRITE 4
#define CLKOFF 5
#define PRINTF 6
#ifndef FAULT
#error "define FAULT=SPIN|CRASH|UNMODELLED|RO_WRITE|CLKOFF|PRINTF"
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

#if FAULT != CLKOFF
  RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
#endif
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
#elif FAULT == RO_WRITE
  GPIOA->IDR = 1u; /* read-only register: strict abort, not a silent no-op */
#elif FAULT == CLKOFF
  GPIOB->ODR = 1u; /* GPIOB clock (RCC AHB2ENR bit 1) never enabled: strict abort */
#elif FAULT == PRINTF
  {
    const char *lines = getenv("STEST_FAULT_STDOUT_LINES");
    long n = lines ? strtol(lines, NULL, 10) : 0;
    for (long i = 0; i < n; i++)
      printf("line %ld\n", i);
    printf("hi\n");
  }
#else
#error "unknown FAULT"
#endif
  for (;;) {
    __WFI();
  }
}
