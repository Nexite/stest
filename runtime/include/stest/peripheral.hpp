#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace stest {

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
  virtual nlohmann::json call(std::string_view op, const nlohmann::json& args);  // throws UnknownOp
};

// 32-bit register store with reset values, rw/w1c masks. Sub-word accesses
// operate on the byte lanes of the containing aligned register.
class RegisterFile {
 public:
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

 private:
  struct Reg { uint32_t value, reset, rw, w1c; bool write_only; };
  Reg& find(uint32_t off, unsigned width, bool is_write);
  const Reg& find(uint32_t off, unsigned width, bool is_write) const;
  std::map<uint32_t, Reg> regs_;
};

}  // namespace stest
