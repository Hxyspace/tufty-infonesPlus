"""Dependency lifecycle checks using local Git fixtures, without network access."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from deps import DependencyError, ensure_checkout, load_lock, patch_checkout


def run(root, *args):
    env = dict(os.environ, GIT_AUTHOR_NAME="Fixture", GIT_AUTHOR_EMAIL="fixture@example.invalid",
               GIT_COMMITTER_NAME="Fixture", GIT_COMMITTER_EMAIL="fixture@example.invalid")
    return subprocess.check_output(["git", "-C", str(root), *args], env=env, stderr=subprocess.PIPE)


class DependencyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.origin = self.root / "origin"
        self.origin.mkdir()
        run(self.origin, "init", "-q")
        (self.origin / "source.txt").write_text("one\ntwo\nthree\n")
        (self.origin / "other.txt").write_text("original\n")
        run(self.origin, "add", ".")
        run(self.origin, "commit", "-qm", "fixture")
        self.commit = run(self.origin, "rev-parse", "HEAD").decode().strip()
        self.spec = {"url": str(self.origin), "commit": self.commit}
        self.checkout = self.root / "lib/example"
        with contextlib.redirect_stdout(io.StringIO()):
            ensure_checkout(self.checkout, self.spec)
        (self.origin / "source.txt").write_text("one\nTWO\nthree\n")
        self.patch = self.root / "change.patch"
        self.patch.write_bytes(run(self.origin, "diff"))

    def apply(self, check_only=False):
        with contextlib.redirect_stdout(io.StringIO()):
            patch_checkout(self.checkout, [self.patch], check_only)

    def test_fetch_is_detached_pinned_and_repeatable(self):
        self.assertEqual(run(self.checkout, "rev-parse", "HEAD").decode().strip(), self.commit)
        ensure_checkout(self.checkout, self.spec, check_only=True)
        ensure_checkout(self.checkout, self.spec)
        self.assertEqual(run(self.checkout, "branch", "--show-current"), b"")

    def test_patch_applied_once_without_changing_index(self):
        self.apply()
        self.apply()
        self.apply(check_only=True)
        self.assertEqual((self.checkout / "source.txt").read_text(), "one\nTWO\nthree\n")
        self.assertEqual(run(self.checkout, "diff", "--cached"), b"")
        self.assertEqual(run(self.checkout, "rev-parse", "HEAD").decode().strip(), self.commit)

    def test_check_does_not_apply(self):
        with self.assertRaises(DependencyError):
            self.apply(check_only=True)
        self.assertEqual((self.checkout / "source.txt").read_text(), "one\ntwo\nthree\n")

    def test_unrelated_change_preserved(self):
        path = self.checkout / "other.txt"
        path.write_text("my changes\n")
        with self.assertRaises(DependencyError):
            self.apply()
        self.assertEqual(path.read_text(), "my changes\n")
        self.assertEqual((self.checkout / "source.txt").read_text(), "one\ntwo\nthree\n")

    def test_changes_outside_patch_hunk_are_not_silently_accepted(self):
        self.apply()
        path = self.checkout / "source.txt"
        path.write_text(path.read_text() + "my extra line\n")
        with self.assertRaises(DependencyError):
            self.apply()
        self.assertTrue(path.read_text().endswith("my extra line\n"))

    def test_wrong_revision_preserved(self):
        wrong = dict(self.spec, commit="0" * 40)
        with self.assertRaises(DependencyError):
            ensure_checkout(self.checkout, wrong)
        self.assertEqual(run(self.checkout, "rev-parse", "HEAD").decode().strip(), self.commit)

    def test_staged_and_untracked_changes_preserved(self):
        path = self.checkout / "other.txt"
        path.write_text("staged\n")
        run(self.checkout, "add", "other.txt")
        with self.assertRaises(DependencyError):
            self.apply()
        self.assertEqual(run(self.checkout, "show", ":other.txt"), b"staged\n")
        fresh = self.root / "fresh"
        with contextlib.redirect_stdout(io.StringIO()):
            ensure_checkout(fresh, self.spec)
        (fresh / "my_file").write_text("keep")
        with self.assertRaises(DependencyError):
            patch_checkout(fresh, [])
        self.assertEqual((fresh / "my_file").read_text(), "keep")

    def test_missing_check_never_downloads(self):
        path = self.root / "missing"
        with self.assertRaises(DependencyError):
            ensure_checkout(path, self.spec, check_only=True)
        self.assertFalse(path.exists())

    def test_lock_requires_full_commit_sha(self):
        lock = self.root / "lock.json"
        lock.write_text(json.dumps({"version": 1, "dependencies": {"test": dict(self.spec, commit="main")}}))
        with self.assertRaises(DependencyError):
            load_lock(lock)

    def test_lf_patch_on_crlf_dependency_still_requires_exact_tree(self):
        run(self.origin, "checkout", "--", "source.txt")
        (self.origin / "source.txt").write_bytes(b"one\r\ntwo\r\nthree\r\n")
        run(self.origin, "add", "source.txt")
        run(self.origin, "commit", "-qm", "CRLF upstream")
        spec = dict(self.spec, commit=run(self.origin, "rev-parse", "HEAD").decode().strip())
        checkout = self.root / "crlf"
        ensure_checkout(checkout, spec)
        self.patch.write_text("--- a/source.txt\n+++ b/source.txt\n@@ -1,3 +1,3 @@\n one\n-two\n+TWO\n three\n")
        patch_checkout(checkout, [self.patch])
        patch_checkout(checkout, [self.patch], check_only=True)
        self.assertEqual((checkout / "source.txt").read_text(), "one\nTWO\nthree\n")
        with (checkout / "source.txt").open("ab") as stream:
            stream.write(b" ")
        with self.assertRaises(DependencyError):
            patch_checkout(checkout, [self.patch])


if __name__ == "__main__":
    unittest.main()
