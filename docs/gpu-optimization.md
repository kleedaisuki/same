# CUDA 子树融合与传输削减 / CUDA subtree fusion and transfer reduction

> 历史阶段记录；当前实现契约见 [auto-dispatch](auto-dispatch.md)，最新分派实验见 [dispatch-results](dispatch-results.md)。
> Historical-stage record; follow those documents for the current policy and latest measurements.

## 结论与范围 / Conclusion and scope

目标不是提高 GPU 利用率，而是降低完整哈希时间。融合后在本机四档输入均快于旧 CUDA，
但官方 CPU SIMD 实现依然更快，因此不能把“CUDA 可用”当作“CUDA 更快”。
The objective is end-to-end hashing latency, not GPU utilization. The fused implementation wins
against historical CUDA in these measurements, but the official SIMD CPU still wins all four sizes.
These are single-worker, warm-memory microbenchmarks, not disk-scan throughput claims.

## 实现与不变量 / Implementation and invariants

- 一个 32-thread block 计算 32 个 BLAKE3 叶，再用共享内存完成 5 层二叉归约；
  所有线程参加每次屏障，没有 grid-wide synchronization 或 cooperative-launch 限制。
  One 32-thread block hashes 32 leaves and reduces five tree levels in shared memory. Every thread
  reaches every barrier; no grid-wide synchronization or cooperative-launch support is required.
- 只提交全局叶索引对齐到 32 的完整子树。前后边界各最多 31 叶由 CPU 计算。
  Only complete globally aligned subtrees enter the kernel; CPU handles at most 31 leaves per edge.
- 每 32 KiB GPU 输入仅回传 32 B，而非原来的 1024 B：D2H 负载缩小 32 倍。
  Each 32 KiB GPU input returns 32 B instead of 1024 B, a 32x D2H payload reduction.
- 31 次 parent compression 融合入同一内核，中间 CV 不往返全局内存/主机；
  提高每次主机传输字节所承载的 GPU 工作量，但不改变密码算法或人为增加无用运算。
  Thirty-one parent compressions are fused; intermediate CVs remain shared-memory resident.
  More useful GPU work is performed per transferred byte without changing the cryptographic algorithm.
- 最终叶保留在主机并正确应用 ROOT；单叶/微小批量不再启动 CUDA。
  The final leaf stays host-resident, preserving ROOT semantics and avoiding tiny CUDA launches.
- 比较输入本就在主机，用 memcmp 精确比较，去掉两份 H2D、清零及结果同步。
  Host-resident comparison uses exact memcmp and eliminates both H2D copies and result synchronization.
- 删除第二输入、最终 Output、mismatch flag 的设备分配。为保持配置兼容，容量选择仍沿用旧的
  保守预算公式；真实设备分配仅 capacity + max(32, floor(capacity / 32768) * 32)。
  Remove unused allocations while preserving historical conservative capacity selection.
  Actual device allocation is capacity + max(32, floor(capacity / 32768) * 32), excluding CUDA context.
- 子树压栈时 count 是对齐的二次幂；chunks/count 的二进制进位控制更高层归并。
  Subtree pushes use aligned power-of-two counts and binary carries in chunks/count.

## 生产与研究依据 / Production and research basis

1. [NVIDIA CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)
   建议优先减少主机/设备传输，并把小传输合并。这里直接消除 host-resident 比较传输和末叶往返。
   Recommends minimizing transfers and batching small transfers; we eliminate unnecessary round trips.
