# CPU / GPU 调度基准 / CPU / GPU dispatch benchmark

`tools/dispatch_benchmark.cpp` 通过生产 `Compute` 接口测量**主机驻留输入**的完整增量 BLAKE3，而非单个设备内核。由 CMake 将它链接到生产计算库；以下假设目标名 `dispatch_benchmark`。
It measures complete incremental BLAKE3 through production Compute APIs on host-resident input, not an isolated device kernel. Link against the production compute library; examples assume target name `dispatch_benchmark`.

```powershell
.\build\cuda\dispatch_benchmark.exe --size 4194304 --block 1048576 --repeats 10 --workers 4 --trials 5 --warmup 2 --backend both
```

所有大小均为整数字节，无单位后缀；参数上限用于防止意外无界实验。`--backend cpu|cuda|both`；CUDA 请求不可用、内存不足或摘要错误均退出 2，不允许静默 CPU 回退。
Sizes use integer bytes without suffixes. Bounds prevent accidentally unbounded experiments. Unavailable CUDA, insufficient device resources, or a digest mismatch fails with exit 2, never silent CPU fallback.

## 计时与正确性 / Timing and correctness

- 固定种子生成完整文件；CPU 以不同的 65,537 字节更新边界计算 oracle。每个工作线程的每次完整摘要均比较所有 32 字节，包括预热。Deterministic input and a CPU oracle with different update boundaries; every full 32-byte digest is checked, including warmups.
- `init_ms` 为私有后端顺序构造时间，包含第一个 CUDA 实例的上下文建立与分配，不含数据生成和 oracle。所有实例保持到试验结束。Initialization includes sequential backend creation and first CUDA context setup, excludes input/oracle generation, and is reported separately.
- 各试次线程先启动并报 ready，计时从统一释放开始，到所有完成为止；包括唤醒、摘要状态创建、H2D/内核/D2H、32 字节核验和任务结束同步。Thread creation precedes timing; measured wall time includes release/wakeup, hasher allocation, transfers, kernels, validation, and completion synchronization.
- 非计时预热默认每线程一次；每试次新建宿主线程，因此线程局部运行库开销仍可能存在。短文件应增大 repeats 摊薄调度成本。Default warmup is one hash per worker. Host threads are recreated each trial, so thread-local runtime setup may remain; use enough repeats for short files.
- `both` 在奇数 trial 反转 CPU/CUDA 顺序。输出逐 trial JSONL，不隐瞒离群值；stderr 记录完整 oracle。Both alternates backend order; JSONL preserves each trial, stderr records the full oracle.

## 必须说明的边界 / Important limits

所有工作线程读取同一只读主机缓冲；这是公平的同输入比较，但可能产生共享末级缓存收益。无磁盘、文件打开、元数据或应用路由成本；**不能单凭该基准决定真实文件调度阈值**。CPU/GPU 都通过完整生产增量接口，GPU 包含真实传输；不是把输入预留在显存的有利比较。
Workers share one immutable host input, which may benefit shared last-level cache. There is no disk, file-open, metadata, or application dispatch cost. This benchmark alone cannot set a real-file routing threshold. GPU uses the complete production interface including transfers, not pre-resident device data.

显存预算每工作线程为 `3 * block + 65536` 字节，不含上下文；同时持有全部后端。应记录 GPU、驱动、CPU、构建 SHA/flags、功率状态、设备竞争和运行命令。不得与 ncu/nsys 或其他性能测试同时运行。
Per-worker explicit device budget is `3 * block + 65536`, excluding context. Record GPU/driver/CPU/build hash and flags, power state, contention, and commands. Never run competing benchmarks or profilers concurrently.

## 建议矩阵 / Suggested matrix

| 维度 / Dimension | 值 / Values |
|---|---|
| block | 1048576, 4194304, 16777216 |
| size | 4096, 65536, 1048576, 4194304, 16777216, 67108864, 268435456；必要时 / optional 1073741824 |
| workers | 1, 4, 8, 20 |
| trials | 至少 / at least 5 |

先运行短矩阵确定交叉区，再在交叉点两侧加密 size；报告配对 trial 中位数和范围，不仅最高吞吐。`aggregate_mib_s` 的字节数是 `size * repeats * workers`。用单调可靠的跨试次收益而非一个数字决定是否值得卸载。
Locate a crossover using a short matrix, then sample sizes densely around it. Report paired-trial medians/ranges, not peak throughput. Aggregate bytes are size times repeats times workers. Require reproducible gains rather than a single favorable sample.

## 有界跟踪 / Bounded trace

```powershell
nsys profile --trace=cuda,osrt --output=.cache/perf/dispatch-trace .\build\cuda\dispatch_benchmark.exe --size 16777216 --block 4194304 --workers 1 --repeats 1 --trials 1 --warmup 0 --backend cuda
```

这是单配置一次 GPU 摘要；CPU oracle 在前，且 CUDA 初始化在跟踪内，可区分初始化与实际 update/finish。`--warmup 0` 仅用于跟踪布局，不把其时间当稳态结果。工具不调用设备重置或改变运行环境。
This traces one configuration and one GPU hash; CPU oracle precedes CUDA, and setup remains visible separately from update/finish. Zero warmup is for trace interpretation, not steady-state timing. The tool never resets devices or changes the environment.

## 动态任务与混合后端 / Dynamic jobs and mixed backend

`--backend mixed` 输出 CPU 与混合后端的交替配对结果；混合后端 worker 0 使用 CUDA，其余使用 CPU。所有计时模式统一从 atomic job index 领取任务，每 trial 共 `workers * repeats` 个完整文件；`worker_jobs`、`cpu_jobs`、`cuda_jobs` 记录实际完成数。预热仍保证每个工作线程各 `warmup` 次，不采用动态抢占。
`--backend mixed` pairs CPU with one CUDA worker plus remaining CPU workers. Every timed backend uses the same atomic job index and total `workers * repeats` complete files. Worker/backend completion counts are emitted. Warmup remains fixed per worker.

混合模式应使用足够 repeats（例如 8），避免任务颗粒过粗造成末尾等待。这里模拟共享工作队列，不是每个 worker 固定等量任务；早期 `dispatch-matrix` 历史测量使用固定等量任务，应与动态结果分开解读。单 GPU 实例胜出不代表多实例共享同一 GPU 仍胜出。
Use enough repeats (e.g. 8) to reduce coarse-job tail effects. Current timing models a shared work queue; earlier `dispatch-matrix` historical results used equal fixed per-worker work and must be distinguished. One GPU instance winning does not imply many instances sharing one GPU win.
