# R10-B18 Runner Native ELF and Trace Integrity Evidence

**Date**: 2026-09-09  
**Spec**: 182-native-di-python-bindings  
**Batch**: R10-B18  
**Baseline**: `ec76b229` (R10-B17 owner context producer)  
**Status**: `CLOSED_FOR_VALIDATION` for native process/trace boundary; T016 remains `PARTIAL` / `UNQUALIFIED`

## Scope and outcome

本批修复 canonical closure runner 的两个实际执行边界。第一，最小环境清空 `PATH` 时，工具
路径必须由 case 明确提供绝对路径；第二，动态 ELF 的 interpreter/DT_NEEDED 使用绝对路径，
声明的 shared-library artifact 必须逐文件挂载到对应绝对目标，而不是只放在 `/probe-root`。
同时，collector 现在按 PID 配对 strace 的 `<unfinished ...>` 与 `<... resumed>`，只有悬挂或
孤立事件才判 `TRACE_UNPAIRED`。

本批没有挂载宿主完整 `/lib`，没有改变业务或协议代码。root probe 使用 `/bin/true` 加其
loader/libc，在 bubblewrap/strace 下返回 0，trace `complete=true`；它故意缺少业务 evidence，
因此保持 `UNQUALIFIED`，不作为 I/PO 或 no-Python qualification。

## Minimum Review Record

| Lane | Files/symbols inspected | Result |
| --- | --- | --- |
| production entry/callers | `run_case` → `make_launch` → `collect_trace` in `tests/standalone/run-spec182-native-closure.py` | covered; absolute tool paths remain case-controlled and the existing minimal env is preserved |
| implementation and wire | shared-library `--ro-bind` insertion; `TRACE_PID`/`TRACE_RESUMED` pairing; staged artifact checks | covered; only declared files are mounted at canonical targets, and pairing is per PID |
| test/harness/oracle | paired and unpaired trace fixtures, dynamic shared-library command construction, root dynamic-ELF probe | covered by 29 focused Python cases plus the root probe; no business oracle is generated |
| build/source closure | Python runner/test only; no C++ target or Waf registration | `BUILD_NOT_APPLICABLE`; `py_compile` and `git diff --check` are the source closure |
| migration/evidence | native-isolation filesystem/trace contract, R10-B17 owner context, this record and raw probes | open for native DI artifact closure, endpoint binding, owner-alive execution and T016 |

## Review trace

Read-only review followed `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, against the complete
R10-B18 diff from baseline `ec76b229`. The review inspected command ordering, absolute mount targets,
artifact identity, trace PID parsing, all `collect_trace` call sites and focused test registration.
The final review found **No findings**.

## Verification

```text
pytest -q tests/python/test_spec182_native_closure.py
29 passed in 0.17s

python3 -m py_compile tests/standalone/run-spec182-native-closure.py tests/python/test_spec182_native_closure.py
git diff --check
```

Root dynamic-ELF probe (fresh output `.codex-tmp/spec182-runner-probe4-20260909051727/`) returned
`exit=2` from the evaluator because required business evidence is intentionally absent; its raw
run recorded `run.returncode=0`, `observation.complete=true`, and no integrity violations. The
previous raw probes and first boundaries are indexed in [`docs/failure-log.md`](../../../docs/failure-log.md).

## Batch retrospective

- **Static review**: no actionable finding after checking tool paths, mount ordering, trace pairing,
  and all test/call sites.
- **Compile/link**: no native target changed; Python compilation and diff checks passed.
- **Runtime/test**: dynamic native process execution now reaches return code 0 with a complete
  observation. Missing evidence correctly keeps the probe `UNQUALIFIED`.
- **Unobserved migration lanes**: the runner has not yet staged a real DI requester/provider closure;
  absolute dependency closure for the full native binary, NFD endpoint binding, multi-process child
  cleanup and owner-alive invocation remain next-batch work.

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to declared dynamic-ELF mounts and strace continuation
integrity. An executable native DI case and owner-alive runner invocation remain
`OPEN_FOR_NEXT_BATCH`; T016 remains `UNQUALIFIED`.
