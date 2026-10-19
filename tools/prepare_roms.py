#!/usr/bin/env python3
"""Inspect a ROM tree; explicitly apply checksum-matched iNES header repairs."""
import argparse
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import tempfile
import zlib

PROJECT = Path(__file__).resolve().parents[1]
MIRRORING = {"horizontal": 0, "vertical": 1, "four-screen": 8}
MAX_ROM = 4 * 1024 * 1024


def load_rules(path):
    rules = {}
    for rule in json.loads(Path(path).read_text(encoding="utf-8"))["rules"]:
        crc = rule["crc32"].lower()
        if not re.fullmatch(r"[0-9a-f]{8}", crc) or crc in rules:
            raise ValueError("Invalid or duplicate CRC32 rule: " + crc)
        if rule.get("mirroring") not in (*MIRRORING, None):
            raise ValueError("Unsupported mirroring: " + str(rule["mirroring"]))
        if "mapper" in rule and (type(rule["mapper"]) is not int or not 0 <= rule["mapper"] <= 255):
            raise ValueError("Only iNES 1.0 mappers 0..255 can be patched")
        if "sha256" in rule and not re.fullmatch(r"[0-9a-f]{64}", rule["sha256"]):
            raise ValueError("Invalid payload SHA256")
        for size in ("prg_bytes", "chr_bytes"):
            if size in rule and (type(rule[size]) is not int or rule[size] < 0):
                raise ValueError("Invalid ROM size constraint")
        rules[crc] = rule
    return rules


def supported_mappers():
    source = (PROJECT / "core_api/cartridge.hpp").read_text()
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    table = source.split("admitted_mappers = {", 1)[1].split("};", 1)[0]
    return {int(m) for m in re.findall(r"\d+", table)}


@dataclass
class Review:
    fixed: bytes
    notes: list = field(default_factory=list)
    error: bool = False


def review(data, rules, mappers, trim_padding=False):
    result = Review(data)
    if len(data) < 16 or data[:4] != b"NES\x1a":
        return Review(data, ["ERROR: missing/truncated iNES header"], True)
    nes2 = data[7] & 0x0C == 0x08
    if nes2:
        # Format preflight; firmware additionally validates board RAM/geometry.
        ram = lambda value: sum(64 << n if n else 0 for n in (value & 15, value >> 4))
        reason = None
        if data[8] & 0xF0:
            reason = "submapper is unsupported"
        elif data[8] & 15:
            reason = "extended mapper is unsupported"
        elif data[9]:
            reason = "extended ROM sizes are unsupported"
        elif ram(data[10]) > 8192 or ram(data[11]) > 8192 or (data[5] and data[11]):
            reason = "RAM layout is unsupported"
        elif data[12] & 0xfc or data[12] == 2:
            reason = "unknown or multi-region timing"
        elif data[13] or data[14] or data[15] > 1:
            reason = "extra hardware/data is unsupported"
        if reason:
            return Review(data, [f"ERROR: NES 2.0 {reason}; unchanged"], True)
    if data[7] & 3 or not data[4]:
        return Review(data, ["ERROR: firmware requires standard NES and nonempty PRG; unchanged"], True)
    prg, chr_size = data[4] * 16384, data[5] * 8192
    start = 16 + (512 if data[6] & 4 else 0)
    end = start + prg + chr_size
    if len(data) < end:
        return Review(data, [f"ERROR: truncated ROM: need {end}, have {len(data)} bytes"], True)
    payload = data[start:end]
    crc = f"{zlib.crc32(payload):08x}"
    mapper = (data[6] >> 4) | (data[7] & 0xF0)
    mirror = "four-screen" if data[6] & 8 else "vertical" if data[6] & 1 else "horizontal"
    result.notes.append(f"{'NES 2.0' if nes2 else 'iNES 1.0'} CRC32={crc} mapper={mapper} mirroring={mirror} PRG={prg} CHR={chr_size}")
    fixed = bytearray(data)
    rule = rules.get(crc)
    if rule:
        digest = hashlib.sha256(payload).hexdigest()
        matches = (rule.get("prg_bytes", prg) == prg and rule.get("chr_bytes", chr_size) == chr_size
                   and rule.get("sha256", digest) == digest)
        if not matches:
            result.notes.append("WARN: CRC matched but size/SHA256 disagrees; header left unchanged")
        else:
            result.notes.append("Known ROM: " + rule.get("name", crc))
            target = rule.get("mapper", mapper)
            if target != mapper:
                fixed[6] = (fixed[6] & 0x0F) | ((target & 15) << 4)
                fixed[7] = (fixed[7] & 0x0F) | (target & 0xF0)
                result.notes.append(f"FIX: mapper {mapper} -> {target}")
                mapper = target
            if "mirroring" in rule and mirror != rule["mirroring"]:
                fixed[6] = (fixed[6] & ~9) | MIRRORING[rule["mirroring"]]
                result.notes.append(f"FIX: mirroring {mirror} -> {rule['mirroring']}")
    else:
        result.notes.append("No known correction; header left unchanged")
    if mapper not in mappers:
        result.notes.append(f"ERROR: mapper {mapper} is not admitted by the firmware loader")
        result.error = True
    if not nes2 and any(data[12:16]):
        result.notes.append("WARN: reserved header bytes are nonzero; no automatic cleanup")
    if len(data) > end:
        tail = data[end:]
        # Do not treat zero/FF/data tails as padding: they can be meaningful.
        whitespace = not tail.strip(b" \t\r\n")
        result.notes.append(f"WARN: {len(tail)} trailing bytes ({'ASCII whitespace' if whitespace else 'unrecognized data'})")
        if trim_padding and whitespace:
            del fixed[end:]
            result.notes.append("FIX: remove trailing ASCII whitespace outside declared PRG/CHR")
    if len(fixed) > MAX_ROM:
        result.notes.append("ERROR: ROM exceeds firmware's 4 MiB file limit")
        result.error = True
    result.fixed = bytes(fixed)
    result.notes.append("Preflight only: firmware applies additional board geometry/RAM checks")
    return result


