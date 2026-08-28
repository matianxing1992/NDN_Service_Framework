# Spec 176 Code-Aware Audit (implementation slice)

**Date**: 2026-08-28 (follow-up NDN data-centric and flow-balance audit)

**Branch**: `UAV-Experimental`

**Audited candidate**: `UAV-Experimental` working tree, 2026-08-28
**Verdict**: **CONDITIONAL PASS for the NDN data-centric contract and the
validator-backed application implementation slice; the capability-driven
selector is wired, focused unit/CPU-integration reruns pass, candidate-
consistent nominal plus ten-case failure-matrix MiniNDN reruns pass, and the
candidate-bound PX4/jMAVSim SITL wrapper passes the two-lifecycle markers.
Adverse multi-segment behavior, hardware, and performance remain open; the
release audit is complete, but branch promotion was intentionally not
performed.**

The corrected specification, plan, tasks, checklist, and traceability map remain
consistent. The candidate now has bounded lifecycle records, named evidence
validation, detector capability/selection, a request-scoped coordinator, a CPU
fixture with passing assertions, deterministic multi-segment CPU evidence, and
candidate-consistent nominal/failure-matrix MiniNDN evidence, and a
candidate-bound PX4/jMAVSim SITL result. Multi-segment adverse-network cases,
hardware, performance, and research claims remain unverified.

This revision closes the naming, lifecycle, and nominal stream portion of the
NDN data-centric architecture review. The design is NDN-compatible at the UAV
application boundary because
provider/object identity is expressed by semantic names, immutable evidence is
retrieved with Interests and intended to be secured as Data, and no application
contract depends on transport endpoint addressing.
Logical NDN Provider names may still be supplied as candidates or learned from
the existing discovery/ACK path; the prohibition is specifically against
IP/host/port/socket endpoint fields. Nominal and deterministic multi-segment
runtime proof is now recorded; adverse segmented-data proof remains a
deployment follow-up, while the candidate-bound SITL gate is now recorded.
T017 supplies the shared trace schema but does not substitute for adverse
evidence or release promotion.

## Ordered validation gates

Spec176 now uses one cumulative progression (after the read-only G0 baseline):
**G1 unit-tests -> G2
CPU/in-process integration-tests -> G3 real multi-process MiniNDN -> G4 PX4
SITL**. A later gate cannot repair or hide an earlier failure. The typed
operator-state regression is a G1 check; the bounded Xvfb Ground Station GUI
smoke is recorded as a G3 profile and is not a replacement for G2. Current
evidence passes G1, G2, the registered G3 nominal/failure profiles, and G4
through the candidate-bound PX4/jMAVSim wrapper. The G5 release audit is
complete; promotion remains a separate explicit authorization decision.

## NDN Principle Matrix

