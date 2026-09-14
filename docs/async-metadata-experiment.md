# 异步目录元数据实测 / Asynchronous directory metadata experiment

日期：2026-09-10。平台：Windows 11 build 26200，NTFS，本机普通权限。
状态：隔离实验；不修改生产读取、遍历或缓存路径。

## 结论 / Result

**异步目录句柄 + NtQueryDirectoryFile 可以正确批量取得元数据，但本轮所有实际请求均
在调用内完成，STATUS_PENDING=0；没有观察到异步重叠，也没有观察到异步模式加速。**

Native directory queries on overlapped handles returned correct metadata. All observed
requests completed inline: this validates the interface path, not deferred completion or overlap.

这个结果不能推导“Windows不能异步枚举”，也不能将句柄带有OVERLAPPED标志直接算作
已经发生异步执行。下一步应优先验证批处理本身的收益，再考虑高延迟负载上的异步重叠。

## 方法 / Method

实现：`tools/research_validation/async_directory_metadata.py`，仅依赖Python标准库，
通过ctypes调用kernel32和ntdll，要求64位Windows。

- `CreateFileW(FILE_LIST_DIRECTORY, SHARE_READ|WRITE|DELETE, OPEN_EXISTING,
  FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_OVERLAPPED)`。
- `NtQueryDirectoryFile` 使用原生信息类`FileIdExtdDirectoryInformation=60`，不是Win32类19。
- 每个目录一个稳定的事件、`IO_STATUS_BLOCK`和缓冲；每目录至多一个在途请求，最多16个目录。
- 查询返回`STATUS_PENDING`才等待事件，随后读取状态块；其他返回直接使用函数返回状态。
- 同步对照使用相同原生查询、信息类和解析器，但不设置OVERLAPPED标志、不传事件。
- 全部元数据在计时前后分别用`os.listdir`清单以及逐条
  `GetFileInformationByHandleEx(Basic/Standard/Id)`核验。首次异步遍历安排在oracle之前，
  但这仍不代表清空了操作系统缓存。

This is the native event/IO_STATUS_BLOCK interface, **not** a Win32 metadata function
accepting an OVERLAPPED pointer, and **not IOCP**. Each directory has one continuation in flight.

### 核验字段 / Checked fields

核验文件名集合、128位File ID、CreationTime、LastWriteTime、ChangeTime、属性以及普通
文件的EndOfFile。不核验可能随访问变化的LastAccessTime。

初次核验发现：非空目录的StandardInfo EndOfFile为65536，而目录枚举信息为0，其他字段
相同。**目录的EOF不是普通文件内容大小**，因此明确排除目录size比较，不隐瞒其差异。
普通文件size仍逐项比较。目录元数据不因此成为未来文件版本或内容快照。

## 数据集与测量 / Workload and measurements

独立语料：16个目录×256个普通文件，UTF-16中文名称、0–4095字节变化长度；每目录额外
一个硬链接；另有空目录及根目录。共遍历18个目录，核验4129个条目（4112个文件路径、
17个目录条目）。语料只创建在新的`build/async-metadata/fixture-*`中并保留，不覆盖或删除
现有文件。硬链接属于同一物理对象的多个名称，不当作独立存储。

每个缓冲配置一个独立语料；一次非计时预热，7轮交替正反顺序。还在实际`src`目录测了
19个条目、3轮。计时含目录打开/关闭、事件分配、查询、Python解析和结果收集，不含
逐文件oracle、语料创建及外部结果比较。没有文件内容读取或BLAKE3计算。

| 配置 | 同步批量中位 ms | 异步句柄深度1 ms | 异步句柄深度16 ms | 每轮查询数 | PENDING |
|---|---:|---:|---:|---:|---:|
| 4129条目，64KiB缓冲 | 8.843 | 10.245 | 10.280 | 36 | 0 |
| 4129条目，4KiB缓冲 | 8.784 | 12.196 | 11.201 | 148 | 0 |
| 实际src，19条目 | 0.144 | 0.231 | 0.230 | 2 | 0 |

两个语料的首次异步遍历也都未返回PENDING。`peak_pending`表示未收割的pending请求数量，
不是内核实际并行度；`not_ready_on_return`额外检查函数返回时事件是否尚未就绪，本次同样为0。
深度16是上限，实际没有积累pending请求，不能把它报告为16路异步执行。

64KiB语料的单次逐文件oracle约296.6ms，但它执行清单+每文件打开+三类查询，与批量枚举
不是等工作量基线。**不能把这个差异归因于异步，也不能直接作为生产扫描加速比。**

## 限制与生命周期 / Limitations and lifetime

- 热缓存为主；没有清系统缓存、网络文件系统、高延迟设备、IOCP或原生C++性能基准。
- Python调度/解析占计时；短试次、固定交替顺序、不同缓冲使用不同语料，不能据此断言
  4KiB与64KiB谁普遍更优，也不能精确预测生产线程池收益。
- 未在实际设备上覆盖PENDING完成、取消或驱动挂起。成功的inline路径不能替代这些验证。
- 文件变化、元数据不匹配、未知状态、溢出或成功但0字节响应均显式失败，不输出伪完整清单。
  本受限探针不做动态缓冲扩容；生产实现仍需处理这些合法边界并重试。
- 无硬超时承诺。正常退出和异常清理都等待pending请求完成后释放缓冲；CLI临时忽略Ctrl+C，
  防止原生返回与pending状态发布之间的KeyboardInterrupt竞争，排空后恢复信号处理。
  不将“请求取消”当“安全释放”；本轮没有修改系统TDR或设备配置。

## 复现 / Reproduce

```powershell
python tools/research_validation/async_directory_metadata.py --fixture --buffer 65536 --trials 7
python tools/research_validation/async_directory_metadata.py --fixture --buffer 4096 --trials 7
python tools/research_validation/async_directory_metadata.py --root src --trials 3
python -m py_compile tools/research_validation/async_directory_metadata.py
```

`--root`只枚举一个目录，不递归、不修改其中内容；语料模式显式创建新测试文件。
原始结果：[64KiB](../benchmarks/async-metadata-20260910/results-64k.jsonl)、
[4KiB](../benchmarks/async-metadata-20260910/results-4k.jsonl)、
[实际src](../benchmarks/async-metadata-20260910/results-src.jsonl)。

## 对方案的修正 / Design implication

原先的“I/O Ring没有元数据操作码”结论不变；但不应扩大为“Windows没有原生异步目录
查询”。为元数据优化增加一个可测候选：`NtQueryDirectoryFile`批量查询及其异步句柄模式。
本轮证据支持先优先研究**减少请求次数**，没有支持切换默认遍历到异步模式。

另一个重要边界：微软明确说明`ZwQueryInformationFile/ZwSetInformationFile`即使作用于
异步打开的文件对象，也属于始终同步的操作。因此不能把目录查询的能力推广到任意单文件
属性查询。目录枚举得到丰富字段不代表跳过新鲜句柄验证就保持同等一致性。

### 一手来源 / Primary sources

- [NtQueryDirectoryFile](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntquerydirectoryfile)：异步句柄、事件、分页及并发目录变动边界。
- [NtQueryDirectoryFileEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntquerydirectoryfileex)：信息类60及同目录游标的串行性；本探针未使用Ex。
- [FILE_ID_EXTD_DIR_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_extd_dir_info)：88字节固定头及变长名称。
- [IO_STATUS_BLOCK](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_io_status_block)：PENDING与最终状态的使用规则。
- [IoIsOperationSynchronous](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ioisoperationsynchronous)：单文件QueryInformation仍同步。
