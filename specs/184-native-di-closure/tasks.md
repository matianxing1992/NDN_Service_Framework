# Tasks: Native DI Closure

**Status**: IN_PROGRESS / current-candidate C++ process qualification refresh recorded; qualification remains partial | **Date**: 2026-09-12
**Input**: [spec](spec.md)、[plan](plan.md)、[transfer matrix](contracts/transfer-matrix.md)、
[promotion candidate](contracts/promotion-candidate.md)、[caller matrix](contracts/caller-matrix.md)、
[qualification matrix](contracts/qualification-matrix.md)

## Execution Progress

本表是逐执行单元的唯一当前状态入口；下方 `Logical Batch Progress` 只记录批次出口和批次级动态验证。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Authority IO Dispatch](evidence/b1-request-correctness-20260911.md) | DONE | documentation gate and migration baseline | B1 focused C++ and TSan exits complete; no remaining T001-specific gate | 2026-09-11 |
| [T002 Turn Publication Synchronization](evidence/b1-request-correctness-20260911.md) | DONE | T001 static gate | B1 focused C++ and TSan exits complete; no remaining T002-specific gate | 2026-09-11 |
| [T003 Durable Outcome Linearization](evidence/b2-durable-outcome-20260911.md) | DONE | B1 behavior exit | B2 normal and unsuppressed ASan/UBSan exits complete; no remaining T003-specific gate | 2026-09-11 |
| [T004 Atomic Private Checkpoint Export](evidence/b3-checkpoint-export-20260911.md) | DONE | B2 exit | B3 normal and unsuppressed ASan/UBSan exits complete; directory-fsync failure remains an explicit implementation limit | 2026-09-11 |
| [T005 Maintained Caller Mode Closure](evidence/b4-caller-convergence-20260911.md) | DONE | B1–B3 exits | Caller/mode matrix and focused route selectors complete; D2b runtime miss, real-model/no-Python and retirement remain in B5 | 2026-09-11 |
| [T006 Inherited Obligation and Harness Closure](contracts/qualification-matrix.md) | DONE | B4 exit | 80-row matrix has one explicit owner, status, evidence path and remaining boundary per inherited row; current candidate identity and fresh convergence are recorded. Open runtime/model/external rows are explicitly transferred to T007/T008, not hidden in the registry | 2026-09-11 |
| [T007 Current Native Qualification](contracts/promotion-candidate.md) | IN_PROGRESS | T006 complete and fresh convergence `PASS_FOR_T007_PRECONDITION` | Current candidate receipt verification, full C++ unit/integration exits, native process runs, candidate-bound I02–I08 counterexample statuses, I02–I08 dynamic behavior samples, no-Python ELF closure, repaired tiny-ONNX sanitizer selectors and bounded root `PO-001-stream` owner are recorded in [process qualification refresh](evidence/t007-process-qualification-20260911.md). The latest source/lifetime review, binding rebuild and same-tree receipt are recorded in [current candidate refresh](evidence/a4-current-candidate-refresh-20260912.md); current-candidate YOLO Y-A, Y-B and Y-N now pass. Same-tree C++ Qwen-config fixture selectors and focused native unit groups pass; their exact boundary is in [model capability evidence](evidence/t007-model-capability-20260912.md). The focused C++ admission negative/immutable-view coverage is recorded in [A4 admission coverage](evidence/a4-offer-admission-coverage-20260912.md). The MiniNDN native requester caller route now excludes the legacy `--native-cpu-provider` diagnostic flag when a native config is supplied; details and focused checks are in [post-ACK caller routing](evidence/native-minindn-post-ack-routing-20260912.md). Qwen3.6-27B remains `WAITING_EXTERNAL_INPUT` because this host can run only Qwen3-0.6B smoke, and inherited negative/retirement rows remain `PARTIAL`. Non-root owner preflight remains `UNQUALIFIED` at `MININDN_REQUIRES_ROOT`; I05 remains an intentional `UNQUALIFIED` observation boundary | 2026-09-12 |
| [T008 Native Development Handoff](plan.md) | NOT_STARTED | T007 `QUALIFICATION_PASS` | Design/API handoff and external experiment transfer remain pending | 2026-09-12 |

## Logical Batch Progress

| Batch | Status | Dynamic profile / stable exit | Next / remaining |
| --- | --- | --- | --- |
| B1 | DYNAMIC_PASS / CLOSED_FOR_VALIDATION | `tsan` PASS；IO owner、turn/ticket 线性化、无 pending residue | T001/T002 已完成普通 C++ selector、独立 TSan 各两次重复；进入 B2/T003 |
| B2 | DYNAMIC_PASS / CLOSED_FOR_VALIDATION | `asan-ubsan` PASS；durable handle/journal 一致、publish 后终态不降级、residue=0 | T003 已完成；进入 B3/T004 |
| B3 | DYNAMIC_PASS / CLOSED_FOR_VALIDATION | `asan-ubsan` PASS；原子 export、symlink refusal、pre-rename failure preservation、loader smoke | T004 已完成；进入 B4/T005 |
| B4 | CLOSED_FOR_VALIDATION / PARTIAL | caller rows use `none` with reason; inherited B1/B2 profiles cover shared async owners | T005 matrix/route closure complete；D2b runtime miss、real model/no-Python and retirement remain T006/T007 |
| B5 | IN_PROGRESS / T006/T007 PARTIAL | one bounded dynamic sample per distinct inherited risk/behavior class；Provider-host unsuppressed ASan/UBSan PASS；observed-offer parser sample PASS；I02 ownership-cycle and 16-case tiny-ONNX sanitizer samples PASS | current candidate unit/integration exits `0`; C++ process/no-Python refresh, I02–I08 dynamic samples, candidate-bound I02–I08 counterexample statuses and authorized root `PO-001-stream` owner are bounded results. The latest framework/DI source refresh passed static review and same-tree build/receipt; A0–A2 current-candidate rows now pass, while A4 inherited negative/retirement remains open. The maintained Qwen MiniNDN caller now selects the C++ native requester route without the legacy tensor diagnostic flag when configured; [post-ACK caller routing](evidence/native-minindn-post-ack-routing-20260912.md) records the focused closure and its pre-execution path-boundary retry. The remaining gates are tracked in the [T007 remainder ledger](evidence/remainder-audit-20260911.md): A0 receipt → A1 YOLO Y-A → A2 YOLO Y-B/Y-N → A4 inherited negative/retirement; A3 Qwen3.6-27B is external because this host only supports 0.6B smoke. The attempted r4 selector build, wrong Waf target lookup, missing Waf environment, and mixed old-test/r4-library retry are preserved as build/ABI boundaries in [model capability evidence](evidence/t007-model-capability-20260912.md). I05 collector boundary is `UNQUALIFIED`; T008 remains blocked by T007 |
| B5-A4-OFFER-ADMISSION | CLOSED_FOR_VALIDATION / PARTIAL | `static` PASS；normal and unsuppressed ASan/UBSan same-tree C++ `Spec182OfferAdmission` selector 18/18 cases and 81/81 assertions；production source unchanged | [A4 admission coverage](evidence/a4-offer-admission-coverage-20260912.md)；full process/no-Python、Python retirement、real Qwen3.6-27B and external rows remain open |

### T007 Remainder Exit Ledger

本表是 T007 当前可执行出口，按依赖排序；已关闭的 B1–B4、C++ sweep、bounded samples 和
Python harness 不因本表再次无条件重跑。`Qwen3-0.6B` 只能作为 C++ smoke/ABI fixture，不能
替代契约要求的 `Qwen/Qwen3.6-27B` qualification row。

