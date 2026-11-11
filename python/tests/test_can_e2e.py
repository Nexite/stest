"""Firmware nodes on simulated CAN buses (selftest firmware `can_node1` / `can_node2`: CubeMX-style
CAN2 at 500 kbit/s, HAL filters, FIFO0 interrupt, id 0x100 + NODE_ID every 10 ms; `can_badrate`:
node 1 with Prescaler 20 = 250 kbit/s)."""

from __future__ import annotations

import json
import time

import pytest

from stest import CanDevice, CanFrame, Sim
from stest.errors import NodeCrashed, StestError

BITRATE = 500_000
IFS_NS = 3 * 2_000


def dur(frame: CanFrame) -> int:
    return frame.duration_ns(BITRATE)


def can_events(sim: Sim, node: str, kind: str) -> list[tuple[int, dict]]:
    out = []
    for t, name, js in sim.event_log:
        d = json.loads(js)
        if name == node and d.get("kind") == kind:
            out.append((t, d))
    return out


def rx_entry(node, can_id: int) -> dict | None:
    """The firmware's rx_table slot for ``can_id`` (None if never received)."""
    for i in range(8):
        slot = node.var(f"rx_table[{i}]")
        if slot["count"] and slot["id"] == can_id:
            return {"count": slot["count"], "dlc": slot["dlc"], "data": bytes(slot["data"][: slot["dlc"]])}
    return None


class Recorder(CanDevice):
    def __init__(self, name: str, **kw):
        super().__init__(name, **kw)
        self.frames = []

    def on_frame(self, lf) -> None:
        self.frames.append(lf)


def test_two_nodes_exchange(can_exe):
    with Sim() as s:
        bus = s.can_bus("drive", BITRATE)
        n1 = s.node("n1", can_exe(1), can={"CAN2": bus})
        n2 = s.node("n2", can_exe(2), can={"CAN2": bus})
        s.start()
        s.advance(ms=105)
        for n, other in ((n1, 0x102), (n2, 0x101)):
            assert n.var("error_count") == 0
            assert n.var("tx_count") == 10 and n.var("tx_skipped") == 0
            assert n.var("rx_count") == 10
            assert n.var("rx_last_id") == other
            assert rx_entry(n, other) == {"count": 10, "dlc": 1, "data": bytes([9])}
            assert n.call("CAN2", "state")["mode"] == "normal"
        # Both firmwares are identical but for NODE_ID, so they request at the same instants:
        # 0x101 wins arbitration, 0x102 follows at EOF + IFS. Exact EOF times.
        tx1 = can_events(s, "n1", "can_tx")
        tx2 = can_events(s, "n2", "can_tx")
        assert [t for t, _ in tx1] == [t for t, _ in tx2]
        assert len(tx1) == 10
        expected = []
        for k, (t_req, ev) in enumerate(tx1):
            assert ev["id"] == 0x101 and ev["data"] == f"{k:02x}"
            f1, f2 = CanFrame(0x101, bytes([k])), CanFrame(0x102, bytes([k]))
            eof1 = t_req + dur(f1)
            expected += [(eof1, f1, "n1"), (eof1 + IFS_NS + dur(f2), f2, "n2")]
        assert [(lf.t_ns, lf.frame, lf.sender) for lf in bus.log] == expected
        assert [lf.bus for lf in bus.log] == ["drive"] * 20


