"""Protocol corruption, resource packing and keyboard-order regression checks."""
import importlib.util
import json
import shutil
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "build_game_index.py"
spec = importlib.util.spec_from_file_location("build_game_index", SCRIPT)
builder = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = builder
spec.loader.exec_module(builder)


def repair_crc(data):
    data[20:24] = bytes(4)
    struct.pack_into("<I", data, 20, zlib.crc32(data))
    return bytes(data)


class IndexProtocolTests(unittest.TestCase):
    def setUp(self):
        self.games = [builder.Game(Path("Zelda.NES"), "A Display Title", "First paragraph.\n\nSecond.", Path("Zelda.565")),
                      builder.Game(Path("Alpha.nes"), "Z Display Title", "")]
        self.data = builder.build_index(self.games)

    def test_wire_format_and_original_extension(self):
        # Read the published byte contract independently of the decoder.
        header = struct.unpack_from("<4sHHHHIIII4B", self.data)
        self.assertEqual(header[:7], (b"GIDX", 1, 32, 2, 16, 64, len(self.data)))
        self.assertEqual(header[8:], (0, 58, 80, 1, 0))
        self.assertEqual(repair_crc(bytearray(self.data)), self.data)
        strings = self.data[64:]
        first = struct.unpack_from("<IIII", self.data, 32)
        self.assertEqual(first[3], 3)
        self.assertEqual(strings[first[0]:].split(b"\0", 1)[0], b"Zelda.NES")
        self.assertEqual(struct.unpack_from("<IIII", self.data, 48)[2:], (0, 0))
        records = builder.validate_index(self.data)
        self.assertEqual([r["rom"] for r in records], ["Zelda.NES", "Alpha.nes"])
        self.assertEqual(records[0]["description"], self.games[0].description)

    def test_crc_detects_corruption(self):
        data = bytearray(self.data)
        data[-2] ^= 1
        with self.assertRaisesRegex(ValueError, "CRC32"):
            builder.validate_index(bytes(data))

    def test_malformed_records_fail_even_with_valid_crc(self):
        def set_value(offset, value):
            data = bytearray(self.data)
            struct.pack_into("<I", data, offset, value)
            return repair_crc(data)
        for data in [set_value(32, 0xffffffff), set_value(32, 2),
                     set_value(44, 0x80), set_value(44, 1), set_value(12, 63)]:
            with self.subTest(data=data[:48]), self.assertRaises(ValueError):
                builder.validate_index(data)
        unterminated = bytearray(self.data)
        unterminated[-1] = ord("x")
        with self.assertRaisesRegex(ValueError, "Unterminated"):
            builder.validate_index(repair_crc(unterminated))

    def test_string_controls_and_limits(self):
        with self.assertRaisesRegex(ValueError, "exceeds limits"):
            builder.build_index([builder.Game(Path("Big.nes"), "Big", "x" * (256 * 1024))])
        with self.assertRaises(ValueError):
            builder.normalize_text("a\x00b", "description", True)
        with self.assertRaises(ValueError):
            builder.normalize_text("中文", "description", True)


class ResourcesAndCliTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="tufty-index-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.work = self.root / "work"
        self.work.mkdir()
        (self.work / "Zebra.NES").write_bytes(b"fixture ROM Z")
        (self.work / "Zebra.565").write_bytes(bytes(9280))
        (self.work / "Zebra.TXT").write_text('<game><name>A &amp; B</name><desc>First &ldquo;quote&rdquo;.<br>Next.\n\nLast.</desc><developer>IGNORED</developer></game>')
        (self.work / "Alpha.nes").write_bytes(b"fixture ROM A")
        (self.work / "Plain.nes").write_bytes(b"fixture ROM P")
        (self.work / "Plain.txt").write_text("\ufeffA plain introduction.\r\n\r\nAnother line.")

    def cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), "--input", str(self.work), *args],
                              capture_output=True, text=True)

    def test_xml_plaintext_and_title_sort(self):
        games = builder.scan_games(self.work)
        order, removed = builder.ordered_games(games, self.work / "game-order.json")
        self.assertEqual([g.rom.name for g in order], ["Zebra.NES", "Alpha.nes", "Plain.nes"])
        self.assertEqual(order[0].title, "A & B")
        self.assertEqual(order[0].description, 'First "quote".\nNext.\n\nLast.')
        self.assertNotIn("IGNORED", order[0].description)
        self.assertEqual(order[-1].description, "A plain introduction.\n\nAnother line.")
        self.assertFalse(removed)

    def test_only_565_covers_are_used(self):
        # Legacy suffixes are ignored, even when malformed or present alongside .565.
        (self.work / "Alpha.rgb565").write_bytes(b"legacy")
        (self.work / "Zebra.rgb565").write_bytes(b"legacy")
        games = {game.rom.name: game for game in builder.scan_games(self.work)}
        self.assertIsNone(games["Alpha.nes"].cover)
        self.assertEqual(games["Zebra.NES"].cover.name, "Zebra.565")
        output = self.root / "new-covers"
        result = self.cli("--output", str(output), "--yes")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(list(output.glob("*.rgb565")))
        self.assertTrue((output / "Zebra.565").is_file())

    def test_saved_order_and_added_removed_games(self):
        path = self.work / "game-order.json"
        path.write_text(json.dumps(["Plain.nes", "Gone.nes", "zebra.nes"]))
        games, removed = builder.ordered_games(builder.scan_games(self.work), path)
        self.assertEqual([g.rom.name for g in games], ["Plain.nes", "Zebra.NES", "Alpha.nes"])
        self.assertEqual(removed, ["Gone.nes"])
        path.write_text('["Zebra.NES", "zebra.nes"]')
        with self.assertRaisesRegex(ValueError, "duplicate"):
            builder.ordered_games(builder.scan_games(self.work), path)

    def test_inplace_and_deployment_bundle(self):
        original = {p.name: p.read_bytes() for p in self.work.iterdir()}
        result = self.cli("--yes")
        self.assertEqual(result.returncode, 0, result.stderr)
        records = builder.validate_index((self.work / "games.idx").read_bytes())
        self.assertEqual([r["flags"] for r in records], [3, 0, 2])
        for name, data in original.items():
            self.assertEqual((self.work / name).read_bytes(), data)
        output = self.root / "device-roms"
        result = self.cli("--output", str(output), "--yes")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual({p.name for p in output.iterdir()}, {"games.idx", "Zebra.NES", "Alpha.nes", "Plain.nes", "Zebra.565"})
        self.assertEqual((output / "games.idx").read_bytes(), (self.work / "games.idx").read_bytes())
        result = self.cli("--output", str(output), "--yes")
        self.assertNotEqual(result.returncode, 0)

    def test_failed_preflight_preserves_index_and_order(self):
        index = self.work / "games.idx"
        order = self.work / "game-order.json"
        index.write_bytes(b"previous index")
        order.write_text('["Plain.nes"]')
        (self.work / "Zebra.565").write_bytes(bytes(60 * 72 * 2))
        result = self.cli("--yes")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("9280", result.stderr)
        self.assertEqual(index.read_bytes(), b"previous index")
        self.assertEqual(order.read_text(), '["Plain.nes"]')

    def test_non_tty_does_not_generate(self):
        result = self.cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--yes", result.stderr)
        self.assertFalse((self.work / "games.idx").exists())
        self.assertFalse((self.work / "game-order.json").exists())

    def test_default_input_independent_of_working_directory(self):
        tools = self.root / "project" / "tools"
        tools.mkdir(parents=True)
        shutil.copyfile(SCRIPT, tools / SCRIPT.name)
        roms = tools.parent / "roms"
        roms.mkdir()
        (roms / "Default.NES").write_bytes(b"fixture")
        result = subprocess.run([sys.executable, str(tools / SCRIPT.name), "--yes"],
                                cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        records = builder.validate_index((roms / "games.idx").read_bytes())
        self.assertEqual(records[0]["rom"], "Default.NES")

    def test_case_conflicts_and_bad_order_paths(self):
        (self.work / "zebra.nes").write_bytes(b"different ROM")
        with self.assertRaisesRegex(ValueError, "conflict"):
            builder.scan_games(self.work)
        (self.work / "zebra.nes").unlink()
        order = self.work / "game-order.json"
        order.write_text('["../Zebra.NES"]')
        with self.assertRaises(ValueError):
            builder.ordered_games(builder.scan_games(self.work), order)
        with self.assertRaises(ValueError):
            builder.validate_rom_name("Game..NES")


class KeyboardOrderTests(unittest.TestCase):
    def editor(self):
        return builder.OrderEditor([builder.Game(Path(f"{name}.nes"), name, "") for name in "ABCD"])

    def test_enter_fixes_item_then_confirms_whole_list(self):
        editor = self.editor()
        editor.handle("s")
        editor.handle("x")
        editor.handle("down")
        editor.handle("down")
        self.assertEqual([g.title for g in editor.games], list("BCAD"))
        self.assertIsNone(editor.handle("enter"))
        self.assertIsNone(editor.picked_from)
        self.assertEqual(editor.handle("enter"), "build")

    def test_escape_undoes_pickup_but_keeps_previous_fixed_move(self):
        editor = self.editor()
        for key in ["s", "x", "down", "enter", "down", "x", "up", "up", "escape"]:
            editor.handle(key)
        self.assertEqual([g.title for g in editor.games], list("BACD"))
        self.assertEqual(editor.cursor, 2)
        editor.handle("escape")
        self.assertFalse(editor.sorting)
        self.assertEqual(editor.handle("q"), "cancel")

    def test_preview_enter_and_movement_boundaries(self):
        editor = self.editor()
        editor.handle("x")
        self.assertIsNone(editor.picked_from)
        editor.handle("up")
        self.assertEqual(editor.cursor, 0)
        for _ in range(10):
            editor.handle("down")
        self.assertEqual(editor.cursor, 3)
        self.assertEqual([g.title for g in editor.games], list("ABCD"))
        self.assertEqual(editor.handle("enter"), "build")


if __name__ == "__main__":
    unittest.main()
