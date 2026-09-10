# R11-B8-G32 Stale NFD Socket Boundary

**Date**: 2026-09-10
**Scope**: Spec110 topology NFD startup/readiness
**Status**: CLOSED_FOR_VALIDATION (bounded deployment harness repair)

## Finding

The supervisor tested only whether the declared NFD Unix socket existed. If a
previous NFD crashed while reusing the same scratch directory, its stale socket
could make readiness pass even when the new NFD step had already exited. A
fresh MiniNDN namespace normally hides this reuse case.

## Repair

The supervisor now removes each socket on its target node immediately before
launching that node's NFD and stores the resulting `srun` PID by node rank.
Readiness requires both a live PID and a newly-created socket; a stale socket
cannot satisfy the barrier.

## Verification

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
bash -n tests/container/itiger-qwen-live/integration/test_network_scripts.sh
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
Ran 23 tests ... OK
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
NETWORK_SCRIPT_PASS
```

The integration source assertion requires socket removal before launch and a
rank-bound `kill -0` check before socket acceptance. This is a local readiness
boundary proof; real Slurm/NFD multi-machine routing, SIF/GPU, no-Python and
T016/T017 qualification remain open.
