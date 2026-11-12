# CAN in stest

stest simulates classic CAN (ISO 11898-1, 0-8 data bytes) buses in Python and connects
firmware nodes to them through a register-level bxCAN model (STM32L4: CAN1, CAN2). Firmware
and ST's HAL compile unchanged: `HAL_CAN_Init`, filters, `HAL_CAN_AddTxMessage`, FIFO
interrupts and error callbacks run against the model. Frames, arbitration, ACK, error frames
and fault confinement are decided by a bus engine on the global virtual clock, so every
timing in a CAN test is deterministic and independent of the quantum.

The reference documentation lives next to the code; this guide is the map:

| What | Where |
|---|---|
| Bus engine (algorithm, timing, errors, fault confinement) | module doc of `python/stest/can/bus.py` |
| Frames (bit length, CRC, arbitration, lookahead) | `python/stest/can/frame.py` |
| Python devices | `python/stest/can/device.py` |
| DBC | `python/stest/can/dbc.py` |
| Log formats (candump, Vector ASC) | `python/stest/can/log.py` |
| bxCAN model (registers, events, `deliver` ops, deviations) | header comment of `chips/stm32l4/models/bxcan.hpp` |
| Self-test firmware | `selftest/firmware/can/main.c` (exes `can_node1`, `can_node2`, `can_badrate`, `can_err`, `can_abom`) |
| Tests | `python/tests/test_can_*.py`, `chips/stm32l4/tests/test_bxcan.cpp` |

## Buses and nodes

```python
from stest import CanDevice, CanFrame, Sim

with Sim() as sim:
    bus = sim.can_bus("drive", 500_000)          # before sim.start(); bitrate in bit/s (int, <= 1 Mbit/s)
    n1 = sim.node("n1", "build/selftest/can_node1", can={"CAN2": bus})
    n2 = sim.node("n2", "build/selftest/can_node2", can={"CAN2": "drive"})   # a bus or its name
    sim.start()
    lf = bus.wait_for_frame(id=0x102, timeout_ms=50)   # virtual time
    assert lf.sender == "n2" and lf.frame.dlc == 1
```

`sim.node(name, exe, can={"<controller>": bus})` wires a node's CAN controllers to buses of
the same Sim. On each bus the node is the sender `name`. A node may use several controllers,
one per bus. Each wiring mistake fails loudly instead of leaving a silent, disconnected
controller:

- When `sim.node(...)` is called: an unknown bus name, a bus of another Sim, two controllers
  of one node on the same bus, or a name already used by a sender on that bus raises `StestError`.
- At `sim.start()`: a controller name that is not a CAN controller of the firmware's chip
  raises `StestError` naming the valid controllers (`CAN1, CAN2` on the L496). Every
  wired name is checked with the model's `can_info` op.
- At run time:
  - A controller that requests a transmission (`can_tx`) without being wired to a bus ends
    the simulation with `NodeCrashed`, which names the `can={...}` fix. Initialising a
    controller the test does not wire is fine.
  - A controller that enters normal mode at a bit rate different from its bus's also ends the
    simulation with `NodeCrashed` ("bit rate X does not match bus ..."). The comparison is exact:
    PCLK1 // ((BRP+1)(1+TS1+1+TS2+1)) must equal `CanBus.bitrate`. A real node with the
    wrong bit timing only produces error frames.

A Sim with buses and no firmware nodes is valid, for example to unit-test Python devices.

### Frames, the log and waits

- `CanFrame(id, data=b"", ext=False, rtr=False, dlc=None)` is immutable. Its `bit_length()`
  is exact, including stuff bits and the CRC.
- `bus.log` is a list of `LoggedFrame(t_ns, bus, frame, sender)` in end-of-frame (EOF) order.
  `t_ns` is the EOF. A frame is appended at the first quantum end at or after its EOF.
