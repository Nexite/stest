# pytest integration

Installing stest registers its pytest plugin, which provides the `sim` fixture, command-line
options, the `stest` marker and extra sections in failure reports. This page shows them in use;
the [pytest plugin reference](../../reference/pytest.md) lists every setting, its default and
the precedence rules. Recipes use the [imports and `firmware` fixture](index.md#how-the-recipes-run)
of the cookbook.

## Settings for one test: the marker

**Goal:** change a simulation setting for one test (or a class or module).

```python
--8<-- "python/tests/test_recipes.py:marker"
```

- The [marker's keys](../../reference/pytest.md#the-stest-marker) are the settings in the
  [reference](../../reference/pytest.md#settings). A marker on a class or module
  (`pytestmark = pytest.mark.stest(...)`) applies to its tests; the closest marker wins as a
  whole.
- A smaller quantum makes waits stop closer to the event and inputs set between quanta land
  closer to where you put them, at the cost of host time (more round trips per virtual second).
  For exact input times use [`Pin.drive_at`](pins.md#exact-edge-times) instead.

## Settings for a whole run: command-line options

**Goal:** run the suite with other settings without editing tests, for example a finer quantum
or a shorter wall-clock limit in CI. These commands are examples, not tested by CI:

<!-- not tested by CI -->
```sh
uv run pytest tests -q --stest-quantum-us 100
uv run pytest tests -q --stest-wall-timeout 10 --stest-strict-analog
```

- Every option is in the [settings table](../../reference/pytest.md#settings). A marker on a
  test wins over an option ([precedence](../../reference/pytest.md#precedence)).
- To make them the default for a project, put them in `addopts` in your `pyproject.toml`'s
  `[tool.pytest.ini_options]`.

## Read a failure report

When a test that uses `sim` fails, the report adds, per node, its last stderr lines (where
`stest: ...` messages appear), its ITM log, firmware output and stdout, then the merged event
log and the last frames of each CAN bus. This one is from a failing SPI test (one node, `app`):

```text
--------------------- Captured stest node app stderr call ----------------------
(no stderr output)
----------------------- Captured stest node app log call -----------------------
(no ITM output)
--------------------- Captured stest node app output call ----------------------
(no firmware output)
--------------------- Captured stest node app stdout call ----------------------
(no stdout output)
-------------------------- Captured stest events call --------------------------
2125 app {"kind":"gpio","level":1,"periph":"GPIOA","pin":4,"port":"A"}
10001800 app {"kind":"gpio","level":0,"periph":"GPIOA","pin":4,"port":"A"}
10005425 app {"kind":"spi","miso":"1020","mosi":"a501","periph":"SPI1","t_start":10002225}
10008925 app {"kind":"spi","miso":"3040","mosi":"0203","periph":"SPI1","t_start":10005725}
10009700 app {"kind":"gpio","level":1,"periph":"GPIOA","pin":4,"port":"A"}
```

Each event line is `<t_ns> <node> <event JSON>`. The sections and their limits are in the
[reference](../../reference/pytest.md#failure-reports). Passing tests get no extra sections.

## Keep CAN logs

`--stest-can-log DIR` writes every CAN bus's frames, after every test that has buses, to
`DIR/<test>-<bus>.log` in candump log format (`candump -l`), which can-utils' `canplayer` and many CAN tools read. A failure
report shows the last 200 frames of each bus even without the option.

## Check that a test is deterministic

**Goal:** prove that the same inputs give the same result, run after run, and that the result
does not depend on the quantum.

```python
--8<-- "python/tests/test_recipes.py:determinism"
```

- Identical firmware, inputs and quantum give a byte-identical event log. A test that passes
  once passes every time.
- Inputs applied with `drive_at` (or `Node.post`) are at exact times, so the log is also the same
  for every quantum. Inputs set between quanta (`set_high()`, `set_voltage()`) land on a quantum
  boundary and can move with it. [Concepts](../../concepts.md#determinism) lists what a quantum
  can change.
