# GPU 性能剖析与取舍 / GPU profiling and decisions

## 问题与实验边界 / Question and boundary

**只有包含 H2D、kernel、D2H、同步和主机树归并的完整摘要快于官方 SIMD CPU，GPU 才有价值。**
不能以 kernel 单独加速或 GPU 利用率来推断文件扫描加速。
**GPU is useful only when the complete digest—including H2D, kernel, D2H, synchronization and host
reduction—beats the official SIMD CPU.** Kernel-only speedups do not prove scan speedups.

2026-09-07，RTX 3070 Ti Laptop GPU（46 SM，CC 8.6），驱动 572.61，CUDA 12.8，MSVC 19.44。
本实验保留生产编译架构 compute_52/sm_52，经驱动在当前设备执行，不用不同架构混淆对比。
2026-09-07: RTX 3070 Ti Laptop GPU (46 SMs, CC 8.6), driver 572.61, CUDA 12.8, MSVC 19.44.
Both variants retain production compute_52/sm_52 compilation rather than changing architecture.

使用 Nsight Compute 2025.1.1，`--set full --launch-count 1`，40 次 replay；各 kernel 记录
SM 频率均为 1.03 GHz。计数器访问成功，没有更改驱动设置、权限或系统策略。
Nsight Compute 2025.1.1: full set, first kernel, 40 replay passes, reported SM clock 1.03 GHz in all
four captures. Counters were accessible; no driver settings, permissions, or system policy changed.

基线不是最早的逐叶实现，而是上一轮已融合 32 叶的当前工作树。源码、头文件、独立二进制与
SHA256 保存在 `.cache/perf/gpu/{fused-baseline.cu,blake3_scalar.hpp,baseline.exe,baseline-sha256.txt}`。
The baseline is the previous 32-leaf-fused worktree, not the original per-leaf implementation.
Source, header, standalone executable, and SHA256 are preserved in the paths above.

## 观察 → 推断 → 实验 / Observation → inference → experiment

| 观察 / Observation | 机制解释 / Mechanism | 采取的实验 / Experiment |
|---|---|---|
| 31 blocks，46 SM；occupancy 2.08% | 1 MiB 提交无法填满 GPU / insufficient work per launch | 测 1 MiB 和 16 MiB 更新粒度 / test both block sizes |
| global sectors 97% excessive，每 sector 有效 1 B | 每叶逐字节装载及线程跨 1 KiB 步进 / byte loads and strided leaves | 完整叶改对齐 32-bit words / aligned word loader |
| shared wavefronts 71% excessive | 8-word CV stride 导致 bank conflicts | CV stride 从 8 改 9 / pad CV rows |
| L1 dependency stall 60.5% | 加载和动态消息数组可能拖慢指令发射 / memory and dynamic indexing | 固定 7 轮展开 / unroll seven rounds |
| 每次 1 MiB 更新末尾 31 叶走 CPU 标量 | kernel 改快后主机残留工作明显 / host tail becomes important | 同一 kernel 计算尾叶 / fuse partial tail without another launch |

word loader、round unroll 和 CV padding 是组合实验，没有分别消融，不能把全部收益单独归因给
其中某项。编译器对该组合报告 0-byte stack frame、0 spill stores/loads。
Word loading, unrolling, and padding are a combined experiment, not isolated ablations; the compiler
reports zero stack-frame bytes and no spill stores/loads for that combination.

## 内核证据 / Kernel evidence

输入约 1 MiB；前两个变体处理 992 叶，最终 tail 变体处理 1023 叶，工作量稍多而非更少。
Approximately 1 MiB input: baseline/word variants process 992 leaves; the final tail variant processes
1023, slightly more work rather than less.

| Variant / 变体 | Kernel µs | Shared bytes | Global sector waste / 浪费 | Decision / 决策 |
|---|---:|---:|---:|---|
| Fused baseline / 已融合基线 | 161.57 | 1024 | 97% | 基线 / baseline |
| Word + unroll + padding | 45.73 | 1152 | 87% | 保留 / retain |
| Shared transpose / 共享转置 | 59.26 | 34944 | 不再产生该警告 / no warning | 拒绝 / reject |
| Word + fused tail / 尾叶融合 | 45.31 | 1152 | 87% | 保留 / retain |

**共享转置确实修复了合并访问，却让完整计算更慢。** 它把 32 KiB 输入先搬到带 padding 的共享
矩阵；额外 staging 和共享内存占用不划算。不能为了让 profiler 指标好看而接受实测退化。
**Shared transpose improved coalescing but slowed complete execution.** Its padded 32 KiB staging
cost was not worthwhile. A better counter value is not a reason to accept an observed regression.

`coalesced` 只保存在 `.cache/perf/gpu/coalesced/`，没有进入生产代码。
The rejected variant remains only in the cache, not production.

