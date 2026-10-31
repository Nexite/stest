#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

// An access to a gated peripheral whose clock is disabled (strict abort). what() is
// "access to <NAME> while its clock is disabled (<what passed to set_gate>)".
struct ClockDisabled : std::runtime_error {
  uint64_t addr; unsigned width; bool is_write;
  ClockDisabled(const std::string& msg, uint64_t a, unsigned w, bool wr)
      : std::runtime_error(msg), addr(a), width(w), is_write(wr) {}
};

class Bus {
 public:
  // Throws std::invalid_argument on zero size, overlap, or a range outside both MMIO windows
  // (see mmio_windows.hpp): firmware accesses there never reach the Bus.
  void map(uint64_t base, uint32_t size, Peripheral* p);
  void unmap(Peripheral* p);  // removes every mapping (and the gate) of p (no-op if none)
  // Clock-enable gate: every read/write of p (all its mappings, present and future) first
  // calls clocked(); false throws ClockDisabled. `what` names the enable bit, e.g.
  // "RCC AHB2ENR bit 1". Replaces any previous gate. p not mapped -> std::invalid_argument.
  // Cost on the access path: one predicted branch when ungated, one indirect call when gated.
  void set_gate(Peripheral* p, std::function<bool()> clocked, std::string what);
  // Same, gated on (*word & mask) != 0, read live on every access (one load, no call); the
  // form the generated chip init uses, with word = ClockTree::enable_word(<RCC reg>).
  void set_gate(Peripheral* p, const uint32_t* word, uint32_t mask, std::string what);
  // width in bytes (1,2,4). Throws UnmodelledAccess if unmapped or the peripheral
  // throws UnmodelledRegister (then .peripheral is set; .read_only for ReadOnlyRegister).
  // Throws ClockDisabled for a gated peripheral whose clock is off (the model is not called).
  uint32_t read(uint64_t addr, unsigned width);
  void write(uint64_t addr, unsigned width, uint32_t v);
  void reset_all();

 private:
  struct Gate {
    const uint32_t* word;  // non-null: word form
    uint32_t mask;
    std::function<bool()> clocked;
    std::string what;
    bool open() const { return word ? (*word & mask) != 0 : clocked(); }
  };
  void install_gate(Peripheral* p, Gate g);
  struct Mapping { uint64_t base; uint32_t size; Peripheral* p; const Gate* gate; };
  const Mapping* find(uint64_t addr, unsigned width) const;
  [[noreturn]] static void gated(const Mapping& m, uint64_t addr, unsigned width, bool is_write);
  std::vector<Mapping> maps_;  // sorted by base
  std::map<Peripheral*, std::unique_ptr<Gate>> gates_;
};

}  // namespace stest
