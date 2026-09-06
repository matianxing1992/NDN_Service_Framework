# Quickstart Validation Guide

Spec 178 introduces a real MVCNN-family ONNX artifact on CPU. The repository now contains the qualified, hash-pinned artifact and checkpoint; the commands below reproduce the acceptance path and must fail closed if provenance or hashes drift.

## 1. Inspect the Reference CPU Runtime

```bash
python3 - <<'PY'
import onnxruntime as ort
print(ort.__version__)
print(ort.get_available_providers())
PY
```

The qualification report must identify `CPUExecutionProvider`. Merely having it available is insufficient; the model session must activate only that provider.

## 2. Qualify the Native Checkpoint and ONNX Export

```bash
python3 NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py \
  --output-dir NDNSF-UAV-APP/models
```

Expected: verified code/weight license, checkpoint and ONNX SHA-256, ONNX checker pass, exact image/mask/logits/pooled-feature contract, and native-versus-ONNX outputs within the registered tolerance. The command must not download an artifact implicitly.

## 3. Run Real CPU Model Tests

```bash
python3 -m pytest -q tests/python/test_uav_mvcnn_onnx_contract.py
python3 -m pytest -q tests/python/test_uav_mvcnn_onnx_inference.py
```

Expected: 1/2/4/6-view execution, CPU-only provider evidence, repeatability, permutation equivalence, mask/padding correctness, pooled-feature sensitivity, and fail-closed artifact/input/output cases.

## 4. Run the Generated Fixture as Claim-Bounded Real-Model Evidence

```bash
python3 NDNSF-UAV-APP/tools/run_multiview_fixture.py \
  --manifest NDNSF-UAV-APP/testdata/multiview-car/manifest.json \
  --mode real --profile-id vehicle-mvcnn-v1 --views 6 \
  --output results/uav-mvcnn-onnx-fixture
```

Expected: one real ONNX inference, six accepted inputs, one fused result, six annotations, explicit CPU provider, and `fallbackUsed=false`. This remains functional-only because the generated images are not a calibrated scientific dataset.

## 5. Run CPU Integration and Existing Regressions

```bash
./waf build --target=unit-tests --target=integration-tests
./build/unit-tests --run_test=UavMultiViewRecognition
./build/integration-tests --run_test=UavMultiViewIntegration
python3 -m pytest -q tests/python/test_uav_multiview_*.py
```

Expected: the real-model extension passes while the Spec 176 single-view and Spec 177 deterministic multi-view paths retain their registered behavior.

## 6. Run Real MiniNDN

```bash
sudo -n python3 NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py \
  --fixture NDNSF-UAV-APP/testdata/multiview-car/manifest.json \
  --model-mode real \
  --execute --all-scenarios \
  --output results/uav-mvcnn-onnx-minindn
```

Expected: multiple UAV producers, two eligible Providers, one selected Provider running the real CPU ONNX session, exact Data retrieval, one terminal owner, and distinct network/model/publication failure stages.

## 7. Run the Paired 1/2/4/6-View Campaign

```bash
python3 NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py \
  --registration <registered-vehicle-dataset.json> \
  --output results/uav-mvcnn-onnx-controlled/evaluation.json
```

Expected: complete paired rows or explicit failures for every eligible target, recognition metrics, latency, bytes, memory, effect sizes, confidence intervals, exclusions, and immutable artifact/result hashes. A positive advantage is not required; conclusions must follow the registered uncertainty.
