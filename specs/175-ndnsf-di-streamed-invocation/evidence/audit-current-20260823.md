# Spec 175 current audit checkpoint (2026-08-23)

This is the current development checkpoint after the registered I01-I15
matrix refresh and the fail-closed Provider-completion fix. The native G2
matrix passes; promotion remains blocked because the source tree is not sealed
and the automatic Python workload proof is still open. A subsequent
MiniNDN-selection correction now preserves the actual ONNX/native backend in
Provider residency offers, and the native Provider target has been rebuilt
against the local Experimental NDN-SVS API; neither source/build correction is
itself a workload qualification.

## Executed gates

```text
python3 scripts/spec175_contract_gate.py \
  --feature-dir specs/175-ndnsf-di-streamed-invocation \
  --output results/spec175/g0/qualification-manifest-current-final-20260823.json
  -> BLOCKED (DIRTY_INPUT_TREE; 165 in-scope dirty paths)

python3 scripts/run_spec175_integration_gate.py \
  --binary build/integration-tests --cases I01-I15 \
  --healthy-repeats 3 --seed 1750001 \
  --output results/spec175/g2/qualification-manifest-live-i13-20260823.json
  -> PASS (27/27; I01-I15 registered, healthy cases repeated three times)
     See `evidence/g2-live-i13-20260823.md` and the generated manifest.

./build/unit-tests --log_level=message
  -> PASS (567/567)

./build/integration-tests --log_level=message
  -> PASS (67/67)

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
  -> PASS (86 passed, 1 skipped)
```

Development binary hashes:

```text
13a37b4ab5c7dfecbe05f2d6c0359319130bfa4ededf8a4ca1cb49e095a27493  build/unit-tests
a6a8dd638d467483c0758fc69503f51a4774eedb53126e930628e641c22799e6  build/integration-tests
```

## Interpretation

- I01-I03, I12, and I15 are registered formal healthy G2 subjects. Each has three
  fresh-process repetitions passing the native tiny-ONNX Request/ACK/Selection/
  NativeProviderHandler/ORT/Event/End/Response path. I02/I03 prove
  epoch-specific activation and token-feedback DATA_V1 across two/four
  Providers; I15 proves a noncanonical ACK-driven role map with the same oracle.
- I04-I11 and I13-I14 also pass their registered fault/control processes; the
  complete result is recorded in
  `evidence/t016-t018-g2-matrix-20260823.md`.
- The gate requires every Provider coordinator to finish cleanly. This closes
  the false-green condition where the final Response succeeded while an
  intermediate Provider later failed during terminal drain.
- Controlled I12 is registered and passes all three fresh-process repetitions;
  its focused output records two attempts, one replacement execution, and one
  terminal response. I13 is registered as the default-disabled live-transport
  boundary; it detaches once at cursor 4 and produces no second Request or
  Response.
- The full source-tree regression was rerun after the r27 source-closure fixes;
  the native unit count is 567 and the full integration binary reports 67/67.
  This is a development
  regression result, not a sealed G0/G1 promotion result.
- G0 remains blocked only because the shared worktree is not a sealed promotion
  subject. No user-owned changes were cleaned, reset, staged, or overwritten.
- G1, G3, G4, G5, G6, and G7 were not run. No SIF, MiniNDN, CUDA, or
  performance claim follows from this checkpoint.

The next implementation gate is the automatic Python workload proof plus the
remaining T017/T018 negative inventories. G0/G1 closure, the complete I01-I15 G2 manifest, and the
sealed local MiniNDN/SIF candidate must precede any Tiger promotion.

## Local SIF candidate update

The old local candidate remains rejected because its deployment environment
contains `functorch`.  A fresh r27 candidate was then built from a new sealed
source subject after fixing two direct native-target source-closure omissions:
`InvocationStream.cpp` and `NativeEpochCoordinator.cpp`.

```text
r27 SIF: sha256:b957f7a5fd1ceef135c2fa9ad548f187f74fbac1389e0ab5f6bed9221f43ba60
source seal: sha256:db303600d7523c109b255879b9ed4b8967a642a99d49b50d82d5f9daceca980b
definition: sha256:7764596db56e3243c2404d91a4c79cc387475e5da4c5b50363b5164e5740be42
preflight: PASS (Python 3.10.18, ORT CUDA EP, no torch/Transformers/functorch, ldd PASS)
build-record validation: PASS
```

This makes r27 eligible for the next local gate; it does not change the G0/G2
status above and does not authorize Tiger submission.

## 2026-08-24 corrections

`Experiments/NDNSF_DI_LlmPipeline_Minindn.py` previously rejected ONNX/native
Selection Dataflow runs and, in the accepted Transformers path, was the only
place that assigned residency backend labels. It now accepts
`qwen-onnx`/`qwen-onnx-cpu-native` and emits `onnxruntime-cuda` or
`onnxruntime-cpu`, while keeping Transformers labels restricted to the
Transformers runtime. The correction is covered by the contract test and
recorded in `evidence/t015-onnx-selection-backend-20260824.md`.

The real-model admission gate had a related schema mismatch: the ONNX
launcher spelling was compared with the stage manifest's backend spelling.
It now maps both ONNX launcher modes to the canonical `onnxruntime` stage
value; both modes are covered by a manifest-bound regression test. This is
still only an admission/source correction until a fresh P1/P2 MiniNDN process
produces workload evidence.

The first native Provider rebuild linked the system's stale `/usr/local`
NDN-SVS and failed at missing Experimental catch-up/statistics methods. The
build was reconfigured with the explicit local NDN-SVS source/build pair and
then succeeded; its binary hash and `ldd` closure are recorded in
`evidence/t015-native-provider-build-20260824.md`. A real fresh-process
Python-user-to-native-Provider run remains the next T015 gate.

The post-correction G0 check remains an honest expected-negative result:
`results/spec175/g0/qualification-manifest-current-20260824-final.json`
(`sha256:c3fe257c5fff512ca29f4ca36a8576fd7ec5f0b199be45d34d0e81620fe0b401`)
reports complete 58-FR/12-SC coverage and the single blocker
`DIRTY_INPUT_TREE`; it does not claim G0 PASS.
