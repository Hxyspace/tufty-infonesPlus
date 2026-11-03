"""End-to-end real UI/input/storage/core checks through the SDL host runtime."""
from pathlib import Path
import hashlib
import os
import shutil
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
5 press Z
6 release Z
7 expect view settings
7 shot settings.bmp
8 press Z
9 release Z
10 expect page 1
11 press Z
12 release Z
13 press Right
14 release Right
15 expect brightness 85
15 press Z
16 release Z
17 press Down
18 release Down
19 press Down
20 release Down
21 press Down
22 release Down
23 press Down
24 release Down
25 press Z
26 release Z
27 expect te 0
27 shot display.bmp
28 press H
29 release H
32 expect view library
32 expect dirty 0
32 expect selected 0
33 quit
""")
    run("reload", """
0 expect brightness 85
0 expect te 1
0 expect dirty 0
1 quit
""")
    run("game", """
0 source serial
2 press Z
3 release Z
20 expect view game
20 shot game.bmp
22 press H
23 release H
26 expect view library
26 expect selected 1
26 shot return.bmp
27 quit
""")
    run("palette-preview", """
0 source serial
0 expect palette current
2 press Z
3 release Z
5 press Down
6 release Down
7 press Down
8 release Down
9 press Z
10 release Z
11 expect page 16
11 shot palette-current.bmp
12 press Right
13 release Right
14 shot palette-unsaturated.bmp
14 expect palette current
14 expect dirty 0
15 press Z
16 release Z
17 expect palette unsaturated-final
17 expect dirty 1
17 expect page 16
17 shot palette-applied.bmp
18 press H
19 release H
22 expect view library
22 expect dirty 0
23 quit
""", settings=True)
    run("palette-game", """
0 source serial
0 expect palette unsaturated-final
2 press Z
3 release Z
20 expect view game
20 shot game-unsaturated.bmp
22 press H
23 release H
26 expect view library
27 quit
""")
    assert (work / "palette-current.bmp").read_bytes() != (work / "palette-unsaturated.bmp").read_bytes()
    assert (work / "game.bmp").read_bytes() != (work / "game-unsaturated.bmp").read_bytes()
    # An assertion in a core callback must unwind via quit and release core allocations.
    run("game-error", """
0 source serial
2 press Z
3 release Z
10 expect view deliberately-wrong
""", ok=False)
    run("save-retry", """
0 source serial
2 press Z
3 release Z
5 press Z
6 release Z
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
16 press K
17 release K
18 press K
19 release K
20 press K
21 release K
22 press W
23 release W
23 expect te 0
24 press Q
25 release Q
27 press K
28 release K
30 press K
31 release K
33 press K
34 release K
36 press W
37 release W
38 expect page 10
38 press K
39 release K
39 press W
40 release W
42 press I
43 release I
45 press W
46 release W
"""
    run("disk-from-settings", disk_entry + """
48 expect view disk
48 expect dirty 0
48 expect brightness 95
50 shot disk-ready.bmp
52 disk-state transferring
53 shot disk-transferring.bmp
54 expect disk_state transferring
55 disk-state unavailable
56 shot disk-unavailable.bmp
57 expect disk_state unavailable
58 disk-state failed
59 shot disk-failed.bmp
60 expect disk_state failed
62 disk-state ejected
63 press H
64 shot disk-ejected.bmp
75 expect view disk
80 expect view disk
81 release H
83 expect view library
83 expect selected 1
83 expect te 1
83 expect dirty 0
83 shot disk-return.bmp
85 quit
""", settings=True)
    run("disk-write-animation", """
1 disk-state transferring
3 shot disk-write-a.bmp
20 shot disk-write-b.bmp
21 quit
""", disk=True)
    assert (work / "disk-write-a.bmp").read_bytes() != (work / "disk-write-b.bmp").read_bytes()
    # A simulated reboot must honor "continue without saving": keep the
    # previously saved 95%, rather than silently persisting the new 100%.
    run("disk-save-bypass", disk_entry.replace("45 press W", "44 save-failure on\n45 press W") + """
