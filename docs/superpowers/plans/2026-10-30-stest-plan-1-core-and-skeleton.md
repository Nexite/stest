# stest Plan 1 — Interception gate + walking skeleton

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove register-access interception meets the speed target, then build the thinnest end-to-end path: an unmodified CubeMX-style STM32L496 blinky (real `main.c`, real ST HAL) runs as a host process on virtual time, driven by a Python scheduler, observed and asserted from pytest — including multi-node lockstep and reading firmware variables by name.

**Architecture:** An LLVM pass plugin rewrites every `volatile` load/store in firmware + HAL code into calls to `__stest_mmio_{load,store}{8,16,32,64}`. The C++ runtime routes calls whose address falls in the STM32 peripheral/core windows to peripheral models; everything else is performed directly and charged a small virtual-time cost. Because peripheral addresses are never dereferenced, **ST's unmodified CMSIS device headers are used as-is** (real base addresses); the chip package only supplies a host replacement for the CMSIS compiler/intrinsics header. Each node is a process; Python owns the global clock and advances nodes in quanta over a Unix-domain-socket control channel.

**Tech Stack:** C11 (firmware), C++17 (runtime, pass), LLVM ≥ 18 pass-plugin API (Homebrew `llvm` on macOS, apt `llvm-dev`/`clang` on Linux), CMake ≥ 3.23 + Ninja, doctest (C++ tests), Python ≥ 3.11 (`tomllib`), pyelftools, pytest, uv.

**Spec:** `docs/superpowers/specs/2026-10-30-stest-sil-design.md`

