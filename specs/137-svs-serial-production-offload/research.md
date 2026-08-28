# Research: Serial Sync-Production Offload Proof

## Source Findings

### Current subject

- Clean NDN-SVS HEAD at specification time:
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`.
- The active NDN-SVS checkout was clean when inspected.
- The external NDN-SVS repository has no CodeGraph index, so its current source
  was inspected directly rather than indexed.

### Publication and production ownership

`SVSPubSub::publishAsync()` currently prepares and signs publication Data in
the application caller, stores prepared packets, and posts ordered commit to
the Face `io_context`. Face-side commit calls `SVSyncCore::updateSeqNo()`.
With Sync batching disabled, that update directly triggers local Sync Interest
production.

Therefore:

- publication inner/outer Data preparation is common application-thread work;
- Sync Interest production is the work whose execution location changes;
- claiming that this experiment offloads all publication signing would be
  factually wrong.

### Existing control path

When parallel production is disabled, `sendSyncInterestSerial()` snapshots the
version vector, obtains extension blocks, encodes the Sync Interest, signs the
V2 Interest, and calls `Face::expressInterest()` before returning.

### Existing treatment path

`setParallelSyncProduction(true, workers, queue, signInWorker,
buildExtraBlockInWorker)` enables a bounded worker pool. With values
`(true, 1, 4096, true, true)`, one worker builds extensions, encodes, and signs.
The result is posted to Face for staleness checking and transmission.

The existing code also has a queue-full `fallback_serial` path. That fallback
could overlap Face and worker signing, so a clean one-worker claim requires a
direct maximum-active-signer counter and zero fallback.

### Receive-side control

NDN-SVS also supports parallel Sync receive processing. It is disabled in both
treatments. Otherwise production offload would be confounded with receive
offload and additional workers.

## Decision 1: One Runtime Binary

**Decision**: Compile one binary containing both treatments and select the mode
through one validated runtime enum.

**Rationale**: This removes compiler, optimization, linkage, source-tree, and
instrumentation differences. The startup record can prove the expanded runtime
settings.

**Rejected alternatives**:

- Two commits or worktrees as treatments: changes source and build identity.
- Two compile-time macros: binary hashes differ and weaken attribution.
- Old versus new NDN-SVS: repeats the confounded comparison that motivated this
  feature.

## Decision 2: One Production Worker And No Receive Workers

**Decision**: Treatment uses exactly one production worker; both modes use
exactly one Face thread and no receive workers.

**Rationale**: The question is whether moving serial work off Face matters, not
whether parallel signing scales. One worker plus direct signer-concurrency
measurement makes the distinction falsifiable.

**Rejected alternatives**:

- Four production workers: conflates offload and parallel capacity.
- Receive worker pool: changes another large Face workload.
- A synthetic sleep worker: proves scheduling mechanics but not real NDN-SVS
  production.

## Decision 3: Preserve HMAC/SHA-256 Security

**Decision**: Retain V2 HMAC Sync Interests and SHA-256 publication Data in both
treatments.

**Rationale**: They are the current benchmark's common security settings and
require no trust-schema changes. Changing to RSA would combine execution
location with a cryptographic workload and overlap Spec 136.

**Rejected alternative**: RSA sign/verify in Spec 137. Real RSA validation,
sign/verify attribution, and ordered publication signing are already Spec 136's
separate research question.

## Decision 4: One Adaptively Frozen Formal Rate

**Superseded decision**: The original pilot grid assumed resources beyond the
known four-core host and is not used. Spec 137 now validates one pre-registered
80 pps sole-publisher workload with one face/worker diagnostic pair.

**Original rejected decision**: Use a pre-registered pilot grid as training data, deterministically
freeze one rate, then collect fresh formal data only at that rate.

**Rationale**: A low rate may show no Face pressure, while a collapsed rate may
measure Fetcher instability instead of production offload. Training/test
separation gives a small experiment without choosing a favorable formal result
after seeing it.

**Rejected alternatives**:

- Five formal rates per treatment: unnecessary for the narrow mechanism proof
  and repeats the campaign-size mistakes of earlier work.
- Select 1000 pps from prior evidence: Spec 133 shows that rate can be dominated
  by Fetcher fallback queues.
- Select the best-looking formal rate afterward: outcome-driven selection.
- Pure microbenchmark: cannot demonstrate Face-loop or PubSub effect.

## Decision 5: Six Formal Cells

**Decision**: Three paired repetitions per treatment in AB/BA/AB order.

**Rationale**: Three run-level pairs expose gross run-to-run instability and
support a modest mechanism result while remaining the simplest credible formal
campaign. Alternating order balances monotonic host drift.

**Statistical boundary**: Six runs do not justify a population-level p-value.
The report uses run-level practical thresholds, all raw values, median paired
effect, and range. Packet samples are not independent replicates.

## Decision 6: Direct Mechanism And User-Visible Endpoints

**Decision**:

- primary mechanism: Face Sync-production CPU per completion and Face heartbeat
  p99;
- primary user-visible: attempted-rate fidelity, delivery ratio, delivery p99;
- safeguards: worker queue wait/service/depth, staleness, fallback, traffic,
  thread identity, and maximum active signers.

**Rationale**: CPU stages alone prove moved work but not value. Delivery alone
cannot show causality. Both layers are necessary.

## Decision 7: Fail-Closed Evidence

**Decision**: Every started formal cell has one terminal receipt; invalid cells
remain visible and cannot be retried.

**Rationale**: Selective replacement would bias a six-cell experiment.
Queue-full fallback, stale results, crashes, and incomplete records must be
classified rather than repaired after formal execution begins.

## Validity Threats And Mitigations

| Threat | Mitigation | Remaining limitation |
|---|---|---|
| Hidden signer parallelism | active/max signer counter around all sign paths; fallback must be zero | Does not establish backend thread safety for >1 signer |
| Pacer failure | independent absolute-deadline pacer and no-op +/-2% preflight | Host scheduling still bounds highest rate |
| Instrumentation changes result | common patch and explicit overhead preflight | Measured binary remains instrumented |
| Fetcher collapse dominates | pilot requires >=95% delivery and reports fallback queues | Tested rate may be below maximum |
| Host drift | fixed CPU placement and AB/BA/AB order | Only one host is studied |
| Pseudoreplication | run is experimental unit | Three pairs support only a modest claim |
| Work merely moves queues | queue wait/service/depth/stale/fallback are mandatory | A longer run could reveal later instability |
| Claim overreach | fixed taxonomy and explicit exclusions | Other crypto/hardware/rates need new Specs |

## Falsification Conditions

The proposition "serial production offload is useful" is not supported if:

- Face production CPU does not materially fall;
- heartbeat improvement is absent or inconsistent;
- delivery or tail latency regresses beyond the no-harm bound;
- the worker queue grows, falls back, or loses/stales work;
- signing concurrency exceeds one;
- treatments differ beyond their runtime mode; or
- no jointly admissible stress rate exists.
