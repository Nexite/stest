"""Protocol v2: timestamped inbox messages on RUN_UNTIL (Node.post), events on ERROR."""

from __future__ import annotations

import json
import struct

import pytest

from stest import NodeCrashed, Sim, StestError
from stest import protocol as proto


# ---- wire format (byte-for-byte mirror of runtime/include/stest/protocol.h) ----


def test_proto_version_is_2():
    assert proto.PROTO_VERSION == 2


def test_run_until_encoding_empty_inbox():
    assert proto.run_until(1_000_000) == struct.pack("<IBQI", 12, 2, 1_000_000, 0)


def test_run_until_encoding_with_inbox():
    a = json.dumps({"model": "GPIOA", "op": "drive_pin", "args": {"pin": 0, "level": 1}},
                   separators=(",", ":")).encode()
    b = json.dumps({"model": "GPIOB", "op": "x", "args": {}}, separators=(",", ":")).encode()
    msg = proto.run_until(5, [(7, a), (9, b)])
    payload = struct.pack("<QI", 5, 2) + struct.pack("<QH", 7, len(a)) + a + struct.pack("<QH", 9, len(b)) + b
    assert msg == struct.pack("<IB", len(payload), 2) + payload


def test_inbox_message_encoding():
    js = proto.inbox_message("GPIOA", "drive_pin", {"pin": 3, "level": None})
    assert json.loads(js) == {"model": "GPIOA", "op": "drive_pin", "args": {"pin": 3, "level": None}}
    with pytest.raises(ValueError, match="too long"):
        proto.inbox_message("GPIOA", "x", {"blob": "a" * 70000})


def test_parse_error_with_events():
    msg = b"stest: boom"
    ev = b'{"kind":"gpio"}'
    p = struct.pack("<H", len(msg)) + msg + struct.pack("<I", 1) + struct.pack("<QH", 42, len(ev)) + ev
    assert proto.parse_error(p) == ("stest: boom", [(42, '{"kind":"gpio"}')])
    with pytest.raises(proto.ProtocolError):
        proto.parse_error(p + b"x")
    with pytest.raises(proto.ProtocolError):
        proto.parse_error(p[:-1])


def test_version_mismatch_names_both_versions(blinky_exe, monkeypatch):
    monkeypatch.setattr(proto, "PROTO_VERSION", 99)
    with Sim() as s:
        s.node("b", blinky_exe)
        with pytest.raises(NodeCrashed, match=r"node speaks control protocol version 2, but this stest Python "
                                             r"expects version 99"):
            s.start()


# ---- Node.post / inbox delivery ----


def test_post_applies_at_time(blinky_exe):
    with Sim() as s:
        n = s.node("b", blinky_exe)
        s.start()
        pa0 = n.pin("PA0")
        assert pa0.level == 0
        # Posted during the quantum ending at 1 ms; takes effect at exactly 1.5 ms.
        n.post("GPIOA", "drive_pin", {"pin": 0, "level": 1}, 1_500_000)
        s.advance(ms=1)
        assert pa0.history == [] and pa0.level == 0
        s.advance(ms=1)
        assert pa0.history == [(1_500_000, 1)]
        assert pa0.level == 1
        ev = [(t, json.loads(js)) for t, node, js in s.event_log if json.loads(js).get("pin") == 0]
        assert ev == [(1_500_000, {"kind": "gpio", "periph": "GPIOA", "port": "A", "pin": 0, "level": 1})]


def test_post_messages_apply_in_order_and_release(blinky_exe):
    with Sim(quantum_us=250) as s:
        n = s.node("b", blinky_exe)
        s.start()
        n.post("GPIOA", "drive_pin", {"pin": 0, "level": 1}, 1_500_000)
        n.post("GPIOA", "drive_pin", {"pin": 0, "level": None}, 1_700_000)  # release
        n.post("GPIOA", "drive_pin", {"pin": 1, "level": 1}, 1_700_000)
        s.advance(ms=3)
        assert n.pin("PA0").history == [(1_500_000, 1), (1_700_000, 0)]
        assert n.pin("PA1").history == [(1_700_000, 1)]
        # same instant: delivery order is posting order
        same = [json.loads(js)["pin"] for t, _, js in s.event_log if t == 1_700_000]
        assert same == [0, 1]