The principle names follow the NDN Architecture reference, [NDN Protocol
Design Principles](https://named-data.net/project/ndn-design-principles/). This
audit evaluates only the UAV application's boundary, not forwarding-plane
conformance.

| NDN principle | Spec176 mapping | Current evidence boundary |
| --- | --- | --- |
| Universality | UAV behavior stays in `NDNSF-UAV-APP`; no UAV-specific Core wire type or transport mode | Implemented/planned; Core compatibility remains gated by T018--T022 |
| Data-centricity and immutability | Producer-owned evidence/report names carry mission/incident/version; consumers fetch immutable Data by name | CPU checks, the multi-segment exact-fetch regression, the post-selector nominal MiniNDN run, and the SITL stage trace publish/fetch producer-owned Data; adverse segmented-data behavior remains unverified |
| Securing Data directly | Data is signed by the producer and fetched through the configured validator; detector input is `UavVerifiedEvidence` only | CPU negative cases, the 48-segment validator-backed CPU fetch, post-selector MiniNDN verification, and the SITL application contract pass; adverse segmented-data behavior remains unverified |
| Hierarchical naming | Producer/`UAV`/mission/incident/kind/version hierarchy binds provenance and policy context | Implemented and unit-tested |
| In-network name discovery | ACK/SVS may bootstrap a logical Provider/object name or availability hint; this feature does not claim that an advertisement alone is the official incomplete-name discovery mechanism | Nominal ACK/Selection trace records logical Provider names and exact follow-up Interests; the measured path is compliant, while incomplete-name discovery itself is outside this feature |
| Hop-by-hop flow balance | Exact fetch has finite Interest lifetime and one application-level retry in flight; stream/segmented windows are bounded | The 48-segment CPU fetch records one bounded exact Interest per segment, and the post-selector nominal MiniNDN trace records issued Interest, returned Data, verified Data, and an active predictive stream; adverse retry evidence pending |

This matrix supports the narrower conclusion **NDN-compatible at the UAV
application boundary, data-centric, and narrowly evidence-driven**. “Data-driven”
is not used as a generic framework or ML contribution label. The official NDN
principle names are used as an audit checklist, not as a claim that Spec176
implements routing, forwarding, or a new wire protocol. In particular, the
incomplete-name discovery principle is intentionally out of scope: the tested
path uses exact-name Interests after ACK/SVS availability hints. Thus the
architecture is compatible with NDN; adverse multi-segment behavior remains
unverified, and the SITL result is limited to the candidate-bound simulator
scenario rather than hardware-flight conformance.

## Code-Aware Evidence

1. The required bounded collaboration primitives already exist. The current
   Core declares `RequestCollaboration`, deferred `BeginCollaboration`, and
   `CommitCollaborationPlan` in `ndn-service-framework/ServiceUser.hpp:787-825`.
   `ServiceUser.cpp:5463-5598` rejects commit before `ACK_CLOSED`, rejects a
   closure-digest mismatch, rejects participants outside the closed ACK set, and
   makes a repeated identical commit idempotent.
2. `CollaborationRoleSpec::terminalResponseOwner` exists at
   `ServiceUser.hpp:81-98`. Core requires exactly one owner when the
   collaboration is streamed (`ServiceUser.cpp:5494-5503`), but that exact
   cardinality check is not general to unary plans. Spec 176 therefore assigns
   application-level validation before every plan commit and again at terminal
   report acceptance; it does not falsely claim a stronger current Core guard.
3. The Provider context already supports caller-chosen exact-name segmented
   publication and retrieval through `publishLargeNamed` and `fetchLarge`
   (`ServiceProvider.hpp:418-442`). Evidence therefore needs an application
   manifest and role adapter, not a new UAV-specific Core wire type.
4. Build ownership is explicit. `tests/wscript:52-98` discovers unit tests by
   glob but explicitly lists UAV shared implementation sources;
   `tests/wscript:104-117` explicitly lists integration test files.
   `examples/wscript:156-182` explicitly lists sources for both UAV processes.
   Tasks now name both build files, and tasks that would edit
   `examples/wscript` or the same service-container owners are serialized.
5. The current application carries `patrol_task_id` across independent Ground
   Station operations and exposes `/UAV/GS/ObjectDetection`. The candidate
   additionally contains application-scoped `UavMissionSession`,
   `UavIncidentCoordinator`, `UavDetectorProvider`,
   `UavCollaborationParticipant`, and `selectUavDetector` implementations;
   focused unit/CPU fixture evidence is recorded separately and is not promoted
   to real-network evidence.
6. Current Core naming helpers place requests and selections under requester
   identities and ACKs/responses under provider identities using unified V2
   service names. `CollaborationContext::publishLargeNamed()` and `fetchLarge()`
   accept exact NDN names, so Spec 176 can enforce producer-owned evidence and
   report names without introducing endpoint addressing or a new Core wire mode.
7. Current control messages are published through the existing NDNSF/SVS path;
   the plan therefore distinguishes synchronization/availability advertisement
   from consumer Interest retrieval. It does not falsely describe the local
   Stream `push()` API as IP-style network push.
8. The raw-Data detector adapter (`UavDetectorProvider.cpp:73-108`) now requires
   a caller-supplied producer
   certificate and performs cryptographic signature verification before
   digest-based execution. It returns an explicit `UavVerifiedEvidence` value;
   detector execution no longer accepts a bare descriptor or unchecked raw-byte
   overload. This closes the previous application-level name-only/trusted-byte
   ambiguity. Core large-data retrieval now routes exact segments through
   `MessageValidator` and the SegmentFetcher branch through its configured
   ndn-cxx trust-schema validator. The deterministic multi-segment CPU runtime
   proof is recorded separately; adverse-network runtime proof remains pending.
9. Evidence references and retained report names are now checked against the
   surrounding mission and incident components before they enter a
   MissionSession, job plan, or terminal report. A report signed by the correct
   terminal provider but named for another mission/incident is rejected. The
   focused unit suite covers this cross-lineage case. Explicit Ground Station
   fallback selection also requires a verified capability candidate; a logical
   Provider name alone cannot opt into fallback. `Begin` enters
   `ACK_COLLECTING` before ACK closure, matching the documented finite request
   lifecycle.
10. The Ground Station collaboration selector now parses capability fields from
    each accepted ACK candidate and delegates detector choice to
    `selectUavDetector()`. Normal selection is constrained by model, quality,
    device, readiness, evidence access, queue depth, and ACK freshness; the
    Ground Station candidate is considered only when the explicit fallback
    profile is enabled and its degraded capability is independently ready. The
    selector records the chosen logical Provider name and never infers
    capability from that name.

### Core-diff scope audit

The working-tree changes under `ndn-service-framework/` are intentionally
narrow support changes, not a new UAV protocol: scoped per-node initialization
locks, registration of producer-namespace application Data, propagation of the
configured trust-schema validator to segmented retrieval, assignment key-scope
installation, and bounded retrieval diagnostics/API compatibility. No
UAV-specific Core TLV, naming mode, transport mode, or NDNSF-DI/TigerCluster
deployment dependency was added. The UAV behavior remains in
`NDNSF-UAV-APP`; these generic fixes are covered by the focused unit,
integration, and candidate-bound MiniNDN/SITL runs.

## Residual Finding

The post-selector runtime gap is resolved: the Ground Station consumes
validator-accepted ACK capability metadata, delegates detector choice to
`selectUavDetector()`, and the candidate-consistent nominal and failure-matrix
MiniNDN reruns pass. The controller startup issue was fixed by passing its
already-known identity name to `CertificatePublisher`, avoiding the current
ndn-cxx certificate-name exception path. An earlier diagnostic invocation
showed a host-library static-teardown failure, but two subsequent focused unit
and integration reruns both returned 0, so it is not an active acceptance
blocker for the current candidate.

**MEDIUM — Adverse-network acceptance evidence is still missing.** The Core
`ServiceProvider::fetchCollaborationLargeData` path now validates exact-segment
responses through `MessageValidator` and passes the same configured
ndn-cxx trust-schema validator to `SegmentFetcher`; the previous
`ValidatorNull` fallback has been removed from this path. The application
  adapter also verifies the supplied producer certificate before detector
  execution. This closes the code-level authentication gap. The deterministic
  CPU regression now proves certificate-backed multi-segment Interest-to-Data
  retrieval, reassembly, digest binding, and a two-consumer fetch. Adverse
  packet-loss, reordering, and tamper injection over MiniNDN remain separate
  follow-up evidence. T017's trace schema and analyzer are complete, but their
  presence does not count as adverse evidence or authorize promotion. The
pre-selector nominal four-process path is retained separately for history; the
post-selector rerun is the current nominal gate, and it does not substitute for
  the failure matrix or the completed SITL gate.

The fixed MiniNDN launcher has a passing read-only preflight, starts the UAV
policy/trust-schema controller, and uses each application's `--runtime-config`
option. The post-selector rootless `unshare --user --map-root-user` run
completed the nominal four-process path in 83.455 seconds with two independent
incident jobs; controller, Ground Station, and all three UAV processes exited
normally. Its trace records the issued exact evidence Interest, returned Data,
validator completion for both jobs, a second-consumer re-fetch,
`SPEC176_CAPABILITY_SELECTION provider=/example/uav/drone/C fallback=false`, and
`SPEC176_STREAM_FINAL ok=true` with increasing stream chunks during the jobs.
The Drone `/UAV/Incident/Analyze` registration performs compact
reference/queue validation and calls the selected participant's
  `CollaborationContext::fetchSignedExactData()` before detector execution. The
  post-selector ten-case failure matrix and the multi-segment CPU evidence are
  recorded separately; adverse segmented-data remains a separate deployment
  follow-up gate, while PX4 SITL acceptance is now recorded.

## Data-Driven Determination

**Verdict: NDN-compatible at the UAV application boundary; data-centric;
narrowly evidence-driven; not yet an end-to-end data-driven deployment claim.**
The design follows NDN principles when it names
producer-owned immutable objects, retrieves them with exact Interests, validates
the returned signed Data, and keeps transport endpoints out of the application
contract.
The CPU and pure selector assertions make the evidence boundary explicit: the Ground
Station selector consumes capability fields from the accepted ACK_CLOSED
snapshot, and only `UavVerifiedEvidence` produced from signed, exact-name Data
drives detector execution. The post-selector nominal and failure-matrix MiniNDN
runs plus the deterministic multi-segment CPU regression exercise that path.
“Data-driven” remains unsuitable as a generic ML/framework contribution label;
it describes only this bounded operational decision flow, not a claim of model
accuracy or hardware-flight completeness while adverse segmented-data evidence
remains open; branch promotion is intentionally separate from this audit.
The current CPU detector is a deterministic acceptance adapter: it proves that
verified named content crosses the execution boundary, but it does not measure
object-detection accuracy or establish a production model-quality claim.

## Findings Resolved in This Audit

| Severity | Finding | Resolution |
| --- | --- | --- |
| BLOCK | Bold task syntax caused the strict parser to see zero executable tasks. | Converted all task rows to standard `- [ ] Tnnn ...`; strict parser now sees 22 tasks. |
| HIGH | “Persistent MissionSession” conflicted with bounded in-memory MVP storage and could imply crash-proof persistence. | Defined long-lived application ownership, versioned snapshot/restore, explicit `RECOVERING`, and no new controls before reconciliation. |
| HIGH | EvidenceSource could be a redundant role because evidence was described as frozen before collaboration. | Trigger now supplies a window descriptor; a selected source freezes/attests it. An already verified immutable manifest causes the role to be omitted. |
| HIGH | Five-minute MVP acceptance conflicted with the registered 60-second short-test window. | SC-001 and T018 now use a 60-second measured mission with multiple shorter finite requests/jobs; endurance is optional. |
| HIGH | Planned new source/test files lacked Waf registration ownership. | Added `examples/wscript` and `tests/wscript` to structure and owning tasks. |
| MEDIUM | T011/T015 were marked parallel despite overlapping build/container owners. | Serialized T011 after T010 and T015 after T014. |
| MEDIUM | MVP text jumped from T012 directly to real MiniNDN while the dependency graph required the CPU failure gate. | MVP now ends at nominal G2 CPU integration; T018 starts only after T013-T017. |
| MEDIUM | Requirement-to-task coverage was asserted but not machine-detectable. | Added `traceability.md` covering every FR, NFR, and SC. |
| MEDIUM | Plan overstated Core enforcement of one terminal owner. | Recorded the streamed-only Core guard and assigned the unary invariant to the UAV coordinator/report acceptance path. |
| HIGH | “Data-driven” was ambiguous and could be read as an analytics claim; named evidence also lacked a clear publication/verification boundary. | Adopted “data-centric service transaction,” scoped it to named producer-owned Data and the existing control exchange, and required signed publication plus configured-validator retrieval before evidence acceptance. |
| MEDIUM | The earlier “no preconfigured endpoint list” wording could be read as forbidding logical NDN Provider-name candidates required by existing NDNSF APIs. | Clarified that logical NDN names are allowed as candidates/discovery results; only IP/host/port/socket transport addressing is prohibited. |
| MEDIUM | Cache semantics and effectful command semantics were not separated. | Allowed policy-compliant reuse of immutable evidence/reports, made cache hits optional, and prohibited cache/timeout from authorizing or proving flight-command execution. |
| MEDIUM | A retained report could duplicate bytes already carried in the terminal Response. | Small results stay in Response; independently retained/segmented reports are referenced by exact producer name and digest. |
| HIGH | A valid producer-owned name could be attached to a different mission/incident, and fallback could be selected from a name without verified capability metadata. | Added contextual name binding for evidence and job/report acceptance, and require a verified fallback capability with explicit degraded-device semantics. |
| MEDIUM | The detector adapter accepted raw bytes beside a signed-Data path, making it possible to mistake a descriptor for authenticated evidence. | Replaced the unchecked execution input with `UavVerifiedEvidence`, produced only after exact-name, certificate/signature, and digest verification. |
| MEDIUM | Compensation and command timeout handling were previously documented separately, leaving the safety boundary implicit. | Added MissionSession compensation/recovery regressions that preserve completed waypoints, compensate only missing parts, represent command timeout as non-accepted/operator-decision, and require explicit vehicle/stream reconciliation before controls resume. |
| MEDIUM | A bounded queue existed, but pressure isolation was not demonstrated independently from ordinary overload handling. | Added a 1,024-item pressure regression showing a four-item bound, explicit drops, and independent command/job progression while detector work is overloaded. |

## Architecture, Security, and Migration Verdict

- **Necessity / Occam**: PASS. MissionSession is necessary because a patrol
  outlives a request; EvidenceSource exists only when source-side freeze and
  retention work is required. No persistent Core session or redundant network
  mode is introduced.
- **Ownership**: PASS. Ground Station owns mission and incident acceptance;
  each NDNSF request owns only its finite network lifecycle; producers own named
  evidence; exactly one selected DetectorReporter owns the terminal report.
- **NDN data-centric fit**: PASS at planning level and in the current code path,
  CONDITIONAL only on runtime evidence.
  Network contracts name
  service control and producer data rather than transport endpoints; logical
  NDN Provider names may be configured or discovered, while IP/host/port/socket
  fields are excluded. The CPU adapter now verifies a supplied producer
  certificate and exposes only `UavVerifiedEvidence` to detector execution.
  Both exact-segment and SegmentFetcher retrieval now use the configured
  validator; adverse multi-process evidence is still pending. MissionSession remains
  local application state, not a network session.
- **Security**: PASS at planning level, CONDITIONAL in implementation. Existing permission, NAC-ABE, token,
  provider-permission, replay, signature, digest, ACK-closure, and plan-digest
  checks remain mandatory. Candidate-consistent nominal and failure-matrix
  runtime proof now passes; adverse multi-process evidence remains required.
- **Migration / rollback**: PASS. The path is additive and profile-gated;
  ordinary patrol, streams, Targeted controls, and Ground Station detection
  remain available. Any newly required Core wire change stops this Spec.
- **Test executability**: PASS for planning. Unit, CPU integration, MiniNDN, and
  SITL have separate, ordered gates and explicit build owners. Focused unit/CPU
  integration, compensation/reconciliation, pressure isolation, and the MiniNDN
  nominal/failure and deterministic multi-segment gates pass; the adverse
  segmented-data scenario remains open.

## Remaining Acceptance Gate

The CPU slice, nominal stream/incident path, compensation/reconciliation state
contract, second-consumer exact fetch, deterministic multi-segment evidence,
ten-case MiniNDN failure matrix, and candidate-bound PX4/jMAVSim SITL run are
reproducible. Adverse segmented-data behavior remains a follow-up task. Do not
call the feature complete or start hardware work until the adverse evidence and
the final documentation/promotion checks pass.

### Follow-up implementation check (2026-08-28)

The container path now makes the nominal role boundary explicit. The shared
`/UAV/Incident/Analyze` service advertises signed ACK capability metadata; a
source-capable drone is selected as `EvidenceSource`, publishes a deterministic
producer-owned manifest through `CollaborationContext::publishSignedExactData`,
and completes its non-terminal role. The compute-capable drone is selected as
`DetectorReporter` and remains the only role allowed to publish the terminal
Response. Role-level provider permissions are listed in `uav_demo.policies`.
This closes the design/code mismatch identified above. The registered MiniNDN
launcher has now completed a rootless isolated-network nominal run and the
ten-case failure matrix on the same candidate; deterministic multi-segment CPU
evidence is recorded separately, while adverse multi-process and SITL results
are not being claimed.
