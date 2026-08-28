# Implementation Plan: Complete SVS V3 Review Commit

**Branch**: `Experimental` plus isolated rewrite worktree | **Date**: 2026-07-16 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/115-ndn-svs-v3-review-history/spec.md`

## Summary

Rewrite the still-open PR #36 history so the old incomplete V3 commit is
replaced by one independently buildable, standards-compliant V3 commit that
contains all official V3 production behavior and directly coupled tests.
Rebase the remaining Experimental-only reliability, fork-extension, and
interop work on that rewritten PR head without changing the accepted final
source tree. Publish only after isolated-commit and full-candidate validation,
using a verified remote backup and an exact force-with-lease fence.

## 2026-08-07 Focused PR Rewrite

The current execution is local-only and supersedes the earlier publication
sequence. It reconstructs the same behavior-owned work in a review-first order:

```text
upstream/master
  -> regex/name-only PubSub
  -> V2 InterestSigner
  -> corrected SVS V3                 <- focused PR branch
  -> bounded Mapping/Data optimization
  -> parallel Sync processing
  -> ordered asynchronous publication
  -> sparse Mapping recovery
  -> failure-atomic segmented publication
  -> bounded Fetcher/Repair recovery  <- continuation branch
```

The corrected V3 owner is rebuilt against only the first two commits. It folds
the current official SVS-PS V3 outer-Data and Mapping-query naming into the V3
integration and removes the obsolete unsigned trailing-extension interpretation.
Optional Mapping piggyback is replayed later and, when present, places MappingData
inside the signed StateVector Data content. RepairData remains a fork recovery
extension in the final continuation owner.

Construction occurs in a new sibling worktree. The dirty `Experimental`
checkout, `master`, all remotes, PR #36, tags, and upstream refs remain
unchanged. The NDN-SVS commit range contains source, examples, unit tests, and
deterministic fixtures only. All planning, audit, interop orchestration, and
validation receipts remain in this NDNSF feature directory.

## Technical Context

**Language/Version**: Git object/ref history; Bash and Python 3 validation;
NDN-SVS C++17 source and Boost.Test suites

**Primary Dependencies**: Git, GitHub CLI, waf, ndn-cxx 0.9.0, tracked Boost
1.74 NDN-SVS baseline, host Boost 1.71 used only through disposable
`compileTMP`, NDNts locked interop peer, MiniNDN, NDNSF consumer regressions

**Storage**: Local Git object database and worktrees; fork refs on GitHub;
tracked Spec 114/115 evidence documents

**Testing**: Per-commit build and focused units; fixed wire vectors; standalone
C++/NDNts interop; immutable MiniNDN cells; NDNSF consumer regression; strict
Spec Kit audit

**Target Platform**: Local Ubuntu development VM and GitHub fork PR #36

**Project Type**: Cross-repository source-history migration and protocol review
preparation

**Performance Goals**: No new performance claim; preserve all current
correctness and network results while changing history ownership

**Constraints**: `upstream/master` is read-only; no plain force push; no remote
mutation before explicit authorization; preserve unrelated dirty work; every
review commit keeps Boost 1.74; local Boost 1.71 compilation occurs only on a
deleted-after-use `compileTMP` branch

**Scale/Scope**: One open PR branch, one local Experimental stack, one complete
V3 replacement commit, two repositories' evidence, and the existing Spec 114
validation matrix

## Constitution Check

*GATE: Must pass before rewrite construction. Re-check before publication.*

| Principle | Gate | Status |
|---|---|---|
| Canonical Dynamic Runtime | No NDNSF runtime API change; only consumer regression is required | PASS |
| Security Is Part Of The Data Path | V3 embedded Data validation and atomic rejection remain inside the owning commit | PASS |
| CodeGraph First, Source Verified | Current V3 codec, Core callers, and tests were traced in the target index | PASS |
| Spec-Driven Changes For Durable Work | Spec 115 owns requirements, contracts, tasks, evidence, and rollback | PASS |
| Verify With The Right Scope | Isolated commit, standalone interop, MiniNDN, and consumer gates are required | PASS |
| Cohesive, Outcome-Based Tasks | Tasks close behavioral history outcomes; test/build/evidence are not mechanically split | PASS |

The design uses six cohesive behavioral stages. It does not create one task per
commit command, file, test invocation, or evidence artifact.

## Project Structure

### Documentation (this feature)

```text
specs/115-ndn-svs-v3-review-history/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── tasks.md
├── traceability.md
├── audit-report.md
├── checklists/
│   └── requirements.md
├── contracts/
│   ├── commit-composition.md
│   ├── ref-publication.md
│   └── validation-evidence.md
└── evidence/
    ├── baseline-and-backups.md
    ├── commit-ownership.md
    ├── isolated-v3-validation.md
    ├── experimental-replay.md
    ├── full-validation.md
    ├── boost174-compiletmp-validation.md
    └── publication-receipt.md