**Deviation from spec §4.2 (builder's call, recorded):** the spec describes host device headers whose instance macros point at host memory. With compiler interception that is unnecessary: ST's own `stm32l4xx.h`/`stm32l496xx.h` are used unchanged; models are keyed by the real base addresses. The chip package still owns the host CMSIS-core compiler shim. If the gate (Task 2) fails and a trap-based fallback is needed, that fallback maps guard pages at the real addresses — same headers.

## Plan series

| Plan | Contents | Written |
|---|---|---|
| **1 (this)** | Interception pass + speed gate; runtime core; Cortex-M core models; L4 RCC/PWR/FLASH/GPIO; node process + control channel; Python Sim/scheduler; symbols; pytest plugin; CI | now |
| 2 | L4 EXTI/SYSCFG, TIM (base/IRQ/PWM state), SPI, ADC, ITM; HAL-mode shim mechanism (proved with GPIO `hal`); `MODELS` selection in `stest_add_firmware` | after gate |
| 3 | CAN: bus model in Python, bxCAN register model (CAN1+CAN2, filter banks in CAN1), virtual devices, DBC (cantools), arbitration, candump/ASC export, quantum ≤ bus lookahead | after 2 |
| 4 | STM32U3 family / U3C5RI: chip TOML, CMSIS shim reuse, RCC/GPIO basics, FDCAN (classic frames), cross-family CAN self-test | after 3 |
| 5 | Sakura: `sil` preset + build changes, SPI display (ST7565→framebuffer), `tests/sil/` suite, real-time check against lighting | after 4 |

## Global Constraints

- License **Apache-2.0** (`LICENSE` + `NOTICE`); vendored/fetched ST files keep their notices (CMSIS device: Apache-2.0; HAL: BSD-3-Clause).
- Host platforms: **native Linux (x86-64, arm64)** and **native macOS (arm64)**, both in CI (GitHub Actions). **Official Docker image** (`docker/Dockerfile`, Ubuntu 24.04 base, LLVM 18+) + `.devcontainer/` give the same toolchain anywhere, including Windows. No native Windows code paths.
- Firmware is compiled with **upstream LLVM clang** (not Apple clang, not gcc) with the stest pass plugin. Minimum LLVM 18.
- Firmware sources compile **unchanged**; the only allowed per-source intervention is compile flags (e.g. `-Dmain=stest_firmware_main`).
- **No Sakura names, pins, or DBC assumptions** anywhere in stest.
- **Strict by default:** any access to a peripheral/core address with no model, or a register a model does not implement, aborts the node with a message naming address, width, direction, and the peripheral (if known). No silent zeros.
- **Determinism:** identical inputs → byte-identical event logs. Nothing may depend on wall-clock time or host thread scheduling except the wall-clock safety timeout.
- **Speed target:** Sakura v1 suite ≥ real time; e.g. 2 nodes × 5 virtual s ≤ 5 s wall in CI. Per-node budget used for gating: **≤ 0.4 s wall per virtual second** under the lighting-like load (Task 2).
- Time unit everywhere: **nanoseconds, `uint64_t` / Python `int`**.
- Python package import name `stest` (PyPI name unresolved — do not publish in this plan).

## Review Focus

1. **Firmware hangs in a loop that touches no volatile memory** (`for(;;){}` or a non-volatile flag) → the wall-clock safety timeout fails the test naming the node and its virtual time; CI never hangs. *(test: Task 6 `test_wallclock_timeout_on_spinning_node`)*
2. **Firmware crashes (segfault/abort) mid-test** → `stest.NodeCrashed` with node name, signal/exit code, and last 20 lines of node stderr; all other node processes are terminated; no zombies. *(test: Task 6 `test_crash_reports_and_cleans_up`)*
3. **Access to an unmodelled peripheral address** (e.g. `USART1->CR1` before UART exists) → node aborts with `stest: unmodelled write32 0x40013800` and Python surfaces it verbatim. *(tests: Task 3 doctest, Task 6 `test_unmodelled_access_message`)*
4. **Interrupt raised while `__disable_irq()` / PRIMASK is set** → stays pending and fires immediately on `__enable_irq()`, never lost or delivered early. *(test: Task 4 doctest)*
5. **Same executable launched as two nodes / ambiguous static names** → independent state per node; `var("count")` that matches statics in two translation units raises `SymbolError` listing `file.c:count` candidates. *(tests: Task 6 `test_two_instances_independent`, Task 7 `test_ambiguous_static`)*

## File structure

```
LICENSE  NOTICE  README.md  CMakeLists.txt  pyproject.toml  .gitignore
cmake/
  StestToolchain.cmake        # locate upstream clang + plugin; error if Apple clang
  StestFetchST.cmake          # FetchContent: stm32l4xx-hal-driver, cmsis-device-l4, CMSIS core (pinned)
  stest_add_firmware.cmake    # public CMake function
compiler/mmio_pass/
  StestMmioPass.cpp  CMakeLists.txt
runtime/
  include/stest/
    mmio.h                    # extern "C" __stest_mmio_* + stest_rt_* C API for shims
    peripheral.hpp            # Peripheral base, RegisterFile helper
    bus.hpp                   # address map / dispatch
    clock.hpp                 # local virtual clock + event queue
    cortexm.hpp               # NVIC/SCB/SysTick models + IRQ dispatcher
    node.hpp                  # node lifecycle, firmware thread baton
    protocol.h                # control-channel wire format (shared with Python by spec)
  src/  mmio.cpp bus.cpp clock.cpp cortexm.cpp node.cpp node_main.cpp control_channel.cpp
  tests/ test_bus.cpp test_clock.cpp test_cortexm.cpp   (doctest)
chips/
  cmsis_host/cmsis_compiler.h # host intrinsics; shadows CMSIS's cmsis_compiler.h
  stm32l4/models/ rcc.cpp pwr.cpp flash.cpp gpio.cpp  (+ .hpp)
  stm32l4/parts/stm32l496rg.toml
tools/gen_chip.py             # TOML -> generated C++ (model instances + weak IRQ handlers)
selftest/firmware/
  blinky/  Core/Src/{main.c,stm32l4xx_it.c} Core/Inc/{main.h,stm32l4xx_hal_conf.h,stm32l4xx_it.h}
  faults/  main.c  (spin / crash / unmodelled-access variants selected by -D)
spike/
  mmio_order_test.c  bench_lighting_load.c  bench_sync.cpp  run_gate.py
python/stest/
  __init__.py  errors.py  protocol.py  sim.py  node.py  pin.py  symbols.py  macho.py  pytest_plugin.py
python/tests/
  conftest.py  test_scheduler.py  test_faults.py  test_symbols.py  test_plugin.py
docs/spikes/2026-10-30-interception-gate.md
docker/Dockerfile
.devcontainer/devcontainer.json
.github/workflows/ci.yml
```

---

### Task 1: Repo scaffold + MMIO interception pass

**Files:**
- Create: `LICENSE`, `NOTICE`, `README.md` (one-paragraph stub), `.gitignore`, `CMakeLists.txt`, `cmake/StestToolchain.cmake`, `docker/Dockerfile` (ubuntu:24.04; `clang-18 llvm-18-dev lld-18 cmake ninja-build python3 uv git`; multi-arch amd64/arm64), `.devcontainer/devcontainer.json` (builds that Dockerfile, mounts the workspace), `compiler/mmio_pass/{StestMmioPass.cpp,CMakeLists.txt}`, `spike/mmio_order_test.c`, `spike/CMakeLists.txt`

**Interfaces:**
- Produces: plugin target `stest_mmio_pass` (shared lib); CMake variable `STEST_CLANG` (path) and function `stest_enable_mmio(<target>)` adding `-fpass-plugin=$<TARGET_FILE:stest_mmio_pass>`.
- Produces (ABI the runtime implements, declared later in `runtime/include/stest/mmio.h`):
  ```c
  uint8_t  __stest_mmio_load8 (const volatile void *a);   void __stest_mmio_store8 (volatile void *a, uint8_t  v);
  uint16_t __stest_mmio_load16(const volatile void *a);   void __stest_mmio_store16(volatile void *a, uint16_t v);
  uint32_t __stest_mmio_load32(const volatile void *a);   void __stest_mmio_store32(volatile void *a, uint32_t v);
  uint64_t __stest_mmio_load64(const volatile void *a);   void __stest_mmio_store64(volatile void *a, uint64_t v);
  ```

- [ ] **Step 1: Write the failing test** — `spike/mmio_order_test.c` is compiled with the plugin and linked against a recording stub defined in the same test (stubs append `{op, addr, width, value}` to an array and return scripted values). `main()` asserts, and returns non-zero on failure:
  ```c
  #define GPIOA_BSRR ((volatile uint32_t *)0x48000018u)
  #define GPIOA_IDR  ((volatile uint32_t *)0x48000010u)
  #define SR         ((volatile uint16_t *)0x40006404u)
  static uint32_t plain;                 /* non-volatile: must NOT be instrumented */
  *GPIOA_BSRR = 0x20; *GPIOA_BSRR = 0x200000; plain = 7; uint32_t idr = *GPIOA_IDR; *SR = 0xFFFE;
  /* expect exactly 4 records in order: */
  /* {STORE,0x48000018,32,0x20} {STORE,0x48000018,32,0x200000} {LOAD,0x48000010,32} {STORE,0x40006404,16,0xFFFE} */
  /* idr == scripted value 0xA5; plain == 7 */
  ```
  Also: a `volatile float` store is recorded as a 32-bit store of its bit pattern; a `volatile uint8_t` RMW (`*p |= 1`) records LOAD8 then STORE8. Register with CTest as `mmio_order`.
- [ ] **Step 2: Configure/build without the pass wired in; run** `ctest --test-dir build -R mmio_order --output-on-failure` → FAIL (0 records).
- [ ] **Step 3: Implement** a new-PM pass plugin (`llvmGetPassPluginInfo`, registered at `registerOptimizerLastEPCallback` **and** `registerPipelineStartEPCallback` guarded so it runs once at `-O0` too) that replaces every volatile `LoadInst`/`StoreInst` of integer/float/pointer type ≤ 64 bits with the matching call, bitcasting non-integer values. Atomic volatile ops: leave untouched and emit a remark. `StestToolchain.cmake`: find `clang` from `STEST_LLVM_ROOT` / `brew --prefix llvm` / `llvm-config --bindir`; `FATAL_ERROR` if `clang --version` contains "Apple".
- [ ] **Step 4: Run** the same ctest at `-O0` and `-O2` builds, natively on macOS **and** inside the Docker image (`docker build -t stest-dev docker && docker run --rm -v $PWD:/w -w /w stest-dev ...`) → PASS in both.
- [ ] **Step 5: Commit** `feat: LLVM pass rewriting volatile accesses into stest MMIO calls`.

### Task 2: Speed gate (go / no-go)

**Files:**
- Create: `spike/bench_lighting_load.c`, `spike/bench_sync.cpp`, `spike/run_gate.py`, `docs/spikes/2026-10-30-interception-gate.md`

**Interfaces:**
- Consumes: `stest_mmio_pass`, ABI from Task 1. Bench supplies its own minimal `__stest_mmio_*` implementing the intended fast path (range check against `[0x40000000,0x60000000)` and `[0xE0000000,0xE0100000)`, model dispatch via a 64-entry page table to a trivial array-backed register store, `now_ns += 25` per access).

- [ ] **Step 1: Write `bench_lighting_load.c`** simulating **1 virtual second** of lighting-like load: 20,000 "ISR" invocations each doing 8 volatile peripheral accesses (TIM SR clear, GPIO BSRR ×4, reads), interleaved with a main loop doing 50 volatile accesses per 1 ms tick, and a `volatile uint32_t uwTick` incremented 1,000 times. Prints `mmio_s=<wall seconds>`.
- [ ] **Step 2: Write `bench_sync.cpp`**: parent/child over a `SOCK_STREAM` Unix socket doing 10,000 round trips of a 16-byte RUN_UNTIL / 16-byte STOPPED exchange (= 100 µs quantum, worst case for CAN). Prints `sync_s=<wall seconds>`.
- [ ] **Step 3: `run_gate.py`** builds both (Release), runs each 5×, takes the median, prints `total = mmio_s + sync_s` and `PASS` iff `total <= 0.40`, else `FAIL`.
- [ ] **Step 4: Run** natively on **macOS arm64** (`uv run spike/run_gate.py`) and in the **stest Docker image on arm64 Linux** (same Mac; Docker Desktop must be running). Linux **x86-64** numbers come from the first CI run (Task 8) — record them as pending until then; do not run x86 under emulation (timings meaningless). Record numbers, machine, and verdict in `docs/spikes/2026-10-30-interception-gate.md`.
- [ ] **Step 5: Gate.** macOS and arm64-Linux PASS → continue. Any FAIL → **stop and report to the user** with the numbers; do not start Task 3. (Spec §4.3: options are trap fallback or `hal` mode for affected peripherals on that platform.) If sync alone is the problem, record shared-memory + futex/`os_sync_wait_on_address` as the remedy and continue only with user approval.
- [ ] **Step 6: Commit** `spike: interception speed gate results`.

### Task 3: Runtime core — address map, register helper, clock

**Files:**
- Create: `runtime/include/stest/{mmio.h,peripheral.hpp,bus.hpp,clock.hpp}`, `runtime/src/{mmio.cpp,bus.cpp,clock.cpp}`, `runtime/CMakeLists.txt`, `runtime/tests/{test_bus.cpp,test_clock.cpp}`

**Interfaces:**
- Produces:
  ```cpp
  namespace stest {
  struct Peripheral {                       // one instance per mapped block
    virtual ~Peripheral() = default;
    virtual std::string_view name() const = 0;
    virtual uint32_t read(uint32_t off, unsigned width) = 0;      // width in bytes: 1,2,4
    virtual void     write(uint32_t off, unsigned width, uint32_t v) = 0;
    virtual void     reset() {}
    virtual nlohmann::json call(std::string_view op, const nlohmann::json& args); // default: throws UnknownOp
  };
  class RegisterFile { /* helper: declare(off, reset, rw_mask, w1c_mask); read/write apply masks;
                          undeclared offset -> throws UnmodelledRegister */ };
  class Bus { void map(uint64_t base, uint32_t size, Peripheral*);
              uint32_t read(uint64_t addr, unsigned width); void write(uint64_t addr, unsigned width, uint32_t v);
              /* unmapped in window -> throws UnmodelledAccess{addr,width,is_write} */ };
  class Clock { uint64_t now_ns() const; void advance(uint64_t ns);
                EventId schedule_at(uint64_t t_ns, std::function<void()>); void cancel(EventId);
                uint64_t next_event_ns() const;        // UINT64_MAX if none
                void run_due();                         // fires events with t <= now, in (t, insertion seq) order
  };
  }
  // mmio.h (extern "C"): __stest_mmio_* from Task 1, plus
  void     stest_rt_abort(const char *fmt, ...);        // prints "stest: ..." to stderr, exits 70
  uint64_t stest_rt_now_ns(void);
  ```
  `__stest_mmio_*`: in-window → `Bus` (exceptions caught at the C boundary → `stest_rt_abort("unmodelled write32 0x%08x (%s)", ...)`); out-of-window → direct host access. Every call charges `access_cost_ns` (default **25**, settable) to the `Clock` and then gives the IRQ dispatcher a delivery point (hook pointer, null until Task 4). On Linux, at startup reserve the two windows with `mmap(PROT_NONE, MAP_FIXED_NOREPLACE)` so no host object can live there; on macOS `__PAGEZERO` already covers them (assert at startup that host `&main` is ≥ 4 GiB).
- Dependencies: doctest and nlohmann/json via FetchContent (pinned tags).

- [ ] **Step 1: Write failing doctests**: RegisterFile W1C (`write 0x1` to a W1C bit clears it, other bits untouched), rw_mask (read-only bits ignore writes), undeclared offset throws `UnmodelledRegister`; Bus unmapped address throws `UnmodelledAccess` with `addr==0x40013800, width==4, is_write==true`; Clock fires two events at the same `t` in insertion order and `next_event_ns()` is `UINT64_MAX` when empty; a C-level test that calls `__stest_mmio_store32((void*)0x40013800, 1)` in a subprocess exits 70 with stderr containing `stest: unmodelled write32 0x40013800`.
- [ ] **Step 2: Run** `ctest --test-dir build -R runtime_ --output-on-failure` → FAIL (not implemented).
- [ ] **Step 3: Implement** per Interfaces.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(runtime): address map, register helper, virtual clock`.

### Task 4: Cortex-M core — NVIC, SCB, SysTick, intrinsics, IRQ dispatch

**Files:**
- Create: `runtime/include/stest/cortexm.hpp`, `runtime/src/cortexm.cpp`, `chips/cmsis_host/cmsis_compiler.h`, `runtime/tests/test_cortexm.cpp`

**Interfaces:**
- Consumes: `Peripheral`, `RegisterFile`, `Bus`, `Clock`.
- Produces:
  ```cpp
  namespace stest {
  using IrqHandler = void (*)(void);
  class Cortexm {   // owns NVIC (0xE000E100), SCB (0xE000ED00), SysTick (0xE000E010), maps them on the Bus
   public:
    Cortexm(Bus&, Clock&, unsigned prio_bits, const IrqHandler* irq_table, size_t n_irqs /* index = IRQn + 16 */);
    void set_pending(int irqn);            // peripherals call this
    void deliver();                        // delivery point: run highest eligible pending handler(s), nested by priority
    void set_primask(bool); bool primask() const; void set_basepri(uint32_t);
    void wfi();                            // advance Clock to min(next_event, quantum_end); then deliver()
    void set_quantum_end(uint64_t t_ns);
  };
  }
  ```
  SysTick counts against core clock read from `SystemCoreClock` (C global, via a registered getter) — reload/period from `LOAD`, fires exception −1 (`SysTick_Handler`) through a `Clock` event. NVIC: ISER/ICER/ISPR/ICPR/IABR/IPR; SCB: CPACR, VTOR, AIRCR (priority grouping), SHPR, ICSR (PENDSV/PENDST set/clear). Priority grouping honoured for preemption.
  `cmsis_compiler.h`: defines CMSIS `__STATIC_INLINE`, `__ASM` no-op, `__NOP/__DSB/__ISB/__DMB` (compiler barrier), `__WFI/__WFE` → `stest_rt_wfi()`, `__enable_irq/__disable_irq/__get_PRIMASK/__set_PRIMASK/__get_BASEPRI/__set_BASEPRI` → runtime C API, `__REV/__REV16/__RBIT/__CLZ` via builtins, `__get_MSP/__set_MSP` dummies. Placed before CMSIS's include dir so `core_cm4.h` picks it up; also `#undef __ARM_ARCH` handling deferred to Plan 2 (ITM).

- [ ] **Step 1: Write failing doctests**: (a) IRQ 5 enabled, `set_pending(5)` with PRIMASK=1 → handler count 0 after `deliver()`; `set_primask(false)` → count 1. (b) Higher-priority IRQ pended inside a lower-priority handler preempts (record order `[low_enter, high, low_exit]`). (c) SysTick with `SystemCoreClock=80'000'000`, `LOAD=79999`, ENABLE|TICKINT → after advancing to 10 ms, `SysTick_Handler` ran exactly 10 times. (d) Disabled IRQ pended stays in ISPR and fires on later enable. (e) `wfi()` with next event at 3 ms and quantum end at 5 ms leaves `now_ns()==3'000'000`.
- [ ] **Step 2: Run** `ctest -R runtime_cortexm` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(runtime): Cortex-M NVIC/SCB/SysTick and host CMSIS intrinsics`.

### Task 5: L4 chip package, `stest_add_firmware`, blinky runs standalone

**Files:**
- Create: `chips/stm32l4/models/{rcc,pwr,flash,gpio}.{hpp,cpp}`, `chips/stm32l4/parts/stm32l496rg.toml`, `tools/gen_chip.py`, `cmake/StestFetchST.cmake`, `cmake/stest_add_firmware.cmake`, `runtime/src/{node.cpp,node_main.cpp}`, `runtime/include/stest/node.hpp`, `selftest/firmware/blinky/**`, `selftest/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1, 3, 4.
- Produces:
  - Chip TOML schema:
    ```toml
    [chip]  name="STM32L496RG"  family="stm32l4"  core="cortex-m4"  nvic_prio_bits=4  device_define="STM32L496xx"
    [[peripheral]]  name="GPIOA"  model="stm32l4.gpio"  base=0x48000000  size=0x400   params={port="A"}
    [irq]  WWDG=0  # ... full L496 vector list, names without _IRQHandler; core exceptions implicit
    ```
  - `tools/gen_chip.py <toml> <out.cpp>` emits: model instances + `Bus::map` calls in `stest_chip_init(Bus&, Clock&, Cortexm&)`, the IRQ table, and a **weak default definition** for every handler (`__attribute__((weak)) void TIM7_IRQHandler(void){ stest_rt_abort("unhandled IRQ TIM7 (55)"); }`). Generated file is compiled as an object (not archived) so firmware strong definitions win on ELF and Mach-O.
  - Models: RCC (RegisterFile; writing HSEON/HSION/MSION/PLLON sets the matching RDY bit; `CFGR.SWS` mirrors `CFGR.SW`; all L496 registers declared with reset values), PWR (CR1 VOS, SR2.VOSF reads 0), FLASH (ACR latency read-back), GPIO (MODER/OTYPER/OSPEEDR/PUPDR/IDR/ODR/BSRR/LCKR/AFR/BRR; IDR = external drive if set else ODR for output pins else pull level; `call("get_pin"|"drive_pin"|"release_pin")`; records `(t_ns, port, pin, level)` change events into the node event buffer).
  - Node: `stest_firmware_main` renamed via `-Dmain=stest_firmware_main` on firmware sources; firmware runs on its own thread, runtime holds a baton so exactly one thread runs at a time; `node_main.cpp` provides `main()`. **Standalone mode** (no `STEST_CTRL` env): `--run-ms N` runs to N ms virtual and prints events as `EVT <t_ns> GPIO <port><pin> <level>` lines.
  - CMake:
    ```cmake
    stest_add_firmware(<name>
      CHIP <part>                 # e.g. STM32L496RG -> chips/*/parts/<lower>.toml
      SOURCES <files...>          # firmware sources incl. main.c, *_it.c, *_hal_msp.c
      INCLUDE_DIRS <dirs...>
      DEFINES <defs...>
      HAL_SOURCES <files...>      # optional; default: all HAL .c from fetched or STEST_<FAMILY>_HAL_DIR
    )
    ```
    Firmware + HAL + `system_stm32l4xx.c` compiled with `STEST_CLANG`, plugin, `-g`, `-O0`, `-D<device_define> -DUSE_HAL_DRIVER`; runtime linked uninstrumented. On macOS a POST_BUILD `dsymutil` runs.
  - `StestFetchST.cmake`: `STEST_STM32L4_HAL_DIR`, `STEST_STM32L4_CMSIS_DEVICE_DIR`, `STEST_CMSIS_CORE_DIR` cache vars; if unset, FetchContent pinned to the **same commits as Sakura's submodules** (read them with `git -C <Sakura>/firmware/Sakura submodule status` after `tools/bootstrap.sh`).
- Blinky firmware: CubeMX-style `main.c` with `HAL_Init()`, `SystemClock_Config()` (HSE bypass 8 MHz, PLLM=2 PLLN=40 PLLR=2 → 80 MHz, FLASH latency 4, VOS range 1), `MX_GPIO_Init()` (PA5 push-pull output), loop `HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5); blink_count++; HAL_Delay(500);` with `static volatile uint32_t blink_count;` at file scope; `stm32l4xx_it.c` with `SysTick_Handler(){ HAL_IncTick(); }`.

- [ ] **Step 1: Write failing CTest** `selftest_blinky_standalone`: runs `blinky --run-ms 2100`; a CMake script test asserts exactly 4 `GPIO A5` events at `t_ns` within ±1 ms of 500, 1000, 1500, 2000 ms (first edge low→high), and exit code 0.
- [ ] **Step 2: Run** → FAIL (no target).
- [ ] **Step 3: Implement** generator, models, node, CMake per Interfaces. Missing RCC/PWR/FLASH/SCB registers surface as strict-mode aborts — add them to the model (declared, with correct reset values) until blinky boots; never relax strictness.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(l4): STM32L496RG chip package and blinky self-test`.

