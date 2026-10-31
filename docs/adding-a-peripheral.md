# Adding a peripheral model

This guide covers writing a register-level peripheral model for stest. The worked examples are the STM32L4 models in `chips/stm32l4/models/`:

- `syscfg` is the smallest model (registers only).
- `exti` subscribes to GPIO edges and drives level-sensitive IRQs.
- `tim` is the most complete. It derives its counter from virtual time, schedules Clock events, shares IRQ sources, aborts on features it does not model and exposes `call("state")`.

Read `AGENTS.md` first. Where this guide and the design spec (`docs/superpowers/specs/2026-10-30-stest-sil-design.md`, sections 4.2 and 4.3) disagree, the spec wins.

Sections 1-10 are about `register` mode, the default. Section 11 covers `hal` mode (`stest_add_firmware(... MODELS <TYPE>=hal)`), where a shim replaces ST's `stm32xx_hal_<ppp>.c` at link time.

## 1. Where a model fits

The firmware and ST's HAL are compiled natively. The MMIO pass rewrites every volatile access into a `__stest_mmio_*` call. The runtime charges virtual time for each access, fires due Clock events and then calls `Bus::read/write`. The Bus finds the mapping and calls your model's `read(off, width)` / `write(off, width, v)`, where `off` is relative to the block base.

```
firmware  GPIOA->BSRR = x
  -> __stest_mmio_store32 (runtime/src/mmio.cpp)
    -> Bus::write(addr) -> [clock gate] -> Gpio::write(0x18, 4, x)
```

- **One C++ class per peripheral kind, one instance per TOML entry.** `model = "stm32l4.tim"` in a part's `[[peripheral]]` maps to class `stest::stm32l4::Tim` in `chips/stm32l4/models/tim.hpp` (see `model_class()` in `tools/gen_chip.py`). TIM1 through TIM17 are all instances of `Tim`, and each instance takes its variant from `params`.
- Models are plain, uninstrumented C++ in the static library `stest_chip_<family>`, which is linked into every firmware executable of that family.
- Every model, Clock and core access is single-threaded. The firmware thread and the loop thread hand a baton back and forth (`runtime/include/stest/node.hpp:5-19`), so models need no locks.

## 2. Skeleton

The interface is in `runtime/include/stest/peripheral.hpp:37-47`:

```cpp
struct Peripheral {
  virtual std::string_view name() const = 0;
  virtual uint32_t read(uint32_t off, unsigned width) = 0;   // width in bytes: 1,2,4
  virtual void write(uint32_t off, unsigned width, uint32_t v) = 0;
  virtual void reset() {}
  virtual void connect(ModelContext&) {}
  virtual nlohmann::json call(std::string_view op, const nlohmann::json& args);  // throws UnknownOp
};
```

A minimal model has the shape of `chips/stm32l4/models/syscfg.hpp:15-30` and `syscfg.cpp`:

```cpp
namespace stest::stm32l4 {
class Foo : public Peripheral {
 public:
  Foo(std::string name, const nlohmann::json& params, ModelContext& ctx);
  std::string_view name() const override { return name_; }
  uint32_t read(uint32_t off, unsigned width) override { return regs_.read(off, width); }
  void write(uint32_t off, unsigned width, uint32_t v) override;
  void reset() override { regs_.reset(); }
  void connect(ModelContext& ctx) override;               // optional
  nlohmann::json call(std::string_view op, const nlohmann::json& args) override;  // optional
 private:
  std::string name_;
  RegisterFile regs_;
};
}
```

Rules:

- **Constructor `(std::string name, const nlohmann::json& params, ModelContext& ctx)`.** Parse and validate `params` here. On bad params, throw `std::invalid_argument` with a message that names the instance (`Tim::Tim`, `tim.cpp:37-60`). Keep references to `ctx.clock()` and `ctx.core()` if you need them. **Do not look up other models in the constructor**, because they may not exist yet.
- **`connect(ModelContext&)`** runs once every model exists and the clock tree is registered. Look up peers and subscribe to their Signals here (section 6).
- **`reset()`** returns the model to its reset state. The generated init calls it right after construction, so cancel any pending Clock event in it (`Tim::reset`, `tim.cpp:149-162`).
- **`call(op, args)`** is the Python-facing surface (section 8). Throw `UnknownOp(op)` for anything you do not handle. The base implementation already does this.
- The model header carries the documentation. Start it with a comment that gives the RM section, the registers modelled, the `params` schema, the strictness rules and the `call()` ops (see the top of `tim.hpp` and `exti.hpp`).

