# Implementation Plan: UAV Stage Latency Attribution

**Date**: 2026-07-26  
**Spec**: `specs/157-uav-stage-latency-attribution/spec.md`

## Summary

Add one read-only Python analyzer and deterministic fixtures. Reuse the timeline
events already emitted by the frozen Spec 156 binaries:

```text
capture_origin_ns
  -> encoded-output-ready(frame)
  -> signed-and-materialized(max source cursor for frame)
  -> decoder-input(source_frame_id)
  -> decoder-output(source_id)
```

No runtime instrumentation is necessary: current logs already contain all five
boundaries. The analyzer reads the immutable campaign and writes a separate
diagnostic result directory.

## Technical Context

**Language**: Python 3  
**Dependencies**: standard library only  
**Testing**: standard-library unittest fixtures plus read-only six-cell replay  
**Input**: frozen Spec 156 `drone.log`, `ground-station.log`, and
`cell-summary.json`  
**Output**: per-frame CSV, per-cell JSON, aggregate CSV/JSON/Markdown  
**Constraints**: no MiniNDN execution; no writes within the input root

## Research Design

- Research question: which of the four consecutive intervals accounts for the
  observed end-to-end latency distribution at each configured FPS?
- Dependent variables: interval duration and capture-to-output duration.
- Control: identical immutable campaign artifacts; no treatment or rerun.
- Admissibility: exact frame identity, complete five-boundary join, output
  inside the original measurement window, monotonic timestamps.
- Statistics: count, mean, p50, p95, p99 per interval. Percentiles are computed
  on per-frame deltas and are never added across stages.
- Limitation: descriptive attribution only; no new causal performance claim.

## Constitution Check

PASS. This is a diagnostic-only successor to a frozen network campaign. It
changes no runtime, security, wire protocol, API, or experiment configuration.
One cohesive implementation task owns parser, fixtures, and offline report.

## Project Structure

```text
Experiments/analyze_uav_video_stage_timeline.py
tests/python/test_spec157_uav_stage_latency_attribution.py
specs/157-uav-stage-latency-attribution/
└── contracts/diagnostic-analysis.md
```

## Rollback

Remove the analyzer, its tests, and the separate diagnostic output. The frozen
Spec 156 sources and artifacts remain unchanged throughout.
