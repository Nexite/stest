"""CAN fault confinement (ISO 11898-1, simplified as documented in stest.can.bus), error
injection, bus-off and recovery: the bus engine with scripted fake nodes, and firmware nodes
(selftest `can_err`: error notifications + SCE interrupt; `can_abom`: AutoBusOff) end to end."""

from __future__ import annotations

import json
import time

import pytest

from stest import CanBus, CanDevice, Sim, StestError
from stest.can import IFS_BITS, CanFrame, LoggedFrame, lookahead_ns

BR = 500_000
BIT = 2_000  # ns per bit at 500 kbit/s
IFS = IFS_BITS * BIT
L = lookahead_ns(BR)  # 88 us
ERR = 20 * BIT  # error frame (simplified, see stest.can.bus)
SUSPEND = 8 * BIT  # error-passive transmitter's suspend transmission
W = 11 * BIT  # one occurrence of 11 recessive bits

# ESR (RM0351 CAN_ESR)
EWGF, EPVF, BOFF = 1 << 0, 1 << 1, 1 << 2
# HAL_CAN_ERROR_*
HAL_EWG, HAL_EPV, HAL_BOF, HAL_STF, HAL_ACK, HAL_BR = 0x1, 0x2, 0x4, 0x8, 0x20, 0x40


def dur(f: CanFrame) -> int:
    return f.duration_ns(BR)


def stuffed(bits: list[int]) -> int:
    """Bits on the wire for unstuffed ``bits`` (a stuff bit after 5 equal bits)."""
    n, last, run = len(bits), None, 0
    for b in bits:
        run = run + 1 if b == last else 1
        last = b
        if run == 5:
            n += 1
            last, run = 1 - b, 1
    return n


def std_head(f: CanFrame) -> list[int]:
    """Unstuffed SOF .. data bits of a standard data frame."""
    def bits(v, w):
        return [(v >> i) & 1 for i in range(w - 1, -1, -1)]

    out = [0] + bits(f.id, 11) + [int(f.rtr), 0, 0] + bits(f.dlc, 4)
    for byte in f.data:
        out += bits(byte, 8)
    return out


def arb_bits(f: CanFrame) -> int:
    """Stuffed bits SOF .. RTR (end of the arbitration field) of a standard frame."""
    return stuffed(std_head(f)[:13])


def esr(node) -> int:
    return node.call("CAN2", "state")["esr"]


def esr_fields(v: int) -> dict:
    return {"tec": (v >> 16) & 0xFF, "rec": v >> 24, "lec": (v >> 4) & 7,
            "ewgf": v & EWGF != 0, "epvf": v & EPVF != 0, "boff": v & BOFF != 0}


class FakeNode:
    """A scripted node (actions issued at the first quantum end >= their time, like the real
    node wiring). ``calls`` records every engine callback with its time; results keep their
    counters (tec, rec, epoch)."""

    def __init__(self, sim: Sim, bus: CanBus, name: str):
        self.name, self.bus = name, bus
        self.calls: list[tuple] = []
        self.script: list[tuple] = []
        bus._attach_node(name, self._on_rx, self._on_tx_result, self._on_fault)
        sim._can_hooks.append(self._hook)

    def _on_rx(self, lf, t, fc=None):
        self.calls.append(("rx", t, lf, fc))

    def _on_tx_result(self, key, result, t):
        self.calls.append(("tx_result", t, key, result))

    def _on_fault(self, op, args, t):
        self.calls.append((op, t, args))

    def mode(self, t, mode, *, abom=False, reset=False):
        self.script.append((t, "mode", (mode, BR if mode == "normal" else None, abom, reset)))

    def request(self, t, key, frame, txfp=0, seq=0, **kw):
        self.script.append((t, "request", (key, frame, txfp, seq, kw)))

    def _hook(self, T):
        due = sorted((s for s in self.script if s[0] <= T), key=lambda s: s[0])
        self.script = [s for s in self.script if s[0] > T]
        for t, action, a in due:
            if action == "mode":
                self.bus._set_node_mode(self.name, a[0], a[1], t, abom=a[2], reset=a[3])
            else:
                key, frame, txfp, seq, kw = a
                self.bus._request(self.name, key, frame, t, txfp, seq, **kw)

    def results(self):
        return [c for c in self.calls if c[0] == "tx_result"]


