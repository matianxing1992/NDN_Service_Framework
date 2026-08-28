# Tasks: UAV Two-Lifecycle Collaboration

**Input**: [spec.md](spec.md) and [plan.md](plan.md)

**Canonical branch**: `UAV-Experimental`

**GSD phase**: 37

**Tests**: Required. Each behavioral task includes a failing contract case,
implementation, focused passing validation, and the evidence needed to accept
that behavior. Do not split those steps into separate mechanical tasks.

**Current evidence note (2026-08-28)**: the candidate has a passing CPU slice
for lifecycle records, named evidence, capability selection, bounded
request-scoped coordination, validator-backed signed-Data retrieval, and
diagnostic checks. The UAV applications and controller also link from the same
candidate. The registered MiniNDN launcher now has a successful rootless
isolated-network nominal run with two finite jobs in a 60-second MissionSession;
the failure matrix, second-consumer re-fetch, and multi-segment cases are
tracked separately. The candidate-bound PX4 adapter finds the required
application/PX4 inputs and fails closed when the shell is not root. The
candidate-bound adapter and the official `run_uav_collaboration_probe.sh`
wrapper now complete the PX4/jMAVSim two-lifecycle scenario in a rootless user
namespace with all required stage markers. These results do not close a task
until that task's complete acceptance contract is verified.

**Closed-task evidence (2026-08-28)**: T002--T007 are covered by the focused
`UavTwoLifecycle` suite; T008--T013 by the focused CPU collaboration flow plus
the selector and failure-matrix checks; T006 by the integration lifecycle
fixture; T014 by the compensation/reconciliation state regressions; T015 by
the bounded-pressure regression; and T017
by the trace analyzer and candidate-bound MiniNDN traces. The current focused
unit and integration commands each returned 0 in two consecutive reruns. T016
is closed by the state-model and Xvfb/MiniNDN GUI evidence; T020 is closed by
the candidate-bound PX4/jMAVSim run, while T022 remains open because explicit
promotion authorization and its release checks are not yet closed.

**Mandatory validation order**: after the read-only G0 baseline, execute and
record `unit-tests` first, then the
CPU/in-process `integration-tests`, then real multi-process MiniNDN experiments,
and finally PX4 SITL. A later-stage result cannot be used to mask an earlier
stage failure. T016's state-model test is a unit-gate check; its GUI smoke is a
MiniNDN-gate check. The organization follows the existing NDNSF-DI examples
(`ndnsf-integration-fixture`, `ndnsf-di-core-flow`,
`ndnsf-data-v1-svs-flow`, and `invocation-stream-flow`); these are test
structure references only and add no DI payloads or dependencies.

## Format: `[ID] [P?] [Story] Outcome`

- **[P]**: May run in parallel after its listed dependencies because it owns
  different files and an independent acceptance gate.
- **[US1]**: Long-lived MissionSession.
- **[US2]**: Bounded incident CollaborationJob.
- **[US3]**: Heterogeneous compute-provider selection.
- **[US4]**: Recovery without unsafe command replay.

## Phase 1: Baseline and Frozen Application Contracts

**Purpose**: Preserve current behavior and make the two lifecycle boundaries
machine-checkable before integration begins.

- [x] T001 [US1] Freeze the `UAV-Experimental` behavioral baseline by
  identifying and running the existing mission, Targeted command, stream,
  telemetry, object-detection, security, and collaboration-focused tests;
  record exact commit/config/binary results in
  `specs/176-uav-two-lifecycle-collaboration/evidence/baseline.md`. The task is
  accepted only when failures are either corrected under their existing owner
  or recorded as explicit pre-existing blockers without weakening gates.
  Evidence: `evidence/baseline.md` and `evidence/cpu-lifecycle.md`; the
  unrelated full-suite distributed-inference divide-by-zero is retained as an
  explicit blocker.

