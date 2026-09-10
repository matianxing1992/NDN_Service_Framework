# R11-B8-G18 Portable Network Probe Dependency

Date: 2026-09-10

## Finding and repair

The diagnostic lane of `probe-multinode-network.sh` invoked `nc -z`, but the
container/runtime contract does not install or bind a netcat implementation on
every compute node. MiniNDN and a developer workstation can have `nc` in PATH,
so this dependency could remain hidden until a real allocation.

The probe now runs a bounded Python `socket` TCP or UDP connect through the
target node's `srun` step. It still uses the destination node's own transport
port and treats the result only as the selected/diagnostic connectivity
barrier. It does not claim NDN face, route, authorization, or request success.

## Verification

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/probe-multinode-network.sh
  PASS

bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS

python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  17 tests, OK
```

No real Slurm allocation or cross-host probe was available in this development
environment. The card closes only the optional-netcat dependency and preserves
the existing qualification boundary.
