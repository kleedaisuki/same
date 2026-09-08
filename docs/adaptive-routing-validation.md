# 自适应路由验证 / Adaptive routing validation

> 历史固定偏好调度阶段；当前在线模型见 [online-routing-design.md](online-routing-design.md)。以下测量不能视为当前在线模型的验证。
> Historical fixed-preference stage; these measurements do not validate the current online model.

2026-09-08。策略见 [设计依据](adaptive-routing-design.md) 和 [执行契约](auto-dispatch.md)。
历史基线为 `04419ff`；本报告验证独立 GPU 服务及异步初始化的新实现。
Historical baseline: `04419ff`; this report covers independent GPU service and asynchronous startup.

## 参数与正确性 / Parameters and correctness

默认 GPU 资格下界 16 MiB，CPU 更新块 1 MiB，预算允许时 GPU 更新块 16 MiB；
长类为 `max(64 MiB, 4*GPU block)`。三轮配对校准要求最慢 GPU 不超过最快 CPU 的 80%，
块与长流分别决定偏好。资格不等于偏好；低于下界始终 CPU。
Defaults: 16 MiB eligibility, 1 MiB CPU updates, budget-permitting 16 MiB GPU updates;
long class is `max(64 MiB, 4*GPU block)`. Independent three-round shape calibration
requires slowest GPU <= 80% of fastest CPU. Eligibility does not imply preference.

| 要求 / Requirement | 验证 / Verification |
|---|---|
| 阈值、不同更新粒度、独立偏好 / thresholds, update shapes, preferences | `dispatch` 边界和摘要测试 / boundary and digest tests |
| CPU 全忙卸载、GPU 忙回流 / bidirectional overflow | `adaptive_routing` 单/多 CPU 确定性门控 / deterministic single/multi CPU gates |
| GPU 慢仍可辅助、失败退役 / slow GPU assistance, retirement | fake backend 使用真实 BLAKE3，覆盖队列容量与回收 / real BLAKE3, capacity and reclamation |
| 完整文件重读 / whole-file retry | `hash_retry` 注入消费后失败，验证 2x 读取与相同摘要 / post-consumption failure, 2x reads and equal digest |
| 初始化不阻塞 CPU / overlapping startup | `async_scan` 阻塞工厂期间完成三个真实文件；最大候选、空后端、异常及重复 finish / real files, largest candidate, null backend, exceptions, repeated finish |
| 无 CUDA、缓存、显式后端兼容 / compatibility | CPU-only 和强制要求 CUDA 的完整套件各 **16/16** / both full suites passed |

CPU-only 的 `adaptive_routing|async_scan|hash_retry` 各连续运行十次，共 **30 次通过**。
These three tests also passed ten consecutive repetitions each. Changed C++ files pass
clang-format; no Linux or sanitizer validation is claimed. Unrelated baseline `src/walk.cpp`
include ordering remains outside this change.

## 实测 / Measurements

Windows 11，i9-12900H，RTX 3070 Ti Laptop 8 GiB，CUDA 12.8，MSVC 19.44。
顺序运行，以下均为三次测量的进程耗时中位数，不含预热。
Sequential runs; medians of three measured process durations, excluding warmups.

| 场景 / Scenario | CPU | Auto | 解释 / Interpretation |
|---|---:|---:|---|
| 10×512 MiB + 128×4 KiB，2 workers | 1.238 s | 1.065 s | 合成混合长尾约减少 14.0% / synthetic mixed tail, ~14.0% reduction |
| 8 GiB 单稀疏文件，CPU block 1 MiB | 3.725 s | 3.125 s | 约减少 16.1% / ~16.1% reduction |
| 8×64 MiB 普通文件，4 workers | 0.221 s | 0.440 s | 短批次初始化成本仍导致退化 / startup still regresses short batches |
| 8×64 MiB 普通文件，1 worker | 0.396 s | 0.421 s | 未获得收益 / no speedup |

混合长尾每个测量轮次均记录 GPU overflow=1、CPU spill=4；128 个小文件走 CPU，
138 个文件完整 32 字节摘要均与 CPU 对照一致，无回退、无漏读。
Every measured mixed run observed both spill directions (1 GPU overflow, 4 CPU spills).
All 128 small files remained CPU-only; all 138 full digests matched the CPU oracle.
逐轮数据含预热和校验结果见 [CSV](adaptive-routing-benchmark-2026-09-08.csv)。

2000 小文件与旧版本对照：fresh 中位数 0.527→0.486 s，cache 0.323→0.381 s；
所有轮次均无 GPU 初始化。首轮与文件系统噪声明显，不据此宣称速度提升。
Small-file comparison with the historical baseline had noisy first-touch/cache results;
all runs skipped GPU setup. It does not establish a speedup.

稀疏文件不代表冷盘，未清理 OS 缓存；普通文件短批次仅 512 MiB。
GPU 初始化约 0.21–0.23 s，即使异步也不能保证完全隐藏。
同步初始化原型曾出现明显短批次退化，因此改为异步；这些结果不能推广为整盘收益。
Sparse files are not cold-disk evidence; OS caches were not flushed. Startup remains
roughly 0.21–0.23 s and cannot always be hidden. A synchronous prototype regressed short
batches and motivated asynchronous initialization. No universal or whole-drive gain is claimed.

## 复现 / Reproduction

```powershell
$env:SAME_REQUIRE_CUDA='1'
ctest --test-dir build/cuda --output-on-failure
Remove-Item Env:SAME_REQUIRE_CUDA
ctest --test-dir build/goal-cpu --output-on-failure
ctest --test-dir build/goal-cpu -R '^(adaptive_routing|async_scan|hash_retry)$' --repeat until-fail:10 --output-on-failure
python tools/adaptive_dispatch_benchmark.py --exe build/cuda/same.exe --output .cache/mixed-reproduction --trials 3
python tools/file_dispatch_benchmark.py --exe build/cuda/same.exe --output .cache/files-reproduction --workers 1,4 --block-mib 1 --backends cpu,auto,cuda --trials 3
python tools/sparse_dispatch_benchmark.py --exe build/cuda/same.exe --output .cache/sparse-reproduction --gib 8 --trials 3
```

基准仅创建新的输出目录；混合负载工具保存配置、完整摘要对照、原始 profile 和独立数据库。
Benchmark outputs must be new directories. The mixed tool records configuration, full digest
oracles, raw profiles and independent databases. Machine-local raw artifacts are intentionally
not committed; the portable CSV preserves all eight mixed runs including warmups.
