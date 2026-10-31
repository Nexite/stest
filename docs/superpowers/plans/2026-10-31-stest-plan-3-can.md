# stest Plan 3 — CAN: buses, bxCAN, virtual devices, DBC

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Multiple firmware nodes and Python virtual devices exchange CAN frames over any number of simulated buses defined in Python, with exact, deterministic frame timing and arbitration, a register-level bxCAN model (CAN1 + CAN2, filters in CAN1's block), DBC-aware virtual devices, error states, and standard log export.

**Architecture:** Buses live in Python, which already owns global time. Nodes report transmit requests as timestamped events; after each quantum the Python bus engine decides arbitration for every bus-idle instant it can now see, computes exact frame end times (bit stuffing + CRC), and delivers results back to nodes as timestamped inbox messages that the node applies as Clock events. Causality holds because the quantum is capped at the minimum frame duration on any bus (lookahead). A new protocol version carries the inbox in RUN_UNTIL and pending events in ERROR.

**Tech Stack:** as Plans 1–2; adds `cantools` (DBC) to Python dependencies.

**Spec:** `docs/superpowers/specs/2026-10-30-stest-sil-design.md` (§4.6 CAN buses) · Follow-ups consumed: `docs/superpowers/plans/2026-10-31-plan-2-followups.md` · Guides: `docs/adding-a-peripheral.md`, `AGENTS.md`

**Branch:** `plan-3-can` from `plan-2-peripherals` (PR #2 unmerged; rebase onto `main` once it merges).

## Global Constraints

- License Apache-2.0; no Sakura names, pins, or DBC files in stest (test DBCs are stest-owned and generic).
- Native macOS arm64 + Linux (x86-64, arm64), both in CI.
- Firmware and ST sources compile unchanged.
- Strict by default: unsupported bxCAN features abort with `NotModelled`; nothing silently diverges from hardware.
- Model contract: time-driven state changes are Clock events (or visible in reads).
- **Determinism:** identical inputs → byte-identical event logs and CAN logs, **including across any quantum ≤ the lookahead** (CAN frame times must not depend on quantum size).
- Speed: per node ≤ 0.4 s wall per virtual second; a 2-node CAN test at 500 kbit/s must stay within 2 × 0.4 s per virtual second.
- Time in ns ints; CAN IDs as ints; data as `bytes`.
- CAN frame format per ISO 11898-1 (classic CAN; CAN-FD is out of scope for bxCAN); bxCAN registers per RM0351 and `stm32l496xx.h` on disk (`build/_deps/stest_st_cmsis_device_l4-src/Include/stm32l496xx.h`).

## Review Focus

1. **Node bit timing ≠ bus bitrate** (wrong BTR or PCLK1) → strict abort naming both, e.g. `stest: CAN2: bit rate 250000 does not match bus 'drive' (500000)` when the node leaves init mode. *(Task 5 `test_bitrate_mismatch_aborts`)*
2. **A node transmits with nobody else on the bus** → no ACK: TERR, automatic retransmission (unless NART), TEC +8 per attempt up to error-passive 128 and no further (ISO rule), frame never delivered, firmware never hangs the sim. *(Task 7 `test_no_ack_alone`)*
3. **User quantum larger than the lookahead** → automatically capped to the lookahead with one log line; results identical to running with the capped quantum explicitly. *(Task 3 `test_quantum_capped_by_bus`)*
4. **Two senders request the same instant** → arbitration by ID (11-bit before 29-bit with the same base ID per IDE rules; data before remote for the same ID), deterministic tie-break when frames are bit-identical (both transmit, one delivery, both TX ok). *(Task 3 `test_arbitration_rules`)*
5. **Frame arrives at a node whose CAN is in init/sleep or whose filters reject it** → not received, no FIFO change; the node still doesn't ACK while in init/sleep. *(Task 5 `test_rx_rules`)*

## File structure

```
runtime/include/stest/protocol.h, runtime/src/control_channel.cpp   (modify) protocol v2
python/stest/protocol.py, node.py, sim.py                             (modify) v2, inbox, bus wiring
python/stest/can/__init__.py   frame.py   bus.py   device.py   dbc.py   log.py   (new)
chips/stm32l4/models/bxcan.{hpp,cpp}                                  (new)
chips/stm32l4/parts/stm32l496rg.toml                                  (modify) CAN1, CAN2
selftest/firmware/can/main.c                                          (new) CubeMX-style CAN node
python/tests/test_protocol_v2.py test_can_frame.py test_can_bus.py test_can_e2e.py test_can_dbc.py test_can_errors.py  (new)
python/tests/data/test.dbc                                            (new, generic)
docs/can.md                                                           (new) user guide
```

---

### Task 1: Protocol v2 — inbox on RUN_UNTIL, events on ERROR

**Files:** Modify `runtime/include/stest/protocol.h`, `runtime/src/control_channel.cpp`, `runtime/include/stest/node.hpp` (inbox dispatch), `python/stest/{protocol.py,node.py,sim.py}`; Test `python/tests/test_protocol_v2.py`, `python/tests/test_faults.py`.

**Interfaces — Produces:**
- `STEST_PROTO_VERSION 2`. RUN_UNTIL payload: `u64 t_ns, u32 n_msgs, n × {u64 t_ns, u16 json_len, json}`; each inbox JSON is `{"model": "<name>", "op": "<op>", "args": {...}}`, applied **in order before the node runs**, by calling `Peripheral::deliver(op, args, t_ns)` (new virtual, default throws `UnknownOp`). `deliver` must schedule any state change at `t_ns` (≥ node now, else strict abort `inbox message for <model> at t=… is in the past (now …)`).
- ERROR payload: `u16 msg_len, msg, u32 n_events, events` (same event encoding as STOPPED), so events from the aborting quantum reach Python.
- Python: `Node.post(model: str, op: str, args: dict, t_ns: int) -> None` queues for the next RUN_UNTIL; HELLO version check expects 2; `NodeCrashed` processing records the ERROR's events into `event_log`, `node.output`, pin history before raising.

- [ ] **Step 1: Failing tests**: `test_post_applies_at_time` (a test-only model op — add a `Gpio::deliver("drive_pin_at", {pin, level}, t)` used only by tests — drives PA0 high at exactly t=1.500000 ms when posted during the quantum ending at 1 ms; pin history shows the edge at 1 500 000 ns); `test_post_in_past_aborts`; `test_abort_quantum_events_delivered` (faults firmware prints `printf("boom\n")` and toggles PA5, then a strict-abort access in the same quantum → `node.output` contains "boom" and PA5's edge is in pin history after `NodeCrashed`); version mismatch message names both versions.
- [ ] **Step 2–4:** FAIL → implement (both sides together; update AGENTS.md's "change together" note) → PASS native + Docker.
- [ ] **Step 5: Commit** `feat(protocol): v2 — timestamped inbox on RUN_UNTIL, events on ERROR`.

### Task 2: CAN frame model in Python (bit-exact timing, logs)

**Files:** Create `python/stest/can/{__init__.py,frame.py,log.py}`, `python/tests/test_can_frame.py`.

**Interfaces — Produces:**
```python
@dataclass(frozen=True)
class CanFrame:
    id: int                     # 11-bit (ext=False) or 29-bit (ext=True)
    data: bytes = b""           # len 0..8; for rtr, data must be empty
    ext: bool = False
    rtr: bool = False
    dlc: int | None = None      # defaults to len(data); rtr frames may set 0..8
    def bit_length(self) -> int           # SOF..EOF incl. stuff bits (stuffing over SOF..CRC), excluding IFS
    def duration_ns(self, bitrate: int) -> int   # ceil(bit_length * 1e9 / bitrate)
    def arbitration_key(self) -> tuple    # sortable; lower wins (ID bits, then SRR/IDE/RTR per ISO 11898-1)
MIN_FRAME_BITS = 44                       # 0-byte standard data frame, no stuff bits, SOF..EOF
IFS_BITS = 3
def lookahead_ns(bitrate: int) -> int     # floor(MIN_FRAME_BITS * 1e9 / bitrate)
def crc15(bits: list[int]) -> int         # CAN CRC-15 (poly 0x4599)
# log.py
def format_candump(frames: Iterable[LoggedFrame], channel: str) -> str   # "(sec.usec) <ch> ID#DATA" (ID 3 hex digits std / 8 ext; R for rtr)
def format_asc(frames: Iterable[LoggedFrame], channel_no: int) -> str    # Vector ASC with header "date"/"base hex  timestamps absolute"
@dataclass(frozen=True) class LoggedFrame: t_ns: int; bus: str; frame: CanFrame; sender: str   # t_ns = end of frame (EOF)
```
- [ ] **Step 1: Failing tests**: CRC-15 matches known vectors (e.g. the CRC of the bit sequence for std ID 0x000 DLC 0 equals the value computed by an independent reference implementation written in the test); `bit_length` for std id 0x7FF dlc 0 = 44 + stuff count computed by a straightforward reference stuffer in the test; a table of ≥10 frames (std/ext, rtr, 0–8 bytes, all-zero and all-one payloads that force stuffing) cross-checked against the reference stuffer; `lookahead_ns(500_000) == 88_000`; `arbitration_key` ordering: lower ID wins; std beats ext with same 11-bit base; data beats remote for the same ID; candump/ASC line format golden strings.
- [ ] **Step 2–4:** FAIL → implement → PASS.
- [ ] **Step 5: Commit** `feat(can): bit-exact CAN frame model and log formats`.

### Task 3: Bus engine, Python devices, Sim integration

**Files:** Create `python/stest/can/{bus.py,device.py}`, `python/tests/test_can_bus.py`; Modify `python/stest/sim.py`, `python/stest/node.py`, `python/stest/__init__.py`.

**Interfaces — Produces:**
```python
class Sim:
    def can_bus(self, name: str, bitrate: int = 500_000) -> CanBus        # before start(); quantum capped to min lookahead over buses
    def node(self, name, exe, env=None, can: dict[str, CanBus | str] | None = None) -> Node   # {"CAN2": bus}
class CanBus:
    name: str; bitrate: int
    log: list[LoggedFrame]                                   # delivered frames, in EOF order
    def attach(self, device: CanDevice) -> CanDevice
    def send(self, frame: CanFrame, sender: str = "python") -> None   # requested at the current sim time
    def wait_for_frame(self, *, id: int | None = None, timeout_ms: float, match: Callable[[CanFrame], bool] | None = None) -> LoggedFrame
    def export_candump(self, path) -> None; def export_asc(self, path) -> None
class CanDevice:                                              # subclass for virtual devices
    name: str; ack: bool = True                               # participates in ACK
    def on_frame(self, lf: LoggedFrame) -> None: ...          # called at the quantum boundary after EOF
    def send(self, frame: CanFrame) -> None                   # requested at current sim time
    def every(self, period_ms: float, fn: Callable[[], None]) -> None   # periodic callback on the virtual clock
```
Engine algorithm (per bus; run after every quantum, with all node events up to boundary T known):
```
pending = node TX requests (events "can_tx" with t_req, per node mailbox set + TXFP rule) ∪ device requests (t_req = request time)
while True:
    t_idle = max(bus_free_at, earliest t_req among pending)        # bus free = previous EOF + IFS
    if t_idle > T: break                                           # can't decide yet: later requests may join
    contenders = pending requests with t_req <= t_idle; per node take its own top mailbox (TXFP=0: lowest key; TXFP=1: oldest request)
    winner = min(contenders, key=(arbitration_key, sender order)); identical frames from several senders → all win
    eof = t_idle + winner.duration_ns(bitrate); bus_free_at = eof + IFS_BITS*bit_ns
    ack = any other node in normal mode attached to this bus, or any device with ack=True
    ack → post rx (all other attached nodes, at eof) + tx_result ok (sender, at eof); devices' on_frame after the quantum containing eof; log.append
    no ack → tx_result {"ok": false, "error": "ack"} at eof (Task 7 extends error handling); request stays pending unless NART
    losers get tx_result {"arb_lost": true} at eof (ALST observable by the time the bus frees; documented deviation: hardware sets ALST during arbitration)
```
Causality: with quantum ≤ `lookahead_ns(bitrate)` every `eof ≥ T` at decision time, so posted messages are never in the past.
- [ ] **Step 1: Failing tests** (Python devices only — no firmware): `test_device_to_device` (A sends id 0x100 at t=0 → B.on_frame at EOF = exact `duration_ns`); `test_arbitration_rules` (two devices send 0x200 and 0x100 at the same instant → 0x100 first, 0x200 starts at 0x100 EOF + IFS; std vs ext; data vs remote; identical frames → one log entry); `test_busy_bus_queues` (5 frames queued while busy → delivered back-to-back in arbitration order, no loss); `test_quantum_capped_by_bus` (Sim(quantum_us=1000) + 500 kbit/s bus → quantum_ns == 88_000, one INFO log line; logs identical to Sim(quantum_us=88)); `test_determinism_across_quanta` (same scenario at quantum 88 µs and 20 µs → identical bus logs).
- [ ] **Step 2–4:** FAIL → implement → PASS.
- [ ] **Step 5: Commit** `feat(can): Python bus engine with arbitration, virtual devices, Sim wiring`.

### Task 4: bxCAN register model (CAN1 + CAN2)

**Files:** Create `chips/stm32l4/models/bxcan.{hpp,cpp}`, `chips/stm32l4/tests/test_bxcan.cpp`; Modify TOML (CAN1 @0x40006400, CAN2 @0x40006800 — verify in header; rcc_enable APB1ENR1 CAN1EN/CAN2EN; IRQs CAN1_TX/RX0/RX1/SCE and CAN2_TX/RX0/RX1/SCE from `IRQn_Type`), `chips/stm32l4/CMakeLists.txt`.

**Interfaces — Produces:**
- Model `stm32l4.bxcan` with `params.role = "master"|"slave"`; the slave (CAN2) finds the master via `ModelContext::get<Bxcan>("CAN1")` for filter banks (`FMR.CAN2SB`, banks 0..27 in CAN1's block). Filter registers on CAN2's own block are reserved (strict).
- Modes: SLEEP (reset) / INIT (INRQ→INAK) / NORMAL; leaving INIT sets INAK=0 after 11 recessive bit times (Clock event; bit time from BTR + `clocks().pclk1_hz()`); BTR writable only in INIT; `bitrate()` exposed. LBKM/SILM/TTCM/AWUM-on-bus-activity → `NotModelled`. ABOM/NART/RFLM/TXFP honoured.
- TX mailboxes 0–2: TIxR/TDTxR/TDLxR/TDHxR; TXRQ → event `{"kind":"can_tx","periph":"CANn","mailbox":m,"id":..,"ext":..,"rtr":..,"dlc":..,"data":"hex","txfp":0|1,"seq":k}` at request time; ABRQ before start → abort (event `can_tx_abort`); TSR TME/RQCP/TXOK/ALST/TERR/CODE/LOW per RM0351. `deliver("tx_result", {mailbox, ok, arb_lost, error}, t)`.
- RX: `deliver("rx", {id, ext, rtr, dlc, data}, t)` → filter match (mask/list, 16/32-bit scale, FIFO assignment, active banks, FINIT) → FIFO 0/1 (3 deep, FMP/FULL/FOVR, RFLM lock vs overwrite, RFOM release, FMI index per RM0351 numbering), RIxR/RDTxR/RDLxR/RDHxR. Not received if in INIT/SLEEP.
- IRQs via `set_irq_source` on 4 vectors per instance (TME, FMP/FULL/FOV, error/status/wakeup per IER).
- `call("state")` → `{"mode","bitrate","tec","rec","tx":[...],"fifo":[n0,n1]}`.
- [ ] **Step 1: Failing doctests**: init/normal transitions incl. 11-bit idle delay; BTR → bitrate (PCLK1 80 MHz, BRP=9, TS1=12 (13tq), TS2=1 (2tq) → 500 000); BTR write outside INIT ignored-or-abort per RM (verify; strict if undefined); TX request emits event and mailbox busy until tx_result; TXFP ordering; ABRQ; filters: 32-bit mask, 16-bit list (4 IDs/bank), FIFO1 assignment, CAN2SB split (CAN2 uses banks ≥ CAN2SB), FMI numbering across banks; FIFO overrun with RFLM=0/1; RFOM releases; IRQ levels; strict aborts for LBKM/SILM/TTCM.
- [ ] **Step 2–4:** FAIL → implement → PASS (native + Docker).
- [ ] **Step 5: Commit** `feat(l4): bxCAN model (CAN1/CAN2, filters, mailboxes, FIFOs)`.

### Task 5: Firmware ↔ firmware ↔ Python end to end

**Files:** Create `selftest/firmware/can/main.c` (+ msp/it files as needed), `python/tests/test_can_e2e.py`; Modify `selftest/CMakeLists.txt`, `python/stest/node.py` (wire node `can_tx*` events into buses; bitrate check), `python/stest/sim.py`.

**Interfaces:** Node `can={"CAN2": bus}` wiring; on the first `can_tx` or on leaving INIT, Python compares `node.call("CAN2","state")["bitrate"]` with the bus (Review Focus 1). Firmware (`-DNODE_ID=1|2`, one exe per id): CubeMX-style CAN2 at 500 kbit/s (PCLK1 80 MHz, Prescaler 10, BS1 13tq, BS2 2tq — verify against RM; must yield 500 000), CAN1 clock enabled for the filters, filters configured like a typical project: one 16-bit ID-list bank (4 IDs) + one 32-bit mask bank, FIFO0 RX pending interrupt; every 10 ms sends id `0x100 + NODE_ID` with an incrementing counter byte; RX callback stores the last frame per id in globals and counts frames.
- [ ] **Step 1: Failing tests**: `test_two_nodes_exchange` (after 100 ms each node's `rx_count` ≈ 10 and last counter correct; bus log alternates with exact EOF times); `test_python_device_to_node` (device sends 0x123 → node RX global updated; filter-rejected id 0x456 → not received); `test_rx_rules` (frame while node in INIT not received; FIFO overrun when firmware stops draining → FOVR set); `test_bitrate_mismatch_aborts` (NODE built with wrong prescaler variant → NodeCrashed matching `bit rate 250000 does not match bus 'drive' \(500000\)`); `test_can_e2e_determinism` (q=88 µs vs 40 µs identical logs); `test_can_speed` (2 nodes, 2 virtual s ≤ 1.6 s wall; record numbers).
- [ ] **Step 2–4:** FAIL → implement → PASS native + Docker.
- [ ] **Step 5: Commit** `feat(can): firmware nodes on simulated buses end to end`.

### Task 6: DBC-aware devices and messages

**Files:** Create `python/stest/can/dbc.py`, `python/tests/data/test.dbc` (generic: 3 messages, signed/unsigned/scaled/enum signals, one multiplexed message), `python/tests/test_can_dbc.py`; Modify `pyproject.toml` (cantools dependency), `python/stest/can/__init__.py`.

**Interfaces — Produces:**
```python
class Dbc:
    def __init__(self, path: str | os.PathLike)
    def encode(self, message: str, signals: dict) -> CanFrame
    def decode(self, frame: CanFrame) -> tuple[str, dict] | None   # None if id unknown
class DbcDevice(CanDevice):
    def __init__(self, name: str, dbc: Dbc)
    def send_message(self, message: str, **signals) -> None
    def send_periodic(self, message: str, period_ms: float, signals: dict | Callable[[], dict]) -> None
    received: dict[str, tuple[int, dict]]      # message name -> (t_ns, decoded signals), latest
class CanBus:
    def messages(self, dbc: Dbc) -> list[tuple[int, str, dict]]    # decoded log
    def wait_for_message(self, dbc: Dbc, message: str, *, timeout_ms: float, where: Callable[[dict], bool] | None = None) -> tuple[int, dict]
```
- [ ] **Step 1: Failing tests**: encode/decode round trip incl. scaled and enum signals; `send_periodic` at 50 ms produces frames at exact 50 ms multiples (± arbitration delay when contended); `wait_for_message(where=...)` against the `can` firmware node echoing a DBC message; multiplexed message decode.
- [ ] **Step 2–4:** FAIL → implement → PASS.
- [ ] **Step 5: Commit** `feat(can): DBC-aware devices and message waits`.

### Task 7: Error states and fault injection

**Files:** Modify `python/stest/can/bus.py`, `chips/stm32l4/models/bxcan.{hpp,cpp}`; Create `python/tests/test_can_errors.py`; doctests in `test_bxcan.cpp`.

**Interfaces — Produces:** ISO 11898-1 fault confinement (simplified, documented): transmitter error → TEC += 8; receiver error → REC += 1; successful TX → TEC −= 1; successful RX → REC −= 1 (min 0; if REC > 127, a successful RX sets it to 119..127 per ISO 11898-1 — use 127 and document); error passive at ≥128; bus-off at TEC > 255 (BOFF; leaves after 128 × 11 recessive bits when ABOM=1, or on INRQ/exit per RM when ABOM=0); no-ACK while error passive does not increment TEC. `CanBus.inject_errors(count: int = 1, *, target: str | None = None)` corrupts the next `count` frames (error frame at the end of the arbitration field: transmitter TX error, all receivers RX error, retransmission). An error frame occupies the bus for 20 bit times (6-bit flag + up to 6 bits of superposed flags + 8-bit delimiter) before the IFS; document this simplification. ESR LEC/TEC/REC/EWGF/EPVF/BOFF and SCE IRQ per IER. `CanBus.errors -> list[dict]` (t_ns, kind, sender).
- [ ] **Step 1: Failing tests**: `test_no_ack_alone` (Review Focus 2: single node, 1 frame → TERR, TEC climbs to 128 in 16 attempts and stays, no delivery, sim progresses); `test_inject_errors_retransmit` (1 injected error → frame delivered on retry, TEC 8→7 after success); `test_bus_off_and_recovery` (inject 32 errors → BOFF, SCE IRQ fires, ABOM recovery after 128×11 bit times then traffic resumes); `test_error_passive_flags`.
- [ ] **Step 2–4:** FAIL → implement → PASS.
- [ ] **Step 5: Commit** `feat(can): fault confinement, error injection, bus-off recovery`.

### Task 8: Logs, docs, test robustness, speed

**Files:** Modify `python/stest/pytest_plugin.py` (failure report section "stest can <bus>" with the last 200 frames in candump format; `--stest-can-log DIR` option writes `<test>-<bus>.log` candump files), `spike/run_gate.py` (`--quantum-us`, default 100; add a run at 88), `selftest/CMakeLists.txt` (perf tests `RUN_SERIAL`), CI (gate step runs `--quantum-us 88` too); Create `docs/can.md` (user guide: buses, wiring nodes, devices, DBC, errors, logs, quantum cap); Modify `AGENTS.md`, `docs/adding-a-peripheral.md` (inbox `deliver()` pattern for cross-node models).
- [ ] **Step 1:** Reproduce the `python_tests` flake under `ctest -j8` (run 20×, record which test fails and why); write the failing reproduction as a deterministic test where possible (e.g. a wall-clock budget sensitive to load), then fix the root cause (mark wall-clock perf tests `RUN_SERIAL` and/or give them a load-insensitive measurement). Record the cause in the report.
- [ ] **Step 2:** Failing tests for the report section and `--stest-can-log` (pytester), then implement.
- [ ] **Step 3:** Run `uv run spike/run_gate.py --quantum-us 88` natively and in Docker; add it to CI; record numbers (x86 from CI) in `docs/spikes/2026-10-30-interception-gate.md` under a "CAN quantum" heading. If x86 exceeds 0.40, stop and report (sync cost reduction — shared memory — becomes a decision for the user).
- [ ] **Step 4:** Full suites native + Docker; `ctest -j8` 10× clean.
- [ ] **Step 5: Commit** `feat(can): logs, docs, perf-test isolation, CAN-quantum speed gate`.
