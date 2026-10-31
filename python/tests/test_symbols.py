"""Firmware variables by name (DWARF): blinky's Core/Src/vars.c."""

from __future__ import annotations

import pytest

from stest import SymbolError
from stest.symbols import SymbolTable


@pytest.fixture
def node(sim, blinky_exe):
    n = sim.node("a", blinky_exe)
    sim.start()
    return n


def test_scalars(node):
    assert node.var("u8") == 200
    assert node.var("s16") == -5
    assert node.var("f") == 1.5
    assert isinstance(node.var("f"), float)


def test_enum_compares_by_name_and_value(node):
    m = node.var("mode")
    assert m == "MODE_ON"
    assert m == 1
    assert m != "MODE_OFF"
    assert str(m) == "MODE_ON"
    assert int(m) == 1


def test_struct_array_fields(node):
    assert node.var("lamps[1].duty") == 20
    lamp = node.var("lamps[2]")
    assert lamp["on"] is True
    assert lamp["duty"] == 30
    assert dict(lamp) == {"duty": 30, "on": True}
    with pytest.raises(TypeError):
        lamp["duty"] = 1  # read-only mapping
    arr = node.var("lamps")
    assert isinstance(arr, list) and [x["duty"] for x in arr] == [10, 20, 30]
    assert node.var("lamps[1].on") is False


def test_function_static_increments_with_time(sim, node):
    a = node.var("tick_fn::calls")
    sim.advance(ms=2000)
    b = node.var("tick_fn::calls")
    assert b > a


def test_set_var_and_firmware_observes(sim, node):
    node.set_var("lamps[0].duty", 99)
    assert node.var("lamps[0].duty") == 99
    assert node.var("lamps[1].duty") == 20  # neighbours untouched
    sim.advance(ms=1000)
    assert node.var("seen_duty") == 99


def test_set_var_kinds(node):
    node.set_var("f", 2.25)
    assert node.var("f") == 2.25
    node.set_var("mode", "MODE_OFF")
    assert node.var("mode") == "MODE_OFF" and node.var("mode") == 0
    node.set_var("lamps[1].on", True)
    assert node.var("lamps[1].on") is True
    node.set_var("s16", -32768)
    assert node.var("s16") == -32768


def test_set_var_strict(node):
    for expr, bad in [("u8", 256), ("u8", -1), ("s16", 32768), ("s16", -32769)]:
        with pytest.raises(SymbolError, match="out of range"):
            node.set_var(expr, bad)
    assert node.var("u8") == 200 and node.var("s16") == -5  # nothing written
    with pytest.raises(SymbolError):
        node.set_var("mode", "MODE_BOGUS")
    with pytest.raises(SymbolError):
        node.set_var("u8", 1.5)
    with pytest.raises(SymbolError):
        node.set_var("lamps[0]", 1)
    with pytest.raises(SymbolError):
        node.set_var("lamps[0].on", 5)


def test_ambiguous_static(node):
    with pytest.raises(SymbolError) as e:
        node.var("count")
    assert "vars.c:count" in str(e.value) and "vars2.c:count" in str(e.value)
    assert node.var("vars.c:count") == 1
    assert node.var("vars2.c:count") == 2


def test_unknown_and_bad_expressions(node):
    with pytest.raises(SymbolError) as e:
        node.var("nope")
    assert str(e.value) == "no symbol 'nope'"
    for bad in ["lamps[3]", "lamps[0].nofield", "u8.x", "u8[0]", "lamps.duty", "lamps[", "", "1x"]:
        with pytest.raises(SymbolError):
            node.var(bad)


def test_symbol_table_cached_and_static_resolve(blinky_exe):
    t = SymbolTable.load(blinky_exe)
    assert t is SymbolTable.load(blinky_exe)
    r = t.resolve("lamps[1].duty")
    assert r.addr == t.resolve("lamps").addr + 4 + 0  # sizeof(struct lamp) == 4, duty at offset 0
    assert r.type.size == 2
