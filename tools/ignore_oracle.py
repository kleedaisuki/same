"""Compare same ignore semantics against Git. / 对照 Git 验证 same 忽略规则语义。

Usage / 用法: python tools/ignore_oracle.py --probe path/to/ignore_oracle_probe
All fixtures live in a fresh temporary directory; no user repository is modified.
所有测试数据位于新临时目录中，不修改用户仓库。
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

# Compact Cartesian matrix: 25 patterns × 39 paths. / 紧凑矩阵：25 条规则 × 39 个路径。
PATTERNS = [
    '*.tmp', 'build/\n!build/keep.txt',
    'build/\n!build/\nbuild/*\n!build/keep.txt', r'\#literal', r'\!literal',
    'trailing   ', r'escaped\ ', '[a-c].txt', '[!a-c].dat', '[[:digit:]].log',
    r'literal\*.txt', 'ab**cd', '/root-only/', 'a/**/target', '**/target', 'a/**',
    'foo/*', 'foo/**/bar', '[[]', '[]a]', '[abc', 'a\\', '***', 'a***b', '**a/b',
]
PATHS = [
    'x.tmp', 'sub/x.tmp', 'build/', 'build/x', 'build/keep.txt', '#literal',
    '!literal', 'trailing', 'trailing ', 'escaped ', 'escaped', 'b.txt', 'd.txt',
    'z.dat', 'b.dat', '5.log', 'x.log', 'literal*.txt', 'literalX.txt', 'abZZcd',
    'ab/x/cd', 'root-only/', 'sub/root-only/', 'a/target', 'a/b/c/target',
    'target', 'a/', 'a/b', 'a/b/c', 'foo/', 'foo/bar', 'foo/a/bar', 'foo/a/b/bar',
    '[', 'a', ']', '[abc', 'ab', 'aaa/b',
]


def main():
    """Return nonzero for any mismatch or failed oracle. / 不一致或参照失败时返回非零。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--git', default='git')
    args = parser.parse_args()
    probe = str(args.probe.resolve())
    failures = []
    with tempfile.TemporaryDirectory(prefix='same-ignore-oracle-') as temporary:
        root = Path(temporary)
        subprocess.run([args.git, 'init', '-q', temporary], check=True)
        (root / '.same').mkdir()
        # Real directories let Git infer type without matching an artificial empty component.
        # 实际目录让 Git 推断类型，避免末尾斜杠造成虚构的空路径分量匹配。
        for path in PATHS:
            if path.endswith('/'):
                (root / path).mkdir(parents=True, exist_ok=True)
        git = [args.git, '-C', temporary, '-c', 'core.ignorecase=false',
               '-c', 'core.excludesFile=', 'check-ignore', '--no-index', '-q']
        for pattern in PATTERNS:
            for settings in [root / '.gitignore', root / '.same/ignore']:
                settings.write_text(pattern + '\n', encoding='utf-8')
            actual = subprocess.run(
                [probe, temporary], input='\n'.join(PATHS) + '\n', text=True,
                capture_output=True, check=True,
            ).stdout.splitlines()
            if len(actual) != len(PATHS) or any(x not in ('0', '1') for x in actual):
                raise RuntimeError('invalid probe response')
            for path, result in zip(PATHS, actual):
                oracle = subprocess.run(git + [path.rstrip('/')], capture_output=True)
                if oracle.returncode not in (0, 1):
                    raise RuntimeError(oracle.stderr.decode(errors='replace'))
                if (result == '1') != (oracle.returncode == 0):
                    failures.append((pattern, path, result, oracle.returncode == 0))
    for failure in failures:
        print('MISMATCH', repr(failure))
    print(f'{len(PATTERNS) * len(PATHS)} cases; {len(failures)} mismatches')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
