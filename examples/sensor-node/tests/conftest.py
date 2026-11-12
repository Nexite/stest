"""Fixtures shared by the sensor-node tests."""

from __future__ import annotations

# --8<-- [start:imports]
import os
from pathlib import Path

import pytest
# --8<-- [end:imports]

from stest import Dbc

# --8<-- [start:firmware]
EXAMPLE_DIR = Path(__file__).resolve().parents[1]


@pytest.fixture
def firmware():
    """``firmware("sensor")`` -> path of a firmware executable built by CMake.

    Looks in ``$SENSOR_NODE_BIN`` (default: ``<example>/build``) and skips the test if the
    executable is missing."""
    bin_dir = Path(os.environ.get("SENSOR_NODE_BIN", EXAMPLE_DIR / "build"))

    def path(name: str) -> str:
        exe = bin_dir / name
        if not exe.is_file():
            pytest.skip(
                f"{exe} is not built: run `cmake -S . -B build -G Ninja && cmake --build build` "
                f"in {EXAMPLE_DIR} (or set SENSOR_NODE_BIN to your build directory)"
            )
        return str(exe)

    return path
# --8<-- [end:firmware]


@pytest.fixture(scope="session")
def dbc() -> Dbc:
    """The CAN database shared by both boards."""
    return Dbc(EXAMPLE_DIR / "can" / "sensor-node.dbc")