- [x] T002 [US1] Define and enforce the application lifecycle records by
  adding `MissionSessionRecord`, `MissionPartRecord`, `StreamBinding`,
  `IncidentRecord`, `CollaborationJobRecord`, `EvidenceReference`, and terminal
  report/capability types in `NDNSF-UAV-APP/shared/UavProtocol.hpp/.cpp`, with
  test-first encoding, bounds, identity-lineage, enum, malformed-input, exact
  producer-name/version/digest, and rejection of IP/host/port endpoint fields in
  `tests/unit-tests/uav-two-lifecycle.t.cpp`. Register the new shared source
  in the unit target's explicit source list in `tests/wscript` and verify that
  its existing unit-test glob discovers the test. No new NDNSF Core TLV is
  allowed.

- [x] T003 [P] [US2] Freeze service, role, failure, and configuration names
  in `NDNSF-UAV-APP/shared/UavNames.hpp`,
  `NDNSF-UAV-APP/configs/uav_runtime.conf`, and
  `NDNSF-UAV-APP/configs/uav_demo.policies`, including
  `/UAV/Incident/Analyze`, `EvidenceSource`, `DetectorReporter`, capability
  metadata, producer-owned evidence/report name schemas, immutable version
  components, job bounds, fallback policy, and failure stages. Add a focused
  config/policy regression that rejects missing permissions, malformed or
  endpoint-bearing names, duplicate terminal roles, invalid bounds, and implicit
  Ground Station fallback.

**Checkpoint**: Existing behavior is reproducible and the application contract
can distinguish mission, incident, job, evidence, attempt, and terminal owner.

---

## Phase 2: User Story 1 - Long-Lived MissionSession (Priority: P1)

**Goal**: The Ground Station owns a long-lived, bounded in-process patrol lifecycle across
many finite requests and independent streams.

**Independent Test**: A mock mission runs longer than the largest request
deadline, survives request failure, and preserves completed work and stream
bindings without any patrol-wide CollaborationJob.

- [x] T004 [US1] Implement the MissionSession state machine and monotonic
  ledger in new `NDNSF-UAV-APP/shared/UavMissionSession.hpp/.cpp`, covering
  legal transitions, completed-waypoint monotonicity, incident/attempt lineage,
  bounded history, stale callback rejection, and cancellation. Drive the
  implementation from state-table and adversarial ordering cases in
  `tests/unit-tests/uav-two-lifecycle.t.cpp`.

- [x] T005 [P] [US1] Add deterministic MissionSession snapshot and restore
  to `UavMissionSession.hpp/.cpp` with schema version, digests, size limits, and
  fail-closed corruption handling. The focused test must restore an active
  mission into `RECOVERING`, block new controls until vehicle/stream
  reconciliation completes, reject inconsistent completion or attempt lineage,
  and require explicit operator recovery when no valid snapshot is available.

- [x] T006 [US1] Integrate MissionSession ownership into the Ground Station
  patrol path in `GroundStationRuntimeState.hpp` and
  `GroundStationServiceContainer.inc.hpp`: map existing `patrol_task_id` to
  `missionId`, retain fresh NDNSF request IDs per attempt, keep assignment
  responses finite, and bind video/telemetry independently. The integration test
  must run a multi-part mission beyond request deadlines and prove that one
  request timeout neither ends the mission nor stops unrelated streams. The
  integration fixture `MissionSessionOutlivesTimedOutRequestAndKeepsStreamsBound`
  covers this contract with two active stream bindings, two mission parts, one
  completed part, and one timed-out finite request.

**Checkpoint**: US1 is independently usable with current patrol services and no
collaboration implementation.

---

## Phase 3: Named Evidence and Compute-UAV Capability (Priorities: P1/P2)

**Goal**: Scouts expose immutable named evidence and a compute-capable Drone
container can truthfully advertise and execute the detector role.

**Independent Test**: A consumer retrieves a signed/digest-matching frame or
short-window manifest by exact name, and the detector provider ACKs only after
its registered model and device are actually ready.

