# 在线路由实验预注册 / Online routing experiment plan

## 目标与边界 / Objective and scope

在运行性能实验前规定比较方法。优化目标是整轮完成时间，而不是 GPU 利用率。
**绝对零开销不可能由实验证明**；本实验检验有限负载上的开销上界。
Preregister comparison before measurements. Optimize end-to-end completion, not GPU occupancy.
Experiments cannot establish literal zero overhead; they estimate workload-specific bounds.

四臂：旧版 auto、新版 auto、新版 auto `--no-pgo`、新版 CPU。
`--no-pgo` 是同版本消融（ablation），不是 CPU 后端的别名。
Four arms separate version changes, runtime-analysis effects, and CPU reference performance.

## 主指标与门槛 / Primary outcomes and gates

- 小文件 fresh/cache 分开比较新版 auto / 新版 no-pgo 的逐区组进程耗时比。
  Small-file fresh/cache paired process ratios are separate primary outcomes.
- 预定目标：均值比值的单侧 95% 自举上界不超过 1.02。
  Target: one-sided 95% bootstrap upper bound of mean paired ratio <= 1.02.
- 默认七个随机完整区组（randomized complete blocks），一个完整预热区组，种子固定。
  Seven measured blocks and one warmup block by default; deterministic randomized arm order.
- 这是探索性自举（bootstrap）界限，少样本、自相关或系统噪声可能使其不可靠；
  不通过只表示未证明目标，不等同于证明退化。少于七轮标记证据不足。
  Bounds are exploratory and sensitive to small samples, autocorrelation and system noise.
  Failure means not demonstrated, not necessarily a proven regression; fewer than seven trials are insufficient.
- 不丢弃异常值，不事后更改主要指标，不以长文件收益掩盖小文件成本。
  Preserve outliers and preregistered outcomes; large-file wins do not excuse small-file costs.

## 工作负载 / Workloads

| 场景 / Case | 默认或建议 / Default or proposed | 用途 / Purpose |
|---|---|---|
| small | 20000 普通唯一文件，128B–64KiB / ordinary unique files | 路由快路径 / routing fast path |
| tex-like | 20000 个128B–4KiB + 一个64MiB / tiny files plus one large | 用户实际形状：几乎全小文件 / predominantly tiny inputs |
| mixed | 小文件 + 10×64MiB；可设512MiB / configurable large files | 长尾及设备互助 / long tail and assistance |
| stress | 可设100000–245000文件、workers20、block64MiB / configurable | 用户配置规模 / user-like scale |

所有文件为新生成的普通文件，不是稀疏文件；未清除 OS 页面缓存，因此不是冷盘实验。
Each arm gets a fresh application database followed immediately by a cache phase.
Files are ordinary and newly written, not sparse; OS caches are not flushed, so these are not cold-disk tests.
每臂独立数据库，fresh 后紧接 cache。该顺序固定，因此 cache 结论不代表随机冷热混合访问。
Cache always follows fresh; it is not randomized cold/warm mixed access.

## 正确性与证据 / Correctness and evidence

工具 `tools/online_routing_benchmark.py` 在测量前运行显式 CPU/no-pgo 生成完整32字节摘要真值；
每轮检查全部路径、完整摘要、无重复输出、扫描/哈希/缓存数量及哈希读取字节。
An untimed explicit CPU/no-pgo scan establishes full 32-byte digest reference. Each run checks paths,
full digests, unique output, file/cache/hash counts and hash read bytes.
SHA256 文件真值用于描述生成数据；BLAKE3 CPU对照不是独立密码学算法正确性证明。
Fixture SHA256 records describe input; CPU BLAKE3 equivalence is not an independent algorithm proof.

原始 stdout/stderr、完整配置、命令、版本、二进制SHA256、各轮数据库和增量JSON均保留。
失败时保留已完成证据，报告状态为 failed，不生成“通过”摘要。
Preserve raw outputs, configuration, commands, versions, executable hashes, databases and incremental JSON.
Failure preserves evidence and marks the report failed rather than passing partial data.

## 运行约束与复现 / Controls and reproduction

性能实验串行执行，不与编译或其他基准并行。记录机器、CUDA/驱动、供电模式；未记录则明确限制。
All performance runs are sequential, without simultaneous builds/benchmarks. Record hardware, driver and
power mode externally; disclose missing controls. Process startup and output capture are included.
标准输出始终捕获，避免交互终端显示速度混入对照；用户实际终端开销需另做实验。
Captured stdout avoids terminal-rendering differences; interactive terminal costs need a separate experiment.

```powershell
# Smoke only, not performance evidence / 仅冒烟，不作为性能证据
python tools/online_routing_benchmark.py --baseline BASELINE.exe --exe NEW.exe --output .cache/online-smoke --files 64 --trials 1 --warmups 0
# Primary small-file measurement / 主要小文件实验
python tools/online_routing_benchmark.py --baseline BASELINE.exe --exe NEW.exe --output .cache/online-small --files 20000 --trials 7
# Predominantly tiny workload / 几乎全小文件负载
python tools/online_routing_benchmark.py --baseline BASELINE.exe --exe NEW.exe --output .cache/online-tex --workload tex-like --files 20000 --trials 7
# Long tail / 长尾
python tools/online_routing_benchmark.py --baseline BASELINE.exe --exe NEW.exe --output .cache/online-mixed --workload mixed --files 128 --large-mib 512 --trials 7
```

输出目录必须不存在；工具不删除目录，不读取用户数据集，不自动扩大到系统RAM预算。
Output must be new; the tool never deletes directories, scans user datasets, or sizes budgets from installed RAM.

## 独立于端到端实验的验证 / Complementary verification

微基准必须测旧规则、新模型预测、采样/非采样更新及小文件快路径；使用预生成输入、
防优化校验和、空循环对照，报告ns/job及绝对成本。热路径不得每任务分配或新增系统调用。
Microbenchmarks separately measure old rules, prediction, sampled/unsampled updates and small-file paths,
using pregenerated inputs, observable checksum and empty-loop control. Report absolute ns/job.
建议路由CPU成本预算 <= 流水线累计CPU时间的1%；该指标不由进程耗时自动推导。
Suggested routing CPU budget <=1% of summed pipeline CPU time; process timing alone cannot establish it.
确定性测试覆盖部分校准、未知/慢/失败、时变样本、队列互助、重试、关闭分析器。
Deterministic tests cover partial calibration, unknown/slow/failed states, changing samples, assistance,
retry and disabled analysis. GPU路由次数不作为必过指标；摘要正确性始终是硬门槛。
GPU routing counts are observations, not mandatory performance outcomes; correctness is mandatory.
