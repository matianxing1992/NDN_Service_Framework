# Implementation Plan: Placement Observation Collection and Planning

**Branch**: `Experimental` | **Date**: 2026-09-28 | **Spec**: [spec.md](spec.md)

## Summary

Connect existing Core production observations to a separately versioned native DI offer, build one request/attempt-bound observation snapshot, and add a deterministic edge-aware native placement policy. Core remains responsible for generic transport samples; DI remains responsible for role graph, residency, cache and placement semantics. If the frozen request profile is V3/default, keep the existing path. If a V4 offer authenticates but its optimization observations are missing, stale or unusable, retain V4 and apply baseline ranking semantics to its authenticated non-network fields.

## Technical Context

**Language/Version**: C++17 production path; existing Python telemetry helpers remain offline/legacy support only.<br>
**Primary Dependencies**: Core `NetworkTelemetryStore`, DI V4 offer/admission, current native planner and placement strategy.<br>
**Storage**: Bounded process-local observations plus request-scoped immutable snapshot; no persistent raw model/cache inventory.<br>
**Testing**: Named C++ production selectors/oracles and native `integration-tests`; complete Multi-Provider acceptance uses installed-system MiniNDN. No tests or experiments are run while authoring this Spec.<br>
**Target Platform**: Linux NDN service nodes and User process.<br>
**Performance Goals**: Decisions stay within caller-provided policy/deadline budget; p50 end-to-end improvement target is in SC-004.<br>
**Constraints**: No V3 canonical-wire change; no authorization bypass, pre-Selection material fetch, arbitrary cache enumeration, fabricated defaults, or unbounded probing/search.<br>
**Scale/Scope**: Sparse directed links for current request candidates and role edges, not a network-wide all-pairs inventory.

## Constitution Check

**Applicable principles**:

- **G1**: Observation snapshots and selection remain named, request/attempt-bound objects; a local timer or diagnostic does not substitute for protocol identity.
- **G2**: Core owns generic sample collection and delivery; DI owns graph/candidate/cache meaning and strategy. Python does not become a second native planner.
- **G3**: Core ACK authentication, Provider offer signature, policy authorization, current Selection, residency proof and numerical correctness remain separate checks.
- **G4**: Sampling and search have bounded sample/byte/time budgets, explicit freshness, cancellation and cleanup.
- **G5**: Snapshot, provider boot/process/topology epochs, content digests and request attempt are distinct and bound; cached content does not waive current authorization.
- **G6**: Static source review, focused C++ behavior, MiniNDN lifecycle and comparative latency evidence remain distinct.
- **D1–D4**: C++ owns preparation/planning/authorization/execution/terminal; metadata collection cannot prefetch selected material; resource lifetimes stay bounded; DI owns role/tensor dataflow semantics.
- **M1–M3**: Any future MiniNDN gate uses system-installed NFD, NDNSF binaries, libraries, applications and workers, with actual paths/hashes recorded. No build-tree candidate may be used for qualification.

**Result**: `DRAFT`. Source-level ownership is plausible and the target behavior is specified. Each implementation task must close its own source-backed API, identity, wire, owner, and failure-flow decisions before changing the affected code. There is no separate pre-implementation documentation phase; test/source closure is checked in the shared native gate. No principle is changed.

## Design Binding

Detailed field/flow/failure contracts are in [placement-observation-v1.md](contracts/placement-observation-v1.md) and [data-model.md](data-model.md). Planned key deltas are:

