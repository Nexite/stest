# Firmware output

Firmware logs through `printf` (stdout) or the Cortex-M ITM (CMSIS `ITM_SendChar`, often
behind a SWO `printf`). stest captures both, stamped in virtual time. Recipes use the
[imports and `firmware` fixture](index.md#how-the-recipes-run) of the cookbook.

!!! tip "Use `node.output` and `node.wait_for_log`"
    They see every line the firmware logged, through ITM port 0 or stdout, in virtual-time
    order, so the test reads the same on every host. That matters for firmware that picks its
    log backend by architecture: `#if defined(__ARM_ARCH)` is true on arm64 hosts (macOS arm64,
    Linux arm64) and false on x86-64, so such firmware logs through ITM on one host and `printf`
    on another.

## Read what the firmware logged

**Goal:** check log lines, whichever backend printed them.

```python
--8<-- "python/tests/test_recipes.py:output"
```

- [`node.output`][stest.Node.output]: every complete line (text before each `\n`) from ITM
  port 0 and stdout, merged in virtual-time order.
- [`node.log`][stest.Node.log]: ITM port 0 only. [`node.itm_port(n)`][stest.Node.itm_port]:
  another stimulus port.
- [`node.stdout_tail`][stest.Node.stdout_tail]: the last 200 raw lines of the node's stdout
  pipe. A raw `write(1, ...)` reaches only this list: stest replaces the C `stdout` stream
  (with `funopen` on macOS, `fopencookie` on Linux), so `printf`, `puts`, `putchar` and
  `fwrite(..., stdout)` are captured, but file descriptor 1 itself is not.
- A line not yet terminated by `\n` is not in these lists. If the node aborts, it is flushed
  into them as a final, partial line.

## Wait for a log line

**Goal:** run until the firmware prints something, then use what it printed.

```python
--8<-- "python/tests/test_recipes.py:wait-for-log"
```

- [`node.wait_for_log(pattern, timeout_ms)`][stest.Node.wait_for_log] advances to the next
  line that matches the regular expression (`re.search`) and returns the `re.Match`. Lines it
  has passed are consumed: the next call starts after them.
- `source="itm"` or `source="stdout"` restricts it to one backend; the default `"any"` reads
  `node.output`.
- Like every wait, it checks at quantum boundaries and raises
  [`WaitTimeout`][stest.WaitTimeout] when the budget runs out.

## Read the event log

**Goal:** check the exact virtual time of a line, or correlate it with pin changes and other
events.

```python
--8<-- "python/tests/test_recipes.py:events"
```

- `sim.event_log` is a list of `(t_ns, node_name, json_text)` for every event of every node:
  pin changes, output lines, SPI bursts, DAC outputs, CAN activity. Keys in the JSON are sorted;
  parse it rather than matching the text.
- Output events: `{"kind":"itm","periph":"ITM","port":0,"text":"..."}` and
  `{"kind":"stdout","periph":"stdout","text":"..."}`. A line flushed at an abort or at the end of
  a standalone run also has `"partial": true`.

## ITM and the debugger

By default stest models a board with a debugger attached: the debugger has enabled the ITM and
trace (`DEMCR.TRCENA`, `DHCSR.C_DEBUGEN`, ITM port 0), so CMSIS `ITM_SendChar` works unchanged.
For the bare-board power-on state, where `ITM_SendChar` drops its output, see
[Fault scenarios](faults.md#run-without-a-debugger). DWT `CYCCNT` works in both states once the
firmware enables it; it counts virtual time times `SystemCoreClock`.

## In failure reports

When a test that uses the `sim` fixture fails, pytest's report adds each node's output, ITM log,
stdout and stderr, and the merged event log
([failure reports](../../reference/pytest.md#failure-reports)).
