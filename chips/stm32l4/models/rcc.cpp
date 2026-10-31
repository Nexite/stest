#include "stm32l4/models/rcc.hpp"

namespace stest::stm32l4 {
namespace {
enum : uint32_t {
  CR = 0x00, ICSCR = 0x04, CFGR = 0x08, PLLCFGR = 0x0C, PLLSAI1CFGR = 0x10, PLLSAI2CFGR = 0x14,
  CIER = 0x18, CIFR = 0x1C, CICR = 0x20, AHB1RSTR = 0x28, AHB2RSTR = 0x2C, AHB3RSTR = 0x30,
  APB1RSTR1 = 0x38, APB1RSTR2 = 0x3C, APB2RSTR = 0x40, AHB1ENR = 0x48, AHB2ENR = 0x4C,
  AHB3ENR = 0x50, APB1ENR1 = 0x58, APB1ENR2 = 0x5C, APB2ENR = 0x60, AHB1SMENR = 0x68,
  AHB2SMENR = 0x6C, AHB3SMENR = 0x70, APB1SMENR1 = 0x78, APB1SMENR2 = 0x7C, APB2SMENR = 0x80,
  CCIPR = 0x88, BDCR = 0x90, CSR = 0x94, CRRCR = 0x98, CCIPR2 = 0x9C,
};
constexpr uint32_t bit(unsigned n) { return 1u << n; }
// CR enable -> ready pairs
constexpr struct { uint32_t on, rdy; } kCrPairs[] = {
    {bit(0), bit(1)},    // MSION -> MSIRDY
    {bit(8), bit(10)},   // HSION -> HSIRDY
    {bit(16), bit(17)},  // HSEON -> HSERDY
    {bit(24), bit(25)},  // PLLON -> PLLRDY
    {bit(26), bit(27)},  // PLLSAI1ON -> PLLSAI1RDY
    {bit(28), bit(29)},  // PLLSAI2ON -> PLLSAI2RDY
};
constexpr uint32_t kCrRw = bit(0) | bit(2) | bit(3) | 0xF0u | bit(8) | bit(9) | bit(11) | bit(16) |
                           bit(18) | bit(19) | bit(24) | bit(26) | bit(28);
constexpr uint32_t kResetFlags = 0xFF000000u;  // CSR FWRSTF..LPWRRSTF
constexpr uint32_t kRmvf = bit(23);
}  // namespace

Rcc::Rcc(std::string name, const nlohmann::json&, Clock&) : name_(std::move(name)) {
  // off, reset, rw mask (RM0351, STM32L496)
  regs_.declare(CR, 0x00000063, kCrRw);                       // MSION|MSIRDY|MSIRANGE=6 (4 MHz)
  regs_.declare(ICSCR, 0x40000000, 0x7F00FF00);               // HSITRIM=64; *CAL factory -> 0
  regs_.declare(CFGR, 0x00000000, 0x7700BFF3);                // SWS (3:2) read-only
  regs_.declare(PLLCFGR, 0x00001000, 0xFF737F73);
  regs_.declare(PLLSAI1CFGR, 0x00001000, 0xFF737F00);
  regs_.declare(PLLSAI2CFGR, 0x00001000, 0xFF037F00);
  regs_.declare(CIER, 0x00000000, 0x000006FF);
  regs_.declare(CIFR, 0x00000000, 0);                         // read-only
  regs_.declare_write_only(CICR);                             // reads 0
  regs_.declare(AHB1RSTR, 0, 0x00031103);
  regs_.declare(AHB2RSTR, 0, 0x000777FF);
  regs_.declare(AHB3RSTR, 0, 0x00000101);
  regs_.declare(APB1RSTR1, 0, 0xF7FEC23F);
  regs_.declare(APB1RSTR2, 0, 0x00000027);
  regs_.declare(APB2RSTR, 0, 0x01677C01);
  regs_.declare(AHB1ENR, 0x00000100, 0x00031103);             // FLASHEN
  regs_.declare(AHB2ENR, 0, 0x000777FF);
  regs_.declare(AHB3ENR, 0, 0x00000101);
  regs_.declare(APB1ENR1, 0, 0xF7FECE3F);
  regs_.declare(APB1ENR2, 0, 0x00000027);
  regs_.declare(APB2ENR, 0, 0x01677C81);
  regs_.declare(AHB1SMENR, 0x00011303, 0x00031303);
  regs_.declare(AHB2SMENR, 0x000532FF, 0x000773FF);
  regs_.declare(AHB3SMENR, 0x00000101, 0x00000101);
  regs_.declare(APB1SMENR1, 0xF2FECA3F, 0xF7FECE3F);
  regs_.declare(APB1SMENR2, 0x00000025, 0x00000027);
  regs_.declare(APB2SMENR, 0x01677C01, 0x01677C01);
  regs_.declare(CCIPR, 0x00000000, 0xFFFFFFFF);
  regs_.declare(BDCR, 0x00000000, 0x0301833D);                // LSERDY(1), LSECSSD(6) read-only
  regs_.declare(CSR, 0x0C000600, 0x00000F01);                 // PINRSTF|BORRSTF, MSISRANGE=6
  regs_.declare(CRRCR, 0x00000000, 0x00000001);               // HSI48ON; HSI48RDY/CAL read-only
  regs_.declare(CCIPR2, 0x00000000, 0x00000003);              // I2C4SEL
}

void Rcc::reset() { regs_.reset(); }

uint32_t Rcc::read(uint32_t off, unsigned width) { return regs_.read(off, width); }

void Rcc::write(uint32_t off, unsigned width, uint32_t v) {
  uint32_t aligned = off & ~3u;
  uint32_t val = lane_merge(0, off, width, v);
  if (aligned == CICR) {
    regs_.write(off, width, v);  // write-only: stores nothing; strictness checks
    regs_.set(CIFR, regs_.get(CIFR) & ~(val & 0x000007FFu));
    return;
  }
  regs_.write(off, width, v);
  if (aligned == CSR && (val & kRmvf)) regs_.set(CSR, regs_.get(CSR) & ~kResetFlags);
  follow();
}

void Rcc::follow() {
  uint32_t cr = regs_.get(CR);
  for (auto p : kCrPairs) cr = (cr & p.on) ? (cr | p.rdy) : (cr & ~p.rdy);
  regs_.set(CR, cr);
  uint32_t cfgr = regs_.get(CFGR);
  regs_.set(CFGR, (cfgr & ~0xCu) | ((cfgr & 3u) << 2));
  auto follow0 = [&](uint32_t off) {  // bit0 enable -> bit1 ready
    uint32_t r = regs_.get(off);
    regs_.set(off, (r & 1u) ? (r | 2u) : (r & ~2u));
  };
  follow0(CSR);    // LSION -> LSIRDY
  follow0(BDCR);   // LSEON -> LSERDY
  follow0(CRRCR);  // HSI48ON -> HSI48RDY
}

}  // namespace stest::stm32l4
