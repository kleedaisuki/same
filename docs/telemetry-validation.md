# 遥测验证记录 / Telemetry validation

日期 / Date: 2026-09-08. 设计、SQL 检索与工程/学术来源见 [telemetry.md](telemetry.md)。

## 结论 / Findings

**异步写入与跨运行记录已通过功能验证，但实际开销明确存在，不能声称零成本或完全无回归。** 最终二进制的 7 对测量中，fresh/cache/rehash 配对进程耗时中位差分别为 **+68.09/+29.89/+39.95 ms**。缓存扫描平均配对比值 **1.1234**，对短扫描不可忽略。已有库 rehash 也增加耗时，因此不能把初次建库当作唯一原因。

Async persistence passed functional checks, but final-binary paired median process differences were +68.09/+29.89/+39.95 ms for fresh/cache/rehash. Cache scans averaged a paired ratio of 1.1234: material on this short workload. Existing-database cost rules out attributing everything solely to schema creation. Zero overhead and no-regression claims are unsupported.

先前同一测量二进制的两轮 fresh 中位差从 +60.25 ms 变为 −4.55 ms，说明噪声显著；这不抵消最终测量的开销，也不支持选择性报告某一轮提速。历史结果在下方保留。

Earlier fresh-database medians reversed (+60.25 ms to −4.55 ms), showing substantial variability, not negating final measured cost. Historical results remain below rather than being cherry-picked away.

## 功能覆盖 / Functional evidence

CUDA 必需的 Release 与 CPU-only 全量 CTest 均 **19/19 通过**。覆盖有界队列压力与丢弃、独立结束槽、跨运行保留、损坏/锁定/陌生数据库非致命失败、链接和侧文件保护、禁用开关、运行失败历史、配置与完整模型导出。实际测试命令如下；Windows 构建在 VS x64 开发环境中完成。

Both CUDA-required Release and CPU-only suites passed 19/19. Coverage includes queue pressure/loss, reserved finalization, retention, nonfatal database failures, unsafe paths, switches, failure history, configuration and model export. Windows builds use a VS x64 developer environment.

```powershell
$env:SAME_REQUIRE_CUDA = '1'
ctest --test-dir build/release --output-on-failure
Remove-Item Env:SAME_REQUIRE_CUDA
ctest --test-dir build/goal-cpu --output-on-failure
```

最终两套 19/19 均包含“无 `--summary` 仍输出写入失败警告”回归。这里的通过不是 GPU 遥测性能背书，下面性能试验强制使用 CPU。

Both final suites include writer-error warnings without summary. Functional CUDA coverage is not a GPU telemetry performance benchmark: performance trials below force CPU.

## 实验设计 / Experimental design

- Windows 11 (10.0.26200)，Intel Core i9-12900H，20 逻辑处理器；MSVC 19.44，CUDA 12.8，Python 3.14.6。
- 10000 个唯一文件，127 个子目录，文件大小循环 128/1024/4096/16384 B，总计 54,080,000 B；不是用户 TeX Live 目录。
- 固定 CPU 后端、4 内容线程、4 元数据线程、1 MiB 块、64 MiB 主存与显存预算、任务队列 64；PGO 保持启用，仅切换遥测。
- 每轮 7 个顺序执行的随机化配对，每个分支先 1 次预热；stdout 捕获，`--summary --format=tsv --color=never`。
- fresh：新摘要库和新遥测库，强制重算；cache：同库有效缓存；rehash：已有库强制重算。fresh **不是冷操作系统缓存**。
- 每次比较完整路径→32 字节摘要映射、文件/读取计数和无重复结果；参考是同一程序关闭遥测的 CPU 实现，**不是独立 BLAKE3 正确性判据（oracle）**。
- 开启分支检查各轮独立 ID、completed、有效配置 JSON、64 个模型区间和至少 256 个区间字段；关闭分支确认不创建遥测库。

Fixed generated inputs and sequential randomized pairs isolate the telemetry toggle while leaving runtime PGO enabled. Full digests are checked against the same CPU implementation, not an independent cryptographic oracle. Fresh means a fresh database, not a cold OS page cache. Every run also validates history/configuration/model presence.

三份本地报告均 `status=complete` 且所有运行 `verified=true`：首轮 `telemetry-final` 32 次、第二轮 `telemetry-three-phase` 48 次、最终 `telemetry-release-final` 48 次（含预热）。最终报告是当前实现的主要性能证据。脱敏逐次指标见 [telemetry-benchmark.csv](telemetry-benchmark.csv)：112 行非预热运行，保留全部配对而非仅均值；不含本地绝对路径、运行 ID 或巨大逐文件映射。原始完整报告保留在开发工作区 `.cache`。

