# Contract: Performance and Evidence

## Comparison Subjects

1. Matched physical/virtual network ceiling.
2. Matched raw segmented NDN transfer.
3. Legacy exact-packet repository transfer.
4. Digest-only scalable repository transfer.
5. Signed-manifest scalable repository transfer.

Subjects use identical artifact bytes, topology, packet geometry, concurrency,
logging, congestion behavior, and measurement boundaries wherever applicable.

## Formal Matrix

| Factor | Levels |
|---|---|
| Artifact size | 1 MiB, 64 MiB, 1 GiB, 16 GiB |
| Replicas | 1, 3 |
| Concurrent artifacts | 1, 4, 16 |
| Repository verification | legacy, digest-only, signed-manifest |

Raw NDN runs are paired with repository cells. Fault/recovery and warm-cache
campaigns are separate from the cold throughput matrix.

## Repetitions

Each admissible cell has one unmeasured warmup and at least five measured
repetitions in randomized matched blocks. Five closes only the engineering
gate. Inferential paper claims require a pilot-derived sample size frozen
before the formal inferential campaign.

## Metrics

- logical and wire goodput;
- end-to-end and phase latency;
- p50, p95, median, IQR, min/max, confidence intervals;
- CPU time/utilization and peak RSS;
- logical, Data-wire, Interest-wire, retransmitted, payload-store
  read/write, and metadata-store read/write bytes;
- Interest/Data/timeout/retransmission counts and window behavior;
- asymmetric and digest verification counts/time;
- metadata operation and record counts;
- requested/selected/committed replica counts;
- completion, failure, rejection, timeout, and recovery verdict.

## Frozen Gates

- Digest-only goodput >=85% of matched raw NDN goodput for >=64 MiB.
- Signed-manifest goodput no more than 10% below digest-only.
- No per-chunk/per-packet NDNSF control operation.
- No per-4-KiB scalable-format metadata row.
- Peak memory independent of artifact size at fixed window.
- Cold single-replica read amplification <=1.20x logical bytes.
- Cold single-replica write amplification <=1.50x logical bytes.
- Zero integrity, provenance, atomic-visibility, or recovery failures.

## Measurement Boundary

Repository publication time starts before discovery/ACK collection and ends only
after required replica receipts and catalog activation. Phase timings are also
reported so control, transfer, verification, persistence, and replication can
be analyzed separately.

Raw NDN timing covers the equivalent producer-ready through consumer-complete
payload interval and excludes repository-only phases by definition.

## Canonical Operation-Metrics Semantics

Every publication or retrieval evidence record has one caller-visible
`operationId`, stable across retries and resume. It records non-negative
durations for only these lifecycle phases: `discovery`, `ackCollection`,
`planning`, `queueWait`, `sessionStart`, `transfer`, `verification`,
`persistence`, `replication`, `commit`, and `activation`. Phases may overlap;
their sum is therefore not defined as end-to-end latency. The end-to-end
boundary is `startedAtMs` through `completedAtMs`.

Counters have the following non-interchangeable meanings:

- `logicalPayloadBytes`: reconstructed application content, counted once;
- `dataWireBytes` and `interestWireBytes`: complete transmitted NDN Data and
  Interest wire encodings respectively, including retransmissions;
- `wireBytes`: exactly `dataWireBytes + interestWireBytes`;
- `retransmittedBytes`: the repeated subset of `wireBytes`;
- `payloadStoreBytesRead` and `payloadStoreBytesWritten`: content bytes crossing
  the payload-store boundary;
- `metadataStoreBytesRead` and `metadataStoreBytesWritten`: encoded bytes
  crossing the metadata-store boundary, reported separately so SQLite/database
  work cannot be hidden inside payload amplification;
- `storageBytesRead` and `storageBytesWritten`: compatibility totals that MUST
  equal the corresponding payload-store plus metadata-store counters;
- `asymmetricVerifications` and `digestVerifications`: completed verification
  operations, with their CPU/wall measurement accumulated separately;
- `controlOperations`: NDNSF Request/Selection invocations for discovery,
  task assignment, commit, or lifecycle control, never Data-plane Interests
  or packets; ACK_CLOSED is a lifecycle boundary, not a wire attempt;
- `metadataOperations`: metadata-store transactions or explicit operations;
- `metadataRecordCount`: durable records retained after the operation;
- `requestedReplicaCount`: requested durability;
- `selectedReplicaCount`: distinct authorized repositories assigned;
- `committedReplicaCount`: distinct valid retained `ReplicaReceipt` records;
- `rejectedReplicaReceiptCount`: received receipts rejected as invalid,
  duplicate, conflicting, or outside the operation identity.

The invariant is
`committedReplicaCount <= selectedReplicaCount <= requestedReplicaCount`.
Attempted writes, ACKs, or unvalidated receipts never increase committed
replicas.

Every repository performance cell also performs a cold retrieval over the
repo-to-consumer MiniNDN link after publication. Its destination MUST not exist
before the request and MUST become atomically visible only after the full
content digest matches. Cold retrieval reports its own elapsed time, logical
goodput, Data/Interest wire bytes, packet counts, and destination-visibility
verdict; publication and retrieval wire counters are not silently combined.

## Admissibility

A run is inadmissible only under a predeclared mechanical rule such as changed
subject bytes, environment mismatch, missing required metrics, unrelated host
failure, or resource limit that prevents the cell from starting. It remains in
the campaign table with evidence and reason. Slow, failed, timed-out, or
negative runs are not outliers merely because of their result.

## Evidence Files

The future harness must emit:

- `campaign-manifest.json`;
- one row per run in `campaign-runs.csv` or JSONL;
- phase and resource samples;
- integrity/recovery verdicts;
- exact commands and environment;
- source/build hashes and dirty-tree manifest;
- derivation output and canonical Markdown report.

Raw timeline traces use the shared stable request-ID sampler. Results are local
evidence until the canonical finding and reproduction command are documented.
