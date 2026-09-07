# Real-file dispatch benchmark / 真实文件分派基准

`tools/file_dispatch_benchmark.py` creates a **new** output directory containing eight 64 MiB files (512 MiB total), four deterministic equal-content pairs. It generates through a 1 MiB buffer, never keeps the whole dataset in RAM, never scans user data and does not delete old output directories. All configurations reuse the exact same root and filenames.

默认矩阵为 cpu/auto/cuda × workers 1/4 × block 1/16 MiB，共 12 配置。每配置先预热一次，然后三轮测量；第二轮反转完整矩阵顺序。使用 `--rehash`，不复用应用摘要缓存，但文件刚生成且经过预热，**这是系统缓存热态，不是冷盘吞吐量实验**。不自动清缓存或改变系统设置。

```powershell
python .\tools\file_dispatch_benchmark.py --exe .\build\cuda\same.exe --output .\build\file-dispatch-run
# Full concurrency matrix / 完整并发矩阵
python .\tools\file_dispatch_benchmark.py --exe .\build\cuda\same.exe --output .\build\file-dispatch-full --workers 1,4,8,20
```

Host/device budgets track configured workers and block size with fixed overhead; they are explicit allocation budgets, not total process/driver memory limits. Metadata workers stay one, queue capacity 32. `gpu_min_bytes` is intentionally omitted so the executable's current default policy is tested and the executable SHA-256 is recorded. Explicit CUDA can still fall back; inspect raw backend profile rather than assuming mode names establish actual execution.

Each run stores stdout/stderr, raw profile text, parsed numeric metrics, total subprocess time and SHA-256 output checksums in `report.json`. The canonical duplicate groups must match the generation oracle, with scanned=8, hashed=8, cached=0. Raw stdout order/group numbering is not assumed stable; canonical-group checksum is the comparable checksum. Errors stop the experiment and preserve logs.

This corpus contains eight tasks only: workers 20 tests oversubscription/initialization, not twenty concurrently active hash jobs. It validates a real-file case but cannot establish steady-state throughput for larger file counts. Run the compute concurrency matrix separately. Process time includes initialization/calibration, file scanning, comparison and output; compare Profile stages as well as end-to-end time. Execution should not overlap other performance sampling.