- [x] T007 [P] [US2] Deliver immutable incident evidence references across
  `NDNSF-UAV-APP/shared/UavSensorStreams.hpp/.cpp`,
  `UavVideoPipeline.hpp/.cpp`, and `UavProtocol.hpp/.cpp`. Add test-first cases
  for producer/session/sequence/window/digest/retention binding, exact-name
  Interest-driven publication/fetch, producer namespace and immutable version,
  wrong signer/name/digest, stream restart, expiry, tampering, oversized
  objects, second-consumer retrieval, and the invariant that frame bytes and
  transport endpoint fields never enter the service request payload. Cache reuse
  may be observed but is not required for correctness. The fetch path must use
  the existing NDNSF validator (or an equivalent explicit producer-certificate
  verification) for every returned Data segment; a `ValidatorNull` or
  name-only structural check is not acceptable evidence of authenticated Data.
  The application adapter must expose the resulting proof as an explicit
  `UavVerifiedEvidence` value; a descriptor or unchecked raw-byte overload
  cannot reach detector execution. Evidence:
  `evidence/multisegment-evidence-20260828.md`,
  `UavCollaborationFlow/MultiSegmentNamedEvidenceUsesExactValidatedFetchForTwoConsumers`,
  and the focused UAV protocol/lineage negative tests.

- [x] T008 [P] [US3] Generalize detector execution into a reusable provider
  adapter used by Drone and Ground Station containers, keeping the current
  worker/model path but separating model load/readiness, named evidence fetch,
  explicit signed-Data verification, execution, and provenance report
  generation. The task owns
  `NDNSF-UAV-APP/shared/UavDetectorProvider.hpp/.cpp`, the narrow integrations in
  both service containers, the corresponding source registration in
  `examples/wscript`, and a CPU test proving that ACK capability matches the
  model/device actually used. Ground Station fallback remains explicit.

- [x] T009 [US3] Implement deterministic request-scoped detector selection
  in `NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp/.cpp`. Start from failing
  tests for invalid/stale/negative/unverified ACKs, insufficient quality,
  queue/readiness, compute-UAV preference, deterministic tie breaking, explicit
  degraded fallback, and fail-closed no-provider behavior; then connect the
  policy to the existing custom selection interface without hard-coded provider
  addresses. The selector MUST consume only capability fields from the
  validator-accepted `ACK_CLOSED` snapshot; `ackVerified` is provenance state,
  not a cryptographic verifier.

**Checkpoint**: Evidence and capability selection are independently validated;
the bounded collaboration coordinator may now depend on them.

---

## Phase 4: User Story 2 - Bounded Incident Collaboration (Priority: P1)

**Goal**: One incident creates one finite collaboration with EvidenceSource
roles and exactly one DetectorReporter terminal owner.

**Independent Test**: The CPU integration fixture executes Request -> ACK_CLOSED
-> committed plan -> Selection -> named evidence fetch -> exactly one terminal
report, then closes the job before its deadline.

- [x] T010 [US2] Build the Ground Station CollaborationJob coordinator in
  new `NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp/.cpp` with a
  narrow hook from `GroundStationServiceContainer.inc.hpp`. It must create a
  fresh request/attempt, use deferred `BeginCollaboration`, consume the immutable
  `ACK_CLOSED` snapshot, and call `CommitCollaborationPlan` with capability-bound
  role/evidence assignments expressed only as NDN provider/object names and
  digests, never IP/host/port/socket endpoints. Logical NDN Provider names are
  the only allowed provider identifiers. It must enforce exactly one terminal
  owner for both unary and streamed plans, the job deadline, and matching
  MissionSession lineage. Register its source in
  `examples/wscript` and the integration test explicitly in `tests/wscript`;
  cover nominal and malformed/late callback paths in
  `tests/integration-tests/uav-collaboration-flow.t.cpp`.

- [x] T011 [US2] Implement Drone collaboration participant roles in new
  `NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp/.cpp` and the Drone
  service-container integration, including source registration in
  `examples/wscript`. EvidenceSource freezes and attests an assigned bounded
  stream window as an immutable exact-name/digest manifest with bounded
  retention; plans with pre-existing verified manifests omit that role.
  DetectorReporter fetches exact evidence through bounded Interests, verifies
  exact name/signer/policy/version/digest, executes through the shared detector
  adapter, publishes progress if configured, and alone emits the terminal
  result. Tests must reject role/provider/plan mismatch, unauthorized evidence,
  wrong signer or name, duplicate terminal claim, and execution before local
  readiness.

