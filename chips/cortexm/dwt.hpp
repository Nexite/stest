#pragma once
// DWT (0xE0001000): CTRL.CYCCNTENA + CYCCNT, LAR/LSR. Core-generic.
//
// CYCCNT = core cycles since it was enabled (or last written), derived from virtual time x
// the core clock (the firmware's SystemCoreClock, via the getter Cortexm holds); it counts
// only while CTRL.CYCCNTENA and DEMCR.TRCENA are both set, wraps at 32 bits and is never
// driven by per-cycle events (it reads differently on each access, like SysTick VAL). The
// count is settled at every DWT access, so a core clock change is applied to the time since
// the previous access.
//
// CTRL reports 4 comparators and all counters present. Only CYCCNTENA is writable; any other
// CTRL feature bit (POSTPRESET, sync/PC-sample/event tracing, ...) aborts. The comparators
// (COMPn/MASKn/FUNCTIONn) are declared but FUNCTION != 0 aborts. The profiling counters
// (CPICNT, ...) are stored but never count (their enables cannot be set); PCSR reads 0.
#include <cstdint>
#include <functional>
#include "cortexm/coredebug.hpp"
#include "stest/clock.hpp"
#include "stest/peripheral.hpp"

namespace stest {

class Dwt : public Peripheral {
 public:
  static constexpr uint32_t kSize = 0x1000;
  static constexpr uint32_t kCtrl = 0x00, kCyccnt = 0x04, kLar = 0xFB0, kLsr = 0xFB4;
  static constexpr uint32_t kCtrlCycCntEna = 1u, kNumComp = 4;
  using ClockFn = std::function<uint32_t()>;  // core clock in Hz (throws if unavailable)

  Dwt(Clock& clock, const CoreDebug& dbg, ClockFn core_hz, bool debugger_attached);
  std::string_view name() const override { return "DWT"; }
  uint32_t read(uint32_t off, unsigned width) override;
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override;

  // Settles CYCCNT to the current virtual time using the current state (CoreDebug calls
  // this before TRCENA changes).
  void settle();

 private:
  bool counting() const { return (ctrl_ & kCtrlCycCntEna) && dbg_.trcena(); }
  uint32_t rd(uint32_t off);
  void wr(uint32_t off, uint32_t v);
  Clock& clock_;
  const CoreDebug& dbg_;
  ClockFn core_hz_;
  bool attached_;
  uint32_t ctrl_ = 0, cyc_ = 0;   // ctrl_: CYCCNTENA only
  uint64_t frac_ = 0;             // sub-cycle remainder in ns*Hz units (< 1e9)
  uint64_t t_last_ = 0;           // virtual time cyc_/frac_ are settled to
  uint32_t cnt_[5] = {};          // CPICNT, EXCCNT, SLEEPCNT, LSUCNT, FOLDCNT (8-bit)
  uint32_t comp_[kNumComp] = {}, mask_[kNumComp] = {};
  bool locked_ = true;
};

}  // namespace stest
