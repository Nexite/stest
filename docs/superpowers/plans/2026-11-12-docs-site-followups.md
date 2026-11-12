# Documentation site follow-ups

Behaviour gaps, inconsistencies and tooling items found while writing the documentation site
(plan `2026-11-12-stest-docs-site.md`). The docs plan changes no behaviour: the pages describe
what the code does today, and these are left for later plans.

## Python API behaviour
- `Sim(quantum_us=0.5)` gives `quantum_ns` 0, then `ZeroDivisionError` on the first advance or
  wait; `1.5` is silently truncated to 1000 ns; `"100"` raises `TypeError`. The pytest plugin
  passes marker and command-line values to `Sim` unvalidated, so the same happens there.
- `CanBus.wait_for_frame(id=...)` ignores `ext`: a standard and an extended frame with the same
  id both match.
- `Sim.quantum_us` floors a CAN-capped quantum that is not a whole number of µs.
- `CanDevice.every` / `DbcDevice.send_periodic` timers cannot be cancelled.
- A `DbcDevice` subclass that overrides `on_frame` without calling `super()` leaves `received`
  empty.
- `CanBus.send` registers a new plain sender before validating `frame`, so a bad frame raises
  `TypeError` after a side effect.
- pytest `--help` texts for `--stest-poll-skip`, `--stest-strict-analog` and
  `--stest-no-debugger` do not say that the node inherits the environment variable when the
  option is not given.

## Runtime and models
- No fault exceptions: a bad pointer crashes the host process (`NodeCrashed` with a signal)
  instead of entering `HardFault_Handler`, and CFSR/HFSR read 0. Firmware fault handling cannot
  be tested.
- `NVIC_SystemReset()` (AIRCR.SYSRESETREQ) aborts as not modelled. Firmware commonly resets on
  a fatal error or after a firmware update; a node reset (or a clean "node reset" event that
  Python can observe) would let tests cover those paths.
- No UART/USART, I2C or DMA models. CubeMX projects almost always initialise at least one UART;
  today users must compile that code out (Troubleshooting documents the `DEFINES` switch).
- A model's `std::runtime_error` reaches the user as `stest: exception in model at 0x<addr>:
  <text>` (or `exception in clock event:`, `exception in stest_rt_wfi:`), while
  `NotModelled` gets the cleaner `<PERIPH>: <feature> not modelled (<access>)`. The internal
  function name `stest_rt_wfi` / `stest_rt_wfe` in the WFI message means nothing to users.
  Consider one message shape naming the peripheral and the access.
- `dac.hpp`'s header comment gives the abort text as `DAC1 <feature> not modelled (...)` (no
  colon); the code (`NotModelled`) prints `DAC1: <feature> not modelled (...)`. Fix the comment.
- Standalone mode exits with `main`'s return value when `main` returns; under Python the same
  event is a strict abort (exit 70). Documented as is (firmware reference); decide whether
  standalone should also exit 70.
- `SCR.SLEEPDEEP` and `PWR_CR1.LPMS` are stored only: Stop and Standby entry sleep like WFI with
  every clock and peripheral running. Low-power firmware is not checked against what stops in
  Stop mode.

## Build
- No supported way to build the full LL drivers: the default `HAL_SOURCES` glob is
  `stm32l4xx_hal*.c`, and the fetched HAL directory is reachable only through
  `stest_st_family_dirs()`, which is not documented as public API and not tested from a consumer
  project. Troubleshooting describes the workaround as not tested by CI. Options: an `LL_SOURCES`
  option or `MODELS`-like switch, or include `stm32l4xx_ll_*.c` in the default when
  `USE_FULL_LL_DRIVER` is in `DEFINES`.
- `stest_add_firmware` forces C11 on the firmware targets, overriding a consumer's
  `CMAKE_C_STANDARD` (Task 1 finding); C++ firmware sources are untested.
- RTOS ports (FreeRTOS `ARM_CM4F`) do not compile for the host; there is no host port.

## Docs and tooling
- Material for MkDocs reaches end of life on 2027-05-05: plan a migration (its successor reads
  `mkdocs.yml`) before then.
- ~~AGENTS.md's "Determinism" bullet still says a quantum change affects timing "through extra wfi
  wake-ups at quantum ends"~~ Done (Task 7): the WFI wording had already been corrected; the
  bullet now also names the second quantum dependence (inputs set from Python between quanta),
  matching `docs/concepts.md#determinism`.
- Mermaid diagrams (Home, Concepts) were checked by construction only; check the rendered pages
  after the first deploy.
- `examples/sensor-node/uv.lock` drifts when stest's dependency groups change (`uv run` relocks
  silently).
