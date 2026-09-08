"""只读合并 same 遥测快照；Merge same telemetry snapshots without writing source data.

Python 3.10+, standard library only. / 仅需 Python 3.10+ 标准库。
Example / 示例: python merge.py --output history.db first.db second.db
"""

import argparse
from contextlib import closing
from itertools import zip_longest
import json
import math
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import time


# 明确列出 v1 数据契约，绝不执行输入数据库提供的 DDL。 / Never execute source DDL.
RUNTIME_ID = 0x53414D54
ARCHIVE_ID = 0x53414D41
COLUMNS = {
    'runs': ('run_id', 'started_unix_ns', 'ended_unix_ns', 'status', 'version',
             'command', 'root', 'config_json', 'error', 'accepted', 'persisted',
             'dropped', 'errors', 'truncated', 'queue_high_water'),
    'events': ('event_id', 'run_id', 'type', 'name', 'severity', 'message',
               'backend', 'worker', 'span_id', 'parent_span_id', 'time_ns',
               'duration_ns', 'bytes', 'value', 'unit', 'truncated'),
    'metrics': ('run_id', 'name', 'value', 'unit'),
    'parameters': ('run_id', 'category', 'name', 'value'),
}
# 每轮按稳定键流式比较，不将整个运行读入内存。 / Stable, streamed per-run comparisons.
ORDER = {'events': 'event_id', 'metrics': 'name', 'parameters': 'category,name'}
BATCH_SIZE = 512


def validate(source):
    """检查身份、版本、表形状及完整性；Validate ownership, shape and integrity."""
    version = source.execute('PRAGMA user_version').fetchone()[0]
    owner = source.execute('PRAGMA application_id').fetchone()[0]
    if version != 1 or owner not in (RUNTIME_ID, ARCHIVE_ID):
        raise ValueError('unsupported telemetry database identity or schema version')
    for table, columns in COLUMNS.items():
        kind = source.execute('SELECT type FROM sqlite_schema WHERE name=?', (table,)).fetchone()
        actual = tuple(row[1] for row in source.execute(f'PRAGMA table_info({table})'))
        if kind != ('table',) or actual != columns:
            raise ValueError(f'unsupported table shape: {table}')
    if source.execute('PRAGMA quick_check').fetchone() != ('ok',):
        raise ValueError('source integrity check failed')
    if source.execute('PRAGMA foreign_key_check').fetchone() is not None:
        raise ValueError('source has broken foreign keys')
    # 即使来源未声明 FK，也不能静默遗漏孤立子记录。 / Check orphans even without source FKs.
    for table in ORDER:
        orphan = source.execute(
            f'SELECT 1 FROM {table} c LEFT JOIN runs r ON c.run_id=r.run_id '
            'WHERE r.run_id IS NULL LIMIT 1').fetchone()
        if orphan:
            raise ValueError(f'orphan records in {table}')
    return owner, version


def rows_for(db, table, run_id):
    """排除局部事件行号，保留跨度 ID；Exclude local row IDs, preserve span IDs."""
    columns = COLUMNS[table][1:] if table == 'events' else COLUMNS[table]
    return db.execute(f'SELECT {",".join(columns)} FROM {table} '
                      f'WHERE run_id=? ORDER BY {ORDER[table]}', (run_id,))


def same_run(source, target, record, existing):
    """重复必须完整一致，不能凭 ID 覆盖；Require exact content, not just matching IDs."""
    if record != existing:
        return False
    missing = object()
    for table in ORDER:
        with closing(rows_for(source, table, record[0])) as left:
            with closing(rows_for(target, table, record[0])) as right:
                if any(a != b for a, b in zip_longest(left, right, fillvalue=missing)):
                    return False
    return True


def copy_run(source, target, record):
    """有界批量复制完整运行，事件 ID 由目标分配；Copy in bounded batches with fresh row IDs."""
    target.execute(f'INSERT INTO runs VALUES({",".join("?" for _ in record)})', record)
    for table in ORDER:
        columns = COLUMNS[table][1:] if table == 'events' else COLUMNS[table]
        query = (f'INSERT INTO {table}({",".join(columns)}) '
                 f'VALUES({",".join("?" for _ in columns)})')
        with closing(rows_for(source, table, record[0])) as cursor:
            while batch := cursor.fetchmany(BATCH_SIZE):
                target.executemany(query, batch)


