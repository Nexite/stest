#include "stm32l4/models/adc.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include "stest/cortexm.hpp"
#include "stm32l4/models/gpio.hpp"

namespace stest::stm32l4 {
namespace {
// Per-ADC registers (ADC_TypeDef), offsets within an instance's 0x100 window.
enum : uint32_t {
  ISR = 0x00, IER = 0x04, CR = 0x08, CFGR = 0x0C, CFGR2 = 0x10, SMPR1 = 0x14, SMPR2 = 0x18,
  TR1 = 0x20, TR2 = 0x24, TR3 = 0x28, SQR1 = 0x30, SQR2 = 0x34, SQR3 = 0x38, SQR4 = 0x3C,
  DR = 0x40, JSQR = 0x4C, OFR1 = 0x60, OFR4 = 0x6C, JDR1 = 0x80, JDR4 = 0x8C, AWD2CR = 0xA0,
  AWD3CR = 0xA4, DIFSEL = 0xB0, CALFACT = 0xB4,
};
// Common registers (ADC_Common_TypeDef) at +0x300.
enum : uint32_t { COMMON = 0x300, CSR = 0x300, CCR = 0x308, CDR = 0x30C };
// ISR / IER (ADC_ISR_*, ADC_IER_*)
constexpr uint32_t ADRDY = 1u << 0, EOSMP = 1u << 1, EOC = 1u << 2, EOS = 1u << 3, OVR = 1u << 4;
constexpr uint32_t IER_INJ = (1u << 5) | (1u << 6) | (1u << 10), IER_AWD = 7u << 7;
// CR (ADC_CR_*): rs bits act on a written 1; rw bits are plain storage.
constexpr uint32_t ADEN = 1u << 0, ADDIS = 1u << 1, ADSTART = 1u << 2, JADSTART = 1u << 3,
                   ADSTP = 1u << 4, JADSTP = 1u << 5, ADVREGEN = 1u << 28, DEEPPWD = 1u << 29,
                   ADCALDIF = 1u << 30, ADCAL = 1u << 31;
constexpr uint32_t kCrRs = ADEN | ADDIS | ADSTART | JADSTART | ADSTP | JADSTP | ADCAL;
constexpr uint32_t kCrRw = ADVREGEN | DEEPPWD | ADCALDIF;
// CFGR (ADC_CFGR_*)
constexpr uint32_t DMAEN = 1u << 0, DFSDMCFG = 1u << 2, ALIGN = 1u << 5, EXTEN = 3u << 10,
                   OVRMOD = 1u << 12, CONT = 1u << 13, AUTDLY = 1u << 14, DISCEN = 1u << 16,
                   JDISCEN = 1u << 20, JQM = 1u << 21, AWD1EN = 1u << 23, JAWD1EN = 1u << 24,
                   JAUTO = 1u << 25;
constexpr uint32_t ROVSE = 1u << 0, JOVSE = 1u << 1;  // CFGR2
constexpr uint32_t SMPPLUS = 1u << 31;                 // SMPR1
// CCR (ADC_CCR_*)
constexpr uint32_t CCR_DUAL = 0x1Fu, CCR_MDMA = 3u << 14, CCR_CKMODE = 3u << 16,
                   CCR_PRESC = 0xFu << 18, VREFEN = 1u << 22;

// Sampling time in half ADC cycles for SMP = 0..7 (2.5 ... 640.5 cycles).
constexpr uint32_t kSmpHalf[8] = {5, 13, 25, 49, 95, 185, 495, 1281};
// SAR conversion time in half cycles for RES = 0..3 (12.5 / 10.5 / 8.5 / 6.5 cycles).
constexpr uint32_t kSarHalf[4] = {25, 21, 17, 13};
constexpr uint32_t kPresc[12] = {1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256};
constexpr uint64_t kAdrdyNs = 1000;   // ADEN -> ADRDY
constexpr uint64_t kCalHalf = 2 * 116;  // calibration: 116 ADC clock cycles

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}

bool env_strict_analog() {
  const char* s = std::getenv("STEST_STRICT_ANALOG");
  return s && std::strcmp(s, "1") == 0;
}
}  // namespace

