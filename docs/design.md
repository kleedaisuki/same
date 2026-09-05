# 设计与证据 / Design and evidence

## 1. 目标及非目标 / Scope

`same` 在当前目录的普通文件集合中发现字节等价类（byte-equivalence class），排除用户忽略项和内部状态。核心不变量（invariant）：摘要只用于缩小候选集，输出分组必须经过精确比较；失败不能被当成扫描完整完成。程序不执行删除、硬链接合并、后台监视或跨目录全局索引。

`same` finds byte-equivalence classes among regular files below the current directory, excluding ignored entries and internal state. Digests only narrow candidates; reported groups require exact comparison. Failures must not masquerade as completed scans. Deletion, hard-link consolidation, background watching, and global indexing are out of scope.

## 2. 轻量领域驱动设计 / Lightweight domain-driven design (DDD)

DDD 在这里用于表达语义与所有权，而不是创建庞大的继承体系。`Compute` 是需要可替换后端的虚接口；文件、仓储和资源服务使用具体边界，没有为了形式统一而把所有类虚拟化。

DDD expresses semantics and ownership, not a large inheritance framework. `Compute` is polymorphic where backend substitution is needed; file, repository, and resource services remain concrete boundaries.

| 层 / Layer | 概念 / Concept | 契约 / Contract |
|---|---|---|
| 领域 / Domain | `FileStamp` | 大小、平台文件身份、修改/变更时间组成缓存版本证据 / Size, platform identity, modification/change timestamps form cache-version evidence |
| 领域 / Domain | `Digest` | 固定 32 字节 BLAKE3 摘要，不表示字节相等 / Fixed 32-byte BLAKE3 digest, not byte equality |
| 领域 / Domain | `FileRecord` | 根目录相对路径、版本证据、摘要 / Root-relative path, version evidence, digest |
| 应用 / Application | scan → hash → partition → validate → output | 编排流程与失败传播，不直接实现 CUDA 或 SQL / Orchestration and failure propagation, not CUDA or SQL |
| 边界 / Ports | `Store`, `Compute`, `FileReader`, `Resources` | 单所有者仓储、计算后端、句柄读取、统一任务准入 / Single-owner repository, compute backend, handle-based reads, unified admission |
| 基础设施 / Infrastructure | SQLite, CUDA, CPU BLAKE3, Win32/POSIX, run lock | 封装平台和库细节 / Encapsulate library/platform details |

```text
Filesystem traversal ──metadata──► Store cache
            │ changed                    │ cached digest
            ▼                            │
     bounded Resources ──hash results─────┘
            │
            ▼
SQLite (size,digest,path) index → streamed candidates
            │
            ▼
bounded exact comparisons → disk-backed representatives/matches
            │
            ▼
metadata revalidation → stdout groups / stderr statistics
```

## 3. 增量状态及事务 / Incremental state and transactions

缓存命中要求 `FileStamp` 完全相等。POSIX 使用设备/ inode 身份和纳秒形式的 mtime/ctime；Windows 优先使用卷序列号和 128 位文件 ID，并读取 LastWriteTime/ChangeTime。只有明确“不支持该信息类别”的 Windows 身份查询才回退旧身份接口；不能读取必要 ChangeTime 时失败。时间分辨率仍由底层文件系统决定。

Cache hits require identical stamps. POSIX uses device/inode plus mtime/ctime with nanosecond fields. Windows prefers volume serial/128-bit file ID and reads LastWriteTime/ChangeTime. Identity fallback is limited to unsupported information classes; required ChangeTime failures are fatal. Actual timestamp resolution remains filesystem-dependent.

`files` 用路径 BLOB 作主键；大小用固定宽度大端字节保存，避免 SQLite 有符号整数缩窄无符号文件大小；摘要长度有约束，`(size,digest,path)` 索引支持候选流。扫描代次（generation）让成功扫描删除未见记录，消失或新被忽略的路径不会永久残留。架构版本用 `user_version` 检查，未知版本拒绝打开，不能静默重解释旧状态。

`files` uses a BLOB path primary key, fixed-width big-endian size bytes to avoid signed-integer narrowing, checked digest length, and a `(size,digest,path)` candidate index. Scan generations remove unseen paths on success. `user_version` rejects unsupported schemas rather than reinterpreting them silently.

扫描更新是单个事务（transaction）；扫描失败回滚，成功扫描提交后才进入精确比较。**比较失败不回滚已经完成的扫描缓存**，但不会报告成功。进程锁覆盖扫描、比较和输出；数据库连接只由编排线程使用，计算工作线程不操作 SQLite。持久锁文件不能在运行时删除或替换。

Scan updates form one transaction: failures roll back and a successful scan commits before exact comparison. **Comparison failure does not undo the completed scan cache**, but does not report success. The process lock covers scan, comparison, and output. Only the coordinator owns the database connection; workers never use it. Do not unlink/replace the persistent lock file during a run.