### Task 6: Control channel, Python Sim, lockstep scheduler, fault handling

**Files:**
- Create: `runtime/include/stest/protocol.h`, `runtime/src/control_channel.cpp`, `pyproject.toml`, `python/stest/{__init__,errors,protocol,sim,node,pin}.py`, `python/tests/{conftest,test_scheduler,test_faults}.py`, `selftest/firmware/faults/main.c`
- Modify: `runtime/src/node_main.cpp` (control mode when `STEST_CTRL` set)

**Interfaces:**
- Wire format (little-endian; every message `u32 len | u8 type | payload`):

  | type | dir | payload |
  |---|---|---|
  | 1 HELLO | node→py | u32 proto_version=1, u64 anchor_addr (runtime address of `stest_symbol_anchor`) |
  | 2 RUN_UNTIL | py→node | u64 t_ns |
  | 3 STOPPED | node→py | u64 t_ns, u32 n_events, then events: u64 t_ns, u16 json_len, json |
  | 4 READ_MEM | py→node | u64 addr, u32 len → 5 MEM: bytes |
  | 6 WRITE_MEM | py→node | u64 addr, u32 len, bytes → 7 OK |
  | 8 MODEL_CALL | py→node | u16 len, json `{"model":"GPIOA","op":"drive_pin","args":{...}}` → 9 MODEL_RESULT json |
  | 10 ERROR | node→py | utf-8 message (then node exits 70) |
  | 11 SHUTDOWN | py→node | — |
  Events JSON: `{"kind":"gpio","port":"A","pin":5,"level":1}`.
