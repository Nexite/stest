# stest

stest is an open-source (Apache-2.0) software-in-the-loop simulator for STM32 firmware. Firmware is compiled natively with upstream LLVM clang plus the stest pass plugin, which rewrites volatile (memory-mapped I/O) accesses into calls to a simulated peripheral runtime, so unmodified firmware sources (HAL included) run on a host machine in deterministic virtual time (nanoseconds). A Python harness (a pytest plugin) drives one or more firmware nodes in lockstep, reads and writes pins and firmware variables, and reports failures with the nodes' stderr and a merged event log. Plan 1 ships the STM32L496 chip package and a blinky self-test firmware.

## Requirements

- Upstream LLVM 18 or newer (clang, llvm-config and the LLVM CMake package; Apple clang is not supported), CMake 3.23+, Ninja
- Python 3.11+ and [uv](https://docs.astral.sh/uv/)
- Platforms: macOS arm64 and Linux (x86-64, arm64) natively. On Windows, or for zero setup, use the Docker image (`docker/Dockerfile`, Ubuntu 24.04 with LLVM 18) or the dev container in `.devcontainer/`.

macOS: `brew install llvm cmake ninja uv`. Ubuntu 24.04: `apt install clang-18 llvm-18-dev lld-18 cmake ninja-build` and `curl -LsSf https://astral.sh/uv/install.sh | sh` (or `pipx install uv`), then `export STEST_LLVM_ROOT=/usr/lib/llvm-18`.

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # add -DSTEST_LLVM_ROOT=<llvm prefix> if LLVM is not found
cmake --build build
```

With the image instead:

```sh
docker build -t stest docker
docker run --rm -v "$PWD":/w stest sh -c 'cmake -S . -B build-docker -G Ninja && cmake --build build-docker'
```

## Run blinky standalone

```sh
./build/selftest/blinky --run-ms 2100
```

Standalone, the firmware runs against the simulated chip for the given span of virtual time without a harness, and prints its events: `EVT 500001825 GPIO A5 1` is PA5 (the LED) going high at 500.0018 ms of virtual time (GPIO events use this `EVT <t_ns> GPIO <port><pin> <level>` form; every other kind, including the firmware's own stdout lines, is printed as `EVT <t_ns> <json>`).

## Run the tests

```sh
ctest --test-dir build              # C++ runtime, pass, self-test firmware, and the Python tests
uv run pytest python/tests -q       # the Python tests alone
```

The self-test binaries are found in `build/selftest` (or `$STEST_SELFTEST_BIN`).

## Testing firmware with pytest

Installing the package (`uv sync`) registers the `stest` pytest plugin, which provides the `sim` fixture. This is the blinky LED test:

```python
def test_blinky_toggles_pa5(sim):
    node = sim.node("b", "build/selftest/blinky")
    sim.start()
    led = node.pin("PA5")
    led.wait_high(timeout_ms=600)          # virtual time; the host does not sleep
    sim.advance(ms=1600)
    assert [lvl for _, lvl in led.history] == [1, 0, 1, 0]
    assert node.var("uwTick") > 0          # firmware variables by name
```

Options: `--stest-quantum-us` (lockstep quantum, default 1000) and `--stest-wall-timeout` (wall-clock seconds for any blocking wait, default 30). Override per test with `@pytest.mark.stest(quantum_us=100, wall_timeout_s=5)`. When a test fails, the report includes each node's last 200 stderr lines and the merged event log.

Analog inputs: `node.pin("PB1").set_voltage(1.65)` (or `node.adc(1).set(16, 1.65)`, resolved through the part TOML's `[[analog]]` pin table) applies a voltage the ADC converts (only analog-capable pins accept one: ADC inputs from the `[[analog]]` table and DAC outputs; any other pin raises `StestError`); `node.pin("PA4").voltage` and `node.dac(1).channel(1).volts` read a DAC output. Converting a channel with no voltage set reads 0 V and prints one warning per channel; make it abort instead with `Sim(strict_analog=True)`, `--stest-strict-analog`, `@pytest.mark.stest(strict_analog=True)`, or the node env var `STEST_STRICT_ANALOG=1`.

Firmware logging: **use `node.output` and `node.wait_for_log(...)` (default `source="any"`) for firmware logs; they work the same on every host.** `node.output` is every line the firmware logged, through ITM port 0 or stdout (`printf`, `puts`, `putchar`, `fwrite(..., stdout)`), merged in virtual-time order. Code that picks its log backend by architecture (`#if defined(__ARM_ARCH)` is true on arm64 hosts, false on x86-64) therefore reads the same on macOS arm64 and Linux x86-64. `node.wait_for_log(r"hello (\d+)", timeout_ms=500)` advances the simulation to the next unconsumed matching line and returns the `re.Match`; `source="itm"` or `source="stdout"` restricts it to one backend. Every chip maps core-generic ITM, DWT and CoreDebug models, so CMSIS `ITM_SendChar` and DWT `CYCCNT` work unchanged. `node.log` is the list of ITM port-0 lines only (text before each `\n`; `node.itm_port(n)` for other ports), and `node.stdout_tail` holds the last 200 raw lines of the node's stdout pipe. The runtime replaces the firmware's C `stdout` stream (macOS `funopen`, Linux `fopencookie`), so each stdout line is also an event `{"kind":"stdout","periph":"stdout","text":"..."}` stamped in virtual time; a raw `write(1, ...)` bypasses it (it still reaches `stdout_tail`). ITM lines are also events (`{"kind":"itm","periph":"ITM","port":0,"text":"..."}`, stamped in virtual time). By default a debugger is attached (ITM enabled, `DEMCR.TRCENA`/`DHCSR.C_DEBUGEN` set); for the bare-board power-on state, where `ITM_SendChar` drops output unless the firmware unlocks (`ITM->LAR = 0xC5ACCE55`) and enables the ITM itself, use `Sim(debugger_attached=False)`, `--stest-no-debugger`, `@pytest.mark.stest(debugger_attached=False)` or the node env var `STEST_DEBUGGER=0`. A failing test's report adds `stest node <name> output`, `log` (ITM) and `stdout` sections. `CYCCNT` is derived from virtual time and `SystemCoreClock`; DWT comparators and other unmodelled trace features abort.

Poll fast-forward: after 64 store-free loads that re-read already-seen (address, width, value) tuples, a node jumps its clock to the next scheduled event or the quantum end. For loops that poll a hardware value this is exact. A loop that counts iterations in a non-volatile local while reading constant volatile values (`while (--n && !(REG & FLAG));`, flag never set) is time-dilated by it (potentially hundreds of times longer than on hardware, and quantum-dependent when no event is pending). Control the threshold with `Sim(..., poll_skip=N)`, `--stest-poll-skip N`, `@pytest.mark.stest(poll_skip=N)`, or the node env var `STEST_POLL_SKIP=N`; `0` disables it, the default is 64.

HAL mode: by default every peripheral is simulated at the register level (ST's HAL compiles unchanged and its register accesses reach the models). `stest_add_firmware(<name> CHIP ... SOURCES ... MODELS GPIO=hal)` instead replaces ST's `stm32l4xx_hal_gpio.c` with stest's shim (`chips/stm32l4/hal/`), which drives the GPIO model directly while keeping its registers, pins, EXTI setup, event times and IRQs identical to register mode. Firmware that still writes a hal-mode block directly works but gets one stderr warning per register (`stest: warning: firmware accessed GPIOA ODR directly while GPIO is in hal mode`). `MODELS <TYPE>=register` is the default; unknown types and types without a shim (only GPIO has one so far) fail at configure time.

## Design

See the [design spec](docs/superpowers/specs/2026-10-30-stest-sil-design.md).

## License

Apache-2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE).