The generated chip init (`model_context.hpp:4-11`, `tools/gen_chip.py` `generate()`) runs in TOML order:
1. Constructs every model and calls `reset()` on it.
2. Calls `ctx.add`.
3. Calls `ctx.set_clock_tree(&RCC)`.
4. Calls `connect(ctx)` on every model.
5. For each model: `bus.map`, then `bus.set_gate` if the model has `rcc_enable`, then `register_model`.

## 3. Registers: `RegisterFile`

`RegisterFile` (`peripheral.hpp:63-93`) stores 32-bit registers with reset values and write masks, and enforces strictness for you:

| Call | Meaning |
|---|---|
| `declare(off, reset, rw_mask, w1c_mask = 0)` | Normal register. Firmware writes change only `rw_mask` bits. Writing 1 to a `w1c_mask` bit clears it. Other bits are read-only and ignore the write, as on hardware. |
| `declare(off, reset, 0, 0)` | **Read-only register.** Any write throws `ReadOnlyRegister`, so the node aborts. |
| `declare_write_only(off, reset = 0)` | BSRR/KEYR/EGR-style register. `write()` does the strictness checks and stores nothing, and the model applies the effect itself. |
| `read(off, width)` / `write(off, width, v)` | Firmware-path access with masks and strictness. |
| `get(off)` / `set(off, v)` | Model-side access that bypasses the masks, for hardware-set flags such as `SR`. |
| `value_ptr(off)` | Stable pointer to a register's live value, for cheap live reads (the RCC uses it for `ClockTree::enable_word`, which `Bus::set_gate` consumes). |

Strictness that you get for free:

- An access to an undeclared offset throws `UnmodelledRegister`, and the node aborts with `stest: unmodelled read32 0x... (NAME)` (`runtime/src/mmio.cpp:101-125`).
- A **sub-word access that crosses a register boundary** (`(off & 3) + width > 4`) also throws `UnmodelledRegister` (`runtime/src/peripheral.cpp:26,31`).
- **Sub-word lanes** are handled. A byte or halfword access reads or writes the matching lane of the containing aligned register. When you handle a register by hand, use the same helpers: `lane_mask`, `lane_extract` and `lane_merge` (`peripheral.hpp:49-61`). `Tim::read` for `CNT` (`tim.cpp:518-542`) is the pattern. Call `regs_.read(off, width)` first for the strictness check, then compute the live value and `lane_extract` it.
- Offsets must be word aligned and below `RegisterFile::kMaxBlock` (0x400). Otherwise `declare` throws `std::invalid_argument`.

When a write has side effects, **validate before you mutate**. `Tim::write` (`tim.cpp:544-557`) merges the lane into the full word, runs `check_write` (which may abort with "not modelled"), and only then updates state.

**Source of truth.** Take register offsets, bit positions and masks from ST's CMSIS device header for the part. For the L496 that is `stm32l496xx.h` in `cmsis_device_l4`, fetched into your build dir by `cmake/StestFetchST.cmake` (e.g. `TIM_TypeDef`, `TIM_CR1_CEN`). Take reset values and access types (rw, rc_w1, rc_w0, r) from the reference manual (RM0351 for L4). **Never write them from memory.** Note the RM section and the header in the model's header comment, and check reset values in a unit test (`chips/stm32l4/tests/test_exti.cpp`, "syscfg/exti: reset values").

## 4. Time and the model contract

Virtual time is in nanoseconds (`Clock::now_ns()`). `Clock::schedule_at(t_ns, fn)` returns an `EventId` that `Clock::cancel(id)` removes (`runtime/include/stest/clock.hpp`). Events fire on the firmware thread in (time, insertion) order.

**The model contract** (`peripheral.hpp:32-36`, `AGENTS.md`): a register value may change over time only in two ways:

