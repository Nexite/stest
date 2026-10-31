// STM32L4 timers (TIM1-8, TIM15-17) on a private Bus + Clock + Cortexm + Rcc (80 MHz PLL).
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/cortexm.hpp"
#include "stest/model_context.hpp"
#include "stm32l4/models/rcc.hpp"
#include "stm32l4/models/tim.hpp"
using namespace stest;
using nlohmann::json;

namespace {
const IrqHandler kTable[16 + 91] = {};
constexpr uint64_t RCC = 0x40021000;
constexpr uint64_t TIM1 = 0x40012C00, TIM2 = 0x40000000, TIM7 = 0x40001400, TIM16 = 0x40014400;
enum : uint32_t {
  CR1 = 0x00, CR2 = 0x04, SMCR = 0x08, DIER = 0x0C, SR = 0x10, EGR = 0x14, CCMR1 = 0x18,
  CCMR2 = 0x1C, CCER = 0x20, CNT = 0x24, PSC = 0x28, ARR = 0x2C, RCR = 0x30, CCR1 = 0x34,
  CCR2 = 0x38, BDTR = 0x44, DMAR = 0x4C, OR1 = 0x50,
};
constexpr uint32_t CEN = 1, UDIS = 2, URS = 4, OPM = 8, DIR = 16, ARPE = 0x80;
constexpr uint32_t UIF = 1, CC1IF = 2;
constexpr int IRQ_TIM7 = 55, IRQ_TIM2 = 28, IRQ_TIM1_UP_TIM16 = 25, IRQ_TIM1_CC = 27;
constexpr uint64_t kNone = UINT64_MAX;

json tim_params(const char* kind, int width, const char* bus, json irq) {
  return {{"kind", kind}, {"width", width}, {"bus", bus}, {"irq", std::move(irq)}};
}

struct Rig {
  Bus bus;
  Clock clock;
  Cortexm core{bus, clock, 4, kTable, 16 + 91};
  ModelContext ctx{clock, core, bus};
  stm32l4::Rcc rcc{"RCC", json::object(), ctx};
  stm32l4::Tim tim7{"TIM7", tim_params("basic", 16, "apb1", {{"update", IRQ_TIM7}}), ctx};
  stm32l4::Tim tim2{"TIM2",
                    [] {
                      json p = tim_params("general", 32, "apb1",
                                          {{"update", 28}, {"cc", 28}, {"trg_com", 28}});
                      p["opt"] = {{0x50, 0}, {0x60, 0}};
                      return p;
                    }(),
                    ctx};
  stm32l4::Tim tim1{"TIM1",
                    [] {
                      json p = tim_params("advanced", 16, "apb2",
                                          {{"update", 25}, {"cc", 27}, {"break", 24}, {"trg_com", 26}});
                      p["opt"] = {{0x50, 0}, {0x60, 1}, {0x64, 1}};
                      return p;
                    }(),
                    ctx};
  stm32l4::Tim tim16{"TIM16",
                     [] {
                       json p = tim_params("general", 16, "apb2",
                                           {{"update", 25}, {"cc", 25}, {"break", 25}, {"trg_com", 25}});
                       p["channels"] = 1;
                       p["complementary"] = 1;
                       p["up_only"] = true;
                       p["opt"] = {{0x50, 0}, {0x60, 1}};
                       return p;
                     }(),
                     ctx};
  Rig() {
    for (Peripheral* p : std::vector<Peripheral*>{&rcc, &tim7, &tim2, &tim1, &tim16}) {
      p->reset();
      ctx.add(p);
    }
    ctx.set_clock_tree(&rcc);
    for (Peripheral* p : ctx.models()) p->connect(ctx);
    bus.map(RCC, 0x400, &rcc);
    bus.map(TIM7, 0x400, &tim7);
    bus.map(TIM2, 0x400, &tim2);
    bus.map(TIM1, 0x400, &tim1);
    bus.map(TIM16, 0x400, &tim16);
    // HSE 8 MHz bypass, PLL M=2 N=40 R=2 -> SYSCLK = HCLK = PCLK1 = PCLK2 = 80 MHz
    rcc_set(0x00, (1u << 18) | (1u << 16));
    bus.write(RCC + 0x0C, 4, 3u | (1u << 4) | (40u << 8) | (1u << 24));
    rcc_set(0x00, 1u << 24);
    bus.write(RCC + 0x08, 4, 3u);
  }
  void rcc_set(uint32_t off, uint32_t bits) { bus.write(RCC + off, 4, bus.read(RCC + off, 4) | bits); }
  uint32_t rd(uint64_t base, uint32_t off) { return bus.read(base + off, 4); }
  void wr(uint64_t base, uint32_t off, uint32_t v) { bus.write(base + off, 4, v); }
  void to(uint64_t t) {  // advance virtual time to t, firing events on the way
    while (clock.next_event_ns() <= t) {
      clock.advance(clock.next_event_ns() - clock.now_ns());
      clock.run_due();
    }
    clock.advance(t - clock.now_ns());
  }
  uint64_t fire_next() {  // fire the next clock event, return its time (kNone if none)
    uint64_t t = clock.next_event_ns();
    if (t == kNone) return kNone;
    to(t);
    return t;
  }
  // Fire events until `base` has UIF set; returns that time and clears SR (CC events with
  // CCRx = 0 also fire at every wrap, as on hardware).
  uint64_t next_uif(uint64_t base) {
    for (;;) {
      uint64_t t = fire_next();
      if (t == kNone) return kNone;
      if (rd(base, SR) & UIF) {
        wr(base, SR, 0);
        return t;
      }
      wr(base, SR, 0);
    }
  }
  // Fire events until `base` has CC1IF set; returns that time in ticks of 12.5 ns since t0
  // and clears only CC1IF (UIF and the other flags stay as they are).
  uint64_t next_cc1(uint64_t base, uint64_t t0) {
    for (;;) {
      uint64_t t = fire_next();
      if (t == kNone) return kNone;
      if (rd(base, SR) & CC1IF) {
        wr(base, SR, ~CC1IF);  // rc_w0: only the 0 bit clears
        return (t - t0) * 2 / 25;
      }
    }
  }
  // Configure a timer through the HAL's base-init sequence: ARR, PSC, URS+UG, CR1.
  void base_init(uint64_t base, uint32_t psc, uint32_t arr, uint32_t cr1 = 0) {
    wr(base, ARR, arr);
    wr(base, PSC, psc);
    wr(base, CR1, rd(base, CR1) | URS);
    wr(base, EGR, 1);
    wr(base, CR1, cr1);
  }
};
}  // namespace