- Python:
  ```python
  class Sim:
      def __init__(self, quantum_us: int = 1000, wall_timeout_s: float = 30.0, access_cost_ns: int = 25): ...
      def node(self, name: str, exe: str | os.PathLike, env: dict | None = None) -> Node
      def start(self) -> None
      @property
      def now_ns(self) -> int
      def advance(self, *, ms: float = 0, us: float = 0, ns: int = 0) -> None
      def wait_for(self, cond: Callable[[], bool], timeout_ms: float) -> None   # checks cond at every quantum boundary
      def close(self) -> None        # SHUTDOWN, then SIGKILL after 2 s; also __enter__/__exit__
  class Node:
      name: str
      def pin(self, name: str) -> Pin           # "PA5"
      def call(self, model: str, op: str, **args) -> Any
      def read_mem(self, addr: int, n: int) -> bytes; def write_mem(self, addr: int, data: bytes) -> None
      stderr_tail: list[str]
  class Pin:
      level: int                                 # current, via MODEL_CALL get_pin
      history: list[tuple[int, int]]             # (t_ns, level) from events
      def set_high(self) -> None; def set_low(self) -> None; def release(self) -> None
      def wait_high(self, timeout_ms: float) -> None; def wait_low(self, timeout_ms: float) -> None
  # errors.py
  class StestError(Exception); class NodeCrashed(StestError); class WaitTimeout(StestError)
  class WallClockTimeout(StestError); class SymbolError(StestError)
  ```
  Scheduler: each quantum, send RUN_UNTIL(t+q) to **all** nodes, then read STOPPED from each in node-creation order; merge events sorted by `(t_ns, node_index, seq)`. Nodes run concurrently between sync points. `NodeCrashed` message includes node name, exit status/signal, and last 20 stderr lines; on any crash/timeout every node is killed and reaped. Wall-clock timeout covers each blocking read.
