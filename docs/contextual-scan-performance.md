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

## Post-startup-budget revision / 启动预算修订后复测

Binary SHA-256: `a45ca83e29dfdd3e8aee7524f06d76f3a65c0f46b3cfe20eaabf02f4dc766e0d`. Identical harness, new isolated fixtures, two warmups and seven paired randomized measured rounds. Both new runs completed all 148 invocations with expected groups, full hashing, no cache bypass and zero fallback. Earlier adverse results above remain intact. The experiments are sequential, not a randomized old/new binary crossover, so wall-time differences across revisions are descriptive rather than controlled causal effect sizes.

同一工具、全新独立状态、两轮预热七轮测量；共148次调用全部正确且无回退。保留前次负面基线。版本间顺序测量而非新旧二进制交叉随机试验，跨版本时间差只能描述，不能视为精确因果效应。

- Default floor / 默认阈值: `build/contextual-scan-1cbdd7effb08`
- Zero floor / 零阈值: `build/contextual-scan-70cab17b4389`

Each directory retains `raw.jsonl`, `summary.json`, `environment.json`. Times below are median milliseconds; discovery is separately instrumented and must not be silently classified as zero startup when context creation is deferred.

各目录保留完整原始数据。下表中位数单位毫秒；发现设备的时间独立记录，不能因延迟创建上下文就将其视为零启动成本。

| Floor | Workload | Mode | Wall | iGPU discovery | iGPU setup | Model save | iGPU attempted range |
|---|---|---|---:|---:|---:|---:|---|
| 16MiB | small | cpu_pgo | 112.325 | 0.000 | 0.000 | 3.845 | 0–0 |
| 16MiB | small | cpu_no_pgo | 103.421 | 0.000 | 0.000 | 0.000 | 0–0 |
| 16MiB | small | auto_pgo | 117.687 | 0.000 | 0.000 | 4.061 | 0–0 |
| 16MiB | small | auto_no_pgo | 109.090 | 0.000 | 0.000 | 0.000 | 0–0 |
| 16MiB | mixed | cpu_pgo | 110.020 | 0.000 | 0.000 | 3.636 | 0–0 |
| 16MiB | mixed | cpu_no_pgo | 97.351 | 0.000 | 0.000 | 0.000 | 0–0 |
| 16MiB | mixed | auto_pgo | 268.246 | 136.167 | 0.000 | 3.749 | 0–0 |
| 16MiB | mixed | auto_no_pgo | 218.989 | 124.857 | 0.000 | 0.000 | 0–0 |
| 0 | small | cpu_pgo | 115.196 | 0.000 | 0.000 | 3.324 | 0–0 |
| 0 | small | cpu_no_pgo | 102.305 | 0.000 | 0.000 | 0.000 | 0–0 |
| 0 | small | auto_pgo | 216.927 | 132.362 | 0.000 | 3.452 | 0–0 |
| 0 | small | auto_no_pgo | 169.469 | 130.180 | 0.000 | 0.000 | 0–0 |
| 0 | mixed | cpu_pgo | 104.439 | 0.000 | 0.000 | 3.304 | 0–0 |
| 0 | mixed | cpu_no_pgo | 89.021 | 0.000 | 0.000 | 0.000 | 0–0 |
| 0 | mixed | auto_pgo | 222.275 | 121.869 | 0.000 | 3.599 | 0–0 |
| 0 | mixed | auto_no_pgo | 185.812 | 126.003 | 0.000 | 0.000 | 0–0 |

**Verified:** default-floor mixed auto PGO has `igpu_attempted=0`, `igpu_setup_ms=0`, and `igpu_cold_spent_ms=0` in every measured run. Thus the new admission path avoids expensive iGPU context/kernel initialization in this short workload. It does not eliminate all costs: device discovery takes 122.941–145.232 ms (median 136.167 ms), and CUDA worker setup reaches roughly 90–101 ms. Auto PGO remains slower than forced CPU; auto without PGO now also incurs discovery. CPU-only model persistence remains a measurable millisecond lifecycle cost, not eliminated by this revision.

**已核实：** 默认阈值混合自动 PGO 的七轮均未尝试 iGPU 初始化、setup/spent 为零，说明预算准入有效避免短任务的昂贵上下文/内核初始化。但发现设备仍为122.941–145.232毫秒（中位136.167），CUDA 启动仍约90–101毫秒。自动 PGO 仍慢于 CPU，自动无 PGO 现在也承担设备发现成本。CPU 模型持久化仍有毫秒级生命周期成本，不能声称此次修订完全消除开销。

Priority follow-up: defer discovery itself until useful work justifies its cost, or persist safely invalidated device identity without creating an ICD context. Re-measure CPU-only persistence separately from prediction arithmetic. A short-workload admission test is not sufficient evidence that sustained work eventually activates acceleration; that requires a separate live-device activation test.

优先后续：设备发现也需按有效工作量延迟，或安全保存并失效设备身份；单独评估模型持久化成本。短负载不初始化不证明长负载最终能启用加速，需要另行真实设备激活实验。