def res(kind: str, seq: int, tec: int, rec: int = 0, epoch: int = 0) -> dict:
    d = {"ok": kind == "ok", "arb_lost": kind == "arb_lost", "error": None, "seq": seq}
    if kind not in ("ok", "arb_lost"):
        d["error"] = kind
    d.update(tec=tec, rec=rec, epoch=epoch)
    return d


def device_sim(q_us: int = 20) -> tuple[Sim, CanBus]:
    s = Sim(quantum_us=q_us)
    return s, s.can_bus("bus0", BR)


def recovery_instant(start: int, log: list[LoggedFrame]) -> int:
    """Independent count of 128 occurrences of 11 recessive bits from ``start`` on a bus whose
    only traffic is ``log`` (successful frames): each frame's EOF + IFS window counts one,
    idle time one per 11 bit times."""
    cursor, left = start, 128
    for lf in log:
        sof = lf.t_ns - dur(lf.frame)
        if sof < start:
            continue
        n = (sof - cursor) // W
        if n >= left:
            return cursor + left * W
        left -= n + 1
        if left == 0:
            return lf.t_ns + IFS
        cursor = lf.t_ns + IFS
    return cursor + left * W


# ---------------------------------------------------------------- engine (fake nodes)


def test_frame_error_positions():
    assert CanFrame(0x101, b"\x00").arbitration_bits() == 14 == arb_bits(CanFrame(0x101))
    assert CanFrame(0x7FF).arbitration_bits() == stuffed(std_head(CanFrame(0x7FF))[:13])
    ext = CanFrame(0x1ABCDE, ext=True)
    assert ext.arbitration_bits() > 33  # 33 unstuffed bits SOF..RTR, plus stuff bits
    a, b = CanFrame(0x123, b"\x01"), CanFrame(0x123, b"\x02")
    assert a.bits_to_difference(b) == stuffed(std_head(a)[:19 + 7]) == b.bits_to_difference(a)
    with pytest.raises(ValueError):
        a.bits_to_difference(a)


def test_no_ack_lone_node_counters():
    sim, bus = device_sim()
    n = FakeNode(sim, bus, "n")
    n.mode(0, "normal")
    f = CanFrame(0x100)
    n.request(0, 0, f, seq=1)
    sim.start()
    sim.advance(ms=5)
    bits = f.bit_length()
    att = (bits + 12) * BIT  # SOF .. ACK slot, then a 20-bit error frame
    period = att + IFS
    r = n.results()
    assert len(r) > 20
    sofs = [k * period for k in range(16)]
    for _ in range(16, len(r)):  # error passive from the 16th: 8 bits of suspend transmission
        sofs.append(sofs[-1] + period + SUSPEND)
    for k, (_, t, key, result) in enumerate(r):
        assert key == 0
        assert t == sofs[k] + att
        assert result == res("ack", 1, tec=min(8 * (k + 1), 128))  # no-ACK while passive: TEC stays
    assert bus.log == []
    acks = [e for e in bus.errors if e["kind"] == "ack"]
    assert acks[:20] == [{"t_ns": s + (bits - 8) * BIT, "kind": "ack", "sender": "n"} for s in sofs[:20]]
    passive = [e for e in bus.errors if e["kind"] != "ack"]
    assert passive == [{"t_ns": acks[15]["t_ns"], "kind": "error_passive", "sender": "n"}]
    assert all(e["t_ns"] <= sim.now_ns for e in bus.errors)  # never ahead of the simulation
    sim.close()


