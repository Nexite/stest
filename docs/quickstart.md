# Quickstart

Five minutes from a clone to a passing firmware test. You need the toolchain from
[Install](install.md) (or the Docker image).

## 1. Clone stest

The clone itself is not tested by CI (CI starts from a checkout):

<!-- not tested by CI -->
```sh
git clone https://github.com/Nexite/stest.git
cd stest
```

## 2. Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds the pass plugin, the runtime, the STM32L496 chip models and stest's self-test
firmware. One of them is `build/selftest/blinky`: an ordinary CubeMX blinky (`HAL_Init`, the
clock setup, then `HAL_Delay(499)` and `HAL_GPIO_TogglePin` on PA5 in a loop, which toggles every
500 ms because `HAL_Delay` adds one tick), compiled for your computer.

## 3. Run blinky on its own

```sh
./build/selftest/blinky --run-ms 2100
```

The firmware runs for 2100 ms of virtual time against the simulated chip, with no Python, and
prints what happened:

```text
EVT 500001825 GPIO A5 1
EVT 1000001825 GPIO A5 0
EVT 1500001825 GPIO A5 1
EVT 2000001825 GPIO A5 0
```

Each line is an event: the virtual time in nanoseconds, then what changed. `EVT 500001825 GPIO
A5 1` is PA5 (the LED) going high at 500.001825 ms. The extra 1.8 us is virtual time
spent in the firmware's startup code (every register access is charged a little time) before
the HAL's 1 ms tick started. GPIO
changes use this short `EVT <t_ns> GPIO <port><pin> <level>` form; every other event, including
lines the firmware prints, is `EVT <t_ns> <json>`. The run takes a fraction of a second of wall time:
virtual time does not wait for the host.

Run it again and you get the same bytes. That is the point of virtual time (see
[Concepts](concepts.md)).

## 4. Test it with pytest

The same firmware, driven from a test:

```python
--8<-- "examples/quickstart/test_blinky.py:test"
```

Run it from the root of the checkout:

```sh
uv run pytest examples/quickstart -q
```

```text
.                                                                        [100%]
1 passed in 0.10s
```

What happened:

- uv created `.venv/` with stest's Python package. Installing it registers the stest pytest
  plugin, which provides the `sim` fixture.
- `sim.node("b", "build/selftest/blinky")` started the firmware as its own process, under
  control of the test. The path is relative to where you run pytest.
- `led.wait_high(timeout_ms=600)` advanced virtual time until PA5 went high, at most 600 ms.
- `sim.advance(ms=1600)` ran 1.6 virtual seconds. `led.history` holds every change of the pin
  as `(t_ns, level)`.
- `node.var("uwTick")` read the HAL's tick counter from the firmware's memory, by name.

## Next

- Test **your own** firmware: [Getting started with your own project](guide/getting-started.md).
- How it works: [Concepts](concepts.md).
- Everything a test can do: [Writing tests](guide/writing-tests/index.md) and the
  [Python API reference](reference/python/index.md).
