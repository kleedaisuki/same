"""隔离的小文件遥测开销实验；Isolated small-file telemetry overhead experiment.

仅新建普通文件；fresh database 不等于冷 OS 缓存。CPU后端保持PGO默认不变。
Creates ordinary synthetic files only; fresh database is not cold OS cache. CPU backend,
with default PGO unchanged, isolates telemetry enabled versus --no-telemetry.
每臂顺序为新库fresh、缓存cache、保留库rehash，分离建库与持续写入。
Each arm runs fresh DB, cache, then rehash retaining DB to separate creation from ongoing writes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import sqlite3
import statistics
import subprocess
import time


def arguments():
    """验证有界配置且拒绝已有目录；Validate bounded configuration and reject existing output."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--files', type=int, default=10000)
    parser.add_argument('--trials', type=int, default=7)
    parser.add_argument('--warmups', type=int, default=1)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--seed', type=int, default=20260908)
    args = parser.parse_args()
    for name, low, high in [('files', 1, 100000), ('trials', 1, 100), ('warmups', 0, 10), ('workers', 1, 32)]:
        if not low <= getattr(args, name) <= high:
            parser.error(f'{name} must be {low}..{high}')
    args.exe = args.exe.resolve(strict=True)
    args.output = args.output.resolve()
    if args.output.exists():
        parser.error('output must not exist')
    return args


def save(output, report):
    """保留完整或失败证据；Atomically retain complete or failed evidence."""
    temp = output / 'report.json.tmp'
    temp.write_text(json.dumps(report, indent=2), encoding='utf-8')
    temp.replace(output / 'report.json')


def fixture(root, count):
    """生成唯一小文件并记录输入真值；Generate unique small files and input oracle."""
    root.mkdir()
    expected = {}
    for index in range(count):
        path = root / f'd{index % 127:03}' / f'f{index:08}.bin'
        path.parent.mkdir(exist_ok=True)
        size = (128, 1024, 4096, 16384)[index % 4]
        data = index.to_bytes(8, 'little') + bytes(size - 8)
        path.write_bytes(data)
        expected[path.relative_to(root).as_posix()] = dict(bytes=size, sha256=hashlib.sha256(data).hexdigest())
    return expected


def digests(state, expected):
    """显式关闭SQLite以允许Windows归档；Explicitly close SQLite before Windows archive moves."""
    db = sqlite3.connect((state / 'state.db').as_uri() + '?mode=ro', uri=True)
    try:
        rows = db.execute('SELECT path, hex(digest) FROM files').fetchall()
    finally:
        db.close()
    result = {(p.decode('utf-8') if isinstance(p, bytes) else p): digest for p, digest in rows}
    if len(rows) != len(expected) or set(result) != set(expected) or any(not re.fullmatch('[0-9A-F]{64}', v) for v in result.values()):
        raise RuntimeError('full digest/path oracle mismatch')
    return result


def configure(state, workers):
    """固定CPU与有界预算，保留遥测默认值；Fix CPU and bounded memory, preserving telemetry defaults."""
    state.mkdir()
    value = (f'workers = {workers}\nmetadata_workers = 4\nbackend = "cpu"\n'
             f'block_bytes = 1048576\nmemory_bytes = {max(67108864, workers * 2134016)}\n'
             'device_memory_bytes = 67108864\nqueue_capacity = 64\n')
    (state / 'config.toml').write_text(value, encoding='utf-8')
    return value


def telemetry_snapshot(state):
    """保留数据库跨轮计数，显式关闭连接；Retain cross-run DB counts and explicitly close connection."""
    path = state / 'telemetry.db'
    if not path.exists():
        return {'exists': False}
    db = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)
    try:
        db.row_factory = sqlite3.Row
        runs = [dict(row) for row in db.execute('SELECT run_id,status,accepted,persisted,dropped,errors,truncated,config_json FROM runs')]
        events = {row[0]: row[1] for row in db.execute('SELECT run_id,count(*) FROM events GROUP BY run_id')}
        parameters = [dict(row) for row in db.execute('SELECT run_id,category,name,value FROM parameters')]
        return {'exists': True, 'runs': runs, 'events_per_run': events, 'parameters': parameters}
    finally:
        db.close()