```

### Source and history repositories

```text
/home/tianxing/NDN/ndn-svs/
├── ndn-svs/                    # V3 codec, Core, VersionVector, optional extensions
├── tests/unit-tests/           # directly coupled V3 and extension tests
├── tests/fixtures/svs-v3/      # independent fixed wire vectors
├── tests/interop/              # C++ and NDNts independent peers
└── .git/                       # rewritten refs and safety identities

/home/tianxing/NDN/ndn-service-framework/
├── specs/114-ndn-svs-v3-wire-interop/  # prior functional evidence
├── specs/115-ndn-svs-v3-review-history/ # rewrite authority/evidence
└── tests/ and examples/                 # NDNSF consumer gates
```

**Structure Decision**: NDN-SVS owns source and commit history. The service
framework owns cross-project requirements, immutable evidence, and consumer
validation. History surgery occurs in a dedicated NDN-SVS worktree so the
current Experimental checkout remains a recovery authority.

## Architecture and Rewrite Flow

```text
refresh refs and PR state
  -> freeze old master/origin/Experimental OIDs
  -> create local and verified remote safety refs
  -> build commit-ownership manifest
  -> replace incomplete V3 commit in isolated worktree
  -> replay PR #36 tail without V3 repair-only descendants
  -> validate replacement commit in isolation
  -> replay Experimental-only commits and preserve final tree
  -> rerun complete candidate-bound gates
  -> explicit publication checkpoint
  -> lease-protected origin/master update
  -> fresh-fetch containment and PR verification
```

### Commit ownership

The final replacement commit is titled
`svs: implement the version 3 synchronization protocol` and owns:

- the existing BootstrapTime and multi-epoch StateVector behavior;
- explicit V3 naming and V2/V3 profile selection;
- signed StateVector Data inside ApplicationParameters and parameters digest;
- canonical V3 encode/decode and serial/parallel codec parity;
- embedded Data validation before semantic decode and atomic invalid-packet
  rejection;
- normative V3 timers, immediate publication behavior, and zero Sync-Ack;
- direct unit, malformed-vector, fixed-vector, and serial/parallel tests.

It explicitly does not own:

- fork-only Mapping/Repair extension semantics;
- segmented publication recovery or transactional publication;
- independent C++/NDNts peer/harness implementation, which belongs in NDNSF;
- NDNSF consumer behavior.

Those remain separate because they have independent consumers, rollback paths,
and acceptance results.

### Branch topology after completion

```text
upstream/master (unchanged)
  ... PR #36 commits ...
      complete-v3-oid
      remaining-review-commits
      master == origin/master
        \
         Spec 113 reliability commits
         bounded fork-extension commit
         independent interop/harness commit
         Experimental
