# Data Model: Bidirectional Capability Comparison

## CapabilitySubject

Exact base commit, capability label, publication API, parallel-worker settings,
temporary Boost-only head/tree, patch hash, library hash, driver hash, binary
hash, compile command, and linkage proof.

## BidirectionalCell

| Field | Contract |
|---|---|
| ordinal | 1..10 |
| subject | one frozen capability subject |
| ratePpsPerPeer | 200, 400, 600, 800, or 1000 |
| aggregateTargetPps | twice `ratePpsPerPeer` |
| warmup/measure/drain | 10/60/10 seconds |
| peers | exactly `peer-a` and `peer-b` |
| attempt | exactly 1 |

## PeerEvent

Cell ID, peer ID, event kind, local logical ID, SVS sequence, phase, planned
timestamp, observed monotonic timestamp, remote peer ID where applicable, and
structured details. Event kinds include ready, deadline, api-enter, api-return,
api-error, state-update, delivery, duplicate, invalid, and worker-stats.

## DirectionSummary

Joins one sender's successful API returns to the opposite receiver's deliveries.
Contains entered, returned, delivered, missing, attempted rate, return/target,
delivery/return, deadline lateness, API duration, delivery delay, duplicates,
reorder transitions, and validity counters.

## Cell State

`PLANNED -> RUNNING -> COMPLETE | SUBJECT_FAILURE | INFRA_FAILURE`.
Every terminal state writes one immutable attempt receipt. A subject failure is
admissible negative evidence; an infrastructure failure blocks claims but does
not authorize silent replacement.

