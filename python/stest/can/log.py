"""candump and Vector ASC formatting for logged CAN frames."""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass

from .frame import CanFrame

# Fixed so output is deterministic (no wall-clock date in logs).
ASC_DATE = "Thu Jan 1 00:00:00.000 am 1970"


@dataclass(frozen=True)
class LoggedFrame:
    t_ns: int  # end of frame (EOF)
    bus: str
    frame: CanFrame
    sender: str


def _ts(t_ns: int) -> str:
    return f"{t_ns // 1_000_000_000}.{(t_ns % 1_000_000_000) // 1000:06d}"


def format_candump(frames: Iterable[LoggedFrame], channel: str) -> str:
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
