// ModelContext (typed model lookup, clock tree), Signal, and Bus clock-enable gating.
#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/cortexm.hpp"
#include "stest/model_context.hpp"
using namespace stest;

namespace {
struct Dummy : Peripheral {
  explicit Dummy(std::string n) : name_(std::move(n)) {}
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t, unsigned) override { return 0x1234; }
  void write(uint32_t, unsigned, uint32_t v) override { last = v; }
  void connect(ModelContext&) override { ++connects; }
  std::string name_;
  uint32_t last = 0;
  int connects = 0;
};
struct Other : Dummy { using Dummy::Dummy; };

struct FakeTree : ClockTree {
  uint32_t enr = 0;
  uint64_t sysclk_hz() const override { return 1; }
  uint64_t hclk_hz() const override { return 2; }
  uint64_t pclk1_hz() const override { return 3; }
  uint64_t pclk2_hz() const override { return 4; }
  uint64_t pllsai1r_hz() const override { return 0; }
  uint64_t pllsai2r_hz() const override { return 0; }
  uint32_t adcsel() const override { return 0; }
  bool enabled(uint32_t, unsigned bit) const override { return (enr >> bit) & 1u; }
  const uint32_t* enable_word(uint32_t) const override { return &enr; }
};

const IrqHandler kTable[16] = {};
}  // namespace

TEST_CASE("model context: typed lookup, wrong type and missing names throw") {
  Bus bus;
  Clock clock;
  Cortexm core(bus, clock, 4, kTable, 16);
  ModelContext ctx(clock, core, bus);
  CHECK(&ctx.clock() == &clock);
  CHECK(&ctx.core() == &core);
  CHECK(&ctx.bus() == &bus);
  Dummy a("GPIOA");
  Other b("TIM2");
  ctx.add(&a);
  ctx.add(&b);
  CHECK(&ctx.get<Dummy>("GPIOA") == &a);
  CHECK(&ctx.get<Dummy>("TIM2") == &b);  // derived type is fine
  CHECK(&ctx.get<Other>("TIM2") == &b);
  try {
    ctx.get<Other>("GPIOA");
    FAIL("expected logic_error");
  } catch (const std::logic_error& e) {
    std::string w = e.what();
    CHECK(w.find("GPIOA") != std::string::npos);
    CHECK(w.find("Other") != std::string::npos);
  }
  try {
    ctx.get<Dummy>("NOPE");
    FAIL("expected logic_error");
  } catch (const std::logic_error& e) {
    std::string w = e.what();
    CHECK(w.find("NOPE") != std::string::npos);
    CHECK(w.find("Dummy") != std::string::npos);
  }
  Dummy dup("GPIOA");
  CHECK_THROWS_AS(ctx.add(&dup), std::invalid_argument);
}

TEST_CASE("model context: clock tree must be registered") {
  Bus bus;
  Clock clock;
  Cortexm core(bus, clock, 4, kTable, 16);
  ModelContext ctx(clock, core, bus);
  CHECK_THROWS_AS(ctx.clocks(), std::logic_error);
  FakeTree t;
  ctx.set_clock_tree(&t);
  CHECK(&ctx.clocks() == &t);
  CHECK(ctx.clocks().pclk2_hz() == 4);
}

TEST_CASE("signal: synchronous, in subscription order") {
  Signal<int> s;
  std::vector<std::string> log;
  s.emit(0);  // no subscribers: no-op
  s.subscribe([&](const int& v) { log.push_back("a" + std::to_string(v)); });
  s.subscribe([&](const int& v) { log.push_back("b" + std::to_string(v)); });
  s.emit(1);
  CHECK(log == std::vector<std::string>{"a1", "b1"});
  s.subscribe([&](const int& v) { log.push_back("c" + std::to_string(v)); });
  s.emit(2);
  CHECK(log == std::vector<std::string>{"a1", "b1", "a2", "b2", "c2"});
}

