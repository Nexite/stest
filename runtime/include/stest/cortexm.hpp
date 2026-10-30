#pragma once
// Cortex-M core model: NVIC (0xE000E100), SCB (0xE000ED00) and SysTick (0xE000E010)
// register blocks mapped on a Bus, plus the exception dispatcher that calls the
// firmware's real handlers on the host thread.
//
// Exceptions are identified by CMSIS IRQn (external IRQs >= 0, SVCall -5, PendSV -2,
// SysTick -1); the handler table is indexed by exception number = IRQn + 16.
//
// Dispatch model (single host thread, synchronous, re-entrant):
//  - deliver() is a delivery point. It repeatedly takes the highest-priority pending,
//    enabled exception whose *group* priority (per AIRCR.PRIGROUP) is strictly higher
//    (numerically lower) than the current execution priority, marks it active, and
//    calls its handler. A delivery point inside a handler (any MMIO access, via the
//    runtime delivery hook) can therefore preempt it; equal group priority waits.
//  - Execution priority = min(group priority of active exceptions, BASEPRI group);
//    PRIMASK/FAULTMASK boost it to 0 / -1 (nothing configurable preempts).
//  - An exception never preempts itself (it is active, so its group priority is
//    already <= the execution priority); re-pending inside its handler tail-chains.
//
// SysTick counts down at the core clock (CTRL.CLKSOURCE=1) or core clock / external
// divider (CLKSOURCE=0; divider defaults to 1, set_systick_external_divider()). The
// core clock comes from a registered getter (firmware's CMSIS SystemCoreClock); it
// is re-read on every SysTick register access and wrap. Wraps are Clock events.
//
// Unmodelled offsets in any block throw UnmodelledRegister (strict mode -> abort).
// Unsupported actions (SYSRESETREQ, NMIPENDSET) throw std::runtime_error and invalid
// IRQn (NMI/faults, out of range) std::out_of_range; the C boundary (mmio access,
// delivery hook, cortexm_c.h API) turns any exception into stest_rt_abort (exit 70).
#include <cstddef>
#include <cstdint>
#include <memory>
#include "stest/bus.hpp"
#include "stest/clock.hpp"

namespace stest {

using IrqHandler = void (*)(void);
using CoreClockGetter = uint32_t (*)(void);

class Cortexm {
 public:
  static constexpr uint64_t kSysTickBase = 0xE000E010, kSysTickSize = 0x10;
  static constexpr uint64_t kNvicBase = 0xE000E100, kNvicSize = 0x3F0;  // ISER..IPR[239]
  static constexpr uint64_t kScbBase = 0xE000ED00, kScbSize = 0x90;     // CPUID..CPACR
  static constexpr int kSVCall_IRQn = -5, kPendSV_IRQn = -2, kSysTick_IRQn = -1;

  // irq_table[IRQn + 16] = handler (null entries abort if ever delivered).
  // n_irqs = number of table entries (16 + external IRQ count, <= 16 + 240).
  // Maps the three core blocks on `bus`; unmaps them in the destructor.
  Cortexm(Bus& bus, Clock& clock, unsigned prio_bits, const IrqHandler* irq_table, size_t n_irqs);
  ~Cortexm();
  Cortexm(const Cortexm&) = delete;
  Cortexm& operator=(const Cortexm&) = delete;

  // Peripherals call these. Valid irqn: external [0, n_irqs-16), SVCall, PendSV, SysTick.
  void set_pending(int irqn);
  void clear_pending(int irqn);
  bool pending(int irqn) const;
  bool active(int irqn) const;
  bool enabled(int irqn) const;  // system exceptions are always enabled
  uint8_t priority(int irqn) const;

  void deliver();  // delivery point: run eligible pending handlers, nested by priority

  // Lowering a mask (PRIMASK/FAULTMASK cleared, BASEPRI lowered) is itself a
  // delivery point, like the instruction-boundary take on hardware.
  void set_primask(bool on);
  bool primask() const;
  void set_basepri(uint32_t v);
  void set_basepri_max(uint32_t v);  // only raises (BASEPRI_MAX semantics)
  uint32_t basepri() const;
  void set_faultmask(bool on);
  bool faultmask() const;
  uint32_t ipsr() const;  // exception number being handled, 0 in thread mode

  // If an exception is pending and eligible, deliver it and return. Otherwise advance
  // the Clock to min(next_event, quantum_end), fire due events, then deliver().
  // "Pending and eligible" ignores PRIMASK, as on hardware: with interrupts disabled
  // wfi returns immediately without running the handler. Throws std::runtime_error
  // if nothing could ever wake the core (no event and no quantum end).
  void wfi();
  void set_quantum_end(uint64_t t_ns);
  uint64_t quantum_end() const;

  void set_core_clock_getter(CoreClockGetter g);
  void set_systick_external_divider(uint32_t div);  // >= 1; default 1

  // Route the C API (stest/cortexm_c.h) and the runtime delivery hook to this core.
  // The destructor uninstalls it.
  void install();
  static Cortexm* installed();

  struct Impl;  // dispatcher state + NVIC/SCB/SysTick register models (cortexm.cpp)

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace stest
