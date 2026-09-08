# 在线剖析引导分流 / Online profile-guided dispatch

本文件说明当前运行时契约。旧版固定偏好与累计 4 GiB 门槛均不是本模型。
研究依据见[在线模型设计](online-routing-design.md)，验收方案见
[实验计划](online-routing-experiment-plan.md)。历史报告只证明各自版本的行为。
This describes the runtime contract, not the former fixed-preference or aggregate-volume policy.
Historical measurements do not establish this model's performance.

## 资格、缓冲与后端 / Eligibility, buffers and backends

CPU 使用上游 BLAKE3 单指令多数据运行时分派（SIMD runtime dispatch）。CPU 块默认
1 MiB；自动 GPU 资格下界仅为 `gpu_min_bytes`，默认 16 MiB，与 CPU 块无关。
零长度文件保持 CPU；门槛设为零不代表每项任务都应该去 GPU。
CPU uses upstream SIMD. Its default block is 1 MiB; the independent eligibility floor defaults to
16 MiB. Empty files stay CPU; removing the floor does not force GPU placement.

GPU 服务优先采用独立 16 MiB 更新块；剩余预算不足时使用可容纳的降级块，仍不足则仅
停用自动 GPU。CPU 工作者预算与额外 GPU 缓冲均需满足既有资源限制，预算不是进程内存硬上限。
The GPU service prefers a separate 16 MiB block, then tries `min(block_bytes, 16 MiB)`, disabling only
automatic GPU if no supported block fits. Existing valid CPU configurations remain valid; budgets are
not process-memory hard limits.

- 保留全部 N 路 CPU，加最多一路 GPU，不把 CPU 通道改造成 GPU 通道。
  Keep N CPU lanes plus at most one independent GPU service.
- 小于资格下界的文件与比较任务永不进入自动 GPU 队列。
  Ineligible hashes and comparison tasks never enter automatic GPU execution.
- 显式 `backend="cpu"` 不初始化 CUDA；显式 `backend="cuda"` 保留原有强制选择契约。
  Explicit CPU avoids CUDA; explicit CUDA preserves its existing selection contract.

## 冷启动与设备验证 / Startup and device validation

首个未缓存的合格文件触发后台初始化；CPU 继续工作。初始化期间最多保留一个最大候选
句柄，不预读其内容；结束或异常展开时等待初始化线程，不让后台访问已销毁状态。
纯小文件或全缓存扫描不初始化 GPU。
The first uncached eligible file triggers background startup while CPUs continue. At most one largest
candidate handle is retained without prefetch; completion/unwinding joins startup. Small-only and
cache-only scans do not initialize GPU.

生产扫描无论启用还是禁用 PGO，启动时均不运行合成性能校准，只执行
`min(GPU_block_bytes, 64 KiB)` 输入的最小后端正确性检查。不会向在线模型播种合成性能
先验；设备正确性通过不等于已知该设备的任务速度。启动成本仍计入扫描和总耗时。
Both PGO modes use only a minimal backend correctness check with `min(GPU_block_bytes, 64 KiB)` input.
Production startup does not run synthetic performance calibration or seed synthetic model priors.
A valid device does not imply known task performance. Setup remains charged to scan and elapsed time.

独立校准辅助函数仍用于测试、基准和兼容用途，并保留完整局部证据；它不属于生产扫描
启动路径。因此旧两秒软预算与块级/长流探测不再解释当前运行时行为。
The separate calibration helper retains complete partial evidence for tests, benchmarks and compatibility,
but is not on production scan startup. Its former two-second budget and shape probes do not describe
current runtime startup.

## 在线模型 / Online model

每个后端维护 32 个四倍大小区间，覆盖 uint64 大小范围。各区间保存每字节服务成本与
绝对残差的指数加权移动平均（Exponentially Weighted Moving Average, EWMA），系数 1/8。
只使用同后端同区间证据，不向未知区间无条件外推；生产模型仅学习真实成功文件样本。
Each backend has 32 factor-four bands with per-byte cost/residual EWMA, alpha 1/8. Predictions remain
within the same backend and band; production learning uses only real successful file observations.

启用和禁用模式都使用相同初始静态偏好：合格文件达到 64 MiB 时 GPU 优先，其余 CPU
优先。启用模式在取得统计后用在线预测改进选择，未知成本不是零成本。
Both modes start with the same static preference: eligible files at least 64 MiB prefer GPU, smaller
eligible files prefer CPU. Enabled mode refines placement as observations arrive; unknown is not zero.

