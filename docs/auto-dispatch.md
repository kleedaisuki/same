# 惰性、可摊销的 CPU/GPU 分派 / Lazy, amortized CPU/GPU dispatch

## 为什么不是固定文件大小阈值 / Why a size threshold is insufficient

64 MiB 输入在16 MiB分块时单GPU可胜CPU，但20个GPU实例竞争反而显著变慢。
热内存基准的胜利也不能覆盖CUDA初始化、校准、文件读取和pageable输入复制成本。
开发中曾实测512MiB短扫描被无条件启动校准拖慢；该方案已被替换，而不是作为默认留下。

A large update block can make one GPU profitable, while many GPU instances reverse the advantage.
Warm-memory wins exclude startup and file I/O. An early unconditional calibration prototype caused
measured short-scan regressions and was replaced, not shipped as the default policy.

## 最终流程 / Final flow

1. `backend="cpu"` 不探测CUDA；`backend="cuda"` 保留显式选择、大小下界及失败后的CPU重试。
2. `backend="auto"` 启动时只创建CPU后端，摘要缓存命中不触发GPU探测。
3. 暂存达到 `max(gpu_min_bytes,block_bytes)` 的未处理文件，只有记录与打开句柄，不预读内容；
   数量最多 `queue_capacity`。小文件照常在CPU流水线处理。
4. 暂存逻辑字节达到 `gpu_probe_bytes`（默认4GiB）才允许一次有界探索。
   容量先满或EOF不足直接CPU处理，不付CUDA初始化费用；已处理字节不能拿来摊销未来工作。
5. 排空在途哈希并等待线程真正空闲，再建立第一GPU后端，注册其稳定输入缓冲区。
6. 实际块/显存预算下，一块与八块的全部三组配对样本都须GPU至少快20%，完整校验摘要。
7. 多文件还在真实工作线程上比较全CPU与1GPU+其余CPU，使用动态任务领取；
   任务数不超过实际待处理文件数，最多四倍参与线程数。单文件复用串行证据。
8. 用CPU最快、混合最慢样本保守估计待处理批次收益；极值之间仍须至少快20%，
   且收益至少覆盖两倍设置与探测成本才保留GPU。单线程也使用串行样本极值而非中位数。
9. 最多启用一路GPU；暂存批次先按大小降序提交，首个最大文件固定GPU以兑现探测，
   此后所有合格哈希持续携带GPU提示。
   GPU优先领取合格哈希，CPU优先普通任务、空闲时窃取合格哈希；不把整批固定到GPU。
   每轮最多探索一次。

Auto starts CPU-only and buffers bounded metadata/handles, not contents. Only unprocessed eligible
work can trigger a probe. Below the 4GiB exploration threshold there is no CUDA initialization.
The coordinator drains all jobs before changing a backend. Serial evidence must pass a 20% margin;
multi-file evidence additionally uses the real worker pool and a bounded dynamic CPU/mixed probe.
Fastest CPU and slowest mixed samples must retain a 20% margin before feeding the amortization
estimate; serial-only amortization also uses extrema rather than medians. One GPU lane is retained.
Buffered candidates are submitted largest-first, with the first pinned to the GPU to realize the
probe's first-job guarantee. Every later eligible hash carries the same stealable hint.
The GPU prioritizes eligible hashes; CPUs prefer ordinary work and steal eligible hashes when idle.
Explicit CPU/CUDA semantics remain separate from automatic profitability selection.

## 持续调度与背压 / Continuous scheduling and backpressure

`Resources::submit_hash` 是可窃取的资格提示（work stealing），不是设备绑定。
原有 `submit(operation,true)` 保持固定首通道语义，供校准启动门使用；普通提交不变。
固定任务、合格哈希、普通任务三类队列的**总和**受 `queue_capacity` 约束，完成环还限制
在途及未收取结果数。GPU失败后原通道原地回退CPU，队列仍可排空，不丢弃任务。

The hash hint is stealable, not device binding. The existing pinned submission contract remains for
probe gates. All three queue classes share one admission bound; the completion ring additionally
bounds running and uncollected results. A failed GPU lane becomes CPU in place and still drains work.
The pure queue-selection policy is exhaustively tested for all 16 lane/queue-presence combinations,
without requiring CUDA or timing-dependent profitability decisions.

该策略不承诺每个大文件都走GPU，也不在任务开始后迁移Hasher。它避免小文件抢占合格
哈希的队列优先级，但不能消除已经运行中的任务、冷IO和不等长尾部带来的偏差。
It does not promise GPU execution for every large file or migrate a live hasher. Queue priority does
not eliminate already-running work, cold I/O or unequal-length tails.

## 配置 / Configuration

```toml
backend = "auto"
gpu_min_bytes = 16777216
# 尚未处理批次的探测门槛，不是RAM分配量 / Pending logical bytes, not RAM allocation
gpu_probe_bytes = 4294967296
```

