# stest Plan 2 — STM32L4 peripherals, model wiring, HAL-mode shims

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the STM32L496 package the peripherals the Sakura lighting and steering_wheel boards (and typical CubeMX projects) use — EXTI/SYSCFG, TIM, ADC, SPI, ITM/DWT — on top of a model-wiring layer (clock tree, RCC enable gating, cross-model signals), plus the per-peripheral `register`/`hal` mode switch proven with a GPIO HAL shim.

**Architecture:** Models are built in two phases by the generated chip init: construct all, then `connect(ModelContext&)` so models can look each other up by name and type. RCC exposes the clock tree; the Bus refuses accesses to peripherals whose RCC enable bit is clear. Models talk to Python only through the existing `MODEL_CALL`/event channel; Python grows typed helpers (`node.adc(...)`, `node.timer(...)`, `node.spi(...)`, `node.log`). `stest_add_firmware(MODELS GPIO=hal)` swaps ST's `stm32l4xx_hal_gpio.c` for an stest shim at link time.

**Tech Stack:** as Plan 1 (C11 firmware, C++17 runtime, LLVM ≥ 18 pass, CMake ≥ 3.23, Python ≥ 3.11, doctest, pytest, uv).

**Spec:** `docs/superpowers/specs/2026-10-30-stest-sil-design.md` · Plan 1: `docs/superpowers/plans/2026-10-30-stest-plan-1-core-and-skeleton.md` · Follow-ups consumed here: `docs/superpowers/plans/2026-10-30-plan-1-followups.md`

