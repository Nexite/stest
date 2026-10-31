#include "stm32l4/models/syscfg.hpp"
#include <stdexcept>

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  MEMRMP = 0x00, CFGR1 = 0x04, EXTICR1 = 0x08, SCSR = 0x18, CFGR2 = 0x1C, SWPR = 0x20,
  SKR = 0x24, SWPR2 = 0x28,
};
}

Syscfg::Syscfg(std::string name, const nlohmann::json&, ModelContext&) : name_(std::move(name)) {
  regs_.declare(MEMRMP, 0x00000000, 0x00000107);  // MEM_MODE[2:0], FB_MODE
  regs_.declare(CFGR1, 0x7C000001, 0xFC7F0301);   // FPU_IE[5:0], I2Cx_FMP, ANASWVDD, BOOSTEN, FWDIS
  for (uint32_t i = 0; i < 4; ++i) regs_.declare(EXTICR1 + 4 * i, 0, 0x0000FFFF);
  regs_.declare(SCSR, 0x00000000, 0x00000001);    // SRAM2ER; SRAM2BSY (bit 1) never busy
  regs_.declare(CFGR2, 0x00000000, 0x0000000F, 0x00000100);  // CLL SPL PVDL ECCL; SPF is w1c
  regs_.declare(SWPR, 0x00000000, 0xFFFFFFFF);    // SRAM2 pages 0-31 write protection
  regs_.declare(SKR, 0x00000000, 0x000000FF);
  regs_.declare(SWPR2, 0x00000000, 0xFFFFFFFF);   // SRAM2 pages 32-63 (L496: 64 KB)
}

unsigned Syscfg::exti_port(unsigned line) const {
  if (line > 15) throw std::out_of_range("syscfg: EXTI line out of range 0..15");
  return (regs_.get(EXTICR1 + 4 * (line / 4)) >> ((line % 4) * 4)) & 0xFu;
}

}  // namespace stest::stm32l4
