# same

**在当前工作目录查找字节完全相同的文件；扫描不会删除或改写输入文件。**

**Find byte-identical files in the working directory; scanning never deletes or rewrites input files.**

C++23 · BLAKE3 · SQLite · optional CUDA / OpenCL iGPU。扫描目录 → 只为新增或元数据变化的文件计算 BLAKE3 → 对相同大小和摘要的候选文件逐字节比较。相同摘要不是相同内容的证明。

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

### CMake 预设 / CMake presets

Windows 用户请先打开 **x64 Native Tools Command Prompt for VS 2022**，再进入项目目录执行以下命令。

On Windows, open **x64 Native Tools Command Prompt for VS 2022**, then enter the project directory and run:

```sh
cmake --preset release
cmake --build --preset release
ctest --preset release
```

`release` 尝试启用 CUDA，并默认包含可选 OpenCL iGPU 后端；`cuda` 要求 CUDA 可构建；`cpu` 关闭两种加速器；`igpu` 关闭 CUDA、启用可选 OpenCL。启用 CUDA 的构建均支持运行时自动分流。完整配置与故障排查见 [构建指南](docs/build.md)。

`release` attempts CUDA and includes optional OpenCL; `cuda` requires CUDA; `cpu` disables both accelerators; `igpu` enables optional OpenCL without CUDA. CUDA-enabled builds support runtime auto routing. See the [build guide](docs/build.md) for configuration and troubleshooting.

## 使用与输出 / Usage and output

进入**被扫描的目录**，运行已安装的 `same`：

Run the installed executable **from the directory to scan**:

```sh
same                            # 等同 same scan，仅当前目录 / same scan, root files only
same scan -r                    # 递归子目录 / include subdirectories
same scan --cpu --rehash         # CPU 强制重新哈希 / CPU, bypass digest cache
same scan --igpu --rehash        # 请求核显，仍允许安全回退 / request iGPU with safe fallback
same scan -r --no-pgo            # 关闭运行时路由分析器 / disable runtime routing analyzer
same scan -r --no-telemetry      # 本轮不写遥测库 / disable telemetry persistence for this run
same scan -r --summary           # 显示汇总、数据库与性能统计 / show all statistics
same new                        # 生成标准配置与推荐忽略规则 / deploy defaults and ignore
same clean                      # 删除当前 .same / remove this directory's .same
same clean -r                   # 也删除子目录中的 .same / remove descendant states too
same --help
same --version
```

`same` 默认分发到 `same scan`；未写子命令的扫描选项仍有效，如 `same -r --cpu`。扫描默认**不进入子目录**，仅 `-r` / `--recursive` 开启递归。`--cpu` 覆盖后端配置，`--rehash` 忽略本轮摘要缓存。所有子命令以当前工作目录为根，没有目录参数。递归扫描后执行浅扫描会淘汰本轮未访问的子目录缓存；再次递归时重新建立这些缓存，旧子目录结果不会混入浅扫描。

`same` dispatches to `same scan`; implicit scan options such as `same -r --cpu` remain valid. Scans are **nonrecursive by default**; `-r` / `--recursive` enables descent. CPU overrides the backend; rehash bypasses this run's digest cache. Commands use the current directory, without a directory argument. A shallow scan prunes unseen descendant cache records from an earlier recursive scan; a later recursive scan rebuilds them rather than leaking stale results.

### 初始化与清理 / Initialize and clean

`same new` 创建 `.same/config.toml`（写全本机实际默认值）与 `.same/ignore`（推荐规则），**保留已有文件，不覆盖用户设置**。推荐规则排除版本控制元数据与操作系统目录元数据；`build/`、`node_modules/`、`.venv/` 仅以注释提供，不默认隐藏可能需要去重的内容。无需先运行 `new` 才能扫描：缺少配置时扫描使用内建默认值。

`same new` writes all host-resolved defaults and a recommended ignore file, **preserving existing files**. Recommendations exclude version-control and OS metadata; build/dependency directory rules are commented opt-ins. Initialization is optional: scans use built-in defaults when configuration is absent.

**`same clean` 会删除配置、忽略规则和缓存，不只是数据库；需要保留自定义配置时请先备份。** 默认仅删除当前目录的 `.same`；`-r` / `--recursive` 同时清理普通子目录中的所有 `.same`，不受扫描 ignore 限制。不跟随符号链接或 Windows 重解析点（reparse point）；链接形式的 `.same` 会被拒绝，不会跳转删除其目标。运行锁冲突会失败。递归清理不是事务：后续目录失败时，先前完成的清理不会回滚。

