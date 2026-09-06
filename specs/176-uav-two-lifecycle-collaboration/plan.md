# Implementation Plan: UAV Two-Lifecycle Collaboration

**Branch**: `UAV-Experimental` | **Date**: 2026-08-27 | **Spec**: [spec.md](spec.md)

**GSD Index**: Phase 37, `.planning/phases/37-uav-two-lifecycle-collaboration/`

**Input**: Feature specification from
`/specs/176-uav-two-lifecycle-collaboration/spec.md`

## Summary

Extend `NDNSF-UAV-APP` with an application-owned long-lived MissionSession and
bounded request-scoped CollaborationJobs. The Ground Station keeps patrol state,
stream bindings, incident history, and compensation across many finite NDNSF
requests. An incident creates one collaboration plan with evidence-producing
scout roles and one compute-capable `DetectorReporter` terminal role. Evidence
travels as exact named Data, not inline image payloads. Existing NDNSF
collaboration, Stream, Targeted, permission, token, and large-data contracts are
reused unchanged unless testing proves a separate Core defect.

## Technical Context

**Language/Version**: C++17 for UAV application and native tests; Python 3 for
detector tooling, orchestration probes, and analysis.

**Primary Dependencies**: Existing repository-pinned NDNSF Core, ndn-cxx,
NDN-SVS, NAC-ABE/security stack, NFD/MiniNDN, Boost 1.71 system toolchain, Qt UAV
GUI, GStreamer video path, ONNX Runtime detector path, and current MAVLink mock/
PX4 SITL adapters. No dependency upgrade belongs to this feature.

**Storage**: Bounded Ground Station mission ledger in application memory for the
MVP, with deterministic, versioned snapshot/restore serialization; signed named
evidence in the existing NDNSF large-data/stream retention path. The MVP does
not promise automatic crash-proof persistence. After restart, a valid snapshot
enters recovery and blocks new controls until vehicle and stream state have been
reconciled; missing or invalid snapshots require explicit operator recovery.

**Testing**: The required progression is ordered: Boost unit tests, CPU/in-process
integration tests, real multi-process MiniNDN experiments, then PX4 SITL. Focused
Python contract checks support the applicable gate; optional hardware tests are
allowed only after all four software gates pass.

The existing NDNSF-DI test organization is a pattern for this separation:
contract/unit checks remain independent from process/container integration
launchers, and each launcher emits candidate-bound evidence. Spec176 reuses that
discipline without importing DI-specific protocols, models, or deployment
artifacts into the UAV application.

**Target Platform**: Linux Ground Station and UAV companion computers; local
mock profile first, MiniNDN network profile second, PX4 SITL third.

**Project Type**: Existing C++ service-container application using a shared
NDNSF runtime library.

**Performance Goals**: Flight-control processing is isolated from detector and
video backpressure; ordinary requests and collaboration jobs remain bounded;
the registered incident profile completes within its declared deadline. No
latency advantage is claimed before measured evidence exists.

**Constraints**: No patrol-wide `RequestCollaboration`; no raw frame bytes in
service requests; exactly one terminal response owner; no blind retry of
ambiguous flight commands; no new UAV-specific Core wire messages; bounded
queues, evidence retention, role counts, retries, and deadlines.

**Scale/Scope**: MVP topology is one Ground Station, two scout UAVs, and one
compute-capable UAV. The design permits additional scouts through repeated
`EvidenceSource` roles, but the registered acceptance topology remains fixed.

## Constitution Check

### Pre-design Gate

- **Canonical Dynamic Runtime — PASS**: Reuses generic `RequestService`,
  `RequestServiceTargeted`, Stream, and `RequestCollaboration`; no generated
  stubs, split service names, or new framework-specific payload types.
- **Security In Data Path — PASS**: Preserves existing NAC-ABE routing,
  controller permissions, signed Data, tokens, provider permission, and replay
  protection.
- **CodeGraph First — PASS**: The fresh clone was indexed on 2026-08-27;
  `codegraph status .` reports 8,072 files, 180,719 nodes, 450,259 edges, and an
  up-to-date index. The API and build claims below were checked against current
  symbol definitions and call paths before this audit closed.
