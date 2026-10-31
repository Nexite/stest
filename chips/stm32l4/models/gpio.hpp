#pragma once
// STM32L4 (L496) GPIO port (0x400 block): MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR,
// BSRR, LCKR, AFRL, AFRH, BRR (L496 has no ASCR: strict).
//
// Pin level (what IDR reads and what events report), per pin:
//   external drive (drive_pin) if set, else
//   ODR bit for an output pin (open-drain with ODR=1 releases the line -> pull level),
//   else the pull level (pull-up 1, pull-down 0, none 0).
// Every level change emits {"kind":"gpio","port":"A","pin":5,"level":1} at the clock's
// current time into the node event buffer (pins in ascending order per access).
// BSRR: BSx has priority over BRx. LCKR implements the LCKK write sequence; locked
// pins keep their MODER/OTYPER/OSPEEDR/PUPDR/AFR bits until reset.
//
// params: {"port": "A", "moder_reset": u32 (default 0xFFFFFFFF),
//          "ospeedr_reset": u32 (default 0), "pupdr_reset": u32 (default 0)}
// Signal edges(): see PinEdge.
// call(): "get_pin" {pin} -> 0/1; "drive_pin" {pin, level}; "release_pin" {pin}.
#include <array>
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

// A pin level change, emitted synchronously (Gpio::edges()) at the virtual time it happens,
// whether caused by firmware writes (MODER/ODR/BSRR/PUPDR/...) or by drive_pin/release_pin.
struct PinEdge {
  char port;      // 'A'..'I'
  unsigned pin;   // 0..15
  int level;      // new level, 0/1
  uint64_t t_ns;  // Clock::now_ns() at the change
};

class Gpio : public Peripheral {
 public:
  Gpio(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;
  nlohmann::json call(std::string_view op, const nlohmann::json& args) override;

  int pin_level(unsigned pin) const;
  // Fires once per level change, pins in ascending order, before the causing access returns
  // (the same changes that become "gpio" events). EXTI subscribes in its connect().
  Signal<PinEdge>& edges() { return edges_; }

 private:
  struct Snapshot { uint32_t moder, otyper, pupdr, odr; };
  Snapshot snapshot() const {
    return {regs_.get(0x00), regs_.get(0x04), regs_.get(0x0C), regs_.get(0x14)};
  }
  int level_of(unsigned pin, const Snapshot& s) const;
  uint16_t levels() const;
  void update();  // recompute levels, emit change events
  std::string name_;
  std::string port_;
  Clock& clock_;
  RegisterFile regs_;
  std::array<int8_t, 16> drive_{};  // -1: not driven
  uint16_t last_levels_ = 0;
  int lck_stage_ = 0;
  uint16_t lck_mask_ = 0;
  bool locked_ = false;
  Signal<PinEdge> edges_;
};

}  // namespace stest::stm32l4