预计完成时间（Earliest Finish Time, EFT）结合可用等待、预计服务时间与误差余量：

`finish(d, s) = estimated_wait(d) + predicted_service(d, s) + uncertainty_margin(d, s)`

CPU 等待来自最早可接手的工作线程，不是所有 CPU 工作量的串行和。预测误差与滞回
（hysteresis）抑制噪声，但不是统计置信区间。未知区间需要静态后备与受限探索，
不能把未知当成零成本。设备竞争共享磁盘与内存，因此模型不保证调度最优。
CPU waits concern the earliest available lane, not the sum across lanes. Residual margins and hysteresis
are engineering safeguards, not confidence intervals. Unknown bands require bounded exploration or
static fallback, never fictional zero costs. Shared I/O and memory contention limit optimality claims.

未知区间最初最多允许两次 GPU 探索选择，要求 GPU 空闲且 CPU 全忙或初始 GPU 静态偏好。
每区间每 64 次合格任务到达发出一次探索机会，CPU/GPU 交替，各后端每 128 次一次；
仅在目标后端空闲时采用，否则按正常模型调度。机会绑定任务，队尾回收可能使机会不被
执行，因此不保证每周期获得样本。`pgo_exploration_jobs` 计数实际探索选择，不是成功样本。
Unknown bands allow at most two initial idle-GPU explorations when CPU is saturated or initial static
preference favors GPU. Every 64 eligible arrivals per band offers alternating CPU/GPU exploration
(each backend once per 128). Opportunities are best-effort and require a free target; tail reclamation
may consume an opportunity without a sample. The counter measures actual exploration selections,
not successful observations.

预测过期但设备仍忙时，空闲对端可以回收任务；仅在有合格排队任务时使用预测期限唤醒。
关闭线程池必须排空已接受任务，GPU 故障不能导致 CPU 提前退出。
Overdue predictions permit idle-peer reclamation, with deadline wakeups only for eligible queued work.
Shutdown drains admitted tasks even if the GPU retires.

## 关闭分析器 / Disable the analyzer

配置 `pgo=true` 默认开启运行时剖析引导优化（profile-guided optimization, PGO）。
命令行 `--no-pgo` 覆盖为 false，仅适用于扫描。它关闭任务采样、统计学习和
模型决策，但保留最小设备正确性检查、CUDA 错误回退以及既有汇总计时。
Runtime PGO defaults on. Scan-only `--no-pgo` overrides configuration and disables
sampling, learning and model decisions, not minimal device checks, error fallback or existing summary clocks.

禁用后使用静态策略：不合格文件仅 CPU；合格文件小于 64 MiB 时 CPU 优先，达到 64 MiB
时 GPU 优先；CPU 全忙时 GPU 可接手，GPU 忙时 CPU 可接手。它不等于 `--cpu`，也不改变
编译器 PGO。`--summary` 仅控制是否显示统计，不改变分析器是否运行。
Disabled mode uses static eligibility and a 64 MiB preference boundary with bidirectional busy-device
assistance. It is neither forced CPU nor compiler PGO. Summary visibility does not enable or disable learning.

## 持续剖析与汇总 / Continuous profiling and summary

持续剖析（continuous profiling）是任务级、有界、进程内聚合，不是操作系统调用栈采样器。
复用已有哈希任务墙钟计时与完成锁，不逐块计时、不逐文件记录路径、不写入摘要数据库。
正常成功任务才训练后端模型，失败重试与比较不混入正常样本。
合格文件逐任务采样，较小文件每工作线程每 64 项采样一次；禁用时不产生模型样本。
Task-level profiling reuses job wall clocks and completion synchronization. It is not an OS stack sampler,
per-block timer, per-file path log or database trace. Failures/retries and comparisons do not train normal
hash costs. Eligible files are sampled per job; smaller files are sampled once per 64 jobs per worker.
Disabled profiling produces no model observations. Aggregation remains bounded in memory.

