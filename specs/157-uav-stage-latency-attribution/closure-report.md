# Closure Report

**Status**: Closed — offline diagnostic PASS  
**Canonical output**:
`results/spec157-uav-stage-latency-diagnostic-20260726T204922Z`

## Result

All 6 frozen Spec 156 cells were analyzed without rerunning MiniNDN. The report
contains per-frame CSVs and aggregate JSON/CSV/Markdown with mean, p50, p95,
and p99 for all five requested boundaries.

The inverse FPS/latency trend is dominated by capture-to-encoded and
encoded-to-last-materialized time. Materialized-to-decoder-input remains
approximately constant, so the data does not support an Interest-under-supply
explanation.
