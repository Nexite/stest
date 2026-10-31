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


def test_member_offset_decoding():
    from stest.symbols import decode_member_offset as d

    assert d(8) == 8
    assert d([0x23, 0x08]) == 8  # DW_OP_plus_uconst 8
    assert d([0x23, 0x80, 0x01]) == 128  # multi-byte ULEB
    assert d([0x23, 0x80]) is None  # truncated ULEB
    assert d([0x10, 0x08]) is None  # other op: unsupported, never silently 0
    assert d(None) is None


def test_discarded_variable_address_zero_is_skipped():
    from stest.symbols import decode_static_location as d

    assert d(bytes([0x03]) + (0x1000).to_bytes(8, "little"), None) == 0x1000
    assert d(bytes([0x03]) + bytes(8), None) is None  # stripped: addr 0
    assert d(bytes([0xA1, 0x02]), lambda i: 0) is None  # addrx resolving to 0
    assert d(bytes([0xA1, 0x02]), lambda i: 0x2000 + i) == 0x2002


def test_cache_invalidated_when_exe_rebuilt(blinky_exe, tmp_path):
    import os
    import shutil

    exe = tmp_path / "blinky"
    shutil.copy(blinky_exe, exe)
    dsym = blinky_exe.with_name(blinky_exe.name + ".dSYM")
    if dsym.is_dir():
        shutil.copytree(dsym, exe.with_name("blinky.dSYM"))
    a = SymbolTable.load(exe)
    assert SymbolTable.load(exe) is a
    st = exe.stat()
    os.utime(exe, ns=(st.st_atime_ns, st.st_mtime_ns + 1_000_000))
    b = SymbolTable.load(exe)
    assert b is not a and b.anchor_addr == a.anchor_addr


class _Attr:
    def __init__(self, value):
        self.value = value


class _Die:
    """Minimal DIE stand-in for SymbolTable._add_members."""

    _n = 0

    def __init__(self, tag, attrs=None, children=(), type_die=None):
        _Die._n += 1
        self.offset = _Die._n
        self.tag = tag
        self.attributes = {k: _Attr(v) for k, v in (attrs or {}).items()}
        if type_die is not None:
            self.attributes["DW_AT_type"] = _Attr(0)
        self._type = type_die
        self._children = list(children)

    def iter_children(self):
        return iter(self._children)

    def get_DIE_from_attribute(self, name):
        assert name == "DW_AT_type"
        return self._type


def test_computed_member_offset_raises_on_access_named_and_anonymous():
    from stest.symbols import StructType

    u8 = _Die("DW_TAG_base_type", {"DW_AT_byte_size": 1, "DW_AT_encoding": 0x08, "DW_AT_name": b"u"})
    ok = _Die("DW_TAG_member", {"DW_AT_name": b"ok", "DW_AT_data_member_location": 0}, type_die=u8)
    named_bad = _Die(
        "DW_TAG_member", {"DW_AT_name": b"nb", "DW_AT_data_member_location": [0x10, 0x04]}, type_die=u8
    )
    inner = _Die(
        "DW_TAG_structure_type",
        {"DW_AT_byte_size": 2},
        [_Die("DW_TAG_member", {"DW_AT_name": b"x", "DW_AT_data_member_location": 1}, type_die=u8)],
    )
    anon_bad = _Die("DW_TAG_member", {"DW_AT_data_member_location": [0x10, 0x08]}, type_die=inner)
    anon_ok = _Die("DW_TAG_member", {"DW_AT_data_member_location": 4}, type_die=inner)
    outer = _Die("DW_TAG_structure_type", {"DW_AT_byte_size": 16}, [ok, named_bad, anon_bad])

    tbl = SymbolTable.__new__(SymbolTable)
    tbl._types = {}
    st = StructType(16, "outer", "struct")
    tbl._add_members(st, outer, 0)
    assert st.members["ok"][0] == 0 and st.members["ok"][1].decode(b"\x07") == 7
    for name in ("nb", "x"):  # named member and hoisted child of the anonymous member
        with pytest.raises(SymbolError, match="computed"):
            st.members[name][1].decode(b"\0")
        with pytest.raises(SymbolError, match="computed"):
            st.members[name][1].encode(1)

    good = StructType(16, "good", "struct")  # control: constant offset still hoists correctly
    tbl._add_members(good, _Die("DW_TAG_structure_type", {}, [anon_ok]), 0)
    assert good.members["x"][0] == 5 and good.members["x"][1].decode(b"\x09") == 9


def test_var_hot_path(node, monkeypatch):
    import time

    def boom(*a, **k):
        raise AssertionError("SymbolTable.load called after start")

    monkeypatch.setattr(SymbolTable, "load", classmethod(boom))
    t0 = time.monotonic()
    for _ in range(10_000):
        node.var("u8")
    assert time.monotonic() - t0 <= 0.5
    node.set_var("u8", 3)
    assert node.var("u8") == 3
