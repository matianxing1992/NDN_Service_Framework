# Feature Specification: Single-Worker RSA Publication Offload

**Feature Branch**: `136-rsa-sign-verify-offload`  
**Revision**: R3, 2026-07-23  
**Status**: FROZEN — Formal Matrix Complete / `TRADE_OFF`  
**Intent**: Prove or refute that moving real RSA publication preparation from
the NDN-SVS Face thread to exactly one FIFO worker improves useful PubSub
performance.

## Revision Authority And Scope

This R3 specification deliberately replaces the broader R0 design, the R1
Face-timer workload generator, and the physically inadmissible R2 high-rate
matrix. The controlling R3 documents are `spec.md`,
`plan.md`, `tasks.md`,
`contracts/*.md`, and `checklists/pre-implementation-audit.md`.
`traceability.md` is the supplementary R3 coverage map.
`research.md`, `data-model.md`, and `quickstart.md` remain historical R0
context and MUST NOT control implementation.

R3 tests one intervention only:

```text
same current NDN-SVS tree + same binary + same two-node workload
  both modes: independent application pacer thread generates offered load
  control: pacer posts each publication call to Face/io_context
  treatment: pacer calls worker-backed publishAsync() directly
```

Out of scope: multiple signing workers, parallel RSA scaling, general Sync
processing/production offload, Fetcher redesign, UAV/NDNSF logic, version
comparisons, production deployment, and changes to frozen Spec 135 evidence.
The invalid 2026-07-23 Face-timer and signer-overload smokes are retained as
diagnostic evidence and are not performance evidence.

## User Scenarios And Testing

### User Story 1 - Compare The Two Execution Locations (Priority: P1)

As an evaluator, I can run one binary in two runtime modes and determine
whether one-worker asynchronous RSA publication preparation frees the Face
thread without changing publication correctness.

**Independent Test**: A same-binary test runs both modes with identical inputs
and proves that the only runtime difference is whether preparation executes on
the Face thread or on one FIFO worker.

**Acceptance Scenarios**:

1. **Given** `face-inline-rsa`, **when** the independent application pacer
   releases a publication, **then** it posts one call to the sole Face thread
   and inner/outer construction, encoding, and RSA signing complete there.
2. **Given** `worker-rsa`, **when** the same independent application pacer
   releases a publication, **then** it directly calls the byte-oriented
   worker-backed `publishAsync()`, exactly one bounded FIFO worker performs
   preparation, and the prepared result returns to Face for
   state/store/advertisement work.
3. **Given** either mode, **when** a publication is accepted, **then** sequence,
   mapping, store, advertisement, delivery, and error accounting remain
   equivalent and no synchronous fallback changes the treatment.
4. **Given** either mode, **when** a measured cell begins, **then** thread
   evidence proves the pacer differs from Face and proves the publication API
   executes on Face only in control and on the pacer only in treatment.

### User Story 2 - Exercise Real RSA On Both Peers (Priority: P1)

As an evaluator, I can prove that both PubSub peers really sign and validate
RSA objects rather than measuring SHA256 digest or HMAC substitutes.

**Independent Test**: Before formal execution, packet and validator evidence
proves `SignatureSha256WithRsa` for publication Data and Sync Interests, accepts
valid peer objects, and rejects one tampered Data and one tampered signed
Interest.

**Acceptance Scenarios**:

1. **Given** two persistent RSA-2048 identities, **when** either peer publishes
   or sends Sync, **then** both Data and Interest signing use its configured RSA
   identity.
2. **Given** a valid peer-signed object, **when** it is received, **then** the
   configured validator verifies it before protocol/application delivery.
3. **Given** a tampered signed Data or Interest, **when** validation runs,
   **then** it is rejected and cannot reach the subscriber or state merge.

### User Story 3 - Run A Two-Node Bidirectional PubSub Matrix (Priority: P1)

As a researcher, I can compare the two modes under a real two-process PubSub
workload in which both nodes publish and subscribe simultaneously.

**Independent Test**: A sealed manifest contains exactly ten formal cells:
two modes at 200, 250, 300, 350, and 400 publications/s **per peer**. A
separate no-op pacer preflight verifies 1000 releases/s without RSA or NDN-SVS
work.

**Acceptance Scenarios**:

1. **Given** one formal cell, **when** it runs, **then** MiniNDN node A and node
   B each run one independent process; A publishes while receiving B, and B
   publishes while receiving A.
2. **Given** a target rate `R`, **when** the cell runs, **then** each node is
   offered `R` publications/s and the aggregate offered publication rate is
   `2R`.
3. **Given** a formal cell, **when** a runtime mode is selected, **then** both
   peers use that same mode; mixed inline/worker cells are forbidden.
4. **Given** the ten cells, **when** any cell fails or regresses, **then** its
   one terminal receipt remains evidence and is not retried or replaced.
5. **Given** any admission or performance cell, **when** attempted load is
   measured, **then** each peer independently reaches 98%-102% of its target;
   aggregate averaging cannot hide one starved peer.

