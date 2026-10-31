// STM32L4 SYSCFG + EXTI (lines 0-15 driven by GPIO edges) on a private Bus + Clock + Cortexm.
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <memory>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/cortexm.hpp"
#include "stest/model_context.hpp"
#include "stm32l4/models/exti.hpp"
#include "stm32l4/models/gpio.hpp"
#include "stm32l4/models/syscfg.hpp"
using namespace stest;
using nlohmann::json;

namespace {
const IrqHandler kTable[16 + 82] = {};
constexpr uint64_t SYSCFG = 0x40010000, EXTI = 0x40010400, GPIOA = 0x48000000, GPIOC = 0x48000800;
constexpr uint32_t IMR1 = 0x00, RTSR1 = 0x08, FTSR1 = 0x0C, SWIER1 = 0x10, PR1 = 0x14;
constexpr uint32_t EXTICR4 = 0x08 + 3 * 4;

struct Rig {
  Bus bus;
  Clock clock;
  Cortexm core{bus, clock, 4, kTable, 16 + 82};
  ModelContext ctx{clock, core, bus};
  stm32l4::Syscfg syscfg{"SYSCFG", json::object(), ctx};
  stm32l4::Gpio a{"GPIOA", json{{"port", "A"}}, ctx};
  stm32l4::Gpio c{"GPIOC", json{{"port", "C"}}, ctx};
  std::unique_ptr<stm32l4::Exti> exti;
  Rig() {
    json p = {{"irq", {6, 7, 8, 9, 10, 23, 23, 23, 23, 23, 40, 40, 40, 40, 40, 40}}};
    exti = std::make_unique<stm32l4::Exti>("EXTI", p, ctx);
    ctx.add(&syscfg); ctx.add(&a); ctx.add(&c); ctx.add(exti.get());
    syscfg.connect(ctx); a.connect(ctx); c.connect(ctx); exti->connect(ctx);
    bus.map(SYSCFG, 0x400, &syscfg);
    bus.map(EXTI, 0x400, exti.get());
    bus.map(GPIOA, 0x400, &a);
    bus.map(GPIOC, 0x400, &c);
  }
  void pc13_input_pulldown() {
    bus.write(GPIOC, 4, bus.read(GPIOC, 4) & ~(3u << 26));
    bus.write(GPIOC + 0x0C, 4, 2u << 26);
  }
};
}  // namespace

TEST_CASE("syscfg/exti: reset values") {
  Rig r;
  CHECK(r.bus.read(SYSCFG + 0x04, 4) == 0x7C000001);  // CFGR1
  CHECK(r.bus.read(SYSCFG + 0x08, 4) == 0);           // EXTICR1
  CHECK(r.bus.read(EXTI + 0x00, 4) == 0xFF820000);    // IMR1
  CHECK(r.bus.read(EXTI + 0x20, 4) == 0x00000187);    // IMR2
  for (uint32_t off : {0x04u, 0x08u, 0x0Cu, 0x10u, 0x14u, 0x24u, 0x28u, 0x2Cu, 0x30u, 0x34u})
    CHECK(r.bus.read(EXTI + off, 4) == 0);
  for (uint32_t off : {0x00u, 0x04u, 0x18u, 0x1Cu, 0x20u, 0x24u, 0x28u})
    CHECK_NOTHROW(r.bus.read(SYSCFG + off, 4));
  CHECK_THROWS_AS(r.bus.read(EXTI + 0x18, 4), UnmodelledAccess);
}

TEST_CASE("exti_rising_edge_sets_pr_and_irq") {
  Rig r;
  r.pc13_input_pulldown();
  r.bus.write(SYSCFG + EXTICR4, 4, 2u << 4);  // line 13 <- port C
  r.bus.write(EXTI + RTSR1, 4, 1u << 13);
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4) | (1u << 13));
  CHECK_FALSE(r.core.pending(40));
  r.clock.advance(1234);  // edge time is the clock's current time
  r.c.call("drive_pin", {{"pin", 13}, {"level", 1}});
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) != 0);
  CHECK(r.core.pending(40));
  // falling edge: FTSR not set -> no new PR
  r.core.clear_pending(40);
  r.bus.write(EXTI + PR1, 4, 1u << 13);  // rc_w1
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) == 0);
  r.c.call("drive_pin", {{"pin", 13}, {"level", 0}});
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) == 0);
  CHECK_FALSE(r.core.pending(40));
  // falling-edge trigger
  r.bus.write(EXTI + FTSR1, 4, 1u << 13);
  r.c.call("drive_pin", {{"pin", 13}, {"level", 1}});
  r.bus.write(EXTI + PR1, 4, 1u << 13);
  r.core.clear_pending(40);
  r.c.call("drive_pin", {{"pin", 13}, {"level", 0}});
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) != 0);
  CHECK(r.core.pending(40));
}

