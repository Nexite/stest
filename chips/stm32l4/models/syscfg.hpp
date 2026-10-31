#pragma once
// STM32L4 (L496) SYSCFG @0x40010000 (RM0351 section 9): MEMRMP, CFGR1, EXTICR1-4, SCSR,
// CFGR2, SWPR, SKR, SWPR2 with reset values. Only EXTICR has behaviour here: it selects
// which GPIO port drives each EXTI line 0-15 (4 bits per line, 0=A .. 8=I). The remaining
// registers store what firmware writes (no remap / FPU / SRAM2 erase behaviour is modelled).
// The block is clock-gated (RCC APB2ENR bit 0) on the Bus; model-side reads such as
// exti_port() are not (EXTICR keeps its value while the clock is off).
#include <string>
#include <nlohmann/json.hpp>
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Syscfg : public Peripheral {
 public:
  Syscfg(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override { return regs_.read(off, width); }
  void write(uint32_t off, unsigned width, uint32_t v) override { regs_.write(off, width, v); }
  void reset() override { regs_.reset(); }

  // Port selected for EXTI line `line` (0..15) by EXTICR: 0 = A ... 8 = I (values 9..15 are
  // reserved and match no port). line > 15 -> std::out_of_range.
  unsigned exti_port(unsigned line) const;

 private:
  std::string name_;
  RegisterFile regs_;
};

}  // namespace stest::stm32l4
