// STM32L4 ADC1-3 (+ common block) and DAC1 on a private Bus + Clock + Cortexm + Rcc (80 MHz)
// with GPIOA/GPIOB carrying the analog pin voltages.
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/cortexm.hpp"
#include "stest/model_context.hpp"
#include "stest/node.hpp"
#include "stm32l4/models/adc.hpp"
#include "stm32l4/models/dac.hpp"
#include "stm32l4/models/gpio.hpp"
#include "stm32l4/models/rcc.hpp"
using namespace stest;
using nlohmann::json;

namespace {
const IrqHandler kTable[16 + 91] = {};
constexpr uint64_t RCC = 0x40021000, ADC = 0x50040000, ADC1 = ADC, ADC2 = ADC + 0x100,
                   ADC3 = ADC + 0x200, COMMON = ADC + 0x300, DAC = 0x40007400;
enum : uint32_t {
  ISR = 0x00, IER = 0x04, CR = 0x08, CFGR = 0x0C, CFGR2 = 0x10, SMPR1 = 0x14, SMPR2 = 0x18,
  TR1 = 0x20, SQR1 = 0x30, DR = 0x40, JSQR = 0x4C, OFR1 = 0x60, DIFSEL = 0xB0, CALFACT = 0xB4,
  CSR = 0x00, CCR = 0x08, CDR = 0x0C,
};
constexpr uint32_t ADEN = 1u << 0, ADDIS = 1u << 1, ADSTART = 1u << 2, JADSTART = 1u << 3,
                   ADSTP = 1u << 4, ADVREGEN = 1u << 28, DEEPPWD = 1u << 29, ADCAL = 1u << 31;
constexpr uint32_t ADRDY = 1u << 0, EOSMP = 1u << 1, EOC = 1u << 2, EOS = 1u << 3, OVR = 1u << 4;
constexpr uint32_t CKMODE_DIV4 = 3u << 16, VREFEN = 1u << 22;
constexpr uint32_t ALIGN = 1u << 5, OVRMOD = 1u << 12;
constexpr int IRQ_ADC1_2 = 18, IRQ_ADC3 = 47;
constexpr uint64_t kNone = UINT64_MAX;
// DAC
enum : uint32_t { DCR = 0x00, SWTRIGR = 0x04, DHR12R1 = 0x08, DHR12L1 = 0x0C, DHR8R1 = 0x10,
                  DHR12R2 = 0x14, DHR12RD = 0x20, DOR1 = 0x2C, DOR2 = 0x30, MCR = 0x3C };
constexpr uint32_t EN1 = 1u << 0, TEN1 = 1u << 2, TSEL1_SW = 7u << 3, EN2 = 1u << 16;

json adc_params(bool strict = false) {
  return {{"vref", 3.3},
          {"irq", {IRQ_ADC1_2, IRQ_ADC1_2, IRQ_ADC3}},
          {"strict_analog", strict},
          {"analog",
           {{{"pin", "PB1"}, {"adc", {1, 2}}, {"channel", 16}},
            {{"pin", "PC0"}, {"adc", {1, 2, 3}}, {"channel", 1}},
            {{"pin", "PA4"}, {"adc", {1, 2}}, {"channel", 9}}}}};
}

struct Rig {
  Bus bus;
  Clock clock;
  Cortexm core{bus, clock, 4, kTable, 16 + 91};
  ModelContext ctx{clock, core, bus};
  stm32l4::Rcc rcc{"RCC", json::object(), ctx};
  stm32l4::Gpio gpioa{"GPIOA", {{"port", "A"}}, ctx};
  stm32l4::Gpio gpiob{"GPIOB", {{"port", "B"}}, ctx};
  stm32l4::Gpio gpioc{"GPIOC", {{"port", "C"}}, ctx};
  stm32l4::Adc adc;
  stm32l4::Dac dac{"DAC1", {{"vref", 3.3}, {"irq", 54}, {"pins", {"PA4", "PA5"}}}, ctx};
  explicit Rig(bool strict = false) : adc("ADC", adc_params(strict), ctx) {
    for (Peripheral* p : std::vector<Peripheral*>{&rcc, &gpioa, &gpiob, &gpioc, &adc, &dac}) {
      p->reset();
      ctx.add(p);
    }
    ctx.set_clock_tree(&rcc);
    for (Peripheral* p : ctx.models()) p->connect(ctx);
    bus.map(RCC, 0x400, &rcc);
    bus.map(ADC, 0x400, &adc);
    bus.map(DAC, 0x400, &dac);
    // HSE 8 MHz bypass, PLL M=2 N=40 R=2 -> SYSCLK = HCLK = PCLK1 = PCLK2 = 80 MHz
    rcc_set(0x00, (1u << 18) | (1u << 16));
    bus.write(RCC + 0x0C, 4, 3u | (1u << 4) | (40u << 8) | (1u << 24));
    rcc_set(0x00, 1u << 24);
    bus.write(RCC + 0x08, 4, 3u);
    (void)take_events();
  }
  void rcc_set(uint32_t off, uint32_t bits) { bus.write(RCC + off, 4, bus.read(RCC + off, 4) | bits); }
  uint32_t rd(uint64_t a) { return bus.read(a, 4); }
  void wr(uint64_t a, uint32_t v) { bus.write(a, 4, v); }
  void set(uint64_t a, uint32_t bits) { wr(a, rd(a) | bits); }
  void to(uint64_t t) {
    while (clock.next_event_ns() <= t) {
      clock.advance(clock.next_event_ns() - clock.now_ns());
      clock.run_due();
    }
    clock.advance(t - clock.now_ns());
  }
  uint64_t fire_next() {
    uint64_t t = clock.next_event_ns();
    if (t == kNone) return kNone;
    to(t);
    return t;
  }
  void volts(const char* port_pin, double v) {
    stm32l4::Gpio& g = port_pin[1] == 'A' ? gpioa : port_pin[1] == 'B' ? gpiob : gpioc;
    g.call("set_voltage", {{"pin", std::stoi(port_pin + 2)}, {"volts", v}});
  }
  // Regulator on, synchronous HCLK/4 (20 MHz), calibrated, enabled and ready; channel ch.
  void ready(uint64_t base, unsigned ch = 16, uint32_t ccr = CKMODE_DIV4) {
    wr(COMMON + CCR, ccr);
    wr(base + CR, ADVREGEN);
    set(base + CR, ADCAL);
    while (rd(base + CR) & ADCAL) REQUIRE(fire_next() != kNone);
    set(base + CR, ADEN);
    while (!(rd(base + ISR) & ADRDY)) REQUIRE(fire_next() != kNone);
    wr(base + SQR1, ch << 6);
  }
  uint32_t convert(uint64_t base) {
    set(base + CR, ADSTART);
    while (!(rd(base + ISR) & EOC)) REQUIRE(fire_next() != kNone);
    return rd(base + DR);
  }
};

template <class F>
std::string abort_message(F&& f) {
  try {
    f();
  } catch (const std::exception& e) {
    return e.what();
  }
  return "(no exception)";
}
}  // namespace

