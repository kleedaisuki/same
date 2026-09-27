# Explicit three-backend training: design and validation (2026-09-27)

## Decision and contract

`same train <dirs> [options]` separates the cost of obtaining CPU/CUDA/iGPU labels from ordinary automatic scans, **not** the training model from the serving model. Zero positional directories default to the current working directory; explicit directories are read-only, while the current working directory owns `.same/model.db` and the run lock. Subsequent automatic scans launched in that same working directory load and continue updating the same model. The legacy `--corpus=DIR` syntax remains an alias for adding a corpus directory. All directories share one selected-file and logical-byte budget; overlapping roots are rejected. The existing scan cache `state.db`, duplicate groups, and input contents are not written by training. Training is opt-in and forces all three backends for every selected nonempty file, even below the normal GPU eligibility floor, but requires at least one selected file at or above that floor. A missing/failing backend or digest mismatch fails the run without publishing its training deltas. The model ledger still uses per-key atomic replacement rather than one transaction across all three keys; a storage failure during final persistence can leave a subset of keys updated, and is reported as failure.

Metadata selection walks the entire corpus using the existing link-rejecting, ignore-aware `ParallelWalk`. It retains up to 64 deterministic path-ranked candidates per factor-four size band, then selects round-robin across bands under the configured global file/byte ceilings (defaults 128 files and 2 GiB logical corpus bytes; maximum 4096 files). The metadata walk is not bounded by the selected-file count. Each selected file runs through the automatic mode's actual pool budgets and device profiles rather than the forced-scan mode's potentially different batch capacity. Three backend requests are submitted together, rotating order between files; the main thread waits for all complete digests, the resource pool's full idle state, and their equality before the next file. The training reader is reopened before timing so the service label covers the same version checks, full-file read, hash and finish as the scan's metadata-handle reuse path. Startup/discovery costs remain separate from service labels. The training model is then merged by the existing device/CPU identity key and worker count. Training intent is passed through a new call overload, preserving the old `Worker` layout and existing three-argument selection symbol.

This is a supervised calibration experiment on selected actions, not evidence of an end-to-end speedup. Three reads of the same file alter OS cache state; rotating order reduces systematic first-position bias but does not remove storage/cache or thermal confounding. A backend may implement some tiny payload work on the host; a valid backend service sample is not proof that a GPU kernel executed for that payload. Few eligible files, narrow sizes, and low contention can leave a rank-deficient or out-of-domain model. The command warns when fewer than four selected files satisfy GPU eligibility. Real scans continue to supply online updates, but a large training corpus can initially outweigh a few real observations because its sufficient statistics are persisted at full sample weight. This requires held-out scan validation; the train command does not certify model quality. The original file hash oracle is the CPU backend plus full cross-backend digest equality, not an independent cryptographic BLAKE3 proof.

## Reproduction and observations

Source base was `331164a` plus the uncommitted train, telemetry, and report implementation. Windows x64, Visual Studio 2022 MSVC 19.44, CUDA 12.8 and dynamically loaded OpenCL were used for the physical smoke. Training-validation Release executable SHA-256: `C4722A2EDD3A8F93532D79B4B560F3AC0E432D645B1C71FF9EA9F3B42AAA03AD`; a later help-presentation-only rebuild has SHA-256 `6C662AC40FCC56486969E3BE3035644C5C059B2D38A41FEE163A2C59ADDF96D6`. CMake dependencies came from repository `.cache` source checkouts; no external user directory was read. This is a correctness/lifecycle smoke, **not** a benchmark: no controlled repetitions, ordering randomization, power/frequency controls or cold-cache reset were performed.

Isolated fixture beneath repository `.temp`: workspace and separate corpus; four ordinary files of 4 KiB, 64 KiB, 1 MiB and 16 MiB, generated with a fixed PRNG seed 42. Workspace config: workers 2, metadata workers 2, CPU block 1 MiB, host budget 128 MiB, device budget 64 MiB, queue capacity 4, `gpu_min_bytes` 1 MiB, backend auto. Invocation from the workspace:

```powershell
same train --corpus=../corpus --max-files=4 --max-bytes=33554432
```

