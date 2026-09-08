# R4-B4 Authenticated Conversation Chain

## Status and Exit Capability

IN_PROGRESS / T011-C。本批已将公开 native requester 的 FULL_CONTEXT/APPEND_DELTA owner turn、
Provider receipt 收集、COMMIT/ROLLBACK/FINALIZE 控制和终态清理接线；取消和提交失败的状态
边界也已收口。当前只有静态审查、组件/共享回归和本地 oracle 证据，尚无真实两轮跨进程
请求资格，禁止把本批局部 PASS 写成 T011-C 或 Spec182 完成。
遵循 CD-007/M34–M38，不另起 Spec，不修改已有 Provider KV owner。

## Source Audit (baseline before CC-3A/CC-3B)

本表保留接线前的缺口快照；CC-3A/CC-3B 的当前实现与剩余出口见下文对应章节。

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

本记录同时登记当前批次的实现与缺口：CC-1/CC-2 的 wire、journal、coordinator 以及
CC-3A/CC-3B 的 requester/provider 接线已有局部行为 PASS，但没有真实两轮跨进程资格。
R4-B3 的24 cases/411 assertions仅证明其稳定文本范围，不能证明认证会话能力。
T011/T012/T016仍未完成；下一步是补公开 FULL_CONTEXT→receipt→durable commit→
APPEND_DELTA→restore 的 integration harness。

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

## CC-3 Commit Boundary Repair

