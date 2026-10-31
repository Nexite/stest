// STM32L4 RCC clock tree (computed from the live register values) and typed lookup of the
// L4 models through a ModelContext.
#include <doctest/doctest.h>
#include <string>
#include <nlohmann/json.hpp>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/cortexm.hpp"
#include "stest/model_context.hpp"
#include "stm32l4/models/gpio.hpp"
#include "stm32l4/models/rcc.hpp"
using namespace stest;
using nlohmann::json;

namespace {
constexpr uint64_t RCC = 0x40021000;
enum : uint32_t { CR = 0x00, CFGR = 0x08, PLLCFGR = 0x0C, PLLSAI1CFGR = 0x10, PLLSAI2CFGR = 0x14,
                  AHB2ENR = 0x4C, APB1ENR1 = 0x58, CCIPR = 0x88, CSR = 0x94 };
const IrqHandler kTable[16] = {};

struct Fixture {
  Bus bus;
  Clock clock;
  Cortexm core{bus, clock, 4, kTable, 16};
  ModelContext ctx{clock, core, bus};
  stm32l4::Rcc rcc{"RCC", json::object(), ctx};
  Fixture() { bus.map(RCC, 0x400, &rcc); }
  uint32_t rd(uint32_t off) { return bus.read(RCC + off, 4); }
  void wr(uint32_t off, uint32_t v) { bus.write(RCC + off, 4, v); }
  void set(uint32_t off, uint32_t bits) { wr(off, rd(off) | bits); }
};
}  // namespace

TEST_CASE("model context: get<Gpio> / wrong type names both") {
  Fixture f;
  stm32l4::Gpio gpio("GPIOA", json{{"port", "A"}}, f.ctx);
  f.ctx.add(&f.rcc);
  f.ctx.add(&gpio);
  CHECK(&f.ctx.get<stm32l4::Gpio>("GPIOA") == &gpio);
  CHECK(&f.ctx.get<stm32l4::Rcc>("RCC") == &f.rcc);
  try {
    f.ctx.get<stm32l4::Rcc>("GPIOA");
    FAIL("expected logic_error");
  } catch (const std::logic_error& e) {
    std::string w = e.what();
    CHECK(w.find("GPIOA") != std::string::npos);
    CHECK(w.find("Rcc") != std::string::npos);
  }
}

TEST_CASE("rcc clocks: reset is MSI 4 MHz everywhere") {
  Fixture f;
  const ClockTree& t = f.rcc;
  CHECK(t.sysclk_hz() == 4000000);
  CHECK(t.hclk_hz() == 4000000);
  CHECK(t.pclk1_hz() == 4000000);
  CHECK(t.pclk2_hz() == 4000000);
  CHECK(t.pllsai1r_hz() == 0);
  CHECK(t.pllsai2r_hz() == 0);
  CHECK(t.adcsel() == 0);
  CHECK_FALSE(t.enabled(AHB2ENR, 0));
  CHECK(t.enabled(0x48, 8));  // AHB1ENR FLASHEN resets to 1
}

TEST_CASE("rcc clocks: HSE 8 MHz bypass, PLL M=2 N=40 R=2 -> 80 MHz") {
  Fixture f;
  f.set(CR, (1u << 18) | (1u << 16));                           // HSEBYP, HSEON
  f.wr(PLLCFGR, 3u | (1u << 4) | (40u << 8) | (1u << 24));      // HSE, M=2, N=40, REN, R=2
  f.set(CR, 1u << 24);                                          // PLLON
  f.wr(CFGR, 3u);                                               // SW = PLL, HPRE/PPRE = /1
  const ClockTree& t = f.rcc;
  CHECK(t.sysclk_hz() == 80000000);
  CHECK(t.hclk_hz() == 80000000);
  CHECK(t.pclk1_hz() == 80000000);
  CHECK(t.pclk2_hz() == 80000000);
  // Prescalers: HPRE=/2 (1000), PPRE1=/4 (101), PPRE2=/16 (111)
  f.wr(CFGR, 3u | (0x8u << 4) | (0x5u << 8) | (0x7u << 11));
  CHECK(t.sysclk_hz() == 80000000);
  CHECK(t.hclk_hz() == 40000000);
  CHECK(t.pclk1_hz() == 10000000);
  CHECK(t.pclk2_hz() == 2500000);
  f.wr(CFGR, 3u | (0xFu << 4));  // HPRE=/512
  CHECK(t.hclk_hz() == 80000000 / 512);
}

