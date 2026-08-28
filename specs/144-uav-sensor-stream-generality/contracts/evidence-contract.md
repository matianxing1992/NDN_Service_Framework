# Evidence Contract

## Reference implementation provenance

Spec 144's formal APP-side integration reference is the corrected Spec 145 UAV
Video path:

```text
post audit:
  specs/145-uav-video-runtime-corrections/evidence/post-implementation-audit.md
formal result:
  results/spec145-uav-video-runtime-20260724T064253Z
campaign-summary.json SHA-256:
  8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566
run-summary.json SHA-256:
  6d842b6b538ef112a2d0a961fda129d950a9e546769a07974049841bd8055243
```

Pre-implementation and final-closure evidence MUST verify these identities and
the PASS verdict. The comparison records whether each new APP follows the
reference ownership and lifecycle pattern. It MUST NOT treat Spec 145
measurements as a Spec 144 repetition or import video/codec workload semantics.

## Formal matrix

Each workload executes exactly:

```text
zero-loss: 1
loss-only: 5
reorder-only: 5
combined: 5
```

Total: 16 cells/workload, 32 cells overall.

No automatic retry is permitted. A formal process crash, timeout, readiness
failure, count mismatch, analyzer failure, or application failure remains one
failed/incomplete formal cell.

## Required per-cell identity

- stable cell and invocation IDs;
- workload, profile, repetition, output path;
- exact command;
- source/tree and changed-file hashes;
- native library, application, binding, runner, analyzer, and config hashes;
- compiler, ndn-cxx, NFD, MiniNDN, Boost, Python, and kernel/iproute2 identity;
- topology and qdisc before/after evidence for both directions;
- launcher PID/process tree and cleanup owner;
- start/end/timeout state and exit codes;
- readiness, warm-up, measured-window, and expected-count gates.

## Required application metrics

Telemetry:

- attempted, produced, admitted, rejected, duplicate, invalid, out-of-order,
  stale, and terminally skipped samples;
- delivery ratio and monotonic-state violations;
- AoI mean/p50/p95/p99/max;
- longest accepted-sample gap.

Acoustic/audio:

- attempted, produced, complete-direct, complete-recovered, incomplete,
  rejected, duplicate, invalid, and terminally skipped blocks;
- source-item and complete-block delivery ratios;
- capture-to-complete-delivery mean/p50/p95/p99/max;
- longest complete-block gap;
- recovered source items and blocks by missing-source count.

## Required network and prefetch metrics

- Mapping Interests;
- validated Mapping Data responses;
- Mapping responses that add new resolver information;
- Mapping novelty ratio;
- Payload Interests split by initial/retry and source/repair;
- provider-confirmed future Interests and hits split by initial/retry;
- provider-confirmed future-hit ratio;
- retry attempts, successes, suppressions, and reason map;
- timeout, Nack, late-arrival, deadline-skip, retry-exhaustion;
- recovery attempt, success, exhaustion, and provenance.

## Interest utility equations

The unsampled Core terminal-attempt ledger classifies every Payload Interest
attempt into exactly one terminal category:

```text
applicationUseful =
  admittedNewSourceInterest +
  repairInterestConsumedBySuccessfulRecovery

protectionOnly =
  validRepairInterestNotConsumedByRecovery

nonproductive =
  allPayloadInterests - applicationUseful - protectionOnly

nonproductiveInterestRatio =
  nonproductive / allPayloadInterests

protectionOnlyRatio =
  protectionOnly / allPayloadInterests

combinedNonApplicationInterestRatio =
  (protectionOnly + nonproductive) / allPayloadInterests
```

An unresolved attempt, double classification, or negative remainder fails
metric conservation and therefore fails the cell. Sampled TimelineTrace output
is diagnostic only and is not used as the conservation authority.

Mapping utility is:

```text
mappingNovelty =
  mappingNewDataResponses / mappingDataResponses
```

Provider-confirmed future utility is:

```text
futureHitRatio =
  providerFutureHits / providerFutureInterests
```

Zero denominators are reported explicitly and fail any gate that requires the
ratio.

## Latency computation

Keep the complete observation vector for each measured sample/block. Compute
arithmetic mean and nearest-rank p50/p95/p99, plus maximum. Never add percentiles
from independent stages. Report observation count, clock domain, exact origin,
exact terminal event, excluded count, and exclusion reasons.

## Treatment verdicts

- Zero-loss is accepted only if its single cell passes every workload gate.
- Each five-repetition treatment is accepted only if at least four cells pass.
- Report accepted count and the two-sided Clopper-Pearson 95% interval.
- The workload verdict passes only if all four treatments pass.
- The shared verdict passes only if both workload verdicts pass.

## Evidence immutability

Before implementation and after closure:

1. hash all files under Specs 127 and 128 in sorted path order;
2. hash that manifest into one directory-root digest;
3. compare against the definition-time roots in `spec.md`;
4. scan process/command records for historical runner invocation.

After the first formal cell starts, record `formalFrozen=true` and reject any
source, binary, configuration, command, threshold, workload, or analyzer hash
change.

Diagnostic outputs use `results/spec144-diagnostic-*`; formal outputs use one
fresh `results/spec144-uav-sensor-stream-<id>` root. Diagnostics never enter
formal denominators.
