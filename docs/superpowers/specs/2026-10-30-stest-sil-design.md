# stest — STM32 software-in-the-loop simulation & testing

Status: design, pending approval · 2026-10-30

## 1. Intent

Run real STM32 firmware, unmodified, natively on a developer's machine (and in CI),
with simulated peripherals, multiple boards talking over simulated buses, and tests
written in pytest. Firmware gets the same `HAL_*`/LL/register API it uses on the chip;
tests get deterministic virtual time, pin/bus injection, and named access to firmware
memory.

Why: today the Sakura solar-car firmware (`Sakura-2025-2026/firmware/Sakura`) can only
host-test HAL-free logic (DBC pack/unpack, CAN router). Everything that touches
hardware — and every multi-board interaction over CAN — is only testable on the car.
stest closes that gap.

Long-term goal: a public, general-purpose library that **anyone with an STM32 project**
can adopt without restructuring their firmware, built out incrementally one chip family
and one peripheral at a time. Sakura is the first user and the proving ground, not a
special case.

## 2. Goals, non-goals, anti-goals

### Goals
- Compile a project's real sources — including CubeMX `main.c`, `stm32xx_it.c`,
  `stm32xx_hal_msp.c` — for the host with **no firmware source changes**.
- Extensible along two axes: **chip families/parts** and **peripherals**, each added
  as a self-contained unit.
- Deterministic **virtual time** controlled from Python; multiple firmware nodes run in
  lockstep.
- **Multiple simulated CAN buses**, topology defined from Python; firmware nodes and
  Python virtual devices attach to any bus.
- pytest-first Python API: fixtures, waiting on conditions, pin/analog injection,
  bus traffic, and reading/writing firmware variables by name.
- Clear, loud failures for anything not yet simulated.

### Non-goals
- Instruction-level or cycle-accurate emulation (Renode/QEMU already do that).
  Firmware runs natively; timing is modelled at peripheral granularity.
- Electrical/analog circuit simulation beyond "Python sets a pin level / ADC voltage".
- Native Windows builds. Windows users run stest through the official Docker image
  (Docker Desktop / WSL2); native Windows is revisited only if Docker proves a barrier.
- Simulating every peripheral of a chip in v1.
- Not in v1: FreeRTOS host port, USB CDC shim, Sakura telemetry board (next milestone).

### Anti-goals (would be failure even if it "works")
- Requiring users to restructure firmware, add `#ifdef SIL` branches, or hand-write
  per-project glue for each peripheral.
- Flaky tests: anything whose outcome depends on host scheduling or wall-clock time.
- Silent wrong behaviour: an unimplemented HAL function or register that quietly
  returns 0/`HAL_OK` and lets a test pass.
- A Sakura-shaped library: no Sakura names, pinouts, or DBC assumptions in stest core.

## 3. Decisions made in conversation