TEST_CASE("rcc clocks: SW sources, MSI range select, PLLSAI R outputs, ADCSEL") {
  Fixture f;
  const ClockTree& t = f.rcc;
  f.set(CR, 1u << 8);  // HSION
  f.wr(CFGR, 1u);
  CHECK(t.sysclk_hz() == 16000000);
  f.set(CR, 1u << 16);  // HSEON
  f.wr(CFGR, 2u);
  CHECK(t.sysclk_hz() == 8000000);
  f.wr(CFGR, 0u);
  f.wr(CR, (f.rd(CR) & ~0xF0u) | (0xBu << 4));  // MSIRANGE=11 (48 MHz), MSIRGSEL still 0
  CHECK(t.sysclk_hz() == 4000000);              // CSR.MSISRANGE (6) still selects
  f.set(CR, 1u << 3);                           // MSIRGSEL
  CHECK(t.sysclk_hz() == 48000000);
  // PLLSAI1: MSI 48 MHz / M=3 * N=8 / R=4 = 32 MHz; off until PLLSAI1ON and REN
  f.wr(PLLCFGR, 1u | (2u << 4) | (8u << 8));
  f.wr(PLLSAI1CFGR, (8u << 8) | (1u << 25));
  CHECK(t.pllsai1r_hz() == 0);
  f.set(CR, 1u << 26);  // PLLSAI1ON
  CHECK(t.pllsai1r_hz() == 0);
  f.set(PLLSAI1CFGR, 1u << 24);  // PLLSAI1REN
  CHECK(t.pllsai1r_hz() == 32000000);
  // PLLSAI2: N=16, R=8 -> 48/3*16/8 = 32 MHz
  f.wr(PLLSAI2CFGR, (16u << 8) | (3u << 25) | (1u << 24));
  CHECK(t.pllsai2r_hz() == 0);
  f.set(CR, 1u << 28);  // PLLSAI2ON
  CHECK(t.pllsai2r_hz() == 32000000);
  f.wr(CCIPR, 1u << 28);
  CHECK(t.adcsel() == 1);
  f.wr(CCIPR, 3u << 28);
  CHECK(t.adcsel() == 3);
  // Enable bits are read live
  f.set(APB1ENR1, 1u << 28);
  CHECK(t.enabled(APB1ENR1, 28));
  f.wr(APB1ENR1, 0);
  CHECK_FALSE(t.enabled(APB1ENR1, 28));
  const uint32_t* w = t.enable_word(AHB2ENR);  // live: tracks later writes
  CHECK(*w == 0);
  f.wr(AHB2ENR, 2);
  CHECK(*w == 2);
  CHECK_THROWS(t.enable_word(0x44));  // reserved offset
}

TEST_CASE("rcc clocks: hse_hz param") {
  Bus bus;
  Clock clock;
  Cortexm core(bus, clock, 4, kTable, 16);
  ModelContext ctx(clock, core, bus);
  stm32l4::Rcc rcc("RCC", json{{"hse_hz", 12000000}}, ctx);
  bus.map(RCC, 0x400, &rcc);
  bus.write(RCC + CR, 4, bus.read(RCC + CR, 4) | (1u << 16));
  bus.write(RCC + CFGR, 4, 2u);
  CHECK(rcc.sysclk_hz() == 12000000);
}
