# R003 — 证据失效声明完整性审计（Evidence Banner Audit）

> **Current scope correction (revision 5, 2026-09-05)**: 历史 R003 清单保留；其中仍有“无层声明”项，原 PASS 不能代表逐文件层声明要求全部满足。当前 Spec181 的限定层声明已补齐；Spec180 冻结文件不在本輪修改。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（横幅修正落地）+ executed（对 spec 180 冻结树的
只读逐文件审计）;无 measured 声明。

Date: 2026-09-05.
Source HEAD: `286a0098` plus the recorded spec181 Phase-0 worktree changes.
Audit input: agent report `a8df5b433582bb092.output`（378 个证据文件逐行
清单，2026-09-05 生成）;横幅落地点按 spec181 R003 要求「不加内容、只加
失效声明」逐一核验。

## 范围与冻结树状态

| 目录 | evidence 文件数 | 树状态 | R003 要求适用 |
|---|---|---|---|
| `specs/180-ack-driven-cross-model-qualification/`（含根级 `audit.md`） | 105 | **FROZEN** at spec rev 125（2026-09-05 关闭声明） | 是（本审计核心） |
| `specs/181-ndnsf-di-protected-grant-qualification/` | 0 | PLANNED（无证据文件） | 是（新建证据必须带头部层声明） |
| `specs/175-ndnsf-di-streamed-invocation/` | 169（含 archive/） | FROZEN（rev 105/110 确认） | 旁检 |
| `specs/170-reusable-layer-artifacts/` | 104 | Active architecture correction（未冻结） | 旁检 |

180 冻结声明出自 spec.md rev 125：「This document and its evidence stay
frozen; no new work opens here」。关闭当日（2026-09-05）最后一批记录
（t008-protected-grant-gap、t011-negative-verdict-repair、
t011-y-b-live-r22、t011-y-n-live-r35…r42-i-only）作为关闭记录并入冻结集。
`history/revision123-20260905/` 为修订 123 五份核心文档的冻结快照（非
证据文件，未审计）。

## 审计规则

- 疑似缺横幅（仅 180）规则：状态字段含 PASS/通过 且 spec 后续修订
  （最新 125）超过文件标题声明修订号 且无失效横幅 → Y（确认缺失）;
  标题无修订号 → ?（需人工判定）。修订号只取标题;文件名中 r22/r35 等
  为运行序号，非 spec 修订，不计。
- 层声明规则：只看每文件前 15 行;「有?散文」= 词出现在前 15 行但非
  声明语境（非括号/反引号/紧邻 layer 字样的声明式）。声明式语境仅 180
  的 `post-implementation-audit.md`（`(implemented)/(executed)` 分层标题）
  与 `t013-qwen-entrypoint-current-20260903.md`
  （`implemented, executed, qualified`）出现。
- 170/175 全部证据文件头部无任何声明式层声明（仅零散散文命中）;不在
  本任务补层声明范围（180 冻结树禁止改内容;该要求向前适用于 181 全部
  新建证据文件，本文件及全部 R0 evidence 均带头部层声明）。

## 确认缺横幅 → 已补（5 个）

以下 5 个文件声明 PASS 但被后续修订（rev 123 起）失效，且原无横幅;
本审计已逐一在标题后补 `INVALIDATED — DO NOT REUSE.` 横幅，仅加失效
声明、不改正文。

