# AGENTS.md

Instructions for coding agents working in this repo. Read this first; the design spec is the authority when in doubt.

## What this is

stest is an open-source (Apache-2.0) STM32 software-in-the-loop simulator. Real firmware plus ST's unmodified HAL compile natively with upstream clang and an LLVM pass (`compiler/mmio_pass`) that turns volatile accesses into `__stest_mmio_*` calls. A C++ runtime (`runtime/`) routes them to peripheral models (`chips/`) on virtual time (ns). Python (`python/stest`) drives node processes over a socket protocol and ships a pytest plugin (`sim` fixture).

## Repo map

| Path | Responsibility |
|---|---|
| `compiler/mmio_pass/` | LLVM pass plugin (`StestMmioPass.cpp`) rewriting volatile loads/stores |
| `runtime/include/stest/`, `runtime/src/` | Bus, Clock, Cortexm core (NVIC/SysTick/wfi), MMIO entry points, Node, control channel, `protocol.h` |
| `runtime/tests/` | doctest unit tests + abort/e2e helpers (CTest `runtime_*`) |
| `chips/<family>/models/`, `parts/*.toml` | Peripheral models (C++) and per-part chip descriptions (`stm32l4/parts/stm32l496rg.toml`) |
| `chips/cmsis_host/` | Host shim for CMSIS `cmsis_compiler.h` (replaces ARM toolchain header) |
| `tools/gen_chip.py` | TOML -> generated chip glue (IRQ table, weak handlers, model instances, `stest_chip`) |
| `cmake/` | `StestToolchain.cmake` (finds upstream LLVM), `StestFetchST.cmake` (pinned ST sources), `stest_add_firmware.cmake` |
| `selftest/` | Self-test firmwares (`firmware/blinky`, `faults`, `pollcount`) and CTest checks |
| `spike/` | Interception benchmarks and the speed gate (`run_gate.py`) |
| `python/stest/`, `python/tests/` | `Sim`/`Node`, protocol mirror, DWARF/Mach-O symbol lookup, typed peripheral helpers (`peripherals.py`: `node.timer()`, `node.adc()`, `node.dac()`; `pin.py`: levels and analog voltages), pytest plugin; Python tests |
| `docs/superpowers/{specs,plans}/` | Design spec and per-plan implementation plans; `docs/spikes/` gate results |
| `docker/Dockerfile`, `.devcontainer/` | Ubuntu 24.04 + LLVM 18 image (also the zero-setup environment) |

## How it works

Build time:
- `stest_add_firmware(name CHIP <part> SOURCES ... [INCLUDE_DIRS] [DEFINES] [HAL_SOURCES])` builds firmware + HAL with `STEST_CLANG` + the pass, `-g -O0`. Firmware sources (only) get `-Dmain=stest_firmware_main`; the runtime's `node_main.cpp` provides the real `main`.
- `tools/gen_chip.py` generates per-part glue from the TOML. Model `<family>.<kind>` maps to class `stest::<family>::<Kind>` in `chips/<family>/models/<kind>.hpp`, constructed `(name, params-json, ModelContext&)`; the generated init then calls `ctx.add`, `set_clock_tree`, `connect()` on each, `Bus::map` and `Bus::set_gate` (see `runtime/include/stest/model_context.hpp`).
- ST sources (L4 HAL, CMSIS device, CMSIS-Core) are fetched at pinned commits with SHA-256 (`cmake/StestFetchST.cmake`). Override with `-DSTEST_STM32L4_HAL_DIR=`, `-DSTEST_STM32L4_CMSIS_DEVICE_DIR=`, `-DSTEST_CMSIS_CORE_DIR=`. CMSIS-Core is staged without its toolchain headers so the host shim wins.

