# stest documentation site — design

Date: 2026-11-12. Status: draft for user approval (builder check done).

## Intent

stest works, but only its authors can use it. The repo has a 75-line README, contributor guides
(`AGENTS.md`, `docs/adding-a-peripheral.md`, `docs/adding-a-chip.md`), a CAN user guide
(`docs/can.md`) and design/plan documents. Nothing tells a newcomer how to take **their own**
STM32 firmware project from zero to a passing pytest, and nothing lists the Python API in one
place.

The first reader is the project owner, writing SIL tests by hand for their own firmware project
(a CubeMX-generated, multi-board STM32L4 project in a separate repo) and coming back to look
things up while they write. The lasting reader is anyone with an STM32 project who finds stest on
GitHub. The docs must serve both, and therefore stay generic: **no names, pins, boards or DBC
contents of the first user project** (an existing repo rule, AGENTS.md "No Sakura").

Success: a competent embedded developer who has never seen stest can, from the docs alone,
install the toolchain, build their own CubeMX project under stest, write and run a first test,
and then find how to do each common testing task without reading stest's source.

## Goals

1. A documentation **website**: MkDocs with the Material theme, built from Markdown in the repo,
   published to GitHub Pages from `main` by CI, with search and navigation.
2. **Python API reference generated from docstrings** (mkdocstrings, Python handler), so it
   cannot drift from the code. Public API only.
3. Hand-written guides (outline below): Home, Install, Quickstart, Getting started with your own
   project, Writing tests (cookbook), Concepts, CAN (from `docs/can.md`), Firmware reference,
   Troubleshooting, Support matrix, Contributing (links to the existing contributor docs).
4. **Make the getting-started path real**: stest has no supported way to be used from another
   repo today (the top-level `CMakeLists.txt` assumes it is the top-level project and always
   builds the self-tests; nothing documents or tests out-of-tree use). Add a minimal, supported
   out-of-tree consumption path and an example project that exercises it in CI. Every command the
   getting-started guide shows must be one CI runs.
5. A shorter **README** that is a front door: what stest is, a feature list, a 5-minute
   quickstart, and links into the site. The long reference prose now in README moves to the site.
6. Docstring gaps on the public API filled (peripheral helpers, pytest plugin options/markers,
   `Sim`/`Node`/`Pin` items currently undocumented) so the generated reference is complete.

## Non-goals

- No change to simulator behaviour or the Python API (beyond docstrings and the out-of-tree
  build option below). Missing features found while writing go to a follow-ups list, not into
  this work.
- No PyPI release; install is from git (see Install). A release process is future work.
- No versioned docs (one version, built from `main`).
- No docs for internals beyond what exists (protocol, runtime internals stay in AGENTS.md and the
  design spec; the site links to them under Contributing).
- No tutorial videos, no custom theme work beyond Material's configuration.

## Anti-goals (would make this a failure even if it technically works)

- Docs that describe commands nobody has run: any shell or CMake snippet in the getting-started
  path that CI does not execute.
- A generated API reference full of internal helpers (`protocol`, `symbols`, `macho`) that buries
  the handful of classes users touch.
- First-user-project specifics anywhere in the site or the example project.
- Duplicated sources of truth: the same option described in two places that can disagree. Each
  fact lives in one page; others link to it.

## Constraints

- Apache-2.0; docs tooling must be permissively licensed (MkDocs BSD, Material MIT,
  mkdocstrings ISC — fine).
- Docs build must not need the C/C++ toolchain: mkdocstrings reads the Python sources statically
  (griffe), so the Pages job only needs Python + uv.
- The docs build runs in CI on every PR in **strict mode** (`mkdocs build --strict`: broken links,
  missing references fail the build); deploy only from `main`.
- Existing CI (`.github/workflows/ci.yml`) stays the authority for tests; docs get their own
  workflow (`docs.yml`) so a docs failure is visible separately.
- Platforms as today: macOS arm64 and Linux x86-64/arm64 native, Docker for Windows/zero setup.

## Site outline (the decided content)

