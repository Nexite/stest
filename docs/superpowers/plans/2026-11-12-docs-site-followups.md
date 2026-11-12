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