TEST_CASE("tim reset values and register map") {
  Rig r;
  CHECK(r.rd(TIM7, ARR) == 0xFFFF);
  CHECK(r.rd(TIM2, ARR) == 0xFFFFFFFF);
  CHECK(r.rd(TIM7, CR1) == 0);
  CHECK(r.rd(TIM7, SMCR) == 0);  // reserved on basic timers: reads 0 (HAL reads it) ...
  CHECK_THROWS_AS(r.wr(TIM7, SMCR, 0), UnmodelledAccess);  // ... but is not writable
  CHECK_THROWS_AS(r.rd(TIM7, CCMR1), UnmodelledAccess);    // no channels on TIM6/7
  CHECK_THROWS_AS(r.rd(TIM2, RCR), UnmodelledAccess);      // no RCR / BDTR on TIM2-5
  CHECK_THROWS_AS(r.rd(TIM2, BDTR), UnmodelledAccess);
  CHECK(r.rd(TIM1, 0x60) == 1);  // TIM1_OR2 BKINE
  CHECK(r.rd(TIM16, 0x60) == 1);
  CHECK_THROWS_AS(r.rd(TIM16, CCMR2), UnmodelledAccess);  // TIM16: one channel
  r.wr(TIM16, CR1, DIR | (3u << 5));  // TIM16 counts up only: DIR/CMS are reserved
  CHECK(r.rd(TIM16, CR1) == 0);
}