| Gate | Status | Dependency / owner | Exit evidence required | Next action |
| --- | --- | --- | --- | --- |
| T007-A0 current-candidate runtime receipt | `PASS_FOR_ROW` | local build owner | Fresh `build-spec184-b5-candidate-r4/spec180-native-build.json`, `verify` exit `0`, system `/usr/bin/g++ -B/usr/bin`, Waf `-j4`, candidate-first requester/provider paths and `binding_reused=false` | Refreshed receipt and identity are bound in [current candidate refresh](evidence/a4-current-candidate-refresh-20260912.md) |
| T007-A1 YOLO26n Y-A | `PASS_FOR_ROW` | local root MiniNDN owner; depends A0 | Current-candidate C++ native numerical oracle, terminal result, child exits and cleanup | Preserve r58 evidence; continue to A2 |
| T007-A2 YOLO26n Y-B/Y-N | `PASS_FOR_ROW` | local root MiniNDN owner; depends A1 | Protected multi-provider terminal path and all declared negative permutations with first-failure classification and cleanup | Y-B r59 and Y-N r60 pass on the same refreshed candidate; continue to A4 |
| T007-A3 Qwen3.6-27B | `WAITING_EXTERNAL_INPUT` | experiment owner; external model/runtime | Signed three-stage `Qwen/Qwen3.6-27B` manifest, tokenizer, CUDA runtime, model identity and result evidence | Execute on the experiment machine when the exact model is available; do not relabel 0.6B |
| T007-A4 inherited negative/retirement rows | `PARTIAL` | local C++ owner plus matrix owner | Complete row evidence with selector/owner, candidate identity, child exit, cleanup and first-failure boundary; repair I05 collector evidence if possible | Close only rows whose declared evidence is complete |
| T008 native development handoff | `BLOCKED_BY_T007` | documentation owner; depends T007 qualification pass | Final candidate map, local qualification result and external rows explicitly marked `TRANSFERRED` | Do not start handoff or mark Spec184 complete before T007 passes |

## Current Checkpoint

2026-09-12 **T007-NATIVE-MININDN-CALLER-ROUTING / CLOSED_FOR_VALIDATION**：修正
`Experiments/NDNSF_DI_LlmPipeline_Minindn.py` 在 native requester config 存在时误传
`--native-cpu-provider` 的接线问题；该标志会优先进入旧的逐 token tensor-bundle
diagnostic 分支，使 `request_native_reference` 不可达。现在两条路线互斥，配置化请求由
`NativeInferenceClient` 负责 ACK_CLOSED 后的 native planning，Provider 在认证 Selection
后执行 canonical assembly；无配置时的兼容诊断路径仍保留并明确标记。定向 route/legacy
测试 14/14、native backend/binding 测试（显式候选 Provider 路径）27/27、AST、
`py_compile` 和 `git diff --check` 通过。首次 backend 测试因默认
`build-system-j2/examples/di-native-provider` 不存在而在产品启动前失败，已保留原始边界
并用 `build-spec184-b5-candidate-r4` 显式路径重跑通过；完整记录见
[post-ACK caller routing](evidence/native-minindn-post-ack-routing-20260912.md)。本批未运行
MiniNDN，因此不新增协议、模型、数值或资格结果；T007 仍 `IN_PROGRESS`/`PARTIAL`，A3、A4
和 T008 状态不变。

2026-09-12 **T007-A3-EXTERNAL-PREFLIGHT / WAITING_EXTERNAL_INPUT**：本机只能运行
`Qwen3-0.6B`，同树 C++ fixture/test 结果不加载实际 0.6B 权重，不能替代
`Qwen/Qwen3.6-27B`。对 `itiger` 的 SSH、Slurm/Apptainer 和模型库存检查为只读；后续扫描
发现 Qwen3.6-27B Hugging Face cache，但快照只有 15 个分片中的第 1 个（其余 14 个缺失）。
另有完整的 Spec175 三阶段 ONNX artifact 和 tokenizer，但它绑定历史 candidate，且历史
多 Provider 终端为 exit `1`；仍没有当前 Spec184 candidate 或外部资格运行。A4 的
Python 兼容/路由聚焦检查为 30 passed，但 compatibility manifest 的 344 个 entry
仍全部 `removalEligible=false`，所以旧路径退出仍为 `PARTIAL`。详见
[A4 retirement and external boundary](evidence/a4-retirement-external-boundary-20260912.md)。
T007 保持 `IN_PROGRESS`/`PARTIAL`，A3 保持 `WAITING_EXTERNAL_INPUT`，A4 保持 `PARTIAL`，
T008 保持 `BLOCKED_BY_T007`。

2026-09-12 **PLAN-MODEL-BOUNDARY-CORRECTION / DOCUMENT_PASS**：修正 `plan.md` Summary
中把早期 r4 `integration-tests` task-generator 缺失误读为当前阻塞的表述。当前候选同树
C++ build/test 已通过；本机仍只能使用 `Qwen3-0.6B` smoke/ABI，未执行实际 0.6B 权重推理，
也不能替代外部 `Qwen/Qwen3.6-27B` qualification。A3、A4、T007 和 T008 状态不变。

### Proposal Origin coverage restoration (2026-09-11, documentation only)

根据完整 Origin 补齐双语 Proposal 的动机、背景、框架架构/API、三种调用情境、对象/流、UAV 与 DI 工作流及评价方法。参见 [本轮文档证据](evidence/proposal-origin-expansion-20260911.md) 与 [逐节覆盖矩阵](../../docs/PAPER/proposal-defense/origin-coverage-review.md)。初次文档驱动路径错误已定位；修复后四入口编译、镜像文本、引文、页面边界及版式复核通过，英文 49 页、中文 38 页。产品代码、实验数字与 T007/T008 资格状态不因论文补写改变。

2026-09-12 **T007-LOCAL-CXX-SMOKE / PARTIAL**：本机不能运行 Qwen3.6-27B，未尝试 27B。
修复 Spec184 独立测试树的 NAC-ABE include/test-access 边界并补齐同树 worker 依赖后，
`integration-tests` 以 `-j4` 构建成功（46.811 秒、exit `0`），assembly worker 与五个
故障注入工具也在同一树构建成功。设置 `NDNSF_SPEC182_BIN_DIR` 后，full
`integration-tests` exit `0`、`*** No errors detected`；full `unit-tests` 为
1034/1034 cases、71072/71072 assertions。Qwen 原生配置流式、会话续接、
`Spec184DurableOutcome` 以及 `Spec184NativeCheckpoint`、`Spec182NativeAssembly`、
`Spec182NativeInferenceClient`、`Spec182NativeRequestIdentity` 等 C++ selectors 均通过；
这些使用源绑定小 Qwen ONNX fixture 和 CPU runtime contract，不是实际 0.6B 权重推理，
也不关闭 A3。证据见 [model capability evidence](evidence/t007-model-capability-20260912.md)。
T007 保持 `IN_PROGRESS`/`PARTIAL`，A3 为 `WAITING_EXTERNAL_INPUT`，A4 和 T008 状态不变。

