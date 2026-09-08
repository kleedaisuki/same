# 统一工作线程设计 / Unified worker design

## 目标与非目标 / Goals and non-goals

固定 `workers=N` 个内容工作线程，共享一个先入先出（first-in, first-out, FIFO）任务队列。每个线程取出任务后，使用自己拥有的在线模型选择 CPU SIMD 或自己的 CUDA 执行流（stream）。不再维护 N 个 CPU 线程外加一个 GPU 服务，不再在中央调度锁内预测、训练或估计全局设备等待。

Exactly N content workers consume one FIFO. Each worker owns its model and chooses CPU SIMD or its own CUDA stream after dequeueing. There is no extra GPU service and no centralized prediction/training under the queue lock.

这是所有权与复杂度重构，不是新算法已证明更快的声明。目标是让队列只负责有界任务交接，让设备、缓冲和模型的生命周期归属于执行者。比较任务仍使用 CPU；模型不会把队列取出顺序误称为完成顺序。

This is an ownership/complexity redesign, not a proven speedup. The queue handles bounded handoff; workers own resources and learning. Comparisons stay on CPU and FIFO dequeue does not imply FIFO completion.

## 数据与资源所有权 / Data and resource ownership

```text
coordinator → one bounded FIFO → worker 0: CPU / CUDA stream 0 + model 0
                              → worker 1: CPU / CUDA stream 1 + model 1
                              → ...
                              → worker N-1: CPU / CUDA stream N-1 + model N-1
                                      └ shared CUDA primary context
```

每个线程独占 CPU/GPU 缓冲、后端对象、探索计数和 64 个 CPU/GPU 大小区间。主机/显存预算作为全池总预算分摊，不能把全额预算重复授予每个线程。CUDA 按需初始化；不可用设备或可恢复的设备计算错误仅停用该线程 GPU 并完整重试 CPU，不退休线程或增加未经预算的 CPU 通道。验证摘要不一致、内存分配等非设备异常保留原错误并使扫描失败，不作为普通 GPU 不可用吞掉。

Workers exclusively own buffers, backend objects, exploration counters and 64 CPU/GPU bands. Host/device budgets are pool totals divided across workers, not duplicated entitlements. Lazy CUDA unavailability or recoverable compute errors disable only that worker's GPU and retry on CPU. Validation mismatches and other non-device exceptions propagate and fail the scan rather than being swallowed.

自动模式将冷启动与稳态分开：进程级一次性 `cold → initializing → ready` 状态协调首个设备初始化；此时其他线程先执行 CPU，不等待初始化，也不永久标记自己的 GPU 不可用。设备就绪后，各线程仍惰性创建自己的流与后端，可并发使用 GPU。它不是单 GPU 任务准入门或稳态串行锁，不恢复中央任务路由；显式 CUDA 不使用此自动模式冷启动策略。

Auto separates one-time cold startup from steady state: a process-wide cold/initializing/ready state lets one worker initialize while peers continue on CPU without permanently disabling their local GPU. After readiness, private backends/streams initialize lazily and GPU work remains concurrent. This is not a single-GPU admission gate or steady-state serialization; forced CUDA is unchanged.

