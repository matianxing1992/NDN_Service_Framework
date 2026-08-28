# Implementation Plan: DistributedRepo Large-Artifact Transport

**Branch**: working tree (no feature branch created) | **Date**: 2026-07-29 |
**Spec**: [spec.md](spec.md)

**Input**: Feature specification from
`specs/164-distributed-repo-large-artifact-transport/spec.md`

**Status**: Design complete; implementation and formal experiments not started

## Summary

Replace the current large-object path—repeated control operations, per-packet
asymmetric signatures, serial or narrowly batched fetching, and durable
wire-packet rows—with a generic immutable-artifact protocol. One NDNSF
collaboration selects and authorizes repositories; a pipelined segmented NDN
data plane transfers bytes; a publisher-authenticated hierarchical manifest
binds cheap bulk digests; separated payload and metadata backends provide
resumable, crash-consistent activation.

The first implementation target is a filesystem content-addressed payload
backend plus the existing embedded database restricted to transactional
metadata. A different metadata engine is permitted only after a matched
benchmark demonstrates that the corrected metadata workload is the bottleneck.
The exact-packet format remains available as a compatibility mode.

## Technical Context

**Language/Version**: C++17 for the core/runtime and Python >=3.8 for the
current DistributedRepo orchestration and public wrapper

**Primary Dependencies**: ndn-cxx/NDN packet and segmented-transfer facilities,
the generic NDNSF collaboration/Targeted runtime, Python bindings, OpenSSL or
the configured ndn-cxx cryptographic provider, and the current repository
control protocol

**Storage**: Content-addressed immutable payload files and atomic temporary
files; transactional embedded metadata initially; legacy exact wire-packet
storage retained behind an explicit compatibility backend

**Testing**: Existing C++ unit-test/Waf infrastructure, Python pytest suites,
focused repository integration tests, and MiniNDN network/security/performance
campaigns

**Target Platform**: Linux hosts and containers running NFD/NDNSF; MiniNDN is
the normative local network gate; TigerCluster is a deferred external-validity
environment

**Project Type**: Mixed C++ library/runtime, Python extension and orchestration
package, repository node application, and experiment harness

**Performance Goals**: Digest-only repository goodput >=85% of matched raw NDN
goodput for artifacts >=64 MiB; signed-manifest goodput within 10% of
digest-only; no per-chunk NDNSF control calls; metadata and memory scaling
independent of 4 KiB packet count

**Constraints**: Preserve existing NDNSF authorization and replay protection;
never expose partial content; bounded manifest parsing and cryptographic work;
resume only an exact immutable identity; no public shared-HMAC requirement;
no silent format downgrade; preserve negative benchmark results

**Scale/Scope**: 1 MiB through 16 GiB formal artifacts, millions of NDN Data
packets, one or three replicas, and 1/4/16 concurrent artifacts; generic models,
datasets, checkpoints, media, and other immutable byte objects

## Constitution Check

*GATE: PASS before research; PASS after Phase 1 design.*

| Principle | Plan evidence | Verdict |
|---|---|---|
| Canonical Dynamic Runtime | Existing unified `/NDNSF/DistributedRepo` services and generic collaboration/Targeted control are reused; no generated stubs or split service/function API is introduced. | PASS |
| Security Is Part Of The Data Path | Control operations retain NAC-ABE, permissions, tokens, provider checks, and replay protection. Root provenance, digest hierarchy, parser bounds, atomic visibility, revocation, and downgrade behavior are explicit contracts. | PASS |
| CodeGraph First, Source Verified | Current `put_file → put → store_object`, packet serialization, control executor, Python persistence, and C++ backend boundary were traced before planning. | PASS |
| Spec-Driven Durable Work | Spec, research, data model, contracts, quickstart, and experiment plan precede implementation. | PASS |
| Verify With The Right Scope | Unit, integration, corruption/recovery, and MiniNDN matched-throughput gates precede TigerCluster. | PASS |
| Cohesive Outcome-Based Tasks | Future tasks will be grouped by observable behaviors: trusted transfer, persistence/recovery, API, compatibility, and evidence—not by file or test command. | PASS |

No constitutional violation or unresolved clarification remains.

