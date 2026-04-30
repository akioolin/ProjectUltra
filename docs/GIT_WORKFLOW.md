# Git Workflow

Last updated: 2026-04-30

## Goal
Keep workflow simple, safe, and aligned with how this repo is actually maintained.

## Branching
- Default: commit directly to `main`.
- Optional feature branch: use only for risky multi-day work.

## Commit Rules
- Keep commits focused (one logical change per commit when possible).
- Non-WIP commits should build locally before push.
- Prefer imperative commit titles:
  - `Fix ...`
  - `Add ...`
  - `Update ...`
  - `Docs ...`

## Minimal Pre-Push Check

For documentation-only changes:

```bash
cmake --build build -j4
```

For any Tier 0/Tier 1 modem change, follow `docs/QUALITY_STRATEGY.md` and run:

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
./tests/regression_matrix.sh --quick
./scripts/coverage_report.sh
git diff --check
```

For protocol/ARQ/rate-ladder changes, also run:

```bash
scripts/run_alpha_gate.sh --quick
```

## Daily Commands

### Inspect
```bash
git status
git log --oneline -n 10
```

### Commit
```bash
git add <files>
git commit -m "Update <what changed>"
```

### Push
```bash
git push
```

## Safe Recovery

Unstage files:

```bash
git restore --staged <file>
```

Discard local edits in one file:

```bash
git restore <file>
```

Revert an already pushed commit:

```bash
git revert <commit>
git push
```

## Releases and Tags

CI release workflow triggers on tags matching `v*`.

Recommended alpha tagging:

```bash
git tag -a vX.Y.Z-alpha -m "vX.Y.Z-alpha"
git push origin vX.Y.Z-alpha
```

Optional compatibility alias tag:

```bash
git tag -a X.Y.Z-alpha -m "X.Y.Z-alpha"
git push origin X.Y.Z-alpha
```

## Documentation Hygiene
- Keep `docs/KNOWN_BUGS.md` short and current.
- Put detailed fixed history in `docs/CHANGELOG.md`.
- Keep `docs/QUALITY_STRATEGY.md` aligned with the real CI/local gates.
- Remove stale procedures instead of letting them drift.
