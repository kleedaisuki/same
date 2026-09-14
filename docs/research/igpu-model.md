# iGPU 接入：数学模型与动态分析器设计

日期：2026-09-10。状态：**研究方案，未实现、未验证协同收益**。本文件只讨论计算调度与测量；设备恢复、文件一致性与 I/O/缓存方案应共同审查。

## 1. 决策与当前证据

目标是在摘要、文件一致性和资源上限不变的条件下，降低整次扫描完成时间（makespan），而不是最大化设备利用率。第一阶段将不同文件分配到 CPU / CUDA / iGPU；不拆分同一文件的 BLAKE3 树给多个设备。后者需要额外树状态协议和重放设计，当前证据不支持其复杂度。

**建议：保留单有界队列、固定工作线程和线程私有模型，以显式后端标识替换布尔 GPU；增加有界 iGPU 准入、分离测量窗口及争用诊断。先测系统净收益，再启用自动路由。**

| 已检查的证据 | 观察 | 对设计的约束 |
|---|---|---|
| `src/online_model.cpp`、`include/same/detail/online_model.hpp` | 两后端、32 个四倍大小区间；每字节成本/绝对残差 EWMA，alpha=1/8 | 可保持定长状态；`bool gpu` 无法区分两种 GPU；残差不是置信区间 |
| `src/resources.cpp`、`include/same/resources.hpp` | 出队后本线程选择；每线程独占模型、流、缓冲；GPU 活跃数只作诊断 | 不能凭空加入独立设备队列等待时间，不能复制第三套全额内存预算 |
| `src/application.cpp` 哈希回调 | 当前 `elapsed` 包括 `hash_file` 的文件工作；失败/重试不训练 | 是任务服务时间，不是纯内核时间；排队、初始化及失败仍须进入端到端成本 |
| `src/dispatch.cpp` | 校准比较完整摘要、含同步传输，交替顺序、三次样本；`stable_gpu_win` 要求 20% 优势 | 当前在线选择主要使用本地观测，不应误称每次启动都有这套性能校准 |
| `include/same/compute.hpp` | 同步 `update` 返回后输入可复用；`finish` 不重置；固定 32 字节摘要 | iGPU 内部异步不得改变公开缓冲生命周期与摘要语义 |
| `docs/unified-worker-design.md` | 明确不是中央最早完成时间调度 | 扩展不得偷偷恢复第二套中央训练器 |
| `docs/igpu-blake3-experiment.md`、`benchmarks/igpu-20260910/opencl.jsonl` | 大块 OpenCL 原型可正确计算；输入仅二次幂且 >=2 KiB | 不能作为生产后端或任意形状校准先验 |

现有报告中，16 MiB 含上传核显吞吐约 6,975 MiB/s，64 MiB 约 7,917 MiB/s；小输入明显较慢。CPU/CUDA 原生对照来自不同批次，不能组成严格三设备排名。八 CPU 工作线程共享输入的约 19,256 MiB/s 不等于文件吞吐。上述观察支持“大块值得研究”，不支持“CPU+iGPU 一定更快”。原始文件存在且已抽查结构，本方案未重跑性能实验。

## 2. 数学对象：局部服务成本不等于系统收益

### 2.1 时间与单位

对文件任务 j 和后端 d∈{cpu,cuda,igpu} 定义：

| 符号 | 单位 | 定义 |
|---|---|---|
| n | byte | 实际完整输入大小；不是预估大小，预测与实测大小差异单独标记 |
| b_d | byte | 后端实际 update 块大小；与 n 分离 |
| m=ceil(n/b_d) | 次 | 实际提交块数；实现可能另有尾块 CPU 路径，需记录 |
| t_enqueue,t_dequeue,t_start,t_done | ms，单调时钟 | 入队、出队、初始化结束后开始文件操作、验证并完成操作 |
| Q=t_dequeue−t_enqueue | ms | 公共任务队列等待，只计一次 |
| A | ms | 本次冷初始化/编译/资源准备；单列，计入总耗时 |
| S_d=t_done−t_start | ms | 完整单次文件服务时间，包括读取、校验、同步计算 |
| H_d | ms | 对 `hasher/update/finish` 的互不重叠墙钟区间求和；包含设备提交/等待/传输 |
| R_d | ms | 文件读取区间总和，未来异步流水线中可能与 H 重叠 |
| E_d | ms | 对预测 S 的绝对残差 EWMA，不是统计置信上界 |
| z | 无量纲类别 | 争用状态：采样窗口内 CPU/iGPU/CUDA 活跃情况及配置代际 |

