# Implementation Plan: Bidirectional NDN-SVS Capability Comparison

**Branch**: `132-svs-pacer-isolation` | **Date**: 2026-07-22 | **Spec**: [spec.md](spec.md)

## Summary

Replace the one-way publisher/subscriber plus eventfd adapter with one symmetric
peer program. Each process starts Face on its I/O thread, subscribes to the
opposite peer, and uses the application main thread to call either synchronous
`publish()` or asynchronous `publishAsync()` directly. Run a sealed 10-cell
MiniNDN matrix at five per-peer rates, baseline first, and analyze both
directions independently.

## Technical Context

**Language/Version**: C++17 and Python 3.8

**Primary Dependencies**: ndn-cxx, two frozen NDN-SVS commits, MiniNDN, NFD,
OpenSSL, Boost 1.71 headers

**Storage**: Immutable JSONL peer events; JSON manifests/receipts/summaries; CSV
and Markdown comparison tables

**Testing**: Python unittest contracts, C++ self-tests, dual-commit builds,
MiniNDN preflight, exact 10-cell formal matrix, post-implementation audit

**Target Platform**: Linux with four permitted logical CPUs

**Project Type**: Pure NDN-SVS benchmark; no NDNSF runtime

**Performance Goals**: Per-peer targets 200/400/600/800/1000 publications/s;
60-second measured window; direction-specific delivery and delay

**Constraints**: Direct public API calls from application main; Face I/O thread;
no harness queue/post; 10 cells only; baseline block first; no formal retry;
identical Boost 1.71 build patch

**Scale/Scope**: Two peers per cell, two directions, ten formal cells

## Constitution Check

- CodeGraph/source inspection confirms the baseline example uses a Face thread
  and main-thread synchronous publication, while `publishAsync()` stages work
  and posts its own ordered commit internally.
- Spec Kit owns the corrected experiment and leaves Spec 131 frozen.
- GSD health is valid for resumable campaign work.
- ARS experiment discipline fixes the independent variable to the immutable
  capability commit, defines per-peer target as the treatment, and separates
  instrument validity, local API return, and remote delivery.
- The 60-second MiniNDN gate and once-only negative-result retention satisfy the
  project verification constitution.
- Tasks are cohesive outcomes rather than one item per file or command.

## Design

### Peer execution

Both processes execute the same driver:

```text
application main thread: absolute-deadline loop -> direct publish API
Face I/O thread: processEvents -> Sync/update/fetch/delivery callbacks
latest subject only: NDN-SVS receive/production worker pools
```

The main thread never delegates a publication to the Face io_context. It stops
creating calls when the measured end boundary is reached. A slow synchronous
call therefore reduces achieved returns naturally.

### Bidirectional naming and payload

Each peer publishes under `/spec132/publication/<peer-id>/<logical-id>` and
subscribes to `/spec132/publication`. The payload binds peer ID, logical ID,
planned monotonic timestamp, phase, cell identity, and deterministic bytes.
Each receiver accepts only the configured opposite peer ID.

### Timing semantics

- Per-peer period is `1 second / target rate`.
- The loop uses absolute deadlines to avoid cumulative sleep drift.
- `api-enter` is recorded immediately before the direct public API call.
- `api-return` is recorded only after a successful return.
- The loop does not add a queue, skip an individual deadline in advance, or
  continue overdue publications after the fixed end boundary.
- Remote delivery delay joins the payload's planned timestamp to the opposite
  peer's delivery timestamp on the shared host clock.

### Formal campaign

```text
ordinals 01-05: sync-publish-no-internal-parallelism at 200..1000
ordinals 06-10: async-publish-parallel-sync at 200..1000
```

Every outcome receives one receipt. Subject failures do not authorize retry;
independent later cells continue. Source/binary/manifest drift stops admission.

## Project Structure

```text
Experiments/
|- ndn-svs-pubsub-benchmark/svs-pubsub-bench.cpp
|- build_svs_pubsub_commit_bench.py
|- NDN_SVS_PubSub_Commit_Latency_Minindn.py
`- analyze_svs_pubsub_commit_latency.py
tests/python/
`- test_spec132_svs_bidirectional_commit_latency.py
specs/132-svs-pacer-isolation/
|- spec.md
|- plan.md
|- research.md
|- data-model.md
|- contracts/bidirectional-measurement-contract.md
|- quickstart.md
|- tasks.md
`- evidence/
results/spec132-svs-pacer-isolation/
|- treatment-1000-full60-20260722a/  # non-admissible failed harness
`- <new-formal-campaign>/
```

**Structure Decision**: Reuse the existing untracked Spec 131 benchmark file
locations but change their schema/output authority to Spec 132. Keeping the
paths avoids unnecessary parallel implementations; formal identities are
distinguished by content hashes and Spec 132 schemas.

## Rollback And Evidence Boundary

The source rollback is removal of the Spec 132 benchmark files. Raw formal
evidence is immutable once created. The failed adapter diagnostic is retained
but excluded by manifest identity and path. No Spec 131 result is rewritten.
