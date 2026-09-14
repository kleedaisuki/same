# BLAKE3 iGPU feasibility experiment / 核显可行性实验

Date / 日期: 2026-09-10. Base / 基线: `a4cacae02c96f50b5d3bdfa51784f1671c1dcc82`.

## Decision / 判断

Intel Iris Xe can compute verified full BLAKE3 digests. This prototype merits further
large-buffer investigation, not production integration yet. Small buffers lose heavily.
核显完整摘要验证通过；大缓冲值得继续实验，小缓冲明显不合算。不能据此认定
CPU+iGPU 协同加速有效，也不能宣称胜过优化后的 CUDA 或多核 CPU。

No production code, configuration, persisted digest format, or dispatch policy changed.
未改变生产代码、配置、摘要格式及调度策略。

## Setup and scope / 设置与范围

- Windows 11 build 26200; i9-12900H; Iris Xe driver 31.0.101.4502;
  RTX 3070 Ti Laptop GPU driver 32.0.15.7261.
- OpenCL prototype: Python 3.12, numpy 2.2.6, pyopencl 2026.1.4, blake3 1.0.8.
  Production C/CUDA rebuilt in Release; official C BLAKE3 1.8.2 with x64 assembly SIMD.
- Deterministic random host-resident input; one full 32-byte digest per operation.
  核显每次计算完整摘要，包含所有树层，不是仅计叶压缩。
- Prototype supports only power-of-two lengths >= 2048 bytes and little-endian devices.
  It is deliberately NOT a general incremental hasher. No empty/irregular-length support.
  原型仅支持至少 2048 字节的二次幂输入及小端设备，不是通用增量摘要实现。
- Every measured digest checked against the official Python BLAKE3 binding;
  production benchmark checks against the C backend with irregular update boundaries.
  每个计时摘要均校验，正确性不是只比较首字节。
- Five alternating-order trials, medians reported; one untimed iGPU warmup per size.
  OpenCL repeats: max(16, min(100, 16 MiB / input size)); production: 32 hashes/trial.
- Upload timings include host upload, all kernels, synchronization, digest readback,
  Python enqueue/profiling overhead and comparison. Exclude allocation, JIT and file I/O.
  含上传、内核、同步、回读和 Python 调度开销；不含分配、即时编译及磁盘读取。
- Resident timings omit input upload and reuse device data. They are NOT file-hashing
  throughput, NOR proof of zero-copy. 驻留模式不是文件吞吐，也未验证零拷贝。

## OpenCL paired experiment / 核显配对实验

Median MiB/s / 中位吞吐：

| Input | CPU Python binding, one thread | iGPU with upload | iGPU resident |
|---|---:|---:|---:|
| 2 KiB | 1206.38 | 4.87 | 8.03 |
| 4 KiB | 1856.58 | 8.81 | 13.71 |
| 64 KiB | 4181.16 | 100.67 | 133.18 |
| 1 MiB | 4615.47 | 1105.73 | 1425.59 |
| 16 MiB | 4087.78 | 6975.40 | 13215.84 |
| 64 MiB | 1984.54 | 7916.71 | 20264.43 |

## Production C/CUDA control / 生产实现对照

Separate runs, not a single randomized three-device experiment. Different bindings,
buffer generation, sample duration and CPU frequency/cache state limit cross-table ranking.
不同批次，不是三设备统一随机实验；语言绑定、输入生成、样本时长及频率/缓存状态不同。
Observed CPU variability is substantial, so precise speedup ratios are not established.
CPU 波动明显，不宣称精确加速倍数。

| Input | CPU, 1 MiB updates | CUDA, 1 MiB updates | CPU, whole-input update | CUDA, whole-input update |
|---|---:|---:|---:|---:|
| 1 MiB | 3014.26 | 3457.22 | 4286.10 | 4765.02 |
| 16 MiB | 2961.02 | 2726.58 | 3934.92 | 9505.46 |
| 64 MiB | 2057.48 | 3048.85 | 3938.34 | 9943.10 |

Eight CPU workers, 64 MiB input and 1 MiB updates, four hashes per worker budget,
five trials: aggregate median **19256.07 MiB/s**. Shared read-only input reused across
workers; not independent on-disk files. 八 CPU 工作线程合计约 18.8 GiB/s，但共用只读输入，
不能视为真实多文件磁盘吞吐。

