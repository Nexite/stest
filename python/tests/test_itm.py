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
