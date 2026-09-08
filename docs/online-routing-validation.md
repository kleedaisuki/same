# 在线路由验证 / Online routing validation

实现提交 / Implementation commit: `47d57aa553d2fcc2ed6a57078c7f974151b21302`.

## 范围与证据 / Scope and evidence

2026-09-08，Windows 11，Intel i9-12900H，RTX 3070 Ti Laptop 8 GiB，
CUDA 12.8，驱动 572.61，MSVC 19.44。没有 Linux、sanitizer 或冷盘性能验证声明。
Windows-only measurements; no Linux, sanitizer or cold-disk performance claim.

实现契约见 [自动分流](auto-dispatch.md)，文献与模型见 [研究设计](online-routing-design.md)，
预注册方法见 [实验计划](online-routing-experiment-plan.md)。PGO 指运行时反馈优化，
不是编译器 PGO；持续剖析是有界任务采样，不是调用栈采样。
PGO means runtime feedback, not compiler PGO; continuous profiling is bounded task sampling,
not stack sampling.

## 正确性验收 / Correctness audit

| 要求 / Requirement | 已执行验证 / Executed evidence |
|---|---|
| 动态数学模型 / Online model | 独立大小桶、有限值/溢出保护、EWMA 漂移与直方图测试 / bands, finite-value safeguards, drift, histograms |
| 等待时间影响设备选择 / Queue-aware selection | 最早完成时间纯函数反例；实际队列覆盖静态偏好 / EFT counterexamples and real-queue overrides |
| 非选择后端恢复学习 / Censored-feedback recovery | 双向 4096 次任务，仅使用实际选中后端的反馈 / selected-backend-only drift tests |
| 预测失效仍有进度 / Stale-prediction liveness | CPU/GPU 双向门闩，释放被阻塞后端前对端须完成 / gated overdue tests in both directions |
| 关闭与失败不丢任务 / Shutdown and failures | 两类有大小队列，正常/故障关闭排空；全文件 CPU 重读 / drain both queues and whole-file retry |
| `--no-pgo` 真正关闭分析 / Disable contract | CLI/config 校验，零模型观测/先验/性能校准，真实 GPU 路径仍可用 / zero analysis with GPU support retained |
| 参数解耦 / Independent parameters | CPU 块大于文件、GPU 块独立，资格仍由配置下界决定 / independent eligibility and buffers |
| 空文件不吃掉启动机会 / Empty-first startup | 下界为零时空文件后非空任务仍调用设备工厂一次 / deterministic empty-first regression |
| 完整兼容 / Compatibility | CUDA 必需构建与 CPU-only 构建完整套件各 18/18 / both complete suites passed |

核心队列、初始化、重试测试各重复十次，共 40 次；最后空文件修复后初始化测试再重复十次。
Four critical suites passed ten repetitions each; startup passed ten more after the empty-file fix.
独立审查实际发现并修复了时钟重判空队列、条件变量等待模式切换、关闭提前退出及空文件
初始化机会丢失问题，不以纯函数测试代替并发路径验证。
Review exposed and tests covered clock-dependent reselection, condition-variable wait-mode changes,
premature shutdown and empty-file startup consumption.

## 实验过程与负面结果 / Iterations and negative results

第一版模型仍保留合成性能校准。其 20k 小文件首轮消融实验，fresh 比值上界为
1.020340、cache 为 1.049995，均未证明预设 1.02 门槛。不能四舍五入宣布通过。
TeX-like 单大文件实验也显示固定校准成本。最终实现删除生产合成性能探测，只保留
有界完整摘要校验，通过真实成功任务学习。
The first prototype retained synthetic calibration and failed to demonstrate the preregistered
small-file bounds. This negative evidence is retained, not rounded into a pass. Fixed startup
cost motivated removing synthetic performance probes from production.

后续真实任务学习版先完成三组实验；最终又修复下界为零时的空文件启动边界并校正文案，
重新构建同一最终二进制完成独立审计批次。各轮不混合为一次预注册实验，不删异常值，
不把“重复到通过”作为证据。最终结果仍是指定单机负载的探索性证据。
After the real-task-learning revision, a final empty-file boundary fix prompted a fresh binary audit.
Iteration batches remain separate; outliers are retained and results are not pooled or represented
as repeated testing until success. Evidence remains exploratory and machine/workload-specific.

