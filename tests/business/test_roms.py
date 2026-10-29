import contextlib
import csv
import hashlib
from http.client import HTTPException
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from prepare_roms import DATABASE_URL, load_database, load_rules, main, replace_with_backup, review, supported_mappers


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
            args = [str(roms), "--no-database", "--rules", str(rules), "--backup-dir", str(backups)]
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
            args = [str(roms), "--no-database", "--rules", str(rules), "--backup-dir", str(root / "backups")]
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

    def database_row(self, **changes):
        row = [self.crc, "Famicom", "NAMCOT-175", "", "175", "210", "32", "8",
               "0", "0", "2", "1", "v", "1", "", "1", "", ""]
        for index, value in changes.items():
            row[int(index)] = value
        return row

    def database(self, **changes):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "MesenNesDB.txt"
            with path.open("w", encoding="utf-8-sig", newline="") as out:
                out.write("# Fields: Mesen database\n\n")
                csv.writer(out).writerow(self.database_row(**changes))
            return load_database(path)

    def test_database_repairs_board_before_firmware_preflight(self):
        db = self.database()
        for original in (self.data, self.data[:7] + b"DiskDude!" + self.data[16:],
                         self.data[:7] + b"@iskDude!" + self.data[16:]):
            original += b"unrecognized tail\x00\xff"
            result = review(original, {}, {210}, database=db)
            self.assertFalse(result.error, result.notes)
            self.assertEqual(result.fixed[:6], original[:6])
            self.assertEqual(result.fixed[6:16], bytes((0x27, 0xd8, 0x10, 0, 0x50, 0, 0, 0, 0, 1)))
            self.assertEqual(result.fixed[16:], original[16:])
            self.assertEqual(review(result.fixed, {}, {210}, database=db).fixed, result.fixed)
        original = bytearray(self.data)
        original[7] |= 8
        original[8] = 0xf1  # Wrong extended mapper/submapper must not block correction.
        result = review(bytes(original), {}, {210}, database=db)
        self.assertFalse(result.error, result.notes)
        self.assertEqual(result.fixed[6:9], bytes((0x27, 0xd8, 0x10)))

    def test_database_region_mirroring_ram_and_extended_mapper(self):
        for system, timing in (("NesNtsc", 0), ("Famicom", 0), ("NesPal", 1), ("Dendy", 3)):
            for mirror, bits in (("h", 0), ("v", 1), ("4", 8)):
                db = self.database(**{"1": system, "5": "268", "10": "0", "11": "0",
                                      "9": "2", "12": mirror, "15": "0"})
                result = review(self.data, {}, {268}, database=db)
                self.assertFalse(result.error, result.notes)
                self.assertEqual(result.fixed[6:9], bytes((0xc4 | bits, 8, 1)))
                self.assertEqual(result.fixed[10:13], bytes((5, 0, timing)))
                self.assertEqual(result.fixed[16:], self.data[16:])

    def test_database_never_guesses_incomplete_or_conflicting_entries(self):
        variants = ({"6": "16", "7": "24"}, {"7": ""}, {"1": "VsSystem"},
                    {"1": "VT03"}, {"8": ""}, {"9": "9"}, {"10": "0"},
                    {"12": "1"}, {"12": ""}, {"13": "2"}, {"13": ""},
                    {"14": "N"}, {"15": ""}, {"15": "16"}, {"5": "65000"},
                    {"16": "1"}, {"17": "2"}, {"9": "16"})
        dirty = self.data[:7] + b"DiskDude!" + self.data[16:]
        for changes in variants:
            with self.subTest(changes=changes):
                for data in (self.data, dirty):
                    result = review(data, {}, {210}, database=self.database(**changes))
                    self.assertTrue(result.error, result.notes)
                    self.assertEqual(result.fixed, data)
        db = self.database()
        self.assertEqual(review(self.data, self.rules, {210}, database=db).fixed, self.data)
        self.assertTrue(review(self.data, self.rules, {210}, database=db).error)
        self.rule.update(mapper=210, mirroring="vertical")
        self.assertFalse(review(self.data, self.rules, {210}, database=db).error)
        self.rule["sha256"] = "0" * 64
        self.assertTrue(review(self.data, self.rules, {210}, database=db).error)
        # A database match does not grant firmware support.
        result = review(self.data, {}, {0}, database=db)
        self.assertTrue(result.error)
        self.assertEqual(result.fixed, self.data)

    def test_database_unknown_crc_and_unsafe_payload_boundaries(self):
        db = self.database(**{"0": "00000000"})
        result = review(self.data, {}, {67}, database=db)
        self.assertFalse(result.error)
        self.assertEqual(result.fixed, self.data)
        self.assertTrue(any("no match" in note for note in result.notes))
        self.assertNotEqual(review(self.data, self.rules, {2}, database=db).fixed, self.data)
        for size in (0, 10, len(self.data)-1):
            result = review(self.data[:size], {}, {210}, database=self.database())
            self.assertTrue(result.error)
            self.assertEqual(result.fixed, self.data[:size])
        for offset, value in ((7, 1), (9, 1), (13, 1), (14, 1), (15, 2)):
            data = bytearray(self.data);data[7] |= 8;data[offset] = value
            result = review(bytes(data), {}, {210}, database=self.database())
            self.assertTrue(result.error)
            self.assertEqual(result.fixed, data)

    def test_database_invalid_file_and_duplicate_crc_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "db.txt"
            row = self.database_row()
            for rows in ([], [row, row], [row[:-1]], [self.database_row(**{"0": "123"})],
                         [self.database_row(**{"9": "-1"})]):
                with path.open("w", newline="") as out:
                    csv.writer(out).writerows(rows)
                with self.assertRaises(ValueError):
                    load_database(path)

    def test_database_does_not_apply_known_unsupported_geometry(self):
        for mapper, prg_kib, chr_kib in ((0, 64, 8), (0, 32, 16), (268, 512, 0),
                                          (210, 1024, 128), (210, 32, 512), (210, 48, 8)):
            data = bytearray(self.data[:16]);data[4]=prg_kib//16;data[5]=chr_kib//8
            data[6] &= ~4
            payload = bytes((prg_kib+chr_kib)*1024)
            crc = f"{zlib.crc32(payload):08x}"
            db = self.database(**{"0": crc, "5": str(mapper), "6": str(prg_kib), "7": str(chr_kib), "15": "0"})
            original=bytes(data)+payload
            result=review(original, {}, {mapper}, database=db)
            self.assertTrue(result.error, result.notes)
            self.assertEqual(result.fixed, original)

    def test_diskdude_without_database_does_not_create_invalid_nrom(self):
        header = bytearray(b"NES\x1a" + bytes(12))
        header[4], header[5] = 2, 4
        header[7:16] = b"DiskDude!"
        original = bytes(header) + bytes(32768 + 32768)
        for db in (None, {}):
            result = review(original, {}, supported_mappers(), database=db)
            self.assertTrue(result.error)
            self.assertEqual(result.fixed, original)
            self.assertTrue(any("NROM geometry" in note for note in result.notes))

    def test_database_cli_cache_preview_fix_backup_and_disable(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(tmp);roms = root / "roms";roms.mkdir()
            path = roms / "wrong.NES";path.write_bytes(self.data)
            db = root / "db.txt"
            with db.open("w", newline="") as out:
                csv.writer(out).writerow(self.database_row())
            rules = root / "rules.json";rules.write_text('{"rules": []}')
            args = [str(roms), "--rules", str(rules), "--backup-dir", str(root / "backups")]
            with patch("prepare_roms.DEFAULT_DATABASE", db):
                self.assertEqual(main(args), 0)
                self.assertEqual(path.read_bytes(), self.data)
                self.assertFalse((root / "backups").exists())
                self.assertEqual(main(args + ["--no-database"]), 1)  # Mapper 67 unsupported.
                self.assertEqual(main(args + ["--fix", "--database", str(db)]), 0)
                fixed = path.read_bytes()
                self.assertEqual(fixed[16:], self.data[16:])
                self.assertEqual(main(args + ["--fix"]), 0)
                backups = list((root / "backups").rglob("*.bak"))
                self.assertEqual(len(backups), 1)
                self.assertEqual(backups[0].read_bytes(), self.data)
                self.assertEqual(path.read_bytes(), fixed)
                broken = root / "broken-db.txt";broken.write_text("not a database")
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    main(args + ["--fix", "--database", str(broken)])
                self.assertEqual(path.read_bytes(), fixed)

    def test_missing_database_downloads_once_then_uses_cache(self):
        payload = (",".join(self.database_row()) + "\n").encode()
        for explicit in (False, True):
            with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
                root = Path(tmp);roms = root / "roms";roms.mkdir()
                cache = root / "cache/MesenNesDB.txt"
                args = [str(roms)] + (["--database", str(cache)] if explicit else [])
                with patch("prepare_roms.DEFAULT_DATABASE", cache), patch("prepare_roms.urlopen", return_value=io.BytesIO(payload)) as fetch:
                    self.assertEqual(main(args), 0)
                    self.assertEqual(cache.read_bytes(), payload)
                    self.assertEqual(main(args), 0)
                    fetch.assert_called_once_with(DATABASE_URL, timeout=10)
                    self.assertEqual(list(cache.parent.iterdir()), [cache])

    def test_database_download_failure_falls_back_to_crc_rules(self):
        failures = (OSError("offline"), HTTPException("truncated response"), b"<html>error</html>",
                    b"x" * (4 * 1024 * 1024 + 1))
        for failure in failures:
            with self.subTest(failure=type(failure).__name__), tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
                root = Path(tmp);roms = root / "roms";roms.mkdir()
                path = roms / "game.NES";path.write_bytes(self.data)
                rules = root / "rules.json";rules.write_text(json.dumps({"rules": [self.rule]}))
                cache = root / "cache/MesenNesDB.txt"
                args = [str(roms), "--rules", str(rules)]
                options = {"side_effect": failure} if isinstance(failure, Exception) else {"return_value": io.BytesIO(failure)}
                with patch("prepare_roms.DEFAULT_DATABASE", cache), patch("prepare_roms.urlopen", **options):
                    self.assertEqual(main(args), 0)  # CRC rule repairs mapper 67 to 2.
                self.assertEqual(path.read_bytes(), self.data)  # Still preview only.
                self.assertFalse(cache.exists())
                self.assertFalse(list(root.rglob(".mesen-db-*")))

    def test_database_disabled_or_existing_invalid_cache_never_downloads(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()):
            root = Path(tmp);cache = root / "db.txt"
            with patch("prepare_roms.DEFAULT_DATABASE", cache), patch("prepare_roms.urlopen") as fetch:
                self.assertEqual(main([str(root), "--no-database"]), 0)
                cache.write_text("invalid cached database")
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                    main([str(root)])
                self.assertEqual(cache.read_text(), "invalid cached database")
                fetch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
