# same

**在当前工作目录查找字节完全相同的文件；不会删除或改写用户文件。**

**Find byte-identical files below the working directory; never delete or rewrite user files.**

C++23 · BLAKE3 · SQLite · optional CUDA。扫描目录 → 只为新增或元数据变化的文件计算 BLAKE3 → 对相同大小和摘要的候选文件逐字节比较。相同摘要不是相同内容的证明。

Traverse → hash new or metadata-changed files → compare equal-size/equal-digest candidates byte for byte. Digest equality alone is not proof of content equality.

## 构建 / Build

需要 CMake ≥ 3.25、C/C++23 编译器和首次下载依赖所需的网络。依赖由 FetchContent 固定版本及 SHA-256：BLAKE3 1.8.2、toml++ 3.4.0、SQLite 3.53.4。CUDA 翻译单元（translation unit）使用 C++20，主程序使用 C++23。

Requires CMake ≥ 3.25, a C/C++23 toolchain, and network access for the first dependency fetch. Dependencies are version/hash pinned. CUDA translation units use C++20; host C++ uses C++23.

### 原生 CMake：自动选择 / Native CMake: automatic selection

**不需要 Python。** Windows 优先 CUDA Toolkit（SDK）+ Ninja 直接调用 nvcc；不可用再尝试 Visual Studio CUDA Build Customizations，最后 CPU。两条 CUDA 路线都需要 Toolkit 和兼容的 MSVC/Windows SDK；扩展不能代替 Toolkit。

**No Python required.** Windows prefers direct Toolkit/nvcc through Ninja, then Visual Studio CUDA integration, finally CPU. Both CUDA routes require the Toolkit and compatible host tools.

```sh
cmake -DSAME_BUILD_TESTS=ON -P tools/build.cmake
cmake -DSAME_CUDA_MODE=on -DSAME_BUILD_TESTS=ON -P tools/build.cmake
cmake -DSAME_CUDA_MODE=off -P tools/build.cmake
```

严格 `on` 模式要求 CUDA；`off` 跳过 CUDA。所有 `-D` 放在 `-P` 前。Windows 自动初始化 MSVC x64 环境；显式编译器/工具链优先。完整选项、回退机制及诊断见 [构建契约](docs/build.md)。

Strict on requires CUDA; off skips CUDA. Put definitions before `-P`. The launcher initializes MSVC x64; explicit toolchains take precedence. See the build contract for options and diagnostics.

### CLion / 原生配置 / Native configuration

CLion 打开根 CMake 工程，Windows 选择 **Visual Studio 工具链 + Ninja**，使用 release/cuda/cpu 预设；不要把 MinGW 与 MSVC CUDA 混用。真实 CMake CUDA 语言和 targets 提供 IDE 元数据。命令行原生配置需先初始化开发环境：

Open the root CMake project in CLion; on Windows select the **Visual Studio toolchain with Ninja** and a release/cuda/cpu preset. Native CUDA targets expose IDE metadata. Initialize the developer environment before native CLI configuration:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

直接配置尊重已有生成器，不会原地切换 Ninja/Visual Studio；跨生成器自动回退使用上面的脚本入口。构建不等于 GPU 运行验证。

Native configuration preserves the chosen generator. Use the script above for cross-generator fallback. A successful build does not establish GPU runtime availability.

## 使用与输出 / Usage and output

进入**被扫描的目录**，运行已安装的 `same`：

Run the installed executable **from the directory to scan**:

```sh
same
same --cpu
same --rehash
same --cpu --rehash
same --help
same --version
```

`--cpu` 覆盖后端配置；`--rehash` 忽略本次扫描的摘要缓存。没有目录参数。

`--cpu` overrides the backend; `--rehash` bypasses cached digests for this run. There is no directory argument.