| Topic | Decision |
|---|---|
| Firmware entry | Real `main.c` runs unmodified by default. An app-only entry point is an optional alternative, not the main path. |
| Time | Virtual time, owned by the Python-side scheduler, all nodes in lockstep. |
| Peripheral modelling | **Register-level is the foundation**: ST's real HAL/LL sources compile unchanged against simulated register blocks. **HAL-level shims** exist as an escape hatch for complex peripherals (USB, later SDMMC/ETH/crypto) and for quick bring-up of unmodelled peripherals. Selected per peripheral *type*, at build time. Not every peripheral gets both. |
| CAN | Bus model is shared; front-ends per peripheral type (bxCAN, FDCAN). Multiple buses, configured from Python. Python virtual devices are first-class bus nodes. |
| Memory access | Globals/statics (incl. function-local statics, struct fields, array elements) readable/writable by name from Python. Stack locals "where possible" via an optional debugger-backed mode. |
| Waiting | pytest can wait on arbitrary conditions, frames, and pin states in virtual time. |
| Compiler / speed | Upstream LLVM clang may be required for `register` mode; trap-based fallback; Sakura suite must run ≥ real time in CI. |
| Platforms | Native Linux (x86-64, arm64) + native macOS (arm64), both in CI; official Docker image / dev container for Windows and zero-setup use. Native Windows deferred (official Windows LLVM can't load pass plugins). Speed target is about test duration only — results are host-speed independent. |
| License | Apache-2.0. Vendored ST files keep their own notices (CMSIS device: Apache-2.0; HAL: BSD-3-Clause). |
| Public | This repo (`stest`) will be public. |
| v1 boards | Sakura **lighting** and **steering_wheel** (STM32L496RG). Telemetry (USB CDC, FreeRTOS) deferred — its firmware is still a stub. |
| v1 second family | A **minimal STM32U3C5** package (chip description + FDCAN model), proven by a small stest-owned test firmware. |
| Where Sakura tests live | In `Sakura-2025-2026/firmware/Sakura` (e.g. `tests/sil/`), with a `sil` CMake preset. stest itself contains only its own self-test firmwares. |

## 4. Architecture

```
 pytest ──► stest (Python)
              │  scheduler (owns virtual time), topology, buses, virtual devices,
              │  symbol/DWARF reader, pytest plugin
              │
              │  control channel (one per node)
     ┌────────┼──────────────┐
     ▼        ▼              ▼
  node proc  node proc   node proc        each = user firmware + stest runtime
  lighting   steering    (other node)     (host executable)
     │          │            │
     └──── CAN frames routed by the Python-side bus model ────┘
```

### 4.1 Node = one host process
Each firmware instance is its own OS process (isolated globals, multiple instances of
the same firmware, a crash only kills one node). A node executable is:

- the user's firmware sources (unchanged),
- ST's HAL/LL sources for register-mode peripherals (unchanged),
- stest's **chip package** for the target part,
- the stest **runtime** (C/C++): register blocks, peripheral models, IRQ dispatch,
  time keeping, control-channel client, memory-access service.

The firmware's own `main()` must run unmodified; how the runtime gains control before
it (symbol renaming at compile time, a constructor, a linker trick) is the builder's
call.

### 4.2 Chip packages (extensibility axis 1)
Organised **family → part**. A family package (e.g. `stm32l4`, `stm32u3`) contains:

- **Host device headers** with the same names and types as ST's CMSIS device headers
  (`stm32l4xx.h`, `stm32l496xx.h`, …). Peripheral instance macros (`GPIOA`, `CAN1`,
  `FDCAN1`, …) resolve to register blocks in host memory, not fixed addresses.
- A **host CMSIS-core replacement**: intrinsics (`__DSB`, `__ISB`, `__WFI`, `__WFE`,
  `__disable_irq`, …), NVIC/SysTick/SCB access route into the runtime.
- A **chip description** (data, not code): peripherals and instance counts, pin/port
  map, alternate functions as needed, IRQ numbers, memory sizes, clock defaults.
  Format is the builder's call; it must be easy for contributors to add a part.
- Register-level **peripheral models** for that family (shared by all parts in it).
- Optional **HAL shims** for that family.
- Matching **Python helpers** (e.g. typed accessors for that family's peripherals).

Adding a new part in an existing family should be mostly a new chip description.

### 4.3 Peripheral models (extensibility axis 2)
Each model declares the modes it supports (`register`, `hal`, or both) and exposes:

- register read/write semantics (for `register` mode),
- an API to the runtime (time, IRQ raise, bus attachment),
- a Python-visible surface (state, injection, events).

Build-time selection per peripheral type:

```cmake
stest_add_firmware(lighting
  CHIP STM32L496RG
  SOURCES ${LIGHTING_SOURCES}
  MODELS CAN=register TIM=register USB=hal)   # defaults to register where available
```

- `register`: ST's `stm32xx_hal_<ppp>.c` / LL compiled in; the model reacts to register
  accesses.
- `hal`: the stest shim replaces ST's `stm32xx_hal_<ppp>.c` at link time. The shim
  must mirror state into the register block where meaningful (e.g. GPIO ODR) so tests
  see one consistent view.
- Firmware touching registers of a `hal`-mode peripheral → runtime warning naming the
  register.
- Requested mode unsupported by the model → configure-time error.
- Mixed modes within one peripheral type (GPIOA hal, GPIOB register) are not supported.

**Register access interception is the central technical risk.** Firmware writes
registers as plain volatile memory accesses (`GPIOA->BSRR = x`, write-1-to-clear flags,
read-to-clear data registers, HAL polling loops waiting on ready bits with
`HAL_GetTick` timeouts). Models must see accesses **in order and as they happen**, not
by diffing memory later. The mechanism (e.g. guard-page trapping with instruction
emulation, compiler instrumentation of volatile accesses, or another approach) is the
builder's call, but it must be **proven by a spike before the rest of the plan is
committed**, on both macOS arm64 and Linux x86-64, against GPIO BSRR, a W1C status
register, and a HAL polling loop with timeout.

**Compiler requirement (decided):** stest may require **upstream LLVM clang** (e.g.
`brew install llvm`; Apple clang and gcc cannot load pass plugins) for `register`
mode, so compiler instrumentation of volatile/peripheral accesses is an allowed
mechanism. A portable trap-based mechanism (memory protection, any compiler) is the
slower fallback. If neither meets the speed target on a platform, that platform falls
back to `hal` mode for affected peripherals; the register-level foundation does not
change.

**Speed target (decided):** the Sakura v1 SIL suite runs **at least real time** in CI —
e.g. a 2-node test covering 5 virtual seconds finishes in ≤ 5 s wall clock. Lighting's
software lamp PWM (TIM7 IRQ at 20 kHz, touching TIM and GPIO registers each time) is
the stress case the spike must be measured against.

### 4.4 Interrupts and execution
- The runtime calls the firmware's real `*_IRQHandler` functions (from
  `stm32xx_it.c` / the vector table names), honouring NVIC enable/priority/pending and
  `__disable_irq`.
- Interrupts are delivered at well-defined points in the firmware's execution (e.g.
  on peripheral accesses, time-advancing calls, and sync points). A pure computation
  loop that touches no peripheral is not preempted until its next such point —
  acceptable and documented.
