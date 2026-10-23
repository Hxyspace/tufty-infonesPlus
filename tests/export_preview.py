#!/usr/bin/env python3
"""Export host/SDL checkpoints and real-core ROM frames into one local HTML report."""
import argparse
from datetime import datetime, timezone
import html
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(PROJECT / "tools"))
from build_game_index import validate_index
from preview import prepare
import resource_images


class Export:
    def __init__(self, args, work):
        self.args, self.work = args, work
        self.checks, self.roms, self.sections = [], [], []
        (work / "logs").mkdir()

    def run(self, name, command, env=None, rom=False):
        log = self.work / "logs" / (name + ".log")
        command = [str(part) for part in command]
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", **(env or {}))
        # Do not inherit unrelated screenshot destinations from a caller's shell.
        for key in ("TUFTY_PREVIEW", "TUFTY_PREVIEW_DIR", "TUFTY_SETTINGS_PREVIEW",
                    "TUFTY_SETTINGS_ASSETS", "TUFTY_DISK_PREVIEW"):
            if key not in (env or {}):
                environment.pop(key, None)
        try:
            result = subprocess.run(command, cwd=PROJECT, env=environment, text=True,
                                    encoding="utf-8", errors="replace", capture_output=True,
                                    timeout=self.args.timeout)
        except subprocess.TimeoutExpired as error:
            log.write_bytes((error.stdout or b"") + (error.stderr or b""))
            self.checks.append(dict(name=name, command=command, status="TIMEOUT",
                                    log=self.relative(log)))
            raise RuntimeError(f"{name} timed out; see {log}") from error
        log.write_text(result.stdout + result.stderr, encoding="utf-8")
        diagnostic = "Sanitizer" in result.stderr or "runtime error:" in result.stderr
        # A well-formed loader rejection is reportable; crashes/invariant failures are not.
        rejected = (rom and result.returncode == 1 and not diagnostic
                    and any(line.startswith("ok=0 error=") and " frames=0 " in line
                            and " allocations=0 ROM_unchanged=1 " in line
                            for line in result.stdout.splitlines()))
        status = "PASS" if result.returncode == 0 and not diagnostic else "REJECT" if rejected else "FAIL"
        self.checks.append(dict(name=name, command=command, returncode=result.returncode,
                                status=status, log=self.relative(log)))
        if status == "FAIL":
            raise RuntimeError(f"{name} failed; see {log}\n{result.stderr[-1500:]}")
        return result

    def relative(self, path):
        return path.relative_to(self.work).as_posix()

    def executable(self, name, business=False):
        directory = "tests/business" if business else "host" if name == "tufty_preview" else "tests"
        path = self.args.build_dir / directory / (name + (".exe" if os.name == "nt" else ""))
        if not path.is_file():
            raise ValueError(f"Missing executable: {path}; run without --no-build")
        return path

    def section(self, title, paths, labels=None):
        from PIL import Image
        cards = []
        for path in sorted(paths, key=lambda p: (0, int(p.stem)) if p.stem.isdigit() else (1, p.stem)):
            target = path.with_suffix(".png")
            with Image.open(path) as image:
                width, height = image.size
                image.save(target)
            cards.append(dict(path=self.relative(target), width=width, height=height,
                              label=(labels or {}).get(path.stem, path.stem)))
        self.sections.append(dict(title=title, cards=cards))
        return len(cards)

    def host(self, records):
        settings, disk, library = [self.work / "host" / name for name in ("settings", "disk", "library")]
        for directory in (settings, disk, library):
            directory.mkdir(parents=True)
        self.run("host-settings", [self.executable("settings_tests", True)], {
            "TUFTY_SETTINGS_PREVIEW": str(settings), "TUFTY_SETTINGS_ASSETS": str(self.args.resources)})
        self.run("host-disk", [self.executable("disk_ui_tests", True)], {"TUFTY_DISK_PREVIEW": str(disk)})
        self.run("host-library-fixture", [sys.executable, "-B", PROJECT / "tests/business/game_index_fixture.py",
                                        self.executable("storage_tests", True), "index_valid"],
                 {"TUFTY_PREVIEW": str(library / "fixture.ppm")})
        image = self.work / "host/system.bin"
        resource_images.fat(self.args.resources / "system", image)
        # The isolated catalog test mounts FAT only. Supply its built-in background fallback
        # in this disposable FAT image, preserving the selected resources on disk.
        background = self.args.resources / "rom/nes-background.565"
        assets = self.args.resources / "system/assets"
        if not (assets / "nes-background.565").exists() and background.is_file():
            if not assets.exists():
                self.run("host-assets", ["mmd", "-i", image, "::/assets"])
            self.run("host-background", ["mcopy", "-i", image, background, "::/assets/nes-background.565"])
        manifest = self.work / "host/catalog.json"
        manifest.write_text(json.dumps(records, ensure_ascii=False), encoding="utf-8")
        self.run("host-catalog", [self.executable("storage_tests", True), image, "4096", "catalog", manifest],
                 {"TUFTY_PREVIEW_DIR": str(library)})
        settings_count = self.section("Host · Settings fixtures", settings.glob("*.ppm"))
        library_count = self.section("Host · Game library", library.glob("*.ppm"),
                                     {str(i): record["title"] for i, record in enumerate(records)})
        disk_count = self.section("Host · USB Disk state fixtures", disk.glob("*.ppm"))
        if not settings_count or library_count != len(records) + 1 or disk_count != 5:
            raise RuntimeError("Host screenshots missing; rebuild the export targets")

    def sdl(self, records):
        directory = self.work / "sdl"
        directory.mkdir()
        executable = self.executable("tufty_preview")
        # Regression checkpoints intentionally use the bundled resources and controlled state.
        self.run("sdl-regression", [sys.executable, "-B", PROJECT / "tests/test_preview.py",
                                    executable, directory / "regression"])
        if not self.section("SDL · Integration checkpoints (bundled resources)", (directory / "regression").glob("*.bmp")):
            raise RuntimeError("SDL regression produced no screenshots")
        images = prepare(self.args.resources, directory / "images", reset=True)
        library = directory / "library"
        library.mkdir()
        lines = ["0 source none", f"20 expect count {len(records)}", "20 expect indexed 1"]
        # Local keys also work with remote input disabled by the selected configuration.
        if records:
            lines += ["21 press Q", "22 release Q"]
        lines += ["50 expect selected 0", "50 shot settings.bmp"]
        for i in range(len(records)):
            at = 60 + i * 40
            lines += [f"{at} press E", f"{at + 1} release E",
                      f"{at + 30} expect selected {i + 1}", f"{at + 30} shot {i}.bmp"]
        script = library / "catalog.txt"
        script.write_text("\n".join(lines) + "\n", encoding="utf-8")
        self.run("sdl-catalog", [executable, "--images", images, "--headless", "--script", script])
        count = self.section("SDL · Selected game library", library.glob("*.bmp"),
                             {str(i): record["title"] for i, record in enumerate(records)})
        if count != len(records) + 1:
            raise RuntimeError("SDL catalog screenshots missing")

    def core(self, records):
        directory = self.work / "core"
        directory.mkdir()
        labels = {}
        for i, record in enumerate(records):
            key = f"rom-{i:03d}"
            output = directory / f"{i}.ppm"
            result = self.run(key, [self.executable("rom_probe"),
                                   self.args.resources / "system/roms" / record["rom"],
                                   "--frames", self.args.rom_frames, "--start-frame", "20",
                                   "--output", output], rom=True)
            summary = next((line for line in result.stdout.splitlines() if line.startswith("ok=")), "")
            self.roms.append(dict(title=record["title"], rom=record["rom"], summary=summary,
                                  status=self.checks[-1]["status"], log=self.checks[-1]["log"]))
            labels[str(i)] = f"{record['title']} · frame {self.args.rom_frames}"
            print(f"{i + 1}/{len(records)} {self.checks[-1]['status']}: {record['rom']}", flush=True)
        self.section("NES · Actual core frames (scripted Start input)", directory.glob("*.ppm"), labels)

    def render(self, error=None):
        report = dict(mode=self.args.mode, resources=str(self.args.resources),
                      build_dir=str(self.args.build_dir), reused_build=self.args.no_build,
                      rom_frames=self.args.rom_frames, checks=self.checks, roms=self.roms,
                      sections=self.sections, error=str(error) if error else None)
        (self.work / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        escape = html.escape
        page = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
                '<meta name="viewport" content="width=device-width,initial-scale=1">',
                '<title>Tufty preview report</title><style>',
                'body{background:#181a1d;color:#eee;font:15px system-ui;margin:24px}a{color:#9dd9fa}',
                '.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:16px}',
                'figure{margin:0;padding:12px;background:#282c31}img{display:block;width:320px;max-width:100%;height:auto;image-rendering:pixelated}',
                'figcaption,td{overflow-wrap:anywhere}figcaption{margin-top:10px}section{margin-top:30px}',
                'table{border-collapse:collapse;width:100%}td,th{text-align:left;padding:8px;border-bottom:1px solid #555}',
                '.error{color:#ff8a8a}nav a{margin-right:18px}p{line-height:1.6}</style></head><body>',
                '<h1>Tufty preview report</h1>',
                f'<p>Mode: {escape(self.args.mode)} · Resources: {escape(str(self.args.resources))}</p>',
                '<p>Actual project renderers and NES core; hardware services are simulated. ',
                'Fixtures use controlled test state; SDL integration checkpoints use bundled resources. ',
                'ROM frames check loading and brief execution, not full compatibility or device performance. ',
                'Images load lazily. <a href="results.json">Results JSON</a></p>']
        if error:
            page.append(f'<p class="error">Export incomplete: {escape(str(error))}</p>')
        page.append('<nav>' + ''.join(f'<a href="#s{i}">{escape(s["title"])}</a>' for i, s in enumerate(self.sections)) + '</nav>')
        for i, section in enumerate(self.sections):
            page.append(f'<section id="s{i}"><h2>{escape(section["title"])} ({len(section["cards"])})</h2><div class="grid">')
            for card in section["cards"]:
                path, label = escape(card["path"], quote=True), escape(card["label"], quote=True)
                page.append(f'<figure><a href="{path}"><img loading="lazy" decoding="async" src="{path}" '
                            f'width="{card["width"]}" height="{card["height"]}" alt="{label}"></a><figcaption>{label}</figcaption></figure>')
            page.append('</div></section>')
        page.append('<section><h2>ROM results</h2><table><tr><th>Game</th><th>Status</th><th>Core result</th></tr>')
        for rom in self.roms:
            page.append(f'<tr><td>{escape(rom["title"])}</td><td>{rom["status"]}</td><td>'
                        f'<a href="{escape(rom["log"], quote=True)}">{escape(rom["summary"])}</a></td></tr>')
        page.append('</table></section><section><h2>Checks and logs</h2><ul>')
        for check in self.checks:
            page.append(f'<li>{check["status"]} · <a href="{escape(check["log"], quote=True)}">{escape(check["name"])}</a></li>')
        page.append('</ul></section></body></html>')
        (self.work / "index.html").write_text(''.join(page), encoding="utf-8")