- Fault firmware (`faults/main.c`, one exe per `-DFAULT=SPIN|CRASH|UNMODELLED`): SPIN = `for(;;){}` with no volatile access after 10 ms; CRASH = null deref at 10 ms; UNMODELLED = `*(volatile uint32_t*)0x40013800 = 1` at 10 ms.

- [ ] **Step 1: Write failing pytest tests**:
  ```python
  def test_blinky_edges_on_virtual_time(sim, blinky_exe):
      n = sim.node("b", blinky_exe); sim.start(); sim.advance(ms=2100)
      assert [round(t/1e6) for t, _ in n.pin("PA5").history] == [500, 1000, 1500, 2000]
  def test_two_nodes_lockstep_deterministic(blinky_exe):   # run twice, compare merged event logs
      assert run_two_nodes_log(blinky_exe) == run_two_nodes_log(blinky_exe)
  def test_two_instances_independent(sim, blinky_exe):
      a = sim.node("a", blinky_exe); b = sim.node("b", blinky_exe); sim.start()
      a.call("GPIOA", "drive_pin", pin=0, level=1); sim.advance(ms=1)
      assert a.call("GPIOA", "get_pin", pin=0) == 1 and b.call("GPIOA", "get_pin", pin=0) == 0
  def test_wait_for_times_out_in_virtual_time(sim, blinky_exe): ...  # WaitTimeout; sim.now_ns ≈ timeout
  def test_wallclock_timeout_on_spinning_node(faults_exe):          # Sim(wall_timeout_s=2), SPIN
      with pytest.raises(WallClockTimeout, match=r"node 'spin'.*virtual t=10\.\d+ ms"): ...
  def test_crash_reports_and_cleans_up(faults_exe):                  # CRASH; NodeCrashed names node + signal; no child pids left
  def test_unmodelled_access_message(faults_exe):
      with pytest.raises(NodeCrashed, match=r"stest: unmodelled write32 0x40013800"): ...
  ```
  Fixtures in `python/tests/conftest.py` locate exes from `STEST_SELFTEST_BIN` (set by CTest/CI) and build a fresh `Sim` per test.
