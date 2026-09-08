# R4-B6 Real Provider Conversation

## Status and stable exit

READY / T011-C CC-4。该批只围绕一个真实可观察出口：公开
`NativeInferenceClient` 发起 FULL_CONTEXT 首轮，由 `NativeProviderHandler` 通过现有
SVS 接收并发布 authenticated receipt，Requester 完成 COMMIT/commit ACK 后，以同一
`NativeConversationCoordinator` 发起 APPEND_DELTA 二轮并解析 Provider 状态引用；同时保留
一次恢复或 replacement 负例的首个失败边界。预置 `VerifiedCollaborationData` 不计入本批。

实现依赖为 R4-B4/R4-B5 已验证的 requester/coordinator 接口和 Spec175 tiny ONNX Provider
fixture；验收依赖为真实 receipt/control/commit ACK、二轮 checkpoint lineage、必要负例及
`integration-tests` source closure。完成前不推进 T012 bindings 或 T016 qualification。

## Planned members and ownership

| Member | Scope | Owner | Planned selector |
| --- | --- | --- | --- |
| CC-4a | `NdnsfIntegrationEnvironment` 中真实 Provider 与公开 NativeInferenceClient 首轮接线 | current executor | `Spec182NativeConversation/RealProviderFullContext` |
| CC-4b | 同一 coordinator 的 `APPEND_DELTA` 与 receipt/control/commit ACK lineage | current executor | `Spec182NativeConversation/RealProviderAppendDelta` |
| CC-4c | recovery 或 single replacement 负例，记录首个失败边界并保持父 checkpoint | current executor | `Spec182NativeConversation/RealProviderRecovery` |

## Coverage matrix (planned)

| Lane | Status | Required scope |
| --- | --- | --- |
| production entry/callers | planned | `NativeInferenceClient::request`, `NativeConversationCoordinator`, `NdnsfIntegrationEnvironment` and real Provider handler |
| implementation and wire | planned | `NativeProviderHandler` receipt/control, `NativeInferenceClient` continuation, Core/SVS wire |
| test/harness/oracle | planned | existing tiny ONNX fixture, real receipt/ACK assertions, second-turn lineage and recovery negative oracle |
| build/source closure | planned | `tests/integration-tests/ndnsf-di-core-flow.t.cpp`, `tests/wscript`, case-manifest and DI source closure |
| migration/evidence | planned | T011-C evidence, fresh `.codex-tmp/spec182-r4-b6-provider/` run directory, failure-log on first boundary |

## Gate order

Each small implementation member is followed by the read-only official `review-agent` gate and
an updated matrix. After CC-4a/b/c and the composition review, run one system-first
`integration-tests -j4` build and only the named R4-B6 selectors. Keep the batch `PARTIAL` until
both turns and the selected recovery/replacement boundary are observed with real Provider
receipt/control. A local seeded receipt or a successful link is not an acceptance result.
