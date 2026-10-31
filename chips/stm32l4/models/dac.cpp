#include "stm32l4/models/dac.hpp"
#include <cstdio>
#include <stdexcept>
#include "stest/node.hpp"
#include "stm32l4/models/gpio.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  CR = 0x00, SWTRIGR = 0x04, DHR12R1 = 0x08, DHR12L1 = 0x0C, DHR8R1 = 0x10, DHR12R2 = 0x14,
  DHR12L2 = 0x18, DHR8R2 = 0x1C, DHR12RD = 0x20, DHR12LD = 0x24, DHR8RD = 0x28, DOR1 = 0x2C,
  DOR2 = 0x30, SR = 0x34, CCR = 0x38, MCR = 0x3C, SHSR1 = 0x40, SHSR2 = 0x44, SHHR = 0x48,
  SHRR = 0x4C,
};
// CR fields of channel 1 (DAC_CR_*); channel 2 is the same shifted by 16.
constexpr uint32_t EN = 1u << 0, TEN = 1u << 2, TSEL = 7u << 3, WAVE = 3u << 6, DMAEN = 1u << 12,
                   DMAUDRIE = 1u << 13, CEN = 1u << 14;
constexpr uint32_t TSEL_SW = 7u;
constexpr uint32_t DMAUDR = 1u << 13;  // SR

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}
}  // namespace

Dac::Dac(std::string name, const nlohmann::json& params, ModelContext& ctx)
    : name_(std::move(name)), ctx_(ctx), clock_(ctx.clock()) {
  vref_ = params.value("vref", 3.3);
  if (!(vref_ > 0.0)) throw std::invalid_argument(name_ + ": dac vref must be > 0");
  irq_ = params.value("irq", -1);
  if (params.contains("pins")) {
    const auto& pins = params["pins"];
    if (!pins.is_array() || pins.size() != 2) throw std::invalid_argument(name_ + ": dac params.pins needs 2 pins");
    for (unsigned c = 0; c < 2; ++c) {
      ch_[c].pin_name = pins[c].get<std::string>();
      const std::string& p = ch_[c].pin_name;
      if (p.size() < 3 || p[0] != 'P') throw std::invalid_argument(name_ + ": bad dac pin " + p);
      ch_[c].pin = static_cast<unsigned>(std::stoul(p.substr(2)));
      if (ch_[c].pin > 15) throw std::invalid_argument(name_ + ": bad dac pin " + p);
    }
  }
  regs_.declare(CR, 0, 0x7FFD7FFDu);
  regs_.declare(SWTRIGR, 0, 0x3u);
  regs_.declare(DHR12R1, 0, 0xFFFu);
  regs_.declare(DHR12L1, 0, 0xFFF0u);
  regs_.declare(DHR8R1, 0, 0xFFu);
  regs_.declare(DHR12R2, 0, 0xFFFu);
  regs_.declare(DHR12L2, 0, 0xFFF0u);
  regs_.declare(DHR8R2, 0, 0xFFu);
  regs_.declare(DHR12RD, 0, 0x0FFF0FFFu);
  regs_.declare(DHR12LD, 0, 0xFFF0FFF0u);
  regs_.declare(DHR8RD, 0, 0xFFFFu);
  regs_.declare(DOR1, 0, 0);  // read-only
  regs_.declare(DOR2, 0, 0);
  regs_.declare(SR, 0, 0, DMAUDR | (DMAUDR << 16));
  regs_.declare(CCR, 0, 0x001F001Fu);
  regs_.declare(MCR, 0, 0x00070007u);
  regs_.declare(SHSR1, 0, 0x3FFu);
  regs_.declare(SHSR2, 0, 0x3FFu);
  regs_.declare(SHHR, 0x00010001u, 0x03FF03FFu);
  regs_.declare(SHRR, 0x00010001u, 0x00FF00FFu);
}

void Dac::connect(ModelContext& ctx) {
  clocks_ = &ctx.clocks();
  for (Chan& c : ch_)
    if (!c.pin_name.empty()) c.gpio = &ctx.get<Gpio>("GPIO" + c.pin_name.substr(1, 1));
}