1. through a **scheduled Clock event** at the instant it changes, or
2. by **reading differently on each access** (like SysTick VAL, or TIM CNT).

Never expose lazily computed "ready at time T" state that has no event at T.

**Why.** Poll fast-forward (`runtime/include/stest/runtime.hpp:37-52`) watches for loops that re-read the same (addr, width, value) tuples. After 64 store-free loads it jumps the clock to the next Clock event or the quantum end. If your flag only "becomes" set when read after T, the jump skips past T, and the firmware sees it late or never. A scheduled event at T makes the jump stop exactly there.

How `tim` satisfies the contract without hurting speed:

- **Derive counters from virtual time.** CNT is computed from an exact anchor, the clock period and PSC. It is not incremented by events (`tim.hpp:23-28`, `Tim::q_at` / `cnt_of`). A read returns the live value.
- **Schedule only the next instant something observable changes.** `Tim::schedule_from` (`tim.cpp:486-499`) computes the next update event or compare match whose flag is still clear and schedules one event. If every relevant flag is already set, or the counter is stopped, nothing is scheduled. That rules out event storms.
- **No per-tick events, ever.** A 20 kHz timer costs one event per update, not one per counter tick. This is what keeps the speed budget (≤ 0.4 s wall per virtual second per node) for the 20 kHz TIM7 test.
- **Reschedule on every timing-affecting write**: CR1, SR (clearing a flag re-arms), EGR, CNT, ARR, PSC/CCRx/RCR, and the clock tree. `Tim::write` ends each case with `schedule()`. It reads the kernel clock from the clock tree whenever it (re)schedules, and never caches a frequency across configuration changes.
- **Cancel stale events** before scheduling (`Tim::cancel_event`, `tim.cpp:476-479`) and in `reset()`.

If something is "instant" on hardware within the accuracy you need, just do it synchronously in the write. The RCC sets `xxRDY` the moment `xxON` is written (`rcc.hpp:4-6`).

## 5. Interrupts

Flag-driven interrupts are **levels**, as on STM32. Use `Cortexm::set_irq_level(irqn, high)` (`runtime/include/stest/cortexm.hpp:57-64`):

- A low-to-high change pends the IRQ.
- If the line is still high when the handler returns (the ISR did not clear the flag), the IRQ pends again and the handler re-enters, as on hardware.
- Recompute the level whenever the flag or the enable bit changes, and call only on a change. `Tim::update_irqs` (`tim.cpp:501-515`) and `Exti::pend_new` (`exti.cpp:46-57`) are examples.

**Shared vectors** (TIM1_UP_TIM16, TIM6_DAC, TIM1_BRK_TIM15, EXTI9_5, ...): if more than one *model instance* or source can drive one NVIC input, use `ModelContext::set_irq_source(irqn, &tag, high)` (`model_context.hpp:96-102`). `tag` is any address unique to the source (`Tim` uses `&src_tag_[s]`). The line is the OR of all high sources. Calling `set_irq_level` directly from two models would let one override the other. (EXTI drives `set_irq_level` itself because it alone owns lines 0-15 and ORs them internally.)

**`set_pending(irqn)`** is only for genuinely pulse-style sources, an event with no status flag the ISR clears. **Never use it for a status flag.**

IRQ numbers come from the part's TOML through `params` (e.g. `irq = { update = 54 }` for TIM6, or EXTI's 16-entry `irq` array). Never hard-code them in the model. A missing key means "no vector".

## 6. Talking to other models

In `connect(ModelContext& ctx)` (`runtime/include/stest/model_context.hpp`):

