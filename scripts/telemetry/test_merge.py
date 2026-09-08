"""Exercise the standalone SQLite merger through its public CLI.

通过公开 CLI 验证独立 SQLite 合并器；仅使用标准库和隔离临时目录。
Run / 运行: python -m unittest discover -s scripts/telemetry -p test_merge.py
"""

from contextlib import closing
import hashlib
import re
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


MERGER = Path(__file__).with_name("merge.py")
TELEMETRY_ID = 1396788564
ARCHIVE_ID = 1396788545
SCHEMA = """
CREATE TABLE runs(run_id TEXT PRIMARY KEY, started_unix_ns INTEGER NOT NULL,
 ended_unix_ns INTEGER, status TEXT NOT NULL, version TEXT, command TEXT,
 root TEXT, config_json TEXT, error TEXT, accepted INTEGER DEFAULT 0,
 persisted INTEGER DEFAULT 0, dropped INTEGER DEFAULT 0, errors INTEGER DEFAULT 0,
 truncated INTEGER DEFAULT 0, queue_high_water INTEGER DEFAULT 0);
CREATE TABLE events(event_id INTEGER PRIMARY KEY,
 run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE, type TEXT, name TEXT,
 severity TEXT, message TEXT, backend TEXT, worker INTEGER, span_id INTEGER,
 parent_span_id INTEGER, time_ns INTEGER, duration_ns INTEGER, bytes INTEGER,
 value REAL, unit TEXT, truncated INTEGER);
CREATE TABLE metrics(run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE,
 name TEXT NOT NULL, value REAL, unit TEXT, PRIMARY KEY(run_id,name));
CREATE TABLE parameters(run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE,
 category TEXT NOT NULL, name TEXT NOT NULL, value TEXT,
 PRIMARY KEY(run_id,category,name));
CREATE VIEW logs AS SELECT * FROM events WHERE type='log';
CREATE VIEW spans AS SELECT * FROM events WHERE type='span';
CREATE VIEW latest_run AS SELECT * FROM runs ORDER BY rowid DESC LIMIT 1;
PRAGMA user_version=1;
PRAGMA application_id=1396788564;
"""


