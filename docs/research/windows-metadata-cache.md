# Windows 元数据访问与跨运行缓存研究方案

日期：2026-09-10。性质：设计与外部证据，不是已实现功能或性能承诺。范围仅涉及元数据、路径清单与 I/O Ring 边界；不修改生产代码。

## 1. 决策摘要

**I/O Ring 是内容读取后端候选，不是 Windows 上通用的异步元数据系统调用入口。** 应把优化拆成三个独立实验：批量目录信息、跨运行清单缓存、异步内容读取。不能把 `READ` 支持解释为支持路径打开、`stat`、目录枚举或 USN 控制请求。

跨运行缓存应采用“可丢弃的路径清单 + 明确的新鲜度状态 + 增量失效 + 必要时重建”；正确性敏感的摘要复用和重复文件判定仍保留现有新鲜句柄检查与精确字节比较。第一阶段缓存只作建议与失效提示，保留新鲜枚举和 stamp；减少枚举的独立增量模式须满足第 10 节的更严格门槛。

## 2. 当前代码证据

| 已检查位置 | 观察 | 设计约束 |
|---|---|---|
| `src/walk.cpp`，`Cursor` | 已使用 `FindFirstFileExW(FindExInfoBasic, FIND_FIRST_EX_LARGE_FETCH)`，复用类型/reparse 属性 | 不能再把“避免逐项 GetFileAttributes”当新增收益 |
| `ParallelWalk::Impl::metadata` | 每文件打开 `FileReader`，获得 stamp，移交未读句柄；队列有界 | 优先保持所有权移交与背压，不增加无限句柄池 |
| `src/files.cpp` | CreateFileW + 类型检查，Basic/Standard/Id 三类查询，ReadFile 同步读取 | 文件打开、元数据查询、内容读取分别计时；I/O Ring 只能直接替换其中内容读取 |
| `src/application.cpp::unchanged/hash_file/compare` | 同时检查句柄和路径文件戳；哈希前后、比较前后验证 | 删除路径探测会漏掉路径替换；现有检查亦不是原子快照 |
| `src/store.cpp` | schema v1，path 主键；size/identity/modified/changed 匹配复用；generation 清除未见项；单连接、FULL、DELETE、整轮事务 | 增量扫描不能直接复用“没访问就删除”的 end_scan；不改变旧 digest 编码 |
| `docs/small-file-io.md`、`traversal-optimization.md`、`store-optimization.md` | 已记录句柄移交、目录缓存不作版本、预编译 SQL 复用 | 本方案不重复声称这些改进尚未实施 |

仓库递归未发现 AGENTS.md。没有在本研究运行本机卷权限/USN 探针，历史消息的 I/O Ring 探测不构成本轮独立验证。

## 3. 能力矩阵：API 能做什么

| 操作 | 公共 API / 机制 | I/O Ring 直接支持？ | 推荐路径 |
|---|---|---|---|
| 文件内容读取 | `BuildIoRingReadFile` | 有 READ；每环检查 `IsIoRingOpSupported` | 动态加载、按操作探测，保留同步后端 |
| 写入/刷新/散聚读写 | 公开 `IORING_OP_CODE` 列出 WRITE/FLUSH/READ_SCATTER/WRITE_GATHER 等 | 枚举存在不代表本机/所建版本支持 | 本只读扫描器不需要启用 |
| 句柄/缓冲注册、取消 | REGISTER_FILES/REGISTER_BUFFERS/CANCEL | 依具体操作与版本 | 注册是已有句柄注册，不是按路径打开 |
| 路径打开、查询信息 | `CreateFileW`、`GetFileInformationByHandleEx` | 当前公开 opcode 没有 OPEN/QUERY_INFORMATION | 有界同步元数据工作池 |
| 目录批量枚举 | `GetFileInformationByHandleEx(FileIdExtdDirectoryInfo)` | 无目录枚举 opcode | 原生目录句柄 + 可选批量缓冲；不支持则现有 Cursor |
| USN 查询/读取 | `DeviceIoControl(FSCTL_QUERY_USN_JOURNAL/FSCTL_READ_USN_JOURNAL)` | 无通用 DeviceIoControl opcode | 独立控制通路，按 API 契约使用 overlapped 或同步工作线程 |
| 在线目录变动通知 | `ReadDirectoryChangesW`，重叠 I/O（Overlapped I/O）与完成端口（IOCP） | 不是 I/O Ring 元数据查询 | 仅进程存活期间提示；溢出必须补扫，不能覆盖离线期间 |

