#!/usr/bin/env python3
"""Reject XIP dependencies in the linked disk loop (ARM ELF, --emit-relocs).

Checks the entire overlay's relocated pointers, follows outgoing direct branches
through SRAM helpers, and rejects indirect calls/jump tables. Raw MMIO addresses
and the runtime Shared pointer still require source review; this is a build
regression guard, not a general proof for arbitrary C++ pointer arithmetic.
"""
import argparse
import re
import struct
import subprocess


def check(path, prefix="arm-none-eabi-"):
    def tool(name, *args):
        return subprocess.check_output([prefix + name, *args, str(path)], text=True)

    sections = {}
    for line in tool("objdump", "-h").splitlines():
        match = re.match(r"\s*\d+\s+(\S+)\s+([\da-f]+)\s+([\da-f]+)\s+([\da-f]+)\s+([\da-f]+)", line)
        if match:
            name, size, vma, lma, offset = match.groups()
            sections[name] = tuple(int(n, 16) for n in (size, vma, lma, offset))
    image_size, image_at, image_load, _ = sections[".disk_image"]
    bss_size, bss_at, _, _ = sections[".disk_bss"]
    game_size, game_at, _, _ = sections[".game_workspace"]
    vector_size, vector_at, _, _ = sections[".ram_vector_table"]
    vector_alignment = max(128, 1 << (vector_size - 1).bit_length())
    assert vector_at % vector_alignment == 0, (
        f"RAM vector table {vector_at:#x} must be {vector_alignment}-byte aligned")
    assert vector_at + vector_size <= game_at, "boot vector table overlaps mode workspace"
    assert image_at == game_at and bss_at == image_at + image_size
    assert image_size + bss_size <= game_size
    for section in (".data", ".bss"):
        size, at, _, _ = sections[section]
        assert at >= game_at + game_size, f"{section} overlaps mode workspace"
    assert 0x10000000 <= image_load < 0x10200000

    def ram(at):
        return 0x20000000 <= at < 0x20082000

    assert ram(image_at) and ram(game_at + game_size - 1)
    symbols = {}
    sized_symbols = {}
    for line in tool("nm", "-S", "--defined-only").splitlines():
        match = re.match(r"([\da-f]+) ([\da-f]+) ([a-zA-Z]) (\S+)$", line)
        if match:
            at, size, kind, name = match.groups()
            at, size = int(at, 16), int(size, 16)
            sized_symbols[name] = (at, size)
            if kind in "tTwW":
                symbols[at & ~1] = (size, name)
    # Both large buffers must share the NES frame region, while TinyUSB's
    # interface state must survive normal-mode USB init/reset outside it.
    usb_buffers = []
    for name in ("_ZZ18tud_msc_write10_cbE7staging", "_mscd_epbuf"):
        at, size = sized_symbols[name]
        assert size == 4096 and at % 4 == 0, f"unexpected disk buffer: {name}"
        assert bss_at <= at and at + size <= bss_at + bss_size, (
            f"disk buffer outside overlay: {name}")
        usb_buffers.append((at, at + size))
    usb_buffers.sort()
    assert usb_buffers[0][1] <= usb_buffers[1][0], "disk USB buffers overlap"
    at, size = sized_symbols["_mscd_itf"]
    normal_size, normal_at, _, _ = sections[".bss"]
    assert normal_at <= at and at + size <= normal_at + normal_size, (
        "MSC interface state must remain in normal BSS for USB init/reset")
    relocs = []
    reloc_text = tool("readelf", "-rW")
    assert "'.rel.disk_image'" in reloc_text, "link with --emit-relocs"
    for line in reloc_text.splitlines():
        match = re.match(r"([\da-f]{8})\s+\S+\s+(R_ARM_\S+)\s+([\da-f]+)\s+(.*)", line)
        if match:
            at, kind, target, name = match.groups()
            relocs.append((int(at, 16), kind, int(target, 16), name))
    blob = path.read_bytes()

    def word(at):
        for size, vma, _, offset in sections.values():
            if vma <= at <= vma + size - 4:
                return struct.unpack_from("<I", blob, offset + at - vma)[0]
        raise AssertionError(f"unmapped relocation {at:#x}")

    instructions = []
    for line in tool("objdump", "-d").splitlines():
        match = re.match(r"\s*([\da-f]+):\s+(?:[\da-f]{4,8}\s+)+\s*(\S+)\s*(.*)", line)
        if match:
            at, op, operands = match.groups()
            instructions.append((int(at, 16), op, operands))
    pending = [(image_at, image_size, ".disk_image")]
    seen = set()
    helpers = set()
    checked = 0
    while pending:
        start, size, name = pending.pop()
        if start in seen:
            continue
        seen.add(start)
        assert ram(start) and ram(start + size - 1), f"non-SRAM code: {name}"
        for at, kind, target, symbol in relocs:
            if not start <= at < start + size:
                continue
            checked += 1
            if kind == "R_ARM_ABS32":
                target = word(at)  # Includes section-symbol addends.
                assert ram(target & ~1), f"non-SRAM pointer at {at:#x}: {target:#x} ({symbol})"
            else:
                assert kind in ("R_ARM_THM_CALL", "R_ARM_THM_JUMP24", "R_ARM_THM_JUMP19"), (hex(at), kind)
                assert ram(target & ~1), f"non-SRAM call at {at:#x}: {symbol}"
        for at, op, operands in instructions:
            if not start <= at < start + size:
                continue
            assert op not in ("blx", "tbb", "tbh"), f"indirect branch at {at:#x}"
            assert not (op == "bx" and operands.strip() != "lr"), f"indirect jump at {at:#x}"
            assert not (op.startswith("ldr") and operands.startswith("pc,")), f"indirect load at {at:#x}"
            if not re.fullmatch(r"b(?:l|eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(?:\.[nw])?|cb[n]?z", op):
                continue
            target = re.search(r"\b([\da-f]{8})\s+<", operands)
            if not target:
                continue
            target = int(target[1], 16) & ~1
            if start <= target < start + size or image_at <= target < image_at + image_size:
                continue
            assert ram(target), f"XIP branch at {at:#x} -> {target:#x}"
            assert target in symbols, f"unknown branch target {target:#x}"
            length, helper = symbols[target]
            assert length, f"unsized helper {helper}"
            helpers.add(helper)
            pending.append((target, length, helper))
    assert checked >= 10, "missing disk relocations"
    print(f"Disk SRAM: image {image_size} B + zeroed data {bss_size} B <= game workspace {game_size} B")
    print(f"RAM vector table: {vector_at:#010x}, {vector_size} B, {vector_alignment}-byte alignment")
    print("Disk USB buffers: 2 x 4096 B in overlay; MSC interface state outside overlay")
    print(f"Checked {checked} relocations and direct-call closure; SRAM helpers: {', '.join(sorted(helpers)) or 'none'}")


if __name__ == "__main__":
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--prefix", default="arm-none-eabi-")
    args = parser.parse_args()
    check(args.elf, args.prefix)