- `bus.wait_for_frame(id=None, match=None, timeout_ms=...)` advances the simulation until a
  logged frame matches, then returns it. It consumes the log up to that frame, so successive
  waits never return the same frame. After `timeout_ms` of virtual time it raises `WaitTimeout`.
- `bus.send(frame, sender="python")` requests a frame now from a send-only sender. A
  send-only sender never ACKs.
- `bus.export_candump(path)` writes the log in candump format, and `bus.export_asc(path)`
  writes Vector ASC (channel = bus creation index + 1, fixed 1970 date for determinism).

Firmware-side frames are also events in `sim.event_log`: `can_mode`, `can_tx` and
`can_tx_abort` (see `bxcan.hpp`).

## Python devices

```python
class Echo(CanDevice):
    def on_frame(self, lf):                        # at the frame's EOF (self.now_ns == lf.t_ns)
        if lf.frame.id == 0x1F0:
            self.send(CanFrame(0x7E8, lf.frame.data))

echo = bus.attach(Echo("echo"))                    # receives frames whose SOF is >= now
tick = bus.attach(CanDevice("tick"))
tick.every(10, lambda: tick.send(CanFrame(0x300, b"\x01")))   # every 10 virtual ms
```

- Device code runs on the virtual clock. `on_frame` runs at the frame's EOF and `every`
  callbacks run at their due time. Inside a callback `now_ns` is that instant and `send()`
  requests at it, so device timing never depends on the quantum. Callbacks execute at the
  first quantum end at or after their time.
- `ack=True` (the default) acknowledges frames like a controller in normal mode. With
  `CanDevice(name, ack=False)` the device only listens. `ack` is fixed once the device is attached.
- A device's pending frames contend in identifier order, and its queue is unbounded.
- Devices are always error active and keep no error counters. An error on their frame is
  retried at once, forever: two devices whose frames collide with the same identifier and
  different data retry against each other forever, as on a real bus.
- An exception in `on_frame` or an `every` callback ends the simulation with a `StestError`
  naming the device, with the original exception chained.

## DBC

```python
from stest import Dbc, DbcDevice

dbc = Dbc("vehicle.dbc")                           # cantools: DBC (also KCD/SYM)
tester = bus.attach(DbcDevice("tester", dbc))
tester.send_message("Request", Cmd=1)
tester.send_periodic("Request", 100, {"Cmd": 2})   # or a callable returning the dict
t_ns, sig = bus.wait_for_message(dbc, "Status", timeout_ms=200, where=lambda s: s["Mode"] == "Run")
history = bus.messages(dbc)                        # [(t_ns, name, signals)] of every known frame
```

- `Dbc.encode(name, signals)` returns a `CanFrame` and `Dbc.decode(frame)` returns
  `(name, signals)` or `None`. Values are physical (scaled), and `VAL_` enums decode to their
  name and encode from the name or the raw value. A missing signal, an out-of-range value, an
  unknown message or a frame whose length does not fit raises `StestError`, with cantools'
  exception chained. Extended and standard identifiers never cross-match.
- A `DbcDevice` keeps the latest decoded value of every message it has seen in `received`
  (`{name: (t_ns, signals)}`).
- A frame with a known identifier that does not decode (wrong length) raises from
  `DbcDevice.on_frame` and therefore ends the simulation.
- `wait_for_message` shares `wait_for_frame`'s consumption cursor.
- Licensing: DBC support uses cantools (MIT), which depends on python-can (LGPL-3.0). Both
  are imported runtime dependencies of the Apache-2.0 stest package. Neither is vendored or
  modified.

## Errors and fault confinement

The bus engine produces error frames for three causes:

- **ACK error:** nobody acknowledged the frame (a lone node, or only `ack=False` listeners).
- **Injected error:** `bus.inject_errors(count=1, target=None)` corrupts the next `count`
  frames whose SOF is at or after the current virtual time, right after the arbitration field.
  With `target`, only that sender's frames are corrupted and counted. Calls queue up, and a
  frame takes the first call that applies to it. Each retransmission is a new frame, so 32
  injected errors drive a node to bus-off.
