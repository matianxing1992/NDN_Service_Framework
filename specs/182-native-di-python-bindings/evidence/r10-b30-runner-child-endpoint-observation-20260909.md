# R10-B30 Runner Child and Endpoint Observation 2026-09-09

## Scope

本批承接 R10-B29 的多进程生命周期，只补齐 T014 的 manifest/collector 语义：具名
`assembly-worker` child 必须绑定已声明 Provider，endpoint 必须绑定 owner/peer、地址和
用途，trace collector 记录 clone/open/close/mmap/socket/connect 等指定系统调用，并只
把 manifest 声明的 endpoint 视为允许连接。没有修改 NDNSF-DI 协议、Core、Provider、
Python binding 或业务 oracle。

## Implementation and review

- `load_case` now validates child role/executable/Provider parent/concurrency and endpoint
  owner/peer/transport/address/purpose. Abstract UNIX endpoints and malformed addresses fail
  closed before launch.
- `collect_trace` records the frozen lifecycle syscall set and matches successful `connect`
  lines against declared endpoint addresses. Undeclared filesystem, TCP or abstract endpoint
  attempts remain policy violations.
- Tests add valid/invalid child and endpoint declarations plus a lifecycle-syscall trace with a
  declared UNIX endpoint.

The official `/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`) was used for the
read-only review. The complete diff and callers were checked across production entry/callers,
implementation/wire, test/harness/oracle, build/source closure, and migration/evidence. The
review found no introduced P1/P2/P3 finding.

## Verification

```text
python3 -m pytest -q tests/python/test_spec182_native_closure.py
38 passed in 0.16s
python3 -m py_compile tests/standalone/run-spec182-native-closure.py
python3 specs/182-native-di-python-bindings/checklists/validate_design.py
git diff --check
```

No native build, MiniNDN run or qualification claim was made. Real child-process injection,
endpoint allowlists, I02--I08, maintained callers, no-Python execution and T016 remain open.

## Coverage and retrospective

| Lane | Result | Boundary |
| --- | --- | --- |
| Static | PASS | Changed manifest/collector code, callers, child ownership, endpoint policy and tests |
| Compile | PASS | `py_compile`; design validator `ok=true` |
| Focused runtime | PASS | 38 Python harness/collector tests |
| Integration | NOT RUN | Needs real MiniNDN contexts and executable DI cases |
| Qualification | NOT RUN | T016 remains owner |

The newly covered gap was a manifest/collector semantics miss: child and endpoint fields existed
in the frozen contract but were not validated or represented in syscall observations. The next
T016 run must exercise real worker parentage, late-child cleanup and endpoint policy in fresh raw
runs. This batch is `CLOSED_FOR_VALIDATION` for the local harness semantics and
`OPEN_FOR_NEXT_BATCH` for T014/T016; it is not `QUALIFICATION_PASS`.
