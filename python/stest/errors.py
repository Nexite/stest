"""Exceptions raised by the stest Python harness."""

from __future__ import annotations


class StestError(Exception):
    """Base class of every stest error (also: a recoverable node request failed)."""


class NodeCrashed(StestError):
    """A node process died: host crash (signal), strict-mode abort (ERROR, exit 70),
    firmware main returning, or an unexpected exit. Every node has been killed and reaped."""


class WaitTimeout(StestError):
    """A wait_for / Pin.wait_* condition did not hold within its virtual-time budget."""


class WallClockTimeout(StestError):
    """A node did not answer within wall_timeout_s of real time (e.g. firmware spinning
    with no MMIO access). Every node has been killed and reaped."""


class SymbolError(StestError):
    """A firmware symbol could not be resolved (used by the symbol layer)."""


class SymbolRangeError(SymbolError, ValueError):
    """A value written to a firmware variable does not fit the variable's type."""
