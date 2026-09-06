# Spec180 audit evidence — iteration 44

Date: 2026-09-02

## Scope

This checkpoint audited the canonical YOLO graph/initializer transport into the
Provider-local ONNX assembler. It did not claim a YOLO qualification result.

## Finding and repair

The Spec180 exporter emits a graph object and a separate external initializer
object. The previous bridge fetched only the graph, then asked ONNX Runtime to
load external data that had never been staged. The repair binds the initializer
object by NDN name, byte length, and raw SHA-256 in signed root metadata;
`NativeCanonicalOnnxAssembler` fetches/verifies it, the native helper accepts a
sibling `model.onnx.data`, and the Python assembler normalizes an
exporter-specific external-data filename before checking the normalized
initializer digest after loading the pair. Inline graphs remain compatible.
Multiple external-data locations are rejected rather than silently merged into
one transport object.

## Verification

Command:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
python3 -m pytest -q tests/python/test_spec175_native_assembly_helper.py
```

Result: `3 passed in 6.26s`.

The modified native bridge also compiles as a C++17 object with the configured
ndn-cxx/NDN-SVS/Boost/ONNX Runtime include paths (`CXX_OBJECT_RC=0`, object
size 14,210,040 bytes). The full Waf build was intentionally not used as a
qualification gate because it expands to the unrelated all-target rebuild;
the focused object compile is the relevant source check here.

The three tests cover four-provider inline assembly, recipe/source mutation
rejection, external-initializer assembly, and graph-only external assembly
rejection. The existing Spec180 collection remains `91 passed, 19 warnings`
because this regression is maintained with the shared native-assembly tests.

## Remaining qualification boundary

The post-repair document checks remain structural `PASS` (25 FR, 9 SC, 20
tasks, 25 traced requirements), and the contract gate remains
`status=PASS`, `contractReady=true`, `qualificationReady=false`, with the
registered trust root configured.

The signed YOLO package, production trust-backed offer verifier, real
`NDNSF_DI_YoloAckDriven_Minindn.py` runner, T014 convergence `PASS`, MiniNDN
cases, SIF replay, and Tiger jobs are still missing. T007 is partial and
T011/T014/T015--T020 remain open. The canonical catalog/ensurer also still
needs to publish and bind the external initializer metadata in the signed root;
the bridge support alone is not a qualification path. This evidence is not a
performance or functional qualification result.
