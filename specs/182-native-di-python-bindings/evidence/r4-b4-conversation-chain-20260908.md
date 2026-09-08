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

## CC-1 Wire Implementation Checkpoint

新增 NativeConversationWire.hpp/.cpp，放在现有 ndnsf-di 原生库源目录，由既有 glob
纳入构建；这是 C16 共用的格式/认证函数，不另建会话状态 owner。已经编码：

- 旧 canonical checkpoint SHA256/HMAC、严格字段/整数/摘要、身份/epoch/expiry、轮换key验证；
- transcript prefix与checkpoint绑定、完整role receipt集合及摘要、base64格式；
- RuntimeJournal原身份域分离子密钥，以及v1 HMAC/v2/v3 AES-GCM envelope读取。
  使用系统OpenSSL；v1/v2旧格式路径尚缺独立fixture，不能以v3替代兼容证明。

已编写 Spec182ConversationWire 两个 C++ 用例：旧checkpoint签名/解析、错key/期限/
篡改/类型、transcript缺/重复receipt与prefix、v3解密/subkey/错identity/keyId/tag。
**尚未构建运行**；源码审查检查了认证前不返回明文、失败路径清除临时解密输出、
字段与原 wire 对齐、常量时间签名比较、有限key ring和输入上限。下游owner仍待接入。

参考author现新增实际旧Provider receipt/完整transcript，以及RuntimeJournal真实
prepare/commit/reopen产生的两个加密事务；固定随机nonce仅作用于离线参考fixture。
fixture有明确公开测试key，未读取任何运行key/journal。新生成/--check exit0，
旧两个冻结case逐值保持不变。扩展前的--check预期报告fixture尚未更新；生成后通过。
新增checkpoint两个（共四个）、transcript两个、journal transactions两个，
参考实际重新打开并解密成功；这不是原生运行结果。

资源边界暂定checkpoint wire 1MiB、transcript JSON 16MiB、加密envelope 64MiB、
verification key ring 16项。CC-2显式配置总journal quota、单写lease和恢复上限；
不得用逐条上限替代总额约束。C++实现与测试保持未验收，下一成员先实现持久owner，
然后CC-3接公开调用，批末一次构建及相关测试。

## CC-2 Journal Implementation Checkpoint

新增 NativeConversationJournal.hpp/.cpp，作为 C16 的持久化端口，沿用旧 RuntimeJournal
格式，不创建第二种checkpoint协议。已编码：owner目录/文件检查、跨进程flock lease、
journal及旧spool总quota、旧checksum/transaction展开、仅末尾未终结语法损坏的torn-tail
修复、加密envelope+conversation index同一事务追加、fsync后晋升内存视图。
追加失败尝试truncate/fsync恢复旧长度；恢复失败则poison实例，禁止继续读写。
解析/字段重复/未知schema/完整记录checksum错误不能当作可丢弃尾部。

恢复验证旧加密wire和payload摘要、checkpoint/transcript/index身份及期限、role receipt
集合后返回记录。journal负责加密存储认证；checkpoint签名、最新parent CAS、pending turn
与Provider promotion仍由C16负责，当前尚未接入，不能单独调用journal冒充会话验收。
新增wire端口使用OpenSSL RAND/AES-GCM生成v3 envelope，nonce每次随机；base64复用EVP。

Spec182ConversationJournal 已编写旧两条事务读取、native追加再打开、无spool恢复、
writer lease竞争、torn tail、quota拒绝保留旧文件和完整record篡改用例，**尚未运行**。
源码按review-agent只读检查发现并修正两点：duplicate key不能吞为torn tail；quota必须
计入旧兼容spool，不能只算新log。当前无C++编译/行为PASS，产品源码保持未提交。
下一步CC-2剩余：替换NativeConversationCoordinator scaffold，创建真正pending turn/
abort/prepare/CAS/restore owner并消费上述端口；随后CC-3公开接线，同批末统一构建测试。

## CC-2 Coordinator Owner Checkpoint

coordinator scaffold已替换为显式owner/key配置、pending map和opaque ticket：
FULL_CONTEXT经begin创建首轮，APPEND_DELTA验证签名父记录和canonical prefix；
接受前缀只可扩展，abort实际移除pending，旧副本不能prepare/commit；replacement
最多一次，保留接受前缀。path-only构造因没有认证key明确拒绝，不明文降级。

prepare收齐声明角色receipt，核对request/generation/共同scope/prefix，产生旧V1签名
checkpoint/transcript，不再接受任意providerStateDigest或手工seed。commit核对owner中
prepared值/parent，锁外执行Provider晋升，锁内复查取消/期限/parent，journal耐久后才
晋升可见记录；失败调用rollback，rollback失败明确报错。restore全部验证后swap。
journal append增加durable parent epoch检查，避免两个C16共享journal时仅凭内存CAS分叉。

新接口NativeConversationConfig/acceptTokenPrefix/replaceAttempt改变新增会话类型布局，
批末须处理真实消费者ABI，不能复用失效对象。旧手工seed测试已替换为3个
Spec182Conversation case：真实两轮begin/accept/prepare/commit与恢复、abort旧副本/
晋升期间cancel、一次replacement拒绝旧attempt。预期仍取Python oracle。quota用例改为
合法下一epoch后撞额度，避免先触发CAS而伪称quota已测。

按review-agent静态审查修正旧rollback失败可能删除同名新turn及前缀接受异常安全。
一次工具patch同路径delete/add被拒（写入前），已顺序应用；文档patch上下文不匹配也
在修改前被拒，重新读取尾部后修正。均无产品运行。当前全部C++仍未构建/未验收/未提交。
下一步CC-3接NativeInferenceClient operation的continuation/accept/final/cancel/replacement，
复用Provider receipt与晋升控制，再整批构建；仅保存m_conversations不算接线。
