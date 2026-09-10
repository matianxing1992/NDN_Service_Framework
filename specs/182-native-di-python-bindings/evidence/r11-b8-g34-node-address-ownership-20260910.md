# R11-B8-G34 Node Address Ownership Boundary

**Date**: 2026-09-10
**Scope**: Spec110 topology node-address pre-start validation
**Status**: CLOSED_FOR_VALIDATION (bounded deployment harness repair)

## Finding

The process map validated that node addresses were IPv4 and routable-looking,
but it did not confirm that each address was assigned to its declared Slurm
node. A wrong interface or host therefore failed only after NFD startup and
route configuration. MiniNDN's loopback topology hides this mismatch.

## Repair

In non-test mode, the supervisor runs a target-node `python3` socket probe that
binds each declared IPv4 address to an ephemeral TCP port before any NFD is
started. A failed bind emits `SPEC110_NODE_ADDRESS_NOT_LOCAL` and preserves the
pre-start teardown boundary. The probe confirms local address ownership only;
it does not claim NDN face or route connectivity.

## Verification

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
bash -n tests/container/itiger-qwen-live/integration/test_network_scripts.sh
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
Ran 24 tests ... OK
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
NETWORK_SCRIPT_PASS
```

The integration fixture injects a target-node address probe failure and checks
exit 4, zero survivors, and no NFD log, while retaining normal, identity,
port, scratch-job and symlink negative cases. Real Slurm/NFD routing, SIF/GPU,
no-Python and T016/T017 qualification remain open.
