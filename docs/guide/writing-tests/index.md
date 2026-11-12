# Writing tests

These pages are a cookbook: one recipe per task, each with a goal, the code, and notes. They
show how to use the API. The [Python API reference](../../reference/python/index.md) has every
signature and option, and the [pytest plugin reference](../../reference/pytest.md) has every
setting.

| Page | Covers |
|---|---|
| This page | The `sim` fixture, nodes, advancing virtual time, waits and timeouts |
| [Pins](pins.md) | Driving inputs, reading outputs, pin history, exact-time inputs, EXTI |
| [Firmware variables](variables.md) | Reading and writing globals, structs, arrays and enums by name |
| [Firmware output](output.md) | `printf` and ITM logs, waiting for a line, output events |
| [Peripherals](peripherals.md) | Timers and PWM, ADC inputs, DAC outputs, SPI traffic |
| [Multiple nodes](multi-node.md) | Several boards in one test, CAN in brief |
| [CAN](../../can.md) | CAN buses, Python devices, DBC, error injection |
| [Fault scenarios](faults.md) | Asserting a node aborts, strict analog, debugger, poll fast-forward, hangs |
| [pytest integration](pytest.md) | Markers, command-line options, failure reports, CAN logs, determinism checks |

## How the recipes run

Every recipe is a complete test from stest's own test suite
([`python/tests/test_recipes.py`](https://github.com/Nexite/stest/blob/main/python/tests/test_recipes.py)).
CI runs it on Linux and macOS against stest's self-test firmware (`build/selftest/`), so the
code on these pages works as shown. The firmware each recipe uses is described in a comment
inside the test.

The recipes use these imports:

```python
--8<-- "python/tests/test_recipes.py:imports"
```

`firmware("blinky")` is a fixture that returns the path of a built executable. In your own
project it is the `firmware` fixture from
[Getting started](../getting-started.md#finding-the-executables-conftestpy), and your board's
name replaces `blinky`.

## Start a node and run time

**Goal:** run firmware for a while, then wait until something happens.

```python
--8<-- "python/tests/test_recipes.py:basics"
```

- The `sim` fixture is a new, unstarted [`Sim`][stest.Sim] for each test. It is closed
  (every node process stopped) when the test ends, pass or fail.
- `sim.node(name, path)` adds a node. Add every node (and CAN bus) before `sim.start()`.
- `sim.advance(ms=..., us=..., ns=...)` runs exactly that much virtual time. It takes no
  wall-clock time beyond what the host needs to simulate it.
- `sim.wait_for(cond, timeout_ms=...)` checks `cond()` now and at every quantum boundary, so
  it stops at the end of the quantum in which the condition became true. With the default
  1 ms quantum that is up to 1 ms late. [Pins](pins.md#exact-edge-times) shows how to get exact
  times.
- `sim.now_ns` is the current virtual time in nanoseconds.

## Expect a timeout

**Goal:** assert that something does *not* happen within a time budget.

```python
--8<-- "python/tests/test_recipes.py:timeout"
```

- Every wait (`wait_for`, `Pin.wait_high`, `Node.wait_for_log`, `CanBus.wait_for_frame`, ...)
  raises [`WaitTimeout`][stest.WaitTimeout] after `timeout_ms` of **virtual** time, with
  `sim.now_ns` at the end of the budget. The simulation stays usable.
- A different limit, `wall_timeout_s` (real seconds, default 30), stops a node that does not
  answer at all. See [Fault scenarios](faults.md#a-hung-node).

## Use `Sim` without the fixture

**Goal:** run a simulation outside pytest's `sim` fixture, for example two simulations in one
test or a script.

```python
--8<-- "python/tests/test_recipes.py:no-fixture"
```

- `Sim(...)` takes the same settings as the [marker and options](../../reference/pytest.md#settings).
- Use it as a context manager, or call `sim.close()`, so no node process outlives the test.
- Only the `sim` fixture adds stest's sections to pytest's failure report.

## Background

[Concepts](../../concepts.md) explains virtual time, quanta, determinism and strictness, which
these recipes rely on.
