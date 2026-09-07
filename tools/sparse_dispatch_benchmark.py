"""稀疏文件逻辑读取基准，不代表冷盘；Sparse logical-read benchmark, not cold-disk IO."""
import argparse
from functools import cache
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import sqlite3
import subprocess
import time


@cache
def scan_arguments(exe):
    """每个程序只探测一次帮助，兼容旧基线；Probe help once per executable, preserving legacy baselines."""
    result = subprocess.run([str(exe), '--help'], capture_output=True, check=True, timeout=30)
    help_text = result.stdout.decode('utf-8', errors='replace')
    return ('scan', '-r', '--summary') if '--summary' in help_text else ()


def arguments():
    """严格参数及新输出目录契约；Parse arguments with a new-directory contract."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--gib', type=int, default=8)
    parser.add_argument('--trials', type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.gib <= 1024 or not 1 <= args.trials <= 100:
        parser.error('gib must be 1..1024 and trials 1..100')
    args.exe = args.exe.resolve(strict=True)
    args.output = args.output.resolve()
    return args


def fixture(root, size):
    """仅在新目录创建稀疏零文件；Create a sparse zero file only inside a new directory."""
    root.mkdir()
    path = root / 'large.bin'
    path.touch(exist_ok=False)
    if os.name == 'nt':
        subprocess.run(['fsutil', 'sparse', 'setflag', str(path)], check=True, capture_output=True)
    with path.open('r+b') as stream:
        stream.truncate(size)


def run(args, root, trial, backend, block, reference):
    """独立数据库、完整摘要和计数核验；Fresh database, full digest and count validation."""
    state = root / '.same'
    state.mkdir()
    config = f'backend="{backend}"\nworkers=1\nblock_bytes={block * 1024**2}\nmemory_bytes=134217728\ndevice_memory_bytes=134217728\nqueue_capacity=4\n'
    (state / 'config.toml').write_text(config, encoding='utf-8')
    command = [str(args.exe), *scan_arguments(args.exe), '--rehash', '--format=tsv', '--color=never']
    start = time.perf_counter()
    result = subprocess.run(command, cwd=root, capture_output=True, check=False)
    elapsed = (time.perf_counter() - start) * 1000
    name = f't{trial}-{backend}-b{block}'
    (args.output / f'{name}.stdout').write_bytes(result.stdout)
    (args.output / f'{name}.stderr').write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(f'process failed; inspect {name}.stderr')
    raw = result.stderr.decode('utf-8')
    metrics = {key: float(value) for key, value in re.findall(r'(\w+)=([0-9]+(?:\.[0-9]+)?)', raw)}
    with sqlite3.connect((state / 'state.db').as_uri() + '?mode=ro', uri=True) as db:
        rows = db.execute('select hex(digest) from files').fetchall()
    db.close()
    if len(rows) != 1 or len(rows[0][0]) != 64:
        raise RuntimeError('expected one full 32-byte digest')
    digest = rows[0][0]
    if reference is not None and digest != reference:
        raise RuntimeError('complete CPU reference digest mismatch')
    if result.stdout or metrics.get('scanned') != 1 or metrics.get('hashed') != 1 or metrics.get('cached') != 0 or metrics.get('hash_read_bytes') != args.gib * 1024**3:
        raise RuntimeError('unexpected groups or file/read/cache counts')
    state.rename(args.output / f'{name}-state')
    return {'trial': trial, 'backend': backend, 'block_mib': block, 'process_ms': elapsed,
            'metrics': metrics, 'raw_profile': raw, 'digest': digest, 'config': config}


def main():
    """显式执行才产生数据；Generate data only on explicit CLI execution."""
    args = arguments()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output / 'dataset'
    fixture(root, args.gib * 1024**3)
    report = {'note': 'Sparse logical zeros, NOT physical cold disk; each case has fresh DB; auto may use CPU.',
              'environment': {'platform': platform.platform(), 'python': platform.python_version()},
              'exe': str(args.exe), 'exe_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(),
              'gib': args.gib, 'trials': args.trials, 'runs': []}
    reference = None
    cases = [('cpu', 1), ('auto', 1), ('cpu', 16), ('auto', 16)]
    for trial in range(-1, args.trials):
        for backend, block in cases if trial < 0 or trial % 2 == 0 else reversed(cases):
            item = run(args, root, trial, backend, block, reference)
            reference = item['digest'] if reference is None else reference
            report['runs'].append(item)
            (args.output / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
            print(json.dumps(item), flush=True)


if __name__ == '__main__':
    main()
