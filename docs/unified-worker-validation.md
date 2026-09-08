# 统一工作线程验证 / Unified-worker validation

日期 / Date: 2026-09-08. 当前设计见 [unified-worker-design.md](unified-worker-design.md)，运行契约见 [auto-dispatch.md](auto-dispatch.md)。

## 验收与证据 / Acceptance and evidence

| 要求 / Requirement | 实际证据 / Evidence |
|---|---|
| 每线程拥有模型，队列锁外预测/训练 | Worker owns OnlineModel；dequeue 后选择、完成后本地学习；源码审阅和隔离回归 / Ownership inspection and isolation tests |
| 固定 N 线程与单 FIFO | Resources 固定创建 N 线程；一个有界队列；任务完成/关闭测试 / Fixed-count queue and shutdown tests |
| 总主机/显存预算 | CPU 先预留，剩余主机和设备额度除以 N；边界预算/零额度回归 / Per-worker quota boundary tests |
| 错误隔离与排空 | 可恢复设备错误完整 CPU 重试；非设备异常传播；所有已接纳任务完成 / Retry, exception and drain tests |
| 逐线程模型原值 | 每线程 64 区间×4 字段；独立 telemetry-check 检查配置、指标和原始参数 / 256 band fields per worker |
| 真 CUDA 多任务与摘要一致 | 两个线程实际 CUDA 任务并发，与 CPU 完整摘要对照 / Real concurrent CUDA tasks with CPU digest comparison |
| 工具链回归 | CUDA 必需与 CPU-only CTest 各20/20；Python 合并工具12/12 / Both CTest suites and merge tests |
| CUDA 内存/竞争工具 | Compute Sanitizer 12.8：unified_cuda memcheck 0 errors、racecheck 0 hazards / Instrumented real-CUDA test |

Compute Sanitizer 结果仅覆盖被执行的测试路径，不证明所有 GPU 程序无竞争；CPU-only 测试也不冒充 CUDA 执行。源码锁边界审阅与行为测试互补，不能仅用测试数量证明没有中央模型锁。

Sanitizer evidence covers exercised paths only. CPU-only tests are not CUDA execution. Source ownership/lock review complements, rather than follows from, passing test counts.

## 试验方法 / Experimental method

Windows 11，Intel Core i9-12900H，RTX 3070 Ti Laptop，MSVC 19.44/CUDA 12.8。普通文件而非稀疏零文件；每个混合工作负载含 10000 小文件与 16×64 MiB 文件，主机和显存总预算各 1 GiB，4 个元数据线程，CPU 块1 MiB、队列64。workers=1/4/20，auto/no-pgo/cpu 各5对测量加1对预热；顺序执行，随机化配对顺序，fresh数据库但不保证冷OS缓存。

Ordinary mixed files, 1 GiB host/device total budgets, fixed metadata/block/queue settings, and workers 1/4/20 with auto/no-pgo/cpu. Each case uses five sequential randomized pairs plus warmup, fresh databases but not cold OS caches.

旧基线是 N 个 CPU 工作线程加最多一个 GPU 服务，新版是恰好 N 个统一线程。相同 workers 参数不代表相同总内容线程数；这是用户配置语义对照，不是严格等线程资源的算法优劣试验。计时运行使用 `--no-telemetry`；参数持久化在另一次非计时 telemetry-check 验证，不能由本试验推断遥测零开销。

The old baseline has N CPU workers plus up to one GPU service; the new implementation has exactly N unified workers. Matching configuration is not equal total thread count. Timed runs disable telemetry; persistence is checked separately, so this experiment does not establish telemetry overhead.

完整路径→32字节摘要与 CPU 参考对照、输出/计数均校验。参考仍是本项目 CPU 实现，不是独立 BLAKE3 oracle。普通文件读取包含 OS 缓存影响。GPU peak 统计选中 GPU 的任务生命周期（含IO），不是同时执行内核数；cold_start_cpu 是一次性设备初始化期间暂走 CPU 的决策数。

Full digest maps, outputs and counts are checked against the project's CPU implementation, not an independent BLAKE3 oracle. GPU peak counts GPU-selected task lifetimes including I/O, not concurrent kernels. Cold-start CPU counts temporary selections during one-time device initialization.

## 初始实现：保留负面证据 / Initial implementation: negative evidence retained

