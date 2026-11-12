# Getting started with your own project

This page takes an STM32L4 firmware project (CubeMX-generated, or laid out like one) from zero
to a passing pytest under stest. It follows `examples/sensor-node/` in the stest repository: a
small two-board project that CI builds and tests exactly as described here, as an outside
project would.

You need the toolchain from [Install](../install.md).

## Project layout

Keep the tests in a repository of their own (or a directory of your firmware repository) with
stest as a git submodule:

```text
my-firmware-tests/
├── CMakeLists.txt          # builds each board into a host executable
├── pyproject.toml          # the Python test environment
├── external/stest/         # git submodule
├── firmware/
│   └── <board>/Core/       # your CubeMX Core/Src and Core/Inc (or a submodule)
└── tests/
    ├── conftest.py
    └── test_<board>.py
```

Add stest as a submodule. These commands run once, in your repository, so they are not tested
by CI:

<!-- not tested by CI -->
```sh
git submodule add https://github.com/Nexite/stest.git external/stest
git submodule update --init
```

## The CMake file

This is the example's `CMakeLists.txt`:

```cmake
--8<-- "examples/sensor-node/CMakeLists.txt:consumer"
```

Three lines make stest available, in this order:

1. `include(${STEST_DIR}/cmake/StestToolchain.cmake)` **before** `project()`. It finds upstream
   clang and makes it the C and C++ compiler. CMake chooses compilers in `project()`, so the
   include must come first.
2. `project(...)` with `C CXX` (the runtime is C++).
3. `add_subdirectory(${STEST_DIR} stest)`, which builds the runtime and the pass and defines
   `stest_add_firmware()`. stest's own tests are not built when it is a subproject
   (`STEST_BUILD_TESTS` defaults to OFF there).

In your repository, point `STEST_DIR` at the submodule:

```cmake
set(STEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/stest" CACHE PATH "stest source root")
```

### `stest_add_firmware`

One call per board builds one host executable named after its first argument. Use absolute
paths (as here, from `CMAKE_CURRENT_SOURCE_DIR`):

```cmake
set(_core ${CMAKE_CURRENT_SOURCE_DIR}/firmware/sensor/Core)
stest_add_firmware(sensor
  CHIP STM32L496RG
  SOURCES ${_core}/Src/main.c ${_core}/Src/stm32l4xx_it.c ${_core}/Src/stm32l4xx_hal_msp.c
  INCLUDE_DIRS ${_core}/Inc)
```

- `CHIP`: the part. `STM32L496RG` is the part stest models today (see the
  [support matrix](../reference/support.md)).
- `SOURCES`: your own C sources: `main.c`, `stm32l4xx_it.c`, `stm32l4xx_hal_msp.c` and your
  application modules. Do **not** list:
    - ST's HAL (`Drivers/STM32L4xx_HAL_Driver`) or CMSIS: stest compiles its pinned copy of the
      HAL for every firmware.
    - `startup_stm32l4*.s` (assembly for the real core) or `syscalls.c` / `sysmem.c` (newlib
      stubs; the firmware uses the host's C library).
    - `system_stm32l4xx.c` is optional: stest links ST's from the pinned CMSIS device files,
      and a copy in `SOURCES` takes its place.
- `INCLUDE_DIRS`: `Core/Inc` (it holds `stm32l4xx_hal_conf.h`, which selects the HAL modules) and
  your own headers. Do not add `Drivers/.../Include` directories: stest provides CMSIS and a
  host replacement for its compiler header, and a directory with CMSIS-Core headers in
  `INCLUDE_DIRS` is a configure error.
- `DEFINES`: extra preprocessor definitions for your sources and the HAL. The device define
  (`STM32L496xx`) and `USE_HAL_DRIVER` are already set.

Every option is in the [firmware reference](../reference/firmware.md).

### Which HAL version

By default stest downloads ST's STM32L4 HAL, CMSIS device files and CMSIS-Core at pinned
commits, and every firmware is built and tested against that HAL. That is what CI tests.

To build against another HAL checkout (for example the one in your CubeMX project's
`Drivers/`), set `STEST_STM32L4_HAL_DIR` and `STEST_STM32L4_CMSIS_DEVICE_DIR` when you
configure. This is **not tested by CI**: stest's models are checked against the pinned HAL, and
another version may touch registers differently (see [Troubleshooting](../troubleshooting.md)).

<!-- not tested by CI -->
```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTEST_STM32L4_HAL_DIR=$PWD/firmware/Drivers/STM32L4xx_HAL_Driver -DSTEST_STM32L4_CMSIS_DEVICE_DIR=$PWD/firmware/Drivers/CMSIS/Device/ST/STM32L4xx
```

## Build

From the root of your test repository:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The first configure downloads the pinned ST sources and nlohmann/json. The executables land in
`build/` (`build/sensor`, `build/display` in the example). Each is a normal host program: try
`./build/<board> --run-ms 1000` to run it standalone and see its events.

To try the example itself first, run the same commands in its directory of the stest checkout,
then `uv run pytest tests -q` (as CI does):

```sh
cd examples/sensor-node
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
uv run pytest tests -q
```

## The Python test environment

The tests use stest's Python package from the same submodule, as an editable path dependency.
The example's `pyproject.toml`:

```toml
--8<-- "examples/sensor-node/pyproject.toml:pyproject"
```

In your repository the path is the submodule: `stest = { path = "external/stest", editable =
true }`. Drop the `[dbc]` extra if you do not decode CAN messages with a DBC file. The file has
no `[build-system]`, so uv treats the test directory as a project, not a package to build.
Installing stest registers its pytest plugin, which provides the `sim` fixture.

Alternatively, `uv add git+https://github.com/Nexite/stest` installs the package from git
instead of the submodule. That is **not tested by CI**, and the Python package must then match
the stest version your firmware is built with.

## Finding the executables: `conftest.py`

Tests need the path of each built executable. The example's `tests/conftest.py` provides a
`firmware` fixture that looks in `build/`, or in a directory named by an environment variable.
Copy this into `tests/conftest.py` (rename `SENSOR_NODE_BIN` as you like):

```python
--8<-- "examples/sensor-node/tests/conftest.py:imports"


--8<-- "examples/sensor-node/tests/conftest.py:firmware"
```

`EXAMPLE_DIR` is the directory above `tests/`, where `build/` is. The fixture skips the test,
with the command to run, when an executable is not built.

## A first test

A first test needs only the board's pins and globals. This one, from the example's
`tests/test_first.py`, presses a button and checks that the LED toggled and the firmware counted
the press:

```python
--8<-- "examples/sensor-node/tests/test_first.py:first"
```

- `sim.node(name, path)` adds a node: one MCU, running the executable as its own process.
  `sim.start()` starts every node, at virtual time 0.
- Pins are named as on the chip (`"PA5"`). `set_low()` / `set_high()` drive an input; `level`
  (and `history`) read an output.
- `sim.advance(ms=...)` runs virtual time; inputs take effect between these steps.
- `node.var("g_button_count")` reads a firmware global by name, from the debug info.

Swap in your board's executable, pins and variables. (The example's sensor board also sends a
CAN frame every 100 ms, and a node that transmits without a CAN bus stops with an error. This
test ends at 20 ms, before the first frame. A board without CAN has no such limit.)

