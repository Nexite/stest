"""Shared fixtures: self-test executables. (The `sim` fixture comes from the stest pytest plugin.)

Executables are located in $STEST_SELFTEST_BIN (set by CTest's `python_tests`);
for direct `uv run pytest` runs it defaults to <repo>/build/selftest.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

pytest_plugins = ["pytester"]

_REPO = Path(__file__).resolve().parents[2]


def _selftest_bin() -> Path:
    env = os.environ.get("STEST_SELFTEST_BIN")
    d = Path(env) if env else _REPO / "build" / "selftest"
    if not d.is_dir():
        pytest.fail(
            f"self-test binaries not found in {d}: build the project "
            "(cmake --build build) or set STEST_SELFTEST_BIN"
        )
    return d


def _exe(name: str) -> Path:
    p = _selftest_bin() / name
    if not p.is_file():
        pytest.fail(f"self-test executable {p} not found (build it first)")
    return p


@pytest.fixture
def blinky_exe() -> Path:
    return _exe("blinky")


@pytest.fixture
def faults_exe():
    """Factory: faults_exe("spin" | "crash" | "unmodelled" | "ro_write" | "clkoff") -> path of that variant."""

    def get(variant: str) -> Path:
        return _exe(f"faults_{variant.lower()}")

    return get



@pytest.fixture
def pollcount_exe() -> Path:
    return _exe("pollcount")


@pytest.fixture
def exti_exe() -> Path:
    return _exe("exti")


@pytest.fixture
def tim_exe() -> Path:
    return _exe("tim")


@pytest.fixture
def adc_exe():
    """Factory: adc_exe("" | "async" | "async_noclk" | "scan") -> path of that adc firmware."""

    def get(variant: str = "") -> Path:
        return _exe(f"adc_{variant}" if variant else "adc")

    return get


@pytest.fixture
def dac_exe() -> Path:
    return _exe("dac")


@pytest.fixture
def spi_exe():
    """Factory: spi_exe("" | "tx") -> path of that spi firmware (TransmitReceive / Transmit)."""

    def get(variant: str = "") -> Path:
        return _exe(f"spi_{variant}" if variant else "spi")

    return get
