# Feature Specification: Serial Sync-Production Offload Proof

**Feature Branch**: `137-svs-serial-production-offload`

**Created**: 2026-07-23

**Status**: Planned; implementation and formal execution not started

**Input**: User description: "Use the simplest proof: on the same latest
NDN-SVS source and the same binary, compare serial production inside the Face
thread with serial production on one worker. Use Spec Kit to produce a detailed
design and plan."

## Evidence Boundary

- "Latest" means the clean NDN-SVS commit that was current when this
  specification was created:
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`. It is an immutable subject, not
  a floating branch name.
- The treatment is **where NDN-SVS Sync Interest production executes**. It is
  not a comparison of `SVSPubSub` publication-Data signing algorithms, not an
  RSA scaling experiment, and not a comparison of old and new commits.
- Both treatments use one build and one binary. A runtime treatment field is
  the only permitted behavioral difference.
- Spec 133, Spec 135, and Spec 136 are read-only context. Their runners,
  receipts, result directories, and claims MUST NOT be rerun, amended, or
  imported as Spec 137 formal evidence.
- Spec 137 is a causal mechanism experiment. It may conclude that the worker
  is beneficial, neutral, or harmful. A negative result is valid evidence and
  MUST NOT trigger selective reruns or parameter tuning.

## User Scenarios & Testing

### User Story 1 - Isolate Execution Location (Priority: P1)

As an NDN-SVS researcher, I want two executions that differ only in whether
serial Sync Interest production runs on the Face thread or one worker, so any
measured difference has a defensible cause.

**Why this priority**: Earlier cross-version and multi-worker comparisons
changed several variables at once. Without a single-variable contrast, no
performance conclusion is trustworthy.

**Independent Test**: Inspect one sealed manifest and two startup records from
the same campaign. They identify the same source tree, patch set, binary hash,
security settings, topology, workload, CPU allocation, and receive behavior;
only `production_mode` differs.

**Acceptance Scenarios**:

1. **Given** one frozen binary, **when** it starts in `face-serial` mode,
   **then** Sync Interest snapshot, extension construction, encoding, and
   signing complete on the Face thread before transmission.
2. **Given** the same frozen binary, **when** it starts in `worker-serial` mode,
   **then** those production stages execute on exactly one worker and the Face
   thread performs only admission and final transmission.
3. **Given** either treatment, **when** any other controlled setting differs,
   **then** the cell is rejected before it can contribute evidence.

---

### User Story 2 - Prove Seriality And Accounting (Priority: P1)

As a reviewer, I want direct evidence that the treatment did not gain hidden
signer parallelism, drop production work, or fall back to Face execution, so
the experiment proves offload rather than a different mechanism.

**Why this priority**: A one-worker label alone is insufficient. Queue-full
fallback could overlap signing on the Face thread, and missing work could make
the worker appear faster.

**Independent Test**: Run the treatment preflight and verify that maximum
concurrent Sync signers is one, every admitted production job has one terminal
outcome, worker fallback is zero, and stage counts reconcile.

**Acceptance Scenarios**:

1. **Given** `worker-serial`, **when** the cell ends, **then**
   `max_active_sync_signers` equals one and queue-full fallback equals zero.
2. **Given** either mode, **when** counters are reconciled, **then** every
   production trigger is classified as completed, stale, failed, or explicitly
   rejected without an unexplained remainder.
3. **Given** a queue overflow, stale result, production failure, thread
   mismatch, or shutdown loss, **when** admission is evaluated, **then** the
   cell is retained as a failed/inadmissible result and is not replaced.

---

### User Story 3 - Measure Face Relief And User-Visible Effect (Priority: P2)

As an NDN-SVS maintainer, I want both direct Face-thread measurements and
end-to-end PubSub outcomes at one pre-registered stress rate, so I can tell
whether the offload merely moves work or materially improves responsiveness.

**Why this priority**: Shorter production CPU time is mechanistic evidence, but
the feature matters only if it also improves Face-loop responsiveness,
delivery, or the tested capacity boundary without unacceptable harm.

**Independent Test**: Validate the pre-registered 60 pps workload with one
non-formal two-mode pair, then run three paired repetitions per treatment and
generate a report containing
run-level paired contrasts for Face heartbeat delay, Face production CPU,
delivery, publication release, production queueing, and traffic.

**Acceptance Scenarios**:

1. **Given** the four-core preflight passes, **when** the campaign is sealed,
   **then** the fixed 60 pps rate and six formal cells are recorded before the
   first formal cell begins.
2. **Given** six terminal formal cells, **when** analysis runs, **then** it
   reports every run and paired difference without pooling packet samples as
   independent experimental replicates.
3. **Given** no practically meaningful improvement or any regression,
   **when** the report closes, **then** it states that outcome instead of
   searching for a more favorable rate.

---

### User Story 4 - Preserve Reproducible Evidence (Priority: P3)

As a future researcher, I want immutable build, execution, and analysis
artifacts, so the result can be checked without relying on the active checkout
or previous experiments.

**Why this priority**: Performance claims are not durable if the tested source,
binary, runtime treatment, or failed cells cannot be reconstructed.

**Independent Test**: Re-run the offline verifier against the closed result
directory and confirm all source, patch, binary, configuration, receipt, and
analysis hashes.

**Acceptance Scenarios**:

1. **Given** a completed build, **when** admission runs, **then** Boost 1.71
   linkage and every source/patch/binary hash are recorded and verified.
2. **Given** a formal cell has started, **when** it terminates for any reason,
   **then** exactly one immutable receipt is written and no retry is scheduled.
3. **Given** a closed campaign, **when** any protected artifact changes,
   **then** verification fails and the existing conclusion is not silently
   regenerated.

### Edge Cases

- The fixed 60 pps diagnostic pair fails offered-load or mechanism invariants.
  The campaign closes as `FIXED_RATE_INADMISSIBLE`; it does not search a rate
  that produces a more favorable result.
- Face mode misses publication release deadlines. Scheduled, attempted,
  committed, advertised, and delivered counts remain separate; missed releases
  are not relabeled as network loss.
- Worker mode moves latency into its queue. Queue wait and service time are
  reported separately, and a growing queue is not described as eliminated
  work.
- A worker result observes a newer state generation. Current behavior may send
  the already built result (`stale_sent`) or discard a result that cannot be
  safely completed (`stale_dropped`). The analyzer keeps these distinct:
  `stale_sent` is a subset of completed work, while `stale_dropped` is terminal.
- A queue-full path invokes serial fallback. The cell is retained but excluded
  from the clean offload contrast because signing may have overlapped across
  worker and Face.
- The worker reduces Face delay but harms delivery or increases useless
  traffic. The report classifies a trade-off, not an unconditional win.
- Instrumentation overhead differs across modes or exceeds the pre-registered
  budget. Formal execution is blocked until the common instrumentation is
  corrected.
- A process terminates before worker drain and counter flush. The receipt is
  terminal failed; no replacement run is allowed.

## Requirements

### Functional Requirements

- **FR-001**: The experiment MUST use exact clean NDN-SVS commit
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb` plus one recorded common
  Boost-1.71 compatibility patch and one recorded common measurement patch.
