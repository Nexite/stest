#pragma once
// STM32L4 (L496) PWR @0x40007000: CR1..CR4, SR1, SR2, SCR, PUCRA..PDCRI with RM0351
// reset values. Voltage scaling is instantaneous: CR1.VOS reads back what was
// written and SR2.VOSF always reads 0. SCR is write-only (clears SR1 flags).
#include <string>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Pwr : public Peripheral {
 public:
  Pwr(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override { return regs_.read(off, width); }
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override { regs_.reset(); }

 private:
  std::string name_;
  RegisterFile regs_;
};

}  // namespace stest::stm32l4