Adc::Adc(std::string name, const nlohmann::json& params, ModelContext& ctx)
    : name_(std::move(name)), ctx_(ctx), clock_(ctx.clock()) {
  vref_ = params.value("vref", 3.3);
  if (!(vref_ > 0.0)) throw std::invalid_argument(name_ + ": adc vref must be > 0");
  strict_analog_ = params.contains("strict_analog") ? params["strict_analog"].get<bool>() : env_strict_analog();
  for (unsigned i = 0; i < kNumAdc; ++i) adc_[i].idx = i;
  if (params.contains("irq")) {
    const auto& irq = params["irq"];
    if (!irq.is_array() || irq.size() != kNumAdc)
      throw std::invalid_argument(name_ + ": adc params.irq must list the ADC1, ADC2, ADC3 vectors");
    for (unsigned i = 0; i < kNumAdc; ++i) adc_[i].irq = irq[i].get<int>();
  }
  analog_ = params.value("analog", nlohmann::json::array());
  for (const auto& e : analog_) {  // shape only; pins resolve in connect()
    const std::string pin = e.at("pin").get<std::string>();
    const unsigned ch = e.at("channel").get<unsigned>();
    if (pin.size() < 3 || pin[0] != 'P' || ch == 0 || ch >= kNumCh)
      throw std::invalid_argument(name_ + ": bad analog entry " + e.dump());
    for (const auto& n : e.at("adc")) {
      const unsigned a = n.get<unsigned>();
      if (a < 1 || a > kNumAdc) throw std::invalid_argument(name_ + ": bad analog entry " + e.dump());
      auto& slot = adc_[a - 1].pins[ch];
      if (slot) throw std::invalid_argument(name_ + ": ADC" + std::to_string(a) + " channel " +
                                            std::to_string(ch) + " listed twice in the analog table");
      slot = PinRef{pin.substr(1, 1), static_cast<unsigned>(std::stoul(pin.substr(2))), nullptr};
      if (slot->pin > 15) throw std::invalid_argument(name_ + ": bad analog pin " + pin);
    }
  }

  for (unsigned i = 0; i < kNumAdc; ++i) {
    const uint32_t b = i * 0x100;
    regs_.declare(b + ISR, 0, 0, 0x7FF);
    regs_.declare(b + IER, 0, 0x7FF);
    regs_.declare(b + CR, DEEPPWD, kCrRs | kCrRw);  // written by write_cr() only
    regs_.declare(b + CFGR, 0x80000000u, 0xFFFF7FFFu);
    regs_.declare(b + CFGR2, 0, 0x7FF);
    regs_.declare(b + SMPR1, 0, 0xBFFFFFFFu);
    regs_.declare(b + SMPR2, 0, 0x07FFFFFFu);
    regs_.declare(b + TR1, 0x0FFF0000u, 0x0FFF0FFFu);
    regs_.declare(b + TR2, 0x00FF0000u, 0x00FF00FFu);
    regs_.declare(b + TR3, 0x00FF0000u, 0x00FF00FFu);
    regs_.declare(b + SQR1, 0, 0x1F7DF7CFu);
    regs_.declare(b + SQR2, 0, 0x1F7DF7DFu);
    regs_.declare(b + SQR3, 0, 0x1F7DF7DFu);
    regs_.declare(b + SQR4, 0, 0x7DFu);
    regs_.declare(b + DR, 0, 0);  // read-only
    regs_.declare(b + JSQR, 0, 0x7DF7DFFFu);
    for (uint32_t o = OFR1; o <= OFR4; o += 4) regs_.declare(b + o, 0, 0xFC000FFFu);
    for (uint32_t o = JDR1; o <= JDR4; o += 4) regs_.declare(b + o, 0, 0);  // read-only
    regs_.declare(b + AWD2CR, 0, 0x7FFFF);
    regs_.declare(b + AWD3CR, 0, 0x7FFFF);
    regs_.declare(b + DIFSEL, 0, 0x7FFFE);  // channel 0 is single-ended only
    regs_.declare(b + CALFACT, 0, 0x007F007Fu);
  }
  regs_.declare(CSR, 0, 0);  // read-only, computed
  regs_.declare(CCR, 0, 0x01FFEF1Fu);
  regs_.declare(CDR, 0, 0);  // read-only; reading aborts (dual mode)
}

