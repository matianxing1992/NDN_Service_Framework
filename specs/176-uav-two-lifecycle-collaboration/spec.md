# Feature Specification: UAV Two-Lifecycle Collaboration

**Feature Branch**: `UAV-Experimental`

**Created**: 2026-08-27

**Status**: In implementation; NDN data-centric contract audited, the
capability-driven selector is wired, and the post-selector nominal MiniNDN run
plus the ten-case failure matrix pass. The focused UAV unit and CPU-integration
processes pass in two consecutive reruns, and the candidate-bound PX4/jMAVSim
SITL acceptance wrapper passes all required lifecycle markers. Promotion remains
an explicit, separate release decision.

**Input**: Build the cooperative UAV case on two explicit lifecycles. A patrol
mission is an application-owned session; a finite incident-analysis window is a
request-scoped NDNSF collaboration. Do not represent the whole patrol as one
long-running `RequestCollaboration()`.

## Architectural Decision

The application MUST separate two lifecycles:

1. **MissionSession** is a long-lived, application-owned UAV object. It owns the patrol
   plan, sector assignments, progress, stream bindings, incident history, and
   compensation state. It may span many independent NDNSF requests and streams.
   The MVP keeps bounded state in process and supports explicit snapshot/restore;
   it does not promise automatic crash-proof database persistence.
2. **CollaborationJob** is a bounded NDNSF collaboration request created for one
   incident or one finite analysis window. It discovers and selects providers,
   executes a committed role plan, returns one terminal result, and ends before
   its deadline.

The first demonstrator is heterogeneous cooperative patrol and incident
perception: scout UAVs patrol and publish video/evidence, while a compute-capable
UAV is preferred for high-quality object detection. The Ground Station owns the
MissionSession and starts CollaborationJobs only when an operator or lightweight
trigger identifies an incident worth analyzing.

This feature uses the canonical term **data-centric service transaction**. The
phrase “data-driven” is not an ML/analytics claim or a standalone contribution.
In the NDN sense, the network-facing contract is organized around semantically
named, signed Data rather than transport endpoints:

- NDNSF requests, ACKs, Selections, and Responses retain the existing V2
  service-semantic names and security path. SVS advertises publication state;
  participants retrieve the corresponding named publications rather than
  opening application-defined endpoint sessions. A caller may receive a
  candidate set of logical NDN Provider names from configuration or discovery;
  that set is not a transport endpoint list. The nominal incident roles share
  `/UAV/Incident/Analyze`; signed ACK capability metadata (`evidence-source` or
  `detector-reporter`) identifies which role a provider can satisfy. This keeps
  role selection request-scoped without inventing a role-specific endpoint. An
  EvidenceSource candidate must also equal the producer identity in the
  evidence reference; a source-capable but different provider cannot publish
  under another producer's namespace.
- Video, telemetry, evidence manifests, evidence segments, and independently
  retrievable reports are producer-owned Data. Consumers request exact or
  predictable names with Interests and verify the returned name, signer, and
  content binding. A KeyLocator/name-prefix check alone is not cryptographic
  verification: the existing NDNSF MessageValidator or an explicit producer
  certificate verification step MUST run before detector execution. An
  application-side evidence descriptor is only a reference; it is not accepted
  evidence until the corresponding signed Data has been published and
  successfully fetched and verified. The implementation MUST keep this
  boundary explicit: `UavEvidenceReference` may describe a future object, while
  only a validator-produced `UavVerifiedEvidence` may enter detector execution.
  In the Drone collaboration participant, this boundary is implemented by
  `CollaborationContext::fetchSignedExactData()` followed by
  `UavDetectorProvider::acceptValidatedContent()`; a normal request payload
  cannot bypass that path.
- Immutable evidence/report objects use an exact versioned name and digest.
  Compact collaboration messages carry those names and digests, never an IP
  address, host/port, socket URI, or raw media object as a routing substitute.
  Logical NDN names remain the only provider/object identifiers in the
  application contract.
- `MissionSession` is bounded application state at the Ground Station; it is
  not a network connection, a routable endpoint, or a persistent NDN session.
- Cache reuse is permitted for immutable evidence and report Data when
  freshness, retention, and authorization allow it. Cached Data or a network
  timeout can never authorize, prove, or replay an effectful flight command.

