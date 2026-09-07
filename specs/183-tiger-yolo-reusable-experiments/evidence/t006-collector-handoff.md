# T006 collector handoff boundary

**Date:** 2026-09-07

`jobs/yolo/submit.py collect` now has a real, fail-closed handoff to the
authoritative result functions. A worker must atomically publish
`collection-input.json` under the prepared run. The handoff is bound to the
prepared `runId`, `case`, and candidate digest; normal cases carry the exact
request schedule, node receipt/preparation and GPU digests, Provider identities,
reference package/repository, certified graph, and graph/catalogue digests.
The negative case carries a separately validated
`tiger-yolo-expected-rejection-v1` record.

The command imports no oracle until the handoff schema and path/symlink checks
pass. Normal collection calls `collect_normal_verdict`; negative collection
calls `finalize_expected_rejection`. A successful result is written once to an
immutable `verdict.json` with `collectorSchema=tiger-yolo-collector-v1`.
Invalid evidence writes only the first immutable `collection-failure.json` and
cannot be promoted to PASS. Existing verdicts are accepted only when that
collector marker, run binding, candidate binding, and PASS status match.

Focused command-boundary evidence:

```text
python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_submit.py --tb=short
23 passed in 8.37s
```

The positive test exercises the real expected-rejection finalizer and immutable
verdict path; the negative test proves an invalid handoff retains a failure and
does not create a verdict. This remains component/worker-handoff evidence. No
native Provider, CUDA, MiniNDN, SIF, or TigerCluster receipt exists yet, so T006
and T007 remain open.
