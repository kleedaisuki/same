# 上下文学习与三设备路由 / Contextual learning and three-device routing

本文描述当前实现，不是加速或跨平台验收报告。最终性能和 CI 结果须以对应提交的实测为准。
This is an implementation contract, not a speedup or platform-validation claim.

## 输入与目标 / Inputs and objective

每个内容工作线程独占在线模型（online model），分别预测 CPU、CUDA dGPU 和 OpenCL iGPU 的成功哈希服务时间。预测目标是任务服务耗时，不是单独的内核时间、物理磁盘带宽或整个扫描的完成时间。读取、哈希和设备争用相互影响；本模型不宣称因果识别。
Each worker owns independent backend models for successful hash service time, not kernel time or whole-scan makespan. Storage caching and shared-resource pressure remain potential confounders.

输入 payload 字节数为 B，有效更新批量为 L，提交时观测的竞争任务数为 C。四维特征（feature vector）是：

```text
x = [1, B / 2^20, ceil(B/L) / 1024, (B / 2^20) * C / 1024]
predicted_ms = xᵀ β_backend
```

四项分别表示固定成本、字节成本、批量提交成本和与 payload 交互的竞争成本。CPU/iGPU 的 C 包括共享主机资源的在途任务；CUDA 使用同后端任务数。C 是轻量代理而非带宽测量。设备的名称、厂商、驱动、实现版本、架构、计算单元、硬件线程、内存约束、统一内存属性和实际批量参与设备身份键；它们**不是全部作为回归变量输入**。因此这是本机按设备拟合，不是跨任意 GPU 的零样本性能预测。
The four terms represent fixed, byte, batch and payload–contention costs. Device characteristics namespace learned state; the implementation does not regress every hardware field or promise zero-shot transfer across devices.

## 数学模型 / Mathematical model

每后端维护可加充分统计量（sufficient statistics）：S=Σxxᵀ、t=Σxy、q=Σy² 和权重 W，以及特征范围。固定 4×4 数组，无逐样本历史分配。前四个本轮样本每次拟合，之后每 16 个样本重拟合，预测使用缓存系数。求解采用对角尺度归一化的岭回归（ridge regression）：

```text
Dii = sqrt(max(Sii, 1e-24))
A = D^-1 S D^-1 + 1e-8 I
A θ = D^-1 t
β = D^-1 θ
```

使用小型 Cholesky 分解（Cholesky decomposition），原始统计量不加入正则项。非有限值、非法范围或不合格数值状态被拒绝，失败求解不发布新系数；缓存系数与其训练范围同时发布。训练域检查只是逐特征包围盒，不证明联合分布覆盖或统计置信度。
A fixed-size Cholesky solve publishes coefficients and their domain bounds together. Rejected numerical updates never become valid predictions; marginal bounds are not joint-support or confidence guarantees.

预测附带的残差 RMS 是当前拟合的样本内（in-sample）残差尺度，不是置信区间（confidence interval）。另外在加入当前样本**之前**统计预测误差总量、平方和、EWMA 和有界直方图，用于观察真实在线预测表现。不要把样本内 RMS 当成未见数据上的误差保证。
Fit RMS is distinct from pre-update prediction errors. Neither supplies a calibrated confidence interval.

## 决策与边界 / Decisions and boundaries

自动模式先检查大小资格、预算和设备可用性，再比较三个候选的预测；CPU 偏好和残差余量是工程启发式。未知或训练域外输入触发有限初始探索，并有周期性确定性探索。该策略不是 LinUCB 或 Thompson sampling，没有遗憾界（regret bound）、无偏反事实估计或全局最优保证。被选择后端才产生标签，存在选择偏差；周期探索不能自动消除缓存、顺序与设备负载混杂。
Routing uses bounded initial and periodic deterministic exploration, not a statistically calibrated bandit algorithm. Only selected devices generate outcomes, so selection bias remains.

核显只有一个池内计算实例：占用时不等待，当前任务可回退 CPU；CUDA 使用工作线程自己的流。`--cpu`、`--cuda`、`--igpu` 覆盖配置，但加速器仍受大小门槛、预算和可用性约束。可恢复计算错误重试 CPU，内容正确性检查不一致仍失败。精确重复判断始终包含逐字节比较。
The iGPU is admitted without waiting; forced accelerator modes remain subject to eligibility and safe fallback. Correctness failures are not hidden as performance fallback.

