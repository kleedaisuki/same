# Evidence-based CPU/GPU dispatch / 基于证据的 CPU/GPU 分派

> 历史阶段记录；当前实现契约见 [auto-dispatch](auto-dispatch.md)，最新分派实验见 [dispatch-results](dispatch-results.md)。
> Historical-stage record; follow those documents for the current policy and latest measurements.

## Status and scope / 状态与范围

Design proposal only; no production changes. Reviewed `src/resources.cpp`, `src/config.cpp`, `tools/gpu_benchmark.cpp`, and `docs/gpu-benchmark-extended-2026-09-07.csv` on 2026-09-07. Objective: minimize end-to-end scan latency without changing hashes, file checks, resource limits, or failure fallback. GPU utilization is not an objective.

本机已有单工作线程、1 MiB update 分块测试显示，从 4 KiB 到 256 MiB 所有采样点 CPU 都快于优化 CUDA。以中位数计，16 MiB CPU 约 3.832 ms、CUDA 7.472 ms；256 MiB CPU 63.033 ms、CUDA 120.925 ms。这反驳“超过 16 MiB 就值得 GPU”，但没有证明其他机器、块大小或并发下 GPU 永远无收益。当前基准按后端分组执行，CPU/GPU 时间未交错，且不含初始化、文件 I/O 和并发；不要把它当普适交叉点模型。

## Model / 模型

For a host-resident file of n bytes, block size b and effective device capacity c:

`T_cpu ≈ A_cpu + n / R_cpu(k, SIMD, memory contention)`

`T_gpu ≈ A_gpu + ceil(n / min(b,c)) * L_batch + n / R_gpu + transfer(n) + contention(k)`

This is a diagnostic model, not a proven linear law. ROOT/tail handling, exact block boundaries, transfer batching, device contention, pageable-memory staging, and CPU frequency change coefficients. File size alone is insufficient. For k simultaneous CPU workers versus k GPU workers, compare batch makespan (throughput), not the sum of per-worker latency. Mixed CPU/GPU execution can outperform either homogeneous option but requires separate measurement.

对本程序，磁盘读取是共同前置工作；校准内存输入可以隔离计算选择，却不能保证整盘加速。校准必须使用生产 `Hasher::update` 分块和 `finish`，把主机与设备传输（host-device transfer）与同步包含进去；单独 kernel 时间不足以决策。

## Minimal implementable policy / 最小可实施策略

### Configuration compatibility / 配置兼容

| Setting | Proposed contract / 建议契约 |
|---|---|
| `backend="cpu"` | Never initialize or calibrate CUDA; preserve exact CPU path |
| `backend="auto"` | CUDA is eligible only after measured benefit; no winning evidence means CPU |
| `backend="cuda"` | Explicit preference bypasses *performance* calibration, retains existing availability/allocation/ComputeError CPU fallback |
| `gpu_min_bytes` | Preserve parsing and nonnegative range; hard eligibility floor, not a promise that larger files use GPU |

Do not reinterpret `cuda` as fail-if-unavailable: existing semantics allowed fallback. Document that explicit `cuda` still respects an explicitly configured size floor; `gpu_min_bytes=0` requests all-size CUDA eligibility. Avoid silently removing a supported key. Default auto floor may stay 16 MiB for compatibility, but measured selection must still be allowed to choose CPU for every size. A later default-floor change is a tunable behavior change, not a digest/database migration.

Prefer a small immutable policy object owned by Resources, not an ever-growing collection of booleans on Worker. It must distinguish `unavailable`, `calibration rejected`, `CPU size policy`, and `runtime failure`; only the last belongs in fallback counts. Report calibration elapsed, sample configuration, and selected mode explicitly.

### Calibration procedure / 校准过程