| Owner | Existing boundary | Planned change |
|---|---|---|
| Core | `NetworkTelemetryStore` already records User ACK RTT and Provider dependency fetch facts; generic ACK `peerMetrics` transmits one row. | Expose bounded typed snapshots to native User/Provider callers and complete User-side real data-transfer sampling without relabelling ACK RTT. Keep fields and policy generic. |
| DI Provider | readiness collector samples resources/capacity and can put them in a generic readiness payload; when V3 is issued, that payload is replaced and V3 offer generator writes zeros/empty arrays. | Add a distinct V4 offer builder that consumes fresh Core/DI owner snapshots, emits only request-candidate residency and directed observations, and signs canonical bytes. V3 files/bytes stay compatible. |
| DI User | V3 admission verifies Core and Provider identity/signature; V3 proposal binds ACK closure but no observation digest. | Negotiate a placement profile for the complete candidate roster: use V4 only if every candidate declares support, otherwise keep the whole request on V3/default. Validate V4, form an immutable snapshot, feed an observation-aware strategy, and bind its digest/cost summary into the resulting sealed proposal. |
| DI placement | V3 role placement applies hard checks, then preferred/distinct groups and per-role reuse/queue/RTT/bandwidth order. | Add a separate V4 strategy. Preserve feasibility and the current preferred/distinct choice groups; within the selected group, search whole assignments using request graph edges and real transfer estimates. |
| Design docs | Current/target API and design chapters describe V3 and the current source snapshot. | Keep current docs code-faithful. When an implementation changes an API, observable behavior, or security contract, update only the affected contract/design material in that same code task; do not create a standalone documentation task. |

## Documentation Policy

Implementation tasks must change product code or perform named production C++ behavior verification. Update a contract or design/API reference only when the task changes an interface, observable behavior, or security contract, and include that update in the same implementation task. Keep task status and execution evidence in the existing registry/evidence artifact; do not create separate API, PDF, traceability, or evidence-only tasks. A blocking unresolved contract is closed by source-backed work inside its owning implementation task before the affected code is changed.

## Planned Strategy

1. Build candidates only from admitted, signed, current offers. Run the existing identity, admission, backend, device, memory, role, reservation and cancellation checks first. Preserve the current preferred-provider and distinct-provider choice groups; V4 compares complete assignments inside the group selected by existing semantics.
2. Use a deterministic bounded beam over roles in stable graph topological order. Each partial assignment carries its role/provider/device map, reservations and measured cost for closed role-dependency edges. Rank complete assignments by `controlPathMs + dataFlowCriticalPathMs`; `controlPathMs` is the maximum observed User `ACK_RTT` among distinct assigned Providers, while `dataFlowCriticalPathMs` is the longest role-DAG path of node preparation/queue and directed data-transfer costs. The exact beam width and `maxAssignmentStates` are V4 strategy inputs closed and implemented in T004. `NativeCandidateBudget.maxPolicyMs` and the request deadline are hard time bounds; exhausting either falls back to the existing strategy, never returning a partial assignment.
3. Node cost includes only observed queue/wait and explicit residency preparation/load estimates. Exact loaded/assembled/canonical state must match request digests and current boot/process/device context. `missingVerifiedBytes` is a residency fact, not a time estimate without a matching data-transfer source.
4. For directed graph edges use `NativeGraphEdge.tensor.estimatedBytes` only when present and validated; do not reinterpret `NativeDependencySpec.expectedBytes == 0` as known zero. Use actual `DATA_FETCH` samples for byte-transfer estimates. `ACK_RTT` remains a request/ACK round trip that includes Provider work; it contributes only to `controlPathMs`, never to bulk transfer rate. Count a User ingress/egress edge only when the plan declares an actual transfer and its size is known.
5. The result is exact for oracle graphs whose complete assignments fit inside the beam/state bound; for larger graphs it is a deterministic bounded heuristic and must not claim a global optimum. If the request negotiated V3/default, retain that path. If V4 was selected but observations/byte sizes are unknown, stale or identity-mismatched, or no complete scored assignment fits the limits, keep the V4 request profile and apply current baseline ranking semantics to authenticated non-network V4 fields; record the fallback reason. Authentication/admission failures still reject the affected provider and never become usable baseline input. Never substitute constants or mix/downgrade offer schemas.

## Project Structure

