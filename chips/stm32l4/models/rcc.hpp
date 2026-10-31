#pragma once
// STM32L4 (L496) RCC @0x40021000. All RCC_TypeDef registers of STM32L496xx are
// declared with their reset values (RM0351); reserved offsets are strict.
// Oscillators are ideal: setting MSION/HSION/HSEON/PLLON/PLLSAI1ON/PLLSAI2ON (CR),
// LSION (CSR), LSEON (BDCR) or HSI48ON (CRRCR) immediately sets the matching RDY bit,
// clearing it clears RDY. CFGR.SWS mirrors CFGR.SW. CICR writes clear CIFR bits;
// CSR.RMVF clears the reset flags.
//
// Clock tree (ClockTree), computed from the live registers on every call (RM0351 6.2):
//   SYSCLK by CFGR.SW: MSI (range from CR.MSIRANGE if CR.MSIRGSEL else CSR.MSISRANGE),
//   HSI16, HSE (params.hse_hz, default 8 000 000), or PLLCLK = src / M * N / R from
//   PLLCFGR (PLLSRC MSI/HSI16/HSE). HCLK = SYSCLK / HPRE, PCLK1/2 = HCLK / PPRE1/2.
//   PLLSAI1/2 R = src / M * N / R with N, R from PLLSAIxCFGR and source + M shared with
//   PLLCFGR; 0 unless CR.PLLSAIxON and PLLSAIxCFGR.PLLSAIxREN. The oscillators are ideal
//   (instantly ready), so the RDY bits are not consulted.
// params: {"hse_hz": u64 (default 8000000)}
#include <string>
#include <nlohmann/json.hpp>
#include "stest/clock.hpp"
#include "stest/model_context.hpp"
#include "stest/peripheral.hpp"

namespace stest::stm32l4 {

class Rcc : public Peripheral, public ClockTree {
 public:
  Rcc(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;

  // ClockTree
  uint64_t sysclk_hz() const override;
  uint64_t hclk_hz() const override;
  uint64_t pclk1_hz() const override;
  uint64_t pclk2_hz() const override;
  uint64_t pllsai1r_hz() const override;
  uint64_t pllsai2r_hz() const override;
  uint32_t adcsel() const override;
  bool enabled(uint32_t reg_off, unsigned bit) const override;
  const uint32_t* enable_word(uint32_t reg_off) const override { return regs_.value_ptr(reg_off); }

 private:
  uint64_t msi_hz() const;
  uint64_t pll_input_hz() const;  // PLLSRC / M (shared by PLL, PLLSAI1, PLLSAI2)
  uint64_t hse_hz_;
  void follow();  // ready/status bits follow their enables
  std::string name_;
  RegisterFile regs_;
};

}  // namespace stest::stm32l4
