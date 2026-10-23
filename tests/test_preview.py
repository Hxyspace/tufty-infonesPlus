"""End-to-end real UI/input/storage/core checks through the SDL host runtime."""
from pathlib import Path
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "tools"))
from preview import prepare


def main():
    executable = Path(sys.argv[1]).resolve()
    # An explicit directory is useful when collecting screenshots for review.
    temporary = tempfile.TemporaryDirectory(prefix="tufty-preview-")
    work = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else Path(temporary.name)
    work.mkdir(parents=True, exist_ok=True)
    source = PROJECT / "resources"
    original = {str(p): hashlib.sha256(p.read_bytes()).digest() for p in source.rglob("*") if p.is_file()}
    images = prepare(source, work / "images", reset=True)

    def run(name, script, settings=False, ok=True, disk=False):
        path = work / (name + ".txt")
        # Let the initial library fade complete before exercising UI/screenshots.
        lines = []
        for line in script.strip().splitlines():
            if not line.strip():
                continue
            frame, command = line.split(" ", 1)
            at = 0 if int(frame) == 0 and command.startswith("source ") else int(frame) + 20
            lines.append(f"{at} {command}")
        path.write_text("\n".join(lines) + "\n")
        command = [str(executable), "--images", str(images), "--headless", "--script", str(path)]
        if settings:
            command.append("--settings")
        if disk:
            command.append("--disk")
        result = subprocess.run(command, text=True, capture_output=True, timeout=45)
        (work / (name + ".log")).write_text(result.stdout + result.stderr)
        if (result.returncode == 0) != ok:
            raise AssertionError(name + ":\n" + result.stdout + result.stderr)
        if "Sanitizer" in result.stderr or "runtime error:" in result.stderr:
            raise AssertionError(result.stderr)
        return result

    run("settings-save", """
0 source serial
0 expect count 4
0 expect indexed 1
1 expect source serial
2 shot library.bmp
3 press Left
4 release Left
5 press Return
6 release Return
7 expect view settings
7 shot settings.bmp
8 press Return
9 release Return
10 expect page 1
11 press Return
12 release Return
13 press Right
14 release Right
15 expect brightness 85
15 press Return
16 release Return
17 press Down
18 release Down
19 press Return
20 release Return
21 expect te 0
21 shot display.bmp
22 press H
23 release H
26 expect view library
26 expect dirty 0
26 expect selected 0
27 quit
""")
    run("reload", """
0 expect brightness 85
0 expect te 1
0 expect dirty 0
1 quit
""")
    run("game", """
0 source serial
2 press Return
3 release Return
20 expect view game
20 shot game.bmp
22 press H
23 release H
26 expect view library
26 expect selected 1
26 shot return.bmp
27 quit
""")
    # An assertion in a core callback must unwind via quit and release core allocations.
    run("game-error", """
0 source serial
2 press Return
3 release Return
10 expect view deliberately-wrong
""", ok=False)
    run("save-retry", """
0 source serial
2 press Return
3 release Return
5 press Return
6 release Return
8 press Right
9 release Right
10 expect brightness 90
11 save-failure on
12 press H
13 release H
17 expect view settings
17 expect dirty 1
17 shot save-error.bmp
18 save-failure off
19 press W
20 release W
24 expect view library
24 expect dirty 0
25 quit
""", settings=True)
    run("reload-retry", """
0 expect brightness 90
1 quit
""")
    # Use local buttons while the simulated remote is disconnected for discovery.
    run("bluetooth", """
0 source none
2 press K
3 release K
5 press W
6 release W
8 expect page 2
9 press W
10 release W
12 expect page 3
13 press K
14 release K
16 press W
17 release W
20 expect page 4
20 shot searching.bmp
630 shot devices.bmp
632 press W
633 release W
635 expect page 5
636 press K
637 release K
639 press K
640 release K
642 press W
643 release W
645 expect allowed 1
645 expect dirty 1
646 shot device.bmp
648 press H
649 release H
653 expect view library
654 quit
""", settings=True)
    disk_entry = """
0 source none
2 press W
3 release W
5 press W
6 release W
8 press E
9 release E
11 press W
12 release W
14 press K
15 release K
17 press W
18 release W
19 expect te 0
20 press Q
21 release Q
23 press K
24 release K
26 press K
27 release K
29 press K
30 release K
32 press W
33 release W
34 expect page 10
35 press W
36 release W
38 press I
39 release I
41 press W
42 release W
"""
    run("disk-from-settings", disk_entry + """
46 expect view disk
46 expect dirty 0
46 expect brightness 95
46 shot disk-ready.bmp
48 disk-state transferring
49 shot disk-transferring.bmp
50 expect disk_state transferring
51 disk-state unavailable
52 shot disk-unavailable.bmp
53 expect disk_state unavailable
54 disk-state failed
55 shot disk-failed.bmp
56 expect disk_state failed
58 disk-state ejected
59 press H
60 shot disk-ejected.bmp
71 expect view disk
76 expect view disk
77 release H
79 expect view library
79 expect selected 1
79 expect te 1
79 expect dirty 0
79 shot disk-return.bmp
81 quit
""", settings=True)
    # A simulated reboot must honor "continue without saving": keep the
    # previously saved 95%, rather than silently persisting the new 100%.
    run("disk-save-bypass", disk_entry.replace("41 press W", "40 save-failure on\n41 press W") + """
46 expect view settings
46 expect dirty 1
46 expect brightness 100
47 press K
48 release K
50 press W
51 release W
55 expect view disk
55 expect brightness 95
55 expect dirty 0
56 disk-state ejected
74 expect view library
74 expect brightness 95
74 expect selected 1
75 quit
""", settings=True)
    run("disk-startup", """
0 expect view disk
0 expect disk_state ready
0 expect brightness 95
2 press H
3 release H
5 expect view disk
6 disk-state ejected
24 expect view library
24 expect selected 1
25 quit
""", disk=True)
    for image in work.glob("*.bmp"):
        data = image.read_bytes()
        assert data[:2] == b"BM" and struct.unpack_from("<ii", data, 18) == (320, 240)
        assert struct.unpack_from("<H", data, 28)[0] == 24
        offset = struct.unpack_from("<I", data, 10)[0]
        assert len({data[i:i + 3] for i in range(offset, len(data), 3)}) > 4, image
    assert original == {str(p): hashlib.sha256(p.read_bytes()).digest() for p in source.rglob("*") if p.is_file()}
    print(f"Host preview UI/input/storage/core checks passed; screenshots: {work}")
    temporary.cleanup()


if __name__ == "__main__":
    main()
