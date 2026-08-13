# Feature Specification: Four-Provider Multi-AP Mobility Advantage

**Feature Directory**: `specs/171-four-provider-mobility-advantage`  
**Created**: 2026-08-05  
**Status**: In progress  
**Upstream Baseline**: `specs/169-three-provider-mobility-baselines`

## Purpose

Establish a fair, reproducible mobility comparison in which NDNSF can use
multiple authorized Providers while a strict gRPC sequential baseline and NSC
must try Providers one at a time under the same logical-request deadline. The
primary topology is the original one-AP, four-Provider setup; coverage radius
and deterministic drone speed are varied to expose the conditions in which
multi-Provider coordination can reduce recovery cost. A health-assisted
four-target gRPC baseline remains an explicitly enabled sensitivity condition,
not the default or a replacement for strict sequential comparison.

The four mobile drones are modeled as four independent service Providers. A
fixed requester/controller sends the same idempotent service workload to all
systems. Existing three-Provider baseline results remain historical evidence
and are not overwritten or silently combined with this campaign.

## User Stories & Acceptance Scenarios

### User Story 1 - Matched four-Provider mobility run (Priority: P1)

As a researcher, I can run NDNSF, gRPC, and NSC against the same four-Provider
mobility trace, AP geometry, workload, and deadline so that recovery behavior
is comparable.

**Independent acceptance**:

1. Given the same trace and seed, each system receives the same logical request
   count and provider availability schedule.
2. When one Provider leaves coverage, NDNSF may select another eligible
   Provider, while the primary `gRPC-SEQ-4` and NSC-4 baselines advance through
   the endpoint list one Provider at a time under the same global deadline.
   `gRPC-HC-4` may use proactive health probes only when explicitly enabled and
   remains a separately labelled sensitivity condition.
3. Every logical request produces exactly one terminal outcome and records the
   attempts, failovers, selected Provider, and latency.

### User Story 2 - Coverage and speed sensitivity (Priority: P1)

As a researcher, I can vary AP coverage and drone speed independently so that
the comparison distinguishes isolated handoffs from periods in which all
Providers are unavailable.

**Independent acceptance**:

1. The primary campaign runs one physical AP with four Providers and a
   declared coverage radius; three-AP layouts remain an optional secondary
   diagnostic with registered AP positions.
2. The campaign can run slow, medium, and fast deterministic mobility traces.
3. The trace report identifies per-Provider availability, at-least-one-Provider
   availability, and all-Provider outage intervals.

### User Story 3 - Evidence suitable for a mobility claim (Priority: P1)

As a paper author, I can decide whether NDNSF has a mobility advantage using
   paired run-level evidence rather than a favorable single point estimate.

**Independent acceptance**:

1. A smoke run completes in under ten seconds and proves topology, four
   Provider startup, failover, and summary parsing without producing a
   performance claim.
2. Formal runs use a 60-second measured window after warm-up and retain exact
   commands, source hashes, traces, configuration, logs, and terminal status.
3. The analysis reports logical success/deadline misses, recovery gap,
   attempts/request, failovers/request, latency percentiles, and Provider
   execution counts for every system.
4. A positive NDNSF claim is admitted only when matched paired runs show a
   pre-registered improvement in harsh mobility conditions; otherwise the
   result is reported as no demonstrated advantage.

## Functional Requirements

- **FR-001**: The experiment SHALL use four mobile service Providers and one
  fixed requester/controller for the primary comparison.
- **FR-002**: NDNSF, gRPC, and NSC SHALL receive the same idempotent workload,
  request rate, service delay, global deadline, mobility seed, and AP geometry.
- **FR-003**: The primary `gRPC-SEQ-4` and NSC-4 baselines SHALL know all four
  Provider endpoints but SHALL attempt at most one Provider at a time for each
  logical request, without proactive health-based request reordering. Retries
  SHALL consume the same logical deadline and SHALL be counted explicitly.
