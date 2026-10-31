#pragma once
// STM32L4 (L496) GPIO port (0x400 block): MODER, OTYPER, OSPEEDR, PUPDR, IDR, ODR,
// BSRR, LCKR, AFRL, AFRH, BRR (L496 has no ASCR: strict).
//
// Pin level (what IDR reads and what events report), per pin:
//   external drive (drive_pin) if set, else
//   ODR bit for an output pin (open-drain with ODR=1 releases the line -> pull level),
//   else the pull level (pull-up 1, pull-down 0, none 0).
// Every level change emits {"kind":"gpio","periph":"GPIOA","port":"A","pin":5,"level":1} at the clock's
// current time into the node event buffer (pins in ascending order per access).
// BSRR: BSx has priority over BRx. LCKR implements the LCKK write sequence; locked
// pins keep their MODER/OTYPER/OSPEEDR/PUPDR/AFR bits until reset.
//
// params: {"port": "A", "moder_reset": u32 (default 0xFFFFFFFF),
//          "ospeedr_reset": u32 (default 0), "pupdr_reset": u32 (default 0)}
// Signal edges(): see PinEdge.
// call(): "get_pin" {pin} -> 0/1; "drive_pin" {pin, level}; "release_pin" {pin}.
//
// Analog voltage (ADC inputs, DAC outputs), per pin, independent of the digital level
// above (it never changes IDR): an external voltage set from outside (call "set_voltage"
// {pin, volts}, e.g. Python's Pin.set_voltage) wins over an on-chip analog driver
// (set_analog_drive(), used by the DAC for its output pins), like drive_pin overrides the
// firmware's output. Neither set: voltage() is empty (the pin floats). MODEL_CALL is the
// outside world driving the pin, so set_voltage works whatever the RCC clock state.
// set_voltage is accepted only on pins with an analog function: the ADC (from the part
// TOML's [[analog]] table) and the DAC (its output pins) register theirs in connect() via
// add_analog_function(). Any other pin -> std::invalid_argument (REQ_ERROR in Python)
// "PB11 has no analog function on STM32L496RG; analog pins: PA0, PA1, ...".
// call(): "set_voltage" {pin, volts (finite number)}; "get_voltage" {pin} -> volts | null;
// "release_voltage" {pin} (drop the external voltage).
//
// HAL mode (MODELS GPIO=hal): the GPIO shim (chips/stm32l4/hal/stm32l4xx_hal_gpio_shim.c)
// calls the stest_gpio_hal_* C API (stest/hal_api.h, implemented at the end of gpio.cpp),
// which applies ST's driver operations to this model directly through read()/write() (so
// locking, BSRR priority, levels and events behave exactly as in register mode), accounting
// each as the register access ST's code makes (stest::shim_access). Firmware MMIO to a
// hal-mode GPIO block still works but warns once per register (Bus::set_hal_owned, using
// reg_name()).
#include <array>
#include <cstdint>
#include <optional>
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
  std::string_view reg_name(uint32_t off) const override;

  int pin_level(unsigned pin) const;
  // Analog voltage of `pin` (external, else on-chip driver), empty if neither is set.
  std::optional<double> voltage(unsigned pin) const;
  // On-chip analog driver (DAC output buffer): a voltage, or empty to stop driving.
  void set_analog_drive(unsigned pin, std::optional<double> volts);
  // Wiring (called from other models' connect()): `pin` has analog function `fn`
  // ("ADC12_IN16", "DAC1_OUT1"); several functions per pin accumulate.
  void add_analog_function(unsigned pin, const std::string& fn);
  const std::string& analog_function(unsigned pin) const { return analog_fn_.at(pin); }  // "" if none
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
  ModelContext& ctx_;
  Clock& clock_;
  RegisterFile regs_;
  std::array<int8_t, 16> drive_{};  // -1: not driven
  std::array<std::optional<double>, 16> ext_volts_{}, drv_volts_{};
  std::array<std::string, 16> analog_fn_{};  // "ADC12_IN9/DAC1_OUT1"; empty: no analog function
  uint16_t last_levels_ = 0;
  int lck_stage_ = 0;
  uint16_t lck_mask_ = 0;
  bool locked_ = false;
  Signal<PinEdge> edges_;
};

}  // namespace stest::stm32l4
