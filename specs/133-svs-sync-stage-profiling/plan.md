# Implementation Plan: Synchronous NDN-SVS Stage Profiling

**Branch**: `133-svs-sync-stage-profiling` | **Date**: 2026-07-22 | **Spec**: [spec.md](spec.md)

## Summary

Build one profiling-only NDN-SVS subject from the last commit before
`publishAsync()` and internal parallel Sync/production workers. Add structured,
deterministically sampled `NDN_LOG` spans plus exact aggregate counters around
the complete synchronous PubSub hot path. Reuse the two-peer direct-call usage
model from Spec 132, but create separate Spec 133 source, build, runner,
analyzer, and result authorities. Admit exactly five once-only MiniNDN cells at
200/400/600/800/1000 publications/s per peer only after a fixed profiling
overhead preflight passes. Produce CPU-stage, queue/wait, path-frequency, and
rate-boundary tables that identify a supported bottleneck or state that the
evidence is inconclusive.

## Technical Context

**Language/Version**: C++17 and Python 3.8

**Primary Dependencies**: NDN-SVS commit
`a9944019f76791773604999f00128057b9534ace`, ndn-cxx, Boost.Log through
`NDN_LOG`, Boost 1.71, OpenSSL, MiniNDN, NFD

**Storage**: Immutable structured NDN-SVS profile logs, benchmark JSONL events,
JSON manifests/receipts/summaries, and CSV/Markdown tables

**Testing**: Source-contract and analyzer unit tests, instrumented NDN-SVS unit
tests, exact-build audit, three-arm instrumentation/logging-overhead preflight,
five-cell MiniNDN campaign, post-campaign schema/accounting audit

**Target Platform**: Linux host running two MiniNDN namespaces on the same
kernel monotonic clock; fixed four-CPU allowance

**Project Type**: Pure NDN-SVS benchmark and profiling patch; NDNSF runtime is
not linked or executed

**Performance Goals**: Attribute synchronous PubSub cost at 200, 400, 600,
800, and 1000 publications/s per peer while keeping profiling perturbation
within the FR-009 admission bound

**Constraints**: Synchronous `publish()` only; no `publishAsync()` or internal
worker pools; one process per MiniNDN node; application publication and all
NDN-SVS work on one Face/io_context thread per process; 256-byte non-segmented
payload; `NDN_SVS_COMPRESSION` disabled; 10/60/10-second warmup/measure/drain;
five formal cells only; no formal retry; deterministic one-in-100 trace
sampling; exact all-call counters

**Scale/Scope**: One instrumented subject, two equal peers, two directions,
five formal rates, publisher/Sync/Mapping/payload/subscription paths

## Constitution Check

- **Canonical runtime/security boundary**: This is an NDN-SVS-only experiment;
  no NDNSF protocol/API/security behavior is changed. The HMAC Sync Interest and
  digest Data signing profile is frozen and reported rather than optimized.
- **CodeGraph/source verified**: CodeGraph traced current PubSub symbols before
  exact `git show` inspection of the historical commit. The plan is based on
  actual historical calls in `svspubsub.cpp`, `svsync-base.cpp`, `core.cpp`,
  `mapping-provider.cpp`, `fetcher.cpp`, `version-vector.cpp`, and
  `store-memory.hpp`.
- **Spec driven**: Spec 133 owns the profiling patch, schemas, five-cell
  campaign, and claims. Specs 131/132 remain immutable inputs only for the
  usage-model decision, not for formal aggregation.
- **Right verification scope**: Final network evidence uses MiniNDN and a
  60-second measured window. One valid negative result per rate is retained.
- **Resumable benchmark state**: GSD installation health was validated; sealed
  manifests and terminal receipts provide the actual campaign recovery and
  single-writer boundary.
- **Experiment discipline**: The ARS plan-mode contract separates independent,
  dependent, controlled, and confounding variables; logging overhead has a hard
  admission gate; one run per rate is reported descriptively.
- **Task cohesion**: Each implementation task closes a reviewable behavior with
  tests and evidence, rather than splitting test/edit/run/record into mechanical
  fragments.

**Pre-design gate**: PASS. No constitution violation or unresolved
clarification remains.

## Design

### Subject identity and isolation

