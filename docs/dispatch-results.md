# 调度实验结果与边界 / Dispatch results and limitations

2026-09-07，Windows 11，20 逻辑 CPU，RTX 3070 Ti Laptop GPU 8 GiB，驱动 572.61，CUDA 12.8，MSVC 19.44 Release。以下是本机证据，不是通用阈值。所有计时应包含 CPU/GPU 同一完整摘要语义，而非仅设备内核。
These are machine-specific observations, not universal thresholds. Compare full digest semantics rather than device-only kernels.

## 1. 必须保留失败的起点 / Preserve the unsuccessful starting point

最早融合原型虽然比旧 CUDA 快，但在 1 MiB 更新块下 4 KiB、64 KiB、1 MiB、16 MiB 输入均仍慢于官方 SIMD CPU；因此“融合有效”不等于“值得卸载”。原始数据见 `docs/gpu-optimization.md`、`docs/gpu-benchmark-2026-09-07.csv`。后续基于 Nsight 的加载、归约与尾部改进见 `docs/gpu-profiling.md`。这些是不同实现阶段，不能混成同一版本成绩。
The initial fusion prototype improved historical CUDA yet still lost to CPU across all measured sizes. Subsequent profiled loading/reduction/tail changes belong to a later implementation; do not merge version-specific results.

## 2. 单实例胜出，多实例反转 / One instance wins, many reverse

主机驻留同一只读 64 MiB 输入，16 MiB 块，每 worker 两份任务，预热一次，交替三轮中位数（毫秒）：
Shared host-resident 64 MiB input, 16 MiB blocks, two jobs per worker, one warmup, three alternating trials; median milliseconds:

| workers | CPU | 全 CUDA / all CUDA |
|---:|---:|---:|
|1|32.63|13.35|
|4|34.62|49.16|
|8|51.81|98.15|
|20|99.29|249.50|

原始来源 `.cache/perf/dispatch-matrix/*.jsonl`；旧工具固定每 worker 等量任务。1 MiB 块时单 GPU 也略慢（CPU 30.79 ms、GPU 32.48 ms）。工具：`tools/dispatch_benchmark.cpp`，契约：`docs/dispatch-benchmark.md`。
Historical matrix used fixed per-worker work. At 1 MiB blocks even one GPU instance lost slightly. It does not justify routing every worker to CUDA.

## 3. 一个 GPU 加 CPU 共享任务 / One GPU plus CPU shared jobs

当前工具所有计时模式采用统一动态任务索引。64 MiB 输入、16 MiB 块、总任务 `workers*8`，每 worker 固定一次预热；三轮中位数：
Current tool uses the same dynamic job index for every timed backend. Total jobs equal workers times eight; fixed per-worker warmup; medians:

| workers | CPU ms | mixed ms | GPU 完成任务 / completed jobs |
|---:|---:|---:|---|
|4|145.41|107.19|14/14/15 of 32|
|8|192.22|165.73|17/17/17 of 64|
|20|378.24|334.85|28/23/24 of 160|

来源 `.cache/perf/dispatch-mixed-final/*.jsonl`，每次所有32字节摘要校验。初步 `.cache/perf/dispatch-mixed` 的20线程 CPU/mixed 约726/718 ms，说明明显时变噪声；最终样本不能掩盖它。共享输入可受末级缓存影响，且没有文件 I/O。
All full digests were checked. Preliminary 20-worker measurements were materially slower for both variants, exposing temporal noise. Shared input may benefit last-level cache; this is not file I/O.

## 4. 真实普通文件路径：小批量不值得初始化 GPU / Ordinary-file end-to-end path

8 个 64 MiB 文件，4 组副本，总512 MiB，`--rehash`，暖 OS 缓存；进程时间三轮中位数（秒）：
Eight 64 MiB files, four duplicate pairs, forced rehash, warm OS cache; median process seconds:

| workers | CPU 1MiB | auto 1MiB | CPU 16MiB | auto 16MiB |
|---:|---:|---:|---:|---:|
|1|0.413|0.390|0.476|0.463|
|4|0.237|0.228|0.333|0.312|
|20|0.237|0.230|0.410|0.421|