These controls show why a weak CPU baseline or small CUDA blocks can exaggerate
the apparent benefit of iGPU. Strong SIMD/multicore baselines remain necessary.
对照说明：弱 CPU 基线及较小 CUDA 提交块会夸大核显收益。

## Windows asynchronous I/O / Windows 异步 I/O

`windows_ioring_probe.py` successfully queried capabilities, created an empty ring,
verified READ support and closed the ring. No disk throughput measurement performed.
已查询能力、成功创建空环、确认 READ 支持并关闭；尚未做异步磁盘性能实验。

- Maximum API version: 400 (version 4).
- Maximum submission/completion queue entries: 65536 / 131072.
- Create HRESULT: 0; READ supported: true.
- Current `src/files.cpp` opens without FILE_FLAG_OVERLAPPED and calls synchronous
  ReadFile with a null OVERLAPPED pointer. 当前项目仍为同步读取。

Windows offers overlapped I/O + I/O completion ports and newer I/O Rings. Neither is
a universal asynchronous wrapper for all system calls, nor API-compatible with Linux
io_uring. Probe capabilities at runtime and preserve a fallback.
Windows 提供重叠 I/O、完成端口以及 I/O Ring；它们不是任意系统调用异步化包装器，
也不与 Linux io_uring 接口兼容。应运行时探测并保留回退路径。

## Reproduce / 复现

From repository root, use an isolated Python 3.12 environment / 在仓库根目录运行：

```powershell
python -m pip install numpy==2.2.6 pyopencl==2026.1.4 blake3==1.0.8
python tools/igpu_blake3_experiment.py > opencl.jsonl
python tools/windows_ioring_probe.py
# Rebuild in a configured MSVC/CUDA environment. / 使用已配置的 MSVC/CUDA 环境。
cmake --build build/cuda --target dispatch_benchmark compute_tests --parallel 4
ctest --test-dir build/cuda -R '^compute$' --output-on-failure
build/cuda/dispatch_benchmark.exe --backend both --size 67108864 --block 67108864 --workers 1 --repeats 32 --trials 5
```

Raw measurements / 原始结果: `benchmarks/igpu-20260910/`.
Compute test passed, prototype digest checks passed, Python compilation passed.
No full project regression run; no production edits. 未运行完整回归，未修改生产实现。

## Next experiments / 后续实验

1. Common native harness, equal input/update sizes and trial durations, randomized order,
   plus CPU SIMD and multicore controls. 统一原生测试框架及输入粒度。
2. Measure CPU+iGPU against CPU-only at equal memory and power constraints; evaluate
   pinned/mapped buffers separately. 等内存与功耗约束验证协同收益及映射缓冲。
3. Compare synchronous reads, IOCP and I/O Rings on identical real files, separating
   warm/cold cache, queue depth, read-only throughput and read+hash throughput.
   对照缓存冷热、队列深度、纯读取及读取加摘要，避免把缓存吞吐当磁盘性能。
4. Only after these pass, implement arbitrary lengths, incremental updates, failure
   handling and resource lifetimes behind the existing Compute contract.
   之后才考虑完整边界、增量接口、错误回退和资源生命周期的生产实现。

## Sources / 资料

- [Official BLAKE3 implementation](https://github.com/BLAKE3-team/BLAKE3)
- [BLAKE3 design paper](https://raw.githubusercontent.com/BLAKE3-team/BLAKE3-specs/master/blake3.pdf)
- [Microsoft I/O completion ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)
- [Microsoft BuildIoRingReadFile](https://learn.microsoft.com/en-us/windows/win32/api/ioringapi/nf-ioringapi-buildioringreadfile)
- [Microsoft QueryIoRingCapabilities](https://learn.microsoft.com/en-us/windows/win32/api/ioringapi/nf-ioringapi-queryioringcapabilities)
- [CPU/iGPU co-execution research](https://arxiv.org/abs/2106.01726): motivates studying scheduling,
  not evidence of a BLAKE3 or this-machine speedup. 仅支持研究调度问题，不证明本机收益。