### User Story 4 - Make A Bounded Usefulness Claim (Priority: P2)

As a reviewer, I can see whether the worker improves delivered capacity, Face
responsiveness, or tail delay without claiming that RSA itself became faster.

**Independent Test**: The analyzer applies the preregistered criteria in
SC-005 and reports `USEFUL`, `NO_CLEAR_BENEFIT`, `TRADE_OFF`, `REGRESSION`, or
`INCONCLUSIVE` for the formal matrix. A non-formal confirmation MUST use a
distinct descriptive verdict and cannot satisfy SC-006.

## Requirements

### Functional Requirements

- **FR-001**: One source tree and one built binary MUST provide both runtime
  modes. Rebuilding or using mode-specific binaries during the matrix is
  forbidden.
- **FR-002**: `face-inline-rsa` and `worker-rsa` MUST use the same byte-oriented
  public publication API, application pacer, packet format, RSA identities,
  validators, NDN-SVS configuration, topology, resource limits, and
  instrumentation. The only caller-path delta is post-to-Face in control
  versus direct worker-backed invocation in treatment.
- **FR-003**: The treatment MUST use exactly one bounded FIFO publication
  preparation worker per peer. It MUST NOT create a multi-worker reorder gate
  or claim parallel signing scalability.
- **FR-004**: Each peer MUST run an independent application pacer thread that
  never runs the Face event loop. In control it MUST post one publication call
  to Face. In treatment it MUST directly call the byte-oriented worker-backed
  `publishAsync()` from the pacer thread. Thread-identity evidence MUST verify
  this ownership in every cell.
- **FR-005**: Face operations, sequence/state mutation, mapping insertion,
  DataStore mutation, advertisement, and application callbacks MUST remain
  Face-thread owned.
- **FR-006**: Queue-full rejection MUST occur before sequence reservation.
  Every submitted publication MUST finish exactly once as rejected, committed,
  failed, or cancelled; silent fallback, silent drop, and unbounded retry are
  forbidden.
- **FR-007**: Both peers MUST use persistent RSA-2048 identities. Publication
  Data and Sync Interests MUST carry `SignatureSha256WithRsa`; configured
  validators MUST perform real peer-certificate verification before use.
- **FR-008**: Formal cells MUST force the fetched publication path with
  `maxPiggyDataSize=1` so outer and encapsulated Data validation are exercised.
  R3 MUST NOT modify piggyback security behavior. Both modes MUST use the same
  publication Fetch window of 64 so the historical default window of 10 does
  not become the experiment's unrelated receive-side bottleneck.
- **FR-009**: Parallel Sync processing, parallel Sync production, and every
  unrelated NDN-SVS worker pool MUST be disabled identically in both modes.
  Both modes MUST enable the existing 5 ms local-publication Sync batching
  primitive so one publication does not force one RSA-signed Sync Interest.
- **FR-010**: Every formal cell MUST use exactly two MiniNDN nodes and two
  independent processes. Both processes MUST publish and subscribe
  concurrently, and both MUST use the cell's selected runtime mode.
- **FR-011**: The formal target rates MUST be
  200/250/300/350/400 publications/s per peer. A target `R` therefore creates
  aggregate offered load `2R`. 600/800/1000 RSA publication cells are
  resource/overload diagnostics and MUST NOT be formal subjects on the
  four-core host.
- **FR-012**: Each formal cell MUST use a 10-second warmup, 60-second measured
  window, and 10-second drain; 10 ms delay, 100 Mbps, zero configured loss, and
  256-byte deterministic payload.
- **FR-013**: The campaign MUST contain exactly ten once-only formal cells,
  one per mode/rate pair. No retries, replacement cells, adaptive rate search,
  or selective omission are authorized.
- **FR-014**: Preflight MUST prove one binary/two modes, one worker in the
  treatment, zero publication workers in control, RSA signature types and
  positive/negative validation, two-way routes, full accounting, clean
  shutdown, distinct APP-pacer/Face threads, the required mode-specific
  caller path, and a no-op 1000 pps per-peer pacer within +/-2% on each peer.
- **FR-015**: Per peer, the analyzer MUST report scheduled, attempted,
  accepted, committed, delivered, rejected/failed/cancelled, delivery ratio,
  Face heartbeat and release lateness, end-to-end delay, RSA Data sign CPU,
  RSA Sync Interest sign CPU, RSA verify CPU, worker queue wait/service/depth,
  Face commit residence, CPU, RSS, and failure reasons.
- **FR-016**: Spec 135 is frozen historical evidence. Spec 136 MUST NOT modify,
  rerun, supplement, or relabel it.
- **FR-017**: Every measured cell MUST independently admit both peers only when
  `attemptedMeasured / (ratePerPeer * measureSeconds)` is in `[0.98, 1.02]`.
  Failure by either peer makes the cell `HARNESS_INVALID`; peer counts MUST be
  reported separately and MUST NOT be replaced by an aggregate average.