def test_post_at_node_now_is_allowed(blinky_exe):
    with Sim() as s:
        n = s.node("b", blinky_exe)
        s.start()
        s.advance(ms=1)
        t = n.now_ns  # exactly the quantum end
        assert t == s.now_ns
        n.post("GPIOA", "drive_pin", {"pin": 0, "level": 1}, t)
        s.advance(ms=1)
        assert n.pin("PA0").history == [(t, 1)]


def test_pin_drive_at_helper(blinky_exe):
    with Sim() as s:
        n = s.node("b", blinky_exe)
        s.start()
        p = n.pin("PA0")
        p.drive_at(1, 250_000)
        p.drive_at(0, 400_000)
        p.drive_at(None, 600_000)  # release -> analog-mode pin reads its pull level (0): no edge
        p.drive_at(1, 700_000)
        s.advance(ms=1)
        assert p.history == [(250_000, 1), (400_000, 0), (700_000, 1)]
        with pytest.raises(ValueError):
            p.drive_at(2, 900_000)


def test_post_is_deterministic_across_quanta(blinky_exe):
    def once(q):
        with Sim(quantum_us=q) as s:
            n = s.node("b", blinky_exe)
            s.start()
            for i in range(10):
                n.post("GPIOA", "drive_pin", {"pin": 0, "level": i % 2}, 5_000_000 + i * 333_333)
            s.advance(ms=600)
            return list(s.event_log)

    assert once(1000) == once(100)


def test_post_validates_arguments(blinky_exe):
    with Sim() as s:
        n = s.node("b", blinky_exe)
        with pytest.raises(ValueError, match="t_ns"):
            n.post("GPIOA", "drive_pin", {"pin": 0, "level": 1}, -1)
        with pytest.raises(TypeError, match="t_ns"):
            n.post("GPIOA", "drive_pin", {"pin": 0, "level": 1}, 1.5)
        with pytest.raises(TypeError, match="args"):
            n.post("GPIOA", "drive_pin", [0, 1], 0)
        with pytest.raises(ValueError, match="too long"):
            n.post("GPIOA", "drive_pin", {"x": "a" * 70000}, 0)


def test_post_rejects_inbox_over_max_payload(blinky_exe, monkeypatch):
    monkeypatch.setattr(proto, "MAX_PAYLOAD", 200)
    with Sim() as s:
        n = s.node("b", blinky_exe)
        s.start()
        args = {"pin": 0, "level": 1}
        one = proto.INBOX_ENTRY_OVERHEAD + len(proto.inbox_message("GPIOA", "drive_pin", args))
        fits = (200 - proto.RUN_UNTIL_FIXED) // one
        for i in range(fits):
            n.post("GPIOA", "drive_pin", args, 500_000 + i)
        with pytest.raises(StestError, match="inbox would exceed the protocol maximum of 200 bytes"):
            n.post("GPIOA", "drive_pin", args, 900_000)
        s.advance(ms=1)  # flushed: room again
        n.post("GPIOA", "drive_pin", args, 1_500_000)
        s.advance(ms=1)
        assert n.pin("PA0").level == 1


def test_post_in_past_aborts(blinky_exe):
    with Sim() as s:
        n = s.node("b", blinky_exe)
        s.start()
        s.advance(ms=2)
        n.post("GPIOA", "drive_pin", {"pin": 0, "level": 1}, 1_000_000)
        with pytest.raises(NodeCrashed) as ei:
            s.advance(ms=1)
        first = str(ei.value).splitlines()[0]
        assert "stest: inbox message for GPIOA at t=1000000 ns is in the past (now 2000" in first
        assert "op drive_pin" in first and "(exit status 70)" in first


