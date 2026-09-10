# R11-B8-G17 Remote Launcher Materialization

Date: 2026-09-10

## Finding and repair

The topology supervisor generated launchers under the evidence directory on
the submitting node and passed those absolute paths directly to `srun`. A
multi-node allocation may keep evidence on submit-host or shared storage that
is not mounted on every compute node, while MiniNDN and the fake local `srun`
fixture make that path look universally visible.

`run-allocation-topology.sh` now materializes each generated process launcher
with an `srun --relative=<nodeRank>` step into the node's job scratch, sets its
mode, and executes the scratch copy. NFD launchers use the same node-local
path. The source copy remains in evidence for audit, but remote process
execution no longer depends on its visibility.

## Verification

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
  PASS

bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS
  normal supervisor: teardown PASS; nfd-0/controller/user/provider-0/provider-1/provider-2
  launchers materialized and executable below job scratch

python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  17 tests, OK
```

The fake `srun` integration covers the materialization and execution order;
no real Slurm allocation, SIF transfer, route, GPU, or cross-host run was
available in this development environment. The card closes this launcher
filesystem boundary only and does not advance T016/T017 or R11-B9.
