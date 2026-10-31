#include "stm32l4/models/gpio.hpp"
#include <cmath>
#include <stdexcept>
#include "stest/hal_api.h"
#include "stest/mmio.h"
#include "stest/node.hpp"
#include "stest/runtime.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  MODER = 0x00, OTYPER = 0x04, OSPEEDR = 0x08, PUPDR = 0x0C, IDR = 0x10, ODR = 0x14,
  BSRR = 0x18, LCKR = 0x1C, AFRL = 0x20, AFRH = 0x24, BRR = 0x28,
};
constexpr uint32_t kLckk = 1u << 16;

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

Gpio::Gpio(std::string name, const nlohmann::json& params, ModelContext& ctx)
    : name_(std::move(name)), port_(params.value("port", std::string{})), clock_(ctx.clock()) {
  if (port_.size() != 1) throw std::invalid_argument("gpio params need port=\"A\"..\"I\"");
  regs_.declare(MODER, params.value("moder_reset", 0xFFFFFFFFu), 0xFFFFFFFF);
  regs_.declare(OTYPER, 0, 0x0000FFFF);
  regs_.declare(OSPEEDR, params.value("ospeedr_reset", 0u), 0xFFFFFFFF);
  regs_.declare(PUPDR, params.value("pupdr_reset", 0u), 0xFFFFFFFF);
  regs_.declare(IDR, 0, 0);   // read-only (writes abort); computed on read
  regs_.declare(ODR, 0, 0x0000FFFF);
  regs_.declare_write_only(BSRR);
  regs_.declare_write_only(LCKR);  // lock sequence handled in write(); read-back via set()
  regs_.declare(AFRL, 0, 0xFFFFFFFF);
  regs_.declare(AFRH, 0, 0xFFFFFFFF);
  regs_.declare_write_only(BRR);
  drive_.fill(-1);
  last_levels_ = levels();
}

void Gpio::reset() {
  regs_.reset();
  drive_.fill(-1);
  ext_volts_.fill(std::nullopt);
  drv_volts_.fill(std::nullopt);
  lck_stage_ = 0;
  lck_mask_ = 0;
  locked_ = false;
  update();
}

int Gpio::level_of(unsigned pin, const Snapshot& s) const {
  if (drive_[pin] >= 0) return drive_[pin];
  uint32_t mode = (s.moder >> (2 * pin)) & 3u;
  uint32_t pull = (s.pupdr >> (2 * pin)) & 3u;
  int pull_level = (pull == 1) ? 1 : 0;
  if (mode == 1) {
    int odr = (s.odr >> pin) & 1u;
    bool open_drain = (s.otyper >> pin) & 1u;
    if (open_drain && odr) return pull_level;
    return odr;
  }
  return pull_level;
}

int Gpio::pin_level(unsigned pin) const { return level_of(pin, snapshot()); }

std::optional<double> Gpio::voltage(unsigned pin) const {
  return ext_volts_.at(pin) ? ext_volts_[pin] : drv_volts_.at(pin);
}

void Gpio::set_analog_drive(unsigned pin, std::optional<double> volts) { drv_volts_.at(pin) = volts; }

uint16_t Gpio::levels() const {
  const Snapshot s = snapshot();  // registers read once per call, not once per pin
  uint16_t l = 0;
  for (unsigned p = 0; p < 16; ++p)
    if (level_of(p, s)) l |= static_cast<uint16_t>(1u << p);
  return l;
}

void Gpio::update() {
  uint16_t now = levels();
  uint16_t changed = now ^ last_levels_;
  last_levels_ = now;
  if (!changed) return;
  const uint64_t t = clock_.now_ns();
  for (unsigned p = 0; p < 16; ++p)
    if (changed & (1u << p)) {
      const int level = (now >> p) & 1u;
      emit_event(t, {{"kind", "gpio"}, {"periph", name_}, {"port", port_}, {"pin", p}, {"level", level}});
      edges_.emit(PinEdge{port_[0], p, level, t});
    }
}

uint32_t Gpio::read(uint32_t off, unsigned width) {
  if ((off & ~3u) == IDR) {
    (void)regs_.read(off, width);  // strictness checks
    return lane_extract(levels(), off, width);
  }
  return regs_.read(off, width);
}

void Gpio::write(uint32_t off, unsigned width, uint32_t v) {
  uint32_t aligned = off & ~3u;
  uint32_t val = lane_merge(0, off, width, v);
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
      regs_.write(off, width, v);  // ODR; strict throw for IDR (read-only) and unknown offsets
      break;
  }
  update();
}

