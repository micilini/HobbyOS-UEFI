#!/usr/bin/env python3
"""Collect and replay evidence for the kernel ELF section layout."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import struct
import subprocess
import sys
from typing import Any, Iterable


PAGE_SIZE = 4096
KERNEL_P_BASE = 0x02000000
VIRT_TO_PHYS_OFFSET = 0xFFFFFFFF80000000
KERNEL_V_BASE = 0xFFFFFFFF82000000

ET_REL = 1
ET_EXEC = 2
EM_X86_64 = 62
PT_LOAD = 1
PT_GNU_STACK = 0x6474E551
SHT_NULL = 0
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHN_UNDEF = 0

EXPECTED_OUTPUT_SECTIONS = {
    "",
    ".boot_text",
    ".boot_data",
    ".text",
    ".trampoline",
    ".rodata",
    ".cpu_msr_fixup",
    ".data",
    ".bss",
    ".symtab",
    ".strtab",
    ".shstrtab",
}

BOUNDARIES = (
    "_text_start",
    "_text_end",
    "_rodata_start",
    "_rodata_end",
    "_data_start",
)

PRESERVED_SYMBOLS = (
    "_kernel_start",
    "_kernel_end",
    "_bss_start",
    "_bss_end",
    "_phys_end_boot",
    "_trampoline_start",
    "_trampoline_end",
)

MSR_FIXUP_SYMBOLS = (
    "_cpu_msr_fixup_start",
    "_cpu_msr_fixup_end",
)

TRAMPOLINE_PATCH_SYMBOLS = (
    "smp_trampoline_entry",
    "smp_trampoline_gdt_start",
    "smp_trampoline_gdtr",
    "smp_trampoline_cr3",
    "smp_trampoline_cr4",
    "smp_trampoline_efer",
    "smp_trampoline_stack",
    "smp_trampoline_entry_ptr",
    "smp_trampoline_x2apic_flag",
    "smp_trampoline_base",
)

REQUIRED_CANDIDATE_ROLES = {
    "assertions-debug",
    "formatting-test",
    "panic-instrumented",
    "foundation-framed",
    "production-debug-off",
}


class ValidationError(Exception):
    def __init__(self, code: str, detail: str = "") -> None:
        self.code = code
        self.detail = detail
        super().__init__(f"{code}: {detail}" if detail else code)


def fail(code: str, detail: str = "") -> None:
    raise ValidationError(code, detail)


def now() -> str:
    return datetime.datetime.now().astimezone().isoformat()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        fail("EVIDENCE_FILE_UNREADABLE", f"{path}: {exc}")
    return digest.hexdigest()


def regular_file(path: Path, code: str = "EVIDENCE_FILE_INVALID") -> None:
    try:
        mode = path.lstat().st_mode
    except OSError as exc:
        fail(code, f"{path}: {exc}")
    if not stat.S_ISREG(mode):
        fail(code, f"not a regular file: {path}")


def checked_relative(root: Path, value: str, code: str = "EVIDENCE_PATH_INVALID") -> Path:
    pure = PurePosixPath(value)
    if pure.is_absolute() or not pure.parts or any(part in ("", ".", "..") for part in pure.parts):
        fail(code, value)
    path = root.joinpath(*pure.parts)
    try:
        path.resolve().relative_to(root.resolve())
    except (OSError, ValueError):
        fail(code, value)
    return path


def load_json(path: Path, code: str = "EVIDENCE_JSON_INVALID") -> dict[str, Any]:
    regular_file(path)
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        fail(code, f"{path}: {exc}")
    if not isinstance(value, dict):
        fail(code, f"top-level value is not an object: {path}")
    return value


def require_hash(path: Path, expected: Any, code: str = "EVIDENCE_HASH_MISMATCH") -> None:
    if not isinstance(expected, str) or len(expected) != 64:
        fail(code, f"invalid expected hash for {path}")
    observed = sha256_file(path)
    if observed != expected:
        fail(code, f"{path}: expected={expected} observed={observed}")


def c_string(data: bytes, offset: int, code: str) -> str:
    if offset < 0 or offset >= len(data):
        fail(code, f"string offset {offset} outside table of {len(data)} bytes")
    end = data.find(b"\0", offset)
    if end < 0:
        fail(code, "unterminated string")
    try:
        return data[offset:end].decode("utf-8")
    except UnicodeDecodeError as exc:
        fail(code, f"invalid UTF-8 section/symbol name: {exc}")


class ElfFile:
    EHDR = struct.Struct("<16sHHIQQQIHHHHHH")
    PHDR = struct.Struct("<IIQQQQQQ")
    SHDR = struct.Struct("<IIQQQQIIQQ")
    SYM = struct.Struct("<IBBHQQ")

    def __init__(self, data: bytes, label: str = "ELF") -> None:
        self.data = data
        self.label = label
        self.header: dict[str, int] = {}
        self.programs: list[dict[str, int]] = []
        self.sections: list[dict[str, Any]] = []
        self._symbols: list[dict[str, Any]] | None = None
        self._parse()

    @classmethod
    def from_path(cls, path: Path) -> "ElfFile":
        regular_file(path, "ELF_FILE_INVALID")
        try:
            data = path.read_bytes()
        except OSError as exc:
            fail("ELF_FILE_INVALID", f"{path}: {exc}")
        return cls(data, str(path))

    def _slice(self, offset: int, size: int, code: str) -> bytes:
        if offset < 0 or size < 0 or offset > len(self.data) or size > len(self.data) - offset:
            fail(code, f"{self.label}: offset={offset} size={size} file={len(self.data)}")
        return self.data[offset:offset + size]

    def _parse(self) -> None:
        if len(self.data) < self.EHDR.size:
            fail("ELF_TRUNCATED", f"{self.label}: header")
        values = self.EHDR.unpack_from(self.data)
        ident = values[0]
        if ident[:4] != b"\x7fELF" or ident[4] != 2 or ident[5] != 1 or ident[6] != 1:
            fail("ELF_HEADER_INVALID", f"{self.label}: magic/class/data/version")
        keys = (
            "type", "machine", "version", "entry", "phoff", "shoff", "flags",
            "ehsize", "phentsize", "phnum", "shentsize", "shnum", "shstrndx",
        )
        self.header = dict(zip(keys, values[1:]))
        if self.header["machine"] != EM_X86_64 or self.header["version"] != 1:
            fail("ELF_HEADER_INVALID", f"{self.label}: machine/version")
        if self.header["ehsize"] != self.EHDR.size:
            fail("ELF_HEADER_INVALID", f"{self.label}: e_ehsize")
        if self.header["phnum"] and self.header["phentsize"] != self.PHDR.size:
            fail("ELF_HEADER_INVALID", f"{self.label}: e_phentsize")
        if self.header["shnum"] == 0 or self.header["shentsize"] != self.SHDR.size:
            fail("ELF_HEADER_INVALID", f"{self.label}: unsupported section count/entry size")
        if self.header["shstrndx"] >= self.header["shnum"]:
            fail("ELF_HEADER_INVALID", f"{self.label}: e_shstrndx")

        self._slice(
            self.header["phoff"], self.header["phnum"] * self.header["phentsize"],
            "ELF_PROGRAM_BOUNDS_INVALID",
        )
        for index in range(self.header["phnum"]):
            offset = self.header["phoff"] + index * self.header["phentsize"]
            fields = self.PHDR.unpack_from(self.data, offset)
            keys = ("type", "flags", "offset", "vaddr", "paddr", "filesz", "memsz", "align")
            record = dict(zip(keys, fields))
            record["index"] = index
            self.programs.append(record)

        self._slice(
            self.header["shoff"], self.header["shnum"] * self.header["shentsize"],
            "ELF_SECTION_BOUNDS_INVALID",
        )
        raw_sections: list[dict[str, Any]] = []
        for index in range(self.header["shnum"]):
            offset = self.header["shoff"] + index * self.header["shentsize"]
            fields = self.SHDR.unpack_from(self.data, offset)
            keys = ("name_offset", "type", "flags", "addr", "offset", "size", "link", "info", "align", "entsize")
            record = dict(zip(keys, fields))
            record["index"] = index
            record["header_offset"] = offset
            if record["type"] != SHT_NOBITS and record["size"]:
                self._slice(record["offset"], record["size"], "ELF_SECTION_BOUNDS_INVALID")
            raw_sections.append(record)
        shstr = raw_sections[self.header["shstrndx"]]
        if shstr["type"] != SHT_STRTAB:
            fail("ELF_HEADER_INVALID", f"{self.label}: shstrtab type")
        shstr_data = self._slice(shstr["offset"], shstr["size"], "ELF_SECTION_BOUNDS_INVALID")
        for section in raw_sections:
            section["name"] = c_string(shstr_data, section["name_offset"], "ELF_SECTION_NAME_INVALID")
        self.sections = raw_sections

    def section_all(self, name: str) -> list[dict[str, Any]]:
        return [section for section in self.sections if section["name"] == name]

    def section(self, name: str) -> dict[str, Any]:
        matches = self.section_all(name)
        if not matches:
            fail("SECTION_MISSING", f"{self.label}: {name}")
        if len(matches) != 1:
            fail("SECTION_DUPLICATE", f"{self.label}: {name}")
        return matches[0]

    def section_data(self, section: dict[str, Any]) -> bytes:
        if section["type"] == SHT_NOBITS:
            return b""
        return self._slice(section["offset"], section["size"], "ELF_SECTION_BOUNDS_INVALID")

    def symbols(self) -> list[dict[str, Any]]:
        if self._symbols is not None:
            return self._symbols
        result: list[dict[str, Any]] = []
        for section in self.sections:
            if section["type"] != SHT_SYMTAB:
                continue
            if section["entsize"] != self.SYM.size or section["size"] % self.SYM.size:
                fail("ELF_SYMBOL_TABLE_INVALID", f"{self.label}: {section['name']}")
            if section["link"] >= len(self.sections):
                fail("ELF_SYMBOL_TABLE_INVALID", f"{self.label}: linked string table")
            strings = self.sections[section["link"]]
            if strings["type"] != SHT_STRTAB:
                fail("ELF_SYMBOL_TABLE_INVALID", f"{self.label}: linked section is not STRTAB")
            str_data = self.section_data(strings)
            raw = self.section_data(section)
            for index in range(section["size"] // self.SYM.size):
                entry_offset = section["offset"] + index * self.SYM.size
                name_offset, info, other, shndx, value, size = self.SYM.unpack_from(self.data, entry_offset)
                name = c_string(str_data, name_offset, "ELF_SYMBOL_NAME_INVALID")
                result.append({
                    "name": name,
                    "name_offset": name_offset,
                    "info": info,
                    "other": other,
                    "shndx": shndx,
                    "value": value,
                    "size": size,
                    "entry_offset": entry_offset,
                    "string_file_offset": strings["offset"] + name_offset,
                })
        if not result:
            fail("ELF_SYMBOL_TABLE_INVALID", f"{self.label}: no SYMTAB")
        self._symbols = result
        return result

    def symbol(self, name: str) -> dict[str, Any]:
        matches = [symbol for symbol in self.symbols() if symbol["name"] == name and symbol["shndx"] != SHN_UNDEF]
        if not matches:
            fail("SYMBOL_MISSING", f"{self.label}: {name}")
        if len(matches) != 1:
            fail("SYMBOL_DUPLICATE", f"{self.label}: {name}")
        return matches[0]


def pages(start: int, end: int) -> set[int]:
    if end <= start:
        return set()
    return set(range(start // PAGE_SIZE, (end - 1) // PAGE_SIZE + 1))


def exact_section_names(elf: ElfFile) -> None:
    names = [section["name"] for section in elf.sections]
    unexpected = sorted(set(names) - EXPECTED_OUTPUT_SECTIONS)
    if unexpected:
        fail("UNEXPECTED_SECTION", f"{elf.label}: {','.join(unexpected)}")
    for name in EXPECTED_OUTPUT_SECTIONS - {""}:
        if names.count(name) != 1:
            fail("SECTION_SET_INVALID", f"{elf.label}: {name} count={names.count(name)}")
    if ".extra" in names:
        fail("CATCH_ALL_OUTPUT_PRESENT", elf.label)


def validate_loads(elf: ElfFile, allocated_sections: Iterable[dict[str, Any]]) -> dict[str, Any]:
    loads = [program for program in elf.programs if program["type"] == PT_LOAD]
    if len(loads) != 4:
        fail("LOAD_SEGMENT_COUNT_INVALID", f"{elf.label}: {len(loads)}")
    physical_ranges: list[tuple[int, int, int]] = []
    for program in loads:
        if program["filesz"] > program["memsz"]:
            fail("LOAD_SEGMENT_INVALID", f"{elf.label}: filesz > memsz in {program['index']}")
        if program["offset"] > len(elf.data) or program["filesz"] > len(elf.data) - program["offset"]:
            fail("LOAD_SEGMENT_INVALID", f"{elf.label}: file range in {program['index']}")
        if program["align"] != PAGE_SIZE:
            fail("LOAD_SEGMENT_INVALID", f"{elf.label}: p_align in {program['index']}")
        if program["vaddr"] % PAGE_SIZE != program["offset"] % PAGE_SIZE:
            fail("LOAD_SEGMENT_INVALID", f"{elf.label}: VMA/file congruence in {program['index']}")
        if program["paddr"] % PAGE_SIZE != program["offset"] % PAGE_SIZE:
            fail("LOAD_SEGMENT_INVALID", f"{elf.label}: LMA/file congruence in {program['index']}")
        if program["vaddr"] >= KERNEL_V_BASE:
            if program["vaddr"] - VIRT_TO_PHYS_OFFSET != program["paddr"]:
                fail("LOAD_ADDRESS_INVALID", f"{elf.label}: high-half delta in {program['index']}")
        elif program["vaddr"] != program["paddr"]:
            fail("LOAD_ADDRESS_INVALID", f"{elf.label}: low segment VMA/LMA in {program['index']}")
        end = program["paddr"] + program["memsz"]
        if end < program["paddr"]:
            fail("LOAD_SEGMENT_INVALID", f"{elf.label}: physical overflow")
        physical_ranges.append((program["paddr"], end, program["index"]))
    physical_ranges.sort()
    for left, right in zip(physical_ranges, physical_ranges[1:]):
        if left[1] > right[0]:
            fail("LOAD_SEGMENT_OVERLAP", f"{elf.label}: {left[2]} and {right[2]}")

    for section in allocated_sections:
        if section["size"] == 0:
            continue
        candidates = []
        for program in loads:
            section_end = section["addr"] + section["size"]
            program_end = program["vaddr"] + program["memsz"]
            if section["addr"] >= program["vaddr"] and section_end <= program_end:
                candidates.append(program)
        if len(candidates) != 1:
            fail("LOAD_SECTION_MAPPING_INVALID", f"{elf.label}: {section['name']} matches {len(candidates)} PT_LOAD")

    expected_groups = [
        ({".boot_text"}, 0x5),
        ({".text", ".trampoline"}, 0x5),
        ({".rodata", ".cpu_msr_fixup"}, 0x4),
        ({".data", ".bss"}, 0x6),
    ]
    observed_groups: list[tuple[set[str], int]] = []
    for program in loads:
        names = set()
        for section in allocated_sections:
            if section["size"] == 0:
                continue
            if section["addr"] >= program["vaddr"] and section["addr"] + section["size"] <= program["vaddr"] + program["memsz"]:
                names.add(section["name"])
        observed_groups.append((names, program["flags"]))
    for names, flags in expected_groups:
        if (names, flags) not in observed_groups:
            fail("LOAD_FLAGS_OR_GROUP_INVALID", f"{elf.label}: names={sorted(names)} flags={flags}")
    return {
        "load_count": len(loads),
        "physical_min": min(item[0] for item in physical_ranges),
        "physical_max": max(item[1] for item in physical_ranges),
        "groups": [{"sections": sorted(names), "flags": flags} for names, flags in observed_groups],
    }


def trampoline_state(elf: ElfFile) -> dict[str, Any]:
    section = elf.section(".trampoline")
    start = elf.symbol("_trampoline_start")["value"]
    end = elf.symbol("_trampoline_end")["value"]
    if start != section["addr"] or end != section["addr"] + section["size"]:
        fail("TRAMPOLINE_BOUNDARY_INVALID", elf.label)
    if section["size"] != 0x178:
        fail("TRAMPOLINE_SIZE_INVALID", f"{elf.label}: {section['size']}")
    offsets: dict[str, int] = {}
    for name in TRAMPOLINE_PATCH_SYMBOLS:
        value = elf.symbol(name)["value"]
        if value < start or value >= end:
            fail("TRAMPOLINE_PATCH_OUTSIDE", f"{elf.label}: {name}=0x{value:x}")
        offsets[name] = value - start
    return {
        "start": start,
        "end": end,
        "size": section["size"],
        "sha256": sha256_bytes(elf.section_data(section)),
        "patch_offsets": offsets,
    }


def msr_fixup_state(elf: ElfFile) -> dict[str, Any]:
    section = elf.section(".cpu_msr_fixup")
    start = elf.symbol("_cpu_msr_fixup_start")["value"]
    end = elf.symbol("_cpu_msr_fixup_end")["value"]
    if section["type"] != SHT_PROGBITS or section["flags"] != SHF_ALLOC:
        fail("MSR_FIXUP_SECTION_INVALID", f"{elf.label}: type/flags")
    if section["align"] < 16 or section["addr"] % 16:
        fail("MSR_FIXUP_SECTION_INVALID", f"{elf.label}: alignment")
    if start != section["addr"] or end != section["addr"] + section["size"]:
        fail("MSR_FIXUP_BOUNDARY_INVALID", elf.label)
    if section["size"] == 0 or section["size"] % 16:
        fail("MSR_FIXUP_SIZE_INVALID", f"{elf.label}: {section['size']}")

    text = elf.section(".text")
    text_bytes = elf.section_data(text)
    entries = []
    sites = set()
    payload = elf.section_data(section)
    for offset in range(0, len(payload), 16):
        site, fixup = struct.unpack_from("<QQ", payload, offset)
        if site in sites:
            fail("MSR_FIXUP_SITE_DUPLICATE", f"{elf.label}: 0x{site:x}")
        sites.add(site)
        if not (text["addr"] <= site < text["addr"] + text["size"] and
                text["addr"] <= fixup < text["addr"] + text["size"] and
                site != fixup):
            fail("MSR_FIXUP_TARGET_INVALID", f"{elf.label}: site=0x{site:x} fixup=0x{fixup:x}")
        instruction_offset = site - text["addr"]
        opcode = text_bytes[instruction_offset:instruction_offset + 2]
        if opcode not in (b"\x0f\x32", b"\x0f\x30"):
            fail("MSR_FIXUP_SITE_OPCODE_INVALID", f"{elf.label}: site=0x{site:x}")
        entries.append({
            "instruction": site,
            "fixup": fixup,
            "operation": "read" if opcode == b"\x0f\x32" else "write",
        })
    operations = {entry["operation"] for entry in entries}
    if operations != {"read", "write"}:
        fail("MSR_FIXUP_OPERATION_SET_INVALID", f"{elf.label}: {sorted(operations)}")
    return {
        "start": start,
        "end": end,
        "size": section["size"],
        "entry_count": len(entries),
        "entries": entries,
        "sha256": sha256_bytes(payload),
    }


def validate_layout(elf: ElfFile) -> dict[str, Any]:
    if elf.header["type"] != ET_EXEC:
        fail("ELF_TYPE_INVALID", f"{elf.label}: {elf.header['type']}")
    if elf.header["entry"] != KERNEL_P_BASE:
        fail("ELF_ENTRY_INVALID", f"{elf.label}: 0x{elf.header['entry']:x}")
    exact_section_names(elf)

    boot_text = elf.section(".boot_text")
    boot_data = elf.section(".boot_data")
    text = elf.section(".text")
    trampoline = elf.section(".trampoline")
    rodata = elf.section(".rodata")
    msr_fixup = elf.section(".cpu_msr_fixup")
    data = elf.section(".data")
    bss = elf.section(".bss")
    if boot_text["type"] != SHT_PROGBITS or boot_text["flags"] != SHF_ALLOC | SHF_EXECINSTR or boot_text["addr"] != KERNEL_P_BASE:
        fail("BOOT_TEXT_INVALID", elf.label)
    if boot_data["type"] != SHT_PROGBITS or boot_data["flags"] != 0 or boot_data["align"] != PAGE_SIZE:
        fail("BOOT_DATA_INVALID", elf.label)
    expected_types_flags = (
        (text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR),
        (trampoline, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR),
        (rodata, SHT_PROGBITS, SHF_ALLOC),
        (msr_fixup, SHT_PROGBITS, SHF_ALLOC),
        (data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE),
        (bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE),
    )
    for section, section_type, flags in expected_types_flags:
        if section["type"] != section_type or section["flags"] != flags:
            fail("SECTION_FLAGS_OR_TYPE_INVALID", f"{elf.label}: {section['name']}")
    if rodata["addr"] % PAGE_SIZE or data["addr"] % PAGE_SIZE:
        fail("SECTION_ALIGNMENT_INVALID", f"{elf.label}: rodata=0x{rodata['addr']:x} data=0x{data['addr']:x}")

    boundary_values = {name: elf.symbol(name)["value"] for name in BOUNDARIES}
    if boundary_values["_text_start"] != text["addr"]:
        fail("BOUNDARY_START_INVALID", f"{elf.label}: _text_start")
    if boundary_values["_text_end"] != text["addr"] + text["size"]:
        fail("BOUNDARY_END_INVALID", f"{elf.label}: _text_end")
    if boundary_values["_rodata_start"] != rodata["addr"]:
        fail("BOUNDARY_START_INVALID", f"{elf.label}: _rodata_start")
    if boundary_values["_rodata_end"] != rodata["addr"] + rodata["size"]:
        fail("BOUNDARY_END_INVALID", f"{elf.label}: _rodata_end")
    if boundary_values["_data_start"] != data["addr"]:
        fail("BOUNDARY_START_INVALID", f"{elf.label}: _data_start")

    intervals = {
        "text": (text["addr"], text["addr"] + text["size"]),
        "rodata": (rodata["addr"], rodata["addr"] + rodata["size"]),
        "data": (data["addr"], data["addr"] + data["size"]),
    }
    page_sets = {name: pages(*interval) for name, interval in intervals.items()}
    for left, right in (("text", "rodata"), ("text", "data"), ("rodata", "data")):
        overlap = page_sets[left] & page_sets[right]
        if overlap:
            fail("REGION_PAGE_OVERLAP", f"{elf.label}: {left}/{right} pages={sorted(overlap)}")
    if not (text["addr"] + text["size"] <= trampoline["addr"] < trampoline["addr"] + trampoline["size"] <= rodata["addr"] <= rodata["addr"] + rodata["size"] <= msr_fixup["addr"] <= msr_fixup["addr"] + msr_fixup["size"] <= data["addr"] <= bss["addr"]):
        fail("SECTION_ORDER_INVALID", elf.label)

    preserved = {name: elf.symbol(name)["value"] for name in PRESERVED_SYMBOLS}
    if preserved["_kernel_start"] != text["addr"]:
        fail("KERNEL_BOUNDARY_INVALID", f"{elf.label}: _kernel_start")
    if preserved["_bss_start"] != bss["addr"] or preserved["_bss_end"] != bss["addr"] + bss["size"]:
        fail("BSS_BOUNDARY_INVALID", elf.label)
    if preserved["_kernel_end"] != preserved["_bss_end"]:
        fail("KERNEL_BOUNDARY_INVALID", f"{elf.label}: _kernel_end")
    if preserved["_phys_end_boot"] != KERNEL_P_BASE + boot_text["size"] + (PAGE_SIZE - boot_text["size"] % PAGE_SIZE) % PAGE_SIZE + boot_data["size"]:
        fail("BOOTSTRAP_BOUNDARY_INVALID", elf.label)

    trampoline_result = trampoline_state(elf)
    msr_fixup_result = msr_fixup_state(elf)
    allocated = [section for section in elf.sections if section["flags"] & SHF_ALLOC]
    load_result = validate_loads(elf, allocated)
    return {
        "elf_bytes": len(elf.data),
        "elf_sha256": sha256_bytes(elf.data),
        "entry": elf.header["entry"],
        "sections": {
            section["name"]: {
                "address": section["addr"], "size": section["size"],
                "flags": section["flags"], "type": section["type"], "align": section["align"],
            }
            for section in (boot_text, boot_data, text, trampoline, rodata, msr_fixup, data, bss)
        },
        "boundaries": boundary_values | preserved,
        "padding": {
            "text_to_trampoline": trampoline["addr"] - (text["addr"] + text["size"]),
            "trampoline_to_rodata": rodata["addr"] - (trampoline["addr"] + trampoline["size"]),
            "rodata_to_msr_fixup": msr_fixup["addr"] - (rodata["addr"] + rodata["size"]),
            "msr_fixup_to_data": data["addr"] - (msr_fixup["addr"] + msr_fixup["size"]),
            "data_to_bss": bss["addr"] - (data["addr"] + data["size"]),
        },
        "trampoline": trampoline_result,
        "msr_fixup": msr_fixup_result,
        "loads": load_result,
    }


def validate_legacy_layout(elf: ElfFile) -> dict[str, Any]:
    if elf.header["type"] != ET_EXEC or elf.header["entry"] != KERNEL_P_BASE:
        fail("LEGACY_ELF_IDENTITY_INVALID", elf.label)
    for name in (".boot_text", ".boot_data", ".text", ".trampoline", ".rodata", ".data", ".bss", ".extra"):
        elf.section(name)
    return {
        "elf_bytes": len(elf.data),
        "elf_sha256": sha256_bytes(elf.data),
        "trampoline": trampoline_state(elf),
        "sections": {
            name: {
                "address": elf.section(name)["addr"], "size": elf.section(name)["size"],
                "flags": elf.section(name)["flags"], "type": elf.section(name)["type"],
                "align": elf.section(name)["align"],
            }
            for name in (".boot_text", ".boot_data", ".text", ".trampoline", ".rodata", ".data", ".bss", ".extra")
        },
    }


def compare_trampoline(before: ElfFile, after: ElfFile) -> dict[str, Any]:
    left = trampoline_state(before)
    right = trampoline_state(after)
    if left["start"] != right["start"]:
        fail("TRAMPOLINE_START_CHANGED", f"before=0x{left['start']:x} after=0x{right['start']:x}")
    if left["end"] != right["end"] or left["size"] != right["size"]:
        fail("TRAMPOLINE_SIZE_CHANGED", f"before={left['size']} after={right['size']}")
    if left["sha256"] != right["sha256"]:
        fail("TRAMPOLINE_BYTES_CHANGED", f"before={left['sha256']} after={right['sha256']}")
    if left["patch_offsets"] != right["patch_offsets"]:
        fail("TRAMPOLINE_PATCH_OFFSETS_CHANGED", "paired offsets differ")
    return {"before": left, "after": right, "status": "PASS"}


def classify_input_section(name: str, section_type: int, size: int) -> tuple[str, str]:
    if name == "":
        return "elf-metadata", "none"
    if name == ".boot_text":
        return "bootstrap-code", ".boot_text"
    if name == ".boot_data":
        return "bootstrap-storage", ".boot_data"
    if name == ".smp_trampoline_blob":
        return "trampoline-code-and-patch-data", ".trampoline"
    if name == ".text" or name.startswith(".text."):
        return "kernel-code", ".text"
    if name == ".rodata" or name.startswith(".rodata."):
        return "kernel-read-only-data", ".rodata"
    if name == ".cpu_msr_fixup":
        return "cpu-msr-recovery-table", ".cpu_msr_fixup"
    if name == ".data" or name.startswith(".data."):
        return "kernel-mutable-data", ".data"
    if name == ".bss" or name.startswith(".bss."):
        return "kernel-zero-fill", ".bss"
    if name == ".eh_frame" or name.startswith(".eh_frame."):
        return "unwind-metadata-unused-at-runtime", "/DISCARD/"
    if name == ".comment" or name.startswith(".comment."):
        return "compiler-identification-metadata", "/DISCARD/"
    if name == ".note" or name.startswith(".note."):
        return "toolchain-note-metadata", "/DISCARD/"
    if section_type in (SHT_REL, SHT_RELA) or name.startswith(".rel.") or name.startswith(".rela."):
        return "link-time-relocation", "consumed-by-linker"
    if name in (".symtab", ".strtab", ".shstrtab"):
        return "elf-object-metadata", "linker-input-metadata"
    if size == 0 and name in (".got", ".got.plt", ".igot.plt", ".iplt"):
        return "zero-size-linker-synthetic", "no-output-payload"
    return "unexpected", "unclassified"


def command_record(args: list[str], cwd: Path, stem: Path) -> subprocess.CompletedProcess[bytes]:
    started = now()
    completed = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    ended = now()
    stem.parent.mkdir(parents=True, exist_ok=True)
    stem.with_suffix(".stdout.log").write_bytes(completed.stdout)
    stem.with_suffix(".stderr.log").write_bytes(completed.stderr)
    stem.with_suffix(".exit-code.txt").write_text(str(completed.returncode) + "\n")
    stem.with_suffix(".json").write_text(json.dumps({
        "argv": args,
        "cwd": os.fspath(cwd),
        "started_at": started,
        "ended_at": ended,
        "exit_code": completed.returncode,
    }, indent=2, sort_keys=True) + "\n")
    if completed.returncode:
        fail("COLLECTION_COMMAND_FAILED", f"{stem.name}: {completed.returncode}")
    return completed


def repo_relative(repo: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(repo.resolve()).as_posix()
    except ValueError:
        fail("COLLECTION_PATH_OUTSIDE_REPOSITORY", str(path))


def copy_regular(source: Path, target: Path) -> None:
    regular_file(source)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    if sha256_file(source) != sha256_file(target):
        fail("COLLECTION_COPY_MISMATCH", str(source))


def write_tool_reports(repo: Path, elf_path: Path, directory: Path, commands: Path, prefix: str) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    tools = (
        (["readelf", "-hW", os.fspath(elf_path)], "readelf-header.txt"),
        (["readelf", "-SW", os.fspath(elf_path)], "readelf-sections.txt"),
        (["readelf", "-lW", os.fspath(elf_path)], "readelf-segments.txt"),
        (["objdump", "-h", os.fspath(elf_path)], "objdump-sections.txt"),
        (["size", "-A", os.fspath(elf_path)], "size-A.txt"),
        (["nm", "-n", os.fspath(elf_path)], "nm-n.txt"),
    )
    for argv, name in tools:
        result = command_record(argv, repo, commands / f"{prefix}-{name.replace('.', '-')}")
        (directory / name).write_bytes(result.stdout)


def make_object_list(repo: Path, common: list[str], commands: Path, profile: str) -> list[str]:
    rule = 'print-kernel-objs:;@printf "%s\\n" "$(KERNEL_OBJS)"'
    result = command_record(
        ["make", "--no-print-directory", "-s", *common, f"--eval={rule}", "print-kernel-objs"],
        repo,
        commands / f"{profile}-object-list",
    )
    names = result.stdout.decode("utf-8").split()
    if not names or len(names) != len(set(names)):
        fail("COLLECTION_OBJECT_LIST_INVALID", profile)
    return names


def collect_core(repo: Path, evidence: Path, base_commit: str, references: list[str]) -> None:
    if evidence.exists():
        unexpected = [item.name for item in evidence.iterdir() if item.name != "preflight"]
        if unexpected:
            fail("COLLECTION_DIRECTORY_NOT_EMPTY", f"{evidence}: {','.join(sorted(unexpected))}")
    for name in ("input-inventory", "pairs", "orphan-probe", "link-map", "commands", "layout-summary"):
        (evidence / name).mkdir(parents=True, exist_ok=True)
    commands = evidence / "commands"

    base_script_result = command_record(
        ["git", "show", f"{base_commit}:kernel/link.ld"], repo, commands / "base-link-script"
    )
    old_script = evidence / "input-inventory/link-input.ld"
    old_script.write_bytes(base_script_result.stdout)
    new_script = evidence / "input-inventory/link-current.ld"
    copy_regular(repo / "kernel/link.ld", new_script)
    old_text = old_script.read_text(encoding="utf-8")
    new_text = new_script.read_text(encoding="utf-8")
    if "*(.*)" not in old_text:
        fail("COLLECTION_BASE_SCRIPT_INVALID", "base script lacks catch-all")
    if "*(.*)" in new_text:
        fail("CATCH_ALL_PRESENT", "current linker script")

    reference_records = []
    for spec in references:
        if "=" not in spec:
            fail("COLLECTION_REFERENCE_INVALID", spec)
        name, raw_path = spec.split("=", 1)
        source = (repo / raw_path).resolve()
        target = evidence / "input-inventory" / "references" / name / "kernel.elf"
        copy_regular(source, target)
        write_tool_reports(repo, target, target.parent, commands, f"reference-{name}")
        reference_records.append({
            "name": name,
            "path": target.relative_to(evidence).as_posix(),
            "sha256": sha256_file(target),
            "bytes": target.stat().st_size,
            "source_path": raw_path,
        })

    pair_records = []
    all_input_sections: list[dict[str, Any]] = []
    for profile, debug_assert in (("debug-on", 1), ("debug-off", 0)):
        common = [
            f"DEBUG_ASSERT={debug_assert}", "SELFTEST=0", "SELFTEST_AUTORUN=0",
            "ASSERT_TEST=0", "FORMAT_TEST=0", "KERNEL_EXTRA_CFLAGS=",
        ]
        command_record(["make", "clean", *common], repo, commands / f"{profile}-clean")
        command_record(["make", "-j2", "kernel.elf", *common], repo, commands / f"{profile}-canonical-link")
        object_names = make_object_list(repo, common, commands, profile)
        pair_root = evidence / "pairs" / profile
        object_root = pair_root / "objects"
        object_records = []
        copied_paths: list[str] = []
        for relative in object_names:
            source = repo / relative
            target = object_root / relative
            copy_regular(source, target)
            target_rel_repo = repo_relative(repo, target)
            copied_paths.append(target_rel_repo)
            object_record = {
                "path": relative,
                "copy_path": target.relative_to(evidence).as_posix(),
                "bytes": target.stat().st_size,
                "sha256": sha256_file(target),
            }
            object_records.append(object_record)
            object_elf = ElfFile.from_path(target)
            if object_elf.header["type"] != ET_REL:
                fail("COLLECTION_OBJECT_TYPE_INVALID", relative)
            for section in object_elf.sections:
                family, destination = classify_input_section(section["name"], section["type"], section["size"])
                item = {
                    "profile": profile,
                    "object": relative,
                    "object_sha256": object_record["sha256"],
                    "section": section["name"],
                    "type": section["type"],
                    "flags": section["flags"],
                    "size": section["size"],
                    "alignment": section["align"],
                    "classification": family,
                    "destination": destination,
                }
                all_input_sections.append(item)
                if family == "unexpected":
                    fail("UNEXPECTED_INPUT_SECTION", f"{relative}:{section['name']}")
        (pair_root / "objects.json").write_text(
            json.dumps({"schema": 1, "objects": object_records}, indent=2, sort_keys=True) + "\n"
        )
        (pair_root / "object-order.txt").write_text("\n".join(object_names) + "\n")

        canonical = pair_root / "after" / "kernel-canonical.elf"
        copy_regular(repo / "kernel.elf", canonical)
        before_elf = pair_root / "before" / "kernel.elf"
        after_elf = pair_root / "after" / "kernel.elf"
        before_elf.parent.mkdir(parents=True, exist_ok=True)
        after_elf.parent.mkdir(parents=True, exist_ok=True)
        before_map = evidence / "link-map" / f"{profile}-before.map"
        after_map = evidence / "link-map" / f"{profile}-after.map"
        common_ld = ["-static", "-Bsymbolic", "-nostdlib", "-z", "max-page-size=0x1000", "--orphan-handling=warn"]
        before_args = [
            "ld", "-T", repo_relative(repo, old_script), *common_ld,
            "--defsym=_text_start=ADDR(.text)",
            "--defsym=_text_end=ADDR(.text)+SIZEOF(.text)",
            f"-Map={repo_relative(repo, before_map)}", "-o", repo_relative(repo, before_elf), *copied_paths,
        ]
        after_args = [
            "ld", "-T", "kernel/link.ld", *common_ld,
            f"-Map={repo_relative(repo, after_map)}", "-o", repo_relative(repo, after_elf), *copied_paths,
        ]
        command_record(before_args, repo, commands / f"{profile}-before-link")
        command_record(after_args, repo, commands / f"{profile}-after-link")
        if sha256_file(after_elf) != sha256_file(canonical):
            fail("LINK_SCRIPT_RELINK_MISMATCH", profile)
        before_layout = validate_legacy_layout(ElfFile.from_path(before_elf))
        after_layout = validate_layout(ElfFile.from_path(after_elf))
        trampoline = compare_trampoline(ElfFile.from_path(before_elf), ElfFile.from_path(after_elf))
        write_tool_reports(repo, before_elf, pair_root / "before", commands, f"{profile}-before")
        write_tool_reports(repo, after_elf, pair_root / "after", commands, f"{profile}-after")
        record = {
            "profile": profile,
            "configuration": {
                "DEBUG_ASSERT": debug_assert, "SELFTEST": 0, "SELFTEST_AUTORUN": 0,
                "ASSERT_TEST": 0, "FORMAT_TEST": 0, "KERNEL_EXTRA_CFLAGS": "",
            },
            "objects": "pairs/%s/objects.json" % profile,
            "object_order": "pairs/%s/object-order.txt" % profile,
            "before": {
                "elf": "pairs/%s/before/kernel.elf" % profile,
                "sha256": sha256_file(before_elf),
                "map": "link-map/%s-before.map" % profile,
                "map_sha256": sha256_file(before_map),
                "command": "commands/%s-before-link.json" % profile,
                "layout": before_layout,
            },
            "after": {
                "elf": "pairs/%s/after/kernel.elf" % profile,
                "sha256": sha256_file(after_elf),
                "canonical_elf": "pairs/%s/after/kernel-canonical.elf" % profile,
                "canonical_sha256": sha256_file(canonical),
                "map": "link-map/%s-after.map" % profile,
                "map_sha256": sha256_file(after_map),
                "command": "commands/%s-after-link.json" % profile,
                "layout": after_layout,
            },
            "trampoline": trampoline,
            "status": "PASS",
        }
        (pair_root / "result.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
        pair_records.append(record)

    (evidence / "input-inventory/input-sections.json").write_text(
        json.dumps({"schema": 1, "sections": all_input_sections}, indent=2, sort_keys=True) + "\n"
    )

    probe_source = evidence / "orphan-probe/layout-probe.S"
    probe_source.write_text(
        '.section .layout_probe,"a",@progbits\n'
        '.global layout_probe_payload\n'
        'layout_probe_payload:\n'
        '.byte 0x48, 0x4f, 0x42, 0x42, 0x59, 0x4f, 0x53, 0x2d\n'
        '.byte 0x4c, 0x41, 0x59, 0x4f, 0x55, 0x54, 0x21, 0x7f\n'
        '.section .note.GNU-stack,"",@progbits\n',
        encoding="utf-8",
    )
    probe_object = evidence / "orphan-probe/layout-probe.o"
    probe_compile = [
        "gcc", "-ffreestanding", "-mno-red-zone", "-mgeneral-regs-only", "-mcmodel=kernel",
        "-fno-pic", "-fno-pie", "-c", repo_relative(repo, probe_source), "-o", repo_relative(repo, probe_object),
    ]
    command_record(probe_compile, repo, commands / "orphan-probe-compile")
    debug_off_objects = load_json(evidence / "pairs/debug-off/objects.json")["objects"]
    copied_paths = [repo_relative(repo, checked_relative(evidence, item["copy_path"])) for item in debug_off_objects]
    probe_elf = evidence / "orphan-probe/kernel-orphan.elf"
    probe_map = evidence / "orphan-probe/kernel-orphan.map"
    probe_args = [
        "ld", "-T", "kernel/link.ld", "-static", "-Bsymbolic", "-nostdlib", "-z", "max-page-size=0x1000",
        "--orphan-handling=warn", f"-Map={repo_relative(repo, probe_map)}", "-o", repo_relative(repo, probe_elf),
        *copied_paths, repo_relative(repo, probe_object),
    ]
    command_record(probe_args, repo, commands / "orphan-probe-link")
    probe_payload = b"HOBBYOS-LAYOUT!\x7f"
    probe_result = {
        "source": "orphan-probe/layout-probe.S",
        "source_sha256": sha256_file(probe_source),
        "object": "orphan-probe/layout-probe.o",
        "object_sha256": sha256_file(probe_object),
        "elf": "orphan-probe/kernel-orphan.elf",
        "elf_sha256": sha256_file(probe_elf),
        "map": "orphan-probe/kernel-orphan.map",
        "map_sha256": sha256_file(probe_map),
        "command": "commands/orphan-probe-link.json",
        "stderr": "commands/orphan-probe-link.stderr.log",
        "payload_hex": probe_payload.hex(),
        "expected_verdict": "UNEXPECTED_SECTION",
    }
    (evidence / "orphan-probe/result.json").write_text(json.dumps(probe_result, indent=2, sort_keys=True) + "\n")
    validate_orphan_probe(evidence, probe_result)

    status = command_record(["git", "status", "--porcelain", "--untracked-files=no"], repo, commands / "versioned-status")
    if status.stdout.strip() not in (b" M kernel/link.ld", b"M kernel/link.ld"):
        # Documentation or test files may already be edited during development; record rather than infer cleanliness.
        pass
    campaign = {
        "schema": 1,
        "kind": "section-layout-evidence",
        "collection_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
        "base_commit": base_commit,
        "collected_at": now(),
        "link_scripts": {
            "before": {"path": "input-inventory/link-input.ld", "sha256": sha256_file(old_script)},
            "after": {"path": "input-inventory/link-current.ld", "sha256": sha256_file(new_script)},
        },
        "references": reference_records,
        "pair_profiles": [record["profile"] for record in pair_records],
        "pairs": {record["profile"]: f"pairs/{record['profile']}/result.json" for record in pair_records},
        "input_sections": "input-inventory/input-sections.json",
        "orphan_probe": "orphan-probe/result.json",
        "candidates": [],
        "status": "CORE_COLLECTED",
    }
    (evidence / "campaign.json").write_text(json.dumps(campaign, indent=2, sort_keys=True) + "\n")
    print(f"SECTION_LAYOUT_COLLECTION: PASS profiles={len(pair_records)} objects={len(all_input_sections)}")


def collect_candidates(repo: Path, evidence: Path, manifest_path: Path) -> None:
    campaign_path = evidence / "campaign.json"
    campaign = load_json(campaign_path)
    manifest = load_json(manifest_path, "CANDIDATE_MANIFEST_INVALID")
    manifest_copy = evidence / "input-inventory/candidate-manifest.json"
    copy_regular(manifest_path, manifest_copy)
    entries = manifest.get("candidates")
    if not isinstance(entries, list) or not entries:
        fail("CANDIDATE_MANIFEST_INVALID", "non-empty candidates array required")
    roles: set[str] = set()
    records = []
    commands = evidence / "commands"
    for entry in entries:
        if not isinstance(entry, dict):
            fail("CANDIDATE_MANIFEST_INVALID", "candidate is not an object")
        role = entry.get("role")
        source_value = entry.get("elf")
        receipt_value = entry.get("receipt")
        configuration = entry.get("configuration")
        if not isinstance(role, str) or not role or role in roles:
            fail("CANDIDATE_MANIFEST_INVALID", f"role={role!r}")
        if not isinstance(source_value, str) or not isinstance(receipt_value, str) or not isinstance(configuration, dict):
            fail("CANDIDATE_MANIFEST_INVALID", role)
        roles.add(role)
        source = (repo / source_value).resolve()
        receipt_source = (repo / receipt_value).resolve()
        repo_relative(repo, source)
        repo_relative(repo, receipt_source)
        copy_dir = evidence / "candidates" / role
        elf_copy = copy_dir / "kernel.elf"
        receipt_copy = copy_dir / "source-receipt.json"
        copy_regular(source, elf_copy)
        copy_regular(receipt_source, receipt_copy)
        layout = validate_layout(ElfFile.from_path(elf_copy))
        receipt_text = receipt_copy.read_text(encoding="utf-8")
        elf_hash = sha256_file(elf_copy)
        if elf_hash not in receipt_text:
            fail("CANDIDATE_RECEIPT_MISMATCH", f"{role}: ELF hash absent from receipt")
        write_tool_reports(repo, elf_copy, copy_dir, commands, f"candidate-{role}")
        records.append({
            "role": role,
            "elf": elf_copy.relative_to(evidence).as_posix(),
            "elf_sha256": elf_hash,
            "source_path": source_value,
            "receipt": receipt_copy.relative_to(evidence).as_posix(),
            "receipt_sha256": sha256_file(receipt_copy),
            "source_receipt_path": receipt_value,
            "configuration": configuration,
            "layout": layout,
        })
    missing = REQUIRED_CANDIDATE_ROLES - roles
    if missing:
        fail("CANDIDATE_SET_INCOMPLETE", ",".join(sorted(missing)))
    campaign["candidates"] = records
    campaign["candidate_manifest"] = {
        "path": manifest_copy.relative_to(evidence).as_posix(),
        "sha256": sha256_file(manifest_copy),
    }
    campaign["status"] = "COMPLETE"
    campaign["candidates_collected_at"] = now()
    campaign_path.write_text(json.dumps(campaign, indent=2, sort_keys=True) + "\n")
    print(f"SECTION_LAYOUT_CANDIDATES: PASS candidates={len(records)}")


def validate_script_policy(old_script: Path, new_script: Path) -> None:
    regular_file(old_script)
    regular_file(new_script)
    old = old_script.read_text(encoding="utf-8")
    new = new_script.read_text(encoding="utf-8")
    if "*(.*)" not in old:
        fail("BASE_CATCH_ALL_MISSING", str(old_script))
    if "*(.*)" in new or "*(*)" in new:
        fail("CATCH_ALL_PRESENT", str(new_script))
    for symbol in BOUNDARIES:
        if new.count(symbol) != 1:
            fail("LINK_SCRIPT_BOUNDARY_INVALID", f"{symbol} count={new.count(symbol)}")
    for token in ("*(.eh_frame .eh_frame.*)", "*(.note .note.*)", "*(.comment .comment.*)"):
        if token not in new:
            fail("DISCARD_POLICY_MISSING", token)
    if "KEEP(*(.smp_trampoline_blob))" not in new:
        fail("TRAMPOLINE_KEEP_MISSING", str(new_script))
    if "KEEP(*(.cpu_msr_fixup))" not in new:
        fail("MSR_FIXUP_KEEP_MISSING", str(new_script))
    for symbol in MSR_FIXUP_SYMBOLS:
        if new.count(symbol) != 1:
            fail("MSR_FIXUP_LINK_SYMBOL_INVALID", f"{symbol} count={new.count(symbol)}")


def validate_map_policy(map_path: Path, expected_output: str, object_names: list[str], after: bool) -> None:
    regular_file(map_path)
    text = map_path.read_text(encoding="utf-8", errors="strict")
    if expected_output not in text:
        fail("MAP_IDENTITY_MISMATCH", f"{map_path}: output {expected_output}")
    for relative in object_names:
        if relative not in text:
            fail("MAP_OBJECT_MISSING", f"{map_path}: {relative}")
    if after:
        if ".extra" in text or "*(.*)" in text:
            fail("MAP_CATCH_ALL_PRESENT", str(map_path))
        for family in (".eh_frame", ".note.gnu.property", ".comment"):
            if family not in text:
                fail("MAP_DISCARD_FAMILY_MISSING", f"{map_path}: {family}")
    else:
        if ".extra" not in text or "*(.*)" not in text:
            fail("MAP_BASE_CATCH_ALL_MISSING", str(map_path))


def validate_pair(evidence: Path, profile: str, record_path: Path) -> dict[str, Any]:
    record = load_json(record_path, "PAIR_RESULT_INVALID")
    if record.get("profile") != profile or record.get("status") != "PASS":
        fail("PAIR_RESULT_INVALID", profile)
    objects_path = checked_relative(evidence, record.get("objects", ""))
    objects_doc = load_json(objects_path, "OBJECT_MANIFEST_INVALID")
    objects = objects_doc.get("objects")
    if not isinstance(objects, list) or not objects:
        fail("OBJECT_MANIFEST_INVALID", profile)
    order_path = checked_relative(evidence, record.get("object_order", ""))
    regular_file(order_path)
    order = order_path.read_text(encoding="utf-8").splitlines()
    if len(order) != len(objects) or len(order) != len(set(order)):
        fail("OBJECT_ORDER_INVALID", profile)
    for expected_name, item in zip(order, objects):
        if not isinstance(item, dict) or item.get("path") != expected_name:
            fail("OBJECT_ORDER_INVALID", profile)
        copy_path = checked_relative(evidence, item.get("copy_path", ""))
        regular_file(copy_path)
        if copy_path.stat().st_size != item.get("bytes"):
            fail("OBJECT_IDENTITY_MISMATCH", str(copy_path))
        require_hash(copy_path, item.get("sha256"), "OBJECT_IDENTITY_MISMATCH")

    before = record.get("before")
    after = record.get("after")
    if not isinstance(before, dict) or not isinstance(after, dict):
        fail("PAIR_RESULT_INVALID", profile)
    before_path = checked_relative(evidence, before.get("elf", ""))
    after_path = checked_relative(evidence, after.get("elf", ""))
    canonical_path = checked_relative(evidence, after.get("canonical_elf", ""))
    for path, value in ((before_path, before.get("sha256")), (after_path, after.get("sha256")), (canonical_path, after.get("canonical_sha256"))):
        require_hash(path, value, "PAIR_ELF_IDENTITY_MISMATCH")
    if sha256_file(after_path) != sha256_file(canonical_path):
        fail("LINK_SCRIPT_RELINK_MISMATCH", profile)
    before_elf = ElfFile.from_path(before_path)
    after_elf = ElfFile.from_path(after_path)
    before_layout = validate_legacy_layout(before_elf)
    after_layout = validate_layout(after_elf)
    trampoline = compare_trampoline(before_elf, after_elf)
    for name in (".boot_text", ".boot_data"):
        left = before_elf.section(name)
        right = after_elf.section(name)
        attributes = ("addr", "size", "flags", "type", "align")
        if any(left[key] != right[key] for key in attributes) or before_elf.section_data(left) != after_elf.section_data(right):
            fail("BOOTSTRAP_PAIR_CHANGED", f"{profile}: {name}")
    before_map = checked_relative(evidence, before.get("map", ""))
    after_map = checked_relative(evidence, after.get("map", ""))
    require_hash(before_map, before.get("map_sha256"), "MAP_IDENTITY_MISMATCH")
    require_hash(after_map, after.get("map_sha256"), "MAP_IDENTITY_MISMATCH")
    validate_map_policy(before_map, before.get("elf", ""), order, False)
    validate_map_policy(after_map, after.get("elf", ""), order, True)
    return {"profile": profile, "before": before_layout, "after": after_layout, "trampoline": trampoline}


def validate_orphan_binary(elf: ElfFile, payload: bytes) -> None:
    probes = elf.section_all(".layout_probe")
    if len(probes) == 1:
        section = probes[0]
        if section["size"] == 0 or elf.section_data(section) != payload:
            fail("ORPHAN_PAYLOAD_INVALID", elf.label)
        if elf.section_all(".extra"):
            fail("ORPHAN_ABSORBED", f"{elf.label}: .extra")
        try:
            validate_layout(elf)
        except ValidationError as exc:
            if exc.code != "UNEXPECTED_SECTION":
                raise
            return
        fail("ORPHAN_VERIFIER_ACCEPTED", elf.label)
    for name in (".text", ".rodata", ".data", ".extra"):
        for section in elf.section_all(name):
            if payload in elf.section_data(section):
                fail("ORPHAN_ABSORBED", f"{elf.label}: {name}")
    fail("ORPHAN_DISCARDED", elf.label)


def validate_orphan_probe(evidence: Path, record: dict[str, Any]) -> dict[str, Any]:
    source = checked_relative(evidence, record.get("source", ""))
    obj = checked_relative(evidence, record.get("object", ""))
    elf_path = checked_relative(evidence, record.get("elf", ""))
    map_path = checked_relative(evidence, record.get("map", ""))
    stderr_path = checked_relative(evidence, record.get("stderr", ""))
    for path, expected in (
        (source, record.get("source_sha256")),
        (obj, record.get("object_sha256")),
        (elf_path, record.get("elf_sha256")),
        (map_path, record.get("map_sha256")),
    ):
        require_hash(path, expected, "ORPHAN_ARTIFACT_IDENTITY_MISMATCH")
    try:
        payload = bytes.fromhex(record.get("payload_hex", ""))
    except (TypeError, ValueError):
        fail("ORPHAN_PAYLOAD_INVALID", "payload_hex")
    if not payload:
        fail("ORPHAN_PAYLOAD_INVALID", "empty payload")
    map_text = map_path.read_text(encoding="utf-8")
    stderr_text = stderr_path.read_text(encoding="utf-8")
    if ".layout_probe" not in map_text or ".layout_probe" not in stderr_text or "orphan section" not in stderr_text:
        fail("ORPHAN_DIAGNOSTIC_MISSING", str(map_path))
    elf = ElfFile.from_path(elf_path)
    validate_orphan_binary(elf, payload)
    return {"elf_sha256": sha256_file(elf_path), "payload_hex": payload.hex(), "verdict": "REJECTED_UNEXPECTED_SECTION"}


def verify_candidate(evidence: Path, record: dict[str, Any]) -> dict[str, Any]:
    role = record.get("role")
    if not isinstance(role, str):
        fail("CANDIDATE_RECORD_INVALID", "role")
    elf_path = checked_relative(evidence, record.get("elf", ""))
    receipt_path = checked_relative(evidence, record.get("receipt", ""))
    require_hash(elf_path, record.get("elf_sha256"), "CANDIDATE_IDENTITY_MISMATCH")
    require_hash(receipt_path, record.get("receipt_sha256"), "CANDIDATE_RECEIPT_MISMATCH")
    if record["elf_sha256"] not in receipt_path.read_text(encoding="utf-8"):
        fail("CANDIDATE_RECEIPT_MISMATCH", role)
    configuration = record.get("configuration")
    if not isinstance(configuration, dict):
        fail("CANDIDATE_CONFIGURATION_INVALID", role)
    if role == "production-debug-off":
        if configuration.get("DEBUG_ASSERT") != 0 or configuration.get("SELFTEST") != 0:
            fail("CANDIDATE_CONFIGURATION_INVALID", role)
    elif role in REQUIRED_CANDIDATE_ROLES:
        if configuration.get("DEBUG_ASSERT") != 1:
            fail("CANDIDATE_CONFIGURATION_INVALID", role)
    layout = validate_layout(ElfFile.from_path(elf_path))
    return {"role": role, "elf_sha256": record["elf_sha256"], "layout": layout}


def verify_all(evidence: Path, require_candidates: bool = True) -> dict[str, Any]:
    campaign = load_json(evidence / "campaign.json", "CAMPAIGN_INVALID")
    if campaign.get("kind") != "section-layout-evidence" or campaign.get("schema") != 1:
        fail("CAMPAIGN_INVALID", str(evidence))
    scripts = campaign.get("link_scripts")
    if not isinstance(scripts, dict) or not isinstance(scripts.get("before"), dict) or not isinstance(scripts.get("after"), dict):
        fail("CAMPAIGN_INVALID", "link_scripts")
    old_script = checked_relative(evidence, scripts["before"].get("path", ""))
    new_script = checked_relative(evidence, scripts["after"].get("path", ""))
    require_hash(old_script, scripts["before"].get("sha256"), "LINK_SCRIPT_IDENTITY_MISMATCH")
    require_hash(new_script, scripts["after"].get("sha256"), "LINK_SCRIPT_IDENTITY_MISMATCH")
    validate_script_policy(old_script, new_script)

    inventory_path = checked_relative(evidence, campaign.get("input_sections", ""))
    inventory = load_json(inventory_path, "INPUT_INVENTORY_INVALID")
    sections = inventory.get("sections")
    if not isinstance(sections, list) or not sections:
        fail("INPUT_INVENTORY_INVALID", "empty")
    unexpected = [item for item in sections if not isinstance(item, dict) or item.get("classification") == "unexpected"]
    if unexpected:
        fail("UNEXPECTED_INPUT_SECTION", str(unexpected[:3]))
    required_families = {
        "kernel-code", "kernel-read-only-data", "kernel-mutable-data", "kernel-zero-fill",
        "unwind-metadata-unused-at-runtime", "compiler-identification-metadata", "toolchain-note-metadata",
        "link-time-relocation", "trampoline-code-and-patch-data", "bootstrap-code", "bootstrap-storage",
        "cpu-msr-recovery-table",
    }
    found_families = {item.get("classification") for item in sections if isinstance(item, dict)}
    missing_families = required_families - found_families
    if missing_families:
        fail("INPUT_INVENTORY_INCOMPLETE", ",".join(sorted(missing_families)))

    profiles = campaign.get("pair_profiles")
    pairs = campaign.get("pairs")
    if profiles != ["debug-on", "debug-off"] or not isinstance(pairs, dict):
        fail("PAIR_PROFILE_SET_INVALID", repr(profiles))
    pair_results = []
    for profile in profiles:
        record_path = checked_relative(evidence, pairs.get(profile, ""))
        pair_results.append(validate_pair(evidence, profile, record_path))

    orphan_record_path = checked_relative(evidence, campaign.get("orphan_probe", ""))
    orphan_record = load_json(orphan_record_path, "ORPHAN_RECORD_INVALID")
    orphan_result = validate_orphan_probe(evidence, orphan_record)

    candidate_records = campaign.get("candidates")
    if not isinstance(candidate_records, list):
        fail("CANDIDATE_SET_INCOMPLETE", "candidates is not an array")
    manifest_record = campaign.get("candidate_manifest")
    if candidate_records:
        if not isinstance(manifest_record, dict):
            fail("CANDIDATE_MANIFEST_INVALID", "campaign manifest record is missing")
        manifest_path = checked_relative(evidence, manifest_record.get("path", ""))
        require_hash(manifest_path, manifest_record.get("sha256"), "CANDIDATE_MANIFEST_MISMATCH")
    roles = [record.get("role") for record in candidate_records if isinstance(record, dict)]
    if len(roles) != len(set(roles)):
        fail("CANDIDATE_ROLE_DUPLICATE", repr(roles))
    if require_candidates:
        missing = REQUIRED_CANDIDATE_ROLES - set(roles)
        if missing:
            fail("CANDIDATE_SET_INCOMPLETE", ",".join(sorted(missing)))
    candidate_results = [verify_candidate(evidence, record) for record in candidate_records]
    result = {
        "status": "PASS",
        "pair_profiles": len(pair_results),
        "candidates": len(candidate_results),
        "input_section_records": len(sections),
        "orphan": orphan_result,
        "pairs": pair_results,
        "candidate_results": candidate_results,
    }
    return result


def patch_u64(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<Q", data, offset, value)


def patch_symbol_value(data: bytearray, elf: ElfFile, name: str, value: int) -> None:
    symbol = elf.symbol(name)
    patch_u64(data, symbol["entry_offset"] + 8, value)


def patch_section_field(data: bytearray, section: dict[str, Any], field_offset: int, value: int) -> None:
    patch_u64(data, section["header_offset"] + field_offset, value)


def expect_error(identifier: str, expected: str, action: Any, directory: Path, mutation: str) -> dict[str, Any]:
    observed = "PASS"
    detail = ""
    try:
        action()
    except ValidationError as exc:
        observed = exc.code
        detail = exc.detail
    status = "REJECTED" if observed == expected else "FAILED_EXPECTATION"
    record = {
        "id": identifier,
        "kind": "synthetic-parser-fixture",
        "mutation": mutation,
        "expected": expected,
        "observed": observed,
        "detail": detail,
        "status": status,
    }
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "result.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n")
    if status != "REJECTED":
        fail("FIXTURE_EXPECTATION_MISMATCH", f"{identifier}: expected={expected} observed={observed}")
    return record


def run_fixtures(evidence: Path, output: Path) -> dict[str, Any]:
    if output.exists() and any(output.iterdir()):
        fail("FIXTURE_OUTPUT_NOT_EMPTY", str(output))
    output.mkdir(parents=True, exist_ok=True)
    campaign = load_json(evidence / "campaign.json")
    pair_record = load_json(checked_relative(evidence, campaign["pairs"]["debug-off"]))
    before_path = checked_relative(evidence, pair_record["before"]["elf"])
    after_path = checked_relative(evidence, pair_record["after"]["elf"])
    orphan_record = load_json(checked_relative(evidence, campaign["orphan_probe"]))
    orphan_path = checked_relative(evidence, orphan_record["elf"])
    payload = bytes.fromhex(orphan_record["payload_hex"])
    before = ElfFile.from_path(before_path)
    base_data = after_path.read_bytes()
    base = ElfFile(base_data, "fixture-base")
    validate_layout(base)
    compare_trampoline(before, base)
    control_dir = output / "valid-control"
    control_dir.mkdir()
    shutil.copy2(after_path, control_dir / "kernel.elf")
    control = {"id": "valid-control", "expected": "PASS", "observed": "PASS", "status": "PASS"}
    (control_dir / "result.json").write_text(json.dumps(control, indent=2, sort_keys=True) + "\n")
    records = [control]

    def binary_case(identifier: str, expected: str, mutation: str, mutate: Any, validator: Any = validate_layout) -> None:
        case_dir = output / identifier
        case_dir.mkdir()
        data = bytearray(base_data)
        mutate(data, base)
        path = case_dir / "kernel.elf"
        path.write_bytes(data)
        (case_dir / "mutation.json").write_text(json.dumps({"id": identifier, "mutation": mutation, "synthetic": True}, indent=2, sort_keys=True) + "\n")
        records.append(expect_error(identifier, expected, lambda: validator(ElfFile.from_path(path)), case_dir, mutation))

    binary_case(
        "boundary-missing", "SYMBOL_MISSING", "rename _text_start in the ELF string table",
        lambda data, elf: data.__setitem__(elf.symbol("_text_start")["string_file_offset"], ord("X")),
    )
    binary_case(
        "boundary-outside", "BOUNDARY_START_INVALID", "move _text_start one byte into .text",
        lambda data, elf: patch_symbol_value(data, elf, "_text_start", elf.section(".text")["addr"] + 1),
    )
    binary_case(
        "exclusive-end-wrong", "BOUNDARY_END_INVALID", "make _text_end point to the final byte",
        lambda data, elf: patch_symbol_value(data, elf, "_text_end", elf.section(".text")["addr"] + elf.section(".text")["size"] - 1),
    )

    def overlap(data: bytearray, elf: ElfFile) -> None:
        rodata = elf.section(".rodata")
        data_section = elf.section(".data")
        new_size = data_section["addr"] - rodata["addr"] + 1
        patch_section_field(data, rodata, 32, new_size)
        patch_symbol_value(data, elf, "_rodata_end", rodata["addr"] + new_size)

    binary_case("shared-final-page", "REGION_PAGE_OVERLAP", "extend rodata one byte into the data page", overlap)

    def unaligned(data: bytearray, elf: ElfFile) -> None:
        section = elf.section(".data")
        patch_section_field(data, section, 16, section["addr"] + 1)
        patch_symbol_value(data, elf, "_data_start", section["addr"] + 1)

    binary_case("data-unaligned", "SECTION_ALIGNMENT_INVALID", "move .data and _data_start by one byte", unaligned)

    binary_case(
        "msr-fixup-boundary", "MSR_FIXUP_BOUNDARY_INVALID",
        "move the recovery table exclusive end by one byte",
        lambda data, elf: patch_symbol_value(
            data, elf, "_cpu_msr_fixup_end",
            elf.symbol("_cpu_msr_fixup_end")["value"] + 1),
    )

    def duplicate_msr_site(data: bytearray, elf: ElfFile) -> None:
        section = elf.section(".cpu_msr_fixup")
        first_site = struct.unpack_from("<Q", data, section["offset"])[0]
        patch_u64(data, section["offset"] + 16, first_site)

    binary_case(
        "msr-site-duplicate", "MSR_FIXUP_SITE_DUPLICATE",
        "make both recovery entries name the same instruction site",
        duplicate_msr_site,
    )

    def msr_fixup_outside_text(data: bytearray, elf: ElfFile) -> None:
        section = elf.section(".cpu_msr_fixup")
        text = elf.section(".text")
        patch_u64(data, section["offset"] + 8,
                  text["addr"] + text["size"])

    binary_case(
        "msr-fixup-outside-text", "MSR_FIXUP_TARGET_INVALID",
        "place the first recovery destination at the exclusive text end",
        msr_fixup_outside_text,
    )

    def msr_site_opcode(data: bytearray, elf: ElfFile) -> None:
        section = elf.section(".cpu_msr_fixup")
        text = elf.section(".text")
        site = struct.unpack_from("<Q", data, section["offset"])[0]
        data[text["offset"] + site - text["addr"]] ^= 0xff

    binary_case(
        "msr-site-opcode", "MSR_FIXUP_SITE_OPCODE_INVALID",
        "replace the opcode at the first protected instruction site",
        msr_site_opcode,
    )

    def pair_binary_case(identifier: str, expected: str, mutation: str, mutate: Any) -> None:
        case_dir = output / identifier
        case_dir.mkdir()
        data = bytearray(base_data)
        mutate(data, base)
        path = case_dir / "kernel.elf"
        path.write_bytes(data)
        records.append(expect_error(identifier, expected, lambda: compare_trampoline(before, ElfFile.from_path(path)), case_dir, mutation))

    def move_trampoline(data: bytearray, elf: ElfFile) -> None:
        section = elf.section(".trampoline")
        patch_section_field(data, section, 16, section["addr"] + PAGE_SIZE)
        for name in ("_trampoline_start", "_trampoline_end", *TRAMPOLINE_PATCH_SYMBOLS):
            patch_symbol_value(data, elf, name, elf.symbol(name)["value"] + PAGE_SIZE)

    pair_binary_case("trampoline-moved", "TRAMPOLINE_START_CHANGED", "move trampoline and its symbols by one page", move_trampoline)

    def extend_trampoline(data: bytearray, elf: ElfFile) -> None:
        section = elf.section(".trampoline")
        patch_section_field(data, section, 32, section["size"] + 1)
        patch_symbol_value(data, elf, "_trampoline_end", elf.symbol("_trampoline_end")["value"] + 1)

    pair_binary_case("trampoline-extended", "TRAMPOLINE_SIZE_INVALID", "extend trampoline by one byte", extend_trampoline)

    def corrupt_trampoline(data: bytearray, elf: ElfFile) -> None:
        section = elf.section(".trampoline")
        data[section["offset"]] ^= 0x01

    pair_binary_case("trampoline-corrupted", "TRAMPOLINE_BYTES_CHANGED", "flip the first trampoline payload bit", corrupt_trampoline)
    binary_case(
        "patch-symbol-outside", "TRAMPOLINE_PATCH_OUTSIDE", "place smp_trampoline_cr3 at the exclusive blob end",
        lambda data, elf: patch_symbol_value(data, elf, "smp_trampoline_cr3", elf.symbol("_trampoline_end")["value"]),
    )

    orphan_data = orphan_path.read_bytes()
    orphan_elf = ElfFile(orphan_data, "orphan-fixture")
    probe_section = orphan_elf.section(".layout_probe")
    absorbed = bytearray(orphan_data)
    replacement = b".rodata\0" + b"X" * (len(".layout_probe") - len(".rodata"))
    start = orphan_elf.sections[orphan_elf.header["shstrndx"]]["offset"] + probe_section["name_offset"]
    absorbed[start:start + len(replacement)] = replacement
    absorbed_dir = output / "orphan-absorbed"
    absorbed_dir.mkdir()
    (absorbed_dir / "kernel.elf").write_bytes(absorbed)
    records.append(expect_error(
        "orphan-absorbed", "ORPHAN_ABSORBED",
        lambda: validate_orphan_binary(ElfFile.from_path(absorbed_dir / "kernel.elf"), payload),
        absorbed_dir, "rename the visible orphan output section to .rodata while retaining its payload",
    ))
    discarded_dir = output / "orphan-discarded"
    discarded_dir.mkdir()
    shutil.copy2(after_path, discarded_dir / "kernel.elf")
    records.append(expect_error(
        "orphan-discarded", "ORPHAN_DISCARDED",
        lambda: validate_orphan_binary(ElfFile.from_path(discarded_dir / "kernel.elf"), payload),
        discarded_dir, "present a normal ELF with no visible probe payload",
    ))

    def invalid_load(data: bytearray, elf: ElfFile) -> None:
        program = next(item for item in elf.programs if item["type"] == PT_LOAD)
        patch_u64(data, elf.header["phoff"] + program["index"] * elf.header["phentsize"] + 32, program["memsz"] + 1)

    binary_case("load-filesz-invalid", "LOAD_SEGMENT_INVALID", "make PT_LOAD filesz exceed memsz", invalid_load)

    identity_dir = output / "other-candidate"
    identity_dir.mkdir()
    shutil.copy2(after_path, identity_dir / "kernel.elf")
    records.append(expect_error(
        "other-candidate", "CANDIDATE_IDENTITY_MISMATCH",
        lambda: require_hash(identity_dir / "kernel.elf", "0" * 64, "CANDIDATE_IDENTITY_MISMATCH"),
        identity_dir, "supply the valid ELF under another declared identity",
    ))
    truncated_dir = output / "binary-truncated"
    truncated_dir.mkdir()
    (truncated_dir / "kernel.elf").write_bytes(base_data[:48])
    records.append(expect_error(
        "binary-truncated", "ELF_TRUNCATED",
        lambda: ElfFile.from_path(truncated_dir / "kernel.elf"),
        truncated_dir, "truncate the ELF inside its header",
    ))
    map_dir = output / "map-result-mismatch"
    map_dir.mkdir()
    map_path = checked_relative(evidence, pair_record["after"]["map"])
    altered_map = map_dir / "kernel.map"
    altered_map.write_text(map_path.read_text(encoding="utf-8").replace(pair_record["after"]["elf"], "another/kernel.elf"), encoding="utf-8")
    objects_doc = load_json(checked_relative(evidence, pair_record["objects"]))
    order = [item["path"] for item in objects_doc["objects"]]
    records.append(expect_error(
        "map-result-mismatch", "MAP_IDENTITY_MISMATCH",
        lambda: validate_map_policy(altered_map, pair_record["after"]["elf"], order, True),
        map_dir, "replace the linked output identity in the raw map",
    ))

    summary = {
        "schema": 1,
        "status": "PASS",
        "valid_controls": 1,
        "negative_controls": len(records) - 1,
        "records": records,
    }
    (output / "results.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(f"SECTION_LAYOUT_FIXTURES: PASS controls=1 negatives={len(records) - 1}")
    return summary


def command_collect_core(args: argparse.Namespace) -> int:
    collect_core(Path(args.repo).resolve(), Path(args.evidence).resolve(), args.base_commit, args.reference)
    return 0


def command_collect_candidates(args: argparse.Namespace) -> int:
    collect_candidates(Path(args.repo).resolve(), Path(args.evidence).resolve(), Path(args.manifest).resolve())
    return 0


def command_all(args: argparse.Namespace) -> int:
    result = verify_all(Path(args.evidence).resolve(), require_candidates=True)
    print(
        "SECTION_LAYOUT: PASS "
        f"pairs={result['pair_profiles']} candidates={result['candidates']} "
        f"input_sections={result['input_section_records']} orphan=REJECTED"
    )
    return 0


def command_core(args: argparse.Namespace) -> int:
    result = verify_all(Path(args.evidence).resolve(), require_candidates=False)
    print(
        "SECTION_LAYOUT_CORE: PASS "
        f"pairs={result['pair_profiles']} candidates={result['candidates']} "
        f"input_sections={result['input_section_records']} orphan=REJECTED"
    )
    return 0


def command_elf(args: argparse.Namespace) -> int:
    result = validate_layout(ElfFile.from_path(Path(args.elf).resolve()))
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


def command_fixtures(args: argparse.Namespace) -> int:
    run_fixtures(Path(args.evidence_root).resolve(), Path(args.output_dir).resolve())
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    collect = sub.add_parser("collect-core")
    collect.add_argument("evidence")
    collect.add_argument("--repo", default=".")
    collect.add_argument("--base-commit", required=True)
    collect.add_argument("--reference", action="append", default=[])
    collect.set_defaults(handler=command_collect_core)
    candidates = sub.add_parser("collect-candidates")
    candidates.add_argument("evidence")
    candidates.add_argument("--manifest", required=True)
    candidates.add_argument("--repo", default=".")
    candidates.set_defaults(handler=command_collect_candidates)
    all_parser = sub.add_parser("all")
    all_parser.add_argument("evidence")
    all_parser.set_defaults(handler=command_all)
    core = sub.add_parser("core")
    core.add_argument("evidence")
    core.set_defaults(handler=command_core)
    elf = sub.add_parser("elf")
    elf.add_argument("elf")
    elf.set_defaults(handler=command_elf)
    fixtures = sub.add_parser("fixtures")
    fixtures.add_argument("--output-dir", required=True)
    fixtures.add_argument("--evidence-root", required=True)
    fixtures.set_defaults(handler=command_fixtures)
    return result


def main() -> int:
    args = parser().parse_args()
    try:
        return args.handler(args)
    except ValidationError as exc:
        print(f"SECTION_LAYOUT: FAIL code={exc.code} detail={exc.detail}", file=sys.stderr)
        return 1
    except (OSError, UnicodeError, KeyError, TypeError, ValueError, struct.error) as exc:
        print(f"SECTION_LAYOUT: FAIL code=UNCLASSIFIED_INPUT_ERROR detail={exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