TEST_CASE("adc reset values and register map") {
  Rig r;
  CHECK(r.rd(ADC1 + CR) == DEEPPWD);
  CHECK(r.rd(ADC2 + CFGR) == 0x80000000u);  // JQDIS
  CHECK(r.rd(ADC3 + TR1) == 0x0FFF0000u);
  CHECK(r.rd(COMMON + CCR) == 0);
  CHECK(r.rd(COMMON + CSR) == 0);
  CHECK_THROWS_AS(r.rd(ADC1 + 0x1C), UnmodelledAccess);      // reserved
  CHECK_THROWS_AS(r.wr(ADC1 + DR, 1), UnmodelledAccess);     // read-only
  CHECK_THROWS_AS(r.wr(COMMON + CSR, 1), UnmodelledAccess);  // read-only
  CHECK(abort_message([&] { r.rd(COMMON + CDR); }).find("dual") != std::string::npos);
}

TEST_CASE("adc_calibration") {  // 116 ADC clocks at HCLK/4 = 20 MHz -> 5.8 us
  Rig r;
  r.wr(COMMON + CCR, CKMODE_DIV4);
  CHECK(abort_message([&] { r.set(ADC1 + CR, ADCAL); }).find("ADVREGEN") != std::string::npos);
  Rig r2;
  r2.wr(COMMON + CCR, CKMODE_DIV4);
  r2.wr(ADC1 + CR, ADVREGEN);  // leaves deep power-down, regulator on
  CHECK(r2.rd(ADC1 + CR) == ADVREGEN);
  r2.to(1000);
  r2.set(ADC1 + CR, ADCAL);
  CHECK((r2.rd(ADC1 + CR) & ADCAL) != 0);
  CHECK(r2.rd(ADC1 + CALFACT) == 0);
  CHECK(r2.fire_next() == 1000 + 5800);
  CHECK(r2.rd(ADC1 + CR) == ADVREGEN);
  CHECK((r2.rd(ADC1 + CALFACT) & 0x7Fu) != 0);  // single-ended factor
  CHECK(r2.clock.next_event_ns() == kNone);
}

