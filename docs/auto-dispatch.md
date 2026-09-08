# 统一线程在线分流 / Unified-worker online dispatch

本文件描述当前生产契约。设计与研究依据见 [统一线程设计](unified-worker-design.md)。旧 N+1 服务、中央最早完成时间（earliest finish time, EFT）模型及相关性能报告是历史版本，不证明本实现的性能。

This is the current production contract. The former N+1 service, centralized EFT model and its benchmarks are historical, not evidence for this implementation.

## 固定线程与队列 / Fixed workers and queue

`workers=N` 创建恰好 N 个内容工作线程，一个有界先入先出（first-in, first-out, FIFO）队列。线程取出任务后，在队列锁外使用自己的模型选择 CPU SIMD 或自己的 CUDA 流。模型预测、学习和探索计数均为线程私有；中央锁仅用于队列和活动数，不训练模型。完成顺序不要求等于取出顺序，关闭时排空已接纳任务。

Exactly N workers share one bounded FIFO. After dequeue, workers choose their own CPU/CUDA backend outside the queue lock. Prediction, learning and exploration are local; shared locking handles handoff/activity only. Shutdown drains admitted work; completion order need not match dequeue order.

不存在单独 GPU 队列、额外 GPU 工作线程、全局忙闲切换或全局等待时间预测。多个工作线程可在同一设备主上下文（primary context）中使用各自执行流（stream）。实际重叠受 GPU、PCIe、内存和磁盘争用影响，流数量不是 GPU 数量。

There is no dedicated GPU queue/thread or global wait prediction. Worker streams share the device primary context; overlap remains subject to compute/transfer/memory/storage contention.

## 资格与预算 / Eligibility and budgets

CPU 使用上游 BLAKE3 单指令多数据运行时分派（SIMD runtime dispatch），默认块 1 MiB。文件 GPU 资格仅由 `gpu_min_bytes` 控制，默认 16 MiB；零长度文件留 CPU。它不是通用性能交叉点，CPU 块变化不改变资格。

CPU uses upstream BLAKE3 SIMD with a default 1 MiB block. GPU eligibility defaults to 16 MiB independently of CPU block size; empty files stay CPU. This is an eligibility policy, not a universal speed crossover.

令 `B=block_bytes`，`H(b)=2b+b/32+4096`。首先为全部 CPU 线程保留 `N*H(B)`；自动 GPU 的每线程主机配额为 `floor((memory_bytes-N*H(B))/N)`。从 16 MiB 开始逐次减半选择可容纳的 GPU 块，最小 1 KiB；仍不满足则该配置不初始化自动 GPU。显存配额为 `floor(device_memory_bytes/N)`，不把总预算授予每个线程。显式 CUDA 复用 CPU 输入缓冲。预算限制工作缓冲，不是进程总内存硬上限。

CPU reservations precede equal division of remaining host quota. Auto GPU block size halves from 16 MiB down to 1 KiB until its reservation fits. Device quota is total/N. Forced CUDA reuses CPU input buffers. These are working-buffer budgets, not process-memory limits.

## 惰性初始化与失败 / Lazy initialization and failure

只有线程首次选择 GPU 时，才在该线程初始化自己的后端。自动模式首个冷设备初始化用一次性 `cold/initializing/ready` 状态协调；其他线程遇到 initializing 时先执行 CPU，计入 `cold_start_cpu`，不会永久标记自身 GPU 不可用。ready 后各线程私有 GPU 流仍可并发，没有稳态单 GPU 准入门。显式 CUDA 不应用此冷启动协调。不是额外后台初始化服务，也不保留一个额外的大文件候选。其他线程继续执行，纯小文件和缓存命中不要求初始化 GPU。初始化执行至多 64 KiB 的正确性检查，不做合成性能校准或注入合成训练先验；初始化耗时单独逐线程记录，仍属于扫描时间。

Each worker lazily initializes its backend when it first selects GPU. Auto uses a one-time cold/initializing/ready transition: peers run CPU while initializing without marking their GPUs unavailable. After ready, private GPU streams remain concurrent; there is no steady-state admission gate. Forced CUDA bypasses this policy. There is no extra startup service or retained candidate. Startup validates at most 64 KiB without synthetic timing calibration/seeding; setup is measured per worker and remains scan work.

CUDA 不可用或可恢复的设备计算错误使该线程回到 CPU，不停掉其他线程的 GPU。非设备初始化异常（如内存分配错误）通过任务传播，正常使扫描失败并保留原错误。计算失败必须从文件开头完整重读重算，不拼接摘要中间态。正确性检查摘要不一致是错误，不当作性能不佳静默接受。极小降级缓冲可能只覆盖后端主机叶块路径，不能仅凭检查通过声称 GPU 核函数已执行。

Unavailable CUDA or recoverable compute errors fall back on that worker only. Other initialization exceptions propagate unchanged and fail the scan. Compute failure rereads/recomputes the whole file on CPU. A correctness mismatch is an error, not a slow-device observation. Tiny fallback buffers may exercise host leaves only and do not prove a GPU kernel ran.

## 本地模型与选择 / Local model and selection

每线程有 CPU/GPU 各 32 个四倍大小区间，保存每字节服务时间和绝对残差的指数加权移动平均（exponentially weighted moving average, EWMA），alpha=1/8。只学习真实成功且无回退的哈希任务；不把比较和失败样本混入。预测仅使用同线程、同后端、同大小区间证据。

