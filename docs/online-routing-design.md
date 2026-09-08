# 在线路由研究设计 / Online routing research design

## 问题与目标 / Problem and objective

研究对象是在线到达、处理时间未知且时变、共享 I/O 与有限缓冲的异构并行机调度，
不是静态任务依赖图，也不是提高 GPU 占用率。主要目标是扫描流水线完成时间；
整体耗时、正确性、内存与分析器开销是共同约束。
We study online heterogeneous-machine scheduling with unknown drifting service times,
shared I/O and bounded buffers, not a static DAG or GPU occupancy maximization.
Pipeline makespan is primary; end-to-end time, correctness, memory and analyzer overhead
are joint constraints.

用户提供的 245091 文件扫描中，块级校准 CPU/GPU 为 101.48/16.29 ms，长流无结果，
总 setup 2.43 s，最终没有 GPU 服务。这是需求来源，不是可复现基准；不能把该比值
解释为整轮加速潜力，也不应读取或修改用户目录来制造实验数据。
The user-provided scan motivates partial calibration handling, not a reproducible speedup
claim. Its CPU/GPU block ratio is not an end-to-end counterfactual. Experiments use isolated
fixtures rather than the user's directory or database.

## 证据与适用边界 / Evidence and scope

| 来源 / Source | 借鉴 / Adopt | 不继承 / Do not infer |
|---|---|---|
| [HEFT, IEEE TPDS 2002](https://doi.org/10.1109/71.993206) | 最早完成时间 / earliest finish | 静态 DAG 算法对在线文件的最优性保证 / online optimality |
| [StarPU production features](https://starpu.gitlabpages.inria.fr/features.html) | 运行历史成本模型、动态调度 / history models and dynamic placement | 大任务运行时的开销可忽略于微小文件 / negligible tiny-file cost |
| [Automatic Calibration, HPPC 2009 proceedings](https://www.par.univie.ac.at/workshop/hppc/HPPC09/HPPC09-Preproc.pdf) | 按任务形状保留测量 / shape-specific evidence | 数值内核模型完整描述文件 I/O / full I/O modeling |
| [Google-Wide Profiling, IEEE Micro 2010](https://research.google/pubs/google-wide-profiling-a-continuous-profiling-infrastructure-for-data-centers/) | 稀疏持续采样、有界聚合 / sparse continuous sampling | 数据中心摊销比例等于本项目成本 / transferable overhead percentages |
| [NVIDIA CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html) | 传输和同步成本、合并更新、有界固定页内存 / transfer costs and bounded staging | 16 MiB 是通用最优边界 / universal threshold |
| [Online Tasks with Bandit Feedback, 2024 preprint](https://arxiv.org/abs/2402.16463) | 未知代价需要探索 / explicit exploration | 平稳假设与奖励模型直接适用 / matching stationarity and objective |

不引入完整 StarPU 或机器学习依赖：当前任务无 DAG，新增运行时复杂度必须由实验收益证明。
No full StarPU or machine-learning dependency is justified for independent file tasks.

## 候选模型 / Candidate model

对文件大小 s 与后端 d，固定对数大小桶维护服务时间/字节的指数加权移动平均
（Exponentially Weighted Moving Average, EWMA）及绝对预测误差。仅同桶预测，未知
必须显式表示；校准先验与真实文件样本区分。
Fixed logarithmic size bands maintain EWMA service time per byte and prediction error.
Only same-band predictions are allowed; unknown values are explicit and calibration priors
are distinct from actual file observations.

预计完成时间（Earliest Finish Time, EFT）候选式：

`finish(d, s) = max(now, available(d)) + predicted_service(d, s) + uncertainty(d, s)`

CPU 可用时刻来自可接手的工作线程，而非把所有线程排队时间当作串行和。
误差余量是工程保守项，不是经证明的置信区间。忙闲转移仍需保证工作守恒，避免
估计过期而让整个队列停滞。未知桶采用受限探索或静态策略，不编造吞吐率。
CPU availability concerns the earliest usable lane, not a serial sum across lanes.
Error margins are engineering safeguards, not proven confidence intervals. Work must not
stall behind stale estimates. Unknown bands use bounded exploration or static policy.

服务时间包含读取与摘要完成。失败重试、比较任务、异常任务不污染正常哈希模型。
CPU/GPU 样本不是配对反事实；选择偏差（selection bias）和磁盘争用可能使估计失真。
Successful hash service includes reads and completion. Retries, comparison and failed jobs
do not train normal service predictions. Selection bias and I/O contention remain limitations.

## 实现约束 / Implementation constraints

- 解耦 CPU 块大小、GPU 更新块与文件资格。默认资格 16 MiB 仅沿用研究初值。
  Decouple CPU block, GPU update and eligibility; 16 MiB remains a tunable initial policy.
- 设备正确性、各形状证据、超时、设备失败独立表达；保留完整的局部证据。
  Separate correctness, per-shape evidence, deadline and device failure.
- 持续剖析（continuous profiling）是本进程的任务级采样聚合，不是调用栈采样器。
  Task-level continuous profiling is not an OS stack sampler.
- 复用已有任务计时，在已有完成锁下汇总，固定内存；不逐块计时或逐文件写日志。
  Reuse task clocks and completion locks; fixed memory, no per-block timers or file logs.
- `--no-pgo` 禁用运行时采样、学习和模型决策；保留静态路由、安全校验及 summary。
  The flag disables runtime sampling, learning and model decisions, not safety or summary.
- PGO 在这里指运行时剖析引导优化（profile-guided optimization），不改变编译器优化。
  Runtime profile-guided optimization is independent of compiler PGO.

## 实验驱动的修订：不做启动性能探测 / Evidence-driven revision: no startup performance probes

第一版在线模型仍保留合成性能校准。在独立20k小文件加一个64MiB文件的四臂实验中，
该固定成本仍使新版 auto 多付出启动时间。最终生产路径因此只保留至多64KiB的完整摘要
正确性校验，不执行合成性能基准，也不注入性能先验。模型只从真实成功文件任务学习。
The first online prototype retained synthetic startup calibration, which still added fixed cost on
an isolated 20k-small-plus-one64MiB workload. Production now only validates complete digests on at
most64KiB; it runs no synthetic performance benchmark and seeds no timing priors. Learning uses
successful real file tasks only.

未知模型采用研究初始政策：满足资格且至少64MiB时GPU优先，其余CPU优先，之后由真实
观测修正。旧校准函数及独立形状证据仍供测试和离线基准使用，不再属于扫描热路径。
Unknown models use the research-informed initial preference (eligible >=64MiB GPU, otherwise CPU),
then actual observations revise placement. The retained shape-calibration helper serves tests and
standalone benchmarks, not production scanning.

## 反证与验收 / Falsification and acceptance

需要验证同大小冷/热输入偏差、速度漂移、CPU 即将空闲、GPU 长任务占用、部分校准、
64 MiB CPU 块不改变资格、禁用后的零模型样本与正确摘要。微基准必须披露 ns/decision，
端到端实验区分旧版、新版、禁用消融与 CPU 对照。
Test cache-state bias, drift, near-idle CPU, busy GPU, partial calibration, block independence,
disabled zero observations and complete digests. Report decision costs and distinct controls.

**不承诺数学意义的零开销。** 候选验收门槛和完整方案见
[实验计划](online-routing-experiment-plan.md)。实现与实测未完成前，本文件是设计，非结果。
Zero overhead is not a truthful mathematical claim. Until implementation and measurements
are complete this is a design, not a validation report.

### 实际选择不等式 / Implemented comparison

两端都有有效预测时，选择 GPU 的条件为：

`GPU_wait + GPU_service + GPU_error < 0.95 * (CPU_wait + CPU_service) - CPU_error`

这是带 5% 滞回的保守优势判定，不是对称的置信区间，也不保证全局最优。等待时间来自
正在运行任务的剩余预测，未构建所有排队任务的完整离线排程；过期预测由可用对端接手。
With both predictions known, this conservative comparison requires a five-percent margin. It is not
 a symmetric confidence interval or global optimality guarantee. Wait estimates concern in-flight
 residuals, not a complete offline schedule of all queued work; overdue predictions yield to the peer.