初始三个报告均 complete、每份36次运行且全部 verified。当首次 CUDA 初始化占用多个工作线程时，自动模式平均配对耗时比值（new/baseline）在 workers=1/4/20 为 **1.0919/1.1833/1.2624**；20线程 no-pgo 为 **2.1779**，异常慢样本保留，不因不利而排除。CPU 对照平均比值分别0.9988/0.9863/0.9516。

All three initial reports completed with verified runs. Auto mean paired ratios were 1.0919/1.1833/1.2624; the 20-worker no-pgo ratio of 2.1779 retains its slow samples. CPU ratios were 0.9988/0.9863/0.9516. These are negative evidence, not a speedup claim.

据此加入仅冷启动阶段的一次性协调：首个线程初始化设备时，其他线程继续 CPU；ready 后多线程独立流并发不受单设备准入门限制。这是针对观测的改动，必须以重新测量而非设计意图判断效果。

One-time cold-start coordination was added in response: peers continue CPU during first initialization, with unrestricted private-stream concurrency after readiness. Its effect requires remeasurement, not inference from intent.

## 最终结果 / Final results

最终四份报告均 complete，每份36次计时运行（含预热）且全部 verified，另有CPU摘要参考及telemetry-check通过。前三份混合输入同前；steady 使用128个小文件与64×64 MiB普通文件、4线程，它是更大载荷对照，不保证达到无冷启动影响的稳定状态。逐次非预热数据见 [CSV](unified-worker-runs-2026-09-08.csv)，包含初始与最终八份报告共230行，不含机器路径或完整摘要映射。

All four final reports completed with verified runs, CPU digest references and separate telemetry checks. Steady uses 128 small files plus 64×64 MiB files at four workers; its name does not prove initialization-free steady state. The CSV retains 230 non-warmup observations across initial, final and equal-total-thread campaigns.

下表进程时间为ms中位数，平均比值为同trial new/baseline比值的平均，不是中位数之比。GPU峰值为新版各次观察范围；cold CPU是5次新版运行合计，不是单次值。

Process times are medians in ms; ratios average paired new/baseline values, not the ratio of medians. GPU peak ranges cover new runs; cold CPU counts sum five new trials.

| 负载/线程 / Case | backend | baseline中位 | new中位 | 平均配对比值 | GPU peak范围 | cold CPU合计 |
|---|---|---:|---:|---:|---:|---:|
| mixed/1 | auto | 2159.68 | 2460.59 | 1.1604 | 1–1 | 0 |
| mixed/1 | no-pgo | 2202.37 | 2539.57 | 1.1520 | 1–1 | 0 |
| mixed/1 | cpu | 2387.35 | 2371.74 | 1.0076 | 0–0 | 0 |
| mixed/4 | auto | 1013.97 | 1011.44 | 1.0237 | 2–4 | 47 |
| mixed/4 | no-pgo | 1001.51 | 1012.20 | 1.0322 | 4–4 | 41 |
| mixed/4 | cpu | 900.43 | 926.25 | 1.0175 | 0–0 | 0 |
| mixed/20 | auto | 903.30 | 844.04 | 0.9493 | 1–1 | 75 |
| mixed/20 | no-pgo | 903.36 | 838.96 | 0.9255 | 1–1 | 75 |
| mixed/20 | cpu | 752.98 | 730.90 | 0.9821 | 0–0 | 0 |
| steady/4 | auto | 618.33 | 916.54 | 1.5101 | 4–4 | 48 |
| steady/4 | no-pgo | 623.94 | 1002.03 | 1.5407 | 4–4 | 45 |
| steady/4 | cpu | 649.57 | 634.39 | 0.9937 | 0–0 | 0 |

**通用性能胜利并不成立。** 20线程混合负载改善，但GPU peak只有1，75次cold CPU说明短批次大部分任务在初始化期间走CPU，不能声称20路GPU加速。4线程观察到2–4路GPU任务并发；更大载荷auto仍明显慢于旧基线（平均1.5101倍）。所有负面结果保留，不能以功能测试或某个有利case替代性能目标。

There is no universal performance win. The 20-worker mixed improvement observed peak GPU concurrency one with many cold CPU selections, not twenty GPU lanes. Four workers show actual 2–4 GPU-task overlap, yet the larger auto case is substantially slower (mean ratio 1.5101). Functional success does not erase performance regressions.

### 等总内容线程对照 / Equal total content threads

补充实验保持旧4 CPU+1 GPU与新版5统一线程，64×64 MiB+128小文件；报告 complete，24次含预热运行全部verified，CPU参考及telemetry-check也通过。它隔离内容线程总数这一因素，但不等同于相同GPU流数、资源分配或算法。

