"""CAN frame model: bit length / CRC cross-checked against a naive reference."""

import pytest

from stest.can.frame import IFS_BITS, MIN_FRAME_BITS, CanFrame, crc15, lookahead_ns
from stest.can.log import ASC_DATE, LoggedFrame, format_asc, format_candump


# ---- independent reference (deliberately naive, string based) ----------------
def _bits(value, width):
    return format(value, "0%db" % width)


def ref_unstuffed_head(f):
    """SOF..end of data as a '0'/'1' string."""
    dlc = len(f.data) if f.dlc is None else f.dlc
    s = "0"
    if f.ext:
        s += _bits(f.id >> 18, 11) + "11" + _bits(f.id & 0x3FFFF, 18)
        s += ("1" if f.rtr else "0") + "00"
    else:
        s += _bits(f.id, 11) + ("1" if f.rtr else "0") + "00"
    s += _bits(dlc, 4)
    for b in f.data:
        s += _bits(b, 8)
    return s


def ref_crc(s):
    reg = 0
    for ch in s:
        top = (reg >> 14) & 1
        reg = (reg << 1) & 0x7FFF
        if top != int(ch):
            reg ^= 0x4599
    return reg


def ref_stuff(s):
    out = ""
    run_ch, run_len = None, 0
    for ch in s:
        out += ch
        if ch == run_ch:
            run_len += 1
        else:
            run_ch, run_len = ch, 1
        if run_len == 5:
            nxt = "1" if ch == "0" else "0"
            out += nxt
            run_ch, run_len = nxt, 1
    return out


def ref_bit_length(f):
    head = ref_unstuffed_head(f)
    body = head + _bits(ref_crc(head), 15)
    return len(ref_stuff(body)) + 1 + 1 + 1 + 7  # CRC del, ACK, ACK del, EOF


FRAMES = [
    CanFrame(0x000),
    CanFrame(0x7FF),
    CanFrame(0x123, b"\x00" * 8),
    CanFrame(0x7FF, b"\xff" * 8),
    CanFrame(0x555, b"\xaa\x55"),
    CanFrame(0x001, b"\x01\x02\x03"),
    CanFrame(0x100, rtr=True),
    CanFrame(0x100, rtr=True, dlc=8),
    CanFrame(0x000, b"\x00", ext=True),
    CanFrame(0x1FFFFFFF, b"\xff" * 8, ext=True),
    CanFrame(0x1FFFFFFF, ext=True),
    CanFrame(0x0, ext=True, rtr=True, dlc=3),
    CanFrame(0x18DAF110, b"\x02\x10\x01", ext=True),
    CanFrame(0x7E0, b"\x0f\xf0\x0f\xf0\x0f"),
]


@pytest.mark.parametrize("f", FRAMES)
def test_bit_length_matches_reference(f):
    assert f.bit_length() == ref_bit_length(f)


@pytest.mark.parametrize("f", FRAMES)
def test_crc_matches_reference(f):
    head = ref_unstuffed_head(f)
    assert crc15([int(c) for c in head]) == ref_crc(head)


def test_known_sizes():
    assert MIN_FRAME_BITS == 44 and IFS_BITS == 3
    # 0x000/dlc 0 forces stuffing in SOF+ID; 0x7FF likewise
    assert CanFrame(0x000).bit_length() > 44
    assert CanFrame(0x7FF).bit_length() > 44


def test_crc_zero_message_is_zero():
    assert crc15([0] * 20) == 0
    assert crc15([]) == 0


def test_min_frame_is_a_lower_bound():
    assert min(f.bit_length() for f in FRAMES) >= MIN_FRAME_BITS
    # id 0x555 / no data has no stuffing at all in the head; the frame may still be > 44
    assert CanFrame(0x555).bit_length() >= MIN_FRAME_BITS