接线前已确认原Provider handler在COMMIT ACK后立即退出wait，无法补偿后续journal失败。
已按 [Conversation Commit Completion](../contracts/native-token-stream-design.md#conversation-commit-completion)
编码FINALIZE/ROLLBACK有界窗口：COMMIT后可重发ACK，匹配checkpoint的ROLLBACK释放精确
successor并发认证ACK，FINALIZE收尾。超时不能推断journal失败，保留已COMMIT状态至期限。
旧协议参与方未验证此窗口，不因此获得新事务资格；新增Provider路径尚缺行为测试。

同时修正handler原wait/rollback lambda对内层receipt/binding/role局部变量的引用捕获，
改为值捕获；staged rollback失败不再默默发送成功ACK。仍复用runtime现有精确release，
不删除旧parent、不增加另一个KV owner。COMMIT/ROLLBACK ACK使用原身份字段及typed JSON。

coordinator新增durableCommitGate：operation可将journal/parent发布与其单终态门放在同一
取消互斥区；新增finalizeProviderState负责成功后释放Provider等待slot。耐久发布后的
通知/FINALIZE异常不再触发rollback。已编写gate拒绝与发布后异常C++用例，未运行。

## CC-3 Remaining Wiring Map (superseded by CC-3A/CC-3B)

以下是 CC-3 接线前的缺口地图，保留用于解释批次演进；当前状态以 CC-3A/CC-3B 章节为准。

- NativeInferenceClient.cpp::Operation、acceptGenerationEvent、streamComplete、markTerminal：
  目前仍未消费conversation owner。markTerminal立即CancelCollaboration及clear scopes，
  会早于需要密钥的ROLLBACK/FINALIZE；必须将事务清理与串行worker收尾协调，不能只加回调。
- ServiceUser::waitForVerifiedCollaborationData/publishCollaborationData为现成只读/控制端口；
  返回requestId、keyScope、topic、producer、producerRole、signerCertificate、wireDigest。
  receipt topic=/ndnsf-di/conversation/receipt，commit/rollback ACK对应同名前缀，control
  topic=/ndnsf-di/conversation/control，scope=ndnsf-di-conversation-state-v1。
- NativeRequestPlanner.cpp在生成每个NativeRoleProjectionInputs后、NativePlanSealer::project
  前填conversationTurnBinding/stateReference；Core plan增加同scope与所选roles。
  NativePlannedRequest需携带归一化turn上下文，owner begin须在commit前生效。
- 旧plan-role-map digest是排序(role,provider)列表的canonical digest，不能简单用planDigest
  代替；换Provider重算与APPEND_DELTA的旧state引用必须区分新旧map，明确full-prefill语义。
- canonicalTokenIds必须与真实prepared input一致；NativePreparedInput当前只有payload，
  不得把未核对的caller token列表当模型实际输入。完整输入/续接投影仍需按现有tensor格式接线。

首次R4-B4 C++增量构建使用了旧降档`-j2`，构建成功（2m47.131s）；这不是当前主机默认。
随后`Spec182Conversation*`选择器9 cases中8项失败，首边界为canonical JSON Unicode编码
不符合旧Python `ensure_ascii=False`，并连带造成receipt digest失败；不是Provider网络结果。
公开两轮请求仍未完成，继续CC-3输入/投影、receipt收集、控制及单终态接线后，才进入整批验证。
不能把上述失败或修复当作事务资格。

### CC-1/CC-2 Focused Validation

修正`NativeCanonicalJson`保留UTF-8（与旧Python `ensure_ascii=False`一致）后，按当前开发机
默认`-j4`完成增量检查；`vmstat`后续采样`si/so=0`，未见持续换页。运行
`build/unit-tests --run_test=Spec182Conversation*`：**9 cases PASS，exit 0**。
覆盖旧checkpoint/transcript/envelope parity、native journal读写/lease/torn-tail/quota、
owner FULL_CONTEXT/APPEND_DELTA、abort/replacement/durable gate及restore。Python oracle
`build-conversation-oracle.py --check`仍为6 checkpoints/4 transactions PASS。
这次结果只关闭Wire/Journal/Coordinator的focused行为边界；Provider确认窗口、公开client入口、
真实Core/Provider网络和T016资格仍未完成。

共享回归选择器`Spec182CanonicalJson*,Spec182Conversation*,Spec182EpochText*,
Spec182StreamAcceptance*,Spec182Sampling*`按`-j4`构建后的运行结果为**25 cases PASS，exit 0**。
这确认会话专用UTF-8编码没有改变既有框架canonical-json或已验收的epoch/stream/sampling行为；
仍不授予真实Provider/Core网络或完整Spec资格。

### CC-3A Requester-to-Projection Context

在公开 requester 与 Provider 控制接线前，先把 continuation 的 owner 边界接入请求选项、
operation 和 planner。`NativeInferenceClient` 现在在 ACK/plan 前由
`NativeConversationCoordinator::beginTurn` 创建 request-scoped turn；planner 校验
request/attempt/service/contract、完整 role/provider map 和排序 role-map digest，并将
`ConversationTurnBindingV1` 投影到每个角色。APPEND_DELTA 还从已认证 parent checkpoint
读取每个 role 的 receipt digest，形成 role-local state reference；conversation scope
`ndnsf-di-conversation-state-v1` 同时加入 Core plan key scopes。旧 planner 调用保留默认空
turn 参数，未提供 continuation 的请求行为不变。

按只读静态门检查，未发现 caller 提供 attempt/plan digest 取代 owner 生成值、缺角色引用或
把 plan digest 当 role-map digest 的路径。使用已验证 build tree 仅构建
`ndnsf-distributed-inference`，系统 compiler/binutils、`-j4`，exit 0（24.489s）；
`unit-tests --run_test='Spec182PlanSealer*'` 为 12 cases PASS，exit 0。该批尚未证明
真实 continuation projection、receipt 收集或 Provider 网络事务，R4-B4 与 T011-C 仍为
PARTIAL。

## CC-3B Requester Provider Transaction Wiring

CC-3B 将 CC-3A 的 owner turn 和每角色 projection 接到公开 requester 的最终流式路径。
`NativeInferenceClient` 在 stream final 通过现有 `ServiceUser` collaboration 接口等待
每个 sealed role 的 Provider receipt，并逐项核对 request/scope/topic/producer/role、
conversation/epoch/service/plan-role-map、origin request/generation、requester identity、
provider assignment 和 retention expiry。receipt 的 application digest 重新计算并与其
payload 字段一致，外层 SVS 验证仍是认证边界。

Requester 随后建立完整 `NativeCompletedAttempt`：canonical parent token prefix 与已接受
token、generation/tokenizer/chat-template metadata、application payload 和排序后的
receipt set 一起交给 coordinator。Provider promotion 使用现有 request-scope encrypted
collaboration control：每个 role 先收到 canonical `COMMIT`，Requester 等待带有精确
checkpoint/receipt/provider boot/cache identity 的 canonical commit ACK；失败路径只在
已知 checkpoint 时发送 `ROLLBACK`，成功路径发送有界 `FINALIZE`。Provider handler 保留
COMMIT 后补偿窗口，可重发 ACK，或在匹配 checkpoint 的 ROLLBACK 下释放精确 successor；
丢失 FINALIZE 不被推断成 requester 失败。

本批静态审查按官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 的只读协议执行，
覆盖 NativeInferenceClient/NativeConversationCoordinator/NativeProviderHandler 的完整
差异、调用方、服务 collaboration API、planner projection 和已有流式测试。发现并修复的
控制性问题如下：

- coordinator 原先在持有自身 mutex 时调用 durable gate，而 gate 需要 operation mutex；
  现在 callback 在 coordinator 锁外执行，发布 lambda 自己短持锁并复查 owner/parent；
- stream final 的 token 接受先推进 coordinator，再交换 requester prefix，避免半提交；
- deadline/cancel 在会话提交期间不提前清除 scope key，事务 guard 结束后才清理，以保留
  ROLLBACK/FINALIZE 所需密钥；
- replacement 已切换 coordinator attempt 但尚未安装到 operation 时，终态竞态会显式 abort
  新 turn，避免 pending owner 泄漏；无 runtime 的 conversation option 在 begin 前拒绝。

静态审查结果为 `STATIC_PASS / TESTS_DEFERRED`，随后批末验证通过：

- `git diff --check`：exit 0；
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ./waf -o build-nac182 build --targets=ndnsf-distributed-inference -j4 -v`：exit 0，当前 DI shared library 成功构建；
- `./.codex-tmp/spec182-r4-b2/build/unit-tests --run_test='Spec182Conversation*,Spec182StreamAcceptance*,Spec182CanonicalJson*,Spec182EpochText*,Spec182Sampling*' --log_level=test_suite`：25 cases，exit 0；
- `./.codex-tmp/spec182-r4-b2/build/unit-tests --run_test='Spec182ProviderHost*' --log_level=test_suite`：6 cases，exit 0；
- `python3 tests/fixtures/spec182/build-conversation-oracle.py --check tests/fixtures/spec182/conversation-oracle.json`：exit 0；
- 原始日志：[build-final.log](../../../.codex-tmp/spec182-r4-b4-current/build-final.log)、
  [tests-final-r2.log](../../../.codex-tmp/spec182-r4-b4-current/tests-final-r2.log)、
  [provider-host.log](../../../.codex-tmp/spec182-r4-b4-current/provider-host.log)、
  [oracle.log](../../../.codex-tmp/spec182-r4-b4-current/oracle.log)。

这些结果证明当前 native library 的编译、会话 owner、流式接受和独立 oracle 没有回归；
尚未证明真实两轮请求、跨进程 receipt/control、Provider 运行时重算或 T016 qualification。
CC-3B 与 T011-C 保持 `PARTIAL`，下一出口是补真实两轮/恢复 integration harness，再运行
T015/T016 规定的完整 unit→integration→MiniNDN/no-Python gates。

## Progress and Feasibility Audit

（历史快照：pre-CC-3A/CC-3B）

本节保留 2026-09-08 暂停新增实现时的审计快照。Requester/provider 接线后的当前状态
见上方 CC-3B 与 tasks.md 的 Current Checkpoint；不要用本节旧 Findings 覆盖最新批次证据。

2026-09-08；用户要求暂停新增实现、审计继续执行能否达成目标。
源码基线为`3e2d0ea9`加当前未提交改动；本节是源码与证据审计，不是原生行为验收。

### Verdict

**CONDITIONAL PASS**（保留C++迁移路线）；完整交付尚未通过。
现有原生库、准备/授权、配置化requester、stream和epoch测试支持技术可行性，
没有发现要求推倒Spec182的已证实根本障碍。但按当前不断扩大的R4-B4批次继续，
缺少及时行为反馈，不能据此承诺顺利完成。必须先形成稳定接口和可验收的批界。

### Verified Progress

- 结构检查：19 FR、11 SC、17父任务，其中T001--003共3项完成。其余14项未完整验收；
  父任务数量不代表剩余工作量比例，多个PARTIAL包含有效实现。
- R3-B1配置化公开requester初始请求、R4-B2流式接受与有限replacement已有本地证据；
  其transport替身测试不证明真实Provider重算或网络资格。
- 最后原生构建/测试仍是[R4-B3](r4-b3-epoch-text-20260908.md#final-local-result)：
  [tests.log](../../../.codex-tmp/spec182-r4-b3-r2/tests.log)为24 cases/411 assertions PASS；
  同目录build.log和library-build.log分别记录24.266s及9.353s成功增量构建。
  这些结果属于当时源码，不覆盖当前dirty tree。
- 此后连续5个checkpoint（`67bdc448`至`3e2d0ea9`）提交了文档及部分离线参考向量，
  没有提交本批原生实现或产生本批C++行为PASS。新增Wire/Journal四个源文件共814行，
  加上owner、Provider、epoch及测试修改仍未验收。离线Python参考成功不计C++成功。

### Findings

| Severity | Evidence | Finding and required closure |
| --- | --- | --- |
| HIGH | `NativeInferenceClient.cpp:1020,682-699`；上文接线地图 | client只保存conversation owner；stream final直接进入成功终态，尚无begin/receipt/commit/restore会话链。必须让公开入口驱动完整事务，不能用直接owner测试替代。 |
| HIGH | CC-1--CC-4及当前未提交源码 | 同批已覆盖codec、持久存储、owner、Provider补偿、输入投影和终态并发，多轮没有原生反馈。应以稳定端口和独立验收价值划界，避免“减少编译”变成无限推迟验证。 |
| HIGH | `NativeEpochCoordinator.cpp:310,323,718`；`NativeProviderHandler.cpp:2804`；`NativeConversationWire.cpp:380` | Provider receipt使用运行状态链摘要，旧checkpoint使用逻辑token列表摘要。当前新增区分和原始prompt长度metadata尚未获C++验证；必须证明首次、续接、恢复及replacement都验证正确对象。不能将当前修补认定为已关闭契约问题。 |
| HIGH | `NativeRequestPlanner.cpp:224`；上文CC-3接线地图 | turn/state投影、真实prepared tokens、旧parent与新placement、scope密钥清理顺序仍未接通。必须固定数据来源和取消/提交胜负边界，避免逐字段追补。 |
| MEDIUM | tasks.md的R1--R7及T010--017；SC-001/002/005/006/007 | 绑定、完整输入模式、调用方迁移、旧Python退出、无Python资格和交付仍有工作。会话完成也不等于Spec完成；目前没有完整本地生产矩阵通过的证据。 |

### Recommended Next Boundary

1. 保留已验证基线与未验收工作，先冻结prepared tokens、双摘要、parent/placement、
   receipt身份、durable commit及终态清理之间的一份端口/状态表。
2. 评估将已有codec/journal/owner组成可独立验收的组件批：完成该批静态审查后运行
   相关C++单测与必要ABI依赖检查。稳定接口是拆批条件，不按文件数拆分。
3. 再闭合公开FULL_CONTEXT→全部Provider receipt→持久提交→APPEND_DELTA→恢复，
   同时覆盖取消、提交失败和一次replacement。实施阶段写好集成测试；正式运行仍按
   T015/T016顺序，不因本建议跳过既有资格门。
4. 此后完成同库绑定/调用方迁移和旧路径退出，最终以无Python的真实YOLO/Qwen矩阵
   判断SC是否成立。若公开链仍无法形成，应再次审查接口，而不是继续增加组件。

以上为审计建议，未修改plan、执行顺序或测试门；用户本轮未要求恢复实现。

### Checks and Limits

本轮strict结构检查与check-prerequisites PASS；Context Mode active health exit0；
CodeGraph status报告up to date，并用实际源码核对入口。读取现有R4-B3原始日志，
没有重跑产品测试、构建或实验。当前未提交的两份dependency/generation设计文档属于
既有其他会话改动，不纳入本审计checkpoint。审计没有证明当前会话实现无缺陷，
也没有估算日期或完成百分比。

文档checkpoint首次被本地pre-commit全索引引用扫描拒绝，产品状态不变；
首边界已记录到failure-log。后续使用钩子显式支持的`NDNSF_LOCAL_CHECKPOINT=1`
保存本地审计，不关闭禁止路径检查，也不推送。
