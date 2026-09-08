# SQLite 热路径优化 / SQLite hot-path optimization

## 实现与契约 / Implementation and contracts

- `cached`, `save`, `mark_seen`, `add_representative`, `add_match` 各复用一个连接所有的预编译语句，数量恒定。 / Each operation reuses one connection-owned prepared statement; count is constant.
- 每次调用成功时显式检查 reset，异常展开时 noexcept reset；两条路径都 clear bindings，避免残留参数和路径副本。 / Success checks reset; unwinding resets without throwing. Both clear bindings to release parameters and copied paths.
- `SQLITE_TRANSIENT` 仍复制输入；查询返回拥有数据的副本，不借用游标。 / Inputs remain copied with SQLITE_TRANSIENT; queries return owned records.
- 析构先 finalize 全部缓存语句再关闭连接；事务回滚、FULL synchronous、DELETE journal 和 schema v1 不变。 / Destruction finalizes statements before connection close; rollback, FULL synchronous, DELETE journal and schema v1 are unchanged.
- `mark_seen(path)` 只更新非索引 generation 列，避免命中缓存时把 size/digest/path 索引列列入 UPDATE；缺失路径或无活动扫描抛错。调用方仍负责验证完整文件戳。 / mark_seen updates only non-indexed generation; absent paths and inactive scans fail. The caller still validates the complete file stamp.
- 扫描热路径使用 `mark_if_unchanged(path, stamp)`：一条条件 UPDATE 比较 size、identity、modified、changed 并更新 generation，避免先取整条记录再第二次定位主键。不存在/失配返回 false，无活动扫描抛错；旧 `cached` / `mark_seen` API 保留。 / The scan hot path fuses complete stamp comparison and marking into one conditional UPDATE, avoiding record decoding and a second primary-key lookup. Missing/mismatched rows return false; inactive scans throw. Existing APIs remain available.
- 固定热语句使用 `SQLITE_PREPARE_PERSISTENT`，保留短期旁路分配池（lookaside allocator）给其他语句；这只是分配提示，不缓存查询结果。 / Persistent prepare hints preserve lookaside for short-lived statements; they do not cache query results.
- 候选和匹配遍历使用两级有序游标：先流式枚举重复桶，再复用成员游标。已有覆盖索引（covering index）提供分组/顺序，避免宽记录的全局排序；无新增索引、表或迁移。 / Two ordered cursors stream duplicate buckets then members using existing indexes, avoiding global wide-row sorting without schema/index changes.
- 遍历语句保持局部，允许既有不同遍历/插入的嵌套回调；数据库仍单线程独占。 / Traversal statements stay local, retaining existing nested callbacks across distinct operations; database ownership remains single-threaded.

## 依据与限制 / Evidence and limitations

SQLite 官方明确推荐复用 statement，避免重复 SQL 编译；reset 不清除 bindings，必须分别处理。成功路径检查 reset 返回值，异常路径不覆盖原始错误。
SQLite recommends statement reuse to avoid SQL compilation; reset does not clear bindings. Successful cleanup checks reset errors, while exceptional cleanup preserves the original failure.

- [SQLite C/C++ interface: reusing prepared statements](https://www.sqlite.org/cintro.html)
- [sqlite3_reset contract](https://www.sqlite.org/c3ref/reset.html)
- [sqlite3_clear_bindings contract](https://www.sqlite.org/c3ref/clear_bindings.html)
- [SQLite query planning: covering indexes and sorting](https://www.sqlite.org/queryplanner.html)
- [EXPLAIN QUERY PLAN](https://www.sqlite.org/eqp.html)
- [SQLite prepare flags](https://www.sqlite.org/c3ref/c_prepare_dont_log.html)

双游标减少排序但增加每桶绑定/定位，必须分别测大量小桶、单大桶和全唯一场景，不能仅凭
SQL形式宣称更快。保留当前连接单所有者、整轮事务，不为没有并发读取需求的路径引入
预写日志（write-ahead logging, WAL）的检查点管理。
Two cursors trade sorting for per-bucket binding/seeks; benchmark many small buckets, one large bucket
and all-unique inputs. The single-owner transaction model does not need WAL checkpoint machinery.

遍历回调不得依赖修改当前读取表后的可见性；应用只修改不同的临时表。
Visitors must not rely on visibility after mutating the table being traversed; application callbacks
write distinct temporary tables. See [SQLite isolation](https://www.sqlite.org/isolation.html).

[SQLite: Past, Present, and Future (PVLDB 2022)](https://www.vldb.org/pvldb/vol15/p3535-gaffney.pdf)
说明嵌入式数据库的事务与分析负载有不同瓶颈。本项目保留点查/事务路径的SQLite，
仅优化聚合遍历的查询形状，不因分析引擎的基准成绩就迁移数据库或改变文件格式。
Transactional and analytical workloads have different bottlenecks. Keep SQLite for this transactional
cache and improve aggregate query shape instead of migrating formats on unrelated engine benchmarks.

这是热路径工作量削减，不是已测得的整盘加速比。元数据 IO、目录遍历及 SQLite 页访问仍可能主导。未降低持久化保证，也未引入并发数据库连接。 / This reduces hot-path work, not a measured whole-drive speedup. Metadata IO, traversal and SQLite page access may still dominate. Durability is not weakened and concurrent database connections are not introduced.

## 验证 / Validation

2026-09-07: release x86 MSVC `store_tests` 构建成功，`ctest --test-dir build/release -R "^store$" --output-on-failure` 1/1 通过。覆盖二进制路径、完整 uint64 大小、命中/未命中交替、重复键异常后复用、回滚后复用、mark_seen 保持内容且清除未见记录、重开数据库与未来 schema 拒绝。 / Build and 1/1 test pass cover binary paths, uint64 sizes, alternating lookup hits/misses, reuse after constraint failure and rollback, mark_seen preservation/stale cleanup, reopen and future-schema rejection.