重定向或 `--format=tsv` 时，标准输出每行是 `组号<TAB>带双引号的相对路径`，只输出至少两个成员的精确重复组。路径使用 `/`，引号和反斜杠转义，控制字节表示为 `\u00xx`。组号仅属于本次结果，不是稳定标识。

When redirected or using `--format=tsv`, each stdout line is `group-number<TAB>quoted-relative-path`. Only exact groups with at least two members are emitted. Paths use `/`; quotes/backslashes are escaped and control bytes use `\u00xx`. Group numbers are run-local, not persistent identifiers.

```text
1	"copies/a.txt"
1	"original.txt"
2	"empty-a"
2	"empty-b"
```

上述列之间实际为制表符。统计信息和诊断写入 stderr：`scanned`、`hashed`、`cached`、`groups`、`matches`、`gpu_workers`、`cpu_fallbacks`。退出码 **0** 表示扫描完成，无论是否发现重复；**2** 表示失败。输出设备故障可能留下部分输出，必须检查退出码。

Columns above are separated by actual tabs. Statistics and diagnostics go to stderr. Exit **0** means a completed scan, with or without duplicates; **2** means failure. Output-device failure can leave partial output; always check the exit code.

### 终端展示与着色 / Terminal presentation and color

交互终端默认只按组展示绿色 `[SAME]` 文件，并显示彩色 Summary 与 Profile。未找到副本的文件默认折叠，只有加上 `--unique-files` 才展开黄色 `[UNIQUE]` 列表；Summary 仍显示其数量和展开提示。`UNIQUE` 仅表示**本次扫描范围内未找到副本**，不是错误，也不是与某个指定文件的差异报告。忽略规则排除的文件不参与判断；摘要缓存仍遵循下文的信任边界。所有路径沿用 TSV 的转义规则，文件名中的控制字符不会变成终端指令。

Interactive terminals show green `[SAME]` groups and colored Summary/Profile sections. Unmatched paths are hidden by default; `--unique-files` expands the yellow `[UNIQUE]` list. Summary retains the unmatched count and expansion hint. UNIQUE means **no duplicate found within this scan**, not an error or a pairwise diff. Ignored files are outside the comparison scope; the cache trust boundary below still applies. Paths use the same escaping as TSV, including terminal control characters.

```sh
same --unique-files               # 展开未找到副本的文件 / expand unmatched paths
same --color=auto                 # 自动检测 / automatic detection (default)
same --color=never                # 无色，但保留终端排版 / plain terminal report
same --format=pretty --color=never # 在日志中保留可读排版 / readable plain logs
same --format=tsv --color=never    # 固定脚本格式 / stable script format
same --color=always               # 显式强制 ANSI / explicitly force ANSI
same > matches.tsv 2> profile.log # 结果与统计分离 / separate results and statistics
```

`--format=auto|pretty|tsv` 与 `--color=auto|always|never` 相互独立，均为命令行选项，不写入 TOML。自动颜色遵循非空 `NO_COLOR`、`TERM=dumb` 和各标准流是否连接终端；重定向默认无色并保留原 TSV。Windows 自动尝试启用虚拟终端（Virtual Terminal, VT）处理，不支持时降级无色，退出时恢复控制台模式。显式 `always` 会覆盖环境提示，即使重定向也输出 ANSI 控制码；`never` 始终无色。stdout 与 stderr 独立检测：自动模式下，重定向结果不影响终端中的 Profile，重定向统计也不会带入颜色。`--format=tsv` 始终保留原始统计字段；`--format=pretty` 显式选择可读报告与智能单位，重定向时默认仍无色。

Format and color are independent CLI-only options. Auto color honors nonempty `NO_COLOR`, `TERM=dumb`, and independent stdout/stderr terminal detection. Redirected output stays plain legacy TSV. Windows enables VT processing when supported and restores the console mode on exit. Explicit `always` overrides environment hints, including redirection; `never` suppresses all color. Auto mode formats/colors each stream independently: redirecting results does not disable a terminal Profile, and redirecting diagnostics keeps raw uncolored metrics. `--format=tsv` preserves raw profile fields; `--format=pretty` explicitly selects human-readable units, still uncolored by default under redirection.

