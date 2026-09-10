# R11-B8-G33 Job-Scoped Scratch and Identity Root Boundary

**Date**: 2026-09-10
**Scope**: Spec110 topology scratch and identity pre-start checks
**Status**: CLOSED_FOR_VALIDATION (bounded deployment harness repair)

## Findings

The supervisor accepted any `/tmp/ndnsf-di-*` scratch basename. A real Slurm
allocation could therefore point at another job's scratch; G32's stale-socket
removal would then mutate unrelated state. The G31 checks also scanned the
`.ndn` tree but did not explicitly reject a terminal symlink at `identityRef`.
Fresh MiniNDN fixtures do not exercise either boundary.

## Repair

In non-test mode the supervisor now requires the scratch basename to be
`ndnsf-di-<SLURM_JOB_ID>` or `ndnsf-di-<SLURM_JOB_ID>-...`. It rejects a
terminal identity-root symlink before NFD startup. The generated launcher
repeats the identity-root check immediately before copying the identity.

## Verification

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
bash -n tests/container/itiger-qwen-live/integration/test_network_scripts.sh
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
Ran 24 tests ... OK
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
NETWORK_SCRIPT_PASS
```

The integration fixture covers a mismatched job scratch (exit 3, zero
survivors), identity visibility, identity-root/tree symlink rejection, stale
socket ordering, and normal teardown. Real Slurm/SIF/shared-storage routing,
GPU, no-Python and T016/T017 qualification remain open.
