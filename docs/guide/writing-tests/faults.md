# Fault scenarios

Tests for what goes wrong: a node that aborts, analog inputs a test forgot, a board without a
debugger, timing loops, firmware that hangs. Recipes use the
[imports and `firmware` fixture](index.md#how-the-recipes-run) of the cookbook.

## Assert that a node aborts

**Goal:** check that firmware stops on an access stest cannot simulate faithfully, or confirm
which access that is.

```python
--8<-- "python/tests/test_recipes.py:crash"
```

- A strict abort raises [`NodeCrashed`][stest.NodeCrashed]. Its first line is
  `node '<name>' aborted at virtual t=<ms> ms: stest: <message> (exit status 70)`, followed by
  the node's last stderr lines.
- The [firmware reference](../../reference/firmware.md#messages) lists every message, what it
  means and the usual fix.
- A host crash of the firmware (a segmentation fault, say) is also `NodeCrashed`:
  `node '<name>' crashed at virtual t=<ms> ms: <signal>`.
- After `NodeCrashed` every node has been stopped and the `Sim` is unusable; every later call
  raises `NodeCrashed` again. Logs, `sim.event_log` and pin histories, including the events
  of the aborting quantum, stay readable for the test's assertions.

## Require every analog input

**Goal:** make a test fail when the firmware converts an ADC channel the test never gave a
voltage, instead of reading 0 V.

```python
--8<-- "python/tests/test_recipes.py:strict-analog"
```

- Without strict analog, such a conversion reads 0 V and the node prints one warning per
  (ADC, channel): `stest: warning: ADC1 channel 16 converted with no input set (pin PB1); reading 0 V`.
- Turn it on for one test with the marker, for a whole run with `--stest-strict-analog`, or with
  `Sim(strict_analog=True)` ([settings](../../reference/pytest.md#settings)).

## Run without a debugger

**Goal:** test the bare-board power-on state, where nobody has enabled the ITM.

```python
--8<-- "python/tests/test_recipes.py:no-debugger"
```

- By default a debugger is attached, so CMSIS `ITM_SendChar` output appears in `node.log`. With
  `debugger_attached=False` the ITM starts locked and disabled, as on a board without a probe:
  `ITM_SendChar` drops its output (without blocking), and writes to the ITM's TER, TPR and TCR
  registers are ignored until the firmware unlocks it (`ITM->LAR = 0xC5ACCE55`).
- Use it to check that logging code does not hang or misbehave on a production board.
- Other forms: `--stest-no-debugger`, `Sim(debugger_attached=False)`.

## Make a polling loop exact

**Goal:** measure a loop that counts its own iterations while polling a register, such as a
timeout counter.

```python
--8<-- "python/tests/test_recipes.py:poll-skip"
```

- [Poll fast-forward](../../concepts.md#poll-fast-forward) skips ahead when firmware keeps
  re-reading registers that do not change. That is exact for a loop that waits on hardware, but
  it stretches a loop that counts iterations (`while (--n && !(REG & FLAG));`), so a software
  timeout expires far too late in virtual time.
- `poll_skip=0` turns fast-forward off for the test; every access then costs its 25 ns. The
  default threshold is 64 loads. A test with long idle polling runs slower on the host without
  it.
- Other forms: `--stest-poll-skip N`, `Sim(poll_skip=N)`.

## A hung node

**Goal:** fail fast when firmware stops making progress.

```python
--8<-- "python/tests/test_recipes.py:hang"
```

- Virtual time advances only when the firmware accesses a register or sleeps. A loop with no
  register access (an empty `for (;;) {}`, or a fault handler that spins) never reaches the end
  of the quantum, so the node never answers.
- `wall_timeout_s` (default 30 real seconds) bounds every wait for a node. It raises
  [`WallClockTimeout`][stest.WallClockTimeout] naming the node and the last virtual time it
  reported, and stops every node.
- Interrupts are taken at register accesses (and in `__WFI` / `__WFE`), so an empty
  `for (;;) {}` hangs even with interrupts enabled and SysTick running, as `faults_spin` does.
  Idle loops in firmware should call `__WFI()`, which also costs no host time while asleep.

## Firmware that returns from `main`

A firmware `main` that returns is a strict abort (`stest: firmware main returned <code> at t=<ns>
ns`): on the chip there is nothing to return to. Tests that expect it use the same pattern as
[Assert that a node aborts](#assert-that-a-node-aborts) with `match="firmware main returned"`.
