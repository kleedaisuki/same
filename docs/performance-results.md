# 性能优化验证记录 / Performance validation record

> 历史阶段记录；当前实现契约见 [auto-dispatch](auto-dispatch.md)，最新分派实验见 [dispatch-results](dispatch-results.md)。
> Historical-stage record; follow those documents for the current policy and latest measurements.

## 已实现 / Implemented

- 有界动态目录/元数据任务图，Windows 原生批量枚举；不是全树预收集。
- 元数据句柄移交与完成顺序哈希结果，保留文件变更检查和失败回滚。
- SQLite 热语句复用与缓存命中仅更新扫描代次。
- CUDA 32 叶融合子树，主体 D2H 减少 32 倍；主机字节比较消除比较 H2D。
- 小于 `gpu_min_bytes` 的文件路由到 CPU SIMD；默认 16 MiB，仅为保守策略。

Implemented bounded dynamic traversal, native Windows enumeration, handle handoff, completion-order
hash collection, prepared SQL reuse, generation-only cache updates, 32-leaf CUDA subtree fusion,
host comparisons, and configurable size-aware CPU routing. External digest, cache schema, output
ordering and file-change checks are retained.

## 验证范围 / Validation scope

- Windows x64 CUDA: `SAME_REQUIRE_CUDA=1`, all 12 CTest tests passed.
- Linux (WSL Ubuntu 24.04, GCC 13.3) Debug ASan/UBSan: all 12 tests passed.
- Windows x86 CPU: all 12 CTest tests passed.
- NVIDIA Compute Sanitizer 12.8 on `compute_tests`: memcheck reports 0 errors; racecheck reports 0 hazards, 0 errors and 0 warnings. These are tested-input results, not an exhaustive proof.
- Tests include official BLAKE3 vectors, randomized fused subtree boundaries, minimal device budgets,
  metadata concurrency 1/4/16 and capacity 1/3/64, long Unicode paths, cancellation, completion order,
  computation failure after a read, reopen-at-zero retry, replacement detection, warm-cache compatibility,
  size-routing threshold boundaries and explicit routing disablement.

这些检查不能证明任意硬件或冷盘上的普遍加速，也不提供针对恶意并发修改的文件系统快照。
These checks do not prove universal speedups, cold-storage throughput or snapshot isolation against
adversarial concurrent mutation. The user's full D-drive dataset has not been rescanned by the agent.

## 复现 / Reproduce

Use `tools/scan_benchmark.py` with two executable snapshots, `--files 20000 --trials 3 --workers 8
--queue-capacity 512 --backend cpu`; repeat with executable order reversed on a new output directory.
The tool records binary hashes, input seed, raw output, all profile metrics and an independent group oracle.
Use `--metadata-workers 1`, `4`, `8` only with the candidate; older binaries reject this new key.
CPU/GPU microbenchmark instructions and raw CSVs are in [GPU report](gpu-optimization.md).

下列扫描结果区分系统缓存热但数据库全新的重哈希，与摘要缓存命中的二次扫描。
Fresh database does not mean cold OS cache. First-access samples are explicitly separated below.

## CPU synthetic scan comparison, 2026-09-07

All cases: Windows 11, 20 logical CPUs, 8 hash workers, queue 512, CPU backend, default metadata workers 4 for candidate. Sizes 0/128/1024/4096/16384/65536 bytes, mixed flat/wide/deep directories, duplicate every 10. Executable SHA-256 and every numeric stderr field are in report.json. All duplicate oracles and scanned/hashed/cached checks passed. OS cache is NOT flushed. No actual user data scanned.

## 5,000 files (77.52 MiB)

Baseline-first fixture: first baseline elapsed 17.012 s, scan 16.076 s. Remaining baseline fresh elapsed 1.515/1.421 s; candidate fresh 0.889/0.904/0.893 s. Baseline warm 1.102–1.154 s; candidate warm 0.606–0.623 s.

Candidate-first independent fixture: first candidate elapsed 5.445 s, scan 4.492 s. Remaining candidate fresh 0.880/0.918 s; baseline fresh 1.928/1.564/1.561 s. Candidate warm 0.594–0.656 s; baseline warm 1.136–1.164 s.

First-access observations are single samples from separate fixtures, not a valid stable speedup ratio. Warm-OS direction is consistent under reversed executable order.

## 20,000 files (candidate first)

First candidate elapsed 27.258 s, scan 23.630 s is separated from warmed observations.
Warm-OS fresh-state candidate elapsed 3.822/3.997 s; baseline 8.905/10.414/9.071 s.
Warm-state candidate elapsed 2.292/2.360/2.350 s; baseline 6.336/6.122/5.881 s.
Candidate warmed fresh scan 1.598/1.601 s; baseline 5.285/6.306/5.353 s.
Candidate warm scan 0.479/0.486/0.488 s; baseline 2.538/2.617/2.521 s.

## Metadata ablation (same already-warmed 5,000-file fixture)

Orders 1/4/8, 8/4/1, 4/1/8; independent database per pair; median milliseconds:

| metadata workers | fresh elapsed | fresh scan | warm elapsed | warm scan | fresh summed metadata | fresh DB | fresh summed hash |
|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1000.616 | 509.236 | 781.759 | 348.058 | 415.154 | 37.014 | 1224.347 |
| 4 | 876.990 | 431.722 | 588.078 | 162.630 | 1357.785 | 41.525 | 3248.951 |
| 8 | 865.171 | 400.784 | 612.410 | 169.304 | 1462.652 | 45.276 | 3077.776 |

Four is a reasonable conservative default: eight gives little fresh improvement and slightly worse warm behavior here. Summed worker durations rise with contention/overlap and must not be added to wall time or confused with CPU execution time. No statistical significance or general disk optimum claimed. GPU kernel performance is outside this CPU benchmark.

Raw report directories: compare-5000, compare-5000-reverse, compare-20000, metadata-ablation. metadata_ablation.py records the exact supplemental experiment. Cross-workload extrapolation to the user's 245,091-file scan is unproven. Residual first-access delay remains important; profile its OS I/O, filters and per-stage timing rather than attributing everything to enumeration.