TEST_CASE("bus: gated peripheral aborts while its clock is disabled") {
  Bus bus;
  FakeTree t;
  Dummy g("GPIOB");
  Dummy u("UNGATED");
  bus.map(0x48000400, 0x400, &g);
  bus.map(0x48000800, 0x400, &u);
  bus.set_gate(&g, [&t] { return t.enabled(0x4C, 1); }, "RCC AHB2ENR bit 1");
  try {
    bus.write(0x48000414, 4, 1);
    FAIL("expected ClockDisabled");
  } catch (const ClockDisabled& e) {
    CHECK(std::string(e.what()) == "access to GPIOB while its clock is disabled (RCC AHB2ENR bit 1) (write32 0x48000414)");
    CHECK(e.addr == 0x48000414);
    CHECK(e.is_write);
  }
  CHECK_THROWS_AS(bus.read(0x48000410, 4), ClockDisabled);
  CHECK(g.last == 0);                          // the write never reached the model
  CHECK(bus.read(0x48000800, 4) == 0x1234);    // ungated mapping unaffected
  t.enr = 1u << 1;
  CHECK(bus.read(0x48000410, 4) == 0x1234);
  bus.write(0x48000414, 4, 7);
  CHECK(g.last == 7);
  t.enr = 0;                                   // disabling again re-arms the gate
  CHECK_THROWS_AS(bus.read(0x48000410, 2), ClockDisabled);
  Dummy unmapped("X");
  CHECK_THROWS_AS(bus.set_gate(&unmapped, [] { return true; }, "RCC X bit 0"), std::invalid_argument);
  bus.unmap(&g);  // gate goes with the mapping
  bus.map(0x48000400, 0x400, &g);
  CHECK(bus.read(0x48000410, 4) == 0x1234);
}

TEST_CASE("bus: word-form gate reads the enable word live") {
  Bus bus;
  FakeTree t;
  Dummy g("PWR");
  bus.map(0x40007000, 0x400, &g);
  bus.set_gate(&g, t.enable_word(0x58), 1u << 28, "RCC APB1ENR1 bit 28");
  try {
    bus.read(0x40007000, 4);
    FAIL("expected ClockDisabled");
  } catch (const ClockDisabled& e) {
    CHECK(std::string(e.what()) == "access to PWR while its clock is disabled (RCC APB1ENR1 bit 28) (read32 0x40007000)");
    CHECK_FALSE(e.is_write);
  }
  t.enr = 1u << 27;  // another bit: still gated
  CHECK_THROWS_AS(bus.write(0x40007000, 4, 1), ClockDisabled);
  t.enr = 1u << 28;
  CHECK(bus.read(0x40007000, 4) == 0x1234);
  CHECK_THROWS_AS(bus.set_gate(&g, nullptr, 1, "x"), std::invalid_argument);
  CHECK_THROWS_AS(bus.set_gate(&g, &t.enr, 0, "x"), std::invalid_argument);
}

#include <chrono>
#include <cstdio>
TEST_CASE("bench: Bus::write gated vs ungated ns/op" * doctest::skip()) {
  Bus bus;
  FakeTree t;
  t.enr = ~0u;
  Dummy g("G"), w("W"), u("U");
  bus.map(0x48000000, 0x400, &g);
  bus.map(0x48000400, 0x400, &w);
  bus.map(0x48000800, 0x400, &u);
  bus.set_gate(&g, [&t] { return t.enabled(0x4C, 0); }, "RCC AHB2ENR bit 0");
  bus.set_gate(&w, t.enable_word(0x4C), 1u, "RCC AHB2ENR bit 0");
  constexpr int N = 5000000;
  const char* label[] = {"ungated", "gated (std::function)", "gated (word)"};
  uint64_t bases[] = {0x48000800ull, 0x48000000ull, 0x48000400ull};
  for (int k = 0; k < 3; ++k) {
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) bus.write(bases[k] + 0x14, 4, uint32_t(i));
    auto t1 = std::chrono::steady_clock::now();
    std::printf("Bus::write %s: %.2f ns/op\n", label[k],
                std::chrono::duration<double, std::nano>(t1 - t0).count() / N);
  }
}

TEST_CASE("model context: set_irq_source ORs the sources sharing an IRQ line") {
  Bus bus;
  Clock clock;
  const IrqHandler table[16 + 4] = {};
  Cortexm core(bus, clock, 4, table, 16 + 4);
  ModelContext ctx(clock, core, bus);
  int a = 0, b = 0;
  ctx.set_irq_source(2, &a, true);  // low -> high: pends
  CHECK(core.pending(2));
  core.clear_pending(2);
  ctx.set_irq_source(2, &b, true);  // already high (a): no new edge
  CHECK_FALSE(core.pending(2));
  ctx.set_irq_source(2, &a, false);  // b keeps it high
  ctx.set_irq_source(2, &a, true);
  CHECK_FALSE(core.pending(2));
  ctx.set_irq_source(2, &a, false);
  ctx.set_irq_source(2, &b, false);  // both low
  ctx.set_irq_source(2, &b, false);  // idempotent
  ctx.set_irq_source(2, &a, true);   // new rising edge
  CHECK(core.pending(2));
  core.clear_pending(2);
  ctx.set_irq_source(3, &a, true);   // independent line
  CHECK(core.pending(3));
  CHECK_FALSE(core.pending(2));
}
