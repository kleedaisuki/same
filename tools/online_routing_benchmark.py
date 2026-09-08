"""在线路由四臂配对实验；Four-arm paired online-routing experiment.

仅创建新目录，普通文件与热 OS 缓存，不扫描用户数据。
New directories and ordinary files only; warmed OS cache, never user data.
"""
import argparse
from contextlib import closing
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

MIB = 1024 * 1024
ARMS = ('baseline-auto', 'new-auto', 'new-no-pgo', 'new-cpu')


def arguments():
    """限制资源规模并拒绝覆盖；Bound resources and reject existing output."""
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--exe', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--workload', choices=('small', 'tex-like', 'mixed'), default='small')
    p.add_argument('--files', type=int, default=20000)
    p.add_argument('--trials', type=int, default=7)
    p.add_argument('--warmups', type=int, default=1)
    p.add_argument('--workers', type=int, default=4)
    p.add_argument('--metadata-workers', type=int, default=4)
    p.add_argument('--block-mib', type=int, default=1)
    p.add_argument('--large-mib', type=int, default=64)
    p.add_argument('--large-files', type=int, default=10)
    p.add_argument('--seed', type=int, default=20260908)
    a = p.parse_args()
    for name, low, high in [('files', 1, 300000), ('trials', 1, 100), ('warmups', 0, 10),
                           ('workers', 1, 64), ('metadata_workers', 1, 64),
                           ('block_mib', 1, 64), ('large_mib', 1, 1024), ('large_files', 1, 32)]:
        if not low <= getattr(a, name) <= high:
            p.error(f'{name} must be {low}..{high}')
    a.baseline = a.baseline.resolve(strict=True)
    a.exe = a.exe.resolve(strict=True)
    a.output = a.output.resolve()
    if a.output.exists():
        p.error('output must not exist')
    if a.workload == 'mixed' and a.large_mib * a.large_files > 8192:
        p.error('mixed fixture limited to 8 GiB')
    return a


def save(output, report):
    """原子保存中间结果；Atomically preserve incremental evidence."""
    path = output / 'report.json.tmp'
    path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    path.replace(output / 'report.json')


def fixture(root, a):
    """生成唯一普通文件及 SHA256 真值；Generate unique ordinary files and SHA256 oracle."""
    root.mkdir()
    oracle = {}
    sizes = (128, 1024, 4096, 16384, 65536) if a.workload == 'small' else (128, 1024, 4096)
    for index in range(a.files):
        path = root / f'd{index % 127:03}' / f'f{index:08}.bin'
        path.parent.mkdir(exist_ok=True)
        data = index.to_bytes(8, 'little') + bytes(sizes[index % len(sizes)] - 8)
        path.write_bytes(data)
        oracle[path.relative_to(root).as_posix()] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    count = 0 if a.workload == 'small' else (1 if a.workload == 'tex-like' else a.large_files)
    for index in range(count):
        path = root / f'large-{index:03}.bin'
        chunk = index.to_bytes(8, 'little') + bytes(MIB - 8)
        digest = hashlib.sha256()
        with path.open('xb') as stream:
            for _ in range(a.large_mib):
                stream.write(chunk)
                digest.update(chunk)
        oracle[path.name] = {'bytes': a.large_mib * MIB, 'sha256': digest.hexdigest()}
    return oracle


