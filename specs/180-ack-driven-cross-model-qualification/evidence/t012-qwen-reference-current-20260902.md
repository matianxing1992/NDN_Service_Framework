# T012 Qwen reference registration

**Status**: complete for immutable reference registration; local execution is
owned by T015 and is not claimed here.

## Bound identity

- Manifest: `contracts/qwen-reference-manifest-v1.json`
- Source feature: `175-ndnsf-di-streamed-invocation`
- Source revision: `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`
- Local model: `spec175-tiny-causal-lm-v1`, CPU ONNX Runtime/MiniNDN
- Q-C: Spec175 M01, one cold request, eight expected Tokens, one terminal Response
- Q-W: Spec175 M11, two valid conversation turns plus one mismatch rejection
- Controls: 5000 ms initial SVS settle, 1500 ms ACK timeout, 60000 ms request
  timeout, greedy decode, maximum eight generated Tokens

The manifest binds the local fixture manifest digest and keeps the Qwen3.6-27B
Tiger workload separate. In particular,
`packaging/ndnsf-di-container/jobs/spec175/workload.json` is a 27B/Tiger
workload with a 64-Token cap and 120000 ms deadline; it is not Q-C or Q-W.

## Verification

```text
PYTHONPATH=NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_spec180_qwen_reference.py
5 passed
```

The focused test verifies source revision and evidence paths, fixture digest,
all frozen controls and case oracles, separation from the 27B workload, and
mutation detectability. This is a registration and drift-prevention result;
it does not inherit Spec175 execution evidence and does not qualify Qwen3.6-27B
or Tiger.
