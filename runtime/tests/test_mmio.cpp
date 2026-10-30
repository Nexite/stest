#include <doctest/doctest.h>
#include <cstring>
#include "stest/mmio.h"
#include "stest/runtime.hpp"
using namespace stest;

namespace {
struct Mem : Peripheral {
  uint32_t r[4] = {0x11111111, 0x22222222, 0, 0};
  int writes = 0;
  std::string_view name() const override { return "mem"; }
  uint32_t read(uint32_t o, unsigned) override { return r[o / 4]; }
  void write(uint32_t o, unsigned, uint32_t v) override { r[o / 4] = v; ++writes; }
};
int hook_calls = 0;
void hook() { ++hook_calls; }
}  // namespace

TEST_CASE("mmio: init, in-window dispatch, 64-bit split, cost, hook, direct host") {
  stest_rt_init(); stest_rt_init();
  static Mem m;
  runtime_bus().map(0x40020000, 0x10, &m);
  CHECK(access_cost_ns() == 25);
  set_delivery_hook(hook);

  uint64_t t0 = stest_rt_now_ns();
  CHECK(__stest_mmio_load32((void*)0x40020000) == 0x11111111);
  CHECK(stest_rt_now_ns() - t0 == 25);
  CHECK(hook_calls == 1);

  CHECK(__stest_mmio_load64((void*)0x40020000) == 0x2222222211111111ull);
  CHECK(stest_rt_now_ns() - t0 == 50);  // one charge per call
  __stest_mmio_store64((void*)0x40020008, 0xAABBCCDD00112233ull);
  CHECK(m.r[2] == 0x00112233);  // low word first
  CHECK(m.r[3] == 0xAABBCCDD);
  CHECK(m.writes == 2);

  set_access_cost_ns(100);
  uint32_t host = 5; uint8_t b8 = 0;
  uint64_t t1 = stest_rt_now_ns();
  __stest_mmio_store32(&host, 0x1234);
  CHECK(host == 0x1234);
  CHECK(__stest_mmio_load32(&host) == 0x1234);
  __stest_mmio_store8(&b8, 0xFE);
  CHECK(__stest_mmio_load8(&b8) == 0xFE);
  CHECK(stest_rt_now_ns() - t1 == 400);
  set_access_cost_ns(25);
  set_delivery_hook(nullptr);
}