TEST_CASE("tim_update_rate") {  // 80 MHz, PSC 0, ARR 3999 -> an update every 50 000 ns
  Rig r;
  r.base_init(TIM7, 0, 3999);
  CHECK(r.rd(TIM7, SR) == 0);  // URS: UG did not set UIF
  r.wr(TIM7, DIER, 1);
  r.to(1000);
  r.wr(TIM7, CR1, CEN);
  for (uint64_t k = 1; k <= 5; ++k) {
    if (k == 3) {
      r.to(1000 + 2 * 50000 + 25000);
      CHECK(r.rd(TIM7, CNT) == 2000);
    }
    CHECK(r.fire_next() == 1000 + k * 50000);
    CHECK(r.rd(TIM7, SR) == UIF);
    CHECK(r.core.pending(IRQ_TIM7));
    r.wr(TIM7, SR, 0);  // rc_w0
    r.core.clear_pending(IRQ_TIM7);
  }
  json st = r.tim7.call("state", json::object());
  CHECK(st["running"] == true);
  CHECK(st["arr"] == 3999);
  CHECK(st["psc"] == 0);
  CHECK(st["clk_hz"] == 80000000);
  CHECK(st["update_hz"].get<double>() == doctest::Approx(20000.0));
  CHECK(st["channels"].empty());
}

TEST_CASE("tim_arr_zero_no_storm") {
  Rig r;
  r.base_init(TIM7, 0, 0);
  r.wr(TIM7, DIER, 1);
  r.wr(TIM7, CR1, CEN);
  CHECK(r.clock.next_event_ns() == kNone);  // counter blocked while ARR = 0
  r.to(1000000);
  CHECK(r.rd(TIM7, CNT) == 0);
  CHECK(r.rd(TIM7, SR) == 0);
  CHECK_FALSE(r.core.pending(IRQ_TIM7));
  // ARR rewritten to 0 while running (no preload) blocks it as well
  r.wr(TIM7, ARR, 99);
  CHECK(r.clock.next_event_ns() != kNone);
  r.wr(TIM7, ARR, 0);
  CHECK(r.clock.next_event_ns() == kNone);
  // UIF left set with UIE = 0: nothing observable changes at later updates -> no events
  r.wr(TIM7, DIER, 0);
  r.wr(TIM7, ARR, 99);
  r.fire_next();
  CHECK(r.rd(TIM7, SR) == UIF);
  CHECK(r.clock.next_event_ns() == kNone);
  r.to(r.clock.now_ns() + 1000000);
  CHECK(r.rd(TIM7, SR) == UIF);
}

TEST_CASE("tim_stop_no_irq") {
  Rig r;
  r.base_init(TIM7, 0, 3999);
  r.wr(TIM7, DIER, 1);
  r.wr(TIM7, CR1, CEN);
  CHECK(r.fire_next() == 50000);
  CHECK(r.core.pending(IRQ_TIM7));
  r.to(60000);
  uint32_t cnt = r.rd(TIM7, CNT);
  CHECK(cnt == 800);
  r.wr(TIM7, CR1, 0);  // CEN cleared with UIF pending
  r.wr(TIM7, SR, 0);
  r.core.clear_pending(IRQ_TIM7);
  CHECK(r.clock.next_event_ns() == kNone);
  r.to(10000000);
  CHECK_FALSE(r.core.pending(IRQ_TIM7));
  CHECK(r.rd(TIM7, SR) == 0);
  CHECK(r.rd(TIM7, CNT) == cnt);  // frozen
  CHECK(r.tim7.call("state", json::object())["running"] == false);
  // restart continues from the held count
  r.wr(TIM7, CR1, CEN);
  CHECK(r.fire_next() == 10000000 + (4000 - cnt) * 25 / 2);
}

TEST_CASE("tim_preload") {
  Rig r;
  r.base_init(TIM7, 0, 999, ARPE);
  r.wr(TIM7, DIER, 1);
  r.wr(TIM7, CR1, ARPE | CEN);
  r.to(5000);              // 400 ticks in
  r.wr(TIM7, ARR, 1999);   // buffered: the current period still ends at 1000 ticks
  r.wr(TIM7, PSC, 1);      // PSC is always buffered
  CHECK(r.rd(TIM7, ARR) == 1999);
  CHECK(r.fire_next() == 12500);
  r.wr(TIM7, SR, 0);
  // next period: 2000 ticks of 25 ns
  CHECK(r.fire_next() == 12500 + 50000);
  r.wr(TIM7, SR, 0);
  // ARPE = 0: an ARR write takes effect immediately
  r.wr(TIM7, CR1, CEN);
  r.to(62500 + 25 * 100);  // CNT = 100
  CHECK(r.rd(TIM7, CNT) == 100);
  r.wr(TIM7, ARR, 199);
  CHECK(r.fire_next() == 62500 + 25 * 200);
  // CCR preload (OCxPE): TIM2 CCR1 with OC1PE takes effect at the next update
  r.base_init(TIM2, 0, 999);
  r.wr(TIM2, CCMR1, 6u << 4 | 1u << 3);  // PWM1, OC1PE
  r.wr(TIM2, CCR1, 250);
  r.wr(TIM2, EGR, 1);  // UG loads the preload
  r.wr(TIM2, SR, 0);
  r.wr(TIM2, CR1, CEN);
  uint64_t t0 = r.clock.now_ns();
  r.wr(TIM2, CCR1, 500);  // buffered
  CHECK(r.tim2.call("state", json::object())["channels"][0]["duty"].get<double>() ==
        doctest::Approx(0.25));
  CHECK(r.fire_next() == t0 + 250 * 25 / 2);  // CC1IF at the old CCR1
  CHECK(r.rd(TIM2, SR) == CC1IF);
  CHECK(r.fire_next() == t0 + 1000 * 25 / 2);  // update transfers CCR1 = 500
  CHECK(r.tim2.call("state", json::object())["channels"][0]["duty"].get<double>() ==
        doctest::Approx(0.5));
  r.wr(TIM2, SR, 0);
  CHECK(r.fire_next() == t0 + 1500 * 25 / 2);
}