下面示例使用 `same --unique-files`；默认不显示 UNIQUE 段。 / This example uses `same --unique-files`; the UNIQUE section is hidden by default.

```text
same | exact duplicate report
-----------------------------

[SAME] Group 1
  "copies/a.txt"
  "original.txt"

[UNIQUE] No duplicate in this scan
  "notes.txt"

Summary | 1 groups | 2 matching files | 1 unique files
  Database | .same/state.db | 32.00 KiB | 3 records | committed
```

Summary 下的彩色 `Database` 行显示 `.same/state.db` 主文件长度、本轮提交后的文件记录数和 `committed` 状态。示例中的大小仅为示意，实际值动态读取，包含数据库内部可复用空间，不含日志、临时数据库或物理磁盘分配开销。`committed` 表示扫描事务成功提交，不表示执行过完整性检查。机器统计在 stderr 另起一行输出 `database_bytes` 和 `database_records`，默认 TSV 结果不变。

The colored Database row shows the main `.same/state.db` file length, committed file-record count, and `committed` status. The example size is illustrative; the actual size is measured, includes internal reusable space, and excludes journals, temporary databases and physical allocation overhead. Committed means the scan transaction succeeded, not that an integrity check was performed. Machine diagnostics add `database_bytes` and `database_records` on a separate stderr line; default TSV results remain unchanged.

在 TSV 模式下显式使用 `--unique-files`，会在正常重复组后追加 `0<TAB>quoted-path` 行，组号 **0** 表示未找到副本，不表示这些文件彼此相同。默认 TSV 完全不变。

With explicit `--unique-files` in TSV mode, unmatched paths follow the duplicate groups as `0<TAB>quoted-path`. Reserved group **0** means unmatched, not equality among those paths. Default TSV is unchanged.

### 性能统计 / Profiling

每次成功扫描都在 stderr 输出统计；重定向到扫描根目录之外即可保存。机器模式的旧计数行保持不变，新增 `key=value` 字段另起一行；可读模式使用对齐的标签、彩色标题/数值与自适应单位，不再混排原始字段。字节与速率自动使用 B、KiB、MiB、GiB 等二进制单位，耗时自动使用 ns、us、ms、s、min 或 h；机器字段仍是精确字节数和固定毫秒。无需高频计时或每块原子操作：读取量由各工作线程独占累计，所有任务完成后求和。

Successful scans emit statistics to stderr; redirect outside the scan root to retain a log. Machine mode retains the legacy counter line and new `key=value` metrics on a separate line. Pretty mode uses aligned labels, colored headings/values and adaptive units instead of raw fields: B/KiB/MiB/GiB and higher binary units for data/rates, ns/us/ms/s/min/h for durations. Machine metrics retain exact byte counts and milliseconds. Workers accumulate read bytes locally, then totals are collected after all jobs finish; there are no per-block atomic operations or timers.