1. CPU mode skips everything. Auto creates one candidate CUDA backend with the **actual** `block_bytes` and `device_memory_bytes / workers`; do not calibrate a larger hidden device allocation. Reuse an existing worker host buffer for deterministic generated input, rather than allocating n-byte fake files. Repeated updates can feed a logical long file with O(block_bytes) memory. Do not read user files twice for calibration.
2. Warm both CPU and CUDA once, compare resulting full digests. A mismatch must surface as a correctness error, not merely a slow-GPU rejection. Catch only backend availability/ComputeError for safe performance fallback; do not swallow unrelated programming errors.
3. Sample at least one update, eight updates, and the eligibility floor if the floor exceeds that span. Execute CPU/GPU in alternating order (AB/BA), three paired rounds. Sample `hasher + updates + finish`, synchronously timed by steady clock; consume complete digest. First estimate repetition count so each timed sample spans roughly 1–5 ms. Calibration deadline is checked between operations; do not claim hard timeout of a blocking GPU call.
4. Suggested initial margin: GPU median ≤ 0.80 × CPU median **and** slowest GPU round < fastest CPU round. Twenty percent is an engineering guard band, not a statistical confidence interval. Near ties or jitter select CPU. Save samples for diagnostics; do not fabricate a crossover between unevaluated regimes.
5. Only if GPU wins serially, measure aggregate makespan with the planned number of concurrently active GPU workers and matching CPU worker count, using the same per-worker capacity/buffers. Warm outside timing; use a start gate so thread startup is excluded, but include real hashers and transfers. If arbitrary 256-way calibration is too costly, cap GPU lanes and enforce that same lane cap in runtime. Testing four lanes but deploying twenty invalidates the comparison.
6. Keep the decision fixed for this scan. No per-file timers, global locks, or repeated benchmark on the hot path. A future version may use smoothed runtime observations, but CPU/GPU timings of different files are confounded and should not be compared as paired experiments.

A first implementation may skip concurrency probing when GPU loses serially. This is a **conservative policy**, not an optimal heterogeneous scheduler: it may miss GPU benefits when many CPU workers saturate memory bandwidth. State this limitation explicitly. For the present machine's roughly 2× single-worker loss, this is a reasonable low-complexity first step, subject to the requested concurrent benchmark.

### Avoid false extrapolation / 避免错误外推

One large probe must not imply all larger sizes are GPU wins. Minimum acceptable v1 choices:

- Conservative global gate: CPU everywhere unless GPU wins each representative streaming probe plus concurrent gate. This is cheap and may miss special GPU-winning intervals.
- Better: fixed small size buckets with independent winning bits; unknown bucket → CPU. Each bucket also respects `gpu_min_bytes`. A last unbounded bucket is an explicitly assumed steady-state streaming model, validated by at least two separated multi-block sizes and documented as such.

Do not implement a complex online regression or machine-learning scheduler before simple probes have demonstrable value. Never inflate arithmetic work simply to make GPU utilization attractive: BLAKE3 has fixed semantics and redundant math worsens user latency.

## Initialization economics / 初始化成本

Let `C` be extra calibration/setup time and `d` expected per-file savings. Break-even count is `C/d`; a 100 ms calibration for a 10 µs saving requires 10,000 eligible files. CUDA context initialization itself may dominate a tiny scan or a cache-hit scan. Therefore:

- Measure and report probe time separately from CUDA initialization; do not hide it outside Elapsed.
- Set a modest *soft* aggregate calibration budget (e.g. 50–100 ms) excluding uninterruptible context setup. Budget exhaustion means CPU, not evidence of GPU inferiority.
- CPU-only and forced-CUDA paths need no performance probes.
- Lowest-complexity first version calibrates eagerly once. Explicit limitation: it pays unnecessary initialization on cache-hit scans.
- Preferred follow-on: lazy calibration only upon first eligible cache miss. Single-owner initialization and other jobs staying on CPU avoid a stampede; do not block metadata/DB coordinator while waiting for calibration. This complicates Resources ownership and should be a separately tested step.
- Persistent calibration cache is not needed for v1. If later added, key by executable algorithm version, CPU SIMD capability, GPU identity, driver/runtime, block size, device capacity and concurrency. Treat stale entries as hints and atomically replace; they are not file-hash cache records.

## Validation gates / 验证要求

Unit-test policy decisions using injected timings (GPU always loses, clear win, jitter, equality, missing sample, deadline, floor boundaries) without requiring CUDA. Integration tests must verify auto-CPU selection leaves identical digests/groups/cache reuse and does not inflate error fallback counts. Forced CUDA remains usable for benchmark and kernel regression.

Benchmark all-CPU versus auto versus forced CUDA with block sizes 64 KiB/1 MiB/16 MiB and workers 1/4/8/20, within actual memory budgets; include mixed-size and cache-hit scans. Distinguish serial microbenchmark, parallel compute benchmark, and end-to-end scan. Retain raw paired data and repeat order. Reject demonstrated end-to-end regression absent a documented trade-off; if both modes are I/O-bound, prefer CPU to avoid device overhead rather than calling it a GPU success.

Adversarial considerations: background GPU load can reject a normally fast GPU (safe but suboptimal); background CPU load can falsely favor GPU (hence margin and repeats); warming lowers observed setup cost (measure setup separately); thermal/power state changes invalidate a frozen decision; repeated identical synthetic buffers fit caches differently from real I/O (run independent buffers per concurrent worker, retain end-to-end control).