- **FreeRTOS** (post-v1, for telemetry/bms) will run via a host port driven by virtual
  time, one task executing at a time. v1 must not make choices that preclude it
  (e.g. the runtime must tolerate multiple firmware execution contexts later).

### 4.5 Virtual time and the scheduler
- The Python-side scheduler owns the global virtual clock. Nodes run until they reach
  a point where time would pass (`HAL_Delay`, `__WFI`/`__WFE`, RTOS idle, end of
  quantum), then sync. The clock advances only when every node has caught up.
- Cross-node events (CAN frames) are timestamped and delivered in a deterministic
  order. Same inputs → same outputs, every run.
- Busy-wait protection: tick/peripheral reads cost a small configurable amount of
  virtual time, and each node has a maximum quantum, so `while (HAL_GetTick() - t <
  10)` loops terminate.
- Python API: `sim.advance(ms=...)`, `sim.run_until(cond, timeout_ms=...)`, and
  event-specific waits (below). Timeouts are in virtual time; there should also be a
  wall-clock safety timeout so a hung node fails a test instead of hanging CI.
- Clock tree: RCC/PWR configuration is accepted and recorded; derived frequencies
  (`HAL_RCC_GetPCLK1Freq`, etc.) reflect what the firmware configured so baud/bit-time
  calculations come out right.

### 4.6 CAN buses
- A bus is a Python-side object: `sim.can_bus("drive", bitrate=500_000)`. Any number
  of buses.
- Node CAN controllers attach by instance name, from Python:
  `sim.node("lighting", exe=..., can={"CAN2": "drive"})`. A dual-controller board can
  sit on two buses; a gateway can bridge them.
- Python virtual devices attach the same way, send/receive frames, and may use a DBC
  (cantools) for encode/decode. A sniffer/logger can attach to any bus.
- Frame delivery timing derives from bitrate and frame length; simultaneous
  transmissions resolve by ID arbitration.
- Firmware sees correct controller behaviour for the modes it uses: filters (bxCAN
  filter banks, incl. CAN2's banks living in CAN1's block), FIFOs, mailboxes, TX
  complete/RX pending IRQs, overrun. Error states (bus-off, no-ACK when alone on the
  bus) are modelled at least enough not to lie; fault injection is a stretch goal.
