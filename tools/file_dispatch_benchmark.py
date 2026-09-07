"""真实文件分派矩阵，仅生成独立合成数据；Real-file dispatch matrix on isolated synthetic data.

Newly written/warmed files are NOT a cold-disk experiment. No cache flushing occurs.
新写入及预热文件不代表冷盘；不清空系统缓存，不扫描用户目录。
"""
import argparse
from functools import cache
import hashlib
import itertools
import json
import os
from pathlib import Path
import platform
import random
import re
import subprocess
import time

MIB = 1024 * 1024


@cache
def scan_arguments(exe):
    """每个程序只探测一次帮助，兼容旧基线；Probe help once per executable, preserving legacy baselines."""
    result = subprocess.run([str(exe), '--help'], capture_output=True, check=True, timeout=30)
    help_text = result.stdout.decode('utf-8', errors='replace')
    return ('scan', '-r', '--summary') if '--summary' in help_text else ()


def numbers(text):
    """解析正整数列表；Parse a comma-separated positive integer list."""
    values = [int(value) for value in text.split(',')]
    if not values or min(values) < 1:
        raise argparse.ArgumentTypeError('expected positive integers')
    return values


def arguments():
    """小矩阵默认值，完整并发可显式指定；Default to a small, explicitly expandable matrix."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--workers', type=numbers, default=[1, 4])
    parser.add_argument('--block-mib', type=numbers, default=[1, 16])
    parser.add_argument('--backends', default='cpu,auto,cuda')
    parser.add_argument('--trials', type=int, default=3)
    args = parser.parse_args()
    args.backends = args.backends.split(',')
    if max(args.workers) > 256 or max(args.block_mib) > 64 or args.trials < 1:
        parser.error('workers <= 256, block MiB <= 64, trials >= 1 required')
    if any(value not in ('cpu', 'auto', 'cuda') for value in args.backends):
        parser.error('backends must be cpu,auto,cuda')
    args.exe = args.exe.resolve(strict=True)
    return args


def generate(root):
    """以 1 MiB 暂存生成四个不同内容对；Stream four distinct 64 MiB pairs using 1 MiB scratch."""
    rng = random.Random(20260907)
    expected, hashes = [], {}
    for pair in range(4):
        names = [f'pair{pair}-{side}.bin' for side in ('a', 'b')]
        digest = hashlib.sha256()
        with (root / names[0]).open('wb') as first, (root / names[1]).open('wb') as second:
            for _ in range(64):
                data = rng.randbytes(MIB)
                first.write(data)
                second.write(data)
                digest.update(data)
        expected.append(names)
        for name in names:
            hashes[name] = digest.hexdigest()
    if len(set(hashes.values())) != 4:
        raise RuntimeError('fixture pair uniqueness failed')
    return sorted(expected), hashes


def configure(root, backend, workers, block_mib):
    """预算随线程和块大小增长但有界；Bound resource budgets by worker count and block size."""
    block = block_mib * MIB
    memory = max(64 * MIB, workers * (2 * block + block // 32 + 4096))
    device = max(64 * MIB, workers * (2 * block + block // 32 + 4096))
    config = (f'workers = {workers}\nmetadata_workers = 1\nblock_bytes = {block}\n'
              f'memory_bytes = {memory}\ndevice_memory_bytes = {device}\n'
              f'queue_capacity = 32\nbackend = "{backend}"\nrehash = true\n')
    (root / '.same' / 'config.toml').write_text(config, encoding='utf-8')
    return config


def run(exe, root, log, expected):
    """保留原始日志并验证四组八文件；Preserve raw logs and verify four groups of eight total files."""
    command = [str(exe), *scan_arguments(exe), '--format=tsv', '--color=never', '--rehash']
    start = time.perf_counter()
    result = subprocess.run(command,
                            cwd=root, capture_output=True, check=False)
    duration = time.perf_counter() - start
    log.with_suffix('.stdout').write_bytes(result.stdout)
    log.with_suffix('.stderr').write_bytes(result.stderr)
    stdout = result.stdout.decode('utf-8', errors='replace')
    stderr = result.stderr.decode('utf-8', errors='replace')
    groups = {}
    for line in stdout.splitlines():
        group, path = line.split('\t', 1)
        groups.setdefault(group, []).append(json.loads(path))
    actual = sorted(sorted(paths) for paths in groups.values())
    metrics = {key: float(value) for key, value in
               re.findall(r'(\w+)=([0-9]+(?:\.[0-9]+)?)', stderr)}
    valid = (result.returncode == 0 and actual == expected and metrics.get('scanned') == 8
             and metrics.get('hashed') == 8 and metrics.get('cached') == 0)
    record = {'process_seconds': duration, 'returncode': result.returncode,
              'stdout_sha256': hashlib.sha256(result.stdout).hexdigest(),
              'canonical_groups_sha256': hashlib.sha256(json.dumps(actual).encode()).hexdigest(),
              'groups_match_oracle': valid, 'metrics': metrics, 'raw_profile': stderr,
              'stdout_log': str(log.with_suffix('.stdout')), 'stderr_log': str(log.with_suffix('.stderr'))}
    if not valid:
        raise RuntimeError(f'process or result validation failed; inspect {log}')
    return record


def main():
    """先逐配置预热，再交替矩阵方向；Warm each configuration, then alternate complete matrix order."""
    args = arguments()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / 'dataset'
    root.mkdir()
    (root / '.same').mkdir()
    expected, hashes = generate(root)
    cases = list(itertools.product(args.backends, args.workers, args.block_mib))
    report = {'environment': {'platform': platform.platform(), 'python': platform.python_version(),
              'cpu_count': os.cpu_count(), 'root': str(root),
              'cache_note': 'Warm OS cache experiment, NOT cold disk; --rehash disables application digest reuse.'},
              'exe': str(args.exe), 'exe_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(),
              'fixture': {'files': 8, 'bytes_per_file': 64 * MIB, 'sha256': hashes, 'expected_groups': expected},
              'cases': cases, 'trials': args.trials, 'runs': []}
    report_path = output / 'report.json'
    for trial in range(-1, args.trials):
        order = list(enumerate(cases))
        if trial >= 0 and trial % 2:
            order.reverse()
        for index, (backend, workers, block) in order:
            config = configure(root, backend, workers, block)
            phase = 'warmup' if trial < 0 else f'trial{trial + 1}'
            record = run(args.exe, root, output / f'{phase}-case{index}', expected)
            record.update(phase=phase, backend=backend, workers=workers, block_mib=block, config=config)
            report['runs'].append(record)
            report_path.write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(json.dumps({key: record[key] for key in
                  ('phase', 'backend', 'workers', 'block_mib', 'process_seconds', 'groups_match_oracle')}), flush=True)


if __name__ == '__main__':
    main()