def test_duration_and_lookahead():
    assert lookahead_ns(500_000) == 88_000
    assert lookahead_ns(1_000_000) == 44_000
    f = CanFrame(0x7FF, b"\xff" * 8)
    assert f.duration_ns(500_000) == f.bit_length() * 2000
    # ceil
    assert CanFrame(0x555).duration_ns(300_000) == -(-CanFrame(0x555).bit_length() * 10**9 // 300_000)
    assert lookahead_ns(300_000) == 44 * 10**9 // 300_000
    assert lookahead_ns(500_000) <= min(f.duration_ns(500_000) for f in FRAMES)


def test_validation():
    with pytest.raises(ValueError):
        CanFrame(0x800)
    with pytest.raises(ValueError):
        CanFrame(0x20000000, ext=True)
    with pytest.raises(ValueError):
        CanFrame(1, b"\x00" * 9)
    with pytest.raises(ValueError):
        CanFrame(1, b"\x01", rtr=True)
    with pytest.raises(ValueError):
        CanFrame(1, rtr=True, dlc=9)
    with pytest.raises(ValueError):
        CanFrame(1, b"\x01\x02", dlc=3)
    with pytest.raises(ValueError):
        CanFrame(-1)
    assert CanFrame(1, b"\x01\x02").dlc == 2


def test_arbitration_order():
    k = lambda f: f.arbitration_key()
    assert k(CanFrame(0x100)) < k(CanFrame(0x101))
    assert k(CanFrame(0x7FF)) > k(CanFrame(0x000))
    # base beats extended with the same 11-bit base id (data and remote)
    assert k(CanFrame(0x123)) < k(CanFrame(0x123 << 18, ext=True))
    assert k(CanFrame(0x123, rtr=True)) < k(CanFrame(0x123 << 18, ext=True))
    # data beats remote
    assert k(CanFrame(0x123)) < k(CanFrame(0x123, rtr=True))
    assert k(CanFrame(0x123 << 18, ext=True)) < k(CanFrame(0x123 << 18, ext=True, rtr=True))
    # extended: base part dominates, then ext part
    assert k(CanFrame((0x100 << 18) | 5, ext=True)) < k(CanFrame((0x100 << 18) | 6, ext=True))
    assert k(CanFrame((0x0FF << 18) | 0x3FFFF, ext=True)) < k(CanFrame(0x100 << 18, ext=True))
    # lower base id beats a higher-id base frame regardless of ext
    assert k(CanFrame(0x122 << 18, ext=True)) < k(CanFrame(0x123, rtr=True))
    assert k(CanFrame(0x123)) == k(CanFrame(0x123, b"\x01"))


def test_candump_golden():
    frames = [
        LoggedFrame(1_000_000, "bus0", CanFrame(0x123, b"\xde\xad\xbe\xef"), "a"),
        LoggedFrame(2_500_123_456, "bus0", CanFrame(0x1ABCDE, b"\x01", ext=True), "b"),
        LoggedFrame(3_000_000_000, "bus0", CanFrame(0x7FF), "a"),
        LoggedFrame(4_000_000_000, "bus0", CanFrame(0x10, rtr=True), "a"),
        LoggedFrame(5_000_000_000, "bus0", CanFrame(0x10, rtr=True, dlc=4), "a"),
    ]
    assert format_candump(frames, "can0") == (
        "(0.001000) can0 123#DEADBEEF\n"
        "(2.500123) can0 001ABCDE#01\n"
        "(3.000000) can0 7FF#\n"
        "(4.000000) can0 010#R\n"
        "(5.000000) can0 010#R4\n"
    )
    assert format_candump([], "can0") == ""


def test_asc_golden():
    frames = [
        LoggedFrame(1_000_000, "bus0", CanFrame(0x123, b"\xde\xad\xbe\xef"), "a"),
        LoggedFrame(2_500_123_456, "bus0", CanFrame(0x1ABCDE, b"\x01", ext=True), "b"),
        LoggedFrame(3_000_000_000, "bus0", CanFrame(0x10, rtr=True, dlc=2), "a"),
    ]
    assert format_asc(frames, 1) == (
        f"date {ASC_DATE}\n"
        "base hex  timestamps absolute\n"
        "internal events logged\n"
        "0.001000 1  123  Rx   d 4 DE AD BE EF\n"
        "2.500123 1  1ABCDEx  Rx   d 1 01\n"
        "3.000000 1  10  Rx   r 2\n"
    )
