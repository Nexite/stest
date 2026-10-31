#include <doctest/doctest.h>
#include <cstring>
#include <vector>
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

namespace {
struct Rec : Peripheral {
  struct Op { bool wr; uint32_t off; unsigned w; uint32_t v; };
  std::vector<Op> ops;
  uint32_t mem[16] = {};
  std::string_view name() const override { return "rec"; }
  uint32_t read(uint32_t o, unsigned w) override {
    ops.push_back({false, o, w, 0});
    uint32_t v = 0;
    memcpy(&v, (uint8_t*)mem + o, w);
    return v;
  }
  void write(uint32_t o, unsigned w, uint32_t v) override {
    ops.push_back({true, o, w, v});
    memcpy((uint8_t*)mem + o, &v, w);
  }
};
}  // namespace

TEST_CASE("mmio: memset/memcpy split into aligned <=4-byte accesses through the bus") {
  stest_rt_init();
  static Rec r;
  runtime_bus().map(0x40021000, 0x40, &r);
  set_delivery_hook(nullptr);
  uint64_t t0 = stest_rt_now_ns();

  __stest_mmio_memset((void*)0x40021010, 0x1A5, 6);  // low byte used
  REQUIRE(r.ops.size() == 2);
  CHECK((r.ops[0].wr && r.ops[0].off == 0x10 && r.ops[0].w == 4 && r.ops[0].v == 0xA5A5A5A5));
  CHECK((r.ops[1].wr && r.ops[1].off == 0x14 && r.ops[1].w == 2 && r.ops[1].v == 0xA5A5));
  CHECK(stest_rt_now_ns() - t0 == 50);  // one charge per chunk

  r.ops.clear();
  __stest_mmio_memset((void*)0x40021021, 0, 5);  // misaligned start: 1, 2, 2
  REQUIRE(r.ops.size() == 3);
  CHECK((r.ops[0].off == 0x21 && r.ops[0].w == 1));
  CHECK((r.ops[1].off == 0x22 && r.ops[1].w == 2));  // 0x22 aligned for 2, not 4
  CHECK((r.ops[2].off == 0x24 && r.ops[2].w == 2));
  __stest_mmio_memset((void*)0x40021000, 0, 0);  // n == 0: no access
  CHECK(r.ops.size() == 3);

  // host -> MMIO, both aligned
  r.ops.clear();
  alignas(4) uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  __stest_mmio_memcpy((void*)0x40021030, src, 8);
  REQUIRE(r.ops.size() == 2);
  CHECK((r.ops[0].w == 4 && r.ops[0].v == 0x04030201));
  CHECK((r.ops[1].w == 4 && r.ops[1].off == 0x34 && r.ops[1].v == 0x08070605));

  // src/dst misaligned relative to each other: byte-wise
  r.ops.clear();
  __stest_mmio_memcpy((void*)0x40021000, src + 1, 3);
  REQUIRE(r.ops.size() == 3);
  for (auto& o : r.ops) CHECK(o.w == 1);
  CHECK(r.mem[0] == 0x00040302);

  // MMIO -> host
  r.ops.clear();
  alignas(4) uint8_t out[4] = {};
  __stest_mmio_memcpy(out, (void*)0x40021030, 4);
  REQUIRE(r.ops.size() == 1);
  CHECK((!r.ops[0].wr && r.ops[0].w == 4));
  CHECK(memcmp(out, src, 4) == 0);

  // overlapping (host) copies behave like memmove, both directions, runtime length
  alignas(4) uint8_t buf[16];
  for (int i = 0; i < 16; ++i) buf[i] = (uint8_t)i;
  volatile size_t n = 10;
  __stest_mmio_memcpy(buf + 2, buf, n);
  for (int i = 0; i < 10; ++i) CHECK(buf[2 + i] == i);
  for (int i = 0; i < 16; ++i) buf[i] = (uint8_t)i;
  __stest_mmio_memcpy(buf, buf + 2, n);
  for (int i = 0; i < 10; ++i) CHECK(buf[i] == i + 2);
  for (int i = 0; i < 16; ++i) buf[i] = (uint8_t)i;
  __stest_mmio_memcpy(buf + 4, buf, 11);  // aligned both sides, overlapping
  for (int i = 0; i < 11; ++i) CHECK(buf[4 + i] == i);
}
