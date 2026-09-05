# 验证记录 / Validation record

日期 / Date: 2026-09-06。这里记录实际执行结果，不把尚未运行的 CI 视为证据。

These are executed results, not claims that the newly added CI has run.

| 环境 / Environment | 结果 / Result |
|---|---|
| Windows, MinGW GCC 16.1, Release, CUDA disabled | 7/7 CTest suites passed |
| Windows, MSVC 19.44, CUDA Toolkit 13.3, Release | Build and 7/7 suites passed; GPU unavailable, CPU fallback exercised |
| Ubuntu 24.04 WSL, GCC 13.3, CUDA 12.8, RTX 3070 Ti, Release | 7/7 suites passed with `SAME_REQUIRE_CUDA=1`; actual GPU execution required |
| Ubuntu 24.04 WSL, GCC 13.3, Debug, CPU ASan/UBSan | 7/7 suites passed |

CUDA 向量测试包括 35 组上游已知向量、四种流式输入粒度、空输入、重复完成、完成后继续更新、随机 4 MiB 输入差分和最低显存分配。CLI 测试包括 19 项；Windows 跳过 POSIX/GPU 专属项，要求 GPU 的 WSL 测试只跳过 Windows 大小写专属项。

CUDA tests cover 35 upstream known vectors, four streaming granularities, empty input, repeated finalization, updates after finalization, random 4 MiB differential checks, and minimal device allocation. The CLI suite has 19 cases; platform-specific skips are explicit.

其他覆盖：缓存复用、恢复 mtime 后的同长度改写、删除与重命名、硬链接、符号链接与状态目录排除、未来数据库版本不改写、人工摘要碰撞、并行多桶、有界队列、任务异常及 CPU 重试、三工作线程真实 CUDA 流程。

Additional coverage includes cache reuse, same-size writes with restored mtime, deletion/rename, hard links, symlink/state exclusion, future-schema non-mutation, injected digest collisions, parallel buckets, bounded admission, task errors/CPU retry, and three-worker real CUDA execution.

```sh
cmake -S . -B build-cuda -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=86
cmake --build build-cuda --parallel
SAME_REQUIRE_CUDA=1 ctest --test-dir build-cuda --output-on-failure

cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

Windows PowerShell 可先执行 `$env:SAME_REQUIRE_CUDA = '1'`。只有具备兼容 GPU 运行时的测试环境才应设置。

In PowerShell set `$env:SAME_REQUIRE_CUDA = '1'` only when a compatible GPU runtime is expected.

## 比较阶段的小型实验 / Small comparison experiment

在 WSL 本地文件系统生成 32 个相同的 8 MiB 文件，先运行一次建立缓存，再记录五次完整缓存扫描；CPU 后端，4 个工作线程，1 MiB 缓冲块，队列容量 8。结果均验证为一组 32 个成员、零重新哈希。

On the WSL local filesystem, generate 32 identical 8 MiB files, prime the cache once, then time five complete cached scans: CPU backend, four workers, 1 MiB blocks, queue capacity eight. Every result is checked for one group, 32 members, and zero rehashes.

| 版本 / Version | 五次耗时 / Five times (ms) | 中位数 / Median |
|---|---|---|
| `23197f2`, sequential comparisons | 166.0, 163.8, 182.4, 182.5, 168.3 | 168.3 ms |
| bounded parallel comparisons (`dacdc62`) | 64.6, 67.4, 62.9, 67.0, 62.8 | 64.6 ms |

复现脚本 / Reproduction:

```sh
python3 tools/benchmark.py /path/to/baseline/same /path/to/current/same \
  --files 32 --mib 8 --workers 4 --backend cpu --repeats 5
```

**限制：** 这是单机、热缓存、全重复数据的短实验；执行顺序未随机化，没有置信区间（confidence interval），不能推断冷盘表现、GPU 加速比或其他文件分布。脚本包含初始化、元数据与 SQLite 成本，不是裸比较内核吞吐。

**Limits:** one machine, warm cache, entirely duplicate data, short runs, fixed execution order, no confidence interval. This does not establish cold-storage performance, GPU speedup, or behavior on other distributions. Timings include startup, metadata, and SQLite, not just a comparison kernel.

## 未验证范围 / Not verified

macOS 实机、FAT/SMB 真实卷、网络文件系统锁语义、断电注入和 CUDA 设备故障注入尚未验证。Windows 旧文件标识回退仅有策略/序列化单元测试。GPU 运行时错误的完整重试通过注入 `ComputeError` 验证，不等价于真实设备丢失测试。

Not verified: real macOS, FAT/SMB volumes, network-lock semantics, power-loss injection, or CUDA device-loss injection. Legacy Windows identity fallback has policy/serialization tests. Injected `ComputeError` verifies restart control flow, not actual device loss.
