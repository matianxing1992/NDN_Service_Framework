# Spec 170 focused Gate A rerun (2026-08-19)

This is a current-source qualification record. It supplements, but does not
replace, the formal T025 Gate A record and does not authorize T029 freeze.

## Inputs

- Repository: `/home/tianxing/NDN/ndn-service-framework`
- `HEAD`: `989a9daace669a4f93496dade3176c527edb2469`
- Working tree status rows at run time: `506` (the tree was not clean)
- Python path: `pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments`
- The documented `canonical_artifacts` filename was corrected to the actual
  `canonical_layers` filename in `quickstart.md` before this rerun.

## Results

### Python contract subset

Command:

```bash
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
python3 -m pytest -q \
  tests/python/test_spec170_canonical_layers.py \
  tests/python/test_spec170_runtime_topology.py \
  tests/python/test_spec170_placement_v3.py \
  tests/python/test_spec170_hybrid_execution.py \
  tests/python/test_spec170_v2_v3_compatibility.py \
  tests/python/test_spec170_ack_no_reservation.py \
  tests/python/test_spec170_artifact_security.py \
  tests/python/test_spec170_content_addressed_reuse.py \
  tests/python/test_spec170_multi_device_provider.py
```

Result: **32 passed, 1 warning, 3.06 s**. The warning is the existing
`torch.load(weights_only=False)` FutureWarning; no test failed.

### C++ ndn-svs smoke

```bash
build/integration-tests --run_test=NdnSvsSmoke \
  --report_level=short --log_level=error
```

Result: **2/2 cases, 26/26 assertions passed**.

### C++ NDNSF-DI core flow

```bash
build/integration-tests --run_test=Spec170NdnsfDiCoreFlow \
  --report_level=short --log_level=error
```

Result: **26/26 cases, 372/372 assertions passed**. The run emitted READY,
request publication, terminal RESET, cancellation, and deadline evidence.

Captured log hashes:

```text
Python: c3a17ab367880e2e6054c6c007bc89e455536bc739cdbb1953858cf6aacbb4da
SVS:    67fc1297b1bebe8f6fb4ff710d5f23788b6ec54cf1eb67ee09a18d680faee06d
Core:   0e033cfe6859422cb62d91f3c0de96f356c50859f07a6f3b7c3da721339d976d
```

## Boundary

This rerun strengthens local Gate A qualification and confirms that the
quickstart command now names an existing test file. It does **not** prove the
complete T025 matrix, real-Qwen Gate B, exact-current-source Gate C, T028's
full mutation corpus, T029 freeze, or T036 performance optimality. The
overall Spec 170 closure therefore remains **BLOCK**.