def import_source(target, path, timeout):
    """同一源全程保持读快照，包含 WAL 已提交内容；Hold one committed WAL-aware snapshot."""
    with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True,
                                 timeout=min(timeout, 5), isolation_level=None)) as source:
        source.execute('PRAGMA query_only=ON')
        source.execute('PRAGMA trusted_schema=OFF')
        deadline = time.monotonic() + timeout
        source.set_progress_handler(lambda: int(time.monotonic() > deadline), 10000)
        source.execute('BEGIN')
        # 首次读取固定快照；不是多个输入之间的全局同一时刻。 / Not a global multi-input snapshot.
        source.execute('SELECT count(*) FROM sqlite_schema').fetchone()
        captured = time.time_ns()
        owner, version = validate(source)
        added = duplicate = running = 0
        source_id = target.execute(
            'INSERT INTO merge_sources(path,application_id,schema_version,snapshot_unix_ns,'
            'runs_added,runs_duplicate,runs_running) VALUES(?,?,?,?,0,0,0)',
            (str(path), owner, version, captured)).lastrowid
        for record in source.execute(f'SELECT {",".join(COLUMNS["runs"])} '
                                     'FROM runs ORDER BY started_unix_ns,run_id'):
            if time.monotonic() > deadline:
                raise TimeoutError(f'source exceeded {timeout:g}s budget')
            existing = target.execute('SELECT * FROM runs WHERE run_id=?', (record[0],)).fetchone()
            if existing is not None:
                if not same_run(source, target, record, existing):
                    raise ValueError(f'conflicting run_id {record[0]!r}; refusing to overwrite')
                duplicate += 1
            else:
                copy_run(source, target, record)
                added += 1
            running += record[3] == 'running'
            target.execute('INSERT INTO run_sources VALUES(?,?)', (record[0], source_id))
        target.execute('UPDATE merge_sources SET runs_added=?,runs_duplicate=?,runs_running=? '
                       'WHERE source_id=?', (added, duplicate, running, source_id))
        return added, duplicate, running


def require_unused_output(output):
    """拒绝主文件及旧日志，避免错误恢复；Reject existing database names and stale journals."""
    for suffix in ('', '-journal', '-wal', '-shm'):
        candidate = Path(str(output) + suffix)
        if os.path.lexists(candidate):
            raise FileExistsError(f'output or sidecar already exists: {candidate}')


def merge(output, inputs, timeout=300.0):
    """先写私有临时库，成功后无覆盖发布；Publish a completed private DB without overwriting."""
    output = Path(output).absolute()
    require_unused_output(output)
    if not inputs:
        raise ValueError('at least one input database is required')
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('timeout must be finite and positive')
    paths = [Path(path).resolve(strict=True) for path in inputs]
    if any(not path.is_file() for path in paths):
        raise ValueError('all inputs must be database files')
    totals = {'sources': len(paths), 'runs_added': 0, 'runs_duplicate': 0, 'running_snapshots': 0}
    with tempfile.TemporaryDirectory(prefix='.same-merge-', dir=output.parent) as folder:
        temporary = Path(folder) / 'archive.db'
        with closing(sqlite3.connect(temporary, isolation_level=None)) as target:
            target.execute('PRAGMA foreign_keys=ON')
            target.execute('PRAGMA synchronous=FULL')
            target.executescript(Path(__file__).with_name('schema.sql').read_text(encoding='utf-8'))
            target.execute('BEGIN IMMEDIATE')
            for path in paths:
                try:
                    counts = import_source(target, path, timeout)
                except (sqlite3.Error, ValueError, OSError) as error:
                    raise ValueError(f'{path}: {error}') from error
                for key, value in zip(('runs_added', 'runs_duplicate', 'running_snapshots'), counts):
                    totals[key] += value
            if target.execute('PRAGMA foreign_key_check').fetchone() is not None:
                raise ValueError('merged database has broken foreign keys')
            target.execute('COMMIT')
            totals['events'] = target.execute('SELECT count(*) FROM events').fetchone()[0]
        # 同文件系统硬链接原子地创建目标，已存在则失败；不使用会覆盖文件的 replace。
        # Same-filesystem hard link atomically creates a name or fails; never replace user data.
        require_unused_output(output)
        os.link(temporary, output)
    return totals


def main():
    """CLI 只操作独立分析归档，不启动 same；Standalone archive CLI, never starts same."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputs', nargs='+', type=Path, help='runtime or merged telemetry databases')
    parser.add_argument('--output', required=True, type=Path, help='new archive; never overwritten')
    parser.add_argument('--timeout', type=float, default=300, help='per-source budget in seconds (300)')
    args = parser.parse_args()
    try:
        result = merge(args.output, args.inputs, args.timeout)
    except (OSError, ValueError, sqlite3.Error) as error:
        print(f'Merge failed: {error}', file=sys.stderr)
        return 2
    print(json.dumps(result))
    if result['running_snapshots']:
        print('Warning: running snapshots are incomplete; wait for scans to finish for final data.',
              file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
