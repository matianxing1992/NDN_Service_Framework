# Implementation Plan: Serial Sync-Production Offload Proof

**Branch**: `137-svs-serial-production-offload` | **Date**: 2026-07-23 |
**Spec**: [spec.md](spec.md)

## Summary

Build one instrumented binary from exact clean NDN-SVS commit `6bb34545` and
use one runtime option to compare:

```text
face-serial:
  one Face thread
  Sync receive processing on Face
  Sync Interest production on Face

worker-serial:
  one Face thread
  Sync receive processing on Face
  Sync Interest production on exactly one worker
  extension build + encode + Sync signing on that worker
```

Only `peer-a` publishes. Its application pacer remains outside the Face thread
and invokes the same `publishAsync()` path in both modes; `peer-b` remains a
fixed receiver. One short non-formal pair validates the pre-registered 60 pps
workload. The formal campaign then runs only six cells—three
paired repetitions per mode—and reports direct Face CPU/heartbeat effects plus
end-to-end PubSub delivery. This isolates execution location without changing
source, binary, signing algorithms, receive parallelism, or worker count.

## Technical Context

**Language/Version**: C++17; Python 3.8+

**Primary Dependencies**: NDN-SVS
`6bb34545b4f89f1f6c265a68c18f1a40ade413eb`, ndn-cxx, Boost 1.71,
MiniNDN, NFD

**Storage**: One isolated NDN-SVS worktree/build; hash-bound JSON manifests;
JSONL process events; CSV resource samples, receipts, contrasts, and Markdown
report

**Testing**: NDN-SVS unit tests, treatment-contract tests, thread/signer
instrumentation tests, shutdown and conservation tests, Python runner/analyzer
tests, two-process MiniNDN smoke, non-formal pilot, exactly six formal cells

**Target Platform**: Existing Linux MiniNDN host with fixed per-process CPU
allocation

**Project Type**: NDN-SVS library instrumentation and pure PubSub benchmark;
NDNSF runtime is not involved

**Performance Goals**: Determine whether one-worker serial Sync production
reduces Face production CPU by at least 50% and Face-heartbeat p99 by at least
20% at one independently frozen stress rate without material delivery harm

**Constraints**: Same source/tree/patch/binary for both modes; one Face thread;
one production worker only in treatment; receive workers disabled; V2/HMAC Sync
and SHA-256 publication signing unchanged; 10/60/10 formal timing; no formal
retry; Spec 133/135/136 immutable

**Scale/Scope**: Two MiniNDN peers, one active publisher, one fixed receiver,
one two-cell non-formal diagnostic, one fixed 60 pps rate, six formal cells

## Constitution Check

- **CodeGraph/source first**: The framework CodeGraph index is current. The
  external NDN-SVS checkout is not indexed, so exact source was inspected
  directly. Current `SVSPubSub::publishAsync()` stages publication Data in the
  application caller, posts ordered commit to the Face `io_context`, and the
  Face-side commit calls `updateSeqNo()`. With batching disabled,
  `updateSeqNo()` immediately reaches `sendSyncInterest()`.
- **Single causal variable**: At current source, `sendSyncInterestSerial()`
  performs state snapshot/extension creation, Sync encode, V2 signing, and
  `Face::expressInterest()` inline. The production-worker path snapshots and
  enqueues, performs extension construction/encode/sign on its worker, then
  posts finalization/transmission to Face. A runtime option already selects
  these paths.
- **Correct boundary**: This feature measures **Sync Interest production
  offload**. Publication inner/outer Data signing stays in the application
  caller and is identical. Real RSA validation and ordered publication-signing
  offload remain Spec 136's scope.
- **MiniNDN authority**: Formal evidence uses two real node processes under
  MiniNDN with route and topology admission checks; host NFD is diagnostic only.
- **Frozen evidence**: Spec 133, 135, and 136 are never executed or modified.
  Spec 137 has unique build/results paths and verifies protected hashes at
  closure.
- **Cohesive tasks**: Instrumentation/treatment validity, harness admission,
  pilot/sealing, irreversible formal execution, and analysis/freeze are
  independently reviewable outcomes rather than one task per file.

**Pre-design gate**: PASS.

**Implementation gate**: BLOCKED until the pre-implementation audit has no
unresolved HIGH/CRITICAL finding and T001 records a PASS.

## Source-Verified Mechanism

### Common publication path

Both treatments use:

```text
application pacer thread
  -> SVSPubSub::publishAsync()
  -> prepare/sign/store publication Data in caller
  -> post ordered publication commit to Face io_context
  -> Face commits next sequence
  -> SVSyncCore::updateSeqNo()
  -> publication-triggered Sync Interest production
```

