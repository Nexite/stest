// STM32L4 RCC / PWR / FLASH / GPIO models on a private Bus + Clock.
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/node.hpp"
#include "stm32l4/models/flash.hpp"
#include "stm32l4/models/gpio.hpp"
#include "stm32l4/models/pwr.hpp"
#include "stm32l4/models/rcc.hpp"
using namespace stest;
using nlohmann::json;

namespace {
constexpr uint64_t RCC = 0x40021000, PWR = 0x40007000, FLASH = 0x40022000, GPIOA = 0x48000000;
}

TEST_CASE("rcc: reset values, ready bits follow enables, SWS mirrors SW") {
  Bus bus;
  Clock clock;
  stm32l4::Rcc rcc("RCC", json::object(), clock);
  bus.map(RCC, 0x400, &rcc);
  CHECK(bus.read(RCC + 0x00, 4) == 0x00000063);  // MSION | MSIRDY | MSIRANGE=6
  CHECK(bus.read(RCC + 0x04, 4) == 0x40000000);  // ICSCR: HSITRIM=64 (L496)
  CHECK(bus.read(RCC + 0x0C, 4) == 0x00001000);
  CHECK(bus.read(RCC + 0x48, 4) == 0x00000100);  // AHB1ENR: FLASHEN
  CHECK(bus.read(RCC + 0x94, 4) == 0x0C000600);  // CSR
  // HSE bypass + enable -> HSERDY
  bus.write(RCC + 0x00, 4, bus.read(RCC, 4) | (1u << 18));
  bus.write(RCC + 0x00, 4, bus.read(RCC, 4) | (1u << 16));
  CHECK((bus.read(RCC, 4) & (1u << 17)) != 0);
  // PLL on/off
  bus.write(RCC + 0x00, 4, bus.read(RCC, 4) | (1u << 24));
  CHECK((bus.read(RCC, 4) & (1u << 25)) != 0);
  bus.write(RCC + 0x00, 4, bus.read(RCC, 4) & ~(1u << 24));
  CHECK((bus.read(RCC, 4) & (1u << 25)) == 0);
  // HSI
  bus.write(RCC + 0x00, 4, bus.read(RCC, 4) | (1u << 8));
  CHECK((bus.read(RCC, 4) & (1u << 10)) != 0);
  // ready bits cannot be written directly
  bus.write(RCC + 0x00, 4, 0x02000000u | 1u);  // PLLRDY without PLLON
  CHECK((bus.read(RCC, 4) & (1u << 25)) == 0);
  // SW -> SWS
  bus.write(RCC + 0x08, 4, 3);
  CHECK((bus.read(RCC + 0x08, 4) & 0xF) == 0xF);
  bus.write(RCC + 0x08, 4, 0x90 | 1);
  CHECK(bus.read(RCC + 0x08, 4) == (0x90u | 1u | (1u << 2)));
  // reserved offset is strict
  CHECK_THROWS_AS(bus.read(RCC + 0x24, 4), UnmodelledAccess);
  CHECK_THROWS_AS(bus.read(RCC + 0xA0, 4), UnmodelledAccess);
  CHECK_THROWS_AS(bus.write(RCC + 0x1C, 4, 0), UnmodelledAccess);  // CIFR read-only
  CHECK_NOTHROW(bus.write(RCC + 0x20, 4, 0x7FF));                  // CICR write-only
  // LSI / LSE ready
  bus.write(RCC + 0x94, 4, 1);
  CHECK((bus.read(RCC + 0x94, 4) & 2) != 0);
  bus.write(RCC + 0x90, 4, 1);
  CHECK((bus.read(RCC + 0x90, 4) & 2) != 0);
  // RMVF clears reset flags
  bus.write(RCC + 0x94, 4, bus.read(RCC + 0x94, 4) | (1u << 23));
  CHECK((bus.read(RCC + 0x94, 4) & 0xFF800000u) == 0);
}

TEST_CASE("pwr: VOS read-back, VOSF reads 0") {
  Bus bus;
  Clock clock;
  stm32l4::Pwr pwr("PWR", json::object(), clock);
  bus.map(PWR, 0x400, &pwr);
  CHECK(bus.read(PWR + 0x00, 4) == 0x00000200);
  CHECK(bus.read(PWR + 0x08, 4) == 0x00008000);
  bus.write(PWR + 0x00, 4, 0x400);
  CHECK(bus.read(PWR + 0x00, 4) == 0x400);
  CHECK((bus.read(PWR + 0x14, 4) & (1u << 10)) == 0);
  CHECK(bus.read(PWR + 0x64, 4) == 0);  // PDCRI exists on L496
  CHECK_THROWS_AS(bus.read(PWR + 0x1C, 4), UnmodelledAccess);
  CHECK_THROWS_AS(bus.write(PWR + 0x10, 4, 0), UnmodelledAccess);  // SR1 read-only
  CHECK_THROWS_AS(bus.write(PWR + 0x14, 4, 0), UnmodelledAccess);  // SR2 read-only
  CHECK_NOTHROW(bus.write(PWR + 0x18, 4, 0x1F));                   // SCR write-only
}