def test_inject_errors_retransmit_engine():
    sim, bus = device_sim()
    n1, n2 = FakeNode(sim, bus, "n1"), FakeNode(sim, bus, "n2")
    for n in (n1, n2):
        n.mode(0, "normal")
    f1, f2 = CanFrame(0x101, b"\x00"), CanFrame(0x102, b"\x00")
    n1.request(1_000, 0, f1, seq=1)
    n2.request(1_000, 0, f2, seq=1)
    bus.inject_errors(1, target="n1")
    sim.start()
    sim.advance(ms=2)
    s0 = 1_000
    t_err = s0 + arb_bits(f1) * BIT
    post = max(t_err + ERR, s0 + L)  # results never earlier than SOF + lookahead (causality)
    s1 = t_err + ERR + IFS  # retransmission
    e1 = s1 + dur(f1)
    e2 = e1 + IFS + dur(f2)
    assert bus.errors == [{"t_ns": t_err, "kind": "injected", "sender": "n1"}]
    assert [(lf.t_ns, lf.frame, lf.sender) for lf in bus.log] == [(e1, f1, "n1"), (e2, f2, "n2")]
    assert n1.calls == [
        ("tx_result", post, 0, res("bit_recessive", 1, tec=8)),
        ("tx_result", e1, 0, res("ok", 1, tec=7)),
        ("rx", e2, bus.log[1], {"tec": 7, "rec": 0, "epoch": 0}),
    ]
    assert n2.calls == [
        ("tx_result", post, 0, res("arb_lost", 1, tec=0)),
        ("error", post, {"lec": "stuff", "tec": 0, "rec": 1, "epoch": 0}),
        ("tx_result", e1, 0, res("arb_lost", 1, tec=0, rec=1)),
        ("rx", e1, bus.log[0], {"tec": 0, "rec": 0, "epoch": 0}),
        ("tx_result", e2, 0, res("ok", 1, tec=0)),
    ]
    sim.close()


def test_inject_errors_applies_from_now_and_validates():
    sim, bus = device_sim()
    a = bus.attach(CanDevice("a"))
    bus.attach(CanDevice("b"))
    with pytest.raises(ValueError):
        bus.inject_errors(0)
    with pytest.raises(StestError, match="no sender named 'zz'"):
        bus.inject_errors(1, target="zz")
    sim.start()
    f = CanFrame(0x100, bytes(8))
    a.send(f)
    sim.advance(us=40)  # the frame is on the wire (SOF 0)
    bus.inject_errors(2)  # applies to frames whose SOF is >= now: not the one on the wire
    a.send(CanFrame(0x101))
    a.send(CanFrame(0x102))
    a.send(CanFrame(0x103))
    sim.advance(ms=2)
    assert [lf.frame.id for lf in bus.log] == [0x100, 0x101, 0x102, 0x103]
    errs = [(e["kind"], e["sender"]) for e in bus.errors]
    assert errs == [("injected", "a"), ("injected", "a")]  # 0x101 twice (count 2), then delivered
    assert bus.errors[0]["t_ns"] == dur(f) + IFS + arb_bits(CanFrame(0x101)) * BIT
    sim.close()


def test_receiver_error_passive_and_successful_rx_sets_127():
    sim, bus = device_sim()
    d = bus.attach(CanDevice("d"))  # devices are always error active (no counters)
    n = FakeNode(sim, bus, "n")  # the only ACK: an error-passive receiver still ACKs
    n.mode(0, "normal")
    bus.inject_errors(128, target="d")
    sim.start()
    d.send(CanFrame(0x200, b"\x01"))
    sim.advance(ms=12)
    errs = [c for c in n.calls if c[0] == "error"]
    assert [c[2]["rec"] for c in errs] == list(range(1, 129))
    assert all(c[2]["lec"] == "stuff" and c[2]["tec"] == 0 for c in errs)
    rx = [c for c in n.calls if c[0] == "rx"]
    assert len(rx) == 1 and rx[0][3] == {"tec": 0, "rec": 127, "epoch": 0}  # REC > 127 -> 127
    assert [lf.frame.id for lf in bus.log] == [0x200]
    states = [(e["kind"], e["sender"]) for e in bus.errors if e["kind"] != "injected"]
    assert states == [("error_passive", "n"), ("error_active", "n")]
    sim.close()


def test_same_id_different_data_is_a_bit_error():
    sim, bus = device_sim()
    n1, n2, n3 = FakeNode(sim, bus, "n1"), FakeNode(sim, bus, "n2"), FakeNode(sim, bus, "n3")
    for n in (n1, n2, n3):
        n.mode(0, "normal")
    a, b = CanFrame(0x123, b"\x01"), CanFrame(0x123, b"\x02")
    n1.request(1_000, 0, a, seq=1, nart=True)  # one attempt only
    n2.request(1_000, 0, b, seq=1)
    sim.start()
    sim.advance(ms=1)
    t_err = 1_000 + stuffed(std_head(a)[:26]) * BIT  # first differing data bit (bit 6 of byte 0)
    post = max(t_err + ERR, 1_000 + L)
    s1 = t_err + ERR + IFS
    assert bus.errors == [{"t_ns": t_err, "kind": "bit", "sender": "n1"},
                          {"t_ns": t_err, "kind": "bit", "sender": "n2"}]
    assert n1.calls[0] == ("tx_result", post, 0, res("bit_recessive", 1, tec=8))
    assert n2.calls[0] == ("tx_result", post, 0, res("bit_recessive", 1, tec=8))
    assert ("error", post, {"lec": "stuff", "tec": 0, "rec": 1, "epoch": 0}) in n3.calls
    assert [(lf.t_ns, lf.frame, lf.sender) for lf in bus.log] == [(s1 + dur(b), b, "n2")]  # n2 retried
    assert n2.calls[-1] == ("tx_result", s1 + dur(b), 0, res("ok", 1, tec=7))
    sim.close()