依据：[Microsoft IORING_OP_CODE](https://learn.microsoft.com/en-us/windows/win32/api/ntioring_x/ne-ntioring_x-ioring_op_code)、[BuildIoRingReadFile](https://learn.microsoft.com/en-us/windows/win32/api/ioringapi/nf-ioringapi-buildioringreadfile)、[GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex)、[FSCTL_READ_USN_JOURNAL](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ni-winioctl-fsctl_read_usn_journal)、[ReadDirectoryChangesW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)。这是一张公开 API 能力表，不是尚未核对的私有 NT 接口承诺。

`FILE_ID_EXTD_DIR_INFO` 批量返回 FileId、ChangeTime、LastWriteTime、EndOfFile、属性、reparse tag 和名称，适合初步分组和清单构建。应检查 NextEntryOffset、长度与 UTF-16 边界；以目录句柄获取卷身份；不支持信息类时回退既有枚举。**这些值反映枚举观察，不能证明稍后同一路径仍指向该对象，也不能代替打开后的可读性检查。** [结构定义](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_extd_dir_info)

## 4. 缓存数据模型与不变量

建议新增独立 `inventory.db`，而不是破坏旧 state.db v1。清单是可重建旁路数据；hash store 仍是既有输出契约。避免两个数据库事务被误称为原子提交：hash 不依赖 inventory freshness 作安全判定，两个库的代际不一致只令清单失效。

| 表/状态 | 建议内容与不变量 |
|---|---|
| volume_epoch | 卷 GUID 路径、序列号、文件系统种类、卷根 identity、schema/parser 版本、journal ID；盘符不是主键，序列号也不单独证明卷实例 |
| objects | `(volume_epoch, file_id)`、类型、观察 stamp、dirty、deleted/tombstone；文件 ID 不是永不复用的全局 UUID |
| links | `(parent_id, exact_name)` → child_id；一对象多路径，父子图不强制一对一 |
| root_view | root identity、规则内容摘要、递归模式、路径/大小写/链接策略版本、可访问性上下文、完成代际 |
| checkpoint | journal_id、next_usn、generation、state、未解决 dirty 队列；游标只能随已持久化事件作用或 dirty 标记一起提交 |

保留原始名称，不无条件 lowercase：Windows 可出现区分大小写目录；格式转换不得把旧缓存路径键语义改变。FileId 删除后稳定性不再有保证，删除/新建、卷还原、epoch 重置时失效相关摘要关联；不跨卷共享身份。[Microsoft 文件 ID 规范](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/2d3333fe-fc98-4a6f-98a2-4bb805aff407)

清单状态建议为 `Absent → Building → Replaying → Reconciling → Usable`；任意能力缺失、日志缺口、解析错误进入 `NeedsRebuild`，资源暂时不可用进入 `Unavailable`。`Usable` 只表示定义范围内清单已协调，不等于每个文件内容快照。

## 5. NTFS USN 增量协议

USN 变更日志（Update Sequence Number change journal）是失效源，不是包含全部对象状态的数据库重做日志。记录会累积原因、受句柄生命周期影响；不能假设每次写入一个事件，也不能只读 CLOSE 事件就称完整。[Change Journal Records](https://learn.microsoft.com/en-us/windows/win32/fileio/change-journal-records)

### 5.1 能力与基线

1. 对真实根解析卷和 root ID，拒绝从不可信路径缓存决定卷。探测文件系统、卷句柄访问、query/read 能力；不自动申请管理员、不创建/删除/扩容用户日志。
2. 读取 journal ID、FirstUsn、LowestValidUsn、NextUsn，记基线起点 U0。建立 staging generation，按现有忽略和非 reparse 规则完整遍历；受限子树必须显式标未完成而不是当空目录。
3. 重读边界 U1，消费 U0 至 U1 的变更，将受影响对象及目录标 dirty，重新观察当前状态；枚举期间发生的目录移入需完整发现其子树，不能只更新目录行。
4. 重放协调期间的新事件。只有没有未解释缺口、dirty 均处理且约定稳定性检查满足时发布 Usable；长期持续变动给出有界重试/未稳定状态，不无限等待，不伪造“某一时刻完整快照”。对要求全局时间点一致性的新模式另评估 VSS，不能从 USN 协议推导出来。

### 5.2 每轮增量与事务

- 开始前核对卷/root/策略 epoch 与 journal ID；`next_usn < max(FirstUsn, LowestValidUsn)`、`next_usn > NextUsn`、journal 变更/删除、版本不认识、读返回 gap 均重建。首次启用只接受明确验证的 NTFS 组合；不要由 API 对某些 ReFS 的支持推导全部语义相同。
- 读取批次，校验 RecordLength、MajorVersion、文件名边界、USN 前进性，支持的 V2/V3 分别解码文件 ID 宽度；未知 V4 或未来记录不能当 V2 强转或静默跳过。可以明确要求可支持的版本范围，不能因此滤掉有语义影响的变更。
- 同一个 SQLite 事务内：事件转 dirty/链接变更 + 保存未解决集合 + 更新 next_usn。崩溃在提交前重放，提交后继续 dirty 协调；幂等（Idempotence）依事件序号与状态更新，不依“恰好一次”假设。
- 只有新的完整 generation 发布时清除 staging；未完成清单不能触发 state.db `end_scan` 的删除行为。实际运行仍枚举/验证所用路径并 mark seen，或以后引入单独完整集合提交 API，禁止仅将发生变动的文件喂给旧扫描协议。
- 发布前再核对 journal 连续性；若扫描太慢导致 U0 已截断，丢弃 staging 重建。不能推进到“查询得到的 NextUsn”而没有消费到它。

微软明确要求使用 journal ID 与 USN 配对检测实例变化；其管理员权限说明意味着默认普通用户不能假设该功能可用，必须以真实调用探测为准。[Using the Change Journal Identifier](https://learn.microsoft.com/en-us/windows/win32/fileio/using-the-change-journal-identifier)、[USN_JOURNAL_DATA_V0](https://learn.microsoft.com/en-us/windows/win32/api/winioctl/ns-winioctl-usn_journal_data_v0)

### 5.3 重命名、硬链接、规则变化

| 情况 | 必须采取的动作 |
|---|---|
| 目录改名/换父目录 | 更新父子边，失效派生路径；重新计算整个后代的 root 归属和规则。不能仅修改目录自己的完整路径；不假设每个后代都有事件 |
| 外部目录移入已缓存根 | 旧缓存没有其后代；扫描整子树并协调新事件。移出则所有后代离开当前 view，但不意味着对象被删除 |
| OLD_NAME/NEW_NAME 非邻接或缺失 | 不能仅配相邻两条记录；标记相关目录 dirty，重新枚举；无法定位则根重扫 |
| 硬链接（Hard link）增加/删除 | 文件 ID 对应多个 links；HARD_LINK_CHANGE 不代表事件列出全部名称。对已知父目录补扫，必要时使用 FindFirstFileNameW/FindNextFileNameW 枚举链接并按 root 过滤；不支持或无法定位回退根扫描 |
| 内容改变 | 全部同 identity 的路径记录失效，不能只使日志携带的那一个名字失效 |
| 忽略规则变化 | 对实际规则内容及解释版本算摘要；以前剪枝的目录可能未入库，放宽规则必须发现这些子树。最初实现直接全量重建，避免假装有完整全卷索引 |
| reparse/挂载点变化 | 维持既有不跟随策略，失效相关子树；不能因为缓存说普通目录就跳过当前检查 |
| 权限、安全描述符变化 | 不从高权限旧清单泄露条目或跳过今天的访问失败；保留当前错误传播政策 |

理由标志见 [READ_USN_JOURNAL_DATA_V0](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-read_usn_journal_data_v0)，链接枚举见 [FindFirstFileNameW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-findfirstfilenamew)。

## 6. 回退、恢复与信任边界

- **非 NTFS、SMB、无权限、journal 不存在：** 使用现有并行遍历和新鲜 stamp；缓存只提供调度提示/历史统计。ReadDirectoryChangesW 可优化在线刷新，但停止运行时无通知，溢出后补扫；目录 mtime 不足以证明后代未变。
- **缓存损坏或未来 schema：** 不以损坏条目输出结果；旁路 inventory，记录原因并正常扫描。未来版本默认不修改；重建应写新文件并明确保留诊断副本，不未经授权删除用户文件。SQLite 完整性检查不能证明缓存对应真实文件系统。
- **并发运行：** 尊重项目 run lock；inventory 写入同样单所有者。旧进程/另一个根若共享库，使用数据库锁和 root/volume 分区，不能两个工作线程共享 NOMUTEX 连接。无需为本地小缓存引入 Kafka/WAL 服务。
- **取消/磁盘满/进程崩溃：** 原子发布前上一代仍有效但需下次 freshness 验证；失败不推进无 dirty 记录的游标。持久化容量有预算，checkpoint 与 dirty 集不允许被淘汰为“干净”。
- **恶意或持续并发变动：** 现有句柄/路径 stamp 不能保证任意敌手模型，不用 size+mtime、甚至单纯 FileId+ChangeTime 声称数学内容证明。严格一致性需额外快照或禁止写入的契约，作为单独功能。

信任分层：`历史观察` → `已协调清单` → `当前打开对象 stamp` → `本次完整摘要` → `当前精确比较`。上层不可仅由下层的名字推导。缓存旧摘要若使不同内容文件落入不同桶，也可能导致漏报，不能说“最终比较会兜底所有旧摘要问题”。因此首阶段**不降低现有 fresh-stamp 缓存命中条件**，且保留枚举；journal 先作失效提示。减少发现工作是后续增量模式待验证的目标。

## 7. 模型、实验与否证条件

记 N 为当前文件数，D 为目录数，J 为本卷两次运行间 journal 字节量，K 为需协调的对象/子树工作量，V 为本轮安全验证代价：

- 全量：T_full ≈ T_enum(D,N) + T_validate(N) + T_db(N) + T_hash(misses)。
- 增量：T_inc ≈ T_journal(J) + T_reconcile(K) + T_inventory + V + T_hash(misses)。

J 是**全卷**变更，不是仅项目内变更；K 可因目录移动放大到 N。首阶段 V 仍约 O(N)，因此不承诺“未改动扫描 O(changes)”；纯清单查询可以近似变化量工作，但重复文件结果生成仍需处理/输出其集合。若验证主导而枚举很便宜，缓存收益会很小。这是需要接受的负结果，而不是偷偷取消安全检查。

对照组：A 当前遍历；B 批量目录信息但仍 fresh stamp；C USN 清单 + fresh stamp；D 仅研究用清单查询（不得当重复判定性能）。内容读取的同步/IOCP/I/O Ring 实验必须另设组，避免把缓存收益归给 ring。

固定语料和配置、交替顺序至少 5 次，记录中位/P95/范围；变化率 0/0.1/1/10/100%，小文件多目录、单宽目录、深树、外部卷高变更；冷暖条件明确，不能把新建语料叫物理冷缓存。指标：端到端时间、枚举/打开/查询次数、J/K、失效重建率、DB 写字节、峰值句柄/RAM、CPU、读取吞吐、输出一致性。收益阈值在实验前登记，以置信区间和绝对秒数而非利用率判断。

正确性矩阵：跨运行同大小覆盖并还原 mtime、目录重命名/移动进出根、跨目录硬链接、忽略规则放宽、大小写名称、reparse 置换、访问权限变更、根替换、journal 截断/重建、未知记录版本、每个事务边界强杀、磁盘满、损坏数据库、两进程竞争。每项以独立全量扫描 oracle 比较路径集合、摘要和重复分组；未知/未稳定必须显式失败或回退，不能输出成功的部分清单。

否证：C 没有减少端到端时间，或额外维护/重建成本超过节省，默认不开启；任一 stale hit/漏路径/漏报即阻止发布。性能实验不是并发变动正确性证明。

## 8. 工程经验与研究启示

微软提供的日志连续性/溢出恢复协议是生产设计依据；本项目采用可重建派生索引、持久游标与 dirty 同事务、周期/异常协调，而不是“事件流永远完整”。SQLite 已有单连接事务模型足够，不因架构图漂亮引入分布式消息系统。

同行评议 [A Five-Year Study of File-System Metadata, FAST 2007](https://www.usenix.org/conference/fast-07/five-year-study-file-system-metadata) 基于大量 Windows 文件系统，提醒文件数、目录形状、变更分布都应进入工作负载设计；其年代与企业样本不能预测本机 SSD 的阈值。

前沿 [Icicle, 2026 预印本](https://arxiv.org/abs/2604.10295) 将批量快照摄入和持续事件摄入结合，为清单的重建+增量路线提供机制启发，但面向 HPC，且此处未独立复现实验；其规模/加速数据不能移植到 NTFS。采用它的“混合协调”思想，不采用 Kafka/Flink 重型栈，也不把索引新鲜度当内容一致性。

## 9. 分阶段落地与验收

1. **能力与可观测性：** 独立报告 ring op、卷/USN 支持及权限错误；拆分 enumeration/open/query/read/DB 时间；不修改用户卷配置。
2. **批量目录查询原型：** 与已有 FindFirstFileEx 对照，保留相同安全检查；字段解析、特殊文件、回退、资源上界测试通过才进入生产评估。
3. **隔离清单库：** schema 和状态机、基线+USN重放、dirty事务、图重命名、崩溃测试；不削弱 hash store contract。先可选开启。
4. **真实卷评估：** 普通权限及有权限情形、变化率和重建成本；仅在正确性矩阵全通过且端到端有收益时默认启用适用能力。
5. **未来更激进缓存：** 如要跳过 N 次 fresh stamp，必须单独定义并审核新的威胁模型、持续变动/漏报边界、快照或可靠版本协议；不能包装成无语义改变优化。

已由 [本地独立验证](local-evidence.md) 确认 D: NTFS、32 MiB 日志配置及当前非特权无名称读取样本。尚未验证：卷变化率、批量目录查询实际支持、USN 事件完整覆盖、性能收益与崩溃协议实现。本文提供可实现和可否证的方案，不声明这些门槛已通过。

## 10. 集成审核补充：默认安全边界与非特权 USN

本节收紧前述阶段定义，若与“减少枚举”的建议发生歧义，以本节为准。**默认生产模式保留新鲜全量枚举与新鲜 stamp；旁路缓存先用于统计、排序、预取和失效提示。** 跳过目录枚举的增量清单模式仅作为独立、显式开启的实验，需证明不会漏掉新路径并约定静止文件树（Quiescent tree）或快照边界后，才能升级。仅有 journal 连续性不能作为跳过所有 stat 或枚举的许可。

### 长期未关闭写句柄的盲区

USN 原因可合并；一次 overwrite 记下之后，同一打开生命周期后续同类写入不必对应新的独立记录。`ReturnOnlyOnClose=true` 更直接将可见性推迟到 close；设为 false 也不将事件流变为每次写入的实时完整审计。先读到水位，再读文件，再以“水位没变”宣布没写入，是错误推论；文件时间戳也可能延迟更新。维持“最近没有 CLOSE 的对象 dirty”能保守降低风险，但无法单独证明跨 checkpoint、首次建立缓存、长期 writer 的所有情况均已覆盖。

所以 USN 首先承担**只增不减的失效提示**：收到事件就可怀疑，未收到不能独立确认内容不变。陈旧摘要会把本来相同的两个文件分配到不同摘要桶，形成假阴性（False negative），最后对正候选逐字节比较救不了它。未来跳过 fresh stamp 的模式必须明确快照/静止写入契约并独立审查，不能借“最后有字节比较”弱化安全门槛。[Microsoft Change Journal Records](https://learn.microsoft.com/en-us/windows/win32/fileio/change-journal-records)

### 非特权通路：探测动作而不是推断权限

根代理转交的本轮 validator 观察：D: 为 NTFS；中等完整性、Administrators deny-only token；`\\.\D:` 的 GENERIC_READ 打开失败 5，而 D:\ 目录句柄 QueryUsnJournal 成功；常规 READ_USN_JOURNAL 失败 5，`FSCTL_READ_UNPRIVILEGED_USN_JOURNAL` 有界读取成功 65,480 字节。**初步结果现已由 [本地独立验证](local-evidence.md) 的结构解析补全；本文件作者已阅读该权威记录，没有重新运行探针。** 具体名称统计见下一段。

Microsoft 的公开 Windows binding 定义了此控制码：[FSCTL_READ_UNPRIVILEGED_USN_JOURNAL](https://microsoft.github.io/windows-docs-rs/doc/windows/Win32/System/Ioctl/constant.FSCTL_READ_UNPRIVILEGED_USN_JOURNAL.html)。本轮未找到充分的第一方文字契约保证非特权结果总含完整文件名/父链。现已阅读 [独立验证记录](local-evidence.md)：两种根目录访问模式各有界读取成功 65,480 字节，各解析 1,023 条 V2 记录，FileNameLength 非零计数均为 0；常规 READ_USN_JOURNAL 返回访问拒绝。这是本机、本次样本的实际观察，不是所有系统必然省略名称的通用保证；未验证理由位、父目录、硬链接与重命名覆盖。不能将空名字当删除，也不能宣称已获得完整命名空间。

若仅有 ID/原因：可以使已知 object 与其所有已知 links dirty；对未知 ID、目录移动、父 ID 缺失不能构造新路径，必须补扫适当目录或根。能力档位应明确分为 `query-only`、`id-invalidation`、`named-replay`，且任何档位均非内容快照。ACL（Access Control List）与路径可访问性必须按当前用户的真实路径访问验证；OpenFileById 不能绕过祖先路径权限或恢复不可见名称到用户输出。

路径别名（Path alias）如盘符、卷 GUID、短名称或多个输入根，不应导致同一 root 不受控双写；root_view 还应含当前会话/权限上下文和 .same 身份排除。当前 `include/same/config.hpp::Ignore::can_prune` 契约明确采用 Git 式排除父目录剪枝，子否定不重新纳入；以此和 `walk.cpp` 实现为准，不引用历史设计中相反语义。策略摘要包括规则内容、解释器语义版本、recursive/shallow、.same 排除和 root identity。

旁路库可使用持久 `building/dirty` token 标识未完整结束的运行；崩溃后不把其状态升级为 clean，先核对再重建/协调。token 只能减少错误信任，不证明文件树没变；跨两个数据库的 checkpoint 和摘要发布依然不是单事务。

### 内容读取后端接入的独立约束

当前 metadata 移交的 FileReader 在 CreateFileW 时没有 FILE_FLAG_OVERLAPPED。若采用 IOCP，必须在最初打开时按后端选择 flags，或者重新打开并重新验证预期 stamp；后一条路线不能声称零重开。I/O Ring 是否接受当前普通文件句柄要单独用公开契约与实机测试核实，不能将 IOCP 标志要求机械套给 ring。

建议每文件最多两个有界读取槽（Read slot）开始实验，总量还受全局字节/句柄预算限制。请求有显式 offset，完成可能乱序，增量 BLAKE3 update 必须按文件 offset 提交；若采用树分块并行，需要独立通过摘要根归并正确性验证。槽内存租约要等 I/O 完成队列（Completion queue）确认及 CPU/iGPU/dGPU 对该输入消费完成后才释放或复用；取消请求并不等于完成已排空。每个父文件作业恰好一次发布最终结果，失败回退丢弃部分摘要并从验证后的起点重读。目录批量信息仍是同步元数据池工作，这些 ring 管理规则不会赋予其异步元数据能力。
