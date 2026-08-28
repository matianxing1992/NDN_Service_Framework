# Current-source local regression rerun (2026-08-19)

This rerun was performed after the exact-SIF gate check and candidate-input
inventory update. It describes the current working tree only; the r23 SIF
does not contain these uncommitted bytes.

## Results

| Lane | Command | Result |
|---|---|---|
| C++ unit | `taskset -c 3 ./build/unit-tests --report_level=short --log_level=error` | **496/496 cases, 60,009/60,009 assertions, exit 0** |
| C++ integration | `./build/integration-tests --report_level=short --log_level=error` | **34/34 cases, 480/480 assertions, exit 0** |
| Python Spec170 contract glob | `PYTHONPATH=... python3 -m pytest -q tests/python/test_spec170_*.py -rs` | **107 passed, 10 skipped, 1 warning, exit 0**; see `spec170-python-contract-rerun-20260819.md` |

The integration lane was run serially because the legacy fixture shares a
default PIB. The unit output included dependency cancellation/deadline,
admission lease mismatch, and provider replay rejection markers. The
integration output included multi-role, three-provider, custom-selection, and
DATA_V1 drop/duplicate/reorder request terminal markers.

## Scope

These are strong current-source local regressions and named negative cases.
They do not prove exact-SIF parity for the dirty tree, the complete T018/T028
mutation/lifecycle matrix, T029 freeze, or T036 performance optimality.

## Fresh rerun evidence

The three lanes were rerun from the same current working tree after the
post-certificate cancellation evidence was recorded. The compact logs are
retained in `/tmp` for this session with these SHA-256 digests:

```text
python:      9c55c76543fdcc06ebe2058736b03bcad058b3cccd511b1618b44d0903ab8fc3
unit:        ede72736dfff446e684d8b72ab48bdca5a50f052d27ff518ac3dc06a5be1eae4
integration: 25e44b43d22dc94b3a7d65bd025281553300579d6e1807fe3fbbfc32fbfa275b
```

The Python count is higher than the earlier snapshot because the current test
glob now collects two additional contract tests. The ten skips remain explicit
external-environment gates (exact-SIF/NativeTracer/MiniNDN/Qwen), not silent
passes.