- The new CI checks (example jobs, docs build) must be added to the required status checks
  (repository setting) to block merges.
- `mkdocs build --strict` only reported broken anchors at INFO level; `validation: anchors: warn`
  in `mkdocs.yml` now makes them fail the strict build. Links to `github.com/.../blob/main/...`
  are not checked by MkDocs; `python/tests/test_docs_coverage.py` checks those on the reference,
  troubleshooting and cookbook pages only.
- The cookbook recipes (`python/tests/test_recipes.py`) run on the self-test firmware, not on a
  CubeMX-style project; the `firmware` fixture in `python/tests/conftest.py` mirrors the Getting
  started fixture's shape but not its code.
- `pytest-xdist` parallel runs are neither documented nor tested.

## From the final whole-branch review

### Example project (`examples/sensor-node`) and consumer test
- The out-of-tree consumer test covers one register-mode firmware only; add a second target
  built with `MODELS GPIO=hal`.
- An ADC conversion timeout still sends a frame with 0 mV (`sensor/main.c`, the
  `HAL_ADC_PollForConversion` branch leaves `raw` at 0).
- The button counter has no debounce and no comment saying it is deliberately absent.
- The display's `g_last_millivolts` is not `volatile`. Safe in stest (volatile RAM accesses also
  charge time), but a reader may copy the pattern.
- `test_button_count_in_frame` leaves PC0 unset; set it to 0.0 V in `start_sensor`.
- The `g_sample_count` assert is weak.
- A run where every test skips passes; under CI it should fail instead of skip.

### CI and the docs workflow
- `docs.yml` uses `uv run` without `--locked`.
- `docs.yml` has no `workflow_dispatch` trigger (useful for the first deploy).
- `actions/upload-pages-artifact@v3` / `actions/deploy-pages@v4` (latest major is v5) are first
  exercised on `main`.
- The CI blinky step checks the exit status only; the EVT lines shown in the README and
  Quickstart are not pinned.

### Python API docstrings
- `format_candump` prints a bare `R` for an RTR frame with DLC 0.
- `TimerChannel.mode` does not list `"unsupported"`.
- `set_var`'s Raises section omits `StestError` before `start()`.
- `CanBus.send`'s Raises section omits `ValueError` for a bad sender.
- `Pin.drive_at`'s Raises section is incomplete.
- `set_high` / `set_low` say "end of last quantum"; it is `node.now_ns`.
- The pytest options are described both in the plugin docstring and in `pytest.md`; keep them
  in sync or generate one from the other.

### Docs content
- `adding-a-peripheral.md:135` says "design spec" twice.
- The reason given for leaving out `syscalls.c` / `sysmem.c` is understated: `_sbrk` needs
  linker-script symbols, and a strong `_exit` spins, so the node hangs.
- Concepts repeats the poll-skip knobs listed elsewhere.
- The exit-status 2 row omits a bad `--quantum-us`.
- The `vars.c` line-range snippet is fragile; use section markers.
- LL-driver guidance is in two places (Troubleshooting and `firmware.md:32`, which lacks the
  "not tested by CI" label).
- HSE wording: name `params.hse_hz` (default 8 MHz).
- The `wall_timeout_s` default is restated on three pages.
- `firmware.md` says it lists "every" message but omits: exception in a delivery hook, event
  JSON too long, the two GPIO HAL shim aborts, no Cortex-M core installed, a bad
  `STEST_ACCESS_COST_NS`, a bad `STEST_PARENT_PID`, and `MODELS` duplicate/format errors.
- That volatile RAM accesses charge virtual time and deliver interrupts (`mmio.cpp:158-183`) is
  undocumented.
- "The linker dropped it" (`variables.md:59`, `troubleshooting.md:103`) is unverified; more
  likely the compiler drops an unused `static`.
- `getting-started.md:118`'s inline `./build/<board> --run-ms 1000` is neither CI-run nor
  labelled.
- `getting-started.md:57,66-70`: the hand-written CMake is not tied to a tested snippet.
- Trim the README's peripheral list (link the support matrix).
- Version floors (LLVM, CMake, Python) are duplicated across pages.

### Docs tests
- `test_every_abort_message_documented` uses a hand-typed table that over-claims; derive it
  from the `stest_rt_abort` literals with an allow-list.
- The host-crash coverage row pins `node.py` only.
- The docstring test skips `staticmethod`, `classmethod` and `cached_property`.
- The CI parity test ignores context (`continue-on-error`, `cd` pairing, YAML comments).
- The README Python-snippet test is vacuous when the README has no Python block.
- The README link test checks that files exist, not that they are in the nav.
- `test_opt_outs_are_visible` checks per page, not per block.