TEST_CASE("adc_adrdy") {  // ADEN -> ADRDY 1 us later (ADRDYIE -> ADC1_2 pending)
  Rig r;
  r.wr(COMMON + CCR, CKMODE_DIV4);
  r.wr(ADC2 + CR, ADVREGEN);
  r.wr(ADC2 + IER, ADRDY);
  r.to(500);
  r.set(ADC2 + CR, ADEN);
  CHECK(r.rd(ADC2 + ISR) == 0);
  CHECK(r.fire_next() == 1500);
  CHECK(r.rd(ADC2 + ISR) == ADRDY);
  CHECK(r.rd(COMMON + CSR) == (ADRDY << 16));  // ADC2 is the CSR's slave half
  CHECK(r.core.pending(IRQ_ADC1_2));
  r.wr(ADC2 + ISR, ADRDY);  // rc_w1
  CHECK(r.rd(ADC2 + ISR) == 0);
  // ADDIS: ADEN and ADDIS clear
  r.set(ADC2 + CR, ADDIS);
  CHECK(r.rd(ADC2 + CR) == ADVREGEN);
  // ADEN while deep power-down / no regulator aborts
  CHECK(abort_message([&] { r.wr(ADC3 + CR, ADEN); }).find("ADVREGEN") != std::string::npos);
}

TEST_CASE("adc_conversion_timing") {
  Rig r;
  r.volts("PB1", 1.0);
  r.ready(ADC1);
  r.wr(ADC1 + IER, EOC);
  r.core.clear_pending(IRQ_ADC1_2);
  const uint64_t t0 = r.clock.now_ns();
  r.set(ADC1 + CR, ADSTART);
  CHECK(r.fire_next() == t0 + 125);  // SMP 2.5 cycles at 20 MHz: end of sampling
  CHECK(r.rd(ADC1 + ISR) == (ADRDY | EOSMP));
  CHECK(r.fire_next() == t0 + 750);  // + 12.5 cycles SAR (12-bit)
  CHECK(r.rd(ADC1 + ISR) == (ADRDY | EOSMP | EOC | EOS));
  CHECK((r.rd(ADC1 + CR) & ADSTART) == 0);  // single conversion: cleared by hardware
  CHECK(r.core.pending(IRQ_ADC1_2));
  CHECK(r.rd(ADC1 + DR) == 1241);  // round(1.0 / 3.3 * 4095)
  CHECK((r.rd(ADC1 + ISR) & EOC) == 0);  // reading DR clears EOC
  CHECK(r.clock.next_event_ns() == kNone);
  // SMP16 = 7 (640.5 cycles), 10-bit (10.5 cycles): 651 cycles = 32 550 ns
  r.wr(ADC1 + SMPR2, 7u << 18);
  r.wr(ADC1 + CFGR, 0x80000000u | (1u << 3));
  const uint64_t t1 = r.clock.now_ns();
  r.set(ADC1 + CR, ADSTART);
  CHECK(r.fire_next() == t1 + 32025);
  CHECK(r.fire_next() == t1 + 32550);
  CHECK(r.rd(ADC1 + DR) == 310);  // round(1.0 / 3.3 * 1023)
  // ADC3 has its own vector
  r.volts("PC0", 2.0);
  r.ready(ADC3, 1);
  r.wr(ADC3 + IER, EOC);
  CHECK(r.convert(ADC3) == 2482);
  CHECK(r.core.pending(IRQ_ADC3));
}

