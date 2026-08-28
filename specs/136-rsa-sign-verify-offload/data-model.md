# Data Model: Spec 136

## SecuritySubject

| Field | Meaning |
|---|---|
| `subjectId` | `inline-rsa-sign-verify` or `ordered-rsa-signing-offload` |
| `baseHead/baseTree` | Exact clean `6bb34545` identity |
| `commonPatchSha256` | Shared validator/profiler patch |
| `offloadPatchSha256` | Treatment-only patch; absent for control |
| `library/binary/driverSha256` | Frozen executable identities |
| `boost/linkage/compiler` | Build provenance |
| `validatorPolicySha256` | Exact fixed-anchor policy |
| `protectedSpec135Snapshot` | Hash inventory checked before and after |

## PublicationTicket

```text
Submitted
  -> RejectedBeforeReservation
  -> Accepted(seqNo)
       -> Preparing
       -> Prepared
       -> WaitingForOrder
       -> Committing
       -> Committed
       -> FailedAfterReservation
       -> CancelledOnShutdown
```

Required fields: cell, peer, producer, logical ID, optional sequence, admission
timestamp, queue/start/prepared/commit/terminal timestamps, terminal reason,
queue depth, and attempt number. Exactly one terminal state is permitted.

## PreparedCompletion

An immutable worker result containing producer, sequence, signed inner packet,
signed outer packet(s), mapping input, freshness, preparation timings, and
success/failure. It contains no `Face`, scheduler, SVS core, DataStore, or
callback reference.

## ValidationObservation

| Field | Values |
|---|---|
| Object | `mapping`, `outer`, `inner` |
| Transport path | `fetch`, `piggyback` |
| Outcome | `success`, `failure` |
| Signature type | Expected TLV type 1 |
| Policy/anchor | Hash and peer certificate identity |
| Timing | validate-call, callback, leaf verify CPU |
| Publication key | producer, bootstrap time, sequence/logical ID |

No observation is synthesized for an object absent from the path.

## FormalCell

Fields include ordinal, subject, rate per peer, topology, timing, protocol,
security settings, queue bound, CPU assignment, command hash, two peer
identities, one attempt, result directory, and terminal status.

## RateContrast

One inline/offload pair at a fixed rate containing absolute and relative
differences for security, ordering, admission, release lateness, delivery,
queue/service/reorder time, sign/verify CPU, resource use, and the SC-007
classification.

## Invariants

1. Queue rejection has no sequence.
2. Every reserved sequence has exactly one terminal outcome.
3. Committed sequences and callbacks are strictly monotonic per producer.
4. Subscriber delivery requires the path-specific validation chain.
5. A formal cell has exactly one immutable receipt.
6. Spec 135 paths never appear as writable or executable Spec 136 authorities.
