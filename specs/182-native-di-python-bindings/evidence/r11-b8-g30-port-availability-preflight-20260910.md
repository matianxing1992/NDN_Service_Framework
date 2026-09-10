# R11-B8-G30 Port Availability Preflight

**Date**: 2026-09-10  
**Scope**: Spec110 topology node TCP/UDP listener inputs  
**Status**: CLOSED_FOR_VALIDATION (bounded deployment harness repair)

## Finding

The process map validated only the numeric port range. With overlapping Slurm
steps, another job can already occupy a node's NFD TCP or UDP port. The old
supervisor would start NFD and report a later readiness timeout, while a local
MiniNDN run with one job would pass.

## Repair

`allocation_topology.validate_process_map` now rejects duplicate `(address,
transport-port)` endpoints within one map. Before any NFD step, the supervisor
runs a target-node IPv4 TCP and UDP bind probe for every node. An occupied port
fails with `SPEC110_PORT_NOT_AVAILABLE`, preserving the pre-start teardown
record and zero-NFD boundary. NFD remains the final authority if a listener
races the probe after it closes.

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
Ran 22 tests ... OK
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
bash -n tests/container/itiger-qwen-live/integration/test_network_scripts.sh
bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
SPEC110_PORT_NOT_AVAILABLE:0:16363:16363
NETWORK_SCRIPT_PASS
```

The integration negative case injects a failed target-node probe and verifies
exit code 4, `survivors: 0`, and no NFD log. This is a bounded availability
preflight, not a lease or automatic dynamic port allocator; concurrent-job
races after the probe and real Slurm/SIF/cross-node qualification remain open.
