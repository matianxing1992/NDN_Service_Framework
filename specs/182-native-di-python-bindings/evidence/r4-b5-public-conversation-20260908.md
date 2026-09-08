# R4-B5 Public Conversation Requester Boundary

## Scope

本批把 R4-B4 的公开 requester 接线推进到一个可观察的本地成功出口，并修正
continuation 中动态 request envelope digest 的 owner 边界。`NativeInferenceClient`
在编码包含 native owner 分配的 request ID 的 envelope 后，为空的
`NativeConversationContinuation.requestContractDigest` 填入真实 digest；调用方提供
非空值时仍由 `NativeConversationCoordinator` 和 planner 精确校验。

测试夹具通过公开 `NativeInferenceClient`、Core ACK/plan/commit、stream final、
`NativeConversationCoordinator::prepareCheckpoint/commitTurn` 的真实调用路径运行；
receipt 与 Provider commit ACK 使用 `VerifiedCollaborationData` 的已认证结构预置到
LocalMock User，以便确定性地验证 requester 的等待、digest 绑定、终态和清理。
这证明本地事务接线，不证明真实 Provider 进程、SVS 验证或跨进程两轮恢复资格。

## Static Review

按 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的只读协议检查完整差异及上下文，
覆盖 `NativeInferenceClient` 调用方、continuation 所有权、coordinator pending 生命周期、
测试 receipt/ACK 结构和 case-manifest 构建注册。结果：`No findings.`
`git diff --check` 通过；未在静态门运行构建或测试。

## Validation

第一次重建命令错误地把 `PATH=...` 作为 `/usr/bin/time` 的待执行程序，编译器未启动，
返回 `exit127`；原始日志保留在
`.codex-tmp/spec182-r4-b5-public/build-r2.log`，并已登记到
`docs/failure-log.md`。修正 command-local PATH 后使用 system compiler/binutils、
同一 Waf 树和 `-j4`：

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ./waf -o build-nac182 build --targets=unit-tests -j4 -v
exit=0, elapsed=30.69s
```

定向结果：

```text
Spec182V3Placement/PublicClientConversationCommitsSeededReceiptAndCheckpoint
1 case, 19 assertions, exit=0, elapsed=0.30s

Spec182V3Placement/*
9 cases, 555 assertions, exit=0, elapsed=2.20s

Spec182Conversation/*
5 cases, 39 assertions, exit=0, elapsed=0.13s
```

原始输出分别保存在 `.codex-tmp/spec182-r4-b5-public/test-r3.log`、
`placement.log` 和 `conversation.log`。`validate_design.py` 在本轮文档同步前后
均通过；本批未执行完整 integration、Provider runtime、跨进程两轮恢复或 T016。

## Result And Boundary

本地公开请求现在有一个通过的 FULL_CONTEXT 首轮事务示例，动态 envelope digest 不再
要求调用方预知 native request ID；相关测试已登记到
`tests/fixtures/spec182/case-manifest.json`。T011-C 和 R4-B4 仍保持 `PARTIAL`：
尚缺真实 Provider receipt/control 传输、第二轮 `APPEND_DELTA`、恢复/替换以及
跨进程资格证据。下一批应在 `tests/integration-tests/` 使用
`NdnsfIntegrationEnvironment` 和 `NativeProviderHandler` 接通同一公开入口，成功后
再推进 T012-A bindings；不得用本地预置记录关闭 T011-C/T016。
