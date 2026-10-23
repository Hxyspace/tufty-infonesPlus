#!/usr/bin/env python3
"""Convert PNG to headerless RGB565 little-endian without resizing.

Requires Pillow: python3 -m pip install Pillow
Pixels with alpha < 128 become the transparent color key 0xF81F;
other pixels retain their RGB color (binary transparency, no alpha blending).
Usage: python3 tools/png_to_565.py image.png [-o image.565]
"""

import argparse
from pathlib import Path
import struct

from PIL import Image


def convert(source, destination):
    with Image.open(source) as image:
        if image.format != "PNG":
            raise ValueError(f"Not a PNG image: {source}")
        rgba = image.convert("RGBA")
        width, height = rgba.size

    data = bytearray()
    for r, g, b, alpha in struct.iter_unpack("BBBB", rgba.tobytes()):
        pixel = 0xF81F if alpha < 128 else (
            ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        )
        data.extend(struct.pack("<H", pixel))

    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    print(f"{source} -> {destination}")
    print(f"{width}x{height}; RGB565 little-endian; {len(data):,} bytes; no header")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Source PNG")
    parser.add_argument("-o", "--output", type=Path,
                        help="Output .565 (default: beside source PNG)")
    args = parser.parse_args()
    destination = args.output or args.input.with_suffix(".565")
    if destination.suffix.lower() != ".565":
        parser.error("Output filename must use the .565 suffix")
    if args.input.resolve() == destination.resolve():
        parser.error("Input and output must be different files")
    try:
        convert(args.input, destination)
    except (OSError, ValueError) as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    main()