- [ ] **Step 2: Run** `uv run pytest python/tests -q` → FAIL.
- [ ] **Step 3: Implement** protocol, control mode, Python per Interfaces.
- [ ] **Step 4: Run** → PASS; also re-run Task 5's CTest → still PASS.
- [ ] **Step 5: Commit** `feat: control channel and Python lockstep scheduler`.

### Task 7: Firmware variables by name

**Files:**
- Create: `python/stest/{symbols.py,macho.py}`, `python/tests/test_symbols.py`, `selftest/firmware/blinky/Core/Src/vars.c` (test globals)
- Modify: `python/stest/node.py` (add `var`, `set_var`), `runtime/src/node_main.cpp` (define `stest_symbol_anchor`)

**Interfaces:**
- Consumes: HELLO `anchor_addr`, READ_MEM/WRITE_MEM.
- Produces:
  ```python
  class Node:
      def var(self, expr: str) -> Any          # "g_cfg.lamps[2].duty", "count", "vars.c:count", "func::s_local"
      def set_var(self, expr: str, value: Any) -> None
  class SymbolTable:
      @classmethod
      def load(cls, exe: Path) -> "SymbolTable"  # ELF via pyelftools; Mach-O (macOS): DWARF from <exe>.dSYM sections via macho.py
      def resolve(self, expr: str) -> Resolved   # static address + type
  ```
  Runtime address = static address + (`anchor_addr` − static address of `stest_symbol_anchor`). Values: integers (signedness/width from DWARF), floats, `bool`, enums → `str` enumerator name (comparison with int also works via an `IntEnum`-like wrapper), structs → read-only dict-like, arrays → list. `set_var` accepts int/float/enum name. Function-local statics addressed as `"func::name"`. Only allowed while the sim is paused (always true between Python calls).
