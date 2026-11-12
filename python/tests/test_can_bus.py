"""CAN bus engine: arbitration, ACK, queuing, Python devices, Sim wiring (no firmware)."""

from __future__ import annotations

import logging

import pytest

from stest import CanBus, CanDevice, Sim, StestError, WaitTimeout
from stest.can import IFS_BITS, CanFrame, LoggedFrame, format_asc, format_candump, lookahead_ns

BR = 500_000
BIT = 2_000  # ns per bit at 500 kbit/s
IFS = IFS_BITS * BIT


def dur(f: CanFrame, br: int = BR) -> int:
    return f.duration_ns(br)


class Recorder(CanDevice):
    def __init__(self, name: str, ack: bool = True):
        super().__init__(name, ack=ack)
        self.got: list[tuple[int, LoggedFrame]] = []  # (device now at the callback, frame)

    def on_frame(self, lf: LoggedFrame) -> None:
        self.got.append((self.now_ns, lf))


class FakeNode:
    """A node stand-in driving the engine's node-facing API on a script of absolute times.

    Each scripted action is issued at the first quantum end T >= its time (as Task 5's
    node-event wiring will: node events up to T are known after the quantum), and every
    engine callback is recorded with its time. The fault-confinement counters the engine
    adds (tec, rec, epoch; see test_can_errors.py) are recorded separately in ``fc``, so
    ``calls`` keeps the arbitration/ACK view these tests check."""

    def __init__(self, sim: Sim, bus: CanBus, name: str):
        self.name = name
        self.bus = bus
        self.calls: list[tuple] = []
        self.fc: list[tuple] = []  # (t, kind, counters)
        self.script: list[tuple] = []  # (t, action, args)
        bus._attach_node(name, self._on_rx, self._on_tx_result, self._on_fault)
        sim._can_hooks.append(self._hook)

    def _on_rx(self, lf: LoggedFrame, t: int, fc: dict) -> None:
        self.calls.append(("rx", t, lf))
        self.fc.append((t, "rx", fc))

    def _on_tx_result(self, key, result: dict, t: int) -> None:
        fc = {k: result.pop(k) for k in ("tec", "rec", "epoch") if k in result}
        self.calls.append(("tx_result", t, key, result))
        if fc:
            self.fc.append((t, "tx_result", fc))

    def _on_fault(self, op: str, args: dict, t: int) -> None:
        self.calls.append((op, t, args))

    def mode(self, t: int, mode: str, bitrate: int | None = BR) -> None:
        self.script.append((t, "mode", (mode, bitrate if mode == "normal" else None)))

    def request(self, t: int, key, frame: CanFrame, txfp: int = 0, seq: int = 0, **kw) -> None:
        self.script.append((t, "request", (key, frame, txfp, seq, kw)))

    def abort(self, t: int, key) -> None:
        self.script.append((t, "abort", (key,)))

    def _hook(self, T: int) -> None:
        due = [s for s in self.script if s[0] <= T]
        self.script = [s for s in self.script if s[0] > T]
        for t, action, args in sorted(due, key=lambda s: s[0]):
            if action == "mode":
                self.bus._set_node_mode(self.name, args[0], args[1], t)
            elif action == "request":
                key, frame, txfp, seq, kw = args
                self.bus._request(self.name, key, frame, t, txfp, seq, **kw)
            else:
                self.bus._abort(self.name, args[0], t)


def device_sim(quantum_us: int = 1000) -> tuple[Sim, CanBus]:
    s = Sim(quantum_us=quantum_us)
    return s, s.can_bus("bus0", BR)


# ---------------------------------------------------------------- devices


def test_device_to_device():
    sim, bus = device_sim()
    a = bus.attach(Recorder("A"))
    b = bus.attach(Recorder("B"))
    sim.start()
    f = CanFrame(0x100, b"\x01\x02")
    a.send(f)  # at t=0
    sim.advance(ms=1)
    eof = dur(f)
    assert bus.log == [LoggedFrame(eof, "bus0", f, "A")]
    assert b.got == [(eof, LoggedFrame(eof, "bus0", f, "A"))]  # callback runs at the frame's EOF time
    assert a.got == []  # a sender does not receive its own frame
    sim.close()


