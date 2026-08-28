# Implementation Plan: Single-Worker RSA Publication Offload

**Branch**: `136-rsa-sign-verify-offload` | **Revision**: R3, 2026-07-23  
**Spec**: [spec.md](spec.md)

## Summary

Modify the current NDN-SVS tree once and build one binary with two runtime
modes. Each peer uses an independent application pacer. Control posts each
publication call to Face, while treatment directly invokes the byte-oriented
worker-backed `publishAsync()` from the pacer and uses exactly one FIFO
publication worker. Each formal cell uses two MiniNDN processes; both processes
publish and subscribe concurrently and use the same selected mode.

## Controlling Documents

R3 is controlled only by `spec.md`, `plan.md`, `tasks.md`,
`contracts/*.md`, and the R1 pre-implementation audit. The untouched
`research.md`, `data-model.md`, and `quickstart.md` describe the superseded R0
two-binary/multi-path design and are non-controlling. `traceability.md` is the
supplementary R3 coverage map.

## Source-Verified Starting Point

CodeGraph on clean NDN-SVS commit
`6bb34545b4f89f1f6c265a68c18f1a40ade413eb` confirms:

- `SVSPubSub::publishAsync()` calls
  `prepareAndStageAsyncPublication()` and `prepareReservedBytes()`;
- `prepareReservedBytes()` constructs, encodes, and signs inner and outer Data
  before posting `onPreparedPublication()` to the Face `io_context`;
- both Data sign calls are serialized by `m_asyncPublishSigningMutex`;
- `SecurityOptions` independently supplies `interestSigner`, `dataSigner`,
  `pubSigner`, `validator`, and `encapsulatedDataValidator`;
- V2 non-HMAC Sync Interests pass through
  `SecurityOptions.validator`; therefore RSA-signed Sync Interests can be
  validated through the normal validator branch;
- current `SVSPubSubOptions` has no publication-preparation worker option.

This establishes both necessity and the minimal change: move the already
separable preparation stage, not the Face-owned commit/state path.

## Constitution Check

- **CodeGraph first**: current publish/sign/validator ownership was checked
  against the indexed NDN-SVS source before this design was revised.
- **Minimal scope**: one intervention, one binary, one worker, two nodes, and
  ten formal cells; unrelated NDN-SVS and NDNSF mechanisms are excluded.
- **Evidence before claims**: focused correctness/security/harness preflights
  block formal execution, and negative formal outcomes cannot be replaced.
- **MiniNDN boundary**: formal verification uses two real MiniNDN processes and
  the required 60-second measured window.
- **Compatibility and rollback**: worker count defaults to zero and rollback
  is disabling the opt-in mode.

**Gate**: design may proceed to implementation only after the R1
pre-implementation audit records PASS with no HIGH/CRITICAL finding.

## Technical Design

### One binary, two modes

Add opt-in configuration equivalent to:

```cpp
SVSPubSubOptions::publicationPreparationWorkers = 0; // existing inline behavior
SVSPubSubOptions::publicationPreparationQueueCapacity = N;
```

Accepted values for Spec 136 are only 0 and 1:

- `0` (`face-inline-rsa`): preserve current execution.
- `1` (`worker-rsa`): enqueue immutable preparation work to one FIFO worker.

The benchmark selects the mode at runtime and records one binary hash. Existing
callers keep the default `0`; no second API or compatibility alias is needed.
The byte-oriented worker-backed overload is safe to invoke from the APP pacer:
its input is copied before return, sequence reservation is serialized, Core
sequence reads are mutex-protected, preparation is worker-owned, and all
state/store/advertisement work returns to Face. Other overloads receive no new
off-Face guarantee in Spec 136.

### Ownership

| Work | Control owner | Treatment owner |
|---|---|---|
| Offered-load pacing | Independent APP pacer | Independent APP pacer |
| Publication API invocation | Face after APP post | APP pacer directly |
| Admission and sequence reservation | Face | APP caller under Core/SVS locks |
| Inner/outer construction and encoding | Face | One FIFO worker |
| Publication Data RSA signing | Face | One FIFO worker |
| State, mapping, DataStore, advertisement | Face | Face |
| Face I/O and callbacks | Face | Face |

The APP pacer receives no Face responsibilities. In control it posts a
copy-owned publication task to Face. In treatment `publishAsync()` copies the
input, reserves in call order, submits preparation to the worker, and posts one
prepared result back to Face. Since there is exactly one FIFO worker, R3 does
not introduce a
multi-worker reorder gate. KeyChain/TPM access remains serialized and the
treatment has one active publication signer.