def config(state, a, arm):
    """显式有界缓冲区预算，不按系统 RAM 扩张；Explicit bounded buffers, not RAM-sized budgets."""
    state.mkdir()
    block = a.block_mib * MIB
    gpu = max(block, 16 * MIB)
    memory = max(64 * MIB, a.workers * (2 * block + block // 32 + 4096) + 2 * gpu + gpu // 32 + 4096)
    text = (f'workers = {a.workers}\nmetadata_workers = {a.metadata_workers}\n'
            f'block_bytes = {block}\nmemory_bytes = {memory}\ndevice_memory_bytes = {64 * MIB}\n'
            f'gpu_min_bytes = {16 * MIB}\nqueue_capacity = {max(16, a.workers * 4)}\n'
            f'backend = "{"cpu" if arm == "new-cpu" else "auto"}"\n')
    (state / 'config.toml').write_text(text, encoding='utf-8')
    return text


def digests(state, oracle):
    """读取全部32字节摘要，不以分组数代替正确性；Check all full digests and paths, not group counts."""
    with closing(sqlite3.connect((state / 'state.db').as_uri() + '?mode=ro', uri=True)) as db:
        rows = db.execute('SELECT path, hex(digest) FROM files').fetchall()
    result = {(path.decode('utf-8') if isinstance(path, bytes) else path): value for path, value in rows}
    if len(rows) != len(oracle) or set(result) != set(oracle) or any(not re.fullmatch('[0-9A-F]{64}', v) for v in result.values()):
        raise RuntimeError('database path/full digest oracle mismatch')
    return result


def run(a, root, oracle, arm, trial, phase, reference, configuration):
    """捕获输出、计时并核验摘要；Capture output, time process and validate full digests."""
    exe = a.baseline if arm == 'baseline-auto' else a.exe
    command = [str(exe), 'scan', '-r', '--summary', '--format=tsv', '--color=never']
    if arm == 'new-no-pgo':
        command.append('--no-pgo')
    if phase == 'fresh':
        command.append('--rehash')
    name = f't{trial}-{arm}-{phase}'
    start = time.perf_counter()
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=1800)
    elapsed = (time.perf_counter() - start) * 1000
    (a.output / f'{name}.stdout').write_bytes(result.stdout)
    (a.output / f'{name}.stderr').write_bytes(result.stderr)
    raw = result.stderr.decode('utf-8', errors='replace')
    metrics = {k: float(v) if '.' in v else int(v) for k, v in re.findall(r'(?<!\S)([\w.]+)=([0-9]+(?:\.[0-9]+)?)(?=\s|$)', raw)}
    actual = digests(root / '.same', oracle) if result.returncode == 0 else {}
    fresh = phase == 'fresh'
    valid = (result.returncode == 0 and not result.stdout.strip() and actual == reference
             and metrics.get('scanned') == len(oracle)
             and metrics.get('hashed') == (len(oracle) if fresh else 0)
             and metrics.get('cached') == (0 if fresh else len(oracle))
             and metrics.get('hash_read_bytes') == (sum(v['bytes'] for v in oracle.values()) if fresh else 0))
    if arm == 'new-no-pgo':
        valid = valid and metrics.get('pgo_enabled') == 0 and metrics.get('pgo_samples') == 0
    record = dict(arm=arm, trial=trial, warmup=trial < 0, phase=phase, process_ms=elapsed,
                  command=command, config=configuration, metrics=metrics, raw_profile=raw,
                  verified=valid, returncode=result.returncode, digests=actual)
    return record


def summarize(runs, seed):
    """配对比值自举仅为探索性界限；Paired bootstrap ratios are exploratory bounds."""
    rng = random.Random(seed)
    summaries = []
    for phase in ('fresh', 'cache'):
        selected = [r for r in runs if not r['warmup'] and r['phase'] == phase]
        values = {(r['trial'], r['arm']): r['process_ms'] for r in selected}
        for control in ('new-no-pgo', 'baseline-auto', 'new-cpu'):
            ratios = [values[t, 'new-auto'] / values[t, control] for t in sorted({r['trial'] for r in selected})]
            if not ratios:
                continue
            draws = sorted(statistics.mean(rng.choices(ratios, k=len(ratios))) for _ in range(10000))
            upper = draws[int(.95 * (len(draws) - 1))]
            summaries.append(dict(phase=phase, treatment='new-auto', control=control,
                                  paired_ratios=ratios, mean_ratio=statistics.mean(ratios),
                                  exploratory_one_sided_95_upper=upper,
                                  two_percent_gate='insufficient_trials' if len(ratios) < 7 else ('pass' if upper <= 1.02 else 'not_demonstrated')))
    return summaries


def main():
    """固定种子随机完整区组，先独立CPU真值；Seeded complete blocks after an independent CPU oracle."""
    a = arguments()
    a.output.mkdir(parents=True, exist_ok=False)
    report = dict(status='generating', parameters={k: str(v) if isinstance(v, Path) else v for k, v in vars(a).items()},
                  environment=dict(platform=platform.platform(), python=platform.python_version(), cpu_count=os.cpu_count()),
                  note='Ordinary synthetic files; fresh DB is NOT cold OS cache. Paired bootstrap is exploratory, not proof of zero overhead.', runs=[])
    save(a.output, report)
    try:
        report['executables'] = []
        for exe in (a.baseline, a.exe):
            version = subprocess.run([str(exe), '--version'], capture_output=True, timeout=30, check=True)
            report['executables'].append(dict(path=str(exe), sha256=hashlib.sha256(exe.read_bytes()).hexdigest(), version=version.stdout.decode(errors='replace')))
        root = a.output / 'dataset'
        oracle = fixture(root, a)
        report['fixture'] = oracle
        state = root / '.same'
        configuration = config(state, a, 'new-cpu')
        command = [str(a.exe), 'scan', '-r', '--rehash', '--format=tsv', '--color=never', '--no-pgo']
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=1800, check=True)
        (a.output / 'oracle.stdout').write_bytes(result.stdout)
        (a.output / 'oracle.stderr').write_bytes(result.stderr)
        if result.stdout.strip():
            raise RuntimeError('unexpected fixture duplicate groups')
        reference = digests(state, oracle)
        report['oracle'] = dict(command=command, config=configuration, digests=reference)
        state.rename(a.output / 'oracle-state')
        report['status'] = 'running'
        rng = random.Random(a.seed)
        for trial in range(-a.warmups, a.trials):
            order = list(ARMS)
            rng.shuffle(order)
            for arm in order:
                configuration = config(state, a, arm)
                for phase in ('fresh', 'cache'):
                    record = run(a, root, oracle, arm, trial, phase, reference, configuration)
                    report['runs'].append(record)
                    save(a.output, report)
                    if not record['verified']:
                        raise RuntimeError(f'failed correctness: {trial} {arm} {phase}')
                    print(json.dumps({k: record[k] for k in ('trial', 'arm', 'phase', 'process_ms', 'verified')}), flush=True)
                state.rename(a.output / f't{trial}-{arm}-state')
        report['comparisons'] = summarize(report['runs'], a.seed)
        report['status'] = 'complete'
        save(a.output, report)
    except Exception as error:
        report.update(status='failed', error=str(error))
        save(a.output, report)
        raise


if __name__ == '__main__':
    main()
