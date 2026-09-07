# T006 collector handoff checkpoint

**Date:** 2026-09-07

`jobs/yolo/submit.py collect` has a real, fail-closed handoff to the
authoritative result functions. A worker must atomically publish
`collection-input.json` under the prepared run. The handoff is bound to the
prepared `runId`, `case`, and candidate digest; normal cases carry the exact
request schedule, node receipt/preparation and GPU digests, Provider identities,
reference package/repository, certified graph, and graph/catalogue digests.
The negative case carries a separately validated
`tiger-yolo-expected-rejection-v1` record.

* finalizer 要求完整的 rank 集合，且每个返回值必须绑定 run/case，并且是
  `NODE_CLEANUP_COMPONENT_ONLY`；缺 rank 不会创建 collection input。
* handoff writer 重新读取每个实际 `node-receipt.json`，校验 run/case/rank/
  candidate/preparation digest，并把 receipt bytes digest 写入不可覆盖的
  `tiger-yolo-collection-input-v1`。
* node root 必须精确等于 resolved plan 的 `output/node<rank>`；reference
  package/repository 必须是无链接目录；GPU case 还必须提供 allocation 期望和
  已保留的 allocation/probe 文件。它不生成 ACK、Selection、模型输出或 verdict。
* 输出采用 exclusive create、fsync、原子 rename 和 `0444` 权限；重复写入、缺
  rank、路径逃逸和 GPU 证据缺失均拒绝。

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
does not create a verdict. The new `test_yolo_collection.py` adds 6 component
regressions for complete/incomplete ranks, immutable output, path binding and
GPU allocation gates. This remains component/worker-handoff evidence. No native
Provider, CUDA, MiniNDN, SIF, or TigerCluster receipt exists yet, so T006 and
T007 remain open.
