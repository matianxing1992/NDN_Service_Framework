# Tasks: Native NDNSF-DI with Optional Python Bindings

**Revision**: 136 | **Status**: DRAFT / T001 DONE
**Input**: [spec](spec.md), [code design](contracts/code-design.md),
[proof](contracts/proof-design.md), [work units](contracts/work-units.md)

## Execution Progress

### Native-First Dispatch 2026-09-10

剩余调度权威为 [N1--N5 / R11 cards](contracts/native-first-execution.md)。当前 R11-B1
已完成独立 authority↔requester process 的 C++ 正例、负例和不可达边界，R11-B2 已完成
真实 C++ 跨进程 unary 的本地 process 出口；R11-B3 stream 已形成独立出口，当前继续收敛
R11-B4 continuation、R11-B5 recovery 与 R11-B6 replacement 已形成独立出口，下一批转入
R11-B7 cleanup。不得在 N1--N3 通过前以旧
调用方批量迁移、Python 数量或全仓库扫描代替原生出口。R11-B7 已形成 cleanup 出口；
R11-B8 现开始按 caller group 批次执行。已有局部 PASS 及下面历史记录
保留，父任务不因本轮局部实现升级。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [D-NATIVE-FIRST Replan](evidence/native-first-replan-20260910.md) | DONE | User execution-order decision | 文档依赖/链接、旧勾选状态、11/11 workflow 同步及双 PDF 构建检查通过；产品 NOT_RUN | 2026-09-10 |
| [R11-B1 Independent Authority](contracts/native-first-execution.md#dispatch-cards) | CLOSED_FOR_VALIDATION | T001 valid closure; existing T005 implementation | C++ 独立 authority/requester process 经真实 NFD/Controller 通过 1 正例、5 个 Authority handler 拒绝例和 1 个 authority 不可达超时；bwrap requester 隔离、角色 PIB/TPM 快照与 C++ grant 验证通过。T005 父任务、R11-B2 及完整 Spec qualification 仍未关闭 | 2026-09-10 |
| [R11-B2 Native Unary Process](contracts/native-first-execution.md#dispatch-cards) | CLOSED_FOR_VALIDATION | R11-B1; existing T008/T009/T010 implementation | 独立 C++ DI_NativeRequester / authority / di-native-provider；真实 ACK/Selection/handler/Response、受保护 grant、ONNX Runtime CPU evidence 和 C++ numerical oracle `[4,0,12]`；Provider 缺 role 的拒绝例也 fail-closed。T010 父任务、R11-B3 及完整 Spec qualification 仍未关闭 | 2026-09-10 |
| [R11-B3 Native Stream Process](contracts/native-first-execution.md#dispatch-cards) | CLOSED_FOR_VALIDATION | R11-B2 | 独立 C++ requester/Core/Provider process 通过 8 个有序 token 事件、final response、grant verification、post-selection preparation、decode-state commit 和 CPU ORT execution；C++ oracle 与构建/单测/集成证据已记录。gap/timeout/重复/错 generation、父 T010/T011 及完整 qualification 仍未关闭 | 2026-09-10 |
| [R11-B4 Native Continuation Process](contracts/native-first-execution.md#dispatch-cards) | CLOSED_FOR_VALIDATION | R11-B3 | 独立 C++ 双轮 process 已通过第一轮 `FULL_CONTEXT`（stream oracle、COMMIT/FINALIZE、持久 journal checkpoint）及新 generation 的第二轮 `APPEND_DELTA`；错误 parent 进程按预期以 `NATIVE_CONVERSATION_BEGIN_FAILED` 拒绝；publisher stable artifact identity 已绑定 canonical manifest digest。父 T010/T011、R11-B5 及完整 Spec qualification 仍未关闭 | 2026-09-10 |
| [R11-B5 Native Recovery Process](contracts/native-first-execution.md#dispatch-cards) | CLOSED_FOR_VALIDATION | R11-B4 | 第一轮 C++ `FULL_CONTEXT` checkpoint 后对 Provider 进程执行 SIGKILL/restart；重启 Provider 以 `PROVIDER_CONVERSATION_STATE_MISSING` 明确拒绝 `APPEND_DELTA`，requester 以 `NATIVE_STREAM_FAILED` 退出且无重复 execution/stream marker。未宣称 Provider KV durable recovery；R11-B6 及完整 Spec qualification 仍未关闭 | 2026-09-10 |
| [R11-B6 Native Replacement Process](contracts/native-first-execution.md#dispatch-cards) | CLOSED_FOR_VALIDATION | R11-B5 | 独立 Provider B 在 A ACK 后接管 `attempt-2` 并完成 C++ stream/CPU ORT；A 无 execution evidence；无 backup 时单一 `NATIVE_REQUEST_STAGE_FAILED`/`DI_NATIVE_NO_ADMITTED_PROVIDER` 终态。C++ fencing selector 通过；广泛 `Spec182*` 仍有 6 个既有 runner callback fixture failures。父 T010/T011、R11-B7--B9 与完整 qualification 仍未关闭 | 2026-09-10 |
| [R11-B7 Native Cleanup Process](contracts/native-first-execution.md#dispatch-cards) | CLOSED_FOR_VALIDATION | R11-B6 | C++ client/registration/lease/host/stream cleanup selectors 全部通过；终态、cancel、deadline、replacement drain、secret owner 及 shared host isolation 有证据；R11-B6 process finally 后无 requester/provider/authority/controller 残留。T016 isolation/PO matrix、父 T010/T011/T013 与完整 qualification 仍未关闭 | 2026-09-10 |
| [R11-B8 C++ Prepared-Role Fixture](evidence/r11-b8-cpp-fixture-20260910.md) | CLOSED_FOR_VALIDATION | R11-B6; existing T010/T011 fixture contract | **C++ primary:** 修复 `runSamplingEpochs` 缺少 `prepareRunner` 的夹具契约；fresh `unit-tests` 190/190 build，窄 selectors 11 cases 通过，随后完整 `Spec182*` 256 cases/7077 assertions exit 0。仅修复测试夹具，不推进 maintained callers、R11-B8/R11-B9 或父任务；该完整 selector仍只是C++ unit回归 | 2026-09-10 |
| [R11-B1-PY Native Binding Authority](evidence/r11-b1-py-native-authority-20260910.md) | CLOSED_FOR_VALIDATION | R11-B1; T012 ABI | **C++ primary:** `_ndnsf` binding now constructs the transport-only native grant client through `issueThroughCore`/`publishThroughCore`; authority private/content keys and policy fields are rejected before requester key loading. Matching `build-nac182` DI library, extension import, C++ selectors, 7-case native integration selector, and 72 wrapper/contract tests pass. This closes only the Python binding ownership seam; maintained callers, no-Python, dependency closure and T016/T017 remain open | 2026-09-10 |
| [R11-B8-G1 Native Generic Request Facade](evidence/r11-b8-g1-native-generic-facade-20260910.md) | CLOSED_FOR_VALIDATION | R11-B7; T012-A/B | **C++ primary:** matching `Spec182*` unit 256 cases/7077 assertions and 7 native Core/Provider integration cases re-run green. **Python secondary:** generic `APPClient`/`InferenceClient` native inline route, result handle, identity rejection and planner non-fallback checks pass (38 tests). This is one bounded generic unary facade; stream/conversation, 15 remaining callers, legacy zero-use, no-Python and T016 remain open | 2026-09-10 |
| [R11-B8 Maintained Callers](contracts/native-first-execution.md#dispatch-cards) | PARTIAL | R11-B7; corresponding T012 ABI | G1 generic unary facade 已形成稳定出口；仍需 15 个 caller group 的 native entry、实际行为、兼容 wrapper 及旧路径零使用证据，不能按 G1 计全量完成 | 2026-09-10 |
| [R11-B9 Native Closure](contracts/native-first-execution.md#dispatch-cards) | NOT_STARTED | R11-B8 | T014 no-Python/依赖闭包工具 → T015 → T016 → T017；最终资格未开始 | 2026-09-10 |

文档旁路记录（2026-09-10，DOCUMENT PASS）：Proposal／slides 收敛为 DNMP-inspired
授权加配套加密与 NDNSF ABE-backed 两种设计。正文与 slides 六入口及两份讲稿构建
通过；修正 PPTX 输入路径后，38 页可编辑文本与 notes、重导出截图、全文残留检查
通过。见[持久审计](../../docs/PAPER/proposal-defense/research-revision-audit.md)和
[文档验证](../../docs/PAPER/proposal-defense/research-revision-validation.json)；此记录不变更产品任务状态。

文档旁路记录（2026-09-09）：Proposal／slides 授权论证统一为两条理由，机制、撤销与
成本单列；[持久审计](../../docs/PAPER/proposal-defense/research-revision-audit.md)
和[文档验证](../../docs/PAPER/proposal-defense/research-revision-validation.json)
记录编译、版面及交付检查。此为文档工作，不变更下表实现状态或原生资格验收结论。

本表是所有执行者共同维护的**当前子任务进度唯一入口**；点击任务查看 Read、Write、Steps、Verify。
父任务清单保留阶段验收；Current Checkpoint 保存摘要与历史，不作为第二张状态表。
状态：NOT_STARTED（无独立执行记录）、READY（依赖及门禁满足）、IN_PROGRESS（正在执行）、
PARTIAL（已有工作但验收不全）、BLOCKED（已确认阻塞）、DONE（该卡完整验收通过）。
PARTIAL 不表示依赖放行；T001 release 及 plan Gate Order 继续约束执行。
本轮已对 R11-B1 运行同源 C++ 构建、unit/integration selector、CLI 和独立 process 检查；
该卡按本地 process 出口记为 `CLOSED_FOR_VALIDATION`，不以此推进 T005 父任务或计算全 Spec 百分比。
每个工作单元成功/失败/阻塞后、commit 和回复前更新对应行及证据；新增工作先补卡和进度行。
维护规则见 [task progress](../../skills/speckit-code-design/references/task-progress.md)。
每个批次在编码前还要登记分配依据：共同生产入口/调用方、接口/状态/所有权或数据契约、
独立 oracle/测试 selector、源码/构建 closure 和验收出口。任一项不一致，或出现新的硬
验收依赖，必须拆出新的 Batch ID；稳定出口形成后立即进入批末验证。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [R10-B84 Native Request Identity Scope](evidence/r10-b84-native-request-id-scope-20260910.md) | CLOSED_FOR_VALIDATION | R10-B83; T010/T011 | **C++ primary:** each production native client creates a fresh owner scope in one final request-name component; the identity case, complete `Spec182*` C++ unit selector, and 9-case native Core/Provider integration selector pass after the repaired `unit-tests`/`integration-tests` build. The initial multi-component form failed at the C++ integration boundary and is retained in the failure log. **Python secondary:** no Python test was used as native behavior evidence. Cross-process executable transport, independent artifact authority, 16 maintained old callers, no-Python, dependency closure and T016/T017 remain open; no parent advanced | 2026-09-10 |
| [R10-B83 Native Conversation Config Loader](evidence/r10-b83-native-conversation-config-20260910.md) | CLOSED_FOR_VALIDATION | R10-B82; T010/T011 | **C++ primary:** shared loader, 252 `Spec182*` unit cases, 9 native Core/Provider integration cases, requester help and exported-symbol check pass. **Python secondary:** 72 wrapper/contract tests pass. Standalone requester and Python binding both inject the same coordinator; artifact-authority separation, independent worker/process, 16 maintained old callers, no-Python and T016/T017 remain open; no parent advanced | 2026-09-10 |
| [R10-B82 Whole-Chain Static Audit](evidence/r10-b82-whole-chain-static-audit-20260910.md) | OPEN_FOR_NEXT_BATCH | R10-B81; T010/T011/T013/T014/T015/T016/T017 | Its standalone conversation-config mismatch is repaired by R10-B83; requester still composes the artifact-authority private key/issuer, 16 maintained inference calls remain on old routes, and independent worker/process, cross-process recovery, no-Python and host/container dependency closure remain open. No parent task advanced | 2026-09-10 |
| [R10-B34 Spec182 Regression Sweep](evidence/r10-b34-regression-sweep-20260909.md) | DONE | R10-B33; R10-B32 | Spec182 C++ unit suite, full `Spec170NdnsfDiCoreFlow/*` integration suite, and 76 Python binding/compatibility tests all exit 0; expected negative boundaries remain asserted in raw logs. This is regression evidence only; cross-process transport, maintained caller/no-Python and T016 remain open | 2026-09-09 |
| [R10-B35 Spec Kit Command Output Contract](evidence/r10-b35-command-output-contract-20260909.md) | DONE | R10-B34; R10-B32 | Shared reference now defines the mandatory entry output contract; code-design, README, personal install, and 11 local Spec Kit entry copies are synchronized and checked. Documentation boundary only; product parents and T016 remain PARTIAL/UNQUALIFIED | 2026-09-09 |
| [R10-B36 Remaining Production Chain Reorder](evidence/r10-b36-production-chain-reorder-20260909.md) | DONE | R10-B35; R5-B3 caller audit; R10-B34 | Existing T010–T017 cards are reordered by real production exit: native Core/Provider result, Provider worker/cross-process, YOLO caller, Qwen/stream/conversation caller, legacy retirement, isolation, convergence/qualification/handoff. No parent status or acceptance gate changed | 2026-09-09 |
| [R10-B37 Native Client Streaming Provider Request](evidence/r10-b37-native-client-streaming-request-20260909.md) | DONE | R10-B36; R10-B31; R10-B33 | P1 bounded stream-only requester/Core/Provider selector passes; 118/118 system-first `-j4` integration build, new selector (4 assertions), four R4-B6 selectors and two unary selectors exit 0. This closes only the in-process stream-only boundary; conversation, worker/cross-process, maintained caller/no-Python and T016 gates remain open | 2026-09-09 |
| [R10-B38 T016 Runtime Context Recheck](evidence/r10-b38-t016-runtime-context-recheck-20260909.md) | DONE | R10-B37; R6-B4 | Fresh T016 preflight records the current host's system NFD socket but no MiniNDN node/netns metadata; default campaign returns `UNQUALIFIED/MININDN_NODE_CONTEXT_NOT_PROVIDED`, explicit owner mode returns `UNQUALIFIED/MININDN_REQUIRES_ROOT` for UID 1000. No business process or protocol result started; T016 remains open | 2026-09-09 |
| [R10-B39 R4-B6 Fixture Contract Guard](evidence/r10-b39-r4b6-fixture-contract-guard-20260909.md) | DONE | R10-B37; R10-B38 | Post-commit review miss repaired: conversation binding now precedes `failFirst`; system-first `-j4` integration build 118/118 and stream-only, conversation/replacement, unary/repository selectors exit 0. Next native build uses `-j2` after nonzero `vmstat si`; product parents and T016 remain open | 2026-09-09 |
| [R10-B33 Native Unary Repository Reference Request](evidence/r10-b33-native-unary-repository-reference-20260909.md) | DONE | R10-B31; R10-B11; T010-B | Existing R4-B6 real Provider fixture now covers native unary `REPO_REF`: published encrypted object is fetched and checked before the native terminal `Response`; six related selectors pass after system-first `-j4` integration rebuild. This closes only the single-process unary repository boundary; Provider worker, cross-process, maintained caller/no-Python and T016 remain open | 2026-09-09 |
| [R10-B32 Shared Spec Kit Skill Feedback Loop](evidence/r10-b32-skill-feedback-loop-20260909.md) | DONE | R10-B31; R10-B27/R10-B28 skill workflow | `Static Gate Release Checklist`, required `Changed gate` on compile/runtime retries, and `Batch growth decision` are now in the shared code-design references, templates, README, and installed copy; product parents and T016 remain PARTIAL/UNQUALIFIED | 2026-09-09 |
| [R5-B2 Native Runtime Construction Binding](contracts/execution-units.md#t012-b-compatible-python-facades) | PARTIAL | R5-B1; T012-A | [batch evidence](evidence/r5-b2-native-runtime-binding-20260908.md)：native catalog/preparation/admission/runtime/grant owners 已可组合，公开 `InferenceClient.request_native()` 明确拒绝 planner fallback；24 binding cases、32-case compatibility selection PASS；真实请求 parity、maintained caller 全量迁移与 T016 仍未验收 | 2026-09-08 |
| [R5-B1 Native Binding ABI Rebuild](evidence/t012-a-binding-abi-20260908.md) | PARTIAL | R4-B6; T012-A | Explicit candidate Core/DI + NAC-ABE/SVS rebuild, extension import and four focused Python suites 21/21 PASS；完整 native requester construction、caller migration、cross-process closure 与 T016 仍开放 | 2026-09-08 |
| [R4-B4 Authenticated Conversation Chain](evidence/r4-b4-conversation-chain-20260908.md) | PARTIAL | R4-B3; T011-C acceptance retained | CC-1/CC-2 focused C++ 9 cases、CC-3A projection、CC-3B requester/provider transaction wiring 与 runtime+coordinator 构造均已构建验证；49 个 requester/conversation/provider/stream 回归 cases PASS；`integration-tests -j4` 链接与 `Spec182GrantClientFlow/*` 2 cases PASS。R4-B5 的预置 receipt/ACK 边界已由 R4-B6 的真实 Provider 两轮补足，R7-B1 又关闭单 Provider replacement 负例；成功 recovery、跨进程资格与 T011-C/T016 仍未完成 | 2026-09-08 |
| [R4-B6 Real Provider Conversation](evidence/r4-b6-provider-conversation-20260908.md) | PARTIAL | R4-B4/R4-B5; T011-C/CC-4 | 真实 `ServiceProvider` 与公开 `NativeInferenceClient` 已完成 FULL_CONTEXT→receipt/control/commit→APPEND_DELTA 两轮；具名 integration selector 及结构化 request/event/collaboration 名称单测通过；R7-B1 已补单 Provider replacement 负例（`DI_NATIVE_NO_ADMITTED_PROVIDER`、无 checkpoint）。成功 alternate-provider recovery、跨进程资格与 T011-C/T016 仍未关闭 | 2026-09-08 |
| [R4-B3 Epoch Text Commit Boundary](evidence/r4-b3-epoch-text-20260908.md#final-local-result) | DONE | R4-B2; T011-B acceptance retained | terminal stable flush前移至事件接受前；真实tokenizer/epoch及stream/sampling共24 cases/411 assertions PASS，unit与实际DI库增量build PASS；父任务仍未完整验收 | 2026-09-08 |
| [R4-B2 Native Stream Production Chain](evidence/r4-b2-stream-production-20260908.md#final-local-result) | DONE | R3-B1; R4-B1 | Local requester stream batch：7 stream/190 assertions、2 options/21、29 regression/695 PASS；2 SDK recovery wires、CLI/loader PASS；真实Provider/会话/T016仍未完成 | 2026-09-08 |
| [R4-B1 Native Sampling Contract Repair](evidence/r4-b1-sampling-20260908.md) | DONE | R3-B1; T010-C/T011 acceptance retained | Local sampling batch：double惩罚与统一参数校验；4 cases/40 assertions、既有epoch 28 assertions PASS；增量unit与实际DI共享库构建PASS；stream/session仍待完成 | 2026-09-08 |
| [R3-B1 Default Request Lifecycle](evidence/r3-b1-request-lifecycle-20260908.md#final-local-result) | DONE | R2-B4/B5/B6; T010-A/B acceptance retained | initial-request local batch：131 DI cases/2975 assertions经共享测试及失败单例重试通过，13 Core cases/183 assertions通过；2 request/4 grant/7 dataflow SDK oracle、CLI入口与加载检查PASS；真实网络/stream/bindings/旧路径退出仍待后续 | 2026-09-08 |
| [R2-B6 Authorized Group Projection](evidence/r2-b6-group-projection-20260908.md) | DONE | R2-B5; R2-B3 | Initial-request local batch：group/model rank 分离、transfer operation/capability/endpoint 同源、实际 segment 消费；31 cases/1305 assertions PASS；默认 requester 与流式 feedback 未关闭 | 2026-09-08 |
| [R2-B5 Native Group Key Admission](evidence/r2-b5-group-key-admission-20260908.md) | DONE | R2-B4; NativeOfferAdmission | Local batch only：同一认证 ACK key binding→Core RSA→Provider capability unwrap；19 cases/1216 assertions PASS；group rank/operation/endpoint 编排待闭合 | 2026-09-08 |
| [R2-B4 Production Grant Chain](evidence/r2-b4-grant-production-audit-20260908.md) | DONE | R2-B3; CD-004; T004 acceptance retained | Local batch only：真实 signed issuer/authenticated client + sealer/Provider unwrap 组合；37 cases/672 assertions、4 independent oracle grants PASS；Core publication integration authored/T016，默认 requester 待接线 | 2026-09-08 |
| [R2-B3 Projection Builder](evidence/r2-b3-projection-builder-20260908.md) | DONE | T003-C; R2-B1/B2 | PB-1/PB-2/PB-3 batch only：73 cases/1735 assertions PASS；补充1 case/37 assertions + SDK 7 dataflows/11 endpoints PASS；Core group/grant/default requester 仍待接线 | 2026-09-08 |
| [R2-B2 State Source Binding](evidence/r2-b2-state-source-binding-20260908.md) | DONE | R2-B1 | SB-1/SB-2/SB-3 batch only：显式状态映射与源图重复 operand 修复；r3 71 cases/1649 assertions PASS；真实模型 bootstrap/requester 仍待完成 | 2026-09-08 |
| [R2-B1 Preparation Catalog](evidence/r2-b1-preparation-catalog-20260908.md) | DONE | T003-C; T006-D; T007-B | PC-1/PC-2/PC-3 batch only：源目录→完整 preparation 端口组合，70 C++ cases/1596 assertions PASS；真实 bootstrap/state/requester 仍待完成 | 2026-09-08 |
| [D-T003-LOCAL Local Acceptance Audit](evidence/t003-local-closure-20260908.md) | DONE | e3640279; T003 frozen LocalChecks | 原卡 requirement→proof、测试清单/实际成功日志与 design/diff 检查 PASS；T003 局部关闭，source/state、默认 requester 与 PO-002 仍由既定下游 owner 完成 | 2026-09-08 |
| [R1-B5 Canonical Role Preparation](evidence/r1-b5-canonical-role-preparation-20260908.md) | DONE | R1-B4; existing ONNX/candidate contracts | RP-1/RP-2/RP-3 batch only：实际源图→角色→发布后重新认证/抽取；69 C++ cases/1560 assertions PASS；默认 requester、Qwen state/rank source 仍待完成 | 2026-09-08 |
| [R1-B4 Catalog Task Adapter](evidence/r1-b4-catalog-task-adapter-20260908.md) | DONE | T002-A; C09/C18 contracts | CA-1/CA-2 batch only：production adapter→registry/preparation，45 C++ cases/944 assertions PASS；实际图/角色端口与原卡验收仍待完成 | 2026-09-08 |
| [D-LAYERED-SIF Layered Runtime Delivery](contracts/layered-runtime-delivery.md) | DONE | User architecture request | 方案已同步 FR-014/Delivery/T017/Tiger/packaging，定向链接、diff 与 design validator PASS；仅设计完成，部署工具 PLANNED | 2026-09-08 |
| [D-DESIGN-DIAGRAMS Visual Guide](evidence/design-diagrams-20260908.md) | DONE | User diagram request | 两侧各 9 个矢量视图；PDF 91/96 页，版面/目录/字体、API、460/350 文件还原、5 工具回归与视觉检查 PASS；不关闭产品任务 | 2026-09-08 |
| [R1-B3 Native Merge Publication](evidence/r1-b3-native-merge-20260908.md) | DONE | R1-B2; existing shared role contract | NM-1/NM-2 batch only：r3 incremental build 与 68 C++ cases/800 assertions PASS；真实 adapter/requester 与 parent gates unchanged | 2026-09-08 |
| [R1-B2 Candidate Role Semantics](evidence/r1-b2-role-semantics-20260908.md) | DONE | T002-A; existing candidate contract | CR-1/CR-2 batch only：官方静态门与 C++ 26 cases/546 assertions PASS；native Merge publication remains open，T003-C 不变 | 2026-09-08 |
| [D-DISK-CLEANUP Build Object Cleanup](evidence/disk-cleanup-20260908.md) | DONE | User cleanup request | pip cache 及 2702 个旧对象已清理；磁盘可用 910 MB→30 GB；当前构建、二进制与原始证据保留 | 2026-09-08 |
| [D-NATIVE-TEST-POLICY Native Test Ownership](contracts/proof-design.md#native-test-ownership) | DONE | User native testing request | 主要行为测试由 C++ 直接调用生产库；Python 兼容/离线 oracle/外部设施边界已明确，文档检查 PASS；实际测试迁移由各实现卡与 T013/T015/T016 负责 | 2026-09-08 |
| [D-CHAIN-REPLAN Production Chain Review](evidence/production-chain-replan-20260908.md) | DONE | User pause and replan request | 23 张未完成卡归入七个能力阶段；实际 R<n>-B<k> 按完整行为/共享契约/稳定出口领取，逐任务静态门、批末统一验证；36 张原卡状态不变，新增实现暂停 | 2026-09-08 |
| [D-REVIEW-AGENT Official Skill](evidence/review-agent-install-20260908.md) | DONE | User installation request | 官方原版安装/字节身份/技能 schema PASS；逐任务静态门明确调用，文档校验 PASS；不关闭产品任务 | 2026-09-08 |
| [D-SKILL-BATCH Workflow Revision](evidence/skill-batch-workflow-20260908.md) | DONE | User workflow request | 共享 batch-quality-gates、11 个 Spec Kit 入口、code-design references 与 Spec 模板已同步；覆盖生产调用方、测试/harness/oracle、构建注册、稳定出口、批次分配依据、静态/编译/运行漏检及匹配耗时；新增 native test ownership：NDNSF-DI 行为须由 C++ production target/selector 验收，Python 仅作 binding/facade/oracle/外部设施边界；新增 `Review trace` 与 `Closure decision` 的可追溯批次门；旧验证器 compatibility 白名单限制已记录，不关闭产品任务 | 2026-09-08 |
| [D-SKILL-REVIEW-COVERAGE Minimum Review Record](evidence/skill-review-coverage-20260909.md) | DONE | User workflow review request | 共享 `review-agent.md` 新增五 lane Minimum Review Record；`plan-template`/`tasks-template` 明确测试注册、target/source closure、四类 Batch Retrospective；本轮再补漏检反馈闭环：重试链接首边界并登记改变的静态检查，同类漏检触发 skill/template/checklist 修订或替代门禁；本机入口保持同步；不改变产品任务状态 | 2026-09-09 |
| [D-SKILL-CLI-BOUNDARY CLI and Harness Evidence Boundary](evidence/skill-cli-boundary-20260909.md) | DONE | D-SKILL-REVIEW-COVERAGE | 共享 batch gate、code-design、plan/tasks 模板和 skills README 明确 CLI/harness smoke 只证明接线边界，不得替代 native request/result、parity 或 qualification；安装副本 SHA 一致；文档检查通过，不改变产品任务状态 | 2026-09-09 |
| [D-SKILL-CONTEXT-POINTER Context Pointer Workflow Boundary](evidence/skill-context-pointer-20260909.md) | DONE | D-SKILL-CLI-BOUNDARY; D-SKILL-REVIEW-COVERAGE | 版本化 `speckit-code-design` skill 新增 Context Pointer Updates；`speckit-agent-context-update` 明确只维护托管 plan 指针，入口/个人副本同步并通过 marker、diff、validator 和 SHA 检查；不改变产品任务或资格状态 | 2026-09-09 |
| [D-DESIGN-R3 Revision](evidence/design-r3-20260908.md) | PASS | User documentation request | 逐章修订、23 组关键契约、生成/KV/会话重写；双 PDF 82/87 页、5 工具回归、API/八份参考/460+350 源码还原/版面 PASS；不关闭产品任务或全量语义审计 | 2026-09-08 |
| [D-DESIGN-CHAPTER-AUDIT Chapter Review](evidence/design-chapter-audit-20260908.md) | PASS | User document review request | 审阅完成：当前/目标 62/67 章；7 KEEP、36 EXPAND、20 REWRITE、4 CORRECT；被审文档 NEEDS_REVISION，PDF 未改写，不关闭产品任务 | 2026-09-08 |
| [D-DESIGN-R2 Baseline and Contracts](evidence/design-r2-20260907.md) | PASS | User documentation request | 当前/目标 66/69 页；4 工具回归、API、460/350 文件还原、PDF 身份/版面 PASS；BC-01 至 BC-04 已补，TG-01 至 TG-05 PLANNED；不关闭功能任务 | 2026-09-07 |
| [D-DESIGN-API API Developer Guide](evidence/design-api-guide-20260907.md) | PASS | User documentation request | 四模块 23 组 API 契约、295 文件声明参考及 830 绑定操作；双份 63 页 PDF/58 目录项、声明/源码快照检查 PASS；AGENTS 与 MANAGEMENT 同步；不改变功能验收 | 2026-09-07 |
| [D-DESIGN-R0 Framework Design PDFs](evidence/design-pdf-baseline-20260907.md#work-unit-d-design-r0) | PASS | User request; documentation only | 四模块中文双 PDF 各 35 页；正文/字体/版面与 94 文件基线检查通过；新增 Spec 设计变更记录，用户已授权 Design 完整入 Git；不关闭功能任务 | 2026-09-07 |
| [T001-A Identity and Dependency Closure](contracts/execution-units.md#t001-a-identity-and-dependency-closure) | DONE | — | [closure](evidence/t001-ab-closure-20260907.md)；DOC + 依赖契约 vs 持久探针核对通过；onnx 4/4 与 tokenizer 84+14 全新复现 PASS；rust 1.90.0 独立工具链核验；Cargo 边界已在 rust-prefix 上重跑通过（tokenizer-r2） | 2026-09-07 |
| [T001-B Lifecycle and Capability Closure](contracts/execution-units.md#t001-b-lifecycle-and-capability-closure) | DONE | — | [closure](evidence/t001-ab-closure-20260907.md)；DOC + 双向映射核对通过；O-004 处置写入 runtime-boundaries（Rev 8）与 symbol-design（C21/Readiness）；registration generation/late ACK/Selection/共享 lease 已冻结于 lifecycle 设计；parity 按 owner 任务继续，不属本卡 | 2026-09-07 |
| [T001-C Dispatch and Selector Freeze](contracts/execution-units.md#t001-c-dispatch-and-selector-freeze) | DONE | T001-A, T001-B | [closure](evidence/t001-c-freeze-20260907.md)；build identity/L0 命令/每卡 selector 已从实际 Waf 注册冻结到 [case-manifest](../../tests/fixtures/spec182/case-manifest.json)（23 cppSuites + 6 kexpr + 3 system，全部带 author/executeOwner）；proof/code-design/work-units Rev 8、O-002/O-004 关闭；DOC 通过 | 2026-09-07 |
| [T002-A Installed Library Boundary](contracts/execution-units.md#t002-a-installed-library-boundary) | DONE | T001-C | [L0 evidence](evidence/t002-a-l0-20260907.md)；首次 L0 成功（r2 fresh staging）：consumer 独立编译 rc=0、运行打印 `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK` rc=0、无 libpython、staging 无 DI `.cpp/.cc` 副本、NAC-ABE 5-symbol gate 通过；空 registry freeze 语义自修正（[failure-log](../../docs/failure-log.md) 2026-09-07） | 2026-09-07 |
| [T003-A Qwen Split Candidates](contracts/execution-units.md#t003-a-qwen-split-candidates) | DONE | T002-A | [local closure](evidence/t003-local-closure-20260908.md)；元数据图/候选、完整 identity/resource、实际 ONNX inspection 的原卡单测通过；actual source/state 与 requester 仍归 T008/T010；PO-002 留 T016 | 2026-09-08 |
| [T003-B Yolo Split Candidates](contracts/execution-units.md#t003-b-yolo-split-candidates) | DONE | T003-A | [local closure](evidence/t003-local-closure-20260908.md)；实际 ONNX catalog/完整语义接口/确定性候选与独立 oracle 通过；catalog 网络取得和 requester 仍归 T008/T010；PO-002 留 T016 | 2026-09-08 |
| [T003-C Placement and Registry](contracts/execution-units.md#t003-c-placement-and-registry) | DONE | T003-A, T003-B | [local closure](evidence/t003-local-closure-20260908.md)；完整候选/role metadata、确定性放置及 SDK V3 oracle 原卡单测通过；默认调用链仍归 T010；PO-002 留 T016 | 2026-09-08 |
| [T004-A Canonical Plan Sealing](contracts/execution-units.md#t004-a-canonical-plan-sealing) | PARTIAL | T003-C | [publication recertification](evidence/t008-publication-recertification-20260907.md)；发布后 recipe/core 与 SDK 对照、旧 exact-reuse 拒绝及相关 58 cases/710 assertions PASS；dataflow/device binding、真实 requester 主链与完整验收仍待完成 | 2026-09-07 |
| [T005-A InProcess Authority](contracts/execution-units.md#t005-a-inprocess-authority) | PARTIAL | T004-A | [R2-B4](evidence/r2-b4-grant-production-audit-20260908.md) 新 concrete issuer 与 signed request 本地验收；4 independent wire/signature/unwraps PASS，实际 model-key/configuration owner 与默认 requester 待连接 | 2026-09-08 |
| [T005-B Requester Grant Publication](contracts/execution-units.md#t005-b-requester-grant-publication) | PARTIAL | T005-A | [R2-B4](evidence/r2-b4-grant-production-audit-20260908.md) 答复认证、Core publication/cancel/deadline 实现；sealer/Provider unwrap 组合 PASS；Core worker integration authored/T016，默认 requester 待迁移 | 2026-09-08 |
| [T006-A Canonical Source Identity](contracts/execution-units.md#t006-a-canonical-source-identity) | DONE | T002-A | [acceptance](evidence/t006-a-canonical-source-identity-20260907.md)；CPP(Spec182OnnxIdentity/*) 11 cases 全绿（24 v1 + 14 accepted extended 全模型 golden 逐字段、typed/raw pair 摘要恒等、v2 per-tensor 12 accepted 逐字节 + 5 拒绝、bf16 两编码归一、revision 分类、v2 descriptor binding 门、external/function-attr 内联等价、overflow/非法路径/限额拒绝） | 2026-09-07 |
| [T006-B Certified Extraction and Wire](contracts/execution-units.md#t006-b-certified-extraction-and-wire) | DONE | T006-A | [acceptance](evidence/t006-b-certified-extraction-wire-20260907.md)；CPP(Spec182OnnxExtraction/*) 11 cases 全绿（4 accept 逐字节 parity + 独立 sha256 交叉检查，7 reject 精确 reason family）+ Spec182NativeAssembly 3 cases + Spec182OnnxIdentity 11 cases 回归；官方 ONNX 1.17 full-pb 统一（--onnx-prefix）；data_location proto3-optional presence 奇点修正 byteParity（frozen sha 77300e13，diff 唯一 delta）；完整回归 5 个环境性失败（TPM/NFD）与本卡无关 | 2026-09-07 |
| [T006-C Bounded Native Worker](contracts/execution-units.md#t006-c-bounded-native-worker) | DONE | T006-B | [acceptance](evidence/t006-c-bounded-native-worker-20260907.md)；CPP(Spec182OnnxWorkerProtocol/*) 27 cases 全绿（frame 截断/溢出/重复帧逐字节状态机、compose/finalize 语义、metadata envelope 规范往返与 poison、真子进程 cancel 1304ms TERM→KILL escalation、信号死亡/静默/垃圾 stdout、PREFLIGHT 族）+ 842 cases 完整回归（除环境性 StreamFacade）；修复 isSha256Digest 长度门 66→71 root cause（failure-log 2026-09-07）；L0 staged install 链接验证（libexec/ndnsf-di，ELF/ldd clean，pythonWrapper 消息为设计内路径） | 2026-09-07 |
| [T006-D Protected Provider Activation](contracts/execution-units.md#t006-d-protected-provider-activation) | DONE | T006-C | [acceptance](evidence/t006-d-protected-provider-activation-20260907.md)；CPP(Spec182OnnxActivation/*) 9 cases 全绿（无 workerLocation 早拒、真实 worker 固定 manifest 装配两次同 digest、root/source digest mismatch、certified graph poison、cancel/timeout/pinned-hash tamper/信号死亡均在 sign 前拒绝且 checkNothingActivated）+ 回归锁 SubprocessChainRejectionPropagatesItsOwnCode 1 case；修复 T006-C 冻结 worker child catch off-by-one（14 vs 15 字节族前缀，全部链拒绝曾错标 DI_NATIVE_ONNX_WORKER_INTERNAL；failure-log 2026-09-07）；852 cases 完整回归（除环境性 StreamFacade）；Selection-after 真实冷装配 T016 | 2026-09-07 |
| [T007-A Full Tokenizer Ownership](contracts/execution-units.md#t007-a-full-tokenizer-ownership) | DONE | T002-A | [acceptance](evidence/t007-a-full-tokenizer-ownership-20260907.md)；CPP(Spec182NativeTokenizer/*) 3 gate cases + CPP(Spec182TokenizerFull/*) 7 cases 全绿：冻结三 profile 84 向量经真实静态链接 native ABI 逐字节对照、special flags 双路、负/超界/词表外 id 与无效 UTF-8/超限文本、digest-first 早拒（artifact 缺失与 digest mismatch 均在 engine create 前）、owner 5 轮复用确定性与 8×40 并发串行化；dlopen bridge 移除、bridgeLibrary option 删除、factory 兼容签名保留；wscript TOKENIZER_BRIDGE uselib（pinned Rust staticlib，--locked --offline -j2，独立 target 目录）链入 DI shlib 与全部 C++ link consumers；859 cases 完整回归（除环境性 StreamFacade，裸跑段错误负结果已记 failure-log 2026-09-07）；case-manifest T007-A 改名+7 named cases；失败修正：wscript 结构损坏 NameError + boost 1.71 vector 打印 | 2026-09-07 |
| [T007-B Stable Text Decoder Pair](contracts/execution-units.md#t007-b-stable-text-decoder-pair) | DONE | T007-A | [acceptance](evidence/t007-b-stable-text-decoder-pair-20260907.md)；CPP(Spec182TokenizerStable/*) 9 cases 全绿：冻结 stable-vectors.json 5 fixtures 28 rows （byte-fallback-special 11/bytelevel 13/reject-fuse 0/legacy-ascii 2/legacy-unicode 2，whole-file sha a80597b3…6254）经真实静态链接 native ABI 逐 cut 对照 decodeStable （skip/final 两维），每行 final==full 且前缀均为 finalText 文本前缀；合法 U+FFFD 保留、truncated/invalid 字节按 Rust-std 归因（tight E0/ED/F0/F4 约束）、specials skip 消失/retain 关 run、12 步交错无状态双遍一致、Fuse decoder stable 全调用 fail-closed 且完整 decode 不受影响、负/超界 id ABI 前拒；lib.rs Profile 分派 + stable_prefix 实现冻结算法，crate pin 零变化；GenerationTextDecodersFactory 收口 GenerationDecodersFactory；生产注入 留 T009-C/T011-B；859 cases 完整回归（除环境性 StreamFacade）；case-manifest T007-B 改名+9 named cases；失败修正：waf 外部 STLIB 内容不触发 relink（删产品重链）+ surrogate std-vs-python 代理分歧（按 std error 归因镜像重写；failure-log 2026-09-07） | 2026-09-07 |
| [T008-A Native Input and Artifact Preparation](contracts/execution-units.md#t008-a-native-input-and-artifact-preparation) | PARTIAL | T003-C, T006-D, T007-B | [Core publisher](evidence/t008-core-artifact-publisher-20260907.md) 保留；[完整模型绑定](evidence/t003-model-descriptor-20260907.md) 拒绝 revision/ABI/schema 替换，相关 81 cases PASS；实际 source inspection、模型发布配置、requester 接线和网络资格待完成 | 2026-09-07 |
| [T008-B Authenticated Offer Admission](contracts/execution-units.md#t008-b-authenticated-offer-admission) | PARTIAL | T008-A | [Core offer admission](evidence/t008-core-offer-admission-20260907.md)；已移除 policy 伪观测，Core candidate payload/Ed25519 gate 与相关 10 cases PASS；真实订阅接线、planner 完整观测消费及整卡验收待完成 | 2026-09-07 |
| [T009-A Core Scoped Registration](contracts/execution-units.md#t009-a-core-scoped-registration) | DONE | T006-D, T007-B | [acceptance](evidence/t009-a-core-scoped-registration-20260907.md)；CPP(Spec182Registration/*) 6 cases 全绿（新 suite di-native-provider-host.t.cpp）：ServiceRegistration move-only RAII（addScopedService 双载/addScopedCollaborationHandler，close 一次性消费 + closed atomic + Face post cleanup 闭包只 capture RegistrationControl/state，provider 先死 no-op）与 pending/collab registrationState 代次绑定（accept commit 在 finishAckDecisionOnEventLoop 写 pendingKey→state，与 pendingRequests 同生命周期，cleanupPendingRequestState 精确释放）+ dispatch 双路径同 fence（worker dispatchRequestExecutionAsync 入口与 pool-0 gateInlineRequestExecution，mismatch 拒 "generation changed"）；6 cases 覆盖晚到 ACK commit（ack worker 窗口 close → closed gate 正向降级负向，pending 不 store、handler 不执行）、旧代次 Selection after reregister（真实 worker decode + io-post commit 后 fence 同步拒，两代 executions 0）、同步面 inline 同 fence + gen2 正向对照执行成功、真实 cleanupPendingRequestState 后 collab mismatch gate 拒兄弟 role、provider 析构后 handle closed/valid/close no-throw、精确 pendingKey cleanup 不伤同 requester 独立 userToken 后继请求；编排关键修正：worker 面 accept commit 经 io post 落 Face 线程（drain handler pool 只保证 decode 完成），case 2 需 drain + pump io 后再 close（根因是编排非实现，inline 对照证明绑定写正常）；legacy 注册路径零行为变化、无 DI 类型/新 wire；899 cases 完整回归（除环境性 StreamFacade，负结果见 failure-log 2026-09-07）；case-manifest T009-A file 落位；跨服务 NFD 用例留 T016 | 2026-09-07 |
| [T009-B Shared Execution Lease State](contracts/execution-units.md#t009-b-shared-execution-lease-state) | DONE | T009-A | [acceptance](evidence/t009-b-shared-execution-lease-state-20260907.md)；CPP(Spec182SharedLease/*) 3 cases 全绿（新 suite di-native-provider-host.t.cpp）：SharedExecutionLeaseState（Core table + prepare mutex，host boot epoch，原四参数 constructor 保留并委托自有 state）+ shared_state overload + 非 Prepare 操作 find-then-route target 绑定检查（跨 target 行 LEASE_SERVICE_MISMATCH 且响应不泄漏 lease 细节，未知 lease 走 Core LEASE_NOT_FOUND，requester/epoch/state/replay 仍全由 Core 判定）；3 cases 覆盖双 target 争同物理槽 FIFO 等待队列（只允许一个 Prepare、host 不能双订、owner Abort/Release 后对称易主、retryAfterMs 100）、Executing lease 上另一 target 的 Commit/Abort/Renew/Release 全拒 + Released 行仍绑定 + 同 target 幂等 replay（状态未变窗口）与 Core 授权（REQUESTER_MISMATCH/STALE_EPOCH）、target 实例析构不释放执行中槽（shared state 保留 Executing 行、B 与重新 serve 的 A 都不能盗用、A owner 流 Release 后 B 继续 Prepare/执行）；旧 DiExecutionLeaseService 3 cases 零改动全绿（单 target 回归）；902 cases 完整回归（除环境性 StreamFacade）；关键修正：boost 1.71 逗号组合 selector 不可靠改单 selector 执行、FIFO 等待需同 requestId 重试、Core replay state 再验证使弃 idempotency 重放得 INVALID_TRANSITION（用例按真实语义修正）；跨服务真实双服务 PO-014 留 T016 | 2026-09-07 |
| [T009-C Shared Provider Host Wiring](contracts/execution-units.md#t009-c-shared-provider-host-wiring) | DONE | T009-B | [acceptance](evidence/t009-c-shared-provider-host-wiring-20260907.md)；CPP(Spec182ProviderHost/*) 6 cases 全绿（新 suite di-native-provider-host.t.cpp）：NativeInferenceProvider host 单例落地（首次 serve 发布单固定 lease 入口 addScopedService EXECUTION_LEASE_SERVICE_NAME + SharedExecutionLeaseState(host boot epoch)，makeLeaseRouter 按 targetServiceName 路由——miss=LEASE_SERVICE_MISMATCH、draining 且非 Abort/Release=LEASE_TARGET_DRAINING、内部错误 in-band LEASE_INTERNAL_ERROR、wire 恒 status=true 无异常跨 Core 回调）+ serve fence 族（boot 身份/槽位一致性 invalid_argument、同名 active duplicate logic_error 先于 config 检查、executionLeaseTargetService==serviceName 且 table 必须 null 由 host 注入共享表、draining record 替换不继承旧 lease/fence/bindings、guard handler 只盖 drain 窗口）+ close/stop 幂等（registration move-only RAII、close 抬 draining 再 core close、generation 直通、valid()=state 非空 close 后仍 true、stop 全关 + serve→runtime_error、provider owner reset 不提前关闭、host dtor 后 handle 安全）+ example 迁移（DI_NativeProviderExecutable.cpp serve 提前到 installTask、main 等待 serveCompleted cv、删旧 exec-lease 双注册块、config 不再注入 lease table、ackHandler/runtimeObserver 经 def seam、单一注册路径 CD-014）；6 cases 覆盖双 target 共享 host/duplicate 拒不波及 sibling、config 一致性三拒绝 + 顺序优先、close→同 name re-serve（generation 递增、旧 handle 保持 closed）、stop 语义全族、close 后晚到 ack 真实 Core 边界（ack 仍被询问/pending 到 cleanup boundary，fence 在 dispatch 层同 Spec182Registration selector 1/2）、固定入口真实 Core 全链 dispatch（A/B 双 target、close A 后 B 仍 Completed=PO-014、draining 晚到 Prepare 应答不牵连、re-serve 后新 target 正常）；collab handler 真实执行与真实 NFD 多入口（I/di-native-provider-host.t.cpp）留 T016；旧 Spec182Registration 6/Spec182SharedLease 3/DiExecutionLeaseService 3 零改动全绿；908 cases 完整回归（除环境性 StreamFacade）；wscript 零改动（unit-tests ant_glob ndnsf-di/*.cpp 自动收录）；case-manifest T009-C file 落位 + 6 named cases；失败修正：build-nac182 13:03 重配置丢 --with-examples 使 di-native-provider target 消失（补 configure 6.6s 恢复，非代码）、Core close fence 位置假设错误改测真实边界、dot-style provider. 遗留两处、测试常量与 T009-B 冲突 rename | 2026-09-07 |
| [T010-A Request Operation Terminal State](contracts/execution-units.md#t010-a-request-operation-terminal-state) | PARTIAL | T005-B, T008-B, T009-C | [Core I/O repair](evidence/t010-a-core-io-20260907.md)；postToIo/isOnIoThread、result I/O 等待拒绝、共享 user 寿命；-j4 构建 PASS，ClientState 11/11、既有 2/2 PASS；完整请求/成功竞争、有界通知待完成 | 2026-09-07 |
| [T010-B Complete Request Orchestration](contracts/execution-units.md#t010-b-complete-request-orchestration) | PARTIAL | T010-A | [R3-B1](evidence/r3-b1-request-lifecycle-20260908.md#final-local-result)：配置化client的初始请求/签名ACK/规划授权/Core commit/Response及取消本地通过；[R10-B31](evidence/r10-b31-native-client-unary-request-20260909.md) 新增真实 Core/Provider fixture 的 unary `NativeInferenceClient` 请求与最终 Response；[R10-B49](evidence/r10-b49-spec182-cpp-unit-suite-20260909.md) 249 个 `Spec182*` C++ unit cases 全绿；无runtime旧入口仍拒绝，跨进程/Provider worker/完整输入模式/stream 资格及调用方迁移待后续 | 2026-09-09 |
| [T010-C Stream Acceptance and Replacement](contracts/execution-units.md#t010-c-stream-acceptance-and-replacement) | PARTIAL | T010-B | [R4-B2](evidence/r4-b2-stream-production-20260908.md#final-local-result)：client/Core回调接受/恢复/final 7 cases/190 assertions与SDK wire PASS；T010-B完整依赖及真实stream验收保留 | 2026-09-08 |
| [T011-A Sampling Parity Repair](contracts/execution-units.md#t011-a-sampling-parity-repair) | PARTIAL | T010-C | [R4-B1](evidence/r4-b1-sampling-20260908.md)：真实epoch采样4 cases/40 assertions与独立参考PASS；T010-C及完整卡验收依赖仍未关闭 | 2026-09-08 |
| [T011-B Stable Epoch Emission](contracts/execution-units.md#t011-b-stable-epoch-emission) | PARTIAL | T011-A | [R4-B2](evidence/r4-b2-stream-production-20260908.md#final-local-result)：paired factory接线、full-only回退移除、requester final一致性本地通过；真实epoch/三处integration验收仍待完成 | 2026-09-08 |
| [T011-C Conversation Journal and Continuation](contracts/execution-units.md#t011-c-conversation-journal-and-continuation) | PARTIAL | T011-B | [R4-B4](evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring)；[R4-B5](evidence/r4-b5-public-conversation-20260908.md) 补齐本地公开 requester 的动态 envelope digest 与首轮成功；[R4-B6](evidence/r4-b6-provider-conversation-20260908.md) 通过真实 Provider receipt/control/commit 的 FULL_CONTEXT→APPEND_DELTA 两轮；[R7-B1](evidence/r7-b1-r4b6-replacement-20260908.md) 关闭单 Provider replacement 负例并记录首个 `ACK_CLOSED` 边界；成功 alternate-provider recovery、跨进程资格与 Provider runtime qualification 未完成 | 2026-09-08 |
| [T012-A Native Binding Types and Lifetime](contracts/execution-units.md#t012-a-native-binding-types-and-lifetime) | PARTIAL | T011-C | [descriptor binding](evidence/t003-model-descriptor-20260907.md) 暴露完整模型/adapter，单一 pybind11 DI 入口保持共享 native library；显式候选 Core/DI + NAC-ABE/SVS 前缀重建 extension、导入 PASS，四组 focused Python suites 21/21 PASS，见 [ABI evidence](evidence/t012-a-binding-abi-20260908.md)；真实 C++/Python 请求 parity、完整 native runtime construction、跨进程交付与整卡验收仍留 T012-B/T013/T016 | 2026-09-08 |
| [T012-B Compatible Python Facades](contracts/execution-units.md#t012-b-compatible-python-facades) | PARTIAL | T012-A | [R5-B2](evidence/r5-b2-native-runtime-binding-20260908.md)：native runtime construction、explicit facade route 和 public `InferenceClient` export 已完成 focused verification；真实 parity、maintained caller migration、旧路径退出与 T016 仍未验收 | 2026-09-08 |
| [T013-A Maintained Caller Migration](contracts/execution-units.md#t013-a-maintained-caller-migration) | PARTIAL | T012-B | [R5-B3 caller audit](evidence/r5-b3-maintained-caller-audit-20260908.md)；R5-B5 已固定 native runtime config parser/fixture，R5-B6/R5-B8 已把维护中的 Qwen requester 接到显式 native payload 与 callback-aware full-generation route，R5-B10 已给 YOLO User 增加显式 native payload branch，并让固定 tiny streaming harness 对不兼容配置 fail-closed；YOLO lifecycle/Provider request、native streaming execution、Provider retirement、真实请求与全量 migration 仍开放 | 2026-09-08 |
| [R5-B5 Maintained YOLO Native Requester](contracts/execution-units.md#r5-b5-maintained-yolo-native-requester) | PARTIAL | R5-B4; T010-B | [R5-B5 evidence](evidence/r5-b5-yolo-native-requester-20260908.md)：`DI_NativeRequester` runtime composition 已切换到 shared `nativeRequestRuntimeFromJson`；C++ parser selector、CLI help/usage/error、`-j4` build 和 25 Python binding/compatibility checks PASS；真实 Core/Provider request、维护中的 Python YOLO user、其余 caller 迁移与 T016 仍开放 | 2026-09-08 |
| [R5-B6 Qwen and Streaming Native Requesters](contracts/execution-units.md#r5-b6-qwen-and-streaming-native-requesters) | PARTIAL | R5-B5; R5-B6A; T011-C | [R5-B6 evidence](evidence/r5-b6-native-caller-migration-20260908.md)：Qwen 普通维护入口使用 operator-pinned native requester config、generation/stream DTO 和 `request_native_payload`；固定 tiny streaming harness 接受配置转发但在非 Qwen runtime 先拒绝，conversation 缺少 native continuation owner 时同样显式拒绝且不回退 Python。静态门、Python focused suite、shared DI `-j4` 增量构建通过；真实 native streaming/Core/Provider 请求、conversation owner、YOLO user、Provider retirement 与 T016 仍开放 | 2026-09-08 |
| [T013-C Native Stream Observer Facade](contracts/execution-units.md#t013-c-native-stream-observer-facade) | PARTIAL | T012-A; T013-A; T010-A | [R5-B7 evidence](evidence/r5-b7-native-observer-20260908.md)：`NativeInferenceHandle.observe` 已绑定为受控 Python callback，`request_native_payload(on_event=...)` 在提交后挂载；C++ observer selectors、extension rebuild 和 28 Python focused cases PASS；真实 callback delivery through Provider、streaming parity 与 T016 仍开放 | 2026-09-08 |
| [T013-D Native Qwen Stream Callback Route](contracts/execution-units.md#t013-d-native-qwen-stream-callback-route) | PARTIAL | T013-C; T012-B | [R5-B8 evidence](evidence/r5-b8-native-qwen-stream-callback-20260908.md)：维护中的 Qwen full-generation native route 已接入 observer 快照、`GenerationTokenEventV1` 校验与 terminal ordering；28 Python cases 及 C++ observer/token-order selectors PASS；真实 Provider/cross-process streaming 仍未验收 | 2026-09-08 |
| [T013-E Native Conversation Owner Configuration](contracts/execution-units.md#t013-e-native-conversation-owner-configuration) | PARTIAL | T011-C; T012-B | [R5-B9](evidence/r5-b9-native-conversation-owner-20260908.md) 已完成 C++ owner-only key/path 读取、journal/coordinator 构造、ServiceUser identity/service/digest 校验、opaque binding 与 focused selector；真实 Provider/cross-process 两轮、recovery/replacement 与 T016 仍未验收 | 2026-09-09 |
| [R5-B6A Native Generation Stream Conversation Option Binding](contracts/execution-units.md#r5-b6a-native-generation-stream-conversation-option-binding) | DONE | T011-C; T012-A | [R5-B6A evidence](evidence/r5-b6a-native-option-bindings-20260908.md)：generation/stream/controller/conversation DTO 与 `NativeRequestOptions` nested fields 已绑定；C++ stream validation/wire round-trip、explicit candidate rebuild 与 10 Python focused cases PASS；真实 caller migration、完整 lifetime/parity 与 T016 仍开放 | 2026-09-08 |
| [T013-F Maintained YOLO Native Reference Route](contracts/execution-units.md#t013-f-maintained-yolo-native-reference-route-r5-b10r10-b3) | PARTIAL | T013-A; T008-A; T010-B | [R5-B10 evidence](evidence/r5-b10-yolo-native-payload-route-20260908.md) 与 [R10-B3 evidence](evidence/r10-b3-yolo-native-reference-caller-20260909.md)：显式 native config、YOLO package/model identity、加密 tensor bundle publication、journal-bound `REPO_REF`、task/options 与 fail-closed shutdown branch 已写入；`py_compile`、source checks、24 compatibility cases、`git diff --check` PASS；Spec180 lifecycle、真实 Core/Provider 与 numeric parity 未验收 | 2026-09-09 |
| [T013-B Legacy Runtime Retirement](contracts/execution-units.md#t013-b-legacy-runtime-retirement) | PARTIAL | T013-A | [R5-B11 legacy reachability audit](evidence/r5-b11-legacy-reachability-audit-20260908.md)：重新生成 344-entry compatibility manifest，核对 provider/runtime/facade/placement entries 均有 repository consumers 或 external-use-unknown，`removalEligible=false`；5 个 source/retirement checks PASS；真实 maintained caller zero-use、默认 import graph 与 T016 仍未验收 | 2026-09-08 |
| [T014-A Isolation Collector Semantics](contracts/execution-units.md#t014-a-isolation-collector-semantics) | PARTIAL | T013-B | [R6-B8 evidence](evidence/r6-b8-collector-role-cold-20260909.md) + [R10-B20 evidence](evidence/r10-b20-collector-evidence-boundary-20260909.md) + [R10-B23 evidence](evidence/r10-b23-runner-working-directory-20260909.md) + [R10-B29 evidence](evidence/r10-b29-runner-multiprocess-lifecycle-20260909.md) + [R10-B30 evidence](evidence/r10-b30-runner-child-endpoint-observation-20260909.md) + [R10-B40 evidence](evidence/r10-b40-t016-po001-native-owner-pass-20260909.md) + [R10-B41 evidence](evidence/r10-b41-stream-business-oracle-20260909.md) + [R10-B47 evidence](evidence/r10-b47-t016-po001-owner-pass-20260909.md) + [R10-B48 evidence](evidence/r10-b48-t016-i01-consumer-owner-pass-20260909.md)：collector 现从实际 PID/exec/exit/namespace/endpoint/cleanup 观察生成六类 evidence，并支持显式独立 stdout marker；多进程角色、节点 FD、进程组、deadline、每进程环境、named child/clone 绑定和 endpoint 语义已接通；40 个 Python cases、`py_compile`、`git diff --check` PASS，I01 与 `PO-001` 已有真实 native 正向观察；I02-I08、child/endpoint 真实反例与完整 T016 仍开放 | 2026-09-09 |
| [T014-B Qualification Harness Registration](contracts/execution-units.md#t014-b-qualification-harness-registration) | PARTIAL | T014-A | [R6-B2 evidence](evidence/r6-b2-harness-registration-20260908.md) + [R10-B19 evidence](evidence/r10-b19-owner-runner-handoff-20260909.md) + [R10-B21 evidence](evidence/r10-b21-native-consumer-i01-pass-20260909.md) + [R10-B23 evidence](evidence/r10-b23-runner-working-directory-20260909.md) + [R10-B29 evidence](evidence/r10-b29-runner-multiprocess-lifecycle-20260909.md) + [R10-B30 evidence](evidence/r10-b30-runner-child-endpoint-observation-20260909.md) + [R10-B40 evidence](evidence/r10-b40-t016-po001-native-owner-pass-20260909.md) + [R10-B41 evidence](evidence/r10-b41-stream-business-oracle-20260909.md) + [R10-B47 evidence](evidence/r10-b47-t016-po001-owner-pass-20260909.md) + [R10-B48 evidence](evidence/r10-b48-t016-i01-consumer-owner-pass-20260909.md)：I01-I08 与 PO-001-PO-014 仍注册在唯一 manifest，campaign owner 已在真实 MiniNDN owner 存活期间接 canonical runner；多进程 requester/provider 的启动、独立环境、namespace FD、统一清理、child/endpoint 声明校验、clone child 绑定与 syscall 观察已实现；I01 native consumer 与 `PO-001` native DI selector 返回 PASS；其余负例、真实跨进程 transport、maintained caller/no-Python 与 T016 仍未完成 | 2026-09-09 |
| [T015-A CrossTask Convergence](contracts/execution-units.md#t015-a-crosstask-convergence) | PARTIAL | T014-B | [R6-B3 evidence](evidence/r6-b3-cross-task-convergence-20260908.md)：已核对 native requester、maintained caller、Provider host、legacy manifest、collector/harness 和 T016 出口；已知缺口归属 T004/T008/T009/T010/T011/T013/T016，未放宽最终资格 | 2026-09-08 |
| [T016-A Local Qualification](contracts/execution-units.md#t016-a-local-qualification) | PARTIAL | T015-A | [R6-B4 preflight evidence](evidence/r6-b4-t016-preflight-20260908.md) + [R10-B38 recheck](evidence/r10-b38-t016-runtime-context-recheck-20260909.md) + [R10-B40 evidence](evidence/r10-b40-t016-po001-native-owner-pass-20260909.md) + [R10-B41 evidence](evidence/r10-b41-stream-business-oracle-20260909.md) + [R10-B47 evidence](evidence/r10-b47-t016-po001-owner-pass-20260909.md) + [R10-B48 evidence](evidence/r10-b48-t016-i01-consumer-owner-pass-20260909.md)：root MiniNDN owner 现可启动并保持 requester/provider namespace，I01 installed consumer 与 PO-001 native DI canonical runner 均有真实 PASS（native process rc 0、business marker、namespace/process/cleanup evidence）；完整 I01-I08/PO-001-PO-014 matrix、maintained caller/no-Python 和最终 T015 gate 仍未完成，不能升级为 T016 DONE | 2026-09-09 |
| [T017-A Development Handoff](contracts/execution-units.md#t017-a-development-handoff) | NOT_STARTED | T016-A | [baseline](evidence/task-progress-registry-20260907.md)；无本卡独立执行/验收记录；按依赖领取 | 2026-09-07 |

## Remaining Production Chain

暂停新增实现后的执行顺序按真实生产出口固定如下；这是现有 T010–T017 卡的执行视图，
不新增行政任务、不改变父任务依赖或完成状态。每个阶段在自己的 Batch ID 中完成五 lane
静态审查、C++ selector/source closure 和真实结果观察，未达到出口保持 `PARTIAL`。

| Order | Existing cards | Stable exit | Depends / split trigger |
| --- | --- | --- | --- |
| P1 | `T004-A`, `T008-A/B`, `T010-A/B` | Native requester 从准备/授权进入真实 Core，并从 Provider 得到 unary/stream 结果；保留 cancel/deadline/terminal 负例 | 复用 R10-B31/R10-B33 作为局部基线；增加 Provider worker、caller 或新 selector 即拆新批 |
| P2 | `T010-C`, `T011-C` | 同一请求契约跨 Provider worker/进程完成首轮与续接，含 receipt/control/commit、恢复和清理 | 依赖 P1；需要 NFD/MiniNDN 或跨进程 transport 时在 owner 环境执行，不用本地 fixture 替代 |
| P3 | `T012-A/B`, `T013-A`, `T013-F` | 一个维护中的 YOLO caller 使用 native facade/`REPO_REF` 完成真实请求、结果回收和 rollback evidence | 依赖 P1/P2 与 binding source closure；Python source/compatibility 检查不能单独关闭 caller |
| P4 | `T013-C`, `T013-D`, `T013-E`, `T011-C` | Qwen/streaming caller 通过 native observer 交付 token/terminal 顺序；配置 continuation owner 后完成跨进程两轮和 replacement | 依赖 P2；conversation metadata 不由 Python 猜测，缺 owner 时继续 fail-closed |
| P5 | `T013-B` | maintained callers 零使用旧 runtime/default import graph，并保留兼容退出与回滚证据 | 仅在 P3/P4 各有真实 native 结果后执行；legacy manifest 不能替代零调用观察 |
| P6 | `T014-A/B` | 隔离 collector/harness 观察 native scope、子进程、endpoint、清理和必要反例 | 依赖 P5；I02–I08 与真实 no-Python 反例由 T016 owner 执行 |
| P7 | `T015-A` → `T016-A` → `T017-A` | 收敛 PASS 后完成 unit→integration→MiniNDN/no-Python 资格，再生成唯一 handoff | 每一步保留首个失败边界；T016 缺 NFD/node context 时保持 `UNQUALIFIED`，不前移 T017 |

P1–P4 是不同的生产入口、进程边界或 selector，不能为了少一次构建合并；P5–P7 只在
前置真实结果闭合后推进。局部 fixture、CLI smoke 和 Python compatibility PASS 均不等同
于 native qualification。详见 [R10-B36 evidence](evidence/r10-b36-production-chain-reorder-20260909.md)。

## Batch Quality Record

批次结果沿共享 [batch-quality-gates](../../skills/speckit-code-design/references/batch-quality-gates.md)
维护；Coverage matrix 是静态门与批末门的必要证据，不另建每任务报告。历史批次未回填
矩阵，保持其原始证据和状态；从本修订起的新批次必须填写五个 lane、`Review trace` 和
`Closure decision`。前者包括 review-agent skill 路径/SHA、基线、完整 diff 范围、覆盖查询
和复审结果；后者使用 `CLOSED_FOR_VALIDATION` 或 `OPEN_FOR_NEXT_BATCH`，说明稳定行为出口、
触发条件、未纳入成员及下一批依赖。Revision 23 及更早的历史行不回填不可核对的信息；
重开时按共享 reference 补齐。若批次是失败重试或同类漏检后的下一批，`Evidence / remaining`
还必须链接首个失败边界并写明本次改变的静态检查；只重跑原命令不能关闭漏检。

| Batch ID | Coverage matrix | Static findings | Compile/build misses | Runtime/test misses | Build scope / target / `-j` / elapsed / exit | Behavior result | Evidence / remaining |
| --- | --- | --- | --- | --- | --- | --- | --- |
| R10-B77 | production entry/callers: standalone `di-native-provider` Waf target; implementation/wire: target link flags and staged shared-library lookup; test/harness/oracle: Python target-registration regression plus `readelf` RUNPATH inspection; build/source closure: system-first Waf target rebuild from current source, artifact SHA and raw build log; migration/evidence: no Provider process or network request started | static review found the Provider target lacked the `$ORIGIN/..` lookup used by the requester and native smoke targets; no additional P1/P2/P3 finding | no compile/link miss; 90/90 target steps linked successfully in 7.270s | no runtime protocol result; `readelf` contains `$ORIGIN/..`, while remaining host NAC-ABE/SVS/NDNSD dependencies are recorded as an open deployment-closure boundary | `env PATH=/usr/bin:/bin:/usr/sbin:/sbin ./waf -o .codex-tmp/spec182-p1-provider-rpath/build build --targets=di-native-provider -j2`; target artifact SHA `7bd314a551c6ec6c31b5c4b8c216e50bd9f4b30734d4685082dcb5a4faa2b11e`; Python focused test exit 0 | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS` for target RUNPATH wiring; `CLOSED_FOR_VALIDATION` for this boundary; not Provider transport/T010/T016 qualification | [R10-B77 evidence](evidence/r10-b77-provider-runpath-20260910.md); next P1 batch must stage the complete closure and launch independent requester/provider processes |
| R10-B78 | production entry/callers: `di-native-provider --serve` → `ServiceProvider::serve`/Face loop; implementation/wire: bounded `--run-for-ms` serve lifetime and explicit installation-thread join/drain; test/harness/oracle: Provider CLI parser/source regression plus bounded check-only/serve lifecycle probe; build/source closure: current Provider target and registered source list under system-first `-j2`; migration/evidence: establishes a deterministic finite Provider process for the next independent requester/Provider case, without claiming transport or qualification | CLOSED_FOR_VALIDATION | allocation basis: finite daemon exit and callback lifetime are shared prerequisites for P1/P2; stable exit is `SERVE_READY` followed by run-limit shutdown with joined install thread; no requester/Provider request is claimed in this batch | source/static checks, requester/Provider target build, and controller-assisted finite serve probe passed; Provider emitted `RUN_LIMIT_REACHED` and `PERMISSION_WAIT_CANCELLED`, Provider/Controller exited 0 | bounded lifetime result is focused behavior only; parent P1/P2/T010/T016 remain PARTIAL | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for this boundary; parent P1/P2/T010/T016 remain PARTIAL | [R10-B78 evidence](evidence/r10-b78-provider-run-limit-20260909.md); next: use the finite Provider in an independent requester/Provider manifest |
| R10-B79 | production entry/callers: current Spec182 C++/Python/qualification call graph; implementation/wire: requester/provider/stream/continuation contracts and caller migration; test/harness/oracle: CodeGraph, Python AST, Cppcheck, structure/validator, and registration-vs-runnable checks; build/source closure: current R10-B78 Provider build, `readelf`/`ldd`, and source registration; migration/evidence: read-only large static audit of implementation distance and static miss classes | OPEN_FOR_NEXT_BATCH | allocation basis: whole-chain audit before the next implementation batch; no parent task status advanced; stable audit exit is one evidence record with prioritized findings and a closure decision | follow-up found SA-01 and SA-03; both repairs now pass focused checks. SA-02/SA-05/SA-06 remain open, SA-04 is a registration→transient-runner process gap, and runtime transport, cross-process, continuation/recovery, no-Python migration, and deployment qualification remain unobserved | `STATIC_PASS` for the audit/inventory boundary plus focused closure of SA-01/SA-03; it is not a product or protocol qualification result | [R10-B79 evidence](evidence/r10-b79-large-static-audit-20260909.md); next: make registration→runner handoff explicit, then run one independent requester→Core→Provider case |
| R10-B80 | production entry/callers: native runtime JSON loader → NativeInferenceClient → Core BeginCollaboration → real ServiceProvider; implementation/wire: runtime contract tokenizer digest as sole conversation authority; test/harness/oracle: native-config Qwen stream and conversation selectors with persisted transcript digest assertion; build/source closure: integration-tests rebuilt from current source with system-first `-j2`, changed-source Cppcheck and target registration; migration/evidence: no Python planner/runtime in the Qwen path, while independent worker/cross-process transport, caller migration and T016 remain open | CLOSED_FOR_VALIDATION | allocation basis: close SA-07 before independent process work; stable exit is native-config Qwen stream/conversation terminal result plus transcript tokenizer digest equal to runtime.contract.tokenizerDigest and distinct from model.semanticsDigest | commit path now rejects missing/mismatched runtime tokenizer identity; integration target 118/118 and R10-B* five-case sweep pass; no parent task advanced | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for tokenizer authority and in-process Qwen stream/conversation only | [R10-B80 evidence](evidence/r10-b80-native-config-qwen-conversation-20260910.md); next: construct the independent requester/Provider worker case |
| R10-B81 | production entry/callers: `NativeInferenceClient` → Core collaboration → Provider plus 91 maintained Python files and native YOLO branch; implementation/wire: requester/provider/stream/conversation/cleanup contracts, Waf registration and runtime envelope; test/harness/oracle: CodeGraph, Python AST, broad Cppcheck, Clang analyzer, `nm`/`readelf`, integration and unit selectors; build/source closure: current-source integration 118/118 and unit 188/188 rebuilds under system-first `-j2`; migration/evidence: 16 actual maintained inference calls remain on compatibility/automatic-planner APIs, while independent worker/process, no-Python and host/container closure remain open | OPEN_FOR_NEXT_BATCH | broad static audit found and repaired a redundant cleanup condition and removed an analyzer ambiguity; the prior token-budget finding was retracted after full-path tracing; no new compile/link miss; one pre-existing unit warning remains | patched cancellation and R10-B* selectors pass; independent process/transport/continuation/recovery, caller migration and deployment qualification remain unobserved | `./waf build --targets=integration-tests -j2` 22.21s; `./waf build --targets=unit-tests -j2` 4m10.92s; full `Spec182*` 251 cases pass; max RSS 8.396 GB on the full unit run | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS` for local repair only; parent T010/T011/T013/T014/T015/T016/T017 remain open | [R10-B81 evidence](evidence/r10-b81-large-static-audit-20260910.md); next: construct the independent requester/Provider worker case |
| R10-B50 | production entry/callers: `examples/DI_NativeProviderExecutable.cpp` standalone Provider target; implementation/wire: Waf `di_native_session_sources` plus collaboration/ONNX source closure; test/harness/oracle: linker symbol-definition lookup, `ldd` dependency closure and CLI usage probe; build/source closure: system-first `/usr/bin/g++ -B/usr/bin`/binutils, existing `.codex-tmp/spec182-r4-b2/build`, source commit after repair; migration/evidence: three fresh link failures are retained before the repaired build and no Provider process was started until link success | initial static/source-closure miss omitted 18 native translation units from the Provider target across the retry sequence; the changed gate enumerated each definition file and final review found no P1/P2/P3 | first three attempts failed only at `ld` after 72/78/80 compile tasks; no runtime was classified; repaired target linked 80/80 with no unresolved symbols | repaired `di-native-provider` build exit `0`, elapsed `39.34s`, artifact SHA `4be6b29acb10b29792757a23ce0ffc4f098f39e9cec2f0f47f4175d03bdae75a`; `ldd` key dependencies resolved; `--help` remains unsupported and returned usage/exit `2` | system-first `./waf build --targets=di-native-provider -j2`; no Provider serve or network request; next native build remains `-j2` after observed swap-in | `STATIC_PASS`; `BUILD_PASS`; `CLOSED_FOR_VALIDATION` for standalone Provider link/source closure; not Provider runtime or T016 qualification | [R10-B50 evidence](evidence/r10-b50-provider-link-closure-20260909.md); Provider serve, cross-process transport, maintained caller/no-Python and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B51 | production entry/callers: repaired `di-native-provider` → `parseArgs`/`loadPlan`/`loadManifestSpecs` → ONNX Runtime factory → `NativeProviderSession::registerRunner`; implementation/wire: four-role DATA_DRIVEN_V2 plan and manifest from `spec174-exact-bundle-gpu-v5`; test/harness/oracle: standalone `--check-only` process exit plus `NDNSF_DI_EXECUTION_EVIDENCE` and `NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`; build/source closure: Provider binary from source commit `7b0e70268b77b117d647b45890a5453859a7b6a8`, SHA retained, `ldd` closure checked; migration/evidence: fresh raw run directory and exact relative-artifact working directory | no P1/P2/P3 finding; static review covered argument mode, service/role alignment, artifact path resolution, evidence aggregation and registration output | no compile/link miss; this batch reused the already repaired binary and did not rebuild | real ONNX Runtime CPU runner loaded and warmed all four roles (`/Backbone`, `/Head/Shard/0`, `/Head/Shard/1`, `/Merge`); process exit `0`, elapsed `0.05s`, `loadCompleted=true`, `warmupCompleted=true`, `cpuFallbackUsed=false`; no NDN process or terminal Response was attempted | exact check-only command under `.codex-tmp/spec174-exact-bundle-gpu-v5`; no additional native build, no resource promotion | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for Provider load/warmup/registration; not T010/T013/T016 qualification | [R10-B51 evidence](evidence/r10-b51-provider-check-only-20260909.md); Provider `--serve`, independent requester/Provider transport, maintained caller/no-Python and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B52 | production entry/callers: Waf `DI_NativeRequester` target → `DI_NativeRequester.cpp::main` → native catalog/grant/runtime composition; implementation/wire: CLI argument parser and `ndnsf-di-native-requester-v1` schema gate; test/harness/oracle: help, bare invocation and wrong-schema probes with output-file rejection check; build/source closure: system-first Waf incremental target check, requester binary SHA and `ldd` closure; migration/evidence: fresh raw build and CLI run directories, no Python planner or network process | no P1/P2/P3 finding; read-only review covered argument order, schema fail-closed, relative paths, output-write timing and target registration | no compile/link miss; Waf target check exit `0`, elapsed `1.03s` | `--help` exit `0`, bare invocation exit `2`, wrong schema exit `1` with `NATIVE_REQUESTER_FAILED: unsupported requester configuration`; no output written on rejection | `./waf -o .codex-tmp/spec182-r4-b2/build build --targets=DI_NativeRequester -j2`; no new native rebuild beyond target check; resource policy remains `-j2` until swap evidence clears | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for requester build/CLI boundary; not T010/T016 qualification | [R10-B52 evidence](evidence/r10-b52-requester-cli-build-20260909.md); valid requester config, Core/Provider transport, maintained caller/no-Python, conversation and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B53 | production entry/callers: `DI_NativePlanOnnxSmoke.cpp::main` → plan/manifest loaders → `NativeProviderSession::executeRoleAsync`; implementation/wire: `NativeExecutionPlanJson`, `NativeServiceManifest`, ONNX Runtime runner and dependency I/O; test/harness/oracle: existing four-role `spec174-exact-bundle-gpu-v5` smoke marker plus dependency/output assertions; build/source closure: system-first Waf target 86/86 with `-j2`, `$ORIGIN/..` RUNPATH repair in source checkpoint `085359eb`, candidate framework/binary hashes and default `ldd` path; migration/evidence: fresh raw run preserves loader, artifact-cwd and command-construction first boundaries | first run found `/usr/local/lib` runtime framework mismatch (`rc=127`); candidate-library run found bundle-relative artifact cwd miss (`rc=2`); first no-env RPATH retry exposed only a harness `$PWD` construction miss (`rc=127`); all boundaries were logged before the corrected retry; final review had no new P1/P2/P3 | no compile/link miss after 86/86 build; RPATH relink exit `0` in `5.80s`; loader/cwd/harness misses were classified separately | final no-env run from bundle root executes all four ONNX roles, publishes 4 dependency objects and 440 output bytes, `NDNSF_DI_NATIVE_PLAN_ONNX_SMOKE_OK`, exit `0` in `0.06s` | `./waf -o .codex-tmp/spec182-r4-b2/build build --targets=di-native-plan-onnx-smoke -j2`; target RUNPATH selects candidate framework by default; next native build remains `-j2` | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for local plan/ONNX session and runtime library identity; not Provider transport or T016 qualification | [R10-B53 evidence](evidence/r10-b53-plan-onnx-smoke-20260909.md); Provider `--serve`, requester/Provider transport, maintained caller/no-Python and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B49 | production entry/callers: registered `unit-tests` `Spec182*` suites covering NativeInferenceClient, ClientState, Conversation, NativePlanning, Preparation, PlanSealer, Onnx, Grant and ProviderHost; implementation/wire: C++ production targets and canonical protocol/state validators; test/harness/oracle: Boost.Test exact wildcard `--run_test='Spec182*'` with 249 registered cases; build/source closure: existing `.codex-tmp/spec182-r4-b2/build/unit-tests` SHA `f10e68b47d9ba48c7ebe155d09c133f959d8d1f952c8645a96d3ac37c567b0fc`, source commit `6c7ba388f4a87e436b1972f3a21b8dcee5bb7571`, no source change or rebuild; migration/evidence: command, digest, elapsed time, and resource snapshot preserved in the R10-B49 record | no P1/P2/P3 finding; no source diff in this regression batch | no compile/link miss; existing binary reused and identity recorded | 249/249 C++ cases exit `0`, `*** No errors detected`, elapsed `27.72s`; independent requester/Provider transport, maintained caller/no-Python and complete T016 remain unobserved | existing unit binary only; `/usr/bin/time` command exit `0`; post-header `vmstat` showed `si/so=528/0`, `4800/0`, `132/0`, so next native build is constrained to `-j2` | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for the unit-suite boundary; not product qualification | [R10-B49 evidence](evidence/r10-b49-spec182-cpp-unit-suite-20260909.md); cross-process, maintained caller/no-Python and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B47 | production entry/callers: `Experiments/NDNSF_DI_NativeClosure_Minindn.py` owner → canonical `tests/standalone/run-spec182-native-closure.py` → staged `integration-tests` requester process; implementation/wire: existing R4-B6 native conversation requester/Provider fixture and `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` business oracle; test/harness/oracle: PO-001 manifest, MiniNDN node context, seven required evidence classes and trace/policy integrity evaluator; build/source closure: current `.codex-tmp/spec182-r4-b2/build/integration-tests` executable SHA recomputed before staging; migration/evidence: fresh `.codex-tmp/spec182-t016-r10-b46-owner5/` preserves node/PID/namespace/socket, process, stdout/stderr/trace and cleanup records | first owner attempts exposed missing `/usr/local/bin/infoconv`, invalid historical process role, and stale executable digest; all were preserved and fail closed before protocol classification; corrected retry had no P1/P2/P3 finding | no compile/link miss; staging digest preflight caught the changed executable and was repaired by recomputing the transient manifest hash | PO-001 owner/runner exit 0, native process rc 0 in 8001ms, marker present, complete evidence and empty integrity/policy violations; I02-I08, PO-002-PO-014 and independent requester/Provider transport remain unobserved | owner command used `sudo -n` with runtime `PATH` including `/usr/local/bin`; no native rebuild; runner returned PASS | `STATIC_PASS`; `FOCUSED_QUALIFICATION_PASS` for PO-001 only; `CLOSED_FOR_VALIDATION` for this boundary; not T016 qualification | [R10-B47 evidence](evidence/r10-b47-t016-po001-owner-pass-20260909.md); T016, maintained caller/no-Python and cross-process transport remain `PARTIAL`/`UNQUALIFIED` |
| R10-B48 | production entry/callers: `Experiments/NDNSF_DI_NativeClosure_Minindn.py` owner → canonical `tests/standalone/run-spec182-native-closure.py` → staged `spec182-installed-consumer` requester process; implementation/wire: installed native consumer executable and its declared shared-library closure; test/harness/oracle: I01 marker `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK`, complete trace and seven required evidence classes; build/source closure: current manifest-declared artifact hashes staged without source changes or rebuild; migration/evidence: fresh `.codex-tmp/spec182-t016-r10-b48-owner/` preserves node context, runner result, stdout/stderr/trace and cleanup | no P1/P2/P3 finding; existing I01 manifest role and marker satisfy runner vocabulary | no compile/link miss; no source changed and no native rebuild required | I01 owner/runner exit 0, staged process rc 0 in 344ms, marker present, complete evidence and empty violations; DI protocol, I02-I08, PO-002-PO-014, independent requester/Provider transport and maintained caller/no-Python remain unobserved | owner command used `sudo -n` with `/usr/local/bin`; no native build; runner PASS | `STATIC_PASS`; `FOCUSED_QUALIFICATION_PASS` for I01 only; `CLOSED_FOR_VALIDATION` for this boundary; not T016 qualification | [R10-B48 evidence](evidence/r10-b48-t016-i01-consumer-owner-pass-20260909.md); remaining T016 matrix and product parents remain `PARTIAL`/`UNQUALIFIED` |
| R10-B46 | production entry/callers: `NativeInferenceClient::request` → `ServiceUser` Core collaboration → real `ServiceProvider` callback in the existing R4-B6 fixture; implementation/wire: existing preparation/grant/admission/Core commit and Provider final/stream response paths; test/harness/oracle: exact `Spec170NdnsfDiCoreFlow/Spec182*` filter runs seven registered selectors, including unary inline, unary `REPO_REF`, stream, conversation and replacement boundaries; build/source closure: existing `.codex-tmp/spec182-r4-b2/build/integration-tests`, system-first Waf target check; migration/evidence: no Python runtime, independent requester/Provider transport and T016 remain outside this batch | no new P1/P2/P3 finding after read-only review; selector registration and expected replacement failure are explicit | no compile/link miss; source unchanged, Waf incremental target check only | seven selectors exit 0; five `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` markers and one expected `DI_NATIVE_NO_ADMITTED_PROVIDER` replacement failure; cross-process and qualification remain unobserved | `./waf build --targets=integration-tests -j4`, 0.851s incremental; selector suite 37.97s, exit 0; `vmstat` post-header samples show no sustained swap | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for local in-process selectors; not product qualification | [R10-B46 evidence](evidence/r10-b46-real-provider-native-suite-20260909.md); Provider worker/cross-process, maintained caller/no-Python and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B45 | production entry/callers: 11 installed `.agents/skills/speckit-*` feature-facing entrypoints; implementation/wire: `skills/README.md` and `verify-spec-kit-sync.py` marker groups; test/harness/oracle: normal and removed-marker checker probes with entrypoint marker enforcement; build/source closure: N/A (documentation/checker-only); migration/evidence: local entrypoint copies remain outside Git by policy and are checked against the versioned workflow | initial gap was that shared code-design preflight was not explicit in the installed entrypoints; all 11 now contain `Feature Sync Preflight`, and the checker enforces the command and flag markers; official review found no P1/P2/P3 | none; `py_compile` only | forced checker passes 11/11 plus personal copy; a temporary entrypoint without the marker fails as expected; native request/result, maintained caller and T016 remain unobserved | documentation/checker-only; `python3 -m py_compile ...verify-spec-kit-sync.py`; no `-j` build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for workflow preflight only; not product qualification | [R10-B45 evidence](evidence/r10-b45-entrypoint-preflight-enforcement-20260909.md); product parents and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B34 | production entry/callers: existing native requester, Provider host, R4-B6 fixture and public facade inspected; no source changed; implementation/wire: current unary/stream/repository boundaries remain as recorded; test/harness/oracle: exact `Spec182*`, `Spec170NdnsfDiCoreFlow/*` and Python compatibility selectors executed; build/source closure: documentation-only, existing binaries reused; migration/evidence: raw logs preserve expected negative boundaries and external qualification gaps | no new findings; regression-only batch | none; no source changed and no rebuild required | no local suite misses; cross-process transport, maintained caller/no-Python and T016 remain unrun | existing `build-nac182` binaries; C++ unit 30.68s, integration 103.84s, Python 3.03s; all exit 0 | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for regression boundary; not `QUALIFICATION_PASS` | [R10-B34 evidence](evidence/r10-b34-regression-sweep-20260909.md); product migration and T016 remain open |
| R10-B35 | production entry/callers: N/A (workflow documentation); implementation/wire: versioned `batch-quality-gates.md`, code-design skill and README; test/harness/oracle: targeted reference/entry-copy checks and `validate_design.py`, no native selector; build/source closure: N/A (no product target); migration/evidence: personal code-design SHA pair plus 11 `.agents/skills/speckit-*` copies checked, product qualification unchanged | no actionable findings; output contract is now explicit at entry points | none; documentation-only | none observed; native request/result, maintained caller/no-Python, cross-process and T016 remain unrun | documentation-only; `git diff --check`, reference-link scan, targeted `rg`, SHA-256 comparisons and validator; no product build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for workflow boundary; not `QUALIFICATION_PASS` | [R10-B35 evidence](evidence/r10-b35-command-output-contract-20260909.md); product parents and T016 remain open |
| R10-B36 | production entry/callers: N/A (execution-order documentation); implementation/wire: plan/tasks dependency and card references only; test/harness/oracle: existing named selectors and external owner gates are mapped but not run by this batch; build/source closure: N/A (no product source changed); migration/evidence: R5-B3 caller matrix, R10-B31/B33 boundaries and T016 preflight retained | no actionable findings; reorder makes caller/process/qualification boundaries explicit | none; documentation-only | no runtime executed; the listed P1–P7 exits remain open or partial as their cards state | documentation-only; plan/tasks link and dependency checks, `git diff --check`, validator; no native build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for ordering boundary; not `QUALIFICATION_PASS` | [R10-B36 evidence](evidence/r10-b36-production-chain-reorder-20260909.md); next work starts at P1 and stops at each stable exit |
| R10-B37 | production entry/callers: `NativeInferenceClient::request` through `ServiceUser::BeginCollaboration` to the R4-B6 real `ServiceProvider` callback; implementation/wire: existing preparation/grant/admission/Core commit plus a stream-only Provider branch emitting `GenerationTokenEventV1` and `NDNSF-DI-FINAL-V1`; test/harness/oracle: new named stream-only selector asserts token/final validation and exact native result, with unary/conversation/repository regressions; build/source closure: `integration-tests` source registration and system-first `-j4`; migration/evidence: no conversation metadata, Provider worker/cross-process, maintained caller/no-Python or T016 claim | no actionable finding after five-lane read-only review; generation identity and no-conversation ownership are explicit | no compile/link miss; existing aggregate initialization warnings remain | new stream-only selector (4 assertions), four R4-B6 selectors and two unary selectors exit 0; comma-filter invocation was a corrected command selection error | system-first `-j4` integration rebuild, 118/118 steps, 35.854s, exit 0; `vmstat` after first line had `si=0`, `so=0` | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for bounded in-process stream-only result; not qualification | [R10-B37 evidence](evidence/r10-b37-native-client-streaming-request-20260909.md); Provider worker/cross-process, maintained caller/no-Python and T016 remain open |
| R10-B38 | production entry/callers: `Experiments/NDNSF_DI_NativeClosure_Minindn.py` campaign owner; implementation/wire: manifest registration and owner/preflight boundary only; test/harness/oracle: fresh default preflight plus explicit `--execute-owner`; build/source closure: N/A (no product source or target changed); migration/evidence: new raw runs `.codex-tmp/spec182-t016-r6/` and `r7/`, prior R6-B4 failure retained | no product finding; current system NFD socket does not provide MiniNDN node/netns identity | none; no product build | default mode stopped before execution at `MININDN_NODE_CONTEXT_NOT_PROVIDED`; owner mode stopped before topology at `MININDN_REQUIRES_ROOT`; no protocol result | preflight-only; no `-j` build; exit 2 boundaries recorded | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `UNQUALIFIED` preflight; `CLOSED_FOR_VALIDATION` for context recheck; not qualification | [R10-B38 evidence](evidence/r10-b38-t016-runtime-context-recheck-20260909.md); root MiniNDN owner must supply namespace/PID/socket/peer context before I01–I08/PO matrix |
| R10-B39 | production entry/callers: R4-B6 `ServiceProvider` callback invoked by `NativeInferenceClient::request`; implementation/wire: fixture-only ordering of conversation binding assertion and injected ProviderFailure; test/harness/oracle: stream-only plus conversation/replacement/unary/repository selectors; build/source closure: `integration-tests` target and existing source registration; migration/evidence: post-commit review miss recorded with `Changed gate` and new evidence | initial review miss: `failFirst` could bypass conversation binding validation; repair moves binding guard before failure injection while preserving stream-only no-binding branch | no compile/link miss; existing aggregate initialization warnings remain | stream-only, conversation, replacement, unary and repository selectors exit 0 after repair | system-first `-j4` integration rebuild, 118/118 steps, 38.832s, exit 0; `vmstat` had no `so` but nonzero initial `si`, so next native build uses `-j2` | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for fixture guard; not qualification | [R10-B39 evidence](evidence/r10-b39-r4b6-fixture-contract-guard-20260909.md); Provider worker/cross-process, maintained caller/no-Python and T016 remain open |
| R10-B40 | production entry/callers: `Experiments/NDNSF_DI_NativeClosure_Minindn.py` owner → canonical `tests/standalone/run-spec182-native-closure.py` → PO-001 native integration selector; implementation/wire: root MiniNDN requester/provider namespaces, held namespace context, bubblewrap/strace and declared ELF closure; test/harness/oracle: PO-001 `businessOracle.stdoutMarker` plus process/namespace/cleanup evidence; build/source closure: existing current `integration-tests` binary and corrected fresh artifact digests; migration/evidence: one isolated native-process owner/runner acceptance case, with Provider callback still in-process fixture behavior; independent requester/Provider transport, all remaining cases and maintained caller/no-Python remain open | no product finding; r10/r11 exposed stale raw manifest role and digest preflight boundaries, corrected in fresh r12 manifest | no source changed and no rebuild required | PO-001 runner evaluation PASS, native process rc 0, marker present, no timeout; I01-I08 and PO-002-PO-014 remain unrun | owner campaign exit 0; fresh raw r12 records namespace/PID/NFD/peer/process/trace/cleanup evidence | `STATIC_PASS`; `FOCUSED_QUALIFICATION_PASS` for PO-001 only; `OPEN_FOR_NEXT_BATCH`; not T016 qualification | [R10-B40 evidence](evidence/r10-b40-t016-po001-native-owner-pass-20260909.md); next case must use current build identity and the same owner/evidence contract |
| R10-B41 | production entry/callers: `NativeInferenceClient::request` → real R4-B6 `ServiceProvider` callback → canonical MiniNDN owner/runner; implementation/wire: unary and stream-only branches emit `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` only after native result assertions; test/harness/oracle: named stream/unary/repository/conversation selectors plus PO-001 `businessOracle.stdoutMarker`; build/source closure: system-first integration target rebuilt with `-j2`, runner bound to the actual fresh `.codex-tmp/spec182-r4-b2/build/integration-tests` output and recomputed digest; migration/evidence: one isolated native-process PO-001 pass, Provider callback remains an in-process fixture and independent requester/Provider transport is open | no actionable finding after read-only review; r15 exposed the stale `build-nac182` artifact-source identity miss and remains unqualified | no compile/link miss; 118/118 build steps passed | direct stream/unary/conversation/repository/replacement selectors exit 0; r16 PO-001 owner/runner evaluation PASS with marker and complete structural evidence | system-first `-j2` integration rebuild, about 40.562s, exit 0; `vmstat` final samples had no sustained `si/so` | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `FOCUSED_QUALIFICATION_PASS` for PO-001 only; `CLOSED_FOR_VALIDATION` for this boundary; not T016 qualification | [R10-B41 evidence](evidence/r10-b41-stream-business-oracle-20260909.md); I01-I08, PO-002-PO-014, independent Provider transport, maintained caller/no-Python and full T016 remain open |
| R10-B42 | production entry/callers: N/A (shared Spec Kit workflow); implementation/wire: `batch-quality-gates.md`, `speckit-code-design/SKILL.md`, `skills/README.md` and personal installed copies; test/harness/oracle: reference/entry-copy checks only; build/source closure: N/A (documentation-only); migration/evidence: artifact source/digest mismatch from R10-B41 r15 is now a required `Changed gate` and build-lane field | no actionable finding after read-only review; the stale artifact path was converted into a reusable manifest-identity gate | no product build | no product runtime; reference SHA, targeted `rg`, `git diff --check` and validator pass | documentation-only, exit 0 | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for workflow rule; not product qualification | [R10-B42 evidence](evidence/r10-b42-artifact-identity-skill-20260909.md); future batches must record actual output path/digest and regenerate runner manifests |
| R10-B43 | production entry/callers: N/A (shared Spec Kit workflow); implementation/wire: `skills/speckit-code-design/scripts/verify-spec-kit-sync.py`, `batch-quality-gates.md`, `skills/README.md`; test/harness/oracle: normal, stale-copy and absent-install checker probes; build/source closure: N/A (documentation/checker-only); migration/evidence: 11 local entrypoints and all personal shared-skill files compared by SHA-256 | first checker review found source-file absence could raise an uncaught exception; explicit `versioned shared file missing` result added and re-reviewed with no actionable finding | no product compile/link miss; `py_compile` only | normal forced run passes 11/11 entrypoints and personal copy; stale personal copy returns exit 1; optional absent installation returns exit 0 with warnings; native product behavior remains unobserved | documentation/checker-only; no `-j` build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for synchronization only; not product qualification | [R10-B43 evidence](evidence/r10-b43-speckit-sync-check-20260909.md); product parents and T016 remain PARTIAL/UNQUALIFIED |
| R10-B44 | production entry/callers: Spec Kit feature create/update entry in `skills/speckit-code-design/SKILL.md`; implementation/wire: mandatory `verify-spec-kit-sync.py` preflight and synchronized personal copy; test/harness/oracle: checker normal/stale/absent probes from R10-B43; build/source closure: N/A (workflow-only); migration/evidence: this record, tasks/plan and personal install | no actionable static finding after official review; the rule explicitly preserves workflow `gap` when synchronization fails | no product compile/link miss; checker `py_compile` only | normal forced check passes 11/11 entrypoints and personal copy; native request, caller migration and qualification remain unobserved | documentation/checker-only; no `-j` build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for preflight only; not product qualification | [R10-B44 evidence](evidence/r10-b44-speckit-entrypoint-preflight-20260909.md); product parents and T016 remain PARTIAL/UNQUALIFIED |
| R5-B1 | production entry/callers: covered (`pythonWrapper/setup.py`, `di_bindings.cpp`); implementation/wire: covered (pybind11 exports and candidate native libraries); test/harness/oracle: covered for binding closure, gap for full requester parity; build/source closure: covered for explicit candidate Core/DI + NAC-ABE/SVS paths and RPATH; migration/evidence: gap (T012-B/T013/T016) | first extension import selected stale `/usr/local/lib/libnac-abe.so`; candidate DI shared library was stale relative to `descriptorDigest()`; repaired by explicit dependency prefix and relink | static gate did not predict same-SONAME ABI selection or stale shared-library export; recorded in [T012-A ABI evidence](evidence/t012-a-binding-abi-20260908.md) | after repair, import PASS and Python focused suites 21/21; default requester remains `NATIVE_REQUEST_PIPELINE_NOT_READY` without full native preparation/admission configuration | candidate DI relink in configured Waf tree; `python3 setup.py build_ext --inplace --force` with system-first `-j`-independent extension build; extension closure/import and 21-case pytest commands recorded in evidence; all exits 0 | BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL; not QUALIFICATION_PASS | [T012-A ABI evidence](evidence/t012-a-binding-abi-20260908.md); remaining: T012-B/T013/T016 |
| R4-B5 | production entry/callers: covered (`NativeInferenceClient::request`, `NativeConversationCoordinator`, `tests/unit-tests/di-native-v3-placement.t.cpp`); implementation/wire: covered (`NativeInferenceClient.cpp`, conversation headers, Core commit path); test/harness/oracle: covered (placement/conversation selectors and seeded receipt/ACK fixture); build/source closure: covered (unit-tests target and `case-manifest.json`, system-first `-j4`); migration/evidence: gap (no Provider cross-process or caller migration) | native owner fills dynamic envelope digest; no remaining static finding | none | seeded receipt/ACK means real Provider receipt/control and second-turn runtime remain unobserved; first test invocation had command-boundary exit 127 and was corrected | `PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ./waf -o build-nac182 build --targets=unit-tests -j4 -v`; 188 tasks, 30.69s, exit 0; selectors 1/9/5 cases PASS | STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; not QUALIFICATION_PASS | [R4-B5 evidence](evidence/r4-b5-public-conversation-20260908.md); remaining: real Provider receipt/control, second `APPEND_DELTA`, recovery/replacement and T016 |
| R4-B6 | production entry/callers: covered (`NativeInferenceClient::request`, `NativeConversationCoordinator`, `NdnsfIntegrationEnvironment`, real `ServiceProvider` handler); implementation/wire: covered (V2 structured names, SVS session/seq freshness, receipt/control/commit ACK, terminal stream); test/harness/oracle: covered for positive two-turn path, bounded single-provider CC-4c negative and local alternate-provider recovery; build/source closure: covered (`integration-tests` and unit target, system-first `-j4`); migration/evidence: gap (T012--T016) | static review completed; findings were structured request identity loss, freshness replay, post-End retry, one-worker control-plane waits and replacement map/contract binding, all repaired in the recorded batches; legacy event-wire compatibility was also repaired before final gates | R7-B1 initial success expectation exposed the recovery ACK/no-admitted-provider boundary; R7-B2 first build exposed a test-helper syntax boundary and first alternate run exposed recovery request-name truncation; both are retained and repaired | final R4-B6 selector passed two real Provider turns; R7-B1 single-provider negative and R7-B2 alternate-provider replacement passed with no stale checkpoint publication | R7-B2 `PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf ./waf -o build-nac182 build --targets=unit-tests,integration-tests -j4 -v`, 32.93s, 306/306, exit 0; coordinator/parser unit selectors and R4-B6 positive/negative/alternate selectors exit 0; vmstat showed no sustained swap; detailed timing and raw logs in [R7-B2 evidence](evidence/r7-b2-alternate-provider-replacement-20260909.md) | FOCUSED_BEHAVIOR_PASS; PARTIAL; not QUALIFICATION_PASS | [R4-B6 evidence](evidence/r4-b6-provider-conversation-20260908.md), [R7-B1 evidence](evidence/r7-b1-r4b6-replacement-20260908.md), [R7-B2 evidence](evidence/r7-b2-alternate-provider-replacement-20260909.md); remaining: Python caller migration, cross-process conversation, T012/T013, T014/T015/T016 |
| R7-B1 | production entry/callers: covered (`NativeInferenceClient::request`, real `ServiceProvider` handler and `NativeConversationCoordinator` lookup); implementation/wire: covered (ProviderFailure stream injection, replacement ACK planning, failed-provider exclusion, no-checkpoint fence); test/harness/oracle: bounded single-provider negative covered, alternate-provider recovery moved to R7-B2; build/source closure: changed integration source registered by existing Waf target; migration/evidence: T011-C cross-process and T016 remain open | static review checked failure injection, status/result ordering, no record lookup before failed status, test registration and positive helper preservation; no actionable finding | first run expected success and exposed the real `ACK_CLOSED`/`DI_NATIVE_NO_ADMITTED_PROVIDER` boundary; raw exit201 retained and test semantics corrected | final negative selector passed 1 case with `NATIVE_REQUEST_STAGE_FAILED`, `ACK_CLOSED`, one collaboration call and no checkpoint; positive real two-turn selector passed 1 case | system-first `PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf ./waf -o build-nac182 build --targets=integration-tests -j2`, Waf 36.791s, 118/118, exit 0; final negative/positive selectors exit 0; `vmstat` recorded pre-existing swap and no sustained `si/so` | STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL; not QUALIFICATION_PASS | [R7-B1 evidence](evidence/r7-b1-r4b6-replacement-20260908.md); single-provider negative closed; R7-B2 alternate-provider recovery closed locally; T011-C/T016 remain |
| R7-B2 | production entry/callers: covered (`NativeInferenceClient::beginReplacement`/`beginCoreRequest`, `NativeConversationCoordinator`, real two-provider `ServiceProvider` handlers); implementation/wire: covered (new execution/contract digest, replacement role-map binding, parent CAS preservation, structured recovery name parsing and receipt/control ACK identity); test/harness/oracle: covered (`Spec182Conversation/ReplacementFencesOldAttemptAndPreservesAcceptedPrefix`, `GenericDynamicApi/PreparedAndMessages/V2RequestAndResponseNames`, `Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversationReplacement`, `...AlternateReplacement`, and positive two-turn selector); build/source closure: covered (coordinator/client/planner/parser and both test targets registered by existing Waf source closure); migration/evidence: gap (cross-process, Python caller migration and T016) | static review initially found missing coordinator role-set validation; fixed and re-reviewed with no remaining actionable finding. Coverage included production callers/default wiring, wire/parser, test registration/oracle and build closure; review path/SHA and full diff are in the evidence record | first build failed at the integration helper syntax boundary (r1); first alternate runtime failed at recovery Selection request-id truncation before provider1 callback (r4); both raw logs are preserved and repaired before final gates | focused coordinator/parser/unit and single-provider negative, alternate-provider positive, and original positive real-provider selectors all passed; no stale/duplicate checkpoint or old-map mutation observed | system-first `PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf ./waf -o build-nac182 build --targets=unit-tests,integration-tests -j4 -v` -> 306/306, 32.93s, exit 0; vmstat post-first-line samples had no sustained `si/so`; selector timings/rc in [R7-B2 evidence](evidence/r7-b2-alternate-provider-replacement-20260909.md) | STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; DONE for this local batch; not QUALIFICATION_PASS | [R7-B2 evidence](evidence/r7-b2-alternate-provider-replacement-20260909.md); `CLOSED_FOR_VALIDATION` for local alternate replacement; next: cross-process conversation and maintained caller migration (T013/T016) |
| R5-B2 | production entry/callers: covered (`NativeServiceUser` construction methods, canonical `APPClient` and public `InferenceClient` explicit native route); implementation/wire: covered (single pybind entry, catalog source validation, Core preparation, Ed25519 grant owner and runtime constructor); test/harness/oracle: covered for binding/facade closure, gap for real request parity and Provider failure; build/source closure: covered (candidate Core/DI + NAC-ABE/SVS extension target and RPATH, system-first toolchain); migration/evidence: gap (T013/T014/T016) | requester identity mismatch in grant config was found by static review and rejected before key use; no remaining control finding after re-review | first compile exposed invalid const holders and incomplete native headers; first Python catalog/facade checks exposed bytes conversion and wrong test class; all repaired before final checks | 24 binding cases, 31 compatibility-selection cases and invalid-catalog negative passed; native request against a real Provider and legacy retirement remain unobserved | `python3 setup.py build_ext --inplace --force`, one extension target, system-first toolchain, observed ~197s wall, exit 0; focused selectors and import output in evidence/run directory | STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL; not QUALIFICATION_PASS | [R5-B2 evidence](evidence/r5-b2-native-runtime-binding-20260908.md); remaining: real request parity, T013/T014/T015/T016 |
| R5-B3 | production entry/callers: covered (seven T013-A paths in `r5-b3` matrix); implementation/wire: gap for caller composition (`request_native` requires runtime/catalog/preparation/admission); test/harness/oracle: gap for maintained-caller native behavior (no C++ caller selector yet); build/source closure: covered for R5-B2 extension, gap for caller/provider native executable closure; migration/evidence: gap (manifest sourceCommit stale; no zero-caller/rollback evidence) | static review found no new product defect; the audit exposed that requester and Provider migrations are distinct exits and that current callers still use Python routes | none (audit only) | current callers remain on `request_task`/`request_streaming`/`request`/`distributed_inference` or `APPProvider`; no native caller behavior was run | read-only CodeGraph/rg inventory, manifest identity check, `validate_design.py`, and `git diff --check`; no product build or runtime | STATIC_PASS for inventory only; PARTIAL; not QUALIFICATION_PASS | [R5-B3 caller audit](evidence/r5-b3-maintained-caller-audit-20260908.md); next R5-B4 native config fixture, R5-B5 YOLO requester, R5-B6 Qwen/streaming requester, then R6-B1 Provider retirement |
| R5-B4 | production entry/callers: native runtime config and C++ requester selector; implementation/wire: native schema parser, grant identity/epoch accessors and single pybind composition entry; test/harness/oracle: deterministic YOLO catalog/grant fixture plus C++/binding checks; build/source closure: shared native target and extension rebuilt with system-first `-j4`; migration/evidence: maintained caller migration and T016 remain open | `FOCUSED_BEHAVIOR_PASS`; parser rejects unknown/invalid identity, protection, budget, type and state mapping; configured client rechecks grant requester/epoch binding; no maintained caller changed in this batch | first extension import exposed stale shared library because only `unit-tests` was rebuilt; shared target was rebuilt before corrected extension build | C++ 30-case `Spec182NativePlanning` and dedicated runtime-config selector passed; Python compatibility/binding selection 39 passed; no real Provider request | `unit-tests` final source/fixture rebuild 27.990s (initial full rebuild 2m54.491s); shared target 27.990s; extension corrected build ~206s, final source-closure relink ~160s; system-first `/usr/bin/g++` 9.4.0/ld 2.34 | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not QUALIFICATION_PASS | [R5-B4 evidence](evidence/r5-b4-native-runtime-config-20260908.md); next R5-B5 maintained YOLO requester, then R5-B6 Qwen/streaming and R6-B1 Provider retirement |
| R5-B5 | production entry/callers: `examples/DI_NativeRequester.cpp` native CLI; implementation/wire: CLI composition delegates runtime policy to `nativeRequestRuntimeFromJson` and retains native catalog/grant/preparation/admission ownership; test/harness/oracle: C++ runtime parser selector plus CLI help/usage/error selectors and source/binding checks; build/source closure: `DI_NativeRequester` and unit target rebuilt with system-first `-j4`; migration/evidence: maintained Python YOLO caller, real Core/Provider request, T013-B and T016 remain open | static review checked parser is the only runtime construction boundary and no Python fallback is introduced; no actionable finding | none in final batch; the CLI probes are local entry/configuration checks only | C++ `Spec182NativePlanning/NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift`, `DI_NativeRequester --help`, usage and invalid-schema selectors; Python source/binding checks | `PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc ./waf build --out=build-nac182 --targets=DI_NativeRequester,unit-tests -j4` -> exit 0, Waf 14.173s; C++ selector -> exit 0; Python -> 25 passed | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not QUALIFICATION_PASS | [R5-B5 evidence](evidence/r5-b5-yolo-native-requester-20260908.md); next R5-B6A option binding, then R5-B6 Qwen/streaming requester |
| R5-B6A | production entry/callers: `bindDistributedInference` and `NativeRequestOptions`; implementation/wire: native generation, Core stream/controller version, conversation continuation DTOs plus wire-byte grant property; test/harness/oracle: Python DTO construction/nesting and Core stream validate/wire round-trip; build/source closure: DI shared library and `_ndnsf` extension rebuilt from explicit candidate closure with system-first `-j4`; migration/evidence: caller migration, callback lifetime, real request parity and T016 remain open | static review checked single pybind entry, native callback non-export, optional Block conversion and no planner duplication; no actionable finding | first import after extension build failed because the candidate DI shared library lacked `NativeAdapterDescriptor::descriptorDigest()`; an initial Waf invocation used a stale `.codex-tmp` lock/output; both boundaries were corrected before final run | 10 Python focused cases pass, including nested options, stream validation/wire round-trip, and key-grant bytes/None boundary; no real requester or Provider execution | candidate DI shared library `WAFLOCK=.lock-waf ./waf build --targets=ndnsf-distributed-inference -j4` -> exit 0, 5m38.247s; extension `python3 setup.py build_ext --inplace --force --parallel 4` -> exit 0; `ldd`/`nm`/SHA-256 and logs retained under R5-B6A evidence directory | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `DONE` for this bounded binding exit; T012-A/T013-A/T016 remain open | [R5-B6A evidence](evidence/r5-b6a-native-option-bindings-20260908.md); next R5-B6 caller migration after native generation/stream/conversation acceptance |
| R5-B6 | production entry/callers: covered for `APPClient.configure_native_requester_from_config`, `request_native_payload`, maintained Qwen user and harness config forwarding; implementation/wire: covered for native config composition, generation contract, stream options and fail-closed conversation/tiny-runtime boundaries; test/harness/oracle: covered for source route, option DTO construction and Python compatibility/binding suites, gap for real native streaming/Core/Provider and callback delivery; build/source closure: covered for shared `ndnsf-distributed-inference` target and existing extension, system-first `-j4`; migration/evidence: PARTIAL because native streaming execution, YOLO user, Provider retirement, native continuation owner and T016 remain open | static review fixed absolute operator-path handling and preserved native semantic `ValueError` instead of swallowing it; parser rejects native config for non-Qwen runtimes; no remaining actionable finding | initial import probe lacked an optional example-only module path and the option probe revealed pybind byte fields are represented as strings/lists; both were corrected or bounded before final checks | 28 Python focused cases pass; option construction/import pass; no network request, callback lifetime, native streaming execution, or conversation continuation execution | `PATH=/usr/bin:/bin:/usr/sbin:/sbin WAFLOCK=.lock-waf ./waf build --targets=ndnsf-distributed-inference -j4` -> exit 0, 5.419s incremental; `/usr/bin/python3 -m py_compile` for changed Python files and `git diff --check` -> exit 0; extension source-closure rebuild and `ldd`/`nm` checks from R5-B6A remain valid | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not QUALIFICATION_PASS | [R5-B6 evidence](evidence/r5-b6-native-caller-migration-20260908.md); next: native streaming owner and/or conversation owner, then remaining caller migration and T016 |
| R5-B7 | production entry/callers: covered (`NativeInferenceHandle::observe`, `APPClient.request_native_payload(on_event=...)`); implementation/wire: covered (pybind observer translates request id/bytes payload/terminal, C++ queue retains ordering/isolation and publishes accepted stream token events); test/harness/oracle: covered for C++ slow-observer/cancel/replay/deadline and public conversation token-order selectors plus Python facade/source gates; gap for real Provider callback delivery and cross-process stream; build/source closure: covered (shared DI target and forced `_ndnsf` extension source closure); migration/evidence: PARTIAL (T013-A/T016 remain open) | static review caught the const handle binding mismatch and corrected it before final build; test gate updated to permit only observer callback while retaining no-planner assertion | first build found `observe` called through a const handle; the first full placement selector also hit a transient test-process SIGSEGV at `di-native-v3-placement.t.cpp:1026` (raw r0 record); isolated and later full-suite reruns passed, so no product cause is assigned; real Provider remains unobserved | C++ observer selectors and `PublicClientConversationCommitsSeededReceiptAndCheckpoint` passed (two token events + terminal); 28 Python focused cases and exported-method smoke check passed; no real Provider request | shared DI target `./waf build --targets=unit-tests -j4` exit 0, elapsed=56.03s; timed forced extension `python3 setup.py build_ext --inplace --force --parallel 4` exit 0, elapsed=337.88s (single large translation unit; intermittent swap observed); system-first `/usr/bin/g++` 9.4.0, linked `build-nac182` and NAC-ABE prefix | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not QUALIFICATION_PASS | [R5-B7 evidence](evidence/r5-b7-native-observer-20260908.md); next: native streaming execution and conversation owner, then YOLO/Provider migration and T016 |
| R5-B8 | production entry/callers: covered (`llm_pipeline/user.py` `_native_qwen_request` and `full_generation_call`); implementation/wire: covered (optional observer forwarding and `GenerationTokenEventV1` caller-edge validation); test/harness/oracle: covered for source route and native observer/token-order selectors, gap for real Provider/cross-process delivery; build/source closure: N/A (Python-only caller change; native extension/source closure unchanged from R5-B7); migration/evidence: PARTIAL (YOLO, Provider retirement, conversation owner and T016 remain open) | static review found no actionable issue after checking callback lifetime, terminal wait, route ordering and no planner fallback | none | 28 Python focused cases, py_compile and `git diff --check` passed; C++ observer/deadline/public-conversation selectors passed; no real Provider request | Python-only source change; native build N/A; unchanged extension/shared-target identity referenced from R5-B7 | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not QUALIFICATION_PASS | [R5-B8 evidence](evidence/r5-b8-native-qwen-stream-callback-20260908.md); next: native continuation owner or another caller-shaped migration batch |
| R5-B9 | production entry/callers: covered (APPClient config, ServiceUser native conversation factory, NativeServiceUser); implementation/wire: covered for C++ owner parser, journal/key ownership and NativeInferenceClient injection; test/harness/oracle: covered for C++ owner-config selector and Python binding/source checks, gap for real Provider/cross-process conversation; build/source closure: covered by shared DI and explicit extension rebuild; migration/evidence: gap for qualification and T016 | static review found and fixed relative-path escape; final review found no actionable finding; key/path permissions, identity/digest binding, opaque lifetime and constructor overload covered | first extension link used default build path and missed libndnsf-distributed-inference; corrected with explicit candidate closure; one final vmstat sample showed swap-out during the 5.4 GB translation unit | C++ owner selector and conversation suite passed; 15 Python binding/legacy cases passed; direct NativeServiceUser probe stopped before parser because /run/nfd/nfd.sock was unavailable; no network request | shared DI target -j4 1m38.315s; unit-tests -j4 46.323s; extension explicit closure -j4 9m34.555s; ldd/RUNPATH/nm passed | STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; PARTIAL; not QUALIFICATION_PASS | Review trace: /home/tianxing/.codex/skills/review-agent/SKILL.md SHA-256 07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228; baseline 172e1d1b; Closure decision: OPEN_FOR_NEXT_BATCH because Provider/cross-process two-turn and T016 remain; [R5-B9 evidence](evidence/r5-b9-native-conversation-owner-20260908.md) |
| R5-B10 | production entry/callers: `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` explicit `--native-requester-config` branch; implementation/wire: `APPClient.configure_native_requester_from_config` + `request_native_payload` with inline native tensor bytes and native task/options; test/harness/oracle: source route checks, no planner fallback in the branch, payload/result boundary and shutdown ownership; build/source closure: Python-only `py_compile` (native closure unchanged from R5-B9); migration/evidence: real Core/Provider request, numeric parity, default route retirement and T016 remain open | static review completed; branch is ordered before ACK-driven Python planner and fails closed on configuration/constructor errors | no network harness in this batch; all real requester/provider and numeric evidence stays deferred | route/source checks and `py_compile` after implementation; no C++ rebuild because native headers were unchanged | Python-only batch; system-first `-j4` not applicable; native extension identity inherited from R5-B9 | `PARTIAL`; not QUALIFICATION_PASS until real request and caller migration are observed | [R5-B10 evidence](evidence/r5-b10-yolo-native-payload-route-20260908.md); next T013-B legacy retirement review and T014 collector/harness closure |
| R5-B11 | production entry/callers: compatibility manifest plus maintained caller inventory; implementation/wire: no deletion, only current sourceCommit/regenerated caller references; test/harness/oracle: legacy removal eligibility and consumer reachability checks; build/source closure: Python-only manifest generation and focused tests; migration/evidence: zero-consumer proof, default import graph, T013-B/T014/T016 remain open | static review confirms old owners are still reachable or externally unresolved; removal remains forbidden | no product compile; manifest generation is an inventory artifact and cannot prove runtime retirement | regenerated `compatibility-manifest.json` (344 entries) and focused legacy/source checks; no network run | no native build; system-first `-j4` not applicable | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not QUALIFICATION_PASS | [R5-B11 evidence](evidence/r5-b11-legacy-reachability-audit-20260908.md); next: T013-B migration closure after all maintained callers have native route, then T014 |
| R6-B1 | production entry/callers: `tests/standalone/run-spec182-native-closure.py` `collect_trace`/`evaluate_case`; implementation/wire: trace-integrity and policy-violation verdict boundary; test/harness/oracle: `tests/python/test_spec182_native_closure.py` independent status/trace fixtures; build/source closure: N/A (Python harness-only); migration/evidence: real namespace, descendant and endpoint observation remains T016 | static review found the prior conflation of trace completeness and policy violations; fixed by separating integrity/policy lists and preserving exit-1 vs exit-2 boundary; re-review found no actionable issue | none | 12 focused Python cases, `py_compile`, `git diff --check`; no native build or network | Python-only R6-B1; `-j4` not applicable; all validation exits 0 | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `52123b1c`, full diff limited to collector/tests/tasks/evidence; Closure decision: `CLOSED_FOR_VALIDATION` for local verdict semantics; real namespace/child/endpoint observation remains T016; [R6-B1 evidence](evidence/r6-b1-collector-verdict-20260908.md) |
| R6-B2 | production entry/callers: `Experiments/NDNSF_DI_NativeClosure_Minindn.py` campaign owner and `tests/standalone/run-spec182-native-closure.py` canonical runner; implementation/wire: manifest registration schema, case owner and bounded deadline/cleanup contract; test/harness/oracle: `test_minindn_registration_covers_counterexamples_and_proof_cases` and fresh-run record test; build/source closure: N/A (Python harness/manifest only); migration/evidence: frozen manifest now names I01--I08 and PO-001--PO-014, while real MiniNDN node context and qualification remain T016 | static review first caught existing-output mutation on the exception path; fixed with pre-existing output refusal and regression test; re-review found no additional issue | none | 21 Python cases in combined selector, JSON parse, `py_compile`, `validate_design.py`, `git diff --check`; no native build or network | Python-only R6-B2; `-j4` not applicable; all validation exits 0 | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `194155b4`, diff limited to campaign owner/manifest/tests/tasks/evidence; Closure decision: `CLOSED_FOR_VALIDATION` for registration and fresh-run refusal; real node setup, collector execution and PO outcomes remain T016; [R6-B2 evidence](evidence/r6-b2-harness-registration-20260908.md) |
| R6-B3 | production entry/callers: `NativeInferenceClient::request`, `APPClient.request_native_payload`, maintained YOLO/Qwen native branches and `ServiceProvider` registration; implementation/wire: native requester→Core/Provider transaction contracts, owner/config identity and legacy boundary; test/harness/oracle: C++ requester/conversation/provider selectors, 344-entry compatibility manifest, R6-B1/R6-B2 harness selectors; build/source closure: prior candidate native/extension builds remain valid by commit identity, no new build in this audit; migration/evidence: 22 qualification IDs registered but real two-turn, stream, legacy-zero-use and T016 evidence remain open | static cross-task review found no new control defect; known gaps are correctly assigned to T004/T008/T009/T010/T011/T013/T016 and default public Python routes remain retained until native callers are proven | no new compile miss; prior evidence includes candidate closure and focused C++ builds, while this audit is read-only | maintained caller inventory, compatibility manifest, R6-B1/R6-B2 tests and source call-site checks; no network/namespace run. A current-binary legacy D2b probe additionally failed after User published both Selection projections but before provider1 callback; first boundary and raw logs are in [R6-B7 diagnostic](evidence/r6-b7-legacy-d2b-regression-20260908.md) | read-only R6-B3 audit; `-j4` not applicable for the audit; exploratory rebuild was stopped after sustained swap-in/out | `STATIC_PASS` for inventory only; `BUILD_NOT_APPLICABLE`; `RUNTIME_BOUNDARY_FAILURE`; `PARTIAL`; not `QUALIFICATION_PASS` | Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `6171cf4d`, scope audit/traceability/tasks and current source call paths; Closure decision: `OPEN_FOR_NEXT_BATCH` because T016 real qualification, legacy D2b compatibility, and remaining native caller/provider exits are not closed; [R6-B3 evidence](evidence/r6-b3-cross-task-convergence-20260908.md) |
| R6-B4 | production entry/callers: T016-A campaign owner `Experiments/NDNSF_DI_NativeClosure_Minindn.py` and frozen case manifest; implementation/wire: preflight tool/node/socket identity boundary; test/harness/oracle: I01 fresh campaign result and prerequisite command checks; build/source closure: N/A (preflight only); migration/evidence: raw run `.codex-tmp/spec182-t016-r2/` and failure-log entry preserve first boundary, full PO matrix remains open | preflight correctly refused missing node context before business execution; no product defect inferred from environment boundary | no compile/build attempted | command exit 2 and persisted result status `UNQUALIFIED`; no namespace/network run | no native build; `-j4` not applicable | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `UNQUALIFIED` preflight; `PARTIAL`; not `QUALIFICATION_PASS` | Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `c258f93f`, scope campaign/manifest/preflight output; Closure decision: `OPEN_FOR_NEXT_BATCH` until external MiniNDN node/netns/NFD context is available; [R6-B4 evidence](evidence/r6-b4-t016-preflight-20260908.md) |
| R6-B5 | production entry/callers: T015-A/T016-A task and evidence registry; implementation/wire: audit/traceability status alignment; test/harness/oracle: `validate_design.py`, link checks and existing native-closure selector; build/source closure: N/A (documentation-only); migration/evidence: current baseline, 16 DONE/23 PARTIAL/1 NOT_STARTED counts, and T016 preflight boundary synchronized across audit/traceability/tasks | static documentation review found stale `BLOCK for implementation`, `0/17`, and `all planned` statements; corrected to current PARTIAL/preflight state; re-review found no additional issue | none | validator `ok=true`, 21 Python cases and syntax checks remain green; no product build/network | documentation-only; `-j4` not applicable | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `bcc5f798`, scope audit/traceability/tasks and current evidence links; Closure decision: `CLOSED_FOR_VALIDATION` for status-document synchronization; T004/T008/T009/T010/T011/T013/T016 production evidence remains open; [R6-B5 evidence](evidence/r6-b5-status-document-sync-20260908.md) |
| R6-B6 | production entry/callers: public `InferenceClient.configure_native_requester` and canonical `APPClient` facade; implementation/wire: preserve the two-argument native binding contract while forwarding an optional conversation owner only when supplied; test/harness/oracle: `test_public_inference_client_exposes_explicit_native_route`, `test_public_inference_client_forwards_conversation_owner` and the focused Spec182 compatibility selectors; build/source closure: Python source only, native ABI unchanged; migration/evidence: real native requester/provider parity, legacy retirement and T016 remain open | DONE for this bounded compatibility batch; the prior unwanted `None` third argument is removed while explicit conversation forwarding remains covered | no C++ compile expected; source patch is limited to optional-argument forwarding | `py_compile`, `git diff --check`, and focused Python suite: 47 passed; no network run | Python-only; `-j4` not applicable | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `DONE` for batch only; not `QUALIFICATION_PASS` | Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline current pre-batch source; `No findings`; [R6-B6 evidence](evidence/r6-b6-native-route-compat-20260908.md) |
| R6-B8 | production entry/callers: `tests/standalone/run-spec182-native-closure.py` `load_case`/`evaluate_case`; implementation/wire: declared role and cold-path evidence boundary; test/harness/oracle: 18 independent `test_spec182_native_closure.py` cases including missing, valid, duplicate and mismatch observations; build/source closure: N/A for C++, Python syntax only; migration/evidence: local evaluator semantics covered, real namespace/child/endpoint and T016 remain open | first static review found duplicate observed roles could collapse to a set and pass; duplicate observations now return `ROLE_OBSERVATION_INVALID` and `UNQUALIFIED`; no further actionable finding | no compile miss; no native source changed | 18 focused Python cases and `py_compile` pass; no MiniNDN or network run | Python-only; `-j4` not applicable | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | Review trace: `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, baseline `04f361e7`; Closure decision: `CLOSED_FOR_VALIDATION` for local role/cold evaluator; [R6-B8 evidence](evidence/r6-b8-collector-role-cold-20260909.md) |
| R6-B9 | production entry/callers: legacy Spec170 D2b `runProductionD2bDataV1Case` and User/Provider SVS ingress; implementation/wire: `ServiceProvider::isFresh` producer-session/sequence gate; test/harness/oracle: named `ProductionIngressRunsD2bSelectionIntoSvsDataV1` plus provider0/provider1 callback and DATA_V1 fetch assertions; build/source closure: `integration-tests` target and current ServiceProvider source; migration/evidence: T013-B remains gated on compatibility repair and T016 | DONE for this bounded local freshness batch; per-session/per-publication-name sequence fencing preserves old-session rejection and accepts valid out-of-order publications | `ServiceProvider.cpp/.hpp` and synchronized review references only; no User, Selection wire, decrypt, or T016 path changed | current source trace reproduced provider1 loss before repair; each of five D2b selectors passes 10/10 (50/50 total); named D2h212 is intermittent at 18/20 and remains a separate open boundary | system-first `-j4` `integration-tests`: 118/118, 1m37.712s Waf; two subsequent unfiltered suite runs exit 0; first unfiltered attempt and 2/20 D2h repeats show callback/lifetime crash | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS` for D2b; `PARTIAL`; not `QUALIFICATION_PASS` | [R6-B9 evidence](evidence/r6-b9-legacy-d2b-freshness-20260909.md); `CLOSED_FOR_VALIDATION` for local D2b freshness, `OPEN_FOR_NEXT_BATCH` for T013-B/T013-C/T016 and D2h stability |
| R9-B1 | production entry/callers: `ServiceProvider::CollaborationContext::reportOperationStatus`, `NativeProviderHandler` worker reports, `replySelectionExecutionStatus`; implementation/wire: dedicated `m_selectionExecutionStatusMutex` around status map/vector and snapshot reads; test/harness/oracle: `SelectionSnapshotConcurrentMembersRemainOwned` plus `ProductionNativeHandlersRunD2h212ToCompleteOracleResponse`; build/source closure: current `ServiceProvider.cpp/.hpp`, `unit-tests` and `integration-tests` Waf targets; migration/evidence: R6-B9 historical crash, this evidence, `tasks.md`, `docs/failure-log.md` | DONE for this bounded local selection-status ownership repair; T010/T013/T016 and cross-process qualification remain open | no status wire or state-transition contract changed; only concurrent ownership protection and regression test added | ASAN first-boundary trace identified heap-use-after-free during concurrent `memberStatuses` growth; unit selector passes; D2h212 passes 50/50 fresh processes after fix; ASAN-preload follow-up passes 20/20 with type-size mismatch diagnostics disabled | system-first `-j4` unit-tests 188/188 (3m3.636s) and integration-tests 118/118 (1m44.786s); vmstat no sustained swap-out | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for local status ownership; not `QUALIFICATION_PASS` | [R9-B1 evidence](evidence/r9-b1-selection-status-concurrency-20260909.md); historical R6-B9 raw failures preserved; T013/T016 remain open |
| R10-B1 | production entry/callers: `NativeInferenceClient::request` → `dispatchOperation` → `NativeRequestPreparation::prepareInput`; implementation/wire: `NativePreparedInput` verifies encrypted REPO_REF identity while preserving the reference, inline input keeps adapter encoding, and v2 envelope remains unchanged; test/harness/oracle: `Spec182NativeInferenceClient`, `Spec182Preparation`, and REPO_REF envelope selectors; build/source closure: `NativeRequestPreparation.cpp/.hpp`, `NativeInferenceClient.cpp`, `unit-tests` Waf target; migration/evidence: native Qwen/YOLO inline facades remain unchanged, repository callers gain a C++ preparation route, real encrypted fetch/Provider two-turn/T016 remain open | DONE for this bounded local preparation boundary; parent T004/T010/T016 remain open | no requester-side decryption or Python planner fallback; Provider fetch/decrypt boundary remains authoritative | root-cwd focused selectors: Preparation 18, ClientState 17, PlanSealer 12, V3Placement 9; full unit target 1015 cases; existing R4-B6 real-Provider selector 3 cases; all exit 0 | system-first `-j4` incremental unit build exit 0 in 20.37s, max RSS 1,121,768 KB; `vmstat 1` showed no sustained swap-out | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B1 evidence](evidence/r10-b1-repo-ref-preparation-20260909.md); raw retry logs `.codex-tmp/spec182-r10-b1/`; real encrypted fetch/Provider two-turn/T016 remain open |
| R10-B2 | production entry/callers: `APPClient.request_native_reference` → `APPClient.request_native` → `NativeInferenceClient::request`; implementation/wire: journal-bound `LargeDataReference` canonical JSON populates `NativeApplicationInput::REPOSITORY_REFERENCE` while preserving no-decrypt requester ownership; test/harness/oracle: `test_core_native_reference_route_preserves_identity_and_avoids_planner` plus public `InferenceClient` forwarding selector; build/source closure: `app_sdk/client.py` and Python compatibility target, native ABI reused from R10-B1; migration/evidence: published-reference facade is available to maintained callers, real encrypted fetch/Provider execution, caller migration and T016 remain open | DONE for this bounded facade boundary; parent T004/T010/T013/T016 remain open | R10-B1; no Python planner fallback, requester-side decryption, or Provider contract changes | static review no findings; py_compile, scoped diff check, validate_design, and Python selectors pass | Python-only; no native rebuild needed; R10-B1 ABI reused | 17 compatibility cases + 35 related Spec180/Spec182 cases, all exit 0; first harness-default miss is preserved in failure-log/evidence | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B2 evidence](evidence/r10-b2-native-reference-facade-20260909.md); raw retry `.codex-tmp/spec182-r10-b2/`; real encrypted fetch/Provider two-turn/caller migration/T016 remain open |
| R10-B3 | production entry/callers: `_load_yolo_native_payload` in `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`; implementation/wire: native tensor bundle is published once and the returned journal-bound reference is sent via `APPClient.request_native_reference`; test/harness/oracle: `test_maintained_yolo_native_route_uses_repository_reference` source selector plus R10-B2 facade selectors; build/source closure: YOLO user module and existing Python compatibility target, native ABI reused; migration/evidence: native YOLO no longer uses inline native payload, ACK-driven planner/lifecycle paths remain separate, real encrypted fetch/Provider execution and T016 remain open | DONE for this bounded caller migration; parent T004/T010/T013/T016 remain open | R10-B2; no changes to ACK-driven branch or Python planner fallback | static review no findings; py_compile, source selector, scoped diff check and validate_design pass | Python-only; no native rebuild needed; R10-B2/R10-B1 ABI reused | 24 compatibility/legacy cases and source branch oracle pass; no network run | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B3 evidence](evidence/r10-b3-yolo-native-reference-caller-20260909.md); raw logs `.codex-tmp/spec182-r10-b3/`; real Provider/two-process/T016 remain open |
| R10-B4 | production entry/callers: `_native_qwen_request` in `examples/python/NDNSF-DistributedInference/llm_pipeline/user.py`; implementation/wire: each typed Qwen context bundle is published once and sent via `APPClient.request_native_reference`, with existing generation options/observer preserved; test/harness/oracle: `test_maintained_qwen_native_route_uses_repository_reference` source selector plus R10-B2 facade selectors; build/source closure: Qwen user module and Python compatibility target, native ABI reused; migration/evidence: native Qwen generation/stream helper reaches REPO_REF, automatic-planning and legacy paths remain separate, real Provider fetch/conversation/cross-process and T016 remain open | DONE for this bounded caller migration; parent T004/T010/T013/T016 remain open | R10-B3/R10-B2; conversation still fails closed without native owner | static review no findings; py_compile, source selector, scoped diff check and validate_design pass | Python-only; no native rebuild needed; R10-B2/R10-B1 ABI reused | 25 compatibility/legacy cases and source helper oracle pass; no network run | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B4 evidence](evidence/r10-b4-qwen-native-reference-caller-20260909.md); raw logs `.codex-tmp/spec182-r10-b4/`; real Provider/stream/conversation/T016 remain open |
| R10-B5 | production entry/callers: `runNativeIngressCase` → real `ServiceProvider`/`NativeProviderHandler`; implementation/wire: v2 `ndnsf-di-request-envelope-v2` `REPO_REF` parsing, `CollaborationContext::fetchEncryptedLargeData`, plaintext-size binding and native runner input; test/harness/oracle: `ProductionIngressRunsNativeRepositoryReferenceIntoProvider` observes the recovered `request-input` scope; build/source closure: existing `integration-tests` target and `NativeProviderHandler.cpp` source closure; migration/evidence: requester/facade/caller migration remains local-only and cross-process/T016 qualification remains open | DONE for this bounded local Provider consumption boundary | R10-B1--R10-B4; real Provider fixture | provider-side encrypted fetch/decrypt is exercised without changing requester ownership; malformed/missing reference negatives and T016 remain separate | static review no findings; comma-separated Boost.Test filter setup miss retained and corrected with suite selector | system-first `-j4` integration-tests build 118/118, 1m50.876s; `Spec170NativePostSelection` 5/5 cases exit 0; vmstat no sustained swap | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B5 evidence](evidence/r10-b5-provider-repo-ref-execution-20260909.md); raw `.codex-tmp/spec182-r10-b5/`; malformed/missing negatives, cross-process, caller retirement and T016 remain open |
| R10-B6 | production entry/callers: `runNativeIngressCase` → real `ServiceProvider`/`NativeProviderHandler`; implementation/wire: v2 REPO_REF parser/fetch failure boundaries and `ctx.fail` status propagation; test/harness/oracle: missing-object, plaintext-size mismatch and malformed-envelope cases in `Spec170NativePostSelection`; build/source closure: same `integration-tests` target and native handler source closure; migration/evidence: negative local provider behavior only, cross-process/T016 remain open | DONE for this bounded local negative boundary | R10-B5 positive Provider boundary | fail-closed reasons are observed before any runner input or response; missing-object oracle scopes a 1 s test fetch budget with RAII; no requester or Core contract change | static review covered production handler, v2 fields, runner observer, test registration and Waf source closure; first fixed-pump miss is linked in failure-log and evidence | system-first `-j4` integration-tests rebuild exit 0, 118/118, 34.427s; full `Spec170NativePostSelection` 8/8 exit 0 in 8.98s; vmstat showed no sustained swap | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B6 evidence](evidence/r10-b6-provider-repo-ref-fail-closed-20260909.md); raw `.codex-tmp/spec182-r10-b6/`; cross-process/T016 remain open |
| R10-B7 | production entry/callers: maintained Qwen configuration output and T013-D/T013-F route references; implementation/wire: `request_native_reference` and encrypted repository publication naming; test/harness/oracle: source route checks plus `py_compile`; build/source closure: Python caller/contract documents only, native ABI unchanged; migration/evidence: R10-B3/R10-B4 evidence links and legacy route remains explicitly retained | DONE for this bounded caller-contract synchronization | R10-B3; R10-B4 | current route documentation and emitted configuration marker must identify `REPO_REF`; no native requester or Provider contract change | static review covers caller output, execution-unit Outcome/Read/Steps, task registry link and historical-vs-current distinction | Python-only `py_compile`, source assertions, `git diff --check`, and `validate_design.py` | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B7 evidence](evidence/r10-b7-caller-route-contract-sync-20260909.md); real caller execution and T016 remain open |
| R10-B8 | production entry/callers: current source-alignment audit and R10-B1--R10-B7 maintained caller/Provider exits; implementation/wire: current `request_native_reference` route and explicit `NATIVE_REQUEST_PIPELINE_NOT_READY` fail-closed boundary; test/harness/oracle: audit/task/evidence link consistency and design validator; build/source closure: documentation-only, native ABI unchanged; migration/evidence: T004/T008/T010/T011/T013/T016 remaining owners and T016 preflight | DONE for this bounded status-audit synchronization | R10-B7; current tasks/evidence registry | audit must identify the current checkpoint and distinguish local closed boundaries from qualification gaps; no product or task status promotion | static review covers current audit summary, route naming, remaining-owner matrix and historical section dating | documentation-only: `git diff --check`; `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B8 evidence](evidence/r10-b8-cross-task-audit-refresh-20260909.md); product execution and T016 remain open |
| R10-B9 | production entry/callers: configured `NativeInferenceClient` R4-B6 conversation fixture and new `Spec182R4B6RealProviderRepositoryReference`; implementation/wire: `NativeApplicationInput::RepositoryReference` and v2 `input_transport`/`input_reference`; test/harness/oracle: Provider ACK exact data/manifest observation plus inline regression selector; build/source closure: existing `integration-tests` Waf target and `ndnsf-di-core-flow.t.cpp`; migration/evidence: requester/Core wire boundary only, Provider fetch/decrypt R10-B5/B6 and T016 remain open | DONE for this bounded requester Core-wire boundary | R10-B8; R4-B6 real Provider conversation | canonical REPO_REF metadata must reach the real Core/Provider fixture without inline payload; no requester-side decryption or qualification promotion | static review covers payload ownership, temporary-buffer lifetime, exact reference identity, selector registration and baseline preservation; first oracle defect linked in failure-log | system-first `-j4` integration build 118/118, 35.882s; baseline inline and new REPO_REF selectors each pass; final vmstat no sustained swap | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B9 evidence](evidence/r10-b9-requester-repo-ref-core-20260909.md); raw `.codex-tmp/spec182-r10-b9/`; Provider fetch/decrypt, maintained caller execution and T016 remain open |
| R10-B11 | production entry/callers: configured `NativeInferenceClient` R4-B6 `REPO_REF` conversation selector → real `ServiceProvider` collaboration handler; implementation/wire: Provider parses the emitted v2 reference and invokes `CollaborationContext::fetchEncryptedLargeData`; test/harness/oracle: recovered plaintext is compared with the native publisher bytes and inline baseline remains green; build/source closure: existing `integration-tests` Waf target and `ndnsf-di-core-flow.t.cpp`; migration/evidence: one-process requester/Provider fetch boundary only, malformed negatives remain R10-B5/B6 and T016 remains open | DONE for this bounded requester-produced reference consumption boundary | R10-B9; R10-B5/B6 fetch implementation | the Provider must consume the exact requester-produced data name and plaintext before conversation output; no cross-process or qualification promotion | static review covers request payload copy lifetime, exact data-name binding, fetch scope/service binding, failure propagation and selector registration | system-first `-j4` integration build 118/118, 35.626s; `Spec182R4B6RealProviderRepositoryReference` and inline regression each pass; vmstat no sustained swap after initial samples | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B11 evidence](evidence/r10-b11-requester-repo-ref-provider-fetch-20260909.md); raw `.codex-tmp/spec182-r10-b11/`; cross-process maintained caller execution and T016 remain open |
| R10-B12 | production entry/callers: current source-alignment audit after R10-B11; implementation/wire: audit records one-process requester → Core → Provider fetch/decrypt → conversation result and preserves `NATIVE_REQUEST_PIPELINE_NOT_READY` fail-closed behavior; test/harness/oracle: audit/task/evidence link and status checks; build/source closure: documentation-only; migration/evidence: current `0656c2e4` checkpoint with maintained caller cross-process execution, stream/recovery, legacy zero-use and T016 owners still open | DONE for this bounded audit checkpoint refresh | R10-B11; current tasks/evidence registry | audit must reflect the latest Provider fetch observation without promoting local closure to qualification | static review covers source checkpoint, current route, remaining owner matrix and historical dates | documentation-only: `git diff --check`; `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B12 evidence](evidence/r10-b12-current-audit-refresh-20260909.md); product execution and T016 remain open |
| R10-B13 | production entry/callers: `Spec175NativeAssembly` integration fixtures invoking `prepareNativeCanonicalOnnxRole` and the real assembly worker; implementation/wire: test recipe digest now preserves production contract order for `inputNames`/`outputNames`; test/harness/oracle: four previously failing integration selectors plus unaffected ORT assembly cases; build/source closure: `integration-tests` Waf target and `ndnsf-di-native-assembly.t.cpp`; migration/evidence: first full-suite boundary is linked in failure-log, complete T016 matrix remains open | DONE for this bounded test-oracle repair and affected suite | R10-B11; production canonical serializer contract | helper must derive the exact same canonical recipe bytes as production; no production or protocol fallback | static review covers canonical field ordering, all helper call sites, worker digest boundary, selector registration and unchanged fixture inputs | system-first `-j2` rebuild and affected suite selectors; full integration rerun remains required after this repair | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS` for affected suite; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B13 evidence](evidence/r10-b13-integration-recipe-oracle-repair-20260909.md); raw `.codex-tmp/spec182-t016-r3/` and retry directory; full T016 matrix remains open |
| R10-B14 | production entry/callers: all registered native unit/integration targets from the frozen case manifest; implementation/wire: same-source DI/Core/ONNX/Provider contracts after R10-B13 repair; test/harness/oracle: complete `unit-tests` and complete `integration-tests`; build/source closure: `build-nac182`, system-first Waf target closure and resource trace; migration/evidence: complete local suites are green, while MiniNDN owner context, maintained-caller cross-process execution, no-Python and T016 remain open | DONE for this bounded same-source unit/integration validation | R10-B13; T015 static owner remains open | full local suites must pass from the repaired source; no MiniNDN or qualification promotion from this batch | static review covers frozen target registration, repaired fixture identity, complete suite selectors and audit/task evidence links | `unit-tests --log_level=test_suite`; `integration-tests --log_level=test_suite`; `vmstat 1` resource captures | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B14 evidence](evidence/r10-b14-full-unit-integration-20260909.md); raw `.codex-tmp/spec182-t016-r3/` and `.codex-tmp/spec182-t016-r4/`; MiniNDN/no-Python/cross-process/T016 remain open |
| R10-B15 | production entry/callers: `Experiments/NDNSF_DI_NativeClosure_Minindn.py` campaign owner → `tests/standalone/run-spec182-native-closure.py`; implementation/wire: fresh manifest copy, NFD socket preflight and owner result boundary; test/harness/oracle: `campaignCase=I01` registration plus explicit `MININDN_NODE_CONTEXT_NOT_PROVIDED` result; build/source closure: Python harness only, no product rebuild; migration/evidence: NFD is available but no MiniNDN node/netns metadata, so cross-process/no-Python/T016 qualification remains open | DONE for this bounded fresh preflight recheck; T016 remains PARTIAL/UNQUALIFIED | R10-B14; external NFD service and owner node context | confirm the prior socket blocker changed without treating the stub owner result as protocol evidence; no product or qualification promotion | static review covers owner/runner call, manifest schema, result reason and fresh output; the review record identifies the missing node-context lane | `nfd-start`; `nfdc status report`; `test -S /run/nfd/nfd.sock`; `ip netns list`; campaign owner with `--manifest ... --output ...` | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `FOCUSED_BEHAVIOR_PASS` for preflight reason; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B15 evidence](evidence/r10-b15-t016-preflight-recheck-20260909.md); raw `.codex-tmp/spec182-t016-r5/` and `.codex-tmp/spec182-t016-preflight-20260909/`; next owner batch must create real node/netns metadata |
| R10-B16 | production entry/callers: `run_case` → `make_launch` in `tests/standalone/run-spec182-native-closure.py`; implementation/wire: validated `nodes` context, held `/proc/.../ns/net` descriptor and `nsenter` → bubblewrap/strace launch; test/harness/oracle: node inode/PID/start-ticks/socket/peer validation, missing/stale-context negatives, and no incidental namespace adoption; build/source closure: Python harness only, no product rebuild; migration/evidence: runner now has an explicit node-context boundary, while MiniNDN topology/case definitions and T016 qualification remain open | DONE for this bounded runner context boundary; T016 remains PARTIAL/UNQUALIFIED | R10-B15; native-isolation-design node contract | declared-node cases fail closed without valid context and use the held namespace FD when valid; unbound cases cannot adopt an incidental namespace FD; no host-netns masquerade or qualification promotion | static review covers node validation, fd lifetime, `nsenter` argv, process cleanup, test registration and source closure; migration lane remains open for owner topology and business cases | 23 `test_spec182_native_closure.py` cases, `py_compile`, `git diff --check`; no native build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B16 evidence](evidence/r10-b16-native-closure-node-context-20260909.md); next owner batch must provide actual MiniNDN node contexts |
| R10-B17 | production entry/callers: `main --execute-owner` → `_run_owned_campaign` in `Experiments/NDNSF_DI_NativeClosure_Minindn.py`; implementation/wire: tracked two-node MiniNDN topology, per-node NFD startup, socket readiness and `collect_node_context`; test/harness/oracle: pure context export plus host-namespace and outside-topology peer negatives, fresh raw owner run; build/source closure: Python owner/manifest/topology only, no native rebuild; migration/evidence: raw context is now available to a future runner invocation, while executable closure case definitions and T016 qualification remain open | DONE for this bounded owner-context producer; T016 remains PARTIAL/UNQUALIFIED | R10-B16; native-isolation-design node contract | explicit owner mode creates requester/provider namespaces and emits identity-bound context; absent closure artifact/process case returns `NATIVE_CLOSURE_CASE_DEFINITION_MISSING`; default registration-only mode remains unchanged | static review covers owner lifecycle, argv isolation, topology binding, NFD readiness, context identity, cleanup and test registration; no collector or business result is duplicated | 26 Python cases, `py_compile`, `git diff --check`; explicit root owner run exit 2 with `node-context.json` and `NATIVE_CLOSURE_CASE_DEFINITION_MISSING` | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B17 evidence](evidence/r10-b17-minindn-owner-context-20260909.md); next batch must freeze runner case artifact/process definitions and keep owner alive through execution |
| R10-B18 | production entry/callers: `run_case` → `make_launch` → `collect_trace` in `tests/standalone/run-spec182-native-closure.py`; implementation/wire: absolute `--ro-bind` mounts for declared ELF shared libraries and per-PID pairing of strace `<unfinished ...>`/`<... resumed>` events; test/harness/oracle: paired/unpaired trace fixtures and root dynamic-ELF probe; build/source closure: Python runner only, no native rebuild; migration/evidence: dynamic native process now executes in the isolated root with a complete observation, while business evidence and owner-alive integration remain open | DONE for this bounded native process/trace boundary; T016 remains PARTIAL/UNQUALIFIED | R10-B16; R10-B17; native-isolation-design filesystem/trace contract | declared dynamic ELF loader/dependencies are visible at canonical absolute paths; normal strace continuation is not treated as corruption; incomplete traces remain `UNQUALIFIED` | static review covers mount ordering, artifact identity, collector pairing, command call sites and regression registration; no host `/lib` tree is mounted | 29 Python cases, `py_compile`, `git diff --check`; root `/bin/true` probe returns 0 with complete trace but no business evidence | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B18 evidence](evidence/r10-b18-runner-elf-trace-boundary-20260909.md); next batch must use an executable native case and owner-alive runner invocation |
| R10-B19 | production entry/callers: `main --execute-owner --runner-manifest` → `_run_owned_campaign` → `_execute_runner_case` → canonical `run_case`; implementation/wire: owner keeps tracked MiniNDN requester/provider namespaces and NFD sockets alive while loading, staging, launching and evaluating the runner case, then writes `node-context.json` and `runner-result.json`; test/harness/oracle: real root owner run with executable `/bin/true` case, held namespace FD, complete trace and integrity observation; build/source closure: Python owner/runner handoff only, no native rebuild; migration/evidence: owner-alive handoff is now exercised and recorded, while native DI business cases, maintained caller execution, no-Python execution and T016 qualification remain open | DONE for this bounded owner-to-runner handoff; T016 remains PARTIAL/UNQUALIFIED | R10-B17; R10-B18; native-isolation-design node and process contracts | runner receives the exact live node context and canonical evaluator result; `PASS`/`FAIL`/`UNQUALIFIED` exit mapping is preserved; missing business evidence cannot be promoted to qualification | static review covers lifecycle ordering, manifest/case selection, runner call sites, result persistence, cleanup and no duplicate collector/oracle; official `review-agent` path is recorded in evidence | 29 Python cases, `py_compile`, `git diff --check`; root owner/runner run exits 2 with runner returncode 0, complete trace and no business evidence | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B19 evidence](evidence/r10-b19-owner-runner-handoff-20260909.md); next batch must replace the `/bin/true` probe with an executable native DI case and business oracle |
| R10-B20 | production entry/callers: canonical `run_case` → `collect_trace` → `evaluate_case`; implementation/wire: derive runtime evidence from PID/exec/exit/namespace/endpoint/cleanup observations and consume an optional manifest-declared independent business marker; test/harness/oracle: trace fixtures plus marker-present/missing verdict cases; build/source closure: Python collector only, no native rebuild; migration/evidence: close the evidence-generation gap that made every composed runner result `UNQUALIFIED`, while native DI business semantics and T016 qualification remain open | DONE for this bounded collector-evidence boundary; T014/T016 remain PARTIAL/UNQUALIFIED | R10-B19; native-isolation-design evidence contract | evidence is derived only from observed run/trace state; missing or incomplete observation remains `UNQUALIFIED`; a declared marker can satisfy only `business-oracle`, never protocol qualification by itself | official `review-agent` static review found no introduced defect; collector call sites, evidence derivation, manifest validation, marker independence and regression selectors are covered | 31 focused Python cases, `py_compile`, `git diff --check`; fresh root owner/runner run exit 2 with six runtime evidence classes and missing business marker; no native rebuild | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B20 evidence](evidence/r10-b20-collector-evidence-boundary-20260909.md); next batch must provide a real native DI business oracle and complete T014/T016 cases |
| R10-B21 | production entry/callers: tracked MiniNDN owner → canonical runner `I01`; implementation/wire: current same-source `build-nac182/spec182-installed-consumer` with its declared 31-file ELF closure, held node namespace FD and absolute tool paths; test/harness/oracle: I01 independent stdout marker `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK` plus complete trace/evidence evaluation; build/source closure: existing installed-consumer target and recorded ELF hashes, no rebuild in this batch; migration/evidence: first real native C++ consumer passes inside owner namespace, while native DI request/result semantics, I02-I08, maintained callers, no-Python and T016 remain open | DONE for this bounded I01 positive native-consumer case; T014/T016 remain PARTIAL/UNQUALIFIED | R10-B20; T002-A installed consumer boundary; native-isolation-design I01 | the same canonical runner observes a non-Python C++ consumer with returncode 0, complete trace, all required evidence and independent marker; this result does not stand in for a DI request or full qualification | static review covers generated manifest identity, artifact hash/target mapping, owner-alive handoff, marker oracle, command/cleanup and existing case registration; no product source regression found | root owner/runner run `.codex-tmp/spec182-r10-b21-native-consumer-owner/` exits 0; evaluator `PASS`, 31 focused Python cases remain green; no native rebuild | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B21 evidence](evidence/r10-b21-native-consumer-i01-pass-20260909.md); next batch must define executable native DI requester/provider cases and run I02-I08/PO cases |
| R10-B22 | production entry/callers: canonical owner → runner → existing `integration-tests` selector `Spec182R4B6RealProviderConversation`; implementation/wire: same-source C++ `NativeInferenceClient` runtime, real `ServiceProvider` ingress and two-turn conversation fixture execute in the isolated owner namespace; test/harness/oracle: independent stdout marker `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` plus trace/evidence evaluator; build/source closure: `integration-tests` target and its declared ELF closure, rebuilt after marker change; migration/evidence: direct native DI selector passes, while owner/runner reaches fixture setup only after manifest/ELF repairs and remains unqualified because `examples/trust-any.conf` is absent in the minimal root | PARTIAL | R10-B21; R4-B6 fixture; T011-C/T016 acceptance | marker is emitted only after both native requests and result assertions succeed; runner evidence cannot promote an in-process fixture to cross-process qualification | static review covers fixture call sites, marker placement, target registration, manifest closure and independent oracle; v1-v4 owner runs preserve executable-target, interpreter, loader-search and config first boundaries | build output `.codex-tmp/spec182-r4-b2/build/integration-tests` rc=0 in 37.778s with `-j4`; direct selector emits marker rc=0 in 6.801s; owner/runner v4 returns `UNQUALIFIED` at fixture setup (`returncode=201`) | `STATIC_PASS`; `BUILD_PASS`; direct selector `FOCUSED_BEHAVIOR_PASS`; owner/runner `UNQUALIFIED`; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B22 evidence](evidence/r10-b22-native-di-business-case-20260909.md); next batch must add bounded runner config/workdir support before a fresh owner case |
| R10-B23 | production entry/callers: `run_case` → `make_launch` with a manifest-declared staged-root working directory; implementation/wire: optional process `workingDirectory` is validated and passed to bubblewrap, while declared data/config artifacts remain inside `/probe-root`; test/harness/oracle: working-directory validation/argv regression plus the R10-B22 `trust-any.conf` fixture; build/source closure: Python runner only, no native rebuild; migration/evidence: repair the first owner/runner setup boundary without exposing host config paths or allowing arbitrary host cwd | DONE for this bounded runner working-directory/config boundary; T014/T016 remain PARTIAL/UNQUALIFIED | R10-B22; native-isolation-design filesystem contract | only absolute paths under `/probe-root` or `/tmp` are accepted; relative fixture paths resolve inside staged root, and host paths remain inaccessible | static review covers process manifest validation, launch argv, artifact staging, test registration and no host-path escape; batch result records all five lanes and four retrospective categories | 33 focused Python cases PASS, `py_compile` and design validator pass; fresh owner/runner `PO-001` with `/probe-root` cwd and staged config returns evaluator `PASS`; no native build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B23 evidence](evidence/r10-b23-runner-working-directory-20260909.md); native DI multi-process transport, maintained callers, I02-I08 and T016 qualification remain open |
| R10-B24 | production entry/callers: existing Spec182 C++ unit/integration targets and `Spec182R4B6RealProviderConversation`; implementation/wire: no source change, prior native target boundary reused; test/harness/oracle: exact Boost.Test selectors and existing independent assertions; build/source closure: existing `build-nac182` binaries, no rebuild; migration/evidence: establish a fresh native baseline before the next production-chain batch while preserving observed swap pressure | DONE for this bounded native-suite baseline; parent tasks and T016 remain PARTIAL/UNQUALIFIED | R10-B23; existing native target and fixture contracts | all selected C++ cases pass; no qualification status is promoted from an unchanged binary baseline | static review covers selector ownership, unchanged target closure, logs and resource observation; no new code or test registration | 247 Spec182 unit cases, 2 Spec182 grant integration cases, 3 R4-B6 conversation/replacement cases and 1 repository-reference case pass; raw logs and `vmstat` retained; no rebuild | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B24 evidence](evidence/r10-b24-native-suite-baseline-20260909.md); next batch must address maintained caller execution, cross-process transport or registered I02-I08 cases |
| R10-B25 | production entry/callers: `DI_NativeRequester` CLI `main` and its Waf target; implementation/wire: `examples/DI_NativeRequester.cpp` argument/schema loader and existing native runtime composition; test/harness/oracle: `--help`, usage and invalid-schema command checks; build/source closure: `examples/wscript` `DI_NativeRequester` target and linked `ndnsf-distributed-inference`; migration/evidence: command-level boundary only, no real Core/Provider request or qualification | DONE for this bounded executable/CLI boundary; parent tasks and T016 remain PARTIAL/UNQUALIFIED | R10-B24; R5-B5 native requester configuration | target links with the documented system-first toolchain and CLI fails closed on malformed invocation/config; no CLI result is promoted to native behavior or qualification | corrected static review covers source, `main`, Waf registration, linked target and CLI oracle; initial self-check literal mismatch is recorded as a static miss and corrected; five lanes and four retrospective categories are recorded | `-j2` targeted build exits 0 in 1m17.740s; help exits 0, bare invocation exits 2, invalid schema exits 1 with no output; `ldd` has no `not found`; `vmstat` second sample `si=372`, `so=0`; real request/result and T016 remain open | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS` for CLI only; `CLOSED_FOR_VALIDATION` locally; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B25 evidence](evidence/r10-b25-native-requester-cli-20260909.md); next boundary is a real requester/provider execution with declared config/fixture |
| R10-B26 | production entry/callers: `runNativeIngressCase` → `ServiceProvider::CollaborationContext::fetchEncryptedLargeData` → `ctx.fail`; implementation/wire: missing v2 `REPO_REF` object and scoped 1 s fetch budget already present in the Provider fixture; test/harness/oracle: `Spec170NativePostSelection/ProductionIngressRejectsMissingNativeRepositoryReference`; build/source closure: existing `.codex-tmp/spec182-r4-b2/build/integration-tests`, no rebuild; migration/evidence: recheck resolves the old R10-B6 fixed-pump boundary only, with cross-process/T016 still open | DONE for this bounded runtime recheck; parent T008/T013/T016 remain PARTIAL/UNQUALIFIED | R10-B6; current Provider negative source and binary | missing object must fail in the real Provider handler before runner input or successful Response; no qualification promotion | static review covers current source, selector, scoped timeout, status/response assertions, target ownership and failure-log update; no source change | selector exit 0, testing time 1.946850s; expected fetch-failure status observed, no output/runner input; `vmstat` second sample `si=28`, `so=0`; no rebuild | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B26 evidence](evidence/r10-b26-missing-repo-ref-recheck-20260909.md); older R10-B6 failure retained, current missing-object boundary closed; cross-process, maintained caller and T016 remain open |
| R10-B27 | production entry/callers: N/A (skill/template governance); implementation/wire: shared `batch-quality-gates.md` and `speckit-code-design/SKILL.md` now require C++ fixture/driver/oracle for native behavior tests; test/harness/oracle: spec/tasks templates require a production C++ target/selector and constrain Python to orchestration/binding/offline boundaries; build/source closure: N/A (no product target change); migration/evidence: `skills/README.md`, installed `speckit-code-design` copy and local Spec Kit entrypoints remain synchronized | DONE for this bounded skill/template synchronization; product parent tasks and T016 remain PARTIAL/UNQUALIFIED | R10-B26; R8-SKILL shared review contract | future native behavior test cards must name C++ test implementation and direct production target; Python-only native assertions remain `gap`/`PARTIAL` | static review covers shared reference, code-design entry, README, spec/tasks templates, installed SHA pair and local entrypoint references; five lanes and four retrospective categories are recorded | `git diff --check`; key-rule `rg`; `validate_design.py`; versioned/installed SHA comparison; no product build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for skill boundary; not `QUALIFICATION_PASS` | [R10-B27 evidence](evidence/r10-b27-native-cpp-test-ownership-20260909.md); cross-process, maintained caller, I02--I08, no-Python and T016 remain open |
| R10-B28 | production entry/callers: N/A (workflow documentation); implementation/wire: `AGENTS.md`, `docs/agentic_workflow.md` and local `CLAUDE.md` now distinguish required Context/CodeGraph/Spec Kit gates from conditional GSD/ARS and carry the shared native-test contract; test/harness/oracle: N/A (document-only); build/source closure: N/A (no product target); migration/evidence: `skills/README.md`, constitution 1.5.0 and shared `batch-quality-gates.md` cross-checked | DONE for this bounded workflow-authority synchronization; product parent tasks and T016 remain PARTIAL/UNQUALIFIED | R10-B27; constitution 1.5.0; shared batch contract | future executors see one required/optional tool boundary and the native C++ test ownership rule; no product status promotion | static review covers current overview, local Claude entry, canonical AGENTS rules, constitution, shared skill and contradiction scan; five lanes and four retrospective categories recorded | `git diff --check`; targeted `rg`; `validate_design.py`; shared-reference SHA check; no product build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for workflow docs; not `QUALIFICATION_PASS` | [R10-B28 evidence](evidence/r10-b28-workflow-authority-alignment-20260909.md); native request/result, maintained callers, I02--I08, no-Python and T016 remain open |
| R10-B29 | production entry/callers: canonical `run_case`/`make_launch` for every declared requester/provider process; implementation/wire: per-process node validation, held namespace FD, explicit NDN config environment, harness-owned stable supervisor process group, shared deadline and TERM→KILL cleanup; test/harness/oracle: process-selection, environment, supervisor lifecycle and shared-executable role-coverage fixtures; build/source closure: Python runner and `test_spec182_native_closure.py` only, no native rebuild; migration/evidence: cross-process runner execution is now structurally wired, while real MiniNDN requester/provider transport, I02--I08 and T016 remain open | DONE for this bounded harness lifecycle boundary; T014/T016 remain PARTIAL/UNQUALIFIED | R10-B23; native-isolation-design process/node/evidence contract | every declared process is launched with its own trace/output and explicit environment; peer startup cannot be hidden by one shared executable match; single-process result shape remains compatible | static review covers all changed runner/test call sites, FD/process-group ownership, environment and result compatibility; five lanes and four retrospective categories recorded | `python3 -m pytest -q tests/python/test_spec182_native_closure.py` (36 passed); `py_compile`; `git diff --check`; no native build or MiniNDN run | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for this harness boundary; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B29 evidence](evidence/r10-b29-runner-multiprocess-lifecycle-20260909.md); real requester/provider transport, I02--I08, maintained caller/no-Python and T016 remain open |
| R10-B30 | production entry/callers: canonical `collect_trace`/`load_case` for declared child and endpoint boundaries; implementation/wire: assembly-worker must name a Provider parent, endpoint owner/peers/addresses are validated, lifecycle syscall events are retained, successful child execs are bound to clone/fork links, and successful connects are matched against the manifest; test/harness/oracle: valid/invalid child and UNIX endpoint fixtures, clone/open/connect trace and missing-child verdict; build/source closure: Python runner and `test_spec182_native_closure.py` only, no native rebuild; migration/evidence: child/endpoint semantics are now explicit for the future I02-I08 campaign, while real injected worker/endpoint cases and T016 remain open | DONE for this bounded harness observation boundary; T014/T016 remain PARTIAL/UNQUALIFIED | R10-B29; native-isolation-design child/endpoint and trace contract | malformed or abstract endpoints fail before launch; undeclared successful connects and missing declared child execs remain observable failures; syscall observations do not promote protocol status | static review covers changed manifest/collector code, callers, child ownership, endpoint policy and test registration; five lanes and four retrospective categories recorded | `python3 -m pytest -q tests/python/test_spec182_native_closure.py` (40 passed); `py_compile`; `validate_design.py`; `git diff --check`; no native build or MiniNDN run | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for this harness boundary; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B30 evidence](evidence/r10-b30-runner-child-endpoint-observation-20260909.md); real worker parentage, endpoint injection, I02--I08, maintained caller/no-Python and T016 remain open |
| R10-B31 | production entry/callers: `NativeInferenceClient::request` through `ServiceUser::BeginCollaboration` to the real Provider callback; implementation/wire: bounded unary mode uses `TOKEN_DIAGNOSTIC`, native catalog/grant/admission/preparation/placement and terminal `Response`; test/harness/oracle: `Spec182R10B31RealProviderUnaryRequest` asserts ACK, Core commit, Provider callback and exact result payload, with R4-B6 regressions; build/source closure: `integration-tests` target built with system-first `-j4`, no Python product path; migration/evidence: closes the missing non-streaming native-client observation while cross-process worker, maintained caller and T016 remain open | DONE for this bounded native-client unary boundary; T010-B/T013/T016 remain PARTIAL/UNQUALIFIED | R4-B6 real Provider conversation fixture; T010-B orchestration contract | unary options do not allocate stream or conversation state; terminal Response is decoded by the native adapter and the client rejects malformed terminal binding | static review covers changed helper branch, captures, provider callback, native client call and selector registration; five lanes and four retrospective categories recorded | `./waf build --targets=integration-tests -j4` (35.584s); unary selector PASS (3.559s); R4-B6 conversation/replacement (3 cases, 20.366s) and repository reference PASS (6.939s); `git diff --check` | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for this unary boundary; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B31 evidence](evidence/r10-b31-native-client-unary-request-20260909.md); deployed Provider worker, cross-process transport, maintained caller/no-Python, I02--I08 and T016 remain open |
| R10-B32 | production entry/callers: N/A (shared workflow documentation); implementation/wire: `skills/speckit-code-design/SKILL.md`, `references/review-agent.md`, `references/batch-quality-gates.md` and Spec Kit templates now require explicit release checks; test/harness/oracle: Minimum Review Record requires actual fixture/oracle and selector-registration checks; build/source closure: release checklist requires target/source-list/link closure; migration/evidence: `Changed gate` preserves first failure boundaries and `Batch growth decision` records stop/split decisions | DONE for this bounded skill/template feedback-loop boundary; product parent tasks and T016 remain PARTIAL/UNQUALIFIED | R10-B31; R10-B27/R10-B28; shared workflow contract | future static gates must show concrete caller, test-registration, source-closure and migration checks; retries must identify a changed gate; stable exits cannot absorb different callers or acceptance dependencies | static review covers shared references, templates, README and installed SHA pair; five lanes and four retrospective categories recorded | `git diff --check`; targeted `rg`; `sha256sum` versioned/installed code-design files; `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`; no product build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for skill documentation; not `QUALIFICATION_PASS` | [R10-B32 evidence](evidence/r10-b32-skill-feedback-loop-20260909.md); native request/result, maintained caller, cross-process and T016 remain open |
| D-SKILL-CLI-BOUNDARY | production entry/callers: N/A (documentation rule); implementation/wire: `skills/speckit-code-design/references/batch-quality-gates.md`, `skills/speckit-code-design/SKILL.md`; test/harness/oracle: N/A (template/reference checks only); build/source closure: N/A (no product target); migration/evidence: shared README/templates and installed code-design copy | DONE for this documentation boundary; product tasks unchanged | existing shared batch-quality contract | all affected references state CLI/harness smoke is wiring evidence only | static review covers changed reference, templates, README and installed SHA pair | `git diff --check`, validator, reference-link scan and SHA-256 comparison pass; no product build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [CLI boundary evidence](evidence/skill-cli-boundary-20260909.md); product behavior and T016 remain open |
| D-SKILL-CONTEXT-POINTER | production entry/callers: N/A (operational documentation); implementation/wire: `skills/speckit-code-design/SKILL.md` `Context Pointer Updates`, `.agents/skills/speckit-agent-context-update/SKILL.md`; test/harness/oracle: marker/plan/sync checks only; build/source closure: N/A (no product target); migration/evidence: `skills/README.md`, local Spec Kit entrypoints and personal shared skill | DONE for this operational workflow boundary; product tasks unchanged | D-SKILL-CLI-BOUNDARY; D-SKILL-REVIEW-COVERAGE | managed context block remains pointer/context only; exactly one marker pair, active plan pointer and shared sync are checked; no `STATIC_PASS`/`BUILD_PASS`/`DONE` product claim | static review covers versioned skill, operational entrypoint, README and installed SHA pair; five lanes and four retrospective categories recorded | `verify-spec-kit-sync.py --require-entrypoints --require-personal`, `git diff --check`, `validate_design.py`, marker and SHA checks pass; no product build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [Context pointer evidence](evidence/skill-context-pointer-20260909.md); native request/result, maintained caller/no-Python and T016 remain open |
| R10-B10 | production entry/callers: current source-alignment audit after R10-B9; implementation/wire: audit summary names configured requester `REPO_REF` and Provider fetch/decrypt as the next boundary; test/harness/oracle: audit/task/evidence link consistency and validator; build/source closure: documentation-only; migration/evidence: current `9f80a1ce` checkpoint and remaining T004/T008/T010/T011/T013/T016 owners | DONE for this bounded audit checkpoint refresh | R10-B9; current tasks/evidence registry | audit must reflect the newest requester/Core-wire exit without rewriting historical findings or promoting qualification | static review covers source checkpoint, current route, remaining chain and historical section dates | documentation-only: `git diff --check`; `python3 specs/182-native-di-python-bindings/checklists/validate_design.py` | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` locally; not `QUALIFICATION_PASS` | [R10-B10 evidence](evidence/r10-b10-current-audit-refresh-20260909.md); product execution and T016 remain open |
| R10-B54 | production entry/callers: `DI_NativePlanManifestSmoke.cpp::main` → plan/manifest loaders → native role registration/dependency publication; implementation/wire: `NativeExecutionPlanJson`, `NativeServiceManifest`, `NativeProviderSession`, canonical ONNX helpers; test/harness/oracle: four-role `spec174-exact-bundle-gpu-v5` marker with role/artifact/output-tensor assertions and target registration; build/source closure: initial 55/55 link miss retained, project-symbol definition map, repaired Waf source/link closure, candidate framework export/RUNPATH and default `ldd`; migration/evidence: fresh raw build/loader/smoke outputs, no Python/network process | initial static gate missed target source/link closure; first linker boundary mapped ONNX helpers to `NativeOnnxRecipeAssembler.cpp`/`NativeOnnxAssemblyWorker.cpp` and ServiceUser APIs to candidate framework; shared skill now requires this definition map for link retries; re-review no P1/P2/P3 | initial link exit `1` after 55/55 tasks, elapsed `133.41s`; retry after source/link repair exit `0`, 85/85 tasks, elapsed `170.09s` | bundle-root no-env smoke exit `0`, `NDNSF_DI_NATIVE_PLAN_MANIFEST_SMOKE_OK roles=4 artifacts=4 outputTensors=8`, elapsed `0.06s`; no protocol request | `./waf -o .codex-tmp/spec182-r4-b2/build build --targets=di-native-plan-manifest-smoke -j2`; target RUNPATH selects candidate framework; resource policy remains `-j2` after observed swap-in | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for local plan/manifest and role/output smoke; not Provider transport or T016 qualification | [R10-B54 evidence](evidence/r10-b54-plan-manifest-smoke-20260909.md); Provider `--serve`, requester/Provider transport, maintained caller/no-Python and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B57 | production entry/callers: `Spec182V3Placement/PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks` → `runPublicClientScenario` → `NativeInferenceClient`/`LocalServiceUser`; implementation/wire: asynchronous operation keeps the external Face alive until detached worker callbacks release the last `ServiceUser`; test/harness/oracle: three scenario loop plus isolated selector reruns and GDB destructor backtrace; build/source closure: `tests/unit-tests/di-native-v3-placement.t.cpp` registered in `unit-tests`, canonical `build-nac182` tree; migration/evidence: repair fixture lifetime only, preserve production callback and close semantics | DONE for this local fixture lifetime boundary; T010-B/T011-C/T016 remain PARTIAL/UNQUALIFIED | R10-B49; initial SIGSEGV raw runs; T010/T016 lifecycle ownership | no `ServiceUser` destructor may race `DummyClientFace` reactor/timer teardown; selector passes repeatedly and the full Spec182 unit plus existing integration flow pass under one source/build identity | static review covers fixture declaration order, shared ownership, all `face` call sites and no production ownership change; canonical unit/integration source closure and registration checked; changed gate is explicit `WAFLOCK=.lock-waf` plus Face-owner lifetime check | initial full suite aborted with SIGSEGV at line 1026; isolated runs alternated memory fault and pass; GDB first boundary was `ServiceUser::~ServiceUser`/`ndn::Scheduler::~Scheduler`; after repair 10/10 isolated runs pass, Spec182 unit 249/249 cases and integration flow 55/55 cases pass | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for this fixture boundary; not T016 qualification | [R10-B57 evidence](evidence/r10-b57-native-v3-placement-lifetime-20260909.md); preserve initial raw boundaries; cross-process ownership, maintained caller/no-Python and T016 remain open |
| R10-B58 | production entry/callers: N/A (workflow governance); implementation/wire: shared batch/review references plus Spec/plan/tasks templates; test/harness/oracle: templates require async fixture owner or join/drain barrier and repeated selector; build/source closure: N/A (documentation-only); migration/evidence: `skills/README.md`, `AGENTS.md`, `CLAUDE.md`, `docs/agentic_workflow.md` aligned | DONE for shared async-fixture lifetime rule; product tasks unchanged | R10-B57 runtime destructor race; shared workflow contract | future detached native tests must declare external Face/io_context/scheduler/timer/callback ownership and destructor order; no production close/callback changes for fixture races | static review covers all changed shared references, templates, local entry guidance and personal SHA pair; five lanes and four retrospective categories recorded | `git diff --check`; `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --require-entrypoints --require-personal`; `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`; no product build | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for governance rule; not product or T016 qualification | [R10-B58 evidence](evidence/skill-async-fixture-lifetime-20260909.md); applies to future Specs; R10-B57 and all product qualification gates remain separately evidenced |
| R10-B55 | production entry/callers: `DI_NativeProviderExecutable.cpp::main --serve` → `ServiceProvider`/`NativeInferenceProvider::serve`; implementation/wire: metadata-only serving manifest, DATA_DRIVEN_V2 startup and permission/readiness path; test/harness/oracle: bounded timeout probe with startup marker and first-failure classification; build/source closure: repaired Provider binary SHA/`ldd` closure from R10-B50; migration/evidence: fresh raw run, no Python, no preassembled artifact references, no protocol PASS unless serve reaches its readiness marker | DONE for bounded serve/readiness boundary; first CLI mode, missing-permission and runtime-PATH collection boundaries are retained, and read-only review found no P1/P2/P3 | no compile/link miss; repaired Provider binary reused and SHA/`ldd` closure checked | corrected probe reached `SERVE_READY` then exposed missing permission; Controller-assisted retry reached `PERMISSION_READY`, `PROVISION_READY` and `NDNSF_DI_NATIVE_PROVIDER_READY`; provider timeout `124`, Controller exit `0`; no cross-process requester/Provider or T016 claim | existing `di-native-provider`, system-first runtime PATH, timeout wrapper and fresh raw directory; no rebuild | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for serve registration/readiness success and failure classification; not Provider transport or T016 qualification | [R10-B55 evidence](evidence/r10-b55-provider-serve-preflight-20260909.md); authenticated Selection, post-Selection assembly, transport and T016 remain `PARTIAL`/`UNQUALIFIED` |
| R10-B59 | production entry/callers: `_native_qwen_request` → `APPClient.configure_native_requester_from_config`/`native_tokenizer_digest` → `request_native_reference`; implementation/wire: operator-pinned `request.tokenizer_digest` is required for `TOKEN_STREAMING`, validated as canonical SHA-256 and copied into typed generation plus application options; test/harness/oracle: real `_ndnsf` pybind DTO helper execution, configured/missing digest boundaries and legacy route regressions; build/source closure: Python/config-only change, existing native extension ABI reused; migration/evidence: fixes the native-config Qwen pre-Core blocker, while real Provider/cross-process streaming, conversation owner, full caller migration and T016 remain open | PARTIAL for this bounded native-config Qwen contract repair; parent T013-D/F/T016 remain PARTIAL | static review found and closed the empty-digest path; automatic planner metadata cannot silently supply native-config digest | no C++ compile/link change; Python tests use the existing candidate `_ndnsf` extension and fail closed before transport when the config digest is absent | `test_spec182_native_bindings.py` 13/13; combined Spec182 legacy/native Python suite 21/21; `py_compile`; `git diff --check`; no network or T016 qualification run | Python-only; native ABI unchanged; no `-j` build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | [R10-B59 evidence](evidence/r10-b59-qwen-tokenizer-digest-20260909.md); next: real Provider/cross-process Qwen stream and conversation owner |
| R10-B60 | production entry/callers: `NDNSF_DI_QwenAckDriven_Minindn.py::build_delegate_argv/run_from_environment` → `NDNSF_DI_LlmPipeline_Minindn.py::main` → maintained Qwen User; implementation/wire: native config selects `qwen-onnx-cpu-native`, is parsed/validated and forwarded to User while automatic-planning arguments are excluded; test/harness/oracle: wrapper argv and runner parser contract plus existing Qwen/native/legacy Python regressions; build/source closure: Python/launcher-only, no C++ or ABI change; migration/evidence: closes the maintained launcher contract gap but not real Provider/cross-process behavior | PARTIAL for native-config Qwen launcher contract; parent T013-D/T016 remain PARTIAL | static review found and repaired missing runner flag, wrong runtime selection and planner-argument conflict; no real request claim | no C++ compile/link miss; `py_compile` and focused Python checks pass | 27/27 focused Python tests; parser/argv contract assertions; no MiniNDN or T016 qualification run | Python-only; native ABI unchanged; no `-j` build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | [R10-B60 evidence](evidence/r10-b60-native-qwen-launcher-contract-20260909.md); next: real native requester → Core → Provider Qwen run |
| R10-B61 | production entry/callers: maintained `_run_qwen_transformer_generation_sample` native-config full-generation branch → `_native_qwen_request` → `request_native_reference`; implementation/wire: native `NDNSF-DI-FINAL-V1` `tokenIds` are decoded and normalized to `generatedTokenIds`, with observer/terminal checks; operator-pinned tokenizer digest remains the only native-config source; test/harness/oracle: production helper executes with real `_ndnsf` generation/stream DTOs, a terminal observer callback and a complete response payload, then checks EOS/text/token/digest results; build/source closure: Python caller/test only, no C++ or ABI change; migration/evidence: closes the caller decode/field gap while real requester → Core → Provider process transport, conversation owner, YOLO migration and T016 remain open | PARTIAL for native-config Qwen caller execution; parent T013-D/T016 remain PARTIAL | static review found the undefined decoder and missing protocol-to-helper field mapping; no cross-process or qualification claim | no C++ compile/link miss; `py_compile` and `git diff --check` pass | native binding + entrypoint suite 20/20 and legacy exclusion 8/8; production helper branch and typed digest assertions; no network, MiniNDN or T016 qualification | Python-only; native ABI unchanged; no `-j` build | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | [R10-B61 evidence](evidence/r10-b61-native-qwen-full-caller-20260909.md); next: real native requester → Core → Provider Qwen run |
| R10-B62 | production entry/callers: current Spec182 API/caller inventory generated from the maintained Python package; implementation/wire: `build_api_migration_manifest.py` records the current source identity and 344 surface entries; test/harness/oracle: manifest schema/status assertions plus legacy exclusion checks; build/source closure: generator uses AST/source only and does not import runtime dependencies; migration/evidence: `sourceCommit` is rebound to R10-B61 source checkpoint while semantic caller mapping, native parity and retirement remain open | PARTIAL for evidence identity refresh; parent T001/O-004/T013-B remain PARTIAL | stale manifest identity was detected and corrected; routing inventory is not semantic/runtime closure | no product build; generator and `git diff --check` pass | 344 entries (`INVENTORY_ONLY=250`, `DYNAMIC_OR_COMPATIBILITY_REVIEW=67`, `PARTIAL_EXISTING_TYPE=10`, `PLANNED_TYPE=17`); legacy exclusion 8/8; no native qualification | documentation/generator artifact only; no ABI change | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not `QUALIFICATION_PASS` | [R10-B62 evidence](evidence/r10-b62-compatibility-manifest-refresh-20260909.md); next: real native requester → Core → Provider Qwen run |
| R10-B63 | production entry/callers: `APPClient` native and planner-first methods, maintained YOLO/Qwen/llama callers, `DI_NativeRequester`, Provider executable; implementation/wire: `NativeInferenceClient`, `NativeRequestPlanner`, Provider final schema, pybind observer and Waf source closure; test/harness/oracle: 636-file Python AST parse, cppcheck, 249 C++ unit cases and existing R10-B46/B47/B48/B49/B55/B61/B62 evidence review; build/source closure: CodeGraph current index plus existing Waf/link evidence, no new build; migration/evidence: 344-entry manifest and P1–P7 production order checked against source | F-01 default planner reachability, F-02 Qwen diagnostic/stream contract conflict, F-03 observer mapping fail-open, F-04 diagnostic token field mismatch, F-05 weak-operation table growth; F-06 native/application request identity mapping remains an explicit design/evidence gap | no new compile miss; cppcheck parser/macro notices and `NativeEpochCoordinator.cpp:713` iterator warning were manually classified as false positive or non-product style/performance findings | no new runtime run; existing evidence proves only local/in-process selectors, Provider readiness, PO-001 and I01; cross-process Qwen/YOLO, conversation owner, I02–I08, no-Python and T016 remain unobserved | read-only audit; `-j4` not applicable; baseline `d5ec1996`; existing native build identities retained | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B63 evidence](evidence/r10-b63-large-scale-static-audit-20260909.md); next: repair F-02/F-03/F-04, freeze F-06 identity contract, then execute valid native-config cross-process Qwen/YOLO request |
| R10-B64 | production entry/callers: native-config Qwen helper and `APPClient.configure_native_requester_from_config`; implementation/wire: Python/C++ runtime parsers accept only `TOKEN_DIAGNOSTIC`/`TOKEN_STREAMING`, Qwen native catalog configurations require `TOKEN_STREAMING`, and the legacy diagnostic token loop is rejected on the native route; test/harness/oracle: malformed non-mapping observer payload is retained as a failed full-generation result, Qwen diagnostic/native and unsupported-mode configuration boundaries are covered; build/source closure: `unit-tests` rebuilt with system-first `-j4`; migration/evidence: F-02/F-04 are closed for this local contract boundary, while F-01/F-05/F-06, real Provider/cross-process Qwen/YOLO, conversation owner, legacy zero-use and T016 remain open | PARTIAL; parent T010/T013/T016 remain PARTIAL/UNQUALIFIED | static review traced the Python helper, runtime parser, native planner and final wire schema; the existing mapping guard is now backed by a regression case; no unsupported diagnostic field is read from native final responses | C++ parser and unit test source rebuilt; no Waf source/link miss | `python3 tests/python/test_spec182_native_bindings.py` 16/16; `py_compile`; `./waf -o build-nac182 build --targets=unit-tests -j4`; `Spec182NativePlanning/NativeRequestRuntimeLoadsPinnedPolicyAndRejectsDrift` plus `Spec182NativePlanning,Spec182NativeInferenceClient,Spec182ClientState` (49 cases) passed; no network or T016 run | system-first `-j4`; `vmstat 1` showed zero sustained `si`/`so` after the first sample, though host swap remains allocated; no new ABI or deployment artifact | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not `QUALIFICATION_PASS` | [R10-B64 evidence](evidence/r10-b64-native-config-contract-20260909.md); next: freeze request identity mapping and run a real native-config Qwen requester → Core → Provider stream |
| R10-B65 | production entry/callers: `APPClient` planner/native routes, maintained YOLO/Qwen/llama callers and `DI_NativeProviderExecutable`; implementation/wire: `NativeInferenceClient` direct contract/envelope, operation retention, `NativeInferenceProvider::serve` initialization and Provider host lifecycle; test/harness/oracle: CodeGraph call paths, 626-file Python AST parse, cppcheck, Waf source inventory and R10-B1--B64 evidence review; build/source closure: current `Experimental` source at `3608f204`, no rebuild; migration/evidence: default native migration, request identity mapping, cross-process/no-Python and T013--T017 gates remain open | completed this static audit boundary; findings F-01 default planner reachability, F-02 direct C++ generation-mode bypass, F-03 weak operation retention, F-04 non-transactional Provider host initialization, F-05 identity mapping gap, F-06 real worker/cross-process qualification gap and D-01 stale normative status text | no product code or skill change; cppcheck iterator warning was manually classified as a false positive and the remaining two diagnostics as style/maintainability items | `validate_design.py` PASS; Python AST 626/626; `verify-spec-kit-sync.py` PASS; no network, MiniNDN or T016 run | read-only audit; `-j4` not applicable | `STATIC_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B65 evidence](evidence/r10-b65-large-scale-static-audit-20260909.md); next: repair/freeze F-02--F-05, then run a real native-config requester → Core → Provider worker/cross-process path |
| R10-B66 | production entry/callers: public `NativeInferenceClient` contract/envelope construction and `NativeInferenceProvider::serve` first-host lifecycle; implementation/wire: shared generation-mode predicate, expired-operation compaction, transactional host publication and rollback; test/harness/oracle: new direct unsupported-mode, long-lived client compaction, and first-serve rollback cases plus complete `Spec182ClientState` and `Spec182ProviderHost` suites; build/source closure: Waf `unit-tests` source/link inventory rebuilt with system-first `-j4`; migration/evidence: closes R10-B65 F-02/F-03/F-04 local boundaries while F-01/F-05/F-06, caller migration, conversation owner and T013--T017 remain open | PARTIAL for bounded local repair; parent T010/T013/T016 remain PARTIAL/UNQUALIFIED | static review covered production entry, implementation/wire, tests, ownership, locks, rollback and source registration; no introduced control defect found | `unit-tests` build/link PASS (59.180 s); `git diff --check` PASS; cppcheck changed-source check PASS with only pre-existing classified diagnostics | `Spec182ClientState` 18/18 and `Spec182ProviderHost` 7/7; no network, MiniNDN, cross-process, no-Python or T016 run | system-first `-j4`; second `vmstat` sample `si=0`, `so=0`; no ABI or deployment artifact | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not `QUALIFICATION_PASS` | [R10-B66 evidence](evidence/r10-b66-native-contract-host-repair-20260909.md); next: freeze explicit request identity mapping and execute a real native-config Qwen requester → Core → Provider worker/cross-process stream |
| R10-B67 | production entry/callers: maintained `_native_qwen_request` and `APPClient.request_native_reference`; implementation/wire: `NativeRequestOptions.applicationRequestId` local correlation, native owner `requestId`, pybind handle/options properties and public facade forwarding; test/harness/oracle: C++ handle mapping assertion, Python DTO/Qwen caller/public facade forwarding tests; build/source closure: Waf `unit-tests` (`-j4`) and `ndnsf-distributed-inference` (`-j2`) rebuilt, pybind extension rebuilt against explicit NAC-ABE/ndn-svs candidate paths; migration/evidence: closes F-05 local mapping contract while F-01/F-06, real worker/cross-process, caller migration and T013--T017 remain open | PARTIAL for explicit local identity mapping; parent T010/T013/T016 remain PARTIAL/UNQUALIFIED | static review verified owner authority, correlation bounds, lock/lifetime and backward-compatible omitted arguments; initial loader failures were dependency/source-closure boundaries and were repaired | Waf `unit-tests` PASS 56.496 s; shared DI library PASS 43.736 s; pybind build PASS; candidate `ldd`/`nm` paths and mapping symbol PASS; `git diff --check` PASS | C++ mapping selector PASS; `test_spec182_native_bindings.py` 16/16; `test_ndnsf_di_app_sdk_compatibility.py` 18/18 with explicit `PYTHONPATH`/`LD_LIBRARY_PATH`; no network, MiniNDN, cross-process, no-Python or T016 run | system-first `-j4` for unit-tests then `-j2` under swap pressure; no ABI mismatch after shared-library rebuild; no deployment artifact | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `PARTIAL`; not `QUALIFICATION_PASS` | [R10-B67 evidence](evidence/r10-b67-native-application-request-mapping-20260909.md); next: execute a real native-config Qwen requester → Core → Provider worker/cross-process stream and observe both IDs |
| R10-B68 | production entry/callers: existing R4-B6 public native requester and real `ServiceProvider` fixture; implementation/wire: current NativeInferenceClient/Provider host after R10-B66/B67 repairs, including replacement and alternate replacement state; test/harness/oracle: `Spec182R4B6RealProviderConversation`, `...Replacement`, and `...AlternateReplacement`; build/source closure: integration target rebuilt from source `606230fbe783217e140499bc7ac4e9a3e65f72b0` with system-first `-j2`, explicit candidate `LD_LIBRARY_PATH`; migration/evidence: recheck confirms local real-Provider boundary with expected negative first replacement, but independent worker/cross-process, caller migration, no-Python and T010--T017 remain open | DONE for this bounded local recheck; parent T010/T011/T013/T016 remain PARTIAL/UNQUALIFIED | static review re-read Provider registration, request identity ownership, replacement state, callback lifetime and negative boundary; no regression found | integration target PASS 118/118 tasks; `git diff --check` PASS; `validate_design.py --json` PASS | three R4-B6 selectors `rc=0`; success marker and expected `DI_NATIVE_NO_ADMITTED_PROVIDER` negative observed; no MiniNDN, cross-process, no-Python or T016 run | system-first `-j2` after prior swap pressure; second post-run `vmstat` sample had `so=0`; no deployment artifact | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for local fixture; not `QUALIFICATION_PASS` | [R10-B68 evidence](evidence/r10-b68-r4b6-real-provider-recheck-20260909.md); next: run an independently launched native requester → Core → Provider worker/cross-process stream and observe both application/native request IDs |
| R10-B70 | production entry/callers: `DI_NativeRequester` configuration path and independent `di-native-provider` executable; implementation/wire: optional `request.application_request_id` → `NativeRequestOptions.applicationRequestId`, with native owner retaining Core request identity authority; test/harness/oracle: requester source/binding assertion, requester `--help`, provider usage boundary and candidate `ldd`; build/source closure: requester and provider targets rebuilt from current source with system-first `-j2`, explicit NAC-ABE/ndn-svs paths; migration/evidence: closes independent CLI identity composition while real cross-process worker, maintained caller/no-Python and T010--T017 remain open | DONE for this bounded CLI composition repair; parent T010/T013/T016 remain PARTIAL/UNQUALIFIED | static review covered optional JSON type/validation boundary, owner identity, source registration, loader closure and no planner fallback; no introduced control defect found | requester build PASS 13.915 s; provider build PASS 34.309 s; requester help, `ldd`, Python native binding 16/16, and `git diff --check` PASS | no network, MiniNDN, independent Provider process, no-Python or T016 run; post-build `vmstat` second sample showed swap-in, so next native build remains `-j2` | system-first `-j2`; explicit candidate library identity; no deployment artifact | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for CLI mapping; not `QUALIFICATION_PASS` | [R10-B70 evidence](evidence/r10-b70-native-requester-identity-cli-20260909.md); next: execute a fresh native requester → Core → independent Provider worker/cross-process run and observe both IDs |
| R10-B71 | production entry/callers: existing PO-001 native integration process and real R4-B6 Provider fixture; implementation/wire: current integration target and closure runner staging/observation; test/harness/oracle: fresh MiniNDN owner namespace, `PO-001-stream`, business marker `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`, and complete collector evidence; build/source closure: current integration target rechecked from source `7505fcad` with system-first `-j2`, transient manifest artifact hashes refreshed; migration/evidence: isolated owner/collector boundary passes, while independent `DI_NativeRequester`↔`di-native-provider` transport, two-turn cross-process, I02--I08, PO-002--PO-014, caller migration/no-Python and T016 remain open | DONE for bounded PO-001 stream owner recheck; parent T014/T016 remain PARTIAL/UNQUALIFIED | static review checked temporary hash refresh, owner/runner process binding, namespace/trace/marker collection and cleanup; no product or frozen-manifest mutation | owner command PASS; runner `evaluation.status=PASS`, `observation.complete=true`, no failures; integration target Waf recheck PASS 0.586 s | isolated MiniNDN owner process only; no independent Provider executable, no cross-process DI transport, no-Python or T016 qualification | system-first `-j2`; second `vmstat` sample `so=0`, swap-in remained observable; raw run retained outside Git | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for isolated PO-001 stream; not `QUALIFICATION_PASS` | [R10-B71 evidence](evidence/r10-b71-po001-stream-owner-pass-20260909.md); next: construct and run a fresh independent requester/Provider process case, then close remaining isolation and proof cases |
| R10-B72 | production entry/callers: `di-native-provider` CLI `--check-only` → native plan/manifest loaders; implementation/wire: service-name binding, role/artifact registration and execution-evidence construction; test/harness/oracle: four-role `spec174-exact-bundle-gpu-v5` metadata-only plan, `NDNSF_DI_NATIVE_PROVIDER_PLAN_READY` and `...CHECK_OK` markers; build/source closure: current provider executable and explicit candidate DI/framework/NAC-ABE/SVS loader paths; migration/evidence: corrected plan check passes while serve permission, authenticated request, independent requester/Provider transport, caller migration/no-Python and T010--T017 remain open | DONE for bounded Provider plan/manifest readiness check; parent T009/T010/T016 remain PARTIAL/UNQUALIFIED | static review covered service selection, metadata-only branch, role/artifact counts, evidence identity and loader closure; first wrong default service was classified as harness config boundary | corrected `--check-only` rc=0; 4 roles, 4 artifacts, 4 runners; execution evidence emitted; `git diff --check` pending final doc check | no network request, no independent requester/Provider transport, no-Python or T016 run | explicit service `/Inference/NativeTracer`; system-first native build identity; no product source change | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `CLOSED_FOR_VALIDATION` for plan check; not `QUALIFICATION_PASS` | [R10-B72 evidence](evidence/r10-b72-provider-plan-check-20260909.md); next: run an actual independent requester ↔ Provider serve/request case with a declared bundle and identity mapping |
| R10-B73 | production entry/callers: native runtime JSON loader → `NativeInferenceClient::request` → Core `BeginCollaboration` → real R4-B6 `ServiceProvider`; implementation/wire: operator-pinned `tokenizer_digest` is carried in `NativeRequestContract`, native generation DTOs are derived from authenticated options, and `DI_NativeRequester` now composes stream/generation options; test/harness/oracle: source-bound Qwen ONNX fixture, native catalog/runtime JSON, terminal stream marker and existing R10-B31/B33/B37 selectors; build/source closure: current integration target rebuilt with system-first `-j4`, fixture and binary hashes recorded; migration/evidence: bounded native-config Qwen real stream closes this local boundary while worker/cross-process, continuation/recovery, maintained caller migration/no-Python, legacy zero-use and T016 remain open | DONE for bounded native-config Qwen in-process real Provider stream; parent T010/T011/T013/T016 remain PARTIAL/UNQUALIFIED | static review covered exact runtime schema, tokenizer authority, source/semantic mapping, publication/grant binding, stream terminal ownership, fixture lifetime and selector registration; the first fixture digest mismatch was recorded and corrected before the final run | integration target build 118/118, 36.966s; Qwen selector 1/1 with 5 assertions; R10-B* unary/repository/stream sweep 4/4 with 15 assertions; `git diff --check` PASS | real in-process Core/Provider only; no independent requester/Provider worker, cross-process transport, no-Python or T016 qualification | system-first `-j4`; fixture SHA `d6f9b2f7e24931945ded23ba5519ac6b9367ecdeca7ee43f3e866a56e0996afd`; binary SHA `cc639cdef763bed05011af2ca3aa00e39fa7c2b9eb10e993bc3662b5bd884b6b`; no deployment artifact | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `CLOSED_FOR_VALIDATION` for native-config Qwen local stream; not `QUALIFICATION_PASS` | [R10-B73 evidence](evidence/r10-b73-native-config-qwen-real-provider-20260909.md); next: execute the independent requester ↔ Provider worker/cross-process path, then resume P3/P4 caller migrations |
| R10-B74 | production entry/callers: `NativeInferenceClient`/`APPClient` → maintained callers → `DI_NativeRequester`/`di-native-provider`; implementation/wire: runtime/envelope/provider host/CLI and worker lifetime; test/harness/oracle: CodeGraph trace, Python AST, Cppcheck, Waf target build, `ldd`/RUNPATH and task/evidence consistency; build/source closure: current `bc2180f9` checkpoint with system-first requester/Provider `-j4` build; migration/evidence: large audit found Provider serve failure hang, duplicate `--bootstrap-token`, stale compatibility manifest provenance, legacy default routes and host-bound binary closure; no task card advanced | `OPEN_FOR_NEXT_BATCH`; no runtime qualification claim | static audit found one high-impact failure-observability issue, one unreachable parser branch, and three migration/deployment blockers; Cppcheck lifetime/internal-AST reports were classified as false positive/noise where confirmed | Python AST 4307/0 syntax errors; Cppcheck 63 files/317 diagnostics; Waf requester+Provider PASS 24.188s; `ldd` no missing host libraries; `git diff --check` pending documentation checkpoint | no independent requester/Provider transport, continuation/recovery, maintained no-Python, I02–I08 or T016/T017 | system-first `-j4`; no implementation edits in this audit | `STATIC_PASS`; `BUILD_PASS`; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B74 evidence](evidence/r10-b74-large-static-audit-20260909.md); next: repair Provider failure exit and duplicate option, regenerate compatibility manifest, then run independent process transport |
| R10-B75 | production entry/callers: `di-native-provider --serve` → detached provisioning task → Face event loop/NDNSD scheduler; implementation/wire: duplicate `--bootstrap-token` branch removed, provisioning completion/failure state is explicit, failed Provider stops its io_context and cancels NDNSD heartbeat before returning rc=2; test/harness/oracle: fresh metadata-only standalone Provider failure probe plus `--check-only` marker and focused Cppcheck/parser assertions; build/source closure: `ServiceProvider` stop API and Provider/requester targets rebuilt from current source with system-first `-j2` after host swap evidence; migration/evidence: closes the R10-B74 failure-observability/parser defects only | DONE for this bounded failure-observability/CLI boundary; parent P1–P7, T010/T011/T013/T016 remain PARTIAL/UNQUALIFIED | static review found and repaired the original permanent event-loop hang, then found scheduler reactivation during the first repair and moved cancellation to the stopped main-thread boundary; no request/response qualification claim | first API build failed on invalid `ScopedEventId::reset()` use; corrected `-j2` requester+Provider build PASS 1m20.055s and final Provider relink PASS 19.583s; `cppcheck` changed source PASS; `git diff --check` PASS | failure probe PASS rc=2 elapsed 1.08s with `SERVE_READY` and `PROVISION_FAILED`; check-only PASS 4 roles/4 artifacts/4 registered; no independent requester/Provider transport, continuation/recovery, maintained no-Python, I02–I08 or T016/T017 | system-first `-j2` after `vmstat` showed sustained swap-in/out; no deployment artifact | `STATIC_PASS`; `BUILD_PASS`; `FOCUSED_BEHAVIOR_PASS`; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B75 evidence](evidence/r10-b75-provider-failure-exit-20260909.md); next: execute a fresh independent requester → Core → Provider worker/cross-process request and observe terminal failure/success boundaries |
| R10-B76 | production entry/callers: compatibility manifest → maintained Python API inventory; implementation/wire: `checklists/build_api_migration_manifest.py` regenerated all 344 entries and rebounded source line/hash metadata to checkpoint `31fe172a`; test/harness/oracle: generator output, `sourceCommit` equality, design validator and diff check; build/source closure: manifest-only, no ABI or runtime change; migration/evidence: closes stale provenance from R10-B74 while semantic caller mapping, native default migration and legacy retirement remain open | DONE for this bounded manifest provenance boundary; parent T001/O-004/T013-B remain PARTIAL | static review confirms current HEAD identity and 344-entry coverage; manifest remains routing evidence and cannot promote runtime compatibility or qualification | generator PASS (`entries=344`, `dynamicAppSdk=67`); sourceCommit equality PASS; `validate_design.py --json` PASS; `git diff --check` PASS | no product build, requester/Provider transport, maintained caller/no-Python or T016/T017 run | documentation/generator artifact only | `STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `OPEN_FOR_NEXT_BATCH`; not `QUALIFICATION_PASS` | [R10-B76 evidence](evidence/r10-b76-compatibility-manifest-refresh-20260909.md); next: map and migrate maintained native callers, then regenerate after each source checkpoint |

## Current Checkpoint

2026-09-10 R11-B7 native cleanup / **CLOSED_FOR_VALIDATION**（仅限当前 C++ lifecycle 与
bounded process drain）：`Spec182NativeInferenceClient` 2、request identity 1、registration
6、shared lease 3、provider host 7、stream acceptance 7 个 selector 均为 `*** No errors
detected`。覆盖终态、cancel、deadline、late callback、registration/lease drain、close/re-serve、
shared service isolation、replacement fencing 和 safe secret-owner cleanup；R11-B6 四类
process 结束后 native role scan 无残留进程。详见 [R11-B7 evidence](evidence/r11-b7-native-cleanup-20260910.md)。
T016 isolation/PO matrix、maintained callers、no-Python 和完整 qualification 仍未关闭。

2026-09-10 R11-B6 native replacement / **CLOSED_FOR_VALIDATION**（仅限独立 C++ 双 Provider
replacement 出口）：Requester A 先完成认证 ACK 后被停止，Requester/Core 通过真实
replacement 规划切换到独立 Provider B；B 以 `attempt-2` 完成 grant verification、CPU
ONNX execution 和 C++ stream oracle `[4,5,6,7,8,9,10,2]`，A 没有 execution evidence。
无备用 Provider 的负例以单一 `NATIVE_REQUEST_STAGE_FAILED` / `DI_NATIVE_NO_ADMITTED_PROVIDER`
终态退出。`Spec182StreamAcceptance` 7/7 与 C++ integration 9/9 通过；广泛
`Spec182*` selector 的 6 个 runner preparation callback fixture failure 已登记在
[R11-B6 evidence](evidence/r11-b6-native-replacement-20260910.md) 和 failure log。父
T010/T011、R11-B7--B9、maintained callers、no-Python 及完整 qualification 仍未关闭。

2026-09-10 R11-B3 native stream process / **CLOSED_FOR_VALIDATION**（仅限独立
process 的 C++ streaming 出口）：`DI_NativeRequester`、Core、`di-native-provider` 经私有
NFD/Controller 完成真实授权、Selection、Provider 装配和 ORT CPU 执行；C++ requester 收到
有序 token `[4,5,6,7,8,9,10,2]` 的 8 个事件并通过最终结果 oracle，Provider 同时输出 grant
verification 与真实 execution evidence。准备 runner、decode-state commit、事件 digest 和
状态序号接线均由 C++ 生产代码承担；Python 驱动只负责私有进程、身份、配置和生命周期。
详见 [R11-B3 evidence](evidence/r11-b3-native-stream-process-20260910.md)。gap/timeout/
duplicate/wrong-generation 负例、continuation/recovery/replacement/cleanup、maintained
callers、no-Python、T010--T017 仍未关闭。

2026-09-10 R11-B4 native continuation / **CLOSED_FOR_VALIDATION**（仅限独立
C++ 双轮 process 出口）：第一轮 `FULL_CONTEXT` 完成 stream `1..8`/`9`、真实
COMMIT/FINALIZE、持久 checkpoint、C++ stream oracle 和 success markers；第二轮使用新的
generation identity 完成 `APPEND_DELTA`，错误 parent 进程以非零状态在 conversation begin
边界拒绝。publisher stable artifact identity 已加入 canonical manifest digest，避免跨请求
复用 request-scoped canonical root。构建、C++ unit/integration selector 和 raw run 见
[R11-B4 evidence](evidence/r11-b4-native-continuation-20260910.md)。父 T010/T011、R11-B5
recovery、maintained callers、no-Python 及完整 qualification 仍未关闭。

2026-09-10 R11-B5 native recovery / **CLOSED_FOR_VALIDATION**（仅限 Provider restart
safe-rejection）：第一轮 `FULL_CONTEXT` 成功写入 requester journal checkpoint 后，driver 对
Provider 执行 SIGKILL 并以同一配置重启；重启 Provider 报 `PROVIDER_CONVERSATION_STATE_MISSING`，
第二轮 `APPEND_DELTA` 以 `NATIVE_STREAM_FAILED` 明确拒绝，且重启日志没有
`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` 或 `STREAM_EVENT_OBSERVED`。详见
[R11-B5 evidence](evidence/r11-b5-native-recovery-20260910.md)。该卡不证明 Provider KV
跨重启恢复；R11-B6--B9、maintained callers、no-Python 和完整 qualification 仍未关闭。

2026-09-10 R11-B2 native unary process / **CLOSED_FOR_VALIDATION**（仅限本地
process 出口）：独立 Controller、artifact authority、DI_NativeRequester 和 di-native-provider
经私有 NFD/PIB/TPM 完成真实 `/Inference/NativeUnary`；同一 requestId
`/NDNSF/DI/REQUEST/15cb4c52e8fe435592879d4aae68fd1f-1`、`attempt-1` 和 plan digest
`sha256:5a6bdc0145ffd3f1db93b3d6c8d435fc0b550d0215067a7fb0117d5db3bd6628` 贯穿 ACK、Selection、
grant verification、Provider handler、ONNX Runtime CPU execution 和最终 Response；C++ oracle
验证 `predictions=[4,0,12]`。缺少 Provider `FullModel` role 的同链拒绝例返回
`NATIVE_PROVIDER_FAILED`，没有成功或 oracle 标记。详见
[R11-B2 evidence](evidence/r11-b2-native-unary-process-20260910.md)。T010 父任务、R11-B3--B9
和完整 qualification 仍未关闭。

2026-09-10 R11-B1 independent artifact authority / **CLOSED_FOR_VALIDATION**（仅限本地
process 出口）：C++ requester 只读取自身签名私钥与 authority 公钥，通过既有 Core
`RequestServiceTargeted` 请求 `ndnsf-di-native-grant-authority-v1`；authority 独立持有签发
私钥、model content key、recipient registry 与 immutable publication policy，并在 Controller
ProviderPermission 就绪后提供 TargetedOnly service。真实独立 Controller/Authority/requester
进程经私有 NFD 通过 1 个正例、5 个 handler 拒绝例和 1 个 authority 不可达超时；C++ probe
验证 grant，bwrap 隔离与角色 PIB/TPM 快照通过。完整 `Spec182*` C++ unit 选择器为 256/256，
`Spec170NdnsfDiCoreFlow/Spec182*` integration 选择器为 9/9。T005 父任务仍为 `PARTIAL`；
R11-B2 已在后续批次完成本地 ACK/Selection/handler/Response 出口，完整 Spec qualification
仍未关闭；详见
[R11-B1 evidence](evidence/r11-b1-independent-authority-20260910.md)。

2026-09-10 R10-B84 native request identity scope / **CLOSED_FOR_VALIDATION**（仅限 C++ request identity 边界）：
`NativeInferenceClient` 的每个生产 client 构造路径生成一个新的 owner scope，request URI 为
`/NDNSF/DI/REQUEST/<32-hex-owner-scope>-<counter>`，由原生 C++ owner 在请求创建时分配；私有测试
port 保留确定性的 counter-only 名称以维持既有状态机断言。每次构造生成 scope 也避免 `fork()` 子进程
继承父进程的缓存 scope。
新增 `Spec182NativeRequestIdentity`，并重跑完整 `Spec182*` C++ unit selector 与 9 个
`Spec170NdnsfDiCoreFlow/Spec182*` C++ integration cases，均无错误；期间发现并修复多级 Name
组件导致的原生 Core/Provider 接线回归。本批不使用 Python 测试证明 native 行为。跨进程 requester/Provider worker、authority 分离、
16 个 maintained caller、no-Python、依赖闭包和 T016/T017 仍开放，未推进任何父任务。详见
[R10-B84 evidence](evidence/r10-b84-native-request-id-scope-20260910.md)。下一步仍是执行带有
artifact identity/source closure 的独立 requester/Provider process case。

2026-09-10 R10-B83 native conversation config loader / **CLOSED_FOR_VALIDATION**（仅限该接线边界）：
将 `ndnsf-di-native-conversation-v1` 解析、路径与 owner-only key 校验集中到
`NativeConversationCoordinator.cpp`；standalone `DI_NativeRequester` 与 Python binding 共享该
loader，并将 coordinator 注入 runtime client。新的 C++ config selector、完整 `Spec182*` 252
unit cases、9 个 native Core/Provider integration cases、四个 Python focused suites 72/72、
requester help、导出符号检查及三项文档门均通过。
首次 Python 扩展重编译的 `ndn::Name`/`std::string` 类型错误已记录并修复；主机 swap 压力下后续
仍采用 `-j2`。本轮没有推进父任务；artifact authority 分离、独立 requester/Provider worker
跨进程整链、16 个 maintained caller、no-Python、依赖闭包与 T016/T017 仍开放。详见
[R10-B83 evidence](evidence/r10-b83-native-conversation-config-20260910.md)。下一步仍是先明确
authority 边界，再构造带 artifact identity/source closure 的独立 requester/Provider process case。

2026-09-10 R10-B82 whole-chain static audit / **OPEN_FOR_NEXT_BATCH**：
大范围只读审查覆盖 standalone C++ requester、Core/Provider 接线、conversation owner、grant
authority、维护中的 Python caller、请求身份、Waf 注册和 `ldd`/RUNPATH 闭包。确认一个直接的
契约缺口：`DI_NativeRequester.cpp` 没有读取文档化的 `conversation` 配置，也没有注入 native
coordinator；同时 requester 仍读取 artifact-authority 私钥并本地调用 issuer，16 个维护 caller
仍使用 compatibility/automatic-planner API，独立 requester/Provider worker 跨进程整链、no-Python
和部署依赖闭包仍未验收。Cppcheck/既有 Clang 静态结果没有新增确定性算法缺陷；没有推进任何父任务。
详见 [R10-B82 evidence](evidence/r10-b82-whole-chain-static-audit-20260910.md)。下一批先明确
conversation/authority 边界，再构造一个带 artifact identity/source closure 的独立 requester/Provider
process case。

2026-09-10 R10-B81 large C++ static audit / **OPEN_FOR_NEXT_BATCH**：
针对生产入口、C++ 调用链、维护中的 Python caller、Provider/Requester 示例、构建注册和
部署依赖做了一次大范围只读审查。审查发现并修复 `NativeInferenceClient` 取消路径中的
冗余清理条件，并将 `NativeEpochCoordinator` 的范围构造改为显式形式以消除静态工具歧义；
integration target 118/118、unit target 188/188、完整 `Spec182*` 251 cases 及相关
focused selectors 均通过。维护 caller 仍有 16 个实际 inference API 调用未迁移（一个
`.generate` 匹配属于 evidence signer），独立 requester/Provider worker、跨进程两轮会话、
no-Python、主机/容器依赖闭包和 T016/T017 仍未完成；没有推进父任务状态。详见
[R10-B81 evidence](evidence/r10-b81-large-static-audit-20260910.md)。下一批只构造一个带
artifact identity/source closure 的独立 requester/Provider process case。

2026-09-09 R10-B79 large static audit / **OPEN_FOR_NEXT_BATCH**：
完成生产调用链、C++契约、维护中的 Python caller、资格 runner/manifest、构建注册和部署闭包的只读审计。
发现 requester CLI help 路径说明与契约不一致、generation/stream budget 上限互相冲突、Provider
`--run-for-ms` 不能约束 permission-install join、维护 caller 仍保留旧 API，以及 artifact 仍绑定主机依赖路径；
其中 CLI 文本和 run-limit fence 已修复，帮助探针与 controller-assisted finite serve probe 均通过。后续复核确认 registration manifest
与 transient runner manifest 是有意分层，直接把前者交给 runner 属于错误命令。该批没有推进 P1--P7 或任何父任务；静态检查和目标构建
通过不等于独立 transport、两轮会话、no-Python 或 T016/T017 qualification。详见
[R10-B79 evidence](evidence/r10-b79-large-static-audit-20260909.md)。

2026-09-10 R10-B80 native-config Qwen tokenizer authority / **CLOSED_FOR_VALIDATION (local boundary)**：
将 conversation commit 中错误的 semantics-digest fallback 移除；runtime contract 的
tokenizer_digest 现在是唯一 authority，缺失或与 derived generation 不一致即 fail-closed。
新增 native-config Qwen real Provider conversation selector，检查两轮结果后的 transcript
tokenizer digest，并与已有 stream selector 一起通过；integration-tests 118/118、R10-B*
五例 26 assertions、Python closure 41 tests 均通过。该批仍是 in-process Core/Provider，
独立 requester/Provider worker、caller migration、T015/T016/T017 未完成。详见
[R10-B80 evidence](evidence/r10-b80-native-config-qwen-conversation-20260910.md)。

2026-09-10 R10-B77 Provider relocatable RUNPATH / **CLOSED_FOR_VALIDATION (target boundary)**：
Provider target 已补 `$ORIGIN/..` staged-library lookup；90/90 Waf target build 和 Python
target-regression test 通过。该批没有启动 Provider 或网络请求，且当前 artifact 仍依赖主机
NAC-ABE/SVS/NDNSD 路径；完整 source/deployment closure 与 P1 独立 requester/provider
transport 仍开放。详见 [R10-B77 evidence](evidence/r10-b77-provider-runpath-20260910.md)。

2026-09-09 R10-B76 compatibility manifest refresh / **OPEN_FOR_NEXT_BATCH**：
已按当前 `31fe172a` checkpoint 重新生成 344 项清单，`sourceCommit` 与 `git rev-parse HEAD`
一致；设计 validator 和 diff 检查通过。该清单只修复 provenance，不等价于 caller/native
语义闭环或 legacy retirement。详见 [R10-B76 evidence](evidence/r10-b76-compatibility-manifest-refresh-20260909.md)。

2026-09-09 R10-B75 Provider failure-exit and CLI parser repair / **OPEN_FOR_NEXT_BATCH**：
修复并验证了 R10-B74 静态审计指出的 Provider 失败永久存活和重复 `--bootstrap-token` 分支。
失败探针现在在 `SERVE_READY` 后观察到权限失败并以 rc=2 退出；`--check-only` 也通过。期间
首个 API 编译尝试因误用 `ScopedEventId::reset()` 在编译边界失败，按资源规则改为 move
assignment 并以 `-j2` 重试通过。详见 [R10-B75 evidence](evidence/r10-b75-provider-failure-exit-20260909.md)。
独立 requester → Core → Provider、continuation/recovery、maintained no-Python、I02–I08 和
T016/T017 仍未验收。

2026-09-09 R10-B74 large static audit / **OPEN_FOR_NEXT_BATCH**：
对生产 C++、维护中的 Python 调用方、构建注册、运行时契约和证据 provenance 做了只读大范围
审计。Python AST 4307/0 syntax errors，Cppcheck 检查 63 个生产 `.cpp` 文件并返回 317
条诊断（主要是风格项），requester/Provider 目标用 system-first `-j4` 构建通过。审计发现
Provider serve 失败后永久事件循环、重复 `--bootstrap-token`、compatibility manifest
落后于当前 `bc2180f9`、维护调用方仍默认旧路径以及 host-bound 依赖闭包；这些结果没有推进
任何任务卡。详见 [R10-B74 evidence](evidence/r10-b74-large-static-audit-20260909.md)。

2026-09-09 R10-B73 native-config Qwen real Provider stream / **CLOSED_FOR_VALIDATION (local in-process boundary)**：
固定 `runtime.contract.tokenizer_digest` 为 native runtime contract 的 operator-pinned
来源；`NativeInferenceClient` 从已认证 options 派生 generation DTO，并在 digest 不一致时
拒绝请求，`DI_NativeRequester` 同步补齐 stream/generation DTO 组合。新增 source-bound Qwen
ONNX fixture 和 native catalog/runtime JSON 真实 selector，经过 Core `BeginCollaboration`
及 R4-B6 `ServiceProvider` 返回两个 token 与 terminal final；Qwen selector 1 case/5
assertions，R10-B* unary/repository/stream sweep 4 cases/15 assertions，integration target
以 system-first `-j4` 118/118 构建通过。首轮 one-role fixture digest 错配在 graph validation
边界失败，已记录并修正后重跑。该结果只闭合 native-config Qwen 单进程真实流，不证明独立
worker/cross-process、continuation/recovery、maintained caller/no-Python、legacy zero-use
或 T016/T017。详见 [R10-B73 evidence](evidence/r10-b73-native-config-qwen-real-provider-20260909.md)。

2026-09-09 R10-B72 Provider plan check / **CLOSED_FOR_VALIDATION (metadata readiness boundary)**：
复用四角色 bundle 首轮因默认 service 与 plan 不符在解析边界 `rc=2`；显式传入
`/Inference/NativeTracer` 后 `--check-only` 返回 `rc=0`，输出 `PLAN_READY`、四角色/四
artifacts/四 runners 和 `CHECK_OK` execution evidence。该结果只证明 plan/manifest metadata
readiness，不证明 Provider serve permission、独立 requester/Provider transport、会话、
caller migration/no-Python 或 T010--T017。详见
[R10-B72 evidence](evidence/r10-b72-provider-plan-check-20260909.md)。

2026-09-09 R10-B71 PO-001 stream owner recheck / **CLOSED_FOR_VALIDATION (isolated owner boundary)**：
使用新的临时 artifact hashes 在 MiniNDN owner namespace 中启动 PO-001 stream，collector
返回 `evaluation.status=PASS`、`observation.complete=true`、无 failures，业务 marker
`SPEC182_NATIVE_DI_REQUEST_RESULT_OK` 存在。当前 integration target 在 `7505fcad` 上以
system-first `-j2` 重查通过。该结果只证明隔离 native integration process 与已有
in-process real Provider fixture 及 collector 完整性，不证明独立 `DI_NativeRequester`/
`di-native-provider` 跨进程传输、两轮会话、I02--I08、PO-002--PO-014、no-Python 或 T016。
详见 [R10-B71 evidence](evidence/r10-b71-po001-stream-owner-pass-20260909.md)。

2026-09-09 R10-B70 native requester identity configuration / **CLOSED_FOR_VALIDATION (CLI composition boundary)**：
独立 `DI_NativeRequester` 现在读取可选 `request.application_request_id` 并传入
`NativeRequestOptions.applicationRequestId`；权威 Core request ID 仍只由 native owner 分配。
requester target（13.915 秒）和 provider target（34.309 秒）均以 system-first `-j2` 构建通过，
requester help、候选 `ldd`、native binding 16/16 与 diff check 通过。该批次只闭合 CLI
身份映射组合，不宣称独立 Provider worker/跨进程、两轮会话、maintained caller/no-Python
或 T010--T017 资格。详见 [R10-B70 evidence](evidence/r10-b70-native-requester-identity-cli-20260909.md)。

2026-09-09 R10-B68 R4-B6 real Provider recheck / **CLOSED_FOR_VALIDATION (local fixture boundary)**：
在 `606230fbe783217e140499bc7ac4e9a3e65f72b0` 上以 system-first `-j2` 重建
`integration-tests`（118/118 tasks），并运行成功会话、预期首轮拒绝 replacement、alternate
replacement 三个 R4-B6 selector，均 `rc=0`。成功用例输出
`SPEC182_NATIVE_DI_REQUEST_RESULT_OK`；负例保留并断言
`NATIVE_REQUEST_STAGE_FAILED / DI_NATIVE_NO_ADMITTED_PROVIDER`。本批次确认 R10-B66/B67
修复没有让本进程 real-Provider 接线回退，但不证明独立 Provider worker、跨进程传输、
maintained caller/no-Python 或 T010--T017 资格。详见
[R10-B68 evidence](evidence/r10-b68-r4b6-real-provider-recheck-20260909.md)。

2026-09-09 R10-B67 native/application request identity mapping / **PARTIAL (local mapping boundary)**：
native owner 继续唯一分配 Core `requestId`；维护 caller 的 `wire_request_id` 通过
`NativeRequestOptions.applicationRequestId` 显式关联，pybind handle 同时暴露两层 ID，
Qwen native helper 转发并 fail-closed 校验映射。`unit-tests` `-j4` 56.496 秒、共享
`ndnsf-distributed-inference` `-j2` 43.736 秒、pybind 扩展重建及候选 `ldd`/`nm` 检查
通过；C++ 映射测试、native binding 16/16、APP SDK compatibility 18/18 通过。初始
NAC-ABE 与 stale shared-library loader 失败已按首边界记录并修复。真实 Qwen/YOLO
worker/cross-process、双轮会话、caller migration、legacy zero-use、I02--I08 和 T016
仍未完成。详见 [R10-B67 evidence](evidence/r10-b67-native-application-request-mapping-20260909.md)。

2026-09-09 R10-B66 native contract and Provider host repair / **PARTIAL (local repair boundary)**：
按 R10-B65 静态审计结果修复了 direct C++ generation-mode bypass、长期 client 的
过期 operation 弱引用累积，以及 Provider 首次 `serve` 失败仍发布半初始化 host 的问题。
JSON parser、envelope encoder 和 direct constructor 现在共享同一 `TOKEN_DIAGNOSTIC` /
`TOKEN_STREAMING` predicate；host 只有在 runtime、observer、Core registration 和
target 安装全部成功后才发布，首次失败会关闭 fixed lease 并允许重试。`unit-tests`
使用 system-first `-j4` 构建 59.180 秒完成；`Spec182ClientState` 18/18、
`Spec182ProviderHost` 7/7、cppcheck 与 diff check 通过。真实 Qwen/YOLO requester→Core→
Provider worker/cross-process、request identity mapping、caller migration、legacy
zero-use、I02--I08 和 T016 仍未完成。详见 [R10-B66 evidence](evidence/r10-b66-native-contract-host-repair-20260909.md)。

2026-09-09 R10-B65 large-scale static audit / **PARTIAL (static audit boundary)**：
以 `3608f204` 为基线复核 requester/Core/Provider、maintained callers、pybind、Waf
source closure 与资格证据。626 个 Python 文件 AST parse 无语法错误；CodeGraph 当前
索引；cppcheck 的 iterator lifetime 报告经源码核对为误报，另两项为维护性提示。审计
新增 F-02 direct C++ generation-mode bypass、F-03 weak operation retention、F-04 Provider
host 首次 serve 失败回滚缺口，并确认默认 planner 路径、request identity mapping、
真实 worker/cross-process/no-Python、legacy zero-use、T013--T017 仍未关闭。`validate_design.py`
与 skill synchronization PASS；本轮无源码、skill、构建或网络运行。详见
[R10-B65 evidence](evidence/r10-b65-large-scale-static-audit-20260909.md)。

2026-09-09 R10-B64 native-config generation contract / **PARTIAL (local contract boundary)**：
Python 与 C++ runtime parser 现在只接受 `TOKEN_DIAGNOSTIC`/`TOKEN_STREAMING`；Qwen native
catalog 必须显式使用 `TOKEN_STREAMING`，Qwen native route 与
`--diagnostic-token-loop` 组合会 fail-closed。malformed observer mapping 有回归覆盖。
native binding suite 16/16、相关 C++ planner/client/state 49 cases 与 `unit-tests` `-j4`
构建通过。真实 Provider/跨进程请求、request-id 映射、conversation owner、maintained
caller migration、legacy zero-use 和 T016 仍未完成。详见 [R10-B64 evidence](evidence/r10-b64-native-config-contract-20260909.md)。

2026-09-09 R10-B63 large-scale static audit / **PARTIAL (static audit boundary)**：本轮按五 lane 审查当前 requester/Core/Provider、maintained callers、pybind、Waf source closure 与资格证据。636 个维护 Python 文件 AST parse 无语法错误；cppcheck 的 parser/iterator 报告经源码核对未形成新产品缺陷。审计发现默认 Python planner 仍可达、Qwen diagnostic/stream 配置冲突、observer mapping fail-open、diagnostic token 字段不匹配及弱 operation 表增长；native/application request identity mapping 仍缺显式契约。既有真实 native Core/Provider、PO-001、I01 与 249 C++ cases 保留，但 cross-process、conversation owner、maintained caller/no-Python、I02–I08/PO matrix 和 T016 仍未完成。详见 [R10-B63 evidence](evidence/r10-b63-large-scale-static-audit-20260909.md)。

2026-09-09 R10-B61 native-config Qwen full-generation caller / **PARTIAL (caller execution repair)**：
维护中的 native-config full-generation 分支现在使用 `decode_payload`，校验
`NDNSF-DI-FINAL-V1`/`tokenIds`，并向完整生成校验器提供 `generatedTokenIds`；真实生产
helper、native DTO、observer/terminal 和 tokenizer digest 断言通过（20/20，legacy exclusion
8/8）。真实 requester → Core → Provider、跨进程 stream、conversation owner、YOLO migration
和 T016 仍未完成。详见 [R10-B61 evidence](evidence/r10-b61-native-qwen-full-caller-20260909.md)。

2026-09-09 R10-B60 native-config Qwen launcher contract / **PARTIAL (接线修复)**：维护入口现在
在提供 native requester config 时选择 `qwen-onnx-cpu-native`，将配置转发给 User，并排除
automatic-planning 参数冲突；wrapper/runner/parser focused checks 与相关 Python 回归 27/27
通过。真实 native Provider、跨进程 stream、conversation owner、legacy retirement 和 T016
仍未完成。详见 [R10-B60 evidence](evidence/r10-b60-native-qwen-launcher-contract-20260909.md)。

2026-09-09 R10-B59 native-config Qwen tokenizer digest / **PARTIAL (contract repair)**：已将
`request.tokenizer_digest` 固定为 `TOKEN_STREAMING` 的 operator-pinned 配置字段；
`APPClient` 校验并保存该摘要，Qwen native helper 只读取已绑定值，缺失时 fail-closed。
真实 `_ndnsf` pybind DTO helper 测试与 legacy/native Python 回归通过（13/13、21/21）。
真实 Provider/cross-process streaming、conversation owner、完整 caller migration 和 T016
仍未完成。详见 [R10-B59 evidence](evidence/r10-b59-qwen-tokenizer-digest-20260909.md)。

2026-09-09 R10-B58 shared async fixture lifetime rule / **DONE (workflow boundary)**：已将
detached native worker 的 Face/io_context/scheduler/timer/callback owner 或 join/drain barrier、
析构顺序静态检查和重复 selector 要求同步到 versioned shared skill、batch/review references、
三个 Spec 模板以及 AGENTS/CLAUDE/project workflow 文档。同步检查与设计 validator 通过；
不改变产品实现或 T016 状态。详见 [R10-B58 evidence](evidence/skill-async-fixture-lifetime-20260909.md)。

2026-09-09 R10-B57 native V3 placement lifetime / **DONE (focused fixture boundary)**：C++
fixture 改为由 `User` 持有 heap `DummyClientFace`，修复 detached `ServiceUser` 释放与 Face
reactor/timer 析构竞态。canonical `build-nac182` 下 placement selector 连续 10 次通过，
Spec182 unit 为 249/249 cases、6781/6781 assertions，`Spec170NdnsfDiCoreFlow` 为 55/55
cases、978/978 assertions。cross-process ownership、maintained caller/no-Python 与 T016
仍未完成。详见 [R10-B57 evidence](evidence/r10-b57-native-v3-placement-lifetime-20260909.md)。

2026-09-09 R10-B55 Provider serve preflight / **DONE (focused serve/readiness boundary)**：
metadata-only manifest 下的 repaired `di-native-provider --serve` 已真实创建 Face、
ServiceProvider 和 NativeInferenceProvider host，打印 `PLAN_READY`、
`EXECUTION_LEASE_SERVICE_READY` 与 `SERVE_READY`；随后 Controller permission 未安装，
readiness 以 `PROVISION_FAILED` 暴露。Controller-assisted retry 安装临时
`/SERVICE/Inference/NativeTracer` policy 后进一步打印 `PERMISSION_READY`、
`PROVISION_READY` 和 `NDNSF_DI_NATIVE_PROVIDER_READY`；Provider bounded timeout 返回
`124`（常驻事件循环），Controller exit `0`。首个漏传 `--serve` 的 CLI 边界也已保留；
没有把 timeout 或 serve marker 写成 terminal request/result、maintained caller/no-Python
或 T016 qualification。详见 [R10-B55 evidence](evidence/r10-b55-provider-serve-preflight-20260909.md)。

2026-09-09 R10-B54 native plan/manifest smoke / **DONE (focused native behavior boundary)**：
首次 `di-native-plan-manifest-smoke` 链接在 55/55 tasks 后暴露 ONNX/framework source/link
closure 漏项；保留原始 linker 边界，并在 `examples/wscript` 补齐
`di_native_onnx_assembly_sources`、candidate framework/ONNX/Protobuf closure 和
`$ORIGIN/..` RUNPATH。按新增的 project-symbol definition map、导出符号和默认 `ldd`
门禁复审后，retry 以 `-j2` 完成 85/85 tasks（`170.09s`），bundle 根目录无环境注入 smoke
输出 `NDNSF_DI_NATIVE_PLAN_MANIFEST_SMOKE_OK roles=4 artifacts=4 outputTensors=8`，exit
`0`（`0.06s`）。这只关闭本地 plan/manifest、角色注册和输出 accounting；Provider
`--serve`、独立 requester/Provider transport、maintained caller/no-Python 和 T016 仍开放；
详见 [R10-B54 evidence](evidence/r10-b54-plan-manifest-smoke-20260909.md)。

2026-09-09 R10-B53 native plan/ONNX smoke / **DONE (focused native behavior boundary)**：
`di-native-plan-onnx-smoke` 以 system-first `-j2` 完成 86/86 tasks（`153.14s`）。首次运行
保留 `/usr/local` framework loader 缺符号 `rc=127`，候选库重试又保留 bundle-relative
artifact cwd `rc=2`；随后为 target 增加 `$ORIGIN/..` RUNPATH（checkpoint `085359eb`），
并保留一次 `$PWD` 命令构造 `rc=127` 后，最终从 bundle 根目录、无 `LD_LIBRARY_PATH` 重试，
四角色 ONNX session 成功执行，发布 4 个 dependency objects，输出 440 bytes，marker
`NDNSF_DI_NATIVE_PLAN_ONNX_SMOKE_OK`，exit `0`（`0.06s`）。该结果只关闭本地 plan/session
行为及库身份边界，不关闭 Provider `--serve`、独立 requester/Provider transport、
maintained caller/no-Python 或 T016；详见 [R10-B53 evidence](evidence/r10-b53-plan-onnx-smoke-20260909.md)。

2026-09-09 R10-B52 native requester CLI build / **DONE (focused CLI boundary)**：`DI_NativeRequester`
Waf target 在现有 Spec182 build tree 以 `-j2` 增量检查 exit `0`（`1.03s`）；当前 binary
的 help、裸调用、错误 schema 分别返回 `0/2/1`，错误配置在启动 Face 前 fail-closed，
没有写 output。target SHA、`ldd` closure 和 raw logs 已保存。该结果只关闭 CLI/build
边界；合法 requester 配置、Core/Provider transport、terminal Response、maintained
caller/no-Python、conversation 与 T016 仍保持 PARTIAL/UNQUALIFIED。详见 [R10-B52 evidence](evidence/r10-b52-requester-cli-build-20260909.md)。

2026-09-09 R10-B51 Provider check-only with real ONNX bundle / **DONE (focused behavior boundary)**：
复用 R10-B50 修复后的 standalone Provider binary，在已有四角色 `DATA_DRIVEN_V2` bundle 上
执行 `--check-only`。四个 ONNX Runtime CPU role 全部加载并预热，输出完整 execution
evidence，`NDNSF_DI_NATIVE_PROVIDER_CHECK_OK`，进程 exit `0`，耗时 `0.05s`；raw log、
binary SHA 和相对路径工作目录均已保存。该结果只关闭 Provider load/warmup/registration
边界，不启动 `--serve`、独立 requester/Provider transport 或终端 Response；T010-B、
maintained caller/no-Python 与 T016 仍保持 PARTIAL/UNQUALIFIED。详见 [R10-B51 evidence](evidence/r10-b51-provider-check-only-20260909.md)。

2026-09-09 R10-B50 standalone Provider link closure / **DONE (build boundary)**：首次构建暴露
`examples/wscript` source closure 缺失，三次 `ld` 首边界均已保留；补齐缺失 native translation
units 后，同一 `-j2` 构建 Provider 80/80、fault-provider 91/91，均 exit `0`；产物及
`ldd` closure 已核对。该结果只关闭独立 Provider/fault-provider 的编译/链接边界，尚未运行 `--serve`、
独立 requester/Provider transport 或 T016。详见 [R10-B50 evidence](evidence/r10-b50-provider-link-closure-20260909.md)。

2026-09-09 R10-B49 Spec182 C++ unit suite / **DONE (focused behavior boundary)**：复用现有
`unit-tests` 二进制运行精确 `Spec182*` selector，249/249 cases 退出 0，报告
`*** No errors detected`，耗时 `27.72s`。本批无源码变化和重建，已记录二进制 SHA 与源提交。
该结果关闭当前 C++ unit contract 回归边界，但不证明独立 requester/Provider transport、
maintained caller/no-Python 或完整 T016；运行后的 `vmstat` 出现明显 swap-in，下一次 native
build 按资源策略使用 `-j2`。详见 [R10-B49 evidence](evidence/r10-b49-spec182-cpp-unit-suite-20260909.md)。

2026-09-09 R10-B48 T016 I01 installed consumer / **DONE (focused qualification boundary)**：root-enabled
MiniNDN owner 在真实 requester namespace 中运行 I01 canonical runner，staged native consumer
`rc=0`、`344ms`，输出 `SPEC182_INSTALLED_CONSUMER_NATIVE_DI_OK`；七类 evidence 齐全且
collector violations 为空。该结果只关闭 I01 owner/runner 观察，DI protocol、I02-I08、
PO-002-PO-014、独立 requester/Provider transport、maintained caller/no-Python 与完整 T016
仍未完成。详见 [R10-B48 evidence](evidence/r10-b48-t016-i01-consumer-owner-pass-20260909.md)。

2026-09-09 R10-B47 T016 PO-001 owner/runner / **DONE (focused qualification boundary)**：root-enabled
MiniNDN owner 现已在真实 requester namespace 中运行 canonical runner，PO-001 staged native
process 在 `8001ms` 内返回 `0`，`SPEC182_NATIVE_DI_REQUEST_RESULT_OK` business marker、
identity/process-tree/namespace/exec-map/endpoints/business-oracle/cleanup 七类证据齐全，
trace integrity 和 policy violations 均为空。此前 `/usr/local/bin/infoconv` 缺失、历史 role
名称非法和 stale executable digest 三个首边界均保留在新 raw run 中并 fail closed。该结果只
关闭 PO-001 focused qualification；I02-I08、PO-002-PO-014、独立 requester/Provider transport、
maintained caller/no-Python 与完整 T016 仍未完成。详见
[R10-B47 evidence](evidence/r10-b47-t016-po001-owner-pass-20260909.md)。

2026-09-09 R10-B46 real Provider native suite / **DONE (local selector regression boundary)**：现有
`NativeInferenceClient` 到 R4-B6 真实 `ServiceProvider` fixture 的七个 Spec182 selector 全部
通过，包括 unary inline、unary `REPO_REF`、stream、conversation、replacement 和 alternate
replacement；单 Provider replacement 的 `DI_NATIVE_NO_ADMITTED_PROVIDER` 仍按预期保留。该
批次没有改源码，只做既有 target 的增量构建与回归执行；独立 requester/Provider transport、
maintained caller/no-Python 与 T016 qualification 仍未关闭。详见
[R10-B46 evidence](evidence/r10-b46-real-provider-native-suite-20260909.md)。

2026-09-09 R10-B45 Spec Kit entrypoint preflight enforcement / **DONE (shared workflow boundary)**：11 个
本机 feature-facing Spec Kit 入口现在都明确执行 `verify-spec-kit-sync.py --require-entrypoints`；同步
检查器同时强制检查这两个标记，缺失入口会失败。`speckit-agent-context-update` 保持指针例外。
这只改变 workflow gate，不改变任何产品任务或 qualification 状态。详见
[R10-B45 evidence](evidence/r10-b45-entrypoint-preflight-enforcement-20260909.md)。

2026-09-09 R10-B44 Spec Kit entrypoint preflight / **DONE (shared workflow boundary)**：在
`speckit-code-design/SKILL.md` 明确要求每次创建或更新 feature 前运行
`verify-spec-kit-sync.py`；个人安装副本已同步，正常强制检查 11/11 通过。失败只记录
workflow `gap`，不产生产品 PASS。详见 [R10-B44 evidence](evidence/r10-b44-speckit-entrypoint-preflight-20260909.md)。

2026-09-09 R10-B43 Spec Kit entrypoint synchronization / **DONE (shared workflow boundary)**：新增
`verify-spec-kit-sync.py`，强制检查三份模板、11 个 Spec Kit 入口和个人
`speckit-code-design` 副本；正常、过期副本和缺失安装探针均按契约返回。首轮 review 发现
source 文件缺失会抛异常，已修复为显式 finding 并复审通过。该检查只证明规则同步，不改变
任何产品任务、T016 或 native qualification 状态。详见 [R10-B43 evidence](evidence/r10-b43-speckit-sync-check-20260909.md)。

2026-09-09 R10-B42 Artifact identity workflow rule / **DONE (shared skill boundary)**：把 R10-B41
r15 的 stale artifact source 首拒绝边界纳入共享 `batch-quality-gates`、code-design skill、
README 与个人安装副本；build lane 现在必须记录实际 output path、source identity、digest，
runner/qualification manifest 必须从该输出重生成并核对。reference SHA-256、关键规则、文档
链接和 validator 通过；不改变产品任务或 T016 状态。详见 [R10-B42 evidence](evidence/r10-b42-artifact-identity-skill-20260909.md)。

2026-09-09 R10-B41 Native stream business-oracle / **DONE (bounded boundary)**：修复
stream-only 与 unary 分支缺少独立业务 marker 的测试契约；`-j2` integration build
118/118 通过，具名 stream/unary/conversation/repository/replacement selectors 均退出 0。
fresh root owner/runner 使用实际新链接产物和重算 digest 执行 PO-001，evaluation `PASS`，
业务 marker、namespace、identity、process-tree、endpoints、trace、cleanup 齐全。r15 的旧
artifact source mismatch 保留为首拒绝边界；Provider callback 仍是 in-process fixture，
独立 requester/Provider transport、I01-I08、PO-002-PO-014、maintained caller/no-Python
和完整 T016 仍未完成。详见 [R10-B41 evidence](evidence/r10-b41-stream-business-oracle-20260909.md)。

2026-09-09 R10-B40 T016 PO-001 owner/runner / **DONE (one bounded qualification case)**：root
MiniNDN owner 在 requester/provider namespace 存活期间执行 canonical PO-001；fresh corrected
manifest 通过 role/digest preflight，native integration process rc 0，业务 marker
`SPEC182_NATIVE_DI_REQUEST_RESULT_OK` 和 namespace/PID/socket/process/cleanup 证据齐全。r10/r11
的旧 role/digest 失败边界保留；T016 仍为 PARTIAL，下一批需使用当前 build identity 执行其余
I01-I08/PO-002-PO-014 case。详见 [R10-B40 evidence](evidence/r10-b40-t016-po001-native-owner-pass-20260909.md)。

2026-09-09 R10-B39 R4-B6 fixture contract guard / **DONE (bounded test-contract boundary)**：修复
post-commit 静态复核发现的 binding-before-failFirst 顺序缺口；system-first `-j4` integration
build 118/118 通过，stream-only、conversation/replacement、unary/repository selectors 全部
退出 0。R10-B37 的生产结果不回退，stream-only 仍不要求 conversation binding；因 `vmstat`
初段出现非零 `si`，下一次 native build 按规则使用 `-j2`。详见 [R10-B39 evidence](evidence/r10-b39-r4b6-fixture-contract-guard-20260909.md)。

2026-09-09 R10-B38 T016 runtime context recheck / **DONE (preflight boundary)**：新 raw run
`spec182-t016-r6` 在默认模式确认仍无 MiniNDN node/netns context；显式 owner 模式新 run
`spec182-t016-r7` 在 UID 1000 的 root 边界退出。系统 NFD socket 的存在不构成资格条件，
没有启动业务进程或生成协议结果；后续需要 root MiniNDN owner 提供 namespace/PID/socket/
peer metadata。详见 [R10-B38 evidence](evidence/r10-b38-t016-runtime-context-recheck-20260909.md)。

2026-09-09 R10-B37 Native client streaming Provider request / **DONE (bounded boundary)**：在
现有 R4-B6 fixture 中补无 conversation 的 token/final 链；新 selector 4 assertions、四个
R4-B6 selectors 和两个 unary selectors 通过，system-first `-j4` integration build 118/118
通过（35.854 s），五 lane 静态审查无 actionable finding。该批只关闭 in-process stream-only
结果出口，跨进程、maintained caller/no-Python 与 T016 不在本批范围。详见 [R10-B37 evidence](evidence/r10-b37-native-client-streaming-request-20260909.md)。

2026-09-09 R10-B36 Remaining production chain reorder / **DONE (planning boundary)**：暂停新增实现，
按真实出口固定 P1 Native requester→Core/Provider、P2 Provider worker/跨进程、P3 YOLO caller、
P4 Qwen/stream/conversation owner、P5 legacy retirement、P6 isolation、P7 convergence→qualification
→handoff。该表复用现有 T010–T017 卡，不改变父任务或验收门；下一轮只从 P1 开始，并在稳定
出口后拆新 Batch ID。详见 [R10-B36 evidence](evidence/r10-b36-production-chain-reorder-20260909.md)。

2026-09-09 R10-B35 Spec Kit command output contract / **DONE (documentation boundary)**：根据
R10-B34 的流程复盘，共享 `batch-quality-gates` 新增入口输出契约；code-design skill、README、
个人安装副本和 11 个本机 Spec Kit 入口副本均明确批次分配、五 lane、四类漏检复盘、可比
构建测量、`Batch growth decision`、`Changed gate` 和 `Closure decision`。定向引用、文本、
SHA-256 与设计校验通过；本批不改产品代码、不运行 native build，不改变父任务或 T016 状态。
详见 [R10-B35 evidence](evidence/r10-b35-command-output-contract-20260909.md)。

2026-09-09 R10-B34 Spec182 regression sweep / **DONE (regression boundary)**：复用已构建的
`build-nac182` binary 执行 `Spec182*` C++ unit、`Spec170NdnsfDiCoreFlow/*` integration
和 76 个 Python binding/compatibility tests，分别以 30.68s、103.84s、3.03s 退出 0。
日志中的预期负例仍保留各自首拒绝边界；本批没有把预期失败或本地 fixture 提升为资格
通过。跨进程 transport、maintained caller/no-Python 与 T016 仍保持 PARTIAL/UNQUALIFIED。
详见 [R10-B34 evidence](evidence/r10-b34-regression-sweep-20260909.md)。

2026-09-09 R10-B33 Native unary repository-reference request / **DONE (bounded boundary)**：在
R4-B6 真实 Provider fixture 中新增具名 C++ selector，同时启用非 streaming 与 `REPO_REF`。
system-first `-j4` integration rebuild 通过（36.514 s），新 selector 的 Provider fetch、
plaintext identity 与 terminal Response 三项断言通过；conversation、replacement、
alternate replacement、streaming repository reference、unary inline 回归同样通过（6 cases）。
本批只关闭单进程 unary repository boundary；Provider worker、跨进程 transport、maintained
caller/no-Python 与 T016 仍保持 PARTIAL/UNQUALIFIED。详见 [R10-B33 evidence](evidence/r10-b33-native-unary-repository-reference-20260909.md)。

2026-09-09 R10-B32 Shared Spec Kit skill feedback loop / **DONE (documentation boundary)**：根据
R4-B4/R3-B1 的漏检复盘，共享 `review-agent` reference 新增 Static Gate Release Checklist，
批次契约新增重试必填的 `Changed gate` 和稳定出口后的 `Batch growth decision`；tasks/plan
模板与 skills README 已同步，安装副本 SHA 一致。该批不改产品代码、不执行 native build，
不改变 3/17 父任务完成状态；真实 native request/result、maintained caller、跨进程和 T016
仍保持 PARTIAL/UNQUALIFIED。详见 [R10-B32 evidence](evidence/r10-b32-skill-feedback-loop-20260909.md)。

2026-09-09 R10-B31 NativeInferenceClient unary Provider request / **PARTIAL (bounded native-client
boundary)**：在既有 R4-B6 真实 Core/Provider fixture 中，新增不带 stream 或 conversation 的
`NativeInferenceClient` 请求，runtime 使用 `TOKEN_DIAGNOSTIC`，Provider callback 通过真实
`CollaborationContext::publishFinalResponse` 返回固定 payload。system-first `-j4` 构建、unary
selector、R4-B6 conversation/replacement 三例和 repository-reference 回归均通过；没有部署
Provider worker、跨进程 transport、maintained caller/no-Python 或 T016 qualification。详见
[R10-B31 evidence](evidence/r10-b31-native-client-unary-request-20260909.md)。

2026-09-09 R10-B30 runner child/endpoint observation / **PARTIAL (bounded harness boundary)**：在
R10-B29 多进程 lifecycle 之上，manifest 现在校验 named `assembly-worker` child、Provider
parent、endpoint owner/peer/transport/address/purpose；abstract UNIX endpoint 在 launch 前拒绝。
`collect_trace` 记录 clone/open/close/mmap/socket/connect 等冻结系统调用，并只将 manifest
声明地址视为成功 connect 的允许 endpoint。40 个 Python cases、`py_compile`、设计 validator
和 `git diff --check` 通过；没有 native build、MiniNDN 或资格运行。真实 worker parentage、
endpoint injection、I02-I08、maintained caller/no-Python 与 T016 仍开放。详见
[R10-B30 evidence](evidence/r10-b30-runner-child-endpoint-observation-20260909.md)。

2026-09-09 R10-B29 runner multi-process lifecycle / **PARTIAL (bounded harness boundary)**：runner
现在按 manifest 启动全部声明的 requester/provider 等业务进程，为每个进程核对 node context、
持有 namespace FD、传递显式 staged NDN 配置环境，并加入同一 supervisor process group，按
统一 run/cleanup deadline 进行 TERM→KILL。每进程 trace/stdout/stderr 保留并归并；collector
要求共享 executable 的每个声明进程都有成功 exec 才报告完整 role coverage，避免 requester
exec 覆盖缺失 provider。36 个 Python cases、`py_compile` 和 `git diff --check` 通过；没有
native build 或 MiniNDN qualification。真实 requester/provider transport、I02-I08、maintained
caller/no-Python 与 T016 仍开放。详见 [R10-B29 evidence](evidence/r10-b29-runner-multiprocess-lifecycle-20260909.md)。

2026-09-09 R10-B20 collector evidence boundary / **PARTIAL**：canonical `collect_trace` 已根据
实际 PID/exec/exit、namespace、endpoint 和 cleanup 观察生成六类运行证据，并只在 case 显式声明
且 stdout 命中独立 marker 时生成 `business-oracle`。31 个 Python cases、`py_compile`、
`git diff --check`、设计 validator 和 fresh root owner/runner run 通过边界检查；该 run 的
`/bin/true` probe 仍因缺 DI business marker 返回 `UNQUALIFIED`。本批关闭 evidence-generation
边界，不关闭真实 native DI case、T014/T016 资格或 maintained caller。
详见 [R10-B20 evidence](evidence/r10-b20-collector-evidence-boundary-20260909.md)。

2026-09-09 R10-B21 I01 native consumer / **PARTIAL**：使用同源 `build-nac182/spec182-installed-consumer`
及声明的 ELF 依赖，在 MiniNDN owner namespace 存活期间通过 canonical runner 执行 I01。进程
返回 0，trace `complete=true`，六类运行 evidence 和独立 stdout marker 全部满足，evaluator
返回 `PASS`。这只关闭 T014/T016 的 I01 正向 native consumer 边界；I02-I08、真实 DI 请求/结果、
maintained caller、no-Python 及 T016 资格仍开放。详见 [R10-B21 evidence](evidence/r10-b21-native-consumer-i01-pass-20260909.md)。

2026-09-09 R10-B22 native DI business case / **PARTIAL**：marker 已写入真实
`Spec182R4B6RealProviderConversation` C++ selector；同源 `integration-tests` 构建 rc=0，
直接 selector 运行 rc=0 并输出 marker。owner/runner 的四次新目录保留了 executable-target、
ELF interpreter、最小 root loader 搜索和 trust-config 缺失的首个失败边界；v4 在 fixture
setup 以 `returncode=201` 返回 `UNQUALIFIED`。本批只关闭直接 native DI business selector
和其构建边界，不关闭真正多进程 transport、maintained caller、I02-I08 或 T016 资格。
详见 [R10-B22 evidence](evidence/r10-b22-native-di-business-case-20260909.md)。

2026-09-09 R10-B23 runner working-directory/config boundary / **DONE (bounded)**：已登记为
R10-B22 的首个 runtime setup 修复批次。runner 将只接受显式 `/probe-root/...` 或 `/tmp`
工作目录，并通过声明的 data/config artifact 提供相对 trust config；不得读取宿主路径。
33 个 Python cases、`py_compile`、设计 validator 和官方 `review-agent` 静态复核通过；新的
owner output `.codex-tmp/spec182-r10-b23-runner-working-dir-owner/` 中 `PO-001` evaluator
返回 `PASS`，returncode=0，trace complete，七类 evidence 与 marker 全部满足。该批不改变
native DI 协议、maintained caller 或 T016 资格状态。详见 [R10-B23 evidence](evidence/r10-b23-runner-working-directory-20260909.md)。

2026-09-09 R10-B24 native suite baseline / **PARTIAL**：沿用现有 `build-nac182` 二进制运行
`Spec182*` C++ unit（247 cases）、`Spec182*` integration（2 cases）和具名
`Spec182R4B6RealProviderConversation`（1 case），全部 exit 0。该批没有 native source
变更，故未重建；`vmstat` 第二采样出现 `si=35780`，下一次 native build 若换页持续须按
策略降到 `-j2`。这些是同源局部/本地 integration 基线，不关闭 maintained caller、跨进程、
I02-I08、legacy retirement 或 T016 qualification。详见 [R10-B24 evidence](evidence/r10-b24-native-suite-baseline-20260909.md)。

2026-09-09 D-SKILL-CLI-BOUNDARY / **DONE (documentation only)**：共享批次规则、code-design
入口、plan/tasks 模板和 skills README 已明确 `--help`、usage/schema rejection、target/link
smoke 与 harness 启动只能证明接线边界；没有真实 native request/result 时不得升级为
behavior、parity 或 qualification。版本化 reference 与本机安装副本 SHA 一致，validator
与 diff 检查通过。该规则不改变产品任务状态。详见 [CLI boundary evidence](evidence/skill-cli-boundary-20260909.md)。

2026-09-09 R10-B25 DI_NativeRequester build / **DONE (bounded executable/CLI boundary)**：按
登记范围使用 system-first 环境和 `-j2` 构建 target，Waf 报告 `1m17.740s`；`ldd` 无缺失
依赖。`--help`、空参数 usage 和错误 schema 三条命令分别返回 0、2、1，错误 schema 不产生
输出文件。修正静态自检的帮助文本字面量后，五 lane review、CLI smoke 和 evidence 已完成；
该结果只证明接线与 fail-closed 解析，不是 native request/result、parity 或 qualification。
真实 requester/provider、跨进程、maintained caller、I02--I08、no-Python 与 T016 仍开放。
详见 [R10-B25 evidence](evidence/r10-b25-native-requester-cli-20260909.md)。

2026-09-09 R10-B26 missing `REPO_REF` negative recheck / **DONE (bounded runtime recheck)**：使用
现有 `integration-tests` binary 运行缺失对象 selector，真实 Provider handler 在 1.946850s
内返回 fetch failure，未产生 runner input 或成功 Response，exit 0。该结果解决 R10-B6
旧 failure-log 的固定 pump 首边界；原始失败仍保留，当前本地负例已关闭。跨进程、maintained
caller、I02--I08、no-Python 与 T016 qualification 仍开放。详见 [R10-B26 evidence](evidence/r10-b26-missing-repo-ref-recheck-20260909.md)。

2026-09-09 R10-B27 native C++ test ownership skill sync / **DONE (bounded documentation
boundary)**：共享 `batch-quality-gates`、`speckit-code-design`、README 和 spec/tasks templates
已明确 native runtime/protocol/state/concurrency/crypto/model 的测试主体、fixture/driver 与
oracle 必须用 C++ 并直接调用生产 target；Python 仅可编排外部设施或覆盖 binding/facade、
offline oracle 和配置拒绝。versioned/installed shared files SHA 一致，静态查询、diff 检查与
设计 validator 通过，无产品构建。该同步不改变 T004/T008/T010/T011/T013/T016 状态；跨进程、
maintained caller、I02--I08、no-Python 与最终 qualification 仍开放。详见
[R10-B27 evidence](evidence/r10-b27-native-cpp-test-ownership-20260909.md)。

2026-09-09 R10-B28 workflow authority alignment / **DONE (bounded documentation boundary)**：
已修正 `docs/agentic_workflow.md` 与本机 `CLAUDE.md` 中把 GSD 当作默认强制门的旧表述，统一为
Context Mode、CodeGraph、Spec Kit 默认必需，GSD/ARS 按工作范围使用；总览补充共享批次门、
review-agent、四类漏检复盘和 native C++ test ownership。与 `AGENTS.md`、constitution 1.5.0、
`skills/README.md` 及共享 reference 的规则扫描通过；无产品构建、无任务验收状态变化。详见
[R10-B28 evidence](evidence/r10-b28-workflow-authority-alignment-20260909.md)。

2026-09-09 R10-B19 owner-to-runner handoff / **PARTIAL**：`--execute-owner --runner-manifest` 现在在
MiniNDN requester/provider namespace 和 NFD 仍存活时调用 canonical runner，写入
`node-context.json`、`runner-result.json` 及统一 `result.json`。29 个 Python harness cases、
`py_compile`、`git diff --check` 通过；真实 root run 返回 `UNQUALIFIED`（runner returncode=0、
trace complete、integrity empty），因为 `/bin/true` probe 没有 DI business evidence。该批只关闭
owner→runner 接线边界；native DI case、maintained caller、cross-process/no-Python 及 T016 资格仍开放。
详见 [R10-B19 evidence](evidence/r10-b19-owner-runner-handoff-20260909.md)。

2026-09-09 R10-B7 caller route contract synchronization / **PARTIAL**：Qwen 配置完成日志现明确
输出 `route=request_native_reference`，T013-D/T013-F 执行契约与任务 registry 也同步到加密
repository reference publication 和 `REPO_REF` facade；R5 历史 evidence 保持原样。源检查、
py_compile、设计校验通过；真实 caller execution、cross-process、legacy retirement 和 T016
仍开放。详见 [R10-B7 evidence](evidence/r10-b7-caller-route-contract-sync-20260909.md)。

2026-09-09 R10-B8 cross-task audit status refresh / **PARTIAL**：`audit.md` 已从旧的
6171cf4d/暂停状态更新到 `7251f9ca` checkpoint，记录 R10-B1--R10-B7 的局部出口，并明确
当前 `request_native_reference` 路由、`NATIVE_REQUEST_PIPELINE_NOT_READY` fail-closed 边界
以及 T004/T008/T010/T011/T013/T016 的剩余 owner。历史审计章节和证据日期未改写；文档
链接、diff 和设计校验通过。该批不提升任何产品任务或资格状态。详见
[R10-B8 evidence](evidence/r10-b8-cross-task-audit-refresh-20260909.md)。

2026-09-09 R10-B9 requester REPO_REF Core-wire boundary / **PARTIAL**：在既有真实 Provider
两轮 fixture 中新增 `NativeApplicationInput::RepositoryReference` 入口，ACK 侧观察到
v2 `REPO_REF`、空 inline payload 及精确发布对象/manifest identity；baseline inline 与新
selector 均通过，首次测试 oracle 的临时 Buffer 迭代器错误已写入 failure-log 并修复。该批
不证明 Provider fetch/decrypt、跨进程 caller 或 T016 资格，父任务和资格状态保持 PARTIAL。
详见 [R10-B9 evidence](evidence/r10-b9-requester-repo-ref-core-20260909.md)。

2026-09-09 R10-B11 requester-produced reference consumption / **PARTIAL**：同一真实 Provider
conversation fixture 现在通过 `ctx.fetchEncryptedLargeData` 解析 requester 发出的
`REPO_REF`，并将恢复明文与 native publisher 原文逐字节比较；inline baseline 同样通过。
该批闭合单进程 requester→Core→Provider fetch/decrypt→conversation 结果边界，但不证明
跨进程维护入口或 T016 资格。详见 [R10-B11 evidence](evidence/r10-b11-requester-repo-ref-provider-fetch-20260909.md)。

2026-09-09 R10-B12 current audit checkpoint refresh / **PARTIAL**：`audit.md` source 已同步到
`0656c2e4`，当前主链明确记录单进程 requester→Core→Provider fetch/decrypt→conversation
结果的局部观察，并保留 maintained caller 跨进程、stream/recovery、legacy zero-use 和
T016 的未闭合 owner。历史审计日期和局部状态未改写；文档链接、diff 与设计校验通过，本批
不提升任何产品或资格状态。详见 [R10-B12 evidence](evidence/r10-b12-current-audit-refresh-20260909.md)。

2026-09-09 R10-B13 integration recipe oracle repair / **PARTIAL**：完整 integration 首轮
在 `Spec175NativeAssembly` 四个用例于 worker recipe digest 首边界退出；原因是测试 helper
仍排序 input/output names，而生产 canonical serializer 使用 contract order。已修正 helper
并登记首轮 raw run；system-first `-j2` 重建成功，受影响的 `Spec175NativeAssembly/*` 7/7
通过。该修复不改变生产 serializer 或协议；完整 integration 和 T016 资格仍开放。详见
[R10-B13 evidence](evidence/r10-b13-integration-recipe-oracle-repair-20260909.md)。

2026-09-09 R10-B14 same-source unit/integration validation / **PARTIAL**：R10-B13 修复后，
完整 `unit-tests --log_level=test_suite` 通过且无错误（2:16.88，最大 RSS 约 8.4 GB）；
完整 `integration-tests --log_level=test_suite` 通过且无错误（3:15.30）。R10-B13 的四个
`Spec175NativeAssembly` 首轮失败用例均已包含在第二轮完整 integration 绿灯中，运行期间
无新增 swap-out。该批只关闭同源本地 suites，MiniNDN owner 的 node/netns context、跨进程
维护入口、no-Python 和 T016 资格仍开放。详见 [R10-B14 evidence](evidence/r10-b14-full-unit-integration-20260909.md)。

2026-09-09 R10-B15 T016 MiniNDN owner preflight recheck / **PARTIAL**：本机 NFD 已成功启动，
`/run/nfd/nfd.sock` 和 `nfdc status report` 可用；但新的 `campaignCase=I01` 运行仍在业务
启动前以 `MININDN_NODE_CONTEXT_NOT_PROVIDED` 退出，`ip netns list` 没有由 owner 创建的
隔离 namespace。该结果只收窄了 preflight 阻塞边界，不是协议、跨进程或资格结果；T016 仍
需由 owner 提供真实 node/netns/child/socket/peer metadata 后重试。详见
[R10-B15 evidence](evidence/r10-b15-t016-preflight-recheck-20260909.md)。

2026-09-09 R10-B16 native closure node-context boundary / **PARTIAL**：runner 现在校验
MiniNDN 提供的 namespace inode、owner PID/start ticks、NFD socket 和 peer metadata，并以
held FD 通过 `nsenter` 接到既有 bubblewrap/strace launch；缺 context 在业务启动前明确
`UNQUALIFIED`。23 个 Python harness cases、`py_compile` 与 diff 检查通过，批次只关闭该
runner 边界，不新增 DI qualification case、不创建 host-netns 替身。MiniNDN owner topology、
真实 cases 和 T016 资格仍待下一批。详见 [R10-B16 evidence](evidence/r10-b16-native-closure-node-context-20260909.md)。

2026-09-09 R10-B17 MiniNDN owner context producer / **PARTIAL**：显式 `--execute-owner` 已创建
tracked requester/provider topology，启动各自 NFD 并导出 namespace inode、owner PID/start ticks、
NFD socket 与 peer IDs；fresh raw run 产生 `node-context.json`，随后因 manifest 尚无可执行
closure artifact/process case 返回 `NATIVE_CLOSURE_CASE_DEFINITION_MISSING`。默认 registration-only
入口保持原行为。下一批需冻结 runner case 描述并在 owner 存活期间调用 runner；T016 仍未资格通过。
详见 [R10-B17 evidence](evidence/r10-b17-minindn-owner-context-20260909.md)。

2026-09-09 R10-B18 runner dynamic ELF/trace boundary / **PARTIAL**：runner 现在把声明的
shared-library artifact 逐文件挂到 ELF 的绝对路径，并按 PID 配对 strace 的
`<unfinished ...>` 与 `<... resumed>`；root `/bin/true` probe 返回 0 且 trace `complete=true`。
不完整 trace 仍 `UNQUALIFIED`，probe 因缺业务 evidence 仍不作 PASS。下一批需提供真实 native
closure case，并在 owner 存活期间调用 runner。详见 [R10-B18 evidence](evidence/r10-b18-runner-elf-trace-boundary-20260909.md)。

2026-09-09 R10-B10 current audit checkpoint refresh / **PARTIAL**：`audit.md` source 已同步到
`9f80a1ce`，当前主链明确包含 Provider fetch/decrypt，并保留 maintained caller execution、
cross-process 与 T016 的未闭合 owner；历史审计日期和局部状态未改写。文档链接、diff 与设计
校验通过，本批不提升任何产品或资格状态。详见 [R10-B10 evidence](evidence/r10-b10-current-audit-refresh-20260909.md)。

2026-09-09 R10-B6 Provider REPO_REF fail-closed negatives / **PARTIAL**：三个生产 Provider
负例现已编码并由完整 `Spec170NativePostSelection` suite 的 8/8 cases 验证：缺失对象在
局部 1 s fetch budget 内得到 `failed to fetch native DI request input reference`，明文尺寸
不匹配得到精确错误，malformed envelope 得到 parser 错误；三个用例均未进入 runner、未发布
成功响应。首轮默认 30 s fetch budget 超过 fixture 的 3 s pump，失败边界已保留在
`docs/failure-log.md` 和 `.codex-tmp/spec182-r10-b6/`，修正后 system-first `-j4` 118/118
构建通过。该局部出口 `CLOSED_FOR_VALIDATION`；跨进程、维护调用方完整资格和 T016 仍开放。
详见 [R10-B6 evidence](evidence/r10-b6-provider-repo-ref-fail-closed-20260909.md)。

2026-09-09 R10-B5 Provider REPO_REF execution boundary / **PARTIAL**：既有 native ingress
fixture 现发布加密 `REQUEST-LARGE`，提交 v2 `REPO_REF` envelope，并由真实
`ServiceProvider`/`NativeProviderHandler` fetch/decrypt 后交给 runner；新 selector 与同 suite
其余四个 case 共 5/5 通过。一次逗号过滤器 setup 失败（exit 200、未进入测试）已保留并改用
suite selector 重跑；构建为 system-first `-j4`、118/118、1m50.876s，未见持续 swap。
本地 Provider 消费出口 `CLOSED_FOR_VALIDATION`，跨进程、维护调用方完整资格、负例和 T016
仍保持开放。详见 [R10-B5 evidence](evidence/r10-b5-provider-repo-ref-execution-20260909.md)。

2026-09-09 R10-B3 YOLO native reference caller / **PARTIAL**：维护的 YOLO 2x2 native 分支
现先发布加密 tensor bundle，再通过 `request_native_reference` 进入 C++ requester；旧
ACK-driven planner/lifecycle 分支保持独立。源分支检查、py_compile、24 个兼容/legacy 测试、
scoped diff check 和设计校验通过；未执行网络、Provider、跨进程或 T016。该局部出口
`CLOSED_FOR_VALIDATION`，详见 [R10-B3 evidence](evidence/r10-b3-yolo-native-reference-caller-20260909.md)。

2026-09-09 R10-B4 Qwen native reference caller / **PARTIAL**：Qwen native helper 的 typed
context bundle 现发布为加密 REPO_REF 后进入 C++ requester，generation options、observer
与 conversation fail-closed 保持不变。相关源检查、py_compile、25 个兼容/legacy 测试、
scoped diff check 和设计校验通过；真实 Provider/stream/conversation/cross-process/T016
仍开放。详见 [R10-B4 evidence](evidence/r10-b4-qwen-native-reference-caller-20260909.md)。

2026-09-09 R10-B2 native REPO_REF facade / **PARTIAL**：公开 `APPClient` 与 `InferenceClient`
现在可以把 journal 已发布的 `LargeDataReference` 以 canonical JSON 送入 C++ native
requester，并明确设置 `REPOSITORY_REFERENCE`；发布摘要绑定、planner 绕过、observer 和
options 转发均有测试。首轮只暴露测试替身缺少 C++ 默认空字段，已记录并修正；17 个兼容
测试、35 个相关 Spec180/Spec182 测试、py_compile、scoped diff check 和设计校验通过。
该局部出口 `CLOSED_FOR_VALIDATION`，真实 encrypted fetch、Provider 执行、维护调用方迁移、
跨进程及 T016 仍开放。详见 [R10-B2 evidence](evidence/r10-b2-native-reference-facade-20260909.md)。

2026-09-09 R10-B1 native REPO_REF preparation / **PARTIAL**：已让 v2 加密引用通过 C++
requester preparation，保留 reference 身份并移除错误的 “resolution is not linked” 拒绝；
requester 不解密，Provider fetch/decrypt 权限未改变。首轮编译漏检已补充测试头文件并保留
原始边界；复审、`-j4` 增量构建、Preparation 18、ClientState 17、PlanSealer 12、
V3Placement 9 及完整 1015-case unit 均通过。该批本地出口 `CLOSED_FOR_VALIDATION`，
T004/T010/T016 的真实 Provider、跨进程和 MiniNDN 仍开放。详见
[R10-B1 evidence](evidence/r10-b1-repo-ref-preparation-20260909.md)。

2026-09-09 R8-SKILL retrospective feedback / **DONE**：根据 R3-B1 的编译/链接漏检、R4-B4
的批次膨胀与运行时漏检、R6-B9/R9-B1 的 legacy 并发边界，已将首个失败证据与改变的静态
检查写入共享 `batch-quality-gates`、`pre-test-static-review`、code-design、模板及本 Spec
记录。重复同类漏检需先更新 skill/template/checklist 或记录替代门禁；不改变产品任务状态，
不以单次耗时推导提效。详见 [skill review evidence](evidence/skill-review-coverage-20260909.md#retrospective-feedback-loop)。

2026-09-09 R9-B1 selection-status concurrency ownership / **PARTIAL**：ASAN 首次复现定位到
`ServiceProvider::reportSelectionOperationStatus` 并发扩展 `memberStatuses` 时的
heap-use-after-free；现以专用 `m_selectionExecutionStatusMutex` 保护 status map/vector
及查询快照，并加入 8×8 并发成员回归。`unit-tests`/`integration-tests` 均以 system-first
`-j4` 构建通过，D2h212 修复后新鲜进程 50/50 通过，ASAN-preload 20/20 无内存错误；历史
R6-B9 失败日志仍保留。该局部并发出口 `CLOSED_FOR_VALIDATION`，T010/T013/T016 与跨进程
资格仍开放。详见 [R9-B1 evidence](evidence/r9-b1-selection-status-concurrency-20260909.md)。

2026-09-09 R6-B9 legacy D2b freshness repair / **PARTIAL**：当前源码的回归 trace 证实
单一 producer/session frontier 会在 provider0 的 sequence 4 先到时丢弃 provider1 的
sequence 3。现已按 session 与 publication name 分开记录 sequence，在 `svs_mutex` 下
拒绝旧 session 和同名旧序列，同时允许不同 publication 的合法乱序。五个 D2b selector
及具名 D2h212 selector 单独运行通过，`integration-tests` 118/118 的 `-j4` 构建通过；
未过滤的整套 Spec170 suite 首次尝试以及历史具名 D2h212 的 2/20 重复在 D2h 用例暴露
callback 缺失和 `double free`；两次后续 suite 重跑、五个 D2b 各 10 次，以及带依赖追踪的
具名 D2h212 新鲜进程 20/20 均通过。历史边界仍保留为未解释的可复现性风险，因此批次仅
对本地 D2b freshness 出口 `CLOSED_FOR_VALIDATION`，T013/T016 与整套运行稳定性仍开放。
详见 [R6-B9 evidence](evidence/r6-b9-legacy-d2b-freshness-20260909.md)。

2026-09-09 R7-B2 alternate-provider replacement / **DONE (local batch)**：实现并接通
replacement 的新 execution/contract digest 与当前 role map；coordinator pending state 保留
原始 parent map 供 CAS，successor checkpoint 使用 planner 选出的备用 Provider map。补上
coordinator expected role-set/provider 校验，以及结构化 recovery Selection name 与 legacy
decision name 的 parser 边界。逐任务静态门和批末组合审查均覆盖生产 caller、wire、C++
test/harness、Waf source closure 与 migration gap；复审无 actionable finding。`unit-tests`/
`integration-tests` system-first `-j4` 306/306 build 及 coordinator/parser、单 Provider
negative、双 Provider alternate replacement、原有 positive two-turn selectors 全部 exit0；
vmstat 未见持续 swap。详见 [R7-B2 evidence](evidence/r7-b2-alternate-provider-replacement-20260909.md)。
本批 `CLOSED_FOR_VALIDATION` 仅覆盖本地真实 Provider replacement；跨进程会话、Python
caller migration、T012/T013/T014/T015/T016 与最终资格仍保持 PARTIAL。

2026-09-09 R6-B8 collector role/cold coverage / **PARTIAL**：修正
`run-spec182-native-closure.py` 对 manifest `requiredRoles` 和 `cold` marker 的漏检。
缺少或格式错误的角色/冷路径观测现在返回 `UNQUALIFIED`；已观测但角色集合或冷暖状态
不匹配返回 `FAIL`，重复角色不会因集合折叠而放行。18 个独立 Python cases、
`py_compile` 与 `git diff --check` 通过；没有 C++ 构建或 MiniNDN/namespace 运行。真实
process-tree、namespace、endpoint、child cleanup 和 T016 资格仍未完成。详见
[R6-B8 evidence](evidence/r6-b8-collector-role-cold-20260909.md)。

2026-09-08 R7-B1 R4-B6 single-provider replacement boundary / **PARTIAL**：真实 Provider
replacement 负例已接入 `Spec182R4B6RealProviderConversationReplacement`。静态门覆盖
公开 `NativeInferenceClient`、Provider failure injection、recovery ACK、checkpoint
写入边界、测试注册及 integration target；无 actionable finding。首次运行按成功替换
断言在 `ACK_CLOSED` 暴露 `DI_NATIVE_NO_ADMITTED_PROVIDER`（单 Provider 被排除后无可用
候选），原始 exit201 已保留；修正为契约允许的单 Provider 负例后，`-j2` integration
构建 118/118、负例 1 case、正向真实两轮 1 case 均 exit0。负例断言无 checkpoint、一次
Provider collaboration 与 `NATIVE_REQUEST_STAGE_FAILED`/`ACK_CLOSED`。本批只关闭 CC-4c
的单 Provider 负例；成功 alternate-provider recovery、T011-C 跨进程会话、T012/T013
caller parity 与 T016 仍保持 PARTIAL。详见 [R7-B1 evidence](evidence/r7-b1-r4b6-replacement-20260908.md)。

2026-09-08 R5-B7 native stream observer facade / **PARTIAL**：已有 C++
`NativeInferenceHandle::observe` 已通过 `_ndnsf` 绑定为受控 Python callback，stream
path 现在发布已接受的 token event；`APPClient.request_native_payload` 在提交后挂载可选
`on_event`，不改变 native 事件排序、replay、终态或所有权。静态门修正 const handle
漏洞；shared DI target、强制 extension rebuild、C++ slow-observer/replay/deadline 与
public conversation token-order selectors、28 个 Python focused cases 和导出 smoke
check 通过；完整 placement selector 首次运行曾在测试进程内存访问边界 exit201，隔离用例
和后续完整重跑通过，已按瞬时测试边界记入 `docs/failure-log.md` 及 r0/r1/r2 原始目录。
真实 Provider callback、跨进程 streaming parity、conversation owner 与 T016 仍未完成。
详见 [R5-B7 evidence](evidence/r5-b7-native-observer-20260908.md)。

2026-09-08 R5-B8 native Qwen stream callback route / **PARTIAL**：维护中的 Qwen
full-generation native branch 现在把 observer 传入 `request_native_payload`，只记录并校验
非终态 `GenerationTokenEventV1`，等待 terminal 通知后返回 bounded stream count；28 个
Python focused cases、`py_compile`、C++ observer/deadline/public-conversation selectors 与
`git diff --check` 通过。真实 Provider/cross-process streaming、conversation owner、YOLO
及 Provider migration、T016 仍未完成。详见 [R5-B8 evidence](evidence/r5-b8-native-qwen-stream-callback-20260908.md)。

2026-09-08 R5-B9 native conversation owner configuration / **PARTIAL**：已登记
`T013-E` 与 `ndnsf-di-native-conversation-v1` 配置契约；实现将由 C++ 读取 owner-only
key files、构造 journal/coordinator 并注入 configured client，Python 只保留 opaque handle；
shared DI/extension source closure、C++ conversation selectors 与 15 个 Python focused cases
通过，首次 stale-library 链接、资源 swap 样本及无 NFD 的直接 probe 边界已记录。真实
Provider receipt/control、跨进程两轮、恢复/replacement 与 T016 不在本批。详见 [R5-B9
evidence](evidence/r5-b9-native-conversation-owner-20260908.md)。

2026-09-08 R5-B10 maintained YOLO native payload route / **PARTIAL**：YOLO User 新增
显式 `--native-requester-config` 分支，配置由 `APPClient` 交给 native catalog/grant/runtime
owner，inline native tensor bytes 通过 `request_native_payload` 提交；分支校验 YOLO model
identity、task identity、timeout 与数值结果，并在配置/结果错误时 fail-closed，不能落入
Python planner。`py_compile`、5 个 legacy/source route checks 与 `git diff --check` 通过；
Spec180 lifecycle journaling、真实 Core/Provider 请求、数值资格、旧路径退出与 T016 仍未完成。
详见 [R5-B10 evidence](evidence/r5-b10-yolo-native-payload-route-20260908.md)。

2026-09-08 R5-B11 legacy reachability audit / **PARTIAL**：重新生成
`compatibility-manifest.json` 并用当前维护入口核对 `provider.py`、`runtime_v1.py`、
`app_sdk/facades.py` 与 `app_sdk/placement.py`。这些旧 owner 仍有仓库消费者或用途未明，
均保持 `RETAINED_UNTIL_MIGRATION` / `removalEligible=false`；未删除实现，也未把默认路由
宣称已退出。5 个 legacy/source checks、manifest 结构校验与 `git diff --check` 通过。
详见 [R5-B11 evidence](evidence/r5-b11-legacy-reachability-audit-20260908.md)。

2026-09-08 R6-B1 collector verdict boundary / **PARTIAL**：完整 trace 的 Python 映射、未声明
endpoint 或业务退出不匹配现在返回 `FAIL`；trace 缺失/未配对、超时或证据缺口返回
`UNQUALIFIED`，并保留 CLI exit-1/exit-2 边界。12 个独立 Python cases、`py_compile` 与
`git diff --check` 通过；本批没有 native source 变化，未运行 C++ build。真实 namespace、
短命 child、socket 和 MiniNDN 上下文仍由 T016 负责，故 T014-A 只记 PARTIAL。
详见 [R6-B1 evidence](evidence/r6-b1-collector-verdict-20260908.md)。

2026-09-08 R6-B2 qualification harness registration / **PARTIAL**：I01--I08 counterexamples
与 PO-001--PO-014 acceptance IDs 已写入唯一 case manifest；campaign owner 校验 canonical
runner、T016 owner 和 bounded run/cleanup limits，并在缺少真实 node/netns context 时创建
fresh-run `UNQUALIFIED` 记录。21 个 Python cases、manifest registration、`py_compile` 和
结构校验通过；真实 MiniNDN node setup、collector execution 和 PO outcomes 仍由 T016 负责。
详见 [R6-B2 evidence](evidence/r6-b2-harness-registration-20260908.md)。

2026-09-08 R6-B3 cross-task convergence / **PARTIAL**：已完成整体接线审查，核对 native
requester、maintained YOLO/Qwen caller、Provider registration、legacy manifest、collector/
harness 和 T016 出口。当前已知缺口仍属于对应实现卡或 T016：默认 public Python route 尚未
退出，真实 Provider 两轮/stream/recovery、跨进程 callback、legacy zero-use 和 node/netns
资格尚未证明；本卡不把局部 selector 或 manifest 计为整体 PASS。最新旧
Spec170 D2b 兼容性探针还发现当前二进制在 User 已发布两个 Selection 后只有
provider0 到达 callback，provider1 与 DATA_V1 fetch 超时；该运行边界归入
T013-B 迁移前置条件，未新增产品任务完成。详见 [R6-B3 evidence](evidence/r6-b3-cross-task-convergence-20260908.md)
及 [R6-B7 diagnostic](evidence/r6-b7-legacy-d2b-regression-20260908.md)。

2026-09-08 R6-B4 T016 preflight / **PARTIAL**：工具可执行但本机没有 `/run/nfd/nfd.sock`，
且 campaign owner 尚未获得 MiniNDN node/netns metadata；I01 fresh run 在业务启动前以 exit 2
`UNQUALIFIED` 结束，原始结果与 failure-log 已保留。完整 unit/integration/MiniNDN/no-Python
matrix 必须在外部 node/NFD context 可用后，以新 run directory 重试。

2026-09-08 R6-B5 status-document synchronization / **PARTIAL**：已完成文档一致性批次，
修正 audit/traceability 中落后的实现阻塞、完成数量和 planned evidence 表述，使其与当前
Execution Progress、R6-B3 收敛和 R6-B4 preflight 一致。此批不改变产品代码或资格结论，
证据待门禁后补入 R6-B5。

2026-09-08 R6-B6 native route compatibility forwarding / **DONE (batch only)**：修复
public `InferenceClient` 在无 conversation owner 时向 Core 传递多余 `None` 的兼容性回归，
并新增显式 owner 三参数转发回归。47 个 Spec182 Python binding/compatibility/legacy/
closure cases、`py_compile` 与 `git diff --check` 通过；没有 C++ 重编、网络请求或资格运行。
该批只关闭 facade 参数转发，T012-B/T013/T016 与真实 native requester/provider parity 仍开放。
详见 [R6-B6 evidence](evidence/r6-b6-native-route-compat-20260908.md)。

2026-09-08 R5-B6 Qwen/streaming requester / **PARTIAL**：维护中的 Qwen 普通请求已通过
`--native-requester-config` 进入 `APPClient` 的 native catalog/grant/runtime/admission
composition，再由 `request_native_payload` 交给 C++ requester；generation 和 stream DTO 在
`_ndnsf` 中构造。固定 tiny streaming harness 仅转发配置，并在非 Qwen runtime 先拒绝；
conversation continuation 因 `NativeConversationCoordinator` 尚未配置而显式 fail-closed，
不回退 Python coordinator。`py_compile`、`git diff --check`、28 个 Python focused cases 与
shared DI `-j4` 增量 build 已通过；真实 native streaming/Core/Provider 请求、callback lifetime、
native conversation owner、YOLO user、Provider retirement 与 T016 仍未完成。详见 [R5-B6
evidence](evidence/r5-b6-native-caller-migration-20260908.md)。下一步先补 native streaming/
conversation owner 或登记独立批次，再继续其余 caller migration。

2026-09-08 R5-B6A native option binding / **DONE**：`_ndnsf` 单一 pybind 入口已暴露
generation contract、Core `StreamRequestOptions`、`ControllerVersion` 与 conversation
continuation；stream `validate()` 和 wire round-trip 由 C++ 执行，`event_key_grant_wire`
使用 bytes/None 转换，native worker callback 不暴露。extension 与候选 DI shared library
按显式闭包重建，10 个 Python focused cases 通过；真实 caller migration、完整 lifetime/parity
与 T016 仍开放。详见 [R5-B6A evidence](evidence/r5-b6a-native-option-bindings-20260908.md)。

2026-09-08 R5-B5 maintained YOLO native requester / **PARTIAL**：`DI_NativeRequester`
已切换到 shared `nativeRequestRuntimeFromJson`，由 C++ parser 统一绑定 catalog、grant、
security、budget 和 state mapping；静态门、`-j4` 构建、C++ parser selector、CLI selector
和 25 个 Python binding/兼容检查均通过。真实 Core/Provider request、维护中的 Python
YOLO user、其余 caller 迁移与 T016 仍开放；详见 [R5-B5 evidence](evidence/r5-b5-yolo-native-requester-20260908.md)。

2026-09-08 R5-B3 maintained caller audit / **PARTIAL**：七个登记入口已核对真实调用方、
当前 Python/native owner 和迁移边界；没有修改产品源码或运行网络。`request_native()` 的
完整 runtime composition 尚未由任何 maintained caller 提供，YOLO harness 的 native
Provider 命令不等于 User 已迁移，Qwen/streaming harness 仍委托 Python runner。下一步先
完成 R5-B4 operator-pinned native config fixture，再按 caller family 分批迁移；详见
[R5-B3 caller audit](evidence/r5-b3-maintained-caller-audit-20260908.md)。

2026-09-08 R5-B4 native runtime config fixture / **PARTIAL**：native runtime JSON 的字段、
catalog/grant/admission 绑定和 C++ selector 已固定并通过 focused C++/binding 检查；首轮
extension 导入暴露 stale shared library，已重建 shared target 后修复。当前仍不把现有手工
integration fixture 当作 maintained caller 配置，也不运行网络资格；下一步进入 R5-B5 YOLO
requester 迁移。

2026-09-08 R5-B2 native runtime construction binding / **PARTIAL**：本单元扩展了
现有 C++ owner 的 pybind 组合边界（catalog、preparation、offer admission、runtime、grant
client），并将显式 native route 接到 canonical `APPClient` 与公开 `InferenceClient`；不复制
Python planning 或新增 wire。静态门发现并修复 grant requester 与实际 `ServiceUser` identity
脱节；extension 重建成功，binding 24 cases、兼容选择 32 cases 和 invalid-catalog negative
通过。真实请求 parity、maintained caller migration、旧路径退出和跨进程资格仍必须单独验收，
详见 [R5-B2 evidence](evidence/r5-b2-native-runtime-binding-20260908.md)。

2026-09-08 T012-A binding ABI verification / **PARTIAL**：首次 extension 构建误选
`/usr/local/lib/libnac-abe.so`，导入在 `Consumer::clearCache(Name,string)` 符号处失败；候选
Waf DI library 重链并显式选择 NAC-ABE/SVS 依赖后，extension 导入通过，四组 Python
focused suites 21/21 PASS。`ldd`、符号、哈希和命令输出见
[T012-A ABI evidence](evidence/t012-a-binding-abi-20260908.md)。该结果只关闭本地
binding closure 观察项；完整 native requester runtime、caller migration 与 T016 仍未完成。

2026-09-08 R4-B6 real Provider conversation / **PARTIAL**：最终无调试追踪运行通过
`Spec170NdnsfDiCoreFlow/Spec182R4B6RealProviderConversation`；同一真实
`ServiceProvider` 和公开 `NativeInferenceClient` 完成 FULL_CONTEXT 首轮及
APPEND_DELTA 二轮，均收到 receipt、COMMIT/commit ACK 并提交 coordinator checkpoint。
兼容性修复后批末 `integration-tests -j4` 构建 16.65s、combined unit/integration 增量构建
1.01s，stream suite 17/17 和 name suite 16/16 均 PASS；`git diff --check` 与
`validate_design.py` PASS。
本批修复 V2 structured request ID、event/collaboration name、SVS session/seq freshness、
Provider 单 worker 控制面等待和 End 后 gap retry。恢复/replacement 负例尚未运行，故
T011-C、T012--T016 及父任务完整验收仍未关闭；下一步先补 CC-4c 的真实失败边界，再按
T012 caller/binding 依赖推进，不把本地两轮 PASS 写成资格 PASS。

2026-09-08 R4-B6 batch registration / **READY**：R4-B4/R4-B5 的本地公开首轮与预置事务已明确
边界；新增 R4-B6 只负责真实 Provider receipt/control/commit 与第二轮 `APPEND_DELTA`，并在
同一 coordinator 下记录恢复或 replacement 首个失败边界。尚未修改产品源码或运行 integration；
下一步先读 R4-B6 evidence、failure-log、架构和 CodeGraph，再逐小任务静态门执行。

2026-09-08 Shared workflow/build policy documentation sync / **PASS**：共享 Spec Kit
技能与模板沿 `91bb428f` 的批次质量门继续作为新 Spec 的统一标准；当前开发机文档示例
已统一为默认 `-j4`，仅按实测换页/卡顿降档，历史 evidence 的 `-j2` 保持原事实。
本轮又将需求的 Acceptance Evidence Contract、计划的 Coverage matrix/result owner、
任务的 native C++ selector 与 lane 完整性写入模板，并把 constitution 与 taskstoissues
入口纳入共享职责矩阵。本轮只读 review-agent 协议检查无 findings，结构验证与
`git diff --check` 通过；未改产品任务状态，未运行产品构建或测试。见[同步记录](evidence/skill-batch-workflow-20260908.md#follow-up-skill-contract)。

2026-09-08 R4-B5 Public conversation requester boundary / **PARTIAL**：公开
`NativeInferenceClient` 现在在 native owner 分配 request ID 并编码 envelope 后，为空的
`requestContractDigest` 填入真实 digest；非空值继续精确校验。新增本地
`FULL_CONTEXT` requester→Core ACK/plan/commit→stream final→coordinator checkpoint/commit
用例，定向 1 case/19 assertions、placement 9 cases/555 assertions、conversation 5 cases/39
assertions PASS；system-first `-j4` unit build 188 tasks、30.69s PASS。receipt 与 Provider
ACK 是已认证 `VerifiedCollaborationData` 预置，故本轮仅证明本地事务接线，不能关闭真实
Provider 两轮、恢复/替换或 T016 资格；详见[R4-B5证据](evidence/r4-b5-public-conversation-20260908.md)。

2026-09-08 R4-B4 CC-1/CC-2 focused validation / **PARTIAL**：canonical JSON Unicode修复后
9个`Spec182Conversation*` C++ cases PASS，oracle check PASS；这只关闭Wire/Journal/Coordinator
focused边界，未关闭Provider确认或公开请求链。当前主机后续构建恢复默认`-j4`。

共享回归`Spec182CanonicalJson*,Spec182Conversation*,Spec182EpochText*,Spec182StreamAcceptance*,
Spec182Sampling*`共25 cases按默认`-j4`构建并PASS；`Spec182ProviderHost*`另有6 cases PASS。
runtime+coordinator overload 编译验证后，requester/conversation/provider/stream 组合回归共49 cases PASS；
`integration-tests -j4` 在补齐完整 DI source closure 后链接 PASS；`Spec182GrantClientFlow/*` 2 cases
在修正 Core APP Data freshness 后 PASS。保留CC-3及公开两轮请求为PARTIAL；当前 integration
目标仍无公开两轮 conversation selector，不能将授权集成 PASS 当作 T011-C/T016 资格。

2026-09-08 R4-B4 CC-3B requester/provider transaction wiring / **PARTIAL**：Requester 已收集
认证 receipt，严格核对 request/scope/topic/role/provider/conversation/epoch/generation/身份/期限，
通过现有加密 collaboration 端口按角色发布 COMMIT/ROLLBACK/FINALIZE，并等待逐角色 canonical
commit ACK；Provider handler 已保留 COMMIT 后补偿窗口、canonical ACK 与精确 rollback/release。
coordinator 修正 callback 锁顺序，提交期间延后清理 scope key，replacement 安装竞态和 token
接受原子性也已收口。官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md` 只读静态门
覆盖本批完整差异；发现并修复 coordinator→operation 与 operation→coordinator 死锁、提交中
过早清 key、replacement pending 泄漏和 coordinator/local prefix 半提交。`git diff --check`
PASS；DI library 与 unit-tests 使用 system compiler/binutils、`-j4`构建 exit 0，定向 requester/conversation/provider/stream 回归 49 cases PASS，
oracle `build-conversation-oracle.py --check` PASS。尚无真实两轮/跨进程 receipt/control 集成
结果；ProviderHost 6 cases 也已通过；本批保持 PARTIAL，T011-C 不勾选。

2026-09-08 Progress Audit / **PARTIAL**：按用户要求暂停新增实现。路线评估为
CONDITIONAL PASS，不能按当前证据承诺完整目标可顺利收敛；父任务仍3/17验收，
不换算工作量百分比。Progress Audit 之前的 R4-B4 入口判断已被 CC-3A/CC-3B 更新；公开
会话仍未完成真实跨进程资格，新增 runtime/logical prefix 区分及 journal metadata 仅有
本地定向验证。
见[本轮审计](evidence/r4-b4-conversation-chain-20260908.md#progress-and-feasibility-audit)。
结构检查与prerequisites PASS；暂停新增实现的审计结论已转为按 R4-B4 稳定接口继续，下一步
是补真实两轮/恢复集成测试，不重做已验证组件。

2026-09-08 R4-B4 CC-3A projection / **PARTIAL**：NativeInferenceClient 请求选项已接入
continuation，dispatch 创建 owner turn；planner 校验实际 role/provider map，填充每角色
turn/state projection，并把 conversation scope 加入 Core plan。DI library `-j4`
构建 exit0（24.489s），`Spec182PlanSealer*` 12 cases PASS；未运行真实 continuation
projection、receipt/control 或 Provider 网络。见[CC-3A记录](evidence/r4-b4-conversation-chain-20260908.md#cc-3a-requester-to-projection-context)。

2026-09-08 R4-B4 CC-3 boundary / **PARTIAL**：Provider COMMIT后的FINALIZE/ROLLBACK窗口、
lambda值捕获、coordinator durable gate与 requester 的 receipt/control/延后清理已接线；
仍缺真实跨进程两轮集成及 T011-C 资格。相关入口见[CC-3B记录](evidence/r4-b4-conversation-chain-20260908.md#cc-3b-requester-provider-transaction-wiring)。

2026-09-08 R4-B4 CC-2 owner / **PARTIAL**：coordinator已替换空abort和伪seed路径，
真实pending/accepted prefix、一次replacement、认证prepare与Provider promotion→
durable journal→parent晋升流程已编码；3个owner用例未运行，产品仍未提交。
见[owner checkpoint](evidence/r4-b4-conversation-chain-20260908.md#cc-2-coordinator-owner-checkpoint)。
NativeInferenceClient仍仅保存m_conversations；下一步CC-3公开接线，然后处理ABI依赖
并统一构建。T011未完成。

2026-09-08 R4-B4 CC-2 / **PARTIAL**：已编码旧格式加密journal读写、writer lease、
总quota、torn-tail/事务恢复及对应C++测试，尚未构建。见
[journal checkpoint](evidence/r4-b4-conversation-chain-20260908.md#cc-2-journal-implementation-checkpoint)。
源码静态检查修正duplicate-key尾部分类和旧spool额度遗漏；产品仍未验收/未提交。
下一步替换coordinator的空abort/伪seed逻辑，消费journal后接公开两轮请求；同批末统一构建。

2026-09-08 R4-B4 CC-1 / **PARTIAL**：原生checkpoint/transcript/加密envelope认证函数
和C++测试已编写，尚未构建；旧参考4 checkpoint/2 transcript/2 journal事务生成、
重新打开及确定性重现PASS，最初两个冻结案例保持不变。见
[wire checkpoint](evidence/r4-b4-conversation-chain-20260908.md#cc-1-wire-implementation-checkpoint)。
下一步CC-2替换scaffold持久owner、CC-3公开两轮接线，同批末统一构建，父任务未关闭。

2026-09-08 R4-B4 / **IN_PROGRESS**：已审查真实会话链，C++ coordinator 当前为未接线
scaffold，旧测试手工seed不能证明首轮/续接；abort无状态变更，checkpoint/journal与旧
认证加密格式不兼容。详见[R4-B4审计及执行成员](evidence/r4-b4-conversation-chain-20260908.md)。
按旧Python契约冻结独立oracle后，CC-1至CC-4同批完成native owner、持久化和公开接线；
尚未编译或执行本批测试，不关闭T011-C。
CC-1已冻结两个旧checkpoint/continuation oracle并通过确定性重现、错key/篡改拒绝；
这是离线参考验证，尚未证明C++兼容，完整transcript/journal向量与native实现仍待完成。

2026-09-08 R4-B3 / **DONE (local epoch text batch)**：终止 stable flush 和一致性校验
先于事件接受，保留 feedback/state 顺序。真实 tokenizer MAX/EOS/stop/replay 与 mismatch
回归及既有 epoch/stream/sampling 共24 cases/411 assertions PASS；增量 unit build
24.266s，实际 DI 共享库增量构建 PASS。测试 optional 编译错误已修复，原始日志保留。
证据见 [R4-B3](evidence/r4-b3-epoch-text-20260908.md#final-local-result)。下一步完成
T011-B 接线核对及 T011-C 会话日志/续接；T011 和 T016 保持未完成。

2026-09-08 R4-B2 / **DONE (local requester stream batch)**：Core stream回调已接
operation接受/一次replacement/final，保留前缀和原deadline；generation从请求options派生，
反馈操作授权同源且排除单轮readiness。7 stream cases/190 assertions、2 options/21、
29 regression/695 PASS，共38 cases/906 assertions（分三个运行）；两个实际recovery wire
SDK对照及CLI/loader检查PASS。首轮fixture topology错误已修，原始失败保留。
新ABI构建879.773s，后续增量32.338s/32.730s均PASS；未启用-O2，不计发布/性能资格。
当前无运行中构建或测试；下一步T011 stable epoch/会话，真实Provider重算与T016未关闭。
证据见[R4-B2 Final Local Result](evidence/r4-b2-stream-production-20260908.md#final-local-result)。

2026-09-08 R4-B1 / **DONE (local sampling batch)**：double precision与统一参数校验修复；
dedup/Top-P retained mass已有实现并保留。Spec182Sampling四个真实epoch用例40 assertions
及既有epoch用例28 assertions PASS，独立Python参考一致。增量unit构建33.992s/16.161s、
实际DI共享库构建16.304s PASS。下一步接stream acceptance、TOKEN_FEEDBACK与session；
本局部修复不关闭T010-C或T011整体验收。见 [R4-B1](evidence/r4-b1-sampling-20260908.md)。

2026-09-08 R3-B1 / **DONE (initial-request local batch only)**：配置化 native client、
catalog 与 CLI 已接 Core Begin→ACK→prepare/place/seal/grant/project→commit→Response。
131 DI cases/2975 assertions 的最终通过证据由130个共享PASS与1个SDK派生fixture
修复后的单例PASS组成，另13 Core cases/183 assertions PASS。2 request/4 grant/
7 dataflow（11 endpoints）独立oracle、CLI help/usage/错误schema/加载闭包检查PASS。
保留全部编译/测试首轮失败及重试记录，见 [Final Local Result](evidence/r3-b1-request-lifecycle-20260908.md#final-local-result)。
当前没有运行中的构建或测试；下一步 R4 接 streaming/TOKEN_FEEDBACK/session。
T010整体、T012/T013调用方与旧路径退出、T014–T017最终验收交付保持未完成。

Earlier R3-B1 checkpoints below are historical; the result above supersedes their next steps.

2026-09-08 R3-B1 / **PARTIAL / TESTS_DEFERRED**：默认 operation 已持有提交的
model/input/options/策略，并调用配置的 preparation；取消/超时后不接收编码结果。
后续新增 service/task contract 与兼容 V2 wire 的 client 调用、C++/SDK oracle 用例；
尚未构建/执行；无 runtime 配置的旧入口仍明确失败。新 private owner 涉及 client ABI。
sealer/preparation 的 intent/content 绑定已修复并更新 SDK 对照数据；Core 本地取消
接口及 open/closed 清理用例已编写。配置化 client 已接 Core ACK→native 规划/授权→
commit/response/取消，新增 planner 组合用例；下一步补 CLI/bootstrap 和完整回调用例，
限定编译诊断发现并修复 Core Buffer 转换错误，r2 四源码语法检查 PASS（27.272s），
未链接/执行，不计 ABI 或业务验收。原始 r1/r2 记录均保留。
随后已补 catalog/CLI 与 state binding，并修复 publication manifest 改变后 issuer
allowlist 不匹配的问题；新增源身份策略/负例，尚未编译。现已补
[配置契约](contracts/native-requester-configuration.md)、help 和 catalog 配置正向/负例；
catalog/CLI/planning tests 限定 syntax 全部 PASS（17.330s），未链接/执行。
随后补公开 client→Core Begin→空 ACK/取消→pending 清理用例，新增测试尚未编译/执行。
随后补重新签名 ACK→实际规划/授权→Core commit→成功/错误绑定 Response、commit 后取消、
晚到回调重放用例（传输/source 为 local fixtures，非网络资格）。整批进入
READY_FOR_BATCH_TESTS；fresh configure PASS 5.934s，unit-tests/CLI -j4 build 已启动。
下一步读取构建首边界或执行共享单测与 oracle，不重复启动同一构建。
按同批门完成后统一构建。源码未验收、不创建产品 checkpoint，详见
[本批记录](evidence/r3-b1-request-lifecycle-20260908.md)。

2026-09-08 R2-B6 / **DONE (initial-request local batch only)**：已将 sealed dependencies
与认证 key offers 组合为 group capability 和完整 projections，统一 group rank、
operation index 与身份摘要。首次 test const 编译失败已修复；r2 增量24.302s，
31 cases/1305 assertions PASS，既有7 dataflows/11 endpoints SDK 对照通过。
默认 requester 仍未接通；下一步 R3 将现有组件连接至真实 Core commit/response，
R4 接入 TOKEN_FEEDBACK/streaming，T004/T010/T016 未整体完成。
见 [R2-B6](evidence/r2-b6-group-projection-20260908.md)。

2026-09-08 R2-B5 / **DONE (local batch only)**：NativeGroupKeyAdmission 连接同一 ACK
的 V3 admission 与 key offer 身份/epoch/公钥摘要/endpoint 检查，复用 Core RSA 封装；
实际 coordinator capability 投影和 Provider 解封/HMAC 正负例通过。增量 28.372s、
2 compiles + link，19 cases/1216 assertions PASS。group rank 与 assembly rank 仍需
区分，多 redistribution 的 operation index 和 streaming stride 待统一，默认请求
链尚未闭合。见 [R2-B5](evidence/r2-b5-group-key-admission-20260908.md)。

2026-09-08 R2-B4 / **DONE (local batch only)**：已增加真实 signed request/policy issuer、
authenticated client 与 Core worker publication 实现；sealer→issue→reply verification→
finalize/project→Provider unwrap 的 C++ 组合通过。增量 33.757s、5 compiles + link；
37 cases/672 assertions PASS。离线 oracle backend 参数失败已记录并修复，4 份
规范 wire/签名/解封独立对照 PASS，无重复构建。Core integration 只编写，默认
requester 和实际 model-key/configuration owner 待接线；T005/T010/T016 未关闭。
见 [R2-B4 完整记录](evidence/r2-b4-grant-production-audit-20260908.md)。

2026-09-08 R2-B4 GA-1 / **PARTIAL**：原生 signed request 和 concrete policy/crypto
issuer 已编写，Ed25519/X25519/P-256 recipient 与 Provider unwrap 的 C++ 正负例
已登记；静态审查通过，未构建/测试，源码未提交。继续 GA-2 客户端答复认证与发布，
GA-3 独立对照及组合后统一构建。详见 [R2-B4](evidence/r2-b4-grant-production-audit-20260908.md)。

2026-09-08 R2-B4 production grant audit / **AUDIT_COMPLETE / IMPLEMENTATION_NOT_STARTED**：
canonical C++ 与旧生产 Python 对照确认，T005 不只是等待 sealer 依赖复核：
未签名请求、IssuePort-only authority、未验证答复及未接 Core 的 publication 均待实现。
GA-1/2/3 完整签发/发布/消费批次已登记；旧局部 PASS 保留，不作为真实授权证明。
见 [R2-B4 audit and batch](evidence/r2-b4-grant-production-audit-20260908.md)。

2026-09-08 R2-B3 Projection Builder / **DONE (batch only)**：从 sealed plan/candidate 和
admitted offers 生成执行角色、真实 device snapshot 绑定、application-input/dependency
endpoint、ALL readiness 与 terminal。共享 wire/图校验，支持 rank redistribution，
反馈边不进入单 epoch DAG。首轮增量 build 42.473s、73 cases/1735 assertions PASS；
补充SDK非空数据流检查仅测试文件增量29.547s、1 case/37 assertions，7 dataflows/
11 endpoints 与篡改拒绝通过。T004/T008/T010 未关闭；接续真实 group/grant 和默认
requester 的调用，见 [R2-B3](evidence/r2-b3-projection-builder-20260908.md)。

2026-09-08 R2-B2 State Source Binding / **DONE (batch only)**：candidate 选定前用显式
export mapping 绑定源 boundary 的 state shape/dtype/bytes，并重算完整候选摘要。
真实 causal ONNX 三类状态及 source 重复 operand 回归通过；r1 编译与 r2 graph
拒绝已保留并修复。r3 增量 build 30.522s、71 cases/1649 assertions PASS；Core/UAV
未重编。T008-A 未关闭：下一步实际模型配置调用、角色数据流与 requester 接线；
详见 [R2-B2](evidence/r2-b2-state-source-binding-20260908.md)。

2026-09-08 R2-B1 Preparation Catalog / **DONE (batch only)**：完整身份锁定目录，自动
组合 registry/inspection/role/publisher；实际 inline/external 源、双模型查找、工厂销毁、
原输入修改与取消负例通过。单次增量 -j4 build 30.369s，仅两个对象和 unit-tests 链接；
70 cases/1596 assertions PASS。T008-A 保持 PARTIAL，下一步接入两模型真实目录配置、
Qwen state/source 映射并完成 requester 使用；见 [R2-B1](evidence/r2-b1-preparation-catalog-20260908.md)。

2026-09-08 T003 local acceptance：按原 work-unit LocalChecks 核对 A→B→C，局部卡 DONE；
复用当前源码的 32 C++ cases/1165 assertions PASS，PO-002 留 T016。
R1 source/state 映射阶段出口、T008 组合入口与 T010 默认 requester 仍未完成；
详细 requirement→proof 与剩余 owner 见 [local closure](evidence/t003-local-closure-20260908.md)。

2026-09-08 R1-B5 Canonical Role Preparation / **DONE (batch only)**：Owner 当前执行者。
RP-1：源图检查与不同规划坐标；RP-2：冻结 profile/mapping 的原生 role port；
RP-3：实际 ONNX 字节→角色→preparation 回归。对现有 T003/T008 共享契约修复，
共享增量 -j4 build 46.998s，69 C++ cases/1560 assertions PASS；未重编 Core/UAV。
不放行硬依赖或关闭真实 requester；下一步核对 T003 原卡剩余项并接续生产准备端口，详情与共同验证见
[R1-B5](evidence/r1-b5-canonical-role-preparation-20260908.md)。

2026-09-08 R1-B4 Catalog Task Adapter / **DONE (batch only)**：Owner 当前执行者。
CA-1：immutable 模型目录与有界 opaque/JSON 字节入口；CA-2：实际 registry/preparation
消费及 C++ 正负例。复用已通过 T002-A，不放行 T003/T008 硬门；官方逐成员静态审查后
统一增量 -j4 build，执行 Spec182Preparation/Spec182NativePlanning。
设计与剩余边界见 [R1-B4](evidence/r1-b4-catalog-task-adapter-20260908.md)。
CA-1/CA-2 静态审查后单次增量 build 15.489s、45 cases/944 assertions PASS；
实际 source/catalog→角色生产及默认 requester 仍待实现，不关闭 T003/T008 整卡。

2026-09-08 R1-B3 Native Merge Publication / **DONE (batch only)**：Owner 当前执行者。
成员 NM-1：共享 role codec 与 preparation/publisher 的完整非 ONNX 分支及 C++ 负例；
NM-2：发布后绑定、sealer/Provider 消费与组合验证。批次服务现有 T003-C/T008-A/T004-A
共享契约修复，不放行未完成卡硬依赖。统一使用现有 -j4 build，选择器包含
Preparation、CanonicalPublisher、V3Placement、PlanSealer 与 native plan/Merge 用例；
逐成员静态门、批末统一测试。详见 [batch](evidence/r1-b3-native-merge-20260908.md)。
NM-1/NM-2 r3 build 28.762s、68 cases/800 assertions PASS；r1/r2 fixture 首边界
保留。下一步实际 adapter/catalog 角色生产与 requester 接线；本批不关闭原卡。

2026-09-08 R1-B2 Candidate Role Semantics / **DONE (batch only)**：Owner 当前执行者。
复用 T002-A 与已验证候选契约，修复 T003-C 角色消费边界；不放行 T003-A/B/C
整卡依赖。完整行为出口是候选 postprocessing 字段仅绑定到 result egress，
prepare/publish 共用 validateRoles 拒绝偏离；非 ONNX Merge 发布仍是后续缺口。
成员 CR-1：候选到角色语义绑定与 C++ 篡改负例；CR-2：官方只读静态门、组合审查后
单次共享 -j4 build，执行 Spec182Preparation/Spec182CanonicalPublisher/Spec182V3Placement。
首次 fixture 失败已保留，补齐 ingress/egress 后 r2 build 16.033s、26 cases/546 assertions
PASS；T003-C 与生产请求仍 PARTIAL，证据见 [R1-B2](evidence/r1-b2-role-semantics-20260908.md)。
用户清理请求已完成，见 [disk cleanup](evidence/disk-cleanup-20260908.md)。

2026-09-08 Native Test Ownership / **DONE (policy only)**：按用户要求，主要
DI 行为测试使用 C++ 直接调用生产库；Python 用于可选绑定兼容、离线独立 oracle
及外部测试设施。spec FR-013、plan 和 [test ownership](contracts/proof-design.md#native-test-ownership)
已同步。各实现批次迁移所属行为测试，T013/T015 核对覆盖，T016 保留全部真实门。
不删除旧测试或重写既有证据，不宣称测试迁移已全部完成；产品任务状态不变。
文档验证及 diff 检查 PASS，见 [check](../../.codex-tmp/spec182-native-test-policy/design.json)。

2026-09-08 R1-B1 Qwen Metadata to Candidate / **DONE (batch only)**：用户新设“完成
Spec182 全部任务”目标，恢复产品执行。本批属于 T003-A，交付 maintained 元数据
生成语义图并直接进入原生 splitter 的完整路径；不将 semantic layer ID 当作 ONNX
节点索引。Owner：当前执行者。前置 T002-A 已有验收；后续 T003-B/C 硬门不变。

| R1-B1 member | Status | Progress / exit |
| --- | --- | --- |
| QM-1 Metadata graph | DONE | 图、摘要、全部边/input-output/maxNodes；维护 builder 对照和官方静态门通过 |
| QM-2 Candidate consumption | DONE | 实际 metadata 入口→splitter，完整 Python 候选对照及错误 revision/graph/预算负例通过 |
| QM-3 Batch validation | DONE | 官方静态门及整批审查后，单次增量 -j4 build 19.721s；NativePlanning/V3Placement 32 cases/1165 assertions PASS |

详细边界与唯一证据见 [R1-B1](evidence/r1-b1-qwen-metadata-20260908.md)。批内静态
通过但未测试保持 PARTIAL；当前阶段表继续汇总原卡，不以子步骤数计算完成率。

2026-09-08 Remaining Production Chain Replan / **DONE (documentation only)**：
用户要求暂停新增实现。已核对默认 requester、准备 port、model adapter、seal/grant
字段与 Core/provider 接线，保留已验证成果，将未完成卡映射为七个能力阶段。
见 [review and batch exits](evidence/production-chain-replan-20260908.md)。本轮无产品
源码/构建/实验，不新增产品 DONE；用户明确恢复后才领取下一批。

| Remaining stage | Current state | Member cards / next boundary |
| --- | --- | --- |
| R1 Model and Candidate Closure | PARTIAL | T003-A/B/C；复用 YOLO PASS，关闭 Qwen 映射及剩余策略验收 |
| R2 Preparation to Authorized Plan | PARTIAL | T008-A/B、T004-A、T005-A/B；实际 adapter/catalog/role 字段到授权计划 |
| R3 Complete Request | PARTIAL | T010-A/B；默认请求状态机和成功/失败/取消闭合 |
| R4 Streaming and Conversation | PARTIAL | T010-C、T011-A/B/C；流、生成与恢复接入同一 owner |
| R5 Same-Library Callers | PARTIAL | T012-A/B、T013-A；绑定、新 ABI 与 maintained callers |
| R6 Retirement and Qualification Tools | PARTIAL | T013-B、T014-A/B、T015-A；退出、harness 和跨批审查 |
| R7 Final Qualification and Delivery | NOT_STARTED | T016-A、T017-A；完整原有资格与交付 |

本表汇总已有卡的剩余工作，不是第二套任务完成计数，也不直接定义编译次数。
具体执行批次按 [selection rules](evidence/production-chain-replan-20260908.md#executable-batch-selection)
在领取时登记成员、行为出口、依赖、共享构建/选择器和负责人；R1/R2/R5 不机械各做
一个大批。阶段硬前置及最终证明责任保留。暂停调度不改卡的真实状态。

2026-09-08 D-SKILL-BATCH / **DONE**：工作流按逻辑批次修订，逐小任务静态门、批末流程门与统一构建测试；定向文档检查 PASS，安装入口验证器限制及替代核对见 [workflow evidence](evidence/skill-batch-workflow-20260908.md)。本单元不重排产品任务或改变其验收状态。

2026-09-08 Native test ownership / **DONE**：共享 `batch-quality-gates` 和
`speckit-code-design` 明确 NDNSF-DI 原生运行时、协议、状态机、并发、密码和模型行为
必须由直接调用生产 C++ target 的 suite/selector 验收；speckit-plan/tasks/implement/
analyze/audit 的仓库入口同步检查该约束。Python focused test 只能证明 binding/facade、
离线 oracle、配置拒绝或外部设施边界；R5-B2 的 24 binding / 31 compatibility cases
按此分类，真实 native request parity 仍为 PARTIAL。

2026-09-08 D-SKILL-BATCH reusable quality gates / **DONE**：新增共享
`batch-quality-gates.md`，并将其接入 specify/clarify/plan/tasks/analyze/audit/
implement/converge/checklist 入口及 Spec 模板。定向链接、字段和职责检查 PASS；
本轮只改技能与模板，不执行产品构建或测试，不改变 R4-B4/T011-C/T016 的 PARTIAL 边界。

2026-09-08 B-G1-YOLO-SEMANTIC / **PARTIAL**：生产接线审查确认注册语义名称与
planning IDs 尚未映射，完整 catalog interface 也没有 C++ 消费入口。已在 plan
登记 YS-1/YS-2/YS-3 的修复批次、实现依赖和统一验证范围，见
[batch evidence](evidence/t003-yolo-semantic-batch-20260908.md)。YS-1/YS-2/YS-3 已实现并
完成静态门，真实 Python oracle 与生产 factory/enumerate 回归 52 cases/1277
assertions PASS；fresh -j4 ABI tree build PASS（6m48.938s）。全局文档检查的旧
workflow 引用已按共享规则位置修复，独立重跑 PASS；T008-A 硬前置保留，未新增 DONE。

2026-09-08 T003 ordinary candidate publication / **PARTIAL**：修复发布阶段对普通
候选省略 rank map 的不一致处理，见 [rank evidence](evidence/t003-preparation-rank-20260908.md)。
preparation 和实际 publisher 同类缺陷均修复，114 cases/2248 assertions PASS；
完整 requester 接线仍未完成，没有新增 DONE。

2026-09-08 T003 owned ONNX graph inspection / **PARTIAL**：增加实际 ONNX bytes 到
规划图、语义名称、原始节点索引和 metadata 的原生入口，保留独立 canonical assembly
identity。六组真实 Python ONNX 对照及相关回归 114 cases/2212 assertions PASS；
两轮 fixture 问题已修复并保留原始结果，最终 r3 增量 -j4 build PASS（23.480s），
binding 语法与文档检查 PASS。见 [inspection evidence](evidence/t003-onnx-graph-inspection-20260908.md)。
没有新增 DONE；默认 NativeModelAdapter/InspectPort、YOLO catalog/interface 与 Qwen
semantic layer 映射仍待实现，不能把独立入口 PASS 当作 requester 已接通。

2026-09-08 T003 complete candidate identity / **PARTIAL**：补全部候选字段及规范 JSON/摘要，
复用 RedistributionSpec 校验 hybrid cover，两个默认 splitter 和 preparation/V3 输入同步。
r1 fresh ABI -j4 build PASS（406.080s），Qwen 额外 ingress 与 Python 不符导致 1 case
失败；修复后 r2 build PASS（26.152s），87 cases/1246 assertions PASS，binding 语法
与文档检查 PASS。见 [candidate evidence](evidence/t003-candidate-identity-20260908.md)。
完整候选身份的本批检查通过，不新增 DONE；下一步关闭真实 graph adapter/catalog/interface
与 canonical node 映射，再推进 requester 接线。

2026-09-08 D-DESIGN-R3 / **PASS**：修正 grant/流/目录/UAV 门禁叙述，重写 23 组关键契约，
第 53 章补生成状态、KV、journal。双 PDF 82/87 页，5 项工具回归、签名/可读参考、
460/350 文件还原、目录/字体/版面与 67 个原主题追踪 PASS；并行源码变化触发旧基线拒绝后
已重新采样。见 [R3 evidence](evidence/design-r3-20260908.md)。不改变产品任务状态。

2026-09-08 T003 shared resource contract / **PARTIAL**：补五类 nullable 预算、KV、
完整资源规范 JSON，两个 splitter 与 placement/preparation 同步；12 组维护 Python
对照，fresh ABI -j4 build PASS（400.678s），84 cases/1119 assertions PASS，
binding 语法编译与文档检查 PASS。见 [resource evidence](evidence/t003-resource-contract-20260908.md)。
没有新增 DONE；完整 candidate 身份、graph adapter/catalog 映射及 requester 主链仍未闭合。

2026-09-08 D-DESIGN-CHAPTER-AUDIT / **PASS（审阅）**：逐章检查当前/目标全部章节；
第 53 章缺核心 API/调用流程，第 59 章 grant 输入解释错误，目标历史叙述需勘误。
文档内容 **NEEDS_REVISION**；清单记录每章缺口与补写标准，PDF/产品源码本轮不改。
见 [chapter audit](evidence/design-chapter-audit-20260908.md)。既有文档技术 PASS 不代表语义完整。

2026-09-07 T003 YOLO fragment identity / **PARTIAL**：修复 fragment 规范输入，
强制注册摘要、保留节点顺序，并对齐 backend/余量/原子候选 Merge 语义。
-j4 build PASS（74.968s），82 cases/1084 assertions PASS；真实 Python splitter
对照与 T003-B 五个具名用例索引已同步。见 [fragment evidence](evidence/t003-yolo-fragment-20260907.md)。
完整候选摘要、catalog/interface 仍待闭合，不新增 DONE。

2026-09-07 T003 complete model descriptors / **PARTIAL**：补完整 adapter/model
规范 JSON/摘要，sourceRevision 下沉共享描述符，prepare/inspect 比较完整模型身份。
r1 终止日志保留；r2 新 ABI -j4 build PASS（403.580s），r3 最终增量检查 PASS
（6.591s），81 cases/1067 assertions PASS，binding 源码语法编译 PASS。见
[descriptor evidence](evidence/t003-model-descriptor-20260907.md)。完整 candidate 摘要、
graph adapter 绑定与实际 inspection/requester 仍待完成，不新增 DONE。

2026-09-07 D-DESIGN-R2 / **PASS**：当前/目标 66/69 页，4 项工具回归、API、460/350 文件源码还原、
PDF 构建身份/目录/字体/版面通过。重复声明 ID 与长标识符排版失败已修复并保留；
BC-01 至 BC-04 已补，TG-01 至 TG-05 保持 PLANNED；见 [Design R2](evidence/design-r2-20260907.md)。不改变产品任务状态。

2026-09-07 T003 node/state contracts / **PARTIAL**：补共享候选 nodeRoles 与
role state I/O，Qwen/YOLO 同一 validator 校验完整节点/角色覆盖及无环依赖。
r1 新 ABI -j4 build PASS（410.204s）；空角色负例 helper 除零已修复，r2 build
PASS（21.493s），69 cases/972 assertions PASS，原始失败保留。见
[node/state evidence](evidence/t003-node-state-contracts-20260907.md)。
planning/canonical ONNX 节点不混用；完整 descriptor/规范摘要及装配映射仍待完成。

2026-09-07 T003 candidate validation / **PARTIAL**：补共享候选的合法 cut、依赖
tensor 集合与 rank-artifact 覆盖校验。r1 旧 V3 fixture 重复 rank artifact 的失败
已修复并保留日志；r2 -j4 build PASS（32.605s），68 cases/836 assertions PASS，
含 SDK 多 rank core 对照；见 [candidate validation](evidence/t003-candidate-validation-20260907.md)。
执行归属 T003-A/B/C；完整 node/state/interface 和规范候选摘要仍待完成，不新增 DONE。

2026-09-07 D-DESIGN-API / **PASS**：完成声明参考和 23 组中文契约，两份 PDF 各 63 页，
58 目录项/正文/字体/版面、295 文件声明与 350 文件源码还原检查 PASS；AGENTS 本机规则与 MANAGEMENT.md 已同步。
见 [API guide evidence](evidence/design-api-guide-20260907.md)。不关闭 Spec182 功能任务。

2026-09-07 T003 graph edge audit / **PARTIAL**：补共享真实 tensor edges 与
YOLO 分支依赖/预算，修复 Qwen fragment/backend；T003-A/B 从 DONE 撤回 PARTIAL。
新 ABI -j4 build PASS（376.755s），补充修复后增量 build PASS（18.877s），
67 cases/818 assertions PASS。见 [tensor edge evidence](evidence/t003-yolo-tensor-edges-20260907.md)。
完整 candidate/state/interface/identity 及 inspection/requester 接线仍未完成。

2026-09-07 T008 Core artifact publisher / **PARTIAL**：新增可直接注入 ArtifactPort
的原生 Core 发布 owner，验证真实源字节/ONNX 身份，保留 Core I/O 与加密/分段所有权。
r1 -j4 build PASS（57.737s）；补齐 Core 失败原因保留后 r2 build PASS（32.313s），
65 cases/779 assertions PASS，其中 publisher 7 cases/69 assertions；见
[Core publisher evidence](evidence/t008-core-artifact-publisher-20260907.md)。LocalMock-key
加密发布不代表网络/权限资格；实际 source inspection、模型配置与 requester 仍待接线。

2026-09-07 T008 publication recertification / **PARTIAL**：验证发布后的业务 root
并重新认证角色，保留原 proposal 验证，再检查 final recipe 的 offer 可行性。
r1 新 ABI build PASS（302.273s）；稳定名字 fixture 拼接 // 的失败已保留并修正，
r2 build PASS（24.664s），58 cases/710 assertions PASS。见
[publication evidence](evidence/t008-publication-recertification-20260907.md)。真实本地 source
inspection、Core publisher/requester 接线与完整验收仍未完成，不新增 DONE。

2026-09-07 T008 graph identity spaces / **PARTIAL**：维护路径源码确认 planning 与
canonical ONNX graph 不同；原生准备/封存已拆分绑定，新增真实 SDK 不同摘要 oracle。
新 ABI build PASS（288.247s）；负例异常类型期待修正后，r3 增量 build PASS（21.806s），
58 cases/594 assertions PASS，r2 失败日志保留。见
[graph identity evidence](evidence/t008-graph-identity-spaces-20260907.md)。发布前后 manifest
及 source/fetch 身份转换仍待实现，不新增 DONE。

2026-09-07 T008 V3 artifact publication / **PARTIAL**：publication port 改为实际 candidate
与选定 V3 roles，补齐返回 artifact digest 与角色契约的逐项匹配；保留 request/control
与取消边界。新 ABI build PASS（280.317s）；修正旧 sealer fixture 的预算后，r2 增量
build PASS（16.139s）、58 cases/553 assertions PASS，首轮失败日志保留。见
[V3 artifact publication](evidence/t008-v3-artifact-publication-20260907.md)。未新增 DONE。

2026-09-07 D-DESIGN-R0 / **PASS**：Core/UAV/DI/Repo 完整中文双 PDF 各 35 页，
新增 Spec 设计变更记录；用户已授权 Design 完整入 Git。更新后双 PDF 编译、正文一致、字体/版面及 94 文件基线检查通过；
见 [design evidence](evidence/design-pdf-baseline-20260907.md)。创建文档本地 checkpoint，不纳入并行源码修改。
无 runtime 修改、无协议测试或资格验收结论。

2026-09-07 T003 V3 strategy interface / **PARTIAL**：placement 基类改为必需的完整 V3
虚接口，默认策略与自定义策略共享调用契约；旧简化入口不再由基类提供。
新 ABI -j4 build PASS（310.850s），58 cases/498 assertions PASS，
见 [V3 strategy interface](evidence/t003-v3-strategy-interface-20260907.md)。
未新增 DONE，实际 requester 调用仍待接线。

2026-09-07 T004 V3 sealer bridge / **PARTIAL**：新增完整 proposal/admitted offer 直连
sealCore 和 grantView，复用角色及设备可行性检查，不再回填旧简化 view。-j4 build PASS
（21.173s），相关 55 cases/482 assertions PASS；
见 [V3 sealer bridge](evidence/t004-v3-sealer-bridge-20260907.md)。真实 requester/catalog 主链仍缺。

2026-09-07 T008 inspection/roles / **PARTIAL**：删除合成 catalog 来源，inspection port
返回实际来源与 manifest；保留完整请求模型，新增 prepareRoles 绑定候选角色。新 ABI -j4
构建 PASS（300.816s），相关 46 cases/403 assertions PASS，见
[inspection evidence](evidence/t008-inspection-roles-20260907.md)。真实 I/O 与 requester 主链仍未闭合。

2026-09-07 T003 V3 role placement / **PARTIAL**：新增完整 role/rank 与 admitted offer
规划入口，按真实 device/resources/exact residency 选择 Provider；r1 测试宏编译失败已修复，
r4 -j4 构建 PASS（22.560s），相关 34 cases/405 assertions PASS。
旧简化 candidate metadata 与 requester 接线仍缺，见 [V3 placement](evidence/t003-v3-placement-20260907.md)。

2026-09-07 T008 Core offer admission / **PARTIAL**：已替换 caller evidence DTO 和 policy
伪能力入口，新增 candidate policy/key registry、Core ACK payload 与 Ed25519 验证。
SDK signed fixture authoring r1 import 失败，补齐 Repo Python 路径后 r2 PASS；新 ABI -j4
构建 PASS（304.124s），相关 10 cases/2158 assertions PASS。
见 [Core offer evidence](evidence/t008-core-offer-admission-20260907.md)。真实订阅与 planner 接线未闭合。

2026-09-07 T008 observed offer / **PARTIAL**：确认 admission 从 policy 合成观测能力且缺
offer signature 验证；新增无认证权力的 V3 观测解码与真实 SDK oracle，-j4 增量构建
PASS（14.39s），相关 20 cases/2154 assertions PASS。
见 [offer evidence](evidence/t008-observed-offer-20260907.md)。尚未接真实 ACK 或形成可信 planning view。

2026-09-07 T004 sealer integration / **PARTIAL**：已移除七字段 encode 与硬编码 CPU project；
新增必填 assembly/execution/dataflow/device 契约，core/final 摘要改用 canonical JSON，
真实 Python SDK oracle 摘要对照 PASS；新 ABI -j4 构建 PASS（289.48s），62 cases/2358 assertions PASS。
见 [integrated sealer evidence](evidence/t004-sealer-integrated-20260907.md)。真实 planner metadata、
完整 generation/device/rank oracle 与 requester 主链仍待闭合，尚未计整卡完成。

2026-09-07 T004 typed shape / **PARTIAL**：Selection 与 worker 保留 int64/string 维度类型。
新 ABI 构建 r1/r2 消费者编译失败已修复；r3 -j4 构建 PASS（119.84s），核心 73 cases/
2491 assertions、worker metadata 4 cases/30 assertions PASS。完整 projection encoder→生产
parser 往返已通过，但旧 sealer 主链仍未切换；见 [typed shape evidence](evidence/t004-typed-shape-20260907.md)。

2026-09-07 T004 canonical JSON foundation / **PARTIAL**：新增固定原生 typed JSON 库和内部 helper；
r1 DBL_MAX stream failbit 缺陷已修复，r2 构建 PASS，冻结 Python 对照 2/2 cases、2028 assertions PASS。
见 [canonical evidence](evidence/t004-canonical-json-20260907.md)。
完整 core/Selection encoder 尚未接入，不计整卡完成。

2026-09-07 T003 placement repair / **PARTIAL**：逐角色独立 Provider 与目标工件提示排序已实现。
r1 fixture 失败已修复；r2 -j4 构建 PASS（14.68s），37/37 cases、290/290 assertions PASS。
首边界、原始记录与剩余义务见 [placement evidence](evidence/t003-role-placement-20260907.md)。
完整 device/rank/residency 证明尚缺，T003-C 与依赖卡保持 PARTIAL。

2026-09-07 T004 artifact/grant repair / **PARTIAL**：真实工件和完整 grant 输入已接入；
全新 ABI 消费者 -j4 构建 PASS（285.07s），三个相关 suite 33 cases、269 assertions PASS。
见 [repair evidence](evidence/t004-artifact-grant-bindings-20260907.md)。T003-C 将全部角色分配给同一
Provider，并按无关 residency 数量排序；本轮重开该卡及依赖完成状态，旧局部测试仅作历史。
下一步修复逐角色分配与真实工件 residency，再完成 canonical wire。

2026-09-07 T004 actual wire audit / **REOPENED / PARTIAL**：原生 encode→生产 parser
最小诊断重现 schema mismatch；另发现工件摘要来自角色名、core digest 为自定义字符串、
grantView 缺 request/身份/有效期、projection 硬编码 CPU 与单 role。见
[A8-01 evidence](evidence/t004-wire-reopened-20260907.md)。T004-A DONE 回退，父 T004
取消勾选；T005-A/B 随依赖回退 PARTIAL、父 T005 取消勾选，局部测试结果保持历史有效。
当前 18/36 子卡 DONE、父任务 3/17，不重写旧局部测试证据。下一步先按既有
wire/认证工件/完整角色契约修复 T004，再继续 T010；不是给无效片段简单补 schema 标记。

2026-09-07 T010-A Core I/O / **PARTIAL**：已提供原 Face 调度入口，并禁止在 Core
I/O 线程阻塞 result；operation 保持 user 寿命，client close 不关闭共享 Core。
-j4 必要构建 PASS（87.60s），ClientState 11/11、既有 client 2/2 PASS；见
[Core I/O evidence](evidence/t010-a-core-io-20260907.md)。后续完整请求接线前，先纠正
T004 的七字段片段与生产 Selection parser 不兼容、伪工件摘要等前置缺口。

2026-09-07 T010-A active deadline / **PARTIAL**：独立 steady timer 保证排队或慢通知
不延后请求终态；terminal 移除定时项，测试注入收为 private，补严格 ACK/总预算和
wait 参数检查及异步异常隔离。-j4 增量构建 PASS（21.40s），ClientState 8/8、
既有 client 2/2 PASS；design validator、diff 检查 PASS。首次错误 suite filter 未运行
用例，原始失败及修正记录见 [deadline evidence](evidence/t010-a-deadline-20260907.md)。
下步补 Core I/O 调度与完整 request 接线；T010-A/B/C 和最终资格仍未完成。

2026-09-07 Actual progress audit / **T010-A PARTIAL**：21/36 子卡 DONE、父任务 5/17；
未以子卡数量关闭仍有集成编写/接线义务的父任务。requester 仍有 pipeline-not-ready 分支。
接续已有串行执行器切片，修复观察者同步阻塞 cancel/observe，通知改为独立串行队列，
解除 idle worker 自持有；提交异常转失败 handle。-j4 必要构建 PASS，ClientState 3/3、既有 client 2/2 PASS。
见 [audit evidence](evidence/t010-a-lifecycle-audit-20260907.md)。下一步完成 T010-A/B 的 Core
生产接线和成功/取消/deadline 竞争，继续 stream、会话、绑定、迁移及 T015/T016/T017。


2026-09-07 Build parallelism / **PASS**：用户授权本开发机（实查 6 逻辑 CPU、约 12 GB RAM）
后续原生构建默认 -j4，已同步 AGENTS/CLAUDE、plan 与当前执行/验证指引。
现场构建已在用 -j4；只读短样本未见持续换页，未做加速比/全程峰值验收，也未另启构建。
资源快照、使用范围和降档规则见 [build policy](../../docs/native-build-parallelism.md)。
文档 validator 与 diff 检查 PASS；新增说明已显式解除 docs 默认忽略规则，随仓库保存。
本单元为工作流文档更新，不改产品任务状态；下一步由现有构建执行者记录结果和资源，再继续当前验收。

2026-09-07 T006-B Certified Extraction and Wire / **T006-B DONE（父 T006 待
T006-C/D）**：按 T006-B 卡 Verify（CPP(Spec182OnnxExtraction/*)）在 planned
文件 `tests/unit-tests/di-native-onnx-recipe.t.cpp`（case-manifest 登记）建
suite `Spec182OnnxExtraction` 11 cases 全绿：4 accept 逐字节复现冻结 python
wire（inline/external/rank-subset/function-local-domain）+ 独立
sha256(modelBytes)==modelDigest 交叉检查；7 reject 逐字面 exact reason
family（RECIPE/GRAPH/NODE_COVER/IO_DTYPE/LAYER_RANGE，r7 ghost-input 的
python 构造期 IO_CONTRACT 与 native S5 GRAPH 差异已留档）。驱动
NativeOnnxRecipeAssembler 的 S3–S7 certified pipeline：官方 ONNX 1.17
full-protobuf 统一（--onnx-prefix，含 --with-tests/--nac-abe/--ndn-svs 完整
configure）。关键修正：(1) data_location proto3-optional presence 奇点——
external 源行与 python 冻结 wire 逐字节相同，byteParity False→True 重新冻结
（sha 6f289a01→77300e13，11 行 diff 仅该标志），suite 断言升级 byte equality；
(2) If fixture 需 set_type(GRAPH) + cond 零维 shape（full checker 要求）；
(3) 既有 Spec182NativeAssembly 3 cases（含 nested-external If）与
Spec182OnnxIdentity 11 cases 回归全绿；完整回归 5 失败为 TPM/NFD 环境性、
与 ONNX 改动不可达。evidence
[t006-b](evidence/t006-b-certified-extraction-wire-20260907.md)。
下一步：T006-C（Bounded Native Worker，依赖 T006-B 已满足）。

2026-09-07 T006-A Canonical Source Identity / **T006-A DONE（父 T006 待
T006-B/C/D）**：按 T006-A 卡 Verify（CPP(Spec182OnnxIdentity/*)）在 manifest
登记文件 `tests/unit-tests/di-native-assembly.t.cpp`（existingSuite
Spec182NativeAssembly 3 cases 原样回归）建 suite `Spec182OnnxIdentity`
11 cases 全绿：24 v1 + 14 extended accepted 全模型 golden（graphDigest/
initializerDigest/contentDigest/modelDigest/modelHex）逐字段复现、2 rejected
逐字面一致；typed/raw pair 摘要恒等（packing 恒等冻结）；v2 per-tensor
12 accepted payloadHex/byteLength 逐字节 + 5 拒绝；bf16 两编码归一；revision
分类（STRING/BF16-raw/typed-complex→2，external 先内联再分类）与 v2
descriptor binding 门（rev-2 缺 v2 descriptor 拒绝、rev-1 legacy 兼容）；
external S2 规则（同相对 location、offset 有界、无 length/length "0"
rest-from-offset、绝对/../双向 binding/双 location 拒绝）+ function
attribute 深度扫描内联等价；overflow/负 dim/限额/垃圾 parse 边界拒绝。
生产 seam：canonicalOnnxSourceIdentity（owned source + 内存 external
校验 + 原图 identity——parse 后无任何 shape inference）+ 版本化
normalization（normalizedOnnxInitializerPayload/onnxInitializerNormalization
Revision/checkOnnxAssemblerDescriptorBinding，hpp 无 onnx/protobuf 类型）。
实现期三处修正均有绿测兜底：graphFactsJson hex 缺 JSON 引号（python
reference quoted-string 对照，修后 38 全模型 graphDigest 全对）、INT4/UINT4
raw 展开（low-nibble-first、INT4 符号扩展，generator 位型为证）、两测试侧
构造（18-byte weights；initializer-limit 用例过 source 门后精确触发）。
execution-units U 路径差异按 registry 约定留档；无 wscript 改动。
evidence [t006-a](evidence/t006-a-canonical-source-identity-20260907.md)。
下一步：T006-B（Certified Extraction and Wire，依赖 T006-A 已满足）。

2026-09-07 T005-B Requester Grant Publication / **T005-B DONE、父 T005 DONE**：
按 T005-B 卡 Verify（CPP(Spec182GrantClient/*)）在 manifest 登记文件
`tests/unit-tests/di-native-grant-client.t.cpp` 建 suite `Spec182GrantClient`：
既有 2 个 module-level cases 迁入（名不变）+ 6 个新 cases 全绿 —— 构造门；
view 完整性先于任何端口副作用（issue/publish 计数 0）；已过 deadline 在
fence（原因码族、零副作用，cancel/expired-wait 不复活）；已过期 grant 在
authority expiry 边界拒绝（deadline 仍在未来 → 族前缀只能来自 issue 前检查、
policy port 未运行）；canonical exact-name 字面组件布局 + model-manifest→
model digest 回落 + 同向量 determinism（issue=2/publish=2 恰好一次各）；
错名 publication 族拒绝且恰好消费一次、重试为全新尝试、无 pending 状态可
复活（runtime-boundaries"只消费/忽略，不复活"行为面）。生产代码本卡无改动
（既有切片语义与测试一致）；issue 后二次 deadline 复检无时钟注入不可确定性
触发（T010/T016 executor 层覆盖）。集成：`Spec182GrantClientFlow`（
tests/integration-tests/di-native-requester-grant.t.cpp，wscript 注册 +
di_integration_sources 补 NativeGrantClient.cpp）1 case 全绿 —— 真实
ServiceUser::publishSignedAppData 发布 + exact-name fetch replay，KeyLocator==
requester 证书、content 逐字节一致；authority crypto 真验证/Provider 消费 T016。
execution-units U/di-native-grant.t.cpp 与 manifest 文件差异按 registry 约定
留档。回归（Spec182GrantAuthority/PlanSealer/NativePlanning）全绿。
evidence [t005-b](evidence/t005-b-requester-grant-20260907.md)。下一步：T006-A
（Canonical Source Identity，依赖 T002-A 已满足）。

2026-09-07 T005-A InProcess Authority / **T005-A DONE（父 T005 待 T005-B）**：
按 T005-A 卡 Verify（CPP(Spec182GrantAuthority/*)）在 planned 文件
`tests/unit-tests/di-native-grant.t.cpp` 登记 suite `Spec182GrantAuthority`
6 cases 全绿（固定 request/时钟向量、expiry/自授/身份不完整/issuer
不完整原因码族拒绝、注入 policy 拒绝原样传播、空 issue port 构造拒绝）；
issue() 补 requester==provider 自授权限拒绝（Python frozen issue 对照，
Steps"拒绝 caller 自授权限"）；crypto 面经注入 IssuePort，未新增网络
authority/自写密码算法。NativeArtifactPolicyAuthority 为既有切片
（NativeGrantClient.hpp/.cpp 同文件、T002-A 已安装），planned 独立文件
拆分按 registry 复用约定不重做。回归（grant-client 2 cases +
Spec182PlanSealer + Spec182NativePlanning）全绿。evidence
[t005-a](evidence/t005-a-inprocess-authority-20260907.md)。下一步：T005-B
（Requester Grant Publication，依赖已满足）。

2026-09-07 T004-A Canonical Plan Sealing / **T004-A DONE、父 T004 DONE**：
按 T004-A 卡 Verify（CPP(Spec182PlanSealer/*)）在 planned 文件
`tests/unit-tests/di-native-plan-sealer.t.cpp` 登记 suite `Spec182PlanSealer`
7 cases 全绿（canonical 封印 + M22 单源投影、encode 固定 7-key canonical
片段与布局字面量/独立 JSON oracle 一致、逐维度篡改敏感、错误 endpoint/
缺 grant/错 ACK digest 首边界拒绝、plaintext 无 grant 封面、encode 拒绝面
与 canonical 转义）；reconfigure 使新 .t.cpp 进入 unit-tests ant_glob
（configure rc=0），回归两 suite rc=0。修复
NativePlanSealer.cpp::quote() 控制字符 canonical 转义（escape case 发现的
缺陷；可达 ASCII 输入字节零变化）。核心密封字节被真实 Core/Provider
parser 消费、planDigest 密码学再校验、逐 role 投影迭代留 T010/T016。
evidence [t004-a](evidence/t004-a-plan-sealer-20260907.md)。下一步：T005-A
（InProcess Authority，依赖已满足）。

按 T001-C 卡 Verify（DOC；从实际 Waf 注册推导命令；planned 测试全带 author owner；
父 T001 完整验收满足）完成。已把 [case-manifest](../../tests/fixtures/spec182/case-manifest.json)
（schema spec182-case-manifest-v1）写入 tests/fixtures/spec182/：23 张实现卡各一个
selector（与 execution-units Verify 的 CPP suite 名逐一对照，无重复），owning 文件
（planned 文件标注 "(planned)"）、现有 suite/case 计数原位登记、layers 与
executeOwner 全部显式；python kexpr（T012--T014 六个）与 system entries
（closure runner、MiniNDN、installed-consumer）同步冻结。run identity 为实际注册：
tests/wscript `unit-tests` program（ant_glob unit-tests/**/*.cpp excl sanitizer）→
`build/unit-tests`；`./waf build --targets=unit-tests -j2` 构建、Boost.Test
`--run_test=<Case>` 选择具名 case、`--list_content` 确认注册非空。契约同步：
proof-design.md Rev 7（冻结句 + L0 installed-library 命令及 NAC-ABE 5 符号 gate）、
code-design.md Rev 8（Open Questions O-002/O-004 → CLOSED，表内已无 OPEN 项）、
work-units.md Rev 8、spark-execution.md 冻结句。check-prerequisites/validate_design/
git diff --check 见 [c-freeze evidence](evidence/t001-c-freeze-20260907.md)。
下一步：NAC-ABE ABI（5 符号）匹配后重跑 T002-A 首次 L0；T002-A 依赖现已满足，
当前 BLOCKED 仅剩工具链缺口。

2026-09-07 T001-A/B closure / **T001-A DONE、T001-B DONE、T001-C READY**：
按 T001-A/B 卡 Verify（DOC + 依赖契约/双向映射核对）完成设计关闭，证据
[closure](evidence/t001-ab-closure-20260907.md)。实际执行与核对：ONNX probe
4/4 与 tokenizer ABI probe 84 exact+14 negative 在现工具链上全新复现 PASS；
product tokenizer bridge `--release --locked -j2` rc=0（raw
`.codex-tmp/spec182-t001-dependencies/tokenizer-r2/`），failure-log 的 Cargo
exit127 边界已在该独立 rust-prefix（rustc/cargo 1.90.0）上解决；identity 向量
24+16+17 与设计声明及冻结 sha256 一致，A7-07 缺陷诊断保留原位。O-004 收口
处置写入 runtime-boundaries.md（Rev 8）与 symbol-design.md（C21/Readiness），
registration generation/late ACK/Selection/共享 lease 设计已在 lifecycle 设计
冻结。字段/方法/错误 parity 按各契约原文归 T012 及 owner 任务。产品构建仍被
NAC-ABE ABI 缺口阻塞（下一个工具链边界）。
下一步：T001-C Dispatch and Selector Freeze（依赖已满足）。


2026-09-07 O-004 mapping closure / **T001-B IN_PROGRESS**：formal `api` 的 18 项
`UNREVIEWED` 全部完成 owner 语义映射（`PARTIAL_EXISTING_TYPE 10`、
`PLANNED_TYPE 17`、`UNREVIEWED 0`），分类写入
`checklists/build_api_migration_manifest.py` 的 `API_MAPPING_OVERRIDES`
（`InferenceApplication→NativeInferenceClient/NativeConversationCoordinator`、
`RequestRef→NativeInferenceHandle` alias、部署契约十项 → T008/T010
`runtime-boundaries.md#cd-013`、`ProviderDeploymentOffer(s)→NativeProviderPlanningView`、
`ModelIntent/OptimizationObjective→T003 策略输入、RequestContract/
RequestableDeployment→T010 请求契约），manifest 按当前 source commit 重新生成
（344 项），`public-api-migration-review.md` 状态更新为 O-004 MAPPED
（parity open）。字段/方法/错误 parity 与真实调用方逐项核对仍属 T012 及对应
owner 任务义务；O-004 静态映射收口不关闭 T001 或任何产品任务。
下一步：T001-C selector/build identity/case manifest 冻结（依赖 T001-A/B
完整验收），然后工具链（Cargo 安装、NAC-ABE ABI 匹配）→ T009 → T010。


2026-09-07 Skill surface / **PASS**：按用户要求精简共享个人技能 105→18，GSD 69 个技能、
34 个 agent 注册和 4 个 hook 退出活动配置，原文件留本机归档；项目 12 个 Spec Kit 技能保持。
constitution 1.5.0 改由 Spec Kit 进度/证据负责多阶段恢复，历史 `.planning` 保留。
配置语义核对通过；已运行客户端需重启才能卸载旧 agent/hook，未声称本会话已刷新。
产品状态不变；详见 [registry evidence](evidence/task-progress-registry-20260907.md#skill-surface)。
下一步重启客户端核对精简目录，然后继续 T001 收口。

2026-09-07 Native boundary follow-up / **T001 IN_PROGRESS**：补齐 ONNX
protobuf 生成源到 provider、fault-provider、assembly-parity、smoke 和
integration 的 Waf 闭包（`76d074af`），移除装配器废弃 JSON/路径 helper
（`e0a4e248`）；Python 扩展不再重复编译 `NativeGrantVerifier.cpp`，只链接
安装的 DI 库（`2eb259f7`）。嵌套 graph external initializer 的绑定/内联和
超大输入上限已由 `b4f02615` 覆盖。相关静态检查及 Spec182 Python 门禁仍
通过；Cargo 缺失、NAC-ABE ABI mismatch 和完整请求编排未解决，产品仍 **0/17**。
下一步在匹配工具链上完成 bridge/全量链接，再推进 T010/T011；不把本地
focused 结果写成 T016 qualification。

2026-09-07 Spec Kit default / **PASS**：通用 tasks-template 默认包含 Execution Progress 和
Current Checkpoint；任务生成/执行技能增加覆盖检查、增量更新及保留既有状态规则。
仅工作流文档更新，不改变上表产品状态。检查见 [registry evidence](evidence/task-progress-registry-20260907.md#spec-kit-default)。
下一步所有新任务清单沿此模板生成，Spec182 继续按现有 T001 缺口推进。

2026-09-07 Progress registry / **PASS**：36 个执行单元统一登记到上表，详细卡改为通用执行契约。
旧 Spark 路径保留历史入口；本次仅整理进度和规则，未实现或验收产品，不改变父任务勾选。
检查和状态来源见 [registry evidence](evidence/task-progress-registry-20260907.md)。
下一步关闭 T001 公开 API/设计 release 缺口，并处理已记录的构建依赖边界，再按 Gate Order 推进。

2026-09-07 Native component and binding implementation slice / **T001 IN_PROGRESS**：
已加入原生 ONNX recipe assembler（protobuf structural assembly、external
initializer digest binding、deterministic serialization）、grant issue/publish
ports、request preparation/admission ports、conversation journal coordinator、
Provider host registration seam，以及 `pythonWrapper` 的单一薄 pybind11 DI
入口。新增 C++ focused tests 均完成语法检查；Python binding/build-boundary
测试 **17/17 PASS**。绑定只映射 native DTO/错误/句柄/策略类型，`NativeServiceUser`
通过生命周期保持创建 native client，不在 Python 复制 planner 或状态机。
显式 `NDNSF_LIBRARY_DIR` 现在同时要求 Core 和 DI shared library，避免链接回退。
ONNX protobuf 生成源已纳入 Waf；全量 Waf/native qualification 仍受既有
NAC-ABE ABI mismatch 阻断，不能据此关闭 T002/T006/T009/T010 或 T012。
新增 installed-consumer Waf 目标（checkpoint `aba90194`），其公共头语法检查
通过；Spec182 相关 Python 门禁 **29/29 PASS**，设计校验 `errors=[]`。
Rust tokenizer bridge 的本机 release 构建在 `cargo: command not found`
（exit127）处未开始，原始边界见 `.codex-tmp/spec182-tokenizer-r1/`，不能
替代真实 bridge/ABI 验证。T001/O-004保持OPEN，产品任务仍 **0/17**；下一步
在可用同源 Cargo 工具链上完成 bridge/consumer 检查，并继续 T010/T011 的真实
request/stream 接线后再做 T015 静态收敛审查。

2026-09-07 Tokenizer identity guard / **T001 IN_PROGRESS**：修正
`NativeTokenizer` 在加载动态 bridge 前校验 tokenizer 文件摘要，并拒绝空输出
缓冲区；新增 3 个身份/工厂负例单测，独立 Boost.Test **3/3 PASS**（commit
`54595eee`）。Cargo 缺失仍使 Rust bridge 的真实构建保持 **NOT_RUN**，不关闭
T007/O-004。

2026-09-07 Public export inventory / **T001 IN_PROGRESS**：新增[API migration review](contracts/public-api-migration-review.md)和可复现AST snapshot，覆盖api27/sdk76/root174，共277导出，264定义/10assignment/3外部owner。发现正式api中23个名称尚无四份主契约的精确映射；部署catalog、请求handle和provenance不能由现有request概述替代。snapshot逐条UNREVIEWED，动态wildcard/继承/实例字段仍需核对；不是迁移完成。下一步逐行为完成正式api映射及动态层清单，O-004/T001保持OPEN，产品0/17。未修改产品源码、未运行native产品测试。

2026-09-07 Compatibility manifest source review / **T001 IN_PROGRESS**：`build_api_migration_manifest.py` 已改为保留类方法的参数注解/默认值、顶层函数签名和 assignment expression；`RequestRef` 明确解析为 `InferenceRequestHandle`，`RequestableDeployment` 明确保留其 `Union` 表达式。生成物覆盖显式277、动态67、总计344项；formal api 当前静态状态为 `PARTIAL_EXISTING_TYPE 8`、`PLANNED_TYPE 1`、`UNREVIEWED 18`。这只补足 O-004 的机器可读审阅入口，不等价于字段、错误、状态、caller 或 native 行为闭环；未关闭 O-004/T001，未修改产品源码，未运行 native 产品测试。已完成 `py_compile`、manifest invariant check 和 `git diff --check`；下一步继续逐项补齐 O-004 后才释放 T001-C。

2026-09-07 Compatibility caller classification / **T001 IN_PROGRESS**：manifest 继续保留全部 token mention，并新增 `maintainedCandidates`、`tests`、`generatedCopies`、`other` 分组；`packaging/*/build` 副本不再与维护入口混为一谈。分组仍是静态审阅辅助，不替代 owner 的真实调用语义判断，也不改变 `UNREVIEWED`/O-004 状态。已完成生成物 invariant、`py_compile` 和 `git diff --check`；未修改产品源码，未运行 native 产品测试。

### Prior Provider Lifetime

上述inventory单元277个导出键无重复，36个定义文件SHA256与当前源码一致；strict structure、design validator（214本地链接）及diff whitespace检查PASS。AST扫描不导入运行依赖，分namespace生成均exit0；初次全集工具输出截断后改为分namespace读取并合并，未把截断结果作为snapshot。

2026-09-07 Spark execution preparation / **DISPATCH_DESIGNED**：用户指定Spark为后续实现执行者。
已增加[execution cards](contracts/spark-execution.md)，保留17个父任务，补齐定向Read、精确Write、步骤、依赖和planned selector；
共享设计技能及本机tasks/implement入口支持bounded-executor。T001设计关闭与selector release仍是产品前置门，
本轮不替其他设计工作宣告关闭O项，也不勾选任何产品任务。36卡/17父任务覆盖、依赖、211链接、strict structure及三项validator拒绝反例PASS；
技能通用校验器与既有Spec Kit metadata的schema差异及替代检查见[spark preparation](evidence/spark-execution-preparation.md)。
下一步由T001-C汇总已有设计关闭证据、冻结实际选择器和构建入口，再从T002-A按依赖交给Spark执行。
**Spark trial NOT_RUN；产品仍0/17。**下方Provider等记录保留各自设计范围与历史验证事实。

### Spark Checkpoint

历史准备见 [preparation checks](evidence/spark-execution-preparation.md)。
当前状态统一到 [Execution Progress](#execution-progress)，此处不再维护第二张表。

### Prior Provider Lifetime Design

2026-09-07 Provider lifetime design / **T001 IN_PROGRESS**：在[lifecycle contract](contracts/native-provider-lifecycle-design.md#provider-lifetime-control)定义受mutex保护的RegistrationControl、close/post/析构互斥、锁外释放capture及post失败后续清理；补齐ACK同步/异步、Selection、普通lease执行fallback、完成发布检查。源码确认pending cleanup可能早于兄弟role结束，因此Selection将代次转入协作生命周期，work fence不依赖pending表。M47同步指向新增Core scoped接口；不是包装不存在的unregister。Provider生命周期设计范围已明确，O-004其余公开类型/调用及整体T001仍未完成，产品0/17。下一步归并设计清单并核对剩余缺项，不重启已闭合算法研究。

### Prior Registration Generation

上述lifetime设计单元strict structure、design validator（186本地链接）及diff whitespace检查PASS。未构建/运行native产品；新增生命周期单测仍NOT_RUN，文档检查不计T009完成。

2026-09-07 Registration generation / **T001 IN_PROGRESS**：源码确认ACK复制旧handler异步执行、Selection重新查当前service handler，单独DI closed包装不能隔离重注册旧请求。已在[lifecycle contract](contracts/native-provider-lifecycle-design.md#registration-generation-decision)定义Core scoped registration、pending代次绑定、ACK完成/Selection/work fence检查及幂等close清理范围；不增加wire或授权owner，不删除legacy API。尚需补齐Provider存活控制与同步fallback位置，T009仍BLOCK、T001/O-004未完成、产品0/17。本轮无native产品构建/测试；下一步沿新增Core范围完成可实施设计，不重新讨论已确定的代次机制。

### Prior Shared Lease Design

上述registration设计单元strict structure、design validator（183本地链接）和diff whitespace检查PASS；只支持文档checkpoint，不表示Core扩展已实现或生命周期测试通过。

2026-09-07 Shared Provider lease design / **T001 IN_PROGRESS**：核对Core addService/addCollaborationHandler会覆盖同名handler，且没有公开unregister；当前单服务CLI为固定lease入口创建单target私有表，不能直接逐serve复制。新增[Provider lifecycle contract](contracts/native-provider-lifecycle-design.md)，定义一个host共享lease表/prepare mutex、单入口target路由、跨target绑定验证、关闭期间只清理旧lease及精确类/字段/constructor迁移。此为拟议多服务集成风险的源码推导，没有运行native产品；现有单服务结果不被改判失败。

本单元strict structure、design validator（182本地链接）及diff whitespace检查PASS；无产品构建/测试。T001/O-004及T009仍未完成：下一步关闭registration generation、晚到ACK/Selection及Core入口生命周期，再完成其余公开类型清单。产品0/17，SIF/Tiger不介入。

### Prior Stream Caller Closure

2026-09-07 Stream caller/recovery closure / **T001 IN_PROGRESS**：补齐[token stream contract](contracts/native-token-stream-design.md#production-caller-inventory)的paired factory、认证摘要绑定、唯一生产CLI注入点及三个integration consumers；定义NativeInferenceOperation的accept/replacement/final检查。源码确认现有AutomaticStreamingHandle接受前缀只在内存，runtime journal不在逐token接受链；保留进程内replacement与已提交conversation恢复各自边界，禁止终止token后重启生成，新增final text一致性义务。A7-08相关设计已定义，产品修复/测试仍OPEN；O-004其余公开API/schema/Provider注册生命周期仍需关闭，T001未完成、产品0/17。

本轮仅源码核对与契约修订，没有native构建或运行测试。strict structure、design validator（180本地链接）及diff whitespace检查PASS。下一步统一收口O-004剩余注册生命周期及完整公开类型/调用清单，避免再重复已关闭的tokenizer算法问题。

### Prior Qwen Stream Design

2026-09-07 Qwen stream design / **T001 IN_PROGRESS**：读取交付清单固定revision的tokenizer.json，12,807,982 bytes及SHA256完全匹配，确认ByteLevel decoder；本地另一Qwen工件身份单独记录，不混用。新增[token stream design](contracts/native-token-stream-design.md)，定义stable API/第六私有ABI、所有权、ByteLevel与ByteFallback不同算法、终止flush及epoch候选/提交接线。A7-08剩余完整调用方和journal接受边界仍OPEN，T001未完成、产品0/17。

独立`/usr/bin/python3 tests/fixtures/spec182/dependency-probes/check-bytelevel-stream.py`实际exit0：65,536个two-byte序列、9个长/非法/截断序列和3个whole-token fallback检查PASS；Python标准库增量UTF-8结果与固定HF0.20.3完整decode对照。strict structure、design validator（178本地链接）和diff whitespace检查PASS。仅reference诊断，不执行新增ABI/native产品。下一步关闭事件接受/恢复与全部factory调用方，随后统一收口O-004。

### Prior Stream Boundary

2026-09-07 Stream boundary / **T001 IN_PROGRESS**：新增可移植[reference checker](../../tests/fixtures/spec182/dependency-probes/check-stream-boundaries.py)，固定tokenizers0.20.3及既有fixture哈希，7个边界输入实际PASS/exit0。确认完整UTF-8的byte run仍会被后续无效byte改写；合法U+FFFD不能删除，skip special不构成run边界。算法与新版HF原生stream能力比较写入[generation contract](contracts/native-generation-design.md#verified-boundary-and-planned-bytefallback-algorithm)。本轮没有native产品构建/测试。

A7-08 的本轮收口原则：完整decode仅用于完整文本验收，不可替代stream边界；`decodeStable`与`eventSink`恢复边界仍是T007/T011/O-004闭环项。

A7-08仍OPEN：ByteFallback适配选择已明确，但真实Qwen decoder pipeline、完整调用/字段与事件接受后失败的恢复边界尚未关闭。源码确认eventSink接受后仍执行反馈发布与runtime commit，不能承诺靠decoder局部rollback撤回事件。T001未完成、产品0/17；下一步从实际Qwen工件和既有journal/commit路径关闭这些剩余设计，不重跑已固定reference用例。本单元strict structure、design validator（173本地链接）及diff whitespace检查PASS；参考运行命令为`/usr/bin/python3 tests/fixtures/spec182/dependency-probes/check-stream-boundaries.py`。

### Prior Generation Design

2026-09-07 Generation design / **T001 IN_PROGRESS**：[native generation contract](contracts/native-generation-design.md)补齐GenAI/HF/ORT复用比较与现有sampler的参数、double精度、去重惩罚、截断后归一化及旧会话兼容处置。A7-10 已按复用边界清单闭环：GenAI仅作能力对照，不作为默认生产路径；A7-11 CLOSED。A7-08 的完整decode/stream边界与A7-09 的采样数学均为设计约束，不等于实现/通过；A7-08/A7-09与O-004仍OPEN，T001未完成、产品0/17。修订plan旧授权句；不新增生成引擎或产品依赖，不操作实验机器。

本单元检查 **PASS**：prerequisites、strict structure、design validator和diff whitespace。独立Python reference源SHA256 `3affb6f438a4134bb8e69222d79b3f2ec5a6b256bf0022af636b065627397c11`；将log概率按`struct.pack/unpack('<f')`量化后输入`[-0.5108256340026855,-1.2039728164672852,-2.3025851249694824]`，seed8/top_k3/top_p0.8/temperature1/step0实际选0；重复惩罚例也实际选0，assert通过/exit0。仅运行标准库reference，不构建native或运行产品测试。已有ONNX normalization草稿与failure-log修改保留，不在本单元宣称O-002关闭。

下一步关闭stream decoder的preview/commit/flush/restore算法和逐调用方清单，再完成O-004及T001。下方审计检查点为历史事实，不覆盖本段状态。

### Prior Native Reuse Review

2026-09-06 Native reuse review / **BLOCK for implementation**：核对原生库复用与当前源码，ONNX/ORT及HF Rust tokenizer方向合理；新增A7-08流式decode前缀不稳定、A7-09已有C++ Top-P归一化及重复惩罚与Python reference不一致，A7-10缺GenAI复用对照、A7-11计划旧授权语句。详见[native reuse review](evidence/native-reuse-review-20260906.md)。reference诊断exit0：多byte-token文本展示prefix重写，两个采样输入Python实际返回0而native源逻辑推导为1；未运行native产品。raw `.codex-tmp/spec182-native-reuse-review-20260906-r1/`。T001/O-002/O-004保持OPEN、产品0/17；下一步在T001冻结复用/stream/采样兼容处置，再由T007/T011/T016实现和证明。本轮仅审计记录，不改产品源码或既有oracle。

本审计记录检查 **PASS**：Spec Kit prerequisites、strict structure、design validator（163本地链接、17任务/0完成）、`git diff --check`；reference诊断exit0及具体结果见同一review/raw。这些只允许保存审计记录，不关闭A7-08/A7-09、O-002/O-004或T001。

checkpoint首轮被本地pre-commit全索引引用检查拒绝（exit1）；已核对hook提供`NDNSF_LOCAL_CHECKPOINT=1`专用模式，后续本地提交使用该模式且保留禁止路径检查，原始`commit-r1.json`不覆盖。未修改hook、未push；其他工作正在追加的typed-complex诊断不纳入本审计提交。

### Prior ONNX Contract Checkpoint

2026-09-06 T001 ONNX contract / IN_PROGRESS：补齐[native ONNX assembly design](contracts/native-onnx-assembly-design.md)的owned类型、9个函数、8步算法、recipe/manifest字节与native worker生命周期；修复直接进程内替换会丢失硬超时回收的设计缺口。原始Python实现仍未修改；独立reference提取24个普通numeric raw/typed向量PASS。额外诊断确认BFLOAT16 raw摘要内容错误、STRING跨进程摘要不稳定，见[identity evidence](evidence/identity-reference-20260906.json)及failure index。**O-002/O-004仍OPEN，T001未完成，产品实现0/17**。下一步冻结这两种表示的稳定身份/兼容处置，并继续完整能力和Provider注册清单；不复制错误oracle、不以I/O dtype代替initializer能力范围。

本单元检查 **PASS**：strict structure、design validator（160本地链接、17任务/0完成）、diff whitespace；24个model hash、12对identity、6条诊断及未修改reference源hash核对一致。generator锁定原onnx/numpy/source版本，补锁后typed诊断仍通过。Context Mode健康检查PASS，但relevance检索返回了较早的Dependency Design子节，checkpoint以实际tasks顶部为准；CodeGraph的临时副本结果仍剔除，算法按精确生产路径核对。未构建/测试产品，不把发现旧缺陷或补全设计计作T006完成。

### Isolation Design Checkpoint

2026-09-06 T001 isolation design：**O-005 CLOSED**。已冻结[native isolation design](contracts/native-isolation-design.md)的最小root/namespace、权限与服务白名单、逐进程exec/映射/endpoint观测、harness函数/字段和I01--I08反例。bwrap0.4.0/strace5.5工具正例exit0且CapEff=0/NoNewPrivs=1，缺解释器反例在exec边界ENOENT/exit1，均符合预期；raw `.codex-tmp/spec182-t001-isolation-r1/`。没有运行NDNSF、MiniNDN、SIF/Tiger，也未实现T014 detector。当前O-001/O-003/O-005按各自设计范围CLOSED，**O-002/O-004仍OPEN，T001仍未完成**；下一步补完整ONNX算法和迁移/注册/状态清单。

本隔离设计单元检查 **PASS**：strict structure、design validator（149本地链接、17任务/0完成）、`git diff --check`。已静态核对工具选项、namespace/文件根与外部harness分界；最小工具正反例不外推NFD/Repo/Controller或全部后代观测的运行证明。此前依赖设计单元已本地提交`5187e733`，tracked tree随该单元清理，原始构建/日志未入Git；未push。

### Dependency Design Checkpoint

2026-09-06 T001 / IN_PROGRESS：用户已授权在Experimental完成Spec182，按原目标实施与本地验收；上一轮只审计的范围已结束。先关闭O-002--005，不提前迁移业务或执行最终集成/MiniNDN。已补齐tokenizer特殊token参数，建立[依赖设计与探针契约](contracts/native-dependency-design.md)。T001保持未勾选；SIF/Tiger继续外部负责。

T001运行边界：ONNX1.17.0原生依赖-j2构建及四向量探针 **PASS / exit0**，四个模型字节与原Spec181 oracle完全一致。tokenizers0.20.3/Rust1.90.0静态ABI与C++consumer构建 **PASS**，84个完整ids/text对照和14个拒绝检查 **PASS**；两探针ldd均无Python。ABI、生产桥接路径/字段/所有权、许可及依赖锁已冻结，**O-003 CLOSED**。O-002的完整算法/叶子契约、O-004完整兼容与状态设计、O-005隔离方案仍OPEN；T001和T007未完成。Rust工具链R1/R2下载TLS失败在R3更换HTTPS实现后恢复。详见同一依赖设计记录及failure index；不计产品任务完成。

本依赖设计单元检查 **PASS**：prerequisites、strict structure、design validator（143本地链接、17任务/0完成）、`git diff --check`；lock与两个oracle及三个tokenizer JSON的hash一致，Cargo.lock中72个registry依赖均有精确checksum。probe静态审查已在运行前完成，输入/expected来自旧版本，未改变产品源或历史oracle。恢复时Context Mode提供的`probe.cpp write` timeline查询未通过高熵identifier/source guard，改用持久tasks/contracts与实际日志核对，不从被拒绝的session检索推断状态。下一步补O-002/O-004/O-005；不重跑已通过且输入未变的探针。

### Prior Audit Checkpoint

2026-09-06 revision 7 / SOURCE_ALIGNMENT_COMPLETE：用户确认另一台机器已接收，本轮只审计/修订Spec182。以Experimental `81e251a4ef1d8e6a394dc5f0c38bc44e44bfc973`核对代码，修正未提交合并/旧integration失败的过时表述；固定NAC/SVS/NDNSD身份与旧运行证据失效范围。O-001按源码身份和181承接范围CLOSED；O-002--005仍OPEN，T001未勾选，实现仍 **0/17**。当前SVS/NDNSD组合 **UNQUALIFIED**，接收不等于实验通过。审计发现与关闭责任见[audit](audit.md)，具体版本见[integrated baseline](contracts/integrated-baseline.md)。

本轮未构建或执行产品unit/integration/MiniNDN/SIF/Tiger，未管理接收机器，未恢复已停止的ABI构建。下一步只继续T001的ONNX/tokenizer依赖、兼容/字段/注册寿命及隔离设计关闭；后续182开发验证仍按下方任务分工，上一轮delivery-only移交不取消T016义务。

本轮实际文档检查 **PASS**：Spec Kit prerequisites、strict structure、`checklists/validate_design.py`（136链接、17任务/0完成、依赖无环、19 FR/11 SC/14 CD/16 PO）、`git diff --check`；12类137字段AST名称/类型/默认值一致，19 FR/11 SC/16 PO/14负例/48方法/137字段条目相对审计输入未删改。以上不计产品验收；O-002--005保持OPEN。改动仅12份Spec182文档，历史evidence及产品源码不变。

## Historical Checkpoints

以下为各时点事实；其旧“下一步”、失败、未push及验证结果均不覆盖上方当前checkpoint。旧结果只适用于当时身份。

2026-09-06 Source Handoff COMPLETE / SOURCE_READY：用户明确本轮只交付，编译与测试在另一台机器执行。本机额外构建已停止并确认无遗留编译进程；后续NDNSD/全部ABI消费者构建、unit/integration、两wrapper验证、MiniNDN、SIF/Tiger全部TRANSFERRED，不再作为本机交付条件。D001范围修订为固定版本及ABI要求移交，D001--D004已完成；四库Experimental、可迁移输入/模板与root skills已发布。已有检查与中断事实保留，不冒称完整验证PASS。接收步骤及证据见 [source handoff](../../Experiments/TigerCluster/docs/source-handoff.md)。Spec182仍0/17，不计T001--T017完成。

2026-09-06 Existing Presentation Checkpoint：补存此前未跟踪的`docs/NDNSF-UAV/slides/UPDATES.tex`及对应4页PDF；标题和全部命名frame的PDF文本核对PASS。旧版`UPDATES_UAV.pdf`、LaTeX缓存、原始实验输出及本地助手状态继续保留为本地产物，不计源码或182进度。活动指针/managed plan均指182，最终索引刷新后project与active健康检查PASS。

2026-09-06 Branch Closure：`Experimental` 已快进至整合提交 `e91ecc91`，本地只保留 `main`、`Experimental`；原生验证工作树改为detached并保留二进制/raw，临时分支已删除。主工作区原426项源码/文档状态保存在具名恢复stash `4bb5e0a5`，实际成果已归并，不能直接pop旧测试覆盖修复。未push。另将用户既有UAV slides源文件/PDF单独checkpoint：30个frame与30页PDF、标题/日期和两个新增Geo-Capture页的文本对应检查PASS；这是既有演示文档保存，不计182产品验收。

2026-09-06 Experimental Consolidation：原生合并修复已成为真实merge commit `c770f18bb7bf42c3b8a8274b5c029b4883141f60`，包含远端UAV历史；同步本机 `2e7865c7` 的revision6和Tiger目录布局。最终工作分支统一为Experimental，main保持稳定基线。原生生产代码与已验证基线逐字节一致；unit **759/759**、integration **154/154**、current Python **2171 passed / 22 skipped**、三个真实MiniNDN授权/撤销场景 **PASS**，见 [integration closure](evidence/integration-20260906.md)。此前152/154是已修复的历史失败，不再控制合并状态。

新增 [integrated baseline](contracts/integrated-baseline.md) 固定Core/UAV复用接口、生命周期修复与181承接。O-001已提供实际commit/证据/承接表，T001仍须核对最终Experimental差异并关闭O-002--005；182实现仍 **0/17**，其最终产品验收 **NOT_RUN**。不再独立续跑181最终资格，下一步是T001设计关闭。

2026-09-06 revision 6：合并重复审查与报告；实现任务只做静态审查、相关unit及必要构建，集成与MiniNDN在全部实现后由T016统一运行。类/方法/字段设计和既定真实运行用例保留。

本轮只改技能与文档；实现 **0/17**，产品STATIC_REVIEW与unit/integration/MiniNDN均 **NOT_RUN**。
文档结构、107链接、依赖及技能引用检查PASS；既定PO-001--014/负例/运行用例与符号字段表的保留检查PASS。范围见 [workflow simplification](evidence/workflow-simplification.md)。
O-001--005和合并修复状态未被本轮关闭。下一步执行T001确认基线与设计就绪。

## Tiger Directory Checkpoint

2026-09-06 Consolidation Review Closure：当前Tiger源码一并归入Experimental。静态复审修复ACK/选择/请求关联、原生错根拒绝证据组合、有限应用进程组清理；新增负例先复现19项失败，修复后新工具目录 **58/58 PASS**，旧目录兼容及关联工具 **162 passed / 3 skipped**。B001/B002开发检查更新，B003实际SIF/双节点/复用验收仍未完成，Local R8 FAIL保留。用户已停止实验，本轮未运行SIF/Tiger；详见 [baseline checkpoint](../../Experiments/TigerCluster/docs/two-node-baseline.md#usage)。

2026-09-06 Two-node baseline IN_PROGRESS：新增[基础实验契约与进度](../../Experiments/TigerCluster/docs/two-node-baseline.md)，独立B001--B003负责共享runtime、双节点profile、真实NDN/NDNSF探针及实际运行复用；不改变Spec182原生迁移任务状态。B001/B002实现与静态审查完成，相关unit **24/24 PASS**；本地/远端SIF哈希一致。B003尚待精确SIF集成与实际双节点/复用运行，尚无实验PASS。

2026-09-06 Usage Guidance：本地工作约定已补充Tiger唯一入口、共享owner、旧路径同步和双机分工；可随Git交付的说明见[Tiger README](../../Experiments/TigerCluster/README.md#compatibility-and-review-boundary)。路径/链接及文档一致性检查PASS；本轮只改说明，不运行产品测试、不改变实现进度或验收状态。

2026-09-06 Follow-up R2：64个迁移文件的哈希/权限/旧新路径、29个共享文件与审查工作树对比、Tiger文档本地链接均PASS，无新增同步差异。两份已同步测试未变，沿用R1的58/58工具单测证据，本轮不重复执行。审查工作树完整integration日志为152/154 PASS、2 failed；整合未完成，目录迁移无需追加修改，等待该owner交付最终基线。详见 [follow-up evidence](evidence/tiger-directory-migration-20260906.md#follow-up-r2)。

2026-09-06 Review Sync R1：64个迁移文件及共享lib/bin无新差异；两份测试修正已从审查工作树同步，相关工具/collector单测 **58/58 PASS，exit0**，关闭上轮原因码断言失败。源码迁移与原生实现状态不变；证据见下方migration evidence。

2026-09-06：Tiger目录迁移完成，64文件内容/权限与路径静态检查PASS；工具单测50 PASS / 1 FAIL，独立原布局已复现相同原因码失败，留给原工具owner。纯路径checkpoint保留49个已跟踪文件原HEAD内容，已有修改及15个未跟踪文件在新路径继续保留，不混入迁移提交。详见 [migration evidence](evidence/tiger-directory-migration-20260906.md)。本工作不计T001--T017实现或产品验收。

## Validation Standard

唯一规则见 [validation workflow](contracts/pre-test-static-review.md)。
T002--T014的任务[x]仅代表本任务实现、静态审查、相关单测和必要构建完成；
其Proof行引用完整行为义务，跨组件/跨进程与MiniNDN证明登记给T016，不要求各任务提前运行。
T015在全部实现后补审整体接线；T016执行完整unit→integration→MiniNDN并关闭全部必需PO。
不得将真实集成重命名为unit/smoke提前执行。Static review PASS != Behavior PASS。
每任务只保留简短结果或一份evidence链接，最终核对diff与证据，不另建S0/S1报告。

## Phase 1: Design and Native Components

- [x] T001 [US5] **Successor Baseline and Design Closure**。冻结合并基线与181承接表、所有 schema/公开调用方/能力清单及原生依赖，关闭 O-001--005；修订叶子签名与任务至可执行。Dependencies: Merged baseline closure and Spec181 handoff。
  Design: FR-015,FR-016,FR-017,FR-018; CD-001--014。Proof: PO-012。
  [T001 contract](contracts/work-units.md#t001-successor-baseline-and-design-closure)。

- [x] T002 [US1] **Installable Native Library Contract**。existing Provider runtime 可独立安装/链接；planned requester 公开声明先冻结，完整request由T010实现、T016运行验收。Dependencies: T001。
  Design: FR-001,FR-012; CD-001,CD-009。Proof: PO-001。
  [T002 contract](contracts/work-units.md#t002-installable-native-library-contract)。

- [x] T003 [US2] **Native Split and Placement Decisions**。两个原生模型 splitter 与默认 placement 对固定输入生成合法且确定的方案。Dependencies: T002。
  Design: FR-003,FR-009,FR-016; CD-002。Proof: PO-002。
  [T003 contract](contracts/work-units.md#t003-native-split-and-placement-decisions)。

- [ ] T004 [US1] **Canonical Native Plan Sealing**。合法 proposal 转成可被真实 Core/Provider 接受的规范计划；非法投影在首边界拒绝。Dependencies: T003。
  Design: FR-002,FR-004; CD-003。Proof: PO-003。
  [T004 contract](contracts/work-units.md#t004-canonical-native-plan-sealing)。

- [ ] T005 [US1] **Native Requester Grant Path**。原生 requester 签名/申请/发布 grant，实际 Provider 验证并消费密钥。Dependencies: T004。
  Design: FR-005; CD-004。Proof: PO-004。
  [T005 contract](contracts/work-units.md#t005-native-requester-grant-path)。

- [ ] T006 [US1] **Native Cold ONNX Assembly**。Selection后原生装配与既有固定bytes一致；原生worker保留有界取消/清理，删除Python helper及其文件IPC。Dependencies: T002；O-002 closed。
  Design: FR-006,FR-016; CD-005。Proof: PO-005。
  [T006 contract](contracts/work-units.md#t006-native-cold-onnx-assembly)。

- [ ] T007 [US4] **Native Tokenizer Execution**。原生 encode/decode 完整文本，与固定 tokenizer oracle 一致，无子进程解释器。Dependencies: T002；O-003 closed。
  Design: FR-007; CD-006。Proof: PO-006。
  [T007 contract](contracts/work-units.md#t007-native-tokenizer-execution)。

- [ ] T008 [US1] **Native Request Preparation and Admission**。原生输入/认证模型/工件准备与 offer policy 校验闭合，GraphAdapter/TaskAdapter 端口由 native 实现。Dependencies: T003/T006/T007。
  Design: FR-001,FR-002,FR-004,FR-009,FR-016; CD-013。Proof: PO-013。
  [T008 contract](contracts/work-units.md#t008-native-request-preparation-and-admission)。

- [ ] T009 [US3] **Shared Native Provider Host**。CLI/C++/Python 共用服务注册、准备/执行接线与停止语义。Dependencies: T006/T007。
  Design: FR-001,FR-009,FR-010,FR-012; CD-014。Proof: PO-014。
  [T009 contract](contracts/work-units.md#t009-shared-native-provider-host)。

## Phase 2: Invocation and Compatibility

- [ ] T010 [US1] **Complete Native Request Lifecycle**。独立 C++ requester 从模型/输入到真实 Response，cancel/deadline/late callbacks 保持单一终态。Dependencies: T003/T004/T005/T006/T007/T008/T009。
  Design: FR-001,FR-002,FR-008; CD-001,CD-013,CD-014。Proof: PO-001,PO-003,PO-007,PO-013,PO-014。
  [T010 contract](contracts/work-units.md#t010-complete-native-request-lifecycle)。

- [ ] T011 [US4] **Native Conversation Continuation**。原生 requester 续接/有限恢复与既有 epoch/state runtime 协作，文本/lineage 正确。Dependencies: T010。
  Design: FR-008,FR-016; CD-007。Proof: PO-007,PO-008。
  [T011 contract](contracts/work-units.md#t011-native-conversation-continuation)。

- [ ] T012 [US3] **Thin Python Native Bindings**。支持的 Python 调用转发同一 native 库，无 Python strategy trampoline/业务状态机。Dependencies: T011。
  Design: FR-010; CD-008,CD-009。Proof: PO-009。
  [T012 contract](contracts/work-units.md#t012-thin-python-native-bindings)。

- [ ] T013 [US3] **Default Route and Legacy Retirement**。所有 maintained callers 默认原生；旧运行时退出默认 import/调用图。Dependencies: T012。
  Design: FR-011,FR-016; CD-010。Proof: PO-010。
  [T013 contract](contracts/work-units.md#t013-default-route-and-legacy-retirement)。

## Phase 3: Proof and Delivery

- [ ] T014 [US5] **Runtime Dependency Exclusion Gate**。harness 将被测 native scope 与 Python harness 隔离，能拒绝已知 interpreter/libpython/helper 旁路；交付正式 MiniNDN harness/collector 并完成本地单测；真实隔离反例在T016运行。Dependencies: T013。
  Design: FR-001,FR-011,FR-012,FR-014; CD-011。Proof: PO-001,PO-010,PO-012。
  [T014 contract](contracts/work-units.md#t014-runtime-dependency-exclusion-gate)。

- [ ] T015 [US5] **Design-code Convergence Audit**。整体静态读码核对FR/CD/INV/PO、生产接线、test/oracle/harness与依赖身份，控制性发现清零；记录整体审查结论并进入T016。Dependencies: T014。
  Design: FR-013,FR-017,FR-018; CD-001--014。Proof: PO-001--016。
  [T015 contract](contracts/work-units.md#t015-design-code-convergence-audit)。

- [ ] T016 [US5] **Local Native Qualification**。全部实现和T015审查完成后，同源完整unit→integration→YOLO/Qwen MiniNDN/no-Python及必要检错证明通过，核对最终diff与证据。Dependencies: T015 PASS。
  Design: FR-001,FR-005,FR-006,FR-007,FR-008,FR-010,FR-011,FR-012,FR-013,FR-016; CD-011。Proof: PO-001--016。
  [T016 contract](contracts/work-units.md#t016-local-native-qualification)。

- [ ] T017 [US5] **Native Development Handoff**。唯一开发交付版本、维护文档与两个入口示例；外部实验单列 TRANSFERRED。Dependencies: T016 PASS。
  Design: FR-014,FR-015; CD-012。Proof: PO-012。
  [T017 contract](contracts/work-units.md#t017-native-development-handoff)。

## Dependencies & Execution Order

Merged baseline closure and Spec181 handoff → T001 → T002；T003/T004/T005 依次收口；
T006/T007 依赖 T002 和已关闭 native dependency design；
T003/T006/T007 → T008；T006/T007 → T009；
T003--009 → T010 → T011 → T012 → T013 → T014 → T015 PASS → T016 → T017。
所有任务遵循FR-018/019和统一验证规则；每任务具体范围见work-units。只有最早未关闭门可进入其对应实施。每次失败先保留新 raw/evidence、更新本 tasks/failure index。
最终 checkpoint 前核对 task 状态与实际 diff/PO；不得 blanket stage 预存修改。

## Related Paper Comparison Checkpoint — 2026-09-08

用户要求的 ABE-backed、DNMP-inspired 和 per-service role-certificate 论文级比较已形成
[standalone section and evidence](../../docs/PAPER/named-data-network-service-framework-paper/authorization-design-comparison.md)。
核对了 DNMP/NAC-ABE 原文、当前 Controller 聚合策略与 grant/revoke 路径，并记录源码摘要；
补充 User permission DKEY 的额外成本，未把凭证数量推导写成性能测量。
本工作仅为独立论文材料，未修改协议/API、当前/目标设计或上述实现任务状态。
定向检查覆盖表格结构、引用/本地链接、源码摘要及文档 diff；后续需统一论文引用编号，
总体管理成本优势仍待单独实验。该条不关闭任何 Spec182 acceptance gate。

## Related Proposal Authorization Checkpoint — 2026-09-08

用户授权的中英文 Proposal 及对应 slides 已按保密发现、权限聚合/身份凭证复用、
运行时撤销三个方面更新，见 [revision evidence](../../docs/PAPER/proposal-defense/authorization-revision.md)。
正文、长/短 slides 与讲稿构建无未定义引用；新增页经渲染检查。
长版 PPTX 为 64 页，1944/1944 文本 spans 分配通过；LibreOffice 重导出授权页无缺失或重叠。
原始证据和旧交付物位于 `.codex-tmp/proposal-authorization-20260908-docs/`。
明确 DNMP 原文与对照设计的区别，保留传播/重新发钥成本；实验数字未改，未重跑协议实验。
本工作不改变 API、当前/目标设计或任何实现任务验收状态；下一步为论证反馈及管理成本评估。

## Related Final Defense Review Checkpoint — 2026-09-09

按用户要求扩展 [proposal review](../../docs/PAPER/proposal-defense/review.md)：保留 26 条老师批注，
新增 FD01–FD16 问题、贡献/证据对应、验证边界及论文和答辩 slides 的组织建议。
定向检查覆盖审查条目完整性、本地证据链接、输入 PDF 摘要及三个关键页的渲染；
没有修改论文/slides 或重跑实验，没有据此关闭任何实现/资格验收任务。
原始审查产物保留于 `.codex-tmp/proposal-reviewed-20260909/`；下一步先统一 RQ 和最终贡献范围，
再依据已有证据确定必要的补充验证，不自动扩大工程或实验范围。

## Related Official Defense Criteria Checkpoint — 2026-09-09

按用户要求联网核对本校 CS 答辩评价表、PhD 程序及论文准备指南，并参考 CMU/UMD CS 文件；
已在 [review §20](../../docs/PAPER/proposal-defense/review.md#20-根据公开的计算机系-final-defense-要求重新校准)
区分本校要求、外校参考和 NDNSF 审查建议，新增 FD17–FD20。
完整 Graduate Catalog 正文未获取，不宣称已核实全部学位规则；DOCX/HTML 提取工具边界见 failure log。
文档检查核对 C01–C26、FD01–FD20、来源层级、本地证据及冻结 PDF 摘要；
本轮未修改论文/slides、运行实验或关闭实现验收门。下一步为贡献归属、统一 RQ 和模拟问答准备。

## Related Research-first Proposal Revision — 2026-09-09

按新授权依据 review.md 重组 EN/CH Proposal 和 slides，见
[claim/evidence audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)。
三个 RQ、授权替代方案、角色／依赖契约、UAV/DI 范围、实测边界和 Spring 2027 计划已统一；
旧根/分章入口加载同一语义正文。EN 28 页、CH 21 页、slides 38 页（33 主讲、5 备份／引用）。
PDF 编译、替代入口全文一致性、逐页渲染及 PPTX 798/798 span 覆盖通过；
可编辑文字 551 个文本框，38 页 presenter notes；导出备注百分号保留定向回归 2 PASS。
最终摘要及构建／渲染身份见 [validation](../../docs/PAPER/proposal-defense/research-revision-validation.json)。
这只是文档及导出工具修订：Core/DI API 与行为无变化，未执行模型／MiniNDN／Tiger 实验，
未关闭任何 native qualification 或原研究验收任务。C12/C22、完整比较、原始历史 DI provenance
及端到端协作证据仍为 OPEN/PARTIAL。下一步先由导师确认 RQ／贡献范围，再完成与主张对应的既定证据。
