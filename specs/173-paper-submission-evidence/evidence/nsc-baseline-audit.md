# NSC Baseline Semantics and Optional Shared-Queue Control

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-08-12
- Verification Status: ANALYZED; experiment not implemented or run
- Version Label: nsc_baseline_audit_v1

## Finding

The manuscript labels the current mobility baseline **NSC-SEQ-4** because it is
a sequential four-prefix harness, not a complete implementation of NSC's
shared-name cluster architecture.

The distinction is visible in the current source:

- `Experiments/NDN_NSC/consumer.cpp` parses a caller-provided list of Provider
  prefixes and rotates sequential attempts across that list.
- `Experiments/NDN_NSC/producer.cpp` derives the function and result namespaces
  from one Provider identity. It contains no shared task queue, distributed
  datastore, task claim, or lease-recovery mechanism.
- `Experiments/WifiRouterMobilityReliability.py::run_nsc` launches four such
  Provider-specific producers and passes their comma-separated prefixes to the
  consumer.

The NSC technical report describes a different scalability and recovery
boundary: Executors listen under a shared function name, accepted task details
enter a task queue, input and results use shared storage, and another Executor
can continue an incomplete task. That design does not require the Caller to
select sequentially among four Executor prefixes. It does require shared
cluster state that the current harness does not implement.

## Consequence for the Current Paper

The present NSC measurements remain usable only when labeled as the evaluated
sequential four-prefix harness. They must not be presented as evidence that the
NSC architecture requires endpoint discovery, lacks multi-Executor failover,
or cannot use a shared function name. The manuscript now makes this boundary
explicit in Related Work, Table I, Evaluation, and Discussion.

A same-name-only variation would not repair the baseline. NDN forwarding may
send a notification Interest toward one reachable Executor, but without the
shared queue and datastore it cannot reproduce NSC's accepted-task recovery.
Calling that variation `NSC-SHARED` would therefore be misleading.

## Optional Faithful Control

### Objective

Measure how NDNSF's request-scoped ACK/Selection mechanism compares with an
NSC-style shared cluster after both systems are given their required supporting
state. This is an optional new comparison, not required to retain the current
bounded paper claims.

### Required NSC Components

1. One shared function namespace for all four Executors.
2. A task record keyed by the Caller's unique request identifier.
3. Shared task states such as `QUEUED`, `CLAIMED`, `INPUT_READY`, `RESULT_READY`,
   and `COMMITTED`.
4. Atomic claim or lease ownership so that an incomplete task can be reclaimed.
5. Shared input/result storage reachable by the replacement Executor.
6. Exactly one committed logical result, with duplicate executions recorded.

These components must be implemented and correctness-tested before any
performance run. A central in-memory queue is acceptable only as an explicitly
declared idealized cluster control; it must not be described as the original
NSC implementation.

### Matched Pilot Design

| Item | Registered value |
|---|---|
| Systems | NDNSF FirstResponding; current NSC-SEQ-4; optional NSC-SHARED-QUEUE |
| Topology | Existing one-AP field, one User, four mobile Executors |
| Mobility | Byte-identical trace and request schedule per seed |
| Workload | 5 requests/s, 60-second measured window, identical service delay |
| Deadline | 5-second global deadline for every logical request |
| Initial pilot | One smoke seed, then three untouched mobility seeds |
| Expansion rule | Expand to ten or more seeds only after correctness and trace checks pass |
| Admission | Disabled for NDNSF; queue capacity recorded separately for NSC |
| Primary outcomes | Completion and seed-level p95 successful-response latency on the registered switching population |
| Secondary outcomes | Mean latency, task claims, lease expirations, retries, executions per logical request, duplicate committed results |

The Caller must receive only the shared NSC function name in the shared-queue
cell. Queue/store placement, availability, and access latency are experimental
factors and must be declared; silently placing them on an always-reachable
central node would advantage NSC.

### Correctness Gate

Before the pilot, deterministic tests must demonstrate:

- a reachable Executor completes one task normally;
- an Executor failure before claim leaves the task available;
- an Executor failure after claim permits reclaim after lease expiry;
- a replacement Executor retrieves the same input and publishes the result;
- repeated notifications remain idempotent;
- no logical request commits more than one result;
- smoke output contains a `SMOKE_OK` marker and is excluded from performance
  claims.

### Interpretation

- If NSC-SHARED-QUEUE matches NDNSF, the paper should retain NDNSF's protocol
  distinction but remove any comparative NSC mobility advantage.
- If NDNSF is faster, the claim must be limited to the measured cost of
  request-scoped ACK/Selection versus the declared queue/store implementation.
- If the control cannot be implemented faithfully, retain NSC-SEQ-4 as a
  labeled harness and use gRPC as the primary mobility baseline.

## Recommended Decision

Do **not** add a superficial shared-name run. The current paper is already
defensible after relabeling and limitation disclosure. Implement the complete
NSC-SHARED-QUEUE pilot only if a reviewer or advisor considers a full NSC
architectural comparison essential; otherwise it adds a new distributed queue
system whose implementation risk is disproportionate to the paper's central
NDNSF contribution.
