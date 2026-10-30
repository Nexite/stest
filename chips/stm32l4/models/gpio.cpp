#include "stm32l4/models/gpio.hpp"
#include <stdexcept>
#include "stest/node.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  MODER = 0x00, OTYPER = 0x04, OSPEEDR = 0x08, PUPDR = 0x0C, IDR = 0x10, ODR = 0x14,
  BSRR = 0x18, LCKR = 0x1C, AFRL = 0x20, AFRH = 0x24, BRR = 0x28,
};
constexpr uint32_t kLckk = 1u << 16;

uint32_t lane_value(uint32_t off, unsigned width, uint32_t v) {
  unsigned sh = (off & 3u) * 8u;
  uint32_t m = (width == 4) ? 0xFFFFFFFFu : (((1u << (width * 8)) - 1u) << sh);
  return (v << sh) & m;
}
uint32_t spread(uint16_t pins, unsigned bits, unsigned first, unsigned count) {
  uint32_t m = 0;
  for (unsigned i = 0; i < count; ++i)
    if (pins & (1u << (first + i))) m |= ((1u << bits) - 1u) << (i * bits);
  return m;
}
unsigned pin_arg(const nlohmann::json& args) {
  if (!args.contains("pin") || !args["pin"].is_number_integer())
    throw std::invalid_argument("missing integer 'pin'");
  int p = args["pin"].get<int>();
  if (p < 0 || p > 15) throw std::out_of_range("pin out of range 0..15: " + std::to_string(p));
  return static_cast<unsigned>(p);
}
}  // namespace

Gpio::Gpio(std::string name, const nlohmann::json& params, Clock& clock)
    : name_(std::move(name)), port_(params.value("port", std::string{})), clock_(clock) {
  if (port_.size() != 1) throw std::invalid_argument("gpio params need port=\"A\"..\"I\"");
  regs_.declare(MODER, params.value("moder_reset", 0xFFFFFFFFu), 0xFFFFFFFF);
  regs_.declare(OTYPER, 0, 0x0000FFFF);
  regs_.declare(OSPEEDR, params.value("ospeedr_reset", 0u), 0xFFFFFFFF);
  regs_.declare(PUPDR, params.value("pupdr_reset", 0u), 0xFFFFFFFF);
  regs_.declare(IDR, 0, 0);   // computed on read
  regs_.declare(ODR, 0, 0x0000FFFF);
  regs_.declare(BSRR, 0, 0);  // write-only
  regs_.declare(LCKR, 0, 0);  // sequence handled in write()
  regs_.declare(AFRL, 0, 0xFFFFFFFF);
  regs_.declare(AFRH, 0, 0xFFFFFFFF);
  regs_.declare(BRR, 0, 0);   // write-only
  drive_.fill(-1);
  last_levels_ = levels();
}

void Gpio::reset() {
  regs_.reset();
  drive_.fill(-1);
  lck_stage_ = 0;
  lck_mask_ = 0;
  locked_ = false;
  update();
}

int Gpio::pin_level(unsigned pin) const {
  if (drive_[pin] >= 0) return drive_[pin];
  uint32_t mode = (regs_.get(MODER) >> (2 * pin)) & 3u;
  uint32_t pull = (regs_.get(PUPDR) >> (2 * pin)) & 3u;
  int pull_level = (pull == 1) ? 1 : 0;
  if (mode == 1) {
    int odr = (regs_.get(ODR) >> pin) & 1u;
    bool open_drain = (regs_.get(OTYPER) >> pin) & 1u;
    if (open_drain && odr) return pull_level;
    return odr;
  }
  return pull_level;
}

uint16_t Gpio::levels() const {
  uint16_t l = 0;
  for (unsigned p = 0; p < 16; ++p)
    if (pin_level(p)) l |= static_cast<uint16_t>(1u << p);
  return l;
}

void Gpio::update() {
  uint16_t now = levels();
  uint16_t changed = now ^ last_levels_;
  last_levels_ = now;
  if (!changed) return;
  for (unsigned p = 0; p < 16; ++p)
    if (changed & (1u << p))
      emit_event(clock_.now_ns(), {{"kind", "gpio"}, {"port", port_}, {"pin", p},
                                   {"level", (now >> p) & 1u}});
}

uint32_t Gpio::read(uint32_t off, unsigned width) {
  if ((off & ~3u) == IDR) {
    (void)regs_.read(off, width);  // strictness checks
    return (uint32_t{levels()} >> ((off & 3u) * 8u)) &
           (width == 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u));
  }
  return regs_.read(off, width);
}

void Gpio::write(uint32_t off, unsigned width, uint32_t v) {
  uint32_t aligned = off & ~3u;
  uint32_t val = lane_value(off, width, v);
  switch (aligned) {
    case BSRR: {
      regs_.write(off, width, v);  // stores nothing; strictness checks
      uint32_t set = val & 0xFFFFu, clr = (val >> 16) & ~set;  // BS has priority
      regs_.set(ODR, (regs_.get(ODR) & ~clr) | set);
      break;
    }
    case BRR:
      regs_.write(off, width, v);
      regs_.set(ODR, regs_.get(ODR) & ~(val & 0xFFFFu));
      break;
    case LCKR: {
      regs_.write(off, width, v);
      if (locked_) break;
      if (width != 4) {
        lck_stage_ = 0;
        break;
      }
      uint16_t mask = static_cast<uint16_t>(v & 0xFFFFu);
      bool k = v & kLckk;
      if (lck_stage_ == 0 && k) {
        lck_stage_ = 1;
        lck_mask_ = mask;
      } else if (lck_stage_ == 1 && !k && mask == lck_mask_) {
        lck_stage_ = 2;
      } else if (lck_stage_ == 2 && k && mask == lck_mask_) {
        locked_ = true;
        lck_stage_ = 0;
      } else {
        lck_stage_ = k ? 1 : 0;
        lck_mask_ = mask;
      }
      regs_.set(LCKR, (locked_ ? kLckk : 0u) | (locked_ ? lck_mask_ : mask));
      break;
    }
    case MODER: case OTYPER: case OSPEEDR: case PUPDR: case AFRL: case AFRH: {
      uint32_t before = regs_.get(aligned);
      regs_.write(off, width, v);
      if (locked_) {
        uint32_t keep = 0;
        switch (aligned) {
          case OTYPER: keep = lck_mask_; break;
          case AFRL: keep = spread(lck_mask_, 4, 0, 8); break;
          case AFRH: keep = spread(lck_mask_, 4, 8, 8); break;
          default: keep = spread(lck_mask_, 2, 0, 16); break;
        }
        regs_.set(aligned, (regs_.get(aligned) & ~keep) | (before & keep));
      }
      break;
    }
    default:
      regs_.write(off, width, v);  // ODR, or strict throw for IDR (rw=0 is ignored) / unknown
      break;
  }
  update();
}

nlohmann::json Gpio::call(std::string_view op, const nlohmann::json& args) {
  if (op == "get_pin") return pin_level(pin_arg(args));
  if (op == "drive_pin") {
    unsigned p = pin_arg(args);
    if (!args.contains("level") || !args["level"].is_number_integer())
      throw std::invalid_argument("missing integer 'level'");
    drive_[p] = args["level"].get<int>() ? 1 : 0;
    update();
    return nullptr;
  }
  if (op == "release_pin") {
    drive_[pin_arg(args)] = -1;
    update();
    return nullptr;
  }
  throw UnknownOp(op);
}

}  // namespace stest::stm32l4
