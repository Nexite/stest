// Node: firmware thread + baton handoff, quantum-end yields, firmware exit, events.
// Firmware entries here are plain C++ calling the runtime C ABI directly (what the
// MMIO pass would emit), so no instrumentation is needed.
#include <doctest/doctest.h>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
#include "stest/cortexm_c.h"
#include "stest/mmio.h"
#include "stest/node.hpp"
#include "stest/runtime.hpp"
using namespace stest;

namespace {
constexpr uint64_t MS = 1'000'000;
int g_pendsv = 0;
void pendsv_h() { ++g_pendsv; }
IrqHandler g_table[16 + 4] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                              &pendsv_h};  // exception 14: PendSV
const ChipDesc kChip{"TESTCHIP", 4, g_table, 16 + 4, nullptr, nullptr};

std::atomic<int> g_iters{0};
volatile uint32_t g_ram = 0;

int fw_wfi_forever() {
  for (;;) {
    stest_rt_wfi();  // nothing pending, no events: sleeps until an exception
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

TEST_CASE("node: wfi sleeps across quantum ends, yielding the baton at each") {
  g_iters = 0;
  g_pendsv = 0;
  Node node(kChip);
  uint64_t t0 = node.now_ns();
  node.start(&fw_wfi_forever);
  CHECK(node.run_until(t0 + 1 * MS) == Node::Stop::Quantum);
  CHECK(node.now_ns() == t0 + 1 * MS);
  CHECK(g_iters == 0);  // first wfi slept to the quantum end and yielded inside wfi
  CHECK(node.run_until(t0 + 3 * MS) == Node::Stop::Quantum);
  CHECK(node.now_ns() == t0 + 3 * MS);
  CHECK(g_iters == 0);  // nothing woke the core: no wake-up per quantum
  CHECK(node.run_until(t0 + 3 * MS) == Node::Stop::Quantum);  // not in the future: no-op
  CHECK(g_iters == 0);
  // An exception pended by the loop thread between quanta (as MODEL_CALL / an inbox deliver
  // would) wakes the core at the start of the next quantum: one loop iteration, then sleep.
  node.core().set_pending(Cortexm::kPendSV_IRQn);
  CHECK(node.run_until(t0 + 4 * MS) == Node::Stop::Quantum);
  CHECK(g_iters == 1);
  CHECK(g_pendsv == 1);
  CHECK(node.run_until(t0 + 6 * MS) == Node::Stop::Quantum);
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
  CHECK(t1 == t0 + 1000);  // never past the quantum end (here a charge lands exactly on it)
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

namespace {
constexpr int kRec = 200;
uint64_t g_times[kRec];
int g_nrec = 0;
int fw_record_access_times() {
  for (g_nrec = 0; g_nrec < kRec;) {
    (void)__stest_mmio_load32(&g_ram);
    g_times[g_nrec++] = stest_rt_now_ns();
  }
  for (;;) stest_rt_wfi();
}

volatile uint32_t g_flag = 0;
uint64_t g_seen = 0;
int fw_poll_flag() {
  while (!__stest_mmio_load32(&g_flag)) {
  }
  g_seen = stest_rt_now_ns();
  for (;;) stest_rt_wfi();
}

// Access times of fw_record_access_times relative to its start, stopping at each `ends`
// offset; checks that the node stops at exactly the quantum end.
std::vector<uint64_t> record_with_ends(std::initializer_list<uint64_t> ends) {
  Node node(kChip);
  const uint64_t t0 = node.now_ns();
  node.start(&fw_record_access_times);
  for (uint64_t e : ends) {
    CHECK(node.run_until(t0 + e) == Node::Stop::Quantum);
    CHECK(node.now_ns() == t0 + e);
  }
  REQUIRE(g_nrec == kRec);
  std::vector<uint64_t> out;
  for (int i = 0; i < kRec; ++i) out.push_back(g_times[i] - t0);
  return out;
}

uint64_t seen_offset(bool schedule_while_parked) {
  g_flag = 0;
  g_seen = 0;
  Node node(kChip);
  const uint64_t t0 = node.now_ns();
  const uint64_t t = t0 + 1000;  // 40 accesses of 25 ns: a charge lands exactly on it
  auto set_flag = [] { g_flag = 1; };
  if (!schedule_while_parked) runtime_clock().schedule_at(t, set_flag);
  node.start(&fw_poll_flag);
  CHECK(node.run_until(t) == Node::Stop::Quantum);
  CHECK(node.now_ns() == t);  // parked exactly at the quantum end, before that access
  if (schedule_while_parked) runtime_clock().schedule_at(node.now_ns(), set_flag);  // t == now
  CHECK(node.run_until(t0 + 3000) == Node::Stop::Quantum);
  return g_seen - t0;
}
}  // namespace

TEST_CASE("node: a node stops exactly at the quantum end; timing is boundary-independent") {
  set_access_cost_ns(25);
  set_poll_skip_threshold(0);  // constant loads: no fast-forward (its jumps depend on the quantum)
  const auto straight = record_with_ends({10'000});
  CHECK(straight[0] == 25);
  CHECK(straight[kRec - 1] == 25u * kRec);
  // Quantum ends on and off the 25 ns access grid (an access crossing one completes on resume).
  CHECK(record_with_ends({7, 1010, 1011, 1012, 1333, 2000, 2001, 2024, 2025, 4999, 10'000}) == straight);
  set_poll_skip_threshold(64);
}

TEST_CASE("node: an event scheduled at now while parked is seen by the in-flight access") {
  set_access_cost_ns(25);
  set_poll_skip_threshold(0);
  const uint64_t scheduled_before = seen_offset(false);
  CHECK(scheduled_before == 1000);  // the access charged to t reads the flag set at t
  CHECK(seen_offset(true) == scheduled_before);
  set_poll_skip_threshold(64);
}

TEST_CASE("node: only one node per process at a time") {
  Node a(kChip);
  CHECK_THROWS_AS(Node{kChip}, std::logic_error);
}