- **FR-018**: Formal execution MUST remain blocked until a fresh non-formal
  admission run passes FR-014/FR-017. The earlier 400/800/1000 Face-timer smoke
  with asymmetric attempted rates and zero measured delivery MUST remain
  diagnostic evidence and MUST NOT be reused as a baseline.
- **FR-019**: Preflight MUST separately report signer mutex wait time and
  cryptographic service time for Data and Sync Interest signing. Using measured
  service time and the configured 5 ms batching ceiling, each formal target
  MUST have estimated serial-signer utilization at or below 90%.
- **FR-020**: Attempted-rate admission is necessary but not sufficient.
  Preflight MUST reject a rate when either peer has nonzero worker outstanding,
  Face-dispatch abandoned, or uncommitted accepted work at drain end. A queue
  that accepts work faster than the signer/Face path can drain is
  `OVERLOAD_INVALID`, not evidence of NDN-SVS throughput.
- **FR-021**: Repeated Mapping announcements for one producer/bootstrap/sequence
  MUST NOT register the same subscription more than once. A measured cell with
  valid attempted load but less than 98% unique delivery MUST be reported as
  `LOAD_UNSUSTAINED`, not `COMPLETE` or `HARNESS_INVALID`. A control-only
  `LOAD_UNSUSTAINED` outcome may establish a capacity extension only when the
  paired worker cell sustains at least 98%; if both modes are unsustained, that
  rate supports no worker-effect claim.

## Success Criteria

- **SC-001**: Preflight proves the same binary hash in both modes and that the
  runtime configuration delta is exactly the publication preparation mode.
- **SC-002**: Preflight proves real RSA signing/validation for Data and Sync
  Interests, rejects both tampered probes, and records zero invalid delivery.
- **SC-003**: Correctness tests account for every submitted publication once,
  preserve per-producer commit/delivery order, and complete cleanly under queue
  saturation, signing failure, and shutdown.
- **SC-007**: At 1000 pps/peer, both independent APP pacers produce
  980-1020 attempted publications/s during admission, and thread evidence
  matches the R3 caller-path contract on both peers. This is a no-op pacer test
  and performs no signing, publication, Sync, or fetch work.
- **SC-004**: The sealed formal manifest has exactly ten unique cells and each
  terminal receipt is retained without retry.
- **SC-005**: At a rate, `USEFUL_AT_RATE` requires all correctness/security
  gates, delivery-ratio degradation no greater than one percentage point, and
  at least one of:
  (a) delivered pps improves by at least 10% when control misses offered load;
  (b) Face heartbeat p99 improves by at least 20%; or
  (c) delivery-delay p99 improves by at least 20%.
  Criterion (c) is admissible only when the two delivery ratios differ by no
  more than one percentage point; otherwise delivery p99 is a conditional
  statistic over different survivor sets and MUST be labeled
  `delivered-only`, not used as independent evidence.
- **SC-006**: Overall `ASYNC_SINGLE_WORKER_USEFUL` requires
  `USEFUL_AT_RATE` at two adjacent rates, or treatment sustaining the next
  registered rate that control cannot sustain, without correctness/security
  harm. Otherwise the report uses a non-positive bounded verdict.
- **SC-008**: All formal rates pass the R3 signer-utilization and drain gates.
  The retained 800/1000 R2 cells are labeled `OVERLOAD_INVALID` and excluded
  from worker-effect classification.

## Assumptions And Evidence Limits

- Exactly one worker is sufficient to test whether moving blocking publication
  preparation away from Face is useful; it does not test parallel RSA scaling.
- The ten once-only cells support a bounded descriptive conclusion, not
  population inference or a production-readiness claim.
- No performance conclusion exists until all ten new formal cells finish.
- A focused 400-pps confirmation may report its observed capacity contrast as
  `NON_FORMAL_DESCRIPTIVE`, but MUST NOT report `ASYNC_SINGLE_WORKER_USEFUL`,
  satisfy SC-006, or generalize beyond its exact topology, rate, duration, and
  execution order.

## Formal Closure

The once-only R6 campaign
`results/spec136-rsa-single-worker/formal-r6-20260723T224836Z` executed all ten
registered cells in order. Every cell has one unique terminal receipt, every
peer admitted the required offered load, all cells delivered 100% of measured
publications, and no cell was retried or replaced.

Only 400 pps/peer met `USEFUL_AT_RATE`, through a 33.71% delivery-p99
improvement. Its heartbeat p99 regressed by 70.96%, and no adjacent rate met
the registered usefulness threshold. SC-006 therefore does not authorize
`ASYNC_SINGLE_WORKER_USEFUL`; the frozen overall verdict is `TRADE_OFF`.
This shows a bounded execution-location benefit at 400 pps/peer, not RSA
acceleration, universal latency improvement, or production readiness.

The post-implementation audit records two evidence-coverage limitations:
FR-015's complete distribution/CPU/Face-residence metric set was not captured,
and SC-003's worker-specific queue-saturation/signing-failure cases were not
directly executed. The formal matrix MUST NOT be rerun to repair those gaps;
they require a later independently numbered Spec if still needed.