- **`ctx.get<T>("NAME")`** returns the peer as a `T&`. If the peer is missing or of the wrong type, it throws `std::logic_error` naming both. Use it for required peers (`Exti::connect`: `ctx.get<Syscfg>("SYSCFG")`).
- **`ctx.find("NAME")`** returns `Peripheral*` or nullptr, for optional peers (`Exti::connect` probes `GPIOA`..`GPIOI` and `dynamic_cast`s).
- **`ctx.clocks()`** returns the family's `ClockTree` (`model_context.hpp:31-44`), implemented by the RCC model: `sysclk_hz`, `hclk_hz`, `pclk1_hz`, `pclk2_hz`, `pllsai1r_hz`, `pllsai2r_hz`, `adcsel`, `enabled(reg_off, bit)`. Every value is computed live from RCC registers. Keep the reference (`Tim::connect` stores `&ctx.clocks()`), but **never cache a frequency**. Example, the timer ×2 rule (RM0351 6.2.14), `tim.cpp:225-229`:

  ```cpp
  uint64_t pclk = apb2_ ? clocks_->pclk2_hz() : clocks_->pclk1_hz();
  return pclk != clocks_->hclk_hz() ? 2 * pclk : pclk;  // x2 when the APB prescaler != 1
  ```

- **`Signal<T>`** (`model_context.hpp:46-60`) gives synchronous notifications. `emit()` calls every subscriber in order on the calling thread before returning (no queues). `Gpio::edges()` is a `Signal<PinEdge>` that fires on every pin level change, whether caused by firmware or by Python `drive_pin`. EXTI subscribes to it in `connect()` and latches PR synchronously, so the IRQ pends inside the very access that made the edge. If you add a Signal, expose it as a public accessor and document what fires it.

Keep a model's dependencies one-directional where possible. A model publishes a Signal, and its consumers subscribe to it.

## 7. Strictness

Strict by default (`AGENTS.md`, "Non-negotiable rules"). The node aborts with exit 70 and an `stest: ...` message, sent to Python as an ERROR frame. It never returns silent zeros or drops writes.

- **Unmodelled register, crossing access, write to a read-only register**: `RegisterFile` handles these (section 3).
- **Unsupported features abort when firmware enables them, not when it touches the register.** Store configuration bits that are harmless until used. Throw `stest::NotModelled(name_, "<feature> (<REG>.<BITS>)")` (`runtime/include/stest/peripheral.hpp`) when a write *selects* behaviour you do not model; `Tim::not_modelled` and `Tim::check_write` in `chips/stm32l4/models/tim.cpp` list every TIM case (slave mode, input capture, DMA requests, break inputs, option-register remaps, ...). At an MMIO access the runtime (`bus_read`/`bus_write` in `runtime/src/mmio.cpp`) prints `stest: <NAME>: <feature> not modelled (<read|write><bits> 0x<addr> = 0x<value>)` (no value for reads); from a Clock event it prints `stest: <NAME>: <feature> not modelled`. Any other `std::exception` thrown from `read`/`write` becomes `stest: exception in model at 0x...: <what>`, and one from a Clock event `stest: exception in clock event: ...` (`run_due_events` in `mmio.cpp`).
- **Reserved registers that ST's HAL reads anyway** may read 0 and abort on write (`Tim`'s SMCR on TIM6/7, `tim.hpp:6-8`). Write a comment explaining why.
- **Clock gating.** Give the TOML entry `rcc_enable = { reg = "APB1ENR1", bit = 5 }`. The generated init calls `Bus::set_gate` (`bus.hpp:36-43`), and any firmware access while the bit is clear aborts with `access to TIM7 while its clock is disabled (RCC APB1ENR1 bit 5) (read32 0x...)`. The model is not called, so models do not check this themselves. Model-to-model calls (e.g. EXTI reading `Syscfg::exti_port`) are not gated.
- **Never silently ignore**: a write that would change behaviour you do not model, an unknown `params` key value, a DMA or trigger request, or a mode bit. When in doubt, abort and name the feature. A user can then see exactly what to model next.

## 8. Python surface

**`call()` ops.** Python's `node.call("TIM7", "state")` (`python/stest/node.py:67-70`) sends a MODEL_CALL. The runtime looks up the model by name (`register_model`, done by the generated init) and calls `Peripheral::call(op, args)` with `args` as a JSON object (`runtime/src/control_channel.cpp:182-208`). Conventions:

