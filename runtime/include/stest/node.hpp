#pragma once
// A node is one simulated MCU: the process-global runtime bus/clock, a Cortex-M core
// built from a chip description, the chip's peripheral models, and the firmware.
//
// Threading model ("baton"): the firmware runs on its own thread (8 MiB stack); the
// thread that owns the Node (the "loop" thread: standalone main, later the control
// channel) holds the baton otherwise. Exactly one of the two runs at any time; the
// handoff is a mutex + condition variable, so every model/core/clock access is
// single-threaded and ordered (no data races, no host-scheduling dependence).
//
//   loop:     run_until(t) -> sets quantum end t, passes the baton, blocks
//   firmware: runs until (a) an MMIO charge reaches t (runtime quantum hook),
//             (b) wfi() sleeps to t (Cortexm quantum hook), or (c) main returns;
//             then passes the baton back and blocks (a, b) or ends (c)
//   loop:     run_until returns Stop::Quantum / Stop::Exited
//
// IRQ handlers and clock events run on the firmware thread (inside MMIO calls/wfi).
// Between run_until calls the loop thread may freely inspect/modify models, memory
// and the event buffer (e.g. GPIO drive_pin), since the firmware is parked.
// Only one Node may exist per process (the runtime bus/clock are process-global).
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "stest/bus.hpp"
#include "stest/clock.hpp"
#include "stest/cortexm.hpp"
#include "stest/peripheral.hpp"

namespace stest {

// ---- event buffer (filled by models, drained by the node loop) ----
struct Event {
  uint64_t t_ns;
  nlohmann::json data;  // e.g. {"kind":"gpio","port":"A","pin":5,"level":1}
};
void emit_event(uint64_t t_ns, nlohmann::json data);
std::vector<Event> take_events();  // returns and clears, in emission order

// ---- model registry (name -> model, for MODEL_CALL-style access) ----
void register_model(Peripheral* p);  // keyed by p->name(); duplicate -> std::invalid_argument
Peripheral* find_model(std::string_view name);  // nullptr if unknown

// ---- chip description (emitted by tools/gen_chip.py) ----
struct ChipDesc {
  const char* name;                       // "STM32L496RG"
  unsigned nvic_prio_bits;
  const IrqHandler* irq_table;            // index = IRQn + 16
  size_t n_irqs;                          // table entries (16 + external IRQs)
  void (*init)(Bus&, Clock&, Cortexm&);   // maps + registers models; may be null
  void (*startup)(void);                  // e.g. CMSIS SystemInit, on the firmware thread; may be null
};

using FirmwareMain = int (*)(void);

class Node {
 public:
  enum class Stop { Quantum, Exited };

  // stest_rt_init(), builds + installs the Cortex-M core on runtime_bus()/runtime_clock(),
  // runs chip.init. Throws std::logic_error if another Node exists.
  explicit Node(const ChipDesc& chip);
  // Unmaps registered models, clears the registry/event buffer, uninstalls hooks. A
  // firmware thread still parked mid-run is abandoned (detached, never resumed).
  ~Node();
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Creates the (parked) firmware thread: chip.startup() then fw(). Once only.
  void start(FirmwareMain fw);
  // Runs the firmware until virtual time >= t_ns or firmware exit. If now >= t_ns it
  // returns Stop::Quantum immediately without running firmware. After an MMIO-charge
  // yield now may exceed t_ns by less than one access cost; a wfi yield stops at t_ns.
  Stop run_until(uint64_t t_ns);

  bool exited() const;
  int exit_code() const;  // firmware main's return value (valid when exited())
  uint64_t now_ns() const;
  Cortexm& core();
  const ChipDesc& chip() const;

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace stest
