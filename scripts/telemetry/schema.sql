-- 独立分析归档，不可交给运行时写入器。 / Analysis archive, not a runtime writer database.
PRAGMA application_id = 1396788545; -- 0x53414D41, SAMA
PRAGMA user_version = 1;
CREATE TABLE runs(run_id TEXT PRIMARY KEY NOT NULL, started_unix_ns INTEGER NOT NULL,
 ended_unix_ns INTEGER, status TEXT NOT NULL, version TEXT, command TEXT, root TEXT,
 config_json TEXT, error TEXT, accepted INTEGER DEFAULT 0, persisted INTEGER DEFAULT 0,
 dropped INTEGER DEFAULT 0, errors INTEGER DEFAULT 0, truncated INTEGER DEFAULT 0,
 queue_high_water INTEGER DEFAULT 0);
CREATE TABLE events(event_id INTEGER PRIMARY KEY,
 run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE, type TEXT, name TEXT,
 severity TEXT, message TEXT, backend TEXT, worker INTEGER, span_id INTEGER,
 parent_span_id INTEGER, time_ns INTEGER, duration_ns INTEGER, bytes INTEGER,
 value REAL, unit TEXT, truncated INTEGER);
CREATE TABLE metrics(run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE,
 name TEXT NOT NULL, value REAL, unit TEXT, PRIMARY KEY(run_id,name));
CREATE TABLE parameters(run_id TEXT NOT NULL REFERENCES runs ON DELETE CASCADE,
 category TEXT NOT NULL, name TEXT NOT NULL, value TEXT, PRIMARY KEY(run_id,category,name));
-- 记录本次直接输入及运行归属；不会上传路径。 / Record direct inputs and membership locally.
CREATE TABLE merge_sources(source_id INTEGER PRIMARY KEY, path TEXT NOT NULL,
 application_id INTEGER NOT NULL, schema_version INTEGER NOT NULL,
 snapshot_unix_ns INTEGER NOT NULL, runs_added INTEGER NOT NULL,
 runs_duplicate INTEGER NOT NULL, runs_running INTEGER NOT NULL);
CREATE TABLE run_sources(run_id TEXT NOT NULL REFERENCES runs,
 source_id INTEGER NOT NULL REFERENCES merge_sources, PRIMARY KEY(run_id,source_id));
CREATE INDEX events_run_time ON events(run_id,time_ns);
CREATE INDEX events_name ON events(name,run_id);
CREATE INDEX events_span ON events(run_id,span_id);
CREATE INDEX runs_start ON runs(started_unix_ns);
CREATE VIEW logs AS SELECT * FROM events WHERE type='log';
CREATE VIEW spans AS SELECT * FROM events WHERE type='span';
-- 归档没有全局插入时序，按 UTC 排序并以 ID 打破平局。 / Archive latest uses UTC then ID.
CREATE VIEW latest_run AS SELECT * FROM runs ORDER BY started_unix_ns DESC,run_id DESC LIMIT 1;