## Architecture

### Control and Data Flow

```text
Publisher application
  │ public publish API
  ▼
NDNSF collaboration
  Request → advisory ACK offers → ACK_CLOSED → select replicas
  → Selection → provider bounded task queue
  │
  ├─────────────── one bounded control flow per replica/lifecycle
  ▼
Segmented NDN producer
  RootManifest / ManifestPages / Artifact Data
  │
  ▼
Repository transfer engine
  dequeue task → internal transfer session → windowed Interests
  → digest verification → range writes
  │
  ▼
PayloadStore temporary object + MetadataStore session/progress
  │ verify full identity
  ▼
atomic finalize → ReplicaReceipt → catalog ACTIVE
  │
  ▼
Consumer fetch API
  resolve reference → windowed retrieval → verify → atomic destination
```

### Trust Composition

```text
NDNSF trust policy validates publisher and signed RootManifest
  └── root digest authenticates bounded ManifestPages
        └── page digests authenticate artifact chunks
              └── chunk/full digests authenticate reconstructed bytes
```

Data integrity does not require a shared HMAC key. Optional HMAC use is confined
to negotiated closed-domain session authentication and never substitutes for
publisher provenance or content identity.

### Persistence Composition

```text
ArtifactRepository
  ├── PayloadStore
  │     ├── FsCasPayloadStore          # scalable default
  │     └── ExactPacketPayloadStore    # legacy compatibility
  └── MetadataStore
        ├── EmbeddedMetadataStore      # initial corrected workload
        └── future alternative         # only after evidence
```

The deployed runtime must have one authoritative persistence contract. The
current Python direct database ownership and C++ `RepoStoreBackend` cannot
remain independent authorities for the same object state.

### Lifecycle

```text
ABSENT → QUEUED → RECEIVING → VERIFIED → COMMITTED → ACTIVE
           │          │            │
           └──────────┴────────────┴──→ FAILED / EXPIRED
```

`QUEUED` records an accepted Selection assignment in the Provider's bounded
execution queue. It does not reserve artifact bytes or lock storage. The
Provider creates only short-lived internal transfer/finalization ownership when
the queued task actually begins execution. Legacy `RESERVED` rows remain
readable solely for migration and rollback.

Final payload rename, durable metadata transition, receipt publication, and
catalog activation need a recovery protocol that resolves every crash point to
one valid state. Partial files remain outside the active namespace.

## Phase 0: Research Decisions

Research is recorded in [research.md](research.md).

1. Create a separate generic repository feature rather than extending Qwen or
   DI planning specs.
2. Use a signed bounded root plus digest-authenticated hierarchy; reject public
   per-Data HMAC as the default.
3. Use hierarchical manifest pages and derived names rather than a monolithic
   packet list.
4. Use NDNSF once for control and pipelined segmented NDN for bulk bytes.
5. Separate payload and metadata persistence; correct the record model before
   selecting a new database engine.
6. A positive repository ACK is an advisory willingness/capability snapshot,
   never a capacity reservation or lock. Selection authorizes one exact store
   task; the provider's bounded queue supplies backpressure. Internal
   transactional ownership used for writes, resume, finalization, or GC begins
   only during task execution and is not exposed as ACK state.
7. Preserve exact-packet-v1 and introduce versioned artifact-manifest-v2
   without silent downgrade.
7. Freeze MiniNDN matched baselines and acceptance ratios before
   implementation results.

All Technical Context questions are resolved sufficiently for design.

## Phase 1: Design and Contracts

### Data Model

[data-model.md](data-model.md) defines artifact identity, root and page
manifests, chunks, sessions, progress, receipts, capabilities, backend
records, lifecycle transitions, and validation invariants.

### Public API

[contracts/high-level-api.md](contracts/high-level-api.md) defines:

- simple synchronous publish and fetch;
- asynchronous progress/cancellation;
- advanced resumable sessions;
- stable artifact references and receipts;
- idempotency and error categories;
- public control-mode behavior without private-field access.

### Manifest and Trust Contract

[contracts/artifact-manifest-v2.md](contracts/artifact-manifest-v2.md) freezes:

- root/page/chunk logical fields;
- digest and signature composition;
- name derivation and immutable identity;
- algorithm negotiation;
- bounds, substitution defenses, policy epochs, and revocation;
- capability failure and downgrade prohibition.

The exact TLV type numbers remain an implementation task that must check the
repository's assigned-number policy before allocation.

### Transfer and Lifecycle Contract

[contracts/transfer-lifecycle.md](contracts/transfer-lifecycle.md) defines:

- BeginStore collaboration, advisory ACK offer, and exact queued assignment;
- internal transfer-session ownership for resume/finalization, never an
  ACK-time capacity reservation;
- segmented producer/fetcher responsibilities;
- resume, backpressure, retransmission, and progress;
- replica commit/receipt/activation;
- crash and failure semantics.

### Persistence Contract

[contracts/storage-backends.md](contracts/storage-backends.md) defines:

- payload and metadata responsibilities;
- atomicity boundary;
- filesystem CAS default behavior;
- legacy backend;
- metadata-engine neutrality;
- garbage collection and migration.

### Evidence Contract

[contracts/performance-evidence.md](contracts/performance-evidence.md) and
[experiment-plan.md](experiment-plan.md) define matched ceilings, matrix,
metrics, frozen thresholds, admissibility, repetition, statistical reporting,
and retention of negative evidence.

### Validation Guide

[quickstart.md](quickstart.md) gives the ordered, non-destructive validation
path. Commands that depend on future implementation are marked as planned
interfaces and must become executable before the corresponding task closes.

## Implementation Strategy

### Slice 1: Manifest Trust Core

Deliver bounded manifest encoding/decoding, immutable identity, algorithm
capabilities, trust validation, transitive digest verification, and adversarial
parser tests. Acceptance is complete corruption/substitution rejection without
any payload persistence dependency.

### Slice 2: Streaming Payload and Metadata Backends

Deliver range-based temporary writes, verified progress, atomic finalization,
metadata lifecycle, crash recovery, and garbage collection behind one
authoritative backend contract. Acceptance is restart-safe local publication
with no visible partial object and no per-Data metadata row.

### Slice 3: NDNSF Control and Segmented Data Plane

Deliver one collaboration-based queued store assignment, pipelined segmented transfer,
backpressure/retransmission, multi-replica receipts, and control-count
invariants. Acceptance is a deterministic MiniNDN transfer with control
operations independent of bytes and packet count.

### Slice 4: Public API and Compatibility

Deliver simple sync, async, advanced-session, progress/cancel/resume, and
capability APIs; retain exact-packet-v1; provide explicit negotiation,
migration, rollback, and mixed-version tests. Acceptance is two minimal apps
using only public interfaces plus legacy/new coexistence.

### Slice 5: Frozen Performance Campaign

Deliver the raw NDN baseline, measurement instrumentation, reproducible
MiniNDN matrix, derivation tooling, and evidence report. Acceptance requires
all admissible cells, all negative results, and verdicts against thresholds
frozen in the spec—not post-result tuning.

Each slice includes its tests and evidence update as one cohesive behavioral
task unless a real dependency boundary requires a split.

## Migration and Rollback

1. Add capability advertisement before publishing the new format.
2. Keep exact-packet-v1 read/write behavior unchanged.
3. Write all new-format objects under an unambiguous versioned identity.
4. Never reinterpret or rewrite old objects in place.
5. Activate new publication only for capable selected replicas.
6. Retain format identity in catalog, receipts, GC, and metrics.
7. Rollback disables new publication while preserving new-format bytes and
   metadata for a compatible reader or later roll-forward.
8. Any destructive migration requires a separate explicit operator action and
   is outside this feature's automatic upgrade path.

## Security and Threat Model

The implementation and audit must cover:

- malicious or compromised publisher;
- untrusted cache or repository returning substituted bytes;
- manifest bombs, cycles, oversized pages, excessive depth, and hash-work DoS;
- algorithm confusion and signature/digest downgrade;
- name, policy-epoch, manifest-root, size, and resume-state substitution;
- replayed begin/commit/receipt operations;
- partial-object discovery and time-of-check/time-of-use races;
- deduplication provenance confusion;
- advisory-ACK staleness, queue overflow, transfer-session, and GC races;
- replica equivocation or false durability;
- certificate expiration/revocation after caching;
- resource exhaustion through Interests, retransmissions, partial files, or
  concurrent sessions.

