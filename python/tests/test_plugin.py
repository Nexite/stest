"""The stest pytest plugin (via pytester) and the no-orphan-processes guarantees."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import textwrap
import time

import pytest


def _alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def _wait_gone(pid: int, timeout_s: float = 5.0) -> bool:
    end = time.monotonic() + timeout_s
    while time.monotonic() < end:
        if not _alive(pid):
            return True
        time.sleep(0.05)
    return not _alive(pid)


def test_failure_report_has_stderr_and_event_sections(pytester, faults_exe):
    pytester.makepyfile(
        f"""
        def test_crash(sim):
            sim.node("u", r"{faults_exe('unmodelled')}")
            sim.start()
            sim.advance(ms=100)
        """
    )
    r = pytester.runpytest("-rA")
    r.assert_outcomes(failed=1)
    r.stdout.fnmatch_lines(["*Captured stest node u stderr call*", "*stest: unmodelled write32 0x40013800*"])


def test_failure_report_events_section_blinky(pytester, blinky_exe):
    pytester.makepyfile(
        f"""
        def test_fail(sim):
            n = sim.node("b", r"{blinky_exe}")
            sim.start()
            sim.advance(ms=600)
            assert False, "boom"
        """
    )
    r = pytester.runpytest()
    r.assert_outcomes(failed=1)
    r.stdout.fnmatch_lines(["*Captured stest node b stderr call*", "*Captured stest events call*", '*"kind":"gpio"*'])


def test_passing_test_has_no_sections(pytester, blinky_exe):
    pytester.makepyfile(
        f"""
        def test_ok(sim):
            sim.node("b", r"{blinky_exe}")
            sim.start()
            sim.advance(ms=600)
        """
    )
    r = pytester.runpytest("-rA")
    r.assert_outcomes(passed=1)
    assert "stest events" not in r.stdout.str()


def test_options_and_marker_override(pytester):
    pytester.makepyfile(
        """
        import pytest

        def test_defaults(sim):
            assert sim.quantum_us == 1000 and sim.wall_timeout_s == 30.0

        @pytest.mark.stest(quantum_us=100, wall_timeout_s=5)
        def test_marker(sim):
            assert sim.quantum_us == 100 and sim.wall_timeout_s == 5.0
        """
    )
    pytester.runpytest().assert_outcomes(passed=2)
    pytester.makepyfile(
        test_opts="""
        import pytest

        def test_cli(sim):
            assert sim.quantum_us == 250 and sim.wall_timeout_s == 7.5

        @pytest.mark.stest(quantum_us=50)
        def test_marker_partial(sim):
            assert sim.quantum_us == 50 and sim.wall_timeout_s == 7.5
        """
    )
    r = pytester.runpytest("test_opts.py", "--stest-quantum-us=250", "--stest-wall-timeout=7.5", "--strict-markers")
    r.assert_outcomes(passed=2)


def test_teardown_leaves_no_children_when_test_raises(pytester, blinky_exe):
    pidfile = pytester.path / "pid"
    pytester.makepyfile(
        f"""
        def test_raises(sim):
            n = sim.node("b", r"{blinky_exe}")
            sim.start()
            open(r"{pidfile}", "w").write(str(n.pid))
            raise RuntimeError("test blew up")
        """
    )
    pytester.runpytest().assert_outcomes(failed=1)
    pid = int(pidfile.read_text())
    assert not _alive(pid)  # reaped (not even a zombie)


_ORPHAN_SCRIPT = textwrap.dedent(
    """
    import os, signal, sys, threading
    from stest import Sim

    s = Sim()
    n = s.node("spin", sys.argv[1])
    s.start()
    print(n.pid, flush=True)
    mode = sys.argv[2]
    if mode == "interrupt":
        threading.Timer(0.5, lambda: os.kill(os.getpid(), signal.SIGINT)).start()
    elif mode == "gc":
        import gc, time
        from stest import protocol
        n._send(protocol.run_until(10**9))  # the firmware now spins; nobody reads the reply
        time.sleep(0.3)
        del n, s  # dropped without close(): the registry must still kill the node at exit
        gc.collect()
        sys.exit(0)
    elif mode == "sigkill":
        threading.Timer(0.5, lambda: os.kill(os.getpid(), signal.SIGKILL)).start()
    s.advance(ms=1000)  # the firmware spins: blocks until the timer fires
    """
)


def _run_orphan_script(exe, mode: str):
    p = subprocess.run(
        [sys.executable, "-c", _ORPHAN_SCRIPT, str(exe), mode], capture_output=True, text=True, timeout=60
    )
    return int(p.stdout.split()[0]), p


def test_node_killed_when_python_exits_without_close(faults_exe):
    pid, p = _run_orphan_script(faults_exe("spin"), "interrupt")
    assert p.returncode != 0 and "KeyboardInterrupt" in p.stderr
    assert _wait_gone(pid), f"node {pid} orphaned after the interpreter exited"


def test_node_killed_when_sim_garbage_collected_without_close(faults_exe):
    pid, p = _run_orphan_script(faults_exe("spin"), "gc")
    assert p.returncode == 0, p.stderr
    assert _wait_gone(pid), f"node {pid} orphaned after the Sim was garbage-collected"


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="PR_SET_PDEATHSIG is Linux-only")
def test_node_killed_when_python_is_sigkilled(faults_exe):
    pid, p = _run_orphan_script(faults_exe("spin"), "sigkill")
    assert p.returncode == -signal.SIGKILL
    assert _wait_gone(pid), f"node {pid} orphaned after the parent was SIGKILLed"