- **FR-003a**: `gRPC-HC-4` MAY retain proactive health probes as a secondary
  diagnostic only when an explicit campaign flag enables them. Its
  health-directed selections and probe traffic SHALL be labelled separately and
  SHALL NOT substitute for `gRPC-SEQ-4` in SC-005. The default campaign SHALL
  disable the oracle.
- **FR-004**: NDNSF SHALL be allowed to use its configured multi-Provider
  selection behavior. The campaign SHALL record whether the request used one
  selected Provider or multiple selected Providers.
- **FR-005**: The primary campaign SHALL use one physical AP and four
  Providers, with a declared coverage radius and deterministic Provider
  traces; three-AP layouts MAY remain as secondary diagnostics.
- **FR-006**: The campaign SHALL support at least three declared speed bands:
  slow (2 m/s), medium (8 m/s), and fast (15 m/s). The realized trace speed
  SHALL be retained in the manifest.
- **FR-006a**: The campaign SHALL support a registered
  `single-active-handoff` trace profile in which exactly one Provider is in
  declared AP coverage at each epoch and the active Provider rotates on a
  fixed, manifest-recorded period. This is coverage-gated stress evidence, not
  a physical RF association claim.
- **FR-007**: The campaign SHALL distinguish application availability from
  physical Wi-Fi association. Unless association events are directly measured,
  results SHALL be labeled coverage-gated mobility evidence rather than RF
  handoff evidence.
- **FR-008**: Every logical request SHALL have one stable request identifier,
  one terminal result, a bounded attempt list, and an explicit deadline.
- **FR-009**: The harness SHALL record per-Provider availability and derive
  at-least-one-available and all-Providers-unavailable intervals from the
  trace, without exposing those files to clients as an oracle.
- **FR-009a**: The gRPC health condition SHALL be identified as an
  application-level experiment oracle unless a standard `grpc.health.v1`
  service is explicitly registered and configured. No client SHALL infer
  health routing merely from having a list of endpoint addresses.
- **FR-010**: Smoke mode SHALL be shorter than ten seconds and SHALL not be
  used to claim success-rate, latency, or statistical superiority.
- **FR-011**: Formal mode SHALL use a 60-second measured window, at least
  5--10 seconds of warm-up, sampled timeline traces, and no automatic rerun of
  a failed cell.
- **FR-012**: Results SHALL be written to a unique campaign directory with
  machine-readable cell summaries, paired trace hashes, source hashes, and an
  aggregate comparison table.
- **FR-013**: The campaign SHALL preserve `specs/169-three-provider-mobility-baselines`
  evidence and SHALL not rewrite the historical single-endpoint NDNSF row.
- **FR-014**: The slide and paper mobility claim SHALL remain marked as
  historical/diagnostic until this campaign produces trace-paired evidence.
- **FR-015**: The harness SHALL support a strictly trace-paired timeout
  sensitivity condition with the common logical deadline held at 5 s and the
  gRPC/NSC per-attempt timeout changed from the registered 1 s to 2 s. NDNSF
  SHALL remain `FirstResponding`; when `ackTimeoutMs=2 s` is passed, the
  manifest SHALL state that this does not delay first-successful-ACK selection
  or change the current selection semantics.
- **FR-016**: A publication comparison SHALL use an independent holdout whose
  seeds were not used to choose the reported range/speed condition. NDNSF,
  `gRPC-SEQ-4`, and the diagnostic no-failover `gRPC-1` control SHALL use the
  same trace hash and the same trace-relative measurement start for each seed.
  A cell whose traffic-start offset differs by more than the registered
  tolerance SHALL be rejected rather than pooled.
- **FR-017**: The publication analysis SHALL report logical success,
  successful-response mean and tail latency, attempts or Provider executions,
  failovers, measurement-window coverage fractions, paired run-level
  confidence bounds, and input hashes. `gRPC-1` SHALL be labelled as a
  diagnostic fixed-endpoint control rather than the primary fair baseline.
