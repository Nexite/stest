# Multiple nodes

A test can run several firmware nodes (boards) in one simulation. Each node is its own process
with its own simulated MCU; the `Sim` advances them together, quantum by quantum, on one virtual
clock. Recipes use the [imports and `firmware` fixture](index.md#how-the-recipes-run) of the
cookbook.

## Two boards in one test

**Goal:** run two firmware images side by side and check each.

```python
--8<-- "python/tests/test_recipes.py:two-nodes"
```

- Add every node before `sim.start()`. Node names must be unique; they label the node's events,
  its error messages and its frames on a CAN bus.
- The nodes stop at the same virtual time after every `advance` or wait. In between they run in
  parallel on the host, so a two-node test does not take twice as long.
- `sim.event_log` is ordered by virtual time, then by the order the nodes were added, so it is
  the same on every run.
- A node aborting, crashing or hanging ends the whole simulation: every node is stopped and the
  test gets [`NodeCrashed`][stest.NodeCrashed] or [`WallClockTimeout`][stest.WallClockTimeout]
  naming the node ([Fault scenarios](faults.md)).
- Nodes share nothing but virtual time and the buses you connect them with. Two nodes built from
  the same executable are fully independent (their own memory, pins and peripherals).

## Boards that talk over CAN

**Goal:** connect two boards' CAN controllers to one simulated bus and check a frame.

```python
--8<-- "python/tests/test_recipes.py:can"
```

- `sim.can_bus(name, bitrate)` creates a bus. `sim.node(..., can={"CAN2": bus})` connects the
  node's CAN2 controller to it. The firmware's bit timing must give exactly the bus's bit rate.
- [`bus.wait_for_frame(id=..., timeout_ms=...)`][stest.CanBus.wait_for_frame] advances until a
  matching frame has been delivered and returns it with its sender and end-of-frame time.
- A frame needs an acknowledgement. One board alone on a bus retries forever unless something
  else acknowledges; add a [`CanDevice`][stest.CanDevice], as in
  [Getting started](../getting-started.md#a-test-with-a-can-bus).
- A bus caps the quantum at its shortest frame time (88 us at 500 kbit/s), so CAN tests run
  more quanta per virtual second.

The [CAN](../../can.md) page covers Python devices, DBC decoding, error injection, bus-off and
logs.

## Other links between boards

stest simulates CAN buses only. A board-to-board GPIO line, UART or SPI link has no built-in
connection: drive one node's input from the other's output in the test, between quanta, for
example with `Pin.history` and `Pin.drive_at`.