void Adc::connect(ModelContext& ctx) {
  clocks_ = &ctx.clocks();
  for (Inst& a : adc_)
    for (auto& p : a.pins)
      if (p) p->gpio = &ctx.get<Gpio>("GPIO" + p->port);
  // Analog functions for Gpio's set_voltage check, named like ST's signals ("ADC12_IN16").
  for (const auto& e : analog_) {
    const std::string pin = e.at("pin").get<std::string>();
    std::string fn = "ADC";
    for (const auto& n : e.at("adc")) fn += std::to_string(n.get<unsigned>());
    fn += "_IN" + std::to_string(e.at("channel").get<unsigned>());
    ctx.get<Gpio>("GPIO" + pin.substr(1, 1)).add_analog_function(static_cast<unsigned>(std::stoul(pin.substr(2))), fn);
  }
}

void Adc::reset() {
  for (Inst& a : adc_) {
    cancel(a);
    a.ready = false;
    a.warned.fill(false);
  }
  regs_.reset();
  for (Inst& a : adc_) update_irq(a);
}

// ---------------------------------------------------------------- helpers

void Adc::fail(const Inst& a, const std::string& what) const { throw std::runtime_error(label(a) + what); }

void Adc::not_modelled(const Inst* a, const std::string& what) const {
  throw NotModelled(a ? label(*a) : name_, what);
}

uint64_t Adc::adc_hz(const Inst& a) const {
  const uint32_t ccr = regs_.get(CCR);
  const uint32_t ck = (ccr & CCR_CKMODE) >> 16;
  if (ck) return clocks_->hclk_hz() >> (ck - 1);  // HCLK / 1, 2, 4
  static const char* const kSrc[4] = {"none", "PLLSAI1R", "PLLSAI2R", "SYSCLK"};
  const uint32_t sel = clocks_->adcsel() & 3u;
  if (sel == 0) fail(a, " has no clock: RCC CCIPR ADCSEL selects none");
  const uint64_t src = sel == 1 ? clocks_->pllsai1r_hz() : sel == 2 ? clocks_->pllsai2r_hz() : clocks_->sysclk_hz();
  if (src == 0)
    fail(a, std::string(" has no clock: RCC CCIPR ADCSEL selects ") + kSrc[sel] + ", which is off (0 Hz)");
  return src / kPresc[(ccr & CCR_PRESC) >> 18];  // reserved PRESC values abort on write
}

uint64_t Adc::after_half_cycles(uint64_t half_cycles, uint64_t hz) const {
  const unsigned __int128 num = static_cast<unsigned __int128>(half_cycles) * 1000000000u, den = 2u * hz;
  return clock_.now_ns() + static_cast<uint64_t>((num + den - 1) / den);
}

void Adc::schedule(Inst& a, uint64_t t, Phase p) {
  cancel(a);
  a.phase = p;
  a.ev = clock_.schedule_at(t, [this, &a] { on_event(a); });
  a.has_ev = true;
}

void Adc::cancel(Inst& a) {
  if (a.has_ev) clock_.cancel(a.ev);
  a.has_ev = false;
  a.phase = Phase::None;
}

void Adc::update_irq(Inst& a) {
  const bool lv = (reg(a, ISR) & reg(a, IER) & (ADRDY | EOSMP | EOC | EOS | OVR)) != 0;
  if (a.irq >= 0 && lv != a.level) {
    a.level = lv;
    ctx_.set_irq_source(a.irq, &a.tag, lv);
  }
}

// ---------------------------------------------------------------- configuration checks