This common path deliberately preserves publication preparation and signing.
It cannot be cited as evidence that those stages were offloaded.

### Control: `face-serial`

```text
Face thread
  -> snapshot version vector
  -> build extension/piggyback block
  -> encode Sync Interest
  -> sign V2 Sync Interest
  -> express Interest
```

Runtime expansion:

```text
parallel_sync_processing = false
parallel_sync_production = false
sync_interest_batching = false
face_threads = 1
```

### Treatment: `worker-serial`

```text
Face thread
  -> snapshot version vector
  -> enqueue bounded job

single production worker
  -> build extension/piggyback block
  -> encode Sync Interest
  -> sign V2 Sync Interest
  -> post result

Face thread
  -> generation/staleness check
  -> express Interest
```

Runtime expansion:

```text
parallel_sync_processing = false
parallel_sync_production = true
production_workers = 1
production_queue = 4096
sign_in_worker = true
build_extra_block_in_worker = true
sync_interest_batching = false
face_threads = 1
```

Queue size is identical in every treatment repetition and is not tuned after
the pilot. Any queue-full serial fallback invalidates the clean treatment
because it may create Face/worker signer overlap.

### Signer-seriality proof

The common measurement patch wraps every Sync Interest signing entry—serial,
worker, and fallback—with one RAII probe:

```text
on entry:
  active_sync_signers += 1
  max_active_sync_signers = max(max, active)
  record thread role and thread CPU start

on exit:
  record wall and thread CPU duration
  active_sync_signers -= 1
```

The probe does not serialize or alter signing. The treatment is admitted only
when `max_active_sync_signers == 1` and fallback is zero. The existing
production signing mutex remains unmodified.

## Measurement Design

### Clocks and aggregation

- Monotonic lifecycle timestamps: `CLOCK_MONOTONIC_RAW`.
- Per-thread CPU stages: `CLOCK_THREAD_CPUTIME_ID`.
- Resource sampling: process/thread CPU, RSS, context switches, and queue depth
  every 100 ms.
- Face heartbeat: one 1-ms absolute-period timer; record scheduled and observed
  times, skip catch-up bursts, and report p50/p95/p99/max.
- High-frequency stage data: fixed 1% request-ID sampling plus unsampled
  aggregate counts/times. All failures and terminal anomalies are retained.
- Statistics: one formal run is one replicate. Report raw six-run results,
  three paired absolute/relative differences, median paired effect, and range.
  Do not compute packet-level p-values.

### Production stages

The profiler records non-overlapping stages:

| Stage | Control owner | Treatment owner |
|---|---|---|
| trigger-to-entry | Face | Face |
| state snapshot | Face | Face |
| queue wait | N/A | worker queue |
| extension construction | Face | worker |
| Sync encode | Face | worker |
| Sync sign | Face | worker |
| result-post wait | N/A | Face queue |
| generation/staleness check | N/A | Face |
| `expressInterest()` finalization | Face | Face |

Aggregate counters reconcile:

```text
face-serial:
  triggers = completed + failed

worker-serial:
  triggers
    = admission_rejected
    + submitted

  submitted
    = completed
    + stale_dropped
    + worker_failed
    + cancelled_on_shutdown

  stale_sent <= completed  # annotation, not another terminal outcome
  fallback_serial = 0  # clean-admission invariant
```

### End-to-end and traffic measurements

Each peer separately reports:

- scheduled, attempted, API returned, committed, advertised, remote delivered;
- invalid payload, duplicate, out-of-order, timeout, and Nack;
- Mapping/Payload piggyback and fallback counts;
- retry count and Interests/Data by category;
- offered and achieved rates;
- delivery ratio and scheduled-release-to-delivery p50/p95/p99;
- Sync Interest count per committed publication;
- Face heartbeat and per-thread CPU;
- production queue depth/wait/service/stale/failure/fallback.

The analyzer never adds overlapping Mapping and Payload paths into a synthetic
end-to-end latency.

## Experiment Design

### Research question

At a controlled stress rate, does moving the existing serial Sync Interest
production chain from the sole Face thread to one worker reduce Face-loop work
and responsiveness delay without changing delivery correctness or merely
moving instability into a worker queue?

### Hypotheses

- **H1-Mechanism**: Treatment reduces Face-thread Sync production CPU per
  completed production by at least 50%.
- **H2-Responsiveness**: Treatment reduces Face-heartbeat p99 by at least 20%
  in at least two of three paired repetitions.
