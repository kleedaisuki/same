"""可复现的小文件扫描实验；Reproducible small-file scan experiments.

Only generates synthetic data in a new output directory; never scans user data.
仅在新输出目录生成合成数据，绝不扫描用户数据。
"""

import argparse
from functools import cache
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import subprocess
import time


@cache
def scan_arguments(exe):
    """每个程序只探测一次帮助，兼容旧基线；Probe help once per executable, preserving legacy baselines."""
    result = subprocess.run([str(exe), '--help'], capture_output=True, check=True, timeout=30)
    help_text = result.stdout.decode('utf-8', errors='replace')
    return ('scan', '-r', '--summary') if '--summary' in help_text else ()


def arguments():
    """读取实验参数；Parse reproducible experiment parameters."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', action='append', required=True, type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--files', type=int, default=2000)
    parser.add_argument('--sizes', default='0,128,1024,4096,16384,65536')
    parser.add_argument('--duplicate-every', type=int, default=10)
    parser.add_argument('--layout', choices=['flat', 'wide', 'deep', 'mixed'], default='mixed')
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--seed', type=int, default=20260907)
    parser.add_argument('--workers', type=int, default=8)
    parser.add_argument('--metadata-workers', type=int)
    parser.add_argument('--queue-capacity', type=int, default=512)
    parser.add_argument('--backend', choices=['cpu', 'auto', 'cuda'], default='cpu')
    args = parser.parse_args()
    args.sizes = [int(value) for value in args.sizes.split(',')]
    if args.files < 1 or args.trials < 1 or not args.sizes or min(args.sizes) < 0:
        parser.error('files/trials must be positive and sizes nonnegative')
    if args.duplicate_every < 0 or not 1 <= args.workers <= 256 or not 1 <= args.queue_capacity <= 65536:
        parser.error('invalid duplicate interval, worker count, or queue capacity')
    if args.metadata_workers is not None and not 1 <= args.metadata_workers <= 256:
        parser.error('metadata worker count must be 1..256')
    args.exe = [path.resolve(strict=True) for path in args.exe]
    return args


def generate(root, args):
    """生成稳定内容和已知重复组；Generate deterministic content and known groups."""
    rng = random.Random(args.seed)
    groups = {}
    total = 0
    previous = b''
    for index in range(args.files):
        layout = args.layout if args.layout != 'mixed' else ['flat', 'wide', 'deep'][index % 3]
        directory = root
        if layout == 'wide':
            directory /= f'w{index % 257:03}'
        elif layout == 'deep':
            directory = directory.joinpath(*[f'd{level}' for level in range(12)])
        directory.mkdir(parents=True, exist_ok=True)
        data = previous if index and args.duplicate_every and index % args.duplicate_every == 0 else rng.randbytes(args.sizes[index % len(args.sizes)])
        path = directory / f'f{index:08}.bin'
        path.write_bytes(data)
        groups.setdefault(hashlib.sha256(data).hexdigest(), []).append(path.relative_to(root).as_posix())
        previous = data
        total += len(data)
    expected = sorted(sorted(paths) for paths in groups.values() if len(paths) > 1)
    return total, expected


def canonical(stdout):
    """忽略局部组号和输出顺序；Ignore run-local group ids and output ordering."""
    groups = {}
    for line in stdout.decode('utf-8').splitlines():
        group, path = line.split('\t', 1)
        groups.setdefault(group, []).append(json.loads(path))
    return sorted(sorted(paths) for paths in groups.values())


def run(exe, root, log, mode, expected, files):
    """保存原始输出并核验分组；Save raw output and verify exact expected groups."""
    command = [str(exe), *scan_arguments(exe), '--format=tsv', '--color=never']
    if mode == 'fresh-rehash':
        command.append('--rehash')
    start = time.perf_counter()
    result = subprocess.run(command, cwd=root, capture_output=True, check=False)
    duration = time.perf_counter() - start
    log.with_suffix('.stdout').write_bytes(result.stdout)
    log.with_suffix('.stderr').write_bytes(result.stderr)
    metrics = {key: float(value) for key, value in re.findall(r'(\w+)=([0-9]+(?:\.[0-9]+)?)', result.stderr.decode('utf-8', errors='replace'))}
    valid = (result.returncode == 0 and canonical(result.stdout) == expected
             and metrics.get('scanned') == files
             and metrics.get('hashed') == (files if mode == 'fresh-rehash' else 0)
             and metrics.get('cached') == (0 if mode == 'fresh-rehash' else files))
    record = {'exe': str(exe), 'mode': mode, 'process_seconds': duration, 'returncode': result.returncode,
              'stdout_sha256': hashlib.sha256(result.stdout).hexdigest(), 'groups_match_oracle': valid, 'metrics': metrics}
    print(json.dumps(record), flush=True)
    if not valid:
        raise RuntimeError(f'result mismatch or process failure: {log}')
    return record


def main():
    """交替运行独立冷状态及暖缓存实验；Alternate fresh-state and warm-cache pairs."""
    args = arguments()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = output / 'dataset'
    root.mkdir()
    total, expected = generate(root, args)
    report = {'environment': {'platform': platform.platform(), 'python': platform.python_version(),
              'cpu_count': os.cpu_count(), 'filesystem_root': str(root.anchor),
              'note': 'Fresh application state, NOT cold OS page cache. Synthetic files were just written.'},
              'parameters': {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items() if key != 'exe'},
              'executables': [{'path': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest()} for exe in args.exe],
              'bytes': total, 'expected_groups': len(expected), 'runs': []}
    for trial in range(args.trials):
        order = list(range(len(args.exe)))
        if trial % 2:
            order.reverse()
        for index in order:
            state = root / '.same'
            state.mkdir()
            (state / 'config.toml').write_text(f'workers = {args.workers}\nqueue_capacity = {args.queue_capacity}\nmemory_bytes = {max(67108864, args.workers * 2134016)}\nbackend = "{args.backend}"\n', encoding='utf-8')
            if args.metadata_workers is not None:
                with (state / 'config.toml').open('a', encoding='utf-8') as config:
                    config.write(f'metadata_workers = {args.metadata_workers}\n')
            for mode in ['fresh-rehash', 'warm-cache']:
                record = run(args.exe[index], root, output / f't{trial}-e{index}-{mode}', mode, expected, args.files)
                record.update(trial=trial, executable_index=index)
                report['runs'].append(record)
                (output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            state.rename(output / f't{trial}-e{index}-state')


if __name__ == '__main__':
    main()
