# Spec181 Traceability Matrix (revision 7)

Spec 181 从 Spec 180（修订 125 关闭）继承未完成的实现与资格认证工作。
本表是活动完成映射；当前 Status: IN_PROGRESS（T007 audit PASS），完整裁决见 audit.md。
Spec 180 的矩阵冻结为历史，未来证据路径不表示文件已存在或任务完成。

**Scope update (2026-09-06)**：本机负责开发、本地验证与版本交付；
T010/T011 移交实验机器，不计完成。本机活动 10 项中 6 项完成。
原要求与外部 owner/验收的去向见 [handoff contract](handoff-contract.md)。
Git 合并留到当前开发完成后另行讨论。

**边界（2026-09-05 所有者决定）**：
- 撤销子系统由另一分支开发（本 spec 不实现，`revocationSequence`
  固定为 1 的被动 wire 字段）；
- 独立权威网络服务端（生产形态）为延期项；功能切片内权威运行在
  请求方进程中，复用既有 `ServiceUser.publish_signed_app_data` 发布
  路径（main 的明文路径证明"无网络服务"可工作，本切片不新增网络
  角色与前缀）。集成条件见 spec.md Out of Scope。

## Requirement-to-task map

FR-015 由 T002（公共准备与 adapter 收口）、T007（共用路径和接口
兼容性验收）负责；公共准备/adapter 与生成 worker 的定向修复已通过，
T007 的 A05 源码/配置/runtime 核查已 PASS；映射与证据见
`evidence/shared-runtime-reuse-20260905.md`。不增加 Qwen 模型资格任务。

| Requirement | Owner tasks | 三层测试归属 | Closing evidence |
|---|---|---|---|
| FR-001 继承授权契约 | T001--T003, T006 | unit + integration（负例到达真实 verifier） | grant 往返证据 + Spec 180 编码回归（29 用例） |
| FR-002 进程内权威与既有发布路径 | T001（签发/发布/获取闭环） | unit（策略负例）+ integration（真实发布/获取往返） | `evidence/t001-python-provider-grant-current.md` |
| FR-003 Provider 双侧解包 | T001（Python）、T002（native） | unit + integration + parity 向量（T003） | `evidence/t001-python-provider-grant-current.md`、`evidence/t002-native-provider-grant-current.md`、`tests/fixtures/spec181/grant-vectors-v1.json` |
| FR-004 Y-N-E 真实变异 | T006（构造）+ T005（矩阵执行） | T006 unit + 三种实际 native Provider 拒绝/正向控制 PASS；正式同源矩阵仍待 T005 | `evidence/t006-production-repair-20260905.md` + `evidence/t005-y-n-matrix-current.md` |
| FR-005 三层测试标准 | 全部任务 | 每个证据文件头部声明证据层 | tasks.md 标准章节 + 审计 |
| FR-006 本地资格认证 | T005（矩阵）、T008（资格） | MiniNDN 小模型 CPU 全矩阵 | `evidence/t005-y-n-matrix-current.md`、`evidence/local-qualification.md` |
| FR-007 收敛审计 | T007 | PASS：12 原则、A01–A12 关闭、四层分离；正式验证归后续门 | `audit.md`、`evidence/post-implementation-audit.md` |
| FR-008 开发交付封存 | T009 | unit（脏树/跨版本/摘要拒绝）+ 本地完整性检查 | `evidence/development-delivery.json`、`evidence/t009-candidate-seal-current.md`；原 SIF 条款转 T010 |
| FR-009 实验交接与反馈 | T009, T012 | 本地核对复现材料、实验 owner、移交验收和反馈字段 | `handoff-contract.md`、交付清单与 closure；原 Tiger 执行转 T011 |
| FR-010 声称边界 | T012 | 终局语言审计 | `evidence/closure-record.md` |
| FR-011 就绪边界修复 | T004 | PASS：unit + rebuilt native integration（Provider 等待、真实 Controller 启动余量/取消/无热转） | `evidence/t004-readiness-boundary-current.md`、`evidence/t004-lifecycle-acceptance-20260905.md` |
| FR-012 装配 parity | T003 | PASS：8 个固定向量双侧 16 项检查，含真实 C++ 入口/正常 helper、逐字节/摘要、ORT CPU 结果与变异拒绝 | `tests/fixtures/spec181/assembly-vectors-v1.json`、`tests/python/test_spec181_assembly_parity.py`、`evidence/t003-assembly-parity-20260905.md` |
| FR-013 内容密钥真实消费 | T001（AEAD 暂存/解密/零化）、T002（native 同语义） | unit（派生/往返）+ integration（错误密钥/篡改密文在 AEAD 层拒绝）+ MiniNDN（T008 覆盖） | `evidence/t001-python-provider-grant-current.md`、`evidence/t002-native-provider-grant-current.md` |
| FR-014 诚实化先行 | R001（Y-N-E UNAVAILABLE）、R002（native 状态诚实化）、R003（证据失效声明）、R004（保护纪元子用例门禁） | unit + integration（先于全部实现任务，被 T001/T002/T006 吸收） | `evidence/r001-y-n-e-unavailable-current.md`、`evidence/r002-native-protected-runtime-honesty-current.md`、`evidence/r003-evidence-banner-audit-current.md`、`evidence/r004-protected-case-guard-current.md` |

