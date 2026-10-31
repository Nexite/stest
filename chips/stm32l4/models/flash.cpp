#include "stm32l4/models/flash.hpp"
#include <stdexcept>

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  ACR = 0x00, PDKEYR = 0x04, KEYR = 0x08, OPTKEYR = 0x0C, SR = 0x10, CR = 0x14, ECCR = 0x18,
  OPTR = 0x20, PCROP1SR = 0x24, PCROP1ER = 0x28, WRP1AR = 0x2C, WRP1BR = 0x30,
  PCROP2SR = 0x44, PCROP2ER = 0x48, WRP2AR = 0x4C, WRP2BR = 0x50,
};
constexpr uint32_t kLock = 1u << 31, kOptLock = 1u << 30;
constexpr uint32_t kStrt = 1u << 16, kOptStrt = 1u << 17;
}  // namespace

Flash::Flash(std::string name, const nlohmann::json&, ModelContext&) : name_(std::move(name)) {
  regs_.declare(ACR, 0x00000600, 0x00007F07);  // ICEN|DCEN; LATENCY, PRFTEN, ICRST, DCRST, RUN_PD, SLEEP_PD
  regs_.declare_write_only(PDKEYR);
  regs_.declare_write_only(KEYR);
  regs_.declare_write_only(OPTKEYR);
  regs_.declare(SR, 0, 0, 0x0000C3FB);         // error/EOP flags are w1c
  regs_.declare(CR, kLock | kOptLock, 0x0F07FFFF);
  regs_.declare(ECCR, 0, 0x01000000, 0xC0000000);
  regs_.declare(OPTR, 0xFFEFF8AA, 0);          // factory option bytes (RDP level 0)
  regs_.declare(PCROP1SR, 0xFFFF0000, 0);
  regs_.declare(PCROP1ER, 0x0FFF0000, 0);
  regs_.declare(WRP1AR, 0xFF00FF00, 0);
  regs_.declare(WRP1BR, 0xFF00FF00, 0);
  regs_.declare(PCROP2SR, 0xFFFF0000, 0);
  regs_.declare(PCROP2ER, 0xFFFF0000, 0);
  regs_.declare(WRP2AR, 0xFF00FF00, 0);
  regs_.declare(WRP2BR, 0xFF00FF00, 0);
}

void Flash::reset() {
  regs_.reset();
  key_stage_ = 0;
}

void Flash::write(uint32_t off, unsigned width, uint32_t v) {
  uint32_t aligned = off & ~3u;
  if (aligned == KEYR) {
    regs_.write(off, width, v);  // strictness checks; value is not stored (write-only)
    if (width != 4) throw std::runtime_error("FLASH_KEYR must be written as a word");
    if (key_stage_ == 0 && v == 0x45670123u) {
      key_stage_ = 1;
    } else if (key_stage_ == 1 && v == 0xCDEF89ABu) {
      key_stage_ = 0;
      regs_.set(CR, regs_.get(CR) & ~kLock);
    } else {
      key_stage_ = 0;  // wrong sequence: stays locked (hardware: bus error until reset)
    }
    return;
  }
  if (aligned == CR) {
    uint32_t cr = regs_.get(CR);
    if (cr & kLock) {
      (void)regs_.read(off, width);
      return;  // locked: ignored
    }
    regs_.write(off, width, v);
    uint32_t val = lane_merge(0, off, width, v);
    if (val & (kStrt | kOptStrt))
      throw NotModelled("FLASH", "program/erase (CR.STRT/OPTSTRT)");
    if (val & kLock) regs_.set(CR, regs_.get(CR) | kLock);
    if (val & kOptLock) regs_.set(CR, regs_.get(CR) | kOptLock);
    return;
  }
  regs_.write(off, width, v);
}

}  // namespace stest::stm32l4
