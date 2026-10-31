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
  void declare(uint32_t off, uint32_t reset, uint32_t rw_mask, uint32_t w1c_mask = 0);
  uint32_t read(uint32_t off, unsigned width) const;   // throws UnmodelledRegister
  void write(uint32_t off, unsigned width, uint32_t v);  // throws UnmodelledRegister
  void reset();                                          // all regs to reset values
  // Model-side access (bypasses masks). Throws UnmodelledRegister if undeclared.
  uint32_t get(uint32_t off) const;
  void set(uint32_t off, uint32_t v);

 private:
  struct Reg { uint32_t value, reset, rw, w1c; };
  Reg& find(uint32_t off, unsigned width, bool is_write);
  const Reg& find(uint32_t off, unsigned width, bool is_write) const;
  std::map<uint32_t, Reg> regs_;
};

}  // namespace stest
