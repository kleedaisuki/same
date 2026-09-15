# Human-readable summary layout

## Scope and compatibility contract

This note records the presentation contract for `same scan --summary`. The summary is a
diagnostic view of one completed run, written to `stderr`; duplicate results remain on `stdout`.
The change is intentionally confined to the existing human-readable (`pretty`) renderer.

The following interfaces are compatibility boundaries and must not change as part of visual work:

- TSV result rows, including reserved unique-file group `0`.
- Raw `key=value` diagnostic names, values, precision, and line structure.
- Exit codes, warning visibility, stream selection, and `--summary` opt-in behavior.
- Independent format and color policies for `stdout` and `stderr`.

## Information architecture

The human report uses one stable hierarchy rather than one long flat table:

1. **Results** — duplicate groups, matching files, and unique files.
2. **Storage and I/O** — state database, scan/cache counts, logical data, and reads.
3. **Performance** — end-to-end time first, followed by the complete phase and overlapping-work
   breakdown and logical throughput.
4. **Compute and routing** — accelerator state, cold-start accounting, routing attempts, scheduler,
   and GPU input size.
5. **Online routing model** — samples, coverage, residual/error evidence, scope, and latency buckets.
6. **Workers** — aggregate availability/reads and every per-worker contribution.
7. **Model persistence** — enablement, database, disabled reason, load/save/setup counters, cold-path
   I/O, and decay.
8. **Telemetry** — database/run identity, writer health, queue pressure, drain, and total time.

This ordering puts the decision result before implementation detail, while retaining all fields
from the previous pretty renderer. No automatic collapsing or terminal-width truncation is used:
`--summary` is an explicit request for the full current-run evidence.

## Color semantics and accessibility

Color is a redundant channel; brackets, section names, labels, ordering, and status words carry the
same meaning in plain text. The renderer uses the portable ANSI 16-color palette instead of RGB so
terminal themes remain responsible for exact colors:

| Role | ANSI treatment | Meaning |
|---|---|---|
| Page title | bold cyan | report identity |
| Section title | bold blue | structural boundary |
| Good | bold green | completed, available, or error-free state |
| Information | cyan | quantities and timings |
| Warning | bold yellow | hidden, disabled, dropped, retried, or degraded state |
| Danger | bold red | recorded errors |
| Accent | magenta | routing/model evidence without success/failure semantics |
| Muted | dim/default | explanatory context and inactive state |

Automatic color remains per-stream and TTY-aware, honors nonempty `NO_COLOR` and `TERM=dumb`, and
falls back to plain text when Windows virtual-terminal processing is unavailable. Explicit
`--color=always` and `--color=never` retain their established override semantics.

## Evidence and engineering judgment

The [Command Line Interface Guidelines](https://clig.dev/) recommend human-first terminal output,
separate TTY detection for each stream, predictable machine-readable output, and disabling color
under redirection. The [NO_COLOR convention](https://no-color.org/) defines the environment opt-out.
The W3C's [WCAG2ICT command-line guidance](https://www.w3.org/TR/wcag2ict-22/#text-command-line-terminal-applications-and-interfaces)
emphasizes that terminal interfaces must remain usable through textual and assistive-technology
paths. These are stronger production signals for this local CLI than adopting a rich terminal UI
framework.

Visualization research generally supports redundant visual encodings rather than color-only
categories; for example, Zeileis et al.,
[colorspace: A Toolbox for Manipulating and Assessing Colors and Palettes](https://doi.org/10.18637/jss.v096.i01),
discuss perceptually grounded palette design. The direct transfer to terminal text is limited:
terminal themes remap colors and the report is not a chart. Therefore this design makes structure
and literal status words authoritative and uses color only to accelerate scanning.

## Validation

Tests should establish all three layers independently:

1. Raw/TSV compatibility: established keys and result records remain parseable and unchanged.
2. Plain pretty structure: all eight sections and representative fields are present without ANSI.
3. Colored pretty structure: every semantic color role is exercised, escape sequences are reset,
   and stripping ANSI leaves the same labels and values.

Visual checks should cover a normal run, hidden unique files, disabled PGO/telemetry, a warm cache,
and a writer error. Timing values are nondeterministic and must be validated by names, units, and
accounting invariants rather than golden snapshots.