TEST_CASE("flash: ACR latency read-back, CR lock/unlock") {
  Bus bus;
  Clock clock;
  stm32l4::Flash flash("FLASH", json::object(), clock);
  bus.map(FLASH, 0x400, &flash);
  CHECK(bus.read(FLASH + 0x00, 4) == 0x00000600);
  bus.write(FLASH + 0x00, 4, 0x604);
  CHECK((bus.read(FLASH + 0x00, 4) & 7) == 4);
  CHECK(bus.read(FLASH + 0x14, 4) == 0xC0000000);
  bus.write(FLASH + 0x14, 4, 1);  // PG while locked: ignored
  CHECK(bus.read(FLASH + 0x14, 4) == 0xC0000000);
  bus.write(FLASH + 0x08, 4, 0x45670123);
  bus.write(FLASH + 0x08, 4, 0xCDEF89AB);
  CHECK(bus.read(FLASH + 0x14, 4) == 0x40000000);
  bus.write(FLASH + 0x14, 4, 0x80000000);  // re-lock
  CHECK(bus.read(FLASH + 0x14, 4) == 0xC0000000);
  CHECK_THROWS_AS(bus.read(FLASH + 0x1C, 4), UnmodelledAccess);
  CHECK_NOTHROW(bus.write(FLASH + 0x04, 4, 0x04152637));  // PDKEYR write-only
  CHECK_NOTHROW(bus.write(FLASH + 0x0C, 4, 0x08192A3B));  // OPTKEYR write-only
  CHECK_NOTHROW(bus.write(FLASH + 0x10, 4, 0x1));         // SR: EOP is w1c
}

TEST_CASE("gpio: output pin via BSRR/ODR/BRR, IDR, events, calls") {
  Bus bus;
  Clock clock;
  stm32l4::Gpio gpio("GPIOA", json{{"port", "A"}, {"moder_reset", 0xABFFFFFFu},
                                   {"ospeedr_reset", 0x0C000000u}, {"pupdr_reset", 0x64000000u}},
                     clock);
  bus.map(GPIOA, 0x400, &gpio);
  (void)take_events();
  CHECK(bus.read(GPIOA + 0x00, 4) == 0xABFFFFFF);
  CHECK(bus.read(GPIOA + 0x08, 4) == 0x0C000000);
  CHECK(bus.read(GPIOA + 0x0C, 4) == 0x64000000);
  CHECK(bus.read(GPIOA + 0x10, 4) == (1u << 13 | 1u << 15));  // pulled-up SWD pins
  CHECK_THROWS_AS(bus.read(GPIOA + 0x2C, 4), UnmodelledAccess);  // no ASCR on L496
  try { bus.write(GPIOA + 0x10, 4, 0); FAIL("IDR write did not throw"); }  // IDR is read-only
  catch (const UnmodelledAccess& e) { CHECK(e.read_only); CHECK(e.peripheral == "GPIOA"); }
  CHECK_NOTHROW(bus.write(GPIOA + 0x1C, 4, 0));  // LCKR: written by the model's lock sequence

  // PA5 output push-pull
  uint32_t m = bus.read(GPIOA, 4);
  bus.write(GPIOA, 4, (m & ~(3u << 10)) | (1u << 10));
  CHECK(take_events().empty());  // still low
  clock.advance(100);
  bus.write(GPIOA + 0x18, 4, 1u << 5);  // BS5
  CHECK(bus.read(GPIOA + 0x14, 4) == (1u << 5));
  CHECK((bus.read(GPIOA + 0x10, 4) & (1u << 5)) != 0);
  CHECK(bus.read(GPIOA + 0x18, 4) == 0);  // BSRR reads 0
  auto ev = take_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0].t_ns == 100);
  CHECK(ev[0].data.dump() == R"({"kind":"gpio","level":1,"pin":5,"port":"A"})");
  bus.write(GPIOA + 0x18, 4, 1u << 5);  // already high: no event
  CHECK(take_events().empty());
  bus.write(GPIOA + 0x28, 4, 1u << 5);  // BRR
  CHECK((bus.read(GPIOA + 0x10, 4) & (1u << 5)) == 0);
  CHECK(take_events().size() == 1);
  bus.write(GPIOA + 0x18, 4, (1u << 5) | (1u << 21));  // set and reset: set has priority
  CHECK((bus.read(GPIOA + 0x14, 4) & (1u << 5)) != 0);
  CHECK(take_events().size() == 1);
  bus.write(GPIOA + 0x14, 2, 0);  // half-word ODR write
  CHECK(gpio.call("get_pin", {{"pin", 5}}) == 0);
  bus.write(GPIOA + 0x14, 2, 1u << 5);
  CHECK(gpio.call("get_pin", {{"pin", 5}}) == 1);
  CHECK(take_events().size() == 2);

  // input pin with pull-up / pull-down / external drive
  m = bus.read(GPIOA, 4);
  bus.write(GPIOA, 4, m & ~(3u << 0));             // PA0 input
  CHECK(gpio.call("get_pin", {{"pin", 0}}) == 0);  // no pull -> 0
  bus.write(GPIOA + 0x0C, 4, bus.read(GPIOA + 0x0C, 4) | 1u);  // pull-up
  CHECK(gpio.call("get_pin", {{"pin", 0}}) == 1);
  CHECK((bus.read(GPIOA + 0x10, 4) & 1u) == 1u);
  gpio.call("drive_pin", {{"pin", 0}, {"level", 0}});
  CHECK(gpio.call("get_pin", {{"pin", 0}}) == 0);
  gpio.call("release_pin", {{"pin", 0}});
  CHECK(gpio.call("get_pin", {{"pin", 0}}) == 1);
  ev = take_events();
  REQUIRE(ev.size() == 3);  // pull-up 0->1, drive 1->0, release 0->1
  CHECK(ev[1].data["level"] == 0);
  // external drive overrides an output pin too
  gpio.call("drive_pin", {{"pin", 5}, {"level", 0}});
  CHECK((bus.read(GPIOA + 0x10, 4) & (1u << 5)) == 0);
  CHECK_THROWS(gpio.call("drive_pin", {{"pin", 16}, {"level", 0}}));
  CHECK_THROWS_AS(gpio.call("nope", json::object()), UnknownOp);
}

