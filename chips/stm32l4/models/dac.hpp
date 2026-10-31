#pragma once
// STM32L4 (L496) DAC1, RM0351 section 19: two 12-bit channels. Register layout and bits
// follow ST's stm32l496xx.h (DAC_TypeDef, DAC_*). Gated by RCC APB1ENR1 DAC1EN.
//
// Modelled: CR EN1/EN2 and TEN/TSEL (software trigger only, TSELx = 7); SWTRIGR; the data
// holding registers DHR12R1/DHR12L1/DHR8R1, DHR12R2/DHR12L2/DHR8R2, DHR12RD/DHR12LD/DHR8RD
// (views of one 12-bit DHR per channel: a write to any view updates every view); DOR1/DOR2
// (read-only); SR (DMAUDRx rc_w1; CAL_FLAG/BWST read 0); MCR MODEx 0-3; CCR, SHSR1/2, SHHR,
// SHRR stored.
// DOR update (Clock event, model contract): with TENx = 0, DOR <- DHR one APB1 clock after
// the DHR write (RM0351 19.4.5); with TENx = 1 and TSELx = software, one APB1 clock after
// SWTRIGx is written (SWTRIGx then clears). Event times round up to the next ns.
// Output: volts = DOR / 4095 * vref while ENx = 1, else none (not driven). Every change of a
// channel's output emits {"kind":"dac","channel":n,"volts":v|null} and drives its pin
// (params.pins, PA4 / PA5) through Gpio::set_analog_drive when MCR.MODEx connects the pin
// (MODE 0, 1, 2; MODE 3 is on-chip only). An external voltage set on the pin from Python
// wins over the DAC driver (see gpio.hpp).
//
// Strict aborts (std::runtime_error "DAC1 <feature> not modelled (...)"): hardware triggers
// (TENx = 1 with TSELx != 7, "DAC1 hardware triggers not modelled"), wave generation
// (WAVEx != 0), DMA (DMAENx, DMAUDRIEx), calibration mode (CENx), sample-and-hold mode
// (MCR.MODEx >= 4). Hence no interrupt source can be raised; params.irq (TIM6_DAC, shared
// with TIM6) is driven from SR.DMAUDRx & CR.DMAUDRIEx through ModelContext::set_irq_source
// for completeness.
//
// params: {"vref": volts (default 3.3), "irq": n (optional), "pins": ["PA4", "PA5"]
//   (output pins of channel 1 / 2, resolved to "GPIO<port>" models in connect())}
// call("state") -> {"vref", "channels": [{"ch", "enabled", "dhr", "dor", "volts" (null
//   while disabled), "pin" ("PA4", or null when MODE 3 keeps it internal)}]}
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

class Dac : public Peripheral {
 public:
  Dac(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;
  void connect(ModelContext& ctx) override;
  nlohmann::json call(std::string_view op, const nlohmann::json& args) override;

 private:
  struct Chan {
    uint32_t dhr = 0;            // 12-bit holding value
    EventId ev = 0;
    bool has_ev = false;
    std::optional<double> out;   // last reported output
    std::string pin_name;        // "PA4"
    Gpio* gpio = nullptr;
    unsigned pin = 0;
    bool driving = false;
  };
  [[noreturn]] void not_modelled(const std::string& what, uint32_t off, uint32_t v) const;
  void check_write(uint32_t off, uint32_t v) const;
  void sync_dhr_views();
  void schedule_transfer(unsigned ch);
  void transfer(unsigned ch);
  std::optional<double> output(unsigned ch) const;
  bool pin_connected(unsigned ch) const;
  void update_outputs();
  void update_irq();

  std::string name_;
  ModelContext& ctx_;
  Clock& clock_;
  const ClockTree* clocks_ = nullptr;
  double vref_;
  int irq_ = -1;
  char tag_ = 0;
  bool level_ = false;
  std::array<Chan, 2> ch_{};
  RegisterFile regs_;
};

}  // namespace stest::stm32l4
