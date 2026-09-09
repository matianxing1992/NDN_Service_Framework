# R10-B39 R4-B6 Fixture Contract Guard

## Scope

R10-B37 提交后的只读复核发现，R4-B6 handler 的 `failFirst` replacement 注入先于
conversation binding 校验；若测试投影缺少 `conversationTurnBinding`，首轮负例可能直接
进入预期 ProviderFailure，掩盖夹具契约破坏。本批只调整测试夹具顺序：conversation 请求
先验证 binding，stream-only 请求继续明确不要求 binding；生产请求状态机、wire 和 Provider
实现不变。

## Changed gate and implementation

- `conversationRequest && !projection.conversationTurnBinding` 现在在 `failFirst` 前拒绝。
- stream-only 分支在该 guard 后仍发送 generation token/final，且不创建 conversation state。
- 现有 positive conversation、single-provider negative、alternate replacement、unary 和
  repository selectors 均保留并重跑。

这是针对 R10-B37 post-commit review miss 的 `Changed gate`；没有观察到生产运行时失败。

官方只读 `/home/tianxing/.codex/skills/review-agent/SKILL.md`（SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`）已复核完整 diff、
helper 调用方、Provider callback、selector registration 和 `integration-tests` source
closure；修复后无 P1/P2/P3 actionable finding。

## Verification

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=integration-tests -j4 -v
'build' finished successfully (38.832s); 118/118 compile/link steps; exit 0

build-nac182/integration-tests \
  --run_test=Spec170NdnsfDiCoreFlow/Spec182R10B37RealProviderNativeStreamRequest \
  --report_level=no --log_level=nothing
exit 0

for test in \
  Spec182R4B6RealProviderConversation \
  Spec182R4B6RealProviderConversationReplacement \
  Spec182R4B6RealProviderConversationAlternateReplacement \
  Spec182R10B31RealProviderUnaryRequest \
  Spec182R10B33RealProviderUnaryRepositoryReferenceRequest; do
  build-nac182/integration-tests \
    --run_test=Spec170NdnsfDiCoreFlow/$test --report_level=no --log_level=nothing
done
# all five selectors exit 0

python3 specs/182-native-di-python-bindings/checklists/validate_design.py
git diff --check
```

`vmstat 1` 后续样本没有 `so`，但 `si` 在构建初段出现 `120, 40, 40` 后降至个位数；
这次构建成功，下一次 native build 按资源规则采用 `-j2`，不把该单次测量写成普遍速度结论。

## Minimum Review Record

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | R4-B6 `ServiceProvider` callback; `NativeInferenceClient::request`; all helper callers | CodeGraph/source diff and selector scan | existing request routes unchanged; only fixture guard order changed |
| `implementation and wire` | `covered` | `nativeSelectionProjectionV3FromJson`; conversation binding guard; `failStream` branch | exact changed lines and generation/conversation branch review | conversation contract cannot be bypassed by injected failure; stream-only remains no-binding |
| `test/harness/oracle` | `covered` | new stream-only plus conversation/replacement/unary/repository selectors | focused selector rerun | all selected cases exit 0; no oracle weakened |
| `build/source closure` | `covered` | `tests/integration-tests/ndnsf-di-core-flow.t.cpp`; `tests/wscript`; `integration-tests` | source registration scan; system-first `-j4` build | 118/118 steps pass; next build should use `-j2` due nonzero `si` samples |
| `migration/evidence` | `covered` | R10-B37/R10-B39 plan/tasks/evidence and validator | `validate_design.py`; `git diff --check` | changed gate and post-commit miss are recorded; product parents remain partial |

## Batch result

- **Review trace**: the post-commit finding and changed gate are recorded above; repaired diff has
  no further actionable finding.
- **Closure decision**: `CLOSED_FOR_VALIDATION` for the fixture contract guard; `OPEN_FOR_NEXT_BATCH`
  for Provider worker/cross-process and maintained caller qualification.
- **Static findings**: initial ordering miss fixed; no remaining P1/P2/P3 finding.
- **Compile/build misses**: none; existing aggregate initialization warnings remain. Resource
  measurement observed nonzero `si`, so future native builds step down to `-j2`.
- **Runtime/test misses**: stream-only, conversation, replacement and unary/repository selectors
  all exit 0; cross-process worker, maintained caller/no-Python and T016 remain unrun.
- **Build measurement**: system-first `-j4`, 38.832 seconds, 118/118 steps, no `so`; not a general
  speedup claim.
- **Behavior result**: `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; bounded fixture guard
  closed; not `QUALIFICATION_PASS`.
- **Evidence / remaining**: this record plus R10-B37 evidence; next batch must use the first stable
  Provider worker/cross-process exit and keep the corrected guard in regression coverage.

### Batch growth decision

The batch had one stable exit: the conversation fixture cannot bypass binding validation when
injecting a Provider failure. No production caller, process boundary, or protocol contract was
added. Any such change starts a new Batch ID.

### Batch Retrospective

- `static`: found and repaired binding-before-failFirst ordering; this is the recorded changed gate.
- `compile/link`: 118/118 integration target steps pass; warnings are pre-existing.
- `runtime/test`: six focused selectors pass after the repair.
- `unobserved`: Provider worker/cross-process transport, maintained caller/no-Python, cancellation
  under deployed transport, and T016 qualification remain open.
