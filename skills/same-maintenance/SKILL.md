---
name: same-maintenance
description: Maintain or refactor the same C++ duplicate-file scanner while preserving its CLI, UI, persisted state, and release behavior. Use for repository-wide maintenance, build or test governance, and compatibility-sensitive internal changes; not for unrelated C++ projects.
---

# Maintain same

Keep this skill as a routing aid, not a second source of product requirements. Read the relevant existing contract before changing code: [development](../../docs/development.md) for module entry points and checks, [design](../../docs/design.md) for architecture, [build](../../docs/build.md) for toolchain selection, and [releasing](../../docs/releasing.md) for package gates. Consult focused `docs/` notes only for the subsystem being changed.

## Change boundary

- Preserve visible CLI behavior, output formats, exit codes, configuration defaults, and terminal UI unless the task explicitly changes them. The [README](../../README.md) is the user-facing contract; add characterization tests before moving ambiguous behavior.
- Preserve existing `.same` state and telemetry compatibility. A schema or serialized-model change needs explicit migration/rejection behavior, tests for old data, and updated documentation; do not silently reinterpret stored values.
- Consolidate policy at its owner rather than adding caller-specific exceptions. Remove a validation only after identifying its threat model, its actual caller and invariant, and regression coverage for the retained boundary.
- Update bilingual API comments for changed semantics and update the closest durable design note when an architectural decision would otherwise be lost.

## Evidence and artifacts

- Put generated experiments, fixture databases, and test outputs under repository-root `.cache/` or `.temp/`, never outside the project. Do not commit them.
- Run focused tests first; use CTest for native changes and `python -m unittest discover -s scripts/telemetry -p test_merge.py -v` for the standalone archive tool. Use the relevant cross-platform GitHub Actions job as release evidence; a local pass or workflow definition is not proof that CI passed.
- Keep dependency versions and SHA-256 pins in `cmake/Dependencies.cmake` synchronized with user documentation and packaging licenses. Do not change binary runtime requirements merely to quiet a warning.
- Before reporting completion, distinguish observed local checks, unrun platform checks, and Actions results for the exact commit. Commit at a coherent milestone only when requested or coordinated with concurrent owners.
