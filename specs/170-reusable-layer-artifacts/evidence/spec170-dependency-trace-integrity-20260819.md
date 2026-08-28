# Spec170 dependency-trace integrity repair (2026-08-19)

## Symptom

In the retained r23 exact-SIF D0→D1 workload, the final request could succeed
while `collect_dependency_execution_evidence()` reported one incomplete edge.
The consumer had fetched the expected bytes; the suspicious producer record had
either an empty/mismatched name or a malformed value such as `data_name=0.004`.
This was evidence corruption, not proof that the wire transfer failed.

## Diagnosis

Two independent issues were found:

1. The Python ONNX trace uses `planned_name=true|false` as a fast-path marker
   and writes the actual NDN URI in `data_name`. The evidence parser compared
   the marker to the consumer URI.
2. The native timing logger emitted a long record through many concurrent
   `std::cout <<` operations. A combined D0→D1 run could therefore interleave
   fields from concurrent records.

The parser now canonicalizes the actual `data_name` URI and falls back to a
path-valued `planned_name` for the native format, while retaining both raw
fields in the evidence record. `NativeProviderHandler` now serializes the
complete timing/capacity diagnostic block with a shared mutex. The host
`di-native-provider` target rebuilt successfully after the C++ change.

## Regression evidence

```text
tests/python/test_spec170_dependency_evidence.py: 4 passed
./waf build --targets=di-native-provider -j2: PASS
unit-tests: 496/496 cases, 60009/60009 assertions: PASS
integration-tests: 34/34 cases, 480/480 assertions: PASS
tests/python/test_spec170_*.py with exact r23 inputs:
  111 passed, 6 skipped, 1 warning
exact r23 D0+D1 after parser repair:
  one direct block: 2/2 PASS, zero residual Providers
  five sequential blocks: 10/10 PASS, zero residual Providers
```

The source mutex is not present in the sealed r23 image. Therefore these
results prove the parser repair and bounded r23 behavior, while the logger
repair still requires a new source-bound SIF and one exact-SIF repeat before
the trace-integrity warning can be closed.

## Acceptance status

This repair improves protocol evidence integrity but does not complete T029's
source freeze, T028's full lifecycle-fault corpus, or T036 performance
optimality. Do not convert the bounded r23 result into a final release claim.