TEST_CASE("adc_resolution_scaling_and_clamp") {
  Rig r;
  r.volts("PB1", 1.65);
  r.ready(ADC1);
  CHECK(r.convert(ADC1) == 2048);
  r.wr(ADC1 + CFGR, 0x80000000u | (1u << 3));
  CHECK(r.convert(ADC1) == 512);
  r.wr(ADC1 + CFGR, 0x80000000u | (2u << 3));
  CHECK(r.convert(ADC1) == 128);
  r.wr(ADC1 + CFGR, 0x80000000u | (3u << 3));
  CHECK(r.convert(ADC1) == 32);
  r.wr(ADC1 + CFGR, 0x80000000u | (3u << 3) | ALIGN);  // 6-bit left: byte-aligned
  CHECK(r.convert(ADC1) == (32u << 2));
  r.wr(ADC1 + CFGR, 0x80000000u | ALIGN);  // 12-bit left
  CHECK(r.convert(ADC1) == (2048u << 4));
  r.wr(ADC1 + CFGR, 0x80000000u);
  r.volts("PB1", 5.0);  // above VREF+
  CHECK(r.convert(ADC1) == 4095);
  r.volts("PB1", -0.7);
  CHECK(r.convert(ADC1) == 0);
  // ADC2 shares PB1 (ADC12_IN16)
  r.volts("PB1", 3.3);
  r.ready(ADC2);
  CHECK(r.convert(ADC2) == 4095);
  // set_input / get_input resolve (adc, channel) through the analog pin table
  r.adc.call("set_input", {{"adc", 2}, {"channel", 16}, {"volts", 0.825}});
  CHECK(r.gpiob.voltage(1).value() == doctest::Approx(0.825));
  CHECK(r.adc.call("get_input", {{"adc", 1}, {"channel", 16}}).get<double>() == doctest::Approx(0.825));
  CHECK(r.convert(ADC1) == 1024);
  CHECK(r.adc.call("get_input", {{"adc", 1}, {"channel", 5}}).is_null());
  CHECK_THROWS_WITH_AS(r.adc.call("set_input", {{"adc", 3}, {"channel", 16}, {"volts", 1.0}}),
                       doctest::Contains("ADC3 channel 16 has no pin"), std::invalid_argument);
}

TEST_CASE("adc_vrefint_and_internal_channels") {
  Rig r;
  r.ready(ADC1, 0, CKMODE_DIV4);
  CHECK(abort_message([&] { r.convert(ADC1); }).find("VREFEN") != std::string::npos);
  Rig r2;
  r2.ready(ADC1, 0, CKMODE_DIV4 | VREFEN);
  CHECK(r2.convert(ADC1) == 1504);  // round(1.212 / 3.3 * 4095)
  r2.wr(ADC1 + SQR1, 17u << 6);
  CHECK(abort_message([&] { r2.convert(ADC1); }).find("temperature sensor") != std::string::npos);
}

TEST_CASE("adc_overrun") {
  Rig r;
  r.volts("PB1", 1.0);
  r.ready(ADC1);
  r.convert(ADC1);  // DR read: EOC clear
  r.set(ADC1 + CR, ADSTART);
  r.fire_next();
  r.fire_next();  // EOC set, DR not read
  r.volts("PB1", 2.0);
  r.set(ADC1 + CR, ADSTART);
  r.fire_next();
  r.fire_next();
  CHECK((r.rd(ADC1 + ISR) & OVR) != 0);
  CHECK(r.rd(ADC1 + DR) == 1241);  // OVRMOD = 0: old data preserved
  r.wr(ADC1 + ISR, OVR);
  r.wr(ADC1 + CFGR, 0x80000000u | OVRMOD);
  r.set(ADC1 + CR, ADSTART);
  r.fire_next();
  r.fire_next();
  r.set(ADC1 + CR, ADSTART);
  r.fire_next();
  r.fire_next();
  CHECK((r.rd(ADC1 + ISR) & OVR) != 0);
  CHECK(r.rd(ADC1 + DR) == 2482);  // OVRMOD = 1: overwritten
  // ADSTP stops an ongoing conversion
  r.set(ADC1 + CR, ADSTART);
  r.set(ADC1 + CR, ADSTP);
  CHECK((r.rd(ADC1 + CR) & (ADSTART | ADSTP)) == 0);
  CHECK(r.clock.next_event_ns() == kNone);
}

