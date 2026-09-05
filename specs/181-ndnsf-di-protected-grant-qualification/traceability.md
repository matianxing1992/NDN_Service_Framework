# Spec181 Traceability Matrix (revision 4)

Spec 181 从 Spec 180（修订 125 关闭）继承未完成的实现与资格认证工作。
本表是唯一的活动完成映射；Spec 180 的矩阵冻结为历史。

**边界（2026-09-05 所有者决定）**：
- 撤销子系统由另一分支开发（本 spec 不实现，`revocationSequence`
  固定为 1 的被动 wire 字段）；
- 独立权威网络服务端（生产形态）为延期项；功能切片内权威运行在
  请求方进程中，复用既有 `ServiceUser.publish_signed_app_data` 发布
  路径（main 的明文路径证明"无网络服务"可工作，本切片不新增网络
  角色与前缀）。集成条件见 spec.md Out of Scope。

## Requirement-to-task map

| Requirement | Owner tasks | 三层测试归属 | Closing evidence |
|---|---|---|---|
| FR-001 继承授权契约 | T001--T003, T006 | unit + integration（负例到达真实 verifier） | grant 往返证据 + Spec 180 编码回归（29 用例） |
| FR-002 进程内权威与既有发布路径 | T001（签发/发布/获取闭环） | unit（策略负例）+ integration（真实发布/获取往返） | `evidence/t001-python-provider-grant-current.md` |
| FR-003 Provider 双侧解包 | T001（Python）、T002（native） | unit + integration + parity 向量（T003） | `evidence/t001-python-provider-grant-current.md`、`evidence/t002-native-provider-grant-current.md`、`tests/fixtures/spec181/grant-vectors-v1.json` |
| FR-004 Y-N-E 真实变异 | T006（构造）+ T005（矩阵执行） | unit（三种变异）+ integration + MiniNDN | `evidence/t006-y-n-e-grant-mutation-current.md` + `evidence/t005-y-n-matrix-current.md` |
| FR-005 三层测试标准 | 全部任务 | 每个证据文件头部声明证据层 | tasks.md 标准章节 + 审计 |
| FR-006 本地资格认证 | T005（矩阵）、T008（资格） | MiniNDN 小模型 CPU 全矩阵 | `evidence/t005-y-n-matrix-current.md`、`evidence/local-qualification.md` |
| FR-007 收敛审计 | T007 | 审计 PASS + 四层分离 | `audit.md`、`evidence/post-implementation-audit.md` |
| FR-008 候选与 SIF | T009, T010 | unit（封印）+ integration（SIF replay） | `evidence/t009-candidate-seal-current.md`、`evidence/t010-exact-sif-replay-current.md` |
| FR-009 Tiger 一次提交 | T011 | MiniNDN 之后的一次真实提交 | `evidence/t011-tiger-submission-current.md` |
| FR-010 声称边界 | T012 | 终局语言审计 | `evidence/closure-record.md` |
| FR-011 就绪边界修复 | T004 | unit + integration（真实 controller 进程） | `evidence/t004-readiness-boundary-current.md` |
| FR-012 装配 parity | T003 | 固定向量双侧一致 | `evidence/t003-grant-parity-current.md` |
| FR-013 内容密钥真实消费 | T001（AEAD 暂存/解密/零化）、T002（native 同语义） | unit（派生/往返）+ integration（错误密钥/篡改密文在 AEAD 层拒绝）+ MiniNDN（T005 覆盖） | `evidence/t001-python-provider-grant-current.md`、`evidence/t002-native-provider-grant-current.md` |

## Source-owner status

| Path or path group | Owner | Status at Spec 181 start | Required evidence |
|---|---|---|---|
| `core/protected_artifacts.py`、`security/*` | T001, T006 | existing（Spec 180 提交 `d36438c2` + 2026-09-05 撤销清理：规范编码 + 进程内权威 + seam + 注册表，无撤销账本） | 签发/发布/获取/解包的生产连线；29 个编码测试作为回归基线 |
| `app_sdk/placement.py` grant seam、`sdk/placement.py` grant view | T001, T002 | existing（seam 已扩展，`AuthorityBackedGrantProvider` 就绪） | 真实资格路径使用非 `plaintext-v1` 纪元并通过 seam |
| `provider.py` 装配入口 | T001 | planned | 保护纪元下规范名获取 + 解包 + 租约 + 零化 |
| `cpp/ndnsf-di/NativeProviderHandler.cpp`、`ProtectedRuntime.{hpp,cpp}`、`_ndnsf.cpp` | T002, T003 | existing（绑定校验）| grant 精确名获取、权威校验、KeyChain 解包、parity |
| `ServiceUser.publish_signed_app_data` 发布路径 | T001 | existing（runner 目录发布已使用）| grant Data 经此路径发布并被 Provider 精确名获取 |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | T005, T006, T008 | existing（barriered runner + 语义判定已修复）| Y-N 全矩阵语义重跑 + Y-A/Y-B 资格 |
| `scripts/spec180_*`、`packaging/.../jobs/spec180/*` | T009--T012 | existing（Spec 180 工具链，路径沿用）| 候选封印（提交哈希）+ SIF + Tiger 终局 |
| `tests/fixtures/spec181/grant-vectors-v1.json` | T003 | planned | 正例与全部负例的双侧一致向量 |
| 撤销子系统（账本/网络服务/撤销校验） | **另一分支（所有者）** | deferred（本分支不实现） | 集成时插入撤销检查并解除 spec.md Out of Scope 延期标记 |
| 独立权威网络服务端（生产形态） | **操作者（生产部署前）** | deferred（本分支不实现） | 生产部署前拆分为独立服务并恢复网络服务端形态 |

## Evidence levels（每个证据文件头部必须声明其一）

- `implemented`：源码存在。
- `wired`：生产调用方到达。
- `executed`：真实路径完成。
- `measured`：注册指标/工件存在。
- `qualified`：全部用例 oracle 与身份门一致。

禁止从 `implemented`/`wired` 直接跳到 `qualified`；Spec 180 的
历史 PASS 一律是历史观察，不是本 spec 的证据。
