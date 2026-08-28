# Data Model

## FrozenSubject

- source commit/tree
- measurement and Boost patch hashes
- binary and shared-library hashes
- compiler and linkage records
- protected Spec 137 hash inventory

## HostQuiescenceRecord

- sample start/end monotonic timestamps
- selected CPU map
- per-core busy ratios
- load average and CPU pressure
- threshold, timeout, and pass/fail reason

## CalibrationCell

- registered rate and ordinal
- control mode
- 5/15/5 timing
- quiescence record hash
- terminal status and validity gates
- Face pressure endpoints

## QualificationCell

- selected rate
- worker mode
- identical validity gates
- one-worker/one-signer/fallback/drain evidence

## FormalReceipt

- ordinal, pair, mode, rate
- started/ended timestamps
- terminal and admission status
- raw artifact hashes
- host-contamination result
- retry count fixed at zero

## PairedContrast

- pair identifier
- Face and worker run identities
- Face production CPU relief
- heartbeat p99 change
- delivery-ratio and delivery-p99 change
- queue and traffic deltas

## NecessityDecision

- terminal taxonomy value
- exact decision predicates
- passed/failed gates
- included and excluded claims

## State Transitions

```text
DESIGNED
  -> PREFLIGHT_ADMITTED
  -> RATE_SELECTED
  -> WORKER_QUALIFIED
  -> SEALED
  -> SIX_RECEIPTS
  -> ANALYZED
  -> FROZEN
```

Any missing pressure point closes at `NO_TESTABLE_PRESSURE_POINT`. Any failed
qualification closes before sealing. Once sealed, every started ordinal is
terminal and cannot be replaced.
