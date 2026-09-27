# Per-worker iGPU concurrency design (2026-09-27)

## Problem and evidence

The v0.6.0 scheduler has N fixed content workers, each with private CPU and CUDA
execution state, but only one pool-wide `igpu_compute_`, one pair of host buffers,
and an `igpu_busy_` admission flag. The flag remains held for an entire iGPU file
task, so no second worker can dispatch iGPU work even when its own CPU/CUDA state
is idle. This is a software ownership restriction, not an OpenCL requirement.
The OpenCL backend already shares a compiled program/context while allocating
private queues, kernel objects and buffers for each `Device` instance.

The [Khronos OpenCL specification](https://registry.khronos.org/OpenCL/specs/unified/html/OpenCL_API.html)
permits multiple queues in one context and says their commands are independent,
but does not promise physically parallel work-group execution. In particular,
the implementation may serialize work-groups. Concurrent CPU/GPU work can also
contend for a shared memory interface; the [SC '25 shared-memory platform study](https://doi.org/10.1145/3731599.3767497)
is a warning about mechanisms, not a performance estimate for the user's iGPU.

## Required invariants

1. Every eligible content worker owns a private iGPU compute instance and model
   state. It reuses its own CPU file-I/O buffers; no second host input pair is
   needed because the OpenCL backend copies from them synchronously. Discovery/program
   compilation may remain one-time/shared, but full file tasks must not hold a
   pool-wide iGPU lock. The first driver startup remains controlled by the cold
   admission policy; later per-worker activation is independent.
2. Aggregate host and device allocation budgets must account for **all** worker
   iGPU instances, not multiply a one-instance reservation by N. Host budgeting
   adds leaf-CV scratch, not duplicate CPU input buffers. Actual batch
   capacity remains in the device identity used for persistent model loading.
3. A recoverable compute failure retires only its owning worker's iGPU instance;
   a global discovery failure can make the backend unavailable everywhere.
4. Runtime context includes actual in-flight CPU/iGPU contention at selection.
   Fitting also incorporates a later-overlap epoch and exact peer count at task
   start; prediction-error diagnostics retain the original decision-time estimate.
   Existing CUDA peak/contention keys retain their meaning; new iGPU peak and
   contended-sample keys are separate. Neither proves simultaneous hardware
   kernel work.
5. Forced `--igpu`, explicit training, telemetry, exact digest verification,
   fallback correctness, and legacy output/CLI contracts remain intact.

## Verification plan

- Hardware-free two-worker gate test: hold one iGPU task and prove another
  worker can independently select iGPU and complete before the first releases.
- Inject per-worker failures and verify no peer retirement or mislabeled model
  sample. Verify fixed-budget allocations and startup/probe behavior.
- Full CPU-only Windows Debug suite; Linux/macOS CI and PoCL kernel tests after
  push; physical iGPU/CUDA test with full-file digest equivalence.
- Compare 1/2/4 concurrent iGPU instances on matched large-file corpora,
  reporting setup-inclusive wall time, total read bytes, physical device overlap
  if measurable, CPU throughput, error/fallback counts and peak admitted work.
  More queues are not assumed to improve whole-scan throughput.

## Implementation state

The pool-wide iGPU file-task gate was removed. Each worker now lazily owns a
private OpenCL compute instance and model contribution, with aggregate memory
budget divided across all configured workers. CPU/iGPU share owner input buffers
instead of allocating another pair. Initial automatic discovery and cold setup
remain nonblocking for unrelated work; explicit training waits for shared probe
completion and never downgrades a requested backend merely because a peer is
initializing. Setup history is aggregated from owner-local measurements. The
reported iGPU peak counts admitted file tasks, **not** simultaneous hardware
kernel execution. Hardware-free two-worker overlap, per-owner failure isolation,
all-three-backend learning, bounded 12-worker quotas, and 100 repeated iGPU
routing tests pass locally.

The model now uses an exact atomic CPU+iGPU active count for selection features
instead of summing two separately loaded counters. On task completion, a separate
training context takes the maximum of selected context, exact start peers, and a
binary indicator that another task overlapped later. The online error diagnostic
uses the decision-time prediction, so it does not benefit from information that
became available only after dispatch. This remains a coarse contention proxy;
overlap duration and DRAM traffic are not observed.

## Local validation evidence

Windows x64 MSVC 19.51 CPU-only Debug CTest passed 27/27 after the
metric-compatibility and actual-contention edits. The routing test repeated 100
times without a failure after its synchronization assertions were stabilized.
An additional model test verifies that error uses the decision-time prediction
while fitting uses the observed contention feature.

Physical smoke uses Windows 11, MSVC 19.44, CUDA 12.8, the production OpenCL
backend, three 32 MiB distinct files and an isolated `.temp` workspace. With
three workers, 1 MiB file-I/O blocks, 128 MiB host budget and 64 MiB device
budget, `scan --igpu --rehash --summary --format=tsv --no-telemetry` exited 0:
all three file hashes ran on iGPU, none on CPU/CUDA, zero fallback, and
`igpu_peak_concurrency=3`. The executable SHA-256 for that differential run
was `152821C58D6BB15061D801260C140B8F4B9AA9EDB182305124CD04AECA8E80F3`.
Rerunning the same three inputs with `--cpu --rehash` yielded identical three
stored BLAKE3 digests (queried from `state.db`). This demonstrates concurrent
file-task admission and digest correctness, **not physical kernel overlap**.

That forced-iGPU run took 498.139 ms including 110.660 ms discovery and
991.839 ms **summed-worker** setup; the paired CPU run took 33.528 ms. This is
not a claim of GPU acceleration: CPU is clearly preferable for this small
three-file, cold-start workload. A separate `train` over those files completed
with three samples per backend, three saved model keys, and six prior hits;
the next automatic scan selected CPU for all three, with two cold accelerator
starts deferred. This is a desirable decision under the observed setup costs,
not evidence that the devices cannot run concurrently once admitted.

An exploratory local CPU-only rehash of 5,000 distinct 4 KiB files used the
v0.6.0 release archive as baseline and an in-progress version of this branch
as candidate, with order old/new/new/old/old/new/new/old on the same warm
workspace. Median elapsed times were 576.162 ms and 588.274 ms respectively
(+2.1% candidate). The sample is too small and uncontrolled to establish a
regression. After the contention-epoch and compatibility edits, a second
eight-run interleaving on the same workload (new/old/old/new/new/old/old/new)
yielded medians of 578.231 ms candidate and 590.032 ms baseline. The sign
reversed between the two exploratory batches; this is evidence of noise, not
a speedup claim. Preserve both observations and repeat representative
benchmarks before making any broad performance claim.

The final local production-backend rebuild again produced matching digests for
all three 32 MiB files with iGPU and `igpu_peak_concurrency=3`; SHA-256 of the
executable was `26297DDAAEB6A366BB0E370D192155A02E12CEE0A84E414634F2509F9F325E2F`.
After this, a three-backend `train` completed with three samples per backend,
eight prior hits, and three saved keys. A forced CUDA scan hashed all three files
on three CUDA workers, with `gpu_peak_concurrency=3` and digests identical to
the iGPU/CPU results. This confirms both accelerator backends remain usable
and isolated on the same corpus. The current most-recent build may have a
different SHA-256 because metric and model diagnostics changed after this
smoke; the final validation report must name its own binary identity.