def bus_off_scenario(q_us: int):
    """Node n (ABOM) driven to bus-off by 32 injected errors while devices keep the bus busy;
    returns everything observable."""
    sim, bus = device_sim(q_us)
    n = FakeNode(sim, bus, "n")
    bus.attach(CanDevice("ack"))
    p = bus.attach(CanDevice("p"))
    n.mode(0, "normal", abom=True)
    f = CanFrame(0x100, b"\x55")
    n.request(10_000 - 10, 0, f, seq=1)
    n.request(10_000 - 7, 1, CanFrame(0x101), seq=2)
    bus.inject_errors(32, target="n")
    cnt = [0]

    def tick():
        cnt[0] += 1
        p.send(CanFrame(0x300 + cnt[0] % 3, bytes([cnt[0] & 0xFF]) * (cnt[0] % 9)))

    p.every(0.5, tick)
    sim.start()
    sim.advance(ms=12)
    out = (list(bus.log), list(bus.errors), list(n.calls))
    sim.close()
    return out


def test_bus_off_and_abom_recovery_on_a_busy_bus():
    log, errors, calls = bus_off_scenario(20)
    inj = [e for e in errors if e["kind"] == "injected"]
    assert len(inj) == 32 and all(e["sender"] == "n" for e in inj)
    states = [(e["kind"], e["t_ns"]) for e in errors if e["kind"] not in ("injected", "ack")]
    assert [k for k, _ in states] == ["error_passive", "bus_off", "error_active"]
    assert states[0][1] == inj[15]["t_ns"] and states[1][1] == inj[31]["t_ns"]
    tecs = [c[3]["tec"] for c in calls if c[0] == "tx_result" and c[3]["error"]]
    assert tecs == [8 * k for k in range(1, 33)]
    start = inj[31]["t_ns"] + ERR + IFS  # recovery counting starts when the bus is free again
    t_rec = recovery_instant(start, log)
    recover_at = t_rec + L  # BOFF clears one lookahead after the 128th occurrence
    assert states[2][1] == recover_at
    assert ("counters", recover_at, {"tec": 0, "rec": 0, "epoch": 0}) in calls
    assert t_rec - start > 128 * W  # traffic slowed the recovery down
    # the pending frames resume after recovery (0x100 first: arbitration) and only then
    mine = [lf for lf in log if lf.sender == "n"]
    assert [lf.frame.id for lf in mine] == [0x100, 0x101]
    sof = mine[0].t_ns - dur(mine[0].frame)
    assert sof >= recover_at and sof == max(recover_at, max(lf.t_ns + IFS for lf in log if lf.t_ns < sof))
    # while bus-off, n neither transmitted, received nor acknowledged
    assert not [c for c in calls if c[0] == "rx" and inj[31]["t_ns"] < c[1] < recover_at]
    assert [c[3]["tec"] for c in calls if c[0] == "tx_result" and c[3]["ok"]] == [0, 0]


def test_bus_off_determinism_across_quanta():
    ref = bus_off_scenario(88)
    for q in (37, 7, 1):
        assert bus_off_scenario(q) == ref, q