def build(args):
    host, sdl = args.mode in ("host", "all"), args.mode in ("sdl", "all")
    deps = [sys.executable, "-B", PROJECT / "tools/deps.py", "--profile", "host"]
    if host:
        deps.append("--radio")
    subprocess.run([str(p) for p in deps], check=True, cwd=PROJECT)
    subprocess.run(["cmake", "-S", str(PROJECT), "-B", str(args.build_dir),
                    "-DTUFTY_FIRMWARE=OFF", "-DBUILD_TESTING=ON", "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    f"-DPython3_EXECUTABLE={sys.executable}",
                    f"-DTUFTY_BUSINESS_TESTS={'ON' if host else 'OFF'}",
                    f"-DTUFTY_HOST_PREVIEW={'ON' if sdl else 'OFF'}", "-DNES_SANITIZERS=OFF"], check=True)
    targets = ["rom_probe"]
    if host:
        targets += ["settings_tests", "disk_ui_tests", "storage_tests"]
    if sdl:
        targets.append("tufty_preview")
    subprocess.run(["cmake", "--build", str(args.build_dir), "--target", *targets,
                    "--parallel", os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL", "4")], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("host", "sdl", "all"), default="all")
    parser.add_argument("--resources", type=Path, default=PROJECT / "resources")
    parser.add_argument("--build-dir", type=Path, default=PROJECT / "build/preview-export")
    parser.add_argument("--output", type=Path, default=PROJECT / ".tmp/preview-report")
    parser.add_argument("--no-build", action="store_true", help="Reuse matching executables, including sanitizer builds")
    parser.add_argument("--rom-frames", type=int, default=120, help="Frames per indexed ROM (default: 120)")
    parser.add_argument("--timeout", type=int, default=180, help="Timeout per test/ROM in seconds")
    args = parser.parse_args()
    try:
        if not 1 <= args.rom_frames <= 1000000 or args.timeout < 1:
            raise ValueError("--rom-frames must be 1..1000000 and --timeout must be positive")
        for name in ("resources", "build_dir", "output"):
            setattr(args, name, getattr(args, name).resolve())
        for source in (args.resources, PROJECT / "resources"):
            for target in (args.output, args.build_dir):
                if target == source or source in target.parents or target in source.parents:
                    raise ValueError("Output/build and resource trees must be separate")
        for name in ("rom", "system", "state"):
            resource_images.entries(args.resources / name)
        # Fail early on an invalid/missing index instead of silently reporting another catalog.
        records = validate_index((args.resources / "system/roms/games.idx").read_bytes())
        from PIL import Image  # Check the optional report dependency before building/running.
        del Image
        marker = args.output / ".tufty-preview-export"
        if (args.output / "index.html").exists() and not marker.is_file():
            raise ValueError("Output already contains an unrelated index.html; choose another --output")
        if not args.no_build:
            build(args)
        runs = args.output / "runs"
        runs.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-")
        work = Path(tempfile.mkdtemp(prefix=stamp, dir=runs))
        export, failure = Export(args, work), None
        try:
            if args.mode in ("host", "all"):
                export.host(records)
            if args.mode in ("sdl", "all"):
                export.sdl(records)
            export.core(records)
        except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
            failure = error
        export.render(failure)
        relative = work.relative_to(args.output).as_posix() + "/index.html"
        (args.output / "index.html").write_text('<!doctype html><meta charset="utf-8">'
            f'<meta http-equiv="refresh" content="0;url={relative}">'
            f'<a href="{relative}">Open latest preview report</a>\n', encoding="utf-8")
        marker.write_text("Generated by tests/export_preview.py; runs are retained.\n", encoding="utf-8")
        print(f"Report: {args.output / 'index.html'}", flush=True)
        if failure:
            raise RuntimeError(str(failure))
    except (ImportError, ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"Export failed: {error}\n")


if __name__ == "__main__":
    main()