2. [BLAKE3 原始规范与设计论文 / original specification and design paper](https://github.com/BLAKE3-team/BLAKE3-specs)
   树结构支持并行子树计算，但必须保留 chunk counter、树形和 ROOT。这里严格遵循对齐子树。
   Its tree structure supports subtree parallelism, subject to chunk counters, tree shape, and ROOT semantics.
3. [官方 BLAKE3 实现 / official BLAKE3 implementation](https://github.com/BLAKE3-team/BLAKE3)
   作为独立 SIMD CPU 基线及正确性判据；不能只与旧的低效 GPU 实现比较。
   Provides an independent SIMD CPU baseline and correctness oracle, preventing a weak-baseline-only claim.

规范是原始技术研究，非声称同行评审的新 GPU 结果。本实现属于基于树并行的工程优化；
尚未使用跨文件 GPU batching、持久内核、CUDA Graphs 或零拷贝存储。
The specification is primary technical research, not a claimed peer-reviewed GPU result.
Cross-file batching, persistent kernels, CUDA Graphs, and direct storage are not implemented.

## 实测 / Measurements

2026-09-07，RTX 3070 Ti Laptop GPU，驱动 572.61，CUDA 12.8，MSVC 19.44 Release，1 MiB 输入块。
历史基线 c53d318a91701b2cab31ba633769b539725c96ac。初始化不计时；每档预热一轮、记录三轮中位数。
2026-09-07, RTX 3070 Ti Laptop GPU, driver 572.61, CUDA 12.8, MSVC 19.44 Release, 1 MiB updates.
Baseline revision c53d318a91701b2cab31ba633769b539725c96ac. Excludes initialization; one warmup, median of three rounds.

| Input / 输入 | CPU µs | Old CUDA µs / 旧 | Fused CUDA µs / 新 | Old/new / 加速 |
|---|---:|---:|---:|---:|
| 4 KiB | 1.457 | 172.123 | 9.989 | 17.23x |
| 64 KiB | 16.373 | 242.695 | 216.384 | 1.12x |
| 1 MiB | 249.013 | 552.300 | 391.731 | 1.41x |
| 16 MiB | 5600.490 | 10127.600 | 7090.140 | 1.43x |

原始记录见 gpu-benchmark-2026-09-07.csv，源码见 ../tools/gpu_benchmark.cpp。
Raw observations: gpu-benchmark-2026-09-07.csv; source: ../tools/gpu_benchmark.cpp.

局限：固定重复数据、单设备、单工作线程、没有锁定频率或随机交错后端顺序；不是统计显著性声明。
CPU 与 GPU 的输入相同，均计入 hasher 构造、update、finish，不含文件打开/IO。
Limitations: repeated fixed data, one GPU, one worker, no clock lock or randomized backend order;
no statistical significance claim. Both paths include hasher creation, update, and finish, not file IO.

## 复现与验证 / Reproduction and validation

1. `cmake --build build/cuda --target compute_tests`，设置 `SAME_REQUIRE_CUDA=1` 后运行
   `build/cuda/compute_tests.exe`，避免无设备时静默跳过。实机通过官方向量、重复 finish、
   finish 后继续 update、随机 4 MiB+113、32 KiB±1/64 KiB+1/1 MiB 增量及最小预算。
   Build and run with SAME_REQUIRE_CUDA=1; real-device checks cover official vectors, repeatable finish,
   continued updates, random 4 MiB+113 input, boundary splits, and minimum budget.
2. Developer Command Prompt 中用 `nvcc -std=c++20 -O2 -Xcompiler=/MD -Iinclude -Isrc
   -Dtry_cuda_compute=try_cuda_baseline -c baseline.cu -o baseline.obj` 编译历史 CUDA 文件。
   从上述固定提交提取 baseline.cu，不要覆盖工作树生产文件。
   Extract historical CUDA into a separate file; compile with the renamed entry point above.
3. 用 MSVC `/std:c++20 /EHsc /O2 /MD /DSAME_BENCH_BASELINE /Iinclude` 编译 benchmark，
   链接 baseline.obj、build/cuda/same_core.lib、build/cuda/_deps/blake3-build/blake3.lib 和 cudart.lib。
   不定义 SAME_BENCH_BASELINE 可仅比较 CPU 和当前 CUDA。
   Link those libraries; omit SAME_BENCH_BASELINE for CPU-versus-current-only measurements.
4. 用 Nsight Systems 检查 CUDA 调用和传输数量，用 Nsight Compute 检查寄存器溢出、全局加载合并、
   实际算术强度与 occupancy。当前没有这些计数器证据，不声称已经消除所有访存瓶颈。
   Inspect transfer counts in Nsight Systems and spills/coalescing/arithmetic intensity/occupancy in
   Nsight Compute. Those counters have not yet been collected; remaining memory bottlenecks are unproven.

下一步应先以真实文件大小分布测 CPU 路由和多工作线程，不应凭 GPU 理论算力强行 offload。
当前逐线程 1 KiB 叶读取仍可能有不合并全局加载，32-thread blocks 亦可能限制 occupancy。
Next measure size-aware CPU routing and multiple workers on real file distributions. Per-thread 1 KiB
leaf reads may still be poorly coalesced, and 32-thread blocks may limit occupancy.

### 扩展尺寸复核 / Extended-size check

随后同样配置增加 64 MiB 与 256 MiB，每轮 2 次，三轮中位数：
A subsequent run adds 64 MiB and 256 MiB, two repetitions per round, medians of three rounds:

| Input / 输入 | CPU ms | Old CUDA ms / 旧 | Fused CUDA ms / 新 |
|---|---:|---:|---:|
| 64 MiB | 15.982 | 42.140 | 31.374 |
| 256 MiB | 63.033 | 167.474 | 120.925 |

见 gpu-benchmark-extended-2026-09-07.csv；当前工具包含扩展尺寸，并将最少重复次数设为 2
（初始四档实验为 8）。没有观察到单线程 CPU/GPU 性能交叉点；阈值只能是策略，不是实证最优点。
See gpu-benchmark-extended-2026-09-07.csv. The current tool includes extended sizes and uses a minimum
of two repetitions (the initial four-size experiment used eight). No single-worker CPU/GPU crossover
was observed; a routing threshold is a policy rather than an empirically optimal crossover.

### 设备专项验证 / Device-specific validation

集成后对 `compute_tests` 运行 NVIDIA Compute Sanitizer 12.8：
After integration, NVIDIA Compute Sanitizer 12.8 ran the real-device compute suite:

```text
compute-sanitizer --tool memcheck --error-exitcode 99 build/cuda/compute_tests.exe
ERROR SUMMARY: 0 errors
compute-sanitizer --tool racecheck --error-exitcode 99 build/cuda/compute_tests.exe
RACECHECK SUMMARY: 0 hazards displayed (0 errors, 0 warnings)
```

此结果覆盖测试实际执行的内核，不是所有输入的形式化证明；未采集 Nsight 性能计数器。
These results cover executed test kernels, not a formal proof for all inputs. Nsight performance
counters were not collected. See [integrated results](performance-results.md) for scan measurements.
