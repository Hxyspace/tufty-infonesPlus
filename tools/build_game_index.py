#!/usr/bin/env python3
"""Build games.idx with keyboard ordering; English text is wrapped by firmware."""
import argparse
from dataclasses import dataclass
from html import unescape
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import tempfile
import zlib

HEADER = struct.Struct("<4sHHHHIIII4B")
RECORD = struct.Struct("<IIII")
MAX_GAMES = 128
MAX_INDEX_BYTES = 256 * 1024
COVER_BYTES = 58 * 80 * 2
HAS_COVER = 1
HAS_DESCRIPTION = 2
PUNCTUATION = str.maketrans({
    "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'",
    "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u201f": '"',
    "\u2010": "-", "\u2011": "-", "\u2012": "-", "\u2013": "-",
    "\u2014": "--", "\u2212": "-", "\u2026": "...",
    "\u00a0": " ", "\u202f": " ",
})


def ascii_text(text, label, multiline=False):
    for character in text:
        if not (32 <= ord(character) <= 126 or (multiline and character == "\n")):
            raise ValueError(f"{label}: unsupported U+{ord(character):04X} {character!r}; prepare English/ASCII text first")
    return text


def normalize_text(text, label, multiline=False):
    text = text.translate(PUNCTUATION).replace("\r\n", "\n").replace("\r", "\n")
    # Reject unsupported characters before whitespace normalization can hide them.
    ascii_text(text.replace("\t", " "), label, multiline=True)
    if multiline:
        lines = [re.sub(r"[ \t]+", " ", line).strip() for line in text.split("\n")]
        return re.sub(r"\n{3,}", "\n\n", "\n".join(lines)).strip()
    return re.sub(r"[ \t\n]+", " ", text).strip()


def validate_rom_name(name):
    ascii_text(name, "ROM filename")
    if (not name or len(name) > 255 or Path(name).suffix.lower() != ".nes"
            or ".." in name or any(c in name for c in '/\\:"<>|?*') or name.endswith((" ", "."))):
        raise ValueError(f"Invalid ROM leaf filename: {name!r}")


