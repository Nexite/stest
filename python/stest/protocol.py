"""stest control protocol v2 (mirror of runtime/include/stest/protocol.h).

Framing, little-endian: ``u32 len | u8 type | payload[len]`` (len = payload bytes).
v2: RUN_UNTIL carries an inbox of timestamped model messages (``Node.post``); ERROR carries
the aborting quantum's events after its message.
"""

from __future__ import annotations

import json
import socket
import struct
import time
from enum import IntEnum
from collections.abc import Iterable
from typing import Any

PROTO_VERSION = 2
MAX_PAYLOAD = 64 * 1024 * 1024  # STEST_MAX_PAYLOAD: largest frame payload either side accepts

_HDR = struct.Struct("<IB")
_U64 = struct.Struct("<Q")
_HELLO = struct.Struct("<IQ")
_RUN_UNTIL_HDR = struct.Struct("<QI")
_U32 = struct.Struct("<I")
_EVENT_HDR = struct.Struct("<QH")
_MEM_REQ = struct.Struct("<QI")
_U16 = struct.Struct("<H")

MAX_READ_MEM = MAX_PAYLOAD  # the MEM reply carries exactly the bytes read
MAX_WRITE_MEM = MAX_PAYLOAD - _MEM_REQ.size  # WRITE_MEM payload = addr, len, data


class Msg(IntEnum):
    HELLO = 1
    RUN_UNTIL = 2
    STOPPED = 3
    READ_MEM = 4
    MEM = 5
    WRITE_MEM = 6
    OK = 7
    MODEL_CALL = 8
    MODEL_RESULT = 9
    ERROR = 10
    SHUTDOWN = 11
    REQ_ERROR = 12


class ProtocolError(Exception):
    """Malformed message from a node."""


class ChannelClosed(Exception):
    """EOF or a broken connection: the node is gone."""


# ---- encoders (py -> node) ----
def pack(mtype: Msg, payload: bytes = b"") -> bytes:
    return _HDR.pack(len(payload), mtype) + payload


def run_until(t_ns: int, inbox: Iterable[tuple[int, bytes]] = ()) -> bytes:
    """RUN_UNTIL t_ns with inbox messages ``(t_ns, json bytes from inbox_message())``,
    applied by the node in this order before it runs."""
    if not inbox:  # the per-quantum hot path
        return _HDR.pack(_RUN_UNTIL_HDR.size, Msg.RUN_UNTIL) + _RUN_UNTIL_HDR.pack(t_ns, 0)
    parts = [b""]
    for t, js in inbox:
        parts.append(_EVENT_HDR.pack(t, len(js)))
        parts.append(js)
    parts[0] = _RUN_UNTIL_HDR.pack(t_ns, (len(parts) - 1) // 2)
    return pack(Msg.RUN_UNTIL, b"".join(parts))


def inbox_message(model: str, op: str, args: dict[str, Any]) -> bytes:
    """JSON of one RUN_UNTIL inbox message; ValueError if over the u16 length limit."""
    js = json.dumps({"model": model, "op": op, "args": args}, separators=(",", ":")).encode()
    if len(js) > 0xFFFF:
        raise ValueError(f"inbox message too long ({len(js)} bytes of JSON, maximum 65535)")
    return js


def read_mem(addr: int, n: int) -> bytes:
    return pack(Msg.READ_MEM, _MEM_REQ.pack(addr, n))


def write_mem(addr: int, data: bytes) -> bytes:
    return pack(Msg.WRITE_MEM, _MEM_REQ.pack(addr, len(data)) + bytes(data))


def model_call(model: str, op: str, args: dict[str, Any]) -> bytes:
    js = json.dumps({"model": model, "op": op, "args": args}, separators=(",", ":")).encode()
    if len(js) > 0xFFFF:
        raise ValueError("MODEL_CALL request too long")
    return pack(Msg.MODEL_CALL, _U16.pack(len(js)) + js)


def shutdown() -> bytes:
    return pack(Msg.SHUTDOWN)


# ---- decoders (node -> py) ----
def parse_hello(p: bytes) -> tuple[int, int]:
    """-> (proto_version, anchor_addr)"""
    if len(p) != _HELLO.size:
        raise ProtocolError(f"HELLO payload of {len(p)} bytes")
    return _HELLO.unpack(p)


def _parse_events(p: bytes, off: int, what: str) -> tuple[list[tuple[int, str]], int]:
    """``u32 n, n x (u64 t, u16 len, json)`` at ``off`` -> (events, end offset)."""
    (n,) = _U32.unpack_from(p, off)
    off += _U32.size
    events = []
    for _ in range(n):
        et, jl = _EVENT_HDR.unpack_from(p, off)
        off += _EVENT_HDR.size
        js = p[off : off + jl]
        if len(js) != jl:
            raise ProtocolError(f"truncated {what} event")
        off += jl
        events.append((et, js.decode("utf-8")))
    return events, off


def parse_stopped(p: bytes) -> tuple[int, list[tuple[int, str]]]:
    """-> (node t_ns, [(event t_ns, event json text), ...]) in emission order"""
    try:
        (t,) = _U64.unpack_from(p, 0)
        events, off = _parse_events(p, _U64.size, "STOPPED")
    except struct.error as e:
        raise ProtocolError(f"truncated STOPPED: {e}") from None
    if off != len(p):
        raise ProtocolError(f"{len(p) - off} trailing bytes in STOPPED")
    return t, events


def parse_error(p: bytes) -> tuple[str, list[tuple[int, str]]]:
    """-> (message "stest: ...", [(event t_ns, event json text), ...]): the events still
    pending when the node aborted, then its unterminated output lines ("partial": true)."""
    try:
        (n,) = _U16.unpack_from(p, 0)
        msg = p[_U16.size : _U16.size + n]
        if len(msg) != n:
            raise ProtocolError("truncated ERROR message")
        events, off = _parse_events(p, _U16.size + n, "ERROR")
    except struct.error as e:
        raise ProtocolError(f"truncated ERROR: {e}") from None
    if off != len(p):
        raise ProtocolError(f"{len(p) - off} trailing bytes in ERROR")
    return msg.decode("utf-8", "replace"), events


class Channel:
    """Blocking framed socket with per-call wall-clock deadlines."""

    def __init__(self, sock: socket.socket):
        self.sock = sock

    def send(self, data: bytes) -> None:
        try:
            self.sock.settimeout(None)
            self.sock.sendall(data)
        except OSError as e:  # EPIPE / ECONNRESET: the node is gone
            raise ChannelClosed(str(e)) from None

    def _recv_exact(self, n: int, deadline: float) -> bytes:
        buf = bytearray(n)
        view = memoryview(buf)
        got = 0
        while got < n:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError
            self.sock.settimeout(remaining)
            try:
                k = self.sock.recv_into(view[got:], n - got)
            except TimeoutError:
                raise
            except OSError as e:
                raise ChannelClosed(str(e)) from None
            if k == 0:
                raise ChannelClosed("EOF")
            got += k
        return bytes(buf)

    def recv(self, deadline: float) -> tuple[int, bytes]:
        """-> (type, payload). Raises TimeoutError past ``deadline`` (time.monotonic),
        ChannelClosed on EOF, ProtocolError on a bad header."""
        n, mtype = _HDR.unpack(self._recv_exact(_HDR.size, deadline))
        if n > MAX_PAYLOAD:
            raise ProtocolError(f"payload of {n} bytes")
        return mtype, (self._recv_exact(n, deadline) if n else b"")

    def close(self) -> None:
        self.sock.close()
