"""Simulated CAN bus: arbitration, ACK, queuing and delivery timing on the global clock.

Engine (run by the Sim at every quantum end T, after the node events up to T are known
and device callbacks due by T have run):

    while requests are pending:
        t_idle = max(bus_free_at, earliest t_req)       # bus free = previous EOF + IFS
        if t_idle >= T: break                           # later requests (stamped <= that instant) may still join
        aborts with t_abort <= t_idle withdraw their request (no attempt on the wire) and report
            "aborted" at max(t_abort + lookahead, EOF of its previous attempt)
        contenders = each sender's top request with t_req <= t_idle
                     (node TXFP=0: lowest arbitration key, then lowest key; TXFP=1: lowest seq;
                      devices: lowest arbitration key, then request order)
        winner = min(arbitration key, sender order); bit-identical frames from several senders all win
        eof = t_idle + duration; bus_free_at = eof + IFS
        ACK = a non-winning node whose mode at SOF (t_idle) is "normal", or a non-winning device with ack=True
        ACK:    winners: tx_result ok at eof (request done); non-winning nodes in normal mode at SOF: rx at eof;
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
import heapq
from typing import TYPE_CHECKING, Any, Callable

from ..errors import StestError
from .device import CanDevice
from .frame import IFS_BITS, CanFrame, lookahead_ns
from .log import LoggedFrame, format_asc, format_candump

if TYPE_CHECKING:
    from ..sim import Sim
    from .dbc import Dbc

MODES = ("sleep", "init", "normal")


# tx_result payloads (fresh dicts per call: receivers may keep or extend them); "seq" is the
# request's seq, so a late result for an aborted request is never taken for its successor's
def _ok(req: _Req) -> dict:
    return {"ok": True, "arb_lost": False, "error": None, "seq": req.seq}


def _arb_lost(req: _Req) -> dict:
    return {"ok": False, "arb_lost": True, "error": None, "seq": req.seq}


def _no_ack(req: _Req) -> dict:
    return {"ok": False, "arb_lost": False, "error": "ack", "seq": req.seq}


def _aborted(req: _Req) -> dict:
    return {"ok": False, "arb_lost": False, "error": None, "aborted": True, "seq": req.seq}


@functools.lru_cache(maxsize=4096)
def _frame_ns(frame: CanFrame, bitrate: int) -> int:
    return frame.duration_ns(bitrate)


class _Req:
    __slots__ = ("key", "frame", "t_req", "txfp", "seq", "nart", "akey", "abort_t", "last_eof")

    def __init__(self, key: Any, frame: CanFrame, t_req: int, txfp: int, seq: int, nart: bool):
        self.key, self.frame, self.t_req, self.txfp, self.seq, self.nart = key, frame, t_req, txfp, seq, nart
        self.akey = frame.arbitration_key()
        self.abort_t: int | None = None  # time of the abort bound to this request
        self.last_eof = 0  # EOF of its latest attempt


class _DevQueue:
    """A device's / plain sender's requests: a heap by request time, and the ready ones
    (t_req <= the latest idle instant) in a heap by (arbitration key, request order), so a
    decision is O(log n) however long the backlog. Only the ready top is ever removed."""

    __slots__ = ("waiting", "ready", "n")

    def __init__(self) -> None:
        self.waiting: list[tuple] = []
        self.ready: list[tuple] = []
        self.n = 0

    def add(self, req: _Req) -> None:
        heapq.heappush(self.waiting, (req.t_req, req.seq, req))
        self.n += 1

    def earliest(self) -> int:
        # a ready request's t_req is <= the previous idle instant <= bus_free_at, which is
        # all t_idle = max(bus_free_at, earliest) needs
        return self.ready[0][2].t_req if self.ready else self.waiting[0][0]

    def top(self, t_idle: int) -> _Req | None:
        w = self.waiting
        while w and w[0][0] <= t_idle:
            r = heapq.heappop(w)[2]
            heapq.heappush(self.ready, (r.akey, r.key, r))
        return self.ready[0][2] if self.ready else None

    def remove(self, req: _Req) -> None:
        assert self.ready and self.ready[0][2] is req
        heapq.heappop(self.ready)
        self.n -= 1


class _NodeQueue:
    """A node's pending mailbox requests (a handful): scanned per decision."""

    __slots__ = ("reqs",)

    def __init__(self) -> None:
        self.reqs: list[_Req] = []

    @property
    def n(self) -> int:
        return len(self.reqs)

    def add(self, req: _Req) -> None:
        self.reqs.append(req)

    def earliest(self) -> int:
        return min(r.t_req for r in self.reqs)

    def top(self, t_idle: int) -> _Req | None:
        ready = [r for r in self.reqs if r.t_req <= t_idle]
        if not ready:
            return None
        # the TXFP in force at SOF: that of the latest request made by then (never a
        # later-stamped one the engine happens to know already: that would depend on T)
        if max(ready, key=lambda r: r.seq).txfp == 1:
            return min(ready, key=lambda r: (r.seq, r.key))
        return min(ready, key=lambda r: (r.akey, r.key))

    def remove(self, req: _Req) -> None:
        self.reqs.remove(req)


