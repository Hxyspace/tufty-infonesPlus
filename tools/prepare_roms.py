#!/usr/bin/env python3
"""Inspect ROMs; repair DiskDude! and checksum-matched headers using a local Mesen database."""
import argparse
import csv
from dataclasses import dataclass, field
import hashlib
from http.client import HTTPException
import json
import os
from pathlib import Path
import re
import stat
import tempfile
from urllib.request import urlopen
import zlib

PROJECT = Path(__file__).resolve().parents[1]
MIRRORING = {"horizontal": 0, "vertical": 1, "four-screen": 8}
MAX_ROM = 4 * 1024 * 1024
DEFAULT_DATABASE = PROJECT / "build/rom-database/MesenNesDB.txt"
DATABASE_URL = "https://raw.githubusercontent.com/SourMesen/Mesen2/master/UI/Dependencies/MesenNesDB.txt"


def load_database(path):
    """Read Mesen's 18-column database. Never choose between duplicate CRCs."""
    entries = {}
    with Path(path).open(encoding="utf-8-sig", newline="") as stream:
        for line, row in enumerate(csv.reader(stream), 1):
            if not row or not row[0].strip() or row[0].lstrip().startswith("#"):
                continue
            row = [value.strip() for value in row]
            if len(row) != 18 or not re.fullmatch(r"[0-9a-fA-F]{8}", row[0]):
                raise ValueError(f"Invalid Mesen database row {line}")
            crc = row[0].lower()
            if crc in entries:
                raise ValueError(f"Duplicate database CRC32 {crc} at row {line}")
            entry = dict(crc32=crc, system=row[1], board=row[2], mirroring=row[12],
                         bus_conflicts=row[14], vs_type=row[16], ppu=row[17])
            for index, name in ((5, "mapper"), (6, "prg_kib"), (7, "chr_kib"),
                                (8, "chr_ram_kib"), (9, "work_ram_kib"), (10, "save_ram_kib"),
                                (11, "battery"), (13, "controller"), (15, "submapper")):
                value = row[index]
                if value and not re.fullmatch(r"[0-9]+", value):
                    raise ValueError(f"Invalid {name} at database row {line}")
                entry[name] = int(value) if value else None
            entries[crc] = entry
    if not entries:
        raise ValueError("Mesen database has no entries")
    return entries


def download_database(path):
    """Validate a bounded download before atomically publishing the cache."""
    with urlopen(DATABASE_URL, timeout=10) as response:
        data = response.read(4 * 1024 * 1024 + 1)
    if len(data) > 4 * 1024 * 1024:
        raise ValueError("database download exceeds 4 MiB")
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".mesen-db-", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        entries = load_database(temporary)
        os.replace(temporary, path)
        return entries
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


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


def check_rom_geometry(mapper, prg, chr_size):
    """Common limits only; firmware still applies its full board validator."""
    prg_limit = (1024 if mapper in (15, 34, 177, 226, 241) else 512) * 1024
    if (not prg or prg & (prg - 1) or prg > prg_limit or chr_size > 256 * 1024
            or (chr_size and chr_size & (chr_size - 1) and not (mapper == 4 and chr_size % 8192 == 0))):
        raise ValueError("ROM geometry exceeds firmware preflight limits")
    if mapper == 0 and (prg > 32768 or chr_size > 8192):
        raise ValueError("NROM geometry is unsupported by firmware")
    if mapper == 268 and not chr_size:
        raise ValueError("firmware mapper 268 admission requires CHR ROM")


def database_header(data, entry):
    """Encode only explicit, representable standard NES board facts as NES 2.0."""
    timing = {"NesNtsc": 0, "Famicom": 0, "NesPal": 1, "Dendy": 3}
    if entry["system"] not in timing:
        raise ValueError("database system is not standard NES")
    mapper, sub = entry["mapper"], entry["submapper"]
    if not 0 <= mapper <= 4095 or sub is None or not 0 <= sub <= 15:
        raise ValueError("database mapper/submapper is missing or unrepresentable")
    if entry["mirroring"] not in ("h", "v", "4"):
        raise ValueError("database mirroring is missing or needs board-specific wiring")
    if entry["controller"] not in (0, 1) or entry["vs_type"] not in ("", "0") or entry["ppu"] not in ("", "0"):
        raise ValueError("database controller/console hardware is unsupported or unspecified")
    if entry["bus_conflicts"] not in ("",):
        # This separate Mesen setting cannot be encoded generically in NES 2.0.
        raise ValueError("database has a separate bus-conflict override")

    def ram_code(key):
        value = entry[key]
        if value is None:
            raise ValueError("database RAM layout is incomplete")
        size = value * 1024
        if not size:
            return 0
        if size & (size - 1) or not 128 <= size <= 64 << 15:
            raise ValueError("database RAM size cannot be encoded in NES 2.0")
        return size.bit_length() - 7

    work, save, chr_ram = (ram_code(key) for key in ("work_ram_kib", "save_ram_kib", "chr_ram_kib"))
    if entry["battery"] not in (0, 1) or bool(entry["battery"]) != bool(save):
        raise ValueError("database battery layout needs board-internal RAM handling")
    header = bytearray(data[:16])
    header[6] = ((mapper & 15) << 4) | (header[6] & 4) | (entry["battery"] << 1) | {"h": 0, "v": 1, "4": 8}[entry["mirroring"]]
    header[7] = (mapper & 0xf0) | 8
    header[8] = (sub << 4) | (mapper >> 8)
    header[9:16] = bytes((0, work | (save << 4), chr_ram, timing[entry["system"]], 0, 0, entry["controller"]))
    return bytes(header)