TEST_CASE("adc_async_clock") {
  Rig r;
  // ADCSEL = SYSCLK (80 MHz), CKMODE = 0 (asynchronous), PRESC = 2 (/4) -> 20 MHz
  r.rcc_set(0x88, 3u << 28);
  r.wr(COMMON + CCR, 2u << 18);
  r.wr(ADC1 + CR, ADVREGEN);
  r.set(ADC1 + CR, ADCAL);
  CHECK(r.fire_next() == 5800);
  // PRESC = 11 (/256): 312 500 Hz -> 116 cycles = 371 200 ns
  r.wr(COMMON + CCR, 11u << 18);
  const uint64_t t0 = r.clock.now_ns();
  r.set(ADC1 + CR, ADCAL);
  CHECK(r.fire_next() == t0 + 371200);
  // CKMODE/PRESC are frozen while an ADC is enabled
  r.set(ADC1 + CR, ADEN);
  CHECK(abort_message([&] { r.wr(COMMON + CCR, 0); }).find("enabled") != std::string::npos);
}

TEST_CASE("adc_async_no_clock_aborts") {
  Rig r;  // RCC CCIPR ADCSEL = 0 (reset): no asynchronous clock
  r.wr(ADC1 + CR, ADVREGEN);
  CHECK(abort_message([&] { r.set(ADC1 + CR, ADEN); }) ==
        "ADC1 has no clock: RCC CCIPR ADCSEL selects none");
  CHECK(abort_message([&] { r.set(ADC1 + CR, ADCAL); }) ==
        "ADC1 has no clock: RCC CCIPR ADCSEL selects none");
  // PLLSAI1R selected but off
  r.rcc_set(0x88, 1u << 28);
  CHECK(abort_message([&] { r.set(ADC1 + CR, ADEN); }).find("PLLSAI1") != std::string::npos);
}

TEST_CASE("adc_unmodelled_features_abort") {
  Rig r;
  auto msg = [&](uint64_t a, uint32_t v) { return abort_message([&] { r.wr(a, v); }); };
  CHECK(msg(ADC1 + CFGR, 0x80000000u | (1u << 10)) ==
        "ADC1: hardware triggers (CFGR.EXTEN=1, EXTSEL=0) not modelled");
  CHECK_THROWS_AS(r.wr(ADC1 + CFGR, 0x80000000u | 1u), NotModelled);
  CHECK(msg(ADC1 + CFGR, 0x80000000u | 1u).find("DMA") != std::string::npos);
  CHECK(msg(ADC1 + CFGR, 0x80000000u | (1u << 13)).find("continuous") != std::string::npos);
  CHECK(msg(ADC1 + CFGR, 0x80000000u | (1u << 23)).find("watchdog") != std::string::npos);
  CHECK(msg(ADC1 + SQR1, 1u) == "ADC1: scan mode (SQR1.L=1: a sequence of 2 conversions) not modelled");
  CHECK(msg(ADC1 + CFGR2, 1u).find("oversampling") != std::string::npos);
  CHECK(msg(ADC1 + JSQR, 0x100u).find("injected") != std::string::npos);
  CHECK(msg(ADC1 + IER, 1u << 7).find("watchdog") != std::string::npos);
  CHECK(msg(ADC1 + IER, 1u << 5).find("injected") != std::string::npos);
  CHECK(msg(ADC1 + OFR1, 1u << 31).find("offset") != std::string::npos);
  CHECK(msg(ADC1 + DIFSEL, 1u << 16 >> 1).find("differential") != std::string::npos);
  CHECK(msg(COMMON + CCR, 1u).find("dual") != std::string::npos);
  CHECK(msg(ADC1 + CR, JADSTART).find("injected") != std::string::npos);
  // HAL-style writes of the reset/software-trigger configuration are fine
  r.wr(ADC1 + CFGR, 0x80000000u | (1u << 1));  // DMACFG alone
  r.wr(ADC1 + SQR1, 16u << 6);
  r.wr(ADC1 + CFGR2, 0);
  r.wr(ADC1 + DIFSEL, 0);
  // ADSTART before the ADC is ready
  r.wr(COMMON + CCR, CKMODE_DIV4);
  r.wr(ADC1 + CR, ADVREGEN);
  CHECK(abort_message([&] { r.set(ADC1 + CR, ADSTART); }).find("not enabled") != std::string::npos);
}

