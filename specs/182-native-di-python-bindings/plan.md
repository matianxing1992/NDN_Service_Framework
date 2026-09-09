# Implementation Plan: Native NDNSF-DI with Optional Python Bindings

**Branch**: Experimental | **Revision**: 64 | **Date**: 2026-09-09
**Status**: IN_PROGRESS / 当前实现与验收状态见 tasks.md 的 Task Progress Registry 和 Current Checkpoint
**Spec**: [spec.md](spec.md)

## Summary

完整C++ DI复用Core协作/安全原语、Provider runtime和原生model adapters；
Python只作同库兼容绑定，旧默认控制路径退出。O-001源码基线已核对关闭，
O-002--O-005 已于 2026-09-07 全部关闭（code-design Open Questions 无 OPEN 项）；
实现按 T001-C 冻结的 build identity 与 case-manifest selector 从 T002-A 起执行。
详细接口与字段仅在contracts定义；本文件安排实施和验证阶段。

## Technical Context

主要行为测试直接用 C++ 调用生产库；Python 保留可选绑定兼容、离线 oracle 和
外部实验基础设施，不能成为原生核心 PO 的唯一证明。各实施批次同步迁移本行为
测试，T013/T015 核对覆盖，T016 分开执行原生主套件与绑定兼容套件；见
[native test ownership](contracts/proof-design.md#native-test-ownership)。
该 ownership 规则现已同步到共享 `batch-quality-gates` 及 plan/tasks/implement/
analyze/audit/converge skills：native requirement 必须登记生产 C++ target/selector；Python
focused 结果只能作为 binding/facade、离线 oracle 或外部设施证据。

建立可安装ndnsf-distributed-inference库与独立C++ consumer，Python绑定可选。
沿用Waf、C++/Boost/ndn-cxx/ORT；ONNX/tokenizer依赖及ABI锁由T001冻结。
内部 typed JSON 使用固定 nlohmann/json 源与许可证；版本/哈希见 native-dependencies.json。
T004 已接入完整 projection codec 与 canonical core/final identity；实际 planner metadata 和
requester 接线仍按未完成任务推进，定向 PASS 不代表整体资格验收。
原生构建使用核对后的system compiler/binutils、匹配Boost headers/libs、
NAC-ABE prefix与NDN-SVS source/build pair，并包含直接消费SVS ABI的NDNSD；ABI变化重建全部传递消费者与绑定并核对实际加载路径/hash。四库版本及旧证据失效边界见[integrated baseline](contracts/integrated-baseline.md#current-source-identity)。
本开发机默认-j4（6逻辑CPU/12GB RAM，2026-09-07用户授权并于2026-09-08确认），不因历史
证据中的-j2命令继承降档；不并发操作同一Waf树或叠加原生构建。
持续换页或桌面卡顿时下次降为-j2；其他机器及容器builder另核资源，见[build policy](../../docs/native-build-parallelism.md)。
T001允许有界依赖探针；产品构建按设计门和各任务的验证范围执行。

## Constitution Check

沿用Core协作与安全边界、同库绑定和旧路径退出契约；保留全部真实运行验收。
流程只调整记录方式与执行阶段，不缩减权限、协议、数值或隔离要求。

## Gate Order

### Current Dispatch Policy 2026-09-08

用户重新设定“完成 Spec182 全部任务”目标后恢复调度，从 tasks.md 登记的 R1-B1 开始，
此前暂停和重排成果保留，不重做 Spec182。剩余生产顺序
以 [R1–R7 capability stages](evidence/production-chain-replan-20260908.md#remaining-batches)
为准：模型/候选→准备与授权计划→完整请求→流式/会话→同库调用方→旧路径退出及
验证工具→最终资格交付。原 G0–G6、17 个父任务、36 张基线卡和硬验收门仍有效；
当前执行表在此基础上新增 T013-C observer remediation card。
R1–R7 是能力阶段，不是固定执行批次或“每阶段只编译一次”。恢复后按
[batch selection](evidence/production-chain-replan-20260908.md#executable-batch-selection)
为当前阶段登记 R<n>-B<k>：同一行为、共享契约的生产者/消费者及共同验证一起闭合，
有稳定接口和独立验收价值才拆批；各小任务静态门后继续同批，批末统一构建测试。
每批登记分配依据：共同生产入口/调用方、接口/状态/所有权或数据契约、独立
oracle/测试 selector、源码/构建 closure 和验收出口。任一项不一致，或出现新的硬验收
依赖，就在当前批次关闭后登记新的 Batch ID；不以减少一次构建为合批理由。
每个小任务和批末审查还须在同一份 evidence 留下五 lane Coverage matrix，逐项给出
实际文件/符号、查询或检查命令及 `covered`/`N/A`/`gap`；没有矩阵不能记录
`STATIC_PASS` 或 `READY_FOR_BATCH_TESTS`。矩阵的 lane 为 production entry/callers、
implementation/wire、test/harness/oracle、build/source closure、migration/evidence。
不自动更改 Depends、DONE 或 FR/SC/PO；阶段出口和具体分批规则仅在该记录维护。
本次新目标构成恢复授权；后续仍按具体批次及硬门领取，不恢复逐字段构建。
已通过的 B-G1-YOLO-SEMANTIC 定向结果作为 R1 输入复用，不重新起草或重复构建。

2026-09-08 R4-B4 已收口为 `PARTIAL`：在已验证的 conversation wire/journal/coordinator、
stream acceptance 和 CC-3A planner projection 之上，requester receipt/control、Provider
COMMIT/ROLLBACK/FINALIZE 窗口及终态 scope 清理均已接线；只读 `review-agent`、DI
library/unit-tests `-j4` build、49-case requester/conversation/provider/stream regression，
以及补齐完整 DI source closure 后的 `integration-tests -j4` 链接和
`Spec182GrantClientFlow/*` 2-case 集成测试均通过。R4-B5 补出的本地公开 requester FULL_CONTEXT
首轮仍使用预置认证 receipt/ACK，不能代替真实 Provider。

**R4-B6 Real Provider Conversation** 已完成正向稳定出口但保持 `PARTIAL`：公开
`NativeInferenceClient` 的 FULL_CONTEXT 首轮经同一真实 `ServiceProvider` 完成
receipt/control/commit，再由同一 coordinator 发起 `APPEND_DELTA` 并提交第二轮状态引用。
具名 integration selector 和 V2 structured request/event/collaboration name 单测均通过；
静态门还发现并修复 SVS session/seq replay、End 后 gap retry 误判及单 worker 控制面等待
阻塞。R7-B1 已在真实 Provider harness 中补齐 CC-4c 的单 Provider replacement 负例：失败
Provider 被排除后在 `ACK_CLOSED` 返回 `DI_NATIVE_NO_ADMITTED_PROVIDER`，且不写入 successor
checkpoint。成功 alternate-provider recovery、跨进程资格和 T016 仍未关闭。不要把本批局部
PASS 写成 T010/T011 或全 Spec 完成。

### R7-B1 Single-Provider Replacement Negative 2026-09-08

R7-B1 将 CC-4c 作为独立批次收口：同一公开 `NativeInferenceClient`/真实 `ServiceProvider`
fixture 注入 `ProviderFailure`，开启一次 replacement，验证恢复 ACK 排除失败 Provider、
`NATIVE_REQUEST_STAGE_FAILED`/`ACK_CLOSED` 原因和空 conversation journal。首次测试误把单
Provider 环境当成可成功替换，真实运行先暴露 `DI_NATIVE_NO_ADMITTED_PROVIDER`；修正测试并
保留 exit201 原始证据后，负例与原有正向两轮均通过。下一批必须在多 Provider 配置下另行
证明成功 replacement，或沿稳定 owner/caller 依赖推进；不重用这次负例来宣称恢复资格。

### R7-B2 Alternate-Provider Replacement 2026-09-09

R7-B1 的 failure boundary 还揭示了 replacement map 契约缺口：`NativeConversationTurn`
只有父 checkpoint 的 `planRoleMapDigest`，`replaceAttempt` 切换 Provider 后 planner、receipt
和 control ACK 仍比较旧 map，导致备用 Provider 无法形成 successor。R7-B2 已修正这一共享
状态边界并加双 Provider positive/negative selectors：父 CAS 继续核对旧 checkpoint，当前
attempt 单独保存新 map，successor wire/record 使用新 map；失败、过期、旧 attempt 和无
admitted Provider 仍 fail-closed。静态复核还补上 coordinator 对 expected role set/provider
非空的直接校验，并加入 legacy `request-1/3` 与结构化 recovery name 的 parser 回归。
最终 `unit-tests`/`integration-tests` 以 system-first `-j4` 构建通过，具名 coordinator、
parser、单 Provider negative、双 Provider alternate replacement 与原有正向两轮 selectors
均通过；批次按 `CLOSED_FOR_VALIDATION` 关闭。该批不吸收 Python caller migration 或
T016 跨进程资格。

### R6-B9 Legacy D2b Freshness Repair 2026-09-09

R6-B7 的当前源码 trace 确认：同一 producer/session 的两个合法 Selection publication
使用不同 name，provider0 的 sequence 4 先到会让旧的全局 frontier 丢弃 provider1 的
sequence 3。R6-B9 只在 `ServiceProvider::isFresh` 内增加受 `svs_mutex` 保护的 per-name
frontier；更旧 session 和同名旧序列仍拒绝，更高 session 清空该 producer 的 name map，
不改变 User publication、wire、解密或 T016。`integration-tests` system-first `-j4`
构建 118/118 通过；五个 D2b selector、具名 D2h212 selector 及两次新鲜的未过滤
Spec170 suite 均 exit 0。首次未过滤运行曾间歇性暴露 D2h callback/`double free`，作为
未归因的运行稳定性边界保留。该批仅将本地 D2b freshness 标为
`CLOSED_FOR_VALIDATION`；T013-B/T013-C、跨进程兼容和 T016 仍是 `OPEN_FOR_NEXT_BATCH`。
批次证据见 [R6-B9 evidence](evidence/r6-b9-legacy-d2b-freshness-20260909.md)。

### R9-B1 Selection Status Concurrency Ownership 2026-09-09

R6-B9 后续的 ASAN allocator 复现把 D2h212 的间歇性 `double free or corruption` 收敛到
`ServiceProvider::reportSelectionOperationStatus`：多个 Provider worker 并发追加
`memberStatuses` 时，vector 扩容释放旧存储，另一 worker 仍在写旧元素；Face 查询也没有
统一快照锁。R9-B1 增加独立 `m_selectionExecutionStatusMutex`，覆盖 report/update/get 三个
入口，并加入 8×8 并发成员回归。当前 `unit-tests` 188/188、`integration-tests` 118/118
均以 system-first `-j4` 构建通过；D2h212 新鲜进程 50/50、ASAN-preload 20/20 通过。历史
失败仍保留在 R6-B9 目录，不能据此宣称跨进程或 T016 qualification；本批只关闭本地
selection-status ownership 出口。批次证据见 [R9-B1 evidence](evidence/r9-b1-selection-status-concurrency-20260909.md)。

### R10-B1 Native REPO_REF Preparation 2026-09-09

T010 的 native requester 已能编码 `REPO_REF` envelope，但 dispatch 仍在准备边界无条件
拒绝 repository input，导致 provider 已支持的加密大数据引用无法进入同一 C++ 请求链。
本批只修复这一稳定输入边界：`NativeRequestPreparation` 验证加密引用的完整身份并保留
reference，inline 输入继续由 adapter 编码；`NativeInferenceClient::dispatchOperation`
移除错误的“not linked”拒绝。不会在 requester 侧解密数据，也不新增 Python planner 或
改变 Provider 的 fetch/decrypt 权限边界。

分配依据固定为：production entry/caller 是
`NativeInferenceClient::request`→`dispatchOperation`→`NativeRequestPreparation::prepareInput`
及 `NativeRequestEnvelope`；implementation/wire 是 `NativePreparedInput` 的 INLINE/REPO_REF
状态与 v2 envelope；test/oracle 是 `Spec182NativeInferenceClient`、`Spec182Preparation` 和
REPO_REF envelope selector；build/source closure 是 `NativeRequestPreparation.cpp/.hpp`、
`NativeInferenceClient.cpp` 及 `unit-tests` Waf target；migration/evidence 出口是 native
Qwen/YOLO facade 可继续使用 inline，repository caller 获得明确的 C++ preparation route，
真实 encrypted fetch、Provider 两轮和 T016 仍保持开放。

批末必须完成五 lane 静态审查、root-cwd focused selectors 和一次 system-first `-j4`
增量构建；任何 provider/network 未观测不得标为 qualification PASS。

### R10-B2 Native REPO_REF Facade 2026-09-09

R10-B1 opened the C++ preparation boundary for encrypted repository references,
but the public Python SDK still exposes only the inline `request_native_payload`
helper.  This batch adds a thin reference facade that accepts the validated
`LargeDataReference` returned by `publish_application_input_reference`, checks
the journal publication binding, serializes the canonical reference, and calls
the existing C++ `NativeInferenceClient` route.  It does not resolve or decrypt
repository data, invoke the Python planner, or change Provider ownership.

Allocation is intentionally limited to the canonical `APPClient` facade, the
public `InferenceClient` forwarding surface, the Python compatibility selector,
and their source/evidence records.  The independent exit is a mocked binding
boundary that observes `REPOSITORY_REFERENCE` plus canonical JSON and proves
that the native client is called without planner fallback; real encrypted fetch,
Provider execution, caller migration, and T016 remain open for later batches.

### R10-B3 YOLO Native Reference Caller 2026-09-09

The maintained YOLO 2x2 native branch already composes the C++ catalog, grant,
admission, preparation, and split owners, but it still sends its tensor bundle
through `request_native_payload`.  This batch changes that one branch to publish
the same bytes with `publish_application_input_reference` and submit the bound
reference through `request_native_reference`, so the selected Provider can own
encrypted fetch/decrypt.  The ACK-driven Python planner branch and lifecycle
negative cases remain unchanged.

Allocation is limited to `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`
and its maintained-caller source selector.  The independent exit is source and
mock validation that the native branch publishes once, uses `REPO_REF`, and
cannot call `request_native_payload` or `request_task`; real MiniNDN Provider
execution, cross-process behavior, and T016 remain open.

### R10-B4 Qwen Native Reference Caller 2026-09-09

The maintained Qwen native helper uses the same configured `APPClient` and
currently submits every generation payload inline.  This batch publishes each
typed Qwen context bundle as an encrypted repository reference and routes it
through `request_native_reference`, including the existing event observer and
generation options.  Conversation continuation remains fail-closed until a
native conversation owner is configured; no Python planner or requester-side
decrypt path is introduced.

Allocation is limited to `_native_qwen_request`, its CLI route description, and
the maintained-caller source selector.  The independent exit is source and
facade validation that the Qwen native helper uses repository references while
the automatic-planning and legacy routes remain separate.  Real Provider fetch,
streaming/conversation cross-process behavior, caller retirement, and T016
remain open.

### R10-B5 Provider REPO_REF Execution Boundary 2026-09-09

R10-B1 through R10-B4 establish the requester preparation, public facade, and maintained
YOLO/Qwen caller route for encrypted repository references. The remaining local boundary is
Provider consumption: a real `ServiceProvider` handler must receive the v2
`ndnsf-di-request-envelope-v2` with `input_transport=REPO_REF`, fetch the named encrypted
`REQUEST-LARGE` object through `CollaborationContext::fetchEncryptedLargeData`, enforce the
declared plaintext size, and pass the recovered bytes to the production native handler and
runner. This batch adds that integration selector to the existing native ingress fixture; it
does not add requester-side resolution, alter NAC-ABE ownership, or claim cross-process
qualification.

Allocation is limited to the native ingress helper, its existing production C++ runner factory,
and the integration selector that observes the recovered `request-input` scope. They share the
same Provider handler, encrypted large-data transport, `integration-tests` target, and an
independent positive oracle. The stable exit is a successful real Provider execution whose
runner input equals the published plaintext; malformed/missing reference negatives and full
cross-process/T016 qualification remain separate work.

### R10-B6 Provider REPO_REF Fail-Closed Negatives 2026-09-09

R10-B5 proves the positive Provider fetch/decrypt path. This batch exercises the same production
handler with three invalid v2 reference envelopes: a missing object, an incorrect declared
plaintext size, and malformed JSON. Each case must fail at the Provider fetch/parser boundary,
publish no successful response, and expose its own deterministic reason. The runner must not be
entered for any negative case. The missing-object fixture scopes
`NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS=1000` with an RAII guard so the production fallback remains
unchanged while the local oracle completes before the fixture's three-second event-loop pump.

Allocation remains within `runNativeIngressCase`, the existing native ingress runner observer,
the scoped test timeout guard, and one `Spec170NativePostSelection` selector family. The cases
share the same encrypted input fixture, handler, Waf target, and failure oracle; cross-process
behavior and final T016 qualification remain outside this batch.

### R10-B7 Caller Route Contract Synchronization 2026-09-09

R10-B3 and R10-B4 moved maintained YOLO/Qwen native callers from inline payload submission to
encrypted repository references, but one Qwen configuration log marker and the T013 execution-unit
description still named `request_native_payload`. This batch synchronizes the current caller
contract and emitted route marker with `publish_application_input_reference` plus
`request_native_reference`, while preserving the R5 historical evidence and the explicit legacy
ACK-driven route. No native requester, Provider, or wire implementation changes here.

Allocation is limited to the maintained Qwen log marker, the T013-D/T013-F execution-unit text,
the task registry, and one documentation evidence record. The independent exit is source/route
validation, `py_compile`, and design-link checks; real maintained caller execution, cross-process
behavior, legacy retirement, and T016 remain open.

### R10-B8 Cross-Task Audit Status Refresh 2026-09-09

The source-alignment audit still described the pre-R10 paused state and named the superseded inline
route as current. This documentation batch refreshes `audit.md` to the `7251f9ca` checkpoint,
records the R10-B1--R10-B7 local exits, and restates the remaining T004/T008/T010/T011/T013/T016
production boundaries. Historical audit sections retain their original dates and evidence; only
the current finding and convergence summary are updated.

Allocation is limited to `audit.md`, its task/progress entry, and one evidence record. The
independent exit is source/status/link consistency plus the design validator. No product code,
wire contract, task completion claim, or external qualification result changes.

### R10-B9 Requester REPO_REF Core-Wire Boundary 2026-09-09

The existing R4-B6 real-Provider conversation fixture used inline application input, while
R10-B5/B6 exercised Provider repository ingress through a separate manual request path. This
batch adds one sibling selector that constructs a canonical `NativeApplicationInput::RepositoryReference`
from the native publisher metadata and drives the same configured requester/Core/Provider
conversation. The Provider ACK oracle checks the exact `REPO_REF` transport, empty inline payload,
and published data/manifest identity.

Allocation is limited to the integration fixture, selector registration, task/progress row and one
evidence record. Provider fetch/decrypt remains owned by the R10-B5/B6 boundaries; cross-process
maintained-caller execution and T016 qualification remain open. The first test-oracle iterator
boundary is retained in `docs/failure-log.md` and does not count as a protocol failure.

### R10-B10 Current Audit Checkpoint Refresh 2026-09-09

After R10-B9, the source-alignment audit is refreshed to the `9f80a1ce` implementation/docs
checkpoint so its current finding names the new requester `REPO_REF` Core-wire boundary. The
remaining chain now explicitly includes Provider fetch/decrypt and keeps maintained caller
execution, cross-process behavior, and T016 qualification open. Historical dated findings remain
unchanged.

Allocation is limited to `audit.md`, the task/progress registry, and this evidence record. The
independent exit is link/status consistency plus the design validator; no product or qualification
status is promoted.

### R10-B11 Requester REPO_REF Provider Fetch 2026-09-09

The R10-B9 requester/Core-wire selector is extended at the same stable exit so its real
`ServiceProvider` collaboration handler resolves the emitted reference through
`CollaborationContext::fetchEncryptedLargeData` and compares the recovered bytes with the native
publisher's plaintext. The inline R4-B6 conversation selector remains a regression case.

Allocation is limited to the existing integration fixture and its evidence/progress records.
R10-B5/B6 continue to own the independent malformed/missing/size-mismatch negatives; cross-process
maintained-caller execution and T016 qualification remain open.

### R10-B12 Current Audit Refresh 2026-09-09

After R10-B11, the source-alignment audit is refreshed to the `0656c2e4` checkpoint. It records
the observed one-process requester → Core → Provider fetch/decrypt → conversation result boundary
and keeps the remaining maintained-caller cross-process execution, stream/recovery qualification,
legacy zero-use and T016 ownership explicit. Historical audit findings remain dated and unchanged.

Allocation is limited to `audit.md`, the task/progress registry and one evidence record. The
independent exit is link/status consistency plus the design validator; no product or qualification
status is promoted.

### R10-B13 Integration Recipe Oracle Repair 2026-09-09

The first full integration run after R10-B11 found four `Spec175NativeAssembly` failures at the
worker recipe boundary. The test helper independently sorted input/output names, while the
production canonical serializer binds names to contract order. This batch aligns the helper with
the production contract, records the first failure in `docs/failure-log.md`, and revalidates the
affected integration suite with a lower-concurrency rebuild after observed host swap-in.

Allocation is limited to the integration test helper, failure index, task/progress record and one
evidence file. No production serializer or protocol behavior changes; T016 qualification remains
open until the complete same-source matrix passes.

### R10-B14 Same-Source Unit and Integration Validation 2026-09-09

After the R10-B13 fixture repair, the complete `unit-tests` suite and complete `integration-tests`
suite are rerun from the same `build-nac182` source closure. This batch records the full green
result and host resource observations, while keeping MiniNDN/no-Python, maintained-caller
cross-process execution and T016 qualification separate because the campaign owner still lacks
node/netns metadata.

Allocation is limited to the audit, task/progress registry and one evidence record. No task is
promoted to final qualification solely from unit/integration success.

### R10-B15 T016 MiniNDN Owner Preflight Recheck 2026-09-09

After local NFD was started and `/run/nfd/nfd.sock` became available, the T016 campaign owner was
retried from a fresh output directory with `campaignCase=I01`. NFD status succeeded, but the
owner still returned `UNQUALIFIED` / `MININDN_NODE_CONTEXT_NOT_PROVIDED` before starting any
MiniNDN node, namespace, child process, or protocol request. The root cause is now narrower:
the campaign owner has no real node/netns metadata to pass to the canonical closure runner;
NFD socket availability alone is insufficient.

Allocation is limited to a fresh preflight evidence record, failure index, audit, and task/plan
status. No product source, test target, or qualification status is promoted. The next owner work
must create and validate an isolated MiniNDN node context (namespace inode, owner PID/start ticks,
NFD socket and peer metadata) before invoking the existing runner; until then T016 remains
`OPEN_FOR_NEXT_BATCH` / `UNQUALIFIED`.

### R10-B16 Native Closure Node Context Boundary 2026-09-09

The T016 retry showed that the canonical runner accepts a `nodes` parameter in its internal
signature but does not yet validate MiniNDN node identity or enter the supplied network namespace.
This batch adds the frozen node-context preflight (namespace inode, owner PID/start ticks, NFD
socket and peer metadata) and passes a held namespace descriptor through `nsenter` before the
existing bubblewrap/strace launch. Cases without a declared node remain available to the local
harness tests; a qualification case with a declared node fails closed when context is missing or
stale.

Allocation is limited to `tests/standalone/run-spec182-native-closure.py`, its Python harness tests,
the task/plan/evidence record and the runner's standalone documentation. It does not invent the
missing DI qualification cases or change the MiniNDN owner topology. The stable exit is a tested
preflight/launch command that either carries a validated held namespace FD or returns an explicit
`UNQUALIFIED` boundary.

R10-B16 completed this bounded runner boundary in local checkpoint `9a50ab3d`. The implementation
validates node/process binding, namespace inode, owner PID/start ticks, NFD socket type and peer
metadata; it holds the namespace FD across `nsenter` launch and closes it on all launch/timeout
paths. Twenty-three focused Python cases, bytecode compilation and `git diff --check` passed. At
that checkpoint the owner still supplied no real MiniNDN context; R10-B17 now provides the context
producer, while topology-driven business cases, cross-process execution and T016 qualification
remain open.

### R10-B17 MiniNDN Owner Context Producer 2026-09-09

R10-B17 adds an explicit `--execute-owner` mode to the campaign owner and a tracked two-node
requester/provider topology. It starts MiniNDN and per-node NFD applications, waits for their real
filesystem sockets, and exports `/proc` namespace inode, owner PID/start ticks, socket and peer
metadata through one `collect_node_context` function. The owner keeps the existing registration-only
mode and stops at `UNQUALIFIED` when the manifest has no executable closure artifact/process case.

The bounded exit was observed in root run `20260909050945`: both node contexts were written, then
`NATIVE_CLOSURE_CASE_DEFINITION_MISSING` returned exit 2 and the network was cleaned up. The next
batch must freeze runner-compatible cases and invoke the canonical runner before the owner teardown;
this batch does not claim any I/PO or no-Python qualification.

### R10-B18 Runner Native ELF and Trace Integrity Boundary 2026-09-09

The first dynamic-ELF runner probes exposed two harness defects: relative tool names were invisible
under the intentionally minimal environment, and shared libraries mounted only below `/probe-root`
could not satisfy absolute ELF interpreter paths. A third probe showed that normal strace
`<unfinished ...>`/`<... resumed>` pairs were being treated as incomplete observations. R10-B18
mounts each declared shared library at its canonical absolute target and tracks unfinished/resumed
events per PID, preserving `UNQUALIFIED` for genuinely incomplete traces.

The repaired root probe (`.codex-tmp/spec182-runner-probe4-20260909051727/`) executed `/bin/true`
with return code 0 and a complete trace. It intentionally lacks business evidence, so this batch
closes only the native process/observation boundary; an executable DI case and owner-alive runner
invocation remain required for T016.

### R10-B19 Owner-to-Runner Handoff 2026-09-09

R10-B19 connects the explicit MiniNDN owner mode to the canonical closure runner while the
requester/provider namespaces and NFD applications are still alive. The owner passes the
identity-bound node context into `run_case`, persists the runner observation/evaluation, and maps
`PASS`/`FAIL`/`UNQUALIFIED` to the campaign exit boundary before the `finally` cleanup. The
registration-only mode and canonical runner remain separate; no second collector or business
oracle is introduced.

The fresh root run `.codex-tmp/spec182-r10-b19-20260909052040/` used an executable `/bin/true`
isolation probe. It handed a held namespace descriptor through `nsenter`, returned code 0 from the
process with a complete trace and no integrity/policy violations, then correctly classified the
case as `UNQUALIFIED` because the probe has no DI business evidence. Twenty-nine focused cases,
`py_compile`, `git diff --check` and the design validator passed. This batch closes only the
owner-to-runner composition boundary; executable native DI cases, maintained caller execution,
cross-process/no-Python evidence and T016 qualification remain open.

### R10-B20 Collector Evidence Boundary 2026-09-09

R10-B20 addresses the remaining T014-A harness gap exposed by R10-B19: the canonical runner
observed a successful native process and complete trace but returned no derived evidence, so every
composed case was necessarily `UNQUALIFIED`. The collector now derives identity, process-tree,
namespace, exec-map, endpoint and cleanup evidence only from the actual run/trace records. A case
may additionally declare an independent stdout business marker; the marker can satisfy only
`business-oracle` and cannot promote a protocol or T016 result.

The stable boundary is fail-closed: missing trace, incomplete syscall pairing, timeout, policy
violation or missing declared marker remains `UNQUALIFIED`/`FAIL` according to the existing
evaluator rules. This batch is Python collector/harness work and has no native build lane; real DI
business requests, maintained caller execution, no-Python proof and T016 qualification remain
downstream obligations.

### R10-B21 I01 Native Consumer Positive Case 2026-09-09

R10-B21 runs the first real native C++ consumer through the owner-alive composition. A manifest
generated from the current `build-nac182/spec182-installed-consumer` and its resolved ELF closure
declares the executable, shared libraries, absolute tools, requester node and an independent
stdout marker. MiniNDN keeps requester/provider namespaces and NFD applications alive while the
canonical runner stages, launches and evaluates I01.

The fresh run `.codex-tmp/spec182-r10-b21-native-consumer-owner/` returned process code 0 and
evaluator `PASS`: trace completeness and integrity/policy checks passed, all seven required evidence
classes were present, and `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK` matched. This is a native
installed-library consumer/closure proof and does not exercise a requester/provider DI request,
I02--I08 counterexamples, maintained callers or the T016 qualification matrix; those remain open.

### R10-B22 Native DI Business Case 2026-09-09

R10-B22 extends the owner-alive runner with an executable native DI business case. The existing
`Spec182R4B6RealProviderConversation` integration selector is the production C++ requester/provider
fixture: it constructs a configured `NativeRequestRuntime`, submits two native requests through the
real `NativeInferenceClient`, services them through `ServiceProvider` production ingress, and checks
the first and continuation results. A marker is emitted only after both result assertions succeed;
the runner treats that marker as an independent business oracle alongside complete process and
namespace observation.

The batch boundary is the isolated-process native DI request/result observation. The marker change
was rebuilt in 37.778 seconds with the repository system-first toolchain and default `-j4`; the
direct selector passed in 6.801 seconds. Four fresh owner/runner directories preserve the first
manifest and ELF closure failures; the corrected closure reached the test process, which then
failed at setup because the relative `examples/trust-any.conf` file is outside the minimal root
(`returncode=201`). This runtime/config boundary is recorded as a miss for the next runner batch.
The result does not claim true multi-process requester/provider transport, maintained Python caller
migration, I02--I08/PO completion, or T016 qualification. The evidence record preserves the exact
target, source/ELF closure, elapsed time, owner/runner commands, five-lane review matrix, and four
retrospective miss categories.

### R10-B23 Runner Working Directory and Config Boundary 2026-09-09

R10-B23 repairs the first runtime boundary observed after the native DI binary reached the isolated
runner: the integration fixture loads the relative `examples/trust-any.conf`, while the minimal root
deliberately hides the host working tree. The runner gains an optional process `workingDirectory`
field constrained to `/probe-root` descendants or `/tmp`; the manifest declares the trust config as a
data artifact under `/probe-root/examples/`, and the launch command changes directory into the staged
root before starting the test. No host path, arbitrary cwd, or environment-based config injection is
accepted.

The stable exit is a fresh owner/runner `PO-001` execution that reaches the existing native DI
selector with complete trace and the independent business marker. This harness repair has no native
build lane; it must pass its focused Python tests and `py_compile` after the read-only review gate.
That exit passed: 33 focused Python cases, `py_compile`, design validation and the official
`review-agent` re-review passed; the fresh owner/runner output records returncode `0`, complete
trace, all seven evidence classes and `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` after staging
`examples/trust-any.conf` and changing into `/probe-root`. The result remains an isolated-process
business observation and does not close multi-process transport, maintained callers, I02--I08 or
T016 qualification.

### R10-B24 Spec182 Native Suite Baseline 2026-09-09

After R10-B23 closed the runner working-directory/config boundary, the existing `build-nac182`
native binaries were rerun with exact Spec182 selectors. The Spec182 unit selection passed 247
cases, the Spec182 integration selection passed 2 cases, the named conversation/replacement
selection passed 3 cases, and the repository-reference selector passed 1 case. No native source changed, so no
rebuild was performed; raw logs are retained under `.codex-tmp/spec182-r10-b24-native-suite-20260909/`.

The second `vmstat` sample showed `si=35780` and `so=0`. This is recorded as resource pressure,
not as a product failure; if swap-in persists, the next native build follows the documented `-j2`
fallback. The baseline confirms local C++ behavior only and leaves maintained callers, true
requester/provider process transport, I02--I08, legacy retirement, no-Python proof and T016
qualification open.

### D-SKILL-CLI-BOUNDARY CLI and Harness Evidence Boundary 2026-09-09

本次流程复盘把命令级证据单独定界：`--help`、usage/schema rejection、可执行文件存在、
target/link smoke 或 harness 启动只证明命令、构建接线或外部设施边界，不能写成 native
request/result、Python/C++ parity 或 qualification PASS。共享 `batch-quality-gates`、
`speckit-code-design`、plan/tasks templates 和 `skills/README.md` 已同步该规则，个人
安装入口按 SHA-256 对照。此修改不改变产品契约、调用链或现有任务状态。

分配依据是文档规则、批次结果字段和模板示例共享同一 reference；五 lane 对文档变更均
记录为 `N/A` 并说明理由。独立出口是定向链接/格式检查、Spec validator 和安装副本 hash
一致性；真实 C++ 请求和 T016 资格仍由产品任务负责。

### R10-B25 DI_NativeRequester Build and CLI Boundary 2026-09-09

R10-B24 建立了现有 native suite 基线，但当前 `build-nac182` 尚未包含独立
`DI_NativeRequester` 可执行文件。本批只重建已登记的 `examples/DI_NativeRequester.cpp`
Waf target，并执行 `--help`、usage 和错误 schema 的命令级检查。由于 R10-B24 的
`vmstat` 观察出现持续换页，按资源策略使用 `-j2`；不并入 unit/integration 全量构建，
也不把 CLI smoke 写成真实 Core/Provider 请求结果。

分配依据固定为同一 executable target、同一 source closure、同一 CLI oracle 和同一
构建出口。独立出口是 target 成功链接、CLI 解析边界可观察且错误配置 fail-closed；真实
模型、Provider、跨进程请求和 T016 资格仍保持开放。

本批已按登记范围完成：修正静态检查脚本后，`DI_NativeRequester` 目标使用 system-first
环境和 `-j2` 构建成功（Waf 报告 `1m17.740s`），`ldd` 无缺失依赖；`--help` 返回 0，
空参数返回 2，错误 schema 返回 1 且不产生输出文件。构建实际输出为
`.codex-tmp/spec182-r4-b2/build/examples/DI_NativeRequester`。第二次 `vmstat` 样本有
`si=372`、`so=0`，下一次 native build 继续采用 `-j2`，除非新的资源观察证明可升档。
这些结果只关闭 executable/CLI wiring boundary，不关闭真实 Core/Provider 请求、维护调用方、
跨进程、I02--I08、no-Python 或 T016；详见 [R10-B25 evidence](evidence/r10-b25-native-requester-cli-20260909.md)。

### R10-B26 Missing REPO_REF Negative Recheck 2026-09-09

R10-B6 的首轮缺失对象负例曾因默认 30 秒 fetch budget 超过 fixture 的 3 秒 pump 而停在
测试边界；修复后的 source 已包含 1 秒、RAII 恢复的测试作用域预算。本批不重建 binary，
直接用现有 `integration-tests` 的 `ProductionIngressRejectsMissingNativeRepositoryReference`
selector 复核该首失败边界。selector 进入真实 Provider handler，返回缺失对象 failure，
没有 runner input 或成功 Response，exit 0（testing time 1.946850s）。这只关闭本地负例
重试证据，不关闭跨进程、maintained caller 或 T016；详见 [R10-B26 evidence](evidence/r10-b26-missing-repo-ref-recheck-20260909.md)。

### R10-B27 Native C++ Test Ownership Skill Sync 2026-09-09

根据 R4-B4/R3-B1 的流程复盘和用户对 NDNSF-DI 测试语言的要求，共享
`batch-quality-gates.md` 现明确：凡断言 native runtime、protocol、state、concurrency、
crypto 或 model 行为的 unit/integration/regression 测试，其 fixture/driver/oracle 必须
用 C++ 实现并直接调用生产 C++ target；Python 只能编排外部设施、启动 C++ executable，
或覆盖 binding/facade、offline oracle 和配置拒绝。`speckit-code-design`、README 及
spec/tasks templates 已同步该措辞，installed shared skill copy 与 versioned source
SHA 一致。详见 [R10-B27 evidence](evidence/r10-b27-native-cpp-test-ownership-20260909.md)。

本批是文档/技能边界的 `CLOSED_FOR_VALIDATION`，不改变任何产品任务状态，也不把已有
Python focused、CLI smoke、局部 C++ selector 或 T016 preflight 提升为 native qualification。

### R10-B28 Workflow Authority Alignment 2026-09-09

当前工作流总览曾把 GSD Core 写成默认四道强制门，与 constitution 1.5.0 已采用的 Spec Kit
`Execution Progress`/持久证据规则不一致。现已修正 `docs/agentic_workflow.md` 和本机
`CLAUDE.md`：Context Mode、CodeGraph、Spec Kit 是默认门；GSD 仅在长时或需要其 phase/state
模型时使用，ARS 只在研究范围适用时使用。总览同时引用共享 `batch-quality-gates.md`，明确
逐任务 review-agent 静态门、批末组合审查、四类漏检复盘和 NDNSF-DI native C++
fixture/driver/oracle ownership；`AGENTS.md` 也已加入同一执行契约，并保留原有 `-j4` 与增量
构建规则。

分配依据是同一工作流权威、无产品源码或 target 变化、独立出口为当前入口文档对必需/可选
工具和共享批次契约的一致描述；本批 `CLOSED_FOR_VALIDATION` 只适用于文档规则，不改变
T004/T008/T010/T011/T013/T016/T017 状态，也不替代 review-agent 实际执行或产品资格证据。
详见 [R10-B28 evidence](evidence/r10-b28-workflow-authority-alignment-20260909.md)。

### R10-B29 Runner Multi-Process Lifecycle 2026-09-09

T016 的 canonical runner 已可校验多进程 manifest，却此前只启动第一个 process，且没有
传递每个 process 的显式 NDN 配置环境。该批按 native-isolation-design 的 process/node
contract 收敛这一 harness 边界：`make_launch` 可按声明 ID 生成命令；`run_case` 为每个
requester/provider 等业务进程核对并持有 node namespace FD，使用显式环境，加入同一
supervisor process group，按统一 run/cleanup deadline 进行 TERM→KILL，并保留每进程
trace/output/returncode 后归并 canonical inputs。collector 对共享 executable 只有在每个
声明进程都有成功 exec 时才报告完整 role coverage，避免一个 requester exec 覆盖缺失
provider。新增三个 Python harness fixtures；36 个 `test_spec182_native_closure.py` cases、
`py_compile` 和 `git diff --check` 通过。没有 native build 或 MiniNDN 运行；真实
requester/provider transport、I02--I08、maintained caller/no-Python 和 T016 仍开放。

本批在 T014-A/B 范围内 `CLOSED_FOR_VALIDATION`，不改变 T014/T016 的 `PARTIAL` 状态。
官方 `review-agent` 的五 lane 只读审查没有留下 P1/P2/P3；证据见
[R10-B29 evidence](evidence/r10-b29-runner-multiprocess-lifecycle-20260909.md)。

### R10-B30 Runner Child and Endpoint Observation 2026-09-09

R10-B29 接通了每个声明业务进程的启动、环境、namespace FD 和统一 supervisor cleanup；本批
继续补齐其 T014 观察契约。`load_case` 现在要求 `assembly-worker` child 绑定已声明
Provider、并校验并发上限；endpoint 必须声明 owner/peer、transport、地址和用途，抽象
UNIX 地址在 launch 前拒绝。`collect_trace` 保留 clone/fork/vfork、open/openat、close/dup、
mmap/mprotect/munmap、socket/connect 等冻结系统调用，并把成功 connect 与 manifest 地址
逐项核对，未声明 endpoint 仍为 policy violation。新增 child/endpoint 正负夹具和生命周期
trace 检查；40 个 `test_spec182_native_closure.py` cases、`py_compile`、设计 validator 与
`git diff --check` 通过。没有 native build 或 MiniNDN 运行，真实 worker parentage、endpoint
injection、I02--I08、maintained caller/no-Python 与 T016 仍开放。

本批只在 T014-A/B harness 语义范围 `CLOSED_FOR_VALIDATION`，不改变 T014/T016 的 `PARTIAL`
状态；详见 [R10-B30 evidence](evidence/r10-b30-runner-child-endpoint-observation-20260909.md)。

### R10-B31 Native Client Unary Provider Request 2026-09-09

R4-B6 已经证明 streaming/conversation requester 能够通过真实 Core/Provider fixture 完成
两轮请求，但 T010-B 仍缺少不带 generation、stream 或 conversation state 的普通请求出口。
本批复用同一 native catalog、grant/admission、placement 和 Provider transport setup，为
`runR4B6RealProviderConversationCase` 增加 unary 分支：runtime 使用 `TOKEN_DIAGNOSTIC`，
`NativeInferenceClient` 不设置 stream/conversation，Provider callback 通过真实
`CollaborationContext::publishFinalResponse` 返回结果。新增的
`Spec182R10B31RealProviderUnaryRequest` selector 验证 ACK、Core commit、Provider callback、
最终 Response 和结果 payload；没有修改协议或 Python facade，也没有宣称跨进程资格。

本批 `integration-tests` 以 system-first `-j4` 增量构建成功（35.584s）；unary selector
通过（3.559s），R4-B6 conversation/replacement 三例通过（20.366s），repository-reference
回归通过（6.939s）。官方 `review-agent` 五 lane 只读审查没有发现 P1/P2/P3；该批只关闭
T010-B 的 bounded native-client unary 观察，Provider worker/业务执行、maintained caller、
跨进程 transport、T016 与完整请求矩阵仍开放。详见
[R10-B31 evidence](evidence/r10-b31-native-client-unary-request-20260909.md)。

### R10-B32 Shared Spec Kit Skill Feedback Loop 2026-09-09

R4-B4/R3-B1 的复盘显示，静态审查虽然已要求检查 caller、测试/harness 和 build/source
closure，但执行记录仍可能只写 `No findings`，而编译/运行漏检后的重试也容易变成同一
命令的重复运行。本批不改产品代码，补强共享 `review-agent` reference 的 Static Gate
Release Checklist：写入 `STATIC_PASS` 前必须逐 lane 落实真实查询或检查；若此前存在
编译/链接或运行/测试漏检，必须记录 `Changed gate` 并说明它如何覆盖首个失败边界。
批次结果同时显式记录 `Batch growth decision`，在稳定出口出现后，下一入口、状态机、
selector、source closure 或硬验收依赖必须拆到新的 Batch ID。

更新范围是版本化 `skills/speckit-code-design`、Spec Kit plan/tasks 模板、`skills/README.md`
及同步的个人安装副本；文档校验、引用扫描和 SHA-256 对照通过，没有 native build 或
runtime test。该批 `CLOSED_FOR_VALIDATION` 只适用于共享 skill/template 规则，T004/T008/
T010/T011/T013/T014/T015/T016/T017 及资格状态不变。详见
[R10-B32 evidence](evidence/r10-b32-skill-feedback-loop-20260909.md)。

### R10-B34 Spec182 Regression Sweep 2026-09-09

在 R10-B33 后执行一次不改产品源码的批末回归：Spec182 专属 C++ unit、完整
`Spec170NdnsfDiCoreFlow/*` integration 和 Python binding/compatibility suite 均退出 0。
这次运行只证明当前本地边界没有回归；日志中的预期 negative case 保持其原始
`ACK_CLOSED`/stream failure boundary，不把断言内失败解释为套件失败或资格通过。
跨进程 Provider worker、maintained caller/no-Python 与 T016 仍需在其 owner 环境执行。
详见 [R10-B34 evidence](evidence/r10-b34-regression-sweep-20260909.md)。

### R10-B35 Spec Kit Command Output Contract 2026-09-09

R10-B34 复盘确认，当前共享规则已经要求五 lane、漏检分类和批次增长判断，但入口
skill 仍可能只引用规则而不生成这些字段。本批把入口输出契约写入版本化
`batch-quality-gates` reference，并在 code-design skill 与本机 Spec Kit 入口副本中明确：
编辑前登记 `Batch growth decision`，审查记录必须列真实 caller、测试注册和 source
closure，批末同一记录必须包含四类 `Batch Retrospective`、可比构建测量和关闭决定；重试
必须登记 `Changed gate`。该规则不改变产品接口或验收依赖。

更新范围是共享 reference、`skills/README.md`、版本化 code-design skill、安装副本和
本机 `.agents/skills/speckit-*` 入口副本；通过定向文本/引用/SHA 检查后，保持本批
`CLOSED_FOR_VALIDATION` 仅适用于工作流输出契约。T004/T008/T010/T011/T013/T014/T015/
T016/T017 和 native qualification 状态不变。详见 [R10-B35 evidence](evidence/r10-b35-command-output-contract-20260909.md)。

### R10-B36 Remaining Production Chain Reorder 2026-09-09

暂停新增实现期间，按“真实结果出口”重排剩余生产链；这张表只改变执行优先级，不改变
17 个父任务、既有依赖或验收门。每一行都必须在自己的 Batch ID 中完成静态五 lane、C++
selector/source closure 和真实结果观察，未达到出口继续保持 `PARTIAL`。

| Order | Existing cards | Stable production exit | Depends / split trigger |
| --- | --- | --- | --- |
| P1 | `T004-A`, `T008-A/B`, `T010-A/B` | 一个 native requester 从准备/授权进入真实 Core，并从 Provider 得到 unary/stream 结果；保留 cancel/deadline/terminal 负例 | 复用现有 R10-B31/R10-B33 作为局部基线；增加 Provider worker、caller 或新 selector 即拆新批 |
| P2 | `T010-C`, `T011-C` | 同一请求契约跨 Provider worker/进程完成首轮与续接，含 receipt/control/commit、恢复和清理 | 依赖 P1 的请求边界；需要 NFD/MiniNDN 或跨进程 transport 时在 owner 环境执行，不用本地 fixture 替代 |
| P3 | `T012-A/B`, `T013-A`, `T013-F` | 一个维护中的 YOLO caller 使用 native facade/`REPO_REF` 完成真实请求、结果回收和 rollback evidence | 依赖 P1/P2 与 binding source closure；Python source/compatibility 检查不能单独关闭 caller |
| P4 | `T013-C`, `T013-D`, `T013-E`, `T011-C` | Qwen/streaming caller 通过 native observer 交付 token/terminal 顺序；配置 continuation owner 后完成跨进程两轮和 replacement | 依赖 P2；conversation metadata 不由 Python 猜测，缺 owner 时继续 fail-closed |
| P5 | `T013-B` | maintained callers 零使用旧 runtime/default import graph，并保留兼容退出与回滚证据 | 仅在 P3/P4 各有真实 native 结果后执行；legacy manifest 不能替代零调用观察 |
| P6 | `T014-A/B` | 隔离 collector/harness 观察 native scope、子进程、endpoint、清理和必要反例 | 依赖 P5；I02–I08 与真实 no-Python 反例由 T016 owner 执行 |
| P7 | `T015-A` → `T016-A` → `T017-A` | 跨任务收敛 PASS 后，完成 unit→integration→MiniNDN/no-Python 资格，再生成唯一 handoff | 每一步都保留首个失败边界；T016 缺 NFD/node context 时保持 `UNQUALIFIED`，不前移 T017 |

P1–P4 是四个不同的生产入口/进程边界/selector，不能为了少一次构建合并；P5–P7 只在
前置真实结果闭合后推进。该顺序与父任务依赖兼容，未将任何局部 fixture、CLI smoke 或
Python compatibility PASS 提升为 native qualification。详见 [R10-B36 evidence](evidence/r10-b36-production-chain-reorder-20260909.md)。

### R10-B37 Native Client Streaming Provider Request 2026-09-09

P1 的下一个稳定出口是把已有 native requester/Core/Provider 单进程链扩展到**无会话
的真实流式请求**。本批只在 R4-B6 真实 Provider fixture 中增加一个明确的 stream-only
分支：请求仍经过 preparation、grant/admission、Core commit 和 Provider callback，
Provider 发布 `GenerationTokenEventV1` 与 `NDNSF-DI-FINAL-V1`，requester 由原生
`acceptGenerationEvent`/`validateGenerationFinal` 验证并返回结果；不伪造 conversation
binding，也不宣称跨进程或 maintained caller 已迁移。

该批次与已有 unary、conversation、replacement selector 分开登记，因为 caller 选项、
状态契约和结果 oracle 不同。静态门必须覆盖 helper 分支、真实 Provider callback、
stream/final schema、selector 注册和 integration target source closure；批末只运行该
selector及同 helper 的相关回归。若运行暴露 Provider worker、跨进程或 NFD 依赖，保留首个
失败边界并拆到 P2/T016 owner，不扩大本批职责。详见 [R10-B37 evidence](evidence/r10-b37-native-client-streaming-request-20260909.md)。

### R10-B38 T016 Runtime Context Recheck 2026-09-09

R10-B37 后对 T016 重新执行启动前 preflight，使用新的 raw run 目录。主机现在能看到
`/run/nfd/nfd.sock` 和运行中的系统 `nfd`，但这不等同于 MiniNDN owner context：默认
campaign 仍明确返回 `MININDN_NODE_CONTEXT_NOT_PROVIDED`；显式 `--execute-owner` 又在
非 root 边界返回 `MININDN_REQUIRES_ROOT`。本批只记录这两个真实首拒绝边界，不启动业务
进程、不修改 qualification manifest，也不把 NFD socket 的存在提升为 T016 PASS。后续必须
由 root MiniNDN owner 提供 requester/provider 的 namespace、PID starttime、独立 NFD
socket 和 peer metadata，再运行完整 I01–I08/PO matrix。详见 [R10-B38 evidence](evidence/r10-b38-t016-runtime-context-recheck-20260909.md)。

### R10-B39 R4-B6 Fixture Contract Guard 2026-09-09

R10-B37 提交后的只读复核发现，R4-B6 handler 把 `failFirst` replacement 注入放在
conversation binding 检查之前；因此一个缺少 `conversationTurnBinding` 的首轮负例可能
直接进入预期的 ProviderFailure，掩盖测试夹具契约破坏。本批只移动该检查顺序：
conversation 请求必须先验证 binding，stream-only 请求仍明确不要求 binding；不改变生产
代码、wire 或请求状态机。按共享规则记录 `Changed gate`，重建 integration target 并重跑
stream-only、conversation/replacement、unary/repository selectors。详见 [R10-B39 evidence](evidence/r10-b39-r4b6-fixture-contract-guard-20260909.md)。

### R10-B40 T016 PO-001 Native Owner/Runner Pass 2026-09-09

R10-B38 确认当前会话 UID 不能直接进入 MiniNDN owner 后，使用 passwordless root owner
重新执行一个 bounded qualification case。registration manifest 增加 `campaignCase=PO-001`；
旧 raw runner manifest 的两个首拒绝边界（缺 `process.role`、integration executable digest
过期）保留在 `r10`/`r11`，fresh `r12` manifest 只修正这两个可核对身份字段。root owner 成功
创建 requester/provider namespace、NFD socket 和 peer context，canonical runner 在 owner
存活期间启动 native `integration-tests`，PO-001 业务 marker、process/namespace/trace/cleanup
均通过并返回 `PASS`。此处的独立边界是 MiniNDN owner 与被测 native 进程；Provider callback
仍由同一 integration fixture 提供，因此不把本批描述为 requester/Provider 独立进程传输。
这只关闭一个真实 isolated native-process acceptance 出口；I01-I08、PO-002-PO-014、
maintained caller/no-Python 和完整 T016 仍需逐 case 执行。详见 [R10-B40 evidence](evidence/r10-b40-t016-po001-native-owner-pass-20260909.md)。

### R10-B33 Native Unary Repository Reference Request 2026-09-09

R10-B31 已补齐不带 stream 或 conversation state 的普通 native `Response`，R10-B11
已验证 streaming `REPO_REF` 的 Provider fetch/decrypt 边界；本批把两者组合为一个普通
请求出口。复用 `runR4B6RealProviderConversationCase` 的真实 catalog、grant/admission、
preparation、Core commit 和 Provider handler，只把 `repositoryInput=true` 与
`unaryRequest=true` 同时打开，并新增具名 selector。这样可以观察 native requester 产生的
v2 `REPO_REF` 在非 streaming 请求中的完整恢复/消费，不把 repository publication 当作
成功标志，也不引入新的 Python 路径。

本批新增一项 C++ integration selector，按默认 system-first `-j4` 只重建
`integration-tests`；静态门逐项检查 helper 分支、Provider fetch、结果断言、selector
注册和 target/source closure。稳定出口仅是单进程普通 `REPO_REF` 请求；Provider worker、
跨进程 transport、maintained caller/no-Python 和 T016 qualification 仍由后续批次负责。

实现和验证已完成：`integration-tests` system-first `-j4` 构建 exit 0（36.514 s），新
selector 通过（3 assertions，3.510 s），conversation、replacement、alternate replacement、
streaming repository reference 与 unary inline 回归共 6 cases 全部通过。首次未带 suite
前缀的过滤器只触发 Boost.Test no-match setup code 200，随后按 `--list_content` 注册名重跑；
这不是产品失败。官方 `review-agent` 五 lane 静态审查无 P1/P2/P3。该批
`CLOSED_FOR_VALIDATION` 仅适用于单进程 unary `REPO_REF` 边界，详见
[R10-B33 evidence](evidence/r10-b33-native-unary-repository-reference-20260909.md)。

Batch growth decision：该边界已有稳定出口，下一批不得继续把 Provider worker、跨进程
transport 或 maintained caller 迁移并入本批；这些改变调用方/进程生命周期/资格依赖，必须
使用新的 Batch ID，并各自登记 C++ fixture、selector、source closure 和真实验收出口。

### R8-SKILL Review Coverage Contract 2026-09-09

本轮根据 R4-B4/R3-B1 的流程复盘，补强可复用的 Spec Kit skill，而不是改变产品契约或
重做 Spec182。`review-agent.md` 新增固定的 Minimum Review Record：每次小任务与批末组合
审查必须逐行记录 `production entry/callers`、`implementation and wire`、`test/harness/oracle`、
`build/source closure`、`migration/evidence` 的实际文件/符号与查询命令；缺少测试注册、
source closure 或未解释的 `gap` 时不得产生 `STATIC_PASS`。plan/tasks 模板同步要求这张表和
四类 `Batch Retrospective`。本机 `speckit-analyze`、`speckit-taskstoissues`、
`speckit-implement`、`speckit-constitution` 副本也已同步入口说明；同步只改变审查记录格式，
不改变现有任务状态或把局部 C++/Python 测试提升为资格验收。

T012-A 的候选 ABI 观察项已单独记录为 `PARTIAL`：显式候选 Core/DI、NAC-ABE 与 SVS
依赖下 extension 导入和 21 个 focused Python cases 通过，但默认 requester 的完整
native preparation/offer-admission 构造、C++/Python parity、caller migration 及最终
qualification 仍是后续 T012-B/T013/T016 的出口。该批次只复用 ABI 证据，不把 focused
binding PASS 提升为生产调用链完成。

本次复盘进一步把漏检反馈写成后续批次的强制输入：R3-B1 的缺参数/缺头文件与集成 source
注册遗漏属于 `compile/link`，R4-B4 的 freshness 默认值和 R6-B9/R9-B1 的运行崩溃属于
`runtime/test` 或并发边界；重试必须链接首个失败证据并登记改变的静态检查。若同类漏检
再次出现，下一批开始前先修订共享 skill、模板或 checklist，或记录替代门禁。该规则不把
历史批次回填为 PASS，也不把单次耗时解释为总体提效。详见 [R8 feedback evidence](evidence/skill-review-coverage-20260909.md#retrospective-feedback-loop)。

### R5 Caller Route Boundary 2026-09-08

R5-B2 已提供显式 `APPClient.request_native()` 和完整 runtime composition 的绑定出口，
但七个登记 maintained caller 仍分别使用 `request_task()`、`request_streaming()`、
`request()`、`distributed_inference()` 或 Python `APPProvider`。其中 YOLO harness 虽启动
`di-native-provider`，其 User 仍走 Python requester；Qwen/streaming harness 仍委托旧
Python runner。按 [R5-B3 caller audit](evidence/r5-b3-maintained-caller-audit-20260908.md)，
先冻结 caller matrix，再分为 native runtime config fixture、YOLO requester、
Qwen/streaming requester 和 Provider host 四个出口；不能为了少一次构建把它们合成一批。
T013-A 当前保持 `PARTIAL`，直到至少一个真实 caller 具备 operator-pinned catalog、
preparation、grant/admission、C++ request selector 和 rollback evidence。

R5-B7 已先闭合 native handle observer 的局部出口；R5-B8 只处理同一调用链的 Qwen
full-generation callback 接线：其稳定出口是维护入口能把已接受 token snapshot 与 terminal
通知交给 C++ observer facade，并在最终响应前完成顺序核对。该批不吸收 Provider retirement、
conversation owner 或跨进程资格；它们使用不同 owner/selector，继续保持 `PARTIAL` 并在
后续批次单独验收。

自本计划 Revision 14 起，新建或更新的逻辑批次必须在唯一 evidence 记录中附带
`Review trace`（review-agent 路径/SHA、基线、完整 diff 范围、复审结果）和
`Closure decision`（`CLOSED_FOR_VALIDATION` 或 `OPEN_FOR_NEXT_BATCH`、稳定出口及触发条件）。
Revision 13 之前的历史批次记录保持原事实，不回填虚构的审查身份或验收；下一次重开这些批次
时才按新字段补齐。

### R5-B9 Native Conversation Owner 2026-09-08

R5-B8 之后先推进 T013-E/R5-B9：将 requester configuration 中明确的 journal root、
owner-only key files、requester/service identity 和 security-domain digest 在 C++ 中组合为
唯一 `NativeConversationCoordinator`，再注入 `NativeInferenceClient`。Python facade 只转发
配置 JSON 与保留 opaque coordinator；未配置 owner 的 continuation 仍必须 fail-closed，不能
调用 Python `ConversationCoordinator`。本批稳定出口是配置错误在 network side effect 前被 C++
拒绝且正确配置能完成 native client composition；Provider receipt/control、跨进程两轮请求、
恢复/replacement 和 T016 不纳入本批，完成后保持 `PARTIAL`。

2026-09-07 implementation audit：T004-A 因真实 Selection wire/identity 不兼容重开，
见 [A8-01](evidence/t004-wire-reopened-20260907.md)。在继续 T010 完整请求提交前，
先修复 T004 的完整 canonical wire、真实工件和 grant 输入，并复核受影响前置值契约；
不得将七字段片段包装为可执行计划。其余阶段及 T016 正式运行顺序保持。

1. G0 / T001：复用已关闭O-001的源码身份与181承接，关闭O-002--005，冻结schema/调用方/依赖与单测、集成、实验选择器。181旧完整资格不作为前置门；源码基线关闭不表示新依赖组合运行PASS。
2. G1 / T002--009：库、策略、sealer/grant、assembler/tokenizer、准备/admission和Provider host；按已登记逻辑批次执行：逐小任务实现→只读静态门→继续同批；整批逻辑/流程审查后统一构建及相关单测。
3. G2 / T010--012：requester、会话/恢复与绑定；完成接线、相关单测，同时编写注册后续集成用例。
4. G3 / T013--014：迁移旧入口、实现隔离gate和MiniNDN harness/collector；完成静态审查与本地单测，真实跨进程/no-Python用例尚不运行。
5. G4 / T015：全部实现与测试工具完成后，补审跨任务调用链、effective config、测试/oracle/harness和依赖；复用有效局部审查，控制性缺陷修复后进入T016。
6. G5 / T016：统一执行完整unit→integration→MiniNDN/no-Python及既定负例，核对实际证据和最终diff；不另写测试后报告。
7. G6 / T017：交付已验证版本、说明与示例；外部SIF/Tiger由实验机器接手。

审查内容、最小诊断例外、变化/失败处理和唯一结果记录见
[validation workflow](contracts/pre-test-static-review.md)。
实现任务[x]表示实现/审查/单测完成；完整PO与feature验收直到T016才关闭。
任务开始本身不会使前项单测失效；实际变化决定重审和回归范围。

### B-G1-YOLO-SEMANTIC

2026-09-08：当前生产适配器接线的直接前置是 T003-B 尚缺的真实 catalog semantic
partition 消费。先完成该既有 PARTIAL 单元的修复批次，再推进 T008-A；不以共享
ONNX helper 的局部 PASS 放行 T003-A 或 T003-C 的完整验收。

| Member | Behavior boundary | Implementation dependency |
| --- | --- | --- |
| YS-1 / T003-B | 将注册 partition 的语义节点名映射到实际 inspection 的 planning node ID；核对完整 cover | 已通过定向测试的 owned ONNX inspection API；保持其完整输入/graph identity 绑定 |
| YS-2 / T003-B | 对照实际图与 metadata 验证 tensorInterfaces、dependencyEdges、safeCuts、roleInterfaces | YS-1 静态门；不得只有 node cover 就接受 catalog |
| YS-3 / T003-B | 注册 catalog 消费入口接入 NativeYoloComponentSplit，并编写实际 Python splitter 对照及篡改负例 | YS-1/YS-2 静态门；不能只测试独立转换 helper |

Owner：当前执行者。共享测试选择器：Spec182YoloSplit、Spec182NativePlanning、
Spec182Preparation、Spec182CanonicalPublisher、Spec182V3Placement；同一批次最后
统一 `waf build --targets=unit-tests -j4`。ABI 变化仍按 toolchain preflight 选择
fresh tree，不能复用不兼容对象。静态门及测试结果统一记录于
[batch evidence](evidence/t003-yolo-semantic-batch-20260908.md)。
T003-A/T003-C/T008-A 的既有 acceptance dependencies 保留；本批不运行 requester、
integration、MiniNDN 或 Tiger，不把 catalog 声明当作 Core 来源认证。

T003 局部验收已按 [local closure](evidence/t003-local-closure-20260908.md) 逐条核对通过；
上述批次的历史前置描述不再表示 catalog semantic factory 未实现。T003-A/B/C 的
LocalChecks 与 T016 FinalProof 分开；R1 的 source/state 映射阶段出口及 T008/T010
生产组装与默认接线仍未完成，后续必须关闭，不能以局部卡 DONE 替代。

### Dependencies

~~~text
Merged baseline closure and Spec181 handoff -> T001
T001 -> T002 -> T003 -> T004 -> T005
T002 -> T006
T002 -> T007
T003/T006/T007 -> T008
T006/T007 -> T009
T003/T004/T005/T006/T007/T008/T009 -> T010 -> T011 -> T012
T012 -> T013 -> T014 -> T015 -> T016 -> T017
~~~

T006/T007 的原生依赖设计必须先由 T001 关闭，不能一边猜 ABI 一边并入 requester。
T003--011 中的大算法迁移为设计批次，超过工作单元阈值时按 work-units 先沿稳定行为接口细分，
不机械按文件拆分、不授权并行 agent 自动实施。
用户已授权在Experimental完成Spec182；当前按T001关闭设计，再按上述门执行实现与本地验证。SIF/Tiger仍由实验机器负责。Static review PASS != Behavior PASS。

## Bounded Executor Profile

所有执行者统一使用 [execution cards](contracts/execution-units.md)
展开现有17个父任务，按一张就绪卡的 Read/Write/Steps/Verify 分派，沿稳定行为边界拆分。
T001由设计者关闭未决契约与选择器；设计冻结后的实现按依赖分派，ABI、安全、生命周期和整体收敛由相应审查者复核。
每卡记录实际源码身份与证据；依赖或设计变化只重新检查受影响卡。卡完成不提前关闭父任务或T016。
执行卡覆盖与链接检查不证明产品完成；当前子任务状态统一记录在 tasks.md 的 Execution Progress。
原 G0--G6 顺序与上方父任务依赖保持。本文件引用的共享设计技能为本仓库版本。

## Migration and Compatibility

在本地候选版本中将Python默认入口一次切换到同库；独立消费者的完整运行与迁移正确性由T016统一验收后交付。
旧实现仅在迁移窗口保留；T013 关闭时删除无生产消费者的运行实现，离线对照不进入运行包。
无长期双默认路径。公开 API 未实现的兼容项由 inventory 显式 BLOCK，不静默 fallback。
旧 journal/model/contract 格式使用原版本规则；字节不一致先修订设计，不能改 oracle 消除差异。
具体 mixed-version、数据备份/原子转换/回退和旧路径 owner 见
[migration contract](contracts/runtime-boundaries.md#migration-and-rollback-contract)。
GUI、离线训练/导出与实验 Python 保留；其业务调用转向 binding，禁止在工具层藏 runtime owner。

## Evidence Reuse

2026-09-08 [R2-B4 源码审计](evidence/r2-b4-grant-production-audit-20260908.md) 确认
T005 的缺口包含真实签发实现，不能仅按 T004 依赖复核收口。R2 下一批按签名请求/
策略签发 → 答复认证/Core 发布 → sealed projection/Provider 密钥消费推进，
保留已验证 R2-B1/B2/B3；该批不以注入回调替代生产入口，也不提前授予 R3/T016 PASS。

按源码、接口、依赖、配置、oracle/harness的实际变化判断影响；
只更新受影响契约、重读相关调用方并重跑相关检查。没有变化或具体缺口不重复全量验证。
未运行、环境失败、局部通过和完整资格分开，原始失败结果保留。
最终版本必须有全部既定本地运行证明；文档改动不触发模型重跑。

## Delivery

T017 的 evidence/development-handoff.md 包含 exact commit、clean source closure、
库/可选绑定/依赖/adapter/model/config/oracle/harness hashes、可复现命令、
全部本地 verdict、失败处置和已知限制。私钥不入 Git。
另附原生库头/链接说明与最短 C++/Python 示例；示例均调用同一 public API。

外部 owner=实验机器：按 [layered runtime delivery](contracts/layered-runtime-delivery.md)
构建或复用稳定基础 SIF，在对应 builder 内构建外置 DI/UAV 应用包，迁移并验证
组合身份/profile/launcher（当前 PLANNED），验证 exact runtime dependency closure，
运行 Tiger 并返回该身份的日志和 verdict。TRANSFERRED 只表示责任移交，
不得把未运行的 SIF/Tiger/GPU/性能写为 PASS。
既有容器内 ABI/build boundary 仍适用，不把 host .so 或 venv 装入镜像充当构建。
仅应用改动不重新打包未变基础镜像；依赖/ABI 变化仍重建相应闭包。T017 包含
分层清单、构建方法、组合验证及外部工具接续步骤；文档接受不授予部署 PASS。

## Current Planning Result

T001-A/B/C 设计关闭全部完成（2026-09-07，evidence
[t001-ab-closure](evidence/t001-ab-closure-20260907.md) 与
[t001-c-freeze](evidence/t001-c-freeze-20260907.md)）；用户已授权在Experimental
完成182，产品实现与最终运行验证仍 NOT_STARTED，按任务门 T002 起执行。
O-001--O-005 全部关闭：O-002 由 [native ONNX assembly design](contracts/native-onnx-assembly-design.md)
设计收口，O-003/O-005 见 [native dependency design](contracts/native-dependency-design.md) 与
[native isolation design](contracts/native-isolation-design.md)，O-004 静态映射收口
于 compatibility-manifest（62157804/6fa1f756）；运行期 build identity、L0 命令与
每卡 planned suite/case selector 冻结于 [case-manifest](../../tests/fixtures/spec182/case-manifest.json)。
CD-005 新增具名原生装配 worker，以保留不可中断ONNX调用的取消/超时清理；库、worker
安装、父子协议及隔离白名单见 [native ONNX assembly design](contracts/native-onnx-assembly-design.md)。
产品 native 构建的当前工具链边界为 NAC-ABE ABI 5 符号缺口
（[failure-log](../../docs/failure-log.md) 2026-09-07），T002-A 匹配后重跑首次 L0。
后续182开发仍按G1--G6执行本地开发验证；上一轮delivery-only的TRANSFERRED不是永久移走182的T016义务。
使用仓库[shared design skill](../../skills/speckit-code-design/SKILL.md)及其相对引用，避免依赖开发机个人技能路径。T017复用现有[handoff tooling](../../Experiments/TigerCluster/docs/source-handoff.md)，另生成182身份与依赖清单；旧锁包含Python运行包且不含planned原生DI库，不能直接称为182 no-Python交付。
