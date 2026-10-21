#!/usr/bin/env python3
"""Replace only the embedded menu background from a 320x240 RGB565 LE file."""
import argparse
from pathlib import Path
import re
import struct

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--header", type=Path, default=Path(__file__).resolve().parents[1] / "app/menu_assets.hpp")
    args = parser.parse_args()
    raw = args.image.read_bytes()
    if len(raw) != 320 * 240 * 2:
        parser.error("background must be exactly 153600 bytes (320x240 RGB565 little-endian)")
    pixels = struct.unpack("<76800H", raw)
    array = "inline const uint16_t background[76800]={\n"
    array += "\n".join(",".join(map(str, pixels[i:i+24])) + "," for i in range(0, len(pixels), 24))
    array += "\n};"
    text = args.header.read_text()
    updated, count = re.subn(r"inline const uint16_t background\[\d+\]=\{.*?\};", lambda _: array, text, count=1, flags=re.S)
    if count != 1:
        parser.error("background array not found")
    args.header.write_text(updated)

if __name__ == "__main__":
    main()
