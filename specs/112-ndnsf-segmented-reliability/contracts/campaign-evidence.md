# Contract: Spec 112 MiniNDN Evidence

## Candidate Identity

Hash NDNSF, ndn-svs, NAC-ABE, ndn-cxx, dirty diffs, built libraries/tests,
compiler/dependency versions, configuration, and diagnostic scripts. Any changed
input creates a new candidate.

## Pre-Fix And Final Boundary Matrix

For each candidate, run at 0% configured loss:

```text
Normal   x synchronous SVS publish
Normal   x asynchronous SVS publish
Targeted x synchronous SVS publish
Targeted x asynchronous SVS publish
```

Each cell uses a fresh Provider and the ordered response sizes:

```text
64, 4000, 5000, 6500, 8000, 16000 bytes
```

All test roles record
`NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1`; the summary must show that no
automatic large-response reference was selected. There is no production-reference,
direct-object, Wi-Fi, or 5% loss cell in Spec 112.

## Final Same-Epoch Health Cell

One final-candidate Provider serves:

1. 80 consecutive byte-exact 8-KB responses;
2. 10 consecutive 64-B responses;
3. 12 consecutive 4-KB responses.

The Provider process and node-id epoch cannot restart between steps.

## Other Required Evidence

- real Python normal/Targeted positive and invalid-token tests;
- degraded/absent-Provider Targeted timeout and late-response race tests;
- 100 initialized cycles containing Controller, Provider, and User subprocesses
  (300 role exits total);
- rebuilt Boost 1.71 ndn-svs test results and binary/source hashes.

## Ownership And Run-Once Rule

- Confirm there is no live MiniNDN launcher/cleanup owner before launch; a lock
  file alone is insufficient.
- Record launcher PID, process tree, Provider epoch, and unique result directory.
- A candidate/cell pair runs once. Crash, hang, interruption, or partial output
  remains immutable. A fix/configuration change creates a new candidate.
- Cleanup terminates only processes owned by the current cell.

## Required Summary Fields

- candidate/dependency/binary/script identities and dirty digests;
- topology, 0% loss, routes, NFD log level, effective diagnostic environment;
- invocation mode, SVS publication mode, sizes, counts, and timeout;
- per-request bytes, equality, elapsed time, response/timeout callback counts;
- final outer packet sizes, publication/validation failure, cleanup state;
- Provider PID/node-id epoch, liveness, exits, and restart observation;
- evidence level: measured, executed, implemented, estimated, or proposed.

## Stop Conditions

Stop the affected cell on Provider crash, packet above 8800 B, ownership
conflict, missing candidate identity, disk guard, or wall-clock overrun. Preserve
the result; do not rerun the same candidate/cell.