- **FR-002**: Both treatments MUST be produced by one build and MUST execute
  the same binary hash; treatment selection MUST occur only through a recorded
  runtime option.
- **FR-003**: The two runtime treatments MUST be exactly:
  `face-serial`, with parallel Sync receive processing and parallel Sync
  production disabled; and `worker-serial`, with parallel Sync receive
  processing disabled and Sync production enabled with exactly one worker,
  worker-side extension construction, and worker-side Sync Interest signing.
- **FR-004**: The experiment MUST label the mechanism as **Sync Interest
  production offload**. It MUST NOT imply that publication inner/outer Data
  preparation or signing moved from the application caller, and MUST NOT make
  an RSA or multi-signer scaling claim.
- **FR-005**: Both modes MUST use identical protocol version, security
  algorithms, signing identities, Sync timing, batching setting, payload,
  publication API, receive path, topology, routes, NFD configuration, CPU
  allocation, warmup, measured interval, drain interval, and logging level.
- **FR-006**: Each formal cell MUST consist of two MiniNDN nodes and two
  independent application processes. `peer-a` is the sole active publisher and
  also subscribes; `peer-b` is a fixed receiver. Only `peer-a` changes between
  `face-serial` and `worker-serial`.
- **FR-007**: Each formal cell MUST use 10 seconds warmup, 60 seconds
  measurement, and 10 seconds drain. The measured payload MUST be 256
  deterministic bytes and must identify campaign, cell, sender, logical
  publication, and scheduled release.
