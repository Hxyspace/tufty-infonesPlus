"""Independent GIDX wire fixtures exercised by the actual firmware storage/menu."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib


def index_data(scenario):
    description = "First paragraph.\n\nSecond paragraph wraps over several lines with words and punctuation. " * 4
    if scenario == "index_long":
        description = "This line crosses many pages.\n" * 2400 + "Final page beyond the old 256-line limit."
    records = [("Absent.NES", "Z Display Title", description, 3),
               ("Sample Game.nes", "A Display Title", "", 0)]
    if scenario == "index_empty":
        records = []
    if scenario == "index_max":
        records += [(f"Game{i}.NES", f"Title {i}", "", 0) for i in range(126)]
    if scenario == "index_duplicate":
        records[1] = ("absent.nes", "Duplicate", "", 0)
    if scenario == "index_path":
        records[1] = ("../Bad.nes", "Path", "", 0)
    if scenario == "index_control":
        records[1] = ("Bad.nes", "Bad\tTitle", "", 0)
    strings, offsets = bytearray(b"\0"), {"": 0}

    def intern(value):
        if value not in offsets:
            offsets[value] = len(strings)
            strings.extend(value.encode("ascii") + b"\0")
        return offsets[value]

    encoded = bytearray()
    for name, title, desc, flags in records:
        encoded.extend(struct.pack("<IIII", intern(name), intern(title), intern(desc), flags))
    data = bytearray(struct.pack("<4sHHHHIIII4B", b"GIDX", 1, 32, len(records), 16,
                                 32 + len(encoded), 32 + len(encoded) + len(strings), 0, 0, 58, 80, 1, 0))
    data.extend(encoded + strings)
    if scenario == "index_offset":
        struct.pack_into("<I", data, 48, 0xffffffff)
    elif scenario == "index_boundary":
        struct.pack_into("<I", data, 48, 2)
    elif scenario == "index_flags":
        struct.pack_into("<I", data, 60, 0x80)
    elif scenario == "index_desc_flag":
        struct.pack_into("<I", data, 44, 1)
    elif scenario == "index_no_nul":
        data[-1] = ord("x")
    elif scenario == "index_header":
        data[28] = 60
    elif scenario == "index_strings":
        struct.pack_into("<I", data, 12, 63)
    elif scenario == "index_short":
        data = data[:31]
    elif scenario == "index_truncated":
        # Keep the declared size, but truncate the body and repair its CRC.
        # This must fail the actual-length check independently of the checksum.
        data = data[:-8]
    elif scenario == "index_oversize":
        data.extend(bytes(256 * 1024))
    struct.pack_into("<I", data, 20, zlib.crc32(data))
    if scenario == "index_crc":
        data[-2] ^= 1
    return data


with tempfile.TemporaryDirectory(prefix="tufty-gidx-test-") as directory:
    root = Path(directory)
    image = root / "fat.img"
    image.write_bytes(bytes(12 * 1024 * 1024))
    subprocess.run(["mkfs.fat", "--invariant", "-S", "4096", "-s", "1", "-F", "12", str(image)],
                   check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["mmd", "-i", str(image), "::roms"], check=True)
    rom = root / "fixture.nes"
    rom.write_bytes(b"NES\x1a" + bytes((1, 0, 0, 0)) + bytes(8) + b"\xea" * 16384)
    for name in ("Sample Game.nes", "测试.nes"):
        subprocess.run(["mcopy", "-i", str(image), str(rom), "::roms/" + name], check=True)
    index = root / "games.idx"
    index.write_bytes(index_data(sys.argv[2]))
    subprocess.run(["mcopy", "-i", str(image), str(index), "::roms/games.idx"], check=True)
    cover = root / "Absent.565"
    cover.write_bytes(b"\x1f\x00" * 58 * 80)
    subprocess.run(["mcopy", "-i", str(image), str(cover), "::roms/Absent.565"], check=True)
    subprocess.run([sys.argv[1], str(image), "4096", sys.argv[2]], check=True, env=os.environ)