## 完整传输边界证据 / Full-transfer-boundary evidence

随机输入（固定种子17，LCG），64 MiB 总量，一轮预热后五轮中位数。单工作线程，官方 CPU
为独立正确性判据；计时包括 hasher 创建、update 和 finish，不含 CUDA 初始化、输入生成、磁盘IO。
Deterministic random input (seed 17), 64 MiB, one warmup and median of five timed rounds. Single worker;
independent official CPU oracle. Times include hasher creation/update/finish, excluding CUDA startup,
input generation, and disk IO.

### 第一组 / First batch

| Update block / 更新块 | SIMD CPU ms | Fused baseline ms | Word variant ms |
|---|---:|---:|---:|
| 1 MiB | 15.767 | 25.494 | 20.927 |
| 16 MiB | 15.102 | 8.245 | 7.976 |

CPU 列对应 word 同组 CPU 轮次；基线同组 CPU 分别 15.691 / 15.621 ms。
CPU column is the word-executable CPU run; baseline-executable CPU medians were 15.691 / 15.621 ms.

### 第二组 / Second batch

| Update block / 更新块 | Word ms | Shared transpose ms | Final tail ms |
|---|---:|---:|---:|
| 1 MiB | 20.117 | 20.820 | 15.942 |
| 16 MiB | 7.023 | 8.392 | 7.078 |

第二组未重新计时 CPU，不能将其与第一组当作严格配对的 CPU/GPU 比值。
No CPU measurement was repeated in batch two; do not treat cross-batch ratios as strict paired tests.

**关键推论：文件大小不是唯一变量，实际 update block 是决定因素。** 同一 64 MiB 输入，
1 MiB 更新没有可信 GPU 优势，16 MiB 更新则在完整边界测量中明显胜 CPU。
自动路由必须用实际块容量和预算校准；这些数据不证明 20-worker 并发时仍有同样收益。
**File size is not enough: actual update block size is decisive.** The same input has no convincing
GPU advantage at 1 MiB updates but wins at 16 MiB. Auto routing must calibrate actual capacity/budget;
these data do not prove the same benefit with twenty workers.

原始数据 / Raw data:
- `.cache/perf/gpu/word-comparison.csv`：前20行为baseline，后20行为word；每10行为CPU/GPU。
  First 20 rows baseline executable, last 20 word; ten CPU then ten CUDA rows in each.
- `.cache/perf/gpu/variants.csv`：包含 variant/block 分隔行与五轮记录。
  Includes explicit variant/block separators and five observations each.
- `.cache/perf/gpu/{baseline,word,coalesced,tail}-ncu.ncu-rep`
- `.cache/perf/gpu/{baseline,word,coalesced,tail}-details.txt`

## Nsight Systems 失败边界 / Nsight Systems failure boundary

`nsys 2026.1.3 --trace=cuda --sample=none --cpuctxsw=none` 启动成功，但导入活动时内部异常：
`CudaActivityDevice::GetNumTpcs(): Data member NumTpcs was not initialized`。
生成的 SQLite 没有 CUDA trace/kernel/memory 数据，因此**不把空报告当作没有传输**。
Nsight Systems launched but failed internally while importing activity with the error above.
The generated SQLite has no CUDA data; **an empty report is not evidence of absent transfers**.

错误和空统计保存在 `word-nsys.txt`、`word-nsys-stats.txt`；没有尝试改驱动或系统权限。
VTune/WPT 没有用于本 GPU 内核问题；当前 ncu 和真实完成边界计时已能指导改动。
Errors and empty stats are preserved; no driver/policy changes attempted. VTune/WPT were not needed
for this bounded kernel investigation. Transfer-stage attribution remains unmeasured by nsys.

## 正确性与资源契约 / Correctness and resource contracts

- 只对全局32叶边界对齐的完整子树归并，末 block 的剩余叶按顺序回传。
  Reduce only globally aligned subtrees; return partial final-block leaves in order.
- CUDA 设备输入由 cudaMalloc 对齐且叶偏移为1024；32-bit小端读取不用于任意主机地址。
  Device allocations and 1024-byte offsets are aligned; word loading is not applied to arbitrary host pointers.
- 所有 `__syncthreads` 路径由 blockIdx/count 控制、块内一致，末 block 无未初始化CV读取。
  Barrier branches are block-uniform; partial blocks never read uninitialized CVs.
- CV 容量为 `min(capacity/1024, capacity/32768 + 31)`，仍不超过旧预算保留的每叶CV空间。
  CV capacity uses that bound and never exceeds historical per-leaf scratch budgeting.
- 最终叶仍保留 Output/ROOT 语义，重复finish及之后继续update不变；低预算和异常回退接口不变。
  ROOT/final-leaf semantics, repeatable finish, subsequent update, and fallback interfaces remain unchanged.
