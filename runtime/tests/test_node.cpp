// Node: firmware thread + baton handoff, quantum-end yields, firmware exit, events.
// Firmware entries here are plain C++ calling the runtime C ABI directly (what the
// MMIO pass would emit), so no instrumentation is needed.
#include <doctest/doctest.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include "stest/cortexm_c.h"
#include "stest/mmio.h"
#include "stest/node.hpp"
#include "stest/runtime.hpp"
using namespace stest;

namespace {
constexpr uint64_t MS = 1'000'000;
IrqHandler g_table[16 + 4] = {};
const ChipDesc kChip{"TESTCHIP", 4, g_table, 16 + 4, nullptr, nullptr};

std::atomic<int> g_iters{0};
volatile uint32_t g_ram = 0;

int fw_wfi_forever() {
  for (;;) {
    stest_rt_wfi();  // nothing pending, no events: sleeps to the quantum end
    ++g_iters;
  }
}

int fw_poll_forever() {
  for (;;) {
    (void)__stest_mmio_load32(&g_ram);  // RAM access: charged, outside the windows
    ++g_iters;
  }
}

int fw_return_7() {
  (void)__stest_mmio_load32(&g_ram);
  return 7;
}

int g_startup_calls = 0;
void startup() { ++g_startup_calls; }
int fw_check_startup() { return g_startup_calls == 1 ? 0 : 1; }

int fw_emit() {
  emit_event(stest_rt_now_ns(), {{"kind", "test"}, {"n", 1}});
  (void)__stest_mmio_load32(&g_ram);
  emit_event(stest_rt_now_ns(), {{"kind", "test"}, {"n", 2}});
  for (;;) stest_rt_wfi();
}
}  // namespace

TEST_CASE("node: wfi at the quantum end yields the baton (no spinning)") {
  g_iters = 0;
  Node node(kChip);
  uint64_t t0 = node.now_ns();
  node.start(&fw_wfi_forever);
  CHECK(node.run_until(t0 + 1 * MS) == Node::Stop::Quantum);
  CHECK(node.now_ns() == t0 + 1 * MS);
  CHECK(g_iters == 0);  // first wfi slept to the quantum end and yielded inside wfi
  CHECK(node.run_until(t0 + 3 * MS) == Node::Stop::Quantum);
  CHECK(node.now_ns() == t0 + 3 * MS);
  CHECK(g_iters == 1);  // exactly one loop iteration per quantum
  CHECK(node.run_until(t0 + 3 * MS) == Node::Stop::Quantum);  // not in the future: no-op
  CHECK(g_iters == 1);
}

TEST_CASE("node: quantum end reached during an MMIO charge yields; one thread at a time") {
  g_iters = 0;
  set_access_cost_ns(25);
  Node node(kChip);
  uint64_t t0 = node.now_ns();
  node.start(&fw_poll_forever);
  CHECK(node.run_until(t0 + 1000) == Node::Stop::Quantum);
  uint64_t t1 = node.now_ns();
  CHECK(t1 >= t0 + 1000);
  CHECK(t1 < t0 + 1000 + 25);
  int n1 = g_iters;
  CHECK(n1 >= 39);
  CHECK(n1 <= 40);
  // Firmware is parked while the loop thread holds the baton.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(g_iters == n1);
  CHECK(node.run_until(t0 + 2000) == Node::Stop::Quantum);
  CHECK(g_iters - n1 == 40);
}

TEST_CASE("node: firmware exit returns the baton with its exit code") {
  Node node(kChip);
  node.start(&fw_return_7);
  CHECK(node.run_until(node.now_ns() + 10 * MS) == Node::Stop::Exited);
  CHECK(node.exited());
  CHECK(node.exit_code() == 7);
  CHECK(node.run_until(node.now_ns() + 20 * MS) == Node::Stop::Exited);
}

TEST_CASE("node: chip startup hook runs on the firmware thread before main") {
  g_startup_calls = 0;
  const ChipDesc chip{"TESTCHIP", 4, g_table, 16 + 4, nullptr, &startup};
  Node node(chip);
  node.start(&fw_check_startup);
  CHECK(node.run_until(node.now_ns() + MS) == Node::Stop::Exited);
  CHECK(node.exit_code() == 0);
}

TEST_CASE("node: events are buffered in order with their virtual time") {
  set_access_cost_ns(25);
  Node node(kChip);
  uint64_t t0 = node.now_ns();
  node.start(&fw_emit);
  node.run_until(t0 + MS);
  auto ev = take_events();
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].t_ns == t0);
  CHECK(ev[0].data["n"] == 1);
  CHECK(ev[1].t_ns == t0 + 25);
  CHECK(ev[1].data.dump() == R"({"kind":"test","n":2})");
  CHECK(take_events().empty());
}

TEST_CASE("node: model registry") {
  struct Dummy : Peripheral {
    std::string_view name() const override { return "DUMMY"; }
    uint32_t read(uint32_t, unsigned) override { return 0; }
    void write(uint32_t, unsigned, uint32_t) override {}
  } d;
  Node node(kChip);
  CHECK(find_model("DUMMY") == nullptr);
  register_model(&d);
  CHECK(find_model("DUMMY") == &d);
  CHECK_THROWS_AS(register_model(&d), std::invalid_argument);
}

TEST_CASE("node: only one node per process at a time") {
  Node a(kChip);
  CHECK_THROWS_AS(Node{kChip}, std::logic_error);
}
