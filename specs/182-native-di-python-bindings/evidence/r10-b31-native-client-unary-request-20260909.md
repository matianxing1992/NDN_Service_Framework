# R10-B31 Native Client Unary Provider Request 2026-09-09

## Scope

本批补齐 T010-B 缺少的普通请求出口。现有 R4-B6 fixture 已覆盖
`NativeInferenceClient` 的 streaming/conversation 路径；本批在同一真实
Core/Provider transport 环境增加不带 generation、stream 或 conversation state 的 unary
请求。协议、Python facade、Provider worker 实现和 T016 资格范围没有改变。

## Implementation and review

- `runR4B6RealProviderConversationCase` accepts a bounded `unaryRequest` mode. The mode uses
  `TOKEN_DIAGNOSTIC`, leaves `NativeRequestOptions.stream`/`conversation` unset, and keeps the
  native catalog, grant owner, offer admission, preparation, split and placement objects.
- `NativeInferenceClient::request` reaches the real `ServiceUser::BeginCollaboration` path;
  the fixture's Provider callback receives the selected request and returns through
  `CollaborationContext::publishFinalResponse`. The client validates the terminal Response and
  decodes the payload through the native adapter.
- `Spec182R10B31RealProviderUnaryRequest` asserts a successful native handle and the exact
  `native-unary-response` payload. Existing R4-B6 conversation, replacement and repository
  reference selectors were rerun after the change.

The official `/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`) was applied as a
read-only review. The complete diff, changed call sites and test registration were checked
across production entry/callers, implementation/wire, test/harness/oracle, build/source
closure, and migration/evidence. No P1/P2/P3 finding was introduced.

## Verification

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin \
  CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf build --targets=integration-tests -j4
'build' finished successfully (35.584s)

.../integration-tests --run_test=Spec170NdnsfDiCoreFlow/Spec182R10B31RealProviderUnaryRequest
*** No errors detected (3.559s)

.../integration-tests --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation*
3 test cases; *** No errors detected (20.366s)

.../integration-tests --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderRepositoryReference
*** No errors detected (6.939s)

git diff --check
```

## Coverage and retrospective

| Lane | Result | Boundary |
| --- | --- | --- |
| Static | PASS | Changed C++ fixture branch, native client call path, provider callback and selector registration |
| Compile | PASS | `integration-tests` target built with system-first `-j4` |
| Focused runtime | PASS | Unary selector plus R4-B6 conversation/replacement/repository regressions |
| Integration | PARTIAL | Real in-process MiniNDN/Core/Provider transport; Provider callback is a deterministic fixture, not a deployed worker |
| Qualification | NOT RUN | Cross-process execution, maintained callers and T016 remain open |

The newly covered gap was the absence of an explicit non-streaming native client request. The
test demonstrates ACK, planning, commit, Provider callback and terminal Response ownership, but
does not prove the native Provider worker or no-Python deployment boundary. T010-B therefore stays
`PARTIAL`; the next qualification campaign must exercise this request shape through the
registered cross-process manifest and retain fresh raw evidence.
