# 本地跨运行遥测 / Local cross-run telemetry

## 范围与契约 / Scope and contract

`.same/telemetry.db` 是独立于 `.same/state.db` 的可观测性（observability）历史，不是摘要缓存，也不是正确性依据。它记录运行身份、解析后的配置、阶段追踪（tracing）、采样哈希跨度（span）、日志、最终性能指标和在线模型参数。`--summary` 仍只显示本次运行；不提供内置查询命令、HTML 报告、服务或远程导出。下面 SQL 由用户自己的 SQLite 客户端执行。

Telemetry is diagnostic history, separate from the digest cache and never a correctness dependency. It records identity, resolved configuration, stage traces, sampled hash spans, logs, final metrics and online model parameters. Summary remains current-run-only; SQL below is for external clients, not an integrated query/report/server feature.

| 设置 / Setting | 默认 / Default | 含义 / Meaning |
|---|---:|---|
| `telemetry` | `true` | 本地持久化开关 / Local persistence |
| `telemetry_queue_capacity` | 4096 | 1–65536 个固定事件槽 / Fixed event slots |
| `telemetry_retention_runs` | 64 | 1–4096 次运行，包含本轮 / Runs including current |
| `telemetry_max_events` | 16384 | 1–1000000 条入队事件上限；最终快照另计 / Queued-event cap; final snapshot separate |

`--no-telemetry` 不打开数据库、不创建写入线程，但在线调度分析器仍可学习。`--no-pgo` 不再采集哈希性能样本或更新路由模型，但不关闭基础运行记录、阶段与错误日志。`--summary` 不控制采集。禁用遥测不会删除旧历史；`same clean` 删除整个 `.same`，包括遥测库。

No-telemetry skips database access and the writer, not routing learning. No-pgo skips hash profiling and model updates, not basic history, stages or errors. Summary does not control collection. Disabling preserves old history; clean removes the entire state directory including telemetry.

## 为什么这样写入 / Write architecture

```text
workers / coordinator
        │ fixed-size event, try_lock (no waiting)
        ├─ contention / queue full / event cap → counted drop
        ▼
bounded preallocated ring
        ▼
dedicated writer → prepared statements → batched transactions
        ▼
.same/telemetry.db (WAL, synchronous=NORMAL)
        ▲
separate final slot: run status + complete metrics/model + stage spans
```

生产者（producer）不执行 SQL、不等待锁、不因队列满等待磁盘；事件入队只做有界复制与计数。互斥锁尝试失败也会丢弃事件，因此这不是无锁（lock-free）数据结构，也不是实时系统的最坏延迟保证。SQLite 连接仅由后台线程拥有，SQL 执行期间不持有生产者队列锁。复用预编译语句（prepared statement），批量事务避免每条日志独立提交。每批最多 `min(64, telemetry_queue_capacity)` 条，队列容量 1 也有效；有事件后等待批次填满或 250 ms 到期，正常结束立即唤醒排空、不等待批次期限。这是批处理等待策略，不是持久化延迟的硬保证。

Producers perform bounded copies/counters, not SQL or lock/disk waits. A failed try-lock drops the record; this is not a lock-free algorithm or hard real-time guarantee. The writer exclusively owns the SQLite connection and runs SQL outside the queue lock, reusing statements and batching transactions. A batch holds at most `min(64, telemetry_queue_capacity)` events, including capacity 1. Once events exist, it waits for a full batch or 250 ms; finish wakes immediate draining. This is a batching policy, not a hard persistence-latency bound.

结束记录使用独立槽，不会因普通事件队列已满而丢失；但仍可能因数据库错误、内存分配失败或进程中止而无法持久化。正常结束显式等待队列排空及写入线程退出，等待开销单独汇报，不偷偷混入哈希工作时间。运行期间依旧共享 CPU、内存与存储带宽，因此不能承诺零开销。

The final slot bypasses event-queue pressure, not database/allocation failures or process termination. Normal shutdown explicitly drains and joins, with separately reported wait time rather than attributing it to hashing. CPU, memory and storage are still shared: no zero-overhead claim.