Existing NDNSF permissions, NAC-ABE routing, user/provider tokens, provider
permission, and replay protection remain mandatory on control operations.

## Verification Strategy

1. Static contract and schema checks.
2. Unit tests for names, identity, algorithms, manifests, bounds, lifecycle,
   progress maps, receipts, and backend failure injection.
3. Local integration tests for producer/fetcher, atomic persistence, resume,
   dedupe, compatibility, migration, and rollback.
4. MiniNDN security and recovery tests with small deterministic payloads.
5. MiniNDN throughput preflight and frozen formal matrix.
6. Optional TigerCluster application demonstration only after local gates.

The campaign follows [experiment-plan.md](experiment-plan.md). At least five
measured repetitions close engineering acceptance; a separate pilot-derived
sample size is required for paper-level inferential claims.

## Audit-Blocking Remediation Addendum

T019 retained one measured failure and two inconclusive performance criteria.
The feature therefore continues after the original implementation tasks:

1. remove benchmark-only per-consumer process creation from signed-root
   verification by using the same in-process cryptographic substrate as NDNSF;
2. connect the public artifact backend to the generic NDNSF delayed-planning
   Collaboration path rather than measuring only the Data plane;
3. add cold retrieval and canonical payload/metadata/Data-wire/Interest-wire
   accounting;
4. freeze a distinct replacement MiniNDN campaign and preserve the original
   campaign byte-for-byte;
5. repeat the code-aware audit before authorizing TigerCluster.

The remediation may improve implementation or measurement fidelity, but it
MUST NOT change frozen thresholds, delete negative samples, relabel the first
campaign, or treat a point estimate as inferential evidence.

## Project Structure

### Documentation (this feature)

```text
specs/164-distributed-repo-large-artifact-transport/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── experiment-plan.md
├── quickstart.md
├── checklists/
│   └── requirements.md
└── contracts/
    ├── high-level-api.md
    ├── artifact-manifest-v2.md
    ├── transfer-lifecycle.md
    ├── storage-backends.md
    └── performance-evidence.md
```

### Source Code (repository root)

```text
NDNSF-DistributedRepo/
├── include/ndnsf-distributed-repo/
│   ├── RepoTypes.hpp
│   ├── RepoStoreBackend.hpp             # planned authoritative contracts
│   ├── ArtifactManifest.hpp             # planned manifest/identity types
│   └── ArtifactTransfer.hpp             # planned transfer interfaces
├── src/
│   ├── RepoStoreBackend.cpp
│   ├── ArtifactManifest.cpp
│   ├── ArtifactTransfer.cpp
│   └── backends/                         # planned payload/metadata backends
└── pythonWrapper/
    ├── src/                              # native public bindings
    └── py_repoclient/
        └── orchestration.py              # migration from direct authority

pythonWrapper/
├── ndnsf/                                # generic collaboration APIs reused
└── src/ndnsf/

tests/
├── unit-tests/                           # C++ manifest/backend/control tests
├── python/                               # API/lifecycle/compatibility tests
└── container/                            # bounded integration profiles

Experiments/
└── NDNSF_DistributedRepo_Artifact_Minindn.py   # planned formal harness
```

**Structure Decision**: Extend the existing DistributedRepo module and generic
NDNSF collaboration surface. Do not introduce repository logic into
NDNSF-DistributedInference. New source filenames above are planning targets;
existing ownership and naming must be rechecked with CodeGraph immediately
before edits.

## Complexity Tracking

No constitution violation requires an exception. The payload/metadata split is
necessary because large immutable byte I/O and transactional lifecycle metadata
have different scaling, crash, and migration requirements; it also replaces,
rather than adds to, duplicated persistence authority.

## Post-Design Constitution Check

**PASS**. Phase 1 preserves the canonical runtime and security path, keeps DI
out of repository ownership, defines rollback and negative tests, uses
MiniNDN-first verification, and organizes future tasks by complete behavioral
outcomes.