同一设备使用共享的 CUDA 主上下文（primary context），各线程持有独立流；不为每个线程创建独立上下文。独立流允许运行时安排重叠，但不保证实际并行，显存带宽、PCIe、磁盘和 GPU 计算资源仍共享。N 个流不等于 N 块 GPU。共享语义另见 [CUDA Programming Guide §3.2.1/§3.2.8](https://docs.nvidia.com/cuda/archive/12.5.0/cuda-c-programming-guide/index.html)。参考 [NVIDIA CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html) 的上下文、并发与传输讨论。

Streams share the device primary context, avoiding one context per thread. Independent streams allow overlap, not guaranteed concurrency; memory/PCIe/storage/compute remain shared. N streams are not N GPUs. See NVIDIA's best-practices guidance above.

## 数学模型的边界 / Model boundary

线程已经取得任务后才选择后端，所以此处比较的是**本线程任务服务成本**，不是中央最早完成时间（earliest finish time, EFT）排程。每个模型按后端和大小区间更新每字节成本与残差的指数加权移动平均（exponentially weighted moving average, EWMA）。本地观察只在相同后端/区间使用；未知成本不能当成零。

After claiming a task, a worker compares local service costs, not a centralized EFT schedule. Per-backend/size-band cost and residual EWMAs learn from local observations; unknown costs are not zero and evidence is not extrapolated across bands.

本地观测包含该线程遭遇的读取与设备争用，但不是整个 GPU 的排队模型。每线程样本分散可能减慢收敛，同一输入大小在不同线程上的估计可能不同。独立探索也可能同时挤占 GPU。合并摘要只能帮助分析，不能悄悄反向喂给本地模型而重新引入隐藏的中央调度器。

Local observations include experienced I/O/device contention, not a global GPU queue model. Fragmented samples may slow learning, workers may disagree and simultaneous exploration may compete. Reporting aggregates must not silently become a centralized training feedback path.

[StarPU 的性能模型与调度实践](https://starpu.gitlabpages.inria.fr/features.html) 展示了按设备估计任务成本的价值，也说明调度需要结合系统状态。此实现选择更简单的线程私有模型，不能借用 StarPU/HEFT 的理论或实测结果证明全局最优。

StarPU illustrates the value of device-specific costs and system-aware scheduling. This implementation deliberately chooses simpler worker-private models and does not inherit StarPU/HEFT optimality or performance claims.

同行评审依据包括 [StarPU, CCPE](https://onlinelibrary.wiley.com/doi/10.1002/cpe.1631) 和 [JPDC 2025 任务调度研究](https://www.sciencedirect.com/science/article/pii/S0743731525001376)。后者关于数据局部性和核外（out-of-core）调度的共享数据/线性代数假设不等同于独立文件哈希；这里借鉴测量与资源约束意识，不照搬算法复杂度或论文收益。

Peer-reviewed StarPU and the linked JPDC 2025 scheduling study provide research context. Data-locality/out-of-core shared-data linear-algebra assumptions do not transfer directly to independent file hashing; their algorithms and gains are not inherited here.

## 汇总与持久化 / Summary and persistence

运行结束并等待工作线程空闲后聚合样本数和直方图；成本模型仍逐线程保留。各线程误差 EWMA 分别展示，不合成一个冒充全局到达顺序训练结果的误差值。覆盖率统计“线程×后端×区间”，分母随线程数变化，不能与旧单模型的 32 区间覆盖直接比较。

After workers become idle, counts/histograms are aggregated without merging cost models. Each error EWMA is reported locally, never merged into a purported globally ordered EWMA. Coverage counts worker/backend/band cells and is not directly comparable with the former 32-band-per-backend global model.

遥测终态导出每线程全部 64 个区间，包括未知状态；不跨运行自动加载历史参数。具体键名与指标以 [运行时契约](auto-dispatch.md) 和 [遥测模式](telemetry.md) 为准。旧版本实验报告仍只证明对应版本，不能当成本重构的性能证据。

Telemetry exports all 64 bands per worker, including unknown cells, without cross-run loading. Current contracts define concrete keys. Historical reports remain evidence only for their measured versions.

## 验收原则 / Acceptance principles

- 单 FIFO、固定线程数；队列锁中无模型预测/训练，任务无丢失与重复执行。
- CPU/GPU 摘要一致；单线程后端失败不破坏其他线程或 CPU 回退。
- 总预算与惰性分配成立；关闭过程排空已接纳任务并销毁各自流/缓冲。
- `--no-pgo`、`--no-telemetry` 与显式 CPU/CUDA 契约保持独立。
- 测量端到端与流水线时间、峰值资源、采样覆盖和预测误差；披露并发争用和冷启动成本。

Validate FIFO handoff/fixed workers, exclusive ownership, digest equality, isolated failure, total budgets, shutdown, independent switches and measured end-to-end/resource effects. No unmeasured performance gate is considered passed.
