# R11-B8-G19 Visible GPU Binding Proof

Date: 2026-09-10

## Finding and repair

The Provider launcher previously queried every GPU returned by `nvidia-smi` and
accepted a map UUID if it appeared anywhere in that list. On a Slurm node this
could pass while the task was bound to a different device; MiniNDN has no
equivalent GPU visibility boundary.

The launcher now requires a single `CUDA_VISIBLE_DEVICES` selector, queries
that selector with `nvidia-smi -i`, and requires the output to equal the
process-map `gpuUuid` exactly. A missing selector, failed query, or mismatch
fails before the Provider executable and readiness marker run.

## Verification

```text
python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py
  18 tests, OK (valid isolated binding, wrong UUID, and missing selector)

bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh
  NETWORK_SCRIPT_PASS

python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py
bash -n Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh
  PASS
```

The tests use fake `nvidia-smi` and do not establish that a real Slurm GPU
cgroup exposes the same selector/UUID representation. No real GPU allocation,
SIF, or multi-machine qualification was run; this card closes only the
pre-exec binding check.
