# 自适应路由设计依据 / Adaptive routing rationale

> 历史固定偏好调度阶段；当前在线模型见 [online-routing-design.md](online-routing-design.md)。以下测量不能视为当前在线模型的验证。
> Historical fixed-preference stage; these measurements do not validate the current online model.

## 问题定义 / Problem

自动模式要保留小载荷的 CPU SIMD 优势，为长载荷或明显 GPU 优势提供 GPU 偏好，并让空闲
设备帮助忙碌设备。目标不是提高 GPU 利用率，也不是无条件把所有大文件绑定 GPU。
Auto routing preserves CPU SIMD on small payloads, favors GPU for demonstrated profitable shapes,
and allows idle-device assistance. GPU utilization and unconditional large-file binding are not goals.

当前执行契约见 [auto-dispatch.md](auto-dispatch.md)。本文件区分既有观察、推断和设计选择；
历史结果不等于本次改动的性能验证。
See the execution contract there. This document separates historical observation, inference, and policy;
historical results do not validate the performance of this change.

## 研究到参数 / Evidence to parameters

| 既有证据 / Historical evidence | 参数或设计 / Parameter/design | 不足以证明 / Does not prove |
|---|---|---|
| [GPU 剖析](gpu-profiling.md) 的同一 64 MiB 输入，16 MiB 更新明显优于 1 MiB 更新 / identical total input, different update granularity | CPU 默认 1 MiB 不变；独立 GPU 优先 16 MiB / preserve CPU default, separate GPU block | 每台机器、每种 IO 均同样加速 / universal speedup |
| [调度结果](dispatch-results.md) 中单 GPU 胜出、多 GPU 实例竞争反转 / single-instance win reverses under contention | 最多一个 GPU 服务 / one GPU service | GPU 与所有 CPU 并发一定增加吞吐 / guaranteed aggregate gain |
| 既有 `gpu_min_bytes` 默认 16 MiB / existing floor | 默认资格下界仍 16 MiB / retain default floor | 16 MiB 是精确交叉点 / exact crossover |
| 长流正证据使用 64 MiB 总量、16 MiB 更新 / long-stream evidence uses 64 MiB total and 16 MiB updates | `max(64 MiB,4*G)` 长类边界及四块校准 / long class and four-block calibration | 64 MiB 是物理常数 / hardware-independent law |
| 小批次曾受初始化拖慢 / setup has harmed short scans | 仅首次合格任务设置；明确记录初始化成本 / lazy eligible setup, explicit cost accounting | 新策略仍有旧 4 GiB 摊销保护 / preservation of retired gate |

这里的 16 MiB 资格下界、16 MiB GPU 更新块与 64 MiB 长类边界分别控制“能否卸载”、
“如何喂入设备”和“使用哪种性能证据”，不能混为一个文件大小阈值。
The eligibility floor, GPU update size, and long-class boundary respectively control permission,
feeding granularity, and evidence selection. They are not one interchangeable size threshold.

## 为什么分离可用性和偏好 / Availability versus preference

单任务 GPU 未胜 CPU，不意味着 CPU 全忙时 GPU 没有增量能力。因此校准结果只决定偏好，
不再直接销毁可用服务；不过小于资格下界的任务始终 CPU-only，避免为填满设备而做无效卸载。
A GPU that loses a single-job comparison may still add capacity when CPUs are saturated. Calibration
therefore sets preference, not existence. Below-floor jobs remain CPU-only: occupancy is not usefulness.

反过来，GPU 偏好不应使空闲 CPU 等待正在工作的 GPU。任务亲和性（task affinity）是可互助
的偏好，而非永久设备绑定。单任务开始后不迁移状态，避免引入树状态转移及恢复复杂度。
GPU preference must not idle CPUs behind a busy GPU. Affinity is assistable preference, not permanent
binding. No migration after acquisition avoids hash-tree state-transfer and recovery complexity.

额外 GPU 服务保留全部 `N` CPU 工作者，故不应沿用“把一个 CPU 工作者换成 GPU”的旧混合
模型证明新配置吞吐。它还会消耗主机读取/归并时间、内存带宽、锁页内存与显存；必须预算化。
An additional GPU service preserves all `N` CPU workers, unlike the old replacement-worker model.
It still consumes host reading/reduction time, bandwidth, registered memory, and device memory.
Historical replacement-model throughput is not proof of new-model performance.

## 工业与学术依据 / Production and research context

- [NVIDIA CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html)
  支持批量传输、包含传输同步的完整测量以及有界复用注册内存。对应实现选择是独立较大 GPU
  更新块和单服务，不是只看内核时间或无上限增加锁页内存。
  Batching, complete transfer-inclusive measurement, and bounded registered-memory reuse motivate
  a separate larger GPU block and one service, not kernel-only claims or unbounded pinned memory.
- [BLAKE3 官方 C 实现](https://github.com/BLAKE3-team/BLAKE3/tree/1.8.2/c)
  提供 CPU SIMD 分派；复用而非再实现，保留不同机器上的可运行性与摘要一致性。
  Reuse upstream SIMD dispatch instead of duplicating it; preserve portability and digest semantics.
- [StarPU，CCPE 2011](https://doi.org/10.1002/cpe.1631) 将异构设备性能与任务调度关联。
  本项目只借鉴性能偏好与动态领取，不引入通用运行时；一次性文件流与可复用计算图不同。
  Borrow performance affinity and dynamic acquisition, not a general runtime: one-pass file streams
  differ from reusable task graphs.
- [Gonthier 等，JPDC 2025](https://doi.org/10.1016/j.jpdc.2025.105170)
  的内存约束与局部性调度提示未来方向，但线性代数任务的数据复用不能直接外推到文件哈希。
  Memory/locality-aware scheduling is relevant future work, but linear-algebra reuse does not directly
  generalize to streaming hashes. Adoption needs reproducible workload-specific evidence.

## 对抗性审查与后续证据 / Adversarial review and follow-up evidence

稳定 20% 校准优势是保守偏好门槛，不是整个扫描至少快 20% 的保证。CPU 活动数也不是实际
CPU 利用率：任务可能等待磁盘。共享 IO、带宽或功耗约束可能让“多一个执行设备”反而变慢。
因此应分别测 CPU-only、自动模式、显式 CUDA，并同时保留完整摘要、设置时间、设备实际
尝试数、总耗时及输入分布，比较暖缓存与普通文件场景，不以稀疏零文件代表冷盘。
A stable 20% calibration margin is a preference gate, not a scan-speed guarantee. Active CPU jobs may
be waiting for IO. Shared IO, bandwidth, and power can reverse the benefit of another device. Compare
CPU-only, auto, and explicit CUDA with full digests, setup cost, backend attempts, total time, and input
distribution. Sparse zeros are not a substitute for cold physical-file evidence.

若代表性混合扫描持续退化，首先检查初始化、GPU 实际块大小、互助触发与资源竞争，
而不是恢复一个会阻止用户所要求互助的隐式 4 GiB 门槛。任何新限流机制须有独立证据，
并明确保持小载荷 CPU、可用设备双向互助与既有显式模式契约。
If representative scans regress, inspect setup, actual block size, assistance triggers, and contention
before reintroducing an implicit volume gate that defeats requested assistance. New throttling needs
independent evidence and must preserve small-job CPU execution, assistance, and explicit-mode contracts.