TEST_CASE("exti_edge_from_firmware_output_write_is_synchronous") {
  Rig r;
  r.bus.write(SYSCFG + 0x08, 4, 0);  // line 5 <- port A (EXTICR2 is line 4..7)
  r.bus.write(GPIOA, 4, (r.bus.read(GPIOA, 4) & ~(3u << 10)) | (1u << 10));  // PA5 output
  r.bus.write(EXTI + RTSR1, 4, 1u << 5);
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4) | (1u << 5));
  r.bus.write(GPIOA + 0x18, 4, 1u << 5);  // BSRR set
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 5)) != 0);
  CHECK(r.core.pending(23));
}

TEST_CASE("exti_port_mapping") {
  Rig r;
  r.pc13_input_pulldown();
  r.bus.write(EXTI + RTSR1, 4, 1u << 13);
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4) | (1u << 13));
  CHECK(r.syscfg.exti_port(13) == 0);
  r.c.call("drive_pin", {{"pin", 13}, {"level", 1}});  // EXTICR4 selects A: PC13 ignored
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) == 0);
  CHECK_FALSE(r.core.pending(40));
  r.c.call("release_pin", {{"pin", 13}});
  r.bus.write(SYSCFG + EXTICR4, 4, 2u << 4);
  CHECK(r.syscfg.exti_port(13) == 2);
  r.c.call("drive_pin", {{"pin", 13}, {"level", 1}});
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) != 0);
  CHECK(r.core.pending(40));
  // PA13 does not trigger line 13 while C is selected
  r.bus.write(EXTI + PR1, 4, 1u << 13);
  r.core.clear_pending(40);
  r.a.call("drive_pin", {{"pin", 13}, {"level", 1}});
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) == 0);
  // a different pin of the selected port is a different line
  r.bus.write(EXTI + RTSR1, 4, 1u << 12);
  r.c.call("drive_pin", {{"pin", 12}, {"level", 1}});
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 12)) == 0);  // line 12 still maps to A
  CHECK(r.syscfg.exti_port(12) == 0);
}

TEST_CASE("exti_pending_until_unmasked") {
  Rig r;
  r.pc13_input_pulldown();
  r.bus.write(SYSCFG + EXTICR4, 4, 2u << 4);
  r.bus.write(EXTI + RTSR1, 4, 1u << 13);
  r.c.call("drive_pin", {{"pin", 13}, {"level", 1}});  // masked: PR latches, no IRQ
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 13)) != 0);
  CHECK_FALSE(r.core.pending(40));
  r.c.call("drive_pin", {{"pin", 13}, {"level", 0}});
  r.c.call("drive_pin", {{"pin", 13}, {"level", 1}});  // second edge while pending: not doubled
  CHECK_FALSE(r.core.pending(40));
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4) | (1u << 13));  // unmask: IRQ fires once
  CHECK(r.core.pending(40));
  r.core.clear_pending(40);
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4));  // rewriting IMR does not re-fire
  CHECK_FALSE(r.core.pending(40));
  r.bus.write(EXTI + PR1, 4, 1u << 13);
  r.c.call("drive_pin", {{"pin", 13}, {"level", 0}});
  r.c.call("drive_pin", {{"pin", 13}, {"level", 1}});  // after clearing PR a new edge fires again
  CHECK(r.core.pending(40));
}

TEST_CASE("exti_swier") {
  Rig r;
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4) | (1u << 3));
  r.bus.write(EXTI + SWIER1, 4, 1u << 3);
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 3)) != 0);
  CHECK(r.core.pending(9));
  CHECK((r.bus.read(EXTI + SWIER1, 4) & (1u << 3)) != 0);
  r.bus.write(EXTI + PR1, 4, 1u << 3);  // clearing PR clears SWIER too
  CHECK((r.bus.read(EXTI + PR1, 4) & (1u << 3)) == 0);
  CHECK((r.bus.read(EXTI + SWIER1, 4) & (1u << 3)) == 0);
}

TEST_CASE("gpio: edges signal reports port, pin, level and time") {
  Rig r;
  std::vector<stm32l4::PinEdge> got;
  r.c.edges().subscribe([&](const stm32l4::PinEdge& e) { got.push_back(e); });
  r.clock.advance(500);
  r.c.call("drive_pin", {{"pin", 7}, {"level", 1}});
  r.c.call("drive_pin", {{"pin", 7}, {"level", 1}});  // no change, no edge
  r.c.call("release_pin", {{"pin", 7}});
  REQUIRE(got.size() == 2);
  CHECK(got[0].port == 'C');
  CHECK(got[0].pin == 7);
  CHECK(got[0].level == 1);
  CHECK(got[0].t_ns == 500);
  CHECK(got[1].level == 0);
}

