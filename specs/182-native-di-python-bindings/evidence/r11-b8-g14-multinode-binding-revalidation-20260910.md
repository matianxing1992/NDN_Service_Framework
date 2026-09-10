# R11-B8-G14 Multi-Machine Binding Revalidation

Date: 2026-09-10

## Scope and finding

This bounded review followed the Spec110 topology launcher from frozen process
map validation through generated argv, GPU selection, NFD route probing and
`exec`. MiniNDN gives each process a convenient local environment, so three
deployment-only divergences were actionable:

1. A process command could repeat its read-only `identityRef`. Copying the
   identity into a private `HOME` therefore did not stop an executable from
   reopening shared storage. Exact `--identity PATH` and `--identity=PATH`
   tokens now bind to `$runtime_home` after the copy.
2. Provider launch used `--gpu-bind=map_gpu:N` but never checked that the
   allocated node exposed the expected `gpuUuid`. The launcher now requires a
   successful `nvidia-smi` query containing that UUID before `exec`.
3. The diagnostic network lane tested UDP on the selected TCP port (or TCP on
   the selected UDP port). Each lane now probes the destination node's own
   `tcpPort` or `udpPort` while retaining the selected-transport gate.

All three failures remain fail-closed. The fixture-only
`NDNSF_SPEC110_TEST_MODE=1` path may bypass the hardware check for fake
binaries; it is not a deployment result.

## Changed boundary

- `packaging/ndnsf-di-container/lib/allocation_topology.py`
  - deterministic identity-argument rebinding;
  - Provider visible-UUID pre-exec guard;
  - strict Provider UUID presence check.
- `Experiments/TigerCluster/adapters/slurm-apptainer/scripts/probe-multinode-network.sh`
  - transport-specific destination port selection.
- `tests/container/itiger-qwen-live/unit/test_allocation_topology.py`
  - identity argv rebinding and wrong-UUID rejection, in addition to the
    existing identity-copy environment probe.

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  14 tests, OK

bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS

git diff --check
python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/probe-multinode-network.sh \
  Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
  PASS
```

The full C++ Spec182 selector and real Slurm/SIF/MiniNDN execution were not
needed for this launcher-only boundary and are not advanced by this record.
Real multi-node qualification must still verify the exact SIF, mounted
identity source, node-visible GPU UUID, selected NFD route and native request
chain on the target allocation.