Create `build/spec133/worktrees/sync-stage-profile` from exact commit
`a994401...`. Apply and record two logically distinct patches:

1. the byte-identical Specs 131/132 Boost 1.71 `wscript` compatibility patch;
2. a profiling-only patch adding stage timers/counters/log records and their
   call sites.

The profiling patch may change diagnostics and profiling-only build metadata;
it may not change names, payloads, timers, signing configuration, state-vector
decisions, Mapping selection, fetch behavior, data structures used by the
protocol, or callback ordering. The active `/home/tianxing/NDN/ndn-svs`
checkout and old build worktrees must not move or become dirty.

### Diagnostics architecture

The profiling patch adds a dedicated `ndn_svs.Profile` logging module so formal
execution can enable only structured profile records:

```text
NDN_LOG=ndn_svs.Profile=TRACE
NDN_SVS_PROFILE_ENABLED=1
NDN_SVS_PROFILE_CELL_ID=<cell>
NDN_SVS_PROFILE_PEER_ID=<peer-a|peer-b>
NDN_SVS_PROFILE_SAMPLE_MODULUS=100
```

Every instrumented call reads `CLOCK_MONOTONIC_RAW`. Exact aggregate counters
cover every call; a deterministic correlation-key rule emits all spans for one
in 100 publication/Sync/fetch operations. Logging occurs on the calling thread
and adds no logger worker, queue, or protocol task. A process-end flush emits
one summary per registered stage. The profiler never throws into the protocol
path; a dropped/invalid diagnostic record increments an explicit counter.

Span kinds are semantically disjoint:

- `leaf-cpu`: non-overlapping work eligible for service-demand sums;
- `aggregate`: parent duration used for residual/critical-path checks but never
  added to its leaf children;
- `lock-wait`: time before an explicit mutex acquisition, excluded from CPU
  demand and from the protected operation's leaf timer;
- `queue-wait`: scheduler or Face/fetcher waiting, excluded from CPU share;
- `external-wait`: express-to-receive interval containing NFD/network time,
  excluded from CPU share;
- `milestone`: zero-duration correlation boundary.

### Measurement paths

The stage registry in
[contracts/stage-measurement-contract.md](contracts/stage-measurement-contract.md)
covers five paths:

1. application timer callback and synchronous `publish()` on Face/io_context;
2. same-thread Sync Interest production;
3. peer Sync Interest verification/decode/merge;
4. piggyback or Mapping fallback resolution;
5. piggyback or Payload fallback decode/validation/subscription delivery.

The registry also measures explicit lock acquisition on these paths. The
historical patch must place the protected-operation timer after acquisition so
lock waiting cannot be mislabeled as CPU service demand.

Top-level aggregate spans reconcile their valid contained leaf children. The
analyzer computes `residualNs = aggregateNs - union(child leaf intervals)`,
rejects negative durations, and records impossible parent/child containment as
an explicit violation. An uncontained sampled child is excluded from that
parent's residual calculation while its exact all-call stage aggregate remains
available in the rate-stage table. Residual time remains visible and is not
assigned to the nearest convenient stage.

### Correlation without wire changes

Publication, Mapping, and Payload operations use existing node/sequence/range
identities. A cross-peer Sync Interest wait may be emitted only when an existing
wire-derived identity plus occurrence ordering gives one unique send/receive
match. Every join is labeled `exact`, `ambiguous`, or `censored`; ambiguous and
censored joins contribute only counts and never network-wait or critical-path
claims. The profiler must not add a trace field, nonce, name component, or any
other wire-visible value.

### Overhead admission

Before a formal manifest exists, run three short fixed 1000-publications/s-per-
peer arms under matched topology and resource settings:

1. clean historical binary: base plus Boost patch, no profiling patch;
2. profiled binary with the profiler disabled;
3. the same profiled binary with exact formal profiling/logging enabled.

The triplet is diagnostic only and cannot enter formal rate tables. A-vs-B
isolates instrumentation-path cost, B-vs-C isolates enabled logging cost, and
A-vs-C bounds total perturbation. Attempted rate, delivery ratio, CPU use,
failures, and enabled-log completeness are checked against FR-009. A failure
blocks sealing; the experiment is redesigned instead of subtracting an
estimated penalty.