class _NodePort:
    """A firmware node's controller on this bus (see CanBus._attach_node)."""

    __slots__ = ("name", "on_rx", "on_tx_result", "mode_t", "mode_v", "bitrate", "q")

    def __init__(self, name: str, on_rx: Callable, on_tx_result: Callable):
        self.name, self.on_rx, self.on_tx_result = name, on_rx, on_tx_result
        self.mode_t: list[int] = []
        self.mode_v: list[str] = []
        self.bitrate: int | None = None
        self.q = _NodeQueue()

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
        self._queues: dict[str, _DevQueue | _NodeQueue] = {}  # sender -> its pending requests
        self._npending = 0
        self.lookahead_ns = lookahead_ns(bitrate)
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
        device._ack = bool(device.ack)  # fixed from attach on (CanDevice refuses later changes)
        self._devices[device.name] = device
        self._queues[device.name] = _DevQueue()
        return device

    def send(self, frame: CanFrame, sender: str = "python") -> None:
        """Request ``frame`` at the current virtual time from ``sender`` (an attached device's
        name sends as that device; any other name is a send-only sender that never ACKs)."""
        if sender in self._nodes:
            raise StestError(f"bus {self.name!r}: {sender!r} is a firmware node; it cannot send from Python")
        if sender not in self._devices and sender not in self._order:
            self._register(sender, "plain")
            self._queues[sender] = _DevQueue()
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

    def messages(self, dbc: Dbc) -> list[tuple[int, str, dict]]:
        """The delivered frames decoded with ``dbc``: ``(t_ns, message name, signals)`` in EOF
        order; frames the DBC does not know are skipped."""
        out = []
        for lf in self.log:
            d = dbc.decode(lf.frame)
            if d is not None:
                out.append((lf.t_ns, d[0], d[1]))
        return out

    def wait_for_message(
        self,
        dbc: Dbc,
        message: str,
        *,
        timeout_ms: float,
        where: Callable[[dict], bool] | None = None,
    ) -> tuple[int, dict]:
        """Like :meth:`wait_for_frame` (same consumption cursor) for the DBC message
        ``message`` whose decoded signals satisfy ``where``; returns ``(t_ns, signals)``.
        Raises WaitTimeout after ``timeout_ms`` of virtual time."""
        msg = dbc.message(message)
        decoded: list[dict] = []

        def match(frame: CanFrame) -> bool:
            if frame.id != msg.frame_id or frame.ext != msg.is_extended_frame or frame.rtr:
                return False
            d = dbc.decode(frame)
            if d is None or (where is not None and not where(d[1])):
                return False
            decoded.append(d[1])
            return True

        lf = self.wait_for_frame(timeout_ms=timeout_ms, match=match)
        return lf.t_ns, decoded[-1]

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
        """Register firmware-node controller ``sender`` (it starts in mode "sleep"). The
        engine calls, with every t >= the quantum end at which it runs (so callbacks may
        ``Node.post`` at t):

        - ``on_rx(lf, t)``: a frame this node did not send, whose SOF found it in mode
          normal; t = EOF (after any arb_lost result for the same frame).
        - ``on_tx_result(key, result, t)`` for each attempt of a request, t = EOF:
          ``{"ok": bool, "arb_lost": bool, "error": None | "ack", "seq": int}``;
          and once for an effective abort: ``{"ok": False, "arb_lost": False, "error": None,
          "aborted": True, "seq": int}`` (see ``_abort``). ``seq`` is the request's."""
        self._register(sender, "node")
        port = self._nodes[sender] = _NodePort(sender, on_rx, on_tx_result)
        self._queues[sender] = port.q

    def _request(
        self, sender: str, key: Any, frame: CanFrame, t_req: int, txfp: int, seq: int, *, nart: bool = False
    ) -> None:
        """Node ``sender`` requests ``frame`` from mailbox ``key`` (an int) at ``t_req``.
        ``txfp`` 0 = the node's mailboxes contend by identifier (then lowest key), 1 = by
        request order (lowest ``seq``); the latest request's txfp applies. ``nart``: one
        attempt only (dropped after a lost arbitration or missing ACK). Requests are only
        made while the node is in normal mode (the engine does not gate on mode). A mailbox
        may be reloaded once its pending request has an abort at or before ``t_req``."""
        port = self._port(sender)
        if not isinstance(frame, CanFrame):
            raise TypeError(f"_request: expected a CanFrame (got {type(frame).__name__})")
        if txfp not in (0, 1):
            raise ValueError(f"_request: txfp must be 0 or 1 (got {txfp!r})")
        t_req = int(t_req)
        if any(r.key == key and (r.abort_t is None or r.abort_t > t_req) for r in port.q.reqs):
            raise StestError(f"bus {self.name!r}: {sender!r} requested mailbox {key!r} while it is still pending")
        port.q.add(_Req(key, frame, t_req, txfp, int(seq), bool(nart)))
        self._npending += 1

    def _abort(self, sender: str, key: Any, t: int) -> None:
        """Abort ``sender``'s pending request ``key`` at ``t``. Calls arrive in the node's
        event order, so this binds to the request made before the abort (with none pending,
        or only an already-aborted one, it has no effect, like ABRQ on an empty mailbox).

        If no attempt of the request is on the wire at ``t`` (t <= the next SOF), the abort
        takes effect: the request is withdrawn and ``on_tx_result(key, {..., "aborted": True,
        "seq": seq}, max(t + lookahead_ns, EOF of its previous attempt))`` is posted.
        Hardware acknowledges the abort at once; it is reported up to 44 bit times late
        (the lookahead) because only then is the decision causal and quantum-independent.
        An attempt already on the wire at ``t`` is not affected: its result arrives at EOF as
        usual (and if it failed, the abort takes effect before the retry)."""
        port = self._port(sender)
        req = next((r for r in port.q.reqs if r.key == key and r.abort_t is None), None)
        if req is None:
            return
        req.abort_t = int(t)
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
        self._queues[sender].add(_Req(self._dseq, frame, self._sim._can_now(), 0, self._dseq, False))
        self._npending += 1

    def _apply_aborts(self, limit: int, T: int) -> None:
        aborts = self._aborts
        while aborts and aborts[0][0] <= limit:
            t_abort, _, sender, req = aborts.pop(0)
            port = self._nodes[sender]
            if not any(r is req for r in port.q.reqs):
                continue  # completed (or dropped) before the abort: no effect
            port.q.remove(req)
            self._npending -= 1
            t_post = max(t_abort + self.lookahead_ns, req.last_eof)
            if t_post < T:
                raise StestError(
                    f"CAN bus {self.name!r}: engine bug: abort of {sender!r} mailbox {req.key!r} at t={t_abort} ns "
                    f"would be reported at {t_post} ns, before the quantum end {T} ns (into the past)"
                )
            port.on_tx_result(req.key, _aborted(req), t_post)

    def _run(self, T: int) -> None:
        """Decide every bus-idle instant before quantum end ``T`` (see the module doc).
        Cost per decision: O(senders + log(backlog))."""
        queues = self._queues
        while self._npending:
            t_idle = max(self._free_at, min(q.earliest() for q in queues.values() if q.n))
            if t_idle >= T:
                break
            if self._aborts and self._aborts[0][0] <= t_idle:
                self._apply_aborts(t_idle, T)
                continue
            self._decide(t_idle, T)
        if self._aborts:
            self._apply_aborts(T, T)  # any request not on the wire now starts at >= T

    def _decide(self, t_idle: int, T: int) -> None:
        tops = []  # (arbitration key, sender order, sender, request, queue)
        for sender, q in self._queues.items():
            if q.n:
                top = q.top(t_idle)
                if top is not None:
                    tops.append((top.akey, self._order[sender], sender, top, q))
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
        listening = [n for n, p in self._nodes.items() if n not in won and p.mode_at(t_idle) == "normal"]
        ack = bool(listening) or any(
            d._ack and d._attach_t <= t_idle for n, d in self._devices.items() if n not in won
        )
        for c in tops:
            c[3].last_eof = eof
        for c in winners:
            if ack or c[3].nart:
                c[4].remove(c[3])
                self._npending -= 1
        for c in losers:
            if c[3].nart:
                c[4].remove(c[3])
                self._npending -= 1
        self._free_at = eof + self.ifs_ns
        lost = {c[2]: c[3] for c in losers}
        mine = {c[2]: c[3] for c in winners}
        lf = LoggedFrame(eof, self.name, frame, winners[0][2]) if ack else None
        rx = set(listening) if lf is not None else ()
        for name, port in self._nodes.items():
            if name in mine:
                port.on_tx_result(mine[name].key, _ok(mine[name]) if ack else _no_ack(mine[name]), eof)
            elif name in lost:
                port.on_tx_result(lost[name].key, _arb_lost(lost[name]), eof)
            if name in rx:
                port.on_rx(lf, eof)
        if lf is not None:
            self._nframes += 1
            self._sim._can_schedule(eof, 0, self._index, self._nframes, lambda: self._deliver(lf, won, t_idle))

    def _deliver(self, lf: LoggedFrame, won: set[str], sof: int) -> None:
        """At EOF (run at the first quantum end >= EOF): log the frame, call devices. A
        raising on_frame ends the simulation (Sim._device_failed)."""
        self.log.append(lf)
        for name, d in list(self._devices.items()):
            if name not in won and d._attach_t <= sof:
                try:
                    d.on_frame(lf)
                except Exception as e:
                    self._sim._device_failed(d.name, "on_frame", e)
