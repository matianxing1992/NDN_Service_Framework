# Spec181 Traceability Matrix (revision 3)

Spec 181 从 Spec 180（修订 125 关闭）继承未完成的实现与资格认证工作。
本表是唯一的活动完成映射；Spec 180 的矩阵冻结为历史。

**撤销子系统边界（2026-09-05 所有者决定）**：账本、网络撤销服务与
grant 撤销校验由所有者在另一台机器的另一分支开发；本 spec 不含撤销
任务，`revocationSequence` 保持固定为 1 的被动 wire 字段（集成条件见
spec.md Out of Scope 延期项）。

## Requirement-to-task map

| Requirement | Owner tasks | 三层测试归属 | Closing evidence |
|---|---|---|---|
| FR-001 继承授权契约 | T001--T004, T007 | unit + integration（负例到达真实 verifier） | grant 往返证据 + Spec 180 编码回归（29 用例） |
| FR-002 权威网络服务端 | T001（grant 服务；撤销服务属另一分支） | unit（策略负例）+ integration（跨进程真实往返） | `evidence/t001-authority-service-current.md` |
| FR-003 Provider 双侧解包 | T002（Python）、T003（native） | unit + integration + parity 向量（T004） | `evidence/t002-python-provider-grant-current.md`、`evidence/t003-native-provider-grant-current.md`、`tests/fixtures/spec181/grant-vectors-v1.json` |
| FR-004 Y-N-E 真实变异 | T007（构造）+ T006（矩阵执行） | unit（三种变异）+ integration + MiniNDN | `evidence/t007-y-n-e-grant-mutation-current.md` + `evidence/t006-y-n-matrix-current.md` |
| FR-005 三层测试标准 | 全部任务 | 每个证据文件头部声明证据层 | tasks.md 标准章节 + 审计 |
| FR-006 本地资格认证 | T006（矩阵）、T009（资格） | MiniNDN 小模型 CPU 全矩阵 | `evidence/t006-y-n-matrix-current.md`、`evidence/local-qualification.md` |
| FR-007 收敛审计 | T008 | 审计 PASS + 四层分离 | `audit.md`、`evidence/post-implementation-audit.md` |
| FR-008 候选与 SIF | T010, T011 | unit（封印）+ integration（SIF replay） | `evidence/t010-candidate-seal-current.md`、`evidence/t011-exact-sif-replay-current.md` |
| FR-009 Tiger 一次提交 | T012 | MiniNDN 之后的一次真实提交 | `evidence/t012-tiger-submission-current.md` |
| FR-010 声称边界 | T013 | 终局语言审计 | `evidence/closure-record.md` |
| FR-011 就绪边界修复 | T005 | unit + integration（真实 controller 进程） | `evidence/t005-readiness-boundary-current.md` |
| FR-012 装配 parity | T004 | 固定向量双侧一致 | `evidence/t004-grant-parity-current.md` |

## Source-owner status

| Path or path group | Owner | Status at Spec 181 start | Required evidence |
|---|---|---|---|
| `core/protected_artifacts.py`、`security/*` | T001, T002, T007 | existing（Spec 180 提交 `d36438c2` + 2026-09-05 撤销清理：规范编码 + 真实权威 + seam + 注册表，无撤销账本） | 网络服务端与 Provider 双侧解包的生产连线；29 个编码测试作为回归基线 |
| `app_sdk/placement.py` grant seam、`sdk/placement.py` grant view | T002, T003 | existing（seam 已扩展，`AuthorityBackedGrantProvider` 就绪） | 真实资格路径使用非 `plaintext-v1` 纪元并通过 seam |
| `provider.py` 装配入口 | T002 | planned | 保护纪元下规范名获取 + 解包 + 租约 + 零化 |
| `cpp/ndnsf-di/NativeProviderHandler.cpp`、`ProtectedRuntime.{hpp,cpp}`、`_ndnsf.cpp` | T003, T004 | existing（绑定校验）| grant 精确名获取、权威校验、KeyChain 解包、parity |
| controller 维护入口（KEY-GRANT 前缀） | T001 | planned | 权威签发 + 服务 grant Data |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | T006, T007, T009 | existing（barriered runner + 语义判定已修复）| Y-N 全矩阵语义重跑 + Y-A/Y-B 资格 |
| `scripts/spec180_*`、`packaging/.../jobs/spec180/*` | T010--T013 | existing（Spec 180 工具链，路径沿用）| 候选封印（提交哈希）+ SIF + Tiger 终局 |
| `tests/fixtures/spec181/grant-vectors-v1.json` | T004 | planned | 正例与全部负例的双侧一致向量 |
| 撤销子系统（账本/撤销服务/撤销校验） | **另一分支（所有者）** | deferred（本分支不实现） | 集成时插入撤销检查并解除 spec.md Out of Scope 延期标记 |

## Evidence levels（每个证据文件头部必须声明其一）

- `implemented`：源码存在。
- `wired`：生产调用方到达。
- `executed`：真实路径完成。
- `measured`：注册指标/工件存在。
- `qualified`：全部用例 oracle 与身份门一致。

禁止从 `implemented`/`wired` 直接跳到 `qualified`；Spec 180 的
历史 PASS 一律是历史观察，不是本 spec 的证据。