- **Spec-Driven Durable Work — PASS**: Spec 176 defines requirements, plan,
  tasks, boundaries, and validation before implementation.
- **Right-Scope Verification — PASS**: Unit/CPU integration precede MiniNDN and
  SITL; no hardware or performance claim is accepted from unit tests.
- **Cohesive Outcome-Based Tasks — PASS**: Each task below combines its
  test-first contract, implementation, focused validation, and evidence gate.
- **GSD Resumability — PASS**: `.planning/` was restored and validates healthy;
  this work is indexed as Phase 37 while Spec Kit remains canonical for feature
  requirements and tasks.

### Post-design Recheck

The proposed architecture adds only UAV-application state and adapters. Any
required NDNSF Core protocol change is a blocking discovery that must be moved
to a separate Core Spec and cannot be smuggled into this application phase.

## Grounded Current State

The existing UAV application already provides most building blocks:

- Patrol work is linked across independent requests with `patrol_task_id` and
  compensation of missing parts.
- Mission assignment uses shared `/UAV/Mission/Assign` and selective ACK state.
- Provider-specific MAVLink and video control use Targeted requests.
- Video and telemetry use independent streaming/service paths.
- Large images and clips are already documented as named large-data objects,
  not invocation payloads.
- Object detection is currently centered at `/UAV/GS/ObjectDetection`; drones
  request detection over compact metadata for a Ground-Station-decoded frame.
- NDNSF Core exposes bounded `RequestCollaboration`, deferred
  `BeginCollaboration`/`CommitCollaborationPlan`, exact role assignment,
  `CollaborationContext::publishLargeNamed()`, and `fetchLarge()`. A role carries
  `terminalResponseOwner`; Core requires exactly one for streamed collaboration,
  while the UAV application must validate the same invariant for every unary or
  streamed plan before commit and again when accepting a report.

The missing behavior is an explicit MissionSession state owner plus an
incident-level coordinator that uses the collaboration API to select a
compute-capable UAV and move immutable named evidence through a finite plan.

## Architecture

```text
Long-lived application lifecycle

Operator
   |
   v
Ground Station MissionSession
   |-- MissionPart A -- finite RequestService --> Scout A
   |-- MissionPart B -- finite RequestService --> Scout B
   |-- Targeted arm/takeoff/land -------------> selected UAV
   |-- Stream subscriptions <------------------ video/telemetry
   |-- Incident ledger
   `-- Compensation ledger

Short bounded collaboration lifecycle (created only for one incident)

Incident trigger
   |
   v
RequestCollaboration(service=/UAV/Incident/Analyze)
   |-- ACK closure: capabilities/readiness from eligible providers
   |-- committed plan
   |     |-- EvidenceSource[0] -> Scout A
   |     |-- EvidenceSource[1] -> Scout B (optional per incident)
   |     `-- DetectorReporter  -> Compute UAV (terminal owner)
   |-- exact named evidence fetch/publication
   `-- one End/Response -> terminal report -> MissionSession incident ledger
