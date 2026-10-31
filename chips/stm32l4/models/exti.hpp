#pragma once
// STM32L4 (L496) EXTI @0x40010400 (RM0351 section 13): IMR1 EMR1 RTSR1 FTSR1 SWIER1 PR1 and
// IMR2 EMR2 RTSR2 FTSR2 SWIER2 PR2 with reset values (IMR1 0xFF820000, IMR2 0x00000187).
// Only lines 0-15 have a source in this model: every GPIO port's Gpio::edges() Signal is
// subscribed in connect(); an edge on pin N of port P is line N's event iff
// SYSCFG EXTICR selects P for line N, and it sets PR bit N if RTSR (rising) / FTSR (falling)
// allows it. PR latches whether or not the line is masked; each NVIC IRQ's level is driven
// (ModelContext::set_irq_source, one source per line, so a vector shared with another
// model stays the OR of all sources) as "any line routed to it has PR & IMR set": it pends when that
// becomes true (on the edge, or when IMR is later set) and, as on hardware, pends again on
// handler return if the ISR left PR set. A second edge while PR is still set is not a new
// event; clearing PR re-arms. PR is write-1-to-clear. A 0->1 write to SWIER sets PR (and
// SWIER clears with PR, as on hardware). Events (EMR) are stored but have no consumer.
// reset() clears PR and so drops every line's IRQ source.
//
// params: {"irq": [16 NVIC IRQ numbers for lines 0..15]}  (required; from the part's TOML)
#include <array>
#include <string>
#include <nlohmann/json.hpp>
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"
#include "stm32l4/models/gpio.hpp"
#include "stm32l4/models/syscfg.hpp"

namespace stest::stm32l4 {

class Exti : public Peripheral {
 public:
  Exti(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override { return regs_.read(off, width); }
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;
  void connect(ModelContext& ctx) override;

 private:
  void on_edge(const PinEdge& e);
  uint32_t active() const;                 // PR1 & IMR1 for lines 0-15
  void pend_new(uint32_t active_before);   // update the NVIC level of each routed IRQ
  std::string name_;
  ModelContext& ctx_;
  std::array<char, 16> line_tag_{};        // set_irq_source identity of each line
  RegisterFile regs_;
  std::array<int, 16> irq_{};
  Syscfg* syscfg_ = nullptr;
};

}  // namespace stest::stm32l4