| 文件 | 标题修订 | 声称 | 横幅落点（2026-09-05） | 失效原因与当前源替代 |
|---|---|---|---|---|
| `t011-yolo-ya-terminal-current-20260904.md` | Revision 112 | `status=PASS case=Y-A`（首个 terminal） | ✅ | rev 123 变更 Controller PUBPARAMS readiness/运行时源;替代 `t011-y-a-live-current-20260904.md` |
| `t011-yolo-yb-terminal-current-20260904.md` | Revision 115 | `status=PASS case=Y-B` | ✅ | 同上;替代 `t011-y-b-live-current-20260905-r22.md` |
| `t011-yolo-yn-control-current-20260904.md` | Revision 116 | `status=PASS case=Y-N` | ✅ | 同上;替代 `t011-y-n-live-current-20260905-r35…r42` 系列 |
| `post-implementation-audit.md` | Revision 116（T014） | `CONDITIONAL PASS` + Y-A×2 PASS 表 | ✅ | rev 123–125 重做 Controller PUBPARAMS/运行时源、native evidence 与 grant 子系统后按 reassignment 关闭;替代 `audit-revision123-design-code-conformance-20260904.md` + tasks.md rev-124/125 closure 注记 |
| `s0-native-closure-current-20260903.md` | 2026-09-03（无修订号） | `Verdict: PASS` | ✅ | 早于 rev 123 源变更，同管道阶段 s1（rev 112）已挂横幅;替代 rev-123 时代 native 修复证据组（audit-revision123、provider-boundary-repair、t011-native-boundary-repair 等） |

## 人工判定：疑似但判定不补横幅（3 个）

字面规则无法套用（标题无修订号），逐文件人工核验后判定为**不需要**
横幅，理由记录如下：

| 文件 | 声称 | 判定 | 理由 |
|---|---|---|---|
| `t001-contract-gate-current-20260902.md` | `Status: PASS` | 不补 | PASS 声称明确限定 scope（T001 实现门，非资格认证）;无 concrete superseding change 指向该 gate 声称 |
| `t003-candidate-identity-current-20260902.md` | `Status: PASS` | 不补 | 同上（T003 实现门） |
| `t011-y-a-live-current-20260904.md` | `status=PASS case=Y-A` | 不补 | 文件头自证 current-source（mtime 晚于 rev-123 修复证据）;rev 123 fixes 之后的 Y-A wiring 记录，非失效 |

## 组级备注（不逐文件加横幅，统一记录声明）

- **`audit-iteration42..92-*.md`（约 50 个滚动审计快照，2026-09-02/03）**:
  快照自带「CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION
  BLOCKED」或测试计数 PASS，均无修订号;iteration46/78 已有 superseded
  横幅;其余 47 个 flagged `?`。判定：整组为同一滚动审计序列的中间
  快照，自证不构成资格 PASS，且 spec180 现按 reassignment 关闭——
  逐文件横幅会产生 ~50 条同义噪声。本组声明为唯一记录;任何引用方
  不得以 iteration 快照充当新的资格门。冻结树内不再改。
- **`t011-y-b-live-current-20260905-r22` / `t011-y-n-live-current-20260905-r35…r42`（09-05 运行）**:
  当前源（rev 123 之后）运行，不算缺横幅。但 audit.md 09-05 更正
  （`t011-negative-verdict-repair-20260905.md`，已挂 obsolete）可能影响
  其 Y-N 子用例 PASS 含义——引用这些记录进入新 Y-N 门时须先复核
  子用例语义与 t011-negative-verdict-repair 的关联。

## 180 完整清单（105 个：层声明 | 横幅 | 疑似缺 | 理由）

