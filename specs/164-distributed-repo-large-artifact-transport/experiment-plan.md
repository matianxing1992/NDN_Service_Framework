# Experiment Plan: DistributedRepo Large-Artifact Transport

## Material Passport

- **Origin Skill**: Academic Research Suite experiment-agent
- **Origin Mode**: plan
- **Origin Date**: 2026-07-29
- **Verification Status**: UNVERIFIED — design frozen before implementation
- **Version Label**: spec164_transport_plan_v1
- **Controlling Specification**: [spec.md](spec.md)

## Research Questions

- **RQ1**: How much application goodput does the scalable repository path
  preserve relative to a matched raw segmented NDN transfer?
- **RQ2**: What incremental cost does publisher-authenticated hierarchical
  manifest verification add over digest-only transfer?
- **RQ3**: Does removing per-Data-packet control and durable packet-row work
  make CPU, persistence work, memory, and metadata cardinality scale with
  artifacts and chunks rather than packet count?
- **RQ4**: Does resumable transfer reduce retransmitted bytes and completion
  time after interruption without weakening atomic visibility or integrity?
- **RQ5**: How do replica count and concurrent artifact count affect goodput,
  tail latency, resource use, and failure rate?

## Hypotheses and Acceptance Contracts

- **H1**: For artifacts of 64 MiB and above, digest-only repository goodput is
  at least 85% of matched raw NDN goodput.
- **H2**: Signed-manifest goodput is no more than 10% below digest-only goodput
  on the same matched cell.
- **H3**: Scalable-format control operations do not grow with artifact bytes or
  Data-packet count.
- **H4**: Peak transfer memory is bounded by configured in-flight work rather
  than artifact size.
- **H5**: After a controlled interruption, resume retransmits no already
  verified chunk except bounded protocol recovery traffic.
- **H6**: All injected corruption, substitution, truncation, extension, and
  mixed-version cases are rejected before artifact activation.

These are pre-implementation engineering acceptance contracts. They are not
claims of statistical significance. Thresholds must not be changed after
formal results are observed. Any later paper-level inferential claim requires
a pilot-based sample-size decision recorded before the corresponding formal
campaign.

## Experimental Design

### Independent Variables

| Factor | Levels |
|---|---|
| Transfer mode | raw segmented NDN; legacy per-packet asymmetric verification; digest-only repository; signed-manifest repository |
| Artifact size | 1 MiB; 64 MiB; 1 GiB; 16 GiB |
| Requested replicas | 1; 3 |
| Concurrent artifacts | 1; 4; 16 |
| Cache state | cold; explicitly identified warm-cache diagnostic |
| Fault condition | none; publisher interruption; repository interruption; consumer interruption; corruption; replica loss |

The complete throughput matrix uses the first four factors. Cache and fault
conditions run as separate controlled campaigns so they do not contaminate the
cold-path throughput comparison.

### Dependent Variables

- Successful logical payload bytes divided by measured completion time
  (application goodput).
- End-to-end completion latency and phase latency for discovery, reservation,
  transfer, verification, persistence, replica commit, and activation.
- p50, p95, minimum, maximum, median, interquartile range, and confidence
  interval for goodput and completion latency.
- Process CPU time, wall-clock CPU utilization, peak resident memory, and
  cryptographic operation counts.
- Logical bytes, wire bytes, retransmitted bytes, payload-store bytes read and
  written, and metadata operation counts.
- Interest count, Data count, timeout count, retransmission count, congestion
  window behavior, and achieved replica receipts.
- Completion, rejection, timeout, crash-recovery, and integrity-failure counts.
- Metadata record cardinality and manifest/root/page encoded sizes.

### Controlled Variables

- Exact artifact bytes and full-artifact digest.
- Topology, link capacity, delay, loss, queue configuration, routes, and NFD
  configuration.
- Data-packet payload limit, naming layout, freshness policy, and Interest
  lifetime.
- Host assignment, CPU allocation, memory limit, storage device, free-space
  reserve, and filesystem mount options.
- Build identifiers, dependency versions, cryptographic provider, trust
  anchors, and policy epoch.
- Logging level, timeline sampling, metric collection, warmup policy, and
  measured window.
- Publisher, repository, and consumer process lifecycle.

### Potential Confounds

- Page cache, NDN Content Store state, previously loaded trust material, and
  filesystem allocation.
- CPU frequency scaling, thermal throttling, co-scheduled workloads, VM steal
  time, and NUMA placement.
- Different packet geometry or congestion-control behavior between raw and
  repository paths.
- Debug logging, tracing, or metric sampling that differs by transfer mode.
- Replica fan-out occurring serially in one mode and concurrently in another.
- Compression, sparse files, deduplication, or zero-filled test payloads that
  reduce physical I/O.
- Reusing a content identity from an earlier repetition.
- Starting the measurement before the data plane is ready or ending it before
  required replica receipts and activation complete.

Each run manifest must record or rule out these confounds. A run with an
uncontrolled difference is diagnostic, not admissible formal evidence.

## Environment and Gate Order

1. Unit and local integration tests validate manifest parsing, trust,
   corruption rejection, lifecycle transitions, persistence, and recovery.
2. MiniNDN smoke tests validate the complete control and data path with small
   deterministic payloads.
3. MiniNDN preflight establishes resource feasibility and freezes the final
   admissible matrix.
4. The MiniNDN formal campaign runs the frozen matched matrix.
5. TigerCluster or a large model is used only after the MiniNDN gate passes or
   when separately and explicitly requested. It is an external-validity
   extension, not the primary correctness gate.

