# Implementation Plan: Predictive UAV Multi-rate Validation

## Summary

Correct the UAV file-source wall-clock pacing defect at the GStreamer sink,
retain the existing predictive Core/API path, then run one immutable six-rate
MiniNDN matrix. The comparison varies only `--video-fps`.

## Source-backed Design

The current capture graph is:

```text
filesrc -> decodebin -> videorate -> fps caps -> x264enc -> appsink(sync=false)
```

For a non-live source, FPS caps assign PTS but `sync=false` lets the sink consume
buffers as quickly as CPU permits. Use clock-synchronized capture consumption:

```text
filesrc -> decodebin -> videorate -> fps caps -> x264enc -> appsink(sync=true)
```

The callback still receives one encoded access unit with its exact source
identity. Decode `appsink` behavior is not changed because consumer decode
should process arriving network Data immediately rather than introduce a
second presentation clock.

## Formal Matrix

| Cell | FPS | Loss | Reorder | Measured window |
|---|---:|---:|---:|---:|
| fps-10 | 10 | 0% | 0% | >=60 s |
| fps-20 | 20 | 0% | 0% | >=60 s |
| fps-30 | 30 | 0% | 0% | >=60 s |
| fps-40 | 40 | 0% | 0% | >=60 s |
| fps-50 | 50 | 0% | 0% | >=60 s |
| fps-60 | 60 | 0% | 0% | >=60 s |

Fixed parameters: two-node `AI_Lab.conf`, Memphis Controller/Ground Station,
UCLA Drone, GStreamer file source, 8 Mbps, width 480, one FEC repair item,
predictive stream API, zero loss/reorder, 5-second warm-up, 80-second app run.

Achieved FPS is counted from exact provider
`encoded-output-ready ... /NDNSF/UAV/VIDEO/FRAME/...` events inside the same
shared-clock measurement interval used for consumer evidence. Segment
publication rate is reported separately and MUST NOT be mislabeled FPS.

## Validation Order

1. freeze frozen-result digests and inspect single-writer ownership;
2. pre-implementation audit;
3. focused GStreamer pacing/callback/decode tests;
4. rebuild affected targets with `-j2`;
5. short non-formal MiniNDN low/high-rate diagnostics in unique roots;
6. full focused regression, linkage, and source scans;
7. freeze the independent runner/analyzer and execute six formal cells once;
8. aggregate results and perform post-implementation audit.

## Ownership and Safety

- UAV pacing change: `NDNSF-UAV-APP/shared/UavVideoPipeline.cpp`.
- Experiment-only additions: Spec 152 runner, analyzer, and tests.
- No Core, binding, prefetch, retry, FEC, Mapping, or protocol change.
- Dirty files outside these exact hunks are user-owned and remain untouched.
- Specs/results 148–151 are read-only.

## Constitution Check

- Canonical dynamic runtime: PASS; only current predictive stream APIs are used.
- Security path: PASS; names, signing, encryption, and authorization are unchanged.
- CodeGraph first: PASS; FPS propagation and both API owners were traced.
- Spec-driven durable work: PASS; Spec 152 owns the repair and matrix.
- Right validation scope: PASS; final evidence is two-node MiniNDN with >=60 s.
- Cohesive tasks: PASS; each task closes one independently reviewable outcome.
- GSD resumability: PASS; health is valid and immutable checkpoints are explicit.
- ARS experiment design: PASS; matched factors, metrics, and failure rules are frozen.

## Gates

The pre-implementation audit blocks work if the fix would require a Core
workload branch, historical rerun, mutable formal command, or selective cell
replacement. Formal failure remains negative evidence and moves further repair
to a successor Spec.
