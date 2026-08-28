# Spec175 audit full unit/Python regression — 2026-08-23

Status: `DEVELOPMENT_PASS`; formal G1 remains open until the sealed source
subject and final T020 manifests close.

## Native unit binary

```bash
./build/unit-tests --log_level=message
```

- Result: `567` test cases, `*** No errors detected`
- Binary SHA-256:
  `ba5eb8bca2a9dac2d3df168f5ef0e8ffed799b9a88ac7134b1472a5dffa0d97b`
- Captured log SHA-256:
  `3cb9ac3f592f6728d0903abff00b778af568140e84254ec251fbe888090d53ac`

The run includes existing unary, Targeted, authorization/replay, streaming/FEC,
native DI, Spec174, and Spec175 suites; it was executed after correcting the
native attempt-epoch fallback and adding request-contract digest validation.

## Registered Spec175 Python inventory

```bash
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
  python3 -m pytest -q --tb=short \
  tests/python/test_streamed_invocation_api.py \
  tests/python/test_spec175_cpu_fixture.py \
  tests/python/test_spec175_evidence.py \
  tests/python/test_spec175_integration_gate.py \
  tests/python/test_spec175_qwen_stateful_onnx.py \
  tests/python/test_spec175_streamed_generation.py \
  tests/python/test_spec175_qwen_generation.py \
  tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec175_onnx_deployment_boundary.py \
  tests/python/test_build_local_sif_record.py \
  tests/python/test_spec175_sif_preflight.py
```

Result after the Qwen subject-identity audit: `86 passed, 1 skipped`. The refreshed G0/G2
gate tests include structured missing-
binary classification, I12/I13 missing-case enforcement, and in-scope dirty
source filtering. The two additional cases lock the FP16/text-only/
single-token/MTP-disabled Qwen profile and mutate the offline sealer's declared
modality, decode mode, and component inventory.

## Boundary

This evidence is source-tree development evidence. It does not claim a clean
sealed G1 manifest, complete I12/I13, automatic-DI Python process proof,
MiniNDN, Qwen3.6 CUDA, or performance qualification.
