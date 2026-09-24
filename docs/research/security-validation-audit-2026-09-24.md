# Security and validation audit — 2026-09-24

## Scope and decision boundary

Reviewed production checks in `src/application.cpp`, `src/run_lock.cpp`,
`src/workspace.cpp`, `src/config.cpp`, `src/model_store.cpp`,
`src/telemetry.cpp`, and `src/store.cpp`, plus relevant integration tests and
`docs/design.md`. No production code was changed. This is a focused review of
redundant or weakly justified checks, not a penetration test.

`same` scans a user workspace as the current user. Its documented boundary is
**not** a security sandbox for an adversary-writable tree or hostile mutation of
`.same` (`docs/design.md`, section 6). The meaningful local threat is an
*already present* link at a state pathname accidentally redirecting a write;
an active process able to rewrite the state directory is outside the supported
model. Checks needed for file-version correctness and destructive `clean`
operations have different purposes and should not be removed in this exercise.

## Actionable removals

### 1. Remove the model-database preflight in `run` (high confidence)

**Location:** `src/application.cpp:1527-1536` checks `model.db` and its three
sidecars immediately before constructing `ModelStore`; `src/model_store.cpp:121-128`
checks the same four paths again. `ModelStore::guard_path` also rejects multiply
hard-linked files, so it is the stronger, owning check. Both run inside the same
`try` in `run`; a rejection disables optional learning rather than the scan.

**Why this is duplication, not added security:** A check-then-check-then-open
sequence does not bind the name to an object. A hostile replacement between
preflight and `sqlite3_open_v2` remains possible. The earlier check adds four
filesystem metadata probes and a second policy implementation, but no stronger
protection against the only supported threat (preexisting unsafe paths), which
the owner's check already handles. `ModelStore` also requests
`SQLITE_OPEN_NOFOLLOW` where available (`src/model_store.cpp:131-135`).

**Minimal correction:** delete only the four-name preflight loop in `run` and
leave `ModelStore::guard_path` intact. Do not weaken the owner's sidecar or
hard-link checks as part of this cleanup. One incidental behavior changes:
without a usable CPU device identity, the optional model store is not opened,
so an unsafe but unused model path will no longer emit an optional-learning
warning. Main scan/output behavior should remain unchanged; verify that the
warning is not documented as an external contract.

**Tests:** existing model symlink, sidecar symlink, and hard-link rejection tests
must still pass through `ModelStore`; add or retain an end-to-end `--pgo` scan
with a preexisting linked `model.db` showing unchanged duplicate output and
disabled learning. If warning text is asserted anywhere, update it deliberately.

### 2. Remove `run.lock` from `prepare_state`'s child-path loop (high confidence)

**Location:** `src/application.cpp:168-173` preflights `run.lock`; the very next
operation is `RunLock lock(...)` at `src/application.cpp:1456`. The owning
constructor opens the final component without following it (`O_NOFOLLOW` on
POSIX; `FILE_FLAG_OPEN_REPARSE_POINT` and post-open attribute/type inspection
on Windows), and rejects non-regular handles in `src/run_lock.cpp:43-91`.

**Why this is duplication:** the preflight does not hold a handle or prevent a
replacement; the lock constructor does the actual name-to-object validation.
Removing this one entry leaves the no-follow behavior and the persistent lock
identity invariant intact. It also removes a redundant metadata operation on
every scan. The error wording may change for a linked/special lock file; the
failure itself must not.

**Minimal correction:** keep `prepare_state`'s `.same` directory and database
checks, but omit only `run.lock` from the list. Do **not** remove `RunLock`'s
open flags, handle checks, or `WorkspaceLock`: the latter protects the separate
`clean` lifecycle race on POSIX.

**Tests:** `tests/run_lock_tests.cpp` already exercises symlink rejection and
non-truncation; `tests/integration_tests.cpp` exercises state links. Add/retain
an end-to-end scan with linked `run.lock` and assert nonzero exit and unchanged
target bytes. Test lock contention and POSIX `new`/`clean` lifecycle separately.

## Optional cleanup, not a blocking finding

`src/config.cpp:15-33` performs `file_size(path) > limit` before a read capped to
`limit + 1` bytes and then checks `gcount() > limit`. The second check is the
authoritative byte bound. The first is a cold-path early rejection of a known
oversized file, not security against a racing writer. It may be removed if
simplifying `read_settings`, but it is not a substantial quality or performance
issue. If removed, preserve the `limit + 1` read and existing missing/unreadable
file semantics; test exactly-at-limit, one-byte-over, and files growing between
metadata inspection and read. Do not conflate this with `Config::validate()` in
`Resources`: `Config` can be constructed by library callers and CLI overrides
occur after `Config::load`, so the resource-boundary validation is not redundant.

## Checks to preserve and limits not to overclaim

- `prepare_state`'s preexisting state DB/sidecar link rejection
  (`src/application.cpp:168-173`) serves a real compatibility and safety
  contract tested in `tests/integration_tests.cpp:439-469`; `Store` opens SQLite
  without `SQLITE_OPEN_NOFOLLOW` (`src/store.cpp:194-196`). Do not remove this
  preflight merely because it is non-atomic. Likewise preserve `.same` directory
  validation and the capability-anchored destructive cleaner.
- `src/application.cpp:52-55`, `:76-114`, and `:119-139` recheck open handles
  and path bindings around hash/compare to detect ordinary concurrent edits.
  They are correctness checks, not redundant security theater, though they do
  not provide snapshot isolation. Removing them risks reporting stale groups.
- The repeated-looking model and telemetry `guard_path` functions
  (`src/model_store.cpp:21-36`, `src/telemetry.cpp:30-49`) have the same policy but
  independent failure semantics. A shared internal helper could consolidate
  them later; do not simply delete either copy without moving validation to
  the owning SQLite-open boundary.

## Evidence and external context

This audit is based on source tracing, not executed tests. SQLite documents
[`SQLITE_OPEN_NOFOLLOW`](https://www.sqlite.org/c3ref/open.html) as disallowing a
database filename containing symbolic links. The Linux
[`open(2)` manual](https://man7.org/linux/man-pages/man2/open.2.html) states that
`O_NOFOLLOW` rejects the final component, unlike a prior pathname check. The
peer-reviewed [TOCTTOU filesystem model](https://www.sciencedirect.com/science/article/pii/S0167404810000830)
explains why separate check/use operations cannot establish lasting object
identity. These sources support the *mechanism*, not a claim that this project's
trusted-workspace assumption has been violated. A future requirement to safely
scan hostile writable trees would need a separate threat model and handle- or
directory-capability-based design, not more preflight pathname checks.