void Adc::check_inst_write(Inst& a, uint32_t r, uint32_t v) {
  const uint32_t off = a.idx * 0x100 + r;
  char d[64];
  switch (r) {
    case IER:
      if (v & IER_INJ) not_modelled(&a, "injected conversions (IER.JEOCIE/JEOSIE/JQOVFIE)");
      if (v & IER_AWD) not_modelled(&a, "analog watchdog interrupts (IER.AWDxIE)");
      return;
    case CFGR:
      if (v & EXTEN) {
        std::snprintf(d, sizeof d, "(CFGR.EXTEN=%u, EXTSEL=%u", (v & EXTEN) >> 10, (v >> 6) & 0xFu);
        not_modelled(&a, std::string("hardware triggers ") + d + ")");
      }
      if (v & DMAEN) not_modelled(&a, "DMA requests (CFGR.DMAEN)");
      if (v & DFSDMCFG) not_modelled(&a, "DFSDM output (CFGR.DFSDMCFG)");
      if (v & CONT) not_modelled(&a, "continuous conversion mode (CFGR.CONT)");
      if (v & AUTDLY) not_modelled(&a, "auto-delayed conversion mode (CFGR.AUTDLY)");
      if (v & DISCEN) not_modelled(&a, "discontinuous mode (CFGR.DISCEN)");
      if (v & (JDISCEN | JQM | JAWD1EN | JAUTO))
        not_modelled(&a, "injected conversions (CFGR.JDISCEN/JQM/JAWD1EN/JAUTO)");
      if (v & AWD1EN) not_modelled(&a, "analog watchdog (CFGR.AWD1EN)");
      return;
    case CFGR2:
      if (v & (ROVSE | JOVSE)) not_modelled(&a, "oversampling (CFGR2.ROVSE/JOVSE)");
      return;
    case SQR1:
      if (v & 0xFu) {
        std::snprintf(d, sizeof d, "(SQR1.L=%u: a sequence of %u conversions", v & 0xFu, (v & 0xFu) + 1);
        not_modelled(&a, std::string("scan mode ") + d + ")");
      }
      return;
    case JSQR:
      if (v) not_modelled(&a, "injected conversions (JSQR)");
      return;
    case AWD2CR: case AWD3CR:
      if (v) not_modelled(&a, "analog watchdog (AWD2CR/AWD3CR)");
      return;
    case DIFSEL:
      if (v & 0x7FFFFu) not_modelled(&a, "differential inputs (DIFSEL)");
      return;
    default:
      if (r >= OFR1 && r <= OFR4 && (v & (1u << 31)))
        not_modelled(&a, "offset compensation (OFRx.OFFSET_EN)");
      return;
  }
}

void Adc::check_common_write(uint32_t v) {
  if (v & CCR_DUAL) not_modelled(nullptr, "dual mode (CCR.DUAL)");
  if (v & CCR_MDMA) not_modelled(nullptr, "DMA requests (CCR.MDMA)");
  if (((v & CCR_PRESC) >> 18) >= 12) not_modelled(nullptr, "reserved prescaler (CCR.PRESC)");
  const uint32_t old = regs_.get(CCR);
  if ((old ^ v) & (CCR_CKMODE | CCR_PRESC))
    for (const Inst& a : adc_)
      if (reg(a, CR) & ADEN)
        throw std::runtime_error(name_ + ": CCR.CKMODE/PRESC changed while " + label(a) +
                                 " is enabled (RM0351: only while every ADC is disabled; write " +
                                 hex(v) + ")");
}

// ---------------------------------------------------------------- control register

void Adc::write_cr(Inst& a, uint32_t off, unsigned width, uint32_t v) {
  const uint32_t old = reg(a, CR);
  const uint32_t lanes = lane_mask(off, width);
  const uint32_t wv = lane_merge(0, off, width, v) & lanes;
  uint32_t cr = (old & ~(kCrRw & lanes)) | (wv & kCrRw);
  const uint32_t set = wv & kCrRs & ~old;  // rs bits newly written 1
  set_reg(a, CR, cr);
  const std::string me = label(a);
  if (set & (JADSTART | JADSTP))
    not_modelled(&a, "injected conversions (CR.JADSTART/JADSTP)");
  auto powered = [&](const char* bit) {
    if ((cr & DEEPPWD) || !(cr & ADVREGEN))
      throw std::runtime_error(me + ": " + bit + " set in deep power-down or with the voltage regulator "
                               "off (CR.DEEPPWD=" + ((cr & DEEPPWD) ? "1" : "0") +
                               ", CR.ADVREGEN=" + ((cr & ADVREGEN) ? "1" : "0") + ")");
  };
  if (set & ADCAL) {
    powered("ADCAL");
    if (cr & (ADEN | ADSTART | ADDIS))
      throw std::runtime_error(me + ": ADCAL set while the ADC is enabled (CR.ADEN=1)");
    const uint64_t hz = adc_hz(a);
    cr |= ADCAL;
    set_reg(a, CR, cr);
    schedule(a, after_half_cycles(kCalHalf, hz), Phase::Cal);
  }
  if (set & ADEN) {
    powered("ADEN");
    if (cr & ADCAL) throw std::runtime_error(me + ": ADEN set while calibrating (CR.ADCAL=1)");
    (void)adc_hz(a);  // no ADC clock: ADRDY would never set
    cr |= ADEN;
    set_reg(a, CR, cr);
    a.ready = false;
    schedule(a, clock_.now_ns() + kAdrdyNs, Phase::Enable);
  }
  if (set & ADDIS) {
    if (!(cr & ADEN) || (cr & ADSTART))
      throw std::runtime_error(me + ": ADDIS set while the ADC is " +
                               ((cr & ADEN) ? "converting (CR.ADSTART=1)" : "not enabled (CR.ADEN=0)"));
    cr &= ~(ADEN | ADDIS);
    set_reg(a, CR, cr);
    a.ready = false;
    cancel(a);  // a pending ADRDY
  }
  if (set & ADSTART) {
    if (!(cr & ADEN) || !a.ready)
      throw std::runtime_error(me + ": ADSTART set while the ADC is not enabled and ready (ADRDY)");
    start_conversion(a);
  }
  if (set & ADSTP) {
    cr = reg(a, CR);
    if (cr & ADSTART) cancel(a);  // ongoing conversion discarded
    set_reg(a, CR, cr & ~(ADSTART | ADSTP));
  }
}

