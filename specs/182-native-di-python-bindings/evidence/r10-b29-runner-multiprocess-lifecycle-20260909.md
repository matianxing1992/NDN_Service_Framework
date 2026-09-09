# R10-B29 Runner Multi-Process Lifecycle 2026-09-09

## Scope

本批只修复 T014/T016 runner 的跨进程接线边界。manifest 已允许多个具名业务进程，
但旧实现只启动 `isolation.processes[0]`，并丢弃每个进程声明的 NDN 配置环境；这会
使 requester/provider case 在进入生产请求前就失真。本批没有修改 NDNSF-DI 协议、
Core、Provider、Python binding 或业务 oracle。

## Implementation and review

- `tests/standalone/run-spec182-native-closure.py` now validates native process roles, safe
  process IDs, argv/environment types and staged NDN config paths.
- `make_launch` accepts a declared process ID; `run_case` validates and holds each node
  namespace FD, launches every declared process, joins them to one supervisor process group,
  applies the per-process explicit environment, waits against one run deadline, and performs
  TERM→cleanup-budget→KILL escalation for the whole group.
- Per-process trace/stdout/stderr files are retained and deterministically merged into the
  canonical collector inputs. Raw per-process return codes, commands and node records remain
  in the run result.
- A harness-owned supervisor process creates the stable process group before any business
  process starts; this avoids a fast requester making the provider unable to join a vanished
  group.
- `collect_trace` reports declared role coverage only when every process sharing an executable
  has a corresponding successful exec; one requester exec cannot cover a missing provider.
- Tests add process selection/environment and duplicate-executable role coverage fixtures.

The official `/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`) was used as the
read-only review profile. The complete diff and call sites were checked across five lanes:
production entry (`run_case`/`make_launch`), state/lifetime (FDs, process group, deadline),
security/isolation (explicit env and staged paths), compatibility (single-process API/result
shape), and test/build registration. No introduced P1/P2/P3 finding remained after review.

## Verification

Commands and results:

```text
python3 -m pytest -q tests/python/test_spec182_native_closure.py
36 passed in 0.19s
python3 -m py_compile tests/standalone/run-spec182-native-closure.py
git diff --check
```

No native build was run: this batch changes only the Python isolation harness and its Python
unit fixtures. No MiniNDN owner run was claimed; real requester/provider transport, I02--I08,
maintained callers, no-Python evidence and T016 remain open.

## Coverage and retrospective

| Lane | Result | Boundary |
| --- | --- | --- |
| Static | PASS | Full changed diff, call sites, node/FD/process-group ownership and staged env checks reviewed |
| Compile | PASS | `py_compile` |
| Focused runtime | PASS | 36 collector/harness tests, including multi-process selection, stable supervisor lifecycle and role coverage |
| Integration | NOT RUN | Requires real MiniNDN node contexts and executable DI case |
| Qualification | NOT RUN | T016 remains the owner |

The prior implementation gap was a runtime/harness miss: multi-process fields were validated
but not executed. The new tests cover process selection and shared-executable role counting;
the next T016 run must still verify actual requester/provider communication, endpoint allowlists,
late children and cleanup under real namespaces. The batch exit is `CLOSED_FOR_VALIDATION`
for this harness boundary and `OPEN_FOR_NEXT_BATCH` for T014/T016; it is not a qualification
PASS.