TEST_CASE("tim_apb_x2") {  // PPRE1 = /2 -> PCLK1 40 MHz, timer clock x2 = 80 MHz
  Rig r;
  r.wr(RCC, 0x08, 3u | (4u << 8));
  CHECK(r.rcc.pclk1_hz() == 40000000);
  r.base_init(TIM7, 0, 3999);
  r.wr(TIM7, CR1, CEN);
  CHECK(r.tim7.call("state", json::object())["clk_hz"] == 80000000);
  CHECK(r.fire_next() == 50000);
  // PPRE1 = /1: timer clock = PCLK1 (no doubling); read when the timer reschedules
  r.wr(TIM7, CR1, 0);
  r.wr(RCC, 0x08, 3u);
  r.wr(TIM7, SR, 0);
  r.wr(TIM7, CNT, 0);
  r.wr(TIM7, CR1, CEN);
  CHECK(r.tim7.call("state", json::object())["clk_hz"] == 80000000);
  // PPRE1 = /4 (20 MHz) -> 40 MHz timer clock
  r.wr(TIM7, CR1, 0);
  r.wr(RCC, 0x08, 3u | (5u << 8));
  r.wr(TIM7, CR1, CEN);
  CHECK(r.tim7.call("state", json::object())["clk_hz"] == 40000000);
}

TEST_CASE("tim_pwm_state") {
  Rig r;
  r.base_init(TIM2, 0, 999);
  r.wr(TIM2, CCMR1, (6u << 4) | (7u << 12));  // CH1 PWM1, CH2 PWM2
  r.wr(TIM2, CCR1, 250);
  r.wr(TIM2, CCR2, 250);
  r.wr(TIM2, CCER, 1u | (1u << 4) | (1u << 5));  // CC1E, CC2E, CC2P
  r.wr(TIM2, CR1, CEN);
  json st = r.tim2.call("state", json::object());
  CHECK(st["running"] == true);
  CHECK(st["arr"] == 999);
  CHECK(st["clk_hz"] == 80000000);
  CHECK(st["update_hz"].get<double>() == doctest::Approx(80000.0));
  REQUIRE(st["channels"].size() == 4);
  json c1 = st["channels"][0], c2 = st["channels"][1], c3 = st["channels"][2];
  CHECK(c1["ch"] == 1);
  CHECK(c1["enabled"] == true);
  CHECK(c1["mode"] == "pwm1");
  CHECK(c1["duty"].get<double>() == doctest::Approx(0.25));
  CHECK(c1["polarity"] == "high");
  CHECK(c2["mode"] == "pwm2");
  CHECK(c2["duty"].get<double>() == doctest::Approx(0.75));
  CHECK(c2["polarity"] == "low");
  CHECK(c3["ch"] == 3);
  CHECK(c3["enabled"] == false);
  CHECK(c3["mode"] == "frozen");
  CHECK_FALSE(c1.contains("duty_n"));  // TIM2 has no complementary outputs
  // CCR above ARR: PWM1 always active
  r.wr(TIM2, CCR1, 5000);
  CHECK(r.tim2.call("state", json::object())["channels"][0]["duty"].get<double>() == 1.0);
}