def test_abom0_recovers_only_after_init_and_init_restarts_abom_count():
    def run(abom: bool, q: int = 20):
        sim, bus = device_sim(q)
        n = FakeNode(sim, bus, "n")
        bus.attach(CanDevice("ack"))
        n.mode(0, "normal", abom=abom)
        n.request(0, 0, CanFrame(0x100), seq=1)
        bus.inject_errors(32)
        sim.start()
        sim.advance(ms=3)
        boff = next(e for e in bus.errors if e["kind"] == "bus_off")
        free = boff["t_ns"] + ERR + IFS
        n.mode(free + 1_000_000, "init")  # during an ABOM count: restarts it
        n.mode(free + 1_500_000, "normal", abom=abom)
        sim.advance(ms=8)
        out = (boff, [c for c in n.calls if c[0] == "counters"], list(bus.errors), list(bus.log))
        sim.close()
        return out

    for abom in (False, True):
        ref = run(abom)
        for q in (88, 37, 7, 1):
            assert run(abom, q) == ref, (abom, q)
        boff, counters, errors, log = ref
        t_on = boff["t_ns"] + ERR + IFS + 1_500_000
        recover_at = t_on + 128 * W + L
        assert counters == [("counters", recover_at, {"tec": 0, "rec": 0, "epoch": 0})], abom
        assert errors[-1] == {"t_ns": recover_at, "kind": "error_active", "sender": "n"}
        assert log[0].t_ns == recover_at + dur(CanFrame(0x100))


def test_abom0_stays_bus_off_without_init():
    sim, bus = device_sim()
    n = FakeNode(sim, bus, "n")
    bus.attach(CanDevice("ack"))
    n.mode(0, "normal")
    n.request(0, 0, CanFrame(0x100), seq=1)
    bus.inject_errors(32)
    sim.start()
    sim.advance(ms=20)
    assert [e["kind"] for e in bus.errors if e["kind"] != "injected"] == ["error_passive", "bus_off"]
    assert bus.log == [] and not [c for c in n.calls if c[0] == "counters"]
    sim.close()


def test_controller_reset_zeroes_counters_with_a_new_epoch():
    sim, bus = device_sim()
    n = FakeNode(sim, bus, "n")
    bus.attach(CanDevice("ack"))
    n.mode(0, "normal")
    n.request(0, 0, CanFrame(0x100), seq=1)
    bus.inject_errors(17)
    sim.start()
    sim.advance(ms=2)  # TEC 136 (passive), then 135 after the success
    assert n.results()[-1][3] == res("ok", 1, tec=135)
    t = sim.now_ns + 5
    n.mode(t, "sleep", reset=True)  # MCR.RESET
    n.mode(t + 10_000, "normal")
    n.request(t + 50_000, 0, CanFrame(0x100), seq=2)
    sim.advance(ms=1)
    assert n.results()[-1][3] == res("ok", 2, tec=0, epoch=1)
    assert bus.errors[-1] == {"t_ns": t, "kind": "error_active", "sender": "n"}
    # the controller withdraws its requests before a reset (the bxCAN model does); else: engine bug
    bus._request("n", 1, CanFrame(0x7), sim.now_ns, 0, 3)
    with pytest.raises(StestError, match="reset at t=.* with requests still pending"):
        bus._set_node_mode("n", "sleep", None, sim.now_ns + 1, reset=True)
    sim.close()


def test_errors_scenario_determinism():
    def run(q):
        sim, bus = device_sim(q)
        nodes = [FakeNode(sim, bus, f"n{i}") for i in range(3)]
        bus.attach(CanDevice("ack"))
        d = bus.attach(CanDevice("d"))
        for i, n in enumerate(nodes):
            n.mode(0, "normal", abom=i == 0)
        rx = FakeNode(sim, bus, "rx")  # receive only: reset (MCR.RESET) inside d's error burst
        rx.mode(0, "normal")
        rx.mode(450_000 - 7, "sleep", reset=True)
        rx.mode(470_000 - 3, "normal")
        ticks = [0]

        def tick():
            ticks[0] += 1
            d.send(CanFrame(0x121, b"\x07"))
            if ticks[0] == 10:
                bus.inject_errors(2, target="d")  # from a device callback: applies from its time
        for k in range(30):
            t = 20_000 * (k + 1) - 10
            # node i sends id 0x120 + i; n1's 0x121 meets d's 0x121 (other data) now and then
            nodes[k % 3].request(t, k % 3 if k < 3 else 3 + k, CanFrame(0x120 + k % 3, bytes([k % 3])), seq=k)
        d.every(0.3, tick)
        bus.inject_errors(5, target="d")
        bus.inject_errors(40, target="n0")
        sim.start()
        sim.advance(us=300)
        bus.inject_errors(3)
        sim.advance(ms=20)
        out = (list(bus.log), list(bus.errors), [n.calls for n in nodes + [rx]])
        sim.close()
        return out

    ref = run(88)
    inj_d = [e["t_ns"] for e in ref[1] if e["kind"] == "injected" and e["sender"] == "d"]
    assert len(inj_d) >= 7 and sum(t >= 3_000_000 for t in inj_d) >= 2  # incl. the callback's 2
    errs = [(c[1], c[2]) for c in ref[2][3] if c[0] == "error"]  # rx's receive errors in d's burst
    assert {a["epoch"] for _, a in errs} == {0, 1}  # before and after its reset at 450 us - 7 ns
    new = [(t, a["rec"]) for t, a in errs if a["epoch"] == 1]
    assert new[0][1] == 1 and new[0][0] > 470_000  # counted from 0 again after the reset
    assert any(t > 450_000 and a["epoch"] == 0 for t, a in errs)  # posted after the reset: model drops it

    assert {e["kind"] for e in ref[1]} >= {"injected", "bit", "error_passive", "bus_off", "error_active"}
    for q in (37, 13, 7, 1):
        assert run(q) == ref, q