Run time:
- One node = one process = one MCU. The firmware runs on its own thread; the thread owning the Node (loop thread) holds the "baton". Exactly one runs at a time (mutex + condvar handoff), so all model/clock/core access is single-threaded. See `runtime/include/stest/node.hpp`.
- Every MMIO access goes `mmio.cpp` -> `Bus` -> model, charging virtual time. Models schedule `Clock` events; IRQ handlers and events run on the firmware thread.
- Control channel (`runtime/include/stest/protocol.h`): framed messages (`u32 len | u8 type | payload`) over the socket fd in `STEST_CTRL`: HELLO, RUN_UNTIL, STOPPED (+events), READ_MEM/WRITE_MEM, MODEL_CALL, ERROR (node exits 70), SHUTDOWN, REQ_ERROR. Without `STEST_CTRL` the node runs standalone (`--run-ms N`).
- Python `Sim` advances all nodes in lockstep quanta (default 1000 us). Firmware variables are read/written by name via DWARF (`python/stest/symbols.py`, anchored at the `stest_symbol_anchor` runtime address; Mach-O via `macho.py`).
- Poll fast-forward (`runtime.hpp`): after 64 store-free loads re-reading seen (addr,width,value) tuples, the clock jumps to the next event or quantum end.

## Setup and commands

Requirements: upstream LLVM >= 18 (never Apple clang), CMake >= 3.23, Ninja, Python >= 3.11, uv.

```sh
# macOS
brew install llvm cmake ninja uv
# Ubuntu 24.04
apt install clang-18 llvm-18-dev lld-18 cmake ninja-build && export STEST_LLVM_ROOT=/usr/lib/llvm-18

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # add -DSTEST_LLVM_ROOT=<prefix> if LLVM not found
cmake --build build
ctest --test-dir build --output-on-failure                 # includes python_tests (uv, per-build-dir venv)
uv run pytest python/tests -q                              # Python alone; uses build/selftest or $STEST_SELFTEST_BIN
./build/selftest/blinky --run-ms 2100                      # standalone firmware, prints EVT lines
uv run spike/run_gate.py                                   # speed gate (builds build-gate/ itself)
```

Docker, exactly as CI runs it (`.github/workflows/ci.yml`):

```sh
docker build -t stest-ci docker
docker run --rm -v "$PWD":/w -w /w stest-ci bash -euxo pipefail -c '
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ctest --test-dir build --output-on-failure
  uv run pytest python/tests -q
'
docker run --rm -v "$PWD":/w -w /w stest-ci bash -c 'uv run spike/run_gate.py'
```

Note the Docker build writes into the host `build/`; if you share the tree with a native build, use a separate dir (e.g. `build-docker`, as the README does). `build*/` is gitignored.

## Non-negotiable rules

- **Firmware and ST sources compile unchanged.** Never patch ST code or require firmware edits. HAL-mode shims (Plan 2) replace whole ST `.c` files at link time.
- **Strict by default.** Unmodelled address, unimplemented register, write to a fully read-only register, or access to a clock-disabled peripheral (Plan 2) aborts the node with an `stest: ...` message naming address, width, direction and peripheral (exit 70, ERROR frame). Never return silent zeros or drop writes. Registers that merely contain RO bits keep hardware semantics.
- **Determinism.** No wall clock or host scheduling in simulation logic; identical inputs (including quantum size) give byte-identical event logs. Changing the quantum may shift times by less than one access cost, and changes the timing of loops dilated by poll fast-forward. (Wall-clock use is limited to timeouts/watchdogs.)
- **Model contract** (`peripheral.hpp`): any time-driven state change is a scheduled `Clock` event, or visible as a different value on each read (like SysTick VAL). Lazily computed "ready at T" state without an event at T is overshot by poll fast-forward.
- **No Sakura** (the first user project) names, pins or DBC assumptions anywhere in stest. Sakura tests live in Sakura's repo.
- **Licensing.** Apache-2.0; ST files keep their own notices (see `NOTICE`, `LICENSE`).
- **Speed budget:** <= 0.4 s wall per virtual second per node (`selftest_blinky_speed`, `spike/run_gate.py`). Keep the MMIO hot path allocation-free.

## Change together

