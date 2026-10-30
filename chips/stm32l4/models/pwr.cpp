#include "stm32l4/models/pwr.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t { CR1 = 0x00, CR2 = 0x04, CR3 = 0x08, CR4 = 0x0C, SR1 = 0x10, SR2 = 0x14, SCR = 0x18 };
}

Pwr::Pwr(std::string name, const nlohmann::json&, Clock&) : name_(std::move(name)) {
  regs_.declare(CR1, 0x00000200, 0x00004707);  // VOS=01 (range 1); LPR, DBP, LPMS
  regs_.declare(CR2, 0x00000000, 0x000006FF);
  regs_.declare(CR3, 0x00008000, 0x0000851F);  // EIWUL
  regs_.declare(CR4, 0x00000000, 0x0000031F);
  regs_.declare(SR1, 0x00000000, 0);           // read-only
  regs_.declare(SR2, 0x00000000, 0);           // read-only; VOSF/REGLPF never set
  regs_.declare(SCR, 0x00000000, 0);           // write-only, reads 0
  for (uint32_t off = 0x20; off <= 0x64; off += 4) regs_.declare(off, 0, 0x0000FFFF);  // PUCRx/PDCRx A..I
}

void Pwr::write(uint32_t off, unsigned width, uint32_t v) {
  if ((off & ~3u) == SCR) {  // clear SR1 flags (CWUFx, CSBF)
    regs_.write(off, width, v);  // write-only, stores nothing (rw=0); strictness checks
    uint32_t val = (width == 4) ? v : (v << ((off & 3u) * 8u));
    regs_.set(SR1, regs_.get(SR1) & ~(val & 0x0000011Fu));
    return;
  }
  regs_.write(off, width, v);
}

}  // namespace stest::stm32l4
