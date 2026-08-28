# Data Model: Historical NDN-SVS Threading-Contract Recovery

## SourceContractFinding

`sourceClass`, `commit`, `path`, `lineOrSymbol`, `claim`, `evidenceLevel`,
`supportsCrossThread`, `limitation`.

## QualificationSubject

Exact base/tree, canonical Boost patch hash, driver hash, library/binary hashes,
compiler/flags, linkage, and explicit absence of repair/profiling patches.

## PeerExecution

Peer/node/process identity, Face transport, io thread ID/CPU, initialization
thread, timer implementation, forbidden-thread audit, start/end timestamps.

## ReleaseCounters

Scheduled slots, attempted publications, missed release slots, lateness
distribution, publication errors, and boundary-censored slots by phase.

## DeliveryCounters

Local deliveries ignored, valid remote deliveries, invalid remote deliveries,
delivered rate, delivery/attempted ratio, sender/direction identity.

## QualificationReceipt

Subject and command hashes, topology/workload/timing, two PeerExecution records,
release/delivery counters, exits, flush completeness, corruption findings,
error classification, and verdict.

Allowed verdicts: `QUALIFIED`, `NOT_QUALIFIED`, `INFRA_FAILURE`.