- `vars.c` defines: `uint8_t u8 = 200; int16_t s16 = -5; float f = 1.5f; enum mode {MODE_OFF, MODE_ON} mode = MODE_ON; struct lamp {uint16_t duty; bool on;} lamps[3] = {{10,1},{20,0},{30,1}}; static int count = 1;` and a function with `static int calls;`. A second file defines another `static int count = 2;`.

- [ ] **Step 1: Write failing tests**: `var("u8")==200`, `var("s16")==-5`, `var("f")==1.5`, `var("mode")=="MODE_ON"` and `==1`, `var("lamps[1].duty")==20`, `var("lamps[2]")["on"] is True`, `var("tick_fn::calls")` increments across `advance`, `set_var("lamps[0].duty", 99)` then `var` reads 99 and firmware observes it; `test_ambiguous_static`: `var("count")` raises `SymbolError` whose message lists both `vars.c:count` and `<other>.c:count`, and `var("vars.c:count")==1`; unknown name raises `SymbolError("no symbol 'nope'")`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.** macOS: parse the `.dSYM` Mach-O `__DWARF` segment, hand sections to `pyelftools.dwarf.DWARFInfo` (DWARF 4 and 5).
- [ ] **Step 4: Run** → PASS natively on macOS and inside the Docker image (Linux/ELF path).
- [ ] **Step 5: Commit** `feat(python): read/write firmware variables by name`.