- [x] T012 [US2] Enforce terminal-report provenance and acceptance across
  `UavIncidentCoordinator` and `UavMissionSession`: accept exactly one report
  only when mission, incident, attempt, request, plan digest, terminal owner,
  evidence digests, model, provider, and result digest match. A small result
  remains in the provider-named terminal Response; a retained/segmented report
  is producer-named and referenced by exact name/digest without duplicate large
  bytes. Test delayed older
  attempts, duplicate End/Response, forged owner, mismatched evidence, and lost
  final response with recoverable named report.

**Checkpoint**: The end-to-end bounded CollaborationJob works in the CPU fixture
without changing NDNSF Core.

---

## Phase 5: User Story 4 - Recovery and Safety Boundaries (Priority: P2)

**Goal**: Retry only idempotent analysis, compensate only incomplete patrol work,
and reconcile ambiguous flight commands before follow-up.

**Independent Test**: Inject detector, scout, and command failures and verify
that mission progress is preserved, new attempts have new identities, and no
ambiguous physical command is automatically replayed.

- [x] T013 [US4] Add bounded analysis failure and retry policy to
  `UavIncidentCoordinator`: attribute failures to discovery, ACK closure, plan,
  Selection, evidence, execution, report, or delivery; allow at most the
  configured idempotent new-job retry over immutable evidence; and reject late
  prior-attempt completion. Include a full failure-stage table in the CPU
  integration test and structured trace evidence.

- [x] T014 [US4] Unify MissionSession compensation with command-state
  reconciliation in `UavMissionSession` and
  `GroundStationServiceContainer.inc.hpp`. The test must lose one scout after
  partial progress, compensate only missing waypoints, preserve evidence, and
  simulate a Targeted command timeout that requires authoritative telemetry/
  vehicle-state reconciliation rather than blind replay. The focused unit and
  integration cases `CompensationPreservesCompletedWorkAndCommandTimeoutNeedsReconciliation`
  and `CompensationAndCommandTimeoutRequireAuthoritativeRecovery` cover the
  state contract; real vehicle telemetry remains a SITL gate.

- [x] T015 [US1] Isolate control, stream, and detector resource pressure
  with bounded queues/workers and explicit overload state in the existing Drone
  and Ground Station containers. A stress regression must show that evidence or
  detector backlog is bounded and does not block command lifecycle processing or
  corrupt MissionSession/CollaborationJob state. The
  `ResourcePressureDoesNotBlockCommandOrJobLifecycle` regression pushes 1,024
  jobs into a four-item queue and verifies bounded drops plus independent
  command/job progression; hardware throughput and concurrent scheduling remain
  outside this CPU acceptance gate.

**Checkpoint**: Registered failure cases terminate safely and the application
distinguishes analysis retry from physical-command recovery.

---

## Phase 6: Operator State and Diagnostics

**Goal**: Make the two lifecycles and their failures understandable without
reading raw logs.

- [x] T016 [US1] Expose separate operator-visible MissionSession and
  CollaborationJob state through `GroundStationRuntimeState.hpp` and
  `GroundStationWindow.inc.hpp`: mission/part progress, stream bindings,
  incident attempts, ACK/plan/Selection stage, selected roles, terminal owner,
  deadline, fallback, failure stage, compensation, and command reconciliation.
  Include state-model tests and a bounded GUI smoke; do not imply that detection
  can directly control flight. Evidence: `evidence/gui-state-20260828.md`.

- [x] T017 [P] [US2] Add one correlation-complete diagnostic trace schema
  shared by local, MiniNDN, and SITL runners. It must sample without losing the
  registered incident IDs and report every stage from mission trigger through
  terminal acceptance, including exact requested/returned evidence/report names,
  Interest issue/Data verification stages, producer/signer/version/digest, and
  an assertion that no application contract carries IP/host/port/socket
  endpoints (logical NDN names are allowed). Add
  an analyzer contract in
  `tests/python/test_uav_collaboration_campaign.py` that rejects missing lineage,
  duplicate terminal owners, unbounded jobs, silent fallback, or unsupported
  success claims.

---

## Phase 7: Real Integration and Deployment Gates

**Purpose**: Prove the same behavior outside in-process fixtures before any
hardware or research claim.