- **FR-008**: The application release loop MUST execute outside the Face
  thread and call the same `publishAsync()` path in both treatments. The Face
  MUST have exactly one event thread in both treatments.
- **FR-009**: Common instrumentation MUST report, per process and phase:
  production triggers, admissions, completions, stale results, failures,
  queue-full fallbacks, queue depth, queue wait, worker service, snapshot,
  extension construction, encoding, Sync signing, Face finalization, and
  transmission counts and timing.
- **FR-010**: Instrumentation MUST directly report execution thread identity,
  maximum concurrent Sync signers, Face-thread CPU time, worker CPU time, and a
  periodic Face heartbeat's scheduled-versus-observed delay.
- **FR-011**: Publication accounting MUST separately report scheduled,
  attempted, API-returned, committed, advertised, remotely delivered,
  duplicate, invalid, timeout, Nack, Mapping fallback, and Payload fallback
  counts where each concept applies. Counts MUST NOT be substituted for one
  another.
- **FR-012**: A cell is admissible for the clean causal contrast only when
  source/binary/configuration identity passes, the sole publisher attempts
  within +/-2% of 60 pps, `max_active_sync_signers=1`, queue-full fallback is zero,
  unexplained accounting remainder is zero, and both processes produce
  complete resource and event records.
- **FR-013**: Before formal execution, a no-op pacer preflight MUST demonstrate
  the requested rate within +/-2% without NDN-SVS work, and an instrumentation
  preflight MUST demonstrate no more than 5% throughput cost and no more than
  10% Face-heartbeat-p99 cost relative to the same binary with detailed
  measurement disabled.
- **FR-014**: One non-formal diagnostic pair MUST validate the pre-registered
  rate of `60` publications/s from the sole publisher. It MUST NOT search,
  tune, or replace the rate after observing either mode. Diagnostic output MUST
  be stored separately and MUST NOT support the formal causal conclusion.
- **FR-015**: After the pilot freezes the rate, the runner MUST seal exactly
  six formal cells: three paired repetitions of `face-serial` and
  `worker-serial`, with an alternating, pre-declared order and fixed cell
  identifiers. It MUST execute every cell once without automatic or selective
  retry.
- **FR-016**: Formal analysis MUST treat each run as the experimental unit,
  publish all six outcomes, and report paired absolute and relative
  differences. Packet- or heartbeat-level observations MUST NOT be treated as
  independent replicates.
- **FR-017**: The primary mechanistic endpoints MUST be Face-thread Sync
  production CPU per completed production and Face heartbeat p99 delay. The
  primary user-visible endpoints MUST be attempted-rate fidelity, remote
  delivery ratio, and delivery p99 latency.
- **FR-018**: The conclusion MUST use the pre-registered outcome taxonomy:
  `SUPPORTED`, `FACE_RELIEF_ONLY`, `TRADE_OFF`, `NO_MEANINGFUL_EFFECT`,
  `WORKER_WORSE`, `INADMISSIBLE`, or `FIXED_RATE_INADMISSIBLE`.
- **FR-019**: The analyzer MUST report production queue wait and worker
  service separately from Face finalization, and MUST report Sync, Mapping,
  Payload, retry, timeout, Nack, and fallback traffic so moved work and useless
  traffic remain visible.
- **FR-020**: Build and run tooling MUST leave the active NDN-SVS checkout and
  active Git refs unchanged, reject non-Boost-1.71 linkage, and record compiler,
  libraries, source tree, common patch, binary, command, environment, topology,
  and configuration provenance.
- **FR-021**: All Spec 137 artifacts MUST live under Spec 137-owned build and
  result paths. Formal closure MUST verify that Spec 133, Spec 135, and Spec
  136 protected artifacts remain unchanged.
- **FR-022**: Formal execution MUST remain blocked until unit, thread-identity,
  one-signer, fallback, shutdown/drain, counter-conservation, two-process
  MiniNDN smoke, and pre-implementation audit gates pass.

### Key Entities

- **FrozenSubject**: Exact source commit/tree, common patches, compiler, Boost
  linkage, binary hash, and immutable build manifest shared by both modes.