### A test with a CAN bus

When the board talks CAN, give it a bus with a receiver. From the example's
`tests/test_sensor.py`:

```python
--8<-- "examples/sensor-node/tests/test_sensor.py:imports"


--8<-- "examples/sensor-node/tests/test_sensor.py:start"
```

```python
--8<-- "examples/sensor-node/tests/test_sensor.py:button"
```

A CAN frame needs an acknowledgement, so the bus gets a `CanDevice` that acknowledges every
frame. [CAN](../can.md) covers buses, devices and DBC decoding. The
[Writing tests](writing-tests/index.md) pages cover each area.

## Run the tests

```sh
uv run pytest tests -q
```

```text
.....                                                                    [100%]
5 passed in 0.37s
```

The first run creates `.venv/` and installs stest and pytest into it. When a test fails, the
report adds each node's last stderr lines, its firmware output and the merged event log
([pytest integration](writing-tests/pytest.md)).

## First errors

stest is strict: when the firmware does something it cannot simulate faithfully, the node
stops with an `stest: ...` message (exit status 70) instead of guessing. In a test this raises
`NodeCrashed`; its first line names the node and the virtual time, and the message is the
node's last stderr line. These were captured from small firmware variants.

### Unmodelled register or peripheral

```text
stest.errors.NodeCrashed: node 'app' aborted at virtual t=0.000 ms: stest: unmodelled write32 0x4000440c (exit status 70)
```

The firmware wrote 32 bits to address `0x4000440c` (`USART2->BRR`), and nothing in the chip
model covers it: stest has no USART model. An unimplemented register inside a modelled
peripheral names the peripheral, as `stest: unmodelled read32 <address> (<PERIPHERAL>)`.

Look the address up in the reference manual or the device header, then check the
[support matrix](../reference/support.md). Common fixes: leave the unmodelled peripheral's
initialisation out of the simulated build (for example with a `DEFINES` switch in your own code,
not in ST's), or add a model ([Adding a peripheral](../adding-a-peripheral.md)).

### Peripheral clock disabled

```text
stest.errors.NodeCrashed: node 'app' aborted at virtual t=0.000 ms: stest: access to GPIOB while its clock is disabled (RCC AHB2ENR bit 1) (write32 0x48000414) (exit status 70)
```

The firmware accessed GPIOB before enabling its clock. On hardware the write is silently
ignored; stest stops. Enable the clock first (`__HAL_RCC_GPIOB_CLK_ENABLE()`, normally in
`MX_GPIO_Init` or the `HAL_*_MspInit` in `stm32l4xx_hal_msp.c`). The message names the RCC
register and bit.

### `main` returned

```text
stest.errors.NodeCrashed: node 'app' aborted at virtual t=0.000 ms: stest: firmware main returned 0 at t=0 ns (exit status 70)
```

Firmware `main` must not return; on the chip there is nothing to return to. A CubeMX `main` ends
in `while (1)`. A firmware that does return (for example a test build that falls out of its
loop) stops the node.

### stest toolchain not included

At configure time, when `include(.../StestToolchain.cmake)` is missing or comes after
`project()`:

```text
CMake Error at /path/to/stest/CMakeLists.txt:12 (message):
  stest:
  include(/path/to/stest/cmake/StestToolchain.cmake)
  before project() in your top-level CMakeLists.txt (it selects the upstream
  clang the MMIO pass and firmware need).
```

(The paths are your stest checkout's.)

Move the include above `project()`. Delete `build/` before configuring again: CMake keeps the
compilers it chose in its cache.

More messages, and what each means, are in the [firmware reference](../reference/firmware.md)
and [Troubleshooting](../troubleshooting.md).