def review(data, rules, mappers, trim_padding=False, database=None):
    if database is None or len(data) < 16 or data[:4] != b"NES\x1a":
        return _review_header(data, rules, mappers, trim_padding)
    clean = data[:7] + bytes(9) + data[16:] if data[7:16] in (b"DiskDude!", b"@iskDude!") else data
    nes2 = clean[7] & 12 == 8
    # Do not use a guessed payload boundary for a database lookup. Extended
    # sizes, nonstandard consoles and extra hardware need separate support.
    if clean[7] & 12 not in (0, 8) or clean[7] & 3 or (nes2 and (clean[9] or clean[13] or clean[14] or clean[15] > 1)):
        return _review_header(data, rules, mappers, trim_padding)
    prg, chr_size = clean[4] * 16384, clean[5] * 8192
    start = 16 + (512 if clean[6] & 4 else 0)
    end = start + prg + chr_size
    if not prg or len(clean) < end:
        return _review_header(data, rules, mappers, trim_padding)
    payload = clean[start:end]
    crc = f"{zlib.crc32(payload):08x}"
    entry = database.get(crc)
    if entry is None:
        result = _review_header(data, rules, mappers, trim_padding)
        result.notes.insert(0, f"Database: no match for PRG+CHR CRC32={crc}")
        return result
    notes = [f"Database: CRC32={crc}, board={entry['board'] or '(unspecified)'}, mapper={entry['mapper']}.{entry['submapper']}"]
    try:
        if any(entry[key] is None for key in ("mapper", "prg_kib", "chr_kib")):
            raise ValueError("database mapper/ROM sizes are incomplete")
        if entry["prg_kib"] * 1024 != prg or entry["chr_kib"] * 1024 != chr_size:
            raise ValueError("CRC matched but database PRG/CHR sizes disagree")
        header = database_header(clean, entry)
        rule = rules.get(crc)
        if rule:
            digest = hashlib.sha256(payload).hexdigest()
            mirror = {"h": "horizontal", "v": "vertical", "4": "four-screen"}[entry["mirroring"]]
            if (rule.get("prg_bytes", prg) != prg or rule.get("chr_bytes", chr_size) != chr_size
                    or rule.get("sha256", digest) != digest):
                raise ValueError("CRC rule size/SHA256 disagrees with ROM")
            if rule.get("mapper", entry["mapper"]) != entry["mapper"] or rule.get("mirroring", mirror) != mirror:
                raise ValueError("database and CRC rule conflict; choose the intended rules explicitly")
    except ValueError as exc:
        return Review(data, notes + [f"ERROR: {exc}; unchanged"], True)
    candidate = header + data[16:]
    result = _review_header(candidate, {}, mappers, trim_padding)
    result.notes = [note for note in result.notes if note != "No known correction; header left unchanged"]
    if header != data[:16]:
        old_mapper = (clean[6] >> 4) | (clean[7] & 0xf0) | ((clean[8] & 15) << 8 if nes2 else 0)
        old_sub = clean[8] >> 4 if nes2 else 0
        notes.append(f"FIX: database header {old_mapper}.{old_sub} -> {entry['mapper']}.{entry['submapper']} (NES 2.0)")
        notes.append(f"Header: {data[:16].hex(' ')} -> {header.hex(' ')}")
        notes.append("Database fields: mapper, submapper, mirroring, battery, RAM, timing and standard controller")
    else:
        notes.append("Database header already matches")
    if clean != data:
        notes.append("FIX: remove DiskDude! signature; database restores board fields")
    if result.error:
        result.fixed = data
        notes.append("ERROR: corrected header fails firmware preflight; original preserved")
    result.notes = notes + result.notes
    return result