def run(args, root, expected, trial, arm, phase, config, reference):
    """计时包含退出排空，保留分项与损失；Time exit drain too, retaining phase and loss counters."""
    command = [str(args.exe), 'scan', '-r', '--summary', '--format=tsv', '--color=never']
    if arm == 'off':
        command.append('--no-telemetry')
    if phase in ('fresh', 'rehash'):
        command.append('--rehash')
    start = time.perf_counter()
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=1800)
    process_ms = (time.perf_counter() - start) * 1000
    name = f't{trial}-{arm}-{phase}'
    (args.output / f'{name}.stdout').write_bytes(result.stdout)
    (args.output / f'{name}.stderr').write_bytes(result.stderr)
    raw = result.stderr.decode('utf-8', errors='replace')
    metrics = {k: float(v) if '.' in v else int(v) for k, v in re.findall(r'(?<!\S)([\w.]+)=([0-9]+(?:\.[0-9]+)?)(?=\s|$)', raw)}
    actual = digests(root / '.same', expected) if result.returncode == 0 else {}
    fresh = phase in ('fresh', 'rehash')
    verified = (result.returncode == 0 and not result.stdout.strip()
                and (reference is None or reference == actual)
                and metrics.get('scanned') == len(expected)
                and metrics.get('hashed') == (len(expected) if fresh else 0)
                and metrics.get('cached') == (0 if fresh else len(expected))
                and metrics.get('hash_read_bytes') == (sum(v['bytes'] for v in expected.values()) if fresh else 0)
                and metrics.get('gpu_hashes') == 0 and metrics.get('telemetry_enabled') == (arm == 'on'))
    if arm == 'off':
        verified = verified and not (root / '.same' / 'telemetry.db').exists()
    pipeline_ms = metrics.get('scan_work_ms', 0) + metrics.get('hash_wait_ms', 0)
    health = {k: v for k, v in metrics.items() if k.startswith('telemetry_')}
    status = re.search(r'telemetry_status=(\S+)', raw)
    if status:
        health['status'] = status.group(1)
    snapshot = telemetry_snapshot(root / '.same')
    if arm == 'on':
        run_ids = [row['run_id'] for row in snapshot.get('runs', [])]
        parameters = snapshot.get('parameters', [])
        healthy = (snapshot['exists'] and len(run_ids) == {'fresh': 1, 'cache': 2, 'rehash': 3}[phase]
                   and len(set(run_ids)) == len(run_ids)
                   and metrics.get('telemetry_write_errors') == 0
                   and all(row['status'] == 'completed' and row['errors'] == 0
                           and bool(json.loads(row['config_json'])) for row in snapshot.get('runs', [])))
        for run_id in run_ids:
            bands = [p for p in parameters if p['run_id'] == run_id
                     and re.fullmatch(r'worker\.[0-9]+\.model\.(cpu|gpu)\.[0-9]+', p['category'])]
            healthy = healthy and len(bands) >= 256 * args.workers
            for worker in range(args.workers):
                local = [p for p in bands if p['category'].startswith(f'worker.{worker}.model.')]
                healthy = healthy and len(local) >= 256 and len({p['category'] for p in local}) == 64
        verified = verified and healthy
    return dict(telemetry_database=snapshot, trial=trial, warmup=trial < 0, arm=arm, phase=phase, command=command,
                config=config, process_ms=process_ms, pipeline_ms=pipeline_ms,
                elapsed_ms=metrics.get('elapsed_ms'), drain_ms=metrics.get('telemetry_drain_ms', 0),
                total_including_telemetry_ms=metrics.get('total_including_telemetry_ms'),
                metrics=metrics, telemetry_health=health, raw_profile=raw, digests=actual,
                verified=verified, returncode=result.returncode)


