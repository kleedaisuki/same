"""Bounded CLI regressions / 有界命令行回归测试 (Python standard library only)."""
from contextlib import closing
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

EXE = str(Path(sys.argv.pop(1)).resolve())


class Integration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="same-integration-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / ".same").mkdir()
        self.config()

    def config(self, **overrides):
        settings = dict(workers=2, block_bytes=1024, memory_bytes=1048576,
                        device_memory_bytes=1048576, queue_capacity=1, backend="cpu")
        settings.update(overrides)
        lines = [f"{key} = {json.dumps(value)}" for key, value in settings.items()]
        (self.root / ".same/config.toml").write_text("\n".join(lines), encoding="utf-8")

    def file(self, name, contents):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
        return path

    def run_same(self, code=0, *args):
        result = subprocess.run([EXE, *args], cwd=self.root, capture_output=True,
                                encoding="utf-8", errors="strict", timeout=30)
        self.assertEqual(result.returncode, code, result.stderr)
        if code:
            self.assertEqual(result.stdout, "")
            return result
        groups = {}
        for line in result.stdout.splitlines():
            group, name = line.split("\t", 1)
            self.assertTrue(group.isdecimal(), line)
            groups.setdefault(int(group), set()).add(json.loads(name))
        stats = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", result.stderr)}
        for key in ("scanned", "hashed", "cached", "groups", "matches", "gpu_workers", "cpu_fallbacks"):
            self.assertIn(key, stats, result.stderr)
        self.assertEqual(stats["groups"], len(groups))
        self.assertEqual(stats["matches"], sum(map(len, groups.values())))
        return {frozenset(group) for group in groups.values()}, stats

    def test_exact_duplicates_boundaries_and_cache(self):
        expected = set()
        for size in (0, 1, 63, 64, 65, 1023, 1024, 1025, 2048, 3073):
            content = bytes((i * 17 + size) % 256 for i in range(size))
            names = {f"a/{size}", f"b/{size}"}
            for name in names:
                self.file(name, content)
            expected.add(frozenset(names))
            if size:
                self.file(f"unique/{size}", bytes([content[0] ^ 1]) + content[1:])
        groups, stats = self.run_same()
        self.assertEqual(groups, expected)
        self.assertEqual(stats["scanned"], 29)
        self.assertEqual(stats["hashed"], 29)
        self.assertEqual(stats["cached"], 0)
        groups, stats = self.run_same()
        self.assertEqual(groups, expected)
        self.assertEqual(stats["hashed"], 0)
        self.assertEqual(stats["cached"], 29)

    def test_restored_mtime_detected(self):
        path = self.file("a", b"abcd")
        self.file("b", b"abcd")
        self.run_same()
        stamp = path.stat()
        time.sleep(0.02)
        path.write_bytes(b"abce")
        os.utime(path, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        groups, stats = self.run_same()
        self.assertFalse(groups)
        self.assertEqual(stats["hashed"], 1)
        self.assertEqual(stats["cached"], 1)

    def test_delete_rename_and_rehash(self):
        self.file("a", b"same")
        self.file("b", b"same")
        self.file("c", b"different")
        self.run_same()
        (self.root / "b").rename(self.root / "renamed")
        (self.root / "c").unlink()
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset({"a", "renamed"})})
        self.assertEqual(stats["scanned"], 2)
        with closing(sqlite3.connect(self.root / ".same/state.db")) as db:
            paths = {bytes(row[0]).decode("utf-8") for row in db.execute("SELECT path FROM files")}
        self.assertEqual(paths, {"a", "renamed"})
        self.config(rehash=True)
        _, stats = self.run_same()
        self.assertEqual(stats["hashed"], 2)
        self.assertEqual(stats["cached"], 0)

    def test_ignores_negations_and_state_exclusion(self):
        for name in ("a", "drop.tmp", "dir/drop", "dir/keep", ".same/hidden"):
            self.file(name, b"same")
        self.file(".same/ignore", b"*.tmp\ndir/\n!dir/keep\n!.same/hidden\n")
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset({"a", "dir/keep"})})
        self.assertEqual(stats["scanned"], 2)

    def test_hardlinks_are_reported(self):
        path = self.file("original", b"linked content")
        try:
            os.link(path, self.root / "alias")
        except OSError as error:
            self.skipTest(str(error))
        self.file("copy", b"linked content")
        groups, _ = self.run_same()
        self.assertEqual(groups, {frozenset({"original", "alias", "copy"})})

    def test_unicode_paths(self):
        names = {"中文/可莉.txt", "日本語/クレー.txt"}
        for name in names:
            self.file(name, b"unicode")
        groups, _ = self.run_same()
        self.assertEqual(groups, {frozenset(names)})

    @unittest.skipIf(os.name == "nt", "Windows excludes control characters in filenames")
    def test_control_paths_are_json_escaped(self):
        names = {'line\nbreak', 'tab\tand"quote', 'back\\slash'}
        for name in names:
            self.file(name, b"controls")
        groups, _ = self.run_same()
        self.assertEqual(groups, {frozenset(names)})

    def symlink(self, source, destination, directory=False):
        try:
            os.symlink(source, destination, target_is_directory=directory)
        except OSError as error:
            self.skipTest(str(error))

    def test_symlinks_ignored(self):
        self.file("a", b"same")
        self.file("b", b"same")
        self.symlink(self.root / "a", self.root / "link")
        self.symlink(self.root, self.root / "loop", True)
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset({"a", "b"})})
        self.assertEqual(stats["scanned"], 2)

    def test_state_database_symlink_refused(self):
        target = self.file("untouched", b"do not modify")
        self.symlink(target, self.root / ".same/state.db")
        self.run_same(2)
        self.assertEqual(target.read_bytes(), b"do not modify")

    def test_state_directory_symlink_refused(self):
        (self.root / ".same/config.toml").unlink()
        (self.root / ".same").rmdir()
        target = self.root / "state-target"
        target.mkdir()
        self.symlink(target, self.root / ".same", True)
        self.run_same(2)
        self.assertEqual(list(target.iterdir()), [])

    def test_invalid_config_does_not_create_database(self):
        for text in ('workers = 0', 'backend = "typo"', 'unknown = 1', 'rehash = "yes"',
                     'block_bytes = 1', 'workers = 256\nmemory_bytes = 1'):
            with self.subTest(config=text):
                self.file(".same/config.toml", text.encode())
                self.run_same(2)
                self.assertFalse((self.root / ".same/state.db").exists())

    def test_invalid_config_does_not_modify_database(self):
        self.file("a", b"a")
        self.run_same()
        db = self.root / ".same/state.db"
        before = db.read_bytes()
        self.file(".same/config.toml", b"workers = 0")
        self.run_same(2)
        self.assertEqual(db.read_bytes(), before)

    def test_collision_bucket_partitioned_by_exact_comparison(self):
        self.config(queue_capacity=4)
        for name, content in (("a1", b"AAAA"), ("a2", b"AAAA"),
                              ("b1", b"BBBB"), ("b2", b"BBBB"), ("c", b"CCCC")):
            self.file(name, content)
        self.run_same()
        with closing(sqlite3.connect(self.root / ".same/state.db")) as db:
            db.execute("UPDATE files SET digest=?", (b"\x42" * 32,))
            db.commit()
        groups, stats = self.run_same()
        self.assertEqual(stats["cached"], 5)
        self.assertEqual(stats["hashed"], 0)
        self.assertEqual(groups, {frozenset({"a1", "a2"}), frozenset({"b1", "b2"})})

    @unittest.skipUnless(os.name == "nt", "Case-insensitive Windows state directory regression")
    def test_uppercase_state_directory_excluded(self):
        (self.root / ".same").rename(self.root / ".state-renaming")
        (self.root / ".state-renaming").rename(self.root / ".SAME")
        self.file("a", b"state exclusion")
        self.file("b", b"state exclusion")
        self.file(".SAME/hidden", b"state exclusion")
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset({"a", "b"})})
        self.assertEqual(stats["scanned"], 2)
        self.assertEqual(stats["cached"], 0)
        _, stats = self.run_same()
        self.assertEqual(stats["cached"], 2)

    def test_many_duplicates_with_single_slot_queue(self):
        names = {f"file-{index:03}" for index in range(50)}
        for name in names:
            self.file(name, b"backpressure" * 300)
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset(names)})
        self.assertEqual(stats["hashed"], 50)
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset(names)})
        self.assertEqual(stats["cached"], 50)

    def test_parallel_buckets_and_collisions(self):
        self.config(workers=4, queue_capacity=5)
        expected = set()
        for group in range(6):
            names = {f"g{group}-{member}" for member in range(7)}
            for name in names:
                self.file(name, bytes([group]) * (1024 + group % 3))
            expected.add(frozenset(names))
        self.assertEqual(self.run_same()[0], expected)
        with closing(sqlite3.connect(self.root / ".same/state.db")) as db:
            db.execute("UPDATE files SET digest=?", (b"\x11" * 32,))
            db.commit()
        groups, stats = self.run_same()
        self.assertEqual(groups, expected)
        self.assertEqual(stats["cached"], 42)

    @unittest.skipUnless(os.environ.get("SAME_REQUIRE_CUDA") == "1", "Opt-in real CUDA device regression")
    def test_cuda_end_to_end_across_worker_threads(self):
        self.config(backend="cuda", workers=3, block_bytes=65536,
                    memory_bytes=1048576, device_memory_bytes=1048576, queue_capacity=2)
        content = (bytes(range(256)) * 4097)[:1048576 + 113]
        self.file("a", content)
        self.file("nested/b", content)
        self.file("c", content)
        self.file("d", content)
        self.file("distinct", content[:-1] + bytes([content[-1] ^ 1]))
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset({"a", "nested/b", "c", "d"})})
        self.assertEqual(stats["gpu_workers"], 3)
        self.assertEqual(stats["cpu_fallbacks"], 0)
        self.assertEqual(stats["hashed"], 5)
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset({"a", "nested/b", "c", "d"})})
        self.assertEqual(stats["hashed"], 0)
        self.assertEqual(stats["cached"], 5)
        self.assertEqual(stats["gpu_workers"], 3)
        self.assertEqual(stats["cpu_fallbacks"], 0)

    def test_auto_with_tiny_device_budget(self):
        self.config(backend="auto", device_memory_bytes=1)
        self.file("a", b"fallback" * 1024)
        self.file("b", b"fallback" * 1024)
        groups, stats = self.run_same()
        self.assertEqual(groups, {frozenset({"a", "b"})})
        self.assertEqual(stats["gpu_workers"], 0)

    @unittest.skipIf(os.name == "nt", "POSIX flock regression; Windows has native lock unit test")
    def test_concurrent_run_is_refused(self):
        import fcntl
        with (self.root / ".same/run.lock").open("wb") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.run_same(2)
        self.run_same()


if __name__ == "__main__":
    unittest.main()
