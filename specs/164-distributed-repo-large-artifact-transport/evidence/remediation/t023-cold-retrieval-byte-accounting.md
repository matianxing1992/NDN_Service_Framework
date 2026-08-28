# T023 Cold Retrieval, Queue Lifecycle, and Byte Accounting

## Verdict

PASS for the T023 implementation and functional evidence boundary. This task
does not make a formal performance claim; T024 owns the frozen repeated
campaign.

## Corrected Control and Lifecycle Semantics

- ACK metadata remains an advisory capacity/queue snapshot and creates no
  reservation, pin, or lock.
- An exact Selection assignment enters the Provider lifecycle as `QUEUED`.
- `ArtifactReplicaSession.begin_assigned_task()` starts the selected task
  without calling `reserve_artifact_capacity()`.
- The current path is
  `ABSENT -> QUEUED -> RECEIVING -> VERIFIED -> COMMITTED -> ACTIVE`.
- `RESERVED` and `reserve()` remain readable/executable only for legacy
  migration and recovery tests.

The lifecycle contract test proves `reserved_bytes` is unchanged before and
after queue admission and after activation.

## Canonical Evidence Changes

Schema generation 2 replaces the active `reservation` metric phase with:

```text
discovery, ackCollection, planning, queueWait, sessionStart,
transfer, verification, persistence, replication, commit, activation
```

Every new run separately records:

- `dataWireBytes` and `interestWireBytes`;
- `payloadStoreBytesRead` and `payloadStoreBytesWritten`;
- `metadataStoreBytesRead` and `metadataStoreBytesWritten`;
- aggregate compatibility totals with fail-closed equality checks;
- a separate repo-to-consumer cold retrieval, including elapsed time,
  goodput, Data/Interest wire bytes, digest, and atomic destination visibility.

Immutable schema-v1 evidence is normalized only while reading: old
`reservation` becomes `sessionStart`, legacy `wireBytes` is classified as Data
wire with unknown Interest wire set to zero, and absent cold retrieval remains
explicitly not measured. Old result files are never rewritten.

## Verification

### Automated tests

- Python lifecycle: 6/6 PASS.
- Python operation metrics: 6/6 PASS.
- Python performance harness/schema: 13/13 PASS.
- Python performance analysis/legacy normalization: 4/4 PASS.
- Native `DistributedRepoOperationMetrics`: 2/2 PASS.
- Incremental `./waf build --targets=unit-tests -j2`: PASS.
- Both Python C++ extensions rebuilt in place with `-O0 -g0 -j1`.
- Representative generation-2 campaign run validates against
  `campaign-run.schema.json`.

### MiniNDN queued lifecycle

Evidence:
`results/spec164-t023-queued-functional-20260730T0812Z`

- verdict: PASS;
- lifecycle:
  `QUEUED, RECEIVING, VERIFIED, COMMITTED, ACTIVE`;
- publication/retrieval: 2 segments each, zero timeout and retransmission;
- cold destination visible: true;
- performance claim: false.

Summary SHA-256:
`fad1eb911a4b701fbd40b5c7ba98b022ebc2fd67ed96a16146f7c3df82a323ac`.

### MiniNDN 1 MiB signed-manifest cold path

Evidence:
`results/spec164-t023-cold-smoke-1mib-20260730T0807Z`

- verdict: PASS;
- publication logical goodput: 38.2201076029 Mbit/s;
- cold retrieval logical goodput: 30.1025790190 Mbit/s;
- publication Data wire: 1,073,408 bytes;
- publication Interest wire: 13,045 bytes;
- payload-store read/write: 1,048,576 / 1,048,576 bytes;
- metadata-store read/write: 12,288 / 12,288 bytes;
- read amplification: 1.01171875x;
- write amplification: 1.01171875x;
- cold destination visible with exact content digest: true;
- performance claim: false.

All four byte equalities passed:

```text
wireBytes = dataWireBytes + interestWireBytes
storageBytesRead = payloadStoreBytesRead + metadataStoreBytesRead
storageBytesWritten = payloadStoreBytesWritten + metadataStoreBytesWritten
coldRetrievalWireBytes =
  coldRetrievalDataWireBytes + coldRetrievalInterestWireBytes
```

Top summary SHA-256:
`01a7e28aca0319f6feea54582252d8f6e51f746dd6674d1cd550307c0cbf3aac`.

Subject summary SHA-256:
`430b2a1a3b0ca41e31cf7c8a293120c7f8ef80dcabfee3140fdbb00f2829dfec`.

## Retained Negative Diagnostics

The first three cold-path attempts are retained under:

- `results/spec164-t023-cold-smoke-20260730T0755Z`;
- `results/spec164-t023-cold-smoke-20260730T0758Z`;
- `results/spec164-t023-cold-smoke-20260730T0801Z`.

They all completed transfer but failed before emitting the cold result because
the Python `AdaptiveSegmentFetchResult` wrapper had not exposed the newly added
native fields. The fix added `data_wire_bytes` and `interest_wire_bytes` to the
wrapper. Generated duplicate payloads and ephemeral ready/stop files were
removed; diagnostic logs and JSON remain.

## Boundary

The 1 MiB run is a functional smoke and cannot establish SC-002, SC-003,
SC-004, or SC-007 statistically. T024 must freeze a new schema-generation-2
campaign, retain one warmup plus at least five measured repetitions per
admitted cell, and derive the formal verdicts without modifying the original
campaign.
