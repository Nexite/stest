"""Firmware variables by name, from the executable's DWARF debug info.

``SymbolTable.load(exe)`` indexes the global / file-scope-static / function-static
variables of an executable (ELF: pyelftools; Mach-O: the ``.dSYM`` DWARF sections via
:mod:`stest.macho`). ``resolve("g_cfg.lamps[2].duty")`` yields the *static* (link-time)
address and DWARF type; the runtime address is ``static + slide`` where the slide is
``HELLO anchor_addr - SymbolTable.anchor_addr`` (``stest_symbol_anchor`` taken from the
symbol table, since the runtime itself carries no debug info).

Expression grammar::

    expr   := head ( '.' field | '[' index ']' )*
    head   := [ file ':' ] [ func '::' ] name        # file = CU basename, e.g. vars.c

A plain ``name`` searches globals first, then file-scope statics of every CU; more than
one candidate raises SymbolError listing them as ``<file>:<name>``.
"""

from __future__ import annotations

import re
import struct
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

from elftools.elf.elffile import ELFFile

from .errors import SymbolError, SymbolRangeError
from .macho import MachO, MachOError

ANCHOR = "stest_symbol_anchor"

# DWARF constants
DW_ATE_boolean, DW_ATE_float, DW_ATE_signed, DW_ATE_signed_char = 0x02, 0x04, 0x05, 0x06
DW_ATE_unsigned, DW_ATE_unsigned_char, DW_ATE_UTF = 0x07, 0x08, 0x10
DW_OP_addr, DW_OP_addrx = 0x03, 0xA1


# ---------------------------------------------------------------- values
class EnumValue(int):
    """An enum value: an int that also compares equal to its enumerator name."""

    name: str | None

    def __new__(cls, value: int, name: str | None = None):
        obj = super().__new__(cls, value)
        obj.name = name
        return obj

    def __eq__(self, other: object) -> bool:
        if isinstance(other, str):
            return self.name is not None and self.name == other
        return int.__eq__(self, other)

    def __ne__(self, other: object) -> bool:
        r = self.__eq__(other)
        return r if r is NotImplemented else not r

    __hash__ = int.__hash__

    def __str__(self) -> str:
        return self.name if self.name is not None else int.__repr__(self)

    def __repr__(self) -> str:
        return f"EnumValue({int(self)}, {self.name!r})"


class StructValue(Mapping):
    """Read-only mapping of a struct/union's members."""

    def __init__(self, items: dict[str, Any]):
        self._d = items

    def __getitem__(self, k: str) -> Any:
        return self._d[k]

    def __iter__(self) -> Iterator[str]:
        return iter(self._d)

    def __len__(self) -> int:
        return len(self._d)

    def __repr__(self) -> str:
        return f"StructValue({self._d!r})"


# ---------------------------------------------------------------- types
class Type:
    size = 0
    label = "?"

    def decode(self, data: bytes) -> Any:
        raise NotImplementedError

    def encode(self, value: Any) -> bytes:
        raise SymbolError(f"cannot assign to a {self.label}; assign its scalar members individually")


def _check_int(value: Any, size: int, signed: bool, label: str) -> int:
    if isinstance(value, float) or not isinstance(value, int):
        raise SymbolError(f"cannot assign {type(value).__name__} {value!r} to {label}")
    bits = 8 * size
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    if not lo <= value <= hi:
        raise SymbolRangeError(f"value {int(value)} out of range for {label} ({lo}..{hi})")
    return int(value)


class IntType(Type):
    def __init__(self, size: int, signed: bool, name: str):
        self.size, self.signed, self.label = size, signed, name

    def decode(self, data: bytes) -> int:
        return int.from_bytes(data, "little", signed=self.signed)

    def encode(self, value: Any) -> bytes:
        v = _check_int(value, self.size, self.signed, self.label)
        return v.to_bytes(self.size, "little", signed=self.signed)


class PtrType(IntType):
    def __init__(self, size: int):
        super().__init__(size, False, "pointer")


class BoolType(Type):
    def __init__(self, size: int, name: str):
        self.size, self.label = size, name

    def decode(self, data: bytes) -> bool:
        v = int.from_bytes(data, "little")
        if v > 1:
            raise SymbolError(f"{self.label} holds {v}, not 0 or 1")
        return bool(v)

    def encode(self, value: Any) -> bytes:
        if isinstance(value, float) or not isinstance(value, int) or value not in (0, 1):
            raise SymbolError(f"cannot assign {value!r} to {self.label} (use True/False)")
        return int(value).to_bytes(self.size, "little")


