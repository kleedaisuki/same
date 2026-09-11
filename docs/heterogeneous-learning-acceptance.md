# 异构在线学习验收 / Heterogeneous online-learning acceptance

状态：进行中，以下是验收条件，不是完成声明。
Status: in progress; these are acceptance requirements, not completion claims.

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

2026-09-11：原生 iGPU 差分测试通过；完整本机测试首次为 21/22，集成测试的旧两后端
计数断言失败，正在核查。当前大小分桶 EWMA 尚不满足上述最终建模与持久化要求。
On 2026-09-11, native iGPU differential tests passed. The initial full local run passed 21/22;
an integration assertion for two-backend accounting failed and is under investigation.
The current size-band EWMA does not yet satisfy the final modeling and persistence requirements.