### Post-fix raw wall samples / 修订后原始墙钟

```json
[
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "cpu_pgo",
    "wall_ms": [
      124.7043,
      118.1859,
      112.3245,
      111.7967,
      110.1893,
      176.004,
      105.4491
    ]
  },
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      103.4209,
      128.4205,
      106.1287,
      100.971,
      100.322,
      106.2231,
      95.0369
    ]
  },
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "auto_pgo",
    "wall_ms": [
      133.2904,
      120.9668,
      113.3813,
      117.6869,
      111.1153,
      145.2946,
      106.7484
    ]
  },
  {
    "floor": "16MiB",
    "workload": "small",
    "mode": "auto_no_pgo",
    "wall_ms": [
      112.6733,
      109.09,
      97.3497,
      113.4987,
      108.4205,
      109.4498,
      92.4075
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "cpu_pgo",
    "wall_ms": [
      171.1151,
      128.5442,
      108.951,
      107.3009,
      110.0198,
      103.43,
      158.197
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      96.0948,
      128.493,
      92.583,
      97.351,
      95.2724,
      108.6282,
      109.5844
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "auto_pgo",
    "wall_ms": [
      267.3014,
      286.4894,
      290.999,
      265.9302,
      251.9184,
      268.2459,
      277.7493
    ]
  },
  {
    "floor": "16MiB",
    "workload": "mixed",
    "mode": "auto_no_pgo",
    "wall_ms": [
      196.5997,
      368.2068,
      297.0397,
      221.9861,
      213.5607,
      214.2449,
      218.9888
    ]
  },
  {
    "floor": "0",
    "workload": "small",
    "mode": "cpu_pgo",
    "wall_ms": [
      107.4831,
      115.1957,
      104.0655,
      108.3558,
      138.4125,
      117.5266,
      121.9118
    ]
  },
  {
    "floor": "0",
    "workload": "small",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      105.0344,
      94.776,
      102.3051,
      99.0982,
      90.0051,
      121.2083,
      107.7927
    ]
  },
  {
    "floor": "0",
    "workload": "small",
    "mode": "auto_pgo",
    "wall_ms": [
      191.2551,
      206.0172,
      220.6466,
      199.9358,
      216.927,
      220.9347,
      227.3589
    ]
  },
  {
    "floor": "0",
    "workload": "small",
    "mode": "auto_no_pgo",
    "wall_ms": [
      169.4685,
      152.3991,
      170.6853,
      159.7564,
      157.8028,
      183.0214,
      171.7009
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "cpu_pgo",
    "wall_ms": [
      104.4392,
      97.7622,
      107.5653,
      108.8236,
      107.469,
      100.8757,
      90.4667
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      89.7131,
      87.4412,
      100.894,
      89.7999,
      89.0214,
      83.6993,
      84.9264
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "auto_pgo",
    "wall_ms": [
      222.2752,
      217.0944,
      245.6962,
      235.6188,
      218.2622,
      253.7567,
      217.6245
    ]
  },
  {
    "floor": "0",
    "workload": "mixed",
    "mode": "auto_no_pgo",
    "wall_ms": [
      192.0012,
      185.2623,
      212.59,
      203.5501,
      184.9649,
      180.6208,
      185.8117
    ]
  }
]
```

## Final preflight-gated revision / 最终发现前预算准入修订

Binary SHA-256: `1e8ecb98d0e051c75058f71c7eb7543ef2cc161f3145a76c2c9da514f24cd714`. Fresh default-floor fixture `build/contextual-scan-2a4d0ef05198`, identical 2-warmup/7-measurement randomized pairs, 74 successful scans. All correctness/hash assertions passed. Earlier results remain above.

全新默认阈值语料，74次扫描全部正确；保留此前证据。

| Workload | Mode | Median wall ms | Discovery ms | iGPU setup ms | Model save ms |
|---|---|---:|---:|---:|---:|
| small | cpu_pgo | 106.571 | 0.000 | 0.000 | 3.321 |
| small | cpu_no_pgo | 94.723 | 0.000 | 0.000 | 0.000 |
| small | auto_pgo | 104.147 | 0.000 | 0.000 | 3.378 |
| small | auto_no_pgo | 96.225 | 0.000 | 0.000 | 0.000 |
| mixed | cpu_pgo | 106.972 | 0.000 | 0.000 | 3.929 |
| mixed | cpu_no_pgo | 93.168 | 0.000 | 0.000 | 0.000 |
| mixed | auto_pgo | 101.664 | 0.000 | 0.000 | 3.920 |
| mixed | auto_no_pgo | 105.399 | 0.000 | 0.000 | 0.000 |

Both automatic modes now avoid iGPU discovery and initialization in every measured short-workload run. This removes the previously observed unnecessary iGPU startup path; the experiment does not demonstrate GPU acceleration because all such requests stay on CPU. Remaining PGO overhead and noisy wall samples are visible, not hidden by subtracting lifecycle costs. A separate activation experiment below tests that admission is not simply permanent GPU disablement.

