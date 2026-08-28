# Traceability

| Requirement | Implementation | Verification | Evidence |
|---|---|---|---|
| FR-001 | Five-boundary stage model in `Experiments/analyze_uav_video_stage_timeline.py` | Exact synthetic timeline | Per-frame CSVs |
| FR-002 | Capture boundary reads `capture_origin_ns` | Known capture fixture | `capture_us` column |
| FR-003 | Stream, epoch, frame, and cursor join maps | Exact multi-cursor fixture | Per-frame identity columns |
| FR-004 | Greatest source cursor selects materialization end | Two-source fixture expects cursor 11 | Focused unittest PASS |
| FR-005 | Per-stage count/mean/p50/p95/p99 and source-segment statistics | Six cells have nonempty statistics | `campaign-stage-summary.json` and `.csv` |
| FR-006 | Decoder-output wall time filtered by each existing measurement window | Fixture includes an out-of-window exact output | Focused unittest PASS |
| FR-007–FR-008 | Output guard and offline-only CLI | Nested output test plus unchanged diagnostic-input hash | Hash `f597292de62296215cdc2d114d5157cae6e09be6519f2f95dbc130879eda2691` |
| FR-009 | Per-frame deltas; no percentile addition | Report wording and source review | `stage-attribution.md` |
| SC-001 | Synthetic exact/missing/guard/write cases | 4/4 unittest PASS | `tests/python/test_spec157_uav_stage_latency_attribution.py` |
| SC-002–SC-003 | Read-only six-cell replay | 15969/15969 complete, 100% coverage, zero exclusions | Canonical diagnostic result root |
| SC-004 | Descriptive, non-causal interpretation boundary | Post-implementation audit | `post-implementation-audit.md` |