The data-centric transaction is therefore an architectural property and a trace
obligation. The existing NDNSF control exchange remains a
Request/ACK/Selection/Response protocol; this feature does not claim that all
control state has become pure content retrieval or that it introduces a new
data-centric wire protocol.

The feature is checked against the six official [NDN protocol design
principles](https://named-data.net/project/ndn-design-principles/), but only at
the UAV application's boundary; it does not claim to implement an NDN router
or a new wire protocol. The mapping is:

- **Universality**: UAV behavior stays above the generic NDNSF runtime and
  introduces no UAV-specific Core packet type or transport mode.
- **Data-centricity and immutability**: evidence and reports have unique
  producer-owned, versioned names and are retrieved as Data with Interests.
- **Securing Data directly**: signatures, producer identity, trust policy, and
  digest checks travel with and validate the returned Data object.
- **Hierarchical naming**: producer/mission/incident/kind/version components
  bind provenance and application policy context.
- **In-network name discovery**: this feature may learn logical Provider or
  object names from the existing ACK/SVS advertisements, but it does not claim
  that an advertisement alone is name discovery or that every name is
  incomplete. Once a name is known, the consumer still issues an Interest for
  the exact Data name.
- **Hop-by-hop flow balance**: exact, segmented, and stream retrieval uses
  finite Interest/Data windows; one application fetch does not create an
  unbounded server-push or retry loop.

These are application-level acceptance obligations, not additional novelty
claims. The control exchange remains the existing Request/ACK/Selection/
Response protocol.

### NDN conformance boundary

The resulting claim is **NDN-compatible at the UAV application boundary**, not
"full NDN protocol compliance." Universality, producer-owned immutable Data,
direct Data validation, hierarchical names, and bounded Interest/Data flow are
implemented or explicitly contracted here. The official incomplete-name
discovery principle is intentionally outside this feature: ACK/SVS may provide
an availability hint or a logical name, but the acceptance path still uses an
exact-name Interest. This is a scope boundary, not evidence that an
advertisement by itself implements incomplete-name discovery. The candidate
bound SITL run now supplies measured deployment evidence; adverse multi-segment
behavior remains a separate follow-up gate.

The collaboration workflow is **evidence-driven only in this narrow operational
sense**: verified ACK capability metadata drives role selection, and verified
named evidence drives detector execution. This is not a claim of a generic
data-driven/ML framework or a standalone novelty result; while adverse
segmented-data behavior remains open, it is an implemented contract plus
CPU/MiniNDN/SITL evidence rather than an end-to-end hardware deployment claim.

## User Scenarios & Testing

### User Story 1 - Sustain a Patrol Without Holding One Collaboration Open (Priority: P1)

An operator starts a patrol that may run for minutes or hours. The Ground
Station assigns sectors, monitors progress, and keeps video and telemetry active
without treating the complete mission as one NDNSF request.

**Why this priority**: The lifecycle boundary prevents request deadlines,
provider changes, or one failed analysis from destroying the long-lived mission.

**Independent Test**: Run a multi-sector mock patrol for longer than the largest
configured NDNSF request deadline. Verify that the MissionSession remains active,
that each NDNSF request has a finite lifetime and fresh request identity, and
that progress remains available after any request terminates.

**Acceptance Scenarios**:

1. **Given** a planned mission with two scout UAVs, **When** the operator starts
   the mission, **Then** the Ground Station creates one MissionSession and uses
   finite service requests to accept or control mission parts.
2. **Given** an active MissionSession, **When** a mission-assignment or telemetry
   request times out, **Then** the MissionSession remains active and records the
   failed attempt without losing completed progress.
3. **Given** an active patrol, **When** video and telemetry continue across
   multiple service calls, **Then** their stream lifetimes remain independent of
   any CollaborationJob.

---

### User Story 2 - Run One Bounded Incident Collaboration (Priority: P1)

When an incident is detected, the Ground Station creates a finite collaboration
that obtains named evidence from one or more scouts and selects a capable
provider to analyze it. Exactly one selected role publishes the final report.

**Why this priority**: This is the smallest end-to-end case that demonstrates
the NDNSF collaboration API without misusing it as a durable session protocol.

**Independent Test**: Start a CollaborationJob from a recorded incident window,
select an `EvidenceSource` and a `DetectorReporter`, fetch evidence by exact NDN
name, return one report, and verify that the collaboration reaches a terminal
success or failure state within its deadline.

**Acceptance Scenarios**:

1. **Given** a scout evidence object and at least one ready detector, **When** an
   incident is submitted, **Then** the committed collaboration plan references
   the evidence by name and assigns exactly one terminal response owner.
2. **Given** a selected compute UAV, **When** it completes detection, **Then** the
   Ground Station receives one final report linked to the mission, incident,
   collaboration request, evidence names, model identity, and selected provider.
3. **Given** no feasible plan or a missed deadline, **When** the job terminates,
   **Then** the MissionSession records a bounded failure and remains usable.

---

### User Story 3 - Prefer the UAV With the Required Compute Capability (Priority: P2)

The same detection service may be advertised by several providers. The Ground
Station selects a detector only after observing current ACK metadata such as
model availability, execution device, queue state, and estimated readiness.

**Why this priority**: It demonstrates request-scoped runtime provider selection
and heterogeneous UAV cooperation, rather than hard-coded transport addresses.

**Independent Test**: Run two eligible detector providers with different
capabilities and verify that the registered selection policy chooses the
compute-capable UAV when it is ready, then uses only an explicitly configured
Ground Station fallback when the UAV is unavailable.

**Acceptance Scenarios**:

1. **Given** one CPU-only provider and one UAV with the registered high-quality
   model, **When** both return valid ACKs, **Then** the selection policy chooses
   the provider satisfying the required model/device capability.
2. **Given** the preferred compute UAV is unavailable, **When** Ground Station
   fallback is enabled, **Then** the job may select that fallback and records the
   degraded execution mode.
3. **Given** the preferred compute UAV is unavailable and fallback is disabled,
   **When** ACK collection closes, **Then** the job fails explicitly rather than
   silently changing the requested quality level.

---

### User Story 4 - Recover Mission Work Without Replaying Unsafe Commands (Priority: P2)

The Ground Station compensates for missing mission work and may retry idempotent
analysis, but it does not blindly repeat flight commands whose execution state is
unknown.

**Why this priority**: Long-lived missions need recovery, while command ambiguity
must not create unsafe duplicate takeoff, land, or mission-upload effects.

**Independent Test**: Interrupt one patrol part and one detection job. Verify that
only unfinished mission parts are compensated, detection may be retried under a
new request identity, and ambiguous flight commands require state reconciliation
instead of automatic replay.

**Acceptance Scenarios**:

1. **Given** one completed and one missing patrol part, **When** compensation is
   triggered, **Then** the new request contains only the missing part.
2. **Given** a detector timeout before a result is committed, **When** policy
   permits retry, **Then** a new CollaborationJob references the same immutable
   evidence and is deduplicated by incident and attempt identifiers.
3. **Given** a Targeted flight command times out after transmission, **When** its
   execution status is unknown, **Then** the application queries authoritative
   vehicle state before any operator-approved follow-up.

### Edge Cases

- A MissionSession outlives all request deadlines and process-visible request
  objects; no collaboration timer is reused as a mission timer.
- A delayed result from an older attempt cannot overwrite a newer incident or
  mission-part state.
- Duplicate events, reports, and evidence names are idempotently ignored using
  their mission, incident, attempt, and request identities.
- A provider ACKs but disconnects before publishing required collaboration data.
- Evidence expires or cannot be fetched by the selected detector.
- A stream restarts with a new stream session while a CollaborationJob still
  references an immutable frame from the prior session.
- More than one selected participant attempts to claim terminal ownership.
- No provider satisfies the requested model/quality requirement.
- Ground Station fallback is enabled but lacks the declared model artifact.
- A scout is lost after completing some waypoints; compensation excludes
  completed work and preserves its already published evidence.
- Operator cancel stops new work, ends or abandons bounded jobs, and issues
  safety actions through the existing command/state-reconciliation path.

## Requirements

### Functional Requirements

- **FR-001**: The Ground Station MUST represent each patrol as a long-lived,
  application-owned `MissionSession` with a stable `missionId`. The MVP MUST
  keep its in-process ledger bounded, support explicit versioned snapshot and
  restore, and enter recovery-required state after restart until vehicle and
  stream state have been reconciled; automatic crash-proof database persistence
  is out of scope.
- **FR-002**: The application MUST NOT represent the whole patrol, its live
  video, or its telemetry lifetime as one `RequestCollaboration()`.
- **FR-003**: Every service invocation and CollaborationJob MUST retain its own
  fresh NDNSF request identity while carrying explicit `missionId`, `incidentId`,
  and `attemptId` correlation where applicable. Correlation metadata MUST NOT
  replace the NDNSF name with a transport endpoint identifier.
- **FR-004**: Mission assignment MUST return finite acceptance/rejection state;
  mission progress and completion MUST be tracked outside that request.
- **FR-005**: Continuous video and telemetry MUST use the NDNSF Stream API and
  MUST remain independent of CollaborationJob lifetime. Their network transfer
  MUST remain consumer-driven: stream availability may be advertised, but the
  subscriber retrieves mapping and sample Data with bounded Interests.
- **FR-006**: A CollaborationJob MUST be created only for a finite incident or
  explicitly bounded analysis window, with a configured deadline and terminal
  success/failure state.
- **FR-007**: The MVP collaboration plan MUST support zero or more
  `EvidenceSource` roles and exactly one `DetectorReporter` terminal role. The
  registered nominal case MUST assign at least one EvidenceSource to freeze and
  attest a bounded stream window as an immutable manifest. If a verified
  immutable manifest already exists and no source-side work is required, the
  plan MUST omit the redundant EvidenceSource role.
- **FR-008**: Exactly one selected role MUST own the terminal collaboration
  response; changing terminal ownership requires a new committed plan.
- **FR-009**: Frame, clip, and report bytes MUST NOT be embedded as large service
  request payloads. Requests and assignments carry exact names, manifests, and
  compact metadata; providers obtain content through the existing large-data or
  collaboration named-data APIs. No such contract may require an IP address,
  host, port, socket URI, or transport endpoint list to locate a provider or
  object; logical NDN names are permitted.
- **FR-010**: Each immutable evidence reference MUST identify the producer,
  stream session, frame or time window, content digest, and retention deadline.
  Its exact versioned name MUST be rooted in the producer's namespace, and a
  consumer MUST be able to retrieve it with an Interest for that name and reject
  a mismatched name, signer, digest, or expired retention contract. A structural
  descriptor MUST NOT be passed to detector execution until the corresponding
  signed Data has crossed the configured validator and digest checks.
- **FR-011**: Detector ACK metadata MUST make model identity, model quality,
  execution device, readiness, and queue/admission state available to selection.
  The metadata MUST come from an ACK accepted by the existing NDNSF validator
  and the closed ACK snapshot; an application boolean such as `ackVerified` is
  only a provenance assertion and is never a cryptographic verifier by itself.
- **FR-012**: The registered selection policy MUST prefer a ready provider that
  satisfies the requested detector capability. It MUST NOT claim a capability
  advantage from a provider name alone.
- **FR-013**: The high-quality model on a compute-capable UAV is the primary MVP
  detector. Ground Station detection is an explicit configurable fallback, not
  an implicit substitute.
- **FR-014**: Each terminal report MUST identify its mission, incident,
  CollaborationJob request, evidence inputs, selected provider, model, attempt,
  result digest, and terminal status. A small result MAY be carried by the
  provider-named terminal NDNSF Response; if the report is separately retained,
  segmented, or fetched, the Response MUST instead bind its exact
  producer-owned report name and digest without duplicating the full report.
- **FR-015**: Detection over immutable evidence MUST be idempotent and MAY be
  retried or reselected under a new CollaborationJob when policy permits.
- **FR-016**: Flight commands MUST use the existing Targeted command path and
  MUST NOT be blindly retried after an ambiguous timeout.
- **FR-017**: Mission recovery MUST create compensation work only for incomplete
  parts; it MUST preserve completed waypoints and committed evidence.
- **FR-018**: Existing NDNSF permission, NAC-ABE attribute routing, one-time
  token, provider-permission, replay-protection, and signed-data checks MUST
  remain active on every applicable service and collaboration path. Consumers
  MUST authenticate accepted Data from its name, signature/signer, policy, and
  digest rather than trusting the interface or node from which it arrived. The
  application adapter MUST expose this as an explicit verified-evidence input,
  not as an unchecked raw-byte overload.
- **FR-019**: The MVP MUST use existing NDNSF runtime APIs and wire contracts.
  It MUST NOT add a persistent Core collaboration session, a new flight-control
  wire mode, a transport-endpoint discovery protocol, or a service-specific
  framework message type.
- **FR-020**: Operator-visible state and structured traces MUST distinguish
  MissionSession state, CollaborationJob state, stream state, selected roles,
  deadlines, fallback use, compensation, and safety reconciliation.
- **FR-021**: The implementation MUST reject stale reports whose mission,
  incident, attempt, request, plan digest, or terminal owner does not match the
  currently accepted state.
- **FR-022**: UAV-APP feature work, tests, and evidence for this specification
  MUST be developed and validated on `UAV-Experimental` until Tianxing explicitly
  authorizes promotion to another integration branch.

### Non-Functional Requirements

- **NFR-001 Safety**: Network retries MUST never serve as proof that a physical
  flight command did or did not execute.
- **NFR-002 Boundedness**: Every CollaborationJob has finite role count, evidence
  count, event retention, memory use, retry budget, and wall-clock deadline.
- **NFR-003 Determinism**: Registered tests pin topology, provider capabilities,
  model artifact identity, input evidence, and random seed where randomness is
  used.
- **NFR-004 Diagnosability**: A failed job can be attributed to discovery, ACK
  closure, plan commitment, Selection, evidence fetch, execution, reporting, or
  terminal delivery.
- **NFR-005 Compatibility**: Existing mission, Targeted command, telemetry,
  video, and Ground Station detection paths remain available during migration.
- **NFR-006 Resource Isolation**: Continuous video buffering and detector work
  use bounded queues so one incident cannot starve flight-control processing.
- **NFR-007 NDN Data-Centricity and Integrity**: Every accepted evidence object
  and report is signed, rooted in its producer's namespace, immutable at its
  exact versioned name, and digest-bound to the exact name used by the job.
  A name-only descriptor or unverified cache entry MUST NOT be accepted as an
  evidence/report object.
  Object retrieval and provider selection MUST NOT depend on IP/host/port/socket
  endpoint fields. Logical NDN Provider names may be supplied as candidates or
  learned from the existing NDNSF discovery/ACK path.
- **NFR-008 Deployment Progression**: Acceptance is an ordered gate, never a
  blended result: (1) deterministic `unit-tests`, (2) CPU/in-process
  `integration-tests`, (3) real multi-process MiniNDN experiments, and (4)
  PX4 SITL. A later gate cannot be counted until the preceding gate passes;
  optional hardware flight tests come only after all four. The operator-state
  unit test belongs to gate 1, while the Xvfb/MiniNDN GUI smoke belongs to gate
  3 and cannot substitute for the CPU integration gate.
- **NFR-009 Flow Balance and Name Discovery**: Exact evidence/report fetches MUST
  use a finite Interest lifetime and at most one outstanding retry for the same
  object at the application fetch layer. Stream prefetch and segmented fetch
  windows MUST remain explicitly bounded. ACK/SVS MAY advertise logical names or
  availability, but this feature does not claim formal incomplete-name discovery
  from those advertisements. A consumer MUST still retrieve the resulting Data
  with an Interest and MUST record the Interest-to-Data pair in the acceptance
  trace. No application-level server push, endpoint session, or unbounded retry
  loop may replace this exchange.

### Key Entities

- **MissionSession**: Application-owned patrol state: mission identity, plan,
  parts, assignments, progress, stream bindings, incidents, compensation, and
  operator state.
- **MissionPart**: A bounded waypoint/sector unit with assigned provider,
  attempt, completion state, and response digest.
- **StreamBinding**: The producer, stream descriptor/session, content prefix,
  health, and cursor range associated with a mission participant.
- **Incident**: A trigger linked to a mission, time/location, source streams,
  requested analysis capability, and current attempt.
- **CollaborationJob**: One finite NDNSF collaboration request, committed role
  plan, provider assignments, evidence references, deadline, and terminal state.
- **EvidenceObject**: Immutable named frame, short clip, or manifest with producer,
  session, sequence/window, digest, and retention deadline.
- **ProviderCapabilitySnapshot**: ACK-time model/device/readiness/queue metadata
  used by one request-scoped selection decision.
- **TerminalReport**: The authoritative detector result and provenance emitted by
  the one terminal response owner.
- **CompensationAction**: A new bounded request for missing mission work or
  retryable analysis; it never rewrites already committed completion.

## Success Criteria

### Measurable Outcomes

- **SC-001**: A registered two-scout, one-compute-UAV, one-Ground-Station mock
  mission completes a 60-second measured window containing multiple finite
  requests and incident jobs whose individual deadlines are shorter than that
  window, while the MissionSession remains coherent. A longer endurance run is
  optional follow-up evidence, not an MVP acceptance prerequisite.
- **SC-002**: In the registered nominal incident case, the selected compute UAV
  issues Interests for the assigned exact evidence/manifest names, verifies the
  returned signed Data, runs the registered detector, and returns exactly one
  provenance-complete terminal report. The trace contains no application-level
  IP/host/port/socket endpoint fields; logical NDN names remain valid.
- **SC-003**: Across nominal, no-feasible-provider, evidence-unavailable,
  detector-timeout, and terminal-delivery-failure cases, 100% of jobs reach an
  explicit bounded terminal state with no dangling role or duplicate terminal
  response.
- **SC-004**: A failed CollaborationJob does not erase patrol progress, stop an
  unrelated stream, or change completed MissionParts.
- **SC-005**: Compensation tests show that completed mission parts are never
  reassigned and that late older-attempt results are rejected.
- **SC-006**: The capability-selection test chooses the registered compute UAV
  when ready, records explicit fallback when enabled, and fails rather than
  silently degrading when fallback is disabled.
- **SC-007**: Flight-command timeout tests perform state reconciliation and show
  zero automatic duplicate execution of ambiguous Targeted commands.
- **SC-008**: The complete CPU integration profile passes before MiniNDN/SITL;
  MiniNDN and SITL evidence identify the exact commit, configuration, topology,
  model, input digest, and test seed. MiniNDN evidence also records the exact
  evidence/report names and the Interest-to-Data retrieval stages needed to
  establish the data-centric Interest-to-Data path; cache reuse is observed when available but is
  not required for correctness.

## Scope Boundaries

### In Scope

- Application-level MissionSession state and recovery.
- Bounded incident CollaborationJobs using the current collaboration API.
- Named frame/clip evidence and heterogeneous detector selection.
- A compute-UAV detector provider plus optional explicit Ground Station fallback.
- Mock, CPU integration, MiniNDN, and PX4 SITL validation plans.
- Operator-visible lifecycle and provenance state.

### Out of Scope

- A persistent or renewable NDNSF Core collaboration-session protocol.
- Per-frame `RequestCollaboration()` or raw video in request payloads.
- New autonomous formation control, collision avoidance, or autopilot logic.
- Distributed partitioning of one detector model across UAVs.
- Full multi-view 3D fusion, federated learning, or online model training.
- Production certification, outdoor flight qualification, or replacement of a
  mature ground-control station.
- Performance or novelty claims before the registered experiments exist.

## Integration-test reference boundary

The NDNSF-DI integration tests are implementation references for test
organization, not feature dependencies. The complete fixture, core-flow,
segmented-data/SVS, and invocation-stream examples establish the
bootstrap-once/request-isolation, ACK/Selection/fetch assertions,
immutable-segment/replay checks, and bounded stream-fault patterns reused by
Spec176. UAV payloads, names, and lifecycle records remain application-owned.

## Assumptions

- Scout UAVs can publish signed video and immutable named evidence using current
  NDNSF data APIs.
- At least one provider can host the registered detector artifact in the CPU
  integration profile; the final hardware profile may place it on a larger UAV.
- The Ground Station is the mission coordinator and NDNSF collaboration user.
- Existing NDNSF collaboration, Targeted, Stream, permission, and large-data APIs
  are sufficient for the MVP; a discovered Core defect is fixed under a separate
  Core Spec rather than hidden in UAV-APP.
- Registered baseline deadlines and resource bounds will be frozen during
  implementation; exploratory tuning remains separate from acceptance evidence.

## Ambiguity Report

The user has locked the central architecture and branch boundary. Initial
planning scores are Goal 0.95, Boundary 0.90, Constraints 0.82, and Acceptance
0.82, producing weighted ambiguity 0.12. Remaining numerical tuning choices are
explicitly delegated to registered test profiles and do not change the two-layer
lifecycle contract.
