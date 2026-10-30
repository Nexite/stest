// Host half of runtime_cortexm_cmsis_e2e (uninstrumented): builds the core, installs
// it, and runs the instrumented firmware in fw.c.
#include <cstdio>
#include "stest/cortexm.hpp"
#include "stest/mmio.h"
#include "stest/runtime.hpp"

extern "C" {
extern uint32_t SystemCoreClock;
int fw_run(void);
uint32_t fw_ticks(void);
void SysTick_Handler(void);
void PendSV_Handler(void);
void TIM2_IRQHandler(void);
}

int main() {
  stest_rt_init();
  static stest::IrqHandler table[16 + 82] = {};
  table[14] = PendSV_Handler;
  table[15] = SysTick_Handler;
  table[16 + 28] = TIM2_IRQHandler;
  stest::Cortexm core(stest::runtime_bus(), stest::runtime_clock(), 4, table, 16 + 82);
  core.set_core_clock_getter([]() -> uint32_t { return SystemCoreClock; });
  core.set_quantum_end(1'000'000'000);
  core.install();
  int rc = fw_run();
  uint64_t now = stest::runtime_clock().now_ns();
  std::printf("fw_run=%d ticks=%u now_ns=%llu\n", rc, fw_ticks(), (unsigned long long)now);
  if (rc != 0) return 1;
  if (fw_ticks() != 10) return 2;
  if (now < 10'000'000 || now > 10'100'000) return 3;  // 10 ms + per-access costs
  return 0;
}
