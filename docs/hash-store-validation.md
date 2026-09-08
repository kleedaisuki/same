# Hash routing and store validation / 哈希分流与数据库验证

## Scope / 范围

2026-09-08，Windows 11，Intel Core i9-12900H，RTX 3070 Ti Laptop 8 GiB，驱动572.61，
CUDA12.8，MSVC19.44 x64 Release，SQLite3.53.4，BLAKE3 1.8.2上游运行时SIMD。
基线为 `fac81a3` 的独立源码构建，不复用来源不明的旧程序。没有增加运行时依赖。

Baseline is a separate source build of `fac81a3`, not an unverified historical executable.
No runtime dependencies were added. These measurements describe this machine and synthetic workloads.

## Store protocol / 数据库实验协议

```sh
cmake --build build/cuda --target store_benchmark
store_benchmark --rows 100000 --trials 5 --shape pairs --lookup fused
store_benchmark --rows 100000 --trials 5 --shape single --lookup fused
store_benchmark --rows 100000 --trials 5 --shape unique --lookup fused
```

Same `tools/store_benchmark.cpp` was compiled against both versions; the baseline omits
`SAME_STORE_FUSED_LOOKUP`. Each of five paired rounds ran baseline/legacy, new/legacy, new/fused,
reversing the order on odd rounds. Each process used `--trials 1`; all variants used the same shape
and a 2MiB SQLite page cache. Do not run performance sampling concurrently with builds or other probes.

同一基准源码分别链接两版，基线不定义 `SAME_STORE_FUSED_LOOKUP`。每轮独立新库，
扫描时间包含事务提交，不包含schema初始化；不清除OS缓存。计时内旧缓存路径只做生产
所需的完整文件戳比较，新路径检查条件更新结果；计时外逐条检查完整持久记录。
其他阶段校验完整候选、分组顺序和成员数。基准生成的记录在内存中，不包含目录遍历、
文件读取或哈希，因此不能把数据库降幅称为整盘加速。

Fresh databases are not cold OS caches. Commit is timed, schema creation is not. Legacy cache checks
match the production stamp-only contract; full records are audited outside timing in both modes.
Other phases verify complete records, order and counts. Dataset generation is untimed and memory-resident.

Raw measured phase values: [store CSV](store-benchmark-2026-09-08.csv), 270 observations, all verified.

## Store results / 数据库结果

五轮中位数，毫秒；下表新版本为生产fused路径。 / Five-round medians in milliseconds; new uses fused lookup.

| Shape / 形状 | Phase / 阶段 | Baseline | New | Time reduction / 耗时下降 |
|---|---|---:|---:|---:|
|50% paired / 半数重复对|Cache-hit scan / 缓存扫描|331.528|273.312|17.6%|
|One bucket / 单大桶|Cache-hit scan / 缓存扫描|327.707|278.886|14.9%|
|All unique / 全唯一|Cache-hit scan / 缓存扫描|331.602|281.969|15.0%|
|50% paired|Candidate traversal / 候选遍历|84.647|68.544|19.0%|
|One bucket|Candidate traversal / 候选遍历|139.631|83.636|40.1%|
|50% paired|Match traversal / 匹配遍历|35.132|24.961|29.0%|
|One bucket|Match traversal / 匹配遍历|67.491|25.064|62.9%|

消融对照（ablation）：新版保留旧API时，缓存扫描中位数为329.779/321.410/325.353ms，
说明主要缓存收益来自合并操作，而不是编译版本或查询读取改写。首次写入没有稳定加速：
基线215.760/233.824/222.303ms，新版219.070/236.956/219.867ms。全唯一的候选扫描
17.052→17.465ms，没有收益；不能宣称所有查询都更快。`visit_unique` SQL未修改。

Retained-API ablation isolates most cache gains to the fused operation. Initial writes show no clear
improvement. All-unique candidate enumeration does not benefit. `visit_unique` SQL remains unchanged.
Five rounds and alternation reduce order bias but do not establish confidence intervals or generalize
to cold disks, other path lengths, storage, memory budgets or file distributions.

## End-to-end checks / 端到端检查

```sh
python tools/scan_benchmark.py --exe BASELINE --exe build/cuda/same.exe --output NEW_DIRECTORY --files 10000 --trials 5 --backend auto
python tools/sparse_dispatch_benchmark.py --exe build/cuda/same.exe --output ANOTHER_NEW_DIRECTORY --gib 8 --trials 3
```

10,000 mixed-layout small files, five alternating rounds; median process seconds:

| Mode / 模式 | Baseline | New |
|---|---:|---:|
|Rehash / 重算|1.779|1.783|
|Cache hit / 缓存命中|1.212|1.225|

全部20次分组与参考一致，短批次 `gpu_setup_ms=0`。缓存阶段数据库工作中位数
66.832→59.969ms，但端到端没有可辨认的改善；差值约1%，不能宣称整盘加速。
基线第一次重算14.580s，随后约1.7–1.8s，提示首次元数据/系统缓存存在严重顺序干扰；
保留该异常值而非事后删除。此实验重点是功能与短任务不探测GPU的回归检查。

All 20 group oracles passed with zero GPU setup. Cache-phase database work fell from 66.832 to
59.969ms, but total process time did not clearly improve. The first baseline rehash took 14.580s,
unlike later 1.7–1.8s rounds: first-touch metadata/cache effects confound attribution. It is retained.

8GiB sparse zero file, one worker, three alternating measured rounds after warmup:

| Update block / 更新块 | CPU seconds | Auto seconds | Auto decision / 实际选择 |
|---|---:|---:|---|
|1MiB|3.472|3.910|CPU, after 100–117ms probe|
|16MiB|4.396|2.952|GPU, including 279–296ms setup|

所有完整32字节摘要和逻辑读取计数相同。16MiB场景端到端约下降32.9%，但这是稀疏
逻辑零读取，不是冷SSD吞吐，也不是相对旧版本的32.9%改进。1MiB探索后仍走CPU且更慢；
探测只能解释部分差值，剩余噪声/系统影响未隔离。保留该负结果，不通过隐藏探测费
或强制使用GPU美化结论；已知这种工作形状可显式选择CPU避免探索。

All complete digests and read counts match. The 16MiB case is ~32.9% faster than current CPU mode,
not than the old release. Sparse logical reads do not measure cold SSD throughput. The 1MiB auto
case loses after probing; probe cost explains only part of the gap. Do not hide the negative result
or infer that every automatically probed workload improves; explicit CPU avoids exploration.

The small-file and sparse raw reports are retained under `.cache/goal-results/scan` and
`.cache/goal-results/sparse`, respectively; generated datasets/databases are intentionally not committed.

## Correctness gates / 正确性门槛

- Full stamps, binary paths, uint64 sizes, repeated hits, mismatches, stale pruning and rollback reuse.
- Ordered multi-bucket traversal, callback exception cleanup and nested reads of distinct tables.
- All 16 pure lane/queue-selection snapshots, invalid timings and conservative amortization boundaries.
- Completion-ring capacity, move-only captures, exception delivery and reusable slots for hash hints.
- Existing CPU/CUDA digest vectors, whole-file retry and integration output/cache tests remain required.

保留schema v1、FULL同步、DELETE日志、单连接拥有者与整轮原子提交。GPU接受与否由实测证据
决定，不以“GPU必须被使用”作为通过条件。纯队列测试无需GPU；实际窃取测试仅在实机接受
auto GPU时执行，因此不能把纯策略测试说成全部线程交错已被穷尽验证。

Schema v1, FULL synchronization, DELETE journaling, single-owner access and atomic scans are retained.
Actual GPU stealing coverage is conditional on hardware profitability; pure selection tests do not
exhaust thread interleavings. See [dispatch contract](auto-dispatch.md) and [store contract](store-optimization.md).

本机最终硬件测试日志：四线程混合样本CPU18.970ms、mixed20.513ms，正确拒绝GPU；
另一个双线程/单文件校准实例接受GPU，实际执行了阻塞GPU后CPU窃取的容量一回归。
Final hardware logs rejected the four-lane mixed sample (CPU18.970ms, mixed20.513ms). A separate
two-worker, single-file calibrated instance accepted the GPU and executed the capacity-one stealing
test while its GPU lane was blocked. The two cases distinguish policy rejection from missing hardware.

CPU-only and required-CUDA MSVC Release builds both passed 14/14 CTest tests. Changed C++ files pass
`clang-format --dry-run --Werror`, and the patch passes `git diff --check`. The repository-wide
`format-check` still reports the pre-existing include-order issue in `src/walk.cpp` (also reproduced
on the `fac81a3` baseline); that unrelated file was not reformatted. No new Linux/sanitizer run is claimed.

两种构建均14/14通过；本轮变更格式检查通过。全仓库格式检查仍受基线已有的walk.cpp
头文件顺序问题影响，未混入无关格式修改；没有声称本轮运行Linux或Sanitizer。
