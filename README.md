# same

**在当前工作目录查找字节完全相同的文件；不会删除或改写用户文件。**

**Find byte-identical files below the working directory; never delete or rewrite user files.**

C++23 · BLAKE3 · SQLite · optional CUDA。扫描目录 → 只为新增或元数据变化的文件计算 BLAKE3 → 对相同大小和摘要的候选文件逐字节比较。相同摘要不是相同内容的证明。

Traverse → hash new or metadata-changed files → compare equal-size/equal-digest candidates byte for byte. Digest equality alone is not proof of content equality.

## 构建 / Build

需要 CMake ≥ 3.25、C/C++23 编译器和首次下载依赖所需的网络。依赖由 FetchContent 固定版本及 SHA-256：BLAKE3 1.8.2、toml++ 3.4.0、SQLite 3.53.4。CUDA 翻译单元（translation unit）使用 C++20，主程序使用 C++23。

Requires CMake ≥ 3.25, a C/C++23 toolchain, and network access for the first dependency fetch. Dependencies are version/hash pinned. CUDA translation units use C++20; host C++ uses C++23.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix /your/install/prefix
```

默认探测 CUDA 编译器；没有编译器时自动构建 CPU 版本。只构建 CPU：

The default probes for a CUDA compiler and builds the CPU fallback when absent. For an explicitly CPU-only build:

```sh
cmake -S . -B build-cpu -DCMAKE_BUILD_TYPE=Release -DSAME_ENABLE_CUDA=OFF
cmake --build build-cpu --config Release --parallel
ctest --test-dir build-cpu -C Release --output-on-failure
```

| 平台 / Platform | 验证范围 / Verification scope |
|---|---|
| Linux / WSL | CPU 与 CUDA 已测试；CUDA 12.8、RTX 3070 Ti / CPU and CUDA tested, CUDA 12.8 on RTX 3070 Ti |
| Windows / MinGW | CPU 已测试 / CPU tested |
| Windows / MSVC 19.44 | CUDA 13.3 构建和 CPU 回退已测试；本机该运行时无法初始化 GPU / CUDA 13.3 build and CPU fallback tested; this runtime could not initialize the local GPU |
| macOS | POSIX 分支包含元数据适配；尚未验证，无 CUDA / Metadata adaptation present, unverified, CPU only |

Python 3 可用时 CTest 还运行端到端测试。支持的非 MSVC 工具链可通过 `-DSAME_SANITIZERS=ON` 开启 AddressSanitizer/UndefinedBehaviorSanitizer；不应将其视为 CUDA 内核检查器。

CTest also runs integration tests when Python 3 is available. `SAME_SANITIZERS=ON` enables host address/undefined-behavior sanitizers on supported non-MSVC toolchains, not CUDA kernel validation.

使用 Ninja 的预设（preset）：`cmake --preset cpu`、`cmake --build --preset cpu`、`ctest --preset cpu`；另有 `release` 和 `asan`。GPU 测试设置 `SAME_REQUIRE_CUDA=1` 可禁止静默跳过。详见 [验证记录](docs/validation.md)。

Ninja presets are `cpu`, `release`, and `asan`. Set `SAME_REQUIRE_CUDA=1` to make missing CUDA fail GPU tests instead of skipping them. See the [validation record](docs/validation.md).

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

标准输出每行是 `组号<TAB>带双引号的相对路径`，只输出至少两个成员的精确重复组。路径使用 `/`，引号和反斜杠转义，控制字节表示为 `\u00xx`。组号仅属于本次结果，不是稳定标识。

Each stdout line is `group-number<TAB>quoted-relative-path`. Only exact groups with at least two members are emitted. Paths use `/`; quotes/backslashes are escaped and control bytes use `\u00xx`. Group numbers are run-local, not persistent identifiers.

```text
1	"copies/a.txt"
1	"original.txt"
2	"empty-a"
2	"empty-b"
```

上述列之间实际为制表符。统计信息和诊断写入 stderr：`scanned`、`hashed`、`cached`、`groups`、`matches`、`gpu_workers`、`cpu_fallbacks`。退出码 **0** 表示扫描完成，无论是否发现重复；**2** 表示失败。输出设备故障可能留下部分输出，必须检查退出码。

Columns above are separated by actual tabs. Statistics and diagnostics go to stderr. Exit **0** means a completed scan, with or without duplicates; **2** means failure. Output-device failure can leave partial output; always check the exit code.

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
