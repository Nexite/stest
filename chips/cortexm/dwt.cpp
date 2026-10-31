#include "cortexm/dwt.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace stest {
namespace {
constexpr uint32_t kCpicnt = 0x08, kPcsr = 0x1C, kComp0 = 0x20;  // COMPn = 0x20 + 0x10*n, MASKn +4, FUNCTIONn +8
constexpr uint32_t kCtrlRoMask = 0xFF800000u;                    // NUMCOMP, NO*; not writable
constexpr uint32_t kLarKey = 0xC5ACCE55u;

std::string hex32(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}
}  // namespace

Dwt::Dwt(Clock& clock, const CoreDebug& dbg, ClockFn core_hz, bool debugger_attached)
    : clock_(clock), dbg_(dbg), core_hz_(std::move(core_hz)), attached_(debugger_attached) {
  reset();
}

void Dwt::reset() {
  ctrl_ = 0;
  cyc_ = 0;
  frac_ = 0;
  t_last_ = clock_.now_ns();
  for (auto& c : cnt_) c = 0;
  for (uint32_t i = 0; i < kNumComp; ++i) comp_[i] = mask_[i] = 0;
  locked_ = !attached_;
}

void Dwt::settle() {
  uint64_t now = clock_.now_ns();
  if (counting() && now > t_last_) {
    uint64_t hz = core_hz_();
    if (hz == 0) throw std::runtime_error("DWT CYCCNT running but the core clock is 0 Hz (SystemCoreClock not set?)");
    unsigned __int128 tot = (unsigned __int128)frac_ + (unsigned __int128)(now - t_last_) * hz;
    cyc_ += static_cast<uint32_t>(tot / 1'000'000'000u);  // wraps at 32 bits
    frac_ = static_cast<uint64_t>(tot % 1'000'000'000u);
  }
  t_last_ = now;
}

uint32_t Dwt::rd(uint32_t off) {
  settle();
  if (off == kCtrl) return (kNumComp << 28) | ctrl_;  // all counters/trace present
  if (off == kCyccnt) return cyc_;
  if (off >= kCpicnt && off < kPcsr) return cnt_[(off - kCpicnt) / 4];
  if (off == kPcsr) return 0;
  if (off >= kComp0 && off < kComp0 + 0x10 * kNumComp) {
    uint32_t n = (off - kComp0) / 0x10;
    switch ((off - kComp0) % 0x10) {
      case 0: return comp_[n];
      case 4: return mask_[n];
      case 8: return 0;  // FUNCTION: comparators are never enabled (MATCHED always 0)
    }
  }
  if (off == kLsr) return locked_ ? 3u : 1u;
  throw UnmodelledRegister(off, 4, false);
}

void Dwt::wr(uint32_t off, uint32_t v) {
  settle();
  if (off == kCtrl) {
    uint32_t w = v & ~kCtrlRoMask & ~kCtrlCycCntEna;
    if (w)
      throw std::runtime_error("DWT CTRL: only CYCCNTENA is modelled (write " + hex32(v) + ")");
    ctrl_ = v & kCtrlCycCntEna;
    return;
  }
  if (off == kCyccnt) { cyc_ = v; frac_ = 0; return; }
  if (off >= kCpicnt && off < kPcsr) { cnt_[(off - kCpicnt) / 4] = v & 0xFFu; return; }
  if (off == kPcsr) throw ReadOnlyRegister(off, 4);
  if (off >= kComp0 && off < kComp0 + 0x10 * kNumComp) {
    uint32_t n = (off - kComp0) / 0x10;
    switch ((off - kComp0) % 0x10) {
      case 0: comp_[n] = v; return;
      case 4: mask_[n] = v & 0x1Fu; return;
      case 8:
        if (v & 0xFu)
          throw std::runtime_error("DWT comparator " + std::to_string(n) + " enabled (FUNCTION" +
                                   std::to_string(n) + " = " + hex32(v) + "): comparators are not modelled");
        return;  // FUNCTION == 0: disabled; other fields have no effect
    }
  }
  if (off == kLar) { locked_ = v != kLarKey; return; }
  throw UnmodelledRegister(off, 4, true);
}

uint32_t Dwt::read(uint32_t off, unsigned width) {
  if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, false);
  return lane_extract(rd(off & ~3u), off, width);
}

void Dwt::write(uint32_t off, unsigned width, uint32_t v) {
  if ((off & 3u) + width > 4) throw UnmodelledRegister(off, width, true);
  uint32_t base = off & ~3u;
  uint32_t cur = 0;
  if (width < 4 && base != kLar) cur = rd(base);  // read-modify-write of sub-word access
  wr(base, width >= 4 ? v : lane_merge(cur, off, width, v));
}

}  // namespace stest
