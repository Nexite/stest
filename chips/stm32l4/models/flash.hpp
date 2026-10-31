#pragma once
// STM32L4 (L496) FLASH registers @0x40022000 (RM0351 reset values; OPTR/PCROP/WRP
// hold factory-default option bytes). ACR reads back LATENCY/PRFTEN/ICEN/DCEN/...
// (latency changes are instantaneous). CR.LOCK clears after the KEYR sequence
// 0x45670123, 0xCDEF89AB and is set again by writing LOCK; CR writes are ignored
// while locked (a wrong key leaves it locked). Programming/erase is not modelled:
// setting CR.STRT/OPTSTRT throws (strict).
#include <string>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Flash : public Peripheral {
 public:
  Flash(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override { return regs_.read(off, width); }
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;

 private:
  std::string name_;
  RegisterFile regs_;
  int key_stage_ = 0;
};

}  // namespace stest::stm32l4
