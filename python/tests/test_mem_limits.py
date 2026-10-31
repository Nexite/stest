"""READ_MEM / WRITE_MEM size limits: a too-large request is a request error, never a node abort."""

from __future__ import annotations

import pytest

from stest import Sim, StestError
from stest import protocol as proto


@pytest.fixture
def blinky_sim(blinky_exe):
    s = Sim()
    n = s.node("b", blinky_exe)
    s.start()
    s.advance(ms=1)
    yield s, n
    s.close()


def _still_usable(s, n):
    s.advance(ms=600)
    assert n.pin("PA5").level in (0, 1)
    assert n.var("uwTick") > 0


def test_oversized_read_mem_raises_before_sending(blinky_sim):
    s, n = blinky_sim
    with pytest.raises(StestError, match=r"read_mem of .* bytes exceeds the protocol maximum"):
        n.read_mem(n.anchor_addr, proto.MAX_PAYLOAD + 1)
    _still_usable(s, n)


def test_oversized_write_mem_raises_before_sending(blinky_sim):
    s, n = blinky_sim
    with pytest.raises(StestError, match=r"write_mem of .* bytes exceeds the protocol maximum"):
        n.write_mem(n.anchor_addr, bytes(proto.MAX_WRITE_MEM + 1))
    _still_usable(s, n)


def test_runtime_rejects_oversized_read_mem(blinky_sim):
    s, n = blinky_sim
    # Bypass Node.read_mem's pre-check: the runtime itself must answer REQ_ERROR.
    with pytest.raises(StestError, match=r"read_mem: .* bytes exceeds"):
        n._request(proto.read_mem(n.anchor_addr, proto.MAX_PAYLOAD + 1), proto.Msg.MEM, "READ_MEM")
    _still_usable(s, n)


def test_runtime_discards_oversized_frame(blinky_sim):
    s, n = blinky_sim
    # A WRITE_MEM frame whose payload exceeds the maximum: the runtime must read and
    # discard the payload, answer REQ_ERROR and keep serving.
    size = proto.MAX_PAYLOAD + 1
    body = proto._MEM_REQ.pack(n.anchor_addr, size - proto._MEM_REQ.size) + bytes(size - proto._MEM_REQ.size)
    frame = proto._HDR.pack(len(body), proto.Msg.WRITE_MEM) + body
    with pytest.raises(StestError, match=r"payload of \d+ bytes exceeds"):
        n._request(frame, proto.Msg.OK, "WRITE_MEM")
    _still_usable(s, n)