std::string_view Gpio::reg_name(uint32_t off) const {
  switch (off & ~3u) {
    case MODER: return "MODER";
    case OTYPER: return "OTYPER";
    case OSPEEDR: return "OSPEEDR";
    case PUPDR: return "PUPDR";
    case IDR: return "IDR";
    case ODR: return "ODR";
    case BSRR: return "BSRR";
    case LCKR: return "LCKR";
    case AFRL: return "AFRL";
    case AFRH: return "AFRH";
    case BRR: return "BRR";
    default: return {};
  }
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
  if (op == "set_voltage") {
    unsigned p = pin_arg(args);
    if (!args.contains("volts") || !args["volts"].is_number() || !std::isfinite(args["volts"].get<double>()))
      throw std::invalid_argument("missing finite number 'volts'");
    ext_volts_[p] = args["volts"].get<double>();
    return nullptr;
  }
  if (op == "get_voltage") {
    std::optional<double> v = voltage(pin_arg(args));
    return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
  }
  if (op == "release_voltage") {
    ext_volts_[pin_arg(args)] = std::nullopt;
    return nullptr;
  }
  throw UnknownOp(op);
}

}  // namespace stest::stm32l4

// ---- HAL-mode C API (stest/hal_api.h) ----
// Each helper is one register access of ST's stm32l4xx_hal_gpio.c, applied to the GPIO model
// mapped at that address via stest::shim_access (same time/IRQ/poll accounting as MMIO).
namespace {
using stest::Peripheral;
using stest::stm32l4::Gpio;
using namespace stest::stm32l4;  // register offsets (MODER, ODR, ...)
constexpr uint64_t kGpioBase = 0x48000000u, kGpioStride = 0x400u;  // GPIOA_BASE, RM0351 2.2.2
// ST GPIO_InitTypeDef.Mode encoding (stm32l4xx_hal_gpio.h).
constexpr uint32_t kModeMask = 0x3u, kModeOutput = 1u, kModeAf = 2u, kModeAnalog = 3u;
constexpr unsigned kOutputTypePos = 4u;

uint64_t port_base(char port) {
  if (port < 'A' || port > 'Z') stest_rt_abort("GPIO HAL shim: bad GPIO port (not a GPIOx pointer)");
  return kGpioBase + static_cast<uint64_t>(port - 'A') * kGpioStride;
}
Gpio& as_gpio(Peripheral& p) {
  if (auto* g = dynamic_cast<Gpio*>(&p)) return *g;
  stest_rt_abort("GPIO HAL shim: %.*s is not a GPIO model", static_cast<int>(p.name().size()),
                 p.name().data());
}
uint32_t load(uint64_t base, uint32_t off) {
  return stest::shim_access(base + off, 4, false, [&](Peripheral& p) { return as_gpio(p).read(off, 4); });
}
void store(uint64_t base, uint32_t off, uint32_t v) {
  stest::shim_access(base + off, 4, true, [&](Peripheral& p) { as_gpio(p).write(off, 4, v); });
}
// temp = REG; temp &= ~clear; temp |= set; REG = temp;
void rmw(uint64_t base, uint32_t off, uint32_t clear, uint32_t set) {
  store(base, off, (load(base, off) & ~clear) | set);
}
}  // namespace

extern "C" {

void stest_gpio_hal_init(char port, uint16_t pins, uint32_t mode, uint32_t pull, uint32_t speed,
                         uint32_t alternate) {
  const uint64_t b = port_base(port);
  const uint32_t m = mode & kModeMask;
  for (unsigned pos = 0; pos < 16; ++pos) {
    if (!(pins & (1u << pos))) continue;
    if (m == kModeOutput || m == kModeAf) {
      rmw(b, OSPEEDR, 3u << (pos * 2), speed << (pos * 2));
      rmw(b, OTYPER, 1u << pos, ((mode >> kOutputTypePos) & 1u) << pos);
    }
    if (m != kModeAnalog) rmw(b, PUPDR, 3u << (pos * 2), pull << (pos * 2));
    if (m == kModeAf) {
      const uint32_t afr = pos < 8 ? AFRL : AFRH, sh = (pos & 7u) * 4;
      rmw(b, afr, 0xFu << sh, alternate << sh);
    }
    rmw(b, MODER, 3u << (pos * 2), m << (pos * 2));
  }
}

void stest_gpio_hal_deinit(char port, uint16_t pins) {
  const uint64_t b = port_base(port);
  for (unsigned pos = 0; pos < 16; ++pos) {
    if (!(pins & (1u << pos))) continue;
    rmw(b, MODER, 0, 3u << (pos * 2));
    rmw(b, pos < 8 ? AFRL : AFRH, 0xFu << ((pos & 7u) * 4), 0);
    rmw(b, OSPEEDR, 3u << (pos * 2), 0);
    rmw(b, OTYPER, 1u << pos, 0);
    rmw(b, PUPDR, 3u << (pos * 2), 0);
  }
}

void stest_gpio_hal_write(char port, uint16_t pins, int level) {
  store(port_base(port), level ? BSRR : BRR, pins);
}

int stest_gpio_hal_read(char port, uint16_t pin) {
  return (load(port_base(port), IDR) & pin) != 0 ? 1 : 0;
}

void stest_gpio_hal_toggle(char port, uint16_t pins) {
  const uint64_t b = port_base(port);
  const uint32_t odr = load(b, ODR);
  store(b, BSRR, ((odr & pins) << 16) | (~odr & pins));
}

void stest_gpio_hal_lckr_write(char port, uint32_t value) { store(port_base(port), LCKR, value); }

uint32_t stest_gpio_hal_lckr_read(char port) { return load(port_base(port), LCKR); }

}  // extern "C"
