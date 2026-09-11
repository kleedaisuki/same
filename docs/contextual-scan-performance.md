# Contextual scan performance / 上下文扫描性能

## Reproducible harness / 可复现工具

`tools/contextual_scan_benchmark.py` prepares a new uniquely named directory under `build` and never deletes existing files. Python 3.14.6 on this host is `D:/Python/3.14/python.exe`. Each mode has its own `.same` state and hardlinks to the same generated payload, so measured scans do not rewrite input data or copy a corpus. File identities are therefore shared across mode roots, but model/hash stores are not.

工具创建唯一目录、不删除已有数据。每种模式拥有独立状态，数据通过硬链接共享；计时阶段不创建或改写输入文件。不同根目录共享文件身份但不共享模型库或哈希库。

```powershell
D:/Python/3.14/python.exe tools/contextual_scan_benchmark.py
# After a stable build / 稳定构建后:
D:/Python/3.14/python.exe tools/contextual_scan_benchmark.py --prepared build/contextual-scan-489cc423268b --run
```

The fixed seed generates 512 files of 8–128 KiB and a second workload of 24 × 4 MiB plus 4 × 32 MiB (224 MiB). Sizes repeat so every file is a hash candidate; known duplicate pairs validate byte-comparison output. Four workers, two metadata workers, 1 MiB blocks, 128 MiB host budget, 32 MiB device budget and zero GPU size floor are held constant.

固定种子生成小文件和 224 MiB 混合负载。重复尺寸确保全部文件需要哈希，已知重复对验证输出正确性。固定四个计算线程、两个元数据线程、1 MiB 块、128 MiB 主机预算、32 MiB 设备预算及零 GPU 大小阈值。

Four modes: forced CPU with/without runtime PGO, automatic routing with/without runtime PGO. Each mode receives two unmeasured learning warmups followed by seven measured rounds; mode order is seeded-randomized within each round. CPU-only PGO contrast holds routing fixed to isolate profiling/model costs better than auto contrasts. The CLI has TSV output, not JSON: TSV groups are canonicalized to JSON arrays independent of group numbering and compared against both generated expectations and CPU ground truth. Every timed invocation uses `--rehash --summary --no-telemetry --format=tsv --color=never` and must report all files hashed and zero cache hits.

四模式为 CPU 开/关运行时 PGO，以及自动调度开/关 PGO。每模式两轮学习预热、七轮测量，每轮模式顺序确定性随机化。CPU 对照固定路由，更适合区分模型开销。TSV 输出转为编号无关的规范化 JSON 组；与已知语料及 CPU 结果逐项核对。每次强制重哈希，禁止缓存绕过测量。

Raw records retain stdout, stderr, all numeric summary metrics, wall milliseconds, phase, round and canonical groups in `raw.jsonl`. `environment.json` records binary SHA-256, Python and OS version; `summary.json` retains raw wall samples and medians. Runs reuse warm OS caches; there is no eviction, CPU affinity or frequency locking. The experiment measures CLI end-to-end cost including process and device startup, distinct from steady-state kernel throughput. Initial model learning is excluded from the seven measurement rounds but retained in raw output; persistence remains active with `--no-telemetry`.

原始记录保留全部输出、数值统计、墙钟、阶段及规范化组。环境文件记录二进制摘要和运行环境。缓存为热缓存，没有驱逐、亲和性或频率控制。测量包含 CLI 进程与设备启动，不等同于稳定内核吞吐量。学习预热不计入七轮结果但保留记录；关闭遥测不应关闭模型持久化。

## Results / 结果

Measured 2026-09-11 on Windows 11 build 26200, Python 3.14.6, i9-12900H. Binary SHA-256: `2480116cd87977f0ab62da3b3b9489baaf67307bb21ca7f74b6b6979c4f7655e`. The binary was held unchanged across both experiments. Both completed 74 invocations including ground-truth and warmup (148 total), with identical expected groups, all payloads hashed, zero cached hashes, and zero fallback counts. Raw diagnostic records are retained locally in the following directories, not assumed checked into Git:

两组实验共148次调用均通过输出一致性及全量哈希断言，无回退。二进制保持不变。原始数据在下列本地目录中；不假定其被 Git 跟踪。

- Zero-floor stress / 零阈值压力组: `build/contextual-scan-489cc423268b/{raw.jsonl,summary.json,environment.json}`
- Default 16 MiB floor / 默认阈值组: `build/contextual-scan-af8d0a10cced/{raw.jsonl,summary.json,environment.json}`

