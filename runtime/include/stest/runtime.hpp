#pragma once
// C++ side of the stest runtime that the node (a later task) owns/configures.
//  - runtime_clock()/runtime_bus(): process-global instances used by __stest_mmio_*.
//  - set_access_cost_ns(): virtual ns charged per MMIO call (default 25).
//  - Every __stest_mmio_* call charges the cost, fires due Clock events, accesses,
//    then calls the delivery hook.
//  - set_delivery_hook(): called after every __stest_mmio_* call (IRQ delivery
//    point); null by default.
//  - stest_rt_init() (mmio.h) must be called once at startup.
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
}  // namespace stest