class FloatType(Type):
    def __init__(self, size: int, name: str):
        if size not in (4, 8):
            raise SymbolError(f"unsupported floating-point type {name} ({size} bytes)")
        self.size, self.label = size, name
        self._fmt = "<f" if size == 4 else "<d"

    def decode(self, data: bytes) -> float:
        return struct.unpack(self._fmt, data)[0]

    def encode(self, value: Any) -> bytes:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise SymbolError(f"cannot assign {type(value).__name__} {value!r} to {self.label}")
        try:
            return struct.pack(self._fmt, value)
        except (OverflowError, struct.error) as e:
            raise SymbolRangeError(f"value {value!r} out of range for {self.label}") from e


class EnumType(Type):
    def __init__(self, size: int, signed: bool, name: str, enumerators: dict[str, int]):
        self.size, self.signed, self.label = size, signed, name
        self.enumerators = enumerators
        self.names = {}
        for n, v in enumerators.items():
            self.names.setdefault(v, n)

    def decode(self, data: bytes) -> EnumValue:
        v = int.from_bytes(data, "little", signed=self.signed)
        return EnumValue(v, self.names.get(v))

    def encode(self, value: Any) -> bytes:
        if isinstance(value, str):
            if value not in self.enumerators:
                raise SymbolError(
                    f"{value!r} is not an enumerator of {self.label} (have: {', '.join(self.enumerators)})"
                )
            value = self.enumerators[value]
        v = _check_int(value, self.size, self.signed, self.label)
        return v.to_bytes(self.size, "little", signed=self.signed)


class StructType(Type):
    def __init__(self, size: int, name: str, kind: str):
        self.size, self.label = size, f"{kind} {name}" if name else kind
        self.members: dict[str, tuple[int, Type]] = {}

    def decode(self, data: bytes) -> StructValue:
        return StructValue({n: t.decode(data[o : o + t.size]) for n, (o, t) in self.members.items()})


class ArrayType(Type):
    def __init__(self, elem: Type, count: int | None):
        self.elem, self.count = elem, count
        self.size = elem.size * (count or 0)
        self.label = f"{elem.label}[{'' if count is None else count}]"

    def decode(self, data: bytes) -> list:
        n, es = self.count or 0, self.elem.size
        return [self.elem.decode(data[i * es : (i + 1) * es]) for i in range(n)]


class UnsupportedType(Type):
    def __init__(self, why: str):
        self.label = why

    def decode(self, data: bytes) -> Any:
        raise SymbolError(f"unsupported type: {self.label}")

    def encode(self, value: Any) -> bytes:
        raise SymbolError(f"unsupported type: {self.label}")


# ---------------------------------------------------------------- table
@dataclass(frozen=True)
class Resolved:
    """A variable (or element/field of one): static address, type, source expression."""

    expr: str
    addr: int
    type: Type


@dataclass
class _Var:
    name: str
    cu: str  # CU basename
    external: bool
    func: str | None
    addr: int
    die: Any  # DWARF DIE of the variable

    @property
    def label(self) -> str:
        return f"{self.cu}:{self.func + '::' if self.func else ''}{self.name}"


_HEAD = re.compile(r"(?:(?P<file>[^:\[\].\s][^:\[\]\s]*):(?!:))?(?:(?P<func>\w+)::)?(?P<name>[A-Za-z_]\w*)")
_STEP = re.compile(r"\.([A-Za-z_]\w*)|\[\s*(\d+)\s*\]")

_cache: dict[Path, SymbolTable] = {}


def _s(v: Any) -> str:
    return v.decode("utf-8", "replace") if isinstance(v, bytes) else str(v)


