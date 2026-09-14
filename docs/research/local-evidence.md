# 本地独立验证 / Independent local validation

日期：2026-09-10。范围：仅既有实验、原始记录及本机只读能力；没有生产修改、持续性能重跑或卷日志修改。

## 结论 / Verdict

- 原始实验可作为**受限输入上的核显可行性证据**提交；不能用作三设备速度排名、CPU+iGPU 协同收益或一般 BLAKE3 正确性证明。
- 本机 I/O Ring 的 READ 能力独立复核成功；本地 SDK 没有元数据查询/目录枚举/open/DeviceIoControl 的 I/O Ring builder。
- 当前非提升进程能经根目录句柄读取**非特权 USN 日志**，但本次 1023 条记录全部没有文件名。适合作为按文件标识失效的候选证据，不能直接承担路径索引更新。

## 可复现命令 / Commands

在仓库根目录执行。新检出可先用 Python 3.12 建立独立环境（需另行安装 Intel GPU OpenCL 驱动；统计和 USN 脚本仅依赖标准库）：

```powershell
py -3.12 -m venv build/igpu-py
& build/igpu-py/Scripts/python.exe -m pip install numpy==2.2.6 pyopencl==2026.1.4 blake3==1.0.8
```

本机隔离环境已存在；默认 python 是另一版本 3.14，不应混用。native compute 命令要求先按 docs/build.md 构建 CUDA 测试目标。

```powershell
& build/igpu-py/Scripts/python.exe tools/research_validation/verify_igpu.py
& build/igpu-py/Scripts/python.exe tools/research_validation/check_raw.py
& build/igpu-py/Scripts/python.exe tools/windows_ioring_probe.py
ctest --test-dir build/cuda -R '^compute$' --output-on-failure
Get-Volume | Select-Object DriveLetter,FileSystem,HealthStatus,Size,SizeRemaining
whoami /priv
whoami /groups
fsutil usn queryjournal D:
& build/igpu-py/Scripts/python.exe tools/research_validation/usn_access.py --root D:\
& build/igpu-py/Scripts/python.exe -O tools/research_validation/usn_access.py --self-test
Get-Content 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\ioringapi.h'
Select-String -Path 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\ntioring_x.h' -Pattern 'IORING_OP'
Select-String -Path 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\winioctl.h' -Pattern 'FSCTL_(READ_UNPRIVILEGED_USN_JOURNAL|READ_USN_JOURNAL|QUERY_USN_JOURNAL)'
```

验证脚本位于 `tools/research_validation/`；非敏感复核摘要位于 `benchmarks/igpu-20260910/validation/`。脚本使用 main guard、显式异常检查；`-O` 下仍验证摘要、统计与记录边界。USN 脚本只接受显式盘符根目录（示例 D:\ 应替换为待测卷），查询后仅一次 64 KiB 非特权读取，不调整权限、不保存名称。初次探索的原始卷/常规 READ 结果是历史手动探测，精简的持久脚本不重复那些操作。
没有发现仓库下的 AGENTS.md。

## BLAKE3 正确性与统计 / Correctness and statistics

独立构建既有 OpenCL kernel，在 Intel Iris Xe 上比较官方 Python BLAKE3：
输入 2048、4096、8192、65536、1048576、16777216、67108864 字节；每种使用全零、全 FF、新随机种子 43，共 **21/21** 摘要一致。
`compute` 现有构建测试 **1/1 passed（持久记录为 0.21 s）**。本次未重建，故只支持已有构建的测试结果。

原始记录复算：

| 文件 | 行数 | 分组 | 校验 |
|---|---:|---|---|
| opencl.jsonl | 93 | 3 环境记录 + 18 组 × 5 轮 | verified 全 true |
| dispatch.jsonl | 50 | 10 组 × 5 轮 | verified_digests = repeats × workers |
| dispatch-bulk.jsonl | 30 | 6 组 × 5 轮 | 同上 |
| cpu8.jsonl | 5 | 1 组 × 5 轮 | 同上 |

`docs/igpu-blake3-experiment.md` 所有报告中位数与当前原始文件吻合。例：64 MiB 核显上传 7916.71 MiB/s，驻留 20264.43；CPU Python 1984.54；原生 bulk CPU 3938.34、CUDA 9943.10；8 线程 CPU 19256.07。

### 限制 / Limitations