void Dac::reset() {
  for (Chan& c : ch_) {
    if (c.has_ev) clock_.cancel(c.ev);
    c.has_ev = false;
    c.dhr = 0;
    c.out.reset();
    if (c.gpio && c.driving) c.gpio->set_analog_drive(c.pin, std::nullopt);
    c.driving = false;
  }
  regs_.reset();
  update_irq();
}

void Dac::not_modelled(const std::string& what, uint32_t off, uint32_t v) const {
  char b[64];
  std::snprintf(b, sizeof b, "; write %s at offset 0x%02x)", hex(v).c_str(), off);
  throw std::runtime_error(name_ + " " + what + b);
}

void Dac::check_write(uint32_t off, uint32_t v) const {
  if (off == CR) {
    for (unsigned c = 0; c < 2; ++c) {
      const uint32_t f = v >> (16 * c);
      const std::string n = std::to_string(c + 1);
      if ((f & TEN) && ((f & TSEL) >> 3) != TSEL_SW)
        not_modelled("hardware triggers not modelled (CR.TEN" + n + "=1, TSEL" + n + "=" +
                         std::to_string((f & TSEL) >> 3) + "; only the software trigger TSEL=7 is",
                     off, v);
      if (f & WAVE) not_modelled("wave generation not modelled (CR.WAVE" + n, off, v);
      if (f & (DMAEN | DMAUDRIE)) not_modelled("DMA requests not modelled (CR.DMAEN" + n + "/DMAUDRIE" + n, off, v);
      if (f & CEN) not_modelled("calibration mode not modelled (CR.CEN" + n, off, v);
    }
  } else if (off == MCR) {
    for (unsigned c = 0; c < 2; ++c)
      if (((v >> (16 * c)) & 7u) >= 4)
        not_modelled("sample-and-hold mode not modelled (MCR.MODE" + std::to_string(c + 1) + ">=4", off, v);
  }
}

void Dac::sync_dhr_views() {
  const uint32_t a = ch_[0].dhr, b = ch_[1].dhr;
  regs_.set(DHR12R1, a);
  regs_.set(DHR12L1, a << 4);
  regs_.set(DHR8R1, a >> 4);
  regs_.set(DHR12R2, b);
  regs_.set(DHR12L2, b << 4);
  regs_.set(DHR8R2, b >> 4);
  regs_.set(DHR12RD, a | (b << 16));
  regs_.set(DHR12LD, (a << 4) | (b << 20));
  regs_.set(DHR8RD, (a >> 4) | ((b >> 4) << 8));
}

void Dac::schedule_transfer(unsigned c) {
  Chan& ch = ch_[c];
  if (ch.has_ev) return;  // the pending transfer loads the latest DHR
  const uint64_t pclk = clocks_ ? clocks_->pclk1_hz() : 0;
  if (pclk == 0) throw std::runtime_error(name_ + ": APB1 clock is 0 Hz");
  const uint64_t dt = (1000000000ull + pclk - 1) / pclk;  // one APB1 clock, rounded up
  ch.ev = clock_.schedule_at(clock_.now_ns() + dt, [this, c] { transfer(c); });
  ch.has_ev = true;
}

void Dac::transfer(unsigned c) {
  ch_[c].has_ev = false;
  regs_.set(c == 0 ? DOR1 : DOR2, ch_[c].dhr);
  regs_.set(SWTRIGR, regs_.get(SWTRIGR) & ~(1u << c));
  update_outputs();
}

std::optional<double> Dac::output(unsigned c) const {
  if (!((regs_.get(CR) >> (16 * c)) & EN)) return std::nullopt;
  return static_cast<double>(regs_.get(c == 0 ? DOR1 : DOR2)) / 4095.0 * vref_;
}

bool Dac::pin_connected(unsigned c) const { return ((regs_.get(MCR) >> (16 * c)) & 7u) != 3u; }

void Dac::update_outputs() {
  for (unsigned c = 0; c < 2; ++c) {
    Chan& ch = ch_[c];
    const std::optional<double> o = output(c);
    if (o != ch.out) {
      ch.out = o;
      emit_event(clock_.now_ns(), {{"kind", "dac"}, {"channel", c + 1},
                                   {"volts", o ? nlohmann::json(*o) : nlohmann::json(nullptr)}});
    }
    if (ch.gpio) {
      const bool drive = o && pin_connected(c);
      if (drive) ch.gpio->set_analog_drive(ch.pin, o);
      else if (ch.driving) ch.gpio->set_analog_drive(ch.pin, std::nullopt);
      ch.driving = drive;
    }
  }
}