- Read-only queries return a JSON object (`Tim` `"state"`, `tim.cpp:664-725`). Document its schema in the model header (`tim.hpp:46-57`).
- Injection ops take named args (`Gpio`: `"drive_pin" {pin, level}`, `"release_pin" {pin}`, `"get_pin" {pin}`).
- Throw `UnknownOp` for unknown ops and `std::invalid_argument` (or any `std::exception`) for bad args. Python raises `StestError`, and the **node keeps running** (REQ_ERROR, not ERROR).
- MODEL_CALL runs on the loop thread while the firmware is parked between quanta. It calls the model directly, **bypassing Bus clock gating by design**: it models the outside world, or a debugger, looking at the chip.

**Events.** Call `emit_event(clock.now_ns(), json)` (`runtime/include/stest/node.hpp:35-41`) for changes a test wants as a timeline. Conventions:
- The event is a flat JSON object with a `"kind"` key naming the event type and a `"periph"` key naming the emitting instance (the model's `name()`: `"GPIOA"`, `"TIM7"`, `"DAC1"`, `"SPI1"`, `"ITM"`), stamped with the event's virtual time. Example from `Gpio::update()`: `{"kind":"gpio","periph":"GPIOA","port":"A","pin":5,"level":1}`. Do not use `"bus"` for the instance (it is reserved for simulated buses such as CAN); every Python consumer keys on `periph`.
- Keys are serialized sorted, so tests must not match on raw key order.
- An event must stay under 64 KiB of JSON.
- Events show up in `Sim.event_log` and in standalone `EVT <t_ns> <json>` lines (`runtime/src/node_main.cpp:60`).
- Emit one event per meaningful change, never per tick. Events cost wall time.

**Typed helper.** Add a class to `python/stest/peripherals.py` that wraps the raw `call()`s (see `Timer` / `TimerChannel`), and an accessor on `Node` (`node.timer(name)`, `python/stest/node.py:63-65`). Raise `StestError` with a clear message for misuse (e.g. "TIM7 has no channel 1").

### 8.1 Analog models (ADC, DAC, pin voltages)

Analog values belong to the **pin**, not to the ADC. `Gpio` keeps a per-pin voltage next to (and independent of) the digital level, with two sources: an *external* value (Python) that wins, and an *on-chip driver* (the DAC, through `Gpio::set_analog_drive`). `Gpio::voltage(pin)` returns external-else-driver. Python reaches it with `node.pin("PA4").set_voltage(v)`, `.voltage` and `.release_voltage()` (call ops `set_voltage`, `get_voltage`, `release_voltage`; they bypass RCC gating like all MODEL_CALLs). `set_voltage` is refused on a pin with no analog function: every model that uses analog pins registers them in its `connect()` with `Gpio::add_analog_function(pin, "ADC12_IN16")` (`Adc::connect`, `Dac::connect`), and the error lists the chip's analog pins.

- An ADC model gets its pin table as `params.analog` (`[{"pin":"PB1","adc":[1,2],"channel":16}, ...]`), injected by `tools/gen_chip.py` from the part's `[[analog]]` table (see `docs/adding-a-chip.md` section 2.1). It resolves pins to the `GPIO<port>` models in `connect()` and reads `Gpio::voltage()` when a conversion samples its input (`adc.cpp`). Python never parses the TOML.
- A DAC model drives its pin with `set_analog_drive(pin, volts|nullopt)` and emits `{"kind":"dac","periph":"DAC1","channel":n,"volts":v|null}` on every output change.
- Typed helpers: `node.adc(n)` (`.set(channel, volts)`, `.get(channel)`, which act on the channel's pin) and `node.dac(1)` (`.channel(c).volts/.enabled/.dor`, `.state()`), in `python/stest/peripherals.py`.
- **strict_analog.** Converting a channel whose pin has no voltage reads 0 V and warns once per (ADC, channel). With strict analog it aborts instead. Switches: `Sim(strict_analog=True)` (node env `STEST_STRICT_ANALOG=1`), pytest `--stest-strict-analog`, marker `@pytest.mark.stest(strict_analog=True)`; precedence marker > CLI > default. A model reads it from `params.strict_analog`, else the env var.
- Document in the header what is not modelled and make unsupported configurations abort at the configuration write (`adc.hpp` is the template). Tests: `python/tests/test_adc.py`, `chips/stm32l4/tests/test_adc.cpp`.

### 8.2 SPI: bursts, transactions, MISO

`node.spi("SPI1")` (`SpiBus`) exposes `.bursts`, `.queue_miso(bytes)`, `.wait_for_bytes(n, timeout_ms)` and `.state()`. The model has no pin wiring: it emits one event per **burst**, grouped strictly by BSY (frames back to back, BSY never dropping), stamped when BSY falls: `{"kind":"spi","periph":"SPI1","t_start":ns,"mosi":hex,"miso":hex}`. A burst is not a transaction: `HAL_SPI_TransmitReceive` alternates TX and RX, so one call can span several bursts. Group by chip-select window instead: a burst belongs to the CS window with `cs_fall <= burst.t_start_ns` and `burst.t_end_ns <= cs_rise`, using `node.pin("PA4").history` (see the `SpiBus` docstring). `queue_miso` appends the bytes a slave returns, one byte per frame (two, little-endian, above 8 bits); an empty queue reads `0xFF`. Details and the strict-abort list are in `spi.hpp`.

### 8.3 Firmware output: ITM, DWT, CoreDebug, stdout

`chips/cortexm/` holds core-generic models (`Itm`, `Dwt`, `CoreDebug`) compiled into `stest_runtime` and mapped by `Cortexm` for every chip, so a part's TOML does not list them. They follow the same model contract as chip models.

- `CoreDebug` models DHCSR.C_DEBUGEN and DEMCR.TRCENA; ITM output needs TRCENA, ITMENA and the port's TER bit, as on hardware. DWT gives CYCCNT derived from virtual time.
- `Sim(debugger_attached=False)` (env `STEST_DEBUGGER=0`; pytest marker `debugger_attached=`) models a bare board: C_DEBUGEN and TRCENA reset to 0, so ITM prints nothing until the firmware enables it. Default is attached, so `ITM_SendChar` works unchanged.
- Python: **use `node.output` / `node.wait_for_log(regex, timeout_ms)` (default `source="any"`) for firmware logs; they work the same on every host.** `output` merges ITM port-0 and stdout lines in virtual-time order, so firmware that chooses ITM or `printf` by `__ARM_ARCH` (defined on arm64 hosts, not on x86-64) reads identically everywhere. `wait_for_log` is a virtual-time wait that consumes lines (`source="itm"` / `"stdout"` restrict it; each source has its own cursor). `node.log` (ITM port-0 lines only), `node.itm_port(n)`, `node.stdout_tail` / `stdout_lines(n)` (raw stdout pipe). Stdout lines come from the runtime's replacement of the C `stdout` stream (`runtime/include/stest/stdout_capture.hpp`: `funopen` on macOS, `fopencookie` on Linux) and are events `{"kind":"stdout","periph":"stdout","text":...}`; raw `write(1, ...)` bypasses it. An unterminated ITM or stdout line is flushed when the node exits or aborts.
- Tests: `python/tests/test_itm.py`, `selftest/firmware/itm`.

### 8.4 Fast variable access

`node.var(expr)` / `node.set_var(expr, value)` read and write firmware variables by name via DWARF (`python/stest/symbols.py`). `Sim.start()` loads the node's symbol table once (shared per executable path, size and mtime) and each node memoises `resolve(expr)`, so polling a variable in a loop is cheap (`test_var_hot_path`). Resolution is static; only the per-node ASLR slide is applied at access time.

## 9. Testing

Write the failing test first (TDD, `AGENTS.md`). Test at three levels.

**a) doctest unit tests** in `chips/<family>/tests/test_<kind>.cpp`, one executable per file, registered in `chips/<family>/CMakeLists.txt` as CTest `chip_<family>_<kind>` with `TIMEOUT 30`. Build a private rig that mirrors the generated init (`chips/stm32l4/tests/test_tim.cpp:35-136`):

```cpp
struct Rig {
  Bus bus; Clock clock;
  Cortexm core{bus, clock, 4, kTable, 16 + 91};
  ModelContext ctx{clock, core, bus};
  stm32l4::Rcc rcc{"RCC", json::object(), ctx};
  stm32l4::Tim tim7{"TIM7", tim_params("basic", 16, "apb1", {{"update", 55}}), ctx};
  Rig() {
    for (Peripheral* p : std::vector<Peripheral*>{&rcc, &tim7}) { p->reset(); ctx.add(p); }
    ctx.set_clock_tree(&rcc);
    for (Peripheral* p : ctx.models()) p->connect(ctx);
    bus.map(0x40021000, 0x400, &rcc);
    bus.map(0x40001400, 0x400, &tim7);
  }
  void to(uint64_t t) { /* advance, firing events: clock.advance + clock.run_due */ }
};
```

Go through `bus.read/write` (not the model directly) so that strictness surfaces as `UnmodelledAccess` (`CHECK_THROWS_AS(r.wr(TIM7, SMCR, 0), UnmodelledAccess)`). Cover at least the following:
- reset values
- masks, W1C and read-only aborts
- sub-word lanes
- every "not modelled" abort
- event timing (exact ns)
- that nothing is scheduled while flags are set (`clock.next_event_ns() == UINT64_MAX`)
- IRQ levels, including re-entry and shared vectors

**b) Self-test firmware** under `selftest/firmware/<name>/`. It is a CubeMX-style program that uses **ST's unmodified HAL**, built with `stest_add_firmware` in `selftest/CMakeLists.txt` (see `tim`: TIM7 at 20 kHz via `HAL_TIM_Base_Start_IT`, borrowing blinky's `stm32l4xx_it.c`, MSP and clock config):

