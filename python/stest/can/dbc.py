"""DBC support (via cantools): encode/decode, DBC-aware devices with periodic senders.

Signal values are physical (scaled) values; enum (``VAL_``) signals decode to their choice
name (a ``str``) and encode from either the name or the raw int. Errors from cantools
(unknown message or signal, a value out of its range, a missing signal, a frame whose
length does not fit the message) are raised as :class:`~stest.StestError` with the original
exception chained. ``Dbc.decode`` returns ``None`` for a frame no message matches (unknown
id, extended/standard mismatch, remote frames).

cantools (and python-can, LGPL-3.0, which it imports) is an optional dependency, the ``dbc``
extra (``pip install 'stest[dbc]'``), imported only when a :class:`Dbc` is loaded: ``import
stest`` never loads it, and without it ``Dbc(...)`` raises :class:`~stest.StestError`.
"""

from __future__ import annotations

import os
from typing import Callable

from ..errors import StestError
from .device import CanDevice
from .frame import CanFrame
from .log import LoggedFrame


def _cantools():
    """The cantools module, imported on first use (the optional ``dbc`` extra)."""
    try:
        import cantools
        import cantools.database
    except ImportError as e:
        raise StestError(
            "DBC support needs cantools, which is not installed: install the 'dbc' extra "
            "(pip install 'stest[dbc]', or uv add 'stest[dbc]')"
        ) from e
    return cantools


class Dbc:
    """A loaded DBC file."""

    def __init__(self, path: str | os.PathLike):
        cantools = _cantools()
        try:
            db = cantools.database.load_file(os.fspath(path))
        except Exception as e:
            raise StestError(f"cannot load DBC {os.fspath(path)!r}: {e}") from e
        if not isinstance(db, cantools.database.Database):
            raise StestError(f"{os.fspath(path)!r} is not a CAN database (DBC/KCD/SYM)")
        self.path = os.fspath(path)
        self.db = db

    def message(self, name: str):
        """The cantools message named ``name``."""
        try:
            return self.db.get_message_by_name(name)
        except KeyError:
            names = ", ".join(m.name for m in self.db.messages)
            raise StestError(f"DBC {self.path!r} has no message {name!r} (messages: {names})") from None

    def encode(self, message: str, signals: dict) -> CanFrame:
        """The frame for ``message`` with physical ``signals`` (all of them; multiplexed
        messages: the selector plus that multiplexer's signals). Out-of-range values raise."""
        msg = self.message(message)
        try:
            data = msg.encode(dict(signals), scaling=True, strict=True)
        except Exception as e:
            raise StestError(f"DBC encode of {message!r} with {dict(signals)!r} failed: {e}") from e
        return CanFrame(msg.frame_id, data, ext=msg.is_extended_frame)

    def decode(self, frame: CanFrame) -> tuple[str, dict] | None:
        """``(message name, {signal: value})`` or ``None`` if no message has the frame's id."""
        if frame.rtr:
            return None
        try:
            msg = self.db.get_message_by_frame_id(frame.id, force_extended_id=frame.ext)
        except KeyError:
            return None
        if msg.is_extended_frame != frame.ext:
            return None
        try:
            raw = msg.decode(frame.data, decode_choices=True, scaling=True)
        except Exception as e:
            raise StestError(f"DBC decode of {frame} as {msg.name!r} failed: {e}") from e
        return msg.name, {k: (v.name if hasattr(v, "name") and hasattr(v, "value") else v) for k, v in raw.items()}


class DbcDevice(CanDevice):
    """A virtual device that speaks a DBC: ``send_message``, ``send_periodic`` and the latest
    decoded value of every message it has seen (``received``)."""

    def __init__(self, name: str, dbc: Dbc, **kw):
        super().__init__(name, **kw)
        self.dbc = dbc
        self.received: dict[str, tuple[int, dict]] = {}

    def on_frame(self, lf: LoggedFrame) -> None:
        d = self.dbc.decode(lf.frame)
        if d is not None:
            self.received[d[0]] = (lf.t_ns, d[1])

    def send_message(self, message: str, **signals) -> None:
        """Encode and request ``message`` now (device time)."""
        self.send(self.dbc.encode(message, signals))

    def send_periodic(
        self, message: str, period_ms: float, signals: dict | Callable[[], dict]
    ) -> None:
        """Request ``message`` every ``period_ms`` of virtual time, first at now + period,
        at exact multiples of the period (see :meth:`CanDevice.every`). ``signals`` is a dict
        or a callable returning one, evaluated at each send (a bad value raises StestError
        at the first affected send, ending the simulation like any device callback error)."""
        self.dbc.message(message)  # fail now on an unknown name

        def tick() -> None:
            sig = signals() if callable(signals) else signals
            self.send(self.dbc.encode(message, sig))

        self.every(period_ms, tick)
