# Spec170 current-source production D2b CPU-ONNX evidence (2026-08-19)

This record closes a previously untested local seam: the real C++ ONNX Runtime
CPU adapter is driven through two production `NativeProviderHandler` instances
across the request-scoped ACK/Selection path. It is local qualification only;
it is not a Tiger/CUDA result, a T029 freeze, or the complete 3A/3B/3C corpus.

## Fixture and build

The deterministic fixture was generated with:

```bash
python3 tests/fixtures/spec170/generate_cpu_onnx_fixture.py \
  /tmp/ndnsf-di-onnx-integration.sB0BBP
sha256sum /tmp/ndnsf-di-onnx-integration.sB0BBP/linear.onnx
```

The model is a float32 `x -> y=x+1` graph. The exact fixture used by the
production test is:

```text
linear.onnx
sha256=6662d53fad7b8a0f0a63d7bc8d28619194af73c3252a2c3ae1139a5f7db1ee53
ONNX Runtime C++: /opt/onnxruntime; CPUExecutionProvider
```

The integration target now links the real ONNX adapter explicitly. The
runtime readiness validator accepts the canonical equivalent CPU identities
`cpu:0` (Selection) and `cpu0` (ONNX `ExecutionEvidence`), while retaining
strict role, artifact, provider, backend, load, warmup, and fallback checks.

## Commands and results

Default fake-runner regression (proves the existing fixture path is unchanged):

```text
./build/integration-tests --run_test=Spec170NdnsfDiCoreFlow \
  --report_level=short --log_level=message
26 test cases; 372 assertions; PASS
```

Focused real production D2b path:

```text
NDNSF_DI_TEST_ONNX_MODEL=/tmp/ndnsf-di-onnx-integration.sB0BBP/linear.onnx \
  ./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse \
  --report_level=detailed --log_level=all
1 test case; 36 assertions; PASS
```

The same production path also rejects a mutated request-scoped group
capability before native execution:

```text
NDNSF_DI_TEST_ONNX_MODEL=/tmp/ndnsf-di-onnx-integration.sB0BBP/linear.onnx \
  ./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRejectTamperedD2bCapability \
  --report_level=detailed --log_level=message
1 test case; 8 assertions; PASS
```

The negative run observed a failed/rejected Selection execution, zero final
Response publications, and no timeout.  Thus the failure is on the real
ACK/Selection → Provider handler path, rather than a coordinator-only unit
fixture.

The current-source production SVS DATA_V1 boundary is also covered. The
positive path fetches two request-scoped segment wires through the real
Provider-to-Provider SVS bridge and opens the plaintext at the consumer. The
paired negative unwraps the SVS outer Data, flips one byte in the inner
segment, re-signs the transport envelope, and verifies that the consumer-side
`decodeSegment`/`openSegment` rejects it; the transport fetch itself correctly
returns the opaque wire, so this test does not claim that SVS performs
application-level AEAD validation.

```text
./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1 \
  --report_level=detailed --log_level=message
1 test case; 12 assertions; PASS

./build/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/ProductionIngressRejectsTamperedD2bSvsDataV1 \
  --report_level=detailed --log_level=message
1 test case; 9 assertions; PASS
```

The same production SVS bridge now has bounded transport-fault cases:

```text
ProductionIngressBoundsDroppedD2bSvsDataV1       1 case; 7 assertions; PASS
ProductionIngressDeduplicatesD2bSvsDataV1       1 case; 13 assertions; PASS
ProductionIngressReordersD2bSvsDataV1           1 case; 13 assertions; PASS
```

Drop is required to remain bounded without a complete segment fetch;
duplicate delivery reconstructs the plaintext exactly once; and reordered
delivery reconstructs the same plaintext in sequence order. These are
production Provider-to-Provider SVS bridge injections, not only unit-level
faults.

The test proves: two ACKs are observed, both Providers enter their production
handlers, each creates its request-scoped group coordinator, the Backbone and
Aux roles execute on Provider 0, the Head shard executes on Provider 1, all
three runners report real CPU ONNX evidence with no fallback, and exactly one
final Response is published.

The complete current-source integration suite with the same fixture enabled
also passes:

```text
NDNSF_DI_TEST_ONNX_MODEL=/tmp/ndnsf-di-onnx-integration.sB0BBP/linear.onnx \
  ./build/integration-tests --run_test=Spec170NdnsfDiCoreFlow \
  --report_level=short --log_level=message
26 test cases; 387 assertions; PASS
```

Current binaries:

```text
build/unit-tests         sha256=dad0adec3f3c123274e55e5c0f91ffd675d25139645073e9ce53402c791bcbd6
build/integration-tests  sha256=717cdf314b18a96688ceb869b57613541ee8dc4c88a923051d0f82b17e463b3b
```

The full local suites after this fix pass `495/495` unit cases with
`59978/59978` assertions and `34/34` integration cases with `477/477`
assertions. The added unit regression covers CPU identity acceptance and
rejects an unrelated CPU device ID (`4/4` assertions). The focused
`Spec170NdnsfDiCoreFlow` suite passes `26/26` cases and `372/372` assertions
without the external model, and `26/26` cases and `387/387` assertions with
the deterministic CPU ONNX fixture.

The current Spec170 Python contract subset was rerun against the repository
wrapper after the native changes:

```text
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference \
  python3 -m pytest -q tests/python/test_spec170*.py
101 passed, 9 skipped; 1 non-fatal torch.load FutureWarning
```

## Boundary

This evidence upgrades the current-source local production path from
transport-only/fake-runner coverage to a real CPU-ONNX D2b lifecycle, a
production capability-tamper negative, and a production SVS DATA_V1
consumer-rejection negative. It does not prove CUDA/NCCL, Tiger execution,
the complete 3B/3C mutation coverage, T029 freeze, or T036 performance
optimality. The production SVS drop/duplicate/reorder cases are now covered,
but the wider lifecycle/key-wrap/zeroization matrix remains open.
The r23 image is usable for its sealed revision only; any uncommitted source
change still requires a new exact-source SIF before promotion.
