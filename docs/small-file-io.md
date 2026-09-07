# 小文件 I/O 优化设计 / Small-file I/O design

## Scope / 范围

Read-only design review of the baseline `src/application.cpp`, `src/files.cpp`, `src/compute_cuda.cu`, and `src/store.cpp` on 2026-09-07; concurrent implementation may supersede the described baseline. This document is a proposal, not evidence of implemented or measured speedups.

本次针对 245,091 个文件、9.16 GiB、Scan 22.34 分钟的样本。平均文件约 39.2 KiB，但平均值不能替代大小分布。主线程 Scan 包含遍历、元数据、SQLite，不能据此断言存储设备慢。Hash work 是跨线程累计墙钟，不能从总时间简单相减估算 CPU 成本。

## Baseline evidence / 基线证据

| Baseline path | Actual work / 实际工作 | Implication / 含义 |
|---|---|---|
| `scan` → `stamp_path` | Creates and closes a `FileReader` | Metadata-only work requests GENERIC_READ and sequential-scan hint too |
| `hash_file` | New reader; `unchanged` before and after hashing | Uncached file has four opens: initial metadata, content reader, two path-binding probes |
| `unchanged` | Fresh handle stamp **and** fresh path stamp | Detects object changes and path replacement separately; not a filesystem snapshot |
| Windows `stamp` | Basic + Standard + Id information calls | At least three queries per stamp; unsupported Id falls back to legacy identity |
| `FileReader` constructor | Legacy information query + file type check | Additional validation query for every open |
| `CudaHasher::finish` | H2D tail, one-thread kernel, D2H descriptor, sync | Even an empty or tiny file incurs GPU launch/sync in baseline |
| `CudaCompute::equal` | Two H2D transfers + kernel + D2H flag | Host-resident small comparisons need not traverse PCIe |
| `Store::cached/save` | Prepare/finalize SQL for every file | Independent serialized cost, do not mistake it for filesystem I/O |

Baseline normal no-change hashing performs five explicit `stamp()` calls per uncached file (one scan + two each in before/after checks), hence 15 normal Windows extended-info calls plus constructor checks. Final result validation and duplicate comparison add more. These are API call counts, not physical I/O counts: system cache can satisfy many calls.

## Implementation order / 实施顺序

1. **Expose latency and remove serialized scheduling first.** Measure metadata, lookup/save, queue residence, worker file opens/read/hash separately. Parallel metadata hides latency but does not reduce call count. Reuse prepared statements under one DB owner.
2. **CPU route tiny inputs; GPU route sufficiently large batches.** Keep public digest/cache representation unchanged. CPU routing is a normal policy, not a CUDA failure and must not increment fallback counters. Select by actual bytes, not suffix (`.pdf`, `.zip`) because the operation is byte identity, not semantic decoding.
3. **Reuse an owned open reader across metadata → cache decision → hash where practical.** Move-only job payload carries reader and fresh expected stamp. Cache hit closes it. Admission bounds total outstanding handles; pending readers cannot scale with tree size. Preserve start/end freshness checks and path binding probes. This saves initial close/reopen without claiming an atomic snapshot. ComputeError fallback must restart or rewind before retry; a half-consumed reader cannot silently be reused.
4. **Batch only after crossover measurements.** Multi-file GPU packing must preserve per-file BLAKE3 chunk counters, root flags, descriptors, lengths and zero-length inputs; concatenating files is incorrect. Bounded bytes, file count, and maximum waiting time are all required. Packing adds a host copy; it is worthwhile only if transfer/launch savings exceed it. Small buffered synchronous reads plus CPU hash remain the control.

## Correctness boundary / 正确性边界

Safe candidates:

- Use directory enumeration attributes for preliminary link/type filtering if the platform API provides them, but still validate the opened object. Enumeration metadata is not authoritative cache identity.
- Merge constructor validation and the *initial* metadata acquisition into a single validated operation where metadata classes allow it. Do not cache future `stamp()` results.
- A metadata-specific open may request minimal rights rather than GENERIC_READ, but evaluate compatibility: previously unreadable files produced an error even on cache hits. Silently accepting them changes the observable error policy.
- Replace GPU tiny comparisons with exact host byte comparison; keep before/after file checks unchanged.

