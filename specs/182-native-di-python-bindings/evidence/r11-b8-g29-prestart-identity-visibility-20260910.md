# R11-B8-G29 Pre-Start Identity Visibility

**Date**: 2026-09-10  
**Scope**: Spec110 topology supervisor identity inputs  
**Status**: CLOSED_FOR_VALIDATION (bounded deployment harness repair)

## Finding

The launcher already checked identity files immediately before each business
process `exec`, but the supervisor could start all NFDs before discovering that
a Controller, Provider or User identity source was absent on its target node.
That leaves a partially started topology and differs from a MiniNDN setup where
the identity directory is commonly shared by every local process.

## Repair

Before any NFD step is started, `run-allocation-topology.sh` now enumerates all
non-NFD `identityRef` values and checks, on their target node via `srun`, that
`.ndn/pib.db` and `.ndn/ndnsec-key-file` are readable. A missing or unmounted
identity source fails with `SPEC110_IDENTITY_NOT_VISIBLE` and preserves the
pre-start `teardown.json`; the test-only fake identity bypass remains confined
to `NDNSF_SPEC110_TEST_MODE=1`.

## Verification

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
bash -n tests/container/itiger-qwen-live/integration/test_network_scripts.sh
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
Ran 21 tests ... OK
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
SPEC110_IDENTITY_NOT_VISIBLE:0:/project/tma1/ndnsf-di/identities/c1/controller
NETWORK_SCRIPT_PASS
```

The integration negative case verifies exit code 4, `survivors: 0`, and no
NFD log in the identity-failure scratch directory. This is a preflight and
failure-boundary proof; real shared-storage, Slurm, SIF/GPU, cross-node NDN,
no-Python and T016/T017 qualification remain open.
