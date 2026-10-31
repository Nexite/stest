#pragma once
// STM32L4 (L496) ADC1/ADC2/ADC3 and their common block, RM0351 section 18. One model for
// the whole 0x400 block at 0x50040000 (ADC_TypeDef x3 at +0x000/+0x100/+0x200 and
// ADC_Common_TypeDef at +0x300, stm32l496xx.h). Gated as a whole by RCC AHB2ENR ADCEN.
//
// Modelled (per ADC): CR DEEPPWD/ADVREGEN/ADCALDIF (rw) and ADCAL/ADEN/ADDIS/ADSTART/ADSTP
// (rs: writing 1 acts, writing 0 is ignored); ISR ADRDY/EOSMP/EOC/EOS/OVR (rc_w1) with IER
// ADRDYIE/EOSMPIE/EOCIE/EOSIE/OVRIE; CFGR RES/ALIGN/OVRMOD (+ stored DMACFG, EXTSEL,
// AWD1SGL/AWD1CH, JQDIS); SQR1 SQ1 (single conversion of one channel); SMPR1/2 (+ SMPPLUS);
// DR (reading clears EOC); CALFACT; TR1-3, SQR2-4, OFRx (offset disabled) stored. Common:
// CCR CKMODE/PRESC/VREFEN (+ stored TSEN/VBATEN/DELAY/DMACFG), CSR (read-only mirror of the
// ADC1 ISR in bits 10:0 and the ADC2 ISR in bits 26:16).
//
// Timing (all Clock events, model contract): ADCAL self-clears 116 ADC clock cycles after it
// is set (CALFACT_S, or CALFACT_D with ADCALDIF, then reads kCalFactor); ADEN -> ADRDY
// after 1 us; ADSTART -> EOSMP after the channel's sampling time SMP (2.5, 6.5, 12.5, 24.5,
// 47.5, 92.5, 247.5, 640.5 cycles; SMPPLUS turns 2.5 into 3.5) and EOC + EOS (+ DR, ADSTART
// cleared) after SMP + the SAR time (12.5 / 10.5 / 8.5 / 6.5 cycles for 12 / 10 / 8 / 6 bit,
// RM0351 18.4.16). Event times are rounded up to the next ns. ADC clock, read from the RCC
// clock tree when ADCAL / ADEN / ADSTART is set:
//   CCR.CKMODE = 1/2/3: synchronous, HCLK / 1 / 2 / 4;
//   CCR.CKMODE = 0: asynchronous, RCC CCIPR ADCSEL (PLLSAI1R / PLLSAI2R / SYSCLK) divided
//   by CCR.PRESC (1 2 4 6 8 10 12 16 32 64 128 256). ADCSEL = 0 ("no clock", its reset
//   value) or a stopped source aborts at ADCAL / ADEN: "ADC1 has no clock: RCC CCIPR ADCSEL
//   selects none" (on hardware ADRDY never sets and the HAL times out).
// A new EOC while EOC is still set sets OVR; DR keeps the old data unless CFGR.OVRMOD.
//
// Sampled value (at the end of the sampling phase): external channel -> the voltage of its
// pin (Gpio::voltage(), see gpio.hpp) from the analog pin table; ADC1 channel 0 -> VREFINT
// 1.212 V (needs CCR.VREFEN). code = round(clamp(v, 0, vref) / vref * (2^res - 1)),
// right-aligned, or with CFGR.ALIGN shifted left to bit 15 (6-bit: to bit 7).
// Converting a channel whose pin has no voltage (or which has no pin on this package) reads
// 0 V and prints one stderr warning per (ADC, channel); with strict analog it aborts
// "ADC1 channel 16 converted with no input set". Strict analog: params.strict_analog, else
// the node environment STEST_STRICT_ANALOG=1 (Python Sim(strict_analog=True)).
//
// Strict aborts (std::runtime_error naming the ADC and the feature): hardware triggers
// (CFGR.EXTEN != 0), DMA (CFGR.DMAEN, CCR.MDMA), continuous mode (CFGR.CONT: not modelled,
// it would need an event per conversion), scan sequences (SQR1.L > 0, "scan mode not
// modelled"), discontinuous mode, auto-delay (AUTDLY), injected conversions (JADSTART,
// JADSTP, any JSQR bit, JAUTO/JQM/JDISCEN/JAWD1EN, JEOC/JEOS/JQOVF interrupts), analog
// watchdogs (CFGR.AWD1EN, AWD2CR/AWD3CR, AWDx interrupts), oversampling (CFGR2.ROVSE/JOVSE),
// offsets (OFRx.OFFSET_EN), differential inputs (DIFSEL), DFSDM output, dual mode (CCR.DUAL,
// and reading CDR), reserved PRESC values, VBAT / temperature sensor / DAC-internal
// channels, and sequencing errors the RM leaves undefined: ADCAL / ADEN in deep power-down
// or without the regulator, ADCAL / ADEN while enabled or calibrating, ADDIS while not
// enabled or converting, ADSTART before ADRDY, changing CCR.CKMODE/PRESC while an ADC is
// enabled. ADSTP without a conversion is ignored.
//
// params: {"vref": volts (default 3.3), "irq": [ADC1, ADC2, ADC3 vector] (ADC1_2 is shared:
//   driven through ModelContext::set_irq_source), "strict_analog": bool (optional),
//   "analog": [{"pin": "PB1", "adc": [1, 2], "channel": 16}, ...] (the part TOML's
//   [[analog]] table, injected by tools/gen_chip.py)}. Pins resolve to the "GPIO<port>"
//   models in connect().
//
// call() (MODEL_CALL bypasses RCC clock gating by design: it is the outside world):
//   "set_input" {adc, channel, volts}: sets the voltage of the channel's pin (so every ADC
//     sharing the pin sees it); a channel with no pin on this package -> error.
//   "get_input" {adc, channel} -> volts | null (null: the pin has no voltage).
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Gpio;