class MetadataText(HTMLParser):
    """Read name/desc without requiring third-party XML entities or an XML parser."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.fields = {}
        self.active = None

    def handle_starttag(self, tag, attrs):
        if tag in ("name", "desc"):
            if self.active is not None or tag in self.fields:
                raise ValueError(f"Duplicate/nested metadata field: {tag}")
            self.active = tag
            self.fields[tag] = []
        elif self.active and tag in ("br", "p", "div"):
            self.fields[self.active].append("\n" if tag == "br" else "\n\n")

    def handle_endtag(self, tag):
        if tag in ("name", "desc"):
            if tag != self.active:
                raise ValueError(f"Mismatched metadata closing tag: {tag}")
            self.active = None
        elif self.active and tag in ("p", "div"):
            self.fields[self.active].append("\n\n")

    def handle_data(self, data):
        if self.active:
            self.fields[self.active].append(data)

    def unknown_decl(self, data):
        if data.startswith("CDATA["):
            self.handle_data(data[6:])
        else:
            raise ValueError(f"Unsupported metadata declaration: {data}")


def read_text(path, fallback_title):
    title, description = fallback_title, ""
    if path:
        try:
            raw = path.read_text(encoding="utf-8-sig")
            if re.search(r"<(?:game|name|desc)\b", raw, re.IGNORECASE):
                parser = MetadataText()
                parser.feed(raw)
                parser.close()
                if parser.active:
                    raise ValueError(f"Unclosed metadata field: {parser.active}")
                title = "".join(parser.fields.get("name", [])) or fallback_title
                description = "".join(parser.fields.get("desc", []))
            else:
                description = unescape(raw)
        except ValueError as exc:
            raise ValueError(f"{path}: {exc}") from exc
    label = str(path) if path else fallback_title
    title = normalize_text(title, f"{label} title")
    if not title:
        title = normalize_text(fallback_title, f"{label} fallback title")
    description = normalize_text(description, f"{label} description", multiline=True)
    return title, description


@dataclass(frozen=True)
class Game:
    rom: Path
    title: str
    description: str
    cover: Path | None = None

    @property
    def flags(self):
        return (HAS_COVER if self.cover else 0) | (HAS_DESCRIPTION if self.description else 0)


def scan_games(directory):
    files = sorted((p for p in directory.iterdir() if p.is_file()), key=lambda p: p.name)
    resources, roms = {}, []
    for path in files:
        extension = path.suffix.lower()
        if extension not in (".nes", ".565", ".txt"):
            continue
        key = (path.stem.casefold(), extension)
        if key in resources:
            raise ValueError(f"Case-insensitive resource name conflict: {resources[key]} and {path}")
        resources[key] = path
        if extension == ".nes":
            validate_rom_name(path.name)
            roms.append(path)
    if not roms:
        raise ValueError(f"No .nes/.NES files found in {directory}")
    if len(roms) > MAX_GAMES:
        raise ValueError(f"Too many ROMs: {len(roms)}; maximum {MAX_GAMES}")
    games = []
    for rom in roms:
        with rom.open("rb") as source:
            if not source.read(1):
                raise ValueError(f"Empty ROM: {rom}")
        cover = resources.get((rom.stem.casefold(), ".565"))
        if cover:
            with cover.open("rb") as source:
                if len(source.read(COVER_BYTES + 1)) != COVER_BYTES:
                    raise ValueError(f"{cover}: expected {COVER_BYTES} bytes (58x80 RGB565)")
        text = resources.get((rom.stem.casefold(), ".txt"))
        title, description = read_text(text, rom.stem.replace("_", " "))
        games.append(Game(rom, title, description, cover))
    return games


def ordered_games(games, order_path):
    by_name = {game.rom.name.casefold(): game for game in games}
    stored = json.loads(order_path.read_text(encoding="utf-8-sig")) if order_path.exists() else []
    if not isinstance(stored, list) or not all(isinstance(name, str) for name in stored):
        raise ValueError(f"{order_path}: expected a JSON array of ROM filenames")
    seen, selected, removed = set(), [], []
    for name in stored:
        validate_rom_name(name)
        key = name.casefold()
        if key in seen:
            raise ValueError(f"{order_path}: duplicate order entry {name!r}")
        seen.add(key)
        if key in by_name:
            selected.append(by_name[key])
        else:
            removed.append(name)
    added = sorted((game for key, game in by_name.items() if key not in seen),
                   key=lambda game: (game.title.casefold(), game.rom.name.casefold(), game.rom.name))
    return selected + added, removed


def build_index(games):
    strings, offsets = bytearray(b"\0"), {"": 0}

    def intern(text):
        if text not in offsets:
            offsets[text] = len(strings)
            strings.extend(text.encode("ascii") + b"\0")
        return offsets[text]

    records = bytearray()
    for game in games:
        records.extend(RECORD.pack(intern(game.rom.name), intern(game.title),
                                   intern(game.description), game.flags))
    strings_offset = HEADER.size + len(records)
    file_size = strings_offset + len(strings)
    if len(games) > MAX_GAMES or file_size > MAX_INDEX_BYTES:
        raise ValueError(f"Index exceeds limits: {len(games)} games, {file_size} bytes (max {MAX_GAMES}, {MAX_INDEX_BYTES})")
    header = HEADER.pack(b"GIDX", 1, HEADER.size, len(games), RECORD.size,
                         strings_offset, file_size, 0, 0, 58, 80, 1, 0)
    data = bytearray(header + records + strings)
    struct.pack_into("<I", data, 20, zlib.crc32(data) & 0xffffffff)
    return bytes(data)


def validate_index(data):
    """Decode and validate independently; also useful for PC/firmware fixtures."""
    if not HEADER.size <= len(data) <= MAX_INDEX_BYTES:
        raise ValueError("Invalid index file length")
    magic, version, header_size, count, record_size, strings_offset, file_size, crc, reserved, width, height, pixel_format, flags = HEADER.unpack_from(data)
    if (magic, version, header_size, record_size) != (b"GIDX", 1, HEADER.size, RECORD.size):
        raise ValueError("Unsupported index header/version/record size")
    if (reserved, width, height, pixel_format, flags) != (0, 58, 80, 1, 0):
        raise ValueError("Unsupported reserved fields or cover format")
    if count > MAX_GAMES or strings_offset != HEADER.size + count * RECORD.size or file_size != len(data):
        raise ValueError("Invalid index count/offset/length")
    if strings_offset >= len(data) or data[strings_offset] != 0:
        raise ValueError("Missing empty string sentinel")
    checksum_data = bytearray(data)
    checksum_data[20:24] = b"\0" * 4
    if zlib.crc32(checksum_data) & 0xffffffff != crc:
        raise ValueError("Index CRC32 mismatch")
    strings = data[strings_offset:]

    def string(offset):
        if offset >= len(strings) or (offset and strings[offset - 1] != 0):
            raise ValueError("String offset is outside the table or not at a string boundary")
        end = strings.find(b"\0", offset)
        if end < 0:
            raise ValueError("Unterminated index string")
        try:
            return strings[offset:end].decode("ascii")
        except UnicodeDecodeError as exc:
            raise ValueError("Non-ASCII index string") from exc

    decoded, names = [], set()
    for index in range(count):
        name_offset, title_offset, description_offset, flags = RECORD.unpack_from(data, HEADER.size + index * RECORD.size)
        name, title, description = string(name_offset), string(title_offset), string(description_offset)
        validate_rom_name(name)
        ascii_text(title, f"{name} title")
        ascii_text(description, f"{name} description", multiline=True)
        if not title or name.casefold() in names or flags & ~(HAS_COVER | HAS_DESCRIPTION):
            raise ValueError("Empty title, duplicate ROM or unknown record flags")
        if bool(flags & HAS_DESCRIPTION) != bool(description) or (not description and description_offset != 0):
            raise ValueError("Description flag/offset mismatch")
        names.add(name.casefold())
        decoded.append(dict(rom=name, title=title, description=description, flags=flags))
    return decoded


@dataclass
class OrderEditor:
    games: list
    cursor: int = 0
    sorting: bool = False
    picked_from: int | None = None

    def handle(self, key):
        if key == "q":
            return "cancel"
        if key == "enter":
            if self.picked_from is not None:
                self.picked_from = None
            else:
                return "build"
        elif key == "escape":
            if self.picked_from is not None:
                self.games.insert(self.picked_from, self.games.pop(self.cursor))
                self.cursor, self.picked_from = self.picked_from, None
            else:
                self.sorting = False
        elif key == "s":
            self.sorting = True
        elif key == "x" and self.sorting and self.games and self.picked_from is None:
            self.picked_from = self.cursor
        elif key in ("up", "down") and self.games:
            target = max(0, min(len(self.games) - 1, self.cursor + (-1 if key == "up" else 1)))
            if self.picked_from is not None:
                self.games.insert(target, self.games.pop(self.cursor))
            self.cursor = target
        return None


def interactive_order(games, target):
    try:
        import curses
    except ImportError as exc:
        raise ValueError("Keyboard sorting requires curses; use --yes for a non-interactive build") from exc
    editor = OrderEditor(list(games))

    def run(screen):
        screen.keypad(True)
        try:
            curses.curs_set(0)
        except curses.error:
            pass
        if hasattr(curses, "set_escdelay"):
            curses.set_escdelay(25)
        scroll = 0
        while True:
            screen.erase()
            height, width = screen.getmaxyx()

            def line(y, text, attribute=0):
                if 0 <= y < height and width > 1:
                    try:
                        screen.addnstr(y, 0, text, width - 1, attribute)
                    except curses.error:
                        pass

            moving = editor.picked_from is not None
            mode = "MOVING" if moving else "SORT" if editor.sorting else "PREVIEW"
            line(0, f"GAMES.IDX | {mode} | {len(editor.games)} games", curses.A_BOLD)
            if moving:
                help_text = "Up/Down: move game | Enter: fix position | Esc: undo move | Q: cancel"
            elif editor.sorting:
                help_text = "Up/Down: browse | X: pick game | Enter: GENERATE | Esc: preview | Q: cancel"
            else:
                help_text = "Up/Down: browse | Enter: GENERATE | S: sort | Q: cancel"
            line(1, help_text)
            line(2, f"Output: {target}")
            line(3, "Flags: C=cover D=description | Full title/name of current game below")
            capacity = max(1, height - 8)
            scroll = max(0, min(scroll, editor.cursor))
            if editor.cursor >= scroll + capacity:
                scroll = editor.cursor - capacity + 1
            for y, index in enumerate(range(scroll, min(len(editor.games), scroll + capacity)), 5):
                game = editor.games[index]
                current = index == editor.cursor
                marker = "*" if current and moving else ">" if current else " "
                resources = ("C" if game.cover else "-") + ("D" if game.description else "-")
                line(y, f"{marker} {index + 1:3d} [{resources}] {game.title}", curses.A_REVERSE if current else 0)
            if editor.games:
                game = editor.games[editor.cursor]
                line(height - 2, f"Title: {game.title}")
                line(height - 1, f"ROM:   {game.rom.name}")
            screen.refresh()
            try:
                key = screen.get_wch()
            except curses.error:
                continue
            if key == curses.KEY_UP:
                key = "up"
            elif key == curses.KEY_DOWN:
                key = "down"
            elif key in (curses.KEY_ENTER, "\n", "\r"):
                key = "enter"
            elif key == "\x1b":
                key = "escape"
            elif isinstance(key, str):
                key = key.lower()
            result = editor.handle(key)
            if result:
                return editor.games if result == "build" else None

    try:
        return curses.wrapper(run)
    except curses.error as exc:
        raise ValueError(f"Cannot initialize terminal: {exc}; use --yes to build without sorting") from exc


def atomic_write(path, data, check=None):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(prefix=f".{path.name}.", dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if check:
            check(temporary.read_bytes())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def publish(games, data, directory, output):
    if output is None:
        target = directory / "games.idx"
        atomic_write(target, data, validate_index)
        return target
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{output.name}.", dir=output.parent) as temporary:
        staging = Path(temporary) / "roms"
        staging.mkdir()
        for game in games:
            shutil.copyfile(game.rom, staging / game.rom.name)
            if game.cover:
                shutil.copyfile(game.cover, staging / (game.rom.stem + ".565"))
        atomic_write(staging / "games.idx", data, validate_index)
        if output.exists():
            raise ValueError(f"Output already exists: {output}")
        staging.rename(output)
    return output / "games.idx"


def main():
    base = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=base / "roms", help="Working ROM folder (default: project root / roms)")
    parser.add_argument("--output", type=Path, help="New deployment folder; otherwise atomically update INPUT/games.idx")
    parser.add_argument("--order", type=Path, help="PC-only order JSON (default: INPUT/game-order.json)")
    parser.add_argument("--yes", action="store_true", help="Use saved/title order without terminal interaction")
    args = parser.parse_args()
    directory = args.input.resolve()
    output = args.output.resolve() if args.output else None
    order_path = args.order.resolve() if args.order else directory / "game-order.json"
    if not directory.is_dir():
        parser.error(f"Input directory does not exist: {directory}")
    if order_path.suffix.lower() != ".json":
        parser.error("Order file must have a .json suffix")
    if output is not None:
        if output.exists():
            parser.error("Output already exists; use a new deployment directory")
        if output == directory or directory in output.parents:
            parser.error("Keep deployment output outside the working input folder")
        if order_path == output or output in order_path.parents:
            parser.error("Keep PC order JSON outside the deployment directory")
    games, removed = ordered_games(scan_games(directory), order_path)
    # Fail before terminal confirmation if the index cannot be generated.
    validate_index(build_index(games))
    if removed:
        print("Removed missing order entries: " + ", ".join(removed))
    if not args.yes:
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            parser.error("Interactive sorting requires a TTY; use --yes to generate with the current order")
        games = interactive_order(games, output / "games.idx" if output else directory / "games.idx")
        if games is None:
            print("Cancelled; index and saved order were not changed.")
            return
    data = build_index(games)
    validate_index(data)
    target = publish(games, data, directory, output)
    saved_order = json.dumps([game.rom.name for game in games], ensure_ascii=False, indent=2) + "\n"
    try:
        atomic_write(order_path, saved_order.encode("utf-8"))
    except OSError as exc:
        raise ValueError(f"Index published at {target}, but could not save PC order to {order_path}: {exc}") from exc
    print(f"Games: {len(games)}; covers: {sum(game.cover is not None for game in games)}; descriptions: {sum(bool(game.description) for game in games)}")
    print(f"Index: {target} ({len(data):,} bytes; CRC32 verified)")
    print(f"PC order: {order_path}")
    if output:
        print("Deployment contains only ROMs, .565 covers and games.idx.")
    else:
        print("Working TXT files are retained; use --output NEW_DIR for a device-only bundle.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nCancelled.", file=sys.stderr)
        sys.exit(130)