Observed: exit 0; 4 accepted samples each for CPU, CUDA and iGPU; 17,895,424 logical input bytes; three model keys saved. Repeated training and an intervening ordinary auto scan preserved the three-key success. The final-source physical smoke reported CUDA setup sum 108.133 ms, iGPU discovery 121.603 ms and iGPU setup 325.956 ms. These are separate diagnostics, not additive wall times or stable performance estimates. The input corpus SHA-256 checked before/after remained unchanged; a fresh training workspace had no `state.db`. After a normal `same scan --rehash --summary --no-telemetry` on a copied 16 MiB input, the scan reported two CPU prior hits for its two workers and no invalid model state. Later training left the scan's `state.db` SHA-256 unchanged. This proves matching CPU-state reuse, not that the GPU prior was selected by that scan. The fixture is intentionally too small for a model-quality or throughput conclusion and emitted the sparse-eligible warning.

The final successful physical run also verified the telemetry SQLite database directly: `latest_run` had `(command=train,status=completed,accepted=12,persisted=12,dropped=0,errors=0)`. The spans contained one successful `train.select`, `train.initialize`, `train.measure`, `train.persist`, `train.output` and `train.run`, plus four `train.backend_attempt` events per backend. The terminal log was `train.completed`. Final metrics included `train.selected_files=4`, `train.verified_files=4`, `train.model_saved_keys=3` and `train.read_bytes=53,686,272`, exactly three times the selected logical bytes. Stored pre-drain `train.elapsed_ms=495.7609`; stderr separately reported `telemetry_drain_ms=4.8364` and `total_including_telemetry_ms=500.697`. These are one smoke run, not performance evidence.

