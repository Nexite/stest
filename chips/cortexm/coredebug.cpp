#include "cortexm/coredebug.hpp"
#include <stdexcept>
#include <string>
#include "cortexm/dwt.hpp"

namespace stest {
namespace {
constexpr uint32_t kDhcsr = 0x0, kDcrsr = 0x4, kDcrdr = 0x8, kDemcr = 0xC;
constexpr uint32_t kDbgKey = 0xA05Fu;
constexpr uint32_t kDhcsrHaltStepMask = 0xEu;  // C_HALT, C_STEP, C_MASKINTS
}  // namespace

CoreDebug::CoreDebug(bool debugger_attached) : attached_(debugger_attached) { reset(); }

void CoreDebug::reset() {
  dhcsr_ = attached_ ? kDhcsrDebugEn : 0;
  demcr_ = attached_ ? kDemcrTrcEna : 0;
}

uint32_t CoreDebug::read(uint32_t off, unsigned width) {
  if ((off & 3u) + width > 4 || (off & ~3u) > kDemcr) throw UnmodelledRegister(off, width, false);
  switch (off & ~3u) {
    case kDhcsr: return lane_extract(dhcsr_, off, width);
    case kDemcr: return lane_extract(demcr_, off, width);
  }
  throw UnmodelledRegister(off, width, false);  // DCRSR (write-only), DCRDR
}

void CoreDebug::write(uint32_t off, unsigned width, uint32_t v) {
  if ((off & 3u) + width > 4 || (off & ~3u) > kDemcr) throw UnmodelledRegister(off, width, true);
  switch (off & ~3u) {
    case kDhcsr: {
      // C_DEBUGEN is writable only by a debugger; software writes are ignored on hardware.
      // With a debugger attached a keyed C_HALT/C_STEP/C_MASKINTS request would halt the core.
      uint32_t w = lane_merge(0, off, width, v);
      if (debug_enabled() && (w >> 16) == kDbgKey && (w & kDhcsrHaltStepMask))
        throw NotModelled("CoreDebug", "halting/stepping the core (DHCSR C_HALT/C_STEP/C_MASKINTS)");
      return;
    }
    case kDemcr: {
      uint32_t w = lane_merge(demcr_, off, width, v);
      if (w & ~kDemcrTrcEna)
        throw std::runtime_error("DEMCR: only TRCENA is modelled (write 0x" + [&] {
          char b[16];
          snprintf(b, sizeof b, "%08x", w);
          return std::string(b);
        }() + ")");
      if ((w ^ demcr_) & kDemcrTrcEna && dwt_) dwt_->settle();
      demcr_ = w;
      return;
    }
  }
  throw UnmodelledRegister(off, width, true);  // DCRSR, DCRDR
}

}  // namespace stest