2026-09-11 **PROPOSAL-CH4-WORDING / DOCUMENT_PASS**：中英文 Chapter 4 开头
改为直接说明应用评估目的，删除回应修改意见式措辞及重复说明。四个正文
入口构建、镜像一致性及排版日志检查通过；证据见
[wording audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
和 `.codex-tmp/proposal-ch4-wording-20260911/checks.json`。不改 slides、实验
或产品/API，不推进 T007/T008；后续仍按覆盖表关联证据。

2026-09-11 **PROPOSAL-APPLICATION-VALIDATION / DOCUMENT_PASS**：已扩展中英文
Chapter 4 与对应 slides 为完整 UAV／DI 应用级验证范围，并增加功能—机制映射。
初次讲稿 overfull 已修正；八个 LaTeX 入口、PDF/PPTX 边界、文字可编辑性、
镜像一致性与 LibreOffice 回渲检查通过；slides 47 页，原实验内容未改。
证据见 [application-validation audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
及 `.codex-tmp/proposal-app-validation-20260911/verify.log`。无产品/API 变化，
不运行产品实验，不推进 T007/T008；下一步为覆盖表关联已有证据与剩余测试。

2026-09-12 **T007-A1-YA-CURRENT-CANDIDATE / PASS_FOR_ROW**：在重新构建 Python binding 并更新
native identity receipt 后，root MiniNDN Y-A 使用同一 r4 candidate 完成真实 Controller、Repo、
Provider、User 链路。C++ YOLO numerical oracle `matched=true`，shape `[1,50,6]`，
`maxAbsError=0.0005340576171875`；terminal response、四个 child exit 和 cleanup 均通过。
运行证据为 `.codex-tmp/spec184-yolo-Y-A-output-20260912-r58/`，launcher log 为
`.codex-tmp/spec184-yolo-Y-A-run-20260912-r58.log`；A2 的 Y-B/Y-N 仍需在同一 candidate 上运行。

2026-09-12 **T007-A2-YB-CURRENT-CANDIDATE / PARTIAL**：同一 r4 candidate 的 root MiniNDN
Y-B 通过，C++ numerical oracle `matched=true`，shape `[1,50,6]`，
`maxAbsError=0.0005340576171875`；七个 child exit、terminal response 和 cleanup 均完成。
运行证据为 `.codex-tmp/spec184-yolo-Y-B-output-20260912-r59/`，launcher log 为
`.codex-tmp/spec184-yolo-Y-B-run-20260912-r59.log`。同一 candidate 的 Y-N 七子案例随后全部通过，A2 行关闭。

2026-09-12 **T007-A2-YN-CURRENT-CANDIDATE / PASS_FOR_ROW**：同一 r4 candidate 的 root MiniNDN
Y-N 矩阵通过，`SPEC180_CASE_RESULT status=PASS case=Y-N`。矩阵
`.codex-tmp/spec184-yolo-Y-N-output-20260912-r60/y-n-matrix-result.json` 的 SHA-256 为
`009e07d68a766f1545312b87261884e789cbb207db4404432f1ac3729161215a`；
`Y-N-O/C/P/R/I/E/L` 分别在 `TERMINAL_RESPONSE`、`PLACEMENT_DECISION`、`ACK_CLOSED`、
`PLAN_SEALED`、`PROVIDER_EXECUTION_STARTED`、`PROVIDER_GRANT_VERIFICATION`、
`EVIDENCE_ACCEPTANCE` 边界通过，E 子案例包含 `EXPIRED`、`FORGED_AUTHORITY`、
`WRONG_RECIPIENT` 的独立拒绝证据。launcher log 为
`.codex-tmp/spec184-yolo-Y-N-run-20260912-r60.log`，SHA-256
`86cbd5a303b6cd5cf5db51ec5aa4568ec092aa18d09a4e762d32a306b7d694f1`；受控 child teardown
和 cleanup 完成。A2 关闭，T007 仍因 A3 外部输入和 A4 继承行保持 `IN_PROGRESS`/`PARTIAL`。

2026-09-12 **T007-A4-CANDIDATE-REFRESH / PARTIAL**：审查并修复注册重试句柄的
延迟回调生命周期和空输入边界后，使用同一 r4 配置树重建受影响 C++ target，并在候选
NAC-ABE/NDN-SVS 优先的 loader 环境中重新生成、验证 receipt。新 receipt 的
`binding_reused=false`，详见 [current candidate refresh](evidence/a4-current-candidate-refresh-20260912.md)。
由于 framework/DI 二进制身份变化，旧 Y-A/Y-B/Y-N 结果不再绑定当前 candidate，A1/A2
回到 `NOT_RUN_CURRENT_CANDIDATE`，必须重新运行；A3 仍为外部输入，A4 继续收口，T008 仍阻塞。

2026-09-12 **T007-MODEL-CAPABILITY-BOUNDARY / PARTIAL**：本机确认只能运行
`Qwen3-0.6B`，不能运行契约要求的 `Qwen/Qwen3.6-27B`。尝试在当前 r4 candidate
上补跑 C++ `integration-tests` selector 时，Waf 在编译前报告没有该 task generator；原始
日志和 SHA-256 见 [model capability evidence](evidence/t007-model-capability-20260912.md)。随后
拒绝了旧测试二进制与 r4 库混用的尝试：selector 在业务 marker 后 SIGSEGV，作为 ABI 边界保留。
这不是模型或协议失败，也不改变 A0–A2 的已验证结果；A3 保持
`WAITING_EXTERNAL_INPUT`，A4 继续独立收口，T008 仍阻塞。

2026-09-12 **T007-A4-OFFER-ADMISSION-COVERAGE / PARTIAL**：补充当前 `NativeOfferAdmission::verify`
的 C++ 负例和 immutable planning view 覆盖；同树 `Spec182OfferAdmission` 运行 18/18 cases、
81/81 assertions 通过。静态门同时确认两个 Spec182 历史计划名不能映射为当前 DI API：Provider
跨角色复用是有意允许的，`ControllerVersion` 属于 Core 契约且 `offerDigest` 由 admission 派生。
没有修改生产实现或冻结 manifest。详见 [A4 admission coverage](evidence/a4-offer-admission-coverage-20260912.md)。
该批次只关闭测试覆盖单元；由于测试 harness/validation 文档变化，下一次最终 qualification
前需重新计算 candidate identity，T007 仍 `IN_PROGRESS`/`PARTIAL`，A3 仍等待外部 27B，T008 仍阻塞。

2026-09-12 **T007-A4-OFFER-ADMISSION-ASAN / DYNAMIC_PASS**：同一 `Spec182OfferAdmission`
选择器在独立无抑制 ASan/UBSan tree 中运行 18/18 cases、81/81 assertions，通过且没有
sanitizer 或 LeakSanitizer 报告。该动态出口只加强 admission 覆盖，不关闭完整 A4 继承行、
Python retirement、no-Python process 或 Qwen3.6-27B；详见 [A4 admission coverage](evidence/a4-offer-admission-coverage-20260912.md)。

2026-09-11 **PROPOSAL-EMAIL-ALIGNMENT / DOCUMENT_PASS**：中英文 Proposal、slides
及讲稿按邮件统一三个安全要求和额外权限聚合便利；分别解释身份、服务权限与
防重放，补齐 grant／withdrawal，并将专用 Sync 更新宣告保留为待端到端验证
的修订设计。八入口构建、31/24 页正文、44 页 slides、9 页讲稿、同语言文本
一致、897/897 PPTX spans、44 页 notes、全页边界、截图及 LibreOffice 回渲
检查通过，notes parser 2/2，历史实验页内容不变。Context Mode active hash
过期采用文件依据；措辞扫描误匹配跨表格边界已修正并保留失败记录。
证据见 [email-alignment audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
与 `email_alignment_revision`。仅文档修订，无产品实验、API／Design 契约变化，
不推进 T007/T008。下一步核对另一机器的 Sync 路径证据及 RQ1 生命周期成本。

2026-09-11 **PROPOSAL-NONCE-CHALLENGE / DOCUMENT_PASS**：当前中英文 proposal、
slides 和讲稿统一 `RequestNonce`／`ProviderChallenge`，保留请求绑定、
受保护值访问检查及签名／权限／状态边界；不修改运行时字段或 API。
八入口构建、旧称清理、镜像文本一致、全页文字边界、PPTX 829/829 spans、
40 页 notes、LibreOffice 回读和截图审查通过；notes parser 2/2。
详见 [terminology audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
与 `nonce_challenge_revision`，原始记录 `.codex-tmp/proposal-nonce-challenge-20260911/`。
Context Mode 历史检索被 guard 拒绝后采用实际文件；无产品实验、API 或
Design 契约变化，不推进 T007/T008。

2026-09-11 **T007-REMAINDER-AUDIT / PARTIAL**：本轮只读复核并重排 T007 剩余出口，未启动
MiniNDN、YOLO 或 Qwen 全量运行。YOLO26n 当前输入 preflight（Y-A）已通过，日志见
`spec184-yolo-preflight-20260911-r7.log`，但 current candidate 缺少
`build-spec184-b5-candidate/spec180-native-build.json`，native build guard 首边界为
`SPEC180_NATIVE_IDENTITY_REJECTED`，所以 Y-A 仍为 `NOT_RUN`。本机无法执行契约要求的
`Qwen/Qwen3.6-27B`，仅有 `Qwen3-0.6B`；后者只能记录为 C++ smoke/ABI fixture，不能关闭
Qwen qualification。A0→A1→A2→A4 顺序和外部 A3 已写入 [remainder audit](evidence/remainder-audit-20260911.md)。
T007 继续 `IN_PROGRESS`/`PARTIAL`，T008 仍 `NOT_STARTED`。

2026-09-11 **PROPOSAL-REASON3 / DOCUMENT_PASS**：将权限聚合独立为辅助理由三，
理由二保留管理设施复用论证，例子去重；理由一及生命周期正文不变。
四入口构建、同语言 PDF 文本一致、全页文字边界及修订页截图检查通过，
英文 30 页／中文 23 页。标题定位检查误匹配已修正，记录见
[Reason 3 audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
及 `reason3_revision`。无 slides／API／实验变更，不推进 T007/T008。

2026-09-11 **PROPOSAL-REASON2 / DOCUMENT_PASS**：局部重写双语 ABE 理由二，明确
属性权限管理复用、challenge 调用检查、身份签名凭证独立性及 DNMP 对照的成本
边界；理由一和后续章节源码保持不变。四入口编译、同语言 PDF 文本一致性、
全页文字边界及修订页截图检查通过，英文 30 页／中文 23 页。证据见
[Reason 2 audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
及 `reason2_revision`。不修改 slides／产品 API，不运行实验或推进 T007/T008。

2026-09-11 **PROPOSAL-INTRO-FOCUS / DOCUMENT_PASS**：删除双语引言中突兀的 gRPC
名称解析细节，保留后文基线能力说明与引用，衔接 NDN 计算／调用和 DNMP 授权先例。
四个正文入口构建、同语言文本一致性、引言范围和后文引用、全页文字边界与引言
截图检查通过，英文 30 页／中文 23 页。证据见
[Introduction focus audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
及 `intro_focus_revision`。slides／实验／产品 API 不变，不推进 T007/T008。

2026-09-11 **PROPOSAL-INTRO-WORDING / DOCUMENT_PASS**：按作者原句更新英文引言
trust schema 表述并同步中文；四个正文入口构建、对应入口文本一致性、原句与
PDF 文字边界检查通过，英文 30 页／中文 23 页。证据见
[Introduction wording audit](../../docs/PAPER/proposal-defense/research-revision-audit.md)
及 `intro_wording_revision`。slides／实验／产品 API 不变，不推进 T007/T008。

2026-09-11 **PROPOSAL-DNMP-THROUGHLINE / DOCUMENT_PASS**：将 DNMP 的 Trust Schema
命令授权实例贯穿双语 Proposal 的引言、RQ1、机制比较、评价与结论，保留原版／
适配方案边界。八入口构建、30/23 页正文、40 页 slides、8 页讲稿、同语言文本
一致性、828/828 PPTX 文字分配及 LibreOffice 回渲检查通过；讲稿解析 2/2，
历史实验页数字不变。见 [DNMP throughline review](../../docs/PAPER/proposal-defense/dnmp-throughline-review-20260911.md)
及 `dnmp_throughline_revision`。没有产品/API 变更或新实验，不推进 T007/T008。

2026-09-11 **PROPOSAL-INVOCATION-SCOPE / DOCUMENT_PASS**：按作者确认的初版多候选选一
范围，整体区分调用基础与多角色协作扩展；更新摘要、正文、RQ、协议表、结论和 slides。
双语 PDF 30/23 页、slides 40 页、讲稿 8 页；八入口构建与同语言文本一致性通过，
826/826 PPTX span 分配及 PDF／LibreOffice 回渲边界检查通过，实验页数字保持不变。
证据见 [invocation scope review](../../docs/PAPER/proposal-defense/invocation-scope-review-20260911.md)
与 `invocation_scope_revision`。不改产品/API，不运行产品实验，不推进 T007/T008 资格状态。

2026-09-11 **DISK-CLEANUP / CLEANUP_PASS**：清理 `.codex-tmp` 与 `pythonWrapper/build` 中
可重建的 3304 个 `.o/.d` 中间文件（约 5.19 GiB），并清除无活动进程使用的 Go/pip 缓存；
当前候选可执行文件、库、原始证据、模型、SIF、密钥和未提交源码均保留。清理前后磁盘可用空间
约由 35 GiB 增至 41 GiB。详见 [disk cleanup evidence](evidence/disk-cleanup-20260911.md)。
本轮不修改产品行为或资格判据，T007 仍为 `IN_PROGRESS`/`PARTIAL`，T008 仍未开始。

2026-09-11 **PROPOSAL-SENTENCE-REVIEW / DOCUMENT_PASS**：完成双语 Proposal 与 40 页
slides 的逐句措辞、依据范围和逻辑审查，同步 29/22 页正文、8 页讲稿及可编辑 PPTX。
八入口构建、同语言文本一致性、832/832 PPTX 可编辑 span、PDF/LibreOffice 边界及
逐页渲染检查通过；证据见 [sentence review](../../docs/PAPER/proposal-defense/sentence-review-20260911.md)
及其 validation。没有修改产品 API／行为或重跑实验，不改变 T007 PARTIAL／T008 状态。
后续研究仍需完整授权成本比较、协作端到端反例及历史 DI provenance。

2026-09-11 **T007-I02-OWNERSHIP / DYNAMIC_PASS**：静态复核发现
`ServiceProvider::fetchCollaborationSignedExactData` 中 `express`/`retry` 的 shared-pointer
强引用环，修复为 `retry` 持有 `weak_ptr` 并在重试调度时临时提升。独立无抑制
ASan/UBSan 的 `Spec175NativeTinyOnnxI02TwoProviderEpochCoordinator` exit `0`，无
LeakSanitizer 报告；旧 `87,522 bytes / 720 allocations` 首边界保留在
[T007 qualification evidence](evidence/t007-current-native-qualification-20260911.md)。
本轮只关闭该生产 ownership 类别，候选源码/二进制/日志身份已刷新，T007 仍为 PARTIAL。

2026-09-11 **T007-TINY-ASAN / DYNAMIC_PASS**：在同一独立无抑制 ASan/UBSan tree 中重跑
16 个 `Spec175NativeTinyOnnx*` 行为类 selector；所有正例和声明负例的 C++ oracle 均通过，
exit `0`，无 ASan/UBSan/LeakSanitizer 报告。旧的 3,096,985-byte leak 运行保留为首边界，
不再代表当前候选结果。详见 [T007 qualification evidence](evidence/t007-current-native-qualification-20260911.md)。

2026-09-11 **T007-CANDIDATE / PARTIAL**：按 fresh candidate 绑定运行完整 native unit、完整
integration、Provider-host sanitizer gate、authority/native route selectors 和 bounded
process/no-Python owner probe。unit 与 integration 均 exit `0`；process driver 的 frozen manifest
schema 边界仍为 `UNQUALIFIED`，非 root owner probe 在 uid 1000 的 `MININDN_REQUIRES_ROOT`
preflight exit `2`，没有产生 MiniNDN 拓扑或业务结果。随后使用 `/usr/local/bin` 完整 PATH 的
授权 root owner 完成 canonical `PO-001-stream`，exit `0` 并记录拓扑、进程、namespace、
endpoint、business marker 和 cleanup。observed-offer parser-fuzz 样本与修复后的 I02/16-case
tiny-ONNX 无抑制 ASan/UBSan 样本均 PASS；更广 parser/negative collector、真实模型/MiniNDN、
Python retirement 与外部 SIF/Tiger 仍未运行；
详见 [T007 qualification evidence](evidence/t007-current-native-qualification-20260911.md)
与 [failure-log entry](../../docs/failure-log.md)。T007 保持 `IN_PROGRESS`/`PARTIAL`，T008 未开始。

2026-09-11 **T006-MATRIX-CLOSURE / DONE**：80 行 qualification matrix 已逐项绑定继承来源、
owner task、production entry/selector、negative boundary、dynamic profile、candidate/source
identity、evidence path 与状态；当前 ordered candidate 和 fresh convergence 也已记录。仍为
`PARTIAL`/`OPEN` 的运行、真实模型、Python retirement 与 external-owner 行已明确移交 T007/T008，
不再作为 T006 的隐藏文档缺口。

2026-09-11 **T007-PROCESS-REFRESH / PARTIAL**：当前候选的 C++ 主导过程链已形成独立
证据：[process qualification refresh](evidence/t007-process-qualification-20260911.md)。候选
target build、完整 C++ unit/integration、YOLO unary、Qwen stream、conversation continuation、
Provider recovery/replacement、无备份拒绝、七类 native grant process case、六个业务二进制
no-Python ELF closure 以及授权 root `PO-001-stream` MiniNDN owner 均已记录；Python 只做私有
设施编排。该出口只关闭已运行的 bounded process classes，不关闭 I02–I08 的所有
counterexample/collector completeness、真实模型 breadth、Python retirement 或外部 SIF/Tiger。
T007 保持 `IN_PROGRESS`/`PARTIAL`，T008 仍未开始。

2026-09-11 **T007-FRESH-CANDIDATE / PARTIAL**：清理可重建的未跟踪 build 目录后，按系统
`/usr/bin/g++ -B/usr/bin`、`-j4` 和 `.lock-spec184-b5` 重新配置并只构建 Spec184 candidate
targets；主 targets `502/502`、worker/helper closure `15/15`。以显式
`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate` 运行 fresh unit 与 integration，均 exit
`0`；对应日志、二进制摘要和 runner manifest 已写入 [T007 qualification evidence](evidence/t007-current-native-qualification-20260911.md)
与 [promotion candidate](contracts/promotion-candidate.md)。广泛 Waf build 仍保留历史
`spec181-assembly-parity` 链接边界，未把该辅助目标失败混入 Spec184 candidate。fresh candidate
owner 运行因当前用户权限 `MININDN_REQUIRES_ROOT` 为 `UNQUALIFIED`，因此不能重用旧 root
owner PASS；T007 仍为 `PARTIAL`。

2026-09-11 **T007-PO001-ROOT / PASS_FOR_ROW**：在保留非 root `MININDN_REQUIRES_ROOT`
边界后，将 `/usr/local/bin` 加回 owner 的 command-local `PATH`，以 root 重新运行同一
current-candidate runner manifest。canonical two-node `PO-001-stream` exit `0`，trace、
namespace、process-tree、endpoint、business marker 与 cleanup evidence 完整；结果见
`.codex-tmp/spec184-b5-owner-probe-20260911-r7/`。这只关闭 `PO-001` bounded owner row，
不提升 T007 总体状态，I02–I08、真实模型和外部 owner rows 仍开放。

2026-09-11 **B5-CONVERGENCE / PASS_FOR_T007_PRECONDITION**：冻结本地 candidate（详见
[promotion candidate](contracts/promotion-candidate.md)），并完成
[B5 design-to-code convergence audit](evidence/convergence-b5.md)。五条覆盖 lane（production
caller、implementation/wiring、test/harness、build/source closure、migration/evidence）均已
对账，PO-015 review row 已为 `PASS`；当前完整 unit exit `0`，完整 integration exit `1`/48 failures，parser-fuzz 和
process/no-Python 尚未运行。该出口只授权开始候选本地 qualification，不提升 T006/T007 状态；
T006 仍为 `PARTIAL`，T007/T008 仍未完成。

2026-09-11 **D-TASK-REGISTRY / DOCUMENTATION_PASS_ONLY**：补齐逐执行单元 `Execution Progress`
registry，T001–T005 标为 `DONE`、T006 为 `PARTIAL`、T007–T008 为 `NOT_STARTED`；批次表改名为
`Logical Batch Progress`，避免批次出口遮蔽单元状态。共享 skill 与个人安装副本同步了“批次表不能替代
逐单元 registry”的规则。验证见 [task progress registry evidence](evidence/task-progress-registry-20260911.md)。
本轮只改工作流和 Spec184 文档，不改变产品代码或资格结论。

2026-09-11 **B1-TSAN / DYNAMIC_PASS**：B1 普通 `/usr/bin/g++` 构建完成，三个命名 C++ selector
通过；独立 `/usr/bin/clang++` TSan 构建完成，三个 selector 各重复两次，均 exit code 0 且无
ThreadSanitizer 报告。原始输出见 [B1 evidence](evidence/b1-request-correctness-20260911.md)；
TSan 仅覆盖 B1 登记的不变量，不提升后续 durable outcome、checkpoint、caller 或最终 qualification。

2026-09-11 **DYNAMIC-GATE / DOCUMENTATION_UPDATED**：共享 `speckit-code-design`、Spec Kit
模板及 Spec184 已统一登记 `Risk class`、`Dynamic profile` 和 `Dynamic invariants`。
动态分析按批次风险触发，使用独立 ASan/UBSan、TSan 或 parser-fuzz 输出树；本次仅更新流程和
矩阵，未将动态验证记为 `DYNAMIC_PASS`，产品任务仍为0个完成。同步检查、结构审计与 Context
Mode active health 已通过；下一步继续 B1 普通 C++ selector 后再运行 TSan。

2026-09-11 **D-UAV-JOINT / CLOSED_FOR_VALIDATION (documentation only)**：UAV示例改为
区域内未知目标的多视角联合辨认，4页构建/渲染通过；
[证据](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md)。不推进 B1–B5；原生任务状态保持。

2026-09-11 **B1-STATIC / CLOSED**：T001/T002 已完成逐任务只读静态审查、普通 C++ 定向测试和
独立 TSan 动态验证；生产差异、审查边界和动态不变量记录在 [B1 evidence](evidence/b1-request-correctness-20260911.md)。

2026-09-11 **B2-FOCUSED / DYNAMIC_FAIL**：T003 的 `Spec184DurableOutcome` 在普通 C++ 构建下
通过，验证 durable publish 后并发 cancel 不会把 handle 降级，checkpoint 与 coordinator 记录一致。
严格 ASan/UBSan 在 fixture teardown 首次触发外部 `ndn-svs` `new-delete-type-mismatch`；未抑制原始
日志和两次仅用于诊断的抑制重跑均记录在 [B2 evidence](evidence/b2-durable-outcome-20260911.md)。
共享动态门规则要求先修复或隔离一致 ABI 后再写 `DYNAMIC_PASS`，因此 T003 保持 `[ ]` / `PARTIAL`，
不能进入 B3。

2026-09-11 **DYNAMIC-CARD / DOCUMENTATION_UPDATED**：共享 skill、Spec Kit 模板、Spec184
`plan.md`/`spec.md` 已加入批次级 `Dynamic gate card`，冻结参数边界、C++ selector、业务不变量、
预算、toolchain/source identity、输出路径和失败分类；sanitizer 抑制不得直接升级为 `DYNAMIC_PASS`。

2026-09-11 **DYNAMIC-LOOP / DOCUMENTATION_UPDATED**：共享 skill 与 Spec184 将动态分析收敛为
四步批次循环（Freeze → Sample → Run → Classify）。B5 动态样本按不同风险/行为类别取代表性
正负例，同一状态机共享动态构建；80 行 qualification matrix 继续逐项对账，未覆盖行保持
`PARTIAL`，不新增逐参数或逐行构建任务。该记录只改变执行流程，不改变当前产品或资格状态。

该流程变更发生在上一候选冻结之后，因此按 promotion candidate 的 change-plane 规则将旧
candidate 标为 `STALE_LOCAL_PARTIAL`。旧运行结果继续作为历史证据保存；T007 恢复前必须
重新生成 candidate digest 并通过 fresh convergence，不能把旧结果静默绑定到新契约。

2026-09-11 **T007-I01 / C++_DYNAMIC_PASS**：修复预装载兼容路径删除
`request-input` scope 的键冲突，并让无 preparation callback 的 epoch coordinator
调用已装载 runner 的普通 runtime 入口。I01 selector 重新构建后 exit 0，8 个 token
epoch、8 个事件和最终 EOS payload 均通过；原始 trace 见
`evidence/t007-current-native-qualification-20260911.md`。T007 仍为 `PARTIAL`，仅关闭
该 C++ first-boundary 缺陷，不能提升整批资格状态。

2026-09-11 **T007-SPEC175 / C++_DYNAMIC_PASS**：同一候选树运行
`Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnx*` 批次，正例 I01/I02/I03/I04/I05/I06/I11/I12/I15/I16
和预期负例 I07/I09/I10/I13 全部由 C++ selector 断言通过，batch exit 0；见
`evidence/t007-current-native-qualification-20260911.md`。这只是采样行为类动态 PASS，T007
仍保持 PARTIAL，process/no-Python、parser-fuzz、fresh candidate/convergence 和其余继承行未关闭。

2026-09-11 **T007-FULL-NATIVE / C++_SWEEP_PASS**：修复后的候选路径运行完整
`integration-tests` 与 `unit-tests`，均 exit 0 且 `*** No errors detected`；日志和哈希见
`evidence/t007-current-native-qualification-20260911.md`。本地 C++ 全量 sweep 已通过，
但 T007 仍为 PARTIAL，尚需 process/no-Python、parser-fuzz、fresh candidate/convergence、
负例绑定和外部 owner 行。

2026-09-11 **T007-SANITIZER / DYNAMIC_FAIL**：同一 Spec175 tiny 行为类在无抑制
ASan/UBSan 树运行时，业务 selector 断言输出正常，但重复环境 teardown 以 exit 134 结束，
LeakSanitizer 报告 3,096,985 bytes / 25,092 allocations；未见 UAF、越界或 UBSan 报告。原始
日志见 `evidence/t007-current-native-qualification-20260911.md`。该失败不能计为动态 PASS，
需先用单个 C++ case 重跑并判定泄漏归属。

2026-09-11 **T007-I01-ASAN / DYNAMIC_PASS；I02-ASAN / DYNAMIC_FAIL**：单个 I01 在无抑制
ASan/UBSan 下 exit 0 且无 sanitizer 报告；I02 业务断言通过，但 teardown 因测试 fixture 的
`makeD2bCoordinatorOptions` callback captures 报 87,522 bytes LeakSanitizer 泄漏、exit 134。
I01 可计入共享 C++ 状态机的 sanitizer 样本，I02 保持失败边界，不能提升 T007。

2026-09-11 **T007-PARSER-FUZZ / DYNAMIC_PASS**：新增生产 C++ selector
`Spec182ObservedOffer/Spec184NativeParserFuzz`，固定 seed 运行512个有界 JSON/wire 变异，
覆盖截断、字节替换、插入、后缀噪声和结构破坏；候选 unit 与无抑制 ASan/UBSan 各 exit `0`，
无 sanitizer 报告。该结果只关闭 observed-offer parser 风险类别，不能提升未覆盖的继承行或
T007 总体状态。原始日志和哈希见
[T007 qualification evidence](evidence/t007-current-native-qualification-20260911.md)。

2026-09-11 **B2-ASAN / DYNAMIC_PASS**：先保留外部 NDN-SVS 旧头文件/库失配导致的未抑制
`new-delete-type-mismatch`，随后用当前源码重建 NDN-SVS 并重链独立 ASan/UBSan tree。普通
selector 与无抑制 sanitizer selector 各通过，sanitizer selector 共三次、均 exit code 0、无
ASan/UBSan 报告；B2 evidence 记录原始失败、依赖重建和 binary digest。T003 完成，下一步 B3/T004。

2026-09-11 **B3-ASAN / DYNAMIC_PASS**：T004 使用原生 `NativeCheckpointExport` 完成同目录临时文件、
`0600`、canonical JSON、file/directory `fsync`、原子 rename 和 symlink 拒绝。普通 C++ selector
三例通过；独立无抑制 ASan/UBSan selector 重复三次，均 exit code 0 且无 sanitizer 报告。带候选输出
目录优先的 `LD_LIBRARY_PATH` 运行 `DI_NativeRequester --help` 通过；此前 `/usr/local/lib` 优先的
loader 失败保留为库来源边界。详见 [B3 evidence](evidence/b3-checkpoint-export-20260911.md)。

2026-09-11 **B4-CALLER / CLOSED_FOR_VALIDATION**：T005 将五组维护入口收敛为 caller/mode
矩阵 12 行，明确 native owner、显式 compatibility、C++ selector、source/build closure、
zero-use 和 rollback。候选 provider 路径修正后，五个当前 C++ route/provider/Qwen selectors
均 exit 0，Python route/compatibility 回归 38 tests passed。B4 没有新增异步 owner，动态
profile 对 caller/launcher 记为 `none` with reason，B1/B2 的 TSan/ASan/UBSan 仍是共享状态机
的动态证据。旧 D2b selectors 在 bootstrap 后观测到零 response/role/output，作为 runtime/test
miss 保留；真实模型、no-Python、Python retirement 和外部实验仍未验收。详见
[B4 evidence](evidence/b4-caller-convergence-20260911.md) 与 [caller matrix](contracts/caller-matrix.md)。

2026-09-11 **B5-MATRIX / PARTIAL**：T006 已将 14 个继承父任务、16 个 `PO`、8 个 `I`、
19 个 `FR`、14 个 `CD` 和 9 个 `INV` 逐项加入 qualification matrix，共 80 行；矩阵字段
检查在修正继承旧行列数范围后通过，动态参数矩阵要求已同步到共享 skill、Spec Kit templates
和 Spec184。当前 `DEV-865e1ee2` 只是真实开发 checkpoint 关联标签，不是 promotion candidate；
native tokenizer/parser、完整 process/no-Python、真实模型、ordered candidate digest、
fresh convergence audit 和 T007 仍开放。详见 [B5 matrix evidence](evidence/b5-matrix-binding-20260911.md)。

2026-09-11 **B5-COMPONENT / PARTIAL**：候选树按当前源码重建，18 组原生 C++ unit
selector、6 组集成 selector 和 71 个 Python harness 回归通过。Provider-host 的无抑制
ASan/UBSan 运行先发现 `HostState` resolver self-cycle（11,042 bytes/122 allocations），
随后验证了“仅 resolver 弱引用、runtime handler 保持 host”修复；全套 8 个 Provider-host
用例无 sanitizer 或 LeakSanitizer 报告。一次 all-weak 诊断修复被 ASan UAF 否决，未计入结果。
这次只关闭组件生命周期门，T006 仍为 `PARTIAL`；完整 process/no-Python、候选冻结和
convergence 尚未通过。详见 [B5 component evidence](evidence/b5-component-validation-20260911.md)。

2026-09-11 **B5-FULL-SWEEP / PARTIAL**：同一候选目录优先的完整 C++ unit executable
以 `--log_level=test_suite` 运行，exit `0`，耗时 146.304 秒，输出 `*** No errors detected`。
完整 integration executable 在 900 秒边界内结束但 exit `1`，共 48 个 Boost failures；首个
边界仍是 legacy D2b/D2h121/D2h212 的零 response/role 观测以及 Spec175 tiny-ONNX 的
`stream event gap exceeded retry budget`，不是把启动或 collector 失败当作协议结果。Spec184
Authority selectors 在同一 sweep 中通过。原始日志和 SHA-256 见
[B5 component evidence](evidence/b5-component-validation-20260911.md)；因此 T006/T007
保持 `PARTIAL`/`NOT_STARTED`，仍需候选冻结、fresh convergence、process/no-Python 和负例资格。

2026-09-11 **D-UAV-TRIM / CLOSED_FOR_VALIDATION (documentation only)**：按用户要求删除
UAV update PDF 原第4/5页，现4页；双遍构建及全页渲染通过，两份导出同步。
[证据](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md)。不推进 B1–B5，原生下一步保持不变。

2026-09-11 **D-UAV-CASE / CLOSED_FOR_VALIDATION (documentation only)**：
[UAV update PDF](../../docs/NDNSF-UAV/slides/UPDATES_UAV.pdf) 已按案例价值审查修订，6页双遍构建、
文本密度与渲染检查通过；[证据](../../docs/NDNSF-UAV/slides/UPDATES_UAV-review.md)。
无产品/API/实验变化，不推进本 Spec 的 B1–B5；下一原生工作仍按下述文档门禁复核后进入 B1/T001。

文档移交和执行门禁已建立；产品实现0个任务完成，下一步为文档门禁复核后进入 B1/T001。
Spec182 的14个 OPEN 父任务和 R12-A–E 全部承接；未搬运历史长记录，既有 PASS 只保留原证据范围。
本轮只修改 Spec184/历史指针文档，未改产品代码、未编译/运行产品测试；迁移文档的结构 PASS
不授权产品实现或 qualification。迁移记录见 [migration record](evidence/migration-20260911.md)，本轮门禁收口见
[documentation gate repair](evidence/documentation-gate-repair-20260911.md)。

2026-09-11 **T007-I02-I08-DYNAMIC-SAMPLES / PASS_FOR_DYNAMIC_SAMPLE**：以同一
`build-spec184-b5-candidate` 和 candidate-staged native fixtures 运行 I02–I08 C++ owner
selectors。I02 双 Provider/epoch、I03 四 Provider stream、I04 publication reorder、I05
duplicate、I06 loss/retry、I07 三个负例和 I08 cancellation 均由 C++ business oracle 通过；
每个有效 run 的 observation complete、七类 evidence category 和 cleanup 均完整。I02 的
`r1`/`r2`/`r3` 仅为 `ifconfig`、fixture staging 和 manifest-kind harness 边界，已写入
[failure log](../../docs/failure-log.md)。这些样本不替代 `native-isolation-design.md` 要求的
fork/Python ELF、libpython mapping、endpoint、collector fault、cold/role 和 descendant
counterexample，因此 T007 继续 `IN_PROGRESS`/`PARTIAL`，T008 未开始。原始哈希和路径见
[T007 process qualification refresh](evidence/t007-process-qualification-20260911.md)。

2026-09-11 **T007-SPEC175-G2 / PASS_FOR_DYNAMIC_SAMPLE**：同一 candidate
`integration-tests` 以 seed `1840012` 单次运行注册的 Spec175 I01–I15，15/15 case exit `0`，
无 missing case。gate manifest 和 current source-seal 已记录在 [T007 process qualification
refresh](evidence/t007-process-qualification-20260911.md)，并纳入 promotion candidate 的
`evidence.t007_process` digest。该 gate 补充 I09–I15 行为类样本；source-seal 中的预存
integration marker 与无关 extension-build log 仍不属于 candidate production source。继承
I02–I08 isolation counterexample/collector completeness、真实模型、Python retirement 和
external SIF/Tiger 仍未关闭，T007 继续 `IN_PROGRESS`/`PARTIAL`。

2026-09-11 **T007-NO-PYTHON-ELF / PASS_FOR_ROW**：对当前
`build-spec184-b5-candidate` 的六个业务二进制独立执行 `readelf -d`、`strings` 和
`/usr/bin/ldd -r`。每个文件均无 `libpython`/`python3` 的 `DT_NEEDED` 或字符串身份，
`ldd -r` exit `0` 且无 unresolved/missing symbol；摘要及哈希见 [T007 process qualification
refresh](evidence/t007-process-qualification-20260911.md)。这只关闭候选 ELF 静态闭包行，
不替代运行时 I02–I08 counterexample/collector、真实模型或外部 owner 资格。

2026-09-11 **T007-I02-I08-COUNTEREXAMPLES / PASS_FOR_COUNTEREXAMPLE**：补充的 C++ fixture
`tests/standalone/spec182-native-counterexample.cpp` 在当前 canonical runner 与 root MiniNDN
owner 下命中冻结注册表：I02/I03/I04/I06/I08 为可观察 `FAIL`，I05 为
`UNQUALIFIED`（trace budget 观察边界），I07 为 `PASS`。每个 run 具有独立 node context、
runner result、trace 和清理记录；I03 实际打开 staged `libpython3.8.so.1.0` 后由
`PYTHON_MAPPING` 拒绝，I08 记录 detached descendant 的 `OWNED_PROCESS_ALIVE`。详见
[T007 process qualification refresh](evidence/t007-process-qualification-20260911.md) 的
`Candidate-bound C++ isolation counterexamples`。该出口关闭反例样本本身，不提升 T007
总体状态；真实模型 breadth、Python retirement、外部 SIF/Tiger 与 I05 的正式资格仍未完成。

2026-09-11 **T007-I02-I08-STATIC-REVIEW / STATIC_PASS**：按官方 `review-agent` 五条覆盖线
复核 canonical collector/evaluator、C++ counterexample fixture、回归 selector、注册/构建
接线及 candidate/matrix/evidence/task 绑定；未遗留静态问题。复核与文件哈希见
[T007 process qualification refresh](evidence/t007-process-qualification-20260911.md) 的
`Static review and detector batch record`。此静态门不改变 T007 的运行时资格边界。

2026-09-11 **T007-TARGETED-UNIT-ORDER / UNQUALIFIED**：当前 candidate 的 151 个 C++ 定向
套件首次合并运行在 `Spec182V3Placement/PublicClientConversationCommitsSeededReceiptAndCheckpoint`
先观察到 `DI_NATIVE_OFFER_REJECTED`，随后出现内存访问错误（exit `134`）。独立运行该用例三次、
同一 151-case selector 重新运行三次均 exit `0`；因此暂记为顺序相关的失败边界，保留原始日志
`.codex-tmp/spec184-b5-targeted-unit-20260911/unit.log`，待 sanitizer 或压力复现后再决定是否
关闭。该结果不提升 T007，也不把失败尝试计为资格 PASS。

2026-09-11 **T007-FULL-UNIT-ENV / PASS_FOR_ROW**：使用
`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate` 重新运行当前 candidate 的完整 C++ unit
套件，exit `0`，耗时 `1:49.15`，最大 RSS `8396500 KB`；日志及 SHA-256 见
[T007 process qualification refresh](evidence/t007-process-qualification-20260911.md)。先前漏传该
环境变量导致的 exit `201` 仅为测试入口配置失败，已保留在独立 raw run，不计入资格结果。

同一 151-case selector 在相同 candidate 环境下额外重复十次，全部 exit `0`；原始日志目录
`.codex-tmp/spec184-b5-targeted-placement-stress-20260911/` 已写入 [T007 process qualification
refresh](evidence/t007-process-qualification-20260911.md)。这只是稳定性补充，原先一次
顺序相关失败仍保留为待 sanitizer/压力复现的边界。

2026-09-11 **T007-PYTHON-HARNESS-REGRESSION / PASS_FOR_ROW**：单独运行
`python3 -m pytest -q tests/python/test_spec182_native_closure.py`，46 tests 在 `0.20s`
内全部通过；日志和 SHA-256 见 [T007 process qualification refresh](evidence/t007-process-qualification-20260911.md)。
这只验证 collector/closure 的 Python harness 回归，不把 Python 提升为业务 owner，也不关闭
Python retirement 资格行。

2026-09-11 **T007-FULL-INTEGRATION-ENV / PASS_FOR_ROW**：在同一 candidate 环境中使用
`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate` 完整运行 C++ integration 套件，exit `0`，
耗时 `4:09.61`，最大 RSS `114048 KB`；原始日志和 SHA-256 见 [T007 process qualification
refresh](evidence/t007-process-qualification-20260911.md)。该结果关闭当前 candidate 的
integration 行，但不关闭真实模型、Python retirement、I05 正式资格或外部 SIF/Tiger owner。

2026-09-11 **T007-YOLO-CURRENT-CANDIDATE / PASS_FOR_ROW**：当前候选
`build-spec184-b5-candidate-r4` 的 `spec180-native-build.json` 已通过 `verify`，构建使用
系统 `/usr/bin/g++ -B/usr/bin` 与 `-j4`；普通 DI 源码变更复用已验证边界，receipt 记录
`binding_reused=true`。同一候选完成 root MiniNDN 的 YOLO26n Y-A（`r51`）与 Y-B（`r37`）真实
C++ 请求，Y-A/Y-B 数值 oracle 均 `matched=true`，shape 为 `[1,50,6]`，最大绝对误差
`0.0005340576171875`，Provider 日志确认 `onnxruntime-cpu`、`realCompute=true`、load/warmup/
execution 完成；Y-N（`r50`）七个子项及 EXPIRED/FORGED_AUTHORITY/WRONG_RECIPIENT 三个
Provider 授权变异均 `PASS`，无 cleanup error。Python 仅负责 MiniNDN 编排和证据收集，业务
oracle 与执行仍为 C++。本机只能运行 Qwen3-0.6B smoke，Qwen3.6-27B 保持
`WAITING_EXTERNAL_INPUT`；继承 negative/retirement、I05、SIF/Tiger 仍未闭合，因此 T007
总体保持 `IN_PROGRESS`/`PARTIAL`，T008 继续 `BLOCKED_BY_T007`。详见
[T007 process qualification](evidence/t007-process-qualification-20260911.md) 与
[remainder audit](evidence/remainder-audit-20260911.md)。

## Phase 1: Request Correctness

- [x] T001 [US1] **Authority IO Dispatch**. FR-001；修复 F-01，在 `NativeAuthenticatedGrantClient::coreIssue` 将 `ServiceUser::RequestServiceTargeted` 封送到 Core `postToIo`，处理 dispatch 前取消、空 request ID、异常、timeout 与晚回调；在 `tests/integration-tests/di-native-requester-grant.t.cpp` 的 `Spec184AuthorityIoOwnership` 中验证线程 owner、真实 `ServiceUser` 状态和 bounded cleanup。Risk class: `concurrency/lifetime`; Dynamic profile: `tsan`; invariants: IO owner、pending-call balance、late callback no-op。Target: `integration-tests`（已由 `tests/wscript` 注册 TU）。Dependencies: documentation gate and migration baseline。Evidence: [B1 evidence](evidence/b1-request-correctness-20260911.md)。
- [x] T002 [US1] **Turn Publication Synchronization**. FR-002；修复 F-02，核对 `NativeInferenceClient` turn/attempt mutable-state、publication linearization 和 ticket 清理；在 `tests/unit-tests/di-native-client.t.cpp` 的 `Spec184TurnPublicationRace` 中覆盖 cancel/deadline/close/replacement。Risk class: `concurrency`; Dynamic profile: `tsan`; invariants: ticket publication/abort linearization、stale-token rejection、terminal callback fencing。Target: `unit-tests`（unit glob 注册）。与 T001 组成 B1，逐任务静态门后统一运行交错用例。Dependencies: T001 static gate。Evidence: [B1 evidence](evidence/b1-request-correctness-20260911.md)。
- [x] T003 [US1] **Durable Outcome Linearization**. FR-003；修复 F-03，client/coordinator publish 与 handle outcome 一致；在 `tests/integration-tests/ndnsf-di-core-flow.t.cpp` 的 `Spec184DurableOutcome` 中覆盖 publish 后阻塞 FINALIZE、取消和超时。Risk class: `lifetime/linearization`; Dynamic profile: `asan-ubsan`; invariants: journal/handle lifetime、publish-after-cancel fencing、residue=0；普通 C++ selector 通过，独立无抑制 sanitizer selector 三次通过。Target: `integration-tests`。Dependencies: B1 behavior exit。Evidence: [B2 evidence](evidence/b2-durable-outcome-20260911.md)。

## Phase 2: Export and Callers

- [x] T004 [US2] **Atomic Private Checkpoint Export**. FR-004；修复 F-04，`DI_NativeRequester` 以同目录临时文件、`0600`、write/`fsync`/close/rename/目录 `fsync` 安全导出，明确 symlink 不跟随、失败保留和 round-trip；补 `tests/unit-tests/di-native-checkpoint.t.cpp` 的 `Spec184CheckpointExport`。Risk class: `lifetime/serialization`; Dynamic profile: `asan-ubsan`; invariants: temp-file ownership、loader lifetime、old checkpoint preserved on pre-rename failure。普通 C++ selector、独立无抑制 ASan/UBSan selector（三次）及候选目录优先的 example loader smoke 通过；目录 `fsync` 真实错误仍为显式限制。Target: `unit-tests`。Dependencies: B2 exit。Evidence: [B3 evidence](evidence/b3-checkpoint-export-20260911.md)。
- [x] T005 [US2] **Maintained Caller Mode Closure**. FR-005；按 [caller matrix](contracts/caller-matrix.md) 盘点五组维护入口及其模式、默认路由、native/compatibility/removed 状态、C++ oracle、zero-use 和 rollback；按共享逻辑分组迁移并检查薄绑定。Risk class: `routing/lifetime`; Dynamic profile: caller/launcher rows use `none` with reason because no new async owner is introduced; shared owner profiles remain authoritative; bounded parameter matrix covers native, compatibility, invalid-config and shutdown boundaries。当前 C++ route/provider/Qwen selectors and 38 Python route tests passed；旧 D2b runtime miss、real model/no-Python and retirement remain T006/T007。Dependencies: B3 exit。Evidence: [B4 evidence](evidence/b4-caller-convergence-20260911.md)。

## Phase 3: Qualification and Delivery

- [x] T006 [US3] **Inherited Obligation and Harness Closure**. FR-006；维护 [qualification matrix](contracts/qualification-matrix.md)，逐项对账原14个 OPEN 父任务、PO-001–016及适用 I/FR/CD/INV（80 行），并为每行给出 obligation、component、harness、identity、owner 和 external-owner 边界。仍未运行的组件、负例、真实模型或 external rows 已保留为 `PARTIAL`/`OPEN` 并转交 T007/T008；没有隐藏缺口。Risk class: `qualification-evidence`; Dynamic profile: `none` for documentation reconciliation, while inherited rows retain their matrix-assigned profile; invariants: source/artifact identity、selector-to-obligation mapping and bounded parameter matrix。Dependencies: B4 exit；矩阵、candidate identity 与 fresh convergence 已通过。Evidence: [B5 matrix evidence](evidence/b5-matrix-binding-20260911.md)、[current candidate](contracts/promotion-candidate.md)、[convergence audit](evidence/convergence-b5.md)。
- [ ] T007 [US3] **Current Native Qualification**. FR-006；仅在 T006 的 qualification matrix 完整且 `evidence/convergence-b5.md` 为当前 candidate 的 fresh `PASS` 后，按矩阵运行同源完整 unit/integration、YOLO/Qwen MiniNDN/no-Python 与检错负例；绑定源码/二进制/日志，区分局部 PASS 和正式 qualification；不重跑未受影响的历史实验。Risk class: `qualification-runtime`; Dynamic profile: profiles derived from the matrix, with one bounded dynamic sample per distinct risk/behavior class (at minimum `tsan` for concurrency rows and `asan-ubsan` for lifetime/parser rows); invariants: candidate identity、terminal cleanup、negative boundary。Dependencies: T006 static/focused exits and fresh convergence `PASS`。
- [ ] T008 [US3] **Native Development Handoff**. FR-006；同步 Design/API/使用说明、两个入口示例、最终源码基线、剩余外部实验 TRANSFERRED 状态；不得以文档移交替代本地资格。Risk class: `documentation`; Dynamic profile: `none` (no runtime state); invariants: evidence links and status agreement。Dependencies: T007 PASS。

## Dependencies & Execution Order

B1(T001/T002) → B2(T003) → B3(T004) → B4(T005) → B5(T006–T008)。
所有 T ID 属于184；引用182时必须加 Spec 前缀（例如 `182:T005`）避免歧义。
T005/T006 的子组在所属矩阵维护，不继续在 tasks.md 堆积上百个 G 编号。
静态审查通过但批次 C++ 验证或适用动态 profile 未通过时保持 `[ ]` / PARTIAL；每批只维护一个结果记录。
任务状态不能由 migration record、文档结构 PASS、CLI smoke 或 Python wrapper 结果提升；
T007 也不能替代 T001–T006 的定向 C++ 出口。

## Validation

每个任务的 C++ test/negative oracle 见 spec Acceptance Evidence Contract 和注册表；批次组合
审查通过后统一运行相关验证。T007 是最终 qualification，不替代前置批次定向 test，且必须
服从 [promotion candidate](contracts/promotion-candidate.md) 的失效矩阵。