### Formal campaign

The manifest contains only:

```text
01 sync-profile-200
02 sync-profile-400
03 sync-profile-600
04 sync-profile-800
05 sync-profile-1000
```

Each cell has two symmetric MiniNDN nodes with one peer process per node. In
each process, one Face/io_context thread owns the absolute-deadline application
timer, synchronous `publish()`, Sync/fetch handling, and subscription delivery.
There is no publisher/pacer thread and no cross-thread NDN-SVS call. Each cell
uses 10-second warmup, 60-second measurement, 10-second drain, and one terminal
receipt. A subject crash or overload is admissible negative evidence; an
infrastructure-invalid outcome is labeled and still not silently replaced.

### Analysis and bottleneck rule

For each stage/rate/peer, compute calls, samples, mean, p50/p95/p99/max,
exclusive duration, calls per attempted and delivered publication, estimated
service demand, and leaf CPU share within the relevant thread. Separately
compute scheduler lateness, Face/fetcher queue intervals, Sync/Mapping/Payload
external waits, attempted/delivered rate, delivery ratio, and path frequencies.

A stage is named as the primary bottleneck only when at least two signals agree:

- it dominates mutually exclusive leaf demand/share on the constrained thread;
- its p95, queue delay, calls per publication, or fallback frequency grows with
  the rate;
- the growth aligns with the first attempted-rate or delivery-rate boundary.

The final report ranks evidence-backed candidates separately for application
publication work, remaining Face/io_context work, timer delay, and external
wait, even though CPU stages share one thread. It may conclude that no single
bottleneck is established.

## Project Structure

```text
Experiments/
|- ndn-svs-pubsub-benchmark/svs-sync-stage-profile.cpp
|- build_svs_sync_stage_profile.py
|- NDN_SVS_Sync_Stage_Profile_Minindn.py
`- analyze_svs_sync_stage_profile.py
tests/python/
`- test_spec133_svs_sync_stage_profile.py
specs/133-svs-sync-stage-profiling/
|- spec.md
|- plan.md
|- research.md
|- data-model.md
|- contracts/stage-measurement-contract.md
|- quickstart.md
|- tasks.md
|- traceability.md
`- evidence/
build/spec133/
|- worktrees/sync-stage-profile/
|- subject-foundation.json
|- subject-manifest.json
`- bin/
   |- svs-sync-clean-control
   `- svs-sync-stage-profile
results/spec133-svs-sync-stage-profiling/
`- <campaign-id>/
```

The profiling worktree patch is expected to touch historical equivalents of:

```text
ndn-svs/profile.hpp
ndn-svs/profile.cpp
ndn-svs/svspubsub.cpp
ndn-svs/svsync-base.cpp
ndn-svs/core.cpp
ndn-svs/mapping-provider.cpp
ndn-svs/fetcher.hpp
ndn-svs/fetcher.cpp
ndn-svs/version-vector.cpp
ndn-svs/store-memory.hpp
wscript
```

The builder has two ordered modes. `prepare` creates the Boost-only clean head,
builds its library, creates the profiling worktree at that exact head, and
writes `subject-foundation.json`. After T003--T006 create the shared Spec 133
driver and profiling source, `finalize` freezes the profiling patch, rebuilds
the profiled library, compiles the same driver once against each library, and
writes the final `subject-manifest.json`. No task claims a benchmark binary
before its shared source exists.

**Structure Decision**: Use new Spec 133 driver/runner/analyzer names and a new
worktree/result root. Reusing or modifying Spec 132 artifacts would blur the
profiling subject and formal evidence boundary.

## Post-Design Constitution Check

PASS after pre-implementation remediation. The design remains NDN-SVS-only,
MiniNDN-based, hash-bound, synchronous, non-parallel, five-cell, and once-only.
CPU, lock, queue, and external waiting measurements are separated; both the
instrumentation path and enabled logging perturbation are gated; tasks remain
cohesive.

## Rollback And Evidence Boundary

Source rollback is removal of the new Spec 133 files and temporary profiling
worktree/branch after preserving patch/manifests. Once a formal campaign is
sealed, raw logs and receipts are immutable. Removing diagnostics from a future
production patch does not authorize rewriting Spec 133 evidence.
