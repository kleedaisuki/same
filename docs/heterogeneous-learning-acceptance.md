# 异构在线学习验收 / Heterogeneous online-learning acceptance

以下为实现证据与验收门；最终跨平台结果以对应提交的 Actions 为准。
Implementation evidence and acceptance gates; final portability results are those of the matching commit's Actions run.

## 目标与边界 / Objective and boundaries

针对本机 CPU、CUDA 独显和 OpenCL 核显，根据 payload 和实际设备特征预测完整任务服务成本。
以扫描吞吐及完成时间验证调度，不把单独内核吞吐当作端到端收益。
Predict task service cost for the local CPU, CUDA GPU and OpenCL integrated GPU using payload
and actual device features. Validate scan throughput and completion time, not kernel throughput alone.

| 要求 / Requirement | 必须检查的证据 / Required evidence |
|---|---|
| 真实 iGPU / Real integrated GPU | 设备身份、实际内核提交、CPU 差分摘要、完整 CLI 扫描 / Device identity, kernel submissions, differential hashes, CLI scan |
| 三设备预测 / Three-device prediction | 相同上下文比较、冷启动探索、设备忙/失效回退 / Comparable contexts, cold-start exploration, busy/failure fallback |
| 数学模型 / Mathematical model | 特征、目标、更新规则、假设、误差与漂移处理 / Features, objective, update, assumptions, error and drift handling |
| 持续剖析 / Continuous profiling | 线程私有统计、预测前残差、采样与丢弃计数、summary 导出 / Private statistics, pre-update residuals, sampling/drop counts, summary export |
| 跨运行学习 / Cross-run learning | 第二进程确实导入、设备/实现变更失效、损坏数据安全拒绝 / Second-process import, identity invalidation, corrupt-state rejection |
| 合并正确性 / Correct merge | 只合并本轮增量，不能按 worker 数重复累计启动先验 / Merge run deltas without multiplying copied priors |
| 性能 / Performance | 预测/更新微基准、剖析开关消融、端到端重复试验 / Prediction/update microbenchmarks, profiling ablation, repeated end-to-end trials |
| 跨平台 / Cross-platform | 最终提交的 Windows/Linux/macOS Actions 与 PoCL 实际内核运行 / Final-commit Actions plus actual PoCL kernel execution |

## 生命周期约束 / Lifecycle constraints

- 摘要只筛选候选，最终逐字节比较和新鲜文件身份校验不变。
  Hashes only filter candidates; exact comparison and fresh file validation remain mandatory.
- 模型状态不是文件正确性缓存；模型读取失败不能令错误摘要被接受。
  Model state is not a correctness cache; loading failures cannot validate incorrect hashes.
- 学习状态与可禁用/可淘汰的遥测日志分离；不在每个任务上做数据库 I/O。
  Learning state is separate from optional retained telemetry; no per-task database I/O.
- 设备标称参数不是实测带宽；未知字段不应制造精确性能先验。
  Nominal device properties are not measured bandwidth; unknown fields must not invent precise priors.
- 跨运行复用必须允许温度、功耗、驱动、并发负载及存储缓存变化后重新学习。
  Reuse must permit relearning under thermal, power, driver, contention and storage-cache changes.

## 当前证据 / Current evidence

2026-09-11，本机 Release 在 `SAME_REQUIRE_CUDA=1`、`SAME_REQUIRE_IGPU=1` 下通过 25/25 CTest。
Local Release passed all 25 CTests with both physical-device requirements enabled.

| 范围 / Scope | 可检查证据 / Inspectable evidence |
|---|---|
| 原生核显与退化路径 | `igpu_compute` 验证 5,817 个真实内核批次、任意分块、重复 finish、生命周期与 CPU 差分；`integration` 验证真实 CLI 及缓存 |
| 模型数学与局部性 | `contextual_model` 验证固定四维回归、共线、拟合范围、数值拒绝和可加增量；`Worker::model` 仅所属线程更新 |
| 调度与正确性 | `igpu_routing`、`adaptive_routing`、`hash_retry`、`unified_cuda` 验证冻结上下文、三设备探索、准入、重试、双 CUDA 并发与冷启动预算 |
| 持久化 | `model_store` 验证 v1→v2 迁移、外来库不变、损坏拒绝、事务回滚和有界状态；`learning_integration` 验证第二进程导入、先验不重复、无遥测学习及关闭学习不改库 |
| 剖析与分析器数据 | `application.cpp` 的逐 worker 导出包含先验/增量矩阵、范围、更新前误差、设备资料及启动历史；summary 独立于遥测开关 |
| 性能 | [模型微基准](contextual-model-performance.md) 与[扫描消融](contextual-scan-performance.md) 包含命令、原始计时、二进制身份和不利结果；不以核函数吞吐替代扫描耗时 |
| 跨平台门 | `.github/workflows/ci.yml`：Windows/Linux/macOS × OpenCL ON/OFF、PoCL 强制实际内核、ASan/UBSan；最终提交八个 job 全部成功才通过此门 |

实际预算激活实验：第一进程累计 CPU 工作信用后执行一次 16 MiB iGPU 哈希；第二进程恢复
354.620 ms 启动历史并因信用不足避免重新激活。该实验使用显式乐观启动配置，不代表默认策略
的硬性开销上界。原始数据位置与完整配置见扫描报告。
The physical budget experiment executed a 16 MiB iGPU hash, then reused 354.620 ms setup history
in a second process to defer activation. Its optimistic configuration does not establish a hard overhead bound.

## 已知限制 / Known limitations

- 设备参数条件化模型身份；同一设备的固定计算单元数不作为可辨识的因果回归系数。
  Device properties condition model identity; constant hardware properties are not identifiable causal coefficients.
- 冷启动和发现成本均纳入决策，但首次未知成本可超过估计，短扫描可能不探索。
  Startup/discovery participate in admission, but unknown first costs may exceed estimates and short scans may not explore.
- 历史启动最大值不衰减；暂时尖峰会使后续准入更保守。服务成本先验每轮衰减 0.9。
  Setup maxima do not decay; transient spikes make admission conservative. Service priors decay by 0.9 per run.
- 最终小文件实验仍观察到 PGO 的毫秒级额外成本，不声称零开销或普遍 GPU 加速。
  Final small-file trials still show millisecond-scale PGO overhead; no zero-overhead or universal GPU-speedup claim.