All three reports completed with every run verified: 32/48/48 runs including warmups. The final campaign is primary evidence for the final implementation. The linked CSV retains all 112 non-warmup runs and pair identities without machine paths, run IDs or large per-file maps.

测量程序均为 `same 0.3.0`，SHA-256 / Binary identities:

| Campaign | SHA-256 |
|---|---|
| 最终 / telemetry-release-final | `2a6d520524961e77b1f784ebe78d993a22de15ee0af11c12b5ee95e322918980` |
| 历史前两轮 / Earlier two | `b7667ee813d69dcc501b7e1b0d6f45458e0a8b71e1cb157d818a0198bf329ab7` |

不同构建不能混作同一二进制性能样本；最终报告在追加写入错误检查和参数快照后重新测量。 / Do not pool distinct builds as identical-binary evidence; the final report was remeasured after writer error checks and parameter snapshots.

## 结果 / Results

单位均为 ms，暖机样本排除。Δ 是同 trial 的 `on − off`，不是两组中位数之差；平均比值是各配对 `on/off` 的算术平均。errors/drop 是该阶段 7 次开启运行的总数，包含“数据不丢失”的反证。

All times are ms, excluding warmups. Delta is paired on-minus-off, not a difference of group medians. Mean ratio averages paired on/off ratios. Errors/drops sum across the seven enabled trials.

| 轮次/阶段 / Round/phase | off 进程中位 / Median | Δ 配对中位 / Median | Δ 配对平均 / Mean | 平均比值 / Ratio | drain 中位 | errors / dropped |
|---|---:|---:|---:|---:|---:|---:|
| **最终 fresh** | **795.78** | **+68.09** | **+49.85** | **1.0685** | **7.25** | **0 / 2** |
| **最终 cache** | **275.98** | **+29.89** | **+34.14** | **1.1234** | **9.32** | **0 / 0** |
| **最终 rehash** | **777.73** | **+39.95** | **+8.52** | **1.0366** | **10.78** | **0 / 0** |
| 首轮 fresh | 641.14 | +60.25 | +71.34 | 1.1126 | 4.91 | 0 / 1 |
| 首轮 cache | 265.25 | −0.98 | −1.27 | 0.9963 | 8.95 | 0 / 0 |
| 第二轮 fresh | 684.79 | −4.55 | −19.89 | 0.9759 | 5.21 | 0 / 1 |
| 第二轮 cache | 249.94 | +3.88 | −0.04 | 1.0038 | 8.79 | 0 / 0 |
| 第二轮 rehash | 638.01 | +29.24 | +36.98 | 1.0605 | 6.46 | 0 / 1 |

所有阶段无写入错误，但历史两轮分别丢弃 1、2 条普通事件，最终轮丢弃 2 条；完整最终模型和指标仍通过验证。try-lock 竞争也可产生丢弃，因此无需队列填满才出现损失。drain 只是退出等待，不等于全部遥测成本；后台 CPU/存储竞争和初始化仍可能影响主流水线。

There were no writer errors, but earlier rounds dropped 1 and 2 queued events; the final campaign dropped 2. Final model/metrics validation still passed. Try-lock contention can lose events without a full queue. Drain is only exit waiting, not total overhead; startup and background contention can affect the pipeline.

## 复现 / Reproduction

使用新的输出目录，脚本拒绝覆盖已有实验目录。当前脚本包含三个阶段；第一轮是在加入已有库 rehash 阶段前运行，不能把当前三阶段命令称为首轮完全相同的脚本版本。

Use a new output directory; the script refuses existing results. The current script has three phases; round one preceded addition of existing-database rehash and is not claimed to be an identical script revision.

```powershell
python tools/telemetry_benchmark.py --exe build/release/same.exe `
  --output .cache/telemetry-reproduction --files 10000 --trials 7 `
  --warmups 1 --workers 4 --seed 20260908
```

## 局限与下一步 / Limits

只有一台机器、一个小文件形状、短扫描与 7 对样本；OS 缓存、温度、后台负载和文件系统抖动未被完全控制。未测试网络文件系统、断电耐久性、GPU 主导大文件遥测成本或生产长运行。没有证明硬实时延迟、无锁或零 I/O。若目标是可重复的开销上限，需要更长稳态实验、更多独立重复及实际目录分布，并分别比较主流水线与完整进程时间。

One machine, one small-file distribution, short scans and seven pairs do not establish a universal overhead bound. Cache/thermal/background/filesystem effects remain. Network filesystems, power loss, GPU-heavy telemetry and production-duration runs are unmeasured. Longer steady-state runs and independent repetitions are needed for a defensible overhead bound.
