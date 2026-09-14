"""随机配对扫描消融，独立状态且不删除数据。 / Randomized paired scans with isolated state, no deletion.

Example / 示例: python tools/contextual_scan_benchmark.py --exe build/cuda/same.exe --run
Without --run only generate the fixture. / 不带 --run 只创建语料。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import time
import uuid

MODES = {
    "cpu_pgo": ["--cpu"],
    "cpu_no_pgo": ["--cpu", "--no-pgo"],
    "auto_pgo": [],
    "auto_no_pgo": ["--no-pgo"],
}


def canonical(stdout):
    """规范化 TSV 为与编号无关的 JSON 组。 / Normalize TSV to group-ID-independent JSON groups."""
    groups = {}
    for line in stdout.splitlines():
        key, path = line.split("\t", 1)
        groups.setdefault(int(key), []).append(json.loads(path).replace("\\", "/"))
    return sorted(sorted(paths) for paths in groups.values())


def generate(base, gpu_min_bytes):
    """按固定种子生成数据；硬链接隔离工作目录而不复制负载。 / Seeded data and hardlinked isolated roots."""
    rng = random.Random(52741)
    manifest = {}
    for workload, sizes in {
        "small": [(8192 << (i % 5)) for i in range(512)],
        "mixed": [4 << 20] * 24 + [32 << 20] * 4,
    }.items():
        source = base / workload / "payload"
        source.mkdir(parents=True)
        expected = []
        for i, size in enumerate(sizes):
            path = source / f"file-{i:04d}.bin"
            # 每组相邻二文件重复；剩余文件随机唯一。 / Adjacent pair duplicates; others random unique.
            if i % 8 == 1 and sizes[i - 1] == size:
                path.write_bytes((source / f"file-{i - 1:04d}.bin").read_bytes())
                expected.append([f"file-{i - 1:04d}.bin", path.name])
            else:
                path.write_bytes(rng.randbytes(size))
        # 小文件尺寸相邻不同；增加真正重复且保持原有规模。 / Small adjacent sizes differ; add real duplicate.
        if workload == "small":
            (source / "file-0005.bin").write_bytes((source / "file-0000.bin").read_bytes())
            expected.append(["file-0000.bin", "file-0005.bin"])
        manifest[workload] = {"files": len(sizes), "bytes": sum(sizes), "expected": sorted(expected)}
        for mode in MODES:
            root = base / workload / mode
            (root / ".same").mkdir(parents=True)
            for path in source.iterdir():
                os.link(path, root / path.name)
            (root / ".same" / "config.toml").write_text(
                'workers = 4\nmetadata_workers = 2\nblock_bytes = 1048576\n'
                'memory_bytes = 134217728\ndevice_memory_bytes = 33554432\n'
                'queue_capacity = 16\nbackend = "auto"\n' + f'gpu_min_bytes = {gpu_min_bytes}\n', encoding="utf-8")
    (base / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return manifest


def scan(exe, root, mode, phase, round_index):
    """只计子进程端到端墙钟，保留原始输出。 / Time subprocess wall duration and retain raw output."""
    command = [str(exe), "scan", "--rehash", "--summary", "--no-telemetry", "--format=tsv", "--color=never", *MODES[mode]]
    start = time.perf_counter_ns()
    result = subprocess.run(command, cwd=root, text=True, encoding="utf-8", capture_output=True, timeout=300)
    wall_ms = (time.perf_counter_ns() - start) / 1e6
    if result.returncode:
        raise RuntimeError(f"{root}: {result.returncode}\n{result.stderr}")
    metrics = {key: float(value) for key, value in re.findall(r"([\w.]+)=(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)", result.stderr)}
    return {"mode": mode, "phase": phase, "round": round_index, "wall_ms": wall_ms,
            "metrics": metrics, "groups": canonical(result.stdout), "stdout": result.stdout, "stderr": result.stderr}


def execute(exe, base, manifest, rounds):
    """逐轮随机顺序，先学习预热再测量。 / Randomize each round after separate learning warmup."""
    output = base / "raw.jsonl"
    rng = random.Random(99811)
    rows = []
    with output.open("x", encoding="utf-8") as stream:
        for workload, description in manifest.items():
            ground = scan(exe, base / workload / "cpu_no_pgo", "cpu_no_pgo", "ground", -1)
            if ground["groups"] != description["expected"]:
                raise RuntimeError("CPU ground truth differs from fixture expectation")
            ground["workload"] = workload
            stream.write(json.dumps(ground) + "\n")
            for round_index in range(-2, rounds):
                order = list(MODES)
                rng.shuffle(order)
                for mode in order:
                    row = scan(exe, base / workload / mode, mode, "warmup" if round_index < 0 else "measured", round_index)
                    row["workload"] = workload
                    if row["groups"] != ground["groups"]:
                        raise RuntimeError(f"Output mismatch: {workload}/{mode}")
                    if row["metrics"].get("hashed") != description["files"] or row["metrics"].get("cached") != 0:
                        raise RuntimeError("Hashing unexpectedly bypassed")
                    stream.write(json.dumps(row) + "\n")
                    stream.flush()
                    rows.append(row)
                    print(workload, mode, row["phase"], round_index, round(row["wall_ms"], 3), flush=True)
    summary = {}
    for workload in manifest:
        summary[workload] = {}
        for mode in MODES:
            selected = [r for r in rows if r["phase"] == "measured" and r["workload"] == workload and r["mode"] == mode]
            summary[workload][mode] = {"wall_ms": [r["wall_ms"] for r in selected], "median_wall_ms": statistics.median(r["wall_ms"] for r in selected)}
    (base / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")


def main():
    """创建唯一语料或恢复已准备语料；显式开关才运行扫描。 / Prepare unique corpus; explicit flag runs scans."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=Path("build/cuda/same.exe"))
    parser.add_argument("--prepared", type=Path)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--gpu-min-bytes", type=int, default=0)
    args = parser.parse_args()
    if args.rounds < 7:
        parser.error("at least seven measured rounds required")
    base = args.prepared or Path("build") / ("contextual-scan-" + uuid.uuid4().hex[:12])
    manifest = json.loads((base / "manifest.json").read_text()) if args.prepared else generate(base, args.gpu_min_bytes)
    exe = args.exe.resolve()
    metadata = {"python": platform.python_version(), "platform": platform.platform(), "exe": str(exe),
                "exe_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(), "rounds": args.rounds,
                "cache": "OS cache warm; no eviction", "gpu_min_bytes": int(re.search(r"gpu_min_bytes = (\d+)", (base / "small" / "cpu_pgo" / ".same" / "config.toml").read_text()).group(1)), "seed_order": 99811, "seed_payload": 52741}
    (base / "environment.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(base.resolve(), flush=True)
    if args.run:
        execute(exe, base.resolve(), manifest, args.rounds)


if __name__ == "__main__":
    main()