@pytest.mark.parametrize(
    "model, op, args, expect",
    [
        ("GPIOZ", "drive_pin", {"pin": 0, "level": 1},
         r"inbox message for GPIOZ at t=1500000 ns \(op drive_pin\): unknown model"),
        ("GPIOA", "frobnicate", {}, r"inbox message for GPIOA at t=1500000 ns \(op frobnicate\): unknown op: frobnicate"),
        ("GPIOA", "drive_pin", {"pin": 0}, r"inbox message for GPIOA at t=1500000 ns \(op drive_pin\): .*level"),
        ("GPIOA", "drive_pin", {"pin": 99, "level": 1}, r"inbox message for GPIOA at t=1500000 ns \(op drive_pin\): .*pin"),
    ],
)
def test_bad_inbox_message_aborts(blinky_exe, model, op, args, expect):
    with Sim() as s:
        n = s.node("b", blinky_exe)
        s.start()
        n.post(model, op, args, 1_500_000)
        with pytest.raises(NodeCrashed, match=r"aborted at .*stest: " + expect):
            s.advance(ms=1)


def test_post_after_crash_reports_crash(faults_exe):
    with Sim() as s:
        n = s.node("u", faults_exe("unmodelled"))
        s.start()
        with pytest.raises(NodeCrashed):
            s.advance(ms=100)
        with pytest.raises(NodeCrashed, match="no longer usable"):
            n.post("GPIOA", "drive_pin", {"pin": 0, "level": 1}, 200_000_000)


# ---- ERROR carries the aborting quantum's events ----


def test_abort_quantum_events_delivered(faults_exe):
    with Sim() as s:
        n = s.node("x", faults_exe("abort_events"))
        s.start()
        with pytest.raises(NodeCrashed, match=r"stest: unmodelled write32 0x40013800"):
            s.advance(ms=100)
        assert "boom" in n.output
        assert n.stdout_tail[-1:] == ["boom"]
        h = n.pin("PA5").history
        assert len(h) == 1 and h[0][1] == 1 and 10_000_000 <= h[0][0] < 11_000_000
        kinds = [json.loads(js)["kind"] for _, node, js in s.event_log if node == "x"]
        assert kinds == ["gpio", "stdout"]


def test_abort_quantum_events_merged_with_other_nodes(faults_exe, blinky_exe):
    with Sim() as s:
        s.node("b", blinky_exe)
        s.node("x", faults_exe("abort_events"))
        s.start()
        s.advance(ms=9)
        before = len(s.event_log)
        with pytest.raises(NodeCrashed):
            s.advance(ms=100)
        tail = s.event_log[before:]
        assert [node for _, node, _ in tail] == ["x", "x"]
        assert [t for t, _, _ in tail] == sorted(t for t, _, _ in tail)


def test_abort_output_not_duplicated(faults_exe):
    """The aborting quantum's lines arrive as events (in order) and the unterminated
    fragment exactly once, last, although it is also reported on stderr."""
    with Sim() as s:
        n = s.node("x", faults_exe("abort_output"))
        s.start()
        with pytest.raises(NodeCrashed) as ei:
            s.advance(ms=100)
        assert 'stest: stdout unterminated {"text":"frag"}' in str(ei.value)
        assert n.output == ["early 1", "early 2", "early 3", "out 1", "out 2", "itm 1", "frag"]
        assert n.log == ["itm 1"]
        evs = [json.loads(js) for _, _, js in s.event_log]
        assert evs[-1] == {"kind": "stdout", "periph": "stdout", "text": "frag", "partial": True}


# ---- quantum-boundary independence (nodes never stop past the quantum end) ----


