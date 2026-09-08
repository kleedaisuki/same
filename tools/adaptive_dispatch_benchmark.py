"""验证合成长尾载荷的自适应分流；Validate adaptive routing on synthetic long-tail files.

稀疏文件与预热系统缓存不代表冷盘，不扫描用户数据。
Sparse files and warmed OS caches are NOT cold-disk evidence; no user data is scanned.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import sqlite3
import subprocess
import time


MIB = 1024 * 1024
SMALL_FILES = 128
SMALL_BYTES = 4096


def arguments():
    """校验有界参数，拒绝复用输出目录；Validate bounded inputs and reject reused output paths."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--trials', type=int, default=3)
    parser.add_argument('--workers', type=int, default=2)
    parser.add_argument('--large-files', type=int, default=10)
    parser.add_argument('--large-mib', type=int, default=512)
    args = parser.parse_args()
    for name, upper in [('trials', 100), ('workers', 256),
                        ('large_files', 256), ('large_mib', 65536)]:
        if not 1 <= getattr(args, name) <= upper:
            parser.error(f'{name.replace("_", "-")} must be 1..{upper}')
    args.exe = args.exe.resolve(strict=True)
    if not args.exe.is_file():
        parser.error('exe must be a file')
    args.output = args.output.resolve()
    if args.output.exists():
        parser.error('output must not already exist')
    return args


def fixture(root, count, size):
    """仅写新目录的唯一稀疏大文件和小文件；Write unique synthetic files in a new directory only."""
    root.mkdir()
    names = []
    for index in range(count):
        path = root / f'large-{index:03d}.bin'
        path.touch(exist_ok=False)
        if os.name == 'nt':
            subprocess.run(['fsutil', 'sparse', 'setflag', str(path)],
                           check=True, capture_output=True, timeout=30)
        with path.open('r+b') as stream:
            stream.truncate(size)
            stream.write(bytes([index]))
            stream.seek(size - 1)
            stream.write(bytes([255 - index]))
        names.append(path.name)
    for index in range(SMALL_FILES):
        path = root / f'small-{index:03d}.bin'
        path.write_bytes(index.to_bytes(4, 'little') + bytes(SMALL_BYTES - 4))
        names.append(path.name)
    return names


