"""复核既有原始统计，不重跑性能。 / Validate existing raw statistics without benchmarking.

Usage / 用法: python tools/research_validation/check_raw.py
Only Python standard library is needed. / 仅依赖 Python 标准库。
"""

from collections import defaultdict
import json
import math
from pathlib import Path
import statistics


def check_file(path, expected_rows, expected_groups):
    """检查记录数量、摘要计数和有限正吞吐。 / Check counts, verification and finite throughput."""
    rows = [json.loads(s) for s in path.read_text(encoding="utf-8").splitlines()]
    if len(rows) != expected_rows:
        raise ValueError(f"Unexpected row count: {path.name}: {len(rows)}")
    groups = defaultdict(list)
    for record in rows:
        if record.get("kind") == "measurement":
            if record["verified"] is not True:
                raise ValueError("Unverified OpenCL measurement")
            value = record["mib_s"]
        elif "aggregate_mib_s" in record:
            if record["verified_digests"] != record["repeats"] * record["workers"]:
                raise ValueError("Native digest count mismatch")
            value = record["aggregate_mib_s"]
        else:
            if record.get("kind") not in ("ioring", "environment", "device"):
                raise ValueError("Unexpected record type")
            continue
        if not math.isfinite(value) or value <= 0:
            raise ValueError("Invalid throughput")
        groups[(record["size"], record["backend"])].append((record["trial"], value))
    if len(groups) != expected_groups:
        raise ValueError("Unexpected group count")
    print(json.dumps(dict(file=path.name, rows=len(rows), groups=len(groups))))
    for (size, backend), samples in sorted(groups.items()):
        if sorted(trial for trial, _ in samples) != list(range(5)):
            raise ValueError("Missing or repeated trial")
        values = [value for _, value in samples]
        print(json.dumps(dict(size=size, backend=backend, count=len(values),
                              median=round(statistics.median(values), 2),
                              minimum=round(min(values), 2), maximum=round(max(values), 2))))


def main():
    """验证四个指定文件，缺失也失败。 / Verify four named files, failing on missing data."""
    root = Path(__file__).resolve().parents[2] / "benchmarks" / "igpu-20260910"
    for name, rows, groups in (("cpu8.jsonl", 5, 1), ("dispatch-bulk.jsonl", 30, 6),
                               ("dispatch.jsonl", 50, 10), ("opencl.jsonl", 93, 18)):
        check_file(root / name, rows, groups)


if __name__ == "__main__":
    main()