class SymbolTable:
    """Variables of one executable. Use :meth:`load` (cached per path)."""

    def __init__(self, exe: Path, dwarf: Any, anchor_addr: int):
        self.exe = exe
        self.anchor_addr = anchor_addr  # static address of stest_symbol_anchor
        self._dwarf = dwarf
        self._types: dict[int, Type] = {}
        self._vars: list[_Var] = []
        for cu in dwarf.iter_CUs():
            self._index_cu(cu)

    # ---- loading ----
    @classmethod
    def load(cls, exe: Path | str) -> SymbolTable:
        key = Path(exe).resolve()
        t = _cache.get(key)
        if t is None:
            t = _cache[key] = cls._load(key)
        return t

    @classmethod
    def _load(cls, exe: Path) -> SymbolTable:
        if not exe.is_file():
            raise SymbolError(f"executable {exe} not found")
        with exe.open("rb") as f:
            magic = f.read(4)
        if magic == b"\x7fELF":
            with exe.open("rb") as f:
                elf = ELFFile(f)
                if not elf.has_dwarf_info():
                    raise SymbolError(f"{exe}: no DWARF debug info (build with -g)")
                dwarf = elf.get_dwarf_info()
                dwarf.get_addr  # noqa: B018 (API presence check)
                anchor = None
                symtab = elf.get_section_by_name(".symtab")
                if symtab is not None:
                    for sym in symtab.iter_symbols():
                        if sym.name == ANCHOR:
                            anchor = sym["st_value"]
                            break
                # pyelftools reads lazily from the stream: force the load of every CU now
                tbl = cls(exe, dwarf, anchor if anchor is not None else -1)
        else:
            try:
                img = MachO.open(exe)
                anchor = img.symbol_address("_" + ANCHOR)
                dsym = exe.with_name(exe.name + ".dSYM") / "Contents" / "Resources" / "DWARF" / exe.name
                dwarf = MachO.open(dsym).dwarf_info() if dsym.is_file() else img.dwarf_info()
            except MachOError as e:
                raise SymbolError(str(e)) from e
            if dwarf is None:
                raise SymbolError(f"{exe}: no DWARF debug info (expected {dsym}; build with -g and run dsymutil)")
            tbl = cls(exe, dwarf, anchor if anchor is not None else -1)
        if tbl.anchor_addr == -1:
            raise SymbolError(f"{exe}: symbol {ANCHOR} not found in the symbol table")
        return tbl

    # ---- indexing ----
    def _index_cu(self, cu: Any) -> None:
        top = cu.get_top_DIE()
        a = top.attributes.get("DW_AT_name")
        cuname = Path(_s(a.value)).name if a else "?"
        for die in top.iter_children():
            if die.tag == "DW_TAG_variable":
                self._add_var(cu, cuname, die, None)
            elif die.tag == "DW_TAG_subprogram":
                n = die.attributes.get("DW_AT_name")
                if n is not None:
                    self._index_func(cu, cuname, die, _s(n.value))

    def _index_func(self, cu: Any, cuname: str, die: Any, func: str) -> None:
        for ch in die.iter_children():
            if ch.tag == "DW_TAG_variable":
                self._add_var(cu, cuname, ch, func)
            elif ch.tag in ("DW_TAG_lexical_block", "DW_TAG_inlined_subroutine"):
                self._index_func(cu, cuname, ch, func)

    def _add_var(self, cu: Any, cuname: str, die: Any, func: str | None) -> None:
        at = die.attributes
        if "DW_AT_declaration" in at or "DW_AT_location" not in at:
            return
        addr = self._static_addr(cu, at["DW_AT_location"])
        if addr is None:
            return
        spec = die.get_DIE_from_attribute("DW_AT_specification") if "DW_AT_specification" in at else None
        src = spec.attributes if spec is not None else at
        name = at.get("DW_AT_name") or src.get("DW_AT_name")
        if name is None:
            return
        external = "DW_AT_external" in at or "DW_AT_external" in src
        if func is not None:
            external = False
        self._vars.append(_Var(_s(name.value), cuname, external, func, addr, die))

    def _static_addr(self, cu: Any, loc: Any) -> int | None:
        v = loc.value
        if not isinstance(v, (list, bytes)):  # location list: not a static address
            return None
        b = bytes(v)
        if len(b) == 9 and b[0] == DW_OP_addr:
            return int.from_bytes(b[1:], "little")
        if len(b) >= 2 and b[0] == DW_OP_addrx:
            idx = shift = 0
            for byte in b[1:]:
                idx |= (byte & 0x7F) << shift
                shift += 7
            return self._dwarf.get_addr(cu, idx)
        return None

    # ---- types ----
    def _type_of(self, die: Any) -> Type:
        if "DW_AT_type" not in die.attributes:
            return UnsupportedType("void")
        return self._type_from_die(die.get_DIE_from_attribute("DW_AT_type"))

    def _type_from_die(self, die: Any) -> Type:
        t = self._types.get(die.offset)
        if t is not None:
            return t
        t = self._build_type(die)
        self._types[die.offset] = t
        return t

    def _build_type(self, die: Any) -> Type:
        tag, at = die.tag, die.attributes

        def name() -> str:
            n = at.get("DW_AT_name")
            return _s(n.value) if n else ""

        size = at["DW_AT_byte_size"].value if "DW_AT_byte_size" in at else 0
        if tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type", "DW_TAG_restrict_type",
                   "DW_TAG_atomic_type"):
            return self._type_of(die)
        if tag == "DW_TAG_base_type":
            enc = at["DW_AT_encoding"].value
            if enc == DW_ATE_boolean:
                return BoolType(size, name())
            if enc == DW_ATE_float:
                try:
                    return FloatType(size, name())
                except SymbolError as e:
                    return UnsupportedType(str(e))
            if enc in (DW_ATE_signed, DW_ATE_signed_char):
                return IntType(size, True, name())
            if enc in (DW_ATE_unsigned, DW_ATE_unsigned_char, DW_ATE_UTF):
                return IntType(size, False, name())
            return UnsupportedType(f"base type {name()} (encoding {enc:#x})")
        if tag in ("DW_TAG_pointer_type", "DW_TAG_reference_type"):
            return PtrType(size or 8)
        if tag == "DW_TAG_enumeration_type":
            enums = {
                _s(c.attributes["DW_AT_name"].value): c.attributes["DW_AT_const_value"].value
                for c in die.iter_children()
                if c.tag == "DW_TAG_enumerator"
            }
            signed = any(v < 0 for v in enums.values())
            if "DW_AT_type" in at:
                under = self._type_of(die)
                if isinstance(under, IntType):
                    signed = under.signed
                    size = size or under.size
            return EnumType(size, signed, "enum " + name() if name() else "enum", enums)
        if tag in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_class_type"):
            kind = "union" if tag == "DW_TAG_union_type" else "struct"
            st = StructType(size, name(), kind)
            self._types[die.offset] = st  # (members hold no back-references, but be safe)
            self._add_members(st, die, 0)
            return st
        if tag == "DW_TAG_array_type":
            elem = self._type_of(die)
            dims: list[int | None] = []
            for sub in die.iter_children():
                if sub.tag != "DW_TAG_subrange_type":
                    continue
                sa = sub.attributes
                if "DW_AT_count" in sa and isinstance(sa["DW_AT_count"].value, int):
                    dims.append(sa["DW_AT_count"].value)
                elif "DW_AT_upper_bound" in sa and isinstance(sa["DW_AT_upper_bound"].value, int):
                    dims.append(sa["DW_AT_upper_bound"].value + 1)
                else:
                    dims.append(None)
            t = elem
            for d in reversed(dims or [None]):
                t = ArrayType(t, d)
            return t
        return UnsupportedType(tag.removeprefix("DW_TAG_"))

    def _add_members(self, st: StructType, die: Any, base: int) -> None:
        for m in die.iter_children():
            if m.tag != "DW_TAG_member":
                continue
            ma = m.attributes
            off = ma["DW_AT_data_member_location"].value if "DW_AT_data_member_location" in ma else 0
            if not isinstance(off, int):
                off = 0
            mt = self._type_of(m)
            if "DW_AT_bit_size" in ma:
                mt = UnsupportedType("bit-field")
            mn = ma.get("DW_AT_name")
            if mn is None:  # anonymous struct/union member: hoist its members
                sub = m.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in ma else None
                if sub is not None and sub.tag in ("DW_TAG_structure_type", "DW_TAG_union_type"):
                    self._add_members(st, sub, base + off)
                continue
            st.members[_s(mn.value)] = (base + off, mt)

    # ---- resolution ----
    def resolve(self, expr: str) -> Resolved:
        text = expr.strip()
        m = _HEAD.match(text)
        if not m:
            raise SymbolError(f"bad expression {expr!r}")
        var = self._lookup(m["file"], m["func"], m["name"], text[: m.end()])
        addr, typ = var.addr, self._type_of(var.die)
        pos = m.end()
        while pos < len(text):
            s = _STEP.match(text, pos)
            if not s:
                raise SymbolError(f"bad expression {expr!r}: unexpected {text[pos:]!r}")
            pos = s.end()
            if s.group(1) is not None:
                if not isinstance(typ, StructType):
                    raise SymbolError(f"cannot take field '{s.group(1)}' of {typ.label} in {expr!r}")
                if s.group(1) not in typ.members:
                    raise SymbolError(f"no field '{s.group(1)}' in {typ.label}")
                off, typ = typ.members[s.group(1)]
                addr += off
            else:
                i = int(s.group(2))
                if not isinstance(typ, ArrayType):
                    raise SymbolError(f"cannot index {typ.label} in {expr!r}")
                if typ.count is None:
                    raise SymbolError(f"array bound unknown for {typ.label} in {expr!r}")
                if i >= typ.count:
                    raise SymbolError(f"index {i} out of range for {typ.label} in {expr!r}")
                addr += i * typ.elem.size
                typ = typ.elem
        return Resolved(text, addr, typ)

    def _lookup(self, file: str | None, func: str | None, name: str, spec: str) -> _Var:
        base = Path(file).name if file else None
        cands = [v for v in self._vars if v.name == name]
        if func is not None:
            cands = [v for v in cands if v.func == func]
        else:
            cands = [v for v in cands if v.func is None]
        if base is not None:
            cands = [v for v in cands if v.cu == base]
        elif func is None:
            globs = [v for v in cands if v.external]
            cands = globs or cands  # globals first, then file-scope statics
        uniq: dict[tuple[str, int], _Var] = {}
        for v in cands:
            uniq.setdefault((v.label, v.addr), v)
        cands = list(uniq.values())
        if not cands:
            raise SymbolError(f"no symbol '{spec}'")
        if len(cands) > 1:
            names = ", ".join(sorted(v.label for v in cands))
            raise SymbolError(f"ambiguous symbol '{spec}': candidates {names}")
        return cands[0]