- **H3-No-harm**: Treatment loses no more than one delivery-ratio percentage
  point in any pair and does not increase delivery p99 by more than 10% in two
  of three pairs.
- **H4-Moved-work**: Any treatment queue growth, material staleness, fallback,
  or extra traffic is visible. `stale_sent/submitted >1%`,
  `stale_dropped >0`, an undrained final queue, or more than 10% additional
  Sync Interests per committed publication in at least two pairs prevents an
  unconditional benefit claim.

### Fixed network and workload

```text
topology: peer-a <-> switch <-> peer-b
link: 10 ms one-way, 100 Mbps, 0% configured loss
processes: one PubSub process per peer
directions: peer-a publishes; peer-b is the fixed receiver
payload: 256 deterministic bytes
formal timing: 10 s warmup + 60 s measure + 10 s drain
release: absolute-deadline application pacer, no catch-up burst
protocol: V2, batching disabled
security: HMAC Sync Interest, SHA-256 publication Data
Face event threads: 1 per process
receive workers: 0
production workers: treatment-dependent only
```

The host has four vCPUs. Both NFDs share CPU 0; peer-a's pacer and Face share
CPU 1; fixed receiver peer-b uses CPU 2; peer-a's sole treatment worker uses
CPU 3. These declared shares are identical across treatments. The worker is one
thread on one CPU, not a four- or eight-core requirement.

### Non-formal fixed-rate validation

Run one `face-serial` diagnostic and one `worker-serial` diagnostic at the
pre-registered 60 publications/s producer rate with 5 seconds warmup, 15
seconds measurement, and 5 seconds drain. Neither cell is retried. The fixed
rate is admissible when both modes:

1. attempt within +/-2% of target;
2. deliver at least 95% of the remote peer's committed measured publications;
3. have complete accounting and resource records;
4. have zero production fallback and unexplained remainder; and
5. preserve `max_active_sync_signers == 1`.

The diagnostic does not select a rate or require a favorable difference. If
either mode is inadmissible, stop with `FIXED_RATE_INADMISSIBLE`. Otherwise
freeze 60 pps. Diagnostic data validates the harness only and is never combined
with formal estimates.

### Formal matrix

After the rate and all hashes are sealed, execute exactly:

| Ordinal | Pair | Runtime treatment | Rate |
|---:|---:|---|---:|
| 01 | 1 | `face-serial` | frozen \(R^\*\) |
| 02 | 1 | `worker-serial` | frozen \(R^\*\) |
| 03 | 2 | `worker-serial` | frozen \(R^\*\) |
| 04 | 2 | `face-serial` | frozen \(R^\*\) |
| 05 | 3 | `face-serial` | frozen \(R^\*\) |
| 06 | 3 | `worker-serial` | frozen \(R^\*\) |

The AB/BA/AB order balances monotonic host drift while retaining same-binary
execution. Each cell runs once. Timeout, crash, or inadmissibility is a result,
not authorization to replace the cell.

### Outcome decision table

Apply outcomes in this order:

| Condition | Classification |
|---|---|
| Fixed 60 pps diagnostic is inadmissible | `FIXED_RATE_INADMISSIBLE` |
| Any formal identity, completeness, fallback, concurrency, or conservation gate fails | `INADMISSIBLE` |
| H1 or H2 passes, but delivery harm >1 pp in any pair or p99 harm >10% in at least 2/3 pairs | `TRADE_OFF` |
| Neither H1 nor H2 passes, and the same delivery or p99 harm condition occurs | `WORKER_WORSE` |
| H1 and H2 pass; no-harm passes; delivery ratio or p99 improves >=5% relative in at least 2/3 pairs | `SUPPORTED` |
| H1 and H2 pass; no-harm passes; no user-visible endpoint improves >=5% relative in 2/3 pairs | `FACE_RELIEF_ONLY` |
| Some benefit passes but stale-sent ratio exceeds 1%, stale-dropped is nonzero, the final production queue is nonempty, or Sync Interests per committed publication rise >10% in at least 2/3 pairs | `TRADE_OFF` |
| None of the above | `NO_MEANINGFUL_EFFECT` |

`SUPPORTED` means useful at the tested rate under the tested HMAC/SHA-256
configuration. It does not mean a proved new maximum PPS.

## Build And Provenance Design

One builder:

1. verifies the active NDN-SVS checkout and refs before work;
2. creates an isolated worktree at exact `6bb34545`;
3. applies and records only the canonical Boost-1.71 compatibility patch and
   the common Spec 137 measurement patch;
