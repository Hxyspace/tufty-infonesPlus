import contextlib
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from prepare_roms import load_rules, main, replace_with_backup, review, supported_mappers


class RomTests(unittest.TestCase):
    def setUp(self):
        header = bytearray(b"NES\x1a" + bytes(12))
        header[4], header[5] = 2, 1
        header[6], header[7] = 0x37, 0x40  # wrong mapper 67, trainer, battery, vertical
        self.payload = bytes(range(256)) * 160  # 32 KiB PRG + 8 KiB CHR
        self.data = bytes(header) + b"T" * 512 + self.payload
        self.crc = f"{zlib.crc32(self.payload):08x}"
        self.rule = {"name": "test fixture", "crc32": self.crc, "mapper": 2,
                     "mirroring": "horizontal", "sha256": hashlib.sha256(self.payload).hexdigest()}
        self.rules = {self.crc: self.rule}

    def test_patch_preserves_program_trainer_and_other_flags(self):
        fixed = review(self.data, self.rules, {2}).fixed
        self.assertEqual(fixed[6:8], b"\x26\x00")
        self.assertEqual(fixed[:6], self.data[:6])
        self.assertEqual(fixed[8:], self.data[8:])
        self.assertEqual(review(fixed, self.rules, {2}).fixed, fixed)

    def test_four_screen_correction(self):
        data = bytearray(self.data)
        data[6] |= 8
        self.assertFalse(review(bytes(data), self.rules, {2}).fixed[6] & 9)
        self.rule["mirroring"] = "four-screen"
        self.assertEqual(review(self.data, self.rules, {2}).fixed[6] & 9, 8)

    def test_diskdude_signatures_preserve_trainer_payload_and_byte6(self):
        for signature in (b"DiskDude!", b"@iskDude!"):
            with self.subTest(signature=signature):
                dirty = bytearray(self.data)
                dirty[7:16] = signature
                result = review(bytes(dirty), {}, {3})
                self.assertFalse(result.error)
                self.assertEqual(result.fixed[:7], dirty[:7])
                self.assertEqual(result.fixed[7:16], bytes(9))
                self.assertEqual(result.fixed[16:], dirty[16:])
                self.assertTrue(any("FIX: remove DiskDude!" in n for n in result.notes))
                self.assertTrue(any("mapper high bits" in n for n in result.notes))
                self.assertEqual(review(result.fixed, {}, {3}).fixed, result.fixed)

    def test_diskdude_cleanup_precedes_checksum_mapper_correction(self):
        dirty = bytearray(self.data)
        dirty[7:16] = b"DiskDude!"
        self.rule["mapper"] = 66
        result = review(bytes(dirty), self.rules, {66})
        self.assertFalse(result.error)
        self.assertEqual(result.fixed[6:8], b"\x26\x40")
        self.assertEqual(result.fixed[8:16], bytes(8))
        self.assertEqual(result.fixed[16:], self.data[16:])

    def test_diskdude_detection_is_exact_and_header_only(self):
        clean = bytearray(self.data)
        clean[7] = 0
        clean[128:137] = b"DiskDude!"
        self.assertEqual(review(bytes(clean), {}, {3}).fixed, clean)
        for signature in (b"diskdude!", b"DiskDud?", b"\x48iskDude!", b"\x40Ni03330!"):
            with self.subTest(signature=signature):
                dirty = bytearray(self.data)
                dirty[7:16] = signature
                result = review(bytes(dirty), {}, {3, 67})
                self.assertEqual(result.fixed, dirty)
                self.assertFalse(any("FIX: remove DiskDude!" in n for n in result.notes))

    def test_diskdude_errors_leave_original_unchanged(self):
        dirty = bytearray(self.data)
        dirty[7:16] = b"DiskDude!"
        for invalid, mappers in ((bytes(dirty[:-1]), {3}), (bytes(dirty), {0})):
            with self.subTest(size=len(invalid), mappers=mappers):
                result = review(invalid, {}, mappers)
                self.assertTrue(result.error)
                self.assertEqual(result.fixed, invalid)

    def test_diskdude_cli_preview_backup_and_repeat(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(tmp)
            roms = root / "roms"
            roms.mkdir()
            path = roms / "dirty.NES"
            dirty = bytearray(self.data)
            dirty[7:16] = b"DiskDude!"
            path.write_bytes(dirty)
            rules = root / "rules.json"
            rules.write_text('{"rules": []}')
            backups = root / "backups"
            args = [str(roms), "--rules", str(rules), "--backup-dir", str(backups)]
            self.assertEqual(main(args), 0)
            self.assertEqual(path.read_bytes(), dirty)
            self.assertFalse(backups.exists())
            self.assertEqual(main(args + ["--fix"]), 0)
            fixed = path.read_bytes()
            self.assertEqual(fixed[7:16], bytes(9))
            self.assertEqual(fixed[16:], dirty[16:])
            saved = list(backups.rglob("*.bak"))
            self.assertEqual(len(saved), 1)
            self.assertEqual(saved[0].read_bytes(), dirty)
            self.assertEqual(main(args + ["--fix"]), 0)
            self.assertEqual(path.read_bytes(), fixed)
            self.assertEqual(list(backups.rglob("*.bak")), saved)
            dirty[6] = 0x50  # Mapper 5 stays unsupported after cleanup.
            path.write_bytes(dirty)
            self.assertEqual(main(args + ["--fix"]), 1)
            self.assertEqual(path.read_bytes(), dirty)
            self.assertEqual(list(backups.rglob("*.bak")), saved)

    def test_unknown_and_sha_mismatch_never_guess(self):
        self.assertEqual(review(self.data, {}, {67}).fixed, self.data)
        self.rule["sha256"] = "0" * 64
        result = review(self.data, self.rules, {67})
        self.assertEqual(result.fixed, self.data)
        self.assertTrue(any("disagrees" in n for n in result.notes))

    def test_padding_is_optional_and_payload_untouched(self):
        padded = self.data + b" \r\n\t" * 32
        self.assertEqual(review(padded, {}, {67}).fixed, padded)
        self.assertEqual(review(padded, {}, {67}, True).fixed, self.data)
        for tail in (b"\x00" * 64, b"\xff" * 64, b"data\r\n"):
            self.assertEqual(review(self.data + tail, {}, {67}, True).fixed, self.data + tail)

    def test_bad_files_and_unsupported_formats(self):
        for data in (b"", b"not a NES ROM" * 16, self.data[:-1]):
            result = review(data, self.rules, {2})
            self.assertTrue(result.error)
            self.assertEqual(result.fixed, data)
        for flag in (1, 2):  # VS, PlayChoice
            data = bytearray(self.data)
            data[7] = flag
            result = review(bytes(data), self.rules, {2})
            self.assertTrue(result.error)
            self.assertEqual(result.fixed, data)
        self.assertTrue(review(self.data, {}, {0, 1, 2}).error)

    def test_nes2_preserves_format_and_payload(self):
        data = bytearray(self.data)
        data[7] |= 8
        data[15] = 1
        result = review(bytes(data), {}, {67})
        self.assertFalse(result.error)
        self.assertEqual(result.fixed, data)
        self.assertFalse(any("reserved" in n for n in result.notes))
        fixed = review(bytes(data), self.rules, {2})
        self.assertFalse(fixed.error)
        self.assertEqual(fixed.fixed[6:8], b"\x26\x08")
        self.assertEqual(fixed.fixed[8:], data[8:])
        self.assertTrue(review(bytes(data[:-1]), {}, {67}).error)
        self.assertTrue(review(bytes(data), {}, {0}).error)

    def test_nes2_multiregion_preserves_header(self):
        data = bytearray(self.data)
        data[7] |= 8
        data[12] = 2
        data[15] = 1
        result = review(bytes(data), {}, {67})
        self.assertFalse(result.error)
        self.assertEqual(result.fixed, data)
        self.assertTrue(any("NTSC timing" in note for note in result.notes))

    def test_nes2_rejects_unrepresentable_metadata(self):
        data = bytearray(self.data)
        data[7] |= 8
        data[15] = 1
        for index, value in ((8, 0x20), (8, 1), (9, 1), (9, 15), (9, 0xF0),
                             (10, 8), (10, 0x80), (10, 0x77),
                             (11, 7), (11, 8), (11, 0x80), (11, 0x77),
                             (12, 4), (12, 6), (13, 1), (14, 1), (15, 2), (15, 64)):
            with self.subTest(index=index, value=value):
                invalid = bytearray(data)
                invalid[index] = value
                result = review(bytes(invalid), self.rules, {2, 67})
                self.assertTrue(result.error)
                self.assertEqual(result.fixed, invalid)
        data[5] = 0
        data[11] = 7
        for ram in (0, 7, 0x70):
            data[10] = ram
            self.assertFalse(review(bytes(data), {}, {67}).error)

    def test_new_boards_preserve_extended_mapper_and_submapper(self):
        for mapper, sub in ((2, 2), (3, 2), (8, 0), (206, 0), (206, 1), (11, 0), (15, 0), (18, 0), (23, 0), (23, 2), (23, 3),
                            (32, 0), (33, 0), (34, 0), (34, 1), (34, 2), (140, 0), (226, 0),
                            (22, 0), (80, 0), (207, 0), (210, 1), (210, 2),
                            (19, 0), (19, 2), (19, 3), (19, 5),
                            (70, 0), (71, 0), (177, 0), (241, 0),
                            (24, 0), (25, 0), (25, 2), (268, 0), (41, 0), (72, 0), (75, 0), (76, 0),
                            (7, 2), (78, 1), (78, 3), (79, 0), (86, 0), (87, 0), (88, 0), (89, 0), (92, 0),
                            (97, 0), (112, 0), (113, 0), (133, 0), (184, 0), (240, 0), (242, 0), (243, 0), (244, 0)):
            with self.subTest(mapper=mapper, sub=sub):
                data = bytearray(self.data)
                data[6] = (mapper & 15) << 4 | 4  # Keep the fixture's trainer.
                data[7] = (mapper & 0xf0) | 8
                data[8] = (sub << 4) | (mapper >> 8)
                result = review(bytes(data), {}, supported_mappers())
                self.assertFalse(result.error)
                self.assertEqual(result.fixed, data)
                self.assertTrue(any(f"mapper={mapper} " in note for note in result.notes))
                for unsupported in (1, 3, 15):
                    data[8] = (unsupported << 4) | (mapper >> 8)
                    self.assertEqual(review(bytes(data), {}, supported_mappers()).error,
                                     not ((mapper == 23 and unsupported == 3) or (mapper == 206 and unsupported == 1)
                                          or (mapper == 19 and unsupported in (2, 3, 5))
                                          or (mapper == 210 and unsupported in (1, 2))
                                          or (mapper == 78 and unsupported in (1, 3))
                                          or (mapper == 34 and unsupported in (1, 2))))

    def test_nes2_checksum_correction_updates_all_mapper_bits(self):
        data = bytearray(self.data)
        data[6:9] = bytes((0xc4, 8, 1))  # Mapper 268, trainer, no submapper.
        result = review(bytes(data), self.rules, {2, 268})
        self.assertFalse(result.error)
        self.assertEqual(result.fixed[6:9], bytes((0x24, 8, 0)))
        self.assertEqual(result.fixed[9:], data[9:])

    def test_backup_and_repeat_are_lossless(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path, backups = root / "test.nes", root / "backups"
            path.write_bytes(self.data)
            fixed = review(self.data, self.rules, {2}).fixed
            backup = replace_with_backup(path, self.data, fixed, backups, Path("sub/test.nes"))
            self.assertEqual(backup.read_bytes(), self.data)
            self.assertEqual(path.read_bytes(), fixed)
            path.write_bytes(self.data)
            self.assertEqual(replace_with_backup(path, self.data, fixed, backups, Path("sub/test.nes")), backup)
            path.write_bytes(self.data)
            backup.write_bytes(b"bad existing backup")
            with self.assertRaises(ValueError):
                replace_with_backup(path, self.data, fixed, backups, Path("sub/test.nes"))
            self.assertEqual(path.read_bytes(), self.data)

    def test_cli_preview_apply_and_idempotency(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(tmp)
            roms = root / "roms"
            (roms / "nested").mkdir(parents=True)
            path = roms / "nested/中文.NES"
            path.write_bytes(self.data)
            rules = root / "rules.json"
            rules.write_text(json.dumps({"rules": [self.rule]}))
            args = [str(roms), "--rules", str(rules), "--backup-dir", str(root / "backups")]
            self.assertEqual(main(args), 0)
            self.assertEqual(path.read_bytes(), self.data)
            self.assertFalse((root / "backups").exists())
            self.assertEqual(main(args + ["--fix"]), 0)
            fixed = path.read_bytes()
            self.assertNotEqual(fixed, self.data)
            self.assertEqual(main(args + ["--fix"]), 0)
            self.assertEqual(path.read_bytes(), fixed)
            self.assertEqual(len(list((root / "backups").rglob("*.bak"))), 1)

    def test_firmware_support_and_bundled_rules(self):
        mappers = supported_mappers()
        self.assertTrue({0, 1, 2, 3, 4}.issubset(mappers))
        self.assertNotIn(5, mappers)  # retained implementation is not yet admitted
        rules = load_rules(Path(__file__).resolve().parents[2] / "tools/rom_fixes.json")
        self.assertEqual(rules["9ea1dc76"]["mirroring"], "horizontal")
        self.assertEqual(rules["9ea1dc76"]["mapper"], 2)


if __name__ == "__main__":
    unittest.main()
