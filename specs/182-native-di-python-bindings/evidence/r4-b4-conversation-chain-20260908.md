# R4-B4 Authenticated Conversation Chain

## Status and Exit Capability

IN_PROGRESS / T011-C。本批完成后，公开 native requester 应能执行 FULL_CONTEXT 首轮、
收齐 Provider receipts、提交认证 checkpoint/加密 transcript，再执行 APPEND_DELTA；
取消和提交失败不能改变已提交父记录。当前不具备此能力，禁止以旧两个组件测试关闭本卡。
遵循 CD-007/M34–M38，不另起 Spec，不修改已有 Provider KV owner。

## Source Audit

核对基线 bfb66aa4，源码与旧 Python 契约直接对照：

| Boundary | Current evidence | Required repair |
| --- | --- | --- |
| Begin | NativeConversationCoordinator::beginTurn 只接受 parent>0，必须已有 record；旧测试手工伪造 seed turn | FULL_CONTEXT 合法创建首轮；APPEND_DELTA 验证认证父 checkpoint、完整 canonical prefix 与非空 suffix |
| Inflight ownership | beginTurn 返回值但不登记 inflight；abortTurn 只加锁，无状态变更 | coordinator 持有 request/generation/attempt 对应 pending turn；abort 幂等撤销，旧副本不能 prepare/commit |
| Checkpoint | prepareCheckpoint 用分隔字符串 hash，只有单个 providerStateDigest，未验证 acceptedTokenIds | 使用 conversation.py::ConversationCheckpointV1 原 wire、canonical SHA256/HMAC 与完整 role receipt set；结果前缀和 lineage 一致 |
| Commit | commitTurn 可接受未 begin 的 turn；临时文件 ofstream 未检查写失败，无 fsync/跨进程 lease | 只晋升 owner 中 prepared turn；parent CAS、Provider promotion、持久事务后可见；失败保留父 checkpoint |
| Journal | C++ 明文单 .json；旧 RuntimeJournal 是加密 envelope 与 journal.jsonl transaction/index | 复用旧 schema/加密及 key rotation 语义，不能把未认证 C++ scaffold 文件冒充旧兼容格式 |
| Restore | 先 clear m_records，逐文件插入；无摘要重算/签名/版本/期限/完整后继验证 | 全部验证后原子替换已提交视图；未知/篡改/部分提交拒绝；未提交临时文件不晋升 |
| Requester | NativeInferenceClient 仅构造时保存 m_conversations；生产树无 begin/prepare/commit/abort 调用 | options continuation→begin→planner/selection turn binding→全角色receipt→prepare→Provider COMMIT→journal→final；失败/cancel→abort |

旧参考直接定位：
[conversation.py](../../../NDNSF-DistributedInference/ndnsf_distributed_inference/conversation.py)
的 ConversationContinuation、ConversationCheckpointV1、ConversationTranscriptRecordV1、
ConversationCoordinator；[runtime_journal.py](../../../NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/runtime_journal.py)
的 prepare_envelope/commit_prepared_envelope/read_envelope、append_many/_load_records、
authentication_key_ring 与 _exclusive_lock。
已有原生 ProviderConversationStateReceiptV1/ConversationTurnBindingV1 在
[ConversationStateBinding.hpp](../../../NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp)，
Provider state/promotion 原位复用，不复制第二个 KV ledger。

## Execution Members

1. CC-1：以旧 Python 独立生成并冻结 checkpoint/continuation/transcript/journal oracle；
   补原生值契约与严格解析、摘要和认证。保留旧 wire，不采用 scaffold 字段格式。
2. CC-2：替换 coordinator scaffold：owner-injected key/config、FULL_CONTEXT/APPEND_DELTA、
   inflight/abort/prepare/parent CAS、单写 lease、加密事务和 restore；不把 old key 或秘密入日志。
3. CC-3：接公开 NativeInferenceClient operation 及 planner/provider 已有 turn/state引用和
   promotion控制；绑定认证 receipts、原 deadline、单次 replacement、cancel/late callbacks。
4. CC-4：C++ Spec182Conversation 直接走公开两轮请求及恢复；负例覆盖错parent/prefix、
   缺/重复/错role receipt、旧attempt、abort后commit、重复commit、写失败、未知/篡改journal。
   编写 integration 两轮/恢复用例留 T016；批末统一构建测试。

CC-1 至 CC-4 属于同一能力批次。逐成员静态门、批末共享构建；不按每个字段重编，
不以只有 codec 测试通过代替此批出口。ABI变更后按依赖闭包决定构建树边界。

## Validation Boundary

本记录为当前源码审计与可执行缺口登记，尚无本批生产实现或行为 PASS。
R4-B3 的24 cases/411 assertions仅证明其稳定文本范围，不能证明认证会话能力。
T011/T012/T016仍未完成；下一步从CC-1冻结原格式开始，同批推进到公开两轮调用。

## Oracle Author Attempt

首次author exit1：负例 json.dumps 非canonical，旧参考在 from_bytes:703 抛
ConversationCheckpointInvalid("checkpoint is not canonical")，不是预期摘要篡改边界。
author:46捕获仅ValueError而漏该自定义异常。改用参考 _canonical_payload 后检查篡改；
不修改参考实现或从 C++ 抄期望输出。尚未运行任何本批原生构建。

## CC-1 Reference Checkpoint

已生成 [conversation-oracle.json](../../../tests/fixtures/spec182/conversation-oracle.json)，
author=[build-conversation-oracle.py](../../../tests/fixtures/spec182/build-conversation-oracle.py)。
两个案例直接调用旧 ConversationCheckpointV1/ConversationContinuation，包含首轮、追加轮、
中文service、排序role摘要、公开测试key、prefix/checkpoint摘要及HMAC。sourceSha256锁定参考。
生成和 `--check` 均exit0；每例原格式round-trip、wrong-key、canonical篡改拒绝通过。
原始[初次边界](../../../.codex-tmp/spec182-r4-b4/oracle-first-failure.txt)及
[成功日志](../../../.codex-tmp/spec182-r4-b4/oracle.log)保留，author问题RESOLVED。
按review-agent只读审查author与生成物：无生产key读取、无native输出充当期望值，
原checkpoint字节和签名由旧参考生成。当前不含完整transcript/journal交易向量；CC-1
保持PARTIAL，后续补齐其向量和C++消费者，再继续CC-2/CC-3，批末统一构建。