- **Bit error:** two senders win arbitration with the same identifier but different
  control/data bits.

The engine models error timing as follows:

- An error frame occupies the bus for 20 bit times, followed by the IFS.
- The transmitter retries unless the mailbox has NART set.
- An error-passive transmitter waits 8 extra bit times after its own frame (suspend
  transmission).

The bus engine is the single owner of each firmware node's error counters:

- A transmit error adds 8 to TEC. An ACK error while the node is error passive leaves TEC
  unchanged, so a lone node stays at 128.
- A receive error adds 1 to REC, saturating at 255.
- A successful transmission subtracts 1 from TEC.
- A successful reception subtracts 1 from REC, or sets REC to 120 when it was above 128
  (RM0351).
- The node is error passive when TEC or REC is above 127, and bus-off when TEC is above 255.
  A bus-off node neither transmits, ACKs nor receives, and its mailboxes stay pending.

The engine delivers absolute counter values to the bxCAN model. The model shows them as
follows:

- `ESR.TEC` is the low byte of TEC, so a bus-off TEC of 256 reads 0. `ESR.REC` is REC.
- `EWGF` is set when either counter is 96 or more, `EPVF` when either is above 127, and
  `BOFF` when TEC is above 255.
- `LEC` holds the last error code.
- `MSR.ERRI` and the SCE interrupt fire on rising EWG/EPV/BOF flags and on a non-zero LEC,
  each when its IER enable is set. This drives `HAL_CAN_ErrorCallback`.

Bus-off recovery takes 128 occurrences of 11 recessive bits while the controller is in
normal mode:

- With `MCR.ABOM = 1` (`AutoBusOff = ENABLE`), counting starts at bus-off.
- With ABOM = 0, counting starts only after the firmware requests init mode and then leaves
  it (`HAL_CAN_Stop` / `HAL_CAN_Start`).
- The node rejoins one lookahead after the 128th occurrence, with TEC = REC = 0. That instant
  is quantum-independent and is a Clock event, so a firmware polling `ESR.BOFF` is never
  fast-forwarded past it.
- `MCR.RESET` zeroes the counters and leaves bus-off.

You can observe fault confinement in two ways:

- `bus.errors` is a list of `{"t_ns", "kind", "sender"}` entries in time order. The kinds
  `ack`, `injected` and `bit` are error flags, with one entry per transmitter. The kinds
  `error_passive`, `bus_off` and `error_active` are firmware nodes' state changes.
- `node.call("CAN2", "state")` returns `mode`, `bitrate`, `tec`, `rec`, `error_state`
  (`active` | `passive` | `bus_off`), the raw `esr` / `msr` / `tsr`, the TX mailbox states and
  the FIFO fill levels.

## Quantum cap

The engine decides each frame at its start of frame, and its results must never land in a
node's past. A Sim with a CAN bus therefore caps its quantum at `lookahead_ns(bitrate)`, the
time of 44 bits, which is shorter than any real frame. At 500 kbit/s the cap is 88 µs, and at
1 Mbit/s it is 44 µs. With several buses, the fastest bus sets the cap.

- `Sim(quantum_us=1000)`, `--stest-quantum-us` and the `stest` marker request a quantum.
  `sim.quantum_ns` shows the capped value, and the cap is logged at `start()`.
- The cap costs host time, because each quantum is one socket round trip per node. Measured
  on an M3 Pro (`docs/spikes/2026-10-30-interception-gate.md`, "CAN quantum"):
  - The round trips alone cost about 0.065 s of wall time per virtual second per node
    natively and 0.10 s in a Linux VM. x86-64 CI figures are pending.
  - With firmware and the bus engine, two nodes exchanging frames take about 0.18 s per node
    per virtual second natively (`test_can_speed`).
  - The worst case is a lone node retrying a frame nobody ACKs, at about 0.35 s. It is
    measured by `uv run spike/run_gate.py --quantum-us 88 --can-worst-case`.
