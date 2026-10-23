#!/usr/bin/env python3
"""Prepare isolated host resources, build and run the SDL2 preview (no Pico SDK)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

import resource_images

PROJECT = Path(__file__).resolve().parents[1]


def run(args):
    subprocess.run([str(arg) for arg in args], check=True)


def fingerprint(root):
    digest = hashlib.sha256()
    for path in resource_images.entries(root):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(b"D" if path.is_dir() else b"F" + path.read_bytes())
    return digest.hexdigest()


def prepare(resources, session, reset=False):
    """Refresh read-only partitions; retain the host's writable State snapshot."""
    resources, session = resources.resolve(), session.resolve()
    if session == resources or resources in session.parents or session in resources.parents:
        raise ValueError("Host session and resource directories must be separate")
    session.mkdir(parents=True, exist_ok=True)
    manifest_path = session / "resources.json"
    previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    identity = str(resources)
    if previous and previous.get("source") != identity and not reset:
        raise ValueError("Session belongs to another resource directory; use another --session or --reset-state")
    hashes = {name: fingerprint(resources / name) for name in ("rom", "system", "state")}
    with tempfile.TemporaryDirectory(prefix=".prepare-", dir=session) as directory:
        work = Path(directory)
        for name in ("rom", "system", "state"):
            target = session / (name + ".bin")
            if name == "state":
                if target.exists() and not reset:
                    continue
            elif (previous.get(name) == hashes[name] and target.exists()
                  and target.stat().st_size == resource_images.REGIONS[name][1]):
                continue
            output = work / target.name
            if name == "rom":
                output.write_bytes(resource_images.rm1(resources / name))
            elif name == "system":
                resource_images.fat(resources / name, output)
            else:
                resource_images.state(resources / name, output, work)
            output.replace(target)
    manifest_path.write_text(json.dumps({"source": identity, **hashes}, indent=2) + "\n")
    return session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resources", type=Path, default=PROJECT / "resources")
    parser.add_argument("--build-dir", type=Path, default=PROJECT / "build/preview")
    parser.add_argument("--session", type=Path, help="Host image/save directory; defaults to a separate session per resource root")
    parser.add_argument("--reset-state", action="store_true", help="Replace host settings with the resource State image")
    parser.add_argument("--no-build", action="store_true", help="Run an already-built client")
    parser.add_argument("--prepare-only", action="store_true", help="Generate resource images without building/running SDL")
    parser.add_argument("--sanitize", action="store_true", help="Enable ASan/UBSan (Linux GCC/Clang)")
    startup = parser.add_mutually_exclusive_group()
    startup.add_argument("--settings", action="store_true", help="Start directly in Settings instead of the game library")
    startup.add_argument("--disk", action="store_true", help="Start directly in the USB Disk UI preview")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--script", type=Path)
    parser.add_argument("--frames", type=int)
    args = parser.parse_args()
    try:
        resources, build = args.resources.resolve(), args.build_dir.resolve()
        key = hashlib.sha256(str(resources).encode()).hexdigest()[:12]
        session = args.session or build / "sessions" / key
        run([os.sys.executable, "-B", PROJECT / "tools/deps.py", "--profile", "host"])
        images = prepare(resources, session, args.reset_state)
        print(f"Host resources/settings: {images}", flush=True)
        if args.prepare_only:
            return
        if not args.no_build:
            run(["cmake", "-S", PROJECT, "-B", build, "-DTUFTY_FIRMWARE=OFF",
                 "-DTUFTY_HOST_PREVIEW=ON", "-DBUILD_TESTING=ON", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                 f"-DPython3_EXECUTABLE={os.sys.executable}",
                 f"-DNES_SANITIZERS={'ON' if args.sanitize else 'OFF'}"])
            run(["cmake", "--build", build, "--target", "tufty_preview", "--parallel",
                 os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "4")])
        executable = build / "host" / ("tufty_preview.exe" if os.name == "nt" else "tufty_preview")
        command = [executable, "--images", images]
        for name in ("settings", "disk", "headless"):
            if getattr(args, name):
                command.append("--" + name)
        if args.script:
            command += ["--script", args.script.resolve()]
        if args.frames is not None:
            command += ["--frames", str(args.frames)]
        run(command)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Preview failed: {error}\n")


if __name__ == "__main__":
    main()
