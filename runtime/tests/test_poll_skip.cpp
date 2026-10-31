// Poll fast-forward: a streak of identical loads (no store, no IRQ) skips the clock
// to the next event / quantum end instead of spinning access by access.
#include <doctest/doctest.h>
#include <cstdint>
#include "stest/cortexm.hpp"
#include "stest/mmio.h"
#include "stest/runtime.hpp"
using namespace stest;

namespace {
constexpr uint64_t MS = 1'000'000;
volatile uint32_t g_flag = 0;
volatile uint32_t g_tick = 7;

struct Result { uint64_t loads; uint64_t now; uint32_t flag; };

// Firmware-like loop: `while (!flag) ;` as the MMIO pass would emit it.
Result spin_on_flag(uint32_t threshold) {
  set_poll_skip_threshold(threshold);
  g_flag = 0;
  Clock& c = runtime_clock();
  uint64_t t_set = c.now_ns() + 1 * MS;
  c.schedule_at(t_set, [] { g_flag = 1; });
  uint64_t loads = 0;
  while (!__stest_mmio_load32(&g_flag)) ++loads;
  CHECK(c.now_ns() >= t_set);
  CHECK(c.now_ns() < t_set + 2 * access_cost_ns());
  return {loads, c.now_ns() - t_set, g_flag};
}
}  // namespace

TEST_CASE("poll skip: RAM flag set by a clock event") {
  stest_rt_init();
  set_access_cost_ns(25);
  CHECK(poll_skip_threshold() == 64);  // default
  Result on = spin_on_flag(64);
  Result off = spin_on_flag(0);
  CHECK(on.loads < 100);              // 64-load streak, then one jump
  CHECK(off.loads >= 39'990);         // 1 ms / 25 ns
  CHECK(on.flag == 1);
  CHECK(on.flag == off.flag);
  set_poll_skip_threshold(64);
}

TEST_CASE("poll skip: loop alternating two addresses (flag + tick) is skipped too") {
  set_poll_skip_threshold(64);
  g_flag = 0;
  Clock& c = runtime_clock();
  uint64_t t_set = c.now_ns() + 1 * MS;
  c.schedule_at(t_set, [] { g_flag = 1; });
  uint64_t loads = 0;
  for (;;) {
    (void)__stest_mmio_load32(&g_tick);
    ++loads;
    if (__stest_mmio_load32(&g_flag)) break;
    ++loads;
  }
  CHECK(loads < 200);
  CHECK(c.now_ns() >= t_set);
}

TEST_CASE("poll skip: a store breaks the streak") {
  set_poll_skip_threshold(64);
  g_flag = 0;
  Clock& c = runtime_clock();
  uint64_t t0 = c.now_ns();
  EventId ev = c.schedule_at(t0 + 1 * MS, [] { g_flag = 1; });
  for (int i = 0; i < 1000; ++i) {
    (void)__stest_mmio_load32(&g_flag);
    __stest_mmio_store32(&g_tick, 7);
  }
  CHECK(c.now_ns() == t0 + 2000 * 25);  // no jump
  c.cancel(ev);
}

TEST_CASE("poll skip: time-varying values (SysTick VAL) never skip") {
  set_poll_skip_threshold(64);
  static IrqHandler table[16] = {};
  Cortexm core(runtime_bus(), runtime_clock(), 4, table, 16);
  static uint32_t hz = 80'000'000;
  core.set_core_clock_getter([] { return hz; });
  for (uint32_t clksource : {4u, 0u}) {  // 80 MHz core clock, and /8 external reference
    core.set_systick_external_divider(8);
    __stest_mmio_store32((void*)0xE000E014, 0x00FFFFFF);       // LOAD
    __stest_mmio_store32((void*)0xE000E018, 0);                // VAL (restart)
    __stest_mmio_store32((void*)0xE000E010, 1u | clksource);   // ENABLE (no TICKINT)
    Clock& c = runtime_clock();
    REQUIRE(c.next_event_ns() != UINT64_MAX);  // the wrap event a skip would jump to
    uint64_t t0 = c.now_ns();
    for (int i = 0; i < 1000; ++i) (void)__stest_mmio_load32((void*)0xE000E018);
    CHECK(c.now_ns() == t0 + 1000 * 25);
    __stest_mmio_store32((void*)0xE000E010, 0);
  }
}