# ---------------------------------------------------------------- firmware nodes


def can_events(sim: Sim, node: str, kind: str) -> list[tuple[int, dict]]:
    return [(t, d) for t, name, js in sim.event_log if name == node and (d := json.loads(js))["kind"] == kind]


def first_tx(sim: Sim, node: str) -> int:
    return can_events(sim, node, "can_tx")[0][0]


def test_no_ack_alone(can_exe):
    with Sim() as s:
        bus = s.can_bus("drive", BR)
        n = s.node("n", can_exe("err"), can={"CAN2": bus})
        s.start()
        s.advance(ms=10.1)
        t_req = first_tx(s, "n")
        f = CanFrame(0x101, b"\x00")
        bits = f.bit_length()
        period = (bits + 12) * BIT + IFS
        t0 = time.perf_counter()
        s.advance(ns=t_req + 16 * period + 1_000_000 - s.now_ns)  # 16 attempts, then 1 ms passive
        wall = time.perf_counter() - t0
        assert s.now_ns == t_req + 16 * period + 1_000_000  # the simulation progresses
        assert bus.log == []
        acks = [e for e in bus.errors if e["kind"] == "ack"]
        assert [e["t_ns"] for e in acks[:16]] == [t_req + k * period + (bits - 8) * BIT for k in range(16)]
        # still retried, now after 8 bits of suspend transmission (error passive)
        assert len(acks) > 17 and acks[16]["t_ns"] - acks[15]["t_ns"] == period + SUSPEND
        assert acks[17]["t_ns"] - acks[16]["t_ns"] == period + SUSPEND
        assert [e for e in bus.errors if e["kind"] != "ack"] == [
            {"t_ns": acks[15]["t_ns"], "kind": "error_passive", "sender": "n"}]
        st = n.call("CAN2", "state")
        assert (st["tec"], st["rec"], st["error_state"]) == (128, 0, "passive")
        assert esr_fields(esr(n)) == {"tec": 128, "rec": 0, "lec": 0, "ewgf": True, "epvf": True, "boff": False}
        tsr = st["tsr"]
        assert tsr & 0xFF == 0x08 and not tsr & (1 << 26)  # TERR0, mailbox 0 still pending
        assert n.var("err_code_or") & (HAL_ACK | HAL_EWG | HAL_EPV) == HAL_ACK | HAL_EWG | HAL_EPV
        assert n.var("sce_count") >= len(acks) - 1  # one SCE interrupt per error (LECIE)
        print(f"test_no_ack_alone: {wall:.3f} s wall for {(s.now_ns - 10_100_000) / 1e6:.2f} virtual ms")


def inject_scenario(can_exe, q_us: int):
    with Sim(quantum_us=q_us) as s:
        bus = s.can_bus("drive", BR)
        n1 = s.node("n1", can_exe(1), can={"CAN2": bus})
        n2 = s.node("n2", can_exe(2), can={"CAN2": bus})
        bus.inject_errors(1, target="n1")
        s.start()
        s.advance(ms=15)
        st = [n.call("CAN2", "state") for n in (n1, n2)]
        out = {
            "errors": list(bus.errors), "log": list(bus.log), "events": list(s.event_log),
            "state": [(x["tec"], x["rec"], x["error_state"]) for x in st],
            "esr": [esr(n) for n in (n1, n2)], "rx": [n.var("rx_count") for n in (n1, n2)],
            "t_req": first_tx(s, "n1"),
        }
    return out


