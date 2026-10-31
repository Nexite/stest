"""Minimal 64-bit little-endian Mach-O reader: DWARF sections and the symbol table.

Used on macOS, where debug info lives in ``<exe>.dSYM/Contents/Resources/DWARF/<exe>``
and pyelftools cannot open the container. Only what stest needs is parsed.
"""

from __future__ import annotations

import io
import struct
from dataclasses import dataclass
from pathlib import Path

from elftools.dwarf.dwarfinfo import DebugSectionDescriptor, DwarfConfig, DWARFInfo

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
N_STAB = 0xE0
N_TYPE = 0x0E
N_SECT = 0x0E

CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C


class MachOError(ValueError):
    pass


@dataclass
class Section:
    segname: str
    sectname: str
    addr: int
    size: int
    offset: int


class MachO:
    """A parsed 64-bit Mach-O image held in memory."""

    def __init__(self, data: bytes, path: Path | str = "<memory>"):
        self.data = data
        self.path = str(path)
        if len(data) < 32 or struct.unpack_from("<I", data, 0)[0] != MH_MAGIC_64:
            raise MachOError(f"{self.path}: not a 64-bit little-endian Mach-O file (fat binaries unsupported)")
        self.cputype, _sub, _ftype, ncmds, _sizeofcmds, _flags, _res = struct.unpack_from("<iiIIIII", data, 4)
        self.sections: list[Section] = []
        self.symtab: tuple[int, int, int, int] | None = None  # symoff, nsyms, stroff, strsize
        off = 32
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", data, off)
            if cmd == LC_SEGMENT_64:
                nsects = struct.unpack_from("<I", data, off + 64)[0]
                so = off + 72
                for _i in range(nsects):
                    sect, seg, addr, size, offset = struct.unpack_from("<16s16sQQI", data, so)
                    self.sections.append(
                        Section(
                            seg.split(b"\0")[0].decode(),
                            sect.split(b"\0")[0].decode(),
                            addr,
                            size,
                            offset,
                        )
                    )
                    so += 80
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from("<IIII", data, off + 8)
            off += cmdsize

    @classmethod
    def open(cls, path: Path | str) -> MachO:
        return cls(Path(path).read_bytes(), path)

    def section_bytes(self, segname: str, sectname: str) -> bytes | None:
        for s in self.sections:
            if s.segname == segname and s.sectname == sectname:
                return self.data[s.offset : s.offset + s.size]
        return None

    def symbol_address(self, name: str) -> int | None:
        """Value of a defined symbol; ``name`` is the Mach-O name (C names have a leading '_')."""
        if self.symtab is None:
            raise MachOError(f"{self.path}: no LC_SYMTAB")
        symoff, nsyms, stroff, strsize = self.symtab
        strtab = self.data[stroff : stroff + strsize]
        want = name.encode()
        for i in range(nsyms):
            n_strx, n_type, _n_sect, _n_desc, n_value = struct.unpack_from("<IBBHQ", self.data, symoff + 16 * i)
            if n_type & N_STAB or (n_type & N_TYPE) != N_SECT:
                continue
            end = strtab.find(b"\0", n_strx)
            if strtab[n_strx:end] == want:
                return n_value
        return None

    def dwarf_info(self) -> DWARFInfo | None:
        """A pyelftools DWARFInfo over this image's __DWARF sections (None if it has none)."""

        def sec(name: str) -> DebugSectionDescriptor | None:
            raw = self.section_bytes("__DWARF", "__" + name)
            if raw is None:
                return None
            return DebugSectionDescriptor(io.BytesIO(raw), "." + name, None, len(raw), 0)

        info = sec("debug_info")
        if info is None:
            return None
        arch = {CPU_TYPE_X86_64: "x64", CPU_TYPE_ARM64: "AArch64"}.get(self.cputype, "<unknown>")
        cfg = DwarfConfig(little_endian=True, machine_arch=arch, default_address_size=8)
        # Mach-O truncates section names to 16 chars: "__debug_str_offs".
        return DWARFInfo(
            config=cfg,
            debug_info_sec=info,
            debug_aranges_sec=sec("debug_aranges"),
            debug_abbrev_sec=sec("debug_abbrev"),
            debug_frame_sec=sec("debug_frame"),
            eh_frame_sec=None,
            debug_str_sec=sec("debug_str"),
            debug_loc_sec=sec("debug_loc"),
            debug_ranges_sec=sec("debug_ranges"),
            debug_line_sec=sec("debug_line"),
            debug_pubtypes_sec=sec("debug_pubtypes"),
            debug_pubnames_sec=sec("debug_pubnames"),
            debug_addr_sec=sec("debug_addr"),
            debug_str_offsets_sec=sec("debug_str_offs"),
            debug_line_str_sec=sec("debug_line_str"),
            debug_loclists_sec=sec("debug_loclists"),
            debug_rnglists_sec=sec("debug_rnglists"),
            debug_sup_sec=None,
            gnu_debugaltlink_sec=None,
            debug_types_sec=None,
        )