```powershell
# New default-floor fixture and run / 新建默认阈值语料并执行
D:/Python/3.14/python.exe tools/contextual_scan_benchmark.py --gpu-min-bytes 16777216 --run
```

The earlier preparation `019f5105807a` failed its first ground assertion because the harness initially did not decode quoted TSV paths; parser corrected before both reported datasets. No performance observations from that failed invocation are mixed into these results.

早期准备目录的首次校验因工具未解码 TSV 引号失败；修正后重新创建两套语料。失败调用没有混入统计。

### Medians / 中位数

All time columns are milliseconds. `hash_work_ms` is accumulated worker work, not scan wall time; it can exceed wall time under concurrency. Model startup/save are separate lifecycle measurements; do not add all timing columns because some overlap. CPU PGO overhead includes model persistence, not only the nanosecond API measured by the microbenchmark.

时间单位毫秒。哈希工作时间是多工作线程累计值，可超过墙钟；各列可能重叠，不能相加。CPU 开关 PGO 的对照还包含持久化开销，不只是微基准的 API 成本。

| Floor | Workload | Mode | Wall | Hash work | iGPU setup | Model load | Model save | Prior hits | CPU/CUDA/iGPU hashes |
|---|---|---|---:|---:|---:|---:|---:|---:|---|
| 0 | small | cpu_pgo | 97.498 | 243.686 | 0.000 | 1.436 | 5.133 | 4 | 512/0/0 |
| 0 | small | cpu_no_pgo | 88.652 | 244.263 | 0.000 | 0.000 | 0.000 | 0 | 512/0/0 |
| 0 | small | auto_pgo | 553.049 | 163.913 | 480.255 | 1.265 | 7.149 | 6 | 512/0/0 |
| 0 | small | auto_no_pgo | 89.606 | 246.291 | 0.000 | 0.000 | 0.000 | 0 | 512/0/0 |
| 0 | mixed | cpu_pgo | 94.517 | 117.501 | 0.000 | 1.150 | 5.494 | 4 | 28/0/0 |
| 0 | mixed | cpu_no_pgo | 87.078 | 118.211 | 0.000 | 0.000 | 0.000 | 0 | 28/0/0 |
| 0 | mixed | auto_pgo | 576.437 | 127.311 | 479.752 | 1.139 | 9.581 | 6 | 27/1/0 |
| 0 | mixed | auto_no_pgo | 87.800 | 119.114 | 0.000 | 0.000 | 0.000 | 0 | 28/0/0 |
| 16MiB | small | cpu_pgo | 100.680 | 259.071 | 0.000 | 1.271 | 5.381 | 4 | 512/0/0 |
| 16MiB | small | cpu_no_pgo | 91.218 | 264.190 | 0.000 | 0.000 | 0.000 | 0 | 512/0/0 |
| 16MiB | small | auto_pgo | 101.645 | 264.024 | 0.000 | 1.203 | 5.436 | 4 | 512/0/0 |
| 16MiB | small | auto_no_pgo | 93.769 | 258.518 | 0.000 | 0.000 | 0.000 | 0 | 512/0/0 |
| 16MiB | mixed | cpu_pgo | 94.674 | 123.690 | 0.000 | 1.250 | 5.938 | 4 | 28/0/0 |
| 16MiB | mixed | cpu_no_pgo | 87.249 | 118.086 | 0.000 | 0.000 | 0.000 | 0 | 28/0/0 |
| 16MiB | mixed | auto_pgo | 612.283 | 115.912 | 491.830 | 1.719 | 11.062 | 6 | 27/1/0 |
| 16MiB | mixed | auto_no_pgo | 89.087 | 126.588 | 0.000 | 0.000 | 0.000 | 0 | 28/0/0 |

### Interpretation / 解释

Observed: the default floor avoids accelerator initialization entirely for this small-file workload. CPU PGO adds roughly 7–10 ms in these median contrasts; model loading plus durable saving account for several milliseconds, so attributing this whole difference to regression arithmetic would be wrong. No statistical significance claim is made from seven samples.

观察：默认阈值让小文件完全避免设备初始化。CPU PGO 中位数差约7–10毫秒，模型加载及持久化占其中数毫秒；不能把全部差值归因于回归运算。七轮不支持强统计显著性断言。