TEST_CASE("gpio: open-drain output follows pull when ODR=1") {
  Bus bus;
  Clock clock;
  stm32l4::Gpio gpio("GPIOC", json{{"port", "C"}}, clock);
  bus.map(0x48000800, 0x400, &gpio);
  CHECK(bus.read(0x48000800, 4) == 0xFFFFFFFF);  // default reset: analog
  bus.write(0x48000800, 4, 0xFFFFFFFC | 1u);     // PC0 output
  bus.write(0x48000804, 4, 1u);                  // open drain
  bus.write(0x48000818, 4, 1u);                  // ODR=1 -> released, no pull
  CHECK(gpio.call("get_pin", {{"pin", 0}}) == 0);
  bus.write(0x4800080C, 4, 1u);  // pull-up
  CHECK(gpio.call("get_pin", {{"pin", 0}}) == 1);
}

TEST_CASE("gpio: LCKR sequence locks configuration") {
  Bus bus;
  Clock clock;
  stm32l4::Gpio gpio("GPIOB", json{{"port", "B"}}, clock);
  bus.map(0x48000400, 0x400, &gpio);
  const uint64_t L = 0x48000400 + 0x1C;
  bus.write(L, 4, (1u << 16) | 1u);
  bus.write(L, 4, 1u);
  bus.write(L, 4, (1u << 16) | 1u);
  (void)bus.read(L, 4);
  CHECK(bus.read(L, 4) == ((1u << 16) | 1u));
  uint32_t m = bus.read(0x48000400, 4);
  bus.write(0x48000400, 4, m & ~3u);  // PB0 mode locked
  CHECK((bus.read(0x48000400, 4) & 3u) == (m & 3u));
  bus.write(0x48000400, 4, m & ~(3u << 2));  // PB1 not locked
  CHECK((bus.read(0x48000400, 4) & (3u << 2)) == 0);
}

#include <chrono>
#include <cstdio>
TEST_CASE("bench: Gpio::write ns/op" * doctest::skip()) {
  Bus bus;
  Clock clock;
  stm32l4::Gpio gpio("GPIOA", json{{"port", "A"}}, clock);
  bus.map(GPIOA, 0x400, &gpio);
  bus.write(GPIOA + 0x00, 4, 0x55555555);  // MODER: all outputs
  constexpr int N = 2000000;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) bus.write(GPIOA + 0x08, 4, uint32_t(i));  // OSPEEDR: no level change, so update() is pure compute
  auto t1 = std::chrono::steady_clock::now();
  std::printf("Gpio::write (OSPEEDR): %.1f ns/op\n",
              std::chrono::duration<double, std::nano>(t1 - t0).count() / N);
}
