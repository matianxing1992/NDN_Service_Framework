# Tasks: Prepared Model Runtime

**Status**: PLANNED | **Date**: 2026-09-12
**Input**: [spec](spec.md) · [plan](plan.md) · [C-01](contracts/public-api.md) · [C-02](contracts/preparation.md) · [C-03](contracts/execution.md) · [C-04](contracts/validation.md) · [C-05](contracts/api-usability.md) · [C-06](contracts/cpp-first.md) · [C-07](contracts/api-catalog.md) · [C-08](contracts/code-design.md) · [C-09](contracts/core-app-boundary.md)

## Execution Progress

### Proposal Side-by-Side Checkpoint — 2026-09-14

DOCUMENT_PASS：依用户要求将密集标记稿替换为79页左右对照，19主题索引、130个1:1完整页面面板；59／50页源稿全部覆盖，文本精确保留、光栅容差与渲染检查通过。旧110页稿保留于ab9ee6f7。仅呈现变化，不改变正文或native任务；见[side-by-side evidence](evidence/proposal-side-by-side-20260914.md)。

### Proposal PDF Comparison Checkpoint — 2026-09-14

DOCUMENT_PASS：完整59页Origin与当前50页英文proposal的110页文字标记PDF完成；109页文字／图片数量／页面尺寸保留，输入哈希不变，110页渲染越界0页，6项匹配回归通过。仅比较副本，不改正文、slides或native任务勾选；见[comparison evidence](evidence/proposal-pdf-comparison-20260914.md)。

### Proposal Positioning Checkpoint — 2026-09-14

DOCUMENT_PASS：dissertation proposal组织与口径修订完成，独立目录八入口构建、49页PPTX回读、1,006/1,006可编辑文字spans及notes parser 2/2通过；RQ、16个被包含模块及六张实验页正文保持。首轮时间线页溢出修复且原日志保留。仅文档工作，不改变native任务勾选或资格；见[document evidence](evidence/proposal-structure-20260914.md)。

### Documentation Checkpoint — 2026-09-14

Proposal／slides 批注修订完成：英文50页、中文38页、slides49页；八入口构建与镜像检查、49页PPTX回读、1,006/1,006可编辑文字spans、notes parser 2/2通过。19条PDF／26条PPTX批注逐项记录；研究证据仍开放。本项不变更Spec185源码、API、任务勾选或native qualification。详见 [documentation evidence](evidence/proposal-advisor-review-20260914.md)。

