# R10-B17 MiniNDN Owner Context Evidence

**Date**: 2026-09-09  
**Spec**: 182-native-di-python-bindings  
**Batch**: R10-B17  
**Baseline**: `6af23fae` (R10-B16 runner boundary)  
**Status**: `CLOSED_FOR_VALIDATION` for owner context production; T016 remains `PARTIAL` / `UNQUALIFIED`

## Scope and outcome

本批把 MiniNDN owner 从“只写 `MININDN_NODE_CONTEXT_NOT_PROVIDED`”推进到可执行的 context
生产边界。显式 `--execute-owner` 使用 tracked topology
`Experiments/Topology/spec182-native-closure.conf` 创建 requester/provider 两个 namespace，
通过 `AppManager` 启动每个节点的 NFD，等待实际 filesystem socket，再导出 namespace inode、
owner PID/start ticks、NFD socket 和双向 peer IDs。context 只来自已创建节点的 `/proc` 和实际
socket；host namespace 节点、重复/未知 peer 或缺失 socket 会拒绝。

owner 仍不复制 closure collector，也不生成业务 oracle。当前冻结 manifest 只有 qualification
registration，没有 runner 所需的 `cases`/artifact/process 描述，因此 owner 在写出 context
后明确返回 `UNQUALIFIED / NATIVE_CLOSURE_CASE_DEFINITION_MISSING`，没有声称任何 I/PO case
通过。

## Minimum Review Record

| Lane | Files/symbols inspected | Result |
| --- | --- | --- |
| production entry/callers | `main --execute-owner` → `_run_owned_campaign` → `collect_node_context`; existing `run_campaign` registration-only path | covered; explicit owner mode is opt-in and default preflight behavior remains unchanged |
| implementation and wire | `Experiments/NDNSF_DI_NativeClosure_Minindn.py`, `Experiments/Topology/spec182-native-closure.conf`, qualification `topology` field | covered; MiniNDN argv is isolated, fresh work dir is used, NFD socket readiness precedes context export, and cleanup runs in `finally` |
| test/harness/oracle | `tests/python/test_spec182_native_closure.py` context export, host namespace and outside-topology peer negatives; registration regression cases | covered by 26 focused cases; privileged owner run is recorded separately and no business oracle is synthesized |
| build/source closure | Python owner, manifest and topology only; no C++ target or Waf registration changed | `BUILD_NOT_APPLICABLE`; `py_compile` and `git diff --check` are the source closure |
| migration/evidence | `native-isolation-design.md` node contract, `tasks.md` R10-B17, this record, raw owner run | context producer is closed locally; runner case definitions, owner-alive runner invocation, endpoint binding, multi-process lifecycle, no-Python and T016 remain open |

## Review trace

Read-only review followed `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, against the complete
R10-B17 diff from baseline `6af23fae`. The review inspected owner lifecycle, MiniNDN constructor
argv handling, NFD application/socket readiness, `/proc` identity extraction, topology/peer binding,
cleanup, manifest registration and all focused tests. The final review found **No findings**.

## Verification

Local checks:

```text
pytest -q tests/python/test_spec182_native_closure.py
26 passed in 0.19s

python3 -m py_compile Experiments/NDNSF_DI_NativeClosure_Minindn.py tests/python/test_spec182_native_closure.py
git diff --check
python3 specs/182-native-di-python-bindings/checklists/validate_design.py
```

The validator returned `ok: true`, with 40 execution cards (`DONE=16`, `PARTIAL=23`,
`NOT_STARTED=1`) and `runtime_tests=NOT_RUN`, `product_static_review=NOT_RUN`.

Explicit owner run (root, fresh output and fresh manifest copy with `campaignCase=I01`):

```text
sudo -n env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-t016-r10-b17-20260909050945-manifest.json \
  --output .codex-tmp/spec182-t016-r10-b17-20260909050945 --execute-owner
exit=2
result.status=UNQUALIFIED
result.reason=NATIVE_CLOSURE_CASE_DEFINITION_MISSING
```

The raw output contains `node-context.json` with both nodes, `/run/nfd/requester.sock` and
`/run/nfd/provider.sock`, distinct namespace inodes, owner start ticks and reciprocal peers:
`.codex-tmp/spec182-t016-r10-b17-20260909050945/`. The owner stopped the network in `finally`;
the context is an identity record from the live run, not a reusable claim after those PIDs exit.

The preceding malformed invocation (missing `campaignCase`) is preserved in
`.codex-tmp/spec182-t016-r10-b17-20260909050906.stdout`,
`.codex-tmp/spec182-t016-r10-b17-20260909050906.stderr`, and its result directory; it is indexed
in [`docs/failure-log.md`](../../../docs/failure-log.md) as an operator preflight boundary.

## Batch retrospective

- **Static review**: no actionable finding remained; the review covered owner lifecycle, source
  registration, namespace identity and test/source closure.
- **Compile/link**: no native target changed; Python compilation and document validation passed.
- **Runtime/test**: 26 focused cases passed. The explicit root run created real namespaces and NFD
  sockets, then stopped before any business process because no executable closure case exists.
- **Unobserved migration lanes**: runner invocation while the owner is alive, artifact/process
  staging, endpoint/socket binding inside the closure, multi-process cleanup, and all DI I/PO
  outcomes remain unobserved. The raw context proves only the owner boundary.

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to MiniNDN context production and its fail-closed missing-case
exit. Runner case definitions and a live owner→runner call are `OPEN_FOR_NEXT_BATCH`; T016 remains
`UNQUALIFIED`.
