"""Classic CAN (ISO 11898-1) frame model: bit-exact length, CRC-15, arbitration."""

from __future__ import annotations

from dataclasses import dataclass

# Conservative floor of a frame's length SOF..EOF: a 0-byte standard data frame would be 44
# bits without stuff bits, but every real frame has at least one (the true minimum is 45).
MIN_FRAME_BITS = 44
IFS_BITS = 3  # intermission, not part of bit_length()
_CRC_POLY = 0x4599
_TAIL_BITS = 1 + 1 + 1 + 7  # CRC delimiter, ACK slot, ACK delimiter, EOF


def crc15(bits: list[int]) -> int:
    """CAN CRC-15 (polynomial 0x4599) over unstuffed bits SOF..end of data."""
    crc = 0
    for b in bits:
        feedback = ((crc >> 14) & 1) ^ (b & 1)
        crc = (crc << 1) & 0x7FFF
        if feedback:
            crc ^= _CRC_POLY
    return crc


def _int_bits(value: int, width: int) -> list[int]:
    return [(value >> i) & 1 for i in range(width - 1, -1, -1)]


def _stuffed_len(bits: list[int]) -> int:
    n = len(bits)
    last = -1
    run = 0
    for b in bits:
        if b == last:
            run += 1
        else:
            last, run = b, 1
        if run == 5:
            n += 1
            last, run = 1 - b, 1  # the stuff bit starts a new run
    return n


@dataclass(frozen=True)
class CanFrame:
    id: int
    data: bytes = b""
    ext: bool = False
    rtr: bool = False
    dlc: int | None = None

    def __post_init__(self) -> None:
        data = bytes(self.data)
        object.__setattr__(self, "data", data)
        limit = 0x1FFFFFFF if self.ext else 0x7FF
        if not 0 <= self.id <= limit:
            raise ValueError(f"CAN id {self.id:#x} out of range for {'extended' if self.ext else 'standard'} frame")
        if len(data) > 8:
            raise ValueError("classic CAN data length must be 0..8")
        if self.rtr and data:
            raise ValueError("remote frame must not carry data")
        if self.dlc is None:
            object.__setattr__(self, "dlc", len(data))
        elif not 0 <= self.dlc <= 8:
            raise ValueError("dlc must be 0..8")
        elif not self.rtr and self.dlc != len(data):
            raise ValueError("dlc must equal len(data) for data frames")

    def _head_bits(self) -> list[int]:
        """Unstuffed bits SOF .. end of data field."""
        rtr = int(self.rtr)
        if self.ext:
            bits = [0] + _int_bits(self.id >> 18, 11) + [1, 1] + _int_bits(self.id & 0x3FFFF, 18) + [rtr, 0, 0]
        else:
            bits = [0] + _int_bits(self.id, 11) + [rtr, 0, 0]
        bits += _int_bits(self.dlc, 4)  # type: ignore[arg-type]
        for byte in self.data:
            bits += _int_bits(byte, 8)
        return bits

    def bit_length(self) -> int:
        """Bits on the wire SOF..EOF including stuff bits; excludes IFS."""
        head = self._head_bits()
        return _stuffed_len(head + _int_bits(crc15(head), 15)) + _TAIL_BITS

    def duration_ns(self, bitrate: int) -> int:
        return -(-self.bit_length() * 1_000_000_000 // bitrate)

    def arbitration_key(self) -> tuple:
        """Wire arbitration field as a bit tuple; lexicographically smaller wins.

        Standard: base ID, RTR, IDE(0). Extended: base ID, SRR(1), IDE(1), ext ID, RTR.
        """
        rtr = int(self.rtr)
        if self.ext:
            return (*_int_bits(self.id >> 18, 11), 1, 1, *_int_bits(self.id & 0x3FFFF, 18), rtr)
        return (*_int_bits(self.id, 11), rtr, 0)


def lookahead_ns(bitrate: int) -> int:
    """Lower bound of any frame's time (MIN_FRAME_BITS bit times, floored); the scheduler
    quantum cap. Every real frame (>= 45 bits) takes strictly longer."""
    return MIN_FRAME_BITS * 1_000_000_000 // bitrate