void Dac::update_irq() {
  const uint32_t sr = regs_.get(SR), cr = regs_.get(CR);
  const bool lv = ((sr & DMAUDR) && (cr & DMAUDRIE)) || ((sr & (DMAUDR << 16)) && (cr & (DMAUDRIE << 16)));
  if (irq_ >= 0 && lv != level_) {
    level_ = lv;
    ctx_.set_irq_source(irq_, &tag_, lv);
  }
}

uint32_t Dac::read(uint32_t off, unsigned width) { return regs_.read(off, width); }

void Dac::write(uint32_t off, unsigned width, uint32_t v) {
  const uint32_t aligned = off & ~3u;
  uint32_t old = 0;
  try {
    old = regs_.get(aligned);
  } catch (const UnmodelledRegister&) {
    regs_.write(off, width, v);  // undeclared offset: throws the strict write error
    throw;
  }
  const uint32_t full = lane_merge(old, off, width, v);
  check_write(aligned, full);
  regs_.write(off, width, v);  // read-only registers (DOR) throw here
  const uint32_t val = regs_.get(aligned);
  bool d1 = false, d2 = false;
  switch (aligned) {
    case DHR12R1: ch_[0].dhr = val; d1 = true; break;
    case DHR12L1: ch_[0].dhr = val >> 4; d1 = true; break;
    case DHR8R1: ch_[0].dhr = val << 4; d1 = true; break;
    case DHR12R2: ch_[1].dhr = val; d2 = true; break;
    case DHR12L2: ch_[1].dhr = val >> 4; d2 = true; break;
    case DHR8R2: ch_[1].dhr = val << 4; d2 = true; break;
    case DHR12RD: ch_[0].dhr = val & 0xFFFu; ch_[1].dhr = val >> 16; d1 = d2 = true; break;
    case DHR12LD: ch_[0].dhr = (val >> 4) & 0xFFFu; ch_[1].dhr = val >> 20; d1 = d2 = true; break;
    case DHR8RD: ch_[0].dhr = (val & 0xFFu) << 4; ch_[1].dhr = (val >> 8) << 4; d1 = d2 = true; break;
    case SWTRIGR: {
      const uint32_t cr = regs_.get(CR);
      uint32_t keep = 0;
      for (unsigned c = 0; c < 2; ++c) {
        const uint32_t f = cr >> (16 * c);
        if ((val & (1u << c)) && (f & TEN) && ((f & TSEL) >> 3) == TSEL_SW) {
          keep |= 1u << c;
          schedule_transfer(c);
        }
      }
      regs_.set(SWTRIGR, keep);  // without a software trigger selected SWTRIG has no effect
      return;
    }
    case SR:
      update_irq();
      return;
    case CR: case MCR:
      update_outputs();
      update_irq();
      return;
    default:
      return;
  }
  sync_dhr_views();
  const uint32_t cr = regs_.get(CR);
  if (d1 && !(cr & TEN)) schedule_transfer(0);
  if (d2 && !((cr >> 16) & TEN)) schedule_transfer(1);
}

nlohmann::json Dac::call(std::string_view op, const nlohmann::json&) {
  if (op != "state") throw UnknownOp(op);
  nlohmann::json st = {{"vref", vref_}, {"channels", nlohmann::json::array()}};
  for (unsigned c = 0; c < 2; ++c) {
    const std::optional<double> o = output(c);
    st["channels"].push_back({{"ch", c + 1},
                              {"enabled", o.has_value()},
                              {"dhr", ch_[c].dhr},
                              {"dor", regs_.get(c == 0 ? DOR1 : DOR2)},
                              {"volts", o ? nlohmann::json(*o) : nlohmann::json(nullptr)},
                              {"pin", (!ch_[c].pin_name.empty() && pin_connected(c))
                                          ? nlohmann::json(ch_[c].pin_name)
                                          : nlohmann::json(nullptr)}});
  }
  return st;
}

}  // namespace stest::stm32l4
