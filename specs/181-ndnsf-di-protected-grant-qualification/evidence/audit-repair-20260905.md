# Spec181 Audit Repair — 2026-09-05

**Layer**: implemented（源码核查）; executed（只读诊断）; 无资格或 measured 声明。
**Status**: BLOCK（审查与本轮定向修正完成；生产实现缺口仍开放）。
**Source identity**: `67194dc2` + 预存工作区变更（136 modified、639 untracked）；
Spec181 文档与本次拟修复的 Provider 文件在任务开始时无预存差异。

## Initial Tool Boundaries

- `SPEC181-AUDIT-COMMIT-PREFLIGHT-20260905`：第一次本地 checkpoint
  被仓库 hook 拦截（exit 1），原因是证据文档直接提及开发助手临时
  目录名。改用仓库已有的“忽略的工作区临时根目录 + run-id”写法；
  原始日志保持不变，未禁用 hook、未生成提交。

- `SPEC181-AUDIT-ACTIVE-CONTEXT-20260905`：
  `python3 scripts/context_mode_guard.py health --scope active --project-root .`
  exit 4，`managed agent-context file does not point to specs/181-ndnsf-di-protected-grant-qualification/plan.md`。
  project health exit 0；活动 checkpoint 改用仓库文档与源码。修复托管链接并重新索引后才重试 active health。
- `SPEC181-AUDIT-PROJECT-QUERY-20260905`：project query 缺少显式
  `--require`，guard exit 2：`project queries require explicit high-entropy --require identifiers`。
  未调用搜索，非协议结果；补齐项目锚点标识后重试。
  第二次 guard exit 2：`required identifier(s) absent from the query: ndn-service-framework`；
  query 文本与 `--require` 必须同时含该项目标识。两次均未执行检索。
- `SPEC181-AUDIT-STRUCTURE-20260905`：严格结构扫描 exit 1，16 条加粗
  任务行不符合解析语法，解析为 0 tasks / 0 stories。修正文档格式后重跑；
  此结果不表示源码测试失败。
- CodeGraph status 声称 up to date，但 explore 混入 `ignored workspace temporary root / compare-*`
  历史副本；以当前工作区精确路径核查，历史副本不作为生产事实。

## Inherited Failure Boundary

已检查 `ignored workspace temporary root / spec180-yolo-y-n-console-20260905-r42-i-only.log`：
仅历史 Y-N-I PASS；不闭合 Spec181。当前 Spec181 原始诊断在
`/tmp/spec181-y-n-run/yb39.log`，六次均记录
`CASE_RUNTIME_PROCESS_START_FAILED:control`；未产生受保护 Y-B 资格结果。
不能仅凭该日志将故障归因为 OOM 或 libndn-cxx 竞态。
后续定向检查日志保存在 `ignored workspace temporary root / spec181-audit-repair-20260905/`。

## Controlling Findings

`SPEC181-GRANT-REFERENCE-RED-20260905`：新增定向回归在原实现上
exit 1（1 failed / 32 deselected，0.57 s）：相同 request/core/model/epoch
但内容密钥不同、权威签名有效的 grant 能替换 Selection 封印的 grant。
首个失败边界为 Provider 的 grant 引用校验，断言
`ProtectedGrantRejected not raised`。原始日志：
`ignored workspace temporary root / spec181-audit-repair-20260905/grant-reference-red.log`。
修复必须比较已解析 grant 的摘要与 `GrantBindingV1.grant_digest`，
在任何密文暂存或明文租约创建前拒绝替换。该回归是 unit 层。

native protected runtime 仍失败关闭；grant parity 不等于装配 parity；
Y-N-E 的进程内 verifier probe 不等于 Provider 生产链变异；
T001/T002/T003/T004/T006 的完成标记超过现有证据层；
计划中的审计依赖环与权威归属冲突须先修复。后续精确发现与关闭检查
以 `../audit.md` 为准。本记录不授权 MiniNDN 全矩阵、SIF 或 Tiger。

## Focused Repair Result

已关闭的 grant 引用替换漏洞见
[专项修复](grant-reference-repair-20260905.md)，提交 `ff7b5c3b`。
同一环境下 RED 1 failed；GREEN **34 passed in 0.80s**。
只执行 Provider grant 与发布 seam 的 focused unit 回归，无完整资格运行。

## Documentation Repair

revision 5 修正权威归属、撤销延期、七子用例、审计依赖顺序、装配 parity
映射和 5 个过早勾选任务。12 个 T 任务保留，4 个历史 R 项独立列示。
各旧证据增加当前范围声明，Spec180 冻结树保持原样。
T007 当前 BLOCK，完整发现及其 owner 在 `../audit.md`。

文档编辑曾有两个补丁预检拒绝（找不到精确旧行、同一补丁重复目标）；
均未写入文件，改用读取精确旧内容后的一次 Update。第一轮修订扫描
检测到 SC 加粗边界变化导致计数为 0，已恢复独立 SC 标记再核查。
这些均为编辑/结构边界，不是模型或协议结果。

## Final Documentation Checks

- 严格结构扫描 PASS：14 FR、6 SC、4 user stories、12 T tasks；完整
  验收尚无勾选项。FR/SC 引用与相对 Markdown 链接检查通过，活动
  spec/plan/tasks/traceability/audit 的节标题保持英文。
- prerequisite selector 正确解析当前 181 目录，exit 0。
- 活动 plan 托管链接已同步；重新索引后 project/active health 均
  exit 0，活动 spec/plan/tasks 的 file-backed sources 均 fresh。
  project anchor 与活动 tasks checkpoint 使用分离的 relevance 检索，
  guard 通过；没有使用 timeline 自动记忆作为进度权威。
- 所有定向改动通过 `git diff --check`。预存修改独立保留；failure
  index 仅本轮新增段落进入文档 checkpoint，旧修改不混入。

检查产物保存在忽略的工作区临时根目录下
`spec181-audit-repair-20260905/`：`structure-final.log`、
`context-index-final.log`、`context-health-final.json` 与 grant red/green 日志。
这些检查关闭文档/检索缺口，不改变当前资格裁决 BLOCK。