def configure(state, backend, workers):
    """CPU 和 GPU 服务均计入主机预算；Account for CPU workers and GPU service host buffers."""
    state.mkdir()
    cpu_budget = workers * (2 * MIB + MIB // 32 + 4096)
    gpu_block = 16 * MIB
    gpu_budget = 2 * gpu_block + gpu_block // 32 + 4096
    memory = max(64 * MIB, cpu_budget + gpu_budget)
    config = (f'workers = {workers}\nmetadata_workers = 1\nblock_bytes = {MIB}\n'
              f'gpu_min_bytes = {16 * MIB}\nmemory_bytes = {memory}\n'
              f'device_memory_bytes = {64 * MIB}\n'
              f'queue_capacity = {max(4, workers * 2)}\nbackend = "{backend}"\n')
    (state / 'config.toml').write_text(config, encoding='utf-8')
    return config


def read_digests(state, names):
    """只读核验全部路径及完整摘要；Read and verify every path and full 32-byte digest."""
    db = sqlite3.connect((state / 'state.db').as_uri() + '?mode=ro', uri=True)
    try:
        rows = db.execute('SELECT path, hex(digest) FROM files').fetchall()
    finally:
        db.close()
    # Store paths are UTF-8 BLOBs, not SQLite TEXT. / 仓储路径为UTF-8 BLOB，需显式解码。
    result = {path.decode('utf-8'): digest for path, digest in rows}
    if (len(rows) != len(names) or set(result) != set(names)
            or any(not re.fullmatch(r'[0-9A-F]{64}', digest) for digest in result.values())):
        raise RuntimeError('expected exactly the fixture paths and full 32-byte digests')
    return result


def run(args, root, trial, backend, names, logical_bytes, reference):
    """保存原始日志并验证独立数据库结果；Preserve raw logs and validate a fresh database run."""
    state = root / '.same'
    config = configure(state, backend, args.workers)
    name = f't{trial}-{backend}'
    command = [str(args.exe), 'scan', '-r', '--rehash', '--summary',
               '--format=tsv', '--color=never']
    start = time.perf_counter()
    result = subprocess.run(command, cwd=root, capture_output=True, check=False)
    elapsed = (time.perf_counter() - start) * 1000
    stdout_path = args.output / f'{name}.stdout'
    stderr_path = args.output / f'{name}.stderr'
    stdout_path.write_bytes(result.stdout)
    stderr_path.write_bytes(result.stderr)
    raw = result.stderr.decode('utf-8', errors='replace')
    metrics = {key: float(value) if '.' in value else int(value)
               for key, value in re.findall(r'(\w+)=([0-9]+(?:\.[0-9]+)?)', raw)}
    if result.returncode:
        raise RuntimeError(f'process failed; inspect {stderr_path}')
    if (result.stdout.strip() or metrics.get('scanned') != len(names)
            or metrics.get('hashed') != len(names) or metrics.get('cached') != 0
            or metrics.get('hash_read_bytes') != logical_bytes):
        raise RuntimeError(f'unexpected duplicates or scan/read/cache counts; inspect {name}')
    digests = read_digests(state, names)
    if reference is not None and digests != reference:
        raise RuntimeError(f'full CPU reference digest mismatch in {name}')
    archive = args.output / f'{name}-state'
    state.rename(archive)
    return {'trial': trial, 'warmup': trial < 0, 'backend': backend,
            'workers': args.workers, 'process_ms': elapsed, 'returncode': result.returncode,
            'metrics': metrics, 'raw_profile': raw, 'digests': digests,
            'config': config, 'command': command, 'state_archive': str(archive),
            'stdout_log': str(stdout_path), 'stderr_log': str(stderr_path)}


def save_report(output, report):
    """每阶段原子替换报告，保留已完成证据；Atomically preserve completed evidence after each step."""
    temporary = output / 'report.json.tmp'
    temporary.write_text(json.dumps(report, indent=2), encoding='utf-8')
    temporary.replace(output / 'report.json')


def main():
    """先预热，再交替 CPU/auto 顺序，不预设 GPU 收益；Warm up then alternate without assuming GPU wins."""
    args = arguments()
    args.output.mkdir(parents=True, exist_ok=False)
    root = args.output / 'dataset'
    size = args.large_mib * MIB
    logical_bytes = args.large_files * size + SMALL_FILES * SMALL_BYTES
    report = {'note': 'Synthetic sparse files; NOT cold disk; no cache flush; fresh DB per run. '
                      'Routing counts and speedups are observations, not required outcomes.',
              'environment': {'platform': platform.platform(), 'python': platform.python_version(),
                              'cpu_count': os.cpu_count()},
              'exe': str(args.exe), 'exe_sha256': hashlib.sha256(args.exe.read_bytes()).hexdigest(),
              'fixture': {'large_files': args.large_files, 'large_bytes': size,
                          'small_files': SMALL_FILES, 'small_bytes': SMALL_BYTES,
                          'logical_bytes': logical_bytes},
              'trials': args.trials, 'status': 'generating', 'runs': []}
    save_report(args.output, report)
    try:
        names = fixture(root, args.large_files, size)
        report['status'] = 'running'
        save_report(args.output, report)
        reference = None
        for trial in range(-1, args.trials):
            order = ('auto', 'cpu') if trial >= 0 and trial % 2 else ('cpu', 'auto')
            for backend in order:
                record = run(args, root, trial, backend, names, logical_bytes, reference)
                if reference is None:
                    reference = record['digests']
                report['runs'].append(record)
                save_report(args.output, report)
                print(json.dumps({'trial': trial, 'backend': backend,
                                  'process_ms': record['process_ms'],
                                  'metrics': record['metrics']}), flush=True)
        report['status'] = 'complete'
        save_report(args.output, report)
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        save_report(args.output, report)
        raise


if __name__ == '__main__':
    main()
