#include "stm32l4/models/exti.hpp"
#include <stdexcept>
#include "stest/cortexm.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  IMR1 = 0x00, EMR1 = 0x04, RTSR1 = 0x08, FTSR1 = 0x0C, SWIER1 = 0x10, PR1 = 0x14,
  IMR2 = 0x20, EMR2 = 0x24, RTSR2 = 0x28, FTSR2 = 0x2C, SWIER2 = 0x30, PR2 = 0x34,
};
constexpr uint32_t kLines1 = 0x007DFFFF;  // implemented configurable lines 0-16, 18-22
constexpr uint32_t kLines2 = 0x00000078;  // lines 35-38
constexpr uint32_t kMask16 = 0x0000FFFF;
}  // namespace

Exti::Exti(std::string name, const nlohmann::json& params, ModelContext& ctx)
    : name_(std::move(name)), core_(ctx.core()) {
  if (!params.contains("irq") || !params["irq"].is_array() || params["irq"].size() != 16)
    throw std::invalid_argument("exti params need irq = [16 NVIC IRQ numbers for lines 0..15]");
  for (size_t i = 0; i < 16; ++i) irq_[i] = params["irq"][i].get<int>();
  regs_.declare(IMR1, 0xFF820000, 0xFFFFFFFF);  // IM0..IM31 (direct lines reset to 1)
  regs_.declare(EMR1, 0, 0xFFFFFFFF);
  regs_.declare(RTSR1, 0, kLines1);
  regs_.declare(FTSR1, 0, kLines1);
  regs_.declare(SWIER1, 0, kLines1);
  regs_.declare(PR1, 0, 0, kLines1);  // rc_w1
  regs_.declare(IMR2, 0x00000187, 0x000001FF);  // IM32..IM40
  regs_.declare(EMR2, 0, 0x000001FF);
  regs_.declare(RTSR2, 0, kLines2);
  regs_.declare(FTSR2, 0, kLines2);
  regs_.declare(SWIER2, 0, kLines2);
  regs_.declare(PR2, 0, 0, kLines2);
}

void Exti::reset() { regs_.reset(); }

void Exti::connect(ModelContext& ctx) {
  syscfg_ = &ctx.get<Syscfg>("SYSCFG");
  for (char port = 'A'; port <= 'I'; ++port) {
    Peripheral* p = ctx.find(std::string("GPIO") + port);
    if (auto* g = dynamic_cast<Gpio*>(p)) g->edges().subscribe([this](const PinEdge& e) { on_edge(e); });
  }
}

uint32_t Exti::active() const { return regs_.get(PR1) & regs_.get(IMR1) & kMask16; }

// Drive each routed IRQ's level: high while any of its lines has PR & IMR set.
void Exti::pend_new(uint32_t before) {
  uint32_t now = active();
  if (now == before) return;
  for (unsigned line = 0; line < 16; ++line) {
    bool high = false;
    for (unsigned l = 0; l < 16; ++l)
      if (irq_[l] == irq_[line] && (now & (1u << l))) high = true;
    core_.set_irq_level(irq_[line], high);
  }
}

void Exti::on_edge(const PinEdge& e) {
  if (e.pin > 15 || syscfg_->exti_port(e.pin) != static_cast<unsigned>(e.port - 'A')) return;
  uint32_t bit = 1u << e.pin;
  if (!(regs_.get(e.level ? RTSR1 : FTSR1) & bit)) return;
  uint32_t before = active();
  regs_.set(PR1, regs_.get(PR1) | bit);
  pend_new(before);
}

void Exti::write(uint32_t off, unsigned width, uint32_t v) {
  uint32_t aligned = off & ~3u;
  uint32_t before = active();
  switch (aligned) {
    case SWIER1: case SWIER2: {
      uint32_t old = regs_.get(aligned);
      regs_.write(off, width, v);
      uint32_t rose = regs_.get(aligned) & ~old;
      uint32_t pr = aligned == SWIER1 ? PR1 : PR2;
      regs_.set(pr, regs_.get(pr) | rose);
      break;
    }
    case PR1: case PR2: {
      uint32_t old = regs_.get(aligned);
      regs_.write(off, width, v);
      uint32_t cleared = old & ~regs_.get(aligned);
      uint32_t sw = aligned == PR1 ? SWIER1 : SWIER2;
      regs_.set(sw, regs_.get(sw) & ~cleared);  // clearing PR clears the software trigger
      break;
    }
    default:
      regs_.write(off, width, v);
      break;
  }
  pend_new(before);
}

}  // namespace stest::stm32l4