TEST_CASE("tim_down_count_underflow") {  // DIR = 1, ARR 999 -> UEV every 1000 ticks
  Rig r;
  r.wr(TIM2, ARR, 999);
  r.wr(TIM2, CR1, DIR | URS);
  r.wr(TIM2, EGR, 1);
  CHECK(r.rd(TIM2, CNT) == 999);  // UG reloads ARR when counting down
  r.wr(TIM2, DIER, 1);
  r.wr(TIM2, CR1, DIR | CEN);
  r.to(100 * 25 / 2);
  CHECK(r.rd(TIM2, CNT) == 899);
  r.to(12490);  // 999.2 ticks
  CHECK(r.rd(TIM2, CNT) == 0);
  for (uint64_t k = 1; k <= 3; ++k) {
    CHECK(r.next_uif(TIM2) == k * 12500);
    CHECK(r.rd(TIM2, CNT) == 999);
  }
  CHECK(r.tim2.call("state", json::object())["update_hz"].get<double>() == doctest::Approx(80000.0));
}

TEST_CASE("tim_center_aligned_update_rate") {
  // CMS=1, ARR 1000: the counter period is 2000 ticks (0 -> 1000 -> 0); RM0351 sets UIF at
  // both the overflow (tick 1000) and the underflow (tick 2000) without a repetition counter.
  Rig r;
  r.base_init(TIM2, 0, 1000, 1u << 5);
  r.wr(TIM2, CCR1, 400);
  r.wr(TIM2, DIER, 1);
  r.wr(TIM2, CR1, (1u << 5) | CEN);
  r.to(500 * 25 / 2);
  CHECK(r.rd(TIM2, CNT) == 500);
  CHECK((r.rd(TIM2, CR1) & DIR) == 0);  // counting up
  std::vector<uint64_t> ups;
  for (int i = 0; i < 4; ++i) ups.push_back(r.next_uif(TIM2));
  CHECK(ups == std::vector<uint64_t>{12500, 25000, 37500, 50000});
  r.to(50000 + 1500 * 25 / 2);
  CHECK(r.rd(TIM2, CNT) == 500);
  CHECK((r.rd(TIM2, CR1) & DIR) == DIR);  // counting down; DIR is read-only in center mode
  CHECK(r.tim2.call("state", json::object())["update_hz"].get<double>() == doctest::Approx(80000.0));

  // CC1IF: CMS=1 -> only on the down-crossing (tick 1600), CMS=3 -> both (400 and 1600)
  auto cc_times = [&](uint32_t cms) {
    r.wr(TIM2, CR1, 0);
    r.wr(TIM2, DIER, 0);
    r.base_init(TIM2, 0, 1000, cms << 5);
    r.wr(TIM2, SR, 0);
    r.wr(TIM2, DIER, 2);  // CC1IE
    uint64_t t0 = r.clock.now_ns();
    r.wr(TIM2, CR1, (cms << 5) | CEN);
    std::vector<uint64_t> cc;
    while (cc.size() < 3) {
      uint64_t t = r.fire_next();
      REQUIRE(t != kNone);
      if (r.rd(TIM2, SR) & CC1IF) cc.push_back((t - t0) * 2 / 25);  // in ticks
      r.wr(TIM2, SR, 0);
    }
    return cc;
  };
  CHECK(cc_times(1) == std::vector<uint64_t>{1600, 3600, 5600});
  CHECK(cc_times(2) == std::vector<uint64_t>{400, 2400, 4400});
  CHECK(cc_times(3) == std::vector<uint64_t>{400, 1600, 2400});
  // center-aligned PWM1 duty = CCR / ARR
  r.wr(TIM2, CCMR1, 6u << 4);
  r.wr(TIM2, CCR1, 250);
  CHECK(r.tim2.call("state", json::object())["channels"][0]["duty"].get<double>() ==
        doctest::Approx(0.25));
}

