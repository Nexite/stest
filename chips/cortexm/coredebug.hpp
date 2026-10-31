#pragma once
// CoreDebug block (0xE000EDF0): DHCSR and DEMCR. Core-generic (every Cortex-M4 chip gets it
// through stest::Cortexm). Modelled: DHCSR.C_DEBUGEN (read-only to software) and
// DEMCR.TRCENA (the trace enable ITM and DWT depend on). Everything else (halting, vector
// catch, monitor mode, DCRSR/DCRDR) aborts if software touches it with a non-zero effect.
#include <cstdint>
#include "stest/peripheral.hpp"

namespace stest {

class Dwt;

class CoreDebug : public Peripheral {
 public:
  static constexpr uint32_t kSize = 0x10;  // DHCSR, DCRSR, DCRDR, DEMCR
  static constexpr uint32_t kDhcsrDebugEn = 1u << 0, kDemcrTrcEna = 1u << 24;

  // debugger_attached: power-on state models an attached debugger (C_DEBUGEN=1, TRCENA=1)
  // instead of the bare board (both 0).
  explicit CoreDebug(bool debugger_attached);
  std::string_view name() const override { return "CoreDebug"; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;

  bool trcena() const { return demcr_ & kDemcrTrcEna; }
  bool debug_enabled() const { return dhcsr_ & kDhcsrDebugEn; }
  // DWT counts only while TRCENA is set: it settles its count before TRCENA changes.
  void set_dwt(Dwt* d) { dwt_ = d; }

 private:
  bool attached_;
  uint32_t dhcsr_ = 0, demcr_ = 0;
  Dwt* dwt_ = nullptr;
};

}  // namespace stest