`gpu_probe_bytes=0` 仅绕过工作量门槛，不绕过性能与摊销检查；用于显式对照实验。
4GiB是限制探索风险的默认值，不是经过证明的硬件性能交叉点。过小队列可令许多小批次
始终走CPU，这保证有界资源，但可能放弃潜在GPU机会。

Zero disables only the work-volume gate, not profitability/amortization checks. The default is a
risk-control policy, not a proven crossover. Small queue capacities may forgo GPU opportunities
because bounded batches never reach the exploration volume.

## 成本与资源 / Cost and resources

输入通过 `cudaHostRegister` 一次注册既有工作缓冲区，无额外同大小staging分配；Compute/
Hasher最后一个所有者同步后注销。调用方必须保证该缓冲区覆盖注册生命周期，Resources
的字段析构顺序满足此约束。锁页内存是既有预算的一部分，但不可被操作系统换出。

The stable worker input is registered once, not copied into a second staging allocation. Its lifetime
must cover all sharing Compute/Hasher owners; Resources destroys compute before its input vectors.
Registered pages belong to the existing buffer budget but become nonpageable.

两秒软预算覆盖CUDA设置及后续探测，完整操作返回后检查；无法强行中断驱动或文件系统调用。
计算错误选择CPU，错误摘要直接失败。首次探索本身仍可能不划算；不存在未知设备上的免费预知。
收益估计采用等大小缩放，不能证明大小不均、冷IO、温度/功耗变化下的性能保证。

The two-second soft budget stops subsequent work after complete operations; it cannot interrupt a
driver. Compute failures select CPU, digest mismatches fail closed. Exploration itself can still cost
more than it saves. Equal-shape scaling is not a guarantee for heterogeneous sizes, cold I/O or changing
thermal/power conditions. No performance state is persisted across runs.

## 统计 / Metrics

`gpu_setup_ms` 是全部设置与探测时间，包含在Scan/Elapsed而非Initialize中。
`calibration_ms` 是串行校准部分；`probe_mixed_cpu_ms`、`probe_mixed_gpu_ms` 是并发采样证据；
`expected_gpu_saving_ms` 是模型估计而非实际节省。`cpu_hashes`/`gpu_hashes` 是后端尝试数，
不含探测，CUDA后端仍可在CPU处理边界；不能据此推断每个字节都在GPU运算。
自动模式尚未探测时 `auto_backend=cpu-unprobed` 且 `gpu_setup_ms=0`；
显式CPU/CUDA模式显示 `explicit`。

Setup is included in scan time; serial calibration and mixed samples are separate metrics.
Expected savings are a model estimate. CPU/CUDA attempt counts exclude probing and are backend
counts, not proof that every byte executed on a physical GPU.

## 工具与证据 / Tools and evidence

已探查 ncu2025.1.1、nsys2026.1.3、VTune2025.3、WPR10.0.26100、Python3.14、Lean4.33.1。
实际使用ncu硬件计数器、Python基准及CUDA内存/竞争检查；nsys因内部NumTpcs异常未产生
有效跟踪。WPR检查时无活动记录，没有改变驱动权限或系统跟踪。未声称使用未执行的工具。

See [GPU profiling](gpu-profiling.md), [dispatch benchmarks](dispatch-benchmark.md),
[file benchmark protocol](file-dispatch-benchmark.md), and [policy research](dispatch-policy-design.md).

## 设计依据 / Design rationale

- [NVIDIA CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)：
  决策包含主机传输与同步，复用有界注册缓冲；不是单独比较kernel时间。
  Include transfer/synchronization costs and reuse bounded registered buffers, not kernel-only timing.
- [BLAKE3 1.8.2 C implementation](https://github.com/BLAKE3-team/BLAKE3/tree/1.8.2/c)：
  CPU沿用上游运行时SIMD分派，不强制本机ISA、不重复实现摘要算法。
  Retain upstream runtime SIMD dispatch rather than forcing a host-specific ISA or duplicating hashing.
- [StarPU, CCPE 2011](https://doi.org/10.1002/cpe.1631) 与
  [StarPU features](https://starpu.gitlabpages.inria.fr/features.html)：
  异构任务调度同时考虑性能、数据位置与可用资源；这里只借鉴任务亲和性与动态领取，
  不引入完整运行时或在线学习模型。后者需要真实任务成本观测与独立实验才能采用。
  Borrow affinity and dynamic task acquisition, not an entire runtime or unvalidated online model.
- [Gonthier et al., JPDC 2025](https://doi.org/10.1016/j.jpdc.2025.105170)：
  数据局部性与内存受限调度是值得跟进的方向，但其线性代数任务具有复用结构，
  不能直接外推到一次性流式文件哈希；本轮维持有界缓冲和简单资格队列。
  Memory-constrained locality-aware scheduling is relevant future work, but reusable linear-algebra
  task data differs from one-pass streaming hashes; retain bounded buffers and a simple hint queue.