来源 `.cache/perf/file-dispatch-lazy/report.json`，工具 `tools/file_dispatch_benchmark.py`。这里 auto 的轻量 CPU 路径避免不必要设备初始化；不能把 auto 与 CPU 的小差值认作 GPU 加速。该表包括文件打开、读取、比较、报告等端到端成本。
Auto avoids unnecessary GPU initialization for this small workload. Small auto/CPU differences do not establish GPU acceleration. These process times include file operations, comparisons, and reporting.

## 5. 8 GiB 稀疏文件：摊销初始化后的有限正证据 / Sparse 8 GiB file

`.cache/perf/sparse-dispatch-pinned/report.json`：单worker、单个8 GiB零稀疏文件，完整 SQLite 摘要与CPU一致，预热后交替三轮；进程时间中位数：
One worker, one sparse zero file, full SQLite digest matching CPU; alternating trials after warmup:

| case | process seconds |
|---|---:|
| CPU, 1 MiB |3.217|
| auto, 1 MiB |3.351|
| CPU, 16 MiB |3.625|
| auto, 16 MiB |2.818|

16 MiB auto 包含约0.28秒初始化，仍在此工作负载优于CPU。但稀疏零区主要是**逻辑读取**，不是普通SSD上8 GiB实际数据的物理冷盘读取；也不证明所有文件分布同样受益。历史脚本 `.cache/perf/sparse_dispatch_pinned.py` 复用已有数据/状态，下面的新工具每case采用独立数据库，属于更明确的复现实验，不保证逐毫秒复制历史成绩。
The 16 MiB auto run includes roughly 0.28 s setup yet wins here. Sparse zeros are logical reads, not 8 GiB physical cold-SSD data. The historical script reused state; the new harness uses fresh state per case, so exact historical timings are not promised.

```powershell
python tools/sparse_dispatch_benchmark.py --exe build/cuda/same.exe --output .cache/perf/sparse-repeat --gib 8 --trials 3
```

输出目录必须不存在。Windows 先 `fsutil sparse setflag`，再扩展逻辑长度；非Windows使用原生truncate（文件系统是否实际保留空洞依实现）。不清除页缓存，不扫描用户数据；导入模块无副作用。日志和每case数据库归档在扫描根外。CPU完整摘要作为参考，核验单文件、全部哈希、零缓存、逻辑读字节数及全部32字节摘要。`auto` 可选择CPU，因此必须检查profile的实际路由和初始化字段，不能只看配置名。
Output must be new. Windows marks sparse before truncation; other systems use native truncation, with filesystem-dependent hole allocation. No cache purge or user-data scan; importing has no side effects. Logs/state archives stay outside the scan root. Full digests and counts are checked; inspect actual routing rather than assuming auto means GPU.

**结论 / Conclusion:** 限制 GPU 实例、延迟初始化、保留 CPU 优势路径，并让真实端到端证据决定卸载；不以GPU利用率作为成功指标。
Limit GPU instances, initialize lazily, preserve fast CPU paths, and decide using end-to-end evidence rather than GPU utilization.

## 6. 验证 / Verification

- Windows CUDA: 13/13 CTest checks with `SAME_REQUIRE_CUDA=1`.
- Windows CPU: 13/13 CTest checks.
- Linux GCC 13.3 Debug ASan/UBSan: 13/13 CTest checks.
- CUDA compute suite: memcheck 0 errors; racecheck 0 errors/0 warnings.
- Registered-input lifetime, complete retry, queue capacity one, mixed-probe gates, lazy short-batch
  no-probe behavior, official BLAKE3 vectors, and full 8GiB CPU/CUDA digest equality are covered.
- The generalized sparse harness also passed a 1GiB, one-trial functional self-check; this is not a
  new performance claim. Raw self-check: `.cache/perf/sparse-tool-selfcheck/report.json`.

这些结果证明被测场景中的正确性与收益范围，而不是未知硬件、所有文件大小及温度条件的保证。
Correctness and speed evidence cover tested cases, not every hardware, size or thermal condition.