Each worker owns 32 factor-four bands per backend with per-byte cost/error EWMA, alpha=1/8. Only successful unretried hashes train normal cost. Evidence remains local to worker/backend/band.

两个后端均已知时选 GPU 当且仅当：

```text
GPU_service + GPU_error < 0.95 * CPU_service - CPU_error
```

这是服务成本比较，不含虚构的独立 GPU 队列等待；5% 滞回（hysteresis）和残差是工程保护，不是置信区间或全局最优保证。任务已经由该线程领取，不会迁移进行中的摘要。

This compares service costs after the task is claimed, not global finish times. Residuals and a 5% margin are safeguards, not confidence bounds or optimality guarantees. In-progress hashes are never migrated.

未知区间先用静态偏好：合格文件达到 64 MiB 选 GPU，否则 CPU。启用 PGO 时，每线程每区间对两个后端各最多两次初始探索；已有一端证据时尝试未知端。每 64 次合格到达提供一次周期探索，CPU/GPU 交替；GPU 不可用仍退 CPU。探索计数不是成功样本数。不同线程的机会不合并，样本碎片化与并发探索可能增加成本。

Unknown bands use a 64 MiB static preference boundary. PGO allows up to two initial explorations per backend per worker-band and alternating periodic exploration every 64 eligible arrivals. Unavailable GPU falls back. Opportunities are local and are not successful observations; fragmentation/concurrent exploration have costs.

## 开关、剖析和遥测 / Switches, profiling and telemetry

`--no-pgo` 关闭运行时剖析引导优化（profile-guided optimization, PGO）的采样、学习和模型选择；静态资格/64 MiB 偏好、设备正确性检查与 CPU 回退仍有效。它不是 `--cpu`，不改变编译器 PGO。`--no-telemetry` 只关闭独立遥测库入库，`--summary` 只控制本轮展示；三者独立。

No-pgo disables runtime sampling/learning/model selection, not static routing or correctness/fallback. It is not forced CPU or compiler PGO. No-telemetry controls persistence and summary controls current-run presentation independently.

持续剖析（continuous profiling）复用任务墙钟时间，不是系统调用栈采样。合格哈希逐任务采样，较小文件每线程每 64 项一次；不逐块增加时钟或遥测写入。耗时包含 I/O 与设备争用，不是 CPU 时间或 GPU 核函数时间。不同采样率与后端选择使观测分布存在选择偏差。

Task-level profiling reuses wall clocks, not OS stack sampling. Eligible hashes are sampled; smaller files are sampled every 64 jobs per worker. No per-block instrumentation is added. Times include I/O/contention and sampling/placement introduce selection bias.

## Summary 口径 / Summary semantics

| 字段 / Field | 含义 / Meaning |
|---|---|
| `scheduler=worker-local`, `single_queue=1`, `worker_count` | 固定线程私有决策与单队列 / Fixed local decision ownership |
| `pgo_samples`, `pgo_cpu_samples`, `pgo_gpu_samples` | 全线程成功样本累计 / Summed local successful observations |
| `pgo_cpu_known_bands`, `pgo_gpu_known_bands` | 已知线程×区间单元，每后端上限 N×32 / Worker-band coverage, not one global model |
| `pgo_predicted_samples` | 可验证预测累计 / Predictions checked across workers |
| `worker.<id>.local_error_ewma_ms` | 本地误差 EWMA，不是全局误差 / Local error only |
| `worker.<id>.cpu_hashes`, `.gpu_hashes` | 后端尝试，包含回退尝试 / Attempts including retries |
| `worker.<id>.cold_start_cpu` | 首次设备初始化期间暂走 CPU 的选择数，不是永久不可用 / Temporary CPU selections during cold startup |
| `worker.<id>.setup_ms`, `.gpu_init_failures`, `.fallbacks` | 逐线程初始化与失败 / Local startup/failure |
| `pgo_latency_p50_bucket_us`, `pgo_latency_p95_bucket_us`, `pgo_residual_p95_bucket_us` | 合并计数的近似分位桶 / Approximate pooled histogram percentiles |

直方图共 32 个 `floor(log2(us))` 桶，两端饱和。空闲后只聚合计数与桶，不把各线程 EWMA 当作一个全局训练模型。逐线程决策原因、后端字节和并发观测也显示/保存。并发计数覆盖“选择 GPU 的任务”生命周期，包含文件 I/O，不是内核忙碌数；`contended_samples` 仅标记选择时已观察到其他 GPU 任务的样本，不是整个服务区间重叠的完整检测。遥测导出 `worker.<id>.model.cpu/gpu.<band>` 的全部 64 个区间，见 [遥测文档](telemetry.md)。不自动从旧库加载模型。

Thirty-two saturated log2-microsecond buckets are pooled after idle; local EWMAs remain distinct. Concurrency counts GPU-selected task lifetimes including I/O, not active kernels. contended_samples checks overlap only at selection, not across the full interval. Worker decisions/bytes/concurrency and all model bands are retained in telemetry, never auto-loaded into future runs.

未知或已移除配置字段静默忽略；支持字段仍校验类型和预算，TOML 仍须合法。**不承诺零开销或普遍加速。** 应分别验证摘要一致性、资源上限、线程隔离和真实负载性能；历史中央模型报告不是当前架构的通过证明。

Unknown/removed settings are ignored; supported types/budgets and TOML validity remain checked. No zero-overhead/universal-speedup claim. Historical centralized-model reports do not validate this architecture.
