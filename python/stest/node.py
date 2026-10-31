"""One firmware instance (host process) driven over the control protocol."""

from __future__ import annotations

import collections
import json
import fcntl
import os
import re
import select
import signal
import socket
import subprocess
import sys
import threading
import termios
import time
from pathlib import Path
from typing import TYPE_CHECKING, Any

from . import protocol as proto
from .errors import StestError
from .peripherals import Adc, Dac, SpiBurst, SpiBus, Timer
from .pin import Pin

if TYPE_CHECKING:
    from .sim import Sim

STDERR_TAIL_LINES = 20  # shown in NodeCrashed messages / Node.stderr_tail
STDERR_KEEP_LINES = 200  # retained per node (the pytest plugin's failure report)
STDOUT_KEEP_LINES = 200  # retained firmware stdout lines (Node.stdout_tail)
_ITM_PARTIAL_PREFIX = "stest: ITM unterminated "


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
        self._stdout: collections.deque[str] = collections.deque(maxlen=STDOUT_KEEP_LINES)
        self._stdout_lock = threading.Lock()  # held by the drain thread from read() to append
        self._stdout_thread: threading.Thread | None = None
        self._itm: dict[int, list[str]] = {}  # port -> completed lines
        self._log_cursor = 0  # Node.wait_for_log has consumed log[:cursor]
        self._seq = 0
        self._symbols = None  # SymbolTable, loaded once by Sim.start()
        self._symbols_error: Exception | None = None
        self._resolved: dict[str, Any] = {}  # expr -> Resolved (static; valid for the node's lifetime)
        self._pin_history: dict[tuple[str, int], list[tuple[int, int]]] = {}
        self._spi_bursts: dict[str, list[SpiBurst]] = {}

    # ---- public API ----
    @property
    def stderr_tail(self) -> list[str]:
        """The node's last 20 stderr lines (captured continuously)."""
        return self.stderr_lines(STDERR_TAIL_LINES)

    def stderr_lines(self, n: int = STDERR_KEEP_LINES) -> list[str]:
        """The node's last ``n`` (at most 200) captured stderr lines."""
        with self._stderr_lock:
            return list(self._stderr)[-n:] if n > 0 else []

    @property
    def log(self) -> list[str]:
        """ITM port-0 lines (text before each ``\\n``) in order, as of the last completed
        quantum. Empty when no ITM output happened (e.g. ``Sim(debugger_attached=False)``).
        A final unterminated line is appended if the node exits or aborts mid-line."""
        return list(self._itm.get(0, ()))

    def itm_port(self, port: int) -> list[str]:
        """Lines written to ITM stimulus port ``port`` (0 is :attr:`log`)."""
        return list(self._itm.get(int(port), ()))

    @property
    def stdout_tail(self) -> list[str]:
        """The node's last 200 firmware stdout lines (``printf``; line-buffered by the
        runtime, complete as of the last completed quantum)."""
        return self.stdout_lines(STDOUT_KEEP_LINES)

    def stdout_lines(self, n: int = STDOUT_KEEP_LINES) -> list[str]:
        """The node's last ``n`` (at most 200) captured stdout lines."""
        self._sync_stdout()
        with self._stdout_lock:
            return list(self._stdout)[-n:] if n > 0 else []

    def wait_for_log(self, pattern: str, timeout_ms: float) -> re.Match:
        """Advances the simulation until a ITM port-0 line not yet consumed by an earlier
        ``wait_for_log`` matches the regex ``pattern`` (``re.search``); returns the match and
        consumes lines up to and including the matching one. Raises WaitTimeout after
        ``timeout_ms`` of virtual time. Checked at every quantum boundary."""
        rx = re.compile(pattern)
        found: list[re.Match] = []

        def cond() -> bool:
            lines = self._itm.get(0, [])
            for i in range(self._log_cursor, len(lines)):
                m = rx.search(lines[i])
                if m:
                    self._log_cursor = i + 1
                    found.append(m)
                    return True
            return False

        self.sim.wait_for(cond, timeout_ms, what=f"log line matching {pattern!r} on node {self.name!r}")
        return found[0]

    def pin(self, name: str) -> Pin:
        return Pin(self, name)

    def timer(self, name: str) -> Timer:
        """Timer model ``name`` (e.g. "TIM7"): ``.state()``, ``.update_hz``, ``.channel(n).duty``."""
        return Timer(self, name)

    def adc(self, n: int) -> Adc:
        """ADC ``n`` (1-based): ``.set(channel, volts)``, ``.get(channel)``."""
        return Adc(self, n)

    def dac(self, n: int) -> Dac:
        """DAC ``n`` (model "DAC<n>"): ``.channel(c).volts``, ``.state()``."""
        return Dac(self, f"DAC{int(n)}")

    def spi(self, name: str) -> SpiBus:
        """SPI model ``name`` (e.g. "SPI1"): ``.bursts``, ``.queue_miso(data)``, ``.wait_for_bytes(n, timeout_ms)``."""
        return SpiBus(self, name)

    def call(self, model: str, op: str, **args: Any) -> Any:
        """Call ``op`` on peripheral model ``model`` (e.g. "GPIOA"); returns its JSON result."""
        p = self._request(proto.model_call(model, op, args), proto.Msg.MODEL_RESULT, f"MODEL_CALL {model}.{op}")
        return json.loads(p)

    def read_mem(self, addr: int, n: int) -> bytes:
        """Read n bytes of the node's host memory at addr (no MMIO side effects)."""
        if n > proto.MAX_READ_MEM:
            raise StestError(
                f"node {self.name!r}: read_mem of {n} bytes exceeds the protocol maximum of {proto.MAX_READ_MEM}"
            )
        return self._request(proto.read_mem(addr, n), proto.Msg.MEM, f"READ_MEM {addr:#x}")

    def write_mem(self, addr: int, data: bytes) -> None:
        if len(data) > proto.MAX_WRITE_MEM:
            raise StestError(
                f"node {self.name!r}: write_mem of {len(data)} bytes exceeds the protocol maximum "
                f"of {proto.MAX_WRITE_MEM}"
            )
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

    def _load_symbols(self) -> None:
        """Called by Sim.start() after HELLO. Failures (e.g. no DWARF) surface on first var()."""
        from .errors import SymbolError
        from .symbols import SymbolTable

        self._resolved.clear()
        self._symbols_error = None
        try:
            self._symbols = SymbolTable.load(self.exe)
        except SymbolError as e:
            self._symbols, self._symbols_error = None, e

    def _resolve_var(self, expr: str):
        if self.anchor_addr is None:
            raise StestError(f"node {self.name!r}: variables are available after sim.start()")
        if self._symbols is None:
            if self._symbols_error is not None:
                raise self._symbols_error
            self._load_symbols()
            if self._symbols is None:
                raise self._symbols_error
        r = self._resolved.get(expr)
        if r is None:
            r = self._resolved[expr] = self._symbols.resolve(expr)
        return r, self.anchor_addr - self._symbols.anchor_addr

    def __repr__(self) -> str:
        return f"<Node {self.name!r} pid={self.pid} t={self.now_ns} ns>"

    # ---- process management (used by Sim) ----
    def _spawn(self, extra_env: dict[str, str]) -> None:
        if not self.exe.is_file():
            raise StestError(f"node {self.name!r}: executable {self.exe} not found")
        parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        # STEST_PARENT_PID: the runtime exits as soon as this process dies, so a crashed or
        # SIGKILLed test run never leaves firmware spinning (Linux: PR_SET_PDEATHSIG; all
        # platforms: a 200 ms getppid() watchdog thread). On Linux PDEATHSIG follows the
        # forking *thread*, so call sim.start() from a long-lived thread (as pytest does).
        env = {
            **os.environ,
            **self.env,
            **extra_env,
            "STEST_CTRL": str(child.fileno()),
            "STEST_PARENT_PID": str(os.getpid()),
        }
        try:
            self._proc = subprocess.Popen(
                [str(self.exe)],
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                pass_fds=(child.fileno(),),
            )
        finally:
            child.close()
        self.pid = self._proc.pid
        self._chan = proto.Channel(parent)
        self._stderr_thread = threading.Thread(
            target=self._drain_stderr, name=f"stest-stderr-{self.name}", daemon=True
        )
        self._stderr_thread.start()
        self._stdout_thread = threading.Thread(
            target=self._drain_stdout, name=f"stest-stdout-{self.name}", daemon=True
        )
        self._stdout_thread.start()

    def _drain_stderr(self) -> None:
        assert self._proc and self._proc.stderr
        for raw in self._proc.stderr:  # until EOF (process exit)
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            with self._stderr_lock:
                self._stderr.append(line)
            if line.startswith(_ITM_PARTIAL_PREFIX):  # runtime flushed an unterminated ITM line
                try:
                    d = json.loads(line[len(_ITM_PARTIAL_PREFIX):])
                    self._itm.setdefault(int(d["port"]), []).append(str(d["text"]))
                except (ValueError, KeyError, TypeError):
                    pass

    def _drain_stdout(self) -> None:
        """Reads the stdout pipe with non-blocking reads so a chatty firmware never fills it.
        The lock is held from read() through the append: _sync_stdout() relies on it."""
        assert self._proc and self._proc.stdout
        fd = self._proc.stdout.fileno()
        os.set_blocking(fd, False)
        pending = b""
        eof = False
        while not eof:
            select.select([fd], [], [])
            with self._stdout_lock:
                while True:
                    try:
                        chunk = os.read(fd, 65536)
                    except BlockingIOError:
                        break
                    except OSError:
                        eof = True
                        break
                    if not chunk:
                        eof = True
                        break
                    pending += chunk
                    *lines, pending = pending.split(b"\n")
                    for raw in lines:
                        self._stdout.append(raw.decode("utf-8", "replace").rstrip("\r"))
                if eof and pending:
                    self._stdout.append(pending.decode("utf-8", "replace").rstrip("\r"))

    def _sync_stdout(self) -> None:
        """Returns once everything the (parked or exited) node has written to stdout has been
        consumed by the drain thread. The runtime flushes stdout before each STOPPED."""
        if not self._proc or not self._proc.stdout or not self._stdout_thread:
            return
        fd = self._proc.stdout.fileno()
        deadline = time.monotonic() + 2.0
        while self._stdout_thread.is_alive() and time.monotonic() < deadline:
            with self._stdout_lock:
                try:
                    buf = bytearray(4)
                    fcntl.ioctl(fd, termios.FIONREAD, buf)
                    if int.from_bytes(buf, sys.byteorder, signed=True) == 0:
                        return
                except OSError:
                    return
            time.sleep(0.0005)

    def _join_stderr(self, timeout: float = 2.0) -> None:
        if self._stderr_thread:
            self._stderr_thread.join(timeout)
        if self._stdout_thread:
            self._stdout_thread.join(timeout)

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
        self.sim._check_usable()  # first: after a crash or close() the channel is gone too
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
            elif data.get("kind") == "spi":
                self._spi_bursts.setdefault(data["periph"], []).append(
                    SpiBurst(int(data["t_start"]), bytes.fromhex(data["mosi"]), bytes.fromhex(data["miso"]), t)
                )
            elif data.get("kind") == "itm":
                self._itm.setdefault(int(data["port"]), []).append(data["text"])
            out.append((t, self.index, self._seq, self.name, js))
            self._seq += 1
        return out
