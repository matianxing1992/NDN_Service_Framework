# Implementation Plan: Historical NDN-SVS Threading-Contract Recovery

**Branch**: `134-svs-sync-crash-recovery` | **Date**: 2026-07-22 | **Spec**: [spec.md](spec.md)

## Summary

Preserve the failed cross-thread ASan/TSan attempts as evidence that the
historical chat example's thread-safety claim is not backed by the public
contract or tests. Stop the library-lock repair path. Build a clean historical
subject and a two-node/two-process qualification driver where one
Face/io_context thread per process owns application publication and all
NDN-SVS work. Only a qualified result may admit Spec 133's fresh five-cell
profiling campaign.

## Technical Context

**Subject**: NDN-SVS `a9944019f76791773604999f00128057b9534ace`

**Dependencies**: ndn-cxx, NFD, MiniNDN, Boost 1.71, Waf

**Project type**: Experiment-contract correction and isolated qualification
tooling; no NDN-SVS production repair

**Execution model**: Two MiniNDN nodes, one application process per node, one
Face/io_context execution thread per process

**Timing**: Absolute monotonic application timer; 10/60/10 qualification at
target 1000 pps per peer

**Constraints**: Synchronous `publish()` only; no `publishAsync`, publisher
thread, pacer thread, worker pool, cross-thread post adapter, catch-up burst,
library source repair, formal retry, or mutation of old evidence

## Constitution Check

- CodeGraph and exact Git-object reads establish code reality before design.
- Spec Kit owns the durable correction to Specs 133/134.
- MiniNDN remains the network qualification environment.
- Negative evidence and zero formal-cell consumption are preserved.
- Tasks are grouped by reviewable outcomes: audit/reclassification, corrected
  harness, qualification, and handoff.

## Experiment Design

### Research question

Can the exact pre-async NDN-SVS subject complete the original high-rate
correctness boundary when every application and NDN-SVS operation obeys one
Face/io_context execution context per process?

### Evidence hierarchy

```text
README/API silence
  < example claim
  < source ownership/locking facts
  < concurrency tests (absent)
  < sanitizer runtime evidence
```

The example is evidence of intended usage, but not sufficient to override API
silence, absent tests, and measured races.

### Per-peer execution

```text
process initialization
  create Face, security, SVSPubSub, subscription, application timer
  arm start deadline
  call Face::processEvents() on the same thread

timer callback on Face/io_context
  calculate expected absolute release
  record lateness and elapsed/missed slots
  publish at most one item synchronously
  arm the next future absolute release

Face/io_context
  also executes Sync, Mapping, fetch, validation, and delivery callbacks
```

If work runs behind, the next index advances to the first deadline strictly
after the callback completes. Skipped deadlines increment
`missedReleaseSlots`; they are not enqueued.

### Controls

| Class | Definition |
|---|---|
| Independent | Corrected single-I/O-thread qualification only |
| Dependent | scheduled/attempted/missed/delivered/invalid/error counts, exits, corruption scan |
| Controlled | commit, Boost patch, topology, payload, signing, compression, timers, run duration |
| Excluded | Cross-thread repaired library, profiling patch, latest/async subject, NDNSF |

## Phases

### Phase 0: Audit and reclassify

Record the README/header/example/test/source/history/runtime matrix. Mark the
old cross-thread preflight and diagnostics as ineligible for formal profiling.
Retain their paths and hashes. Withdraw the repair direction without deleting
the repair worktree or evidence.

### Phase 1: Correct the harness

Add a new qualification driver rather than mutating the frozen diagnostic
driver. Add source-contract and local self-tests proving one event thread,
single outstanding timer, remote-vs-local delivery classification, missed-slot
accounting, and no disallowed APIs/threads.

### Phase 2: Build and qualify once

Build only the exact clean subject plus canonical Boost 1.71 patch. Freeze
binary/library/source hashes and linkage. Run one fresh MiniNDN qualification.
Preserve its terminal outcome with no retry.

### Phase 3: Gate Spec 133

If `QUALIFIED`, allow Spec 133 T006-T008 to create a corrected driver, new
manifest, and new preflight path. If not, leave formal profiling blocked.

## Artifact Layout

```text
specs/134-svs-sync-crash-recovery/
├── spec.md
├── plan.md
├── research.md
├── tasks.md
├── traceability.md
├── contracts/crash-evidence-contract.md
├── checklists/pre-implementation-audit.md
└── evidence/threading-contract-audit.md

Experiments/
├── build_svs_sync_crash_recovery.py
├── NDN_SVS_Sync_Crash_Recovery_Minindn.py
└── ndn-svs-pubsub-benchmark/
    ├── svs-sync-crash-recovery.cpp        # frozen cross-thread diagnostic
    └── svs-sync-io-qualification.cpp      # corrected model

results/spec134-svs-sync-crash-recovery/
├── diagnosis-*                            # preserved, ineligible
└── io-qualification-*                     # new once-only gate
```

## Rollback

The corrected work changes only repository experiment/spec files and isolated
build artifacts. The active NDN-SVS checkout is untouched. Rollback consists
of discarding the new harness/spec edits; old diagnostic evidence remains
available. No repair commit is merged or loaded.