端到端单任务延迟为 Q+A+S（失败重放还需加失败尝试时间）。当前同步路径可用互斥计时段分解 S；异步 I/O 后，**不得用 R+H 替代真实墙钟 S**。理想重叠下 max(R,H) 是忽略其他开销的下界，不是已达到的吞吐。

### 2.2 解释性模型（用于实验，而非立即训练所有参数）

主机驻留输入的计算成本可写为：

`H_d(n,b,z) ≈ a_d + m*l_d + n/r_d(z) + X_d(n,b,z) + F_d(n,b)`。

a 为每摘要固定成本(ms)，l 为每提交开销(ms/次)，r 为有效吞吐(byte/ms)，X 为复制/映射/一致性成本(ms)，F 为尾块与最终树归并成本(ms)。这些项未必可辨识：若 b 固定，则 m 与 n 高相关，不能从同一文件大小序列独立拟合 l、r。需要固定 n 扫 b、固定 b 扫 n，以及驻留/上传实验；否则只拟合总成本。

简单单块模型 H_cpu=a_c+n/r_c、H_i=a_i+n/r_i 给出交叉点

`n*=(a_i−a_c)/(1/r_c−1/r_i)`。

只在分母为正且 n* 位于测量范围内时有意义；多块、树层跃迁、动态频率或传输改变时不外推。现有四倍区间的 `S≈n*c` 会把固定开销错误按 n 缩放。首版仍保留当前 32 个四倍区间，使用 GPU 最小输入资格排除启动开销主导区域；只增加第三后端并不证明模型充分。更窄二倍区间、显式单块/流式形状或仿射模型均为后续替代项，只有残差诊断与消融证明净收益后才引入。

### 2.3 共享资源与反事实

CPU 与 iGPU 共享 DRAM，CUDA 上传和文件缓存也消耗主机带宽。必要而非充分的容量约束为：

`Σ_d x_d*v_d + B_io + B_other ≤ B_dram`。

