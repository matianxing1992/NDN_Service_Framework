# Spec176 release-gate evidence

Date: 2026-08-28
Branch: `UAV-Experimental`
Candidate commit: `e9c096420909039c50b726ad9b7417e496dcbc04`
Candidate tree: `0dfe3927af1501172fe5a75cb85c9c7969d261b6`

## Ordered verification

The required progression was rerun on the candidate in order:

```text
G1 unit-tests:
  ./build/unit-tests --run_test='UavProtocolState/*:UavTwoLifecycle/*' --log_level=message
  Running 87 test cases; return code 0

G2 CPU/in-process integration-tests:
  ./build/integration-tests --run_test='UavCollaborationFlow/*' --log_level=message
  Running 6 test cases; return code 0

G3 real multi-process MiniNDN:
  candidate-consistent nominal and ten-case failure-matrix evidence is recorded
  in minindn-nominal-20260828.md and minindn-failure-matrix-20260828.md;
  the deterministic 48-segment validator-backed CPU evidence is recorded
  separately in multisegment-evidence-20260828.md

G4 PX4/jMAVSim SITL:
  results/spec176-rootless-sitl-r15-20260828/summary.json
  status=PASS, returnCode=0, missingStageMarkers=[]
```

The r15 preflight binds the SITL run to the candidate commit/tree and PX4
commit `b405d75553905d272c0c69aa3e2d29a8e3fc8d0c`. It records three UDP-backed
PX4 instances, the 60-second measured window, and the named-Data/no-endpoint
application contract. The log contains stream, patrol, successful incident,
failed incident, compensation, and `duplicate_execution=0` reconciliation
markers.

## Scope and promotion boundary

The core-diff audit found only generic support changes required for signed,
producer-namespaced application Data, configured segmented-fetch validation,
per-node initialization isolation, and bounded diagnostics. No UAV-specific
Core wire type, transport mode, NDNSF-DI payload/dependency, or TigerCluster
deployment path was added. Existing NDNSF-DI integration examples are
organizational references only.

The release audit is complete and the worktree is clean at the candidate
commit. Promotion or merge to `Experimental`/`main` was intentionally not
performed; it remains a separate explicit authorization decision. Hardware
flight and adverse segmented-data behavior are also outside this release-gate
result.