1. **Home** — what stest is (unmodified firmware + ST HAL compiled natively, register-level
   peripheral models, deterministic virtual time, pytest, multi-node CAN), a diagram of the
   pieces (firmware → MMIO pass → runtime → models; Python Sim ↔ nodes ↔ CAN buses), what it is
   good for and what it is not (not cycle-accurate, not an instruction-set simulator, only
   modelled peripherals), link to Quickstart.
2. **Install** — toolchain per platform (macOS Homebrew, Ubuntu apt, Docker/dev container for
   Windows), uv, building stest itself, running its tests, common install failures (Apple clang,
   LLVM not found, Anaconda `ld`).
3. **Quickstart** — 5 minutes: clone, build, run the bundled blinky test with pytest, read the
   standalone `EVT` output.
4. **Getting started with your own project** — the out-of-tree path end to end, following the
   example project: project layout, the small CMake file, `stest_add_firmware` for a CubeMX
   project (which sources to list, include dirs, defines), building, installing the Python package
   into the test environment, pointing tests at the built executable, first test, running it, and
   the first errors people hit (unmodelled register, clock disabled, `main` returning) with what
   each means.
5. **Writing tests** (cookbook; one page per area, each recipe = goal, code, notes):
   - The `sim` fixture, nodes, starting, advancing virtual time, waits and timeouts.
   - Pins: drive inputs, read outputs, history, waits; EXTI.
   - Firmware variables by name (scalars, structs, arrays, enums), writing them.
   - Firmware output: `node.output`, `wait_for_log`, ITM vs stdout.
   - Timers/PWM, ADC/analog voltages, DAC, SPI helpers.
   - Multiple nodes; CAN (summary + link to the CAN page).
   - Fault scenarios and asserting a node crashes (`NodeCrashed`), strict analog, debugger
     attached/detached, poll fast-forward settings.
   - pytest integration: markers (`@pytest.mark.stest(...)`), CLI options, failure reports,
     `--stest-can-log`.
6. **Concepts** — virtual time and quanta (why results are deterministic, what changes with the
   quantum), strictness (why stest aborts instead of guessing), register vs HAL mode, WFI/WFE
   behaviour, poll fast-forward, how firmware is compiled (the pass, `-Dmain=`), one process per
   node.
7. **CAN** — `docs/can.md` moved into the site (kept as the single source; split into sub-pages if
   long).
8. **Firmware reference** — `stest_add_firmware` signature and every option, CMake cache
   variables (`STEST_LLVM_ROOT`, ST source overrides, the new out-of-tree option), environment
   variables (`STEST_POLL_SKIP`, `STEST_STRICT_ANALOG`, `STEST_DEBUGGER`, `STEST_PERF_STRICT`, …),
   standalone mode flags (`--run-ms`, `--quantum-us`), the `EVT` format, and every `stest: ...`
   abort message with its meaning and fix.
9. **Python API reference** — generated: `stest` top-level exports (`Sim`, `Node`, `Pin`,
   peripheral helpers, errors), `stest.can` (`CanBus`, `CanFrame`, `CanDevice`, `Dbc`,
   `DbcDevice`, log helpers), and the pytest plugin (fixture, options, markers — hand-written
   table, since hooks are not API).