TEST_CASE("tim_rcr_gates_uev") {  // TIM1 RCR = 2 -> an update every 3rd period
  Rig r;
  r.wr(TIM1, RCR, 2);
  r.base_init(TIM1, 0, 999);  // UG loads the repetition counter
  r.wr(TIM1, DIER, 1);
  r.wr(TIM1, CR1, CEN);
  for (uint64_t k = 1; k <= 3; ++k) {
    CHECK(r.next_uif(TIM1) == k * 3 * 12500);
    CHECK(r.core.pending(IRQ_TIM1_UP_TIM16));
    r.core.clear_pending(IRQ_TIM1_UP_TIM16);
  }
  CHECK(r.tim1.call("state", json::object())["update_hz"].get<double>() ==
        doctest::Approx(80000.0 / 3));
  // center-aligned with RCR = 1: one update per full counter period (2 * ARR ticks)
  r.wr(TIM1, CR1, 0);
  r.wr(TIM1, RCR, 1);
  r.base_init(TIM1, 0, 1000, 1u << 5);
  r.wr(TIM1, SR, 0);
  uint64_t t0 = r.clock.now_ns();
  r.wr(TIM1, CR1, (1u << 5) | CEN);
  CHECK(r.next_uif(TIM1) == t0 + 2000 * 25 / 2);
  CHECK(r.next_uif(TIM1) == t0 + 4000 * 25 / 2);
}

TEST_CASE("tim1_complementary_dead_time") {
  Rig r;
  r.base_init(TIM1, 0, 999);
  r.wr(TIM1, CCMR1, 6u << 4);  // CH1 PWM1
  r.wr(TIM1, CCR1, 250);
  r.wr(TIM1, CCER, 1u | 4u);   // CC1E | CC1NE
  r.wr(TIM1, BDTR, 0x20);      // DTG = 32 x t_DTS (CKD = 0: 12.5 ns) = 400 ns, MOE = 0
  r.wr(TIM1, CR1, CEN);
  json c1 = r.tim1.call("state", json::object())["channels"][0];
  CHECK(c1["enabled"] == false);  // advanced timer outputs need BDTR.MOE
  CHECK(c1["enabled_n"] == false);
  r.wr(TIM1, BDTR, 0x20 | (1u << 15));
  c1 = r.tim1.call("state", json::object())["channels"][0];
  CHECK(c1["enabled"] == true);
  CHECK(c1["enabled_n"] == true);
  CHECK(c1["dead_time_ns"].get<double>() == doctest::Approx(400.0));
  CHECK(c1["ref_duty"].get<double>() == doctest::Approx(0.25));
  // period 12 500 ns: OC1 high 3125 - 400 ns, OC1N high 9375 - 400 ns
  CHECK(c1["duty"].get<double>() == doctest::Approx(2725.0 / 12500));
  CHECK(c1["duty_n"].get<double>() == doctest::Approx(8975.0 / 12500));
  // CKD = /2 doubles t_DTS; DTG = 0xA0 -> (64 + 32) x 2 x t_DTS
  r.wr(TIM1, CR1, CEN | (1u << 8));
  r.wr(TIM1, BDTR, 0xA0 | (1u << 15));
  CHECK(r.tim1.call("state", json::object())["channels"][0]["dead_time_ns"].get<double>() ==
        doctest::Approx(96 * 2 * 25.0));
  // DTG 110xxxxx: (32 + x) x 8 x t_DTS; 111xxxxx: (32 + x) x 16 x t_DTS
  r.wr(TIM1, CR1, CEN);
  r.wr(TIM1, BDTR, 0xC1 | (1u << 15));
  CHECK(r.tim1.call("state", json::object())["channels"][0]["dead_time_ns"].get<double>() ==
        doctest::Approx(33 * 8 * 12.5));
  r.wr(TIM1, BDTR, 0xE2 | (1u << 15));
  CHECK(r.tim1.call("state", json::object())["channels"][0]["dead_time_ns"].get<double>() ==
        doctest::Approx(34 * 16 * 12.5));
  // only CC1NE: OC1N carries OC1REF without dead time
  r.wr(TIM1, CCER, 4u);
  c1 = r.tim1.call("state", json::object())["channels"][0];
  CHECK(c1["enabled"] == false);
  CHECK(c1["duty_n"].get<double>() == doctest::Approx(0.25));
  // TIM16 CH1N is reported too
  json t16 = r.tim16.call("state", json::object())["channels"];
  CHECK(t16.size() == 1);
  CHECK(t16[0].contains("duty_n"));
}