- **FR-018**: NDNSF SHALL keep NDN-SVS parallel Sync receive processing and
  parallel Sync production as explicit opt-ins while fixed-trace replays show
  reconnect-sensitive delivery. User and Provider startup logs SHALL state
  whether each optimization is enabled. Either default may be restored only
  after paired reconnect regressions close every affected lifecycle stage.
- **FR-019**: A 35-second successful boundary replay SHALL NOT authorize matrix
  expansion by itself. The same corrected default SHALL complete an
  independent 60-second replay of the canonical seed-61 trace, and the report
  SHALL separately count (a) missing/late ACKs and (b) post-selection Response
  failures.
- **FR-020**: A reconnected Provider's newest synchronously published ACK state
  SHALL be discoverable within the ACK window. Publication completion and SVS
  sequence number, User-side state-vector receipt, mapping fetch, publication
  fetch, and ACK match SHALL be distinguishable in the evidence. The ACK-path
  cause and repair SHALL be established without changing Response retry
  behavior; shorter periodic Sync alone is not accepted as a repair. When
  `block_network=true` restores a Provider link, the harness SHALL recreate
  that Provider's NFD remote faces and bind the group and identity routes to
  the newly returned face IDs before treating the Provider as reconnected.
- **FR-021**: Only after FR-020 passes its independent replay gate, a selected
  Provider's missing Response SHALL trigger bounded retry/reselection within
  the unchanged global deadline. This Response-path mechanism SHALL be
  implemented and evaluated separately. Merely increasing a timeout does not
  satisfy either recovery requirement.

## Non-Functional Requirements

- **NFR-001**: A rerun with the same source tree, topology, trace, seed, and
  workload SHALL reproduce the same logical request schedule and trace hashes.
- **NFR-002**: The experiment SHALL use MiniNDN-WiFi for network validation and
  SHALL not use the host NFD as the final path.
- **NFR-003**: Logs SHALL separate application operations from wire-level
  packet/byte counts; one TCP segment SHALL not be treated as one RPC.
- **NFR-004**: The experiment SHALL retain negative cells and anomalous results
  instead of replacing them with successful reruns.

## Success Criteria

- **SC-001**: Four Provider NDNSF, gRPC, and NSC smoke cells complete within
  ten seconds of measured workload and emit parseable terminal summaries.
- **SC-002**: The formal campaign contains paired cells for all three systems
  under at least two one-AP coverage/speed conditions, each with a 60-second
  measured window and complete manifests.
- **SC-003**: Every formal cell reports 300 logical requests at 5 RPS, or
  records a terminal setup failure with the reason and partial counts.
- **SC-004**: The analysis can quantify the difference between logical success
  and retry/control cost for all three systems under identical traces.
- **SC-005**: A mobility advantage claim is accepted only if the registered
  `single-active-handoff` harsh paired condition shows NDNSF's lower confidence
  bound for logical success at least 10 percentage points above both
  sequential-retry baselines (`gRPC-SEQ-4` and NSC-4), while its median
  attempts/request remains no more than twice the lower baseline. `gRPC-HC-4`
  remains a separately reported diagnostic.
- **SC-006**: If SC-005 is not met, the output explicitly states that the
  campaign did not demonstrate an NDNSF mobility advantage and retains the
  negative result.
- **SC-005a**: A one-AP range/speed result SHALL be called an NDNSF advantage
  only when a registered paired cell has a positive run-level lower confidence
  bound against the corresponding sequential baseline and reports the
  all-unreachable fraction, retry cost, and latency alongside success rate.
- **SC-007**: A timeout sensitivity pilot SHALL use the same one-AP geometry,
  trace files, seeds, workload, admission setting, and health policy as its
  1 s/5 s counterpart, and SHALL report the failure stage at
  `ACK_MATCHED`, `PROVIDER_SELECTED`, or `RESPONSE_OBSERVED` whenever the
  corresponding lifecycle trace is available.
