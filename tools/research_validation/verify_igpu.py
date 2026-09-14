"""复核受限核显摘要，不测吞吐。 / Verify restricted iGPU digests, not throughput.

Usage / 用法: python tools/research_validation/verify_igpu.py
Requires / 依赖: numpy, pyopencl, blake3 and an Intel GPU OpenCL driver.
"""

import json
from pathlib import Path

import blake3
import numpy as np
import pyopencl as cl


def digest(ctx, queue, leaf, parent, data):
    """提交完整二叉树并回读摘要。 / Submit a full binary tree and read its digest."""
    size = len(data)
    source = cl.Buffer(ctx, cl.mem_flags.READ_ONLY | cl.mem_flags.COPY_HOST_PTR,
                       hostbuf=data)
    a, b = [cl.Buffer(ctx, cl.mem_flags.READ_WRITE, size // 32) for _ in range(2)]
    count = size // 1024
    leaf(queue, (count,), None, source, a)
    while count > 1:
        count //= 2
        parent(queue, (count,), None, a, b, np.uint32(count == 1))
        a, b = b, a
    result = np.empty(32, dtype=np.uint8)
    cl.enqueue_copy(queue, result, a, is_blocking=True)
    return result.tobytes()


def main():
    """核验 21 个独立输入，失败退出。 / Verify 21 independent inputs, failing explicitly."""
    devices = [d for p in cl.get_platforms() for d in p.get_devices()
               if "Intel" in d.vendor and d.type & cl.device_type.GPU]
    if not devices or not devices[0].endian_little:
        raise RuntimeError("A little-endian Intel OpenCL GPU is required")
    ctx = cl.Context([devices[0]])
    queue = cl.CommandQueue(ctx)
    source = Path(__file__).resolve().parents[1] / "igpu_blake3.cl"
    program = cl.Program(ctx, source.read_text(encoding="utf-8")).build()
    leaf, parent = cl.Kernel(program, "leaves"), cl.Kernel(program, "parents")
    rng = np.random.default_rng(43)
    checks = 0
    for size in (2048, 4096, 8192, 65536, 1048576, 16777216, 67108864):
        for pattern in ("zero", "ff", "random"):
            data = (rng.integers(0, 256, size, dtype=np.uint8) if pattern == "random"
                    else np.full(size, 255 if pattern == "ff" else 0, dtype=np.uint8))
            if digest(ctx, queue, leaf, parent, data) != blake3.blake3(data).digest():
                raise RuntimeError(f"Digest mismatch: size={size}, pattern={pattern}")
            checks += 1
            print(json.dumps(dict(size=size, pattern=pattern, verified=True)))
    print(json.dumps(dict(checks=checks, device=devices[0].name)))


if __name__ == "__main__":
    main()