## 跨运行状态 / Cross-run state

`.same/model.db` 独立于摘要缓存 `state.db` 和遥测日志 `telemetry.db`。设备按需初始化后，才按真实设备特征加载匹配的不可变启动快照；身份键还包含特征版本和工作线程数。驱动、实现或相关参数变化导致独立状态，而不是静默混用。
The model database is independent of digest and telemetry databases. Lazy device initialization resolves the exact identity before loading priors.

启动先验在运行边界乘以 0.9；每个工作线程单独累加本轮增量。成功扫描收尾按身份键**只合并一次先验**，再加各线程增量，避免工作线程数倍增历史证据。衰减针对统计权重，不意味着原始样本计数按比例减少，也不是按墙钟时间遗忘。存储故障形成诊断，不改变哈希与分组正确性。
At run boundaries, prior statistical weight decays by 0.9. Finalization merges one prior per key plus worker deltas, rather than multiplying history by worker count. Raw counts are not effective decayed weight.

| 开关 / Switch | 学习与 model.db / Learning | telemetry.db |
|---|---|---|
| 默认 / default | 加载、学习、保存 / load, learn, save | 写入 / enabled |
| `--no-telemetry` | 保留 / retained | 不打开 / unopened |
| `--no-pgo` | 不加载、不学习、不保存；静态路由 / disabled, static routing | 保留基础历史 / basic history retained |

`--summary` 只控制展示。模型读写在冷路径（cold path）；工作线程读取内存启动快照，不执行 SQL。模型更新和预测使用线程局部固定数组；少量设备准入与在途计数仍为共享原子状态，因此不能称整个调度器无同步。
Summary visibility does not enable learning. SQL stays off worker hot paths; shared admission and activity atomics still exist.

## 观测、导出与验证 / Observation, export and validation

summary 包含各后端任务/采样计数、预测误差、域外与数值拒绝计数、延迟/残差有界分布，以及模型加载命中、保存错误和冷路径时间。工作线程快照导出先验与本轮充分统计量，可用于离线分析；遥测开启时记录分析器参数。这里的持续剖析（continuous profiling）是任务级统计，不是操作系统调用栈采样器。它追求低开销，但没有“零成本”承诺。
Worker snapshots expose additive statistics for analysis; task-level profiling is not OS stack sampling. Measure its overhead rather than assuming it away.

验收应分别覆盖摘要差分与生命周期、真实 iGPU 内核执行、无设备回退、跨进程复用、参数失效、数值退化、冷/热缓存、线程数和设备争用。PoCL 可验证 OpenCL 内核语义，但运行于 CPU 时不能证明物理 iGPU 的吞吐量。跨平台 CI 和本机性能数据由对应实验报告记录；本文不预填通过结果。
Validation must distinguish software portability, real-device execution and performance. PoCL execution is not physical-iGPU performance evidence.


## 依据与取舍 / Sources and trade-offs