```cmake
stest_add_firmware(tim
  CHIP STM32L496RG
  SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/firmware/tim/main.c ${_bl}/Src/stm32l4xx_it.c
          ${_bl}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/firmware/tim/Inc ${_bl}/Inc)
```

Enable the peripheral's RCC clock first, as real firmware does. If the HAL module is disabled in blinky's `stm32l4xx_hal_conf.h`, give the firmware its own `Inc/stm32l4xx_hal_conf.h` (as `firmware/tim/Inc` does). Expose observable state as `volatile` globals (`tick_count`, `exti_count`) and pins.

**c) pytest end-to-end test** in `python/tests/test_<kind>.py`. Add an `<name>_exe` fixture to `python/tests/conftest.py`, then drive the firmware (`python/tests/test_tim.py`, `test_exti.py`):

```python
def test_tim7_20khz(tim_exe):
    with Sim() as s:
        n = s.node("n", tim_exe)
        s.start()
        s.advance(ms=1)
        n.set_var("tick_count", 0)
        s.advance(ms=100)
        assert n.var("tick_count") == 2000
        assert n.timer("TIM7").update_hz == pytest.approx(20_000.0)
```

Also add:
- **Determinism**: two identical runs give identical `s.event_log`. Where it applies, a different `quantum_us` gives the same edges at the same times (`test_tim_is_deterministic`, `test_exti_is_deterministic`).
- **Speed**, if the model is on a hot path: a wall-clock bound per virtual second (`test_tim_speed_two_virtual_seconds`, 2 virtual s in ≤ 0.8 s wall). For standalone checks see `selftest/check_speed.py`.