def test_python_device_to_node(can_exe):
    with Sim() as s:
        bus = s.can_bus("drive", BITRATE)
        n = s.node("n", can_exe(1), can={"CAN2": bus})
        dev = bus.attach(Recorder("tester"))
        s.start()
        s.advance(ms=1)
        dev.send(CanFrame(0x123, b"\xab\xcd"))
        s.advance(ms=1)
        assert rx_entry(n, 0x123) == {"count": 1, "dlc": 2, "data": b"\xab\xcd"}
        assert n.var("rx_last_fmi") == 2  # bank 14 (16-bit list), third identifier
        # rejected by the filters: never stored
        dev.send(CanFrame(0x456, b"\x01"))
        dev.send(CanFrame(0x310, b"\x02"))  # outside the 0x300..0x30F mask bank
        s.advance(ms=1)
        assert rx_entry(n, 0x456) is None and rx_entry(n, 0x310) is None
        assert n.var("rx_count") == 1
        # the 32-bit mask bank: FMI 4 (the list bank has filters 0-3)
        dev.send(CanFrame(0x30A, b"\x07\x08\x09"))
        s.advance(ms=1)
        assert rx_entry(n, 0x30A) == {"count": 1, "dlc": 3, "data": b"\x07\x08\x09"}
        assert n.var("rx_last_fmi") == 4 and n.var("rx_count") == 2
        # request -> immediate reply from the RX callback
        dev.send(CanFrame(0x1F0, b"\x5a"))
        lf = bus.wait_for_frame(id=0x181, timeout_ms=2)
        assert lf.frame == CanFrame(0x181, b"\x5a") and lf.sender == "n"
        assert n.var("reply_count") == 1
        # the device ACKs the node's periodic frames and sees them
        s.advance(ms=10)
        periodic = [lf for lf in dev.frames if lf.frame.id == 0x101]
        assert [lf.frame.data for lf in periodic] == [b"\x00"]
        assert n.var("tx_count") == 2 and n.var("error_count") == 0


def test_rx_rules(can_exe):
    with Sim() as s:
        bus = s.can_bus("drive", BITRATE)
        n = s.node("n", can_exe(1), can={"CAN2": bus})
        dev = bus.attach(CanDevice("tester"))
        bus.attach(CanDevice("acker"))  # ACKs while the node is in init
        s.start()
        s.advance(ms=1)
        # can_run = 0: HAL_CAN_Stop -> init mode; a frame then is not received
        n.set_var("can_run", 0)
        s.advance(ms=2)
        assert n.call("CAN2", "state")["mode"] == "init"
        dev.send(CanFrame(0x123, b"\x01"))
        s.advance(ms=1)
        assert len(bus.log) == 1  # on the bus (ACKed by "acker") but not received
        assert n.var("rx_count") == 0 and rx_entry(n, 0x123) is None
        n.set_var("can_run", 1)
        s.advance(ms=2)
        assert n.call("CAN2", "state")["mode"] == "normal"
        dev.send(CanFrame(0x123, b"\x02"))
        s.advance(ms=1)
        assert rx_entry(n, 0x123) == {"count": 1, "dlc": 1, "data": b"\x02"}
        modes = [d["mode"] for _, d in can_events(s, "n", "can_mode")]
        assert modes == ["init", "normal", "init", "normal"]

        # rx_hold = 1: the firmware stops draining FIFO0 -> 3 messages, the 4th overruns (FOVR),
        # RFLM = 0 so it overwrites the newest one
        n.set_var("rx_hold", 1)
        s.advance(ms=1)
        for k in range(5):
            dev.send(CanFrame(0x123, bytes([0x10 + k])))
        s.advance(ms=2)
        assert n.call("CAN2", "state")["fifo"] == [3, 0]
        assert n.var("rx_count") == 1
        assert n.var("fifo0_ovr") == 1
        # draining again: frames 1, 2 and the last one (5) are read
        n.set_var("rx_hold", 0)
        s.advance(ms=2)
        assert n.call("CAN2", "state")["fifo"] == [0, 0]
        assert n.var("rx_count") == 4
        assert rx_entry(n, 0x123) == {"count": 4, "dlc": 1, "data": bytes([0x14])}
        assert n.var("error_count") == 0