def test_inject_errors_retransmit(can_exe):
    out = inject_scenario(can_exe, 1000)
    t_req = out["t_req"]
    f1, f2 = CanFrame(0x101, b"\x00"), CanFrame(0x102, b"\x00")
    t_err = t_req + arb_bits(f1) * BIT
    assert out["errors"] == [{"t_ns": t_err, "kind": "injected", "sender": "n1"}]
    e1 = t_err + ERR + IFS + dur(f1)  # retransmitted at once, delivered
    assert [(lf.t_ns, lf.frame, lf.sender) for lf in out["log"]] == [(e1, f1, "n1"), (e1 + IFS + dur(f2), f2, "n2")]
    assert out["state"] == [(7, 0, "active"), (0, 0, "active")]  # n1: TEC 8 -> 7 after success
    assert [esr_fields(v)["tec"] for v in out["esr"]] == [7, 0]
    assert out["rx"] == [1, 1]
    assert inject_scenario(can_exe, 37) == out  # determinism across quanta


def bus_off_e2e(can_exe, q_us: int):
    with Sim(quantum_us=q_us) as s:
        bus = s.can_bus("drive", BR)
        n1 = s.node("n1", can_exe("abom"), can={"CAN2": bus})
        n2 = s.node("n2", can_exe(2), can={"CAN2": bus})
        bus.inject_errors(32, target="n1")
        s.start()
        s.advance(ms=10.1)
        t_req = first_tx(s, "n1")
        f1, f2 = CanFrame(0x101, b"\x00"), CanFrame(0x102, b"\x00")
        pa = (arb_bits(f1) + 20) * BIT + IFS  # one injected attempt + IFS
        s16 = t_req + 15 * pa
        free16 = s16 + pa
        s17 = free16 + dur(f2) + IFS  # n1 error passive: suspended 8 bits, so n2's frame goes first
        s32 = s17 + 15 * (pa + SUSPEND)
        post32 = s32 + L  # the bus-off result (SOF + lookahead > error frame end)
        start = s32 + pa
        recover_at = start + 128 * W + L
        snaps = {}
        for name, t in (("before", post32 - 1), ("boff", post32), ("late", recover_at - 1), ("rec", recover_at)):
            s.advance(ns=t - s.now_ns)
            snaps[name] = (esr_fields(esr(n1)), n1.call("CAN2", "state")["error_state"])
        s.advance(ns=19_000_000 - s.now_ns)
        out = {
            "times": (t_req, s17, s32, recover_at), "snaps": snaps, "errors": list(bus.errors), "log": list(bus.log),
            "events": list(s.event_log), "fw": [n1.var(v) for v in ("sce_count", "boff_cb_count", "err_code_or")],
            "n2": n2.call("CAN2", "state"),
        }
    return out


