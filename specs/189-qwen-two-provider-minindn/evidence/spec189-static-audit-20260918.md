# Spec189 Static Audit and Progress Reclassification

## B189-1a follow-up — 2026-09-18 15:42 -0500

官方只读 `review-agent` 对 B189-1a 冻结的十个文件完成复审，返回
`STATIC_PASS`，未发现 P0/P1/P2 控制性缺陷。该复审覆盖重型 hash/encrypt/Repo
commit 的 worker 边界、取消窗口、Repo rollback identity、wrapped-key RAII release、
准备线程 join/drain 以及 C++ fixture 的 owner 顺序。它没有运行构建、测试或模型。

因此，原先“同步大提交、无取消、name-only identity、publisher 强 pin”的静态缺陷
已经有对应修复和复审记录；原始 `STATIC_FAIL` 记录保留为历史边界，不再描述当前
B189-1a 状态。当前唯一必要的 B189-1a 缺口是走真实 `ModelPreparationCache`/
`PreparedModelPackage` owner 的回收反例，然后执行组合构建和 C++ runtime selectors。
原子材料 producer/consumer 仍属于后续 B189-1b，不能提前混入本批或把静态通过写成
T003 完成。

**Review trace**: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`; frozen ten-file
scope; no build or write by the reviewer. **Closure decision**: `OPEN_FOR_NEXT_BATCH`.

## Execution and ownership follow-up

**Updated**: 2026-09-18 14:50 -0500
**Scope**: `eedbbfd3` + current dirty source; documentation repair only.

实际仍为 1/7 capability tasks DONE（T001 仅 mapping），不是 14% 产品完成。
r25 run-record 仍 FAIL；原生 source-borrow r3 日志 Runtime prepare 38 assertions
通过，不能据此授予双 Provider 资格。protected Repo 13-file 草稿保持未验，未改源码。

| Finding | Evidence / consequence | Repair |
| --- | --- | --- |
| HIGH: receipt retention | NativeCanonicalArtifactPublisher.cpp 成功 emplace 到 CacheState::prepared，新增 servingLeases 随 receipt 强存；正常成功项没有 eviction 路径，手动 Core token reset 测试不能证明 package 回收 | T003 要求 preparation cache 统一预算/淘汰；真实 publisher/package eviction 反例，禁止第二个无界 owner |
| MEDIUM: batch inflation | T003 同时纳入 Core protected store、原子模型生产和复用；contract 曾拒绝共享接缝作为稳定出口 | B189-1a/1b 分别审查/验证，共用 T003 与唯一证据；不增加行政任务，不削弱 T003 总验收 |
| MEDIUM: prerequisite ambiguity | T008 前置但要求后续 T003/T006 才能提供的 native counters | host safety entry 前置，小 fixture 可继续；新 counters 归实际 owner，T009 前完整核对 |
| MEDIUM: contradictory resource criterion | FR-015 请求终态全部 baseline 与 AD-04/数据模型 warm cache 复用冲突 | active request 与 idle cache 分账；eviction/close 后核对保留 owner，绝不以永久 pin 解释正常缓存 |
| MEDIUM: stale wiring statement | requester 已存在 encrypted_repository/encryptedRangeStore 草稿；直接接 plain publisher 与 assembler encrypted fetch 不兼容 | 更新 current/target，保留 Core 加密/签名/serving、Repo 范围存储职责；草稿未运行，仍 PARTIAL |

**Scope reduction**: 保留先前 10→7 的合并成果；不另建 Qwen Repo、第二协议/serializer、
跨重启 key/serving 恢复、全局依赖再迁移或新报告框架。现有 host guard 不重做。
原子层 producer/consumer、授权、独立输出与真实重复验收均不可删除。

**Five lanes**: callers=Runtime/requester 注入草稿；implementation=publisher receipt/cache
与 assembler 整 initializer 路径；test=新 protected Core fixture 与实际 package eviction
缺口、历史 r3 selector；build=本轮文档不构建，后续 Core/DI ABI 受影响目标必须全局同步；
migration/evidence=tasks/plan/spec/contract/data-model/quickstart/traceability 一致性。
CodeGraph broad query 混入 .codex-tmp 快照，已用维护源码精确查询核对，不以索引推断通过。
**Four miss classes**: static=上述问题；compile-link=本轮未运行；runtime-test=保留历史
r25 FAIL 与 r3 focused PASS；unobserved=当前草稿、原子材料、真实输出/峰值/reuse/drain。
**Closure decision**: OPEN_FOR_NEXT_BATCH；下一步 B189-1a 修复/复审/原生验证，
不是重新开始 Spec189，也不立即重跑完整模型。

### Protected draft review disposition

官方只读 review-agent 对 `.codex-tmp/spec189-protected-repo-review-v1/` 的
13-file snapshot（diff SHA-256 `3d4d009cfb0dae9483267adba99603c61102735945a10db99712a6519cacafa8`）
返回 STATIC_FAIL：同步 commit 阻塞 Core I/O 且缺取消控制；receipt cache 强持 lease；
按 name 读/删缺少防同名替代的 identity fence（条件性风险，未证明当前 run 已触发）。
已纳入 B189-1a 契约/反例，未改冻结源码，不能开始该草稿 runtime 验收。
现有双 provider 失败与这些新草稿风险不能混为同一已证实根因。

### Follow-up verification

官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md` SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`；
只读代理复核 HEAD `eedbbfd3` 至九份 Spec 文档完整 diff 及周边契约，
冻结 v2 patch SHA-256 `d442b80584642c041f1babd45dae18bd6d8fdeb661fa5585c0a1f3b590888266`，
返回 DOCUMENTATION_STATIC_PASS；本段为其后追加的验证记录，不改变受审契约。
源码 13 文件逐一 SHA 与原冻结 manifest 一致，仍 STATIC_FAIL。
结构检查 PASS（7 tasks / 1 DONE / 25 FR / 6 SC；ID 不连续是保留合并历史的已解释 warning）；
11/11 技能入口/个人共享副本同步、前置文档、相对路径和 diff 检查通过。
Context Mode authority 刷新后 project/active health 通过。没有新增 native build、
模型运行、MiniNDN 或功能 PASS。仅提交本轮文档；原有源码/Design 草稿继续留在工作区。

**Updated**: 2026-09-18 02:22 -0500
**Mode**: full / post-test-adversarial
**Verdict**: `BLOCKED_FOR_NATIVE_EXECUTION`
**Scope**: Spec189 documents, maintained MiniNDN runner, the current C++ provider/coordinator path, and attempts r01-r21.

**Review trace**: official read-only `/root/spec185_review` review returned
`STATIC_FAIL` with no P0, three P1 and two P2 findings. It read the complete
Spec189 artifacts, runner and relevant C++ source without editing, building or
spawning another reviewer. The primary audit below incorporates that review;
the affected evidence and task statuses were corrected in this checkpoint.

This audit also installs the reusable
[experiment static re-review loop](../../../skills/speckit-code-design/references/experiment-static-review-loop.md)
as the retry rule for Spec189. A future run must preserve the prior attempt,
classify its first missing production boundary, record a real `Changed gate`,
freeze the changed source/configuration/oracle/build snapshot, and pass a new
read-only review before any rebuild or MiniNDN retry.

### Loop adoption review trace

| Field | Value |
| --- | --- |
| `Skill` | `skills/speckit-code-design/references/experiment-static-review-loop.md` |
| `Skill SHA-256` | `e587e06af27ec0b80f011ec574dc5e7c9d1538fd6b527ea62f5a2c5ba53d21d2`; personal installed copy matches via `verify-spec-kit-sync.py --require-personal` |
| `Immutable base` | repository `HEAD` `6a1aaf507fa1129d2453e1b727628c22db9c17f6`; frozen 21-file changed-scope manifest is [the snapshot manifest](spec189-static-review-snapshot-20260918.manifest), with canonical-entry SHA-256 `28dadc53d2b2a44cf77ce8c44e36627b8e41644c5ae88dd5b4d41685ae2ffb54`; no unrelated worktree file was included |
| `Diff scope` | exact relative paths and per-file hashes are in `spec189-static-review-snapshot-20260918.manifest`; the manifest excludes this audit record to avoid self-reference |
| `Queries/checks` | `rg` link/path check: exit 0, `missing links=0`; `git diff --check`: exit 0; `verify-spec-kit-sync.py --require-entrypoints --require-personal`: exit 0, `PASS: 11/11 local entrypoints`; `quick_validate.py`: exit 0, `Skill is valid!`; `audit_speckit_structure.py --strict`: exit 0, `Structural verdict: PASS`, 24 FR/6 SC/10 tasks |
| `Initial review` | `/root/spec185_review` returned `STATIC_FAIL` with P1 copied-digest exception and P2 incomplete trace; both are repaired and require re-review |
| `Re-review` | `/root/spec185_review` final read-only review returned `STATIC_PASS`; it verified the copied-digest rule, manifest path and 21/21 hashes, HEAD/skill identity, command results and AGENTS consistency. No build or experiment was run. |

## Current progress

The candidate and the real topology are substantially prepared, but Spec189 has
not reached native execution. The strongest observed sequence is:

```text
candidate fence → Controller/Authority/Provider ready
→ signed ACK offers → ACK closed → two-provider Selection committed
→ protected grant verified on both Providers → no provider execution marker
→ requester stream gap
```

The r21 requester and provider log hashes are:

| Log | SHA-256 |
| --- | --- |
| `requester-0.log` | `42c94ea5f83bf28a325382c762652407b7fd01d55e9cca6ea17e8f0ce1f4ecea` |
| `provider-0.log` | `761d393b7de74cece91a145509d9faf9cf2680b6a7e4bd3bc8630e363ec9bd2c` |
| `provider-1.log` | `cff5f5af426ef0db28c22f83e0d52744d950b072bb277c4649fa097d50f1a0d6` |

This proves ACK/Selection and grant verification only. It does not prove Repo
commit, placement-bound layer fetch, assembly, ONNX execution, hidden-state
handoff, terminal response, or cleanup.

## Findings and whether static review could have helped

| ID | Severity | Finding | Static/preflight value | Current state |
| --- | --- | --- | --- | --- |
| SA-01 | HIGH | `NativeEpochCoordinator` rebuilt role edges from the legacy plan and lost the authenticated V3 endpoint digest. | A source review comparing `roleSpecFor` with `roleSpecFromSelectionProjectionV3`, plus a C++ endpoint-preservation assertion, would have found this before r18. | Fixed by the coordinator role factory; a focused regression selector is still required. |
| SA-02 | HIGH | The batch had no stable observable exit between grant verification and first provider execution. r19/r21 therefore ended as a generic requester stream gap while provider logs contained no next-stage marker. | Static review of the five-lane gate and event contract should have required `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`, `ASSEMBLY_STARTED`, `RUNNER_READY`, `EXECUTION_COMPLETED` and `TERMINAL` markers before a MiniNDN retry. | Unfixed observability/diagnostic gap; exact runtime cause remains unobserved. |
| SA-03 | HIGH | The experiment policy originally used the application-root role prefix instead of the service-scoped role prefix. | Comparing generated policy text with the controller/provider permission contract would have caught r12 without a full run. | Fixed in the runner; add a policy-scope preflight and C++ negative. |
| SA-04 | HIGH | The operator credential required `trust-root-registry-v1.json` and its key closure, while the runner initially supplied only `authority-public.pem`. | Static inspection of `NativeProtectedGrantCredentials` and runner materialization could have caught r13 before MiniNDN. | Fixed in the runner; add a candidate-closure checker. |
| SA-05 | MEDIUM | Manually copied digests caused r01, r03, r11, r14, r16 and r20 preflight failures. | A dispatch/static gate should derive every expected digest from the frozen candidate manifest/build receipt instead of accepting repeated hand-copied values. | Still a process hazard; command path should be simplified. |
| SA-06 | MEDIUM | Dynamic KV graph shape and the fixed 5-second planning budget were discovered by runtime retries (r04/r05). | Candidate preflight can inspect ONNX state names/count and compute a graph-size budget before starting MiniNDN. | Graph and budget were corrected; checks are not yet mandatory gates. |
| SA-07 | MEDIUM | Stale 1.5-GB publication files caused r08 and reduced the 12-GB host margin. | A run-scoped publication directory, free-space reservation and pre-run residue check belong in the static/resource gate. | Cleanup was performed after the failed run; durable guard is still missing. |
| SA-08 | MEDIUM | Root Python lacked `onnx`/`numpy` in r17. | The candidate closure should verify the exact interpreter/module path before privileged launch. | Worked around with `PYTHONPATH`; not yet encoded as a hard preflight. |
| SA-09 | HIGH | The named Spec189 C++ full-path oracle is listed in the plan but no source/target is registered in `tests/wscript` or `examples/wscript`. | Static source/build review would prevent production binaries from being mistaken for an independent oracle. | B189-3 remains blocked until the oracle and link closure exist. |
| SA-10 | HIGH | T008's runner has no RSS/MemAvailable/swap/Repo/materialization sampler or resource guard; termination alone is not drain evidence. | Static runner review would catch the missing resource lane before r01-r21. | T008 remains `NOT_STARTED`; historical resource samples are not reused. |

The exact failure in r21 is **not** statically established. Static review can
prove that the required post-grant markers and failure correlation are absent;
only a rerun with those markers (or a direct C++ selector) can distinguish a
coordinator entry failure, dependency fetch failure, provider callback loss,
or requester stream transport timeout.

## Five-lane coverage at this checkpoint

| Lane | State | Evidence |
| --- | --- | --- |
| production entry/callers | `covered-partial` | real requester/provider reached `Runtime.open → User.prepare → PreparedModel.request`; full post-selection caller closure is not observed |
| implementation/wire | `covered-partial` | V3 projection fix and policy/credential fixes are in source; post-grant execution wire remains unobserved |
| test/harness/oracle | `gap` | no C++ full-path oracle, fetch/assembly/terminal assertion, or endpoint regression selector has run |
| build/source closure | `covered-partial` | affected DI targets built with the recorded receipt; Spec189 symbol/`nm` map and selector evidence are incomplete |
| migration/evidence | `covered-partial` | immutable candidate and r01-r21 raw logs are retained; repeat convergence and cleanup evidence are absent |

The four miss classes are:

- `static`: SA-02, SA-05, SA-06, SA-07 and SA-08 gates were not present before the runs;
- `compile/link`: the affected DI build passed, but no Spec189 C++ oracle target has run;
- `runtime/test`: ACK/Selection and grant verification are observed; execution and terminal lanes are not;
- `unobserved`: the first boundary after grant verification, Repo commit ownership, layer fetch, assembly, hidden-state handoff and drain.

## Required Spec189 corrections

1. B189-0 must freeze a candidate preflight that checks policy scope, operator
   registry closure, dynamic KV shape, planning budget, Python module closure,
   run-scoped disk reservation and all derived digests.
2. B189-3 must stop at named exits, in order:
   `GRANT_VERIFIED → EXECUTION_ENTERED → DEPENDENCY_FETCH → ASSEMBLY_STARTED
   → RUNNER_READY → EXECUTION_COMPLETED → TERMINAL`. A requester stream gap is
   only a transport symptom until the provider-side first missing marker is
   classified.
3. T006/T007 must include a C++ endpoint-preservation regression and provider
   execution-entry/failure oracle. A static review cannot substitute for the
   runtime handoff assertion.
4. T009 must use a candidate-derived digest lock and fail before MiniNDN when
   the policy, credential closure, interpreter or residue check is wrong.
5. T010 remains `NOT_STARTED` until a second run uses the same immutable tuple
   and reaches the same complete terminal/cleanup classification.
6. Register the C++ full-path oracle and its Waf source/link closure before
   calling B189-3 runtime evidence complete; production `DI_NativeRequester`
   and `di-native-provider` binaries are not independent oracle targets.
7. Implement T008's C++ counters and maintained sampler/guard before treating
   child termination or a `finally` cleanup path as resource evidence.

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: the post-grant execution markers and C++
endpoint regression selector are implemented and reviewed, then a fresh run
observes either the first provider-side failure boundary or the complete
fetch/assembly/execute/terminal sequence. No task may be marked `[x]` from the
current r21 evidence.

## Architecture and progress correction

### Progress reconciliation — 2026-09-18 13:53 -0500

文档修订已在 `2ee71559` 落地，T001 接线映射在 `42add32c` 关闭；当前为
1/7 任务完成（仅映射），不能解读为产品完成比例。T008 的 host supervisor
有未提交实现，仍未验收；本轮同步 tasks 与 batch 的 IN_PROGRESS 状态，
保留其余生产代码和测试改动，不将工作区草稿视为已通过。
追踪表区间写法使结构工具漏识别四个 FR，改为显式 ID 后 25/25 均识别。
结构检查 PASS、11/11 技能入口及个人副本同步 PASS、限定文档 diff 检查 PASS；
剩余 ID 不连续警告来自明确保留的合并历史，不重新编号。
没有新增构建、原生测试或 MiniNDN 结果；后续仍按 T008 → T003 → T005 →
T006/T007 → T009 执行。不得以持续增加 guard 功能代替模型 producer/consumer 接线；
T008 达到已定义的小 fixture 安全出口即转下一批，实际模型峰值归 T009。

**Date**: 2026-09-18
**Review baseline**: HEAD `76b26e2c` plus pre-existing working tree.
**Scope**: source-aware audit and Spec documentation repair; no product code/build/run.
Earlier r21 sections above are historical. This section supersedes their current-progress
and mandatory total-order recommendations; original logs and selector results remain intact.

### Findings and dispositions

| ID | Severity / confidence | Source or evidence | Disposition |
| --- | --- | --- | --- |
| RA-01 | HIGH / confirmed design conflict | old spec FR-002/SC-001, data-model layerRefs[2], plan AD-01 | 固定两个 prepare package 混淆原子准备与 ACK 后分区。T003 发布拓扑无关 layer/shared 材料；T005 规划两个范围；T006 组装最终模型。 |
| RA-02 | HIGH / confirmed wiring gap | Runtime.cpp:1335-1351,1465 onward; DI_NativeRequester.cpp:237-260; NativeCanonicalArtifactPublisher.cpp:631-654; NativeCanonicalOnnxAssembler.cpp:278-381 | Runtime 有可注入 Repo publisher，但实际 requester 没有设置它。fallback 发布整 graph/initializer，assembler 获取整 initializer 并持有 buffer/vector。必须接通同一 Repo producer/consumer，磁盘缓存本身不足；T003/T006 修复。不能误称模型在 request payload 中，发布发生在 prepare。 |
| RA-03 | HIGH / confirmed oracle conflict | examples/Spec189TwoProviderOracle.cpp:192-205; NativeProviderHandler.cpp prepareRunner/execute/input paths | checker 强制 dependency fetch 在 assembly 前且所有角色都有；实际流程存在不同顺序和首段直用请求输入。T007 按角色/attempt 的因果偏序，区分模型材料与 tensor fetch；不强迫生产代码迎合错误 checker。 |
| RA-04 | MEDIUM / confirmed fixture defect | tests/integration-tests/spec189-placement-oracle.t.cpp:103-106; NativeGrantVerifier.cpp canonicalNativeGrantName | synthetic name 拼接可有双斜线，cache selector 未走 grant parser；旧“canonical naming used”声明更正。T005 修正 CPU/backend/ABI fixture 并测试生产 ingress；原 selector 通过仅保留组件意义。 |
| RA-05 | HIGH / confirmed plan and harness gap | old T008 depends T007; Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py main lifecycle | 资源保护在完整模型执行之后，没有维护的连续 sampler/guard。T008 前置 full-model prepare/run，小 fixture 验证停止；T009 才补实际峰值，消除循环门禁。 |
| RA-06 | HIGH / confirmed acceptance weakness | old SC-003/SC-006 and T009 acceptance; data-model state sequence | “分类失败”不能作为完成；输出 digest 不是独立正确性证明。T009 同 live handle 两请求及新 run-id 独立成功重复，T007 增加固定输入独立 reference；cache reuse 不要求每请求重建 runner。 |
| RA-07 | MEDIUM / confirmed scope inflation | old ten-task linear graph, plan AD-06 | T002/T004→T003、T010→T009，共七活动任务；保留所有负例，删除独立领卡/重复构建要求。candidate 内容身份与 run-id 分离，只有受影响证据重验。 |

原生 Repo header 位于
`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp`：
已有 layer payload/冷热 receipt/事务实现与测试不能被说成“全无实现”；
问题是本实验实际 producer/consumer 未走完该路径。也不应重新修复已变更的旧 source-retention
问题而不先核对当前 releaseTransientSource/lease 所有权。

### Latest raw boundary

Raw root: `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r25/`。
`run-record.json` status=FAIL；本轮 sudo 只读核对 root-owned workload 日志：

| File | SHA-256 | Observed stages |
| --- | --- | --- |
| workload/provider-0.log | 9fc45427b8960350af3ed9950b27148607004bc55b7e3bd0224c97dde67791fa | GRANT_VERIFIED, EXECUTION_ENTERED, ASSEMBLY_STARTED |
| workload/provider-1.log | 2e8280b8c666792f3f31c4281cebe9ed3d152d3f48ea6dd339021883729d7851 | GRANT_VERIFIED, EXECUTION_ENTERED, DEPENDENCY_FETCH |
| workload/requester-0.log | e8ea14a23f5da538fbe16993ee15a7ae5b22eaee2ce2fecead02f3f1e7399b6c | 仍服务 di-canonical-initializer segmented Data |

Provider-0 日志请求该 initializer 的 segment 367；这证明旧数据路径被使用，
不证明完整下载结束、具体内存峰值或超时根因。
两 Provider 均无 RUNNER_READY / EXECUTION_COMPLETED stage；终态成功尚未证明。
r21/r23 “未见执行入口”不能继续当最新进度，r25 也不是当前脏工作区的 qualification。

### Retained evidence and execution order

保留 b189-build 的依赖/loader证据、b189-prepare 的 Repo 组件检查、
b189-placement 的 wire/handle/cache selector 结果，以及已有 C++ oracle 注册。
不重做未变工作；新 schema/consumer/授权变化只复测受影响闭包。

下一步 T001 短收敛真实接线 → T008 安全门 → T003 准备/发布/复用 →
T005 生产选择门 → T006/T007 范围组装与 handoff → T009 真实重复验收。
七活动任务仍未完整验收，不勾选任何任务。
每批保留逐任务静态门和批末组合审查/统一测试；不新增字段级行政任务。

### Review and verification

主代理使用 speckit-audit/speckit-code-design 和 CodeGraph 复核源码，
已授权的官方 review-agent 子会话只读核对。
初审引用 r21 的陈旧结论由主代理以 r25 原始日志纠正，不能机械接受审查结论。
冻结文档差异复审与文档检查结果追加于此。

Context Mode project health 通过，初始 active health 因 AGENTS 的过期
`specs/003-native-di-real-minindn/plan.md` 指针失败；本轮以仓库/source/raw evidence
为权威并修复 managed pointer。检索工具健康不代表产品状态。
设计记录只追加本轮条目，不提交该文件原有并行修改，不覆盖冻结 Design PDF/API。

Four miss classes: static=RA-01/02/03/04/05/06/07；
compile-link=no build this audit；
runtime-test=historical r25 FAIL retained；
unobserved=full-model output, peak, same-handle real reuse and drain.

**Closure**: DOCUMENTATION_STATIC_PASS; product remains PARTIAL.

### Final document validation

- 官方只读 review-agent 审完 v1 组合 diff，指出 current/target 边界、真实类型路径、
  oracle source/target 区分、固定 fetch/runner 计数和合并映射表述；修正后复审 v2
  返回 DOCUMENTATION_STATIC_PASS，无剩余控制性文档问题。
- Base: `76b26e2c`。v1 patch SHA-256:
  `4f7a2fd9c8ffd5b63046e968915bbd6432ccdc7d0f9b1904b12658de438404ce`；
  v2: `e453fc0a52a56712ecbe70858634aba48a39dc0590d8edc24f559ae183a40068`。
  原始冻结 patch 分别保留于 `.codex-tmp/spec189-plan-audit-review-v1/docs.patch`
  和 `spec189-plan-audit-review-v2/docs.patch`（后者同属 `.codex-tmp/`）。
  本段和 tasks 的验证结果句是审查返回后追加的结果记录，不更改受审契约。
- `verify-spec-kit-sync.py --require-entrypoints --require-personal`: PASS 11/11。
  定向文档检查：7 个活动 task ID、25 个 FR、相关链接/锚点和纠正的源码路径均 PASS；
  所有产品任务仍未勾选。`git diff --cached --check` PASS。
- Context Mode authority 已重建索引，project 与 active health 均通过；原始工具输出在
  `.codex-tmp/spec189-plan-audit-review-v2/{sync.log,context-index.log,context-active.json}`。
  AGENTS.md 是本机 `.git/info/exclude` 忽略的未跟踪文件，已修正指针但不强制纳入 Git；
  `.specify/feature.json` 原有变更保留不入本 checkpoint。
- 只提交 Spec 文档、failure-log 本轮追补和 Design 记录本轮 7 行；原有 Design 及
  源码/技能/实验脏文件不混入。未运行 C++ 构建、模型推理、MiniNDN、SIF 或 Tiger。
