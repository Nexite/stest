#pragma once
#include <cstdint>

namespace stest {
// Address windows the runtime owns: every instrumented volatile access inside one is routed
// to the Bus (mmio.cpp); Bus::map rejects ranges outside them.
constexpr uint64_t kPeriphBase = 0x40000000u, kPeriphSize = 0x20000000u;  // [0x40000000,0x60000000)
constexpr uint64_t kCoreBase = 0xE0000000u, kCoreSize = 0x100000u;        // [0xE0000000,0xE0100000)

constexpr bool in_mmio_window(uint64_t a) {
  return (a - kPeriphBase) < kPeriphSize || (a - kCoreBase) < kCoreSize;
}
}  // namespace stest