def test_arbitration_rules():
    sim, bus = device_sim()
    a, b, c = (bus.attach(Recorder(n)) for n in "ABC")
    sim.start()

    def race(fa: CanFrame, fb: CanFrame) -> list[LoggedFrame]:
        n0 = len(bus.log)
        a.send(fa)
        b.send(fb)
        sim.advance(ms=2)
        return bus.log[n0:]

    t0 = sim.now_ns
    lo, hi = CanFrame(0x100, b"\xaa"), CanFrame(0x200, b"\xbb")
    got = race(hi, lo)
    assert [lf.frame for lf in got] == [lo, hi]  # lower id wins regardless of sender order
    assert got[0].t_ns == t0 + dur(lo)
    assert got[1].t_ns == got[0].t_ns + IFS + dur(hi)  # loser starts after EOF + IFS
    assert [lf.sender for lf in got] == ["B", "A"]

    std, ext = CanFrame(0x100), CanFrame(0x100 << 18, ext=True)
    assert [lf.frame for lf in race(ext, std)] == [std, ext]  # same base id: 11-bit beats 29-bit
    ext_low = CanFrame(0x0FF << 18 | 0x3FFFF, ext=True)
    assert [lf.frame for lf in race(std, ext_low)] == [ext_low, std]  # lower base id wins

    data, remote = CanFrame(0x123, b""), CanFrame(0x123, rtr=True)
    assert [lf.frame for lf in race(remote, data)] == [data, remote]  # data beats remote
    ext_data, ext_rtr = CanFrame(0x1234, ext=True), CanFrame(0x1234, ext=True, rtr=True)
    assert [lf.frame for lf in race(ext_rtr, ext_data)] == [ext_data, ext_rtr]

    same = CanFrame(0x321, b"\x05")
    c.got.clear()
    got = race(same, same)
    assert len(got) == 1 and got[0].frame == same and got[0].sender == "A"  # bit-identical: one frame on the wire
    assert [lf.frame for _, lf in c.got] == [same]
    sim.close()


def test_busy_bus_queues():
    sim, bus = device_sim(quantum_us=20)
    a = bus.attach(Recorder("A"))
    b = bus.attach(Recorder("B"))
    c = bus.attach(Recorder("C"))
    rx = bus.attach(Recorder("RX"))
    sim.start()
    first = CanFrame(0x7F0, b"\xff" * 8)
    a.send(first)
    sim.advance(us=40)  # bus busy with `first` (~ 200+ us)
    assert bus.log == []
    queued = [CanFrame(0x050), CanFrame(0x010, b"\x01"), CanFrame(0x040, rtr=True, dlc=2), CanFrame(0x030)]
    for f in queued:
        b.send(f)
    c.send(CanFrame(0x020, b"\x02\x03"))
    sim.advance(ms=3)
    order = [lf.frame.id for lf in bus.log]
    assert order == [0x7F0, 0x010, 0x020, 0x030, 0x040, 0x050]  # no loss, arbitration order
    for prev, cur in zip(bus.log, bus.log[1:]):
        assert cur.t_ns == prev.t_ns + IFS + dur(cur.frame)  # back to back
    assert [lf for _, lf in rx.got] == bus.log
    sim.close()


def test_lone_sender_is_never_acknowledged():
    sim, bus = device_sim()
    a = bus.attach(Recorder("A"))
    bus.attach(Recorder("quiet", ack=False))
    sim.start()
    a.send(CanFrame(0x100))
    sim.advance(ms=5)
    assert bus.log == []  # no ACK: retransmitted forever, never delivered
    sim.close()