TEST_CASE("adc_unset_channel") {
  {
    Rig r;  // lenient: 0 V (and one stderr warning)
    r.ready(ADC1);
    CHECK(r.convert(ADC1) == 0);
    CHECK(r.convert(ADC1) == 0);
  }
  {
    Rig r(true);  // strict analog
    r.ready(ADC1);  // calibration converts nothing: unaffected
    r.set(ADC1 + CR, ADSTART);
    CHECK(abort_message([&] { r.to(r.clock.now_ns() + 1000); }) == "ADC1 channel 16 converted with no input set");
  }
}

TEST_CASE("dac_output") {
  Rig r;
  r.to(1000);
  r.wr(DAC + DHR12R1, 2048);
  CHECK(r.rd(DAC + DOR1) == 0);
  CHECK(r.fire_next() == 1013);  // one APB1 clock (12.5 ns at 80 MHz), rounded up
  CHECK(r.rd(DAC + DOR1) == 2048);
  CHECK_FALSE(r.gpioa.voltage(4).has_value());  // not enabled: not driven
  CHECK(take_events().empty());
  r.set(DAC + DCR, EN1);
  CHECK(r.gpioa.voltage(4).value() == doctest::Approx(2048.0 / 4095.0 * 3.3));
  json st = r.dac.call("state", json::object());
  CHECK(st["channels"][0]["volts"].get<double>() == doctest::Approx(1.6504));
  auto ev = take_events();
  REQUIRE(ev.size() == 1);
  CHECK(ev[0].data["kind"] == "dac");
  CHECK(ev[0].data["channel"] == 1);
  CHECK(ev[0].data["volts"].get<double>() == doctest::Approx(1.6504));
  // views of the holding register
  CHECK(r.rd(DAC + DHR12L1) == (2048u << 4));
  CHECK(r.rd(DAC + DHR8R1) == 128);
  r.wr(DAC + DHR8R1, 0xFF);
  CHECK(r.rd(DAC + DHR12R1) == 0xFF0);
  r.fire_next();
  CHECK(r.rd(DAC + DOR1) == 0xFF0);
  // dual: both channels at once; channel 2 drives PA5 once enabled
  r.wr(DAC + DHR12RD, (4095u << 16) | 0);
  r.fire_next();
  CHECK(r.rd(DAC + DOR1) == 0);
  CHECK(r.rd(DAC + DOR2) == 4095);
  r.set(DAC + DCR, EN2);
  CHECK(r.gpioa.voltage(5).value() == doctest::Approx(3.3));
  // the ADC sees the DAC output on PA4 (ADC12_IN9)
  r.ready(ADC1, 9);
  CHECK(r.convert(ADC1) == 0);
  r.wr(DAC + DHR12R1, 1000);
  r.fire_next();
  CHECK(r.convert(ADC1) == 1000);
  // an external voltage wins over the DAC driver
  r.volts("PA4", 0.0);
  CHECK(r.gpioa.voltage(4).value() == 0.0);
}

TEST_CASE("dac_software_trigger") {
  Rig r;
  r.wr(DAC + DCR, TEN1 | TSEL1_SW | EN1);
  r.wr(DAC + DHR12R1, 4095);
  CHECK(r.clock.next_event_ns() == kNone);  // TEN: no automatic transfer
  r.set(DAC + SWTRIGR, 1);
  CHECK(r.rd(DAC + SWTRIGR) == 1);
  r.fire_next();
  CHECK(r.rd(DAC + SWTRIGR) == 0);
  CHECK(r.rd(DAC + DOR1) == 4095);
}

TEST_CASE("dac_hw_trigger_aborts") {
  Rig r;
  CHECK(abort_message([&] { r.wr(DAC + DCR, TEN1 | (2u << 3)); }) .find("DAC1: hardware triggers (CR.TEN1=1, TSEL1=2;") !=
        std::string::npos);
  CHECK(abort_message([&] { r.wr(DAC + DCR, 1u << 6); }).find("wave") != std::string::npos);
  CHECK(abort_message([&] { r.wr(DAC + DCR, 1u << 12); }).find("DMA") != std::string::npos);
  CHECK(abort_message([&] { r.wr(DAC + DCR, 1u << 28); }).find("DMA") != std::string::npos);
  CHECK(abort_message([&] { r.wr(DAC + MCR, 4u); }).find("sample-and-hold") != std::string::npos);
  r.wr(DAC + MCR, 2u);  // buffer off, external pin: fine
  CHECK_THROWS_AS(r.wr(DAC + DOR1, 1), UnmodelledAccess);  // read-only
}
