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
| `chips/cortexm/` | Core-generic ITM, DWT and CoreDebug (DHCSR/DEMCR) models; compiled into `stest_runtime` and mapped by `Cortexm` for every chip |
| `chips/<family>/models/`, `parts/*.toml` | Peripheral models (C++) and per-part chip descriptions (`stm32l4/parts/stm32l496rg.toml`); L4 bxCAN (CAN1 master, CAN2 slave) is `stm32l4/models/bxcan.{hpp,cpp}` |
| `chips/<family>/family.cmake`, `chips/<family>/hal/` | HAL-mode shims (`MODELS <TYPE>=hal`) replacing whole ST HAL files (L4: GPIO) |
| `chips/cmsis_host/` | Host shim for CMSIS `cmsis_compiler.h` (replaces ARM toolchain header) |
| `tools/gen_chip.py` | TOML -> generated chip glue (IRQ table, weak handlers, model instances, `stest_chip`) |
| `cmake/` | `StestToolchain.cmake` (finds upstream LLVM), `StestFetchST.cmake` (pinned ST sources), `stest_add_firmware.cmake` |
| `selftest/` | Self-test firmwares (`firmware/`: `adc`, `blinky`, `can` (exes `can_node1`, `can_node2`, `can_badrate` (250 kbit/s), `can_err` (error notifications + CAN2_SCE), `can_abom` (`can_err` + AutoBusOff)), `dac`, `exti` (+ `exti_wfe`: ST's WFE sleep entry), `faults` (variants incl. `faults_abort_output`, `faults_abort_events`), `hal_direct`, `itm` (+ `itm_mixed`: printf and ITM interleaved), `pollcount`, `pollpin`, `spi`, `tim`, `wfiloop` (WFI quantum-independence)), CTest checks, and `out_of_tree/` consumer projects for the add_subdirectory checks |
| `examples/sensor-node/` | Example consumer project (what a user's test repo looks like): two CubeMX-style L496 boards (`sensor`: PC13 button/EXTI -> PA5 LED, PC0 ADC1_IN1 sent every 100 ms on CAN1 as DBC `SensorReading` 0x120; `display`: receives it, PA5 on above 2000 mV), `can/sensor-node.dbc`, pytest tests, its own `pyproject.toml` (`stest[dbc]` by path). CI job `example` builds and tests it from a fresh build dir and venv with no FetchContent overrides (Linux in the Docker image, macOS native); the docs quote its files, so keep paths stable |
| `spike/` | Interception benchmarks and the speed gate (`run_gate.py [--quantum-us 88] [--can-two-node] [--can-worst-case] [--build-dir DIR]`; `gate_can_two_node.py`: the gated two-node CAN row, budget 2 x 0.40; `gate_can_noack.py`: the informational CAN no-ACK worst case) |
| `python/stest/`, `python/tests/` | `Sim`/`Node`, protocol mirror, DWARF/Mach-O symbol lookup, typed peripheral helpers (`peripherals.py`: `node.timer()`, `node.adc()`, `node.dac()`, `node.spi()`; `pin.py`: levels and analog voltages), pytest plugin; Python tests |
| `python/stest/can/` | Simulated CAN (user guide `docs/can.md`): `bus.py` bus engine (arbitration, ACK, error frames, fault confinement, `inject_errors`, `errors`; algorithm in its module doc), `frame.py` (`CanFrame`, bit length, `lookahead_ns`), `device.py` (`CanDevice`), `dbc.py` (`Dbc`, `DbcDevice`, via cantools), `log.py` (candump / ASC) |
| `examples/quickstart/` | The Quickstart test (`test_blinky.py`, run from the repo root against `build/selftest/blinky`); README and `docs/quickstart.md` quote its `test` snippet section |
| `mkdocs.yml`, `docs/` | User docs site (MkDocs Material + mkdocstrings, `https://nexite.github.io/stest/`): `index.md`, `install.md`, `quickstart.md`, `concepts.md`, `can.md`, `troubleshooting.md`, `contributing.md`, contributor guides `adding-a-{peripheral,chip}.md`, `guide/` (`getting-started.md`, `writing-tests/*`), `reference/` (`python/*` generated from docstrings, `pytest.md`, `firmware.md`, `support.md`). `docs/superpowers/` and `docs/spikes/` are excluded from the site (`exclude_docs`). README is the short front door and links into the site by URL |
| `docs/superpowers/{specs,plans}/` | Design spec and per-plan implementation plans; `docs/spikes/` gate results (neither is on the site) |
| `docker/Dockerfile`, `.devcontainer/` | Ubuntu 24.04 + LLVM 18 image (also the zero-setup environment) |
| `.github/workflows/` | `ci.yml` (Linux in the Docker image, macOS native: build, ctest, pytest, the Quickstart commands, non-blocking speed gates; job `example` builds and tests `examples/sensor-node`), `docs.yml` (`mkdocs build --strict` on every PR, GitHub Pages deploy from `main`) |

## How it works

Build time:
- `stest_add_firmware(name CHIP <part> SOURCES ... [INCLUDE_DIRS] [DEFINES] [HAL_SOURCES] [MODELS <TYPE>=register|hal ...])` builds firmware + HAL with `STEST_CLANG` + the pass, `-g -O0`. Firmware sources (only) get `-Dmain=stest_firmware_main`; the runtime's `node_main.cpp` provides the real `main`.
- `tools/gen_chip.py` generates per-part glue from the TOML. Model `<family>.<kind>` maps to class `stest::<family>::<Kind>` in `chips/<family>/models/<kind>.hpp`, constructed `(name, params-json, ModelContext&)`; the generated init then calls `ctx.add`, `set_clock_tree`, `connect()` on each, `Bus::map` and `Bus::set_gate` (see `runtime/include/stest/model_context.hpp`).
- `MODELS <TYPE>=hal` (TYPE = model kind upper-cased): `chips/<family>/family.cmake` lists the types with a HAL shim (`STEST_<FAMILY>_HAL_SHIMS`, `..._HAL_SHIM_<TYPE>_REPLACES/_SOURCE`); the shim (`chips/<family>/hal/*_shim.c`) replaces that whole ST file in the HAL archive, is compiled with the pass like the HAL, and calls the model's C API (`runtime/include/stest/hal_api.h`, implemented in the model; each call is accounted as the register accesses ST's code makes via `stest::shim_access`, so event logs match register mode). The node learns the hal types from the generated chip glue (`gen_chip.py --hal GPIO` emits `Bus::set_hal_owned`): firmware MMIO to a hal-owned block works but warns once per (peripheral, register). Only GPIO has a shim (L4).
- ST sources (L4 HAL, CMSIS device, CMSIS-Core) are fetched at pinned commits with SHA-256 (`cmake/StestFetchST.cmake`). Override with `-DSTEST_STM32L4_HAL_DIR=`, `-DSTEST_STM32L4_CMSIS_DEVICE_DIR=`, `-DSTEST_CMSIS_CORE_DIR=`. CMSIS-Core is staged without its toolchain headers so the host shim wins.

Run time:
- One node = one process = one MCU. The firmware runs on its own thread; the thread owning the Node (loop thread) holds the "baton". Exactly one runs at a time (mutex + condvar handoff), so all model/clock/core access is single-threaded. See `runtime/include/stest/node.hpp`.
- Every MMIO access goes `mmio.cpp` -> `Bus` -> model, charging virtual time. Models schedule `Clock` events; IRQ handlers and events run on the firmware thread.
- Control channel (`runtime/include/stest/protocol.h`, version 2): framed messages (`u32 len | u8 type | payload`) over the socket fd in `STEST_CTRL`: HELLO, RUN_UNTIL (+inbox), STOPPED (+events), READ_MEM/WRITE_MEM, MODEL_CALL, ERROR (+events; node exits 70), SHUTDOWN, REQ_ERROR. RUN_UNTIL's inbox carries `Node.post(model, op, args, t_ns)` messages, applied in order before the node runs via `Peripheral::deliver` (which schedules Clock events at `t_ns`); an inbox message the node cannot apply (unknown model/op, bad args, `t_ns` before node now) is a strict abort. ERROR carries the aborting quantum's pending events plus the unterminated ITM/stdout lines as `"partial": true` events; Python records them (event_log, pin history, `node.output`/`log`) before raising NodeCrashed, and ignores the stderr `... unterminated` copies of those lines. Without `STEST_CTRL` the node runs standalone (`--run-ms N`); there a strict abort first prints the aborting quantum's pending events and partial stdout/ITM lines as EVT lines (standalone abort hook in `node_main.cpp`), then exits 70.
- Python `Sim` advances all nodes in lockstep quanta (default 1000 us). Firmware variables are read/written by name via DWARF (`python/stest/symbols.py`, anchored at the `stest_symbol_anchor` runtime address; Mach-O via `macho.py`).
- CAN (`python/stest/can/`, guide `docs/can.md`): buses live in Python. At every quantum end the Sim hands each wired node's `can_mode` / `can_tx` / `can_tx_abort` events to its bus (`Node._can_flush`), runs the Python device callbacks due by T, then the bus engine (decides frames with SOF before T; results always land after T thanks to the quantum cap), and posts results to the bxCAN models as inbox `deliver` ops at their EOF.
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
ctest --test-dir build --output-on-failure                 # includes python_tests + python_perf_tests (uv, per-build-dir venv)
uv run pytest python/tests -q                              # Python alone; uses build/selftest or $STEST_SELFTEST_BIN
./build/selftest/blinky --run-ms 2100                      # standalone: EVT <t> GPIO <port><pin> <level> / EVT <t> <json>
uv run spike/run_gate.py                                   # speed gate (builds build-gate/ itself)
uv run spike/run_gate.py --quantum-us 88 --can-two-node --can-worst-case  # gate at the CAN quantum + gated two-node CAN row + informational no-ACK worst case
uv run pytest examples/quickstart -q                       # the Quickstart test (needs build/selftest/blinky)
uv run --group docs mkdocs build --strict                  # docs site into site/ (CI: uv run --only-group docs ...; no C/C++ toolchain needed)
uv run --group docs mkdocs serve                           # live preview at http://127.0.0.1:8000/
(cd examples/sensor-node && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && uv run pytest tests -q)  # the example consumer (CI job `example` also unsets UV_PROJECT_ENVIRONMENT)
```

Docker, exactly as CI runs it (`.github/workflows/ci.yml`):

```sh
docker build -t stest-ci docker
docker run --rm -v "$PWD":/w -w /w stest-ci bash -euxo pipefail -c '
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ctest --test-dir build --output-on-failure
  uv run pytest python/tests -q
  ./build/selftest/blinky --run-ms 2100
  uv run pytest examples/quickstart -q
'
docker run --rm -v "$PWD":/w -w /w stest-ci bash -c 'uv run spike/run_gate.py --build-dir build-gate-docker'
docker run --rm -v "$PWD":/w -w /w stest-ci bash -c 'uv run spike/run_gate.py --build-dir build-gate-docker --quantum-us 88 --can-two-node --can-worst-case'
```

Out-of-tree use (the consumer contract; a user's test repo with stest as a git submodule, referenced by path; user guide `docs/guide/getting-started.md`, worked example `examples/sensor-node/`): include the toolchain file before `project()`, then `add_subdirectory` stest:

```cmake
cmake_minimum_required(VERSION 3.23)
include(<stest>/cmake/StestToolchain.cmake)   # before project(); stest's configure fails otherwise
project(my_sil C CXX)
add_subdirectory(<stest> stest)
stest_add_firmware(board_a CHIP STM32L496RG SOURCES ... INCLUDE_DIRS ...)
```

`STEST_BUILD_TESTS` (default ON only when stest is the top-level project) gates `spike/`, `selftest/`, the runtime/chip test executables, doctest and every stest `add_test`; the runtime/chip libraries and `stest_add_firmware` work either way. `STEST_ROOT_DIR` (internal cache) is stest's source root. `selftest_out_of_tree_ok` / `_no_toolchain` (`selftest/check_out_of_tree.cmake`, consumers in `selftest/out_of_tree/`) configure, build and test exactly this pattern.

Note the Docker build writes into the host `build/`; if you share the tree with a native build, use a separate dir (e.g. `build-docker`, as `docs/install.md` suggests, and `--build-dir build-gate-docker` for the gate; the Quickstart test hardcodes `build/selftest/blinky`, and `python/tests` finds a different build through `STEST_SELFTEST_BIN`). `build*/` is gitignored.

## Non-negotiable rules

- **Firmware and ST sources compile unchanged.** Never patch ST code or require firmware edits. HAL-mode shims (Plan 2) replace whole ST `.c` files at link time.
- **Strict by default.** Unmodelled address, unimplemented register, write to a fully read-only register, or access to a clock-disabled peripheral (Plan 2) aborts the node with an `stest: ...` message naming address, width, direction and peripheral (exit 70, ERROR frame). Never return silent zeros or drop writes. Registers that merely contain RO bits keep hardware semantics.
- **Determinism.** No wall clock or host scheduling in simulation logic; identical inputs (including quantum size) give byte-identical event logs. Clock events fire at their own times (`Clock::advance_to`) and a node stops at exactly the quantum end with every event up to it fired (a charge that would cross it moves the clock to T, yields, and completes on resume), so each MMIO access and event happens at the same virtual time wherever boundaries fall and inbox messages at t >= T are never in the past; WFI sleeps across quantum ends (yielding the baton at each) and returns only when an exception is pending or was taken, as on hardware, so a `while (!x) __WFI();` with `x` written from Python sees it at the next interrupt (`__WFE` adds the ARMv7-M event register: `__SEV`, exception entry/return and SCR.SEVONPEND set it; see `Cortexm::wfe`); changing the quantum can still change timing in two ways: inputs set from Python between quanta (`set_high()`, `set_voltage()`, ...) take effect at the quantum boundary where the nodes are paused (`Pin.drive_at` / `Node.post` take an exact time), and loops dilated by poll fast-forward jump to the quantum end when no event is pending (user-facing version: `docs/concepts.md#determinism`). (Wall-clock use is limited to timeouts/watchdogs.)
- **Model contract** (`peripheral.hpp`): any time-driven state change is a scheduled `Clock` event, or visible as a different value on each read (like SysTick VAL). Lazily computed "ready at T" state without an event at T is overshot by poll fast-forward.
- **No Sakura** (the first user project) names, pins or DBC assumptions anywhere in stest. Sakura tests live in Sakura's repo.
- **Licensing.** Apache-2.0; ST files keep their own notices (see `NOTICE`, `LICENSE`).
- **Speed budget:** <= 0.4 s wall per virtual second per node (`selftest_blinky_speed`, `spike/run_gate.py`). Keep the MMIO hot path allocation-free.

## Change together

- `runtime/include/stest/protocol.h` <-> `runtime/src/control_channel.cpp` <-> `python/stest/protocol.py` (+ `node.py`/`sim.py` consumers): byte-for-byte mirror, currently version 2. Bump `STEST_PROTO_VERSION` / `PROTO_VERSION` together on any wire change (HELLO mismatch names both versions), update the header comment table, and add a Python test (`python/tests/test_protocol_v2.py` has the wire-format goldens).
- `Peripheral::deliver` ops <-> their Python posters; document ops in the model header. Current ops: GPIO `drive_pin` (`Pin.drive_at`); bxCAN `tx_result`, `rx`, `error`, `counters` (the CAN bus engine via `_node_can_callbacks` in `sim.py`). The bxCAN events `can_mode` / `can_tx` / `can_tx_abort` <-> `Node._can_flush` <-> `CanBus._set_node_mode` / `_request` / `_abort`; a new CAN controller model must implement the same events, ops and the `can_info` call op.
- `chips/*/parts/*.toml` <-> `tools/gen_chip.py` (schema/validation) <-> model class names/ctors. `stest_add_firmware` re-runs the generator when either changes.
- Model added: its source in `chips/<family>/CMakeLists.txt`, a TOML `[[peripheral]]` entry, and tests.
- HAL shims (`chips/<family>/hal/`) <-> `family.cmake` <-> `runtime/include/stest/hal_api.h` <-> the model's C API implementation; a shim must define every public symbol of the ST file it replaces, with ST's signatures (see `docs/adding-a-peripheral.md`, "Writing a HAL shim").
- New `stest_add_firmware` options: update the header comment in `cmake/stest_add_firmware.cmake` and the options table in `docs/reference/firmware.md`.
- Public Python API <-> its Google-style docstrings <-> `docs/reference/python/*.md` (mkdocstrings renders the docstrings; `python/tests/test_docstrings.py` requires every item of `stest.__all__` / `stest.can.__all__` to be documented). A new public name goes into a reference page; internals (`protocol`, `symbols`, `macho`) stay off the site. pytest plugin options and the `stest` marker <-> `docs/reference/pytest.md`.
- Env vars (`getenv("STEST_...")`, Python's `STEST_*`, CMake `$ENV{}`), `stest_add_firmware` keywords, `STEST_*` cache options and strict-abort / warning messages <-> `docs/reference/firmware.md` (`python/tests/test_docs_coverage.py` fails when one is missing; a message row there pins its source literal and its example on the page, so update both when the text changes).
- Modelled peripherals (part TOML `model =` entries, `chips/cortexm/`) <-> `docs/reference/support.md` (the coverage test requires each model's header link) and the README feature list.
- Example files (`examples/quickstart/test_blinky.py`, `examples/sensor-node/**`, `python/tests/test_recipes.py`) <-> the guides that quote them through `--8<--` snippet sections (`# --8<-- [start:name]` / `[end:name]`) or line ranges; renaming a file or section breaks `mkdocs build --strict` (`check_paths`), and README's Python block must equal the quickstart `test` section (`test_docs_commands.py`).
- Commands in fenced `sh` blocks of README, `docs/install.md`, `docs/quickstart.md`, `docs/guide/getting-started.md` (and the cookbook, reference and troubleshooting pages) <-> `.github/workflows/ci.yml` / `docs.yml`: each must be a CI line verbatim, or carry `<!-- not tested by CI -->` just before the block and say "not tested by CI" in visible text (`test_docs_commands.py`, `test_docs_coverage.py`). Change CI and the docs together.
- stest's CMake must keep working as a subproject (`selftest/out_of_tree/`): use `${STEST_ROOT_DIR}` (not `${PROJECT_SOURCE_DIR}` / `${CMAKE_SOURCE_DIR}`) for stest paths and `${PROJECT_BINARY_DIR}` / `${CMAKE_CURRENT_BINARY_DIR}` for build outputs; anything a function such as `stest_add_firmware` reads must be a cache variable, global property or set inside the function (the consumer's directory scope does not see stest's plain variables); new tests, test executables and test-only fetches go under `STEST_BUILD_TESTS`.

## Adding a peripheral model (current state)

`docs/adding-a-peripheral.md` holds the full guide (cross-node models fed by Python through `deliver()`: section 8, with bxCAN as the worked example). Follow the existing L4 models (`chips/stm32l4/models/{rcc,pwr,flash,gpio}.{hpp,cpp}`):
1. Create `chips/<family>/models/<kind>.{hpp,cpp}`: class `stest::<family>::<Kind>` deriving `stest::Peripheral`, ctor `(name, params-json, ModelContext& ctx)` (use `ctx.clock()`, `ctx.core()`; do not look up other models here); implement `name/read/write/reset`, and `call()` for Python-facing ops (throw `UnknownOp` otherwise).
2. Cross-model wiring goes in `connect(ModelContext&)`, called once every model exists: `ctx.get<OtherModel>("NAME")` (wrong type/missing -> `std::logic_error`), `ctx.clocks()` (the RCC's `ClockTree`: SYSCLK/HCLK/PCLKx/PLLSAIxR/ADCSEL, computed live from RCC registers; never cache a frequency; a model running on a clock across time subscribes to `ctx.clock_tree_changed()`, emitted by the RCC when a clock-tree output changes, see `docs/adding-a-peripheral.md` "Clock changes"), and `Signal<T>::subscribe` for synchronous notifications.
3. Use `RegisterFile`: `declare(off, reset, rw_mask, w1c_mask)`; `declare_write_only` for BSRR/KEYR-style registers; rw=w1c=0 makes a register read-only (writes abort). Layouts and reset values follow RM0351 and ST's `stm32l496xx.h`.
4. Time-driven behaviour: schedule `Clock` events (model contract above); derive counters from virtual time and schedule only the next instant a flag can change (see `tim.hpp`: no per-tick events, nothing scheduled while flags are already set). Emit observable changes with `emit_event`. Flag-driven interrupts are levels: `Cortexm::set_irq_level`, or `ModelContext::set_irq_source(irqn, &tag, high)` when the vector is shared (TIM1_UP_TIM16, TIM6_DAC, ...; the line is the OR of its sources). Never `set_pending` for a status flag.
5. Add the `.cpp` to `chips/stm32l4/CMakeLists.txt`, doctest cases to `chips/stm32l4/tests/`, a `[[peripheral]]` entry (`name`, `model = "stm32l4.<kind>"`, `base`, `size`, optional `params`, and `rcc_enable = { reg = "APB2ENR", bit = n }` if RCC gates its clock; `reg` must be listed in `[clock_tree] enable_regs`). Gated peripherals abort on any access while the bit is clear: `access to <NAME> while its clock is disabled (RCC <REG> bit <n>) (<read|write><bits> 0x%08x)`.
6. Add a Python/e2e test and, if firmware behaviour matters, a self-test firmware (enable the peripheral's RCC clock first).

## Adding a chip / part

1. `chips/<family>/parts/<part>.toml`: `[chip]` (name, family, core = "cortex-m4", nvic_prio_bits, device_define, system_init, systick_external_divider), `[[peripheral]]` entries, IRQ list (base addresses and IRQ numbers from the CMSIS device header). Run `python3 tools/gen_chip.py --info <toml>` and generate to a scratch file to validate.
2. New family: `chips/<family>/CMakeLists.txt` (library `stest_chip_<family>`, test targets), `add_subdirectory` in `chips/CMakeLists.txt`, ST pins and a family branch in `stest_st_family_dirs` (`StestFetchST.cmake`), plus a generator test like `chips/stm32l4/tests/check_gen.cmake`.
3. Add a self-test firmware under `selftest/` using `stest_add_firmware(... CHIP <PART> ...)`.

## Testing conventions

- TDD: write the failing test first. C++ uses doctest; CTest names are prefixed `runtime_`, `chip_`, `selftest_` (and `python_tests`, `python_perf_tests`). Python and e2e use pytest (`python/tests/`, `pytester` for plugin tests).
- Self-test firmwares live in `selftest/firmware/*`; fault injection is one exe per variant (`faults_spin|crash|unmodelled|ro_write|clkoff`, plus `faults_printf` for stdout capture, `faults_abort_output` / `faults_abort_events` for output and events of an aborting quantum). HAL-mode builds are suffixed `_gpio_hal` (`blinky_gpio_hal`, `exti_gpio_hal`, `hal_direct_gpio_hal`).
- Every CTest has a `TIMEOUT` (30 s for runtime/chip/selftests, 120 for the `selftest_configure_*` nested configures, 300 for the `selftest_out_of_tree_*` nested configure + build, 300 for `python_tests` / `python_perf_tests`); set one on new tests.
- Wall-clock budgets are load-sensitive (under `ctest -j8`, `test_can_speed` reached 0.97 of its budget beside the nested configures, and fails under CPU load): mark every Python test that reads a wall clock `@pytest.mark.perf`. CTest's `python_tests` runs `-m "not perf"` and `python_perf_tests` runs `-m perf` with `RUN_SERIAL` (alone), as does `selftest_blinky_speed`; `python/tests/test_perf_isolation.py` enforces both. Plain `uv run pytest python/tests` runs everything. `test_can_speed` (two CAN nodes, host-sensitive) checks its traffic always but its 1.6 s wall budget only with `STEST_PERF_STRICT=1` (otherwise a 4x sanity bound); the gated CAN speed figure is `spike/run_gate.py --can-two-node` (CI's non-blocking 88 us gate step).
- Before claiming done, run the full suite natively AND in Docker (Linux-only paths like `PR_SET_PDEATHSIG` and `/proc` differ from macOS).
- Do not match on raw JSON key order in event logs (keys are sorted).
- Docs honesty tests (all in `python/tests`, run by `python_tests`): `test_docstrings.py` (public API documented), `test_docs_commands.py` (shell commands on README and the getting-started pages are CI lines; README site links resolve to `docs/` pages; README's Python block is the quickstart test), `test_docs_coverage.py` (env vars, options, cache variables, messages, modelled peripherals and GitHub links on the reference pages), `test_recipes.py` (every cookbook snippet runs; the guides quote it by snippet section). Docs-only changes still need `uv run --group docs mkdocs build --strict` (broken links, anchors and snippet paths fail it).

## Known gotchas

- Apple clang is rejected; use Homebrew/upstream LLVM. `stest_add_firmware` fails at configure if the C compiler is not `STEST_CLANG`, or clang and LLVM majors differ.
- macOS: a cctools `ld` earlier in PATH (Anaconda) breaks plugin bundles and DWARF; the toolchain forces Xcode's `ld` via `xcrun` (`STEST_XCODE_LD`). dSYM is generated by a POST_BUILD `dsymutil`.
- Do not put CMSIS-Core headers (`core_cm*.h`, `cmsis_gcc.h`, `cmsis_compiler.h`) in `INCLUDE_DIRS`; configure fails by design.
- `HAL_Delay(n)` waits n+1 ticks (ST adds one); blinky uses `HAL_Delay(499)` so edges land at 500 ms, 1000 ms...
- Analog: pin voltages live in the GPIO models (`Pin.set_voltage`, DAC outputs); the ADC resolves channels to pins through the part TOML's `[[analog]]` table (injected into the ADC's params by `gen_chip.py`). `set_voltage` is accepted only on pins with an analog function (the ADC and DAC register theirs with `Gpio::add_analog_function` in `connect()`); any other pin is a REQ_ERROR (`PB11 has no analog function on STM32L496RG; analog pins: ...`). An unset input reads 0 V with one stderr warning per (ADC, channel), or aborts with `STEST_STRICT_ANALOG=1` / `Sim(strict_analog=True)` / `--stest-strict-analog` / `@pytest.mark.stest(strict_analog=True)`. MODEL_CALL (Python) bypasses RCC clock gating by design.
- ITM/DWT/CoreDebug (`chips/cortexm/`) are mapped by `Cortexm` on every chip; the default models an attached debugger (ITM/TRCENA/C_DEBUGEN on) so CMSIS `ITM_SendChar` works. `STEST_DEBUGGER=0` / `Sim(debugger_attached=False)` / `--stest-no-debugger` / `@pytest.mark.stest(debugger_attached=False)` selects the bare-board state (marker > CLI > default), where `ITM_SendChar` drops output and the ITM is software-locked: TER/TPR/TCR writes are ignored until firmware writes 0xC5ACCE55 to `ITM->LAR`, as on hardware. Python: **use `node.output` / `node.wait_for_log(regex, timeout_ms)` (default `source="any"`) for firmware logs; they work the same on every host** (`output` merges ITM port 0 and stdout lines in virtual-time order, so firmware that picks ITM vs `printf` with `#if defined(__ARM_ARCH)`, true on arm64 hosts only, behaves the same on macOS arm64 and Linux x86-64). `node.log` (ITM port-0 lines only), `node.itm_port(n)`, `wait_for_log(..., source="itm"|"stdout")`, `node.stdout_tail` (last 200 raw lines of the stdout pipe). Stdout capture (`runtime/include/stest/stdout_capture.hpp`): the node replaces the C `stdout` FILE with an unbuffered custom stream (macOS `funopen`, Linux `fopencookie`) that emits `{"kind":"stdout","periph":"stdout","text":...}` per line at the current virtual time and forwards the bytes to fd 1; raw `write(1, ...)` bypasses it; standalone mode shows stdout only as EVT lines. ITM events are `{"kind":"itm","periph":"ITM","port":N,"text":...}`; an unterminated line at abort/exit is written to stderr as `stest: ITM unterminated {"port":N,"text":...}` / `stest: stdout unterminated {"text":...}` (and appended to `node.log` / `node.output`). DWT CYCCNT derives from virtual time x `SystemCoreClock` (a clock change applies to the time since the previous DWT access); comparators and other trace features abort.
- Poll fast-forward dilates loops that count iterations over constant volatile reads (`while (--n && !(REG & FLAG));`). Disable with `STEST_POLL_SKIP=0`, `Sim(poll_skip=0)`, `--stest-poll-skip 0` or `@pytest.mark.stest(poll_skip=0)`.
- CAN (`docs/can.md`): creating a bus caps the Sim quantum at `lookahead_ns(bitrate)` (88 us at 500 kbit/s), which costs one socket round trip per node per 88 us; the speed gate's `--quantum-us 88` row measures it and `--can-worst-case` the lone-node endless no-ACK case. Wiring is strict: `sim.node(..., can={"CAN2": bus})`; a controller that transmits unwired, or enters normal mode at a bit rate not exactly the bus's, ends the sim with NodeCrashed, and wired names are checked with the model's `can_info` op at `start()`. The bus engine owns TEC/REC (absolute values delivered to the model with an `epoch`); Python `CanDevice`s have no fault confinement and retry forever. CAN2's filters live in CAN1's block, so firmware needs CAN1's RCC clock. `bus.log` / `bus.errors` are unbounded. cantools (DBC) pulls in python-can (LGPL-3.0); both are the optional `dbc` extra (`stest[dbc]`, in the dev group), imported only when a `Dbc` is loaded, never by `import stest` (`python/tests/test_import_light.py`). `test_can_speed` checks its traffic always and its 1.6 s wall budget only with `STEST_PERF_STRICT=1`; the gated CAN speed figure is the gate's two-node row (`spike/run_gate.py --can-two-node`, budget 2 x 0.40); see "Testing conventions" for how wall-clock tests are isolated.
- Zombie nodes: in a container where pytest is PID 1 (as in CI's `docker run ... bash -c '...; uv run pytest'`), killed orphans are never reaped; `python/tests/test_plugin.py` checks `/proc/<pid>/stat` for state `Z`. Keep that in mind when writing process-lifetime tests.
- Nodes exit when their parent dies: Python passes `STEST_PARENT_PID`; the runtime uses `PR_SET_PDEATHSIG` (Linux) plus a getppid watchdog thread. Do not use `preexec_fn` for this. macOS SIGTERM bypasses `atexit`.
- CMake word-wraps `message()` text by path length; tests matching error text must collapse whitespace first (`selftest/check_configure_fails.cmake`).
- Firmware `main` returning in control mode is a strict abort (ERROR, exit 70). A bad Python request (oversized or invalid READ_MEM/WRITE_MEM, unknown model/op) is a REQ_ERROR: Python raises `StestError` and the node keeps running.
- Only one Node per process (bus/clock are process-global).
- Plan 1 deferred minor findings (pass vs. volatile `memcpy`/`memset`, `RegisterFile` using `std::map`, etc.) are listed in `docs/superpowers/plans/2026-10-30-plan-1-followups.md`; check it before assuming a behaviour is a regression.

## Workflow

- Specs and plans live in `docs/superpowers/` (`specs/`, `plans/`, one plan per milestone, plus `...-followups.md`). One branch per plan (`plan-1-core`, `plan-2-peripherals`, `plan-3-can`); the docs site followed `docs/superpowers/plans/2026-11-12-stest-docs-site.md` (branch `docs-site`). Record decisions and deferred findings in the plan's follow-ups file (`docs/superpowers/plans/2026-10-30-plan-1-followups.md`, `2026-10-31-plan-2-followups.md`, `2026-10-31-plan-3-followups.md`, `2026-11-12-docs-site-followups.md`; check them before assuming a behaviour is a regression).
- Commits use a conventional prefix (`feat`, `fix`, `fix(runtime):`, `docs:`, `docs(plan-N):`) and a body explaining why. Every commit in history ends with a `Co-Authored-By: <model name> <noreply@anthropic.com>` trailer naming the model that did the work; use the attribution line your harness instructs.
