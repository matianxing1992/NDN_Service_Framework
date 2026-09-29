# Implementation Plan: Placement Observation Collection and Planning

**Branch**: `Experimental` | **Date**: 2026-09-28 | **Spec**: [spec.md](spec.md)

## Summary

Connect existing Core production observations to a separately versioned native DI offer, build one request/attempt-bound observation snapshot, and add a deterministic edge-aware native placement policy. Core remains responsible for generic transport samples; DI remains responsible for role graph, residency, cache and placement semantics. If the frozen request profile is V3/default, keep the existing path. If a V4 offer authenticates but its optimization observations are missing, stale or unusable, retain V4 and apply baseline ranking semantics to its authenticated non-network fields. This plan describes future work; this turn changes documentation only.

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

**Result**: `DRAFT`. Source-level ownership is plausible and the target behavior is specified, but exact V4 canonical bytes, key-policy binding, current endpoint epoch source, target API/PDF edits and test registration are locked only after T001's semantic/source audit. No principle is changed.

## Design Binding

Detailed field/flow/failure contracts are in [placement-observation-v1.md](contracts/placement-observation-v1.md) and [data-model.md](data-model.md). Planned key deltas are:

| Owner | Existing boundary | Planned change |
|---|---|---|
| Core | `NetworkTelemetryStore` already records User ACK RTT and Provider dependency fetch facts; generic ACK `peerMetrics` transmits one row. | Expose bounded typed snapshots to native User/Provider callers and complete User-side real data-transfer sampling without relabelling ACK RTT. Keep fields and policy generic. |
| DI Provider | readiness collector samples resources/capacity and can put them in a generic readiness payload; when V3 is issued, that payload is replaced and V3 offer generator writes zeros/empty arrays. | Add a distinct V4 offer builder that consumes fresh Core/DI owner snapshots, emits only request-candidate residency and directed observations, and signs canonical bytes. V3 files/bytes stay compatible. |
| DI User | V3 admission verifies Core and Provider identity/signature; V3 proposal binds ACK closure but no observation digest. | Negotiate a placement profile for the complete candidate roster: use V4 only if every candidate declares support, otherwise keep the whole request on V3/default. Validate V4, form an immutable snapshot, feed an observation-aware strategy, and bind its digest/cost summary into the resulting sealed proposal. |
| DI placement | V3 role placement applies hard checks, then preferred/distinct groups and per-role reuse/queue/RTT/bandwidth order. | Add a separate V4 strategy. Preserve feasibility and the current preferred/distinct choice groups; within the selected group, search whole assignments using request graph edges and real transfer estimates. |
| Design docs | Current/target API and design chapters describe V3 and the current source snapshot. | T001 freezes accepted target signatures and updates the target API/contract/design documents and PDFs before implementation. Current docs continue to describe source until code changes. |

## Planned Strategy

1. Build candidates only from admitted, signed, current offers. Run the existing identity, admission, backend, device, memory, role, reservation and cancellation checks first. Preserve the current preferred-provider and distinct-provider choice groups; V4 compares complete assignments inside the group selected by existing semantics.
2. Use a deterministic bounded beam over roles in stable graph topological order. Each partial assignment carries its role/provider/device map, reservations and measured cost for closed role-dependency edges. Rank complete assignments by `controlPathMs + dataFlowCriticalPathMs`; `controlPathMs` is the maximum observed User `ACK_RTT` among distinct assigned Providers, while `dataFlowCriticalPathMs` is the longest role-DAG path of node preparation/queue and directed data-transfer costs. The exact beam width and `maxAssignmentStates` are V4 strategy inputs frozen at T001. `NativeCandidateBudget.maxPolicyMs` and the request deadline are hard time bounds; exhausting either falls back to the existing strategy, never returning a partial assignment.
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

Names marked planned are not claims that these files or interfaces currently exist. The known integration executable is the existing `integration-tests` target; the planned named suite is `Spec194PlacementObservationContract` in the C++ file above. T001 must verify the exact registration/source closure and may refine private names while preserving ownership and external behavior.

Profile negotiation is one request-level decision made after the eligible candidate roster is fixed. T001 must locate the authenticated capability advertisement in the actual discovery/ACK path. T003 adds V4 capability there without changing V3 canonical offer bytes; T004 selects exactly one profile for the whole roster. A mixed-capability roster uses the existing V3/default path and may not emit mixed offer versions within one request or attempt.

## Work Units and Gates

Implementation is strict-serial. Each task contains its production change, focused C++ regression, API/behavior documentation and evidence. Each small task receives the repository read-only `review-agent` static gate before the shared batch build. The batch records five coverage lanes, stable exit, exact target/source closure, review-agent baseline/path/digest, and `static` / `compile-link` / `runtime-test` / `unobserved` misses. Shared build/test runs only after all member reviews and batch composition review pass.