## 性能结果 / Performance results

最终三个审计数据集各64轮（7个测量区组+1个预热区组，四臂、fresh/cache），共 **192轮全部通过完整摘要与计数校验**。三份报告的被测二进制SHA256均与最终 `build/release/same.exe` 一致：

`19660b7a1512482cbee78872f94de5e59293fb9b883e07c818c9da9285f77ac5`

Each audit has 64 runs: seven measured blocks plus one warmup, four arms, fresh/cache.
All192 runs passed full-digest/count oracles and share the final executable digest above.

以下为 fresh **进程耗时中位数**，不是配对比值均值；预热不计入。
Fresh process medians below exclude warmups and differ from means of paired ratios.

| 场景 / Workload | 旧 auto | 新 auto | 新 `--no-pgo` | 新 CPU |
|---|---:|---:|---:|---:|
| small | 1.468 s | 1.445 s | 1.453 s | 1.452 s |
| tex-like | 4.953 s | 4.393 s | 4.429 s | 4.278 s |
| mixed | 0.955 s | 0.871 s | 0.894 s | 0.913 s |

- small：20k普通唯一文件，128B–64KiB，4内容/4元数据线程，CPU块1MiB。
- tex-like：20k小文件+一个64MiB普通文件，20内容/8元数据线程，CPU块64MiB。
- mixed：128小文件+10×512MiB普通文件，4内容/4元数据线程，CPU块1MiB。

These are bounded synthetic fixtures, not the user's installation. All arms use identical per-case
configuration/budgets, with the declared backend/PGO switch as the ablation.

预注册的小文件新版auto/no-pgo配对均值比：fresh **1.005057**，cache **1.001114**；
各自探索性单侧95%上界为 **1.018113 / 1.014974**，本批次均低于1.02。
这支持该负载下低开销，不证明字面零成本或两个指标联合95%覆盖。
Small-file mean paired ratios imply about0.51% fresh and0.11% cache differences; individual exploratory
upper bounds satisfy the1.02 target here, not universal zero overhead or joint95% coverage.

TeX-like 新/旧auto配对比值均值 **0.887442**，mixed为 **0.907017**；旧版对照收益包括
取消启动性能探测与缓冲解耦，不能独归因于EFT。TeX-like 新auto/CPU为 **1.036698**，
说明直接CPU在此仍更快。mixed 新auto/no-pgo均值 **0.985739**、上界 **1.063991**，
不能宣称模型确定优于静态策略；其cache消融上界 **1.078676** 同样未证明2%门槛。
The new version beats the older auto implementation in these selected cases, but not every control.
Static routing remains competitive; neither the mixed disable comparison nor every cache comparison
establishes noninferiority. Full ratios/bounds, including failed gates, remain in the evidence JSON.

### 模型与真实队列成本 / Arithmetic and queue costs

最终独立微基准中位数：loop/load/checksum对照 **0.492ns**，静态资格判断 **0.513ns**，
预测 **4.544ns**，更新 **14.549ns**。不减基线制造零成本，不把仅资格分支当作完整分析器。
Raw arithmetic medians are not baseline-subtracted and are not whole-scheduler costs.

| 真实CPU队列 / Real CPU queue | 关闭中位数 / Off | 开启中位数 / On |
|---|---:|---:|
| 1 worker，小任务 | 481.864ns/job | 490.830ns/job |
| 4 workers，小任务 | 895.971ns/job | 866.565ns/job |
| 1 worker，合格任务 | 505.325ns/job | 562.366ns/job |
| 4 workers，合格任务 | 840.757ns/job | 1031.039ns/job |

队列测量包含真实提交、锁竞争、采样发布和聚合，但无文件I/O或GPU，更新输入明确为
合成1ms。四线程小任务的负差不能解释成分析器免费或加速；早先原型轮次同项约增加49ns，
说明这种空任务微基准对线程调度噪声敏感。合格任务的四线程配对增量中位数约 **196ns/job**，
必须承认非零成本。实际GPU等待预测含额外时钟与最多256通道扫描，不由裸模型微基准完整覆盖。
The real queue benchmark has synthetic1ms update inputs, no file I/O/GPU. Negative small-task deltas
are noise-sensitive, not free profiling. Eligible four-worker paired overhead is about196ns/job.
Actual GPU scheduling also includes clock reads and a bounded lane scan, outside the bare-model test.

