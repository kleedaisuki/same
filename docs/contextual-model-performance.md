# Contextual model microbenchmark / 上下文模型微基准

## Scope / 范围

2026-09-11, Windows x64, Intel Core i9-12900H, MSVC 19.44.35228, `/O2 /EHsc /std:c++latest /utf-8`, no LTO. This measures a hot, single-thread, worker-owned model API, not device execution, file I/O, queueing or whole-scan acceleration. No affinity or frequency lock was imposed; concurrent desktop activity and hybrid-core migration remain confounders.

此实验测量热缓存下、单线程独占模型的 API 开销，不测文件 I/O、设备执行或整扫描收益。没有绑定核心或锁定频率；桌面活动、混合核心迁移仍可能干扰。绝对成本不扣除基线，因为两条路径的指令重叠不同。

Source identity / 源码身份 (SHA-256):

- `src/online_model.cpp`: `80FA0729D4B72FA16FBA79B7F0F4BE9C6A18CE529AE6FE066A2F665E3B471B96`
- `include/same/detail/online_model.hpp`: `2900854ABB6087918300C3EDE7E8E3A9C9EFE4F4E13E18689ED560C2068AFFC9`

## Reproduce / 复现

Run from the repository in an x64 Visual Studio Developer Command Prompt; create `build` if absent. No project CMake modification or GPU SDK is required.

在 x64 Visual Studio 开发者命令提示符从仓库执行；若无 `build` 则先创建。无需修改项目 CMake 或安装 GPU SDK。

```bat
cl /nologo /O2 /EHsc /std:c++latest /utf-8 /Iinclude tools\contextual_model_benchmark.cpp src\online_model.cpp /Febuild\contextual_model_benchmark.exe /Fobuild\
build\contextual_model_benchmark.exe
```

The harness uses a fixed 256-context array: payload 4 KiB–64 MiB, batch 64 KiB–1 MiB, extra-peer count 0–3. It trains all three backends before measurement. Synthetic service time is `0.02 + bytes/1e7` milliseconds; this is an overhead workload, not a device-performance model. Each case doubles its loop count until calibration exceeds 100 ms; nine fresh-model trials follow, each with 4096 warmup operations. Setup is excluded. A volatile final checksum and separately compiled production translation unit without LTO prevent dead-code elimination of calls. Solver cost is included amortized at its actual cadence (first four observations, then every 16; warmup passes the initial phase).

固定输入含大小、批量、竞争参数变化；三设备均预训练。合成服务时间仅构造有效训练负载，不代表真实设备速度。自动校准后执行九轮，每轮独立初始化并预热；计时不含初始化。最终可观察校验和及非 LTO 分离编译防止消除调用。更新成本包含每 16 次观测一次求解的摊销成本（初始前四次求解已被预热覆盖）。

## Results / 结果

| Operation / 操作 | Median ns/op | Meaning / 含义 |
|---|---:|---|
| Baseline / 输入算术 | 0.989 | Array access + arithmetic / 数组读取与算术 |
| Legacy band predict / 旧分桶预测 | 5.641 | One backend / 一个后端 |
| Context predict / 上下文预测 | 20.923 | One trained backend / 一个已训练后端 |
| Legacy band observe / 旧分桶更新 | 15.237 | One accepted sample / 一个有效样本 |
| Context observe / 上下文更新 | 116.541 | Includes profiling and amortized solve / 含剖析与摊销求解 |
| Three-backend predict / 三设备预测 | 65.497 | Three trained calls per operation / 每操作三次已训练预测 |

`sizeof(OnlineModel) = 5328` bytes in this build. The contextual API is materially more expensive than a band lookup, but remains sub-microsecond here. Three predictions plus one observation total roughly 182 ns **by arithmetic**, not a measured combined scheduling path. At a 100 μs task this arithmetic ratio is about 0.18%; do not apply it to tiny tasks, whole scans or other machines without measurement. No contention benchmark was performed: worker ownership removes model sharing in this harness, not proof that the application has no other locks.

本构建模型大小 5328 字节。上下文模型比旧分桶查询明显更贵，但此机热路径仍低于微秒。三次预测加一次更新的约 182 ns 是算术相加，不是完整调度路径实测。100 μs 任务的算术占比约 0.18%；不能外推到极小任务、整扫描或其他机器。此实验不测锁竞争，线程独占模型也不代表应用其他位置没有锁。

### Raw output / 原始输出

```text
sizeof_model=5328
baseline,iterations=134217728,ns/op=0.986;0.995;1.026;1.030;0.989;0.991;0.962;0.938;0.970;,median=0.989
band_predict,iterations=16777216,ns/op=5.659;5.641;5.634;5.781;5.581;5.657;5.436;5.665;5.539;,median=5.641
context_predict,iterations=8388608,ns/op=21.063;20.752;20.928;21.348;20.822;20.700;20.923;20.502;22.961;,median=20.923
band_observe,iterations=8388608,ns/op=15.962;15.113;15.673;14.837;14.879;15.789;15.176;15.237;15.438;,median=15.237
context_observe_amortized,iterations=1048576,ns/op=114.237;113.922;116.541;116.850;116.352;116.928;117.291;118.363;116.462;,median=116.541
three_backend_predict,iterations=2097152,ns/op=64.838;64.896;65.497;70.173;67.293;64.558;65.191;69.124;66.198;,median=65.497
```

## End-to-end ablation plan / 端到端消融方案

Use a deterministic corpus with duplicate candidates and byte-comparison correctness checks; stratify 4 KiB, 64 KiB, 1 MiB, 16 MiB and 64 MiB, plus a mixed corpus. Compare forced CPU, forced CUDA, forced iGPU, static/no-PGO routing, fresh contextual state, and reused contextual state. Keep worker count and memory/batch budgets fixed; use separate workspace states so hash-cache reuse does not bypass hashing. Always use `--rehash` for hashing comparisons. Use one warmup and at least five interleaved/reversed-order runs; do not call OS-cache-warm runs cold. Report median and all raw wall times, hash counts by backend, retries, setup time, service distributions, prediction error, out-of-domain observations and persisted/restored sample counts. Verify duplicate groups identical in every condition. Separate startup/compilation from steady-state service, and disclose that warm model reuse may coexist with cold process/device startup. Add changed payload distribution to test adaptation rather than merely replaying training data.

确定性语料应含重复候选并核对最终逐字节结果；按上述大小分层并增加混合分布。比较 CPU、CUDA、iGPU 强制模式、静态无 PGO、全新模型、跨运行复用模型。固定线程与内存参数，隔离状态并使用 `--rehash` 防止缓存绕过计算。一次预热、至少五轮交错或逆序测量，记录全部原始时间和后端任务数、重试、启动成本、服务分布、预测残差、域外观测及恢复样本。所有条件输出必须一致。改变 payload 分布以检查适应性，不能只重放训练数据。以上为待执行方案，不是已取得的端到端证据。
