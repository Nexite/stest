#pragma once
#include <cstdint>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>

namespace stest {

class ModelContext;  // model_context.hpp

struct UnmodelledRegister : std::runtime_error {
  uint32_t off; unsigned width; bool is_write;
  UnmodelledRegister(uint32_t o, unsigned w, bool wr)
      : std::runtime_error("unmodelled register"), off(o), width(w), is_write(wr) {}
};

// A write to a register with no writable and no W1C bit (e.g. an input/status register).
// Strict mode: the access aborts like an unmodelled one. (Writes to registers that merely
// contain some read-only bits keep hardware semantics: those bits ignore the write.)
struct ReadOnlyRegister : UnmodelledRegister {
  ReadOnlyRegister(uint32_t o, unsigned w) : UnmodelledRegister(o, w, true) {}
  const char* what() const noexcept override { return "write to read-only register"; }
};

// A model was asked for a feature it does not implement (strict abort). `periph` is the
// instance name ("TIM2", "ADC1"), `feature` names the feature and the register bits that
// selected it ("DMA requests (DIER.UDE/CCxDE/COMDE/TDE)"). what() is
// "<periph>: <feature> not modelled"; at an MMIO access the runtime (mmio.cpp) prints
//   stest: <periph>: <feature> not modelled (<read|write><bits> 0x<addr>[ = 0x<value>])
struct NotModelled : std::runtime_error {
  std::string periph, feature;
  NotModelled(std::string p, std::string f)
      : std::runtime_error(p + ": " + f + " not modelled"), periph(std::move(p)), feature(std::move(f)) {}
};

struct UnknownOp : std::runtime_error {
  explicit UnknownOp(std::string_view op)
      : std::runtime_error("unknown op: " + std::string(op)) {}
};

// Model contract (poll fast-forward, see runtime.hpp): a register value may change
// over time only through a scheduled Clock event or by reading differently on each
// access (like SysTick VAL). Lazily-computed "becomes ready at time T" state with no
// Clock event at T would be skipped over: a firmware loop polling it would jump past T
// to the next event or quantum end. Schedule an event at T instead.
struct Peripheral {  // one instance per mapped block
  virtual ~Peripheral() = default;
  virtual std::string_view name() const = 0;
  virtual uint32_t read(uint32_t off, unsigned width) = 0;  // width in bytes: 1,2,4
  virtual void write(uint32_t off, unsigned width, uint32_t v) = 0;
  virtual void reset() {}
  // Called once after every model of the chip exists and the clock tree is registered
  // (generated init, see model_context.hpp): look up peers, subscribe to their Signals.
  virtual void connect(ModelContext&) {}
  virtual nlohmann::json call(std::string_view op, const nlohmann::json& args);  // throws UnknownOp
  // Reference-manual name of the register at word offset `off` ("ODR"), or empty if the
  // model does not name it. Used in diagnostics only (e.g. the hal-mode warning, Bus).
  virtual std::string_view reg_name(uint32_t /*off*/) const { return {}; }
};

// Sub-word access helpers shared by every model. `off` is taken modulo 4; width is 1, 2 or 4
// bytes and the lane must not cross the word (callers check that).
inline uint32_t lane_mask(uint32_t off, unsigned width) {  // e.g. (2,2) -> 0xFFFF0000
  uint32_t m = width >= 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u);
  return m << ((off & 3u) * 8);
}
inline uint32_t lane_extract(uint32_t word, uint32_t off, unsigned width) {  // right-aligned
  return (word & lane_mask(off, width)) >> ((off & 3u) * 8);
}
inline uint32_t lane_merge(uint32_t word, uint32_t off, unsigned width, uint32_t v) {
  uint32_t m = lane_mask(off, width);
  return (word & ~m) | ((v << ((off & 3u) * 8)) & m);
}

// 32-bit register store with reset values, rw/w1c masks. Sub-word accesses
// operate on the byte lanes of the containing aligned register.
class RegisterFile {
 public:
  // Registers live in a flat array indexed by off/4: off must be word aligned and below
  // kMaxBlock, else std::invalid_argument.
  static constexpr uint32_t kMaxBlock = 0x400;
  static constexpr uint32_t kMaxRegs = kMaxBlock / 4;
  // rw_mask: bits software may write. w1c_mask: bits cleared by writing 1.
  // rw_mask == w1c_mask == 0 declares a read-only register: write() throws ReadOnlyRegister.
  void declare(uint32_t off, uint32_t reset, uint32_t rw_mask, uint32_t w1c_mask = 0);
  // A write-only / model-handled register (BSRR, KEYR, ...): write() performs the
  // strictness checks and stores nothing; the model applies the write's effect itself.
  void declare_write_only(uint32_t off, uint32_t reset = 0);
  uint32_t read(uint32_t off, unsigned width) const;   // throws UnmodelledRegister
  // throws UnmodelledRegister, or ReadOnlyRegister (a subclass) for a read-only register
  void write(uint32_t off, unsigned width, uint32_t v);
  void reset();                                          // all regs to reset values
  // Model-side access (bypasses masks). Throws UnmodelledRegister if undeclared.
  uint32_t get(uint32_t off) const;
  void set(uint32_t off, uint32_t v);
  // Stable address of a declared register's current value (valid for the RegisterFile's
  // lifetime), for cheap live reads such as Bus clock gates. Throws UnmodelledRegister.
  const uint32_t* value_ptr(uint32_t off) const;

 private:
  struct Reg { uint32_t value = 0, reset = 0, rw = 0, w1c = 0; bool declared = false, write_only = false; };
  Reg& find(uint32_t off, unsigned width, bool is_write);
  const Reg& find(uint32_t off, unsigned width, bool is_write) const;
  std::array<Reg, kMaxRegs> regs_{};
};

}  // namespace stest
