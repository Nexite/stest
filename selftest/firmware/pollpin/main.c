/* Tight-polling self-test firmware for timestamped input (Node.post / Pin.drive_at).
 * PA0 is an input, PA5 an output. For each of ROUNDS rounds it spins on GPIOA->IDR until
 * PA0 reads the wanted level (1 on even rounds, 0 on odd ones), counting iterations in a
 * non-volatile local, then mirrors the level on PA5 (a gpio event at the access time) and
 * stores the count in counts[round]. Used to check that inbox deliveries are observed at
 * the same access whatever the quantum and whenever they were posted. */
#include <stdint.h>
#include "stm32l4xx.h"

#define ROUNDS 4u

volatile uint32_t counts[ROUNDS];
volatile uint32_t rounds = 0;

int main(void)
{
  RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
  GPIOA->MODER = (GPIOA->MODER & ~(3u << 0) & ~(3u << 10)) | (1u << 10); /* PA0 in, PA5 out */
  for (uint32_t r = 0; r < ROUNDS; r++) {
    const uint32_t want = (r & 1u) ? 0u : 1u;
    uint32_t n = 0;
    while ((GPIOA->IDR & 1u) != want)
      n++;
    GPIOA->BSRR = want ? (1u << 5) : (1u << 21);
    counts[r] = n;
    rounds = r + 1u;
  }
  for (;;) {
    __WFI();
  }
}
