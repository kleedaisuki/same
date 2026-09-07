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
8. 用CPU最快、混合最慢样本保守估计待处理批次收益，至少覆盖两倍设置与探测成本才保留GPU。
9. 最多启用一路GPU；首个最大待处理文件指定GPU，其余共享普通队列。每轮最多探索一次。

Auto starts CPU-only and buffers bounded metadata/handles, not contents. Only unprocessed eligible
work can trigger a probe. Below the 4GiB exploration threshold there is no CUDA initialization.
The coordinator drains all jobs before changing a backend. Serial evidence must pass a 20% margin;
multi-file evidence additionally uses the real worker pool and a bounded dynamic CPU/mixed probe.
Fastest CPU and slowest mixed samples feed a conservative amortization estimate. Only one GPU lane
is retained, and the largest first candidate is routed to it explicitly. Explicit CPU/CUDA semantics
remain separate from automatic profitability selection.

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