- [x] T018 [US2] Pass the registered real multi-process MiniNDN nominal
  profile using one Ground Station, two scouts, and one compute UAV in
  `examples/ndnsf/uav-collaboration/minindn_uav_collaboration.py`. Validate real
  permission/bootstrap, discovery, ACK closure, role plan, exact named evidence,
  compute-UAV selection, detector execution, one terminal report, stream
  independence, and a 60-second measured MissionSession window containing
  multiple finite requests/jobs with shorter deadlines; preserve a
  candidate-bound manifest, packet/application trace proving consumer Interest
  -> verified Data retrieval, and the no-transport-endpoint contract. Re-fetch one
  immutable object from a second consumer to demonstrate location-independent
  naming; record but do not require a Content Store hit. Longer endurance is
  optional follow-up evidence and is not required for acceptance. The
  post-selector candidate rerun in `evidence/minindn-nominal-20260828.md`
  records `SPEC176_CAPABILITY_SELECTION`, the 60-second stream window, and two
  successful finite jobs.

- [x] T019 [US3] Pass the registered MiniNDN failure matrix on the same
  candidate and fixed configuration: no feasible detector, compute-UAV loss,
  explicit Ground Station fallback on/off, evidence expiry/tamper, selected
  provider loss, delayed older result, terminal-delivery loss, and bounded queue
  overload. Do not tune against individual seeds; classify mechanisms before
  extending repetitions. The post-selector candidate rerun in
  `evidence/minindn-failure-matrix-20260828.md` reports all ten cases PASS,
  including explicit fallback and stale-attempt rejection.

- [x] T020 [US4] Pass the PX4 SITL two-lifecycle acceptance scenario through
  `NDNSF-UAV-APP/tools/run_uav_collaboration_probe.sh`: run patrol and streams,
  complete one incident, fail one incident without ending the mission, lose and
  compensate one mission part, and reconcile one ambiguous command without
  duplicate execution. Follow the established NDNSF-DI integration pattern:
  perform a read-only preflight, start all processes from one candidate-bound
  manifest, wait for explicit readiness before requests, isolate each request
  from bootstrap state, and retain stage-correlated logs/hashes. Freeze exact
  SITL parameters and environment manifest; do not import DI-specific payloads
  or dependencies. Evidence: `evidence/sitl-adapter-run-20260828.md` and
  `results/spec176-rootless-sitl-r14-20260828/summary.json` (wrapper return
  code 0 and all seven required stage markers present).

**Checkpoint**: Unit, CPU integration, MiniNDN, and SITL gates are evaluated in
that order on the same source contract. Hardware work remains unauthorized and
out of scope.

---

## Phase 8: Documentation, Audit, and Promotion Decision

- [x] T021 [US1] Close Spec 176 documentation and traceability by updating
  `NDNSF-UAV-APP/README.md` and `README_ch.md` with the final two-lifecycle API
  example, lifecycle/failure semantics, NDN data-centric contract, deployment
  profile, and evidence boundary; map every FR/SC to tests/evidence; run Spec Kit consistency and
  code-aware audit; use “data-centric service transaction” as the precise term,
  explain that “data-driven” is not a standalone novelty claim, and verify that only application-scoped
  changes occurred. Evidence: synchronized English/Chinese READMEs,
  `AUDIT.md`, `traceability.md`, and the strict Spec Kit audit pass on
  2026-08-28.

- [ ] T022 [US2] Perform the `UAV-Experimental` release/promotion gate:
  verify clean ancestry and tree, no missing tests/contracts/evidence, all G0-G5
  results bound to the candidate, and no accidental NDNSF-DI/TigerCluster or Core
  protocol edits. Keep development on `UAV-Experimental`; promotion or merge to
  `Experimental`/`main` requires Tianxing's separate explicit authorization.

## Dependencies and Execution Order

```text
T001 baseline
  |
  +--> T002 lifecycle records --> T004 MissionSession --> T006 GS integration
  |                                |                     |
  |                                +--> T005 snapshot ---+
  |
  +--> T003 names/config/policy -------------------------+
                                                           \
T007 named evidence ----------------------------------------> T010 coordinator
T008 detector provider --> T009 capability selection ------> T010 coordinator
T010 coordinator --> T011 participant roles ---------------> T012 provenance

T012 --> T013 bounded retry --> T014 compensation/safety --> T015 pressure isolation
T006  -------------------------> T014 compensation/safety
T008  ------------------------------------------------------> T015 pressure isolation

T006/T012/T014 --> T016 operator state
T010/T012/T013 --> T017 diagnostics

T001-T017 --> T018 MiniNDN nominal --> T019 MiniNDN failures --> T020 SITL
T018/T019 --> T021 documentation/audit
T020 + T021 --> T022 promotion decision
```