- **RuntimeTreatment**: The sole causal variable: `face-serial` or
  `worker-serial`, plus the fully expanded runtime settings used to prove no
  hidden difference.
- **PilotObservation**: Non-formal rate/mode result used only by the
  deterministic stress-rate selection rule.
- **SealedCampaign**: Frozen stress rate, ordered six-cell matrix, hashes,
  seeds, topology, timing, admission thresholds, and single-writer lock.
- **FormalCell**: One two-peer execution with one immutable terminal receipt
  and raw per-process events/resources.
- **ProductionLifecycle**: Trigger, admission, queue, production stages,
  terminal outcome, Face finalization, and transmission for one Sync production
  operation.
- **PublicationLifecycle**: Scheduled release through remote delivery for one
  application publication.
- **PairedContrast**: Run-level difference between the two treatments for the
  same pair index and frozen controls.
- **OutcomeClassification**: One pre-registered conclusion plus supporting and
  limiting evidence.

## Success Criteria

### Measurable Outcomes

- **SC-001**: One verifier proves that every admitted formal cell used the same
  source tree, patch bytes, binary hash, security settings, topology, timing,
  workload, and CPU allocation, with runtime treatment as the only difference.
- **SC-002**: Every admissible worker cell reports exactly one maximum active
  Sync signer, zero queue-full fallback, zero unexplained production-accounting
  remainder, and complete two-process evidence.
- **SC-003**: The runner admits the pre-registered 60 pps workload or closes
  with `FIXED_RATE_INADMISSIBLE`; no operator-selected or searched rate is
  accepted.
- **SC-004**: If a rate is frozen, exactly six formal cells terminate once and
  yield six immutable receipts; no formal cell is retried, replaced, or hidden.
- **SC-005**: The final report provides all run-level values and three paired
  contrasts for both primary mechanistic endpoints, all primary user-visible
  endpoints, queue movement, and traffic overhead.
- **SC-006**: `SUPPORTED` is permitted only when all six cells are admissible,
  all three worker pairs reduce Face production CPU by at least 50%, at least
  two of three reduce Face-heartbeat p99 by at least 20%, no worker pair loses
  more than 1 percentage point of delivery ratio, and attempted-rate fidelity
  remains within +/-2% in both modes, and at least two of three pairs improve
  either delivery ratio or delivery p99 by at least 5% relative.
- **SC-007**: `FACE_RELIEF_ONLY` is used when the mechanistic thresholds in
  SC-006 pass but neither delivery ratio nor delivery p99 improves by at least
  5% in two of three pairs. It MUST NOT be reported as a capacity increase.
- **SC-008**: Any delivery harm greater than 1 percentage point, p99 latency
  harm greater than 10% in two of three pairs, production fallback, hidden
  concurrency, `stale_dropped>0`, `stale_sent/submitted>1%`, or a positive
  undrained final production queue, or more than 10% additional Sync Interests
  per committed publication in at least two pairs forces `TRADE_OFF`,
  `WORKER_WORSE`, or `INADMISSIBLE` according to the ordered decision table.
- **SC-009**: The report states only a result at the tested frozen rate. It
  makes no unmeasured exact-maximum, RSA, publication-signing, multi-worker, or
  general hardware claim.
- **SC-010**: Offline closure verifies all campaign hashes and confirms the
  active NDN-SVS checkout plus protected Spec 133/135/136 evidence were not
  modified or executed.

## Assumptions

- The current one-worker Sync production option is available at commit
  `6bb34545` and can be selected at runtime without a second build.
- HMAC-signed V2 Sync Interests and SHA-256-signed publication Data are retained
  because changing cryptography would confound execution-location isolation.
- The experiment is intended to prove whether existing offload is useful even
  with a serial signer; it does not attempt to maximize worker throughput.
- Three paired repetitions are sufficient for a small, pre-registered
  mechanism demonstration but not for a broad population-level statistical
  claim. The report uses practical thresholds and raw run-level values rather
  than a significance test.
- The fixed 60 pps rate is a bounded four-core mechanism workload, not an
  estimate of maximum capacity.
- MiniNDN remains the authoritative network environment; host NFD may be used
  only for diagnosis and cannot supply formal evidence.
