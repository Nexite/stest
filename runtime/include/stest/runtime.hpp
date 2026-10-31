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

// Poll fast-forward. A "poll streak" is a run of consecutive loads (in or out of the
// peripheral windows) with no store and no exception taken, in which every load's
// (addr, width, value) is one of at most 8 distinct tuples (re-reading an address with
// a different value, or a 9th tuple, starts a new streak seeded by that load). Once a
// streak reaches the threshold, the clock jumps to min(next clock event, quantum end)
// and then behaves as a charge reaching that time would (due events fire, quantum hook
// at the quantum end, delivery point); the streak restarts. Spinning would only have
// re-read the same values until then, so the observable state is the same, with times
// shifted by less than one access cost. Firmware that counts iterations of a loop
// whose loads all return constant values (no store) is compressed too.
// Model contract: see Peripheral (peripheral.hpp).
void set_poll_skip_threshold(uint32_t loads);  // default 64; 0 disables
uint32_t poll_skip_threshold();
// Ends the current poll streak. Called by the core when it takes an exception and by
// the node after a quantum handoff (the loop thread may have changed state).
void poll_streak_break();
}  // namespace stest