- Traffic can be exported in a format standard tools read (candump log and/or ASC).
- bxCAN (L4) and FDCAN (U3) are separate register models on the same bus model. In
  v1 FDCAN needs classic CAN frames; CAN-FD frames are a stretch goal.

### 4.7 Memory and symbol access
- Node executables are built with debug info. Python reads symbols, addresses, and
  types from the binary's debug info and resolves expressions like
  `node.var("g_state.lamps[2].duty")` and function-local statics.
- Reads/writes are served **by the node's own runtime** over the control channel while
  the node is paused at a sync point (handles ASLR via a reported load base). No
  debugger or OS privileges required. Works on macOS and Linux.
- Values come back typed (ints, floats, enums by name, structs as dict-like, arrays).
- Optional debugger-backed mode (LLDB/GDB) for stack locals and breakpoints, e.g.
  `with node.break_at("turn_signal_update") as f: f.local("elapsed")`. Best-effort;
  depends on build flags and platform debugger permissions.

### 4.8 Python package & pytest plugin
Indicative API (names are the builder's call; capabilities are not):

```python
def test_left_blinker(sim, sakura):
    drive = sim.can_bus("drive", bitrate=500_000)
    sw    = sim.node("steering_wheel", exe=sakura.exe("steering_wheel"), can={"CAN2": drive})
    light = sim.node("lighting",       exe=sakura.exe("lighting"),       can={"CAN2": drive})
    sim.start()

    sw.pin("PC15").set_low()                      # press left-turn button
    drive.wait_for_frame(id=0x123, timeout_ms=200)
    light.pin("PA8").wait_high(timeout_ms=100)   # illustrative lamp pin
    sim.wait_for(lambda: light.var("s_turn.state") == 1, timeout_ms=1000)
```

- pytest plugin: fixtures for the sim, automatic teardown, per-test node logs and
  CAN logs attached on failure.
- Injection: digital pin levels, ADC input voltages, EXTI edges.
- Observation: pin levels and history, PWM duty/frequency, CAN traffic, firmware stdout/`printf`, display framebuffer.
- Waiting: generic condition, frame match, pin level/edge — all in virtual time.
- Building firmware is outside the pytest run: tests receive paths to already-built
  node executables (the consuming project's CMake builds them). *(builder may add a
  convenience build hook)*

### 4.9 Distribution & integration
- This repo holds: C/C++ runtime, chip packages, CMake module, Python package,
  self-test firmwares, docs.
- C side consumed via CMake (`find_package` and/or `FetchContent`); provides
  `stest_add_firmware(...)`.
- Python side installable with pip. Package name is the builder's call (check PyPI
  availability of `stest`).
- Host platforms for v1: **native Linux (x86-64 and arm64) and native macOS (arm64)**,
  both tested in CI. An **official Docker image** (plus a dev-container config) provides
  the same toolchain anywhere, including Windows; Sakura's CI uses it.
- ST HAL/CMSIS sources are taken from the consuming project (Sakura already pins them
  as submodules) or fetched by stest at pinned versions — builder's call, but stest
  must not require users to replace their HAL version.

## 5. v1 scope

### stest
- Runtime core: process model, control channel, scheduler, virtual time, IRQ dispatch,
  memory/symbol access, unimplemented-access reporting.
