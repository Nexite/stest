// Subprocess helper for runtime_cortexm_abort_*: each mode must end in stest_rt_abort
// (exit 70) with a diagnostic, never an escaping exception or a hang.
#include <cstring>
#include <stdexcept>
#include "stest/cortexm.hpp"
#include "stest/cortexm_c.h"
#include "stest/mmio.h"
#include "stest/runtime.hpp"
using namespace stest;

int main(int argc, char** argv) {
  stest_rt_init();
  const char* m = argc > 1 ? argv[1] : "";
  static IrqHandler table[16 + 8] = {};  // all null
  volatile uint32_t host = 0;
  if (!strcmp(m, "clock_throw_int")) {
    runtime_clock().schedule_at(runtime_clock().now_ns() + 1, [] { throw 7; });
    (void)__stest_mmio_load32(&host);
  } else if (!strcmp(m, "clock_throw_std")) {
    runtime_clock().schedule_at(runtime_clock().now_ns() + 1, [] { throw std::runtime_error("tick"); });
    __stest_mmio_store32(&host, 1);
  } else if (!strcmp(m, "capi_no_core")) {
    stest_rt_wfi();
  } else {
    static Cortexm core(runtime_bus(), runtime_clock(), 4, table, 16 + 8);
    core.install();
    if (!strcmp(m, "sysresetreq")) {
      __stest_mmio_store32((void*)0xE000ED0C, 0x05FA0004);
    } else if (!strcmp(m, "null_handler")) {
      stest_rt_disable_irq();
      __stest_mmio_store32((void*)0xE000E200, 1);  // ISPR: IRQ 0
      __stest_mmio_store32((void*)0xE000E100, 1);  // ISER
      stest_rt_enable_irq();
    } else if (!strcmp(m, "wfi_deadlock")) {
      stest_rt_wfi();
    } else if (!strcmp(m, "systick_no_getter")) {
      __stest_mmio_store32((void*)0xE000E014, 999);
      __stest_mmio_store32((void*)0xE000E010, 7);
    }
  }
  return 0;
}