void Adc::start_conversion(Inst& a) {
  const unsigned ch = (reg(a, SQR1) >> 6) & 0x1Fu;
  const std::string me = label(a) + " channel " + std::to_string(ch);
  if (ch >= kNumCh) throw std::runtime_error(me + " does not exist (SQR1.SQ1)");
  if (ch == 0) {
    if (a.idx != 0) throw NotModelled(label(a), "channel 0 (internal)");
    if (!(regs_.get(CCR) & VREFEN))
      throw std::runtime_error(me + " (VREFINT) converted with ADC_CCR.VREFEN clear");
  } else if (ch == 17 || ch == 18) {
    const std::string c = "channel " + std::to_string(ch);
    if (a.idx == 1) throw NotModelled(label(a), c + " (DAC1 output " + std::to_string(ch - 16) + ", internal)");
    throw NotModelled(label(a), c + (ch == 17 ? " (temperature sensor)" : " (VBAT/3)"));
  }
  const uint32_t smp = ch < 10 ? (reg(a, SMPR1) >> (3 * ch)) & 7u : (reg(a, SMPR2) >> (3 * (ch - 10))) & 7u;
  uint64_t smp_half = kSmpHalf[smp];
  if (smp == 0 && (reg(a, SMPR1) & SMPPLUS)) smp_half = 7;  // 3.5 cycles
  const uint64_t sar_half = kSarHalf[(reg(a, CFGR) >> 3) & 3u];
  const uint64_t hz = adc_hz(a);
  set_reg(a, CR, reg(a, CR) | ADSTART);
  a.t_end = after_half_cycles(smp_half + sar_half, hz);
  schedule(a, after_half_cycles(smp_half, hz), Phase::Sample);
}

double Adc::sample_volts(Inst& a, unsigned ch) {
  if (ch == 0) return kVrefint;
  const auto& p = a.pins[ch];
  if (p && p->gpio)
    if (std::optional<double> v = p->gpio->voltage(p->pin)) return *v;
  const std::string what = label(a) + " channel " + std::to_string(ch) + " converted with no input set";
  if (strict_analog_) throw std::runtime_error(what);
  if (!a.warned[ch]) {
    a.warned[ch] = true;
    std::string where = p ? " (pin P" + p->port + std::to_string(p->pin) + ")" : " (no pin on this package)";
    std::fprintf(stderr, "stest: warning: %s%s; reading 0 V\n", what.c_str(), where.c_str());
  }
  return 0.0;
}

void Adc::on_event(Inst& a) {
  a.has_ev = false;
  const Phase p = a.phase;
  a.phase = Phase::None;
  switch (p) {
    case Phase::Cal: {
      const uint32_t cr = reg(a, CR), cal = reg(a, CALFACT);
      set_reg(a, CALFACT, (cr & ADCALDIF) ? (cal & 0x7Fu) | (kCalFactor << 16) : (cal & 0x7F0000u) | kCalFactor);
      set_reg(a, CR, cr & ~ADCAL);
      break;
    }
    case Phase::Enable:
      a.ready = true;
      set_reg(a, ISR, reg(a, ISR) | ADRDY);
      break;
    case Phase::Sample: {
      const unsigned ch = (reg(a, SQR1) >> 6) & 0x1Fu;
      const uint32_t cfgr = reg(a, CFGR);
      const unsigned bits = 12 - 2 * ((cfgr >> 3) & 3u);
      const double v = std::clamp(sample_volts(a, ch), 0.0, vref_);
      uint32_t code = static_cast<uint32_t>(std::lround(v / vref_ * ((1u << bits) - 1u)));
      if (cfgr & ALIGN) code <<= (bits == 6 ? 2 : 16 - bits);
      a.code = code;
      set_reg(a, ISR, reg(a, ISR) | EOSMP);
      update_irq(a);
      schedule(a, a.t_end, Phase::Convert);
      return;
    }
    case Phase::Convert: {
      uint32_t isr = reg(a, ISR);
      if (isr & EOC) isr |= OVR;
      if (!(reg(a, ISR) & EOC) || (reg(a, CFGR) & OVRMOD)) set_reg(a, DR, a.code);
      set_reg(a, ISR, isr | EOC | EOS);
      set_reg(a, CR, reg(a, CR) & ~ADSTART);  // single conversion, software trigger
      break;
    }
    case Phase::None:
      break;
  }
  update_irq(a);
}

