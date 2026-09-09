# R10-B16 Native Closure Node Context Evidence

**Date**: 2026-09-09  
**Spec**: 182-native-di-python-bindings  
**Batch**: R10-B16  
**Baseline**: `e0b46007` (R10-B16 allocation and preflight boundary)  
**Status**: `CLOSED_FOR_VALIDATION` for the runner boundary; T016 remains `PARTIAL` / `UNQUALIFIED`

## Scope and outcome

本批只实现 canonical closure runner 的 MiniNDN node-context 边界。`run_case` 现在要求
声明 node 的 case 提供与 process ID 绑定的实际 namespace 路径/inode、owner PID/start ticks、
NFD filesystem socket 和唯一 peer IDs；校验通过后打开并持有 namespace FD，以
`nsenter --net=/proc/self/fd/N` 启动既有 `strace`/bubblewrap 命令。缺失、过期或不匹配的
context 在任何业务进程启动前抛出 `PreflightError`。未声明 node 的 local case 不会因为传入
偶然的 `_namespaceFdPath` 而进入其他 namespace。

本批没有创建 MiniNDN topology、没有补 DI qualification cases、没有改变 NFD 配置，也没有
把宿主 namespace 当作节点资格。owner 仍需在后续批次产生真实 node context，并为 I01--I08
和 PO-001--PO-014 提供业务请求、进程树、endpoint 和 cleanup 证据。

## Minimum Review Record

| Lane | Files/symbols inspected | Result |
| --- | --- | --- |
| production entry/callers | `run_case` → `make_launch` in `tests/standalone/run-spec182-native-closure.py`; CLI `main` remains the caller boundary | covered; CLI still has no owner context source and therefore cannot qualify T016 |
| implementation and wire | `_read_proc_start_ticks`, `_validate_node_context`, `os.open`/`fstat`, `pass_fds`, `nsenter --net=/proc/self/fd/N`, explicit FD cleanup | covered; node identity is checked before launch and the held FD is closed on launch construction, timeout, and normal completion paths |
| test/harness/oracle | `tests/python/test_spec182_native_closure.py`; missing context, valid binding, stale starttime, unheld path, and incidental namespace cases | covered by 23 focused cases; tests exercise command construction and preflight, not a privileged MiniNDN run |
| build/source closure | Python runner and test module only; no C++ target or Waf registration is changed | `BUILD_NOT_APPLICABLE`; `py_compile` and diff checks are the source closure for this batch |
| migration/evidence | `native-isolation-design.md` node contract, `tasks.md` R10-B16 row, this record, R10-B15 owner failure | open for owner topology, multi-process lifecycle, endpoint binding, and no-Python qualification |

## Review trace

Read-only review followed `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, against the complete
R10-B16 diff from baseline `e0b46007`. The review inspected both changed Python files, all
`run_case`/`make_launch` call sites, the isolation contract, and the focused test registration.
The first-pass resource concern (FD opened before launch construction) was repaired before this
record: launch construction now closes the held FD on every exception path. The final read-only
review found **No findings**.

## Verification

Commands run from the repository root:

```text
pytest -q tests/python/test_spec182_native_closure.py
23 passed in 0.14s

python3 -m py_compile tests/standalone/run-spec182-native-closure.py tests/python/test_spec182_native_closure.py
git diff --check
```

No native build was run because this batch changes only the standalone Python runner and its
Python tests. Existing R10-B14 full C++ unit/integration evidence remains the source-compatible
baseline; it does not prove namespace or MiniNDN qualification.

## Batch retrospective

- **Static review**: no actionable finding remained after the FD-lifetime and incidental-node
  checks; the review explicitly covered production entry, implementation/wire, tests, and source
  closure.
- **Compile/link**: no native target changed; Python bytecode compilation passed.
- **Runtime/test**: all 23 focused cases passed. A privileged `nsenter`/MiniNDN execution was not
  attempted, so actual owner namespace reachability and socket connectivity remain unobserved.
- **Unobserved migration lanes**: the owner stub still exits with
  `MININDN_NODE_CONTEXT_NOT_PROVIDED`; the manifest has no real topology/case definitions; the
  runner currently handles the first declared process only. These are explicit next-batch gaps,
  not PASS claims.

## Closure decision

`CLOSED_FOR_VALIDATION` applies only to the runner's node-context preflight and held-FD launch
boundary. The production owner integration, real MiniNDN cases, cross-process maintained callers,
no-Python execution, and T016 qualification remain `OPEN_FOR_NEXT_BATCH` / `UNQUALIFIED`.
