# R11-B8-G20 Per-Node Workdir Visibility Gate

Date: 2026-09-10

## Finding and repair

The topology launcher accepted an absolute `--workdir` and checked it only
inside each generated process script. A bundle present on the submit node but
absent from one compute node could therefore start NFD and fail later during
the application readiness phase, unlike a MiniNDN run with one filesystem.

`run-allocation-topology.sh` now enumerates the validated node ranks and runs
`srun --relative=<rank> test -d <workdir>` before any NFD directory creation or
process startup. The existing per-process check remains as a race guard.

## Verification

```text
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
  PASS

bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS

python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  18 tests, OK
```

The fake `srun` integration covers the pre-start check but cannot model a real
node-local/shared filesystem split. No Slurm/SIF multi-machine qualification
was run; this card closes only the early workdir visibility boundary. The
canonical Spec110 topology contract and Tiger baseline guide were synchronized
with this requirement.
