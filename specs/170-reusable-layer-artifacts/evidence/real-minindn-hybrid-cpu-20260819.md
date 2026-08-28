# Real MiniNDN hybrid CPU qualification (2026-08-19)

This record promotes the positive `[1,2,1]` and `[2,1,2]` post-Selection
paths from fixture-only checks to the real `NDNSF_DI_NativeTracer_Minindn.py`
request path. It is local CPU evidence only: it is not a CUDA/Tiger result,
does not cover mutation faults, and does not authorize T029 or T036.

## Identity and environment

```text
source revision:       989a9daace669a4f93496dade3176c527edb2469
working tree:          dirty; this run used the locally rebuilt binaries
runner:                Experiments/NDNSF_DI_NativeTracer_Minindn.py
runtime:               onnxruntime-cpu
ONNX Runtime:          /opt/onnxruntime/lib/libonnxruntime.so.1.26.0
topology:              Experiments/Topology/AI_Lab.conf
fault injection:       none
CPU fallback:          false (ExecutionEvidence)
```

## Commands and results

```text
SPEC170_RUN_REAL_NATIVE_MININDN=1 \
SPEC170_ORT_LIBRARY_PATH=/opt/onnxruntime/lib \
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference \
python3 -m pytest -q tests/python/test_spec170_real_minindn_gate.py \
  -k 'hybrid_121 or hybrid_212'

2 passed, 9 deselected in 84.67s
```

The test requires the real MiniNDN topology, real Provider processes, the
post-Selection assignment path, the CPU ONNX Runtime runner, dependency
publish/fetch closure, and the numerical oracle. The `[1,2,1]` and `[2,1,2]`
cases both returned one complete response; the test verified the exact role
mapping, expected dependency-edge set, no missing publications/fetches/name
mismatches/empty payloads, and `cpuFallbackUsed=false`.

An isolated rerun of `[2,1,2]` also passed (`1 passed, 10 deselected,
42.44s`). Its preserved summary reported five complete dependency edges,
`numericalOracle.status=PASS`, `maxAbsoluteError=0.0`, and one successful user
request. A first sequential invocation had one transient failure before the
isolated rerun; because the two subsequent runs passed, this is retained as a
reproducibility warning rather than counted as a protocol PASS or silently
discarded.

## Boundary

This closes only the real positive 3C request path for the two frozen mappings.
The complete omitted/duplicate/wrong-redistribution, delayed-rank, cycle,
loss, cancellation, and zero-partial-output mutation corpus remains open.
The current source also still requires a new exact-source SIF before any
TigerCluster execution.
