"""SPI1 master (selftest firmware `spi`: HAL_SPI_TransmitReceive, `spi_tx`: HAL_SPI_Transmit):
burst capture, MISO queue, CS correlation through the GPIO history."""

from __future__ import annotations

import json

import pytest

from stest import Sim
from stest.errors import StestError
from stest.peripherals import SpiBurst

PAYLOAD = bytes([0xA5, 0x01, 0x02, 0x03])


def cs_low_windows(history: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """(t_fall, t_rise) of each low period of a pin, from Pin.history."""
    out, start = [], None
    for t, level in history:
        if level == 0 and start is None:
            start = t
        elif level == 1 and start is not None:
            out.append((start, t))
            start = None
    return out


def in_window(b: SpiBurst, w: tuple[int, int]) -> bool:
    return w[0] <= b.t_start_ns and b.t_end_ns <= w[1]


def test_spi_burst_captured(spi_exe):
    with Sim() as s:
        n = s.node("n", spi_exe())
        s.start()
        s.advance(ms=15)
        assert n.var("error_count") == 0 and n.var("xfer_count") == 1
        bus = n.spi("SPI1")
        windows = cs_low_windows(n.pin("PA4").history)
        assert len(windows) == 1
        # HAL_SPI_TransmitReceive alternates TX and RX (2 frames, then wait for RXNE), so BSY
        # falls between the pairs: two bursts, both inside the chip-select window.
        inside = [b for b in bus.bursts if in_window(b, windows[0])]
        assert inside == bus.bursts
        assert b"".join(b.mosi for b in inside) == PAYLOAD
        assert [b.mosi for b in inside] == [PAYLOAD[:2], PAYLOAD[2:]]
        assert bus.bursts[0].miso == b"\xff\xff"
        # 8 bits at 80 MHz / 16 -> 1600 ns per frame, back to back within a burst
        assert all(b.t_end_ns - b.t_start_ns == 1600 * len(b.mosi) for b in inside)


def test_spi_transmit_is_one_burst(spi_exe):
    with Sim() as s:
        n = s.node("n", spi_exe("tx"))
        s.start()
        s.advance(ms=25)
        assert n.var("error_count") == 0 and n.var("xfer_count") == 2
        bursts = n.spi("SPI1").bursts
        windows = cs_low_windows(n.pin("PA4").history)
        assert len(bursts) == 2 and len(windows) == 2
        for b, w in zip(bursts, windows):
            assert b.mosi == PAYLOAD and b.miso == b"\xff" * 4
            assert in_window(b, w)
            assert b.t_end_ns - b.t_start_ns == 4 * 1600
        # the event log carries the same burst as sorted-key JSON
        ev = [json.loads(js) for _, _, js in s.event_log if json.loads(js)["kind"] == "spi"]
        assert ev[0] == {"kind": "spi", "bus": "SPI1", "t_start": bursts[0].t_start_ns,
                         "mosi": "a5010203", "miso": "ffffffff"}


def test_spi_miso_queue(spi_exe):
    with Sim() as s:
        n = s.node("n", spi_exe())
        s.start()
        n.spi("SPI1").queue_miso(b"\x10\x20\x30\x40")
        s.advance(ms=15)
        assert n.var("rx") == [0x10, 0x20, 0x30, 0x40]
        assert b"".join(b.miso for b in n.spi("SPI1").bursts) == b"\x10\x20\x30\x40"
        assert n.spi("SPI1").state()["miso_queued"] == 0


def test_spi_miso_default_ff(spi_exe):
    with Sim() as s:
        n = s.node("n", spi_exe())
        s.start()
        n.spi("SPI1").queue_miso(b"\x55")  # one byte queued: the rest of the transfer reads 0xFF
        s.advance(ms=25)
        assert n.var("xfer_count") == 2
        assert n.var("rx") == [0xFF] * 4  # second transfer: queue empty
        assert b"".join(b.miso for b in n.spi("SPI1").bursts) == b"\x55" + b"\xff" * 7


def test_spi_wait_for_bytes(spi_exe):
    with Sim() as s:
        n = s.node("n", spi_exe("tx"))
        s.start()
        bus = n.spi("SPI1")
        bus.wait_for_bytes(8, timeout_ms=50)
        assert sum(len(b.mosi) for b in bus.bursts) >= 8
        assert 20 <= s.now_ns / 1e6 <= 21
        with pytest.raises(StestError, match="SPI1"):
            bus.wait_for_bytes(1000, timeout_ms=5)


def test_spi_helper_errors(spi_exe):
    with Sim() as s:
        n = s.node("n", spi_exe())
        s.start()
        assert n.spi("SPI2").bursts == []
        with pytest.raises(StestError, match="unknown model"):
            n.spi("SPI9").queue_miso(b"\x00")
        with pytest.raises(TypeError):
            n.spi("SPI1").queue_miso("10")  # type: ignore[arg-type]


def test_spi_is_deterministic(spi_exe):
    def run(quantum_us: int):
        with Sim(quantum_us=quantum_us) as s:
            n = s.node("n", spi_exe())
            s.start()
            n.spi("SPI1").queue_miso(bytes(range(16)))
            s.advance(ms=45)
            return list(s.event_log), n.var("rx")

    a = run(1000)
    assert a == run(1000)
    assert a == run(250)
