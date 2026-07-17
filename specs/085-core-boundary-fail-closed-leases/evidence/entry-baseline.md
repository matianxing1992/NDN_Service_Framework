# Entry Baseline And Dirty-Worktree Resolution

**Date**: 2026-07-10
**Branch**: `Experimental`
**Starting HEAD**: `2235fddc63863c644afa55065cf0098ba795d2bd`

## Ownership Resolution

The pre-existing dirty worktree was preserved, tested, and split into four
ownership-scoped commits. No reset, checkout, stash, or destructive cleanup was
used.

| Commit | Owner and scope | Verification |
|---|---|---|
| `ab61fbc` | Core Targeted Python API, authenticated invocation context, exact-Data and Repo producer bindings | Core build; 14 Targeted C++ tests; 2 Python Targeted tests |
| `f919b61` | Distributed Repo cache, exact packet storage, quorum/failover, repair, campaigns, and tests | 80 Repo Python tests; 3 Repo C++ contract binaries |
| `91cedb0` | Specs 071-083 and Repo/UAV design slides | Repo PDF 20 pages; UAV PDF 22 pages; both built twice |
| `f67434e` | Removal of the obsolete delegate helper and its tests | deletion-only workflow cleanup |

## Target Ownership

After these commits, all 085 production targets listed in `plan.md` are clean.
The remaining untracked paths are only Specs 084/085 and the read-only Occam
audit utility/test owned by the parent planning work.

## Exclusions

- `.planning/` and `.codegraph/` remain ignored local workflow state.
- LaTeX `.aux`, `.log`, `.nav`, `.out`, `.snm`, and `.toc` files remain ignored.
- `results/` remains local experimental output; evidence documents link exact
  result paths rather than committing bulk logs.
- Proposal slides, NDN-SVS, V1 permission behavior, and credentials are outside
  Spec 085.

## Rollback Boundary

085 implementation begins after `f67434e`. Each migration wave must be a new
commit. Roll back in reverse order: export deletion, caller migration, DI lease
service, then Core lease table. The four baseline commits above are not part of
085 rollback.
