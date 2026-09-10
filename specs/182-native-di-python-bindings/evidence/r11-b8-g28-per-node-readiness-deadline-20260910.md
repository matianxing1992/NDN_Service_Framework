# R11-B8-G28 Per-Node Readiness Deadline

**Date**: 2026-09-10  
**Scope**: `run-allocation-topology.sh` NFD readiness preflight  
**Status**: CLOSED_FOR_VALIDATION (bounded deployment harness repair)

## Finding

The supervisor previously created one `deadline=$((SECONDS+30))` before the
loop over all NFD processes. In a multi-node allocation, a slow first node
could consume most of that shared budget and leave later nodes with little or
no readiness time. MiniNDN commonly starts all local NFDs quickly, so this
coupling was not visible in the local topology check.

## Repair

The 30-second readiness budget is now created inside the per-node loop. Every
node therefore receives one independent bounded window while retaining the
same fail-closed timeout code and readiness evidence file.

## Verification

The following checks passed:

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
bash -n tests/container/itiger-qwen-live/integration/test_network_scripts.sh
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
SPEC110_WORKDIR_NOT_VISIBLE:0
SPEC110_WORKDIR_INVALID
NETWORK_SCRIPT_PASS
```

The integration script also asserts that the deadline assignment occurs
inside the NFD row loop and before its polling loop, preventing the shared
deadline from being reintroduced. The negative workdir lines are expected
failure-boundary probes; the final verdict is `NETWORK_SCRIPT_PASS`.

This is a harness/static boundary check. It does not qualify real Slurm,
multi-machine NFD routing, SIF/GPU visibility, no-Python execution, or the
Spec182 T016/T017 gates.
