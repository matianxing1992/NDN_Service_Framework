# Requirements Quality Checklist: UAV Two-Lifecycle Collaboration

**Purpose**: Verify that Spec 176 is implementable, bounded, testable, and does
not confuse a long-lived UAV mission with one request-scoped collaboration.

**Created**: 2026-08-27

**Feature**: [UAV Two-Lifecycle Collaboration](../spec.md)

## Lifecycle Boundary

- [x] **CHK001** MissionSession ownership and lifetime are explicitly defined.
- [x] **CHK002** CollaborationJob ownership, deadline, and terminal state are
  explicitly defined.
- [x] **CHK003** The specification prohibits patrol-wide
  `RequestCollaboration()`.
- [x] **CHK004** Continuous video and telemetry lifetimes are independent of
  CollaborationJob lifetime.
- [x] **CHK005** One mission may create zero, one, or many finite jobs without
  changing the Core collaboration contract.
- [x] **CHK006** Retry creates a new job/request identity rather than renewing a
  completed or timed-out collaboration.

## Provider Collaboration and Data

- [x] **CHK007** MVP roles and exactly one terminal response owner are defined.
- [x] **CHK008** Provider capability fields and deterministic selection behavior
  are testable.
- [x] **CHK009** Ground Station fallback is explicit and observable.
- [x] **CHK010** Evidence is referenced by exact name/digest and large bytes are
  prohibited from service request payloads.
- [x] **CHK011** Evidence retention, expiry, restart, tamper, and missing-object
  cases are covered.
- [x] **CHK012** Terminal-report provenance and stale-attempt rejection are
  specified.
- [x] **CHK038** “Data-centric service transaction” is the canonical term;
  consumer retrieval of named, producer-owned, signed Data is distinguished
  from analytics-style “data-driven” wording and transport-endpoint addressing.
- [x] **CHK039** Evidence/report naming requires producer namespace, immutable
  version, exact Interest retrieval, signer/policy verification, and digest
  binding.
- [x] **CHK040** SVS advertisement and the local Stream `push()` API are not
  misdescribed as transport endpoint push; consumers still retrieve named Data.
- [x] **CHK041** Cache reuse is limited to immutable data and is neither required
  for correctness nor allowed to authorize/replay effectful flight commands.

## Mission Recovery and Safety

- [x] **CHK013** Completed mission work is monotonic and compensation covers only
  missing work.
- [x] **CHK014** Idempotent detector retry is distinguished from flight-command
  retry.
- [x] **CHK015** Ambiguous flight-command outcomes require authoritative state
  reconciliation.
- [x] **CHK016** Detector roles cannot directly bypass existing flight-control
  authorization/safety paths.
- [x] **CHK017** Operator cancel, scout loss, late result, and process restart
  boundaries are addressed.

## Security and Compatibility

- [x] **CHK018** Existing permission, NAC-ABE, token, provider-permission,
  replay, signature, and digest checks remain required.
- [x] **CHK019** No UAV-specific NDNSF Core message type or persistent Core
  session is introduced.
- [x] **CHK020** Existing mission, Targeted, stream, telemetry, and detection
  paths have an additive migration/rollback path.
- [x] **CHK021** A newly discovered Core requirement is routed to a separate Spec
  rather than hidden in UAV-APP.
- [x] **CHK042** Provider/object contracts reject IP, host, port, and socket
  endpoint fields while retaining logical NDN names and request-scoped ACK
  discovery.

## Acceptance and Evidence

- [x] **CHK022** Every user story has an independent test and concrete Given/
  When/Then scenarios.
- [x] **CHK023** Functional and non-functional requirements use falsifiable MUST
  statements without unresolved placeholders.
- [x] **CHK024** Success criteria cover nominal, failure, recovery, safety, and
  provenance outcomes.
- [x] **CHK025** Unit, CPU integration, real MiniNDN, and PX4 SITL claims are kept
  separate.
- [x] **CHK026** Registered evidence requires exact source, binary, dependency,
  topology, configuration, model, input, identity, and lifecycle provenance.
- [x] **CHK027** Performance and novelty claims are explicitly deferred until
  corresponding measured evidence exists.
- [x] **CHK043** MiniNDN acceptance requires exact Interest/Data-name,
  signer/version/digest, and no-endpoint trace evidence; a Content Store hit is
  observable but not mandatory.
- [x] **CHK044** The data-centric transaction is treated as an architectural
  property and test obligation, not inflated into an unsupported standalone
  contribution; a pre-publication descriptor is not accepted evidence.
- [x] **CHK045** NDN name discovery is separated from data retrieval, and exact,
  segmented, and stream fetches retain finite Interest/retry windows with no
  unbounded application-level push loop; runtime acceptance must record the
  Interest-to-Data pairs.
- [x] **CHK046** Detector selection consumes capability metadata only from the
  validator-accepted ACK_CLOSED snapshot; `ackVerified` is documented as a
  provenance assertion, not a cryptographic verifier.
- [x] **CHK047** The specification uses the official NDN principle names without
  claiming that ACK/SVS advertisements alone provide incomplete-name discovery;
  exact-name Interest/Data retrieval remains the measured contract.

## Task Quality and Governance

- [x] **CHK028** Tasks are cohesive behavioral outcomes rather than separate
  test/implementation/command/evidence chores.
- [x] **CHK029** Dependencies prevent real-network testing before complete CPU
  integration coverage.
- [x] **CHK030** `UAV-Experimental` is the declared implementation branch and
  promotion requires Tianxing's explicit decision.
- [x] **CHK031** GSD Phase 37 points to the canonical Spec Kit feature instead of
  creating a competing protocol specification.
- [x] **CHK032** Spec 176 source changes remain application-scoped and do not add
  a new NDNSF Core wire type or protocol mode; unrelated pre-existing Core
  worktree edits are not attributed to this feature.
- [x] **CHK033** The strict Spec Kit structural audit parses all 22 tasks and
  reports no malformed task records.
- [x] **CHK034** `tasks.md` maps every FR/NFR/SC group to owning tasks and
  validation gates.
- [x] **CHK035** New UAV application and integration sources have explicit
  `examples/wscript` and `tests/wscript` ownership rather than relying on
  implicit source discovery.
- [x] **CHK036** MissionSession is defined as long-lived application state, not
  automatic crash-proof persistence; restart enters reconciliation before new
  controls.
- [x] **CHK037** EvidenceSource performs necessary source-side freeze/attestation
  work and is omitted when an already verified immutable manifest makes the role
  redundant.

## Review Finding

The requirements are sufficiently clear for implementation planning. The
registered MVP acceptance window is 60 seconds; numerical values that do not
change architecture (individual job deadline, queue size, evidence retention,
and sampling rate) must be frozen in T003 before network integration and must
not be tuned per result seed.