- Results do not depend on the quantum: every frame, result and callback happens at the same
  virtual time for any quantum at or below the cap. The tests check this at quanta of 88, 37,
  13, 7 and 1 µs.

## Logs in pytest

The `sim` fixture adds two features for CAN:

- On failure, the report has a `stest can <bus>` section per bus with the last 200 delivered
  frames in candump format, `(<s>.<us>) <bus> <id>#<data>`. Older frames are summarised as
  `... (N earlier frames omitted)`, and a bus with no frames shows `(no frames)`.
- `--stest-can-log DIR` writes `DIR/<test>-<bus>.log` after every test whose `sim` has CAN
  buses, whether it passed or failed. The file holds the bus's full log in candump format,
  identical to `bus.export_candump`. Characters other than letters, digits, `_`, `.` and `-`
  in the test and bus names become `_` (`test_x[a/b]` gives `test_x_a_b_-drive.log`). Tests
  with the same name in different modules overwrite each other's files.

For a `Sim` you create yourself (not the fixture), call `bus.export_candump()` or
`bus.export_asc()`.

## Deviations and limits

These are the deliberate simplifications. Their details are in the two reference files
(`bus.py` and `bxcan.hpp`).

- **Error frames:** every error frame is 20 bit times long, with no separate passive-flag
  timing. The only error causes are the three above: no form or CRC errors (receivers of an
  injected or bit error report a stuff error). Overload frames are not modelled.
- **Arbitration and requests:** the bus is decided at each start of frame, with no bit-level
  arbitration inside a frame. A request stamped exactly at a quantum end joins the next
  decision, so the outcome never depends on where quantum boundaries fall.
- **Busy-bus bus-off recovery:** each transmission counts as one occurrence of 11 recessive
  bits. RM0351 only requires "at least" 128 x 11 bits.
- **bxCAN mode entry:** INRQ or SLEEP during a reception enters init or sleep at once and
  drops the frame being received. Hardware finishes that frame first.
- **bxCAN ABRQ:** an ABRQ on a mailbox withdrawn by a mode change completes when the engine
  answers, not at once.
- **bxCAN MCR.RESET:** resets BTR, IER, ESR and the mailboxes too.
- **bxCAN filters:** CAN2's FMI counts from 0 at bank CAN2SB, and nothing is received while
  `FMR.FINIT = 1`.
- **Unmodelled bxCAN features** abort the node strictly when configured:
  - time-triggered mode (TTCM)
  - wake-up on bus activity (AWUM, WKUIE)
  - loop-back and silent modes (LBKM, SILM)
  - DLC above 8
- **Strict bxCAN aborts where hardware ignores the write:** writes to BTR outside init mode,
  to a busy TX mailbox, or to the filter configuration outside `FINIT`.
- **Python devices** have no fault confinement (see above).
- **Unbounded lists:** `bus.log` and `bus.errors` grow without bound. Endless no-ACK retries
  produce about 1.4 MB of `errors` entries per virtual second.
- **Bit rates:** each bus has one bit rate (no CAN FD), and a node's bit rate must match its
  bus's exactly.

## Adding another CAN controller model

To add another controller model (for example FDCAN), implement the same contract as bxCAN:

- the `can_info` call op (`{"type", "role"}`)
- the events `can_mode`, `can_tx` and `can_tx_abort`
- the `deliver` ops `tx_result`, `rx`, `error` and `counters`

`Node._can_flush` (`python/stest/node.py`) and `_node_can_callbacks` (`python/stest/sim.py`)
then wire the model to a bus unchanged. See `docs/adding-a-peripheral.md`, section 8, for the
inbox `deliver()` pattern.