def test_every_fires_at_exact_multiples():
    sim, bus = device_sim(quantum_us=20)
    a = bus.attach(Recorder("A"))
    rx = bus.attach(Recorder("RX"))
    sim.start()
    sim.advance(us=3_010)  # registration time is off the quantum grid
    n = [0]

    def tick():
        n[0] += 1
        a.send(CanFrame(0x111, bytes([n[0]])))

    a.every(10, tick)
    sim.advance(ms=45)
    assert [lf.t_ns for lf in bus.log] == [3_010_000 + k * 10_000_000 + dur(lf.frame) for k, lf in zip((1, 2, 3, 4), bus.log)]
    assert [lf.frame.data for lf in bus.log] == [bytes([k]) for k in (1, 2, 3, 4)]
    assert len(rx.got) == 4
    sim.close()


def test_device_reaction_is_stamped_at_eof():
    sim, bus = device_sim(quantum_us=50)

    class Echo(Recorder):
        def on_frame(self, lf):
            super().on_frame(lf)
            if lf.frame.id == 0x100:
                self.send(CanFrame(0x101, lf.frame.data))

    a = bus.attach(Recorder("A"))
    bus.attach(Echo("E"))
    sim.start()
    f = CanFrame(0x100, b"\x42")
    a.send(f)
    sim.advance(ms=1)
    reply = CanFrame(0x101, b"\x42")
    assert [lf.frame for lf in bus.log] == [f, reply]
    assert bus.log[1].t_ns == dur(f) + IFS + dur(reply)  # request at EOF, starts after the IFS
    sim.close()


def test_wait_for_frame_and_exports(tmp_path):
    sim, bus = device_sim()
    a = bus.attach(Recorder("A"))
    bus.attach(Recorder("B"))
    sim.start()
    a.every(10, lambda: a.send(CanFrame(0x1ABCDE, b"\x01", ext=True)))
    lf = bus.wait_for_frame(id=0x1ABCDE, timeout_ms=50)
    assert lf.t_ns == 10_000_000 + dur(lf.frame)
    lf2 = bus.wait_for_frame(timeout_ms=50, match=lambda fr: fr.data == b"\x01")
    assert lf2.t_ns == 20_000_000 + dur(lf.frame)  # consumed frames are not matched again
    with pytest.raises(WaitTimeout):
        bus.wait_for_frame(id=0x7FF, timeout_ms=5)
    bus.export_candump(tmp_path / "x.log")
    bus.export_asc(tmp_path / "x.asc")
    assert (tmp_path / "x.log").read_text() == format_candump(bus.log, "bus0")
    assert (tmp_path / "x.log").read_text().startswith(f"(0.{lf.t_ns // 1000:06d}) bus0 001ABCDE#01\n")
    assert (tmp_path / "x.asc").read_text() == format_asc(bus.log, 1)  # channel = bus creation index + 1
    sim.close()


def test_bus_send_uses_named_sender():
    sim, bus = device_sim()
    rx = bus.attach(Recorder("RX"))
    sim.start()
    bus.send(CanFrame(0x10))
    bus.send(CanFrame(0x11), sender="tester")
    sim.advance(ms=1)
    assert [(lf.frame.id, lf.sender) for lf in bus.log] == [(0x10, "python"), (0x11, "tester")]
    assert len(rx.got) == 2
    sim.close()


def test_device_attached_later_sees_only_later_frames():
    sim, bus = device_sim()
    a = bus.attach(Recorder("A"))
    bus.attach(Recorder("B"))
    sim.start()
    a.send(CanFrame(0x1))
    sim.advance(ms=1)
    late = bus.attach(Recorder("late"))
    a.send(CanFrame(0x2))
    sim.advance(ms=1)
    assert [lf.frame.id for _, lf in late.got] == [0x2]
    sim.close()


# ---------------------------------------------------------------- node-facing API (fake nodes)


