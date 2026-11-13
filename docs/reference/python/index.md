# Python API reference

Everything a test needs is importable from the `stest` package:

```python
from stest import Sim, CanFrame, NodeCrashed
```

This reference is generated from the docstrings in `python/stest/`. For a guided tour, see
[Writing tests](../../guide/writing-tests/index.md).

## Conventions

- **Virtual time.** Arguments named `*_ms` are virtual milliseconds (floats are allowed,
  rounded to the nearest ns), `*_us` virtual microseconds, and `t_ns` / `*_ns` virtual
  nanoseconds (integers). Virtual time is the simulated MCU's time. It is deterministic and
  unrelated to how long the test takes. Only `wall_timeout_s` is real time.
- **Between quanta.** Nodes run in lockstep quanta (default 1 ms). Python sees and changes a
  node only between quanta, while it is paused. Logs, histories and model state are as of the
  last completed quantum, and waits are checked at quantum boundaries. Use
  [`Pin.drive_at`][stest.Pin.drive_at] or [`Node.post`][stest.Node.post] for inputs at an
  exact instant.
- **Units.** Voltages are in volts, frequencies in Hz, CAN bit rates in bit/s.
- **Errors.** stest raises [`StestError`][stest.StestError] subclasses; see
  [Errors](errors.md).

## Contents

| Name | What it is |
|---|---|
| [`Sim`][stest.Sim] | The simulation: nodes, virtual clock, waits. |
| [`Node`][stest.Node] | One firmware instance: variables, memory, logs, model calls. |
| [`Pin`][stest.Pin] | A GPIO pin: levels, external drive, analog voltage. |
| [`Timer`][stest.Timer], [`TimerChannel`][stest.TimerChannel] | Timer state, update rate and PWM duty. |
| [`Adc`][stest.Adc] | ADC input voltages. |
| [`Dac`][stest.Dac], [`DacChannel`][stest.DacChannel] | DAC outputs. |
| [`SpiBus`][stest.SpiBus], [`SpiBurst`][stest.SpiBurst] | SPI traffic and MISO data. |
| [`CanBus`][stest.CanBus] | A simulated CAN bus: log, waits, error injection. |
| [`CanDevice`][stest.CanDevice], [`DbcDevice`][stest.DbcDevice] | Python virtual CAN devices. |
| [`CanFrame`][stest.CanFrame], [`LoggedFrame`][stest.LoggedFrame] | CAN frames, as sent and as delivered. |
| [`Dbc`][stest.Dbc] | DBC encode and decode (optional `dbc` extra). |
| [`stest.can`](can.md#functions-and-constants) | Frame timing helpers and log formatters. |
| [`StestError`][stest.StestError] and subclasses | Errors. |

The [pytest plugin](../pytest.md) provides the `sim` fixture, options and the `stest` marker.

The modules `stest.protocol`, `stest.symbols` and `stest.macho` are internal (the control
protocol and debug-info readers) and may change without notice.