```text
ndn-service-framework/NetworkTelemetry.hpp/.cpp
ndn-service-framework/ServiceUser.hpp/.cpp
ndn-service-framework/ServiceProvider.hpp/.cpp
NDNSF-DistributedInference/cpp/ndnsf-di/
  NativeProviderOfferV4.hpp/.cpp          # planned signed wire production
  NativeObservedOfferV4.hpp/.cpp          # planned strict decoder/value types
  NativeOfferAdmissionV4.hpp/.cpp          # planned identity/signature/freshness gate
  NativePlacementObservation.hpp/.cpp      # planned immutable snapshot builder
  NativeObservationAwarePlacement.hpp/.cpp # planned V4 heuristic
  NativeRequestEnvelope / NativeRequestPlanner / NativePlanSealer # planned profile, snapshot and proposal binding
examples/DI_NativeProviderExecutable.cpp  # planned real ACK/collector wiring
tests/integration-tests/spec194-placement-observation.t.cpp # planned Boost.Test C++ oracle
tests/wscript                                 # register in existing integration-tests target
specs/194-placement-observation-planning/  # contract, evidence, progress
Design/                                     # current/target/API synchronization
```

Names marked planned are not claims that these files or interfaces currently exist. The known integration executable is the existing `integration-tests` target; the planned named suite is `Spec194PlacementObservationContract` in the C++ file above. T001 verifies the Core test registration and source closure while implementing the telemetry boundary; T005 verifies the combined native source closure and selectors. Private names may be refined while preserving ownership and external behavior.

Profile negotiation is one request-level decision made after the eligible candidate roster is fixed. T002 locates and adds the authenticated capability advertisement in the actual discovery/ACK path without changing V3 canonical offer bytes. T003 selects exactly one profile for the whole roster. A mixed-capability roster uses the existing V3/default path and may not emit mixed offer versions within one request or attempt.

## Work Units and Gates

Implementation is strict-serial. T001–T004 each change one production boundary, add its named C++ assertions, and pass a read-only task review before the next task. Apply the Documentation Policy above only when the owning contract changes. Their native target and selectors are not built or run separately; the shared native build/test runs once in T005 after every member review and the combined composition/convergence review pass. This avoids repeating the same target run for each slice.

**Provisional Batch ID**: `B194-NATIVE-OBSERVATION-PLACEMENT`, members T001–T004. They form one source-to-proposal behavior path under one observation contract and planned C++ oracle suite. **Batch growth decision**: T001 records the actual Core target/test source closure as part of its implementation; T005 confirms the combined Core/DI closure and shared oracle. If separate closures require separate gates, update the batch boundary before running the shared native tests. T006 is the separate installed-system qualification gate. Each task closes its own Design binding before modifying the affected code; T005 closes the shared batch before qualification.

**Batch observable exit**: for the same frozen request/candidate identity, the named C++ production path emits either (a) a fully verified selected profile, fresh snapshot digest, deterministic cost breakdown and complete valid proposal, or (b) the explicit V3/default or V4-baseline fallback with a stable reason. All member reviews and the combined batch review pass; the named target/source closure compiles/links; independent C++ selectors pass. This exit does not imply a real multi-Provider request, terminal response, cleanup or latency improvement.

| Lane | T005 native batch coverage | T006 qualification coverage |
|---|---|---|
| Source / identity / catalog | Core producer samples, signer identity, request/candidate/attempt digest, endpoint boot/topology epoch. | Verify the same identities in the installed run. |
| Prepare / material / Repo | Static trace and C++ contract checks for no pre-Selection model prefetch/assembly, exact residency claims and missing-material behavior. | Observe Prepare/Repo publication and prove no unselected Provider fetches or assembles material. |
| ACK / Selection | Core ACK proof plus V4 signature, freshness, duplicate/direction rejection, snapshot digest and fallback reason. | Observe production ACK closure and selected profile/Providers. |
| Placement fetch / assembly | Role graph edge mapping, byte estimates, cost decomposition, selection-bound fetch and no cross-attempt reuse. | Observe placement-bound fetch and Provider assembly for assigned layers. |
| Runner / execute / terminal / cleanup | Trace actual production owners; runtime behavior not reached by the proposal oracle is recorded as `unobserved`. | Observe execution, terminal response and cleanup for the full production request. |