写前日志（write-ahead logging, WAL）允许读取旧快照与写入并行，但仍只有一个写者。`synchronous=NORMAL` 优先诊断吞吐量：断电时最近已提交遥测可能丢失，不用于审计合规或恢复文件结果。它不适用于跨主机网络文件系统。详见 [SQLite WAL 文档](https://www.sqlite.org/wal.html)。

WAL supports concurrent snapshot readers and one writer. NORMAL trades power-loss durability for diagnostic throughput; recent telemetry commits may be lost and are not an audit/compliance guarantee. Cross-host network filesystems are unsupported for WAL. See the official SQLite reference above.

设计借鉴 [OpenTelemetry 批量日志处理器](https://opentelemetry.io/docs/specs/otel/logs/sdk/) 的有界批量与显式结束语义，以及 [NanoLog, USENIX ATC 2018](https://www.usenix.org/conference/atc18/presentation/yang-stephen) 将昂贵处理移出应用热路径的原则。这里没有引入 OpenTelemetry SDK、OTLP 协议或 NanoLog 实现；论文性能数字不适用于本程序。

The design borrows bounded batching/shutdown semantics from OpenTelemetry and off-hot-path processing from NanoLog. It does not implement their SDKs/protocols or inherit their benchmark results.

## 模式与检索 / Schema and retrieval

`PRAGMA user_version = 1`，`PRAGMA application_id = 1396788564`（`0x53414D54`）。版本与应用所有者在修改持久 PRAGMA、日志模式或模式之前校验；拒绝陌生、有未知对象或未来版本的数据库，不覆盖重建。所有历史通过 `run_id` 关联；运行 UTC 时间为 Unix 纳秒，事件时间是本轮起点之后的单调纳秒。不要跨运行直接比较事件 `time_ns`，不要把重叠工作线程跨度相加当作实际耗时。

Schema version is 1 with application ID 1396788564 (`0x53414D54`). Ownership/version are checked before persistent PRAGMA, journal or schema changes; foreign, nonempty unknown and future databases are refused rather than reset. Run IDs correlate all rows. Run timestamps use UTC Unix nanoseconds; event timestamps are monotonic offsets within a run. Offsets are not cross-run timestamps and overlapping worker spans do not sum to elapsed time.

| 表/视图 / Table/view | 核心内容 / Contents |
|---|---|
| `runs` | `run_id`, UTC 开始/结束、状态、版本、命令、根路径、`config_json`、错误及写入计数 / Identity, lifecycle, configuration and writer counters |
| `events` | `type`, `name`, `severity`, `message`, `backend`, `worker`, `span_id`, `parent_span_id`, `time_ns`, `duration_ns`, `bytes`, `value`, `unit`, `truncated` |
| `metrics` | 每轮 `name`, 数值 `value`, `unit` / Named final numeric metrics |
| `parameters` | 每轮 `category`, `name`, 字符串 `value` / Exact named parameter snapshots |
| `logs` | `events WHERE type='log'` |
| `spans` | `events WHERE type='span'` |
| `latest_run` | 按插入顺序最新的运行，不保证已完成 / Latest inserted run, not necessarily completed |

`runs.accepted/persisted/dropped` 仅统计普通入队事件；独立结束槽内的阶段、指标与参数不占这些计数和事件上限。最终 drain 等待包含关闭数据库和线程退出，准确值只能在结束后取得，因此在本轮 Summary 中显示，而非伪装为库内已知值。`truncated` 统计与事件行标记应一起检查。

Run accepted/persisted/dropped counters cover queued events only, not reserved final stages/metrics/parameters. Exact final drain includes DB close/thread exit and is available in current-run Summary after joining, not falsely persisted before it is known. Inspect both truncation totals and row flags.

索引覆盖 `(run_id,time_ns)`、`(name,run_id)`、`(run_id,span_id)` 和运行开始时间；指标、参数按运行与名字建立主键。外部客户端建议以只读模式打开，并保持短读事务。仅复制主 `.db` 可能遗漏 WAL 中尚未合并的数据；运行中备份应使用 SQLite 备份 API 或客户端备份功能。

Indexes cover run/time, event name/run, run/span and run start. Metrics and parameters have run/name keys. Prefer read-only clients and short transactions. Copying the main DB alone can omit live WAL data; use SQLite backup facilities for live backups.

```sql
-- 最近运行与遥测损失 / Recent runs and telemetry loss.
SELECT run_id, datetime(started_unix_ns / 1000000000, 'unixepoch') AS started_utc,
       status, accepted, persisted, dropped, errors, truncated, queue_high_water
FROM runs ORDER BY started_unix_ns DESC LIMIT 20;

-- 最新运行的阶段与采样任务时间线 / Latest stage and sampled-task timeline.
SELECT name, backend, worker, span_id, parent_span_id,
       time_ns / 1000000.0 AS start_ms, duration_ns / 1000000.0 AS duration_ms,
       bytes, severity, message
FROM spans WHERE run_id = (SELECT run_id FROM latest_run)
ORDER BY time_ns, event_id;

-- 跨运行查错误与回退 / Cross-run errors and fallback diagnostics.
SELECT run_id, name, severity, message, truncated
FROM logs WHERE severity IN ('warn', 'error') ORDER BY event_id DESC LIMIT 100;

-- 原始终态指标；单位不能省略 / Final metrics with units.
SELECT name, value, unit FROM metrics
WHERE run_id = (SELECT run_id FROM latest_run) ORDER BY name;

-- 所有模型/算法参数，不仅汇总分位数 / All model/algorithm parameters.
SELECT category, name, value FROM parameters
WHERE run_id = (SELECT run_id FROM latest_run) ORDER BY category, name;

-- 已学习的每个大小区间；保留字符串精度 / Learned bands with preserved text precision.
SELECT category, name, value FROM parameters
WHERE run_id = (SELECT run_id FROM latest_run)
  AND (category LIKE 'model.cpu.%' OR category LIKE 'model.gpu.%')
ORDER BY category, name;

-- 原始直方图桶计数 / Raw histogram bucket counts.
SELECT name, value FROM metrics
WHERE run_id = (SELECT run_id FROM latest_run)
  AND (name LIKE 'pgo.latency_bucket_us.%' OR name LIKE 'pgo.residual_bucket_us.%')
ORDER BY name;

-- 配置 JSON 原文 / Resolved configuration JSON.
SELECT config_json FROM latest_run;
```

## 参数、采样和解释边界 / Parameters, sampling and interpretation

`metrics.unit` 显式区分 `ms`、`count`、`bool`、`bytes` 和 `MiB/s`；例如 `dispatch.setup_ms` 与 `pgo.mean_absolute_error_ms` 为 `ms`，`pgo.samples` 为 `count`。整数汇总转为 SQLite REAL，超过 2^53 的整数可能不再精确；模型样本参数以文本存储以保留原值。

Metric units distinguish ms/count/bool/bytes/MiB/s. Numeric summaries use SQLite REAL, so integers above 2^53 may lose precision; model sample parameters remain text to preserve their exact value.

最终快照保留 CPU/GPU 各 32 个大小区间，共 64 个区间的学习参数，包括未知区间；记录成本、误差、样本数和 known 状态，以及算法设置和原始延迟/残差直方图（histogram）。未知区间的零值不表示零耗时。参数是运行末态，不会跨运行自动加载为训练先验。`parameters.category` 使用 `routing`、`model`、`dispatch`、`telemetry` 和 `model.cpu.0`…`model.gpu.31`；每个区间记录 `known`、`samples`、`cost_ms_per_byte`、`error_ms_per_byte`，浮点参数以 17 位精度文本保存。直方图桶键是 `pgo.latency_bucket_us.0`…`.31` 和 `pgo.residual_bucket_us.0`…`.31`，桶索引为 `floor(log2(us))`、两端饱和，数值是次数而非微秒。

写入器自身在 `parameters.category='telemetry'` 保存 8 个实际生效值：`queue_capacity`、`batch_size`、`event_cap`、`retain_runs`、`busy_timeout_ms`、`flush_interval_ms`、`schema_version`、`application_id`。它们来自写入器最终配置，例如容量 1 对应 `batch_size=1`，而非未经裁剪的配置请求值。

The writer persists eight effective settings in category telemetry: queue_capacity, batch_size, event_cap, retain_runs, busy_timeout_ms, flush_interval_ms, schema_version and application_id. These reflect the actual clamped configuration, including batch_size=1 for capacity 1.

The final snapshot retains all 32 size bands per CPU/GPU backend (64 total), including unknown bands: costs, errors, counts and known state, algorithm settings and raw latency/residual histograms. Unknown zero-valued bands are not zero-cost predictions. Snapshots are terminal diagnostics, not automatically reloaded training priors.

哈希跨度复用已有分析器采样：合格任务采样，小任务每个工作线程每 64 次采样；它们不是所有文件的完整追踪。未采样的文件任务失败也记录 `hash.error`，不受 `--no-pgo` 抑制；开启采样时失败记录在 `hash` 错误跨度中。GPU 回退记录 `hash.fallback`。这些诊断仍受事件队列容量与上限约束，不应从缺少跨度推断“没有执行”。普通事件还可能因容量、争用和每轮上限丢弃；应先看损失计数再分析分布。模型直方图与事件数据库采样不等同，事件队列丢弃不撤销已经更新的模型。

Hash spans reuse analyzer sampling: eligible work is sampled, small work once every 64 tasks per worker. They are not a complete file trace. Unsampled failures emit hash.error even with no-pgo; sampled failures use an error-severity hash span. Fallbacks emit hash.fallback. Diagnostics remain subject to queue/cap limits. Missing spans do not prove work was absent. Inspect loss counters before distribution analysis; event loss does not undo model observations.

## 生命周期、保留与隐私 / Lifecycle, retention and privacy

后台打开前检查主库与 `-wal`、`-shm`、`-journal` 侧文件：拒绝符号链接、Windows 重解析点（reparse point）、多硬链接与非普通文件。该保护用于拒绝已存在的不安全路径，不声称抵御拥有同目录写权限的恶意进程并发替换路径。检查失败仅禁用遥测并报告，不阻塞文件比较业务。

Before opening, the writer rejects existing symlinks, Windows reparse points, multiply hard-linked files and special files at the main DB and its -wal/-shm/-journal paths. This rejects preexisting unsafe paths, not malicious concurrent replacement by a process able to write the directory. Failure disables telemetry, not file comparison.

正常完成记 `completed`，显式失败记 `failed`，未显式结束记 `interrupted`。硬崩溃可能留下 `running`；下一次成功打开同一库会将旧 running 标记 interrupted。写入器错误会在 stderr 警告，即使没有 `--summary` 也不静默隐藏。数据库损坏、外部写锁或不支持的模式只禁用遥测并报告，不重建覆盖陌生数据库，不使文件比较失败。

Normal completion is completed, explicit failure failed, implicit finalization interrupted. A hard crash can leave running; the next successful open marks stale running rows interrupted. Writer errors warn on stderr even without summary. Corruption, external write locks or unsupported schema disable telemetry with diagnostics rather than overwrite an unknown database or fail comparison.

后台保留当前运行，再按 UTC 开始时间保留最新的其余 `telemetry_retention_runs - 1` 次运行；即使系统时钟回拨也不会淘汰本轮。`latest_run` 使用插入顺序而不是 UTC 排序，避免回拨导致选错本轮。关联事件、指标与参数随淘汰一并删除。删除释放的页可复用，但不会自动缩小主文件，也不运行在线 VACUUM。每轮事件上限不是数据库字节硬上限。长时间外部读事务可能阻止检查点回收并使 WAL 增长；查询后及时结束事务。需要磁盘回收时，先停止 same 与其他客户端，再由用户自己的 SQLite 管理工具维护。

Retention always keeps the current run and the newest remaining runs by UTC start, so clock rollback cannot evict the current run. The latest_run view uses insertion order rather than UTC. Retention cascades to dependent rows. Freed pages are reused, not automatically returned to the filesystem; no online VACUUM runs. Event caps are not a byte-size guarantee. Long external readers can prevent WAL reclamation; close read transactions promptly. Offline maintenance belongs to the user's SQLite tools.

库仅写本地，不上传数据，不记录文件内容；根路径、采样相对路径、配置和错误仍可能敏感。事件文本有界截断并标记 `truncated`，不能当作完整路径索引。共享日志前检查/脱敏整库；同目录访问权限与本地备份策略由用户管理。

Storage is local without uploads or file contents. Roots, sampled relative paths, configuration and errors can still be sensitive. Bounded event text may be truncated and is not a complete path index. Review/redact before sharing; directory permissions and backup policy remain user-managed.