| 字段 / Field | 口径 / Meaning |
|---|---|
| `unique` | 扫描文件数减重复组成员数 / Scanned files minus duplicate members |
| `scanned_bytes` | 所有纳入文件的逻辑大小总和，硬链接按路径计数 / Logical file sizes, hard links counted per path |
| `cached_bytes` | 复用摘要的文件大小，不是本次读取量 / Sizes of hash-cache hits, not reads |
| `hash_read_bytes` | 哈希流程成功读取的字节，包含后端失败后的重读 / Successful hashing reads, including backend retries |
| `compare_read_bytes` | 逐字节验证双方的读取量，包含提前失配前读取与重试 / Reads from both comparison inputs, including early mismatches and retries |
| `read_bytes` | 上述两种读取量之和，不含 SQLite、元数据、配置与忽略文件 I/O / Hash plus comparison reads, excluding database, metadata, config and ignore I/O |
| `init_ms` | 状态、锁、数据库与工作线程初始化 / State, lock, database and worker initialization |
| `scan_ms` | 目录遍历、缓存查询、哈希与扫描事务；这些工作有重叠 / Traversal, cache lookup, hashing and scan transaction; work overlaps |
| `scan_work_ms` | 原 `scan_ms` 减主线程提交/等待哈希区间，包含遍历、元数据、缓存与数据库写入 / Pipeline wall time minus submit/join intervals; includes traversal, metadata, cache and database writes |
| `hash_wait_ms` | 主线程提交及获取哈希 future 的区间总和，包含调度开销，不是纯阻塞时间 / Main-thread submission and future-get intervals including dispatch overhead, not pure blocked time |
| `hash_work_ms` | 所有哈希任务墙钟耗时之和，包含打开、读取、摘要、文件戳校验及 CPU 回退重试，不含排队 / Summed hash-job wall time, including open/read/hash/stamp checks and CPU retries, excluding queue residence |
| `compare_ms` | 候选分组、字节比较及结果入库 / Candidate partitioning, byte comparison and result storage |
| `validate_ms` | 输出前文件戳复核 / Pre-output file-stamp validation |
| `output_ms` | 结果遍历、格式化、写入与 flush / Result iteration, formatting, writing and flush |
| `elapsed_ms` | 上述阶段之和；不含 CLI 配置加载、统计输出及析构清理 / Sum of phases, excluding CLI config loading, profile output and teardown |
| `read_mib_s` | `read_bytes / 2^20 / elapsed_seconds`，逻辑读取吞吐量 / Logical read throughput |

计时使用单调时钟（Monotonic Clock）；机器模式使用毫秒并保留三位小数，可读模式自动选择单位并保留两位小数（零值除外）；并非 CPU 时间、GPU 核函数时间或物理磁盘带宽。热摘要缓存可让哈希读取量为零，但重复候选仍需重新读取验证；操作系统页缓存（Page Cache）也会影响耗时。单次测量不是严格基准测试，比较性能时应固定输入、后端、缓存条件并重复运行。

Timings use a monotonic clock, with three decimal places in milliseconds for machine mode and two decimal places in adaptive units for human mode (except zero), not CPU time, GPU kernel time, or physical disk bandwidth. Warm digest caches can eliminate hashing reads but duplicate candidates are still reread. OS page caching affects elapsed time. For benchmarks, control input, backend and cache state and repeat measurements.