48 expect view settings
48 expect dirty 1
48 expect brightness 100
49 press K
50 release K
52 press W
53 release W
57 expect view disk
57 expect brightness 95
57 expect dirty 0
58 disk-state ejected
76 expect view library
76 expect brightness 95
76 expect selected 1
77 quit
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
    library_entry="""
0 source serial
1 press Down
2 release Down
3 press Down
4 release Down
5 press Down
6 release Down
7 press Z
8 release Z
9 expect page 10
14 press Z
15 release Z
16 expect page 12
16 shot game-library.bmp
"""
    run("library-priority",library_entry+"""
17 press Z
18 release Z
19 expect page 13
20 press Down
21 release Down
22 press Down
23 release Down
24 press Z
25 expect first_game Lan_Master.nes
25 expect order_pending 1
29 release Z
30 shot game-order.bmp
31 press H
32 release H
34 expect view library
34 expect order_pending 0
35 quit
""",settings=True)
    run("library-reset",library_entry+"""
17 expect first_game Lan_Master.nes
18 press Down
19 release Down
20 press Z
21 release Z
22 press Up
23 release Up
24 press Z
25 release Z
26 expect first_game cargo_shift.nes
27 press Down
28 release Down
29 press Z
30 release Z
33 expect view library
33 expect selected 0
34 expect indexed 1
35 shot library-rebuilt.bmp
36 quit
""",settings=True)
    run("restore-defaults",library_entry+"""
17 press Z
18 release Z
20 press Down
21 release Down
23 press Z
24 release Z
25 expect order_pending 1
26 press X
27 release X
29 press X
30 release X
32 expect page 10
33 press Down
34 release Down
36 press Down
37 release Down
39 press Z
40 release Z
42 shot restore-defaults.bmp
43 press Up
44 release Up
46 press Z
47 release Z
50 expect view library
50 expect brightness 80
50 expect swap 0
50 expect dirty 0
50 expect order_pending 0
50 expect first_game cargo_shift.nes
50 expect count 4
50 expect indexed 1
51 quit
""",settings=True)
    run("defaults-persisted","""
0 expect brightness 80
0 expect swap 0
0 expect first_game cargo_shift.nes
0 expect count 4
1 quit
""")
    run("game-lists", """
0 source serial
1 expect list 0
1 expect game_index 0
2 press Return
3 release Return
4 expect favorite 1
4 expect lists_pending 1
5 press Right
6 release Right
7 press Right
8 release Right
9 expect game_index 2
10 press Return
11 release Return
13 shot favorite-all.bmp
14 press Right Shift
15 release Right Shift
16 expect list 1
16 expect list_count 2
16 expect selected 1
16 expect game_index 2
18 press Right
19 release Right
20 expect game_index 0
30 shot favorites.bmp
31 press Right Shift
32 release Right Shift
33 expect list 2
33 expect list_count 0
33 expect game_index -1
34 shot recent-empty.bmp
35 press Z
36 release Z
37 expect view library
38 press Right Shift
39 release Right Shift
40 expect list 0
41 press Z
42 release Z
44 expect view game
44 expect lists_sram 0
48 press H
49 release H
52 expect view library
52 expect game_index 0
53 expect lists_pending 0
54 press Right Shift
55 release Right Shift
56 expect list 1
56 expect selected 2
56 expect game_index 0
57 press Right Shift
58 release Right Shift
59 expect list 2
59 expect game_index 0
59 expect list_count 1
61 shot recently-played.bmp
62 press Right Shift
63 release Right Shift
64 press Right Shift
65 release Right Shift
66 expect list 1
66 expect selected 2
67 press Return
68 release Return
69 expect list_count 1
69 expect selected 1
69 expect game_index 2
70 press Return
71 release Return
72 expect list_count 0
72 expect game_index -1
73 shot favorites-empty.bmp
74 press Right Shift
75 release Right Shift
76 press Right Shift
77 release Right Shift
78 press Return
79 release Return
80 expect game_index 0
80 expect favorite 1
220 expect lists_pending 0
221 quit
""")
    run("game-lists-reload", """
0 source serial
1 expect list 0
1 expect favorite 1
2 press Right Shift
3 release Right Shift
4 expect list_count 1
4 expect game_index 0
5 press Right Shift
6 release Right Shift
7 expect list_count 1
7 expect game_index 0
8 quit
""")
    run("game-lists-local", library_entry+"""
18 press Down
19 release Down
20 press Down
21 release Down
22 press Down
23 release Down
24 press Down
25 release Down
26 press Z
27 release Z
28 expect page 15
29 press Z
30 release Z
31 expect lists_pending 1
32 press H
33 release H
36 expect view library
36 expect lists_pending 0
37 quit
""",settings=True)
    run("game-lists-view", library_entry+"""
18 press Down
19 release Down
20 press Down
21 release Down
22 press Down
23 release Down
24 press Z
25 release Z
26 expect page 14
27 press Down
28 release Down
29 press Down
30 release Down
31 press Z
32 release Z
35 expect view library
35 expect list 2
35 expect list_count 1
35 expect game_index 0
36 quit
""",settings=True)
    run("game-lists-save-retry", """
0 source serial
1 save-failure on
2 press Return
3 release Return
4 expect lists_pending 1
135 expect lists_pending 1
136 press Z
137 release Z
140 expect view settings
141 save-failure off
142 press Z
143 release Z
147 expect view library
147 expect lists_pending 0
147 expect favorite 1
148 quit
""")
    run("game-lists-reset", """
0 source serial
1 press Down
2 release Down
3 press Down
4 release Down
5 press Down
6 release Down
7 press Z
8 release Z
9 press Down
10 release Down
11 press Down
12 release Down
13 press Z
14 release Z
15 press Up
16 release Up
17 press Z
18 release Z
22 expect view library
22 expect lists_pending 0
23 press Right Shift
24 release Right Shift
25 expect list_count 0
26 press Right Shift
27 release Right Shift
28 expect list_count 0
29 quit
""",settings=True)
    run("gamepad-keyboard", """
0 source serial
1 expect list 0
2 press Return
3 release Return
4 expect view library
4 expect favorite 1
5 press S
6 release S
7 expect favorite 1
8 press Escape
9 release Escape
10 press Left Shift
11 release Left Shift
12 expect list 1
13 press Right Shift
14 release Right Shift
15 expect list 2
16 press Left Shift
17 press Right Shift
18 release Left Shift
19 expect list 0
20 release Right Shift
21 press Left
22 release Left
23 press Z
24 release Z
25 expect view settings
26 press Z
27 release Z
28 expect page 1
29 press Return
30 release Return
31 press Escape
32 release Escape
33 expect page 1
34 press X
35 release X
36 expect page 0
37 press H
38 release H
41 expect view library
42 quit
""")
    run("gamepad-test", """
0 source serial
2 press Down
3 release Down
5 press Z
6 release Z
7 expect page 2
8 press Down
9 release Down
17 press Z
100 expect page 17
100 release Z
102 shot gamepad-idle.bmp
104 press X
110 shot gamepad-button.bmp
112 release X
114 expect page 17
116 press Return
117 release Return
118 press Left Shift
119 release Left Shift
120 press Left
120 press Up
122 shot gamepad-diagonal.bmp
123 release Left
123 release Up
124 expect page 17
125 press X
130 expect page 17
220 expect page 2
222 release X
225 expect page 2
230 press Z
232 release Z
233 expect page 17
235 source none
236 shot gamepad-local.bmp
238 press W
330 expect page 2
332 release W
335 press H
336 release H
340 expect view library
341 quit
""", settings=True)
    # The production Settings flow persists each position's own artwork and
    # shares the same HUD compositor with real gameplay in the SDL runtime.
    previous_position=0
    for position,row,filename in (("left",1,"Star map"),("center",0,"Brass"),("right",2,"Moon fox")):
        lines=["0 source serial"];tick=2
        def emit(command):
            nonlocal tick
            lines.append(f"{tick} {command}");tick+=2
        def tap(key):
            emit("press "+key);emit("release "+key)
        tap("Z")
        for _ in range(3): tap("Down")
        tap("Z");emit("expect page 20")
        tap("Z");emit("expect page 21")
        for _ in range(abs(row-previous_position)): tap("Down" if row>previous_position else "Up")
        tap("Z");emit("expect game_position "+position)
        if position=="left":
            tap("Down");tap("Down");tap("Z");tap("Up")
        else: tap("Down")
        tap("Z");emit("expect page 22")
        group="center" if position=="center" else "side"
        names=sorted((p.stem for p in (source/"rom/side-background"/group).glob("*.565")),key=str.casefold)
        for _ in range(names.index(filename)+1): tap("Down")
        tap("Z");emit("expect page 23")
        emit("expect side_background black")
        emit("shot side-"+position+"-preview.bmp")
        tap("Z");emit("expect side_background /rom/side-background/"+group+"/"+filename+".565")
        tap("X");tap("X")
        emit("expect fps_monitor 1")
        tap("H");emit("expect view library");emit("expect dirty 0")
        tap("Right");tap("Z")
        tick+=150
        emit("expect view game");emit("shot side-"+position+"-game.bmp")
        tap("H");emit("expect view library");emit("quit")
        run("side-"+position,"\n".join(lines),settings=True)
        previous_position=row
    run("side-reload","""
0 expect game_position right
0 expect fps_monitor 1
0 expect side_background /rom/side-background/side/Moon fox.565
1 quit
""")
    # Isolated background fixtures: never edit the user's resource collection.
    background_source=Path(temporary.name)/"background-resources"
    shutil.copytree(source,background_source)
    shutil.rmtree(background_source/"rom/background",ignore_errors=True)
    (background_source/"rom/background").mkdir()
    shutil.copyfile(source/"rom/background/background.565",background_source/"rom/background/background.565")
    external=background_source/"system/assets/background"
    shutil.rmtree(external,ignore_errors=True); external.mkdir(parents=True)
    (external/"Aurora.565").write_bytes(b"\x1f\x00"*(320*240))
    (external/"background.565").write_bytes(b"\x00\xf8"*(320*240))
    (external/"invalid.565").write_bytes(bytes(100))
    images=prepare(background_source,work/"background-images",reset=True)
    run("background-preview", """
0 source serial
0 expect background default
2 press Z
3 release Z
5 press Down
6 release Down
8 press Z
9 release Z
11 expect page 18
11 expect background_count 3
11 shot background-list.bmp
12 press Up
13 release Up
15 press Z
80 expect page 19
80 expect background default
80 expect dirty 0
81 release Z
83 shot background-external.bmp
85 press Right
86 release Right
88 shot background-default.bmp
89 expect background default
90 press Right
91 release Right
93 shot background-duplicate.bmp
94 press Z
95 release Z
97 expect background /system/assets/background/background.565
97 expect dirty 1
97 shot background-applied.bmp
98 press X
99 release X
101 expect page 18
102 press H
103 release H
107 expect view library
107 expect background_count 0
107 expect dirty 0
108 shot background-library.bmp
109 quit
""",settings=True)
    run("background-reload", """
0 source serial
0 expect background /system/assets/background/background.565
2 press Z
3 release Z
5 press Down
6 release Down
8 press Z
9 release Z
11 expect page 18
12 press Z
13 release Z
15 expect page 19
16 press Left
17 release Left
19 expect background /system/assets/background/background.565
20 press Up
21 release Up
23 press Z
24 release Z
26 expect page 18
27 press H
28 release H
31 expect view library
31 expect dirty 0
32 expect background_count 0
33 quit
""",settings=True)
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
