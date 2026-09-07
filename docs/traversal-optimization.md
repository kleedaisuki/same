# 并行遍历设计 / Parallel traversal design

## 观测与假设 / Observation and hypothesis

用户基线：245,091 文件、9.16 GiB、Scan 22.34 min、Hash work 5.49 min（工作线程时间之和）。这证明串行前端区间远大于哈希等待，但 **不能分离枚举、元数据、SQLite**。因此不得把所有时间归因于 Windows metadata，也不得宣称遍历改动会带来固定倍数提升。

The reported scan interval includes enumeration, metadata and database work. It is not an isolated metadata benchmark. New summed `enumerate_ms` and `metadata_ms` measurements exclude output backpressure but can overlap other work; they are not additive to elapsed time.

## 外部证据 / External evidence

| 来源 / Source | 对设计的启发 / Design implication | 适用边界 / Limitation |
|---|---|---|
| [ripgrep ignore walk upstream](https://raw.githubusercontent.com/BurntSushi/ripgrep/master/crates/ignore/src/walk.rs) | 生产实现采用 crossbeam deque stealing；Windows 枚举得到的 metadata 被缓存。Reuse enumeration attributes; distribute independent work. | 我们不能把目录项缓存时间替代保守 FileStamp。Enumeration metadata is not a trustworthy cache version. |
| [jwalk upstream](https://github.com/Byron/jwalk) | Rayon 并行目录遍历与流式迭代。Parallel traversal can expose a streaming consumer interface. | 仅目录并行对单个巨大目录帮助有限；目前仓库已声明不维护。Directory-only parallelism misses a single wide directory; project is now unmaintained. |
| [Microsoft FindFirstFileExW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfileexw) | Basic info avoids short-name retrieval; large-fetch flag requests larger directory query buffers. | 实际加速取决于文件系统和存储；并非原子快照。Benefits are workload-dependent; not a snapshot. |
| [Blumofe–Leiserson, JACM 1999](https://doi.org/10.1145/324133.324234) | 动态 DAG / dynamic DAG 可用 work stealing 平衡不规则任务。 | 理论假设 fully strict computation；阻塞 I/O、队列背压不直接满足同样执行模型，不能套用理论加速保证。 |
| [Scheduling I/O Latency-Hiding Futures in Task-Parallel Platforms](https://www.cse.wustl.edu/~angelee/home_page/papers/futureIO.pdf) | I/O 延迟隐藏必须进入调度设计，而不只是 CPU DAG。 | 本实现仍用阻塞 OS I/O；没有宣称异步 futures runtime 的理论性质。 |
| [X-OpenMP, FGCS 2024](https://doi.org/10.1016/j.future.2024.05.019) | 最新细粒度工作研究强调调度同步开销。Fine-grained scheduling overhead can dominate. | 此处文件打开通常远重于队列锁；先测共享锁再考虑无锁 work stealing。 |

## 实现 / Implementation

`ParallelWalk` provides a single-consumer `next()` stream of `{path, stamp, reader}`. Keys are root-relative UTF-8; the reader starts at offset zero and transfers ownership to the hash stage. Cache hits simply destroy it. The stages overlap, rather than collecting the entire tree first.

- **Dynamic work sharing, not work stealing**: one bounded FIFO contains directory and file-metadata tasks. A directory discovers child work at runtime. Idle workers consume it. This deliberately avoids claiming implementation of the Cilk algorithm.
- **Bounded recursive production**: a worker never waits to enqueue more work. When the task queue is full it processes a file inline or descends with an explicit DFS cursor stack. This prevents the classic all-workers-blocked-on-full-work-queue deadlock.
- **Bounded outputs**: a separate queue holds at most `capacity` metadata results. Output backpressure waits only for the external consumer. Files already opened but blocked before publication add at most one reader per worker. Downstream retained entries must be separately bounded.
- **Space**: O(capacity + workers × maximum tree depth) entries/cursors, ignoring variable-length paths and OS buffers. This is not a byte-exact process memory limit. Directory handles grow with depth, not breadth; extremely deep trees can still exhaust platform limits and fail explicitly.
- **Termination**: an empty task queue alone is insufficient. Completion requires both an empty task queue and zero active jobs; queued results are still drained. Worker errors stop production, wake waiters and rethrow on the consumer. Destruction cancels and joins, but cannot interrupt an OS call already blocked in the kernel.
- **Windows enumeration**: `FindFirstFileExW` with `FindExInfoBasic` and `FIND_FIRST_EX_LARGE_FETCH` directly provides directory/reparse flags; no per-entry `GetFileAttributesW` round-trip. `windows_path` extends native boundaries without changing cache keys. POSIX keeps its portable `directory_iterator` and `symlink_status` route.
- **Safety**: ignore negation semantics are unchanged, `.same` identity exclusion remains, reparse/symlink entries are not descended, and metadata comes from `FileReader` rather than enumeration timestamps. As before, this is not protection against malicious concurrent ancestor replacement or a filesystem snapshot.

## 验证 / Validation

`walk_tests` covers workers 1/4/16 × queue capacity 1/3/64; wide and deep trees; no duplicate/missing keys; ignore negation; `.same` exclusion; symlink cycle exclusion when the platform permits symlink creation; transferred reader metadata/offset; queue peaks; repeated EOF; cancellation with a full output queue; worker error propagation; invalid zero-worker configuration.

集成测试与真实盘性能必须另外验证。Unit tests do not establish speedup. Compare same fixture and cache condition, 1/2/4/8 metadata workers, original vs new executable, cold vs warm filesystem cache, and SQLite cost separately. Use enough files to reveal front-end overhead; report distributions, not only the fastest run. A single deep chain has almost no directory parallelism; a flat directory tests file-task parallelism; broad trees test directory scheduling. Actual D-drive workload remains the final validation target.