**Clean removes configuration, ignore rules and cache, not only the database; back up custom settings first.** Default clean targets the root state only; recursive clean finds descendant states independently of scan ignore rules. It does not follow symlinks/reparse points and refuses a linked `.same`. Lock conflicts fail. Recursive cleanup is not transactional: an error does not restore states already removed.

清理使用原生目录句柄固定删除范围；目录被并发替换时允许安全失败，而不会沿替换链接删除外部目标。POSIX 扫描、初始化与清理共同锁住工作目录本身，删除 `.same` 不会解除互斥。请先停止旧版进程再升级使用 `clean`：旧版 POSIX 进程不遵守新增生命周期锁协议。

Cleanup uses native directory capabilities to prevent link-redirection. Concurrent changes may fail safely. POSIX scan, initialization and cleanup lock the surviving workspace directory itself; deleting `.same` does not split lock identity. Stop older processes before using the upgraded cleaner: older POSIX versions lack this lifecycle protocol.

输出遵循结果与诊断分离、显式选择详细统计的原则，参考 [Command Line Interface Guidelines](https://clig.dev/)。 / Results and diagnostics remain separate, with detailed statistics opt-in, informed by the CLI guidelines.

重定向或 `--format=tsv` 时，标准输出每行是 `组号<TAB>带双引号的相对路径`，只输出至少两个成员的精确重复组。路径使用 `/`，引号和反斜杠转义，控制字节表示为 `\u00xx`。组号仅属于本次结果，不是稳定标识。

When redirected or using `--format=tsv`, each stdout line is `group-number<TAB>quoted-relative-path`. Only exact groups with at least two members are emitted. Paths use `/`; quotes/backslashes are escaped and control bytes use `\u00xx`. Group numbers are run-local, not persistent identifiers.

```text
1	"copies/a.txt"
1	"original.txt"
2	"empty-a"
2	"empty-b"
```

上述列之间实际为制表符。只有 `--summary` 才输出统计信息，写入 stderr：`scanned`、`hashed`、`cached`、`groups`、`matches`、`gpu_workers`、`cpu_fallbacks`。退出码 **0** 表示扫描完成，无论是否发现重复；**2** 表示失败。输出设备故障可能留下部分输出，必须检查退出码。

Columns above are separated by actual tabs. Statistics require `--summary` and go to stderr; warnings and errors remain visible without it. Exit **0** means a completed scan, with or without duplicates; **2** means failure. Output-device failure can leave partial output; always check the exit code.

### 终端展示与着色 / Terminal presentation and color

交互终端默认只按组展示绿色 `[SAME]` 文件；仅 `--summary` 显示 Summary、Database 与 Profile。未找到副本的文件默认折叠，只有加上 `--unique-files` 才展开黄色 `[UNIQUE]` 列表；开启 Summary 时显示其数量和展开提示。`UNIQUE` 仅表示**本次扫描范围内未找到副本**，不是错误，也不是与某个指定文件的差异报告。忽略规则排除的文件不参与判断；摘要缓存仍遵循下文的信任边界。所有路径沿用 TSV 的转义规则，文件名中的控制字符不会变成终端指令。

Interactive terminals show green `[SAME]` groups; `--summary` opts into Summary, Database and Profile sections. Unmatched paths are hidden by default; `--unique-files` expands the yellow `[UNIQUE]` list. When enabled, Summary retains the unmatched count and expansion hint. UNIQUE means **no duplicate found within this scan**, not an error or a pairwise diff. Ignored files are outside the comparison scope; the cache trust boundary below still applies. Paths use the same escaping as TSV, including terminal control characters.

```sh
same --unique-files               # 展开未找到副本的文件 / expand unmatched paths
same --color=auto                 # 自动检测 / automatic detection (default)
same --color=never                # 无色，但保留终端排版 / plain terminal report
same --format=pretty --color=never # 在日志中保留可读排版 / readable plain logs
same --format=tsv --color=never    # 固定脚本格式 / stable script format
same --color=always               # 显式强制 ANSI / explicitly force ANSI
same --summary > matches.tsv 2> profile.log # 结果与统计分离 / separate results and statistics
```

Summary、Database、Profile 以及下文所有性能字段均须 `--summary` 才输出；该开关不隐藏警告或错误。 / All statistics below require `--summary`; warnings and errors remain independent.

`--format=auto|pretty|tsv` 与 `--color=auto|always|never` 相互独立，均为命令行选项，不写入 TOML。自动颜色遵循非空 `NO_COLOR`、`TERM=dumb` 和各标准流是否连接终端；重定向默认无色并保留原 TSV。Windows 自动尝试启用虚拟终端（Virtual Terminal, VT）处理，不支持时降级无色，退出时恢复控制台模式。显式 `always` 会覆盖环境提示，即使重定向也输出 ANSI 控制码；`never` 始终无色。stdout 与 stderr 独立检测：自动模式下，重定向结果不影响终端中的 Profile，重定向统计也不会带入颜色。`--format=tsv` 始终保留原始统计字段；`--format=pretty` 显式选择可读报告与智能单位，重定向时默认仍无色。

Format and color are independent CLI-only options. Auto color honors nonempty `NO_COLOR`, `TERM=dumb`, and independent stdout/stderr terminal detection. Redirected output stays plain legacy TSV. Windows enables VT processing when supported and restores the console mode on exit. Explicit `always` overrides environment hints, including redirection; `never` suppresses all color. Auto mode formats/colors each stream independently: redirecting results does not disable a terminal Profile, and redirecting diagnostics keeps raw uncolored metrics. `--format=tsv` preserves raw profile fields; `--format=pretty` explicitly selects human-readable units, still uncolored by default under redirection.

下面示例使用 `same scan -r --unique-files --summary --format=pretty`；默认不显示 UNIQUE 与统计段。 / This example explicitly enables recursive scan, unmatched paths and statistics in pretty format.

```text
[SAME] Group 1
  "copies/a.txt"
  "original.txt"

[UNIQUE] No duplicate in this scan
  "notes.txt"

Summary
-----------------------------------------
  Groups          1
  Matching files  2
  Unique files    1
  Database        .same/state.db | 32.00 KiB | 3 records | committed
  Files           3 scanned | 3 hashed | 0 cached
  ...
```

Summary 表中的 `Database` 行显示 `.same/state.db` 主文件长度、本轮提交后的文件记录数和 `committed` 状态。示例中的大小仅为示意，实际值动态读取，包含数据库内部可复用空间，不含日志、临时数据库或物理磁盘分配开销。`committed` 表示扫描事务成功提交，不表示执行过完整性检查。机器统计在 stderr 另起一行输出 `database_bytes` 和 `database_records`，默认 TSV 结果不变。

The Database table row shows the main `.same/state.db` file length, committed file-record count, and `committed` status. The example size is illustrative; the actual size is measured, includes internal reusable space, and excludes journals, temporary databases and physical allocation overhead. Committed means the scan transaction succeeded, not that an integrity check was performed. Machine diagnostics add `database_bytes` and `database_records` on a separate stderr line; default TSV results remain unchanged.

在 TSV 模式下显式使用 `--unique-files`，会在正常重复组后追加 `0<TAB>quoted-path` 行，组号 **0** 表示未找到副本，不表示这些文件彼此相同。默认 TSV 完全不变。

With explicit `--unique-files` in TSV mode, unmatched paths follow the duplicate groups as `0<TAB>quoted-path`. Reserved group **0** means unmatched, not equality among those paths. Default TSV is unchanged.

### 汇总与性能统计 / Summary

只有添加 `--summary` 的成功扫描才在 stderr 输出统计；重定向到扫描根目录之外即可保存。机器模式的旧计数行保持不变，新增 `key=value` 字段另起一行；可读模式使用对齐的标签、彩色标题/数值与自适应单位，不再混排原始字段。字节与速率自动使用 B、KiB、MiB、GiB 等二进制单位，耗时自动使用 ns、us、ms、s、min 或 h；机器字段仍是精确字节数和固定毫秒。无需高频计时或每块原子操作：读取量由各工作线程独占累计，所有任务完成后求和。

Successful scans emit statistics to stderr only with `--summary`; redirect outside the scan root to retain a log. Machine mode retains the legacy counter line and new `key=value` metrics on a separate line. Pretty mode uses aligned labels, colored headings/values and adaptive units instead of raw fields: B/KiB/MiB/GiB and higher binary units for data/rates, ns/us/ms/s/min/h for durations. Machine metrics retain exact byte counts and milliseconds. Workers accumulate read bytes locally, then totals are collected after all jobs finish; there are no per-block atomic operations or timers.

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

### 跨运行遥测 / Cross-run telemetry

扫描默认在工作区的 **`.same/telemetry.db`** 保存跨运行日志、阶段追踪（tracing）、采样哈希跨度（span）、性能指标和动态分析器参数；它与摘要缓存 `.same/state.db` 独立。使用外部 SQLite 客户端查询，**same 不内置历史查询、报表或服务**；`--summary` 始终只展示本次运行。

Scans persist local cross-run logs, stage traces, sampled hash spans, metrics and analyzer parameters in **`.same/telemetry.db`**, separate from the digest cache. Query it with an external SQLite client; same has no integrated history query, report or service. Summary always describes the current run.

固定容量队列与独立写入线程将 SQL、批量提交和检查点（checkpoint）移出工作线程；队列争用或超限时丢弃遥测并统计损失，不阻塞文件任务。写入器错误即使未启用 `--summary` 也会警告。结束时仍需等待最终写入，单独报告此开销；异步不意味着零 CPU、零磁盘 I/O 或绝不丢失。

A bounded queue and dedicated writer keep SQL, batched commits and checkpoints off workers. Contention or limits drop telemetry with visible counters instead of blocking file work. Writer errors warn even without Summary. Final draining still waits and is reported separately; asynchronous does not mean zero CPU/I/O cost or losslessness.

`--no-telemetry` 本轮不打开遥测库，但不关闭在线路由；`--no-pgo` 关闭哈希性能采样与学习，但保留基础运行历史、阶段和错误。两个开关互相独立。默认保留最近 64 次运行，每轮队列事件最多 16384 条；完整结束快照独立保存。库中可能含扫描根路径、采样文件相对路径和错误消息，不含文件内容、不上传网络。`same clean` 也会删除这份历史。

`--no-telemetry` avoids telemetry DB access without disabling online routing. `--no-pgo` disables hash profiling and learning while retaining basic run history, stages and errors. Defaults retain 64 runs and cap queued events at 16384 per run; final snapshots are separate. Data may contain the root, sampled relative paths and errors, never file contents or network uploads. Clean also deletes this history.

配置、表结构、时间口径和只读 SQL 示例见 [遥测设计与检索](docs/telemetry.md)。 / See [telemetry design and queries](docs/telemetry.md) for configuration, schema, timing semantics and read-only SQL examples.

多个运行库可用独立的 [Python 合并脚本](scripts/telemetry/README.md) 汇总为新分析归档；只读源库，不向 same 主程序添加查询命令。 / Use the standalone [Python merger](scripts/telemetry/README.md) to combine journals into a new analysis archive without modifying source data or adding same query commands.

## 状态与配置 / State and configuration

程序自动创建真实目录 `.same`，持久化状态位于 `.same/state.db`。配置是 **`.same/config.toml`**，忽略规则是 **`.same/ignore`**；两者均可省略。`.same/run.lock` 防止同一状态目录的并发扫描；不要在运行期间删除锁文件。

The program creates a real `.same` directory, persists `.same/state.db`, and reads optional **`.same/config.toml`** and **`.same/ignore`**. `.same/run.lock` prevents concurrent runs against the same state; do not remove it while running.

优先级：默认值 → TOML → 命令行。仅处理下表列出的配置字段；未知或已移除的字段静默忽略，不校验其值的类型或范围。受支持字段的错误类型和非法预算仍直接报错。所有容量单位是字节。

Precedence: defaults → TOML → CLI. Only the fields listed below are processed; unknown or removed fields are silently ignored without type or range validation of their values. Wrong types and invalid budgets for supported fields still fail explicitly. Capacity values are bytes.

配置文件仍须是有效 TOML，且最多 64 KiB；语法错误或超限直接报错。受支持的整数配置不接受 `2.0` 或布尔值等隐式转换。

The configuration file must still be valid TOML and is capped at 64 KiB; syntax errors or exceeding this limit fail explicitly. Supported integer settings reject implicit conversions such as `2.0` or booleans.

```toml
# 示例为 4 个内容线程；same new 写入本机实际默认值。 / Four-worker example; new resolves host defaults.
workers = 4
metadata_workers = 4
gpu_min_bytes = 16777216
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
| `metadata_workers` | `min(workers, 4)` | 1–256；独立的目录与元数据线程 / separate directory and metadata workers |
| `gpu_min_bytes` | 16777216 (16 MiB) | 非负整数；小于阈值走 CPU SIMD，0 禁用大小路由 / smaller files use CPU SIMD; 0 disables size routing |
| `block_bytes` | 1048576 (1 MiB) | 1024 的正整数倍，最大 64 MiB / positive multiple of 1024, ≤64 MiB |
| `memory_bytes` | 67108864 (64 MiB) | ≥ `workers * (2*block_bytes + block_bytes/32 + 4096)` |
| `device_memory_bytes` | 67108864 (64 MiB) | 正整数 / positive integer |
| `queue_capacity` | `2 * workers` | 1–65536 |
| `backend` | `"auto"` | `"auto"`, `"cpu"`, `"cuda"`, `"igpu"` |
| `rehash` | `false` | 布尔值 / boolean |
| `igpu_bootstrap_ms` | `100.0` | 缺少历史时的核显初始化估计，毫秒 / Initial setup estimate without history, ms |
| `cuda_bootstrap_ms` | `100.0` | CUDA 冷启动预检估计，毫秒 / CUDA preflight estimate, ms |
| `cold_exploration_fraction` | `0.05` | 自动 PGO 本轮冷启动信用比例，不是概率或硬耗时保证 / Run-local cold-start credit fraction, not a guarantee |
| `pgo` | `true` | 运行时剖析引导路由；`--no-pgo` 可覆盖关闭 / Runtime profile-guided routing; CLI can disable |
| `telemetry` | `true` | 持久化本地遥测；`--no-telemetry` 可覆盖关闭 / Persist local telemetry; CLI can disable |
| `telemetry_queue_capacity` | `4096` | 1–65536；事件槽数，不是字节 / Event slots, not bytes |
| `telemetry_retention_runs` | `64` | 1–4096；包含本轮 / Retained runs including current |
| `telemetry_max_events` | `16384` | 1–1000000；每轮队列事件上限，不含最终快照 / Per-run queued-event cap, excluding final snapshot |

`auto` 使用固定 `workers` 个统一工作线程，共享一个先入先出任务队列；每个线程根据自己的真实任务统计选择 CPU SIMD、自己的 CUDA 执行流（stream）或池内 OpenCL iGPU。没有额外 GPU 服务线程，也没有中央模型训练锁。
默认小于 16 MiB 直接 CPU；资格仅由 `gpu_min_bytes` 控制，与 CPU `block_bytes` 无关。
自动路由使用 CPU、CUDA 和 OpenCL iGPU 三个候选，基于 payload、实际批量与在途竞争的四维服务成本模型。设备特征用于身份隔离；工作线程独占模型和增量。iGPU 单实例忙时不等待，CUDA 使用线程私有流；预算、大小门槛与正确性检查独立于模型。预测不是全局最优或性能提升保证。

Auto routing predicts service cost for CPU, CUDA and OpenCL iGPU using payload, effective batches and concurrent peers. Device profiles namespace worker-local learning. Busy iGPU admission never waits; CUDA streams remain worker-private. Eligibility and correctness override predictions.

`pgo=true` 默认启用运行时剖析引导优化（profile-guided optimization, PGO）。独立 `.same/model.db` 保存跨运行充分统计量，按设备/驱动/实现与配置匹配，运行边界衰减先验。`--no-telemetry` 不关闭该学习库；`--no-pgo` 不加载、更新或保存模型，并使用静态分流，不等同于 `--cpu` 或编译器 PGO。`--summary` 仅控制展示。

PGO persists model statistics independently of telemetry. Disabling telemetry preserves learning; disabling PGO disables model access and uses static routing. Summary visibility is independent.

数学定义、统计口径、探索局限与持久化契约见 [上下文学习](docs/contextual-learning.md)。旧架构报告保留为历史证据，不证明当前实现性能。
See [contextual learning](docs/contextual-learning.md) for mathematics and limitations; historical routing benchmarks do not establish current performance.

设备探测与激活分离，自动 PGO 仅为实际请求设备执行共享非阻塞冷启动预检，用已知 CPU 任务潜力或本轮探索信用覆盖估计；取得 profile 后再检查 iGPU 预测节省；`--igpu` 绕过自动门槛。初始化历史与稳态模型分开保存。旧策略的负收益保留于 [扫描性能记录](docs/contextual-scan-performance.md)，最终修订已完成配对实测，仍有 PGO 开销，不宣称普遍加速。 / Probe and activation are separate; automatic cold admission uses predicted savings or run-local credit. Setup history is independent of steady-state learning. Paired final measurements retain earlier negative evidence and disclose remaining PGO overhead; universal speedup is not claimed.

### 有界扫描流水线 / Bounded scanning pipeline

目录枚举与文件元数据构成动态任务图：`metadata_workers` 个线程共享有界任务队列，满时就地深度优先处理，不等待递归提交。结果流入单线程数据库协调器，缓存未命中交给 `workers` 个内容工作线程。哈希按完成顺序收取，慢首任务不阻塞后完成结果。`queue_capacity` 分别约束遍历任务、遍历结果和未收取哈希任务；它不是所有队列合计的容量。

Directory and file-metadata work form a dynamic task graph. Metadata workers share a bounded queue and process overflow depth-first instead of blocking on recursive submission. A single-owner database coordinator checks cache records and sends misses to content workers. Hash results are consumed in completion order. `queue_capacity` independently bounds traversal tasks, traversal results and unconsumed hash jobs, not their aggregate sum.

元数据取得的打开句柄直接移交给哈希，缓存命中则关闭，减少重复打开；读取前后的新鲜文件戳与路径绑定检查仍保留。每个结果最多携带一个打开句柄；队列之外还包括执行中的线程及深度优先目录游标，因此预算不是句柄数或进程内存的硬限制。

Metadata handles transfer directly to hashing or close on cache hits, avoiding a duplicate open while retaining fresh pre/post-read object and path-binding checks. Each result carries at most one open file; active workers and depth-first directory cursors add resources outside queue lengths. Budgets are not hard handle/process-memory limits.

`cpu_routed_hashes` 是大小策略路由次数，不是失败回退。`cpu_hashes` / `gpu_hashes`
统计真实文件哈希尝试，不包括设备检查。`gpu_setup_ms` 展示初始化成本；运行时不再执行
合成性能校准，设备检查成功后 `calibration_stop=not-run-device-checked`，旧校准耗时字段
保留为零，不表示 GPU 没优势。`gpu_block_bytes` 是实际 GPU 更新块，不代表已经执行文件任务。
精确比较始终在主机完成。

Size routing is not failure fallback. Hash-attempt counters exclude device checks. Setup metrics expose
initialization costs. Runtime synthetic calibration is not run: after device validation,
`calibration_stop=not-run-device-checked` and legacy calibration timings remain zero, not evidence of
GPU inferiority. GPU block size alone does not prove file execution; comparison remains host-side.

持续剖析（continuous profiling）在线程局部聚合任务级样本，并在运行边界保存模型统计量，不采集操作系统调用栈，
不逐文件记录路径、不逐块计时，也不写入摘要数据库。固定大小统计包括 CPU/CUDA/iGPU 样本数、
已知大小区间、预测误差和有界延迟分布；它们通过 `--summary` 展示。
该机制追求低开销，而不是声称数学意义上的零成本；性能结论须由对照实验支持。

Continuous profiling aggregates bounded worker-local task statistics and persists model statistics at run boundaries; it is not an OS stack sampler.
It adds no per-file path log, per-block timers or digest-database writes. Summary reports backend sample
counts, known bands, prediction errors and bounded latency distributions. Low overhead must be measured,
not asserted as literally zero.

新增统计：`walk_wait_ms` 是协调线程等待遍历结果的时间；`enumerate_work_ms`、`metadata_work_ms` 是各工作线程累计时间，不应与总耗时相加；`database_work_ms` 为扫描协调器数据库操作时间。旧 `scan_work_ms` 字段仍为 `scan_ms - hash_wait_ms`，现在包含等待元数据的时间，不是 CPU 工作时间。`walk_task_peak`、`walk_result_peak` 显示队列峰值。

New metrics separate coordinator walk wait, summed enumeration/metadata worker time, coordinator database work, and traversal queue peaks. Legacy `scan_work_ms = scan_ms - hash_wait_ms` is retained and includes metadata waiting, not CPU execution time. Summed worker times overlap and must not be added to elapsed time.

研究、实现与复现实验 / Research and reproducibility: [遍历调度](docs/traversal-optimization.md)、[小文件 I/O](docs/small-file-io.md)、[GPU 融合](docs/gpu-optimization.md)、[数据库](docs/store-optimization.md)、[扫描基准](docs/scan-benchmark.md)。

**预算不是进程内存硬上限。** `memory_bytes` 约束工作线程的数据缓冲和计算暂存；SQLite 主/临时缓存各配置约 2 MiB，任务路径等元数据受队列数量约束，但不包含在该预算中。线程栈、驱动上下文、分配器和操作系统文件缓存也不包含。显存预算仅约束程序显式设备分配，不包含驱动开销。详见设计文档。

**Budgets are not hard process-RSS limits.** `memory_bytes` covers worker payload buffers/staging, not SQLite caches, queue/path metadata, stacks, allocator overhead, driver contexts, or OS file caches. Device budgets cover explicit application allocations, not driver overhead. See the design document.

## 忽略规则 / Ignore rules

忽略文件固定为扫描根目录下的 `.same/ignore`，采用 [Git 官方 gitignore 模式语法](https://git-scm.com/docs/gitignore)。规则相对于**扫描根目录**，不是 `.same` 内部。不会加载根目录或子目录中的 `.gitignore`、Git 全局配置或索引；这是模式语义兼容，不是 Git 多来源规则发现机制。匹配区分大小写（Windows 也一样），以路径字节为单位。

The sole source is root `.same/ignore`, using Git's pattern syntax relative to the **scan root**. Root/nested `.gitignore` files, global Git configuration and the Git index are not loaded. Matching is case-sensitive over path bytes, including on Windows; pattern compatibility does not imply Git's multi-source discovery.

```text
# 构建产物 / Build outputs
build/
*.tmp
/cache/**
!important.tmp
# 保留父目录，才能恢复其中文件 / Keep the parent traversable to restore a child
scratch/*
!scratch/keep.txt
# 字面井号与字符类 / Literal hash and character class
\#notes
log[0-9].txt
```

- 空行忽略，前导 `#` 为注释；`\#`、`\!` 匹配字面字符；反斜杠转义，未转义的尾部空格剥离。 / Empty lines and leading comments are ignored; backslashes escape characters and unescaped trailing spaces are stripped.
- `*`、`?` 不跨 `/`；支持 `[]` 字符类及范围；`**/`、`/**/`、`/**` 表达跨目录匹配。 / Wildcards stay within components; bracket classes/ranges and directory globstars are supported.
- 前导或中间 `/` 使模式相对于根；无此分隔符的模式匹配任意层级名称；尾部 `/` 仅匹配目录。 / Leading/interior slashes anchor patterns; slash-free names match any depth; trailing slashes restrict matches to directories.
- 最后匹配规则胜出；`!` 取消忽略，但**不能恢复仍被忽略的父目录内的文件**。例如 `scratch/` 后跟 `!scratch/keep.txt` 无效，应改为上面的 `scratch/*`，或先恢复父目录。被忽略目录直接剪枝。 / Last match wins, but negation cannot restore children of an excluded parent; excluded directories are pruned.
- `.same` 始终排除且不可重新包含。忽略文件最大 1 MiB、最多 4096 条有效规则。 / `.same` cannot be re-included; limits are 1 MiB and 4096 effective rules.

## 正确性边界 / Correctness boundaries

- 只处理普通文件，不跟随符号链接或 Windows 重解析点（reparse point）；状态目录必须是真实目录，其等价路径也排除。 / Only regular files are processed; symlinks and Windows reparse points are not followed; the real state directory and equivalent paths are excluded.
- Windows 长路径：可执行文件声明 `longPathAware`，原生属性查询、读取及运行锁使用扩展长度路径；输出与缓存路径保持不变。完整扫描仍要求 Windows 启用 `LongPathsEnabled`，以覆盖 CRT/STL 文件操作；程序不会修改系统策略。 / Windows long paths: executables declare `longPathAware`; native attribute queries, reads and run locks use extended-length paths without changing output or cache keys. Full scans still require Windows `LongPathsEnabled` for CRT/STL operations; the program never modifies system policy.
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
