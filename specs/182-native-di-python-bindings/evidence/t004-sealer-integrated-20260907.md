# T004 Sealer Complete Wire Integration

## Status

2026-09-07 / PARTIAL。已删除七字段 encode 与硬编码 CPU/adapter version 的 project；
新增必填 assembly 与 execution/dataflow/device 契约，core/final 摘要使用既有 canonical JSON。
上游 planner 真实 metadata、完整 generation/device/rank oracle 与 requester 主链仍未闭合，
T004-A 保持 PARTIAL；不把本轮定向 PASS 当作 T016 或所有模型资格验收。

## Implementation and Review

- sealCore 冻结每个角色的完整 assembly、request contract 与 generation contract，
  精确核对 role/artifact/manifest/graph/epoch/adapter，复用生产 assembly parser 拒绝无效契约。
- core identity 使用 PlacementPlanCoreV3 unsigned fields；final identity 使用
  core/grants/securityPolicySnapshotDigest，grant tuples 排序。validate 重算这些摘要，
  拒绝已封印字段、shape 类型和 grant digest 被替换。
- project 的新增必填 NativeRoleProjectionInputs 由执行 owner 提供；assembly/expiry/
  request/generation 来自 core。encode 直接调用完整 nativeSelectionProjectionV3ToJson，
  返回前执行既有生产 parser 检查。未添加默认 CPU 或临时补字段兼容入口。
- 新内部 detail/NativeSelectionJsonValues.hpp 复用同一 role/dependency/generation JSON
  表达式；不安装第三方 JSON 类型到公开 SDK。tests/wscript 的显式 native source closure
  同步登记 NativeSelectionJson.cpp，避免独立 consumer 缺符号。
- 旧测试中只检查七字段片段的断言已替换为生产 parser 往返和真实 Python SDK oracle；
  保留 preparation→sealCore→grantView→acquire 无中途补字段、外国工件、精确覆盖等检查。

## Independent Oracle

author-sealer-python-oracle.py 直接导入现有 SDK 的 PlacementPlanCoreV3、RoleAssemblySpec、
GrantBindingV1 和 PlanSealerV3。它只在离线 authoring 时运行，不进入 native runtime。
固定案例覆盖完整 CPU pipeline assembly（含整数和符号 shape）及 protected grant。
原生 core/final 摘要与 oracle 完全相同；未声称完整 projection wire 与所有 Python 场景已等价。

- SDK placement.py SHA-256：75c32038cb33e7ea3a59e3c3692adb3ca48b47638b0d6ef433fda9309b1fff9f。
- sealer-python-oracle.json SHA-256：8534ca7733b5bd5ad511274eb405b1fd00432a640f2705abc377d74e303006d5。

## Validation

原始 `.codex-tmp/spec182-t004-sealer-r1/` 保留 configure.log、build.log、focused.log 与设计检查。
公开 core/input DTO 布局和 project 签名改变，使用新 build 目录，system g++/binutils、
Boost 1.71、既定 NAC/SVS/ONNX/Rust 依赖闭包。`waf ... build --targets=unit-tests -j4 -v`
exit 0，289.48s。构建期间短 vmstat 后续样本未见持续换页；不是全程峰值或速度对照。

新 unit-tests：`--run_test=Spec182PlanSealer,Spec182NativePlanning,Spec182Preparation,Spec182CanonicalJson,NativeV3*`
配合 detailed report，exit 0，62/62 cases、2358/2358 assertions PASS，真实 SDK 摘要 case 已执行。
设计 validator 和 git diff --check PASS。未运行全量回归、网络集成、SIF 或 Tiger。
计划更新后已刷新 Context Mode authority index；helper 最终 exit 5，project/active health
均为 NO_REAL_SESSION_EVENTS。保留 context-index.log 与 health JSON，使用仓库权威状态继续，
不把检索统计或索引刷新误称为 host capture 验收。

## Next Boundary

NativeSplitCandidate 与已认证 offer/preparation 的实际字段仍不足以自动提供完整 assembly/
device/residency 证明，T003-C/T008 仍需修复；本轮 fixture 提供的测试契约不是生产数据来源。
在真实 owner 构造这些输入后完成 requester 接线，再做完整模型/生成/设备矩阵与正式验收。