```

The MissionSession may create zero, one, or many CollaborationJobs during a
patrol. A CollaborationJob never owns the patrol, flight-control state, or
continuous stream lifetime.

## NDN Data-Centric Service Transaction Contract

The implementation follows the NDN architecture at the application boundary:

0. **Use precise terminology.** The canonical description is “data-centric
   service transaction,” not “data-driven service framework.” The latter is
   ambiguous with analytics/ML pipelines and is not a separate contribution.
   The contract below covers the content/evidence path while preserving the
   existing NDNSF control exchange.

1. **Name the data, not its location.** NDNSF control publications retain the
   existing V2 requester/provider service-semantic names. Provider discovery is
   the accepted, request-scoped ACK set. A caller may supply logical NDN Provider
   names as candidates (as the existing API permits), but no application
   contract contains an IP address, host, port, socket URI, or transport
   endpoint list. Capability selection must use the ACK metadata from that
   accepted set; a provider name alone never implies readiness or model ability.
2. **Keep application objects under producer-owned namespaces.** The registered
   logical schemas are:

   ```text
   /<producer>/UAV/MISSION/<missionId>/INCIDENT/<incidentId>/EVIDENCE/<evidenceId>/<version>
   /<detector>/UAV/MISSION/<missionId>/INCIDENT/<incidentId>/REPORT/<attemptId>/<version>
   ```

   The final component is an exact immutable version. Segments, when required,
   extend that versioned object name using the existing NDNSF/ndn-cxx segmented
   Data convention. Concrete component encodings are frozen in T003 without
   changing the existing NDNSF V2 control-name schema.
3. **Retrieve with Interests.** SVS may advertise control publication or stream
   mapping availability, but a consumer fetches named publications, mapping
   Data, samples, manifests, segments, and separately retained reports by name.
   This feature does not claim that ACK/SVS advertisements alone implement the
   official incomplete-name discovery principle; they only bootstrap a logical
   name or availability hint. The actual object retrieval remains Interest/Data.
   The application `push()` call is a local publisher API; it does not create a
   transport-level server push or endpoint session.
4. **Secure Data directly.** An accepted object is validated against the exact
   requested name, expected producer/signer, trust policy, immutable version,
   and content digest. The application adapter requires an explicit producer
   certificate when it receives a raw `ndn::Data`; the normal NDNSF fetch path
   must complete its configured `MessageValidator` before exposing bytes to the
   detector. Arrival from the expected face or process is not proof of
   authenticity. Both exact-segment retrieval and the SegmentFetcher path must
   use that configured trust-schema validator; `ValidatorNull` or a validation
   bypass is not a valid Spec 176 acceptance path. The deterministic
   multi-segment CPU regression records this validator-backed path; adverse
   multi-process behavior remains a deployment gate.
   An evidence reference created before publication is only a descriptor; the
   object becomes accepted evidence after signed Data publication and a
   successful configured-validator fetch. The application boundary makes this
   machine-checkable: `UavEvidenceReference` is descriptor metadata, while
   `UavVerifiedEvidence` is the only input accepted by detector execution.
   The Drone collaboration participant realizes this path with
   `CollaborationContext::fetchSignedExactData()` followed by
   `UavDetectorProvider::acceptValidatedContent()`; ordinary request payloads
   cannot bypass the validator-backed fetch.
5. **Use caching only where semantics allow.** Immutable evidence and reports
   are cache-safe within their freshness, retention, and authorization policy.
   Correctness cannot depend on a cache hit. Mission state and effectful
   flight-control commands are not cacheable authorization; one-time tokens,
   replay protection, and post-timeout reconciliation remain authoritative.

6. **Preserve NDN flow balance and discovery semantics.** ACK/SVS publications
   may provide a logical name or availability hint, but the consumer still
   issues bounded Interests for each evidence/report object. Exact-object fetch
   keeps one application-level Interest outstanding for an object at a time,
   while stream and segmented windows have finite limits. This prevents
   endpoint-style push or an unbounded retry loop from being mistaken for an
   NDN data path.

The Ground Station `MissionSession` is local application state that records
names and digests. It is not a connection, remote endpoint, or new persistent
NDN protocol session. “Data-centric service transaction” is recorded as a design
property and test obligation, not as an independent novelty claim about using
Data packets in isolation. The control plane remains the existing generic
Request/ACK/Selection/Response API.

The workflow is evidence-driven only in a bounded operational sense: the
NDNSF-validated ACK snapshot supplies capability facts for role selection, and
validator-produced named evidence drives detector execution. The application
selector treats `ackVerified` as a provenance assertion from `ServiceUser`, not
as a cryptographic check. Do not use “data-driven service framework” as a
generic ML or novelty claim; the end-to-end wording remains provisional until
the container and MiniNDN/SITL gates produce evidence.

For review purposes, the resulting claim is **NDN data-centric and narrowly
evidence-driven**. It is not a claim that this feature creates a general
data-driven/ML framework, a new NDN wire protocol, or a replacement for the
existing Request/ACK/Selection/Response control exchange.

The conformance boundary is explicit: this is **NDN-compatible application
behavior**, not a new router or a claim of complete implementation of every NDN
research principle. Principles 1--4 and 6 are implemented as application
contracts and have CPU/nominal evidence where noted; principle 5 (incomplete
name discovery) is deliberately not claimed. ACK/SVS advertisements only
bootstrap a logical name or availability hint in this feature, after which the
consumer issues an exact-name Interest. Multi-segment adverse runs and PX4 SITL
remain the evidence needed to upgrade the relevant contracts from implemented
to measured deployment behavior.

Normative architecture references for this interpretation are the official
[NDN Design Principles](https://named-data.net/project/ndn-design-principles/)
and [NDN Architecture Overview](https://named-data.net/project/archoverview/).

## Lifecycle Contracts

### MissionSession State Machine

```text
PLANNED
  -> STARTING
  -> ACTIVE
       -> DEGRADED       (one part/stream/provider unavailable)
       -> COMPENSATING   (missing parts only)
       -> CANCELLING
       -> RECOVERING     (restored process; controls blocked pending reconciliation)
  -> COMPLETED | CANCELLED | FAILED
