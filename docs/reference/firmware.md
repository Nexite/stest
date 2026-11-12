# Firmware reference

Everything on the firmware side of stest: the CMake function that builds a firmware, the CMake
and environment variables, the standalone mode of a firmware executable, and every `stest: ...`
message with what it means and what to do. [Getting started](../guide/getting-started.md)
walks through a first build; [Concepts](../concepts.md) explains the rules behind the messages.

## `stest_add_firmware`

```cmake
stest_add_firmware(<name>
  CHIP <part>
  SOURCES <file>...
  [INCLUDE_DIRS <dir>...]
  [DEFINES <definition>...]
  [HAL_SOURCES <file>...]
  [MODELS <TYPE>=register|hal ...])
```

Builds the host executable `<name>` (in the current binary directory) from your firmware sources,
ST's HAL and the chip model. It is defined once stest is added to the build
(`add_subdirectory(<stest> stest)`, after `include(<stest>/cmake/StestToolchain.cmake)` and
`project()`; see [Getting started](../guide/getting-started.md#the-cmake-file)). Use absolute
paths.

| Option | Required | Meaning |
|---|---|---|
| `CHIP` | yes | The part, e.g. `STM32L496RG` (case-insensitive). It selects `chips/*/parts/<part>.toml`; see the [support matrix](support.md). |
| `SOURCES` | yes | Your C sources: `main.c`, `stm32l4xx_it.c`, `stm32l4xx_hal_msp.c` and your modules. Not ST's HAL or CMSIS, not `startup_*.s`, not `syscalls.c` / `sysmem.c`. A `system_stm32l4xx.c` here replaces ST's. |
| `INCLUDE_DIRS` | no | Your include directories, e.g. CubeMX's `Core/Inc` (it holds `stm32l4xx_hal_conf.h`). A directory with CMSIS-Core headers (`core_cm*.h`, `cmsis_gcc.h`, `cmsis_compiler.h`) is a configure error: stest provides CMSIS-Core and a host replacement for `cmsis_compiler.h`. |
| `DEFINES` | no | Extra preprocessor definitions for your sources and the HAL, e.g. `MY_FEATURE=1` (no `-D`). The device define (`STM32L496xx`) and `USE_HAL_DRIVER` are always set. |
| `HAL_SOURCES` | no | The ST HAL files to compile, **instead of** the default: every `stm32l4xx_hal*.c` of the HAL except `*_template.c`. The LL drivers (`stm32l4xx_ll_*.c`) are not in the default; list the LL files your firmware needs (with their full path) in `SOURCES`. |
| `MODELS` | no | Per peripheral type (the model kind upper-cased: `GPIO`, `TIM`, ...), `register` (the default) or `hal`. `hal` replaces ST's HAL file for that type with stest's shim ([register and HAL mode](../concepts.md#register-mode-and-hal-mode)). Only `GPIO=hal` exists. |

What it builds:

- Your sources and the HAL are compiled by stest's clang with the MMIO pass, with `-g -O0`
  whatever `CMAKE_BUILD_TYPE` is (the build type applies to stest's runtime). C sources are
  C11.
- Your sources (only) get `-Dmain=stest_firmware_main`: the runtime provides the real `main`.
- The HAL and ST's `system_stm32l4xx.c` go into a static library, so your definition of a
  `__weak` HAL function (`HAL_GPIO_EXTI_Callback`, `HAL_InitTick`, ...) or your own
  `system_stm32l4xx.c` wins.
- The chip glue generated from the part TOML supplies the interrupt vector table and a weak
  default for every handler, so handlers you do not define link. A default that runs aborts
  ([unhandled interrupt](#unhandled-interrupt)).
- Include order: stest's host `cmsis_compiler.h`, then `INCLUDE_DIRS`, the HAL's `Inc`, the
  CMSIS device `Include`, CMSIS-Core `Include`, stest's `runtime/include`.
- On macOS a post-build step writes `<name>.dSYM` (debug information for
  [`node.var`][stest.Node.var]). Keep it next to the executable.

Configure-time errors from `stest_add_firmware` name the firmware and say what to change:

| Message (after `stest_add_firmware(<name>): `) | Fix |
|---|---|
| `CHIP and SOURCES are required` / `unknown arguments ...` | Check the call against the signature above. |
| `chip '<CHIP>' -> 0 matches for chips/*/parts/<part>.toml` | The part is not supported (see the [support matrix](support.md)) or misspelled. |
| `the C compiler is ... but firmware must be built with ... (STEST_CLANG)` | Include `StestToolchain.cmake` before `project()` and do not set `CMAKE_C_COMPILER`; delete the build directory to reconfigure. |
| `clang major version ... differs from LLVM ... the MMIO pass was built against` | Use the clang of the same LLVM installation (`STEST_LLVM_ROOT`). |
| `INCLUDE_DIRS entry ... contains CMSIS-Core headers` | Remove your `Drivers/CMSIS/Include` from `INCLUDE_DIRS`. |
| `MODELS <spec>: unknown peripheral type ...` / `... has no hal shim ...` / `unknown mode ...` | The message lists the supported types and modes. |

Errors from the toolchain file (Apple clang, LLVM not found) are in
[Install](../install.md#install-failures); the "include the toolchain before `project()`" error
is in [Getting started](../guide/getting-started.md#first-errors).

## CMake variables

Set them with `-D<NAME>=<value>` on the first configure (they are cached).

| Variable | Default | Meaning |
|---|---|---|
| `STEST_LLVM_ROOT` | empty | Prefix of the upstream LLVM to use (it contains `bin/clang` and `lib/cmake/llvm`), e.g. `/usr/lib/llvm-18`. When empty, the `STEST_LLVM_ROOT` environment variable is used, then `brew --prefix llvm`, then `llvm-config` / `clang` on `PATH`. |
| `STEST_BUILD_TESTS` | `ON` when stest is the top-level project, `OFF` when it is added with `add_subdirectory` | Build and register stest's own tests (self-test firmware, unit tests, the speed gate). |
| `STEST_STM32L4_HAL_DIR` | empty (download the pinned commit) | An `stm32l4xx_hal_driver` checkout (`Inc/`, `Src/`) to build against instead. Not tested by CI. |
| `STEST_STM32L4_CMSIS_DEVICE_DIR` | empty (download) | A `cmsis_device_l4` checkout (`Include/`, `Source/Templates/`). Not tested by CI. |
| `STEST_CMSIS_CORE_DIR` | empty (download) | CMSIS-Core: a directory containing `core_cm4.h`, or a checkout whose `Include/` does. Not tested by CI. |

By default the ST sources are downloaded from ST's GitHub repositories at pinned commits and
checked by SHA-256 (`cmake/StestFetchST.cmake`). [Troubleshooting](../troubleshooting.md#a-different-hal-version)
covers using another HAL version.

## Environment variables

### Read by a node

A node (a firmware executable) reads these at startup, also in standalone mode. Under pytest the Sim sets them from its
settings when those are given; otherwise the node inherits them from the environment pytest runs
in, or from `sim.node(..., env={...})`. The
[pytest plugin reference](pytest.md#precedence) has the precedence rules.

| Variable | Values | Meaning |
|---|---|---|
| `STEST_POLL_SKIP` | integer >= 0, default 64 | [Poll fast-forward](../concepts.md#poll-fast-forward) threshold in store-free loads; `0` disables it. Anything else exits with status 2 (`stest: bad STEST_POLL_SKIP ...`). |
| `STEST_STRICT_ANALOG` | `1` to enable | Converting an ADC channel with no input voltage aborts the node instead of reading 0 V with a warning. Any other value: off. |
| `STEST_DEBUGGER` | `0` to detach | `0` gives the bare-board power-on state (ITM locked and disabled, `DHCSR.C_DEBUGEN` and `DEMCR.TRCENA` clear). Any other value or unset: a debugger is attached. |

Set by the Python harness for every node it starts (internal, listed for completeness):

| Variable | Meaning |
|---|---|
| `STEST_CTRL` | File descriptor of the control socket. When set, the node runs under Python's control instead of standalone. |
| `STEST_PARENT_PID` | The Python process; the node exits as soon as it dies. |
| `STEST_ACCESS_COST_NS` | Virtual ns charged per register access, from `Sim(access_cost_ns=...)` (default 25, also the standalone default). |

### Read at configure time

| Variable | Meaning |
|---|---|
| `STEST_LLVM_ROOT` | LLVM prefix, used when the CMake variable of the same name is empty. |

### Read by stest's own test suite

| Variable | Meaning |
|---|---|
| `STEST_SELFTEST_BIN` | Directory of the self-test executables for `python/tests` (default `build/selftest`; CTest sets it). |
| `STEST_PERF_STRICT` | `1` enforces the tight CAN speed budget in `test_can_speed` (load-sensitive, so opt-in). |

## Standalone mode

A firmware executable runs on its own, without Python, for a fixed span of virtual time:

```sh
./build/selftest/blinky --run-ms 2100
```

| Flag | Meaning |
|---|---|
| `--run-ms N` | Required. Run N ms of virtual time, then exit 0. |
| `--quantum-us Q` | Run in quanta of Q us of virtual time (default 1000), printing the events at the end of each. Event times do not depend on it, except in loops stretched by [poll fast-forward](../concepts.md#poll-fast-forward). |

It prints every event on stdout, one line each, in virtual-time order:

- `EVT <t_ns> GPIO <port><pin> <level>` for a pin change, e.g. `EVT 500001825 GPIO A5 1` (PA5
  high at 500.001825 ms);
- `EVT <t_ns> <json>` for every other event, with sorted keys, including the firmware's own
  stdout lines: `EVT 10000225 {"kind":"stdout","periph":"stdout","text":"hi"}`.

The firmware's stdout appears only as these events. Exit status:

| Status | When |
|---|---|
| 0 | `--run-ms` reached. |
| 2 | Bad arguments (`stest: --run-ms is required in standalone mode`, `bad --run-ms value`, `unknown argument`, each followed by `usage: <exe> --run-ms N [--quantum-us Q]`) or a bad `STEST_POLL_SKIP`. |
| 70 | A strict abort: the `stest: ...` message on stderr, then on stdout the events up to the abort and any unterminated output line (as an event with `"partial":true`). |
| `main`'s return value | The firmware's `main` returned. stderr has `stest: firmware main returned <code> at t=<ns> ns`. (Under Python this is an abort.) |

## Messages

Every message starts with `stest: ` and is written to the node's stderr. Under pytest a strict
abort raises [`NodeCrashed`][stest.NodeCrashed], whose first line is

```text
node '<name>' aborted at virtual t=<ms> ms: stest: <message> (exit status 70)
```

followed by the node's last stderr lines. In the messages below, an access is written
`<read|write><bits> <address>`, e.g. `write32 0x48000414`.

### Strict aborts

The node stops (exit status 70) at the access, at the virtual time it happened.

#### Unmodelled address

```text
stest: unmodelled write32 0x40013800
```

No model covers the address: usually a peripheral stest does not model (here USART1). Check the
address in the reference manual and the [support matrix](support.md). Keep the peripheral's
code out of the simulated build (a `DEFINES` switch in your code) or
[add a model](../adding-a-peripheral.md).

#### Unimplemented register

```text
stest: unmodelled read32 0x4800002c (GPIOA)
```

The peripheral is modelled, but not this register (here `GPIOA` offset 0x2C, `ASCR`, which the
STM32L496 does not have). Firmware or a HAL built for a different part, or a register the model
leaves out (see the model's header from the [support matrix](support.md)).

#### Write to a read-only register

```text
stest: write to read-only register 0x48000010 (write32, GPIOA)
```

The register has no writable bit (here `GPIOA->IDR`). On hardware the write does nothing; it is
almost always a bug, so stest stops. Registers that merely contain some read-only bits accept
writes, which those bits ignore, as on hardware.

#### Peripheral clock disabled

```text
stest: access to GPIOB while its clock is disabled (RCC AHB2ENR bit 1) (write32 0x48000414)
```

The firmware accessed a peripheral whose RCC enable bit is clear. Enable the clock first
(`__HAL_RCC_GPIOB_CLK_ENABLE()`, usually in `MX_GPIO_Init` or the `HAL_*_MspInit` function).
The message names the RCC register and bit.

#### Feature not modelled

```text
stest: ADC1: scan mode (SQR1.L=1: a sequence of 2 conversions) not modelled (write32 0x50040030 = 0x00000001)
```

The model implements the register but not the feature this write selects. The form is
`<peripheral>: <feature> not modelled (<access>[ = <value written>])`. The
[support matrix](support.md) lists what each model leaves out. Common ones: DMA, input
capture, ADC scan and continuous modes, `NVIC_SystemReset()` (`SCB: system reset
(AIRCR.SYSRESETREQ) not modelled`).

#### Model errors

```text
stest: exception in model at 0x50040008: ADC1 has no clock: RCC CCIPR ADCSEL selects none
stest: exception in clock event: ADC1 channel 16 converted with no input set
```

A model rejected what the firmware asked of it: a sequence the reference manual leaves undefined
(enabling an ADC that is already enabled, starting a conversion before it is ready), a missing
clock, or, with strict analog, an ADC input the test did not set. The first form happens at a
register access (at the address shown), the second in a scheduled model event (a conversion
completing, for example). The text after the colon says what is wrong.

#### Firmware `main` returned

```text
stest: firmware main returned 1 at t=11003375 ns
```

On the chip there is nothing to return to, so under Python a returning `main` is an abort.
CubeMX's `main` ends in `while (1)`; look for a path that leaves the loop.

#### Inbox messages

```text
stest: inbox message for GPIOC at t=1000000 ns is in the past (now 2000000 ns; op drive_pin)
stest: inbox message for GPIOZ at t=3000000 ns (op drive_pin): unknown model
stest: inbox message for GPIOC at t=3000000 ns (op bogus): unknown op: bogus
```

An exact-time input from [`Pin.drive_at`][stest.Pin.drive_at] or [`Node.post`][stest.Node.post]
could not be applied: its time was before the node's current time, the model or op does not
exist, or the model rejected the arguments (the model's error follows the colon). Use a time at
or after `sim.now_ns`, and the model names and ops from the model's header.

#### Unhandled interrupt

```text
stest: unhandled IRQ TIM7 (55)
stest: unhandled exception HardFault (-13)
```

An interrupt or exception was taken, but the firmware defines no handler for it, so the weak
default ran. Usually an interrupt was enabled (`HAL_NVIC_EnableIRQ`, or CubeMX's NVIC settings)
without its `<name>_IRQHandler` in `stm32l4xx_it.c`, or the handler's file is missing from
`SOURCES`. The number is the IRQ number (negative for core exceptions).

#### Breakpoints and sleeping forever

```text
stest: __BKPT(0) reached
stest: exception in stest_rt_wfi: wfi: nothing can wake the core (no pending exception, no clock event, no quantum end)
```

The firmware executed `__BKPT()` (often in an error handler), or slept with nothing that could
ever wake it (`stest_rt_wfe: wfe: ...` for `__WFE`). A related model error is
`SysTick clock is 0 Hz (SystemCoreClock not set?)`: SysTick was enabled while the firmware's
`SystemCoreClock` was 0.

#### CAN wiring

These come from the Python side and also raise `NodeCrashed`:

```text
node 'app' CAN1 requested a transmission but is not wired to a bus: Sim.node('app', ..., can={'CAN1': bus}) (at virtual t=100.000 ms)
node 'app' CAN1 entered normal mode at virtual t=1.000 ms: bit rate 250000 does not match bus 'can' (500000) (check the CAN bit timing: Prescaler, BS1, BS2, PCLK1)
```

Wire the controller to a bus with `sim.node(..., can={"CAN1": bus})`, and make the firmware's
bit timing give exactly the bus's bit rate. See [CAN](../can.md#buses-and-nodes).

#### Internal errors

`stest: control protocol: ...`, `stest: control channel: ...`, `stest: unknown exception ...`,
`stest: cannot reserve address window ...` and `stest: host image mapped below 4 GiB ...` point
to a harness or platform problem rather than your firmware. Please report them with the test
that triggers them.

### Warnings

The node keeps running. Under pytest they are in the `stderr` section of a failure report.

```text
stest: warning: ADC1 channel 16 converted with no input set (pin PB1); reading 0 V
```

An ADC channel was converted before the test gave its pin a voltage. Set one with
`pin.set_voltage(...)`, or make this an abort with [strict analog](../guide/writing-tests/faults.md#require-every-analog-input).
Printed once per ADC and channel.

```text
stest: warning: firmware accessed GPIOA ODR directly while GPIO is in hal mode
```

The firmware was built with `MODELS GPIO=hal` but also accesses the GPIO registers directly. It
works; printed once per register.

```text
stest: warning: ITM TER write ignored: the ITM is locked (write 0xC5ACCE55 to ITM->LAR first, as a debugger does)
```

Without a debugger attached, the firmware configured the ITM without unlocking it. As on
hardware the write is ignored. Printed once.

```text
stest: ITM unterminated {"port":0,"text":"partial"}
stest: stdout unterminated {"text":"half"}
```

The node stopped (or a standalone run ended) with a line not yet terminated by `\n`. The text is
also the last entry of `node.output` (and `node.log` for ITM port 0), and an event with
`"partial":true`.

`stest: <n> events of the aborting quantum not reported (too large)` means an abort happened in
a quantum with more events than the error report can carry; the events before that quantum are
all in `sim.event_log`.

### Usage errors

```text
stest: bad STEST_POLL_SKIP '-1' (expected an integer >= 0)
stest: --run-ms is required in standalone mode
usage: ./build/selftest/blinky --run-ms N [--quantum-us Q]
```

Exit status 2, before the firmware starts.
