# 小文件扫描基准 / Small-file scan benchmark

`tools/scan_benchmark.py` 只生成合成数据，不扫描真实用户目录。Python 3.10+，无额外依赖。
The script generates synthetic data only; Python 3.10+ needs no third-party packages.

```powershell
python tools/scan_benchmark.py --exe .cache/perf/same-baseline.exe --exe build/cuda/same.exe --output .cache/perf/comparison-2000 --files 2000 --trials 3 --backend cpu
```

输出目录必须不存在，数据位于其 `dataset` 子目录；日志、JSON 和每次归档的 `.same` 状态全部放在扫描根以外，避免污染扫描。不要提交生成数据或报告。
The output directory must not exist. Logs, JSON, and archived per-run state stay outside the scanned dataset. Do not commit generated data/reports.

## 控制变量 / Controls

- 默认 2,000 文件，大小按 `0,128,1024,4096,16384,65536` 字节循环；用 `--sizes` 覆盖。Default sizes cycle deterministically; override with `--sizes`.
- `--layout flat|wide|deep|mixed`：单目录、257 个子目录、12 层目录、三者混合。Controls directory shape.
- `--duplicate-every 10`：每第十个文件复制前一文件；`0` 禁用显式复制。空文件自然重复。Every tenth file duplicates its predecessor; zero disables explicit copies, not inherent empty-file duplicates.
- `--seed` 固定伪随机内容；默认 `--workers 8 --queue-capacity 512 --backend cpu`。Seed fixes content; workers/backend/queue remain constant across executable comparisons.
- `--metadata-workers N` 仅在显式提供时写入配置，适合新版单程序消融；与不支持该参数的旧版比较时不要提供。Only writes the metadata-worker configuration when explicitly requested; omit for comparisons with older executables that reject this key.
- 每个程序/试次新建独立状态，先 `--rehash`，立即接正常缓存运行。不同试次交替程序顺序，减少顺序偏差。Each executable/trial receives fresh state, a forced hash run, then a warm-cache run; executable order reverses on alternate trials.

**fresh-rehash 是应用冷状态，不是操作系统冷缓存。** 文件刚生成，页缓存、设备缓存、防病毒软件、温度及其他负载均可能影响结果；脚本不清除系统缓存。不要把此结果外推成真实冷盘吞吐。
**Fresh application state is not cold OS cache.** Newly generated files, device cache, antivirus, thermals, and other load affect measurements. The script does not purge OS caches; do not extrapolate to cold-drive throughput.

## 正确性与度量 / Correctness and metrics

每次核对 stdout 的重复组与生成内容的 SHA-256 分组；忽略组号及组内/组间顺序，同时记录原始 stdout SHA-256、退出码、进程墙钟时间和 stderr 全部数值字段。JSON 保留程序 SHA-256、Python/平台/CPU 数和参数，原始 stdout/stderr 可检查。
Each run checks duplicate groups against the generated SHA-256 content oracle, ignoring local group ids/order. Raw stdout digest, exit code, process wall time, all numeric stderr metrics, executable hashes, environment, parameters, and raw logs are retained.

分析时按 fresh/warm 分开，报告各试次和中位数；`scan_work_ms` 混合遍历、元数据、数据库，不应直接称为纯元数据耗时。`hash_work_ms` 是线程求和，不能与墙钟简单相加。计数和 oracle 必须先正确，才比较性能。
Separate fresh/warm results and report trial values/medians. `scan_work_ms` includes traversal, metadata, and database work; summed worker time is not additive to wall time. Validate counts and oracle before interpreting speedups.

推荐分别测试 CPU 和 CUDA，并扩大至 20,000/200,000 文件验证规模效应；默认试验仅定位与回归，不证明整盘性能。CUDA 比较应检查 `gpu_workers`/`cpu_fallbacks`，记录驱动和 GPU 型号。
Test CPU and CUDA separately, then scale to 20,000/200,000 files. Small defaults support diagnosis/regression, not whole-drive claims. Check GPU/fallback counts and record GPU/driver for CUDA comparisons.
