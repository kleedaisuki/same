"""隔离的核显完整摘要实验，不改生产实现。 / Isolated iGPU full-digest experiment.

Usage / 用法: python tools/igpu_blake3_experiment.py > results.jsonl
Requires / 依赖: numpy, pyopencl, blake3. Inputs are restricted to powers of two.
Includes transfer, all tree levels, synchronization and digest readback; excludes
allocation, compilation and disk I/O. 包含传输、整棵树、同步及回读，不含分配编译和磁盘。
"""

import ctypes
import importlib.metadata
import json
from pathlib import Path
import time

import blake3
import numpy as np
import pyopencl as cl


def emit(**record):
    """输出可保存的实验记录。 / Emit a durable experiment record."""
    print(json.dumps(record), flush=True)


def main():
    """验证完整摘要并交替测 CPU 和核显。 / Validate digests and alternate CPU/iGPU trials."""
    caps = (ctypes.c_uint32 * 4)()
    query = ctypes.WinDLL("kernelbase").QueryIoRingCapabilities
    query.argtypes = [ctypes.c_void_p]
    query.restype = ctypes.c_long
    hr = query(ctypes.byref(caps))
    emit(kind="ioring", hresult=hr, max_version=caps[0],
         max_submission=caps[1], max_completion=caps[2], flags=caps[3])
    devices = [d for p in cl.get_platforms() for d in p.get_devices()]
    emit(kind="environment", devices=[d.name for d in devices],
         packages={p: importlib.metadata.version(p) for p in ("numpy", "pyopencl", "blake3")})
    device = next(d for d in devices if "Intel" in d.vendor and d.type & cl.device_type.GPU)
    if not device.endian_little:
        raise RuntimeError("Little-endian device required")
    ctx = cl.Context([device])
    queue = cl.CommandQueue(ctx, properties=cl.command_queue_properties.PROFILING_ENABLE)
    start = time.perf_counter()
    program = cl.Program(ctx, Path(__file__).with_name("igpu_blake3.cl").read_text(encoding="utf-8")).build()
    leaves, parents = cl.Kernel(program, "leaves"), cl.Kernel(program, "parents")
    emit(kind="device", name=device.name, driver=device.driver_version,
         unified_memory=bool(device.host_unified_memory), build_ms=1000*(time.perf_counter()-start))
    rng = np.random.default_rng(20260910)
    for size in (2048, 4096, 65536, 1048576, 16777216, 67108864):
        data = rng.integers(0, 256, size, dtype=np.uint8)
        expected = blake3.blake3(data).digest()
        source = cl.Buffer(ctx, cl.mem_flags.READ_ONLY, size)
        a = cl.Buffer(ctx, cl.mem_flags.READ_WRITE, size // 32)
        b = cl.Buffer(ctx, cl.mem_flags.READ_WRITE, size // 32)
        result = np.empty(32, dtype=np.uint8)

        def gpu(upload=True):
            """返回完整摘要和内核耗时；驻留模式仅作上界参考。 / Full digest and kernel time."""
            if upload:
                cl.enqueue_copy(queue, source, data, is_blocking=False)
            count = size // 1024
            events = [leaves(queue, (count,), None, source, a)]
            left, right = a, b
            while count > 1:
                count //= 2
                events.append(parents(queue, (count,), None, left, right, np.uint32(count == 1)))
                left, right = right, left
            cl.enqueue_copy(queue, result, left, is_blocking=True)
            kernel_ms = sum(e.profile.end-e.profile.start for e in events)/1e6
            return result.tobytes(), kernel_ms

        if gpu()[0] != expected:
            raise RuntimeError(f"Digest mismatch: {size}")
        repeats = max(16, min(100, 16777216 // size))
        for trial in range(5):
            order = ("cpu", "igpu_upload", "igpu_resident")
            if trial % 2:
                order = order[::-1]
            for backend in order:
                kernel_ms = 0.0
                start = time.perf_counter()
                for _ in range(repeats):
                    if backend == "cpu":
                        digest = blake3.blake3(data).digest()
                    else:
                        digest, elapsed = gpu(backend == "igpu_upload")
                        kernel_ms += elapsed
                    if digest != expected:
                        raise RuntimeError("Digest mismatch during timing")
                elapsed = time.perf_counter()-start
                emit(kind="measurement", backend=backend, size=size, trial=trial,
                     repeats=repeats, ms=elapsed*1000/repeats,
                     mib_s=size*repeats/1048576/elapsed,
                     kernel_ms=kernel_ms/repeats, verified=True)


if __name__ == "__main__":
    main()