### Task 8: pytest plugin, CI, getting-started docs

**Files:**
- Create: `python/stest/pytest_plugin.py`, `python/tests/test_plugin.py`, `.github/workflows/ci.yml`
- Modify: `pyproject.toml` (entry point `pytest11 = { stest = "stest.pytest_plugin" }`), `README.md` (install, build blinky, run tests)

**Interfaces:**
- Produces: fixture `sim` (function-scoped `Sim`, closed on teardown; options `--stest-wall-timeout` default 30 s, `--stest-quantum-us` default 1000); on test failure attaches each node's last 200 stderr lines and merged event log to the report (`item.add_report_section`); marker `@pytest.mark.stest(quantum_us=..., wall_timeout_s=...)` overrides per test.

- [ ] **Step 1: Write failing tests** with `pytester`: a failing test using `sim` + blinky shows a "stest node b stderr" section and an "stest events" section in output; marker override visible via `sim.quantum_us`; teardown leaves no child processes even when the test raises.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** plugin; CI workflow, matrix: (a) `ubuntu-24.04` x86-64 **inside the stest Docker image** (built in the workflow), (b) `macos-14` arm64 native with `brew install llvm ninja`. Each runs CMake configure/build, `ctest`, `uv run pytest`, and `uv run spike/run_gate.py` (prints numbers; x86-64 result is copied into the gate doc; non-blocking thereafter). The workflow builds the image but does not publish it; publishing is a later decision.
- [ ] **Step 4: Run** locally `ctest --test-dir build && uv run pytest -q` natively on macOS and inside the Docker image → all PASS in both. CI runs when the user pushes.
- [ ] **Step 5: Commit** `feat: pytest plugin, CI, getting-started docs`.
