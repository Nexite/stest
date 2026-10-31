"""Simulated CAN bus: arbitration, ACK, queuing and delivery timing on the global clock.

Engine (run by the Sim at every quantum end T, after the node events up to T are known
and device callbacks due by T have run):

    while requests are pending:
        t_idle = max(bus_free_at, earliest t_req)       # bus free = previous EOF + IFS
        if t_idle >= T: break                           # later requests (stamped <= that instant) may still join
        aborts with t_abort <= t_idle withdraw their request (the frame had not started)
        contenders = each sender's top request with t_req <= t_idle
                     (node TXFP=0: lowest arbitration key, then lowest key; TXFP=1: lowest seq;
                      devices: lowest arbitration key, then request order)
        winner = min(arbitration key, sender order); bit-identical frames from several senders all win
        eof = t_idle + duration; bus_free_at = eof + IFS
        ACK = a non-winning node whose mode at SOF (t_idle) is "normal", or a non-winning device with ack=True
        ACK:    winners: tx_result ok at eof (request done); every other node: rx at eof;
                devices' on_frame + log append at eof (run at the first quantum end >= eof)
        no ACK: winners: tx_result error "ack" at eof; request stays pending (dropped if NART)
        losers: tx_result arb_lost at eof; request stays pending (dropped if NART)

The brief's algorithm decides idle instants <= T; this one decides only t_idle < T, because a
request stamped exactly T can still arrive after the engine ran at T (a test's ``send()`` at
``sim.now_ns == T``, a device callback due at T running at a later boundary, a node event at
T reported in the next quantum). Deciding T early would make arbitration depend on where
quantum boundaries fall.

Causality: the Sim caps its quantum at ``lookahead_ns(bitrate)`` (the floor of 44 bit times)
for every bus. Every request is stamped at or after the previous quantum end T_prev, so
every decided frame starts at t_idle >= T_prev and, being at least 45 bits long (one stuff
bit is unavoidable), ends at eof > T_prev + lookahead >= T. Results are therefore always
posted strictly after the current quantum end (never into a node's past) and every frame is
delivered to devices at a later boundary, in EOF order. ``_run`` asserts this.

Bit time: ``frame.duration_ns(bitrate)`` is ceil(bits * 1e9 / bitrate) per frame; the IFS is
ceil(3 * 1e9 / bitrate) ns. Both are exact for bitrates dividing 1e9 (500 k, 250 k, 1 M, ...);
otherwise each rounds up by < 1 ns.
"""

from __future__ import annotations

import bisect
import functools
from typing import TYPE_CHECKING, Any, Callable

from ..errors import StestError
from .device import CanDevice
from .frame import IFS_BITS, CanFrame
from .log import LoggedFrame, format_asc, format_candump

if TYPE_CHECKING:
    from ..sim import Sim

MODES = ("sleep", "init", "normal")


# tx_result payloads (fresh dicts per call: receivers may keep or extend them)
def _ok() -> dict:
    return {"ok": True, "arb_lost": False, "error": None}


def _arb_lost() -> dict:
    return {"ok": False, "arb_lost": True, "error": None}


def _no_ack() -> dict:
    return {"ok": False, "arb_lost": False, "error": "ack"}


@functools.lru_cache(maxsize=4096)
def _frame_ns(frame: CanFrame, bitrate: int) -> int:
    return frame.duration_ns(bitrate)


class _Req:
    __slots__ = ("key", "frame", "t_req", "txfp", "seq", "nart", "akey")

    def __init__(self, key: Any, frame: CanFrame, t_req: int, txfp: int, seq: int, nart: bool):
        self.key, self.frame, self.t_req, self.txfp, self.seq, self.nart = key, frame, t_req, txfp, seq, nart
        self.akey = frame.arbitration_key()