def test_stop_withdraws_pending_tx_and_restart_resends(can_exe):
    """HAL_CAN_Stop with an un-ACKed frame pending: the model withdraws it (can_tx_abort), the
    bus drops the request (aborted result), and HAL_CAN_Start requests it again (new seq)."""
    with Sim() as s:
        bus = s.can_bus("drive", BITRATE)
        n = s.node("n", can_exe(1), can={"CAN2": bus})
        s.start()
        s.advance(ms=12)  # 0x101 #0 requested at ~10 ms: nobody ACKs, retried back to back
        assert bus.log == []
        state = n.call("CAN2", "state")
        assert state["tx"][0]["state"] == "pending"
        n.set_var("can_run", 0)  # HAL_CAN_Stop at the 13 ms tick
        s.advance(ms=2)
        state = n.call("CAN2", "state")
        assert state["mode"] == "init" and state["tx"][0]["state"] == "held"
        aborts = can_events(s, "n", "can_tx_abort")
        assert [(d["mailbox"], d["seq"]) for _, d in aborts] == [(0, 1)]
        dev = bus.attach(Recorder("acker"))
        n.set_var("can_run", 1)  # HAL_CAN_Start at the 15 ms tick: the held frame is requested again
        s.advance(ms=1)
        txs = can_events(s, "n", "can_tx")
        assert [(d["mailbox"], d["seq"], d["data"]) for _, d in txs] == [(0, 1, "00"), (0, 2, "00")]
        assert [(lf.frame, lf.sender) for lf in dev.frames] == [(CanFrame(0x101, b"\x00"), "n")]
        assert dev.frames[0].t_ns == txs[1][0] + dur(CanFrame(0x101, b"\x00"))
        assert n.call("CAN2", "state")["tx"][0]["state"] == "empty"
        assert n.var("error_count") == 0


def test_bitrate_mismatch_aborts(can_exe):
    with Sim() as s:
        bus = s.can_bus("drive", BITRATE)
        s.node("n", can_exe("badrate"), can={"CAN2": bus})
        s.start()
        with pytest.raises(NodeCrashed, match=r"bit rate 250000 does not match bus 'drive' \(500000\)") as e:
            s.advance(ms=5)
        assert "'n'" in str(e.value) and "CAN2" in str(e.value)


def test_unwired_controller_transmitting_aborts(can_exe):
    """A can_tx from a controller not wired to a bus is a harness/test bug: strict abort."""
    with Sim() as s:
        s.node("n", can_exe(1))  # no can= at all
        s.start()
        with pytest.raises(
            NodeCrashed,
            match=r"node 'n' CAN2 requested a transmission but is not wired to a bus: "
            r"Sim\.node\('n', \.\.\., can=\{'CAN2': bus\}\)",
        ):
            s.advance(ms=15)
    with Sim() as s:  # wired, but the wrong controller (firmware uses CAN2)
        bus = s.can_bus("drive", BITRATE)
        s.node("n", can_exe(1), can={"CAN1": bus})
        s.start()
        s.advance(ms=5)  # mode events of the unwired CAN2 are fine
        with pytest.raises(NodeCrashed, match=r"node 'n' CAN2 requested a transmission but is not wired"):
            s.advance(ms=10)
        assert bus.log == []


@pytest.mark.parametrize("periph", ["CAN9", "GPIOA"])
def test_can_wiring_names_are_validated(can_exe, periph):
    with Sim() as s:
        bus = s.can_bus("drive", BITRATE)
        s.node("n", can_exe(1), can={periph: bus})
        with pytest.raises(
            StestError,
            match=rf"node 'n': can={{'{periph}': \.\.\.}}: {periph} is not a CAN controller of this "
            r"firmware's chip \(CAN controllers: CAN1, CAN2\)",
        ):
            s.start()


# ---- determinism across quanta, with traffic landing just before quantum boundaries ----

GRID = 7_000  # ns: the small quantum
PERIOD_MS = 7  # probe period: a multiple of GRID, so each probe keeps its phase on the grid


def near_boundary(t: int, grid: int = GRID) -> bool:
    """t in (T - 25, T] for a quantum end T on ``grid`` (25 ns = one MMIO access)."""
    return t % grid == 0 or t % grid > grid - 25


class Prober(CanDevice):
    """Sends ``frame`` at ``first_ns`` + k * 7 ms (k >= 0)."""

    def __init__(self, name: str, frame: CanFrame, first_ns: int):
        super().__init__(name)
        self.frame, self.started, self.sent = frame, False, []
        self._first_ns = first_ns

    def arm(self) -> None:
        self.every(self._first_ns / 1e6, self._kick)

    def _kick(self) -> None:
        if not self.started:
            self.started = True
            self._send()
            self.every(PERIOD_MS, self._send)

    def _send(self) -> None:
        self.sent.append(self.now_ns)
        self.send(self.frame)