```

Rules:

- State changes are application decisions backed by accepted finite NDNSF
  results or authoritative vehicle state.
- A request timeout records an attempt failure; it does not terminate the
  MissionSession unless mission policy decides the remaining work is impossible.
- Completed MissionParts are monotonic. Late results cannot move them backward.
- Stream restarts create a new StreamBinding/session while preserving previously
  accepted immutable evidence references.
- Snapshot restore never proves current physical state. `RECOVERING` permits
  observation and reconciliation but prohibits new flight-control actions until
  authoritative vehicle and stream state agree with the restored ledger.

### CollaborationJob State Machine

```text
CREATED
  -> ACK_COLLECTING
  -> ACK_CLOSED
  -> PLAN_COMMITTED
  -> SELECTED
  -> EVIDENCE_READY
  -> EXECUTING
  -> REPORTING
  -> SUCCEEDED | FAILED | TIMED_OUT | CANCELLED
```

Rules:

- One `requestId` identifies this finite job only.
- The role plan is immutable after commit.
- Exactly one selected `DetectorReporter` has terminal ownership.
- Internal evidence and progress events may precede the terminal response, but
  they do not extend the global deadline.
- A new attempt creates a new CollaborationJob/requestId and preserves lineage
  to the same incident and immutable evidence.

## API Mapping

| Application behavior | NDNSF mechanism | Lifetime/ownership |
| --- | --- | --- |
| Mission-part acceptance | `RequestService` on `/UAV/Mission/Assign` | One finite request; MissionSession owns progress |
| Arm/takeoff/land and selected-UAV control | `RequestServiceTargeted` | One finite command; reconcile state after ambiguity |
| Continuous video | Stream publisher/subscriber API; subscriber retrieves mapping/sample Data with bounded Interests | Independent stream session |
| Continuous telemetry | Existing stream or bounded status request | Independent of collaboration |
| Immutable frame/clip | Stream exact name or `publishLargeNamed`/large-data abstraction, retrieved by exact Interest | Producer-owned versioned and signed Data with retention |
| Incident analysis | Deferred `BeginCollaboration` -> `ACK_CLOSED` -> `CommitCollaborationPlan` on `/UAV/Incident/Analyze`; fixed plans may use `RequestCollaboration` | One bounded CollaborationJob |
| Role-local metadata/progress | `CollaborationContext::publish` | Internal to one job |
| Large collaboration object | `publishLargeNamed` and `fetchLarge` | Exact signed object, bounded by job/retention |
| Final detection report | One provider-named terminal `ResponseMessage`; exact report name/digest when independent retrieval is needed | Owned by `DetectorReporter`; no duplicate large bytes |

## Application Data Model

### MissionSessionRecord

```text
missionId
planDigest
operatorIdentity
state
createdAt / deadline / updatedAt
MissionPart[]
StreamBinding[]
IncidentRecord[]
CompensationRecord[]
lastAcceptedTransition
```

### MissionPartRecord

```text
partId
sector / waypointDigest
attemptId
assignedProvider
state: pending / accepted / executing / completed / missing / compensated
completedWaypointSet
responseDigest
authoritativeVehicleState
```

### IncidentRecord

```text
incidentId
missionId
triggerKind / triggerTime / location
requestedCapability
EvidenceReference[]
currentAttemptId
CollaborationJobRecord[]
acceptedTerminalReportDigest
```

### CollaborationJobRecord

```text
requestId
missionId / incidentId / attemptId
ackDeadline / globalDeadline
planDigest
RoleAssignment[]
terminalOwner
state / failureStage / failureReason
fallbackMode
terminalReportDigest
```

### EvidenceReference

```text
producerIdentity
streamId / streamSession
frameSequence or timeWindow
exactDataName or manifestName
exactVersion
contentDigest
contentType
retentionDeadline
```

### ProviderCapabilitySnapshot

```text
providerIdentity
modelId / modelDigest / qualityProfile
deviceClass
ready
queueDepth / estimatedStartMs
camera/evidence access capability
snapshotTime
```

## Provider Selection Policy

The registered MVP strategy is capability-first and request-scoped:

1. Reject invalid, unauthorized, stale, or negative ACKs.
2. Reject providers that cannot satisfy the requested model/quality profile.
3. Prefer ready compute-UAV providers over Ground Station fallback.
4. Rank feasible compute providers using declared readiness, queue/admission
   state, and deterministic provider-name tie breaking.
5. Use Ground Station fallback only when explicitly enabled and capability
   verified; record the fallback in the terminal report.
6. Fail closed when no feasible provider exists.

The strategy does not infer capability from an IP address, host, port, socket
URI, or transport endpoint list. It uses the signed request-scoped ACK
candidate set; candidate provider identities remain logical NDN names.

## Evidence Flow

1. A scout publishes continuous video and its stream/mapping Data under its own
   namespace; subscribers drive network retrieval with bounded Interests.
2. An incident trigger records bounded stream-window descriptors and any
   already available exact immutable evidence references; it does not pretend
   that a mutable live window is already frozen.
3. The collaboration request contains compact incident metadata, window
   descriptors, and existing evidence references only.
4. In the registered nominal case, an assigned EvidenceSource freezes and
   attests its requested window as an immutable exact-name/digest manifest and
   guarantees bounded retention. A plan omits this role when an already verified
   immutable manifest exists and no source-side action is required.
5. DetectorReporter issues Interests for the assigned exact names, verifies
   returned names, signatures/signers, policies, versions, and digests, runs the
   model, and publishes one provenance-complete report under its own namespace.
   A separately retained/segmented report is referenced by exact name and digest
   from the terminal Response instead of duplicating the report bytes.
6. The MissionSession accepts the report only if all lineage and terminal-owner
   fields match its current incident attempt.

## Failure and Recovery Matrix

| Failure | CollaborationJob action | MissionSession action |
| --- | --- | --- |
| No valid ACK | Fail with `NO_FEASIBLE_PROVIDER` | Keep patrol active; optionally schedule later attempt |
| Selected provider lost before evidence | Fail current job or use only the bounded replacement policy explicitly configured for that job | Preserve incident/evidence; create new attempt if policy permits |
| Evidence expired/missing | Fail with exact missing name and stage | Preserve patrol; request a new evidence capture if still useful |
| Detector execution fails | Publish bounded failure; no false terminal success | Optional idempotent retry under new job |
| Final response lost | Use existing bounded delivery semantics; do not run detector twice solely because caller missed response | Reconcile accepted report by exact name/digest if available |
| Scout lost during patrol | No effect on unrelated job unless it owns required evidence | Mark unfinished MissionParts missing; compensate only those parts |
| Flight command timeout | Not a CollaborationJob concern | Query vehicle state; never blind retry |
| Ground Station restarts | Active network jobs terminate or expire by their own deadlines | Restore a valid mission snapshot into `RECOVERING`, or require explicit operator recovery when no valid snapshot exists; reconcile streams/vehicles before new controls |

## Security and Safety Model

- Existing controller-issued encrypted permissions determine who may request and
  provide the new unified service names.
- REQUEST/SELECTION and ACK/RESPONSE retain current service/permission NAC-ABE
  routing.
- One-time UserToken/ProviderToken and replay checks remain enabled.
- Evidence and reports are signed under producer-owned names and digest-bound in
  assignments/results.
- Face, process, or transport origin is never an authorization fact; acceptance
  follows the Data name/signature/policy/digest contract.
- Cacheable immutable evidence/report Data is distinct from effectful Targeted
  command requests, whose tokens and reconciliation semantics remain one-time.
- Role assignment and terminal owner are included in the committed plan digest.
- Detector input is untrusted application data; parsers and model adapters must
  fail closed on malformed manifests, types, dimensions, or oversized objects.
- Flight-control authority remains outside collaboration roles. A detector result
  cannot directly issue MAVLink commands without the existing operator/safety
  command path.

## Project Structure

### Documentation (this feature)

```text
specs/176-uav-two-lifecycle-collaboration/
├── spec.md
├── plan.md
├── tasks.md
└── checklists/
    └── requirements.md
