# R11-B8-G15 Job-Scoped NFD Socket Binding

Date: 2026-09-10

## Finding and repair

The Spec110 map validator accepted any socket beginning with
`/tmp/ndnsf-di-`. The launcher receives a job-owned `--scratch`, so a stale or
malicious map could still point at another job's socket/state directory. That
could make two Slurm allocations share an NFD endpoint or remove one another's
runtime files.

`render_process_launcher` now rejects a socket unless its path is below the
current scratch directory. The check is performed before generated execution
can create directories or start NFD. Existing path traversal and absolute
scratch checks remain in force.

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  15 tests, OK (including cross-job socket rejection)

bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS

git diff --check
python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/probe-multinode-network.sh \
  Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
  PASS
```

This is a launcher isolation gate only. It does not prove the v2 exact-SIF
runner, real Slurm node allocation, NFD route reachability, or the native
request qualification gates.
