"""Exercise the reusable ROM diagnostic's load/run/output/failure boundary."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="rom-probe-") as folder:
        work = Path(folder)
        rom = bytearray(16 + 32768 + 8192)
        rom[:6] = b"NES\x1a\x02\x01"
        rom[16:19] = b"\x4c\x00\x80"
        for vector in (0x7ffa, 0x7ffc, 0x7ffe):
            rom[16 + vector:18 + vector] = b"\x00\x80"
        path, output = work / "loop.NES", work / "frame.ppm"
        path.write_bytes(rom)

        def run(*args, ok=True):
            result = subprocess.run([str(executable), *map(str, args)], text=True, capture_output=True)
            if (result.returncode == 0) != ok:
                raise AssertionError(result.stdout + result.stderr)
            if "Sanitizer" in result.stderr or "runtime error:" in result.stderr:
                raise AssertionError(result.stderr)
            return result

        result = run(path, "--frames", 3, "--output", output)
        assert "frames=3" in result.stdout and "allocations=0 ROM_unchanged=1" in result.stdout
        assert output.read_bytes().startswith(b"P6\n256 240\n255\n")
        assert output.stat().st_size == len(b"P6\n256 240\n255\n") + 256 * 240 * 3
        assert path.read_bytes() == rom
        run(path, "--frames", "-1", ok=False)
        run(path, "--frames", "0", ok=False)
        run(path, "--frames", "3junk", ok=False)
        run(path, "--output", work / "missing/frame.ppm", "--frames", 1, ok=False)
        run(work / "missing.nes", ok=False)
        rom[6] = 0x80
        rom[7] = 0x10  # Mapper 24 is retained but not admitted.
        path.write_bytes(rom)
        assert "not yet admitted" in run(path, "--frames", 1, ok=False).stdout
    print("ROM probe load/run/output checks passed")


if __name__ == "__main__":
    main()