**Provisional Batch ID**: `B194-NATIVE-OBSERVATION-PLACEMENT`, members T002–T005. They form one source-to-proposal behavior path under one observation contract and planned C++ oracle suite. **Batch growth decision**: retain these members together only if T001 verifies the actual Core/DI target and source closure supports the shared oracle; if Core and DI require separate build/source closures, split the batch and update this plan before implementation. T008 remains a separate system-qualification gate and is not absorbed into this native batch. T001 is a hard design gate; T002–T005 are not `READY_FOR_IMPLEMENTATION` until this composition is frozen.

**Batch observable exit**: for the same frozen request/candidate identity, the named C++ production path emits either (a) a fully verified selected profile, fresh snapshot digest, deterministic cost breakdown and complete valid proposal, or (b) the explicit V3/default or V4-baseline fallback with a stable reason. All member static reviews and the batch composition review pass; named target/source closure compiles/links; independent C++ selectors pass. This exit does not imply execution, terminal response, cleanup or latency improvement.

| Lane | Coverage obligation |
|---|---|
| Source / identity / catalog | Core producer samples, signer identity, request/candidate/attempt digest, endpoint boot/topology epoch. |
| Prepare / material / Repo | No model prefetch or assembly before Selection; exact residency claims and missing-material behavior. |
| ACK / Selection | Core ACK proof plus V4 signature, freshness, duplicate/direction rejection, snapshot digest and fallback reason. |
| Placement fetch / assembly | role graph edge mapping, byte estimates, cost decomposition, selection-bound fetch and no cross-attempt reuse. |
| Runner / execute / terminal / cleanup | actual multi-Provider native request, terminal outputs, process/cache cleanup and paired latency comparison. |

**Feature qualification exit**: both baseline and V4 produce verified placement proposals from the same request/candidate identity; all edge/rank/resource decisions are visible in the native trace; real execution reaches terminal response and cleanup; latency report binds code, installed binaries, model, workload, topology, observation snapshot and strategy identity. This complete exit belongs to T008, beyond native batch closure.

**Requirement traceability**:

| Requirement / outcome | Owning task(s) |
|---|---|
| FR-001–FR-002, SC-001 | T002, T003, T004 |
| FR-003–FR-004 | T003 |
| FR-009 whole-roster profile decision | T003, T004 |
| FR-010–FR-011 | T003 |
| FR-005, SC-003 | T004 |
| FR-006–FR-008, SC-002, SC-005 | T005, T006 |
| FR-012 | T006–T008 |
| SC-004 | T008 |
| SC-006 | T005, T006, T008 |

**Batch closure**: source audit has no unresolved identity/wire/owner gaps; static gate passes; named C++ target compiles/links and focused selectors pass; batch composition review passes; complete `static`, `compile-link`, `runtime-test`, and `unobserved` retrospective is recorded. MiniNDN/SC-004 remains a later qualification gate and is not implied by batch closure.

**Risk class / Dynamic profile**: production code will touch signed parsing, native caches and async NDN collection. T001 must select a justified C++ dynamic profile and record ownership/lifetime invariants for the actual batch; no sanitizer/fuzz run is requested in this documentation turn.

## Pre-Qualification Design-Code Convergence

**Design authority**: `spec.md`, `data-model.md`, `contracts/placement-observation-v1.md`, and the source/ownership decisions frozen by T001.<br>
**Production paths to inspect**: `ServiceUser` ACK/data transfer; `ServiceProvider` dependency fetch and generic ACK; V4 offer issuer/admission; native request planner; placement proposal/sealer; Selection-bound fetch; Provider assembly/execute/terminal/cleanup.<br>
**Required audit artifact**: `specs/194-placement-observation-planning/evidence/design-code-convergence.md` with five-lane traceability, severity/owner, focused regression and first observable boundary.<br>
**Closure rule**: `PASS` only after every controlling discrepancy is repaired, focused C++ regression passes and code review confirms actual production wiring. Any semantic, auth, ownership, source-closure or evidence gap is `BLOCK`.<br>
**Formal validation boundary**: T008 real installed MiniNDN and comparative latency.<br>
**Re-audit triggers**: changes to signed offer bytes, telemetry source/TTL, endpoint identity, role graph/byte estimate, candidate ordering, strategy budget, caller, tests, build target, installed binary or evidence contract.

## Complexity Tracking

No constitution principle is weakened. Versioned V4 adds a new signed wire and deterministic fallback to protect existing V3. The potential added Core API and V4 plumbing are justified only if T001 confirms actual production sample reachability and T008 proves end-to-end value; otherwise Spec194 remains `BLOCKED / NOT COMPLETE` instead of keeping unused protocol machinery.