`--summary` 展示开关状态、后端样本数、已知区间、预测残差与延迟分布。分布采用有界
直方图（histogram），桶值不是精确分位数；所有任务墙钟时间包含 I/O 等待，不是 CPU 时间
或 GPU 核函数时间。没有真实观测不等于零耗时，没有模型样本不等于没有执行哈希。
Summary exposes enabled state, backend observations, known bands, residuals and bounded latency histograms.
Bucket percentiles are approximate; job durations include I/O rather than measuring CPU or kernel time.
No observations are not zero cost, and zero model samples do not imply zero hashing.

| 汇总字段 / Summary field | 解释 / Meaning |
|---|---|
| `calibration_stop` | 设备检查后为 `not-run-device-checked`；未运行合成性能校准 / Device validated, synthetic performance calibration not run |
| `pgo_enabled` | 运行时分析器开关 / Runtime analyzer enabled |
| `pgo_exploration_jobs` | 实际探索选择数，不等于成功样本 / Actual exploration selections, not successful samples |
| `pgo_samples`, `pgo_cpu_samples`, `pgo_gpu_samples` | 成功任务观测，不含校准 / Successful task observations, excluding calibration |
| `pgo_cpu_known_bands`, `pgo_gpu_known_bands` | 真实文件观测覆盖区间 / Bands covered by real file observations |
| `pgo_predicted_samples`, `pgo_mae_ms` | 可检验预测数、绝对误差 EWMA；未知显示 unknown / Checked predictions and absolute-error EWMA; unknown is explicit |
| `pgo_rejected_samples` | 模型拒绝的不合法观测 / Invalid observations rejected by the model |
| `pgo_latency_p50_bucket_us`, `pgo_latency_p95_bucket_us` | 已采样服务时间的近似分位桶范围 / Approximate sampled-service percentile bucket ranges |
| `pgo_residual_p95_bucket_us` | 预测绝对残差的近似分位桶范围 / Approximate absolute-residual percentile bucket range |

直方图共 32 桶，按 `floor(log2(microseconds))` 聚合，两端饱和。样本分布不是全部文件的
无偏分布：不同大小采样率不同，且后端选择会影响可见样本。
Histograms have 32 logarithmic microsecond buckets with saturated ends. Different sampling rates and
backend selection mean the sample distribution is not an unbiased distribution over every file.

**不承诺字面零开销或整盘加速。** 必须对照启用/禁用和 CPU 模式，报告决策成本、流水线
与整轮时间、输出一致性及缓存条件。完整实验方案见链接，尚未实测的结果不在此宣称通过。
No literal zero-overhead or whole-drive speedup is promised. Controlled ablations must report decision
cost, pipeline/end-to-end time, output equality and cache conditions; unmeasured gates are not passes.

## 兼容与失败 / Compatibility and failure

`gpu_probe_bytes` 继续接受为退役兼容键，不再以累计 4 GiB 决定初始化。
CUDA 不可用则 CPU；GPU 计算错误完整重读并 CPU 重试，不拼接后端部分状态。
失败服务退休，由已预算的 CPU 工作者排空任务。摘要不匹配必须失败，不能接受为有效结果。
The retired aggregate probe key remains accepted. Unavailable CUDA uses CPU; GPU failures retry fully
from the beginning on CPU. A failed service retires rather than becoming an extra unbudgeted CPU lane.
Digest mismatches fail, never silently becoming valid results.

两种 PGO 模式的正确性检查均使用至多 64 KiB 输入；默认 GPU 缓冲足以覆盖真实内核路径。极小的预算降级缓冲可能仅执行该后端的主机叶块路径，不据此声称 GPU 内核已执行。
In both PGO modes, correctness checks use up to 64 KiB; default buffers exercise the device kernel. Tiny budget-fallback buffers can use only the backend host-leaf path, so validation alone does not universally prove a kernel launch.

### 实际选择不等式 / Implemented comparison

两端都有有效预测时，选择 GPU 的条件为：

`GPU_wait + GPU_service + GPU_error < 0.95 * (CPU_wait + CPU_service) - CPU_error`

这是带 5% 滞回的保守优势判定，不是对称的置信区间，也不保证全局最优。等待时间来自
正在运行任务的剩余预测，未构建所有排队任务的完整离线排程；过期预测由可用对端接手。
With both predictions known, this conservative comparison requires a five-percent margin. It is not
 a symmetric confidence interval or global optimality guarantee. Wait estimates concern in-flight
 residuals, not a complete offline schedule of all queued work; overdue predictions yield to the peer.
