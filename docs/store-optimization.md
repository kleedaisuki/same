# SQLite 热路径优化 / SQLite hot-path optimization

## 实现与契约 / Implementation and contracts

- `cached`, `save`, `mark_seen`, `add_representative`, `add_match` 各复用一个连接所有的预编译语句，数量恒定。 / Each operation reuses one connection-owned prepared statement; count is constant.
- 每次调用成功时显式检查 reset，异常展开时 noexcept reset；两条路径都 clear bindings，避免残留参数和路径副本。 / Success checks reset; unwinding resets without throwing. Both clear bindings to release parameters and copied paths.
- `SQLITE_TRANSIENT` 仍复制输入；查询返回拥有数据的副本，不借用游标。 / Inputs remain copied with SQLITE_TRANSIENT; queries return owned records.
- 析构先 finalize 全部缓存语句再关闭连接；事务回滚、FULL synchronous、DELETE journal 和 schema v1 不变。 / Destruction finalizes statements before connection close; rollback, FULL synchronous, DELETE journal and schema v1 are unchanged.
- `mark_seen(path)` 只更新非索引 generation 列，避免命中缓存时把 size/digest/path 索引列列入 UPDATE；缺失路径或无活动扫描抛错。调用方仍负责验证完整文件戳。 / mark_seen updates only non-indexed generation; absent paths and inactive scans fail. The caller still validates the complete file stamp.
- 遍历语句保持局部，允许既有不同遍历/插入的嵌套回调；数据库仍单线程独占。 / Traversal statements stay local, retaining existing nested callbacks across distinct operations; database ownership remains single-threaded.

## 依据与限制 / Evidence and limitations

SQLite 官方明确推荐复用 statement，避免重复 SQL 编译；reset 不清除 bindings，必须分别处理。成功路径检查 reset 返回值，异常路径不覆盖原始错误。
SQLite recommends statement reuse to avoid SQL compilation; reset does not clear bindings. Successful cleanup checks reset errors, while exceptional cleanup preserves the original failure.

- [SQLite C/C++ interface: reusing prepared statements](https://www.sqlite.org/cintro.html)
- [sqlite3_reset contract](https://www.sqlite.org/c3ref/reset.html)
- [sqlite3_clear_bindings contract](https://www.sqlite.org/c3ref/clear_bindings.html)

这是热路径工作量削减，不是已测得的整盘加速比。元数据 IO、目录遍历及 SQLite 页访问仍可能主导。未降低持久化保证，也未引入并发数据库连接。 / This reduces hot-path work, not a measured whole-drive speedup. Metadata IO, traversal and SQLite page access may still dominate. Durability is not weakened and concurrent database connections are not introduced.

## 验证 / Validation

2026-09-07: release x86 MSVC `store_tests` 构建成功，`ctest --test-dir build/release -R "^store$" --output-on-failure` 1/1 通过。覆盖二进制路径、完整 uint64 大小、命中/未命中交替、重复键异常后复用、回滚后复用、mark_seen 保持内容且清除未见记录、重开数据库与未来 schema 拒绝。 / Build and 1/1 test pass cover binary paths, uint64 sizes, alternating lookup hits/misses, reuse after constraint failure and rollback, mark_seen preservation/stale cleanup, reopen and future-schema rejection.
