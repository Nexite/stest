#pragma once
// STM32L4 (L496) RCC @0x40021000. All RCC_TypeDef registers of STM32L496xx are
// declared with their reset values (RM0351); reserved offsets are strict.
// Oscillators are ideal: setting MSION/HSION/HSEON/PLLON/PLLSAI1ON/PLLSAI2ON (CR),
// LSION (CSR), LSEON (BDCR) or HSI48ON (CRRCR) immediately sets the matching RDY bit,
// clearing it clears RDY. CFGR.SWS mirrors CFGR.SW. CICR writes clear CIFR bits;
// CSR.RMVF clears the reset flags.
#include <string>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Rcc : public Peripheral {
 public:
  Rcc(std::string name, const nlohmann::json& params, Clock& clock);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;

 private:
  void follow();  // ready/status bits follow their enables
  std::string name_;
  RegisterFile regs_;
};

}  // namespace stest::stm32l4