namespace {
stm32l4::Exti* g_exti = nullptr;
Bus* g_bus = nullptr;
int g_isr_runs = 0, g_isr_clear_after = 0;
void exti_isr() {  // firmware-style EXTI15_10 handler; clears PR only on the Nth entry
  ++g_isr_runs;
  if (g_isr_runs >= g_isr_clear_after) g_bus->write(EXTI + PR1, 4, 1u << 13);
}
}  // namespace

TEST_CASE("exti: an ISR that does not clear PR re-enters (level-sensitive IRQ)") {
  Bus bus;
  Clock clock;
  static IrqHandler table[16 + 82];
  table[40 + 16] = &exti_isr;
  Cortexm core(bus, clock, 4, table, 16 + 82);
  ModelContext ctx(clock, core, bus);
  stm32l4::Syscfg syscfg("SYSCFG", json::object(), ctx);
  stm32l4::Gpio c("GPIOC", json{{"port", "C"}}, ctx);
  stm32l4::Exti exti("EXTI", json{{"irq", {6, 7, 8, 9, 10, 23, 23, 23, 23, 23, 40, 40, 40, 40, 40, 40}}}, ctx);
  ctx.add(&syscfg); ctx.add(&c); ctx.add(&exti);
  syscfg.connect(ctx); c.connect(ctx); exti.connect(ctx);
  bus.map(SYSCFG, 0x400, &syscfg);
  bus.map(EXTI, 0x400, &exti);
  bus.map(GPIOC, 0x400, &c);
  g_bus = &bus;
  g_isr_runs = 0;
  g_isr_clear_after = 3;
  bus.write(SYSCFG + EXTICR4, 4, 2u << 4);
  bus.write(EXTI + RTSR1, 4, 1u << 13);
  bus.write(EXTI + IMR1, 4, bus.read(EXTI + IMR1, 4) | (1u << 13));
  bus.write(0xE000E100 + 4, 4, 1u << (40 - 32));  // NVIC ISER1: enable IRQ 40
  c.call("drive_pin", {{"pin", 13}, {"level", 1}});
  core.deliver();
  CHECK(g_isr_runs == 3);
  CHECK(bus.read(EXTI + PR1, 4) == 0);
  CHECK_FALSE(core.pending(40));
  core.deliver();
  CHECK(g_isr_runs == 3);
}

TEST_CASE("exti: lines sharing an IRQ keep it high until every PR is cleared") {
  Rig r;
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4) | (3u << 10));
  r.bus.write(EXTI + SWIER1, 4, 3u << 10);
  CHECK(r.core.pending(40));
  r.core.clear_pending(40);
  r.bus.write(EXTI + PR1, 4, 1u << 10);  // line 11 still set: level stays high, nothing new pends
  CHECK_FALSE(r.core.pending(40));
  r.bus.write(EXTI + PR1, 4, 1u << 11);
  CHECK_FALSE(r.core.pending(40));
}

TEST_CASE("exti/syscfg: masks let firmware clear reset-1 bits and set I2C4_FMP") {
  Rig r;
  r.bus.write(EXTI + IMR1, 4, r.bus.read(EXTI + IMR1, 4) & ~(1u << 23));
  CHECK(r.bus.read(EXTI + IMR1, 4) == (0xFF820000u & ~(1u << 23)));
  r.bus.write(EXTI + 0x04, 4, 0xFFFFFFFF);  // EMR1: IM0..IM31 all writable
  CHECK(r.bus.read(EXTI + 0x04, 4) == 0xFFFFFFFF);
  r.bus.write(EXTI + 0x20, 4, r.bus.read(EXTI + 0x20, 4) & ~1u);  // IMR2 line 32
  CHECK(r.bus.read(EXTI + 0x20, 4) == 0x186);
  r.bus.write(EXTI + 0x20, 4, 0xFFFFFFFF);
  CHECK(r.bus.read(EXTI + 0x20, 4) == 0x1FF);
  r.bus.write(EXTI + 0x24, 4, 0xFFFFFFFF);
  CHECK(r.bus.read(EXTI + 0x24, 4) == 0x1FF);
  r.bus.write(SYSCFG + 0x04, 4, 0xFFFFFFFF);
  CHECK(r.bus.read(SYSCFG + 0x04, 4) == 0xFCFF0101);  // bit 23 writable, bit 9 reserved
}
