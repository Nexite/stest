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
    """CAN CRC-15 (polynomial 0x4599).

    Args:
        bits: Unstuffed frame bits from SOF to the end of the data field, one ``int``
            (0 or 1) per bit.

    Returns:
        The 15-bit CRC.
    """
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
    """An immutable classic CAN frame (ISO 11898-1, 0..8 data bytes).

    ```python
    CanFrame(0x123, b"\\x01\\x02")            # standard data frame, dlc 2
    CanFrame(0x18FF0001, b"", ext=True)       # extended identifier
    CanFrame(0x123, rtr=True, dlc=4)          # remote frame requesting 4 bytes
    ```

    Frames compare equal by value and are hashable.

    Args:
        id: Identifier: 0..0x7FF (standard) or 0..0x1FFFFFFF (``ext=True``).
        data: Payload, 0..8 bytes (any bytes-like value; stored as ``bytes``).
        ext: Extended (29-bit) identifier.
        rtr: Remote frame (no data; ``dlc`` is the requested length).
        dlc: Data length code 0..8. Defaults to ``len(data)``; for a data frame it must
            equal ``len(data)``.

    Raises:
        ValueError: ``id`` out of range, more than 8 data bytes, a remote frame with data,
            ``dlc`` outside 0..8, or a data frame whose ``dlc`` differs from ``len(data)``.

    Attributes:
        id: The identifier.
        data: The payload (``bytes``).
        ext: True for an extended identifier.
        rtr: True for a remote frame.
        dlc: The data length code (never ``None`` after construction).
    """

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
        """Exact number of bits on the wire from SOF to EOF, stuff bits included.

        Excludes the 3-bit intermission (`IFS_BITS`).
        """
        head = self._head_bits()
        return _stuffed_len(head + _int_bits(crc15(head), 15)) + _TAIL_BITS

    def duration_ns(self, bitrate: int) -> int:
        """Time on the wire from SOF to EOF.

        Args:
            bitrate: Bit rate in bit/s.

        Returns:
            ``ceil(bit_length() * 1e9 / bitrate)`` in ns (exact for bit rates dividing 1e9).
        """
        return -(-self.bit_length() * 1_000_000_000 // bitrate)

    def arbitration_bits(self) -> int:
        """Bits on the wire, stuff bits included, from SOF through the last bit of the
        arbitration field (RTR): 13 unstuffed bits for a standard frame, 33 for an extended
        one. An injected error starts right after them."""
        return _stuffed_len(self._head_bits()[: 33 if self.ext else 13])

    def bits_to_difference(self, other: CanFrame) -> int:
        """Bits on the wire from SOF through the first bit at which two frames differ.

        For two frames with the same arbitration field sent at the same instant, this is
        where the transmitter of the recessive bit detects a bit error. Both share the
        prefix, so their stuff bits agree.

        Args:
            other: A frame with the same arbitration field.

        Returns:
            The number of bits, stuff bits included.

        Raises:
            ValueError: The arbitration fields differ, or the frames are identical on the wire.
        """
        if other.arbitration_key() != self.arbitration_key():
            raise ValueError("bits_to_difference: the frames have different arbitration fields")
        a, b = self._head_bits(), other._head_bits()
        for i, (x, y) in enumerate(zip(a, b)):
            if x != y:
                return _stuffed_len(a[: i + 1])
        raise ValueError("bits_to_difference: the frames are identical on the wire")

    def arbitration_key(self) -> tuple:
        """The wire arbitration field as a tuple of bits; the lexicographically smaller
        one wins arbitration.

        Standard: base ID, RTR, IDE (0). Extended: base ID, SRR (1), IDE (1), extended ID,
        RTR. So a standard frame beats an extended one with the same base ID, and a data
        frame beats a remote frame with the same identifier.
        """
        rtr = int(self.rtr)
        if self.ext:
            return (*_int_bits(self.id >> 18, 11), 1, 1, *_int_bits(self.id & 0x3FFFF, 18), rtr)
        return (*_int_bits(self.id, 11), rtr, 0)


def lookahead_ns(bitrate: int) -> int:
    """Lower bound of any frame's time on the wire: the scheduler's quantum cap.

    ``MIN_FRAME_BITS`` (44) bit times, rounded down. Every real frame (at least 45 bits)
    takes strictly longer, which keeps the bus engine causal.

    Args:
        bitrate: Bit rate in bit/s.

    Returns:
        The bound in ns (e.g. 88_000 at 500 kbit/s).
    """
    return MIN_FRAME_BITS * 1_000_000_000 // bitrate
