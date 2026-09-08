# 按载荷与设备可用性分流 / Payload- and availability-aware dispatch

本文定义 `backend="auto"` 的当前目标契约，取代旧版“累计 4 GiB 待处理任务后才探测”的策略。
历史测量仍保留在 [dispatch-results.md](dispatch-results.md)，不能将其旧版路由结果当作本策略的新测量。
This contract supersedes the pending-4-GiB probe gate. Historical measurements describe earlier routing,
not measured performance of this policy. Design rationale: [adaptive-routing-design.md](adaptive-routing-design.md).

## 载荷分类 / Payload classes

CPU 使用上游 BLAKE3 单指令多数据运行时分派（SIMD runtime dispatch），默认读取块仍为 1 MiB。
GPU 有独立服务与输入缓冲，优先采用 `max(block_bytes,16 MiB)` 更新块；若预算不足，则尝试
配置的 `block_bytes`，仍不足时禁用自动 GPU 服务，而不是使原本合法的 CPU 配置失效。
CPU retains upstream SIMD dispatch and the default 1 MiB block. The independent GPU service prefers
`max(block_bytes,16 MiB)`, falls back to the configured block if budgets require, then disables only
automatic GPU service if that also cannot fit. Existing valid CPU configurations remain valid.

令 `C` 为 CPU 块大小，`G` 为实际 GPU 块大小，`E=max(gpu_min_bytes,C)`，
`L=max(64 MiB,4*G)`。默认 `gpu_min_bytes=16 MiB`，是自动卸载资格下界，不是通用性能交叉点。
Let `C` and `G` be actual CPU and GPU block sizes, `E=max(gpu_min_bytes,C)`, and
`L=max(64 MiB,4*G)`. The default 16 MiB eligibility floor is not a universal crossover.

| 类别 / Class | 判据 / Rule | 默认执行与互助 / Execution and assistance |
|---|---|---|
| `cpu_only` | 文件小于 `E` / size below `E` | 仅 CPU，即使 CPU 全忙 / CPU only, even when CPUs are busy |
| `cpu_preferred` | 达到 `E`，对应校准未证明稳定 GPU 优势 / eligible without demonstrated GPU advantage | CPU 优先；全部 CPU 正在执行任务时，空闲 GPU 可接手 / CPU first; idle GPU assists when all CPUs are active |
| `gpu_preferred` | 达到 `E`，对应校准证明 GPU 至少快 20% / eligible with stable ≥20% GPU advantage | GPU 优先；GPU 忙时空闲 CPU 可接手 / GPU first; idle CPU assists when GPU is busy |

小于 `L` 的合格文件使用单 GPU 块形状证据；达到 `L` 的长载荷使用四 GPU 块流式证据。
两种形状独立决策，不能用长流收益替小载荷背书，也不能因短流不胜而抹掉长流机会。
Eligible files below `L` use one-GPU-block evidence; long payloads use four-block streaming evidence.
The shapes are independent: long-stream wins do not justify short-stream preference, and short-stream
losses do not erase long-stream opportunities.
每类三轮交替CPU/GPU采样，最慢GPU仍须比最快CPU快20%，避免中位数掩盖抖动。
Three alternating paired rounds per shape require slowest GPU to beat fastest CPU by 20%.
未达到实际GPU单块输入长度的任务不外推单块优势，仍为CPU偏好、可在饱和时卸载。
Below the actual GPU block length, do not extrapolate a block win; prefer CPU with saturation spill.

## 服务与调度 / Services and scheduling

- `N` 路 CPU 加最多一路独立 GPU；不拿走一条 CPU 通道充当 GPU。
  Keep `N` CPU lanes plus at most one independent GPU service, not `N-1` CPUs plus a GPU.
- 第一个未缓存、合格哈希触发一次后台 GPU 设置与有界校准；CPU 流水线继续运行。
  初始化仍有真实延迟，不承诺小批次能摊销。纯小文件与缓存命中不触发 GPU 设置。
  The first uncached eligible hash triggers background bounded setup/calibration while CPU work
  continues. Setup still costs time; short scans may not amortize it. Small-only/cached scans do not probe.
  初始化期间只保留一个最大候选句柄，其余正常入队；EOF等待或异常展开会先等待后台初始化。
  暂存一项元数据不预读内容，完成环的容量不变；不会迁移已运行的Hasher。
  Retain only one largest candidate handle during startup; submit other work normally. EOF/unwinding
  joins startup. This extra metadata slot does not prefetch bytes or enlarge the completion ring.