4. rejects every unrecognized source hunk;
5. configures and builds once against Boost 1.71;
6. records base commit/tree, patched tree, patch bytes/SHA-256, compiler,
   include/library paths, configure/build commands, ELF build ID, binary/library
   SHA-256, and `ldd`;
7. rejects Boost 1.74 residue; and
8. seals the binary read-only before pilot execution.

The runtime binary prints the fully expanded treatment configuration on startup.
The runner compares this record with the sealed manifest before releasing the
barrier.

## Safety, Failure, And Shutdown

- Worker lifetime must be stopped and joined before dependent Face/Core state is
  destroyed. A sanitizer-backed lifecycle test covers enable, load, drain,
  disable, and destruction.
- Formal teardown explicitly disables production offload after all submitted
  jobs have terminal outcomes and before Face destruction.
- Queue full uses the current serial fallback, but the event is counted and the
  cell becomes inadmissible. The experiment does not alter production semantics
  to make the result look cleaner.
- Stale behavior is split into `stale_sent`, which annotates a completed
  production, and terminal `stale_dropped`. Neither is hidden by decreasing the
  denominator.
- A process watchdog writes a failure receipt from the supervisor when a peer
  cannot write its own terminal record.
- One campaign lock and one receipt ledger provide single-writer ownership.

## Analysis And Evidence

Canonical result layout:

```text
results/spec137-svs-serial-production-offload/<campaign-id>/
├── campaign-manifest.json
├── campaign.lock
├── build/
│   ├── source-manifest.json
│   ├── patches/
│   ├── linkage.txt
│   └── hashes.sha256
├── preflight/
├── pilot/
│   ├── pilot-cells.csv
│   └── rate-selection.json
├── formal/
│   ├── cell-01/ ... cell-06/
│   ├── campaign-cells.csv
│   └── campaign-runs.csv
├── receipts/
├── analysis/
│   ├── run-metrics.csv
│   ├── paired-contrasts.csv
│   ├── stage-breakdown.csv
│   ├── traffic-breakdown.csv
│   └── conclusion.json
└── closure/
    ├── protected-evidence-hashes.json
    └── verification.txt
```

The final tracked interpretation is written to
`specs/137-svs-serial-production-offload/evidence/offload-proof-report.md`.
Raw result files remain local evidence referenced by campaign ID and hashes.

## Project Structure

### Documentation

```text
specs/137-svs-serial-production-offload/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── traceability.md
├── contracts/
│   └── experiment-contract.md
├── checklists/
│   └── requirements.md
├── evidence/                         # created after execution
└── tasks.md
```

### Planned implementation

```text
Experiments/
├── ndn-svs-pubsub-benchmark/
│   ├── svs-serial-production-offload.cpp
│   └── spec137-measurement.patch
├── build_svs_serial_production_offload.py
├── NDN_SVS_Serial_Production_Offload_Minindn.py
└── analyze_svs_serial_production_offload.py

tests/python/
└── test_spec137_svs_serial_production_offload.py

build/spec137-four-core/              # local, generated, isolated
results/spec137-svs-serial-production-offload/  # local, generated
```

**Structure Decision**: Keep the active NDN-SVS checkout untouched. Store the
auditable common source patch and all orchestration in the framework repository;
apply the patch only inside a hash-bound isolated worktree.

## Complexity Tracking

No constitution violation or extra runtime abstraction is required. A new
standalone benchmark is used because reusing and conditionally extending the
Spec 132/133 binaries would blur frozen evidence ownership.

## Implementation Phases

1. **Audit and freeze**: Verify source reality, claim boundary, exact subject,
   protected evidence, and this design.
2. **Mechanism instrumentation**: Add runtime treatment selection, thread/CPU/
   signer/conservation probes, and lifecycle tests in one common patch.
3. **Harness and admission**: Implement immutable build, two-process MiniNDN
   runner, structured evidence, analyzer, and fail-closed validators.
4. **Preflight and pilot**: Prove harness validity and instrumentation budget,
   run the declared pilot once, then seal \(R^\*\) and six formal cells.
5. **Formal execution**: Execute all six once with terminal receipts.
6. **Analysis and freeze**: Produce paired contrasts, classify the result,
   verify protected evidence, audit post-implementation, and close Spec 137.

## Post-Design Constitution Re-check

PASS. The plan preserves the single-variable design, keeps security and
publication-signing scope unchanged, uses MiniNDN, provides negative-result and
rollback semantics, and expresses six cohesive implementation tasks.
