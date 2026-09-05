"""Warm-cache comparison benchmark; not a cold-storage/GPU throughput claim.
热缓存比较基准；不能据此推断冷存储或 GPU 核函数吞吐量。
"""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time


def measure(executable, args):
    with tempfile.TemporaryDirectory(prefix="same-benchmark-") as directory:
        root = Path(directory)
        (root / ".same").mkdir()
        (root / ".same/config.toml").write_text(
            f'workers={args.workers}\nqueue_capacity={2 * args.workers}\n'
            f'backend="{args.backend}"\n', encoding="utf-8")
        content = bytes(range(256)) * (args.mib * 4096)
        for index in range(args.files):
            (root / f"file-{index:04}").write_bytes(content)
        samples = []
        for trial in range(args.repeats + 1):
            start = time.perf_counter()
            result = subprocess.run([executable], cwd=root, capture_output=True, text=True, check=True)
            elapsed = time.perf_counter() - start
            stats = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", result.stderr)}
            if stats.get("matches") != args.files or stats.get("groups") != 1:
                raise RuntimeError(f"wrong result: {result.stderr}")
            if trial:
                if stats.get("hashed") != 0:
                    raise RuntimeError(f"not a cached comparison: {result.stderr}")
                samples.append(elapsed)
        return dict(executable=executable, files=args.files, mib_per_file=args.mib,
                    workers=args.workers, backend=args.backend, seconds=samples,
                    median_seconds=statistics.median(samples), last_stats=stats)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executables", nargs="+")
    parser.add_argument("--files", type=int, default=32)
    parser.add_argument("--mib", type=int, default=8)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--backend", choices=("cpu", "cuda", "auto"), default="cpu")
    parser.add_argument("--repeats", type=int, default=5)
    arguments = parser.parse_args()
    if min(arguments.files, arguments.mib, arguments.workers, arguments.repeats) < 1 or arguments.files < 2:
        parser.error("files must be >=2 and all numeric arguments must be positive")
    print(json.dumps([measure(str(Path(executable).resolve()), arguments)
                      for executable in arguments.executables], indent=2))