def replace_with_backup(path, original, replacement, backup_root, relative):
    """Preserve exact original bytes before atomically replacing one local file."""
    digest = hashlib.sha256(original).hexdigest()
    backup = backup_root / relative.parent / (relative.name + "." + digest + ".bak")
    backup.parent.mkdir(parents=True, exist_ok=True)
    try:
        with backup.open("xb") as out:
            out.write(original)
            out.flush()
            os.fsync(out.fileno())
    except FileExistsError:
        if backup.read_bytes() != original:
            raise ValueError("Existing backup does not match original: " + str(backup))
    # Guard against a changed file between inspection and write.
    if path.is_symlink() or path.read_bytes() != original:
        raise ValueError("ROM changed during inspection: " + str(path))
    mode = stat.S_IMODE(path.stat().st_mode)
    fd, name = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(replacement)
            out.flush()
            os.fsync(out.fileno())
        os.chmod(name, mode)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)
    return backup


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", nargs="?", type=Path, default=PROJECT / "roms")
    parser.add_argument("--fix", action="store_true", help="apply fixes after saving original backups (default: preview)")
    parser.add_argument("--trim-padding", action="store_true", help="also remove trailing ASCII whitespace beyond declared PRG/CHR")
    parser.add_argument("--rules", type=Path, default=Path(__file__).with_name("rom_fixes.json"))
    parser.add_argument("--backup-dir", type=Path, default=PROJECT / "build/rom-backups")
    args = parser.parse_args(argv)
    if not args.directory.is_dir():
        parser.error("ROM directory does not exist: " + str(args.directory))
    try:
        rules, mappers = load_rules(args.rules), supported_mappers()
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.error(str(exc))
    files = sorted(p for p in args.directory.rglob("*") if p.is_file() and p.suffix.lower() == ".nes")
    changed = errors = 0
    for path in files:
        relative = path.relative_to(args.directory)
        print(f"\n{relative}:")
        try:
            if path.is_symlink() or any(p.is_symlink() for p in path.parents):
                raise ValueError("symlink path skipped")
            original = path.read_bytes()
            result = review(original, rules, mappers, args.trim_padding)
            for note in result.notes:
                print("  " + note)
            if relative.parent != Path("."):
                print("  WARN: firmware only lists ROMs directly inside /system/roms")
            if len(path.name.encode("utf-8")) > 255:
                print("  ERROR: filename exceeds firmware's 255-byte limit")
                result.error = True
            if result.error:
                errors += 1
                print("  Unchanged due to errors")
            elif result.fixed != original:
                if args.fix:
                    backup = replace_with_backup(path, original, result.fixed, args.backup_dir, relative)
                    print(f"  Applied; backup: {backup}")
                else:
                    print("  Preview only; use --fix to apply")
                changed += 1
        except (OSError, ValueError) as exc:
            errors += 1
            print(f"  ERROR: {exc}")
    print(f"\nScanned {len(files)} ROM(s); {'changed' if args.fix else 'would change'} {changed}; errors {errors}")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
