# Current-source local regression rerun (2026-08-19)

This record captures the latest local rerun after the DATA_V1 request/capability
filter and explicit no-progress-bound changes.  The working tree was dirty at
the time of the run; this is current-source host evidence, not evidence that
the sealed r23 SIF contains these bytes.

## Identity and retention

```text
repository: /home/tianxing/NDN/ndn-service-framework
HEAD:       989a9daace669a4f93496dade3176c527edb2469
tree:       dirty (uncommitted source and generated entries present)
filesystem: 177G total, 95G used, 74G available, 57% used
```

The binaries used by the rerun were:

```text
build/unit-tests         28c4b6350cf071711399aa8aa09b603323ff560ef20360a3d6a099c54200ba86
build/integration-tests  b1c4ee6d3f1175e1f6f6de4a06150a68ace68d89e22f857b708fd7ee43b11428
```

## C++ regression

The full unit suite was run on one CPU core to remove the unrelated
wall-clock scheduler jitter in the UAV timing test:

```bash
taskset -c 3 ./build/unit-tests --report_level=short --log_level=error
```

Result:

```text
496 test cases out of 496 passed
60009 assertions out of 60009 passed
EXIT_CODE=0
```

The full integration target was run serially because its legacy fixture uses a
shared default PIB:

```bash
./build/integration-tests --report_level=short --log_level=error
```

Result:

```text
34 test cases out of 34 passed
480 assertions out of 480 passed
EXIT_CODE=0
```

The earlier 495/59,978 and 34/477 totals belong to the preceding binary and
are retained in their historical evidence files.  They must not be quoted as
the latest current-source totals.

## Python contract regression

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments:pythonWrapper \
  python3 -m pytest -q -rs tests/python/test_spec170_*.py
```

Result:

```text
103 passed, 9 skipped, 1 warning in 5.30 s
```

The nine skips are explicit external-environment gates: exact-SIF checks,
real NativeTracer/MiniNDN, and cached Qwen multi-request execution.  The one
warning is the existing `torch.load(weights_only=False)` warning; no test
failed.

## Interpretation

This rerun strengthens current-source local protocol qualification and covers
the cancellation/filter changes together with the full local C++ and Python
contract suites.  It does not close the production 3A/3B/3C transport matrix,
the T029 frozen-candidate gate, the exact-current-source SIF gate, or the
publication-quality T036 performance analysis.