x 是文件处理速率(byte/ms)，v 是每输入字节引起的 DRAM 字节量(byte/byte)，B 是 byte/ms。CPU+iGPU 还受共享功耗与散热约束；不能把功率上限当相互独立预算，也不能把 BLAKE3 理论指令数转换成本机实际吞吐。Intel 的共享 DRAM 文档支持资源耦合，但本机实际饱和点仍须测量。[Intel GPU Roofline](https://www.intel.com/content/www/us/en/docs/oneapi/optimization-guide-gpu/2023-0/optimize-memory-bound-applications-with-gpu.html)

局部选 iGPU 的必要启发式：`S_i+E_i < 0.95*S_baseline−E_baseline`，沿用现有 5% 优势门槛作为**初始策略参数**，不是证明。baseline 是当次可用 CPU/CUDA 中的保守选择。若两边不确定区间重叠，维持基线。已知但样本不足不等于可信。

最强竞争解释是：iGPU 单任务更快，却拖慢其他 CPU 文件，使全局更慢。因此系统决策要比较

`Δ = M(best CPU/CUDA baseline) − M(same budget + iGPU)`，

其中 M 是同一工作集的完成时间，必须按重复实验的配对差估计。不能用局部差 `S_cpu−S_i` 代替 Δ；也不能将 CPU/iGPU 的单独最大吞吐相加。第一阶段通过离线组合实验决定可启用并发范围；不在生产中从几个相关样本估计“对其他所有任务的外部损害”。

## 3. 最小生产模型与动态分析器

### 3.1 数据结构和所有权

建议内部 `BackendId {cpu,cuda,igpu}`，`BackendSlot` 持有能力、资源和生命周期，`Selection` 持有本次后端与准入票据。票据使用资源获取即初始化（Resource Acquisition Is Initialization, RAII）保证所有退出路径释放；不再增加一组 `igpu_selected/igpu_retired/...` 布尔组合。

每线程定长模型首版按 `(BackendId, band)` 索引，保留当前 32 个四倍大小区间；block 配置代际改变时重建模型。`quiet/mixed/changed` 仅作为诊断标签，不增加模型键维度。允许的 CPU/CUDA/iGPU 混合并发范围须先通过离线端到端门槛；quiet 样本的胜利不能证明未验证 mixed 组合可启用。不要枚举 CPU数×GPU数×大小×温度×盘符的巨大笛卡尔积。保持工作线程写自己的模型；中央仅维护原子准入计数、固定配置和不参与逐任务学习的运行策略。

每个 cell 保存 c(ms/byte)、e(ms/byte)、真实样本数、最近有效观测时刻和冷却状态。沿用 `c←(1−α)c+α*S/n`，`e←(1−α)e+α*abs(S−S_pred)/n`，α=1/8。解释模型的 a/l/r 暂不在线拟合；若跨大小残差呈系统性趋势，先对更窄区间、非负仿射模型和争用分层分别消融；只有端到端收益超过其额外开销才修改模型结构。保留未知状态、有限数检查、饱和计数和不跨形状外推。

### 3.2 样本定义与测量开销

1. 对被采样任务记录预测快照，再执行，再用真实后端和实际字节训练，避免事后重算预测导致残差过小。
2. 只训练完整成功、未重试、文件验证通过且输入形状未变的样本；失败时间、重试字节、初始化和回退次数仍计入端到端统计，不能消失。
3. `service_ms` 保留文件操作墙钟。另记 `compute_wall_ms`、`read_wall_ms`，OpenCL 事件的 device queued/submit/start/end 只供抽样诊断；不同设备时钟不可直接相减。
4. 记录窗口起止的活跃计数及全局活动变化代数；起止计数相同也可能中途争用。只要窗口中代数变化或有其他工作，就标 mixed/changed，不伪装精确平均并发。
5. 小文件维持有界低频采样；大文件可逐任务采样；计时开销必须有“分析器开/关”配对对照。不要在每 1 KiB 叶压缩处读时钟。
6. 把文件元数据复用命中、内容缓存命中与真实计算区分。未执行哈希的缓存命中不训练设备成本；I/O 引擎、block、模型版本或设备驱动变化引发新配置代际。

### 3.3 准入、探索与漂移

初版 iGPU 全池最多 1 个在途文件；`try_acquire` 失败立即选可用基线，不让固定 CPU 线程阻塞等设备。这样不会额外引入 GPU 队列。该上限是待实验提升的资源配置，不是 iGPU 永远只准一个流的论断。CUDA 现有并发策略不随本方案自动收紧。

启动时先做正确性验证，再做软预算限制的性能探测（到期停止追加提交，已运行操作仍可能阻塞）；冷启动其他线程继续 CPU。现有 `calibrate_dispatch` 是候选复用点，不直接复用其 CUDA 错误文本、二后端结构或旧样本语义。每次探测检查截止时间只能限制追加工作，不能中止已阻塞驱动调用；需要硬超时保证时必须依赖独立进程隔离，不虚构线程可安全取消。

探索按**资源预算**而非只按任务数：保留每形状少量冷探索，并设全池同一时刻最多一次探索、单次最大输入、累计时间/字节预算。超预算不开新探索；已经开始的任务允许正确完成。可用 64 次机会一轮的原有周期作为初值，但不能由 N 个线程各自耗尽一份“全局预算”。机会使用固定种子打散而非永远每第64个文件，避免与排序周期混淆。无性能先验的超大文件不用于首次探索。

漂移（concept drift）初版：记录有符号残差；连续 3 个同向大残差，例如 `S−S_pred > max(3E,0.2*S_pred)`，将对应 cell 标 suspect 并暂停利用，仅允许预算内再校准。阈值是待敏感性验证的建议值；E 是尺度而非“3σ”。单一极端慢任务保留观测、标明 I/O/环境变化，不无限延长退休。长时间未观察的 cell 变 stale，不直接当已校准；建议 60 秒仅作实验初值。设备错误进入健康状态机而不是性能漂移逻辑。

### 3.4 可实现的路由伪代码

以下为接口语义，不是已存在 API；`lease` 生命周期覆盖完整尝试及清理。

```text
choose(task, worker, snapshot):
    # 保留显式模式契约。 / Preserve explicit backend contracts.
    baseline = existing_cpu_cuda_choice(task, worker)
    if not igpu_enabled_for_run or not eligible(task): return baseline
    # 未取得配额不等待。 / Never wait for admission capacity.
    lease = igpu_gate.try_acquire()
    if not lease: return baseline
    if not offline_validated_mixture(snapshot): return baseline
    pred = model.predict(igpu, task.bytes)
    if pred.trusted and conservative_win(pred, baseline.pred):
        return Selection(igpu, lease, reason=model)
    if exploration_budget.try_reserve(task, pred):
        return Selection(igpu, lease, reason=explore)
    return baseline  # 临时票据在此释放。 / Temporary lease releases here.

observe(completion):
    # 失败也进入成本统计，但不训练成功服务模型。
    # Account failed work without training successful-service estimates.
    account_end_to_end(completion)
    if completion.valid_single_attempt:
        owner_model.observe(completion.frozen_prediction, completion.sample)
```

读者应注意：snapshot 与准入之间存在竞争；准入票据才是容量权威，预测快照只是提示。选择 GPU 后开始计数、完成后减计数；异常也必须释放。不得在公共队列锁中调用驱动、等待设备、采样或训练。

## 4. 队列残余时间：何时需要，何时不需要

当前工作线程已经出队并持有任务，公共 Q 对三个候选相同且已经付出；**不再加入 `queue_length*mean_gpu_time`**。设备内部排队成本已包含在 S/H；再加一次会双计。只有将来真的引入独立 GPU 服务队列，才需最早完成时间（Earliest Finish Time, EFT）模型：

`F_d = now + W_d + S_d`。

串行设备可近似 `W_d = residual_running + Σ pending S_k`。运行剩余时间不能普遍用 `max(0,predicted−elapsed)`；重尾服务的正确统计对象是 `E[S−e | S>e, shape,z]`，当实测超预期而预测归零会错误吸入更多任务。可采用有上限的经验残余分布，但这又引入样本和队列所有权成本。**本阶段没有独立队列，故不实现该模型**；未来异步化也应先证明等待估计能改善端到端结果，再增加这一层。

## 5. 健壮性与兼容边界

- `cpu` 模式不加载 GPU 驱动；`cuda` 不暗中改成 iGPU；新增显式 `igpu` 与 feature gate 后再扩展 auto。旧 `--no-pgo` 不突然启用探索，旧遥测键含义不重写；新 backend 维度使用版本化扩展。
- 资源总账将 iGPU buffer 算入主机预算，不把共享显存再当免费容量。异步 I/O 注册缓冲、GPU 可映射缓冲与普通读缓冲可共享实体，但生命周期和所有者必须唯一；所有还在使用的页不可重用。
- 初始化失败可禁用该设备；可恢复计算错误最多完整 CPU 重试一次；重试须重新打开并重建摘要，不能把 CPU 状态接到未知 GPU 状态上。摘要不匹配是完整性错误，不能当性能回退静默接受。设备挂起、异步失败后的资源回收细节须由后端健壮性方案定义。
- 故障、漂移、性能不佳是不同状态；错误不能通过“更多探索”治愈。工作线程不退休，其他设备不因单个上下文错误被全局 reset。
- 模型初版只在单次运行内使用。跨运行保存最多作分析记录；若未来加载先验，必须绑定 device/driver/kernel/build/block/memory mode/policy schema，并使用短验证窗口，历史记录不得覆盖当前健康状态。

## 6. 判别性实验与验收

| 问题/竞争解释 | 实验与控制 | 推翻采用建议的结果 |
|---|---|---|
| Python/提交粒度使核显看似更快 | 同一原生框架；官方 C SIMD；CPU/CUDA/iGPU 等摘要、等输入、等分块或公开调优；同时报告相同粒度和各自最佳合法粒度 | 优化基线消除核显优势 |
| 共享缓存虚增吞吐 | 独立只读随机输入；总工作集跨 LLC；共享输入仅作对照；完整摘要验证 | 优势只存在重复小驻留数据 |
| iGPU 拖慢 CPU | CPU工作数 1/2/4/8/配置上限 × iGPU并发0/1/2 × CUDA关闭/现有策略；总线程和内存预算不增，记录电源、温度/频率（可得时） | 个体变快而 M 变慢；不启用该并发组合 |
| 磁盘才是瓶颈 | 内存、热缓存真实文件、冷缓存真实文件分组；固定数据集/文件顺序种子；同步与新I/O引擎分开 | 纯计算优势不能传递到扫描 |
| 模型只追随选择偏差 | 留出固定诊断任务和预算内随机机会，保存选择概率/原因；离线按真实执行样本回放，不臆造未选后端结果 | 模型胜利只来自只挑容易输入或缓存热点 |
| 偶然频率/热状态 | 配对随机顺序，至少10个独立扫描对作为初始计划；固定预热与试验持续时间；报告每对比值及不确定性 | 配对区间跨无收益，或长时反转 |
| 分析器本身太贵 | 强基线下分析器开/关、探索开/关；候选争用分层仅在单独消融（ablation）中评估 | 增量测量/探索开销高于净收益 |

主指标为全扫描完成时间、处理字节/墙钟时间、尾延迟与峰值主机内存；补充 initialization、fallback wasted time、read/hash time、coverage、绝对误差、signed residual、underprediction rate。p95 在样本过少时不作为稳定结论。功耗无可靠传感器时明确缺失，不凭 GPU utilization 推断能耗。

建议部署门槛（待产品接受，不是已通过）：主目标工作集完成时间中位数改善至少 10%，且配对重采样（resampling）所得 95% 时间比区间上界 <1；预注册保护工作集中位数退化不超过 max(3%, 事先声明并实测的噪声容差)，实质性尾延迟退化须失败或专项审查；摘要/一致性/预算/故障测试必须全部通过。重复次数按先导方差追加，不能“反复测到显著为止”；保存原始慢样本和失败。门槛不满足则保持显式实验模式，不宣布 auto 已完成。

## 7. 分阶段变更与测试映射

1. **观测与类型阶段**：内部 BackendId/BackendSlot、版本化遥测、时间窗口；iGPU 默认不可选。旧 CPU/CUDA 路由回放等价，`--no-pgo/--no-telemetry` 组合测试。相关文件 `resources.hpp/.cpp`、`online_model.hpp/.cpp`、`application.cpp`、遥测模块。
2. **后端阶段**：完整任意长度与增量契约，显式 iGPU；官方测试向量、0/1/63/64/65/1023/1024/1025/2047/2048/2049、跨块更新、重复 finish、空 update、大文件、非对齐地址、尾部。原型二次幂向量不足以通过此门槛。
3. **分析器阶段**：注入时钟/设备/准入器，用模拟样本测未知/非有限/零大小/溢出/漂移/过期/争用变化；执行失败不训练但计费；同一任务完成只发布一次；所有异常释放票据；固定工作线程数和主机预算。
4. **实验自动路由阶段**：仅在已证实组合中开启 iGPU；完成上表组合试验及消融。模型留在线程本地，统一队列不改，不新增通用调度框架依赖。
5. **后续可选项**：映射缓冲、双缓冲流水线、有限更多并发、更窄区间/仿射模型/争用模型键、独立设备服务队列；每项独立 benchmark/回滚 gate，不与首版一次合并。

## 8. 外部实践、学术方向与证据边界

| 来源 | 可借鉴结论 | 强度与不可迁移部分 |
|---|---|---|
| [Intel kernel launch guide](https://www.intel.com/content/www/us/en/docs/oneapi/optimization-guide-gpu/2024-1/kernel-launch.html) | 内核启动成本需要与实际工作量一起分析 | 厂商一手工程文档；不能提供本机BLAKE3交叉点 |
| [Intel GPU Roofline](https://www.intel.com/content/www/us/en/docs/oneapi/optimization-guide-gpu/2023-0/optimize-memory-bound-applications-with-gpu.html) | 集成GPU共享DRAM；考虑内存通路限制 | 一手架构实践；不证明BLAKE3必然带宽受限 |
| [StarPU features](https://starpu.gitlabpages.inria.fr/features.html) | 性能模型与数据位置、传输、资源一起管理 | 成熟开源运行时；本项目无复杂任务图，不建议引入整个框架 |
| [Nozal & Bosque, Euro-Par 2021](https://doi.org/10.1007/978-3-030-85665-6_31)，[作者公开稿](https://arxiv.org/abs/2106.01726) | CPU+iGPU共同执行应比较动态负载分配与内存方式 | 同行评审会议，作者稿注明出版信息；其HPC任务/硬件结果不是BLAKE3收益证据 |
| [StarPU DARTS 2026 tutorial](https://starpu.gitlabpages.inria.fr/tutorials/2026-06-DARTS-OOC/darts.html) | 新近方向将数据移动量和容量约束纳入调度，给出固定提交复现实验 | 项目一手可复现教程，非独立性能证明；核外LU共享数据图不等于独立文件哈希 |

学术前沿启发是“数据移动与资源状态也是调度输入”，不是立即采用强化学习（Reinforcement Learning）、高维上下文多臂老虎机（Contextual Bandit）或复杂任务图调度。本项目三个候选、有限硬件、独立文件和强回退约束下，低维可解释模型更容易证明安全与测量收益；若简单策略持续无法区分争用且有充分训练数据，再重新评估复杂方法。