- `runtime/include/stest/protocol.h` <-> `python/stest/protocol.py`: byte-for-byte mirror. Bump `STEST_PROTO_VERSION` / `PROTO_VERSION` on any wire change, update the header comment table, and add a Python test.
- `chips/*/parts/*.toml` <-> `tools/gen_chip.py` (schema/validation) <-> model class names/ctors. `stest_add_firmware` re-runs the generator when either changes.
- Model added: its source in `chips/<family>/CMakeLists.txt`, a TOML `[[peripheral]]` entry, and tests.
- HAL shims <-> `family.cmake` (Plan 2: neither exists yet; check `docs/superpowers/plans/2026-10-31-stest-plan-2-l4-peripherals.md` Task 9).
- New `stest_add_firmware` options: update the header comment in `cmake/stest_add_firmware.cmake` and README.

## Adding a peripheral model (current state)

`docs/adding-a-peripheral.md` will hold the full guide (Plan 2). Follow the existing L4 models (`chips/stm32l4/models/{rcc,pwr,flash,gpio}.{hpp,cpp}`):
1. Create `chips/<family>/models/<kind>.{hpp,cpp}`: class `stest::<family>::<Kind>` deriving `stest::Peripheral`, ctor `(name, params-json, ModelContext& ctx)` (use `ctx.clock()`, `ctx.core()`; do not look up other models here); implement `name/read/write/reset`, and `call()` for Python-facing ops (throw `UnknownOp` otherwise).
2. Cross-model wiring goes in `connect(ModelContext&)`, called once every model exists: `ctx.get<OtherModel>("NAME")` (wrong type/missing -> `std::logic_error`), `ctx.clocks()` (the RCC's `ClockTree`: SYSCLK/HCLK/PCLKx/PLLSAIxR/ADCSEL, computed live from RCC registers; never cache a frequency), and `Signal<T>::subscribe` for synchronous notifications.
3. Use `RegisterFile`: `declare(off, reset, rw_mask, w1c_mask)`; `declare_write_only` for BSRR/KEYR-style registers; rw=w1c=0 makes a register read-only (writes abort). Layouts and reset values follow RM0351 and ST's `stm32l496xx.h`.
4. Time-driven behaviour: schedule `Clock` events (model contract above); derive counters from virtual time and schedule only the next instant a flag can change (see `tim.hpp`: no per-tick events, nothing scheduled while flags are already set). Emit observable changes with `emit_event`. Flag-driven interrupts are levels: `Cortexm::set_irq_level`, or `ModelContext::set_irq_source(irqn, &tag, high)` when the vector is shared (TIM1_UP_TIM16, TIM6_DAC, ...; the line is the OR of its sources). Never `set_pending` for a status flag.
5. Add the `.cpp` to `chips/stm32l4/CMakeLists.txt`, doctest cases to `chips/stm32l4/tests/`, a `[[peripheral]]` entry (`name`, `model = "stm32l4.<kind>"`, `base`, `size`, optional `params`, and `rcc_enable = { reg = "APB2ENR", bit = n }` if RCC gates its clock; `reg` must be listed in `[clock_tree] enable_regs`). Gated peripherals abort on any access while the bit is clear: `access to <NAME> while its clock is disabled (RCC <REG> bit <n>) (<read|write><bits> 0x%08x)`.
6. Add a Python/e2e test and, if firmware behaviour matters, a self-test firmware (enable the peripheral's RCC clock first).

## Adding a chip / part

1. `chips/<family>/parts/<part>.toml`: `[chip]` (name, family, core = "cortex-m4", nvic_prio_bits, device_define, system_init, systick_external_divider), `[[peripheral]]` entries, IRQ list (base addresses and IRQ numbers from the CMSIS device header). Run `python3 tools/gen_chip.py --info <toml>` and generate to a scratch file to validate.
2. New family: `chips/<family>/CMakeLists.txt` (library `stest_chip_<family>`, test targets), `add_subdirectory` in `chips/CMakeLists.txt`, ST pins and a family branch in `stest_st_family_dirs` (`StestFetchST.cmake`), plus a generator test like `chips/stm32l4/tests/check_gen.cmake`.
3. Add a self-test firmware under `selftest/` using `stest_add_firmware(... CHIP <PART> ...)`.

## Testing conventions

- TDD: write the failing test first. C++ uses doctest; CTest names are prefixed `runtime_`, `chip_`, `selftest_` (and `python_tests`). Python and e2e use pytest (`python/tests/`, `pytester` for plugin tests).
- Self-test firmwares live in `selftest/firmware/*`; fault injection is one exe per variant (`faults_spin|crash|unmodelled|ro_write|clkoff`).
- Every CTest has a `TIMEOUT` (30 s for runtime/selftests, 300 for `python_tests`); set one on new tests.
- Before claiming done, run the full suite natively AND in Docker (Linux-only paths like `PR_SET_PDEATHSIG` and `/proc` differ from macOS).
- Do not match on raw JSON key order in event logs (keys are sorted).

## Known gotchas

- Apple clang is rejected; use Homebrew/upstream LLVM. `stest_add_firmware` fails at configure if the C compiler is not `STEST_CLANG`, or clang and LLVM majors differ.
- macOS: a cctools `ld` earlier in PATH (Anaconda) breaks plugin bundles and DWARF; the toolchain forces Xcode's `ld` via `xcrun` (`STEST_XCODE_LD`). dSYM is generated by a POST_BUILD `dsymutil`.
- Do not put CMSIS-Core headers (`core_cm*.h`, `cmsis_gcc.h`, `cmsis_compiler.h`) in `INCLUDE_DIRS`; configure fails by design.
- `HAL_Delay(n)` waits n+1 ticks (ST adds one); blinky uses `HAL_Delay(499)` so edges land at 500 ms, 1000 ms...
- Analog: pin voltages live in the GPIO models (`Pin.set_voltage`, DAC outputs); the ADC resolves channels to pins through the part TOML's `[[analog]]` table (injected into the ADC's params by `gen_chip.py`). An unset input reads 0 V with one stderr warning per (ADC, channel), or aborts with `STEST_STRICT_ANALOG=1` / `Sim(strict_analog=True)` / `--stest-strict-analog` / `@pytest.mark.stest(strict_analog=True)`. MODEL_CALL (Python) bypasses RCC clock gating by design.
- Poll fast-forward dilates loops that count iterations over constant volatile reads (`while (--n && !(REG & FLAG));`). Disable with `STEST_POLL_SKIP=0`, `Sim(poll_skip=0)`, `--stest-poll-skip 0` or `@pytest.mark.stest(poll_skip=0)`.
- Zombie nodes: in a container where pytest is PID 1 (as in CI's `docker run ... bash -c '...; uv run pytest'`), killed orphans are never reaped; `python/tests/test_plugin.py` checks `/proc/<pid>/stat` for state `Z`. Keep that in mind when writing process-lifetime tests.
- Nodes exit when their parent dies: Python passes `STEST_PARENT_PID`; the runtime uses `PR_SET_PDEATHSIG` (Linux) plus a getppid watchdog thread. Do not use `preexec_fn` for this. macOS SIGTERM bypasses `atexit`.
- CMake word-wraps `message()` text by path length; tests matching error text must collapse whitespace first (`selftest/check_configure_fails.cmake`).
- Firmware `main` returning in control mode is a strict abort (ERROR, exit 70). A bad Python request (oversized or invalid READ_MEM/WRITE_MEM, unknown model/op) is a REQ_ERROR: Python raises `StestError` and the node keeps running.
- Only one Node per process (bus/clock are process-global).
- Plan 1 deferred minor findings (pass vs. volatile `memcpy`/`memset`, `RegisterFile` using `std::map`, etc.) are listed in `docs/superpowers/plans/2026-10-30-plan-1-followups.md`; check it before assuming a behaviour is a regression.

## Workflow

- Specs and plans live in `docs/superpowers/` (`specs/`, `plans/`, one plan per milestone, plus `...-followups.md`). One branch per plan (`plan-1-core`, `plan-2-peripherals`); the current work follows `docs/superpowers/plans/2026-10-31-stest-plan-2-l4-peripherals.md`. Record decisions and deferred findings in the plan's follow-ups file.
- Commits use a conventional prefix (`feat`, `fix`, `fix(runtime):`, `docs:`, `docs(plan-N):`) and a body explaining why. Every commit in history ends with a `Co-Authored-By: <model name> <noreply@anthropic.com>` trailer naming the model that did the work; use the attribution line your harness instructs.