- [StarPU 官方性能手册](https://files.inria.fr/starpu/doc/html_web_performances/)将在线监测与离线追踪作为异构运行时的配套能力。same 借鉴“观测支撑调度”的工程方向，但不引入其完整任务图运行时，也不把其性能结果迁移到本项目。 / StarPU supplies a mature reference for monitoring heterogeneous execution; its results do not validate same.
- [Li et al., WWW 2010, A Contextual-Bandit Approach to Personalized News Article Recommendation](https://www.schapire.net/papers/www10.pdf)说明上下文与仅观察所选动作反馈的建模方式。same 的动作是设备、响应是服务时间，但当前确定性探索不是论文的 LinUCB；不能继承其经验收益或理论保证。 / Contextual-bandit framing motivates explicit contexts and partial feedback, not an inherited guarantee.
- [Li et al., Unbiased Offline Evaluation of Contextual-bandit-based News Article Recommendation Algorithms](https://arxiv.org/abs/1003.5956)研究基于随机日志的重放评估。same 当前没有随机动作概率日志，因此直接比较已选择设备的平均耗时不是无偏策略评估。 / Randomized-log replay assumptions do not hold for deterministic routing logs.

下一步证据应优先回答：相同受控 payload 和缓存条件下是否降低总耗时？学习开销是否抵消收益？设备/驱动变化与负载漂移后是否及时恢复？可重复的交错对照、消融和域外测试比单次“GPU 利用率上升”更有解释力。
Prioritize controlled interleaved comparisons and ablations over utilization alone; faster prediction is not automatically faster scanning.

## 冷启动准入 / Cold-start admission

设备探测（probe）与激活（activation）分离：探测建立设备身份与能力，激活才创建执行资源、编译内核并检查正确性。探测本身仍可能调用驱动、产生耗时，不能称为免费。初始化历史独立于稳态服务模型，保存在模型库 schema 2；不能把初始化时间作为每个 payload 的哈希服务标签。
Probing identifies capabilities; activation creates executable resources and checks correctness. Probe costs are not necessarily zero. Schema 2 stores setup history separately from steady-state service statistics.

自动且启用 PGO 时，只有临时决策实际请求某设备，才执行其冷启动预检（preflight）。共享非阻塞门槛要求：当前 CPU 任务的已知保守成本至少覆盖 bootstrap 估计，或本轮探索信用覆盖该估计，才允许 OpenCL ICD 探测或 CUDA 创建。竞争失败直接继续 CPU，不等待；静态 no-PGO 决策为 CPU 时不探测设备。首次未知且很短的扫描因此有意保持 CPU。`cuda_bootstrap_ms=100.0` 是 CUDA 的独立默认估计。
Automatic PGO preflights only a provisionally requested device, before ICD discovery or CUDA creation. A shared nonblocking gate requires known CPU-task potential or funded credit; unknown short scans intentionally remain CPU. This is not guaranteed optimal under unknown cold costs.

获得设备 profile 后，冷 iGPU 激活还必须满足下列条件之一：当前任务的已知预测节省至少覆盖历史最大初始化耗时；或本轮探索信用足以覆盖初始化估计。信用约为 `cold_exploration_fraction × eligible_CPU_work_ms / workers − actual_cold_spent_ms`。共享支出包括 OpenCL 发现、iGPU 激活及 CUDA 初始化的实际耗时。它只在本轮累积，不跨运行结转。历史不足时使用显式配置 `igpu_bootstrap_ms=100.0` 毫秒；`cold_exploration_fraction=0.05` 为默认值。
Automatic PGO admission amortizes estimated setup against either known current-task savings or run-local exploration credit. Credit uses eligible CPU work divided by worker count and subtracts actual discovery, iGPU activation and CUDA setup spending; it is not persisted.

这是预算启发式，不是随机探索概率、硬实时上界或百分之五总耗时保证。首次初始化的实际耗时可能超过 100 毫秒估计，历史最大值也不保证未来成本上界。历史初始化最大值不随稳态统计衰减，一次异常尖峰可能长期使准入更保守。`--igpu` 绕过自动冷启动门槛，但仍保留设备资格、内存预算和错误回退；`--no-pgo` 保持静态路由语义。
The fraction is a credit budget, not a probability or overhead guarantee. First activation can exceed its estimate. Forced iGPU bypasses this automatic gate; static no-PGO routing is unchanged.

模型库采用预写日志（write-ahead logging, WAL）与 `synchronous=NORMAL`。学习状态是建议性数据：断电可能丢失最近提交，允许退回较旧学习状态；摘要与精确比较的正确性契约不因此放宽。主机 CPU 身份为空时禁用跨运行复用，而不是关闭本轮学习。
WAL/NORMAL trades recent-learning durability for lower advisory-state overhead; it does not weaken content correctness. Missing CPU identity disables cross-run reuse, not in-run learning.

早期初始化策略造成的负收益保留在 [扫描性能记录](contextual-scan-performance.md)，作为历史反例，不删除或改写为成功。冷启动修正后的性能需另行测量，本文不预先宣称已消除回归。
The earlier negative result remains in the performance record. Post-fix performance is pending measurement, not assumed improved.

显式把某后端启动估计设为零，可用于允许不受启动信用限制的校准实验；已加载的正核显历史估计仍优先于零回退值。默认值不采用这种实验设置。
An explicit zero bootstrap estimate permits unbudgeted calibration; a loaded positive iGPU setup estimate still overrides the zero fallback. Defaults do not enable this experiment mode.