**Running.** Natively:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure            # everything, incl. python_tests
ctest --test-dir build -R chip_stm32l4_tim --output-on-failure   # one unit-test binary
uv run pytest python/tests/test_tim.py -q             # uses build/selftest or $STEST_SELFTEST_BIN
```

Then in Docker, exactly as CI runs it (`.github/workflows/ci.yml`):

```sh
docker build -t stest-ci docker
docker run --rm -v "$PWD":/w -w /w stest-ci bash -euxo pipefail -c '
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ctest --test-dir build --output-on-failure
  uv run pytest python/tests -q
'
```

That command writes into the host `build/`. If you also build natively in the same tree, use a separate directory such as `build-docker`, and run pytest with `STEST_SELFTEST_BIN=build-docker/selftest`. Run the full suite **both natively and in Docker** before calling a model done. Linux and macOS paths differ.

## 10. Checklist

- [ ] `chips/<family>/models/<kind>.hpp`: class `stest::<family>::<Kind>`, a header comment (RM section, registers, `params`, not-modelled list, `call()` ops and schema, events).
- [ ] `chips/<family>/models/<kind>.cpp`: layouts and masks from the CMSIS device header, reset values from the RM.
- [ ] Time-driven behaviour follows the model contract: derived counters, one next-instant event, rescheduled on timing writes, cancelled in `reset()`.
- [ ] IRQs are levels (`set_irq_level`, or `set_irq_source` on shared vectors), and IRQ numbers come from `params`.
- [ ] Unsupported features throw `NotModelled(name, feature)` (printed `stest: <NAME>: <feature> not modelled (<access>)`).
- [ ] `chips/<family>/CMakeLists.txt`: the `.cpp` added to `stest_chip_<family>`, plus a doctest executable and `add_test(NAME chip_<family>_<kind> ...)` with `TIMEOUT 30`.
- [ ] `chips/<family>/parts/<part>.toml`: a `[[peripheral]]` per instance (`name`, `model`, `base`, `size`, `params`, `rcc_enable`). Check with `python3 tools/gen_chip.py --info <toml>` (see `docs/adding-a-chip.md`).
- [ ] `selftest/firmware/<name>/` and `selftest/CMakeLists.txt`: HAL-based self-test firmware.
- [ ] `python/tests/conftest.py` fixture and `python/tests/test_<kind>.py`: e2e, determinism and (if hot) speed.
- [ ] Analog pins / SPI / output: sections 8.1-8.3 followed if the model touches them.
- [ ] `python/stest/peripherals.py` typed helper and `Node` accessor, if Python needs more than raw `call()`.
- [ ] Docs: the model header comment, `AGENTS.md` repo map or notes if conventions change, and this guide if you introduced a new pattern.
- [ ] The full suite passes natively and in Docker.

## 11. Writing a HAL shim

`MODELS <TYPE>=hal` swaps one whole ST HAL file for an stest shim; ST files are never patched. The GPIO shim (`chips/stm32l4/hal/stm32l4xx_hal_gpio_shim.c`) is the worked example.

1. **Register the shim** in `chips/<family>/family.cmake`: add the type to `STEST_<FAMILY>_HAL_SHIMS` and set `STEST_<FAMILY>_HAL_SHIM_<TYPE>_REPLACES` (the ST file name) and `..._SOURCE`. `stest_add_firmware` drops the ST file from the HAL archive, adds the shim and passes `--hal <TYPE>` to `gen_chip.py`. The generated init then calls `Bus::set_hal_owned` on every peripheral of that type, so firmware MMIO to it still works but prints one `stest: warning: firmware accessed GPIOA ODR directly while GPIO is in hal mode` per register (name it with `Peripheral::reg_name`). TYPE is the model kind upper-cased. Unknown types, types without a shim and other modes fail at configure time.
2. **Same symbols, same signatures.** Define every public (non-static) symbol of the ST file, `__weak` callbacks included, with ST's prototypes from its header. A missing symbol is a link error for firmware that uses it.
3. **No MMIO to the hal-mode peripheral.** The shim is compiled like the HAL (with the MMIO pass) but never dereferences the peripheral's `TypeDef`: derive the instance from the pointer value and call the model's C API in `runtime/include/stest/hal_api.h`. Other peripherals the ST code touches (SYSCFG/EXTI for GPIO) stay in register mode. Keep ST's code for them as it is, so their accesses, strictness and timing are unchanged.
4. **The C API lives in the model** (`extern "C"` at the end of `<kind>.cpp`). It updates the model so its registers read back as in register mode. Account time with `stest::shim_access(addr, width, is_write, fn)` (`runtime.hpp`), one call per register access ST's code makes, in the same order. Each call charges the access cost, fires due events, checks the mapping and clock gate (strict aborts as for MMIO), runs `fn(Peripheral&)` on the model, delivers IRQs and feeds poll fast-forward, and never triggers the hal-mode warning. When the shim matches ST's access sequence, event logs are byte-identical to register mode. `test_hal_mode.py` checks this for blinky and exti.
5. **Test it.** Add doctests for the C API (registers after each call, access counts, no warning; see `chips/stm32l4/tests/test_gpio_hal.cpp`), `_<type>_hal` self-test builds of existing firmwares compared against register mode, and a direct-access warning case.