### 可审计材料 / Audit artifacts

- [所有阶段512轮记录](online-routing-runs-2026-09-08.csv)：保留原型、学习版与最终审计，不合并阶段统计。
- [参数、二进制摘要及全部比较](online-routing-evidence-2026-09-08.json)：保留所有通过/未证明门槛。
- [最终模型微基准](online-routing-model-2026-09-08.csv) 与 [真实队列微基准](online-routing-queue-2026-09-08.csv)。

Raw portable artifacts preserve every iteration and failed comparison; final audit results alone
refer to the final executable. Benchmarks ran sequentially without concurrent builds; the host was
not a dedicated isolated performance machine, so background activity and thermal drift remain limits.


## 已知边界 / Limits

- 新版与旧版的差异同时包含 GPU 块解耦、取消合成校准与路由变化，不能将整体收益全部
  归因于数学模型。新版与 `--no-pgo` 才是分析器消融。
  Version differences include startup and buffer changes; use same-version disable ablations.
- 样本是选择后的观测，不是配对反事实。离散大小桶不能完整描述冷热缓存、磁盘争用、
  CPU 异构核心或外部 GPU 负载。探索是机会式，不保证固定周期获得样本。
  Selection bias, I/O contention and heterogeneous cores remain; exploration is best-effort.
- GPU 设备初始化仍有实际成本，单个短大文件可能不及直接 CPU；不保证所有负载优于静态策略。
  Device startup still costs time; a short batch can favor CPU or static routing.
- 数据集为普通合成文件，不是用户 TeX Live，也不是冷盘；输出捕获到管道且全部文件唯一，
  不模拟用户数千重复组的交互终端输出成本。
  Synthetic unique-file workloads with captured output do not model thousands of terminal groups.
- 小文件消融的探索性上界不构成跨硬件保证。真实队列微基准有非零增量成本，不能用裸模型
  的数纳秒成本代替；也未据进程耗时宣称累计 CPU 开销严格低于 1%。
  Neither nanosecond arithmetic nor wall-clock ratios establish universally zero overhead or a
  strict aggregate-CPU bound.

## 复现 / Reproduction

```powershell
# 在 VS x64 开发环境运行。 / Run in the VS x64 developer environment.
cmake --preset release
cmake --build --preset release --parallel 4
$env:SAME_REQUIRE_CUDA='1'
ctest --preset release
Remove-Item Env:SAME_REQUIRE_CUDA
cmake --build --preset release --target online_model_benchmark online_queue_benchmark --parallel 4
build/release/online_model_benchmark.exe 2000000 10
build/release/online_queue_benchmark.exe 50000 7
python tools/online_routing_benchmark.py --baseline BASELINE.exe --exe build/release/same.exe --output .cache/reproduce-small --workload small --files 20000 --trials 7
python tools/online_routing_benchmark.py --baseline BASELINE.exe --exe build/release/same.exe --output .cache/reproduce-tex --workload tex-like --files 20000 --workers 20 --metadata-workers 8 --block-mib 64 --trials 7
python tools/online_routing_benchmark.py --baseline BASELINE.exe --exe build/release/same.exe --output .cache/reproduce-mixed --workload mixed --files 128 --large-mib 512 --workers 4 --trials 7
```

输出目录必须新建。原始本机报告包含参数、版本/二进制摘要、配置、完整文件摘要、
每轮 profile 与数据库副本；不提交机器路径、用户数据或大型数据库。
Outputs must be new directories. Local raw reports preserve configuration, executable/file digests,
profiles and databases; machine paths, user data and large databases are not committed.

原型中间源码未作为发布版本保留；其记录用于过程审计，不是可下载版本。最终实现与旧版基线均有明确提交，可按上述脚本重新测量。
Intermediate prototypes were not retained as release source versions; their records document the iteration rather than a downloadable build. Final and baseline sources have explicit commits for fresh reproduction.