```

### Planned Source and Test Touchpoints

```text
NDNSF-UAV-APP/
├── shared/
│   ├── UavProtocol.hpp/.cpp                 # typed application records/wire payloads
│   ├── UavMissionSession.hpp/.cpp           # application mission/incident ledger
│   ├── UavCollaborationPolicy.hpp/.cpp      # role plan and capability selection
│   └── UavDetectorProvider.hpp/.cpp         # reusable detector execution adapter
├── ground-station/
│   ├── GroundStationRuntimeState.hpp        # operator-visible lifecycle state
│   ├── GroundStationServiceContainer.inc.hpp# coordinator integration
│   └── UavIncidentCoordinator.hpp/.cpp      # finite CollaborationJob owner
├── drone/
│   ├── DroneServiceContainer.inc.hpp        # evidence/detector role providers
│   └── UavCollaborationParticipant.hpp/.cpp # bounded role execution
├── configs/
│   ├── uav_runtime.conf
│   └── uav_demo.policies
├── tools/
│   └── run_uav_collaboration_probe.sh       # deterministic local/SITL probe
├── README.md
└── README_ch.md

tests/
├── unit-tests/
│   └── uav-two-lifecycle.t.cpp
├── integration-tests/
│   └── uav-collaboration-flow.t.cpp
└── python/
    └── test_uav_collaboration_campaign.py