使用 `journal_mode=DELETE` 和 `synchronous=FULL`，不引入写前日志（write-ahead logging, WAL），因为当前运行已串行化，没有并行数据库读写的需求。原子提交依赖操作系统锁、同步及存储设备遵守契约，不保证抵御损坏硬件或恶意篡改状态目录。[SQLite atomic commit](https://www.sqlite.org/atomiccommit.html) 解释这些假设。[SQLite threading modes](https://www.sqlite.org/threadsafe.html) 说明连接并发使用的约束。

`DELETE` journaling with `FULL` synchronization avoids WAL machinery because runs and database access are serialized. Atomic commit still depends on functioning locks, flushes, and storage; it is not protection against broken hardware or hostile state-directory mutation. See the linked SQLite primary documentation.

## 4. 有界资源与背压 / Bounded resources and backpressure

定义工作线程数 `W`、块大小 `B`、排队容量 `Q`、显存预算 `V`。所有哈希和比较任务共享一个资源池。每个线程独占两个读取缓冲和计算实例，避免数据竞争（data race）和隐式嵌套线程池。队列满时提交者等待；在途 future 同样有界，防止早期慢任务导致后续已完成结果无限堆积。

Let `W` be workers, `B` block bytes, `Q` queue capacity, and `V` the device budget. Hashing and comparison share one pool. Each worker owns two read buffers and a compute instance. A full queue blocks producers, and pending futures are also bounded so slow early tasks cannot cause unbounded completed-result accumulation.

| 资源 / Resource | 管理方式 / Accounting |
|---|---|
| 工作数据 / Worker payload | `W * (2B + B/32 + 4096) <= memory_bytes` |
| 设备分配 / Device allocation | 每线程不超过 `floor(V/W)` / per worker ≤ `floor(V/W)` |
| 排队任务与结果 / Queued tasks/results | 数量受 `Q` 约束，路径/闭包等为元数据 / count bounded by `Q`; paths and closures are metadata |
| SQLite 页缓存 / Page caches | 主库和临时库各约 2 MiB 的配置目标 / about 2 MiB configured target each for main and temp |
| 候选、碰撞代表和结果 / Candidates, representatives, matches | 流式查询、磁盘临时表 / streamed queries and disk-backed temporary tables |
| 排除项 / Exclusions | 线程栈、CUDA 上下文、分配器开销、OS 页缓存等 / stacks, CUDA contexts, allocator overhead, OS page cache, etc. |

因此这是**主要数据缓冲有界**，不是严格驻留集大小（resident set size, RSS）上限。SQLite 缓存设置也是目标而非整个数据库引擎的硬内存配额；临时数据和回滚日志需要可用磁盘空间。忽略匹配还需与路径长度成比例的临时空间；文件遍历栈与目录深度有关。[SQLite temporary files](https://www.sqlite.org/tempfiles.html) 描述临时表及其缓存行为。

These are **bounded major payload buffers**, not a hard RSS cap. SQLite cache settings are targets, not a whole-engine memory quota. Temporary data and rollback journals require disk space. Ignore matching uses path-length-proportional scratch space; traversal state depends on directory depth. SQLite's linked documentation explains temporary-file caching.

CUDA 可以选用小于主机块的设备子块以符合预算；至少需要每线程 2196 字节的显式设备空间，不足时回退 CPU。即使声明的预算足够，实际设备分配仍可能失败并触发回退。

CUDA may choose device sub-blocks smaller than host blocks to fit the budget. At least 2196 bytes of explicit device storage per worker is required; insufficient budgets fall back to CPU. Actual allocation can still fail despite a nominally adequate configured budget.

## 5. 哈希与精确分组 / Hashing and exact partitioning

CPU 使用上游 BLAKE3 实现。CUDA 并行处理 BLAKE3 块，主机进行树归约（tree reduction）；不是将各块独立摘要简单拼接。完整文件摘要必须与 CPU 一致，尤其需要测试空文件、1024 字节边界、非整块输入和跨更新边界。算法依据为 [BLAKE3 specification and paper](https://raw.githubusercontent.com/BLAKE3-team/BLAKE3-specs/master/blake3.pdf)。

The CPU uses upstream BLAKE3. CUDA processes BLAKE3 chunks in parallel with host-side tree reduction, not concatenated independent hashes. Whole-file digests must match CPU results, especially at empty, 1024-byte, partial-chunk, and update boundaries. The BLAKE3 specification/paper is the algorithm reference.

精确比较在相同 `(size,digest)` 桶中构造内容等价类。常见路径是每个候选与首个代表比较，可进行有界并行；不相等的候选再流式比较额外代表，必要时成为新代表。代表和结果保存在 SQLite 临时表而不是无界向量中。即使人为注入摘要碰撞，也不能把不同内容合并，且不能遗漏非首代表的重复类。最坏碰撞桶的比较次数可以是二次方；这是正确性兜底，而不是围绕极端碰撞建立复杂常态调度。

Within each `(size,digest)` bucket, exact comparisons construct content-equivalence classes. The common path compares candidates against the first representative with bounded parallelism. Mismatches stream through additional representatives and create a new representative when necessary. SQLite temporary tables avoid unbounded representative/result vectors. Even artificial digest collisions must neither merge unequal contents nor hide duplicate classes outside the first representative. Worst-case collision comparison count is quadratic; this correctness fallback does not justify complicating the normal scheduler.

CUDA 比较也是逐字节验证，不再使用另一个摘要。计算后端发生可回退错误时销毁 GPU 实例，切换 CPU，并重新打开文件从头完成该次操作；文件 I/O 和版本错误不能被吞掉。CUDA 传输与同步有实际成本，不能仅用算术吞吐推导端到端收益。[NVIDIA CUDA programming guide](https://docs.nvidia.com/cuda/cuda-c-programming-guide/) 是平台行为参考；当前没有承诺 GPU 更快的基准结论。

CUDA equality is also byte verification, not a second hash. Recoverable compute failures replace the GPU instance with CPU and restart the file operation; I/O/version errors propagate. Transfers and synchronization cost real time, so arithmetic throughput does not establish end-to-end benefit. NVIDIA's programming guide is the platform reference; no GPU speedup is claimed here.

## 6. 忽略、文件系统及一致性 / Ignore and filesystem consistency

glob 使用动态规划（dynamic programming, DP），每条规则约 `O(pattern_length * path_length)` 时间和 `O(path_length)` 额外内存，避免递归回溯的指数膨胀。规则有 1 MiB/4096 条上限，TOML 输入单独限制为 64 KiB；有否定规则时不能盲目剪掉父目录。此实现明确不宣称完整 Git 忽略语义。
Glob matching uses DP with approximately `O(pattern_length * path_length)` time per rule and `O(path_length)` scratch memory, avoiding recursive exponential backtracking. The 1 MiB/4096-rule caps bound input; TOML input is separately capped at 64 KiB. Negation prevents blindly pruning parent directories. Full Git ignore compatibility is not claimed.

读取前后同时核对打开句柄和路径版本，输出前再次复核成员。它们提高对通常并发编辑和路径替换的检测能力，但不提供线性一致性（linearizability）或对敌对写入者的快照隔离（snapshot isolation）。一个文件仍可在最后检查后变化；元数据不变的修改也可能逃过缓存检测。`--rehash` 消除历史摘要复用，不消除实时文件系统竞态。

Handle/path checks before and after I/O, followed by member revalidation before output, detect ordinary edits and replacements. They do not provide linearizability or snapshot isolation against hostile writers. Files can change after the final check, and unchanged metadata can conceal changes from cache reuse. `--rehash` removes historical digest reuse, not live-filesystem races.

状态目录与数据库应由可信用户控制；程序不应以高权限扫描敌对可写目录。路径链接排除和检查不是完整安全沙箱。硬链接按路径报告，同一物理文件的多个名称不表示可回收的重复存储。

The state directory/database should be trusted. Do not treat scanning an adversarial writable tree with elevated privileges as safe. Link exclusions and metadata checks are not a security sandbox. Hard links are reported as paths, not independently reclaimable storage.

## 7. 验证与演进 / Validation and evolution

验证应区分：算法已知向量、CPU/CUDA 差分（differential testing）、元数据行为、事务回滚、资源背压，以及完整 CLI 工作流。跨平台宣称以实际执行记录为准；README 的测试矩阵不意味着所有文件系统、驱动或编译器已验证。

Validation separates known vectors, CPU/CUDA differential testing, metadata behavior, rollback, resource backpressure, and end-to-end CLI behavior. Platform claims depend on executed tests; the README matrix does not imply every filesystem, driver, or compiler was tested.

未来研究方向包括文件系统快照、可靠变更事件与元数据索引结合、设备批处理和 I/O/计算重叠。它们是**待验证的设计方向，不是本版本能力或已证实收益**。采用前应测量冷/热缓存、文件大小分布、重复率、变化率、存储介质、CPU/GPU 分别耗时和总运行时间，并保持相同正确性要求与公平 CPU 基线。先证明真实瓶颈，再增加复杂度。

Future research directions include snapshots, reliable change-event/metadata-index integration, device batching, and I/O/compute overlap. These are **unvalidated directions, not current capabilities or proven gains**. Adoption needs cold/warm-cache measurements across size distributions, duplicate/change rates, storage media, backend time, and end-to-end time, with equal correctness requirements and a fair CPU baseline. Establish the real bottleneck before adding complexity.

近期相关工作 [Icicle (ISC 2026)](https://arxiv.org/abs/2604.10295) 将周期性元数据摄入与事件摄入结合，面向大型 HPC 文件系统。**本项目的推论**是：未来如果引入事件缓存，应同时设计漏事件后的重扫与校准；论文的集群索引吞吐不能直接证明本地文件比较器的收益，也不值得在此引入 Kafka/Flink。当前保留每次完整目录遍历的简单模型。

[Icicle (ISC 2026)](https://arxiv.org/abs/2604.10295) combines periodic metadata ingestion with event ingestion for large HPC filesystems. **Our design inference:** any future event cache needs rescan/reconciliation after lost events. Cluster-index throughput does not establish benefit for a local comparator or justify Kafka/Flink here; the current model retains full traversal on every run.