- **STM32L4 family / STM32L496RG** package with register models for: RCC/PWR (accept
  and report), FLASH latency, GPIO, EXTI/SYSCFG, NVIC, SysTick, TIM (base + update IRQ
  + PWM output state), bxCAN (CAN1+CAN2), SPI (master TX/TXRX), ADC (steering_wheel uses it), and ITM
  stimulus port output (Sakura's `sk_log` routes logs through ITM). DAC and I2C are
  not used by the v1 boards and are out of v1.
- **HAL-mode mechanism proven** by at least one HAL shim exercised in a self-test
  (e.g. GPIO as `hal`), since no v1 Sakura board needs one.
- **STM32U3 family / STM32U3C5RI** minimal package: chip description, device headers,
  RCC/GPIO/NVIC/SysTick basics, **FDCAN** register model (classic CAN frames). Proven by
  a small stest-owned test firmware that exchanges frames with an L496 node across
  one simulated bus.
- Display support for the steering wheel: capture the ST7565 SPI command/data stream
  (as driven by u8g2) and decode it to a framebuffer readable/savable from Python.
  Implemented as a reusable "SPI device" model attached in Python, not a Sakura special
  case.
- Python package + pytest plugin as in §4.8; DBC-aware virtual CAN devices.
- Self-tests per peripheral model using tiny test firmwares; CI.
- Docs: getting started, "add a chip", "add a peripheral model".

### Sakura (`Sakura-2025-2026/firmware/Sakura`)
- A `sil` configure/build preset producing host node executables for lighting and
  steering_wheel. No changes to `boards/*/Core`, `boards/*/app`, or `shared/`.
  Build-system changes (top-level `CMakeLists.txt`, `CMakePresets.json`, `cmake/`)
  are allowed — today it builds one board per build directory.
- `tests/sil/` pytest suite, at minimum:
  - lighting boots, runs its periodic CAN TX schedule at the right periods.
  - steering-wheel input (button) → CAN frame → lighting output pin/PWM reacts with the
    correct timing (e.g. blinker period).
  - steering-wheel display shows expected content for a known state.
  - a Python virtual device on the bus (DBC-encoded) drives a board's behaviour.

## 6. Constraints & known facts about Sakura (for the builder)
- MCU: STM32L496RGTx, 80 MHz SYSCLK from 8 MHz HSE bypass via PLL; PCLK1 80 MHz.
- CAN: **bxCAN on CAN2** (PB12/PB13), 500 kbit/s; CAN2 filter banks live in CAN1's
  block, so both clocks are enabled. `shared/can` `can_if` is the only bxCAN code.
- Message code generated from `dbc/solarcar.dbc` by cantools at build time.
- lighting, steering_wheel: bare-metal super-loop (v1 targets). telemetry/bms:
  FreeRTOS, deferred.
- steering_wheel display: ST7565 (NHD C12864) via u8g2 over SPI1 (`HAL_SPI_Transmit`).
- HAL usage across boards (most → least): GPIO, NVIC/EXTI, CAN, RCC/PWR, TIM IRQ, SPI,
  ADC, DAC, I2C, USB PCD/CDC.
- Existing `tests/host` (Unity, HAL-free) stays as is.
- Left-turn input on steering_wheel is PC15; PB3 is SWO (don't use it in examples).
- `shared/app_common/sk_log.h` selects ITM vs stdout via `__ARM_ARCH`, which arm64
  hosts also define — the ITM model (or the host CMSIS-core) must handle this so logs
  appear on both macOS arm64 and Linux x86-64.
- lighting's lamp PWM is software-driven from a 20 kHz TIM7 interrupt
  (`app/lamp_pwm.c`).
- Sakura-2026-2027 BCM/PDU use STM32U3C5 with FDCAN; stubs today, not v1 SIL targets.

## 7. Left to the builder
- Register-interception mechanism (after the mandatory spike), runtime language split
  (C vs C++), control-channel transport and protocol.
- How the runtime takes control around the firmware's `main()`.
- Chip description format; Python package name; exact API names.
- Quantum sizes and per-access time costs (configurable defaults).
- How ST HAL sources are located/fetched.
- Extent of CAN error modelling beyond "doesn't lie".

## 8. Calls I made (not discussed)
- One process per node; scheduler in Python.
- Busy-wait protection via per-access time cost + max quantum.
- Wall-clock safety timeout in addition to virtual-time timeouts.
- Display as a reusable SPI-device model decoded to a framebuffer.
- CAN logs exportable in candump/ASC.
- Firmware is built by the consuming project's CMake, not by pytest.
- Sakura SIL tests in `tests/sil/` alongside `tests/host/`.


## 9. Open questions
None blocking. Resolved after review: telemetry deferred; compiler requirement and
speed target set (§4.3).