Observed: auto PGO is substantially slower on this short mixed workload even at the default floor: 612.283 ms vs 89.087 ms without PGO. iGPU setup is 491.830 ms while zero iGPU hashes complete; CUDA typically handles one file. This strongly points to startup/exploration lifecycle overhead, not slow iGPU hash throughput, although no causal profiler trace was captured. Persisted priors are loaded on later runs, but the measured implementation still pays accelerator startup. Model reuse alone does not amortize driver setup across processes.

观察：默认阈值混合负载自动 PGO 为612.283毫秒，无 PGO 为89.087毫秒。iGPU 初始化491.830毫秒但完成零次哈希，CUDA 通常只计算一个文件。证据强烈指向启动/探索生命周期开销，而不是 iGPU 哈希吞吐量；未采集因果剖析轨迹，不把推断冒充直接证明。跨运行先验确实恢复，但仍重复设备启动，模型复用不能自动摊销跨进程驱动启动。

Recommendation: account for startup cost and remaining eligible work before device initialization; measure lazy-admission/warm-device alternatives separately. Repeat this paired harness after any fix, retaining the adverse baseline. Larger sustained eligible workloads and distribution-shift tests remain needed before claiming routing benefit. These measurements demonstrate functioning persistence and output correctness, **not** that learned scheduling improves end-to-end time.

建议：设备初始化决策同时考虑启动成本及剩余适用负载；分别测量延迟准入和热设备方案。修改后保留本次不利基线重测。证明调度收益仍需更长的大负载及分布变化实验。本实验验证持久化和输出正确性，不证明学习调度提升端到端性能。

### Raw measured wall times / 测量墙钟原始样本

```json
[
  {
    "floor": "0",
    "workload": "small",
    "mode": "cpu_pgo",
    "wall_ms": [
      92.4899,
      97.3734,
      97.4977,
      94.1637,
      110.5101,
      99.4467,
      112.4965
    ]
  },
  {
    "floor": "0",
    "workload": "small",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      88.6522,
      86.4161,
      95.042,
      85.6232,
      85.9734,
      109.6934,
      96.7954
    ]
  },
  {
    "floor": "0",
    "workload": "small",
    "mode": "auto_pgo",
    "wall_ms": [
      570.3051,
      546.7763,
      548.0147,
      552.5087,
      553.0494,
      598.3561,
      575.0976
    ]
  },
  {
    "floor": "0",
    "workload": "small",
    "mode": "auto_no_pgo",
    "wall_ms": [
      88.8317,
      88.1711,
      89.6055,
      84.8856,
      101.6946,
      137.1147,
      93.3178
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "cpu_pgo",
    "wall_ms": [
      92.8394,
      90.6128,
      96.9703,
      98.049,
      92.1551,
      98.8418,
      94.5169
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      95.9845,
      94.6716,
      87.0784,
      79.1148,
      76.0148,
      91.997,
      82.8625
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "auto_pgo",
    "wall_ms": [
      587.681,
      572.5093,
      593.5977,
      576.2851,
      580.1994,
      576.4371,
      568.9869
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "auto_no_pgo",
    "wall_ms": [
      85.1997,
      87.0296,
      82.2808,
      89.9637,
      94.9598,
      91.7975,
      87.8002
    ]
  },
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "cpu_pgo",
    "wall_ms": [
      99.1948,
      104.1082,
      103.3769,
      101.6799,
      96.8343,
      100.6799,
      93.0228
    ]
  },
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      86.9044,
      90.71,
      97.1959,
      104.1049,
      97.445,
      91.2176,
      85.9604
    ]
  },
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "auto_pgo",
    "wall_ms": [
      99.0115,
      103.6598,
      110.9936,
      101.645,
      103.4548,
      94.2122,
      100.2913
    ]
  },
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "auto_no_pgo",
    "wall_ms": [
      95.3598,
      93.7689,
      89.1571,
      101.1549,
      107.3215,
      88.1514,
      84.6204
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "cpu_pgo",
    "wall_ms": [
      102.5596,
      94.545,
      91.9045,
      94.674,
      97.6987,
      97.5088,
      94.4572
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      85.7405,
      85.0352,
      91.803,
      85.4159,
      94.3885,
      87.2489,
      103.6933
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "auto_pgo",
    "wall_ms": [
      634.7723,
      606.2356,
      612.9213,
      612.2829,
      633.4778,
      595.91,
      603.1029
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "auto_no_pgo",
    "wall_ms": [
      87.2435,
      89.0871,
      87.0958,
      188.4818,
      94.1355,
      87.2622,
      138.4371
    ]
  }
]
```
