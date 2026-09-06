# Spec180 audit evidence — iteration 45

Date: 2026-09-02

## Scope

This checkpoint re-audited the canonical root publication boundary after the
external-initializer transport repair. It did not claim YOLO, MiniNDN, SIF, or
Tiger qualification.

## Finding and repair

The previous root publisher could carry graph-source metadata but had no
contracted way to bind or publish the external initializer object. The
canonical artifact API now accepts `canonicalInitializerDataName`,
`canonicalInitializerObjectDigest`, and `canonicalInitializerBytes` as one
validated tuple. `CanonicalCatalogEnsurer` accepts the matching payload,
publishes it before layer manifests and the root, and preserves the root-last
barrier. Inline and source-only callers remain compatible.

## Verification

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
python3 -m pytest -q tests/python/test_spec170_canonical_layers.py
...........                                                              [100%]
11 passed in 0.95s

PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
python3 -m pytest -q tests/python/test_spec175_native_assembly_helper.py
...                                                                        [100%]
3 passed in 6.26s

PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
python3 -m pytest -q tests/python/test_spec170_canonical_layers.py \
  tests/python/test_spec175_native_assembly_helper.py \
  tests/python/test_spec170_provider_assembly.py
20 passed in 6.33s
```

The related regression command passed 20 tests; the authoritative individual
canonical-layer result above is 11 tests. The earlier focused C++ object compile
remains `CXX_OBJECT_RC=0`; no C++ source changed in this iteration.

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
timeout 240s python3 -m pytest -q tests/python/test_spec180_*.py
91 passed, 19 warnings in 34.55s
```

The Spec Kit structural audit remains PASS (25 FR, 9 SC, 20 tasks, 25 traced
requirements). The contract gate remains `status=PASS`,
`contractReady=true`, `qualificationReady=false`, with the registered trust
root configured.

## Remaining qualification boundary

The signed YOLO package, production trust-backed offer verifier, real
`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` runner, T014 convergence PASS,
MiniNDN cases, SIF replay, and Tiger jobs remain missing. T007 is still partial
because native/wire parity and the live ACK-to-Response path are not complete;
T011/T014/T015--T020 remain open. This evidence is not a performance or
functional qualification result.
