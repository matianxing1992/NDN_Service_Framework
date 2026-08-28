# T015 provider timing evidence — 2026-08-27

## Subject

One current-source host MiniNDN M11 run (`seed=1750001`) was executed after
the Spec175 launcher began enabling `NDNSF_DI_RUNTIME_TIMING=1` for the
registered cases. The run used the checked-in tiny causal ONNX fixture, four
physical Provider processes, V3 automatic planning, two fresh conversation
Requests, and no admission-control path.

The runner now parses the Provider handler markers into a bounded,
metadata-only report. It requires a matched start/end pair for every observed
epoch and all four logical roles; an unmatched marker, missing role, or
negative duration aborts result publication. The report contains no prompt,
response, token, logits, or state payload.

## Result

```text
runRoot=/tmp/spec175-m11-timing-Y6TybR
case=M11
seed=1750001
status=PASS
providerTimingSchema=ndnsf-di-spec175-provider-timing-v1
spanCount=8
observedRoles=/LLM/Pipeline/Stage/0,/LLM/Pipeline/Stage/1,/LLM/Pipeline/Stage/2,/LLM/Pipeline/Stage/3
```

Each role produced two matched handler spans. The measured handler-duration
summaries were:

| Role | Spans | Mean/p50 (ms) | p95 (ms) |
|---|---:|---:|---:|
| Stage/0 | 2 | 4908.185 | 5262.106 |
| Stage/1 | 2 | 4908.855 | 5264.333 |
| Stage/2 | 2 | 4908.965 | 5267.341 |
| Stage/3 | 2 | 4907.205 | 5267.741 |

The long handler duration is an observed tiny-ONNX/MiniNDN process value, not
a performance claim for Qwen3.6 or CUDA. It includes the Provider handler's
request/dependency/runner path and must be replaced by the final G5/G6
measurements for performance conclusions.

## Hashes

```text
spec175-provider-timing.json
sha256:36cde8df31ba0b71bd4eb87a0c6af4b7bf91f79011ac1f0ec34c631a1150ea3a

spec175-case-result.json
sha256:16584c8df77c23f9ef5eb1efdb76542addc6ddc1baaa95422eaab3f13f54ba3d

llm-pipeline-user.log
sha256:59d271510890647197bda4c3766efe3e1a53689a3b5267f2075d2da3d0a3acfa
```

The timing report was re-parsed with the final strict numeric/dependency
marker parser from the same four Provider logs; the re-parsed report has the
hash shown above. The current tiny fixture emitted no dependency timing
markers, so its dependency span counts are explicitly zero rather than
invented.

This is one current-source execution checkpoint. It does not close the
repeated T015/T020/T022 qualification gates, exact-SIF replay, CUDA readiness,
or Tiger execution.
