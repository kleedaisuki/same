"""统一worker前后对照；Unified-worker before/after benchmark.

新目录普通文件、固定配置、串行随机区组；不预设GPU并发或性能收益。
New ordinary files, fixed configuration, sequential randomized blocks; no assumed speedup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import re
import sqlite3
import statistics
import subprocess
import time

MIB = 1048576


def arguments():
    """限制输入规模，不覆盖既有证据；Bound fixture size without overwriting evidence."""
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, default=Path('.cache/same-pre-unified.exe'))
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--workers', type=int, default=4)
    p.add_argument('--baseline-workers', type=int, help='baseline configured workers; defaults to --workers')
    p.add_argument('--trials', type=int, default=7)
    p.add_argument('--warmups', type=int, default=1)
    p.add_argument('--small-files', type=int, default=10000)
    p.add_argument('--large-files', type=int, default=16)
    p.add_argument('--large-mib', type=int, default=64)
    p.add_argument('--backends', default='auto,no-pgo,cpu')
    p.add_argument('--seed', type=int, default=20260909)
    p.add_argument('--telemetry-check', action='store_true', help='one additional untimed new-auto run preserving telemetry parameters')
    a = p.parse_args()
    if a.baseline_workers is None:
        a.baseline_workers = a.workers
    for key, low, high in [('workers', 1, 64), ('baseline_workers', 1, 64), ('trials', 1, 30), ('warmups', 0, 3), ('small_files', 0, 100000), ('large_files', 0, 64), ('large_mib', 1, 512)]:
        if not low <= getattr(a, key) <= high:
            p.error(f'{key} must be {low}..{high}')
    if a.small_files + a.large_files == 0 or a.large_files * a.large_mib > 8192:
        p.error('fixture must be nonempty and large files <=8GiB')
    a.backends = a.backends.split(',')
    if len(set(a.backends)) != len(a.backends) or not set(a.backends) <= {'auto', 'no-pgo', 'cpu'}:
        p.error('backends must be unique auto,no-pgo,cpu choices')
    a.exe = a.exe.resolve(strict=True)
    a.baseline = a.baseline.resolve(strict=True)
    a.output = a.output.resolve()
    if a.output.exists():
        p.error('output must not exist')
    return a


def save(a, report):
    """原子保存失败或完整报告；Atomically preserve failed or completed report."""
    temp = a.output / 'report.json.tmp'
    temp.write_text(json.dumps(report, indent=2), encoding='utf-8')
    temp.replace(a.output / 'report.json')


def fixture(root, a):
    """写唯一普通文件并保存输入SHA256；Write unique ordinary files with input SHA256."""
    root.mkdir()
    expected = {}
    for index in range(a.small_files):
        path = root / f'd{index % 127:03}' / f'small-{index:08}.bin'
        path.parent.mkdir(exist_ok=True)
        data = index.to_bytes(8, 'little') + bytes(4088)
        path.write_bytes(data)
        expected[path.relative_to(root).as_posix()] = dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
    for index in range(a.large_files):
        path = root / f'large-{index:03}.bin'
        data = index.to_bytes(8, 'little') + bytes(MIB - 8)
        digest = hashlib.sha256()
        with path.open('xb') as f:
            for _ in range(a.large_mib):
                f.write(data)
                digest.update(data)
        expected[path.name] = dict(bytes=a.large_mib * MIB, sha256=digest.hexdigest())
    return expected


def read_db(path, sql):
    """显式关闭Windows数据库句柄；Explicitly close Windows database handles."""
    db = sqlite3.connect(path.as_uri() + '?mode=ro', uri=True)
    try:
        db.row_factory = sqlite3.Row
        return [dict(row) for row in db.execute(sql)]
    finally:
        db.close()


def run(a, root, expected, version, backend, trial, reference, telemetry=False):
    """隔离应用状态，保存真实度量及完整摘要；Isolate state and retain actual metrics/full digests."""
    state = root / '.same'
    state.mkdir()
    configured_workers = a.workers if version == 'new' else a.baseline_workers
    config = (f'workers = {configured_workers}\nmetadata_workers = 4\nblock_bytes = 1048576\n'
              'memory_bytes = 1073741824\ndevice_memory_bytes = 1073741824\nqueue_capacity = 64\n'
              f'backend = "{"cpu" if backend == "cpu" else "auto"}"\n')
    (state / 'config.toml').write_text(config, encoding='utf-8')
    exe = a.exe if version == 'new' else a.baseline
    command = [str(exe), 'scan', '-r', '--rehash', '--summary', '--format=tsv', '--color=never']
    if not telemetry:
        command.append('--no-telemetry')
    if backend == 'no-pgo':
        command.append('--no-pgo')
    name = f't{trial}-{version}-{backend}' + ('-telemetry' if telemetry else '')
    start = time.perf_counter()
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=1800)
    elapsed = (time.perf_counter() - start) * 1000
    (a.output / f'{name}.stdout').write_bytes(result.stdout)
    (a.output / f'{name}.stderr').write_bytes(result.stderr)
    raw = result.stderr.decode('utf-8', errors='replace')
    metrics = {k: float(v) if '.' in v else int(v) for k, v in re.findall(r'(?<!\S)([\w.]+)=([0-9]+(?:\.[0-9]+)?)(?=\s|$)', raw)}
    rows = read_db(state / 'state.db', 'SELECT path,hex(digest) AS digest FROM files') if result.returncode == 0 else []
    digests = {(r['path'].decode('utf-8') if isinstance(r['path'], bytes) else r['path']): r['digest'] for r in rows}
    valid = (result.returncode == 0 and not result.stdout.strip() and len(rows) == len(expected)
             and set(digests) == set(expected) and all(re.fullmatch('[0-9A-F]{64}', d) for d in digests.values())
             and (reference is None or digests == reference)
             and metrics.get('scanned') == len(expected) and metrics.get('hashed') == len(expected)
             and metrics.get('cached') == 0 and metrics.get('hash_read_bytes') == sum(r['bytes'] for r in expected.values())
             and metrics.get('cpu_hashes', 0) + metrics.get('gpu_hashes', 0) == len(expected))
    if backend == 'no-pgo':
        valid = valid and metrics.get('pgo_samples') == 0
    if not telemetry:
        valid = valid and not (state / 'telemetry.db').exists()
    snapshot = None
    if telemetry:
        snapshot = dict(runs=read_db(state / 'telemetry.db', 'SELECT * FROM runs'),
                        parameters=read_db(state / 'telemetry.db', 'SELECT * FROM parameters'))
        parameters = snapshot['parameters']
        valid = valid and len(snapshot['runs']) == 1 and snapshot['runs'][0]['status'] == 'completed'
        for worker in range(a.workers):
            local = [p for p in parameters if re.fullmatch(rf'worker\.{worker}\.model\.(cpu|gpu)\.[0-9]+', p['category'])]
            valid = valid and len(local) >= 256 and len({p['category'] for p in local}) == 64
    if version == 'new':
        valid = valid and metrics.get('worker_count') == a.workers
        valid = valid and sum(metrics.get(f'worker.{w}.cpu_hashes', 0) + metrics.get(f'worker.{w}.gpu_hashes', 0) for w in range(a.workers)) == len(expected)
    peak = max((value for key, value in metrics.items() if key.endswith('gpu_peak_concurrency')), default=0)
    state.rename(a.output / f'{name}-state')
    return dict(configured_workers=configured_workers, version=version, backend=backend, trial=trial, warmup=trial < 0,
                gpu_peak_concurrency=peak, multi_gpu_concurrency_observed=peak > 1, telemetry=telemetry, process_ms=elapsed, metrics=metrics, raw_profile=raw,
                config=config, command=command, digests=digests, verified=valid, telemetry_database=snapshot)


def main():
    """CPU对照后固定种子随机区组；Seeded randomized blocks after CPU equivalence oracle."""
    a = arguments()
    a.output.mkdir(parents=True, exist_ok=False)
    report = dict(status='generating', parameters={k: str(v) if isinstance(v, Path) else v for k, v in vars(a).items()},
                  environment=platform.platform(), note='Fresh state, warm OS cache, ordinary files. CPU digest equivalence is not independent BLAKE3 validation. Negative performance results retained.', runs=[])
    save(a, report)
    try:
        report['executables'] = []
        for exe in (a.baseline, a.exe):
            version = subprocess.run([str(exe), '--version'], check=True, capture_output=True, timeout=30)
            report['executables'].append(dict(path=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), version=version.stdout.decode(errors='replace')))
        root = a.output / 'dataset'
        expected = fixture(root, a)
        report['fixture'] = expected
        oracle = run(a, root, expected, 'baseline', 'cpu', -999, None)
        report['oracle'] = oracle
        if not oracle['verified']:
            raise RuntimeError('CPU oracle failed')
        reference = oracle['digests']
        rng = random.Random(a.seed)
        report['status'] = 'running'
        for trial in range(-a.warmups, a.trials):
            order = [(v, b) for v in ('baseline', 'new') for b in a.backends]
            rng.shuffle(order)
            for version, backend in order:
                record = run(a, root, expected, version, backend, trial, reference)
                report['runs'].append(record)
                save(a, report)
                if not record['verified']:
                    raise RuntimeError(f'correctness failed: {trial}/{version}/{backend}')
                print(json.dumps({k: record[k] for k in ('version', 'backend', 'trial', 'process_ms', 'metrics')}), flush=True)
        report['comparisons'] = []
        for backend in a.backends:
            data = {(r['trial'], r['version']): r['process_ms'] for r in report['runs'] if r['trial'] >= 0 and r['backend'] == backend}
            ratios = [data[t, 'new'] / data[t, 'baseline'] for t in range(a.trials)]
            report['comparisons'].append(dict(backend=backend, paired_ratios=ratios, mean_ratio=statistics.mean(ratios), median_ratio=statistics.median(ratios)))
        if a.telemetry_check:
            record = run(a, root, expected, 'new', 'auto', -998, reference, telemetry=True)
            report['telemetry_check'] = record
            if not record['verified']:
                raise RuntimeError('telemetry check failed')
        report['status'] = 'complete'
        save(a, report)
    except Exception as error:
        report.update(status='failed', error=str(error))
        save(a, report)
        raise


if __name__ == '__main__':
    main()