Not safe as local optimizations:

- Delete path checks because an open handle is stable: rename-and-replace leaves the handle on the old object.
- Use size/mtime alone for cache hits: identity and change-time matter; preserve fallback identity formatting across versions.
- Skip hashing unique-size files while persisting the same complete-digest record contract. A two-pass size filter is a valid *new architecture*, but requires explicit absent-digest state and unchanged output/cache semantics; it also delays work and stores a whole metadata index.
- Hash prefixes as final evidence or trust equal digests instead of the existing byte comparison.
- Read whole files based only on extension or unbounded size; retain bounded buffers.
- Promise detection of every adversarial concurrent write. The existing checks are not a snapshot, and timestamps are filesystem dependent.

## Strategy and trade-offs / 策略与权衡

| Input class | Initial policy hypothesis / 初始策略假设 | Trade-off / 权衡 |
|---|---|---|
| Empty / tiny | CPU digest and compare | Avoid launch/sync; boundary tests required |
| Small, warm cache | Buffered read + CPU; bounded concurrency | Avoid packing and paging overhead; likely metadata dominated |
| Medium | Benchmark CPU versus GPU crossover | Threshold is hardware/backend dependent, not universal |
| Large | Streaming GPU hash; sequential hint | Amortize transfer/launch; keep RAM bounded |
| HDD tree | Limited concurrency and local directory order | Excess queue depth may increase seeks |
| SSD/NVMe tree | Bounded concurrent metadata/reads | More overlap, but saturation and CPU/antivirus effects possible |
| Sparse/compressed/encrypted files | Ordinary logical reads initially | Do not bypass filesystem semantics or interpret physical extents as file bytes |

Keep Windows buffered I/O as default. `FILE_FLAG_NO_BUFFERING` imposes alignment constraints and loses normal system caching; it is not a small-file shortcut. `FILE_FLAG_SEQUENTIAL_SCAN` is documented mainly for large sequential accesses; benchmark its effect rather than assuming it benefits tiny files. Do not repack the user's corpus into archive/container files: preprocessing would add a full read/write pass and invalidation complexity.

A bounded small-content cache for later duplicate comparison is possible but lower priority: current comparison reads only 329 MiB and takes 4.64 s. Retaining all content to avoid that work would attack a minor phase, increase memory footprint, and still require fresh path validation. Existing OS cache already offers opportunistic reuse.

## Benchmark gates / 基准验收

Generate reproducible, non-user synthetic roots with a fixed seed and a manifest of expected duplicate groups. Record executable/build/config, OS, filesystem, storage model, GPU/driver, cache condition, and antivirus state (do not disable protections automatically).

- Size points: 0, 1, 1023/1024/1025, 4 KiB, 16 KiB, 64 KiB, 256 KiB, 1 MiB, 16 MiB; also block-size ±1 boundaries.
- Corpus shapes: fixed total bytes/varying file count; fixed count/varying bytes; shallow-wide and deep; mixed distributions with rare giant files.
- Duplicate rates: 0%, 10%, 90%; hardlinks, empty files, long Unicode names, reparse exclusions.
- Cache: first application scan; repeated `--rehash` warm filesystem cache; application hash-cache hit. Do not call a newly created corpus a guaranteed cold-cache run.
- Concurrency: 1, 2, 4, 8, 20; fixed bounded queue and memory; compare baseline and new code on identical roots/configs with order alternation, at least 3 repetitions.
- Measure elapsed, files/s, logical bytes/s, open/query/read counts, stage timings, peak working set/handles, GPU transfers/launches/synchronizations. Logical reads do not equal physical disk bytes.
- CPU/GPU microbench: identical digest vectors and input segmentation; include end-to-end host-to-device transfers and finish, not kernel time alone. Threshold acceptance needs confidence intervals or at least ranges, and no meaningful large-input regression.
- Mutation tests: same-size rewrite, truncation/growth, rename replacement between metadata/read, replacement during comparison, CUDA failure retry; preserve error propagation and transaction rollback.

