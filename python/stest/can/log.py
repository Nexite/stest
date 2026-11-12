"""candump and Vector ASC formatting for logged CAN frames."""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass

from .frame import CanFrame

# Fixed so output is deterministic (no wall-clock date in logs).
ASC_DATE = "Thu Jan 1 00:00:00.000 am 1970"


@dataclass(frozen=True)
class LoggedFrame:
    """A frame delivered on a bus, as recorded in [`CanBus.log`][stest.CanBus].

    Attributes:
        t_ns: Virtual time in ns of the frame's end of frame (EOF), when receivers get it.
        bus: Name of the bus.
        frame: The [`CanFrame`][stest.CanFrame].
        sender: Who sent it: a firmware node's name, a device's name, or a
            ``CanBus.send`` sender name (default ``"python"``).
    """

    t_ns: int  # end of frame (EOF)
    bus: str
    frame: CanFrame
    sender: str


def _ts(t_ns: int) -> str:
    return f"{t_ns // 1_000_000_000}.{(t_ns % 1_000_000_000) // 1000:06d}"


def format_candump(frames: Iterable[LoggedFrame], channel: str) -> str:
    """Format frames as candump log lines (``candump -l`` / ``can-utils`` style).

    One line per frame: ``(<s>.<us>) <channel> <ID>#<DATA>`` with the EOF time in seconds and
    microseconds, a 3-digit (standard) or 8-digit (extended) hex identifier, and
    ``R<dlc>`` for remote frames.

    Args:
        frames: The frames, e.g. ``bus.log``.
        channel: Interface name written on every line, e.g. the bus name.

    Returns:
        The text, one ``\\n``-terminated line per frame (empty for no frames).
    """
    out = []
    for lf in frames:
        f = lf.frame
        ident = f"{f.id:08X}" if f.ext else f"{f.id:03X}"
        if f.rtr:
            payload = "R" + (str(f.dlc) if f.dlc else "")
        else:
            payload = f.data.hex().upper()
        out.append(f"({_ts(lf.t_ns)}) {channel} {ident}#{payload}\n")
    return "".join(out)


def format_asc(frames: Iterable[LoggedFrame], channel_no: int) -> str:
    """Format frames as a Vector ASC log (absolute timestamps, hex).

    The header carries a fixed 1970 date so the output is deterministic; every frame is
    logged as ``Rx``.

    Args:
        frames: The frames, e.g. ``bus.log``.
        channel_no: ASC channel number written on every line.

    Returns:
        The text, header included.
    """
    out = [f"date {ASC_DATE}\n", "base hex  timestamps absolute\n", "internal events logged\n"]
    for lf in frames:
        f = lf.frame
        ident = f"{f.id:X}" + ("x" if f.ext else "")
        if f.rtr:
            tail = f"r {f.dlc}"
        else:
            tail = " ".join(["d", str(f.dlc)] + [f"{b:02X}" for b in f.data])
        out.append(f"{_ts(lf.t_ns)} {channel_no}  {ident}  Rx   {tail}\n")
    return "".join(out)