The instrumentation uses the existing local SQLite writer, not an OpenTelemetry exporter and not a claim of OpenTelemetry schema compliance. Its run/phase/attempt hierarchy follows the useful distinction between root spans, child operation spans, terminal metrics and logs in the [OpenTelemetry tracing API](https://opentelemetry.io/docs/specs/otel/trace/api/) and [semantic-convention overview](https://opentelemetry.io/docs/concepts/semantic-conventions/). Per-attempt spans are bounded by the configured queue/event cap; terminal aggregate metrics and explicit dropped/error counters remain the reliable source for completeness. This trade-off is also motivated by research on information-preserving trace sampling, such as [STEAM (ESEC/FSE 2023)](https://www.microsoft.com/en-us/research/publication/steam-observability-preserving-trace-sampling-2/), but no adaptive sampling algorithm was adopted here: this is a local, small-batch command, and the immediate requirement is to make loss observable rather than add sampling complexity.

A CPU-only MSVC Debug build produced `training backend unavailable: cuda`, exit 2, no `state.db`, unchanged corpus length and an empty newly created model database. Hardware-free `training_tests` asserts zero model rows after failure, a finalized `failed` telemetry run, `train.measure` error span, `train.failed` log and complete `train.model_saved_keys=0` metric. Its disabled-telemetry variant creates no telemetry database. `igpu_routing_tests` injects fake CUDA/iGPU backends and verifies forced training ignores the size and cold-credit gates while keeping backend labels distinct. The final focused CTest batch passed 9/9: telemetry, training, iGPU routing, online resources, adaptive routing, hash retry, unified CUDA, scan integration and learning integration. The test temporary directory was redirected beneath repository `.temp`.

The default-corpus path was exercised separately: a fresh workspace beneath `.temp` held one ordinary 1 MiB input with `gpu_min_bytes=1 MiB`; `same train --max-files=1 --max-bytes=2097152` returned exit 0, one accepted sample for each backend, three saved model keys and no `state.db`. This verifies the requested current-working-directory default, not adequate one-sample model quality; the sparse-eligible warning was emitted.

On that same isolated workspace, `same train --max-files=1 --max-bytes=2097152 --no-telemetry` also succeeded with one accepted sample per backend and three model keys while `telemetry.db` remained absent. This confirms that the switch disables diagnostics, not model training.

## Training report validation

A final physical quiet-output check used a fresh repository-local `.temp/train-silent-report` workspace with four GPU-eligible files. Plain `same train` exited 0 with zero-byte stdout and stderr logs; `same train --summary --format=pretty --color=never` exited 0 with zero-byte stdout and a nonempty sectioned stderr report. Both runs used real CPU/CUDA/iGPU backends and persisted the three model keys. The three directly affected Debug tests (`training`, `integration`, `telemetry`) passed after the quiet-output change. The build kept the pre-existing MSVC LNK4098 default-library warning; no link failure occurred.

The new `same train --summary --format=pretty --color=never` was run with the real CPU/CUDA/iGPU fixture, exiting 0. After the quiet-output change it emits no stdout result line and a sectioned stderr report using the scan report's existing `PrettyReport`, `human_bytes`, `human_duration`, `render_learning`, and `render_telemetry` helpers. It reported 4 selected/verified files, 4 samples for each backend, 17.07 MiB logical corpus input, 51.20 MiB logical reads across backends, phase times, setup costs, model load/save health, and 12/12 persisted attempt spans. The fixture only had 2 GPU-eligible files, correctly warning that coverage is weak. `--summary --format=tsv --no-telemetry` also exited 0 and emitted raw `train_*` and `model_*` key-value lines followed by `telemetry_enabled=0`; training still saved all three model keys. The report's prediction checks are online residual diagnostics, not held-out policy validation. The old C++ `train` entry point remains available for linkage; successful training is now quiet without presentation options, while warnings and failures remain visible. Following the implementation, the 9-test focused Debug CTest batch passed before a final wording-only report row was added; that row was compiled in the final Release build.

## Multi-directory and merge smoke (2026-09-27)

After the provenance-ledger and positional-directory changes, the final Release
Windows CUDA executable had SHA-256
`91A0502BAB930C28EEB88AEDCCD71E8234896C0FD88346E19A08F96A8D79D34A`.
It was built using Visual Studio 2022 MSVC 19.44 and CUDA 12.8. The linker still
emitted the pre-existing LNK4098 default-library warning; linking succeeded.

An isolated `.temp/train-multi-final` workspace trained over two repository-local
corpus directories, with three GPU-eligible files totaling 22 MiB logical bytes.
`same train <dir1> <dir2> --max-files=3 --max-bytes=33554432 --summary
--format=pretty --color=never` exited 0, wrote no stdout, verified all three
files, recorded three CPU, three CUDA, and three iGPU samples, saved three model
keys, and persisted all 9 accepted trace records with zero drops/errors. The
measured run reported 543.07 ms before telemetry drain and 546.74 ms including
drain, 95.61 ms summed CUDA setup, 123.69 ms iGPU discovery, and 345.58 ms iGPU
activation. Only three eligible files were selected, so it correctly warned
about sparse coverage; these single-run times are not performance estimates.

A fresh `.temp/merge-final` destination then ran `same merge
../train-multi-final --summary --format=pretty --color=never` using that same
Release binary. It exited 0 with no stdout and reported one workspace imported,
three model contributions and two setup contributions considered, one telemetry
run and 16 event rows newly imported. This source had no scan state cache or
ignore rules, so both relevant import counts were zero. Debug regression tests
separately exercise those paths. The related Debug CTest batch passed 8/8 after
the main merge implementation; the schema-2 migration and first-copy telemetry
recovery changes were each verified by their focused tests afterward. Finally,
the complete CPU-only MSVC Debug CTest suite passed 27/27 in 8.80 seconds, with
`TEMP` and `TMP` redirected beneath repository `.temp`.

## Follow-up evidence needed

- More than four eligible files over several size and batch regimes; report pre-update prediction errors and out-of-domain rates separately for all three backends.
- Controlled warm/cold-cache and backend-order crossover. A shared filesystem may be I/O-bound enough that GPU compute advantage is irrelevant.
- Compare CPU/no-PGO, auto before training, auto after training, and forced-device controls with identical input and `--rehash`. Preserve full output correctness and setup-inclusive process times.
- Verify deployment identity changes (driver, binary implementation, worker count or capacity) invalidate the prior, while an identical configuration loads all relevant backends once they are initialized.

External engineering anchor: StarPU's official performance-model tutorial describes continual calibration and invalidation of stale kernel models, but does not establish this scanner's performance: https://starpu.gitlabpages.inria.fr/tutorials/2021-02-EoCoE/ . Scientific framing: selected-device-only labels are partial-feedback data, so calibration coverage must not be confused with unbiased counterfactual policy evaluation (Li et al., 2010): https://arxiv.org/abs/1003.5956 .