```
180-ack-driven-cross-model-qualification/audit.md | 有?散文:wired | 有:invalidated | N | 根级审计,含失效字样
180-ack-driven-cross-model-qualification/evidence/audit-iteration42-current-20260902.md | 无声明 | 无 | ? | 滚动快照含PASS,无修订号（组级声明，见上）
…（audit-iteration43…audit-iteration91 同型：无声明 | 无 | ?）…
180-ack-driven-cross-model-qualification/evidence/audit-iteration46-current-20260903.md | 无声明 | 有:superseded | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/audit-iteration78-current-20260903.md | 无声明 | 有:superseded | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/audit-iteration92-execution-readiness-20260903.md | 有?散文:wired | 无 | ? | 同上（组级声明）
180-ack-driven-cross-model-qualification/evidence/audit-revision123-design-code-conformance-20260904.md | 无声明 | 有:invalidated | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/candidate-input-integrity-20260903.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/implementation-recovery-20260903.md | 有?散文:wired | 有:obsolete | N | 已有横幅(rev102)
180-ack-driven-cross-model-qualification/evidence/post-implementation-audit.md | 有:implemented,executed | 有:INVALIDATED(本次补) | Y→N | 见上表
180-ack-driven-cross-model-qualification/evidence/provider-boundary-repair-20260904.md | 无声明 | 无 | N | 09-04当前源限定修复PASS,非全T014,rev123之后
180-ack-driven-cross-model-qualification/evidence/recovery-plan-20260903.md | 有?散文:wired | 无 | N | PASS为散文/计划语,无状态字段
180-ack-driven-cross-model-qualification/evidence/revision103-delay-diagnosis-20260903.md | 无声明 | 有:obsolete | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/revision104-delay-diagnosis-20260903.md | 无声明 | 有:obsolete | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/revision105-scope-order-correction-20260903.md | 无声明 | 有:invalidated | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/revision107-input-status-20260903.md | 无声明 | 无 | N | PASS仅出现在测试名,无状态PASS
180-ack-driven-cross-model-qualification/evidence/revision108-delay-diagnosis-20260903.md | 无声明 | 无 | N | PASS为散文,无状态PASS
180-ack-driven-cross-model-qualification/evidence/revision109-native-closure-diagnosis-20260903.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/revision110-delay-and-gate-order-20260903.md | 无声明 | 无 | N | PASS为计划目标,无状态PASS
180-ack-driven-cross-model-qualification/evidence/revision112-tiger-mvp-scope-audit-20260903.md | 无声明 | 无 | N | PASS为计划目标,无状态PASS
180-ack-driven-cross-model-qualification/evidence/s0-native-closure-current-20260903.md | 无声明 | 有:INVALIDATED(本次补) | Y→N | 见上表
180-ack-driven-cross-model-qualification/evidence/s1-candidate-seal-current-20260904.md | 无声明 | 有:DO NOT REUSE | N | 已有横幅(rev112)
180-ack-driven-cross-model-qualification/evidence/t001-contract-gate-current-20260902.md | 无声明 | 无 | ? | 人工判定不补（gate-scoped）
180-ack-driven-cross-model-qualification/evidence/t002-generic-request-current-20260902.md | 无声明 | 有:Superseded | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/t002-input-publication-audit-current-20260902.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t002-input-publication-implementation-current-20260902.md | 有?散文:implemented | 无 | N | 仅构建输出PASS,无状态字段
180-ack-driven-cross-model-qualification/evidence/t003-candidate-identity-current-20260902.md | 无声明 | 无 | ? | 人工判定不补（gate-scoped）
180-ack-driven-cross-model-qualification/evidence/t004-yolo-export-current-20260902.md | 无声明 | 有:invalidated | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/t005-yolo-adapter-current-20260902.md | 有?散文:implemented | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t006-ack-planning-current-20260902.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t006-provider-offer-verifier-current-20260903.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t007-role-kind-current-20260902.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t008-protected-grant-gap-20260905.md | 无声明 | 无 | N | Status=BLOCK,PASS为散文
180-ack-driven-cross-model-qualification/evidence/t009-yolo-application-current-20260902.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t010-yolo-security-current-20260902.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t011-native-boundary-repair-20260904.md | 无声明 | 无 | N | Rev123修复证据自身,PASS为编译散文
180-ack-driven-cross-model-qualification/evidence/t011-negative-verdict-repair-20260905.md | 无声明 | 有:obsolete | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/t011-y-a-live-current-20260904.md | 有?散文:executed | 无 | ? | 人工判定不补（current-source）
180-ack-driven-cross-model-qualification/evidence/t011-y-b-live-current-20260905-r22.md | 无声明 | 无 | N | 09-05当前源运行,rev123之后(r22为运行序号)
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r35.md | 无声明 | 无 | N | 同上(r35运行序号)
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r36.md | 无声明 | 无 | N | 同上
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r37-p-only-preflight.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r38-p-only.md | 无声明 | 无 | N | 09-05当前源,rev123之后
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r39.md | 无声明 | 无 | N | 同上
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r40-path-preflight.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r41-extension-check.md | 无声明 | 无 | N | 09-05当前源,rev123之后
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-extension-build-cwd.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md | 无声明 | 无 | N | 09-05当前源,rev123之后
180-ack-driven-cross-model-qualification/evidence/t011-yolo-ya-terminal-current-20260904.md | 无声明 | 有:INVALIDATED(本次补) | Y→N | 见上表
180-ack-driven-cross-model-qualification/evidence/t011-yolo-yb-terminal-current-20260904.md | 有?散文:executed | 有:INVALIDATED(本次补) | Y→N | 见上表
180-ack-driven-cross-model-qualification/evidence/t011-yolo-yn-control-current-20260904.md | 无声明 | 有:INVALIDATED(本次补) | Y→N | 见上表
180-ack-driven-cross-model-qualification/evidence/t012-qwen-reference-current-20260902.md | 无声明 | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t013-controller-pubparams-readiness-current-20260904.md | 无声明 | 无 | N | rev123修复证据自身,当前源
180-ack-driven-cross-model-qualification/evidence/t013-native-evidence-repair-20260904.md | 无声明 | 无 | N | 09-04当前源修复,限定范围
180-ack-driven-cross-model-qualification/evidence/t013-numerical-repair-20260904.md | 无声明 | 无 | N | 同上
180-ack-driven-cross-model-qualification/evidence/t013-qwen-entrypoint-current-20260903.md | 有:implemented,executed,qualified | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t013-release-workflow-current-20260902.md | 有?散文:implemented | 无 | N | 无PASS/通过
180-ack-driven-cross-model-qualification/evidence/t013-supervision-repair-20260904.md | 有?散文:implemented,wired | 无 | N | 09-04当前源修复,限定范围
180-ack-driven-cross-model-qualification/evidence/t014-tiger-path-audit-20260904.md | 无声明 | 有:invalidated | N | 已有横幅
180-ack-driven-cross-model-qualification/evidence/t016-exact-sif-yb-replay-current-20260904.md | 有?散文:executed | 有:DO NOT REUSE | N | 已有横幅(rev121,已知)
```