def reply_latency_ns(can_exe) -> int:
    """Firmware time from the EOF of a 0x1F0 request to node 1's reply request (can_tx),
    measured in the probed scenario itself (a short run at the 88 us quantum)."""
    r = run_probed(can_exe, 88, 0, ms=6)
    eof = next(t for t, f, _ in r["bus"] if f.id == 0x1F0)
    t_req = next(t for t, name, js in r["events"] if name == "n1" and json.loads(js).get("id") == 0x181)
    return t_req - eof


def run_probed(can_exe, quantum_us: int, latency: int, ms: int = 63):
    a = CanFrame(0x123, b"\xa1")  # request at T - 10
    b = CanFrame(0x305, b"\xb2")  # EOF (node RX) at T - 10
    c = CanFrame(0x1F0, b"\xc3")  # node replies with a request at T - 10
    base = 3 * GRID * 100  # 2.1 ms: a grid point, clear of the nodes' 10 ms traffic
    probes = [
        Prober("pa", a, base - 10),
        Prober("pb", b, base + 140 * GRID - 10 - dur(b)),  # ~3.1 ms
        Prober("pc", c, base + 300 * GRID - 10 - dur(c) - latency),  # ~4.2 ms
    ]
    with Sim(quantum_us=quantum_us) as s:
        bus = s.can_bus("drive", BITRATE)
        n1 = s.node("n1", can_exe(1), can={"CAN2": bus})
        n2 = s.node("n2", can_exe(2), can={"CAN2": bus})
        for p in probes:
            bus.attach(p)
            p.arm()
        s.start()
        s.advance(ms=ms)
        state = {
            "bus": [(lf.t_ns, lf.frame, lf.sender) for lf in bus.log],
            "events": list(s.event_log),
            "vars": [(n.var("rx_count"), n.var("tx_count"), n.var("reply_count"), n.var("error_count")) for n in (n1, n2)],
            "sent": [list(p.sent) for p in probes],
        }
        return state


def test_can_e2e_determinism(can_exe):
    latency = reply_latency_ns(can_exe)
    assert 0 < latency < 50_000
    big = run_probed(can_exe, 88, latency)
    small = run_probed(can_exe, GRID // 1000, latency)
    assert small["bus"] == big["bus"]
    assert small["events"] == big["events"]
    assert small["vars"] == big["vars"]
    assert small["sent"] == big["sent"]
    # Meaningful on the small grid: device requests, node RX deliveries (EOFs of frames the
    # nodes receive) and node TX requests (can_tx) all land in (T - 25, T].
    sent = [t for p in small["sent"] for t in p]
    rx_eofs = [t for t, f, sender in small["bus"] if sender.startswith("p")]
    node_tx = [t for t, _, js in small["events"] if json.loads(js).get("kind") == "can_tx"]
    assert sum(map(near_boundary, sent)) >= 9
    assert sum(map(near_boundary, rx_eofs)) >= 6
    assert sum(map(near_boundary, node_tx)) >= 6
    rx = [v[0] for v in small["vars"]]
    assert all(r >= 20 for r in rx)  # each node received the other's frames and the probes
    assert all(v[2] >= 8 for v in small["vars"])  # replies to every 0x1F0
    assert all(v[3] == 0 for v in small["vars"])


def test_can_speed(can_exe):
    """2 nodes exchanging frames: 2 virtual s within 1.6 s of wall time (0.4 s per node per
    virtual s), at the 88 us quantum a 500 kbit/s bus imposes."""
    with Sim() as s:
        bus = s.can_bus("drive", BITRATE)
        n1 = s.node("n1", can_exe(1), can={"CAN2": bus})
        s.node("n2", can_exe(2), can={"CAN2": bus})
        s.start()
        t0 = time.perf_counter()
        s.advance(ms=2000)
        wall = time.perf_counter() - t0
        print(f"test_can_speed: 2 nodes, 2 virtual s, quantum {s.quantum_ns} ns: wall {wall:.3f} s")
        assert n1.var("rx_count") == 200 - 1 or n1.var("rx_count") == 200
        assert len(bus.log) >= 398
        assert wall <= 1.6, f"too slow: {wall:.3f} s > 1.6 s for 2 nodes x 2 virtual s"