## Production and research evidence / 工程与学术依据

- [NVIDIA CUDA Best Practices Guide](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html) explicitly requires transfer overhead in offload decisions. Applies directly to host-resident hash input; it supplies no universal byte threshold.
- [StarPU features](https://starpu.gitlabpages.inria.fr/features.html) demonstrates a mature runtime using automatically tuned performance models across heterogeneous resources. Adopt its principle of measured task cost, not the full runtime dependency for this small scanner.
- [StarPU publications](https://starpu.gitlabpages.inria.fr/publications.html) lists the peer-reviewed 2011 CCPE system paper and automatic-calibration work. History-based task models motivate stable size/config classes, but numerical kernels' reported prediction accuracy cannot be transferred to filesystem scans.
- [Gavel, OSDI 2020](https://www.usenix.org/conference/osdi20/presentation/narayanan-deepak) uses heterogeneous throughput profiles in scheduling. Cluster deep-learning workloads differ substantially; its useful lesson is to model actual resource-specific throughput rather than count accelerators.
- [Gonthier et al., JPDC 2025](https://doi.org/10.1016/j.jpdc.2025.105170), also listed by StarPU, studies locality-aware GPU/out-of-core scheduling. Frontier relevance: placement and memory residency change task cost. Here inputs start on CPU and GPU outputs are not consumed by another GPU task, weakening the usual locality advantage.

These sources support the design mechanisms, not a claim that auto-calibration is already faster in same.

## Read-only profiler availability / 只读工具检查

Verified locally: Nsight Systems 2026.1.3.243; Intel VTune 2025.3.0 build 630104; Windows Performance Recorder 10.0.26100 reports **not recording**. Only version/status commands ran. No capture was started, no privileges/driver settings changed. Future per-process Nsight timing can distinguish transfers, launches, and synchronization; optional system-level traces require separate deliberate scope.

## Follow-up: one GPU lane in auto / 后续：auto 单 GPU 通道

New parent-provided measurements report a GPU win at 64 MiB input with 16 MiB updates (about 7 ms versus CPU 15 ms), but not a robust 20% win with 1 MiB updates. These numbers are not independently reproduced by this design review; they reinforce calibrating actual block size.

A simple conservative deployment after a successful serial calibration is **one GPU-capable worker plus workers−1 CPU workers**, all consuming the existing bounded shared FIFO. `workers=1` remains one GPU-capable worker, not two workers. Explicit `cuda` can retain the existing all-GPU-worker preference. This bounds device concurrency to the calibrated single instance, lowers device allocation/setup, and does not need a second queue, cross-queue stealing, or per-file lock-based dispatcher.

This is preferable to silently deploying many GPU contexts from a single-instance win. It is not equivalent to measuring an optimal heterogeneous pool: CPU workers still compete for host memory bandwidth with staging/transfers; a GPU worker may dequeue a small CPU-routed file while CPU workers dequeue large files. A shared FIFO provides progress but not optimal placement. Keep those limitations explicit and measure mixed-pool makespan against the same total worker count on CPU. Report initialized GPU lanes, CPU-only workers, size-routed files and runtime fallback separately.

Do not add a GPU-specific queue just to improve utilization before evidence warrants it. Such a queue needs bounded admission, no eligible-task starvation, cancellation and cross-resource load balancing. A single shared queue with CPU-capable workers can process every file and preserves the simple ownership/error model.

Decision gate: if one-GPU mixed concurrency shows no stable end-to-end gain over equal-worker CPU, auto should remain CPU for that configuration; explicit CUDA stays available for experiments. Requiring a full parallel calibration at every startup is more expensive and complex and can regress short/cache-hit scans. A serial gate plus one GPU lane is a defensible *conservative heuristic*, not a guarantee of lower elapsed time. The concurrent benchmark matrix, not GPU utilization, decides whether the heuristic is ready to adopt.

### Pending-file shape restriction / 待处理文件形状约束

The lazy mixed probe uses `lanes=min(pending_files, workers)` and `jobs=min(pending_files, 4*lanes)`, two blocks per job. It never invents more schedulable files than the buffered workload. With one lane it reuses the existing eight-block serial evidence rather than launching another probe. Savings normalization uses measured total bytes: `jobs*2*block_bytes` for mixed probes, `8*block_bytes` for serial evidence.

这是同大小任务的近似模型，不是任意文件大小分布的性能证明；当文件数等于通道数，每通道只能领取一项，GPU 无法靠不存在的额外文件证明吞吐收益。不同大小文件可能改变尾部完成时间；保守样本和两倍成本门限降低风险，但不提供最优调度或必然加速保证。