def _pollpin_run(exe, quantum_us, post_at, poll_skip):
    """Drive PA0 high at 2.0 ms, low at 2.5 ms + 12 345 ns, high at 3.0 ms, low at 3.4 ms
    (pollpin mirrors each on PA5). ``post_at``: "boundary" posts each message when the sim
    is at the quantum boundary at or just before its t (t == T when t is on the grid);
    "early" posts all of them at t=0. -> (event log, counts)."""
    drives = [(2_000_000, 1), (2_512_345, 0), (3_000_000, 1), (3_400_000, 0)]
    with Sim(quantum_us=quantum_us, poll_skip=poll_skip) as s:
        n = s.node("p", exe)
        s.start()
        if post_at == "early":
            for t, lv in drives:
                n.pin("PA0").drive_at(lv, t)
        else:
            for t, lv in drives:
                s.advance(ns=(t // s.quantum_ns) * s.quantum_ns - s.now_ns)
                assert n.now_ns == s.now_ns <= t
                n.pin("PA0").drive_at(lv, t)
        s.advance(ms=5)
        assert n.var("rounds") == 4
        return list(s.event_log), [n.var(f"counts[{i}]") for i in range(4)]


@pytest.mark.parametrize("poll_skip", [0, None])
def test_inbox_delivery_is_quantum_independent(pollpin_exe, poll_skip):
    ref = _pollpin_run(pollpin_exe, 1000, "early", poll_skip)
    log, counts = ref
    pa5 = [(t, json.loads(js)["level"]) for t, _, js in log if json.loads(js)["pin"] == 5]
    assert [lv for _, lv in pa5] == [1, 0, 1, 0]
    # PA5 follows within a few accesses of each drive
    for (t5, _), t0 in zip(pa5, (2_000_000, 2_512_345, 3_000_000, 3_400_000)):
        assert 0 < t5 - t0 < 200
    for q, post_at in [(1000, "boundary"), (100, "early"), (100, "boundary")]:
        got = _pollpin_run(pollpin_exe, q, post_at, poll_skip)
        assert got[0] == log, (q, post_at)
        if poll_skip == 0:  # with fast-forward on, counted spins are dilated per quantum (documented)
            assert got[1] == counts, (q, post_at)


@pytest.mark.parametrize("quantum_us", [1000, 100, 7])
def test_node_stops_exactly_at_the_quantum_end(blinky_exe, tim_exe, quantum_us):
    with Sim(quantum_us=quantum_us) as s:
        nodes = [s.node("b", blinky_exe), s.node("t", tim_exe)]
        s.start()
        for _ in range(40):
            s.advance(ns=33_337)  # ends off the quantum grid too
            for n in nodes:
                assert n.now_ns == s.now_ns, (n.name, n.now_ns, s.now_ns)
        s.advance(ms=600)
        assert all(n.now_ns == s.now_ns for n in nodes)
        assert [t for t, _ in nodes[0].pin("PA5").history] == [500_001_825]


@pytest.mark.parametrize("off", [3, 13, 24])
def test_events_up_to_an_off_grid_quantum_end_are_visible(pollpin_exe, off):
    """A quantum end off the 25 ns access grid: the node still stops at exactly T and every
    event up to T (here the drive at T itself) has fired and been reported."""
    t = 2_000_000 + off
    with Sim(poll_skip=0) as s:
        n = s.node("p", pollpin_exe)
        s.start()
        n.pin("PA0").drive_at(1, t)
        s.advance(ns=t)
        assert n.now_ns == t == s.now_ns
        assert n.pin("PA0").history == [(t, 1)]


def _two_node_log(exe, quantum_us):
    with Sim(quantum_us=quantum_us, poll_skip=0) as s:
        a, b = s.node("a", exe), s.node("b", exe)
        s.start()
        # Off-grid times, interleaved across the nodes (and one shared instant).
        for node, plan in ((a, [(1_000_013, 1), (1_700_007, 0), (2_300_011, 1), (2_900_000, 0)]),
                           (b, [(1_000_013, 1), (1_699_999, 0), (2_300_012, 1), (2_899_993, 0)])):
            for t, lv in plan:
                node.pin("PA0").drive_at(lv, t)
        s.advance(ms=4)
        assert a.var("rounds") == b.var("rounds") == 4
        return list(s.event_log)


def test_two_node_event_log_is_quantum_independent(pollpin_exe):
    ref = _two_node_log(pollpin_exe, 1000)
    assert {node for _, node, _ in ref} == {"a", "b"}
    assert ref == sorted(ref, key=lambda e: (e[0], e[1]))  # merged by time, then node order
    assert _two_node_log(pollpin_exe, 100) == ref
    assert _two_node_log(pollpin_exe, 7) == ref