Acceptance is exact output/cache compatibility plus measured improvement in small-file end-to-end workloads; GPU utilization alone is not a success metric.

## External evidence / 外部依据

1. [Microsoft: File buffering](https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering): unbuffered I/O bypasses cache and imposes alignment constraints. Supports retaining buffered reads, not a universal performance prediction.
2. [Microsoft: CreateFile2 extended parameters](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/ns-fileapi-createfile2_extended_parameters): sequential-scan hint is primarily described for large sequential files.
3. [Microsoft: GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex): documents information classes and API boundaries. Enumerated directory info does not establish a stable path-to-open-handle binding.
4. [NVIDIA CUDA Best Practices](https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html): reduce host/device movement and batch small transfers. This motivates the CPU/tiny versus GPU/batch design but does not establish a threshold for this machine.
5. [TABLEFS, USENIX ATC 2013](https://www.usenix.org/system/files/conference/atc13/atc13-ren.pdf): metadata/small-file organization can dominate storage efficiency. This is filesystem-level research; its reported gains are not transferable to an unmodified NTFS user-space scanner.
6. [Composite-file File System, FAST 2016](https://www.usenix.org/conference/fast16/technical-sessions/presentation/zhang-shuanglong): consolidation changes metadata/data locality. Useful mechanism, but modifying user storage layout is outside same's safe optimization scope.
7. [Icicle, 2026 preprint](https://arxiv.org/abs/2604.10295): emerging hybrid snapshot/event metadata indexing for HPC. Relevant future direction for repeated scans, not a validated NTFS implementation or grounds to trust incomplete event streams; journal overflow requires rescan semantics.

## Decision / 决策

Implement bounded pipelining, prepared-statement reuse and measured CPU/GPU routing before complex content preprocessing. Keep open-handle reuse as a explicit follow-on design with retry and handle-budget tests. Research supports amortizing fixed costs and exploiting locality; the user trace supports prioritizing the serialized frontend. Neither alone proves the eventual speedup.

## Implemented handoff contract / 已实施的移交契约

`WalkEntry::reader` now carries a unique, unread `FileReader`; `metadata()` opens once, obtains its stamp, and moves ownership into the bounded result queue. `FileReader::stamp()` remains fresh and no initial-stamp cache/API is added. Consumer integration must move the handle into hash work; merely ignoring this field saves no opens.

句柄资源上界由结果队列容量 + 活跃元数据工作线程 + 消费者持有项 + 在途哈希任务决定，而不是文件总数；目录枚举句柄另计。异常、取消与缓存命中通过 RAII 自动关闭。该改动只可消除“元数据完成后关闭，再在哈希开始打开”的一组 close/open：它不消除哈希前后 reader stamp 与 path stamp。`ComputeError` 重试需要重新打开路径（并重新检查预期 stamp），或显式 rewind；不可重用已经读到末尾的移交句柄。

## 集成状态 / Integration status

句柄移交、小于可配置 `gpu_min_bytes` 的 CPU SIMD 路由、主机精确比较及数据库热路径
复用已实现。元数据阶段至哈希的打开次数从通常 4 次降到 3 次，前后文件戳及路径绑定
检查不删除。`hash_retry_tests` 注入读取后的计算错误，验证重新打开、官方 abc 摘要和
双倍读取计数；同内容路径替换也必须在读取前失败。

Handle handoff, configurable size-aware CPU SIMD routing, host comparisons and prepared SQL reuse
are integrated. The usual initial metadata-to-hash path reduces four opens to three without deleting
freshness or path-binding checks. Fault-injection tests verify reopening after partial computation,
the official abc digest, doubled read accounting, and rejection of same-content path replacement.
No whole-file packing, unsafe extension-based assumptions, preprocessing archive or unique-size
hash skipping was added. These would impose additional copies, state semantics or preprocessing
cost not justified by current measurements. See [performance results](performance-results.md).
