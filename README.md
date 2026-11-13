# stest

**stest runs your STM32 firmware on your computer, in deterministic virtual time, and lets you test it with pytest.**

Your firmware and ST's unmodified HAL are compiled natively with upstream LLVM clang plus an LLVM pass. The pass turns every memory-mapped register access into a call into the stest runtime, which routes it to a register-level model of the peripheral. A firmware build is an ordinary host executable; a Python harness (a pytest plugin) runs one or more of them in lockstep, drives pins, analog inputs and CAN buses, and reads back events, logs and firmware variables by name.

**Documentation: <https://nexite.github.io/stest/>**

## Features

- **Unmodified firmware.** Your sources and ST's HAL compile as they are: no `#ifdef SIMULATION`, no stubs.
- **Register-level peripheral models** for the STM32L496: RCC, PWR, FLASH, GPIO, EXTI/SYSCFG, timers, ADC, DAC, SPI and bxCAN, plus the Cortex-M core (NVIC, SysTick, WFI/WFE), ITM and DWT.
- **Deterministic virtual time** in nanoseconds: the same inputs give a byte-identical event log, and the host never sleeps.
- **Strict by default:** an unmodelled register, a write to a read-only register or access to a peripheral whose clock is off aborts with a message naming the address, instead of returning a silent zero.
- **pytest:** a `sim` fixture, pins, firmware variables by name, firmware logs (ITM and stdout), and failure reports with each node's output and event log.
- **Multi-node CAN:** firmware nodes and Python devices (with optional DBC support) on simulated CAN buses, with error injection and fault confinement.

Supported today: the STM32L496 on macOS arm64 and Linux x86-64 / arm64 hosts (Docker image or dev container on Windows). See the [support matrix](https://nexite.github.io/stest/reference/support/) for exactly what is modelled.

## Quickstart

You need upstream LLVM 18+ (not Apple clang), CMake 3.23+, Ninja, Python 3.11+ and [uv](https://docs.astral.sh/uv/); [Install](https://nexite.github.io/stest/install/) has the commands per platform and the Docker image. Cloning is not tested by CI (CI starts from a checkout):

<!-- not tested by CI -->
```sh
git clone https://github.com/Nexite/stest.git
cd stest
```

Build stest, its chip models and its self-test firmware, then run the blinky firmware for 2.1 virtual seconds on its own:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/selftest/blinky --run-ms 2100
```

```text
EVT 500001825 GPIO A5 1
EVT 1000001825 GPIO A5 0
EVT 1500001825 GPIO A5 1
EVT 2000001825 GPIO A5 0
```

The same firmware under pytest (`examples/quickstart/test_blinky.py`):

```python
def test_blinky_toggles_pa5(sim):
    node = sim.node("b", "build/selftest/blinky")
    sim.start()
    led = node.pin("PA5")
    led.wait_high(timeout_ms=600)  # virtual time; the host does not sleep
    sim.advance(ms=1600)
    assert [lvl for _, lvl in led.history] == [1, 0, 1, 0]
    assert node.var("uwTick") > 0  # firmware variables by name
```

```sh
uv run pytest examples/quickstart -q
```

The [Quickstart](https://nexite.github.io/stest/quickstart/) explains each step.

## Documentation

- [Install](https://nexite.github.io/stest/install/): toolchain per platform, building stest and running its tests, install failures.
- [Getting started with your own project](https://nexite.github.io/stest/guide/getting-started/): build a CubeMX project under stest and write its first test ([`examples/sensor-node`](examples/sensor-node) is the worked example).
- [Concepts](https://nexite.github.io/stest/concepts/): virtual time and quanta, determinism, strictness, WFI/WFE, poll fast-forward, register and HAL mode.
- [Writing tests](https://nexite.github.io/stest/guide/writing-tests/): recipes for pins, variables, firmware output, peripherals, multiple nodes, [CAN](https://nexite.github.io/stest/can/), fault scenarios and pytest options.
- Reference: [Python API](https://nexite.github.io/stest/reference/python/), [pytest plugin](https://nexite.github.io/stest/reference/pytest/), [firmware build, environment variables and messages](https://nexite.github.io/stest/reference/firmware/), [support matrix](https://nexite.github.io/stest/reference/support/).
- [Troubleshooting](https://nexite.github.io/stest/troubleshooting/).
- [Contributing](https://nexite.github.io/stest/contributing/): start with [AGENTS.md](AGENTS.md) and the [design spec](docs/superpowers/specs/2026-10-30-stest-sil-design.md).

## License

Apache-2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE). ST's HAL and CMSIS sources are downloaded at configure time and keep their own licenses. The optional DBC support uses cantools, which depends on python-can (LGPL-3.0); see [CAN](https://nexite.github.io/stest/can/).