def _review_header(data, rules, mappers, trim_padding=False):
    result = Review(data)
    if len(data) < 16 or data[:4] != b"NES\x1a":
        return Review(data, ["ERROR: missing/truncated iNES header"], True)
    original = data
    # DiskDude! overwrote bytes 7..15, including mapper high bits and format.
    # Some dumpers already cleared byte 7's low nibble: '@iskDude!' remains.
    diskdude = data[7:16] in (b"DiskDude!", b"@iskDude!")
    if diskdude:
        data = data[:7] + bytes(9) + data[16:]
        result.notes.append("FIX: remove DiskDude! signature from header bytes 7..15")
        result.notes.append("WARN: overwritten mapper high bits default to zero unless a matching CRC rule restores them; verify gameplay")
    if data[7] & 0x0C not in (0, 8):
        return Review(original, result.notes + ["ERROR: unknown NES header format; unchanged"], True)
    nes2 = data[7] & 0x0C == 0x08
    mapper = (data[6] >> 4) | (data[7] & 0xF0)
    if nes2:
        mapper |= (data[8] & 15) << 8
    if nes2:
        # Format preflight; firmware additionally validates board RAM/geometry.
        ram = lambda value: sum(64 << n if n else 0 for n in (value & 15, value >> 4))
        reason = None
        if data[8] & 0xF0 and not ((mapper in (2, 3, 7, 25) and data[8] >> 4 == 2)
                                 or (mapper == 23 and data[8] >> 4 in (2, 3))
                                 or (mapper == 19 and data[8] >> 4 in (2, 3, 5))
                                 or (mapper == 206 and data[8] >> 4 == 1)
                                 or (mapper == 210 and data[8] >> 4 in (1, 2))
                                 or (mapper == 34 and data[8] >> 4 in (1, 2))
                                 or (mapper == 78 and data[8] >> 4 in (1, 3))):
            reason = "submapper is unsupported"
        elif mapper > 255 and mapper not in mappers:
            reason = f"extended mapper {mapper} is unsupported"
        elif data[9]:
            reason = "extended ROM sizes are unsupported"
        elif ram(data[10]) > 8192 or ram(data[11]) > 8192 or (data[5] and data[11]):
            reason = "RAM layout is unsupported"
        elif data[12] & 0xfc:
            reason = "unknown timing"
        elif data[13] or data[14] or data[15] > 1:
            reason = "extra hardware/data is unsupported"
        if reason:
            return Review(original, result.notes + [f"ERROR: NES 2.0 {reason}; unchanged"], True)
    if data[7] & 3 or not data[4]:
        return Review(original, result.notes + ["ERROR: firmware requires standard NES and nonempty PRG; unchanged"], True)
    prg, chr_size = data[4] * 16384, data[5] * 8192
    start = 16 + (512 if data[6] & 4 else 0)
    end = start + prg + chr_size
    if len(data) < end:
        return Review(original, result.notes + [f"ERROR: truncated ROM: need {end}, have {len(data)} bytes; unchanged"], True)
    payload = data[start:end]
    crc = f"{zlib.crc32(payload):08x}"
    mirror = "four-screen" if data[6] & 8 else "vertical" if data[6] & 1 else "horizontal"
    result.notes.append(f"{'NES 2.0' if nes2 else 'iNES 1.0'} CRC32={crc} mapper={mapper} mirroring={mirror} PRG={prg} CHR={chr_size}")
    if nes2 and data[12] == 2:
        result.notes.append("INFO: multi-region ROM runs with NTSC timing; original header preserved")
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
                if nes2:
                    fixed[8] = (fixed[8] & 0xF0) | (target >> 8)
                result.notes.append(f"FIX: mapper {mapper} -> {target}")
                mapper = target
            if "mirroring" in rule and mirror != rule["mirroring"]:
                fixed[6] = (fixed[6] & ~9) | MIRRORING[rule["mirroring"]]
                result.notes.append(f"FIX: mirroring {mirror} -> {rule['mirroring']}")
    else:
        result.notes.append("No checksum-matched board correction" if diskdude else "No known correction; header left unchanged")
    try:
        check_rom_geometry(mapper, prg, chr_size)
    except ValueError as exc:
        result.notes.append(f"ERROR: {exc}")
        result.error = True
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
    result.fixed = original if result.error else bytes(fixed)
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
    database_args = parser.add_mutually_exclusive_group()
    database_args.add_argument("--database", type=Path, help="MesenNesDB.txt cache (default: build/rom-database/MesenNesDB.txt; downloaded if missing)")
    database_args.add_argument("--no-database", action="store_true", help="use only the small CRC rules and DiskDude! cleanup")
    parser.add_argument("--backup-dir", type=Path, default=PROJECT / "build/rom-backups")
    args = parser.parse_args(argv)
    if not args.directory.is_dir():
        parser.error("ROM directory does not exist: " + str(args.directory))
    try:
        rules, mappers = load_rules(args.rules), supported_mappers()
        database_path = None if args.no_database else args.database or DEFAULT_DATABASE
        database = None
        if database_path:
            if database_path.exists():
                database = load_database(database_path)
            else:
                print("Database missing; downloading from Mesen...")
                try:
                    database = download_database(database_path)
                except (OSError, ValueError, HTTPException) as exc:
                    print(f"WARN: database download failed: {exc}; using CRC rules only")
                    database_path = None
    except (OSError, ValueError, KeyError, TypeError, UnicodeError) as exc:
        parser.error(str(exc))
    if database_path:
        print(f"Database: {database_path} ({len(database)} entries)")
    files = sorted(p for p in args.directory.rglob("*") if p.is_file() and p.suffix.lower() == ".nes")
    changed = errors = 0
    for path in files:
        relative = path.relative_to(args.directory)
        print(f"\n{relative}:")
        try:
            if path.is_symlink() or any(p.is_symlink() for p in path.parents):
                raise ValueError("symlink path skipped")
            original = path.read_bytes()
            result = review(original, rules, mappers, args.trim_padding, database)
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
