# R11-B8-G31 Identity Symlink Boundary

**Date**: 2026-09-10  
**Scope**: Spec110 identity copy and process HOME isolation  
**Status**: CLOSED_FOR_VALIDATION (bounded deployment harness repair)

## Finding

The launcher copied identity trees with `cp -a`, which preserves symbolic
links. A `.ndn/pib.db` or TPM link could therefore leave the scratch HOME
pointing back to shared project storage, despite the exported runtime paths
looking process-local.

## Repair

The direct launcher now rejects any symbolic link below the source `.ndn`
directory before copying. The supervisor performs the same target-node check
for every non-NFD identity before starting NFD, so the topology cannot reach a
partial readiness state with an identity that bypasses the scratch copy.

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
Ran 23 tests ... OK
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
bash -n tests/container/itiger-qwen-live/integration/test_network_scripts.sh
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
SPEC110_IDENTITY_SYMLINK_FORBIDDEN:0:/project/tma1/ndnsf-di/identities/c1/controller
NETWORK_SCRIPT_PASS
```

The unit case executes a provider launcher with a symlinked PIB and observes
exit 8 before the provider binary runs. The integration case observes the
pre-start exit 4, `survivors: 0`, and no NFD log. Real shared-storage,
Slurm/SIF/GPU, cross-node NDN, no-Python and T016/T017 qualification remain
open.