The default functional payload is deterministic pseudo-random data that cannot
benefit from compression or sparse-file shortcuts. A minimal Qwen artifact may
be added as an application-level demonstration after the generic byte-artifact
gate, but it must not replace the deterministic matched workload.

## Baselines

### Physical or Virtual Network Ceiling

Measure transport capacity between the same hosts or namespaces without NDN
application processing. Record bidirectional capacity, loss, CPU, and the
exact tool and command.

### Raw NDN Ceiling

Use a standard segmented producer/consumer or the same low-level segmented
producer/fetcher used by the repository data plane, without repository control,
manifest traversal, replication, or persistence. Packet geometry, topology,
security mode, logging, and congestion behavior must match the repository run.

### Legacy Repository Baseline

Preserve the current exact-packet publication path with its current signing,
control, fetching, and persistence behavior. Do not tune or repair this subject
after observing the new path. Any necessary correctness fix creates a new
declared baseline version.

## Repetition, Ordering, and Sampling

- Every admissible cell receives one unmeasured warmup followed by at least
  five measured repetitions.
- The minimum five repetitions are sufficient only for the engineering
  acceptance gate and distribution reporting.
- Before a paper-level inferential campaign, use preflight variance and the
  smallest practically meaningful goodput ratio to determine a larger sample
  size. Record the calculation and freeze the resulting count before execution.
- Run order is randomized within blocks. A block contains the matched transfer
  modes for one artifact-size, replica, and concurrency combination on the same
  environment allocation.
- Raw NDN, digest-only, and signed-manifest subjects are paired by block. A
  failed subject does not authorize deletion of the other paired observations.
- Cold-cache formal repetitions use a documented reset procedure. Warm-cache
  runs are labeled separately and never mixed into cold distributions.
- Performance short tests retain the project-standard 60-second measured
  window when rate is measured over time. Finite-file completion tests measure
  through required commit and activation and report their actual duration.

## Analysis Plan

### Primary Analysis

- Compute the paired goodput ratio:
  `repository_goodput / matched_raw_NDN_goodput`.
- Compute the paired security overhead ratio:
  `signed_manifest_goodput / digest_only_goodput`.
- Report every repetition plus median, interquartile range, p50, p95, and a
  bootstrap confidence interval for each ratio.
- Evaluate H1 and H2 against the frozen ratio thresholds. A confidence interval
  crossing a threshold is reported as inconclusive, not rounded into a pass.

### Scaling Analysis

- Regress or plot control operations, metadata records, CPU time, memory, and
  storage bytes against logical bytes, packet count, chunk count, replicas, and
  concurrency.
- Check the required invariants directly: no per-packet NDNSF control call, no
  per-4-KiB durable metadata row, and memory independent of artifact size at a
  fixed window.
- Report read and write amplification with numerator and denominator boundaries
  stated explicitly.

### Recovery Analysis

- For each interruption point, compare verified bytes before failure, bytes
  transferred after resume, final digest, final state, and replica receipts.
- Report any retransmission of previously verified chunks and distinguish
  protocol recovery traffic from payload retransmission.
- Treat exposure of partial or unverified content as a hard security failure,
  regardless of performance.

### Statistical Guardrails

- Do not equate a p-value with practical importance; always report effect size
  and interval estimates.
- Do not claim independence for repetitions sharing a long-lived process,
  cache, host allocation, or fault state; preserve block identifiers.
- Correct or explicitly bound family-wise error if later confirmatory analysis
  tests multiple thresholds beyond H1–H6.
- Do not exclude outliers unless a predeclared mechanical criterion proves the
  run invalid; retain excluded rows with reason codes.
- Resource-inadmissible cells remain in the matrix with evidence and status
  rather than disappearing from the report.

## Evidence Contract

Every campaign must preserve:

- Immutable campaign identifier, feature revision, source revision, dirty-tree
  manifest, and build hashes.
- Exact commands, environment variables, topology, routes, limits, dependency
  versions, trust configuration, and cryptographic algorithms.
- Artifact generator, artifact digest, size, packet geometry, chunk geometry,
  manifest sizes, and replica policy.
- One machine-readable row per repetition and separate phase, resource,
  protocol, persistence, and failure records.
- Raw logs sufficient to audit claimed metrics, sampled according to the
  repository's shared timeline-trace policy.
- A derivation script or explicit formulas for all reported ratios and
  distributions.
- All failed, negative, timed-out, corrupt, and inadmissible results.

## Stop Conditions

- Stop the formal campaign if payload identity differs between matched modes,
  environment identity changes within a block, or required metric collection
  fails.
- Stop performance acceptance on any integrity, provenance, atomic-visibility,
  or mixed-version security failure.
- Stop before TigerCluster if MiniNDN correctness, recovery, or matched-
  throughput gates are incomplete.
- Resource exhaustion makes only the affected cell inadmissible; it does not
  authorize changing the frozen subject or discarding the observation.

## Expected Outputs

| Output | Format | Success Criterion |
|---|---|---|
| Campaign manifest | JSON | Identifies immutable subjects, environment, matrix, thresholds, and commands |
| Per-run measurements | CSV or JSONL | Contains every repetition, block, result, failure, and resource metric |
| Phase measurements | CSV or JSONL | Attributes discovery through activation without missing phases |
| Integrity and recovery report | JSON and Markdown | Covers every fault and adversarial case with final state |
| Statistical summary | JSON and Markdown | Reproducibly derives ratios, distributions, intervals, and verdicts |
| Canonical experiment report | Markdown | Separates verified evidence, inference, limitations, and external-validity claims |

