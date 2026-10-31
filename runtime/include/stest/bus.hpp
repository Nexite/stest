#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
// "access to <NAME> while its clock is disabled (<what passed to set_gate>) (<read|write><bits> 0x%08x)".
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
  // HAL mode (stest_add_firmware MODELS <TYPE>=hal; the generated chip init calls this for
  // every peripheral of a hal-mode type): `p` is driven by a HAL shim through the model's
  // C++ API (stest/hal_api.h), not by firmware MMIO. A firmware access to it through read()/
  // write() still works but prints once per (peripheral, register word), on stderr:
  //   stest: warning: firmware accessed GPIOA ODR directly while GPIO is in hal mode
  // (register named by Peripheral::reg_name, else "+0x018"). `type` is the MODELS type name.
  // Cost on the access path: one predicted branch when not hal-owned. p not mapped ->
  // std::invalid_argument.
  void set_hal_owned(Peripheral* p, std::string type);
  // The peripheral mapped at [addr, addr+width), after the same mapping and clock-gate checks
  // read()/write() make (UnmodelledAccess / ClockDisabled), without accessing the model and
  // without the hal-mode warning: for HAL-shim accesses performed on the firmware's behalf.
  Peripheral* require_clock(uint64_t addr, unsigned width, bool is_write) const;
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
  struct HalOwned {
    std::string type;
    std::set<uint32_t> warned;  // register word offsets already reported
  };
  void install_gate(Peripheral* p, Gate g);
  struct Mapping { uint64_t base; uint32_t size; Peripheral* p; const Gate* gate; HalOwned* hal; };
  const Mapping* find(uint64_t addr, unsigned width) const;
  [[noreturn]] static void gated(const Mapping& m, uint64_t addr, unsigned width, bool is_write);
  static void hal_access(const Mapping& m, uint64_t addr);
  std::vector<Mapping> maps_;  // sorted by base
  std::map<Peripheral*, std::unique_ptr<Gate>> gates_;
  std::map<Peripheral*, std::unique_ptr<HalOwned>> hal_;
};

}  // namespace stest
