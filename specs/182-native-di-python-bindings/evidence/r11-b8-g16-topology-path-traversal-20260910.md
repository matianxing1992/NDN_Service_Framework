# R11-B8-G16 Topology Path Traversal Boundary

Date: 2026-09-10

## Finding and repair

The previous job-scope check used a string prefix for `nfdSocket`. A path such
as `/tmp/ndnsf-di-current-job/../other/nfd.sock` still passed that check and
escaped the job directory after shell path resolution. The same class of
escape was possible for the rendered NFD state directory.

The topology validator, NFD config renderer and process launcher now reject
`..` path components for sockets and state directories. The launcher also keeps
the socket below the current `--scratch` root, and all checks occur before NFD
directory creation or process execution.

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  17 tests, OK (socket and state traversal cases included)

bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS

git diff --check
python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/probe-multinode-network.sh \
  Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
  PASS
```

This closes path validation only. It does not advance the exact-SIF v2 runner,
real Slurm/MiniNDN execution, native caller migration, or T016/T017.