### Parallel Opportunities

- T003 may run beside T002 after T001.
- T005 may run beside Ground Station integration preparation after T004.
- T007 and T008 are independent after T001/T002.
- T011 follows T010 because both register UAV application sources in
  `examples/wscript`; this prevents conflicting build-file edits.
- T017 may proceed after its listed behavioral dependencies. T015 follows T014
  because both modify the Ground Station/Drone state owners.

### Non-Negotiable Gates

- Do not start T010 until lifecycle, evidence, capability, and configuration
  contracts are frozen.
- Do not start MiniNDN until the complete CPU integration failure table passes.
- Do not start SITL until real MiniNDN nominal and failure gates pass.
- Do not start hardware flight tests under this Spec.
- If implementation requires a persistent Core collaboration session or new
  Core wire contract, stop and create a separate Core Spec.

## Implementation Strategy

### MVP Slice

The smallest valid demonstrator is T001-T012 plus the nominal portion of G2 CPU
integration: one MissionSession, two finite mission assignments, independent
streams, one incident, one scout EvidenceSource, one compute-UAV
DetectorReporter, named evidence, and one final report. It deliberately excludes
retry, fallback, GUI polish, MiniNDN, and SITL until the two lifecycle boundaries
are proven. T018 starts only after T013-T017 complete the registered CPU failure
and diagnostic gates.

### Incremental Delivery

1. Preserve existing UAV behavior and freeze application contracts.
2. Deliver MissionSession without collaboration.
3. Deliver named evidence and compute-UAV detector capability.
4. Deliver one bounded CollaborationJob.
5. Add recovery/safety and operator diagnostics.
6. Advance through CPU, MiniNDN, and SITL evidence gates.
7. Audit and request an explicit promotion decision.

## Requirement Traceability

| Requirements / criteria | Owning tasks and gates |
| --- | --- |
| FR-001--FR-005, SC-001 | T002, T004--T006, T016, G1--G3 |
| FR-006, FR-008, SC-002--SC-003 | T002, T003, T010--T013, T017--T019, G2--G3 |
| FR-007 | T003, T010--T012, T018; registered nominal includes one necessary EvidenceSource |
| FR-009--FR-010, NFR-007 | T002, T007, T010--T012, T019, G1--G3 |
| FR-011--FR-013, SC-006 | T003, T008--T009, T018--T019 |
| FR-014--FR-015, FR-021 | T002, T010--T013, T017, T019 |
| FR-016--FR-017, NFR-001, SC-004--SC-005, SC-007 | T004, T006, T013--T014, T019--T020 |
| FR-018, NFR-007 | T001, T003, T007, T011--T012, T018--T019 |
| FR-019 | T001--T012, T021--T022; separate-Core-Spec stop gate |
| FR-020, NFR-004 | T016--T020 |
| FR-022 | T001, T018--T022, G5 |
| NFR-002 | T002--T004, T007, T010, T013, T015, T017, T019 |
| NFR-003, NFR-008, SC-008 | T001, T017--T022, G0--G5 |
| NFR-005 | T001, T006, T008, T014, T022 |
| NFR-006 | T015, T019 |
| NFR-009 | T007, T015, T017--T019; G2--G3 |

## Notes

- T001, T007, T014, T015, T016, T018, T019, T020, T021, and the selector policy gate
  are closed with evidence. T016 includes the unit state-model check and the
  MiniNDN/Xvfb GUI smoke; T018/T019 include candidate-consistent post-selector
  reruns. T020 is closed by the candidate-bound PX4/jMAVSim wrapper run;
  T022 remains open until the explicit promotion acceptance contract is
  authorized and verified.
- Commit per cohesive task or reviewable group; do not use `git add -A`.
- Preserve old evidence; new runs use new immutable result directories.
- Do not label fixture/CPU evidence as real-network, GPU, SITL, or flight proof.
