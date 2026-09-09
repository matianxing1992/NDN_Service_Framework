# R10-B37 Native Client Streaming Provider Request

## Scope

本批完成 P1 的一个独立真实结果出口：现有 R4-B6 in-process Core/Provider fixture 增加
无 conversation state 的 native streaming request。请求仍由 `NativeInferenceClient::request`
完成 preparation、grant/admission、Core commit 和 `ServiceUser::BeginCollaboration`；真实
`ServiceProvider` callback 发布 `GenerationTokenEventV1` 与 `NDNSF-DI-FINAL-V1`，requester
通过原生 generation acceptance/final validation 返回结果。该批不改变 Provider worker、
跨进程 transport、maintained caller/no-Python 或 T016 资格边界。

## Implementation and review

- `runR4B6RealProviderConversationCase` 新增 `conversationRequest` 选项。stream-only 模式
  保留 `options.stream` 与 `options.generation`，明确不设置 `options.conversation`。
- Provider fixture 在无 conversation 时仍解析 authenticated selection projection，并用
  projection 内的 generation identity 构造两个 token event 和一个 terminal final；不会构造
  或猜测 `conversationTurnBinding`、receipt、control 或 commit。
- `Spec182R10B37RealProviderNativeStreamRequest` 断言原生结果为 `{"text":"ab"}`，并断言
  coordinator 没有创建 conversation record；已有 unary、repository、conversation 和
  replacement selectors 作为同 fixture 回归保持通过。

官方只读 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）已应用。审查基线为
`8b396a08`，完整 diff 覆盖 helper 分支、真实 Provider callback、generation stream/final
wire、selector 注册和 Waf source closure；五个 lane 均无 P1/P2/P3 actionable finding。

## Verification

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=integration-tests -j4 -v
'build' finished successfully (35.854s); 118/118 compile/link steps; exit 0

build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R10B37RealProviderNativeStreamRequest \
  --report_level=detailed --log_level=nothing
1 test case; 4 assertions; exit 0

build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R4B6RealProvider* \
  --report_level=no --log_level=nothing
4 R4-B6 selectors; exit 0

build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R10B31RealProviderUnaryRequest \
  --report_level=no --log_level=nothing
exit 0

build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R10B33RealProviderUnaryRepositoryReferenceRequest \
  --report_level=no --log_level=nothing
exit 0

git diff --check
```

`vmstat 1` 的首行之后持续样本 `si=0`、`so=0`，未观察到持续 swap-in/out。首次尝试把多个
Boost.Test filter 用逗号拼接，测试进程返回“no test cases matching filter”；这是命令选择器
边界，未计为产品失败。随后按独立 selector 重跑，全部退出 0。

## Minimum Review Record

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | `NativeInferenceClient::request`; `runR4B6RealProviderConversationCase`; registered `ServiceProvider` callback | CodeGraph flow query; changed helper and selector diff | stream-only options reach the same native Core/Provider path; no caller fallback |
| `implementation and wire` | `covered` | `NativeInferenceClient::beginCoreRequest`; `acceptGenerationEvent`; `validateGenerationFinal`; Provider callback branch | CodeGraph symbols; exact generation/final fields in diff | generation identity comes from projection; no conversation binding is fabricated |
| `test/harness/oracle` | `covered` | `Spec182R10B37RealProviderNativeStreamRequest` and R4-B6/B31/B33 selectors | selector registration scan and focused runs | token/final validation and no conversation record are asserted; related regressions exit 0 |
| `build/source closure` | `covered` | `tests/integration-tests/ndnsf-di-core-flow.t.cpp`; `tests/wscript`; `integration-tests` | source registration scan; system-first `-j4` build | 118/118 compile/link steps and target closure pass |
| `migration/evidence` | `covered` | `plan.md`, `tasks.md`, this record, prior R4-B6/R10-B31/B33 evidence | link/status scan; `validate_design.py`; `git diff --check` | P1 boundary recorded; worker/cross-process, maintained caller/no-Python and T016 remain open |

## Batch result

- **Review trace**: official `review-agent` path/SHA, baseline and full diff scope are recorded
  above; no actionable finding remained after the source and selector checks.
- **Closure decision**: `CLOSED_FOR_VALIDATION` for this single-process stream-only P1 boundary;
  `OPEN_FOR_NEXT_BATCH` for Provider worker/cross-process and caller migration. No parent task is
  promoted to qualification.
- **Static findings**: none. The stream-only branch preserves existing fail-closed generation
  validation and does not introduce conversation state.
- **Compile/build misses**: no source or link miss; the compiler emitted existing aggregate
  initialization warnings in the large fixture. The initial comma-filter command was a test
  selection error and was corrected before the final evidence runs.
- **Runtime/test misses**: no focused runtime miss. Provider worker, cross-process transport,
  maintained caller/no-Python and T016 remain unrun by design.
- **Build measurement**: system-first `-j4`, 35.854 seconds, 118/118 steps, no sustained swap;
  this is a batch measurement and does not claim a general speedup.
- **Behavior result**: `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION`
  for the bounded in-process stream-only result; not `QUALIFICATION_PASS`.
- **Evidence / remaining**: this record plus the selector output above; next P1/P2 batch must
  exercise Provider worker/cross-process lifecycle and preserve first failure boundaries.

### Batch growth decision

The batch had one stable exit: a native requester without conversation receives and validates a
real streamed Provider result. No worker process, maintained caller, replacement, or NFD dependency
was added. Any such addition starts a new Batch ID.

### Batch Retrospective

- `static`: five-lane review caught no new defect; generation identity and no-conversation ownership
  were checked explicitly.
- `compile/link`: source closure rebuilt successfully; only existing aggregate initialization
  warnings remained.
- `runtime/test`: new stream-only selector and four R4-B6 plus two unary selectors passed; the
  first comma-filter invocation was corrected and rerun independently.
- `unobserved`: Provider worker/cross-process transport, maintained caller/no-Python, cancellation
  under deployed transport, and T016 MiniNDN/NFD qualification remain open.