TEST_CASE("tim: TIM1 update and TIM16 share TIM1_UP_TIM16 (level = OR of both)") {
  Rig r;
  r.base_init(TIM1, 0, 999);
  r.base_init(TIM16, 0, 999);
  r.wr(TIM1, DIER, 1);
  r.wr(TIM16, DIER, 1);
  r.wr(TIM1, EGR, 1);   // URS = 0 now: UG sets UIF
  r.wr(TIM16, EGR, 1);
  CHECK(r.core.pending(IRQ_TIM1_UP_TIM16));
  r.core.clear_pending(IRQ_TIM1_UP_TIM16);
  r.wr(TIM1, SR, 0);    // TIM16 still holds the line high: no new edge, nothing pending
  CHECK_FALSE(r.core.pending(IRQ_TIM1_UP_TIM16));
  r.wr(TIM1, EGR, 1);   // TIM1 re-asserts while the shared line is still high: no new pend
  CHECK_FALSE(r.core.pending(IRQ_TIM1_UP_TIM16));
  r.wr(TIM1, SR, 0);
  r.wr(TIM16, SR, 0);   // both low
  r.wr(TIM16, EGR, 1);  // rising edge again
  CHECK(r.core.pending(IRQ_TIM1_UP_TIM16));
  // TIM1 CC goes to its own vector
  r.wr(TIM1, DIER, 2);
  r.wr(TIM1, EGR, 2);  // CC1G
  CHECK(r.core.pending(IRQ_TIM1_CC));
}