- 独立链接最终变体执行 `SAME_REQUIRE_CUDA=1 compute_tests.exe` 已通过官方向量、随机增量、
  32 KiB 边界和最低显存预算；没有静默跳过 CUDA。
  Real-device compute tests passed official vectors, streaming, boundaries, and minimum budget without skipping.

## 复现 / Reproduce

可审查驱动源码：`tools/gpu_profile_driver.cpp`。Developer Command Prompt 中按生产方式编译 CUDA
对象，链接该驱动、`src/compute_cpu.cpp`、官方 blake3.lib 和 cudart.lib。
Reviewable driver: `tools/gpu_profile_driver.cpp`. Compile CUDA as production, link driver, CPU adapter,
official blake3.lib, and cudart.lib in a Developer Command Prompt.

```text
nvcc -std=c++20 -O2 -Xcompiler=/MD -Xptxas=-v -Iinclude -Isrc -c src/compute_cuda.cu -o candidate.obj
candidate.exe gpu 67108864 1048576 5
candidate.exe cpu 67108864 1048576 5
candidate.exe gpu 67108864 16777216 5
candidate.exe cpu 67108864 16777216 5
ncu --set full --launch-count 1 --export candidate-ncu candidate.exe gpu 1048576 1048576 1
ncu --import candidate-ncu.ncu-rep --page details
```

## 外部依据与仍待验证 / External basis and remaining uncertainty

- [NVIDIA Nsight Compute Profiling Guide](https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html)：
  sector fragmentation、bank conflict 与 replay 指标的解释；按其含义分析而非把建议百分比当保证。
  Explains sectors, bank conflicts, and replay metrics; estimated speedups are not guarantees.
- [NVIDIA CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)：
  传输批量化和实际端到端测量优先；pinned memory 会消耗稀缺资源，未未经预算就增加。
  Supports batching and end-to-end measurement; pinned memory consumes scarce resources and was not added blindly.
- [BLAKE3 原始设计研究 / original design](https://github.com/BLAKE3-team/BLAKE3-specs)：
  子树并行必须保持 chunk counter、树形及ROOT；本任务不是重新设计哈希算法。
  Subtree parallelism preserves counters, tree shape, and ROOT, not a changed hash algorithm.

尚存：global sector浪费、少量更新下occupancy低、多worker竞争、冷文件IO、CUDA启动摊销。
FP32/FP64 roofline 对以整数ARX运算为主的BLAKE3不是正确性能目标，不能通过无用浮点运算提高
所谓“算术强度”。下一步应验证真实并发和自动路由，而不是重复调文件大小阈值。
Remaining issues: strided loads, underfilled small launches, multiworker contention, cold IO, startup
amortization. Floating-point rooflines are not the objective for integer ARX BLAKE3; adding useless
floating-point work does not improve useful arithmetic intensity. Next validate concurrency and routing.

## 后续：稳定输入注册实验 / Follow-up: stable input registration

真实文件读取路径可能与反复读取已驻留输入的微基准不同；“pageable H2D staging 是主因”仍是
待验证假设，不因为 kernel 优化成功就成立。增加可选 `Compute::prepare_input(span<byte>)`，
默认无操作，CUDA 对调用方已有的稳定缓冲执行一次 `cudaHostRegisterDefault`；不增加额外
输入缓冲、不在每次 update 注册。用于使真实 IO 填充的同一缓冲持续适合传输。
Real file reads may differ from repeated resident-input microbenchmarks. Pageable H2D staging is a
hypothesis, not established causality. Optional `Compute::prepare_input(span<byte>)` defaults to no-op;
CUDA registers the caller's existing stable buffer once, without another input buffer or per-update
registration. Subsequent real IO refills use that same registered storage.

注册所有权在共享 Device 内；stream 完成后才 unregister。调用方存储必须存活至 Compute 和
所有 Hasher 析构；同范围重复调用幂等，异范围或注册失败抛 ComputeError，供 CPU 回退。
Registration lives in shared Device ownership and is removed after stream synchronization. Caller
storage must outlive Compute and every Hasher. Identical ranges are idempotent; different ranges or
registration failure throw ComputeError for caller-managed CPU fallback.

依据：[CUDA 12.8 Runtime memory API](https://docs.nvidia.com/cuda/archive/12.8.1/cuda-runtime-api/group__CUDART__MEMORY.html)
说明注册范围进入 CUDA 的跟踪机制以加速 memcpy，必须用相同 base 调用 cudaHostUnregister。
某些 HMM/ATS 系统只填充页面而不实际page-lock，不能跨平台假定相同收益。
The Runtime API documents transfer tracking and same-base unregistration. Some HMM/ATS systems
populate pages rather than pinning them; performance benefits are not assumed identical across platforms.
