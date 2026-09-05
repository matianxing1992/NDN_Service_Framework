# Spec181 Traceability Matrix (revision 2)

Spec 181 从 Spec 180（修订 125 关闭）继承未完成的实现与资格认证工作。
本表是唯一的活动完成映射；Spec 180 的矩阵冻结为历史。

## Requirement-to-task map

| Requirement | Owner tasks | 三层测试归属 | Closing evidence |
|---|---|---|---|
| FR-001 继承授权契约 | T001--T005, T008 | unit + integration（负例到达真实 verifier） | grant 往返证据 + Spec 180 编码回归（29 用例） |
| FR-002 权威网络服务端 | T001（grant 服务）、T002（撤销服务） | unit（策略负例）+ integration（跨进程真实往返） | `evidence/t001-authority-service-current.md`、`evidence/t002-revocation-service-current.md` |
| FR-003 Provider 双侧解包 | T003（Python）、T004（native） | unit + integration + parity 向量（T005） | `evidence/t003-python-provider-grant-current.md`、`evidence/t004-native-provider-grant-current.md`、`tests/fixtures/spec181/grant-vectors-v1.json` |
| FR-004 Y-N-E 真实变异 | T008（构造）+ T007（矩阵执行） | unit（四种变异）+ integration + MiniNDN | `evidence/t008-y-n-e-grant-mutation-current.md` + `evidence/t007-y-n-matrix-current.md` |
| FR-005 三层测试标准 | 全部任务 | 每个证据文件头部声明证据层 | tasks.md 标准章节 + 审计 |
| FR-006 本地资格认证 | T007（矩阵）、T010（资格） | MiniNDN 小模型 CPU 全矩阵 | `evidence/t007-y-n-matrix-current.md`、`evidence/local-qualification.md` |
| FR-007 收敛审计 | T009 | 审计 PASS + 四层分离 | `audit.md`、`evidence/post-implementation-audit.md` |
| FR-008 候选与 SIF | T011, T012 | unit（封印）+ integration（SIF replay） | `evidence/t011-candidate-seal-current.md`、`evidence/t012-exact-sif-replay-current.md` |
| FR-009 Tiger 一次提交 | T013 | MiniNDN 之后的一次真实提交 | `evidence/t013-tiger-submission-current.md` |
| FR-010 声称边界 | T014 | 终局语言审计 | `evidence/closure-record.md` |
| FR-011 就绪边界修复 | T006 | unit + integration（真实 controller 进程） | `evidence/t006-readiness-boundary-current.md` |
| FR-012 装配 parity | T005 | 固定向量双侧一致 | `evidence/t005-grant-parity-current.md` |

## Source-owner status

| Path or path group | Owner | Status at Spec 181 start | Required evidence |
|---|---|---|---|
| `core/protected_artifacts.py`、`security/*` | T001--T003, T008 | existing（Spec 180 提交 `d36438c2`：规范编码 + 真实权威 + seam + 注册表） | 网络服务端与 Provider 双侧解包的生产连线；29 个编码测试作为回归基线 |
| `app_sdk/placement.py` grant seam、`sdk/placement.py` grant view | T003, T004 | existing（seam 已扩展，`AuthorityBackedGrantProvider` 就绪） | 真实资格路径使用非 `plaintext-v1` 纪元并通过 seam |
| `provider.py` 装配入口 | T003 | planned | 保护纪元下规范名获取 + 解包 + 租约 + 零化 |
| `cpp/ndnsf-di/NativeProviderHandler.cpp`、`ProtectedRuntime.{hpp,cpp}`、`_ndnsf.cpp` | T004, T005 | existing（绑定校验）| grant 精确名获取、权威校验、KeyChain 解包、parity |
| controller 维护入口（KEY-GRANT / REVOCATION 前缀） | T001, T002 | planned | 权威签发 + 服务 + 撤销 Data |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | T007, T008, T010 | existing（barriered runner + 语义判定已修复）| Y-N 全矩阵语义重跑 + Y-A/Y-B 资格 |
| `scripts/spec180_*`、`packaging/.../jobs/spec180/*` | T011--T014 | existing（Spec 180 工具链，路径沿用）| 候选封印（提交哈希）+ SIF + Tiger 终局 |
| `tests/fixtures/spec181/grant-vectors-v1.json` | T005 | planned | 正例与全部负例的双侧一致向量 |

## Evidence levels（每个证据文件头部必须声明其一）

- `implemented`：源码存在。
- `wired`：生产调用方到达。
- `executed`：真实路径完成。
- `measured`：注册指标/工件存在。
- `qualified`：全部用例 oracle 与身份门一致。

禁止从 `implemented`/`wired` 直接跳到 `qualified`；Spec 180 的
历史 PASS 一律是历史观察，不是本 spec 的证据。
