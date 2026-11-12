# Troubleshooting

Symptoms, their usual causes, and what to do. For the toolchain, see
[Install failures](install.md#install-failures). For the text of every `stest: ...` message, see
the [firmware reference](reference/firmware.md#messages).

## The node aborts

### `NodeCrashed: ... stest: unmodelled ...` during initialisation

The firmware touched a peripheral (or a register) stest does not model. The address tells you
which: look it up in the device header (`stm32l496xx.h`) or the reference manual, then check the
[support matrix](reference/support.md). CubeMX projects often initialise a UART, I2C or DMA in
`main()` that a test does not need.

- Keep that initialisation out of the simulated build with a switch in **your** code, set
  through `DEFINES` in `stest_add_firmware` (`#ifndef SIL_BUILD` around `MX_USART2_UART_Init()`,
  for example). ST's code stays unchanged.
- Or [add a model](adding-a-peripheral.md).

### `... not modelled` for a feature you use

The model covers the peripheral but not the mode your firmware selects (DMA, input capture, ADC
scan mode, ...). The message names the register bits. Use a supported mode in the simulated build,
or extend the model. The header linked from the [support matrix](reference/support.md) lists
what is covered.

### `... while its clock is disabled`

The firmware accessed a peripheral before enabling its RCC clock. On hardware the access is
silently lost, which hides a real bug. Enable the clock (`__HAL_RCC_<PERIPH>_CLK_ENABLE()`)
before the first access, usually in `HAL_<PPP>_MspInit` or `MX_GPIO_Init`.

### `firmware main returned`

`main` returned, which the chip cannot do. Check the paths out of the main loop.

### `NodeCrashed: node '...' crashed at virtual t=... ms: killed by signal SIGSEGV` (or `SIGBUS`)

The firmware crashed the host process: a null or wild pointer, a stack overflow, or code that
assumes a 32-bit address space (casting a pointer to `uint32_t` and back). On the chip this
would be a HardFault; stest has no fault exceptions. Run the executable standalone under a
debugger (`lldb` / `gdb`, `--run-ms N`) to find the faulting line.

## The test hangs, or a wait times out

### `WallClockTimeout: node '...' did not answer within 30 s`

The firmware is running but virtual time stopped: a loop with no register access never reaches
the end of the quantum. Typical causes:

- CubeMX's `Error_Handler()` (`__disable_irq(); while (1) {}`): a HAL call returned an error.
  The node's last virtual time in the message tells you when. Check which HAL call failed, for
  example by logging in `Error_Handler` before the loop.
- An idle loop `while (1) {}` waiting for interrupts. Interrupts are taken at register accesses
  and in `__WFI()`; call `__WFI()` in idle loops.
- A busy-wait on a plain (non-volatile) variable.

Lower `wall_timeout_s` (marker or `--stest-wall-timeout`) to fail sooner.

### `WaitTimeout` for something that should happen

- The firmware has not configured the peripheral yet, or a HAL call failed early (look at
  `node.output` and the failure report's stderr section).
- An input set from the test is not seen: the firmware waits in `__WFI()` and only reads it
  after the next interrupt. See [WFI and WFE](concepts.md#wfi-and-wfe).
- A frame on CAN is never delivered because nobody acknowledges it: add a
  [`CanDevice`][stest.CanDevice] to the bus.
- The budget is virtual time: a HAL timeout of 1000 ms needs a `timeout_ms` above 1000.

## Timing looks wrong

### A software timeout takes far too long

A loop that counts iterations while polling a register (`while (--n && !(REG & FLAG));`) is
stretched by [poll fast-forward](concepts.md#poll-fast-forward). Turn it off for the test with
`@pytest.mark.stest(poll_skip=0)` ([recipe](guide/writing-tests/faults.md#make-a-polling-loop-exact)).
Timeouts based on `HAL_GetTick()` are exact.

### Results change with the quantum

Inputs set between quanta (`set_high()`, `set_voltage()`, `set_var()`) take effect at a quantum
boundary, so their virtual time moves with the quantum. Use
[`Pin.drive_at`](guide/writing-tests/pins.md#exact-edge-times) or `Node.post` for exact times. See
[Determinism](concepts.md#determinism).

### Results differ between macOS and Linux

Firmware that chooses its log backend with `#if defined(__ARM_ARCH)` logs through ITM on arm64
hosts and `printf` on x86-64. Read logs with `node.output` and `node.wait_for_log`, which merge
both ([Firmware output](guide/writing-tests/output.md)).

Both hosts are 64-bit, unlike the chip: code that stores a pointer in a `uint32_t`, or assumes
`long` is 32 bits, behaves differently from the chip on both.

## Variables and output

### `SymbolError` from `node.var`

- `no DWARF debug info`: the executable was not built by `stest_add_firmware`, or on macOS its
  `<exe>.dSYM` directory was not copied with it.
- Unknown name: the variable is a local (only globals and `static` variables have an address),
  or the linker dropped it because nothing uses it.
- `ambiguous symbol`: several files define a `static` with that name; use `"file.c:name"`.

### An ADC reads 0

The test never set the pin's voltage: the node printed
`stest: warning: ADC1 channel <n> converted with no input set (pin P..); reading 0 V`. Call
`node.pin(...).set_voltage(...)`. Use [strict analog](guide/writing-tests/faults.md#require-every-analog-input)
to turn the warning into a failure.

### ITM output is missing

With `debugger_attached=False` (or `STEST_DEBUGGER=0`) the ITM starts locked and disabled, as
on a board without a probe, so `ITM_SendChar` drops the output. Unless the test is about that
state, keep the default.

## Building your firmware

### A different HAL version

stest builds every firmware against its pinned STM32L4 HAL, which its models are tested with.
Your CubeMX project may use another HAL version. Usually that does not matter: the HAL's
register-level behaviour changes little between versions. Symptoms of a mismatch:

- your code calls a HAL function or macro that the pinned HAL does not have (a compile or link
  error);
- `stm32l4xx_hal_conf.h` from a newer CubeMX enables a module the pinned HAL lacks;
- the HAL touches a register or feature in a way the models do not cover (an abort during
  initialisation).

To build against your own HAL, set `STEST_STM32L4_HAL_DIR` and
`STEST_STM32L4_CMSIS_DEVICE_DIR` ([CMake variables](reference/firmware.md#cmake-variables)). This
is not tested by CI.

### LL drivers

The default HAL sources are `stm32l4xx_hal*.c`, so the full LL drivers (`stm32l4xx_ll_*.c`,
`LL_GPIO_Init` and friends) are not built. The inline LL functions in the headers work. For the
others, add the `.c` files to `SOURCES` and `USE_FULL_LL_DRIVER` to `DEFINES`. stest's copy of
the HAL is in the directory that the CMake function `stest_st_family_dirs(stm32l4 <hal_var>
<device_var>)` returns. This is not tested by CI.

### Duplicate or missing symbols at link time

- `main`: do not define `stest_firmware_main` yourself; `stest_add_firmware` renames your `main`.
- `_sbrk`, `_write`, `_exit` and other newlib stubs: leave `syscalls.c` and `sysmem.c` out of
  `SOURCES`. The firmware uses the host's C library.
- `Reset_Handler`, the vector table: leave `startup_stm32l4*.s` out.

### RTOS

RTOS ports for Cortex-M (such as FreeRTOS's `ARM_CM4F` port) contain Cortex-M assembly and do
not compile for the host. stest runs bare-metal firmware with the HAL.

## Processes

### Node processes left behind

Nodes exit when the Python process that started them dies. In a container where pytest is
process 1, killed node processes are not reaped and remain as zombies until the container
exits; they use no CPU.

### A test is slow

- A small quantum (`quantum_us`) or a CAN bus (which caps the quantum at its shortest frame time)
  means more round trips per virtual second.
- `poll_skip=0` makes polling loops run every iteration.
- Long `advance` spans with busy firmware cost host time; idle firmware in `__WFI()` costs
  almost none.
