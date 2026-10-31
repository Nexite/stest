#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include "stest/peripheral.hpp"

namespace stest {

struct UnmodelledAccess : std::runtime_error {
  uint64_t addr; unsigned width; bool is_write;
  std::string peripheral;  // empty if no peripheral owns the address
  bool read_only;          // a write to a read-only register (the peripheral threw ReadOnlyRegister)
  UnmodelledAccess(uint64_t a, unsigned w, bool wr, std::string p = {}, bool ro = false)
      : std::runtime_error(ro ? "write to read-only register" : "unmodelled access"), addr(a),
        width(w), is_write(wr), peripheral(std::move(p)), read_only(ro) {}
};

class Bus {
 public:
  // Throws std::invalid_argument on zero size, overlap, or a range outside both MMIO windows
  // (see mmio_windows.hpp): firmware accesses there never reach the Bus.
  void map(uint64_t base, uint32_t size, Peripheral* p);
  void unmap(Peripheral* p);  // removes every mapping of p (no-op if none)
  // width in bytes (1,2,4). Throws UnmodelledAccess if unmapped or the peripheral
  // throws UnmodelledRegister (then .peripheral is set; .read_only for ReadOnlyRegister).
  uint32_t read(uint64_t addr, unsigned width);
  void write(uint64_t addr, unsigned width, uint32_t v);
  void reset_all();

 private:
  struct Mapping { uint64_t base; uint32_t size; Peripheral* p; };
  const Mapping* find(uint64_t addr, unsigned width) const;
  std::vector<Mapping> maps_;  // sorted by base
};

}  // namespace stest
