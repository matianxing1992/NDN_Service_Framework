# Spec170 local collective-runtime evidence (2026-08-19)

This record documents the bounded local regression added after the previous
protocol audit. It is qualification evidence only; it does not freeze a SIF or
claim that the production ONNX/NCCL path is complete.

## What is covered

`CollectiveRuntime` models one authenticated, fixed-rank collective group. A
group can start only after every rank has authenticated, passed local
readiness, and reported input readiness for the same epoch. Progress is
monotonic per rank. A rank failure, cancellation, no-progress timeout, or hard
deadline is a terminal transition for the whole group. The runtime is now
bound to `ProviderRoleWorker`: dependency-ready rank work is held until the
group starts, all released ranks complete through the same worker path, and a
worker exception fails the whole group. The implementation has no global
model-ready barrier; the snapshot records
`usedGlobalModelReadyBarrier=false`.

The deterministic unit corpus covers four classes over 50 fixed seeds each:

| Class | Seeds | Expected terminal outcomes |
|---|---:|---|
| zero-delay | 50 | completed |
| delayed-input | 50 | completed |
| dropped-progress | 50 | 33 completed, 17 stalled |
| rank-failure | 50 | failed |

For every completed row, a two-rank sliced tensor result is compared with the
unsplit reference. This is a state-machine/oracle test, not a production model
or transport test.

## Commands and results

```text
./build/unit-tests --run_test=DistributedInferenceCollectiveRuntime \
  --report_level=short --log_level=message
  8 test cases; 2,313 assertions; PASS (fixture-gated adapter case skipped)

NDNSF_DI_TEST_ONNX_MODEL=/tmp/ndnsf-di-onnx-cpu-fixtures/linear.onnx \
  ./build/unit-tests --run_test=DistributedInferenceCollectiveRuntime \
  --report_level=short --log_level=message
  8 test cases; 2,336 assertions; PASS (real CPU ONNX adapter case enabled)

./build/unit-tests --report_level=short --log_level=message
  495 test cases; 59,978 assertions; PASS

./build/integration-tests --report_level=short --log_level=message
  34 test cases; 477 assertions; PASS
```

The full integration target was run serially. A deliberately parallel unit +
integration invocation is not evidence: both legacy fixtures attempted to
open the same default PIB and one aborted with `database is locked`.

The full local run includes the existing integration and security gates. ONNX
smoke cases that require an unset external model/runtime are reported as
skipped by their existing guards; they are not counted as production-model
evidence.

Binary hashes for this run:

```text
build/unit-tests         dad0adec3f3c123274e55e5c0f91ffd675d25139645073e9ce53402c791bcbd6
build/integration-tests  717cdf314b18a96688ceb869b57613541ee8dc4c88a923051d0f82b17e463b3b
```

The current-source production D2b path was also enabled with the deterministic
CPU ONNX fixture. The focused production test passed `1/1` case and `36/36`
assertions; the complete `Spec170NdnsfDiCoreFlow` suite passed `26/26` cases
and `387/387` assertions. See
[`production-d2b-onnx-cpu-local-20260819.md`](production-d2b-onnx-cpu-local-20260819.md)
for the exact command and fixture hash.

The production SVS DATA_V1 boundary is now also exercised: the positive
request-scoped segment fetch/open path passes `1/1` case and `12/12`
assertions, while a mutated inner segment is rejected by the consumer
`decodeSegment`/`openSegment` path in `1/1` case and `9/9` assertions.
The same production bridge also passes bounded drop (`7/7`), duplicate
(`13/13`), and reorder (`13/13`) cases; drop remains bounded without a
complete fetch, while duplicate and reorder reconstruct the exact plaintext.

## Boundary and remaining gaps

The r23 image is valid for its sealed source revision only. The current
working tree contains uncommitted/generated entries outside that seal, so this
regression does not qualify those changed bytes; no new SIF was generated
during this local regression. A new local SIF and closure record are required
before executing any such changed source.

This evidence strengthens the local T016/T028 qualification set, including the
real CPU ONNX two-rank adapter regression and the current-source production
D2b lifecycle recorded in
`production-d2b-onnx-cpu-local-20260819.md`, but does not close either task.
The wider cross-Provider key-wrap/zeroization lifecycle, CUDA/NCCL execution,
the complete production 3A unsplit numerical oracle, and the complete T028
security/lifecycle matrix still require explicit integration evidence. It also does not close
T029 (immutable release freeze) or T036 (statistically supported performance
optimality). The current `runtime-r23.sif` remains a bounded verification
candidate only; no new SIF was generated for this test.
