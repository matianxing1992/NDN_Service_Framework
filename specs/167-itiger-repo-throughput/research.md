# Research: TigerCluster DistributedRepo Throughput Validation

## Decision 1: External-validity scope

Use a two-node, CPU-only TigerCluster campaign with 64 MiB and 1 GiB,
single-replica/single-concurrency cells.  This isolates data-plane and trust
overhead before replication or concurrency scaling.

**Rationale**: Spec 164 already establishes correctness and scaling mechanics
in MiniNDN.  The unanswered question is whether the 2.65-times legacy
improvement and near-raw goodput survive the real cluster data path.

**Rejected**: infer throughput from Qwen job 181096.  Its model artifacts were
pre-staged and its total duration includes orchestration and inference.

## Decision 2: Measured path

All payload, repository store, database, and cold destination bytes live in
rank-local scratch.  Shared `/project` storage carries only immutable inputs,
small coordination records, and durable evidence.

**Rationale**: Allowing payload files in `/project` would measure NFS reads and
writes in addition to, or instead of, NDN transfer.

## Decision 3: Matched subjects

Retain the Spec 164 subjects: physical network, raw segmented NDN, legacy exact
packet, digest-only manifest, and signed manifest.  Packet geometry, route,
payload digest, nodes, and schedule remain matched.

**Rationale**: Absolute Mbps alone cannot distinguish repository overhead from
the available physical and NDN ceilings.

## Decision 4: Candidate reuse boundary

The accepted Spec 166 SIF may be reused only after an exact import/package
closure probe.  Current source may be mounted only through a checksum-bound,
immutable overlay whose manifest is retained.  A failed probe blocks formal
submission.

**Rationale**: Spec 166 demonstrated that sealed-image API and package-data
drift can survive source-only local tests.

## Decision 5: Statistics and negative evidence

Use one warmup and five measured repetitions, randomized within repetition,
paired ratios, and bootstrap intervals.  Preserve all measured failures and do
not replace a failed campaign identity.

**Rationale**: This matches Spec 164 and prevents survivor bias, optional
stopping, unmatched pairing, and post-hoc workload selection.

## Decision 6: Physical ceiling implementation

Use a checksum-bound Python TCP streaming sender/receiver over the same two
nodes and interface rather than relying on `iperf3` inside the accepted SIF.

**Rationale**: Preserved preflight job 181097 proved that the exact SIF does not
contain `iperf3`, while both application imports succeeded. Installing a mutable
package at job time would break candidate identity. The Python ceiling remains
an explicitly labelled userspace TCP ceiling, not an unqualified NIC line-rate
claim.

## ARS 11-Fallacy Guard

The analyzer/audit must check: survivor bias, optional stopping, pseudoreplication,
unmatched pairing, pooled heterogeneous cells, post-hoc thresholds, hidden
warmups, denominator loss, selective retry, ceiling substitution, and causal
claims from observational resource correlations.
