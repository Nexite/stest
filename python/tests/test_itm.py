"""ITM / DWT / firmware output capture (selftest/firmware/itm, faults_printf)."""

from __future__ import annotations

import re

import pytest

from stest import Sim, WaitTimeout


def run(exe, ms, **kw):
    s = Sim(**kw)
    n = s.node("n", exe)
    s.start()
    s.advance(ms=ms)
    return s, n


def test_itm_lines(itm_exe):
    s, n = run(itm_exe, 250)
    with s:
        assert n.log[:3] == ["hello 0", "hello 1", "hello 2"]
        assert len(n.log) == 3
        evs = [e for e in s.event_log if '"kind": "itm"' in e[2] or '"kind":"itm"' in e[2]]
        assert len(evs) == 3 and '"port"' in evs[0][2]


def test_itm_event_times_are_virtual_and_deterministic(itm_exe):
    def times():
        s, n = run(itm_exe, 350)
        with s:
            return [t for t, _, js in s.event_log if "itm" in js]

    a = times()
    assert a == times()
    assert [round((b - a_) / 1e6) for a_, b in zip(a, a[1:])] == [100, 100, 100]


def test_wait_for_log(itm_exe):
    with Sim() as s:
        n = s.node("n", itm_exe)
        s.start()
        m = n.wait_for_log(r"hello (\d+)", timeout_ms=500)
        assert m.group(0) == "hello 0" and m.group(1) == "0"
        # consumed lines are not matched again
        m = n.wait_for_log(r"hello (\d+)", timeout_ms=500)
        assert m.group(1) == "1"
        m = n.wait_for_log(r"hello 3$", timeout_ms=1000)
        assert isinstance(m, re.Match)
        with pytest.raises(WaitTimeout):
            n.wait_for_log("never printed", timeout_ms=150)


def test_dwt_cyccnt(itm_exe):
    s, n = run(itm_exe, 60)
    with s:
        assert n.var("cycles_10ms") == pytest.approx(800_000, abs=8_000)


def test_no_debugger_drops_itm(itm_exe):
    s, n = run(itm_exe, 250, debugger_attached=False)
    with s:
        assert n.log == []
        assert n.var("loops") >= 2  # the firmware did not block
        # DWT is still usable once the firmware enables TRCENA itself
        assert n.var("cycles_10ms") == pytest.approx(800_000, abs=8_000)


def test_no_debugger_env(itm_exe):
    with Sim() as s:
        n = s.node("n", itm_exe, env={"STEST_DEBUGGER": "0"})
        s.start()
        s.advance(ms=250)
        assert n.log == []


def test_stdout_captured(faults_exe):
    with Sim() as s:
        n = s.node("n", faults_exe("printf"))
        s.start()
        s.advance(ms=20)
        assert n.stdout_tail[-1] == "hi"


def test_stdout_tail_keeps_last_200_lines(faults_exe):
    with Sim() as s:
        n = s.node("n", faults_exe("printf"), env={"STEST_FAULT_STDOUT_LINES": "20000"})
        s.start()
        s.advance(ms=20)
        t = n.stdout_tail
        assert len(t) == 200 and t[-1] == "hi" and t[-2] == "line 19999"


def test_unterminated_itm_line_is_flushed_on_abort(itm_exe):
    from stest import NodeCrashed

    with Sim() as s:
        n = s.node("n", itm_exe, env={"STEST_ITM_PARTIAL": "1"})
        s.start()
        with pytest.raises(NodeCrashed) as e:
            s.advance(ms=100)
        assert 'ITM unterminated {"port":0,"text":"partial"}' in str(e.value)
        assert n.log[-1] == "partial"


# ---- Node.output: ITM port 0 + stdout merged in virtual time (itm_mixed) ----


def test_output_merges_printf_and_itm_in_order(itm_mixed_exe):
    s, n = run(itm_mixed_exe, 320)
    with s:
        assert n.output == ["out 0", "itm 0", "pc A!", "out 1", "itm 1", "pc B!",
                            "out 2", "itm 2", "pc C!", "out 3"]
        assert n.log == ["itm 0", "itm 1", "itm 2"]  # ITM-only view unchanged
        assert n.stdout_tail == ["out 0", "pc A!", "out 1", "pc B!", "out 2", "pc C!", "out 3"]
        import json

        evs = [(t, json.loads(js)) for t, _, js in s.event_log]
        out = [(t, e) for t, e in evs if e["kind"] in ("stdout", "itm")]
        assert [e["text"] for _, e in out] == n.output
        assert all(e["periph"] == ("ITM" if e["kind"] == "itm" else "stdout") for _, e in out)
        t = {e["text"]: t for t, e in out}
        # printf at the start of each 100 ms iteration, ITM 10 ms later, then stdio at once
        assert [round((t[f"out {i + 1}"] - t[f"out {i}"]) / 1e6) for i in range(3)] == [100, 100, 100]
        assert all(round((t[f"itm {i}"] - t[f"out {i}"]) / 1e6) == 10 for i in range(3))
        assert all(t[f"pc {c}!"] == t[f"itm {i}"] for i, c in enumerate("ABC"))  # stdio costs no virtual time


def test_output_is_deterministic(itm_mixed_exe):
    def once(q):
        s, n = run(itm_mixed_exe, 420, quantum_us=q)
        with s:
            return n.output, [(t, js) for t, _, js in s.event_log]

    a = once(1000)
    assert a == once(1000)
    assert a[0] == once(250)[0]


def test_wait_for_log_sources(itm_mixed_exe):
    with Sim() as s:
        n = s.node("n", itm_mixed_exe)
        s.start()
        m = n.wait_for_log(r"out (\d+)", timeout_ms=500)  # default source "any": a printf-only line
        assert m.group(1) == "0" and n.now_ns < 20_000_000
        m = n.wait_for_log(r"(out|itm) \d+", timeout_ms=500)  # "any" cursor moved past "out 0"
        assert m.group(0) == "itm 0"
        assert n.wait_for_log(r"itm 1", timeout_ms=500, source="itm").group(0) == "itm 1"
        assert n.wait_for_log(r"pc .!", timeout_ms=500, source="stdout").group(0) == "pc A!"
        with pytest.raises(WaitTimeout):
            n.wait_for_log(r"itm", timeout_ms=50, source="stdout")
        with pytest.raises(ValueError, match="source must be one of"):
            n.wait_for_log("x", timeout_ms=1, source="uart")


def test_unterminated_stdout_line_is_flushed_on_abort(itm_mixed_exe):
    from stest import NodeCrashed

    with Sim() as s:
        n = s.node("n", itm_mixed_exe, env={"STEST_STDOUT_PARTIAL": "1"})
        s.start()
        with pytest.raises(NodeCrashed) as e:
            s.advance(ms=100)
        assert 'stdout unterminated {"text":"half"}' in str(e.value)
        # events of the quantum that aborted are not delivered (ERROR replaces STOPPED);
        # the unterminated tail still arrives through stderr
        assert n.output[0] == "out 0" and n.output[-1] == "half"