10. **Support matrix** — chips/parts, per-peripheral what is modelled and known deviations (links
    to model headers' deviation notes), host platforms.
11. **Troubleshooting / FAQ**.
12. **Contributing** — links to AGENTS.md, adding-a-peripheral, adding-a-chip, the design spec;
    these files stay where they are (not moved into the site's nav as content, to keep the user
    site user-focused) — or included via a "Contributing" section that renders them; builder's
    choice, see below.

## Out-of-tree consumption (the decided shape)

- Supported pattern: the user's test repo has a `CMakeLists.txt` that includes stest's toolchain
  file **before** `project()`, then adds stest as a subdirectory (git submodule or
  `FetchContent`), then calls `stest_add_firmware(...)` for each board. A new cache option (name
  builder's choice, e.g. `STEST_BUILD_TESTS`, default ON only when stest is the top-level project)
  skips stest's self-tests, spike and their test registrations when consumed.
- Python side: `uv add git+https://github.com/Nexite/stest` (or a path dependency to the
  submodule), which installs the package and registers the pytest plugin; tests find the built
  executables by a path the user's conftest provides (document a recommended pattern, e.g. an env
  var or a conftest fixture).
- An `examples/` project (generic CubeMX-style STM32L496 firmware, e.g. a button + LED + one UART
  or CAN message; not the self-test code copied verbatim, not first-user-project code) with its
  own `CMakeLists.txt` and `tests/`, built and tested by CI **as an external consumer** (configured
  from its own directory, stest referenced by path), on Linux and macOS.

## Settled after the builder check (designer's calls, not user decisions)

1. **HAL source.** The example and the guide use stest's pinned, fetched ST HAL (the
   `stest_add_firmware` default). The user lists only their own `Core/` sources (`main.c`,
   `*_it.c`, `*_hal_msp.c`, their modules) and `Core/Inc`; they do **not** add their project's
   `Drivers/` (HAL, CMSIS) to `INCLUDE_DIRS` (CMSIS-Core there is a configure error by design).
   The guide documents `STEST_STM32L4_HAL_DIR` / `STEST_STM32L4_CMSIS_DEVICE_DIR` as the way to
   use a different HAL version, marked "not tested by CI", and Troubleshooting covers HAL version
   skew symptoms. No work to validate other HAL versions.
2. **Hosting.** The repo is public. GitHub Pages is not enabled yet: the owner enables it once
   (Settings → Pages → Source: GitHub Actions); `docs.yml` deploys with the standard
   `actions/upload-pages-artifact` + `actions/deploy-pages` flow and the `github-pages`
   environment. `site_url` = `https://nexite.github.io/stest/`.
3. **Primary out-of-tree pattern = stest as a git submodule (a path) + `add_subdirectory`.** CI
   tests exactly that (the example configured from its own directory, stest referenced by
   relative path, which is what a submodule is). FetchContent is **not** documented (it would be
   an untested command). Fix what breaks under `add_subdirectory` (e.g. `${PROJECT_SOURCE_DIR}`
   and `${CMAKE_BINARY_DIR}` uses in stest's CMake that assume stest is top level).
4. **Python install = path dependency on the submodule** in the user's test project
   (`uv add --editable ./<path-to-stest>` or the equivalent `[tool.uv.sources]` path entry); this
   is what the example's `pyproject.toml` uses and what CI runs. The git-URL install is mentioned
   in one line as an alternative, labelled untested. The root `pyproject.toml` keeps
   `readme = "README.md"`; the trimmed README is fine as package metadata.
5. **Example peripherals.** No UART model exists. The example uses only modelled peripherals:
   a button on EXTI, an LED on GPIO, an ADC reading reported in a CAN frame, and a second
   example node (or a Python `CanDevice`) to show multi-node CAN — DBC optional, via the `dbc`
   extra in the example's dev dependencies. The "first errors" section is illustrated with small
   deliberate-mistake variants or captured messages (builder's choice), not by breaking the main
   example.
6. **CI structure.** A separate job (or workflow) builds and tests the example on Linux (Docker
   image) and macOS; it does not share stest's own `build/`. Docs build/deploy is `docs.yml`.
7. **`docs_dir`.** `docs/` stays the docs directory; `docs/superpowers/` and `docs/spikes/` are
   excluded from the site (`exclude_docs`), and contributor docs (`adding-a-*.md`) stay in place
   and appear under Contributing. `docs/can.md` stays at its path (no reference changes). New
   user pages are added under `docs/` (e.g. `docs/guide/`, `docs/reference/`).

## Left to the builder

- MkDocs plugin set beyond Material + mkdocstrings (e.g. `mkdocs-include-markdown` to render
  AGENTS.md/contributor docs, mermaid for diagrams), and whether contributor docs render in the
  site or are linked.
- Docstring style (Google or NumPy; pick one, configure mkdocstrings for it) and the exact public
  module list for the reference (`members`/filters).
- Page splitting within each section; wording; diagrams (mermaid or SVG).
- The example firmware's exact behaviour and the conftest pattern, within the constraints above.
- Where the docs dependencies live (a `docs` dependency group in `pyproject.toml` is the default).
- How README content is trimmed, as long as nothing is lost (it moves to the site) and the README
  quickstart is a command CI runs.