## Source-owner status

| Path or path group | Owner | Current verified scope | Required evidence |
|---|---|---|---|
| `core/protected_artifacts.py`、`security/*` | T001, T006 | existing（Spec 180 提交 `d36438c2` + 2026-09-05 撤销清理：规范编码 + 进程内权威 + seam + 注册表，无撤销账本） | 签发/发布/获取/解包的生产连线；29 个编码测试作为回归基线 |
| `app_sdk/placement.py` grant seam、`sdk/placement.py` grant view | T001, T002 | existing（seam 已扩展，`AuthorityBackedGrantProvider` 就绪） | 真实资格路径使用非 `plaintext-v1` 纪元并通过 seam |
| `provider.py` 装配入口 | T001 | PASS（任务验收）：真实获取/装配与负例、独立策略绑定、inline/external 清理、请求/排队取消与过期；151 项定向回归、6 个真实进程用例通过 | `evidence/t001-request-lifecycle-20260905.md`；正式 MiniNDN 资格仍由 T005/T008 验收 |
| `cpp/ndnsf-di/NativeProviderHandler.cpp`、`NativeProtectedProvider.{hpp,cpp}`、`NativeProtectedGrantCredentials.cpp`、`NativeProtectedGrantTransport.cpp`、`ProtectedRuntime.{hpp,cpp}`、`NativeGrantVerifier.{hpp,cpp}`、`_ndnsf.cpp` | T002, T003 | PASS（T002 unit/integration 与公共准备/adapter 验收完成；`1ba99000` 的维护 native build 身份已刷新，仅适用于该提交）| `evidence/t002-acceptance-20260905.md`、`evidence/t007-native-plan-closure-20260906.md`；同源资格与剩余收敛审计仍归后续门 |
| `ServiceUser.publish_signed_app_data` 发布路径 | T001 | existing（runner 目录发布已使用）| grant Data 经此路径发布并被 Provider 精确名获取 |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | T005, T006, T008 | existing（barriered runner + 语义判定已修复）| Y-N 全矩阵语义重跑 + Y-A/Y-B 资格 |
| `scripts/spec180_candidate.py` 与本地验证/交付工具 | T009/T012（本机） | existing 工具需按修订 7 限定本地平面，不要求 SIF 字节 | 开发封存、复现/交接完整性与本地 closure |
| SIF/Tiger 工具及 `packaging/.../jobs/spec180/*` | T010/T011（实验机器） | TRANSFERRED，当前未取得实验验收 | 外部原 SIF/replay/Tiger 证据；不是本地关闭依赖 |
| `tests/fixtures/spec181/grant-vectors-v1.json` | T003 | executed（重建后的 3 项 parity 检查消费 9 个 grant 向量 PASS）| grant 向量不代替独立 canonical ONNX 装配 parity |
| `tests/fixtures/spec181/assembly-vectors-v1.json`、`assembly-parity-driver.cpp`、`tests/python/test_spec181_assembly_parity.py` | T003 | executed（16 项固定装配检查 PASS） | C++ 获取/序列化/缓存入口到正常 helper；不声称第二套 ONNX 算法 |
| 撤销子系统（账本/网络服务/撤销校验） | **另一分支（所有者）** | deferred（本分支不实现） | 集成时插入撤销检查并解除 spec.md Out of Scope 延期标记 |
| 独立权威网络服务端（生产形态） | **操作者（生产部署前）** | deferred（本分支不实现） | 生产部署前拆分为独立服务并恢复网络服务端形态 |

## Success Criterion Map

| Criterion | Tasks | Required evidence and current status |
|---|---|---|
| SC-001 | T001/T002/T003/T005/T006 | T001/T002/T003/T006 验收完成；正式同源 MiniNDN 未闭合；撤销延期 |
| SC-002 | T001/T002/T004/T005/T006/T007 | T001/T002/T004/T006/T007 验收完成；公共 generation worker 追加修复 48 cases / 366 assertions PASS；正式同源资格仍开放 |
| SC-003 | T005/T008 | planned: `evidence/local-qualification.md`；同源 Y-A/Y-B/Y-N 与全部退出/清理 |
| SC-004 | T007/T008 | `audit.md`、`evidence/post-implementation-audit.md` 的审计 PASS；local-suite inventory 待执行，SC 尚未整体完成 |
| SC-005 | T009 | planned: 同源开发交付清单、复现命令、输入/证据摘要与移交契约；SIF/Tiger 验收转外部 |
| SC-006 | T012 | planned: `evidence/closure-record.md` 的 LOCAL_DEVELOPMENT_PASS；未发出本地关闭裁决 |

## Evidence Levels

- `implemented`：源码存在。
- `wired`：生产调用方到达。
- `executed`：真实路径完成。
- `measured`：注册指标/工件存在。
- `qualified`：全部用例 oracle 与身份门一致。

禁止从 `implemented`/`wired` 直接跳到 `qualified`；Spec 180 的
历史 PASS 一律是历史观察，不是本 spec 的证据。
