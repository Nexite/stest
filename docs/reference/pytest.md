# pytest plugin reference

Installing stest registers its pytest plugin (`stest.pytest_plugin`, through the `pytest11`
entry point). It provides the `sim` fixture, command-line options, the `stest` marker and
extra sections in failure reports. The guide page
[pytest integration](../guide/writing-tests/pytest.md) shows them in use.

## The `sim` fixture

```python
def test_led_blinks(sim):
    node = sim.node("app", "build/selftest/blinky")
    sim.start()
    node.pin("PA5").wait_high(timeout_ms=600)
```

- Function-scoped: every test gets a new [`Sim`][stest.Sim].
- The Sim is configured from the marker, the options and the defaults below, but not started.
  Add nodes (and CAN buses), then call `sim.start()`.
- It is closed on teardown, also when the test failed, raised or timed out, so no node
  process outlives the test.

## Settings

| Setting | Command-line option | Marker key | `Sim(...)` argument | Node environment variable | Default |
|---|---|---|---|---|---|
| Lockstep quantum | `--stest-quantum-us US` | `quantum_us=` | `quantum_us` | none | 1000 (virtual µs) |
| Wall-clock timeout per node wait | `--stest-wall-timeout SECONDS` | `wall_timeout_s=` | `wall_timeout_s` | none | 30.0 (real seconds) |
| Poll fast-forward threshold | `--stest-poll-skip N` | `poll_skip=` | `poll_skip` | `STEST_POLL_SKIP=N` | 64 store-free loads; `0` disables |
| Strict analog inputs | `--stest-strict-analog` (flag) | `strict_analog=` | `strict_analog` | `STEST_STRICT_ANALOG=1` | off: an unset ADC input reads 0 V with one warning per (ADC, channel) |
| Debugger attached | `--stest-no-debugger` (flag, turns it off) | `debugger_attached=` | `debugger_attached` | `STEST_DEBUGGER=0` (turns it off) | attached: ITM output works |
| CAN log files | `--stest-can-log DIR` | none | none | none | off |

The MMIO access cost (`Sim(access_cost_ns=25)`) has no option or marker key.

### Precedence

For each setting: **marker > command-line option > node environment variable > default.**

- A marker key, when present, wins. A marker can switch a flag back off, e.g.
  `@pytest.mark.stest(strict_analog=False)` under `--stest-strict-analog`, or
  `debugger_attached=True` under `--stest-no-debugger`.
- The two flags act only when given: `--stest-strict-analog` turns strict analog on, and
  `--stest-no-debugger` turns the debugger off. Without the flag or a marker key, the plugin
  passes `None` for that setting, and so does `--stest-poll-skip` when it is not given.
- `None` means the node process inherits the environment variable, from the environment
  pytest runs in or from `sim.node(..., env={...})`. Without the variable, the runtime default
  applies. `STEST_STRICT_ANALOG` is strict only when it is `"1"`, and `STEST_DEBUGGER`
  detaches only when it is `"0"`.
- `quantum_us` and `wall_timeout_s` have no environment variable: their options default to
  1000 and 30.

### The `stest` marker

```python
@pytest.mark.stest(quantum_us=100, poll_skip=0, strict_analog=True, debugger_attached=False)
def test_fine_grained(sim): ...
```

| Key | Type | Meaning |
|---|---|---|
| `quantum_us` | `int` | Lockstep quantum in virtual microseconds. |
| `wall_timeout_s` | `float` | Real-time limit in seconds for every blocking wait on a node. |
| `poll_skip` | `int` | Poll fast-forward threshold; `0` disables it. |
| `strict_analog` | `bool` | `True`: converting an ADC channel with no input voltage aborts the node. |
| `debugger_attached` | `bool` | `False`: bare-board power-on state, where `ITM_SendChar` drops output. |

- Only keyword arguments are accepted. Any other key, or a positional argument, raises
  `pytest.UsageError` when the fixture is set up.
- Only the closest marker applies: a function marker replaces a class or module marker
  entirely. Keys are not merged across levels.
- Values are passed to `Sim(...)` unchanged, so an invalid value, such as a negative
  `poll_skip`, raises the `ValueError` from [`Sim`][stest.Sim].

### `--stest-can-log DIR`

After every test whose `sim` has CAN buses, passed or failed, each bus's full log is written
as `DIR/<test>-<bus>.log` in candump format. This is the same text as
[`CanBus.export_candump`][stest.CanBus.export_candump].

- `<test>` is the test's name, with parameters, and `<bus>` is the bus name. In both,
  characters other than letters, digits, `_`, `.` and `-` are replaced by `_`.
- `DIR` is created if needed.
- Existing files are overwritten, including those of same-named tests in different modules.

## Failure reports

When a test that uses `sim` fails in setup, call or teardown, the report gets these sections:

| Section | Content |
|---|---|
| `stest node <name> stderr` | The node's last 200 stderr lines (runtime warnings, `stest: ...` abort messages). |
| `stest node <name> log` | The last 200 ITM port-0 lines ([`Node.log`][stest.Node.log]). |
| `stest node <name> output` | The last 200 firmware output lines, ITM port 0 and stdout in virtual-time order ([`Node.output`][stest.Node.output]). |
| `stest node <name> stdout` | The last 200 raw lines of the node's stdout pipe ([`Node.stdout_tail`][stest.Node.stdout_tail]). |
| `stest events` | The last 500 entries of [`sim.event_log`][stest.Sim] as `<t_ns> <node> <event JSON>`, with a count of earlier events omitted. Present only when there are events. |
| `stest can <bus>` | The last 200 frames delivered on each CAN bus, in candump format. |

The node sections appear once per node. Passing tests get no extra sections.