The supplementary case matches old4CPU+1GPU against new5 unified workers on the larger input. All24 runs including warmup, the CPU reference and telemetry check passed. Equal total threads do not imply equal stream count, allocation or scheduling behavior.

| backend | baseline中位ms | new中位ms | 平均配对比值 | 新GPU peak | cold CPU合计 |
|---|---:|---:|---:|---:|---:|
| auto | 493.60 | 658.13 | 1.4714 | 4–5 | 64 |
| no-pgo | 490.21 | 622.30 | 1.2775 | 5–5 | 60 |

**即使匹配总线程，负面性能结果仍然存在**，不能把此前更大载荷回归全部归因于旧N+1与新N的线程差异。也不能跨不同轮次直接比较中位数推导因果；应使用各轮内部配对。该证据支持“固定统一线程与本地所有权已实现”，不支持“其性能优于原架构”。

Regressions remain after matching total threads: the oldN+1/newN count difference is not the sole explanation. Cross-campaign medians are not causal comparisons; use within-campaign pairs. The architecture is implemented, not proven faster.

### 二进制身份 / Binary identity

| 实现 / Implementation | SHA-256 |
|---|---|
| baseline | `d58a491a7a47194ed771316f03b843cc98d2b4ffdee0321545b76a810db4010d` |
| 初始 / Initial unified | `3353422a0d76f572e7531356ee13230e4bea4596219ed94bf7c9a32ab5d35880` |
| 最终冷启动协调 / Final | `972bc2467c49ce3738023e2f9818b12f7d923e0b596de5b145fd0345f1c7b3c4` |

产品版本均保持0.4.0，哈希区分不同实现。旧基线没有GPU peak或cold CPU指标：CSV缺失peak留空，不能解释为0；旧setup是单服务时间，新setup_sum是多线程累计，可重叠，不能直接当作墙钟差。

All identify version 0.4.0; hashes distinguish implementations. Missing old GPU peak is blank, not zero. Old setup is one service; new setup sums worker time and may overlap, not directly comparable wall time.

### 复现 / Reproduction

```powershell
python tools/unified_worker_benchmark.py --baseline .cache/same-pre-unified.exe `
  --exe build/release/same.exe --output .cache/unified-reproduce `
  --workers 4 --trials 5 --warmups 1 --small-files 10000 `
  --large-files 16 --large-mib 64 --telemetry-check
```

输出使用新目录，保持顺序配对；更大载荷将small-files改128、large-files改64。基线可执行文件必须与上表身份相符。等总线程额外对照已完成：加 `--workers 5 --baseline-workers 4 --small-files 128 --large-files 64 --backends auto,no-pgo`，其余设置相同。

Use a new output directory, matching baseline identity, and sequential pairs. The larger case changes small-files to128 and large-files to64. The completed equal-total-thread comparison uses workers5, baseline-workers4, small-files128, large-files64 and backends auto,no-pgo.

## 限制 / Limitations

一台机器、有限输入分布和5对测量无法证明普遍加速；本地模型样本碎片化、GPU共享资源争用、热状态与文件系统噪声仍影响结果。初始失败测量与最终测量属于不同二进制，不能池化为同一实现的平均收益。摘要一致与资源所有权重构是功能证据，不等价于性能目标通过。

One machine and five pairs do not establish universal speedup. Model fragmentation, shared-device contention, thermal/cache/filesystem effects remain. Distinct binaries must not be pooled as one implementation. Correctness and ownership evidence are not performance evidence.

## 收尾复核 / Final checks

最终同一 Release 二进制的 CUDA 必需 CTest 连续三轮全部通过（20×3），CPU-only 再次20/20，Python合并工具再次12/12。遥测工具以128文件验证 fresh/cache/rehash 三阶段开关对照，六次均 verified；这是功能冒烟，不是开销估计。发布页在1440px与390px视口检查无横向溢出、无失效页内锚点，并检查截图。Release增量构建无待构建工作，SHA与最终实测身份一致。

The same final Release binary passed three repetitions of all20 CUDA-required suites; CPU-only20/20 and Python merge12/12 passed again. A128-file telemetry smoke verified all six on/off fresh/cache/rehash cases, not an overhead estimate. Desktop1440px/mobile390px layout and screenshots were checked without horizontal overflow or missing local anchors. The no-op Release rebuild retained the measured binary identity.
