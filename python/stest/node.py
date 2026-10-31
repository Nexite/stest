"""One firmware instance (host process) driven over the control protocol."""

from __future__ import annotations

import collections
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import protocol as proto
from .errors import StestError
from .pin import Pin

if TYPE_CHECKING:
    from .sim import Sim

STDERR_TAIL_LINES = 20  # shown in NodeCrashed messages / Node.stderr_tail
STDERR_KEEP_LINES = 200  # retained per node (the pytest plugin's failure report)


def _make_pdeathsig():
    """Linux: have the kernel SIGKILL the node if this (parent) process dies, so a
    crashed or killed test run never leaves firmware spinning. None elsewhere (macOS has
    no equivalent; there the Sim registry's atexit handler is the safety net)."""
    if not sys.platform.startswith("linux"):
        return None
    import ctypes

    libc = ctypes.CDLL(None, use_errno=True)
    PR_SET_PDEATHSIG = 1
    prctl = libc.prctl

    def preexec() -> None:
        prctl(PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0)

    return preexec


_PDEATHSIG = _make_pdeathsig()


class Node:
    """A node of a :class:`Sim`. Create with ``sim.node(name, exe)``."""

    def __init__(self, sim: Sim, index: int, name: str, exe: str | os.PathLike, env: dict | None):
        self.sim = sim
        self.index = index
        self.name = name
        self.exe = Path(exe)
        self.env = dict(env or {})
        self.pid: int | None = None
        self.proto_version: int | None = None
        self.anchor_addr: int | None = None  # runtime address of stest_symbol_anchor
        self.now_ns = 0  # virtual time the node last reported (STOPPED)
        self._proc: subprocess.Popen | None = None
        self._chan: proto.Channel | None = None
        self._stderr: collections.deque[str] = collections.deque(maxlen=STDERR_KEEP_LINES)
        self._stderr_lock = threading.Lock()
        self._stderr_thread: threading.Thread | None = None
        self._seq = 0
        self._pin_history: dict[tuple[str, int], list[tuple[int, int]]] = {}

    # ---- public API ----
    @property
    def stderr_tail(self) -> list[str]:
        """The node's last 20 stderr lines (captured continuously)."""
        return self.stderr_lines(STDERR_TAIL_LINES)

    def stderr_lines(self, n: int = STDERR_KEEP_LINES) -> list[str]:
        """The node's last ``n`` (at most 200) captured stderr lines."""
        with self._stderr_lock:
            return list(self._stderr)[-n:] if n > 0 else []

    def pin(self, name: str) -> Pin:
        return Pin(self, name)

    def call(self, model: str, op: str, **args: Any) -> Any:
        """Call ``op`` on peripheral model ``model`` (e.g. "GPIOA"); returns its JSON result."""
        p = self._request(proto.model_call(model, op, args), proto.Msg.MODEL_RESULT, f"MODEL_CALL {model}.{op}")
        return json.loads(p)

    def read_mem(self, addr: int, n: int) -> bytes:
        """Read n bytes of the node's host memory at addr (no MMIO side effects)."""
        return self._request(proto.read_mem(addr, n), proto.Msg.MEM, f"READ_MEM {addr:#x}")

    def write_mem(self, addr: int, data: bytes) -> None:
        self._request(proto.write_mem(addr, data), proto.Msg.OK, f"WRITE_MEM {addr:#x}")

    def var(self, expr: str) -> Any:
        """Read a firmware variable by name: "count", "vars.c:count", "func::s_local",
        "g_cfg.lamps[2].duty". Ints, floats, bool, enums (an int that equals its enumerator
        name), structs (read-only mapping), arrays (list). The sim is paused between calls."""
        r, slide = self._resolve_var(expr)
        return r.type.decode(self.read_mem(r.addr + slide, r.type.size))

    def set_var(self, expr: str, value: Any) -> None:
        """Write a scalar firmware variable (int, float, bool or enumerator name); the value
        is range-checked against the variable's DWARF type before anything is written."""
        r, slide = self._resolve_var(expr)
        self.write_mem(r.addr + slide, r.type.encode(value))

    def _resolve_var(self, expr: str):
        from .symbols import SymbolTable

        if self.anchor_addr is None:
            raise StestError(f"node {self.name!r}: variables are available after sim.start()")
        table = SymbolTable.load(self.exe)
        return table.resolve(expr), self.anchor_addr - table.anchor_addr

    def __repr__(self) -> str:
        return f"<Node {self.name!r} pid={self.pid} t={self.now_ns} ns>"

    # ---- process management (used by Sim) ----
    def _spawn(self, extra_env: dict[str, str]) -> None:
        if not self.exe.is_file():
            raise StestError(f"node {self.name!r}: executable {self.exe} not found")
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        env = {**os.environ, **self.env, **extra_env, "STEST_CTRL": str(child.fileno())}
        try:
            self._proc = subprocess.Popen(
                [str(self.exe)],
                env=env,
                stdin=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                pass_fds=(child.fileno(),),
                preexec_fn=_PDEATHSIG,
            )
        finally:
            child.close()
        self.pid = self._proc.pid
        self._chan = proto.Channel(parent)
        self._stderr_thread = threading.Thread(
            target=self._drain_stderr, name=f"stest-stderr-{self.name}", daemon=True
        )
        self._stderr_thread.start()

    def _drain_stderr(self) -> None:
        assert self._proc and self._proc.stderr
        for raw in self._proc.stderr:  # until EOF (process exit)
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            with self._stderr_lock:
                self._stderr.append(line)

    def _join_stderr(self, timeout: float = 2.0) -> None:
        if self._stderr_thread:
            self._stderr_thread.join(timeout)

    @property
    def _alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def _exit_description(self) -> str:
        rc = self._proc.returncode if self._proc else None
        if rc is None:
            return "still running"
        if rc < 0:
            try:
                return f"killed by signal {signal.Signals(-rc).name}"
            except ValueError:
                return f"killed by signal {-rc}"
        return f"exit status {rc}"

    def _wait_exit(self, timeout: float) -> None:
        """Wait for the process to end on its own; SIGKILL it after ``timeout``. Reaps."""
        if not self._proc:
            return
        try:
            self._proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self._proc.kill()
            self._proc.wait()

    def _kill(self, grace_s: float) -> None:
        """SIGTERM, then SIGKILL after grace_s; always reaps."""
        if not self._proc:
            return
        if self._proc.poll() is None:
            try:
                self._proc.terminate()
            except ProcessLookupError:
                pass
            self._wait_exit(grace_s)
        else:
            self._proc.wait()

    def _close_channel(self) -> None:
        if self._chan:
            self._chan.close()
            self._chan = None

    # ---- requests ----
    def _send(self, data: bytes) -> None:
        self.sim._check_usable()
        assert self._chan
        try:
            self._chan.send(data)
        except proto.ChannelClosed:
            self.sim._node_gone(self, None)

    def _recv(self, what: str, deadline: float) -> tuple[int, bytes]:
        """Receive one message; ERROR/EOF/timeout end the simulation (raise)."""
        assert self._chan
        try:
            mtype, payload = self._chan.recv(deadline)
        except TimeoutError:
            self.sim._node_timeout(self, what)
        except proto.ChannelClosed:
            self.sim._node_gone(self, None)
        except proto.ProtocolError as e:
            self.sim._node_gone(self, f"protocol error: {e}")
        if mtype == proto.Msg.ERROR:
            self.sim._node_gone(self, payload.decode("utf-8", "replace"))
        return mtype, payload

    def _request(self, msg: bytes, expect: proto.Msg, what: str) -> bytes:
        if not self._chan:
            raise StestError(f"node {self.name!r} is not running (call sim.start() first)")
        self._send(msg)
        mtype, payload = self._recv(what, time.monotonic() + self.sim.wall_timeout_s)
        if mtype == proto.Msg.REQ_ERROR:
            raise StestError(f"node {self.name!r}: {payload.decode('utf-8', 'replace')}")
        if mtype != expect:
            self.sim._node_gone(self, f"protocol error: expected {expect.name}, got message type {mtype}")
        return payload

    def _record_events(self, events: list[tuple[int, str]]) -> list[tuple[int, int, int, str, str]]:
        """-> merge keys (t_ns, node_index, seq, node_name, json) for the Sim's event log."""
        out = []
        for t, js in events:
            data = json.loads(js)
            if data.get("kind") == "gpio":
                self._pin_history.setdefault((data["port"], int(data["pin"])), []).append((t, int(data["level"])))
            out.append((t, self.index, self._seq, self.name, js))
            self._seq += 1
        return out
