#pragma once
// C++ side of the stest runtime that the node (a later task) owns/configures.
//  - runtime_clock()/runtime_bus(): process-global instances used by __stest_mmio_*.
//  - set_access_cost_ns(): virtual ns charged per MMIO call (default 25).
//  - Every __stest_mmio_* call charges the cost, fires due Clock events, accesses,
//    then calls the delivery hook.
//  - set_delivery_hook(): called after every __stest_mmio_* call (IRQ delivery
//    point); null by default.
//  - stest_rt_init() (mmio.h) must be called once at startup.
//  - Quantum end: when a charge brings the clock to >= quantum_end(), the quantum
//    hook (if set) is called right there, on the calling (firmware) thread, before
//    the access proceeds. The node uses it to hand the baton back to its loop
//    thread; the hook returns once the loop has set a later quantum end.
#include <cstdint>
#include <type_traits>
#include "stest/bus.hpp"
#include "stest/clock.hpp"

namespace stest {
Clock& runtime_clock();
Bus& runtime_bus();
uint64_t access_cost_ns();
void set_access_cost_ns(uint64_t ns);
using DeliveryHook = void (*)();
void set_delivery_hook(DeliveryHook h);
using QuantumHook = void (*)();
void set_quantum_end(uint64_t t_ns);  // default UINT64_MAX (never)
uint64_t quantum_end();
void set_quantum_hook(QuantumHook h);  // null by default

// Abort hook: stest_rt_abort() prints "stest: <msg>" to stderr, then calls the hook
// (once) with that same "stest: <msg>" text, then _exit(70)s. Runs on whichever thread
// aborted (usually the firmware thread, with the loop thread parked), so the hook must
// not depend on the loop thread. The control channel uses it to send ERROR.
using AbortHook = void (*)(const char* msg);
void set_abort_hook(AbortHook h);  // null by default

// Poll fast-forward. A "poll streak" is a run of consecutive loads (in or out of the
// peripheral windows) with no store and no exception taken, in which every load's
// (addr, width, value) is one of at most 8 distinct tuples (re-reading an address with
// a different value, or a 9th tuple, starts a new streak seeded by that load). Once a
// streak reaches the threshold, the clock jumps to min(next clock event, quantum end)
// and then behaves as a charge reaching that time would (due events fire, quantum hook
// at the quantum end, delivery point); the streak restarts. Spinning would only have
// re-read the same values until then, so the observable state is the same, with times
// shifted by less than one access cost. For a loop that polls a value this is exact.
// A loop that counts iterations in a NON-volatile local while re-reading constant
// volatile values (e.g. `while (--n && !(REG & FLAG));` with FLAG never set) jumps once
// per `threshold` iterations, so its virtual time is DILATED (can be hundreds of times
// longer than on hardware) and, when no clock event is pending, depends on the quantum
// size. Disable with threshold 0 (node env STEST_POLL_SKIP=0).
// Model contract: see Peripheral (peripheral.hpp).
void set_poll_skip_threshold(uint32_t loads);  // default 64; 0 disables
uint32_t poll_skip_threshold();
// Ends the current poll streak. Called by the core when it takes an exception and by
// the node after a quantum handoff (the loop thread may have changed state).
void poll_streak_break();

// HAL-shim access (MODELS <TYPE>=hal; the C++ side of stest/hal_api.h). One register access
// that ST's replaced driver function would have made, performed by calling the model directly
// on the firmware's behalf: exactly like an MMIO access at `addr` (charge the access cost, due
// events fire, quantum end; Bus::require_clock mapping/clock-gate checks; op(p, ctx), with p
// the peripheral mapped at addr, applies the model-side effect and returns the loaded value; then the IRQ delivery point; a store ends
// the poll streak, a load feeds it with (addr, width, value)), but without the Bus's hal-mode
// warning. Any error aborts strictly like MMIO (unmodelled, read-only, clock disabled).
// So a shim that accounts for the same accesses as ST's code gives byte-identical event logs.
using ShimOp = uint32_t (*)(Peripheral& p, void* ctx);
uint32_t shim_access(uint64_t addr, unsigned width, bool is_write, ShimOp op, void* ctx);
template <class F>  // f: (Peripheral&) -> uint32_t (loads) or (Peripheral&) -> void (stores)
uint32_t shim_access(uint64_t addr, unsigned width, bool is_write, F&& f) {
  using Fn = std::remove_reference_t<F>;
  return shim_access(addr, width, is_write, [](Peripheral& p, void* c) -> uint32_t {
    Fn& fn = *static_cast<Fn*>(c);
    if constexpr (std::is_void_v<decltype(fn(p))>) {
      fn(p);
      return 0;
    } else {
      return static_cast<uint32_t>(fn(p));
    }
  }, &f);
}
}  // namespace stest