**Branch:** `plan-2-peripherals`, branched from `plan-1-core` (PR #1 not yet merged; rebase onto `main` after it merges).

## Global Constraints

- License Apache-2.0; ST files keep their notices. No Sakura names, pins, or DBC assumptions anywhere in stest.
- Platforms: native macOS arm64 and Linux (x86-64, arm64), both in CI; Docker image for everything else.
- Firmware sources and ST HAL sources compile **unchanged**; HAL-mode shims replace whole ST `.c` files at link time, never patch them.
- **Strict by default:** an access with no model, an unimplemented register, a write to a fully read-only register, or (new) an access to a peripheral whose RCC clock is disabled aborts the node with `stest: ...` naming address, width, direction and peripheral. No silent zeros.
- **Model contract** (peripheral.hpp): any time-driven state change is a scheduled `Clock` event or is visible in a changing read value (needed by the poll fast-forward).
- **Determinism:** identical inputs → byte-identical event logs; nothing depends on wall-clock time or host scheduling.
- **Speed:** per node ≤ 0.4 s wall per virtual second under the 20 kHz software-PWM load (Task 5 measures it end to end).
- Time unit: ns, `uint64_t` / Python `int`. Analog values: volts as Python `float`.
- Register layouts, reset values and bit semantics follow RM0351 (STM32L4x5/L4x6 reference manual) and ST's `stm32l496xx.h`; base addresses and IRQ numbers come from `stm32l496xx.h`.

## Review Focus

1. **Peripheral touched before its RCC clock is enabled** (common CubeMX ordering bug) → strict abort naming the enable bit, e.g. `stest: access to TIM7 while its clock is disabled (RCC APB1ENR1 bit 5)`. *(Task 2 `test_access_with_clock_disabled_aborts`)*
2. **Timer edge cases:** ARR=0, PSC/ARR rewritten while running, CEN cleared with UIF pending → no event storm, no divide-by-zero, a stopped timer raises no IRQs, ARPE/preload honoured. *(Task 5 doctests `tim_arr_zero_no_storm`, `tim_stop_no_irq`, `tim_preload`)*
3. **ADC channel with no input set** → reads 0 V and prints one `stest: warning: ADC1 channel 16 converted with no input set (0 V)` per channel; never aborts (calibration and unused channels are normal). *(Task 6 `test_unset_channel_warns_once`)*
4. **EXTI edge while the line's IRQ is masked/disabled** → PR bit latches; IRQ fires when unmasked; no edge lost or doubled. *(Task 4 doctest `exti_pending_until_unmasked`)*
5. **HAL-mode peripheral also poked directly by firmware** → one warning per register, run continues, state stays consistent. *(Task 9 `test_hal_mode_direct_access_warns_once`)*

## File structure

```
runtime/include/stest/
  peripheral.hpp      (modify) flat RegisterFile; lane helpers; ModelContext; Peripheral::connect
  model_context.hpp   (new)    ModelContext: typed lookup + clock tree + signals
  bus.hpp             (modify) map() window check; per-mapping clock gate
runtime/src/          peripheral.cpp bus.cpp node.cpp mmio.cpp (modify); model_context.cpp (new)
compiler/mmio_pass/StestMmioPass.cpp  (modify) volatile mem intrinsics
chips/stm32l4/models/
  rcc.*  gpio.*  (modify)   clock tree; GPIO level signal; hal-mode API
  syscfg.*  exti.*  tim.*  adc.*  spi.*  (new)
chips/cortexm/  itm.* dwt.* (new; core-generic, mapped by Cortexm)
chips/stm32l4/hal/stm32l4xx_hal_gpio_shim.c  (new)
chips/stm32l4/family.cmake    (new) hal-shim table for MODELS=
chips/stm32l4/parts/stm32l496rg.toml (modify) new peripherals, rcc_enable
tools/gen_chip.py (modify) two-phase init, rcc_enable gates
cmake/stest_add_firmware.cmake (modify) MODELS=
selftest/firmware/{exti,tim,adc,spi,itm}/main.c  (new)
python/stest/{node.py,sim.py,symbols.py,peripherals.py} (modify/new)
python/tests/test_{exti,tim,adc,spi,itm,hal_mode}.py (new)
docs/adding-a-peripheral.md, docs/adding-a-chip.md (new)
```

---

### Task 1: RegisterFile, lane helpers, Bus window check (refactor, no behaviour change)

**Files:** Modify `runtime/include/stest/peripheral.hpp`, `runtime/src/peripheral.cpp`, `runtime/include/stest/bus.hpp`, `runtime/src/bus.cpp`, `runtime/src/cortexm.cpp`, `chips/stm32l4/models/{rcc,gpio,pwr,flash}.cpp`; Test `runtime/tests/test_bus.cpp`, `chips/stm32l4/tests/*`.

**Interfaces — Produces:**
```cpp
namespace stest {
// Sub-word access helpers shared by every model (replace the 4 private copies).
uint32_t lane_mask(uint32_t off, unsigned width);                 // e.g. (2,2) -> 0xFFFF0000
uint32_t lane_extract(uint32_t word, uint32_t off, unsigned width); // value of that lane, right-aligned
uint32_t lane_merge(uint32_t word, uint32_t off, unsigned width, uint32_t v); // replace lane
class RegisterFile {  // public API unchanged; storage becomes std::array<Reg, kMaxRegs> indexed by off/4
  static constexpr uint32_t kMaxBlock = 0x400;  // declare() beyond this -> std::invalid_argument
  // declare(off...) with off % 4 != 0 -> std::invalid_argument
};
class Bus { void map(uint64_t base, uint32_t size, Peripheral*); // range outside both MMIO windows -> std::invalid_argument
};
}
```
`Gpio::update()` reads MODER/ODR/PUPDR/OTYPER once per call.

- [ ] **Step 1: Failing tests**: `lane_*` table test (all offsets 0–3 × widths 1/2/4, including width 4 at off 0); `RegisterFile.declare(0x402, …)` and `declare(0x401, …)` throw `std::invalid_argument`; `Bus.map(0xA0000000, 0x400, p)` throws; a micro-benchmark doctest (`[.bench]`, not in default run) prints ns per `Gpio::write` before/after.
- [ ] **Step 2: Run** `ctest --test-dir build -R "runtime_|chip_"` → new tests FAIL.
- [ ] **Step 3: Implement**; delete every private lane helper (rcc.cpp, gpio.cpp, peripheral.cpp, cortexm.cpp, flash.cpp:61, pwr.cpp:22 inline forms) in favour of the shared ones.
- [ ] **Step 4: Run** full `ctest` + `uv run pytest python/tests -q` natively and in Docker → all PASS; bench shows `Gpio::write` faster.
- [ ] **Step 5: Commit** `refactor(runtime): flat RegisterFile, shared lane helpers, Bus window check`.

### Task 2: Model wiring — ModelContext, clock tree, RCC enable gating

**Files:** Create `runtime/include/stest/model_context.hpp`, `runtime/src/model_context.cpp`; Modify `peripheral.hpp` (add `connect`), `bus.hpp/.cpp`, `chips/stm32l4/models/rcc.*`, every existing model ctor, `tools/gen_chip.py`, `chips/stm32l4/parts/stm32l496rg.toml`, `selftest/firmware/faults/main.c`; Test `runtime/tests/test_model_context.cpp`, `chips/stm32l4/tests/test_rcc_clocks.cpp`, `python/tests/test_faults.py`.

**Interfaces — Produces:**
```cpp
namespace stest {
struct ClockTree {            // implemented by the family's RCC model
  virtual ~ClockTree() = default;
  virtual uint64_t sysclk_hz() const = 0;
  virtual uint64_t hclk_hz() const = 0;
  virtual uint64_t pclk1_hz() const = 0;
  virtual uint64_t pclk2_hz() const = 0;
  virtual bool enabled(uint32_t reg_off, unsigned bit) const = 0;  // RCC enable bit state
};
template <class T> class Signal {   // synchronous, ordered by subscription; no threads
 public:
  using Fn = std::function<void(const T&)>;
  void subscribe(Fn f);
  void emit(const T& v);
};
class ModelContext {
 public:
  ModelContext(Clock& clock, Cortexm& core, Bus& bus);
  Clock& clock(); Cortexm& core(); Bus& bus();
  void add(Peripheral* p);                    // keyed by p->name()
  template <class T> T& get(std::string_view name);   // missing or wrong type -> std::logic_error naming both
  ClockTree& clocks();                        // the model registered via set_clock_tree; none -> std::logic_error
  void set_clock_tree(ClockTree* t);
};
struct Peripheral { virtual void connect(ModelContext&) {} /* called after all models exist */ };
class Bus { void set_gate(Peripheral* p, std::function<bool()> clocked, std::string what); };
// gated access with clocked()==false -> abort "access to <NAME> while its clock is disabled (RCC <REG> bit <n>)"
}
```
RCC computes SYSCLK from CFGR.SW (MSI per CR.MSIRANGE / ICSCR, HSI16, HSE = `params.hse_hz` default 8 000 000, PLL from PLLCFGR M/N/R with PLLSRC), HCLK = SYSCLK/HPRE, PCLK1/2 from PPRE1/2. Cross-check: Sakura-style config (HSE 8 MHz bypass, M=2 N=40 R=2) → SYSCLK 80 MHz.
TOML: each gated `[[peripheral]]` gets `rcc_enable = { reg = "AHB2ENR", bit = 0 }` (GPIOA). GPIOA–I get theirs in this task; later tasks add theirs. Ungated (no key): RCC, PWR? — **PWR is gated** (APB1ENR1 bit 28, HAL enables it), RCC/FLASH/core blocks ungated. Generated init: construct all → `ctx.add` → `set_clock_tree(&rcc)` → `connect()` on each in TOML order → `bus.map` + `set_gate`.

- [ ] **Step 1: Failing tests**: doctest — `get<Gpio>("GPIOA")` works, `get<Rcc>("GPIOA")` throws mentioning both names; Signal ordering; RCC clocks: reset → MSI 4 MHz (SYSCLK=HCLK=PCLK1=PCLK2=4 000 000); Sakura-style PLL config → 80 000 000 with PPRE1=PPRE2=1. Python `test_access_with_clock_disabled_aborts`: new faults variant `CLKOFF` writes `GPIOB->ODR` without enabling GPIOB → `NodeCrashed` matching `access to GPIOB while its clock is disabled \(RCC AHB2ENR bit 1\)`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.** Model ctors become `(std::string name, const nlohmann::json& params, ModelContext& ctx)`. Gate check lives in Bus dispatch (cost: one indirect call per gated access).
- [ ] **Step 4: Run** full suites native + Docker (blinky enables GPIOA first, so it still passes) → PASS.
- [ ] **Step 5: Commit** `feat(runtime): model context, RCC clock tree, clock-enable gating`.

### Task 3: Pass — volatile memcpy/memset

**Files:** Modify `compiler/mmio_pass/StestMmioPass.cpp`; `runtime/include/stest/mmio.h`, `runtime/src/mmio.cpp`; Test `spike/mmio_order_test.c` (+stubs).

**Interfaces — Produces (ABI additions):**
```c
void __stest_mmio_memcpy(volatile void *dst, const volatile void *src, size_t n);  // either side may be MMIO
void __stest_mmio_memset(volatile void *dst, int c, size_t n);
```
`llvm.memcpy/memmove/memset` intrinsics with `isvolatile=true` are rewritten to these. Runtime splits into the widest aligned accesses ≤ 4 bytes, each going through the normal load/store path (cost, events, strictness, poll streak).

- [ ] **Step 1: Failing test** in `mmio_order_test.c`: `struct S {uint32_t a,b;} volatile *P = (void*)0x40000000; *P = (struct S){1,2};` records two STORE32s at 0x40000000 / 0x40000004 in order; `memset`-style zeroing of a `volatile uint8_t[6]` at 0x40000010 records STORE32 + STORE16 (aligned split); at -O0 and -O2.
- [ ] **Step 2–4:** run → FAIL; implement; run → PASS (native + Docker).
- [ ] **Step 5: Commit** `feat(pass): route volatile memcpy/memset through the runtime`.

### Task 4: SYSCFG + EXTI (lines 0–15 from GPIO)

**Files:** Create `chips/stm32l4/models/{syscfg,exti}.{hpp,cpp}`, `selftest/firmware/exti/main.c`, `python/tests/test_exti.py`; Modify `gpio.*` (level signal), TOML (SYSCFG 0x40010000, EXTI 0x40010400; SYSCFG gated APB2ENR bit 0; EXTI ungated), `selftest/CMakeLists.txt`.

**Interfaces — Produces:** `Gpio` exposes `Signal<PinEdge>& edges()` with `struct PinEdge { char port; unsigned pin; int level; uint64_t t_ns; }`. SYSCFG: EXTICR1–4 (+ all other L496 SYSCFG registers declared with reset values), `unsigned exti_port(unsigned line) const` (0=A…8=I). EXTI: IMR1/EMR1/RTSR1/FTSR1/SWIER1/PR1 (+ IMR2…PR2 declared); for lines 0–15 subscribes to all GPIO ports' edges, filters by SYSCFG mapping, sets PR on matching edge, pends IRQ (`EXTI0..4`, `EXTI9_5`, `EXTI15_10`) when IMR set. PR is rc_w1. SWIER sets PR.

- [ ] **Step 1: Failing tests**: doctests `exti_rising_edge_sets_pr_and_irq`, `exti_port_mapping` (PC13 on line 13 only when EXTICR4 selects C), `exti_pending_until_unmasked` (Review Focus 4), `exti_swier`. Selftest firmware `exti`: PC13 input pull-down, `GPIO_MODE_IT_RISING` via HAL, `HAL_GPIO_EXTI_Callback` toggles PA5. Python `test_button_edge_triggers_callback`: drive PC13 high → within 1 ms PA5 toggles; drive low/high again → toggles again; exactly one toggle per rising edge.
- [ ] **Step 2–4:** FAIL → implement → PASS native + Docker.
- [ ] **Step 5: Commit** `feat(l4): SYSCFG and EXTI lines 0-15`.

### Task 5: TIM (basic, general-purpose, advanced base) + 20 kHz end-to-end speed check

**Files:** Create `chips/stm32l4/models/tim.{hpp,cpp}`, `selftest/firmware/tim/main.c`, `python/tests/test_tim.py`; Modify TOML (TIM1 0x40012C00, TIM2 0x40000000, TIM3 0x40000400, TIM4 0x40000800, TIM5 0x40000C00, TIM6 0x40001000, TIM7 0x40001400, TIM8 0x40013400, TIM15 0x40014000, TIM16 0x40014400, TIM17 0x40014800, each with its RCC enable bit and `params.kind = "basic"|"general"|"advanced"`, `width = 16|32`), `selftest/CMakeLists.txt`, `python/stest/{node.py,peripherals.py}`; possibly `runtime/src/control_channel.cpp`, `python/stest/{protocol.py,sim.py}` (event cost, Step 5).

**Interfaces — Produces:**
- Model: up-counting edge-aligned mode (CR1 CEN/UDIS/URS/OPM/ARPE, DIR/CMS → strict abort if down/center: "not modelled"), PSC (always preloaded), ARR (+ARPE), CNT derived from virtual time (`cnt = ticks_since_start % (ARR+1)` with ticks = elapsed_ns × timer_clk / (PSC+1)), EGR.UG, SR.UIF (rc_w0), DIER.UIE → update IRQ (`TIM6_DAC`, `TIM7`, `TIM2` … from TOML `irq`), next update scheduled as a Clock event (model contract). Timer clock = PCLKx, ×2 when the APB prescaler ≠ 1 (RM0351 §6.2.14). Channels 1–4 (general/advanced): CCMR OCxM PWM mode 1/2, CCRx (+OCxPE preload), CCER CCxE/CCxP, CCxIF on compare + DIER.CCxIE IRQ; BDTR.MOE required for advanced output.
- `call("state")` → `{"running": bool, "cnt": int, "arr": int, "psc": int, "clk_hz": int, "update_hz": float, "channels": [{"ch":1,"enabled":bool,"mode":"pwm1"|"pwm2"|"frozen"|…, "duty": float}]}`.
- Python (`python/stest/peripherals.py`): `node.timer(name) -> Timer` with `Timer.state() -> dict`, `Timer.update_hz -> float`, `Timer.channel(n).duty -> float`.
- Selftest firmware `tim`: CubeMX-style TIM7, Prescaler 0, Period 3999 (→ 20 kHz at 80 MHz PCLK1), `HAL_TIM_Base_Start_IT`; `HAL_TIM_PeriodElapsedCallback` runs a software PWM on PA6 at 25 % duty over 100 updates (5 ms period), and increments `tick_count`.

- [ ] **Step 1: Failing tests**: doctests `tim_update_rate` (80 MHz, PSC 0, ARR 3999 → updates exactly every 50 000 ns), `tim_arr_zero_no_storm`, `tim_stop_no_irq`, `tim_preload` (ARR write with ARPE takes effect at next update), `tim_apb_x2` (PPRE1=/2 → timer clk ×2), `tim_pwm_state`. Python `test_tim7_20khz`: after `advance(ms=100)`, `var("tick_count") == 2000` and PA6 high fraction over the window is 0.25 ± 0.01 from `pin.history`; `test_tim_speed_two_virtual_seconds`: 2 virtual s of the `tim` firmware completes in ≤ 0.8 s wall (0.4 s/virtual-s) — measured with `time.perf_counter`, Release runtime.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** the model and helpers.
- [ ] **Step 4: Run.** If `test_tim_speed_two_virtual_seconds` fails, profile first (`py-spy`/`perf` or simple timers) and record where time goes in the report. Expected culprit: one JSON event per GPIO edge (40 000/virtual s). Prescribed fix if so: GPIO edges travel as fixed 16-byte binary records in STOPPED (new event section after the JSON events: `u32 n_gpio; n × {u64 t_ns, u8 port, u8 pin, u8 level, u8 pad[5]}`), decoded in Python with `struct.iter_unpack`; JSON events unchanged for other kinds; `Sim.event_log` keeps GPIO edges as tuples, not dicts; pin history stays complete. Update `protocol.h`/`protocol.py` together and bump proto_version to 2. Re-run → PASS native + Docker; paste timings.
- [ ] **Step 5: Commit** `feat(l4): TIM models with update IRQ and PWM state` (+ `perf(protocol): binary GPIO events` if Step 4 needed it).

### Task 6: ADC (ADC1–3 + common) and analog inputs

**Files:** Create `chips/stm32l4/models/adc.{hpp,cpp}`, `selftest/firmware/adc/main.c`, `python/tests/test_adc.py`; Modify TOML (one model `ADC` at 0x50040000 size 0x400: ADC1 @+0x000, ADC2 @+0x100, ADC3 @+0x200, common @+0x300; gate AHB2ENR bit 13; IRQ `ADC1_2`, `ADC3`; `params.vref = 3.3`), `python/stest/peripherals.py`.

**Interfaces — Produces:**
- Model (per ADC instance): CR DEEPPWD/ADVREGEN/ADCAL/ADEN/ADDIS/ADSTART/ADSTP; ISR ADRDY/EOC/EOS/OVR (rc_w1); CFGR RES/ALIGN/CONT; SQR1 L + SQ1–4 (sequence length > 1 → strict abort "scan mode not modelled"), SMPR1/2, DR; CCR CKMODE/PRESC. Semantics: ADCAL self-clears after 116 ADC clock cycles (event); ADEN → ADRDY after 1 µs; ADSTART → EOC after (SMPx + 12.5) ADC cycles, ADC clock = HCLK/CKMODE (1,2,4) when CKMODE≠0, else strict abort "asynchronous ADC clock not modelled" unless RCC CCIPR ADCSEL selects SYSCLK (then SYSCLK/PRESC). Conversion value = `round(clamp(v, 0, vref) / vref * (2^res − 1))`, right-aligned unless ALIGN. Channel 0/17/18 internal (VREFINT reads 1.212 V, VBAT/temp strict abort). EOC IRQ if IER.EOCIE.
- `call("set_input", {"adc": 1, "channel": 16, "volts": 1.2})`, `call("get_input", …)`. Unset channel → 0 V + one stderr warning per (adc, channel) (Review Focus 3).
- Python: `node.adc(n: int) -> Adc` with `Adc.set(channel: int, volts: float)`; also `node.pin("PB1").set_voltage(volts)` resolving pin→(adc, channel) from a TOML `[[analog]] pin = "PB1", adc = [1,2], channel = 16` table (fill for every ADC-capable LQFP64 pin from DS11585 Table 16).
- Selftest firmware `adc`: CubeMX-style ADC1 with `ADC_CLOCK_SYNC_PCLK_DIV4`, 12-bit, calibration, `HAL_ADC_ConfigChannel` CH16, `HAL_ADC_Start` + `HAL_ADC_PollForConversion(…, 2)` + `HAL_ADC_GetValue` into global `last_raw` every 1 ms.

- [ ] **Step 1: Failing tests**: doctests for calibration, ADRDY, conversion timing, 12/10/8/6-bit scaling, clamp, async-clock abort. Python `test_adc_reads_voltage`: `node.pin("PB1").set_voltage(1.65)` → after 2 ms `var("last_raw") == 2048` (±1); `test_unset_channel_warns_once` (stderr contains the warning exactly once after 10 conversions); `test_adc_scan_mode_aborts`.
- [ ] **Step 2–4:** FAIL → implement → PASS native + Docker.
- [ ] **Step 5: Commit** `feat(l4): ADC1-3 with calibration, single conversions, analog pin inputs`.

### Task 7: SPI master + byte-stream capture

**Files:** Create `chips/stm32l4/models/spi.{hpp,cpp}`, `selftest/firmware/spi/main.c`, `python/tests/test_spi.py`; Modify TOML (SPI1 0x40013000 APB2ENR bit 12, SPI2 0x40003800 APB1ENR1 bit 14, SPI3 0x40003C00 APB1ENR1 bit 15, IRQs), `python/stest/peripherals.py`.

**Interfaces — Produces:**
- Model: master mode only (MSTR=0 → strict abort "SPI slave not modelled"); CR1 BR/CPOL/CPHA/SPE/LSBFIRST/SSM/SSI/BIDIMODE (bidi → strict abort), CR2 DS (4–16 bit)/FRXTH/TXEIE/RXNEIE/ERRIE/SSOE; SR TXE/RXNE/BSY/FTLVL/FRLVL/OVR; DR with 4-frame TX and RX FIFOs (8-bit access packs per RM0351). Each frame takes `DS bits × (2^(BR+1)) / PCLKx` ns; completion is a Clock event that moves the frame to RX FIFO, updates flags, raises IRQs. MISO data: per-bus queue filled from Python, default 0xFF when empty.
- Events: one per **burst** (frames back-to-back with BSY never dropping): `{"kind":"spi","bus":"SPI1","t_start":ns,"mosi":"<hex>","miso":"<hex>"}` emitted when BSY falls.
- `call("queue_miso", {"data":"<hex>"})`.
- Python: `node.spi(name) -> SpiBus` with `.bursts -> list[SpiBurst(t_start_ns, mosi: bytes, miso: bytes)]`, `.queue_miso(data: bytes)`, `.wait_for_bytes(n: int, timeout_ms)`. A burst plus the merged GPIO log is what a Python device decoder (e.g. a display) consumes; the display decoder itself is Plan 5.
- Selftest firmware `spi`: SPI1 master mode 0, 8-bit, prescaler 16, soft NSS on PA4 (GPIO); every 10 ms: CS low, `HAL_SPI_TransmitReceive` of `{0xA5, 0x01, 0x02, 0x03}`, CS high, stores RX into global `rx[4]`.

- [ ] **Step 1: Failing tests**: doctests for frame timing (8 bits at 80 MHz/16 → 1600 ns), FIFO thresholds/RXNE with FRXTH, OVR when RX overflows, BSY/burst grouping. Python `test_spi_burst_captured` (`bursts[0].mosi == bytes([0xA5,1,2,3])`, CS (PA4) low for the whole burst in `pin.history`), `test_spi_miso_queue` (`queue_miso(b"\x10\x20\x30\x40")` → `var("rx") == [0x10,0x20,0x30,0x40]`), `test_spi_miso_default_ff`.
- [ ] **Step 2–4:** FAIL → implement → PASS native + Docker.
- [ ] **Step 5: Commit** `feat(l4): SPI master with FIFO timing and burst capture`.

### Task 8: ITM, DWT, firmware output capture

**Files:** Create `chips/cortexm/{itm,dwt}.{hpp,cpp}` (mapped by `Cortexm`: ITM 0xE0000000, DWT 0xE0001000, CoreDebug DEMCR/DHCSR at 0xE000EDF0), `selftest/firmware/itm/main.c`, `python/tests/test_itm.py`; Modify `runtime/src/cortexm.cpp`, `python/stest/node.py` (stdout pipe + drain), `python/stest/pytest_plugin.py` (report section).

**Interfaces — Produces:**
- ITM: TER/TCR/TPR/LAR/LSR + 32 stimulus ports; reset state models "debugger attached": `TCR.ITMENA=1`, `TER=0x1`, DEMCR.TRCENA=1 (CMSIS `ITM_SendChar` then works unchanged). Port writes (8/16/32-bit) append bytes to a per-port line buffer; `\n` completes a line → event `{"kind":"itm","port":0,"text":"..."}`. FIFOREADY always 1.
- DWT: CTRL.CYCCNTENA, CYCCNT = core cycles since enable (derived from virtual time × SystemCoreClock; wraps 32-bit), LAR; other DWT registers declared, comparators strict.
- Node stdout is piped and drained like stderr (firmware `printf` on hosts where `sk_log`-style code picks stdout).
- Python: `Node.log -> list[str]` (ITM port-0 lines, in order) · `Node.stdout_tail -> list[str]` (last 200) · `Node.wait_for_log(pattern: str, timeout_ms: float) -> re.Match` (regex over new ITM lines). pytest failure report gains "stest node <n> log" (ITM) and "stdout" sections.
- Selftest firmware `itm`: CMSIS `ITM_SendChar` loop printing `"hello %d\n"` every 100 ms; enables DWT CYCCNT and stores CYCCNT delta across `HAL_Delay(9)` (ST's HAL_Delay adds one tick → 10 ms) in global `cycles_10ms`.

- [ ] **Step 1: Failing tests**: Python `test_itm_lines` (`n.log[:3] == ["hello 0","hello 1","hello 2"]` after 250 ms), `test_wait_for_log`, `test_dwt_cyccnt` (`var("cycles_10ms")` = 800 000 ± 8 000, i.e. 10 ms at 80 MHz ± 1 %), `test_stdout_captured` (faults-style firmware variant `PRINTF` writes via `printf`, `n.stdout_tail[-1] == "hi"`).
- [ ] **Step 2–4:** FAIL → implement → PASS native + Docker (both arm64 and x86: the ITM path must work regardless of `__ARM_ARCH`).
- [ ] **Step 5: Commit** `feat(cortexm): ITM/DWT models and firmware output capture`.

### Task 9: HAL-mode shims — `MODELS=` and GPIO `hal`

**Files:** Create `chips/stm32l4/family.cmake`, `chips/stm32l4/hal/stm32l4xx_hal_gpio_shim.c`, `runtime/include/stest/hal_api.h`, `python/tests/test_hal_mode.py`; Modify `cmake/stest_add_firmware.cmake`, `chips/stm32l4/models/gpio.*`, `runtime/src/bus.cpp`, `selftest/CMakeLists.txt`, `README.md`.

**Interfaces — Produces:**
```cmake
stest_add_firmware(<name> ... MODELS <TYPE>=<register|hal> ...)  # default register
# chips/<family>/family.cmake:
#   set(STEST_<FAMILY>_HAL_SHIMS GPIO)                 # types with a hal shim
#   set(STEST_<FAMILY>_HAL_SHIM_GPIO_REPLACES stm32l4xx_hal_gpio.c)
#   set(STEST_<FAMILY>_HAL_SHIM_GPIO_SOURCE ${CMAKE_CURRENT_LIST_DIR}/hal/stm32l4xx_hal_gpio_shim.c)
# unknown TYPE or unsupported mode -> FATAL_ERROR listing supported types/modes
```
```c
/* hal_api.h — C API shims call (runtime side, uninstrumented) */
void     stest_gpio_hal_init(char port, uint16_t pins, uint32_t mode, uint32_t pull, uint32_t speed, uint32_t alternate);
void     stest_gpio_hal_write(char port, uint16_t pins, int level);
int      stest_gpio_hal_read(char port, uint16_t pin);
void     stest_gpio_hal_toggle(char port, uint16_t pins);
```
Shim implements every public symbol of ST's `stm32l4xx_hal_gpio.c` (`HAL_GPIO_Init/DeInit/ReadPin/WritePin/TogglePin/LockPin/EXTI_IRQHandler/EXTI_Callback`) with ST's signatures; the GPIO model updates its registers (MODER/ODR/…) so register-level observers see the same state; EXTI configuration in `HAL_GPIO_Init` for IT modes goes through the SYSCFG/EXTI models' registers. With GPIO in `hal` mode, the Bus marks GPIO blocks "hal-owned": a direct firmware access still works but prints `stest: warning: firmware accessed GPIOA ODR directly while GPIO is in hal mode` once per (peripheral, register).

- [ ] **Step 1: Failing tests**: CMake configure tests (`MODELS FOO=hal` and `MODELS TIM=hal` → FATAL_ERROR with the supported list). Python `test_blinky_hal_mode_matches_register_mode`: blinky built as `blinky` and `blinky_gpio_hal` → identical `event_log` for 2100 ms; `test_exti_hal_mode` (exti selftest in hal mode toggles the same); `test_hal_mode_direct_access_warns_once` (firmware variant writes `GPIOA->BSRR` twice in hal mode → exactly one warning line, run continues).
- [ ] **Step 2–4:** FAIL → implement → PASS native + Docker.
- [ ] **Step 5: Commit** `feat: per-peripheral MODELS= selection and GPIO HAL shim`.

### Task 10: Python performance/cleanup + contributor docs

**Files:** Modify `python/stest/{node.py,sim.py,symbols.py}`; Create `docs/adding-a-peripheral.md`, `docs/adding-a-chip.md`; Test `python/tests/test_symbols.py`.

**Interfaces — Produces:** `Node` loads its `SymbolTable` once in `Sim.start()` (after HELLO) and memoises `resolve(expr)` per expression string; `Node._kill` removed (one shutdown path: `Sim._kill_all`/`close`). Docs: how a model is written (RegisterFile, ModelContext/`connect`, Clock events + model contract, `call()` ops, events, strictness, tests), how a HAL shim is added (family.cmake), how a part TOML is added (peripherals, rcc_enable, irq, analog table), with the L4 models as the worked example.

- [ ] **Step 1: Failing test** `test_var_hot_path`: 10 000 `n.var("u8")` calls with the sim paused take ≤ 0.5 s wall, and monkeypatching `SymbolTable.load` to raise after start proves no reload happens.
- [ ] **Step 2–4:** FAIL → implement → PASS (full suites native + Docker).
- [ ] **Step 5: Commit** `perf(python): cache symbol table per node; docs for adding peripherals and chips`.