def test_fake_node_api_rx_and_tx_results():
    sim, bus = device_sim(quantum_us=20)
    n1 = FakeNode(sim, bus, "n1")
    n2 = FakeNode(sim, bus, "n2")
    dev = bus.attach(Recorder("dev", ack=False))
    for n in (n1, n2):
        n.mode(0, "init", None)
        n.mode(1_000, "normal")
    f = CanFrame(0x123, b"\x11\x22")
    n1.request(30_000 - 10, 0, f)  # off the 20 us grid
    sim.start()
    sim.advance(ms=1)
    eof = 30_000 - 10 + dur(f)
    lf = LoggedFrame(eof, "bus0", f, "n1")
    assert n1.calls == [("tx_result", eof, 0, {"ok": True, "arb_lost": False, "error": None, "seq": 0})]
    assert n2.calls == [("rx", eof, lf)]
    assert bus.log == [lf] and dev.got == [(eof, lf)]
    sim.close()


def test_fake_node_arbitration_txfp_and_identical_frames():
    sim, bus = device_sim(quantum_us=20)
    n1 = FakeNode(sim, bus, "n1")
    n2 = FakeNode(sim, bus, "n2")
    bus.attach(Recorder("dev"))  # ACKs everything
    for n in (n1, n2):
        n.mode(0, "normal")
    hi, lo = CanFrame(0x300), CanFrame(0x200)
    # TXFP=0: n1's mailboxes by identifier (lowest arbitration key, then lowest key)
    n1.request(1_000, 0, hi, txfp=0, seq=0)
    n1.request(1_000, 1, lo, txfp=0, seq=1)
    # n2 contends with 0x250 at the same instant: loses to n1's 0x200, gets arb_lost, rx
    mid = CanFrame(0x250)
    n2.request(1_000, 2, mid, txfp=0, seq=0)
    sim.start()
    sim.advance(ms=2)
    assert [lf.frame for lf in bus.log] == [lo, mid, hi]
    e1 = 1_000 + dur(lo)
    e2 = e1 + IFS + dur(mid)
    e3 = e2 + IFS + dur(hi)
    assert [lf.t_ns for lf in bus.log] == [e1, e2, e3]
    assert ("tx_result", e1, 2, {"ok": False, "arb_lost": True, "error": None, "seq": 0}) in n2.calls
    assert n2.calls.index(("tx_result", e1, 2, {"ok": False, "arb_lost": True, "error": None, "seq": 0})) < n2.calls.index(
        ("rx", e1, bus.log[0])
    )
    # n1 loses to n2's 0x250 at e1 + IFS with its 0x300
    assert ("tx_result", e2, 0, {"ok": False, "arb_lost": True, "error": None, "seq": 0}) in n1.calls
    assert ("tx_result", e3, 0, {"ok": True, "arb_lost": False, "error": None, "seq": 0}) in n1.calls

    # TXFP=1: request order (seq) regardless of id
    t = sim.now_ns + 1_000
    n1.request(t, 0, hi, txfp=1, seq=10)
    n1.request(t, 1, lo, txfp=1, seq=11)
    # identical frames from both nodes: both win, one log entry, both TX ok, no rx to each other
    same = CanFrame(0x7A0, b"\x01")
    n1.request(t + 5_000, 2, same, txfp=1, seq=12)
    n2.request(t + 5_000, 0, same, txfp=1, seq=1)
    n0 = len(bus.log)
    c1, c2 = len(n1.calls), len(n2.calls)
    sim.advance(ms=2)
    assert [lf.frame for lf in bus.log[n0:]] == [hi, lo, same]
    e_same = bus.log[-1].t_ns
    assert bus.log[-1].sender == "n1"
    assert ("tx_result", e_same, 2, {"ok": True, "arb_lost": False, "error": None, "seq": 12}) in n1.calls[c1:]
    assert ("tx_result", e_same, 0, {"ok": True, "arb_lost": False, "error": None, "seq": 1}) in n2.calls[c2:]
    assert not any(c[0] == "rx" and c[2].frame == same for c in n1.calls[c1:] + n2.calls[c2:])
    sim.close()