class Adc : public Peripheral {
 public:
  static constexpr unsigned kNumAdc = 3, kNumCh = 19;
  static constexpr uint32_t kCalFactor = 0x40;  // CALFACT_S / CALFACT_D after calibration
  static constexpr double kVrefint = 1.212;     // DS11585 VREFINT typical

  Adc(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;
  void connect(ModelContext& ctx) override;
  nlohmann::json call(std::string_view op, const nlohmann::json& args) override;

 private:
  enum class Phase { None, Cal, Enable, Sample, Convert };
  struct PinRef { std::string port; unsigned pin = 0; Gpio* gpio = nullptr; };
  struct Inst {
    unsigned idx = 0;  // 0..2 -> ADC1..3
    int irq = -1;
    char tag = 0;      // address identifies this source on a shared vector
    bool level = false;
    bool ready = false;  // ADRDY reached since the last ADEN
    Phase phase = Phase::None;
    EventId ev = 0;
    bool has_ev = false;
    uint64_t t_end = 0;  // conversion end (Sample phase)
    uint32_t code = 0;   // sampled result (Convert phase)
    std::array<bool, kNumCh> warned{};
    std::array<std::optional<PinRef>, kNumCh> pins{};
  };

  std::string label(const Inst& a) const { return "ADC" + std::to_string(a.idx + 1); }
  [[noreturn]] void fail(const Inst& a, const std::string& what) const;
  [[noreturn]] void not_modelled(const Inst* a, const std::string& what, uint32_t off, uint32_t v) const;
  uint32_t reg(const Inst& a, uint32_t r) const { return regs_.get(a.idx * 0x100 + r); }
  void set_reg(const Inst& a, uint32_t r, uint32_t v) { regs_.set(a.idx * 0x100 + r, v); }
  uint64_t adc_hz(const Inst& a) const;  // aborts when there is no clock
  uint64_t after_half_cycles(uint64_t half_cycles, uint64_t hz) const;
  void check_inst_write(Inst& a, uint32_t r, uint32_t v);
  void check_common_write(uint32_t v);
  void write_cr(Inst& a, uint32_t off, unsigned width, uint32_t v);
  void start_conversion(Inst& a);
  double sample_volts(Inst& a, unsigned ch);
  void schedule(Inst& a, uint64_t t, Phase p);
  void cancel(Inst& a);
  void on_event(Inst& a);
  void update_irq(Inst& a);
  Inst& inst_arg(const nlohmann::json& args, unsigned* ch);

  std::string name_;
  ModelContext& ctx_;
  Clock& clock_;
  const ClockTree* clocks_ = nullptr;
  double vref_;
  bool strict_analog_;
  nlohmann::json analog_;
  std::array<Inst, kNumAdc> adc_{};
  RegisterFile regs_;
};

}  // namespace stest::stm32l4
