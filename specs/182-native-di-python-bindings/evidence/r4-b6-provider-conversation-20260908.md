# R4-B6 Real Provider Conversation

## Status and stable exit

PARTIAL / T011-C CC-4。该批已经闭合一个真实可观察出口：公开
`NativeInferenceClient` 发起 FULL_CONTEXT 首轮，由同一测试环境中的真实 `ServiceProvider`
接收并发布 authenticated receipt，Requester 完成 COMMIT/commit ACK 后，以同一
`NativeConversationCoordinator` 发起 APPEND_DELTA 二轮并解析新的 Provider 状态引用。
预置 `VerifiedCollaborationData` 没有用于这条出口。正向两轮已通过；恢复或 replacement
负例仍未在本批真实 Provider harness 中运行，因此 T011-C 和 T016 继续保持未关闭。

本批还修复并验证了结构化 request ID 的 V2、event/collaboration name 编码，SVS session/seq
freshness，Provider 单 worker 控制面等待死锁，以及 End 事件后的 stream gap 重试误判。
这些修复属于生产接线的一部分，不能由单独的 helper 测试替代。

## Members and ownership

| Member | Scope | Owner | Planned selector |
| --- | --- | --- | --- |
| CC-4a | `NdnsfIntegrationEnvironment` 中真实 Provider 与公开 NativeInferenceClient 首轮接线 | implemented and focused-tested | `Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation` |
| CC-4b | 同一 coordinator 的 `APPEND_DELTA` 与 receipt/control/commit ACK lineage | implemented and focused-tested | `Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation` |
| CC-4c | recovery 或 single replacement 负例，记录首个失败边界并保持父 checkpoint | remaining | no dedicated real-Provider selector yet |

## Coverage matrix

| Lane | Status | Required scope |
| --- | --- | --- |
| production entry/callers | covered | `NativeInferenceClient::request`, `NativeConversationCoordinator`, `NdnsfIntegrationEnvironment`, and the registered `ServiceProvider` collaboration handler are exercised by `runR4B6RealProviderConversationCase`; query: `rg -n 'runR4B6RealProviderConversationCase|NativeInferenceClient client|addCollaborationHandler' tests/integration-tests/ndnsf-di-core-flow.t.cpp` |
| implementation and wire | covered | V2 request/response/ACK/Selection parsers, event/collaboration names, SVS session/seq freshness, receipt/control/commit ACK and stream terminal handling; query: `git diff -- ndn-service-framework/ServiceProvider.cpp ndn-service-framework/utils.cpp ndn-service-framework/InvocationStream.cpp` |
| test/harness/oracle | covered for positive path; gap for negative | real encrypted catalog publication, real receipt/control/commit, second-turn lineage and result text are asserted in `Spec182R4B6RealProviderConversation`; CC-4c recovery/replacement oracle remains absent |
| build/source closure | covered | system-first `./waf -o build-nac182 build --targets=integration-tests -j4` and combined unit/integration target both succeeded; integration source is registered by the existing `tests/wscript` closure; focused unit selectors cover V2, collaboration and event names |
| migration/evidence | gap | this evidence and `.codex-tmp/spec182-r4-b6-real-provider-final-20260908/` record the local run; T012/T013 caller migration, T014 isolation, T015 convergence and T016 qualification remain open |

## Gate order

Each implementation member was followed by the read-only official `review-agent` gate and the
matrix was updated before the batch build. Final batch commands and results were:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=integration-tests -j4
  result: success, 16.549s (elapsed 16.65s) after the legacy event-wire compatibility repair
./build-nac182/integration-tests \
  --run_test='Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation' \
  --log_level=test_suite
  result: success, 6.788s, *** No errors detected
env PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf \
  ./waf -o build-nac182 build --targets=unit-tests,integration-tests -j4
  result: success, 0.913s (elapsed 1.01s)
./build-nac182/unit-tests --run_test='GenericDynamicApi/PreparedAndMessages/V2RequestAndResponseNames'
./build-nac182/unit-tests --run_test='GenericDynamicApi/PreparedAndMessages/CollaborationNamePreservesStructuredRequestId'
./build-nac182/unit-tests --run_test='Spec175InvocationStreamMessage/StructuredRequestIdRemainsBoundInEventName'
  result: 3/3 selectors passed
```

`git diff --check` and `validate_design.py` also pass. The batch remains `PARTIAL`: both positive
turns use real Provider receipt/control/commit, while CC-4c recovery/replacement and all T016
qualification gates are still open. A local seeded receipt or a successful link is not an
acceptance result.

## Related replacement boundary

The existing native Provider replacement fixture was rerun against the refreshed candidate
integration binary:

```text
integration-tests --run_test='Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI12ProviderUnavailableAfterEvent3WithReplacement'
result: 1 case, 19 assertions, exit 0; requests=2 completed=1 failed=0,
replacementExecutions=1, replacementEventsPublished=1
```

This is durable evidence that the shared stream Provider can detach after event 3 and complete
through replacement. It is not the dedicated R4-B6 `NativeInferenceClient` continuation or a
cross-process qualification run, so CC-4c/T011-C remains open. Raw output is retained at
`.codex-tmp/spec182-r4-b6-cc4c-20260908/integration-cc4c.log` with `rc.txt`.

## Miss taxonomy

- **Static findings**：审查及接线复核定位了 structured request ID 被截断、SVS 同 session 旧
  sequence 重放、End 后错误制造 gap，以及单 worker handler 等待造成的控制面阻塞；均已在
  本批修复并由对应具名测试或 R4-B6 运行覆盖。
- **Compile/build misses**：最终 `-j4` 构建无失败，仅保留 fixture 聚合初始化 warning；
  早期缺少 backend/epoch fixture 值的编译与链接问题已在本批之前的增量迭代修正。
- **Runtime/test misses**：初始运行依次暴露 subscription regex、structured parser、
  provider epoch、SVS replay、catalog publication、event lineage、post-End retry、控制
  发布调度、保留 control 重放和 continuation prefix 等边界；修复后正向两轮通过。一次
  1010-case 全量 unit 尝试还触发既有 `StreamFacade` TPM 私钥缺失及其连锁 abort；这不是
  R4-B6 逻辑结果，最终 `Spec175InvocationStreamMessage/*` 17 cases 和
  `GenericDynamicApi/PreparedAndMessages/*` 16 cases 均通过。该环境失败仍交给 T016 的
  完整资格记录，不能标为 PASS。