def test_ack_only_from_normal_nodes_and_ack_devices():
    sim, bus = device_sim(quantum_us=20)
    n1 = FakeNode(sim, bus, "n1")
    n2 = FakeNode(sim, bus, "n2")
    bus.attach(Recorder("silent", ack=False))
    n1.mode(0, "normal")
    n2.mode(0, "init", None)  # in init: receives nothing useful and never ACKs
    f = CanFrame(0x100)
    n1.request(0, 0, f)
    sim.start()
    sim.advance(us=500)
    assert bus.log == []
    errs = [c for c in n1.calls if c[0] == "tx_result"]
    # an ACK error: SOF .. ACK slot, then a 20-bit error frame (Task 7; see stest.can.bus)
    att = dur(f) + 12 * BIT
    assert errs[0] == ("tx_result", att, 0, {"ok": False, "arb_lost": False, "error": "ack", "seq": 0})
    period = att + IFS
    assert [c[1] for c in errs] == [att + k * period for k in range(len(errs))]  # automatic retransmission
    # n2 goes normal: the next attempt starting at or after that instant is acknowledged
    t_on = sim.now_ns + 7
    n2.mode(t_on, "normal")
    sim.advance(us=500)
    assert len(bus.log) == 1
    sof = bus.log[0].t_ns - dur(f)
    assert sof >= t_on and sof - t_on < period
    sim.close()


def test_nart_drops_failed_request():
    sim, bus = device_sim(quantum_us=20)
    n1 = FakeNode(sim, bus, "n1")
    n1.mode(0, "normal")
    n1.request(0, 0, CanFrame(0x100), nart=True)
    sim.start()
    sim.advance(ms=1)
    assert [c[3]["error"] for c in n1.calls] == ["ack"]  # one attempt only
    sim.close()


def test_abort_before_and_after_start():
    sim, bus = device_sim(quantum_us=20)
    n1 = FakeNode(sim, bus, "n1")
    bus.attach(Recorder("dev"))
    n1.mode(0, "normal")
    busy = CanFrame(0x001, b"\x00" * 8)
    bus.send(busy)  # t=0: bus busy until dur(busy)
    a, b = CanFrame(0x200), CanFrame(0x300)
    n1.request(10_000, 0, a)
    n1.request(10_000, 1, b)
    n1.abort(dur(busy) - 1, 1)  # before mailbox 1 could start: effective
    n1.abort(dur(busy) + IFS + 1, 0)  # mailbox 0 started at EOF + IFS: too late
    sim.start()
    sim.advance(ms=2)
    assert [lf.frame for lf in bus.log] == [busy, a]
    assert [c for c in n1.calls if c[0] == "tx_result"] == [
        # effective: reported one lookahead (44 bit times) after the abort
        ("tx_result", dur(busy) - 1 + lookahead_ns(BR), 1,
         {"ok": False, "arb_lost": False, "error": None, "aborted": True, "seq": 0}),
        # too late (mailbox 0 was on the wire): no effect, normal result at EOF
        ("tx_result", dur(busy) + IFS + dur(a), 0, {"ok": True, "arb_lost": False, "error": None, "seq": 0}),
    ]
    sim.close()


def test_engine_refuses_to_post_into_the_past():
    sim, bus = device_sim(quantum_us=20)
    n1 = FakeNode(sim, bus, "n1")
    bus.attach(Recorder("dev"))
    sim.start()
    sim.advance(ms=1)
    bus._request("n1", 0, CanFrame(0x100), 0, 0, 0)  # a request stamped long before now
    with pytest.raises(StestError, match="engine bug.*past"):
        sim.advance(us=20)


# ---------------------------------------------------------------- Sim wiring