```

The same `complete-v3-oid` must be an ancestor of all three required refs. The
refs do not have to share the same head because Experimental retains separately
owned later work.

## Safety, Concurrency, and Rollback

- Capture local `master`, `origin/master`, `Experimental`, upstream base, PR
  head, and clean/dirty state before creating a candidate.
- Use both immutable local backup refs and a verified remote backup branch.
- Treat the recorded old `origin/master` OID as a fencing token. Refresh
  immediately before publication and abort on mismatch.
- Permit only `--force-with-lease=refs/heads/master:<expected-old-oid>`; never
  use unqualified `--force` or `--force-with-lease` without the explicit OID.
- Do not delete safety refs during Spec 115.
- Rollback is another lease-protected transition from the published candidate
  OID to the verified remote backup OID; it is never an unconditional push.

## Validation Strategy

1. **Composition gate**: a machine-readable ownership audit proves every
   official V3 hunk and direct test belongs to the replacement commit, and no
   descendant exists only to repair it.
2. **Isolated commit gate**: checkout the replacement V3 commit in a clean
   worktree; build and run fixed-vector, malformed, validation, timer, and
   serial/parallel tests.
3. **PR-tail gate**: each rewritten PR commit is buildable and no unrelated
   diff entered the stack.
4. **Experimental replay gate**: compare old and new Experimental trees and
   detect duplicated or lost Spec 113/114 behavior.
5. **Full candidate gate**: rerun all Spec 114 units, independent interop,
   MiniNDN cells, and NDNSF consumer regressions against new OIDs.
6. **Remote gate**: fresh fetch, master equality, three-ref ancestry, safety-ref
   identity, PR #36 head/description, and remote diff verification.
7. **Final narrative and three-implementation gate**: preserve every validated
   tree while rewriting commit messages to state owned behavior and boundaries;
   then bind a fresh 71-case ASan suite, five-case no-route C++/NDNts matrix,
   and six run-once MiniNDN C++/NDNts cells to the exact final Experimental
   candidate before any new remote publication.
8. **External harness ownership gate**: keep the executable C++ peer, actual
   TypeScript NDNts peer, pinned Node dependencies, standalone launcher, and
   MiniNDN orchestration under NDNSF. Rewrite Experimental to remove the two
   NDN-SVS harness commits, then rerun standalone and MiniNDN gates from the
   NDNSF-owned paths against the exact clean NDN-SVS candidate.
9. **Historical Experimental review-history gate**: the earlier seven-commit
   split preserved the accepted tree and established exact-OID unit evidence.
   It is retained as historical evidence, not as the final review boundary.
10. **Full local-range consolidation gate**: freeze the current shared
    `master`/`Experimental` head, rebuild the local range above
    `upstream/master` as exactly nine behavior-owned commits, fold corrective-only
    descendants into their owners, preserve the final tree exactly, and only
    after construction run every exact-OID unit gate before moving live refs.

### Full Local Review-History Consolidation

The candidate sequence is organized by reviewer question and behavior owner,
not by the date on which a bug was found:

```text
pub/sub selection and name-only publication APIs
  -> bounded piggyback Mapping/Data delivery
  -> parallel sync processing and batching
  -> async publication and parallel production
  -> V2 Signed Interest encoding with ndn-cxx InterestSigner
  -> complete version 3 synchronization protocol
  -> sparse Mapping recovery without duplicate fetches
  -> failure-atomic segmented publication
  -> bounded segmented fetch, validation, and Repair recovery
```

Late piggyback recovery, typed bounds, cache limits, and iterator stability are
folded into one piggyback owner. Async and piggyback tests are folded into their
production owners. Sparse Mapping and duplicate-fetch suppression share one
recovery behavior. Segmented publication is a producer-side commit transaction;
segmented fetch/Repair is a receiver-side recovery transaction, so they remain
separate owners. Validated Mapping/Repair extension transport belongs to the
receiver recovery owner. The official V3 owner remains separate from all fork
extensions. The former Boost 1.71 build-policy commit remains excluded.

Construction occurs on a temporary branch in an isolated NDN-SVS worktree.
The whole candidate is constructed first, matching the user's requested
workflow. Validation then walks the surviving commits in order; a failure is
repaired in its owner and the walk resumes from that OID. Only after every
commit passes and the final tree ID matches frozen `8335643f` are both local
`master` and `Experimental` moved together. No remote publication belongs to
this consolidation.

### NDN-SVS local Boost validation boundary

Tracked NDN-SVS history always retains the Boost 1.74 requirement. For each
local compile, validation creates `compileTMP` at the exact candidate OID,
applies the old one-file 1.71 threshold patch only on that temporary branch,
runs configure/build/unit tests, switches back to the candidate branch, and
deletes `compileTMP`. Receipts must show both the 1.71 configure result and final
branch deletion. This rule is scoped to NDN-SVS and does not change the Boost
1.71 policy of NDNSF or any other repository.

## Evidence and Completion Semantics

Checked tasks alone do not establish completion. The completion summary must
bind:

- old and new OIDs and tree IDs;
- ownership-manifest result;
- isolated and full validation commands/results;
- local and remote branch identities after a fresh fetch;
- PR #36 head and review-visible replacement commit;
- verified backup and rollback command;
- explicit statement that upstream refs, tags, and releases were unchanged.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
|---|---|---|
| Rewrite an open remote review branch | The incomplete V3 behavior must appear complete in its owning commit and reviewers explicitly requested folding fixes | Appending another fix commit preserves the misleading hybrid V3 commit and does not meet the review goal |
| Preserve a separate Experimental tail | Spec 113 reliability and fork extensions are validated but are not official V3 protocol behavior | Folding all later work into the V3 commit would misrepresent the standard and create an unreviewable change |
