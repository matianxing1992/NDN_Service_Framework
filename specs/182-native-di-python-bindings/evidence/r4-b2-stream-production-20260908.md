# R4-B2 Native Stream Production Chain

## Scope and Status

基线e70161b4；**IN_PROGRESS / TESTS_DEFERRED**。对应T010-C与T011-B的生产接线，
沿用[native token stream](../contracts/native-token-stream-design.md#requester-acceptance-and-recovery-resolution)
及既有GenerationRecoveryV1。不重新设计Spec182，不将本批完成等同于T016真实网络资格。

## Verified Production Gaps

- NativeInferenceClient.cpp::beginCoreRequest只传BeginCollaboration普通response/timeout；
  Core ServiceUser.hpp已有streamOptions/onStreamEvent/onStreamComplete/onStreamError参数，
  应直接复用，不新增stream网络层。
- NativeInferenceHandle::Operation已有serial worker、绝对deadline、attempt与单终态锁；
  尚无acceptedTokenIds/acceptedText/acceptedTerminalHint/replacementStarted。
- publishEvent/observe是普通观察机制，异常隔离是现有契约；生成事件的接受回调必须
  明确区分，不能全局改变普通observer异常语义或把observer投递当作业务accept。
- NativeGroupProjectionBuilder::build显式拒绝TOKEN_FEEDBACK；其group operation按epoch
  扩展的机制已经存在。需补feedback endpoint/operation身份，不复制group授权实现。
- 现有Python AutomaticStreamingHandle._accept_event在内存锁内接受token，锁外回调；
  不是持久journal。其prefix摘要为逗号连接十进制token IDs的SHA-256。
  原生实现还须按已接受契约验证textDelta、finishHint和final文本，不照搬旧文本缺口。

## Batch Members

| Member | Write / observable outcome | Static gate |
| --- | --- | --- |
| ST-1 | NativeInferenceClient.hpp/.cpp；同一operation内严格event身份/顺序/摘要/预算校验与原子接受，明确生成回调及普通observe边界 | stale attempt、重复/缺序、callback异常与取消竞争不会回滚或重复接受 |
| ST-2 | NativeRequestPlanner、NativeGroupProjectionBuilder与既有projection owner；generation contract、TOKEN_FEEDBACK端点与capability同源，注册Core stream回调 | 复用sealed identity、operation stride和终端Provider授权，不借环境或另一份库绕过 |
| ST-3 | NativeInferenceClient.cpp；eligible attempt1错误触发一次replacement，保留原deadline和已接受前缀，final逐字段核对 | terminal token后禁止replacement，未接受token不进入recovery，旧attempt晚到事件不改变结果 |
| ST-4 | tests/unit-tests/distributed-inference-stream-recovery.t.cpp及现有生产链fixture；三处真实integration caller按既有契约同步 | 六个具名accept/recovery负例及stable delta拼接等于final；测试调用真实operation而非复制状态机 |

修改接口/字段时同步CD-001的原有owner及case-manifest；普通局部细节不另建第二份契约。
每个member编码后只读静态门，全部接线审查后统一构建和相关C++测试。
client布局/公共API改变时先核对受影响消费者ABI边界，不能直接沿用R4-B1的无ABI结论。
真实integration/MiniNDN执行保留T016；本批编写所需测试，不提前运行正式资格。

## Current Evidence and Next Step

CodeGraph定位当前NativeInferenceClient.cpp后以canonical源码核对以上边界；搜索结果中
.codex-tmp staging副本已排除，不作为当前实现依据。本批尚未修改产品或运行测试。
下一步先在ST-1完成accept操作及显式生成回调接线设计，再继续ST-2/ST-3；不得以孤立
accept辅助类测试通过代替完整Core回调接线。进度表保持T010-C PARTIAL。

本次批次登记的validate_design.py检查PASS（ok=true、748个local links），git diff
whitespace检查PASS；仅文档/执行边界核对，不构成产品STATIC_PASS或行为验收。