The Data worker and Face protocol path share that one serialized signer.
Therefore both modes enable the existing 5 ms local-publication Sync batching
primitive. Batching coalesces state advertisement only; it does not move Sync
production to another worker or change the publication preparation
intervention.

Queue-full rejection happens before reservation. Accepted jobs receive exactly
one terminal state. A post-reservation failure prevents later commit for that
producer and is explicit; there is no synchronous fallback. Shutdown stops
admission and drains or explicitly cancels every accepted job before joining
the worker.

### Real RSA setup

Each MiniNDN process creates or loads its own persistent RSA-2048 identity.
Both `interestSigner` and publication Data signers use that identity with
`SignatureSha256WithRsa`. Each process installs the peer certificate and uses a
real validator for signed Sync Interests and fetched publication Data.

The benchmark sets `maxPiggyDataSize=1` in both modes. This forces fetched
outer/inner Data validation and avoids expanding R1 into a piggyback-security
change. A preflight accepts valid peer objects and rejects one tampered signed
Data and one tampered signed Interest before formal execution.

## Experimental Design

### Research question

Under identical two-node bidirectional PubSub load, does moving real RSA
publication preparation from the Face thread to one FIFO worker improve useful
capacity, Face responsiveness, or tail delivery delay?

### Experimental unit and workload

One cell contains two MiniNDN nodes and two independent application processes:

```text
node A: publish at R pps + subscribe to B
node B: publish at R pps + subscribe to A
aggregate offered publication load: 2R pps
```

Both processes use the same runtime mode in a cell. A mixed-mode cell is
invalid. Rate always means publications/s **per peer**.

### Formal matrix

One binary is frozen before execution. Exactly ten cells run once in this
fixed, paired order:

| Cell | Runtime mode | Rate/peer | Aggregate offered rate |
|---:|---|---:|---:|
| 01 | face-inline-rsa | 200 | 400 |
| 02 | worker-rsa | 200 | 400 |
| 03 | worker-rsa | 250 | 500 |
| 04 | face-inline-rsa | 250 | 500 |
| 05 | face-inline-rsa | 300 | 600 |
| 06 | worker-rsa | 300 | 600 |
| 07 | worker-rsa | 350 | 700 |
| 08 | face-inline-rsa | 350 | 700 |
| 09 | face-inline-rsa | 400 | 800 |
| 10 | worker-rsa | 400 | 800 |

The alternating within-pair order bounds monotonic host drift without adding
cells. There are no repetitions, retries, capacity-search cells, or mode
rebuilds.

### Fixed controls

- same source tree, binary, configuration schema, RSA identities, validators,
  pacing, instrumentation, and process placement;
- two MiniNDN nodes, 10 ms link delay, 100 Mbps, zero configured loss;
- 256-byte deterministic payload and `maxPiggyDataSize=1`;
- identical publication Fetch window `64` in both modes;
- one Face/io_context thread per process;
- parallel Sync processing/production and unrelated workers disabled;
- identical existing 5 ms local-publication Sync batching in both modes;
- one publication worker only in treatment;
- 10 s warmup, 60 s measurement, 10 s drain;
- same four-core host limits; no dedicated-core or >4-core assumption.

### Admission preflight

Before the manifest seals:

1. one binary hash and the exact 0-versus-1 worker runtime delta are proved;
2. thread evidence proves zero publication workers in control and exactly one
   FIFO publication worker in treatment;
3. packet inspection proves RSA signature type on Data and Sync Interests;
4. valid objects pass and tampered Data/Interest fail real validation;
5. queue-full, failure, ordering, and shutdown accounting tests pass;
6. both MiniNDN routes and bidirectional delivery pass;
7. a no-op 1000 pps/peer pacer reaches 980-1020 attempted pps on each peer;
8. thread IDs prove APP pacer differs from Face, control API calls execute on
   Face, and treatment API calls execute directly on the APP pacer;
9. every later measured cell repeats the per-peer 98%-102% admission check
   before any inline-versus-worker comparison is allowed.
10. signer lock-wait and crypto-service counters show estimated serialized
    signer utilization no greater than 90% at every formal rate;
11. every peer drains accepted work completely with no abandoned Face calls.
12. repeated Mapping announcements register each subscription once, and
    delivered/attempted is at least 98% for a sustained-load result.

Preflight is not a formal performance cell.

The 2026-07-23 Face-timer smoke at 400/800/1000 is retained as
`HARNESS_INVALID`: one peer starved in every cell and no measured publication
was delivered. It motivated this R2 correction but is not a baseline and cannot
support a worker-effect claim.