def test_quantum_capped_by_bus(caplog):
    caplog.set_level(logging.INFO, logger="stest")

    def scenario(q: int):
        s = Sim(quantum_us=q)
        bus = s.can_bus("drive", 500_000)
        s.can_bus("body", 250_000)
        a = bus.attach(Recorder("A"))
        bus.attach(Recorder("B"))
        a.every(1, lambda: a.send(CanFrame(0x100 + (a.now_ns // 1_000_000) % 5, b"\x00\x01")))
        s.start()
        s.advance(ms=20)
        q_ns = s.quantum_ns
        s.close()
        return q_ns, list(bus.log)

    q_big, log_big = scenario(1000)
    assert q_big == 88_000 == lookahead_ns(500_000)
    lines = [r for r in caplog.records if r.name == "stest" and "quantum" in r.getMessage()]
    assert len(lines) == 1 and lines[0].levelno == logging.INFO
    assert "88" in lines[0].getMessage() and "drive" in lines[0].getMessage()
    caplog.clear()
    q_small, log_small = scenario(88)
    assert q_small == 88_000 and log_small == log_big and len(log_big) == 19  # the 20th ends after 20 ms
    assert not [r for r in caplog.records if "quantum" in r.getMessage()]


def scenario_log(quantum_us: int) -> list:
    """Two buses, devices with periodic and reactive traffic, fake nodes with off-grid
    requests (T-10 ns on a 20 us grid), an abort and mode changes; returns everything
    observable, with times."""
    sim = Sim(quantum_us=quantum_us)
    b0 = sim.can_bus("b0", 500_000)
    b1 = sim.can_bus("b1", 1_000_000)
    seen: list = []

    class Echo(CanDevice):
        def on_frame(self, lf):
            seen.append((self.name, self.now_ns, lf))
            if lf.frame.id < 0x100:
                self.send(CanFrame(lf.frame.id + 0x400, lf.frame.data))

    class Bridge(CanDevice):  # forwards b0 traffic onto b1 at the same virtual instant
        def on_frame(self, lf):
            seen.append((self.name, self.now_ns, lf))
            b1.send(lf.frame, sender="bridge")

    p = b0.attach(CanDevice("periodic"))
    b0.attach(Echo("echo"))
    b0.attach(Bridge("bridge"))
    b1.attach(Echo("echo1", ack=True))
    n1 = FakeNode(sim, b0, "n1")
    n2 = FakeNode(sim, b0, "n2")
    n1.mode(0, "normal")
    n2.mode(0, "init", None)
    n2.mode(150_000 - 10, "normal")
    for k in range(40):
        t = 20_000 * (k + 1) - 10  # T-10 ns on a 20 us grid
        n1.request(t, k % 3 if k < 3 else 3 + k, CanFrame(0x50 + (k * 7) % 13, bytes([k])), txfp=int(k >= 30), seq=k)
        if k % 4 == 0:
            n2.request(t + 3, 100 + k, CanFrame(0x55, bytes([k, k])), seq=k)
    n1.abort(30_000, 1)  # before mailbox 1's request (39_990): no effect
    n1.abort(250_000, 13)  # k=10, requested at 219_990, still queued behind the backlog: withdrawn
    n1.abort(400_000 - 3, 20)  # k=17
    cnt = [0]

    def tick():
        cnt[0] += 1
        p.send(CanFrame(0x52, bytes([cnt[0] & 0xFF])))

    p.every(0.437, tick)
    sim.start()
    sim.advance(ms=8)
    assert not any(lf.frame.data == bytes([10]) and lf.sender == "n1" for lf in b0.log)  # aborted
    assert any(lf.frame.data == bytes([1]) and lf.sender == "n1" for lf in b0.log)  # abort before request
    assert {lf.sender for lf in b0.log} == {"n1", "n2", "periodic", "echo"}
    out = [("b0", lf) for lf in b0.log] + [("b1", lf) for lf in b1.log] + seen + n1.calls + n2.calls
    sim.close()
    return out


def test_determinism_across_quanta():
    ref = scenario_log(88)
    assert len(ref) > 100
    assert scenario_log(20) == ref
    assert scenario_log(1000) == ref  # capped to 44 us by the 1 Mbit/s bus
    for q in (44, 37, 13, 7, 1):
        assert scenario_log(q) == ref, q


def test_sim_with_only_buses_and_validation():
    s = Sim()
    with pytest.raises(StestError):
        s.start()  # nothing to simulate
    bus = s.can_bus("x")
    with pytest.raises(StestError, match="duplicate"):
        s.can_bus("x")
    with pytest.raises(ValueError):
        s.can_bus("y", 0)
    d = bus.attach(Recorder("d"))
    with pytest.raises(StestError):
        bus.attach(d)  # already attached
    with pytest.raises(StestError):
        Recorder("free").send(CanFrame(1))  # not attached to a bus
    s.start()
    with pytest.raises(StestError):
        s.can_bus("late")
    s.advance(ms=1)
    assert s.now_ns == 1_000_000
    s.close()


def test_node_can_wiring(blinky_exe):
    s = Sim()
    bus = s.can_bus("drive")
    with pytest.raises(StestError, match="no CAN bus"):
        s.node("bad", blinky_exe, can={"CAN2": "nope"})
    n = s.node("n", blinky_exe, can={"CAN2": "drive"})
    assert n.can == {"CAN2": bus}
    other = Sim().can_bus("foreign")
    with pytest.raises(StestError, match="another Sim"):
        s.node("m", blinky_exe, can={"CAN1": other})
    port = bus._nodes["n"]
    f = CanFrame(0x123, b"\x01\x02", ext=False)
    port.on_rx(LoggedFrame(5_000, "drive", f, "x"), 5_000)
    port.on_tx_result(2, {"ok": True, "arb_lost": False, "error": None}, 6_000)
    import json

    msgs = [(t, json.loads(js)) for t, js in n._inbox]
    assert msgs == [
        (5_000, {"model": "CAN2", "op": "rx", "args": {"id": 0x123, "ext": False, "rtr": False, "dlc": 2, "data": "0102"}}),
        (6_000, {"model": "CAN2", "op": "tx_result", "args": {"mailbox": 2, "ok": True, "arb_lost": False, "error": None}}),
    ]


# ---------------------------------------------------------------- review fixes


def abort_reload_calls(quantum_us: int) -> tuple[list, list]:
    sim, bus = device_sim(quantum_us)
    bus.attach(CanDevice("r"))
    blk = bus.attach(CanDevice("blk"))
    n = FakeNode(sim, bus, "n")
    n.mode(0, "normal")
    blk.send(CanFrame(0x001, bytes(8)))  # t=0: bus busy for ~230 us
    n.request(5_000, 0, CanFrame(0x300), seq=1)
    n.abort(30_000, 0)  # not on the wire yet: effective
    n.request(30_010, 0, CanFrame(0x301), seq=2)  # firmware reloads mailbox 0 right after ABRQ
    # an abort while the attempt is on the wire has no effect on it
    n.request(400_000, 1, CanFrame(0x302), seq=3)
    n.abort(400_000 + 2 * BIT, 1)
    sim.start()
    sim.advance(ms=1)
    out = (list(bus.log), list(n.calls))
    sim.close()
    return out


@pytest.mark.parametrize("q", [1, 7, 20, 88])
def test_abort_then_reload_same_mailbox(q):
    log, calls = abort_reload_calls(q)
    assert [lf.frame.id for lf in log] == [0x001, 0x301, 0x302]
    results = [(c[1], c[2], c[3]) for c in calls if c[0] == "tx_result"]
    assert results == [
        (30_000 + lookahead_ns(BR), 0, {"ok": False, "arb_lost": False, "error": None, "aborted": True, "seq": 1}),
        (log[1].t_ns, 0, {"ok": True, "arb_lost": False, "error": None, "seq": 2}),
        (log[2].t_ns, 1, {"ok": True, "arb_lost": False, "error": None, "seq": 3}),
    ]
    assert (log, calls) == abort_reload_calls(88)


def test_reload_without_abort_is_refused():
    sim, bus = device_sim()
    FakeNode(sim, bus, "n")
    bus._request("n", 0, CanFrame(0x1), 0, 0, 0)
    bus._abort("n", 0, 500)
    with pytest.raises(StestError, match="still pending"):
        bus._request("n", 0, CanFrame(0x2), 400, 0, 1)  # before the abort
    bus._request("n", 0, CanFrame(0x2), 500, 0, 1)  # at the abort: fine


def test_abort_during_failed_attempt_takes_effect_before_retry():
    sim, bus = device_sim(quantum_us=20)
    n = FakeNode(sim, bus, "n")  # alone: never ACKed
    n.mode(0, "normal")
    f = CanFrame(0x100)
    n.request(0, 0, f, seq=7)
    n.abort(10_000, 0)  # mid-flight of the first attempt (SOF 0, error frame ends ~ 120 us)
    sim.start()
    sim.advance(ms=1)
    att = dur(f) + 12 * BIT  # SOF .. ACK slot + 20-bit error frame (Task 7)
    assert [(c[1], c[3]) for c in n.calls] == [
        (att, {"ok": False, "arb_lost": False, "error": "ack", "seq": 7}),
        (max(10_000 + lookahead_ns(BR), att), {"ok": False, "arb_lost": False, "error": None, "aborted": True, "seq": 7}),
    ]
    sim.close()


def test_rx_only_to_nodes_in_normal_mode():
    sim, bus = device_sim(quantum_us=20)
    a = bus.attach(Recorder("A"))
    n_init = FakeNode(sim, bus, "init")
    n_norm = FakeNode(sim, bus, "norm")
    n_init.mode(0, "init", None)
    n_norm.mode(0, "normal")
    sim.start()
    a.send(CanFrame(0x10))
    sim.advance(ms=1)
    assert n_init.calls == []
    assert [c[0] for c in n_norm.calls] == ["rx"]
    sim.close()


@pytest.mark.parametrize("where", ["on_frame", "every"])
def test_device_callback_exception_ends_the_sim(where):
    sim, bus = device_sim()

    class Boom(CanDevice):
        def on_frame(self, lf):
            if where == "on_frame":
                raise RuntimeError("user bug")

    a = bus.attach(CanDevice("a"))
    bus.attach(Boom("boom"))

    def tick():
        a.send(CanFrame(0x100))
        if where == "every" and a.now_ns >= 300_000:
            raise RuntimeError("user bug")

    a.every(0.1, tick)
    sim.start()
    with pytest.raises(StestError, match=f"CAN device '(boom|a)' {where} callback raised: RuntimeError: user bug") as ei:
        sim.advance(ms=2)
    assert isinstance(ei.value.__cause__, RuntimeError)
    with pytest.raises(StestError, match="no longer usable"):
        sim.advance(ms=1)
    sim.close()


def test_ack_is_fixed_after_attach_and_quantum_is_read_only():
    sim, bus = device_sim()
    d = CanDevice("d", ack=False)
    d.ack = True  # fine before attach
    bus.attach(d)
    with pytest.raises(StestError, match="ack cannot change"):
        d.ack = False
    with pytest.raises(AttributeError):
        sim.quantum_ns = 1_000_000


def test_engine_cost_is_not_quadratic_in_backlog():
    """A lone sender (nobody ACKs) queueing 1 frame/ms: the backlog grows to 4000 requests
    while the head is retransmitted ~9 times per ms. Quadratic engines took ~6 s."""
    import time

    sim, bus = device_sim()
    a = bus.attach(CanDevice("a"))
    bus.attach(CanDevice("r", ack=False))
    a.every(1, lambda: a.send(CanFrame(0x100, bytes(8))))
    sim.start()
    t0 = time.perf_counter()
    sim.advance(ms=4000)
    wall = time.perf_counter() - t0
    assert bus.log == [] and bus._npending == 4000
    assert wall < 2.5, f"4 virtual s took {wall:.2f} s of wall time"
    sim.close()