class MergeTests(unittest.TestCase):
    """Keep each CLI invocation and every generated database isolated.

    每项测试拥有独立目录，CLI 超时受限，绝不操作真实工作区数据库。
    """

    def setUp(self):
        """Create a private fixture root. / 创建独立测试目录。"""
        self.temporary = tempfile.TemporaryDirectory(prefix="same-merge-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "archive.db"

    def source(self, name, run_id=None, event_base=1, status="completed"):
        """Create schema v1 with one fully specified run, or a valid empty journal.

        创建版本 1 模式及完整运行数据；run_id 为空时生成合法空遥测库。
        """
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        with closing(sqlite3.connect(path)) as connection, connection:
            connection.executescript(SCHEMA)
            if run_id is not None:
                connection.execute(
                    "INSERT INTO runs VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                    (run_id, 100, None if status == "running" else 200,
                     status, "0.3.0", "scan", "C:/研究/数据", '{"pgo":1}',
                     None, 2, 2, 0, 0, 0, 2),
                )
                for offset, kind in enumerate(("log", "span")):
                    connection.execute(
                        "INSERT INTO events VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (event_base + offset, run_id, kind, "hash" if offset else "run.start",
                         "info", "可莉/样本.bin", "cpu", 0, 17 if offset else 1,
                         3 if offset else 0, 10 + offset, 5, 4096, 1.0, "bytes", 0),
                    )
                connection.execute("INSERT INTO metrics VALUES(?,?,?,?)",
                                   (run_id, "pgo.samples", 2.0, "count"))
                connection.execute("INSERT INTO parameters VALUES(?,?,?,?)",
                                   (run_id, "model.cpu.0", "cost_ms_per_byte", "0.125"))
        return path

    def merge(self, *sources, output=None, success=True):
        """Run the public CLI with bounded execution and assert its exit contract.

        有界运行公开命令行，并验证成功/失败退出契约。
        """
        destination = output or self.output
        result = subprocess.run(
            [sys.executable, str(MERGER), "--output", str(destination),
             *map(str, sources)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=15,
        )
        if success:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(destination.is_file())
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    @staticmethod
    def rows(path, query):
        """Read query results after CLI completion. / 读取 CLI 完成后的查询结果。"""
        with closing(sqlite3.connect(path)) as connection, connection:
            return connection.execute(query).fetchall()

    @staticmethod
    def digest(path):
        """Compare source bytes without relying on timestamps. / 用字节摘要验证源文件未修改。"""
        return hashlib.sha256(path.read_bytes()).digest()

    def test_distinct_runs_rekey_events_preserve_spans(self):
        """Event IDs are local; run/span identities and all payloads survive merging.

        事件主键局部化重建，运行及跨度关联与载荷完整保留。
        """
        first = self.source("first.db", "run-a")
        second = self.source("second.db", "run-b")
        before = [self.digest(first), self.digest(second)]
        self.merge(first, second)
        self.assertEqual(self.rows(self.output, "PRAGMA application_id"), [(ARCHIVE_ID,)])
        self.assertEqual(self.rows(self.output, "PRAGMA user_version"), [(1,)])
        self.assertEqual(self.rows(self.output, "SELECT count(*),count(DISTINCT event_id) FROM events"), [(4, 4)])
        self.assertEqual(self.rows(self.output, "SELECT run_id,span_id,parent_span_id,message FROM spans ORDER BY run_id"),
                         [("run-a", 17, 3, "可莉/样本.bin"), ("run-b", 17, 3, "可莉/样本.bin")])
        self.assertEqual(self.rows(self.output, "SELECT count(*) FROM logs"), [(2,)])
        self.assertEqual(self.rows(self.output, "SELECT count(*) FROM metrics"), [(2,)])
        self.assertEqual(self.rows(self.output, "SELECT count(*) FROM parameters"), [(2,)])
        self.assertEqual(self.rows(self.output, "SELECT count(*) FROM latest_run"), [(1,)])
        self.assertEqual(self.rows(self.output, "SELECT count(*) FROM merge_sources"), [(2,)])
        self.assertEqual(self.rows(self.output, "PRAGMA foreign_key_check"), [])
        self.assertEqual([self.digest(first), self.digest(second)], before)

    def test_identical_run_deduplicates_ignoring_event_ids(self):
        """Different local event IDs do not make an otherwise identical run conflict.

        原事件编号不同，但完整运行内容相同，应去重而非冲突。
        """
        first = self.source("first.db", "same-run", event_base=1)
        second = self.source("second.db", "same-run", event_base=90)
        self.merge(first, second, first)
        self.assertEqual(self.rows(self.output, "SELECT count(*) FROM runs"), [(1,)])
        self.assertEqual(self.rows(self.output, "SELECT count(*) FROM events"), [(2,)])
        self.assertEqual(self.rows(self.output, "SELECT type FROM events ORDER BY event_id"), [("log",), ("span",)])

    def test_each_payload_conflict_is_atomic(self):
        """A mismatch in any run-owned table rejects the entire archive.

        四张运行所属表任意内容不一致，都必须拒绝整个归档。
        """
        changes = (
            "UPDATE runs SET dropped=1",
            "UPDATE events SET duration_ns=6 WHERE type='span'",
            "UPDATE metrics SET value=3",
            "UPDATE parameters SET value='0.25'",
            "UPDATE events SET event_id=CASE event_id WHEN 1 THEN 100 ELSE 50 END",
        )
        for index, sql in enumerate(changes):
            with self.subTest(table_change=sql):
                first = self.source(f"left-{index}.db", "same-run")
                second = self.source(f"right-{index}.db", "same-run")
                with closing(sqlite3.connect(second)) as connection, connection:
                    connection.execute(sql)
                before = self.digest(second)
                destination = self.root / f"conflict-{index}.db"
                self.merge(first, second, output=destination, success=False)
                self.assertFalse(destination.exists())
                self.assertEqual(self.digest(second), before)

    def test_reject_unknown_schema_business_database_and_empty_sqlite(self):
        """Only recognized telemetry/archive schemas are admissible sources.

        只接受明确归属的遥测或归档模式，不能误导入业务库或无模式库。
        """
        unknown = self.source("unknown.db", "run-a")
        with closing(sqlite3.connect(unknown)) as connection, connection:
            connection.execute("PRAGMA user_version=999")
        business = self.root / "state.db"
        with closing(sqlite3.connect(business)) as connection, connection:
            connection.execute("CREATE TABLE files(path BLOB PRIMARY KEY)")
            connection.execute("PRAGMA user_version=1")
        empty = self.root / "empty-sqlite.db"
        with closing(sqlite3.connect(empty)):
            pass
        for index, source in enumerate((unknown, business, empty)):
            with self.subTest(source=source.name):
                destination = self.root / f"rejected-{index}.db"
                self.merge(source, output=destination, success=False)
                self.assertFalse(destination.exists())

    def test_existing_output_is_never_replaced(self):
        """Existing paths and source/output aliasing must preserve original bytes.

        既有目标及源目标重合均被拒绝，原内容保持不变。
        """
        source = self.source("source.db", "run-a")
        self.output.write_bytes(b"preserve existing output")
        self.merge(source, success=False)
        self.assertEqual(self.output.read_bytes(), b"preserve existing output")
        before = self.digest(source)
        self.merge(source, output=source, success=False)
        self.assertEqual(self.digest(source), before)

    def test_output_sidecars_are_never_reused_or_modified(self):
        """Reject stale SQLite sidecars instead of associating them with a new archive.

        拒绝遗留 SQLite 侧文件，避免新归档误用旧日志，并保持侧文件字节不变。
        """
        source = self.source("source.db", "run-a")
        before = self.digest(source)
        for suffix in ("-journal", "-wal", "-shm"):
            with self.subTest(sidecar=suffix):
                destination = self.root / ("target" + suffix + ".db")
                sidecar = Path(str(destination) + suffix)
                sidecar.write_bytes(b"preserve stale sidecar bytes")
                self.merge(source, output=destination, success=False)
                self.assertFalse(destination.exists())
                self.assertEqual(sidecar.read_bytes(), b"preserve stale sidecar bytes")
                self.assertEqual(self.digest(source), before)

    def test_dangling_output_sidecar_link_is_not_followed(self):
        """A dangling sidecar symlink still occupies its name and must block publication.

        悬空侧文件链接仍占用路径，必须拒绝发布，不能跟随链接或创建其目标。
        """
        source = self.source("source.db", "run-a")
        for suffix in ("-journal", "-wal", "-shm"):
            with self.subTest(sidecar=suffix):
                destination = self.root / ("linked" + suffix + ".db")
                sidecar = Path(str(destination) + suffix)
                target = self.root / ("missing-target" + suffix)
                try:
                    sidecar.symlink_to(target)
                    original_link = sidecar.readlink()
                except (OSError, NotImplementedError) as error:
                    self.skipTest(f"symlinks unavailable: {error}")
                self.merge(source, output=destination, success=False)
                self.assertFalse(destination.exists())
                self.assertTrue(sidecar.is_symlink())
                self.assertEqual(sidecar.readlink(), original_link)
                self.assertFalse(target.exists())

    def test_active_wal_snapshot_preserves_running_status(self):
        """Read committed WAL state without finalizing or mutating the live run.

        从活动 WAL 读取已提交快照，不能替活跃进程修改运行状态。
        """
        source = self.source("active.db", "live-run", status="running")
        connection = sqlite3.connect(source)
        self.addCleanup(connection.close)
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("PRAGMA wal_autocheckpoint=0")
        connection.execute("UPDATE metrics SET value=73")
        connection.commit()
        wal = Path(str(source) + "-wal")
        self.assertTrue(wal.exists())
        before = (self.digest(source), self.digest(wal))
        self.merge(source)
        self.assertEqual(self.rows(self.output, "SELECT status,ended_unix_ns FROM runs"), [("running", None)])
        self.assertEqual(self.rows(self.output, "SELECT value FROM metrics"), [(73.0,)])
        self.assertEqual((self.digest(source), self.digest(wal)), before)
        self.assertEqual(connection.execute("SELECT status FROM runs").fetchone(), ("running",))

    def test_empty_valid_unicode_and_archive_inputs(self):
        """Valid empty journals and Unicode paths work; archives remain composable.

        合法空遥测库及 Unicode 路径可用，已有归档可继续合并。
        """
        empty = self.source("研究/空库.db")
        populated = self.source("日本語/可莉.db", "unicode-run")
        self.merge(empty, populated)
        destination = self.root / "再合并.db"
        self.merge(self.output, populated, output=destination)
        self.assertEqual(self.rows(destination, "SELECT run_id FROM runs"), [("unicode-run",)])
        only_empty = self.root / "empty-archive.db"
        self.merge(empty, output=only_empty)
        self.assertEqual(self.rows(only_empty, "SELECT count(*) FROM runs"), [(0,)])

    def test_runtime_ddl_contract_has_not_drifted(self):
        """Use actual C++ runtime DDL to detect fixture or importer schema drift.

        直接提取 C++ 实际建表语句，避免测试模式与生产模式同时脱节。
        """
        runtime = Path(__file__).resolve().parents[2] / "src" / "telemetry.cpp"
        text = runtime.read_text(encoding="utf-8")
        match = re.search(r'R"SQL\(BEGIN IMMEDIATE;([\s\S]+?)\)SQL"', text)
        self.assertIsNotNone(match, "runtime telemetry DDL not found")
        actual = self.root / "actual-runtime.db"
        fixture = self.source("fixture-schema.db")
        with closing(sqlite3.connect(actual)) as connection, connection:
            connection.executescript("BEGIN IMMEDIATE;" + match.group(1))
            connection.execute(
                "INSERT INTO runs(run_id,started_unix_ns,status) VALUES('runtime',1,'completed')"
            )
        for table in ("runs", "events", "metrics", "parameters"):
            with self.subTest(table=table):
                self.assertEqual(self.rows(actual, f"PRAGMA table_info({table})"),
                                 self.rows(fixture, f"PRAGMA table_info({table})"))
                self.assertEqual(self.rows(actual, f"PRAGMA foreign_key_list({table})"),
                                 self.rows(fixture, f"PRAGMA foreign_key_list({table})"))
        self.merge(actual)
        self.assertEqual(self.rows(self.output, "SELECT run_id FROM runs"), [("runtime",)])

    def test_orphans_rejected_with_or_without_declared_foreign_keys(self):
        """Never silently discard orphan records, even if source FK declarations are absent.

        不论来源是否声明外键，孤立子记录都必须拒绝，不能在合并时静默丢弃。
        """
        for declared in (True, False):
            with self.subTest(declared_foreign_keys=declared):
                source = self.root / f"orphan-{declared}.db"
                schema = SCHEMA if declared else SCHEMA.replace(
                    " REFERENCES runs ON DELETE CASCADE", "")
                with closing(sqlite3.connect(source)) as connection, connection:
                    connection.executescript(schema)
                    connection.execute("INSERT INTO metrics VALUES('missing-run','x',1,'count')")
                before = self.digest(source)
                destination = self.root / f"orphan-archive-{declared}.db"
                self.merge(source, output=destination, success=False)
                self.assertFalse(destination.exists())
                self.assertEqual(self.digest(source), before)

    def test_late_missing_source_leaves_no_output(self):
        """A later source failure must not expose a partially copied archive.

        后续源失败不能留下看似完成的部分归档。
        """
        source = self.source("valid.db", "run-a")
        missing = self.root / "missing.db"
        before = self.digest(source)
        self.merge(source, missing, success=False)
        self.assertFalse(self.output.exists())
        self.assertFalse(missing.exists())
        self.assertEqual(self.digest(source), before)


if __name__ == "__main__":
    unittest.main()