The corrected remote-prefix 800/1000 smoke is also retained, but R3 classifies
it as `OVERLOAD_INVALID`: measured R2 RSA service time gives a serialized
signer ceiling of approximately 626.6 publications/s per peer before other
Face work, so those rates cannot test the intended execution-location effect.
The R3 600-pps diagnostic remains below that signer-only ceiling but exhibits
random peer starvation on the four-core host; it is `RESOURCE_INVALID` and
also excluded from the formal matrix.

### Measurements and analysis

For each peer record:

- scheduled, attempted, accepted, committed, delivered, rejected, failed,
  cancelled, duplicate, missing, and invalid;
- attempted/scheduled, delivered/attempted, and delivered pps;
- publication call/admission duration;
- Face heartbeat and release lateness p50/p95/p99/max;
- delivery delay p50/p95/p99/max;
- publication Data RSA sign CPU;
- publication Data RSA signer wait/service;
- Sync Interest RSA signer wait/service and receive-side RSA verify CPU;
- worker queue wait/service/depth;
- Face commit residence, CPU, RSS, and terminal reasons.

Apply SC-005/SC-006 exactly. One cell per condition permits descriptive
contrasts only; do not compute p-values. Moving queueing away from Face is not
reported as cryptographic acceleration.

Delivery-delay percentiles are comparable only when the paired delivery ratios
differ by at most one percentage point. When one mode is
`LOAD_UNSUSTAINED`, retain its delivered-sample percentile as a censored
diagnostic, label it `delivered-only`, and do not use it as independent latency
evidence. Report heartbeat skipped ticks alongside heartbeat p99 because the
timer intentionally skips missed 1 ms deadlines.

A two-cell confirmation is non-formal descriptive evidence. It may establish
that one observed control cell missed the offered rate while its paired worker
cell sustained it, but it cannot produce the formal SC-006 verdict or a
population/general deployment claim.

## Implementation And Verification Boundary

Implementation files may include the focused NDN-SVS option/worker change, one
two-peer benchmark, one build/manifest driver, one analyzer, and focused tests.
Do not reuse or rerun Spec 135 or create another Spec.

Rollback is defaulting/disabling the opt-in worker (`workers=0`). Nothing is
installed globally or promoted to production by Spec 136.

## T004b Admission Closure

The execution-ready preflight is
`results/spec136-rsa-single-worker/preflight-r6-20260723T224509Z`.
It uses the frozen R6 binary and the unchanged NDN-SVS library recorded in
`build/spec136-rsa-single-worker-r6/build-manifest.json`.

- Both peer security probes accept valid RSA Data/Interests, reject tampered
  Data/Interests, and record zero tampered processing callbacks.
- For each runtime mode, two independent 60-second no-op processes each
  schedule and attempt exactly 60,000 releases at 1000 pps. Control records all
  60,000 calls on its io_context thread; treatment records all 60,000 on its
  APP pacer thread. All four processes record zero RSA, publication, Sync, and
  Fetch work.
- Fresh inline and worker 200-pps MiniNDN smokes each deliver 600/600 measured
  publications per peer with real peer validation, zero invalid objects, zero
  worker outstanding, and zero abandoned Face calls.
- The exact ten-cell matrix is frozen in
  `build/spec136-rsa-single-worker-r6/sealed-formal-manifest.json`.
  Sealing does not execute or authorize an implicit campaign; T005 requires an
  explicit `--formal --sealed-manifest ...` command.

R4 and R5 are retained but superseded. R4's manifest made future explicit T005
execution impossible. R5 admitted the subject, but its formal loop would stop
after the first non-`COMPLETE` receipt and could therefore selectively omit
later registered cells. R6 removes only that fail-fast behavior before the
full preflight and seal. No formal cell ran under R4 or R5.

## Formal Execution And Analysis Closure

The R6 manifest was executed exactly once at
`results/spec136-rsa-single-worker/formal-r6-20260723T224836Z`. All ten cells
completed in registered order with one receipt per cell. The analyzer produced
five admissible paired contrasts and the overall `TRADE_OFF` verdict:
200/250/300/350 pps were `NOT_USEFUL_AT_RATE`; 400 pps was
`USEFUL_AT_RATE` because delivery p99 improved by 33.71% with equal 100%
delivery, despite a 70.96% heartbeat-p99 regression. One useful rate does not
satisfy SC-006.

The campaign and its negative/positive outcomes are frozen. The post-
implementation audit is `CONDITIONAL PASS` for experiment closure because the
central SC-005/SC-006 question is answered, while incomplete FR-015 metrics and
missing worker-specific failure-injection evidence prohibit production or
complete-observability claims.