**Progress Timestamp**: `YYYY-MM-DD HH:mm ±HH:MM`，项目时区`America/Chicago`；Updated为该行最后修订时间，不是完成时间。
本表于`2026-09-12 16:24 -05:00`升级时间格式，原18行仅记录`2026-09-12`，精确历史事件时间UNKNOWN；本次统一时间仅表示格式迁移。后续只更新状态、依赖、证据或剩余项实际变化的行，规则见[task progress](../../skills/speckit-code-design/references/task-progress.md#progress-timestamp)。

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T015 Installed C++ API and ABI Closure](#t015) | PASS | none | B0 closed; [b0-installed-api](evidence/b0-installed-api.md) covers v17 STATIC_PASS, four C++ consumers, 67 headers, negative gate, ABI/ldd/hash | 2026-09-12 16:24 -05:00 |
| [T017 Core Operation Runtime and Channels](#t017) | PASS | B0 exit | B0C closed; [b0c-core-operation](evidence/b0c-core-operation.md), PO-C1,C2 C++/TSan/installed-consumer PASS | 2026-09-12 16:24 -05:00 |
| [T018 DI Delegation to Core Operations](#t018) | PASS | T017 static | B0C closed; [b0c-core-operation](evidence/b0c-core-operation.md), PO-C3,C4 C++ real-provider/regression PASS | 2026-09-12 16:24 -05:00 |
| [T001 Runtime Configuration and Export](#t001) | PASS | B0C exit | B1 closed; [b1-runtime](evidence/b1-runtime.md) static/compile-link/runtime PASS; later request path remains open | 2026-09-12 16:24 -05:00 |
| [T002 Runtime Shutdown and Child Ownership](#t002) | PASS | T001 static | B1 closed; [b1-runtime](evidence/b1-runtime.md) normal/TSan/installed C++ lifecycle PASS; owner-thread public path remains unobserved | 2026-09-12 16:24 -05:00 |
| [T016 Extension Registration and Cooperative Control](#t016) | PASS | B1 exit | B2E closed; [b2e-extensions](evidence/b2e-extensions.md) static/compile-link/runtime/TSan/installed C++ PASS; full packaging remains unobserved | 2026-09-12 16:24 -05:00 |
| [T003 Verified Package Preparation](#t003) | PASS | B2E exit | B2 closed; [b2-preparation](evidence/b2-preparation.md) covers static/combination review, normal/TSan C++ preparation and affected Runtime/Core suites | 2026-09-12 21:07 -05:00 |
| [T004 Single Flight Refresh and Leases](#t004) | PASS | T003 static | B2 closed; [b2-preparation](evidence/b2-preparation.md) covers static/combination review, normal/TSan C++ preparation and lease/refresh/concurrency cases | 2026-09-12 21:07 -05:00 |
| [T005 Prepared Request Projection](#t005) | PASS | B2 exit + Spec184 scoped dependency gate | B3 closed; [b3-request](evidence/b3-request.md) covers static/combination review, normal and ASan/UBSan C++ prepared-request selectors (12/12 x 2 each) | 2026-09-13 07:50 -05:00 |
| [T006 Handle Deadlines Events and Cancellation](#t006) | PASS | T005 static | B3 closed; [b3-request](evidence/b3-request.md) covers handle/deadline/event/cancel/drain cases, normal and ASan/UBSan C++ extension/request selectors (10/10 and 12/12 x 2 each) | 2026-09-13 07:50 -05:00 |
| [T007 Prepared Conversations and Committed Checkpoints](#t007) | PASS | B3 exit | B4 closed; final composition `B4_COMPOSITION_PASS`, normal v26 and ASan/UBSan+LSan v19 builds, conversation r49/r50 and r40/r41, API r51/r52 and r42/r43 all `RC=0`; C++ two-turn, checkpoint, close/drain and unauthenticated rejection evidence in [b4-conversation](evidence/b4-conversation.md) | 2026-09-13 13:01 -05:00 |
| [T008 Conversation Recovery Replacement and Export](#t008) | PASS | T007 static | B4 closed; final composition `B4_COMPOSITION_PASS`, normal v26 and ASan/UBSan+LSan v19 builds, conversation r49/r50 and r40/r41, API r51/r52 and r42/r43 all `RC=0`; C++ recovery, export/import and replacement isolation evidence in [b4-conversation](evidence/b4-conversation.md) | 2026-09-13 13:01 -05:00 |
| [T009 Provider Facade and Authenticated Assembly](#t009) | PASS | B4 exit | B5 closed; T009 static v11, T010 v67 and final composition v5 `PASS`; normal compile v22/runtime v21-v23 and sanitizer compile v3/runtime v2-v3 all `RC=0`, 8/8 C++ cases; [b5-provider](evidence/b5-provider.md#b5-composition-review-pass-v5-and-closure) | 2026-09-15 13:05 -05:00 |
| [T010 Protected Artifact and Runner Template Reuse](#t010) | PASS | T009 static | B5 closed; T010 static v67 and final composition v5 `PASS`; normal compile v22/runtime v21-v23 and sanitizer compile v3/runtime v2-v3 all `RC=0`, 8/8 C++ cases; [b5-provider](evidence/b5-provider.md#b5-composition-review-pass-v5-and-closure) | 2026-09-15 13:05 -05:00 |
| [T011 Native Caller Migration and Compatibility Registry](#t011) | PASS | B5 exit | B6 closed; v12 static/composition pass; 359/359 `-j4` build, C++ compatibility selector, in-tree and external installed consumers, public example checks, ELF/no-Python closure, and fresh C++ unary/stream process oracles all pass; full qualification remains B7; [b6-migration](evidence/b6-migration.md) | 2026-09-14 01:00 -05:00 |
| [T013 Current Candidate Process Qualification](#t013) | PASS | B6 exit | Final B7 C++ process matrix passes on normal and ASan/UBSan candidates after rebuilding the complete source/external closure; installed C++ caller and 24-artifact ELF/no-Python receipt pass. Leak-enabled ASan remains an external OpenABE limitation and is recorded separately; Python SC-005 remains T012. [b7-cpp-qualification](evidence/b7-cpp-qualification.md#b7-final-candidate-convergence-20260915) | 2026-09-15 06:10 -05:00 |
| [T012 Thin Python Prepared Model Facade](#t012) | PASS | B7 C++ qualification exit | B8 closed; v19 static PASS, DI/extension compile-link PASS, refreshed C++ selectors and 31 Python binding/compatibility tests PASS; subinterpreter and packaging-wheel stress remain unobserved. [b8-python](evidence/b8-python.md) | 2026-09-15 07:50 -05:00 |
| [T014 Design API and Scoped Handoff](#t014) | PASS | T012 acceptance | B9 closed; static review v05 and final composition `B9_COMPOSITION_PASS`; current API/behavior/source snapshot, scoped current-vs-target design, three diagram views and dual-PDF identity/layout all pass; Spec184 outstanding registry retained; [b9-handoff](evidence/b9-handoff.md) | 2026-09-15 13:05 -05:00 |
| [T019 Prepared Client Ownership and Eviction](#t019) | PASS | T003/T005 static | B7R static v3, normal and ASan/UBSan C++ eviction/source-lifetime selector `RC=0`; [b7r-lifecycle-fixes](evidence/b7r-lifecycle-fixes-20260915.md) | 2026-09-15 02:36 -05:00 |
| [T020 Conversation Terminal Admission Ordering](#t020) | PASS | T007 static | B7R follow-up static `PASS`; normal and ASan/UBSan C++ selectors plus repeated selectors `RC=0`; delayed completion callback gate, failed-turn `CANCELLED` immediate replacement, and successful result-to-next-turn oracle observed; [b7r-lifecycle-fixes](evidence/b7r-lifecycle-fixes-20260915.md#t020-follow-up-delayed-completion-and-immediate-retry) | 2026-09-15 03:16 -05:00 |
| [T021 Generation Identity and Grant-Bound Provider Cache](#t021) | PASS | T007/T010 static | B7R v9 static `PASS`; production protected Provider matrix with two independent grants, canonical source fetch, assembly, runner creation and execution passes normal and ASan/UBSan C++ selectors `RC=0`; [b7r-lifecycle-fixes](evidence/b7r-lifecycle-fixes-20260915.md#t021-follow-up-production-protected-independent-grant-matrix) | 2026-09-15 04:00 -05:00 |

## Current Checkpoint

2026-09-15 13:05 -05:00 B9 T014：按 `Design/MANAGEMENT.md` 刷新当前 API/绑定参考、行为覆盖与源码快照；`test_design_state.py`、`verify-api-reference.py`、`verify-source-baseline.py` 均 `PASS`（327 文件、17,589 声明、1,114 绑定、514 快照文件）。当前/目标双 PDF 在 `.codex-tmp/design-pdf-20260915T130303244487Z/` 构建并通过 `verify.py`（91/98 页、59/65 目录章节、字体嵌入、无版面警告）；当前设计明确 PreparedModel 原生路径和兼容 `NATIVE_REQUEST_PIPELINE_NOT_READY` 分支，目标设计保持独立。B5 T009/T010 的 final composition v5、normal/sanitizer C++ 运行已在本 checkpoint 复核为 PASS；Spec184 Qwen3.6-27B、继承 negative/retirement、I05、SIF/Tiger 和 B8 subinterpreter/wheel 等未观测项仍保持原状态。T014 已通过静态审查 v05 与 `B9_COMPOSITION_PASS`，现标记 `PASS`。详见 [B9 handoff](evidence/b9-handoff.md)。

2026-09-15 11:46 -05:00 Tiger SIF+APP local-first tooling：已实现基础 SIF + 外置 `/opt/ndnsf-app` APP 的本地交付入口，准备本地 checkpoint；r29 实现经官方 `review-agent` 静态通过，r30 证据更新复核同样 `STATIC_PASS`。78 项离线脚本/manifest 测试、Python/Bash/ShellCheck 和 diff 门禁通过；当前机器没有有效 regular base/candidate SIF，因此 pair materialization、C++ 请求链、Apptainer runtime、Slurm/Tiger 仍未观测，不改变 T013/T014 的产品任务状态。详见 [SIF+APP static evidence](../../Experiments/TigerCluster/docs/sif-app-static-check-20260915.md)。

2026-09-15 04:00 -05:00 B7R T021 follow-up：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t021-review-v9` 返回 `STATIC_PASS`，manifest SHA256=`37b893574652b639ee1d814d9b5e04be67e89fa61ab9c1d26b65f1b461db403d`、diff SHA256=`d7fc266003e3b7ce305ba1b7d6f28a35fb1895795d35c61f0cbdb044936fdb52`，无 P0-P3。修复 exact-forward cache runner 地址 ABA：外部 registry 维护单调 runner identity 并在析构移除，公共基类布局保持不变。`spec185-provider-assembly` normal `-j4` build 58.876s、ASan/UBSan `-j4` build 91.044s 均 `RC=0`；两次独立 grant 的 production protected Provider selector normal/ASan 均 `RC=0`、`*** No errors detected`，分别观察 sourceFetches=2、assemblies=2、templateHits=0、runnersCreated=2 与 runner execution=2。T021 已 PASS；T013、T012/T014 仍保持各自状态。详情见 [T021 follow-up](evidence/b7r-lifecycle-fixes-20260915.md#t021-follow-up-production-protected-independent-grant-matrix)。

2026-09-15 06:10 -05:00 B7 T013 final convergence：先保留只构建 `spec185-process` 导致旧外部 requester ABI 的失败（requester `-11`）；随后以系统优先 `-j4` 重建 normal 与 ASan/UBSan 的完整 DI、process、prepared-selector、integration 和外部 requester/provider/authority/controller/worker 闭包。normal process 五 case×2、ASan/UBSan（`detect_leaks=0`）五 case×2 均 `RC=0`、`*** No errors detected`；独立 drain selector 两次通过。24-artifact ELF/readelf/ldd receipt 为 `CLOSURE_PASS=True`、`NO_PYTHON_NEEDED=NONE`、`ELF_LDD_ERRORS=NONE`；仓库外当前公共头/库 staged prefix 的 C++ caller consumer 通过。Leak-enabled ASan 的唯一边界是 `/usr/local/lib/libopenabe.so` 16项策略树分配泄漏，单独保留为外部依赖限制，不伪造 sanitizer PASS。T013 已 PASS；T012/T014 仍按依赖保持未完成。详见 [B7 final candidate convergence](evidence/b7-cpp-qualification.md#b7-final-candidate-convergence-20260915)。

2026-09-15 07:50 -05:00 B8 T012：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t012-review-v19-20260915` 返回 `STATIC_PASS`，manifest 已记录且无 P0-P3；v15/v17/v18 的 translator 与 teardown fixture findings 均保留。系统优先 `-j4` 重建 DI 受影响闭包 `RC=0`（2:24.38）及 Python extension `RC=0`（r4）；同一候选运行 `test_spec185_prepared_model.py` 9/9、继承 `test_spec182_native_bindings.py` 合计31/31，真实 C++ `spec185-process` 5 cases、`spec185-prepared-request` 15 cases、`spec185-prepared-conversation` 16 cases 均 `RC=0` 且无 Boost 错误。修正了标准异常 translator 吞咽和继承测试对已退役 requester parser 的过时断言；新增子进程 `DiError` 字段/模块退出回归。B8 已 PASS；Python subinterpreter、wheel packaging 和 wrapper sanitizer 仍未观测。详见 [b8-python](evidence/b8-python.md)。

2026-09-15 03:16 -05:00 B7R T020 follow-up：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t020-review-v01cytpt` 返回 `STATIC_PASS`，manifest SHA256=`8d882b58a426f004617ab10ee641f3f3db3e6c33b02f406b3025b26cd9835437`，无 P0-P3。`spec185-prepared-request` normal 与独立 ASan/UBSan `-j4` 构建均 `RC=0`；同一 conversation selector normal、ASan/UBSan 各运行两次，均 `RC=0`、`*** No errors detected`。C++ 夹具用共享 native worker 上的公开 completion callback gate 延迟会话失败回调，确认 `CANCELLED` 结果可见后立即 replacement；首个成功 `result(0)` 后立即提交下一 turn，后续 checkpoint/commit/recovery/drain 继续通过。T020 已 PASS；T021、T013、T012/T014 仍按各自未观测项保持原状态。详情见 [T020 follow-up](evidence/b7r-lifecycle-fixes-20260915.md#t020-follow-up-delayed-completion-and-immediate-retry)。

2026-09-15 02:36 -05:00 B7R lifecycle fixes：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-lifecycle-fixes-review-v3` 返回 `STATIC_PASS`，manifest SHA256=`dbbe150f1c03f3fc5b509e01e19b43724ed5882719ea94f5c0e81d1edc201736`，无 P0-P3。Runtime client registry 改为 weak index、PreparedModel 共享惰性 client、会话终态在结果可见前释放 turn gate、默认 generation identity 合并后归一化；Provider 受保护 cache 明确按 Provider/grantName/grantDigest 隔离。normal `-j4` 与独立 ASan/UBSan `-j4` 受影响目标构建均 `RC=0`，新增缓存淘汰、两轮会话和独立授权 cold/hit/cold C++ selector 均 normal/ASan `RC=0`、无 sanitizer 报告。T019 已 PASS；T020/T021 因人工延迟/失败会话压力及受保护 Provider 跨独立授权生产请求尚未观测保持 `PARTIAL`；T013 的 source/ELF/no-Python convergence、T012/T014 仍未完成。详见 [b7r lifecycle evidence](evidence/b7r-lifecycle-fixes-20260915.md)。

2026-09-15 01:50 -05:00 B7 served-provider integration：新增 C++ `PreparedRequestCompletesThroughServedProvider`，从 Runtime prepare、assignment/ACK、authenticated grant、Provider serve/runner 到 response 完整走通；normal focused、完整 `Spec185Process`（5 cases，含38个隔离 native selector）及 ASan/UBSan focused/full matrix 均 `RC=0`、无 sanitizer 报告。过程中真实发现并修复测试/接线缺陷：缺失头文件、deferred bridge 引发的 borrowed-Face 竞态、publication source 未授权、`onnxruntime-cpu` factory 名称不一致；一次 drain selector 超时经隔离复跑和完整矩阵复跑通过。T013 仍保持 `PARTIAL`，因为本轮未重新声明最终 source/ELF/no-Python identity convergence 或文档闭合；详见 [b7 evidence](evidence/b7-cpp-qualification.md#b7-served-provider-integration-and-qualification-20260915)。

2026-09-14 22:50 -05:00 Request-chain static audit：定向审查已实现公共请求/native/Core/Provider/会话终态链，发现F01会话准入晚于结果通知、F02客户端持有目录绕开prepared-cache释放计费两项P2；G01跨授权缓存复用、G02 B5/B6状态一致性、G03当前完整资格仍需核对。见[报告及修复顺序](evidence/request-chain-static-audit-20260914.md)与[源码身份](evidence/request-chain-static-audit-20260914-manifest.json)。本轮只读产品源码，无构建/运行、无源码修复、不改变任务勾选；整体NOT_STATIC_PASS，下一步先处理F01/F02及对应C++反例。

2026-09-13 22:32 -05:00 B5 closed：v67 修复候选通过官方最终 composition review v5（`B5_COMPOSITION_PASS`，无 P0/P1/P2/P3）；normal compile v22、focused/runtime v21-v23、独立 ASan/UBSan+LSan compile v3/runtime v2-v3 均真实 C++ `RC=0`，8/8 cases 且无 sanitizer 报告。T009/T010 的五 lane、生产调用链和 stop/drain/reaper 出口已闭合，现标为 `[x]`；下一批从依赖满足的 T011 开始。
2026-09-14 05:24 -05:00 B5 dynamic validation：v67 静态修复候选 normal compile v22 通过，authenticated focused v21 及完整 selector v22/v23 各 8/8 通过；独立 ASan/UBSan+LSan compile v3、完整 selector v2/v3 各 8/8 通过且无 sanitizer 报告。v23 UAF 已由 dedicated provider Face worker 与 deferred bridge fixture 修复验证；B5 最终 composition/closure 尚待只读审查，T009/T010 保持 `PARTIAL`。
2026-09-14 05:45 -05:00 B6 T011 static v9：官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-t011-review-v9-20260914` 返回 `STATIC_FAIL`（P1）；取消后的 readiness retry 仍可持续排 timer。未构建/运行，已在 v10 修复并保留首个失败边界。
2026-09-14 05:45 -05:00 B6 T011 static v10：官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-t011-review-v10-20260914` 返回 `STATIC_PASS`，随后 `B6_COMPOSITION_PASS`，无 P0/P1/P2/P3；五 lane 静态覆盖，compile-link/runtime/sanitizer 尚未执行。T011 保持 `PARTIAL`。
2026-09-14 05:45 -05:00 B6 unary v2/v3：新 Runtime C++ requester 真实进程分别在相同生产边界失败，controller/authority/provider 已启动；首个 requester 失败为 `NATIVE_REQUEST_BEGIN_FAILED`，底层日志是 NAC 解密 readiness 未就绪且 Runtime 未发起 user permission bootstrap。原始运行根目录 `.codex-tmp/spec185-b6/unary-v2/`、`unary-v3/`，已修复生产接线，待 v10 候选 `-j4` 重建和 fresh process 验证；T011 保持 `PARTIAL`。
2026-09-14 00:49 -05:00 B6 T011 static v12：官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-t011-review-v12-20260914` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；显式 `<cstdlib>`、成对 PIB/TPM 校验、外部 KeyChain 复用、memory fallback、异常清理及 v10 retry/cancel 不变量通过。compile-link/runtime/sanitizer 尚未执行，T011 保持 `PARTIAL`。
2026-09-14 00:49 -05:00 B6 unary v4：v10 readiness 修复后的真实 C++ requester 已到达 permission bootstrap，但 Controller 使用进程 PIB 证书加密，Runtime 使用不同 memory 证书，解密失败并以 `NATIVE_REQUEST_BOOTSTRAP_TIMEOUT` 结束；无 ACK/Selection，原始根目录 `.codex-tmp/spec185-b6/unary-v4/`，已加入 v12 外部 PIB/TPM 复用修复，待新构建和 fresh unary/stream 验证；T011 保持 `PARTIAL`。
2026-09-14 00:49 -05:00 B6 unary v5：launcher 使用错误的相对 NAC-ABE 库路径，在 Python wrapper import 阶段缺少 `ndn::nacabe::Consumer::clearCache`，未启动协议进程；原始根目录 `.codex-tmp/spec185-b6/unary-v5/`，已记录为环境边界并修正重试命令；T011 保持 `PARTIAL`。
2026-09-14 01:00 -05:00 B6 T011 closed：v12 static/composition pass 后，以 `-j4` 对受影响 Core/DI closure 完成 359/359 build（56.675s）；compatibility selector、in-tree/external installed C++ consumers、三个 public example `--help`、ELF/no-Python/nm checks、fresh C++ unary-v6 和 stream-v6 均 `RC=0`，分别发出 numerical/stream oracle pass 与 Provider grant/execution evidence。T011 现标为 `[x]`；B7/T013 继续独立的 failure/cancel/revoke/continuation/replacement qualification。
2026-09-14 01:42 -05:00 B7 T013：v10 官方 `review-agent` 静态门与 `B7_COMPOSITION_PASS` 均通过（无 P0-P3）；普通 `-j4` 增量构建 v1 在 `di-prepared-process.t.cpp` 翻译阶段首错为缺失 `requireAbsentMarker`，尚未链接或运行。原始记录见 `.codex-tmp/spec185-b7/process-build-v1.log`、`.rc`、`.vmstat.log`；已补回 C++ helper，需受影响范围重新静态复审后重建，T013 保持 `PARTIAL`。
2026-09-14 06:52 -05:00 B7 T013：v11 官方 `review-agent` 静态门与 `B7_COMPOSITION_PASS` 均通过（无 P0-P3）；普通系统优先 `-j4` 构建 v2 已成功链接 `spec185-process`，随后在既有 `integration-tests` 目标链接阶段首错为 `OperationRuntime::{notifyWaiters,drain,drainAsync,create,close}` 未定义，返回 `rc=1`，未运行 selector。原始记录见 `.codex-tmp/spec185-b7/process-build-v2.log`、`.rc`、`.vmstat.log`；定位为 `tests/wscript` 的 integration framework source closure 缺少 `ndn-service-framework/OperationRuntime.cpp`，修复后需重新冻结并静态复审接线，T013 保持 `PARTIAL`。

2026-09-14 03:08 -05:00 B5 dynamic boundary：v63 静态门后 normal `spec185-provider-assembly` compile v21 通过；同一候选完整 selector v19/v20 在 authenticated Provider 用例分别以 `rc=134`/`rc=201` 发生内存破坏，gdb v22 首个信号位于主线程 SVS `ndn::Buffer` shared_ptr 路径，borrowed Face worker 同时泵共享 `io_context`。已写入 [b5-provider](evidence/b5-provider.md#b5-normal-runtime-boundaries-v19-v20) 与 `docs/failure-log.md`；T009/T010 保持 `PARTIAL`，先做 sanitizer/最小化 C++ 诊断。

2026-09-13 21:42 -05:00 B5 T010 v59 repair review：v19 的 `finish(*target)` 编译边界已修正为 `finish(target)`；官方 `review-agent` 对快照 `.codex-tmp/spec185-b5-t010-static-v59` 返回 `STATIC_PASS / B5_COMPOSITION_PASS`，五 lane 无控制性缺陷。允许重建；T009/T010 仍为 `PARTIAL`。

2026-09-13 21:35 -05:00 B5 composition v4：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-b5-composition-v4` 返回 `B5_COMPOSITION_PASS`，无 P0/P1/P2/P3；Provider ingress、protected preparation/cache/lease、runner/Response、ServeInvocation 与 stop/drain/reaper 及五 lane 静态闭合。完整生产 assembler 动态组合、compile-link、runtime-test、sanitizer 未新增观察；Batch decision=`STOP_GROWTH / CLOSED_FOR_VALIDATION`，T009/T010 保持 `PARTIAL`，开始批末验证。

2026-09-13 21:27 -05:00 B5 T010 static v58：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-b5-t010-static-v58` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；reaper target 预分配、同步/异步非 Joined 终态、weak owner 及 owned/borrowed Face 生命周期通过五 lane 静态审查。compile-link/runtime/sanitizer 未新增观察；T009/T010 保持 `PARTIAL`，待最终 B5 组合门。

2026-09-14 00:55 -05:00 B5 normal v18/v16/v17：v44 与 composition v3 后 normal 候选 `dbdd7117…` 以 `-j4` compile-link `RC=0`，完整 selector 连续两次 `RC=0`，每次 `Running 8 test cases`、`*** No errors detected`；C-04 normal repeat 已闭合，独立 sanitizer 重建/运行和批次收口待执行，T009/T010 保持 `PARTIAL`。
2026-09-14 00:03 -05:00 B5 composition v3：官方 review-agent `B5_COMPOSITION_PASS`，无控制性缺陷；五 lane 覆盖，Batch growth=`STOP_GROWTH`、static closure=`CLOSED_FOR_VALIDATION`，待修复候选 normal/sanitizer compile-link/runtime，T009/T010 保持 `PARTIAL`。
2026-09-14 00:03 -05:00 B5 T010 v44 repair review：官方 review-agent `STATIC_PASS`，无 P0/P1/P2/P3；ServeInvocation owning payload、self-thread/无线程 reaper release、锁序与幂等均闭合，待修复候选 normal/sanitizer compile-link/runtime，T009/T010 保持 `PARTIAL`。
2026-09-14 00:03 -05:00 B5 T010 v43 static boundary：官方 review-agent 发现异步 Face 回调引用捕获 `definition/nativeConfig/this` 可能悬空（P1），self-thread reaper 未释放 owners（P2），`STATIC_FAIL`；未构建/运行，已改为共享 payload/统一 reaper release 并待 v44 复审，T009/T010 保持 `PARTIAL`。
2026-09-14 00:03 -05:00 B5 T010 v42 static boundary：官方 review-agent 发现 `requestStopIo()` 的 `ioMutex`→`releaseStoppedResources()`→`serveMutex` 与并发 `serve()` 的 `serveMutex`→`ioMutex` 锁顺序反转，`STATIC_FAIL` P1；未构建/运行，已移出临界区并待 v43 复审，T009/T010 保持 `PARTIAL`。
2026-09-14 00:03 -05:00 B5 T010 v41 static boundary：官方 review-agent 发现 `releaseStoppedResources()` 与 `serve/stop/drain` 的 shared_ptr 成员访问未统一同步，Provider copy/reaper 可能产生数据竞争或 UAF，`STATIC_FAIL` P1；未构建/运行，已修复并待 v42 复审，T009/T010 保持 `PARTIAL`。
2026-09-14 00:03 -05:00 B5 sanitizer runtime r1：独立 ASan/UBSan 候选 `fef99152…` 的业务断言到 `*** No errors detected`，但严格 LSan 在 Provider-only teardown 报 27 个间接泄漏/4518 bytes，`RC=134`；T009/T010 保持 `PARTIAL`，已定位 stop 后 Face-bound owner 释放时序并待 v41 静态复审。
2026-09-14 00:03 -05:00 B5 normal runtime v15：与 v14 使用完全相同的候选和 worker 第二次运行，`RC=0`，8/8 cases、106/106 assertions 通过；C-04 normal repeat 已闭合，仍需独立 sanitizer 和批次组合收口，T009/T010 保持 `PARTIAL`。
2026-09-13 19:35 -05:00 B5 normal runtime v14：v17 候选首次修复后 `RC=0`，8/8 cases、106/106 assertions 通过，包含正向 assembly/runner/Response、三类拒绝及 stop→drain；按 C-04 尚需同候选第二次运行和独立 sanitizer，T009/T010 保持 `PARTIAL`。
2026-09-13 19:25 -05:00 B5 normal compile v17：v40 静态复审通过后，以系统优先 PATH、`-j4` 在既有 build tree 仅构建 `spec185-provider-assembly`，`RC=0`，耗时 22.994s，候选 SHA256 `8683a0902628355bb261b6923bd091a9146a80c8cf31d58c29f50b11e779bde8`；compile-link PASS，runtime/sanitizer 未执行，T009/T010 保持 `PARTIAL`。
2026-09-13 19:12 -05:00 B5 T010 v40 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v40` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；fixture 明确 `registration.close()` → `facade.stop()` → `facade.drain()`，生产 close/callback 语义不变，五 lane 全部 covered，尚未重新 compile-link/runtime/sanitizer，T009/T010 保持 `PARTIAL`。
2026-09-13 19:02 -05:00 B5 normal runtime v13：v16 候选 `RC=201`，105/107 assertions 通过；正向计数、Response 和三类身份拒绝均通过，唯一失败是 `facade.drain(2000ms)` 返回 false 后 SIGABRT。T009/T010 保持 `PARTIAL`；下一步修正 stop/drain fixture 出口并复审。
2026-09-13 18:55 -05:00 B5 normal compile v16：v39 静态复审通过后，以系统优先 PATH、`-j4` 在既有 build tree 仅构建 `spec185-provider-assembly`，`RC=0`，耗时 21.664s，候选 SHA256 `a111e193ef01c07793089e86c284e60611f2f236a01d5c96c6b2d699be20ae6e`；compile-link PASS，runtime/sanitizer 未执行，T009/T010 保持 `PARTIAL`。
2026-09-13 18:49 -05:00 B5 T010 v39 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v39` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；test preparation seam 成功后递增 `ProviderMetrics::assemblies`，三个负向 request ID 改为单一 NDN component，v38 seam metadata、assembly identity 和 path validator 约束保持。五 lane 全部 covered，尚未 compile-link/runtime/sanitizer，T009/T010 保持 `PARTIAL`。
2026-09-13 18:44 -05:00 B5 diagnostic runtime v13：TRACE 确认正向请求完整经过 ACK、Selection、Provider execution、Response publication、User 解密和 callback；唯一正向断言缺口是 test preparation seam 未更新 `assemblies` metrics。三个负向 ID `/spec185-provider-reject-/N` 被解析为 service 后缀并在 admission 处拒绝，未形成 Selection；原始证据见 [b5-provider](evidence/b5-provider.md#b5-diagnostic-runtime-boundary-v13)。已定位为 fixture/test seam 边界，T009/T010 保持 `PARTIAL`；下一步修正 seam 计数和单组件负向 ID，静态复审后重建运行。
2026-09-13 20:18 -05:00 B5 normal runtime v12：v15 候选编译通过后完整 selector 返回 `RC=201`，7/8 用例通过；正向 ACK/Selection/Response 已形成但 `assemblies=0`，三个身份替换负向未见 Selection，`facade.drain` 失败并 SIGABRT，T009/T010 保持 `PARTIAL`。下一步以 TRACE 定位 assembly 失败。

2026-09-13 20:02 -05:00 B5 T010 v38 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v38` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；fixture 恢复 runner factory 所需四项 seam metadata，并保留 `model.onnx` 与完整 assembly identity。compile-link/runtime 尚未执行，T009/T010 保持 `PARTIAL`。

2026-09-13 19:42 -05:00 B5 T010 v37 static failure：官方 review-agent 发现 preparationFactory 删除了 fixture runner factory 通过 `metadata.at()` 读取的 `provider/boot/plan/artifact`，首个 `create()` 将抛 `std::out_of_range`；未构建/运行，T009/T010 保持 `PARTIAL`。已安排恢复四个 metadata 并重新静态审查。

2026-09-13 19:18 -05:00 B5 diagnostic runtime v12：TRACE 诊断确认正向 REQUEST/ACK/Selection/Provider execution 已闭合；首个生产失败为 `DI_PROVIDER_ASSEMBLY_PATH_UNSAFE`，fixture 使用 `oracle.onnx` 且 assembly identity metadata 不完整，未发生 assembly/runner/Response，T009/T010 保持 `PARTIAL`。已修正 fixture runner spec，待 v37 静态复审。

2026-09-13 18:49 -05:00 B5 normal runtime v11：v14 候选 `7f22f35a282f8e624f6a7630e992807a5333e812d636928c102110ec8ffd5f09` 编译通过后运行返回 `RC=201`，7/8 用例通过；bootstrap role grant 已安装但正向和负向请求均未形成 Selection，`facade.drain` 失败并 SIGABRT，T009/T010 保持 `PARTIAL`，继续定位 ACK/Selection 入口。

2026-09-13 18:36 -05:00 B5 T010 v36 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v36` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；`providerRoles` 默认空，Spec185 role grant 纳入 bootstrap permission wave，避免 post-bootstrap 权限替换副作用。compile-link/runtime 尚未执行，T009/T010 保持 `PARTIAL`。

2026-09-13 18:26 -05:00 B5 normal runtime v10：v35 候选 `b4af4a1c50d18a5f5ba9f008416d3501b53673293afafc8266e0c226f2219d2f` 编译通过后运行返回 `RC=201`；post-bootstrap `applyPermissionResponse` 的 role grant 替换触发刷新副作用，正向未形成 ACK/Selection/Response，负向未形成 Selection，T009/T010 保持 `PARTIAL`。下一步将 role grant 纳入 bootstrap permission wave，完成后重新静态审查。

2026-09-13 18:18 -05:00 B5 T010 v35 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v35` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；fixture 保留服务级 Provider permission 并增加 `/Inference/Spec185ProviderOracle/ROLE/Backbone` role permission，修复 v9 已定位的生产 admission 授权边界。compile-link/runtime 尚未执行，T009/T010 保持 `PARTIAL`。

2026-09-14 00:02 -05:00 B5 T010 v34 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v34` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；仅补充 mutex 保护下的同步 failure 文本诊断。compile-link/runtime 尚未执行，T009/T010 保持 `PARTIAL`。

2026-09-13 23:46 -05:00 B5 normal runtime v8：v33 候选 selector 返回 `RC=201`，13 项断言失败；同源 key setup 后正向仍无 response/assembly/runner counters，负向无 Selection，末尾 `facade.drain` 失败。已保留原始日志，下一步仅增加同步 probe failure 诊断再定位，不计 T009/T010 完成。

2026-09-13 23:38 -05:00 B5 normal compile v11：v33 静态复审后既有 build tree 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 19.738s，`rc=0`，候选 SHA256 `c280d8104d060959ffbf98c72653c3eab246fe47b326ed1b7d6b35f25bbad0fb`。compile-link PASS；下一步运行完整 C++ selector，T009/T010 仍为 `PARTIAL`。

2026-09-13 23:25 -05:00 B5 T010 v33 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v33` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；Response 已回到生产 `OnResponse` 解密路径，ACK/RESPONSE/SELECTION 测试密钥边界已补齐。compile-link/runtime/sanitizer 尚未执行，T009/T010 保持 `PARTIAL`，现进入动态验证。

2026-09-13 23:17 -05:00 B5 T010 static boundary v32：官方 review-agent 发现 fixture 将加密 SVS `HybridMessageEnvelope` 直接传给 `handleDecryptedResponseByName`，没有经过生产 `OnResponse` 解密入口；该测试 oracle 无效，未构建/运行，T010 保持 `PARTIAL`。下一步修正 response ingress 后重新静态复审。

2026-09-13 22:59 -05:00 B5 normal runtime v7：v31 候选 selector 返回 `RC=201`，13 项断言失败；Response publication 未转发到 User `handleDecryptedResponseByName`，正向请求超时且 counters=0，负向循环未形成终态，最后 `facade.drain` 失败。原始记录见 [b5-provider](evidence/b5-provider.md#b5-normal-runtime-boundary-v7)。下一步修复 fixture ingress/联合等待，必须重新静态复审后再构建；T009/T010 保持 `PARTIAL`。

2026-09-13 22:53 -05:00 B5 normal compile v10：v31 静态复审后既有 build tree 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 26.097s，`rc=0`，候选 SHA256 `4261653e2829172da85a5b1dd1983b8753327ac7a2e9d745d6764e8f0589a878`。compile-link PASS；下一步运行完整 C++ selector，T009/T010 仍为 `PARTIAL`。

2026-09-13 22:49 -05:00 B5 T010 v31 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v31` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；每请求 probe 的同步、不可变请求捕获和 terminal 等待已闭合。v6 是 v31 之前的候选运行，因异步 Boost.Test 断言 SIGSEGV（RC=201）不计通过；现仅允许用 v31 快照重建并重跑 B5，T009/T010 保持 `PARTIAL`。

2026-09-13 22:43 -05:00 B5 T010 static boundary v30：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v30` 返回 `STATIC_FAIL`；fixture 异步 response/failure 状态未统一同步，受控失败循环在 terminal callback 前返回并重置可变请求状态，存在跨线程数据竞争和旧回调污染风险。未构建/运行，T010 保持 `PARTIAL`，修复要求及快照身份见 [b5-provider](evidence/b5-provider.md#t010-static-boundary-v30)。

2026-09-13 12:02 -05:00 B4 sanitizer repeat：ASan/UBSan conversation r24 单次通过全部 20 项，但紧邻重复 r25 在 atomic 并发断言处 `RC=134` stack-smash；未形成连续资格通过。现做受控隔离，将并发负例改为同线程直接捕获，待静态复审后重建和重复验证，T007/T008 保持 `PARTIAL`。
2026-09-13 12:08 -05:00 B4 direct-exception static gate：受控同线程并发负例通过官方 `review-agent` 静态审查；不可变快照 `.codex-tmp/spec185-b4-after-direct-exception-static-v1` 的 base、DIFF SHA 和 PATHS SHA 已记录于 [b4-conversation](evidence/b4-conversation.md)。审查确认 active-turn 拒绝及生产 mutex/completion/close 生命周期不变；真实跨线程竞争仍是明确的未观测证据边界。待共享构建、normal 与严格 ASan/UBSan selector 重复运行，T007/T008 保持 `PARTIAL`。
2026-09-13 12:32 -05:00 B4 direct-exception runtime：normal build v20（既有 `.lock-spec185-b0c-normal`、`-j4`）成功，耗时 `25.611s`；conversation r39/r40 连续通过全部 20 项。ASan/UBSan + LSan build v14 成功，耗时 `40.614s`；严格 conversation r26 为 `RC=134` stack-smash，r27 为 `RC=1` 嵌套 `AddressSanitizer: DEADLYSIGNAL`，均无生产 requester/coordinator 栈；独立 API r19/r20 连续通过。原始日志与元数据见 [b4-conversation](evidence/b4-conversation.md)。sanitizer 会话 lane 仍未形成资格通过，T007/T008 保持 `PARTIAL`，不得将 normal/API PASS 外推为 B4 完成。
2026-09-13 13:26 -05:00 B4 deferred-bridge static gate：为隔离 ndn-svs/fixture 同步重入，新增默认关闭的 `BootstrapProfile::deferBridgeDelivery`，仅 Spec185 conversation profile 启用；repair-only 快照 `.codex-tmp/spec185-b4-after-deferred-bridge-static-v6` 经官方 `review-agent` `STATIC_PASS`，无 P0/P1/P2/P3，身份见 [b4-conversation](evidence/b4-conversation.md)。审查确认 packet 所有权、FIFO/fault 顺序、pump/drain 出口和默认兼容；queued handler 异常及 deferred fault/reorder 仍待运行验证。待 normal/ASan 共享构建和严格 selector 重复，T007/T008 保持 `PARTIAL`。
2026-09-13 13:58 -05:00 B4 deferred-bridge runtime：normal build v21（`.lock-spec185-b0c-normal`、`-j4`）成功，耗时 `31.529s`；conversation r41/r42 连续通过全部 20 项。ASan/UBSan + LSan build v15（`.lock-spec185-b3-asan-ubsan-fast`、`-j4`）成功，耗时 `45.586s`，但严格 conversation r31 仍以 `RC=134` stack-smash 在 active-turn 断言处失败；无生产 requester/coordinator 栈。排队桥接未改变 sanitizer 边界；先做仅跳过该断言的诊断，不计资格 PASS，T007/T008 保持 `PARTIAL`。
2026-09-13 14:20 -05:00 B4 exception-path diagnosis：诊断开关经静态审查后，normal build v22 和 ASan/UBSan build v16 成功；仅设置 `SPEC185_SKIP_CONVERSATION_BUSY_PROBE=1` 的严格 conversation r32 完成全部后续三轮/恢复/export/close/drain，`RC=0` 且无 sanitizer 错误。该开关已移除，真实 `CONVERSATION_TURN_IN_PROGRESS` 断言已恢复；诊断仅定位 sanitizer/dependency 异常展开边界，不计资格 PASS。T007/T008 保持 `PARTIAL`，等待外部栈回溯/fixture 边界修复后再验收。
2026-09-13 13:19 -05:00 B5 T009 static failure：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t009-static-v2` 返回 `STATIC_FAIL`，发现 Provider 初始化/assembly/角色绑定/drain/config consistency 的 P1 及 parser/oracle P2；未构建未运行，T009 保持 `PARTIAL`，失败边界与修复要求见 [b5-provider](evidence/b5-provider.md)。

2026-09-13 13:31 -05:00 B5 T009 repair review failure：官方 review-agent 对冻结修复快照 `.codex-tmp/spec185-b5-t009-static-v3` 复审返回 `STATIC_FAIL`；drain timeout/join、Runtime drain false、Controller permission/bootstrap、manifest binding、serve rollback 与 C++ oracle 仍有缺口，未构建未运行，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 13:45 -05:00 B5 T009 repair review failure v4：官方 review-agent 对冻结修复快照 `.codex-tmp/spec185-b5-t009-static-v4` 复审返回 `STATIC_FAIL`；protected factory 接线、Provider drain/async、IO join 竞态、controller certificate 测试边界及 C++ authenticated oracle 仍有缺口。随后已修复前四项中的生产接线/生命周期实现，未构建未运行，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 13:57 -05:00 B5 T009 repair review failure v5：官方 review-agent 对冻结修复快照 `.codex-tmp/spec185-b5-t009-static-v5` 复审返回 `STATIC_FAIL`；Provider serve 的 Face 线程亲和性、Runtime.close non-blocking、authenticated Selection/assembly/counter oracle 及 drainAsync Subscription 保持缺口，未构建未运行，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 14:18 -05:00 B5 T009 repair review failure v6：官方 review-agent 对冻结修复快照 `.codex-tmp/spec185-b5-t009-static-v6` 复审返回 `STATIC_FAIL`；ProviderCounters API 错位、Face dispatch/stop 永久等待、registration cleanup barrier、start/stop admission race 及 authenticated 正向 oracle 仍有缺口，未构建未运行，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 14:30 -05:00 B5 T009 repair review failure v7：官方 review-agent 对冻结修复快照 `.codex-tmp/spec185-b5-t009-static-v7` 复审返回 `STATIC_FAIL`；`serveMutex` 跨 Face 等待、serve/start/stop 稳定错误映射及 authenticated Selection/assembly/runner/Response 正向与拒绝 oracle 仍有缺口，未构建未运行，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 19:55 -05:00 B5 T009 repair review failure v9：官方 review-agent 对冻结修复快照 `.codex-tmp/spec185-b5-t009-static-v9` 返回 `STATIC_FAIL`；ProtectedRuntime factory lambda 捕获、ProviderConfig pImpl 命名空间/访问控制、Provider ingress 负向身份 oracle 及 runner/protected factory 初始化异常映射仍有缺口，未构建未运行，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 20:05 -05:00 B5 T009 repair review failure v10：官方 review-agent 对冻结修复快照 `.codex-tmp/spec185-b5-t009-static-v10` 返回 `STATIC_FAIL`；Provider-only Runtime drain 状态和真实 NativeCanonicalOnnxAssembler/source-fetch 正向 oracle 仍有缺口，未构建未运行，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 20:20 -05:00 B5 T009 repair review pass v11：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t009-static-v11` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；Runtime provider-only drain 失败状态及生产 NativeCanonicalOnnxAssembler/source-fetch/OA02 正向 oracle 已覆盖。尚未构建/运行，等待 B5 组合审查，T009 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 20:40 -05:00 B5 T010 static failure v12：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v12` 返回 `STATIC_FAIL`；single-flight creator 取消误伤其他 waiter，cache 保存 request/grant projection 与 plaintext runner path，source identity/异常安全/生产 cold-hit oracle及内部 header exposure仍有缺口。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 16:10 -05:00 B5 T010 static failure v17：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v17` 返回 `STATIC_FAIL`；已取消 job 可被重新加入，admission 提前阻断可驱逐 LRU，命中未重验 assembled 上限，cache API 未校验 grant identity，另有 runner 字节溢出和 stop 异常边界。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 16:18 -05:00 B5 T010 static failure v18/v19：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v18`、`.codex-tmp/spec185-b5-t010-static-v19` 依次返回 `STATIC_FAIL`；v18 修复范围仍有 admission 异常原子性、cancelled flight generation barrier 和 stop 错误优先级缺口，v19 另确认 protected fixture 已纳入但上述三个 P1 仍未闭合。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 16:22 -05:00 B5 T010 static failure v20：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v20` 返回 `STATIC_FAIL`；cancelled generation barrier、stop 错误优先级、grant/epoch 和 protected cache oracle 已闭合，剩余唯一 P1 是 admission 部分驱逐后的异常回滚。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 16:24 -05:00 B5 T010 static failure v21：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v21` 返回 `STATIC_FAIL`；victim 两阶段异常原子性已闭合，剩余唯一 P1 是预选循环用总 chargedBytes 而非实际 required 回收量，混合 pinned/unpinned 场景会错误拒绝。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 16:26 -05:00 B5 T010 static pass v22：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v22` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；protected cache、取消代际、grant/epoch、预算和异常原子性静态闭合。mixed pinned/unpinned 反例、Provider 默认 protected 端到端及 compile-link/runtime/sanitizer 仍未观察，T010 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 16:37 -05:00 B5 composition pass：官方 review-agent 对 `.codex-tmp/spec185-b5-composition-v1` 返回 `B5_COMPOSITION_PASS`，无 P0/P1/P2/P3；`STOP_GROWTH`、`CLOSED_FOR_VALIDATION`，T009/T010 可进入共享 compile-link/runtime/sanitizer 验证，当前仍未将静态结果计为完成，详见 [b5-provider](evidence/b5-provider.md)。
2026-09-13 15:55 -05:00 B5 T010 static failure v14：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v14` 返回 `STATIC_FAIL`；受保护 Provider 路径绕过 artifact cache，creator 唯一 waiter 取消不触发 last-waiter cancellation，publish 异常事务和 assembly 暂存/template 预算仍不闭合。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](evidence/b5-provider.md)。

2026-09-13 17:19 -05:00 B5 normal runtime boundary v3：完整 `Spec185ProviderAssembly` selector 使用候选 `1792d5d2d212e0dbd43347cffe240546c7694584e395db1f199c5457676bf62a` 返回 `rc=201`，5/8 用例通过、3/8 失败；authenticated ACK candidate 集合为空，两个真实 assembler oracle 以 `DI_NATIVE_ONNX_RECIPE` 拒绝 recipe digest。原始日志见 `.codex-tmp/spec185-b5/normal-run-v3/output.log`，失败边界和后续静态复审要求见 [b5-provider](evidence/b5-provider.md#b5-normal-runtime-boundary-v3)。T009/T010 继续 `PARTIAL`。

2026-09-13 17:22 -05:00 B5 T010 v28 静态复审通过：官方 review-agent 核对冻结快照 `.codex-tmp/spec185-b5-t010-static-v28` 返回 `T010_STATIC_PASS`，无 P0-P3；fixture 完成信号只来自 RequestService response callback，五 lane 无回退。随后 worker target `di-native-assembly-worker` 在既有 build tree 以 `-j4` 编译链接通过，但完整 selector v4 仍为 `rc=201`（7/8），authenticated fixture counters 与异步拒绝循环仍未形成资格通过。详见 [b5-provider](evidence/b5-provider.md#t010-repair-review-pass-v28)。

2026-09-13 17:28 -05:00 B5 T010 v29 静态复审通过：官方 review-agent 核对冻结快照 `.codex-tmp/spec185-b5-t010-static-v29` 返回 `T010_STATIC_PASS`，无 P0-P3；fixture 改用生产 `canonicalOnnxSourceIdentity` 派生真实 graph/initializer digest。下一步只重建受影响 C++ selector 并重跑 B5；T009/T010 仍为 `PARTIAL`。

2026-09-13 13:01 -05:00 B4 closed：最终组合快照 `.codex-tmp/spec185-b4-composition-v3` 经官方 `review-agent` 报告 `B4_COMPOSITION_PASS / STATIC_PASS`，无P0-P3；身份、五lane和未观测边界见 [b4-conversation](evidence/b4-conversation.md#final-b4-composition-and-qualification)。组合门后 normal build v26 与 ASan/UBSan+LSan build v19 均 `BUILD_RC=0`；normal conversation r49/r50、API r51/r52，以及严格 sanitizer conversation r40/r41、API r42/r43 全部 `RC=0`、无Boost.Test或sanitizer错误。T007/T008 完整验收通过并改为 `[x]`；仍未观测项不扩展为跨线程调度资格，Python、Provider及后续B5-B9尚未开始。

2026-09-13 11:54 -05:00 B4 sanitizer boundary：atomic 并发异常探针经静态 PASS 后，normal build v19、conversation r37/r38、API r37/r38 通过；ASan/UBSan build v13 成功，但 conversation r23 仍在第二请求原子断言附近 `RC=134` stack-smash。移除 promise/future 未改变边界，未见生产 requester/coordinator 诊断；T007/T008 保持 `PARTIAL`，需继续缩小 fixture 并发探针。

2026-09-13 11:46 -05:00 B4 compile boundary：atomic repair 后 normal build v18 在编译前因 `build-spec185-b0c-normal` 未配置返回 `BUILD_RC=1`；原始记录为 `.codex-tmp/spec185-b4/normal-build-v18.log`，不计源码编译或运行结果。需用 verified system toolchain 重新配置后再执行受影响目标构建，T007/T008 继续 `PARTIAL`。

2026-09-13 11:46 -05:00 B4 repair static PASS：atomic 并发异常探针快照 `.codex-tmp/spec185-b4-after-atomic-exception-static-v1` 经官方 `review-agent` 通过，无 P0/P1/P2/P3；生产 active-turn/close/cancel 与 C++ fixture lifetime 未改变。compile-link/runtime-test 尚未重跑，T007/T008 继续 `PARTIAL`，证据见 [b4-conversation](evidence/b4-conversation.md)。

2026-09-13 07:50 -05:00 B3 closed：T005→T006 完成逐任务官方 `review-agent` 静态门、受影响复审及最终组合门；最终组合快照 v6 的 base/diff/path 身份见 [b3-request](evidence/b3-request.md)。normal `Spec185PreparedRequest` 12/12 与 `Spec185ExtensionRegistry` 10/10 各顺序重复两次；独立 ASan/UBSan + LSan 两个 selector 同样各重复两次，均 `rc=0`、`*** No errors detected` 且无 LSan/UBSan 报告。批次唯一证据为 [b3-request](evidence/b3-request.md)；保留并发资源干扰及早期 LSan/deadline 失败边界。B3 未运行 TSan、B4–B9、Python、跨进程资格、SIF/Tiger/MiniNDN；下一依赖满足批次为 T007/B4，Spec185 整体仍为 `PLANNED`。

2026-09-13 09:08 -05:00 B4 partial：T007/T008 完成逐任务静态门及 B4 组合门；v4/composition 快照身份与官方 review-agent 结果见 [b4-conversation](evidence/b4-conversation.md)。共享 normal `-j4` 构建在 `di-prepared-request.t.cpp` 测试翻译阶段首错停止（`StreamFinishReason` 命名空间及 vector 断言打印器），生产会话源已编译；失败边界见 `docs/failure-log.md`。已修复测试并等待受影响静态复审，未运行任何 B4 selector，任务保持 `PARTIAL`。

2026-09-13 09:13 -05:00 B4 compile retry：测试翻译修复经受影响静态复审通过；第二次 `-j4` 构建在 `Conversation.cpp` 首个生产翻译阶段发现 `DiError` 定义缺失，记录于 [b4-conversation](evidence/b4-conversation.md) 与 `docs/failure-log.md`。已加入 `Runtime.hpp`，等待生产受影响范围复审；B4 selector 尚未运行，T007/T008 保持 `PARTIAL`。
2026-09-13 09:23 -05:00 B4 runtime boundary：normal build v3 成功后，首个 selector 命令因 Boost.Test filter 写法返回 `200` 无匹配；修正 selector 进入真实 native planning，但以 `NATIVE_REQUEST_STAGE_FAILED: generation role omits a sealed state input` 返回 `201`。根因是会话 fixture 将 YOLO source graph 与 Qwen streaming state defaults 混用，生产规划正确拒绝；失败日志为 `.codex-tmp/spec185-b4/normal-runs/conversation-r1.log`、`conversation-r2.log`，详见 [b4-conversation](evidence/b4-conversation.md)。现将真实会话改用已有 source-bound Qwen native-config fixture，普通 streaming probes 保持 YOLO；需受影响静态复审和重新运行，T007/T008 仍为 `PARTIAL`。
2026-09-13 09:31 -05:00 B4 grant boundary：Qwen fixture 经静态修复并正常构建后，精确 conversation selector 在授权阶段返回 `201`：`DI_PROTECTED_GRANT_REJECTED: published manifest differs from authorized source`（`.codex-tmp/spec185-b4/normal-runs/conversation-r3.log`）。根因是授权 publication source 仍使用 `fixture-profile`，而 Qwen catalog 使用另一 profile；生产校验正确拒绝未绑定制品。现将 Qwen catalog profile 对齐显式授权值，需受影响静态复审和重建，T007/T008 保持 `PARTIAL`。
2026-09-13 09:39 -05:00 B4 payload boundary：grant profile 修复后 selector 继续到 native request preparation，但以 `201` 返回 `native task payload exceeds the adapter byte bound`（`.codex-tmp/spec185-b4/normal-runs/conversation-r4.log`）。Qwen generation envelope 超出复用的 YOLO `max_payload_bytes=32`；生产输入校验正确拒绝。现将 Qwen catalog 上限调整为维护配置的 4096，YOLO probe 保持 32，需受影响静态复审、重建和 selector，T007/T008 保持 `PARTIAL`。
2026-09-13 09:46 -05:00 B4 append boundary：Qwen source/grant/payload 修复后首轮真实请求已 commit，checkpoint export/import 断言通过；第二轮在 `NativeConversationCoordinator::beginTurn` 返回 `201`：`conversation append prefix mismatch`（`.codex-tmp/spec185-b4/normal-runs/conversation-r5.log`）。`Conversation::makeContinuation` 未把新输入绑定为更长 canonical token prefix，而 coordinator 正确要求 append 严格增长。需补 native tokenizer/input-prefix 绑定并受影响复审，T007/T008 保持 `PARTIAL`。
2026-09-13 10:05 -05:00 B4 prefix provenance review：首版 `RequestOptions.canonicalTokenIds` 经官方 review-agent 发现 P1（`.codex-tmp/spec185-b4-after-prefix-static-v1`）：调用者可伪造 parent-extending token vector。现已移除公开 token lineage 字段，改由 verified native adapter 根据 `Input` 生成 suffix；无 pinned encoder 时 fail closed，Qwen fixture 声明 operator-owned `OPAQUE_BYTE_TOKEN_IDS`。待新快照静态复审，未构建/未运行，T007/T008 保持 `PARTIAL`。
2026-09-13 10:16 -05:00 B4 encoder move review：`.codex-tmp/spec185-b4-after-token-provenance-static-v2` 发现 `NativeCanonicalPreparationCatalog` 首个 entry 的 encoder 被 move 后又按 moved-from 状态比较，Qwen prepare 会错误失败。现已在 move 前保存 presence bit；待受影响复审，未构建/未运行，T007/T008 保持 `PARTIAL`。
2026-09-13 10:28 -05:00 B4 prefix provenance static PASS：快照 `.codex-tmp/spec185-b4-after-token-provenance-static-v3`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，DIFF SHA `4b4e230a2c2d5c399fe2a567c39bb1641bd36e85dc0f1933f0e9ee7c1683c7cf`）通过官方 review-agent，无 P0/P1/P2/P3；五 lane 覆盖，未构建/未运行。进入 B4 组合门，T007/T008 保持 `PARTIAL`。
2026-09-13 10:44 -05:00 B4 composition static PASS：快照 `.codex-tmp/spec185-b4-composition-v2`（base `775d0687d97eeb6c9f053d8e78c8c01b31864f91`，DIFF SHA `32616260ed5a19855c994d023c03df695b37dbfae31462c85f8a62cec54300dd`）通过官方 review-agent，无 P0/P1/P2/P3；完整调用链和五 lane 覆盖，未构建/未运行。进入 B4 批末共享构建，T007/T008 保持 `PARTIAL`。

2026-09-13 15:14 -05:00 B4 compile-link PASS：组合静态门后对 `spec185-prepared-conversation` 进行一次共享 normal `-j4` 增量构建，`BUILD_RC=0`，耗时 `1m26.556s`，108 个受影响 DI 源文件完成编译链接；记录见 [b4-conversation](evidence/b4-conversation.md#batch-build-compile-link)。仅有既有编译警告，尚未运行 normal/sanitizer selector，T007/T008 继续保持 `PARTIAL`。
2026-09-13 15:16 -05:00 B4 runtime boundary：normal `Spec185PreparedRequest/PreparedConversationCommitsTwoNativeTurns` 首次真实运行（`conversation-r6.log`）进入请求和 checkpoint 路径，但在 `last checkpoint` 断言以 `final payload disagrees with accepted generation` 返回 `RC=201`；失败边界已记入 [b4-conversation](evidence/b4-conversation.md#runtime-boundary-after-compile-link)，不计资格 PASS。暂停重跑，先核对 accepted generation 与最终 payload 的生产绑定。
2026-09-13 15:39 -05:00 B4 fixture repair static PASS：恢复 turn 的 final payload 改为复用生成事件的 `nextToken`；受影响不可变快照 `.codex-tmp/spec185-b4-after-fixture-token-static-v1` 经官方 review-agent `STATIC_PASS`，无 P0/P1/P2/P3，五 lane 覆盖，身份见 [b4-conversation](evidence/b4-conversation.md#runtime-boundary-after-compile-link)。允许重新 compile-link，T007/T008 仍保持 `PARTIAL`。
2026-09-13 15:52 -05:00 B4 runtime repeat boundary：修复后 normal selector `conversation-r9.log` 首次通过（`RC=0`，三次 committed turn、恢复、导出断言通过）；重复 selector `conversation-r10.log` 在恢复第三 turn 的 8 秒 `result()` 观察窗口以 `native result wait timed out` 返回 `RC=201`，未出现新的协议错误。该失败已记入 [b4-conversation](evidence/b4-conversation.md#runtime-boundary-after-compile-link)，不计资格 PASS；先扩大有界 fixture 请求/观察预算并复审，再重复运行。
2026-09-13 16:00 -05:00 B4 sanitizer boundary：ASan/UBSan build v5 成功，但 conversation selector r1/r2 在首轮结果等待以 `native result wait timed out` 返回（无 ASan/UBSan/LSan 报告）。该失败记录于 [b4-conversation](evidence/b4-conversation.md#runtime-boundary-after-compile-link)，不计资格 PASS；已将 sanitizer fixture 的有界请求/观察预算分别扩大到 60s/45s，等待受影响静态复审后重建和复测。
2026-09-13 16:22 -05:00 B4 normal repeat boundary：复审后的 normal build v10 成功；conversation r15 通过但重复 r16 在首轮结果等待以 `native result wait timed out` 返回（RC=201，五项早期断言通过），API r17/r18 通过。该失败与 r10/ASan r1-r2 属同一主机调度/观察预算边界，不计资格 PASS；已将 normal 与 sanitizer 会话 fixture 统一为 60s/45s，等待静态复审后重建和重复验证。
2026-09-13 16:38 -05:00 B4 uniform-budget boundary：统一 60s/45s 修复经静态复审后 normal build v11 成功，API r21/r22 通过；conversation r19 在首轮结果等待 45s 超时而 r20 通过。该偶发边界未观察到协议/内存错误；已保留原始记录并把 normal/sanitizer 会话预算统一提高到 180s/120s，需再次静态复审、重建和连续 selector 验证。
2026-09-13 17:08 -05:00 B4 extended-budget runtime：扩展预算经静态复审；normal build v12、conversation r23/r24、API r25/r26 全部通过；ASan/UBSan build v7 和 API r7/r8 通过，但 conversation r7/r8 分别在 120s/180s 首轮结果等待返回 `RC=201 native result wait timed out`，无 sanitizer 诊断。T007/T008 保持 `PARTIAL`，需先定位 sanitizer liveness/调度边界，不能把 normal PASS 外推为完整 B4 PASS。
2026-09-13 17:45 -05:00 B4 fixture liveness diagnosis：静态检查确认 `NdnsfIntegrationEnvironment::pumpUntil()` 每次只泵约4秒，测试随后直接 `future.get()`，ASan 慢路径会在未继续处理 DummyFace 事件时超时。拟在 C++ fixture 增加按 request deadline 分块重复 pump 的 helper；未改生产请求/coordinator，待受影响静态复审后重建与重跑。
2026-09-13 18:02 -05:00 B4 compile boundary：pump helper 首次 normal v13 编译在测试翻译阶段因未限定 `ndn_service_framework::test::NdnsfIntegrationEnvironment` 停止（`BUILD_RC=1`）；已记录于 [b4-conversation](evidence/b4-conversation.md)，不涉及生产代码，修正后需再次静态复审。
2026-09-13 11:14 -05:00 B4 sanitizer fixture crash：pump helper 修正经 `.codex-tmp/spec185-b4-after-pump-helper-static-v3` 官方 review-agent `STATIC_PASS` 后，normal build v14、conversation r27/r28、API r29/r30 通过；sanitizer build v9 与 API r9/r10 通过，但 conversation r9/r10 在 fixture `pumpUntil` 的 sanitizer 栈清理写触发 `DEADLYSIGNAL`，gdb 栈未进入生产 requester/coordinator。该内存失败已保留原始日志，T007/T008 继续 `PARTIAL`，下一步修复 fixture pump 复入/清理边界并重新静态审查、构建、运行。
2026-09-13 11:27 -05:00 B4 sanitizer exception boundary：第二请求异常捕获修复经 `.codex-tmp/spec185-b4-after-concurrent-exception-static-v1` 官方 review-agent `STATIC_PASS` 后，normal build v15/v16、conversation r31/r32、API r33/r34 全部通过；sanitizer build v10/v11 与 API r13-r18 通过，但 conversation r11/r12/r15/r17-r20 间歇性在预期 `CONVERSATION_TURN_IN_PROGRESS` 异常点出现 stack-smash 或嵌套 `AddressSanitizer: DEADLYSIGNAL`，r16 单次通过。原始日志没有生产 requester/coordinator 栈或 UBSan 诊断；放宽 sanitizer 的 no-abort 运行不能作为资格证据。T007/T008 保持 `PARTIAL`，下一步隔离 C++ fixture 异常展开边界，再受影响复审、共享重建和严格 sanitizer 重复验收。
2026-09-13 11:27 -05:00 当前执行边界：B4 尚未闭合，未开始 B5 Provider；下一步只处理 sanitizer 会话选择器在第二请求异常展开处的可重复边界。正常构建/运行及 API 反例已形成证据，但不能替代严格 ASan/UBSan 会话验收；Spec185 仍为 `PLANNED`。

2026-09-12 23:03 -05:00 Build policy documentation：按用户要求将所有机器后续SIF构建版本固定为Apptainer1.5.3；同步Tiger/packaging说明、交付入口、版本化/个人操作skill及本机AGENTS。文档diff/版本规则一致性检查通过；无API/产品变化、无新构建或Tiger运行，任务状态/依赖不变。下一步实验机按[SIF规则](../../Experiments/TigerCluster/docs/sif-build.md#apptainer-version-policy)核对环境。
2026-09-12 22:59 -05:00 Host tooling：用户要求本机Apptainer升级1.5.3，安装、默认/兼容/root入口和最小SIF构建执行通过；见[运维记录](../../Experiments/TigerCluster/docs/apptainer-153-upgrade-20260912.md)。不改变185任务状态、依赖或验收；计算节点版本由实验机在实际作业核验，此记录不是185产品完成证据。

2026-09-12 21:07 -05:00 B2 closed：T003→T004 完成逐任务官方 review-agent 静态门、B2组合门及批末共享验收。普通 `Spec185Preparation` 14/14、`Spec185Runtime` 10/10、`Spec185CoreOperation` 35/35 通过；独立 clang/TSan 三套件各重复两次共6次均 `rc=0` 且 `*** No errors detected`。证据见[b2-preparation](evidence/b2-preparation.md)，失败边界及修复见 `docs/failure-log.md`。准备链未运行 Python、Provider、会话、跨进程资格、SIF/Tiger；下一依赖满足任务为T005/B3。

2026-09-12 17:00 -05:00 Dependency-scoped gate revision：门禁仅阻塞依赖工作；无依赖、文件边界清晰且前置满足的任务可由主代理在子代理只读审查固定快照期间继续。执行细则见[批次执行表](batch-execution.md)，验证见[调度修订](evidence/dependency-scoped-dispatch-20260912.md)。现有任务状态、Depends、Updated和验收不变；本轮没有启动并行产品实现。下一步执行者先登记独立任务/子任务边界，无合格工作则等待，不绕过T003→T004等硬依赖。

2026-09-12 16:24 -05:00 Progress timestamp revision：18行Updated升级为分钟及UTC offset；同步Spec Kit规则、模板和本机安装入口。状态/勾选/验收证据不变，历史只有日期的checkpoint原样保留；本次时间不是历史完成时间。见[时间规则验证](evidence/progress-timestamps-20260912.md)。下一产品任务仍按当前registry及依赖选择。

2026-09-12 B2E closed：T016 已完成 v9/v10b/v11/v12 官方 review-agent 静态门及 B2E 组合审查；normal `Spec185ExtensionRegistry` 10/10、clang/TSan 重复两次各10/10、installed-prefix C++ extension consumer 通过。失败的 fixture 身份与系统工具链边界均已保留并修复，详见[b2e-extensions](evidence/b2e-extensions.md)；完整 Waf packaging、准备/请求/会话/Provider/完整资格/Python/文档仍未完成。下一依赖满足任务为T003/B2。
2026-09-12 B1 closed：T001/T002 已完成官方 review-agent 静态门（T001 v7、T002 v4）及 B1 组合审查；正常 DI 7/7、Core 34/34，DI/Core TSan 各按要求重复通过，安装前缀 C++ Runtime consumer 通过。证据见[b1-runtime](evidence/b1-runtime.md)。全树安装曾在无关 spec181 链接和 Python editable hook 边界停止，未计入B1产品失败；准备/请求/会话/Provider/完整资格/Python/文档仍未完成。下一依赖满足任务为T016/B2E。
2026-09-12 B0C closed：T017/T018 已完成 v29 官方 review-agent 静态门及组合审查；正常 Core selector 33、DI selector 5、Spec170 回归59、TSan Core重复2次、Core-only staged installed consumer均通过。证据见[b0c-core-operation](evidence/b0c-core-operation.md)；未观测项为全树安装、Python绑定、Runtime及后续准备/请求/会话/Provider/资格批次。下一依赖满足任务为T001/B1。
2026-09-12 B0/T015 closed：安装 API/ABI 证据[b0-installed-api](evidence/b0-installed-api.md)记录 v17 STATIC_PASS、四配置 C++ consumer、67 独立头和 DI/SVS ldd/hash；下一依赖满足任务为T017/B0C。其余17任务保持NOT_STARTED；已补[批次执行表](batch-execution.md)。
每任务编码后review-agent静态门→同批继续→整批组合审查→共享构建/定向测试；不逐小修改编译，也不拖到全Spec末尾首次测试。
本轮为执行计划整理，未修改API/产品设计；验证见[evidence](evidence/batch-execution-20260912.md)。下一步T015/B0。

2026-09-12 Core/App修订：源码确认四消息/协作/流/scoped registration已有Core实现，但DI仍自持通用executor和等待状态。
新增[C-09](contracts/core-app-boundary.md)与T017/T018，18任务12批，全部NOT_STARTED；当前顺序B0→B0C→B1→B2E→B2及其余原序。
本轮设计覆盖此前“新公开类都在DI”的表述；DI保留领域包装，通用实现下移Core。证据见[boundary audit](evidence/core-boundary-20260912.md)。
以下16任务/11批记录为上一文档checkpoint历史，不能作为当前执行队列；当前执行队列为18任务/12批。

2026-09-12：核对现有skill确有Class/Function/Field契约，但185原任务缺内部实现绑定；已补[C-08](contracts/code-design.md)，逐任务Design binding覆盖类/文件delta、字段、关键函数、流程和PO。
新增shared skill开工前设计检查，更新plan/tasks模板及本机入口/个人安装副本。历史记录为16任务11批；当前为18任务12批，全部NOT_STARTED。
T016提供合作splitter，前移到B1后/T003前；实际顺序T015→T001/T002→T016→T003–T011→T013→T012→T014。
本轮[证据](evidence/implementation-design-20260912.md)；设计/技能验证不计产品完成，生产源码及native构建测试未运行。
下一实现单元T015；保持Spec184资格和既有Design 54文件漂移边界。

2026-09-13 16:37 -05:00 B5 首次共享构建：组合门后使用 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、`-j4` 仅构建 `spec185-provider-assembly`，在 C++ fixture 两处临时 `ndn::Buffer` 绑定 `RequestMessage::setPayload(ndn::Buffer&, size_t)` 处返回 `rc=1`；生产 Provider/cache 尚未进入链接。原始日志见 [b5-provider](evidence/b5-provider.md#b5-normal-compile-boundary-v1)，已修复具名可变 Buffer，T009/T010 保持 `PARTIAL`，待 v23 静态复审后重建。
2026-09-13 16:45 -05:00 B5 T010 v23 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v23` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；两处具名可变 Buffer 修复符合 `RequestMessage::setPayload(ndn::Buffer&, size_t)` 契约，五 lane 无控制性缺陷。compile-link/runtime-test/sanitizer 仍未观测，T009/T010 保持 `PARTIAL`，重试 B5 共享构建。
2026-09-13 16:47 -05:00 B5 第二次共享构建：v23 复审后仍使用 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、`-j4` 仅构建 `spec185-provider-assembly`，生产源码编译返回 `rc=1`；`ProviderArtifactCache.cpp:166` 的匿名 lease helper 越过 private constructor，`Provider.cpp:1203` 缺少 `NativeRunnerPreparation.hpp` 声明。原始日志见 [b5-provider](evidence/b5-provider.md#b5-normal-compile-boundary-v2)，已将 helper 移入 cache 成员并补 include，T009/T010 保持 `PARTIAL`，待新静态复审。
2026-09-13 16:49 -05:00 B5 T010 v24 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v24` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；lease helper 权限、runner preparation include 与 v23 fixture 修复均闭合，五 lane 无新增控制性缺陷。compile-link/runtime-test/sanitizer 仍未观测，T009/T010 保持 `PARTIAL`，重试 B5 共享构建。
2026-09-13 16:51 -05:00 B5 normal compile：v24 复审后既有 `build-spec185-b0c-normal` 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时 `59.408s`，候选 SHA256 `c70c7855a8d506c6376b9b03ac3eeb78a9070935566aedeb77fefb79bf512c79`；日志见 [b5-provider](evidence/b5-provider.md#b5-normal-compile-pass-v3)。compile-link PASS，runtime-test/sanitizer 尚未执行，T009/T010 保持 `PARTIAL`。
2026-09-13 16:52 -05:00 B5 selector harness：首次使用 `--list_content=tests` 返回 Boost.Test 参数错误 `rc=200`，无测试执行；原始日志见 [b5-provider](evidence/b5-provider.md#b5-selector-invocation-boundary-v1)。随后改用正确枚举命令，T009/T010 仍保持 `PARTIAL`。
2026-09-13 16:53 -05:00 B5 normal runtime v1：候选 `spec185-provider-assembly` 完整 selector 返回 `rc=201`，3/8 用例通过、5/8 失败；authenticated projection 缺完整 execution/dataflow/device binding，assembler oracle 找不到同树 worker，cache pinned-entry 反例多启动一次 build（3 而非 2）。原始日志见 [b5-provider](evidence/b5-provider.md#b5-normal-runtime-boundary-v1)，T009/T010 保持 `PARTIAL`，已定位并修复上述边界后复审。
2026-09-13 16:59 -05:00 B5 T010 v25 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v25` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；projection binding、worker 路径和 entry-admission 修复均闭合，五 lane 无新增控制性缺陷。compile-link/runtime-test/sanitizer 仍未观测，T009/T010 保持 `PARTIAL`，重试 B5 构建。
2026-09-13 17:10 -05:00 B5 T010 v26 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v26` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；仅删除未使用局部变量，makeLease/锁/发布/reservation/异常/lease 生命周期无回退。runtime-test/sanitizer/压力并发仍未观测，T009/T010 保持 `PARTIAL`，重建后重跑 selector。
2026-09-13 17:12 -05:00 B5 normal compile v5：v26 复审后 `spec185-provider-assembly` 以既有 `-j4` 成功，耗时 `9.594s`，候选 SHA256 `00b7cc48c9fa9830ab6755cf35cc4c23c32942f3ea8350a486488622444f36b9`，无新增 warning。compile-link PASS，runtime-test/sanitizer 待执行，T009/T010 保持 `PARTIAL`。
2026-09-13 17:12 -05:00 B5 normal runtime v2：候选完整 selector 返回 `rc=201`，4/8 通过、4/8 失败（55/59 assertions）；cache pin/eviction、protected cache 已通过，剩余为 `onnxruntime` device binding 与 V3 校验不一致、OA02 fixture 伪 recipe digest 被正确拒绝。原始日志见 [b5-provider](evidence/b5-provider.md#b5-normal-runtime-boundary-v2)，已修复并待静态复审，T009/T010 保持 `PARTIAL`。
2026-09-13 17:14 -05:00 B5 T010 v27 repair review：官方 review-agent 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v27` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；assembler/provider projection 的 adapterVersion、canonical recipe digest、SINGLE_DEVICE/cpu binding 修复闭合，五 lane 无回退。runtime-test/sanitizer/压力并发仍未观测，T009/T010 保持 `PARTIAL`，重建重跑。
2026-09-13 17:18 -05:00 B5 normal compile v6：v27 复审后 `spec185-provider-assembly` 以既有 `-j4` 成功，耗时 `21.696s`，候选 SHA256 `1792d5d2d212e0dbd43347cffe240546c7694584e395db1f199c5457676bf62a`，无新增 warning。compile-link PASS，runtime-test/sanitizer 待执行，T009/T010 保持 `PARTIAL`。
2026-09-13 17:08 -05:00 B5 normal compile v4：v25 复审后 `spec185-provider-assembly` 以 `-j4` 构建成功，耗时 `18.793s`，候选 SHA256 `91c92e422b4870c039bcc0416114c0d5a8847875a6d39ee776d127ec750758d6`；仅有 `ProviderArtifactCache.cpp:537` 未使用变量 warning，compile-link PASS、runtime-test/sanitizer 待执行。清理该 warning 后仍须静态复审，T009/T010 保持 `PARTIAL`。

## Shared Task Rules

调度使用[Dependency-Scoped Dispatch](../../skills/speckit-code-design/references/pre-test-static-review.md#dependency-scoped-dispatch)：依赖工作等待静态通过，无依赖且文件边界清晰的就绪工作可继续；固定快照审查、失败依赖闭包和批末组合门均保留。

路径缩写 `Runtime.cpp` 等未带前缀的DI文件均位于
`NDNSF-DistributedInference/cpp/ndnsf-di/`；tests/examples/pythonWrapper/Design路径从repo root解析。
原生行为任务先编写相应C++反例与fixture，再实现完整行为并用官方review-agent只读审查全diff及五lane。
同批任务静态通过、测试尚未运行时状态PARTIAL，不能勾选。B0C、B1、B2、B3、B4、B5在全部成员静态门和组合门通过后共享构建/测试；
B0、B2E、B6–B9是单任务批次，各自达到出口即验证；B7R是B7后的审计修复验证门。完整批次/五lane/构建复测范围见[执行表](batch-execution.md)。T012只写binding断言，T014只做文档交付。
跨批验收依赖必须实际通过；T012还要求T013完整C++ qualification出口，不接受仅静态接线。
成员证据采用C-04同一批记录模板，不创建第二份进度权威。
C-07每行是T015 exposure、T011 C++消费、T012绑定和T014文档的共同检查项；没有实现/证据不能关闭对应任务。
所有新生产公共方法补C-01英文Doxygen义务；涉及private owner/提交点补说明原因的英文注释。
目标路径须在task开始用CodeGraph/rg定位，若历史路径变动先更新本卡与symbol map，不能在未知文件下另建重复实现。

## Implementation Units

<a id="t015"></a>

- [x] T015 [US4] Installed C++ API and ABI Closure — root wscript; libndn-service-framework.pc.in; NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in; NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp; tests/installed-api/

  **Batch / Depends**: B0 / none。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD10 / FN09 / PO09；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-05/C-06；api-review U10/U11/U12；实际头安装规则和宏条件布局。

  **Implementation and review**: 建立contracts/api-exposure.json逐符号/头分层清单；application/provider umbrella与advanced/internal边界；修复安装include闭包，保留合法旧consumer兼容；ONNX按C-08 FN09固定无条件PImpl及disabled分支完整Impl。开始时核对实际源码路径。

  **Exit / oracle**: 每个安装公共头单独包含；外部consumer仅用安装prefix/pkg-config编译链接及构造析构；ONNX enabled/disabled各自同配置安装消费，normal及ASan构造/析构通过。Spec185InstalledApi记录include/link/运行边界，不以--help代替。已通过，见[evidence/b0-installed-api.md](evidence/b0-installed-api.md)。

<a id="t017"></a>

- [x] T017 [US1] Core Operation Runtime and Channels — ndn-service-framework/OperationRuntime.hpp/.cpp; ndn-service-framework/OperationState.hpp; tests/unit-tests/core-operation-runtime.t.cpp; tests/installed-api/core-operation-consumer.cpp; wscript; tests/wscript

  **Batch / Depends**: B0C / B0 exit。

  **Design binding**: [C-09](contracts/core-app-boundary.md#concrete-design-binding) CB01,CB02 / PO-C1,C2；文件、字段owner及关键签名按契约，状态PLANNED。

  **Implementation and review**: 提取通用调度/ticket/close/drain、完成等待/退订/可靠reader；复用Core已有stream传输，不导入DI领域类型。逐任务review-agent只读静态门。

  **Exit / oracle**: Core-only安装消费者无DI/Python/ONNX依赖；PO-C1全部C++竞态/生命周期反例通过，TSan重复两次；批末与T018共同验证。已通过，见[evidence/b0c-core-operation.md](evidence/b0c-core-operation.md)。

<a id="t018"></a>

- [x] T018 [US2] DI Delegation to Core Operations — NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp/.cpp; tests/integration-tests/di-core-operation.t.cpp; tests/wscript

  **Batch / Depends**: B0C / T017 static；启动前核对Spec184同树生产链及并行源码变化。

  **Design binding**: [C-09](contracts/core-app-boundary.md#concrete-design-binding) CB03,CB04 / PO-C3,C4；旧签名兼容、领域完成时机不变。

  **Implementation and review**: 删除DI私有executor与重复通用等待状态，桥接Core State；保留模型/attempt/会话语义，复用BeginCollaboration/CommitCollaborationPlan/CancelCollaboration及scoped registration。静态审查不得把stream final当durable完成。

  **Coverage lanes**: production entry/callers→NativeInferenceClient/Core-only consumer；implementation and wire→C-09 CB01–CB04及旧Core协议；test/harness/oracle→PO-C1–C3及C++fixture；build/source closure→wscript/安装消费者/PO-C2,C4；migration/evidence→旧API、重复owner退出、PO-C4及批次证据。安全/错误/并发穿过五lane核对。

  **Exit / oracle**: 两任务静态门与组合流程审查后运行Spec185CoreOperation、Spec185DiCoreOperation及受影响Core流/协作/注册回归；记录review-agent路径/SHA、候选与selector、static/compile-link/runtime-test/unobserved漏检复盘。C++断言闭合且依赖零反向边才CLOSED_FOR_VALIDATION，否则保留OPEN_FOR_NEXT_BATCH及具体触发条件；已通过，结果见[evidence/b0c-core-operation.md](evidence/b0c-core-operation.md)。

<a id="t001"></a>

- [x] T001 [US1] Runtime Configuration and Export — Runtime.hpp/Runtime.cpp; root wscript; tests/unit-tests/di-runtime.t.cpp

  **Batch / Depends**: B1 / B0C exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD01 / F01–F04 / FN01 / FLOW01 / PO01；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeInferenceClient constructors, NativeRequestRuntime, examples/DI_NativeRequester.cpp; C-01。

  **Implementation and review**: 提取现有operator配置组合为Runtime::open；校验profile、trust、所有资源上限；新增公开头安装/target map。RuntimeConfig.nativeConfigPath沿用当前native requester-v1配置；单用户default规则见C-01；taskContract独立绑定，不能从catalog猜测。

  **API revision**: C-05/C-06：启动冻结model key注册与精确配置；稳定application头不泄露advanced/native类型。

  **Exit / oracle**: 有效配置可创建User；非default profile拒绝、错误trust/零预算/初始化中途失败无残留；公开头消费链接成功。

  **Completion**: B1 `PASS`; static v7, normal/TSan C++ selectors, and installed-prefix consumer are recorded in [evidence/b1-runtime.md](evidence/b1-runtime.md).

<a id="t002"></a>

- [x] T002 [US1] Runtime Shutdown and Child Ownership — Runtime.cpp; tests/unit-tests/di-runtime.t.cpp

  **Batch / Depends**: B1 / T001 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD01,CD04 / F01,F02,F10 / FN01,FN04 / FLOW07 / PO01,PO04；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: SerialRequestExecutor, NativeInferenceClient::close, C-01 lifetime。

  **Implementation and review**: 按C-06实现原生drainAsync及可退订token；接入owner registry、close/drain和失败逆序清理；weak callback断环；补prepare/wait/drain的owner线程拒绝路径和最后owner释放流程。

  **Lifecycle completeness**: C-07 Runtime外壳析构即close；drainAsync屏障排除自身通知，保留安全State及join；测试子对象仍在/已释放和owner线程最后释放。

  **Exit / oracle**: close幂等、回调中close无自join、drain超时可重试；外部Face/IO fixture显式寿命；TSan同矩阵两次通过。

  **Completion**: B1 `PASS`; static v4 after repair, normal/TSan lifecycle runs, and C++ installed consumer are recorded in [evidence/b1-runtime.md](evidence/b1-runtime.md). Public owner-thread prepare/request paths remain deferred to later tasks.

<a id="t016"></a>

- [x] T016 [US4] Extension Registration and Cooperative Control — NativePlanning.hpp/NativePlanning.cpp; NativeModelRunner.hpp/NativeModelRunner.cpp; NativeRequestPlanner.cpp; tests/unit-tests/di-extension-contract.t.cpp

  **Batch / Depends**: B2E / B1 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD08,CD09 / F16 / FN08 / FLOW03 / PO08；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-05 Extension Lifecycle；api-review U13/U14/U15；实际registry、strategy及runner调用方。

  **Implementation and review**: startup builder后freeze，duplicate拒绝、显式replace仅限freeze前；runner实例并发所有权；新strategy/control端口带deadline/cancel。旧非协作端口留高级兼容，普通Runtime拒绝不满足控制契约的插件，不声称强制抢占任意回调。

  **Exit / oracle**: Spec185ExtensionRegistry覆盖重复/冻结/替换、并发lookup、协作超时/cancel、旧插件拒绝及runner隔离；TSan同矩阵两次；过期不能发布Selection。

  **Completion**: B2E `PASS`; v12 static re-review and prior composition pass, normal/TSan C++ selectors, and installed-prefix extension consumer are recorded in [evidence/b2e-extensions.md](evidence/b2e-extensions.md). Full Waf packaging and later preparation/request/Provider/Python exits remain unobserved.

<a id="t003"></a>

- [x] T003 [US1] Verified Package Preparation — PreparedModel.hpp/PreparedModel.cpp; ModelPreparationCache.hpp/ModelPreparationCache.cpp; tests/unit-tests/di-preparation.t.cpp

  **Batch / Depends**: B2 / B2E exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD02 / F04–F09 / FN02 / FLOW02 / PO02；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeRequestCatalog/NativeCanonicalPreparationCatalog/NativeRequestPreparation; C-02。

  **Implementation and review**: 实现User::prepare的owned source路径及既有受保护fetch接线；校验配置/双graph/initializer/adapter；将冻结catalog收进Package，构造后才发布。补独立内容oracle和生产解析计数。

  **API revision**: C-05/C-06：普通prepare("default")与原生prepareAsync共享唯一owner；只读capabilities/输入输出schema；高级PrepareRequest另层保留。

  **Exit / oracle**: 冷准备成功；错digest/任务/JSON冒充ONNX/缺initializer拒绝；无grant或Provider副作用，预算限制生效。

  **Completion**: B2 `PASS`; static/combination review, normal and repeated clang/TSan C++ selectors, independent graph oracle, and Runtime/Core affected suites are recorded in [evidence/b2-preparation.md](evidence/b2-preparation.md). Request, conversation, Provider, Python and full qualification remain deferred to later batches.

<a id="t004"></a>

- [x] T004 [US1] Single Flight Refresh and Leases — ModelPreparationCache.cpp; tests/unit-tests/di-preparation.t.cpp

  **Batch / Depends**: B2 / T003 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD02,CD04 / F06–F10 / FN02,FN04 / FLOW02 / PO02,PO04；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-02 matrix/key/generation/lease。

  **Implementation and review**: 实现四种policy、独立waiter/job deadline、取消、generation CAS、预算/LRU和活动lease；补8线程及refresh交错fixture，真实调用缓存owner。

  **API revision**: C-06：PreparationHandle代表独立waiter；可靠completion迟注册仍一次交付，取消单waiter不能终止共享job。

  **Lifecycle completeness**: C-07 PreparationHandle最后副本释放取消该waiter；resultAsync仅取消等待；移除普通PrepareOptions取消回调，native handle统一取消。

  **Exit / oracle**: C-02全部反例；单waiter取消不影响其他人；Refresh失败旧对象可用；8并发仅一次fetch/inspect；TSan重复两次。

  **Completion**: B2 `PASS`; single-flight, refresh generation, lease/LRU/budget, waiter cancellation/deadline and exactly-once completion cases passed in normal and repeated clang/TSan C++ selectors. Full cross-process and Python qualification remain unobserved; see [evidence/b2-preparation.md](evidence/b2-preparation.md).

<a id="t005"></a>

- [x] T005 [US2] Prepared Request Projection — PreparedModel.cpp; NativeInferenceClient.hpp/NativeInferenceClient.cpp; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B3 / B2 exit + Spec184 scoped dependency gate。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD03 / F03,F05,F16 / FN03,FN08 / FLOW03 / PO03,PO08；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeRequestPlanner.cpp, NativeRequestPreparation.cpp, C-01/C-03。

  **Implementation and review**: 将Package绑定既有client，保留旧五参数签名与逐请求expectedModel检查；映射Input/DataRef、默认与覆盖placement、generation/stream；不得缓存grant/candidate/plan。

  **API revision**: C-05：原生Input.text按adapter能力开放，不引入Python tokenizer；DataRef保留完整受保护引用，Provider继续取得/解密。

  **Exit / oracle**: `PASS` in B3；2次request共用Package且ID/授权独立；hot-cache revoke拒绝；repository错digest/size拒绝；invalid placement不能产生可执行Selection；详见[evidence/b3-request.md](evidence/b3-request.md)。

<a id="t006"></a>

- [x] T006 [US2] Handle Deadlines Events and Cancellation — PreparedModel.hpp/PreparedModel.cpp; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B3 / T005 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD03,CD04 / F10–F12 / FN04 / FLOW04,FLOW07 / PO04；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeInferenceHandle::result/observe/cancel; C-01 handle compatibility。

  **Implementation and review**: 实现RequestHandle wrapper及lease，wait双重载；逐项对照历史回放/观察者异常/慢消费者行为；记录有界队列现状和支持能力，不发明全局ASSEMBLING。

  **API revision**: C-05/C-06：统一Result/DiError/status；可退订Subscription、原生onCompletion与EventReader next/nextAsync、单游标、有界队列和STREAM_GAP独立于best-effort observe；覆盖慢读者、溢出、迟订阅和取消竞争。

  **Lifecycle completeness**: C-07 observe/nextAsync返回Subscription；验证退订不吞事件、错误流不伪装EOF、READ_IN_PROGRESS、64订阅额度回收及moved-from。

  **Exit / oracle**: `PASS` in B3；局部wait超时后可取成功；deadline终态不可重复；cancel/complete/close交错；观察者不成为提交oracle；normal 与 ASan/UBSan + LSan 无抑制通过；详见[evidence/b3-request.md](evidence/b3-request.md)。

<a id="t007"></a>

- [x] T007 [US2] Prepared Conversations and Committed Checkpoints — Conversation.hpp/Conversation.cpp; tests/integration-tests/di-prepared-conversation.t.cpp

  **Batch / Depends**: B4 / B3 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD05 / F13 / FN05 / FLOW05 / PO05；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeConversationCoordinator/NativeConversationJournal; C-03。

  **Implementation and review**: 接入openConversation、request、checkpoint、close；Package/模型/任务/tokenizer绑定；单会话turn串行；由coordinator生成parent/role map，保留durableCommitGate。

  **API revision**: C-03：公开opaque ConversationCheckpoint，避免应用传receipt/commit owner；恢复仍校验安全绑定。

  **Exit / oracle**: 两轮真实native结果；stream final未commit时无新checkpoint；并发turn拒绝；不支持adapter及错模型checkpoint拒绝。B7 C++ qualification and composition evidence: [evidence/b7-cpp-qualification.md](evidence/b7-cpp-qualification.md#b7-final-candidate-convergence-20260915)。

<a id="t008"></a>

- [x] T008 [US2] Conversation Recovery Replacement and Export — Conversation.cpp; tests/integration-tests/di-prepared-conversation.t.cpp

  **Batch / Depends**: B4 / T007 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD05 / F13 / FN05 / FLOW05 / PO05；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: Spec184 durable/export evidence; existing checkpoint export helper; C-03。

  **Implementation and review**: 复用原子private export与restore；配置恢复/替换接同一coordinator；覆盖成功提交后取消、Provider替换和Runtime重开；禁止第二套journal。

  **Exit / oracle**: 恢复后parent/hash chain一致；replacement attempt隔离；export失败旧文件保留；durable成功不降级；ASan/UBSan同selector两次。B7 C++ qualification and composition evidence: [evidence/b7-cpp-qualification.md](evidence/b7-cpp-qualification.md#b7-final-candidate-convergence-20260915)。

<a id="t009"></a>

- [x] T009 [US3] Provider Facade and Authenticated Assembly — Provider.hpp/Provider.cpp; NativeInferenceProvider.cpp; examples/DI_NativeProviderExecutable.cpp; tests/integration-tests/di-prepared-provider.t.cpp

  **Batch / Depends**: B5 / B4 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD06 / F02,F14 / FN06 / FLOW06,FLOW07 / PO06；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: NativeProviderHandler/NativeRunnerPreparation/NativeCanonicalOnnxAssembler; C-03。

  **Implementation and review**: 从executable提取复用组合owner；serve/registration/stop/drain绑定既有handler；保留认证前后guard；将可观测source_fetch/assembly/runner_created counters接到生产边界。

  **API revision**: C-06：Provider::drainAsync及关闭竞争；独立ProviderConfig::fromFile/fromCommandLine及Runtime::open(ProviderConfig)，无User目录可单独serve；CLI复用C++ parser，非法字段拒绝。

  **Lifecycle completeness**: C-07重复serve拒绝；registration析构停新接收、Provider handle析构不stop；测试Runtime.close与已接收工作收敛。

  **Exit / oracle**: 无Selection时fetch/assembly=0；有效Selection执行；wrong provider/epoch/grant拒绝；registration关闭及stop清理不悬空。已由 B5 composition v5、normal v22/v21-v23 和 sanitizer v3/v2-v3 真实 C++ 证据闭合，详见 [b5-provider](evidence/b5-provider.md#b5-composition-review-pass-v5-and-closure)。

<a id="t010"></a>

- [x] T010 [US3] Protected Artifact and Runner Template Reuse — Provider.cpp; NativeRunnerPreparation.cpp; NativeProtectedArtifactStore.cpp; tests/integration-tests/di-prepared-provider.t.cpp

  **Batch / Depends**: B5 / T009 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD07 / F15 / FN07 / FLOW06 / PO07；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-03 Provider cache layers。

  **Implementation and review**: 增加受限artifact/template cache及backend支持矩阵；key绑定role/recipe/ABI/device/security；per-request mutable runner与plaintext lease；命中仍重验Selection/grant。

  **Exit / oracle**: 相同artifact避免重复assembly；不同ABI/role/epoch不误命中；KV不串请求；撤销拒绝；drain后lease=0；ASan/UBSan两次。已由 B5 composition v5、normal v22/v21-v23 和 sanitizer v3/v2-v3 真实 C++ 证据闭合，详见 [b5-provider](evidence/b5-provider.md#b5-composition-review-pass-v5-and-closure)。

<a id="t011"></a>

- [x] T011 [US4] Native Caller Migration and Compatibility Registry — examples/DI_NativeRequester.cpp; examples/wscript; tests/integration-tests/di-prepared-compatibility.t.cpp; contracts/caller-matrix.md

  **Batch / Depends**: B6 / B5 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD10,CD11 / FN09,FN10 / FLOW01–FLOW07 / PO09,PO10；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: actual NativeInferenceClient callers, Spec184 caller-matrix, C-04。

  **Implementation and review**: 盘点维护调用方并按shared backend分组；C++ requester/provider使用Runtime/PreparedModel；保持旧签名对照与显式route日志；安装头消费示例，建立symbol→TU→target映射。

  **API revision**: C-06：安装prefix外部consumer仅包含api.hpp/provider.hpp；交付同步/异步prepare、unary/stream、conversation/recovery/replacement及Provider示例，不导入Python。

  **Lifecycle completeness**: C-07 A01–A64逐组完整外部C++例子与行为oracle；新增入口必须更新表，普通应用不得引用Native内部头。

  **Exit / oracle**: 新旧结果/失败语义对照；C++真实进程unary/stream先通过；matrix无漏项；nm/readelf与安装消费链接确认。

<a id="t013"></a>

- [x] T013 [US4] Current Candidate Process Qualification — tests/integration-tests/di-prepared-process.t.cpp; tests/wscript; evidence/b7-cpp-qualification.md

  **Batch / Depends**: B7 / B6 exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD11 / FN10 / FLOW01–FLOW07 / PO01–PO10（资格fixture）；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: C-04 full five-lane convergence and dynamic matrix。

  **Implementation and review**: 先审查完整调用链/source closure，再构建同树candidate并刷新binary receipt；独立authority/requester/provider完成unary/stream/continuation/recovery/replacement/cancel/revoke/cleanup，C++ oracle判定。

  **API revision**: C-06：T012之前完成全部native矩阵、安装消费和ELF/子进程no-Python闭包；不可用Python PASS补缺，SC-005包装行留后批。

  **Lifecycle completeness**: C-07生命周期矩阵全部原生反例先通过；Python随后仅验边界语义。

  **Exit / oracle**: SC-001至SC-008的原生部分逐条证据；SC-005 Python部分留T012；no-Python ELF/进程依赖；不同安全域和cache命中反例；无startup/collector错误冒充协议结果。已完成 normal 与 ASan/UBSan `detect_leaks=0` C++ matrix、安装 C++ caller 和 24-artifact ELF receipt；外部 OpenABE leak-enabled ASan 限制见唯一 B7 evidence，不计产品 PASS。

<a id="t012"></a>

- [x] T012 [US4] Thin Python Prepared Model Facade — pythonWrapper/src/ndnsf/di_bindings.cpp; NDNSF-DistributedInference/ndnsf_distributed_inference/api/__init__.py; NDNSF-DistributedInference/ndnsf_distributed_inference/api/_async.py; NDNSF-DistributedInference/ndnsf_distributed_inference/provider_api.py; tests/python/test_spec185_prepared_model.py; tests/python/test_spec182_native_bindings.py

  **Batch / Depends**: B8 / B7 C++ qualification exit。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD12 / F10 / FN10 / C-07 mappings / PO10；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: existing binding declarations and maintained APPClient paths; C-01/C-04。

  **Implementation and review**: 在现有绑定模块导出同一native对象；prepare/wait/drain释放GIL，observe正确获取GIL并保持owner；迁移维护Python用户路径，兼容shim保留/删除逐项记录，不在Python重做planning。

  **API revision**: C-05/C-06：固定导出、timeout_s、结构化错误、上下文退出与asyncio adapter；桥接C++ completion/nextAsync，禁止Python状态机或线程补齐缺失native能力。

  **Lifecycle completeness**: C-07直接pybind对象/便利方法逐项映射；start_prepare保留显式handle；async取消/GC/loop关闭/解释器退出反例；不引入Python领域owner。

  **Exit / oracle**: Python输入/异常/事件映射和寿命通过；native断言仍C++；无静默legacy fallback；实际模块路径已按当前 `api/` 与 `provider_api.py` caller 映射更新。v19 静态、DI/extension compile-link、31项 Python 回归及刷新后的 C++ process/request/conversation selectors 均有证据；subinterpreter、wheel packaging 与 wrapper sanitizer 保持未观测。

<a id="t014"></a>

- [x] T014 [US4] Design API and Scoped Handoff — Design/; specs/185-prepared-model-runtime/; docs/architecture.md

  **Batch / Depends**: B9 / T012 acceptance。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) N/A生产类改动；实际Design/API/PDF与导出/证据交付，见FN10；Design status以C-08范围审查为准，开工前核对当前源码。

  **Read / contract**: Design/MANAGEMENT.md, C-04, Spec184 outstanding registry。

  **Implementation and review**: 更新实际当前API/中文契约/源码摘要/三类图/双PDF；核对caller退出和每FR/SC证据；记录184仍未完成外部资格，提交明确candidate/source交付入口。

  **API revision**: C-05/C-06：同步六层API exposure manifest、全声明索引及独立C++指南；原生与包装验收分别记录。

  **Lifecycle completeness**: 核对C-07全表、exposure、安装头和Python实际导出，advanced/CLI不绑定必须显式登记。

  **Exit / oracle**: 所有本Spec任务完整验收且链接可追溯；当前设计不混入planned；双PDF身份/排版通过；184不被自动勾选。B9 static review v05 and final composition `B9_COMPOSITION_PASS` are recorded in [evidence/b9-handoff.md](evidence/b9-handoff.md)。

<a id="t019"></a>

- [x] T019 [US1] Prepared Client Ownership and Eviction — PreparedModel.hpp/PreparedModel.cpp; Runtime.cpp; NativeCanonicalPreparationCatalog.*; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B7R / T003/T005 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD02,CD04 / F06,F10 / FN02,FN04 / FLOW02,FLOW07 / PO02,PO04。

  **Implementation and review**: PreparedModel copies share one lazy client owner; Runtime lookup/snapshot is weak; pending handles retain the client; the C++ fixture observes exact catalog source lifetime across request/cancel/release and `maxEntries=1` eviction.

  **Exit / oracle**: normal and ASan/UBSan eviction selectors pass with source weak owner expired after the cache evicts an idle package; no public API or wire change.

<a id="t020"></a>

- [x] T020 [US2] Conversation Terminal Admission Ordering — NativeConversationContinuation.hpp; NativeInferenceClient.cpp; Conversation.cpp; tests/integration-tests/di-prepared-request.t.cpp

  **Batch / Depends**: B7R / T007 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD05 / F13 / FN05 / FLOW05 / PO05。

  **Implementation and review**: Internal conversation abort and the process-local terminal hook run before terminal event/Core result visibility; public completion remains an observer path. The two-turn C++ oracle passes normal and ASan/UBSan.

  **Exit / oracle**: delayed completion notification and a separate failed-turn immediate retry are covered by the C++ worker-gate fixture; normal and ASan/UBSan selectors each pass twice. The public successful result-to-next-turn path, checkpoint/recovery and drain remain covered by the same native case.

<a id="t021"></a>

- [x] T021 [US2] Generation Identity and Grant-Bound Provider Cache — PreparedModel.cpp; ProviderArtifactCache.hpp; C-03 execution contract; tests/integration-tests/di-prepared-provider.t.cpp

  **Batch / Depends**: B7R / T007/T010 static。

  **Design binding**: [C-08](contracts/code-design.md#design-binding-and-readiness) CD03,CD05 / F05,F13 / FN03,FN05 / FLOW03,FLOW05 / PO03,PO05。

  **Implementation and review**: Merge model generation defaults before rebinding the current continuation identity; keep protected artifact cache identity bound to authenticated Provider/grantName/grantDigest and document independent-grant isolation. The C++ cache oracle passes same-grant hit and independent-grant cold behavior in normal and ASan/UBSan.

  **Exit / oracle**: normal and ASan/UBSan production protected Provider selectors under two independent grants observe sourceFetches=2, assemblies=2, templateHits=0, runnersCreated=2 and two C++ runner executions; the task is PASS. Cross-grant reuse remains intentionally unclaimed because grant identity is part of the protected cache key.

## Fragmentation Review

21个任务按12个产品批次及B7R审计修复门组织；B0安装/ABI、B2E扩展边界有独立出口；B0C及B1–B5成对组合，B6–B9分别迁移、完整C++资格、Python包装与文档，B7R重新验证生命周期和身份缺口。任务卡按registry实际执行顺序排列，保留原ID并登记审计修复ID；详细分组依据见[执行表](batch-execution.md#allocation-and-closure-decision)。
Runtime与cache不同owner、request与conversation不同持久语义、Provider与Python不同安全/验收边界，因此不合并。
T013是真实集成验收，T014是源码/API/资格交付，不替代前面的行为测试。

2026-09-14 01:53 -05:00 B7 T013 修复复审：对不可变快照 `.codex-tmp/spec185-t013-review-v12-20260914` 的官方 `review-agent` 返回 `STATIC_PASS`，无 P0-P3；diff SHA256=`e7490697a283386df5c49731360e8e91e6adf036f519018a2eefc4cab0f32712`，paths SHA256=`7ebf1bbebff4e3fd77ef7a5bad58ce657c9adc21574388d4c877ada06e13fed2`。确认 `OperationRuntime.cpp` 仅加入 `integration-tests` 手工 framework source closure，五 lane 覆盖；等待批末重新构建，T013 仍为 `PARTIAL`。
2026-09-14 01:55 -05:00 B7 T013 compile-link：v3 使用系统优先工具链与 `-j4`，构建 `spec185-process`、`spec185-prepared-request`、`spec185-provider-assembly`、`integration-tests` 全部成功链接，耗时 18.80 秒；无持续 swap。C++ selector、真实跨进程矩阵、sanitizer 尚未运行，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](evidence/b7-cpp-qualification.md)。
2026-09-14 01:56 -05:00 B7 T013 批次组合复审：官方 `review-agent` 对同一不可变 v12 快照返回 `B7_COMPOSITION_PASS`，无 P0-P3；确认 C++ 进程 selector/native oracle、Python 生命周期编排、C-04 矩阵、双次 selector、超时、日志边界和 `OperationRuntime.cpp` 构建闭包均一致。运行、sanitizer 与残留进程清理尚未验证，T013 保持 `PARTIAL`。
2026-09-14 02:03 -05:00 B7 T013 runtime v1：`spec185-process` 返回 `rc=201`。stream conversation 在 native input 返回 `UNSUPPORTED_CAPABILITY`（catalog 未声明 `conversation_input`）；`spec185-prepared-request` 的 `PreparedRequestCompletesThroughProvider` 返回 `native result wait timed out`；revoke 编排因 Controller 60 秒撤销而只等待 30 秒未见 `NDNSF_REVOCATION_APPLIED`。原始运行目录已保留在 `.codex-tmp/spec185-b7/runtime-v1/`，三个边界分别记录于 B7 证据；T013 保持 `PARTIAL`，sanitizer 未运行。
2026-09-14 02:08 -05:00 B7 T013 runtime repair static v13：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v13-20260914` 返回 `STATIC_PASS`，无 P0-P3；diff SHA256=`ef3688ef12e744a9be5f4abe4eeb50c30e7a1a4778295f8dcdaf442fb1001c2c`，paths SHA256=`6b717d23f180e720eab4a9d61c236614df9f23dd88b04387a8bb504252488e27`。确认 conversation encoder 声明、revoke 等待预算及 C++ oracle 接线；复用 v3 同一 binary candidate，进入受控 runtime 重试，T013 保持 `PARTIAL`。
2026-09-14 02:16 -05:00 B7 T013 focused timeout：单独运行 `Spec185PreparedRequest/PreparedRequestCompletesThroughProvider` 仍在 `NDNSF_INTEGRATION_BOOTSTRAP_READY` 后约 5.8 秒返回 `native request budget expired`（`rc=201`），未到结果/清理断言；原始记录 `.codex-tmp/spec185-b7/prepared-timeout-focus-v1.log`、`.rc`。这是诊断边界，T013 保持 `PARTIAL`。
2026-09-14 02:22 -05:00 B7 T013 focused trace：`prepared-timeout-trace-v1.log` 显示 request `...-1` 在约 4.74 秒已发布并接收 response；并发 request `...-2` 的 ACK 解密回调在 2 秒 ACK deadline 后才到达并被 `ACK_SKIPPED_AFTER_ACK_WINDOW` 丢弃，导致请求预算过期。将该 C++ fixture 的 request/ACK 预算调整为 10s/5s，生产代码不变；需受影响范围静态复审后重建，T013 保持 `PARTIAL`。
2026-09-14 02:31 -05:00 B7 T013 timeout fixture review v14：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v14-20260914` 返回 `STATIC_PASS`，无 P0-P3；diff SHA256=`263e8d14cb23debec2f383980cc39ccc1ebb57b7ff0d961dcef1466b2476f530`，paths SHA256=`20201a5c1c58efed82f64557270108bb576b169f7564f82d97f91276772ab7ed`。确认 C++ fixture 10s/5s 有界预算、conversation encoder、revoke 顺序、native oracle 和 Waf 闭包；需 fresh compile-link 后重试，T013 保持 `PARTIAL`。
2026-09-14 02:33 -05:00 B7 T013 compile-link v4：v14 静态门后以系统优先 `-j4` 重建同一四目标集合，`rc=0`，耗时 28.79 秒；`spec185-prepared-request` 重新编译链接，其余候选保持同一 build tree，资源记录无持续 swap。进入 runtime 重试，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](evidence/b7-cpp-qualification.md)。
2026-09-14 02:45 -05:00 B7 T013 focused retry v2：10s/5s fixture budget 后，单独 `PreparedRequestCompletesThroughProvider` 仍在约 10.7 秒返回 `native request budget expired`（`rc=201`，last checkpoint 883）。未形成 PASS；已保留 `.codex-tmp/spec185-b7/prepared-timeout-focus-v2.log`、`.rc`，需 trace 诊断后再改动，T013 保持 `PARTIAL`。
2026-09-14 03:00 -05:00 B7 T013 focused trace/diagnostic v2：`prepared-timeout-trace-v2.log`（`rc=201`）确认两个 request 的 ACK 均在约 100ms 内匹配且 `ackWindowExpired=false`，但 10 秒预算内没有 `COLLAB_ACK_CLOSED`、selection 或 Provider execution；临时诊断确认两个 request 均设置 `scheduleAckTimeout=true`、`scheduleImmediateAckTimeout=true`、`ackTimeoutMs=5000`，却没有 `ACK_TIMEOUT_CALLBACK`，strace 也未见 5 秒 ACK timerfd。边界转为生产 scheduler/state-machine 问题，T013 保持 `PARTIAL`，诊断代码不计 PASS；证据见 [b7-cpp-qualification](evidence/b7-cpp-qualification.md)。
2026-09-14 07:58 -05:00 B7 T013 bounded-pump repair review v15：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v15-20260914` 返回 `STATIC_PASS`，无 P0-P3；changes SHA256=`37db1c903cf8299b5a53404816b754bc39934fe0a1c697d19819ad3ab9be3bf4`，paths SHA256=`20201a5c1c58efed82f64557270108bb576b169f7564f82d97f91276772ab7ed`。确认 C++ fixture 在 future 等待前最多继续四轮 `pumpUntil`，覆盖 5 秒 ACK scheduler 边界，不改变生产 timeout/owner 语义；compile-link、runtime、sanitizer 仍待执行，T013 保持 `PARTIAL`。
2026-09-14 03:04 -05:00 B7 T013 compile-link v5：v15 静态门后，以系统优先 PATH、`-j4` 重建 `spec185-process`、`spec185-prepared-request`、`spec185-provider-assembly` 和 `integration-tests`，`rc=0`，耗时 107.22 秒；因当前工作树时间戳重新编译 `ServiceUser.cpp` 与聚焦 fixture，未见持续 swap。原始日志 `.codex-tmp/spec185-b7/process-build-v5.log`、`.rc`、`.vmstat.log`；仍未计入 runtime 或 sanitizer PASS，T013 保持 `PARTIAL`。
2026-09-14 03:04 -05:00 B7 T013 focused runtime retry v3：使用 v5 候选仅运行 `Spec185PreparedRequest/PreparedRequestCompletesThroughProvider`，`rc=0`，约 11.09 秒，`*** No errors detected`。四轮有界 `pumpUntil` 让真实 User Face scheduler 跨过 5 秒 ACK 窗口；这是聚焦 C++ 运行证据，完整 process 矩阵、sanitizer、清理和批次组合收口仍待执行，T013 保持 `PARTIAL`，详见 [b7-cpp-qualification](evidence/b7-cpp-qualification.md)。
2026-09-14 03:27 -05:00 B7 T013 repair static v17：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v17-20260914` 返回 `STATIC_PASS`，无 P0-P3；`NativeRequestCatalog.cpp` 的 `TENSOR_BUNDLE_TOKEN_IDS` 与 `NativeEpochCoordinator` 共享 Int64 `input_ids` 提取契约，stream 普通路径缩进和 fixture 声明已修复，五 lane 静态覆盖完成。changes SHA256=`32065d2bdcb3e0da550d9953ddb532108d9c3da691e30fe848edc69202edfa70`，paths SHA256=`56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`；未构建/运行，T013 保持 `PARTIAL`，进入 fresh compile-link。
2026-09-14 03:29 -05:00 B7 T013 compile-link v6：v17 static pass 后，以系统优先 PATH、`-j4` 在既有 `build-spec185-b0c-normal` 重建 `spec185-process`、`spec185-prepared-request`、`spec185-provider-assembly` 和 `integration-tests`，`rc=0`，耗时 28.79 秒；`NativeRequestCatalog.cpp` 的 normal/test/provider/integration 对象均重新编译，资源记录无持续 swap。原始日志 `.codex-tmp/spec185-b7/process-build-v6.log`、`.rc`、`.vmstat.log`；仅 compile-link PASS，runtime、sanitizer 和批次收口仍待执行，T013 保持 `PARTIAL`。
2026-09-14 03:32 -05:00 B7 T013 conversation runtime v3：v6 C++ candidate 仅运行 `ConversationRecoveryAndReplacementRemainTerminal`，`rc=0`，约 116.35 秒，`*** No errors detected`。两次 conversation 正向 checkpoint/second-turn、recovery failure、replacement 与 no-backup rejection 均通过 C++ selector 和 native process logs；该子矩阵已通过，完整 process、revoke、sanitizer、清理和最终 composition 仍待执行，T013 保持 `PARTIAL`。
2026-09-14 03:32 -05:00 B7 T013 process runtime v3：v6 candidate 的 unary/stream、conversation/recovery/replacement 与 negative cache/deadline/revoke 前置矩阵通过；`RealCrossProcessRevokeFailsClosed` 仅因 C++ oracle 依赖未稳定发出的 `NDNSF_DI_PROVIDER_HANDLER_TIMING event=start` 失败，Provider 日志同一 baseline 已有真实 `event=PROVIDER_EXECUTE_DONE`。原始 `.codex-tmp/spec185-b7/process-runtime-v3.log`、`.rc` 和 `/tmp/spec185-b7-unary-2204547-13/` 保留；不计完整 runtime PASS，T013 保持 `PARTIAL`。
2026-09-14 03:39 -05:00 B7 T013 oracle repair static v18：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v18-20260914` 返回 `STATIC_PASS`，无 P0-P3；C++ revoke oracle 改为检查真实 Provider `event=PROVIDER_EXECUTE_DONE`，保留 baseline 有执行、Controller revocation marker、revoked requester fail-closed 和撤销后零执行。changes SHA256=`0549e53000e79aadd8665801cbfde66058b39051e9d0768d22ac046c1ee4f1db`，paths SHA256=`b227804f0d93ccae7b4548db087215b801b1f5b25797fdfeacb47f3ff16a2296`；未构建/运行，T013 保持 `PARTIAL`，进入受影响 selector 重建。
2026-09-14 03:42 -05:00 B7 T013 compile-link v7：v18 static pass 后以系统优先 PATH、`-j4` 在既有 `build-spec185-b0c-normal` 仅重建受影响 `spec185-process`，`rc=0`，耗时 9.00 秒；`di-prepared-process.t.cpp` 重新编译并链接，v6 的其他三个目标保持同一 production candidate。原始日志 `.codex-tmp/spec185-b7/process-build-v7.log`、`.rc`、`.vmstat.log`；仅 compile-link PASS，等待 revoke/runtime 重跑，T013 保持 `PARTIAL`。
2026-09-14 03:47 -05:00 B7 T013 final composition v19：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-composition-v19-20260914` 返回 `B7_COMPOSITION_PASS`，无 P0-P3；五 lane、C-03/C-04/C-06/C-08 调用链、OperationRuntime/source closure、C++ oracle 与 Python lifecycle 边界均闭合。changes SHA256=`e3fd98d94cd22d8637cca8d9c62de4c5ee02c60073d59b0775460f84117fac8b`，paths SHA256=`56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`；组合静态门通过但动态 qualification 仍 `OPEN_FOR_VALIDATION`，T013 保持 `PARTIAL`。
2026-09-14 03:52 -05:00 B7 T013 revoke runtime v4：v7 C++ process selector 仅运行 `RealCrossProcessRevokeFailsClosed`，两次均 `rc=0`，约 133.28 秒，`*** No errors detected`。每次都通过 baseline requester success、Controller revocation、revoked requester `DI_NATIVE_NO_ADMITTED_PROVIDER`、native failure terminal，以及 Provider baseline `event=PROVIDER_EXECUTE_DONE` 与撤销后零执行断言；完整 normal process 结果与该修复后的 revoke 子矩阵已闭合，sanitizer、清理和最终提交仍待执行，T013 保持 `PARTIAL`。
2026-09-14 04:24 -05:00 B7 T013 fixture owner repair review v22：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v22-20260914` 返回 `STATIC_PASS`，无 P0-P3；结果状态与线程 owner 分离，析构取消/连接边界不抛异常，初始化失败会取消 active request，正向/负向/drain 均移除 `std::async` future predicate。changes SHA256=`c096cdcf2b763d9407863673bcbc419c821cfd5d71f769f50f27ebbd9551a101`，paths SHA256=`4a8f0f88415fa22f9cf6f5f27c43c4da9888b8e637acc73f2f0b007e2e224d09`；进入组合复审，T013 保持 `PARTIAL`。
2026-09-14 04:27 -05:00 B7 T013 final composition review v23：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-composition-v23-20260914` 返回 `B7_COMPOSITION_PASS`，无 P0-P3；五 lane 与 C-03/C-04/C-06/C-08 调用链、C++ fixture/oracle、构建闭包和证据边界通过。changes SHA256=`f113752f96814e36081dd021ffa623f24bce6b82c516ed08b5f17ef741c8a690`，paths SHA256=`56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`；动态资格仍 `OPEN_FOR_VALIDATION`，T013 保持 `PARTIAL`。
2026-09-14 04:31 -05:00 B7 T013 compile-link v8：v22 static pass 与 v23 composition pass 后，以系统优先 PATH、`-j4` 在既有 `build-spec185-b0c-normal` 仅重建受影响 `spec185-prepared-request`，`rc=0`，耗时 24.92 秒；原始日志 `.codex-tmp/spec185-b7/process-build-v8.log`、`.rc`、`.vmstat.log`，无持续 swap，T013 保持 `PARTIAL`，进入 focused runtime。
2026-09-14 04:32 -05:00 B7 T013 focused normal runtime v4：同一 v8 C++ candidate 的 `PreparedRequestCompletesThroughProvider` selector 返回 `rc=0`、约 17.87 秒，`*** No errors detected`；正向 Provider、错误 digest、撤销 fail-closed 和 drain 均通过。`PreparedConversationCommitsTwoNativeTurns` selector 返回 `rc=0`、约 15.52 秒，`*** No errors detected`；两轮 native turn、checkpoint/recovery、replacement 和 drain 均通过。原始日志 `.codex-tmp/spec185-b7/prepared-request-runtime-v4.log`、`.rc` 与 `prepared-conversation-runtime-v4.log`、`.rc`；sanitizer、清理和 T013 最终资格仍待执行，T013 保持 `PARTIAL`。
