#!/usr/bin/env python3
"""Package firmware variants, resource images, user docs, provenance and licenses."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

PROJECT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    dest = args.destination
    dest.mkdir(parents=True, exist_ok=True)
    info = {"dependencies": json.loads((PROJECT / "deps.lock.json").read_text()),
            "artifacts": {}, "ble_enabled": True}
    try:
        info["source_commit"] = subprocess.check_output(["git", "-C", str(PROJECT), "rev-parse", "HEAD"],
                                                       stderr=subprocess.DEVNULL).decode().strip()
        info["source_dirty"] = bool(subprocess.check_output(["git", "-C", str(PROJECT), "status", "--porcelain"]))
    except subprocess.CalledProcessError:
        info["source_commit"] = "uncommitted"
    for suffix in ("uf2", "elf"):
        name = "tufty-nes-flash." + suffix
        shutil.copy2(args.build / name, dest / name)
        data = (dest / name).read_bytes()
        info["artifacts"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    resource_info = json.loads((args.build / "resources/resource-images.json").read_text())
    info["resource_images"] = resource_info
    for obsolete in ("tufty-nes-rom.uf2", "tufty-nes-full.uf2"):
        (dest / obsolete).unlink(missing_ok=True)
    for name in ("tufty-nes-with-romfs.uf2", "tufty-nes-with-filesystem.uf2", "rom.bin", "system.bin", "state.bin", "resource-images.json"):
        shutil.copy2(args.build / "resources" / name, dest / name)
        data = (dest / name).read_bytes()
        info["artifacts"][name] = {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    for name in ("README.md", "LICENSE", "deps.lock.json"):
        shutil.copy2(PROJECT / name, dest / name)
    (dest / "resources").mkdir(exist_ok=True)
    shutil.copy2(PROJECT / "resources/README.md", dest / "resources/README.md")
    docs = dest / "docs"
    docs.mkdir(exist_ok=True)
    for source in sorted((PROJECT / "docs").glob("*.md")):
        shutil.copy2(source, docs / source.name)
    # This configuration entry was removed; do not retain it in reused packages.
    examples = dest / "examples"
    (examples / "gamepad.json").unlink(missing_ok=True)
    if examples.is_dir() and not any(examples.iterdir()):
        examples.rmdir()
    licenses = dest / "licenses"
    licenses.mkdir(exist_ok=True)
    for source in (PROJECT / "licenses").iterdir():
        if source.is_file():
            shutil.copy2(source, licenses / source.name)
    for source, name in (("littlefs/LICENSE.md", "LITTLEFS-LICENSE"), ("cjson/LICENSE", "CJSON-LICENSE"),
                         ("pico-sdk/LICENSE.TXT", "PICO-SDK-LICENSE"),
                         ("pico-sdk/lib/tinyusb/LICENSE", "TINYUSB-LICENSE"), ("picotool/LICENSE.TXT", "PICOTOOL-LICENSE"),
                         ("pico-sdk/lib/btstack/LICENSE", "BTSTACK-LICENSE"),
                         ("pico-sdk/src/rp2_common/pico_btstack/LICENSE.RP", "BTSTACK-LICENSE.RP"),
                         ("pico-sdk/lib/btstack/3rd-party/micro-ecc/LICENSE.txt", "MICRO-ECC-LICENSE"),
                         ("pico-sdk/lib/cyw43-driver/LICENSE", "CYW43-LICENSE"),
                         ("pico-sdk/lib/cyw43-driver/LICENSE.RP", "CYW43-LICENSE.RP")):
        shutil.copy2(PROJECT / "lib" / source, licenses / name)
    fatfs = (PROJECT / "lib/fatfs/source/ff.c").read_text()
    (licenses / "FATFS-LICENSE").write_text("\n".join(fatfs.splitlines()[:19]) + "\n")
    info["patch_sha256"] = {str(p.relative_to(PROJECT)): hashlib.sha256(p.read_bytes()).hexdigest()
                             for p in sorted((PROJECT / "patch").rglob("*.patch"))}
    (dest / "build-info.json").write_text(json.dumps(info, indent=2) + "\n")


if __name__ == "__main__":
    main()