def summarize(runs, seed):
    """报告配对差与探索性界限，不将丢失隐藏；Report paired deltas and exploratory bounds, never hide losses."""
    rng = random.Random(seed)
    output = []
    for phase in ('fresh', 'cache', 'rehash'):
        selected = {(r['trial'], r['arm']): r for r in runs if r['trial'] >= 0 and r['phase'] == phase}
        trials = sorted({t for t, _ in selected})
        for metric in ('process_ms', 'pipeline_ms', 'elapsed_ms', 'drain_ms'):
            differences = [selected[t, 'on'][metric] - selected[t, 'off'][metric] for t in trials]
            entry = dict(phase=phase, metric=metric, paired_deltas_ms=differences,
                         mean_delta_ms=statistics.mean(differences), median_delta_ms=statistics.median(differences))
            if metric != 'drain_ms' and all(selected[t, 'off'][metric] > 0 for t in trials):
                ratios = [selected[t, 'on'][metric] / selected[t, 'off'][metric] for t in trials]
                boot = sorted(statistics.mean(rng.choices(ratios, k=len(ratios))) for _ in range(10000))
                entry.update(paired_ratios=ratios, mean_ratio=statistics.mean(ratios), exploratory_one_sided_95_upper=boot[9499])
            output.append(entry)
    return output


def main():
    """先CPU关闭遥测真值，再随机配对；Establish CPU telemetry-off oracle, then randomize pairs."""
    args = arguments()
    args.output.mkdir(parents=True, exist_ok=False)
    report = dict(status='generating', parameters={k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                  environment=dict(platform=platform.platform(), python=platform.python_version(), cpu_count=os.cpu_count()),
                  note='Digest oracle is same CPU implementation with telemetry off, NOT independent BLAKE3 validation. Fresh database, NOT cold OS cache. Captured stdout. Sequential randomized pairs. No zero-overhead claim.', runs=[])
    save(args.output, report)
    try:
        version = subprocess.run([str(args.exe), '--version'], capture_output=True, timeout=30, check=True)
        report['executable'] = dict(sha256=hashlib.sha256(args.exe.read_bytes()).hexdigest(), version=version.stdout.decode(errors='replace'))
        root = args.output / 'dataset'
        expected = fixture(root, args.files)
        report['fixture'] = expected
        state = root / '.same'
        config = configure(state, args.workers)
        oracle = run(args, root, expected, -999, 'off', 'fresh', config, None)
        report['oracle'] = oracle
        if not oracle['verified']:
            raise RuntimeError('CPU oracle failed')
        reference = oracle['digests']
        state.rename(args.output / 'oracle-state')
        report['status'] = 'running'
        rng = random.Random(args.seed)
        for trial in range(-args.warmups, args.trials):
            arms = ['on', 'off']
            rng.shuffle(arms)
            for arm in arms:
                config = configure(state, args.workers)
                for phase in ('fresh', 'cache', 'rehash'):
                    record = run(args, root, expected, trial, arm, phase, config, reference)
                    report['runs'].append(record)
                    save(args.output, report)
                    if not record['verified']:
                        raise RuntimeError(f'correctness failure: {trial}/{arm}/{phase}')
                    print(json.dumps({k: record[k] for k in ('trial', 'arm', 'phase', 'process_ms', 'drain_ms', 'telemetry_health', 'verified')}), flush=True)
                state.rename(args.output / f't{trial}-{arm}-state')
        report['comparisons'] = summarize(report['runs'], args.seed)
        report['telemetry_losses_observed'] = any(r['metrics'].get(k, 0) for r in report['runs'] for k in ('telemetry_dropped', 'telemetry_write_errors', 'telemetry_truncated'))
        report['status'] = 'complete'
        save(args.output, report)
    except Exception as error:
        report.update(status='failed', error=str(error))
        save(args.output, report)
        raise


if __name__ == '__main__':
    main()