TEST_CASE("tim: unsupported features abort (not silently ignored)") {
  Rig r;
  auto msg = [&](uint64_t base, uint32_t off, uint32_t v) -> std::string {
    try {
      r.wr(base, off, v);
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  };
  CHECK(msg(TIM2, SMCR, 4).find("TIM2: slave mode") != std::string::npos);
  CHECK(msg(TIM2, SMCR, 1u << 14).find("external clock mode 2") != std::string::npos);
  CHECK(msg(TIM2, SMCR, 0x70).empty());  // TS without SMS: harmless
  CHECK(msg(TIM2, CCMR1, 1).find("input capture") != std::string::npos);
  CHECK(msg(TIM2, CCMR1, 1u << 16).find("not modelled") != std::string::npos);  // OC1M[3]
  CHECK(msg(TIM2, CCMR1, 1u << 7).find("OCREF clear") != std::string::npos);
  CHECK(msg(TIM2, DIER, 1u << 8).find("DMA") != std::string::npos);
  CHECK(msg(TIM2, OR1, 1).find("not modelled") != std::string::npos);
  CHECK(msg(TIM1, BDTR, 1u << 12).find("break input") != std::string::npos);
  CHECK(msg(TIM1, CR2, 1).find("CCPC") != std::string::npos);
  CHECK(msg(TIM1, 0x60, 0).find("not modelled") != std::string::npos);  // OR2 BKINE cleared
  CHECK(msg(TIM1, 0x20, 1u << 16).find("channels 5/6") != std::string::npos);
  CHECK_THROWS(r.rd(TIM2, DMAR));
  CHECK(msg(TIM2, CR2, 2u << 4).empty());  // TRGO select: stored (consumers check triggers)
  CHECK(msg(TIM2, SMCR, 0).empty());
}

TEST_CASE("tim: OPM stops at the update, UDIS suppresses it, URS=0 UG sets UIF") {
  Rig r;
  r.base_init(TIM7, 0, 99);
  r.wr(TIM7, CR1, OPM | CEN);
  CHECK(r.fire_next() == 100 * 25 / 2);
  CHECK(r.rd(TIM7, SR) == UIF);
  CHECK((r.rd(TIM7, CR1) & CEN) == 0);
  CHECK(r.clock.next_event_ns() == kNone);
  r.wr(TIM7, SR, 0);
  r.wr(TIM7, CR1, UDIS | CEN);
  CHECK(r.clock.next_event_ns() == kNone);  // no update events, UIF never sets
  r.to(r.clock.now_ns() + 100000);
  CHECK(r.rd(TIM7, SR) == 0);
  r.wr(TIM7, CR1, CEN);
  r.wr(TIM7, EGR, 1);
  CHECK(r.rd(TIM7, SR) == UIF);
  CHECK(r.rd(TIM7, CNT) == 0);
}

TEST_CASE("tim: after an update, compare events stay on time with UIF left set") {
  // Regression: after the UEV renumbered the counter positions the next event was computed
  // in the old numbering, one or more periods late. No test here clears UIF.
  using V = std::vector<uint64_t>;
  SUBCASE("up") {
    Rig r;
    r.base_init(TIM2, 0, 999);
    r.wr(TIM2, CCR1, 500);
    uint64_t t0 = r.clock.now_ns();
    r.wr(TIM2, CR1, CEN);
    V cc;
    for (int i = 0; i < 4; ++i) cc.push_back(r.next_cc1(TIM2, t0));
    CHECK(cc == V{500, 1500, 2500, 3500});
    CHECK((r.rd(TIM2, SR) & UIF) != 0);
  }
  SUBCASE("up, exact probe times") {
    Rig r;
    r.base_init(TIM2, 0, 999);
    r.wr(TIM2, CCR1, 500);
    r.wr(TIM2, CR1, CEN);
    CHECK(r.fire_next() == 6250);
    r.wr(TIM2, SR, ~CC1IF);
    CHECK(r.fire_next() == 12500);  // UEV (+ CC2-4 at CNT = 0)
    CHECK(r.fire_next() == 18750);  // CC1 in the next period
    CHECK((r.rd(TIM2, SR) & CC1IF) != 0);
  }
  SUBCASE("down") {
    Rig r;
    r.wr(TIM2, ARR, 999);
    r.wr(TIM2, CR1, DIR | URS);
    r.wr(TIM2, EGR, 1);
    r.wr(TIM2, CR1, DIR);
    r.wr(TIM2, CCR1, 500);
    uint64_t t0 = r.clock.now_ns();
    r.wr(TIM2, CR1, DIR | CEN);
    V cc;
    for (int i = 0; i < 4; ++i) cc.push_back(r.next_cc1(TIM2, t0));
    CHECK(cc == V{499, 1499, 2499, 3499});  // CNT 999 -> 500 takes 499 ticks
    CHECK((r.rd(TIM2, SR) & UIF) != 0);
  }
  SUBCASE("center-aligned 3") {
    Rig r;
    r.base_init(TIM2, 0, 1000, 3u << 5);
    r.wr(TIM2, CCR1, 400);
    uint64_t t0 = r.clock.now_ns();
    r.wr(TIM2, CR1, (3u << 5) | CEN);
    V cc;
    for (int i = 0; i < 5; ++i) cc.push_back(r.next_cc1(TIM2, t0));
    CHECK(cc == V{400, 1600, 2400, 3600, 4400});
    CHECK((r.rd(TIM2, SR) & UIF) != 0);
  }
  SUBCASE("RCR chain") {
    Rig r;
    r.wr(TIM1, RCR, 2);
    r.base_init(TIM1, 0, 999);
    r.wr(TIM1, CCR1, 500);
    uint64_t t0 = r.clock.now_ns();
    r.wr(TIM1, CR1, CEN);
    V cc;
    for (int i = 0; i < 7; ++i) cc.push_back(r.next_cc1(TIM1, t0));
    CHECK(cc == V{500, 1500, 2500, 3500, 4500, 5500, 6500});
    CHECK((r.rd(TIM1, SR) & UIF) != 0);
  }
}

TEST_CASE("tim: time blocked by ARR = 0 is not counted when the counter resumes") {
  SUBCASE("ARR written while blocked") {
    Rig r;
    r.base_init(TIM7, 0, 0);
    r.wr(TIM7, CR1, CEN);
    r.to(1000125);
    r.wr(TIM7, ARR, 999);
    CHECK(r.rd(TIM7, CNT) == 0);
    CHECK(r.fire_next() == 1000125 + 12500);
  }
  SUBCASE("ARR = 0 transferred from the preload at an update") {
    Rig r;
    r.base_init(TIM7, 0, 999, ARPE);
    r.wr(TIM7, CR1, ARPE | CEN);
    r.wr(TIM7, ARR, 0);  // buffered
    CHECK(r.fire_next() == 12500);
    CHECK(r.clock.next_event_ns() == kNone);  // blocked
    r.to(500000);
    r.wr(TIM7, ARR, 999);  // buffered, and no update can happen while blocked
    CHECK(r.clock.next_event_ns() == kNone);
    r.to(1000125);
    r.wr(TIM7, SR, 0);     // UIF from the 12500 ns update; otherwise nothing needs an event
    r.wr(TIM7, CR1, CEN);  // ARPE off: ARR = 999 takes effect now
    CHECK(r.rd(TIM7, CNT) == 0);
    CHECK(r.fire_next() == 1000125 + 12500);
  }
}
