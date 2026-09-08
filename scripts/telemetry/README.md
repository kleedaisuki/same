# 遥测归档脚本 / Telemetry archive scripts

独立于 `tools/` 和 same 程序。仅需 Python 3.10+（含标准库 `sqlite3`），不需要安装第三方包。
Merge runtime history into a standalone analysis database with Python 3.10+ and standard-library SQLite. No same CLI integration or third-party packages.

## 使用 / Usage

```powershell
# 从仓库根目录运行；输出必须是新文件，父目录必须存在。
# Run from the repository root; output must be new, with an existing parent directory.
python scripts/telemetry/merge.py --output D:/analysis/history.db `
  D:/workspace-a/.same/telemetry.db D:/workspace-b/.same/telemetry.db

# PowerShell 显式展开文件列表；不要包含输出库。 / Expand the list explicitly; exclude output.
$inputs = (Get-ChildItem D:/snapshots -Filter *.db -File).FullName
python scripts/telemetry/merge.py --output D:/analysis/history-next.db $inputs

# 增量合并也生成新文件，绝不原地修改旧归档。 / Incremental merge creates a new archive.
python scripts/telemetry/merge.py --output D:/analysis/history-new.db `
  D:/analysis/history.db D:/workspace-c/.same/telemetry.db
```

输出主文件及其 `-journal`、`-wal`、`-shm` 名字必须均不存在；旧侧文件也不会被删除或覆盖。请独占所选输出名称，不要让其他进程同时创建这些文件。
The output and its journal/WAL/SHM names must all be unused. Existing sidecars are never deleted; do not concurrently create or modify the selected output names.

成功退出码为 0，stdout 为 JSON 计数；失败退出码为 2，stderr 说明原因。Ctrl+C 也不会发布部分归档。
Exit 0 prints JSON counts; exit 2 reports an error. Cancellation does not publish partial data.

## 合并契约 / Merge contract

| 项目 / Item | 行为 / Behavior |
|---|---|
| 输入 / Input | 接受 `SAMT` 运行库和 `SAMA` 归档，模式版本 1；拒绝 `state.db`、未来版本或错误表形状 / Only recognized schema-v1 telemetry/archive databases |
| 完整记录 / Payload | 保留 `runs/events/metrics/parameters` 全部数据列，包括配置、模型、错误、丢弃计数和时间单位 / Preserve every payload column |
| 运行与跨度 / Identity | 保留 `run_id`、`span_id`、`parent_span_id`、worker；跨运行使用 `(run_id, span_id)` 关联 / Preserve run-scoped identities |
| 事件行号 / Row IDs | `event_id` 是源库局部编号，重建为目标唯一编号；每个运行内事件相对顺序不变 / Reassign local row IDs, retaining per-run event order |
| 相同运行 / Duplicates | 同 ID 的四张表内容完全相同则只复制一次；忽略局部 `event_id` 数值但不忽略事件顺序 / Exact-content deduplication |
| 冲突 / Conflicts | 同 ID 有任意内容不同则整次失败，不选择较新者、不静默丢字段 / Fail the entire merge, never silently choose a version |
| 来源 / Provenance | `merge_sources` 保存直接输入路径、快照采集时间、身份及计数；`run_sources` 关联各输入 / Record direct input membership |
| 查询 / Queries | 保留 `logs`、`spans`；归档 `latest_run` 按 UTC 开始时间再按 ID 排序，不代表刚导入或已完成 / Latest by UTC then ID, not import order or completion |

归档身份 `application_id=1396788545`（`0x53414D41`，SAMA），不同于运行库的 SAMT。这防止它被 same 当作活动遥测库自动清理。**不要将归档放到工作区 `.same/telemetry.db`。** 归档无保留数量限制；脚本也不会合并业务摘要数据库或改动运行时模型。

The distinct archive application ID prevents the runtime writer from treating it as a retention-managed journal. Keep archives outside the runtime telemetry path. No retention is applied and no runtime learning state is changed.

再次合并归档时保留四张载荷表；来源表重新描述本次直接输入，不递归复制之前的合并来源链。原始路径及错误仍可能敏感，脚本不脱敏、不上传。JSON 中 `runs_duplicate` 和 `running_snapshots` 统计输入中的出现次数，而非唯一运行数。

Archive inputs preserve the four payload tables; provenance describes immediate inputs, not recursively flattened ancestry. Paths/errors remain potentially sensitive and are neither redacted nor uploaded. Duplicate/running counts refer to input occurrences.

## 一致性与资源 / Consistency and resources

- 输入连接使用 `mode=ro`，每个输入保持一个读事务，读取写前日志（write-ahead logging, WAL）中的已提交内容；不使用会忽略活跃 WAL 的 `immutable=1`。各输入不是同一个全局时刻的快照。
- 优先合并已结束的扫描。活动库允许读取，但 `running` 保持原值并发出警告；最新提交之后的数据不在快照内。不同时间抓取同一运行可能冲突，脚本不会猜测哪份完整。
- 长读事务可能阻碍 WAL 检查点（checkpoint）回收、增加磁盘占用。只读意味着不修改源数据，不代表 SQLite 不使用或维护共享内存侧文件。离线复制时不要只复制尚有未合并 WAL 的主 `.db`。
- 每批最多 512 行，不将整个库或运行载入内存。完整性检查和重复比较仍消耗 I/O，超大日志应预留时间和输出磁盘容量。
- `--timeout 300` 默认每源 300 秒预算，在 SQLite 指令及运行边界检查；可调大。它不是操作系统 I/O 的硬实时中断保证。
- 私有临时库与输出位于同一文件系统；完成事务并关闭连接后，用硬链接（hard link）无覆盖发布，随后删除临时名字。要求 NTFS/ext4 等支持硬链接的文件系统；不支持则失败，不降级成覆盖复制。异常会清理临时目录，强杀进程可能留下 `.same-merge-*` 临时目录。正常发布原子可见，不承诺断电时目录项绝对持久。

Read-only transactions provide per-input committed snapshots including WAL, not one global timestamp. Prefer completed scans: live snapshots remain incomplete and long reads can pin WAL. Copying is streamed in 512-row batches. Timeouts are cooperative, not hard OS-I/O deadlines. A private, fully committed database is published by a no-overwrite hard link on a supporting filesystem. Forced termination may leave a private temporary directory; power-loss directory durability is not guaranteed.

设计遵循 [SQLite 快照隔离](https://www.sqlite.org/isolation.html) 与 [Python sqlite3 连接、事务和只读 URI 契约](https://docs.python.org/3/library/sqlite3.html)。这是离线数据合并，不引入新的在线分析算法或扫描性能主张。
The design follows the official isolation and Python connection/transaction contracts above; it adds no online learning algorithm or scan speedup claim.

## 验证 / Tests

```powershell
python -m unittest discover -s scripts/telemetry -p test_merge.py -v
```

独立 CLI 回归覆盖完整载荷、重复/冲突、WAL 活动快照、源数据不变、已有输出保护、未知模式、归档再合并和失败原子性。
Standalone CLI regressions cover payloads, duplicates/conflicts, live WAL, source preservation, output protection, schema rejection, archive composition and failure atomicity.