examples/ndnsf/uav-collaboration/
└── minindn_uav_collaboration.py             # registered real-network profile

examples/wscript                              # register all new UAV app sources
tests/wscript                                 # register unit/integration sources
```

**Structure Decision**: Keep mission and collaboration semantics in UAV-APP;
reuse NDNSF Core unchanged. New shared classes isolate state and selection from
the existing large GUI/service-container include files, while narrow integration
adapters connect them to current Ground Station and Drone processes.

### NDNSF-DI Integration-Test Patterns Used as References

Spec176 should reuse the repository's established NDNSF-DI test organization,
not copy DI-specific roles or payloads. The relevant references are:

- `tests/integration-tests/ndnsf-integration-fixture.hpp/.cpp`: construct a
  complete in-process environment once (faces, identities, SVS, policies, and
  permissions), finish `bootstrap()`/readiness first, then create an isolated
  request scope. Its deterministic packet bridge and scoped drop/duplicate/
  reorder faults are the model for the Spec176 CPU gate.
- `tests/integration-tests/ndnsf-di-core-flow.t.cpp`: exercise production
  ingress rather than only helper methods, and cover ACK closure, selection,
  assignment, exact named-data fetch, role-split/multi-provider execution, and
  fail-closed cases such as missing output, tamper, stale attempts, and
  unavailable providers. Spec176 maps these checks to its CollaborationJob and
  MissionSession contracts while keeping UAV payloads application-owned.
- `tests/integration-tests/ndnsf-data-v1-svs-flow.t.cpp`: use manifests and
  exact segments to validate mapping, repair, replay fences, and reassembly.
  This is the reference for immutable UAV evidence and second-consumer fetches.
- `tests/integration-tests/invocation-stream-flow.t.cpp`: keep stream lifecycle
  tests separate from unary request tests and cover retry, reordering,
  duplicate suppression, tamper rejection, cancellation, and bounded consumer
  capacity. These cases inform stream independence during a timed-out incident.
- `scripts/run_spec175_python_gate.py`: keep the subject test list, artifact
  hashes, command/return-code records, and diagnostic-only historical tests
  explicit. Spec176 runners must use the same candidate-bound evidence rule.

The mapping is intentionally one-way: DI examples provide fixture, fault,
process, and evidence patterns; they do not add NDNSF-DI dependencies, native
model claims, or new Core wire types to UAV-APP.

## Implementation Sequence

### Stage A - Freeze Contracts and Baseline

- Add application data structures, explicit state transitions, serialization,
  and invariant tests without changing network behavior.
- Freeze service names, role names, capability fields, evidence reference fields,
  failure codes, and registered deadlines in configuration.
- Capture a current `UAV-Experimental` baseline for existing mission, Targeted,
  video, and object-detection regressions.

### Stage B - MissionSession Boundary

- Move patrol/incident/compensation ownership behind MissionSession APIs.
- Keep existing service calls finite and correlate them to mission records.
- Add snapshot/restore and stale-attempt rejection before network collaboration.

### Stage C - Compute-UAV Detector Provider

- Generalize the current detector worker so a Drone container can advertise the
  same application-level detector capability with verified model/device metadata.
- Preserve explicit Ground Station fallback.
- Introduce named evidence fetch and provenance-complete reports.

### Stage D - Bounded CollaborationJob Coordinator

- Build one finite incident request, collect capability ACKs, commit roles,
  select providers, fetch evidence, and accept one terminal report.
- Bind every callback to mission/incident/attempt/request/plan identities.
- Add bounded failure and optional idempotent retry policies.

### Stage E - Operator State and Recovery

- Expose MissionSession and CollaborationJob state separately in Ground Station
  runtime/UI state.
- Integrate mission compensation and command reconciliation without granting
  detector roles flight-control authority.

### Stage F - Validation Progression

The following sequence is mandatory and cumulative:

1. **Unit gate (G1)**: run `unit-tests`, including the typed operator-state
   regression and all lifecycle/security contracts.
2. **CPU integration gate (G2)**: run the in-process NDNSF integration fixture,
   including exact named multi-segment retrieval and the registered failure
   table. No MiniNDN result may compensate for a failed G2.
3. **MiniNDN gate (G3)**: run the real multi-process experiments only after G1
   and G2 pass. The Xvfb Ground Station GUI smoke is a bounded G3 profile; it
   proves operator rendering, not flight or SITL behavior.
4. **PX4 SITL gate (G4)**: run the two-lifecycle scenario only after G3 nominal
   and failure profiles pass.

Optional hardware validation remains outside this feature and requires a
separate safety approval after G1--G4.

## Validation Strategy

### Gate G0 - Existing Behavior Baseline

Run existing focused UAV mission, Targeted command, video, telemetry, security,
and collaboration tests. Record exact binary/commit/config hashes. No new code is
accepted if this baseline cannot be reproduced or its failures explained.
G0 is a read-only inventory/preflight gate; it does not replace G1 and does not
permit skipping the ordered unit -> CPU integration -> MiniNDN -> SITL sequence.

### Gate G1 - Unit Contracts

Verify MissionSession monotonicity, fresh request/attempt identity, state
transition legality, stale-result rejection, compensation of missing parts only,
capability ranking, exactly one terminal owner, evidence-reference validation,
producer-namespace/version rules, rejection of endpoint-address fields, direct
Data authentication, and command retry prohibition.

### Gate G2 - CPU Integration Fixture

Use the existing NDNSF integration fixture with deterministic evidence and a
small CPU detector. Cover the full Request -> ACK_CLOSED -> plan -> Selection ->
named evidence -> terminal Response path plus each registered failure stage. This
gate must pass before MiniNDN. The fixture must prove that tampered name/content,
wrong signer, stale version, and endpoint-bearing assignments fail closed, and
that a second consumer can retrieve the same immutable object without changing
its identity; a cache hit is permitted but not required.

### Gate G3 - Real MiniNDN

Run distinct processes and NFDs for Ground Station, two scouts, and compute UAV
only after G1 and G2 have passed. The optional Xvfb Ground Station GUI smoke is
recorded under this gate and must not be used to waive any CPU integration
failure.
Validate real permissions, provider discovery, signed named evidence retrieval,
selection, terminal ownership, loss/disconnect handling, and stream independence.
Capture the exact application names and the consumer Interest -> verified Data
stages for evidence and any independently retained report. Confirm that the plan
and payload contracts contain no IP/host/port/socket transport endpoint fields
(logical NDN names are allowed) and that effectful
Targeted commands are not satisfied or authorized as cached evidence.
Use a fixed topology/configuration and registered seeds. After smoke trace
correctness passes, the registered nominal acceptance uses a 60-second measured
window with multiple collaboration deadlines shorter than the mission window;
longer endurance runs are optional follow-up evidence.

### Gate G4 - PX4 SITL

Validate that an ongoing mission and video streams survive one successful and
one failed CollaborationJob; compensate a missing mission part; and reconcile an
ambiguous command without blind replay.

### Gate G5 - Documentation and Branch Gate

Verify English/Chinese UAV documentation agree, the feature remains on
`UAV-Experimental`, no unintended Core protocol changes exist, all evidence maps
to Spec 176 requirements, and promotion requires Tianxing's explicit decision.

## Registered Evidence Fields

Every integration/MiniNDN/SITL result must record:

- repository commit and dirty-state manifest;
- application/binary/library hashes;
- exact NDNSF, ndn-cxx, NDN-SVS, NFD, and detector runtime versions;
- topology, identities, policies, trust schema, service names, and role plan;
- mission, incident, attempt, request, plan, evidence, and report identifiers;
- exact requested/returned Data names, producer identities, immutable versions,
  and Interest/verification stages for evidence and retained reports;
- provider capabilities and selected/fallback provider;
- lifecycle timestamps and terminal/failure stage;
- model/input/output digests and execution device;
- stream session and evidence-retention settings;
- seed where randomness is used.

## Migration and Rollback

- New two-lifecycle behavior is enabled by an explicit UAV runtime profile until
  acceptance gates pass; existing patrol and Ground Station detection remain the
  fallback reference paths.
- Data-model changes are additive at first. Existing `patrol_task_id` is mapped to
  `missionId` rather than rewritten in one step.
- If collaboration integration fails, disable incident collaboration while
  preserving ordinary patrol, streams, Targeted controls, and the existing
  Ground Station detector service.
- Do not retain source aliases that make patrol-wide collaboration appear
  supported. The rollback is a configuration/feature-path rollback, not a second
  ambiguous API.

## Risks and Controls

| Risk | Control |
| --- | --- |
| Collaboration becomes a hidden durable session | Hard deadline/state tests; MissionSession owns long-lived application state |
| Large frames enter request payloads | Wire-size and exact-name contract tests |
| Data-centric transaction degrades into endpoint RPC with NDN labels | Reject endpoint fields; trace exact Interests, Data names, signers, versions, and digests |
| Cached or replayed control is mistaken for execution | Cache only immutable evidence/report objects; retain Targeted one-time tokens and state reconciliation |
| Selected detector lacks claimed model/device | Signed capability snapshot plus provider-local readiness gate |
| Duplicate physical command | Targeted command reconciliation and no-blind-retry invariant |
| Detector/video starves control path | Separate bounded worker queues and load tests |
| Late callback corrupts current attempt | Full lineage and plan-digest matching |
| Core protocol scope creep | Separate-Core-Spec stop rule |
| Unit fixture passes but deployment fails | Mandatory real multi-process MiniNDN and SITL gates |

## Complexity Tracking

No constitution violation is planned. The additional MissionSession layer is
necessary because the application mission lifetime is fundamentally longer than
the request-scoped collaboration contract. A persistent Core collaboration
session was rejected because it would mix mission ownership, streaming, flight
safety, and finite provider selection into one protocol lifecycle.