- GPU 可用性与性能偏好是两件事。校准未胜出不删除可工作的 GPU，它仍可在 CPU 全忙时互助。
  Availability differs from preference: a functioning GPU that loses calibration remains available
  for CPU-saturation assistance.
- “CPU 全忙”是锁内维护的 `cpu_active == N`，不是“CPU 队列非空”。任务领取、活动计数与唤醒
  必须保持一致。GPU 不领取 `cpu_only` 或文件比较任务。
  CPU saturation uses lock-protected active counts, not queue nonemptiness. Selection, counters, and
  wakeups stay consistent. GPU never takes CPU-only hashes or file-comparison jobs.
- 互助只在任务领取边界发生，不迁移进行中的哈希状态。队列与未收取结果仍保持有界背压
  （backpressure）；独立 GPU 缓冲必须计入资源预算。
  Assistance happens at job acquisition, never by migrating a live hasher. Queue/completion admission
  remains bounded, and the additional GPU buffer belongs in resource budgets.
- GPU空闲时为它预留一项优先任务；有多项积压时CPU可从队尾领取其余任务，不白等。
  Reserve one preferred job for an idle GPU; CPUs may take excess backlog from the tail immediately.

## 兼容与失败 / Compatibility and failure

`backend="cpu"` 不初始化 CUDA；显式 `backend="cuda"` 保留其原有选择、大小下界与完整 CPU
重试语义。本次只改变自动模式的偏好与互助策略。
Explicit CPU avoids CUDA setup; explicit CUDA retains its prior selection, floor, and complete CPU retry.
Only automatic preference and assistance change here.

`gpu_probe_bytes` 原配置键和字段继续接受，但退役为兼容输入；包括 `0`、默认 `4294967296`
在内均不再控制自动 GPU 初始化。移除 4 GiB 门槛是用户要求的行为变更：不能因为尚未累计
足够批次，就禁止 GPU 在 CPU 全忙时帮助合格任务。旧值不再代表摊销保证。
The existing `gpu_probe_bytes` key/field remains accepted as a retired compatibility input. Neither
zero nor the old 4-GiB default gates setup now. This deliberate change implements requested assistance
without a batch-volume prerequisite; the old value no longer implies amortization protection.

CUDA 设置/计算不可用时走 CPU。哈希计算失败须从头完整 CPU 重试，不拼接不同后端的部分
状态；GPU 服务失败后退休，由 CPU 排空余下队列。不得让失败的独立服务成为额外、未预算的
CPU 工作线程。错误摘要不能被当作有效结果。
Unavailable CUDA falls back to CPU. Failed hashing retries fully from the beginning, never by joining
partial backend states. Failed GPU service retires and CPUs drain pending work; it must not become an
extra unbudgeted CPU lane. Incorrect digests are never accepted as valid results.

## 验证范围 / Verification scope

本次验收须覆盖：分类边界、两种形状分别胜/负、全部 CPU 活动时 GPU 互助、GPU 忙时 CPU
互助、CPU 未满时保留 CPU 偏好、小文件与比较任务不进 GPU、预算降级、缓存跳过初始化、
GPU 失败后完整重试及队列排空、显式模式与旧配置兼容。
Acceptance covers class boundaries, independent shape outcomes, bidirectional assistance, CPU preference
without saturation, CPU-only exclusions, budget fallback, cache-only laziness, full retry/draining,
explicit modes, and old configuration acceptance.

确定性 CPU 持续集成（continuous integration, CI）使用注入式假 CUDA 后端验证调度，不依赖
本机有设备或偶然的计时胜出；实机正确性与端到端性能须另行记录。此节列出验收要求，
不宣称任何尚未执行的测试通过，也不从历史微基准推导本实现的加速比例。
Deterministic CPU CI uses an injected fake CUDA backend, not hardware presence or lucky timing wins.
Real-device correctness and end-to-end performance require separate evidence. These are acceptance
requirements, not claims of completed tests or speedups extrapolated from historical microbenchmarks.