// ---------------------------------------------------------------- bus access

uint32_t Adc::read(uint32_t off, unsigned width) {
  const uint32_t aligned = off & ~3u;
  if (aligned >= COMMON) {
    if (aligned == CSR) {
      (void)regs_.read(off, width);
      const uint32_t v = (reg(adc_[0], ISR) & 0x7FFu) | ((reg(adc_[1], ISR) & 0x7FFu) << 16);
      return lane_extract(v, off, width);
    }
    if (aligned == CDR) {
      throw NotModelled(name_, "dual mode (common data register CDR)");
    }
    return regs_.read(off, width);
  }
  Inst& a = adc_[aligned >> 8];
  const uint32_t v = regs_.read(off, width);
  if ((aligned & 0xFFu) == DR) {
    set_reg(a, ISR, reg(a, ISR) & ~EOC);
    update_irq(a);
  }
  return v;
}

void Adc::write(uint32_t off, unsigned width, uint32_t v) {
  const uint32_t aligned = off & ~3u;
  uint32_t old = 0;
  try {
    old = regs_.get(aligned);
  } catch (const UnmodelledRegister&) {
    regs_.write(off, width, v);  // undeclared offset: throws the strict write error
    throw;
  }
  const uint32_t full = lane_merge(old, off, width, v);
  if (aligned >= COMMON) {
    if (aligned == CCR) check_common_write(full);
    regs_.write(off, width, v);  // CSR / CDR: read-only (throws)
    return;
  }
  Inst& a = adc_[aligned >> 8];
  const uint32_t r = aligned & 0xFFu;
  if (r == CR) {
    regs_.read(off, width);  // strictness (declared)
    write_cr(a, off, width, v);
    return;
  }
  check_inst_write(a, r, full);
  regs_.write(off, width, v);
  if (r == ISR || r == IER) update_irq(a);
}

// ---------------------------------------------------------------- Python-facing ops

Adc::Inst& Adc::inst_arg(const nlohmann::json& args, unsigned* ch) {
  if (!args.contains("adc") || !args["adc"].is_number_integer())
    throw std::invalid_argument("missing integer 'adc' (1..3)");
  if (!args.contains("channel") || !args["channel"].is_number_integer())
    throw std::invalid_argument("missing integer 'channel' (0..18)");
  const int n = args["adc"].get<int>(), c = args["channel"].get<int>();
  if (n < 1 || n > static_cast<int>(kNumAdc)) throw std::out_of_range("adc out of range 1..3: " + std::to_string(n));
  if (c < 0 || c >= static_cast<int>(kNumCh)) throw std::out_of_range("channel out of range 0..18: " + std::to_string(c));
  *ch = static_cast<unsigned>(c);
  return adc_[n - 1];
}

nlohmann::json Adc::call(std::string_view op, const nlohmann::json& args) {
  if (op == "set_input" || op == "get_input") {
    unsigned ch = 0;
    Inst& a = inst_arg(args, &ch);
    const auto& p = a.pins[ch];
    if (op == "get_input") {
      std::optional<double> v = (p && p->gpio) ? p->gpio->voltage(p->pin) : std::nullopt;
      return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
    }
    if (!p || !p->gpio)
      throw std::invalid_argument(label(a) + " channel " + std::to_string(ch) +
                                  " has no pin on this package (part TOML [[analog]] table)");
    if (!args.contains("volts") || !args["volts"].is_number())
      throw std::invalid_argument("missing number 'volts'");
    return p->gpio->call("set_voltage", {{"pin", p->pin}, {"volts", args["volts"]}});
  }
  throw UnknownOp(op);
}

}  // namespace stest::stm32l4
