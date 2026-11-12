#pragma once
// Model wiring: what a peripheral model sees of the rest of the chip.
//
// Generated chip init (tools/gen_chip.py), in TOML order:
//   1. construct every model as (name, params-json, ModelContext&) and reset() it
//   2. ctx.add(&model) for each
//   3. ctx.set_clock_tree(&<clock-tree model>)   (the family's RCC)
//   4. model.connect(ctx) for each: look up peer models (ctx.get<T>), subscribe to
//      their Signals, cache ClockTree&; every model exists by now
//   5. bus.map(...) + bus.set_gate(...) for peripherals with an RCC enable bit
// Constructors must not look up other models (they may not exist yet); do it in connect().
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>
#include "stest/peripheral.hpp"

namespace stest {

class Bus;
class Clock;
class Cortexm;

// The family's clock tree, implemented by its RCC model. Every value is computed from the
// RCC registers at call time (never cached), so it always reflects the firmware's latest
// configuration.
struct ClockTree {
  virtual ~ClockTree() = default;
  virtual uint64_t sysclk_hz() const = 0;
  virtual uint64_t hclk_hz() const = 0;
  virtual uint64_t pclk1_hz() const = 0;
  virtual uint64_t pclk2_hz() const = 0;
  virtual uint64_t pllsai1r_hz() const = 0;  // 0 when PLLSAI1 is off or its R output disabled
  virtual uint64_t pllsai2r_hz() const = 0;  // 0 when PLLSAI2 is off or its R output disabled
  virtual uint32_t adcsel() const = 0;       // RCC CCIPR ADCSEL (0 none, 1 PLLSAI1R, 2 PLLSAI2R, 3 SYSCLK)
  virtual bool enabled(uint32_t reg_off, unsigned bit) const = 0;  // RCC enable bit state
  // Live address of the RCC register at reg_off (for Bus::set_gate's word form: one load
  // per gated access). Unknown offset -> throws.
  virtual const uint32_t* enable_word(uint32_t reg_off) const = 0;
};

// Synchronous model-to-model notification: emit() calls every subscriber in subscription
// order, on the calling thread, before returning. No queues, no threads.
// Re-entrancy: a subscriber may subscribe further handlers while it runs; they are called
// from the next emit on. Subscribers live in a std::deque, whose push_back never moves or
// destroys existing elements, so the running std::function stays valid. (Unsubscribing
// does not exist.)
template <class T>
class Signal {
 public:
  using Fn = std::function<void(const T&)>;
  void subscribe(Fn f) { subs_.push_back(std::move(f)); }
  void emit(const T& v) {
    for (size_t i = 0, n = subs_.size(); i < n; ++i) subs_[i](v);  // n: snapshot of the count
  }

 private:
  std::deque<Fn> subs_;
};

std::string demangled_type_name(const std::type_info& t);

class ModelContext {
 public:
  ModelContext(Clock& clock, Cortexm& core, Bus& bus) : clock_(clock), core_(core), bus_(bus) {}
  ModelContext(const ModelContext&) = delete;
  ModelContext& operator=(const ModelContext&) = delete;

  Clock& clock() { return clock_; }
  Cortexm& core() { return core_; }
  Bus& bus() { return bus_; }

  // Keyed by p->name(); null or duplicate name -> std::invalid_argument.
  void add(Peripheral* p);
  Peripheral* find(std::string_view name) const;  // nullptr if unknown
  const std::vector<Peripheral*>& models() const { return order_; }  // in add() order

  // The model called `name` as a T. Missing, or not a T -> std::logic_error naming both
  // the model and the requested type.
  template <class T>
  T& get(std::string_view name) const {
    Peripheral* p = find(name);
    if (!p)
      throw std::logic_error("model context: no model named '" + std::string(name) + "' (wanted " +
                             demangled_type_name(typeid(T)) + ")");
    if (T* t = dynamic_cast<T*>(p)) return *t;
    throw std::logic_error("model context: model '" + std::string(name) + "' is a " +
                           demangled_type_name(typeid(*p)) + ", not a " +
                           demangled_type_name(typeid(T)));
  }

  // Part name ("STM32L496RG") for diagnostics; empty unless set (Node sets it from ChipDesc).
  const std::string& chip_name() const { return chip_name_; }
  void set_chip_name(std::string n) { chip_name_ = std::move(n); }

  ClockTree& clocks() const;  // none registered -> std::logic_error
  void set_clock_tree(ClockTree* t) { tree_ = t; }
  // Emitted by the clock-tree model (the family's RCC) after a register write or reset
  // that changed any ClockTree output (SYSCLK, HCLK, PCLK1/2, PLLSAI1R/2R, ADCSEL), at the
  // virtual time of that write; the argument is the tree, already showing the new values.
  // Models that run on a clock across time (a counting timer, ...) subscribe in connect()
  // and re-derive their state at the change instant (see tim.hpp and
  // docs/adding-a-peripheral.md, "Clock changes"). Not emitted for writes that leave every
  // output unchanged (enable bits, oscillator ready bits, ...).
  Signal<ClockTree>& clock_tree_changed() { return clock_tree_changed_; }

  // Shared level-sensitive IRQ line: several flag sources (e.g. TIM1 update and TIM16 on
  // TIM1_UP_TIM16, TIM6 and the DAC on TIM6_DAC) drive one NVIC input. `source` is any
  // address unique to one flag source; the line's level (Cortexm::set_irq_level) is the OR
  // of every source currently high, so it pends only when the first source rises and
  // stays high until the last one drops. Models with a shared vector must use this rather
  // than calling set_irq_level directly (which would let one source override the other).
  void set_irq_source(int irqn, const void* source, bool high);

 private:
  Clock& clock_;
  Cortexm& core_;
  Bus& bus_;
  ClockTree* tree_ = nullptr;
  Signal<ClockTree> clock_tree_changed_;
  std::string chip_name_;
  std::map<std::string, Peripheral*, std::less<>> by_name_;
  std::vector<Peripheral*> order_;
  std::map<int, std::vector<const void*>> irq_high_;  // irqn -> sources currently high
};

}  // namespace stest