def test_bus_off_and_recovery(can_exe):
    out = bus_off_e2e(can_exe, 1000)
    t_req, s17, s32, recover_at = out["times"]
    f1, f2 = CanFrame(0x101, b"\x00"), CanFrame(0x102, b"\x00")
    inj = [e for e in out["errors"] if e["kind"] == "injected"]
    assert [e["t_ns"] for e in inj][16:] == [s17 + k * (out["times"][2] - s17) // 15 + arb_bits(f1) * BIT for k in range(16)]
    assert inj[31]["t_ns"] == s32 + arb_bits(f1) * BIT
    states = [(e["kind"], e["t_ns"]) for e in out["errors"] if e["kind"] != "injected"]
    assert states == [("error_passive", inj[15]["t_ns"]), ("bus_off", inj[31]["t_ns"]), ("error_active", recover_at)]
    snaps = out["snaps"]
    assert snaps["before"] == ({"tec": 248, "rec": 0, "lec": 0, "ewgf": True, "epvf": True, "boff": False}, "passive")
    assert snaps["boff"][0]["boff"] and snaps["boff"][0]["tec"] == 255 and snaps["boff"][1] == "bus_off"
    assert snaps["late"][0]["boff"]
    assert snaps["rec"] == ({"tec": 0, "rec": 0, "lec": 0, "ewgf": False, "epvf": False, "boff": False}, "active")
    sce, boff_cb, code = out["fw"]
    assert sce >= 32 and boff_cb >= 1
    assert code & (HAL_BR | HAL_EWG | HAL_EPV | HAL_BOF) == HAL_BR | HAL_EWG | HAL_EPV | HAL_BOF
    # traffic resumes: n2's frame during n1's suspend, then n1's pending frame at recovery
    assert [(lf.t_ns, lf.frame, lf.sender) for lf in out["log"]] == [
        (s17 - IFS, f2, "n2"), (recover_at + dur(f1), f1, "n1")]
    # n2 was a receiver of all 32 errors (REC 32) and then received n1's frame
    assert (out["n2"]["tec"], out["n2"]["rec"]) == (0, 31)
    assert bus_off_e2e(can_exe, 37) == out  # determinism across quanta


def test_bus_off_manual_recovery(can_exe):
    with Sim() as s:
        bus = s.can_bus("drive", BR)
        n1 = s.node("n1", can_exe("err"), can={"CAN2": bus})
        bus.attach(CanDevice("ack"))
        bus.inject_errors(32, target="n1")
        s.start()
        s.advance(ms=16)
        boff = [e for e in bus.errors if e["kind"] == "bus_off"]
        assert len(boff) == 1 and boff[0]["t_ns"] < 13_000_000
        assert esr_fields(esr(n1))["boff"]  # ABOM = 0: no recovery while normal, however long
        n1.set_var("can_run", 0)  # HAL_CAN_Stop: INRQ
        s.advance(ms=1)
        n1.set_var("can_run", 1)  # HAL_CAN_Start: leave init, then 128 x 11 recessive bits
        s.advance(ms=2)
        assert esr_fields(esr(n1))["boff"]
        assert [d["mode"] for _, d in can_events(s, "n1", "can_mode")][-2:] == ["init", "normal"]
        t_on = [t for t, d in can_events(s, "n1", "can_mode") if d["mode"] == "normal"][-1]
        s.advance(ms=4)
        recover_at = recovery_instant(t_on, bus.log) + L
        assert bus.errors[-1] == {"t_ns": recover_at, "kind": "error_active", "sender": "n1"}
        assert esr_fields(esr(n1)) == {"tec": 0, "rec": 0, "lec": 0, "ewgf": False, "epvf": False, "boff": False}
        assert any(lf.sender == "n1" and lf.t_ns - dur(lf.frame) >= recover_at for lf in bus.log)


def test_error_passive_flags(can_exe):
    with Sim() as s:
        bus = s.can_bus("drive", BR)
        n1 = s.node("n1", can_exe("err"), can={"CAN2": bus})
        n2 = s.node("n2", can_exe(2), can={"CAN2": bus})
        bus.inject_errors(16, target="n1")
        s.start()
        s.advance(ms=10.1)
        t_req = first_tx(s, "n1")
        pa = (arb_bits(CanFrame(0x101)) + 20) * BIT + IFS
        # 12th error (TEC 96): warning; its result is posted at SOF + lookahead (+10 us: the ISR ran)
        s.advance(ns=t_req + 11 * pa + L + 10_000 - s.now_ns)
        e = esr_fields(esr(n1))
        assert (e["tec"], e["ewgf"], e["epvf"]) == (96, True, False)
        assert n1.var("err_code_or") & HAL_EWG and not n1.var("err_code_or") & HAL_EPV
        s.advance(ns=4 * pa)  # 16th error: TEC 128, error passive
        e = esr_fields(esr(n1))
        assert (e["tec"], e["ewgf"], e["epvf"], e["boff"]) == (128, True, True, False)
        assert n1.var("err_code_or") & (HAL_EPV | HAL_BR) == HAL_EPV | HAL_BR
        assert esr_fields(esr(n2))["rec"] == 16  # a receiver of every error frame
        s.advance(ms=2)  # n2's frame, then n1's succeeds: TEC 127, back to error active
        e = esr_fields(esr(n1))
        assert (e["tec"], e["ewgf"], e["epvf"]) == (127, True, False)
        assert esr_fields(esr(n2))["rec"] == 15
        assert n1.call("CAN2", "state")["error_state"] == "active"
        assert [x["kind"] for x in bus.errors if x["kind"] != "injected"] == ["error_passive", "error_active"]