class _NodePort:
    """A firmware node's controller on this bus (see CanBus._attach_node)."""

    __slots__ = ("name", "on_rx", "on_tx_result", "mode_t", "mode_v", "bitrate")

    def __init__(self, name: str, on_rx: Callable, on_tx_result: Callable):
        self.name, self.on_rx, self.on_tx_result = name, on_rx, on_tx_result
        self.mode_t: list[int] = []
        self.mode_v: list[str] = []
        self.bitrate: int | None = None

    def mode_at(self, t: int) -> str:
        i = bisect.bisect_right(self.mode_t, t) - 1
        return self.mode_v[i] if i >= 0 else "sleep"  # bxCAN resets into sleep


class CanBus:
    """A CAN bus of a :class:`~stest.Sim`. Create with ``sim.can_bus(name, bitrate)``."""

    def __init__(self, sim: Sim, name: str, bitrate: int, index: int):
        self._sim = sim
        self.name = name
        self.bitrate = bitrate
        self._index = index
        self.ifs_ns = -(-IFS_BITS * 1_000_000_000 // bitrate)
        self.log: list[LoggedFrame] = []  # delivered frames in EOF order (appended at the boundary after EOF)
        self._order: dict[str, int] = {}  # sender name -> sender order (registration order)
        self._nodes: dict[str, _NodePort] = {}
        self._devices: dict[str, CanDevice] = {}
        self._pending: dict[str, list[_Req]] = {}
        self._npending = 0
        self._aborts: list[tuple[int, int, str, _Req]] = []  # (t, n, sender, request), sorted
        self._nabort = 0
        self._free_at = 0
        self._dseq = 0  # device/plain request counter (their key and seq)
        self._nframes = 0
        self._cursor = 0  # wait_for_frame consumed log[:cursor]

    # ---- public API ----
    def attach(self, device: CanDevice) -> CanDevice:
        """Attach a virtual device; it receives frames whose SOF is at or after now."""
        if not isinstance(device, CanDevice):
            raise TypeError(f"attach: expected a CanDevice (got {type(device).__name__})")
        if getattr(device, "bus", None) is not None:
            raise StestError(f"CAN device {device.name!r} is already attached to bus {device.bus.name!r}")
        self._register(device.name, "device")
        device.bus = self
        device._attach_t = self._sim._can_now()
        self._devices[device.name] = device
        return device

    def send(self, frame: CanFrame, sender: str = "python") -> None:
        """Request ``frame`` at the current virtual time from ``sender`` (an attached device's
        name sends as that device; any other name is a send-only sender that never ACKs)."""
        if sender in self._nodes:
            raise StestError(f"bus {self.name!r}: {sender!r} is a firmware node; it cannot send from Python")
        if sender not in self._devices and sender not in self._order:
            self._register(sender, "plain")
        self._device_send(sender, frame)

    def wait_for_frame(
        self,
        *,
        id: int | None = None,
        timeout_ms: float,
        match: Callable[[CanFrame], bool] | None = None,
    ) -> LoggedFrame:
        """Advance until a delivered frame not consumed by an earlier ``wait_for_frame``
        matches ``id`` and ``match``; returns it and consumes frames up to it. Raises
        WaitTimeout after ``timeout_ms`` of virtual time."""
        found: list[LoggedFrame] = []

        def cond() -> bool:
            log = self.log
            for i in range(self._cursor, len(log)):
                lf = log[i]
                if (id is None or lf.frame.id == id) and (match is None or match(lf.frame)):
                    self._cursor = i + 1
                    found.append(lf)
                    return True
            return False

        what = f"CAN frame{'' if id is None else f' id {id:#x}'}{' matching predicate' if match else ''} on bus {self.name!r}"
        self._sim.wait_for(cond, timeout_ms, what=what)
        return found[0]

    def export_candump(self, path) -> None:
        with open(path, "w") as f:
            f.write(format_candump(self.log, self.name))

    def export_asc(self, path) -> None:
        """Vector ASC; the channel number is the bus's creation index + 1."""
        with open(path, "w") as f:
            f.write(format_asc(self.log, self._index + 1))

    def __repr__(self) -> str:
        return f"<CanBus {self.name!r} {self.bitrate} bit/s, {len(self.log)} frames>"

    # ---- node-facing API (used by the Sim's firmware-node wiring) ----
    def _attach_node(
        self,
        sender: str,
        on_rx: Callable[[LoggedFrame, int], None],
        on_tx_result: Callable[[Any, dict, int], None],
    ) -> None:
        """Register firmware-node controller ``sender``. The engine calls
        ``on_rx(lf, t)`` for every frame this node did not win (t = EOF) and
        ``on_tx_result(key, {"ok", "arb_lost", "error"}, t)`` for each attempt of one of its
        requests (t = EOF; error is None or "ack"). Every t is > the quantum end at which
        the engine runs, so callbacks may Node.post at t. The node starts in mode "sleep"."""
        self._register(sender, "node")
        self._nodes[sender] = _NodePort(sender, on_rx, on_tx_result)

    def _request(
        self, sender: str, key: Any, frame: CanFrame, t_req: int, txfp: int, seq: int, *, nart: bool = False
    ) -> None:
        """Node ``sender`` requests ``frame`` from mailbox ``key`` (an int) at ``t_req``.
        ``txfp`` 0 = the node's mailboxes contend by identifier (then lowest key), 1 = by
        request order (lowest ``seq``); the latest request's txfp applies. ``nart``: one
        attempt only (dropped after a lost arbitration or missing ACK). Requests are only
        made while the node is in normal mode (the engine does not gate on mode)."""
        port = self._port(sender)
        if not isinstance(frame, CanFrame):
            raise TypeError(f"_request: expected a CanFrame (got {type(frame).__name__})")
        if txfp not in (0, 1):
            raise ValueError(f"_request: txfp must be 0 or 1 (got {txfp!r})")
        reqs = self._pending.setdefault(port.name, [])
        if any(r.key == key for r in reqs):
            raise StestError(f"bus {self.name!r}: {sender!r} requested mailbox {key!r} while it is still pending")
        reqs.append(_Req(key, frame, int(t_req), txfp, int(seq), bool(nart)))
        self._npending += 1

    def _abort(self, sender: str, key: Any, t: int) -> None:
        """Abort ``sender``'s pending request ``key`` at ``t`` (calls arrive in the node's
        event order, so this is the request made before the abort; with none pending it has
        no effect, as ABRQ on an empty mailbox). Effective (the request is withdrawn, with no
        result) only if its next attempt has not started by ``t`` (t <= its SOF); otherwise
        the attempt in progress completes and reports its result as usual."""
        self._port(sender)
        req = next((r for r in self._pending.get(sender, ()) if r.key == key), None)
        if req is None:
            return
        self._nabort += 1
        bisect.insort(self._aborts, (int(t), self._nabort, sender, req), key=lambda a: a[:2])

    def _set_node_mode(self, sender: str, mode: str, bitrate: int | None, t: int) -> None:
        """Node ``sender``'s controller entered ``mode`` (sleep|init|normal) at ``t``; it
        acknowledges frames whose SOF is at or after a change to normal and before a change
        away from it. ``bitrate`` is the node's configured bit rate (when entering normal)."""
        if mode not in MODES:
            raise ValueError(f"_set_node_mode: mode must be one of {', '.join(MODES)} (got {mode!r})")
        port = self._port(sender)
        i = bisect.bisect_right(port.mode_t, int(t))
        port.mode_t.insert(i, int(t))
        port.mode_v.insert(i, mode)
        if bitrate is not None:
            port.bitrate = int(bitrate)

    # ---- internals ----
    def _register(self, name: str, kind: str) -> None:
        if not name or not isinstance(name, str):
            raise ValueError("sender name must be a non-empty string")
        if name in self._order:
            raise StestError(f"bus {self.name!r} already has a sender named {name!r}")
        self._order[name] = len(self._order)

    def _port(self, sender: str) -> _NodePort:
        port = self._nodes.get(sender)
        if port is None:
            raise StestError(f"bus {self.name!r}: {sender!r} is not an attached node")
        return port

    def _device_send(self, sender: str, frame: CanFrame) -> None:
        if not isinstance(frame, CanFrame):
            raise TypeError(f"send: expected a CanFrame (got {type(frame).__name__})")
        self._dseq += 1
        req = _Req(self._dseq, frame, self._sim._can_now(), 0, self._dseq, False)
        self._pending.setdefault(sender, []).append(req)
        self._npending += 1

    def _remove(self, sender: str, req: _Req) -> None:
        reqs = self._pending[sender]
        reqs.remove(req)
        if not reqs:
            del self._pending[sender]
        self._npending -= 1

    def _apply_aborts(self, limit: int) -> None:
        aborts = self._aborts
        while aborts and aborts[0][0] <= limit:
            _, _, sender, req = aborts.pop(0)
            if any(r is req for r in self._pending.get(sender, ())):
                self._remove(sender, req)

    def _run(self, T: int) -> None:
        """Decide every bus-idle instant before quantum end ``T`` (see the module doc)."""
        pending = self._pending
        while self._npending:
            t_idle = max(self._free_at, min(r.t_req for reqs in pending.values() for r in reqs))
            if t_idle >= T:
                break
            if self._aborts and self._aborts[0][0] <= t_idle:
                self._apply_aborts(t_idle)
                continue
            self._decide(t_idle, T)
        if self._aborts:
            self._apply_aborts(T)  # any frame not started yet starts at >= T

    def _decide(self, t_idle: int, T: int) -> None:
        tops = []  # (arbitration key, sender order, sender, request)
        for sender, reqs in self._pending.items():
            ready = [r for r in reqs if r.t_req <= t_idle]
            if not ready:
                continue
            # the TXFP in force at SOF: that of the latest request made by then (never a
            # later-stamped one the engine happens to know already: that would depend on T)
            if max(ready, key=lambda r: r.seq).txfp == 1:
                top = min(ready, key=lambda r: (r.seq, r.key))
            else:
                top = min(ready, key=lambda r: (r.akey, r.key))
            tops.append((top.akey, self._order[sender], sender, top))
        tops.sort(key=lambda c: c[:2])
        frame = tops[0][3].frame
        winners = [c for c in tops if c[3].frame == frame]
        losers = [c for c in tops if c[3].frame != frame]
        eof = t_idle + _frame_ns(frame, self.bitrate)
        if eof <= T:
            raise StestError(
                f"CAN bus {self.name!r}: engine bug: frame {frame} decided at idle t={t_idle} ns ends at "
                f"{eof} ns, not after the quantum end {T} ns, so its results would be posted into the past "
                f"(quantum {self._sim.quantum_ns} ns, lookahead {self._sim._can_lookahead_ns()} ns)"
            )
        won = {c[2] for c in winners}
        ack = any(p.mode_at(t_idle) == "normal" for n, p in self._nodes.items() if n not in won) or any(
            d.ack and d._attach_t <= t_idle for n, d in self._devices.items() if n not in won
        )
        lost = {c[2]: c[3] for c in losers}
        mine = {c[2]: c[3] for c in winners}
        for c in winners:
            if ack or c[3].nart:
                self._remove(c[2], c[3])
        for c in losers:
            if c[3].nart:
                self._remove(c[2], c[3])
        self._free_at = eof + self.ifs_ns
        lf = LoggedFrame(eof, self.name, frame, winners[0][2]) if ack else None
        for name, port in self._nodes.items():
            if name in mine:
                port.on_tx_result(mine[name].key, _ok() if ack else _no_ack(), eof)
            elif name in lost:
                port.on_tx_result(lost[name].key, _arb_lost(), eof)
            if lf is not None and name not in won:
                port.on_rx(lf, eof)
        if lf is not None:
            self._nframes += 1
            self._sim._can_schedule(eof, 0, self._index, self._nframes, lambda: self._deliver(lf, won, t_idle))

    def _deliver(self, lf: LoggedFrame, won: set[str], sof: int) -> None:
        """At EOF (run at the first quantum end >= EOF): log the frame, call devices."""
        self.log.append(lf)
        for name, d in list(self._devices.items()):
            if name not in won and d._attach_t <= sof:
                d.on_frame(lf)
