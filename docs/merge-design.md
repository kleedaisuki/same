# Workspace merge design (2026-09-27)

## Decision and invariants

The user requested `same merge [dir] [-r] [--move] [--summary]` to combine telemetry,
online learning, content-cache knowledge, and missing ignore rules into the current
workspace while preserving its configuration. Recursive mode discovers descendant
`.same` workspaces without following links. The default source directory is `.`;
the current workspace itself is never imported into itself. `--move` removes source
workspace state only after every requested import succeeds and is verified.

Clarifications: cache keys should be absolute paths, so unrelated directories can
contribute reusable entries; ordinary scan results must still contain **only files in
the current scan scope**. A source that is trained again and merged again must
**replace its previous model contribution**, not add the entire cumulative state a
second time. These choices supersede an earlier proposed relative-prefix mapping.

### State cache

`state.db` schema v1 stored root-relative paths in `files` and deleted unvisited
records at `end_scan`. Changing this table in place to an all-workspace archive would
make duplicate output include external paths and would break callers. Schema v2
adds an `absolute_cache` table keyed by canonical absolute file path; it keeps
`files` as the current-scan projection. Merge reads source relative records and
absolute archive rows, converts relative records using the source root, and imports
them even if files are temporarily absent. Scan checks the complete current
`FileStamp` before reusing an absolute entry on an active-cache miss, then materializes a relative
record in the current scan; comparison candidates remain active-scope only.
The file stamp is a conservative cache key, **not** proof against an adversary who
changes content while preserving all metadata. No imported digest may bypass the
existing bytewise duplicate verification.

### Online model

The original `models` table stored already-aggregated, run-decayed sufficient
statistics. Blindly adding two such rows is not idempotent, and choosing the row
with more samples discards independent evidence. Schema v4 adds a provenance ledger
whose rows are keyed by stable workspace identity and model identity key. Keep the
existing `models` table as a materialized aggregate for current readers. Merge
replaces a source's ledger row and recomputes the aggregate. Local scan/train
updates must also update the local contribution, rather than writing an opaque
aggregate that would be impossible to decompose on later source replacement.
Run-boundary decay must be applied consistently to every contributing component
for any key touched by a local run. Setup histories need corresponding provenance
and recomputation. Migrating a v2 database treats its existing aggregate as one
local contribution. Each workspace instance has a random identity in addition to
its canonical path. A copied database retains imported contributions, but when
opened at a new root it forks its local identity. Recreating `.same` at the same
path after `--move` also gets a new identity, preventing stale revision collisions.
The ledger is bounded by the existing 32 model identities, which can evict old
device/configuration keys; this is not an unlimited cross-machine repository.

### Telemetry, ignore, and deletion

Telemetry import is by immutable run ID, with related events, metrics and
parameters copied together; a repeated merge must not duplicate runs. Existing
writer retention may later remove old imported runs according to the destination
configuration, so the merge report must not promise indefinite archival.
Missing destination ignore rules are appended one by one. Descendant-source rules
are rewritten as target-root-anchored patterns under the source relative path;
external-source rules cannot be scoped and therefore apply at the destination root
with an explicit warning. Existing target rules keep their order, new rules are
appended in source order, and a repeated
merge does not append exact duplicate translated lines. Configuration remains untouched.
Destination `.same/config.toml` is never copied or overwritten. Import operations
must be idempotent across partial failures. Source deletion is deferred until all
database connections are closed and the complete import succeeds; any inability
to prove safe deletion leaves sources intact and reports failure. The source-state
snapshot checks direct child names, identity, timestamps, size, and BLAKE3 contents
while under a workspace lock. A non-cooperating external process can still race a
filesystem operation; `--move` is not an adversarial-filesystem security guarantee.
With multiple sources, deletion is not globally atomic: a later failure can leave
earlier sources removed, but the imported data remains in the destination.

SQLite transactions are per database here. WAL-mode transactions across attached
databases do not provide all-database crash atomicity, so imports are independently
transactional and designed to be retryable. The telemetry first-copy path uses the
[SQLite backup API](https://www.sqlite.org/backup.html); the cross-database limit is
documented in [SQLite ATTACH](https://www.sqlite.org/lang_attach.html). The
[SQLite WAL documentation](https://sqlite.org/wal.html) explains the associated
journal-mode behavior. This is an engineering constraint, not a claim that a full
merge is one transaction.

## Validation evidence and remaining limits

Debug tests cover multi-root selection and overlap rejection, model remerge and
continued training, copied and recreated workspace identities, telemetry run-ID
deduplication, scoped ignore rules, destination config preservation, active-scan
result isolation, actual absolute-cache reuse without rehash in a parent scan,
recursive move, active source locks, and same-size modification of source state
before cleanup. A physical CUDA/iGPU multi-root training run was also performed
on three selected files; see `docs/train-mode-validation.md` for
hardware-specific details. These tests do not establish general scan speedup.
Experiments and merge fixtures are kept under repository `.temp`/`.cache`.