注：上表省略 audit-iteration 系列的中间行，完整逐行记录在审计 agent
report（`a8df5b433582bb092.output`）。被省略行的分类如下——
`无声明 | 无 | ?`（滚动快照含 PASS/CONDITIONAL PASS，无修订号）：
iteration43/44/45/47/49/50/52/53/54/55/56/58/59/60/61/62/63/64/67/
68/69/70/71/72/73/74/75/76/77/79/80/81/82/83/84/85/86/87/88/89/90/
91;`无声明 | 无 | N`（无状态 PASS）：iteration65/66;`有?散文:
implemented | 无 | N`（仅测试计数）：iteration48;`有?散文:wired | 无 | ?`：
iteration57/92。全部归入组级声明，不逐文件加横幅。180 中
横幅已存在的文件共 14 个（audit.md、audit-iteration46/78、
audit-revision123、implementation-recovery、revision103/104/105、
s1-candidate-seal、t002-generic-request、t004-yolo-export、
t011-negative-verdict-repair、t014-tiger-path-audit、
t016-exact-sif-yb-replay），与任务所述「t016 与 s1 已有横幅」一致;
本次新增 5 个后横幅覆盖全部 Y 类文件。

## 层声明结论（本任务向前要求）

180/181 之外的 170/175 证据文件头部无声明式层标注（170 未冻结、
175 冻结，均不在本任务改内容范围）;180 冻结树中仅 2 个文件带声明式
层标注。**181 自本文件起：每个新建 evidence 文件头部必须声明
implemented/wired/executed/measured 层**（R001/R002/R004 evidence 与本
文件均已遵守）。

## Verdict

PASS（R003 范围）—— 5 个确认缺横幅文件已补失效声明（不加内容）;
3 个人工判定不补;2 组组级备注记录;完整清单 + 每文件层声明入本文件。