设计依据 / Design references: [NO_COLOR convention](https://no-color.org/), [Microsoft VT console processing](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences), [USENIX: Auto-pilot benchmarking methodology](https://www.usenix.org/legacy/event/usenix05/tech/freenix/full_papers/wright/wright_html/)（基准测试链接仅作为方法背景，不代表本工具已通过性能认证 / methodological context, not a performance certification）。

## 状态与配置 / State and configuration

程序自动创建真实目录 `.same`，持久化状态位于 `.same/state.db`。配置是 **`.same/config.toml`**，忽略规则是 **`.same/ignore`**；两者均可省略。`.same/run.lock` 防止同一状态目录的并发扫描；不要在运行期间删除锁文件。

The program creates a real `.same` directory, persists `.same/state.db`, and reads optional **`.same/config.toml`** and **`.same/ignore`**. `.same/run.lock` prevents concurrent runs against the same state; do not remove it while running.

优先级：默认值 → TOML → 命令行。未知键、错误类型和非法预算直接报错，不会静默忽略。所有容量单位是字节。

Precedence: defaults → TOML → CLI. Unknown keys, wrong types, and invalid budgets fail explicitly. Capacity values are bytes.

配置文件最多 64 KiB，整数配置不接受 `2.0` 或布尔值等隐式转换。

The configuration file is capped at 64 KiB; integer settings reject implicit conversions such as `2.0` or booleans.

```toml
workers = 4
block_bytes = 1048576
memory_bytes = 67108864
device_memory_bytes = 67108864
queue_capacity = 8
backend = "auto"
rehash = false
```

| 字段 / Field | 默认 / Default | 约束 / Constraint |
|---|---|---|
| `workers` | 硬件并发数限制在 1–8 / hardware concurrency clamped to 1–8 | 1–256 |
| `block_bytes` | 1048576 (1 MiB) | 1024 的正整数倍，最大 64 MiB / positive multiple of 1024, ≤64 MiB |
| `memory_bytes` | 67108864 (64 MiB) | ≥ `workers * (2*block_bytes + block_bytes/32 + 4096)` |
| `device_memory_bytes` | 67108864 (64 MiB) | 正整数 / positive integer |
| `queue_capacity` | `2 * workers` | 1–65536 |
| `backend` | `"auto"` | `"auto"`, `"cpu"`, `"cuda"` |
| `rehash` | `false` | 布尔值 / boolean |

`auto` 和 `cuda` 都尝试 CUDA，并在不可用、预算不足或计算失败时回退 CPU；`cuda` **不是强制成功、否则退出的模式**。计算失败会从头重试整个文件操作，不会拼接 CPU/GPU 的部分摘要。GPU 不保证更快：传输、启动和同步成本可能超过计算收益，应在真实文件分布上测量。

Both `auto` and `cuda` attempt CUDA and fall back on unavailability, insufficient budget, or compute failure. `cuda` is **not** a strict GPU-required mode. A failed operation is retried from the beginning. GPU acceleration is not a speed guarantee; transfers, launches, and synchronization may dominate.

**预算不是进程内存硬上限。** `memory_bytes` 约束工作线程的数据缓冲和计算暂存；SQLite 主/临时缓存各配置约 2 MiB，任务路径等元数据受队列数量约束，但不包含在该预算中。线程栈、驱动上下文、分配器和操作系统文件缓存也不包含。显存预算仅约束程序显式设备分配，不包含驱动开销。详见设计文档。

**Budgets are not hard process-RSS limits.** `memory_bytes` covers worker payload buffers/staging, not SQLite caches, queue/path metadata, stacks, allocator overhead, driver contexts, or OS file caches. Device budgets cover explicit application allocations, not driver overhead. See the design document.

## 忽略规则 / Ignore rules

这是明确界定的 glob 子集，**不是完整 `.gitignore` 兼容实现**。按大小写敏感的路径字节匹配，即使在 Windows 上也是如此。

This is a defined glob subset, **not full `.gitignore` compatibility**. Matching is case-sensitive over path bytes, including on Windows.

```text
# build outputs / 构建输出
build/
*.tmp
/cache/**
!important.tmp
```

- `#` 开头为注释，空行忽略；不剥离空格，不提供反斜杠转义或 `[]` 字符类。 / Leading `#` comments and empty lines are ignored; spaces are literal; no backslash escaping or `[]` classes.
- `*` 匹配不含 `/` 的任意字节串，`?` 匹配一个非 `/` 字节，`**` 可跨目录，`**/` 也可匹配零层目录。 / `*` stays within a component, `?` matches one non-slash byte, `**` crosses directories, and `**/` can match zero directories.
- 前导 `/` 锚定根目录；没有 `/` 的未锚定规则匹配任意层级的名称；包含 `/` 的规则相对根目录。尾部 `/` 限定目录及其后代。 / Leading `/` anchors at root; unanchored slash-free patterns match names at any depth; patterns containing `/` are root-relative; trailing `/` selects directories and descendants.
- `!` 重新包含匹配项，最后匹配的规则胜出。存在任何否定规则时保守遍历被忽略目录，以发现可重新包含的子项；否则可剪枝。 / `!` re-includes; the last matching rule wins. With any negation, ignored directories are traversed conservatively; otherwise they may be pruned.
- `.same` 始终排除，不能重新包含。忽略文件最大 1 MiB、最多 4096 条有效规则。 / `.same` is always excluded and cannot be re-included. Limits: 1 MiB ignore file and 4096 effective rules.

## 正确性边界 / Correctness boundaries

- 只处理普通文件，不跟随符号链接或 Windows 重解析点（reparse point）；状态目录必须是真实目录，其等价路径也排除。 / Only regular files are processed; symlinks and Windows reparse points are not followed; the real state directory and equivalent paths are excluded.
- 硬链接（hard link）的不同路径可以出现在同一组；成员数不是可回收空间估计。 / Multiple hard-link paths may be reported; member counts do not estimate reclaimable bytes.
- 增量缓存依赖大小、文件身份、修改和变更时间。这是元数据启发式（metadata heuristic），不是对抗性内容认证；异常时间语义可能导致漏报。 / Incremental reuse uses size, identity, modification/change timestamps. This is a metadata heuristic, not adversarial content authentication; unusual timestamp semantics can cause missed candidates.
- 候选文件即使全部命中缓存，每次仍精确比较。因此重复扫描不是零内容读取。 / Even fully cached duplicate candidates are byte-compared on every run; repeat scans are not zero-content-I/O.
- 前后句柄/路径元数据检查能检测通常的并发变化，但不是原子快照（atomic snapshot）。可靠审计应扫描静止目录或文件系统快照，必要时使用 `--rehash`；后者本身不能制造快照。 / Before/after handle/path checks detect ordinary concurrent changes, not an atomic snapshot. Audit a quiescent tree or filesystem snapshot and use `--rehash` when appropriate; rehashing alone does not create a snapshot.
- 无法读取的文件、目录遍历错误和检测到的文件变化导致失败，不伪装成完整结果。 / Read errors, traversal errors, and detected changes fail the run rather than silently claiming completeness.
- POSIX 路径允许非 UTF-8 字节，输出保留这些字节；此时应按字节读取结果，不能假定整个输出是合法 UTF-8。 / POSIX paths can contain non-UTF-8 bytes, which are preserved; consume such output as bytes rather than assuming valid UTF-8.

详见 `docs/design.md` 中的架构、预算、碰撞处理和持久化边界。

See `docs/design.md` for architecture, budgeting, collision handling, and persistence boundaries.

开发说明与源码阅读顺序见 [开发指南](docs/development.md)。 / See the [development guide](docs/development.md) for the reading map and formatting workflow.

### 扫描与哈希分项 / Separate scan and hash timing

`scan_ms` 保留原来的流水线总耗时语义，以兼容既有日志。新增三项另起一行；可读 Profile 分别展示 Scan、Hash wait、Hash work 与 Pipeline。满足 `scan_ms ≈ scan_work_ms + hash_wait_ms`（显示舍入存在误差）。`hash_work_ms` 是并发工作线程耗时之和，与扫描重叠，也可能超过流水线总耗时，**不能再加到 elapsed_ms 上**；它不是 CPU 时间或纯哈希核函数时间。全缓存命中时 Hash work / Hash wait 均为零。保持现有有界并行流水线，仅每个哈希任务与提交/获取结果区间计时，不在每个数据块上计时。

`scan_ms` retains its legacy pipeline-wall-time meaning. New fields appear on a separate log line; the human Profile separates Scan, Hash wait, Hash work and Pipeline. `scan_ms ≈ scan_work_ms + hash_wait_ms`, allowing display rounding. `hash_work_ms` sums concurrent worker durations, overlaps scanning and may exceed pipeline elapsed time; **do not add it to elapsed_ms**. It is neither CPU time nor pure hash-kernel time. Full cache hits produce zero Hash work / Hash wait. The bounded parallel pipeline is unchanged; timers bracket jobs and submit/get intervals, never individual data blocks.
