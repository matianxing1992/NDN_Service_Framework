# Implementation Plan: NDN-SVS Experimental Convergence

**Branch**: `Experimental` | **Date**: 2026-07-15 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/113-ndn-svs-experimental-convergence/spec.md`

## Summary

Preserve the current divergent NDN-SVS state in an immutable backup, rebuild a
clean review branch from the pinned `origin/master` baseline, retain only true
post-review deltas, repair the atomic publication and reviewability defects
found by the merge audit, then move local `master` and `Experimental` to the
user-approved topology without touching remote refs. Reviewable commits and a
new source-bound unit/MiniNDN candidate are mandatory outputs.

## Technical Context

**Language/Version**: C++17, Python 3.8 validation tools, Git 2.25

**Primary Dependencies**: ndn-cxx 0.9.0, NFD/MiniNDN, Boost 1.71 local baseline,
Waf, local NDNSF integration tree

**Storage**: Git objects/refs for migration recovery; existing NDN-SVS in-memory
publication and mapping stores; immutable local evidence directories

**Testing**: Waf configure/build, complete NDN-SVS Boost.Test suite, focused
failure injection, NDNSF C++/Python tests, Spec 112 MiniNDN campaign

**Target Platform**: Local Ubuntu 20.04/x86-64 and MiniNDN; no Docker/iTiger/Wi-Fi

**Project Type**: Cross-repository library convergence and reliability repair

**Performance Goals**: No throughput claim; retained asynchronous behavior must
not perform undocumented network-face work from arbitrary caller threads

**Constraints**: Preserve all user work; do not push; pin remote baseline before
migration; no transient stash as sole backup; active packet limit 8800 B; no
visible sequence gap; retain negative evidence; use Boost 1.71 locally

**Scale/Scope**: One NDN-SVS dependency repository, one NDNSF Spec Kit control
plane, five final concern-oriented commits, complete 36-case unit baseline plus
focused additions and six Spec 112 MiniNDN cells

## Constitution Check

*GATE: PASS before research and after design.*

| Principle | Design response | Gate |
|---|---|---|
| Canonical Dynamic Runtime | No NDNSF invocation API or wire-name change | PASS |
| Security Is Part Of Data Path | Existing InterestSigner, validation, tokens, and Targeted security remain unchanged | PASS |
| CodeGraph First | Current branch, publication transaction, callers, and tests were traced before design | PASS |
| Spec-Driven Durable Work | Spec 113 owns branch topology, repair contracts, tasks, and evidence | PASS |
| Verify With The Right Scope | Rebuilt unit/failure tests precede one fresh 0% MiniNDN candidate | PASS |

The NDN-SVS repository's four-tool gate is also satisfied through its local
Context Mode/CodeGraph indexes and this NDNSF Spec Kit/GSD control plane.

## Design Decisions

### 1. Preserve, reconstruct, then rename

The original `Experimental` and dirty worktree are committed only on a permanent
backup branch. A separate convergence branch replays the two actual
post-`0521665` commits and the snapshot delta onto pinned `origin/master`.
After repair, split commits, and validation, local `Experimental` is moved to
the convergence result and local `master` is moved to the pinned baseline. The
backup is never rewritten. See [research.md](research.md) and
[contracts/branch-topology.md](contracts/branch-topology.md).

### 2. Preserve reviewed upstream implementations

The remote eight-commit reviewed sequence is authoritative. Equivalent local
commits are not replayed, and the upstream ndn-cxx `InterestSigner` path is not
replaced by the superseded local timestamp implementation. Range-diff/cherry
classification and tests decide any non-equivalent delta explicitly.

### 3. Separate review concerns

Final product commits are organized by behavior, with tests in the same commit:

1. sparse mapping and duplicate-fetch suppression;
2. bounded publication recovery/repair (Spec 110 delta);
3. size-safe transactional segmented publication and callback lifetime;
4. Boost 1.71 fork build baseline;
5. any necessary documentation/evidence-only update outside product history.

The exact number may decrease if a delta is already upstream or fail validation;
it may not increase by mixing unrelated behavior. Local workflow ignores are
excluded. See [contracts/review-commit.md](contracts/review-commit.md).

### 4. Define asynchronous publication honestly

`publishAsync` keeps its public signature. It performs the fallible local
preparation transaction before returning a reserved sequence: final signing and
encoding, complete store insertion, and mapping preparation. Network-face work
and visible state advertisement remain owned by the Face event loop. Its public
documentation calls this **asynchronous advertisement with synchronous safe
preparation**, not worker-pool preparation.

The event-loop commit cursor advances only after mapping installation and the
local version-vector transition succeed. A failure before that local commit
point retains the failed publication at the head, blocks later sequence
advertisement, and schedules bounded retry; it never discards later prepared
state or advances past the failure. Once the local version vector changes, a
network Sync send failure is contained as a delivery attempt failure rather than
misreported as a transaction rollback; periodic/subsequent Sync recovers it.
Optional active Data emission failure likewise leaves readable stored Data for
normal fetch.
Shutdown removes every stored but unadvertised publication.

### 5. Enforce rollback at the DataStore boundary

`DataStore` gains a backward-compatible rollback-capability query. Existing
custom implementations continue to compile and default to unsupported. Every
asynchronous publication transaction that can leave stored but unadvertised
Data, including a single-packet publication, rejects an unsupported store
before its first insert. Stores that opt in implement exact-name erase,
including the built-in and test stores. This prevents false atomicity without
forcing unrelated consumers to change at compile time.

### 6. Rebuild evidence after history changes

Old Spec 112 evidence remains historical. A new candidate is created only after
the final source commits and rebuilt artifacts stabilize. Every final MiniNDN
cell runs once under that identity. Any later source change produces another
candidate rather than overwriting results.

## Migration Phases

1. Record baseline refs, status, diffs, ignored design files, and remote refs.
2. Create permanent backup snapshot and verify recovery hashes.
3. Create convergence branch from the snapshot and rebase only descendants of
   `0521665` onto pinned `origin/master`.
4. Resolve conflicts in favor of reviewed upstream implementations unless a
   Spec 113 requirement says otherwise.
5. Split concerns, write failing tests, repair implementation, and build.
6. Run focused and cross-repository tests; create and execute new MiniNDN candidate.
7. Audit/converge; only then move local `Experimental` and `master` refs to the
   approved topology and prove remote refs unchanged.

## Failure, Rollback, And Stop Rules

- Any missing backup artifact, baseline drift, unowned process, or unexpected
  remote-ref change stops migration before a local branch move.
- Rebase conflicts are resolved on the convergence branch only; abort returns
  to the permanent backup.
- A commit-stage failure test that permits a sequence gap, later advertisement,
  or leaked packet is a correctness blocker.
- Any source change after candidate creation invalidates that candidate for
  final acceptance.
- MiniNDN startup failure before a valid cell is retained as infrastructure
  evidence and does not consume the product cell; a valid cell is never rerun.
- No command in this plan pushes or force-pushes a remote ref.

## Project Structure

### Documentation (this feature)

```text
specs/113-ndn-svs-experimental-convergence/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── branch-topology.md
│   ├── publication-transaction.md
│   ├── review-commit.md
│   └── validation-evidence.md
├── checklists/requirements.md
├── evidence/
└── tasks.md
```

### Source Code And Validation

```text
../ndn-svs/
├── ndn-svs/
│   ├── fetcher.cpp/.hpp
│   ├── core.cpp/.hpp
│   ├── mapping-provider.cpp/.hpp
│   ├── store.hpp
│   ├── store-memory.hpp
│   ├── svspubsub.cpp/.hpp
│   ├── svsync-base.cpp/.hpp
│   └── tlv.hpp
├── tests/unit-tests/
│   ├── mapping-provider.t.cpp
│   └── svspubsub.t.cpp
└── wscript

ndn-service-framework/
├── Experiments/NDNSF_Segmented_Response_Minindn.py
├── Experiments/spec112_candidate_manifest.py
├── tests/python/test_spec112_*.py
└── specs/112-ndnsf-segmented-reliability/
```

**Structure Decision**: Product changes remain in the existing NDN-SVS owners;
Spec Kit control, immutable evidence, and cross-repository validation remain in
the NDNSF repository. No new runtime library or wire namespace is created.

## Post-Design Constitution Check

PASS. The design preserves the reviewed security path, avoids duplicate
history, assigns failure handling to the publication owner, retains all user
work, uses MiniNDN for network closure, and prohibits remote mutation.
