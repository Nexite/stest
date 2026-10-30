/* Speed gate: 1 virtual second of lighting-like load (20 kHz software-PWM ISR
 * plus a 1 kHz main-loop tick) through the MMIO interception pass. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

extern uint64_t stest_now_ns;

#define TIM2_BASE  0x40000000u
#define GPIOA_BASE 0x40020000u
#define GPIOB_BASE 0x40020400u
#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define TIM_SR   R32(TIM2_BASE + 0x10)
#define TIM_CNT  R32(TIM2_BASE + 0x24)
#define GPIOA_IDR  R32(GPIOA_BASE + 0x10)
#define GPIOA_BSRR R32(GPIOA_BASE + 0x18)
#define GPIOB_BSRR R32(GPIOB_BASE + 0x18)
#define SCB_ICSR R32(0xE000ED04u)

volatile uint32_t uwTick;
volatile uint32_t sink;

static void isr(uint32_t n) {            /* 8 volatile peripheral accesses */
  TIM_SR = 0;                            /* 1: clear update flag */
  GPIOA_BSRR = 1u << (n & 15);           /* 2-5: four BSRR writes */
  GPIOA_BSRR = 0x10000u << ((n + 1) & 15);
  GPIOB_BSRR = 1u << ((n + 2) & 15);
  GPIOB_BSRR = 0x10000u << ((n + 3) & 15);
  uint32_t a = GPIOA_IDR;                /* 6-8: reads */
  uint32_t b = TIM_CNT;
  uint32_t c = TIM_SR;
  sink = a ^ b ^ c;                      /* sink is volatile in host memory: counts as an access too */
}

static double now_s(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void) {
  double t0 = now_s();
  uint32_t acc = 0;
  for (uint32_t tick = 0; tick < 1000; tick++) {
    for (uint32_t i = 0; i < 20; i++) isr(tick * 20 + i);   /* 20 kHz => 20 per ms */
    uwTick = uwTick + 1;                                    /* volatile RAM load+store */
    for (uint32_t j = 0; j < 50; j++) {                     /* main loop: 50 accesses per tick */
      if (j % 5 == 0) acc += SCB_ICSR;
      else if (j % 5 == 1) GPIOA_BSRR = j;
      else acc += GPIOA_IDR;
    }
  }
  double dt = now_s() - t0;
  /* print results so the work is observable */
  fprintf(stderr, "uwTick=%u acc=%u sink=%u virt_ms=%.3f\n",
          (unsigned)uwTick, (unsigned)acc, (unsigned)sink, stest_now_ns / 1e6);
  printf("mmio_s=%.6f\n", dt);
  return 0;
}
