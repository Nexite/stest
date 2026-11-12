"""Wall-clock speed budgets are load-sensitive: run in parallel with other CTests
(``ctest -j8``) they fail at random. The ``perf`` marker routes them to the CTest
``python_perf_tests``, which runs RUN_SERIAL (alone), and ``python_tests`` skips them. These
checks keep that true: every test that reads a wall clock is marked, and the CTests are
registered that way."""

from __future__ import annotations

import ast
import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

TESTS = Path(__file__).resolve().parent
CLOCKS = {"perf_counter", "monotonic", "time", "process_time", "perf_counter_ns", "monotonic_ns"}


def _reads_wall_clock(fn: ast.FunctionDef) -> bool:
    for node in ast.walk(fn):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute):
            f = node.func
            if f.attr in CLOCKS and isinstance(f.value, ast.Name) and f.value.id == "time":
                return True
    return False


def _marked_perf(fn: ast.FunctionDef) -> bool:
    for d in fn.decorator_list:
        if isinstance(d, ast.Attribute) and d.attr == "perf":
            return True
    return False


def test_every_wall_clock_test_is_marked_perf():
    unmarked, marked = [], []
    for path in sorted(TESTS.glob("test_*.py")):
        tree = ast.parse(path.read_text(), str(path))
        for fn in tree.body:
            if isinstance(fn, ast.FunctionDef) and fn.name.startswith("test_"):
                if _reads_wall_clock(fn) and not _marked_perf(fn):
                    unmarked.append(f"{path.name}::{fn.name}")
                if _marked_perf(fn):
                    marked.append(fn.name)
    assert unmarked == [], f"wall-clock tests without @pytest.mark.perf: {unmarked}"
    assert len(marked) >= 5  # adc, tim, can speed, engine backlog cost, var hot path


def _ctest_json() -> dict:
    binary = os.environ.get("STEST_SELFTEST_BIN")
    ctest = shutil.which("ctest")
    if not binary or not ctest:
        pytest.skip("needs CTest's environment (STEST_SELFTEST_BIN) and ctest")
    out = subprocess.run(
        [ctest, "--test-dir", str(Path(binary).parent), "--show-only=json-v1"],
        capture_output=True, text=True, check=True,
    ).stdout
    return json.loads(out)


def test_ctest_runs_perf_tests_serially():
    tests = {t["name"]: t for t in _ctest_json()["tests"]}

    def props(name: str) -> dict:
        return {p["name"]: p["value"] for p in tests[name].get("properties", [])}

    assert "-m" in tests["python_tests"]["command"] and "not perf" in tests["python_tests"]["command"]
    assert "perf" in tests["python_perf_tests"]["command"]
    for name in ("python_perf_tests", "selftest_blinky_speed"):
        assert props(name).get("RUN_SERIAL") is True, name