1. 原型只处理 >=2 KiB 的二次幂输入、完整 1024 字节叶、非密钥 32 字节摘要；没有空输入、短叶、不平衡树、增量接口、密钥或扩展输出测试。
2. kernel 的 global id/counter 与乘法使用 32 位整数；不能从 64 MiB 测试外推到任意大单次输入（例如 `id*256` 地址索引还会先于 64 位计数器语义到达边界）。
3. 只测一块 Intel 设备、一个驱动、单进程有序队列；无错误注入、驱动挂起、取消、超时、设备恢复、资源预算验证。
4. 每次时延包含 Python 调度、profiling 读取和摘要比较，但不含初始化、分配、文件读取。驻留模式复用已上传数据，不等于零拷贝（zero-copy）。
5. 每尺寸仅一个随机输入、5 轮固定交替顺序；无功率/温度/频率控制、独立冷缓存输入、置信区间（confidence interval）、系统干扰记录。64 MiB CPU Python 范围 1955.09–3948.92 MiB/s，驻留核显范围 17303.58–27617.68，波动不能忽略。
6. native 与 Python 不是共同随机化框架；多线程复用同一只读输入。不得据此推断磁盘吞吐或 CPU+iGPU 净收益。
7. 主实验脚本直接加载 Windows I/O Ring API 并选择 Intel GPU；能力缺失会直接失败。这对隔离实验可接受，不是生产回退（fallback）设计。

## I/O Ring 本机与 SDK / Runtime and SDK

独立重跑输出：

```json
{"max_version":400,"max_submission":65536,"max_completion":131072,"flags":2,"create_hresult":0,"read_supported":true}
```

探针创建并关闭空环，**未提交真实 READ**。版本 400 是枚举编码，报告称 version 4 是解释值。
`ioringapi.h` ABI 与探针结构一致；`ntioring_x.h` 的 READ 是枚举值 1。
本地 SDK `10.0.26100.0` 的 builder 包括 read/write/flush/scatter/gather/cancel/register files/register buffers；无 open/stat/query-directory/DeviceIoControl。这个结论范围是**已检查的公开本地 SDK**，不是对未来 API 的断言。
因此 I/O Ring 优化文件内容读取；路径发现和元数据减少应单独设计，不能把 READ 支持冒充元数据操作支持。

## NTFS、USN 与权限 / Filesystem, journal and access

当前 C:、D: 均为 NTFS、Healthy；工作区在 D:。
进程是 Medium Mandatory Level，管理员组为 deny-only；权限清单没有启用备份/管理卷权限。

`fsutil usn queryjournal D:` 当次输出：journal id `0x01d92a6b26dcd1e8`，FirstUsn `0x22c800000`，NextUsn `0x22eb51cc0`，Maximum Size 32 MiB，Allocation Delta 8 MiB，支持记录版本 2–4，write range tracking disabled。
USN 游标是动态状态，此数值不能作为持久配置。

只读 ctypes 探测采用 SDK 控制码及 READ_USN_JOURNAL_DATA_V0 ABI：

| 打开方式 | QUERY | READ_USN_JOURNAL | READ_UNPRIVILEGED_USN_JOURNAL |
|---|---|---|---|
| 原始卷 `\\.\D:`，access=0 | ERROR_INVALID_FUNCTION (1) | 未尝试 | 未尝试 |
| 原始卷 `\\.\D:`，GENERIC_READ | 打开 ERROR_ACCESS_DENIED (5) | 未尝试 | 未尝试 |
| 根目录 `D:\`，BACKUP_SEMANTICS，access=0 | 成功，80 字节 | ERROR_ACCESS_DENIED (5) | 成功，65480 字节 |
| 根目录 `D:\`，BACKUP_SEMANTICS，GENERIC_READ | 成功，80 字节 | ERROR_ACCESS_DENIED (5) | 成功，65480 字节 |

两种根目录访问模式，各一次非等待、有界读取：**1023 条 V2 记录、文件名长度非零记录 0 条**。仅解析记录长度/版本/名称长度作结构核验，不保存文件名或内容。没有执行 ENUM_USN_DATA、创建/删除/调整日志、遍历整个日志、修改目标文件、触发变化来检验事件覆盖。

### 对方案的约束 / Design implications

- 非特权通道确实可用，不能把“普通进程”直接等同于“无法读取任何 USN”。
- 无名称记录可以作为已有文件标识缓存的失效信号候选；**本实验未验证理由位、父目录、重命名、硬链接、安全描述符变化的完整覆盖与行为**。
- 不能将该结果视为完整路径更新流；路径映射需要目录重扫/其他校验与保守失效。
- 32 MiB 日志存在截断/覆盖风险：跨运行缓存必须校验卷身份、日志身份、游标有效区间；中断、权限变化、格式未知和日志 gap 均不能静默信任缓存。
- 对“事件读取成功”的证据与“缓存正确性已证明”必须严格分开。后者仍需要专门故障/并发/重命名/崩溃恢复测试。

## 持久脚本复核 / Durable-script revalidation

2026-09-10 再次执行三个持久脚本：Python 语法检查通过；以 -O 执行摘要 21/21、统计全部通过，USN 单批仍为 1023 条 V2、0 条带名称记录。解析器额外 12 项隔离检查通过，涵盖非法根目录、短响应/短 V2/V3、未知版本以及合法无名称 V2/V3。compute 和 ring 探针再次通过。摘要输出不包含用户账号、文件名或本地绝对路径。