- **SC-008**: The confirmatory 50 m / 2 m/s holdout SHALL use five new seeds,
  a 4 s trace-relative measurement start, a 60 s measured window, 5 RPS,
  disabled admission and health-oracle routing, a 1 s attempt/ACK timeout, and
  a 5 s global deadline. A conditional multi-Provider advantage is admitted
  only when (a) the paired 95% run-level lower bound for NDNSF minus `gRPC-1`
  success is positive, (b) the corresponding lower bound versus
  `gRPC-SEQ-4` is at least -5 percentage points, and (c) the paired 95% upper
  bounds for both successful-response mean-latency and p95-latency ratios
  (`NDNSF / gRPC-SEQ-4`) are below 1.0. Otherwise the figure SHALL retain the
  measurements and state that the holdout did not confirm the conditional
  claim.
- **SC-009**: On the fixed 100 m / 10 m/s / seed-61 diagnostic trace, the
  correctness-first default SHALL complete all 175 requests in the 35-second
  boundary window, and all 175 requests SHALL reach Provider Request fetch,
  ACK publish, User ACK fetch, ACK match, Provider selection, and successful
  Response without explicitly setting the parallel-production environment
  variable.
- **SC-010**: Three independent 60-second replays of the fixed seed-61 trace
  SHALL show no terminal timeout caused by a missing or late ACK when at least
  one Provider was reachable at Request publication and that Provider completed
  synchronous ACK publication within the 500 ms ACK window. Publication entry,
  completion, sequence number, User fetch, and ACK match SHALL be recorded.
  Each run receipt SHALL record that NFD reconnect face repair was enabled for
  the NDNSF and NSC systems.
- **SC-011**: After SC-010 passes, three independent 60-second replays SHALL
  show that every post-selection missing Response either succeeds through a
  bounded retry/reselection attempt or terminates only at the unchanged 5 s
  global deadline after the bounded alternatives are exhausted. Until both
  gates pass, wider range/speed results remain diagnostic and no NDNSF
  mobility-advantage claim may be made.
- **SC-012**: The post-repair 50 m extension SHALL replay the same deterministic
  300-second-burn-in mobility seeds 62--71 used by the 100/150 m campaign,
  execute NDNSF, `gRPC-SEQ-4`, and NSC-4 for 300 logical requests per seed,
  and treat the mobility seed as the unit of inference. The report SHALL show
  every seed-level success rate, mean/median/standard deviation and a
  seed-bootstrap 95% interval, paired seed-level system differences, realized
  reachable-Provider coverage, and seed-level p95 latency. Pre-repair and
  no-burn-in 50 m results SHALL remain diagnostic and SHALL NOT be pooled with
  this extension.

## Key Entities

- **Logical request**: One application operation with a stable identifier,
  deadline, terminal outcome, and bounded attempts.
- **Provider trace**: Time-indexed Provider position, AP coverage state, speed,
  and deterministic seed.
- **AP layout**: Declared AP coordinates and coverage radius used by the trace.
- **System cell**: One system, trace, AP layout, speed, workload, and terminal
  result.
- **Campaign manifest**: Immutable configuration, source hashes, commands,
  trace hashes, and cell registry.

## Assumptions

- Four drones correspond to four mobile service Providers; the requester is
  fixed. This is the default interpretation of the mobility requirement.
- The service operation is idempotent and side-effect free, so retry attempts
  can be counted without changing application semantics.
- The first implementation uses deterministic coverage gating to isolate
  Provider availability. Physical association/handoff claims require a later
  instrumented Mininet-WiFi mode.
- The existing three-Provider campaign is a baseline reference, not a paired
  result for this four-Provider campaign.

## Scope Boundaries

- No change to NDNSF service protocol semantics.
- No GPU, model, or distributed-inference workload.
- No claim that multi-AP coverage alone proves physical wireless handoff.
- No replacement of the existing Spec169 evidence.