**Feature qualification exit**: both baseline and V4 produce verified placement proposals from the same request/candidate identity; all edge/rank/resource decisions are visible in the native trace; real execution reaches terminal response and cleanup; latency report binds code, installed binaries, model, workload, topology, observation snapshot and strategy identity. This complete exit belongs to T006, beyond native batch closure.

**Requirement traceability**:

| Requirement / outcome | Owning task(s) |
|---|---|
| User Story 1 - Trustworthy directed observations | T001–T003 |
| User Story 2 - Request-scoped cache and capacity view | T002–T003 |
| User Story 3 - Global role placement uses observed transfer cost | T004–T006 |
| FR-001 | T001–T003 |
| FR-002 | T001–T003 |
| FR-003 | T002 |
| FR-004 | T002 |
| FR-005 | T003 |
| FR-006 | T004–T005 |
| FR-007 | T004–T005 |
| FR-008 | T004–T005 |
| FR-009 | T002–T003 |
| FR-010 | T001–T003 |
| FR-011 | T001–T003 |
| FR-012 | T005–T006 |
| SC-001 | T001–T003 |
| SC-002 | T004–T005 |
| SC-003 | T003, T005 |
| SC-004 | T006 |
| SC-005 | T004–T005 |
| SC-006 | T004–T006 |

**T005 closure**: the combined read-only composition/convergence review has no unresolved controlling identity, wire, owner or lifecycle gaps; the named C++ target compiles/links and its independent selectors pass; the stable exit and complete five-lane `static` / `compile-link` / `runtime-test` / `unobserved` retrospective are recorded in one `evidence/native-batch-closure.md`. A blocking finding prevents the build and keeps T005 open; repair is a separately registered dependency-ordered work unit, never an untracked fix inside the verification gate. MiniNDN/SC-004 remains T006 and is not implied by batch closure.

**Risk class / Dynamic profile**: production code will touch signed parsing, native caches and async NDN collection. T005 selects a justified C++ dynamic profile and records ownership/lifetime invariants for the actual batch; no sanitizer/fuzz run is requested in this task-structure review.

## T005 Full-Path Convergence and Batch Closure

**Design authority**: `spec.md`, `data-model.md`, `contracts/placement-observation-v1.md`, and the source/ownership decisions closed by T001–T004.<br>
**Production paths to inspect**: `ServiceUser` ACK/data transfer; `ServiceProvider` dependency fetch and generic ACK; V4 offer issuer/admission; native request planner; placement proposal/sealer; Selection-bound fetch; Provider assembly/execute/terminal/cleanup.<br>
**Single closure artifact**: `evidence/native-batch-closure.md` holds the five-lane trace, severity/owner findings, first observable boundary, review-agent baseline/path/SHA, exact target/source closure, stable batch exit, and four-category retrospective. Do not create a second convergence report.<br>
**Order**: member static reviews → combined full-path/composition review → if clear, one shared native build and selector run → closure record. T005 itself is read-only with respect to production code. If review finds a controlling gap, stop before build; register a scoped repair work unit, update dependencies, complete its static gate, then resume the affected shared validation.<br>
**Closure rule**: `PASS` only when the review finds no controlling discrepancy and the named C++ target, independent oracles and retrospective all pass. Any semantic, authorization, ownership, source-closure or evidence gap is `BLOCK`.<br>
**Formal validation boundary**: T006 real installed MiniNDN and comparative latency.<br>
**Re-audit triggers**: changes to signed offer bytes, telemetry source/TTL, endpoint identity, role graph/byte estimate, candidate ordering, strategy budget, caller, tests, build target, installed binary or evidence contract.

## Complexity Tracking

No constitution principle is weakened. Versioned V4 adds a new signed wire and deterministic fallback to protect existing V3. The potential added Core API and V4 plumbing are justified only if T001 confirms actual production sample reachability and T006 proves end-to-end value; otherwise Spec194 remains `BLOCKED / NOT COMPLETE` instead of keeping unused protocol machinery.