两种自动模式所有测量均未发现或初始化 iGPU，消除了前次不必要启动路径；这些扫描都留在 CPU，所以不证明 GPU 加速。PGO 剩余开销及噪声保留，不通过减去生命周期成本掩盖。以下独立实验验证准入并非永久禁用 GPU。

```json
[
  {
    "workload": "small",
    "mode": "cpu_pgo",
    "wall_ms": [
      106.571,
      109.8494,
      104.8753,
      100.2161,
      120.7809,
      108.9218,
      105.028
    ]
  },
  {
    "workload": "small",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      93.147,
      95.5419,
      95.8965,
      93.7813,
      90.3894,
      98.6011,
      94.7226
    ]
  },
  {
    "workload": "small",
    "mode": "auto_pgo",
    "wall_ms": [
      105.6456,
      104.1473,
      104.0992,
      103.9438,
      111.5853,
      103.9307,
      113.6262
    ]
  },
  {
    "workload": "small",
    "mode": "auto_no_pgo",
    "wall_ms": [
      105.0728,
      94.6847,
      95.7831,
      96.2246,
      97.7792,
      93.7296,
      103.3876
    ]
  },
  {
    "workload": "mixed",
    "mode": "cpu_pgo",
    "wall_ms": [
      97.3245,
      99.2118,
      119.8524,
      122.0504,
      115.7537,
      106.9716,
      92.3877
    ]
  },
  {
    "workload": "mixed",
    "mode": "cpu_no_pgo",
    "wall_ms": [
      89.7037,
      85.1372,
      92.2544,
      93.1678,
      106.0641,
      163.3221,
      93.3625
    ]
  },
  {
    "workload": "mixed",
    "mode": "auto_pgo",
    "wall_ms": [
      98.5263,
      100.0185,
      118.0517,
      114.0122,
      119.6771,
      101.6641,
      95.8114
    ]
  },
  {
    "workload": "mixed",
    "mode": "auto_no_pgo",
    "wall_ms": [
      92.1233,
      86.444,
      105.5772,
      105.3991,
      118.3779,
      113.0296,
      93.768
    ]
  }
]
```

### Real positive-credit activation / 真实设备正预算激活

Separate diagnostic, **not default production policy**, not part of paired timing. Dataset: 32 unique random 16 MiB files (512 MiB), Python `random.Random(432).randbytes(16<<20)` for each file in index order; expected duplicate group set is empty. Each process used the same binary and `--rehash --summary --no-telemetry --format=tsv --color=never`. First forced CPU with `--no-pgo` established ground truth, then two automatic processes shared model state. Every process hashed all 32 files and produced the expected empty group set.

独立诊断而非默认策略，也不混入配对测量。32个唯一随机16MiB文件，固定种子432；CPU基准及随后两次自动扫描均全量哈希、输出一致。

```toml
workers=1
metadata_workers=1
block_bytes=1048576
memory_bytes=134217728
device_memory_bytes=33554432
gpu_min_bytes=16777216
backend="auto"
igpu_bootstrap_ms=1
cuda_bootstrap_ms=1000000
cold_exploration_fraction=1
```

Raw diagnostics and exact config: `build/contextual-activation-15d0809f0e1e/activation.json` and `.same/config.toml`.

| Process | Wall ms | Earned credit ms | Discovery ms | Setup ms | iGPU hashes | Setup prior hits | Setup estimate ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| CPU ground | 278.908 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 1.000 |
| Auto 1 | 790.090 | 227.389 | 125.140 | 354.620 | 1.000 | 0.000 | 1.000 |
| Auto 2 | 426.108 | 247.545 | 125.120 | 0.000 | 0.000 | 1.000 | 354.620 |

First automatic process activates the physical Iris Xe and hashes one complete 16 MiB payload, persisting one setup-history key. Its optimistic 1 ms bootstrap underestimates actual setup (354.620 ms), so total spent 479.760 ms exceeds earned 227.389 ms; this is a deliberately aggressive exploratory policy, not a hard runtime spending guarantee. The second process restores the measured 354.620 ms setup prior, has 247.545 ms earned minus 125.120 ms discovery, and correctly does not activate iGPU (`igpu_attempted=0`). Model prior hits are 2 and setup prior hits 1. This is direct live-device evidence of credit-funded activation followed by cross-process measured-cost reuse; it is not a speedup result (CPU ground is faster).

首次自动扫描真实激活 Iris Xe 并完成16MiB哈希，保存一条启动历史。故意乐观的1ms先验低估实际354.620ms启动，累计花费479.760ms超过挣得227.389ms，因此这种探索策略不是运行时绝对支出上限。第二进程恢复354.620ms先验，247.545ms预算扣除125.120ms发现后不足，正确不再激活；模型命中2次、启动历史命中1次。这直接证明真实设备预算激活及跨进程成本复用，但不是性能加速结果，CPU基准更快。
